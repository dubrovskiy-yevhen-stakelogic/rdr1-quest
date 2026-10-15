#pragma once
// Live GRAPHICS overrides for unattended testing on the headset.
//   setprop debug.rdrvr.gfx "shadowUpdate=2,grassDistance=1,cascadeCull=1[:tag]"
// On every change of the property value (polled once per second by the teleport main-thread tick, after the startup
// baseline) each key=value pair is applied through the same GraphicsSettings setters the in-headset menu uses, so the
// effect, the menu text and graphics.cfg persistence are exactly as if the row had been changed by hand.
// Keys = graphics.cfg keys of the live rows. Report: teleport.gfxProperty {value, applies, applied, rejected, last}.
#include <atomic>
#include <cstdlib>
#include <string>
#include <string_view>
#include "runtime/GraphicsSettings.h"

namespace rdrvr::gfxprop {

inline std::atomic<unsigned> applies{0},appliedPairs{0},rejectedPairs{0};

inline bool applyPair(std::string_view key,unsigned v) noexcept {
    auto& g=::rdrvr::runtime::graphicsSettings;
    const auto cur=g.snapshot().requested;
    if(key=="shadows")return g.setSunShadows(v!=0,cur.shadowUpdate);
    if(key=="grass")return g.setGrassRow(v!=0,cur.grassDistance);
    if(key=="shadowUpdate")return g.setShadowUpdate(v);
    if(key=="grassDistance")return g.setGrassDistance(v);
    if(key=="foveation")return g.setFoveation(v);
    if(key=="antiAliasing")return g.setAntiAliasing(v);
    if(key=="frameRate")return g.setFrameRate(v);
    if(key=="spaceWarp")return g.setSpaceWarp(v);
    if(key=="postFx")return g.setPostFx(v);
    if(key=="textureFilter")return g.setTextureFilter(v);
    if(key=="mipBias")return g.setMipBias(v);
    if(key=="sharpen")return g.setSharpen(v);
    if(key=="cascadeCull")return g.setCascadeCull(v);
    if(key=="lampShadows")return g.setLampShadows(v);
    if(key=="smallObjectsPx")return g.setSmallObjectsPx(v);
    if(key=="edgeMarginPercent")return g.setEdgeMarginPercent(v);
    if(key=="occlusionV2")return g.setOcclusion(v!=0);
    return false;
}

// Called from the main-thread property poll with the new (changed) value. Returns the number of pairs applied.
inline unsigned apply(const std::string& raw,std::string& last) noexcept {
    applies.fetch_add(1,std::memory_order_relaxed);
    const std::string body=raw.substr(0,raw.find(':'));
    unsigned ok=0;last.clear();
    std::size_t pos=0;
    while(pos<=body.size()){
        const std::size_t comma=body.find(',',pos);
        const std::string_view pair=std::string_view(body).substr(pos,comma==std::string::npos?std::string::npos:comma-pos);
        pos=comma==std::string::npos?body.size()+1:comma+1;
        if(pair.empty())continue;
        const std::size_t eq=pair.find('=');
        bool good=false;
        if(eq!=std::string_view::npos&&eq>0&&eq+1<pair.size()){
            const std::string value(pair.substr(eq+1));char* end{};const unsigned long v=std::strtoul(value.c_str(),&end,10);
            if(end&&!*end&&v<100000)good=applyPair(pair.substr(0,eq),unsigned(v));
        }
        (good?appliedPairs:rejectedPairs).fetch_add(1,std::memory_order_relaxed);
        if(good)++ok;
        if(!last.empty())last+=' ';
        last.append(pair.data(),pair.size());last+=good?"=OK":"=REJECTED";
    }
    return ok;
}

} // namespace rdrvr::gfxprop
