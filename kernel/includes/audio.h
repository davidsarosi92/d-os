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
 * that a short count is legal.  Returns 0 when everything was played.
 *
 * THIS IS THE DEVICE-LEVEL PATH and it is what the MIXER uses; ordinary
 * playback goes through a stream (below).  Calling it directly while streams
 * are running would interleave two sources onto one device — which is the
 * problem the mixer exists to solve. */
int  audio_play_pcm(struct audio_dev* dev, const int16_t* frames, uint32_t nframes);

/* ---------------------------------------------------------------------------
 * Streams and the mixer (§M23 stage 4).
 *
 * Before this, a sound OWNED the device for its whole duration: `play` and a
 * second program could not both be heard, and `/dev/dsp` enforced that with a
 * busy flag — one writer at a time, everyone else waiting.  That is a
 * defensible thing for a beep and an obviously wrong thing for a desktop,
 * where a notification sound arriving during music must not have to wait for
 * the music to end.
 *
 * So every source now opens a STREAM and writes into its own ring; ONE pump
 * task owns the device and mixes whatever is active into each period.  The
 * shape is deliberately §M55's: the pump runs exactly while somebody has
 * audio to play and is fully blocked otherwise, so a machine making no sound
 * costs nothing.
 *
 * MIXING SATURATES.  Two loud streams sum past what 16 bits can hold, and a
 * wrap turns the loudest moment of a piece into white noise — the failure is
 * both maximally audible and maximally confusing, since it appears only when
 * two things happen at once.
 *
 * Each stream is single-producer / single-consumer, so the ring needs no lock
 * of its own; the table lock covers open, close and the pump's walk.
 * --------------------------------------------------------------------------- */

struct audio_stream;

/* Open a stream on the primary device.  NULL if there is no device or no free
 * slot — bounded like every other table here, and reported rather than
 * silently queued behind somebody else. */
struct audio_stream* audio_stream_open(const char* name);

/* Write interleaved 16-bit stereo frames at the device's rate.  BLOCKS while
 * the stream's ring is full, which is what paces a producer to real time —
 * and is the only backpressure a writer needs, since the pump drains at
 * exactly the speed the hardware consumes.  Returns frames written, or
 * negative on error. */
int  audio_stream_write(struct audio_stream* s, const int16_t* frames, uint32_t nframes);

/* Volume in 1/256ths (256 = unity, 0 = silent).  Per stream, because "turn the
 * music down while this plays" is the whole point of having streams. */
void audio_stream_volume(struct audio_stream* s, int vol_256);

/* Drain what is buffered, then release the slot.  It DRAINS rather than
 * dropping, so `play` still returns when the sound has actually finished — a
 * command that returns early leaves the shell printing its prompt over its own
 * audio. */
void audio_stream_close(struct audio_stream* s);

/* Backs `lsaudio`'s stream section: how many are open and what they are. */
void audio_stream_list(void);

/* ---------------------------------------------------------------------------
 * Master volume and mute.
 *
 * Applied ONCE to the finished mix rather than to each stream, because that is
 * what "the system volume" means: turning it down must not change the balance
 * between two things that are playing.
 *
 * MUTE IS NOT VOLUME 0.  They are stored separately so that unmuting restores
 * the level the user had chosen — a mute implemented by zeroing the volume
 * forgets it, and the user has to find their setting again every time.
 * --------------------------------------------------------------------------- */
void audio_master_set(int vol_256, int muted);
void audio_master_get(int* vol_256, int* muted);

/* Is there a working output device?  The three answers a status indicator
 * needs: playing, muted, or nothing to play through. */
int  audio_available(void);

/* Write the current master level + mute into the config store, so a change
 * made from the taskbar survives a reboot exactly as `volume 40` does.  Uses
 * config_set rather than config_apply: the value is already in effect, and
 * applying would re-enter the watcher to set what it just set. */
void audio_volume_persist(void);

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
void audio_cmd_volume(const char* args);

/* Publish /dev/dsp (§M23 stage 3).  Called next to clipboard_devfs_init() on
 * BOTH entry paths — the node belongs to the audio subsystem, not to a card,
 * so it exists even with no driver up and a write then fails with a reason. */
void audio_devfs_init(void);

#endif /* AUDIO_H */
