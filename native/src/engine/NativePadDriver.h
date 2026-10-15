#pragma once
#include "NativePadEvents.h"
#include <atomic>
#include <thread>

namespace rdrvr::engine {
struct NativePadApi {
    bool (*read)(void*,std::uintptr_t,void*,std::size_t){};
    void* (*getSystem)(){};
    void* (*getPad)(const void*,int){};
    void* (*getInfo)(void*,int){};
    void (*connect)(void*,int,int,int,const char*){};
    void (*activate)(void*,void*){};
    void (*touchMode)(void*,bool){};
    NativePadEventApi events{};
    std::uintptr_t systemVtable{},padVtable{};
    void (*setOverride)(void*,void*){};
    explicit operator bool()const{return read&&getSystem&&getPad&&getInfo&&connect&&activate&&touchMode&&events.axis&&events.button&&systemVtable&&padVtable;}
};
// Runs immediately before the pinned Android -> base InputSystem::Update call.
// Native APIs own allocation, raw state, edge computation and later destruction.
class NativePadDriver {
public:
    using Movement=bool(*)(void*,const runtime::ControllerSample&,std::uint64_t,std::array<float,2>&);
    using HorizontalLook=bool(*)(void*,const runtime::ControllerSample&,std::uint64_t,float&);
    // Last word on the pad state before it is submitted (VR holsters: grip masking, weapon-hand trigger
    // routing). input is null when no ready Touch sample exists (the filter suspends); state.ready may be false.
    using PadFilter=void(*)(void*,const runtime::ControllerSample*,std::uint64_t,runtime::PadState&);
private:
    Movement movement_{};void* movementContext_{};
    HorizontalLook look_{};void* lookContext_{};
    PadFilter filter_{};void* filterContext_{};std::atomic<std::uint64_t> filterRejected_{};
    PadFilter controls_{};void* controlsContext_{};std::atomic<std::uint64_t> controlsRejected_{}; // Controls: button layout, BEFORE filter_
    // A filter may rewrite buttons/axes, never readiness; out-of-range or non-finite output is discarded.
    // controls: bit 11 (PadDpadUp, output only) is a valid pad button.
    static bool saneFilterOutput(const runtime::PadState& before,const runtime::PadState& after){
        if(after.ready!=before.ready||(after.buttons&~0xfffu))return false;
        for(float x:after.sticks)if(!std::isfinite(x)||std::abs(x)>1.001f)return false;
        for(float x:after.triggers)if(!std::isfinite(x)||x<0.f||x>1.001f)return false;
        return true;
    }
    NativePadApi api_{};runtime::ControllerMailbox& mailbox_;runtime::ControllerPolicy policy_;
    bool routed_{},routePending_{};
    void* system_{};void* pad_{};bool ownerSet_{},failed_{};std::thread::id owner_{};
    std::atomic<const char*> status_{"NOT_CONFIGURED"};
    std::atomic<std::uint64_t> updates_{},neutral_{},created_{};
    inline static thread_local const NativePadDriver* outputDriver_{};
    runtime::ControllerSample outputInput_{};std::uint64_t outputNs_{};
    bool table(const void* object,std::uintptr_t expected)const {
        std::uintptr_t found{};return object&&api_.read(nullptr,reinterpret_cast<std::uintptr_t>(object),&found,sizeof(found))&&found==expected;
    }
    bool overrideSlot(void*& value,std::size_t offset=0x68)const {
        return system_&&api_.read(nullptr,reinterpret_cast<std::uintptr_t>(system_)+offset,&value,sizeof(value));
    }
    bool registeredPad()const {
        auto* info=api_.getInfo(system_,deviceId);void* registered{};
        if(!info||!api_.read(nullptr,reinterpret_cast<std::uintptr_t>(info)+0x40,&registered,sizeof(registered))||
           registered!=pad_||!table(pad_,api_.padVtable))return false;
        if(!routed_)return api_.getPad(system_,0)==pad_;
        std::array<void*,4> slots{};
        return api_.read(nullptr,reinterpret_cast<std::uintptr_t>(system_)+0x28,slots.data(),sizeof(slots))&&
            std::count(slots.begin(),slots.end(),pad_)==1;
    }

public:
    static constexpr int deviceId=0x52445256;
    explicit NativePadDriver(runtime::ControllerMailbox& mailbox):mailbox_(mailbox){}
    ~NativePadDriver(){if(outputDriver_==this)outputDriver_=nullptr;}
    void configure(NativePadApi api){api_=api;status_=api?"WAITING_FOR_TOUCH":"NOT_CONFIGURED";}
    void setMovement(Movement movement,void* context=nullptr){movement_=movement;movementContext_=context;}
    void setHorizontalLook(HorizontalLook look,void* context=nullptr){look_=look;lookContext_=context;}
    void setPadFilter(PadFilter filter,void* context=nullptr){filter_=filter;filterContext_=context;}
    std::uint64_t filterRejected()const{return filterRejected_.load();}
    void setControlsFilter(PadFilter filter,void* context=nullptr){controls_=filter;controlsContext_=context;}
    std::uint64_t controlsRejected()const{return controlsRejected_.load();}
    const char* status()const{return status_.load();}
    std::uint64_t updates()const{return updates_.load();}
    std::uint64_t neutralUpdates()const{return neutral_.load();}
    std::uint64_t created()const{return created_.load();}
    // Called on the same native input thread after a menu/world transition.
    void rearm() noexcept {policy_.invalidate();if(outputDriver_==this)outputDriver_=nullptr;}
    // Output is accepted only on the native thread that most recently supplied
    // ready Touch input, and only while slot zero still belongs to the driver's virtual pad.
    bool outputInput(std::uint64_t now,runtime::ControllerSample& out)const noexcept {
        const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};
        try{
            if(outputDriver_!=this||now<outputNs_||now-outputNs_>100'000'000||!pad_||failed_)return false;
            if(api_.getSystem()!=system_||!table(system_,api_.systemVtable)||api_.getPad(system_,0)!=pad_||!table(pad_,api_.padVtable))return false;
            const auto info=reinterpret_cast<std::uintptr_t>(api_.getInfo(system_,deviceId));void* registered{};
            if(!info||info>UINTPTR_MAX-0x48||!api_.read(nullptr,info+0x40,&registered,sizeof(registered))||registered!=pad_)return false;
            runtime::ControllerSample current{};
            if(!mailbox_.snapshot(now,current)||current.continuity!=outputInput_.continuity||
               (current.inputEpoch?current.inputEpoch:current.epoch)!=(outputInput_.inputEpoch?outputInput_.inputEpoch:outputInput_.epoch))return false;
            out=current;return true;
        }catch(...){return false;}
    }
    void beforeUpdate(void* system,std::uint64_t now) noexcept {
        const int saved=errno;
        struct Restore {int value;~Restore(){errno=value;}} restore{saved};
        if(outputDriver_==this)outputDriver_=nullptr;
        routePending_=false;
        try{
            if(!api_){status_="NOT_CONFIGURED";return;}
            if(system!=api_.getSystem()){status_="INPUT_SYSTEM_MISMATCH";return;}
            if(!table(system,api_.systemVtable)){status_="INPUT_SYSTEM_VTABLE_MISMATCH";return;}
            if(!ownerSet_){owner_=std::this_thread::get_id();ownerSet_=true;}
            if(owner_!=std::this_thread::get_id()){status_="INPUT_THREAD_MISMATCH";return;}
            if(system_&&system_!=system){pad_=nullptr;routed_=false;policy_.invalidate();failed_=false;}
            system_=system;
            // The native base Update visits physical slots AND the override.
            // Clear only the driver's own override around it so pressed edges are computed once.
            if(routed_){
                void* current{};
                if(!overrideSlot(current)||(current&&current!=pad_)){status_="PAD_OVERRIDE_CHANGED";return;}
                if(current){api_.setOverride(system_,nullptr);if(!overrideSlot(current)||current){status_="PAD_OVERRIDE_CLEAR_FAILED";return;}}
            }
            runtime::ControllerSample sample{};const bool available=mailbox_.snapshot(now,sample);
            auto state=policy_.sample(available?&sample:nullptr);
            auto* info=api_.getInfo(system,deviceId);
            if(pad_){
                if(!registeredPad()){
                    pad_=nullptr;policy_.invalidate();status_="VIRTUAL_PAD_REMOVED";return;
                }
            }else{
                if(failed_){status_="INPUT_QUARANTINED";return;}
                if(info){status_="DEVICE_ID_COLLISION";return;}
                if(!state.ready){status_=available?"WAITING_FOR_NEUTRAL_TOUCH":"WAITING_FOR_FRESH_TOUCH";return;}
                // Keep Oculus/physical devices in their native slots. The native
                // override routes ioPad(0) only after the base update completes.
                const bool occupied=api_.getPad(system,0)!=nullptr;
                if(occupied){
                    void* override{},*touchOverride{};
                    if(!api_.setOverride){status_="EXTERNAL_PAD_SLOT_ZERO";return;}
                    if(!overrideSlot(override)||override||!overrideSlot(touchOverride,0x70)||touchOverride){status_="EXTERNAL_PAD_OVERRIDE";return;}
                }
                api_.connect(system,deviceId,0x045e,0,"RDRVR OpenXR controller");
                info=api_.getInfo(system,deviceId);void* registered{};
                if(!info||!api_.read(nullptr,reinterpret_cast<std::uintptr_t>(info)+0x40,&registered,sizeof(registered))||
                   !registered||(!occupied&&registered!=api_.getPad(system,0))||!table(registered,api_.padVtable)){
                    failed_=true;status_="VIRTUAL_PAD_CREATION_FAILED";return;
                }
                pad_=registered;routed_=occupied;
                if(!registeredPad()){failed_=true;status_="PAD_REGISTRATION_MISMATCH";return;}
                api_.activate(system,info);api_.touchMode(system,false);++created_;
            }
            if(failed_)state={};
            if(state.ready&&movement_){
                auto moved=std::array<float,2>{state.sticks[0],state.sticks[1]};
                try{
                    if(movement_(movementContext_,sample,now,moved)&&std::isfinite(moved[0])&&std::isfinite(moved[1])&&
                       std::abs(moved[0])<=1.001f&&std::abs(moved[1])<=1.001f){
                        state.sticks[0]=std::clamp(moved[0],-1.f,1.f);state.sticks[1]=std::clamp(moved[1],-1.f,1.f);
                    }
                }catch(...){/* A movement provider cannot quarantine native buttons or rumble. */}
            }
            if(state.ready&&look_){
                float x=state.sticks[2];
                try{
                    if(look_(lookContext_,sample,now,x)&&std::isfinite(x)&&std::abs(x)<=1.001f)
                        state.sticks[2]=std::clamp(x,-1.f,1.f);
                }catch(...){/* Preserve all native axes/buttons if the provider fails. */}
            }
            if(controls_){ // Controls: Touch -> game button layout (VrControls.h), every update that reaches submission
                auto mapped=state;
                try{
                    controls_(controlsContext_,state.ready&&available?&sample:nullptr,now,mapped);
                    if(saneFilterOutput(state,mapped))state=mapped;else ++controlsRejected_;
                }catch(...){++controlsRejected_;}
            }
            if(filter_){ // Runs on every update that reaches submission, ready or neutral
                auto filtered=state;
                try{
                    filter_(filterContext_,state.ready&&available?&sample:nullptr,now,filtered);
                    if(saneFilterOutput(state,filtered))state=filtered;else ++filterRejected_;
                }catch(...){++filterRejected_;}
            }
            if(!submitPadEvents(api_.events,pad_,deviceId,state)){
                // Best-effort release uses the same checked owning object.
                submitPadEvents(api_.events,pad_,deviceId,{});failed_=true;policy_.invalidate();status_="PAD_EVENTS_FAILED";return;
            }
            if(state.ready){
                bool active=state.buttons!=0;
                for(float x:state.sticks)active=active||std::abs(x)>0.15f;
                for(float x:state.triggers)active=active||x>0.15f;
                if(active){api_.activate(system,info);api_.touchMode(system,false);}
                ++updates_;status_="TOUCH_PAD_UPDATED";outputInput_=sample;outputNs_=now;outputDriver_=this;
            }else{++neutral_;status_="TOUCH_PAD_NEUTRAL";}
            routePending_=routed_;
        }catch(...){failed_=true;policy_.invalidate();status_="PAD_API_EXCEPTION";}
    }
    void afterUpdate(void* system,std::uint64_t) noexcept {
        const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};
        if(!routePending_)return;routePending_=false;
        try{
            void* current{},*touchOverride{};
            if(owner_!=std::this_thread::get_id()||system!=system_||api_.getSystem()!=system_||
               !table(system_,api_.systemVtable)||!registeredPad()||!overrideSlot(current)||current||
               !overrideSlot(touchOverride,0x70)||touchOverride){status_="PAD_ROUTE_REJECTED";return;}
            api_.setOverride(system_,pad_);
            if(!overrideSlot(current)||current!=pad_||api_.getPad(system_,0)!=pad_){status_="PAD_ROUTE_FAILED";return;}
        }catch(...){failed_=true;status_="PAD_ROUTE_EXCEPTION";}
    }

};
}
