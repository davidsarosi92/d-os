// ============================================================================
// cpplib.cpp — a C++ SHARED LIBRARY that throws (§M38).
//
// The M38 definition-of-done gate: a C++ program that throws + catches across a
// .so boundary.  This library's throw_if_negative() raises a std::runtime_error
// that must unwind back through the caller in the main program — i.e. DWARF
// exception unwinding (.eh_frame + _Unwind_*) has to work ACROSS the shared
// object boundary.  It also exercises a thread-safe static (the local static
// counter → __cxa_guard_*) and RTTI (the exception type).
// ============================================================================
#include <stdexcept>
#include <string>

// A local static in a .so: its guarded init exercises __cxa_guard_acquire/
// release (thread-safe statics → futex under the hood).
static int call_seq() {
    static int n = 0;
    return ++n;
}

// Throws across the .so boundary when x < 0; otherwise returns a std::string
// built with the STL (tests libstdc++ string machinery in the library).
std::string cpplib_check(int x) {
    int seq = call_seq();
    if (x < 0)
        throw std::runtime_error("cpplib: negative value (seq " +
                                 std::to_string(seq) + ")");
    return std::string("cpplib ok, x=") + std::to_string(x) +
           " (seq " + std::to_string(seq) + ")";
}
