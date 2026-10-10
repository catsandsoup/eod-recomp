// offload.cpp - the VIF1/VU1, GIF DMA and GS command stream on a worker thread
// (see offload.h).
#include "offload.h"

#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#if defined(__linux__)
#include <pthread.h>
#endif

#include "../core/log.h"
#include "../platform/platform.h"

namespace {

bool g_on;
std::thread::id g_worker;
// never destroyed: the worker may still be waiting on them when the process exits
std::mutex &g_m = *new std::mutex;
std::condition_variable &g_cv_work = *new std::condition_variable;  // jobs queued
std::condition_variable &g_cv_done = *new std::condition_variable;  // progress (jobs run, channels done)
std::deque<std::function<void()>> &g_q = *new std::deque<std::function<void()>>;
bool g_running;  // the worker is executing jobs taken from the queue (guarded by g_m)

void worker() {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "eod-vu1");
#endif
    std::deque<std::function<void()>> batch;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g_m);
            g_running = false;
            g_cv_done.notify_all();
            g_cv_work.wait(lk, [] { return !g_q.empty(); });
            batch.swap(g_q);
            g_running = true;
        }
        for (auto &fn : batch) fn();
        batch.clear();
    }
}

}  // namespace

void offload_start() {
    if (g_on) return;
    // EOD_VU1_THREAD=1 (experimental, off by default: the worker still needs more
    // work than the EE saves, see README)
    const char *e = getenv("EOD_VU1_THREAD");
    bool want = e && atoi(e) != 0 && platform_cpu_count() >= 3;
    if (!want) return;
    std::thread t(worker);
    g_worker = t.get_id();
    t.detach();
    g_on = true;
    LOG(LOG_CORE, "VIF1/VU1 and the GS command stream run on their own thread");
}

bool offload_active() { return g_on; }

bool offload_in_worker() { return g_on && std::this_thread::get_id() == g_worker; }

void offload_run(std::function<void()> fn) {
    if (!g_on || std::this_thread::get_id() == g_worker) {
        fn();
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_m);
        g_q.push_back(std::move(fn));
    }
    g_cv_work.notify_one();
}

void offload_sync() {
    if (!g_on || std::this_thread::get_id() == g_worker) return;
    std::unique_lock<std::mutex> lk(g_m);
    g_cv_done.wait(lk, [] { return g_q.empty() && !g_running; });
}

void offload_wait(const std::function<bool()> &pred) {
    if (!g_on || std::this_thread::get_id() == g_worker) return;
    std::unique_lock<std::mutex> lk(g_m);
    g_cv_done.wait(lk, [&] { return pred() || (g_q.empty() && !g_running); });
}

void offload_notify() {
    if (!g_on) return;
    { std::lock_guard<std::mutex> lk(g_m); }  // (a waiter is either before its check or asleep)
    g_cv_done.notify_all();
}

bool offload_busy() {
    if (!g_on) return false;
    std::lock_guard<std::mutex> lk(g_m);
    return !g_q.empty() || g_running;
}
