#include "netplay_hooks.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
namespace {
int shared_rules=-1,calls=0;
int rules(){++calls;return shared_rules;}
constexpr std::array<netplay_hooks::RaceRule,5> flags={
 netplay_hooks::Turbo,netplay_hooks::Chickens,netplay_hooks::Invincible,
 netplay_hooks::Durability,netplay_hooks::ManualRecovery};
}
int main(){
 using namespace netplay_hooks;
 // Disconnected hooks and an explicitly offline session preserve preferences.
 for(bool personal:{false,true})for(auto flag:flags)assert(rule(flag,personal)==personal);
 assert(race_scale(3.0f,1.0f)==3.0f);
 race_rules=rules;
 for(int offline:{-1,-2}){
  shared_rules=offline;
  for(bool personal:{false,true})for(auto flag:flags)assert(rule(flag,personal)==personal);
  assert(race_scale(3.0f,1.0f)==3.0f);
 }
 // Every advertised host rule combination wins over either saved local value.
 // The same interface serves both roles: there is no host preference exception.
 for(int mask=0;mask<32;++mask){
  shared_rules=mask;
  for(bool personal:{false,true})for(auto flag:flags){
   calls=0;assert(rule(flag,personal)==bool(mask&flag));assert(calls==1);
  }
  assert(race_scale(.1f,1.75f)==1.75f);
  assert(race_scale(50.f,1.f)==1.f);
  assert(race_scale(std::numeric_limits<float>::quiet_NaN(),1.f)==1.f);
  assert(race_scale(std::numeric_limits<float>::infinity(),1.f)==1.f);
 }
 // Unknown protocol bits cannot turn on any known optional advantage.
 shared_rules=0x4000;for(auto flag:flags)assert(!rule(flag,true));
 shared_rules=0;for(auto flag:flags)assert(!rule(flag,true));
 // Local rain/AI/master/native turbo/chicken remnants cannot bypass all-off.
 // Preserve unrelated native state bits while clearing documented combo bits.
 const uint32_t unrelated=0x80000000u;
 for(uint32_t stale=0;stale<256;++stale){
  for(bool turbo:{false,true})for(bool chickens:{false,true}){
   const auto race=online_cheats(stale|unrelated,false,turbo,chickens);
   assert((race&unrelated)==unrelated);
   assert((race&0xA4u)==0);
   assert(bool(race&0x02u)==turbo);
   assert(bool(race&0x08u)==chickens);
   assert(bool(race&0x01u)==(turbo||chickens));
   if(chickens)assert(!(race&0x10u));
   const auto battle=online_cheats(stale|unrelated,true,turbo,chickens);
   assert((battle&0xA4u)==0);
   assert(bool(battle&0x08u)==(chickens||bool(stale&0x08u)));
   assert(bool(battle&0x01u)==(turbo||bool(battle&0x08u)));
  }
 }
 // Leaving the room restores the offline preference immediately.
 shared_rules=-1;for(auto flag:flags)assert(rule(flag,true));
 race_rules=nullptr;assert(race_scale(2.f,1.f)==2.f);
 std::puts("Host race rules override local preferences and scales");
}
