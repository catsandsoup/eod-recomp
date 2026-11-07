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
[[maybe_unused]] void prime(Session& s){Frame out;assert(s.playout(0,out)==Playback::Priming&&silent(out));assert(s.playout(0,out)==Playback::Priming&&silent(out));}
[[maybe_unused]] void lifecycle(){
 Session tx,rx;Packet p;Frame out;
 assert(!tx.encode(input(0),p)&&p.size==0);assert(!tx.start(0,1,0));assert(!tx.start(nonce,1,4));
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
 std::array<Packet,10> packets{};for(unsigned i=0;i<packets.size();++i)assert(tx.encode(input(i),packets[i]));
 for(unsigned i=0;i<3;++i)assert(ordered.receive(0,packets[i].bytes.data(),packets[i].size)==Receive::Accepted);
 for(unsigned i:{0u,2u,1u})assert(reordered.receive(0,packets[i].bytes.data(),packets[i].size)==Receive::Accepted);
 assert(reordered.receive(0,packets[1].bytes.data(),packets[1].size)==Receive::Duplicate);
 assert(reordered.receive(0,packets[8].bytes.data(),packets[8].size)==Receive::OutsideWindow);
 prime(ordered);prime(reordered);Frame a,b;
 for(unsigned i=0;i<3;++i){assert(ordered.playout(0,a)==Playback::Decoded);assert(reordered.playout(0,b)==Playback::Decoded);assert(a==b);}
 assert(reordered.receive(0,packets[0].bytes.data(),packets[0].size)==Receive::Stale);
 assert(reordered.playout(0,b)==Playback::Concealed);
 assert(reordered.receive(0,packets[4].bytes.data(),packets[4].size)==Receive::Accepted);
 assert(reordered.playout(0,b)==Playback::Decoded);
 // Bounded queue accepts8, rejects9th; modular sequence wraps correctly.
 Session wrap;assert(wrap.start(nonce,1,1));
 for(unsigned i=0;i<8;++i){seq(packets[i],0xfffffffeu+i);assert(wrap.receive(0,packets[i].bytes.data(),packets[i].size)==Receive::Accepted);}
 seq(packets[8],6);assert(wrap.receive(0,packets[8].bytes.data(),packets[8].size)==Receive::OutsideWindow);prime(wrap);
 for(unsigned i=0;i<8;++i)assert(wrap.playout(0,b)==Playback::Decoded);
 assert(wrap.receive(0,packets[8].bytes.data(),packets[8].size)==Receive::Accepted);
}
[[maybe_unused]] void invalid_packets(){
 Session tx,rx;assert(tx.start(nonce,9,0)&&rx.start(nonce,9,1));tx.set_transmit_enabled(true);Packet p;assert(tx.encode(input(0),p));
 assert(rx.receive(0,nullptr,p.size)==Receive::Invalid);
 for(size_t n=0;n<=HeaderBytes;++n)assert(rx.receive(0,p.bytes.data(),n)==Receive::Invalid);
 assert(rx.receive(0,p.bytes.data(),MaxPacketBytes+1)==Receive::Invalid);
 auto q=p;q.bytes[1]=2;assert(rx.receive(0,q.bytes.data(),q.size)==Receive::Invalid);
 q=p;q.bytes[2]^=1;assert(rx.receive(0,q.bytes.data(),q.size)==Receive::WrongSession);
 q=p;q.bytes[14]^=1;assert(rx.receive(0,q.bytes.data(),q.size)==Receive::WrongSession);
 assert(rx.receive(2,p.bytes.data(),p.size)==Receive::WrongSender);assert(rx.receive(4,p.bytes.data(),p.size)==Receive::WrongSender);
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
 prime(rx);assert(rx.playout(2,out)==Playback::Priming);assert(rx.playout(2,out)==Playback::Priming);
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
int main(){
#ifdef EOD_HAVE_OPUS
 assert(Session::available());lifecycle();reorder_loss_and_bounds();invalid_packets();separate_peers();mute_and_deafen();std::puts("Voice codec/session/jitter checks passed");
#else
 Session s;assert(!Session::available()&&!s.start(nonce,1,0));Packet p;assert(!s.encode(input(0),p));Frame out;assert(s.playout(1,out)==Playback::Silence&&silent(out));std::puts("Voice unavailable fallback checks passed");
#endif
}
