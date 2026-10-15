#pragma once
// INDOOR RT (lever bit 0x10 IR; bit 0x20 IT = diagnostic) and the AO-sphere observer (observation only).
// librdr 1.58 arm64:
//  * sagRenderer::DrawReflectionMap(grcViewport&) 0x2a4df14, JUMP_SLOT 0x2e5f6f8, single caller bl @0x2a534ac {33,107,9,148}
//    (RenderScene_RenderThread, behind `tbz w9,#6` @0x2a53494) -> exact return 0x2a534b0. Skipping it equals the engine's own
//    "no reflection plane" path (CalculateReflectionPlane false -> tbz @0x2a4df68 -> 0x2a4e068: viewport dtor, return). The
//    caller's own WaitForVisibilityList(6) after the call (0x2a534b0..0x2a534cc) stays native.
//  * sagRenderer::DrawTreeRenderTargets() 0x2a4d8cc, JUMP_SLOT 0x2e5f6b8, single caller bl @0x2a53228 {162,107,9,148} -> ret
//    0x2a5322c. Diagnostic only (tree imposters seen through the windows could go stale).
//  * Gate: the exact caller, IR / IT lever bit in force for this frame, indoor::flag()==1 (IndoorState.h, filtered). Theater/menu frames
//    are not special-cased: indoors there is no reflection plane in view either.
//  * renLightPrePassLightsMgr::DrawAOSphere(const Vector4&, bool) 0x2a3909c, JUMP_SLOT 0x2e5f468, exact returns 0x2a39510
//    (actor loop, call {193,209,9,148} @0x2a3950c) and 0x2a3959c (ellipsoid loop, call {158,209,9,148} @0x2a39598), both in
//    DrawAOSpheres 0x2a392dc. Observer: counts per kind and a radius (v4.w) histogram; always native.
// Report graphics.indoorRt{hooked, calls, skippedReflection, skippedTree, ...} and graphics.aoSpheres{actor, ellipsoid, radiusHist}.
#include "IndoorState.h"
#include "S02Levers.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>

