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
    // Why a read failed (the riding turn counts declines by reason): 0 ok, Unavailable (identity / cells), Transition.
    int read(State& s)const{
        std::uintptr_t table{},top{};std::uint32_t transition{};
        if(!get(api_.camera.singleton,0,s.manager)||!get(s.manager,0,table)||!api_.camera.matchesManager(table)||
           !get(s.manager,8,s.channel)||!get(s.channel,0,table)||table!=api_.camera.channelTable||
           !get(s.manager,0x28,s.game)||!get(s.channel,0x18,s.node)||!get(s.node,0x10,top)||top!=s.game||
           !get(s.node,8,transition))return int(Turn::Unavailable);
        if(transition)return int(Turn::Transition);
        if(!get(s.game,0,table)||table!=api_.gameTable||
           !get(s.game,0x620,s.cmx)||!get(s.cmx,0,table)||table!=api_.cmxTable||
           !get(s.cmx,0x10,s.machine)||!get(s.machine,0x10,s.variables)||!get(s.variables,8,s.table)||
           !get(s.cmx,0x30,s.parameters)||!get(s.parameters,8,s.parameterTable)||!get(s.parameterTable,0x2e0,s.reset)||
           !get(s.reset,0,s.resetValue)||s.resetValue>1)return int(Turn::Unavailable);
        constexpr std::array<std::size_t,5> offsets{0x20,0x28,0x18,0x68,0x48};
        for(unsigned i=0;i<offsets.size();++i){
            if(!get(s.table,offsets[i],s.cells[i])||!get(s.cells[i],0,s.values[i])||
               !std::isfinite(s.values[i])||std::abs(s.values[i])>1000.f)return int(Turn::Unavailable);
        }
        return 0;
    }
public:
    enum class Turn : int {Applied,Range,Unavailable,Transition,Unstable,Exception};
    explicit NativeCameraTurn(NativeCameraTurnApi api):api_(api){}
    // Same established input owner thread only. All identities and values are
    // re-read immediately before the one native setter call; no state retained.
    Turn turn(float delta,std::array<float,3>& applied)const noexcept{
        const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};
        try{
            if(!api_)return Turn::Unavailable;
            if(!std::isfinite(delta)||std::abs(delta)<.001f||std::abs(delta)>std::numbers::pi_v<float>/2)return Turn::Range;
            State before{},after{};
            if(const int r=read(before))return Turn(r);
            if(const int r=read(after))return Turn(r);
            if(before!=after)return Turn::Unstable;
            const auto& v=before.values;
            // GetEulers adds these three native float cells in this order.
            const float yaw=(v[2]+v[3])+v[4];
            std::array<float,3> eulers{v[0],std::remainder(yaw+delta,2*std::numbers::pi_v<float>),v[1]};
            api_.setOrientation(reinterpret_cast<void*>(before.game),eulers);
            applied=eulers;return Turn::Applied;
        }catch(...){return Turn::Exception;}
    }
    bool apply(float delta,std::array<float,3>& applied)const noexcept{return turn(delta,applied)==Turn::Applied;}
    // RIDING ALIGN: would turn() reach the setter now (the same identities / cells / no transition, nothing called)?
    Turn probe()const noexcept{
        const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};
        try{if(!api_)return Turn::Unavailable;State s{};if(const int r=read(s))return Turn(r);return Turn::Applied;}catch(...){return Turn::Exception;}
    }
    // HORSE-RELATIVE STEERING: the camGlueCMXGame that sagPlayerInputHorse reads ([[camManager]]+0x28, ApplyInput 0x26b8c1c
    // ldr w8,[x8,#0x638]) and its horse-control cells. Same manager / channel / game identities as the turn (no transition or CMX
    // requirement: the flag belongs to the game camera object, whatever shot is on top). Read only; GameXr writes +0x638 / +0x63c
    // right after this check on the same main thread, and only when sane (the layout was verified in the variant-1
    // binary only - a vtable match alone never licenses a write into a field that does not look like the horse flag / device int).
    struct HorseControl {
        std::uintptr_t game{};
        std::int32_t relative{-1}; // +0x638: 0 = camera-relative stick, != 0 = horse-relative (ctor 0x262a890 = !CameraRelativeControl)
        std::int32_t device{-1};   // +0x63c: input device 0 pad / 1 KBM / 2 touch; 3 = PreUpdate 0x262f430 re-copies the player's option
        std::array<std::uint8_t,3> option{}; // +0x1a24 pad / +0x1a25 KBM / +0x1a26 touch: the player's 'Horse Control' option (1 = horse)
        bool sane{}; // relative 0 / 1, device 0..3, each option byte 0 / 1 (ctor 0x262a890 bool xor, PreUpdate / SyncOptionsToGame bools)
    };
    bool horseControl(HorseControl& out)const noexcept{
        const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};
        try{
            std::uintptr_t manager{},channel{},game{},table{};HorseControl h{};
            if(!api_||!get(api_.camera.singleton,0,manager)||!get(manager,0,table)||!api_.camera.matchesManager(table)||
               !get(manager,8,channel)||!get(channel,0,table)||table!=api_.camera.channelTable||
               !get(manager,0x28,game)||!get(game,0,table)||table!=api_.gameTable||
               !get(game,0x638,h.relative)||!get(game,0x63c,h.device)||!get(game,0x1a24,h.option))return false;
            h.sane=(h.relative==0||h.relative==1)&&h.device>=0&&h.device<=3&&h.option[0]<=1&&h.option[1]<=1&&h.option[2]<=1;
            h.game=game;out=h;return true;
        }catch(...){return false;}
    }
};
}
