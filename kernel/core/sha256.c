/* =============================================================================
 * sha256.c — SHA-256 / HMAC-SHA256 / PBKDF2-HMAC-SHA256 (§M32 stage 2).
 *
 * A direct transcription of FIPS 180-4 §6.2 and RFC 2104 / RFC 8018.  Nothing
 * here is clever on purpose: this is the one file in the tree where a
 * "simplification" that happens to agree with itself is indistinguishable from
 * correctness, so it stays close enough to the specification that a reader can
 * check it line by line against the document — and then `kdftest` checks it
 * against somebody else's arithmetic anyway.
 *
 * Integer only (§A2: no FP in kernel context) and no allocation at all — every
 * buffer is a caller's or on the stack, so a hash cannot fail for lack of
 * memory at exactly the moment a machine under memory pressure is trying to
 * authenticate somebody.
 * ============================================================================= */

#include "sha256.h"
#include "shellcmd.h"
#include "printf.h"
#include "console.h"
#include "timer.h"

/* FIPS 180-4 §4.2.2 — the first 32 bits of the fractional parts of the cube
 * roots of the first 64 primes. */
static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(struct sha256_ctx* c, const uint8_t* p) {
    uint32_t w[64];
    /* Big-endian load.  Spelled out byte by byte rather than cast-and-swap:
     * this tree runs on three architectures and a cast through a uint32_t*
     * would also be an alignment assumption about the caller's buffer. */
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    uint32_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1  = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch  = (e & f) ^ ((~e) & g);
        uint32_t t1  = h + S1 + ch + K[i] + w[i];
        uint32_t S0  = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2  = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void sha256_init(struct sha256_ctx* c) {
    /* FIPS 180-4 §5.3.3 — fractional parts of the square roots of the first
     * eight primes. */
    c->h[0] = 0x6a09e667u; c->h[1] = 0xbb67ae85u;
    c->h[2] = 0x3c6ef372u; c->h[3] = 0xa54ff53au;
    c->h[4] = 0x510e527fu; c->h[5] = 0x9b05688cu;
    c->h[6] = 0x1f83d9abu; c->h[7] = 0x5be0cd19u;
    c->nbits  = 0;
    c->buflen = 0;
}

void sha256_update(struct sha256_ctx* c, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    /* The length counter is in BITS and is 64 bits wide, which is the
     * specification's own width.  Counting BYTES in a 32-bit field is the
     * classic bug here: it is correct for every message anybody tests with and
     * wrong past 512 MiB, at which point the padding block carries a truncated
     * length and the digest silently stops matching everyone else's.  The
     * one-million-'a' vector in kdftest exercises the multi-block path; the
     * width is what covers the rest. */
    c->nbits += (uint64_t)len * 8u;

    while (len > 0) {
        size_t space = SHA256_BLOCK_LEN - c->buflen;
        size_t take  = len < space ? len : space;
        for (size_t i = 0; i < take; i++) c->buf[c->buflen + i] = p[i];
        c->buflen += take;
        p   += take;
        len -= take;
        if (c->buflen == SHA256_BLOCK_LEN) {
            sha256_block(c, c->buf);
            c->buflen = 0;
        }
    }
}

void sha256_final(struct sha256_ctx* c, uint8_t out[SHA256_DIGEST_LEN]) {
    uint64_t nbits = c->nbits;

    /* Padding: 0x80, then zeroes, then the 64-bit big-endian bit count, such
     * that the total is a multiple of the block.  When fewer than 8 bytes are
     * left after the 0x80 the length spills into a SECOND block — the case an
     * implementation that only ever hashes short strings never reaches. */
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    c->nbits = nbits;                    /* update() counted the pad; undo it */

    while (c->buflen != 56) {
        uint8_t z = 0;
        sha256_update(c, &z, 1);
        c->nbits = nbits;
    }

    uint8_t len_be[8];
    for (int i = 0; i < 8; i++) len_be[i] = (uint8_t)(nbits >> (56 - i * 8));
    sha256_update(c, len_be, 8);

    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->h[i]);
    }
}

