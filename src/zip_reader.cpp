#include "zip_reader.h"

#include <zlib.h>
#include <cstring>
#include <stdexcept>
#include <sys/stat.h>
#include <FileUtil.h>

static uint16_t rd16(const unsigned char* p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const unsigned char* p) { return rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

ZipReader::ZipReader(const std::string& path) {
    f_ = fopen(path.c_str(), "rb");
    if(!f_)
        throw std::runtime_error("Cannot open " + path + ": " + strerror(errno));
    fseeko(f_, 0, SEEK_END);
    off_t size = ftello(f_);
    if(size < 22)
        throw std::runtime_error(path + " is not a zip/apk file");
    // End of central directory is within the last 64KiB + 22 bytes
    off_t searchLen = size < 65557 ? size : 65557;
    std::vector<unsigned char> tail(searchLen);
    fseeko(f_, size - searchLen, SEEK_SET);
    if(fread(tail.data(), 1, searchLen, f_) != (size_t)searchLen)
        throw std::runtime_error("Failed to read " + path);
    ssize_t eocd = -1;
    for(ssize_t i = searchLen - 22; i >= 0; i--) {
        if(rd32(&tail[i]) == 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if(eocd < 0)
        throw std::runtime_error(path + " is not a zip/apk file (no central directory)");
    uint64_t cdCount = rd16(&tail[eocd + 10]);
    uint64_t cdSize = rd32(&tail[eocd + 12]);
    uint64_t cdOffset = rd32(&tail[eocd + 16]);
    // ZIP64 locator directly in front of the EOCD
    if(eocd >= 20 && rd32(&tail[eocd - 20]) == 0x07064b50) {
        uint64_t z64off = rd64(&tail[eocd - 20 + 8]);
        unsigned char z64[56];
        fseeko(f_, z64off, SEEK_SET);
        if(fread(z64, 1, 56, f_) == 56 && rd32(z64) == 0x06064b50) {
            cdCount = rd64(z64 + 32);
            cdSize = rd64(z64 + 40);
            cdOffset = rd64(z64 + 48);
        }
    }
    std::vector<unsigned char> cd(cdSize);
    fseeko(f_, cdOffset, SEEK_SET);
    if(fread(cd.data(), 1, cdSize, f_) != cdSize)
        throw std::runtime_error("Failed to read central directory of " + path);
    size_t p = 0;
    for(uint64_t i = 0; i < cdCount && p + 46 <= cd.size(); i++) {
        if(rd32(&cd[p]) != 0x02014b50)
            break;
        Entry e;
        e.method = rd16(&cd[p + 10]);
        e.crc = rd32(&cd[p + 16]);
        e.compressedSize = rd32(&cd[p + 20]);
        e.uncompressedSize = rd32(&cd[p + 24]);
        uint16_t nameLen = rd16(&cd[p + 28]);
        uint16_t extraLen = rd16(&cd[p + 30]);
        uint16_t commentLen = rd16(&cd[p + 32]);
        e.localHeaderOffset = rd32(&cd[p + 42]);
        e.name.assign((const char*)&cd[p + 46], nameLen);
        // ZIP64 extended information
        size_t x = p + 46 + nameLen, xend = x + extraLen;
        while(x + 4 <= xend && xend <= cd.size()) {
            uint16_t id = rd16(&cd[x]), len = rd16(&cd[x + 2]);
            if(id == 0x0001) {
                size_t q = x + 4;
                if(e.uncompressedSize == 0xFFFFFFFF && q + 8 <= x + 4 + len) { e.uncompressedSize = rd64(&cd[q]); q += 8; }
                if(e.compressedSize == 0xFFFFFFFF && q + 8 <= x + 4 + len) { e.compressedSize = rd64(&cd[q]); q += 8; }
                if(e.localHeaderOffset == 0xFFFFFFFF && q + 8 <= x + 4 + len) { e.localHeaderOffset = rd64(&cd[q]); }
            }
            x += 4 + len;
        }
        p += 46 + nameLen + extraLen + commentLen;
        entries_.push_back(std::move(e));
    }
}

ZipReader::~ZipReader() {
    if(f_)
        fclose(f_);
}

const ZipReader::Entry* ZipReader::find(const std::string& name) const {
    for(auto& e : entries_)
        if(e.name == name)
            return &e;
    return nullptr;
}

bool ZipReader::dataOffset(const Entry& e, uint64_t& off) {
    unsigned char lh[30];
    fseeko(f_, e.localHeaderOffset, SEEK_SET);
    if(fread(lh, 1, 30, f_) != 30 || rd32(lh) != 0x04034b50)
        return false;
    off = e.localHeaderOffset + 30 + rd16(lh + 26) + rd16(lh + 28);
    return true;
}

template <class Sink>
bool ZipReader::decode(const Entry& e, Sink&& sink) {
    uint64_t off;
    if(!dataOffset(e, off))
        return false;
    fseeko(f_, off, SEEK_SET);
    std::vector<unsigned char> in(1 << 16), out(1 << 16);
    uint64_t remaining = e.compressedSize;
    if(e.method == 0) {
        while(remaining > 0) {
            size_t n = fread(in.data(), 1, remaining < in.size() ? remaining : in.size(), f_);
            if(n == 0)
                return false;
            sink(in.data(), n);
            remaining -= n;
        }
        return true;
    }
    if(e.method != 8)
        return false;
    z_stream zs{};
    if(inflateInit2(&zs, -MAX_WBITS) != Z_OK)
        return false;
    int ret = Z_OK;
    while(ret != Z_STREAM_END) {
        if(zs.avail_in == 0) {
            if(remaining == 0)
                break;
            size_t n = fread(in.data(), 1, remaining < in.size() ? remaining : in.size(), f_);
            if(n == 0)
                break;
            remaining -= n;
            zs.next_in = in.data();
            zs.avail_in = (uInt)n;
        }
        zs.next_out = out.data();
        zs.avail_out = (uInt)out.size();
        ret = inflate(&zs, Z_NO_FLUSH);
        if(ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&zs);
            return false;
        }
        sink(out.data(), out.size() - zs.avail_out);
    }
    inflateEnd(&zs);
    return ret == Z_STREAM_END;
}

bool ZipReader::read(const Entry& e, std::string& outStr) {
    outStr.clear();
    outStr.reserve(e.uncompressedSize);
    return decode(e, [&](const unsigned char* d, size_t n) { outStr.append((const char*)d, n); });
}

bool ZipReader::extractTo(const Entry& e, const std::string& destPath) {
    auto slash = destPath.rfind('/');
    if(slash != std::string::npos)
        FileUtil::mkdirRecursive(destPath.substr(0, slash));
    std::string tmp = destPath + ".part";
    FILE* o = fopen(tmp.c_str(), "wb");
    if(!o)
        return false;
    bool ok = decode(e, [&](const unsigned char* d, size_t n) { fwrite(d, 1, n, o); });
    ok = (fclose(o) == 0) && ok;
    if(!ok || rename(tmp.c_str(), destPath.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}
