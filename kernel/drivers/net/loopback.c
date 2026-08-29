/* =============================================================================
 * loopback.c — the `lo` network device (§M24.8).
 *
 * WHY A LOOPBACK IS THE FIRST THING §M24's second half needed.
 *
 * Everything this milestone adds — several TCP connections at once, a LISTEN
 * socket, retransmission — is only observable when two endpoints talk.  Until
 * now the only peer this kernel could reach was QEMU's SLIRP gateway, which
 * means every network test depended on the host's network being up, on SLIRP
 * behaving, and on an external server answering; and none of them could ever
 * exercise the SERVER half, because SLIRP will not open a connection INTO the
 * guest without a hostfwd rule the automated runs do not have.  A loopback
 * makes both endpoints ours: deterministic, external-dependency-free, and
 * available on an arch with no NIC driver at all (aarch64 today), which is
 * precisely where a stack regression would otherwise go unnoticed.
 *
 * It is a real device, not a shortcut through the stack.  A frame handed to
 * `transmit` goes through the full IPv4/TCP output path, sits in a queue, and
 * comes back in through `net_rx` exactly as a NIC's would — so what the tests
 * exercise is the code that runs on the wire, not a bypass of it.
 *
 * WHY THE QUEUE.  transmit() runs with the STACK LOCK HELD (net.c's header),
 * and net_rx() must be called with it held too, so the naive implementation —
 * transmit calls net_rx directly — type-checks and is a stack overflow waiting
 * for a conversation: an arriving segment generates an ACK, whose transmit
 * re-enters net_rx, which may generate another segment, all on one C stack with
 * a lock held on every frame.  Queueing breaks the recursion: transmit only
 * appends, and the stack's own poller task drains the queue in poll(), which is
 * where every other driver delivers from.  The result is that loopback traffic
 * is scheduled exactly like real traffic instead of running to completion
 * inside somebody else's send.
 *
 * DROP INJECTION.  `lo drop <permille>` (net.lo.drop) makes transmit discard a
 * pseudo-random share of frames.  This is a test instrument, and a necessary
 * one: a retransmission timer that is never given a loss to recover from is a
 * feature nothing in the build can falsify — §M57's lesson, applied before the
 * fact rather than after it.
 * ============================================================================= */

#include "net.h"
#include "driver.h"
#include "hwdev.h"
#include "printf.h"
#include <stdint.h>
#include <stddef.h>

/* Queue depth.  Sized for a burst of a full TCP window's worth of segments
 * plus the ACKs coming the other way; a full queue DROPS, which is legitimate
 * behaviour for a link and is exactly what the retransmit path exists to
 * survive. */
#define LO_QSLOTS   64

struct lo_frame {
    uint32_t len;
    uint8_t  data[ETH_FRAME_MAX];
};

static struct lo_frame g_q[LO_QSLOTS];
static uint32_t        g_head, g_tail;          /* head = produce, tail = drain */
static uint32_t        g_dropped;               /* queue-full drops             */
static uint32_t        g_injected;              /* deliberate test drops        */
static uint32_t        g_drop_permille;         /* 0 = a perfect link           */
static uint32_t        g_rand = 0x12345678u;

static struct net_device g_lo;

/* A tiny LCG.  Deliberately NOT the kernel CSPRNG (§M39): a test instrument
 * should be reproducible from boot to boot, and drawing from the entropy pool
 * would make a failing run impossible to repeat. */
static uint32_t lo_rand(void) {
    g_rand = g_rand * 1103515245u + 12345u;
    return (g_rand >> 16) & 0x7FFF;
}

/* Called with the stack lock held (see the file header). */
static int lo_transmit(struct net_device* dev, const void* frame, uint32_t len) {
    if (len > ETH_FRAME_MAX) return -1;

    if (g_drop_permille && (lo_rand() % 1000u) < g_drop_permille) {
        g_injected++;
        dev->tx_packets++; dev->tx_bytes += len;   /* the sender saw it leave   */
        return 0;                                  /* ...and the link ate it    */
    }

    uint32_t nx = (g_head + 1) % LO_QSLOTS;
    if (nx == g_tail) { g_dropped++; return -1; }  /* queue full → a lost frame */

    const uint8_t* p = (const uint8_t*)frame;
    for (uint32_t i = 0; i < len; i++) g_q[g_head].data[i] = p[i];
    g_q[g_head].len = len;
    g_head = nx;

    dev->tx_packets++; dev->tx_bytes += len;
    return 0;
}

