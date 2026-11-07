#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include "netplay_control.h"
int main() {
 assert(netplay::begins_epoch(300,0,true));
 assert(!netplay::begins_epoch(0,0,true));
 assert(!netplay::begins_epoch(300,0,false));
 assert(!netplay::begins_epoch(300,301,true));
 auto a=netplay::test_control(1); assert(a.steering==32768 && a.throttle==0 && a.brake==0);
 auto b=netplay::test_control(240);assert(b.throttle==255 && b.brake==0);
 auto c=netplay::test_control(480);assert(c.steering==40000);
 auto d=netplay::test_control(720);assert(d.throttle==0 && d.brake==255);
 auto player2=netplay::test_control(480,1);assert(player2.steering==25000);
 auto seed=netplay::session_seeds(123,0);assert(seed.first==123 && seed.second==10798);
 auto other=netplay::session_seeds(123,1);assert(other.first!=seed.first && other.second<30000);
 unsigned char p[4];netplay::encode_control(c,p);assert(p[0]==0x40 && p[1]==0x9c && p[2]==255 && p[3]==0);
}
