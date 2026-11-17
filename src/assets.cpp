// AAssetManager / AAsset / AAssetDir backed by the extracted assets/ folder.

#include "host.h"

#include <android/asset_manager.h>
#include <log.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <string>

namespace fassets {

struct FakeAssetManager {
    std::string root;  // ends with '/'
};
FakeAssetManager manager;

struct FakeAsset {
    int fd = -1;
    off64_t length = 0;
    off64_t offset = 0;
    void* mapped = nullptr;
};

struct FakeAssetDir {
    DIR* dir = nullptr;
    std::string name;
};

std::string resolve(const char* name) {
    if(!name)
        return {};
    std::string n = name;
    while(n.rfind("./", 0) == 0)
        n = n.substr(2);
    while(!n.empty() && n[0] == '/')
        n = n.substr(1);
    if(n.find("..") != std::string::npos)
        return {};
    return manager.root + n;
}

AAsset* open(AAssetManager*, const char* filename, int) {
    std::string path = resolve(filename);
    if(path.empty())
        return nullptr;
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if(fd < 0) {
        Log::trace("AAssetManager", "open('%s') failed", filename);
        return nullptr;
    }
    struct stat st;
    if(fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return nullptr;
    }
    auto a = new FakeAsset;
    a->fd = fd;
    a->length = st.st_size;
    return (AAsset*)a;
}

AAssetDir* openDir(AAssetManager*, const char* dirname) {
    std::string path = resolve(dirname ? dirname : "");
    if(path.empty())
        path = manager.root;
    DIR* d = opendir(path.c_str());
    if(!d)
        return nullptr;
    auto r = new FakeAssetDir;
    r->dir = d;
    return (AAssetDir*)r;
}

const char* dirNext(AAssetDir* ad) {
    auto d = (FakeAssetDir*)ad;
    if(!d)
        return nullptr;
    while(dirent* e = readdir(d->dir)) {
        // Android only lists files, not sub directories
        if(e->d_type == DT_DIR)
            continue;
        if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        d->name = e->d_name;
        return d->name.c_str();
    }
    return nullptr;
}

#define A(x) ((FakeAsset*)(x))

int readAsset(AAsset* a, void* buf, size_t count) {
    off64_t left = A(a)->length - A(a)->offset;
    if(left <= 0)
        return 0;
    if((off64_t)count > left)
        count = (size_t)left;
    ssize_t r = pread(A(a)->fd, buf, count, A(a)->offset);
    if(r > 0)
        A(a)->offset += r;
    return (int)r;
}

off64_t seek64(AAsset* a, off64_t off, int whence) {
    off64_t n = whence == SEEK_SET ? off : whence == SEEK_CUR ? A(a)->offset + off : A(a)->length + off;
    if(n < 0 || n > A(a)->length)
        return -1;
    return A(a)->offset = n;
}

const void* getBuffer(AAsset* a) {
    if(!A(a)->mapped && A(a)->length > 0) {
        void* m = mmap(nullptr, A(a)->length, PROT_READ, MAP_PRIVATE, A(a)->fd, 0);
        A(a)->mapped = m == MAP_FAILED ? nullptr : m;
    }
    return A(a)->mapped ? A(a)->mapped : "";
}

void closeAsset(AAsset* a) {
    if(!a)
        return;
    if(A(a)->mapped)
        munmap(A(a)->mapped, A(a)->length);
    ::close(A(a)->fd);
    delete A(a);
}

int openFd64(AAsset* a, off64_t* start, off64_t* length) {
    if(start) *start = 0;
    if(length) *length = A(a)->length;
    return dup(A(a)->fd);
}

int openFd(AAsset* a, off_t* start, off_t* length) {
    if(start) *start = 0;
    if(length) *length = (off_t)A(a)->length;
    return dup(A(a)->fd);
}

}  // namespace fassets
using namespace fassets;

void* fakelibs::assetManager() {
    return &manager;
}

void fakelibs::installAssets(std::unordered_map<std::string, void*>& s, const std::string& assetsDir) {
    manager.root = assetsDir;
    if(!manager.root.empty() && manager.root.back() != '/')
        manager.root += '/';
    s["AAssetManager_fromJava"] = (void*)+[](void*, void*) -> AAssetManager* { return (AAssetManager*)&manager; };
    s["AAssetManager_open"] = (void*)fassets::open;
    s["AAssetManager_openDir"] = (void*)openDir;
    s["AAssetDir_getNextFileName"] = (void*)dirNext;
    s["AAssetDir_rewind"] = (void*)+[](AAssetDir* d) { rewinddir(((FakeAssetDir*)d)->dir); };
    s["AAssetDir_close"] = (void*)+[](AAssetDir* d) {
        if(!d) return;
        closedir(((FakeAssetDir*)d)->dir);
        delete (FakeAssetDir*)d;
    };
    s["AAsset_read"] = (void*)readAsset;
    s["AAsset_seek"] = (void*)+[](AAsset* a, off_t off, int whence) -> off_t { return (off_t)seek64(a, off, whence); };
    s["AAsset_seek64"] = (void*)seek64;
    s["AAsset_close"] = (void*)closeAsset;
    s["AAsset_getBuffer"] = (void*)getBuffer;
    s["AAsset_getLength"] = (void*)+[](AAsset* a) -> off_t { return (off_t)A(a)->length; };
    s["AAsset_getLength64"] = (void*)+[](AAsset* a) -> off64_t { return A(a)->length; };
    s["AAsset_getRemainingLength"] = (void*)+[](AAsset* a) -> off_t { return (off_t)(A(a)->length - A(a)->offset); };
    s["AAsset_getRemainingLength64"] = (void*)+[](AAsset* a) -> off64_t { return A(a)->length - A(a)->offset; };
    s["AAsset_openFileDescriptor"] = (void*)openFd;
    s["AAsset_openFileDescriptor64"] = (void*)openFd64;
    s["AAsset_isAllocated"] = (void*)+[](AAsset*) -> int { return 0; };
}
