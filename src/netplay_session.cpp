// Online play session: see netplay_session.h and docs/NETPLAY_STATE_SYNC.md.
#ifdef _WIN32
// Include the SDK before the EE register macros; Win64 unwind metadata has a field named R.
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include "netplay_session.h"
#include "netplay_peer_activity.h"
#include "netplay_lobby_maps.h"
#include "battle_layout.h"
#include "netplay_battle.h"
#include "netplay_chicken.h"
#include "battle_driver.h"
#include "mutator_engine.h"
#include "mutator_hooks.h"
#include "mutator_presets.h"
#include "../runtime/core/cpu.h"
#include <bitset>
#include "netplay_content.h"
#include "../runtime/app/online_profile.h"
#include "netplay_impact_fixture.h"
#include "netplay_damage_detail.h"
#include "../runtime/core/config.h"
#include "netplay_online.h"
#include "netplay_relay.h"
#include "netplay_online_wire.h"
#include "netplay_voice.h"
#include "catalog_cache.h"
#include "session_start.h"
#include "../runtime/platform/platform.h"
#include "../runtime/ui/overlay.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <random>
#include <sstream>

extern "C" void f_001671E8_orig(void); // BattleMaster::requestRespawn
extern "C" void f_001675C0(void);      // BattleMaster::respawn
extern "C" void f_00166F48(void); // BattleMaster::participantKilled(victim, attacker)

#ifdef _WIN32
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
constexpr uint64_t kResultsDisplayUs = 7000000;
constexpr uint64_t kResultsLeadUs = 400000;
constexpr uint32_t kNativeRaceState = 0x003D1A70;
constexpr uint32_t kNativeRaceQuit = 0x00437BAC;
constexpr uint32_t kNativeRaceClock = 0x003CDACC;
constexpr uint32_t kNativeEventType = 0x003CF19C;
constexpr uint32_t kDetention = 0x09002223;
constexpr uint32_t kNativeEventDeadline = 0x00485394;
constexpr uint64_t kDirectJoinTimeoutUs = 8000000;
constexpr uint64_t kOnlineHandshakeTimeoutUs = 10000000;
constexpr uint64_t kOnlineDiscoveryTimeoutUs = 30000000;
// Leave the next network poll and frontend frame room inside the 10s UI bound.
constexpr uint64_t kServiceAdmissionUs = 9750000;
uint64_t now_us() { return platform_time_us(); }
uint32_t now_ms() { return (uint32_t)(now_us() / 1000); }

// Optional details follow the existing one-byte mismatch refusal reason.
void put_required_content(Writer &w, const ContentFingerprint &content) {
    w.u8(1);
    w.str(content.value, 16);
    w.u8(static_cast<uint8_t>(content.profile.source));
    w.u8(content.profile.packages.size());
    for (const auto &package : content.profile.packages) w.str(package, 96);
    w.u8(content.profile.more_packages);
}
bool get_required_content(Reader &r, ContentFingerprint &out) {
    if (r.u8() != 1) return false;
    out.value = r.str();
    const auto source = r.u8(), count = r.u8();
    if (!checksum_text(out.value, 16) || source < 1 || source > 3 || count > 3 ||
        (count && source != static_cast<uint8_t>(ContentSource::Custom))) return false;
    out.profile.source = static_cast<ContentSource>(source);
    for (unsigned i = 0; i < count; ++i) {
        const auto package = r.str();
        if (!package_label(package)) return false;
        out.profile.packages.push_back(package);
    }
    const auto more = r.u8();
    out.profile.more_packages = more != 0;
    return r.ok && r.p == r.e && (more == 0 || (more == 1 && count == 3));
}

const char *kDefaultEvents[] = {"No-Rules Race", "Battle (4 AI)", "Battle (8 AI)"};   // EOD_NETPLAY_MODE=0|1|2 picks the lobby default

std::string default_name() {
    static const char *first[] = {"Dust","Iron","Rally","Nitro","Gravel","Rust","Chrome","Apex"};
    static const char *last[] = {"Fox","Hawk","Wolf","Bolt","Ace","Owl","Cat","Lynx"};
    std::random_device rand;
    return std::string(first[rand() % 8]) + last[rand() % 8] + std::to_string(10 + rand() % 90);
}
std::string sockaddr_text(const sockaddr_in &a) {
    if (GarageRelay::relayed(a)) return "relay peer " + std::to_string(ntohs(a.sin_port));
    char buf[32]; inet_ntop(AF_INET, &a.sin_addr, buf, sizeof buf);
    return std::string(buf) + ":" + std::to_string(ntohs(a.sin_port));
}
std::string clean_name(std::string n) {
    n.erase(std::remove_if(n.begin(), n.end(), [](unsigned char c) { return c < 32 || c > 126; }), n.end());
    const auto first = n.find_first_not_of(' ');
    if (first == n.npos) return "";
    return n.substr(first, n.find_last_not_of(' ') - first + 1).substr(0, 16);
}
bool same_addr(const sockaddr_in &a, const sockaddr_in &b) { return a.sin_family == b.sin_family && a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port; }
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
std::string vehicle_label(const std::string &model, size_t paint_index, size_t paint_count) {
    return paint_count > 1 ? model + " / Paint " + std::to_string(paint_index + 1) : model;
}

EventFinishState native_outcome(uint32_t slot, EventCompletionMode mode) {
    const uint32_t car = netplay_car::car_of_slot(slot);
    if (!car || car > EE_RAM_SIZE - 0x1BEC) return EventFinishState::Active;
    const auto wreck = ee_r32(car + 0x1BE8);
    if (wreck >= 10 && wreck <= 12) return EventFinishState::Totalled;
    if (mode != EventCompletionMode::Race) return EventFinishState::Active;
    // The native lap completion path sets driver+0x34. Use the lap target as
    // well so a different mid-race native exit cannot qualify as a line finish.
    const auto driver = ee_r32(0x00485230 + 4 * slot);
    const auto laps = ee_r32(0x0048501C);
    if (driver && driver <= EE_RAM_SIZE - 0x44 && !(driver & 3) && laps &&
        ee_r32(driver + 4) == car && ee_r32(driver + 0x34) && ee_r32(driver + 0x40) >= laps)
        return EventFinishState::Finished;
    return EventFinishState::Active;
}

