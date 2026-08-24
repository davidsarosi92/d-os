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
    uint32_t done = 0;
    while (done < want) {
        uint32_t n = want - done;
        if (n > TONE_MAX_FRAMES) n = TONE_MAX_FRAMES;
        for (uint32_t f = 0; f < n; f++) {
            g_tone[f * 2 + 0] = level;         /* left                        */
            g_tone[f * 2 + 1] = level;         /* right                       */
            if (++phase >= half) { phase = 0; level = (int16_t)-level; }
        }
        int rc = audio_play_pcm(dev, g_tone, n);
        if (rc != 0) return rc;
        done += n;
    }
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
        if (n && audio_play_pcm(dev, g_wav_out, n) != 0) {
            kprintf("play: device stopped accepting audio\n");
            vfs_close(f);
            return -1;
        }
        played += n;
        /* Keep the accumulator small so it cannot wrap on a long file: the
         * source frames already consumed are behind us for good. */
        acc  -= (have - 1) << 16;
        have  = 1;
    }

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

/* One writer at a time.  Two tasks writing interleaved periods would produce
 * garbled audio rather than a mix — mixing is a MIXER, which §M23 scopes out
 * — so the second writer waits its turn.  A flag plus a sleep, not a spinlock:
 * the write blocks for the buffer's real duration, and holding a spinlock
 * across that would be the §M49 mistake with extra steps. */
static volatile int g_dsp_busy = 0;

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

    /* Wait for the previous writer, with a bound so a wedged device cannot
     * hang every other writer forever. */
    uint64_t deadline = timer_ticks_ms() + 5000;
    while (g_dsp_busy) {
        if (timer_ticks_ms() > deadline) return -1;
        task_msleep(1);
    }
    g_dsp_busy = 1;

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
        if (audio_play_pcm(dev, g_dsp_buf, g_dsp_fill) != 0) { rc = -1; break; }
        g_dsp_fill = 0;
    }

    g_dsp_busy = 0;
    return rc;
}

/* Drain on close — the tail of the sound.  Without this the last partial
 * period (up to ~85 ms) would be silently dropped from every write sequence,
 * which is the sort of loss that sounds like "the file was truncated". */
static int dsp_dev_close(void* ctx) {
    (void)ctx;
    struct audio_dev* dev = audio_primary();
    if (dev && g_dsp_fill) audio_play_pcm(dev, g_dsp_buf, g_dsp_fill);
    g_dsp_fill   = 0;
    g_dsp_tail_n = 0;          /* an incomplete frame at close is not audio */
    return 0;
}

static ssize_t dsp_dev_read(void* ctx, void* buf, size_t n, uint64_t off) {
    (void)ctx; (void)buf; (void)n; (void)off;
    /* PCM capture is §M23's next stage and there is no input path at all yet.
     * Returning 0 would mean END OF FILE, and a recorder told EOF writes an
     * empty file and reports success — so this fails instead. */
    return -1;
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

void audio_cmd_play(const char* args) {
    while (args && *args == ' ') args++;
    if (!args || !*args) {
        kprintf("play <path.wav>  |  play testwav <path> [freq] [ms]"
                "  |  play dsptest [freq] [ms]\n");
        return;
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
