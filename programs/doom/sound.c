/*
 * Doom for Minimal-OS: sound effects.
 *
 * Doom's effects are 8-bit samples (the DMX format of the DOS version,
 * mostly 11025 Hz). This module plays up to 16 of them at once: it mixes
 * them here, with Doom's volume and stereo separation, into one 22050 Hz
 * stereo audio stream (SYS_AUDIO_STREAM, the SDK's audio.h), which the
 * kernel mixes with whatever else is playing.
 *
 * Each game tic (S_UpdateSounds -> Update) tops the stream up to about
 * 70 ms ahead of what plays: enough to ride out a slow frame, little
 * enough that a shot is heard when it is fired.
 *
 * Music is not played yet: Doom's songs are MUS scores for an OPL2 FM
 * synthesizer (or MIDI), which would need a synthesizer here first.
 */
#include <string.h>
#include "audio.h"
#include <stdio.h>
#include "src/doomtype.h"
#include "src/i_sound.h"
#include "src/w_wad.h"
#include "src/z_zone.h"

#define MIX_RATE 22050u
#define VOICES 16
#define AHEAD_FRAMES (MIX_RATE * 70u / 1000u)
#define CHUNK 1024u

typedef struct {
    const uint8_t* samples;     /* unsigned 8-bit, 128 = silence */
    uint32_t length;
    uint32_t pos;               /* 16.16 */
    uint32_t step;              /* 16.16: sample rate / MIX_RATE */
    int left, right;            /* 0-254 */
    boolean playing;
} voice_t;

/* Settings of the SDL sound module upstream, kept so default.cfg reads
 * the same; resampling here is always the simple kind. */
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

static voice_t g_voices[VOICES];
static int g_stream = -1;
static boolean g_use_prefix;

static void set_params(voice_t* v, int vol, int sep) {
    if (vol < 0) vol = 0;
    if (vol > 127) vol = 127;
    if (sep < 0) sep = 0;
    if (sep > 254) sep = 254;
    /* sep: 0 = all left, 128 = middle, 254 = all right. */
    v->left = vol * (254 - sep) / 127;
    v->right = vol * sep / 127;
}

static boolean Snd_Init(boolean use_sfx_prefix) {
    g_use_prefix = use_sfx_prefix;
    g_stream = mos_audio_stream_open(MIX_RATE, 2);
    if (g_stream < 0) {
        g_stream = -1;
        printf("I_InitSound: no audio stream (no sound card?); playing without sound\n");
        return false;
    }
    memset(g_voices, 0, sizeof(g_voices));
    printf("I_InitSound: sound effects at %u Hz through an audio stream\n", MIX_RATE);
    return true;
}

static void Snd_Shutdown(void) {
    if (g_stream >= 0) mos_audio_stream_close(g_stream);
    g_stream = -1;
}

static int Snd_GetSfxLumpNum(sfxinfo_t* sfx) {
    char name[9];
    if (sfx->link) sfx = sfx->link;
    if (g_use_prefix) snprintf(name, sizeof(name), "ds%s", sfx->name);
    else snprintf(name, sizeof(name), "%s", sfx->name);
    return W_CheckNumForName(name);
}

static void mix(int16_t* out, uint32_t frames) {
    static int32_t acc[CHUNK * 2];
    memset(acc, 0, frames * 2 * sizeof(int32_t));
    for (int k = 0; k < VOICES; k++) {
        voice_t* v = &g_voices[k];
        if (!v->playing) continue;
        for (uint32_t i = 0; i < frames; i++) {
            uint32_t at = v->pos >> 16;
            if (at >= v->length) { v->playing = false; break; }
            int s = ((int)v->samples[at] - 128) << 8;
            acc[i * 2] += s * v->left / 254;
            acc[i * 2 + 1] += s * v->right / 254;
            v->pos += v->step;
        }
    }
    for (uint32_t i = 0; i < frames * 2; i++) {
        int32_t s = acc[i];
        out[i] = (int16_t)(s > 32767 ? 32767 : (s < -32768 ? -32768 : s));
    }
}

