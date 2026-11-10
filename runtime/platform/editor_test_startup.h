#pragma once
// Editor test-drive presentation only. Asset loading and native initialization
// still run in full; readiness follows the ordered native race drawing stream.
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdint>

namespace editor_test_startup {
inline bool requested(const char *editor, const char *direct) {
    return editor && std::strcmp(editor, "1") == 0 && direct && *direct;
}

// EE-side semantic gate. The generic session loop draws loading and interlude
// states too; a gameplay viewport draw with native full-screen fade alpha zero
// must finish before native frame publication. No host-time/frame-count delay.
class NativeFrameGate {
public:
    void begin_draw(bool race_loop) {
        drawing_ = race_loop;
        gameplay_ = pending_ = false;
    }
    void gameplay_drawn(bool valid_viewports, uint32_t fade_alpha) {
        if (drawing_ && valid_viewports && fade_alpha == 0) gameplay_ = true;
    }
    void end_draw() {
        pending_ = drawing_ && gameplay_;
        drawing_ = false;
    }
    bool frame_published(bool race_loop) {
        if (!race_loop || !pending_ || marked_) return false;
        marked_ = true;
        return true;
    }
private:
    bool drawing_ = false, gameplay_ = false, pending_ = false, marked_ = false;
};

class State {
public:
    explicit State(bool enabled) : enabled_(enabled) {}
    bool enabled() const { return enabled_; }
    bool ready() const { return phase_.load() == Presented; }
    bool hide_window(bool headless) const { return enabled_ && !headless && !ready(); }
    // GS command consumer, after the native race draw commands ahead of it.
    bool race_submitted(uint64_t drawn) {
        if (!enabled_ || phase_.load() != Waiting) return false;
        submitted_drawn_ = drawn;
        return advance(Waiting, Submitted);
    }
    // Software composition publishes a fresh image before the main-thread SDL
    // presenter can acknowledge it. An old loading image must not reveal it.
    bool software_composed(uint64_t drawn) { return drawn > submitted_drawn_ && advance(Submitted, Composed); }
    bool software_presented() { return advance(Composed, Presented); }
    // A cached redraw after submission may still contain the loading image.
    // Display changes must follow the marker; overlay-only presents cannot arm it.
    // Only a fresh scanout composition may arm the hardware acknowledgement.
    void hardware_display_changed(uint64_t drawn) { if (drawn > submitted_drawn_) advance(Submitted, ScanoutChanged); }
    bool hardware_composed() { return advance(ScanoutChanged, Composed); }
    bool hardware_presented() { return advance(Composed, Presented); }
    // Snapshot claims do not acknowledge a visible presentation. A failed write
    // consumes the one attempt as well, keeping preview work bounded.
    bool claim_preview(bool offscreen) {
        const Phase phase = phase_.load();
        const bool eligible = phase == Presented ||
            (offscreen && phase == Composed);
        return enabled_ && eligible && !preview_claimed_.exchange(true);
    }
private:
    enum Phase { Waiting, Submitted, ScanoutChanged, Composed, Presented };
    bool advance(Phase from, Phase to) {
        return enabled_ && phase_.compare_exchange_strong(from, to);
    }
    const bool enabled_;
    std::atomic<Phase> phase_{Waiting};
    std::atomic<bool> preview_claimed_{false};
    // GS-owned: updated by the same command consumer that composes scanout.
    uint64_t submitted_drawn_ = 0;
};

inline State &state() {
    static State startup(requested(std::getenv("EOD_EDITOR_TEST"), std::getenv("EOD_DIRECT_START")));
    return startup;
}
} // namespace editor_test_startup
