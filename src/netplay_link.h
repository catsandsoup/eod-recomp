#pragma once
#include "netplay_protocol.h"
#include <memory>
#include <string>
namespace netplay {
// Portable GNS direct-IP link. Call from its owning event-loop thread. It does
// not claim account authentication, invite signaling, or simulation agreement.
class PeerLink {
public:
 enum class State {Unavailable,Offline,Listening,Connecting,Connected,Failed};
 PeerLink();~PeerLink();
 PeerLink(const PeerLink&)=delete;PeerLink&operator=(const PeerLink&)=delete;
 static bool available();
 bool host(uint16_t port);
 bool join(const std::string &numeric_address);
 void poll();
 bool send(const Packet &packet,bool reliable=true);
 bool receive(Packet &packet);
 void close();
 State state()const;
 std::string error()const;
private:
 struct Impl;std::unique_ptr<Impl> impl_;
};
}
