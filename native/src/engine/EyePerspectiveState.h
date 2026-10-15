#pragma once
#include "runtime/Math.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <span>

namespace rdrvr::engine {
struct EyePerspectiveState {
    // Native FOV/aspect, near/far, cached vertical/horizontal tangents,
    // axis signs and projection offsets at +338..35c.
    std::array<float,10> fields{};
};
// Invert the pinned RegeneratePerspective metadata arithmetic on an owned
// render clone. Lighting also reads these tangents, so they must describe the
// actual off-axis projection, never a wider CPU culling envelope. Independent
// shaft construction must use the existing conservative union viewport.
inline bool buildEyePerspectiveState(const runtime::Mat4& p,float nearClip,float farClip,
    EyePerspectiveState& output) noexcept {
    if(!std::isfinite(nearClip)||!std::isfinite(farClip)||nearClip<=0||farClip<=nearClip)return false;
    for(float x:p)if(!std::isfinite(x))return false;
    for(unsigned i:{1,2,3,4,6,7,12,13,15})if(p[i]!=0)return false;
    if(p[0]<=0||std::abs(p[5])<0.0001f||p[11]!=-1||std::abs(p[8])>=1||std::abs(p[9])>=1)return false;
    const auto agrees=[](float actual,float expected){return std::isfinite(expected)&&std::abs(actual-expected)<=
        4*std::numeric_limits<float>::epsilon()*std::max(std::abs(expected),std::numeric_limits<float>::min());};
    // These clip distances travel with CameraAdapter's projection. Do not
    // reconstruct far from p10+1: that subtraction can round to zero.
    if(!agrees(p[10],-farClip/(farClip-nearClip))||!agrees(p[14],-farClip*nearClip/(farClip-nearClip)))return false;
    const float h=1/p[0],v=1/std::abs(p[5]);
    const float fov=float(2*std::atan(double(v))*180/3.14159265358979323846),aspect=h/v;
    if(!std::isfinite(h)||!std::isfinite(v)||!std::isfinite(aspect)||h<=0||v<=0||aspect<=0||fov<=0||fov>=180)return false;
    EyePerspectiveState next{{fov,aspect,nearClip,farClip,v,h,1,p[5]<0?-1.f:1.f,p[8],p[9]}};
    output=next;return true;
}
inline void applyEyePerspectiveState(std::span<std::byte,0x470> clone,const EyePerspectiveState& state) noexcept {
    std::memcpy(clone.data()+0x338,state.fields.data(),sizeof(state.fields));
}
inline bool matchesEyePerspectiveState(std::span<const std::byte,0x470> clone,const EyePerspectiveState& state) noexcept {
    return !std::memcmp(clone.data()+0x338,state.fields.data(),sizeof(state.fields));
}
}
