// kernel.cpp - EE kernel HLE.
//
// EE threads map 1:1 onto host fibers.  There is a single CPU state
// (g_ee); switching threads saves it into the outgoing thread's control
// block and loads the incoming one, exactly as the real kernel does.
// Interrupts are delivered only at safe points: loop back-edges in
// recompiled code (IRQ_CHECK), syscalls and the idle loop.
#include "kernel.h"

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../core/cpu.h"
#include "../core/log.h"
#include "../core/memory.h"
#include "../gs/gs.h"
#include "../hw/hw.h"
#include "../iop/iop.h"
#include "../iop/sif.h"
#include "../platform/platform.h"
#include "../core/prof.h"
#include "../ui/overlay.h"

void iop_pad_vblank();
void game_patches_vblank();  // src/game_patches.cpp
[[noreturn]] void app_exit(int code);  // main.cpp

extern "C" volatile int g_ee_irq_pending;
volatile int g_ee_irq_pending;

// time the EE spent asleep waiting for events (EOD_FPS statistics)
std::atomic<uint64_t> g_ee_idle_us{0};

namespace {

constexpr int kMaxThreads = 256;
constexpr int kMaxSemas = 256;
constexpr uint32_t kThreadExitAddr = 0x00081000;  // fake return address
constexpr uint32_t kIrqStackTop = 0x000FF000;     // interrupt handler stack (kernel RAM)
constexpr uint32_t kSyscallTable = 0x00010000;    // kernel syscall table (kernel RAM)

enum ThreadStatus {
    THS_RUN = 0x01,
    THS_READY = 0x02,
    THS_WAIT = 0x04,
    THS_SUSPEND = 0x08,
    THS_WAITSUSPEND = 0x0C,
    THS_DORMANT = 0x10,
};

enum WaitType { WAIT_NONE = 0, WAIT_SLEEP = 1, WAIT_SEMA = 2 };

struct Thread {
    bool used = false;
    int id = 0;
    int status = THS_DORMANT;
    int prio = 0, init_prio = 0;
    uint32_t entry = 0, stack = 0, stack_size = 0, gp = 0, attr = 0, option = 0;
    uint32_t arg = 0;
    int wait_type = WAIT_NONE, wait_id = 0;
    int wakeup_count = 0;
    int suspend_count = 0;
    uint64_t ready_seq = 0;  // FIFO order within a priority
    EEState regs;
    HostFiber *fiber = nullptr;
    bool parked = false;  // fiber is idle in thread_main's exit point
    int ret = 0;          // syscall return value delivered on wakeup
};

struct Sema {
    bool used = false;
    int count = 0, max_count = 0, init_count = 0;
    uint32_t attr = 0, option = 0;
    std::vector<int> waiters;  // thread ids in FIFO order
};

struct Handler {
    int id;
    uint32_t func, arg, gp;
    bool enabled;
};

struct Alarm {
    bool used = false;
    uint64_t due_us = 0;
    uint32_t func = 0, arg = 0, gp = 0;
    uint16_t time = 0;
};

Thread g_threads[kMaxThreads];
Sema g_semas[kMaxSemas];
Thread *g_cur = nullptr;
uint64_t g_seq = 1;
bool g_dispatch_disabled = false;
bool g_ints_enabled = false;
bool g_in_interrupt = false;
bool g_resched_pending = false;
std::vector<HostFiber *> g_zombies;

std::vector<Handler> g_intc_handlers[15];
std::vector<Handler> g_dmac_handlers[16];
int g_next_handler_id = 1;
// INTC_STAT as software sees it: a bit stays set until the program writes 1
// to it (libgraph's sceGsSyncV clears the vblank bit and polls for it, with
// interrupts enabled).  g_intc_new holds the events not yet dispatched to
// handlers: every event runs the handlers once.
std::atomic<uint32_t> g_intc_stat{0};
std::atomic<uint32_t> g_intc_new{0};
uint32_t g_intc_mask = 0;
std::atomic<uint32_t> g_dmac_pending{0};
static void ee_sleep_us(uint32_t us) {
    ProfScope prof(P_EE_IDLE);
    uint64_t t = platform_time_us();
    platform_sleep_us(us);
    g_ee_idle_us += platform_time_us() - t;
}

Alarm g_alarms[64];
uint32_t g_syscall_override[256];

uint32_t g_heap_start, g_heap_end;
uint32_t g_main_stack, g_main_stack_size;

// timeline
constexpr double kVblankPeriodUs = 1000000.0 / 59.94;
constexpr double kHsyncUs = 1000000.0 / 15734.0;
uint64_t g_next_vblank_us;
uint64_t g_vblank_end_us;
bool g_vblank_end_pending;
uint64_t g_vblank_count;
uint32_t g_vsync_flag_addr, g_vsync_flag_cnt_addr;

uint32_t g_osd_config = 0;

// ------------------------------------------------------------- helpers
Thread *thread_by_id(int id) {
    if (id <= 0 || id >= kMaxThreads || !g_threads[id].used) return nullptr;
    return &g_threads[id];
}

void ticker_cb() { g_ee_irq_pending = 1; }

void delete_zombies() {
    for (HostFiber *f : g_zombies) fiber_delete(f);
    g_zombies.clear();
}

void thread_main(void *p);

// Picks the best runnable thread (current thread wins ties).
Thread *pick_next() {
    Thread *best = nullptr;
    if (g_cur && g_cur->status == THS_RUN) best = g_cur;
    for (auto &t : g_threads) {
        if (!t.used || t.status != THS_READY) continue;
        if (!best || t.prio < best->prio || (t.prio == best->prio && best != g_cur && t.ready_seq < best->ready_seq))
            best = &t;
    }
    return best;
}

void make_ready(Thread *t) {
    t->status = THS_READY;
    t->ready_seq = g_seq++;
}

void switch_to(Thread *next) {
    if (next == g_cur) {
        next->status = THS_RUN;
        return;
    }
    Thread *prev = g_cur;
    LOG(LOG_THREAD, "switch %d -> %d", prev ? prev->id : -1, next->id);
    if (prev) {
        prev->regs = g_ee;
        if (prev->status == THS_RUN) make_ready(prev);
    }
    g_cur = next;
    next->status = THS_RUN;
    g_ee = next->regs;
    if (!next->fiber) next->fiber = fiber_create(1024 * 1024, thread_main, next);
    fiber_switch(next->fiber);
    // resumed: we are prev again
    delete_zombies();
}

void service_events(bool run_alarms = true);
void dispatch_interrupts();

// Wait until some thread can run; runs interrupts in the meantime.
void idle_until_runnable() {
    for (;;) {
        Thread *n = pick_next();
        if (n) {
            switch_to(n);
            return;
        }
        // nothing to run: sleep until the next event
        uint64_t now = platform_time_us();
        uint64_t next = g_vblank_end_pending ? g_vblank_end_us : g_next_vblank_us;
        for (auto &a : g_alarms)
            if (a.used && a.due_us < next) next = a.due_us;
        if (next > now && !(g_intc_new.load() & g_intc_mask) && !g_dmac_pending.load()) {
            uint64_t d = next - now;
            ee_sleep_us((uint32_t)(d > 2000 ? 2000 : d));
        }
        bool saved = g_ints_enabled;
        g_ints_enabled = true;  // the idle thread runs with interrupts on
        ee_service_interrupts();
        g_ints_enabled = saved;
    }
}

void reschedule() {
    if (g_in_interrupt) {
        g_resched_pending = true;
        return;
    }
    if (g_dispatch_disabled && g_cur && g_cur->status == THS_RUN) return;
    idle_until_runnable();
}

void thread_main(void *p) {
    Thread *t = static_cast<Thread *>(p);
    delete_zombies();
    for (;;) {
        // g_ee was loaded by switch_to
        LOG(LOG_THREAD, "thread %d starts at %08x", t->id, t->entry);
        ee_call(t->entry);
        LOG(LOG_THREAD, "thread %d returned from entry", t->id);
        t->status = THS_DORMANT;
        t->parked = true;
        reschedule();
        t->parked = false;
    }
}

void wake(Thread *t, int ret) {
    t->wait_type = WAIT_NONE;
    t->wait_id = 0;
    t->regs.r[2].sd[0] = ret;
    if (t->status == THS_WAITSUSPEND)
        t->status = THS_SUSPEND;
    else
        make_ready(t);
    if (t == g_cur) g_ee.r[2].sd[0] = ret;
}

// Blocks the current thread. Returns the value given by the waker.
int block_current(int wait_type, int wait_id) {
    Thread *t = g_cur;
    t->status = THS_WAIT;
    t->wait_type = wait_type;
    t->wait_id = wait_id;
    t->regs.r[2].sd[0] = 0;
    reschedule();
    return g_ee.r[2].sw[0];
}

void init_thread_regs(Thread *t) {
    memset(&t->regs, 0, sizeof t->regs);
    t->regs.r[28].sd[0] = (int32_t)t->gp;
    t->regs.r[29].sd[0] = (int32_t)(t->stack + t->stack_size - 16);
    t->regs.r[30].sd[0] = t->regs.r[29].sd[0];
    t->regs.r[31].sd[0] = kThreadExitAddr;
    t->regs.r[4].sd[0] = (int32_t)t->arg;
    t->regs.fcr31 = 0x01000001;  // FCR31 reset value-ish
}

// ------------------------------------------------- interrupt delivery
int call_handler(const Handler &h, uint32_t a0, uint32_t a1, uint32_t a2) {
    EEState saved = g_ee;
    g_ee.r[4].sd[0] = (int32_t)a0;
    g_ee.r[5].sd[0] = (int32_t)a1;
    g_ee.r[6].sd[0] = (int32_t)a2;
    g_ee.r[28].sd[0] = (int32_t)h.gp;
    g_ee.r[29].sd[0] = (int32_t)(kIrqStackTop - 0x100);
    g_ee.r[31].sd[0] = kThreadExitAddr;
    ee_call(h.func);
    int ret = g_ee.r[2].sw[0];
    g_ee = saved;
    return ret;
}

// EOD_WATCH=addr[,addr...] (hex): logs float values at EE addresses when they change.
void debug_watch() {
    static std::vector<uint32_t> addrs;
    static std::vector<uint32_t> last;
    static bool parsed;
    if (!parsed) {
        parsed = true;
        if (const char *w = getenv("EOD_WATCH")) {
            for (const char *p = w; *p;) {
                char *end;
                uint32_t a = (uint32_t)strtoul(p, &end, 16);
                if (*end == '+') a += (uint32_t)strtoul(end + 1, &end, 16);
                addrs.push_back(a & ~3u);
                p = *end ? end + 1 : end;
            }
            last.assign(addrs.size(), 0xDEADBEEF);
        }
    }
    for (size_t i = 0; i < addrs.size(); i++) {
        uint32_t v = ee_r32(addrs[i]);
        if (v == last[i]) continue;
        last[i] = v;
        float f;
        memcpy(&f, &v, 4);
        LOG(LOG_CORE, "watch %08x = %08x (%g) at vblank %llu", addrs[i], v, f, (unsigned long long)g_vblank_count);
    }
}

// The in-game menu holds the game: keep its window alive and the menu drawn,
// then resume the timing as if no time had passed.
static void hold_for_menu() {
    ProfScope prof(P_EE_IDLE);
    while (overlay_paused()) {
        gs_overlay_redraw();
        platform_sleep_us(16000);
        if (!platform_pump()) {
            LOG(LOG_CORE, "window closed - exiting");
            app_exit(0);
        }
        overlay_frame();
    }
    g_next_vblank_us = platform_time_us() + (uint64_t)kVblankPeriodUs;
}

// Generates the timed events (vblank start/end set INTC_STAT even while
// interrupts are disabled, as on hardware); alarm callbacks run only when
// `run_alarms` (interrupts enabled).
void service_events(bool run_alarms) {
    uint64_t now = platform_time_us();
    iop_sound_update();
    debug_watch();
    if (!g_next_vblank_us) g_next_vblank_us = now + (uint64_t)kVblankPeriodUs;
    if (g_vblank_end_pending && now >= g_vblank_end_us) {
        g_vblank_end_pending = false;
        intc_raise(INTC_VBLANK_E);
    }
    if (now >= g_next_vblank_us && !g_vblank_end_pending) {
        g_vblank_count++;
        // Vblanks missed while the EE was busy are delivered back to back (the
        // game counts them as time), up to a few frames; beyond that, resync.
        g_next_vblank_us += (uint64_t)kVblankPeriodUs;
        bool behind = g_next_vblank_us <= now;
        if (now > g_next_vblank_us + 12 * (uint64_t)kVblankPeriodUs) g_next_vblank_us = now + (uint64_t)kVblankPeriodUs;
        g_vblank_end_us = behind ? now : now + 1400;
        g_vblank_end_pending = true;
        iop_pad_vblank();
        game_patches_vblank();
        gs_vblank();
        if (g_vsync_flag_addr) {
            ee_w32(g_vsync_flag_addr, 1);
            if (g_vsync_flag_cnt_addr) MEM_SD(g_vsync_flag_cnt_addr, g_vblank_count);
        }
        intc_raise(INTC_VBLANK_S);
        if (!platform_pump()) {
            LOG(LOG_CORE, "window closed - exiting");
            app_exit(0);
        }
        overlay_frame();
        if (overlay_paused()) hold_for_menu();
    }
    for (int i = 0; i < 64 && run_alarms; i++) {
        Alarm &a = g_alarms[i];
        if (a.used && now >= a.due_us) {
            a.used = false;
            Handler h{i, a.func, a.arg, a.gp, true};
            LOG(LOG_KERNEL, "alarm %d fires -> %08x", i, a.func);
            call_handler(h, i, a.time, a.arg);
        }
    }
}

void dispatch_interrupts() {
    g_in_interrupt = true;
    uint32_t pend = g_intc_new.exchange(0);
    for (int cause = 0; cause < 15; cause++) {
        if (!(pend & (1u << cause))) continue;
        if (!(g_intc_mask & (1u << cause))) {
            // masked: dispatched once unmasked
            g_intc_new |= 1u << cause;
            continue;
        }
        for (const Handler &h : g_intc_handlers[cause]) {
            if (!h.enabled) continue;
            LOG(LOG_KERNEL, "INTC %d -> handler %08x", cause, h.func);
            call_handler(h, cause, h.arg, 0);
        }
    }
    uint32_t dpend = g_dmac_pending.exchange(0);
    for (int ch = 0; ch < 16; ch++) {
        if (!(dpend & (1u << ch))) continue;
        if (!hw_dmac_channel_irq_enabled(ch)) {
            continue;
        }
        for (const Handler &h : g_dmac_handlers[ch]) {
            if (!h.enabled) continue;
            LOG(LOG_KERNEL, "DMAC %d -> handler %08x", ch, h.func);
            call_handler(h, ch, h.arg, 0);
        }
        if (ch == 5) sif_after_sif0_irq();
    }
    g_in_interrupt = false;
}

int add_handler(std::vector<Handler> &list, uint32_t func, int next, uint32_t arg) {
    Handler h{g_next_handler_id++, func, arg, g_ee.r[28].uw[0], true};
    if (next == 0)
        list.insert(list.begin(), h);
    else
        list.push_back(h);
    return h.id;
}

int remove_handler(std::vector<Handler> *lists, int n, int id) {
    for (int i = 0; i < n; i++)
        for (auto it = lists[i].begin(); it != lists[i].end(); ++it)
            if (it->id == id) {
                lists[i].erase(it);
                return 0;
            }
    return -1;
}

[[maybe_unused]] int set_handler_enabled(std::vector<Handler> *lists, int n, int id, bool on) {
    for (int i = 0; i < n; i++)
        for (auto &h : lists[i])
            if (h.id == id) {
                h.enabled = on;
                return 0;
            }
    return -1;
}

// ---------------------------------------------------------- syscalls
int sys_create_thread(uint32_t param) {
    for (int i = 1; i < kMaxThreads; i++) {
        Thread &t = g_threads[i];
        if (t.used) continue;
        t = Thread();
        t.used = true;
        t.id = i;
        t.status = THS_DORMANT;
        t.entry = ee_r32(param + 4);
        t.stack = ee_r32(param + 8);
        t.stack_size = ee_r32(param + 12);
        t.gp = ee_r32(param + 16);
        t.init_prio = t.prio = (int)ee_r32(param + 20);
        t.attr = ee_r32(param + 28);
        t.option = ee_r32(param + 32);
        LOG(LOG_THREAD, "CreateThread %d entry=%08x stack=%08x+%x prio=%d", i, t.entry, t.stack, t.stack_size,
            t.prio);
        return i;
    }
    return -1;
}

int sys_start_thread(int id, uint32_t arg) {
    Thread *t = thread_by_id(id);
    if (!t || t->status != THS_DORMANT) return -1;
    t->arg = arg;
    t->prio = t->init_prio;
    init_thread_regs(t);
    if (t->fiber && !t->parked) {
        // was terminated while blocked somewhere: needs a fresh host stack
        g_zombies.push_back(t->fiber);
        t->fiber = nullptr;
    }
    make_ready(t);
    LOG(LOG_THREAD, "StartThread %d arg=%08x", id, arg);
    reschedule();
    return id;
}

void remove_from_sema_queue(Thread *t) {
    if (t->wait_type != WAIT_SEMA) return;
    if (t->wait_id > 0 && t->wait_id < kMaxSemas) {
        auto &w = g_semas[t->wait_id].waiters;
        for (auto it = w.begin(); it != w.end(); ++it)
            if (*it == t->id) {
                w.erase(it);
                break;
            }
    }
}

int sys_terminate_thread(int id) {
    Thread *t = thread_by_id(id);
    if (!t || t == g_cur) return -1;
    remove_from_sema_queue(t);
    t->status = THS_DORMANT;
    t->wait_type = WAIT_NONE;
    return id;
}

int sys_delete_thread(int id) {
    Thread *t = thread_by_id(id);
    if (!t || t == g_cur || t->status != THS_DORMANT) return -1;
    if (t->fiber) g_zombies.push_back(t->fiber);
    t->fiber = nullptr;
    t->used = false;
    return id;
}

int sys_change_priority(int id, int prio) {
    Thread *t = id == 0 ? g_cur : thread_by_id(id);
    if (!t) return -1;
    int old = t->prio;
    t->prio = prio;
    if (t->status == THS_READY) t->ready_seq = g_seq++;
    reschedule();
    return old;
}

int sys_rotate_ready_queue(int prio) {
    if (g_cur && g_cur->prio == prio && g_cur->status == THS_RUN) {
        // move current to the back of its priority level
        bool other = false;
        for (auto &t : g_threads)
            if (t.used && t.status == THS_READY && t.prio == prio) other = true;
        if (other) {
            make_ready(g_cur);
            reschedule();
        }
    } else {
        for (auto &t : g_threads)
            if (t.used && t.status == THS_READY && t.prio == prio) {
                t.ready_seq = g_seq++;
                break;
            }
    }
    return prio;
}

int sys_sleep_thread() {
    if (g_cur->wakeup_count > 0) {
        g_cur->wakeup_count--;
        return g_cur->id;
    }
    block_current(WAIT_SLEEP, 0);
    return g_cur->id;
}

int sys_wakeup_thread(int id) {
    Thread *t = thread_by_id(id);
    if (!t || t == g_cur) return -1;
    if ((t->status & THS_WAIT) && t->wait_type == WAIT_SLEEP)
        wake(t, id);
    else
        t->wakeup_count++;
    reschedule();
    return id;
}

int sys_release_wait(int id) {
    Thread *t = thread_by_id(id);
    if (!t || !(t->status & THS_WAIT)) return -1;
    remove_from_sema_queue(t);
    wake(t, -1);
    reschedule();
    return id;
}

int sys_suspend_thread(int id) {
    Thread *t = thread_by_id(id);
    if (!t || t == g_cur) return -1;
    if (t->status == THS_READY) t->status = THS_SUSPEND;
    else if (t->status == THS_WAIT) t->status = THS_WAITSUSPEND;
    t->suspend_count++;
    return id;
}

int sys_resume_thread(int id) {
    Thread *t = thread_by_id(id);
    if (!t) return -1;
    if (t->suspend_count > 0 && --t->suspend_count == 0) {
        if (t->status == THS_SUSPEND) make_ready(t);
        else if (t->status == THS_WAITSUSPEND) t->status = THS_WAIT;
    }
    reschedule();
    return id;
}

int sys_refer_thread_status(int id, uint32_t info) {
    Thread *t = id == 0 ? g_cur : thread_by_id(id);
    if (!t) return -1;
    if (info) {
        ee_w32(info + 0, t->status);
        ee_w32(info + 4, t->entry);
        ee_w32(info + 8, t->stack);
        ee_w32(info + 12, t->stack_size);
        ee_w32(info + 16, t->gp);
        ee_w32(info + 20, t->init_prio);
        ee_w32(info + 24, t->prio);
        ee_w32(info + 28, t->attr);
        ee_w32(info + 32, t->option);
        ee_w32(info + 36, t->wait_type);
        ee_w32(info + 40, t->wait_id);
        ee_w32(info + 44, t->wakeup_count);
    }
    return t->status;
}

int sys_create_sema(uint32_t param) {
    for (int i = 1; i < kMaxSemas; i++) {
        Sema &s = g_semas[i];
        if (s.used) continue;
        s = Sema();
        s.used = true;
        s.max_count = (int)ee_r32(param + 4);
        s.init_count = (int)ee_r32(param + 8);
        s.count = s.init_count;
        s.attr = ee_r32(param + 16);
        s.option = ee_r32(param + 20);
        LOG(LOG_THREAD, "CreateSema %d init=%d max=%d", i, s.init_count, s.max_count);
        return i;
    }
    return -1;
}

Sema *sema_by_id(int id) {
    if (id <= 0 || id >= kMaxSemas || !g_semas[id].used) return nullptr;
    return &g_semas[id];
}

int sys_signal_sema(int id) {
    Sema *s = sema_by_id(id);
    if (!s) return -1;
    if (!s->waiters.empty()) {
        // wake highest priority waiter (FIFO among equals)
        auto best = s->waiters.begin();
        for (auto it = s->waiters.begin(); it != s->waiters.end(); ++it)
            if (g_threads[*it].prio < g_threads[*best].prio) best = it;
        Thread *t = &g_threads[*best];
        s->waiters.erase(best);
        wake(t, id);
        reschedule();
    } else {
        s->count++;
    }
    return id;
}

int sys_wait_sema(int id) {
    Sema *s = sema_by_id(id);
    if (!s) return -1;
    if (s->count > 0) {
        s->count--;
        return id;
    }
    if (g_in_interrupt) fatal("WaitSema(%d) inside an interrupt handler", id);
    s->waiters.push_back(g_cur->id);
    return block_current(WAIT_SEMA, id);
}

int sys_poll_sema(int id) {
    Sema *s = sema_by_id(id);
    if (!s) return -1;
    if (s->count > 0) {
        s->count--;
        return id;
    }
    return -1;
}

int sys_delete_sema(int id) {
    Sema *s = sema_by_id(id);
    if (!s) return -1;
    for (int tid : s->waiters) wake(&g_threads[tid], -1);
    s->waiters.clear();
    s->used = false;
    reschedule();
    return id;
}

int sys_refer_sema(int id, uint32_t info) {
    Sema *s = sema_by_id(id);
    if (!s) return -1;
    if (info) {
        ee_w32(info + 0, s->count);
        ee_w32(info + 4, s->max_count);
        ee_w32(info + 8, s->init_count);
        ee_w32(info + 12, (uint32_t)s->waiters.size());
        ee_w32(info + 16, s->attr);
        ee_w32(info + 20, s->option);
    }
    return id;
}

int sys_set_alarm(uint32_t time, uint32_t func, uint32_t arg) {
    for (int i = 0; i < 64; i++) {
        Alarm &a = g_alarms[i];
        if (a.used) continue;
        a.used = true;
        a.time = (uint16_t)time;
        a.func = func;
        a.arg = arg;
        a.gp = g_ee.r[28].uw[0];
        a.due_us = platform_time_us() + (uint64_t)(time * kHsyncUs);
        return i;
    }
    return -1;
}

void sys_setup_thread(uint32_t gp, uint32_t stack, uint32_t stack_size, uint32_t args, uint32_t root) {
    (void)root;
    if (stack == 0xFFFFFFFF) stack = EE_RAM_SIZE - stack_size;
    g_main_stack = stack;
    g_main_stack_size = stack_size;
    Thread &t = g_threads[1];
    t.gp = gp;
    t.stack = stack;
    t.stack_size = stack_size;
    // argc/argv for crt0
    if (args) {
        const char *arg0 = "cdrom0:\\SLUS_209.10;1";
        uint32_t strs = args + 4 + 16 * 4;
        ee_w32(args, 1);
        ee_w32(args + 4, strs);
        ee_write_block(strs, arg0, (uint32_t)strlen(arg0) + 1);
    }
    uint32_t sp = stack + stack_size - 0x2A0;
    LOG(LOG_KERNEL, "SetupThread gp=%08x stack=%08x size=%x -> sp=%08x", gp, stack, stack_size, sp);
    ret_s32((int32_t)sp);
}

int handle_syscall(int num) {
    uint32_t a0 = arg_u32(0), a1 = arg_u32(1), a2 = arg_u32(2), a3 = arg_u32(3);
    bool i_variant = num < 0;
    if (i_variant) num = -num;
    switch (num) {
    case 0x01:  // ResetEE
        return 0;
    case 0x02:  // SetGsCrt(interlace, mode, ffmd)
        gs_set_crt((int)a0, (int)a1, (int)a2);
        return 0;
    case 0x04:  // Exit
        LOG(LOG_CORE, "Exit(%d)", (int)a0);
        if (a0) cpu_dump_state();
        exit((int)a0);
    case 0x06:  // LoadExecPS2
    case 0x07: {  // ExecPS2
        char name[256];
        ee_read_string(a0, name, sizeof name);
        fatal("game tried to execute another program (%s)", name);
    }
    case 0x10:  // AddIntcHandler(cause, handler, next, arg)
        if (a0 >= 15) return -1;
        LOG(LOG_KERNEL, "AddIntcHandler(%d, %08x, %d, %08x)", a0, a1, (int)a2, a3);
        return add_handler(g_intc_handlers[a0], a1, (int)a2, a3);
    case 0x11:  // RemoveIntcHandler(cause, id)
        return remove_handler(g_intc_handlers, 15, (int)a1);
    case 0x12:  // AddDmacHandler(channel, handler, next, arg)
        if (a0 >= 16) return -1;
        LOG(LOG_KERNEL, "AddDmacHandler(%d, %08x, %d, %08x)", a0, a1, (int)a2, a3);
        return add_handler(g_dmac_handlers[a0], a1, (int)a2, a3);
    case 0x13:  // RemoveDmacHandler
        return remove_handler(g_dmac_handlers, 16, (int)a1);
    case 0x14:  // _EnableIntc
    case 0x1A: {
        uint32_t was = (g_intc_mask >> a0) & 1;
        g_intc_mask |= 1u << a0;
        if (g_intc_new.load() & (1u << a0)) g_ee_irq_pending = 1;
        return (int)was;
    }
    case 0x15:  // _DisableIntc
    case 0x1B: {
        uint32_t was = (g_intc_mask >> a0) & 1;
        g_intc_mask &= ~(1u << a0);
        return (int)was;
    }
    case 0x16:  // _EnableDmac
    case 0x1C:
        return hw_dmac_set_channel_irq(a0, true);
    case 0x17:  // _DisableDmac
    case 0x1D:
        return hw_dmac_set_channel_irq(a0, false);
    case 0x18:  // SetAlarm(time, handler, arg)
    case 0x1E:
        return sys_set_alarm(a0, a1, a2);
    case 0x19:  // ReleaseAlarm(id)
    case 0x1F:
        if (a0 < 64 && g_alarms[a0].used) {
            g_alarms[a0].used = false;
            return 0;
        }
        return -1;
    case 0x20:
        return sys_create_thread(a0);
    case 0x21:
        return sys_delete_thread((int)a0);
    case 0x22:
        return sys_start_thread((int)a0, a1);
    case 0x23:  // ExitThread
    case 0x24:  // ExitDeleteThread
        LOG(LOG_THREAD, "ExitThread %d", g_cur->id);
        g_cur->status = THS_DORMANT;
        g_cur->parked = false;  // StartThread will give it a fresh fiber
        if (num == 0x24) {
            g_cur->used = false;
            g_zombies.push_back(g_cur->fiber);
            g_cur->fiber = nullptr;
        }
        reschedule();
        fatal("exited thread %d resumed", g_cur->id);
    case 0x25:
    case 0x26:
        return sys_terminate_thread((int)a0);
    case 0x27:  // DisableDispatchThread
        g_dispatch_disabled = true;
        return 0;
    case 0x28:
        g_dispatch_disabled = false;
        reschedule();
        return 0;
    case 0x29:
    case 0x2A:
        return sys_change_priority((int)a0, (int)a1);
    case 0x2B:
    case 0x2C:
        return sys_rotate_ready_queue((int)a0);
    case 0x2D:
    case 0x2E:
        return sys_release_wait((int)a0);
    case 0x2F:  // GetThreadId
        return g_cur->id;
    case 0x30:
    case 0x31:
        return sys_refer_thread_status((int)a0, a1);
    case 0x32:
        return sys_sleep_thread();
    case 0x33:
    case 0x34:
        return sys_wakeup_thread((int)a0);
    case 0x35:
    case 0x36: {  // CancelWakeupThread
        Thread *t = a0 == 0 ? g_cur : thread_by_id((int)a0);
        if (!t) return -1;
        int c = t->wakeup_count;
        t->wakeup_count = 0;
        return c;
    }
    case 0x37:
    case 0x38:
        return sys_suspend_thread((int)a0);
    case 0x39:
    case 0x3A:
        return sys_resume_thread((int)a0);
    case 0x3C:
        sys_setup_thread(a0, a1, a2, a3, arg_u32(4));
        return g_ee.r[2].sw[0];
    case 0x3D:  // SetupHeap(start, size)
        g_heap_start = a0;
        g_heap_end = (a1 == 0xFFFFFFFF) ? g_main_stack : a0 + a1;
        LOG(LOG_KERNEL, "SetupHeap %08x-%08x", g_heap_start, g_heap_end);
        return (int)g_heap_end;
    case 0x3E:  // EndOfHeap
        return (int)g_heap_end;
    case 0x40:
        return sys_create_sema(a0);
    case 0x41:
        return sys_delete_sema((int)a0);
    case 0x42:
    case 0x43:
        return sys_signal_sema((int)a0);
    case 0x44:
        return sys_wait_sema((int)a0);
    case 0x45:
    case 0x46:
        return sys_poll_sema((int)a0);
    case 0x47:
    case 0x48:
        return sys_refer_sema((int)a0, a1);
    case 0x4A:  // SetOsdConfigParam
        g_osd_config = ee_r32(a0);
        return 0;
    case 0x4B:  // GetOsdConfigParam: English, 4:3, ...
        ee_w32(a0, g_osd_config);
        return 0;
    case 0x4C:
    case 0x4D:
        return 0;
    case 0x4E:
    case 0x4F:
        return 0;
    case 0x54:  // TLB management (libkernel InitTLB). The HLE memory map is
    case 0x55:  // fixed, so these are accepted and ignored.
    case 0x57:
    case 0x58:
    case 0x59:
    case 0x82:
        return 0;
    case 0x56:  // PutTLBEntry(index, ...)
        return (int)a0;
    case 0xFC:  // SetAlarm / iSetAlarm (installed by libkernel's alarm patch)
    case 0xFD:
        return sys_set_alarm(a0, a1, a2);
    case 0xFE:  // ReleaseAlarm / iReleaseAlarm
    case 0xFF:
        if (a0 < 64 && g_alarms[a0].used) {
            g_alarms[a0].used = false;
            return 0;
        }
        return -1;
    case 0x0D:  // SetVTLBRefillHandler
    case 0x0E:  // SetVCommonHandler (game crash screen) - exceptions are fatal here
    case 0x0F:  // SetVInterruptHandler
        return 0;
    case 0x6B:  // SifStopDma (IOP reboot sequence)
        return 0;
    case 0x64:  // FlushCache
    case 0x68:  // iFlushCache
        return 0;
    case 0x70:  // GsGetIMR
        return (int)gs_get_imr();
    case 0x71:  // GsPutIMR
        gs_put_imr(a0);
        return 0;
    case 0x73:  // SetVSyncFlag(flag, count)
        g_vsync_flag_addr = a0;
        g_vsync_flag_cnt_addr = a1;
        return 0;
    case 0x74:  // SetSyscall(num, addr)
        LOG(LOG_KERNEL, "SetSyscall(%d, %08x)", (int)a0, a1);
        if (a0 < 256) {
            g_syscall_override[a0] = a1;
            // libkernel locates the kernel's syscall table by searching
            // kernel memory for these entries, so keep a real table there.
            ee_w32(kSyscallTable + a0 * 4, a1);
        }
        return 0;
    case 0x75: {  // _print
        char buf[512];
        ee_read_string(a0, buf, sizeof buf);
        LOG(LOG_GAME, "%s", buf);
        return 0;
    }
    case 0x76:  // SifDmaStat
        return sif_dma_stat((int)a0);
    case 0x77:  // SifSetDma(list, count)
        return (int)sif_set_dma(a0, (int)a1);
    case 0x78:  // SifSetDChain
        sif_set_dchain();
        return 0;
    case 0x79:  // SifSetReg
        sif_set_reg(a0, a1);
        return 0;
    case 0x7A:  // SifGetReg
        return (int)sif_get_reg(a0);
    case 0x7B:  // ExecOSD
        LOG(LOG_CORE, "ExecOSD - exiting");
        app_exit(0);
    case 0x7C:  // Deci2Call
        return -1;
    case 0x7D:  // PSMode
        return 0;
    case 0x7E:  // MachineType
        return 0;
    case 0x7F:  // GetMemorySize
        return (int)EE_RAM_SIZE;
    default:
        break;
    }
    if (num < 256 && g_syscall_override[num] && ee_lookup_func(g_syscall_override[num])) {
        // a kernel-mode routine installed by the game via SetSyscall (only
        // routines that live in the recompiled program can run; libkernel's
        // kernel patches copied to kernel RAM are replaced by the HLE above)
        LOG(LOG_KERNEL, "syscall %d -> installed handler %08x", num, g_syscall_override[num]);
        uint32_t ra = g_ee.r[31].uw[0];
        ee_call(g_syscall_override[num]);
        g_ee.r[31].sd[0] = (int32_t)ra;
        return g_ee.r[2].sw[0];
    }
    if (num < 256 && g_syscall_override[num]) {
        LOG(LOG_KERNEL, "syscall %d: kernel patch at %08x not executed", num, g_syscall_override[num]);
        return 0;
    }
    WARN("unimplemented syscall %d (0x%x) a0=%08x a1=%08x a2=%08x", i_variant ? -num : num, num, a0, a1, a2);
    return 0;
}

}  // namespace

