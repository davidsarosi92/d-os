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
#define DMA_FRAMES       32000  /* matches the tone core's cap; ×4 = 128 KB   */

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
};

static struct ac97 g_ac97;
static struct audio_dev g_audio;

/* ----------------------- Playback ----------------------------------------- */

static int ac97_play(struct audio_dev* dev, const int16_t* frames, uint32_t nframes) {
    struct ac97* a = (struct ac97*)dev->priv;
    if (nframes > DMA_FRAMES) nframes = DMA_FRAMES;

    /* Copy the caller's PCM into our DMA buffer (phys == virt). */
    for (uint32_t i = 0; i < nframes * 2; i++) a->pcm[i] = frames[i];

    /* One BDL entry covers the whole buffer; IOC|BUP so the engine halts
     * cleanly at the end instead of underrunning into noise. */
    a->bdl[0].addr    = a->pcm_phys;
    a->bdl[0].samples = (uint16_t)(nframes * 2);      /* stereo → 2 per frame */
    a->bdl[0].control = BDL_IOC | BDL_BUP;

    outl(a->nabm + PO_BDBAR, a->bdl_phys);
    outb(a->nabm + PO_LVI, 0);                        /* last valid index = 0 */
    outw(a->nabm + PO_SR, 0x1C);                      /* clear RWC status bits */

    outb(a->nabm + PO_CR, CR_RPBM);                   /* run                   */

    /* Poll until the DMA controller halts (playback consumed the buffer at
     * real-time rate).  Bounded so a stuck device can't wedge the shell. */
    uint32_t spins = 0;
    while (!(inw(a->nabm + PO_SR) & SR_DCH)) {
        hal_cpu_pause();
        if (++spins > 2000000000u) { kprintf("ac97: playback timeout\n"); break; }
    }
    outb(a->nabm + PO_CR, 0);                          /* stop                  */
    return 0;
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
    g_ac97.bdl_phys = pmm_alloc_frame();
    g_ac97.pcm_phys = pmm_alloc_contiguous(32);        /* 128 KB               */
    if (!g_ac97.bdl_phys || !g_ac97.pcm_phys) { kprintf("ac97: DMA OOM\n"); return -3; }
    g_ac97.bdl = (struct bdl_entry*)(uintptr_t)g_ac97.bdl_phys;
    g_ac97.pcm = (int16_t*)(uintptr_t)g_ac97.pcm_phys;
    for (int i = 0; i < BDL_ENTRIES; i++) { g_ac97.bdl[i].addr = 0; g_ac97.bdl[i].samples = 0; g_ac97.bdl[i].control = 0; }

    /* Register the abstract audio device. */
    g_audio.name     = "ac97";
    g_audio.rate     = AC97_RATE;
    g_audio.channels = 2;
    g_audio.play     = ac97_play;
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
