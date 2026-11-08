// Online play session: see netplay_session.h and docs/NETPLAY_STATE_SYNC.md.
#include "netplay_session.h"
#include "netplay_content.h"
#include "../runtime/core/config.h"
#include "netplay_online.h"
#include "netplay_online_wire.h"
#include "netplay_voice.h"
#include "session_start.h"
#include "../runtime/platform/platform.h"
#include "../runtime/ui/overlay.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define EOD_CLOSESOCK closesocket
static void set_nonblock(int fd) { u_long on = 1; ioctlsocket(fd, FIONBIO, &on); }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define EOD_CLOSESOCK close
static void set_nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }
#endif

#include "../runtime/iop/iop.h"
#include "../runtime/core/memory.h"

namespace netplay {
namespace {
constexpr uint16_t kGamePort = 27015, kDiscoveryPort = 27014;
constexpr uint64_t kPeerTimeoutUs = 5000000;
uint64_t now_us() { return platform_time_us(); }
uint32_t now_ms() { return (uint32_t)(now_us() / 1000); }

const char *kDefaultEvents[] = {"No-Rules Race", "Battle (4 AI)", "Battle (8 AI)"};   // EOD_NETPLAY_MODE=0|1|2 picks the lobby default

std::string default_name() {
    static const char *first[] = {"Dust","Iron","Rally","Nitro","Gravel","Rust","Chrome","Apex"};
    static const char *last[] = {"Fox","Hawk","Wolf","Bolt","Ace","Owl","Cat","Lynx"};
    std::random_device rand;
    return std::string(first[rand() % 8]) + last[rand() % 8] + std::to_string(10 + rand() % 90);
}
std::string sockaddr_text(const sockaddr_in &a) {
    char buf[32]; inet_ntop(AF_INET, &a.sin_addr, buf, sizeof buf);
    return std::string(buf) + ":" + std::to_string(ntohs(a.sin_port));
}
std::string clean_name(std::string n) {
    n.erase(std::remove_if(n.begin(), n.end(), [](unsigned char c) { return c < 32 || c > 126; }), n.end());
    const auto first = n.find_first_not_of(' ');
    if (first == n.npos) return "";
    return n.substr(first, n.find_last_not_of(' ') - first + 1).substr(0, 16);
}
bool same_addr(const sockaddr_in &a, const sockaddr_in &b) { return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port; }
std::string local_ip() {
#ifndef _WIN32
    ifaddrs *ifs = nullptr;
    std::string best;
    if (getifaddrs(&ifs) == 0) {
        for (ifaddrs *i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)) continue;
            char buf[32]; inet_ntop(AF_INET, &((sockaddr_in *)i->ifa_addr)->sin_addr, buf, sizeof buf);
            std::string ip = buf;
            if (ip.compare(0, 8, "169.254.") == 0) continue;
            if (best.empty() || ip.compare(0, 4, "192.") == 0 || ip.compare(0, 3, "10.") == 0) best = ip;
        }
        freeifaddrs(ifs);
    }
    return best.empty() ? "127.0.0.1" : best;
#else
    return "127.0.0.1";
#endif
}
std::string vehicle_name(uint32_t number) {
    const uint32_t table = ee_r32(0x003CBEE4), n = ee_r32(0x00439704);
    if (!table || n == 0 || n > 4096) return "CAR " + std::to_string(number);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t v = table + i * 0x54;
        if (ee_r32(v + 4) == number) {
            std::string s;
            for (uint32_t k = 0; k < 32; ++k) { char c = (char)ee_r8(v + 8 + k); if (!c) break; s.push_back(c); }
            return s;
        }
    }
    return "CAR " + std::to_string(number);
}
bool vehicle_known(uint32_t number) {
    const uint32_t table = ee_r32(0x003CBEE4), n = ee_r32(0x00439704);
    if (!table || n == 0 || n > 4096) return true;
    for (uint32_t i = 0; i < n; ++i)
        if (ee_r32(table + i * 0x54 + 4) == number) return true;
    return false;
}
uint32_t fallback_vehicle() {
    const uint32_t table = ee_r32(0x003CBEE4), n = ee_r32(0x00439704);
    return (table && n > 0) ? ee_r32(table + 4) : 1;
}
}  // namespace

bool parse_endpoint(const std::string &text, uint32_t &ip, uint16_t &port, uint16_t default_port) {
    std::string host = text; port = default_port;
    size_t c = text.rfind(':');
    if (c != std::string::npos) { host = text.substr(0, c); long p = std::strtol(text.c_str() + c + 1, nullptr, 10); if (p <= 0 || p > 65535) return false; port = (uint16_t)p; }
    in_addr a{};
    if (inet_pton(AF_INET, host.c_str(), &a) != 1) return false;
    ip = ntohl(a.s_addr);
    return true;
}

struct Peer {
    sockaddr_in addr{};
    uint8_t id = 0;
    std::string name, secret;
    std::string requested_name;
    bool ready = false, at_menu = true, car_done = false, got_track = false, got_roster = false, got_go = false, got_start = false;
    uint32_t vehicle = 0;
    std::string vname;
    uint8_t phase = P_LOBBY;
    uint32_t seen_event = 0;
    uint64_t last_heard = 0, last_lobby = 0;
    uint32_t ping_ms = 0;
    uint32_t last_ts_sent = 0;
    bool alive = true;
    uint64_t identity = 0;
    uint64_t latest_tick = 0; netplay_car::State latest; bool has_latest = false; uint64_t latest_at = 0;
};
struct LobbyPlayer { uint8_t id; std::string name; bool ready; std::string vname; uint16_t ping; };
struct LanEntry { sockaddr_in addr; std::string name; uint8_t players; uint64_t seen; };
struct Friend { std::string secret, name; };                 // remembered crew: a shared random secret per friend
struct CrewHost { sockaddr_in addr{}; std::string name; uint8_t players = 0; uint64_t seen = 0; };

struct Session::Impl {
    int fd = -1, disc_fd = -1;
    enum class Role { None, Host, Guest } role = Role::None;
    uint16_t port = 0;
    std::string status, name = default_name();
    // host side
    std::map<uint8_t, Peer> peers;
    uint8_t rules = 0;
    unsigned max_players = kMaxPlayers;
    uint64_t next_peer_identity = 0;
    int ai_count = 0;
    // lobby picks: the host owns TRACK and MODE (event), every player owns CAR and PAINT
    std::string track_name, event_pick, lobby_track;
    std::string my_model; int my_paint = 0;
    bool local_ready = true;
    uint64_t last_beacon = 0, last_lobby_send = 0;
    // guest side
    sockaddr_in host_addr{};
    uint8_t my_id = 0;
    bool welcomed = false;
    uint64_t last_join = 0, last_hb = 0, host_last_heard = 0;
    uint32_t last_host_ts = 0;
    std::vector<LobbyPlayer> lobby_players;
    std::string lobby_event; uint8_t host_phase = 0;
    bool guest_ready = false;
    // common event state
    uint32_t event_id = 0;
    bool pending_start = false; StartInfo start_info;
    std::string event_name, track;
    bool have_track = false;
    uint32_t my_vehicle = 0; std::string my_vname;
    bool car_done = false;
    std::vector<RosterSlot> roster; bool have_roster = false;
    bool go = false, loaded_sent = false;
    uint32_t grid_event = 0, applied_grid_event = 0;
    std::vector<netplay_car::State> initial_grid;
    std::map<uint8_t, uint32_t> grid_acks;
    uint8_t my_phase = P_LOBBY;
    uint64_t last_menu_tick = 0;
    bool in_menu = false; uint32_t inject_count = 0;
    std::string strip;       // "Waiting for ..." shown over the game while players choose
    uint32_t tick = 0;       // world steps since GO
    int my_slot = -1; bool bound = false;
    std::map<int, netplay_car::Remote> remote;
    std::map<int, netplay_car::State> shown;
    std::map<int, uint32_t> puppet_cars;
    std::map<std::string, LanEntry> lan;
    bool discovering = false;
    bool abort_requested = false;
    std::string last_error;
    std::string content_note, content_id;
    // ---- internet (Garage Code), see docs/NETPLAY_INTERNET.md
    netplay_online::Online online;
    bool online_on = false;
    std::string code;                          // host: the code to share; guest: the code being joined
    std::string join_secret;                   // guest: secret (code or friend secret) the rendezvous runs on
    uint32_t join_tag = 0;
    std::vector<Friend> friends; bool friends_loaded = false;
    std::vector<sockaddr_in> targets;          // guest: host candidates from the rendezvous (JOIN is sent to all)
    std::vector<sockaddr_in> punch_targets;    // host: joiners found at the trackers (we send them a punch)
    uint64_t join_started = 0, last_punch = 0, last_probe = 0, ui_seen = 0, last_remember = 0;
    bool joining_online = false, remembered = false;
    std::string join_note;
    std::map<std::string, CrewHost> crew;      // remembered friends currently hosting, by secret
    std::string route_text;
    // ---- voice chat (Opus 48 kHz mono + Halo HUD indicator)
    netplay::voice::Session voice;
    bool mic_enabled = false, muted_all = false, deafened = false, mic_open = false;
    std::string host_address;
    uint64_t mic_retry_at = 0;
    uint64_t speaking_until[kMaxPlayers] = {};
    uint64_t last_voice_playout = 0;
    mutable std::mutex voice_mtx;
    std::vector<int16_t> voice_out;
    size_t voice_pos = 0;

    void sync_voice() {
        const bool connected = (role == Role::Host) || (role == Role::Guest && welcomed);
        const uint8_t local_id = role == Role::Host ? 0 : my_id;
        if (connected && local_id < netplay::voice::MaxPeers) {
            if (!voice.active()) voice.start(0x454F44564F494345ull, 1, local_id);
            voice.set_transmit_enabled(mic_enabled && !deafened);
            voice.set_microphone_muted(!mic_enabled || deafened);
            voice.set_deafened(deafened || muted_all);
        } else {
            if (voice.active()) voice.stop();
        }
        const bool want_mic = connected && voice.active() && mic_enabled && !deafened;
        if (want_mic && !mic_open) {
            mic_open = platform_mic_open((int)netplay::voice::SampleRate);
            mic_retry_at = now_us() + 3000000;
        } else if (!want_mic && mic_open) {
            platform_mic_close();
            mic_open = false;
        }
    }
    void poll_voice(uint64_t t) {
        const uint8_t local_id = (role == Role::Guest && welcomed) ? my_id : 0;
        // Explicit diagnostic source exercises the real codec and transport without
        // requesting a physical microphone or producing speaker output.
        static const bool test_voice = std::getenv("EOD_NETPLAY_TEST_VOICE") != nullptr;
        static uint64_t next_test_frame = 0;
        if (test_voice && voice.active()) {
            voice.set_transmit_enabled(true);
            voice.set_microphone_muted(false);
            if (t >= next_test_frame) {
                next_test_frame = t + 20000;
                netplay::voice::Frame pcm{};
                static uint64_t frame = 0;
                // Include a long silence to exercise resume after inactivity.
                if ((frame++ % 150) < 50)
                    for (size_t i=0;i<pcm.size();++i) pcm[i] = (int16_t)(5000 * std::sin(i * (local_id ? 0.087 : 0.0576)));
                netplay::voice::Packet pkt;
                if (voice.encode(pcm,pkt)) {
                    Writer w; w.b.assign(pkt.bytes.data(),pkt.bytes.data()+pkt.size);
                    if (role == Role::Host) for (auto &kv : peers) { if (kv.second.alive) send_to(kv.second.addr,K_VOICE,0,w); }
                    else if (welcomed) send_to(host_addr,K_VOICE,my_id,w);
                }
            }
        }
        if (voice.active() && mic_enabled && !deafened) {
            if (!mic_open && t >= mic_retry_at) {
                mic_open = platform_mic_open((int)netplay::voice::SampleRate);
                mic_retry_at = t + 3000000;
            }
            netplay::voice::Frame frame{};
            int batches = 0;
            while (mic_open && batches++ < 4) {
                const int samples = platform_mic_read(frame.data(), (int)netplay::voice::FrameSamples);
                if (samples < 0) {
                    platform_mic_close();
                    mic_open = false;
                    mic_retry_at = t + 3000000;
                    break;
                }
                if (samples != (int)netplay::voice::FrameSamples) break;
                int peak = 0;
                for (int16_t s : frame) peak = std::max(peak, std::abs((int)s));
                if (std::getenv("EOD_NETPLAY_TEST_MICROPHONE")) {
                    static uint64_t last_log=0, captured=0;
                    captured += frame.size();
                    if(t-last_log>1000000) {
                        last_log=t;
                        std::fprintf(stderr,"[physical-mic] samples=%llu peak=%d role=%d device_open=%d\n",
                                     (unsigned long long)captured,peak,(int)role,(int)mic_open);
                    }
                }
                if (peak > 500 && local_id < kMaxPlayers) speaking_until[local_id] = t + 300000;
                if ((role != Role::Host || alive_count() > 0) && local_id < kMaxPlayers) {
                    netplay::voice::Packet pkt;
                    if (voice.encode(frame, pkt) && pkt.size > 0) {
                        Writer w;
                        w.b.assign(pkt.bytes.data(), pkt.bytes.data() + pkt.size);
                        if (role == Role::Host) {
                            for (auto &kv : peers) if (kv.second.alive) send_to(kv.second.addr, K_VOICE, 0, w);
                        } else if (role == Role::Guest && welcomed) {
                            send_to(host_addr, K_VOICE, my_id, w);
                        }
                    }
                }
            }
        }
        if (!voice.active() || (role == Role::Host && alive_count() == 0)) { last_voice_playout = t; return; }
        if (!last_voice_playout || t - last_voice_playout > 100000) last_voice_playout = t - 20000;
        while (t - last_voice_playout >= 20000) {
            last_voice_playout += 20000;
            int32_t mixed[netplay::voice::FrameSamples] = {};
            bool any = false;
            for (uint8_t peer = 0; peer < netplay::voice::MaxPeers; ++peer) {
                if (peer == local_id) continue;
                netplay::voice::Frame pf;
                auto res = voice.playout(peer, pf);
                if (res == netplay::voice::Playback::Decoded || res == netplay::voice::Playback::Concealed) {
                    for (size_t i = 0; i < netplay::voice::FrameSamples; ++i) {
                        if (pf[i] != 0) any = true;
                        mixed[i] += pf[i];
                    }
                }
            }
            if (any && !deafened && !muted_all) {
                std::lock_guard<std::mutex> lk(voice_mtx);
                if (voice_pos > 0 && voice_pos >= voice_out.size()) { voice_out.clear(); voice_pos = 0; }
                else if (voice_pos >= 4800) { voice_out.erase(voice_out.begin(), voice_out.begin() + (ptrdiff_t)voice_pos); voice_pos = 0; }
                if (voice_out.size() - voice_pos < 9600) {
                    for (size_t i = 0; i < netplay::voice::FrameSamples; ++i)
                        voice_out.push_back((int16_t)std::clamp(mixed[i], -32768, 32767));
                }
            }
        }
    }

