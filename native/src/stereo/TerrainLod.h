#pragma once
// TERRAIN LOD (debug.rdrvr.lod.terrain): knob on the terrain quadtree split distance. Default OFF (native).
// Terrain is NOT drawn through the static lists of StaticLod.h (no CachedVisibility::IsNotTooSmall, no AddToDrawListFade): it is a
// quadtree of terTerrainVisual tiles. librdr 1.58 arm64, module-relative:
//  * sagRenderer::GetTerrainStreamingFactor @0x2a57fbc = ldr s0,[x0,#0x574]; ret (JUMP_SLOT 0x2e5e830). renderer+0x574 is the
//    TerrainLOD graphics option (TerrainLOD::SetValueToGame; lodDiagnostics terrainStreamingFactor = 1.0 on the Quest 3).
//  * Only gameplay caller: sagTerrainWorld::Update bl 0x2aa5d38 (return 0x2aa5d3c) -> str s0,[world+0x9830]; the other PLT user is
//    TerrainLOD::GetValueFromGame (options UI, a tail call, never scaled here).
//  * Readers: Update 0x2aa5dc4 and UpdateVisibilityLocationArray 0x2aa72a4: factor + world+0x9834 -> task+0x60 -> buildLocationListSpu
//    -> BuildLocationList2 @0x2aa8b70 (s0 = factor): a square splits into its 4 finer children while dist^2 < (0.5*size + e)^2 * factor
//    (0x2aa8cc4..0x2aa8d08). So the split distance scales with sqrt(factor); the tiles drawn (sagTerrainWorld::CreateVisibilityList,
//    terTerrainVisual::Draw) and streamed come from that list.
//  => terrain LOD is live and distance-driven at the authored option value (not stuck at high detail); the eye has the stock pixel
//     density, so any change here is a uniform detail cut. This header only scales what Update receives.
// debug.rdrvr.lod.terrain <percent of the native split distance, 10..300> (factor * (p/100)^2); ""/0/100 = native (kill).
// debug.rdrvr.lod.terrain.pin 1 = 25 % (factor / 16): a visibly coarse terrain for an eye dump. Properties re-read once per second.
// Report graphics.terrainLod: native and applied factor, scaled calls, foreign calls.
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif
#if defined(_MSC_VER)&&!defined(__clang__)
#include <intrin.h>
#define RDRVR_TLOD_NOINLINE __declspec(noinline)
#define RDRVR_TLOD_RETURN_ADDRESS() reinterpret_cast<std::uintptr_t>(_ReturnAddress())
#else
#define RDRVR_TLOD_NOINLINE __attribute__((noinline))
#define RDRVR_TLOD_RETURN_ADDRESS() reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))
#endif

