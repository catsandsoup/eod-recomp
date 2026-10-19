#include "battle_navigation.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace battle_navigation {
namespace {
constexpr uint32_t ram_size=0x02000000;
bool span(uint32_t a,uint32_t size) { return a && !(a&3) && size<=ram_size && a<=ram_size-size; }
bool finite(Point p) { return std::isfinite(p.x)&&std::isfinite(p.z); }
float distance(Point a,Point b) { return std::hypot(a.x-b.x,a.z-b.z); }
float number(Reader r,uint32_t a) { uint32_t u=r.word(r.context,a);float f;std::memcpy(&f,&u,4);return f; }
}
void Navigation::reset() { nodes_.clear();choices_={};resource_=root_=count_=0;length_=0; }
bool Navigation::refresh(Reader r) {
    if (!r.word) {reset();return false;}
    const uint32_t entries=r.word(r.context,0x3CD1B4);
    // Native cache occupies three 12-byte records before its count word.
    if (!entries || entries>3) {reset();return false;}
    uint32_t resource=0;
    for (uint32_t i=0;i<entries;++i) {
        const uint32_t p=0x3CD190+i*12;
        // 0x101 is the currently loaded forward oval course. Do not take an
        // arbitrary ghost, reverse line, or unrelated cached resource.
        if (r.word(r.context,p+4)!=0x101 || static_cast<int32_t>(r.word(r.context,p+8))<=0) continue;
        if (resource) {reset();return false;} // Ambiguous ownership: use direct pursuit.
        resource=r.word(r.context,p);
    }
    if (!span(resource,48) || r.word(r.context,resource)!=0x104F128B ||
        r.word(r.context,resource+4)!=48 || r.word(r.context,resource+8)!=60) {reset();return false;}
    const uint32_t count=r.word(r.context,resource+28)>>16;
    const uint32_t root=r.word(r.context,resource+32);
    if (count<2 || count>4096 || !span(root,count*60) || root<resource+48) {reset();return false;}
    if (resource==resource_ && root==root_ && count==count_ && !nodes_.empty()) return true;
    reset();
    // Native 13AF20 indexes this contiguous primary array modulo the HIGH
    // halfword at +1C. 139B60 steps backward by 60, wrapping at that count.
    // +10 is packed metadata, NOT a previous pointer. Secondary branch nodes
    // and their +38 linked lists are deliberately outside this reader.
    nodes_.reserve(count);
    for (uint32_t i=0;i<count;++i) {
        const uint32_t p=root+i*60;
        Node n{{number(r,p),number(r,p+4)},{number(r,p+8),number(r,p+12)},number(r,p+24),length_};
        if (r.word(r.context,p+20)!=root+((i+1)%count)*60 || !finite(n.position) ||
            !finite(n.tangent) || !std::isfinite(n.length) || n.length<=0 || n.length>10000) {reset();return false;}
        nodes_.push_back(n);length_+=n.length;
    }
    if (!std::isfinite(length_) || length_<=0) {reset();return false;}
    resource_=resource;root_=root;count_=count;
    return true;
}
bool Navigation::waypoint(uint32_t slot,uint32_t opponent,Point car,Point heading,
                          float speed,Point target,Point &out) {
    if(slot>=choices_.size() || nodes_.empty() || !finite(car) || !finite(target) ||
       !finite(heading) || !std::isfinite(speed)) return false;
    struct Projection { float along=0,error=std::numeric_limits<float>::infinity();Point tangent; };
    auto project=[&](Point p,bool bot) {
        Projection best;
        for(size_t i=0;i<nodes_.size();++i) {
            const Node &a=nodes_[i],&b=nodes_[(i+1)%nodes_.size()];
            const float dx=b.position.x-a.position.x,dz=b.position.z-a.position.z,d2=dx*dx+dz*dz;
            if(d2<.0001f) continue;
            const float t=std::clamp(((p.x-a.position.x)*dx+(p.z-a.position.z)*dz)/d2,0.0f,1.0f);
            const float e=distance(p,{a.position.x+t*dx,a.position.z+t*dz});
            // At an actual crossing, prefer the bot's aligned course tangent.
            // This does not establish a traversable connection across branches.
            const float alignment=bot?std::fabs(heading.x*a.tangent.x+heading.z*a.tangent.z):0;
            const float old_alignment=bot?std::fabs(heading.x*best.tangent.x+heading.z*best.tangent.z):0;
            if(e<best.error-.05f || (std::fabs(e-best.error)<=.05f && alignment>old_alignment))
                best={a.start+t*a.length,e,a.tangent};
        }
        return best;
    };
    auto &choice=choices_[slot];
    const Projection a=project(car,true),b=project(target,false);
    // Course following is useful close to a racing line. It cannot plan a safe
    // approach from arbitrary arena/infield positions or test obstacle clearance.
    if(a.error>35 || b.error>35) {choice={};return false;}
    float forward=std::fmod(b.along-a.along+length_,length_);
    const float backward=length_-forward,shortest=std::min(forward,backward),range=distance(car,target);
    // Nearby across a distant course section can mean a wall or figure-eight
    // crossing. Require both world proximity and local course proximity to exit.
    if(shortest<18 || (range<22 && shortest<std::max(35.0f,range*2))) {choice={};return false;}
    int direction=forward<=backward?1:-1;
    if(std::fabs(forward-backward)<5)
        direction=(heading.x*a.tangent.x+heading.z*a.tangent.z)>=0?1:-1;
    if(choice.opponent==opponent && choice.direction) {
        const float retained=choice.direction>0?forward:backward,alternative=choice.direction>0?backward:forward;
        if(!(alternative+30<retained*.55f)) direction=choice.direction;
    }
    choice={opponent,direction};
    const float arc=direction>0?forward:backward;
    const float lookahead=std::min(arc,std::clamp(5+std::max(speed,0.0f)*.65f,8.0f,20.0f));
    float along=std::fmod(a.along+direction*lookahead+length_,length_);
    for(size_t i=0;i<nodes_.size();++i) {
        const Node &n=nodes_[i];
        if(along>n.start+n.length && i+1<nodes_.size()) continue;
        const Node &next=nodes_[(i+1)%nodes_.size()];
        const float t=std::clamp((along-n.start)/n.length,0.0f,1.0f);
        out={n.position.x+(next.position.x-n.position.x)*t,n.position.z+(next.position.z-n.position.z)*t};
        return finite(out);
    }
    return false;
}
} // namespace battle_navigation
