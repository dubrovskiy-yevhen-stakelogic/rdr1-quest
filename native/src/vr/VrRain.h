#pragma once
// RAIN QUALITY: what rain costs on the Quest 3S and two live levers, one CHEATS row.
//
// What rain adds per frame (librdr.so; [V] = read in the code):
//  1. GPU rain streaks [V]. CVfxWeather::Render 0x2980f64 -> CPtFxGPU::Render 0x2982468 -> ptxGPUParticle::Render 0x2985a68 draws ONE
//     indexed draw of N alpha-blended camera-facing quads (SetQuadDrawIndices(6N)), N = int(float(count) * emission/100):
//       count    = ptxGPUParticle+0xb8 = 16384 (CPtFxGPU::Init 0x2981b60: Init(0x4000), a 128x128 particle texture), constant after Init
//       emission = CPtFxGPU+0x5d0 = rain x 100 (RAINY 0.5 -> 8192 quads, STORMY 1.0 -> 16384 quads)
//     CPtFxGPU = g_vfxWeather+0x730, ptxGPUParticle = CPtFxGPU+0x730 = g_vfxWeather+0xe60 (CPtFxGPU::PreRender uses x21+0xe60 with
//     x21 = g_vfxWeather). With TAIL IN EYES the draw is inside the left colour stage AND in the colour tape the right eye
//     replays: 2 x N quads of full-eye-resolution alpha blend per frame. The simulation (CPtFxGPU::PreRender -> ptxGPUParticle::Update,
//     one 128x128 pass, 16 only after a >20 m camera jump) runs once per frame, before the stages, not replayed.
//  2. Wet world [V]. CVfxWeather::PreRender 0x2980f10 copies the wetness into renLightPrePassLightsMgr::sm_Instance+0x9c54 (the ONLY
//     writer). Four render-thread gates read it (> 0 / != 0):
//       RenderScene_RenderThread+0x340  DrawCheapEnvironmentMap(false) a SECOND time (extra environment map pass, pre-stage)
//       sagWorld::DrawStart+0x2f0       wet technique callbacks (BeginWetPass/EndWetPass) around the bucket draw (colour stage)
//       PostWaterDrawCallback+0x144     terrain drawn AGAIN to 70 m with the forced wet technique + clip plane (colour stage)
//       PostAlphaMaskDrawCallback+0x104 SetPuddleGlobals (puddle shader globals)
//     The wetness stays > 0 for ~90 s after the rain stops (CVfxWeather::Update: down at dt/90).
// Levers (render thread only, everything restored, nothing persistent in the engine):
//  - DROPS: around the early TAIL IN EYES weather call (PostFxSkip.h postfx::tail::early::run) the constant count 16384 is scaled
//    and put back right after the call -> the engine's own thinning (it already drops the tail of the list for lower intensity),
//    both eyes identical (the right eye replays the recorded index count).
//  - DRY: GOT hook of CVfxWeather::PreRender (slot 0x2e5d838, the only caller RenderScene+0x2d0 / ret 0x2a533c4): after the
//    original, the lights-manager copy of the wetness is zeroed for this frame -> all four gates stay closed (no wet look, no extra
//    passes). Rain intensity, audio, footsteps and CVfxWeather's own wetness are untouched; the next frame's PreRender rewrites it.
// Row: CHEATS -> RAIN QUALITY  FULL / HALF / LOW / DRY / LOW DRY / NO DROPS / OFF (stick or A). Not saved (session only).
// Property debug.rdrvr.rain <0..6> beats the row (read every 64 frames); debug.rdrvr.rainhook 0 at startup = DRY hook not installed.
// Report vr.rain: levels, live drop counts (native vs drawn), wetness, per-level render-thread frame interval while it rains.
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#include <time.h>
#endif

