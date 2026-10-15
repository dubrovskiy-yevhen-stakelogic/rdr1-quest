#pragma once
#include "engine/NativeOcclusionDrawWitness.h"
#include "NativeCurrentContextSnapshot.h"
#include <array>
#include <cmath>
#include <cstring>

namespace rdrvr::experimental {
// Current DrawCommon-entry target/raster qualification. Separate from material,
// query, native ownership, camera and object bounds: this result alone never
// licenses skipping a call. Reads must be native-owner borrowed copies.
// Pinned BindInputLayout uses pending VkContext+3440/3450 and emitted+3380/3390.
// Comparing BOTH prevents treating a previous draw's full viewport as current.
class NativeCurrentPassRaster {
public:
    using Address=std::uintptr_t;
    using Target=engine::NativeOcclusionDrawWitness::Target;
    struct Api {void* context{};bool(*read)(void*,Address,void*,std::size_t){};
        Address gfxVtable{},vkVtable{},framebufferVtable{};};
    struct Call {Address gfx{},vk{},framebuffer{},command{};};
    enum class Failure {None,Unqualified,Read,Identity,Target,Viewport,Scissor,Changed};
    struct Result {bool valid{};Failure failure{};unsigned reads{},bytes{};};
    static bool capture(const Api& api,const Call& call,const Target& target,Result& out,
                        const rdr95::NativeCurrentContextSnapshot* snapshot=nullptr)noexcept {
        out={};
        auto fail=[&](Failure f){out.failure=f;out.valid=false;return false;};
        if(!api.read||!api.gfxVtable||!api.vkVtable||!api.framebufferVtable||!call.gfx||!call.vk||
           !call.framebuffer||!call.command||!target.verified||!target.width||!target.height||
           target.width>8192||target.height>8192||!target.colorSurfaces[0]||!target.depthSurface||
           call.gfx>UINTPTR_MAX-0x168||call.vk>UINTPTR_MAX-0x3468||call.framebuffer>UINTPTR_MAX-0x100)return fail(Failure::Unqualified);
        auto read=[&](Address p,void* v,std::size_t n){if(!p||n>UINTPTR_MAX-p)return false;
            ++out.reads;out.bytes+=unsigned(n);return api.read(api.context,p,v,n);};
        auto load=[&](Address p,auto& v){return read(p,&v,sizeof v);};
        Address gfxType{},vkType{};std::array<std::byte,0x40> gfx{};
        std::array<std::byte,0x20> pass{};std::array<std::byte,0x100> fb{};
        std::array<std::byte,40> actual{},pending{};
        // Only Admission can seal these copied bytes to this exact active
        // DrawScope and physical Read. Never reuse across native calls or eyes.
        const bool reused=snapshot&&snapshot->matches(api.context,api.gfxVtable,api.vkVtable,
            call.gfx,call.vk,call.framebuffer,call.command);
        if(reused){gfx=snapshot->gfx_;pass=snapshot->pass_;}
        else {
            if(!load(call.gfx,gfxType)||gfxType!=api.gfxVtable||!load(call.vk,vkType)||vkType!=api.vkVtable)return fail(Failure::Identity);
            if(!read(call.gfx+0x128,gfx.data(),gfx.size())||!read(call.vk+0x30a0,pass.data(),pass.size()))return fail(Failure::Read);
        }
        const auto wrapper=get<Address>(pass,0);
        Address command=reused?snapshot->command_:0;
        if(get<Address>(gfx,0)!=call.vk||get<Address>(gfx,0x30)!=call.framebuffer||
           get<Address>(pass,0x10)!=call.framebuffer||get<Address>(pass,0x18)!=call.framebuffer||
           !get<Address>(pass,8)||!wrapper||wrapper>UINTPTR_MAX-0xb0||
           (!reused&&!load(wrapper+0x38,command))||command!=call.command)return fail(Failure::Identity);
        Address framebufferType{};
        if(!load(call.framebuffer,framebufferType)||framebufferType!=api.framebufferVtable)return fail(Failure::Identity);
        if(!load(call.framebuffer,fb))return fail(Failure::Read);
        if(get<Address>(fb,0)!=api.framebufferVtable)return fail(Failure::Changed);
        if(!get<std::uint32_t>(fb,0xc)||get<std::uint32_t>(fb,0xf8)!=target.width||get<std::uint32_t>(fb,0xfc)!=target.height||
           get<Address>(fb,0x18)!=target.depthSurface)return fail(Failure::Target);
        for(unsigned i=0;i<8;++i)if(get<Address>(fb,0x20+i*8)!=target.colorSurfaces[i]||target.colorSurfaces[i]==target.depthSurface)return fail(Failure::Target);
        std::array<std::uint32_t,2> dimensions{};
        if(!load(call.vk+0x32d4,dimensions)||dimensions!=std::array<std::uint32_t,2>{target.width,target.height})return fail(Failure::Target);
        if(!read(call.vk+0x3380,actual.data(),actual.size())||!read(call.vk+0x3440,pending.data(),pending.size()))return fail(Failure::Read);
        // GfxContext SetViewport converts integer LR/TB into a negative-height
        // Vulkan viewport. NativePrimaryRaster already follows this exact ABI.
        const auto rect=get<std::array<std::int32_t,4>>(gfx,8);
        const auto depth=get<std::array<float,2>>(gfx,24);
        const auto clip=get<std::array<std::int32_t,4>>(gfx,32);
        if(rect!=std::array<std::int32_t,4>{0,std::int32_t(target.width),0,std::int32_t(target.height)}||
           depth!=std::array<float,2>{0,1})return fail(Failure::Viewport);
        const std::array<float,6> expected{0,float(target.height),float(target.width),-float(target.height),0,1};
        if(get<std::array<float,6>>(actual,16)!=expected||get<std::array<float,6>>(pending,16)!=expected)return fail(Failure::Viewport);
        if(clip!=rect||get<std::array<std::int32_t,2>>(actual,0)!=std::array<std::int32_t,2>{0,0}||
           get<std::array<std::int32_t,2>>(pending,0)!=std::array<std::int32_t,2>{0,0}||
           get<std::array<std::uint32_t,2>>(actual,8)!=dimensions||get<std::array<std::uint32_t,2>>(pending,8)!=dimensions)return fail(Failure::Scissor);
        std::array<std::byte,0x20> after{};
        if(!read(call.vk+0x30a0,after.data(),after.size())||after!=pass||!load(wrapper+0x38,command)||command!=call.command)return fail(Failure::Changed);
        if(reused&&!snapshot->current())return fail(Failure::Changed);
        out.valid=true;return true;
    }
private:
    template<class T,std::size_t N>static T get(const std::array<std::byte,N>& b,unsigned offset)noexcept {
        T value{};std::memcpy(&value,b.data()+offset,sizeof value);return value;
    }
};
}
