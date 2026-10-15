#pragma once
#include "WeaponOwnerReader.h"
#include <array>
#include <mutex>
namespace rdrvr::engine {
enum class WeaponCallPhase:unsigned {Reticle=1,Pose=2,Aim=3,PoseComplete=4,Manager=5,ManagerComplete=6,Fire=7,FireComplete=8};
struct WeaponCallRecord {
    WeaponCallPhase phase{};std::uint64_t stamp{},thread{},reticleOrdinal{};
    std::uint32_t targetGuid{};int mode{},targetIndex{-1};std::uint16_t instance{},part{};unsigned flags{};
    float nativeScalar{};std::uint64_t nativeTimerOrdinal{}; // Native uint32 counter + 1; zero means unavailable.
    int projectileCount{},fireMode{},fireValue{};
};
struct WeaponCallSnapshot {
    std::array<WeaponCallRecord,128> records{};unsigned used{};std::uint64_t overwritten{},ignored{};
    std::uint32_t actorGuid{};int weaponTypeId{};bool bound{};
};
// Diagnostic association with a recently qualified local owner, not a permit
// to change the weapon. Keeps only a bounded tail and does not retain pointers
// to any native call arguments or results.
class WeaponCallTrace {
    mutable std::mutex mutex_;WeaponOwnerApi api_{};LocalWeapon owner_{};
    std::uint64_t boundNs_{};bool bound_{};WeaponCallSnapshot data_{};unsigned next_{};
    template<class T>bool get(std::uintptr_t p,std::size_t offset,T& out)const{
        return p&&p<=UINTPTR_MAX-offset&&p+offset<=UINTPTR_MAX-sizeof(T)&&api_.read(api_.context,p+offset,&out,sizeof(T));
    }
    std::uint64_t timerOrdinal()const{
        std::uintptr_t table{},again{};std::uint32_t before{},after{};
        if(!api_.frameTimer||!api_.frameTimerTable||!get(api_.frameTimer,0,table)||table!=api_.frameTimerTable||
           !get(api_.frameTimer,0x5c,before)||!get(api_.frameTimer,0x5c,after)||before!=after||
           !get(api_.frameTimer,0,again)||again!=table)return 0;
        return std::uint64_t(before)+1;
    }
public:
    static constexpr std::uint64_t maxAgeNs=100'000'000;
    void configure(WeaponOwnerApi api){std::lock_guard lock(mutex_);api_=api;bound_=false;}
    void bind(const LocalWeapon& owner,std::uint64_t now){
        std::lock_guard lock(mutex_);
        if(owner.weapon!=owner_.weapon||owner.actor!=owner_.actor||owner.player!=owner_.player||
           owner.item!=owner_.item||owner.inventory!=owner_.inventory||owner.actorGuid!=owner_.actorGuid||
           owner.type.id!=owner_.type.id||owner.type.kind!=owner_.type.kind){data_={};next_=0;}
        owner_=owner;boundNs_=now;bound_=now&&owner.actor&&owner.weapon&&api_.read&&api_.weaponTable;
        data_.actorGuid=owner.actorGuid;data_.weaponTypeId=owner.type.id;
    }
    void invalidate(){std::lock_guard lock(mutex_);bound_=false;}
    bool record(std::uintptr_t object,const WeaponCallRecord& record)noexcept{
        const int saved=errno;struct Restore{int saved;~Restore(){errno=saved;}} restore{saved};
        try{
            std::lock_guard lock(mutex_);
            if(!bound_||!record.thread||record.stamp<boundNs_||record.stamp-boundNs_>maxAgeNs){++data_.ignored;return false;}
            std::uintptr_t table{},actor{},reticle{};std::uint32_t guid{};
            bool valid=get(owner_.weapon,0,table)&&table==api_.weaponTable&&get(owner_.weapon,0x20,guid)&&guid==owner_.actorGuid;
            if(record.phase==WeaponCallPhase::Reticle)valid=valid&&get(object,0x10,actor)&&actor==owner_.actor&&get(actor,0xb8,reticle)&&reticle==object;
            else valid=valid&&object==owner_.weapon&&(record.phase==WeaponCallPhase::Aim||record.phase==WeaponCallPhase::Pose||record.phase==WeaponCallPhase::PoseComplete||
                record.phase==WeaponCallPhase::Manager||record.phase==WeaponCallPhase::ManagerComplete||
                record.phase==WeaponCallPhase::Fire||record.phase==WeaponCallPhase::FireComplete);
            if(!valid){++data_.ignored;return false;}
            auto observed=record;observed.nativeTimerOrdinal=timerOrdinal();
            data_.records[next_]=observed;next_=(next_+1)%unsigned(data_.records.size());
            if(data_.used<data_.records.size())++data_.used;else ++data_.overwritten;
            return true;
        }catch(...){return false;}
    }
    WeaponCallSnapshot snapshot()const{
        std::lock_guard lock(mutex_);auto out=data_;out.bound=bound_;
        const unsigned first=data_.used<data_.records.size()?0:next_;
        for(unsigned i=0;i<data_.used;++i)out.records[i]=data_.records[(first+i)%unsigned(data_.records.size())];
        return out;
    }
};
}
