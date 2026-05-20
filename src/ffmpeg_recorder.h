#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
/*
 * Windows stub: ffmpeg recording uses fork()/pipe()/execvp() which are
 * POSIX-only. These inline stubs ensure callers compile and link harmlessly.
 */

typedef enum {
	RECORD_FFMPEG_PAUSE = 0,
	RECORD_FFMPEG_RECORD,
	RECORD_FFMPEG_SNAP,
} ffmpeg_recorder_command_t;

typedef enum {
	RECORD_FFMPEG_DISABLED = 0,
	RECORD_FFMPEG_ERROR,
	RECORD_FFMPEG_PAUSED,
	RECORD_FFMPEG_ACTIVE,
} ffmpeg_recorder_state_t;

static inline void ffmpeg_recorder_init(const char *path) { (void)path; }
static inline void ffmpeg_recorder_push_video(const uint8_t *framebuffer) { (void)framebuffer; }
static inline void ffmpeg_recorder_push_audio(const int16_t *samples, int num_samples) { (void)samples; (void)num_samples; }
static inline void ffmpeg_recorder_shutdown(void) {}
static inline void ffmpeg_recorder_set(ffmpeg_recorder_command_t command) { (void)command; }
static inline uint8_t ffmpeg_recorder_get_state(void) { return RECORD_FFMPEG_DISABLED; }

#else /* POSIX implementation */

typedef enum {
	RECORD_FFMPEG_PAUSE = 0,
	RECORD_FFMPEG_RECORD,
	RECORD_FFMPEG_SNAP,
} ffmpeg_recorder_command_t;

typedef enum {
	RECORD_FFMPEG_DISABLED = 0,
	RECORD_FFMPEG_ERROR,
	RECORD_FFMPEG_PAUSED,
	RECORD_FFMPEG_ACTIVE,
} ffmpeg_recorder_state_t;

void ffmpeg_recorder_init(const char *path);
void ffmpeg_recorder_push_video(const uint8_t *framebuffer);
void ffmpeg_recorder_push_audio(const int16_t *samples, int num_samples);
void ffmpeg_recorder_shutdown(void);
void ffmpeg_recorder_set(ffmpeg_recorder_command_t command);
uint8_t ffmpeg_recorder_get_state(void);

#endif /* _WIN32 */