    // ---------------------------------------------------------------- friends file (next to eod.ini)
    std::string friends_path() const {
        std::string p = g_config_path;
        size_t sl = p.find_last_of("/\\");
        return (sl == std::string::npos ? std::string() : p.substr(0, sl + 1)) + "friends.txt";
    }
    void load_friends() {
        if (friends_loaded) return;
        friends_loaded = true;
        std::ifstream in(friends_path());
        std::string line;
        while (std::getline(in, line)) {
            size_t t = line.find('\t');
            if (t == std::string::npos) continue;
            Friend f{line.substr(0, t), line.substr(t + 1)};
            std::vector<uint8_t> raw;
            if (!netplay_online::unhex(f.secret, raw) || raw.size() != 16 || f.name.empty()) continue;
            friends.push_back(f);
        }
    }
    void save_friends() {
        std::ofstream out(friends_path(), std::ios::trunc);
        for (auto &f : friends) out << f.secret << '\t' << f.name << '\n';
    }
    std::string available_name(std::string preferred, uint8_t slot) const {
        preferred = clean_name(preferred);
        if (preferred.empty()) preferred = "Driver";
        for (unsigned i = 0;; ++i) {
            const std::string suffix = i ? " (" + std::to_string(i) + ")" : "";
            const std::string candidate = preferred.substr(0, 16 - suffix.size()) + suffix;
            bool used = slot != 0 && name == candidate;
            for (const auto &kv : peers)
                if (kv.first != slot && kv.second.alive && kv.second.name == candidate) used = true;
            if (!used) return candidate;
        }
    }
    void remember(const std::string &friend_name, const std::string &secret) {
        load_friends();
        if (friend_name.empty() || secret.size() != 32) return;
        friends.erase(std::remove_if(friends.begin(), friends.end(), [&](const Friend &f) { return f.name == friend_name || f.secret == secret; }), friends.end());
        friends.insert(friends.begin(), Friend{secret, friend_name});
        if (friends.size() > 12) friends.resize(12);
        save_friends();
        std::fprintf(stderr, "[netplay] remembered friend '%s'\n", friend_name.c_str());
    }
    static std::string new_secret() { uint8_t b[16]; std::random_device rd; for (auto &x : b) x = (uint8_t)rd(); return netplay_online::hex(b, 16); }
    static std::string new_code() { uint8_t b[5]; std::random_device rd; for (auto &x : b) x = (uint8_t)rd(); return netplay_online::code_from_bytes(b, 5); }
    // The secret a JOIN tag belongs to ("" when it matches nothing we know).
    bool tag_known(uint32_t tag, std::string *secret) const {
        if (!code.empty() && tag == netplay_online::join_tag(code)) { if (secret) secret->clear(); return true; }
        for (auto &f : friends) if (tag == netplay_online::join_tag(f.secret)) { if (secret) *secret = f.secret; return true; }
        return false;
    }

    // ---------------------------------------------------------------- internet jobs
    static sockaddr_in to_sockaddr(const netplay_online::Addr &a) { sockaddr_in s{}; s.sin_family = AF_INET; s.sin_addr.s_addr = htonl(a.ip); s.sin_port = htons(a.port); return s; }
    void online_start(bool want_mapping) {
        if (online_on || fd < 0) return;
        online.start(fd, port, want_mapping);
        online_on = true;
    }
    void online_stop() {
        if (!online_on) return;
        online.stop();
        online_on = false; targets.clear(); punch_targets.clear(); crew.clear();
    }
    void host_jobs() {
        using namespace netplay_online;
        std::vector<Job> jobs;
        jobs.push_back({"code", rendezvous_hash('H', code), true, false, false, 5000});
        jobs.push_back({"joiners", rendezvous_hash('J', code), false, false, true, 4000});
        for (auto &f : friends) {
            jobs.push_back({"ch:" + f.secret, rendezvous_hash('H', f.secret), true, false, false, 5000});
            jobs.push_back({"cj:" + f.secret, rendezvous_hash('J', f.secret), false, false, true, 8000});
        }
        online.set_jobs(jobs);
    }
    void guest_jobs(const std::string &secret) {
        using namespace netplay_online;
        std::vector<Job> jobs;
        jobs.push_back({"me", rendezvous_hash('J', secret), true, true, false, 5000});
        jobs.push_back({"hosts", rendezvous_hash('H', secret), false, false, true, 3000});
        online.set_jobs(jobs);
    }
    void watch_jobs() {   // ONLINE tab open, not in a game: look for remembered friends who are hosting
        using namespace netplay_online;
        std::vector<Job> jobs;
        for (auto &f : friends) {
            jobs.push_back({"w:" + f.secret, rendezvous_hash('J', f.secret), true, true, false, 5000});
            jobs.push_back({"ch:" + f.secret, rendezvous_hash('H', f.secret), false, false, true, 8000});
        }
        online.set_jobs(jobs);
    }
    std::string route_description(const netplay_online::Report &r) const {
        if (!r.stun_done) return "Looking for the best route...";
        if (!r.stun_ok && r.trackers_ok == 0) return "No internet link found. Friends on your network can still join.";
        if (r.mapped) return "Internet play ready (router port opened by " + r.map_kind + ")";
        if (r.symmetric) return "Internet play limited: your connection is strict, friends may not be able to connect";
        return "Internet play ready";
    }

    // ---------------------------------------------------------------- sockets
    bool open(uint16_t want_port, bool listen_range) {
        close_game();
        fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return false;
        set_nonblock(fd);
        int one = 1; setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);
        for (uint16_t p = want_port; p < want_port + (listen_range ? 16 : 1); ++p) {
            sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(p);
            if (bind(fd, (sockaddr *)&a, sizeof a) == 0) {
                port = p;
                if (!port) { sockaddr_in me{}; socklen_t ml = sizeof me; if (getsockname(fd, (sockaddr *)&me, &ml) == 0) port = ntohs(me.sin_port); }
                return true;
            }
        }
        EOD_CLOSESOCK(fd); fd = -1;
        return false;
    }
    void close_game() { online_stop(); if (fd >= 0) EOD_CLOSESOCK(fd); fd = -1; }
    void ensure_discovery() {
        if (discovering || std::getenv("EOD_NETPLAY_NO_LAN")) return;   // NO_LAN: developer switch that forces the internet path
        discovering = true;
        disc_fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
        if (disc_fd < 0) return;
        int one = 1;
        setsockopt(disc_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
#ifdef SO_REUSEPORT
        setsockopt(disc_fd, SOL_SOCKET, SO_REUSEPORT, (const char *)&one, sizeof one);
#endif
        set_nonblock(disc_fd);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(kDiscoveryPort);
        if (bind(disc_fd, (sockaddr *)&a, sizeof a) != 0) { EOD_CLOSESOCK(disc_fd); disc_fd = -1; }
    }
    void send_to(const sockaddr_in &to, uint8_t kind, uint8_t from, const Writer &body) {
        if (fd < 0) return;
        Writer w; w.u16(kMagic); w.u8(kind); w.u8(from);
        w.b.insert(w.b.end(), body.b.begin(), body.b.end());
        sendto(fd, (const char *)w.b.data(), (int)w.b.size(), 0, (const sockaddr *)&to, sizeof to);
    }
    sockaddr_in addr_of(uint32_t ip_host, uint16_t p) { sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(ip_host); a.sin_port = htons(p); return a; }

    // ---------------------------------------------------------------- lobby helpers
    std::string mode_name() const { return event_pick; }
    // ---- catalog-backed picks (guest context fills the catalog; see Session::menu_buttons)
    const session_catalog::Track *find_track(const std::string &n) const { for (auto &t : session_catalog::tracks()) if (t.name == n) return &t; return nullptr; }
    void ensure_defaults() {
        if (!session_catalog::ready()) return;
        if (!find_track(track_name)) {
            const char *want = std::getenv("EOD_NETPLAY_TRACK");
            const session_catalog::Track *t = want ? find_track(want) : nullptr;
            if (!t) t = find_track("Anoka");
            if (!t) t = &session_catalog::tracks()[0];
            track_name = t->name; event_pick.clear();
        }
        const session_catalog::Track *t = find_track(track_name);
        if (t && std::find(t->events.begin(), t->events.end(), event_pick) == t->events.end()) {
            const int idx = std::getenv("EOD_NETPLAY_MODE") ? std::atoi(std::getenv("EOD_NETPLAY_MODE")) % 3 : 0;
            event_pick = std::find(t->events.begin(), t->events.end(), kDefaultEvents[idx]) != t->events.end() ? kDefaultEvents[idx] : t->events[0];
        }
        auto models = session_catalog::models();
        if (!models.empty() && std::find(models.begin(), models.end(), my_model) == models.end()) {
            const char *want = std::getenv("EOD_NETPLAY_CAR");
            my_model = want && std::find(models.begin(), models.end(), want) != models.end() ? want : models[0];
            my_paint = 0;
        }
        apply_car();
    }
    void apply_car() {
        auto paints = session_catalog::paints(my_model);
        if (paints.empty()) return;
        my_paint = ((my_paint % (int)paints.size()) + (int)paints.size()) % (int)paints.size();
        my_vehicle = paints[(size_t)my_paint]; my_vname = my_model;
    }
    static constexpr struct RuleOption { const char *id, *label; uint8_t bit; } rule_options[] = {
        {"rule_turbo","TURBO BOOST",netplay_hooks::Turbo}, {"rule_chicken","CHICKEN FIGHT",netplay_hooks::Chickens},
        {"rule_damage","INVINCIBILITY",netplay_hooks::Invincible}, {"rule_durability","CAR DURABILITY",netplay_hooks::Durability},
        {"rule_recovery","MANUAL RECOVERY",netplay_hooks::ManualRecovery}
    };
    void cycle(const std::string &what, int dir) {
        if (what == "players" && role == Role::Host) {
            max_players = std::clamp<int>((int)max_players + (dir >= 0 ? 1 : -1),1+alive_count(),kMaxPlayers);
            return;
        }
        for (const auto &opt : rule_options) if (what == opt.id) {
            if (role == Role::Host) rules ^= opt.bit;
            return;
        }
        if (!session_catalog::ready()) return;
        auto step = [&](size_t n, size_t cur) { return (cur + (dir >= 0 ? 1 : n - 1)) % n; };
        if (what == "track" && role == Role::Host) {
            auto &ts = session_catalog::tracks(); size_t cur = 0;
            for (size_t i = 0; i < ts.size(); ++i) if (ts[i].name == track_name) cur = i;
            track_name = ts[step(ts.size(), cur)].name;
        } else if (what == "mode" && role == Role::Host) {
            const session_catalog::Track *t = find_track(track_name);
            if (t && !t->events.empty()) { size_t cur = 0; for (size_t i = 0; i < t->events.size(); ++i) if (t->events[i] == event_pick) cur = i; event_pick = t->events[step(t->events.size(), cur)]; }
        } else if (what == "car") {
            auto models = session_catalog::models(); size_t cur = 0;
            for (size_t i = 0; i < models.size(); ++i) if (models[i] == my_model) cur = i;
            if (!models.empty()) { my_model = models[step(models.size(), cur)]; my_paint = 0; }
        } else if (what == "paint") my_paint += dir >= 0 ? 1 : -1;
        ensure_defaults();
    }
    std::vector<LobbyPlayer> make_lobby() const {
        std::vector<LobbyPlayer> v;
        v.push_back({0, name, true, my_vname, 0});
        for (const auto &kv : peers) if (kv.second.alive) v.push_back({kv.first, kv.second.name, kv.second.ready, kv.second.vname, (uint16_t)kv.second.ping_ms});
        return v;
    }
    void put_lobby(Writer &w, uint32_t ts) {
        w.u32(ts); w.u32(event_id); w.u8(my_phase); w.str(mode_name(), 40); w.u8((uint32_t)ai_count);
        auto v = make_lobby();
        w.u8((uint32_t)v.size());
        for (auto &p : v) { w.u8(p.id); w.str(p.name, 16); w.u8(p.ready); w.str(p.vname, 24); w.u16(p.ping); }
        w.str(track_name, 40); w.u8(rules); w.u8(max_players);
    }
    bool all_peers(bool (*pred)(const Peer &)) const { for (auto &kv : peers) if (kv.second.alive && !pred(kv.second)) return false; return true; }
    int alive_count() const { int n = 0; for (auto &kv : peers) n += kv.second.alive; return n; }
};

