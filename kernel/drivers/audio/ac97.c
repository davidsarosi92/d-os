/* =============================================================================
 * ac97.c — Intel AC'97 audio driver (PCI vendor 0x8086, device 0x2415).
 *
 * QEMU exposes this with `-device AC97`.  It is the simplest well-documented
 * PC audio device: two PCI I/O BARs and a bus-master DMA engine fed by a
 * Buffer Descriptor List (BDL).  We implement PCM *output* only (a tone / WAV
 * player); input (mic/line) and the mixer beyond master+PCM volume are out of
 * §M23 scope.
 *
 * --------------------------------------------------------------------------
 * Two register windows (from the PCI BARs)
 * --------------------------------------------------------------------------
 *   BAR0 → NAM  (Native Audio Mixer)      — codec mixer: reset, volumes, rate
 *   BAR1 → NABM (Native Audio Bus Master) — the DMA engine + global control
 *
 * PCM-OUT box (the "PO" channel) lives at NABM offset 0x10:
 *   +0x10 PO_BDBAR (32)  physical base of the BDL
 *   +0x14 PO_CIV   (8)   current index value (RO)
 *   +0x15 PO_LVI   (8)   last valid index (RW)
 *   +0x16 PO_SR    (16)  status (RWC bits)
 *   +0x18 PO_PICB  (16)  position in current buffer, in samples (RO)
 *   +0x1B PO_CR    (8)   control (run / reset / interrupt-enables)
 *
 * Global:
 *   +0x2C GLOB_CNT (32)  bit1 = AC-link cold reset# (1 = out of reset)
 *
 * A BDL entry is 8 bytes: { u32 buffer phys addr; u16 length-in-samples;
 * u16 control (bit15 IOC, bit14 BUP) }.  "Samples" are 16-bit units, so a
 * stereo frame is 2 samples.
 *
 * Reference: Intel AC'97 spec + the osdev.org AC97 article.
 * ============================================================================= */

#include "audio.h"
#include "pci.h"
#include "hal.h"
#include "hal_api.h"
#include "pmm.h"
#include "printf.h"
#include "driver.h"
#include "task.h"      /* task_msleep — a wait must not cost a CPU */
#include "timer.h"     /* timer_ticks_ms — the drain deadline is real time */
#include <stdint.h>
#include <stddef.h>

#define AC97_VENDOR      0x8086
#define AC97_DEVICE      0x2415

/* NAM (mixer) register offsets. */
#define NAM_RESET        0x00
#define NAM_MASTER_VOL   0x02
#define NAM_PCM_VOL      0x18
#define NAM_EXT_AUDIO    0x28   /* extended audio ID (supports VRA?)          */
#define NAM_EXT_CTRL     0x2A   /* extended audio status/control (VRA enable) */
#define NAM_PCM_DAC_RATE 0x2C   /* front DAC sample rate (with VRA)           */

/* NABM (bus master) register offsets — PCM OUT box at 0x10. */
#define PO_BDBAR         0x10
#define PO_CIV           0x14
#define PO_LVI           0x15
#define PO_SR            0x16
#define PO_PICB          0x18
#define PO_CR            0x1B
#define NABM_GLOB_CNT    0x2C

/* PO_CR bits. */
#define CR_RPBM          0x01   /* run bus master                             */
#define CR_RR            0x02   /* reset registers                            */

/* PO_SR bits. */
#define SR_DCH           0x01   /* DMA controller halted                      */
#define SR_LVBCI         0x04   /* last valid buffer completion               */
#define SR_BCIS          0x08   /* buffer completion interrupt status         */

/* BDL control bits. */
#define BDL_IOC          (1u << 15)
#define BDL_BUP          (1u << 14)

