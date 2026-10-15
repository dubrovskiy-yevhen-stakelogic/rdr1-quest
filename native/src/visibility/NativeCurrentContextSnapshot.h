#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rdrvr::experimental { class NativeCurrentPassRaster; }
namespace rdr95 {
class NativeCurrentFixedState;
class NativeCurrentAdmission;

// Copied bytes from one successful fresh FixedState check. This is
// not a renderer-state cache or a new native ownership permission. Admission
// may seal it only to the exact active DrawScope AND real physical Read. End,
// nesting, rebegin or a different call must revoke that scope before native
// code runs. The issuer and its Read outlive any use of the lexical receipt.
class NativeCurrentContextSnapshot {
    friend class NativeCurrentFixedState;
    friend class NativeCurrentAdmission;
    friend class rdrvr::experimental::NativeCurrentPassRaster;
    using Address=std::uintptr_t;
    using Current=bool(*)(const void*,std::uint64_t)noexcept;
    struct CallKey {
        Address gfx{},vk{},container{},record{};
        unsigned command{},bucket{};std::uint32_t fade{};unsigned flags{},extra{};
        bool operator==(const CallKey&)const=default;
    };
    template<class Call>static CallKey key(const Call& c)noexcept {
        return {c.gfx,c.vk,c.container,c.record,c.command,c.bucket,
                std::bit_cast<std::uint32_t>(c.fade),c.flags,c.extra};
    }
    CallKey call_{};
    void* memory_{};
    Address gfxType_{},vkType_{},wrapper_{},framebuffer_{},command_{};
    std::array<std::byte,0x40> gfx_{};
    std::array<std::byte,0x20> pass_{};
    const void* owner_{};
    std::uint64_t serial_{};
    Current current_{};
    bool captured_{};

    template<class Call,std::size_t G,std::size_t V>
    void capture(const Call& call,void* memory,Address gfxType,Address vkType,
                 const std::array<std::byte,G>& gfx,const std::array<std::byte,V>& vk,
                 Address wrapper,Address framebuffer,Address command)noexcept {
        static_assert(G>=0x40&&V>=0x40);
        *this={};
        call_=key(call);memory_=memory;gfxType_=gfxType;vkType_=vkType;
        wrapper_=wrapper;framebuffer_=framebuffer;command_=command;
        std::memcpy(gfx_.data(),gfx.data(),gfx_.size());
        // FixedState's window starts at VkContext+3080; Raster needs +30a0.
        std::memcpy(pass_.data(),vk.data()+0x20,pass_.size());
        captured_=call.gfx&&call.vk&&wrapper&&framebuffer&&command&&gfxType&&vkType;
    }
    template<class Call>
    bool seal(const void* owner,std::uint64_t serial,Current check,const Call& call)noexcept {
        owner_=nullptr;serial_=0;current_=nullptr;
        if(!captured_||call_!=key(call)||!owner||!serial||!check||!check(owner,serial))return false;
        owner_=owner;serial_=serial;current_=check;return true;
    }
    bool matches(void* memory,Address gfxType,Address vkType,Address gfx,Address vk,
                 Address framebuffer,Address command)const noexcept {
        return captured_&&memory_==memory&&gfxType_==gfxType&&vkType_==vkType&&
            call_.gfx==gfx&&call_.vk==vk&&framebuffer_==framebuffer&&command_==command&&current();
    }
public:
    NativeCurrentContextSnapshot()noexcept=default;
    // Truthiness is current scope authority, not merely the presence of bytes.
    explicit operator bool()const noexcept{return current();}
    bool current()const noexcept{return captured_&&owner_&&serial_&&current_&&current_(owner_,serial_);}
};
}
