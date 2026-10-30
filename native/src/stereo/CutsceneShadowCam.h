#pragma once
// SUN SHADOW CAM (MOVEMENT row, cfg cutsceneShadows: 0 STOCK = native, 1 EYE IN CUTSCENES default, 2 EYE ALWAYS = normal play too): in a stereo cutscene the sun cascades
// follow the eye instead of the stock shot camera. The mod hands the stock viewport to CreateSceneVisibility unchanged, so
// the eye can stand far from the camera the cascades are built around; every stock cut would move the cascade volumes while the
// eye stays put. librdr 1.58 arm64:
//  * rage::CShadowMap::CalcOrthoViewportAroundSphere @0x29467e4 (GOT 0x2e24ac0; x4 = camera Matrix34) -> CalculateSphereAroundFrustumSlice
//    @0x29463c0 reads the camera ONLY from that matrix; the viewport argument gives only FOV / aspect, so the stock FOV stays.
//    Callers: GetOrthoViewport @0x29467b4 tail call (b @0x29467e0: CreateSceneVisibility return 0x2a5141c, PrepareShadowVisibilityRequests
//    return 0x2a52534, GenerateCCShadowMapViewports return 0x2a5188c), RenderIntoShadowMaps bl @0x294742c (render thread, return 0x2947430,
//    camera = grcViewport::sm_Current copied to +0xf0), CalcAndSetOrthoViewportAroundSphere bl @0x294774c.
//  * rage::CShadowMap::GetCullPlanes @0x294750c (GOT 0x2e24ae8; x1 = viewport, copied by the engine, far clip = cascade range): the frustum planes
//    facing away from the sun that cull casters in PrepareShadowVisibilityRequests (bl @0x2a52408, return 0x2a5240c). In a stereo cutscene that
//    call gets the mod's cull clone (union of both eye frusta, leased by PrepareMainVisibilityList @0x2a51588, earlier in
//    CreateSceneVisibility); without a clone the stock viewport stays. RenderIntoShadowMaps' own call (bl @0x29472e4) only feeds the clip
//    planes, which are off (+0x68 = 0 in the ctor @0x2945ef8): not changed.
// Threads: the visibility calls run on the main thread, the atlas on the render thread, concurrently: while the render thread draws frame N-1 the main
// thread already prepares N (ShadowUpdate.h). Each thread therefore keeps its own eye camera: main = this visibility call (publish), render = the ticket
// of the frame RenderScene draws (renderFrame), the camera its caster lists were built around. With the newest eye on the render thread a cut would draw
// frame N-1's casters through frame N's cascade matrices.
// Not changed: lamp shadow selection (SelectShadows @0x2a4a658 scores point lamps from the stock viewport), tree / LOD camera of the requests.
// Lever: debug.rdrvr.vr.cutscene.shadowcam 0 / 1 / 2 beats the row. Report graphics.shadowUpdate.cutsceneCam.
#include "ShadowTimeline.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <ostream>

