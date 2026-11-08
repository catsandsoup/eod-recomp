// Developer probe (not a ctest): exercises the internet layer against the real STUN servers, trackers and router.
//   netplay_online_probe host CODE [port]    announce CODE and show who joins
//   netplay_online_probe join CODE [port]    look CODE up
//   netplay_online_probe stun [port]         only the public address / NAT type / router mapping
#include "netplay_online.h"
#include "netplay_online_wire.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <thread>
#include <chrono>
using namespace netplay_online;
int main(int argc, char **argv) {
    std::string mode = argc > 1 ? argv[1] : "stun", code = argc > 2 ? argv[2] : "";
    uint16_t port = (uint16_t)(argc > 3 ? atoi(argv[3]) : (mode == "stun" && argc > 2 ? atoi(argv[2]) : 27015));
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(port);
    if (bind(fd, (sockaddr *)&a, sizeof a)) { perror("bind"); return 1; }
    Online on;
    on.start(fd, port, true);
    std::vector<Job> jobs;
    if (mode == "host") {
        jobs.push_back({"host", rendezvous_hash('H', code), true, false, false, 5000});
        jobs.push_back({"joiners", rendezvous_hash('J', code), false, false, true, 5000});
    } else if (mode == "join") {
        jobs.push_back({"me", rendezvous_hash('J', code), true, true, false, 5000});
        jobs.push_back({"hosts", rendezvous_hash('H', code), false, false, true, 3000});
    }
    on.set_jobs(jobs);
    for (int i = 0; i < 120; ++i) {
        uint8_t buf[1500];
        for (int n; (n = (int)recv(fd, buf, sizeof buf, 0)) > 0;) if (looks_like_stun(buf, n)) on.feed_stun(buf, n);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (i % 8 == 7) {
            Report r = on.report();
            std::printf("t=%ds stun=%d pub=%s:%u sym=%d mapped=%d(%s ext %u) note='%s' trackers %d/%d rounds %d\n", (i + 1) / 4, r.stun_ok, ip_text(r.pub_ip).c_str(), r.pub_port, r.symmetric,
                        r.mapped, r.map_kind.c_str(), r.ext_port, r.map_note.c_str(), r.trackers_ok, r.trackers_total, r.rounds);
            for (auto &j : jobs) if (j.lookup) { auto f = on.found(j.tag); std::printf("   %s:", j.tag.c_str()); for (auto &x : f) std::printf(" %s:%u", ip_text(x.ip).c_str(), x.port); std::printf("\n"); }
            std::fflush(stdout);
        }
        if (mode == "stun" && i > 24) break;
    }
    on.stop();
    Online::wait_cleanup(4000);
    return 0;
}
