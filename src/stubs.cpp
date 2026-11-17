// Generates stand-in functions for Android symbols that an app imports but
// that the launcher does not implement. Without them the Android linker would
// refuse to load the library ("cannot locate symbol"). Each stub logs its
// name once and returns 0.

#include "stubs.h"

#include <log.h>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr size_t kMaxStubs = 4096;
const char* names[kMaxStubs];
std::atomic<bool> logged[kMaxStubs];
size_t used = 0;
std::mutex mutex;
std::vector<std::string> storage;

template <size_t I>
uintptr_t stubFn() {
    if(!logged[I].exchange(true))
        Log::warn("Stub", "Unimplemented Android function called: %s (returning 0)", names[I]);
    return 0;
}

template <size_t... I>
std::array<void*, sizeof...(I)> makeTable(std::index_sequence<I...>) {
    return {{(void*)&stubFn<I>...}};
}

const std::array<void*, kMaxStubs>& table() {
    static const auto t = makeTable(std::make_index_sequence<kMaxStubs>{});
    return t;
}

intptr_t vkError() { return -3; /* VK_ERROR_INITIALIZATION_FAILED */ }
intptr_t slUnsupported() { return 12; /* SL_RESULT_FEATURE_UNSUPPORTED */ }
intptr_t returnZero() { return 0; }

}  // namespace

void* stubs::make(const std::string& name) {
    // Special cases where "0" would mean success and lead to a crash later
    if(name.rfind("vk", 0) == 0) {
        if(name.find("ProcAddr") != std::string::npos)
            return (void*)returnZero;
        return (void*)vkError;
    }
    if(name == "slCreateEngine")
        return (void*)slUnsupported;

    std::lock_guard<std::mutex> l(mutex);
    if(used >= kMaxStubs)
        return (void*)returnZero;
    storage.reserve(kMaxStubs);
    storage.push_back(name);
    names[used] = storage.back().c_str();
    return table()[used++];
}
