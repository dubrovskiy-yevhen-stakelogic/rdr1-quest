#pragma once
#include "ControllerAimTarget.h"
#include "NativeReticleVisibility.h"
#include "runtime/StereoReticle.h"
namespace rdrvr::engine {
// Project a completed native aim target through THIS recorded stereo pair.
// No engine/XR calls or camera-relative retargeting happen on the render thread.
inline bool projectAimTarget(const StereoCameraPlan& plan,const ControllerAimTarget& target,
                             std::uint32_t actorGuid,std::uint64_t revision,std::uint64_t stamp,
                             std::uint64_t now,runtime::StereoReticle& out){
    if(!plan.headAnchored||!actorGuid||plan.actorGuid!=actorGuid||!revision||plan.presentationRevision!=revision||
       !stamp||now<stamp||now-stamp>100'000'000||!plan.sceneId||!target.ray.sceneId||target.ray.sceneId>plan.sceneId||
       target.ray.trackingEpoch!=plan.tracking.epochs.tracking||!target.ray.trackingEpoch||
       target.ray.referenceSpaceEpoch!=plan.tracking.epochs.referenceSpace||!target.ray.referenceSpaceEpoch||
       !plan.headSampledNs||now<plan.headSampledNs||now-plan.headSampledNs>100'000'000||
       !plan.tracking.publishedNs||now<plan.tracking.publishedNs||now-plan.tracking.publishedNs>runtime::TrackedViewsMailbox::maxAgeNs||
       !std::isfinite(plan.unitsPerMetre)||plan.unitsPerMetre<=0)return false;
    const std::array<double,4> point{target.point.x,target.point.y,target.point.z,1};
    for(double x:point)if(!std::isfinite(x))return false;
    runtime::StereoReticle result{plan.sceneId,target.ray.trackingEpoch,target.ray.referenceSpaceEpoch,stamp};bool visible=false;
    for(unsigned eye=0;eye<2;++eye){
        for(float x:plan.view[eye])if(!std::isfinite(x))return false;
        for(float x:plan.projection[eye])if(!std::isfinite(x))return false;
        std::array<double,4> view{},clip{};
        for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)view[r]+=plan.view[eye][4*c+r]*point[c];
        for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)clip[r]+=plan.projection[eye][4*c+r]*view[c];
        for(double x:clip)if(!std::isfinite(x))return false;
        // Points behind/inside the near plane cannot produce a usable marker.
        // No far-Z reject: an unobstructed native aim point can exceed far clip.
        if(clip[3]<=.01*plan.unitsPerMetre||clip[2]<0)continue;
        // EnginePassBinding pins VkViewport {y=height, height=-height}.
        // The private transfer uses positive height and gl_FragCoord pixels:
        // map native clip Y through the ENGINE viewport before that transfer.
        const double u=.5*(clip[0]/clip[3]+1),v=.5*(1-clip[1]/clip[3]);
        if(u<0||u>1||v<0||v>1)continue;
        result.eyes[eye]={float(u),float(v),.0025f};visible=true;
    }
    if(!visible)return false;out=result;return true;
}
// Permission and target have independent leases. Keep the older timestamp so
// the display transfer cannot outlive either observation.
inline bool projectVisibleAimTarget(const StereoCameraPlan& plan,const ControllerAimTarget& target,
                                    std::uint32_t actorGuid,std::uint64_t revision,std::uint64_t stamp,
                                    const NativeReticleFrame& native,std::uint64_t now,runtime::StereoReticle& out){
    if(!native.allows(plan.actorGuid,plan.presentationRevision,now))return false;
    runtime::StereoReticle result{};
    if(!projectAimTarget(plan,target,actorGuid,revision,stamp,now,result))return false;
    result.sampleNs=std::min(stamp,native.stamp);
    for(auto& eye:result.eyes)eye.opacity=native.state.alpha;
    out=result;return true;
}
}
