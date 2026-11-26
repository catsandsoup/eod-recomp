#pragma once
#include "../runtime/platform/platform.h"

namespace netplay {
// Sample time at the point a packet passes validation. A long receive queue can
// take longer to drain than the keepalive interval of the entire session.
class PeerActivity {
    uint64_t last_heard_ = 0;

public:
    void heard() { last_heard_ = platform_time_us(); }
    bool expired() const {
        constexpr uint64_t timeout_us = 5000000;
        const uint64_t now = platform_time_us();
        return now > last_heard_ && now - last_heard_ > timeout_us;
    }
};
}
