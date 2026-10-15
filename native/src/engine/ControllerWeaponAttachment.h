#pragma once
#include "ControllerAimTarget.h"
#include "TrackedWeaponPose.h"
#include "WeaponPoseCallBoundary.h"

namespace rdrvr::engine {
struct ControllerAttachmentScope {
    LocalWeapon owner{};
    std::uint64_t revision{},epoch{},reference{},continuity{};
};
inline bool sameAttachmentScope(const ControllerAttachmentScope& a,const ControllerAttachmentScope& b){
    return sameAimOwner(a.owner,b.owner)&&a.revision==b.revision&&a.epoch==b.epoch&&
        a.reference==b.reference&&a.continuity==b.continuity;
}
inline bool sameAttachmentGeometry(const WeaponAttachmentGeometry& a,const WeaponAttachmentGeometry& b){
    return a.weapon==b.weapon&&a.base==b.base&&a.locator==b.locator&&a.ownerGuid==b.ownerGuid&&
        a.modelGuid==b.modelGuid&&a.flags==b.flags&&a.nativeScale==b.nativeScale&&
        a.attachment==b.attachment&&a.model==b.model&&a.muzzle==b.muzzle;
}
struct ControllerAttachmentPlan {
    ControllerAttachmentScope scope{};WeaponAttachmentGeometry before{};
    ConstrainedWeaponPose tracked{};WeaponNativeMatrix argument{};
    std::uint64_t sampledNs{};float nativeScalar{};bool nativeFlag{},continuedPendingFire{};
};
// One instance belongs to the established native owner thread. It does not
// discover/claim that thread, cache an ownership permit, or call native Update.
// The caller freshly qualifies owner/context/render/input both before and after
// prepare; finish runs only after the one original Update returns successfully.
class ControllerWeaponAttachment {
    ControllerAttachmentScope lastScope_{};WeaponAttachmentGeometry lastGeometry_{};
    std::uint64_t completedNs_{};
public:
    void reset(){completedNs_=0;}
    bool muzzleRay(WeaponAttachmentApi api,const ControllerAttachmentScope& scope,std::uint64_t now,
                   const ControllerRay& reference,ControllerRay& out)const noexcept{
        if(!completedNs_||now<completedNs_||now-completedNs_>100'000'000||!sameAttachmentScope(scope,lastScope_)||
           reference.hand!=1||reference.gripPose||reference.trackingEpoch!=scope.epoch||reference.referenceSpaceEpoch!=scope.reference)return false;
        WeaponAttachmentGeometry actual{};
        if(!WeaponAttachmentReader(api).geometry(scope.owner,actual,true)||!sameAttachmentGeometry(actual,lastGeometry_))return false;
        auto ray=reference;
        for(unsigned i=0;i<3;++i){ray.origin[i]=actual.muzzle[12+i];ray.right[i]=actual.muzzle[i]/actual.scale;
            ray.up[i]=actual.muzzle[4+i]/actual.scale;ray.direction[i]=-actual.muzzle[8+i]/actual.scale;}
        out=ray;return true;
    }
    bool prepare(WeaponAttachmentApi api,const NativeAimProbe& physics,const LocalWeapon& owner,
                 const WeaponContext& context,const StereoCameraPlan& camera,const runtime::ControllerSample& input,
                 std::uint64_t revision,std::uint64_t now,const WeaponPoseCall& call,ControllerAttachmentPlan& out)noexcept{
        const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};
        const auto previous=completedNs_;completedNs_=0;
        try{
            if(!now||!input.continuity||reinterpret_cast<std::uintptr_t>(call.weapon)!=owner.weapon||
               !std::isfinite(call.nativeScalar)||call.nativeScalar<=0)return false;
            // Pending aniAim::Fire consumes the PREVIOUS attachment before the
            // new argument is copied. Only a completed, unchanged tracked pose
            // may continue through it; never enter hand mode on that fire.
            auto ordinary=context;ordinary.animationFirePending=false;ControllerRay aim{};
            if(!controllerAimRay(owner,ordinary,camera,input,revision,now,aim))return false;
            ControllerAttachmentPlan result{};
            result.scope={owner,revision,input.epoch,input.referenceSpace,input.continuity};
            WeaponAttachmentReader reader(api);runtime::Mat4 source{},identity{};float scale{};
            if(!reader.argument(call.attachment,source)||!deriveModelMuzzle(source,source,identity,scale)||
               !reader.geometry(owner,result.before,true))return false;
            // Animated weapons update their skeleton from the previous model
            // before copying this argument (UpdateSkeleton at 0x25437a4).
            // A rigid Matrix34 alone cannot prove that skeleton/model epochs
            // agree. Keep native placement until that binding is qualified.
            if(result.before.flags&0x20)return false;
            if(context.animationFirePending&&(!previous||now<previous||now-previous>100'000'000||
               !sameAttachmentScope(result.scope,lastScope_)||!sameAttachmentGeometry(result.before,lastGeometry_)))return false;
            ControllerWeaponFrame frame{};TrackedWeaponPose requested{};
            if(!mapControllerWeaponFrame(camera,input,1,now,frame))return false;
            double reach2{};for(unsigned i=0;i<3;++i){const double d=(double(frame.grip.origin[i])-frame.grip.viewOrigin[i])/frame.grip.unitsPerMetre;reach2+=d*d;}
            if(!std::isfinite(reach2)||reach2>4||!prepareTrackedWeaponPose(frame,result.before.localMuzzle,scale,requested)||
               !constrainTrackedWeaponPose(physics,owner.actorGuid,requested,result.tracked))return false;
            // Bullet-stopper queries may run native callbacks. Re-read borrowed
            // input and the complete weapon geometry before accepting the plan.
            runtime::Mat4 sourceAfter{};WeaponAttachmentGeometry after{};
            if(!reader.argument(call.attachment,sourceAfter)||sourceAfter!=source||!reader.geometry(owner,after,true)||
               !sameAttachmentGeometry(result.before,after))return false;
            result.argument.values=result.tracked.pose.attachmentWorld;result.sampledNs=now;
            result.nativeScalar=call.nativeScalar;result.nativeFlag=call.nativeFlag;
            result.continuedPendingFire=context.animationFirePending;out=result;return true;
        }catch(...){return false;}
    }
    bool finish(WeaponAttachmentApi api,const ControllerAttachmentPlan& plan,const ControllerAttachmentScope& current,
                const WeaponPoseCall& effective,bool prepared,std::uint64_t now,WeaponAttachmentGeometry& out)noexcept{
        const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};reset();
        if(!prepared||!plan.sampledNs||now<plan.sampledNs||now-plan.sampledNs>100'000'000||
           !sameAttachmentScope(plan.scope,current)||reinterpret_cast<std::uintptr_t>(effective.weapon)!=current.owner.weapon||
           effective.nativeScalar!=plan.nativeScalar||effective.nativeFlag!=plan.nativeFlag)return false;
        WeaponAttachmentReader reader(api);runtime::Mat4 argument{};WeaponAttachmentGeometry actual{};
        if(!reader.argument(effective.attachment,argument)||argument!=plan.argument.values||!reader.geometry(current.owner,actual,true)||
           actual.attachment!=argument||actual.nativeScale!=plan.nativeScalar||actual.base!=plan.before.base||
           actual.locator!=plan.before.locator||actual.modelGuid!=plan.before.modelGuid||actual.flags!=plan.before.flags)return false;
        // Native animation may change the local muzzle during Update. Preserve
        // that result and use its actual geometry for next-call continuity.
        // Completion establishes a copied attachment, not shot accuracy.
        lastScope_=current;lastGeometry_=actual;completedNs_=now;out=actual;return true;
    }
};
}
