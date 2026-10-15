#pragma once
#include "NativeCameraContext.h"
#include <array>
#include <cmath>
#include <numbers>

namespace rdrvr::engine {
struct NativeCameraTurnApi {
    NativeCameraApi camera{};
    std::uintptr_t gameTable{},cmxTable{};
    void (*setOrientation)(void*,const std::array<float,3>&){};
    explicit operator bool()const{return camera&&gameTable&&cmxTable&&setOrientation;}
};
// Exact retail camGlueCMXGame / camCMXCamera layout. Read the variable cells
// directly: GetCurrentEulers returns a nontrivial Vector3 through ARM64 x8.
// The setter takes a borrowed const Vector3&, and also resets the CMX offsets
// and requests a camera reset through its parameter set. Never write cells.
class NativeCameraTurn {
    NativeCameraTurnApi api_;
    template<class T>bool get(std::uintptr_t p,std::size_t offset,T& out)const{
        return p&&p<=UINTPTR_MAX-offset&&p+offset<=UINTPTR_MAX-sizeof(T)&&
            api_.camera.read(api_.camera.context,p+offset,&out,sizeof(T));
    }
    struct State {
        std::uintptr_t manager{},channel{},node{},game{},cmx{},machine{},variables{},table{},parameters{},parameterTable{},reset{};
        std::array<std::uintptr_t,5> cells{};
        std::array<float,5> values{};
        unsigned char resetValue{};
        bool operator==(const State&)const=default;
    };
    bool read(State& s)const{
        std::uintptr_t table{},top{};std::uint32_t transition{};
        if(!get(api_.camera.singleton,0,s.manager)||!get(s.manager,0,table)||!api_.camera.matchesManager(table)||
           !get(s.manager,8,s.channel)||!get(s.channel,0,table)||table!=api_.camera.channelTable||
           !get(s.manager,0x28,s.game)||!get(s.channel,0x18,s.node)||!get(s.node,0x10,top)||top!=s.game||
           !get(s.node,8,transition)||transition||!get(s.game,0,table)||table!=api_.gameTable||
           !get(s.game,0x620,s.cmx)||!get(s.cmx,0,table)||table!=api_.cmxTable||
           !get(s.cmx,0x10,s.machine)||!get(s.machine,0x10,s.variables)||!get(s.variables,8,s.table)||
           !get(s.cmx,0x30,s.parameters)||!get(s.parameters,8,s.parameterTable)||!get(s.parameterTable,0x2e0,s.reset)||
           !get(s.reset,0,s.resetValue)||s.resetValue>1)return false;
        constexpr std::array<std::size_t,5> offsets{0x20,0x28,0x18,0x68,0x48};
        for(unsigned i=0;i<offsets.size();++i){
            if(!get(s.table,offsets[i],s.cells[i])||!get(s.cells[i],0,s.values[i])||
               !std::isfinite(s.values[i])||std::abs(s.values[i])>1000.f)return false;
        }
        return true;
    }
public:
    explicit NativeCameraTurn(NativeCameraTurnApi api):api_(api){}
    // Same established input owner thread only. All identities and values are
    // re-read immediately before the one native setter call; no state retained.
    bool apply(float delta,std::array<float,3>& applied)const noexcept{
        const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};
        try{
            if(!api_||!std::isfinite(delta)||std::abs(delta)<.001f||std::abs(delta)>std::numbers::pi_v<float>/2)return false;
            State before{},after{};if(!read(before)||!read(after)||before!=after)return false;
            const auto& v=before.values;
            // GetEulers adds these three native float cells in this order.
            const float yaw=(v[2]+v[3])+v[4];
            std::array<float,3> eulers{v[0],std::remainder(yaw+delta,2*std::numbers::pi_v<float>),v[1]};
            api_.setOrientation(reinterpret_cast<void*>(before.game),eulers);
            applied=eulers;return true;
        }catch(...){return false;}
    }
};
}
