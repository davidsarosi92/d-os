/* cpuidmusl.c — §M90: the CPU-feature ABI a Linux program reads, checked from
 * the program's side (an unmodified musl binary under the Linux personality).
 *
 *   uname        the Linux personality names the ABI it targets ("Linux")
 *   /proc/self/auxv  exists, and its AT_HWCAP equals getauxval(AT_HWCAP)
 *   arm64 only:  HWCAP_CPUID is set and an EL0 `mrs` of the ID registers is
 *                EMULATED (it used to be SIGILL), with the values SANITISED —
 *                SVE hidden (its state is not switched), and HWCAP agreeing
 *                with ISAR0 (AES, CRC32, LSE atomics).
 * Prints "cpuid: PASS" or "cpuid: FAIL <reason>". */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/auxv.h>
#include <sys/utsname.h>

static int fail(const char* why) { printf("cpuid: FAIL %s\n", why); return 1; }

int main(void) {
    struct utsname u;
    if (uname(&u) != 0) return fail("uname");
    printf("cpuid: uname %s %s\n", u.sysname, u.release);
    if (strcmp(u.sysname, "Linux") != 0) return fail("sysname is not Linux");

    unsigned long hw = getauxval(AT_HWCAP);
    unsigned long v[64];
    int fd = open("/proc/self/auxv", O_RDONLY);
    if (fd < 0) return fail("no /proc/self/auxv");
    long n = read(fd, v, sizeof v);
    close(fd);
    unsigned long hw2 = 0; int found = 0, terminated = 0;
    for (long i = 0; i + 1 < n / (long)sizeof(long); i += 2) {
        if (v[i] == AT_NULL) { terminated = 1; break; }
        if (v[i] == AT_HWCAP) { hw2 = v[i + 1]; found = 1; }
    }
    printf("cpuid: auxv %ld bytes, AT_HWCAP %lx, getauxval %lx\n", n, hw2, hw);
    if (n <= 0 || !terminated) return fail("auxv empty or unterminated");
    if (found && hw2 != hw) return fail("auxv file and getauxval disagree");
#ifdef __aarch64__
    if (!found || !(hw & (1ul << 11))) return fail("no HWCAP_CPUID");
    unsigned long isar0, pfr0, midr;
    __asm__ volatile ("mrs %0, ID_AA64ISAR0_EL1" : "=r"(isar0));
    __asm__ volatile ("mrs %0, ID_AA64PFR0_EL1"  : "=r"(pfr0));
    __asm__ volatile ("mrs %0, MIDR_EL1"         : "=r"(midr));
    printf("cpuid: isar0 %lx pfr0 %lx midr %lx\n", isar0, pfr0, midr);
    if ((pfr0 >> 32) & 0xf) return fail("SVE visible");
    if (!!((isar0 >> 4) & 0xf) != !!(hw & (1ul << 3))) return fail("AES: HWCAP and ISAR0 disagree");
    if (!!((isar0 >> 16) & 0xf) != !!(hw & (1ul << 7))) return fail("CRC32: HWCAP and ISAR0 disagree");
    if ((((isar0 >> 20) & 0xf) >= 2) != !!(hw & (1ul << 8))) return fail("ATOMICS: HWCAP and ISAR0 disagree");
    if (!midr) return fail("MIDR reads 0");
#endif
    printf("cpuid: PASS\n");
    return 0;
}
