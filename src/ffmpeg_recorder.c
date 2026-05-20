#include "ffmpeg_recorder.h"
#include "glue.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>

#define VGA_PIXEL_FREQ  25000000.0
#define VGA_SCAN_W      800.0
#define SCAN_H          525.0
#define SCREEN_W        640
#define SCREEN_H        480

/* ~2 seconds of storage */
#define VIDEO_RING_FRAMES  120
#define AUDIO_RING_SAMPLES (49000 * 2 * 2)   /* stereo int16_t, ~2s @ ~49 kHz */

/* ---- ring buffers ---- */
static uint8_t **video_ring;
static int video_write_pos = 0;
static int video_read_pos  = 0;
static int video_count     = 0;

static int16_t *audio_ring;
static int audio_write_pos = 0;
static int audio_read_pos  = 0;
static int audio_fill      = 0;


/* ---- pipes & subprocess ---- */
static int   video_fd   = -1;
static int   audio_fd   = -1;
static pid_t child_pid  = -1;

/* ---- threading ---- */
static pthread_t       consumer_thread;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond  = PTHREAD_COND_INITIALIZER;
static bool            consumer_running = false;
static bool            consumer_exited = false;
static ffmpeg_recorder_state_t state = RECORD_FFMPEG_DISABLED;

static void *consumer_func(void *arg);

void
ffmpeg_recorder_init(const char *path)
{
	if (path == NULL) {
		state = RECORD_FFMPEG_DISABLED;
		return;
	}

	consumer_exited = false;

	/* ---- allocate ring buffers ---- */
	video_ring = calloc(VIDEO_RING_FRAMES, sizeof(uint8_t *));
	for (int i = 0; i < VIDEO_RING_FRAMES; i++) {
		video_ring[i] = malloc(SCREEN_W * SCREEN_H * 4);
	}
	video_write_pos = 0;
	video_read_pos  = 0;
	video_count     = 0;

	audio_ring = malloc(AUDIO_RING_SAMPLES * sizeof(int16_t));
	audio_write_pos = 0;
	audio_read_pos  = 0;
	audio_fill      = 0;

	/* ---- create pipes ---- */
	int video_pipe[2], audio_pipe[2];
	if (pipe(video_pipe) < 0 || pipe(audio_pipe) < 0) {
		printf("Failed to create pipes for recording.\n");
		goto fail;
	}

	/* ---- ffmpeg subprocess ---- */
	double framerate = VGA_PIXEL_FREQ / (VGA_SCAN_W * SCAN_H);
	char fps_str[32];
	snprintf(fps_str, sizeof(fps_str), "%.8f", framerate);
	char sample_rate_str[16];
	snprintf(sample_rate_str, sizeof(sample_rate_str), "%u",
		 host_sample_rate ? host_sample_rate : 48828);

	child_pid = fork();
	if (child_pid == 0) {
		close(video_pipe[1]);
		close(audio_pipe[1]);
		dup2(video_pipe[0], 0);
		close(video_pipe[0]);
		dup2(audio_pipe[0], 3);
		close(audio_pipe[0]);

		const char *argv[] = {
			"ffmpeg", "-y",
			"-f", "rawvideo", "-pix_fmt", "bgr0", "-s", "640x480",
			"-r", fps_str, "-i", "pipe:0",
			"-f", "s16le", "-ar", sample_rate_str, "-ac", "2", "-i", "pipe:3",
			"-c:v", "libx264", "-crf", "0", "-preset", "slow",
			"-pix_fmt", "yuv420p",
			"-c:a", "aac", "-b:a", "128k",
			path,
			NULL
		};
		execvp("ffmpeg", (char *const *)argv);
		_exit(1);
	}
	if (child_pid < 0) {
		printf("Failed to fork for recording.\n");
		goto fail;
	}

	/* parent */
	close(video_pipe[0]);
	close(audio_pipe[0]);
	video_fd = video_pipe[1];
	audio_fd = audio_pipe[1];

	/* ---- consumer thread ---- */
	consumer_running = true;
	if (pthread_create(&consumer_thread, NULL, consumer_func, NULL) != 0) {
		printf("Failed to create recorder consumer thread.\n");
		consumer_running = false;
		close(video_fd); video_fd = -1;
		close(audio_fd); audio_fd = -1;
		kill(child_pid, SIGTERM);
		waitpid(child_pid, NULL, 0);
		child_pid = -1;
		goto fail;
	}

	printf("Recording to %s\n", path);
	state = RECORD_FFMPEG_ACTIVE;
	return;

fail:
	state = RECORD_FFMPEG_ERROR;
	for (int i = 0; i < VIDEO_RING_FRAMES; i++) free(video_ring[i]);
	free(video_ring);       video_ring = NULL;
	free(audio_ring);       audio_ring = NULL;
}

