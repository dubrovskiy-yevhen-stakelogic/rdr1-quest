#pragma once
#include "WeaponAimBoundary.h"
#include "WeaponContextReader.h"
#include "TrackedMuzzleConstraint.h"
#include <algorithm>
namespace rdrvr::engine {
inline bool controllerAimFirearm(WeaponClass kind){
    return kind==WeaponClass::Pistol||kind==WeaponClass::Rifle||
        kind==WeaponClass::Repeater||kind==WeaponClass::Shotgun;
}
inline bool sameAimOwner(const LocalWeapon& a,const LocalWeapon& b){
    return a.actorGuid==b.actorGuid&&a.player==b.player&&a.actor==b.actor&&a.inventory==b.inventory&&
        a.item==b.item&&a.weapon==b.weapon&&a.type.id==b.type.id&&a.type.kind==b.type.kind;
}
// Qualify a target replacement, not a projectile or attachment replacement.
// Native firing still starts at the animated muzzle and owns all ballistics.
inline bool controllerAimRay(const LocalWeapon& owner,const WeaponContext& context,
                             const StereoCameraPlan& plan,const runtime::ControllerSample& input,
                             std::uint64_t revision,std::uint64_t now,ControllerRay& out){
    if(!owner.player||!owner.actor||!owner.inventory||!owner.item||!owner.weapon||!owner.actorGuid||
       !controllerAimFirearm(owner.type.kind)||context.physicalAimBlocked()||!revision||
       !plan.headAnchored||plan.actorGuid!=owner.actorGuid||plan.presentationRevision!=revision||
       !plan.headSampledNs||now<plan.headSampledNs||now-plan.headSampledNs>100'000'000)return false;
    ControllerRay ray{};if(!mapControllerRay(plan,input,1,now,ray))return false;
    // Reject an implausible tracking discontinuity before any engine query.
    double distance2{};for(unsigned i=0;i<3;++i){const double d=(double(ray.origin[i])-ray.viewOrigin[i])/ray.unitsPerMetre;distance2+=d*d;}
    if(!std::isfinite(distance2)||distance2>4)return false;
    out=ray;return true;
}
struct ControllerAimTargetApi {
    bool (*read)(void*,std::uintptr_t,void*,std::size_t){};void* context{};
    std::uintptr_t reticleTable{};NativeAimProbeApi probe{};
};
struct ControllerAimTarget {
    WeaponAimVector point{};ControllerRay ray{};float distance{};bool blocked{},wallClamped{};
};
class ControllerAimTargetReader {
    ControllerAimTargetApi api_;
    template<class T>bool get(std::uintptr_t p,std::size_t offset,T& out)const{
        return p&&p<=UINTPTR_MAX-offset&&p+offset<=UINTPTR_MAX-sizeof(T)&&api_.read(api_.context,p+offset,&out,sizeof(T));
    }
    struct Identity{std::uintptr_t reticle{},table{},actor{},type{};float range{};bool operator==(const Identity&)const=default;};
    bool identity(const LocalWeapon& owner,Identity& out)const{
        return get(owner.actor,0xb8,out.reticle)&&out.reticle%8==0&&get(out.reticle,0,out.table)&&out.table==api_.reticleTable&&
            get(out.reticle,0x10,out.actor)&&out.actor==owner.actor&&get(out.reticle,8,out.type)&&
            get(out.type,0x20,out.range)&&std::isfinite(out.range)&&out.range>0;
    }
public:
    explicit ControllerAimTargetReader(ControllerAimTargetApi api):api_(api){}
    // Synchronous on the already-qualified native owner/input thread. Reticle
    // identity and range are read again after both bullet-stopper queries.
    // No borrowed physics pointers, native target GUIDs or old targets escape.
    bool sample(const LocalWeapon& owner,const ControllerRay& ray,ControllerAimTarget& out)const noexcept{
        const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};
        try{
            if(!api_.read||!api_.reticleTable||!api_.probe||!owner.actor||!owner.weapon||
               !controllerAimFirearm(owner.type.kind)||ray.hand!=1||ray.gripPose)return false;
            double direction2{};for(float x:ray.direction){if(!std::isfinite(x))return false;direction2+=double(x)*x;}
            if(std::abs(direction2-1)>0.0001)return false;
            Identity before{},after{};if(!identity(owner,before))return false;
            NativeAimProbe physics(api_.probe);TrackedMuzzleConstraint constrained{};
            if(!constrainTrackedMuzzle(physics,owner.actorGuid,ray,constrained))return false;
            AimNativeVector origin{},direction{};for(unsigned i=0;i<3;++i){origin.v[i]=constrained.ray.origin[i];direction.v[i]=constrained.ray.direction[i];}
            NativeAimHit hit{};if(!physics.cast(owner.actorGuid,origin,direction,std::min(before.range,2000.f),hit)||
                !identity(owner,after)||before!=after)return false;
            // A target coincident with the source has no usable aim direction.
            if(hit.distance<.01f*ray.unitsPerMetre)return false;
            ControllerAimTarget result{};result.point={hit.point.v[0],hit.point.v[1],hit.point.v[2],0};
            result.ray=constrained.ray;result.distance=hit.distance;result.blocked=hit.blocked;result.wallClamped=constrained.clamped;
            out=result;return true;
        }catch(...){return false;}
    }
};
}
