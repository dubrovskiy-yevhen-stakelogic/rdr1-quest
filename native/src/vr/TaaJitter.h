#pragma once
// TAA FULL: sub-pixel projection jitter of the stereo eye clones. Vulkan-free, header-only (host-testable).
//
// Who does what (all on the render thread):
//   TaaPass (recordEye)      publishes `published`: enabled = TAA FULL requested AND the TAA pass ran on both eyes of the
//                            last frame (so a skipping / failed / killed TAA stops the jitter one frame later), the eye
//                            extents, the amplitude / sequence length (properties debug.rdrvr.vr.taa.jitter / .jitterlen).
//   EyeViewports::prepare    issuer.issue(plan.sceneId) once per scene (a second prepare of the same scene reuses the sample)
//                            and uploads apply(plan.projection[e], offset) into the eye clones ONLY. plan.projection stays
//                            un-jittered for every other consumer (culling / visibility, hands, timewarp FOV, SpaceWarp, TAA
//                            reprojection, aim): only the engine's eye draws (left native + right replay via the clone's
//                            regenerated relative clip) see the sub-pixel offset.
//   TaaPass (recordEye)      issuer.find(plan.sceneId) = the offset this eye was really drawn with (0 when none).
// Same framebuffer-pixel offset for both eyes (stereo correspondences stay exact; no per-eye shimmer difference).
// Sequence: Halton(2,3), index 1..length, centred: offset = (H2(i) - 0.5, H3(i) - 0.5) * amplitude, amplitude 1 = +-0.5 px.
#include "runtime/Math.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace rdrvr::vr::taa::jitter {
inline double halton(std::uint32_t index,std::uint32_t base) noexcept {
    double f=1,r=0;while(index){f/=double(base);r+=f*double(index%base);index/=base;}return r;
}
// Offset (framebuffer pixels: x right, y down) of sequence position `index` (0-based, wraps at length).
inline std::array<float,2> offset(std::uint32_t index,std::uint32_t length,float amplitude) noexcept {
    const std::uint32_t i=(length?index%length:0u)+1u;
    return {float((halton(i,2)-0.5)*amplitude),float((halton(i,3)-0.5)*amplitude)};
}
// The projection whose image is P's image moved by (dx, dy) framebuffer pixels of a width x height eye drawn with a
// positive-height Vulkan viewport: F P' = T F P (T = Vulkan NDC translation by 2dx/W, 2dy/H; F = the Y flip of the GL-style
// engine projection exactly as hands::vulkanProjection decides it: P[5] > 0). Only the shear terms P[8] / P[9] change (the
// w row of a perspective P is (0, 0, -1, 0)); P[5] keeps its sign, so the flip decision is unchanged.
inline runtime::Mat4 apply(const runtime::Mat4& p,float dx,float dy,std::uint32_t width,std::uint32_t height) noexcept {
    if(!width||!height||(dx==0.f&&dy==0.f))return p;
    const float tx=2.f*dx/float(width),ty=(p[5]>0?-1.f:1.f)*2.f*dy/float(height);
    runtime::Mat4 r=p;
    for(int c=0;c<4;++c){r[c*4+0]+=tx*p[c*4+3];r[c*4+1]+=ty*p[c*4+3];}
    return r;
}
inline constexpr std::uint32_t packExtent(std::uint32_t w,std::uint32_t h) noexcept {return w<=0xFFFFu&&h<=0xFFFFu?(w|(h<<16)):0u;}

// TaaPass -> issuer.
struct Published {
    std::atomic<std::uint32_t> enabled{0};          // 1: jitter the next scenes
    std::atomic<std::uint32_t> amplitudeMilli{1000}; // 1000 = +-0.5 px (debug.rdrvr.vr.taa.jitter, percent x10)
    std::atomic<std::uint32_t> length{8};           // Halton positions before wrapping (debug.rdrvr.vr.taa.jitterlen, 2..64)
    std::array<std::atomic<std::uint32_t>,2> extent{}; // packExtent of each eye colour the TAA pass last processed
};
inline Published published;

// One scene's jitter (what the eye clones of that scene were drawn with).
struct Sample {
    std::uint64_t scene{};
    bool on{};
    std::uint32_t index{},length{};
    std::array<float,2> px{};                  // framebuffer pixels, both eyes
    std::array<std::uint32_t,2> extent{};      // packed eye extents the NDC offset was computed for
};
class Issuer {
    mutable std::mutex mutex_;
    std::array<Sample,8> ring_{};unsigned next_{};
    std::uint32_t counter_{};
    std::atomic<std::uint64_t> issued_{},jittered_{},reused_{},found_{},missing_{};
    Sample last_{};
public:
    // Render thread, EyeViewports::prepare. scene 0 / jitter disabled / unknown extents -> on = false (P unchanged).
    Sample issue(std::uint64_t scene) noexcept {
        std::lock_guard lock(mutex_);
        if(scene)for(const auto& s:ring_)if(s.scene==scene){reused_.fetch_add(1,std::memory_order_relaxed);return s;}
        Sample s{};s.scene=scene;
        const auto amplitude=published.amplitudeMilli.load(std::memory_order_relaxed);
        const auto length=published.length.load(std::memory_order_relaxed);
        s.extent={published.extent[0].load(std::memory_order_relaxed),published.extent[1].load(std::memory_order_relaxed)};
        if(scene&&published.enabled.load(std::memory_order_relaxed)&&amplitude&&length>=2&&(s.extent[0]&0xFFFFu)&&(s.extent[0]>>16)&&
           (s.extent[1]&0xFFFFu)&&(s.extent[1]>>16)){
            s.on=true;s.length=length;s.index=counter_++%length;s.px=offset(s.index,length,float(amplitude)/1000.f);
            jittered_.fetch_add(1,std::memory_order_relaxed);
        }
        if(scene){ring_[next_]=s;next_=(next_+1)%unsigned(ring_.size());}
        issued_.fetch_add(1,std::memory_order_relaxed);last_=s;return s;
    }
    // TaaPass: the sample this scene was drawn with (false: never issued / evicted -> treat as no jitter).
    bool find(std::uint64_t scene,Sample& out) noexcept {
        std::lock_guard lock(mutex_);
        if(scene)for(const auto& s:ring_)if(s.scene==scene){out=s;found_.fetch_add(1,std::memory_order_relaxed);return true;}
        missing_.fetch_add(1,std::memory_order_relaxed);out=Sample{};return false;
    }
    // EyeViewports: the jittered matrix was refused (the clones use the plain projection): the scene counts as un-jittered.
    void cancel(std::uint64_t scene) noexcept {
        std::lock_guard lock(mutex_);
        for(auto& s:ring_)if(scene&&s.scene==scene){s.on=false;s.px={};}
        if(last_.scene==scene){last_.on=false;last_.px={};}
    }
    Sample last() const noexcept {std::lock_guard lock(mutex_);return last_;}
    std::uint64_t issued() const noexcept {return issued_.load(std::memory_order_relaxed);}
    std::uint64_t jittered() const noexcept {return jittered_.load(std::memory_order_relaxed);}
    std::uint64_t reused() const noexcept {return reused_.load(std::memory_order_relaxed);}
    std::uint64_t found() const noexcept {return found_.load(std::memory_order_relaxed);}
    std::uint64_t missing() const noexcept {return missing_.load(std::memory_order_relaxed);}
    // Host tests.
    void resetForTests() noexcept {std::lock_guard lock(mutex_);ring_={};next_=0;counter_=0;last_={};}
};
inline Issuer issuer;
} // namespace rdrvr::vr::taa::jitter
