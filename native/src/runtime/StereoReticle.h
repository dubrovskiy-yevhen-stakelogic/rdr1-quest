#pragma once
#include "contracts/Contracts.h"
#include <algorithm>
#include <cmath>
namespace rdrvr::runtime {
struct ScreenReticle {float u{},v{},radius{},opacity{1};}; // UV and radius / smaller eye dimension; zero disables.
inline bool validReticle(const ScreenReticle& r){
    return std::isfinite(r.u)&&std::isfinite(r.v)&&std::isfinite(r.radius)&&std::isfinite(r.opacity)&&
        r.opacity>0&&r.opacity<=1&&r.u>=0&&r.u<=1&&r.v>=0&&r.v<=1&&r.radius>=.001f&&r.radius<=.02f;
}
struct StereoReticle {
    std::uint64_t frameId{},tracking{},space{},sampleNs{};
    std::array<ScreenReticle,2> eyes{};
    StereoReticle forFrame(const FrameContext& frame,std::uint64_t now)const{
        if(frameId!=frame.renderFrameId||!frameId||tracking!=frame.epochs.tracking||!tracking||
           space!=frame.epochs.referenceSpace||!space||!sampleNs||now<sampleNs||now-sampleNs>100'000'000)return {};
        auto result=*this;for(auto& eye:result.eyes)if(!validReticle(eye))eye={};return result;
    }
};
}
