/* =============================================================================
 * audio.c — portable audio core (§M23): device registry + a square-wave tone
 * generator (the smoke test).  Arch-independent; the codec driver (AC97) is
 * the only hardware-specific piece and lives under kernel/drivers/audio/.
 * ============================================================================= */

#include "audio.h"
#include "printf.h"
#include "vfs.h"
#include "klog.h"
#include "devfs.h"
#include "task.h"      /* task_msleep — the /dev/dsp writer waits its turn */
#include "timer.h"
#include "waitq.h"
#include "lock.h"
#include "config.h"
#include "settings.h"
#include <stdint.h>
#include <stddef.h>

/* ----------------------- Registry ----------------------------------------- */

static struct audio_dev* g_head = NULL;

int audio_register(struct audio_dev* dev) {
    dev->next = NULL;
    if (!g_head) g_head = dev;
    else { struct audio_dev* n = g_head; while (n->next) n = n->next; n->next = dev; }
    kprintf("audio: registered %s (%u Hz, %u ch)\n", dev->name, dev->rate, dev->channels);
    return 0;
}

struct audio_dev* audio_primary(void) { return g_head; }

void audio_list(void) {
    if (!g_head) { kprintf("no audio devices\n"); return; }
    for (struct audio_dev* n = g_head; n; n = n->next)
        kprintf("%s  %u Hz  %u ch  16-bit PCM\n", n->name, n->rate, n->channels);
}

/* ----------------------- Tone generator ----------------------------------- */

/* Rendered into a static scratch buffer (the driver copies it into its own
 * DMA region).  Capped at one AC97 BDL entry's worth (~0.68 s at 48 kHz
 * stereo — plenty for a beep, ≤ 0xFFFE samples). */

#define TONE_MAX_FRAMES 32000          /* × 2 samples = 64000 ≤ 0xFFFE        */
static int16_t g_tone[TONE_MAX_FRAMES * 2];    /* interleaved L,R (static BSS) */

int audio_play_tone(uint32_t freq, uint32_t ms) {
    struct audio_dev* dev = audio_primary();
    if (!dev) { kprintf("audio: no device\n"); return -1; }
    if (freq == 0) freq = 440;

    uint32_t want = (dev->rate / 1000) * ms;   /* frames for `ms`             */

    /* Square wave: +/- amplitude, half a period each way. */
    uint32_t half = dev->rate / (freq * 2);
    if (half == 0) half = 1;
    const int16_t amp = 8000;
    int16_t level = amp;
    uint32_t phase = 0;

    kprintf("audio: playing %u Hz for %u ms (%u frames) on %s\n",
            freq, ms, want, dev->name);

    /* RENDERED IN CHUNKS, so `ms` means what it says.  This used to clamp to
     * one buffer and report success: `tone 440 3000` played 666 ms and said
     * nothing about the missing two and a third seconds — the same silent
     * truncation the device's `play` had, one layer up, and equally invisible
     * while nobody asked for a long tone.
     *
     * PHASE AND LEVEL CARRY ACROSS CHUNKS.  Restarting them per buffer would
     * put a discontinuity at every boundary — an audible click roughly twice a
     * second, and a frequency that is subtly wrong because the last partial
     * half-period is dropped each time. */
    /* Through a STREAM, like every other source — so a tone no longer owns
     * the device and can be heard over whatever else is playing. */
    struct audio_stream* st = audio_stream_open("tone");
    if (!st) return -1;
    uint32_t done = 0;
    while (done < want) {
        uint32_t n = want - done;
        if (n > TONE_MAX_FRAMES) n = TONE_MAX_FRAMES;
        for (uint32_t f = 0; f < n; f++) {
            g_tone[f * 2 + 0] = level;         /* left                        */
            g_tone[f * 2 + 1] = level;         /* right                       */
            if (++phase >= half) { phase = 0; level = (int16_t)-level; }
        }
        if (audio_stream_write(st, g_tone, n) < (int)n) break;
        done += n;
    }
    audio_stream_close(st);                    /* drains before returning     */
    return 0;
}

/* ----------------------- Whole-buffer playback ---------------------------- */

int audio_play_pcm(struct audio_dev* dev, const int16_t* frames, uint32_t nframes) {
    if (!dev || !dev->play) return -1;
    uint32_t done = 0;
    while (done < nframes) {
        int n = dev->play(dev, frames + (size_t)done * 2, nframes - done);
        if (n < 0) return n;
        if (n == 0) {                  /* accepted nothing and reported success */
            klog(KLOG_WARN, "audio", "%s accepted 0 frames — giving up\n", dev->name);
            return -1;                 /* better than spinning here forever     */
        }
        done += (uint32_t)n;
    }
    return 0;
}

/* ===========================================================================
 * Streams + the mixer (§M23 stage 4).  See audio.h for why this exists.
 * =========================================================================== */

#define MIX_STREAMS      8
/* Frames per mix pass.  ~85 ms at 48 kHz, and the size is MEASURED rather than
 * chosen for tidiness: the device is stopped between periods (one BDL entry
 * per call today), so every period boundary is a small gap, and QEMU's capture
 * holds the last sample across it.  At 2048 the stretch was visible —
 * 300 ms of square wave came back as 323 ms at 411 Hz instead of 443 — while
 * 4096 measures clean.  The real fix is queueing the next buffer before the
 * current one drains (the AC97 BDL has 32 entries and we use one); until then
 * this is the trade, and it costs latency, not correctness. */
#define MIX_PERIOD_MAX   4096   /* the buffer we allocate; the default too      */
#define STREAM_FRAMES    16384                 /* ring depth, ~341 ms at 48 k  */
#define STREAM_MASK      (STREAM_FRAMES - 1)   /* power of two: index by AND   */

struct audio_stream {
    int      used;
    char     name[16];
    int16_t  ring[STREAM_FRAMES * 2];
    /* SINGLE producer, SINGLE consumer: the writer owns `head`, the pump owns
     * `tail`, and neither needs a lock to read the other's — the ring's
     * contents are only ever touched in the region the index pair says is
     * safe.  The table lock below covers open/close and the pump's walk, not
     * these. */
    volatile uint32_t head, tail;
    int      volume;                           /* 1/256ths, 256 = unity        */
    volatile int closing;                      /* drain, then release          */
};

static struct audio_stream g_streams[MIX_STREAMS];
static spinlock_t          g_mix_lock = SPINLOCK_INIT;
static struct waitq        g_mix_wq;           /* pump sleeps here when idle   */
static int                 g_pump_started;
static int16_t             g_mix_buf[MIX_PERIOD_MAX * 2];

/* How many frames one mix pass produces.  THE DEVICE CHOOSES, because the
 * floor is a driver property: one that stops the hardware between buffers
 * leaves a gap at every boundary and needs a long period to hide it, while one
 * that QUEUES can afford a short one.  Letting the core pick would hold a
 * queueing driver to a non-queueing driver's latency. */
