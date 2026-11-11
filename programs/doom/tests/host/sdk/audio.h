/* Host test only: a stand-in for the SDK's audio streams. It keeps count
 * of what Doom's sound module writes (the stream "plays" instantly), so
 * the test can check that the sounds of firing came out. */
#ifndef MINIMALOS_AUDIO_H
#define MINIMALOS_AUDIO_H
#include <stdint.h>
extern unsigned long test_audio_frames, test_audio_loud;
static inline int mos_audio_stream_open(uint32_t rate, uint32_t channels) { (void)rate; (void)channels; return 0; }
static inline long mos_audio_stream_write(int h, const int16_t* s, uint32_t frames) {
    (void)h;
    for (uint32_t i = 0; i < frames * 2; i++)
        if (s[i] > 2000 || s[i] < -2000) { test_audio_loud++; }
    test_audio_frames += frames;
    return frames;
}
static inline long mos_audio_stream_queued(int h) { (void)h; return 0; }
static inline long mos_audio_stream_close(int h) { (void)h; return 0; }
#endif
