#pragma once

#include <string>

namespace stubs {
// Returns a unique function pointer that logs `name` on first call and returns 0.
void* make(const std::string& name);
}
