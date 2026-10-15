#pragma once
#include "runtime/Presentation.h"
#include "NativeCameraContext.h"
#include "NativeScopeContext.h"
#include <atomic>
#include <cerrno>
#include <cstddef>
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
class NativeUiContext {
    NativeUiApi api_{};runtime::PresentationMailbox& mailbox_;
    std::thread::id owner_{};bool owned_{},inputRearm_{};
    runtime::PresentationContext previous_{runtime::PresentationContext::Unknown};
    std::atomic<const char*> status_{"NOT_CONFIGURED"};
    std::atomic<std::uint64_t> observations_{},changes_{},rejected_{};
public:
    explicit NativeUiContext(runtime::PresentationMailbox& mailbox):mailbox_(mailbox){}
    void configure(NativeUiApi api){api_=api;status_=api?"WAITING_FOR_UI":"NOT_CONFIGURED";}
    const char* status()const{return status_.load();}
    std::uint64_t observations()const{return observations_.load();}
    std::uint64_t changes()const{return changes_.load();}
    std::uint64_t rejected()const{return rejected_.load();}
    bool inputRearmRequired()const noexcept{return inputRearm_;}
    bool update(std::uint64_t now) noexcept {
        const int saved=errno;struct Restore{int value;~Restore(){errno=value;}} restore{saved};
        auto context=runtime::PresentationContext::Unknown;
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
                        else switch(NativeCameraContext(api_.camera).sample()){
                            case NativeCameraMode::Gameplay:
                                switch(NativeScopeContext(api_.scope).sample()){
                                    case NativeScopeMode::Inactive:context=runtime::PresentationContext::World;break;
                                    case NativeScopeMode::Active:case NativeScopeMode::Transition:context=runtime::PresentationContext::Optic;break;
                                    default:rejected="SCOPE_CONTEXT_UNAVAILABLE";break;
                                }break;
                            case NativeCameraMode::Override:context=runtime::PresentationContext::CameraOverride;break;
                            case NativeCameraMode::Transition:context=runtime::PresentationContext::CameraTransition;break;
                            default:rejected="CAMERA_CONTEXT_UNAVAILABLE";break;
                        }
                    }
                }
            }
            if(!rejected){
                ++observations_;status_=context==runtime::PresentationContext::Menu?"NATIVE_MENU":
                    context==runtime::PresentationContext::Cutscene?"NATIVE_CUTSCENE":
                    context==runtime::PresentationContext::CameraOverride?"NATIVE_CAMERA_OVERRIDE":
                    context==runtime::PresentationContext::CameraTransition?"NATIVE_CAMERA_TRANSITION":
                    context==runtime::PresentationContext::Optic?"NATIVE_OPTIC":"NATIVE_WORLD";
            }else{++rejected_;status_=rejected;}
        }catch(...){++rejected_;status_="UI_QUERY_FAILED";}
        const bool changed=mailbox_.publish(context,now);
        inputRearm_=changed&&!runtime::preserveHeldInput(previous_,context);
        if(changed){previous_=context;++changes_;}return changed;
    }
};
}