Session::Session() : d_(new Impl) {}
Session::~Session() { if (d_->mic_open) platform_mic_close(); d_->close_game(); if (d_->disc_fd >= 0) EOD_CLOSESOCK(d_->disc_fd); netplay_online::Online::wait_cleanup(2500); }

// ====================================================================== UI side
game_frontend::Snapshot Session::snapshot() const {
    Impl &d = *d_;
    Session *self = const_cast<Session *>(this);
    self->d_->ensure_discovery();
    d.ui_seen = now_us();
    using S = game_frontend::Snapshot;
    S s;
    s.direct_connect = true; s.internet_invites = true; s.native_race_sync = true;
    s.voice = netplay::voice::Session::available();
    s.microphone_enabled = d.mic_enabled;
    s.microphone_capture = d.mic_open;
    s.muted = d.muted_all;
    s.deafened = d.deafened;
    s.error = d.last_error;
    s.content_problem = d.content_note;
    s.is_host = d.role == Impl::Role::Host;
    if (d.role == Impl::Role::None) {
        s.phase = in_event_ ? S::Phase::Results : S::Phase::Offline;
        s.status = in_event_ ? "Leaving the match" : "Not connected";
        if (d.online_on) s.route = d.route_text;
        uint64_t t = now_us();
        for (auto &kv : d.crew)
            if (t - kv.second.seen < 9000000) s.crew.push_back({kv.second.name, kv.first, kv.second.players});
    } else if (d.role == Impl::Role::Guest && !d.welcomed) {
        s.phase = S::Phase::Connecting; s.status = d.join_note.empty() ? "Connecting..." : d.join_note; s.can_cancel = true;
        s.route = d.route_text;
    } else {
        s.phase = d.abort_requested ? S::Phase::Results : in_event_ && d.bound ? S::Phase::Racing :
                  (in_event_ || d.pending_start) ? S::Phase::Loading : S::Phase::Lobby;
        s.status = s.phase == S::Phase::Results ? "Returning to lobby" :
                   s.phase == S::Phase::Racing ? "Racing" :
                   s.phase == S::Phase::Loading ? "Loading race" : d.status;
        s.can_cancel = true;
        s.invite = d.role == Impl::Role::Host ? d.host_address : "";
        s.address = s.invite;
        s.code = d.role == Impl::Role::Host ? d.code : "";
        s.track_pic = d.role == Impl::Role::Host ? d.track_name : d.lobby_track; s.car_pic = d.my_vehicle;
        s.route = d.route_text;
        const uint64_t t_now = now_us();
        const uint8_t local_id = d.role == Impl::Role::Host ? 0 : d.my_id;
        if (d.role == Impl::Role::Host) {
            for (auto &p : d.make_lobby()) {
                const bool spk = p.id < kMaxPlayers && t_now < d.speaking_until[p.id];
                const bool mtd = p.id != local_id && (d.muted_all || d.deafened);
                s.players.push_back({p.id, p.name + (p.id == local_id ? " (YOU)  " : "  ") + p.vname + (p.id ? "  " + std::to_string(p.ping) + "ms" : ""), p.ready, mtd, false, spk});
                if (p.id) s.players.back().identity = d.peers.at(p.id).identity;
            }
            s.can_start = s.phase == S::Phase::Lobby && d.in_menu && session_catalog::ready() &&
                          d.all_peers([](const Peer &p) { return p.at_menu && p.ready; });
            if (!s.can_start && s.phase == S::Phase::Lobby)
                s.status = d.in_menu ? "Waiting for game data or players to be ready" : "Waiting for the game main menu";
            s.options.push_back({"players","MAX PLAYERS",std::to_string(d.max_players),s.phase == S::Phase::Lobby,"Includes the host. Race capacity depends on the event."});
            s.options.push_back({"track", "TRACK", d.track_name.empty() ? "Loading game data..." : d.track_name, true, ""});
            s.options.push_back({"mode", "MODE", d.mode_name(), true, ""});
        } else {
            for (auto &p : d.lobby_players) {
                const bool spk = p.id < kMaxPlayers && t_now < d.speaking_until[p.id];
                const bool mtd = p.id != local_id && (d.muted_all || d.deafened);
                s.players.push_back({p.id, p.name + (p.id == local_id ? " (YOU)  " : "  ") + p.vname + "  " + std::to_string(p.ping) + "ms", p.ready, mtd, false, spk});
            }
            s.ready = d.guest_ready;
            if (s.phase == S::Phase::Lobby && s.ready) s.status = "Waiting for host";
            s.options.push_back({"players","MAX PLAYERS",std::to_string(d.max_players),false,"Host only."});
            s.options.push_back({"track", "TRACK", d.lobby_track, false, "Host only."});
            s.options.push_back({"mode", "MODE", d.lobby_event, false, "Host only."});
        }
        s.options.push_back({"car", "YOUR CAR", d.my_model.empty() ? "Loading game data..." : d.my_model, true, ""});
        s.options.push_back({"paint", "PAINT", d.my_model.empty() ? "-" : "PAINT " + std::to_string(d.my_paint + 1) + " OF " + std::to_string(std::max<size_t>(1, session_catalog::paints(d.my_model).size())), true, ""});
        for (const auto &opt : Impl::rule_options)
            s.options.push_back({opt.id,opt.label,(d.rules & opt.bit) ? "ON" : "OFF",s.is_host,
                                 "Applies to all players. Locked during the race."});
    }
    if (s.phase != S::Phase::Lobby) for (auto &o : s.options) o.editable = false;
    uint64_t t = now_us();
    for (auto &kv : d.lan) if (t - kv.second.seen < 4000000) s.lan.push_back({kv.second.name, kv.first, kv.second.players});
    s.strip = d.strip;
    return s;
}

