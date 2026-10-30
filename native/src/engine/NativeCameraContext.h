#pragma once
#include <cerrno>
#include <cstddef>
#include <cstdint>
namespace rdrvr::engine {
// Shot = an Override whose top camera is a camCutsceneCamera (gohCameraShot sequences: CutsceneCameraTuning::PlayCutScene /
// rdr2GOH::AddCameraTransitionShot* - the scripted gameplay shots of looting, hitching, hogtying, finishers, skinning).
enum class NativeCameraMode { Unknown, Gameplay, Override, Transition, Shot };
struct NativeCameraApi {
    bool (*read)(void*,std::uintptr_t,void*,std::size_t){};void* context{};
    std::uintptr_t singleton{},managerTable{},channelTable{},singletonTable{};
    std::uintptr_t shotTable{}; // camCutsceneCamera vtable (_ZTV17camCutsceneCamera+0x10); 0 = every top camera is a plain Override
    std::uintptr_t plainTable{},base{}; // Diagnostics only: camCamera vtable (script cameras, _ZTV9camCamera+0x10), engine base
    bool matchesManager(std::uintptr_t table)const{return table==managerTable||(singletonTable&&table==singletonTable);}
    explicit operator bool()const{return read&&singleton&&managerTable&&channelTable;}
};
// Read the primary channel that InitCameraChannels seeds with manager+0x28.
// No camera virtual calls, simulation updates, stack changes or render-channel
// writes occur here. The latter is a render-thread selector, not this channel.
class NativeCameraContext {
    NativeCameraApi api_;
    template<class T>bool get(std::uintptr_t p,std::size_t offset,T& out)const{
        return p&&p<=UINTPTR_MAX-offset&&p+offset<=UINTPTR_MAX-sizeof(T)&&api_.read(api_.context,p+offset,&out,sizeof(T));
    }
    struct Identity {
        std::uintptr_t manager{},channel{},game{},topNode{},topCamera{},topTable{};std::uint32_t transition{};
        bool operator==(const Identity&)const=default;
    };
    bool identity(Identity& out)const{
        Identity i{};std::uintptr_t table{};
        if(!get(api_.singleton,0,i.manager)||!get(i.manager,0,table)||!api_.matchesManager(table)||
           !get(i.manager,8,i.channel)||!get(i.channel,0,table)||table!=api_.channelTable||
           !get(i.manager,0x28,i.game)||!i.game||!get(i.channel,0x18,i.topNode)||
           !get(i.topNode,0x10,i.topCamera)||!i.topCamera||!get(i.topCamera,0,i.topTable)||!get(i.topNode,8,i.transition)||i.transition>2)return false;
        out=i;return true;
    }
public:
    explicit NativeCameraContext(NativeCameraApi api):api_(api){}
    // topTable (optional): the top camera's vtable of a consistent sample (diagnostics: which camera class an Override is).
    // name (optional): a top camera that is not the game camera: its script wrapper's name (camCamera+0x5d0 -> gohNamedBase +0x34, the
    // chain of rage::IdleCameraContext::IsContextActive), empty when it has none.
    NativeCameraMode sample(std::uintptr_t* topTable=nullptr,char (*name)[16]=nullptr)const noexcept{
        const int saved=errno;struct Restore{int saved;~Restore(){errno=saved;}} restore{saved};
        try{
            Identity before{},after{};
            if(!api_||!identity(before)||!identity(after)||before!=after)return NativeCameraMode::Unknown;
            // Native TransitionCamera::Update resets 1/2 to 0 on completion;
            // camCameraChannel::Update then removes a finished outgoing node.
            if(topTable)*topTable=before.topTable;
            if(name&&before.topCamera!=before.game){std::uintptr_t wrapper{};
                if(!get(before.topCamera,0x5d0,wrapper)||!wrapper||!get(wrapper,0x34,*name))(*name)[0]=0;
                (*name)[15]=0;}
            if(before.transition)return NativeCameraMode::Transition;
            if(before.topCamera==before.game)return NativeCameraMode::Gameplay;
            return api_.shotTable&&before.topTable==api_.shotTable?NativeCameraMode::Shot:NativeCameraMode::Override;
        }catch(...){return NativeCameraMode::Unknown;}
    }
};
}
