#include "../core/host_file.h"
#include "custom_music.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include "../core/log.h"
#include "music_core.h"
#include "music_decode.h"

#ifdef __APPLE__
std::string mac_app_support();  // runtime/app/macos_app.mm
#endif

namespace fs = std::filesystem;

namespace custom_music {
namespace {

constexpr uint64_t kMaxFrames = 48000ull * 60 * 20;  // 20 minutes

struct Lib {
    std::mutex m;
    std::string root, cache;
    std::vector<Song> songs;
    std::vector<std::string> keys;  // cache key per song
    std::set<std::string> disabled;
    Progress prog;
    music::ShuffleBag bag;
    // worker
    std::thread scan_thread, load_thread;
    std::condition_variable cv;
    bool want_scan = false, quit = false, inited = false;
    // load jobs
    struct Job { int slot; uint32_t gen; uint32_t id; };
    std::vector<Job> jobs;
} g;

struct Pending {
    uint32_t gen = 0;
    bool valid = false, failed = false;
    std::shared_ptr<std::vector<int16_t>> data;
    float gain = 1;
};
struct Slot {  // EE-thread fields, plus `pending` shared with the loader under g.m
    uint32_t gen = 0;
    music::HostVoice voice;
    std::shared_ptr<std::vector<int16_t>> data;
    bool started = false, loaded = false, failed = false;
    Pending pending;
};
Slot g_slot[2];

std::string cache_path(const std::string &key) { return g.cache + "/" + key + ".pcm"; }

std::string make_key(const fs::path &p, uint64_t size, int64_t mtime) {
    char b[40];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)music::fnv1a(p.u8string() + "|" + std::to_string(size) + "|" + std::to_string(mtime)));
    return b;
}

void save_state_locked() {
    std::vector<std::string> d(g.disabled.begin(), g.disabled.end());
    std::error_code ec;
    fs::create_directories(fs::u8path(g.root), ec);
    const std::string tmp = g.root + "/state.json.tmp", fin = g.root + "/state.json";
    {
        std::ofstream o(fs::u8path(tmp), std::ios::binary);
        o << music::state_to_json(d);
    }
    fs::rename(fs::u8path(tmp), fs::u8path(fin), ec);
}

bool read_header(const std::string &path, music::CacheHeader &h) {
    FILE *f = host_fopen(path.c_str(), "rb");
    if (!f) return false;
    const bool ok = fread(&h, sizeof h, 1, f) == 1 && music::cache_header_ok(h);
    fclose(f);
    return ok;
}

// Decodes one file into the cache (or finds it there). Updates `s` (state/reason/length/gain).
void process(Song &local, const fs::path &file, const std::string &key) {
    music::CacheHeader h;
    if (read_header(cache_path(key), h)) {
        local.state = State::Ready;
        local.seconds = (double)h.frames / 48000.0;
        local.gain = h.gain;
        return;
    }
    const std::string tmp = cache_path(key) + ".tmp";
    FILE *out = host_fopen(tmp.c_str(), "wb");
    if (!out) { local.state = State::Skipped; local.reason = "Cannot write the music cache"; return; }
    music::CacheHeader hd = {};
    fwrite(&hd, sizeof hd, 1, out);
    music::LoudnessMeter meter;
    uint64_t frames = 0;
    bool too_long = false;
    music::DecodeResult r = music::decode_file(file.u8string(), [&](const int16_t *s, size_t n) {
        if (frames + n > kMaxFrames) { too_long = true; return false; }
        meter.add(s, n);
        frames += n;
        return fwrite(s, 4, n, out) == n;
    });
    if (r.status == music::DecodeStatus::Ok && frames > 0) {
        hd = {};
        memcpy(hd.magic, "TDM1", 4);
        hd.frames = frames;
        hd.gain = meter.gain();
        hd.rms = meter.rms();
        fseek(out, 0, SEEK_SET);
        fwrite(&hd, sizeof hd, 1, out);
        fclose(out);
        std::error_code ec;
        fs::rename(fs::u8path(tmp), fs::u8path(cache_path(key)), ec);
        if (ec) { local.state = State::Skipped; local.reason = "Cannot write the music cache"; return; }
        local.state = State::Ready;
        local.seconds = (double)frames / 48000.0;
        local.gain = hd.gain;
        return;
    }
    fclose(out);
    std::error_code ec;
    fs::remove(fs::u8path(tmp), ec);
    local.state = r.status == music::DecodeStatus::Protected ? State::Protected : State::Skipped;
    local.reason = too_long ? "Longer than 20 minutes" : r.reason.empty() ? "Cannot be decoded" : r.reason;
}

