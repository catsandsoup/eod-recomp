#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <netinet/in.h>
#endif

namespace netplay {

// One outbound game socket carries signalling and all relayed peers. A
// virtual peer address identifies one room member in the existing host-star
// session; it is never passed to the OS as a datagram destination.
class GarageRelay {
public:
    static bool configured();
    sockaddr_in peer(uint16_t route) const;
    static bool relayed(const sockaddr_in &addr);

    void start(const std::string &secret, bool host);
    void stop(int fd);
    void poll(int fd, uint64_t now);
    bool receive(const sockaddr_in &source, uint8_t *packet, int &size, sockaddr_in &peer_addr, uint64_t now);
    void send(int fd, const sockaddr_in &destination, const std::vector<uint8_t> &packet) const;

    void publish(int fd, const std::vector<std::string> &secrets, uint8_t players) const;
    void query(int fd, const std::string &secret) const;
    bool presence(const std::string &secret, uint8_t &players) const;
    bool watching() const { return active() && key_.empty(); }
    bool active() const { return bool(resolver_); }
    bool ready() const { return ready_since_ != 0; }
    bool choose_relay(uint64_t now) const;
    bool matches_candidate(const sockaddr_in &addr) const;
    bool take_candidate(sockaddr_in &out);
    std::string join_attempt() const;
    void admit(const std::string &attempt, uint64_t now);
    void welcomed(int fd) const { transmit(fd, 'F', nullptr, 0); }
    bool has_joining_guest(uint64_t now) const;
    char error() const { return error_; }

private:
    struct Resolution;
    std::shared_ptr<Resolution> resolver_;
    sockaddr_in server_{}, candidate_{};
    uint32_t generation_ = 0; // network byte order, issued by the room
    std::array<uint8_t, 16> token_{}, cookie_{};
    std::vector<uint8_t> key_, presence_key_;
    uint8_t presence_players_ = 0;
    uint64_t sent_at_ = 0, ready_since_ = 0, last_reply_ = 0, retry_dns_at_ = 0;
    bool host_ = false, force_ = false, pending_candidate_ = false;
    struct Admission { uint64_t until; bool finished; };
    std::map<std::string, Admission> admissions_;
    char error_ = 0;
    void resolve();
    void transmit(int fd, char kind, const uint8_t *payload, size_t size) const;
};

}
