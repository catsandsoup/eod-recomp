#pragma once
#include <cstdint>
namespace netplay {
// The first four bytes of the native driver replay record. Action/gear bits
// are deliberately retained separately until their full semantics are proven.
inline bool begins_epoch(uint32_t previous,uint32_t next,bool seen_world) {return seen_world && previous>0 && next==0;}
struct SeedPair {uint32_t first,second;};
inline SeedPair session_seeds(uint32_t seed,uint32_t stream) {
 const uint32_t mixed=seed*1664525u+1013904223u+stream*7919u;
 return {(seed+stream*997u)%30000u,mixed%30000u};
}
struct Control {uint16_t steering;uint8_t throttle,brake;};
inline Control test_control(uint64_t tick,uint32_t player=0) {
 if(tick<240)return {32768,0,0};
 if(tick<480)return {32768,255,0};
 if(tick<720)return {uint16_t(player==0?40000:25000),255,0};
 return {32768,0,255};
}
inline void encode_control(Control c,uint8_t *p) {
 p[0]=uint8_t(c.steering);p[1]=uint8_t(c.steering>>8);
 p[2]=c.throttle;p[3]=c.brake;
}
}