void scan_pass() {
    std::string root, cache;
    {
        std::lock_guard<std::mutex> lk(g.m);
        root = g.root;
        cache = g.cache;
        g.prog = {0, 0, true};
    }
    std::error_code ec;
    fs::create_directories(fs::u8path(cache), ec);
    struct Item { fs::path file; std::string rel, key; };
    std::vector<Item> items;
    if (fs::is_directory(fs::u8path(root), ec)) {
        for (auto it = fs::recursive_directory_iterator(fs::u8path(root), fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            const std::string name = it->path().filename().u8string();
            if (name.empty() || name[0] == '.') continue;  // hidden files, AppleDouble "._x"
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            std::string ext = it->path().extension().u8string();
            for (char &c : ext) c = (char)tolower((unsigned char)c);
            if (!music::is_music_extension(ext)) continue;
            Item i;
            i.file = it->path();
            i.rel = fs::relative(i.file, fs::u8path(root), e2).generic_u8string();
            const uint64_t size = (uint64_t)it->file_size(e2);
            const auto mt = fs::last_write_time(i.file, e2);
            i.key = make_key(i.file, size, (int64_t)mt.time_since_epoch().count());
            items.push_back(std::move(i));
        }
    }
    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.rel < b.rel; });
    {
        std::lock_guard<std::mutex> lk(g.m);
        std::vector<Song> old = g.songs;
        std::vector<std::string> old_keys = g.keys;
        g.songs.clear();
        g.keys.clear();
        for (size_t i = 0; i < items.size(); i++) {
            Song s;
            s.rel = items[i].rel;
            s.name = fs::u8path(items[i].rel).stem().u8string();
            s.enabled = !g.disabled.count(s.rel);
            s.id = music::kVirtualBase + (uint32_t)i;
            for (size_t o = 0; o < old.size() && o < old_keys.size(); o++)  // unchanged file: stays as it was
                if (old[o].rel == s.rel && old_keys[o] == items[i].key && old[o].state != State::Scanning) {
                    s.state = old[o].state; s.reason = old[o].reason; s.seconds = old[o].seconds; s.gain = old[o].gain;
                }
            g.songs.push_back(s);
            g.keys.push_back(items[i].key);
        }
        g.prog.total = (int)items.size();
    }
    for (size_t i = 0; i < items.size(); i++) {
        {
            std::lock_guard<std::mutex> lk(g.m);
            if (g.quit) return;
            if (i < g.songs.size()) g.songs[i].state = State::Scanning;
        }
        Song local;
        process(local, items[i].file, items[i].key);
        LOG(LOG_SND, "music: %s -> %s %s (%.1fs gain %.2f)", items[i].rel.c_str(), local.state == State::Ready ? "ready" : "skipped",
            local.reason.c_str(), local.seconds, local.gain);
        std::lock_guard<std::mutex> lk(g.m);
        if (i < g.songs.size()) {
            g.songs[i].state = local.state;
            g.songs[i].reason = local.reason;
            g.songs[i].seconds = local.seconds;
            g.songs[i].gain = local.gain;
            // remember the cache key for loading
        }
        g.prog.done = (int)i + 1;
    }
    // Keep the cache bounded (1 GB): drop files of songs no longer in the folder, oldest first.
    uint64_t total = 0;
    std::set<std::string> live;
    for (auto &i : items) live.insert(i.key + ".pcm");
    std::vector<std::pair<fs::file_time_type, fs::path>> orphans;
    for (auto &e : fs::directory_iterator(fs::u8path(cache), ec)) {
        std::error_code e2;
        total += (uint64_t)e.file_size(e2);
        if (!live.count(e.path().filename().u8string())) orphans.push_back({e.last_write_time(e2), e.path()});
    }
    std::sort(orphans.begin(), orphans.end());
    for (auto &o : orphans) {
        if (total <= (1ull << 30)) break;
        std::error_code e2;
        total -= (uint64_t)fs::file_size(o.second, e2);
        fs::remove(o.second, e2);
    }
    // song keys are recomputed on load from the songs list: store them
    std::lock_guard<std::mutex> lk(g.m);
    g.prog.scanning = false;
}

