// Exercise real offload/ticker worker lifetimes without a window or GPU.
#include "hw/offload.h"
#include "core/log.h"
#include "platform/platform.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <unordered_map>
#include <sys/wait.h>
#include <unistd.h>
bool g_log_enabled[LOG_CAT_COUNT]{};
void log_write(LogCat, const char *, ...) {}
static std::atomic<unsigned> ticks{0};
static void tick() { ++ticks; }
static void check(bool ok, const char *why) { if (!ok) { std::fprintf(stderr,"FAIL %s\n",why); std::exit(1); } }
static void run() {
    // Queue work that references an object about to be destroyed. Shutdown
    // must finish the active job and every pending job before that happens.
    setenv("EOD_VU1_THREAD","1",1);
    for (int cycle=0; platform_cpu_count()>=3 && cycle<4; ++cycle) {
        offload_start();
        check(offload_active(),"offload starts");
        std::unordered_map<int,int> cache;
        std::atomic<bool> started{false},release{false};
        offload_run([&] { started=true; while(!release.load()) std::this_thread::yield(); cache[0]=17; });
        for (int i=1;i<100;++i) offload_run([&,i] {cache[i]=i*3;});
        while(!started.load()) std::this_thread::yield();
        std::thread unblock([&] {std::this_thread::sleep_for(std::chrono::milliseconds(2));release=true;});
        offload_shutdown();
        unblock.join();
        check(cache.size()==100 && cache[0]==17 && cache[99]==297,"shutdown drains active and queued work");
        check(!offload_active() && !offload_busy(),"offload stopped and joined");
        offload_shutdown(); // idempotent
        cache.clear(); // Safe now; no detached job may touch the old owner.
    }
    for (int cycle=0;cycle<3;++cycle) {
        unsigned old=ticks.load();
        platform_start_ticker(tick,1000);
        while(ticks.load()<old+2) std::this_thread::yield();
        platform_stop_ticker();
        old=ticks.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        check(ticks.load()==old,"no ticker callback after stop/join");
        platform_stop_ticker();
    }
}
int main() {
    // The subprocess exits normally, invoking static destructors. A surviving
    // joinable worker would terminate here; a detached active worker could
    // race cache destruction, as in the original game exit crash.
    pid_t child=fork();check(child>=0,"fork");
    if(!child){run();std::exit(0);}
    int status=0;check(waitpid(child,&status,0)==child,"waitpid");
    check(WIFEXITED(status)&&WEXITSTATUS(status)==0,"normal child exit after worker shutdown");
    std::puts("worker drain, join, restart and normal-exit checks passed");
}