namespace rdrvr::vr::rain {

// ---- engine facts (vaddr; .text file offset = vaddr - 0x4000) ----
inline constexpr std::uintptr_t vfxWeatherGot=0x2e014f0,vfxWeatherObject=0x4e829580;      // &g_vfxWeather (GLOB_DAT)
inline constexpr std::uintptr_t lightsMgrGot=0x2e0a860,lightsMgrVar=0x4e85e728;           // &renLightPrePassLightsMgr::sm_Instance
inline constexpr std::uintptr_t preRenderOffset=0x2980f10,preRenderGot=0x2e5d838,preRenderReturn=0x2a533c4; // CVfxWeather::PreRender
inline constexpr std::uintptr_t vfxRenderOffset=0x2980f64,cptfxInitCountOffset=0x2981b54,cptfxRenderCountOffset=0x2982984,
    ptxRenderHeadOffset=0x2985a98,ptxRenderCountOffset=0x2985d74,ptxInitCountOffset=0x29850c4,preRenderCallOffset=0x2a533c0;
inline constexpr std::uintptr_t rainField=0x150c;      // float, GPU rain intensity (0.5 RAINY / 1.0 STORMY)
inline constexpr std::uintptr_t wetnessField=0x151c;   // float, 0..3 (GetCurrWetnessDueToRain = /3)
inline constexpr std::uintptr_t emissionField=0xd00;   // float, CPtFxGPU(+0x730)+0x5d0 = rain x 100
inline constexpr std::uintptr_t countField=0xf18;      // int, ptxGPUParticle(+0xe60)+0xb8 = 16384
inline constexpr std::uintptr_t wetGlobalField=0x9c54; // float, *sm_Instance + 0x9c54
inline constexpr int nativeCount=16384;
// CVfxWeather::PreRender (whole function): x8=*GOT 0x2e01610 (sagEnvironment), s0=[x0+0x151c], s1=[x0+0x1554], x8=*GOT 0x2e0a860,
// stores s0 -> +0x9c58, s1 -> +0x9c54, [x0+0x1518] -> +0x9c60; [x0+0x150c] != 0 -> b CPtFxGPU::PreRender.
inline constexpr std::array<unsigned char,84> preRenderFingerprint{
#include "gamecode/VrRain_2980f10_84_8d.inc"
};
// CVfxWeather::Render (whole): [x0+0x150c] == 0 -> ret; x0 += 0x730; b CPtFxGPU::Render.
inline constexpr std::array<unsigned char,24> vfxRenderFingerprint{
#include "gamecode/VrRain_2980f64_24_8d.inc"
};
// CPtFxGPU::Init 0x2981b54: add x0,x19,#0x730 ... mov w1,#0x4000 ... bl ptxGPUParticle::Init.
inline constexpr std::array<unsigned char,28> cptfxInitCountFingerprint{
#include "gamecode/VrRain_2981b54_28_8d.inc"
};
// CPtFxGPU::Render 0x2982984: s8 = [x19+0x5d0]/100 ... x0 = x19+0x730 ... bl ptxGPUParticle::Render(.., s0 = s8, paused).
inline constexpr std::array<unsigned char,56> cptfxRenderCountFingerprint{
#include "gamecode/VrRain_2982984_56_8d.inc"
};
// ptxGPUParticle::Render 0x2985a98: ldr w8,[x0,#0x1c8]; cbnz -> no draw.
inline constexpr std::array<unsigned char,8> ptxRenderHeadFingerprint{8,200,65,185,40,31,0,53};
// ptxGPUParticle::Render 0x2985d74: n = fcvtzs(scvtf([x19+0xb8]) * s8); n < 1 -> no draw; SetQuadDrawIndices(6n); DrawIndexedPrimitive(3,0,6n).
inline constexpr std::array<unsigned char,52> ptxRenderCountFingerprint{
#include "gamecode/VrRain_2985d74_52_8d.inc"
};
// ptxGPUParticle::Init 0x29850c4: stp w1,#128 -> +0xb8/+0xbc, +0xc0 = count/128.
inline constexpr std::array<unsigned char,28> ptxInitCountFingerprint{
#include "gamecode/VrRain_29850c4_28_8d.inc"
};
// RenderScene_RenderThread+0x2d0: bl CVfxWeather::PreRender@plt (the only caller).
inline constexpr std::array<unsigned char,4> preRenderCallFingerprint{252,91,9,148};

// ---- levels ----
enum Level : unsigned {Full,Half,Low,Dry,LowDry,NoDrops,Off,LevelCount};
inline constexpr std::array<const char*,LevelCount> levelNames{"FULL","HALF","LOW","DRY","LOW DRY","NO DROPS","OFF"};
inline constexpr std::array<const char*,LevelCount> levelKeys{"FULL","HALF","LOW","DRY","LOW_DRY","NO_DROPS","OFF"};
inline constexpr std::array<unsigned,LevelCount> dropsPermille{1000,500,250,1000,250,0,0};
inline constexpr std::array<bool,LevelCount> dryLevel{false,false,false,true,true,false,true};
inline constexpr int scaledCount(unsigned level) noexcept {return level<LevelCount?int(std::int64_t(nativeCount)*dropsPermille[level]/1000):nativeCount;}
// The engine's own arithmetic (float(count) * (emission/100), truncated).
inline int quadsFor(int count,float emission) noexcept {
    if(!(emission>0.f)||count<=0)return 0;
    const float n=float(count)*(emission/100.f);
    return n>=1.f&&n<2.e9f?int(n):0;
}

// ---- state ----
inline std::atomic<unsigned> menuLevel{Full};          // CHEATS row (XR thread writes)
inline std::atomic<int> property{-1};                  // debug.rdrvr.rain 0..6, -1 absent
inline std::atomic<bool> dropsReady{false},wetReady{false};
inline std::atomic<const char*> installState{"NOT INSTALLED"};
inline std::uintptr_t vfx{};                           // &g_vfxWeather (set once at install, before the hooks go live)
inline std::uintptr_t lightsMgrPtr{};                  // &sm_Instance (holds the manager pointer)
inline std::uintptr_t engineBase{};
using PreRenderFn=void(*)(void*);
inline PreRenderFn preRenderOriginal{};
inline unsigned level() noexcept {
    const int p=property.load(std::memory_order_relaxed);
    if(p>=0&&p<int(LevelCount))return unsigned(p);
    const unsigned m=menuLevel.load(std::memory_order_relaxed);return m<LevelCount?m:unsigned(Full);
}

struct Stats {
    std::atomic<std::uint64_t> preRenderCalls{},preRenderForeign{},propertyReads{};
    std::atomic<std::uint64_t> renders{},noRain{},scaled{},countMismatch{},sumNative{},sumDrawn{},cpuNs{},cpuSamples{};
    std::atomic<int> lastNative{},lastDrawn{},lastCount{};
    std::atomic<std::uint64_t> wetZeroed{},wetFrames{};
    std::atomic<float> lastRain{},lastEmission{},lastWetness{},lastWetGlobal{};
    struct PerLevel {std::atomic<std::uint64_t> frames{},rainFrames{},intervalNs{},intervals{};};
    std::array<PerLevel,LevelCount> byLevel{};
};
inline Stats stats;

template<class T> inline T peek(std::uintptr_t a) noexcept {T v{};std::memcpy(&v,reinterpret_cast<const void*>(a),sizeof v);return v;}
template<class T> inline void poke(std::uintptr_t a,T v) noexcept {std::memcpy(reinterpret_cast<void*>(a),&v,sizeof v);}
inline std::uint64_t threadCpuNs() noexcept {
#if defined(__ANDROID__)
    timespec t{};if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t))return 0;
    return std::uint64_t(t.tv_sec)*1000000000ull+std::uint64_t(t.tv_nsec);