#define AC97_RATE        48000
#define BDL_ENTRIES      32
/* THE BDL IS A RING NOW.  Four slices of the same 128 KB DMA region: the
 * driver fills one, points the engine at it and RETURNS, so the next buffer is
 * already queued before the current one drains and the hardware never stops
 * between them.  It used to be one buffer played to completion per call, which
 * made every boundary a gap — measured at 2048-frame periods as 300 ms of
 * square wave coming back as 323 ms at 411 Hz instead of 443.
 *
 * Four, not thirty-two: the depth IS the latency, and three buffers in flight
 * is enough to cover the gap between the mixer finishing one period and
 * starting the next.  A deeper queue would only make the volume slider feel
 * further away from the sound. */
/* THE RING IS THE WHOLE 32-ENTRY BDL, not a few slots of it.
 *
 * The first attempt used four entries and wrapped LVI every four — and lost
 * more than half the audio, deterministically and worse the longer the sound
 * (300 ms came back as 129, 600 ms as 88).  LVI is an index into the 32-entry
 * list and the engine plays until CIV reaches it: wrapping it back to 0 while
 * CIV sits at 3 reads as "already past the end", so the engine halts and the
 * queued buffers are skipped.  Wrapping over the FULL list is what the
 * hardware's index arithmetic expects.
 *
 * 32 x 1024 frames x 4 bytes = the same 128 KB allocation, and 1024 frames is
 * exactly the mix period, so a period is one entry. */
#define AC97_NBUF        BDL_ENTRIES        /* 32 — the whole list            */
#define AC97_BUF_FRAMES  1024
/* How far ahead we let the queue run.  THE DEPTH IS THE LATENCY: four buffers
 * is ~85 ms of sound already committed to the hardware, which is enough to
 * cover the gap between mix passes and little enough that the volume slider
 * still feels attached to what you hear. */
#define AC97_DEPTH       4
#define DMA_FRAMES       AC97_BUF_FRAMES

/* Slack on top of the buffer's own duration before the drain wait gives up.
 * The engine reports completion slightly after the last sample leaves, and an
 * emulated device under host load can be later still — so this is generous on
 * purpose: a spurious "timeout" would cut the tail off every sound. */
#define AC97_DRAIN_GRACE_MS  500
/* Time given to the codec to empty after the DMA engine reports itself halted
 * — see ac97_drain().  Measured, not guessed: without it a sound lost 3-10 ms
 * off its tail. */
#define AC97_SETTLE_MS       4

struct bdl_entry {
    uint32_t addr;
    uint16_t samples;
    uint16_t control;
} __attribute__((packed));

struct ac97 {
    uint16_t nam;                    /* mixer I/O base                        */
    uint16_t nabm;                   /* bus-master I/O base                   */
    uint32_t bdl_phys;               /* BDL (one frame)                       */
    struct bdl_entry* bdl;
    uint32_t pcm_phys;               /* PCM DMA buffer                        */
    int16_t* pcm;
    uint32_t head;                   /* next ring slot to fill                */
    int      running;                /* engine started and not yet drained    */
};

static struct ac97 g_ac97;
static struct audio_dev g_audio;

/* ----------------------- Playback ----------------------------------------- */

/* Take the PCM-out box back to a known state.  Used when starting a fresh
 * sound: after a halt the engine's current index still points at whatever it
 * finished, and starting again from there would REPLAY that buffer. */
static void ac97_engine_reset(struct ac97* a) {
    outb(a->nabm + PO_CR, 0);
    outb(a->nabm + PO_CR, CR_RR);
    for (uint32_t s = 0; (inb(a->nabm + PO_CR) & CR_RR) && s < 100000; s++) hal_cpu_pause();
    outw(a->nabm + PO_SR, 0x1C);
    outl(a->nabm + PO_BDBAR, a->bdl_phys);
    a->head = 0;
    a->running = 0;
}

/* How many buffers the engine has not finished with.
 *
 * `head` never catches up to CIV because the caller blocks at NBUF-1, so the
 * two indices are never equal while running — which is what makes this count
 * unambiguous rather than "full or empty, cannot tell". */