static uint32_t mix_period(void) {
    struct audio_dev* d = audio_primary();
    uint32_t p = (d && d->period_frames) ? d->period_frames : MIX_PERIOD_MAX;
    if (p > MIX_PERIOD_MAX) p = MIX_PERIOD_MAX;
    return p;
}
static uint32_t            g_mix_passes, g_mix_clips;
/* Set by the pump when it has drained the device and is about to block.  A
 * closer waits for THIS rather than draining the device itself: only the pump
 * may touch the hardware (this file's own header says so), and the first
 * version broke that rule — the closer called `drain` while the pump could
 * still be queueing the final period, so the engine was stopped out from under
 * it.  The loss was small and variable, 3 to 9 ms off the end of a sound,
 * which is exactly what a race looks like from the outside. */
static volatile int        g_pump_idle = 1;

/* Master level, applied to the finished mix.  Separate from `muted` on
 * purpose — see audio.h: a mute that zeroes the volume forgets it. */
static int g_master_vol = 256;
static int g_master_mute = 0;

void audio_master_set(int vol_256, int muted) {
    if (vol_256 < 0)   vol_256 = 0;
    if (vol_256 > 256) vol_256 = 256;
    g_master_vol  = vol_256;
    g_master_mute = muted ? 1 : 0;
    klog(KLOG_INFO, "audio", "master %d/256%s\n", g_master_vol,
         g_master_mute ? " (muted)" : "");
}

void audio_master_get(int* vol_256, int* muted) {
    if (vol_256) *vol_256 = g_master_vol;
    if (muted)   *muted   = g_master_mute;
}

int audio_available(void) { return audio_primary() != NULL; }

