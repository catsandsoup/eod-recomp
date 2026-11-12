// Garage Code internet layer: see netplay_online.h and docs/NETPLAY_INTERNET.md.
#include "netplay_online.h"
#include "netplay_online_wire.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define EOD_CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define EOD_CLOSESOCK close
#endif

namespace netplay_online {
namespace {
using Clock = std::chrono::steady_clock;
uint64_t now_ms() { return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }

// Public rendezvous infrastructure. Several independent trackers, so one outage never matters.
struct HostPort { const char *host; uint16_t port; };
const HostPort kTrackers[] = {
    {"tracker.opentrackr.org", 1337}, {"open.stealth.si", 80},        {"tracker.torrent.eu.org", 451}, {"explodie.org", 6969},
    {"tracker.dler.org", 6969},       {"open.demonii.com", 1314},     {"udp.tracker.projectk.org", 23333}, {"tracker.tryhackx.org", 6969},
};
const HostPort kStun[] = {{"stun.l.google.com", 19302}, {"stun.cloudflare.com", 3478}, {"stun1.l.google.com", 19302}};

bool resolve_v4(const char *host, uint16_t port, sockaddr_in &out) {
    // Degraded-network test hooks (never set in play): EOD_NETPLAY_TEST_DNS_DELAY_MS makes every lookup take
    // that long, as a blackholed DNS server does; EOD_NETPLAY_TEST_UNREACHABLE answers every lookup with an
    // address that never replies (TEST-NET-1), as a dead tracker or a network with no route out does.
    static const int dns_delay = std::getenv("EOD_NETPLAY_TEST_DNS_DELAY_MS") ? std::atoi(std::getenv("EOD_NETPLAY_TEST_DNS_DELAY_MS")) : 0;
    if (dns_delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(dns_delay));
    if (std::getenv("EOD_NETPLAY_TEST_UNREACHABLE")) {
        out = sockaddr_in{}; out.sin_family = AF_INET; out.sin_addr.s_addr = htonl(0xC0000201); out.sin_port = htons(port);
        return true;
    }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) return false;
    out = *(sockaddr_in *)res->ai_addr; out.sin_port = htons(port);
    freeaddrinfo(res);
    return true;
}
int udp_socket() {
    int fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
#ifdef _WIN32
    u_long on = 1; ioctlsocket(fd, FIONBIO, &on);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#endif
    return fd;
}
bool wait_readable(int fd, int ms) {
    fd_set s; FD_ZERO(&s); FD_SET(fd, &s);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    return select(fd + 1, &s, nullptr, nullptr, &tv) > 0;
}
void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
std::mutex g_log_mutex;
std::deque<std::string> g_log;
void logf(const char *fmt, ...) {
    char buf[300];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    std::fprintf(stderr, "[netplay-net] %s\n", buf);
    std::lock_guard<std::mutex> l(g_log_mutex);
    g_log.push_back(buf); if (g_log.size() > 40) g_log.pop_front();
}
void random_bytes(uint8_t *p, size_t n) { std::random_device rd; for (size_t i = 0; i < n; ++i) p[i] = (uint8_t)rd(); }

// ---- tiny blocking HTTP/1.0 over TCP with a hard deadline (UPnP only)
std::string http_exchange(const sockaddr_in &to, const std::string &request, int timeout_ms) {
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return {};
    std::string out;
#ifndef _WIN32
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#endif
    const uint64_t t0 = now_ms();
    int rc = connect(fd, (const sockaddr *)&to, sizeof to);
    if (rc != 0) {
        fd_set w; FD_ZERO(&w); FD_SET(fd, &w);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        if (select(fd + 1, nullptr, &w, nullptr, &tv) <= 0) { EOD_CLOSESOCK(fd); return {}; }
        int err = 0; socklen_t el = sizeof err; getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &el);
        if (err) { EOD_CLOSESOCK(fd); return {}; }
    }
    size_t sent = 0;
    while (sent < request.size() && now_ms() - t0 < (uint64_t)timeout_ms) {
        int n = (int)send(fd, request.data() + sent, request.size() - sent, 0);
        if (n > 0) sent += (size_t)n; else sleep_ms(5);
    }
    while (now_ms() - t0 < (uint64_t)timeout_ms) {
        if (!wait_readable(fd, 100)) continue;
        char buf[4096];
        int n = (int)recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        out.append(buf, (size_t)n);
        if (out.size() > 200000) break;
    }
    EOD_CLOSESOCK(fd);
    return out;
}
std::string http_body(const std::string &r) { size_t p = r.find("\r\n\r\n"); return p == std::string::npos ? std::string() : r.substr(p + 4); }
int http_status(const std::string &r) { return r.compare(0, 5, "HTTP/") == 0 && r.size() > 12 ? std::atoi(r.c_str() + 9) : 0; }