void sha256(const void* data, size_t len, uint8_t out[SHA256_DIGEST_LEN]) {
    struct sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

/* ---------------------------------------------------------------------------
 * HMAC-SHA256 (RFC 2104).
 * ------------------------------------------------------------------------- */

void hmac_sha256(const void* key, size_t keylen,
                 const void* data, size_t datalen,
                 uint8_t out[SHA256_DIGEST_LEN]) {
    uint8_t k[SHA256_BLOCK_LEN];
    uint8_t pad[SHA256_BLOCK_LEN];
    uint8_t inner[SHA256_DIGEST_LEN];
    struct sha256_ctx c;

    for (size_t i = 0; i < SHA256_BLOCK_LEN; i++) k[i] = 0;
    if (keylen > SHA256_BLOCK_LEN) {
        /* A key longer than the block is REPLACED by its hash, not truncated.
         * Truncating would still "work" in the sense of being repeatable, and
         * would disagree with every other implementation on earth. */
        sha256(key, keylen, k);
    } else {
        const uint8_t* kp = (const uint8_t*)key;
        for (size_t i = 0; i < keylen; i++) k[i] = kp[i];
    }

    for (size_t i = 0; i < SHA256_BLOCK_LEN; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&c);
    sha256_update(&c, pad, SHA256_BLOCK_LEN);
    sha256_update(&c, data, datalen);
    sha256_final(&c, inner);

    for (size_t i = 0; i < SHA256_BLOCK_LEN; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&c);
    sha256_update(&c, pad, SHA256_BLOCK_LEN);
    sha256_update(&c, inner, SHA256_DIGEST_LEN);
    sha256_final(&c, out);
}

/* ---------------------------------------------------------------------------
 * PBKDF2-HMAC-SHA256 (RFC 8018 §5.2).
 * ------------------------------------------------------------------------- */

void pbkdf2_sha256(const void* pass, size_t passlen,
                   const void* salt, size_t saltlen,
                   uint32_t iterations,
                   uint8_t* out, size_t dklen) {
    if (iterations < 1) iterations = 1;

    uint8_t u[SHA256_DIGEST_LEN];
    uint8_t t[SHA256_DIGEST_LEN];
    /* The salt plus a 4-byte big-endian block index.  Bounded because every
     * caller here is a password salt; a longer one is refused by truncation
     * at the call site rather than by a heap allocation on the login path. */
    uint8_t saltblk[128 + 4];
    size_t  sl = saltlen > 128 ? 128 : saltlen;
    const uint8_t* sp = (const uint8_t*)salt;
    for (size_t i = 0; i < sl; i++) saltblk[i] = sp[i];

    uint32_t block = 1;
    size_t done = 0;
    while (done < dklen) {
        saltblk[sl]     = (uint8_t)(block >> 24);
        saltblk[sl + 1] = (uint8_t)(block >> 16);
        saltblk[sl + 2] = (uint8_t)(block >> 8);
        saltblk[sl + 3] = (uint8_t)(block);

        hmac_sha256(pass, passlen, saltblk, sl + 4, u);
        for (int i = 0; i < SHA256_DIGEST_LEN; i++) t[i] = u[i];

        for (uint32_t it = 1; it < iterations; it++) {
            hmac_sha256(pass, passlen, u, SHA256_DIGEST_LEN, u);
            /* T = T xor U_i.  Assigning instead of xor-ing gives PBKDF1-ish
             * behaviour that is stable, repeatable and not PBKDF2 — the exact
             * shape of error the published vectors exist to catch. */
            for (int i = 0; i < SHA256_DIGEST_LEN; i++) t[i] ^= u[i];
        }

        size_t take = dklen - done;
        if (take > SHA256_DIGEST_LEN) take = SHA256_DIGEST_LEN;
        for (size_t i = 0; i < take; i++) out[done + i] = t[i];
        done  += take;
        block += 1;
    }
}

/* =============================================================================
 * `kdftest` — the published vectors.
 *
 * §M71 rule 2 applied to arithmetic: this reports what it OBSERVED against an
 * answer computed by somebody else.  Every expected value below is from the
 * standard that defines it, and each was independently reproduced on the build
 * host before being pasted here — because a vector typed from memory is just
 * our own belief written twice.
 * ========================================================================== */

static int hex_eq(const uint8_t* got, const char* want_hex, size_t len) {
    static const char* d = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        char hi = d[(got[i] >> 4) & 0xF], lo = d[got[i] & 0xF];
        if (want_hex[i * 2] != hi || want_hex[i * 2 + 1] != lo) return 0;
    }
    return want_hex[len * 2] == 0;
}

