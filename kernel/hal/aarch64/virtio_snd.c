/* =============================================================================
 * virtio_snd.c — virtio-sound over virtio-MMIO (aarch64, §M23 stage 2 tail).
 *
 * WHY THIS DRIVER EXISTS.  §M23 shipped AC97, which is a PCI device: it works
 * on both x86 arches and there is no such card on QEMU's `-M virt`.  So this
 * architecture had the audio CORE, the `lsaudio`/`tone`/`play` commands and no
 * audio DEVICE at all — `lsaudio` printed nothing, and the honest reading of
 * that is "sound is an x86 feature here".  virtio-sound is the ARM answer for
 * the same reason virtio-gpu, virtio-input and virtio-net were: on `-M virt`
 * the way a device arrives is a virtio-MMIO slot.
 *
 * --------------------------------------------------------------------------
 * The device (virtio spec, device type 25)
 * --------------------------------------------------------------------------
 * Four virtqueues, in this order:  0 control, 1 event, 2 tx, 3 rx.
 * We drive CONTROL (to configure a stream) and TX (to feed it samples); the
 * event queue carries jack/stream notifications we do not act on, and RX is
 * capture, which is out of §M23's scope.  All four are still made READY,
 * because a device is entitled to expect the queues it advertises to exist.
 *
 * Playing a sound is a conversation, not a register write:
 *
 *     PCM_INFO      "describe your streams"      -> pick an OUTPUT one
 *     PCM_SET_PARAMS  channels / format / rate / buffer + period size
 *     PCM_PREPARE   allocate the stream
 *     PCM_START     begin consuming
 *     (tx buffers)  each: [xfer header][PCM frames][status written back]
 *     PCM_STOP / PCM_RELEASE
 *
 * THE TX MESSAGE LAYOUT IS THE PART THAT BITES: one message is a THREE-part
 * descriptor chain — a device-READABLE header carrying the stream id, the
 * device-READABLE PCM payload, and a device-WRITABLE status.  Putting the
 * status in the same descriptor as the header (or forgetting VRING_DESC_F_WRITE
 * on it) makes the device reject the buffer, and a rejected buffer is silence
 * with no error anywhere.
 *
 * --------------------------------------------------------------------------
 * A note on duplication, with a trigger rather than a complaint
 * --------------------------------------------------------------------------
 * This is the FIFTH copy of the virtio-MMIO transport in this tree
 * (virtio_mmio_blk.c, virtio_input.c, virtio_mmio_net.c, virtio_gpu.c, here) —
 * the same magic/version/status handshake, the same feature negotiation, the
 * same descriptor-ring setup, written out five times.  It is copied again here
 * deliberately rather than refactored in passing: extracting a shared
 * `virtio_mmio.c` means touching four WORKING drivers, one of which is the
 * display this arch boots on, and a bring-up is the wrong moment to do that.
 * The trigger for actually doing it is the SIXTH device, or the first bug that
 * has to be fixed in more than one of these files at once.
 * ============================================================================= */

#include "audio.h"
#include "printf.h"
#include "klog.h"
#include "task.h"
#include "timer.h"
#include <stdint.h>
#include <stddef.h>

/* ---- MMIO transport (shared layout with the other four) -------------------- */
#define VIRTIO_MMIO_BASE    0x0a000000UL
#define VIRTIO_MMIO_STRIDE  0x200
#define VIRTIO_MMIO_SLOTS   32

#define R_MAGIC 0x000
#define R_VERSION 0x004
#define R_DEVICEID 0x008
#define R_DEVFEAT 0x010
#define R_DEVFEATSEL 0x014
#define R_DRVFEAT 0x020
#define R_DRVFEATSEL 0x024
#define R_QUEUESEL 0x030
#define R_QUEUENUMMAX 0x034
#define R_QUEUENUM 0x038
#define R_QUEUEREADY 0x044
#define R_QUEUENOTIFY 0x050
#define R_INTSTATUS 0x060
#define R_INTACK 0x064
#define R_STATUS 0x070
#define R_QDESC_LO 0x080
#define R_QDESC_HI 0x084
#define R_QDRV_LO 0x090
#define R_QDRV_HI 0x094
#define R_QDEV_LO 0x0a0
#define R_QDEV_HI 0x0a4
#define R_CONFIG  0x100   /* device config space: jacks, streams, chmaps */

#define ST_ACK 1
#define ST_DRIVER 2
#define ST_DRIVER_OK 4
#define ST_FEATURES_OK 8

