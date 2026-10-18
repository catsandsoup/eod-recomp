// platform_win32.cpp - Windows backend (Windows 7 and later).
//
// Uses only components that ship with Windows 7: GDI for presentation,
// waveOut for audio, XInput (loaded dynamically, optional) and the
// keyboard for input, fibers for EE threads.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <atomic>
#include <windows.h>
#include <mmsystem.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../core/config.h"
#include "../../ui/overlay.h"
#include "../platform.h"
#include "../pad_script.h"

void build_key_map();

namespace {

PlatformConfig g_cfg;
std::atomic<int> g_aspect{0};  // display aspect (platform_set_aspect)
HWND g_hwnd;
bool g_quit;
bool g_fullscreen;
WINDOWPLACEMENT g_prev_placement = {sizeof(WINDOWPLACEMENT)};
std::vector<uint32_t> g_bgra;
int g_fw = 640, g_fh = 448;
LARGE_INTEGER g_qpf;
bool g_keys[256];

// ------------------------------------------------------------- XInput
struct XINPUT_GAMEPAD_ {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
struct XINPUT_STATE_ {
    DWORD dwPacketNumber;
    XINPUT_GAMEPAD_ Gamepad;
};
struct XINPUT_VIBRATION_ {
    WORD wLeftMotorSpeed, wRightMotorSpeed;
};
typedef DWORD(WINAPI *XInputGetState_t)(DWORD, XINPUT_STATE_ *);
typedef DWORD(WINAPI *XInputSetState_t)(DWORD, XINPUT_VIBRATION_ *);
XInputGetState_t pXInputGetState;
XInputSetState_t pXInputSetState;

void load_xinput() {
    const char *dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (const char *d : dlls) {
        HMODULE m = LoadLibraryA(d);
        if (!m) continue;
        pXInputGetState = (XInputGetState_t)GetProcAddress(m, "XInputGetState");
        pXInputSetState = (XInputSetState_t)GetProcAddress(m, "XInputSetState");
        if (pXInputGetState) return;
    }
}

void toggle_fullscreen() {
    DWORD style = GetWindowLong(g_hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi = {sizeof mi};
        if (GetWindowPlacement(g_hwnd, &g_prev_placement) &&
            GetMonitorInfo(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLong(g_hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_fullscreen = true;
        }
    } else {
        SetWindowLong(g_hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_hwnd, &g_prev_placement);
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = false;
    }
}

void paint(HDC dc) {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (g_bgra.empty() || cw <= 0 || ch <= 0) return;
    int vx, vy, vw, vh;
    platform_display_rect(cw, ch, &vx, &vy, &vw, &vh);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = g_fw;
    bmi.bmiHeader.biHeight = -g_fh;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, vx, vy, vw, vh, 0, 0, g_fw, g_fh, g_bgra.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
    // black bars
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RECT r;
    if (vx > 0) {
        SetRect(&r, 0, 0, vx, ch);
        FillRect(dc, &r, black);
        SetRect(&r, vx + vw, 0, cw, ch);
        FillRect(dc, &r, black);
    }
    if (vy > 0) {
        SetRect(&r, 0, 0, cw, vy);
        FillRect(dc, &r, black);
        SetRect(&r, 0, vy + vh, cw, ch);
        FillRect(dc, &r, black);
    }
}

LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (overlay_wndproc(h, msg, wp, lp)) return 0;  // the in-game menu
    switch (msg) {
    case WM_CLOSE:
        g_quit = true;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) {  // Alt+Enter
            toggle_fullscreen();
            return 0;
        }
        if (wp == VK_F4) break;
        g_keys[wp & 255] = true;
        return 0;
    case WM_SYSKEYUP:
        g_keys[wp & 255] = false;
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_F11) toggle_fullscreen();
        g_keys[wp & 255] = true;
        return 0;
    case WM_KEYUP:
        g_keys[wp & 255] = false;
        return 0;
    case WM_KILLFOCUS:
        memset(g_keys, 0, sizeof g_keys);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

// ------------------------------------------------------------- audio
HWAVEOUT g_wave;
constexpr int kAudioBuffers = 8;
constexpr int kAudioChunk = 1024;  // frames per buffer
WAVEHDR g_hdr[kAudioBuffers];
std::vector<int16_t> g_abuf[kAudioBuffers];
std::vector<int16_t> g_pending;
CRITICAL_SECTION g_audio_cs;

void audio_pump() {
    if (!g_wave) return;
    for (int i = 0; i < kAudioBuffers; i++) {
        WAVEHDR &h = g_hdr[i];
        if ((h.dwFlags & WHDR_PREPARED) && !(h.dwFlags & WHDR_DONE)) continue;
        if (g_pending.size() < (size_t)kAudioChunk * 2) return;
        if (h.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(g_wave, &h, sizeof h);
        memcpy(g_abuf[i].data(), g_pending.data(), kAudioChunk * 4);
        g_pending.erase(g_pending.begin(), g_pending.begin() + kAudioChunk * 2);
        memset(&h, 0, sizeof h);
        h.lpData = reinterpret_cast<LPSTR>(g_abuf[i].data());
        h.dwBufferLength = kAudioChunk * 4;
        waveOutPrepareHeader(g_wave, &h, sizeof h);
        waveOutWrite(g_wave, &h, sizeof h);
    }
}

// ------------------------------------------------------------- ticker
void (*g_tick_cb)();
DWORD WINAPI ticker_thread(LPVOID p) {
    unsigned period = (unsigned)(uintptr_t)p;
    DWORD ms = period / 1000;
    if (ms == 0) ms = 1;
    for (;;) {
        Sleep(ms);
        g_tick_cb();
    }
}

}  // namespace

// ================================================================= API
bool platform_init(const PlatformConfig &cfg) {
    g_cfg = cfg;
    g_aspect = cfg.aspect;
    QueryPerformanceFrequency(&g_qpf);
    timeBeginPeriod(1);
    load_xinput();
    if (cfg.headless) return true;
    WNDCLASSA wc = {};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.lpszClassName = "EodWindow";
    RegisterClassA(&wc);
    build_key_map();
    RECT r = {0, 0, cfg.width > 0 ? cfg.width : 1280, cfg.height > 0 ? cfg.height : 960};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowA("EodWindow", cfg.title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) return false;
    ShowWindow(g_hwnd, SW_SHOW);
    if (cfg.fullscreen) toggle_fullscreen();
    UpdateWindow(g_hwnd);
    return true;
}

void platform_shutdown() {
    if (g_wave) {
        waveOutReset(g_wave);
        waveOutClose(g_wave);
        g_wave = nullptr;
    }
    timeEndPeriod(1);
}

void platform_fatal_message(const char *msg) {
    MessageBoxA(g_hwnd, msg, "Eve of Destruction - fatal error", MB_OK | MB_ICONERROR);
}

bool platform_pump() {
    MSG m;
    while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    if (g_wave) {
        EnterCriticalSection(&g_audio_cs);
        audio_pump();
        LeaveCriticalSection(&g_audio_cs);
    }
    return !g_quit;
}

void platform_present(const uint32_t *pixels, int width, int height, int stride) {
    if (!g_hwnd) return;
    g_fw = width;
    g_fh = height;
    g_bgra.resize((size_t)width * height);
    for (int y = 0; y < height; y++) {
        const uint32_t *s = pixels + (size_t)y * stride;
        uint32_t *d = &g_bgra[(size_t)y * width];
        for (int x = 0; x < width; x++) {
            uint32_t p = s[x];
            d[x] = ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF);
        }
    }
    HDC dc = GetDC(g_hwnd);
    paint(dc);
    ReleaseDC(g_hwnd, dc);
}

uint64_t platform_time_us() {
    LARGE_INTEGER c;
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);  // (replays run without platform_init)
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / (double)g_qpf.QuadPart * 1000000.0);
}

