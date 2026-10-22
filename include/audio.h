#ifndef MINIMALOS_AUDIO_H
#define MINIMALOS_AUDIO_H

/*
 * Playing .adi audio files (SYS_AUDIO).
 *
 *   mos_audio_open(path)    load a file and start playing it
 *   mos_audio_pause()       pause / mos_audio_resume() continue
 *   mos_audio_stop()        stop and unload
 *   mos_audio_seek(ms)      jump to a position
 *   mos_audio_volume(0-100)
 *   mos_audio_status(&st)   state, position, duration, sound card ...
 *
 * There is one player for the whole system: opening a file replaces
 * whatever was playing. The kernel picks the sound card (Intel HD Audio
 * or AC97); status.driver is "" when there is none.
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
static inline long mos_audio_pause(void)  { return mos_audio__call(SYS_AUDIO_PAUSE, 0, 0, 0); }
static inline long mos_audio_resume(void) { return mos_audio__call(SYS_AUDIO_RESUME, 0, 0, 0); }
static inline long mos_audio_stop(void)   { return mos_audio__call(SYS_AUDIO_STOP, 0, 0, 0); }
static inline long mos_audio_seek(uint32_t ms) { return mos_audio__call(SYS_AUDIO_SEEK, ms, 0, 0); }
static inline long mos_audio_volume(uint32_t volume) { return mos_audio__call(SYS_AUDIO_VOLUME, volume, 0, 0); }
static inline long mos_audio_status(syscall_audio_status_t* status) {
    return mos_audio__call(SYS_AUDIO_STATUS, 0, 0, status);
}

#endif