// Default gateway (macOS/BSD: `route -n get default`; elsewhere the .1 of the local /24 is the only guess).
uint32_t default_gateway() {
#ifdef _WIN32
    return 0;
#else
    FILE *p = popen("/sbin/route -n get default 2>/dev/null", "r");
    if (!p) return 0;
    char line[256]; uint32_t gw = 0;
    while (std::fgets(line, sizeof line, p)) {
        const char *g = std::strstr(line, "gateway:");
        if (g) { std::string v = g + 8; while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1); while (!v.empty() && (v.back() == '\n' || v.back() == ' ')) v.pop_back(); parse_ipv4(v, gw); }
    }
    pclose(p);
    return gw;
#endif
}
// The address of the interface that routes towards `to`.
uint32_t local_ip_towards(uint32_t to) {
    int fd = udp_socket();
    if (fd < 0) return 0;
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(to); a.sin_port = htons(9);
    uint32_t me = 0;
    if (connect(fd, (sockaddr *)&a, sizeof a) == 0) { sockaddr_in l{}; socklen_t ll = sizeof l; if (getsockname(fd, (sockaddr *)&l, &ll) == 0) me = ntohl(l.sin_addr.s_addr); }
    EOD_CLOSESOCK(fd);
    return me;
}
}  // namespace

// ============================================================================ implementation
struct Online::Impl {
    int game_fd = -1;
    uint16_t local_port = 0;
    std::atomic<bool> stop_flag{false}, running{false};
    std::thread net_thread, map_thread;
    mutable std::mutex m;
    std::condition_variable cv;
    Report rep;
    std::vector<Job> jobs;
    struct JobState { uint64_t next_due = 0; bool registered = false; std::vector<uint16_t> ports; uint64_t found_at = 0; std::vector<Addr> found; };
    std::map<std::string, JobState> state;
    std::deque<std::vector<uint8_t>> stun_in;
    bool poke_flag = false;
    uint8_t peer_id[20];
    std::vector<uint16_t> ports_cache;        // announce ports last used
    // mapping state (map thread)
    uint32_t gw = 0;
    uint16_t map_internal = 0;
    std::string map_control_path, map_service; sockaddr_in map_igd{};

    struct Tracker { std::string host; uint16_t port; sockaddr_in addr{}; bool resolved = false; uint64_t cid = 0, cid_at = 0; bool answered = false; };
    std::vector<Tracker> trackers;
    int tsock = -1;
    uint32_t next_tid = 1;

