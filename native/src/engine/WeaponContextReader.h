#pragma once
#include "WeaponOwnerReader.h"
#include <array>
#include <cmath>
#include <cstring>
namespace rdrvr::engine {
struct WeaponContext {
    bool alive{},controlDisabled{},deadEye{},riding{},ridingVehicle{},vehicleTransition{};
    bool duel{},mountedWeapon{},mountingHorse{},blindFiring{},cover{},linkedAnimation{},usingGringo{};
    bool trackedShot{},aimDirectionOverride{},launchOverride{},driveLaunchOverride{};
    bool animationFirePending{};
    // Preserve raw references alongside their generation-checked native validity.
    std::uint32_t linkedActorGuid{},gringoGuid{};
    std::uint32_t flags()const {
        return (!alive?1u:0u)|(controlDisabled?2u:0u)|(deadEye?4u:0u)|(riding?8u:0u)|
            (ridingVehicle?16u:0u)|(vehicleTransition?32u:0u)|(duel?64u:0u)|(mountedWeapon?128u:0u)|
            (mountingHorse?256u:0u)|(blindFiring?512u:0u)|(cover?1024u:0u)|(linkedAnimation?2048u:0u)|(usingGringo?4096u:0u)|
            (trackedShot?8192u:0u)|(aimDirectionOverride?16384u:0u)|(launchOverride?32768u:0u)|(driveLaunchOverride?65536u:0u)|
            (animationFirePending?131072u:0u);
    }
    bool knownLock()const{return !alive||controlDisabled||deadEye||riding||ridingVehicle||vehicleTransition||
        duel||mountedWeapon||mountingHorse||blindFiring||cover||linkedAnimation||usingGringo;}
    // Separate from gameplay locks: observation may continue through a native
    // launch override, but a future hand-aim provider must not replace that shot.
    bool physicalAimBlocked()const{return knownLock()||trackedShot||aimDirectionOverride||launchOverride||driveLaunchOverride||animationFirePending;}
};
class WeaponContextReader {
    WeaponOwnerApi api_;
    struct Entry{std::uintptr_t address{};std::array<std::byte,8> value{};std::size_t size{};};
    struct Journal {
        const WeaponOwnerApi& api;std::array<Entry,80> entries{};unsigned used{};
        template<class T>bool get(std::uintptr_t object,std::size_t offset,T& out){
            static_assert(sizeof(T)<=8);
            if(!object||object>UINTPTR_MAX-offset||object+offset>UINTPTR_MAX-sizeof(T)||used==entries.size())return false;
            auto& e=entries[used];e.address=object+offset;e.size=sizeof(T);
            if(!api.read(api.context,e.address,e.value.data(),e.size))return false;
            std::memcpy(&out,e.value.data(),sizeof(T));++used;return true;
        }
        bool resolve(std::uintptr_t managerGlobal,std::uintptr_t slotsGlobal,std::uint32_t guid,
                     bool& valid,std::uintptr_t* object=nullptr){
            std::uintptr_t manager{},slots{};std::uint16_t count{},generation{};
            if(!get(managerGlobal,0,manager)||!get(slotsGlobal,0,slots)||!slots||slots%8||!get(manager,0,count))return false;
            valid=false;const auto index=guid&0xffffu;
            if(index>=count)return true;
            if(!get(slots,std::uintptr_t(index)*16+8,generation))return false;
            valid=generation==(guid>>16);
            if(valid&&object&&!get(slots,std::uintptr_t(index)*16,*object))return false;
            return true;
        }
        bool stable(){for(unsigned i=0;i<used;++i){std::array<std::byte,8> now{};const auto& e=entries[i];
            if(!api.read(api.context,e.address,now.data(),e.size)||std::memcmp(e.value.data(),now.data(),e.size))return false;
        }return true;}
    };
    // Call only on the current owner's native thread, with a fresh LocalWeapon.
    // No native methods/virtual casts, no mutations and no use as a cached permit.
    bool inspectImpl(const LocalWeapon& owner,WeaponContext& out,bool weapon)const noexcept {
        const int saved=errno;struct Restore{int value;~Restore(){errno=value;}} restore{saved};
        try {
            if(!api_.read||!owner.player||!owner.actor||(weapon&&(!api_.weaponTable||!owner.weapon)))return false;
            Journal r{api_};WeaponContext result{};bool validActor{};std::uintptr_t resolvedActor{};
            std::uintptr_t current{},table{},health{},deadEye{},vehicle{},behavior{},contexts{},duel{},mounted{},mount{},aim{},linked{},motion{},component{};
            std::uint32_t guid{},state{},eyeState{},duelState{},mountedState{},mountState{},shotFlags{};
            std::array<unsigned char,3> launchFlags{};
            unsigned char animationFire{};
            std::uint16_t locks{},aimFlags{};unsigned char active{},lock1{},lock2{},enter{},leave{},cover{};std::int32_t enterState{};float hp{};
            if(!r.get(api_.playerManager,0xa20,current)||current!=owner.player||
               !r.get(owner.player,0,table)||table!=api_.playerTable||!r.get(owner.player,0x5f1,active)||active!=1||
               !r.get(owner.player,0x5f4,guid)||guid!=owner.actorGuid||
               (weapon&&(!r.get(owner.weapon,0,table)||table!=api_.weaponTable||!r.get(owner.weapon,0x20,guid)||guid!=owner.actorGuid))||
               !r.resolve(api_.actorManagerGlobal,api_.actorSlotsGlobal,owner.actorGuid,validActor,&resolvedActor)||
               !validActor||resolvedActor!=owner.actor)return false;
            // sagActor::IsAlive, sagPlayerStates::CheckIsPlayerControlDisabled,
            // sagPlayer::IsRiding/IsRidingVehicle and Dead Eye state readers.
            if(!r.get(owner.actor,0x60,health)||!r.get(health,0x20,hp)||!std::isfinite(hp)||
               !r.get(owner.player,0x104,locks)||!r.get(owner.player,0x10c,lock1)||!r.get(owner.player,0x10d,lock2)||
               !r.get(owner.player,0x5d4,state)||!r.get(owner.player,0x410,deadEye)||!r.get(deadEye,0x20,eyeState)||
               !r.get(owner.player,0x418,vehicle)||!r.get(vehicle,8,enter)||!r.get(vehicle,9,leave)||!r.get(vehicle,0x18,enterState))return false;
            result.alive=hp>0;result.controlDisabled=locks||lock1||lock2;result.deadEye=eyeState!=0;
            result.riding=state==5;result.ridingVehicle=state==3;result.vehicleTransition=(enter&&enterState!=3)||leave;
            // Vector Aim does not clear a previous tracked-shot bit. Native
            // ManagerAim / FireProjectile and the launch-override setters use
            // these fields independently of Dead Eye's current player state.
            if(weapon){
                if(!r.get(owner.weapon,0x188,shotFlags)||!r.get(owner.weapon,0x9c1,launchFlags)||!r.get(owner.weapon,0x9d0,animationFire))return false;
                result.trackedShot=(shotFlags&0x20)!=0;result.aimDirectionOverride=launchFlags[0]!=0;
                result.launchOverride=launchFlags[1]!=0;result.driveLaunchOverride=launchFlags[2]!=0;
                // Update consumes this and calls aniAim::Fire before copying its
                // new attachment. A new pose must not be assumed to govern that fire.
                result.animationFirePending=animationFire!=0;
            }
            // Exact action-context paths from the audited sagPlayerStates
            // getters. Missing required contexts decline instead of inferring
            // that a partially created/destroyed actor is in ordinary gameplay.
            if(!r.get(owner.actor,0x38,behavior)||!r.get(behavior,0xab8,contexts)||
               !r.get(contexts,0x20,duel)||!r.get(duel,0x30,duelState)||
               !r.get(contexts,0x28,mounted)||!r.get(mounted,0x30,mountedState)||
               !r.get(contexts,0x68,mount)||!r.get(mount,0x18,mountState)||
               !r.get(contexts,0x18,aim)||!r.get(aim,0xbc,aimFlags)||
               !r.get(contexts,0x50,linked)||!r.get(linked,0x18,result.linkedActorGuid)||
               !r.get(owner.actor,0x88,motion)||!r.get(owner.actor,0xb0,component)||!r.get(component,0x8d0,result.gringoGuid))return false;
            result.duel=duelState!=0;result.mountedWeapon=mountedState!=0;
            result.mountingHorse=mountState==1||mountState==5||mountState==6;result.blindFiring=(aimFlags&4)!=0;
            if(motion&&!r.get(motion,0xaca,cover))return false;
            result.cover=cover!=0;
            if(!r.resolve(api_.actorManagerGlobal,api_.actorSlotsGlobal,result.linkedActorGuid,result.linkedAnimation))return false;
            // CheckIsUsingGringo explicitly rejects zero before the goh registry.
            if(result.gringoGuid&&!r.resolve(api_.gohManagerGlobal,api_.gohSlotsGlobal,result.gringoGuid,result.usingGringo))return false;
            if(!r.stable())return false;
            out=result;return true;
        }catch(...){return false;}
    }
public:
    explicit WeaponContextReader(WeaponOwnerApi api):api_(api){}
    bool inspect(const LocalWeapon& owner,WeaponContext& out)const noexcept{return inspectImpl(owner,out,true);}
    // Shared player restrictions without requiring an equipped item. Weapon
    // fields are intentionally unavailable here; use only knownLock().
    bool inspectPlayer(const LocalWeapon& player,WeaponContext& out)const noexcept{return inspectImpl(player,out,false);}
};
}
