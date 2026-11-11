#ifndef MINIMALOS_AUDIO_H
#define MINIMALOS_AUDIO_H

/*
 * Playing .adi audio files (SYS_AUDIO).
 *
 *   mos_audio_open(path)    load a file and start playing it
 *   mos_audio_open_at(path, offset)  the same for .adi data inside a file
 *   mos_audio_pause()       pause / mos_audio_resume() continue
 *   mos_audio_stop()        stop and unload
 *   mos_audio_seek(ms)      jump to a position
 *   mos_audio_volume(0-100)
 *   mos_audio_status(&st)   state, position, duration, sound card ...
 *
 * There is one player for the whole system: opening a file replaces
 * whatever was playing. The kernel picks the sound card (Intel HD Audio
 * or AC97); status.driver is "" when there is none.
 *
 * Streams - sound a program makes itself, mixed with the player and with
 * other programs' streams (up to SYSCALL_AUDIO_STREAMS at once):
 *
 *   int h = mos_audio_stream_open(22050, 2);      // rate, channels
 *   long n = mos_audio_stream_write(h, samples, frames);  // frames taken
 *   mos_audio_stream_queued(h)                    // frames still waiting
 *   mos_audio_stream_set_volume(h, 0-100)
 *   mos_audio_stream_close(h)                     // also on exit
 *
 * Samples are signed 16-bit, interleaved L, R for stereo. Write a little
 * ahead of what plays (50-100 ms) and top it up as `queued` falls.
 */

#include <stdint.h>
#include "syscall.h"
#include "string.h"
#include "x86_64/user_syscalls.h"

static inline long mos_audio__call(uint32_t op, uint32_t value, const char* path,
                                   syscall_audio_status_t* status) {
    static syscall_audio_request_t r;
    memset(&r, 0, sizeof(r));
    r.size = sizeof(r);
    r.version = SYS_AUDIO_ABI_VERSION;
    r.op = op;
    r.value = value;
    if (path) {
        uint32_t i = 0;
        for (; path[i] && i + 1 < SYSCALL_AUDIO_PATH_MAX; i++) r.path[i] = path[i];
        r.path[i] = '\0';
    }
    long res = (long)mos_syscall1(SYS_AUDIO, (uint64_t)(uintptr_t)&r);
    if (status) *status = r.status;
    return res;
}

static inline long mos_audio_open(const char* path) { return mos_audio__call(SYS_AUDIO_OPEN, 0, path, 0); }
// Plays .adi data stored `offset` bytes into a file (a .vid soundtrack).
static inline long mos_audio_open_at(const char* path, uint32_t offset) {
    return mos_audio__call(SYS_AUDIO_OPEN, offset, path, 0);
}
static inline long mos_audio_pause(void)  { return mos_audio__call(SYS_AUDIO_PAUSE, 0, 0, 0); }
static inline long mos_audio_resume(void) { return mos_audio__call(SYS_AUDIO_RESUME, 0, 0, 0); }
static inline long mos_audio_stop(void)   { return mos_audio__call(SYS_AUDIO_STOP, 0, 0, 0); }
static inline long mos_audio_seek(uint32_t ms) { return mos_audio__call(SYS_AUDIO_SEEK, ms, 0, 0); }
static inline long mos_audio_volume(uint32_t volume) { return mos_audio__call(SYS_AUDIO_VOLUME, volume, 0, 0); }
static inline long mos_audio_status(syscall_audio_status_t* status) {
    return mos_audio__call(SYS_AUDIO_STATUS, 0, 0, status);
}


static inline long mos_audio_stream__call(syscall_audio_stream_t* r) {
    r->size = sizeof(*r);
    r->version = SYS_AUDIO_STREAM_ABI_VERSION;
    return (long)mos_syscall1(SYS_AUDIO_STREAM, (uint64_t)(uintptr_t)r);
}

// A handle (1 and up), or a negative SYS_ERR_* value.
static inline int mos_audio_stream_open(uint32_t rate, uint32_t channels) {
    syscall_audio_stream_t r;
    memset(&r, 0, sizeof(r));
    r.op = SYS_AUDIO_STREAM_OPEN;
    r.rate = rate;
    r.channels = channels;
    return (int)mos_audio_stream__call(&r);
}

// Frames taken (0 when the stream's buffer is full), or a negative SYS_ERR_*.
static inline long mos_audio_stream_write(int handle, const int16_t* samples, uint32_t frames) {
    syscall_audio_stream_t r;
    memset(&r, 0, sizeof(r));
    r.op = SYS_AUDIO_STREAM_WRITE;
    r.handle = (uint32_t)handle;
    r.samples = samples;
    r.frames = frames;
    return mos_audio_stream__call(&r);
}

// Frames waiting to be played, or a negative SYS_ERR_*. `info` (may be
// NULL) gets the rest: capacity, the mix rate, underruns.
static inline long mos_audio_stream_query(int handle, syscall_audio_stream_t* info) {
    syscall_audio_stream_t r;
    memset(&r, 0, sizeof(r));
    r.op = SYS_AUDIO_STREAM_QUERY;
    r.handle = (uint32_t)handle;
    long res = mos_audio_stream__call(&r);
    if (info) *info = r;
    return res < 0 ? res : (long)r.queued;
}

static inline long mos_audio_stream_queued(int handle) { return mos_audio_stream_query(handle, 0); }

static inline long mos_audio_stream_set_volume(int handle, uint32_t volume) {
    syscall_audio_stream_t r;
    memset(&r, 0, sizeof(r));
    r.op = SYS_AUDIO_STREAM_VOLUME;
    r.handle = (uint32_t)handle;
    r.volume = volume;
    return mos_audio_stream__call(&r);
}

static inline long mos_audio_stream_close(int handle) {
    syscall_audio_stream_t r;
    memset(&r, 0, sizeof(r));
    r.op = SYS_AUDIO_STREAM_CLOSE;
    r.handle = (uint32_t)handle;
    return mos_audio_stream__call(&r);
}
#endif
