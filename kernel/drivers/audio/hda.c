/* =============================================================================
 * hda.c — Intel High Definition Audio, PCM output (§M23).
 *
 * The second PC audio device, next to AC97.  It exists because HDA is what
 * actual hardware from the last twenty years has, and because a second driver
 * behind `struct audio_dev` is the only real test of whether that interface
 * was an abstraction or just AC97 with extra steps.  (It was an abstraction:
 * the mixer, the WAV player, `/dev/dsp` and the taskbar control needed no
 * change at all.)
 *
 * --------------------------------------------------------------------------
 * Shape of the device
 * --------------------------------------------------------------------------
 * HDA is two halves that know almost nothing about each other:
 *
 *   THE CONTROLLER — a PCI device with an MMIO register block.  It owns DMA
 *   "streams", each fed by a Buffer Descriptor List exactly like AC97's, and
 *   it moves bytes between memory and a serial link.
 *
 *   THE CODEC(S)   — devices on that link, addressed by VERBS.  A codec is a
 *   graph of widgets (converters, pins, mixers) that must be walked to find
 *   out which node is a DAC and which is the socket the speaker is in.
 *
 * So bringing it up is: reset the controller, find which codec addresses
 * answered, walk that codec's widget graph for an output converter and an
 * output pin, wire and unmute them, then drive the DMA stream.
 *
 * --------------------------------------------------------------------------
 * Two deliberate simplifications, with their reasons
 * --------------------------------------------------------------------------
 * VERBS GO THROUGH THE IMMEDIATE COMMAND REGISTERS, not CORB/RIRB.  Those
 * rings exist so a driver can queue hundreds of verbs without a round trip
 * each; we send a few dozen, once, at bring-up.  Two DMA rings and their
 * wrap-around bookkeeping would be pure cost — and every one of them is a
 * thing to get wrong for no benefit this driver can measure.
 *
 * OUTPUT ONLY.  Capture on this architecture already works through AC97, and
 * an input stream here would be a second untestable path: no QEMU audio
 * backend available can inject a known signal (see DOCS §4.26.1), so it could
 * not be held to the standard the rest of this subsystem is held to.
 * ============================================================================= */

#include "audio.h"
#include "pci.h"
#include "hal.h"
#include "hal_api.h"
#include "vmm.h"
#include "pmm.h"
#include "printf.h"
#include "klog.h"
#include "driver.h"
#include "task.h"
#include "timer.h"
#include <stdint.h>
#include <stddef.h>

/* QEMU offers both: ich6 (`-device intel-hda`) and ich9. */
#define HDA_VENDOR_INTEL   0x8086
#define HDA_DEV_ICH6       0x2668
#define HDA_DEV_ICH9       0x293E

/* ---- controller registers (MMIO, byte offsets) ---------------------------- */
#define HDA_GCAP        0x00    /* 16: stream counts                          */
#define HDA_GCTL        0x08    /* 32: bit0 CRST — 0 resets, 1 runs           */
#define HDA_STATESTS    0x0E    /* 16: one bit per codec address that replied  */
#define HDA_INTCTL      0x20
#define HDA_IC          0x60    /* 32: immediate command                      */
#define HDA_IR          0x64    /* 32: immediate response                     */
#define HDA_ICS         0x68    /* 16: bit0 busy, bit1 result valid           */

#define GCTL_CRST       0x00000001u
#define ICS_BUSY        0x0001
#define ICS_VALID       0x0002

/* Stream descriptors begin at 0x80, 0x20 bytes apart.  The OUTPUT streams come
 * after the input ones, and how many inputs there are is in GCAP — assuming a
 * fixed layout would write a playback buffer into a capture engine on any
 * controller whose mix differs. */
#define HDA_SD_BASE     0x80
#define HDA_SD_STRIDE   0x20
#define SD_CTL          0x00    /* 24-bit: bit1 RUN, bit0 SRST, [23:20] tag   */
#define SD_STS          0x03
#define SD_CBL          0x08    /* cyclic buffer length, bytes                */
#define SD_LVI          0x0C    /* last valid index                           */
#define SD_FMT          0x12
#define SD_BDPL         0x18
#define SD_BDPU         0x1C

