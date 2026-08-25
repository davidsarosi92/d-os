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
#include "idt.h"       /* irq_install, struct int_frame */
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

/* PCM IN box at NABM offset 0x00 — the same register layout as PCM OUT, one
 * box lower.  Capture is the same DMA engine pointed the other way: the device
 * WRITES our buffers instead of reading them. */
#define PI_BDBAR         0x00
#define PI_CIV           0x04
#define PI_LVI           0x05
#define PI_SR            0x06
#define PI_PICB          0x08
#define PI_CR            0x0B
#define NAM_PCM_ADC_RATE 0x32   /* record rate, with VRA                      */

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
#define CR_LVBIE         0x04   /* interrupt on last-valid-buffer             */
#define CR_IOCE          0x10   /* interrupt on buffer completion             */

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
/* How far the CAPTURE engine may run ahead of the reader, in buffers.  Small
 * for the reason above: the runway is latency on the way in, and a long one
 * lets the device lap the reader. */
#define AC97_REC_DEPTH   4
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
    /* §M23 stage 6 — completion accounting from the INTERRUPT.
     *
     * `submitted` is ours; `completed` is bumped by the ISR.  Their difference
     * is an EXACT count of what the device still holds — which is what the
     * aarch64 driver has had all along from its used ring, and why that one
     * measured exact while this one was inferring the answer from CIV plus a
     * settle constant.
     *
     * `irq_seen` is the §M55 rule: a driver learns that its interrupt works by
     * RECEIVING one.  Until then it keeps using the polled path, so wiring an
     * interrupt that never fires costs latency, not silence. */
    volatile uint32_t submitted;
    volatile uint32_t completed;
    volatile int      irq_seen;
    /* Capture side.  Its own BDL and buffers: recording while playing is two
     * independent DMA engines, and sharing either would make one silently
     * corrupt the other. */
    uint32_t rec_bdl_phys;
    struct bdl_entry* rec_bdl;
    uint32_t rec_pcm_phys;
    int16_t* rec_pcm;
    uint32_t rec_next;               /* next ring slot we will read from      */
    int      rec_running;
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
    a->submitted = a->completed = 0;
}

/* How many buffers the engine has not finished with.
 *
 * `head` never catches up to CIV because the caller blocks at NBUF-1, so the
 * two indices are never equal while running — which is what makes this count
 * unambiguous rather than "full or empty, cannot tell". */
static uint32_t ac97_outstanding(struct ac97* a) {
    if (!a->running) return 0;
    /* The exact answer, once the interrupt has proved itself. */
    if (a->irq_seen) return a->submitted - a->completed;
    if (inw(a->nabm + PO_SR) & SR_DCH) return 0;      /* halted: nothing left */
    uint32_t civ = (uint32_t)inb(a->nabm + PO_CIV) & (AC97_NBUF - 1);
    return (a->head - civ) & (AC97_NBUF - 1);
}

/* The ISR does two things and no third: acknowledge the device and count the
 * completion.  No mixing, no wakeups into the audio core — §M49's xHCI lesson,
 * where draining a ring inside the interrupt handler reached code that blocks. */
static void ac97_irq(struct int_frame* f) {
    (void)f;
    struct ac97* a = &g_ac97;
    if (!a->nabm) return;
    uint16_t sr = inw(a->nabm + PO_SR);
    if (!(sr & (SR_BCIS | SR_LVBCI))) return;         /* not ours             */
    a->completed++;
    a->irq_seen = 1;
    outw(a->nabm + PO_SR, sr & (SR_BCIS | SR_LVBCI)); /* RWC: write to clear  */
}

