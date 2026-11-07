#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include "netplay_link.h"
int main(){assert(netplay::PeerLink::available());}
