#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include "netplay_protocol.h"
int main() {
 netplay::Packet p; p.kind=netplay::MessageKind::Input;p.session=0x0102030405060708ull;p.epoch=2;p.frame=0x11223344;p.slot=1;p.size=6;p.payload={0,128,255,0,0,1};
 netplay::WirePacket w;assert(netplay::encode_packet(p,w));
 assert(w.size==34 && w.bytes[8]==1 && w.bytes[15]==8 && w.bytes[20]==0x11 && w.bytes[23]==0x44);
 netplay::Packet q;assert(netplay::decode_packet(w.bytes.data(),w.size,q));
 assert(q.session==p.session && q.frame==p.frame && q.slot==p.slot && q.payload[2]==255);
 assert(!netplay::decode_packet(w.bytes.data(),w.size-1,q));
 w.bytes[5]=2;assert(!netplay::decode_packet(w.bytes.data(),w.size,q));w.bytes[5]=1;
 w.bytes[27]=1;assert(!netplay::decode_packet(w.bytes.data(),w.size,q));w.bytes[27]=0;
 w.bytes[26]=4;assert(!netplay::decode_packet(w.bytes.data(),w.size,q));w.bytes[26]=1;
 w.bytes[7]=99;assert(!netplay::decode_packet(w.bytes.data(),w.size,q));
 p.size=7;assert(!netplay::encode_packet(p,w));
 p.kind=netplay::MessageKind::Voice;p.size=420;assert(!netplay::encode_packet(p,w));
}