int platform_cpu_count() {
    DWORD_PTR proc = 0, sys = 0;
    int n = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys))
        for (; proc; proc &= proc - 1) n++;
    if (n <= 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        n = (int)si.dwNumberOfProcessors;
    }
    return n > 0 ? n : 1;
}

int platform_core_count() {
    // cores with at least one of our processors (GetLogicalProcessorInformation: XP SP3 and later)
    DWORD_PTR proc = 0, sys = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys)) proc = ~(DWORD_PTR)0;
    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) + 1);
    int n = 0;
    if (len && GetLogicalProcessorInformation(info.data(), &len)) {
        for (DWORD i = 0; i < len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION); i++)
            if (info[i].Relationship == RelationProcessorCore && (info[i].ProcessorMask & proc)) n++;
    }
    int cpus = platform_cpu_count();
    return n > 0 && n <= cpus ? n : cpus;
}

void platform_sleep_us(uint32_t us) {
    if (us >= 1000)
        Sleep(us / 1000);
    else
        Sleep(0);
}

// ------------------------------------------------------------------ input
static inline bool key(int vk) { return g_keys[vk & 255]; }

namespace {
struct KeyBind {
    int vk;
    uint16_t btn;
};
std::vector<KeyBind> g_key_map;
const uint16_t kPadBits[KEY_COUNT] = {PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_CROSS, PAD_CIRCLE, PAD_SQUARE,
                                      PAD_TRIANGLE, PAD_L1, PAD_R1, PAD_L2, PAD_R2, PAD_L3, PAD_R3, PAD_START,
                                      PAD_SELECT};
}  // namespace