void
ffmpeg_recorder_push_video(const uint8_t *framebuffer)
{
	if (state != RECORD_FFMPEG_ACTIVE) {
		return;
	}
	if (warp_mode) {
		return;
	}

	pthread_mutex_lock(&mutex);
	if (video_count < VIDEO_RING_FRAMES) {
		memcpy(video_ring[video_write_pos], framebuffer,
		       SCREEN_W * SCREEN_H * 4);
		video_write_pos = (video_write_pos + 1) % VIDEO_RING_FRAMES;
		video_count++;
		pthread_cond_signal(&cond);
	}
	pthread_mutex_unlock(&mutex);
}

void
ffmpeg_recorder_push_audio(const int16_t *samples, int num_samples)
{
	if (state != RECORD_FFMPEG_ACTIVE) {
		return;
	}

	pthread_mutex_lock(&mutex);
	int to_copy = num_samples * 2;
	int room = AUDIO_RING_SAMPLES - audio_fill;
	if (to_copy <= room) {
		int first = AUDIO_RING_SAMPLES - audio_write_pos;
		if (to_copy <= first) {
			memcpy(&audio_ring[audio_write_pos], samples,
			       to_copy * sizeof(int16_t));
		} else {
			memcpy(&audio_ring[audio_write_pos], samples,
			       first * sizeof(int16_t));
			memcpy(&audio_ring[0], samples + first,
			       (to_copy - first) * sizeof(int16_t));
		}
		audio_write_pos = (audio_write_pos + to_copy) % AUDIO_RING_SAMPLES;
		audio_fill += to_copy;
		pthread_cond_signal(&cond);
	}
	pthread_mutex_unlock(&mutex);
}

static void *
consumer_func(void *arg)
{
	(void)arg;

	for (;;) {
		pthread_mutex_lock(&mutex);
		while (video_count == 0 && audio_fill == 0 && consumer_running) {
			pthread_cond_wait(&cond, &mutex);
		}
		if (!consumer_running && video_count == 0 && audio_fill == 0) {
			pthread_mutex_unlock(&mutex);
			break;
		}

		bool write_video = (video_count > 0);
		int video_idx = video_read_pos;

		int audio_to_write = audio_fill;
		int audio_idx = audio_read_pos;

		pthread_mutex_unlock(&mutex);

		/* blocking writes to pipes (safe in background thread) */
		if (write_video) {
			ssize_t written;
			const uint8_t *p = video_ring[video_idx];
			size_t remain = SCREEN_W * SCREEN_H * 4;
			while (remain > 0) {
				written = write(video_fd, p, remain);
				if (written > 0) {
					p += written;
					remain -= written;
				} else if (errno == EINTR) {
					continue;
				} else {
					break;
				}
			}
		}

		if (audio_to_write > 0) {
			ssize_t written;
			int first = AUDIO_RING_SAMPLES - audio_idx;
			if (audio_to_write <= first) {
				const int16_t *ap = &audio_ring[audio_idx];
				size_t aremain = audio_to_write;
				while (aremain > 0) {
					written = write(audio_fd, ap, aremain * sizeof(int16_t));
					if (written > 0) {
						ap += written / sizeof(int16_t);
						aremain -= written / sizeof(int16_t);
					} else if (errno == EINTR) {
						continue;
					} else {
						break;
					}
				}
			} else {
				const int16_t *ap1 = &audio_ring[audio_idx];
				size_t aremain1 = first;
				bool ok = true;
				while (aremain1 > 0) {
					written = write(audio_fd, ap1, aremain1 * sizeof(int16_t));
					if (written > 0) {
						ap1 += written / sizeof(int16_t);
						aremain1 -= written / sizeof(int16_t);
					} else if (errno == EINTR) {
						continue;
					} else {
						ok = false;
						break;
					}
				}
				if (ok) {
					const int16_t *ap2 = &audio_ring[0];
					size_t aremain2 = audio_to_write - first;
					while (aremain2 > 0) {
						written = write(audio_fd, ap2, aremain2 * sizeof(int16_t));
						if (written > 0) {
							ap2 += written / sizeof(int16_t);
							aremain2 -= written / sizeof(int16_t);
						} else if (errno == EINTR) {
							continue;
						} else {
							break;
						}
					}
				}
			}
		}

		pthread_mutex_lock(&mutex);
		if (write_video) {
			video_read_pos = (video_read_pos + 1) % VIDEO_RING_FRAMES;
			video_count--;
		}
		if (audio_to_write > 0) {
			audio_read_pos = (audio_read_pos + audio_to_write) % AUDIO_RING_SAMPLES;
			audio_fill -= audio_to_write;
		}
		pthread_mutex_unlock(&mutex);
	}

	pthread_mutex_lock(&mutex);
	consumer_exited = true;
	pthread_mutex_unlock(&mutex);
	return NULL;
}