#define SDCTL_SRST      0x01
#define SDCTL_RUN       0x02
#define SDCTL_IOCE      0x04
#define SDSTS_BCIS      0x04    /* buffer completion                          */

/* ---- codec verbs ---------------------------------------------------------- */
#define VERB_GET_PARAM          0xF0000
#define VERB_SET_STREAM_CHAN    0x70600
#define VERB_SET_FORMAT         0x20000
#define VERB_SET_AMP            0x30000
#define VERB_SET_PIN_CTL        0x70700
#define VERB_SET_POWER          0x70500
#define VERB_SET_CONN_SEL       0x70100

#define PARAM_NODE_COUNT        0x04
#define PARAM_FN_GROUP_TYPE     0x05
#define PARAM_WIDGET_CAP        0x09
#define PARAM_PIN_CAP           0x0C
#define PARAM_CONN_LIST_LEN     0x0E
#define PARAM_OUT_AMP_CAP       0x12

#define WIDGET_TYPE(cap)        (((cap) >> 20) & 0xF)
#define WIDGET_AUDIO_OUT        0x0
#define WIDGET_PIN              0x4
#define PINCAP_OUTPUT           (1u << 4)

/* 48 kHz, 16-bit, 2 channels: base 48k, no mult/div, 16-bit = 0b001, chan-1 = 1. */
#define HDA_FMT_48K_16_2        0x0011

/* ---- sizing (mirrors the AC97 ring, for the same measured reasons) --------- */
#define HDA_NBUF        32                  /* the whole BDL                  */
#define HDA_BUF_FRAMES  1024                /* one mix period, ~21 ms         */
#define HDA_DEPTH       4                   /* buffers in flight = latency    */
#define HDA_DRAIN_GRACE_MS 500
#define HDA_SETTLE_MS      20   /* measured — see hda_drain() */

struct bdl_entry {
    uint32_t addr_lo, addr_hi;
    uint32_t len;
    uint32_t flags;                          /* bit0 = interrupt on completion */
} __attribute__((packed));

struct hda {
    uintptr_t mmio;
    int       codec;                         /* address that answered          */
    int       out_sd;                        /* first output stream descriptor */
    uint32_t  dac_nid, pin_nid;

    uint32_t  bdl_phys;
    struct bdl_entry* bdl;
    uint32_t  pcm_phys;
    int16_t*  pcm;
    uint32_t  head;
    int       running;
    volatile uint32_t submitted, completed;
    volatile int irq_seen;
};

static struct hda       g_hda;
static struct audio_dev g_audio;

static inline uint8_t  mr8 (uintptr_t b, uint32_t o) { return *(volatile uint8_t*)(b + o); }
static inline uint16_t mr16(uintptr_t b, uint32_t o) { return *(volatile uint16_t*)(b + o); }
static inline uint32_t mr32(uintptr_t b, uint32_t o) { return *(volatile uint32_t*)(b + o); }
static inline void mw8 (uintptr_t b, uint32_t o, uint8_t v)  { *(volatile uint8_t*)(b + o) = v; }
static inline void mw16(uintptr_t b, uint32_t o, uint16_t v) { *(volatile uint16_t*)(b + o) = v; }
static inline void mw32(uintptr_t b, uint32_t o, uint32_t v) { *(volatile uint32_t*)(b + o) = v; }

static uintptr_t sd_reg(struct hda* h) {
    return h->mmio + HDA_SD_BASE + (uintptr_t)h->out_sd * HDA_SD_STRIDE;
}

/* ---- codec verbs through the immediate-command registers ------------------ */

/* Send one verb, return the response.  `*ok` is cleared if the codec did not
 * answer — a caller that ignores that would read a stale IR and enumerate a
 * widget graph out of whatever the last verb returned. */