namespace rdrvr::engine::terrainlod {
// ---- librdr 1.58 arm64 (module-relative) ----
inline constexpr std::uintptr_t getFactorOffset=0x2a57fbc,getFactorGotOffset=0x2e5e830;
inline constexpr std::uintptr_t updateSiteOffset=0x2aa5d38,updateReturnOffset=0x2aa5d3c; // bl; ldr x8,[x19,#0x68]; str s0,[x22,#0x830]; cbz
inline constexpr std::uintptr_t splitReaderOffset=0x2aa5dc4,splitTestOffset=0x2aa8cc4;
// GetTerrainStreamingFactor 0x2a57fbc: ldr s0,[x0,#0x574]; ret
inline constexpr std::array<unsigned char,8> getFactorFingerprint{0,116,69,189,192,3,95,214};
// sagTerrainWorld::Update 0x2aa5d38: bl GetTerrainStreamingFactor@plt; ldr x8,[x19,#0x68]; str s0,[x22,#0x830]; cbz x8
inline constexpr std::array<unsigned char,16> updateSiteFingerprint{
#include "gamecode/TerrainLod_2aa5d38_16_8d.inc"
};
// Update 0x2aa5dc4: ldr s1,[x22,#0x830] ... fadd s1,s1,s2 (+0x9834) -> task+0x60
inline constexpr std::array<unsigned char,20> splitReaderFingerprint{
#include "gamecode/TerrainLod_2aa5dc4_20_8d.inc"
};
// BuildLocationList2 0x2aa8cc4: split while dist^2 < (0.5*size+e)^2 * factor
inline constexpr std::array<unsigned char,72> splitTestFingerprint{
#include "gamecode/TerrainLod_2aa8cc4_72_8d.inc"
};

// ---- pure ----
struct Rule {unsigned percent{};unsigned pin{};bool operator==(const Rule&) const = default;};
inline float scaleOf(const Rule& r) noexcept {
    if(r.pin==1)return 1.f/16.f;
    if(!r.percent||r.percent==100)return 1.f;
    const float k=float(r.percent)/100.f;return k*k;
}
inline bool parseUnsigned(const char* s,unsigned lo,unsigned hi,unsigned& out) noexcept {
    if(!s||!*s)return false;char* end{};const long v=std::strtol(s,&end,10);
    if(end==s||*end||v<long(lo)||v>long(hi))return false;out=unsigned(v);return true;
}
struct ResolvedRule {Rule rule;bool percentBad{},pinBad{};};
inline ResolvedRule resolve(const std::string& percent,const std::string& pin) noexcept {
    ResolvedRule r{};unsigned v{};
    if(parseUnsigned(percent.c_str(),0,300,v)&&(v==0||v>=10))r.rule.percent=v;else if(!percent.empty())r.percentBad=true;
    if(parseUnsigned(pin.c_str(),0,1,v))r.rule.pin=v;else if(!pin.empty())r.pinBad=true;
    return r;
}

// ---- runtime ----
using GetFactor=float(*)(void*);
inline GetFactor originalGetFactor{};
inline std::uintptr_t updateReturn{};
inline std::atomic<bool> installed{};
inline std::atomic<const char*> fingerprint{"unconfigured"};
inline std::atomic<float> scale{1.f};
inline std::atomic<float> lastNative{-1.f},lastApplied{-1.f};
inline std::atomic<std::uint64_t> calls{},scaledCalls{},foreignCalls{};
inline float getFactorAt(void* renderer,std::uintptr_t caller) {
    const float native=originalGetFactor(renderer);
    if(caller!=updateReturn){foreignCalls.fetch_add(1,std::memory_order_relaxed);return native;}
    calls.fetch_add(1,std::memory_order_relaxed);
    const float k=scale.load(std::memory_order_relaxed);float v=native;
    if(k!=1.f&&std::isfinite(native)&&native>0.f){v=native*k;scaledCalls.fetch_add(1,std::memory_order_relaxed);}
    lastNative.store(native,std::memory_order_relaxed);lastApplied.store(v,std::memory_order_relaxed);
    return v;
}
RDRVR_TLOD_NOINLINE inline float getFactorHook(void* renderer) {return getFactorAt(renderer,RDRVR_TLOD_RETURN_ADDRESS());}

template<class Code,class Read,class Replace>
bool install(std::uintptr_t base,const Code& code,const Read& read,const Replace& replace) {
    const auto got=[&](std::uintptr_t slot,std::uintptr_t target){std::uintptr_t v{};return read(base+slot,&v,sizeof v)&&v==base+target;};
    const char* r="ok";
    if(!code(getFactorOffset,getFactorFingerprint))r="code";
    else if(!code(updateSiteOffset,updateSiteFingerprint)||!code(splitReaderOffset,splitReaderFingerprint)||!code(splitTestOffset,splitTestFingerprint))r="sites";
    else if(!got(getFactorGotOffset,getFactorOffset))r="got";
    if(r[0]!='o'){fingerprint.store(r,std::memory_order_relaxed);return false;}
    updateReturn=base+updateReturnOffset;originalGetFactor=reinterpret_cast<GetFactor>(base+getFactorOffset);
    const bool ok=replace(base+getFactorGotOffset,base+getFactorOffset,reinterpret_cast<std::uintptr_t>(&getFactorHook));
    fingerprint.store(ok?"ok":"replace",std::memory_order_relaxed);installed.store(ok,std::memory_order_relaxed);
    return ok;
}

struct Runtime {std::mutex mutex;std::string percent,pin;ResolvedRule resolved;std::uint64_t reads{};std::uint64_t lastReadNs{};};
inline Runtime runtimeState;
inline void applyProperties(const std::string& percent,const std::string& pin) {
    const ResolvedRule r=resolve(percent,pin);scale.store(scaleOf(r.rule),std::memory_order_relaxed);
    std::lock_guard lock(runtimeState.mutex);runtimeState.percent=percent;runtimeState.pin=pin;runtimeState.resolved=r;++runtimeState.reads;
}
// Render thread (TraceEntry observeScene): returns true when a read is due (first scene, then once per second).
inline bool readDue(std::uint64_t nowNs) {
    std::lock_guard lock(runtimeState.mutex);
    if(runtimeState.lastReadNs&&nowNs-runtimeState.lastReadNs<1'000'000'000ull)return false;
    runtimeState.lastReadNs=nowNs;return true;
}
#if defined(__ANDROID__)
inline std::string propertyText(const char* name) {char v[PROP_VALUE_MAX]{};const int n=__system_property_get(name,v);return n>0?std::string(v,std::size_t(n)):std::string();}
#endif
inline void sceneBegin() {
    if(!installed.load(std::memory_order_relaxed))return;
    const auto now=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    if(!readDue(now))return;
#if defined(__ANDROID__)
    const int saved=errno;applyProperties(propertyText("debug.rdrvr.lod.terrain"),propertyText("debug.rdrvr.lod.terrain.pin"));errno=saved;
#endif
}
inline std::string report() {
    std::ostringstream o;o.setf(std::ios::fixed);o.precision(4);
    o<<"\"terrainLod\":{\"installed\":"<<(installed.load()?1:0)<<",\"fingerprint\":\""<<fingerprint.load()<<'"'<<",\"scale\":"<<scale.load()
     <<",\"nativeFactor\":"<<lastNative.load()<<",\"appliedFactor\":"<<lastApplied.load()<<",\"calls\":"<<calls.load()
     <<",\"scaledCalls\":"<<scaledCalls.load()<<",\"foreignCalls\":"<<foreignCalls.load();
    std::lock_guard lock(runtimeState.mutex);const auto& s=runtimeState;
    o<<",\"rule\":{\"percent\":"<<s.resolved.rule.percent<<",\"pin\":"<<s.resolved.rule.pin<<",\"bad\":["<<(s.resolved.percentBad?1:0)<<','
     <<(s.resolved.pinBad?1:0)<<"],\"reads\":"<<s.reads<<"}}";
    return o.str();
}
} // namespace rdrvr::engine::terrainlod