    // ------------------------------------------------------------------ STUN
    void stun_phase() {
        sockaddr_in servers[3]; bool have[3] = {};
        int n_servers = (int)(sizeof kStun / sizeof kStun[0]);
        for (int i = 0; i < n_servers; ++i) have[i] = resolve_v4(kStun[i].host, kStun[i].port, servers[i]);
        uint8_t tid[3][12];
        for (int i = 0; i < n_servers; ++i) random_bytes(tid[i], 12);
        uint32_t ips[3] = {}; uint16_t ports[3] = {}; bool got[3] = {};
        const uint64_t t0 = now_ms();
        int next_send = 0;
        while (now_ms() - t0 < 3000 && !stop_flag) {
            if (now_ms() - t0 >= (uint64_t)next_send * 500 && next_send < 5) {
                for (int i = 0; i < n_servers; ++i)
                    if (have[i] && !got[i]) { auto req = stun_request(tid[i]); sendto(game_fd, (const char *)req.data(), (int)req.size(), 0, (const sockaddr *)&servers[i], sizeof servers[i]); }
                ++next_send;
            }
            std::vector<uint8_t> pkt;
            {
                std::unique_lock<std::mutex> l(m);
                if (stun_in.empty()) cv.wait_for(l, std::chrono::milliseconds(50));
                if (!stun_in.empty()) { pkt = stun_in.front(); stun_in.pop_front(); }
            }
            if (pkt.empty()) continue;
            for (int i = 0; i < n_servers; ++i)
                if (!got[i] && stun_parse_response(pkt.data(), pkt.size(), tid[i], ips[i], ports[i])) got[i] = true;
            if (got[0] && got[1] && got[2]) break;
            if (now_ms() - t0 > 1200 && got[0] + got[1] + got[2] >= 2) break;
        }
        std::lock_guard<std::mutex> l(m);
        int replies = got[0] + got[1] + got[2];
        rep.stun_replies = replies; rep.stun_done = true; rep.stun_ok = replies > 0;
        for (int i = 0; i < n_servers; ++i) if (got[i]) { rep.pub_ip = ips[i]; rep.pub_port = ports[i]; break; }
        for (int i = 0; i < n_servers; ++i) for (int j = i + 1; j < n_servers; ++j)
            if (got[i] && got[j] && (ips[i] != ips[j] || ports[i] != ports[j])) rep.symmetric = true;
        logf("STUN: %d replies, public %s:%u%s", replies, ip_text(rep.pub_ip).c_str(), rep.pub_port, rep.symmetric ? " (SYMMETRIC: port depends on destination)" : "");
    }

