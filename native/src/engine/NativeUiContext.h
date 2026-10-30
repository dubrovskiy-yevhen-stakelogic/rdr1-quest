#pragma once
#include "runtime/Presentation.h"
#include "NativeCameraContext.h"
#include "NativeScopeContext.h"
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
namespace rdrvr::engine {
struct NativeUiApi {
    bool (*read)(void*,std::uintptr_t,void*,std::size_t){};
    bool (*menuActive)(){};
    std::uintptr_t singleton{},vtable{};
    bool (*cutsceneActive)(){};
    std::uintptr_t cutsceneSingleton{},cutsceneTable{};
    NativeCameraApi camera{};NativeScopeApi scope{};
    std::uintptr_t singletonTable{},cutsceneSingletonTable{};
    explicit operator bool()const{return read&&menuActive&&singleton&&vtable&&cutsceneActive&&cutsceneSingleton&&cutsceneTable;}
};
// One line per presentation transition / status change / longer head loss in WORLD (report vr.presentationHistory).
struct PresentationTransition {
    std::uint64_t ns{};runtime::PresentationContext from{},to{};std::uintptr_t top{};
    const char* status{},*veto{},*cameraClass{};char camera[16]{};
};
class NativeUiContext {
    NativeUiApi api_{};runtime::PresentationMailbox& mailbox_;
    std::thread::id owner_{};bool owned_{},inputRearm_{};
    runtime::PresentationContext previous_{runtime::PresentationContext::Unknown};
    std::atomic<const char*> status_{"NOT_CONFIGURED"};
    std::atomic<std::uint64_t> observations_{},changes_{},rejected_{};
    // Scripted gameplay shot (NativeCameraMode::Shot, published as CameraOverride): when it began (0 = none now), the last
    // Override top camera vtable, Override samples per class.
    std::atomic<std::uint64_t> shotSince_{},shotSamples_{},plainOverrideSamples_{};std::atomic<std::uintptr_t> overrideTable_{};
    // The ring: writers = the owner thread, the reader = the report. Strings are literals; the mutex is taken only on a transition.
    mutable std::mutex historyMutex_;std::array<PresentationTransition,16> history_{};std::uint64_t historyTotal_{};
    std::atomic<const char*> stereoVeto_{nullptr};std::atomic<std::uint64_t> stereoVetoNs_{};
    std::uint64_t headBadSince_{};bool headLost_{};
    static constexpr std::uint64_t stereoVetoFreshNs=1'000'000'000,headLossNs=300'000'000;
    const char* freshStereoVeto(std::uint64_t now)const noexcept{
        const auto t=stereoVetoNs_.load(std::memory_order_relaxed);
        return t&&now>=t&&now-t<=stereoVetoFreshNs?stereoVeto_.load(std::memory_order_relaxed):nullptr;
    }
    const char* cameraClass(NativeCameraMode mode,std::uintptr_t top)const noexcept{
        if(!top)return "";
        if(mode==NativeCameraMode::Gameplay)return "GAME";
        if(api_.camera.shotTable&&top==api_.camera.shotTable)return "camCutsceneCamera";
        if(api_.camera.plainTable&&top==api_.camera.plainTable)return "camCamera";
        return "";
    }
    void remember(std::uint64_t now,runtime::PresentationContext from,runtime::PresentationContext to,const char* status,const char* veto,
                  std::uintptr_t top,const char* cameraClassName,const char* camera)noexcept{
        try{
            std::lock_guard guard(historyMutex_);
            auto& e=history_[historyTotal_++%history_.size()];e=PresentationTransition{};
            e.ns=now;e.from=from;e.to=to;e.status=status;e.veto=veto;e.cameraClass=cameraClassName;
            e.top=top&&api_.camera.base&&top>=api_.camera.base?top-api_.camera.base:top;
            if(camera)for(std::size_t n=0;n+1<sizeof e.camera&&camera[n];++n)e.camera[n]=camera[n];
        }catch(...){}
    }
    static const char* contextName(runtime::PresentationContext c)noexcept{
        using P=runtime::PresentationContext;
        return c==P::World?"WORLD":c==P::Menu?"MENU":c==P::Cutscene?"CUTSCENE":c==P::CameraOverride?"CAMERA_OVERRIDE":
            c==P::CameraTransition?"CAMERA_TRANSITION":c==P::Optic?"OPTIC":"UNKNOWN";
    }
public:
    explicit NativeUiContext(runtime::PresentationMailbox& mailbox):mailbox_(mailbox){}
    void configure(NativeUiApi api){api_=api;status_=api?"WAITING_FOR_UI":"NOT_CONFIGURED";}
    const char* status()const{return status_.load();}
    std::uint64_t observations()const{return observations_.load();}
    std::uint64_t changes()const{return changes_.load();}
    std::uint64_t rejected()const{return rejected_.load();}
    bool inputRearmRequired()const noexcept{return inputRearm_;}
    // SNIPER SCOPE: PARAM_nonewsniperscope (the print-checked scope api), 0 when the scope api is not configured.
    std::uintptr_t scopeDisableParam()const noexcept{return api_.scope?api_.scope.disableParam:0;}
    // SniperScopeManager::Init ran (+0x8 = the effect Init stores at 0x2a1c3f4; Init skips itself while the parameter is set).
    bool scopeManagerInitialised()const noexcept{
        std::uintptr_t manager{},effect{};
        return api_.scope&&api_.scope.read(api_.scope.context,api_.scope.singleton,&manager,sizeof manager)&&manager&&
            api_.scope.read(api_.scope.context,manager+0x8,&effect,sizeof effect)&&effect;
    }
    std::uint64_t shotSince()const noexcept{return shotSince_.load(std::memory_order_relaxed);}
    std::uint64_t shotSamples()const noexcept{return shotSamples_.load(std::memory_order_relaxed);}
    std::uint64_t plainOverrideSamples()const noexcept{return plainOverrideSamples_.load(std::memory_order_relaxed);}
    std::uintptr_t overrideTable()const noexcept{return overrideTable_.load(std::memory_order_relaxed);}
    // Any thread: the world pass refused a frame (literal reason); a transition within a second of it logs it as its veto.
    void noteStereoVeto(const char* why,std::uint64_t now)noexcept{stereoVeto_.store(why,std::memory_order_relaxed);stereoVetoNs_.store(now,std::memory_order_relaxed);}
    // Owner thread, per head sample: a head anchor missing for headLossNs in WORLD keeps the context but shows no stereo, so it gets its own lines.
    void noteHead(std::uint64_t now,bool world,bool published)noexcept{
        using P=runtime::PresentationContext;
        if(!world||published){
            if(headLost_){headLost_=false;remember(now,P::World,world?P::World:previous_,"LOCAL_HEAD_RESTORED",freshStereoVeto(now),0,"",nullptr);}
            headBadSince_=0;return;}
        if(!headBadSince_)headBadSince_=now?now:1;
        if(!headLost_&&now>=headBadSince_&&now-headBadSince_>=headLossNs){headLost_=true;remember(now,P::World,P::World,"LOCAL_HEAD_UNAVAILABLE",freshStereoVeto(now),0,"",nullptr);}
    }
    std::string history(std::uint64_t now)const{
        std::lock_guard guard(historyMutex_);
        std::string out="{\"total\":"+std::to_string(historyTotal_)+",\"entries\":[";
        const std::uint64_t first=historyTotal_>history_.size()?historyTotal_-history_.size():0;
        for(auto k=first;k<historyTotal_;++k){
            const auto& e=history_[k%history_.size()];char b[320];
            std::snprintf(b,sizeof b,"%s{\"ns\":%llu,\"ageMs\":%lld,\"from\":\"%s\",\"to\":\"%s\",\"status\":\"%s\",\"top\":\"0x%llx\",\"class\":\"%s\",\"camera\":\"",k==first?"":",",
                (unsigned long long)e.ns,now>=e.ns?(long long)((now-e.ns)/1000000u):-1,contextName(e.from),contextName(e.to),e.status?e.status:"",(unsigned long long)e.top,e.cameraClass?e.cameraClass:"");
            out+=b;
            for(const char* c=e.camera;*c;++c){const auto v=static_cast<unsigned char>(*c);out+=v<32||v>126||*c=='"'||*c==92?'_':*c;}
            out+="\",\"veto\":\"";out+=e.veto?e.veto:"";out+="\"}";
        }
        return out+"]}";
    }
    bool update(std::uint64_t now) noexcept {
        const int saved=errno;struct Restore{int value;~Restore(){errno=value;}} restore{saved};
        auto context=runtime::PresentationContext::Unknown;bool shot=false;
        auto cameraMode=NativeCameraMode::Unknown;std::uintptr_t cameraTop{};char cameraWrapper[16]{};const char* veto=nullptr;
        const char* const statusBefore=status_.load();
        try{
            if(!owned_){owner_=std::this_thread::get_id();owned_=true;}
            if(owner_!=std::this_thread::get_id()){++rejected_;return false;}
            std::uintptr_t object{},table{};unsigned char ready{};
            const char* rejected=nullptr;
            if(!api_)rejected="NOT_CONFIGURED";
            else if(!api_.read(nullptr,api_.singleton,&object,sizeof(object))||!object)rejected="UI_SINGLETON_UNAVAILABLE";
            else if(!api_.read(nullptr,object,&table,sizeof(table))||(table!=api_.vtable&&(!api_.singletonTable||table!=api_.singletonTable)))rejected="UI_VTABLE_MISMATCH";
            else if(!api_.read(nullptr,object+0x174,&ready,sizeof(ready))||ready!=1)rejected="UI_NOT_READY";
            if(!rejected){
                if(api_.menuActive())context=runtime::PresentationContext::Menu;
                else{
                    std::uintptr_t manager{},managerTable{},again{};
                    if(!api_.read(nullptr,api_.cutsceneSingleton,&manager,sizeof(manager))||!manager||
                       !api_.read(nullptr,manager,&managerTable,sizeof(managerTable))||(managerTable!=api_.cutsceneTable&&(!api_.cutsceneSingletonTable||managerTable!=api_.cutsceneSingletonTable)))
                        rejected="CUTSCENE_MANAGER_UNAVAILABLE";
                    else{
                        const bool playing=api_.cutsceneActive();
                        if(!api_.read(nullptr,api_.cutsceneSingleton,&again,sizeof(again))||again!=manager||
                           !api_.read(nullptr,manager,&managerTable,sizeof(managerTable))||(managerTable!=api_.cutsceneTable&&(!api_.cutsceneSingletonTable||managerTable!=api_.cutsceneSingletonTable)))
                            rejected="CUTSCENE_MANAGER_CHANGED";
                        else if(playing)context=runtime::PresentationContext::Cutscene;
                        else{std::uintptr_t& top=cameraTop;const auto mode=cameraMode=NativeCameraContext(api_.camera).sample(&top,&cameraWrapper);
                        if(mode==NativeCameraMode::Override||mode==NativeCameraMode::Shot){overrideTable_.store(top,std::memory_order_relaxed);
                            (mode==NativeCameraMode::Shot?shotSamples_:plainOverrideSamples_).fetch_add(1,std::memory_order_relaxed);}
                        switch(mode){
                            case NativeCameraMode::Gameplay:
                                switch(NativeScopeContext(api_.scope).sample()){
                                    case NativeScopeMode::Inactive:context=runtime::PresentationContext::World;break;
                                    case NativeScopeMode::Active:case NativeScopeMode::Transition:context=runtime::PresentationContext::Optic;break;
                                    default:rejected="SCOPE_CONTEXT_UNAVAILABLE";break;
                                }break;
                            case NativeCameraMode::Override:context=runtime::PresentationContext::CameraOverride;break;
                            case NativeCameraMode::Shot:context=runtime::PresentationContext::CameraOverride;shot=true;break;
                            case NativeCameraMode::Transition:context=runtime::PresentationContext::CameraTransition;break;
                            default:rejected="CAMERA_CONTEXT_UNAVAILABLE";break;
                        }}
                    }
                }
            }
            if(!rejected){
                ++observations_;status_=context==runtime::PresentationContext::Menu?"NATIVE_MENU":
                    context==runtime::PresentationContext::Cutscene?"NATIVE_CUTSCENE":
                    context==runtime::PresentationContext::CameraOverride?"NATIVE_CAMERA_OVERRIDE":
                    context==runtime::PresentationContext::CameraTransition?"NATIVE_CAMERA_TRANSITION":
                    context==runtime::PresentationContext::Optic?"NATIVE_OPTIC":"NATIVE_WORLD";
            }else{++rejected_;status_=rejected;veto=rejected;}
        }catch(...){++rejected_;status_="UI_QUERY_FAILED";veto="UI_QUERY_FAILED";}
        if(!shot)shotSince_.store(0,std::memory_order_relaxed);else if(!shotSince_.load(std::memory_order_relaxed))shotSince_.store(now?now:1,std::memory_order_relaxed);
        const bool changed=mailbox_.publish(context,now);
        const char* const statusAfter=status_.load();
        if(changed||statusAfter!=statusBefore)remember(now,changed?previous_:context,context,statusAfter,veto?veto:freshStereoVeto(now),cameraTop,cameraClass(cameraMode,cameraTop),cameraWrapper);
        inputRearm_=changed&&!runtime::preserveHeldInput(previous_,context);
        if(changed){previous_=context;++changes_;}return changed;
    }
};
}