static void ac97_drain(struct audio_dev* dev) {
    struct ac97* a = (struct ac97*)dev->priv;
    if (!a->running) return;
    /* Bounded by what is actually queued plus slack, so a device that stops
     * answering costs a wait rather than a hung shell. */
    uint64_t deadline = timer_ticks_ms() +
                        (AC97_DEPTH * AC97_BUF_FRAMES * 1000ull) /
                        (dev->rate ? dev->rate : 48000u) + AC97_DRAIN_GRACE_MS;
    while (ac97_outstanding(a) || (!a->irq_seen && !(inw(a->nabm + PO_SR) & SR_DCH))) {
        if (timer_ticks_ms() > deadline) {
            kprintf("ac97: drain timeout\n");
            break;
        }
        task_msleep(1);
    }
    /* THE SETTLE IS THE POLLED PATH'S CRUTCH.  `DCH` says the DMA engine
     * halted, which is not the same as the last samples having left — so
     * without a better signal the driver had to wait a measured constant, and
     * a constant tuned against one emulator is exactly the kind of number that
     * is wrong on the next machine.  With the interrupt the count is exact and
     * the crutch is not used. */
    if (!a->irq_seen) task_msleep(AC97_SETTLE_MS);

    /* DO NOT CLEAR THE RUN BIT HERE.  Every buffer we queued has been
     * completed, so the engine halts by itself once it reaches the last one —
     * and stopping it by hand at that moment cuts whatever the codec has not
     * yet emitted, which measured as a few milliseconds off the end.  There is
     * nothing to gain by it either: the next sound calls ac97_engine_reset(),
     * which resets the PCM box properly before queueing anything. */
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
    a->submitted++;
    outb(a->nabm + PO_LVI, (uint8_t)slot);

    if (!a->running) {
        outw(a->nabm + PO_SR, 0x1C);
        outb(a->nabm + PO_CR, CR_RPBM | CR_IOCE | CR_LVBIE);
        a->running = 1;
    }
    return (int)nframes;                              /* QUEUED, not yet heard */
}

/* ----------------------- Capture ------------------------------------------ */

/* Arm the input engine with the whole ring, so the device always has somewhere
 * to put samples.  Unlike playback we do not feed it buffer by buffer: a
 * recorder that runs out of somewhere-to-write DROPS audio, and the drop is
 * silent — the samples simply never existed. */
static void ac97_rec_start(struct ac97* a) {
    outb(a->nabm + PI_CR, 0);
    outb(a->nabm + PI_CR, CR_RR);
    for (uint32_t s = 0; (inb(a->nabm + PI_CR) & CR_RR) && s < 100000; s++) hal_cpu_pause();

    for (uint32_t i = 0; i < AC97_NBUF; i++) {
        a->rec_bdl[i].addr    = a->rec_pcm_phys + i * AC97_BUF_FRAMES * 4;
        a->rec_bdl[i].samples = (uint16_t)(AC97_BUF_FRAMES * 2);
        a->rec_bdl[i].control = BDL_IOC;
    }
    outw(a->nabm + PI_SR, 0x1C);
    outl(a->nabm + PI_BDBAR, a->rec_bdl_phys);
    /* THE READER PACES THE DEVICE.  Handing the engine the whole ring lets it
     * run a full 32 buffers — ~680 ms — ahead of whoever is reading, and then
     * a `rec` drains that backlog faster than real time: 1000 ms of frames
     * arrived in 804.  Worse, once it laps the reader the samples being copied
     * out have already been overwritten, so the count stays right while the
     * audio is wrong.  A window of AC97_REC_DEPTH buffers keeps the device
     * just ahead of the reader and no further. */
    outb(a->nabm + PI_LVI, (uint8_t)(AC97_REC_DEPTH - 1));
    outb(a->nabm + PI_CR, CR_RPBM);
    a->rec_next = 0;
    a->rec_running = 1;
}

/* Begin a fresh session: re-arming the box drops whatever was collected while
 * nobody was listening, so a recording starts at the moment it was asked for. */
static void ac97_record_start(struct audio_dev* dev) {
    struct ac97* a = (struct ac97*)dev->priv;
    if (a->rec_pcm) ac97_rec_start(a);
}

