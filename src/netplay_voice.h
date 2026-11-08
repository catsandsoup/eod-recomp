#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace netplay::voice {
constexpr unsigned SampleRate=48000, FrameSamples=960, MaxPeers=16;
constexpr unsigned HeaderBytes=19, MaxPayloadBytes=400, MaxPacketBytes=HeaderBytes+MaxPayloadBytes;
constexpr unsigned JitterFrames=8, StartupFrames=2;
using Frame=std::array<int16_t,FrameSamples>;
struct Packet { std::array<uint8_t,MaxPacketBytes> bytes{}; size_t size=0; };
enum class Receive { Accepted, Inactive, Invalid, WrongSession, WrongSender, Duplicate, Stale, OutsideWindow };
enum class Playback { Silence, Priming, Decoded, Concealed };

// Single owning voice worker thread; callers serialize commands from UI/network.
// No sockets/devices/accounts. The authenticated outer connection supplies sender.
// Capture/playout cadence is 20ms wall time, independent of simulation ticks.
class Session {
public:
    Session(); ~Session();
    Session(const Session&)=delete; Session& operator=(const Session&)=delete;
    static bool available();
    bool start(uint64_t nonce,uint32_t epoch,uint8_t local_slot);
    void stop();
    bool active() const;
    void set_transmit_enabled(bool); // opt-in; start/stop always turn this off
    void set_microphone_muted(bool);
    void set_deafened(bool); // receive-only preference; independent of mic mute
    void set_peer_muted(uint8_t,bool);
    bool transmit_enabled() const;
    bool microphone_muted() const;
    bool deafened() const;
    bool peer_muted(uint8_t) const;
    // Call every capture interval, including muted intervals. Empty packet means
    // intentionally not transmitted or codec error. Never stores captured PCM.
    bool encode(const Frame&,Packet&);
    Receive receive(uint8_t authenticated_sender,const uint8_t*,size_t);
    // Call once per remote peer every 20ms, even muted/deafened. Always writes PCM.
    Playback playout(uint8_t peer,Frame&);
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
}
