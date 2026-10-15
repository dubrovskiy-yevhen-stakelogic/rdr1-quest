#pragma once
#include "WeaponOwnerReader.h"
#include <cmath>
#include <cstring>
namespace rdrvr::engine {
struct NativeReticleVisibility {
    std::uint32_t actorGuid{},prevention{};float alpha{};bool visible{};
};
// Read-only counterpart of sagPlayerStates::AllowDrawReticle and the fade
// published by EvaluateReticleDraw. Sample on the existing native owner thread;
// never set prevention flags or UI alpha to hide the game's other indicators.
class NativeReticleVisibilityReader {
    WeaponOwnerApi api_;
    template<class T>bool get(std::uintptr_t p,std::size_t offset,T& out)const{
        return api_.read&&p&&p<=UINTPTR_MAX-offset&&p+offset<=UINTPTR_MAX-sizeof(T)&&api_.read(api_.context,p+offset,&out,sizeof out);
    }
    struct State {
        std::uintptr_t player{},table{},states{},backref{};std::uint32_t guid{},prevention{};
        unsigned char active{};float alpha{};bool operator==(const State&)const=default;
    };
    bool sample(const LocalWeapon& owner,State& s)const{
        return get(api_.playerManager,0xa20,s.player)&&s.player==owner.player&&
            get(s.player,0,s.table)&&s.table==api_.playerTable&&get(s.player,0x5f1,s.active)&&s.active==1&&
            get(s.player,0x5f4,s.guid)&&s.guid==owner.actorGuid&&get(s.player,0x410,s.states)&&s.states%8==0&&
            get(s.states,0,s.backref)&&s.backref==owner.player&&get(s.states,0xc,s.prevention)&&
            get(s.states,0x10,s.alpha)&&std::isfinite(s.alpha)&&s.alpha>=0&&s.alpha<=1;
    }
public:
    explicit NativeReticleVisibilityReader(WeaponOwnerApi api):api_(api){}
    bool inspect(const LocalWeapon& owner,NativeReticleVisibility& out)const noexcept{
        const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};
        try{
            if(!owner.player||!owner.actorGuid||!api_.playerManager||!api_.playerTable)return false;
            State a{},b{};if(!sample(owner,a)||!sample(owner,b)||a!=b)return false;
            out={a.guid,a.prevention,a.alpha,a.prevention==0&&a.alpha>0};return true;
        }catch(...){return false;}
    }
};
struct NativeReticleFrame {
    NativeReticleVisibility state{};std::uint64_t stamp{},revision{};
    bool allows(std::uint32_t actorGuid,std::uint64_t currentRevision,std::uint64_t now)const{
        return state.visible&&state.prevention==0&&std::isfinite(state.alpha)&&state.alpha>0&&state.alpha<=1&&
            actorGuid&&state.actorGuid==actorGuid&&revision&&revision==currentRevision&&
            stamp&&now>=stamp&&now-stamp<=100'000'000;
    }
};
}
