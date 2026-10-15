#pragma once
#include "Math.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include <limits>

namespace rdrvr::runtime {
enum class FragmentRateProfile {Off,Conservative,Medium};
struct FragmentRateCenter {float x{.5f},y{.5f};};
struct FragmentRateMapSpec {
    unsigned width{},height{},texelWidth{},texelHeight{},layers{};
    FragmentRateProfile profile{FragmentRateProfile::Off};
    std::array<FragmentRateCenter,2> centers{};
    // Explicit 1x1 / 2x2 ellipse radii (fractions of the half extents) for a non-Off profile; 0 = the
    // profile's own radii (Conservative .60/.85, Medium .45/.70).
    double inner{},outer{};
    // Vertical scale of both ellipses (dy is divided by it; .82 = vertical radii x .82). 1.0 keeps every map
    // bit-identical to the unscaled profile (x/1.0 is exact). Accepted range [.25, 4].
    double verticalScale{1.0};
};
struct FragmentRateMap {
    unsigned width{},height{},layers{};
    // Tightly packed R8_UINT, each layer row-major. VkImage row/array pitches
    // belong to the image owner and must not be assumed to match this vector.
    std::vector<std::uint8_t> pixels;
    std::array<std::uint64_t,3> tileCounts{}; // 1x1,2x2,4x4 (not measured GPU work)
};

// Optical forward-axis location for runtime::projection's Vulkan convention.
// Not eye tracking. The map is static until FOV/profile/extent changes.
inline bool fragmentRateCenter(const Mat4& projection,FragmentRateCenter& output) noexcept {
    for(float x:projection)if(!std::isfinite(x))return false;
    if(projection[0]<=0 || projection[5]>=0 || projection[3]!=0 || projection[7]!=0 ||
       projection[11]!=-1 || projection[15]!=0)return false;
    const FragmentRateCenter next{(1-projection[8])*.5f,(1-projection[9])*.5f};
    if(next.x<.1f || next.x>.9f || next.y<.1f || next.y>.9f)return false;
    output=next;return true;
}

// Capability checking is the caller's responsibility: all three exact rates,
// R8_UINT attachment usage, texel limits, layers and the device feature must be
// supported. This function neither enables VRS nor changes eye dimensions.
inline bool buildFragmentRateMap(const FragmentRateMapSpec& spec,FragmentRateMap& output) noexcept {
    auto power=[](unsigned n){return n&&!(n&(n-1))&&n<=256;};
    if(!spec.width || !spec.height || spec.width>8192 || spec.height>8192 ||
       !power(spec.texelWidth) || !power(spec.texelHeight) || !spec.layers || spec.layers>2 ||
       (spec.profile!=FragmentRateProfile::Off && spec.profile!=FragmentRateProfile::Conservative && spec.profile!=FragmentRateProfile::Medium))return false;
    if(!std::isfinite(spec.verticalScale)||spec.verticalScale<.25||spec.verticalScale>4)return false;
    for(unsigned layer=0;layer<spec.layers;++layer){const auto& c=spec.centers[layer];
        if(!std::isfinite(c.x)||!std::isfinite(c.y)||c.x<.1f||c.x>.9f||c.y<.1f||c.y>.9f)return false;
    }
    const unsigned w=(spec.width+spec.texelWidth-1)/spec.texelWidth,h=(spec.height+spec.texelHeight-1)/spec.texelHeight;
    const std::size_t count=std::size_t(w)*h*spec.layers;
    if(count>2*1024*1024)return false;
    try {
        FragmentRateMap next{};next.width=w;next.height=h;next.layers=spec.layers;next.pixels.resize(count);
        const bool explicitRadii=spec.inner>0&&spec.outer>=spec.inner&&spec.outer<=2;
        const double inner=explicitRadii?spec.inner:spec.profile==FragmentRateProfile::Medium?.45:.60;
        const double outer=explicitRadii?spec.outer:spec.profile==FragmentRateProfile::Medium?.70:.85;
        for(unsigned layer=0;layer<spec.layers;++layer){
            const double cx=double(spec.centers[layer].x)*spec.width,cy=double(spec.centers[layer].y)*spec.height;
            for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
                const double x0=x*spec.texelWidth,x1=std::min(spec.width,(x+1)*spec.texelWidth);
                const double y0=y*spec.texelHeight,y1=std::min(spec.height,(y+1)*spec.texelHeight);
                const double dx=(std::clamp(cx,x0,x1)-cx)/(double(spec.width)*.5);
                const double dy=(std::clamp(cy,y0,y1)-cy)/(double(spec.height)*.5)/spec.verticalScale; // Divided last (x/1.0 is exact)
                const double distance2=dx*dx+dy*dy;
                // A tile touching a finer region retains that finer rate.
                const unsigned rate=spec.profile==FragmentRateProfile::Off || distance2<=inner*inner?0:distance2<=outer*outer?1:2;
                next.pixels[(std::size_t(layer)*h+y)*w+x]=static_cast<std::uint8_t>(rate*5);
                ++next.tileCounts[rate];
            }
        }
        output=std::move(next);return true;
    }catch(...){return false;}
}

// The FOVEATION level table (GRAPHICS menu 0..4), the single source for engine/GameFragmentRate.cpp and the
// host test. Radii are x1000 fractions of the half extents (1x1 inside inner, 2x2 inside outer, 4x4 beyond); verticalScale
// x1000. LOW/MEDIUM/HIGH equal GameFragmentRate.cpp levelRadii with verticalScale 1000: byte-identical maps.
constexpr unsigned fragmentRateLevelCount=5;
struct FragmentRateLevel {const char* name;int inner,outer,verticalScale;};
inline constexpr std::array<FragmentRateLevel,fragmentRateLevelCount> fragmentRateLevels{{
    {"OFF",0,0,1000},{"LOW",700,950,1000},{"MEDIUM",550,800,1000},{"HIGH",400,650,1000},{"ULTRA",300,450,820}}};
// Shaded fragments of a map relative to full rate, x1000: round(1000*(t0+t1/4+t2/16)/(t0+t1+t2)); 0 for an empty map.
inline unsigned fragmentRatePermille(const std::array<std::uint64_t,3>& t) noexcept {
    const double n=double(t[0])+double(t[1])+double(t[2]);
    if(!(n>0))return 0;
    return unsigned(std::lround(1000.*(double(t[0])+double(t[1])/4.+double(t[2])/16.)/n));
}
} // namespace rdrvr::runtime