#else
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
inline std::uint64_t steadyNs() noexcept {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
inline int readIntProperty(const char* name) noexcept {
#if defined(__ANDROID__)
    const int saved=errno;char v[PROP_VALUE_MAX]{};const int n=__system_property_get(name,v);errno=saved;
    if(n<=0)return -1;
    int r=0;for(int i=0;i<n;++i){if(v[i]<'0'||v[i]>'9')return -1;r=r*10+(v[i]-'0');if(r>1000)return -1;}
    return r;
#else
    (void)name;return -1;
#endif
}

// ---- DROPS: RAII around the early TAIL IN EYES weather call (render thread) ----
// Scales the constant particle count for exactly this call and puts it back. Counts native vs drawn quads (per eye; the right eye
// replays the same draw).
class DropScope {
    std::uintptr_t countAddress_{};int saved_{};bool restore_{},timed_{};std::uint64_t cpu0_{};
public:
    DropScope() noexcept {
        if(!dropsReady.load(std::memory_order_acquire)||!vfx)return;
        const float rain=peek<float>(vfx+rainField);stats.lastRain.store(rain,std::memory_order_relaxed);
        if(rain==0.f){stats.noRain.fetch_add(1,std::memory_order_relaxed);return;} // CVfxWeather::Render returns at once
        const float emission=peek<float>(vfx+emissionField);stats.lastEmission.store(emission,std::memory_order_relaxed);
        countAddress_=vfx+countField;saved_=peek<int>(countAddress_);stats.lastCount.store(saved_,std::memory_order_relaxed);
        const unsigned lv=level();const int native=quadsFor(saved_,emission);
        int drawnCount=saved_;
        if(saved_!=nativeCount)stats.countMismatch.fetch_add(1,std::memory_order_relaxed); // never scale an unexpected value
        else if(dropsPermille[lv]!=1000){drawnCount=scaledCount(lv);poke<int>(countAddress_,drawnCount);restore_=true;stats.scaled.fetch_add(1,std::memory_order_relaxed);}
        const int drawn=quadsFor(drawnCount,emission);
        stats.renders.fetch_add(1,std::memory_order_relaxed);
        stats.lastNative.store(native,std::memory_order_relaxed);stats.lastDrawn.store(drawn,std::memory_order_relaxed);
        stats.sumNative.fetch_add(std::uint64_t(native),std::memory_order_relaxed);stats.sumDrawn.fetch_add(std::uint64_t(drawn),std::memory_order_relaxed);
        timed_=true;cpu0_=threadCpuNs();
    }
    ~DropScope(){
        if(restore_)poke<int>(countAddress_,saved_);
        if(timed_){const auto t=threadCpuNs();if(t>cpu0_){stats.cpuNs.fetch_add(t-cpu0_,std::memory_order_relaxed);stats.cpuSamples.fetch_add(1,std::memory_order_relaxed);}}
    }
    DropScope(const DropScope&)=delete;DropScope& operator=(const DropScope&)=delete;
};

// ---- DRY: CVfxWeather::PreRender GOT hook (render thread, once per RenderScene, before every stage) ----
inline std::uint64_t lastPreRenderNs{};inline unsigned lastPreRenderLevel{LevelCount}; // render thread only
inline void preRenderHook(void* self) noexcept {
    preRenderOriginal(self);
    const auto calls=stats.preRenderCalls.fetch_add(1,std::memory_order_relaxed);
    if(calls%64==0){property.store(readIntProperty("debug.rdrvr.rain"),std::memory_order_relaxed);stats.propertyReads.fetch_add(1,std::memory_order_relaxed);}
#if defined(__clang__)||defined(__GNUC__)
    if(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))!=engineBase+preRenderReturn){stats.preRenderForeign.fetch_add(1,std::memory_order_relaxed);return;}
