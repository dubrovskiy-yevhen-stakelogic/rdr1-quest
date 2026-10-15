#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rdr95 {
// A bounded read of the ACTUAL pending native shader state at ordinary+0x30.
// It proves only that the shown EndPass/EndDraw clears are already satisfied.
// It is NOT an object/material certificate, read lease, or skip permission.
class OrdinaryCanonicalState final {
public:
    using Address=std::uintptr_t;
    static_assert(sizeof(Address)==8,"Pinned native layout requires a 64-bit target");
    struct Boundary {
        Address gfx{},vk{};
        std::uint64_t revision{}; // caller's non-reusable current borrow epoch
        bool operator==(const Boundary&)const=default;
    };
    struct Api {
        void* context{};
        bool(*read)(void*,Address,void*,std::size_t)noexcept{};
        // Actual TLS owner/current native borrow, never a previously cached
        // context. Called before the first read and after the last read.
        bool(*boundary)(void*,Boundary&)noexcept{};
        Address image{},gfxVtable{},vkVtable{};
    };
    static constexpr Address globalsFlushed=0x4e6d9161;
    static constexpr Address vertexProgram=0x4e6d9168;
    static constexpr Address technique=0x4e6e06d0;
    static bool clear(const Api& api)noexcept {
        if(!api.read||!api.boundary||!api.image||api.image>UINTPTR_MAX-technique-40||
           !api.gfxVtable||!api.vkVtable)return false;
        Boundary first{};
        if(!api.boundary(api.context,first)||!first.revision||!first.gfx||!first.vk||
           first.gfx>UINTPTR_MAX-0x184||first.vk>UINTPTR_MAX-0x32c4)return false;
        auto read=[&](Address address,void* out,std::size_t count)noexcept{return api.read(api.context,address,out,count);};
        Address gfxType{},vkType{},linked{};
        if(!read(first.gfx,&gfxType,sizeof gfxType)||gfxType!=api.gfxVtable||
           !read(first.gfx+0x128,&linked,sizeof linked)||linked!=first.vk||
           !read(first.vk,&vkType,sizeof vkType)||vkType!=api.vkVtable)return false;
        std::uint8_t flushed{};std::array<Address,2> programs{};
        // TechniqueHandle/PassIndex occupy bytes8..15 and deliberately remain
        // unconstrained: neither EndPass nor EndDraw clears them.
        std::array<std::byte,40> active{};std::array<std::byte,12> pending{};
        std::array<std::uint32_t,2> indices{};
        if(!read(api.image+globalsFlushed,&flushed,sizeof flushed)||flushed||
           !read(api.image+vertexProgram,programs.data(),sizeof programs)||programs[0]||programs[1]||
           !read(api.image+technique,active.data(),active.size())||
           !read(first.gfx+0x178,pending.data(),pending.size())||
           !read(first.vk+0x32bc,indices.data(),sizeof indices))return false;
        for(unsigned offset:{0u,16u,24u,32u}){Address value{};std::memcpy(&value,active.data()+offset,sizeof value);if(value)return false;}
        Address program{};std::uint32_t id{};
        std::memcpy(&program,pending.data(),sizeof program);std::memcpy(&id,pending.data()+8,sizeof id);
        if(program||id||indices[0]!=UINT32_MAX||indices[1]!=UINT32_MAX)return false;
        // Recheck context links and types, then the caller's immutable epoch.
        // The epoch must reject mutations even if pointers changed and changed
        // back; these reads alone cannot solve a native allocation ABA.
        Address finalGfx{},finalVk{},finalLink{};Boundary last{};
        return read(first.gfx,&finalGfx,sizeof finalGfx)&&finalGfx==gfxType&&
            read(first.gfx+0x128,&finalLink,sizeof finalLink)&&finalLink==linked&&
            read(first.vk,&finalVk,sizeof finalVk)&&finalVk==vkType&&
            api.boundary(api.context,last)&&last==first;
    }
};
}
