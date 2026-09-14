/* =============================================================================
 * sha256.h — SHA-256, HMAC-SHA256 and PBKDF2 (§M32 stage 2).
 *
 * WHY THIS IS IN THE KERNEL AT ALL.  §M39 shipped a ChaCha20 CSPRNG and
 * `/dev/urandom`, so this machine has ENTROPY and therefore salts.  What it
 * has never had is a HASH: Mbed TLS is a ring-3 library, and a login check
 * runs in the kernel long before any ring-3 process exists.  So §M32's account
 * database needed exactly one primitive, and this is it.
 *
 * -----------------------------------------------------------------------------
 * THE VERIFICATION IS NOT OPTIONAL, AND THAT IS THE WHOLE POINT OF THIS FILE.
 *
 * A hash that is wrong in a way nobody notices is a password file anybody can
 * forge — and **a self-consistent implementation cannot detect its own error**.
 * Ours would hash, store, re-hash at login and agree with itself perfectly
 * while producing something that is not SHA-256 at all.  The only thing that
 * can tell us apart from that is an answer computed by somebody else.
 *
 * `kdftest` therefore runs PUBLISHED vectors, and it runs them before any
 * account depends on the result:
 *
 *   SHA-256          FIPS 180-2 appendix B — "abc", the 448-bit message, and
 *                    the one-million-'a' case that exercises the length
 *                    counter past a 32-bit byte count.
 *   HMAC-SHA256      RFC 4231 test cases 1 and 2 — case 2 matters because its
 *                    key is SHORTER than the block and must be zero-padded,
 *                    which is the half of HMAC an implementation gets wrong.
 *   PBKDF2-HMAC-256  RFC 7914 §11.
 *
 * -----------------------------------------------------------------------------
 * WHAT THIS IS NOT.
 *
 * PBKDF2 is a real KDF and a MEMORY-CHEAP one: it resists a CPU attacker in
 * proportion to its iteration count and barely inconveniences a GPU or an
 * ASIC.  scrypt and argon2 exist precisely because of that, and neither is
 * here.  Said plainly rather than left to be assumed, because "we hash
 * passwords" and "our password hashing is hard to attack" are different
 * claims, and §M33's whole argument is that the second must not be made on the
 * strength of the first.
 * ============================================================================= */

#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>
#include <stddef.h>

#define SHA256_DIGEST_LEN 32
#define SHA256_BLOCK_LEN  64

struct sha256_ctx {
    uint32_t h[8];
    uint64_t nbits;                     /* message length in BITS — see .c   */
    uint8_t  buf[SHA256_BLOCK_LEN];
    size_t   buflen;
};

void sha256_init(struct sha256_ctx* c);
void sha256_update(struct sha256_ctx* c, const void* data, size_t len);
void sha256_final(struct sha256_ctx* c, uint8_t out[SHA256_DIGEST_LEN]);

/* One-shot. */
void sha256(const void* data, size_t len, uint8_t out[SHA256_DIGEST_LEN]);

/* HMAC-SHA256 (RFC 2104).  A key longer than the block is hashed first; a
 * shorter one is zero-padded — both are in the implementation and both are
 * covered by the RFC 4231 cases in `kdftest`. */
void hmac_sha256(const void* key, size_t keylen,
                 const void* data, size_t datalen,
                 uint8_t out[SHA256_DIGEST_LEN]);

/* PBKDF2-HMAC-SHA256 (RFC 8018).  `iterations` must be >= 1; `dklen` may be
 * any length, including more than one block. */
void pbkdf2_sha256(const void* pass, size_t passlen,
                   const void* salt, size_t saltlen,
                   uint32_t iterations,
                   uint8_t* out, size_t dklen);

#endif /* SHA256_H */
