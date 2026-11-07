#include "netplay_link.h"
#include <deque>
#ifdef EOD_HAVE_GNS
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#endif
namespace netplay {
struct PeerLink::Impl {
 State state=available()?State::Offline:State::Unavailable;
 std::string error;
 std::deque<Packet> inbox;
#ifdef EOD_HAVE_GNS
 bool initialized=false;
 ISteamNetworkingSockets *api=nullptr;
 HSteamNetConnection connection=k_HSteamNetConnection_Invalid;
 HSteamListenSocket listener=k_HSteamListenSocket_Invalid;
 static Impl *current;
 bool initialize() {
  if(initialized)return true;
  if(current && current!=this){error="Only one peer link is supported per process";state=State::Failed;return false;}
  SteamDatagramErrMsg reason;
  if(!GameNetworkingSockets_Init(nullptr,reason)){error=reason;state=State::Failed;return false;}
  current=this;initialized=true;api=SteamNetworkingSockets();return true;
 }
 void fail(const std::string &reason) {
  error=reason;state=State::Failed;
  if(connection!=k_HSteamNetConnection_Invalid){api->CloseConnection(connection,4001,reason.c_str(),false);connection=k_HSteamNetConnection_Invalid;}
 }
 static void callback(SteamNetConnectionStatusChangedCallback_t *event) {
  auto *p=current;if(!p)return;
  if(event->m_info.m_eState==k_ESteamNetworkingConnectionState_Connecting && p->listener!=k_HSteamListenSocket_Invalid) {
   if(p->connection!=k_HSteamNetConnection_Invalid){p->api->CloseConnection(event->m_hConn,4001,"Room is full",false);return;}
   p->connection=event->m_hConn;
   if(p->api->AcceptConnection(p->connection)!=k_EResultOK){p->fail("Connection could not be accepted");return;}
   p->state=State::Connecting;
  }
  if(event->m_hConn!=p->connection)return;
  switch(event->m_info.m_eState) {
   case k_ESteamNetworkingConnectionState_Connected:p->state=State::Connected;break;
   case k_ESteamNetworkingConnectionState_ClosedByPeer:
   case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:p->fail(event->m_info.m_szEndDebug);break;
   default:break;
  }
 }
#endif
};
#ifdef EOD_HAVE_GNS
PeerLink::Impl *PeerLink::Impl::current=nullptr;
#endif
bool PeerLink::available() {
#ifdef EOD_HAVE_GNS
 return true;
#else
 return false;
#endif
}
PeerLink::PeerLink():impl_(new Impl){}
PeerLink::~PeerLink(){close();}
bool PeerLink::host(uint16_t port) {
 close();
#ifdef EOD_HAVE_GNS
 if(!port){impl_->error="A nonzero listen port is required";impl_->state=State::Failed;return false;}
 if(!impl_->initialize())return false;
 SteamNetworkingIPAddr address;address.Clear();address.m_port=port;
 SteamNetworkingConfigValue_t option;option.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,(void*)Impl::callback);
 impl_->listener=impl_->api->CreateListenSocketIP(address,1,&option);
 if(impl_->listener==k_HSteamListenSocket_Invalid){impl_->fail("Cannot listen on that port");return false;}
 impl_->state=State::Listening;return true;
#else
 (void)port;impl_->error="This build has no network transport";return false;
#endif
}
bool PeerLink::join(const std::string &numeric_address) {
 close();
#ifdef EOD_HAVE_GNS
 SteamNetworkingIPAddr address;address.Clear();
 if(!address.ParseString(numeric_address.c_str()) || !address.m_port){impl_->error="Enter an IP address and port";impl_->state=State::Failed;return false;}
 if(!impl_->initialize())return false;
 SteamNetworkingConfigValue_t option;option.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,(void*)Impl::callback);
 impl_->connection=impl_->api->ConnectByIPAddress(address,1,&option);
 if(impl_->connection==k_HSteamNetConnection_Invalid){impl_->fail("Cannot begin connection");return false;}
 impl_->state=State::Connecting;return true;
#else
 (void)numeric_address;impl_->error="This build has no network transport";return false;
#endif
}
void PeerLink::poll() {
#ifdef EOD_HAVE_GNS
 if(!impl_->initialized)return;
 impl_->api->RunCallbacks();
 if(impl_->state!=State::Connected)return;
 SteamNetworkingMessage_t *messages[16];
 const int count=impl_->api->ReceiveMessagesOnConnection(impl_->connection,messages,16);
 if(count<0){impl_->fail("Connection receive failed");return;}
 for(int i=0;i<count;++i) {
  Packet packet;
  const bool valid=decode_packet(static_cast<const uint8_t*>(messages[i]->m_pData),size_t(messages[i]->m_cbSize),packet);
  messages[i]->Release();
  if(!valid){impl_->fail("Invalid network packet or incompatible protocol");continue;}
  if(impl_->state!=State::Connected)continue;
  if(impl_->inbox.size()>=128){impl_->fail("Network receive queue exceeded its limit");continue;}
  impl_->inbox.push_back(packet);
 }
#endif
}
bool PeerLink::send(const Packet &packet,bool reliable) {
#ifdef EOD_HAVE_GNS
 if(impl_->state!=State::Connected)return false;
 WirePacket wire;if(!encode_packet(packet,wire))return false;
 const int flags=reliable?k_nSteamNetworkingSend_ReliableNoNagle:k_nSteamNetworkingSend_UnreliableNoDelay;
 return impl_->api->SendMessageToConnection(impl_->connection,wire.bytes.data(),uint32_t(wire.size),flags,nullptr)==k_EResultOK;
#else
 (void)packet;(void)reliable;return false;
#endif
}
bool PeerLink::receive(Packet &packet) {
 if(impl_->inbox.empty())return false;
 packet=impl_->inbox.front();impl_->inbox.pop_front();return true;
}
void PeerLink::close() {
#ifdef EOD_HAVE_GNS
 if(impl_->initialized) {
  if(impl_->connection!=k_HSteamNetConnection_Invalid)impl_->api->CloseConnection(impl_->connection,0,"Session closed",false);
  if(impl_->listener!=k_HSteamListenSocket_Invalid)impl_->api->CloseListenSocket(impl_->listener);
  impl_->connection=k_HSteamNetConnection_Invalid;impl_->listener=k_HSteamListenSocket_Invalid;
  GameNetworkingSockets_Kill();impl_->initialized=false;impl_->api=nullptr;
  if(Impl::current==impl_.get())Impl::current=nullptr;
 }
#endif
 impl_->inbox.clear();impl_->error.clear();impl_->state=available()?State::Offline:State::Unavailable;
}
PeerLink::State PeerLink::state()const{return impl_->state;}
std::string PeerLink::error()const{return impl_->error;}
}