static int streq_a(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static uint32_t stream_count(const struct audio_stream* s) {
    return s->head - s->tail;                  /* wraps correctly on uint32    */
}

static int any_stream_has_audio(void) {
    for (int i = 0; i < MIX_STREAMS; i++) {
        const struct audio_stream* s = &g_streams[i];
        if (s->used && stream_count(s)) return 1;
    }
    return 0;
}

/* Mix one period.  Returns the number of frames produced (0 = nothing to do).
 *
 * Streams that are EMPTY contribute silence rather than stalling the mix: one
 * source running dry must not stop the others, which is the difference between
 * a mixer and a queue. */
static uint32_t mix_one_period(void) {
    uint32_t want = mix_period(), produced = 0;
    for (uint32_t i = 0; i < want * 2; i++) g_mix_buf[i] = 0;

    for (int i = 0; i < MIX_STREAMS; i++) {
        struct audio_stream* s = &g_streams[i];
        if (!s->used) continue;
        uint32_t have = stream_count(s);
        if (!have) continue;
        uint32_t n = have < want ? have : want;
        if (n > produced) produced = n;

        uint32_t t = s->tail;
        for (uint32_t f = 0; f < n; f++) {
            uint32_t idx = (t + f) & STREAM_MASK;
            for (int c = 0; c < 2; c++) {
                int32_t v   = s->ring[idx * 2 + c];
                int32_t cur = g_mix_buf[f * 2 + c];
                int32_t sum = cur + ((v * s->volume) >> 8);
                /* SATURATE.  A wrap here turns the loudest instant into white
                 * noise, and only when two streams overlap — maximally audible
                 * and maximally confusing to diagnose. */
                if (sum >  32767) { sum =  32767; g_mix_clips++; }
                if (sum < -32768) { sum = -32768; g_mix_clips++; }
                g_mix_buf[f * 2 + c] = (int16_t)sum;
            }
        }
        s->tail = t + n;                       /* publish AFTER reading        */
    }

    /* MASTER, applied once to the finished mix.  Muted still produces frames
     * rather than skipping the period: the device keeps its timing, the
     * streams keep draining, and unmuting resumes mid-sound instead of
     * replaying a backlog that piled up while nothing was consuming. */
    if (produced) {
        int v = g_master_mute ? 0 : g_master_vol;
        if (v != 256) {
            for (uint32_t i = 0; i < produced * 2; i++)
                g_mix_buf[i] = (int16_t)((g_mix_buf[i] * v) >> 8);
        }
    }
    return produced;
}

static void mixer_pump(void) {
    for (;;) {
        struct audio_dev* dev = audio_primary();

        /* THE DOCUMENTED DISCIPLINE, verbatim from waitq.h: hold the lock,
         * loop on the condition, unlock after.  `waitq_block` RE-ACQUIRES the
         * lock before returning — the first version treated it as if it
         * returned unlocked and `continue`d straight back into `waitq_lock`,
         * which is a task deadlocking against itself with interrupts masked.
         * It presented as an NMI hard lockup in `hal_cpu_pause`, and §4.67.1's
         * report named the wedged CPU correctly. */
        /* Nothing left to mix: let the hardware finish what it already has,
         * THEN say we are idle, THEN block.  The order matters — announcing
         * idle before the drain would let a closer return while the tail of
         * the sound is still in the device. */
        {
            struct audio_dev* d = audio_primary();
            if (d && !any_stream_has_audio()) {
                if (d->drain) d->drain(d);
                g_pump_idle = 1;
            }
        }

        uint32_t fl = waitq_lock(&g_mix_wq);
        while (!(dev = audio_primary()) || !any_stream_has_audio())
            waitq_block(&g_mix_wq);
        g_pump_idle = 0;
        waitq_unlock(&g_mix_wq, fl);

        uint32_t n = mix_one_period();
        if (!n) continue;
        g_mix_passes++;
        /* The device call BLOCKS for the period's real duration — that is what
         * paces the whole mixer, so there is no timing loop anywhere here. */
        audio_play_pcm(dev, g_mix_buf, n);
    }
}

static void mixer_start_once(void) {
    uint32_t fl = spin_lock_irqsave(&g_mix_lock);
    int start = !g_pump_started;
    g_pump_started = 1;
    spin_unlock_irqrestore(&g_mix_lock, fl);
    if (start) {
        waitq_init(&g_mix_wq);
        task_spawn_detached("sound", mixer_pump);
    }
}

struct audio_stream* audio_stream_open(const char* name) {
    if (!audio_primary()) return NULL;
    mixer_start_once();

    uint32_t fl = spin_lock_irqsave(&g_mix_lock);
    struct audio_stream* s = NULL;
    for (int i = 0; i < MIX_STREAMS; i++) {
        if (!g_streams[i].used) { s = &g_streams[i]; break; }
    }
    if (s) {
        s->used = 1; s->head = s->tail = 0; s->volume = 256; s->closing = 0;
        int i = 0;
        for (; name && name[i] && i < (int)sizeof s->name - 1; i++) s->name[i] = name[i];
        s->name[i] = '\0';
    }
    spin_unlock_irqrestore(&g_mix_lock, fl);
    if (!s) klog(KLOG_WARN, "audio", "no free stream (%d in use)\n", MIX_STREAMS);
    return s;
}

void audio_stream_volume(struct audio_stream* s, int vol_256) {
    if (!s) return;
    if (vol_256 < 0)   vol_256 = 0;
    if (vol_256 > 1024) vol_256 = 1024;        /* 4x, bounded: above that the
                                                * mix is clipping by design    */
    s->volume = vol_256;
}

int audio_stream_write(struct audio_stream* s, const int16_t* frames, uint32_t nframes) {
    if (!s || !s->used || !frames) return -1;
    uint32_t written = 0;

    while (written < nframes) {
        uint32_t used = stream_count(s);
        uint32_t room = STREAM_FRAMES - used;
        if (!room) {
            /* Full: wait for the pump to drain some.  A SLEEP rather than a
             * second waitq, deliberately — the ring is ~341 ms deep, so a 2 ms
             * poll costs nothing measurable, and it buys a BOUNDED wait: a
             * device that stops consuming makes the writer fail and say so
             * instead of parking forever on a wake that will never come. */
            uint64_t deadline = timer_ticks_ms() + 5000;
            while (stream_count(s) >= STREAM_FRAMES) {
                if (timer_ticks_ms() > deadline) {
                    klog(KLOG_WARN, "audio", "stream '%s' stalled — %u frames written\n",
                         s->name, written);
                    return (int)written;
                }
                task_msleep(2);
            }
            continue;
        }
        uint32_t n = nframes - written;
        if (n > room) n = room;

        uint32_t h = s->head;
        for (uint32_t f = 0; f < n; f++) {
            uint32_t idx = (h + f) & STREAM_MASK;
            s->ring[idx * 2 + 0] = frames[(written + f) * 2 + 0];
            s->ring[idx * 2 + 1] = frames[(written + f) * 2 + 1];
        }
        s->head = h + n;                       /* publish AFTER filling        */
        written += n;

        /* Tell the pump there is work.  Every write, not just the first: the
         * pump may have gone back to sleep between periods.  Under the lock,
         * because waitq.h says so — the queue is mutated by the wake, and
         * doing it unlocked corrupts the list a blocked task is linked into. */
        uint32_t wf = waitq_lock(&g_mix_wq);
        waitq_wake_all(&g_mix_wq);
        waitq_unlock(&g_mix_wq, wf);
    }
    return (int)written;
}

void audio_stream_close(struct audio_stream* s) {
    if (!s || !s->used) return;

    /* DRAIN, do not drop.  A close that discarded the buffered tail would cut
     * the end off every sound, and the caller — which asked for the whole
     * thing to be played — has no way to notice. */
    uint64_t deadline = timer_ticks_ms() + 10000;
    while (stream_count(s)) {
        uint32_t wf = waitq_lock(&g_mix_wq);
        waitq_wake_all(&g_mix_wq);
        waitq_unlock(&g_mix_wq, wf);
        if (timer_ticks_ms() > deadline) break;
        task_msleep(2);
    }

    uint32_t fl = spin_lock_irqsave(&g_mix_lock);
    s->used = 0;
    s->head = s->tail = 0;
    int last = !any_stream_has_audio();
    for (int i = 0; i < MIX_STREAMS && last; i++) if (g_streams[i].used) last = 0;
    spin_unlock_irqrestore(&g_mix_lock, fl);

    /* THE RING BEING EMPTY IS NOT THE SOUND BEING OVER.  `play` returns when
     * frames are QUEUED, so the last period may still be inside the device —
     * returning now would let the caller print its prompt, or the next sound
     * start, over the tail of this one.
     *
     * We wait for the PUMP to report itself idle rather than draining the
     * device here: only the pump touches the hardware.  Only the last stream
     * waits — doing it while another is still playing would block this caller
     * until that one finished too. */
    if (last) {
        uint64_t d2 = timer_ticks_ms() + 10000;
        while (!g_pump_idle && timer_ticks_ms() < d2) task_msleep(2);
    }
}

void audio_stream_list(void) {
    uint32_t fl = spin_lock_irqsave(&g_mix_lock);
    int n = 0;
    for (int i = 0; i < MIX_STREAMS; i++) if (g_streams[i].used) n++;
    kprintf("streams: %d/%d open, %u mix passes, %u clipped samples\n",
            n, MIX_STREAMS, g_mix_passes, g_mix_clips);
    for (int i = 0; i < MIX_STREAMS; i++) {
        struct audio_stream* s = &g_streams[i];
        if (!s->used) continue;
        kprintf("  %s  %u frames buffered  vol %d/256\n",
                s->name, stream_count(s), s->volume);
    }
    spin_unlock_irqrestore(&g_mix_lock, fl);
}

/* ----------------------- WAV playback (§M23 stage 2) ---------------------- */

/* A bounded reader over the source file.  The point of it is that NOTHING here
 * holds the song: one input block and one output block, both static, both
 * small.  §M60 made the same argument about a 9 MB wallpaper — and a WAV is
 * worse, because a four-minute one is ~40 MB. */
#define WAV_IN_BYTES    8192
#define WAV_OUT_FRAMES  4096                  /* ×4 bytes = 16 KB              */

static uint8_t  g_wav_in[WAV_IN_BYTES];
static int16_t  g_wav_out[WAV_OUT_FRAMES * 2];

struct wav_src {
    struct file* f;
    uint32_t     remaining;                   /* bytes of `data` left unread   */
    int          fill, pos;                   /* g_wav_in occupancy            */
    uint16_t     channels, bits;
    uint16_t     block_align;                 /* bytes per source frame        */
};

static int wav_byte(struct wav_src* s) {
    if (s->pos >= s->fill) {
        if (s->remaining == 0) return -1;
        uint32_t want = WAV_IN_BYTES;
        if (want > s->remaining) want = s->remaining;
        ssize_t got = vfs_read(s->f, g_wav_in, want);
        if (got <= 0) return -1;
        s->remaining -= (uint32_t)got;
        s->fill = (int)got;
        s->pos  = 0;
    }
    return g_wav_in[s->pos++];
}

/* One source frame, normalised to 16-bit signed stereo.  Returns 0 on success,
 * -1 at end of data.
 *
 * The two source formats are the two that exist in practice: 16-bit signed
 * little-endian, and 8-bit UNSIGNED — the odd one out in WAV, where 8-bit
 * samples are biased around 128 rather than 0.  Treating them as signed is a
 * classic and very audible bug (loud buzz), so the bias is subtracted here
 * rather than assumed away. */
static int wav_frame(struct wav_src* s, int16_t* l, int16_t* r) {
    int16_t ch[2] = { 0, 0 };
    for (int c = 0; c < (int)s->channels; c++) {
        int v;
        if (s->bits == 8) {
            int b = wav_byte(s);
            if (b < 0) return -1;
            v = (b - 128) << 8;               /* unsigned → signed, 8 → 16 bit */
        } else {
            int lo = wav_byte(s), hi = wav_byte(s);
            if (lo < 0 || hi < 0) return -1;
            v = (int16_t)((uint16_t)lo | ((uint16_t)hi << 8));
        }
        if (c < 2) ch[c] = (int16_t)v;
    }
    /* MONO IS DUPLICATED, not left silent on one side: a mono file played into
     * one channel sounds like a broken speaker, and every listener blames the
     * hardware. */
    *l = ch[0];
    *r = (s->channels >= 2) ? ch[1] : ch[0];
    return 0;
}

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static int tag_is(const uint8_t* p, const char* s) {
    for (int i = 0; i < 4; i++) if (p[i] != (uint8_t)s[i]) return 0;
    return 1;
}

int audio_play_wav(const char* path) {
    struct audio_dev* dev = audio_primary();
    if (!dev) { kprintf("audio: no device\n"); return -1; }
    if (!path || !*path) { kprintf("play: give a path\n"); return -1; }

    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) { kprintf("play: cannot open %s\n", path); return -1; }

    uint8_t hdr[12];
    if (vfs_read(f, hdr, 12) != 12 || !tag_is(hdr, "RIFF") || !tag_is(hdr + 8, "WAVE")) {
        kprintf("play: %s is not a RIFF/WAVE file\n", path);
        vfs_close(f);
        return -1;
    }

    /* Walk the chunks.  `fmt ` and `data` are the two we need and they are NOT
     * guaranteed to be adjacent or in order — real files carry LIST/INFO and
     * fact chunks between them, so anything unrecognised is SKIPPED by its
     * declared length rather than treated as a parse error. */
    struct wav_src src = { f, 0, 0, 0, 0, 0, 0 };
    uint32_t rate = 0;
    int have_fmt = 0;
    for (;;) {
        uint8_t ch[8];
        if (vfs_read(f, ch, 8) != 8) break;
        uint32_t len = rd32(ch + 4);

        if (tag_is(ch, "fmt ") && len >= 16) {
            uint8_t fmt[16];
            if (vfs_read(f, fmt, 16) != 16) break;
            uint16_t format = rd16(fmt);
            src.channels    = rd16(fmt + 2);
            rate            = rd32(fmt + 4);
            src.block_align = rd16(fmt + 12);
            src.bits        = rd16(fmt + 14);
            /* 1 = PCM.  Anything else is compressed (ADPCM, µ-law, MP3-in-WAV)
             * and needs a decoder — REFUSED by name instead of played as
             * noise, which is what feeding compressed bytes to a DAC sounds
             * like. */
            if (format != 1) {
                kprintf("play: %s is WAV format %u, not PCM — refusing\n", path, format);
                vfs_close(f);
                return -1;
            }
            if (src.bits != 8 && src.bits != 16) {
                kprintf("play: %u-bit samples are not supported (8 or 16)\n", src.bits);
                vfs_close(f);
                return -1;
            }
            if (src.channels < 1 || src.channels > 8) {
                kprintf("play: %u channels makes no sense\n", src.channels);
                vfs_close(f);
                return -1;
            }
            have_fmt = 1;
            if (len > 16) {                   /* extensible fmt — skip the tail */
                uint8_t skip[64];
                uint32_t rest = len - 16;
                while (rest) {
                    uint32_t n = rest > sizeof skip ? (uint32_t)sizeof skip : rest;
                    if (vfs_read(f, skip, n) != (ssize_t)n) { rest = 0; break; }
                    rest -= n;
                }
            }
            continue;
        }

        if (tag_is(ch, "data")) {
            if (!have_fmt) {
                kprintf("play: %s has data before fmt — refusing\n", path);
                vfs_close(f);
                return -1;
            }
            src.remaining = len;
            break;
        }

        /* Unknown chunk: skip its payload (padded to even, per RIFF). */
        uint32_t rest = len + (len & 1);
        uint8_t skip[64];
        while (rest) {
            uint32_t n = rest > sizeof skip ? (uint32_t)sizeof skip : rest;
            if (vfs_read(f, skip, n) != (ssize_t)n) { rest = 0; break; }
            rest -= n;
        }
    }

    if (!have_fmt || src.remaining == 0) {
        kprintf("play: %s has no PCM data\n", path);
        vfs_close(f);
        return -1;
    }

    uint32_t dst_rate = dev->rate ? dev->rate : 48000;
    uint32_t frames_in = src.remaining / (src.block_align ? src.block_align
                                                          : (uint32_t)(src.bits / 8) * src.channels);
    kprintf("play: %s — %u Hz, %u ch, %u-bit, %u frames (%u ms) -> %s at %u Hz\n",
            path, rate, src.channels, src.bits, frames_in,
            rate ? (frames_in * 1000u) / rate : 0, dev->name, dst_rate);
    if (rate != dst_rate)
        kprintf("play: resampling %u -> %u Hz (nearest sample)\n", rate, dst_rate);

    /* NEAREST-SAMPLE RATE CONVERSION, in fixed point.
     *
     * `step` is how far the source position advances per OUTPUT frame.  The
     * accumulator is carried across blocks and never recomputed from a block
     * index — the §M53 lesson about periodic timers, in a different costume:
     * re-deriving the position per block accumulates one rounding error per
     * block, and over a three-minute file that is audible drift rather than a
     * rounding detail. */
    uint32_t step = rate ? (uint32_t)(((uint64_t)rate << 16) / dst_rate) : (1u << 16);
    uint32_t acc  = 0;                        /* fractional source position     */
    int16_t  cl = 0, cr = 0;                  /* the current source frame       */
    if (wav_frame(&src, &cl, &cr) != 0) { vfs_close(f); return -1; }
    uint32_t have = 1;                        /* source frames consumed so far  */

    /* Through a stream, so a file no longer owns the device: a second sound
     * during playback is MIXED rather than made to wait. */
    struct audio_stream* st = audio_stream_open("play");
    if (!st) { vfs_close(f); return -1; }

    uint32_t played = 0;
    int eof = 0;
    while (!eof) {
        uint32_t n = 0;
        while (n < WAV_OUT_FRAMES) {
            uint32_t want = (acc >> 16) + 1;   /* source frame this output needs */
            while (have < want) {
                if (wav_frame(&src, &cl, &cr) != 0) { eof = 1; break; }
                have++;
            }
            if (eof) break;
            g_wav_out[n * 2 + 0] = cl;
            g_wav_out[n * 2 + 1] = cr;
            n++;
            acc += step;
        }
        if (n && audio_stream_write(st, g_wav_out, n) < (int)n) {
            kprintf("play: device stopped accepting audio\n");
            audio_stream_close(st);
            vfs_close(f);
            return -1;
        }
        played += n;
        /* Keep the accumulator small so it cannot wrap on a long file: the
         * source frames already consumed are behind us for good. */
        acc  -= (have - 1) << 16;
        have  = 1;
    }

    audio_stream_close(st);                    /* drains before returning     */
    vfs_close(f);
    kprintf("play: done — %u frames (%u ms)\n", played,
            (played * 1000u) / dst_rate);
    return 0;
}

