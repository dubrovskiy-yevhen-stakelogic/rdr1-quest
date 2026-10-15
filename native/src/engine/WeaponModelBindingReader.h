#pragma once
#include "WeaponAttachmentReader.h"

namespace rdrvr::engine {
struct WeaponModelBindingApi {
    bool (*read)(void*,std::uintptr_t,void*,std::size_t){};void* context{};
    std::uintptr_t weaponTable{},managerGlobal{},slotsGlobal{},propTable{},artPropTable{};
};
struct WeaponModelBinding {
    std::uintptr_t weapon{},model{},table{},fragment{},resource{},locator{};
    std::uint32_t modelGuid{},flags{};runtime::Mat4 localModel{};
    bool operator==(const WeaponModelBinding&)const=default;
};
// Read-only preconditions for the SetTransform call already made by native
// weapWeapon::UpdateSkeleton. Does not move a model, patch a vtable, resolve a
// GUID by index alone or authorize calls outside the qualified outer Update.
class WeaponModelBindingReader {
    WeaponModelBindingApi api_;
    template<class T>bool get(std::uintptr_t p,std::size_t offset,T& out)const{
        return api_.read&&p&&p<=UINTPTR_MAX-offset&&p+offset<=UINTPTR_MAX-sizeof(T)&&api_.read(api_.context,p+offset,&out,sizeof out);
    }
    struct State {
        std::uintptr_t weaponTable{},base{},manager{},slots{},model{},table{},fragment{},header{},resource{},skeletonData{},locator{};
        std::uint32_t ownerGuid{},modelGuid{},flags{},skeletonState{},headerFlags{};
        std::uint16_t count{},generation{};runtime::Mat4 localModel{};
        bool operator==(const State&)const=default;
    };
    bool state(const LocalWeapon& owner,bool nativeFlag,State& s)const{
        if(!get(owner.weapon,0,s.weaponTable)||s.weaponTable!=api_.weaponTable||
           !get(owner.weapon,0x20,s.ownerGuid)||s.ownerGuid!=owner.actorGuid||!get(owner.weapon,0x28,s.base)||!s.base||
           !get(owner.weapon,0x120,s.flags)||!(s.flags&0x20)||!get(owner.weapon,0x2bc,s.modelGuid)||
           !get(owner.weapon,0x2c0,s.skeletonState)||s.skeletonState!=2||!get(owner.weapon,0x338,s.locator)||
           !get(api_.managerGlobal,0,s.manager)||!get(api_.slotsGlobal,0,s.slots)||!s.slots||s.slots%8||
           !get(s.manager,0,s.count)||(s.modelGuid&0xffffu)>=s.count||
           !get(s.slots,std::uintptr_t(s.modelGuid&0xffffu)*16+8,s.generation)||s.generation!=(s.modelGuid>>16)||
           !get(s.slots,std::uintptr_t(s.modelGuid&0xffffu)*16,s.model)||s.model%8||!get(s.model,0,s.table))return false;
        // These two audited goh types have DIFFERENT interface/fragment offsets.
        std::size_t fragmentOffset{};
        if(api_.propTable&&s.table==api_.propTable)fragmentOffset=0xd0;
        else if(api_.artPropTable&&s.table==api_.artPropTable)fragmentOffset=0x70;
        else return false;
        if(!get(s.model,fragmentOffset,s.fragment)||!get(s.fragment,0x78,s.header))return false;
        if(s.header){
            if(!get(s.header,0xc,s.headerFlags)||(s.headerFlags&0x30000020u)!=0x30000000u||!get(s.header,0,s.resource))return false;
        }else if(!get(s.fragment,0x98,s.resource))return false;
        if(!get(s.resource,0xc8,s.skeletonData)||!s.skeletonData)return false;
        s.localModel={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        if(s.locator){
            WeaponNativeMatrix native{};if(!get(s.locator,nativeFlag?0x100:0xc0,native))return false;
            s.localModel=native.values;s.localModel[3]=s.localModel[7]=s.localModel[11]=0;s.localModel[15]=1;
            if(!weapon_pose_detail::rigid(s.localModel))return false;
        }
        return true;
    }
public:
    explicit WeaponModelBindingReader(WeaponModelBindingApi api):api_(api){}
    bool inspect(const LocalWeapon& owner,bool nativeFlag,WeaponModelBinding& out)const noexcept{
        const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};
        try{
            if(!owner.weapon||!owner.actorGuid||!api_.weaponTable||!api_.managerGlobal||!api_.slotsGlobal)return false;
            State a{},b{};if(!state(owner,nativeFlag,a)||!state(owner,nativeFlag,b)||a!=b)return false;
            out={owner.weapon,a.model,a.table,a.fragment,a.resource,a.locator,a.modelGuid,a.flags,a.localModel};return true;
        }catch(...){return false;}
    }
};
}
