#pragma once
#include "SkeletonPoseAudit.h"
#include "ControllerWeaponAttachment.h"
namespace rdrvr::engine {
struct WeaponSkeletonEvidence {SkeletonPoseEvidence pose{};std::uint32_t flags{},modelGuid{};};
class WeaponSkeletonAudit {
    WeaponAttachmentApi api_;
public:
    explicit WeaponSkeletonAudit(WeaponAttachmentApi api):api_(api){}
    // The caller supplies a freshly qualified owner on the existing game
    // thread. Native GetSkeleton(false) cannot request skeleton creation.
    // Nothing from this sample is a cached authorization for a later write.
    bool inspect(const LocalWeapon& owner,WeaponSkeletonEvidence& out)const noexcept{
        const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};
        try{
            if(!api_.skeleton)return false;
            WeaponAttachmentReader geometry(api_);WeaponAttachmentGeometry before{},after{};
            if(!geometry.geometry(owner,before))return false;
            const auto object=reinterpret_cast<void*>(owner.weapon);
            const auto skeleton=reinterpret_cast<std::uintptr_t>(api_.skeleton(object,false));
            if(!skeleton)return false;
            WeaponSkeletonEvidence result{};
            if(!SkeletonPoseAudit({api_.read,api_.context}).inspect(skeleton,before.model,result.pose)||
               reinterpret_cast<std::uintptr_t>(api_.skeleton(object,false))!=skeleton||
               !geometry.geometry(owner,after)||!sameAttachmentGeometry(before,after))return false;
            result.flags=before.flags;result.modelGuid=before.modelGuid;out=result;return true;
        }catch(...){return false;}
    }
};
}