// song id -> cache key of a ready song ("" if none)
std::string key_for_id(uint32_t id) {
    std::lock_guard<std::mutex> lk(g.m);
    for (size_t i = 0; i < g.songs.size() && i < g.keys.size(); i++)
        if (g.songs[i].id == id && g.songs[i].state == State::Ready) return g.keys[i];
    return "";
}

void load_loop() {
    for (;;) {
        Lib::Job job;
        {
            std::unique_lock<std::mutex> lk(g.m);
            g.cv.wait(lk, [] { return g.quit || !g.jobs.empty(); });
            if (g.quit) return;
            job = g.jobs.back();  // newest request wins
            g.jobs.clear();
        }
        const std::string key = key_for_id(job.id);
        std::shared_ptr<std::vector<int16_t>> data;
        float gain = 1;
        if (!key.empty()) {
            FILE *f = host_fopen(cache_path(key).c_str(), "rb");
            music::CacheHeader h;
            if (f && fread(&h, sizeof h, 1, f) == 1 && music::cache_header_ok(h)) {
                data = std::make_shared<std::vector<int16_t>>((size_t)h.frames * 2);
                if (fread(data->data(), 4, (size_t)h.frames, f) != (size_t)h.frames) data.reset();
                gain = h.gain;
            }
            if (f) fclose(f);
        }
        std::lock_guard<std::mutex> lk(g.m);
        Slot &s = g_slot[job.slot];
        if (s.gen != job.gen) continue;  // superseded
        s.pending.gen = job.gen;
        s.pending.valid = true;
        s.pending.failed = !data;
        s.pending.data = data;
        s.pending.gain = gain;
    }
}

void scan_loop() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g.m);
            g.cv.wait(lk, [] { return g.quit || g.want_scan; });
            if (g.quit) return;
            g.want_scan = false;
        }
        scan_pass();
    }
}

}  // namespace

void init(const std::string &root, const std::string &cache_dir) {
    {
        std::lock_guard<std::mutex> lk(g.m);
        if (g.inited) return;
        g.inited = true;
        g.root = root;
        g.cache = cache_dir;
        std::error_code ec;
        fs::create_directories(fs::u8path(root), ec);
        std::ifstream f(fs::u8path(root + "/state.json"));
        std::stringstream ss;
        ss << f.rdbuf();
        for (auto &s : music::state_from_json(ss.str())) g.disabled.insert(s);
        g.want_scan = true;
        g.bag = music::ShuffleBag((uint64_t)std::chrono::steady_clock::now().time_since_epoch().count());
    }
    g.scan_thread = std::thread(scan_loop);
    g.load_thread = std::thread(load_loop);
    g.cv.notify_all();
}

void init_default() {
    std::string root;
    if (const char *e = getenv("EOD_MUSIC_DIR")) root = e;
    std::string support;
#ifdef __APPLE__
    support = mac_app_support();
#else
    support = ".";
#endif
    if (root.empty()) root = support + "/mods/music";
    init(root, support + "/mods/.cache/music");
}

void shutdown() {
    {
        std::lock_guard<std::mutex> lk(g.m);
        if (!g.inited) return;
        g.quit = true;
    }
    g.cv.notify_all();
    if (g.scan_thread.joinable()) g.scan_thread.join();
    if (g.load_thread.joinable()) g.load_thread.join();
}

void rescan() {
    std::lock_guard<std::mutex> lk(g.m);
    if (g.inited && !g.prog.scanning) { g.want_scan = true; g.cv.notify_all(); }
}