static uint32_t ac97_outstanding(struct ac97* a) {
    if (!a->running) return 0;
    if (inw(a->nabm + PO_SR) & SR_DCH) return 0;      /* halted: nothing left */
    uint32_t civ = (uint32_t)inb(a->nabm + PO_CIV) & (AC97_NBUF - 1);
    return (a->head - civ) & (AC97_NBUF - 1);
}

static void ac97_drain(struct audio_dev* dev) {
    struct ac97* a = (struct ac97*)dev->priv;
    if (!a->running) return;
    /* Bounded by what is actually queued plus slack, so a device that stops
     * answering costs a wait rather than a hung shell. */
    uint64_t deadline = timer_ticks_ms() +
                        (AC97_DEPTH * AC97_BUF_FRAMES * 1000ull) /
                        (dev->rate ? dev->rate : 48000u) + AC97_DRAIN_GRACE_MS;
    while (!(inw(a->nabm + PO_SR) & SR_DCH)) {
        if (timer_ticks_ms() > deadline) {
            kprintf("ac97: drain timeout\n");
            break;
        }
        task_msleep(1);
    }
    /* DCH says the DMA engine halted, which is NOT the same as the last
     * samples having left the codec.  Stopping the moment it sets cut a few
     * milliseconds off the end of every sound — small, variable, and audible
     * only as sounds feeling clipped.  Give it a beat before pulling the run
     * bit; the cost is paid once per sound, not per buffer. */
    task_msleep(AC97_SETTLE_MS);
    outb(a->nabm + PO_CR, 0);
    a->running = 0;
    a->head = 0;
}

static int ac97_play(struct audio_dev* dev, const int16_t* frames, uint32_t nframes) {
    struct ac97* a = (struct ac97*)dev->priv;
    if (nframes > AC97_BUF_FRAMES) nframes = AC97_BUF_FRAMES;
    if (nframes == 0) return 0;

    /* A fresh sound: the engine is stopped, so put it back to slot 0. */
    if (!a->running || (inw(a->nabm + PO_SR) & SR_DCH)) ac97_engine_reset(a);

    /* BLOCK WHILE THE QUEUE IS FULL.  This is what still paces the caller to
     * real time now that `play` returns on QUEUEING rather than on playback:
     * the queue only frees a slot when the hardware has consumed one.  Sleep,
     * never spin — the §M49/§M55/§M56 rule this driver already had to learn. */
    uint64_t deadline = timer_ticks_ms() +
                        (AC97_DEPTH * AC97_BUF_FRAMES * 1000ull) /
                        (dev->rate ? dev->rate : 48000u) + AC97_DRAIN_GRACE_MS;
    while (ac97_outstanding(a) >= AC97_DEPTH) {
        if (timer_ticks_ms() > deadline) {
            kprintf("ac97: queue stuck — dropping %u frames\n", nframes);
            return -1;
        }
        task_msleep(1);
    }

    uint32_t slot = a->head;
    int16_t* dst = a->pcm + (size_t)slot * AC97_BUF_FRAMES * 2;
    for (uint32_t i = 0; i < nframes * 2; i++) dst[i] = frames[i];

    a->bdl[slot].addr    = a->pcm_phys + slot * AC97_BUF_FRAMES * 4;
    a->bdl[slot].samples = (uint16_t)(nframes * 2);   /* stereo → 2 per frame */
    /* IOC so the engine updates its status per buffer; NOT BUP.  BUP means
     * "this is the end — emit zeros after it", which is right for a driver
     * that plays exactly one buffer and wrong for a ring: setting it on every
     * entry tells the engine each one is the last. */
    a->bdl[slot].control = BDL_IOC;

    /* LVI AFTER the entry is valid: it is what tells the engine the buffer
     * exists, so publishing it first is handing over a descriptor that is
     * still being written. */
    a->head = (slot + 1) & (AC97_NBUF - 1);
    outb(a->nabm + PO_LVI, (uint8_t)slot);

    if (!a->running) {
        outw(a->nabm + PO_SR, 0x1C);
        outb(a->nabm + PO_CR, CR_RPBM);
        a->running = 1;
    }
    return (int)nframes;                              /* QUEUED, not yet heard */
}