// keyboard bindings from eod.ini ([Input], comma separated key names)
void build_key_map() {
    g_key_map.clear();
    for (int i = 0; i < KEY_COUNT; i++) {
        const std::string &list = g_config.keys[i];
        size_t p = 0;
        while (p <= list.size()) {
            size_t c = list.find(',', p);
            if (c == std::string::npos) c = list.size();
            int vk = key_code(list.substr(p, c - p));
            if (vk) g_key_map.push_back({vk, kPadBits[i]});
            p = c + 1;
        }
    }
}

void platform_get_pad(int port, HostPadState *out) {
    *out = HostPadState();
    if (port == 0) {
        // keyboard (always available on port 0)
        out->connected = true;
        for (const KeyBind &m : g_key_map)
            if (key(m.vk)) out->buttons |= m.btn;
        // the direction keys also move the left stick (steering)
        if (out->buttons & PAD_LEFT) out->lx = 0;
        if (out->buttons & PAD_RIGHT) out->lx = 255;
        if (out->buttons & PAD_UP) out->ly = 0;
        if (out->buttons & PAD_DOWN) out->ly = 255;
    }
    HostPadState c;
    platform_get_controller(port, &c);
    if (c.connected) {
        out->connected = true;
        out->buttons |= c.buttons;
        if (c.lx != 128) out->lx = c.lx;  // (the stick outside its dead zone)
        if (c.ly != 128) out->ly = c.ly;
        out->rx = c.rx;
        out->ry = c.ry;
        out->pressure[10] = c.pressure[10];
        out->pressure[11] = c.pressure[11];
    }
    // the in-game menu has the input
    if (overlay_menu_open()) {
        *out = HostPadState();
        out->connected = true;
    }
    // EOD_PAD_SCRIPT (automated testing) replaces port 0's input
    uint16_t sb;
    if (port == 0 && pad_script_buttons(sb)) {
        *out = HostPadState();
        out->connected = true;
        out->buttons = sb;
    }
    // pressure for digital buttons
    static const uint16_t order[12] = {PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
                                       PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2};
    for (int i = 0; i < 12; i++)
        if ((out->buttons & order[i]) && out->pressure[i] == 0) out->pressure[i] = 255;
}

