#pragma once
// Right-eye scissor remap for the stereo replay.
//
// Re-emitting every recorded vkCmdSetScissor verbatim is not enough: a scissor rectangle that the engine computed
// in LEFT-eye screen space is wrong for the right eye. The visible case is the moon:
//   rage::SkyDome::Draw (0x2a8254c) -> rage::SkyDome::SetMoonExtents(grcViewport const&) (0x2a8190c, PLT 0x2c4c390)
//   projects the 4 corners of the moon quad with the LEFT grcViewport (grcViewport::Transform), takes their pixel
//   bounding box, clamps it to the target and calls grcDevice::SetScissor; the moon technique (EmbeddedModel::Draw of
//   the sky dome, technique +0x3b4) is then drawn under that scissor and the scissor is reset to the full target.
// In the right eye the moon lands some pixels to the side (asymmetric per-eye frusta) while the scissor stays where the
// LEFT moon was, so the right eye shows only the part of the moon that overlaps the left rectangle ("cut in half").
//
// Remap: a sub-rectangle scissor (an edge strictly inside the current viewport) is mapped into the right eye with the
// replay's clip correction C (clipR = C * clipL, the same matrix the patched vertex shaders apply) evaluated at the far
// plane (NDC z = 1, D24 forward Z: far = 7500 m). That is exact for content at sky distance (moon, sun, sky dome) and is
// the identity for C = I. Edges lying on the viewport border stay on the border, so full-target and letterbox
// scissors (sagRenderer::ResetScissor: (0, border, w, h-2*border)) are unchanged. The other engine sub-rect scissors
// (sagRenderer::SetBoundScissor from DrawPortals / RenderLightsList) are issued in the light pre-pass, which is not
// replayed (native per-eye), and the volumetric lights of the tail only reset the scissor.
#include <vulkan/vulkan.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace rdrvr::engine::stereo::scissor {
enum class Result : unsigned {Full=0,Remapped=1,Declined=2};
// Diagnostics (report stereoReplay.scissor). Any thread (inline replay on the render thread, worker jobs).
struct Counters {
    std::atomic<std::uint64_t> seen{},full{},remapped{},declined{},verbatim{},empty{};
    std::atomic<std::uint64_t> lastLeft{},lastRight{};           // packed x|y<<16|w<<32|h<<48 (16 bits each)
    std::atomic<std::int32_t> lastShiftX{},lastShiftY{},maxAbsShiftX{};
};
inline Counters counters{};
inline std::uint64_t pack(const VkRect2D& r) noexcept {
    return (std::uint64_t(std::uint16_t(r.offset.x)))|(std::uint64_t(std::uint16_t(r.offset.y))<<16)|
           (std::uint64_t(std::uint16_t(r.extent.width))<<32)|(std::uint64_t(std::uint16_t(r.extent.height))<<48);}

