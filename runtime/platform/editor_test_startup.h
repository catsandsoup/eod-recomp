#pragma once
// Editor test-drive presentation only. Asset loading and native initialization
// still run in full; readiness follows the ordered native race drawing stream.
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace editor_test_startup {
inline bool requested(const char *editor, const char *direct) {
    return editor && std::strcmp(editor, "1") == 0 && direct && *direct;
}

class State {
public:
    explicit State(bool enabled) : enabled_(enabled) {}
    bool enabled() const { return enabled_; }
    bool ready() const { return phase_.load() == Presented; }
    bool hide_window(bool headless) const { return enabled_ && !headless && !ready(); }
    // GS command consumer, after the native race draw commands ahead of it.
    void race_submitted() { advance(Waiting, Submitted); }
    // Software composition publishes a fresh image before the main-thread SDL
    // presenter can acknowledge it. An old loading image must not reveal it.
    void software_composed() { advance(Submitted, Composed); }
    void software_presented() { advance(Composed, Presented); }
    // Hardware presenter acknowledges only after presenting the composed image.
    void hardware_presented() { advance(Submitted, Presented); }
private:
    enum Phase { Waiting, Submitted, Composed, Presented };
    void advance(Phase from, Phase to) {
        if (enabled_) phase_.compare_exchange_strong(from, to);
    }
    const bool enabled_;
    std::atomic<Phase> phase_{Waiting};
};

inline State &state() {
    static State startup(requested(std::getenv("EOD_EDITOR_TEST"), std::getenv("EOD_DIRECT_START")));
    return startup;
}
} // namespace editor_test_startup
