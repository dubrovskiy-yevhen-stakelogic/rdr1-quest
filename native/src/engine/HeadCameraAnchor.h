#pragma once
#include "CameraAdapter.h"
#include <mutex>

namespace rdrvr::engine {
struct HeadCameraAnchor {
    runtime::Vec3 position{},up{};
    std::uint32_t actorGuid{};
    std::uint64_t sampledNs{},presentationRevision{};
    std::uintptr_t bodyCreature{},hatNode{};
    // Published together with `position` so the render thread never pairs a capsule of one frame with the
    // body point B of another (camera continuity while roomscale moves the capsule). origin.valid=false = legacy latch.
    TrackingOrigin origin{};
    enum class Kind : std::uint8_t {LegacyHead,Body,BodyFiltered,Head};
    Kind kind{Kind::LegacyHead};
};
// Player mover (capsule) facts read by HeadAnchorReader on the PostCameraUpdate owner (main
// thread, after physics; librdr 1.58 offsets). `*Known` false = not readable.
struct BodyAnchorFacts {
    bool valid{};               // filled by a successful sample
    runtime::Vec3 capsule{};    // mover+0x70 (= actor matrix row d, the capsule base), world units
    runtime::Vec3 up{};         // actor matrix row b
    bool headValid{};runtime::Vec3 head{}; // sagActor::GetHeadPosition (head bone), only when it returned true
    bool moverMatrix{};         // actor+0xb0->+0x18 == actor+0x88 +0x40 (actor matrix is the capsule matrix)
    bool simKnown{};            // *(mover) == base+0x2db79e0 (mvrMoverSimComponent address point)
    bool ragdoll{};             // Sim +0xb8c (u32) != 0: suspended, mover follows the ragdoll/entity instance
    bool attached{};            // +0x830 phInst* != 0 (mounted/attached; normally rejected earlier by player+0x5d4)
    bool suspended{},frozen{};  // int16 +0x846 / +0x848 > 0: kinematic mover branch (Teleport without sweep)
    bool cover{};               // u8 +0xaca
    bool ladder{};              // u32 +0xaf0 (HandleTeleport -> StopLadder)
    bool velocityKnown{};runtime::Vec3 velocity{}; // +0x7b0 cached linear velocity (sagMsgGetVelocity)
    bool onFoot() const {return valid&&simKnown&&!ragdoll&&!attached&&!suspended&&!frozen&&!cover&&!ladder;}
};
// Keep the game's horizontal camera heading while removing animation/camera
// pitch and roll. HMD orientation is composed once later by CameraAdapter.
// No player heading is used to remap the controllers separately.
inline bool prepareHeadCamera(const StockCamera& stock,const HeadCameraAnchor& anchor,StockCamera& out){
    using namespace runtime;
    for(float f:anchor.position)if(!std::isfinite(f))return false;
    for(float f:anchor.up)if(!std::isfinite(f))return false;
    const auto dot=[](Vec3 a,Vec3 b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    const auto cross=[](Vec3 a,Vec3 b){return Vec3{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};};
    auto up=anchor.up;const float upLength=std::sqrt(dot(up,up));
    if(!std::isfinite(upLength)||upLength<0.99f||upLength>1.01f)return false;
    for(float& f:up)f/=upLength;
    Vec3 back{stock.world[8],stock.world[9],stock.world[10]};
    const float vertical=dot(back,up);for(unsigned i=0;i<3;++i)back[i]-=vertical*up[i];
    const float length=std::sqrt(dot(back,back));
    // Looking vertically gives no horizontal heading. Do not invent one.
    if(!std::isfinite(length)||length<0.01f)return false;
    for(float& f:back)f/=length;
    const auto right=cross(up,back);up=cross(back,right);
    StockCamera result=stock;const auto& p=anchor.position;
    result.world={right[0],right[1],right[2],0,up[0],up[1],up[2],0,
        back[0],back[1],back[2],0,p[0],p[1],p[2],1};
    out=result;return true;
}
class HeadCameraMailbox {
    mutable std::mutex mutex_;HeadCameraAnchor latest_{};bool available_{};
public:
    static constexpr std::uint64_t maxAgeNs=100'000'000;
    void invalidate(){std::lock_guard lock(mutex_);available_=false;}
    bool publish(const HeadCameraAnchor& sample){
        std::lock_guard lock(mutex_);
        if(!sample.sampledNs||!sample.presentationRevision||sample.sampledNs<latest_.sampledNs){available_=false;return false;}
        for(float f:sample.position)if(!std::isfinite(f)){available_=false;return false;}
        float norm{};for(float f:sample.up)norm+=f*f;
        if(!std::isfinite(norm)||std::abs(norm-1)>0.005f){available_=false;return false;}
        if(sample.origin.valid){
            float q{};for(float f:sample.origin.inverseYaw)q+=f*f;bool finite=std::isfinite(q)&&std::abs(q-1)<0.01f;
            for(float f:sample.origin.position)finite=finite&&std::isfinite(f);
            if(!finite||!sample.origin.referenceSpace){available_=false;return false;}
        }
        latest_=sample;available_=true;return true;
    }
    bool snapshot(std::uint64_t now,std::uint64_t revision,HeadCameraAnchor& out)const{
        std::lock_guard lock(mutex_);
        if(!available_||revision!=latest_.presentationRevision||now<latest_.sampledNs||now-latest_.sampledNs>maxAgeNs)return false;
        out=latest_;return true;
    }
    bool matches(const StereoCameraPlan& plan,std::uint64_t now,std::uint64_t revision)const{
        if(!plan.headAnchored||plan.presentationRevision!=revision||!plan.headSampledNs||
           now<plan.headSampledNs||now-plan.headSampledNs>maxAgeNs)return false;
        HeadCameraAnchor latest{};return snapshot(now,revision,latest)&&latest.actorGuid==plan.actorGuid&&latest.bodyCreature==plan.bodyCreature&&latest.hatNode==plan.hatNode;
    }
};
}
