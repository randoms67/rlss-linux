#include "thread_mover.h"

#include <atomic>
#include <future>
#include <thread>

namespace {
struct MainThreadInfo {
    void* (*fn)(void*) = nullptr;
    void* arg = nullptr;
};
std::atomic<std::thread::id> startThreadId;
std::atomic<bool> taken{false};
std::promise<MainThreadInfo> promise;

using pthread_create_fn = int (*)(void*, const void*, void* (*)(void*), void*);
pthread_create_fn origPthreadCreate;

int hookedPthreadCreate(void* thread, const void* attr, void* (*start)(void*), void* arg) {
    if(startThreadId.load() == std::this_thread::get_id()) {
        bool expected = false;
        if(taken.compare_exchange_strong(expected, true)) {
            // The app gets a valid-looking handle; joining it is not supported.
            if(thread)
                *(unsigned long*)thread = 0;
            promise.set_value({start, arg});
            return 0;
        }
    }
    return origPthreadCreate(thread, attr, start, arg);
}
}  // namespace

void ThreadMover::hookLibC(std::unordered_map<std::string, void*>& syms) {
    origPthreadCreate = (pthread_create_fn)syms["pthread_create"];
    syms["pthread_create"] = (void*)hookedPthreadCreate;
}

void ThreadMover::markStartThread() {
    startThreadId.store(std::this_thread::get_id());
}

void ThreadMover::onCreateFinished() {
    startThreadId.store(std::thread::id());
    bool expected = false;
    if(taken.compare_exchange_strong(expected, true))
        promise.set_value({});
}

bool ThreadMover::runMovedThread() {
    auto info = promise.get_future().get();
    if(!info.fn)
        return false;
    info.fn(info.arg);
    return true;
}
