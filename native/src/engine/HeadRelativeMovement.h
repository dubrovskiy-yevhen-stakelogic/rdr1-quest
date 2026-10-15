#pragma once
#include "CameraAdapter.h"
#include "runtime/ControllerInput.h"
namespace rdrvr::engine {
// Same horizontal-stick rotation used by the Quest Vice City port, derived
// from the exact rendered view/base pair so recenter yaw is not applied twice.
inline bool headRelativeStick(const StereoCameraPlan& plan,const runtime::ControllerSample& input,
                              std::uint64_t now,std::array<float,2>& out){
    using namespace runtime;const auto& t=plan.tracking;
    const auto rigid=[](const Mat4& m){
        for(float x:m)if(!std::isfinite(x))return false;
        if(m[3]!=0||m[7]!=0||m[11]!=0||m[15]!=1)return false;
        for(unsigned a=0;a<3;++a)for(unsigned b=a;b<3;++b){float d{};
            for(unsigned i=0;i<3;++i)d+=m[a*4+i]*m[b*4+i];
            if(std::abs(d-(a==b?1.f:0.f))>.005f)return false;}
        const float det=(m[1]*m[6]-m[2]*m[5])*m[8]+(m[2]*m[4]-m[0]*m[6])*m[9]+(m[0]*m[5]-m[1]*m[4])*m[10];
        return det>.995f;
    };
    if(!plan.headAnchored||!plan.sceneId||!plan.actorGuid||!plan.presentationRevision||
       !validController(input)||input.sequence<t.frameId||!t.frameId||
       input.epoch!=t.epochs.tracking||input.referenceSpace!=t.epochs.referenceSpace||!input.referenceSpace||
       !t.publishedNs||now<t.publishedNs||now-t.publishedNs>TrackedViewsMailbox::maxAgeNs||
       now<input.publishedNs||now-input.publishedNs>ControllerMailbox::maxAgeNs||
       !rigid(plan.baseWorld)||!rigid(plan.world[0])||
       !rigid(plan.world[1]))return false;
    float right{},forward{};
    for(unsigned i=0;i<3;++i){const float f=-(plan.world[0][8+i]+plan.world[1][8+i])*.5f;
        right+=f*plan.baseWorld[i];forward-=f*plan.baseWorld[8+i];}
    const float length=std::hypot(right,forward);
    // Looking nearly vertically provides no reliable horizontal direction.
    if(!std::isfinite(length)||length<.1f)return false;
    right/=length;forward/=length;
    const float x=input.sticks[0],y=input.sticks[1],magnitude=std::max(1.f,std::hypot(x,y));
    out={(x*forward+y*right)/magnitude,(y*forward-x*right)/magnitude};return true;
}
inline bool movementUsesHeadCamera(bool world,std::uint64_t presented,std::uint64_t now){
    return world&&presented&&now>=presented&&now-presented<=runtime::TrackedViewsMailbox::maxAgeNs;
}
enum class MovementMapping {Native,Head,Unavailable};
// Only movement is latched. Releasing/re-pressing a trigger is never required
// by a locomotion change. A held stick cannot jump to a different coordinate
// system when entering/exiting an optic, constrained animation or stale frame.
class MovementMappingLatch {
    MovementMapping mode_{MovementMapping::Native};bool waiting_{};
public:
    void reset(){mode_=MovementMapping::Native;waiting_=false;}
    std::array<float,2> sample(MovementMapping mode,const std::array<float,2>& raw,const std::array<float,2>& mapped){
        const bool neutral=std::abs(raw[0])<=.15f&&std::abs(raw[1])<=.15f;
        if(mode!=mode_){mode_=mode;if(!neutral)waiting_=true;}
        if(neutral)waiting_=false;
        if(waiting_||mode==MovementMapping::Unavailable)return {};
        return mode==MovementMapping::Head?mapped:raw;
    }
};
}
