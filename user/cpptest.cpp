// ============================================================================
// cpptest.cpp — a C++ program exercising the runtime (§M38 DoD).
//
// Links dynamically against libcpplib.so + libstdc++ + libc (musl).  It:
//   1. uses the STL (std::vector<std::string>, range-for, std::sort),
//   2. calls into cpplib.so and CATCHES a std::runtime_error thrown THERE —
//      the cross-.so exception unwinding that is M38's definition-of-done.
// Output via printf (C) to keep it independent of iostream static init; the
// C++ machinery under test is templates + exceptions + the standard library.
// ============================================================================
#include <string>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <cstdio>

// From libcpplib.so:
std::string cpplib_check(int x);

int main() {
    // 1. STL: build, sort, iterate a vector of strings.
    std::vector<std::string> v = { "gamma", "alpha", "beta" };
    std::sort(v.begin(), v.end());
    std::printf("cpptest: sorted:");
    for (const auto& s : v) std::printf(" %s", s.c_str());
    std::printf("\n");

    // 2. Positive path — no throw.
    std::printf("cpptest: %s\n", cpplib_check(7).c_str());

    // 3. THE gate: catch an exception thrown inside cpplib.so.
    try {
        cpplib_check(-1);
        std::printf("cpptest: ERROR — no exception thrown\n");
    } catch (const std::runtime_error& e) {
        std::printf("cpptest: caught across .so: \"%s\"\n", e.what());
    } catch (...) {
        std::printf("cpptest: caught (wrong type)\n");
    }

    std::printf("cpptest: done\n");
    return 0;
}