void
ffmpeg_recorder_shutdown(void)
{
	if (state == RECORD_FFMPEG_DISABLED) {
		return;
	}
	state = RECORD_FFMPEG_DISABLED;

	/* stop consumer thread */
	pthread_mutex_lock(&mutex);
	consumer_running = false;
	pthread_cond_signal(&cond);
	pthread_mutex_unlock(&mutex);

	/* Wait up to 2 seconds for consumer thread to drain and exit */
	bool joined = false;
	for (int i = 0; i < 20; i++) {
		pthread_mutex_lock(&mutex);
		if (consumer_exited) {
			pthread_mutex_unlock(&mutex);
			pthread_join(consumer_thread, NULL);
			joined = true;
			break;
		}
		pthread_mutex_unlock(&mutex);
		usleep(100000); // 100ms
	}

	/* If consumer did not exit, close pipes to force-unblock it */
	if (!joined) {
		printf("Warning: Recorder consumer thread did not exit cleanly. Closing pipes...\n");
		if (video_fd >= 0) {
			close(video_fd);
			video_fd = -1;
		}
		if (audio_fd >= 0) {
			close(audio_fd);
			audio_fd = -1;
		}
		// Give it another second to react to closed pipes
		for (int i = 0; i < 10; i++) {
			pthread_mutex_lock(&mutex);
			if (consumer_exited) {
				pthread_mutex_unlock(&mutex);
				pthread_join(consumer_thread, NULL);
				joined = true;
				break;
			}
			pthread_mutex_unlock(&mutex);
			usleep(100000);
		}
		if (!joined) {
			printf("Error: Recorder consumer thread is completely hung. Joining anyway...\n");
			if (child_pid > 0) {
				kill(child_pid, SIGKILL);
			}
			pthread_join(consumer_thread, NULL);
			joined = true;
		}
	} else {
		/* Normal exit: close pipes after joining so ffmpeg receives EOF */
		if (video_fd >= 0) {
			close(video_fd);
			video_fd = -1;
		}
		if (audio_fd >= 0) {
			close(audio_fd);
			audio_fd = -1;
		}
	}

	/* reap ffmpeg */
	if (child_pid > 0) {
		int status;
		// Give ffmpeg up to 2 seconds to exit cleanly
		int ret = 0;
		for (int i = 0; i < 20; i++) {
			ret = waitpid(child_pid, &status, WNOHANG);
			if (ret > 0 || ret < 0) {
				break;
			}
			usleep(100000);
		}
		if (ret == 0) {
			kill(child_pid, SIGTERM);
			waitpid(child_pid, &status, 0);
		}
		if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
			printf("ffmpeg exited with code %d\n",
			       WEXITSTATUS(status));
		}
		child_pid = -1;
	}

	/* free ring buffers */
	for (int i = 0; i < VIDEO_RING_FRAMES; i++) free(video_ring[i]);
	free(video_ring);       video_ring = NULL;
	free(audio_ring);       audio_ring = NULL;
	video_count = 0;
	audio_fill = 0;
}

void
ffmpeg_recorder_set(ffmpeg_recorder_command_t command)
{
	if (state == RECORD_FFMPEG_DISABLED || state == RECORD_FFMPEG_ERROR) {
		return;
	}
	switch (command) {
		case RECORD_FFMPEG_PAUSE:
			state = RECORD_FFMPEG_PAUSED;
			break;
		case RECORD_FFMPEG_RECORD:
			if (state == RECORD_FFMPEG_ACTIVE) {
				break;
			}
			state = RECORD_FFMPEG_ACTIVE;
			break;
		case RECORD_FFMPEG_SNAP:
			break;
	}
}

uint8_t
ffmpeg_recorder_get_state(void)
{
	return (uint8_t)state;
}
