/* =============================================================================
 * cpuid_emu.c — the arm64 CPU-feature ABI a Linux program expects (§M90).
 *
 * Two halves of one contract, and Linux has both:
 *
 *   AT_HWCAP  the auxv word naming the user-visible features (AES, CRC32,
 *             LSE atomics, ...).  hal_user_hwcap() computes it from the
 *             SANITISED ID registers below, so the two can never disagree.
 *   HWCAP_CPUID  "you may read the ID registers": an EL0 `mrs` of
 *             ID_AA64ISAR0_EL1 and friends is UNDEFINED in hardware (or traps,
 *             EC 0x18), and the kernel EMULATES it — returning a sanitised
 *             value and stepping over the instruction.  Linux has done this
 *             since 4.11.
 *
 * WHY IT IS NEEDED.  Go's x/sys/cpu reads /proc/self/auxv for HWCAP; when it
 * cannot (it could not here — there was no such file), it reads the ID
 * registers DIRECTLY if the kernel claims to be 4.11 or newer.  The day the
 * Linux personality started reporting its real target (6.1), dockerd died
 * with SIGILL at its first instruction of `golang.org/x/sys/cpu.getisar0`.
 *
 * SANITISED, NOT RAW.  The hardware may offer features whose STATE this kernel
 * does not save or enable — SVE (registers wider than the FP/SIMD set the
 * context switch keeps), pointer authentication (keys never programmed), MTE,
 * BTI.  A program told about them would use them and corrupt itself, so their
 * fields read as absent, exactly the way Linux hides what it has not enabled.
 * Visible: the instruction-set fields of ISAR0/ISAR1 that need nothing from
 * the kernel, FP/AdvSIMD/DIT/EL0 of PFR0; MIDR as is, MPIDR as Linux shows it
 * (bit 31 only), everything else in the ID space 0.
 * ============================================================================= */

#include <stdint.h>
#include <stddef.h>
#include "vmm.h"         /* copy_from_user */

#define RD(name) ({ uint64_t _v; __asm__ volatile ("mrs %0, " name : "=r"(_v)); _v; })
#define FLD(v, lo) (((v) >> (lo)) & 0xFull)

/* The value an EL0 read of ID register (op0=3, op1=0, CRn=0, CRm, op2) sees. */
static uint64_t id_reg(int crm, int op2) {
    switch (crm << 3 | op2) {
    case 0 << 3 | 0: return RD("S3_0_C0_C0_0");                 /* MIDR_EL1 */
    case 0 << 3 | 5: return 0x80000000ull;                      /* MPIDR: as Linux */
    case 4 << 3 | 0: {                                          /* ID_AA64PFR0_EL1 */
        uint64_t v = RD("S3_0_C0_C4_0");
        return (v & (0xFFull << 16)) | (v & (0xFull << 48)) | 0x1ull;   /* FP, AdvSIMD, DIT, EL0=AArch64 */
    }
    case 5 << 3 | 0: return 0x6ull;                             /* ID_AA64DFR0: DebugVer v8 */
    case 6 << 3 | 0: {                                          /* ID_AA64ISAR0_EL1 */
        uint64_t v = RD("S3_0_C0_C6_0");
        return v & 0xF0FFFFFFF0FFFFF0ull;                       /* all but TLB[59:56], [27:24], [3:0] */
    }
    case 6 << 3 | 1: {                                          /* ID_AA64ISAR1_EL1 */
        uint64_t v = RD("S3_0_C0_C6_1");
        uint64_t keep = (0xFull << 52) | (0xFull << 44) | (0xFull << 36) | (0xFull << 32)   /* I8MM BF16 SB FRINTTS */
                      | (0xFull << 20) | (0xFull << 16) | (0xFull << 12) | 0xFull;       /* LRCPC FCMA JSCVT DPB */
        return v & keep;                                        /* PAC fields hidden */
    }
    default: return 0;                                          /* SVE, MTE, BTI, MMFR*, AArch32: absent */
    }
}