// ------------------------------------------------------------ public API
extern "C" void ee_syscall(uint32_t pc) {
    g_ee.pc = pc;
    int num = g_ee.r[3].sw[0];
    LOG(LOG_KERNEL, "syscall %d (0x%x) a0=%08x a1=%08x a2=%08x from %08x", num, num < 0 ? -num : num,
        arg_u32(0), arg_u32(1), arg_u32(2), g_ee.r[31].uw[0]);
    int ret = handle_syscall(num);
    g_ee.r[2].sd[0] = ret;
    if (g_ee_irq_pending && g_ints_enabled && !g_in_interrupt) ee_service_interrupts();
}

extern "C" void ee_service_interrupts(void) {
    if (g_in_interrupt) return;
    if (!g_ints_enabled) {
        // code polling INTC_STAT with interrupts disabled (libgraph's vsync
        // wait) must still see vblanks; EI re-arms dispatch for what latched
        g_ee_irq_pending = 0;
        service_events(false);
        return;
    }
    g_ee_irq_pending = 0;
    g_in_interrupt = true;
    service_events();
    g_in_interrupt = false;
    if (g_intc_new.load() || g_dmac_pending.load()) dispatch_interrupts();
    // anything raised while dispatching gets picked up next time
    if (g_intc_new.load() & g_intc_mask) g_ee_irq_pending = 1;
    if (g_dmac_pending.load()) g_ee_irq_pending = 1;
    if (g_resched_pending) {
        g_resched_pending = false;
        if (g_cur && !g_dispatch_disabled) {
            Thread *n = pick_next();
            if (n && n != g_cur) {
                LOG(LOG_THREAD, "preempt %d by %d", g_cur->id, n->id);
                switch_to(n);
            } else if (!n || g_cur->status != THS_RUN) {
                // we are inside idle_until_runnable's caller chain; it re-picks
            }
        }
    }
}