std::vector<Song> songs() {
    std::lock_guard<std::mutex> lk(g.m);
    return g.songs;
}
Progress progress() {
    std::lock_guard<std::mutex> lk(g.m);
    return g.prog;
}
std::string root_dir() {
    std::lock_guard<std::mutex> lk(g.m);
    return g.root;
}
int ready_count() {
    std::lock_guard<std::mutex> lk(g.m);
    int n = 0;
    for (const Song &s : g.songs) n += s.state == State::Ready && s.enabled;
    return n;
}
void set_enabled(const std::string &rel, bool on) {
    std::lock_guard<std::mutex> lk(g.m);
    for (Song &s : g.songs)
        if (s.rel == rel) s.enabled = on;
    if (on) g.disabled.erase(rel); else g.disabled.insert(rel);
    save_state_locked();
}

uint32_t pick_for_native() {
    // Test hooks (never set in play): EOD_MUSIC_WAIT_SCAN=1 waits for the first scan to
    // finish before the first pick; EOD_MUSIC_CUSTOM_ONLY=1 leaves out the native slots.
    static const bool wait = getenv("EOD_MUSIC_WAIT_SCAN") && getenv("EOD_MUSIC_WAIT_SCAN")[0] == '1';
    static const bool only = getenv("EOD_MUSIC_CUSTOM_ONLY") && getenv("EOD_MUSIC_CUSTOM_ONLY")[0] == '1';
    static bool waited = false;
    if (wait && !waited) {
        waited = true;
        for (int i = 0; i < 1200; i++) {
            { std::lock_guard<std::mutex> lk(g.m); if (!g.want_scan && !g.prog.scanning && g.prog.total > 0 && g.prog.done >= g.prog.total) break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    std::lock_guard<std::mutex> lk(g.m);
    if (!g.inited) return 0;
    std::vector<uint32_t> ids;
    for (const Song &s : g.songs)
        if (s.state == State::Ready && s.enabled) ids.push_back(s.id);
    return g.bag.next(ids, only ? 0 : 12);
}

void host_preload(int slot, uint32_t id) {
    if (slot < 0 || slot > 1) return;
    Slot &s = g_slot[slot];
    std::lock_guard<std::mutex> lk(g.m);
    s.gen++;
    s.voice = music::HostVoice();
    s.data.reset();
    s.started = s.loaded = s.failed = false;
    s.pending = Pending();
    g.jobs.push_back({slot, s.gen, id});
    g.cv.notify_all();
}

void host_start(int slot) {
    if (slot < 0 || slot > 1) return;
    Slot &s = g_slot[slot];
    s.started = true;
    if (s.loaded) { s.voice.pos = 0; s.voice.active = true; }
}

void host_stop(int slot) {
    if (slot < 0 || slot > 1) return;
    Slot &s = g_slot[slot];
    std::shared_ptr<std::vector<int16_t>> drop;
    {
        std::lock_guard<std::mutex> lk(g.m);
        s.gen++;
        s.pending = Pending();
        drop = std::move(s.data);
    }
    s.voice = music::HostVoice();
    s.started = s.loaded = s.failed = false;
}

bool host_finished(int slot) {
    if (slot < 0 || slot > 1) return true;
    const Slot &s = g_slot[slot];
    return s.failed || (s.started && s.loaded && s.voice.pos >= s.voice.frames);
}
bool host_active(int slot) { return slot >= 0 && slot <= 1 && g_slot[slot].voice.active; }

void host_begin_block() {
    if (!g.m.try_lock()) return;
    for (Slot &s : g_slot) {
        if (!s.pending.valid || s.pending.gen != s.gen) continue;
        if (s.pending.failed) s.failed = true;
        else {
            s.data = std::move(s.pending.data);
            s.voice.pcm = s.data->data();
            s.voice.frames = s.data->size() / 2;
            s.voice.pos = 0;
            s.voice.gain_q12 = (int)(s.pending.gain * 4096.0f + 0.5f);
            s.loaded = true;
            if (s.started) s.voice.active = true;
        }
        s.pending = Pending();
    }
    g.m.unlock();
}

bool host_frame(int slot, int &l, int &r) { return g_slot[slot].voice.next(l, r); }

}  // namespace custom_music