#define VIRTIO_MAGIC 0x74726976u
#define VIRTIO_DEVID_SOUND 25
#define VIRTIO_F_VERSION_1_BIT 0

#define QSIZE 8                      /* control/tx rings: we use one at a time */

struct virtq_desc  { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; } __attribute__((packed));
struct virtq_avail { uint16_t flags; uint16_t idx; uint16_t ring[QSIZE]; uint16_t used_event; } __attribute__((packed));
struct virtq_used_elem { uint32_t id; uint32_t len; } __attribute__((packed));
struct virtq_used  { uint16_t flags; uint16_t idx; struct virtq_used_elem ring[QSIZE]; uint16_t avail_event; } __attribute__((packed));

#define VRING_DESC_F_NEXT  0x01
#define VRING_DESC_F_WRITE 0x02

/* ---- virtio-sound protocol -------------------------------------------------- */
#define VIRTIO_SND_R_PCM_INFO       0x0100
#define VIRTIO_SND_R_PCM_SET_PARAMS 0x0101
#define VIRTIO_SND_R_PCM_PREPARE    0x0102
#define VIRTIO_SND_R_PCM_RELEASE    0x0103
#define VIRTIO_SND_R_PCM_START      0x0104
#define VIRTIO_SND_R_PCM_STOP       0x0105

#define VIRTIO_SND_S_OK             0x8000

#define VIRTIO_SND_D_OUTPUT         0
#define VIRTIO_SND_PCM_FMT_S16      5
#define VIRTIO_SND_PCM_RATE_48000   7

struct snd_hdr { uint32_t code; } __attribute__((packed));

struct snd_query_info {
    uint32_t code;
    uint32_t start_id;
    uint32_t count;
    uint32_t size;
} __attribute__((packed));

struct snd_pcm_info {
    uint32_t hda_fn_nid;
    uint32_t features;
    uint64_t formats;
    uint64_t rates;
    uint8_t  direction;
    uint8_t  channels_min;
    uint8_t  channels_max;
    uint8_t  padding[5];
} __attribute__((packed));

struct snd_pcm_set_params {
    uint32_t code;
    uint32_t stream_id;
    uint32_t buffer_bytes;
    uint32_t period_bytes;
    uint32_t features;
    uint8_t  channels;
    uint8_t  format;
    uint8_t  rate;
    uint8_t  padding;
} __attribute__((packed));

struct snd_pcm_hdr {
    uint32_t code;
    uint32_t stream_id;
} __attribute__((packed));

struct snd_pcm_xfer  { uint32_t stream_id; } __attribute__((packed));
struct snd_pcm_status { uint32_t status; uint32_t latency_bytes; } __attribute__((packed));

/* ---- sizing ----------------------------------------------------------------
 * One period is what a single `play` call hands the device.  16 KB = 4096
 * stereo frames = ~85 ms at 48 kHz: long enough that the millisecond-granular
 * completion wait cannot make the gap between periods audible, short enough
 * that a `play` caller is not blocked for a noticeable time in one call. */
#define SND_PERIOD_FRAMES 4096
#define SND_PERIOD_BYTES  (SND_PERIOD_FRAMES * 4)
#define SND_BUFFER_BYTES  (SND_PERIOD_BYTES * 2)

struct snd_dev {
    uintptr_t base;
    int       ready;
    uint32_t  stream_id;

    /* Control queue (0). */
    struct virtq_desc  cdesc[QSIZE] __attribute__((aligned(16)));
    struct virtq_avail cavail       __attribute__((aligned(16)));
    struct virtq_used  cused        __attribute__((aligned(16)));
    uint16_t clast;

    /* TX queue (2). */
    struct virtq_desc  tdesc[QSIZE] __attribute__((aligned(16)));
    struct virtq_avail tavail       __attribute__((aligned(16)));
    struct virtq_used  tused        __attribute__((aligned(16)));
    uint16_t tlast;

    /* Bounce buffers.  Static because they must be physically contiguous and
     * live for the device's lifetime; the identity map makes virt == phys. */
    uint8_t  req[64]   __attribute__((aligned(16)));
    uint8_t  resp[512] __attribute__((aligned(16)));
    struct snd_pcm_xfer   xfer   __attribute__((aligned(16)));
    struct snd_pcm_status status __attribute__((aligned(16)));
    int16_t  pcm[SND_PERIOD_FRAMES * 2] __attribute__((aligned(16)));
};

static struct snd_dev   g_snd;
static struct audio_dev g_audio;