namespace rdrvr::engine::shadowupdate::cscam {
inline constexpr std::uintptr_t calcOrthoOffset=0x29467e4,calcOrthoGotOffset=0x2e24ac0,calcOrthoRenderReturnOffset=0x2947430;
inline constexpr std::uintptr_t cullPlanesOffset=0x294750c,cullPlanesGotOffset=0x2e24ae8,cullPlanesMainReturnOffset=0x2a5240c;
inline constexpr std::array<unsigned char,16> calcOrthoFingerprint{
#include "gamecode/CutsceneShadowCam_1be9fcc_16_8d.inc"
};
inline constexpr std::array<unsigned char,16> cullPlanesFingerprint{
#include "gamecode/CutsceneShadowCam_188c85c_16_8d.inc"
};
inline constexpr std::array<unsigned char,4> calcOrthoCallFingerprint{37,197,11,148};  // bl 0x2c388c0 @0x294742c
inline constexpr std::array<unsigned char,4> calcOrthoTailFingerprint{56,200,11,20};   // b  0x2c388c0 @0x29467e0
inline constexpr std::array<unsigned char,4> cullPlanesCallFingerprint{66,153,7,148};  // bl 0x2c38910 @0x2a52408
inline constexpr std::uintptr_t calcOrthoCallOffset=0x294742c,calcOrthoTailOffset=0x29467e0,cullPlanesCallOffset=0x2a52408;
using CalcOrtho=void(*)(void*,void*,void*,void*,const void*,const void*);
using CullPlanes=int(*)(const void*,const void*,void*,bool);
inline CalcOrtho originalCalcOrtho{};
inline CullPlanes originalCullPlanes{};
inline std::atomic<bool> hooked{},planesHooked{};
inline std::atomic<bool> active{};  // main thread's view: the last visibility call was a stereo cutscene frame with the row on
inline std::atomic<bool> wanted{true},always{}; // always = 2 EYE ALWAYS: normal play too (a fast head turn can force full atlas renders through the coverage trigger: check atlasCpuMs)
inline std::mutex lock;
inline std::array<float,16> eyeCamera{}; // Matrix34 memory layout (4 rows of 16 bytes), as grcViewport+0x40
inline bool renderActive{};inline std::array<float,16> renderCamera{}; // render thread only: the eye of the frame it is drawing
struct Stats {std::atomic<std::uint64_t> publishes{},clears{},refused{},renderFrames{},swapsMain{},swapsRender{},planeSwaps{},planeNoClone{};
              std::atomic<float> distLast{-1.f},distMax{0.f};};
inline Stats stats;
// Left eye rotation, position between the eyes. left / right: eye world matrices of the plan.
inline bool compose(std::array<float,16>& m,const float* left,const float* right) noexcept {
    std::memcpy(m.data(),left,sizeof(float)*16);
    for(unsigned i=12;i<15;++i)m[i]=(left[i]+right[i])*.5f;
    bool finite=true;for(unsigned i=0;i<15;++i)finite=finite&&std::isfinite(m[i]);
    if(!finite)stats.refused.fetch_add(1,std::memory_order_relaxed);
    return finite;
}
// Main thread (GameXr gameBeginVisibility), before the native CreateSceneVisibility.
inline void publish(const float* left,const float* right) noexcept {
    std::array<float,16> m{};
    if(!compose(m,left,right)){active.store(false,std::memory_order_relaxed);return;}
    {std::lock_guard guard(lock);eyeCamera=m;}
    active.store(true,std::memory_order_relaxed);stats.publishes.fetch_add(1,std::memory_order_relaxed);
}
inline void clear() noexcept {if(active.exchange(false,std::memory_order_relaxed))stats.clears.fetch_add(1,std::memory_order_relaxed);}
// Render thread (GameXr gameObserveFrozenCamera: the first SetCurrent of RenderScene_RenderThread, before DrawShadowMaps).
inline void renderFrame(const float* left,const float* right) noexcept {
    renderActive=compose(renderCamera,left,right);if(renderActive)stats.renderFrames.fetch_add(1,std::memory_order_relaxed);
}
inline void renderClear() noexcept {renderActive=false;}
// CalcOrtho hook: the eye camera for this call. renderThread = the call inside RenderIntoShadowMaps. stock = the engine's camera argument.
inline bool eye(float* out,const void* stock,bool renderThread) noexcept {
    if(renderThread){if(!renderActive)return false;std::memcpy(out,renderCamera.data(),sizeof(float)*16);}
    else{
        if(!active.load(std::memory_order_relaxed))return false;
        std::lock_guard guard(lock);std::memcpy(out,eyeCamera.data(),sizeof(float)*16);
    }
    float s[16];std::memcpy(s,stock,sizeof s);
    const float dx=s[12]-out[12],dy=s[13]-out[13],dz=s[14]-out[14];const float d=std::sqrt(dx*dx+dy*dy+dz*dz);
    if(std::isfinite(d)){stats.distLast.store(d,std::memory_order_relaxed);if(d>stats.distMax.load(std::memory_order_relaxed))stats.distMax.store(d,std::memory_order_relaxed);
        shadowtimeline::max(shadowtimeline::EyeDistDm,std::uint32_t(std::min(d,1e6f)*10.f));}
    (renderThread?stats.swapsRender:stats.swapsMain).fetch_add(1,std::memory_order_relaxed);
    shadowtimeline::bump(shadowtimeline::EyeCamera);
    return true;
}
inline void report(std::ostream& o) {
    const auto& s=stats;
    o<<"\"cutsceneCam\":{\"hooked\":"<<(hooked.load()?1:0)<<",\"planesHooked\":"<<(planesHooked.load()?1:0)<<",\"wanted\":"<<(wanted.load()?1:0)<<",\"always\":"<<(always.load()?1:0)
     <<",\"active\":"<<(active.load()?1:0)<<",\"publishes\":"<<s.publishes.load()<<",\"clears\":"<<s.clears.load()<<",\"refused\":"<<s.refused.load()<<",\"renderFrames\":"<<s.renderFrames.load()
     <<",\"swapsMain\":"<<s.swapsMain.load()<<",\"swapsRender\":"<<s.swapsRender.load()
     <<",\"planeSwaps\":"<<s.planeSwaps.load()<<",\"planeNoClone\":"<<s.planeNoClone.load()
     <<",\"eyeStockM\":{\"last\":"<<s.distLast.load()<<",\"max\":"<<s.distMax.load()<<"}}";
}
} // namespace rdrvr::engine::shadowupdate::cscam