namespace rdrvr::engine::indoorrt {
// ---- librdr 1.58 arm64 (module-relative) ----
inline constexpr std::uintptr_t reflectionOffset=0x2a4df14,reflectionGotOffset=0x2e5f6f8,reflectionReturnOffset=0x2a534b0;
inline constexpr std::array<unsigned char,16> reflectionFingerprint{
#include "gamecode/IndoorRt_185205c_16_8d.inc"
};
inline constexpr std::array<unsigned char,4> reflectionCallFingerprint{33,107,9,148};
inline constexpr std::uintptr_t reflectionGateOffset=0x2a53494;  // tbz w9,#6 (env flag bit 6 gate of the call)
inline constexpr std::array<unsigned char,4> reflectionGateFingerprint{9,2,48,54};
inline constexpr std::uintptr_t noPlaneOffset=0x2a4df64;         // bl CalculateReflectionPlane; tbz w0,#0 -> 0x2a4e068 (no plane: return)
inline constexpr std::array<unsigned char,8> noPlaneFingerprint{255,247,7,148,0,8,0,54};
inline constexpr std::uintptr_t treeOffset=0x2a4d8cc,treeGotOffset=0x2e5f6b8,treeReturnOffset=0x2a5322c;
inline constexpr std::array<unsigned char,16> treeFingerprint{
#include "gamecode/IndoorRt_2a4d8cc_16_8d.inc"
};
inline constexpr std::array<unsigned char,4> treeCallFingerprint{162,107,9,148};
inline constexpr std::uintptr_t aoOffset=0x2a3909c,aoGotOffset=0x2e5f468,aoActorReturnOffset=0x2a39510,aoEllipsoidReturnOffset=0x2a3959c;
inline constexpr std::array<unsigned char,32> aoFingerprint{
#include "gamecode/IndoorRt_2a3909c_32_8d.inc"
};
inline constexpr std::array<unsigned char,4> aoActorCallFingerprint{193,209,9,148},aoEllipsoidCallFingerprint{158,209,9,148};

using Reflection=void(*)(void*,void*);
using Tree=void(*)(void*);
using AoSphere=void(*)(void*,const float*,bool);
inline Reflection originalReflection{};
inline Tree originalTree{};
inline AoSphere originalAo{};
struct Config {std::uintptr_t reflectionReturn{},treeReturn{},aoActorReturn{},aoEllipsoidReturn{};};
inline Config config;
struct Stats {
    std::atomic<bool> reflectionHooked{},treeHooked{},aoHooked{};
    std::atomic<const char*> reflectionFp{"unconfigured"},treeFp{"unconfigured"},aoFp{"unconfigured"};
    std::atomic<std::uint64_t> reflectionCalls{},reflectionForeign{},skippedReflection{},reflectionIndoorNative{};
    std::atomic<std::uint64_t> treeCalls{},treeForeign{},skippedTree{};
    // AO observer
    std::atomic<std::uint64_t> aoCalls{},aoForeign{},aoActor{},aoEllipsoid{},aoFrames{};
    std::atomic<std::uint32_t> aoActorMax{},aoEllipsoidMax{};
    std::array<std::atomic<std::uint64_t>,7> radiusHist{}; // <0.25, <0.5, <1, <2, <4, <8, >=8 m (actor+ellipsoid)
    std::array<std::atomic<std::uint64_t>,7> ellipsoidRadiusHist{};
};
inline Stats stats;
inline constexpr float radiusEdges[6]{0.25f,0.5f,1.f,2.f,4.f,8.f};

template<class Code,class Read> void configure(std::uintptr_t base,Code code,Read read) {
    const auto got=[&](std::uintptr_t slot,std::uintptr_t target){std::uintptr_t v{};return read(nullptr,base+slot,&v,sizeof v)&&v==base+target;};
    config={base+reflectionReturnOffset,base+treeReturnOffset,base+aoActorReturnOffset,base+aoEllipsoidReturnOffset};
    stats.reflectionFp.store(!code(reflectionOffset,reflectionFingerprint)?"code":!code(reflectionReturnOffset-4,reflectionCallFingerprint)?"call":
        !code(reflectionGateOffset,reflectionGateFingerprint)||!code(noPlaneOffset,noPlaneFingerprint)?"gate":!got(reflectionGotOffset,reflectionOffset)?"got":"ok");
    stats.treeFp.store(!code(treeOffset,treeFingerprint)?"code":!code(treeReturnOffset-4,treeCallFingerprint)?"call":!got(treeGotOffset,treeOffset)?"got":"ok");
    stats.aoFp.store(!code(aoOffset,aoFingerprint)?"code":!code(aoActorReturnOffset-4,aoActorCallFingerprint)||!code(aoEllipsoidReturnOffset-4,aoEllipsoidCallFingerprint)?"call":
        !got(aoGotOffset,aoOffset)?"got":"ok");
}
inline bool fpOk(const std::atomic<const char*>& f) noexcept {const char* s=f.load(std::memory_order_relaxed);return s&&s[0]=='o'&&s[1]=='k';}
inline bool indoorNow() noexcept {return indoor::flag()==1;}

// ---- GOT replacements (TraceEntry passes the return address) ----
inline void reflection(void* renderer,void* viewport,std::uintptr_t caller) noexcept {
    auto& s=stats;s.reflectionCalls.fetch_add(1,std::memory_order_relaxed);
    if(caller!=config.reflectionReturn){s.reflectionForeign.fetch_add(1,std::memory_order_relaxed);originalReflection(renderer,viewport);return;}
    if(indoorNow()){
        if(s02::wants(s02::IR)){s.skippedReflection.fetch_add(1,std::memory_order_relaxed);return;} // = the engine's no-plane path
        s.reflectionIndoorNative.fetch_add(1,std::memory_order_relaxed);
    }
    originalReflection(renderer,viewport);
}
inline void tree(void* renderer,std::uintptr_t caller) noexcept {
    auto& s=stats;s.treeCalls.fetch_add(1,std::memory_order_relaxed);
    if(caller!=config.treeReturn){s.treeForeign.fetch_add(1,std::memory_order_relaxed);originalTree(renderer);return;}
    if(indoorNow()&&s02::wants(s02::IT)){s.skippedTree.fetch_add(1,std::memory_order_relaxed);return;}
    originalTree(renderer);
}
inline unsigned radiusBucket(float r) noexcept {if(!std::isfinite(r))return 6;unsigned b=0;while(b<6&&r>=radiusEdges[b])++b;return b;}
inline void storeMax(std::atomic<std::uint32_t>& a,std::uint32_t v) noexcept {if(v>a.load(std::memory_order_relaxed))a.store(v,std::memory_order_relaxed);}
// Render thread (DrawAOSpheres runs inside the left colour stage); per-scene counts close at the next scene.
inline std::uint64_t aoScene{};inline std::uint32_t aoSceneActor{},aoSceneEllipsoid{};
inline void aoCloseScene(std::uint64_t scene) noexcept {
    if(scene==aoScene)return;
    if(aoSceneActor||aoSceneEllipsoid){stats.aoFrames.fetch_add(1,std::memory_order_relaxed);storeMax(stats.aoActorMax,aoSceneActor);storeMax(stats.aoEllipsoidMax,aoSceneEllipsoid);}
    aoScene=scene;aoSceneActor=0;aoSceneEllipsoid=0;
}
inline void aoSphere(void* manager,const float* v4,bool flag,std::uintptr_t caller) noexcept {
    auto& s=stats;s.aoCalls.fetch_add(1,std::memory_order_relaxed);
    const bool actor=caller==config.aoActorReturn,ellipsoid=caller==config.aoEllipsoidReturn;
    if(!(actor||ellipsoid)||!v4)s.aoForeign.fetch_add(1,std::memory_order_relaxed);
    else{
        aoCloseScene(s02::sceneSerial.load(std::memory_order_relaxed));
        (actor?s.aoActor:s.aoEllipsoid).fetch_add(1,std::memory_order_relaxed);if(actor)++aoSceneActor;else ++aoSceneEllipsoid;
        float r{};std::memcpy(&r,v4+3,sizeof r);const unsigned b=radiusBucket(r);
        s.radiusHist[b].fetch_add(1,std::memory_order_relaxed);if(ellipsoid)s.ellipsoidRadiusHist[b].fetch_add(1,std::memory_order_relaxed);
    }
    originalAo(manager,v4,flag);
}
inline std::string report() {
    const auto& s=stats;std::ostringstream o;
    const auto f=[](const std::atomic<const char*>& a){const char* x=a.load();return x?x:"";};
    o<<"\"indoorRt\":{\"hooked\":{\"reflection\":"<<(s.reflectionHooked.load()?1:0)<<",\"tree\":"<<(s.treeHooked.load()?1:0)<<'}'
     <<",\"fingerprint\":{\"reflection\":\""<<f(s.reflectionFp)<<"\",\"tree\":\""<<f(s.treeFp)<<"\"},\"indoor\":"<<indoor::flag()
     <<",\"calls\":{\"reflection\":"<<s.reflectionCalls.load()<<",\"reflectionForeign\":"<<s.reflectionForeign.load()<<",\"tree\":"<<s.treeCalls.load()
     <<",\"treeForeign\":"<<s.treeForeign.load()<<"},\"skippedReflection\":"<<s.skippedReflection.load()<<",\"reflectionIndoorNative\":"<<s.reflectionIndoorNative.load()
     <<",\"skippedTree\":"<<s.skippedTree.load()<<'}';
    const auto frames=s.aoFrames.load();
    o<<",\"aoSpheres\":{\"hooked\":"<<(s.aoHooked.load()?1:0)<<",\"fingerprint\":\""<<f(s.aoFp)<<"\",\"calls\":"<<s.aoCalls.load()<<",\"foreign\":"<<s.aoForeign.load()
     <<",\"actor\":"<<s.aoActor.load()<<",\"ellipsoid\":"<<s.aoEllipsoid.load()<<",\"frames\":"<<frames
     <<",\"actorPerFrame\":"<<(frames?double(s.aoActor.load())/double(frames):-1.0)<<",\"ellipsoidPerFrame\":"<<(frames?double(s.aoEllipsoid.load())/double(frames):-1.0)
     <<",\"actorMax\":"<<s.aoActorMax.load()<<",\"ellipsoidMax\":"<<s.aoEllipsoidMax.load()<<",\"radiusEdgesM\":[0.25,0.5,1,2,4,8],\"radiusHist\":[";
    for(unsigned i=0;i<s.radiusHist.size();++i)o<<(i?",":"")<<s.radiusHist[i].load();
    o<<"],\"ellipsoidRadiusHist\":[";for(unsigned i=0;i<s.ellipsoidRadiusHist.size();++i)o<<(i?",":"")<<s.ellipsoidRadiusHist[i].load();
    o<<"]}";
    return o.str();
}
} // namespace rdrvr::engine::indoorrt