// A loop polling memory that only interrupt handlers change: rather than
// spin, sleep until the next timed event (vblank, alarm) or an interrupt
// raised by another host thread, then service it.
extern "C" void ee_idle_poll(void) {
    if (!g_ee_irq_pending && !g_in_interrupt && g_ints_enabled) {
        uint64_t now = platform_time_us();
        uint64_t next = g_vblank_end_pending ? g_vblank_end_us : g_next_vblank_us;
        for (auto &a : g_alarms)
            if (a.used && a.due_us < next) next = a.due_us;
        if (next > now && !(g_intc_new.load() & g_intc_mask) && !g_dmac_pending.load()) {
            uint64_t d = next - now;
            ee_sleep_us((uint32_t)(d > 500 ? 500 : d));
        }
    }
    ee_service_interrupts();
}

void intc_raise(int cause) {
    g_intc_stat |= 1u << cause;
    g_intc_new |= 1u << cause;
    g_ee_irq_pending = 1;
}

void dmac_raise(int channel) {
    g_dmac_pending |= 1u << channel;
    g_ee_irq_pending = 1;
}

uint32_t intc_stat() { return g_intc_stat.load(); }
uint32_t intc_mask() { return g_intc_mask; }
void intc_write_stat(uint32_t v) {
    g_intc_stat &= ~v;
    // an acknowledge cancels events the CPU could not have taken yet; enabled
    // ones would already have run their handlers on hardware
    uint32_t cancel = g_ints_enabled ? (v & ~g_intc_mask) : v;
    g_intc_new &= ~cancel;
}
void intc_write_mask(uint32_t v) {
    g_intc_mask ^= v;
    if (g_intc_new.load() & g_intc_mask) g_ee_irq_pending = 1;
}