/* ----------------------- A test file, written by the GUEST ---------------- */

/* §M60's rule: the test asset is generated HERE rather than shipped, so it
 * travels through the real VFS and the real parser instead of a path the test
 * prepared for itself.
 *
 * It is deliberately NOT the device's format — 22050 Hz, MONO, 8-bit unsigned
 * — because those are the three conversions that can silently be wrong, and a
 * file that already matched the DAC would exercise none of them.  A square
 * wave, because its frequency survives resampling in a way a measurement can
 * check (count zero crossings) and its amplitude is a constant a test can
 * assert on. */
static int wav_put(struct file* f, const void* p, uint32_t n) {
    return vfs_write(f, p, n) == (ssize_t)n ? 0 : -1;
}

static int audio_write_testwav(const char* path, uint32_t freq, uint32_t ms) {
    const uint32_t rate = 22050;
    uint32_t frames = (rate / 1000u) * ms;
    uint32_t data_len = frames;                    /* 8-bit mono: 1 byte/frame */

    struct file* f = vfs_open(path, VFS_WRONLY | VFS_CREATE);
    if (!f) { kprintf("play: cannot create %s\n", path); return -1; }

    uint8_t h[44];
    #define P4(o,s) do { h[o]=(uint8_t)s[0]; h[o+1]=(uint8_t)s[1]; \
                         h[o+2]=(uint8_t)s[2]; h[o+3]=(uint8_t)s[3]; } while (0)
    #define P32(o,v) do { uint32_t _v=(v); h[o]=(uint8_t)_v; h[o+1]=(uint8_t)(_v>>8); \
                          h[o+2]=(uint8_t)(_v>>16); h[o+3]=(uint8_t)(_v>>24); } while (0)
    #define P16(o,v) do { uint16_t _v=(uint16_t)(v); h[o]=(uint8_t)_v; \
                          h[o+1]=(uint8_t)(_v>>8); } while (0)
    P4(0, "RIFF");  P32(4, 36 + data_len);  P4(8, "WAVE");
    P4(12, "fmt "); P32(16, 16);
    P16(20, 1);                                    /* PCM                      */
    P16(22, 1);                                    /* mono                     */
    P32(24, rate);
    P32(28, rate);                                 /* byte rate = rate × 1 × 1 */
    P16(32, 1);                                    /* block align              */
    P16(34, 8);                                    /* bits                     */
    P4(36, "data"); P32(40, data_len);
    #undef P4
    #undef P32
    #undef P16
    if (wav_put(f, h, 44) != 0) { vfs_close(f); return -1; }

    if (!freq) freq = 440;                         /* never divide by zero */
    uint32_t half = rate / (freq * 2);
    if (!half) half = 1;
    uint8_t blk[512];
    uint32_t phase = 0, written = 0;
    uint8_t level = 200;                           /* unsigned: 128 is silence */
    while (written < data_len) {
        uint32_t n = data_len - written;
        if (n > sizeof blk) n = (uint32_t)sizeof blk;
        for (uint32_t i = 0; i < n; i++) {
            blk[i] = level;
            if (++phase >= half) { phase = 0; level = (level == 200) ? 56 : 200; }
        }
        if (wav_put(f, blk, n) != 0) { vfs_close(f); return -1; }
        written += n;
    }
    vfs_close(f);
    kprintf("play: wrote %s — %u Hz square, %u Hz mono 8-bit, %u ms (%u bytes)\n",
            path, freq, rate, ms, 44 + data_len);
    return 0;
}

