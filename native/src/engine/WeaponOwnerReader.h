#pragma once
#include "WeaponTypeReader.h"
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <mutex>
#include <atomic>
namespace rdrvr::engine {
struct WeaponOwnerApi {
    bool (*read)(void*,std::uintptr_t,void*,std::size_t){};void* context{};
    std::uintptr_t playerManager{},actorManagerGlobal{},actorSlotsGlobal{};
    std::uintptr_t playerManagerTable{},playerTable{},inventoryTable{},itemTable{},weaponTable{};
    WeaponTypeApi typeApi{};
    std::uintptr_t gohManagerGlobal{},gohSlotsGlobal{};
    std::uintptr_t frameTimer{},frameTimerTable{}; // Optional native timer observation, never an ownership permit.
    explicit operator bool() const{return read&&playerManager&&actorManagerGlobal&&actorSlotsGlobal&&
        playerManagerTable&&playerTable&&inventoryTable&&itemTable&&weaponTable&&bool(typeApi);}
};
struct LocalWeapon {
    WeaponType type{};std::uint32_t actorGuid{};std::uintptr_t player{},actor{},inventory{},item{},weapon{};
};
// Read-only ordinary invWeapon path. Never calls a virtual cast/getter on a
// guessed object, resolves a GUID by index alone, or substitutes a ready slot
// for the actual active item. Other inventory subclasses intentionally decline.
class WeaponOwnerReader {
    WeaponOwnerApi api_{};std::thread::id owner_{};bool owned_{};
    std::atomic<const char*> state_{"NOT_CONFIGURED"};std::mutex mutex_;
    template<class T> bool read(std::uintptr_t object,std::size_t offset,T& out) const {
        if(!object||object>UINTPTR_MAX-offset||object+offset>UINTPTR_MAX-sizeof(T))return false;
        return api_.read(api_.context,object+offset,&out,sizeof(T));
    }
    bool table(std::uintptr_t object,std::uintptr_t expected) const {
        std::uintptr_t actual{};return object%8==0&&read(object,0,actual)&&actual==expected;
    }
    bool fail(const char* why){state_=why;return false;}
public:
    bool onOwnerThread() {std::lock_guard lock(mutex_);return owned_&&owner_==std::this_thread::get_id();}
    void configure(WeaponOwnerApi api){std::lock_guard lock(mutex_);api_=api;state_=api?"WAITING_FOR_LOCAL_WEAPON":"NOT_CONFIGURED";owned_=false;}
    const char* state()const{return state_.load();}
    bool inspect(LocalWeapon& out) noexcept {
        const int saved=errno;struct Restore{int value;~Restore(){errno=value;}} restore{saved};
        try{
            std::lock_guard lock(mutex_);
            if(!api_)return fail("NOT_CONFIGURED");
            if(!owned_){owner_=std::this_thread::get_id();owned_=true;}
            if(owner_!=std::this_thread::get_id())return fail("WEAPON_OBSERVER_WRONG_THREAD");
            LocalWeapon result{};unsigned char active{};
            if(!table(api_.playerManager,api_.playerManagerTable)||!read(api_.playerManager,0xa20,result.player)||
               !table(result.player,api_.playerTable)||!read(result.player,0x5f1,active)||active!=1||
               !read(result.player,0x5f4,result.actorGuid))return fail("LOCAL_PLAYER_UNAVAILABLE");
            std::uintptr_t manager{},slots{};std::uint16_t count{},generation{};
            if(!read(api_.actorManagerGlobal,0,manager)||!read(api_.actorSlotsGlobal,0,slots)||
               !read(manager,0,count))return fail("ACTOR_REGISTRY_UNAVAILABLE");
            const auto index=result.actorGuid&0xffffu;
            if(index>=count||!slots||slots%8||slots>UINTPTR_MAX-std::uintptr_t(index)*16||
               !read(slots,std::uintptr_t(index)*16,result.actor)||
               !read(slots,std::uintptr_t(index)*16+8,generation)||generation!=(result.actorGuid>>16)||!result.actor)
                return fail("LOCAL_ACTOR_GENERATION_MISMATCH");
            if(!read(result.actor,0x70,result.inventory)||!table(result.inventory,api_.inventoryTable)||
               !read(result.inventory,0x80,result.item)||!table(result.item,api_.itemTable)||
               !read(result.item,0xa0,result.weapon)||!table(result.weapon,api_.weaponTable))return fail("ACTIVE_ORDINARY_WEAPON_UNAVAILABLE");
            std::uint32_t weaponGuid{};if(!read(result.weapon,0x20,weaponGuid)||weaponGuid!=result.actorGuid)return fail("WEAPON_OWNER_MISMATCH");
            if(!WeaponTypeReader(api_.typeApi).inspect(result.weapon,result.type))return fail("WEAPON_TYPE_UNAVAILABLE");
            std::uintptr_t playerAgain{},itemAgain{},weaponAgain{},managerAgain{},slotsAgain{},actorAgain{},inventoryAgain{};
            std::uint32_t guidAgain{};std::uint16_t generationAgain{},countAgain{};unsigned char activeAgain{};
            if(!read(api_.playerManager,0xa20,playerAgain)||playerAgain!=result.player||
               !read(result.player,0x5f4,guidAgain)||guidAgain!=result.actorGuid||
               !read(result.player,0x5f1,activeAgain)||activeAgain!=1||
               !read(api_.actorManagerGlobal,0,managerAgain)||managerAgain!=manager||
               !read(api_.actorSlotsGlobal,0,slotsAgain)||slotsAgain!=slots||
               !read(manager,0,countAgain)||countAgain!=count||
               !read(slots,index*16,actorAgain)||actorAgain!=result.actor||
               !read(slots,index*16+8,generationAgain)||generationAgain!=generation||
               !read(result.actor,0x70,inventoryAgain)||inventoryAgain!=result.inventory||
               !read(result.inventory,0x80,itemAgain)||itemAgain!=result.item||
               !read(result.item,0xa0,weaponAgain)||weaponAgain!=result.weapon||
               !read(result.weapon,0x20,weaponGuid)||weaponGuid!=result.actorGuid)return fail("WEAPON_SNAPSHOT_CHANGED");
            out=result;state_="LOCAL_ORDINARY_WEAPON_OBSERVED";return true;
        }catch(...){return fail("WEAPON_OBSERVER_EXCEPTION");}
    }
};
}
