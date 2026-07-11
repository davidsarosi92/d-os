/* =============================================================================
 * audio.h — abstract audio-device registry (§M23).
 *
 * Mirrors block.h / net.h: a driver registers a `struct audio_dev` exposing a
 * single "play these PCM frames" callback; the portable core (tone generator,
 * `lsaudio`, a future WAV player) sits on top and never talks to a specific
 * codec.  The only coupling between a codec driver (AC97, HDA, …) and the core
 * is this struct.
 *
 * PCM format for this first slice is fixed: 16-bit signed, stereo (2 channels
 * interleaved L,R), at the device's native `rate` (48 kHz on QEMU AC97).  A
 * "frame" is one L+R sample pair (4 bytes).  Mixer / multiple streams /
 * resampling are deferred (§M23 out-of-scope).
 * ============================================================================= */

#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

struct audio_dev {
    const char* name;                         /* e.g. "ac97"                 */
    uint32_t    rate;                         /* native sample rate (Hz)     */
    uint32_t    channels;                     /* 2 (stereo)                  */

    /* Play `nframes` interleaved 16-bit stereo frames.  Blocking: returns
     * after the samples have been queued/played.  Returns 0 on success.
     * `frames` holds nframes*2 int16 samples. */
    int (*play)(struct audio_dev* dev, const int16_t* frames, uint32_t nframes);

    void* priv;                               /* driver-private state        */
    struct audio_dev* next;                   /* registry link               */
};

/* Registry. */
int  audio_register(struct audio_dev* dev);
struct audio_dev* audio_primary(void);        /* first registered, or NULL   */
void audio_list(void);                        /* backs the `lsaudio` command  */

/* Generate + play a square-wave tone of `freq` Hz for `ms` milliseconds on
 * the primary device (the §M23 smoke test).  Returns 0 on success. */
int  audio_play_tone(uint32_t freq, uint32_t ms);

#endif /* AUDIO_H */