/* ----------------------- /dev/dsp (§M23 stage 3) -------------------------- */

/* RAW PCM AS A FILE.  `cat sound.raw > /dev/dsp` plays it, and a ring-3
 * program reaches the speaker by opening a file — no new syscall, and it works
 * for BOTH personalities, because a Linux-ABI binary has no d-os syscall
 * numbers to call (the argument §M59 made for /dev/clipboard, and the reason
 * that device shipped instead of an ABI operation).
 *
 * THE NODE BELONGS TO THE SUBSYSTEM, NOT TO A CARD.  It is registered whether
 * or not any driver came up, and a write with no device fails with a REASON.
 * Registering it from the driver instead would make `/dev/dsp` present on a
 * machine with a sound card and *absent* on one without — so "this system
 * cannot play audio" and "this system has no such device file" would look
 * identical to a program, and the second is a much more confusing answer.
 *
 * The format is the device's own: 16-bit signed stereo at its native rate.
 * There is no `SNDCTL_DSP_SPEED` to change it, and asking is REFUSED rather
 * than accepted-and-ignored — silently taking a rate you then do not honour
 * plays everything at the wrong pitch, which is the single most confusing way
 * for an audio device to fail.  A caller asks what the device IS (the two
 * ioctls below) and converts on its own side; `play` already does exactly
 * that, and its converter is the one to reuse. */

#define DSP_IOC_GET_RATE     0x4401
#define DSP_IOC_GET_CHANNELS 0x4402

/* §M23 stage 4 — the node now owns a STREAM rather than the device.  The
 * "one writer at a time" busy flag this used to need is gone: two programs
 * writing to /dev/dsp are two streams and get MIXED, which is what a person
 * expects a sound device to do.
 *
 * The stream is opened lazily on the first write and released by close(),
 * because devfs has no open hook — only the close one this device needed for
 * its drain. */
static struct audio_stream* g_dsp_stream = NULL;
/* Has this open already started a capture session?  Reset by close(), so each
 * open of /dev/dsp reads audio from when it opened rather than from whenever
 * the device last happened to be armed. */
static int g_dsp_rec_armed = 0;

/* A write must be a whole number of FRAMES, and a caller has no reason to know
 * that: `cat` writes whatever block size it likes, so a 4-byte frame gets
 * split across two writes roughly always.  The remainder is carried here
 * instead of being dropped — dropping it does not merely lose a sample, it
 * shifts every following sample by one channel and swaps left with right for
 * the rest of the stream. */
static uint8_t g_dsp_tail[4];
static int     g_dsp_tail_n = 0;

/* The period being assembled.  Its own buffer, not the WAV player's: the two
 * can be in use at the same time (a `play` on one task, a /dev/dsp writer on
 * another) and sharing the scratch would make each corrupt the other's audio
 * in a way that only shows up under concurrency. */
static int16_t  g_dsp_buf[WAV_OUT_FRAMES * 2];
static uint32_t g_dsp_fill = 0;

static ssize_t dsp_dev_write(void* ctx, const void* buf, size_t n, uint64_t off) {
    (void)ctx; (void)off;                     /* a stream has no position */
    if (!buf || n == 0) return 0;

    struct audio_dev* dev = audio_primary();
    if (!dev) {
        klog(KLOG_WARN, "audio", "/dev/dsp: no audio device\n");
        return -1;
    }

    if (!g_dsp_stream) {
        g_dsp_stream = audio_stream_open("dsp");
        if (!g_dsp_stream) return -1;
    }

    const uint8_t* src = (const uint8_t*)buf;
    size_t consumed = 0;
    ssize_t rc = (ssize_t)n;

    /* BUFFER UP TO A FULL PERIOD BEFORE PLAYING ANYTHING.
     *
     * The first version played whatever each write() contained.  A writer
     * using 1000-byte chunks — which is what a real one does — therefore got a
     * 250-frame playback per call: the DMA engine started and halted every
     * five milliseconds, and the gaps between those starts stretched the
     * stream.  It was audible in the capture as 403 Hz where 444 was written,
     * over 256 ms where 300 was asked for; the amplitude and the L/R pairing
     * were both perfect, which is what said the framing was right and the
     * PACING was wrong.
     *
     * So a write only plays whole periods, and whatever is left waits for the
     * next write — or for close(), which is why this device needed a close
     * hook to exist at all. */
    while (consumed < n) {
        while (g_dsp_fill < WAV_OUT_FRAMES && consumed < n) {
            while (g_dsp_tail_n < 4 && consumed < n) g_dsp_tail[g_dsp_tail_n++] = src[consumed++];
            if (g_dsp_tail_n < 4) break;      /* partial frame — keep it for next time */
            g_dsp_buf[g_dsp_fill * 2 + 0] = (int16_t)((uint16_t)g_dsp_tail[0] | ((uint16_t)g_dsp_tail[1] << 8));
            g_dsp_buf[g_dsp_fill * 2 + 1] = (int16_t)((uint16_t)g_dsp_tail[2] | ((uint16_t)g_dsp_tail[3] << 8));
            g_dsp_tail_n = 0;
            g_dsp_fill++;
        }
        if (g_dsp_fill < WAV_OUT_FRAMES) break;          /* not a period yet   */
        if (audio_stream_write(g_dsp_stream, g_dsp_buf, g_dsp_fill) < (int)g_dsp_fill) {
            rc = -1; break;
        }
        g_dsp_fill = 0;
    }
    return rc;
}