void Session::submit(game_frontend::Command c) {
    Impl &d = *d_;
    using K = game_frontend::Command::Kind;
    d.last_error.clear();
    const bool joining = c.kind == K::Host || c.kind == K::JoinCode || c.kind == K::JoinAddress || c.kind == K::JoinCrew;
    if (joining) {
        if (in_event_ || d.role != Impl::Role::None) return;
        d.name = clean_name(g_config.nickname);
        if (d.name.empty()) d.name = default_name();
        const char *overlay = std::getenv("EOD_MOD_DIR");
        auto content = content_fingerprint(g_game_root, overlay ? overlay : "");
        if (!content.error.empty()) { d.last_error = "Could not check game compatibility: " + content.error; return; }
        d.content_id = content.value;
        std::fprintf(stderr,"[netplay] content %s\n",d.content_id.c_str());
        d.rules = 0;
        d.event_id = 0;
        d.pending_start = false;
        d.abort_requested = false;
        d.inject_count = 0;
        d.my_phase = P_LOBBY;
    }
    switch (c.kind) {
    case K::Host: {
        if (d.role != Impl::Role::None) return;
        if (!d.open(kGamePort, true)) { d.last_error = "Could not open the network port. Another copy of the game may already be hosting; close it and try again."; return; }
        d.load_friends();
        d.host_address = local_ip() + ":" + std::to_string(d.port);
        d.role = Impl::Role::Host; d.peers.clear(); d.ensure_defaults(); d.status = "Waiting for friends"; d.my_phase = P_LOBBY;
        d.my_vehicle = d.my_vehicle ? d.my_vehicle : 0;
        d.code = std::getenv("EOD_NETPLAY_CODE") ? std::getenv("EOD_NETPLAY_CODE") : Impl::new_code();
        d.route_text = "Looking for the best route...";
        if (!std::getenv("EOD_NETPLAY_NO_INTERNET")) { d.online_start(true); d.host_jobs(); }
        d.sync_voice();
        std::fprintf(stderr, "[netplay] hosting on %s:%u, Garage Code %s\n", local_ip().c_str(), d.port, d.code.c_str());
        break;
    }
    case K::JoinAddress: {
        uint32_t ip; uint16_t port;
        if (!parse_endpoint(c.text, ip, port, kGamePort)) { d.last_error = "That address does not look right. Use the form 192.168.1.20 or 192.168.1.20:27015."; return; }
        if (d.role != Impl::Role::None) return;
        if (!d.open(0, false)) { d.last_error = "Could not open the network port."; return; }
        d.role = Impl::Role::Guest; d.host_addr = d.addr_of(ip, port); d.welcomed = false; d.last_join = 0;
        d.joining_online = false; d.join_tag = 0; d.targets.clear(); d.join_note.clear(); d.remembered = false; d.join_started = now_us();
        d.host_last_heard = now_us(); d.status = "Connecting..."; d.guest_ready = false;
        std::fprintf(stderr, "[netplay] joining %s from local port %d\n", c.text.c_str(), [&] { sockaddr_in me{}; socklen_t ml = sizeof me; getsockname(d.fd, (sockaddr *)&me, &ml); return (int)ntohs(me.sin_port); }());
        break;
    }
    case K::JoinCode: case K::JoinCrew: {
        if (d.role != Impl::Role::None) return;
        std::string secret, shown;
        d.load_friends();
        if (c.kind == K::JoinCode) {
            using netplay_online::CodeCheck;
            CodeCheck ck = netplay_online::normalize_code(c.text, secret);
            if (ck == CodeCheck::TooShort) { d.last_error = "A Garage Code has 5 characters, like K7QD2. Check the code your friend sent you."; return; }
            if (ck == CodeCheck::TooLong) { d.last_error = "That is longer than a Garage Code (5 characters, like K7QD2). Check what you pasted."; return; }
            if (ck == CodeCheck::BadChar) { d.last_error = "A Garage Code never contains 0, O, 1, I or L. Check the code your friend sent you."; return; }
            shown = secret;
        } else {
            for (auto &f : d.friends) if (f.secret == c.text) { secret = f.secret; shown = f.name; }
            if (secret.empty()) { d.last_error = "That friend is not in your list any more."; return; }
        }
        // keep the watcher's socket (its router mapping is what the host just opened a path to)
        if (d.fd < 0 && !d.open(0, false)) { d.last_error = "Could not open the network port."; return; }
        d.role = Impl::Role::Guest; d.welcomed = false; d.last_join = 0; d.host_addr = sockaddr_in{};
        d.code = c.kind == K::JoinCode ? secret : std::string();
        d.join_secret = secret; d.join_tag = netplay_online::join_tag(secret);
        d.joining_online = true; d.targets.clear(); d.remembered = false; d.join_started = now_us(); d.guest_ready = false;
        d.host_last_heard = now_us(); d.status = "Connecting..."; d.join_note = "Looking for " + (c.kind == K::JoinCode ? "game " + shown : shown + "'s game") + "...";
        d.route_text = "Looking for the best route...";
        d.online_start(false);
        d.guest_jobs(secret);
        d.online.poke();
        std::fprintf(stderr, "[netplay] joining by %s %s\n", c.kind == K::JoinCode ? "code" : "friend", shown.c_str());
        break;
    }
    case K::Nickname: {
        g_config.nickname = clean_name(c.text);
        const auto requested = g_config.nickname.empty() ? default_name() : g_config.nickname;
        d.name = d.role == Impl::Role::Host ? d.available_name(requested, 0) : requested;
        break;
    }
    case K::Ready:
        if (d.role == Impl::Role::Guest && d.welcomed && !in_event_ && !d.pending_start) d.guest_ready = c.enabled;
        break;
    case K::Start: {
        if (d.role != Impl::Role::Host || d.pending_start || in_event_) return;
        if (!d.in_menu) { d.last_error = "Go back to the game's main menu first (leave the current event), then press START."; return; }
        for (auto &kv : d.peers) if (kv.second.alive && !kv.second.at_menu) { d.last_error = kv.second.name + " is not at the main menu yet."; return; }
        for (const auto &kv : d.peers) if (kv.second.alive && !kv.second.ready) { d.last_error = kv.second.name + " is not ready."; return; }
        if (!session_catalog::ready() || d.track_name.empty()) { d.last_error = "The game data is still loading. Wait a moment at the main menu, then press START."; return; }
        d.ensure_defaults();
        d.event_id++; d.start_info = {d.mode_name(), d.track_name, d.event_id, (uint32_t)now_us() ^ 0x5bd1e995u, d.my_vehicle, true};
        d.event_name = d.mode_name(); d.pending_start = true; d.have_track = false; d.have_roster = false; d.go = false; d.grid_event = d.applied_grid_event = 0; d.initial_grid.clear(); d.grid_acks.clear(); d.car_done = false; d.loaded_sent = false;
        for (auto &kv : d.peers) { Peer &p = kv.second; p.got_start = p.got_track = p.got_roster = p.got_go = p.car_done = false; p.seen_event = 0; p.phase = P_LOBBY; }
        d.status = "Starting event";
        if (overlay_menu_open()) overlay_toggle();
        break;
    }
    case K::EndEvent: {
        if (d.role != Impl::Role::Host || (!in_event_ && !d.pending_start)) return;
        Writer w; w.u32(d.event_id);
        for (auto &kv : d.peers) if (kv.second.alive) d.send_to(kv.second.addr, K_END, 0, w);
        if (d.pending_start) {
            d.pending_start = false;
            d.status = "Back in the lobby";
            overlay_show_online();
            return;
        }
        d.strip = "Returning to lobby...";
        d.abort_requested = true;
        session_abort_event();
        break;
    }
    case K::Cancel: case K::Disconnect: {
        if (d.role == Impl::Role::Guest && d.welcomed) { Writer w; d.send_to(d.host_addr, K_LEAVE, d.my_id, w); }
        if (d.role == Impl::Role::Host) { Writer w; for (auto &kv : d.peers) d.send_to(kv.second.addr, K_KICK, 0, w); }
        for (auto &kv : d.puppet_cars) netplay_car::set_puppet(kv.second, false);
        d.puppet_cars.clear(); d.remote.clear(); d.shown.clear();
        if (in_event_) { d.abort_requested = true; session_abort_event(); }
        d.pending_start = false; d.role = Impl::Role::None; d.peers.clear(); d.close_game(); d.status.clear(); d.welcomed = false; d.lobby_players.clear();
        d.joining_online = false; d.code.clear(); d.route_text.clear(); d.crew.clear(); d.content_note.clear();
        d.sync_voice();
        break;
    }
    case K::Kick: {
        if (d.role != Impl::Role::Host) return;
        char *identity_text = nullptr;
        const auto id = (uint8_t)std::strtoul(c.text.c_str(), &identity_text, 10);
        if (!identity_text || *identity_text != ':') return;
        const uint64_t identity = std::strtoull(identity_text + 1, nullptr, 10);
        auto it = d.peers.find(id);
        if (id == 0 || it == d.peers.end() || !it->second.alive || it->second.identity != identity) return;
        Writer notice; notice.str("Removed from the lobby by the host.",80);
        d.send_to(it->second.addr,K_KICK,0,notice);
        it->second.alive = false;
        auto puppet = d.puppet_cars.find(id);
        if (puppet != d.puppet_cars.end()) {
            netplay_car::set_puppet(puppet->second,false);
            d.puppet_cars.erase(puppet);
        }
        for (size_t slot=0; slot<d.roster.size(); ++slot) if (d.roster[slot].owner == id) {
            d.remote.erase((int)slot); d.shown.erase((int)slot);
        }
        d.status = it->second.name + " removed";
        break;
    }
    case K::Option: {
        if (d.role == Impl::Role::None || d.pending_start || in_event_) return;
        if (d.role == Impl::Role::Guest && (c.text == "car" || c.text == "paint")) d.guest_ready = false;
        d.cycle(c.text, c.value >= 0 ? 1 : -1);
        break;
    }
    case K::MicrophoneEnabled:
        d.mic_enabled = c.enabled;
        d.sync_voice();
        break;
    case K::Muted:
        d.muted_all = c.enabled;
        d.sync_voice();
        break;
    case K::Deafened:
        d.deafened = c.enabled;
        d.sync_voice();
        break;
    default: break;
    }
}

// ====================================================================== network pump
// Developer/automation: EOD_NETPLAY_AUTO=host | join:<ip[:port]> drives the lobby without the menu
// (host presses START by itself once a guest is in and both are at the main menu).
void Session::auto_drive(uint64_t t) {
    Impl &d = *d_;
    Session &s = *this;
    static const char *mode = std::getenv("EOD_NETPLAY_AUTO");
    if (!mode || !*mode) return;
    static uint64_t t0 = t, ready_since = 0;
    using game_frontend::Command;
    if (d.role == Impl::Role::None && t - t0 > 8000000 && !d.event_id) {
        Command c{};
        if (!std::strcmp(mode, "host")) c.kind = Command::Kind::Host;
        else if (!std::strncmp(mode, "join:", 5)) { c.kind = Command::Kind::JoinAddress; c.text = mode + 5; }
        else if (!std::strncmp(mode, "joincode:", 9)) { c.kind = Command::Kind::JoinCode; c.text = mode + 9; }
        else if (!std::strncmp(mode, "joinfriend", 10)) {   // first remembered friend
            d.load_friends(); if (d.friends.empty()) return;
            c.kind = Command::Kind::JoinCrew; c.text = d.friends[0].secret;
        }
        else return;
        s.submit(c);
        t0 = t;
    }
    { static uint64_t lastp = 0; if (d.role == Impl::Role::Host && t - lastp > 3000000) { lastp = t; std::fprintf(stderr, "[netplay] auto: peers=%d in_menu=%d event=%u\n", d.alive_count(), (int)d.in_menu, d.event_id); } }
    if (d.role == Impl::Role::Guest && d.welcomed && d.in_menu && !d.guest_ready) {
        Command ready{}; ready.kind = Command::Kind::Ready; ready.enabled = true; s.submit(ready);
    }
    if (std::getenv("EOD_NETPLAY_TEST_MICROPHONE") && !d.mic_enabled && d.role != Impl::Role::None) {
        Command mic{}; mic.kind = Command::Kind::MicrophoneEnabled; mic.enabled = true; s.submit(mic);
    }
    // Opt-in actual-session probe. Script rows: sequence delay_ms action args.
    // Delays are relative to hosting; monotonically increasing sequences execute
    // once, so a test may append commands after observing UDP assertions.
    if (const char *path = std::getenv("EOD_NETPLAY_TEST_LOBBY")) {
        static uint64_t began = 0, checked = 0; static unsigned sequence = 0;
        if (d.role == Impl::Role::Host && !began) began = t;
        if (began && d.role == Impl::Role::Host && t - checked >= 100000) {
            checked = t; std::ifstream script(path); std::string line;
            while (std::getline(script, line)) {
                std::istringstream row(line); unsigned seq; uint64_t delay; std::string action;
                if (!(row >> seq >> delay >> action) || seq <= sequence || t - began < delay * 1000) continue;
                Command command{};
                if (action == "option") { command.kind = Command::Kind::Option; if (!(row >> command.text >> command.value)) continue; }
                else if (action == "nickname") { command.kind = Command::Kind::Nickname; std::getline(row >> std::ws, command.text); }
                else if (action == "kick") { command.kind = Command::Kind::Kick; if (!(row >> command.text)) continue; }
                else if (action == "kick_current") {
                    unsigned slot; if (!(row >> slot)) continue;
                    command.kind = Command::Kind::Kick;
                    const auto snapshot = s.snapshot();
                    for (const auto &player : snapshot.players) if (player.slot == slot)
                        command.text = std::to_string(slot) + ":" + std::to_string(player.identity);
                } else continue;
                s.submit(command); sequence = seq;
                std::fprintf(stderr, "[netplay-test] lobby seq=%u action=%s cap=%u rules=%u peers=%d\n",
                             seq, action.c_str(), d.max_players, d.rules, d.alive_count());
            }
        }
    }
    const bool repeat = std::getenv("EOD_NETPLAY_TEST_REMATCH") != nullptr;
    if (repeat && s.is_host() && in_event_ && !d.abort_requested && d.tick >= 120) {
        Command end{}; end.kind = Command::Kind::EndEvent; s.submit(end);
    }
    if (d.role == Impl::Role::Host && !s.event_active() && !d.pending_start &&
        (!d.event_id || (repeat && d.event_id == 1)) && (d.alive_count() >= 1 || std::getenv("EOD_NETPLAY_TEST_SOLO")) && d.in_menu) {
        if (!ready_since) ready_since = t;
        if (t - ready_since > 3000000) { ready_since = t; Command c{}; c.kind = Command::Kind::Start; s.submit(c); if (!d.last_error.empty()) { std::fprintf(stderr, "[netplay] start refused: %s\n", d.last_error.c_str()); ready_since = t; } }
    }
}