float native_float(uint32_t address) {
    const auto bits = ee_r32(address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void trace_result_rows(const std::vector<EventResultRow> &rows) {
    for (const auto &row : rows) {
        std::fprintf(stderr, "[netplay] result slot=%u status=%u rank=%u\n", row.slot,
                     static_cast<unsigned>(row.state), row.rank);
        if (row.state == EventFinishState::BattleScore)
            std::fprintf(stderr, "[netplay] battle result slot=%u rank=%u score=%d\n", row.slot, row.rank, row.score);
    }
}

struct BattleStandings {
    std::array<int32_t, battle_layout::kCapacity> points{};
    std::array<uint8_t, battle_layout::kCapacity> place{};
};

bool read_battle_standings(size_t count, uint16_t eligible, BattleStandings &standings) {
    const uint32_t master = ee_r32(0x003CF194);
    if (!master || (master & 3u) || master > EE_RAM_SIZE - battle_layout::kMasterSize ||
        ee_r32(master) != battle_layout::kMasterVtable || ee_r32(master + 4) != 2 ||
        !count || count > battle_layout::kCapacity || ee_r32(master + battle_layout::kCount) != count) return false;
    // 0x167D58 enters phase 2 at five net kills, then 0x167658 orders all
    // slots by net points, kills, and deaths. Keep its native tie ordering.
    uint16_t seen = 0;
    uint8_t rank = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint32_t slot = ee_r32(master + battle_layout::kRanking + 4 * i);
        if (slot >= count || (seen & (1u << slot))) return false;
        seen |= static_cast<uint16_t>(1u << slot);
        if (!(eligible & (1u << slot))) continue;
        const uint32_t record = master + battle_layout::kParticipants + slot * battle_layout::kParticipantStride;
        standings.points[slot] = static_cast<int32_t>(ee_r32(record + 0x0C) - ee_r32(record + 0x14));
        standings.place[slot] = ++rank;
        if (rank == 1 && standings.points[slot] < 5) return false;
    }
    return rank > 0;
}

// Headless integration fixture. Supply native terminal records at fixed world
// steps so tests exercise the normal observation, transport and native unwind.
void test_terminal_records(uint32_t event_id, uint32_t tick, size_t local_slot,
                           const std::vector<RosterSlot> &roster, bool host) {
    static const char *schedule = netplay_impact_fixture::test_session() ? std::getenv("EOD_NETPLAY_TEST_TERMINAL") : nullptr;
    static uint32_t last_event = 0, last_scheduled_tick = 0;
    if (!schedule) return;
    if (last_event != event_id) {
        last_event = event_id;
        last_scheduled_tick = 0;
    }
    std::istringstream entries(schedule);
    std::string entry;
    while (std::getline(entries, entry, ',')) {
        const auto colon = entry.find(':');
        if (colon == std::string::npos) continue;
        const auto scheduled_tick = std::strtoul(entry.c_str(), nullptr, 10);
        if (scheduled_tick <= last_scheduled_tick || scheduled_tick > tick) continue;
        last_scheduled_tick = static_cast<uint32_t>(scheduled_tick);
        const auto action = entry.substr(colon + 1);
        if (action == "battle-frag" && ee_r32(kNativeEventType) == 0x18000224) {
            const uint32_t master = ee_r32(0x003CF194);
            const uint8_t owner = (!host || std::getenv("EOD_NETPLAY_TEST_BATTLE_RESPAWN")) ? 0 : event_id % 2 ? 1 : 0xFF;
            const auto scorer = std::find_if(roster.begin(), roster.end(),
                                            [owner](const auto &r) { return r.owner == owner; });
            if (!master || master > EE_RAM_SIZE - battle_layout::kMasterSize ||
                ee_r32(master) != battle_layout::kMasterVtable || ee_r32(master + 4) != 1 ||
                ee_r32(master + battle_layout::kCount) != roster.size() ||
                roster.size() < 2 || scorer == roster.end()) continue;
            const uint32_t slot = static_cast<uint32_t>(scorer - roster.begin());
            const uint32_t victim = (slot + 1) % roster.size();
            const uint32_t record = master + battle_layout::kParticipants + slot * battle_layout::kParticipantStride;
            // Seed a close game, then invoke the ordinary native death award.
            // Its update on the next frame must sort the actual standings.
            ee_w32(record + 0x0C, 5);
            ee_w32(record + 0x10, 3);
            ee_w32(record + 0x14, 1);
            const EEState saved = g_ee;
            g_ee.r[4].ud[0] = master;
            g_ee.r[5].ud[0] = victim;
            g_ee.r[6].ud[0] = slot;
            f_00166F48();
            g_ee = saved;
            std::fprintf(stderr, "[netplay-test] battle award event=%u slot=%u owner=%u kills=%u penalties=%u deaths=%u\n",
                         event_id, slot, owner, ee_r32(record + 0x0C), ee_r32(record + 0x14), ee_r32(record + 0x10));
            continue;
        }
        for (size_t i = 0; i < roster.size(); ++i) {
            if (action == "objective" && i == local_slot) {
                ee_w32(kNativeRaceState, 3);
                std::fprintf(stderr, "[netplay-test] terminal event=%u tick=%lu actual=%u slot=%zu action=objective\n",
                             event_id, scheduled_tick, tick, i);
                continue;
            }
            const bool ai = host && action == "ai" && roster[i].owner == 0xFF;
            if (!ai && (i != local_slot || (action != "finish" && action != "wreck"))) continue;
            const uint32_t car = netplay_car::car_of_slot(static_cast<uint32_t>(i));
            if (!car || car > EE_RAM_SIZE - 0x1BF0) continue;
            if (action == "finish") {
                const auto driver = ee_r32(0x00485230 + 4 * i);
                const auto laps = ee_r32(0x0048501C);
                if (!driver || driver > EE_RAM_SIZE - 0x44 || !laps || ee_r32(driver + 4) != car)
                    continue;
                ee_w32(driver + 0x34, 1);
                ee_w32(driver + 0x40, laps + 1);
            } else {
                ee_w32(car + 0x1BE8, 12);
                ee_w32(car + netplay_car::kDamage, 0x3F800000);
            }
            // Exercise the early native exit guard as part of the fixture.
            ee_w32(kNativeRaceState, 3);
            std::fprintf(stderr, "[netplay-test] terminal event=%u tick=%lu actual=%u slot=%zu action=%s\n",
                         event_id, scheduled_tick, tick, i, action.c_str());
        }
    }
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

struct DamageFeedback {
    uint32_t tick = 0;
    float flash = 0.f;
    float shake = 0.f;
};
struct Bodywork {
    std::vector<uint8_t> detail, encoded;
    uint32_t revision = 0, applied = 0, spawn = 0, capture_until = 0;
    uint64_t next_retry = 0;
};

struct Peer {
    sockaddr_in addr{};
    uint8_t id = 0;
    std::string name, secret;
    std::string requested_name, join_attempt;
    bool ready = false, at_menu = true, car_done = false, got_track = false, got_roster = false, got_go = false, got_start = false;
    bool lobby_heartbeat_received = false;
    uint32_t heartbeat_serial = 0;
    uint64_t next_map_send = 0;
    std::string verified_map;
    uint32_t vehicle = 0;
    std::string vname;
    uint8_t phase = P_LOBBY;
    uint32_t seen_event = 0, waiting_event = 0;
    PeerActivity activity;
    uint64_t last_lobby = 0;
    uint32_t ping_ms = 0;
    uint32_t last_ts_sent = 0;
    bool alive = true;
    uint64_t identity = 0;
    // The host's native solver temporarily owns a guest body at a damaging
    // car contact. Keep its final checkpoint until that owner echoes its tick.
    uint32_t battle_respawn_requested = 0;
    uint32_t impact_through = 0, impact_revision = 0;
    DamageFeedback impact_feedback;
    netplay_car::State impact_state;
    std::array<uint32_t, kMaxPlayers> bodywork_acks{};
    ChickenChannel chickens;
    uint64_t latest_tick = 0; uint32_t latest_host_tick = 0; netplay_car::State latest; bool has_latest = false; uint64_t latest_at = 0;
};
struct LobbyPlayer { uint8_t id; std::string name; bool ready; std::string vname; uint16_t ping; bool waiting = false; };
struct LanEntry { sockaddr_in addr; std::string name; uint8_t players; uint64_t seen; };
struct Friend { std::string secret, name; };                 // remembered crew: a shared random secret per friend
struct CrewHost { sockaddr_in addr{}; std::string name; uint8_t players = 0; uint64_t seen = 0; };

struct Session::Impl {
    int fd = -1, disc_fd = -1;
    enum class Role { None, Host, Guest } role = Role::None;
    uint16_t port = 0;
    std::string status, name = default_name();
    maps::LobbyMaps maps;
    std::filesystem::path map_directory() const {
        return std::filesystem::u8path(g_config_path).parent_path() / "mods" / "maps";
    }
    // host side
    std::map<uint8_t, Peer> peers;
    uint8_t rules = 0;
    names::Camera name_camera{};
    names::Occluders name_occluders;
    bool name_geometry_ready = false;
    struct TagSight { uint32_t next_tick = 0; bool known = false, clear = false; };
    std::array<TagSight, kMaxPlayers> tag_sight{};
    uint8_t tuning[netplay_hooks::TuneCount] = {7, 4, 4, 12};   // lobby cheat tuning in quarter units (netplay_hooks::Tune)
    unsigned host_capacity = kMaxPlayers;
    uint64_t next_peer_identity = 0;
    int ai_count = 0;       // 0 = OFF (humans only), 1..15 = explicit AI bots, 16 = FILL TO MAX
    int lobby_ai_count = 0; // replicated host ai_count for guests
    // lobby picks: the host owns TRACK and MODE (event), every player owns CAR and PAINT
    std::string track_name, event_pick, lobby_track;
    std::string my_model; int my_paint = 0;
    bool local_ready = true;
    uint64_t last_beacon = 0, last_lobby_send = 0;
    // guest side
    sockaddr_in host_addr{};
    uint8_t my_id = 0;
    bool welcomed = false, lobby_received = false;
    uint64_t last_join = 0, last_hb = 0;
    PeerActivity host_activity;
    uint32_t heartbeat_serial = 0;
    uint32_t last_host_ts = 0;
    std::vector<LobbyPlayer> lobby_players;
    unsigned lobby_capacity = kMaxPlayers;
    std::string lobby_event; uint8_t host_phase = 0;
    bool guest_ready = false;
    // common event state
    uint32_t event_id = 0, waiting_event = 0;
    uint16_t event_guest_mask = 0;
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
    bool event_has_mutator = false;
    uint32_t mutator_applied_event = 0;
    std::map<uint8_t, uint32_t> mutator_acks;
    std::map<uint8_t, uint32_t> mutator_round_acks;
    uint64_t last_mutator_round_send = 0;
    uint64_t last_mutator_trigger_send = 0;
    uint8_t gametype_preset = mutator::PRESET_STANDARD;
    uint8_t lobby_gametype_preset = mutator::PRESET_STANDARD;
    game_frontend::MutatorDraft host_mutator_draft{};
    bool has_host_mutator_draft = false;
    uint32_t host_mutator_serial = 0;
    PendingOfflineMatch last_offline_match{};
    uint8_t my_phase = P_LOBBY;
    uint64_t last_menu_tick = 0;
    bool in_menu = false; uint32_t inject_count = 0;
    std::string strip;       // "Waiting for ..." shown over the game while players choose
    BattleFrame battle, received_battle;
    ChickenChannel host_chickens; // guest's channel to its host
    std::array<uint32_t, kMaxPlayers> chicken_serial{}, chicken_flying{}, chicken_last_blast{};
    uint32_t prop_wait_tick = 0;
    std::array<Bodywork, kMaxPlayers> bodywork{};
    uint64_t next_bodywork_sample = 0;
    uint64_t last_battle_state = 0, last_respawn_request = 0;
    uint32_t respawn_request = 0;
    uint32_t tick = 0;       // world steps since GO
    int my_slot = -1; bool bound = false;
    std::map<int, netplay_car::Remote> remote;
    std::map<int, netplay_car::State> shown;
    uint32_t impact_received = 0, impact_applied = 0;
    DamageFeedback impact_feedback;
    uint32_t predicted_impact_at = 0, last_impact_resolution = 0;
    uint64_t prediction_deadline = 0;
    netplay_car::State before_contact;
    netplay_car::State impact_state;
    std::map<int, uint32_t> puppet_cars;
    std::map<std::string, LanEntry> lan;
    bool discovering = false;
    bool abort_requested = false;
    EventCompletionMode completion_mode = EventCompletionMode::Native;
    EventCompletion completion;
    EventFinishState local_finish = EventFinishState::Active;
    uint64_t finish_countdown = 0, result_until = 0, last_outcome_send = 0;
    uint64_t event_started_at = 0, last_clock_probe = 0;
    uint32_t local_terminal_ms = 0, result_show_host_ms = 0;
    uint32_t saved_auto_driver = 0;
    bool native_lap_driver = false;
    bool logged_racing_contact = false;
    EventHostClock host_clock;
    uint32_t result_event = 0;
    float mode_deadline = 0;
    std::vector<EventResultRow> result_rows;
    std::string last_error;
    bool retryable_loss = false;
    std::string content_note;
    ContentFingerprint checked_content;
    std::optional<game_frontend::Command> guest_invite;
    struct ProfileCheck {
        std::mutex mutex;
        bool done = false;
        std::optional<std::string> match;   // empty path means the original disc
    };
    std::shared_ptr<ProfileCheck> profile_check;
    void content_mismatch(const ContentFingerprint &required) {
        std::string message = "The host uses " + std::string(profile_name(required.profile.source)) +
                              ". Your game files differ. ";
        if (!required.profile.packages.empty()) {
            message += "Host packages: ";
            for (size_t i = 0; i < required.profile.packages.size(); ++i)
                message += (i ? ", " : "") + required.profile.packages[i];
            message += required.profile.more_packages ? " and more. " : ". ";
        }
        if (required.profile.source == ContentSource::Custom)
            message += "Use the same gameplay packages and versions in World Studio's Mod Manager, then relaunch.";
        else if (required.profile.source == ContentSource::Integrated)
            message += "Use the same integrated pack and game release as the host.";
        else
            message += checked_content.profile.source == ContentSource::Disc
                ? "Use the same supported disc release. Reimport your disc if its files were changed."
                : "Use your supported disc installation without extra gameplay content.";
        std::fprintf(stderr, "[netplay] %s\n", message.c_str());
        fail_guest_join(std::move(message));
        // Only search local startup locations. Untrusted network packets never supply paths or downloads.
        auto job = std::make_shared<ProfileCheck>();
        profile_check = job;
        const char *active = std::getenv("EOD_MOD_DIR");
        std::thread([job, wanted = required.value, type = required.profile.source,
                     disc = g_game_root, current = std::string(active ? active : "")] {
            std::optional<std::string> match;
            try {
                auto candidates = online_profile::installed_overlays();
                if (type == ContentSource::Disc) candidates.insert(candidates.begin(), "");
                else candidates.emplace_back();
                for (const auto &candidate : candidates) {
                    if (job.use_count() == 1) break;
                    if (candidate == current) continue;
                    auto result = content_fingerprint(disc, candidate);
                    if (result.error.empty() && result.value == wanted) { match = candidate; break; }
                }
            } catch (const std::filesystem::filesystem_error &) { }
            if (job.use_count() > 1 && std::getenv("EOD_NETPLAY_LIFECYCLE"))
                std::fprintf(stderr, "[netplay] installed profile match %s\n", match ? "available" : "unavailable");
            std::lock_guard<std::mutex> lock(job->mutex);
            job->match = std::move(match);
            job->done = true;
        }).detach();
    }
    // Game-content check (hashes the whole DATA tree: seconds) runs off the game thread, which is the app's main
    // thread. It starts when the ONLINE tab comes on screen; HOST/JOIN pressed before it finishes waits for it.
    struct ContentJob { std::mutex m; bool done = false; netplay::ContentFingerprint result; uint64_t finished_us = 0; };
    std::shared_ptr<ContentJob> content_job;
    std::unique_ptr<game_frontend::Command> pending_join;   // HOST/JOIN waiting for the content check
    void start_content_check() {
        auto job = std::make_shared<ContentJob>();
        content_job = job;
        const char *overlay = std::getenv("EOD_MOD_DIR");
        std::thread([job, root = g_game_root, ov = std::string(overlay ? overlay : "")] {
            auto r = netplay::content_fingerprint(root, ov);
            std::lock_guard<std::mutex> l(job->m);
            job->result = r; job->done = true; job->finished_us = now_us();
        }).detach();
    }
    bool content_ready(netplay::ContentFingerprint &out) {
        if (!content_job) return false;
        std::lock_guard<std::mutex> l(content_job->m);
        if (!content_job->done) return false;
        out = content_job->result;
        return true;
    }
    // Re-check on each visit to the tab (content can change between sessions), at most every 30 s.
    void refresh_content_check() {
        bool stale = !content_job;
        if (!stale) {
            std::lock_guard<std::mutex> l(content_job->m);
            stale = content_job->done && now_us() - content_job->finished_us > 30000000;
        }
        if (stale) start_content_check();
    }
    // ---- internet (Garage Code), see docs/NETPLAY_INTERNET.md
    netplay_online::Online online;
    GarageRelay relay;
    std::string join_attempt;
    uint64_t relay_presence_at = 0;
    bool online_on = false;
    std::string code;                          // host: the code to share; guest: the code being joined
    std::string join_secret;                   // guest: secret (code or friend secret) the rendezvous runs on
    uint32_t join_tag = 0;
    std::vector<Friend> friends; bool friends_loaded = false;
    std::vector<sockaddr_in> targets;          // guest: host candidates from the rendezvous (JOIN is sent to all)
    std::vector<sockaddr_in> punch_targets;    // host: joiners found at the trackers (we send them a punch)
    uint64_t join_started = 0, handshake_started = 0, last_punch = 0, last_probe = 0, last_remember = 0;
    std::atomic<uint64_t> ui_seen{0};   // last frame the ONLINE tab was on screen (viewing_online)
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
    netplay::voice::StereoMixer voice_mixer;

    void sync_voice() {
        const bool connected = (role == Role::Host) || (role == Role::Guest && lobby_received);
        const uint8_t local_id = role == Role::Host ? 0 : my_id;
        if (connected && local_id < netplay::voice::MaxPeers) {
            if (!voice.active()) voice.start(0x454F44564F494345ull, 1, local_id);
            voice.set_transmit_enabled(mic_enabled && !deafened);
            voice.set_microphone_muted(!mic_enabled || deafened);
            voice.set_deafened(deafened || muted_all);
            if (deafened || muted_all) {
                std::lock_guard<std::mutex> lock(voice_mtx);
                voice_out.clear();
                voice_pos = 0;
                voice_mixer = {};
            }
        } else {
            if (voice.active()) voice.stop();
            std::fill(std::begin(speaking_until), std::end(speaking_until), 0);
            std::lock_guard<std::mutex> lock(voice_mtx);
            voice_out.clear();
            voice_pos = 0;
            voice_mixer = {};
            last_voice_playout = 0;
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
                bool speaking = false;
                auto res = voice.playout(peer, pf, &speaking);
                if (speaking) speaking_until[peer] = t + 300000;
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
    void close_game() { maps.clear(); relay.stop(fd); relay_presence_at = 0; online_stop(); if (fd >= 0) EOD_CLOSESOCK(fd); fd = -1; }
    void fail_guest_join(std::string message) {
        if (welcomed) { Writer w; send_to(host_addr, K_LEAVE, my_id, w); }
        last_error = std::move(message);
        role = Role::None;
        welcomed = lobby_received = joining_online = false;
        lobby_players.clear();
        route_text.clear();
        close_game();
        sync_voice();
    }
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
        if (GarageRelay::relayed(to)) {
            relay.send(fd, to, w.b);
            return;
        }
        sendto(fd, (const char *)w.b.data(), (int)w.b.size(), 0, (const sockaddr *)&to, sizeof to);
    }
    bool chickens_enabled() const {
        return bound && go && !abort_requested && !result_until &&
               (completion_mode == EventCompletionMode::Battle || (rules & netplay_hooks::Chickens));
    }
    uint32_t chicken_life(size_t slot) const {
        return completion_mode == EventCompletionMode::Battle ? battle.seats[slot].spawn_tick : 0;
    }
    void trace_chicken(const char *verb, const ChickenEvent &e) const {
        if (std::getenv("EOD_NETPLAY_DEBUG") || std::getenv("EOD_NETPLAY_TEST_CHICKEN"))
            std::fprintf(stderr, "[netplay-chicken] %s event=%u slot=%u life=%u shot=%u tick=%u pos=%.3f,%.3f,%.3f vel=%.3f,%.3f,%.3f\n",
                         verb, event_id, e.slot, e.life, e.shot, tick, e.position[0], e.position[1],
                         e.position[2], e.velocity[0], e.velocity[1], e.velocity[2]);
    }
    void send_chickens(ChickenChannel &channel, const sockaddr_in &to) {
        channel.transmit(now_us(), [&](uint32_t serial, const ChickenEvent &e) {
            Writer w; w.u32(event_id); w.u32(serial); put_chicken(w, e);
            send_to(to, K_CHICKEN, my_id, w);
        });
    }
    void queue_chicken(Peer &peer, const ChickenEvent &e) {
        if (!peer.chickens.queue(e)) {
            // Even at the shortest cooldown a healthy link clears this queue
            // in a fraction of its capacity. End only this unresponsive link.
            Writer reason; reason.str("Event updates stopped reaching your game. Rejoin the lobby.", 96);
            send_to(peer.addr, K_KICK, 0, reason);
            peer.alive = false;
            release_driver(peer);
            return;
        }
        send_chickens(peer.chickens, peer.addr);
    }
    void broadcast_chicken(const ChickenEvent &e) {
        for (auto &[id, peer] : peers)
            if (peer.alive && peer.waiting_event != event_id) queue_chicken(peer, e);
    }
    void ack_chickens(const ChickenChannel &channel, const sockaddr_in &to) {
        Writer w; w.u32(event_id); w.u32(channel.received());
        send_to(to, K_CHICKEN_ACK, my_id, w);
    }
    void read_chicken(Reader &r, uint8_t kind, ChickenChannel &channel, const sockaddr_in &from,
                      uint8_t owner) {
        const uint32_t id = r.u32(), serial = r.u32();
        if (!r.ok || id != event_id || !serial || !chickens_enabled()) return;
        if (kind == K_CHICKEN_ACK) {
            if (r.p == r.e) channel.acknowledge(serial);
            return;
        }
        ChickenEvent e;
        if (!get_chicken(r, e) || e.slot >= roster.size() ||
            (role == Role::Host && (e.action != ChickenAction::Launch || roster[e.slot].owner != owner))) return;
        channel.receive(serial, e);
        if (serial <= channel.received()) ack_chickens(channel, from);
    }
    void apply_chickens(ChickenChannel &channel, const sockaddr_in &to, Peer *peer) {
        const uint32_t previous = channel.received();
        channel.apply([&](ChickenEvent e) {
            const uint32_t life = chicken_life(e.slot);
            if (e.life != life) {
                // A host shot can outrun the separate Battle respawn packet.
                // The next seat update releases it. Old-life FX are discarded.
                if (e.action == ChickenAction::Prop && e.life < life) prop_wait_tick = 0;
                return peer || e.life < life;
            }
            auto &serial = chicken_serial[e.slot];
            auto &flying = chicken_flying[e.slot];
            if (peer) {
                if (e.shot <= serial) return true;
                serial = e.shot;
                if ((completion_mode != EventCompletionMode::Battle || !battle.seats[e.slot].dead) &&
                    battle_chicken_replay_launch(e.slot, e.position.data(), e.velocity.data())) {
                    flying = e.shot;
                    trace_chicken("launch", e);
                    broadcast_chicken(e);
                } else {
                    e.action = ChickenAction::Cancel;
                    trace_chicken("reject", e);
                    queue_chicken(*peer, e);
                }
                return true;
            }
            if (e.action == ChickenAction::Launch) {
                if (static_cast<int>(e.slot) == my_slot) {
                    trace_chicken("echo-launch", e); // local shot already used the native muzzle
                    return true;
                }
                if (e.shot <= serial) return true;
                if (!battle_chicken_replay_launch(e.slot, e.position.data(), e.velocity.data())) return false;
                serial = flying = e.shot;
                trace_chicken("replay-launch", e);
            } else if (e.action == ChickenAction::Prop) {
                if (e.shot != chicken_last_blast[e.slot]) { prop_wait_tick = 0; return true; }
                const auto &hit = e.prop;
                const bool debris = hit.type != chicken_prop::Type::Wobble;
                if (debris && (hit.object >= roster.size() || chicken_life(hit.object) > hit.donor_life)) {
                    prop_wait_tick = 0;
                    return true;
                }
                if ((debris && chicken_life(hit.object) < hit.donor_life) || !chicken_prop::apply(e.slot, hit)) {
                    // Bodywork and weapons arrive independently. Allow the donor's
                    // detached part to spawn, then expire a missing/recycled part.
                    if (!prop_wait_tick) prop_wait_tick = tick;
                    if (tick - prop_wait_tick < 30) return false;
                } else trace_chicken("replay-prop", e);
                prop_wait_tick = 0;
            } else if (e.shot == flying) {
                if (e.action == ChickenAction::Blast) {
                    if (!battle_chicken_replay_blast(e.slot, e.position.data())) return false;
                    chicken_last_blast[e.slot] = e.shot;
                    trace_chicken("replay-blast", e);
                } else {
                    battle_chicken_cancel(e.slot);
                    trace_chicken("cancel", e);
                }
                flying = 0;
            }
            return true;
        });
        if (previous != channel.received()) ack_chickens(channel, to);
        send_chickens(channel, to);
    }
    void sync_chickens() {
        if (!chickens_enabled()) return;
        if (role == Role::Guest) apply_chickens(host_chickens, host_addr, nullptr);
        else for (auto &[id, peer] : peers)
            if (peer.alive && peer.waiting_event != event_id) apply_chickens(peer.chickens, peer.addr, &peer);
    }
    void sync_bodywork() {
        const uint64_t now = now_us();
        if (role == Role::Host && now < next_bodywork_sample) return;
        if (role == Role::Host) next_bodywork_sample = now + 125000;
        for (size_t i = 0; i < roster.size(); ++i) {
            auto &shape = bodywork[i];
            const uint32_t spawn = completion_mode == EventCompletionMode::Battle && i < battle.seats.size() ? battle.seats[i].spawn_tick : 0;
            if (role == Role::Guest) {
                if (shape.revision <= shape.applied || shape.spawn != spawn) continue;
                if (netplay_damage::apply(netplay_car::car_of_slot(i), shape.detail)) {
                    shape.applied = shape.revision;
                    Writer ack; ack.u32(event_id); ack.u8(i); ack.u32(shape.revision);
                    send_to(host_addr, K_DAMAGE_DETAIL_ACK, my_id, ack);
                }
                continue;
            }
            if (shape.spawn != spawn) {
                shape = {};
                shape.spawn = spawn;
            }
            // The host owns car-to-car impacts. Outside that contact window a driver
            // can scrape a wall locally without an old host checkpoint repairing it.
            if (shape.capture_until) {
                const bool settled = tick >= shape.capture_until;
                auto detail = netplay_damage::capture(netplay_car::car_of_slot(i));
                if (!detail.empty() && (settled || detail != shape.detail)) {
                    shape.detail = std::move(detail);
                    shape.encoded = netplay_damage::pack(shape.detail);
                    shape.revision = tick + 1;
                    shape.next_retry = 0;
                }
                if (settled) shape.capture_until = 0;
            }
            if (!shape.revision || now < shape.next_retry) continue;
            shape.next_retry = now + 250000;
            Writer w;
            for (const auto &kv : peers) {
                const Peer &p = kv.second;
                if (!p.alive || p.waiting_event == event_id || p.bodywork_acks[i] == shape.revision) continue;
                if (w.b.empty()) {
                    w.u32(event_id); w.u8(i); w.u32(shape.revision); w.u32(spawn);
                    w.b.insert(w.b.end(), shape.encoded.begin(), shape.encoded.end());
                }
                send_to(p.addr, K_DAMAGE_DETAIL, 0, w);
            }
        }
    }
    sockaddr_in addr_of(uint32_t ip_host, uint16_t p) { sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(ip_host); a.sin_port = htons(p); return a; }

    static void populate_draft_from_config(game_frontend::MutatorDraft &out, const mutator::GametypeConfig &cfg, uint8_t preset_id) {
        out = game_frontend::MutatorDraft{};
        out.preset_id = preset_id;
        out.id = cfg.id;
        out.name = cfg.name;
        out.rule_flags = cfg.base_rule_flags;
        out.feat_bits = cfg.base_feats;
        out.tips_fuse_sec = cfg.tips_fuse_sec;
        out.fiesta_interval_sec = cfg.fiesta_interval_sec;
        out.shuffle_rounds = cfg.shuffle_rounds;
        out.rounds_to_win = cfg.rounds_to_win;
        out.gravity_scale = cfg.base_traits.gravity_scale;
        out.mass_scale = cfg.base_traits.mass_scale;
        out.grip_scale = cfg.base_traits.grip_scale;
        out.torque_scale = cfg.base_traits.torque_scale;
        out.top_speed_scale = cfg.base_traits.top_speed_scale;
        out.damage_taken_scale = cfg.base_traits.damage_taken_scale;
        out.chicken_force_scale = cfg.base_traits.chicken_force_scale;
        out.chicken_radius_scale = cfg.base_traits.chicken_radius_scale;
        out.chicken_reload_sec = cfg.base_traits.chicken_reload_sec;
        out.uses_weapons = mutator::config_uses_weapons(cfg);
        for (const auto &tr : cfg.base_triggers) {
            if (tr.kind == mutator::TriggerKind::RingOut) {
                out.storm_radius_m = tr.extents[0];
                out.storm_shrink_mps = tr.shrink_rate_mps;
                out.storm_dps = tr.dps_outside;
                break;
            }
        }
        out.custom_json = mutator::serialize_gametype_json(cfg);
        out.summary_lines.push_back(preset_id < mutator::preset_count() ? mutator::preset_description(preset_id) : "Custom Gametype");
        if (!out.uses_weapons) out.summary_lines.push_back("Weapons: OFF (Mode Enforced)");
        else if (out.feat_bits & (mutator::FB_CHICKEN_STRAIGHT | mutator::FB_CHICKEN_MORTAR))
            out.summary_lines.push_back("Weapons: Chickens ON");
        if (out.rule_flags & mutator::RF_TIPS_TAG) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "Personal Fuse: %.0fs | Immunity: 3.5s", out.tips_fuse_sec);
            out.summary_lines.push_back(buf);
        }
    }
    void send_lobby_mutator_draft(const sockaddr_in *single_peer = nullptr) {
        if (role != Role::Host) return;
        mutator::GametypeConfig cfg;
        std::string err;
        bool ok = false;
        if (has_host_mutator_draft && !host_mutator_draft.custom_json.empty()) {
            ok = mutator::parse_gametype_json(host_mutator_draft.custom_json, cfg, err);
        } else if (gametype_preset != mutator::PRESET_STANDARD) {
            ok = mutator::build_preset(gametype_preset, cfg);
        } else {
            cfg.id = "standard";
            cfg.name = "Standard (Vanilla)";
            ok = true;
        }
        if (!ok) return;
        Writer mw;
        mw.u32(0); // event_id = 0 -> pre-start lobby draft sync
        mutator::put_mutator_config(mw, cfg);
        if (single_peer) {
            send_to(*single_peer, K_MUTATOR_CONFIG, 0, mw);
        } else {
            for (auto &kv : peers) {
                if (kv.second.alive) send_to(kv.second.addr, K_MUTATOR_CONFIG, 0, mw);
            }
        }
    }

    // ---------------------------------------------------------------- lobby helpers
    std::string mode_name() const { return event_pick; }
    std::string mode_display_name() const {
        const uint8_t preset = (role == Role::Host) ? gametype_preset : lobby_gametype_preset;
        if (preset != mutator::PRESET_STANDARD) return mutator::preset_label(preset);
        return (role == Role::Host) ? event_pick : lobby_event;
    }
    std::string battle_event_for_track(const session_catalog::Track *t) const {
        if (!t) return {};
        if (!maps.id.empty()) return t->human_battle_contest;
        std::string fallback;
        for (size_t i = 0; i < t->events.size() && i < t->event_types.size(); ++i) {
            if (t->event_types[i] != 0x18000224u) continue;
            if (t->events[i] != session_catalog::kHumanBattle) return t->events[i];
            if (fallback.empty()) fallback = t->events[i];
        }
        return !fallback.empty() ? fallback : t->human_battle_contest;
    }
    bool human_battle() const {
        return gametype_preset == mutator::PRESET_STANDARD && event_pick == session_catalog::kHumanBattle;
    }
    unsigned capacity() const { return human_battle() ? 2 : host_capacity; }
    const char *human_battle_wait() const {
        if (!human_battle() || alive_count() == 1) return nullptr;
        return alive_count() == 0 ? "Waiting for a second driver" :
               "Choose a larger event or reduce the lobby to two drivers";
    }
    // ---- catalog-backed picks (guest context fills the catalog; see Session::menu_buttons)
    const session_catalog::Track *find_track(const std::string &name) const {
        for (const auto &track : session_catalog::tracks())
            if (track.name == name) return &track;
        if (!maps.base_arena.empty()) {
            const auto *canvas = session_catalog::track_for_arena(maps.base_arena);
            if (canvas && canvas->name == name) return canvas;
        }
        return nullptr;
    }
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
        if (t && role == Role::Host && !maps.id.empty()) {
            event_pick = t->human_battle_contest;
        } else if (t && role == Role::Host && gametype_preset != mutator::PRESET_STANDARD) {
            const std::string bev = battle_event_for_track(t);
            if (!bev.empty()) {
                event_pick = bev;
            } else {
                gametype_preset = mutator::PRESET_STANDARD;
                event_pick = t->events.empty() ? std::string() : t->events[0];
            }
        } else if (t && std::find(t->events.begin(), t->events.end(), event_pick) == t->events.end()) {
            const int idx = std::getenv("EOD_NETPLAY_MODE") ? std::atoi(std::getenv("EOD_NETPLAY_MODE")) % 3 : 0;
            const char *fixture = netplay_impact_fixture::test_session() ? std::getenv("EOD_NETPLAY_TEST_EVENT") : nullptr;
            const auto wanted = fixture ? fixture : kDefaultEvents[idx];
            event_pick = std::find(t->events.begin(), t->events.end(), wanted) != t->events.end() ? wanted : t->events[0];
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
        if (paints.empty()) {
            my_vehicle = 0;
            my_vname.clear();
            return;
        }
        my_paint = ((my_paint % (int)paints.size()) + (int)paints.size()) % (int)paints.size();
        my_vehicle = paints[(size_t)my_paint];
        my_vname = vehicle_label(my_model, my_paint, paints.size());
    }
    static constexpr struct RuleOption { const char *id, *label; uint8_t bit; } rule_options[] = {
        {"rule_turbo","TURBO BOOST",netplay_hooks::Turbo}, {"rule_chicken","CHICKEN FIGHT",netplay_hooks::Chickens},
        {"rule_damage","INVINCIBILITY",netplay_hooks::Invincible}, {"rule_durability","CAR DURABILITY",netplay_hooks::Durability},
        {"rule_recovery","MANUAL RECOVERY",netplay_hooks::ManualRecovery}
    };
    static constexpr struct TuneOption { const char *id, *label, *unit; netplay_hooks::Tune tune; } tune_options[] = {
        {"tune_turbo","TURBO STRENGTH","X",netplay_hooks::TurboStrength}, {"tune_reload","CHICKEN RELOAD"," S",netplay_hooks::ChickenReload},
        {"tune_force","CHICKEN BLAST FORCE","X",netplay_hooks::ChickenForce}, {"tune_size","CHICKEN BLAST SIZE","X",netplay_hooks::ChickenSize}
    };
    void cycle(const std::string &what, int dir) {
        if (!maps.id.empty() && what == "track") return;
        if (what == "players" && role == Role::Host && !human_battle()) {
            host_capacity = std::clamp<int>((int)host_capacity + (dir >= 0 ? 1 : -1),1+alive_count(),kMaxPlayers);
            return;
        }
        if (what == "driver_names" && role == Role::Host) {
            rules ^= names::toggle_bit(mode_name());
            return;
        }
        if (what == "ai_bots" && role == Role::Host && !human_battle()) {
            ai_count = (ai_count + (dir >= 0 ? 1 : 16)) % 17;
            return;
        }
        for (const auto &opt : rule_options) if (what == opt.id) {
            if (role == Role::Host) rules ^= opt.bit;
            return;
        }
        for (const auto &opt : tune_options) if (what == opt.id) {
            const auto range = netplay_hooks::tune_ranges[opt.tune];
            if (role == Role::Host) tuning[opt.tune] = (uint8_t)std::clamp<int>(tuning[opt.tune] + (dir >= 0 ? 1 : -1), range.lo, range.hi);
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
            if (t) {
                struct ModeEntry { uint8_t preset; std::string contest; };
                std::vector<ModeEntry> entries;
                const std::string bev = battle_event_for_track(t);
                if (!maps.id.empty()) {
                    if (!bev.empty()) entries.push_back({mutator::PRESET_STANDARD, bev});
                } else {
                    for (const auto &ev : t->events) entries.push_back({mutator::PRESET_STANDARD, ev});
                }
                if (!bev.empty()) {
                    for (uint8_t p = 1; p < mutator::preset_count(); ++p) {
                        entries.push_back({p, bev});
                    }
                }
                if (!entries.empty()) {
                    size_t cur = 0;
                    for (size_t i = 0; i < entries.size(); ++i) {
                        if (entries[i].preset == gametype_preset &&
                            (gametype_preset != mutator::PRESET_STANDARD || entries[i].contest == event_pick)) {
                            cur = i;
                            break;
                        }
                    }
                    const auto &next = entries[step(entries.size(), cur)];
                    gametype_preset = next.preset;
                    event_pick = next.contest;
                    mutator::GametypeConfig cfg;
                    if (gametype_preset != mutator::PRESET_STANDARD && mutator::build_preset(gametype_preset, cfg)) {
                        populate_draft_from_config(host_mutator_draft, cfg, gametype_preset);
                        has_host_mutator_draft = true;
                    } else {
                        has_host_mutator_draft = false;
                        host_mutator_draft = {};
                    }
                    send_lobby_mutator_draft();
                }
            }
        } else if (what.compare(0, 10, "car_model:") == 0) {
            const std::string want = what.substr(10);
            auto models = session_catalog::models();
            if (std::find(models.begin(), models.end(), want) != models.end()) {
                my_model = want;
                my_paint = 0;
            }
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
        for (const auto &kv : peers) if (kv.second.alive) v.push_back({kv.first, kv.second.name, kv.second.ready, kv.second.vname, (uint16_t)kv.second.ping_ms, event_id && kv.second.waiting_event == event_id && (go || pending_start || my_phase != P_LOBBY)});
        return v;
    }
    void put_lobby(Writer &w, uint32_t ts, bool round_active, bool waiting) {
        w.u32(ts); w.u32(event_id); w.u8(my_phase | (round_active ? 0x40 : 0) | (waiting ? 0x80 : 0)); w.str(mode_name(), 40); w.u8((uint32_t)ai_count);
        auto v = make_lobby();
        w.u8((uint32_t)v.size());
        for (auto &p : v) { w.u8(p.id); w.str(p.name, 16); w.u8((p.ready ? 1 : 0) | (p.waiting ? 2 : 0)); w.str(p.vname, 48); w.u16(p.ping); }
        w.str(track_name, 40); w.u8(rules); w.u8(capacity());
        for (uint8_t q : tuning) w.u8(q);
        w.str(maps.offer.hash, 64); w.u32(maps.offer.size); w.str(maps.offer.title, 160);
        w.u8(gametype_preset);
    }
    template <typename Predicate> bool all_peers(Predicate pred) const { for (auto &kv : peers) if (kv.second.alive && !pred(kv.second)) return false; return true; }
    void release_driver(Peer &peer) {
        peer.has_latest = false;
        peer.impact_through = peer.impact_revision = 0;
        // Keep a departed duel opponent under network control until event teardown.
        if (event_name == session_catalog::kHumanBattle && go && (event_guest_mask & (1u << peer.id))) {
            if (!result_until && !abort_requested) {
                abort_requested = true;
                strip = "Opponent left the 1v1 Battle. Returning to lobby.";
                session_abort_event();
            }
            return;
        }
        auto car = puppet_cars.find(peer.id);
        if (car != puppet_cars.end()) {
            netplay_car::set_puppet(car->second, false);
            puppet_cars.erase(car);
        }
        for (size_t slot = 0; slot < roster.size(); ++slot) {
            if (roster[slot].owner == peer.id) {
                remote.erase((int)slot);
                shown.erase((int)slot);
            }
        }
    }
    int alive_count() const { int n = 0; for (auto &kv : peers) n += kv.second.alive; return n; }
};

Session::Session() : d_(new Impl) {
    // Session teardown may release a mounted archive during process shutdown.
    (void)eod::vfs::global_vfs();
}
Session::~Session() { if (d_->mic_open) platform_mic_close(); d_->close_game(); if (d_->disc_fd >= 0) EOD_CLOSESOCK(d_->disc_fd); netplay_online::Online::wait_cleanup(2500); }

// ====================================================================== UI side
game_frontend::Snapshot Session::snapshot() const {
    Impl &d = *d_;
    Session *self = const_cast<Session *>(this);
    self->d_->ensure_discovery();
    using S = game_frontend::Snapshot;
    S s;
    s.direct_connect = true; s.internet_invites = true; s.native_race_sync = true;
    s.voice = netplay::voice::Session::available();
    s.microphone_enabled = d.mic_enabled;
    s.microphone_capture = d.mic_open;
    s.muted = d.muted_all;
    s.deafened = d.deafened;
    s.error = d.last_error;
    if (d.profile_check) {
        std::lock_guard<std::mutex> lock(d.profile_check->mutex);
        s.checking_installed_content = !d.profile_check->done;
        s.can_match_content = d.profile_check->match.has_value();
        if (s.can_match_content) s.error = "The host's " +
            std::string(d.profile_check->match->empty() ? "disc content" : "gameplay content") +
            " is already installed. Select Restart and Rejoin to use it for this session.";
    }
    s.retryable_error = d.retryable_loss;
    s.content_problem = d.content_note;
    s.is_host = d.role == Impl::Role::Host;
    if (d.role == Impl::Role::None && d.pending_join && !in_event_) {
        s.phase = S::Phase::CheckingCompatibility; s.status = "Checking your game files"; s.can_cancel = true;
    } else if (d.role == Impl::Role::None) {
        s.phase = in_event_ ? S::Phase::Results : d.last_error.empty() ? S::Phase::Offline : S::Phase::Error;
        s.status = in_event_ ? "Leaving the match" : "Not connected";
        if (d.online_on) s.route = d.route_text;
        uint64_t t = now_us();
        for (auto &kv : d.crew)
            if (t - kv.second.seen < 9000000) s.crew.push_back({kv.second.name, kv.first, kv.second.players});
    } else if (d.role == Impl::Role::Guest && !d.lobby_received) {
        s.phase = S::Phase::Connecting;
        s.status = d.welcomed ? "Waiting for lobby details" : d.join_note.empty() ? "Contacting the host" : d.join_note;
        s.can_cancel = true;
        s.route = d.route_text;
    } else {
        s.phase = d.abort_requested || d.result_until ? S::Phase::Results : in_event_ && d.bound ? S::Phase::Racing :
                  (in_event_ || d.pending_start) ? S::Phase::Loading : S::Phase::Lobby;
        s.status = s.phase == S::Phase::Results ? "Returning to lobby" :
                   s.phase == S::Phase::Racing ? "Racing" :
                   s.phase == S::Phase::Loading ? "Loading race" : d.status;
        s.custom_map_id = d.maps.id;
        s.custom_map_title = d.maps.offer.title;
        s.lobby_ai_count = s.is_host ? d.ai_count : d.lobby_ai_count;
        s.lobby_capacity = s.is_host ? static_cast<int>(d.capacity()) : static_cast<int>(d.lobby_capacity);
        s.map_verified = (d.maps.verified == d.maps.offer.hash);
        s.map_downloading = (d.role == Impl::Role::Guest && !s.map_verified && d.maps.offer.size > 0);
        s.map_download_pct = s.map_downloading
            ? static_cast<int>(std::min<uint64_t>(100, 100ULL * d.maps.download.received_bytes() / d.maps.offer.size))
            : (s.map_verified ? 100 : 0);
        if (d.role == Impl::Role::Host) s.custom_maps = d.maps.catalog;
        else if (s.map_downloading)
            s.status = "Receiving " + d.maps.offer.title + " (" + std::to_string(s.map_download_pct) + "%)";
        s.can_cancel = true;
        s.invite = d.role == Impl::Role::Host ? d.host_address : "";
        s.address = s.invite;
        s.code = d.role == Impl::Role::Host ? d.code : "";
        s.track_pic = d.role == Impl::Role::Host ? d.track_name : d.lobby_track; s.car_pic = d.my_vehicle;
        s.route = d.route_text;
        const auto *track = d.find_track(s.track_pic);
        bool detention = false;
        if (track) {
            const auto event = std::find(track->events.begin(), track->events.end(), s.is_host ? d.event_pick : d.lobby_event);
            detention = event != track->events.end() && track->event_types[event - track->events.begin()] == kDetention;
        }
        const uint64_t t_now = now_us();
        const uint8_t local_id = d.role == Impl::Role::Host ? 0 : d.my_id;
        if (d.role == Impl::Role::Host) {
            for (auto &p : d.make_lobby()) {
                const bool spk = p.id < kMaxPlayers && t_now < d.speaking_until[p.id];
                const bool mtd = p.id != local_id && (d.muted_all || d.deafened);
                s.players.push_back({p.id, p.name + (p.id == local_id ? " (YOU)  " : "  ") + (detention && p.id == 0 ? "SCHOOL BUS" : p.vname) + (p.id ? "  " + std::to_string(p.ping) + "ms" : ""), p.ready, mtd, false, spk});
                s.players.back().waiting = p.waiting;
                if (p.id) {
                    const Peer &peer = d.peers.at(p.id);
                    s.players.back().identity = peer.identity;
                    s.players.back().map_ready = (peer.verified_map == d.maps.offer.hash);
                }
            }
            const bool joining = d.relay.has_joining_guest(t_now);
            s.can_start = s.phase == S::Phase::Lobby && !joining && !d.human_battle_wait() && d.in_menu && session_catalog::ready() &&
                          session_catalog::online_vehicle_is_drivable(d.my_vehicle) &&
                          d.all_peers([&](const Peer &p) {
                              return p.at_menu && p.ready && p.verified_map == d.maps.offer.hash &&
                                     session_catalog::online_vehicle_is_drivable(p.vehicle);
                          });
            if (!s.can_start && s.phase == S::Phase::Lobby) {
                if (joining || !d.all_peers([](const Peer &p) { return p.lobby_heartbeat_received; }))
                    s.status = "Waiting for players to finish joining";
                else if (const char *reason = d.human_battle_wait()) s.status = reason;
                else s.status = d.in_menu ? "Waiting for game data or players to be ready" : "Waiting for the game main menu";
            }
            auto ai_label = [](int v) -> std::string {
                if (v <= 0) return "OFF";
                if (v >= 16) return "FILL TO MAX";
                return std::to_string(v);
            };
            s.options.push_back({"players","MAX PLAYERS",std::to_string(d.capacity()),s.phase == S::Phase::Lobby && !d.human_battle(),"Includes the host. Race capacity depends on the event."});
            s.options.push_back({"ai_bots","AI BOTS",ai_label(d.ai_count),s.phase == S::Phase::Lobby && !d.human_battle(),"AI opponents to add: OFF (humans only), 1-15, or FILL TO MAX."});
            s.options.push_back({"track", "TRACK", d.track_name.empty() ? "Loading game data..." : d.track_name, d.maps.id.empty(), ""});
            s.options.push_back({"mode", "MODE", d.mode_display_name(), s.phase == S::Phase::Lobby,
                                 d.gametype_preset != mutator::PRESET_STANDARD ? mutator::preset_description(d.gametype_preset) : ""});
        } else {
            for (auto &p : d.lobby_players) {
                const bool spk = p.id < kMaxPlayers && t_now < d.speaking_until[p.id];
                const bool mtd = p.id != local_id && (d.muted_all || d.deafened);
                s.players.push_back({p.id, p.name + (p.id == local_id ? " (YOU)  " : "  ") + (detention && p.id == 0 ? "SCHOOL BUS" : p.vname) + "  " + std::to_string(p.ping) + "ms", p.ready, mtd, false, spk});
                s.players.back().waiting = p.waiting;
            }
            s.ready = d.guest_ready;
            s.can_rejoin_lobby = in_event_ && d.bound && d.go && !d.abort_requested && !d.result_until;
            if (s.phase == S::Phase::Lobby && d.waiting_event && d.waiting_event == d.event_id)
                s.status = "In lobby for the next event";
            else if (s.phase == S::Phase::Lobby && s.ready) s.status = "Waiting for host";
            auto ai_label = [](int v) -> std::string {
                if (v <= 0) return "OFF";
                if (v >= 16) return "FILL TO MAX";
                return std::to_string(v);
            };
            s.options.push_back({"players","MAX PLAYERS",std::to_string(d.lobby_capacity),false,"Host only."});
            s.options.push_back({"ai_bots","AI BOTS",ai_label(d.lobby_ai_count),false,"Host only."});
            s.options.push_back({"track", "TRACK", d.lobby_track, false, "Host only."});
            s.options.push_back({"mode", "MODE", d.mode_display_name(), false,
                                 d.lobby_gametype_preset != mutator::PRESET_STANDARD ? mutator::preset_description(d.lobby_gametype_preset) : "Host only."});
        }
        if (detention) {
            s.options.push_back({"detention_role", "YOUR ROLE", s.is_host ? "BUS DRIVER" : "RUNNER", false,
                                 "The host drives the school bus. Other players drive their selected cars."});
        }
        if (detention && s.is_host) {
            s.options.push_back({"assigned_car", "YOUR CAR", "SCHOOL BUS", false, "Assigned by Detention."});
            s.car_pic = 0;
        } else {
            s.options.push_back({"car", "YOUR CAR", d.my_model.empty() ? "Loading game data..." : d.my_model, true, ""});
            s.options.push_back({"paint", "PAINT", d.my_model.empty() ? "-" : "PAINT " + std::to_string(d.my_paint + 1) + " OF " + std::to_string(std::max<size_t>(1, session_catalog::paints(d.my_model).size())), true, ""});
        }
        const auto &tag_mode = s.is_host ? d.event_pick : d.lobby_event;
        s.options.push_back({"driver_names", "PLAYER GAMERTAGS", names::enabled(tag_mode, d.rules) ? "ON" : "OFF",
                             s.is_host && s.phase == S::Phase::Lobby, "Show the names of human drivers above their cars."});
        for (const auto &opt : Impl::rule_options)
            s.options.push_back({opt.id,opt.label,(d.rules & opt.bit) ? "ON" : "OFF",s.is_host,
                                 "Applies to all players. Locked during the race."});
        for (const auto &opt : Impl::tune_options) {
            char value[24]; std::snprintf(value, sizeof value, "%.2f%s", d.tuning[opt.tune] * 0.25, opt.unit);
            s.options.push_back({opt.id, opt.label, value, s.is_host, "Applies to all players. Locked during the race."});
        }
    }
    if (s.phase != S::Phase::Lobby) for (auto &o : s.options) o.editable = false;
    uint64_t t = now_us();
    for (auto &kv : d.lan) if (t - kv.second.seen < 4000000) s.lan.push_back({kv.second.name, kv.first, kv.second.players});
    s.strip = d.strip;

    // Unified Front-End persistent catalog & live state (Phases 2-5)
    s.catalog_ready = session_catalog::ready();
    s.ee_state = static_cast<int>(session_ee_state());
    for (const auto &tr : session_catalog::tracks()) s.retail_tracks.push_back(tr.name);
    s.retail_cars = session_catalog::models();
    // Build full class-sorted Car Catalog (Compact -> Midsize -> Muscle -> Special -> Trailers -> Modded)
    {
        auto class_order = [](const session_catalog::Vehicle &v) -> int {
            if (v.is_modded) return 5;
            if (v.is_trailer) return 4;
            switch (v.car_class) {
            case 0: return 0; // Compact
            case 1: return 1; // Midsize
            case 2: return 2; // Muscle
            default: return 3; // Special
            }
        };
        auto class_label = [](int ord) -> const char * {
            switch (ord) {
            case 0: return "COMPACT";
            case 1: return "MIDSIZE";
            case 2: return "MUSCLE";
            case 3: return "SPECIAL";
            case 4: return "TRAILERS";
            default: return "MODDED";
            }
        };
        std::map<std::string, size_t> seen;
        for (const auto &v : session_catalog::vehicles()) {
            if (v.name.empty() || !v.number) continue;
            auto it = seen.find(v.name);
            if (it != seen.end()) {
                s.car_catalog[it->second].paint_count++;
                continue;
            }
            const int ord = class_order(v);
            game_frontend::CarEntry ce;
            ce.model = v.name;
            ce.category = class_label(ord);
            ce.car_class = v.car_class;
            ce.vehicle_num = v.number;
            ce.paint_count = 1;
            ce.is_trailer = v.is_trailer;
            ce.is_modded = v.is_modded;
            seen[v.name] = s.car_catalog.size();
            s.car_catalog.push_back(std::move(ce));
        }
        std::stable_sort(s.car_catalog.begin(), s.car_catalog.end(), [](const game_frontend::CarEntry &a, const game_frontend::CarEntry &b) {
            auto rank = [](const game_frontend::CarEntry &c) {
                if (c.is_modded) return 5;
                if (c.is_trailer) return 4;
                return (int)std::min<uint32_t>(c.car_class, 3);
            };
            const int ra = rank(a), rb = rank(b);
            if (ra != rb) return ra < rb;
            return a.model < b.model;
        });
    }
    if (d.role == Impl::Role::None) {
        s.custom_maps.clear();
        for (const auto &cm : catalog_cache::global().maps()) {
            s.custom_maps.push_back({cm.filename, cm.title, cm.base_arena, cm.author});
        }
    }
    // Modes catalog: Retail + Party & Arena + Custom JSON
    for (uint8_t pid = mutator::PRESET_BATTLE_ROYALE; pid < mutator::PRESET_CUSTOM_JSON; ++pid) {
        game_frontend::GameModeEntry me;
        me.id = "preset:" + std::to_string(pid);
        me.name = mutator::preset_label(pid);
        me.category = "PARTY & ARENA";
        me.description = mutator::preset_description(pid);
        me.preset_id = pid;
        me.uses_weapons = mutator::preset_uses_weapons(pid);
        me.featured = (pid == mutator::PRESET_TIPS_TAG || pid == mutator::PRESET_BATTLE_ROYALE || pid == mutator::PRESET_DESTRUCTIONWARE);
        s.modes.push_back(std::move(me));
    }
    for (const auto &cg : catalog_cache::global().gametypes()) {
        game_frontend::GameModeEntry me;
        me.id = cg.config.id;
        me.name = cg.config.name;
        me.category = "MY CUSTOM MODES";
        me.description = "Custom gametype from " + cg.filename;
        me.default_track = cg.config.default_track;
        me.is_custom_json = true;
        me.uses_weapons = mutator::config_uses_weapons(cg.config);
        s.modes.push_back(std::move(me));
    }
    // Career summary from EE RAM (0x003D9384 = CareerSaveInfo)
    if (g_ram) {
        const uint32_t save_ptr = ee_r32(0x003D9384);
        if (save_ptr && !(save_ptr & 3) && save_ptr <= EE_RAM_SIZE - 0xE7C && ee_r32(save_ptr) == 0x43726572u) {
            s.career.loaded = true;
            s.career.money = static_cast<int>(ee_r32(save_ptr + 0x80 + 0x08));
            s.career.garage_count = static_cast<int>(ee_r32(save_ptr + 0x80 + 0x10));
            s.career.garage_cap = g_config.career_garage_expansion ? 16 : 4;
            s.career.rank = static_cast<int>(ee_r32(save_ptr + 0xCE8));
            int done_eves = 0;
            for (uint32_t i = 0; i < 40; ++i) {
                if (ee_r8(save_ptr + 0xCE8 + 0x118 + i) != 0xFF) ++done_eves;
            }
            s.career.events_done = done_eves;
        }
    }
    if (d.has_host_mutator_draft) {
        s.mutator_draft = d.host_mutator_draft;
    } else if (d.role != Impl::Role::None) {
        const uint8_t active_preset = (d.role == Impl::Role::Host) ? d.gametype_preset : d.lobby_gametype_preset;
        mutator::GametypeConfig pcfg;
        if (active_preset != mutator::PRESET_STANDARD && mutator::build_preset(active_preset, pcfg)) {
            Impl::populate_draft_from_config(s.mutator_draft, pcfg, active_preset);
        }
    }
    // Build 16-slot fixed roster
    s.roster_16.resize(16);
    for (uint8_t i = 0; i < 16; ++i) s.roster_16[i].slot = i;
    for (const auto &pl : s.players) {
        if (pl.slot < 16) {
            auto &sl = s.roster_16[pl.slot];
            sl.occupied = true;
            sl.is_ai = false;
            sl.is_host = (pl.slot == 0);
            sl.ready = pl.ready;
            sl.speaking = pl.speaking;
            sl.muted = pl.muted;
            sl.waiting = pl.waiting;
            sl.map_ready = pl.map_ready;
            sl.name = pl.name;
            sl.identity = pl.identity;
        }
    }
    if (mutator_hooks::active) {
        for (uint8_t i = 0; i < 16; ++i) {
            if (!mutator_hooks::slots[i].active) continue;
            game_frontend::ScoreboardRow row;
            row.slot = i;
            row.name = (i < s.roster_16.size() && s.roster_16[i].occupied) ? s.roster_16[i].name : ("CAR " + std::to_string(i + 1));
            row.role = mutator::role_id_name(mutator_hooks::slots[i].role);
            row.score = mutator_hooks::slots[i].score;
            row.alive = (mutator_hooks::hud.eliminated_mask & (1u << i)) == 0;
            row.is_tagger = (mutator_hooks::hud.tips_in_slot == i);
            row.fuse_sec = row.is_tagger ? mutator_hooks::hud.tips_fuse_sec : 0.0f;
            s.scoreboard.push_back(std::move(row));
        }
    }
    return s;
}

void Session::viewing_online() {
    Impl &d = *d_;
    const uint64_t t = now_us();
    if (t - d.ui_seen >= 3000000) d.refresh_content_check();   // tab just came on screen
    d.ui_seen = t;
}

void Session::submit(game_frontend::Command c) {
    Impl &d = *d_;
    using K = game_frontend::Command::Kind;
    if (c.kind == K::MatchContent) {
        if (in_event_ || d.role != Impl::Role::None || !d.guest_invite || !d.profile_check) return;
        std::optional<std::string> match;
        { std::lock_guard<std::mutex> lock(d.profile_check->mutex); match = d.profile_check->match; }
        if (!match) return;
        std::fprintf(stderr, "[netplay] restarting with a matching installed profile\n");
        online_profile::restart_with(*match, *d.guest_invite);
        d.profile_check.reset();
        d.last_error = "Could not restart the game. Reopen it with the host's content and rejoin.";
        return;
    }
    d.last_error.clear();
    d.retryable_loss = false;
    const bool joining = c.kind == K::Host || c.kind == K::JoinCode || c.kind == K::JoinAddress || c.kind == K::JoinCrew;
    if (joining) {
        if (in_event_ || d.role != Impl::Role::None) return;
        if (eod::vfs::global_vfs().is_mounted(eod::vfs::Layer::EphemeralPie) && eod::vfs::active_map()) {
            if (d.in_menu) {
                eod::vfs::global_vfs().unmount(eod::vfs::Layer::EphemeralPie);
            } else {
                d.last_error = "Leave the local map playtest before entering an online lobby.";
                return;
            }
        }
        d.profile_check.reset();
        if (c.kind != K::Host) d.guest_invite = c;
        else d.guest_invite.reset();
        netplay::ContentFingerprint content;
        if (!d.content_ready(content)) {   // never hash the game files on the game (main) thread
            if (!d.content_job) d.start_content_check();
            d.pending_join = std::make_unique<game_frontend::Command>(c);
            return;
        }
        d.pending_join.reset();
        d.welcomed = d.lobby_received = false;
        d.lobby_players.clear();
        d.lobby_track.clear();
        d.lobby_event.clear();
        d.lobby_gametype_preset = mutator::PRESET_STANDARD;
        d.lobby_ai_count = 0;
        d.last_host_ts = 0;
        d.heartbeat_serial = 0;
        d.handshake_started = 0;
        d.host_clock = {};
        d.last_clock_probe = 0;
        d.name = clean_name(g_config.nickname);
        if (d.name.empty()) d.name = default_name();
        if (!content.error.empty()) { d.content_job.reset(); d.last_error = "Could not check game compatibility: " + content.error; return; }
        d.checked_content = std::move(content);
        std::fprintf(stderr,"[netplay] content %s\n",d.checked_content.value.c_str());
        d.rules = 0;
        {   // a new lobby starts from the host's own tuning
            using namespace netplay_hooks;
            d.tuning[TurboStrength] = tune_quarters(TurboStrength, g_config.turbo_scale);
            d.tuning[ChickenForce] = tune_quarters(ChickenForce, g_config.chicken_force_scale);
            d.tuning[ChickenSize] = tune_quarters(ChickenSize, g_config.chicken_radius_scale);
            d.tuning[ChickenReload] = tune_quarters(ChickenReload, g_config.chicken_reload);
        }
        d.event_id = d.waiting_event = 0;
        d.event_guest_mask = 0;
        d.pending_start = false;
        d.abort_requested = false;
        d.inject_count = 0;
        d.my_phase = P_LOBBY;
    }
    switch (c.kind) {
    case K::Host: {
        if (d.role != Impl::Role::None) return;
        const bool ephemeral_port = netplay_impact_fixture::enabled() || std::getenv("EOD_NETPLAY_TEST_EPHEMERAL_PORT");
        if (!d.open(ephemeral_port ? 0 : kGamePort, !ephemeral_port)) { d.last_error = "Could not open the network port. Another copy of the game may already be hosting; close it and try again."; return; }
        d.load_friends();
        d.host_address = local_ip() + ":" + std::to_string(d.port);
        d.role = Impl::Role::Host; d.peers.clear(); d.ensure_defaults(); d.status = "Waiting for friends"; d.my_phase = P_LOBBY;
        d.maps.inventory(d.map_directory());
        d.my_vehicle = d.my_vehicle ? d.my_vehicle : 0;
        d.code = std::getenv("EOD_NETPLAY_CODE") ? std::getenv("EOD_NETPLAY_CODE") : Impl::new_code();
        d.route_text = "Looking for the best route...";
        if (!std::getenv("EOD_NETPLAY_NO_INTERNET")) {
            if (GarageRelay::configured()) { d.join_started = now_us(); d.relay.start(d.code, true); }
            else { d.online_start(true); d.host_jobs(); }
        }
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
        d.status = "Connecting..."; d.guest_ready = false;
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
        d.status = "Connecting..."; d.join_note = "Looking for " + (c.kind == K::JoinCode ? "game " + shown : shown + "'s game") + "...";
        d.route_text = "Looking for the best route...";
        if (GarageRelay::configured()) {
            d.relay.start(secret, false);
            d.join_attempt = d.relay.join_attempt();
        } else {
            d.online_start(false);
            d.guest_jobs(secret);
            d.online.poke();
        }
        std::fprintf(stderr, "[netplay] joining by %s %s\n", c.kind == K::JoinCode ? "code" : "friend", shown.c_str());
        break;
    }
    case K::Nickname: {
        g_config.nickname = clean_name(c.text);
        const auto requested = g_config.nickname.empty() ? default_name() : g_config.nickname;
        d.name = d.role == Impl::Role::Host ? d.available_name(requested, 0) : requested;
        break;
    }
    case K::SelectCustomMap: {
        if (d.role != Impl::Role::Host || d.pending_start || in_event_ || d.maps.id == c.text) return;
        if (!d.maps.select(c.text, d.last_error)) return;
        if (!c.text.empty()) {
            auto *track = session_catalog::track_for_arena(d.maps.base_arena);
            if (!track || track->human_battle_contest.empty()) {
                d.maps.clear();
                for (auto &entry : d.peers) entry.second.ready = false;
                d.last_lobby_send = 0;
                d.last_error = "This map needs an installed arena with a Battle event.";
                return;
            }
            d.track_name = track->name;
            d.ensure_defaults();
        }
        for (auto &entry : d.peers) entry.second.ready = false;
        d.last_lobby_send = 0;
        break;
    }
    case K::Ready:
        if (d.role == Impl::Role::Guest && d.lobby_received && !in_event_ && !d.pending_start) {
            if (c.enabled && d.maps.verified != d.maps.offer.hash) {
                d.last_error = "Waiting for the verified host map.";
                break;
            }
            if (c.enabled && !session_catalog::online_vehicle_is_drivable(d.my_vehicle)) {
                d.last_error = "Choose a drivable car before marking Ready.";
                break;
            }
            d.guest_ready = c.enabled;
            d.last_hb = 0;
        }
        break;
    case K::Start: {
        if (d.role != Impl::Role::Host || d.pending_start || in_event_) return;
        if (d.relay.has_joining_guest(now_us())) { d.last_error = "Waiting for players to finish joining"; return; }
        if (!d.in_menu) { d.last_error = "Go back to the game's main menu first (leave the current event), then press START."; return; }
        for (auto &kv : d.peers) if (kv.second.alive && !kv.second.at_menu) { d.last_error = kv.second.name + " is not at the main menu yet."; return; }
        if (!d.all_peers([&](const Peer &p) { return p.verified_map == d.maps.offer.hash; })) {
            d.last_error = "Waiting for players to verify the selected map.";
            return;
        }
        for (const auto &kv : d.peers) if (kv.second.alive && !kv.second.ready) { d.last_error = kv.second.name + " is not ready."; return; }
        if (!session_catalog::ready() || d.track_name.empty()) { d.last_error = "The game data is still loading. Wait a moment at the main menu, then press START."; return; }
        d.ensure_defaults();
        if (!d.maps.id.empty()) {
            const auto *arena = session_catalog::track_for_arena(d.maps.base_arena);
            if (!arena || arena->name != d.track_name || arena->human_battle_contest.empty() ||
                d.mode_name() != arena->human_battle_contest) {
                d.last_error = "The host map requires its original Battle arena.";
                return;
            }
        }
        if (const char *reason = d.human_battle_wait()) { d.last_error = reason; return; }
        if (!session_catalog::online_vehicle_is_drivable(d.my_vehicle)) {
            d.last_error = "Choose a drivable car before starting.";
            return;
        }
        for (const auto &kv : d.peers) {
            const Peer &peer = kv.second;
            if (peer.alive && !session_catalog::online_vehicle_is_drivable(peer.vehicle)) {
                d.last_error = peer.name + " must select a drivable car installed on the host.";
                return;
            }
        }
        d.event_guest_mask = 0;
        for (const auto &item : d.peers)
            if (item.second.alive) d.event_guest_mask |= uint16_t(1u << item.first);
        d.event_id++; d.start_info = {d.mode_name(), d.track_name, d.event_id, (uint32_t)now_us() ^ 0x5bd1e995u, d.my_vehicle, true};
        {
            mutator::GametypeConfig preset_cfg;
            std::string draft_err;
            if (d.has_host_mutator_draft && !d.host_mutator_draft.custom_json.empty() &&
                mutator::parse_gametype_json(d.host_mutator_draft.custom_json, preset_cfg, draft_err)) {
                mutator::engine().set_config(preset_cfg);
            } else if (mutator::build_preset(d.gametype_preset, preset_cfg)) {
                mutator::engine().set_config(preset_cfg);
            } else {
                mutator::engine().clear_config();
                if (const char *env_path = std::getenv("EOD_GAMETYPE_JSON"); env_path && *env_path) {
                    std::string err;
                    if (!mutator::engine().load_file(env_path, err))
                        std::fprintf(stderr, "[netplay] failed to load EOD_GAMETYPE_JSON '%s': %s\n", env_path, err.c_str());
                }
            }
        }
        d.event_has_mutator = mutator::engine().has_config();
        d.mutator_applied_event = d.event_has_mutator ? d.event_id : 0;
        d.mutator_acks.clear();
        d.mutator_round_acks.clear();
        d.last_mutator_round_send = 0;
        d.last_mutator_trigger_send = 0;
        d.event_name = d.mode_name(); d.pending_start = true; d.have_track = false; d.have_roster = false; d.go = false; d.grid_event = d.applied_grid_event = 0; d.initial_grid.clear(); d.grid_acks.clear(); d.car_done = false; d.loaded_sent = false;
        for (auto &kv : d.peers) { Peer &p = kv.second; p.got_start = p.got_track = p.got_roster = p.got_go = p.car_done = false; p.seen_event = 0; p.phase = P_LOBBY; }
        d.status = "Starting event";
        if (overlay_menu_open()) overlay_toggle();
        break;
    }
    case K::RejoinLobby: {
        if (d.role != Impl::Role::Guest || !in_event_ || !d.bound || !d.go || d.result_until || d.abort_requested) return;
        d.waiting_event = d.event_id;
        d.guest_ready = false;
        d.last_hb = 0;
        d.abort_requested = true;
        d.strip = "Returning to the lobby. Other drivers will keep playing.";
        session_abort_event();
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
        d.pending_join.reset(); d.guest_invite.reset(); d.profile_check.reset();
        if (d.role == Impl::Role::Guest && d.welcomed) { Writer w; d.send_to(d.host_addr, K_LEAVE, d.my_id, w); }
        if (d.role == Impl::Role::Host) { Writer w; for (auto &kv : d.peers) d.send_to(kv.second.addr, K_KICK, 0, w); }
        for (auto &kv : d.puppet_cars) netplay_car::set_puppet(kv.second, false);
        d.puppet_cars.clear(); d.remote.clear(); d.shown.clear();
        if (in_event_) { d.abort_requested = true; session_abort_event(); }
        d.pending_start = false; d.role = Impl::Role::None; d.peers.clear(); d.close_game(); d.status.clear(); d.welcomed = d.lobby_received = false; d.lobby_players.clear();
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
        d.release_driver(it->second);
        d.status = it->second.name + " removed";
        break;
    }
    case K::Option: {
        if (d.role == Impl::Role::None || d.pending_start || in_event_) return;
        if (d.role == Impl::Role::Guest && !d.lobby_received) return;
        if (d.role == Impl::Role::Guest && (c.text == "car" || c.text == "paint" || c.text.compare(0, 10, "car_model:") == 0)) {
            d.guest_ready = false;
            d.last_hb = 0;
        }
        if ((c.text == "track" || c.text == "mode") && !d.maps.id.empty()) return;
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
    case K::LaunchTransition: {
        SessionTransition tr;
        tr.target = static_cast<TransitionTarget>(c.transition.target);
        tr.save_career_first = c.transition.save_career_first;
        tr.new_career = c.transition.new_career;
        tr.classic_career_hub = c.transition.classic_career_hub;
        tr.online_arg = c.transition.online_arg;
        tr.career_warp.active = (c.transition.career_place >= 0);
        tr.career_warp.target_place = c.transition.career_place;
        tr.career_warp.target_eve = c.transition.career_eve;
        tr.offline.active = (tr.target == TransitionTarget::Solo || tr.target == TransitionTarget::SplitScreen);
        tr.offline.track = c.transition.track;
        tr.offline.event = c.transition.event;
        tr.offline.custom_map_id = c.transition.custom_map_id;
        tr.offline.gametype_preset = c.transition.gametype_preset;
        tr.offline.has_custom_mutator = !c.transition.mutator_json.empty();
        tr.offline.mutator_json = c.transition.mutator_json;
        tr.offline.humans = c.transition.humans;
        tr.offline.ai_bots = c.transition.ai_bots;
        tr.offline.laps = c.transition.laps;
        tr.offline.seat_vehicles = c.transition.seat_vehicles;
        tr.offline.seat_paints = c.transition.seat_paints;
        tr.offline.seat_cars = c.transition.seat_cars;
        if (tr.offline.active) d.last_offline_match = tr.offline;
        if (d.role != Impl::Role::None && (tr.target == TransitionTarget::Career || tr.target == TransitionTarget::Solo || tr.target == TransitionTarget::SplitScreen)) {
            d.role = Impl::Role::None;
            d.close_game();
            d.lobby_players.clear();
            d.sync_voice();
        }
        session_request_transition(tr);
        break;
    }
    case K::UpdateMutatorDraft: {
        if (d.role == Impl::Role::Guest) return;
        d.host_mutator_draft = c.draft;
        d.has_host_mutator_draft = true;
        if (d.role == Impl::Role::Host && c.draft.preset_id < mutator::preset_count()) {
            d.gametype_preset = c.draft.preset_id;
            d.ensure_defaults();
        }
        ++d.host_mutator_serial;
        if (d.role == Impl::Role::Host && !in_event_) {
            d.send_lobby_mutator_draft();
            d.last_lobby_send = 0;
        }
        break;
    }
    case K::SaveCustomGametype: {
        mutator::GametypeConfig cfg;
        std::string err, out_path;
        if (!c.draft.custom_json.empty() && mutator::parse_gametype_json(c.draft.custom_json, cfg, err)) {
            catalog_cache::global().save_gametype(overlay_platform_mods_root(), cfg, out_path, err);
        }
        break;
    }
    case K::CareerWarp: {
        career_warp_arm(static_cast<int>(c.value), -1, 2);
        break;
    }
    case K::CareerSaveNow: {
        std::fprintf(stderr, "[CAREER] Quick Rail save requested\n");
        break;
    }
    case K::NextRound: {
        std::fprintf(stderr, "[MUTATOR] Host requested Next Round\n");
        break;
    }
    case K::InstantRematch: {
        if (d.last_offline_match.active) {
            SessionTransition tr;
            tr.target = d.last_offline_match.humans > 1 ? TransitionTarget::SplitScreen : TransitionTarget::Solo;
            tr.offline = d.last_offline_match;
            session_request_transition(tr);
        }
        break;
    }
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
    if (d.role == Impl::Role::Guest && d.lobby_received && d.in_menu && !d.guest_ready && d.my_vehicle) {
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
        const bool seated = d.role == Impl::Role::Host || (d.role == Impl::Role::Guest && d.lobby_received);
        if ((seated || d.profile_check) && !began) began = t;
        if (began && t - checked >= 100000) {
            checked = t; std::ifstream script(path); std::string line;
            while (std::getline(script, line)) {
                std::istringstream row(line); unsigned seq; uint64_t delay; std::string action;
                if (!(row >> seq >> delay >> action) || seq <= sequence || t - began < delay * 1000) continue;
                Command command{};
                if (action == "match_content" && d.profile_check && s.snapshot().can_match_content)
                    command.kind = Command::Kind::MatchContent;
                else if (action == "host" && d.role == Impl::Role::None && !in_event_) command.kind = Command::Kind::Host;
                else if (action == "join" && d.role == Impl::Role::None && !in_event_) { command.kind = Command::Kind::JoinAddress; if (!(row >> command.text)) continue; }
                else if (!seated) continue;
                else if (action == "start" && s.snapshot().can_start) command.kind = Command::Kind::Start;
                else if (action == "option") { command.kind = Command::Kind::Option; if (!(row >> command.text >> command.value)) continue; }
                else if (action == "withdraw") command.kind = Command::Kind::RejoinLobby;
                else if (action == "disconnect") command.kind = Command::Kind::Disconnect;
                else if (action == "end") command.kind = Command::Kind::EndEvent;
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
                             seq, action.c_str(), d.capacity(), d.rules, d.alive_count());
            }
        }
    }
    if (in_event_ || d.pending_start) ready_since = 0; // give each rematch the same lobby action window
    const bool repeat = std::getenv("EOD_NETPLAY_TEST_REMATCH") != nullptr;
    const bool repeat_natural = std::getenv("EOD_NETPLAY_TEST_NATURAL_REMATCH") != nullptr;
    if (repeat && s.is_host() && in_event_ && !d.abort_requested && d.tick >= 120) {
        Command end{}; end.kind = Command::Kind::EndEvent; s.submit(end);
    }
    const auto *minimum = std::getenv("EOD_NETPLAY_TEST_PEERS");
    const int required = std::getenv("EOD_NETPLAY_TEST_SOLO") ? 0 : minimum ? std::clamp(std::atoi(minimum), 1, 15) : 1;
    if (d.role == Impl::Role::Host && !s.event_active() && !d.pending_start &&
        (!d.event_id || ((repeat || repeat_natural) && d.event_id == 1)) && d.alive_count() >= required && d.in_menu) {
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
        static bool was_joining = false;
        const bool joining = d.role == Impl::Role::Host && d.relay.has_joining_guest(now_us());
        if (joining != was_joining) {
            std::fprintf(stderr, "[lifecycle] Garage Code join %s\n", joining ? "pending" : "cleared");
            was_joining = joining;
        }
        static std::string previous;
        const bool show_results = event_results() != nullptr;
        const char *state = d.abort_requested ? "returning" : show_results ? "results" : in_event_ ? (d.bound ? "racing" : "loading") :
                            d.pending_start ? "starting" : d.role == Impl::Role::None ? "offline" :
                            d.role == Impl::Role::Guest && !d.lobby_received ? "connecting" : "lobby";
        const std::string banner = strip();
        const char *hud = show_results ? "results" : banner.rfind("Totalled", 0) == 0 ? "totalled" :
                          banner.rfind("Finished", 0) == 0 ? "finished" :
                          banner.rfind("Finish the race", 0) == 0 ? "countdown" : "normal";
        const std::string key = std::string(state) + ":" + std::to_string(d.event_id) + ":" +
                                std::to_string(d.in_menu) + ":" + std::to_string((int)d.role) + ":" + hud;
        if (key != previous) {
            previous = key;
            std::fprintf(stderr, "[lifecycle] ms=%u role=%d event=%u state=%s native_main=%d peers=%d hud=%s\n",
                         now_ms(), (int)d.role, d.event_id, state, (int)d.in_menu, d.alive_count(), hud);
            if (d.role == Impl::Role::None && d.in_menu && !d.last_error.empty()) {
                const auto ui = snapshot();
                std::fprintf(stderr, "[lifecycle] offline retry=%d reason=%s\n", ui.retryable_error, ui.error.c_str());
            }
        }
    }
    if (auto invite = online_profile::take_invite()) {
        std::fprintf(stderr, "[netplay] rejoining the saved lobby with the selected local content\n");
        overlay_resume_join(*invite);
    }
    auto_drive(now_us());
    if (d.pending_join) {   // HOST/JOIN pressed while the content check was running
        netplay::ContentFingerprint ready;
        if (d.content_ready(ready)) { const game_frontend::Command c = *d.pending_join; submit(c); }
    }
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
    // Remembered friends: keep LAN + Crew discovery live whenever idle (role == None)
    // across Unified Hub, Career, Solo, and Split-Screen. Only pause when hosting/joined.
    if (d.role == Impl::Role::None && !std::getenv("EOD_NETPLAY_NO_INTERNET")) {
        d.load_friends();
        if (!d.friends.empty()) {
            if (d.fd < 0) d.open(0, false);
            if (d.fd >= 0 && !d.online_on && !d.relay.active()) {
                if (GarageRelay::configured()) d.relay.start("", false);
                else { d.online_start(false); d.watch_jobs(); }
            }
        } else if (d.online_on || d.relay.active()) {
            d.close_game(); d.crew.clear(); d.route_text.clear();
        }
    }
    if (d.fd < 0) return;
    if (d.online_on) d.route_text = d.route_description(d.online.report());

    if (d.relay.active()) {
        d.relay.poll(d.fd, t);
        if (d.role == Impl::Role::Host) {
            if (d.relay.error() == 'C') {
                d.code = Impl::new_code();
                d.relay.start(d.code, true);
            }
            d.route_text = d.relay.ready() ? "Garage Code ready" :
                (d.relay.error() || t - d.join_started >= kOnlineHandshakeTimeoutUs)
                ? "Garage Code service unavailable. Retrying..." : "Opening your Garage Code...";
        }
        if (t - d.relay_presence_at > 2000000) {
            d.relay_presence_at = t;
            if (d.relay.watching()) {
                for (const auto &f : d.friends) d.relay.query(d.fd, f.secret);
            } else if (d.role == Impl::Role::Host && d.relay.ready()) {
                std::vector<std::string> secrets;
                for (const auto &f : d.friends) secrets.push_back(f.secret);
                d.relay.publish(d.fd, secrets, static_cast<uint8_t>(1 + d.alive_count()));
            }
        }
    }

    uint8_t buf[1528]; sockaddr_in from; socklen_t fl = sizeof from;
    for (int n; (n = (int)recvfrom(d.fd, (char *)buf, sizeof buf, 0, (sockaddr *)&from, &fl)) > 0; fl = sizeof from) {
        const auto source = from;
        if (d.relay.receive(source, buf, n, from, t) && n == 0) {
            sockaddr_in candidate{};
            if (d.relay.take_candidate(candidate)) {
                Writer w;
                if (d.role == Impl::Role::Host) d.send_to(candidate, K_PUNCH, 0, w);
                else if (d.role == Impl::Role::Guest && !d.welcomed && d.targets.empty()) {
                    w.u32(d.join_tag);
                    d.send_to(candidate, K_PROBE, 0, w);
                }
            }
            if (d.relay.watching()) for (const auto &f : d.friends) {
                uint8_t players;
                if (d.relay.presence(f.secret, players))
                    d.crew[f.secret] = CrewHost{{}, f.name, players, t};
            }
            continue;
        }
        if (d.online_on && netplay_online::looks_like_stun(buf, (size_t)n)) { d.online.feed_stun(buf, (size_t)n); continue; }
        Reader r(buf, (size_t)n);
        if (r.u16() != kMagic) continue;
        const uint8_t kind = (uint8_t)r.u8(), sender = (uint8_t)r.u8();
        if (!r.ok) continue;
        if (kind == K_PUNCH) continue;                      // its only job was to open our router for the sender
        if (kind == K_PROBE) {                              // a remembered friend asks whether we are hosting
            const uint32_t tag = r.u32();
            if (r.ok && d.role == Impl::Role::Host && d.tag_known(tag, nullptr)) {
                Writer w; w.u32(tag); w.str(d.name, 16); w.u8(1 + (uint32_t)d.alive_count());
                d.send_to(from, K_PROBE_REPLY, 0, w);
            }
            continue;
        }
        if (kind == K_PROBE_REPLY) {
            const uint32_t tag = r.u32(); std::string nm = r.str(); const uint32_t pc = r.u8();
            if (!r.ok) continue;
            if (d.role == Impl::Role::Guest && !d.welcomed && d.targets.empty() && tag == d.join_tag &&
                d.relay.matches_candidate(from)) d.targets.push_back(from);
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
                if (proto != kProtocol) {
                    w.u8(1); w.u16(kProtocol); w.str(EOD_RELEASE_VERSION, 24);
                    d.send_to(from, K_REFUSE, 0, w);
                    continue;
                }
                const auto content = r.str();
                if (!r.ok) continue;
                if (content != d.checked_content.value) {
                    w.u8(4); put_required_content(w, d.checked_content);
                    d.send_to(from, K_REFUSE, 0, w); continue;
                }
                const auto attempt = r.p < r.e ? r.str() : std::string();
                if (!r.ok || r.p != r.e || sender != 0 || (!attempt.empty() && attempt.size() != 32)) continue;
                if (pp && !attempt.empty() && !pp->join_attempt.empty() && pp->join_attempt != attempt) continue;
                if (!pp && !attempt.empty()) {
                    // A direct JOIN can reach the host even when its reply
                    // never reaches the guest. Move that pending seat once.
                    for (auto &entry : d.peers) {
                        Peer &existing = entry.second;
                        if (existing.alive && existing.join_attempt == attempt) { pp = &existing; break; }
                    }
                    if (pp) {
                        if (pp->lobby_heartbeat_received || GarageRelay::relayed(pp->addr) || !GarageRelay::relayed(from))
                            continue;
                        pp->addr = from;
                        std::fprintf(stderr, "[netplay] pending player %u continued through relay\n", pp->id);
                    }
                }
                if (!pp) {
                    const bool round_active = in_event_ || d.pending_start || d.abort_requested;
                    uint8_t id = 1;
                    while (id < kMaxPlayers && ((d.peers.count(id) && d.peers[id].alive) ||
                           (round_active && (d.event_guest_mask & (1u << id))))) ++id;
                    if (id >= kMaxPlayers || 1 + d.alive_count() >= (int)d.capacity()) { w.u8(3); d.send_to(from, K_REFUSE, 0, w); continue; }
                    Peer p; p.identity = ++d.next_peer_identity; p.addr = from; p.id = id; p.requested_name = nm; p.name = d.available_name(nm,id);
                    p.join_attempt = attempt;
                    p.waiting_event = round_active ? d.event_id : 0;
                    p.at_menu = false; // Wait for the guest's first lobby heartbeat.
                    p.secret = !friend_secret.empty() ? friend_secret : Impl::new_secret();
                    d.peers[id] = p; pp = &d.peers[id];
                    d.remember(nm, p.secret);
                    std::fprintf(stderr, "[netplay] %s joined from %s as player %u\n", nm.c_str(), sockaddr_text(from).c_str(), id);
                }
                w.u8(pp->id);
                { std::vector<uint8_t> raw; netplay_online::unhex(pp->secret, raw); for (uint8_t b : raw) w.u8(b); }
                w.str(d.checked_content.value,16);
                d.relay.admit(attempt, t);
                d.send_to(from, K_WELCOME, 0, w);
                if (!in_event_ && (d.has_host_mutator_draft || d.gametype_preset != mutator::PRESET_STANDARD)) {
                    d.send_lobby_mutator_draft(&from);
                }
                pp->activity.heard();
                d.status = std::to_string(1 + d.alive_count()) + " players in the lobby";
                continue;
            }
            auto it = d.peers.find(sender);
            if (it == d.peers.end() || !it->second.alive || !same_addr(it->second.addr, from)) continue;
            Peer &p = it->second;
            if (kind == K_MAP_REQUEST) {
                const auto hash = r.str();
                const uint32_t first = r.u32(), missing = r.u32();
                if (!r.ok || r.p != r.e || !missing || hash != d.maps.offer.hash ||
                    first >= d.maps.offer.chunks() || t < p.next_map_send) continue;
                p.next_map_send = t + 12000;
                for (uint32_t i = 0; i < maps::window_chunks; ++i) {
                    if (!(missing & (uint32_t(1) << i))) continue;
                    std::vector<uint8_t> data;
                    if (!d.maps.read_chunk(first + i, data)) continue;
                    Writer packet; packet.str(hash, 64); packet.u32(first + i);
                    packet.b.insert(packet.b.end(), data.begin(), data.end());
                    d.send_to(p.addr, K_MAP_CHUNK, 0, packet);
                }
            } else if (kind == K_HEARTBEAT) {
                const auto requested = clean_name(r.str());
                const bool ready = r.u8();
                const auto vehicle = r.u32();
                auto vehicle_name = r.str();
                const auto phase = r.u8();
                const auto seen_event = r.u32();
                const auto flags = r.u8(), echo = r.u32();
                const auto serial = r.u32();
                const auto verified_map = r.p == r.e ? std::string() : r.str();
                if (!r.ok || r.p != r.e || phase > P_DONE ||
                    (p.lobby_heartbeat_received && static_cast<int32_t>(serial - p.heartbeat_serial) <= 0)) continue;
                p.activity.heard();
                p.heartbeat_serial = serial;
                if (requested != p.requested_name) { p.requested_name = requested; p.name = d.available_name(requested, p.id); }
                p.lobby_heartbeat_received = true;
                p.verified_map = verified_map;
                p.ready = ready && verified_map == d.maps.offer.hash; p.vehicle = vehicle; p.vname = std::move(vehicle_name); p.phase = (uint8_t)phase; p.seen_event = seen_event;
                p.at_menu = flags & 1; p.got_track = flags & 2; p.got_roster = flags & 4; p.got_go = flags & 8; p.got_start = flags & 16; p.car_done = flags & 32;
                if ((flags & 64) && seen_event == d.event_id && in_event_ && d.go &&
                    p.waiting_event != d.event_id) {
                    p.waiting_event = d.event_id;
                    d.release_driver(p);
                    std::fprintf(stderr, "[netplay] player %u withdrew from event %u, staying in lobby\n", p.id, d.event_id);
                }
                if (echo && echo == p.last_ts_sent) p.ping_ms = now_ms() - echo;
            } else if (kind == K_LEAVE) {
                p.alive = false;
                std::fprintf(stderr, "[netplay] %s left\n", p.name.c_str());
                d.status = p.name + " left";
                d.release_driver(p);
            } else if (kind == K_CHICKEN || kind == K_CHICKEN_ACK) {
                if (in_event_ && p.waiting_event != d.event_id)
                    d.read_chicken(r, kind, p.chickens, p.addr, p.id);
            } else if (kind == K_DAMAGE_DETAIL_ACK) {
                const uint32_t id = r.u32(), slot = r.u8(), revision = r.u32();
                if (r.ok && r.p == r.e && in_event_ && d.bound && id == d.event_id && slot < d.roster.size() &&
                    p.waiting_event != id && revision == d.bodywork[slot].revision) p.bodywork_acks[slot] = revision;
            } else if (kind == K_GRID_ACK) {
                const uint32_t id = r.u32();
                if (r.ok && id == d.event_id && d.grid_event == id) d.grid_acks[p.id] = id;
            } else if (kind == K_MUTATOR_CONFIG_ACK) {
                const uint32_t id = r.u32();
                if (r.ok && r.p == r.e && id == d.event_id && d.event_has_mutator) d.mutator_acks[p.id] = id;
            } else if (kind == K_MUTATOR_ROUND_ACK) {
                const uint32_t id = r.u32(), serial = r.u32();
                if (r.ok && r.p == r.e && id == d.event_id && serial > d.mutator_round_acks[p.id])
                    d.mutator_round_acks[p.id] = serial;
            } else if (kind == K_RESULT_CLOCK) {
                const auto sent = r.u32();
                if (!r.ok || r.p != r.e) continue;
                Writer w; w.u32(sent); w.u32(now_ms());
                d.send_to(p.addr, K_RESULT_CLOCK, 0, w);
            } else if (kind == K_BATTLE_RESPAWN) {
                const uint32_t id = r.u32(), deaths = r.u32();
                if (!r.ok || r.p != r.e || !deaths || id != d.event_id || !in_event_ ||
                    !d.go || !d.bound || d.abort_requested || d.result_until || p.waiting_event == id ||
                    d.completion_mode != EventCompletionMode::Battle) continue;
                for (size_t i = 0; i < d.roster.size(); ++i)
                    if (d.roster[i].owner == p.id && d.battle.seats[i].dead && d.battle.seats[i].deaths == deaths)
                        p.battle_respawn_requested = deaths;
            } else if (kind == K_FINISH_STATE) {
                const uint32_t id = r.u32();
                const auto state = static_cast<EventFinishState>(r.u8());
                const uint32_t observed_ms = r.u32();
                if (!r.ok || r.p != r.e || id != d.event_id || !in_event_ || !d.bound || !d.go ||
                    d.abort_requested || d.result_event == id ||
                    (state != EventFinishState::Finished && state != EventFinishState::Totalled &&
                     !(state == EventFinishState::Completed && d.completion_mode == EventCompletionMode::Native))) continue;
                const auto seat = std::find_if(d.roster.begin(), d.roster.end(), [&](const RosterSlot &slot) {
                    return slot.owner == p.id;
                });
                if (seat == d.roster.end()) continue;
                const size_t slot = seat - d.roster.begin();
                if (state == EventFinishState::Totalled && p.impact_through &&
                    native_outcome(slot, d.completion_mode) != EventFinishState::Totalled) continue;
                const int32_t age_ms = static_cast<int32_t>(static_cast<uint32_t>(t / 1000) - observed_ms);
                const uint64_t elapsed = t - d.event_started_at;
                if (age_ms < -1000 || (age_ms > 0 && uint64_t(age_ms) * 1000 > elapsed + 1000000)) continue;
                const uint64_t age = std::min<uint64_t>(elapsed, std::max(0, age_ms) * 1000ull);
                if (p.waiting_event != d.event_id) d.completion.record(slot, state, t, t - age);
            } else if (kind == K_SNAP) {
                const uint32_t id = r.u32();
                if (!r.ok || id != d.event_id || !in_event_ || !d.bound || !d.go) continue;
                const uint32_t ack = r.u32();
                if (r.u8() != 1) continue;  // a guest controls exactly one car
                uint8_t slot; uint32_t tick; netplay_car::State s;
                if (!get_car(r, slot, tick, s)) continue;
                const uint32_t life = d.completion_mode == EventCompletionMode::Battle ? r.u32() : 0;
                if (!r.ok || r.p != r.e || slot >= d.roster.size() ||
                    p.waiting_event == d.event_id || d.roster[slot].owner != p.id ||
                    (d.completion_mode == EventCompletionMode::Battle && life != d.battle.seats[slot].spawn_tick) || (p.has_latest && tick <= p.latest_tick)) continue;
                if (netplay_impact_fixture::observation.started && netplay_impact_fixture::pair_slot(p.id) < netplay_impact_fixture::collider_count() &&
                    tick <= netplay_impact_fixture::kStartTick) continue;
                p.activity.heard();
                p.latest = s; p.latest_tick = tick; p.latest_host_tick = d.tick; p.has_latest = true; p.latest_at = t;
                if (p.impact_through && p.impact_revision >= p.impact_through && ack == p.impact_revision) {
                    p.impact_through = 0;
                    d.remote[slot].reset();
                }
                if (!p.impact_through) {
                    d.remote[slot].push(tick, s);
                    // A wall or terrain wreck can reach the host in the owner's
                    // normal snapshots before its separate finish report.
                    if (d.completion_mode == EventCompletionMode::Derby && s.damage_stage >= 10)
                        d.completion.record(slot, EventFinishState::Totalled, t);
                }
            } else if (kind == K_VOICE) {
                if (d.voice.receive(sender, r.p, (size_t)(r.e - r.p)) == netplay::voice::Receive::Accepted) {
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
            if (kind == K_WELCOME) {
                if (d.welcomed || sender != 0) continue;
                const uint8_t assigned = (uint8_t)r.u8();
                uint8_t raw[16]; for (auto &b : raw) b = (uint8_t)r.u8();
                const auto content = r.str();
                if (!r.ok || r.p != r.e || assigned == 0 || assigned >= kMaxPlayers) continue;
                if (content != d.checked_content.value) {
                    d.fail_guest_join("Game content does not match the host. Install the same gameplay mods.");
                    return;
                }
                d.host_activity.heard();
                d.my_id = assigned;
                d.welcomed = true;
                if (d.relay.active()) {
                    d.relay.welcomed(d.fd);
                    d.route_text = GarageRelay::relayed(from) ? "Connected through the relay" : "Connected directly";
                    std::fprintf(stderr, "[netplay] Garage Code route: %s\n", d.route_text.c_str());
                }
                d.join_secret = netplay_online::hex(raw,16);
            }
            else if (kind == K_REFUSE) {
                if (d.welcomed || sender != 0) continue;
                uint32_t why = r.u8();
                if (!r.ok) continue;
                if (why == 4 && r.p != r.e) {
                    ContentFingerprint required;
                    if (get_required_content(r, required) && required.value != d.checked_content.value) {
                        d.content_mismatch(required);
                        return;
                    }
                }
                if (why == 1) {
                    std::string message = "The host has a different game version. ";
                    if (r.p != r.e) {
                        const auto protocol = r.u16();
                        const auto version = r.str();
                        if (r.ok && r.p == r.e && !version.empty() && version.size() <= 24 &&
                            version.find_first_not_of("0123456789.") == std::string::npos)
                            message += "Host runs " + version + " (online " + std::to_string(protocol) + "). ";
                    }
                    message += "You run " EOD_RELEASE_VERSION " (online " + std::to_string(kProtocol) +
                               "). Install the same build and retry.";
                    d.fail_guest_join(std::move(message));
                } else {
                    d.fail_guest_join(why == 2 ? "An event is running. You can join when it ends." : why == 4 ? "Game content does not match. Install the same gameplay mods as the host." : "The game is full.");
                }
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
                if (!d.welcomed || sender != 0) continue;
                const uint32_t stamp = r.u32();
                const uint32_t lobby_event_id = r.u32();
                const auto phase_flags = r.u8();
                const auto phase = phase_flags & 0x3f;
                std::string event = r.str();
                const uint32_t remote_ai = r.u8(); // host AI count (0=OFF, 1..15, 16=FILL TO MAX)
                const uint32_t count = r.u8();
                if (!r.ok || phase > P_DONE || remote_ai > 16u || count < 2 || count > kMaxPlayers) continue;
                std::vector<LobbyPlayer> players;
                players.reserve(count);
                bool seen[kMaxPlayers] = {};
                for (uint32_t i = 0; i < count; ++i) {
                    LobbyPlayer player;
                    player.id = (uint8_t)r.u8();
                    if (player.id >= kMaxPlayers || seen[player.id]) { r.ok = false; break; }
                    seen[player.id] = true;
                    player.name = r.str();
                    const unsigned flags = r.u8();
                    player.ready = flags & 1;
                    player.waiting = flags & 2;
                    player.vname = r.str();
                    player.ping = (uint16_t)r.u16();
                    if (!r.ok) break;
                    players.push_back(std::move(player));
                }
                if (!r.ok || !seen[0] || !seen[d.my_id]) continue;
                std::string track = r.str(); const uint8_t rules = r.u8(); const uint8_t capacity = r.u8();
                uint8_t tune[netplay_hooks::TuneCount]; for (uint8_t &q : tune) q = (uint8_t)r.u8();
                maps::Offer remote;
                uint8_t remote_preset = mutator::PRESET_STANDARD;
                if (r.p != r.e) {
                    remote.hash = r.str(); remote.size = r.u32(); remote.title = r.str();
                    if (r.p != r.e) remote_preset = static_cast<uint8_t>(r.u8());
                }
                if (!r.ok || r.p != r.e || !remote.valid() || remote_preset >= mutator::preset_count() || capacity < count || capacity > kMaxPlayers) continue;
                if (d.lobby_received && static_cast<int32_t>(stamp - d.last_host_ts) < 0) continue;
                if (!d.lobby_received || stamp != d.last_host_ts) d.host_activity.heard();
                if (remote.hash != d.maps.offer.hash) {
                    if (in_event_ || d.pending_start) continue;
                    d.guest_ready = false;
                    std::string error;
                    if (!d.maps.receive_offer(d.map_directory(), remote, error)) {
                        d.fail_guest_join("Host map unavailable: " + error);
                        return;
                    }
                    d.maps.last_progress = now_us();
                    d.last_hb = 0;
                }
                d.last_host_ts = stamp;
                d.host_clock.anchor(stamp, static_cast<uint32_t>(t / 1000));
                d.host_phase = (uint8_t)phase;
                if (!in_event_ && !d.pending_start && lobby_event_id >= d.event_id) {
                    if (phase_flags & 0x80) {
                        if (d.waiting_event != lobby_event_id)
                            std::fprintf(stderr, "[netplay] joined lobby for next event during event %u\n", lobby_event_id);
                        d.event_id = d.waiting_event = lobby_event_id;
                    } else if (!(phase_flags & 0x40)) {
                        // Once the host is back in its lobby, a delayed START
                        // for that event must never launch a race on this guest.
                        d.event_id = lobby_event_id;
                        d.waiting_event = 0;
                    }
                }
                d.lobby_capacity = capacity;
                d.lobby_players = std::move(players);
                d.lobby_event = std::move(event);
                d.lobby_track = std::move(track);
                d.lobby_gametype_preset = remote_preset;
                d.lobby_ai_count = static_cast<int>(remote_ai);
                if (!in_event_ && !d.pending_start) {
                    d.rules = rules;
                    for (int i = 0; i < netplay_hooks::TuneCount; ++i)
                        d.tuning[i] = std::clamp(tune[i], netplay_hooks::tune_ranges[i].lo, netplay_hooks::tune_ranges[i].hi);
                }
                if (!d.lobby_received) {
                    d.lobby_received = true;
                    d.status = "Connected";
                    d.ensure_defaults();
                    d.sync_voice();
                    std::fprintf(stderr, "[netplay] connected, player %u (via %s)\n", d.my_id, sockaddr_text(from).c_str());
                }
                if (!d.remembered && d.join_secret.size() == 32) {
                    d.remembered = true;
                    for (const LobbyPlayer &player : d.lobby_players)
                        if (player.id == 0) { d.remember(player.name, d.join_secret); break; }
                }
            } else if (kind == K_MAP_CHUNK) {
                if (!d.lobby_received || sender != 0 || d.maps.offer.hash == d.maps.verified) continue;
                const auto hash = r.str(); const auto index = r.u32();
                if (!r.ok || hash != d.maps.offer.hash) continue;
                std::string error;
                if (d.maps.download.accept(index, r.p, size_t(r.e - r.p), error)) {
                    d.maps.last_progress = now_us();
                    if (d.maps.download.complete() && d.maps.finish_download(error)) d.last_hb = 0;
                }
                if (!error.empty()) { d.fail_guest_join("Host map download failed: " + error); return; }
            } else if (kind == K_START) {
                if (!d.lobby_received) continue;
                uint32_t id = r.u32(); std::string ev = r.str(); uint32_t seed = r.u32(); std::string tr = r.str(); uint8_t rules = r.u8();
                uint8_t tune[netplay_hooks::TuneCount]; for (uint8_t &q : tune) q = (uint8_t)r.u8();
                const uint16_t participants = (uint16_t)r.u16();
                const auto event_map = r.p == r.e ? std::string() : r.str();
                const uint32_t has_mutator_byte = r.p == r.e ? 0u : r.u8();
                if (r.ok && r.p == r.e && has_mutator_byte <= 1u && event_map == d.maps.verified && event_map == d.maps.offer.hash && id > d.event_id && (participants & (1u << d.my_id))) {
                    if (!event_map.empty()) {
                        const auto map = eod::vfs::active_map();
                        auto *arena = map ? session_catalog::track_for_arena(map->base_arena) : nullptr;
                        if (!arena || arena->name != tr || arena->human_battle_contest.empty() ||
                            arena->human_battle_contest != ev) {
                            d.fail_guest_join("The host map and its Battle event do not match.");
                            return;
                        }
                    }
                    d.event_guest_mask = participants;
                    d.waiting_event = 0;
                    d.ensure_defaults();
                    for (int i = 0; i < netplay_hooks::TuneCount; ++i) d.tuning[i] = std::clamp(tune[i], netplay_hooks::tune_ranges[i].lo, netplay_hooks::tune_ranges[i].hi);
                    d.rules = rules; d.event_id = id; d.event_name = ev; d.pending_start = true; d.start_info = {ev, tr, id, seed, d.my_vehicle, false};
                    d.event_has_mutator = (has_mutator_byte != 0);
                    d.mutator_applied_event = 0;
                    if (!d.event_has_mutator) mutator::engine().clear_config();
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
            } else if (kind == K_MUTATOR_CONFIG) {
                const uint32_t id = r.u32();
                mutator::GametypeConfig cfg;
                if (r.ok && mutator::get_mutator_config(r, cfg) && r.p == r.e) {
                    if (id == 0 && !in_event_) {
                        // Pre-start lobby draft sync from Host -> Guest
                        d.has_host_mutator_draft = true;
                        Impl::populate_draft_from_config(d.host_mutator_draft, cfg, d.lobby_gametype_preset);
                    } else if (id == d.event_id && d.event_has_mutator) {
                        if (d.mutator_applied_event != id) {
                            mutator::engine().set_config(cfg);
                            d.mutator_applied_event = id;
                        }
                        Writer ack; ack.u32(id); d.send_to(d.host_addr, K_MUTATOR_CONFIG_ACK, d.my_id, ack);
                    }
                }
            } else if (kind == K_MUTATOR_ROUND) {
                const uint32_t id = r.u32();
                mutator::RoundPacket pkt;
                if (r.ok && id == d.event_id && in_event_ && d.bound && d.go &&
                    mutator::get_mutator_round(r, pkt) && r.p == r.e) {
                    const uint8_t prev_warp = mutator::engine().warp_gen();
                    if (mutator::engine().apply_remote_round(pkt, d.tick) && pkt.warp_gen != prev_warp) {
                        // Invalidate pre-warp impact checkpoints & puppet buffers
                        d.predicted_impact_at = 0;
                        d.impact_applied = d.impact_received;
                        for (auto &kv : d.remote) kv.second.reset();
                        d.shown.clear();
                    }
                    Writer ack; ack.u32(id); ack.u32(pkt.round_serial);
                    d.send_to(d.host_addr, K_MUTATOR_ROUND_ACK, d.my_id, ack);
                }
            } else if (kind == K_MUTATOR_TRIGGER) {
                const uint32_t id = r.u32();
                mutator::TriggerSyncPacket pkt;
                if (r.ok && id == d.event_id && in_event_ && d.bound && d.go &&
                    mutator::get_mutator_triggers(r, pkt) && r.p == r.e) {
                    mutator::engine().apply_remote_triggers(pkt);
                }
            } else if (kind == K_GO) {
                uint32_t id = r.u32();
                if (r.ok && id == d.event_id && d.applied_grid_event == id &&
                    (!d.event_has_mutator || d.mutator_applied_event == id)) d.go = true;
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
            } else if (kind == K_RESULT_CLOCK) {
                const uint32_t sent = r.u32(), host = r.u32();
                if (r.ok && r.p == r.e && d.last_clock_probe && sent == uint32_t(d.last_clock_probe / 1000))
                    d.host_clock.observe(sent, static_cast<uint32_t>(t / 1000), host);
            } else if (kind == K_FINISH_COUNTDOWN) {
                const uint32_t id = r.u32();
                const uint64_t remaining = r.u16() * 1000ull;
                if (!r.ok || r.p != r.e || id != d.event_id || !in_event_ || !d.bound || !d.go ||
                    d.abort_requested || d.result_event == id || d.completion_mode != EventCompletionMode::Race ||
                    remaining > EventCompletion::kCountdownUs) continue;
                const uint64_t deadline = t + remaining;
                if (!d.finish_countdown || deadline < d.finish_countdown) d.finish_countdown = deadline;
            } else if (kind == K_EVENT_RESULTS) {
                const uint32_t id = r.u32(), show_ms = r.u32();
                if (!r.ok || id != d.event_id || !in_event_ || !d.bound || !d.go ||
                    d.abort_requested || d.result_event == id) continue;
                std::vector<EventResultRow> rows;
                if (!get_event_results(r, rows)) continue;
                bool valid = true;
                size_t participants = 0;
                for (size_t i = 0; i < d.roster.size(); ++i) if (d.completion.participates(i)) ++participants;
                for (const auto &row : rows) {
                    valid &= d.completion.participates(row.slot);
                    if (row.state == EventFinishState::Finished) valid &= d.completion_mode == EventCompletionMode::Race;
                    if (row.state == EventFinishState::Survivor) valid &= d.completion_mode == EventCompletionMode::Derby;
                    if (row.state == EventFinishState::Completed) valid &= d.completion_mode == EventCompletionMode::Native;
                    if (row.state == EventFinishState::BattleScore) valid &= d.completion_mode == EventCompletionMode::Battle;
                    if (d.completion_mode == EventCompletionMode::Battle)
                        valid &= row.state == EventFinishState::BattleScore || row.state == EventFinishState::Withdrawn ||
                                 row.state == EventFinishState::Disconnected;
                }
                if (!valid || rows.size() != participants) continue;
                d.result_rows = std::move(rows);
                d.result_event = id;
                const uint32_t host_now = d.host_clock.host_ms(static_cast<uint32_t>(t / 1000));
                const int32_t wait_ms = static_cast<int32_t>(show_ms - host_now);
                // A late retry shares the host's return deadline. A guest that
                // missed the entire presentation still gets a brief results view.
                const int64_t display_left = std::clamp<int64_t>(int64_t(wait_ms) * 1000 + static_cast<int64_t>(kResultsDisplayUs),
                                                                 1000000, kResultsLeadUs + kResultsDisplayUs);
                d.result_until = t + display_left;
                d.finish_countdown = 0;
                std::fprintf(stderr, "[netplay] event %u standings received (%zu entries)\n", id, d.result_rows.size());
                trace_result_rows(d.result_rows);
            } else if (kind == K_BATTLE_STATE) {
                const uint32_t id = r.u32();
                BattleFrame frame;
                if (sender == 0 && id == d.event_id && in_event_ && d.bound && d.go &&
                    d.completion_mode == EventCompletionMode::Battle && !d.abort_requested && !d.result_until &&
                    get_battle_frame(r, d.roster.size(), frame) && frame.tick > d.received_battle.tick)
                    d.received_battle = frame;
            } else if (kind == K_CHICKEN || kind == K_CHICKEN_ACK) {
                if (sender == 0 && in_event_)
                    d.read_chicken(r, kind, d.host_chickens, d.host_addr, 0);
            } else if (kind == K_DAMAGE_DETAIL) {
                const uint32_t id = r.u32(), slot = r.u8(), revision = r.u32(), spawn = r.u32();
                if (!r.ok || sender != 0 || !in_event_ || !d.bound || !d.go || d.abort_requested || d.result_until ||
                    id != d.event_id || slot >= d.roster.size() || !revision || spawn >= revision) continue;
                auto &shape = d.bodywork[slot];
                if (revision < shape.revision) continue;
                if (revision == shape.applied) {
                    Writer ack; ack.u32(id); ack.u8(slot); ack.u32(revision);
                    d.send_to(d.host_addr, K_DAMAGE_DETAIL_ACK, d.my_id, ack);
                } else if (revision > shape.revision) {
                    auto detail = netplay_damage::unpack(r.p, r.e - r.p);
                    if (detail.empty()) continue;
                    shape.detail = std::move(detail);
                    shape.revision = revision;
                    shape.spawn = spawn;
                }
            } else if (kind == K_IMPACT_STATE) {
                const uint32_t id = r.u32();
                uint8_t slot; uint32_t revision; netplay_car::State state;
                if (!in_event_ || !d.bound || !d.go || d.abort_requested || d.result_until ||
                    id != d.event_id || !get_car(r, slot, revision, state) ||
                    slot != d.my_slot || revision <= d.impact_received) continue;
                const DamageFeedback feedback{r.u32(), r.f32(), r.f32()};
                const uint32_t life = d.completion_mode == EventCompletionMode::Battle ? r.u32() : 0;
                const uint32_t my_car = netplay_car::car_of_slot(slot);
                const bool stale_warp = my_car && state.warp_gen != mutator_hooks::warp_gen_for_car(my_car);
                if (sender != 0 || stale_warp || life != d.chicken_life(slot) || !r.ok || r.p != r.e || feedback.tick > revision ||
                    !(feedback.flash >= 0.f && feedback.flash <= .5f) ||
                    !(feedback.shake >= 0.f && feedback.shake <= .4f)) continue;
                d.impact_feedback = feedback;
                d.impact_received = revision;
                d.impact_state = state;
            } else if (kind == K_SNAP) {
                const uint32_t id = r.u32();
                if (!r.ok || id != d.event_id || !in_event_ || !d.bound || !d.go) continue;
                const uint32_t n_cars = r.u8();
                if (sender != 0 || !n_cars || n_cars > d.roster.size()) continue;
                bool valid = true;
                for (uint32_t i = 0; i < n_cars && r.ok; ++i) {
                    uint8_t slot; uint32_t tick; netplay_car::State s;
                    if (!get_car(r, slot, tick, s) || slot >= d.roster.size() || d.roster[slot].owner == d.my_id) {
                        valid = false;
                        break;
                    }
                    if (netplay_impact_fixture::observation.started && slot < d.roster.size() &&
                        netplay_impact_fixture::pair_slot(d.roster[slot].owner) < netplay_impact_fixture::collider_count() && tick <= netplay_impact_fixture::kStartTick) continue;
                    if (d.completion_mode != EventCompletionMode::Battle || tick > d.battle.seats[slot].spawn_tick)
                        d.remote[slot].push(tick, s);
                }
                if (valid && r.ok && r.p == r.e) d.host_activity.heard();
            } else if (kind == K_VOICE) {
                d.voice.receive(sender, r.p, (size_t)(r.e - r.p));
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
            if (p.alive && p.activity.expired()) {
                p.alive = false; d.status = p.name + " lost connection";
                std::fprintf(stderr, "[netplay] %s timed out\n", p.name.c_str());
                d.release_driver(p);
            }
        }
        if ((d.pending_start || in_event_) && !d.go && !d.abort_requested) {
            for (const auto &item : d.peers) {
                if ((d.event_guest_mask & (1u << item.first)) && !item.second.alive) {
                    submit({game_frontend::Command::Kind::EndEvent});
                    d.last_error = item.second.name + " left during loading. Start again with the remaining drivers.";
                    break;
                }
            }
        }
        // lobby state out
        const uint64_t period = in_event_ ? 500000 : 250000;
        if (t - d.last_lobby_send > period) {
            d.last_lobby_send = t;
            for (auto &kv : d.peers) {
                Peer &p = kv.second; if (!p.alive) continue;
                Writer w; p.last_ts_sent = now_ms() ? now_ms() : 1; d.put_lobby(w, p.last_ts_sent, in_event_ || d.pending_start,
                            (in_event_ || d.pending_start) && p.waiting_event == d.event_id);
                d.send_to(p.addr, K_LOBBY, 0, w);
            }
        }
        // event flow: repeat until each guest confirms
        if (d.event_id && d.pending_start + in_event_ + (d.my_phase != P_LOBBY) > 0) {
            for (auto &kv : d.peers) {
                Peer &p = kv.second; if (!p.alive || p.waiting_event == d.event_id) continue;
                if (!p.got_start || p.seen_event != d.event_id) {
                    Writer w; w.u32(d.event_id); w.str(d.event_name, 40); w.u32(d.start_info.seed); w.str(d.start_info.track, 40); w.u8(d.rules); for (uint8_t q : d.tuning) w.u8(q); w.u16(d.event_guest_mask); w.str(d.maps.offer.hash, 64); w.u8(d.event_has_mutator ? 1u : 0u); d.send_to(p.addr, K_START, 0, w);
                }
                if (d.event_has_mutator && d.mutator_acks[p.id] != d.event_id) {
                    Writer mw; mw.u32(d.event_id); mutator::put_mutator_config(mw, mutator::engine().config());
                    d.send_to(p.addr, K_MUTATOR_CONFIG, 0, mw);
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
        // Replicate host-authoritative micro-round transitions & role changes reliably (10 Hz until acked)
        if (in_event_ && d.bound && d.go && d.event_has_mutator && mutator::engine().has_round_packet() &&
            t - d.last_mutator_round_send >= 100000) {
            const uint32_t serial = mutator::engine().round_serial();
            bool any_pending = false;
            for (const auto &kv : d.peers) {
                const Peer &p = kv.second;
                if (p.alive && p.waiting_event != d.event_id && d.mutator_round_acks[p.id] < serial) {
                    any_pending = true; break;
                }
            }
            if (any_pending) {
                d.last_mutator_round_send = t;
                Writer rw; rw.u32(d.event_id); mutator::put_mutator_round(rw, mutator::engine().latest_round_packet());
                for (const auto &kv : d.peers) {
                    const Peer &p = kv.second;
                    if (p.alive && p.waiting_event != d.event_id && d.mutator_round_acks[p.id] < serial) {
                        d.send_to(p.addr, K_MUTATOR_ROUND, 0, rw);
                    }
                }
            }
        }
        // Replicate live trigger zones, shrinking storm radius, flag carrier & chain tethers (10 Hz)
        if (in_event_ && d.bound && d.go && d.event_has_mutator &&
            mutator::engine().has_live_sync_state() && t - d.last_mutator_trigger_send >= 100000) {
            d.last_mutator_trigger_send = t;
            Writer tw; tw.u32(d.event_id);
            mutator::put_mutator_triggers(tw, mutator::engine().build_trigger_packet(d.tick));
            for (const auto &kv : d.peers) {
                const Peer &p = kv.second;
                if (p.alive && p.waiting_event != d.event_id) {
                    d.send_to(p.addr, K_MUTATOR_TRIGGER, 0, tw);
                }
            }
        }
        // Results survive teardown and are resent until each peer returns to the lobby.
        if (d.event_id && d.result_event == d.event_id && t - d.last_outcome_send >= 100000) {
            d.last_outcome_send = t;
            Writer w; w.u32(d.event_id); w.u32(d.result_show_host_ms); put_event_results(w, d.result_rows);
            for (const auto &kv : d.peers) {
                const Peer &p = kv.second;
                if (p.alive && p.seen_event == d.event_id && p.phase != P_LOBBY)
                    d.send_to(p.addr, K_EVENT_RESULTS, 0, w);
            }
        } else if (in_event_ && d.bound && !d.abort_requested && d.finish_countdown &&
                   t - d.last_outcome_send >= 100000) {
            d.last_outcome_send = t;
            const uint64_t ms = t < d.finish_countdown ? (d.finish_countdown - t) / 1000 : 0;
            Writer w; w.u32(d.event_id); w.u16(ms);
            for (const auto &kv : d.peers) if (kv.second.alive) d.send_to(kv.second.addr, K_FINISH_COUNTDOWN, 0, w);
        }
        // Manual exits and other event modes keep the original lobby acknowledgement.
        if (!in_event_ && !d.pending_start && d.event_id && d.result_event != d.event_id) {
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
        if (d.lobby_received && d.maps.offer.hash != d.maps.verified) {
            if (t > d.maps.last_progress && t - d.maps.last_progress > 30000000) {
                d.fail_guest_join("Host map transfer stalled. Rejoin to try again.");
                return;
            }
            const auto window = d.maps.download.window();
            if (t >= d.maps.next_request || window.first >= d.maps.last_window + maps::window_chunks) {
                Writer request; request.str(d.maps.offer.hash, 64); request.u32(window.first); request.u32(window.missing);
                d.send_to(d.host_addr, K_MAP_REQUEST, d.my_id, request);
                d.maps.last_window = window.first;
                d.maps.next_request = t + 150000;
            }
        }
        if (!d.lobby_received && d.relay.ready() && !d.targets.empty() && !GarageRelay::relayed(d.targets.front()) &&
            d.handshake_started && t - d.handshake_started >= 2500000) {
            d.targets.assign(1, d.relay.peer(1));
            d.host_addr = {};
            d.welcomed = false;
            d.handshake_started = t;
            d.last_join = 0;
        }
        if (!d.lobby_received && d.welcomed) {
            const uint64_t deadline = d.joining_online
                ? std::min(d.handshake_started + kOnlineHandshakeTimeoutUs, d.join_started + (d.relay.active() ? kServiceAdmissionUs : kOnlineDiscoveryTimeoutUs))
                : d.join_started + kDirectJoinTimeoutUs;
            if (t >= deadline) {
                d.fail_guest_join("The host responded, but the lobby details did not arrive. Try again.");
            } else if (t - d.last_join > 400000) {
                // Reassert the admitted seat without sending a Ready heartbeat.
                d.last_join = t;
                Writer w;
                w.u16(kProtocol); w.str(d.name, 16); w.u32(d.join_tag); w.str(d.checked_content.value, 16);
                if (d.relay.active()) w.str(d.join_attempt, 32);
                d.send_to(d.host_addr, K_JOIN, 0, w);
            }
            return;
        }
        if (!d.welcomed && d.joining_online && d.relay.active()) {
            if (d.targets.empty() && d.relay.choose_relay(t)) d.targets.push_back(d.relay.peer(1));
            if (!d.targets.empty()) {
                if (!d.handshake_started) d.handshake_started = t;
                d.join_note = "Room found. Joining the lobby...";
                if (t - d.last_join > 400000) {
                    d.last_join = t;
                    Writer w;
                    w.u16(kProtocol); w.str(d.name, 16); w.u32(d.join_tag); w.str(d.checked_content.value, 16);
                    w.str(d.join_attempt, 32);
                    d.send_to(d.targets.front(), K_JOIN, 0, w);
                }
            }
            if (d.relay.error() == 'F' || d.relay.error() == 'U' ||
                (d.relay.error() == 'N' && t - d.join_started > 4000000) ||
                t - d.join_started >= kServiceAdmissionUs) {
                d.fail_guest_join(d.relay.error() == 'N' ? "Room not found. Check the code or ask your friend to host." :
                                  d.relay.error() == 'F' ? "The room service is full. Try again shortly." :
                                  "The room did not respond. Check your connection and try again.");
            }
            return;
        }
        if (!d.welcomed && d.joining_online) {
            // Garage Code / friend: JOIN goes to every address the rendezvous found for the host (hole punching: both sides send)
            const netplay_online::Report rep = d.online.report();
            d.route_text = d.route_description(rep);
            std::vector<sockaddr_in> cand;
            for (auto &a : d.online.found("hosts")) cand.push_back(Impl::to_sockaddr(a));
            if (!cand.empty() && cand.size() != d.targets.size()) std::fprintf(stderr, "[netplay] found %zu candidate address(es) for the host\n", cand.size());
            d.targets = cand;
            if (!d.targets.empty() && !d.handshake_started) d.handshake_started = t;
            if (t - d.last_join > 400000) {
                d.last_join = t;
                Writer w; w.u16(kProtocol); w.str(d.name, 16); w.u32(d.join_tag); w.str(d.checked_content.value,16);
                for (auto &a : d.targets) d.send_to(a, K_JOIN, 0, w);
            }
            const std::string what = d.code.empty() ? "your friend's game" : "game " + d.code;
            d.join_note = !rep.stun_done ? "Looking for the best route..." : d.targets.empty() ? "Looking for " + what + "..." : "Found " + what + ", connecting...";
            const uint64_t elapsed = t - d.join_started;
            const bool handshake_timed_out = d.handshake_started &&
                (t - d.handshake_started >= kOnlineHandshakeTimeoutUs || elapsed >= kOnlineDiscoveryTimeoutUs);
            const bool discovery_timed_out = !d.handshake_started && (elapsed >= kOnlineDiscoveryTimeoutUs ||
                (elapsed >= 12000000 && rep.stun_done && !rep.internet()));
            if (handshake_timed_out || discovery_timed_out) {
                std::string message;
                if (handshake_timed_out) {
                    message = rep.symmetric ? "Lobby found, but this network could not connect directly. Try again from a different network."
                                            : "Lobby found, but the host did not respond. Try again.";
                } else if (!rep.internet()) {
                    message = "Could not reach the internet. Check your connection, then try again.";
                } else {
                    message = d.code.empty() ? "Your friend is not hosting right now. Ask them to host, then try again."
                                             : "Lobby not found. Check the Garage Code, then try again.";
                }
                std::fprintf(stderr, "[netplay] join failed: %s\n%s", message.c_str(), d.online.log_tail().c_str());
                d.fail_guest_join(std::move(message));
            }
            return;
        }
        if (!d.welcomed) {
            if (t - d.last_join > 400000) { d.last_join = t; Writer w; w.u16(kProtocol); w.str(d.name, 16); w.u32(0); w.str(d.checked_content.value,16); d.send_to(d.host_addr, K_JOIN, 0, w); }
            if (t - d.join_started >= kDirectJoinTimeoutUs)
                d.fail_guest_join("Could not reach the host. Check the address and that both of you are on the same network.");
            return;
        }
        if (in_event_ && d.bound && d.go && !d.abort_requested && d.result_event != d.event_id &&
            d.local_finish != EventFinishState::Active &&
            !(d.local_finish == EventFinishState::Totalled && d.predicted_impact_at) &&
            t - d.last_outcome_send >= 100000) {
            d.last_outcome_send = t;
            Writer w; w.u32(d.event_id); w.u8(static_cast<uint8_t>(d.local_finish)); w.u32(d.local_terminal_ms);
            d.send_to(d.host_addr, K_FINISH_STATE, d.my_id, w);
        }
        if (d.lobby_received && t - d.last_clock_probe >= 1000000) {
            d.last_clock_probe = t;
            Writer w; w.u32(static_cast<uint32_t>(t / 1000));
            d.send_to(d.host_addr, K_RESULT_CLOCK, d.my_id, w);
        }
        const uint64_t period = (in_event_ || d.my_phase != P_LOBBY) ? 100000 : 250000;
        if (t - d.last_hb > period) {
            d.last_hb = t;
            Writer w; w.str(d.name, 16); w.u8(d.guest_ready); w.u32(d.my_vehicle); w.str(d.my_vname, 48); w.u8(d.my_phase); w.u32(d.event_id);
            const bool at_menu = !in_event_ && !d.pending_start && d.in_menu;
            w.u8((at_menu ? 1 : 0) | (d.have_track ? 2 : 0) | (d.have_roster ? 4 : 0) | (d.go ? 8 : 0) | ((d.event_id && (d.pending_start || in_event_ || d.my_phase != P_LOBBY)) ? 16 : 0) | (d.car_done ? 32 : 0) | ((d.waiting_event && d.waiting_event == d.event_id) ? 64 : 0));
            w.u32(d.last_host_ts);
            w.u32(++d.heartbeat_serial);
            w.str(d.maps.verified, 64);
            d.send_to(d.host_addr, K_HEARTBEAT, d.my_id, w);
        }
        if (d.host_activity.expired()) {
            d.last_error = "Lost connection to the host.";
            d.retryable_loss = true;
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
        if (d_->role == Impl::Role::None || d_->abort_requested) break;
        if (done()) { ok = true; break; }
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
    if (d.in_menu && !session_catalog::ready()) { session_catalog::refresh(); d.ensure_defaults(); }
    // A fast rematch may be requested while the native menu is still opening.
    // Pulse Confirm until the main selection actually exits its input loop.
    if (!netplay_hooks::main_selection_active) return 0;
    // Unified PLAY Hub offline/Career launch while native Main Menu selection loop is already running:
    // pulse Confirm so f_00270C10_orig() exits and caller 0x002A16D8 dispatches the armed transition.
    if (session_transition_pending()) {
        const uint32_t c = d.inject_count++;
        return ((c % 6) < 2) ? 0x4000 : 0;
    }
    if ((!d.pending_start && !in_event_) || d.abort_requested || d.role == Impl::Role::None) return 0;
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
    d.completion_mode = EventCompletionMode::Native;
    d.completion.reset(d.completion_mode, 0);
    d.local_finish = EventFinishState::Active;
    d.local_terminal_ms = d.result_show_host_ms = 0;
    d.finish_countdown = d.result_until = d.last_outcome_send = 0;
    d.result_event = 0;
    d.mode_deadline = 0;
    d.result_rows.clear();
    d.name_geometry_ready = false;
    d.name_camera = {};
    names::on_world_view = nullptr;
    if (names::enabled(d.event_name, d.rules) || d.event_has_mutator) {
        names::on_world_view = [] {
            const names::Camera cam = names::read_world_camera();
            session().d_->name_camera = cam;
            auto &wc = mutator_hooks::world_camera;
            for (int i = 0; i < 3; ++i) {
                wc.position[i] = cam.position[i];
                for (int j = 0; j < 3; ++j) wc.axes[i][j] = cam.axes[i][j];
            }
            wc.horizontal_scale = cam.horizontal_scale;
            wc.vertical_scale = cam.vertical_scale;
            wc.width = cam.width;
            wc.height = cam.height;
            wc.valid = cam.width > 0.f && cam.height > 0.f;
        };
    }
    d.tag_sight = {};
    d.my_phase = P_STARTED; d.tick = 0; d.bound = false; d.my_slot = -1; d.remote.clear(); d.puppet_cars.clear(); d.roster.clear(); d.have_roster = false;
    d.have_track = d.role == Impl::Role::Host ? false : false; d.strip.clear();
    std::fprintf(stderr, "[netplay] event %u '%s' starting (%s) rules=%u tuning=%u,%u,%u,%u\n", out.event_id, out.event.c_str(), out.host ? "host" : "guest",
                 d.rules, d.tuning[0], d.tuning[1], d.tuning[2], d.tuning[3]);
    return true;
}
void Session::car_chosen(uint32_t vehicle, const std::string &) {
    Impl &d = *d_;
    if (!session_catalog::online_vehicle_is_drivable(vehicle)) {
        error_ = d.last_error = "The selected vehicle cannot be driven in this online event. Choose a drivable car.";
        d.my_vehicle = 0;
        d.my_vname.clear();
        d.car_done = true;
        d.my_phase = P_CAR_DONE;
        return;
    }
    d.my_vehicle = vehicle;
    const std::string model = vehicle_name(vehicle);
    const auto paints = session_catalog::paints(model);
    const auto paint = std::find(paints.begin(), paints.end(), vehicle);
    d.my_vname = paint == paints.end() ? model : vehicle_label(model, paint - paints.begin(), paints.size());
    d.car_done = true; d.my_phase = P_CAR_DONE;
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
std::vector<names::Tag> Session::driver_tags() {
    Impl &d = *d_;
    std::vector<names::Tag> tags;
    if (!in_event_ || !d.bound || !d.go || d.abort_requested || d.result_until ||
        !names::enabled(d.event_name, d.rules)) return tags;
    static const bool trace = std::getenv("EOD_NETPLAY_TEST_NAMES") != nullptr;
    if (!d.name_geometry_ready) {
        d.name_occluders.load();
        d.name_geometry_ready = true;
        if (trace) std::fprintf(stderr, "[netplay-names] event=%u terrain=%zu boxes=%zu tubes=%zu\n", d.event_id,
                                d.name_occluders.terrain_count(), d.name_occluders.box_count(), d.name_occluders.tube_count());
    }
    const auto &camera = d.name_camera;
    static uint32_t traced_tick = 0;
    const bool sample = trace && (d.tick < traced_tick || d.tick - traced_tick >= 60);
    if (sample) traced_tick = d.tick;
    const uint8_t local_owner = d.role == Impl::Role::Host ? 0 : d.my_id;
    for (size_t slot = 0; slot < d.roster.size(); ++slot) {
        const uint8_t owner = d.roster[slot].owner;
        if (owner == 0xFF || owner == local_owner) continue;
        const std::string *name = nullptr;
        if (d.role == Impl::Role::Host) {
            const auto peer = d.peers.find(owner);
            if (peer != d.peers.end() && peer->second.alive && peer->second.waiting_event != d.event_id)
                name = &peer->second.name;
        } else {
            for (const auto &driver : d.lobby_players)
                if (driver.id == owner && !driver.waiting) { name = &driver.name; break; }
        }
        const auto car = netplay_car::car_of_slot(static_cast<uint32_t>(slot));
        if (!name || !car) continue;
        const auto anchor = names::roof_anchor(car);
        names::Tag tag{};
        const bool on_screen = names::project(camera, anchor, tag);

        auto &sight = d.tag_sight[slot];
        // Stagger sightline checks across four simulation ticks while reprojecting every frame.
        if (on_screen && (!sight.known || d.tick >= sight.next_tick)) {
            const auto start = trace ? now_us() : 0;
            sight.clear = !d.name_occluders.blocked(camera.position, anchor);
            sight.known = true;
            sight.next_tick = d.tick + 1 + ((slot - d.tick - 1) & 3);
            if (trace && sample) std::fprintf(stderr, "[netplay-names] sightline_us=%llu\n",
                                               (unsigned long long)(now_us() - start));
        }
        if (sample)
            std::fprintf(stderr, "[netplay-names] event=%u tick=%u owner=%u name=%s screen=%d visible=%d x=%.3f y=%.3f distance=%.2f camera=%.2f,%.2f,%.2f anchor=%.2f,%.2f,%.2f\n",
                         d.event_id, d.tick, owner, name->c_str(), on_screen, on_screen && sight.clear, tag.x, tag.y, tag.distance,
                         camera.position[0], camera.position[1], camera.position[2], anchor[0], anchor[1], anchor[2]);
        if (on_screen && sight.clear) {
            tag.name = *name;
            tags.push_back(std::move(tag));
        }
    }
    return tags;
}

std::string Session::strip() const {
    if (in_event_ && d_->abort_requested)
        return d_->role == Impl::Role::None ? "Leaving the event..." : "Returning to the lobby...";
    if (!d_->strip.empty()) return d_->strip;
    if (!in_event_ || event_results()) return {};
    const bool totalled = d_->local_finish == EventFinishState::Totalled;
    const bool finished = d_->local_finish == EventFinishState::Finished || d_->local_finish == EventFinishState::Completed;
    if (!d_->finish_countdown) {
        if (totalled) return "Totalled. Waiting for event results...";
        return finished ? "Finished. Waiting for event results..." : std::string{};
    }
    const auto now = now_us();
    const auto seconds = now < d_->finish_countdown ? (d_->finish_countdown - now + 999999) / 1000000 : 0;
    return std::string(totalled ? "Totalled. Race ends in 0:" : finished ? "Finished. Race ends in 0:" : "Finish the race in 0:") +
        (seconds < 10 ? "0" : "") + std::to_string(seconds);
}

const std::vector<EventResultRow> *Session::event_results() const {
    return in_event_ && !d_->abort_requested && d_->result_until &&
           now_us() + kResultsDisplayUs >= d_->result_until ? &d_->result_rows : nullptr;
}
// The lobby's rules apply to the online event only; a lobby that is still waiting leaves the player's own settings in force.
int Session::online_rules() const { return in_event_ ? d_->rules : -1; }
const uint8_t *Session::online_tuning() const { return in_event_ ? d_->tuning : nullptr; }
bool Session::active() const { return in_event_ || d_->role != Impl::Role::None; }
bool Session::is_host() const { return d_->role == Impl::Role::Host; }
uint32_t Session::remote_humans() const { return in_event_ ? std::bitset<16>(d_->event_guest_mask).count() : 0; }

void Session::mix_voice(int16_t *stereo, int frames) {
    Impl &d = *d_;
    std::lock_guard<std::mutex> lk(d.voice_mtx);
    const size_t count = std::min<size_t>(frames, d.voice_out.size() - d.voice_pos);
    if (const char *capture = std::getenv("EOD_NETPLAY_TEST_VOICE_PCM")) {
        static uint64_t samples_written = 0;
        if (count && samples_written < 48000 * 15) {
            FILE *f = std::fopen(capture, samples_written ? "ab" : "wb");
            if (f) {
                const size_t n = std::min<size_t>((size_t)frames, d.voice_out.size() - d.voice_pos);
                std::fwrite(d.voice_out.data()+d.voice_pos,sizeof(int16_t),n,f);
                samples_written += n;
                std::fclose(f);
            }
        }
    }
    d.voice_mixer.mix(stereo, frames, count ? d.voice_out.data() + d.voice_pos : nullptr,
                      count, g_config.volume, g_config.voice_volume, g_config.lower_game_during_voice);
    d.voice_pos += count;
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
        if (!session_catalog::online_vehicle_is_drivable(d.my_vehicle)) {
            error_ = d.last_error = "Choose a drivable car before starting.";
            d.abort_requested = true; session_abort_event(); return;
        }
        d.my_phase = P_CAR_DONE;
        // everyone needs to have picked a car before the grid is decided
        if (!wait_until([&] { for (auto &kv : d.peers) if (kv.second.alive && kv.second.waiting_event != d.event_id && !(kv.second.car_done && kv.second.seen_event == d.event_id)) return false; return true; },
                   300000, "Waiting for everyone to choose a car")) {
            if (d.abort_requested) return;
            error_ = d.last_error = "Not everyone finished choosing a car.";
            d.abort_requested = true; session_abort_event(); return;
        }
        if (d.abort_requested) return;
        if (count < 1 + remote_humans()) {
            error_ = d.last_error = "This event has too few starting seats for everyone. Choose a larger event.";
            d.abort_requested = true; session_abort_event(); return;
        }
        // Reserve exactly one host seat, by native pad-zero identity. Duplicate
        // vehicle selections are valid and must not consume additional seats.
        uint32_t host_slot = count;
        const bool detention = ee_r32(description + 0x9A4) == kDetention;
        for (uint32_t i = 0; i < count; ++i) {
            if (ee_r32(rec(i) + 0x68) == d.my_vehicle &&
                (!detention || vehicle_name(ee_r32(rec(i) + 0x6C)) == "Bus")) {
                host_slot = i;
                break;
            }
        }
        if (host_slot == count) {
            error_ = d.last_error = "The host's selected car is missing from the starting grid.";
            d.abort_requested = true; session_abort_event(); return;
        }
        d.roster.assign(count, RosterSlot{});
        d.roster[host_slot].owner = 0;
        uint32_t seat = 0;
        for (auto &kv : d.peers) {
            Peer &p = kv.second; if (!p.alive || p.waiting_event == d.event_id) continue;
            while (seat < count && d.roster[seat].owner != 0xFF) ++seat;
            if (seat == count) { d.abort_requested = true; session_abort_event(); return; }
            uint32_t veh = p.vehicle;
            if (!session_catalog::online_vehicle_is_drivable(veh)) {
                d.content_note = p.name + " must select a drivable car installed on the host.";
                error_ = d.last_error = d.content_note; d.abort_requested = true; session_abort_event(); return;
            }
            ee_w32(rec(seat) + 0x68, veh); ee_w32(rec(seat) + 0x6C, veh);
            d.roster[seat].owner = p.id;
        }
    }
    if (d.role == Impl::Role::Guest) {
        d.my_phase = P_CAR_DONE;
        if (!wait_until([&] { return d.have_roster; }, 120000, "Waiting for the host to set the grid")) {
            if (d.abort_requested) return;
            error_ = d.last_error = "The host did not send the grid.";
            d.abort_requested = true; session_abort_event(); return;
        }
        if (d.roster.size() != count) { error_ = d.last_error = "The grid size does not match (different mods?)"; d.abort_requested = true; session_abort_event(); std::fprintf(stderr, "[netplay] roster size %zu != %u\n", d.roster.size(), count); return; }
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t drv = d.roster[i].driver, veh = d.roster[i].vehicle;
            if (d.roster[i].owner == 0xFF ? !vehicle_known(veh) :
                !session_catalog::online_vehicle_is_drivable(veh)) {
                d.content_note = "A grid vehicle is missing or a driver chose a non-drivable vehicle. Check the selected cars and installed mods.";
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
    // 1116E0 appends one 0x56 trailer per driver for event types with the
    // trailer flag. These rows keep their native type and hitch partner.
    const uint32_t drivers = static_cast<uint32_t>(d.roster.size());
    if (d.event_name == session_catalog::kHumanBattle &&
        (drivers != 2 || d.roster[0].owner == d.roster[1].owner ||
         d.roster[0].owner == 0xFF || d.roster[1].owner == 0xFF)) {
        error_ = d.last_error = "Both drivers must be connected to start a 1v1 Battle.";
        d.abort_requested = true; session_abort_event(); return;
    }
    bool trailer_grid = drivers && count == 2 * drivers && (ee_r32(description + 0x9A4) & 0x800);
    for (uint32_t i = drivers; trailer_grid && i < count; ++i) {
        trailer_grid = ee_r32(rec(i)) == 0x56;
    }
    if (drivers != count && !trailer_grid) {
        error_ = d.last_error = "The event changed the starting grid size. Choose another event.";
        d.abort_requested = true; session_abort_event(); return;
    }
    const uint8_t local_id = d.role == Impl::Role::Host ? 0 : d.my_id;
    d.my_slot = -1;
    netplay_hooks::detention_bus_slot = -1;
    for (uint32_t i = 0; i < drivers; ++i) {
        if (d.role == Impl::Role::Host) {
            d.roster[i].driver = ee_r32(rec(i) + 0x68);
            d.roster[i].vehicle = ee_r32(rec(i) + 0x6C);
        }
        const bool mine = d.roster[i].owner == local_id;
        if (ee_r32(description + 0x9A4) == kDetention && d.roster[i].owner == 0)
            netplay_hooks::detention_bus_slot = static_cast<int>(i);
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
    if (trailer_grid) {
        std::fprintf(stderr, "[netplay] native trailer grid drivers=%u trailers=%u\n", drivers, count - drivers);
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
    names::on_world_view = nullptr;
    Impl &d = *d_;
    netplay_hooks::detention_bus_slot = -1;
    mutator::engine().reset_match();
    if (!in_event_) return;
    in_event_ = false;
    d.abort_requested = false;
    d.finish_countdown = d.result_until = 0;
    if (d.native_lap_driver) {
        ee_w32(0x003CE55C, d.saved_auto_driver);
        d.native_lap_driver = false;
    }
    for (auto &kv : d.puppet_cars) netplay_car::set_puppet(kv.second, false);
    d.puppet_cars.clear(); d.remote.clear(); d.shown.clear(); d.bound = false; d.my_phase = P_DONE;
    if (d.role == Impl::Role::Host && d.result_event != d.event_id) {
        Writer w; w.u32(d.event_id);
        for (auto &kv : d.peers) d.send_to(kv.second.addr, K_END, 0, w);
    }
    d.my_phase = P_LOBBY; d.go = false; d.strip.clear();
    d.status = "Back in the lobby";
    d.guest_ready = false;
    std::fprintf(stderr, "[netplay] event ended\n");
    overlay_show_online();
}

void Session::race_frame() {
    Impl &d = *d_;
    if (d.bound && d.tick == 1 && std::getenv("EOD_NETPLAY_TEST_TERMINAL"))
        std::fprintf(stderr, "[netplay-test] frame gate type=%08x mode=%u quit=%u\n",
                     ee_r32(kNativeEventType), static_cast<unsigned>(d.completion_mode), ee_r32(kNativeRaceQuit));
    if (!in_event_ || !d.bound || !d.go || d.abort_requested || d.role == Impl::Role::None ||
        ee_r32(kNativeRaceQuit)) return;
    const uint64_t now = now_us();
    if (d.result_until && now >= d.result_until) {
        d.abort_requested = true;
        session_abort_event();
        return;
    }

    if (!d.result_until && d.completion_mode == EventCompletionMode::Battle) battle_frame(now);
    if (!d.result_until) test_terminal_records(d.event_id, d.tick, d.my_slot, d.roster, d.role == Impl::Role::Host);
    const bool native_end = ee_r32(kNativeRaceState) == 3;
    if (d.completion_mode != EventCompletionMode::Battle && d.local_finish == EventFinishState::Active &&
        (d.completion_mode != EventCompletionMode::Native || native_end)) {
        d.local_finish = native_outcome(d.my_slot, d.completion_mode);
        // Respawning modes can temporarily wreck an otherwise active player.
        // A special-mode vote requires the local controller's actual end flag.
        if (d.completion_mode == EventCompletionMode::Native && d.local_finish == EventFinishState::Active)
            d.local_finish = EventFinishState::Completed;
        if (d.local_finish != EventFinishState::Active) {
            if (d.native_lap_driver && d.local_finish == EventFinishState::Finished) {
                const auto driver = ee_r32(0x00485230 + 4 * d.my_slot);
                std::fprintf(stderr, "[netplay-test] native crossing event=%u slot=%d tick=%u lap=%u target=%u finished=%u\n",
                             d.event_id, d.my_slot, d.tick, ee_r32(driver + 0x40), ee_r32(0x0048501C), ee_r32(driver + 0x34));
            }
            const auto clock_ms = static_cast<uint32_t>(now / 1000);
            d.local_terminal_ms = d.role == Impl::Role::Host ? clock_ms : d.host_clock.host_ms(clock_ms);
        }
    }

    if (d.role == Impl::Role::Host && !d.result_until) {
        d.completion.record(d.my_slot, d.local_finish, now);
        for (size_t i = 0; i < d.roster.size(); ++i) {
            if (!d.completion.participates(i)) continue;
            const auto owner = d.roster[i].owner;
            if (owner == 0xFF) {
                d.completion.record(i, native_outcome(i, d.completion_mode), now);
            } else if (owner != 0) {
                const auto peer = d.peers.find(owner);
                if (peer == d.peers.end() || !peer->second.alive)
                    d.completion.record(i, EventFinishState::Disconnected, now);
                else if (peer->second.waiting_event == d.event_id)
                    d.completion.record(i, EventFinishState::Withdrawn, now);
                else if (d.completion_mode == EventCompletionMode::Derby && peer->second.impact_through &&
                         native_outcome(i, d.completion_mode) == EventFinishState::Totalled)
                    d.completion.record(i, EventFinishState::Totalled, now);
            }
        }
        // Native EventUpdate stores a finite event deadline, then replaces it
        // with a NaN sentinel when it expires (0x001B5EE4).
        bool mode_timeout = false;
        if (d.completion_mode != EventCompletionMode::Race) {
            const float deadline = native_float(kNativeEventDeadline);
            if (std::isfinite(deadline) && deadline > 0) d.mode_deadline = deadline;
            const float clock = native_float(kNativeRaceClock);
            mode_timeout = d.mode_deadline > 0 && clock >= d.mode_deadline;
        }
        // A wrecked host's local end vote never stops surviving guests. A
        // completed host objective, a real mode timer or all terminal drivers
        // can close the round. Special modes keep their native scoring rules.
        BattleStandings battle;
        const bool battle_finished = d.completion_mode == EventCompletionMode::Battle &&
                                     read_battle_standings(d.roster.size(), eligible_battle_slots(), battle);
        if (d.completion.update(now, mode_timeout, battle_finished || d.local_finish == EventFinishState::Completed)) {
            for (size_t i = 0; i < d.roster.size(); ++i) {
                if (!d.completion.participates(i)) continue;
                const auto owner = d.roster[i].owner;
                const auto peer = d.peers.find(owner);
                std::string name = owner == 0 ? d.name :
                    owner == 0xFF ? "AI Driver " + std::to_string(i + 1) :
                    peer != d.peers.end() ? peer->second.name : "Driver " + std::to_string(owner + 1);
                d.result_rows.push_back({static_cast<uint8_t>(i), d.completion.state(i),
                                         d.completion.rank(i), std::move(name)});
                if (d.completion.state(i) == EventFinishState::BattleScore) {
                    d.result_rows.back().rank = battle.place[i];
                    d.result_rows.back().score = battle.points[i];
                }
            }
            std::stable_sort(d.result_rows.begin(), d.result_rows.end(), [](const auto &left, const auto &right) {
                const auto order = [](uint8_t rank) { return rank ? rank : EventCompletion::kSlots + 1; };
                return order(left.rank) < order(right.rank);
            });
            d.finish_countdown = 0;
            d.result_event = d.event_id;
            const uint64_t show_at = now + kResultsLeadUs;
            d.result_show_host_ms = static_cast<uint32_t>(show_at / 1000);
            d.result_until = show_at + kResultsDisplayUs;
            d.last_outcome_send = 0;
            std::fprintf(stderr, "[netplay] event %u standings committed (%zu entries) ms=%u\n",
                         d.event_id, d.result_rows.size(), static_cast<uint32_t>(now / 1000));
            trace_result_rows(d.result_rows);
        } else {
            d.finish_countdown = d.completion.deadline();
        }
    }

    // RaceFrameLoop tests state 3 immediately after this frame. Each peer's
    // native controller knows only its local player, so its vote cannot end
    // the shared event. The online results timer requests the normal unwind.
    if (ee_r32(kNativeRaceState) == 3) ee_w32(kNativeRaceState, 1);
}

uint16_t Session::eligible_battle_slots() const {
    if (!in_event_ || !d_->bound || d_->completion_mode != EventCompletionMode::Battle) return UINT16_MAX;
    if (d_->role != Impl::Role::Host) return 0;
    uint16_t eligible = 0;
    for (size_t i = 0; i < d_->roster.size(); ++i) {
        const uint8_t owner = d_->roster[i].owner;
        const auto peer = d_->peers.find(owner);
        if (owner == 0 || owner == 0xFF ||
            (peer != d_->peers.end() && peer->second.alive && peer->second.waiting_event != d_->event_id))
            eligible |= static_cast<uint16_t>(1u << i);
    }
    return eligible;
}

bool Session::remote_driver_car(uint32_t car) const {
    if (!in_event_ || !car || (car & 3u) || car > EE_RAM_SIZE - 0x1C34 ||
        ee_r32(car + netplay_car::kType) != 0x51) return false;
    // The constructor copies the race description to car+0x1E0 before damage
    // setup. roster_post stamped +0x72 with the common grid seat.
    const auto slot = ee_r8(car + 0x252);
    return slot < d_->roster.size() && d_->roster[slot].owner != 0xFF &&
        d_->roster[slot].vehicle == ee_r32(car + 0x24C) && (!d_->bound || netplay_car::is_puppet(car));
}

// Native humans reach this request through the SELECT input path. The host
// alone accepts a new life and runs the track's native spawn rotation.
bool Session::battle_respawn(uint32_t master, uint32_t slot) {
    Impl &d = *d_;
    if (!in_event_ || d.completion_mode != EventCompletionMode::Battle || !d.bound) return false;
    if (!d.go || d.abort_requested || d.result_until || !master || master != ee_r32(0x003CF194) ||
        master > EE_RAM_SIZE - battle_layout::kMasterSize || ee_r32(master) != battle_layout::kMasterVtable ||
        ee_r32(master + 4) != 1 || slot >= d.roster.size() || slot >= battle_layout::kCapacity) return true;
    const uint32_t record = master + battle_layout::kParticipants + slot * battle_layout::kParticipantStride;
    if (!ee_r32(record + 4)) return true;
    if (d.role == Impl::Role::Guest) {
        if (static_cast<int>(slot) == d.my_slot && d.battle.seats[slot].dead) {
            if (!d.respawn_request)
                std::fprintf(stderr, "[netplay] Battle SELECT event=%u slot=%u death=%u\n", d.event_id, slot, d.battle.seats[slot].deaths);
            d.respawn_request = d.battle.seats[slot].deaths;
        }
        return true;
    }
    auto peer = d.peers.find(d.roster[slot].owner);
    if (peer != d.peers.end() && peer->second.alive && peer->second.waiting_event != d.event_id &&
        peer->second.battle_respawn_requested != ee_r32(record + 0x10)) return true;

    f_001671E8_orig(); // native checks its two-second delay, then restores controls, panels and body
    if (!ee_r32(record + 4)) {
        auto &life = d.battle.seats[slot];
        if (netplay_car::read(netplay_car::car_of_slot(slot), life.spawn)) {
            life.spawn_tick = d.tick;
            if (d.chicken_flying[slot]) battle_chicken_cancel(slot);
            d.chicken_flying[slot] = 0;
            d.bodywork[slot] = {};
            d.bodywork[slot].spawn = life.spawn_tick;
            d.last_battle_state = 0;
            if (peer != d.peers.end()) {
                auto &p = peer->second;
                p.battle_respawn_requested = p.impact_through = p.impact_revision = 0;
                p.impact_feedback = {};
                p.has_latest = false;
                d.remote[slot].reset();
                d.shown.erase(slot); // the post-step puppet writer must discard its pre-respawn pose too
            }
            std::fprintf(stderr, "[netplay] Battle respawn event=%u slot=%u death=%u stamp=%u damage=%.4f pos=%.2f,%.2f,%.2f\n",
                         d.event_id, slot, ee_r32(record + 0x10), life.spawn_tick, life.spawn.damage,
                         life.spawn.pose[9], life.spawn.pose[10], life.spawn.pose[11]);
        }
    }
    return true;
}

extern "C" void f_001671E8(void) {
    if (!session().battle_respawn(g_ee.r[4].uw[0], g_ee.r[5].uw[0])) f_001671E8_orig();
}

void Session::battle_frame(uint64_t now) {
    Impl &d = *d_;
    const uint32_t master = ee_r32(0x003CF194);
    if (!master || master > EE_RAM_SIZE - battle_layout::kMasterSize ||
        ee_r32(master) != battle_layout::kMasterVtable || ee_r32(master + 4) != 1 ||
        ee_r32(master + battle_layout::kCount) != d.roster.size() ||
        d.roster.size() > battle_layout::kCapacity || !d.tick) return;
    if (d.role == Impl::Role::Guest) {
        if (d.received_battle.tick > d.battle.tick) {
            for (size_t i = 0; i < d.roster.size(); ++i) {
                const auto &host = d.received_battle.seats[i];
                const uint32_t record = master + battle_layout::kParticipants + i * battle_layout::kParticipantStride;
                if (host.spawn_tick > d.battle.seats[i].spawn_tick) {
                    if (d.chicken_flying[i]) battle_chicken_cancel(i);
                    d.chicken_flying[i] = 0;
                    // Invoke the complete native reset locally, then use the
                    // host's chosen transform. Each machine has its own RNG.
                    const EEState saved = g_ee;
                    g_ee.r[4].ud[0] = master; g_ee.r[5].ud[0] = i;
                    f_001675C0();
                    g_ee = saved;
                    const uint32_t car = netplay_car::car_of_slot(i);
                    netplay_car::write(car, host.spawn);
                    netplay_car::write_condition(car, host.spawn);
                    d.remote.erase(i); d.shown.erase(i);
                    if (static_cast<int>(i) == d.my_slot) {
                        d.impact_applied = std::max(d.impact_applied, host.spawn_tick);
                        d.impact_received = std::max(d.impact_received, d.impact_applied);
                        d.predicted_impact_at = 0; d.last_impact_resolution = d.tick; d.before_contact = host.spawn;
                    }
                    std::fprintf(stderr, "[netplay] Battle respawn applied event=%u slot=%zu death=%u stamp=%u damage=%.4f pos=%.2f,%.2f,%.2f\n",
                                 d.event_id, i, host.deaths, host.spawn_tick, host.spawn.damage,
                                 host.spawn.pose[9], host.spawn.pose[10], host.spawn.pose[11]);
                }
                if (host.dead && (!d.battle.seats[i].dead || host.deaths != d.battle.seats[i].deaths))
                    ee_w32(record + 8, ee_r32(0x003CDACC)); // native HUD uses its local race clock
                ee_w32(record + 4, host.dead);
                ee_w32(record + 0x0C, host.kills);
                ee_w32(record + 0x10, host.deaths);
                ee_w32(record + 0x14, host.penalties);
            }
            d.battle = d.received_battle;
        }
        const auto &life = d.battle.seats[d.my_slot];
        if (!life.dead || life.deaths != d.respawn_request) d.respawn_request = 0;
        if (d.respawn_request && now - d.last_respawn_request >= 100000) {
            Writer w; w.u32(d.event_id); w.u32(d.respawn_request);
            d.send_to(d.host_addr, K_BATTLE_RESPAWN, d.my_id, w);
            d.last_respawn_request = now;
        }
        return;
    }
    for (size_t i = 0; i < d.roster.size(); ++i) {
        auto &life = d.battle.seats[i];
        const uint32_t record = master + battle_layout::kParticipants + i * battle_layout::kParticipantStride;
        auto peer = d.peers.find(d.roster[i].owner);
        if (peer != d.peers.end() && peer->second.alive && peer->second.waiting_event != d.event_id &&
            peer->second.battle_respawn_requested && peer->second.battle_respawn_requested == ee_r32(record + 0x10)) {
            const EEState saved = g_ee;
            g_ee.r[4].ud[0] = master; g_ee.r[5].ud[0] = i;
            battle_respawn(master, static_cast<uint32_t>(i));
            g_ee = saved;
        }
        const bool dead = ee_r32(record + 4) != 0;
        if (dead != life.dead || ee_r32(record + 0x10) != life.deaths) d.last_battle_state = 0;
        life.dead = dead;
        life.kills = ee_r32(record + 0x0C); life.deaths = ee_r32(record + 0x10); life.penalties = ee_r32(record + 0x14);
    }
    if (now - d.last_battle_state < 250000) return;
    d.last_battle_state = now;
    d.battle.tick = d.tick;
    Writer w; w.u32(d.event_id); put_battle_frame(w, d.battle, d.roster.size());
    for (auto &kv : d.peers) {
        const auto &p = kv.second;
        if (p.alive && p.waiting_event != d.event_id) d.send_to(p.addr, K_BATTLE_STATE, 0, w);
    }
}

int Session::chicken_authority() const {
    if (!in_event_ || d_->role == Impl::Role::None) return -1;
    return d_->role == Impl::Role::Host ? 1 : 0;
}

bool Session::chicken_player_slot(uint32_t slot) const {
    return in_event_ && slot < d_->roster.size() && d_->roster[slot].owner != 0xFF;
}

void Session::chicken_launch(uint32_t slot, const float position[3], const float velocity[3]) {
    auto &d = *d_;
    if (!in_event_ || !d.chickens_enabled() || slot >= d.roster.size()) return;
    const uint8_t owner = d.roster[slot].owner;
    const bool host = d.role == Impl::Role::Host;
    if (host ? (owner != 0 && owner != 0xFF) : (static_cast<int>(slot) != d.my_slot)) return;
    ChickenEvent e;
    e.slot = static_cast<uint8_t>(slot); e.life = d.chicken_life(slot);
    e.shot = ++d.chicken_serial[slot]; d.chicken_flying[slot] = e.shot;
    std::copy_n(position, 3, e.position.begin());
    std::copy_n(velocity, 3, e.velocity.begin());
    d.trace_chicken(host ? "launch" : "request", e);
    if (host) {
        d.broadcast_chicken(e);
    } else if (d.host_chickens.queue(e)) {
        d.send_chickens(d.host_chickens, d.host_addr);
    } else {
        d.last_error = "The host stopped receiving event updates.";
        d.abort_requested = true;
        session_abort_event();
    }
}

void Session::chicken_blast(uint32_t slot, const float position[3]) {
    auto &d = *d_;
    if (slot >= d.roster.size()) return;
    d.chicken_last_blast[slot] = 0;
    if (!in_event_ || !d.chickens_enabled() || d.role != Impl::Role::Host || !d.chicken_flying[slot]) return;
    ChickenEvent e;
    e.action = ChickenAction::Blast; e.slot = static_cast<uint8_t>(slot);
    e.life = d.chicken_life(slot); e.shot = d.chicken_flying[slot];
    d.chicken_flying[slot] = 0;
    std::copy_n(position, 3, e.position.begin());
    d.chicken_last_blast[slot] = e.shot;
    d.trace_chicken("blast", e);
    d.broadcast_chicken(e);
}

void Session::chicken_prop_contact(uint32_t shooter, uint32_t body, uint32_t point, uint32_t impulse) {
    auto &d = *d_;
    if (!in_event_ || d.role != Impl::Role::Host || shooter >= d.roster.size() || !d.chicken_last_blast[shooter]) return;
    ChickenEvent e;
    if (!chicken_prop::capture(body, point, impulse, e.prop)) return;
    if (e.prop.type != chicken_prop::Type::Wobble) {
        if (e.prop.object >= d.roster.size()) return;
        e.prop.donor_life = d.chicken_life(e.prop.object);
    }
    e.action = ChickenAction::Prop; e.slot = static_cast<uint8_t>(shooter);
    e.shot = d.chicken_last_blast[shooter]; e.life = d.chicken_life(shooter);
    d.trace_chicken("prop", e);
    d.broadcast_chicken(e);
}

void Session::chicken_victim(uint32_t slot) {
    auto &d = *d_;
    if (!in_event_ || !d.chickens_enabled() || d.role != Impl::Role::Host || slot >= d.roster.size()) return;
    if (std::getenv("EOD_NETPLAY_DEBUG") || std::getenv("EOD_NETPLAY_TEST_CHICKEN"))
        std::fprintf(stderr, "[netplay-chicken] victim event=%u slot=%u tick=%u\n", d.event_id, slot, d.tick);
    // A radial hit has no second car. Hold the recipient's host body through
    // the native impulse and damage callbacks, then send its owner's checkpoint.
    d.bodywork[slot].capture_until = d.tick + 36;
    const auto peer = d.peers.find(d.roster[slot].owner);
    if (peer != d.peers.end() && peer->second.alive && d.puppet_cars.count(peer->first))
        peer->second.impact_through = d.tick + 12;
}

bool Session::car_contact(uint32_t victim, int32_t other_index, uint32_t other_car) {
    Impl &d = *d_;
    if (d.role == Impl::Role::None || !in_event_ || !d.bound || !d.go || d.abort_requested ||
        d.result_until || other_index < 0 || ee_r32(victim + 0x1DC) == (uint32_t)other_index) return false;
    // Confirm both participants are real cars in this event before taking over
    // their bodies. Native collision callbacks identify the other car by its
    // entity index, which can differ from its starting-grid slot.
    int victim_slot = -1, other_slot = -1;
    for (size_t i = 0; i < d.roster.size(); ++i) {
        const uint32_t car = netplay_car::car_of_slot(i);
        if (!car) continue;
        if (car == victim) victim_slot = static_cast<int>(i);
        if ((!other_car || car == other_car) && ee_r32(car + 0x1DC) == (uint32_t)other_index)
            other_slot = static_cast<int>(i);
    }
    if (victim_slot < 0 || other_slot < 0) return false;
    if (d.role == Impl::Role::Guest) {
        // Do not feed a locally predicted bounce back into the host's still
        // approaching pair. It would collide the host against a car which has
        // already recoiled on another timeline. Keep sending the last approach
        // until the host resolves the hit or this bounded prediction expires.
        if (!d.predicted_impact_at && d.tick > d.last_impact_resolution + 24 &&
            (victim_slot == d.my_slot || other_slot == d.my_slot)) {
            d.predicted_impact_at = d.tick;
            d.prediction_deadline = now_us() + 600000;
        }
        return true;
    }
    if (d.native_lap_driver && !d.logged_racing_contact && d.tick >= 60) {
        const auto car_speed = [](uint32_t car) {
            netplay_car::State state;
            return netplay_car::read(car, state) ? std::hypot(state.vel[0], state.vel[2]) : 0.f;
        };
        const float first = car_speed(victim);
        const float second = car_speed(netplay_car::car_of_slot(other_slot));
        if (std::min(first, second) > 10.f) {
            d.logged_racing_contact = true;
            std::fprintf(stderr, "[netplay-test] racing contact event=%u tick=%u slots=%d,%d speed_mps=%.2f,%.2f\n",
                         d.event_id, d.tick, victim_slot, other_slot, first, second);
        }
    }
    for (int slot : {victim_slot, other_slot}) {
        d.bodywork[slot].capture_until = d.tick + 36;
        const auto peer = d.peers.find(d.roster[slot].owner);
        if (peer != d.peers.end() && peer->second.alive && d.puppet_cars.count(peer->first))
            peer->second.impact_through = d.tick + 12;
    }
    return false;
}

void Session::record_damage_feedback(uint32_t car, float flash, float shake) {
    Impl &d = *d_;
    if (!in_event_ || d.role != Impl::Role::Host || !d.go || d.result_until) return;
    for (const auto &[owner, puppet] : d.puppet_cars) {
        if (puppet != car) continue;
        Peer &peer = d.peers.at(owner);
        if (peer.impact_through > d.tick) {
            // Several native contact callbacks can contribute to one checkpoint.
            // The local camera keeps the strongest pulse; adding their damage
            // together would turn a series of small taps into a heavy hit.
            auto &feedback = peer.impact_feedback;
            if (feedback.tick <= peer.impact_revision) feedback.shake = 0.f;
            feedback.tick = d.tick + 1; // step_end stamps this completed step
            feedback.flash = flash;
            feedback.shake = std::max(feedback.shake, shake);
        }
        return;
    }
}

void Session::step_begin() {
    Impl &d = *d_;
    if (!in_event_ || d.role == Impl::Role::None || d.abort_requested) return;
    auto fail_grid = [&](const char *reason) {
        if (d.abort_requested) return; // A disconnect or explicit cancellation already owns the reason and teardown.
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
            if (netplay_hooks::detention_bus_slot >= 0 && d.roster[i].owner != 0xFF && std::getenv("EOD_NETPLAY_DEBUG"))
                std::fprintf(stderr, "[netplay] Detention owner=%u slot=%u vehicle=%u bus_slot=%u damage_factor=%.6f\n",
                             d.roster[i].owner, i, ee_r32(car + 0x24C), ee_r32(0x003CD8F4), native_float(car + 0x4B0));
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
        d.battle = {}; d.received_battle = {};
        d.host_chickens = {}; d.chicken_serial = {}; d.chicken_flying = {}; d.chicken_last_blast = {}; d.prop_wait_tick = 0;
        for (auto &[id, peer] : d.peers) peer.chickens = {};
        d.bodywork = {}; d.next_bodywork_sample = 0;
        for (auto &kv : d.peers) kv.second.bodywork_acks.fill(0);
        d.last_battle_state = d.last_respawn_request = d.respawn_request = 0;
        d.impact_received = d.impact_applied = 0;
        d.predicted_impact_at = d.last_impact_resolution = 0;
        for (auto &kv : d.peers) {
            kv.second.battle_respawn_requested = 0;
            kv.second.has_latest = false; kv.second.latest_tick = 0;
            kv.second.impact_through = kv.second.impact_revision = 0;
            kv.second.impact_feedback = {};
        }
        for (uint32_t i = 0; i < d.roster.size(); ++i) {
            if ((int)i == d.my_slot) continue;
            const bool remote_human = d.roster[i].owner != 0xFF && d.roster[i].owner != local_id;
            if (d.role == Impl::Role::Guest || remote_human) {
                netplay_car::set_puppet(cars[i], true);
                d.puppet_cars[d.role == Impl::Role::Host ? d.roster[i].owner : (int)(100 + i)] = cars[i];
            }
        }
        if (ee_r32(kNativeEventType) & 0x800) {
            // The participant table contains tow cars only. Synchronize their
            // trailers in the same grid message, in matching tow-car order.
            for (size_t i = 0; i < d.roster.size(); ++i) {
                const uint32_t trailer = netplay_car::attached_trailer(cars[i]);
                if (!trailer || !netplay_car::body_of(trailer)) {
                    fail_grid("The event did not attach a trailer to every starting car."); return;
                }
                cars.push_back(trailer);
                if (std::getenv("EOD_NETPLAY_DEBUG"))
                    std::fprintf(stderr, "[netplay] hitch slot=%zu tow=%u trailer=%u\n",
                                 i, ee_r32(cars[i] + 0x24C), ee_r32(trailer + 0x24C));
            }
        }
        d.my_phase = P_LOADED;
        if (d.role == Impl::Role::Host) {
            d.initial_grid.clear(); d.grid_acks.clear();
            const bool impact_test = netplay_impact_fixture::enabled();
            const bool test_battle = d.event_name == "Battle (4 AI)" || d.event_name == session_catalog::kHumanBattle;
            const bool impact_event = test_battle || d.event_name == "Demolition Derby";
            const size_t fixture_size = (netplay_impact_fixture::pack_collision() || std::getenv("EOD_NETPLAY_TEST_OBSERVER")) ? 3 : 2;
            if (impact_test && (cars.size() != fixture_size || !impact_event || d.track_name != "Anoka")) {
                fail_grid("Impact fixture requires its configured driver count on Anoka Battle or Demolition Derby."); return;
            }
            unsigned ai_index = 0;
            for (uint32_t i = 0; i < cars.size(); ++i) {
                netplay_car::State state;
                if (!netplay_car::read(cars[i], state)) { fail_grid("The event created an invalid starting position."); return; }
                if (test_battle && d.track_name == "Anoka") {
                    const auto owner = d.roster[i].owner;
                    netplay_impact_fixture::position_chicken_test_car(cars[i], owner, ai_index, state);
                    if (owner == 0xff) ++ai_index;
                }
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
                    for (const auto &kv : d.peers) if (kv.second.alive && kv.second.waiting_event != d.event_id &&
                        (kv.second.seen_event != d.event_id || kv.second.phase < P_LOADED || d.grid_acks[kv.first] != d.event_id ||
                         (d.event_has_mutator && d.mutator_acks[kv.first] != d.event_id))) return false;
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
        const uint32_t active_event = ee_r32(kNativeEventType);
        d.completion_mode = event_completion_mode(active_event);
        if (d.completion_mode == EventCompletionMode::Battle && d.roster.size() > battle_layout::kCapacity) {
            fail_grid("The starting grid exceeds Battle's participant limit."); return;
        }
        uint16_t participants = 0;
        for (size_t i = 0; i < d.roster.size(); ++i) {
            if (d.completion_mode == EventCompletionMode::Derby || d.completion_mode == EventCompletionMode::Battle ||
                d.roster[i].owner != 0xFF)
                participants |= static_cast<uint16_t>(1u << i);
        }
        d.completion.reset(d.completion_mode, participants);
        d.event_started_at = now_us();
        if (session_test_native_lap() && d.completion_mode == EventCompletionMode::Race) {
            // DriverWrapper selects its built-in racing AI at 0x0018E898.
            // The local slot stays human-owned and receives real checkpoint and lap updates.
            d.saved_auto_driver = ee_r32(0x003CE55C);
            d.native_lap_driver = true;
            d.logged_racing_contact = false;
            ee_w32(0x003CE55C, 1);
            // The native skill, catch-up and steering logic reads this live descriptor.
            std::fprintf(stderr, "[netplay-test] retail CPU event=%u owner=%u slot=%d strength=%u\n",
                         d.event_id, d.my_id, d.my_slot, ee_r32(0x00495BE0 + 0x28));
        }
        if (std::getenv("EOD_NETPLAY_TEST_TERMINAL"))
            std::fprintf(stderr, "[netplay-test] active type=%08x mode=%u participant_mask=%04x\n",
                         active_event, static_cast<unsigned>(d.completion_mode), participants);
        d.my_phase = P_RACING; d.bound = true; d.tick = 0;
        mutator::engine().on_world_bound(d.event_id, d.start_info.seed, d.role == Impl::Role::Host, d.my_slot);
        if (netplay_impact_fixture::enabled() && (cars.size() == 2 || cars.size() == 3)) {
            auto &observation = netplay_impact_fixture::observation;
            observation = {};
            observation.event = d.event_id;
            for (unsigned i = 0; i < cars.size(); ++i) {
                const auto owner = d.roster[i].owner;
                const unsigned fixture_slot = netplay_impact_fixture::pair_slot(owner);
                if (i == static_cast<unsigned>(d.my_slot)) observation.local_slot = fixture_slot;
                if (fixture_slot >= netplay_impact_fixture::collider_count()) { observation.observer = cars[i]; continue; }
                observation.cars[fixture_slot] = cars[i];
                observation.native_indices[fixture_slot] = static_cast<int32_t>(ee_r32(cars[i] + 0x1DC));
            }
        }
        std::fprintf(stderr, "[netplay] GO (slot %d, %zu puppets)\n", d.my_slot, d.puppet_cars.size());
    }
    poll();
    if (d.result_until) return;
    if (netplay_impact_fixture::enabled() && d.tick >= netplay_impact_fixture::kStartTick) {
        auto &observation = netplay_impact_fixture::observation;
        if (observation.event == d.event_id && !observation.started) {
            if (const char *gate = std::getenv("EOD_NETPLAY_TEST_IMPACT_GATE")) {
                // In a headless multi-process run a faster guest can reach
                // the fixture before the host reaches the native countdown.
                std::fprintf(stderr, "[netplay-impact] ready event=%u\n", d.event_id);
                if (!wait_until([&] { return std::ifstream(gate).good(); }, 45000, "Waiting for the impact fixture")) {
                    d.abort_requested = true;
                    session_abort_event();
                    return;
                }
            }
            // Use the driving area for each Anoka variant. The derby
            // penalizes both drivers immediately for leaving its boundary.
            for (size_t seat = 0; seat < d.roster.size(); ++seat) {
                if (netplay_impact_fixture::pair_slot(d.roster[seat].owner) >= netplay_impact_fixture::collider_count()) continue;
                d.remote.erase(seat);
                d.shown.erase(seat);
            }
            const bool battle_frag = d.completion_mode == EventCompletionMode::Battle &&
                                     std::getenv("EOD_NETPLAY_TEST_BATTLE_FRAG");
            const char *following = std::getenv("EOD_NETPLAY_TEST_FOLLOWING");
            for (unsigned i = 0; i < netplay_impact_fixture::collider_count(); ++i) {
                // For a pileup, all three approach the same point 120 degrees apart.
                constexpr float pack_x[] = {1.f, -.5f, -.5f};
                constexpr float pack_z[] = {0.f, .8660254f, -.8660254f};
                const float direction = i == 0 ? 1.f : -1.f;
                const float forward_x = netplay_impact_fixture::pack_collision() ? pack_x[i] : direction;
                const float forward_z = netplay_impact_fixture::pack_collision() ? pack_z[i] : 0.f;
                netplay_car::State state;
                state.pose[0] = state.pose[8] = forward_z;
                state.pose[2] = -forward_x;
                state.pose[6] = forward_x;
                // Leave a one-step gap for the simultaneous impact case. A longer
                // approach allows older in-flight driver snapshots to stagger arrivals.
                const float approach_radius = netplay_impact_fixture::pack_collision() ? 2.7f : 5.f;
                state.pose[9] = -approach_radius * forward_x;
                const char *height = std::getenv("EOD_NETPLAY_TEST_AIRBORNE_METRES");
                state.pose[10] = 0.35f + (height ? std::clamp(std::strtof(height, nullptr), 0.f, 12.f) : 0.f);
                const char *offset = std::getenv("EOD_NETPLAY_TEST_IMPACT_OFFSET");
                const float separation = offset ? std::clamp(std::strtof(offset, nullptr), 0.f, 1.5f) : 0.f;
                const float contact_z = d.event_name == "Demolition Derby" ? 20.f : -45.f;
                state.pose[11] = contact_z - approach_radius * forward_z + direction * separation * .5f;
                state.vel[0] = forward_x * (103.f / 3.6f);
                state.vel[2] = forward_z * (103.f / 3.6f);
                if (following) {
                    // The guest follows the host on the same line. Only the
                    // initial velocities differ; the native solver handles contact.
                    state.pose[2] = -1.f;
                    state.pose[6] = 1.f;
                    state.pose[9] = i == 0 ? -5.5f : 0.f;
                    float speed_kmh = std::strcmp(following, "follow") == 0 ? 72.f : 36.f;
                    if (i == 0 && std::strcmp(following, "nudge") == 0) speed_kmh = 46.f;
                    if (i == 0 && std::strcmp(following, "ram") == 0) speed_kmh = 105.f;
                    state.vel[0] = speed_kmh / 3.6f;
                }
                state.gear = 3;
                // This fixture seeds pre-existing wear. The real native
                // collision must make the final transition to a wreck.
                const char *wreck_driver = std::getenv("EOD_NETPLAY_TEST_WRECK_DRIVER");
                const bool test_respawn = battle_frag && std::getenv("EOD_NETPLAY_TEST_BATTLE_RESPAWN");
                const bool wreck_second = battle_frag ? (!test_respawn && d.event_id % 2 != 0) :
                                          !wreck_driver || std::strcmp(wreck_driver, "guest") != 0;
                const bool wear_both = wreck_driver && std::strcmp(wreck_driver, "both") == 0 && !battle_frag;
                if ((d.event_name == "Demolition Derby" || battle_frag) && (wear_both || ((i == 1) == wreck_second))) {
                    state.damage = battle_frag || wear_both ? 0.999f : 0.94f;
                    state.damage_stage = 3;
                } else if (battle_frag && !test_respawn && d.role == Impl::Role::Host) {
                    const uint32_t master = ee_r32(0x003CF194);
                    const uint32_t slot = ee_r8(observation.cars[i] + 0x252);
                    if (master && master <= EE_RAM_SIZE - battle_layout::kMasterSize &&
                        ee_r32(master) == battle_layout::kMasterVtable && slot < d.roster.size()) {
                        const uint32_t record = master + battle_layout::kParticipants + slot * battle_layout::kParticipantStride;
                        ee_w32(record + 0x0C, ee_r32(record + 0x14) + 4);
                        std::fprintf(stderr, "[netplay-test] battle approach event=%u slot=%u owner=%u points=4\n",
                                     d.event_id, slot, d.roster[slot].owner);
                    }
                }
                netplay_car::write(observation.cars[i], state);
                netplay_car::write_condition(observation.cars[i], state);
            }
            observation.started = true;
        }
    }
    if (d.impact_received > d.impact_applied) {
        const uint32_t car = netplay_car::car_of_slot(d.my_slot);
        netplay_car::State local;
        if (netplay_car::read(car, local)) {
            netplay_car::write(car, d.impact_state);
            netplay_car::write_condition(car, d.impact_state);
            netplay_car::write_controls(car, local);
            if (d.local_finish == EventFinishState::Totalled && d.impact_state.damage_stage < 10)
                d.local_finish = EventFinishState::Active;
            if (d.impact_feedback.tick > d.impact_applied) {
                netplay_hooks::apply_damage_feedback(car, d.impact_feedback.flash, d.impact_feedback.shake);
                if (netplay_impact_fixture::enabled())
                    std::fprintf(stderr, "[netplay-impact] feedback event=%u tick=%u flash=%.6f shake=%.6f\n",
                                 d.event_id, d.impact_feedback.tick, d.impact_feedback.flash, d.impact_feedback.shake);
            }
            d.impact_applied = d.impact_received;
            d.predicted_impact_at = 0;
            d.last_impact_resolution = d.tick;
            netplay_car::State applied;
            if ((netplay_impact_fixture::enabled() || std::getenv("EOD_NETPLAY_TEST_CHICKEN")) &&
                netplay_car::read(car, applied))
                std::fprintf(stderr, "[netplay-impact] reconciled event=%u tick=%u revision=%u slot=%d before_vx=%.6f vx=%.6f vz=%.6f damage=%.6f stage=%u\n",
                             d.event_id, d.tick, d.impact_applied, d.my_slot, local.vel[0],
                             applied.vel[0], applied.vel[2], applied.damage, applied.damage_stage);
        }
    }
    if (d.role == Impl::Role::Guest) {
        if (d.predicted_impact_at && now_us() >= d.prediction_deadline) {
            d.predicted_impact_at = 0;
            d.last_impact_resolution = d.tick;
        }
        if (!d.predicted_impact_at) netplay_car::read(netplay_car::car_of_slot(d.my_slot), d.before_contact);
    }
    // puppets: place each remote car before the step so contacts see it where its owner has it
    std::array<netplay_car::State, kMaxPlayers> contact_cars;
    std::bitset<kMaxPlayers> contact_seats;
    for (size_t i = 0; i < std::min(d.roster.size(), contact_cars.size()); ++i) {
        // The host resolves contacts throughout the field, even when its
        // own driver is far away from a pair of guests or an AI opponent.
        if (d.role == Impl::Role::Host || static_cast<int>(i) == d.my_slot)
            contact_seats[i] = netplay_car::read(netplay_car::car_of_slot(i), contact_cars[i]);
    }
    for (auto &kv : d.remote) {
        if (!kv.second.ready()) continue;
        uint32_t car = netplay_car::car_of_slot((uint32_t)kv.first);
        if (!car || !netplay_car::is_puppet(car)) continue;
        uint32_t route_rtt = d.host_clock.round_trip_ms();
        if (d.role == Impl::Role::Host) {
            auto peer = d.peers.find(d.roster[kv.first].owner);
            if (peer != d.peers.end()) route_rtt = peer->second.ping_ms;
            if (peer != d.peers.end() && peer->second.impact_through) {
                if (d.tick - peer->second.latest_host_tick >= netplay_car::Remote::kStallSteps)
                    netplay_car::release_controls(car);
                else if (peer->second.has_latest)
                    netplay_car::write_controls(car, peer->second.latest);
                continue;  // native bodies keep both sides of the host's collision impulse
            }
        }
        if (kv.second.stalled()) {
            netplay_car::release_controls(car);
            continue;  // gravity, ground contact and momentum continue in the native solver
        }
        float proximity = 0.f;
        for (size_t i = 0; i < contact_cars.size() && proximity < 1.f; ++i) {
            if (contact_seats[i] && i != static_cast<size_t>(kv.first))
                proximity = std::max(proximity, kv.second.proximity_to(contact_cars[i]));
        }
        if (d.role == Impl::Role::Guest && proximity > 0.f) {
            // A guest-owned snapshot reaches another guest via the host.
            const uint8_t owner = d.roster[kv.first].owner;
            if (owner != 0 && owner != 0xFF)
                for (const auto &player : d.lobby_players)
                    if (player.id == owner) { route_rtt += player.ping; break; }
        }
        d.shown[kv.first] = kv.second.advance(g_config.snapshot_buffer_ms, proximity, route_rtt);
        netplay_car::write(car, d.shown[kv.first]);
        netplay_car::write_condition(car, d.shown[kv.first]);
    }
    d.sync_chickens(); // replay at a world-step boundary, after placing all shooter bodies
    mutator::engine().tick(d.tick);
    if (netplay_impact_fixture::enabled()) {
        const auto &obs = netplay_impact_fixture::observation;
        if (obs.started && obs.event == d.event_id && obs.tick <= 120 && d.role == Impl::Role::Host) {
            for (unsigned slot = 0; slot < netplay_impact_fixture::collider_count(); ++slot) {
                netplay_car::State state;
                if (netplay_car::read(obs.cars[slot], state))
                    std::fprintf(stderr, "[netplay-impact] before event=%u tick=%u slot=%u vx=%.5f vz=%.5f z=%.5f\n",
                                 obs.event, obs.tick, slot, state.vel[0], state.vel[2], state.pose[11]);
            }
        }
    }
}
bool Session::step_allowed() const {
    return !in_event_ || (d_->bound && d_->go && !d_->abort_requested && !d_->result_until);
}
void Session::step_end() {
    Impl &d = *d_;
    if (!in_event_ || !d.bound || !d.go || d.abort_requested || d.role == Impl::Role::None) return;
    d.sync_bodywork();
    if (netplay_impact_fixture::enabled()) {
        auto &observation = netplay_impact_fixture::observation;
        if (observation.started && observation.event == d.event_id && observation.tick <= 180) {
            for (unsigned i = 0; i < netplay_impact_fixture::collider_count(); ++i) {
                const auto car = observation.cars[i];
                netplay_car::State state;
                if (!netplay_car::read(car, state)) continue;
                if (observation.tick == 0 || observation.tick == 120 || observation.tick == 180) {
                    const auto detail = netplay_damage::capture(car);
                    uint32_t hash = 2166136261u;
                    for (uint8_t b : detail) hash = (hash ^ b) * 16777619u;
                    std::fprintf(stderr, "[netplay-impact] shape event=%u tick=%u slot=%u bytes=%zu hash=%08x panels=%04x wheels=%x\n",
                                 observation.event, observation.tick, i, detail.size(), hash,
                                 detail.empty() ? 0 : (detail[1] << 8) | detail[2], detail.empty() ? 0 : detail[3]);
                }
                std::fprintf(stderr, "[netplay-impact] sample event=%u tick=%u local=%u slot=%u x=%.5f y=%.5f z=%.5f vx=%.5f vz=%.5f damage=%.6f wreck=%u native_tick=%u paused=%u\n",
                             observation.event, observation.tick, observation.local_slot, i,
                             state.pose[9], state.pose[10], state.pose[11], state.vel[0], state.vel[2], state.damage, ee_r32(car + 0x1BE8),
                             ee_r32(0x003CDA8C), ee_r32(0x003CDAC4));
            }
            if (observation.local_slot == 2 && observation.observer) {
                netplay_car::State third;
                if (netplay_car::read(observation.observer, third))
                    std::fprintf(stderr, "[netplay-impact] observer tick=%u x=%.5f z=%.5f gas=%.5f\n",
                                 observation.tick, third.pose[9], third.pose[11], third.gas);
            }
            ++observation.tick;
        }
    }
    for (auto &kv : d.shown) {
        uint32_t car = netplay_car::car_of_slot((uint32_t)kv.first);
        if (!car || !netplay_car::is_puppet(car)) continue;
        if (d.role == Impl::Role::Host) {
            auto peer = d.peers.find(d.roster[kv.first].owner);
            if (peer != d.peers.end() && peer->second.impact_through) continue;
        }
        auto remote = d.remote.find(kv.first);
        if (remote != d.remote.end() && remote->second.stalled()) {
            netplay_car::State native;
            if (netplay_car::read(car, native)) remote->second.follow_native(native);
        } else {
            netplay_car::write(car, kv.second);
        }
        netplay_car::write_condition(car, kv.second);
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
            Writer w; w.u32(d.event_id); w.u32(d.impact_applied);
            w.u8(1);
            if (d.predicted_impact_at)
                put_car(w, (uint8_t)d.my_slot, d.predicted_impact_at, d.before_contact);
            else
                put_car(w, (uint8_t)d.my_slot, d.tick, s);
            if (d.completion_mode == EventCompletionMode::Battle) w.u32(d.battle.seats[d.my_slot].spawn_tick);
            d.send_to(d.host_addr, K_SNAP, d.my_id, w);
        }
    } else {
        for (auto &kv : d.peers) {
            Peer &p = kv.second; if (!p.alive || p.waiting_event == d.event_id) continue;
            if (p.impact_through) {
                for (size_t i = 0; i < d.roster.size(); ++i) {
                    if (d.roster[i].owner != p.id) continue;
                    if ((!p.impact_revision || p.impact_revision < p.impact_through) &&
                        netplay_car::read(netplay_car::car_of_slot(i), p.impact_state)) {
                        p.impact_revision = d.tick;
                        if (netplay_impact_fixture::enabled() || std::getenv("EOD_NETPLAY_TEST_CHICKEN"))
                            std::fprintf(stderr, "[netplay-impact] confirmed event=%u revision=%u slot=%zu vx=%.6f vz=%.6f damage=%.6f stage=%u\n",
                                         d.event_id, p.impact_revision, i, p.impact_state.vel[0], p.impact_state.vel[2],
                                         p.impact_state.damage, p.impact_state.damage_stage);
                    }
                    if (p.impact_revision) {
                        Writer correction; correction.u32(d.event_id);
                        put_car(correction, i, p.impact_revision, p.impact_state);
                        correction.u32(p.impact_feedback.tick);
                        correction.f32(p.impact_feedback.flash);
                        correction.f32(p.impact_feedback.shake);
                        if (d.completion_mode == EventCompletionMode::Battle) correction.u32(d.battle.seats[i].spawn_tick);
                        d.send_to(p.addr, K_IMPACT_STATE, 0, correction);
                    }
                    break;
                }
            }
            Writer w; uint32_t n = 0; Writer body;
            for (uint32_t i = 0; i < d.roster.size(); ++i) {
                if (d.roster[i].owner == p.id) continue;
                netplay_car::State s;
                const bool remote_human = d.roster[i].owner != 0xFF && d.roster[i].owner != 0;
                const auto it = d.peers.find(d.roster[i].owner);
                if (remote_human && it != d.peers.end() && it->second.alive && it->second.waiting_event != d.event_id) {
                    const auto remote = d.remote.find(i);
                    if (it->second.impact_through || (remote != d.remote.end() && remote->second.stalled())) {
                        if (!netplay_car::read(netplay_car::car_of_slot(i), s)) continue;
                        put_car(body, (uint8_t)i, d.tick, s);
                    } else {
                        if (!it->second.has_latest || t - it->second.latest_at > 500000) continue;
                        // Timestamp at receipt. A repeated old packet must not
                        // pin an observer's extrapolator to a false new sample.
                        put_car(body, (uint8_t)i, it->second.latest_host_tick, it->second.latest);
                    }
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

int Session::online_ai_override() const {
    const Impl &d = *d_;
    if (!in_event_ || d.role != Impl::Role::Host || d.human_battle()) return -1;
    if (d.ai_count >= 16) {
        const int humans = 1 + d.alive_count();
        return std::max(0, static_cast<int>(d.capacity()) - humans);
    }
    return std::clamp(d.ai_count, 0, 15);
}

Session &session() { static Session s; return s; }
namespace {
const bool g_registered = [] {
    namespace H = netplay_hooks;
    H::remote_driver_car = [](uint32_t car) { return session().remote_driver_car(car); };
    H::chicken_authority = [] { return session().chicken_authority(); };
    H::chicken_player_slot = [](uint32_t slot) { return session().chicken_player_slot(slot); };
    H::chicken_launch = [](uint32_t slot, const float p[3], const float v[3]) { session().chicken_launch(slot, p, v); };
    H::chicken_blast = [](uint32_t slot, const float p[3]) { session().chicken_blast(slot, p); };
    H::chicken_prop_contact = [](uint32_t s, uint32_t b, uint32_t p, uint32_t i) { session().chicken_prop_contact(s, b, p, i); };
    H::chicken_victim = [](uint32_t slot) { session().chicken_victim(slot); };
    H::eligible_battle_slots = [] { return session().eligible_battle_slots(); };
    H::car_contact = [](uint32_t car, int32_t other) { return session().car_contact(car, other); };
    H::damage_feedback = [](uint32_t car, float flash, float shake) { session().record_damage_feedback(car, flash, shake); };
    H::race_rules = [] { return session().online_rules(); };
    H::race_tuning = [] { return session().online_tuning(); };
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
    H::online_ai_override = [] { return session().online_ai_override(); };
    H::roster_pre = [](uint32_t d) { session().roster_pre(d); };
    H::roster_post = [](uint32_t d) { session().roster_post(d); };
    return true;
}();
}
}  // namespace netplay