/* Drain on close — the tail of the sound.  Without this the last partial
 * period (up to ~85 ms) would be silently dropped from every write sequence,
 * which is the sort of loss that sounds like "the file was truncated". */
static int dsp_dev_close(void* ctx) {
    (void)ctx;
    if (g_dsp_stream) {
        if (g_dsp_fill) audio_stream_write(g_dsp_stream, g_dsp_buf, g_dsp_fill);
        audio_stream_close(g_dsp_stream);      /* drains */
        g_dsp_stream = NULL;
    }
    g_dsp_fill   = 0;
    g_dsp_tail_n = 0;          /* an incomplete frame at close is not audio */
    g_dsp_rec_armed = 0;
    return 0;
}

static ssize_t dsp_dev_read(void* ctx, void* buf, size_t n, uint64_t off) {
    (void)ctx; (void)off;                     /* a stream has no position */
    struct audio_dev* dev = audio_primary();
    if (!dev || !dev->record || !buf) {
        /* NOT 0: zero means END OF FILE, and a recorder told EOF writes an
         * empty file and reports success.  "This device cannot record" has to
         * be an error, not an empty result. */
        return -1;
    }
    if (!g_dsp_rec_armed) {
        if (dev->record_start) dev->record_start(dev);
        g_dsp_rec_armed = 1;
    }
    uint32_t want = (uint32_t)(n / 4);         /* whole frames only */
    if (!want) return 0;
    if (want > WAV_OUT_FRAMES) want = WAV_OUT_FRAMES;
    int got = dev->record(dev, g_dsp_buf, want);
    if (got <= 0) return got < 0 ? -1 : 0;
    for (int i = 0; i < got * 2; i++) {
        ((uint8_t*)buf)[i * 2 + 0] = (uint8_t)(g_dsp_buf[i] & 0xFF);
        ((uint8_t*)buf)[i * 2 + 1] = (uint8_t)((g_dsp_buf[i] >> 8) & 0xFF);
    }
    return (ssize_t)got * 4;
}

static int dsp_dev_ioctl(void* ctx, int cmd, void* arg) {
    (void)ctx;
    struct audio_dev* dev = audio_primary();
    if (!dev || !arg) return -1;
    if (cmd == DSP_IOC_GET_RATE)     { *(uint32_t*)arg = dev->rate;     return 0; }
    if (cmd == DSP_IOC_GET_CHANNELS) { *(uint32_t*)arg = dev->channels; return 0; }
    return -1;                                 /* including any "set format" */
}

static struct devfs_node dsp_dev = {
    .name = "dsp", .kind = DEVFS_CHAR,
    .read = dsp_dev_read, .write = dsp_dev_write,
    .ioctl = dsp_dev_ioctl, .ctx = NULL,
    .close = dsp_dev_close,
};

void audio_devfs_init(void) { devfs_register(&dsp_dev); }

/* `play dsptest [freq] [ms]` — drive /dev/dsp THROUGH THE VFS, the way a
 * program would, rather than calling the audio core directly.
 *
 * THE CHUNK SIZE IS DELIBERATELY AWKWARD.  1000 bytes is not a multiple of the
 * 4-byte frame, so every write leaves a partial frame behind — which is what a
 * real writer does (`cat` uses whatever block size it likes) and what the
 * device node has to carry across calls.  Getting that wrong does not merely
 * lose a sample: it shifts every following sample by one channel and swaps
 * left with right for the rest of the stream, which a capture makes obvious
 * and a listener would describe as "it sounds fine but backwards". */
static void audio_dsptest(uint32_t freq, uint32_t ms) {
    struct audio_dev* dev = audio_primary();
    if (!dev) { kprintf("audio: no device\n"); return; }
    if (!freq) freq = 440;
    if (!ms)   ms   = 300;

    struct file* f = vfs_open("/dev/dsp", VFS_WRONLY);
    if (!f) { kprintf("dsptest: cannot open /dev/dsp\n"); return; }

    uint32_t total = (dev->rate / 1000u) * ms;
    uint32_t half  = dev->rate / (freq * 2);
    if (!half) half = 1;
    int16_t level = 8000;
    uint32_t phase = 0;

    /* Render into the same output block the WAV player uses, then hand it to
     * the device node in 1000-byte pieces. */
    uint32_t done = 0;
    uint32_t wrote = 0;
    while (done < total) {
        uint32_t n = total - done;
        if (n > WAV_OUT_FRAMES) n = WAV_OUT_FRAMES;
        for (uint32_t i = 0; i < n; i++) {
            g_wav_out[i * 2 + 0] = level;
            g_wav_out[i * 2 + 1] = level;
            if (++phase >= half) { phase = 0; level = (int16_t)-level; }
        }
        const uint8_t* p = (const uint8_t*)g_wav_out;
        uint32_t bytes = n * 4, off = 0;
        while (off < bytes) {
            uint32_t chunk = bytes - off;
            if (chunk > 1000) chunk = 1000;        /* NOT a multiple of 4      */
            ssize_t w = vfs_write(f, p + off, chunk);
            if (w <= 0) { kprintf("dsptest: write failed at %u\n", wrote + off); vfs_close(f); return; }
            off += (uint32_t)w;
        }
        wrote += bytes;
        done  += n;
    }
    vfs_close(f);
    kprintf("dsptest: wrote %u bytes (%u frames, %u ms) to /dev/dsp in 1000-byte chunks\n",
            wrote, total, ms);
}

/* `play mixtest [ms]` — TWO SOURCES AT ONCE, from two TASKS.
 *
 * The assertion is an amplitude, because that is the one thing a mix cannot
 * fake: both tones are the same frequency and start together, at 6000 and
 * 4000, so a working mixer produces 10000 and either stream alone produces
 * 6000 or 4000.  A test that just played two sounds and listened for "both"
 * would pass with one of them silently dropped. */
static volatile int g_mixtest_ms;
static volatile int g_mixtest_done;

static void mixtest_second(void) {
    struct audio_stream* st = audio_stream_open("mixtest-B");
    if (st) {
        struct audio_dev* dev = audio_primary();
        uint32_t rate = dev ? dev->rate : 48000;
        uint32_t total = (rate / 1000u) * (uint32_t)g_mixtest_ms;
        uint32_t half = rate / 880;            /* 440 Hz: half-period frames   */
        int16_t level = 4000; uint32_t phase = 0, done = 0;
        while (done < total) {
            uint32_t n = total - done;
            if (n > WAV_OUT_FRAMES) n = WAV_OUT_FRAMES;
            for (uint32_t i = 0; i < n; i++) {
                g_tone[i * 2 + 0] = level; g_tone[i * 2 + 1] = level;
                if (++phase >= half) { phase = 0; level = (int16_t)-level; }
            }
            if (audio_stream_write(st, g_tone, n) < (int)n) break;
            done += n;
        }
        audio_stream_close(st);
    }
    g_mixtest_done = 1;
}