static void Snd_Update(void) {
    if (g_stream < 0) return;
    long queued = mos_audio_stream_queued(g_stream);
    if (queued < 0) return;
    static int16_t buf[CHUNK * 2];
    while ((uint32_t)queued < AHEAD_FRAMES) {
        uint32_t n = AHEAD_FRAMES - (uint32_t)queued;
        if (n > CHUNK) n = CHUNK;
        mix(buf, n);
        long taken = mos_audio_stream_write(g_stream, buf, n);
        if (taken <= 0) break;
        queued += taken;
    }
}

static void Snd_UpdateSoundParams(int channel, int vol, int sep) {
    if (channel < 0 || channel >= VOICES) return;
    set_params(&g_voices[channel], vol, sep);
}

static int Snd_StartSound(sfxinfo_t* sfx, int channel, int vol, int sep) {
    if (g_stream < 0 || channel < 0 || channel >= VOICES) return -1;
    int lump = sfx->lumpnum >= 0 ? sfx->lumpnum : Snd_GetSfxLumpNum(sfx);
    if (lump < 0) return -1;
    const uint8_t* data = W_CacheLumpNum(lump, PU_STATIC);
    uint32_t size = (uint32_t)W_LumpLength(lump);
    /* DMX: format 3, rate, sample count, then 16 bytes of padding on
     * each side of the samples (the DOS mixer's lead-in). */
    if (size < 8 || data[0] != 3 || data[1] != 0) return -1;
    uint32_t rate = (uint32_t)(data[2] | (data[3] << 8));
    uint32_t count = (uint32_t)(data[4] | (data[5] << 8) | (data[6] << 16) | ((uint32_t)data[7] << 24));
    if (count > size - 8) count = size - 8;
    if (count <= 32 || rate < 4000) return -1;
    voice_t* v = &g_voices[channel];
    v->playing = false;
    v->samples = data + 8 + 16;
    v->length = count - 32;
    v->pos = 0;
    v->step = (uint32_t)(((uint64_t)rate << 16) / MIX_RATE);
    set_params(v, vol, sep);
    v->playing = true;
    return channel;
}

static void Snd_StopSound(int channel) {
    if (channel >= 0 && channel < VOICES) g_voices[channel].playing = false;
}

static boolean Snd_SoundIsPlaying(int channel) {
    return channel >= 0 && channel < VOICES && g_voices[channel].playing;
}

static void Snd_CacheSounds(sfxinfo_t* sounds, int num_sounds) { (void)sounds; (void)num_sounds; }

static snddevice_t k_sound_devices[] = { SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER,
                                         SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32 };

sound_module_t DG_sound_module = {
    k_sound_devices, (int)(sizeof(k_sound_devices) / sizeof(k_sound_devices[0])),
    Snd_Init, Snd_Shutdown, Snd_GetSfxLumpNum, Snd_Update, Snd_UpdateSoundParams,
    Snd_StartSound, Snd_StopSound, Snd_SoundIsPlaying, Snd_CacheSounds,
};

/* --------------------------------------------- music: none yet (see top) */

static boolean Mus_Init(void) { return false; }
static void Mus_Shutdown(void) {}
static void Mus_SetMusicVolume(int volume) { (void)volume; }
static void Mus_PauseMusic(void) {}
static void Mus_ResumeMusic(void) {}
static void* Mus_RegisterSong(void* data, int len) { (void)data; (void)len; return NULL; }
static void Mus_UnRegisterSong(void* handle) { (void)handle; }
static void Mus_PlaySong(void* handle, boolean looping) { (void)handle; (void)looping; }
static void Mus_StopSong(void) {}
static boolean Mus_MusicIsPlaying(void) { return false; }

music_module_t DG_music_module = {
    k_sound_devices, (int)(sizeof(k_sound_devices) / sizeof(k_sound_devices[0])),
    Mus_Init, Mus_Shutdown, Mus_SetMusicVolume, Mus_PauseMusic, Mus_ResumeMusic,
    Mus_RegisterSong, Mus_UnRegisterSong, Mus_PlaySong, Mus_StopSong, Mus_MusicIsPlaying, NULL,
};