static int ac97_record(struct audio_dev* dev, int16_t* frames, uint32_t nframes) {
    struct ac97* a = (struct ac97*)dev->priv;
    if (!a->rec_pcm) return -1;
    if (nframes > AC97_BUF_FRAMES) nframes = AC97_BUF_FRAMES;
    if (nframes == 0) return 0;
    if (!a->rec_running) ac97_rec_start(a);

    /* Wait until the engine has moved PAST the slot we want to read, which is
     * what says that slot is full.  Sleeping, bounded — a device that captures
     * nothing must cost a wait, not a hung shell. */
    uint64_t deadline = timer_ticks_ms() +
                        (AC97_BUF_FRAMES * 1000ull) /
                        (dev->rate ? dev->rate : 48000u) + AC97_DRAIN_GRACE_MS;
    while (((uint32_t)inb(a->nabm + PI_CIV) & (AC97_NBUF - 1)) == a->rec_next) {
        if (timer_ticks_ms() > deadline) return 0;     /* nothing captured    */
        task_msleep(1);
    }

    const int16_t* src = a->rec_pcm + (size_t)a->rec_next * AC97_BUF_FRAMES * 2;
    for (uint32_t i = 0; i < nframes * 2; i++) frames[i] = src[i];

    /* Hand the slot back and move the window on by one, so the engine gains
     * exactly the buffer we just freed — never more. */
    a->rec_bdl[a->rec_next].samples = (uint16_t)(AC97_BUF_FRAMES * 2);
    a->rec_bdl[a->rec_next].control = BDL_IOC;
    a->rec_next = (a->rec_next + 1) & (AC97_NBUF - 1);
    outb(a->nabm + PI_LVI,
         (uint8_t)((a->rec_next + AC97_REC_DEPTH - 1) & (AC97_NBUF - 1)));
    return (int)nframes;
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
    outw(nam + NAM_PCM_ADC_RATE, AC97_RATE);           /* capture rate too    */

    /* DMA memory: one frame for the BDL, contiguous frames for the PCM
     * buffer.  PMM-backed → phys == virt in the identity map. */
    g_ac97.rec_bdl_phys = pmm_alloc_frame_dma32();
    g_ac97.rec_pcm_phys = pmm_alloc_contiguous_dma32(32);    /* 128 KB capture */
    g_ac97.bdl_phys = pmm_alloc_frame_dma32();
    g_ac97.pcm_phys = pmm_alloc_contiguous_dma32(32);        /* 128 KB               */
    if (!g_ac97.bdl_phys || !g_ac97.pcm_phys) { kprintf("ac97: DMA OOM\n"); return -3; }
    g_ac97.bdl = (struct bdl_entry*)(uintptr_t)g_ac97.bdl_phys;
    if (g_ac97.rec_bdl_phys && g_ac97.rec_pcm_phys) {
        g_ac97.rec_bdl = (struct bdl_entry*)(uintptr_t)g_ac97.rec_bdl_phys;
        g_ac97.rec_pcm = (int16_t*)(uintptr_t)g_ac97.rec_pcm_phys;
    } else {
        /* Capture is optional: a box short of DMA memory should still PLAY. */
        kprintf("ac97: no DMA for capture — recording unavailable\n");
    }
    g_ac97.pcm = (int16_t*)(uintptr_t)g_ac97.pcm_phys;
    for (int i = 0; i < BDL_ENTRIES; i++) { g_ac97.bdl[i].addr = 0; g_ac97.bdl[i].samples = 0; g_ac97.bdl[i].control = 0; }

    /* The completion interrupt.  NOTE the tree's own warning: `irq_install`
     * does not CHAIN, so two devices sharing a line overwrite each other —
     * the line is logged so a collision is visible rather than mysterious.
     * If no line is routed, or it never fires, the polled path stays in use
     * (§M55: a driver learns its interrupt works by receiving one). */
    if (pd.irq_line != 0xFF) {
        irq_install(pd.irq_line, ac97_irq);
        kprintf("ac97: completion IRQ on line %u\n", pd.irq_line);
    } else {
        kprintf("ac97: no IRQ line — completion stays polled\n");
    }

    /* Register the abstract audio device. */
    g_audio.name     = "ac97";
    g_audio.rate     = AC97_RATE;
    g_audio.channels = 2;
    g_audio.play     = ac97_play;
    g_audio.drain    = ac97_drain;
    if (g_ac97.rec_pcm) {
        g_audio.record       = ac97_record;
        g_audio.record_start = ac97_record_start;
    }
    /* This driver QUEUES, so it can afford a short period — and says so rather
     * than letting the core assume the conservative one a non-queueing device
     * needs.  1024 frames = ~21 ms of mixer latency. */
    g_audio.period_frames = 1024;
    g_audio.priv     = &g_ac97;
    audio_register(&g_audio);

    /* READ THE RATES BACK.  A codec CLAMPS what it cannot do, so believing the
     * write leaves the driver assuming a rate the hardware is not using — and
     * capture is measurably running ~20 %% fast, which is exactly what a rate
     * mismatch looks like from outside.  §M61 made the same argument about
     * mode setting: read it back, do not believe the write. */
    kprintf("ac97: ext_audio=%x ext_ctrl=%x dac_rate=%u adc_rate=%u\n",
            inw(nam + NAM_EXT_AUDIO), inw(nam + NAM_EXT_CTRL),
            inw(nam + NAM_PCM_DAC_RATE), inw(nam + NAM_PCM_ADC_RATE));

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
