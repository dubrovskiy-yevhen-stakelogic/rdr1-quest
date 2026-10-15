#pragma once
#include "WeaponPosePlan.h"
#include "TrackedMuzzleConstraint.h"
namespace rdrvr::engine {
struct ControllerWeaponFrame {ControllerRay aim{},grip{};};
inline bool mapControllerWeaponFrame(const StereoCameraPlan& plan,const runtime::ControllerSample& input,
                                    unsigned hand,std::uint64_t now,ControllerWeaponFrame& out){
    ControllerWeaponFrame frame{};
    if(!mapControllerRay(plan,input,hand,now,frame.aim)||!mapControllerRay(plan,input,hand,now,frame.grip,true))return false;
    out=frame;return true;
}
struct TrackedWeaponPose {
    runtime::Mat4 attachmentWorld{},muzzleWorld{};ControllerRay muzzleRay{};
    float attachmentScale{};
};
// Place the native attachment at the grip, orient the authored muzzle along
// aim, and retain roll from the grip. Never put the muzzle at the controller's
// origin: the authored barrel/attachment offset must stay in front of the hand.
// Geometry must be freshly sampled from the same qualified native weapon. This
// pure plan is not a calibration cache or an instruction to call Update again.
inline bool prepareTrackedWeaponPose(const ControllerWeaponFrame& frame,const runtime::Mat4& localMuzzle,
                                     float attachmentScale,TrackedWeaponPose& out){
    using namespace runtime;const auto& aim=frame.aim;const auto& grip=frame.grip;
    if(aim.gripPose||!grip.gripPose||aim.hand>1||aim.hand!=grip.hand||
       !aim.sceneId||aim.sceneId!=grip.sceneId||!aim.controllerSequence||aim.controllerSequence!=grip.controllerSequence||
       !aim.trackingEpoch||aim.trackingEpoch!=grip.trackingEpoch||!aim.referenceSpaceEpoch||aim.referenceSpaceEpoch!=grip.referenceSpaceEpoch||
       !std::isfinite(aim.unitsPerMetre)||aim.unitsPerMetre<.01f||aim.unitsPerMetre>100||aim.unitsPerMetre!=grip.unitsPerMetre||
       aim.viewOrigin!=grip.viewOrigin)return false;
    for(float x:grip.origin)if(!std::isfinite(x))return false;
    const auto cross=[](Vec3 a,Vec3 b){return Vec3{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};};
    const auto& f=aim.direction;auto up=grip.up;
    const float directionLength=weapon_pose_detail::dot(f,f),upLength=weapon_pose_detail::dot(up,up);
    if(!std::isfinite(directionLength)||std::abs(directionLength-1)>.0001f||
       !std::isfinite(upLength)||std::abs(upLength-1)>.0001f)return false;
    const float along=weapon_pose_detail::dot(up,f);for(unsigned i=0;i<3;++i)up[i]-=along*f[i];
    const float length=std::sqrt(weapon_pose_detail::dot(up,up));if(!std::isfinite(length)||length<.01f)return false;
    for(float& x:up)x/=length;
    auto aligned=aim;aligned.right=cross(f,up);aligned.up=cross(aligned.right,f);
    WeaponPosePlan basis{};if(!prepareWeaponPose(aligned,localMuzzle,attachmentScale,basis))return false;
    TrackedWeaponPose result{};result.attachmentWorld=basis.modelWorld;result.attachmentScale=attachmentScale;
    for(unsigned i=0;i<3;++i)result.attachmentWorld[12+i]=grip.origin[i];
    result.muzzleWorld=multiply(result.attachmentWorld,localMuzzle);
    result.muzzleRay=aligned;
    for(unsigned i=0;i<3;++i)result.muzzleRay.origin[i]=result.muzzleWorld[12+i];
    for(float x:result.muzzleWorld)if(!std::isfinite(x))return false;
    out=result;return true;
}
struct ConstrainedWeaponPose {TrackedWeaponPose pose{};bool clamped{};float requestedReach{},safeReach{};};
// Use the native view-to-muzzle guard on the actual authored muzzle. Translate
// the entire planned attachment by the same correction so the barrel, ray and
// native muzzle stay coherent. This does not rotate the shot or alter spread.
inline bool constrainTrackedWeaponPose(const NativeAimProbe& physics,std::uint32_t actorGuid,
                                      const TrackedWeaponPose& requested,ConstrainedWeaponPose& out)noexcept{
    const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};
    runtime::Mat4 local{};float scale{};
    if(!std::isfinite(requested.attachmentScale)||requested.attachmentScale<=0||
       !deriveModelMuzzle(requested.attachmentWorld,requested.muzzleWorld,local,scale)||
       std::abs(scale-requested.attachmentScale)>requested.attachmentScale*.0001f||
       requested.muzzleRay.gripPose||requested.muzzleRay.hand>1)return false;
    for(unsigned i=0;i<3;++i){
        if(requested.muzzleRay.origin[i]!=requested.muzzleWorld[12+i]||
           std::abs(requested.muzzleWorld[i]/scale-requested.muzzleRay.right[i])>.0001f||
           std::abs(requested.muzzleWorld[4+i]/scale-requested.muzzleRay.up[i])>.0001f||
           std::abs(requested.muzzleWorld[8+i]/scale+requested.muzzleRay.direction[i])>.0001f)return false;
        if(!std::isfinite(requested.muzzleRay.right[i])||!std::isfinite(requested.muzzleRay.up[i])||!std::isfinite(requested.muzzleRay.direction[i]))return false;
    }
    TrackedMuzzleConstraint safe{};
    if(!constrainTrackedMuzzle(physics,actorGuid,requested.muzzleRay,safe))return false;
    ConstrainedWeaponPose result{};result.pose=requested;result.clamped=safe.clamped;
    result.requestedReach=safe.requestedDistance;result.safeReach=safe.safeDistance;
    for(unsigned i=0;i<3;++i){const float d=safe.ray.origin[i]-requested.muzzleRay.origin[i];
        result.pose.attachmentWorld[12+i]+=d;result.pose.muzzleWorld[12+i]=safe.ray.origin[i];}
    result.pose.muzzleRay=safe.ray;
    for(float x:result.pose.attachmentWorld)if(!std::isfinite(x))return false;
    for(float x:result.pose.muzzleWorld)if(!std::isfinite(x))return false;
    out=result;return true;
}
}
