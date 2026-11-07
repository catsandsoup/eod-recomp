#pragma once
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
namespace netplay {
enum class MessageKind:uint16_t {Hello=1,Ready=2,Start=3,Input=4,StateDigest=5,Leave=6,Voice=32};
struct Packet {MessageKind kind=MessageKind::Hello;uint64_t session=0;uint32_t epoch=0,frame=0;uint8_t slot=0;uint16_t size=0;std::array<uint8_t,512>payload{};};
struct WirePacket {std::array<uint8_t,540>bytes{};size_t size=0;};
// All integer fields are big endian. Native six-byte input payloads retain
// their explicitly documented EE byte layout; no host structs are sent.
inline bool valid_payload(const Packet &p) {
 if(p.slot>3 || !p.session || p.size>p.payload.size())return false;
 switch(p.kind) {
 case MessageKind::Hello:return p.size==148;
 case MessageKind::Ready:return p.size==1 && p.payload[0]<=1;
 case MessageKind::Start:return p.size==8;
 case MessageKind::Input:return p.size==6;
 case MessageKind::StateDigest:return p.size==16;
 case MessageKind::Leave:return p.size==0;
 case MessageKind::Voice:return p.size>=19 && p.size<=419;
 }
 return false;
}
inline void put_be(uint8_t *p,uint64_t v,size_t n) {for(size_t i=0;i<n;++i)p[n-1-i]=uint8_t(v>>(i*8));}
inline uint64_t get_be(const uint8_t *p,size_t n) {uint64_t v=0;for(size_t i=0;i<n;++i)v=(v<<8)|p[i];return v;}
inline bool encode_packet(const Packet &p,WirePacket &out) {
 if(!valid_payload(p))return false;
 out.bytes.fill(0);auto *w=out.bytes.data();
 w[0]='T';w[1]='D';w[2]='E';w[3]='P';
 put_be(w+4,1,2);put_be(w+6,uint16_t(p.kind),2);put_be(w+8,p.session,8);
 put_be(w+16,p.epoch,4);put_be(w+20,p.frame,4);put_be(w+24,p.size,2);w[26]=p.slot;
 std::copy_n(p.payload.data(),p.size,w+28);out.size=28+p.size;return true;
}
inline bool decode_packet(const uint8_t *w,size_t size,Packet &out) {
 if(!w || size<28 || size>540 || w[0]!='T' || w[1]!='D' || w[2]!='E' || w[3]!='P' || get_be(w+4,2)!=1 || w[27])return false;
 Packet p;p.kind=MessageKind(get_be(w+6,2));p.session=get_be(w+8,8);
 p.epoch=uint32_t(get_be(w+16,4));p.frame=uint32_t(get_be(w+20,4));p.size=uint16_t(get_be(w+24,2));p.slot=w[26];
 if(p.size>p.payload.size() || size!=size_t(28+p.size))return false;
 std::copy_n(w+28,p.size,p.payload.data());if(!valid_payload(p))return false;
 out=p;return true;
}
}
