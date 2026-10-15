#pragma once
#include "WeaponPosePlan.h"
#include "WeaponOwnerReader.h"
#include <cstring>
#include <limits>
namespace rdrvr::engine {
struct alignas(16) WeaponNativeMatrix {runtime::Mat4 values{};};
static_assert(sizeof(WeaponNativeMatrix)==64);
struct WeaponAttachmentApi {
    bool (*read)(void*,std::uintptr_t,void*,std::size_t){};void* context{};
    std::uintptr_t weaponTable{};
    void (*muzzleMatrix)(const void*,WeaponNativeMatrix*,bool){};
    const WeaponNativeMatrix* (*worldMatrix)(const void*,bool){};
    void* (*skeleton)(void*,bool){};
    explicit operator bool()const{return read&&weaponTable&&muzzleMatrix;}
};
struct WeaponAttachmentSample {runtime::Mat4 localMuzzle{};float scale{};WeaponPosePlan handPose{};};
struct WeaponAttachmentGeometry {
    std::uintptr_t weapon{},base{},locator{};std::uint32_t ownerGuid{},modelGuid{},flags{};
    float nativeScale{},scale{};
    runtime::Mat4 attachment{},model{},muzzle{},localMuzzle{};
};
class WeaponAttachmentReader {
    WeaponAttachmentApi api_;
    template<class T>bool get(std::uintptr_t p,std::size_t offset,T& out)const{
        return p&&p<=UINTPTR_MAX-offset&&p+offset<=UINTPTR_MAX-sizeof(T)&&api_.read(api_.context,p+offset,&out,sizeof(T));
    }
    struct State {
        std::uintptr_t table{},base{},locator{};std::uint32_t guid{},modelGuid{},flags{};float nativeScale{};
        WeaponNativeMatrix attachment{},model{};
    };
    bool state(const LocalWeapon& owner,State& result)const{
        return get(owner.weapon,0,result.table)&&result.table==api_.weaponTable&&
            get(owner.weapon,0x20,result.guid)&&result.guid==owner.actorGuid&&get(owner.weapon,0x28,result.base)&&result.base&&
            get(owner.weapon,0x338,result.locator)&&get(owner.weapon,0x2bc,result.modelGuid)&&
            get(owner.weapon,0x120,result.flags)&&get(owner.weapon,0xc8,result.nativeScale)&&
            std::isfinite(result.nativeScale)&&result.nativeScale>0&&
            get(owner.weapon,0x40,result.attachment)&&get(owner.weapon,0x80,result.model);
    }
    static runtime::Mat4 affine(WeaponNativeMatrix matrix){
        matrix.values[3]=matrix.values[7]=matrix.values[11]=0;matrix.values[15]=1;return matrix.values;
    }
    static bool sameMatrix(const WeaponNativeMatrix& a,const WeaponNativeMatrix& b){
        // Matrix34's fourth lanes are not geometry or an identity revision.
        // Compare only its twelve authored values, allowing padding to change.
        for(unsigned col=0;col<4;++col)for(unsigned row=0;row<3;++row)
            if(a.values[col*4+row]!=b.values[col*4+row])return false;
        return true;
    }
public:
    explicit WeaponAttachmentReader(WeaponAttachmentApi api):api_(api){}
    // weapWeapon::Update takes the attachment matrix (+0x40), then multiplies
    // authored locator transforms to produce +0x80. Supplying a model matrix
    // directly as its argument would apply the weapon's authored offset twice.
    bool geometry(const LocalWeapon& owner,WeaponAttachmentGeometry& out,bool requireDirectModel=false)const noexcept{
        const int saved=errno;struct Restore{int saved;~Restore(){errno=saved;}} restore{saved};
        try{
            if(!api_||!owner.weapon)return false;
            State before{},after{};if(!state(owner,before))return false;
            runtime::Mat4 rootBefore{},rootAfter{};const WeaponNativeMatrix* root{};
            if(requireDirectModel){
                if(!api_.worldMatrix)return false;
                root=api_.worldMatrix(reinterpret_cast<const void*>(owner.weapon),false);
                // GetWeaponMatrix(false) returns +0x80 on the no-skeleton
                // path; otherwise it returns skeleton+0x10's root pointer.
                // Equal values alone cannot prove global bone matrices fresh.
                if(owner.weapon>UINTPTR_MAX-0x80||reinterpret_cast<std::uintptr_t>(root)!=owner.weapon+0x80||
                   !argument(root,rootBefore)||rootBefore!=affine(before.model))return false;
            }
            WeaponNativeMatrix muzzle{};
            // Poison meaningful lanes to reject an incomplete native result.
            for(float& x:muzzle.values)x=std::numeric_limits<float>::quiet_NaN();
            api_.muzzleMatrix(reinterpret_cast<const void*>(owner.weapon),&muzzle,false);
            if(requireDirectModel){
                if(api_.worldMatrix(reinterpret_cast<const void*>(owner.weapon),false)!=root||
                   !argument(root,rootAfter)||rootAfter!=rootBefore)return false;
            }
            if(!state(owner,after)||before.base!=after.base||before.locator!=after.locator||
               before.modelGuid!=after.modelGuid||before.flags!=after.flags||before.nativeScale!=after.nativeScale||
               !sameMatrix(before.attachment,after.attachment)||!sameMatrix(before.model,after.model))return false;
            WeaponAttachmentGeometry result{};
            runtime::Mat4 attachmentToModel{};float modelScale{};
            // A changed/invalid weapon model cannot be accepted just because
            // the actor's old attachment happened to stay in place.
            if(!deriveModelMuzzle(affine(before.attachment),affine(before.model),attachmentToModel,modelScale))return false;
            if(!deriveModelMuzzle(affine(before.attachment),affine(muzzle),result.localMuzzle,result.scale))return false;
            result.weapon=owner.weapon;result.ownerGuid=owner.actorGuid;result.base=before.base;result.locator=before.locator;
            result.modelGuid=before.modelGuid;result.flags=before.flags;result.nativeScale=before.nativeScale;
            result.attachment=affine(before.attachment);result.model=affine(before.model);result.muzzle=affine(muzzle);
            out=result;return true;
        }catch(...){return false;}
    }
    bool argument(const WeaponNativeMatrix* borrowed,runtime::Mat4& out)const noexcept{
        const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};
        try{
            WeaponNativeMatrix matrix{};const auto address=reinterpret_cast<std::uintptr_t>(borrowed);
            if(!api_.read||!get(address,0,matrix))return false;
            const auto pose=affine(matrix);runtime::Mat4 identity{};float scale{};
            if(!deriveModelMuzzle(pose,pose,identity,scale))return false;
            out=pose;return true;
        }catch(...){return false;}
    }
    bool sample(const LocalWeapon& owner,const ControllerRay& hand,WeaponAttachmentSample& out)const noexcept{
        const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};
        WeaponAttachmentGeometry observed{};if(!geometry(owner,observed))return false;
        WeaponAttachmentSample result{};result.localMuzzle=observed.localMuzzle;result.scale=observed.scale;
        if(!prepareWeaponPose(hand,result.localMuzzle,result.scale,result.handPose))return false;
        out=result;return true;
    }
};
}