void Session::poll() {
    Impl &d = *d_;
    if (d.abort_requested && g_ram && ee_r32(0x00437BB8) != 0) {
        session_abort_event();
    }
    static const bool trace = std::getenv("EOD_NETPLAY_LIFECYCLE") != nullptr;
    if (trace) {
        static std::string previous;
        const char *state = d.abort_requested ? "returning" : in_event_ ? (d.bound ? "racing" : "loading") :
                            d.pending_start ? "starting" : d.role == Impl::Role::None ? "offline" :
                            d.role == Impl::Role::Guest && !d.welcomed ? "connecting" : "lobby";
        const std::string key = std::string(state) + ":" + std::to_string(d.event_id) + ":" +
                                std::to_string(d.in_menu) + ":" + std::to_string((int)d.role);
        if (key != previous) {
            previous = key;
            std::fprintf(stderr, "[lifecycle] ms=%u role=%d event=%u state=%s native_main=%d peers=%d\n",
                         now_ms(), (int)d.role, d.event_id, state, (int)d.in_menu, d.alive_count());
        }
    }
    auto_drive(now_us());
    const uint64_t t = now_us();
    d.poll_voice(t);
    if (std::getenv("EOD_NETPLAY_TEST_VOICE_PCM") && (std::getenv("EOD_HEADLESS") || std::getenv("EOD_MUTE"))) {
        int16_t stereo[1920]{};
        static uint64_t last = 0;
        if (t - last >= 20000) { last=t; mix_voice(stereo,960); }
    }
    { static uint64_t last = 0; static const bool dbg = std::getenv("EOD_NETPLAY_DEBUG") != nullptr; if (dbg && t - last > 20000000) { last = t; std::fprintf(stderr, "[netplay] poll role=%d fd=%d welcomed=%d\n", (int)d.role, d.fd, (int)d.welcomed); } }
    // LAN discovery listener
    if (d.disc_fd >= 0) {
        uint8_t buf[256]; sockaddr_in from; socklen_t fl = sizeof from;
        for (int n; (n = (int)recvfrom(d.disc_fd, (char *)buf, sizeof buf, 0, (sockaddr *)&from, &fl)) > 0; fl = sizeof from) {
            Reader r(buf, (size_t)n);
            if (r.u16() != kMagic || r.u8() != K_BEACON) continue;
            r.u8(); uint32_t proto = r.u16(); uint16_t gp = (uint16_t)r.u16(); std::string nm = r.str(); uint8_t pc = (uint8_t)r.u8();
            if (!r.ok || proto != kProtocol) continue;
            sockaddr_in a = from; a.sin_port = htons(gp);
            if (d.role == Impl::Role::Host && d.fd >= 0) { sockaddr_in me{}; socklen_t ml = sizeof me; getsockname(d.fd, (sockaddr *)&me, &ml); if (gp == d.port && a.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) continue; }
            d.lan[sockaddr_text(a)] = {a, nm, pc, t};
        }
    }
    // Remembered friends: while the ONLINE tab is open (and no game is joined/hosted) look for hosts among them.
    if (d.role == Impl::Role::None && !std::getenv("EOD_NETPLAY_NO_INTERNET")) {
        const bool tab_open = t - d.ui_seen < 3000000;
        d.load_friends();
        if (tab_open && !d.friends.empty()) {
            if (d.fd < 0) d.open(0, false);
            if (d.fd >= 0 && !d.online_on) { d.online_start(false); d.watch_jobs(); }
        } else if (d.online_on && !tab_open) {
            d.close_game(); d.crew.clear(); d.route_text.clear();
        }
    }
    if (d.fd < 0) return;
    if (d.online_on) d.route_text = d.route_description(d.online.report());

    uint8_t buf[1500]; sockaddr_in from; socklen_t fl = sizeof from;
    for (int n; (n = (int)recvfrom(d.fd, (char *)buf, sizeof buf, 0, (sockaddr *)&from, &fl)) > 0; fl = sizeof from) {
        if (d.online_on && netplay_online::looks_like_stun(buf, (size_t)n)) { d.online.feed_stun(buf, (size_t)n); continue; }
        Reader r(buf, (size_t)n);
        if (r.u16() != kMagic) continue;
        const uint8_t kind = (uint8_t)r.u8(), sender = (uint8_t)r.u8();
        if (!r.ok) continue;
        if (kind == K_PUNCH) continue;                      // its only job was to open our router for the sender
        if (kind == K_PROBE) {                              // a remembered friend asks whether we are hosting
            const uint32_t tag = r.u32();
            if (r.ok && d.role == Impl::Role::Host && !in_event_ && d.tag_known(tag, nullptr)) {
                Writer w; w.u32(tag); w.str(d.name, 16); w.u8(1 + (uint32_t)d.alive_count());
                d.send_to(from, K_PROBE_REPLY, 0, w);
            }
            continue;
        }
        if (kind == K_PROBE_REPLY) {
            const uint32_t tag = r.u32(); std::string nm = r.str(); const uint32_t pc = r.u8();
            if (!r.ok) continue;
            for (auto &f : d.friends)
                if (tag == netplay_online::join_tag(f.secret)) { CrewHost &c = d.crew[f.secret]; c.addr = from; c.name = f.name; c.players = (uint8_t)pc; c.seen = t; }
            continue;
        }
        if (d.role == Impl::Role::None) continue;
        static const bool dbg = std::getenv("EOD_NETPLAY_DEBUG") != nullptr;
        if (dbg && kind != K_SNAP && kind != K_HEARTBEAT && kind != K_LOBBY && kind != K_VOICE) std::fprintf(stderr, "[netplay] rx kind %u from %s (%d bytes)\n", kind, sockaddr_text(from).c_str(), n);
        if (d.role == Impl::Role::Host) {
            // ---- host receives
            if (kind == K_JOIN) {
                uint32_t proto = r.u16(); std::string nm = r.str(); const uint32_t tag = r.u32();
                if (!r.ok) continue;
                std::string friend_secret;
                if (tag != 0 && !d.tag_known(tag, &friend_secret)) continue;   // meant for another game: say nothing
                Peer *pp = nullptr;
                for (auto &kv : d.peers) if (kv.second.alive && same_addr(kv.second.addr, from)) pp = &kv.second;
                Writer w;
                if (proto != kProtocol) { w.u8(1); d.send_to(from, K_REFUSE, 0, w); continue; }
                const auto content = r.str();
                if (!r.ok || content != d.content_id) { w.u8(4); d.send_to(from,K_REFUSE,0,w); continue; }
                if (!pp) {
                    if (in_event_) { w.u8(2); d.send_to(from, K_REFUSE, 0, w); continue; }   // late joiners wait for the next event
                    uint8_t id = 1; while (d.peers.count(id) && d.peers[id].alive) ++id;
                    if (id >= kMaxPlayers || 1 + d.alive_count() >= (int)d.max_players) { w.u8(3); d.send_to(from, K_REFUSE, 0, w); continue; }
                    Peer p; p.identity = ++d.next_peer_identity; p.addr = from; p.id = id; p.requested_name = nm; p.name = d.available_name(nm,id); p.last_heard = t;
                    p.secret = !friend_secret.empty() ? friend_secret : Impl::new_secret();
                    d.peers[id] = p; pp = &d.peers[id];
                    d.remember(nm, p.secret);
                    std::fprintf(stderr, "[netplay] %s joined from %s as player %u\n", nm.c_str(), sockaddr_text(from).c_str(), id);
                }
                w.u8(pp->id);
                { std::vector<uint8_t> raw; netplay_online::unhex(pp->secret, raw); for (uint8_t b : raw) w.u8(b); }
                w.str(d.content_id,16);
                d.send_to(from, K_WELCOME, 0, w);
                pp->last_heard = t;
                d.status = std::to_string(1 + d.alive_count()) + " players in the lobby";
                continue;
            }
            auto it = d.peers.find(sender);
            if (it == d.peers.end() || !it->second.alive || !same_addr(it->second.addr, from)) continue;
            Peer &p = it->second;
            p.last_heard = t;
            if (kind == K_HEARTBEAT) {
                const auto requested = clean_name(r.str());
                if (requested != p.requested_name) { p.requested_name = requested; p.name = d.available_name(requested, p.id); }
                p.ready = r.u8(); p.vehicle = r.u32(); p.vname = r.str(); p.phase = (uint8_t)r.u8(); p.seen_event = r.u32();
                uint32_t flags = r.u8(); uint32_t echo = r.u32();
                p.at_menu = flags & 1; p.got_track = flags & 2; p.got_roster = flags & 4; p.got_go = flags & 8; p.got_start = flags & 16; p.car_done = flags & 32;
                if (echo && echo == p.last_ts_sent) p.ping_ms = now_ms() - echo;
            } else if (kind == K_LEAVE) {
                p.alive = false;
                std::fprintf(stderr, "[netplay] %s left\n", p.name.c_str());
                d.status = p.name + " left";
                if (in_event_) { auto pc = d.puppet_cars.find(p.id); if (pc != d.puppet_cars.end()) { netplay_car::set_puppet(pc->second, false); d.puppet_cars.erase(pc); } }
            } else if (kind == K_GRID_ACK) {
                const uint32_t id = r.u32();
                if (r.ok && id == d.event_id && d.grid_event == id) d.grid_acks[p.id] = id;
            } else if (kind == K_SNAP) {
                const uint32_t id = r.u32();
                if (!r.ok || id != d.event_id || !in_event_ || !d.bound || !d.go) continue;
                uint32_t n_cars = r.u8();
                for (uint32_t i = 0; i < n_cars && r.ok; ++i) {
                    uint8_t slot; uint32_t tick; netplay_car::State s;
                    if (!get_car(r, slot, tick, s)) break;
                    if (d.roster.size() > slot && d.roster[slot].owner == p.id && (!p.has_latest || tick > p.latest_tick)) { d.remote[slot].push(tick, s); p.latest = s; p.latest_tick = tick; p.has_latest = true; p.latest_at = t; }
                }
            } else if (kind == K_VOICE) {
                if (d.voice.receive(sender, r.p, (size_t)(r.e - r.p)) == netplay::voice::Receive::Accepted) {
                    if (sender < kMaxPlayers) d.speaking_until[sender] = t + 300000;
                    Writer w; w.b.assign(r.p, r.e);
                    for (auto &kv : d.peers)
                        if (kv.second.alive && kv.first != sender)
                            d.send_to(kv.second.addr, K_VOICE, sender, w);
                }
            }
        } else {
            // ---- guest receives (only from the host)
            if (!d.welcomed && d.joining_online && (kind == K_WELCOME || kind == K_REFUSE)) {
                for (auto &a : d.targets) if (same_addr(a, from)) { d.host_addr = from; break; }
            }
            if (!same_addr(from, d.host_addr)) { static int warned = 0; if (warned++ < 5) std::fprintf(stderr, "[netplay] ignoring packet kind %u from %s (host is %s)\n", kind, sockaddr_text(from).c_str(), sockaddr_text(d.host_addr).c_str()); continue; }
            d.host_last_heard = t;
            if (kind == K_WELCOME) {
                const uint8_t assigned = (uint8_t)r.u8();
                uint8_t raw[16]; bool have = true; for (auto &b : raw) { b = (uint8_t)r.u8(); } have = r.ok;
                const auto content = r.str();
                if (!r.ok || !have || assigned == 0 || assigned >= kMaxPlayers || content != d.content_id) {
                    d.last_error = "Game content does not match the host. Install the same gameplay mods.";
                    d.role = Impl::Role::None; d.close_game(); d.sync_voice(); return;
                }
                d.my_id = assigned; d.welcomed = true; d.status = "Connected";
                d.join_secret = netplay_online::hex(raw,16); d.sync_voice();
                std::fprintf(stderr, "[netplay] connected, player %u (via %s)\n", d.my_id, sockaddr_text(from).c_str());
            }
            else if (kind == K_REFUSE) {
                uint32_t why = r.u8();
                d.last_error = why == 1 ? "The host has a different game version." : why == 2 ? "An event is running. You can join when it ends." : why == 4 ? "Game content does not match. Install the same gameplay mods as the host." : "The game is full.";
                d.role = Impl::Role::None; d.close_game(); d.sync_voice();
                return;
            } else if (kind == K_KICK) {
                d.last_error = r.p != r.e ? r.str() : "The host closed the game.";
                for (auto &kv : d.puppet_cars) netplay_car::set_puppet(kv.second, false);
                d.puppet_cars.clear(); d.remote.clear(); d.shown.clear();
                if (in_event_) { d.abort_requested = true; session_abort_event(); }
                d.pending_start = false; d.role = Impl::Role::None; d.close_game(); d.lobby_players.clear(); d.sync_voice();
                if (overlay_menu_open() || !in_event_) overlay_show_online();
                return;
            } else if (kind == K_LOBBY) {
                d.last_host_ts = r.u32(); uint32_t ev = r.u32(); d.host_phase = (uint8_t)r.u8(); std::string le = r.str(); (void)r.u8();
                uint32_t n = r.u8(); std::vector<LobbyPlayer> v;
                for (uint32_t i = 0; i < n && r.ok; ++i) { LobbyPlayer p; p.id = (uint8_t)r.u8(); p.name = r.str(); p.ready = r.u8(); p.vname = r.str(); p.ping = (uint16_t)r.u16(); v.push_back(p); }
                std::string lt = r.str(); const uint8_t rules = r.u8(); const uint8_t capacity = r.u8();
                if (r.ok) { d.max_players = std::clamp<unsigned>(capacity,1,kMaxPlayers); d.lobby_players = v; d.lobby_event = le; d.lobby_track = lt; if (!in_event_ && !d.pending_start) d.rules = rules; }
                (void)ev;
                if (r.ok && !d.remembered && !v.empty() && d.join_secret.size() == 32) { d.remembered = true; d.remember(v[0].name, d.join_secret); }
            } else if (kind == K_START) {
                uint32_t id = r.u32(); std::string ev = r.str(); uint32_t seed = r.u32(); std::string tr = r.str(); uint8_t rules = r.u8();
                if (r.ok && id > d.event_id) {
                    d.ensure_defaults();
                    d.rules = rules; d.event_id = id; d.event_name = ev; d.pending_start = true; d.start_info = {ev, tr, id, seed, d.my_vehicle, false};
                    d.have_track = false; d.have_roster = false; d.go = false; d.grid_event = d.applied_grid_event = 0; d.initial_grid.clear(); d.grid_acks.clear(); d.car_done = false; d.loaded_sent = false; d.my_phase = P_LOBBY;
                    if (overlay_menu_open()) overlay_toggle();
                }
            } else if (kind == K_TRACK) {
                uint32_t id = r.u32(); std::string tr = r.str();
                if (r.ok && id == d.event_id) { d.track = tr; d.have_track = true; }
            } else if (kind == K_ROSTER) {
                uint32_t id = r.u32(); uint32_t n = r.u8(); std::vector<RosterSlot> v;
                for (uint32_t i = 0; i < n && r.ok; ++i) { RosterSlot s; s.driver = r.u32(); s.vehicle = r.u32(); s.owner = (uint8_t)r.u8(); v.push_back(s); }
                if (r.ok && id == d.event_id && !d.have_roster) { d.roster = v; d.have_roster = true; }
            } else if (kind == K_GRID) {
                const uint32_t id = r.u32(), count = r.u8();
                std::vector<netplay_car::State> grid;
                bool complete = count > 0 && count <= 16;
                for (uint32_t i = 0; i < count && complete; ++i) {
                    uint8_t slot; uint32_t tick; netplay_car::State state;
                    complete = get_car(r, slot, tick, state) && slot == i && tick == 0;
                    if (complete) grid.push_back(state);
                }
                if (r.ok && r.p == r.e && complete && id == d.event_id && !d.go && d.applied_grid_event != id) {
                    d.initial_grid = grid; d.grid_event = id;
                }
                // A duplicate GRID can mean the ACK was lost. Never reapply it.
                if (id == d.event_id && d.applied_grid_event == id) {
                    Writer ack; ack.u32(id); d.send_to(d.host_addr, K_GRID_ACK, d.my_id, ack);
                }
            } else if (kind == K_GO) {
                uint32_t id = r.u32(); if (r.ok && id == d.event_id && d.applied_grid_event == id) d.go = true;
            } else if (kind == K_END) {
                uint32_t id = r.u32();
                if (r.ok && id == d.event_id && d.pending_start) {
                    d.pending_start = false;
                    d.status = "Back in the lobby";
                    overlay_show_online();
                } else if (r.ok && id == d.event_id && in_event_) {
                    d.strip = "Returning to lobby...";
                    if (in_event_) { d.abort_requested = true; session_abort_event(); }
                }
            } else if (kind == K_SNAP) {
                const uint32_t id = r.u32();
                if (!r.ok || id != d.event_id || !in_event_ || !d.bound || !d.go) continue;
                uint32_t n_cars = r.u8();
                for (uint32_t i = 0; i < n_cars && r.ok; ++i) {
                    uint8_t slot; uint32_t tick; netplay_car::State s;
                    if (!get_car(r, slot, tick, s)) break;
                    if (slot < d.roster.size() && d.roster[slot].owner != d.my_id) d.remote[slot].push(tick, s);
                }
            } else if (kind == K_VOICE) {
                if (d.voice.receive(sender, r.p, (size_t)(r.e - r.p)) == netplay::voice::Receive::Accepted) {
                    if (sender < kMaxPlayers) d.speaking_until[sender] = t + 300000;
                }
            }
        }
    }

    if (d.role == Impl::Role::None) {
        // watching friends: ask every host the trackers know under a friend's key whether it is really hosting now
        if (d.online_on && t - d.last_probe > 1500000) {
            d.last_probe = t;
            for (auto &f : d.friends)
                for (auto &a : d.online.found("ch:" + f.secret)) {
                    Writer w; w.u32(netplay_online::join_tag(f.secret));
                    sockaddr_in to = Impl::to_sockaddr(a);
                    d.send_to(to, K_PROBE, 0, w);
                }
        }
        return;
    }
    if (d.role == Impl::Role::Host) {
        // internet: punch towards whoever registered as a joiner (opens our router for their JOIN)
        if (d.online_on && !in_event_ && t - d.last_punch > 400000) {
            d.last_punch = t;
            std::vector<netplay_online::Addr> addrs = d.online.found("joiners");
            for (auto &f : d.friends) { auto more = d.online.found("cj:" + f.secret); addrs.insert(addrs.end(), more.begin(), more.end()); }
            for (auto &a : addrs) { Writer w; sockaddr_in to = Impl::to_sockaddr(a); d.send_to(to, K_PUNCH, 0, w); }
        }
        // timeouts
        for (auto &kv : d.peers) {
            Peer &p = kv.second;
            if (p.alive && t - p.last_heard > kPeerTimeoutUs) {
                p.alive = false; d.status = p.name + " lost connection";
                std::fprintf(stderr, "[netplay] %s timed out\n", p.name.c_str());
                auto pc = d.puppet_cars.find(p.id); if (pc != d.puppet_cars.end()) { netplay_car::set_puppet(pc->second, false); d.puppet_cars.erase(pc); }
            }
        }
        // lobby state out
        const uint64_t period = in_event_ ? 500000 : 250000;
        if (t - d.last_lobby_send > period) {
            d.last_lobby_send = t;
            for (auto &kv : d.peers) {
                Peer &p = kv.second; if (!p.alive) continue;
                Writer w; p.last_ts_sent = now_ms() ? now_ms() : 1; d.put_lobby(w, p.last_ts_sent);
                d.send_to(p.addr, K_LOBBY, 0, w);
            }
        }
        // event flow: repeat until each guest confirms
        if (d.event_id && d.pending_start + in_event_ + (d.my_phase != P_LOBBY) > 0) {
            for (auto &kv : d.peers) {
                Peer &p = kv.second; if (!p.alive) continue;
                if (!p.got_start || p.seen_event != d.event_id) {
                    Writer w; w.u32(d.event_id); w.str(d.event_name, 40); w.u32(d.start_info.seed); w.str(d.start_info.track, 40); w.u8(d.rules); d.send_to(p.addr, K_START, 0, w);
                }
                if (d.have_track && !p.got_track) { Writer w; w.u32(d.event_id); w.str(d.track, 40); d.send_to(p.addr, K_TRACK, 0, w); }
                if (d.have_roster && !p.got_roster) {
                    Writer w; w.u32(d.event_id); w.u8((uint32_t)d.roster.size());
                    for (auto &s : d.roster) { w.u32(s.driver); w.u32(s.vehicle); w.u8(s.owner); }
                    d.send_to(p.addr, K_ROSTER, 0, w);
                }
                if (d.grid_event == d.event_id && d.grid_acks[p.id] != d.event_id) {
                    Writer grid; grid.u32(d.event_id); grid.u8((uint32_t)d.initial_grid.size());
                    for (uint32_t i = 0; i < d.initial_grid.size(); ++i) put_car(grid, (uint8_t)i, 0, d.initial_grid[i]);
                    d.send_to(p.addr, K_GRID, 0, grid);
                }
                if (d.go && !p.got_go) { Writer w; w.u32(d.event_id); d.send_to(p.addr, K_GO, 0, w); }
            }
        }
        // Repeat the end notice until the guest heartbeat acknowledges the lobby.
        if (!in_event_ && !d.pending_start && d.event_id) {
            for (auto &kv : d.peers) {
                const Peer &p = kv.second;
                if (p.alive && p.seen_event == d.event_id && p.phase != P_LOBBY) {
                    Writer end; end.u32(d.event_id); d.send_to(p.addr, K_END, 0, end);
                }
            }
        }
        // LAN beacon
        if (!in_event_ && !std::getenv("EOD_NETPLAY_NO_LAN") && t - d.last_beacon > 1000000) {
            d.last_beacon = t;
            Writer w; w.u16(kProtocol); w.u16(d.port); w.str(d.name, 16); w.u8(1 + (uint32_t)d.alive_count());
            sockaddr_in b{}; b.sin_family = AF_INET; b.sin_addr.s_addr = htonl(INADDR_BROADCAST); b.sin_port = htons(kDiscoveryPort);
            d.send_to(b, K_BEACON, 0, w);
            sockaddr_in lo{}; lo.sin_family = AF_INET; lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK); lo.sin_port = htons(kDiscoveryPort);
            d.send_to(lo, K_BEACON, 0, w);
        }
    } else if (d.role == Impl::Role::Guest) {
        if (!d.welcomed && d.joining_online) {
            // Garage Code / friend: JOIN goes to every address the rendezvous found for the host (hole punching: both sides send)
            const netplay_online::Report rep = d.online.report();
            d.route_text = d.route_description(rep);
            std::vector<sockaddr_in> cand;
            for (auto &a : d.online.found("hosts")) cand.push_back(Impl::to_sockaddr(a));
            if (!cand.empty() && cand.size() != d.targets.size()) std::fprintf(stderr, "[netplay] found %zu candidate address(es) for the host\n", cand.size());
            d.targets = cand;
            if (t - d.last_join > 400000) {
                d.last_join = t;
                Writer w; w.u16(kProtocol); w.str(d.name, 16); w.u32(d.join_tag); w.str(d.content_id,16);
                for (auto &a : d.targets) d.send_to(a, K_JOIN, 0, w);
            }
            const std::string what = d.code.empty() ? "your friend's game" : "game " + d.code;
            d.join_note = !rep.stun_done ? "Looking for the best route..." : d.targets.empty() ? "Looking for " + what + "..." : "Found " + what + ", connecting...";
            const uint64_t elapsed = t - d.join_started;
            if (elapsed > 30000000 || (elapsed > 12000000 && rep.stun_done && !rep.internet())) {
                if (!rep.internet()) d.last_error = "Could not reach the internet. Check your connection, then try again.";
                else if (d.targets.empty()) d.last_error = d.code.empty() ? "Your friend is not hosting right now. Ask them to press HOST GAME, then try again."
                                                                          : "Nobody is hosting " + d.code + ". Check the code, or ask the host to press HOST GAME again.";
                else if (rep.symmetric) d.last_error = "Found the game, but your internet connection is too strict for direct play (it uses a different address for every destination). Try home Wi-Fi instead of a phone hotspot.";
                else d.last_error = "Found the game, but no connection could be made (a router is blocking it). Try again; if it keeps failing, join from the host's Wi-Fi or ask them to turn on UPnP in their router.";
                std::fprintf(stderr, "[netplay] join failed: %s\n%s", d.last_error.c_str(), d.online.log_tail().c_str());
                d.role = Impl::Role::None; d.close_game(); d.joining_online = false; d.route_text.clear();
            }
            return;
        }
        if (!d.welcomed) {
            if (t - d.last_join > 400000) { d.last_join = t; Writer w; w.u16(kProtocol); w.str(d.name, 16); w.u32(0); w.str(d.content_id,16); d.send_to(d.host_addr, K_JOIN, 0, w); }
            if (t - d.host_last_heard > 8000000) { d.last_error = "Could not reach the host. Check the address and that both of you are on the same network."; d.role = Impl::Role::None; d.close_game(); }
            return;
        }
        const uint64_t period = (in_event_ || d.my_phase != P_LOBBY) ? 100000 : 250000;
        if (t - d.last_hb > period) {
            d.last_hb = t;
            Writer w; w.str(d.name, 16); w.u8(d.guest_ready); w.u32(d.my_vehicle); w.str(d.my_vname, 24); w.u8(d.my_phase); w.u32(d.event_id);
            const bool at_menu = !in_event_ && !d.pending_start && d.in_menu;
            w.u8((at_menu ? 1 : 0) | (d.have_track ? 2 : 0) | (d.have_roster ? 4 : 0) | (d.go ? 8 : 0) | ((d.event_id && (d.pending_start || in_event_ || d.my_phase != P_LOBBY)) ? 16 : 0) | (d.car_done ? 32 : 0));
            w.u32(d.last_host_ts);
            d.send_to(d.host_addr, K_HEARTBEAT, d.my_id, w);
        }
        if (t - d.host_last_heard > kPeerTimeoutUs) {
            d.last_error = "Lost connection to the host.";
            for (auto &kv : d.puppet_cars) netplay_car::set_puppet(kv.second, false);
            d.puppet_cars.clear(); d.remote.clear(); d.shown.clear();
            if (in_event_) { d.abort_requested = true; session_abort_event(); }
            d.pending_start = false; d.role = Impl::Role::None; d.close_game(); d.lobby_players.clear(); d.sync_voice();
            if (!in_event_) overlay_show_online();
        }
    }
}

