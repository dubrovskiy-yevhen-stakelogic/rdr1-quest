#pragma once
#include "HudBlendReader.h"
#include "HudDrawRouting.h"
#include "HudResources.h"
namespace rdrvr::engine {
struct HudRenderEntry {
    std::uintptr_t blendBlock{};
    void (*set)(unsigned,int){};void (*flush)(){};HudRoutingApi::Bind bind{};
    void (*factoryFlush)(){};
    explicit operator bool()const{return blendBlock&&set&&flush&&bind;}
};
class HudNativeRouting {
    HudResourceApi api_;std::uintptr_t deviceGlobal_{},deviceVtable_{};
    HudRenderEntry entry_;HudBlendReader blend_;
    template<class T> bool read(std::uintptr_t base,std::size_t offset,T& out) const {
        return base&&base%8==0&&offset<=UINTPTR_MAX-base&&sizeof(T)<=UINTPTR_MAX-(base+offset)&&
            api_.memory.read(api_.memory.context,base+offset,&out,sizeof(out));
    }
    bool table(std::uintptr_t address,std::uintptr_t expected) const {
        std::uintptr_t actual{};return expected&&read(address,0,actual)&&actual==expected;
    }
    bool owner(void* gfx) const {
        void* actual{};
        return api_.memory.read&&api_.ownsThread&&api_.currentGfx&&api_.ownsThread()&&
            api_.currentGfx(&actual)&&actual==gfx&&table(reinterpret_cast<std::uintptr_t>(gfx),api_.gfxVtable);
    }
    static bool current(void* self,void* gfx,void** out){
        auto& s=*static_cast<HudNativeRouting*>(self);std::uintptr_t fb{};
        if(!out||!s.owner(gfx)||!s.read(reinterpret_cast<std::uintptr_t>(gfx),0x158,fb)||
           (fb&&!s.table(fb,s.api_.framebufferVtable)))return false;
        *out=reinterpret_cast<void*>(fb);return true;
    }
    static bool resolveNull(void* self,void* gfx,void** out){
        auto& s=*static_cast<HudNativeRouting*>(self);std::uintptr_t context{},device{},window{},fb{};std::uint8_t noDefault{};
        if(!out||!s.owner(gfx)||!s.read(reinterpret_cast<std::uintptr_t>(gfx),0x128,context)||
           !s.table(context,s.api_.memory.contextVtable)||!s.read(context,0x74,noDefault)||noDefault>1)return false;
        // GfxContext::BindFrameBuffer(nullptr) consults VkContext+0x74, then
        // GfxDevice::ms_Instance -> +0x58 window -> +0x60 framebuffer. A null
        // argument is not necessarily an unbind and must not always map to HUD.
        if(!noDefault){
            if(!s.read(s.deviceGlobal_,0,device)||!s.table(device,s.deviceVtable_)||
               !s.read(device,0x58,window)||!s.read(window,0x60,fb)||
               (fb&&!s.table(fb,s.api_.framebufferVtable)))return false;
        }
        *out=reinterpret_cast<void*>(fb);return true;
    }
public:
    HudNativeRouting(HudResourceApi api,std::uintptr_t deviceGlobal,std::uintptr_t deviceVtable,HudRenderEntry entry):
        api_(api),deviceGlobal_(deviceGlobal),deviceVtable_(deviceVtable),entry_(entry),
        blend_{api.memory.read,api.memory.context,entry.blendBlock,api.ownsThread}{}
    HudNativeRouting(const HudNativeRouting&)=delete;
    HudNativeRouting& operator=(const HudNativeRouting&)=delete;
    HudRoutingApi api(){return {{HudBlendReader::read,&blend_,entry_.set,entry_.flush},entry_.bind,current,resolveNull,this};}
};
}