static uint32_t hda_verb(struct hda* h, uint32_t nid, uint32_t verb,
                         uint32_t payload, int* ok) {
    uint32_t cmd = ((uint32_t)h->codec << 28) | (nid << 20) | verb | payload;

    for (int i = 0; i < 1000 && (mr16(h->mmio, HDA_ICS) & ICS_BUSY); i++)
        hal_cpu_pause();

    mw16(h->mmio, HDA_ICS, ICS_VALID);        /* clear a stale result         */
    mw32(h->mmio, HDA_IC, cmd);
    mw16(h->mmio, HDA_ICS, ICS_BUSY);

    for (int i = 0; i < 100000; i++) {
        uint16_t st = mr16(h->mmio, HDA_ICS);
        if (!(st & ICS_BUSY)) {
            if (ok) *ok = (st & ICS_VALID) ? 1 : 0;
            return mr32(h->mmio, HDA_IR);
        }
        hal_cpu_pause();
    }
    if (ok) *ok = 0;
    return 0;
}

static uint32_t hda_param(struct hda* h, uint32_t nid, uint32_t p, int* ok) {
    return hda_verb(h, nid, VERB_GET_PARAM, p, ok);
}

/* Walk the codec for an output converter and an output pin.
 *
 * ENUMERATED, NOT HARDCODED.  QEMU's `hda-output` has a fixed and well-known
 * topology, and using its node numbers directly would work here and nowhere
 * else — including on the ich9 model in the same emulator.  The walk is thirty
 * lines and turns "works on the machine it was written on" into "works". */
static int hda_find_widgets(struct hda* h) {
    int ok = 0;
    uint32_t sub = hda_param(h, 0, PARAM_NODE_COUNT, &ok);
    if (!ok) return -1;
    uint32_t fg_start = (sub >> 16) & 0xFF, fg_count = sub & 0xFF;

    for (uint32_t fg = fg_start; fg < fg_start + fg_count; fg++) {
        uint32_t type = hda_param(h, fg, PARAM_FN_GROUP_TYPE, &ok);
        if (!ok || (type & 0x7F) != 0x01) continue;      /* not an audio FG   */

        /* Power the group up before asking it anything about its widgets. */
        hda_verb(h, fg, VERB_SET_POWER, 0, &ok);

        uint32_t wsub = hda_param(h, fg, PARAM_NODE_COUNT, &ok);
        if (!ok) continue;
        uint32_t w_start = (wsub >> 16) & 0xFF, w_count = wsub & 0xFF;

        h->dac_nid = h->pin_nid = 0;
        for (uint32_t w = w_start; w < w_start + w_count; w++) {
            uint32_t cap = hda_param(h, w, PARAM_WIDGET_CAP, &ok);
            if (!ok) continue;
            uint32_t t = WIDGET_TYPE(cap);
            if (t == WIDGET_AUDIO_OUT && !h->dac_nid) h->dac_nid = w;
            if (t == WIDGET_PIN && !h->pin_nid) {
                uint32_t pc = hda_param(h, w, PARAM_PIN_CAP, &ok);
                if (ok && (pc & PINCAP_OUTPUT)) h->pin_nid = w;
            }
        }
        if (h->dac_nid && h->pin_nid) return 0;
    }
    return -1;
}

/* Set one widget's output amp to its own 0 dB, or leave it alone if it has
 * none.  See the call site for why the distinction matters. */
static void hda_set_amp(struct hda* h, uint32_t nid, const char* what) {
    int ok = 0;
    uint32_t cap = hda_param(h, nid, PARAM_OUT_AMP_CAP, &ok);
    if (!ok || cap == 0) { kprintf("hda: %s has no output amp\n", what); return; }
    /* THE OFFSET FIELD IS THE ANSWER; THE STEP COUNT IS NOT A BOUND ON IT.
     * QEMU's codec reports 0x80034A4A — offset 0x4A with a "num steps" field
     * reading 3, which cannot both be true on a real part.  Clamping the
     * offset to that step count produced +/-313 for a tone written at
     * +/-8000: a plausible-looking correction that made things much worse.
     * The offset is what the spec says means unity, it fits in the 7-bit gain
     * field, and it measures right — so it is used as given.  The only
     * check that earns its place is "does this widget have an amp at all",
     * which is what silenced the PIN when it did not. */
    uint32_t offset = cap & 0x7F;
    hda_verb(h, nid, VERB_SET_AMP, 0xB000 | offset, &ok);
    kprintf("hda: %s amp gain %u (cap %x)\n", what, (unsigned)offset,
            (unsigned)cap);
}