#endif
    const unsigned lv=level();
    const float rain=peek<float>(vfx+rainField),wet=peek<float>(vfx+wetnessField);
    stats.lastRain.store(rain,std::memory_order_relaxed);stats.lastWetness.store(wet/3.f,std::memory_order_relaxed);
    const std::uintptr_t mgr=peek<std::uintptr_t>(lightsMgrPtr);
    float global=0.f;
    if(mgr){global=peek<float>(mgr+wetGlobalField);stats.lastWetGlobal.store(global,std::memory_order_relaxed);}
    if(global!=0.f)stats.wetFrames.fetch_add(1,std::memory_order_relaxed);
    if(dryLevel[lv]&&mgr&&global!=0.f){poke<float>(mgr+wetGlobalField,0.f);stats.wetZeroed.fetch_add(1,std::memory_order_relaxed);}
    // Render-thread frame interval per level (the RenderScene period, pacing included) while it rains or the world is wet.
    auto& b=stats.byLevel[lv];b.frames.fetch_add(1,std::memory_order_relaxed);
    const auto now=steadyNs();
    const bool weather=rain!=0.f||global!=0.f;
    if(weather)b.rainFrames.fetch_add(1,std::memory_order_relaxed);
    if(weather&&lastPreRenderNs&&lastPreRenderLevel==lv&&now>lastPreRenderNs){
        const auto dt=now-lastPreRenderNs;
        if(dt>=4'000'000&&dt<=250'000'000){b.intervalNs.fetch_add(dt,std::memory_order_relaxed);b.intervals.fetch_add(1,std::memory_order_relaxed);}}
    lastPreRenderNs=now;lastPreRenderLevel=lv;
}