    // ------------------------------------------------------------------ router port mapping
    void map_thread_main() {
        const bool force_off = std::getenv("EOD_NETPLAY_NO_MAPPING") != nullptr;
        gw = default_gateway();
        uint32_t me = gw ? local_ip_towards(gw) : 0;
        if (force_off || !gw || !me) {
            std::lock_guard<std::mutex> l(m);
            rep.map_done = true; rep.map_note = force_off ? "router mapping disabled" : "no gateway found"; cv.notify_all();
            logf("mapping: %s", rep.map_note.c_str());
            return;
        }
        bool ok = try_pcp(me) || try_pmp() || try_upnp(me);
        {
            std::lock_guard<std::mutex> l(m);
            rep.map_done = true;
            if (!ok) rep.map_note = "the router did not offer a port mapping";
            cv.notify_all();
        }
        if (!ok) { logf("mapping: none (router %s offers no PCP/NAT-PMP/UPnP, or it is disabled)", ip_text(gw).c_str()); return; }
        // renew at half the lease until stopped
        uint64_t renew_at = now_ms() + 900000;
        while (!stop_flag) {
            sleep_ms(250);
            if (now_ms() < renew_at) continue;
            renew_at = now_ms() + 900000;
            if (rep.map_kind == "UPnP") try_upnp(me); else if (rep.map_kind == "PCP") try_pcp(me); else try_pmp();
        }
    }
    void set_mapped(const char *kind, uint32_t ext_ip, uint16_t ext_port) {
        std::lock_guard<std::mutex> l(m);
        rep.mapped = true; rep.map_kind = kind; rep.ext_ip = ext_ip; rep.ext_port = ext_port; rep.map_note.clear();
        logf("mapping: %s forwards UDP %u -> %s:%u", kind, local_port, ip_text(ext_ip).c_str(), ext_port);
    }
    bool gateway_exchange(const std::vector<uint8_t> &req, std::vector<uint8_t> &resp, int ms = 600) {
        int fd = udp_socket();
        if (fd < 0) return false;
        sockaddr_in to{}; to.sin_family = AF_INET; to.sin_addr.s_addr = htonl(gw); to.sin_port = htons(5351);
        bool ok = false;
        for (int attempt = 0; attempt < 2 && !ok && !stop_flag; ++attempt) {
            sendto(fd, (const char *)req.data(), (int)req.size(), 0, (sockaddr *)&to, sizeof to);
            if (wait_readable(fd, ms)) {
                uint8_t buf[128]; sockaddr_in from; socklen_t fl = sizeof from;
                int n = (int)recvfrom(fd, (char *)buf, sizeof buf, 0, (sockaddr *)&from, &fl);
                if (n > 0 && from.sin_addr.s_addr == to.sin_addr.s_addr) { resp.assign(buf, buf + n); ok = true; }
            }
        }
        EOD_CLOSESOCK(fd);
        return ok;
    }
    bool try_pcp(uint32_t me) {
        uint8_t nonce[12]; random_bytes(nonce, 12);
        std::vector<uint8_t> resp;
        if (!gateway_exchange(pcp_map_request(me, nonce, local_port, local_port, 1800), resp)) return false;
        uint16_t ep; uint32_t eip, life;
        if (!pcp_parse_map(resp.data(), resp.size(), nonce, local_port, ep, eip, life)) return false;
        if (is_private_or_shared_ipv4(eip)) { logf("mapping: PCP gave a private external address %s (double NAT?)", ip_text(eip).c_str()); return false; }
        set_mapped("PCP", eip, ep);
        return true;
    }
    bool try_pmp() {
        std::vector<uint8_t> resp;
        uint32_t eip = 0;
        if (!gateway_exchange(pmp_external_request(), resp) || !pmp_parse_external(resp.data(), resp.size(), eip)) return false;
        if (!gateway_exchange(pmp_map_request(local_port, local_port, 1800), resp)) return false;
        uint16_t ep; uint32_t life;
        if (!pmp_parse_map(resp.data(), resp.size(), local_port, ep, life)) return false;
        if (is_private_or_shared_ipv4(eip)) { logf("mapping: NAT-PMP external address %s is private (double NAT?)", ip_text(eip).c_str()); return false; }
        set_mapped("NAT-PMP", eip, ep);
        return true;
    }
    std::string soap(const std::string &action, const std::string &args, int &status) {
        const std::string body = soap_envelope(map_service, action, args);
        const std::string req = "POST " + map_control_path + " HTTP/1.0\r\nHOST: " + ip_text(ntohl(map_igd.sin_addr.s_addr)) + ":" + std::to_string(ntohs(map_igd.sin_port)) +
                                "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nContent-Length: " + std::to_string(body.size()) + "\r\nSOAPAction: \"" + map_service + "#" + action +
                                "\"\r\nConnection: close\r\n\r\n" + body;
        const std::string r = http_exchange(map_igd, req, 2500);
        status = http_status(r);
        return http_body(r);
    }
    bool try_upnp(uint32_t me) {
        if (map_control_path.empty()) {
            int fd = udp_socket();
            if (fd < 0) return false;
            sockaddr_in to{}; to.sin_family = AF_INET; to.sin_addr.s_addr = inet_addr("239.255.255.250"); to.sin_port = htons(1900);
            static const char *sts[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1", "urn:schemas-upnp-org:device:InternetGatewayDevice:2", "upnp:rootdevice"};
            std::string location;
            for (int round = 0; round < 2 && location.empty() && !stop_flag; ++round) {
                for (const char *st : sts) { std::string q = ssdp_search(st); sendto(fd, q.data(), (int)q.size(), 0, (sockaddr *)&to, sizeof to); }
                const uint64_t t0 = now_ms();
                while (now_ms() - t0 < 1500 && location.empty() && !stop_flag) {
                    if (!wait_readable(fd, 100)) continue;
                    char buf[2048]; sockaddr_in from; socklen_t fl = sizeof from;
                    int n = (int)recvfrom(fd, buf, sizeof buf - 1, 0, (sockaddr *)&from, &fl);
                    if (n <= 0) continue;
                    std::string msg(buf, (size_t)n);
                    std::string loc = header_value(msg, "location");
                    if (!loc.empty() && ntohl(from.sin_addr.s_addr) == gw) location = loc;   // only the default gateway is trusted
                }
            }
            EOD_CLOSESOCK(fd);
            Url u;
            if (location.empty() || !parse_http_url(location, u)) return false;
            if (!resolve_v4(u.host.c_str(), u.port, map_igd) || ntohl(map_igd.sin_addr.s_addr) != gw) return false;
            const std::string desc = http_exchange(map_igd, "GET " + u.path + " HTTP/1.0\r\nHOST: " + u.host + ":" + std::to_string(u.port) + "\r\nConnection: close\r\n\r\n", 2500);
            std::string control;
            if (!upnp_find_service(http_body(desc), map_service, control)) { logf("mapping: UPnP gateway found but no WAN connection service"); return false; }
            if (control.compare(0, 7, "http://") == 0) { Url cu; if (parse_http_url(control, cu)) control = cu.path; }
            if (control.empty() || control[0] != '/') control = "/" + control;
            map_control_path = control;
        }
        int status = 0;
        std::string ext = xml_tag(soap("GetExternalIPAddress", "", status), "NewExternalIPAddress");
        uint32_t eip = 0;
        if (!parse_ipv4(ext, eip)) { logf("mapping: UPnP could not tell the external address (HTTP %d)", status); return false; }
        if (is_private_or_shared_ipv4(eip)) { logf("mapping: UPnP external address %s is private (double NAT or carrier NAT)", ext.c_str()); return false; }
        const std::string client = ip_text(me);
        uint32_t lease = 1800;
        for (int attempt = 0; attempt < 3; ++attempt) {
            std::string r = soap("AddPortMapping", upnp_add_mapping_args(local_port, local_port, client, lease), status);
            if (status == 200) { set_mapped("UPnP", eip, local_port); return true; }
            const std::string code = xml_tag(r, "errorCode");
            logf("mapping: UPnP AddPortMapping refused (HTTP %d, error %s)", status, code.c_str());
            if (code == "725") lease = 0;                  // router only allows permanent leases
            else if (code == "718" || code == "729") break; // already in use by someone else
            else if (attempt == 0) lease = 0;
            else break;
        }
        return false;
    }
    void remove_mapping() {
        if (!rep.mapped) return;
        if (rep.map_kind == "UPnP") {
            int status = 0;
            soap("DeletePortMapping", "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(local_port) + "</NewExternalPort><NewProtocol>UDP</NewProtocol>", status);
            logf("mapping: UPnP mapping removed (HTTP %d)", status);
        } else if (rep.map_kind == "NAT-PMP") {
            std::vector<uint8_t> resp; gateway_exchange(pmp_map_request(local_port, local_port, 0), resp, 300);
            logf("mapping: NAT-PMP mapping removed");
        } else if (rep.map_kind == "PCP") {
            uint8_t nonce[12] = {}; std::vector<uint8_t> resp;
            gateway_exchange(pcp_map_request(local_ip_towards(gw), nonce, local_port, local_port, 0), resp, 300);
        }
    }

    // ------------------------------------------------------------------ trackers
    struct Op { std::vector<uint8_t> hash; uint16_t port; TrackerEvent ev; uint64_t left; bool want; std::string tag; std::vector<Addr> peers; bool answered = false; };
    void drain_tsock(std::map<uint32_t, std::pair<int, int>> &pending_announce, std::map<uint32_t, int> &pending_connect, std::vector<Op> &ops, int wait_ms) {
        const uint64_t t0 = now_ms();
        uint64_t first_answer = 0;
        while (now_ms() - t0 < (uint64_t)wait_ms && !stop_flag) {
            if (pending_announce.empty() && pending_connect.empty()) return;
            if (first_answer && now_ms() - first_answer > 450) return;   // the slow or dead trackers do not hold everyone up
            if (!wait_readable(tsock, 50)) continue;
            for (;;) {
                uint8_t buf[2048]; sockaddr_in from; socklen_t fl = sizeof from;
                int n = (int)recvfrom(tsock, (char *)buf, sizeof buf, 0, (sockaddr *)&from, &fl);
                if (n <= 0) break;
                uint32_t tid, interval; uint64_t cid; std::vector<TrackerPeer> peers;
                if (tracker_parse_connect(buf, (size_t)n, tid, cid)) {
                    auto it = pending_connect.find(tid);
                    if (it != pending_connect.end()) { Tracker &t = trackers[it->second]; t.cid = cid; t.cid_at = now_ms(); t.answered = true; pending_connect.erase(it); if (!first_answer) first_answer = now_ms(); }
                } else if (tracker_parse_announce(buf, (size_t)n, tid, interval, peers)) {
                    auto it = pending_announce.find(tid);
                    if (it != pending_announce.end()) {
                        trackers[it->second.first].answered = true;
                        Op &op = ops[it->second.second];
                        op.answered = true;
                        for (auto &p : peers) { Addr a{p.ip, p.port}; if (p.port && std::find(op.peers.begin(), op.peers.end(), a) == op.peers.end()) op.peers.push_back(a); }
                        pending_announce.erase(it);
                        if (!first_answer) first_answer = now_ms();
                    }
                } else if (tracker_is_error(buf, (size_t)n, tid)) {
                    pending_announce.erase(tid); pending_connect.erase(tid);
                }
            }
        }
    }
    void tracker_round(std::vector<Op> &ops) {
        if (tsock < 0) { tsock = udp_socket(); if (tsock < 0) return; }
        std::map<uint32_t, std::pair<int, int>> pending_announce;
        std::map<uint32_t, int> pending_connect;
        for (size_t i = 0; i < trackers.size(); ++i) {
            Tracker &t = trackers[i];
            if (!t.resolved || (t.cid && now_ms() - t.cid_at < 50000)) continue;
            const uint32_t tid = next_tid++;
            auto req = tracker_connect(tid);
            sendto(tsock, (const char *)req.data(), (int)req.size(), 0, (sockaddr *)&t.addr, sizeof t.addr);
            pending_connect[tid] = (int)i;
        }
        drain_tsock(pending_announce, pending_connect, ops, 1800);
        for (size_t i = 0; i < trackers.size(); ++i) {
            Tracker &t = trackers[i];
            if (!t.resolved || !t.cid || now_ms() - t.cid_at >= 58000) continue;
            for (size_t k = 0; k < ops.size(); ++k) {
                if (ops[k].ev == TE_STOPPED) continue;
                const uint32_t tid = next_tid++;
                auto req = tracker_announce(t.cid, tid, ops[k].hash, peer_id, ops[k].left, ops[k].ev, ops[k].port, ops[k].want ? 60 : 0);
                sendto(tsock, (const char *)req.data(), (int)req.size(), 0, (sockaddr *)&t.addr, sizeof t.addr);
                pending_announce[tid] = {(int)i, (int)k};
            }
        }
        std::map<uint32_t, int> none;
        drain_tsock(pending_announce, none, ops, 1800);
        // fire-and-forget "stopped" for lookups (they only wanted the list) and for announces being withdrawn
        for (size_t i = 0; i < trackers.size(); ++i) {
            Tracker &t = trackers[i];
            if (!t.resolved || !t.cid || now_ms() - t.cid_at >= 58000) continue;
            for (auto &op : ops) {
                if (!(op.ev == TE_STOPPED || op.want)) continue;
                auto req = tracker_announce(t.cid, next_tid++, op.hash, peer_id, op.left, TE_STOPPED, op.port, 0);
                sendto(tsock, (const char *)req.data(), (int)req.size(), 0, (sockaddr *)&t.addr, sizeof t.addr);
            }
        }
    }

    std::vector<uint16_t> announce_ports() {
        std::vector<uint16_t> p;
        std::lock_guard<std::mutex> l(m);
        if (rep.mapped && rep.ext_port) p.push_back(rep.ext_port);
        if (rep.stun_ok && rep.pub_port && std::find(p.begin(), p.end(), rep.pub_port) == p.end()) p.push_back(rep.pub_port);
        if (p.empty()) p.push_back(local_port);
        return p;
    }

    void net_thread_main() {
        stun_phase();
        {
            std::lock_guard<std::mutex> l(m);
            for (auto &h : kTrackers) { Tracker t; t.host = h.host; t.port = h.port; trackers.push_back(t); }
            rep.trackers_total = (int)trackers.size();
        }
        for (auto &t : trackers) { if (stop_flag) return; t.resolved = resolve_v4(t.host.c_str(), t.port, t.addr); }
        while (!stop_flag) {
            {
                std::unique_lock<std::mutex> l(m);
                cv.wait_for(l, std::chrono::milliseconds(100));
            }
            if (stop_flag) break;
            const uint64_t now = now_ms();
            const std::vector<uint16_t> ports = announce_ports();
            std::vector<Op> ops;
            {
                std::lock_guard<std::mutex> l(m);
                const bool poked = poke_flag; poke_flag = false;
                ports_cache = ports;
                std::set<std::string> live;
                for (const Job &j : jobs) {
                    live.insert(j.tag);
                    JobState &st = state[j.tag];
                    const bool due = now >= st.next_due || (poked && j.lookup);
                    if (!due && st.ports == ports) continue;
                    for (uint16_t port : ports) {
                        Op op; op.hash = j.hash; op.port = port; op.left = (j.leecher || j.lookup) ? 1 : 0;
                        op.ev = (j.announce && (!st.registered || st.ports != ports)) ? TE_STARTED : TE_NONE;
                        op.want = j.lookup; op.tag = j.tag;
                        ops.push_back(op);
                    }
                    st.ports = ports;
                    st.registered = st.registered || j.announce;
                    st.next_due = now + (j.lookup ? j.lookup_ms : 240000);
                }
                for (auto it = state.begin(); it != state.end();) it = live.count(it->first) ? std::next(it) : state.erase(it);
                for (auto &w : withdraw_list) { Op op; op.hash = w.first; op.port = w.second; op.left = 0; op.ev = TE_STOPPED; op.want = false; ops.push_back(op); }
                withdraw_list.clear();
            }
            if (ops.empty()) continue;
            tracker_round(ops);
            int ok = 0;
            for (auto &t : trackers) ok += t.answered;
            std::lock_guard<std::mutex> l(m);
            rep.trackers_ok = ok; ++rep.rounds;
            for (const Op &op : ops) {
                if (!op.want || !op.answered) continue;
                auto it = state.find(op.tag);
                if (it == state.end()) continue;
                JobState &st = it->second;
                if (st.found_at != now) { st.found.clear(); st.found_at = now; }
                for (auto &a : op.peers) if (std::find(st.found.begin(), st.found.end(), a) == st.found.end()) st.found.push_back(a);
            }
            for (auto &kv : state) {
                if (kv.second.found_at != now) continue;
                std::string list;
                for (auto &a : kv.second.found) list += " " + ip_text(a.ip) + ":" + std::to_string(a.port);
                std::string &prev = last_list[kv.first];
                if (prev != list) { prev = list; logf("lookup '%s' -> %zu peer(s):%s", kv.first.c_str(), kv.second.found.size(), list.c_str()); }
            }
        }
    }
    std::map<std::string, std::string> last_list;
    // announces that were registered and are no longer wanted (set_jobs removed them)
    std::vector<std::pair<std::vector<uint8_t>, uint16_t>> withdraw_list;

    // Stops the network thread (fast) and says goodbye to the trackers; the router mapping is removed by finish().
    void halt() {
        stop_flag = true;
        cv.notify_all();
        if (net_thread.joinable()) net_thread.join();
        if (tsock >= 0) {
            for (const Job &j : jobs) {
                if (!(j.announce || j.lookup)) continue;
                for (auto &t : trackers) {
                    if (!t.resolved || !t.cid || now_ms() - t.cid_at >= 58000) continue;
                    for (uint16_t port : ports_cache) {
                        auto req = tracker_announce(t.cid, next_tid++, j.hash, peer_id, j.leecher ? 1 : 0, TE_STOPPED, port, 0);
                        sendto(tsock, (const char *)req.data(), (int)req.size(), 0, (sockaddr *)&t.addr, sizeof t.addr);
                    }
                }
            }
            EOD_CLOSESOCK(tsock); tsock = -1;
        }
#ifndef _WIN32
        if (game_fd >= 0) { close(game_fd); game_fd = -1; }
#endif
        running = false;
    }
    void finish() {
        if (map_thread.joinable()) map_thread.join();
        remove_mapping();
    }
};

namespace { std::atomic<int> g_cleanups{0}; }

Online::Online() : d_(std::make_shared<Impl>()) { random_bytes(d_->peer_id, 20); std::memcpy(d_->peer_id, "-EO0001-", 8); }
Online::~Online() { stop(); wait_cleanup(2500); }

bool Online::running() const { return d_->running; }

void Online::start(int game_fd, uint16_t local_port, bool want_mapping) {
    if (d_->running) return;
    std::shared_ptr<Impl> dp = d_;
    Impl &d = *dp;
    // Its own handle on the game socket: stop() returns before the network thread has ended, and the caller
    // closes its socket right after, so the thread must never send on a descriptor number the next socket reuses.
#ifdef _WIN32
    d.game_fd = game_fd;
#else
    d.game_fd = dup(game_fd);
#endif
    d.local_port = local_port; d.stop_flag = false; d.running = true;
    d.rep = Report{};
    d.state.clear(); d.ports_cache.clear(); d.trackers.clear(); d.stun_in.clear();
    d.net_thread = std::thread([dp] { dp->net_thread_main(); });
    if (want_mapping) d.map_thread = std::thread([dp] { dp->map_thread_main(); });
    else { d.rep.map_done = true; }
}

// Called on the game thread, which is also the app's main thread: never wait here. The network thread
// can be inside a DNS lookup or a tracker exchange for seconds, so joining it, the tracker goodbyes and
// the router unmapping all happen on the cleanup thread.
void Online::stop() {
    if (!d_->running) return;
    std::shared_ptr<Impl> old = d_;
    {
        std::lock_guard<std::mutex> l(old->m);
        old->stop_flag = true;
    }
    old->cv.notify_all();
    d_ = std::make_shared<Impl>();
    std::memcpy(d_->peer_id, old->peer_id, 20);
    ++g_cleanups;
    std::thread([old] { old->halt(); old->finish(); --g_cleanups; }).detach();
}
void Online::wait_cleanup(int ms) {
    const uint64_t t0 = now_ms();
    while (g_cleanups > 0 && now_ms() - t0 < (uint64_t)ms) sleep_ms(20);
}

void Online::set_jobs(const std::vector<Job> &jobs) {
    Impl &d = *d_;
    std::lock_guard<std::mutex> l(d.m);
    for (const Job &old : d.jobs) {
        if (!old.announce) continue;
        bool kept = false;
        for (const Job &j : jobs) if (j.tag == old.tag && j.hash == old.hash) kept = true;
        if (!kept) for (uint16_t p : d.ports_cache) d.withdraw_list.push_back({old.hash, p});
    }
    d.jobs = jobs;
    d.cv.notify_all();
}
void Online::poke() { std::lock_guard<std::mutex> l(d_->m); d_->poke_flag = true; d_->cv.notify_all(); }
Report Online::report() const { std::lock_guard<std::mutex> l(d_->m); return d_->rep; }
std::vector<Addr> Online::my_addresses() const {
    std::lock_guard<std::mutex> l(d_->m);
    std::vector<Addr> v;
    if (d_->rep.mapped) v.push_back({d_->rep.ext_ip, d_->rep.ext_port});
    if (d_->rep.stun_ok) { Addr a{d_->rep.pub_ip, d_->rep.pub_port}; if (std::find(v.begin(), v.end(), a) == v.end()) v.push_back(a); }
    return v;
}
std::vector<Addr> Online::found(const std::string &tag) const {
    std::vector<Addr> mine = my_addresses(), out;
    std::lock_guard<std::mutex> l(d_->m);
    auto it = d_->state.find(tag);
    if (it == d_->state.end()) return out;
    for (auto &a : it->second.found) if (std::find(mine.begin(), mine.end(), a) == mine.end()) out.push_back(a);
    return out;
}
uint64_t Online::found_age_ms(const std::string &tag) const {
    std::lock_guard<std::mutex> l(d_->m);
    auto it = d_->state.find(tag);
    if (it == d_->state.end() || !it->second.found_at) return UINT64_MAX;
    return now_ms() - it->second.found_at;
}
void Online::feed_stun(const uint8_t *data, size_t n) {
    std::lock_guard<std::mutex> l(d_->m);
    if (d_->stun_in.size() < 16) d_->stun_in.emplace_back(data, data + n);
    d_->cv.notify_all();
}
std::string Online::log_tail() const {
    std::lock_guard<std::mutex> l(g_log_mutex);
    std::string s;
    for (auto &x : g_log) s += x + "\n";
    return s;
}

}  // namespace netplay_online