void platform_get_controller(int port, HostPadState *out) {
    // (asked by the game and by the in-game menu every vblank: XInput is read once)
    static HostPadState last[4];
    static uint64_t when[4];
    uint64_t now = platform_time_us();
    if (port >= 0 && port < 4 && when[port] && now - when[port] < 4000) {
        *out = last[port];
        return;
    }
    *out = HostPadState();
    if (pXInputGetState) {
        XINPUT_STATE_ xs;
        if (pXInputGetState((DWORD)port, &xs) == 0) {
            out->connected = true;
            WORD b = xs.Gamepad.wButtons;
            struct XMap {
                WORD x;
                uint16_t btn;
            } xm[] = {
                {0x0001, PAD_UP},     {0x0002, PAD_DOWN},  {0x0004, PAD_LEFT},     {0x0008, PAD_RIGHT},
                {0x0010, PAD_START},  {0x0020, PAD_SELECT}, {0x0040, PAD_L3},      {0x0080, PAD_R3},
                {0x0100, PAD_L1},     {0x0200, PAD_R1},    {0x1000, PAD_CROSS},    {0x2000, PAD_CIRCLE},
                {0x4000, PAD_SQUARE}, {0x8000, PAD_TRIANGLE},
            };
            for (const XMap &m : xm)
                if (b & m.x) out->buttons |= m.btn;
            if (xs.Gamepad.bLeftTrigger > 30) out->buttons |= PAD_L2;
            if (xs.Gamepad.bRightTrigger > 30) out->buttons |= PAD_R2;
            auto axis = [](SHORT v, bool invert) {
                int a = (v + 32768) >> 8;
                if (invert) a = 255 - a;
                return (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a);
            };
            uint8_t lx = axis(xs.Gamepad.sThumbLX, false), ly = axis(xs.Gamepad.sThumbLY, true);
            if (lx < 112 || lx > 144) out->lx = lx;
            if (ly < 112 || ly > 144) out->ly = ly;
            out->rx = axis(xs.Gamepad.sThumbRX, false);
            out->ry = axis(xs.Gamepad.sThumbRY, true);
            out->pressure[10] = xs.Gamepad.bLeftTrigger;
            out->pressure[11] = xs.Gamepad.bRightTrigger;
        }
    }
    if (port >= 0 && port < 4) {
        last[port] = *out;
        when[port] = now;
    }
}

void platform_set_aspect(int aspect) { g_aspect = aspect; }

void platform_display_rect(int cw, int ch, int *x, int *y, int *w, int *h) {
    int vw = cw, vh = ch, aspect = g_aspect;
    if (aspect != 2) {  // 4:3 or 16:9, letterboxed
        int an = aspect == 1 ? 16 : 4, ad = aspect == 1 ? 9 : 3;
        vh = cw * ad / an;
        if (vh > ch) {
            vh = ch;
            vw = ch * an / ad;
        }
    }
    *x = (cw - vw) / 2;
    *y = (ch - vh) / 2;
    *w = vw;
    *h = vh;
}

void *platform_native_window() { return g_hwnd; }

void platform_set_fullscreen(bool on) {
    if (g_hwnd && on != g_fullscreen) toggle_fullscreen();
}

bool platform_fullscreen() { return g_fullscreen; }

void platform_set_status(const char *text) {
    if (!g_hwnd) return;
    char buf[256];
    snprintf(buf, sizeof buf, "%s - %s", g_cfg.title, text);
    SetWindowTextA(g_hwnd, buf);
}

void platform_show_hud_banner(const char *text, int duration_ms) {
    (void)duration_ms;
    platform_set_status(text);
}

void platform_set_mod_overlay(bool visible) {
    (void)visible;
}

static uint64_t g_haptic_pulse_until = 0;

void platform_haptic_pulse(int port, int duration_ms, uint8_t small_motor, uint8_t large_motor) {
    if (!pXInputSetState || !g_config.vibration) return;
    g_haptic_pulse_until = platform_time_us() + (uint64_t)duration_ms * 1000;
    XINPUT_VIBRATION_ v;
    v.wLeftMotorSpeed = (WORD)(large_motor * 257);
    v.wRightMotorSpeed = small_motor ? 0xFFFF : 0;
    pXInputSetState((DWORD)port, &v);
}

void platform_set_vibration(int port, uint8_t small_motor, uint8_t large_motor) {
    if (!pXInputSetState) return;
    uint64_t now_us = platform_time_us();
    if (now_us < g_haptic_pulse_until) return; // protect timed haptic pulse
    if (!g_config.vibration) small_motor = large_motor = 0;
    XINPUT_VIBRATION_ v;
    v.wLeftMotorSpeed = (WORD)(large_motor * 257);
    v.wRightMotorSpeed = small_motor ? 0xFFFF : 0;
    pXInputSetState((DWORD)port, &v);
}

