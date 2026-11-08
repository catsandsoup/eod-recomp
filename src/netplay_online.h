#pragma once
// Garage Code internet layer (worker threads, own sockets except the game socket, which is only
// written to for STUN; incoming STUN replies are fed in by the session's poll()).
//   * STUN from the game socket: the public address and port the router gave it (and whether the
//     mapping depends on the destination = symmetric NAT, which hole punching cannot cross).
//   * NAT-PMP / PCP / UPnP IGD: asks the router to forward the game port when it allows it.
//   * BitTorrent UDP trackers: the host announces the join code's hash, joiners announce theirs and
//     look the host up. No account, no server of ours, no game data on the tracker.
// Design, wire details and honest limits: docs/NETPLAY_INTERNET.md.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct sockaddr_in;

namespace netplay_online {

struct Addr {
    uint32_t ip = 0; uint16_t port = 0;
    bool operator==(const Addr &o) const { return ip == o.ip && port == o.port; }
};

struct Job {
    std::string tag;                 // results are read back by tag
    std::vector<uint8_t> hash;       // 20-byte rendezvous key
    bool announce = false;           // stay registered under the hash (refreshed every 4 minutes)
    bool leecher = false;            // announce as a downloader (joiners) instead of a seeder (hosts)
    bool lookup = false;             // also fetch who else is registered, every lookup_ms
    uint32_t lookup_ms = 5000;
};

struct Report {
    bool stun_done = false, stun_ok = false, symmetric = false;
    uint32_t pub_ip = 0; uint16_t pub_port = 0; int stun_replies = 0;
    bool map_done = false, mapped = false; std::string map_kind, map_note; uint16_t ext_port = 0; uint32_t ext_ip = 0;
    int trackers_total = 0, trackers_ok = 0, rounds = 0;
    bool internet() const { return stun_ok || trackers_ok > 0; }
};

class Online {
public:
    Online();
    ~Online();
    // game_fd: the session's UDP socket (STUN requests are sent from it). want_mapping: ask the router.
    void start(int game_fd, uint16_t local_port, bool want_mapping);
    static void wait_cleanup(int ms);             // process exit: let router-mapping removal finish
    void stop();                                  // sends "stopped" to the trackers, removes the router mapping (bounded)
    bool running() const;
    void set_jobs(const std::vector<Job> &jobs);
    void poke();                                  // look everything up again now
    Report report() const;
    std::vector<Addr> found(const std::string &tag) const;     // peers registered under the job's hash, minus ourselves
    uint64_t found_age_ms(const std::string &tag) const;       // since the last successful lookup (UINT64_MAX = never)
    std::vector<Addr> my_addresses() const;       // what others can reach: mapped port, STUN port
    void feed_stun(const uint8_t *data, size_t n);// from the session's receive loop
    std::string log_tail() const;                 // last few diagnostic lines (stderr gets them too)
private:
    struct Impl;
    std::shared_ptr<Impl> d_;
};

}  // namespace netplay_online