static void audio_mixtest(uint32_t ms) {
    struct audio_dev* dev = audio_primary();
    if (!dev) { kprintf("audio: no device\n"); return; }
    if (!ms) ms = 400;
    g_mixtest_ms = (int)ms;
    g_mixtest_done = 0;

    task_spawn_detached("mixtest", mixtest_second);

    struct audio_stream* st = audio_stream_open("mixtest-A");
    if (!st) { kprintf("mixtest: no stream\n"); return; }
    uint32_t rate = dev->rate;
    uint32_t total = (rate / 1000u) * ms;
    uint32_t half = rate / 880;
    int16_t level = 6000; uint32_t phase = 0, done = 0;
    while (done < total) {
        uint32_t n = total - done;
        if (n > WAV_OUT_FRAMES) n = WAV_OUT_FRAMES;
        for (uint32_t i = 0; i < n; i++) {
            g_wav_out[i * 2 + 0] = level; g_wav_out[i * 2 + 1] = level;
            if (++phase >= half) { phase = 0; level = (int16_t)-level; }
        }
        if (audio_stream_write(st, g_wav_out, n) < (int)n) break;
        done += n;
    }
    audio_stream_close(st);

    uint64_t deadline = timer_ticks_ms() + 5000;
    while (!g_mixtest_done && timer_ticks_ms() < deadline) task_msleep(5);
    kprintf("mixtest: two streams, %u ms, amplitudes 6000 + 4000 "
            "(a working mix reads 10000)\n", ms);
    audio_stream_list();
}



/* `volume` / `volume <0..100>` / `volume mute|unmute|toggle` — the headless
 * path, which exists BEFORE the taskbar control that calls the same functions
 * (the rule §M60 paid for: a setting with no shell command cannot be
 * regression-tested on a machine with no display). */
/* ----------------------- Capture to a file (§M23 stage 7) ----------------- */

int audio_record_wav(const char* path, uint32_t ms) {
    struct audio_dev* dev = audio_primary();
    if (!dev)          { kprintf("audio: no device\n"); return -1; }
    if (!dev->record)  { kprintf("rec: %s cannot record\n", dev->name); return -1; }
    if (!path || !*path) { kprintf("rec: give a path\n"); return -1; }
    if (!ms) ms = 1000;

    uint32_t rate  = dev->rate ? dev->rate : 48000;
    uint32_t total = (rate / 1000u) * ms;
    uint32_t bytes = total * 4;

    struct file* f = vfs_open(path, VFS_WRONLY | VFS_CREATE);
    if (!f) { kprintf("rec: cannot create %s\n", path); return -1; }

    /* A 44-byte canonical WAV header, written FIRST with the length we intend
     * to record.  If the capture stops short the header would over-promise, so
     * the real count is checked at the end and reported — a file whose header
     * lies about its length is worse than a short one. */
    uint8_t h[44];
    #define P4(o,str) do { h[o]=(uint8_t)str[0]; h[o+1]=(uint8_t)str[1]; \
                           h[o+2]=(uint8_t)str[2]; h[o+3]=(uint8_t)str[3]; } while (0)
    #define P32(o,v) do { uint32_t _v=(v); h[o]=(uint8_t)_v; h[o+1]=(uint8_t)(_v>>8); \
                          h[o+2]=(uint8_t)(_v>>16); h[o+3]=(uint8_t)(_v>>24); } while (0)
    #define P16(o,v) do { uint16_t _v=(uint16_t)(v); h[o]=(uint8_t)_v; \
                          h[o+1]=(uint8_t)(_v>>8); } while (0)
    P4(0,"RIFF"); P32(4, 36 + bytes); P4(8,"WAVE");
    P4(12,"fmt "); P32(16,16); P16(20,1); P16(22,2);
    P32(24, rate); P32(28, rate * 4); P16(32,4); P16(34,16);
    P4(36,"data"); P32(40, bytes);
    #undef P4
    #undef P32
    #undef P16
    vfs_write(f, h, 44);

    /* Start the session HERE, so what lands in the file is what happened from
     * this moment — not whatever the device had already buffered. */
    if (dev->record_start) dev->record_start(dev);

    uint64_t t0 = timer_ticks_ms();
    uint32_t done = 0;
    uint32_t first_ms = 0, first_frames = 0;   /* the FIRST buffer, measured  */
    while (done < total) {
        uint32_t want = total - done;
        if (want > WAV_OUT_FRAMES) want = WAV_OUT_FRAMES;
        int got = dev->record(dev, g_wav_out, want);
        if (got <= 0) break;
        /* THE FIRST BUFFER IS THE DEVICE'S OWN FILL RATE, with no backlog and
         * no accumulation behind it: a buffer of N frames cannot arrive faster
         * than N/rate seconds unless the device is producing faster than the
         * rate it reports.  One number that separates "our loop" from "the
         * device" — the whole reason the overall ratio was ambiguous. */
        if (!done) {
            first_ms     = (uint32_t)(timer_ticks_ms() - t0);
            first_frames = (uint32_t)got;   /* what it ACTUALLY returned */
        }
        vfs_write(f, g_wav_out, (size_t)got * 4);
        done += (uint32_t)got;
    }
    uint32_t elapsed = (uint32_t)(timer_ticks_ms() - t0);
    vfs_close(f);

    kprintf("rec: %s — %u of %u frames (%u ms asked, %u ms elapsed) at %u Hz\n",
            path, done, total, ms, elapsed, rate);
    /* Compare against the frames the call ACTUALLY returned, not the frames we
     * asked for: the driver clamps to its own buffer size, and the first
     * version of this line printed the request (4096 frames / 85 ms) against a
     * device that had handed back 1024 (21 ms) — an instrument that reports
     * the wrong expectation is worse than no instrument, because the number it
     * prints looks authoritative. */
    if (first_frames)
        kprintf("rec: first buffer %u frames in %u ms (nominal %u ms at %u Hz)\n",
                first_frames, first_ms, (first_frames * 1000u) / rate, rate);
    if (done < total)
        kprintf("rec: SHORT — the header promises %u frames\n", total);

    /* THE FILE IS FINE; THE TIMELINE IS THE THING TO REPORT.  A device that
     * hands over frames faster than real time still produces the right NUMBER
     * of them, so the recording plays back at the right length — but it did
     * not take the time it should have, and with a real microphone that is the
     * signature of an overrun: the samples being copied out are not the ones
     * that were arriving while we were away.
     *
     * Measured here as QEMU's AC97 input with the `none` backend, which is not
     * rate-limited; virtio-sound on the same core paces to within 1 %.  Said
     * out loud rather than smoothed over with a sleep, because the sleep would
     * make the number look right without making the audio right. */
    if (elapsed && elapsed * 10u < ms * 9u)
        kprintf("rec: captured FASTER than real time (%u ms for %u ms of audio)"
                " — the device is not rate-limiting its input\n", elapsed, ms);
    return done ? 0 : -1;
}

