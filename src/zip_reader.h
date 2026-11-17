#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Minimal ZIP (APK) reader: central directory listing + stored/deflate extraction.
class ZipReader {
public:
    struct Entry {
        std::string name;
        uint16_t method = 0;
        uint32_t crc = 0;
        uint64_t compressedSize = 0;
        uint64_t uncompressedSize = 0;
        uint64_t localHeaderOffset = 0;
    };

    explicit ZipReader(const std::string& path);
    ~ZipReader();
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    const std::vector<Entry>& entries() const { return entries_; }
    const Entry* find(const std::string& name) const;

    // Reads a whole entry into memory.
    bool read(const Entry& e, std::string& out);
    // Streams an entry to a file on disk (creates parent directories).
    bool extractTo(const Entry& e, const std::string& destPath);

private:
    FILE* f_ = nullptr;
    std::vector<Entry> entries_;
    bool dataOffset(const Entry& e, uint64_t& off);
    template <class Sink>
    bool decode(const Entry& e, Sink&& sink);
};
