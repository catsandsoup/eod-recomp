#include "netplay_relay.h"
#include "netplay_online_wire.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

#ifndef EOD_RELAY_ENDPOINT
#define EOD_RELAY_ENDPOINT ""
#endif

namespace netplay {
namespace {
constexpr size_t kHeader = 21;
constexpr uint64_t kAdmissionUs = 10000000;
const char *endpoint() {
    const char *override = std::getenv("EOD_NETPLAY_RELAY");
    return override ? override : EOD_RELAY_ENDPOINT;
}
bool equal(const sockaddr_in &a, const sockaddr_in &b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}
}

struct GarageRelay::Resolution {
    sockaddr_in addr{};
    std::atomic<bool> done{false};
};

bool GarageRelay::configured() { return *endpoint() != 0; }

std::string GarageRelay::join_attempt() const {
    const auto hash = netplay_online::rendezvous_hash('A', netplay_online::hex(token_.data(), token_.size()));
    return netplay_online::hex(hash.data(), 16);
}

void GarageRelay::admit(const std::string &attempt, uint64_t now) {
    if (host_ && ready() && attempt.size() == 32)
        admissions_[attempt] = {now + kAdmissionUs, true};
}

bool GarageRelay::has_joining_guest(uint64_t now) const {
    for (const auto &entry : admissions_)
        if (!entry.second.finished && now < entry.second.until) return true;
    return false;
}

sockaddr_in GarageRelay::peer(uint16_t route) const {
    sockaddr_in addr{};
    addr.sin_family = AF_UNSPEC;
    addr.sin_addr.s_addr = generation_;
    addr.sin_port = htons(route);
    return addr;
}

bool GarageRelay::relayed(const sockaddr_in &addr) {
    return addr.sin_family == AF_UNSPEC && addr.sin_port != 0;
}

void GarageRelay::start(const std::string &secret, bool host) {
    *this = {};
    host_ = host;
    force_ = std::getenv("EOD_NETPLAY_FORCE_RELAY") != nullptr;
    if (!secret.empty()) key_ = netplay_online::rendezvous_hash('R', secret);
    std::random_device random;
    for (auto &byte : token_) byte = static_cast<uint8_t>(random());
    resolve();
}

void GarageRelay::resolve() {
    resolver_ = std::make_shared<Resolution>();
    // Resolution may outlive cancel/retry. The worker owns only its result,
    // never the session socket or the next attempt's state.
    std::thread([result = resolver_, target = std::string(endpoint())] {
        const size_t colon = target.find(':');
        const auto name = target.substr(0, colon);
        const auto port = colon == std::string::npos ? "27016" : target.substr(colon + 1);
        addrinfo hints{}, *addresses = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_flags = AI_NUMERICSERV;
        if (!name.empty() && getaddrinfo(name.c_str(), port.c_str(), &hints, &addresses) == 0) {
            if (addresses) std::memcpy(&result->addr, addresses->ai_addr, sizeof(result->addr));
            freeaddrinfo(addresses);
        }
        result->done.store(true);
    }).detach();
}

void GarageRelay::transmit(int fd, char kind, const uint8_t *payload, size_t size) const {
    if (fd < 0 || !server_.sin_port || size > 1506) return;
    // Avoid heap allocation on the gameplay send path.
    std::array<uint8_t, kHeader + 1506> packet;
    std::memcpy(packet.data(), "EOR1", 4);
    packet[4] = static_cast<uint8_t>(kind);
    std::copy(token_.begin(), token_.end(), packet.begin() + 5);
    if (size) std::memcpy(packet.data() + kHeader, payload, size);
    sendto(fd, reinterpret_cast<const char *>(packet.data()), static_cast<int>(kHeader + size), 0,
           reinterpret_cast<const sockaddr *>(&server_), sizeof(server_));
}

void GarageRelay::stop(int fd) {
    if (active()) transmit(fd, 'X', nullptr, 0);
    *this = {};
}

void GarageRelay::poll(int fd, uint64_t now) {
    if (!active() || !resolver_->done.load()) return;
    for (auto it = admissions_.begin(); it != admissions_.end();) {
        if (now >= it->second.until) it = admissions_.erase(it);
        else ++it;
    }
    server_ = resolver_->addr;
    if (!server_.sin_port) {
        error_ = 'U';
        if (!retry_dns_at_) retry_dns_at_ = now + 5000000;
        if (now >= retry_dns_at_) { retry_dns_at_ = 0; resolve(); }
        return;
    }
    if (watching()) return;
    if (ready() && now - last_reply_ > 5000000) {
        // Refresh the lease if the relay restarted or missed several keepalives.
        ready_since_ = sent_at_ = 0;
        cookie_.fill(0);
    }
    if (sent_at_ && now - sent_at_ < (ready() ? 2000000u : 500000u)) return;
    sent_at_ = now;
    if (ready()) {
        transmit(fd, 'K', nullptr, 0);
    } else {
        std::array<uint8_t, 36> request;
        std::copy(key_.begin(), key_.end(), request.begin());
        std::copy(cookie_.begin(), cookie_.end(), request.begin() + 20);
        transmit(fd, host_ ? 'H' : force_ ? 'j' : 'J', request.data(), request.size());
    }
}

bool GarageRelay::receive(const sockaddr_in &source, uint8_t *packet, int &size, sockaddr_in &peer_addr, uint64_t now) {
    if (!active() || !server_.sin_port || !equal(server_, source)) return false;
    const int received = size;
    size = 0;
    if (received < static_cast<int>(kHeader) || std::memcmp(packet, "EOR1", 4) ||
        !std::equal(token_.begin(), token_.end(), packet + 5)) return true;
    auto body = packet + kHeader;
    const size_t n = static_cast<size_t>(received) - kHeader;
    presence_key_.clear();
    switch (packet[4]) {
    case 'Q':
        if (watching() && n == 21) {
            presence_key_.assign(body, body + 20);
            presence_players_ = body[20];
        }
        break;
    case 'C':
        if (n == cookie_.size() && !ready()) {
            std::copy_n(body, n, cookie_.begin());
            sent_at_ = 0;
        }
        break;
    case 'R':
        if (n == 4 && (!ready() || std::memcmp(&generation_, body, 4) == 0)) {
            if (std::memcmp(&generation_, body, 4) != 0) admissions_.clear();
            std::memcpy(&generation_, body, 4);
            if (!ready()) ready_since_ = now;
            last_reply_ = now;
            error_ = 0;
        }
        break;
    case 'N':
        if (host_ && ready() && n == 22 && std::memcmp(&generation_, body, 4) == 0) {
            const std::string attempt = netplay_online::hex(body + 4, 16);
            const uint64_t remaining = static_cast<uint64_t>(body[20] << 8 | body[21]) * 1000;
            if (!remaining) admit(attempt, now);
            else if (remaining <= kAdmissionUs) {
                auto inserted = admissions_.emplace(attempt, Admission{now + remaining, false});
                auto &entry = inserted.first->second;
                if (!entry.finished) entry.until = std::min(entry.until, now + remaining);
            }
        }
        break;
    case 'E':
        if (n == 1 && !ready()) error_ = static_cast<char>(body[0]);
        break;
    case 'P':
        if (n == 6 && !force_) {
            candidate_.sin_family = AF_INET;
            std::memcpy(&candidate_.sin_addr, body, 4);
            std::memcpy(&candidate_.sin_port, body + 4, 2);
            pending_candidate_ = candidate_.sin_port != 0;
        }
        break;
    case 'D':
        if (n >= 10 && n <= 1506 && ready() && std::memcmp(body, &generation_, 4) == 0) {
            const uint16_t route = static_cast<uint16_t>(body[4] << 8 | body[5]);
            if (host_ ? route < 2 : route != 1) break;
            peer_addr = peer(route);
            size = static_cast<int>(n - 6);
            std::memmove(packet, body + 6, size);
        }
        break;
    }
    return true;
}

void GarageRelay::send(int fd, const sockaddr_in &destination, const std::vector<uint8_t> &packet) const {
    if (!ready() || destination.sin_addr.s_addr != generation_ || packet.size() > 1500) return;
    std::array<uint8_t, 1506> body;
    std::memcpy(body.data(), &generation_, 4);
    std::memcpy(body.data() + 4, &destination.sin_port, 2);
    std::copy(packet.begin(), packet.end(), body.begin() + 6);
    transmit(fd, 'D', body.data(), packet.size() + 6);
}

void GarageRelay::publish(int fd, const std::vector<std::string> &secrets, uint8_t players) const {
    std::vector<uint8_t> body{players};
    for (const auto &secret : secrets) {
        auto key = netplay_online::rendezvous_hash('R', secret);
        body.insert(body.end(), key.begin(), key.end());
    }
    transmit(fd, 'A', body.data(), body.size());
}
void GarageRelay::query(int fd, const std::string &secret) const {
    const auto key = netplay_online::rendezvous_hash('R', secret);
    transmit(fd, 'Q', key.data(), key.size());
}
bool GarageRelay::presence(const std::string &secret, uint8_t &players) const {
    if (presence_key_.empty() || netplay_online::rendezvous_hash('R', secret) != presence_key_) return false;
    players = presence_players_;
    return true;
}

bool GarageRelay::choose_relay(uint64_t now) const {
    return ready() && (force_ || now - ready_since_ >= 1200000);
}
bool GarageRelay::matches_candidate(const sockaddr_in &addr) const {
    return candidate_.sin_port && equal(candidate_, addr);
}
bool GarageRelay::take_candidate(sockaddr_in &out) {
    if (!pending_candidate_) return false;
    pending_candidate_ = false;
    out = candidate_;
    return true;
}
}
