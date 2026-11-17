// Android arm64 code compiled with the TLS stack protector reads the canary
// from bionic's thread slot 5: [TPIDR_EL0 + 0x28]. With glibc, TPIDR_EL0
// points to a 16 byte TCB that is directly followed by the executable's own
// thread-local block, so +0x28 is offset 0x18 of *our* TLS data.
//
// This block must be the first thread-local data of the executable (this file
// is linked first; checked at startup in main.cpp) so that offset 0x18 always
// holds the same, never written canary in every thread.
#include <cstdint>

#if defined(__aarch64__)
extern "C" {
// .tdata (initialized) so every new thread gets the same value from the TLS template
__attribute__((used, aligned(8))) thread_local uintptr_t apkl_bionic_tls_slots[6] = {
    0, 0, 0,
    0x5a17c0de5a17c0deull,  // offset 0x18 = TPIDR_EL0 + 0x28: stack guard
    0, 0,
};
}
#endif
