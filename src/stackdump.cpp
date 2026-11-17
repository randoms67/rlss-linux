// Debug aid for hangs: APKL_STACKDUMP=<seconds> makes a watchdog thread signal
// every thread of the process after that delay (and every 30s after); each
// thread then prints its stack by walking frame pointers, with addresses as
// library+offset (works for the Android libraries loaded by our linker too).

#include "stackdump.h"

#include <mcpelauncher/linker.h>
#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>

namespace {

int stackSignal() { return SIGPWR; }  // unused by apps and not remapped by qemu-user

void describe(uintptr_t addr, char* out, size_t len) {
    Dl_info info{};
    if((linker::dladdr((void*)addr, &info) || dladdr((void*)addr, &info)) && info.dli_fname) {
        const char* base = strrchr(info.dli_fname, '/');
        snprintf(out, len, "%s+0x%lx", base ? base + 1 : info.dli_fname, (unsigned long)(addr - (uintptr_t)info.dli_fbase));
    } else {
        snprintf(out, len, "0x%lx", (unsigned long)addr);
    }
}

void printStack(const char* tag, void* ctx) {
    auto uc = (ucontext_t*)ctx;
    uintptr_t pc = 0, fp = 0, lr = 0;
#if defined(__aarch64__)
    pc = uc->uc_mcontext.pc;
    fp = uc->uc_mcontext.regs[29];
    lr = uc->uc_mcontext.regs[30];
#elif defined(__x86_64__)
    pc = uc->uc_mcontext.gregs[REG_RIP];
    fp = uc->uc_mcontext.gregs[REG_RBP];
#endif
    char name[32] = "?";
    char path[64];
    long tid = syscall(SYS_gettid);
    snprintf(path, sizeof(path), "/proc/self/task/%ld/comm", tid);
    if(FILE* f = fopen(path, "r")) {
        if(fgets(name, sizeof(name), f))
            name[strcspn(name, "\n")] = 0;
        fclose(f);
    }
    char line[2048];
    size_t n = snprintf(line, sizeof(line), "[%s] tid %ld (%s):", tag, tid, name);
    char buf[160];
    describe(pc, buf, sizeof(buf));
    n += snprintf(line + n, sizeof(line) - n, " %s", buf);
    if(lr) {
        describe(lr, buf, sizeof(buf));
        n += snprintf(line + n, sizeof(line) - n, " <- lr %s", buf);
    }
    for(int i = 0; i < 14 && fp && (fp & 7) == 0 && n < sizeof(line) - 200; i++) {
        uintptr_t next = ((uintptr_t*)fp)[0];
        uintptr_t ret = ((uintptr_t*)fp)[1];
        if(!ret)
            break;
        describe(ret, buf, sizeof(buf));
        n += snprintf(line + n, sizeof(line) - n, " <- %s", buf);
        if(next <= fp)
            break;
        fp = next;
    }
    fprintf(stderr, "%s\n", line);
}

void handler(int, siginfo_t*, void* ctx) {
    printStack("stack", ctx);
}

// --- Fault report -----------------------------------------------------------
// The generic crash handler only prints "Signal N received" for faults, and
// often dies before its backtrace. Print what is needed to find the cause
// first: signal code, faulting address and the mapping (file) it belongs to,
// the instruction (library+offset) and the stack; then chain to the old handler.

struct sigaction prevAction[NSIG];

const char* codeName(int sig, int code) {
    if(sig == SIGBUS) {
        switch(code) {
            case BUS_ADRALN: return "BUS_ADRALN (misaligned access)";
            case BUS_ADRERR: return "BUS_ADRERR (mapped page has no backing, e.g. past end of a mapped file)";
            case BUS_OBJERR: return "BUS_OBJERR (object error)";
        }
    } else if(sig == SIGSEGV) {
        switch(code) {
            case SEGV_MAPERR: return "SEGV_MAPERR (address not mapped)";
            case SEGV_ACCERR: return "SEGV_ACCERR (no permission)";
        }
    }
    return "?";
}

void describeMapping(uintptr_t addr, char* out, size_t len) {
    snprintf(out, len, "(not in any mapping)");
    FILE* f = fopen("/proc/self/maps", "r");
    if(!f)
        return;
    char l[512];
    while(fgets(l, sizeof(l), f)) {
        unsigned long lo, hi;
        if(sscanf(l, "%lx-%lx", &lo, &hi) == 2 && addr >= lo && addr < hi) {
            l[strcspn(l, "\n")] = 0;
            snprintf(out, len, "%s (+0x%lx)", l, (unsigned long)(addr - lo));
            break;
        }
    }
    fclose(f);
}

void faultHandler(int sig, siginfo_t* si, void* ctx) {
    static volatile int reported = 0;
    if(!__sync_lock_test_and_set(&reported, 1)) {
        char where[600];
        describeMapping((uintptr_t)si->si_addr, where, sizeof(where));
        fprintf(stderr, "[fault] signal %d %s, address %p\n[fault] address is in: %s\n", sig, codeName(sig, si->si_code),
                si->si_addr, where);
        printStack("fault", ctx);
        fflush(stderr);
    }
    auto& p = prevAction[sig];
    if((p.sa_flags & SA_SIGINFO) && p.sa_sigaction) {
        p.sa_sigaction(sig, si, ctx);
    } else if(p.sa_handler != SIG_DFL && p.sa_handler != SIG_IGN && p.sa_handler) {
        p.sa_handler(sig);
    } else {
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

}  // namespace

void stackdump::installFaultReport() {
    for(int sig : {SIGBUS, SIGSEGV, SIGILL, SIGFPE}) {
        struct sigaction sa{};
        sa.sa_sigaction = faultHandler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(sig, &sa, &prevAction[sig]);
    }
}

void stackdump::install() {
    const char* env = getenv("APKL_STACKDUMP");
    if(!env)
        return;
    int delay = atoi(env);
    struct sigaction sa{};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(stackSignal(), &sa, nullptr);
    std::thread([delay]() {
        std::this_thread::sleep_for(std::chrono::seconds(delay > 0 ? delay : 60));
        while(true) {
            fprintf(stderr, "[stack] ---- dumping all threads ----\n");
            if(DIR* d = opendir("/proc/self/task")) {
                long self = syscall(SYS_gettid);
                while(dirent* e = readdir(d)) {
                    long tid = atol(e->d_name);
                    if(tid <= 0 || tid == self)
                        continue;
                    syscall(SYS_tgkill, getpid(), tid, stackSignal());
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                closedir(d);
            }
            std::this_thread::sleep_for(std::chrono::seconds(30));
        }
    }).detach();
}