bool Session::wait_until(const std::function<bool()> &done, int timeout_ms, const char *what) {
    const uint64_t t0 = now_us();
    d_->strip = what;
    bool ok = false;
    for (;;) {
        poll();
        if (done()) { ok = true; break; }
        if (d_->role == Impl::Role::None || d_->abort_requested) break;
        if ((int)((now_us() - t0) / 1000) > timeout_ms) break;
        if (!platform_pump()) break;
        overlay_frame();
        platform_sleep_us(4000);
    }
    d_->strip.clear();
    return ok;
}

// ====================================================================== game hooks
void Session::menu_enter() {
    d_->in_menu = true;
    if (active() && session_catalog::refresh()) d_->ensure_defaults();
    static bool once = false; if (!once) { once = true; std::fprintf(stderr, "[netplay] main menu reached\n"); }
}
void Session::menu_leave() { d_->in_menu = false; }
// The native main menu is a blocking loop with no hook to start a game from. When a lobby START is
// waiting, press the buttons that choose ACTION (the game's own single event path, which the
// online flow then drives through its native car and track screens).
uint16_t Session::menu_buttons() {
    Impl &d = *d_;
    if (netplay_hooks::title_screen && active()) return (d.inject_count++ % 12 < 3) ? 0x0008 : 0;
    if (d.in_menu && !session_catalog::ready() && (d.role != Impl::Role::None || now_us() - d.ui_seen < 3000000)) { session_catalog::refresh(); d.ensure_defaults(); }
    if (!d.in_menu || !d.pending_start || d.role == Impl::Role::None) return 0;
    const uint32_t c = d.inject_count++;
    if ((c % 6) >= 2) return 0;
    StartInfo info;
    if (take_start(info) && netplay_hooks::arm) netplay_hooks::arm(info);
    return 0x4000;                             // CROSS
}
bool Session::take_start(StartInfo &out) {
    Impl &d = *d_;
    if (!d.pending_start || d.role == Impl::Role::None) return false;
    d.pending_start = false;
    out = d.start_info;
    in_event_ = true;
    d.my_phase = P_STARTED; d.tick = 0; d.bound = false; d.my_slot = -1; d.remote.clear(); d.puppet_cars.clear(); d.roster.clear(); d.have_roster = false;
    d.have_track = d.role == Impl::Role::Host ? false : false; d.strip.clear();
    std::fprintf(stderr, "[netplay] event %u '%s' starting (%s)\n", out.event_id, out.event.c_str(), out.host ? "host" : "guest");
    return true;
}
void Session::car_chosen(uint32_t vehicle, const std::string &) {
    Impl &d = *d_;
    d.my_vehicle = vehicle; d.my_vname = vehicle_name(vehicle); d.car_done = true; d.my_phase = P_CAR_DONE;
    std::fprintf(stderr, "[netplay] my car: %s (#%u)\n", d.my_vname.c_str(), vehicle);
}
void Session::track_chosen(const std::string &track) {
    Impl &d = *d_;
    d.track = track; d.have_track = true;
    std::fprintf(stderr, "[netplay] host picked track '%s'\n", track.c_str());
}
bool Session::wait_track(std::string &track) {
    Impl &d = *d_;
    if (!wait_until([&] { return d.have_track; }, 300000, "Waiting for the host to pick the track")) return false;
    track = d.track;
    return true;
}
std::string Session::strip() const { return d_->strip; }
int Session::online_rules() const { return active() ? d_->rules : -1; }
bool Session::active() const { return in_event_ || d_->role != Impl::Role::None; }
bool Session::is_host() const { return d_->role == Impl::Role::Host; }
uint32_t Session::remote_humans() const { return in_event_ ? (uint32_t)(d_->role == Impl::Role::Host ? d_->alive_count() : (d_->lobby_players.size() > 1 ? d_->lobby_players.size() - 1 : 0)) : 0; }

