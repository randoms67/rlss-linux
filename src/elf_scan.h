#pragma once

#include <string>
#include <vector>
#include <unordered_set>

// Reads the dynamic symbol table of an Android shared library without loading it.
struct ElfInfo {
    bool valid = false;
    bool is64 = false;
    int machine = 0;
    std::string soname;
    std::vector<std::string> needed;
    std::unordered_set<std::string> defined;
    std::unordered_set<std::string> undefinedStrong;  // undefined, non-weak
    std::unordered_set<std::string> undefinedWeak;

    static ElfInfo read(const std::string& path);
};
