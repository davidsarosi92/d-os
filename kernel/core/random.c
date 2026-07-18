/* =============================================================================
 * random.c — kernel CSPRNG + entropy source (§M39 stage 1).  See random.h.
 *
 * ChaCha20 keystream (RFC 8439) driven from a 256-bit key that we reseed with
 * "fast key erasure": after every request the key is overwritten with fresh
 * keystream that is NEVER output, so an attacker who later reads the state
 * cannot reconstruct past output (forward secrecy) — this is exactly how
 * Linux's get_random_bytes works.  Entropy (hardware RNG + timing jitter) is
 * folded into the key, never trusted blindly for the whole state.
 *
 * Portable C throughout; the sole arch hook is hal_hw_random() (RDRAND/RNDR),
 * which is a weak no-op on arches that lack it — the CSPRNG still runs, seeded
 * from boot/timing jitter (lower quality, honestly noted).
 * ============================================================================= */
#include "random.h"
#include "devfs.h"
#include "lock.h"
#include "timer.h"
#include "printf.h"
#include <stdint.h>
#include <stddef.h>

/* Weak default for the optional hardware RNG: 0 = "no hardware word available".
 * The x86 HAL provides a strong RDRAND-backed override (hal_arch.c). */
int hal_hw_random(uint32_t* out) __attribute__((weak));
int hal_hw_random(uint32_t* out) { (void)out; return 0; }

/* ---- ChaCha20 block function (RFC 8439) ---------------------------------- */
#define ROTL32(a, b) (((a) << (b)) | ((a) >> (32 - (b))))
#define QR(a, b, c, d)                          \
    do {                                        \
        a += b; d ^= a; d = ROTL32(d, 16);      \
        c += d; b ^= c; b = ROTL32(b, 12);      \
        a += b; d ^= a; d = ROTL32(d, 8);       \
        c += d; b ^= c; b = ROTL32(b, 7);       \
    } while (0)

static void chacha20_block(const uint32_t in[16], uint32_t out[16]) {
    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = in[i];
    for (int i = 0; i < 10; i++) {              /* 20 rounds = 10 double-rounds */
        QR(x[0], x[4], x[8],  x[12]);
        QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]);
        QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) out[i] = x[i] + in[i];
}

/* ---- CSPRNG state --------------------------------------------------------- */
static const uint32_t SIGMA[4] = {              /* "expand 32-byte k" */
    0x61707865, 0x3320646e, 0x79622d32, 0x6b206574
};
static uint32_t   g_key[8];                     /* the 256-bit secret key */
static uint64_t   g_counter;                    /* block counter / nonce  */
static int        g_seeded;
static spinlock_t g_lock = SPINLOCK_INIT;

/* Build the 16-word ChaCha input block from the current key + counter. */
static void load_input(uint32_t in[16]) {
    in[0] = SIGMA[0]; in[1] = SIGMA[1]; in[2] = SIGMA[2]; in[3] = SIGMA[3];
    for (int i = 0; i < 8; i++) in[4 + i] = g_key[i];
    in[12] = (uint32_t)g_counter;
    in[13] = (uint32_t)(g_counter >> 32);
    in[14] = 0;
    in[15] = 0;
    g_counter++;
}

/* Generate `n` bytes of keystream, then fast-key-erasure: replace the key with
 * one fresh (never-output) block.  Caller holds g_lock. */
static void generate_locked(uint8_t* out, size_t n) {
    uint32_t in[16], blk[16];
    while (n) {
        load_input(in);
        chacha20_block(in, blk);
        size_t take = n < 64 ? n : 64;
        const uint8_t* src = (const uint8_t*)blk;
        for (size_t i = 0; i < take; i++) out[i] = src[i];
        out += take; n -= take;
    }
    /* Fast key erasure: derive the next key from fresh keystream. */
    load_input(in);
    chacha20_block(in, blk);
    for (int i = 0; i < 8; i++) g_key[i] = blk[i];
}

/* Fold `n` bytes into the key by XOR-then-mix (a block step), so new entropy
 * can only add uncertainty, never remove it.  Caller holds g_lock. */
static void add_entropy_locked(const uint8_t* buf, size_t n) {
    uint8_t* kb = (uint8_t*)g_key;
    for (size_t i = 0; i < n; i++) kb[i % 32] ^= buf[i];
    uint32_t in[16], blk[16];
    load_input(in);
    chacha20_block(in, blk);
    for (int i = 0; i < 8; i++) g_key[i] ^= blk[i];   /* diffuse across the key */
}

/* ---- public API ----------------------------------------------------------- */

void random_add_entropy(const void* buf, size_t n) {
    uint32_t f = spin_lock_irqsave(&g_lock);
    add_entropy_locked((const uint8_t*)buf, n);
    spin_unlock_irqrestore(&g_lock, f);
}

void random_bytes(void* buf, size_t n) {
    uint32_t f = spin_lock_irqsave(&g_lock);
    if (!g_seeded) {
        /* Lazy fallback seed if used before random_init — better than zeros. */
        uint32_t t = timer_ticks_ms();
        add_entropy_locked((const uint8_t*)&t, sizeof t);
        g_seeded = 1;
    }
    generate_locked((uint8_t*)buf, n);
    spin_unlock_irqrestore(&g_lock, f);
}

uint32_t random_u32(void) {
    uint32_t v;
    random_bytes(&v, sizeof v);
    return v;
}

/* ---- /dev/urandom + /dev/random ------------------------------------------- */
/* Both draw from the same CSPRNG.  /dev/random does NOT block here (we have no
 * blocking entropy estimator yet); documented simplification — acceptable once
 * the pool is seeded from a hardware RNG at boot. */
static ssize_t urandom_read(void* ctx, void* buf, size_t n, uint64_t off) {
    (void)ctx; (void)off;
    random_bytes(buf, n);
    return (ssize_t)n;
}
static ssize_t rnd_write(void* ctx, const void* buf, size_t n, uint64_t off) {
    (void)ctx; (void)off;
    random_add_entropy(buf, n);      /* userspace may contribute entropy */
    return (ssize_t)n;
}

static struct devfs_node node_urandom = {
    .name = "urandom", .kind = DEVFS_CHAR,
    .read = urandom_read, .write = rnd_write, .ioctl = NULL, .ctx = NULL,
    ._next = NULL,
};
static struct devfs_node node_random = {
    .name = "random", .kind = DEVFS_CHAR,
    .read = urandom_read, .write = rnd_write, .ioctl = NULL, .ctx = NULL,
    ._next = NULL,
};

void random_init(void) {
    uint32_t f = spin_lock_irqsave(&g_lock);

    /* 1. Hardware RNG: pull as many words as we can into the key. */
    int hw = 0;
    for (int i = 0; i < 16; i++) {
        uint32_t w;
        if (hal_hw_random(&w)) { add_entropy_locked((const uint8_t*)&w, sizeof w); hw++; }
    }

    /* 2. Boot/timing jitter — weak, but adds per-boot variation even with no
     *    hardware RNG (addresses of stack objects + the current tick). */
    uintptr_t jitter[3] = {
        (uintptr_t)&node_urandom, (uintptr_t)&f, (uintptr_t)timer_ticks_ms()
    };
    add_entropy_locked((const uint8_t*)jitter, sizeof jitter);
    g_seeded = 1;
    spin_unlock_irqrestore(&g_lock, f);

    devfs_register(&node_urandom);
    devfs_register(&node_random);

    kprintf("random: CSPRNG seeded (%s), /dev/urandom + /dev/random ready\n",
            hw ? "hardware RNG + jitter" : "jitter only — no HW RNG");
}