void Session::mix_voice(int16_t *stereo, int frames) {
    Impl &d = *d_;
    std::lock_guard<std::mutex> lk(d.voice_mtx);
    if (d.voice_pos >= d.voice_out.size()) return;
    if (const char *capture = std::getenv("EOD_NETPLAY_TEST_VOICE_PCM")) {
        static uint64_t samples_written = 0;
        if (samples_written < 48000 * 15) {
            FILE *f = std::fopen(capture, samples_written ? "ab" : "wb");
            if (f) {
                const size_t n = std::min<size_t>((size_t)frames, d.voice_out.size() - d.voice_pos);
                std::fwrite(d.voice_out.data()+d.voice_pos,sizeof(int16_t),n,f);
                samples_written += n;
                std::fclose(f);
            }
        }
    }
    const int vol = std::clamp(g_config.volume, 0, 100);
    for (int i = 0; i < frames && d.voice_pos < d.voice_out.size(); ++i, ++d.voice_pos) {
        const int s = (int)d.voice_out[d.voice_pos] * vol / 100;
        stereo[i * 2 + 0] = (int16_t)std::clamp(stereo[i * 2 + 0] + s, -32768, 32767);
        stereo[i * 2 + 1] = (int16_t)std::clamp(stereo[i * 2 + 1] + s, -32768, 32767);
    }
    if (d.voice_pos >= d.voice_out.size()) {
        d.voice_out.clear();
        d.voice_pos = 0;
    }
}

std::vector<std::string> Session::speakers() const {
    const Impl &d = *d_;
    const uint64_t t = now_us();
    std::vector<std::string> out;
    if (d.role == Impl::Role::Host) {
        if (t < d.speaking_until[0]) out.push_back(d.name);
        for (const auto &kv : d.peers)
            if (kv.second.alive && kv.first < kMaxPlayers && t < d.speaking_until[kv.first])
                out.push_back(kv.second.name);
    } else if (d.role == Impl::Role::Guest && d.welcomed) {
        for (const auto &p : d.lobby_players)
            if (p.id < kMaxPlayers && t < d.speaking_until[p.id])
                out.push_back(p.name);
        if (out.empty() && d.my_id < kMaxPlayers && t < d.speaking_until[d.my_id])
            out.push_back(d.name);
    } else {
        if (t < d.speaking_until[0]) out.push_back(d.name);
    }
    return out;
}

void Session::roster_pre(uint32_t description) {
    Impl &d = *d_;
    d.content_note.clear();
    const uint32_t count = ee_r32(description + 0x98C);
    if (count == 0 || count > 16) return;
    auto rec = [&](uint32_t i) { return description + 0x2C + i * 0x78; };
    if (d.role == Impl::Role::Host) {
        d.my_phase = P_CAR_DONE;
        // everyone needs to have picked a car before the grid is decided
        if (!wait_until([&] { for (auto &kv : d.peers) if (kv.second.alive && !(kv.second.car_done && kv.second.seen_event == d.event_id)) return false; return true; },
                   300000, "Waiting for everyone to choose a car")) {
            error_ = d.last_error = "Not everyone finished choosing a car.";
            d.abort_requested = true; session_abort_event(); return;
        }
        if (d.abort_requested) return;
        if (count < (uint32_t)(1 + d.alive_count())) {
            error_ = d.last_error = "This event has too few starting seats for everyone. Choose a larger event.";
            d.abort_requested = true; session_abort_event(); return;
        }
        // Reserve exactly one host seat, by native pad-zero identity. Duplicate
        // vehicle selections are valid and must not consume additional seats.
        uint32_t host_slot = count;
        for (uint32_t i = 0; i < count; ++i)
            if (ee_r32(rec(i) + 0x68) == d.my_vehicle) { host_slot = i; break; }
        if (host_slot == count) {
            error_ = d.last_error = "The host's selected car is missing from the starting grid.";
            d.abort_requested = true; session_abort_event(); return;
        }
        d.roster.assign(count, RosterSlot{});
        d.roster[host_slot].owner = 0;
        uint32_t seat = 0;
        for (auto &kv : d.peers) {
            Peer &p = kv.second; if (!p.alive) continue;
            if (!p.vehicle) { d.abort_requested = true; session_abort_event(); return; }
            while (seat < count && d.roster[seat].owner != 0xFF) ++seat;
            if (seat == count) { d.abort_requested = true; session_abort_event(); return; }
            uint32_t veh = p.vehicle;
            if (!vehicle_known(veh)) {
                d.content_note = p.name + " chose a car not installed here. Install matching car mods before starting.";
                error_ = d.last_error = d.content_note; d.abort_requested = true; session_abort_event(); return;
            }
            ee_w32(rec(seat) + 0x68, veh); ee_w32(rec(seat) + 0x6C, veh);
            d.roster[seat].owner = p.id;
        }
    }
    if (d.role == Impl::Role::Guest) {
        d.my_phase = P_CAR_DONE;
        if (!wait_until([&] { return d.have_roster; }, 120000, "Waiting for the host to set the grid")) { error_ = d.last_error = "The host did not send the grid."; d.abort_requested = true; session_abort_event(); return; }
        if (d.roster.size() != count) { error_ = d.last_error = "The grid size does not match (different mods?)"; d.abort_requested = true; session_abort_event(); std::fprintf(stderr, "[netplay] roster size %zu != %u\n", d.roster.size(), count); return; }
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t drv = d.roster[i].driver, veh = d.roster[i].vehicle;
            if (!vehicle_known(veh)) {
                d.content_note = "A selected car is not installed here. Install matching car mods before starting.";
                error_ = d.last_error = d.content_note; d.abort_requested = true; session_abort_event(); return;
            }
            ee_w32(rec(i) + 0x68, drv); ee_w32(rec(i) + 0x6C, veh);
        }
    }
}
void Session::roster_post(uint32_t description) {
    Impl &d = *d_;
    const uint32_t count = ee_r32(description + 0x98C);
    if (count == 0 || count > 16) return;
    auto rec = [&](uint32_t i) { return description + 0x2C + i * 0x78; };
    if (d.abort_requested) return;
    if (d.roster.size() != count) {
        error_ = d.last_error = "The event changed the starting grid size. Choose another event.";
        d.abort_requested = true; session_abort_event(); return;
    }
    const uint8_t local_id = d.role == Impl::Role::Host ? 0 : d.my_id;
    d.my_slot = -1;
    for (uint32_t i = 0; i < count; ++i) {
        if (d.role == Impl::Role::Host) {
            d.roster[i].driver = ee_r32(rec(i) + 0x68);
            d.roster[i].vehicle = ee_r32(rec(i) + 0x6C);
        }
        const bool mine = d.roster[i].owner == local_id;
        if (mine && d.my_slot >= 0) {
            error_ = d.last_error = "The starting grid assigned your controller to more than one car.";
            d.abort_requested = true; session_abort_event(); return;
        }
        // 1116E0 assigns pad bits before we replace Human/AI types. The scene
        // constructor copies this byte to car+0x250; 177288 uses its low two
        // bits to populate pad -> participant. Each machine has one local pad.
        ee_w32(rec(i), mine ? 0x50 : 0x51);
        ee_w8(rec(i) + 0x70, ee_r8(rec(i) + 0x70) & ~3u);
        // 112408 swaps this native grid index by matching human driver IDs.
        // Identical cars make that machine-dependent: own the permutation here.
        ee_w8(rec(i) + 0x72, (uint8_t)i);
        if (mine) d.my_slot = (int)i;
    }
    if (d.my_slot < 0) {
        error_ = d.last_error = "The starting grid did not assign you a car.";
        d.abort_requested = true; session_abort_event(); return;
    }
    if (d.role == Impl::Role::Host) d.have_roster = true;
    std::fprintf(stderr, "[netplay] roster:");
    for (uint32_t i = 0; i < count; ++i) std::fprintf(stderr, " %u:%u/%x%s", i, ee_r32(rec(i) + 0x68), ee_r32(rec(i)), d.roster.size() > i && d.roster[i].owner != 0xFF ? "*" : "");
    std::fprintf(stderr, "\n");
}

