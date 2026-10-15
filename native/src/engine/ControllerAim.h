#pragma once
#include "CameraAdapter.h"
#include "runtime/ControllerInput.h"
#include <mutex>

namespace rdrvr::engine {
struct ControllerRay {
    runtime::Vec3 origin{},direction{},right{},up{},viewOrigin{};
    float unitsPerMetre{};
    unsigned hand{};bool gripPose{};
    bool operator==(const ControllerRay&)const=default;
    std::uint64_t sceneId{},controllerSequence{},trackingEpoch{},referenceSpaceEpoch{};
};
// Derive the exact tracking -> world transform from the rendered eye and its
// matching tracked pose. Do not anchor a hand independently to current head yaw
// or player-model heading; either would move the shot when looking elsewhere.
inline bool mapControllerRay(const StereoCameraPlan& plan,const runtime::ControllerSample& input,
                             unsigned hand,std::uint64_t now,ControllerRay& out,bool gripPose=false){
    using namespace runtime;
    if(hand>1)return false;
    const auto& pose=gripPose?input.grip[hand]:input.aim[hand];
    const auto& track=plan.tracking;
    if(hand>1||!plan.sceneId||!track.frameId||!validController(input)||input.sequence<track.frameId||
       !track.epochs.tracking||!track.epochs.referenceSpace||input.epoch!=track.epochs.tracking||
       input.referenceSpace!=track.epochs.referenceSpace||!track.publishedNs||now<track.publishedNs||
       now-track.publishedNs>TrackedViewsMailbox::maxAgeNs||now<input.publishedNs||
       now-input.publishedNs>TrackedViewsMailbox::maxAgeNs||!validPose(track.eyes[0])||!validPose(pose)||
       !std::isfinite(plan.unitsPerMetre)||plan.unitsPerMetre<0.01f||plan.unitsPerMetre>100)return false;
    const auto& world=plan.world[0];
    for(float x:world)if(!std::isfinite(x))return false;
    if(world[3]!=0||world[7]!=0||world[11]!=0||world[15]!=1)return false;
    const auto dot=[](Vec3 a,Vec3 b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    const Vec3 x{world[0],world[1],world[2]},y{world[4],world[5],world[6]},z{world[8],world[9],world[10]};
    const Vec3 cross{x[1]*y[2]-x[2]*y[1],x[2]*y[0]-x[0]*y[2],x[0]*y[1]-x[1]*y[0]};
    if(std::abs(dot(x,x)-1)>0.005f||std::abs(dot(y,y)-1)>0.005f||std::abs(dot(z,z)-1)>0.005f||
       std::abs(dot(x,y))>0.005f||std::abs(dot(x,z))>0.005f||std::abs(dot(y,z))>0.005f||dot(cross,z)<0.995f)return false;
    const auto q=track.eyes[0].orientation;
    const Quat inverse{-q[0],-q[1],-q[2],q[3]};
    const auto toWorld=[&](Vec3 v){
        v=rotate(inverse,v);
        return Vec3{x[0]*v[0]+y[0]*v[1]+z[0]*v[2],x[1]*v[0]+y[1]*v[1]+z[1]*v[2],x[2]*v[0]+y[2]*v[1]+z[2]*v[2]};
    };
    Vec3 offset{};for(unsigned i=0;i<3;++i)offset[i]=pose.position[i]-track.eyes[0].position[i];
    offset=toWorld(offset);
    ControllerRay ray{};
    ray.unitsPerMetre=plan.unitsPerMetre;ray.hand=hand;ray.gripPose=gripPose;
    for(unsigned i=0;i<3;++i){
        ray.viewOrigin[i]=(plan.world[0][12+i]+plan.world[1][12+i])*0.5f;
        if(!std::isfinite(ray.viewOrigin[i]))return false;
    }
    ray.right=toWorld(rotate(pose.orientation,{1,0,0}));
    ray.up=toWorld(rotate(pose.orientation,{0,1,0}));
    ray.direction=toWorld(rotate(pose.orientation,{0,0,-1}));
    const auto length=std::sqrt(dot(ray.direction,ray.direction));
    if(!std::isfinite(length)||length<0.99f||length>1.01f)return false;
    for(unsigned i=0;i<3;++i){
        ray.origin[i]=world[12+i]+offset[i]*plan.unitsPerMetre;
        ray.direction[i]/=length;if(!std::isfinite(ray.origin[i]))return false;
    }
    ray.sceneId=plan.sceneId;ray.controllerSequence=input.sequence;
    ray.trackingEpoch=input.epoch;ray.referenceSpaceEpoch=input.referenceSpace;
    out=ray;return true;
}
// This publishes only a successfully recorded world's transform.
class ControllerAimSpace {
    mutable std::mutex mutex_;StereoCameraPlan plan_{};bool available_{},movementAvailable_{};std::uint64_t afterHeadSampleNs_{};
public:
    void publish(const StereoCameraPlan& plan){std::lock_guard lock(mutex_);
        if(afterHeadSampleNs_&&(!plan.headAnchored||plan.headSampledNs<=afterHeadSampleNs_))return;
        plan_=plan;available_=movementAvailable_=true;}
    void invalidate(){std::lock_guard lock(mutex_);available_=movementAvailable_=false;}
    // A rendered plan already in flight when native yaw changes must not
    // republish the previous hand/world transform after this invalidation.
    void afterNativeTurn(std::uint64_t now){std::lock_guard lock(mutex_);
        afterHeadSampleNs_=std::max(afterHeadSampleNs_,now);available_=false;}
    bool plan(StereoCameraPlan& out)const{std::lock_guard lock(mutex_);if(!available_)return false;out=plan_;return true;}
    // Head-relative walking uses only eye yaw relative to this same base;
    // rotating the native world base does not change that relative angle.
    // Keep its already freshness-checked sample across a snap so held walking
    // does not enter the unrelated mapping-change neutral latch.
    bool movementPlan(StereoCameraPlan& out)const{std::lock_guard lock(mutex_);if(!movementAvailable_)return false;out=plan_;return true;}
    bool ray(const runtime::ControllerSample& input,unsigned hand,std::uint64_t now,ControllerRay& out)const{
        StereoCameraPlan plan{};{std::lock_guard lock(mutex_);if(!available_)return false;plan=plan_;}
        return mapControllerRay(plan,input,hand,now,out);
    }
};
}