bool kernel_interrupts_enabled() { return g_ints_enabled; }
void kernel_set_interrupts_enabled(bool on) {
    g_ints_enabled = on;
    if (on && (g_intc_new.load() || g_dmac_pending.load())) g_ee_irq_pending = 1;
}
bool kernel_in_interrupt() { return g_in_interrupt; }
uint64_t kernel_vblank_count() { return g_vblank_count; }

bool kernel_call_trampoline(uint32_t addr) {
    if (addr == kThreadExitAddr) {
        // the code jumped to the fake return address: thread function returned
        g_ee.r[3].sd[0] = 0x23;
        ee_syscall(0);
        return true;
    }
    return false;
}

void kernel_yield() {
    // Let interrupts/other threads run (used by HLE busy waits).
    bool saved = g_ints_enabled;
    g_ints_enabled = true;
    g_ee_irq_pending = 1;
    ee_service_interrupts();
    g_ints_enabled = saved;
    ee_sleep_us(100);
}

void kernel_dump_threads() {
    for (auto &t : g_threads) {
        if (!t.used) continue;
        log_write(LOG_CORE, "thread %d%s status=%x prio=%d entry=%08x wait=%d/%d pc~ra=%08x", t.id,
                  &t == g_cur ? "*" : " ", t.status, t.prio, t.entry, t.wait_type, t.wait_id,
                  (&t == g_cur ? g_ee : t.regs).r[31].uw[0]);
    }
}

void kernel_init(int, const char *const *) {
    HostFiber *main_fiber = fiber_init_main();
    Thread &t = g_threads[1];
    t = Thread();
    t.used = true;
    t.id = 1;
    t.status = THS_RUN;
    t.prio = t.init_prio = 0;
    t.fiber = main_fiber;
    t.parked = false;
    g_cur = &t;
    g_intc_mask = 0;
    platform_start_ticker(ticker_cb, 1000);
}

void kernel_run(uint32_t entry) {
    prof_thread_begin(P_EE_GAME);
    g_threads[1].entry = entry;
    memset(&g_ee, 0, sizeof g_ee);
    g_ints_enabled = false;
    ee_call(entry);
    LOG(LOG_CORE, "entry point returned");
}