static inline void     w32(uintptr_t b, uint32_t off, uint32_t v) { *(volatile uint32_t*)(b + off) = v; }
static inline uint32_t r32(uintptr_t b, uint32_t off)             { return *(volatile uint32_t*)(b + off); }
static inline void dsb(void) { __asm__ volatile ("dsb sy" ::: "memory"); }

/* Wait for a used-ring entry, SLEEPING rather than spinning — the same rule the
 * AC97 drain wait had to learn: a wait must not cost a CPU.  The deadline is
 * real time, so a device that never answers costs a bounded wait instead of a
 * hung shell. */
static int wait_used(const struct virtq_used* used, uint16_t* last, uint32_t ms) {
    uint64_t deadline = timer_ticks_ms() + ms;
    /* Read the index THROUGH the struct rather than through a pointer to the
     * packed member: `virtq_used` is packed, so a `uint16_t*` into it is not a
     * pointer the compiler can assume is aligned — true on this arch today and
     * exactly the kind of assumption an arch port later trips over. */
    while (*last == *(const volatile uint16_t*)&used->idx) {
        if (timer_ticks_ms() > deadline) return -1;
        task_msleep(1);
    }
    dsb();
    (*last)++;
    return 0;
}

/* ---- control-queue request -------------------------------------------------
 * Two descriptors: the request (device-readable) and the response buffer
 * (device-WRITABLE).  Returns the device's status code, or negative on a
 * transport failure. */
static int ctrl_request(struct snd_dev* d, const void* req, uint32_t reqlen,
                        uint32_t resplen) {
    const uint8_t* s = (const uint8_t*)req;
    for (uint32_t i = 0; i < reqlen && i < sizeof d->req; i++) d->req[i] = s[i];
    for (uint32_t i = 0; i < resplen && i < sizeof d->resp; i++) d->resp[i] = 0;

    d->cdesc[0].addr  = (uint64_t)(uintptr_t)d->req;
    d->cdesc[0].len   = reqlen;
    d->cdesc[0].flags = VRING_DESC_F_NEXT;
    d->cdesc[0].next  = 1;
    d->cdesc[1].addr  = (uint64_t)(uintptr_t)d->resp;
    d->cdesc[1].len   = resplen;
    d->cdesc[1].flags = VRING_DESC_F_WRITE;
    d->cdesc[1].next  = 0;

    uint16_t ai = d->cavail.idx;
    d->cavail.ring[ai % QSIZE] = 0;
    dsb();
    d->cavail.idx = ai + 1;
    dsb();
    w32(d->base, R_QUEUENOTIFY, 0);

    if (wait_used(&d->cused, &d->clast, 2000) != 0) {
        klog(KLOG_WARN, "audio", "virtio-snd: control request timed out\n");
        return -1;
    }
    if (r32(d->base, R_INTSTATUS) & 1) w32(d->base, R_INTACK, 1);
    return (int)(*(volatile uint32_t*)d->resp);
}

static int ctrl_simple(struct snd_dev* d, uint32_t code) {
    struct snd_pcm_hdr h = { code, d->stream_id };
    int st = ctrl_request(d, &h, sizeof h, sizeof(struct snd_hdr));
    return (st == VIRTIO_SND_S_OK) ? 0 : -1;
}

/* ---- playback --------------------------------------------------------------- */

