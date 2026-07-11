/* =============================================================================
 * audio.c — portable audio core (§M23): device registry + a square-wave tone
 * generator (the smoke test).  Arch-independent; the codec driver (AC97) is
 * the only hardware-specific piece and lives under kernel/drivers/audio/.
 * ============================================================================= */

#include "audio.h"
#include "printf.h"
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
    if (want > TONE_MAX_FRAMES) want = TONE_MAX_FRAMES;

    /* Square wave: +/- amplitude, half a period each way. */
    uint32_t half = dev->rate / (freq * 2);
    if (half == 0) half = 1;
    const int16_t amp = 8000;
    int16_t level = amp;
    uint32_t phase = 0;
    for (uint32_t f = 0; f < want; f++) {
        g_tone[f * 2 + 0] = level;             /* left                        */
        g_tone[f * 2 + 1] = level;             /* right                       */
        if (++phase >= half) { phase = 0; level = (int16_t)-level; }
    }

    kprintf("audio: playing %u Hz for %u ms (%u frames) on %s\n",
            freq, ms, want, dev->name);
    return dev->play(dev, g_tone, want);
}