// in: the recorded (left) rectangle; vp: the viewport in effect; c: clip correction, column-major (out = c * in).
// out is written only for Remapped. Pure function (host-tested).
inline Result remap(const VkRect2D& in,const VkViewport& vp,const float* c,VkRect2D& out) noexcept {
    if(!c||!std::isfinite(vp.x)||!std::isfinite(vp.y)||!std::isfinite(vp.width)||!std::isfinite(vp.height)||
       std::fabs(vp.width)<1.f||std::fabs(vp.height)<1.f)return Result::Declined;
    const double vx0=std::min<double>(vp.x,double(vp.x)+vp.width),vx1=std::max<double>(vp.x,double(vp.x)+vp.width);
    const double vy0=std::min<double>(vp.y,double(vp.y)+vp.height),vy1=std::max<double>(vp.y,double(vp.y)+vp.height);
    const double x0=in.offset.x,y0=in.offset.y,x1=x0+double(in.extent.width),y1=y0+double(in.extent.height);
    const bool openL=x0<=vx0+0.5,openR=x1>=vx1-0.5,openT=y0<=vy0+0.5,openB=y1>=vy1-0.5;
    if(openL&&openR&&openT&&openB)return Result::Full;
    if(in.extent.width==0||in.extent.height==0)return Result::Full; // nothing to draw in either eye
    const double hw=double(vp.width)*0.5,hh=double(vp.height)*0.5,cx=double(vp.x)+hw,cy=double(vp.y)+hh;
    double mx0=1e30,my0=1e30,mx1=-1e30,my1=-1e30;
    for(unsigned k=0;k<4;++k){
        const double px=(k&1)?x1:x0,py=(k&2)?y1:y0;
        const double p[4]{(px-cx)/hw,(py-cy)/hh,1.0,1.0}; // NDC at the far plane, w = 1
        double q[4];for(unsigned r=0;r<4;++r){double s=0;for(unsigned j=0;j<4;++j)s+=double(c[j*4+r])*p[j];q[r]=s;}
        if(!std::isfinite(q[0])||!std::isfinite(q[1])||!std::isfinite(q[3])||q[3]<=1e-9)return Result::Declined;
        const double sx=q[0]/q[3]*hw+cx,sy=q[1]/q[3]*hh+cy;
        mx0=std::min(mx0,sx);mx1=std::max(mx1,sx);my0=std::min(my0,sy);my1=std::max(my1,sy);
    }
    // Interior edges: mapped, rounded outwards plus one pixel (the engine floors/ceils its own box). Border edges stay.
    double nx0=openL?x0:std::floor(mx0)-1.0,nx1=openR?x1:std::ceil(mx1)+1.0;
    double ny0=openT?y0:std::floor(my0)-1.0,ny1=openB?y1:std::ceil(my1)+1.0;
    const double bx0=std::max(0.0,vx0),by0=std::max(0.0,vy0); // Vulkan: offset >= 0
    nx0=std::clamp(nx0,bx0,vx1);nx1=std::clamp(nx1,bx0,vx1);ny0=std::clamp(ny0,by0,vy1);ny1=std::clamp(ny1,by0,vy1);
    if(nx1<nx0)nx1=nx0;
    if(ny1<ny0)ny1=ny0;
    out.offset.x=std::int32_t(nx0);out.offset.y=std::int32_t(ny0);
    out.extent.width=std::uint32_t(nx1-nx0);out.extent.height=std::uint32_t(ny1-ny0);
    return Result::Remapped;
}
// Replay hook: rect points at the recorded rectangles (first 0, count >= 1); only rectangle 0 is remapped (the engine
// sets one). Returns the rectangle to emit in place of rect[0] (rect[0] itself when nothing changes).
inline VkRect2D forRight(const VkRect2D& rect,bool viewportKnown,const VkViewport& vp,const float* c,bool verbatim) noexcept {
    counters.seen.fetch_add(1,std::memory_order_relaxed);
    if(verbatim){counters.verbatim.fetch_add(1,std::memory_order_relaxed);return rect;}
    if(!viewportKnown){counters.declined.fetch_add(1,std::memory_order_relaxed);return rect;}
    VkRect2D out{};
    switch(remap(rect,vp,c,out)){
    case Result::Full:counters.full.fetch_add(1,std::memory_order_relaxed);return rect;
    case Result::Declined:counters.declined.fetch_add(1,std::memory_order_relaxed);return rect;
    case Result::Remapped:break;
    }
    counters.remapped.fetch_add(1,std::memory_order_relaxed);
    if(!out.extent.width||!out.extent.height)counters.empty.fetch_add(1,std::memory_order_relaxed);
    counters.lastLeft.store(pack(rect),std::memory_order_relaxed);counters.lastRight.store(pack(out),std::memory_order_relaxed);
    const std::int32_t dx=out.offset.x-rect.offset.x,dy=out.offset.y-rect.offset.y;
    counters.lastShiftX.store(dx,std::memory_order_relaxed);counters.lastShiftY.store(dy,std::memory_order_relaxed);
    const std::int32_t ax=dx<0?-dx:dx;if(ax>counters.maxAbsShiftX.load(std::memory_order_relaxed))counters.maxAbsShiftX.store(ax,std::memory_order_relaxed);
    return out;
}
}