void platform_audio_feedback(bool enabled) {
    (void)enabled;
}

// ------------------------------------------------------------------ audio
bool platform_audio_open(int sample_rate) {
    InitializeCriticalSection(&g_audio_cs);
    WAVEFORMATEX wf = {};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = (DWORD)sample_rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * 4;
    if (waveOutOpen(&g_wave, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        g_wave = nullptr;
        return false;
    }
    for (int i = 0; i < kAudioBuffers; i++) {
        g_abuf[i].assign(kAudioChunk * 2, 0);
        memset(&g_hdr[i], 0, sizeof(WAVEHDR));
    }
    return true;
}

void platform_audio_submit(const int16_t *samples, int frames) {
    if (!g_wave) return;
    EnterCriticalSection(&g_audio_cs);
    g_pending.insert(g_pending.end(), samples, samples + frames * 2);
    // avoid unbounded latency if the game produces audio faster than real time
    if (g_pending.size() > (size_t)48000 * 2 / 4) g_pending.erase(g_pending.begin(), g_pending.end() - 48000 * 2 / 8);
    audio_pump();
    LeaveCriticalSection(&g_audio_cs);
}

int platform_audio_queued_frames() {
    if (!g_wave) return 0;
    int n = (int)(g_pending.size() / 2);
    for (int i = 0; i < kAudioBuffers; i++)
        if ((g_hdr[i].dwFlags & WHDR_PREPARED) && !(g_hdr[i].dwFlags & WHDR_DONE)) n += kAudioChunk;
    return n;
}

// ----------------------------------------------------------------- fibers
struct HostFiber {
    LPVOID fiber;
    void (*fn)(void *);
    void *arg;
};

static VOID CALLBACK fiber_start(LPVOID p) {
    HostFiber *f = static_cast<HostFiber *>(p);
    f->fn(f->arg);
    platform_fatal_message("fiber returned");
    ExitProcess(1);
}

static HostFiber *g_cur_fiber;

HostFiber *fiber_init_main() {
    HostFiber *f = new HostFiber();
    f->fiber = ConvertThreadToFiber(nullptr);
    g_cur_fiber = f;
    return f;
}

HostFiber *fiber_create(size_t stack_size, void (*fn)(void *), void *arg) {
    HostFiber *f = new HostFiber();
    f->fn = fn;
    f->arg = arg;
    f->fiber = CreateFiberEx(64 * 1024, stack_size, 0, fiber_start, f);
    if (!f->fiber) {
        platform_fatal_message("CreateFiber failed");
        ExitProcess(1);
    }
    return f;
}

void fiber_switch(HostFiber *to) {
    if (to == g_cur_fiber) return;
    g_cur_fiber = to;
    SwitchToFiber(to->fiber);
}

HostFiber *fiber_current() { return g_cur_fiber; }

void fiber_delete(HostFiber *f) {
    if (f->fiber) DeleteFiber(f->fiber);
    delete f;
}

void platform_start_ticker(void (*cb)(), unsigned period_us) {
    g_tick_cb = cb;
    HANDLE t = CreateThread(nullptr, 0, ticker_thread, (LPVOID)(uintptr_t)period_us, 0, nullptr);
    if (t) {
        SetThreadPriority(t, THREAD_PRIORITY_TIME_CRITICAL);
        CloseHandle(t);
    }
}

// ------------------------------------------------------------------ files
bool platform_find_file(const char *root, const char *rel, char *out, size_t out_size) {
    std::string p = root;
    p += "\\";
    for (const char *c = rel; *c; c++) p += (*c == '/') ? '\\' : *c;
    DWORD a = GetFileAttributesA(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return false;
    snprintf(out, out_size, "%s", p.c_str());
    return true;
}

bool platform_mkdir(const char *path) { return CreateDirectoryA(path, nullptr) != 0; }

bool platform_list_dir(const char *path, std::vector<DirEntryInfo> &out) {
    std::string pat = std::string(path) + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        DirEntryInfo info = {};
        snprintf(info.name, sizeof info.name, "%s", fd.cFileName);
        info.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        info.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        out.push_back(info);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return true;
}
