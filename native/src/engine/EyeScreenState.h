#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace rdrvr::engine {
struct EyeScreenState {
    std::array<std::byte,64> matrix{};
    std::array<std::uint32_t,2> extent{};
    bool operator==(const EyeScreenState&)const=default;
};
// Equivalent to the pinned RegenerateScreen arithmetic, but writes only an
// owned eye clone. Never changes grcDevice dimensions, the stock viewport,
// culling/LOD state or camera/projection matrices. FullDRSRes is NOT equivalent.
inline bool buildEyeScreenState(std::span<const std::byte> viewport,
    std::uint32_t width,std::uint32_t height,EyeScreenState& output) noexcept {
    if(viewport.size()!=0x470||!width||!height||width>8192||height>8192)return false;
    std::array<float,5> window{};
    std::memcpy(window.data(),viewport.data()+0x300,sizeof(window));
    for(float x:window)if(!std::isfinite(x))return false;
    if(std::abs(window[0])>1||std::abs(window[1])>1||window[2]<=0||window[2]>1||
       window[3]<=0||window[3]>1||window[4]<0||window[4]>1)return false;
    const int x=int(window[0]*float(width)),y=int(window[1]*float(height));
    const int w=int(window[2]*float(width)),h=int(window[3]*float(height));
    // Tiny positive subwindows which truncate to zero do not describe a draw.
    if(!w||!h)return false;
    EyeScreenState next{};std::memcpy(next.matrix.data(),viewport.data()+0x280,64);
    // Native code deliberately leaves +0x2a0..0x2ac untouched. Preserve those
    // bytes rather than replacing the whole transform with a guessed matrix.
    const auto put=[&](unsigned offset,float value){std::memcpy(next.matrix.data()+offset,&value,4);};
    put(0,float(w>>1));std::memset(next.matrix.data()+4,0,16);
    put(0x14,float((-h)>>1));std::memset(next.matrix.data()+0x18,0,8);
    put(0x30,float(x+(w>>1)));put(0x34,float(y+(h>>1)));put(0x38,window[4]);put(0x3c,1.f);
    next.extent={width,height};output=next;return true;
}
inline void applyEyeScreenState(std::span<std::byte,0x470> ownedClone,const EyeScreenState& state) noexcept {
    std::memcpy(ownedClone.data()+0x280,state.matrix.data(),64);
    std::memcpy(ownedClone.data()+0x330,state.extent.data(),8);
}
inline bool matchesEyeScreenState(std::span<const std::byte,0x470> clone,const EyeScreenState& state) noexcept {
    return !std::memcmp(clone.data()+0x280,state.matrix.data(),64)&&!std::memcmp(clone.data()+0x330,state.extent.data(),8);
}
}
