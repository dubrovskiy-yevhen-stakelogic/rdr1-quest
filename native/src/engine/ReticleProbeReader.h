#pragma once
#include "NativeAimProbe.h"
#include "WeaponOwnerReader.h"
#include "ControllerAim.h"
#include "WeaponAttachmentReader.h"
#include "TrackedMuzzleConstraint.h"
#include "TrackedWeaponPose.h"
#include <algorithm>
namespace rdrvr::engine {
struct ReticleProbeApi {
    bool (*read)(void*,std::uintptr_t,void*,std::size_t){};void* context{};
    std::uintptr_t reticleTable{};
    bool (*head)(const void*,AimNativeVector*){};
    NativeAimProbeApi probe{};
    void (*muzzle)(const void*,AimNativeVector*,float,bool){};
    WeaponAttachmentApi attachment{};
    explicit operator bool()const{return read&&reticleTable&&head&&bool(probe)&&muzzle;}
};
struct ReticleProbeSample {
    NativeAimHit hit{};bool headValid{};AimNativeVector head{};float headToRayOrigin{};
    AimNativeVector muzzle{};bool muzzleProbeValid{};NativeAimHit muzzleHit{};float muzzleToTarget{};
    bool attachmentValid{};WeaponAttachmentSample attachment{};
    bool trackedWeaponPoseValid{},trackedWeaponProbeValid{};ConstrainedWeaponPose trackedWeaponPose{};NativeAimHit trackedWeaponHit{};
    bool trackedOriginClamped{};float requestedReach{},safeReach{};
};
class ReticleProbeReader {
    ReticleProbeApi api_;
    template<class T>bool get(std::uintptr_t p,std::size_t offset,T& out)const{
        return p&&p<=UINTPTR_MAX-offset&&p+offset<=UINTPTR_MAX-sizeof(T)&&api_.read(api_.context,p+offset,&out,sizeof(T));
    }
    struct Identity{std::uintptr_t table{},actor{},reticle{},type{};float range{};};
    bool identity(std::uintptr_t reticle,const LocalWeapon& owner,Identity& out)const{
        return reticle%8==0&&get(reticle,0,out.table)&&out.table==api_.reticleTable&&
            get(reticle,0x10,out.actor)&&out.actor==owner.actor&&get(owner.actor,0xb8,out.reticle)&&out.reticle==reticle&&
            get(reticle,8,out.type)&&get(out.type,0x20,out.range)&&std::isfinite(out.range)&&out.range>0;
    }
public:
    explicit ReticleProbeReader(ReticleProbeApi api):api_(api){}
    // Owner/context qualification and the post-camera engine thread are required
    // by the caller. This never edits the native reticle or weapon state.
    bool sample(std::uintptr_t reticle,const LocalWeapon& owner,const ControllerRay& ray,ReticleProbeSample& out,const ControllerWeaponFrame* frame=nullptr)const noexcept{
        const int saved=errno;struct Restore{int saved;~Restore(){errno=saved;}} restore{saved};
        try{
            if(!api_||!owner.actor||!owner.weapon||!ray.sceneId||!ray.controllerSequence||!ray.trackingEpoch||!ray.referenceSpaceEpoch)return false;
            switch(owner.type.kind){case WeaponClass::Pistol:case WeaponClass::DualPistol:case WeaponClass::Rifle:
                case WeaponClass::Repeater:case WeaponClass::SniperRifle:case WeaponClass::Shotgun:break;default:return false;}
            Identity before{},after{};if(!identity(reticle,owner,before))return false;
            ReticleProbeSample result{};
            // false means GetHeadPosition used the body fallback, not a head pose.
            result.headValid=api_.head(reinterpret_cast<const void*>(owner.actor),&result.head);
            if(result.headValid){double gap{};for(unsigned i=0;i<3;++i){
                if(!std::isfinite(result.head.v[i])||!std::isfinite(ray.origin[i]))return false;
                const double d=double(result.head.v[i])-ray.origin[i];gap+=d*d;
            }result.headToRayOrigin=float(std::sqrt(gap));if(!std::isfinite(result.headToRayOrigin))return false;}
            else result.head={};
            const NativeAimProbe physics(api_.probe);
            TrackedMuzzleConstraint constrained{};if(!constrainTrackedMuzzle(physics,owner.actorGuid,ray,constrained))return false;
            const auto& safeRay=constrained.ray;result.trackedOriginClamped=constrained.clamped;
            result.requestedReach=constrained.requestedDistance;result.safeReach=constrained.safeDistance;
            AimNativeVector origin{},direction{};for(unsigned i=0;i<3;++i){origin.v[i]=safeRay.origin[i];direction.v[i]=safeRay.direction[i];}
            if(!physics.cast(owner.actorGuid,origin,direction,std::min(before.range,2000.f),result.hit))return false;
            // Native getter honors its existing launch override and authored locator.
            // Zero extra offset / false skeleton option; no launch override is set here.
            api_.muzzle(reinterpret_cast<const void*>(owner.weapon),&result.muzzle,0.f,false);
            double muzzleDistance2{};
            for(unsigned i=0;i<3;++i){
                if(!std::isfinite(result.muzzle.v[i]))return false;
                const double delta=double(result.hit.point.v[i])-result.muzzle.v[i];muzzleDistance2+=delta*delta;
            }
            result.muzzle.v[3]=0;result.muzzleToTarget=float(std::sqrt(muzzleDistance2));
            if(!std::isfinite(result.muzzleToTarget))return false;
            result.muzzleProbeValid=physics.castBetween(owner.actorGuid,result.muzzle,result.hit.point,result.muzzleHit);
            result.attachmentValid=WeaponAttachmentReader(api_.attachment).sample(owner,safeRay,result.attachment);
            if(frame&&frame->aim==ray&&result.attachmentValid){
                TrackedWeaponPose pose{};
                if(prepareTrackedWeaponPose(*frame,result.attachment.localMuzzle,result.attachment.scale,pose)&&
                   constrainTrackedWeaponPose(physics,owner.actorGuid,pose,result.trackedWeaponPose)){
                    result.trackedWeaponPoseValid=true;AimNativeVector start{},direction{};
                    const auto& tracked=result.trackedWeaponPose.pose.muzzleRay;
                    for(unsigned i=0;i<3;++i){start.v[i]=tracked.origin[i];direction.v[i]=tracked.direction[i];}
                    result.trackedWeaponProbeValid=physics.cast(owner.actorGuid,start,direction,std::min(before.range,2000.f),result.trackedWeaponHit);
                }
            }
            if(!identity(reticle,owner,after)||before.type!=after.type||before.range!=after.range)return false;
            out=result;return true;
        }catch(...){return false;}
    }
};
}
