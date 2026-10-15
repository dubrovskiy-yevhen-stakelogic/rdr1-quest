#pragma once
#include "NativeEarlyTargets.h"

namespace rdrvr::experimental {
// The actual Game adapter and the host fixture share this read path. This is
// decoding only: the caller still owns the current stage, short resource Read,
// frozen pose and retained RIGHT targets. No cached native state or new permit.
class NativeEarlyPassTargets {
public:
    using Address=std::uintptr_t;
    using Api=NativeEarlyTargets::Api;
    using Target=NativeEarlyTargets::Target;
    using Ticket=NativeEarlyImageRegistry::Ticket;
    struct Context {Address gfx{},vk{};};
    struct Selection {
        bool right{},depth{};
        Address collectorSlot{},postfxSlot{};
        Address color{},z{};
        const engine::TargetImage* expectedColor{};
        const engine::TargetImage* expectedDepth{};
    };
    // Kept separate so Game's RIGHT EngineEyeTargets ready/serial/eye-count
    // checks stay at exactly their previous position, after these two reads.
    static bool context(const engine::PassSourceAbi& memory,Address tlsSlot,
                        Address expectedGfx,Context& out) noexcept {
        out={};Address gfx{},vk{};
        if(!memory.read||!tlsSlot||!expectedGfx||
           !memory.read(memory.context,tlsSlot,&gfx,sizeof gfx)||gfx!=expectedGfx||
           gfx>UINTPTR_MAX-0x130||!memory.read(memory.context,gfx+0x128,&vk,sizeof vk)||!vk)return false;
        out={gfx,vk};return true;
    }
    static bool targets(const Api& api,const Selection& selection,Target& out,Ticket& ticket) noexcept {
        out={};ticket={};
        if(selection.right)
            return NativeEarlyTargets::capture(api,selection.color,selection.z,out,
                selection.expectedColor,selection.expectedDepth,&ticket);
        if(!api.memory.read)return false;
        Address collector{},postfx{},color{},z{};
        auto read=[&](Address address,Address& value){return api.memory.read(api.memory.context,address,&value,sizeof value);};
        if(!read(selection.collectorSlot,collector)||!read(selection.postfxSlot,postfx)||
           !collector||!postfx||collector>UINTPTR_MAX-0x50||postfx>UINTPTR_MAX-0x18||
           !read(collector+0x48,z)||!read(selection.depth?collector+0x40:postfx+0x10,color))return false;
        return NativeEarlyTargets::capture(api,color,z,out,nullptr,nullptr,&ticket);
    }
};
}