void audio_cmd_rec(const char* args) {
    while (args && *args == ' ') args++;
    if (!args || !*args) { kprintf("rec <path.wav> [ms]\n"); return; }
    char path[128];
    int n = 0;
    while (*args && *args != ' ' && n < (int)sizeof path - 1) path[n++] = *args++;
    path[n] = '\0';
    while (*args == ' ') args++;
    uint32_t ms = 0;
    while (*args >= '0' && *args <= '9') ms = ms * 10 + (uint32_t)(*args++ - '0');
    audio_record_wav(path, ms);
}

void audio_cmd_volume(const char* args) {
    while (args && *args == ' ') args++;
    int vol, muted;
    audio_master_get(&vol, &muted);

    if (!args || !*args) {
        kprintf("volume %d%%%s  (%s)\n", (vol * 100 + 128) / 256,
                muted ? " [MUTED]" : "",
                audio_available() ? "device present" : "NO AUDIO DEVICE");
        kprintf("  volume <0..100> | mute | unmute | toggle\n");
        return;
    }

    if (streq_a(args, "mute"))        { audio_master_set(vol, 1); }
    else if (streq_a(args, "unmute")) { audio_master_set(vol, 0); }
    else if (streq_a(args, "toggle")) { audio_master_set(vol, !muted); }
    else {
        uint32_t pct = 0; int digits = 0;
        for (const char* p = args; *p >= '0' && *p <= '9'; p++) {
            pct = pct * 10 + (uint32_t)(*p - '0'); digits++;
        }
        if (!digits) { kprintf("volume: <0..100> | mute | unmute | toggle\n"); return; }
        if (pct > 100) pct = 100;
        audio_master_set((int)((pct * 256 + 50) / 100), muted);
    }

    audio_master_get(&vol, &muted);
    kprintf("volume %d%%%s\n", (vol * 100 + 128) / 256, muted ? " [MUTED]" : "");
    /* Persist through config so it survives a reboot and the Control Panel
     * shows it — the §M63 machinery, reused rather than duplicated. */
    audio_volume_persist();
}

/* ---- persistence -----------------------------------------------------------
 * The master level is a SETTING, so it goes through §M63's machinery rather
 * than growing a private file: declared with CONFIG_KEY so the Control Panel
 * renders it with no per-key UI code, and watched so a value restored from the
 * disk (or set with `setconf`) takes effect immediately instead of one boot
 * later — the §M63 stage 0 lesson. */
CONFIG_KEY(ck_audio_volume) = {
    .key = "audio.volume", .group = "Sound", .type = CFG_INT,
    .min = 0, .max = 100, .def = "80",
    .help = "master output level, 0-100",
};
CONFIG_KEY(ck_audio_muted) = {
    .key = "audio.muted", .group = "Sound", .type = CFG_BOOL, .def = "0",
    .help = "silence all output without forgetting the level",
};

static int cfg_num(const char* key, int def) {
    const char* v = config_get(key, NULL);
    if (!v || !*v) return def;
    int n = 0, any = 0;
    for (; *v >= '0' && *v <= '9'; v++) { n = n * 10 + (*v - '0'); any = 1; }
    return any ? n : def;
}

static void audio_conf_changed(const char* key, const char* value) {
    (void)key; (void)value;
    /* ROUND TO NEAREST, both ways.  The level is stored in 1/256ths and shown
     * as a percentage, so truncating at each step made 35 come back as 34 —
     * a one-percent wobble that reads as "the setting did not take". */
    audio_master_set((cfg_num("audio.volume", 80) * 256 + 50) / 100,
                     cfg_num("audio.muted", 0));
}
CONFIG_WATCH(audio_watch) = {
    .prefix  = "audio.",
    .changed = audio_conf_changed,
};

void audio_volume_persist(void) {
    int vol, muted;
    audio_master_get(&vol, &muted);
    char b[8];
    int pct = (vol * 100 + 128) / 256, n = 0;
    if (!pct) b[n++] = '0';
    else { char t[8]; int m = 0; while (pct) { t[m++] = (char)('0' + pct % 10); pct /= 10; }
           while (m) b[n++] = t[--m]; }
    b[n] = 0;
    /* config_set, not config_apply: the value is already in effect, and
     * applying would re-enter the watcher to set what it just set. */
    config_set("audio.volume", b);
    config_set("audio.muted", muted ? "1" : "0");
}

void audio_cmd_play(const char* args) {
    while (args && *args == ' ') args++;
    if (!args || !*args) {
        kprintf("play <path.wav>  |  play testwav <path> [freq] [ms]"
                "  |  play dsptest [freq] [ms]  |  play mixtest [ms]\n");
        return;
    }

    {
        const char* p = args; const char* kw = "mixtest"; int is_mix = 1;
        for (int i = 0; kw[i]; i++) if (p[i] != kw[i]) { is_mix = 0; break; }
        if (is_mix && (p[7] == '\0' || p[7] == ' ')) {
            p += 7; while (*p == ' ') p++;
            uint32_t ms = 0;
            while (*p >= '0' && *p <= '9') ms = ms * 10 + (uint32_t)(*p++ - '0');
            audio_mixtest(ms);
            return;
        }
    }

    {
        const char* p = args;
        const char* kw = "dsptest";
        int is_dsp = 1;
        for (int i = 0; kw[i]; i++) if (p[i] != kw[i]) { is_dsp = 0; break; }
        if (is_dsp && (p[7] == '\0' || p[7] == ' ')) {
            p += 7;
            while (*p == ' ') p++;
            uint32_t freq = 0, ms = 0;
            while (*p >= '0' && *p <= '9') freq = freq * 10 + (uint32_t)(*p++ - '0');
            while (*p == ' ') p++;
            while (*p >= '0' && *p <= '9') ms = ms * 10 + (uint32_t)(*p++ - '0');
            audio_dsptest(freq, ms);
            return;
        }
    }

    /* `play testwav <path>` — generate, then play it, so one command exercises
     * the writer, the VFS and the parser in the order they actually run. */
    const char* p = args;
    int is_test = 1;
    const char* kw = "testwav ";
    for (int i = 0; kw[i]; i++) if (p[i] != kw[i]) { is_test = 0; break; }
    if (is_test) {
        p += 8;
        while (*p == ' ') p++;
        char path[128];
        int n = 0;
        while (*p && *p != ' ' && n < (int)sizeof path - 1) path[n++] = *p++;
        path[n] = '\0';
        while (*p == ' ') p++;
        uint32_t freq = 0, ms = 0;
        while (*p >= '0' && *p <= '9') freq = freq * 10 + (uint32_t)(*p++ - '0');
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') ms = ms * 10 + (uint32_t)(*p++ - '0');
        if (!path[0]) { kprintf("play: testwav needs a path\n"); return; }
        if (!freq) freq = 440;
        if (!ms)   ms   = 500;
        if (audio_write_testwav(path, freq, ms) != 0) return;
        audio_play_wav(path);
        return;
    }

    audio_play_wav(args);
}