static int snd_play(struct audio_dev* dev, const int16_t* frames, uint32_t nframes) {
    struct snd_dev* d = (struct snd_dev*)dev->priv;
    if (!d->ready) return -1;
    if (nframes > SND_PERIOD_FRAMES) nframes = SND_PERIOD_FRAMES;
    if (nframes == 0) return 0;

    for (uint32_t i = 0; i < nframes * 2; i++) d->pcm[i] = frames[i];
    d->xfer.stream_id     = d->stream_id;
    d->status.status      = 0;
    d->status.latency_bytes = 0;

    /* THREE descriptors, and the split is not cosmetic: the header and the
     * payload are what the DEVICE reads, the status is what it WRITES back.
     * A status descriptor without VRING_DESC_F_WRITE is rejected, and a
     * rejected buffer is silence with nothing logged anywhere. */
    d->tdesc[0].addr  = (uint64_t)(uintptr_t)&d->xfer;
    d->tdesc[0].len   = sizeof d->xfer;
    d->tdesc[0].flags = VRING_DESC_F_NEXT;
    d->tdesc[0].next  = 1;
    d->tdesc[1].addr  = (uint64_t)(uintptr_t)d->pcm;
    d->tdesc[1].len   = nframes * 4;
    d->tdesc[1].flags = VRING_DESC_F_NEXT;
    d->tdesc[1].next  = 2;
    d->tdesc[2].addr  = (uint64_t)(uintptr_t)&d->status;
    d->tdesc[2].len   = sizeof d->status;
    d->tdesc[2].flags = VRING_DESC_F_WRITE;
    d->tdesc[2].next  = 0;

    uint16_t ai = d->tavail.idx;
    d->tavail.ring[ai % QSIZE] = 0;
    dsb();
    d->tavail.idx = ai + 1;
    dsb();
    w32(d->base, R_QUEUENOTIFY, 2);

    /* The device completes the buffer once it has been PLAYED, so this wait is
     * the real-time pacing — the buffer's own duration plus slack, exactly as
     * the AC97 drain does. */
    uint32_t ms = (nframes * 1000u) / (dev->rate ? dev->rate : 48000u);
    if (wait_used(&d->tused, &d->tlast, ms + 500) != 0) {
        klog(KLOG_WARN, "audio", "virtio-snd: playback timed out\n");
        return -1;
    }
    if (r32(d->base, R_INTSTATUS) & 1) w32(d->base, R_INTACK, 1);
    return (int)nframes;
}

/* ---- bring-up ---------------------------------------------------------------- */

static int queue_setup(struct snd_dev* d, uint32_t q, struct virtq_desc* desc,
                       struct virtq_avail* avail, struct virtq_used* used) {
    w32(d->base, R_QUEUESEL, q);
    if (r32(d->base, R_QUEUEREADY) != 0) return -1;
    uint32_t nmax = r32(d->base, R_QUEUENUMMAX);
    if (nmax == 0) return -1;
    w32(d->base, R_QUEUENUM, (nmax < QSIZE) ? nmax : QSIZE);
    uint64_t dd = (uint64_t)(uintptr_t)desc;
    uint64_t aa = (uint64_t)(uintptr_t)avail;
    uint64_t uu = (uint64_t)(uintptr_t)used;
    w32(d->base, R_QDESC_LO, (uint32_t)dd); w32(d->base, R_QDESC_HI, (uint32_t)(dd >> 32));
    w32(d->base, R_QDRV_LO,  (uint32_t)aa); w32(d->base, R_QDRV_HI,  (uint32_t)(aa >> 32));
    w32(d->base, R_QDEV_LO,  (uint32_t)uu); w32(d->base, R_QDEV_HI,  (uint32_t)(uu >> 32));
    w32(d->base, R_QUEUEREADY, 1);
    return 0;
}

/* Ask the device to describe its streams and pick the first OUTPUT one.
 * Returns 0 on success.  A device with no output stream is REPORTED rather
 * than assumed away — "there is a sound card but nothing to play into" is a
 * real configuration (a capture-only device) and silence is not the answer. */
static int pick_output_stream(struct snd_dev* d) {
    /* ASK THE DEVICE HOW MANY STREAMS IT HAS — do not assume a number.
     *
     * The first version queried a fixed four and the device answered
     * VIRTIO_SND_S_BAD_MSG (0x8001): per the spec, a query whose
     * `start_id + count` runs past the available items is malformed, not
     * merely optimistic.  The count lives in the device's config space, which
     * is what config space is FOR, and the failure was a clean one only
     * because the status code was checked — an unchecked request would have
     * left the info buffer full of zeros and picked "stream 0, direction 0",
     * i.e. silently the right answer on this device and the wrong one on the
     * next. */
    uint32_t nstreams = r32(d->base, R_CONFIG + 4);      /* jacks, STREAMS, chmaps */
    if (nstreams == 0) {
        klog(KLOG_WARN, "audio", "virtio-snd: device reports no PCM streams\n");
        return -1;
    }
    uint32_t maxfit = (uint32_t)((sizeof d->resp - sizeof(struct snd_hdr)) /
                                 sizeof(struct snd_pcm_info));
    if (nstreams > maxfit) nstreams = maxfit;

    struct snd_query_info q = { VIRTIO_SND_R_PCM_INFO, 0, nstreams,
                                (uint32_t)sizeof(struct snd_pcm_info) };
    int st = ctrl_request(d, &q, sizeof q,
                          (uint32_t)(sizeof(struct snd_hdr) +
                                     nstreams * sizeof(struct snd_pcm_info)));
    if (st != VIRTIO_SND_S_OK) {
        klog(KLOG_WARN, "audio", "virtio-snd: PCM_INFO failed (0x%x)\n", (unsigned)st);
        return -1;
    }
    const struct snd_pcm_info* info =
        (const struct snd_pcm_info*)(d->resp + sizeof(struct snd_hdr));
    for (uint32_t i = 0; i < nstreams; i++) {
        if (info[i].direction != VIRTIO_SND_D_OUTPUT) continue;
        if (info[i].channels_max < 2) continue;    /* the core is stereo-only  */
        d->stream_id = i;
        return 0;
    }
    klog(KLOG_WARN, "audio", "virtio-snd: no stereo output stream\n");
    return -1;
}

