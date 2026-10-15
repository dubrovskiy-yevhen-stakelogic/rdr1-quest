#pragma once
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace rdrvr::engine {
struct ReplayRenderPhaseApi {
    using Read=bool(*)(void*,std::uintptr_t,void*,std::size_t);
    using Write=bool(*)(void*,std::uintptr_t,const void*,std::size_t);
    Read read{};Write write{};void* user{};bool (*ownsThread)(){};
    std::uintptr_t slot{},slotGot{};
    explicit operator bool()const{return read&&write&&ownsThread&&slot&&slotGot;}
};
// sm_CommandBufferIndex also selects direct DrawCommon shading/AO and drawable
// paths when command lists are disabled. Native RenderScene sets 1 before the
// collector and 3 before main color; the latter store is outside DrawColorPass.
// Capture around the WHOLE replay, after native color. Shader-global journals
// do not cover this separate CPU data symbol. No game update is repeated here.
class ReplayRenderPhase {
public:
    enum class Stage {Depth,Lighting,Color};
private:
    const ReplayRenderPhaseApi api_;
    const std::thread::id owner_{std::this_thread::get_id()};
    inline static thread_local ReplayRenderPhase* current_{};
    std::uint32_t before_{};
    bool active_{},captured_{},failed_{},unsafe_{},restoreAttempted_{},restored_{true};
    const char* status_{"NOT_CAPTURED"};
    struct Errno {int value{errno};~Errno(){errno=value;}};
    bool fail(const char* reason,bool unsafe=false)noexcept {
        failed_=true;unsafe_|=unsafe;status_=reason;return false;
    }
    bool identity() {
        std::uintptr_t now{};
        if(owner_!=std::this_thread::get_id()||!api_.ownsThread())return fail("PHASE_OWNER",true);
        if(!api_.read(api_.user,api_.slotGot,&now,sizeof(now))||now!=api_.slot)return fail("PHASE_IDENTITY",true);
        return true;
    }
    bool read(std::uint32_t& value) {
        if(!identity())return false;
        if(!api_.read(api_.user,api_.slot,&value,sizeof(value)))return fail("PHASE_READ",true);
        return identity();
    }
    bool write(std::uint32_t value,const char* failure) {
        // Revalidate immediately before ANY write, including restoration. A
        // changed GOT/owner is terminal; later apparent recovery is not proof
        // that the borrowed slot still denotes the captured native object.
        if(!identity())return false;
        try{if(!api_.write(api_.user,api_.slot,&value,sizeof(value)))return fail(failure);}
        catch(...){return fail(failure);}
        std::uint32_t after{};
        return read(after)&&(after==value||fail(failure));
    }
    void detach()noexcept {active_=false;if(current_==this)current_=nullptr;}
public:
    explicit ReplayRenderPhase(ReplayRenderPhaseApi api):api_(api){}
    ~ReplayRenderPhase(){restore();}
    ReplayRenderPhase(const ReplayRenderPhase&)=delete;
    ReplayRenderPhase& operator=(const ReplayRenderPhase&)=delete;
    bool capture()noexcept {
        Errno preserve;
        if(captured_||active_||failed_)return false;
        if(current_){current_->fail("PHASE_NESTED");return fail("PHASE_NESTED");}
        try{
            if(!api_||api_.slot%alignof(std::uint32_t)||api_.slotGot%alignof(std::uintptr_t)||
               api_.slot>UINTPTR_MAX-sizeof(std::uint32_t)||api_.slotGot>UINTPTR_MAX-sizeof(std::uintptr_t))return fail("PHASE_API");
            if(!read(before_))return false;
            // The production boundary is immediately after main native color.
            // Do not silently borrow a reflection/shadow/partially drawn scene.
            if(before_!=3)return fail("PHASE_NOT_NATIVE_COLOR");
            captured_=active_=true;restored_=false;current_=this;status_="READY";return true;
        }catch(...){return fail("PHASE_CAPTURE_EXCEPTION",true);}
    }
    bool enter(Stage stage)noexcept {
        Errno preserve;
        if(active_&&owner_!=std::this_thread::get_id())return fail("PHASE_OWNER",true);
        if(!active_||failed_||unsafe_||current_!=this)return false;
        std::uint32_t selected{};
        switch(stage){case Stage::Depth:case Stage::Lighting:selected=1;break;
            case Stage::Color:selected=3;break;default:return fail("PHASE_STAGE");}
        try{
            std::uint32_t now{};if(!read(now))return false;
            if(now!=1&&now!=3)return fail("PHASE_UNEXPECTED_VALUE",true);
            if(now!=selected&&!write(selected,"PHASE_WRITE"))return false;
            status_="READY";return true;
        }catch(...){return fail("PHASE_ENTER_EXCEPTION",true);}
    }
    bool restore()noexcept {
        Errno preserve;
        if(!active_)return restored_;
        if(owner_!=std::this_thread::get_id())return fail("PHASE_RESTORE_OWNER",true);
        if(restoreAttempted_)return restored_;
        restoreAttempted_=true;
        try{
            if(unsafe_||current_!=this){fail("PHASE_RESTORE_IDENTITY",true);detach();return false;}
            std::uint32_t now{};
            if(!read(now)){detach();return false;}
            if(now!=1&&now!=3){fail("PHASE_RESTORE_VALUE",true);detach();return false;}
            if(now!=before_&&!write(before_,"PHASE_RESTORE_WRITE")){detach();return false;}
            restored_=true;detach();if(!failed_)status_="RESTORED";return true;
        }catch(...){fail("PHASE_RESTORE_EXCEPTION",true);detach();return false;}
    }
    bool good()const noexcept{return captured_&&!failed_;}
    bool active()const noexcept{return active_;}
    bool restorationGood()const noexcept{return restored_;}
    const char* status()const noexcept{return status_;}
};
}

