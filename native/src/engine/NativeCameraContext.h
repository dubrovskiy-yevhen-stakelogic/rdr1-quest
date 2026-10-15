#pragma once
#include <cerrno>
#include <cstddef>
#include <cstdint>
namespace rdrvr::engine {
enum class NativeCameraMode { Unknown, Gameplay, Override, Transition };
struct NativeCameraApi {
    bool (*read)(void*,std::uintptr_t,void*,std::size_t){};void* context{};
    std::uintptr_t singleton{},managerTable{},channelTable{},singletonTable{};
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
        std::uintptr_t manager{},channel{},game{},topNode{},topCamera{};std::uint32_t transition{};
        bool operator==(const Identity&)const=default;
    };
    bool identity(Identity& out)const{
        Identity i{};std::uintptr_t table{};
        if(!get(api_.singleton,0,i.manager)||!get(i.manager,0,table)||!api_.matchesManager(table)||
           !get(i.manager,8,i.channel)||!get(i.channel,0,table)||table!=api_.channelTable||
           !get(i.manager,0x28,i.game)||!i.game||!get(i.channel,0x18,i.topNode)||
           !get(i.topNode,0x10,i.topCamera)||!i.topCamera||!get(i.topNode,8,i.transition)||i.transition>2)return false;
        out=i;return true;
    }
public:
    explicit NativeCameraContext(NativeCameraApi api):api_(api){}
    NativeCameraMode sample()const noexcept{
        const int saved=errno;struct Restore{int saved;~Restore(){errno=saved;}} restore{saved};
        try{
            Identity before{},after{};
            if(!api_||!identity(before)||!identity(after)||before!=after)return NativeCameraMode::Unknown;
            // Native TransitionCamera::Update resets 1/2 to 0 on completion;
            // camCameraChannel::Update then removes a finished outgoing node.
            if(before.transition)return NativeCameraMode::Transition;
            return before.topCamera==before.game?NativeCameraMode::Gameplay:NativeCameraMode::Override;
        }catch(...){return NativeCameraMode::Unknown;}
    }
};
}