/* ----------------------- Bring-up ----------------------------------------- */

static int ac97_probe(void* ctx) {
    (void)ctx;
    struct pci_device pd;
    return pci_find_device(AC97_VENDOR, AC97_DEVICE, &pd) == 0 ? 0 : -1;
}

static int ac97_init(void* ctx) {
    (void)ctx;
    struct pci_device pd;
    if (pci_find_device(AC97_VENDOR, AC97_DEVICE, &pd) != 0) return -1;

    uint16_t nam  = pci_bar_io_base(pd.bar[0]);
    uint16_t nabm = pci_bar_io_base(pd.bar[1]);
    if (!nam || !nabm) { kprintf("ac97: BARs not I/O-space\n"); return -2; }

    /* Enable I/O + bus master. */
    uint16_t cmd = pci_read16(pd.bus, pd.slot, pd.func, PCI_COMMAND);
    cmd |= PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER;
    pci_write16(pd.bus, pd.slot, pd.func, PCI_COMMAND, cmd);

    g_ac97.nam = nam; g_ac97.nabm = nabm;

    /* Bring the AC-link out of cold reset, then reset the codec + PCM box. */
    outl(nabm + NABM_GLOB_CNT, 0x00000002);
    outw(nam + NAM_RESET, 1);                          /* any write resets codec */
    outb(nabm + PO_CR, CR_RR);                         /* reset the PCM-out box  */
    { uint32_t s = 0; while ((inb(nabm + PO_CR) & CR_RR) && ++s < 100000) hal_cpu_pause(); }

    /* Unmute + full volume on master and PCM (0x0000 = 0 dB attenuation). */
    outw(nam + NAM_MASTER_VOL, 0x0000);
    outw(nam + NAM_PCM_VOL,    0x0000);

    /* Try to set the DAC to 48 kHz via variable-rate audio, if supported. */
    outw(nam + NAM_EXT_CTRL, inw(nam + NAM_EXT_CTRL) | 0x0001);  /* VRA enable */
    outw(nam + NAM_PCM_DAC_RATE, AC97_RATE);

    /* DMA memory: one frame for the BDL, contiguous frames for the PCM
     * buffer.  PMM-backed → phys == virt in the identity map. */
    g_ac97.bdl_phys = pmm_alloc_frame_dma32();
    g_ac97.pcm_phys = pmm_alloc_contiguous_dma32(32);        /* 128 KB               */
    if (!g_ac97.bdl_phys || !g_ac97.pcm_phys) { kprintf("ac97: DMA OOM\n"); return -3; }
    g_ac97.bdl = (struct bdl_entry*)(uintptr_t)g_ac97.bdl_phys;
    g_ac97.pcm = (int16_t*)(uintptr_t)g_ac97.pcm_phys;
    for (int i = 0; i < BDL_ENTRIES; i++) { g_ac97.bdl[i].addr = 0; g_ac97.bdl[i].samples = 0; g_ac97.bdl[i].control = 0; }

    /* Register the abstract audio device. */
    g_audio.name     = "ac97";
    g_audio.rate     = AC97_RATE;
    g_audio.channels = 2;
    g_audio.play     = ac97_play;
    g_audio.drain    = ac97_drain;
    /* This driver QUEUES, so it can afford a short period — and says so rather
     * than letting the core assume the conservative one a non-queueing device
     * needs.  1024 frames = ~21 ms of mixer latency. */
    g_audio.period_frames = 1024;
    g_audio.priv     = &g_ac97;
    audio_register(&g_audio);

    kprintf("ac97: up at PCI %u:%u.%u nam=%x nabm=%x\n",
            pd.bus, pd.slot, pd.func, nam, nabm);
    return 0;
}

static const struct driver_ops ac97_ops = {
    .probe    = ac97_probe,
    .init     = ac97_init,
    .shutdown = NULL,
};

DRIVER(ac97, "audio", &ac97_ops, NULL);
