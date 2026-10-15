#pragma once
#include "ControllerWeaponAttachment.h"
#include "WeaponSkeletonAudit.h"
#include "ScopedWeaponModelPose.h"
namespace rdrvr::engine {
struct AnimatedAttachmentPlan {
    ControllerAttachmentPlan pose{};WeaponModelBinding binding{};WeaponNativeMatrix modelArgument{};
};
inline bool sameWeaponPlacement(const WeaponAttachmentGeometry& a,const WeaponAttachmentGeometry& b){
    return a.weapon==b.weapon&&a.base==b.base&&a.locator==b.locator&&a.ownerGuid==b.ownerGuid&&
        a.modelGuid==b.modelGuid&&a.flags==b.flags&&a.nativeScale==b.nativeScale&&a.attachment==b.attachment&&a.model==b.model;
}
class AnimatedControllerAttachment {
    ControllerAttachmentScope lastScope_{};WeaponAttachmentGeometry lastPlacement_{};std::uint64_t completedNs_{};
    static bool modelMatches(const runtime::Mat4& a,const runtime::Mat4& b){
        for(unsigned col=0;col<4;++col)for(unsigned row=0;row<3;++row){
            const float d=std::abs(a[col*4+row]-b[col*4+row]);
            if(!std::isfinite(d)||d>(col==3?.001f:.0001f))return false;
        }
        return true;
    }
    static bool coherent(WeaponAttachmentApi api,const LocalWeapon& owner,const WeaponAttachmentGeometry& geometry){
        WeaponSkeletonEvidence evidence{};
        return WeaponSkeletonAudit(api).inspect(owner,evidence)&&evidence.flags==geometry.flags&&
            evidence.modelGuid==geometry.modelGuid&&evidence.pose.rootMatchesModel&&evidence.pose.globalsMatchLocals;
    }
public:
    void reset(){completedNs_=0;}
    bool muzzleRay(WeaponAttachmentApi api,const ControllerAttachmentScope& scope,std::uint64_t now,
                   const ControllerRay& reference,ControllerRay& out)const noexcept{
        if(!completedNs_||now<completedNs_||now-completedNs_>100'000'000||!sameAttachmentScope(scope,lastScope_)||
           reference.hand!=1||reference.gripPose||reference.trackingEpoch!=scope.epoch||reference.referenceSpaceEpoch!=scope.reference)return false;
        WeaponAttachmentGeometry actual{},after{};WeaponAttachmentReader reader(api);
        if(!reader.geometry(scope.owner,actual)||!sameWeaponPlacement(actual,lastPlacement_)||!coherent(api,scope.owner,actual)||
           !reader.geometry(scope.owner,after)||!sameAttachmentGeometry(actual,after))return false;
        auto ray=reference;
        for(unsigned i=0;i<3;++i){ray.origin[i]=actual.muzzle[12+i];ray.right[i]=actual.muzzle[i]/actual.scale;
            ray.up[i]=actual.muzzle[4+i]/actual.scale;ray.direction[i]=-actual.muzzle[8+i]/actual.scale;}
        out=ray;return true;
    }
    bool prepare(WeaponAttachmentApi api,WeaponModelBindingApi bindingApi,const NativeAimProbe& physics,
                 const LocalWeapon& owner,const WeaponContext& context,const StereoCameraPlan& camera,
                 const runtime::ControllerSample& input,std::uint64_t revision,std::uint64_t now,
                 const WeaponPoseCall& call,AnimatedAttachmentPlan& out)noexcept{
        const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};
        const auto previous=completedNs_;reset();
        try{
            if(!now||!input.continuity||reinterpret_cast<std::uintptr_t>(call.weapon)!=owner.weapon||
               !std::isfinite(call.nativeScalar)||call.nativeScalar<=0)return false;
            auto ordinary=context;ordinary.animationFirePending=false;ControllerRay aim{};
            if(!controllerAimRay(owner,ordinary,camera,input,revision,now,aim))return false;
            AnimatedAttachmentPlan result{};auto& p=result.pose;p.scope={owner,revision,input.epoch,input.referenceSpace,input.continuity};
            WeaponAttachmentReader reader(api);runtime::Mat4 source{},identity{},modelMuzzle{};float scale{},modelScale{};
            if(!reader.argument(call.attachment,source)||!deriveModelMuzzle(source,source,identity,scale)||
               !reader.geometry(owner,p.before)||!WeaponModelBindingReader(bindingApi).inspect(owner,call.nativeFlag,result.binding)||
               result.binding.modelGuid!=p.before.modelGuid||result.binding.flags!=p.before.flags||result.binding.locator!=p.before.locator||
               !coherent(api,owner,p.before)||!deriveModelMuzzle(p.before.model,p.before.muzzle,modelMuzzle,modelScale))return false;
            // Bones may legitimately animate since the last successful placement.
            // Pending native fire still requires unchanged model/attachment and a
            // freshly coherent skeleton, not a saved muzzle from an old frame.
            if(context.animationFirePending&&(!previous||now<previous||now-previous>100'000'000||
               !sameAttachmentScope(p.scope,lastScope_)||!sameWeaponPlacement(p.before,lastPlacement_)))return false;
            const auto attachmentMuzzle=runtime::multiply(result.binding.localModel,modelMuzzle);
            ControllerWeaponFrame frame{};TrackedWeaponPose requested{};
            if(!mapControllerWeaponFrame(camera,input,1,now,frame))return false;
            double reach2{};for(unsigned i=0;i<3;++i){const double d=(double(frame.grip.origin[i])-frame.grip.viewOrigin[i])/frame.grip.unitsPerMetre;reach2+=d*d;}
            if(!std::isfinite(reach2)||reach2>4||!prepareTrackedWeaponPose(frame,attachmentMuzzle,scale,requested)||
               !constrainTrackedWeaponPose(physics,owner.actorGuid,requested,p.tracked))return false;
            runtime::Mat4 sourceAfter{};WeaponAttachmentGeometry after{};WeaponModelBinding afterBinding{};
            if(!reader.argument(call.attachment,sourceAfter)||sourceAfter!=source||!reader.geometry(owner,after)||
               !sameAttachmentGeometry(p.before,after)||!WeaponModelBindingReader(bindingApi).inspect(owner,call.nativeFlag,afterBinding)||
               afterBinding!=result.binding||!coherent(api,owner,after))return false;
            p.argument.values=p.tracked.pose.attachmentWorld;p.sampledNs=now;p.nativeScalar=call.nativeScalar;
            p.nativeFlag=call.nativeFlag;p.continuedPendingFire=context.animationFirePending;
            result.modelArgument.values=runtime::multiply(p.argument.values,result.binding.localModel);
            out=result;return true;
        }catch(...){return false;}
    }
    bool finish(WeaponAttachmentApi api,WeaponModelBindingApi bindingApi,const AnimatedAttachmentPlan& plan,
                const ControllerAttachmentScope& current,const WeaponPoseCall& effective,bool prepared,
                const ScopedWeaponModelPose& modelCall,std::uint64_t now,WeaponAttachmentGeometry& out)noexcept{
        const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};reset();const auto& p=plan.pose;
        bool nativeResult{};
        if(!prepared||!p.sampledNs||now<p.sampledNs||now-p.sampledNs>100'000'000||!sameAttachmentScope(p.scope,current)||
           reinterpret_cast<std::uintptr_t>(effective.weapon)!=current.owner.weapon||effective.nativeScalar!=p.nativeScalar||
           effective.nativeFlag!=p.nativeFlag||!modelCall.completed(nativeResult)||!nativeResult)return false;
        WeaponAttachmentReader reader(api);runtime::Mat4 argument{};WeaponAttachmentGeometry actual{};WeaponModelBinding binding{};
        if(!reader.argument(effective.attachment,argument)||argument!=p.argument.values||!reader.geometry(current.owner,actual)||
           actual.attachment!=argument||!modelMatches(actual.model,plan.modelArgument.values)||actual.nativeScale!=p.nativeScalar||
           actual.base!=p.before.base||actual.locator!=p.before.locator||actual.modelGuid!=p.before.modelGuid||actual.flags!=p.before.flags||
           !WeaponModelBindingReader(bindingApi).inspect(current.owner,p.nativeFlag,binding)||binding!=plan.binding)return false;
        // This proves only the copied placement and one native model call. Aim
        // publication separately requires current root AND global-bone coherence.
        lastScope_=current;lastPlacement_=actual;completedNs_=now;out=actual;return true;
    }
};
}
