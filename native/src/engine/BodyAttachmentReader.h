#pragma once
#include <cstddef>
#include <cstdint>
#include <cerrno>

namespace rdrvr::engine {
struct BodyAttachmentApi {
    using Lookup=const void*(*)(const void*);
    Lookup hat{},fragment{};
    std::uintptr_t navTable{},gohTable{};
};
// Called on the native PostCameraUpdate owner, alongside the head sample.
// GetHat traverses this actor's inventory; GetFragInst resolves the accessory's
// generation-qualified GOH handle. Only the attached, per-instance node is kept.
inline std::uintptr_t readBodyHat(const BodyAttachmentApi& api,
    bool(*read)(void*,std::uintptr_t,void*,std::size_t),void* context,std::uintptr_t actor) noexcept {
    const int saved=errno;struct Restore{int value;~Restore(){errno=value;}} restore{saved};
    try {
        if(!read||!actor||!api.hat||!api.fragment||!api.navTable||!api.gohTable)return 0;
        const auto* hat=api.hat(reinterpret_cast<const void*>(actor));if(!hat)return 0;
        const auto frag=reinterpret_cast<std::uintptr_t>(api.fragment(hat));
        auto get=[&](std::uintptr_t p,std::size_t offset,auto& value){
            return p&&p%8==0&&p<=UINTPTR_MAX-offset-sizeof(value)&&read(context,p+offset,&value,sizeof(value));};
        std::uintptr_t table{},node{},back{};std::uint32_t flags{};
        if(!get(frag,0,table)||(table!=api.navTable&&table!=api.gohTable)||
           !get(frag,0x110,node)||!get(node,0xc0,back)||back!=frag||
           !get(node,0x2c,flags)||(flags&0x120000u)!=0x120000u)return 0;
        // Native SetAttachedFlag marks bits 17 (attached) and 20 (hat).
        // Dropped hats remain visible; nothing is written to game objects.
        return node;
    }catch(...){return 0;}
}
}
