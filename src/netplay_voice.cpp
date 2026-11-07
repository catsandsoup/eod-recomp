#include "netplay_voice.h"
#include <algorithm>
#include <cstring>
#ifdef EOD_HAVE_OPUS
#include <opus.h>
#endif
namespace netplay::voice {
namespace {
constexpr uint16_t Version=1;
[[maybe_unused]] void put(uint8_t* p,uint64_t n,unsigned count) { for(unsigned i=0;i<count;++i)p[count-i-1]=uint8_t(n>>(8*i)); }
uint64_t get(const uint8_t* p,unsigned count) {uint64_t n=0;for(unsigned i=0;i<count;++i)n=(n<<8)|p[i];return n;}
}
struct Session::Impl {
    bool running=false,transmit=false,mic_muted=false,deafen=false;
    uint64_t nonce=0;uint32_t epoch=0,sequence=0;uint8_t local=0;
    struct Entry {bool used=false;uint32_t seq=0;size_t size=0;std::array<uint8_t,MaxPayloadBytes> payload{};};
    struct Peer {
        bool muted=false,started=false;uint32_t next=0;unsigned priming=0;
        std::array<Entry,JitterFrames> queue{};
#ifdef EOD_HAVE_OPUS
        OpusDecoder* decoder=nullptr;
#endif
    };
    std::array<Peer,MaxPeers> peers{};
#ifdef EOD_HAVE_OPUS
    OpusEncoder* encoder=nullptr;
#endif
    void flush(Peer& p) {
        p.queue={};
#ifdef EOD_HAVE_OPUS
        if(p.decoder)opus_decoder_ctl(p.decoder,OPUS_RESET_STATE);
#endif
    }
};
Session::Session():impl_(new Impl) {}
Session::~Session(){stop();}
bool Session::available(){
#ifdef EOD_HAVE_OPUS
return true;
#else
return false;
#endif
}
bool Session::start(uint64_t nonce,uint32_t epoch,uint8_t local){
    stop();if(!nonce||local>=MaxPeers)return false;
#ifdef EOD_HAVE_OPUS
    auto& s=*impl_;int error=0;
    s.encoder=opus_encoder_create(SampleRate,1,OPUS_APPLICATION_VOIP,&error);
    if(!s.encoder||error!=OPUS_OK){stop();return false;}
    if(opus_encoder_ctl(s.encoder,OPUS_SET_BITRATE(24000))!=OPUS_OK ||
       opus_encoder_ctl(s.encoder,OPUS_SET_VBR(0))!=OPUS_OK ||
       opus_encoder_ctl(s.encoder,OPUS_SET_COMPLEXITY(5))!=OPUS_OK ||
       opus_encoder_ctl(s.encoder,OPUS_SET_DTX(0))!=OPUS_OK){stop();return false;}
    for(unsigned i=0;i<MaxPeers;++i)if(i!=local){
        s.peers[i].decoder=opus_decoder_create(SampleRate,1,&error);
        if(!s.peers[i].decoder||error!=OPUS_OK){stop();return false;}
    }
    s.nonce=nonce;s.epoch=epoch;s.local=local;s.running=true;return true;
#else
    (void)epoch;return false;
#endif
}
void Session::stop(){
    auto& s=*impl_;s.running=false;s.transmit=false;s.sequence=0;s.nonce=0;s.epoch=0;
#ifdef EOD_HAVE_OPUS
    if(s.encoder)opus_encoder_destroy(s.encoder);
    s.encoder=nullptr;
#endif
    for(auto& p:s.peers){
#ifdef EOD_HAVE_OPUS
        if(p.decoder)opus_decoder_destroy(p.decoder);
        p.decoder=nullptr;
#endif
        p.queue={};p.started=false;p.next=0;p.priming=0;p.muted=false;
    }
}
bool Session::active()const{return impl_->running;}
void Session::set_transmit_enabled(bool v){
    auto& s=*impl_;v=s.running&&v;
#ifdef EOD_HAVE_OPUS
    if(v!=s.transmit&&s.encoder)opus_encoder_ctl(s.encoder,OPUS_RESET_STATE);
#endif
    s.transmit=v;
}
void Session::set_microphone_muted(bool v){
    auto& s=*impl_;
#ifdef EOD_HAVE_OPUS
    if(v!=s.mic_muted&&s.encoder)opus_encoder_ctl(s.encoder,OPUS_RESET_STATE);
#endif
    s.mic_muted=v;
}
void Session::set_deafened(bool v){auto& s=*impl_;if(s.deafen!=v)for(auto& p:s.peers)s.flush(p);s.deafen=v;}
void Session::set_peer_muted(uint8_t peer,bool v){if(peer>=MaxPeers)return;auto& p=impl_->peers[peer];if(p.muted!=v)impl_->flush(p);p.muted=v;}
bool Session::transmit_enabled()const{return impl_->transmit;}
bool Session::microphone_muted()const{return impl_->mic_muted;}
bool Session::deafened()const{return impl_->deafen;}
bool Session::peer_muted(uint8_t peer)const{return peer<MaxPeers&&impl_->peers[peer].muted;}
bool Session::encode(const Frame& pcm,Packet& out){
    out={};auto& s=*impl_;if(!s.running)return false;
    const uint32_t seq=s.sequence++;if(!s.transmit||s.mic_muted)return false;
#ifdef EOD_HAVE_OPUS
    int n=opus_encode(s.encoder,pcm.data(),FrameSamples,out.bytes.data()+HeaderBytes,MaxPayloadBytes);
    if(n<=0)return false;
    put(out.bytes.data(),Version,2);put(out.bytes.data()+2,s.nonce,8);out.bytes[10]=s.local;
    put(out.bytes.data()+11,s.epoch,4);put(out.bytes.data()+15,seq,4);out.size=HeaderBytes+size_t(n);return true;
#else
    (void)pcm;(void)seq;return false;
#endif
}
Receive Session::receive(uint8_t sender,const uint8_t* data,size_t size){
    auto& s=*impl_;if(!s.running)return Receive::Inactive;
    if(!data||size<=HeaderBytes||size>MaxPacketBytes||get(data,2)!=Version)return Receive::Invalid;
    if(get(data+2,8)!=s.nonce||get(data+11,4)!=s.epoch)return Receive::WrongSession;
    if(sender>=MaxPeers||sender==s.local||data[10]!=sender)return Receive::WrongSender;
#ifdef EOD_HAVE_OPUS
    const auto* payload=data+HeaderBytes;const int length=int(size-HeaderBytes);
    const unsigned char* frames[48];opus_int16 lengths[48];unsigned char toc;int offset;
    if(opus_packet_get_nb_channels(payload)!=1 || opus_packet_get_nb_samples(payload,length,SampleRate)!=FrameSamples ||
       opus_packet_parse(payload,length,&toc,frames,lengths,&offset)<1)return Receive::Invalid;
    auto& p=s.peers[sender];const uint32_t seq=uint32_t(get(data+15,4));
    if(p.started){
        const uint32_t distance=seq-p.next;
        if(distance>=0x80000000u)return Receive::Stale;
        if(distance>=JitterFrames)return Receive::OutsideWindow;
    }
    for(const auto& e:p.queue)if(e.used&&e.seq==seq)return Receive::Duplicate;
    auto slot=std::find_if(p.queue.begin(),p.queue.end(),[](const Impl::Entry& e){return !e.used;});
    if(slot==p.queue.end())return Receive::OutsideWindow;
    if(!p.started){p.started=true;p.next=seq;p.priming=StartupFrames;}
    slot->used=true;slot->seq=seq;slot->size=size-HeaderBytes;std::copy_n(payload,slot->size,slot->payload.begin());
    return Receive::Accepted;
#else
    return Receive::Inactive;
#endif
}
Playback Session::playout(uint8_t peer,Frame& pcm){
    pcm.fill(0);auto& s=*impl_;if(!s.running||peer>=MaxPeers||peer==s.local)return Playback::Silence;
#ifdef EOD_HAVE_OPUS
    auto& p=s.peers[peer];if(!p.started)return Playback::Silence;
    if(p.priming){--p.priming;return Playback::Priming;}
    auto slot=std::find_if(p.queue.begin(),p.queue.end(),[&](const Impl::Entry& e){return e.used&&e.seq==p.next;});
    ++p.next;
    int samples;Playback result;
    if(slot!=p.queue.end()){
        samples=opus_decode(p.decoder,slot->payload.data(),int(slot->size),pcm.data(),FrameSamples,0);
        *slot={};result=Playback::Decoded;
        if(samples<0){samples=opus_decode(p.decoder,nullptr,0,pcm.data(),FrameSamples,0);result=Playback::Concealed;}
    }else{samples=opus_decode(p.decoder,nullptr,0,pcm.data(),FrameSamples,0);result=Playback::Concealed;}
    if(samples!=int(FrameSamples)||p.muted||s.deafen)pcm.fill(0);
    return result;
#else
    return Playback::Silence;
#endif
}
}
