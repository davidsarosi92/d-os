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

    /* Play interleaved 16-bit stereo frames.  Blocking: returns once the
     * samples it accepted have actually been played.  `frames` holds
     * nframes*2 int16 samples.
     *
     * RETURNS THE NUMBER OF FRAMES CONSUMED, or negative on error — it may be
     * FEWER than asked for, because a driver's DMA buffer is finite.  §M23
     * stage 1 clamped silently and returned success, which is invisible while
     * the only caller is a beep that fits, and becomes SILENT DATA LOSS the
     * moment something streams a file through it: the caller has no way to
     * learn that the tail of its chunk was dropped.  A short count is the
     * normal case for a streaming caller — see audio_play_pcm(). */
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

/* Play a whole PCM buffer, however many `play` calls that takes — the loop
 * every caller would otherwise write, and get subtly wrong in its own way now
 * that a short count is legal.  Returns 0 when everything was played. */
int  audio_play_pcm(struct audio_dev* dev, const int16_t* frames, uint32_t nframes);

/* ---------------------------------------------------------------------------
 * WAV playback (§M23 stage 2).
 *
 * STREAMED, never loaded: a file is read a chunk at a time into a bounded
 * buffer and converted on the way through.  A four-minute song is ~40 MB of
 * PCM, which is not a thing to hold in kernel memory — the same argument
 * §M60's wallpaper decoder made about a 9 MB bitmap.
 *
 * The device is fixed at 16-bit stereo at its native rate, so the converter
 * handles what real files actually are: 8-bit unsigned or 16-bit signed, mono
 * or stereo, at any rate.  Rate conversion is NEAREST-SAMPLE and says so —
 * it is not a band-limited resampler and will alias on a big ratio.  Refusing
 * every file that is not already 48 kHz would be the alternative, and most
 * WAV files in the world are 44.1.
 * --------------------------------------------------------------------------- */
int  audio_play_wav(const char* path);

/* The `play` / `lsaudio` / `beep` / `tone` commands, implemented HERE rather
 * than in a shell — §M24's rule, so the aarch64 serial REPL runs the same
 * implementation instead of a second one that drifts. */
void audio_cmd_play(const char* args);

#endif /* AUDIO_H */