static int snd_dev_init(struct snd_dev* d, uintptr_t base) {
    d->base = base;
    w32(base, R_STATUS, 0);
    w32(base, R_STATUS, ST_ACK);
    w32(base, R_STATUS, ST_ACK | ST_DRIVER);
    w32(base, R_DEVFEATSEL, 1); (void)r32(base, R_DEVFEAT);
    w32(base, R_DRVFEATSEL, 1); w32(base, R_DRVFEAT, 1u << VIRTIO_F_VERSION_1_BIT);
    w32(base, R_DEVFEATSEL, 0); (void)r32(base, R_DEVFEAT);
    w32(base, R_DRVFEATSEL, 0); w32(base, R_DRVFEAT, 0);
    w32(base, R_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK);
    if (!(r32(base, R_STATUS) & ST_FEATURES_OK)) return -1;

    /* Control (0) and TX (2) are the two we drive.  Event (1) and RX (3) are
     * left un-ready on purpose: this driver posts no buffers to either, and a
     * ready queue with no buffers is indistinguishable to the device from one
     * that is simply never serviced. */
    if (queue_setup(d, 0, d->cdesc, &d->cavail, &d->cused) != 0) return -1;
    if (queue_setup(d, 2, d->tdesc, &d->tavail, &d->tused) != 0) return -1;

    w32(base, R_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);
    d->clast = d->tlast = 0;

    if (pick_output_stream(d) != 0) return -1;

    struct snd_pcm_set_params p = {
        VIRTIO_SND_R_PCM_SET_PARAMS, d->stream_id,
        SND_BUFFER_BYTES, SND_PERIOD_BYTES, 0,
        2, VIRTIO_SND_PCM_FMT_S16, VIRTIO_SND_PCM_RATE_48000, 0
    };
    if (ctrl_request(d, &p, sizeof p, sizeof(struct snd_hdr)) != VIRTIO_SND_S_OK) {
        klog(KLOG_WARN, "audio", "virtio-snd: SET_PARAMS refused\n");
        return -1;
    }
    if (ctrl_simple(d, VIRTIO_SND_R_PCM_PREPARE) != 0) {
        klog(KLOG_WARN, "audio", "virtio-snd: PREPARE refused\n");
        return -1;
    }
    if (ctrl_simple(d, VIRTIO_SND_R_PCM_START) != 0) {
        klog(KLOG_WARN, "audio", "virtio-snd: START refused\n");
        return -1;
    }

    d->ready = 1;
    return 0;
}

/* Scan the MMIO slots for a virtio-sound device and register it with the
 * portable audio core.  Returns the number of devices found (0 or 1 — one
 * output stream is what §M23 is scoped to). */
int virtio_snd_init(void) {
    for (int i = 0; i < VIRTIO_MMIO_SLOTS; i++) {
        uintptr_t base = VIRTIO_MMIO_BASE + (uintptr_t)i * VIRTIO_MMIO_STRIDE;
        if (*(volatile uint32_t*)(base + R_MAGIC) != VIRTIO_MAGIC) continue;
        if (*(volatile uint32_t*)(base + R_VERSION) != 2) continue;
        if (*(volatile uint32_t*)(base + R_DEVICEID) != VIRTIO_DEVID_SOUND) continue;

        if (snd_dev_init(&g_snd, base) != 0) {
            kprintf("virtio-snd: device at slot %d failed to start\n", i);
            return 0;
        }
        g_audio.name     = "virtio-snd";
        g_audio.rate     = 48000;
        g_audio.channels = 2;
        g_audio.play     = snd_play;
        g_audio.priv     = &g_snd;
        audio_register(&g_audio);
        kprintf("virtio-snd: up at slot %d (base %p), stream %u\n",
                i, (void*)base, (unsigned)g_snd.stream_id);
        return 1;
    }
    return 0;
}
