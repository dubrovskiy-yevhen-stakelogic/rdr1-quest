#pragma once
#include "WeaponContextReader.h"
namespace rdrvr::engine {
struct LocomotionApi {WeaponOwnerApi player{};std::uintptr_t footTable{};};
// playerMode = sagPlayer+0x5d4 (2 = on foot, sagPlayer::UpdateInput 0x2698218 switch), same stable read.
struct LocomotionContext {bool ordinaryFoot{};std::uint32_t actorGuid{};std::uint32_t playerMode{};};
// Read on the established native input thread. No game methods or writes.
class LocomotionContextReader {
    LocomotionApi api_;
    template<class T>bool get(std::uintptr_t p,std::size_t o,T& out)const{
        return p&&p<=UINTPTR_MAX-o&&p+o<=UINTPTR_MAX-sizeof(T)&&api_.player.read(api_.player.context,p+o,&out,sizeof(T));
    }
    struct State {
        std::uintptr_t player{},actor{},manager{},slots{},foot{},motion{};
        std::uint32_t guid{},mode{},footMode{},controlMode{},moverMode{},moverSpecial{};
        std::int32_t scripted{};std::uint16_t count{},generation{},moveLocks{};
        unsigned char active{},overrideVector{},forceLock{},footDisabled{};
        bool operator==(const State&)const=default;
    };
    bool state(State& s)const{
        const auto& a=api_.player;std::uintptr_t table{},back{};
        if(!a.read||!api_.footTable||!a.playerManagerTable||!a.playerTable||!get(a.playerManager,0,table)||table!=a.playerManagerTable||
           !get(a.playerManager,0xa20,s.player)||!get(s.player,0,table)||table!=a.playerTable||
           !get(s.player,0x5f1,s.active)||s.active!=1||!get(s.player,0x5f4,s.guid)||
           !get(a.actorManagerGlobal,0,s.manager)||!get(a.actorSlotsGlobal,0,s.slots)||!s.slots||s.slots%8||
           !get(s.manager,0,s.count))return false;
        const auto index=s.guid&0xffffu;
        if(index>=s.count||!get(s.slots,index*16,s.actor)||!s.actor||
           !get(s.slots,index*16+8,s.generation)||s.generation!=(s.guid>>16)||!get(s.player,0x5d4,s.mode))return false;
        // UpdateInput's exact state-2 branch uses player+118 (InputFoot).
        // Other states retain native axes without interpreting their layouts.
        if(s.mode!=2)return true;
        return get(s.player,0x118,s.foot)&&get(s.foot,0,table)&&table==api_.footTable&&
            get(s.foot,0x40,back)&&back==s.player&&get(s.foot,0x190,s.footMode)&&get(s.foot,0x15c,s.footDisabled)&&
            get(s.player,0x108,s.moveLocks)&&get(s.player,0x390,s.scripted)&&
            get(s.player,0x18,s.controlMode)&&get(s.player,0x50,s.overrideVector)&&get(s.player,0x51,s.forceLock)&&
            get(s.actor,0x88,s.motion)&&get(s.motion,0x928,s.moverMode)&&get(s.motion,0xaf0,s.moverSpecial);
    }
public:
    explicit LocomotionContextReader(LocomotionApi api):api_(api){}
    bool sample(LocomotionContext& out)const noexcept{
        const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};
        try{
            State before{},after{};if(!state(before))return false;
            LocomotionContext result{};result.actorGuid=before.guid;result.playerMode=before.mode;
            if(before.mode==2){
                LocalWeapon player{};player.player=before.player;player.actor=before.actor;player.actorGuid=before.guid;
                WeaponContext restriction{};
                if(!WeaponContextReader(api_.player).inspectPlayer(player,restriction))return false;
                // ControlVector state 1 is ordinary directional hysteresis and
                // can turn on during walking. Do not toggle mapping on that
                // transition; the original engine keeps its normal base lock.
                result.ordinaryFoot=!restriction.knownLock()&&!before.moveLocks&&before.scripted<0&&
                    before.controlMode<=1&&!before.overrideVector&&!before.forceLock&&!before.footMode&&
                    !(before.footDisabled&1)&&!before.moverMode&&!before.moverSpecial;
            }
            if(!state(after)||before!=after)return false;
            out=result;return true;
        }catch(...){return false;}
    }
};
}