void Session::event_ended() {
    Impl &d = *d_;
    if (!in_event_) return;
    in_event_ = false;
    d.abort_requested = false;
    for (auto &kv : d.puppet_cars) netplay_car::set_puppet(kv.second, false);
    d.puppet_cars.clear(); d.remote.clear(); d.shown.clear(); d.bound = false; d.my_phase = P_DONE;
    if (d.role == Impl::Role::Host) { Writer w; w.u32(d.event_id); for (auto &kv : d.peers) d.send_to(kv.second.addr, K_END, 0, w); }
    d.my_phase = P_LOBBY; d.strip.clear();
    d.status = "Back in the lobby";
    d.guest_ready = false;
    std::fprintf(stderr, "[netplay] event ended\n");
    overlay_show_online();
}

void Session::step_begin() {
    Impl &d = *d_;
    if (!in_event_ || d.role == Impl::Role::None || d.abort_requested) return;
    auto fail_grid = [&](const char *reason) {
        error_ = d.last_error = reason;
        d.abort_requested = true;
        if (d.role == Impl::Role::Host) {
            Writer end; end.u32(d.event_id);
            for (const auto &kv : d.peers) if (kv.second.alive) d.send_to(kv.second.addr, K_END, 0, end);
        }
        session_abort_event();
    };
    if (!d.bound) {
        if (const char *path = std::getenv("EOD_NETPLAY_BIND_RAM")) {
            if (FILE *f = std::fopen(path, "wb")) { std::fwrite(g_ram, 1, EE_RAM_SIZE, f); std::fclose(f); }
        }
        // Ownership comes from the shared roster, never from whichever native
        // Human happens to be visited last. Validate the constructed world.
        const uint8_t local_id = d.role == Impl::Role::Host ? 0 : d.my_id;
        int owned_slot = -1; uint32_t humans = 0;
        if (d.roster.empty() || d.roster.size() != netplay_car::slot_count()) {
            fail_grid("The event did not create every starting seat."); return;
        }
        std::vector<uint32_t> cars, bodies;
        for (uint32_t i = 0; i < d.roster.size(); ++i) {
            const uint32_t car = netplay_car::car_of_slot(i);
            const uint32_t body = netplay_car::body_of(car);
            if (!car || !body || std::find(cars.begin(), cars.end(), car) != cars.end() ||
                std::find(bodies.begin(), bodies.end(), body) != bodies.end()) {
                fail_grid("The event did not create a separate physical car for every player."); return;
            }
            cars.push_back(car); bodies.push_back(body);
            humans += netplay_car::is_human(car);
            if (d.roster[i].owner == local_id) {
                if (owned_slot >= 0 || !netplay_car::human_controller_of(car) || (ee_r8(car + 0x250) & 3)) {
                    fail_grid("Your selected car did not receive the local controller."); return;
                }
                owned_slot = (int)i;
            }
            std::fprintf(stderr, "[netplay] bind slot=%u owner=%u car=%08x type=%x pad=%u grid=%u body=%08x driver=%08x human=%08x\n",
                         i, d.roster[i].owner, car, ee_r32(car + netplay_car::kType),
                         ee_r8(car + 0x250) & 3, ee_r8(car + 0x252), body, ee_r32(car + 0x1C1C), netplay_car::human_controller_of(car));
        }
        if (owned_slot < 0 || humans != 1 || ee_r32(0x4473C8) != (uint32_t)owned_slot) {
            fail_grid("The event did not bind exactly one local human driver."); return;
        }
        d.my_slot = owned_slot;
        d.remote.clear(); d.shown.clear();
        for (auto &kv : d.peers) { kv.second.has_latest = false; kv.second.latest_tick = 0; }
        for (uint32_t i = 0; i < d.roster.size(); ++i) {
            if ((int)i == d.my_slot) continue;
            const bool remote_human = d.roster[i].owner != 0xFF && d.roster[i].owner != local_id;
            if (d.role == Impl::Role::Guest || remote_human) {
                netplay_car::set_puppet(cars[i], true);
                d.puppet_cars[d.role == Impl::Role::Host ? d.roster[i].owner : (int)(100 + i)] = cars[i];
            }
        }
        d.my_phase = P_LOADED;
        if (d.role == Impl::Role::Host) {
            d.initial_grid.clear(); d.grid_acks.clear();
            for (uint32_t i = 0; i < cars.size(); ++i) {
                netplay_car::State state;
                if (!netplay_car::read(cars[i], state)) { fail_grid("The event created an invalid starting position."); return; }
                for (const auto &other : d.initial_grid) {
                    float dist2 = 0;
                    for (int k = 9; k < 12; ++k) { const float delta = state.pose[k] - other.pose[k]; dist2 += delta * delta; }
                    if (dist2 < 1.f) { fail_grid("The event placed two cars in the same starting seat."); return; }
                }
                std::fprintf(stderr, "[netplay] grid slot=%u pos=%.3f %.3f %.3f vel=%.3f %.3f %.3f\n", i,
                             state.pose[9], state.pose[10], state.pose[11], state.vel[0], state.vel[1], state.vel[2]);
                d.initial_grid.push_back(state);
            }
            d.grid_event = d.applied_grid_event = d.event_id;
            if (!wait_until([&] {
                    for (const auto &kv : d.peers) if (kv.second.alive &&
                        (kv.second.seen_event != d.event_id || kv.second.phase < P_LOADED || d.grid_acks[kv.first] != d.event_id)) return false;
                    return true;
                }, 45000, "Waiting for everyone to confirm the starting grid") || d.abort_requested) {
                fail_grid("Not everyone confirmed a valid starting grid."); return;
            }
            d.go = true;
        } else {
            if (!wait_until([&] { return d.grid_event == d.event_id; }, 45000, "Waiting for the host's starting grid") || d.abort_requested) {
                fail_grid("The host did not send a starting grid."); return;
            }
            if (d.initial_grid.size() != cars.size()) { fail_grid("The host's starting grid has a different number of cars."); return; }
            // Apply once, including our own human, before acknowledging. Later
            // snapshots never write the local owner's slot.
            for (uint32_t i = 0; i < cars.size(); ++i) netplay_car::write(cars[i], d.initial_grid[i]);
            d.applied_grid_event = d.event_id;
            Writer ack; ack.u32(d.event_id); d.send_to(d.host_addr, K_GRID_ACK, d.my_id, ack);
            if (!wait_until([&] { return d.go; }, 45000, "Waiting for everyone to confirm the starting grid") || d.abort_requested) {
                fail_grid("The host did not start the confirmed grid."); return;
            }
        }
        d.my_phase = P_RACING; d.bound = true; d.tick = 0;
        std::fprintf(stderr, "[netplay] GO (slot %d, %zu puppets)\n", d.my_slot, d.puppet_cars.size());
    }
    poll();
    // puppets: place each remote car before the step so contacts see it where its owner has it
    for (auto &kv : d.remote) {
        if (!kv.second.ready()) continue;
        uint32_t car = netplay_car::car_of_slot((uint32_t)kv.first);
        if (!car || !netplay_car::is_puppet(car)) continue;
        d.shown[kv.first] = kv.second.advance();
        netplay_car::write(car, d.shown[kv.first]);
    }
}
bool Session::step_allowed() const {
    return !in_event_ || (d_->bound && d_->go && !d_->abort_requested);
}
void Session::step_end() {
    Impl &d = *d_;
    if (!in_event_ || !d.bound || !d.go || d.abort_requested || d.role == Impl::Role::None) return;
    for (auto &kv : d.shown) {
        uint32_t car = netplay_car::car_of_slot((uint32_t)kv.first);
        if (car && netplay_car::is_puppet(car)) netplay_car::write(car, kv.second);
    }
    ++d.tick;
    static const bool dbg = std::getenv("EOD_NETPLAY_DEBUG") != nullptr;
    if (dbg && d.tick % 60 == 0) {
        netplay_car::State me;
        if (d.my_slot >= 0 && netplay_car::read(netplay_car::car_of_slot((uint32_t)d.my_slot), me))
            std::fprintf(stderr, "[netplay] t=%u own slot %d pos %.2f %.2f %.2f speed %.1f steer %.3f gas %.3f brake %.3f native_frame %llu\n", d.tick, d.my_slot, me.pose[9], me.pose[10], me.pose[11], std::sqrt(me.vel[0] * me.vel[0] + me.vel[2] * me.vel[2]), me.steer, me.gas, me.brake, (unsigned long long)ee_r32(0x3CDA8C));
        for (uint32_t i = 0; i < d.roster.size(); ++i)
            if (d.roster[i].owner != 0xFF && (int)i != d.my_slot && d.shown.count((int)i))
                std::fprintf(stderr, "[netplay] t=%u remote slot %u shown %.2f %.2f %.2f (newest tick %.0f)\n", d.tick, i, d.shown[(int)i].pose[9], d.shown[(int)i].pose[10], d.shown[(int)i].pose[11], d.remote[(int)i].newest_tick());
    }
    if (d.tick & 1) return;   // 30 Hz
    const uint64_t t = now_us();
    if (d.role == Impl::Role::Guest) {
        netplay_car::State s;
        if (d.my_slot >= 0 && netplay_car::read(netplay_car::car_of_slot((uint32_t)d.my_slot), s)) {
            Writer w; w.u32(d.event_id); w.u8(1); put_car(w, (uint8_t)d.my_slot, d.tick, s);
            d.send_to(d.host_addr, K_SNAP, d.my_id, w);
        }
    } else {
        for (auto &kv : d.peers) {
            Peer &p = kv.second; if (!p.alive) continue;
            Writer w; uint32_t n = 0; Writer body;
            for (uint32_t i = 0; i < d.roster.size(); ++i) {
                if (d.roster[i].owner == p.id) continue;
                netplay_car::State s;
                const bool remote_human = d.roster[i].owner != 0xFF && d.roster[i].owner != 0;
                if (remote_human) {
                    auto it = d.peers.find(d.roster[i].owner);
                    if (it == d.peers.end() || !it->second.alive || !it->second.has_latest || t - it->second.latest_at > 500000) continue;
                    put_car(body, (uint8_t)i, (uint32_t)it->second.latest_tick, it->second.latest);
                } else {
                    if (!netplay_car::read(netplay_car::car_of_slot(i), s)) continue;
                    put_car(body, (uint8_t)i, d.tick, s);
                }
                ++n;
            }
            if (!n) continue;
            w.u32(d.event_id); w.u8(n); w.b.insert(w.b.end(), body.b.begin(), body.b.end());
            d.send_to(p.addr, K_SNAP, 0, w);
        }
    }
}

Session &session() { static Session s; return s; }
namespace {
const bool g_registered = [] {
    namespace H = netplay_hooks;
    H::race_rules = [] { return session().online_rules(); };
    H::menu_enter = [] { session().menu_enter(); };
    H::menu_leave = [] { session().menu_leave(); };
    H::take_start = [](H::StartInfo &i) { return session().take_start(i); };
    H::car_chosen = [](uint32_t v) { session().car_chosen(v, ""); };
    H::track_chosen = [](const std::string &t) { session().track_chosen(t); };
    H::wait_track = [](std::string &t) { return session().wait_track(t); };
    H::event_ended = [] { session().event_ended(); };
    H::event_active = [] { return session().event_active(); };
    H::active = [] { return session().active(); };
    H::is_host = [] { return session().is_host(); };
    H::remote_humans = [] { return session().remote_humans(); };
    H::roster_pre = [](uint32_t d) { session().roster_pre(d); };
    H::roster_post = [](uint32_t d) { session().roster_post(d); };
    return true;
}();
}
}  // namespace netplay
