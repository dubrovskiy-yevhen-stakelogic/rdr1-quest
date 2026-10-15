#pragma once
#include "WeaponAttachmentReader.h"
#include "WeaponPoseCallBoundary.h"

namespace rdrvr::engine {
struct WeaponPoseGeometryDelta {float offsetDistance{},rotationDegrees{};bool animated{};};
// Compare two observations surrounding ONE original native Update. World-space
// movement is factored out: this measures authored attachment/muzzle change.
// It is not a persistent calibration or authorization to substitute a pose.
inline bool compareWeaponPoseGeometry(const WeaponAttachmentGeometry& before,const WeaponAttachmentGeometry& after,
                                      const runtime::Mat4& argument,float nativeScalar,WeaponPoseGeometryDelta& out){
    if(!before.weapon||before.weapon!=after.weapon||before.ownerGuid!=after.ownerGuid||before.base!=after.base||
       before.locator!=after.locator||before.modelGuid!=after.modelGuid||before.flags!=after.flags||
       !std::isfinite(nativeScalar)||nativeScalar<=0||after.nativeScale!=nativeScalar||
       !std::isfinite(before.scale)||!std::isfinite(after.scale)||before.scale<=0||after.scale<=0||
       !weapon_pose_detail::rigid(before.localMuzzle)||!weapon_pose_detail::rigid(after.localMuzzle))return false;
    // The native implementation copies twelve input lanes before computing its
    // model. Exact copy equality is appropriate here, unlike derived geometry.
    for(unsigned col=0;col<4;++col)for(unsigned row=0;row<3;++row){
        const unsigned i=col*4+row;if(!std::isfinite(argument[i])||argument[i]!=after.attachment[i])return false;
    }
    double distance2{},trace{};
    for(unsigned row=0;row<3;++row){const double d=double(after.localMuzzle[12+row])-before.localMuzzle[12+row];distance2+=d*d;}
    for(unsigned col=0;col<3;++col)for(unsigned row=0;row<3;++row)trace+=double(before.localMuzzle[col*4+row])*after.localMuzzle[col*4+row];
    WeaponPoseGeometryDelta result{};result.offsetDistance=float(std::sqrt(distance2));
    result.rotationDegrees=float(std::acos(std::clamp((trace-1)*.5,-1.0,1.0))*180.0/3.14159265358979323846);
    result.animated=(after.flags&0x20)!=0;
    if(!std::isfinite(result.offsetDistance)||!std::isfinite(result.rotationDegrees))return false;
    out=result;return true;
}
class WeaponPoseGeometryAudit {
    WeaponAttachmentGeometry before_{};LocalWeapon owner_{};runtime::Mat4 argument_{};
    std::uint64_t stamp_{};float scalar_{};bool flag_{},pending_{};
    static bool sameOwner(const LocalWeapon& a,const LocalWeapon& b){
        return a.weapon==b.weapon&&a.player==b.player&&a.actor==b.actor&&a.actorGuid==b.actorGuid&&
            a.inventory==b.inventory&&a.item==b.item&&a.type.id==b.type.id&&a.type.kind==b.type.kind;
    }
public:
    void reset(){pending_=false;}
    bool pending()const{return pending_;}
    // Caller freshly qualifies ownership/context on its existing native owner
    // thread. Do not use this diagnostic to claim that thread on first use.
    bool begin(WeaponAttachmentApi api,const LocalWeapon& owner,const WeaponPoseCall& call,std::uint64_t stamp)noexcept{
        pending_=false;
        if(!stamp||reinterpret_cast<std::uintptr_t>(call.weapon)!=owner.weapon||!std::isfinite(call.nativeScalar)||call.nativeScalar<=0)return false;
        const WeaponAttachmentReader reader(api);WeaponAttachmentGeometry before{};runtime::Mat4 argument{};
        if(!reader.argument(call.attachment,argument)||!reader.geometry(owner,before))return false;
        owner_=owner;before_=before;argument_=argument;scalar_=call.nativeScalar;flag_=call.nativeFlag;stamp_=stamp;pending_=true;return true;
    }
    bool finish(WeaponAttachmentApi api,const LocalWeapon& owner,const WeaponPoseCall& call,bool prepared,
                std::uint64_t stamp,WeaponPoseGeometryDelta& out)noexcept{
        const bool pending=pending_;pending_=false;
        if(!pending||prepared||stamp<stamp_||stamp-stamp_>100'000'000||!sameOwner(owner_,owner)||
           reinterpret_cast<std::uintptr_t>(call.weapon)!=owner.weapon||call.nativeScalar!=scalar_||call.nativeFlag!=flag_)return false;
        const WeaponAttachmentReader reader(api);runtime::Mat4 argument{};WeaponAttachmentGeometry after{};
        if(!reader.argument(call.attachment,argument)||argument!=argument_||!reader.geometry(owner,after))return false;
        return compareWeaponPoseGeometry(before_,after,argument_,scalar_,out);
    }
};
}