// ---- install (TraceEntry, once). code(offset, print) = executable bytes match; got(slot, target) = *(base+slot) == base+target;
// replace(address, expected, new) = GOT word swap. DROPS needs no hook (PostFxSkip early::run uses DropScope). ----
template<class Code,class Got,class Replace>
inline bool install(std::uintptr_t base,Code&& code,Got&& got,Replace&& replace) noexcept {
    engineBase=base;
    bool vfxOk=false,mgrOk=false;
    try{vfxOk=got(vfxWeatherGot,vfxWeatherObject);mgrOk=got(lightsMgrGot,lightsMgrVar);}catch(...){}
    if(!vfxOk){installState.store("GOT g_vfxWeather");return false;}
    vfx=base+vfxWeatherObject;
    bool drops=false;
    try{drops=code(vfxRenderOffset,vfxRenderFingerprint)&&code(cptfxInitCountOffset,cptfxInitCountFingerprint)&&
        code(cptfxRenderCountOffset,cptfxRenderCountFingerprint)&&code(ptxRenderHeadOffset,ptxRenderHeadFingerprint)&&
        code(ptxRenderCountOffset,ptxRenderCountFingerprint)&&code(ptxInitCountOffset,ptxInitCountFingerprint);}catch(...){drops=false;}
    dropsReady.store(drops,std::memory_order_release);
    const char* state=drops?"DROPS ONLY":"PRINT DROPS";
    if(!mgrOk)state=drops?"DROPS ONLY - GOT LIGHTSMGR":"GOT LIGHTSMGR";
    else if(readIntProperty("debug.rdrvr.rainhook")==0)state=drops?"DROPS ONLY - HOOK OFF PROP":"HOOK OFF PROP";
    else{
        bool prints=false;
        try{prints=code(preRenderOffset,preRenderFingerprint)&&code(preRenderCallOffset,preRenderCallFingerprint)&&got(preRenderGot,preRenderOffset);}catch(...){}
        if(!prints)state=drops?"DROPS ONLY - PRINT PRERENDER":"PRINT PRERENDER";
        else{
            lightsMgrPtr=base+lightsMgrVar;preRenderOriginal=reinterpret_cast<PreRenderFn>(base+preRenderOffset);
            bool hooked=false;
            try{hooked=replace(base+preRenderGot,base+preRenderOffset,reinterpret_cast<std::uintptr_t>(&preRenderHook));}catch(...){}
            if(hooked){wetReady.store(true,std::memory_order_release);state=drops?"INSTALLED":"DRY ONLY - PRINT DROPS";}
            else state=drops?"DROPS ONLY - HOOK FAILED":"HOOK FAILED";
        }
    }
    installState.store(state);
    return drops||wetReady.load();
}

// ---- CHEATS row "RAIN QUALITY" (XR thread; VrMenuPages.h CheatRow callbacks) ----
inline void rowValue(void*,char* out,std::size_t size) noexcept {
    if(!size)return;
    const unsigned lv=level();const bool prop=property.load(std::memory_order_relaxed)>=0;
    const bool drops=dropsReady.load(std::memory_order_relaxed),wet=wetReady.load(std::memory_order_relaxed);
    if(!drops&&!wet){std::snprintf(out,size,"%s - UNAVAILABLE",levelNames[lv]);return;}
    const float rain=stats.lastRain.load(std::memory_order_relaxed);
    if(prop)std::snprintf(out,size,"%s - PROP",levelNames[lv]);
    else if(dryLevel[lv]&&!wet)std::snprintf(out,size,"%s - NO DRY HOOK",levelNames[lv]);
    else if(dropsPermille[lv]!=1000&&!drops)std::snprintf(out,size,"%s - NO DROP LEVER",levelNames[lv]);
    else if(rain==0.f)std::snprintf(out,size,"%s - NO RAIN",levelNames[lv]);
    else std::snprintf(out,size,"%s  %d DROPS",levelNames[lv],stats.lastDrawn.load(std::memory_order_relaxed));
}
inline void rowAdjust(void*,int direction) noexcept {
    const unsigned m=menuLevel.load(std::memory_order_relaxed);
    menuLevel.store((m+(direction<0?LevelCount-1:1u))%LevelCount,std::memory_order_relaxed);
}
inline void rowActivate(void* ctx) noexcept {rowAdjust(ctx,1);}

