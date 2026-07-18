/* =============================================================================
 * random.h — kernel CSPRNG + entropy (§M39 stage 1).
 *
 * A ChaCha20-based cryptographically-secure PRNG (the construction Linux uses),
 * seeded from a hardware RNG where available (hal_hw_random → RDRAND on x86,
 * RNDR on aarch64) plus boot/timing jitter, and continuously reseedable.  This
 * is ARCH-GENERIC: the ChaCha20 core + pool are portable C; the only arch bit
 * is the optional hal_hw_random() seed source (a weak no-op elsewhere).
 *
 * Exposed to userland as /dev/urandom, /dev/random, and the getrandom syscall,
 * and used to fill the per-exec AT_RANDOM auxv.  This is the real entropy
 * primitive §M32 flagged as missing ("NOT production crypto until this lands").
 * ============================================================================= */
#ifndef RANDOM_H
#define RANDOM_H

#include <stdint.h>
#include <stddef.h>

/* Seed the CSPRNG (hardware RNG + boot jitter) and register /dev/urandom +
 * /dev/random.  Call once at boot, after devfs is up. */
void random_init(void);

/* Fill `buf` with `n` cryptographically-strong random bytes (never blocks). */
void random_bytes(void* buf, size_t n);

/* Convenience: one random 32-bit word. */
uint32_t random_u32(void);

/* Mix `n` bytes of (possibly low-quality) entropy into the pool — e.g. IRQ /
 * timer jitter.  Safe to call from interrupt context. */
void random_add_entropy(const void* buf, size_t n);

#endif