/* Called from the stack's pump with the lock held.
 *
 * Drains a BOUNDED number of frames: net_rx can enqueue more (an ACK for the
 * segment just delivered), so an "until empty" loop would keep the lock and
 * interrupts for as long as the two endpoints kept talking.  Whatever is left
 * is picked up by the next pump, which the poller comes straight back for
 * because this one delivered frames. */
static void lo_poll(struct net_device* dev) {
    uint32_t budget = LO_QSLOTS;
    while (g_tail != g_head && budget--) {
        struct lo_frame* f = &g_q[g_tail];
        g_tail = (g_tail + 1) % LO_QSLOTS;
        net_rx(dev, f->data, f->len);
    }
}

void loopback_set_drop(uint32_t permille) {
    g_drop_permille = permille > 1000 ? 1000 : permille;
}

void loopback_stats(uint32_t* drop_permille, uint32_t* injected, uint32_t* qfull) {
    if (drop_permille) *drop_permille = g_drop_permille;
    if (injected)      *injected      = g_injected;
    if (qfull)         *qfull         = g_dropped;
}

/* probe: 0 = present (driver.h's convention — a return value, not a boolean).
 * A loopback needs no hardware, so it is always there. */
static int lo_probe(void* ctx) { (void)ctx; return 0; }

static int lo_init(void* ctx) {
    (void)ctx;
    g_lo.name    = "lo";
    g_lo.flags   = NETDEV_F_LOOPBACK;
    /* All-zero MAC.  A loopback frame never reaches a wire, so the address is
     * arbitrary — but it must be CONSISTENT, because net_rx accepts a frame
     * only if its destination matches the receiving device's own MAC. */
    for (int i = 0; i < ETH_ALEN; i++) g_lo.mac[i] = 0;
    g_lo.ip      = IPV4(127, 0, 0, 1);
    g_lo.netmask = IPV4(255, 0, 0, 0);
    g_lo.gateway = 0;                            /* never a route to anywhere  */
    /* The same MTU as Ethernet, not Linux's 65536.  A larger one would mean a
     * segment size that only ever works on loopback, so every test that passed
     * here would be testing a path the NIC never takes. */
    g_lo.mtu      = ETH_MTU;
    g_lo.transmit = lo_transmit;
    g_lo.poll     = lo_poll;
    net_register(&g_lo);
    return 0;
}

/* §M67 — a real shutdown hook, and it is REQUIRED rather than polite.
 *
 * §M66 already refused to STOP a driver that has none: without one there is no
 * way to withdraw its registrations, so stopping it would leave the registry
 * pointing at a device nobody is driving.  A module raises the stakes — its
 * descriptor lives in memory `rmmod` frees — so the loader refuses to LOAD a
 * driver with no shutdown at all.  Loading code that can never be removed is a
 * leak by construction, and this file was the first place that bit. */
static int lo_shutdown(void* ctx) {
    (void)ctx;
    if (!g_lo.name) return 0;                 /* never came up */
    net_unregister(&g_lo);
    return 0;
}

static const struct driver_ops lo_ops = {
    .probe    = lo_probe,
    .init     = lo_init,
    .shutdown = lo_shutdown,
};

/* Built in, or loaded — see the same construct in hda.c for the argument.  The
 * loopback device is the module this tree ships on EVERY arch: `hda` is a PCI
 * card and aarch64 has no slot for one, so without a portable module the
 * loader would be an x86 feature with untested relocation code on the third
 * arch — the one-arch-only shape this project keeps paying for. */
/* WHAT THIS HARDWARE IS, declared OUTSIDE the packaging choice.
 *
 * It sat in the `#else` branch, so only the BUILT-IN build carried it and the
 * module — which is the form this driver actually ships in — declared nothing.
 * §M67's rule is that a module is the same source as its built-in form and
 * ONLY THE REGISTRATION DIFFERS; a hardware declaration is not registration,
 * it is a fact about the device, and it is the same fact either way. */
DRIVER_MATCH(m_lo) = { .driver = "loopback",
                       .name = "loopback network interface" };

#ifdef DOS_MODULE_BUILD
#include "module_abi.h"

static struct driver lo_driver = {
    .name    = "loopback",
    .class   = "net",
    .ops     = &lo_ops,
    .ctx     = NULL,
    .version = DOS_VERSION,
    .domains = DOMAIN_KERNEL,
    .flags   = 0,              /* no hardware at all: nothing to DMA with */
};

DOS_MODULE("loopback", &lo_driver, NULL, NULL);
#else
DRIVER(loopback, "net", &lo_ops, NULL);
#endif