/* ---- playback -------------------------------------------------------------- */

static void hda_stream_reset(struct hda* h) {
    uintptr_t sd = sd_reg(h);
    mw8(sd, SD_CTL, 0);
    for (int i = 0; i < 1000 && (mr8(sd, SD_CTL) & SDCTL_RUN); i++) hal_cpu_pause();
    mw8(sd, SD_CTL, SDCTL_SRST);
    for (int i = 0; i < 1000 && !(mr8(sd, SD_CTL) & SDCTL_SRST); i++) hal_cpu_pause();
    mw8(sd, SD_CTL, 0);
    for (int i = 0; i < 1000 && (mr8(sd, SD_CTL) & SDCTL_SRST); i++) hal_cpu_pause();

    mw32(sd, SD_BDPL, h->bdl_phys);
    mw32(sd, SD_BDPU, 0);
    mw32(sd, SD_CBL, HDA_NBUF * HDA_BUF_FRAMES * 4);
    mw16(sd, SD_LVI, HDA_NBUF - 1);
    mw16(sd, SD_FMT, HDA_FMT_48K_16_2);
    mw8(sd, SD_STS, SDSTS_BCIS);
    h->head = 0;
    h->running = 0;
    h->submitted = h->completed = 0;
}

/* How many buffers the controller has not finished with.  Without an interrupt
 * this is inferred from the link position; with one it is exact. */
static uint32_t hda_outstanding(struct hda* h) {
    if (!h->running) return 0;
    if (h->irq_seen) return h->submitted - h->completed;
    /* Position within the cyclic buffer tells us which entry it is on. */
    uint32_t pos = mr32(sd_reg(h), 0x04);            /* SD_LPIB */
    uint32_t cur = (pos / (HDA_BUF_FRAMES * 4)) % HDA_NBUF;
    return (h->head - cur) & (HDA_NBUF - 1);
}

static void hda_drain(struct audio_dev* dev) {
    struct hda* h = (struct hda*)dev->priv;
    if (!h->running) return;
    uint64_t deadline = timer_ticks_ms() +
                        (HDA_DEPTH * HDA_BUF_FRAMES * 1000ull) /
                        (dev->rate ? dev->rate : 48000u) + HDA_DRAIN_GRACE_MS;
    while (hda_outstanding(h)) {
        if (timer_ticks_ms() > deadline) { kprintf("hda: drain timeout\n"); break; }
        task_msleep(1);
    }
    /* An HDA stream is CYCLIC — it wraps to the first descriptor and plays the
     * ring again rather than halting at the last valid index — so unlike AC97
     * this one HAS to be stopped, or the tail of the sound loops forever.
     *
     * But not the instant the last buffer is accounted for: the link and the
     * codec are still emptying, and stopping there cut ~17 ms off a 600 ms
     * sound.  The settle is measured, not guessed, and it is the same crutch
     * AC97 carries on ITS polled path — with the same escape: wiring this
     * controller's completion interrupt would make the count exact and retire
     * the constant, exactly as it did there. */
    task_msleep(HDA_SETTLE_MS);
    mw8(sd_reg(h), SD_CTL, 0);
    h->running = 0;
    h->head = 0;
}

static int hda_play(struct audio_dev* dev, const int16_t* frames, uint32_t nframes) {
    struct hda* h = (struct hda*)dev->priv;
    if (!h->mmio) return -1;
    if (nframes > HDA_BUF_FRAMES) nframes = HDA_BUF_FRAMES;
    if (nframes == 0) return 0;

    if (!h->running) hda_stream_reset(h);

    uint64_t deadline = timer_ticks_ms() +
                        (HDA_DEPTH * HDA_BUF_FRAMES * 1000ull) /
                        (dev->rate ? dev->rate : 48000u) + HDA_DRAIN_GRACE_MS;
    while (hda_outstanding(h) >= HDA_DEPTH) {
        if (timer_ticks_ms() > deadline) {
            kprintf("hda: queue stuck — dropping %u frames\n", nframes);
            return -1;
        }
        task_msleep(1);
    }

    uint32_t slot = h->head;
    int16_t* dst = h->pcm + (size_t)slot * HDA_BUF_FRAMES * 2;
    for (uint32_t i = 0; i < nframes * 2; i++) dst[i] = frames[i];
    /* A short period must not leave the previous contents of the slot behind:
     * the descriptor's length is fixed by the cyclic buffer, so the tail of
     * the slot is played whatever we put there. */
    for (uint32_t i = nframes * 2; i < HDA_BUF_FRAMES * 2; i++) dst[i] = 0;

    h->head = (slot + 1) & (HDA_NBUF - 1);
    h->submitted++;

    if (!h->running) {
        uintptr_t sd = sd_reg(h);
        /* Stream tag 1, and the codec must be told the same number — the tag
         * is what associates this DMA engine with that converter.  Mismatch
         * them and the DMA runs happily into a converter that is listening
         * for somebody else. */
        mw32(sd, SD_CTL, (1u << 20) | SDCTL_RUN | SDCTL_IOCE);
        h->running = 1;
    }
    return (int)nframes;
}