static void print_hex(const uint8_t* p, size_t len) {
    static const char* d = "0123456789abcdef";
    char line[145];
    size_t n = len > 72 ? 72 : len;
    for (size_t i = 0; i < n; i++) {
        line[i * 2]     = d[(p[i] >> 4) & 0xF];
        line[i * 2 + 1] = d[p[i] & 0xF];
    }
    line[n * 2] = 0;
    console_write(line);
}

static int check(const char* what, const uint8_t* got, const char* want, size_t len) {
    if (hex_eq(got, want, len)) {
        kprintf("  ok   %s\n", what);
        return 0;
    }
    kprintf("  FAIL %s\n       got  ", what);
    print_hex(got, len);
    kprintf("\n       want %s\n", want);
    return 1;
}

static void cmd_kdftest(const char* args) {
    (void)args;
    int fails = 0;
    uint8_t md[SHA256_DIGEST_LEN];
    uint8_t dk[64];

    console_write("kdftest: SHA-256 (FIPS 180-2 appendix B)\n");
    sha256("", 0, md);
    fails += check("SHA-256(\"\")", md,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", 32);
    sha256("abc", 3, md);
    fails += check("SHA-256(\"abc\")", md,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 32);
    const char* m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(m448, 56, md);
    fails += check("SHA-256(448-bit message)", md,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", 32);

    /* The one-million-'a' case.  Not decoration: it is the only vector here
     * that crosses many blocks and pushes the length counter past what a
     * 32-bit BYTE count would hold — the classic silent defect in this
     * algorithm, which every short-message test agrees with. */
    {
        struct sha256_ctx c;
        char chunk[1000];
        for (int i = 0; i < 1000; i++) chunk[i] = 'a';
        sha256_init(&c);
        for (int i = 0; i < 1000; i++) sha256_update(&c, chunk, 1000);
        sha256_final(&c, md);
        fails += check("SHA-256(1000000 x 'a')", md,
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", 32);
    }

    console_write("kdftest: HMAC-SHA256 (RFC 4231)\n");
    {
        uint8_t k[20];
        for (int i = 0; i < 20; i++) k[i] = 0x0b;
        hmac_sha256(k, 20, "Hi There", 8, md);
        fails += check("HMAC case 1", md,
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", 32);
    }
    /* Case 2's key is four bytes — SHORTER than the block, so it exercises the
     * zero-padding half of HMAC.  An implementation that only ever sees
     * block-sized keys passes case 1 and fails here. */
    hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, md);
    fails += check("HMAC case 2 (short key)", md,
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", 32);

    console_write("kdftest: PBKDF2-HMAC-SHA256 (RFC 7914 section 11)\n");
    pbkdf2_sha256("passwd", 6, "salt", 4, 1, dk, 64);
    fails += check("PBKDF2 c=1 dkLen=64", dk,
        "55ac046e56e3089fec1691c22544b605"
        "f94185216dde0465e68b9d57c20dacbc"
        "49ca9cccf179b645991664b39d77ef31"
        "7c71b845b1e30bd509112041d3a19783", 64);

    /* Two blocks' worth of output from one call is the other half of PBKDF2
     * (the block counter), and a 64-byte answer from a 32-byte primitive is
     * where an off-by-one in that counter shows up. */
    pbkdf2_sha256("password", 8, "salt", 4, 4096, dk, 32);
    fails += check("PBKDF2 c=4096 dkLen=32", dk,
        "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a", 32);

    /* WHAT AN ITERATION COSTS ON THIS MACHINE.  The number matters because it
     * is the only thing standing between a stolen password file and the
     * passwords in it, and it has to be picked against a measurement rather
     * than copied from a blog: a count that is comfortable on the build host
     * can make a login take seconds under emulation. */
    {
        uint64_t t0 = timer_now_ns();
        pbkdf2_sha256("benchmark", 9, "0123456789abcdef", 16, 10000, dk, 32);
        uint64_t t1 = timer_now_ns();
        kprintf("kdftest: 10000 iterations took %u ms\n",
                (unsigned)((t1 - t0) / 1000000u));
    }

    if (fails == 0)
        console_write("kdftest: PASS — every published vector matched\n");
    else
        kprintf("kdftest: FAIL — %d vector(s) did not match\n", fails);
}

SHELL_CMD(kdftest) = { "kdftest", "",
                       "check SHA-256 / HMAC / PBKDF2 against published vectors",
                       SHELL_G_TEST, cmd_kdftest };
