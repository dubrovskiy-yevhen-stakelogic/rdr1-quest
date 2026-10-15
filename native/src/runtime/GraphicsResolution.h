#pragma once
#include "TrackedViews.h"
#include <algorithm>
#include <cstdint>

namespace rdrvr::runtime {
struct GraphicsExtent {std::uint32_t width{},height{};explicit operator bool()const{return width&&height;}};
inline GraphicsExtent graphicsExtent(std::uint32_t width,std::uint32_t height,
                                    std::uint32_t maxWidth,std::uint32_t maxHeight,unsigned percent) noexcept {
    if(width<16||height<16||width>8192||height>8192||maxWidth<width||maxHeight<height||percent<50||percent>150)return {};
    const double scale=std::min({double(percent)/100.,double(std::min(maxWidth,8192u))/width,double(std::min(maxHeight,8192u))/height});
    return {static_cast<std::uint32_t>(width*scale),static_cast<std::uint32_t>(height*scale)};
}
// Render extra pixels around the optical FOV. The enlarged FOV is stored with
// the rendered pair and submitted unchanged; never relabel old pixels with a
// new head pose or a smaller field of view to hide missing geometry.
inline Fov graphicsFov(Fov f,unsigned margin) noexcept {
    if(!usableFov(f)||(margin!=10&&margin!=20))return f;
    const auto grow=[&](float low,float high){
        const double a=std::tan(low),b=std::tan(high),center=(a+b)*.5,half=(b-a)*.5*(1.+margin/100.);
        return std::array<float,2>{float(std::atan(center-half)),float(std::atan(center+half))};
    };
    const auto x=grow(f.left,f.right),y=grow(f.down,f.up);
    const Fov grown{x[0],x[1],y[1],y[0]};return usableFov(grown)?grown:f;
}
}
