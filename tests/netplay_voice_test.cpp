#ifdef NDEBUG
#undef NDEBUG
#endif
#include "netplay_voice.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace netplay::voice;
namespace {
constexpr uint64_t nonce=0x1020304050607080ull;
Frame input(unsigned index){Frame f{};for(unsigned i=0;i<FrameSamples;++i)f[i]=int16_t(10000*std::sin((index*FrameSamples+i)*0.0575958653));return f;}
bool silent(const Frame& f){return std::all_of(f.begin(),f.end(),[](int16_t v){return v==0;});}
[[maybe_unused]] void seq(Packet& p,uint32_t n){for(unsigned i=0;i<4;++i)p.bytes[18-i]=uint8_t(n>>(8*i));}
[[maybe_unused]] void prime(Session& s){Frame out;for(unsigned i=0;i<StartupFrames;++i)assert(s.playout(0,out)==Playback::Priming&&silent(out));}
[[maybe_unused]] void lifecycle(){
 Session tx,rx;Packet p;Frame out;
 assert(!tx.encode(input(0),p)&&p.size==0);assert(!tx.start(0,1,0));assert(!tx.start(nonce,1,MaxPeers));
 assert(tx.start(nonce,1,0)&&rx.start(nonce,1,1));assert(!tx.transmit_enabled());
 assert(!tx.encode(input(0),p));tx.set_transmit_enabled(true);assert(tx.encode(input(1),p));
 assert(p.size<=MaxPacketBytes&&p.bytes[0]==0&&p.bytes[1]==1&&p.bytes[2]==0x10&&p.bytes[9]==0x80);
 assert(rx.receive(0,p.bytes.data(),p.size)==Receive::Accepted);prime(rx);assert(rx.playout(0,out)==Playback::Decoded&&!silent(out));
 tx.set_microphone_muted(true);assert(!tx.encode(input(2),p)&&p.size==0);tx.set_microphone_muted(false);assert(tx.encode(input(3),p));
 rx.stop();out.fill(42);assert(rx.playout(0,out)==Playback::Silence&&silent(out));assert(rx.receive(0,p.bytes.data(),p.size)==Receive::Inactive);
 assert(rx.start(nonce,2,1));assert(rx.receive(0,p.bytes.data(),p.size)==Receive::WrongSession);
 assert(tx.start(nonce,2,0)&&!tx.transmit_enabled());tx.set_transmit_enabled(true);assert(tx.encode(input(0),p));assert(p.bytes[18]==0);
 assert(rx.receive(0,p.bytes.data(),p.size)==Receive::Accepted);prime(rx);assert(rx.playout(0,out)==Playback::Decoded);
}
[[maybe_unused]] void reorder_loss_and_bounds(){
 Session tx,ordered,reordered;assert(tx.start(nonce,1,0)&&ordered.start(nonce,1,1)&&reordered.start(nonce,1,1));tx.set_transmit_enabled(true);
 std::array<Packet,JitterFrames+2> packets{};for(unsigned i=0;i<packets.size();++i)assert(tx.encode(input(i),packets[i]));
 for(unsigned i=0;i<3;++i)assert(ordered.receive(0,packets[i].bytes.data(),packets[i].size)==Receive::Accepted);
 for(unsigned i:{0u,2u,1u})assert(reordered.receive(0,packets[i].bytes.data(),packets[i].size)==Receive::Accepted);
 assert(reordered.receive(0,packets[1].bytes.data(),packets[1].size)==Receive::Duplicate);
 assert(reordered.receive(0,packets[JitterFrames].bytes.data(),packets[JitterFrames].size)==Receive::OutsideWindow);
 prime(ordered);prime(reordered);Frame a,b;
 for(unsigned i=0;i<3;++i){assert(ordered.playout(0,a)==Playback::Decoded);assert(reordered.playout(0,b)==Playback::Decoded);assert(a==b);}
 assert(reordered.receive(0,packets[0].bytes.data(),packets[0].size)==Receive::Stale);
 assert(reordered.playout(0,b)==Playback::Concealed);
 assert(reordered.receive(0,packets[4].bytes.data(),packets[4].size)==Receive::Accepted);
 assert(reordered.playout(0,b)==Playback::Decoded);
 // Bounded queue accepts JitterFrames, rejects next; modular sequence wraps correctly.
 Session wrap;assert(wrap.start(nonce,1,1));
 for(unsigned i=0;i<JitterFrames;++i){seq(packets[i],0xfffffffeu+i);assert(wrap.receive(0,packets[i].bytes.data(),packets[i].size)==Receive::Accepted);}
 seq(packets[JitterFrames],0xfffffffeu+JitterFrames);assert(wrap.receive(0,packets[JitterFrames].bytes.data(),packets[JitterFrames].size)==Receive::OutsideWindow);prime(wrap);
 for(unsigned i=0;i<JitterFrames;++i)assert(wrap.playout(0,b)==Playback::Decoded);
 assert(wrap.receive(0,packets[JitterFrames].bytes.data(),packets[JitterFrames].size)==Receive::Accepted);
}
[[maybe_unused]] void invalid_packets(){
 Session tx,rx;assert(tx.start(nonce,9,0)&&rx.start(nonce,9,1));tx.set_transmit_enabled(true);Packet p;assert(tx.encode(input(0),p));
 assert(rx.receive(0,nullptr,p.size)==Receive::Invalid);
 for(size_t n=0;n<=HeaderBytes;++n)assert(rx.receive(0,p.bytes.data(),n)==Receive::Invalid);
 assert(rx.receive(0,p.bytes.data(),MaxPacketBytes+1)==Receive::Invalid);
 auto q=p;q.bytes[1]=2;assert(rx.receive(0,q.bytes.data(),q.size)==Receive::Invalid);
 q=p;q.bytes[2]^=1;assert(rx.receive(0,q.bytes.data(),q.size)==Receive::WrongSession);
 q=p;q.bytes[14]^=1;assert(rx.receive(0,q.bytes.data(),q.size)==Receive::WrongSession);
 assert(rx.receive(2,p.bytes.data(),p.size)==Receive::WrongSender);assert(rx.receive(MaxPeers,p.bytes.data(),p.size)==Receive::WrongSender);
 q=p;q.bytes[10]=1;assert(rx.receive(1,q.bytes.data(),q.size)==Receive::WrongSender);
 // TOC: stereo is forbidden, a valid40ms packet is forbidden, malformed frame count is forbidden.
 q=p;q.bytes[HeaderBytes]|=4;assert(rx.receive(0,q.bytes.data(),q.size)==Receive::Invalid);
 q=p;q.bytes[HeaderBytes]=0x10;assert(rx.receive(0,q.bytes.data(),q.size)==Receive::Invalid);
 q=p;q.bytes[HeaderBytes]=3;q.bytes[HeaderBytes+1]=0;q.size=HeaderBytes+2;assert(rx.receive(0,q.bytes.data(),q.size)==Receive::Invalid);
 // Mutations cannot corrupt bounded parser state; sanitizer exercises short/bad payloads.
 std::mt19937 rng(123);for(unsigned i=0;i<5000;++i){q=p;q.size=HeaderBytes+1+rng()%MaxPayloadBytes;for(size_t j=HeaderBytes;j<q.size;++j)q.bytes[j]=uint8_t(rng());rx.receive(0,q.bytes.data(),q.size);}
}
[[maybe_unused]] void separate_peers(){
 Session a,b,rx;assert(a.start(nonce,1,0)&&b.start(nonce,1,2)&&rx.start(nonce,1,1));
 a.set_transmit_enabled(true);b.set_transmit_enabled(true);Packet pa,pb;Frame out;
 assert(a.encode(input(0),pa)&&b.encode(input(5),pb));
 assert(rx.receive(0,pa.bytes.data(),pa.size)==Receive::Accepted);
 assert(rx.receive(2,pb.bytes.data(),pb.size)==Receive::Accepted);
 prime(rx);for(unsigned j=0;j<StartupFrames;++j)assert(rx.playout(2,out)==Playback::Priming);
 rx.set_peer_muted(0,true);assert(rx.playout(0,out)==Playback::Concealed&&silent(out));
 assert(rx.playout(2,out)==Playback::Decoded&&!silent(out));
 assert(rx.receive(0,pb.bytes.data(),pb.size)==Receive::WrongSender);
}
[[maybe_unused]] void mute_and_deafen(){
 Session tx,rx;assert(tx.start(nonce,1,0)&&rx.start(nonce,1,1));tx.set_transmit_enabled(true);Packet p;Frame out;
 assert(tx.encode(input(0),p));assert(rx.receive(0,p.bytes.data(),p.size)==Receive::Accepted);prime(rx);
 rx.set_peer_muted(0,true);assert(rx.peer_muted(0));rx.playout(0,out);assert(silent(out));
 assert(tx.encode(input(1),p));assert(rx.receive(0,p.bytes.data(),p.size)==Receive::Accepted);assert(rx.playout(0,out)==Playback::Decoded&&silent(out));
 rx.set_peer_muted(0,false);assert(tx.encode(input(2),p));assert(rx.receive(0,p.bytes.data(),p.size)==Receive::Accepted);assert(rx.playout(0,out)==Playback::Decoded&&!silent(out));
 rx.set_deafened(true);assert(rx.deafened());assert(tx.encode(input(3),p));assert(rx.receive(0,p.bytes.data(),p.size)==Receive::Accepted);assert(rx.playout(0,out)==Playback::Decoded&&silent(out));
 rx.set_deafened(false);assert(tx.encode(input(4),p));assert(rx.receive(0,p.bytes.data(),p.size)==Receive::Accepted);assert(rx.playout(0,out)==Playback::Decoded&&!silent(out));
 // Deafen and microphone/transmit preferences are independent, explicitly documented.
 tx.set_deafened(true);assert(tx.encode(input(5),p));tx.set_microphone_muted(true);assert(!tx.encode(input(6),p));
 rx.set_peer_muted(0,true);rx.stop();assert(!rx.peer_muted(0));assert(rx.start(nonce,2,1));assert(rx.playout(0,out)==Playback::Silence&&silent(out));
}
}
#ifdef EOD_HAVE_OPUS
void quiet_microphone_playback() {
    Session tx, rx;
    assert(tx.start(nonce, 1, 0) && rx.start(nonce, 1, 1));
    tx.set_transmit_enabled(true);
    Packet packet;
    Frame decoded;
    bool speaking = true;
    assert(rx.playout(0, decoded, &speaking) == Playback::Silence && !speaking);
    for (unsigned i = 0; i < 25; ++i) {
        Frame mic = input(i);
        for (auto &sample : mic) sample /= 12;
        assert(tx.encode(mic, packet));
        assert(rx.receive(0, packet.bytes.data(), packet.size) == Receive::Accepted);
        if (i == 0)
            for (unsigned j = 0; j < StartupFrames; ++j) assert(rx.playout(0, decoded) == Playback::Priming);
        assert(rx.playout(0, decoded, &speaking) == Playback::Decoded);
    }
    assert(speaking);
    int peak = 0;
    for (auto sample : decoded) peak = std::max(peak, std::abs(int(sample)));
    assert(peak > 4000 && peak < 20000);
    for (unsigned i = 0; i < 20; ++i) {
        assert(tx.encode(Frame{}, packet));
        assert(rx.receive(0, packet.bytes.data(), packet.size) == Receive::Accepted);
        assert(rx.playout(0, decoded, &speaking) == Playback::Decoded);
    }
    assert(!speaking);
    rx.set_peer_muted(0, true);
    assert(tx.encode(input(0), packet));
    assert(rx.receive(0, packet.bytes.data(), packet.size) == Receive::Accepted);
    assert(rx.playout(0, decoded, &speaking) == Playback::Decoded && !speaking && silent(decoded));
}
void resumes_after_silence() {
 using namespace netplay::voice;
 Session tx,rx; assert(tx.start(nonce,1,0)&&rx.start(nonce,1,1));
 tx.set_transmit_enabled(true); Packet p; Frame out;
 assert(tx.encode(input(0),p)); assert(rx.receive(0,p.bytes.data(),p.size)==Receive::Accepted);
 for(unsigned i=0;i<80;++i)rx.playout(0,out);
 assert(tx.encode(input(1),p)); assert(rx.receive(0,p.bytes.data(),p.size)==Receive::Accepted);
 bool decoded=false;
 for(unsigned i=0;i<8;++i)if(rx.playout(0,out)==Playback::Decoded)decoded=!silent(out);
 assert(decoded);
}
#endif
void stereo_voice_balance() {
 using namespace netplay::voice;
 StereoMixer mixer;
 Frame speech{}; speech.fill(7000);
 std::array<int16_t, FrameSamples * 2> bed{}, stereo{};
 for (size_t i = 0; i < FrameSamples; ++i) { bed[2*i] = 10000; bed[2*i+1] = -7000; }
 for (int level : {0, 50, 100, 200}) {
  stereo = bed;
  mixer.mix(stereo.data(), FrameSamples, speech.data(), FrameSamples, 100, level, false);
  const int voice = 7000 * level / 100;
  assert(stereo[0] == 10000 + voice && stereo[1] == -7000 + voice);
 }
 stereo = bed;
 mixer.mix(stereo.data(), FrameSamples, speech.data(), FrameSamples, 50, 100, false);
 assert(stereo[0] == 13500 && stereo[1] == -3500);
 stereo = bed;
 mixer.mix(stereo.data(), FrameSamples, speech.data(), FrameSamples, 100, 100, true);
 assert(stereo.back() == -1120 + 7000);
 // Silence releases smoothly, even when there are no further remote packets.
 for (int i=0; i<30; ++i) {
  stereo = bed;
  mixer.mix(stereo.data(), FrameSamples, nullptr, 0, 100, 100, true);
 }
 assert(stereo == bed);
 speech.fill(32000); stereo.fill(12000);
 mixer.mix(stereo.data(), FrameSamples, speech.data(), FrameSamples, 100, 200, false);
 assert(stereo[0] < 32767 && stereo[0] > 30000);
 stereo = bed;
 mixer.mix(stereo.data(), FrameSamples, speech.data(), FrameSamples, 100, 0, true);
 assert(stereo == bed);
}
void full_lobby_voice() {
 Session rx; assert(rx.start(nonce,1,MaxPeers-1));
 std::array<Session,MaxPeers-1> tx;
 Packet p; Frame out;
 for(unsigned i=0;i<MaxPeers-1;++i) {
  assert(tx[i].start(nonce,1,uint8_t(i)));tx[i].set_transmit_enabled(true);
  assert(tx[i].encode(input(i),p));
  assert(rx.receive(uint8_t(i),p.bytes.data(),p.size)==Receive::Accepted);
 }
 for(unsigned i=0;i<MaxPeers-1;++i) {
  for(unsigned j=0;j<StartupFrames;++j)assert(rx.playout(uint8_t(i),out)==Playback::Priming);
  assert(rx.playout(uint8_t(i),out)==Playback::Decoded&&!silent(out));
 }
 // The last legal sender also works when the host is the receiver.
 Session host;assert(host.start(nonce,1,0));rx.set_transmit_enabled(true);
 assert(rx.encode(input(0),p));assert(host.receive(MaxPeers-1,p.bytes.data(),p.size)==Receive::Accepted);
 for(unsigned j=0;j<StartupFrames;++j)assert(host.playout(MaxPeers-1,out)==Playback::Priming);
 assert(host.playout(MaxPeers-1,out)==Playback::Decoded&&!silent(out));
}
int main(){
 stereo_voice_balance();
#ifdef EOD_HAVE_OPUS
 quiet_microphone_playback();
 assert(Session::available());resumes_after_silence();full_lobby_voice();lifecycle();reorder_loss_and_bounds();invalid_packets();separate_peers();mute_and_deafen();std::puts("Voice codec/session/jitter checks passed");
#else
 Session s;assert(!Session::available()&&!s.start(nonce,1,0));Packet p;assert(!s.encode(input(0),p));Frame out;assert(s.playout(1,out)==Playback::Silence&&silent(out));std::puts("Voice unavailable fallback checks passed");
#endif
}