/* ---- bring-up -------------------------------------------------------------- */

static int hda_probe(void* ctx) {
    (void)ctx;
    struct pci_device pd;
    if (pci_find_device(HDA_VENDOR_INTEL, HDA_DEV_ICH6, &pd) == 0) return 0;
    if (pci_find_device(HDA_VENDOR_INTEL, HDA_DEV_ICH9, &pd) == 0) return 0;
    return -1;
}

static int hda_init(void* ctx) {
    (void)ctx;
    struct pci_device pd;
    if (pci_find_device(HDA_VENDOR_INTEL, HDA_DEV_ICH6, &pd) != 0 &&
        pci_find_device(HDA_VENDOR_INTEL, HDA_DEV_ICH9, &pd) != 0) return -1;

    /* BAR0 is MEMORY here, not I/O — the first PCI device in this tree that is
     * (AC97 and virtio-net both use port I/O), so the window has to be mapped
     * before a single register can be read. */
    uint32_t bar = pd.bar[0];
    if (bar & 1) { kprintf("hda: BAR0 is I/O space — unexpected\n"); return -2; }
    uintptr_t phys = (uintptr_t)(bar & ~0xFu);
    if (!phys) { kprintf("hda: BAR0 not assigned\n"); return -2; }

    uint16_t cmd = pci_read16(pd.bus, pd.slot, pd.func, PCI_COMMAND);
    cmd |= PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER;
    pci_write16(pd.bus, pd.slot, pd.func, PCI_COMMAND, cmd);

    for (uintptr_t off = 0; off < 0x4000; off += 0x1000)
        vmm_map(phys + off, phys + off, VMM_WRITABLE);
    g_hda.mmio = phys;

    /* Out of reset, then wait for the link to come up.  Reading STATESTS
     * before the codecs have had time to answer finds none of them. */
    mw32(g_hda.mmio, HDA_GCTL, 0);
    for (int i = 0; i < 1000 && (mr32(g_hda.mmio, HDA_GCTL) & GCTL_CRST); i++)
        hal_cpu_pause();
    mw32(g_hda.mmio, HDA_GCTL, GCTL_CRST);
    for (int i = 0; i < 1000 && !(mr32(g_hda.mmio, HDA_GCTL) & GCTL_CRST); i++)
        hal_cpu_pause();
    task_msleep(2);                       /* codecs need a moment to reply */

    uint16_t sts = mr16(g_hda.mmio, HDA_STATESTS);
    if (!sts) { kprintf("hda: no codec responded\n"); return -3; }
    g_hda.codec = 0;
    while (g_hda.codec < 15 && !(sts & (1u << g_hda.codec))) g_hda.codec++;

    /* Which stream descriptor is the first OUTPUT one?  GCAP says how many
     * input streams come before it. */
    uint16_t gcap = mr16(g_hda.mmio, HDA_GCAP);
    g_hda.out_sd = (gcap >> 8) & 0x0F;               /* ISS count */

    if (hda_find_widgets(&g_hda) != 0) {
        kprintf("hda: no output converter + pin found on codec %d\n", g_hda.codec);
        return -4;
    }

    /* DMA memory: one frame for the BDL, 128 KB for the ring. */
    g_hda.bdl_phys = pmm_alloc_frame_dma32();
    g_hda.pcm_phys = pmm_alloc_contiguous_dma32(32);
    if (!g_hda.bdl_phys || !g_hda.pcm_phys) { kprintf("hda: DMA OOM\n"); return -5; }
    g_hda.bdl = (struct bdl_entry*)(uintptr_t)g_hda.bdl_phys;
    g_hda.pcm = (int16_t*)(uintptr_t)g_hda.pcm_phys;
    for (uint32_t i = 0; i < HDA_NBUF; i++) {
        g_hda.bdl[i].addr_lo = g_hda.pcm_phys + i * HDA_BUF_FRAMES * 4;
        g_hda.bdl[i].addr_hi = 0;
        g_hda.bdl[i].len     = HDA_BUF_FRAMES * 4;
        g_hda.bdl[i].flags   = 1;                    /* interrupt on completion */
    }

    /* Wire the codec: converter format + stream tag, pin powered, output
     * enabled, and both amps unmuted at 0 dB.  An amp left muted is the
     * classic "everything works and there is no sound" — the DMA runs, the
     * counters advance, and the speaker is disconnected. */
    int ok = 0;
    hda_verb(&g_hda, g_hda.dac_nid, VERB_SET_POWER, 0, &ok);
    hda_verb(&g_hda, g_hda.pin_nid, VERB_SET_POWER, 0, &ok);
    hda_verb(&g_hda, g_hda.dac_nid, VERB_SET_FORMAT, HDA_FMT_48K_16_2, &ok);
    hda_verb(&g_hda, g_hda.dac_nid, VERB_SET_STREAM_CHAN, (1u << 4) | 0, &ok);
    hda_verb(&g_hda, g_hda.pin_nid, VERB_SET_PIN_CTL, 0x40, &ok);   /* out enable */
    /* THE 0 dB GAIN IS A PROPERTY OF THE WIDGET, NOT A CONSTANT.  The amp
     * capabilities carry an OFFSET — the step index that means unity — and it
     * differs per widget and per codec.  The first version wrote a plausible
     * 0x2F and the capture came back at +/-5050 for a tone written at +/-8000:
     * not silence, not distortion, just quietly the wrong level, which is the
     * kind of wrong that gets shipped.  Ask, do not assume (§M61 made the same
     * argument about reading a display mode back). */
    /* AND CHECK THE AMP EXISTS BEFORE SETTING IT.  `Num Steps == 0` means the
     * widget has no output amplifier at all, and its capability word then
     * reads as zero — from which the offset field is also zero.  Writing that
     * as a gain is not "0 dB", it is MAXIMUM ATTENUATION: the second version
     * of this code silenced the pin completely, having just fixed the level on
     * the DAC.  An absent capability and a capability whose value is zero look
     * identical unless you ask the right field. */
    hda_set_amp(&g_hda, g_hda.dac_nid, "dac");
    hda_set_amp(&g_hda, g_hda.pin_nid, "pin");
    /* If the pin can choose its source, point it at our converter. */
    uint32_t cll = hda_param(&g_hda, g_hda.pin_nid, PARAM_CONN_LIST_LEN, &ok);
    if (ok && (cll & 0x7F) > 0)
        hda_verb(&g_hda, g_hda.pin_nid, VERB_SET_CONN_SEL, 0, &ok);

    g_audio.name          = "hda";
    g_audio.rate          = 48000;
    g_audio.channels      = 2;
    g_audio.play          = hda_play;
    g_audio.drain         = hda_drain;
    g_audio.period_frames = HDA_BUF_FRAMES;
    g_audio.priv          = &g_hda;
    audio_register(&g_audio);

    kprintf("hda: up at PCI %u:%u.%u mmio=%p codec=%d dac=%u pin=%u out_sd=%d\n",
            pd.bus, pd.slot, pd.func, (void*)g_hda.mmio, g_hda.codec,
            (unsigned)g_hda.dac_nid, (unsigned)g_hda.pin_nid, g_hda.out_sd);
    return 0;
}

static const struct driver_ops hda_ops = {
    .probe    = hda_probe,
    .init     = hda_init,
    .shutdown = NULL,
};

DRIVER(hda, "audio", &hda_ops, NULL);