/* AT_HWCAP, from the same sanitised registers. */
uintptr_t hal_user_hwcap(void) {
    uint64_t pfr0 = id_reg(4, 0), isar0 = id_reg(6, 0), isar1 = id_reg(6, 1);
    uintptr_t h = 1ul << 11;                                    /* CPUID: ID registers readable */
    uint64_t fp = FLD(pfr0, 16), simd = FLD(pfr0, 20);
    if (fp != 0xF)   { h |= 1ul << 0;  if (fp == 1)   h |= 1ul << 9;  }   /* FP, FPHP */
    if (simd != 0xF) { h |= 1ul << 1;  if (simd == 1) h |= 1ul << 10; }   /* ASIMD, ASIMDHP */
    if (FLD(pfr0, 48)) h |= 1ul << 24;                          /* DIT */
    uint64_t aes = FLD(isar0, 4);
    if (aes >= 1) h |= 1ul << 3;                                /* AES */
    if (aes >= 2) h |= 1ul << 4;                                /* PMULL */
    if (FLD(isar0, 8))  h |= 1ul << 5;                          /* SHA1 */
    if (FLD(isar0, 12) >= 1) h |= 1ul << 6;                     /* SHA2 */
    if (FLD(isar0, 12) >= 2) h |= 1ul << 21;                    /* SHA512 */
    if (FLD(isar0, 16)) h |= 1ul << 7;                          /* CRC32 */
    if (FLD(isar0, 20) >= 2) h |= 1ul << 8;                     /* ATOMICS (LSE) */
    if (FLD(isar0, 28)) h |= 1ul << 12;                         /* ASIMDRDM */
    if (FLD(isar0, 32)) h |= 1ul << 17;                         /* SHA3 */
    if (FLD(isar0, 36)) h |= 1ul << 18;                         /* SM3 */
    if (FLD(isar0, 40)) h |= 1ul << 19;                         /* SM4 */
    if (FLD(isar0, 44)) h |= 1ul << 20;                         /* ASIMDDP */
    if (FLD(isar0, 48)) h |= 1ul << 23;                         /* ASIMDFHM */
    if (FLD(isar0, 52)) h |= 1ul << 27;                         /* FLAGM */
    if (FLD(isar1, 0))  h |= 1ul << 16;                         /* DCPOP */
    if (FLD(isar1, 12)) h |= 1ul << 13;                         /* JSCVT */
    if (FLD(isar1, 16)) h |= 1ul << 14;                         /* FCMA */
    if (FLD(isar1, 20) >= 1) h |= 1ul << 15;                    /* LRCPC */
    if (FLD(isar1, 20) >= 2) h |= 1ul << 26;                    /* ILRCPC */
    if (FLD(isar1, 36)) h |= 1ul << 29;                         /* SB */
    return h;
}

/* An EL0 instruction that faulted as undefined (EC 0x00) or as a trapped
 * system-register access (EC 0x18): if it is an `mrs` from the ID space,
 * emulate it — Xt gets the sanitised value, the PC steps over — and return 1.
 * Anything else is the caller's (SIGILL). */
int aarch64_emulate_el0_mrs(uint64_t esr, uint64_t* x, uint64_t* elr) {
    uint32_t ec = (uint32_t)(esr >> 26);
    int op0, op1, crn, crm, op2, rt;
    if (ec == 0x18) {
        uint32_t iss = (uint32_t)(esr & 0x1FFFFFF);
        if (!(iss & 1)) return 0;                               /* a write (msr): no */
        op0 = (int)((iss >> 20) & 3); op2 = (int)((iss >> 17) & 7); op1 = (int)((iss >> 14) & 7);
        crn = (int)((iss >> 10) & 0xF); rt = (int)((iss >> 5) & 0x1F); crm = (int)((iss >> 1) & 0xF);
    } else if (ec == 0x00) {
        uint32_t insn = 0;
        if (copy_from_user(&insn, (uintptr_t)*elr, sizeof insn) != 0) return 0;
        if ((insn & 0xFFF00000u) != 0xD5300000u) return 0;      /* not mrs */
        op0 = 2 + (int)((insn >> 19) & 1); op1 = (int)((insn >> 16) & 7);
        crn = (int)((insn >> 12) & 0xF); crm = (int)((insn >> 8) & 0xF);
        op2 = (int)((insn >> 5) & 7); rt = (int)(insn & 0x1F);
    } else {
        return 0;
    }
    if (op0 != 3 || op1 != 0 || crn != 0) return 0;             /* only the ID space */
    if (crm == 0 && op2 != 0 && op2 != 5 && op2 != 6) return 0; /* CTR/DCZID etc. are not ours */
    uint64_t v = (crm == 0 && op2 == 6) ? 0 : id_reg(crm, op2); /* REVIDR reads 0 */
    if (rt != 31) x[rt] = v;                                    /* xzr discards */
    *elr += 4;
    return 1;
}