// ---- report vr.rain ----
inline std::string report() {
    std::string out;out.reserve(1536);char b[512];
    const unsigned lv=level();
    const auto renders=stats.renders.load(),cpuN=stats.cpuSamples.load();
    std::snprintf(b,sizeof b,"{\"install\":\"%s\",\"dropsReady\":%s,\"dryReady\":%s,\"level\":%u,\"levelName\":\"%s\",\"menuLevel\":%u,\"property\":%d,"
        "\"rain\":%.3f,\"emission\":%.1f,\"wetness\":%.3f,\"wetGlobal\":%.3f,\"nativeCount\":%d,\"count\":%d,",
        installState.load(),dropsReady.load()?"true":"false",wetReady.load()?"true":"false",lv,levelNames[lv],menuLevel.load(),property.load(),
        double(stats.lastRain.load()),double(stats.lastEmission.load()),double(stats.lastWetness.load()),double(stats.lastWetGlobal.load()),nativeCount,stats.lastCount.load());
    out+=b;
    std::snprintf(b,sizeof b,"\"drops\":{\"renders\":%llu,\"noRain\":%llu,\"scaled\":%llu,\"countMismatch\":%llu,\"lastNativeQuads\":%d,\"lastDrawnQuads\":%d,"
        "\"meanNativeQuads\":%.0f,\"meanDrawnQuads\":%.0f,\"cpuUsMean\":%.1f,\"drawsPerEye\":1,\"eyes\":\"left native + right replay of the same draw\"},",
        (unsigned long long)renders,(unsigned long long)stats.noRain.load(),(unsigned long long)stats.scaled.load(),(unsigned long long)stats.countMismatch.load(),
        stats.lastNative.load(),stats.lastDrawn.load(),renders?double(stats.sumNative.load())/double(renders):0.0,renders?double(stats.sumDrawn.load())/double(renders):0.0,
        cpuN?double(stats.cpuNs.load())/double(cpuN)/1000.0:0.0);
    out+=b;
    std::snprintf(b,sizeof b,"\"dry\":{\"preRenderCalls\":%llu,\"foreign\":%llu,\"wetFrames\":%llu,\"zeroedFrames\":%llu,\"propertyReads\":%llu},\"byLevel\":{",
        (unsigned long long)stats.preRenderCalls.load(),(unsigned long long)stats.preRenderForeign.load(),(unsigned long long)stats.wetFrames.load(),
        (unsigned long long)stats.wetZeroed.load(),(unsigned long long)stats.propertyReads.load());
    out+=b;
    for(unsigned i=0;i<LevelCount;++i){
        const auto& l=stats.byLevel[i];const auto n=l.intervals.load();
        std::snprintf(b,sizeof b,"%s\"%s\":{\"frames\":%llu,\"rainFrames\":%llu,\"intervals\":%llu,\"frameMs\":%.2f,\"fps\":%.1f}",i?",":"",levelKeys[i],
            (unsigned long long)l.frames.load(),(unsigned long long)l.rainFrames.load(),(unsigned long long)n,
            n?double(l.intervalNs.load())/double(n)/1e6:0.0,n&&l.intervalNs.load()?1e9*double(n)/double(l.intervalNs.load()):0.0);
        out+=b;
    }
    out+="}}";
    return out;
}

// Host tests only.
inline void configureForTest(std::uintptr_t fakeVfx,std::uintptr_t fakeMgrPtr,bool drops,bool dry) noexcept {
    vfx=fakeVfx;lightsMgrPtr=fakeMgrPtr;dropsReady.store(drops);wetReady.store(dry);property.store(-1);menuLevel.store(Full);}

} // namespace rdrvr::vr::rain
