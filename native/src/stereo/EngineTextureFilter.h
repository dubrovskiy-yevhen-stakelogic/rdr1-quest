#pragma once
// GRAPHICS rows TEXTURE FILTER (anisotropy cap NATIVE/8X/4X/2X/OFF) and MIP BIAS (0/+0.25/+0.5),
// STEREO TEST 18/19 alternating filter/bias windows, and an observe-only hook on the engine's triangle-budget LOD. librdr 1.58 arm64,
// module-relative addresses; each one checked with llvm-readelf -r / llvm-objdump.
//
// Mechanism (the engine's own option path):
//   d11::GfxDevice_c::ms_Instance (bss 0x4e292f60, GLOB_DAT 0x2ddcaf8) points to the d11::VkDevice_c. Its fields
//   +0xb8 int aniso request, +0xbc int aniso cap, +0xc0 float global mip bias, +0xc7 u8 dirty.
//   sagRenderer::PerformDMZ (0x2a5548c, main thread, only caller sagShellDerived::Execute bl 0x23a0304) tests the dirty
//   byte at 0x2a555f4 and calls grcEffect::RecreateSamplerStates (0x1ac4a10, bl 0x2a555fc) when it is set (and
//   sagRenderer+0x3c==0, nomtrendering -> +0x6fb!=0, device +0x60|+0x68 != 0). RecreateSamplerStates walks the 55
//   g_D11SamplerStates: SetAnisotropy(+0xb8,+0xbc) -> level=min(max(request,own +0x5c),cap) for min/mag filter 3 only;
//   SetMipBias(+0xc0) -> +0x4c=own +0x58 + bias, skipped for bindless index 1 and 38; Recreate when either changed;
//   then clears +0xc7. Cap 0 = anisotropyEnable 0 = plain trilinear (GetOrCreateSampler 0x18b9038).
// dmzHook (PerformDMZ JUMP_SLOT 0x2e499a8) writes cap/bias and the dirty byte immediately before the engine's own check,
// only when a field differs from the wanted value. NATIVE/0 never writes until the row was changed once; after that it
// writes the values captured at the first call back. recreateHook (JUMP_SLOT 0x2e21590) only times the original and
// decodes the 55 samplers afterwards (confirmed cap/bias for the byState signature _af<cap>_mb<centi>).
// Samplers are created clamped to +0xbc (CreateD11SamplerState 0x1ac5290, called from grcParameter::Load), so a cap in
// force while effects load would be permanent: live mode therefore writes nothing before the first world scene.
// debug.rdrvr.texfilter.live=0 = boot-only: the wanted values are applied before the first world scene and then frozen.
// debug.rdrvr.engineknobs=0 installs nothing of this file nor MeminfoCache.h.
// engineLod: grcDevice::BeginFrame (0x1aba2f0, JUMP_SLOT 0x2e21320, only caller grcSetup::BeginDraw bl 0x1aed474) turns
// sm_UnmodifiedTriCount (u32, previous frame, native lists only) into sm_LodCostModifier: <=4.5M -> bounds[0] (2.0),
// >=6.0M -> bounds[1] (10.0), linear between; sm_EnableLodCostModifier==0 -> 2.0. The hook reads the count before the
// original (which zeroes it) and the modifier after it. Observe only.
#include "MeminfoCache.h"
#include "StereoTestMode.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

namespace rdrvr::engine::enginetex {
// ---- librdr 1.58 arm64 offsets (module-relative) ----
inline constexpr std::uintptr_t gfxDeviceGotOffset=0x2ddcaf8,gfxDeviceOffset=0x4e292f60; // GLOB_DAT -> bss pointer variable
inline constexpr std::uintptr_t requestField=0xb8,capField=0xbc,biasField=0xc0,dirtyField=0xc7;
inline constexpr std::uintptr_t performDmzOffset=0x2a5548c,performDmzGotOffset=0x2e499a8,performDmzReturnOffset=0x23a0308;
inline constexpr std::array<unsigned char,32> performDmzFingerprint{
#include "gamecode/EngineTextureFilter_189c0e0_32_8d.inc"
};
inline constexpr std::uintptr_t dirtySiteOffset=0x2a555dc; // GfxDevice load, +0x60|+0x68, ldrb +0xc7, cbz, bl RecreateSamplerStates@plt
inline constexpr std::array<unsigned char,36> dirtySiteFingerprint{
#include "gamecode/EngineTextureFilter_2a555dc_36_8d.inc"
};
inline constexpr std::uintptr_t shellDmzCallOffset=0x23a0304; // sagShellDerived::Execute: bl PerformDMZ@plt
inline constexpr std::array<unsigned char,4> shellDmzCallFingerprint{227,136,35,148};
inline constexpr std::uintptr_t recreateOffset=0x1ac4a10,recreateGotOffset=0x2e21590,recreateReturnOffset=0x2a55600;
inline constexpr std::array<unsigned char,32> recreateFingerprint{
#include "gamecode/EngineTextureFilter_1ac4a10_32_8d.inc"
};
inline constexpr std::uintptr_t recreateAnisoSiteOffset=0x1ac4a5c;   // ldp w1,w2,[x8,#0xb8]; ldr x8,[x9,#0x28]
inline constexpr std::array<unsigned char,16> recreateAnisoSiteFingerprint{
#include "gamecode/EngineTextureFilter_1ac4a5c_16_8d.inc"
};
inline constexpr std::uintptr_t recreateExcludeSiteOffset=0x1ac4a88; // cmp w0,#1 / cmp w0,#0x26 (bias exclusions)
inline constexpr std::array<unsigned char,16> recreateExcludeSiteFingerprint{
#include "gamecode/EngineTextureFilter_1ac4a88_16_8d.inc"
};
inline constexpr std::uintptr_t recreateBiasSiteOffset=0x1ac4aa4;    // ldr s0,[x8,#0xc0]; ldr x8,[x9,#0x30]
inline constexpr std::array<unsigned char,16> recreateBiasSiteFingerprint{
#include "gamecode/EngineTextureFilter_1ac4aa4_16_8d.inc"
};
inline constexpr std::uintptr_t recreateClearSiteOffset=0x1ac4ac4;   // strb wzr,[x8,#0xc7]
inline constexpr std::array<unsigned char,8> recreateClearSiteFingerprint{136,2,64,249,31,29,3,57};
inline constexpr std::uintptr_t setAnisotropyOffset=0x18b96f4;       // d11::VkSamplerState_c::SetAnisotropy(unsigned,unsigned)
inline constexpr std::array<unsigned char,84> setAnisotropyFingerprint{
#include "gamecode/EngineTextureFilter_18b96f4_84_8d.inc"
};
inline constexpr std::uintptr_t setMipBiasOffset=0x18b9748;          // d11::VkSamplerState_c::SetMipBias(float)
inline constexpr std::array<unsigned char,32> setMipBiasFingerprint{
#include "gamecode/EngineTextureFilter_18b9748_32_8d.inc"
};
inline constexpr std::uintptr_t samplerRecreateOffset=0x18b9768;     // d11::VkSamplerState_c::Recreate() -> CreateInternal(+0x18)
inline constexpr std::array<unsigned char,8> samplerRecreateFingerprint{1,96,0,145,109,142,77,20};
inline constexpr std::uintptr_t getBindlessOffset=0x18b9778;         // d11::VkSamplerState_c::GetBindlessIndex(): ldr w0,[x0,#0x14]; ret
inline constexpr std::array<unsigned char,8> getBindlessFingerprint{0,20,64,185,192,3,95,214};
inline constexpr std::uintptr_t samplerVtableOffset=0x2cbf390;       // _ZTVN3d1116VkSamplerState_cE (0x2cbf380) + 0x10
inline constexpr std::array<std::uintptr_t,4> samplerVtableSlots{0x20,0x28,0x30,0x38};
inline constexpr std::array<std::uintptr_t,4> samplerVtableFunctions{getBindlessOffset,setAnisotropyOffset,setMipBiasOffset,samplerRecreateOffset};
inline constexpr std::uintptr_t d11SamplersGotOffset=0x2de0a10,d11SamplersOffset=0x4e6da0a0; // rage::g_D11SamplerStates[55] (pointers)
inline constexpr unsigned samplerCount=55;
inline constexpr std::uintptr_t packedSamplersGotOffset=0x2de0a40,packedSamplersOffset=0x4e6db610; // rage::g_SamplerStates, 55 x 20 B
inline constexpr std::size_t packedStride=20;
// VkSamplerState_c fields (desc at +0x18)
inline constexpr std::uintptr_t poolField=0x10,bindlessField=0x14,minFilterField=0x24,magFilterField=0x28,mipFilterField=0x2c,
    levelField=0x38,samplerBiasField=0x4c,ownBiasField=0x58,ownAnisoField=0x5c;
inline constexpr std::uint32_t anisotropicFilter=3;
inline constexpr std::array<std::uint32_t,2> biasExcludedBindless{1,38};
// LOD (grcDevice::BeginFrame)
inline constexpr std::uintptr_t beginFrameOffset=0x1aba2f0,beginFrameGotOffset=0x2e21320,beginFrameReturnOffset=0x1aed478;
inline constexpr std::array<unsigned char,32> beginFrameFingerprint{
#include "gamecode/EngineTextureFilter_1aba2f0_32_8d.inc"
};
inline constexpr std::uintptr_t beginFrameCallOffset=0x1aed474; // grcSetup::BeginDraw: bl grcDevice::BeginFrame@plt
inline constexpr std::array<unsigned char,4> beginFrameCallFingerprint{67,17,69,148};
inline constexpr std::uintptr_t lodBlockOffset=0x1aba3b4; // the LOD block through ret (0x1aba3b4..0x1aba458)
inline constexpr std::array<unsigned char,164> lodBlockFingerprint{
#include "gamecode/EngineTextureFilter_1aba3b4_164_8d.inc"
};
inline constexpr std::uintptr_t lodModifierOffset=0x2e736b0,lodModifierGotOffset=0x2de0930;   // float sm_LodCostModifier
inline constexpr std::uintptr_t lodBoundsOffset=0x2e736b4,lodBoundsGotOffset=0x2de0928;       // float[2] {2.0,10.0}
inline constexpr std::uintptr_t triCountOffset=0x2e736bc,triCountGotOffset=0x2de0918;         // u32 sm_UnmodifiedTriCount
inline constexpr std::uintptr_t triBoundsOffset=0x2e736c0,triBoundsGotOffset=0x2de0920;       // u32[2] {4500000,6000000}
inline constexpr std::uintptr_t lodEnableOffset=0x2e736ae,lodEnableGotOffset=0x2de0910;       // u8 sm_EnableLodCostModifier

// ---- levels ----
inline constexpr unsigned filterLevels=5,biasLevels=3;
inline constexpr const char* filterNames[filterLevels]{"NATIVE","8X","4X","2X","OFF"};
inline constexpr const char* biasNames[biasLevels]{"0","+0.25","+0.5"};
inline constexpr int filterCaps[filterLevels]{-1,8,4,2,0}; // -1 = native cap; OFF = 0 (trilinear, the other Adreno 740 profiles)
inline constexpr float biasDeltas[biasLevels]{0.f,.25f,.5f};
inline constexpr unsigned testModeFilterAB=stereo::testModeTextureFilterAB,testModeBiasAB=stereo::testModeMipBiasAB;
inline constexpr unsigned abFilterLevel=2,abBiasLevel=2; // window a: 4X / +0.5; window b: NATIVE / 0
inline constexpr const char* filterName(unsigned f) noexcept {return f<filterLevels?filterNames[f]:"?";}
inline constexpr const char* biasName(unsigned b) noexcept {return b<biasLevels?biasNames[b]:"?";}

// ---- pure core (host-testable) ----
struct Want {int cap;float bias;};
// Row values f/b; STEREO TEST 18/19 replace the filter/bias row by their window (phase 1 = a, else b). Never raises the cap.
constexpr Want want(unsigned f,unsigned b,unsigned mode,unsigned phase,int nativeCap,float nativeBias) noexcept {
    if(f>=filterLevels)f=0;
    if(b>=biasLevels)b=0;
    if(mode==testModeFilterAB)f=phase==1?abFilterLevel:0u;
    if(mode==testModeBiasAB)b=phase==1?abBiasLevel:0u;
    const int cap=f?(filterCaps[f]<nativeCap?filterCaps[f]:nativeCap):nativeCap;
    return {cap,nativeBias+biasDeltas[b]};
}
constexpr bool sameBits(float a,float b) noexcept {return std::bit_cast<std::uint32_t>(a)==std::bit_cast<std::uint32_t>(b);}
struct Plan {bool write;bool engineOverride;};
// write: a field differs from the want. engineOverride: a rewrite of a value that is not the last own write
// (another writer changed it since; this includes an engine write of the native value).
constexpr Plan plan(int curCap,float curBias,Want w,bool haveLast,int lastCap,float lastBias) noexcept {
    const bool write=curCap!=w.cap||!sameBits(curBias,w.bias);
    const bool foreign=haveLast&&(curCap!=lastCap||!sameBits(curBias,lastBias));
    return {write,write&&foreign};
}
constexpr bool plausible(int request,int cap,float bias) noexcept {
    return request>=0&&request<=16&&cap>=0&&cap<=16&&bias==bias&&bias>-4.f&&bias<4.f;
}
struct SamplerView {std::uint32_t bindless,minF,magF,mipF;std::int32_t level,ownAniso;float bias,ownBias;bool vtableOk;};
inline constexpr unsigned levelBuckets=7; // 0,1,2,4,8,16,other
inline constexpr const char* levelBucketNames[levelBuckets]{"0","1","2","4","8","16","other"};
constexpr unsigned levelBucket(int level) noexcept {return level==0?0:level==1?1:level==2?2:level==4?3:level==8?4:level==16?5:6;}
constexpr bool biasExcluded(std::uint32_t bindless) noexcept {return bindless==biasExcludedBindless[0]||bindless==biasExcludedBindless[1];}
struct Summary {
    unsigned samplers{},anisoSamplers{},foreignVtable{},biasSamplers{};
    std::array<unsigned,levelBuckets> levels{};
    int maxLevel{-1},minLevel{-1};
    float biasMin{NAN},biasMax{NAN}; // applied global bias = bias - ownBias, bindless 1/38 excluded (the engine skips them)
};
inline Summary decode(const SamplerView* v,unsigned n) noexcept {
    Summary s{};
    for(unsigned i=0;i<n;++i){
        const auto& x=v[i];
        if(!x.vtableOk){++s.foreignVtable;continue;}
        ++s.samplers;
        if(x.minF==anisotropicFilter||x.magF==anisotropicFilter){
            ++s.anisoSamplers;++s.levels[levelBucket(x.level)];
            s.maxLevel=s.maxLevel<0?x.level:std::max(s.maxLevel,x.level);
            s.minLevel=s.minLevel<0?x.level:std::min(s.minLevel,x.level);
        }
        if(!biasExcluded(x.bindless)){
            const float applied=x.bias-x.ownBias;
            if(!std::isfinite(applied))continue;
            s.biasMin=s.biasSamplers?std::min(s.biasMin,applied):applied;
            s.biasMax=s.biasSamplers?std::max(s.biasMax,applied):applied;
            ++s.biasSamplers;
        }
    }
    return s;
}
// LOD classification, exactly as grcDevice::BeginFrame: tri<=lo floor, tri>=hi ceiling, else between.
enum LodClass : unsigned {LodFloor,LodBetween,LodCeil,LodDisabled};
constexpr LodClass classifyLod(std::uint32_t tri,std::uint32_t lo,std::uint32_t hi,bool enabled) noexcept {
    return !enabled?LodDisabled:tri<=lo?LodFloor:tri>=hi?LodCeil:LodBetween;
}
inline constexpr unsigned triBuckets=33;            // 0.25 M triangles each, the last one is >= 8 M
inline constexpr std::uint32_t triBucketSize=250000;
constexpr unsigned triBucket(std::uint32_t tri) noexcept {return tri/triBucketSize<triBuckets-1?unsigned(tri/triBucketSize):triBuckets-1;}
// Upper edge (triangles) of the bucket holding the pct-th percentile, never above the observed max; 0 without samples.
inline std::uint64_t triPercentile(const std::uint64_t* hist,unsigned pct,std::uint64_t max) noexcept {
    std::uint64_t total{};for(unsigned i=0;i<triBuckets;++i)total+=hist[i];
    if(!total)return 0;
    const std::uint64_t need=(total*pct+99)/100;std::uint64_t seen{};
    for(unsigned i=0;i<triBuckets;++i){seen+=hist[i];if(seen>=need&&seen)return i==triBuckets-1?max:std::min<std::uint64_t>(std::uint64_t(i+1)*triBucketSize,max);}
    return max;
}

// ---- engine memory access (plain/atomic loads of engine-owned memory) ----
#if defined(_MSC_VER)&&!defined(__clang__)
inline std::uint32_t loadU32(const void* p) noexcept {return *static_cast<const volatile std::uint32_t*>(p);}
inline void storeU32(void* p,std::uint32_t v) noexcept {*static_cast<volatile std::uint32_t*>(p)=v;}
inline unsigned char loadU8(const void* p) noexcept {return *static_cast<const volatile unsigned char*>(p);}
inline void storeU8Release(void* p,unsigned char v) noexcept {std::atomic_thread_fence(std::memory_order_release);*static_cast<volatile unsigned char*>(p)=v;}
#else
inline std::uint32_t loadU32(const void* p) noexcept {return __atomic_load_n(static_cast<const std::uint32_t*>(p),__ATOMIC_RELAXED);}
inline void storeU32(void* p,std::uint32_t v) noexcept {__atomic_store_n(static_cast<std::uint32_t*>(p),v,__ATOMIC_RELAXED);}
inline unsigned char loadU8(const void* p) noexcept {return __atomic_load_n(static_cast<const unsigned char*>(p),__ATOMIC_RELAXED);}
inline void storeU8Release(void* p,unsigned char v) noexcept {__atomic_store_n(static_cast<unsigned char*>(p),v,__ATOMIC_RELEASE);}
#endif
inline std::int32_t loadI32(const void* p) noexcept {return std::int32_t(loadU32(p));}
inline float loadF32(const void* p) noexcept {return std::bit_cast<float>(loadU32(p));}
inline std::uintptr_t loadPointer(std::uintptr_t address) noexcept {std::uintptr_t v{};if(address)std::memcpy(&v,reinterpret_cast<const void*>(address),sizeof v);return v;}

// ---- runtime state ----
struct Config {
    std::uintptr_t deviceVar{},samplers{},packed{},samplerVtable{},dmzReturn{},recreateReturn{},beginFrameReturn{};
    std::uintptr_t lodModifier{},lodBounds{},triCount{},triBounds{},lodEnable{};
    bool knobs{},live{true},lod{};
};
inline Config config; // written once while the hooks are installed (before the slots go live)
using Dmz=void(*)(void*);
using Recreate=void(*)();
using BeginFrame=void(*)();
inline Dmz originalDmz{};
inline Recreate originalRecreate{};
inline BeginFrame originalBeginFrame{};

struct Inputs { // render thread (publish) -> main thread (apply)
    std::atomic<unsigned> filterLevel{},biasLevel{},testMode{},phase{},phaseOwn{};
    std::atomic<std::uint64_t> scenes{};
};
inline Inputs inputs;
struct State {
    std::atomic<bool> hooked{},live{true},captured{},disabled{},biasSuspended{},suspended{};
    std::atomic<const char*> declineReason{nullptr};
    std::atomic<int> nativeRequest{-1},nativeCap{-1},wantCap{-1};
    std::atomic<float> nativeBias{0.f},wantBias{0.f};
    std::atomic<std::uint64_t> dmzCalls{},foreignDmz{},applies{},writes{},deferredBoot{},frozenBoot{},recreates{},foreignRecreates{};
    std::atomic<std::uint64_t> recreateNsLast{},recreateNsMax{},dirtyStuck{},dirtyAfterRecreate{},engineOverrides{},engineNativeWrites{},declined{};
    std::atomic<int> confirmedCap{-1},confirmedBiasCenti{INT_MIN};
    // summary of the last decode (after a recreate)
    std::atomic<unsigned> decodes{},samplers{},nullSamplers{},anisoSamplers{},foreignVtable{},biasSamplers{};
    std::array<std::atomic<unsigned>,levelBuckets> levels{};
    std::atomic<int> maxLevel{-1},minLevel{-1};
    std::atomic<float> biasMin{NAN},biasMax{NAN};
    std::array<std::atomic<int>,samplerCount> appliedLevels{}; // -1 null entry, -2 foreign vtable
};
inline State state;
struct Lod {
    std::atomic<bool> hooked{};
    std::atomic<std::uint64_t> frames{},foreignCalls{},modSumMilli{},atFloor{},atCeil{},between{},disabled{},triSum{};
    std::atomic<float> modLast{NAN},boundLo{NAN},boundHi{NAN};
    std::atomic<std::uint32_t> triLast{},triMax{},triLo{},triHi{};
    std::atomic<bool> enabled{};
    std::array<std::atomic<std::uint64_t>,triBuckets> triHist{};
};
inline Lod lod;
// Main-thread-only bookkeeping of apply() (PerformDMZ runs on one thread).
struct Owner {
    bool haveLast{};int lastCap{};float lastBias{};
    std::uint64_t fightWindowNs{},fightWindowBase{};
};
inline Owner owner;
inline std::atomic<bool> baselineReady{};
inline std::string baselineDump; // written once on the main thread before baselineReady (release); read-only afterwards

inline unsigned char* device() noexcept {return reinterpret_cast<unsigned char*>(loadPointer(config.deviceVar));}

// Render thread, once per observed scene (TraceEntry observeScene). S = runtime::GraphicsSettings::Settings.
template<class S> void publish(const S& s,unsigned mode,unsigned phase) noexcept {
    unsigned f=0,b=0;
    if constexpr(requires{s.textureFilter;})f=unsigned(s.textureFilter);
    if constexpr(requires{s.mipBias;})b=unsigned(s.mipBias);
    const auto scenes=inputs.scenes.fetch_add(1,std::memory_order_relaxed)+1;
    unsigned own=0;
    if((mode==testModeFilterAB||mode==testModeBiasAB)&&phase==0){phase=((scenes/60)&1)?2u:1u;own=1;} // no caller phase: own 60-scene windows
    inputs.filterLevel.store(f,std::memory_order_relaxed);inputs.biasLevel.store(b,std::memory_order_relaxed);
    inputs.testMode.store(mode,std::memory_order_relaxed);inputs.phase.store(phase,std::memory_order_relaxed);
    inputs.phaseOwn.store(own,std::memory_order_relaxed);
}

inline bool readSampler(std::uintptr_t p,SamplerView& v) noexcept {
    v=SamplerView{};
    const auto* o=reinterpret_cast<const unsigned char*>(p);
    std::uintptr_t vptr{};std::memcpy(&vptr,o,sizeof vptr);
    v.vtableOk=vptr==config.samplerVtable;
    if(!v.vtableOk)return false;
    std::memcpy(&v.bindless,o+bindlessField,4);std::memcpy(&v.minF,o+minFilterField,4);std::memcpy(&v.magF,o+magFilterField,4);
    std::memcpy(&v.mipF,o+mipFilterField,4);std::memcpy(&v.level,o+levelField,4);std::memcpy(&v.ownAniso,o+ownAnisoField,4);
    std::memcpy(&v.bias,o+samplerBiasField,4);std::memcpy(&v.ownBias,o+ownBiasField,4);
    return true;
}
inline long milli(float x) noexcept {return std::isfinite(x)?std::lround(double(x)*1000.0):LONG_MIN;}
// Baseline: the native samplers before the first override write (main thread, once at least one sampler exists).
inline void buildBaseline() noexcept {
    if(baselineReady.load(std::memory_order_relaxed)||!config.samplers)return;
    try{
        std::ostringstream o;unsigned rows{};
        static constexpr char digits[]="0123456789abcdef";
        for(unsigned i=0;i<samplerCount;++i){
            const auto p=loadPointer(config.samplers+8u*i);if(!p)continue;
            SamplerView v{};const bool ok=readSampler(p,v);
            std::string packed;
            if(config.packed){const auto* q=reinterpret_cast<const unsigned char*>(config.packed+packedStride*i);
                for(std::size_t k=0;k<packedStride;++k){packed+=digits[q[k]>>4];packed+=digits[q[k]&15];}}
            o<<(rows++?",":"")<<'['<<i;
            if(ok){
                const long bias=milli(v.bias),own=milli(v.ownBias);
                o<<','<<v.bindless<<','<<v.minF<<','<<v.magF<<','<<v.mipF<<','<<v.level<<','<<v.ownAniso<<',';
                if(bias==LONG_MIN)o<<"null";else o<<bias;
                o<<',';
                if(own==LONG_MIN)o<<"null";else o<<own;
            }else o<<",\"foreignVtable\"";
            o<<",\""<<packed<<"\"]";
        }
        if(!rows)return; // samplers not created yet: retry on the next call
        baselineDump=o.str();baselineReady.store(true,std::memory_order_release);
    }catch(...){}
}

// Main thread, inside dmzHook before the native PerformDMZ.
inline void apply() noexcept {
    auto& s=state;s.applies.fetch_add(1,std::memory_order_relaxed);
    if(s.disabled.load(std::memory_order_relaxed)||s.suspended.load(std::memory_order_relaxed))return;
    unsigned char* dev=device();
    if(!dev){s.declined.fetch_add(1,std::memory_order_relaxed);return;}
    if(!s.captured.load(std::memory_order_relaxed)){
        const int request=loadI32(dev+requestField),cap=loadI32(dev+capField);const float bias=loadF32(dev+biasField);
        s.nativeRequest.store(request,std::memory_order_relaxed);s.nativeCap.store(cap,std::memory_order_relaxed);
        s.nativeBias.store(bias,std::memory_order_relaxed);
        if(!plausible(request,cap,bias)){
            s.declineReason.store("implausible",std::memory_order_relaxed);s.disabled.store(true,std::memory_order_relaxed);
            s.declined.fetch_add(1,std::memory_order_relaxed);return;
        }
        s.wantCap.store(cap,std::memory_order_relaxed);s.wantBias.store(bias,std::memory_order_relaxed);
        s.confirmedCap.store(cap,std::memory_order_relaxed);s.confirmedBiasCenti.store(0,std::memory_order_relaxed); // nothing written yet
        s.captured.store(true,std::memory_order_release);
    }
    buildBaseline();
    const int nativeCap=s.nativeCap.load(std::memory_order_relaxed);const float nativeBias=s.nativeBias.load(std::memory_order_relaxed);
    const auto scenes=inputs.scenes.load(std::memory_order_relaxed);
    Want w{};
    if(!config.live&&scenes>0){ // boot-only: frozen at the value in force when the world started
        w={s.wantCap.load(std::memory_order_relaxed),s.wantBias.load(std::memory_order_relaxed)};
        s.frozenBoot.fetch_add(1,std::memory_order_relaxed);
    }else{
        w=want(inputs.filterLevel.load(std::memory_order_relaxed),inputs.biasLevel.load(std::memory_order_relaxed),
               inputs.testMode.load(std::memory_order_relaxed),inputs.phase.load(std::memory_order_relaxed),nativeCap,nativeBias);
        if(config.live&&scenes==0&&(w.cap!=nativeCap||!sameBits(w.bias,nativeBias))){ // no cap while effects may still load
            w={nativeCap,nativeBias};s.deferredBoot.fetch_add(1,std::memory_order_relaxed);
        }
    }
    const int curCap=loadI32(dev+capField);const float curBias=loadF32(dev+biasField);
    if(s.biasSuspended.load(std::memory_order_relaxed))w.bias=curBias; // fight: the engine owns the bias
    s.wantCap.store(w.cap,std::memory_order_relaxed);s.wantBias.store(w.bias,std::memory_order_relaxed);
    auto& o=owner;
    if(!o.haveLast&&w.cap==curCap&&sameBits(w.bias,curBias)){if(loadU8(dev+dirtyField))s.dirtyStuck.fetch_add(1,std::memory_order_relaxed);return;}
    if(!o.haveLast&&w.cap==nativeCap&&sameBits(w.bias,nativeBias))return; // NATIVE/0 and never changed: the engine owns both fields
    const Plan p=plan(curCap,curBias,w,o.haveLast,o.lastCap,o.lastBias);
    if(p.engineOverride){
        const auto overrides=s.engineOverrides.fetch_add(1,std::memory_order_relaxed)+1;
        if(curCap==nativeCap&&sameBits(curBias,nativeBias))s.engineNativeWrites.fetch_add(1,std::memory_order_relaxed);
        const auto now=monotonicNs();
        if(!o.fightWindowNs||now-o.fightWindowNs>10'000'000'000ull){o.fightWindowNs=now?now:1;o.fightWindowBase=overrides-1;}
        else if(overrides-o.fightWindowBase>100){
            if(!s.biasSuspended.load(std::memory_order_relaxed)){
                s.biasSuspended.store(true,std::memory_order_relaxed);s.declineReason.store("fight",std::memory_order_relaxed);
                o.fightWindowNs=now?now:1;o.fightWindowBase=overrides;
            }else{ // still fighting with the bias left alone: stop writing entirely
                s.suspended.store(true,std::memory_order_relaxed);s.declineReason.store("fight-cap",std::memory_order_relaxed);
                o.haveLast=true;o.lastCap=curCap;o.lastBias=curBias;return;
            }
            w.bias=curBias;s.wantBias.store(w.bias,std::memory_order_relaxed);
        }
    }
    if(p.write&&(curCap!=w.cap||!sameBits(curBias,w.bias))){
        storeU32(dev+capField,std::uint32_t(w.cap));storeU32(dev+biasField,std::bit_cast<std::uint32_t>(w.bias));
        storeU8Release(dev+dirtyField,1);
        s.writes.fetch_add(1,std::memory_order_relaxed);
    }else if(loadU8(dev+dirtyField))s.dirtyStuck.fetch_add(1,std::memory_order_relaxed);
    // Native values restored: the engine owns both fields again (no enforcement, no override counting at NATIVE/0).
    if(w.cap==nativeCap&&sameBits(w.bias,nativeBias))o.haveLast=false;
    else{o.haveLast=true;o.lastCap=w.cap;o.lastBias=w.bias;}
}

inline void dmzHookAt(void* renderer,std::uintptr_t caller) { // not noexcept: the native call may unwind
    state.dmzCalls.fetch_add(1,std::memory_order_relaxed);
    if(caller==config.dmzReturn&&config.knobs){const int saved=errno;apply();errno=saved;}
    else state.foreignDmz.fetch_add(1,std::memory_order_relaxed);
    originalDmz(renderer);
}
// GOT replacement for sagRenderer::PerformDMZ (main thread).
RDRVR_EN_NOINLINE inline void dmzHook(void* renderer) {dmzHookAt(renderer,RDRVR_EN_RETURN_ADDRESS());}

// After every exact RecreateSamplerStates: decode the 55 samplers and confirm what the GPU now samples with.
inline void observeRecreate() noexcept {
    auto& s=state;
    unsigned char* dev=device();
    if(dev&&loadU8(dev+dirtyField))s.dirtyAfterRecreate.fetch_add(1,std::memory_order_relaxed);
    std::array<SamplerView,samplerCount> views{};unsigned n{},nulls{};
    for(unsigned i=0;i<samplerCount;++i){
        const auto p=loadPointer(config.samplers+8u*i);
        if(!p){++nulls;s.appliedLevels[i].store(-1,std::memory_order_relaxed);continue;}
        const bool ok=readSampler(p,views[n]);
        s.appliedLevels[i].store(ok?views[n].level:-2,std::memory_order_relaxed);++n;
    }
    const Summary sum=decode(views.data(),n);
    s.samplers.store(sum.samplers,std::memory_order_relaxed);s.nullSamplers.store(nulls,std::memory_order_relaxed);
    s.anisoSamplers.store(sum.anisoSamplers,std::memory_order_relaxed);s.foreignVtable.store(sum.foreignVtable,std::memory_order_relaxed);
    s.biasSamplers.store(sum.biasSamplers,std::memory_order_relaxed);
    for(unsigned i=0;i<levelBuckets;++i)s.levels[i].store(sum.levels[i],std::memory_order_relaxed);
    s.maxLevel.store(sum.maxLevel,std::memory_order_relaxed);s.minLevel.store(sum.minLevel,std::memory_order_relaxed);
    s.biasMin.store(sum.biasMin,std::memory_order_relaxed);s.biasMax.store(sum.biasMax,std::memory_order_relaxed);
    s.decodes.fetch_add(1,std::memory_order_relaxed);
    if(!s.captured.load(std::memory_order_acquire))return;
    // Confirmed = what this recreate really applied (+0xbc/+0xc0 as the loop read them, whoever wrote them), checked
    // against the decoded samplers: no anisotropic level above the cap, applied bias = the global bias everywhere.
    const int cap=dev?loadI32(dev+capField):s.wantCap.load(std::memory_order_relaxed);
    const float bias=dev?loadF32(dev+biasField):s.wantBias.load(std::memory_order_relaxed);
    const float nb=s.nativeBias.load(std::memory_order_relaxed);
    s.confirmedCap.store(cap>=0&&(sum.anisoSamplers==0||sum.maxLevel<=cap)?cap:-1,std::memory_order_relaxed);
    const bool biasOk=std::isfinite(bias)&&sum.biasSamplers>0&&std::fabs(sum.biasMin-bias)<=1e-3f&&std::fabs(sum.biasMax-bias)<=1e-3f;
    s.confirmedBiasCenti.store(biasOk?int(std::lround(double(bias-nb)*100.0)):INT_MIN,std::memory_order_relaxed);
}
inline void recreateHookAt(std::uintptr_t caller) {
    if(caller!=config.recreateReturn||!config.knobs){state.foreignRecreates.fetch_add(1,std::memory_order_relaxed);originalRecreate();return;}
    const int saved=errno;
    const auto t0=monotonicNs();originalRecreate();const auto t1=monotonicNs();
    auto& s=state;s.recreates.fetch_add(1,std::memory_order_relaxed);
    const std::uint64_t ns=t1>=t0?t1-t0:0;s.recreateNsLast.store(ns,std::memory_order_relaxed);
    if(ns>s.recreateNsMax.load(std::memory_order_relaxed))s.recreateNsMax.store(ns,std::memory_order_relaxed);
    observeRecreate();
    errno=saved;
}
// GOT replacement for grcEffect::RecreateSamplerStates (called by PerformDMZ, main thread). Observe only.
RDRVR_EN_NOINLINE inline void recreateHook() {recreateHookAt(RDRVR_EN_RETURN_ADDRESS());}

inline void observeLod(std::uint32_t tri,float modifier,float lo,float hi,std::uint32_t triLo,std::uint32_t triHi,bool enabled) noexcept {
    auto& l=lod;l.frames.fetch_add(1,std::memory_order_relaxed);
    l.modLast.store(modifier,std::memory_order_relaxed);l.boundLo.store(lo,std::memory_order_relaxed);l.boundHi.store(hi,std::memory_order_relaxed);
    l.triLo.store(triLo,std::memory_order_relaxed);l.triHi.store(triHi,std::memory_order_relaxed);l.enabled.store(enabled,std::memory_order_relaxed);
    if(std::isfinite(modifier)&&modifier>=0.f&&modifier<1e6f)l.modSumMilli.fetch_add(std::uint64_t(std::llround(double(modifier)*1000.0)),std::memory_order_relaxed);
    switch(classifyLod(tri,triLo,triHi,enabled)){
    case LodFloor:l.atFloor.fetch_add(1,std::memory_order_relaxed);break;
    case LodCeil:l.atCeil.fetch_add(1,std::memory_order_relaxed);break;
    case LodBetween:l.between.fetch_add(1,std::memory_order_relaxed);break;
    default:l.disabled.fetch_add(1,std::memory_order_relaxed);break;
    }
    l.triLast.store(tri,std::memory_order_relaxed);l.triSum.fetch_add(tri,std::memory_order_relaxed);
    if(tri>l.triMax.load(std::memory_order_relaxed))l.triMax.store(tri,std::memory_order_relaxed);
    l.triHist[triBucket(tri)].fetch_add(1,std::memory_order_relaxed);
}
inline void beginFrameHookAt(std::uintptr_t caller) {
    const bool exact=caller==config.beginFrameReturn&&config.lod;
    std::uint32_t tri{};
    if(exact)tri=loadU32(reinterpret_cast<const void*>(config.triCount)); // the original zeroes it
    originalBeginFrame();
    if(!exact){lod.foreignCalls.fetch_add(1,std::memory_order_relaxed);return;}
    const auto f=[](std::uintptr_t a){return loadF32(reinterpret_cast<const void*>(a));};
    const auto u=[](std::uintptr_t a){return loadU32(reinterpret_cast<const void*>(a));};
    observeLod(tri,f(config.lodModifier),f(config.lodBounds),f(config.lodBounds+4),u(config.triBounds),u(config.triBounds+4),
        loadU8(reinterpret_cast<const void*>(config.lodEnable))!=0);
}
// GOT replacement for rage::grcDevice::BeginFrame (grcSetup::BeginDraw). Observe only.
RDRVR_EN_NOINLINE inline void beginFrameHook() {beginFrameHookAt(RDRVR_EN_RETURN_ADDRESS());}

// byState signature letters (GameXr stereoGpuState): _af<confirmed cap|x>_mb<confirmed bias centi|x>.
inline void appendSignature(char* sig,std::size_t cap) noexcept {
    if(!sig||!cap)return;
    const void* end=std::memchr(sig,0,cap);if(!end)return;
    const std::size_t len=std::size_t(static_cast<const char*>(end)-sig);
    const int c=state.hooked.load(std::memory_order_relaxed)?state.confirmedCap.load(std::memory_order_relaxed):-1;
    const int b=state.hooked.load(std::memory_order_relaxed)?state.confirmedBiasCenti.load(std::memory_order_relaxed):INT_MIN;
    char af[16]="x",mb[16]="x";
    if(c>=0)std::snprintf(af,sizeof af,"%d",c);
    if(b!=INT_MIN)std::snprintf(mb,sizeof mb,"%d",b);
    std::snprintf(sig+len,cap-len,"_af%s_mb%s",af,mb);
}

// Installation (TraceEntry). code(offset,bytes): executable fingerprint at engine base+offset; read(address,out,size):
// checked read of an absolute address; replace(slot,expected,replacement): GOT compare-and-swap on absolute addresses.
struct InstallResult {bool knobs{},textureFilter{},live{},lod{},meminfo{},meminfoEnabled{},rollbackFault{};const char* reason{"not run"};};
template<class Code,class Read,class Replace,class Prop=AndroidProperty>
InstallResult install(std::uintptr_t base,const Code& code,const Read& read,const Replace& replace,const Prop& prop=Prop{}) {
    InstallResult r{};
    if(prop("debug.rdrvr.engineknobs")==0){r.reason="property";state.declineReason.store("property");return r;}
    r.knobs=true;r.reason="ok";
    const auto got=[&](std::uintptr_t slot,std::uintptr_t target){std::uintptr_t v{};return read(base+slot,&v,sizeof v)&&v==base+target;};
    // TEXTURE FILTER / MIP BIAS: RecreateSamplerStates observer first, then PerformDMZ; the first is rolled back if the second fails.
    bool ok=got(gfxDeviceGotOffset,gfxDeviceOffset)&&got(d11SamplersGotOffset,d11SamplersOffset)&&got(packedSamplersGotOffset,packedSamplersOffset)&&
        code(performDmzOffset,performDmzFingerprint)&&code(dirtySiteOffset,dirtySiteFingerprint)&&code(shellDmzCallOffset,shellDmzCallFingerprint)&&
        code(recreateOffset,recreateFingerprint)&&code(recreateAnisoSiteOffset,recreateAnisoSiteFingerprint)&&
        code(recreateExcludeSiteOffset,recreateExcludeSiteFingerprint)&&code(recreateBiasSiteOffset,recreateBiasSiteFingerprint)&&
        code(recreateClearSiteOffset,recreateClearSiteFingerprint)&&code(setAnisotropyOffset,setAnisotropyFingerprint)&&
        code(setMipBiasOffset,setMipBiasFingerprint)&&code(samplerRecreateOffset,samplerRecreateFingerprint)&&code(getBindlessOffset,getBindlessFingerprint);
    for(unsigned i=0;i<samplerVtableSlots.size()&&ok;++i)ok=got(samplerVtableOffset+samplerVtableSlots[i],samplerVtableFunctions[i]);
    if(!ok)state.declineReason.store("fingerprint");
    else{
        config.deviceVar=base+gfxDeviceOffset;config.samplers=base+d11SamplersOffset;config.packed=base+packedSamplersOffset;
        config.samplerVtable=base+samplerVtableOffset;config.dmzReturn=base+performDmzReturnOffset;config.recreateReturn=base+recreateReturnOffset;
        config.live=prop("debug.rdrvr.texfilter.live")!=0;config.knobs=true; // before the slots go live: the hooks read config
        originalRecreate=reinterpret_cast<Recreate>(base+recreateOffset);originalDmz=reinterpret_cast<Dmz>(base+performDmzOffset);
        const auto recreateWrapper=reinterpret_cast<std::uintptr_t>(&recreateHook),dmzWrapper=reinterpret_cast<std::uintptr_t>(&dmzHook);
        if(replace(base+recreateGotOffset,base+recreateOffset,recreateWrapper)){
            if(replace(base+performDmzGotOffset,base+performDmzOffset,dmzWrapper))r.textureFilter=true;
            else{if(!replace(base+recreateGotOffset,recreateWrapper,base+recreateOffset))r.rollbackFault=true;state.declineReason.store("got");}
        }else state.declineReason.store("got");
        if(!r.textureFilter)config.knobs=false; // a wrapper left behind by a failed rollback only forwards
    }
    r.live=config.live;state.live.store(config.live);state.hooked.store(r.textureFilter);
    // engineLod: observe-only BeginFrame hook.
    if(code(beginFrameOffset,beginFrameFingerprint)&&code(lodBlockOffset,lodBlockFingerprint)&&code(beginFrameCallOffset,beginFrameCallFingerprint)&&
       got(lodModifierGotOffset,lodModifierOffset)&&got(lodBoundsGotOffset,lodBoundsOffset)&&got(triCountGotOffset,triCountOffset)&&
       got(triBoundsGotOffset,triBoundsOffset)&&got(lodEnableGotOffset,lodEnableOffset)){
        config.beginFrameReturn=base+beginFrameReturnOffset;config.lodModifier=base+lodModifierOffset;config.lodBounds=base+lodBoundsOffset;
        config.triCount=base+triCountOffset;config.triBounds=base+triBoundsOffset;config.lodEnable=base+lodEnableOffset;
        originalBeginFrame=reinterpret_cast<BeginFrame>(base+beginFrameOffset);config.lod=true;
        r.lod=replace(base+beginFrameGotOffset,base+beginFrameOffset,reinterpret_cast<std::uintptr_t>(&beginFrameHook));
        if(!r.lod)config.lod=false;
    }
    lod.hooked.store(r.lod);
    // meminfo cache.
    if(code(meminfo::functionOffset,meminfo::fingerprint)&&code(meminfo::lowMemoryOffset,meminfo::lowMemoryFingerprint)&&
       code(meminfo::checkpointCallOffset,meminfo::checkpointCallFingerprint)){
        r.meminfoEnabled=prop("debug.rdrvr.meminfocache")!=0;
        meminfo::original=reinterpret_cast<meminfo::Original>(base+meminfo::functionOffset);
        meminfo::cache.configure(base+meminfo::lowMemoryReturn,base+meminfo::checkpointReturn,r.meminfoEnabled);
        r.meminfo=replace(base+meminfo::gotOffset,base+meminfo::functionOffset,reinterpret_cast<std::uintptr_t>(&meminfo::hook));
    }
    meminfo::hooked.store(r.meminfo);
    if(!r.textureFilter)r.reason=state.declineReason.load()?state.declineReason.load():"texture filter";
    return r;
}

// Host tests: back to the pristine state (config, inputs, state, lod, owner, baseline).
inline void resetForTest() noexcept {
    config=Config{};originalDmz=nullptr;originalRecreate=nullptr;originalBeginFrame=nullptr;
    for(auto* a:{&inputs.filterLevel,&inputs.biasLevel,&inputs.testMode,&inputs.phase,&inputs.phaseOwn})a->store(0);
    inputs.scenes.store(0);
    auto& s=state;
    for(auto* a:{&s.hooked,&s.captured,&s.disabled,&s.biasSuspended,&s.suspended})a->store(false);
    s.live.store(true);s.declineReason.store(nullptr);
    s.nativeRequest.store(-1);s.nativeCap.store(-1);s.wantCap.store(-1);s.nativeBias.store(0.f);s.wantBias.store(0.f);
    for(auto* a:{&s.dmzCalls,&s.foreignDmz,&s.applies,&s.writes,&s.deferredBoot,&s.frozenBoot,&s.recreates,&s.foreignRecreates,&s.recreateNsLast,
        &s.recreateNsMax,&s.dirtyStuck,&s.dirtyAfterRecreate,&s.engineOverrides,&s.engineNativeWrites,&s.declined})a->store(0);
    s.confirmedCap.store(-1);s.confirmedBiasCenti.store(INT_MIN);
    for(auto* a:{&s.decodes,&s.samplers,&s.nullSamplers,&s.anisoSamplers,&s.foreignVtable,&s.biasSamplers})a->store(0);
    for(auto& a:s.levels)a.store(0);
    s.maxLevel.store(-1);s.minLevel.store(-1);s.biasMin.store(NAN);s.biasMax.store(NAN);
    for(auto& a:s.appliedLevels)a.store(-1);
    auto& l=lod;l.hooked.store(false);l.enabled.store(false);
    for(auto* a:{&l.frames,&l.foreignCalls,&l.modSumMilli,&l.atFloor,&l.atCeil,&l.between,&l.disabled,&l.triSum})a->store(0);
    l.modLast.store(NAN);l.boundLo.store(NAN);l.boundHi.store(NAN);
    for(auto* a:{&l.triLast,&l.triMax,&l.triLo,&l.triHi})a->store(0);
    for(auto& a:l.triHist)a.store(0);
    owner=Owner{};baselineReady.store(false);baselineDump.clear();
}

inline void jsonFloat(std::ostringstream& o,float x) {if(std::isfinite(x))o<<x;else o<<"null";}
// "textureFilter":{...},"engineLod":{...} (members of the report's graphics object).
inline std::string report() {
    const auto& s=state;const auto& in=inputs;std::ostringstream o;
    const unsigned f=in.filterLevel.load(),b=in.biasLevel.load();
    const char* reason=s.declineReason.load();
    o<<"\"textureFilter\":{\"hooked\":"<<s.hooked.load()<<",\"live\":"<<s.live.load()<<",\"level\":"<<f<<",\"levelName\":\""<<filterName(f)
     <<"\",\"biasLevel\":"<<b<<",\"biasName\":\""<<biasName(b)<<"\",\"testMode\":"<<in.testMode.load()<<",\"phase\":"<<in.phase.load()
     <<",\"phaseOwn\":"<<in.phaseOwn.load()<<",\"scenes\":"<<in.scenes.load()
     <<",\"native\":{\"request\":"<<s.nativeRequest.load()<<",\"cap\":"<<s.nativeCap.load()<<",\"bias\":";
    jsonFloat(o,s.nativeBias.load());
    o<<"},\"want\":{\"cap\":"<<s.wantCap.load()<<",\"bias\":";jsonFloat(o,s.wantBias.load());
    o<<"},\"device\":";
    if(const unsigned char* dev=state.hooked.load()?device():nullptr){
        o<<"{\"request\":"<<loadI32(dev+requestField)<<",\"cap\":"<<loadI32(dev+capField)<<",\"bias\":";jsonFloat(o,loadF32(dev+biasField));
        o<<",\"dirty\":"<<unsigned(loadU8(dev+dirtyField))<<'}';
    }else o<<"null";
    o<<",\"dmzCalls\":"<<s.dmzCalls.load()<<",\"foreignDmz\":"<<s.foreignDmz.load()<<",\"applies\":"<<s.applies.load()
     <<",\"writes\":"<<s.writes.load()<<",\"deferredBoot\":"<<s.deferredBoot.load()<<",\"frozenBoot\":"<<s.frozenBoot.load()
     <<",\"recreates\":"<<s.recreates.load()<<",\"foreignRecreates\":"<<s.foreignRecreates.load()
     <<",\"recreateUsLast\":"<<s.recreateNsLast.load()/1000<<",\"recreateUsMax\":"<<s.recreateNsMax.load()/1000
     <<",\"dirtyStuck\":"<<s.dirtyStuck.load()<<",\"dirtyAfterRecreate\":"<<s.dirtyAfterRecreate.load()
     <<",\"engineOverrides\":"<<s.engineOverrides.load()<<",\"engineNativeWrites\":"<<s.engineNativeWrites.load()
     <<",\"biasSuspended\":"<<s.biasSuspended.load()<<",\"suspended\":"<<s.suspended.load()
     <<",\"declined\":"<<s.declined.load()<<",\"declineReason\":";
    if(reason)o<<'"'<<reason<<'"';else o<<"null";
    const int cc=s.confirmedCap.load(),cb=s.confirmedBiasCenti.load();
    o<<",\"confirmed\":{\"cap\":";if(cc>=0)o<<cc;else o<<"null";
    o<<",\"biasCenti\":";if(cb!=INT_MIN)o<<cb;else o<<"null";
    o<<"},\"applied\":{\"decodes\":"<<s.decodes.load()<<",\"samplers\":"<<s.samplers.load()<<",\"nullSamplers\":"<<s.nullSamplers.load()
     <<",\"anisoSamplers\":"<<s.anisoSamplers.load()<<",\"maxLevel\":"<<s.maxLevel.load()<<",\"minLevel\":"<<s.minLevel.load()<<",\"levels\":{";
    for(unsigned i=0;i<levelBuckets;++i)o<<(i?",":"")<<'"'<<levelBucketNames[i]<<"\":"<<s.levels[i].load();
    o<<"},\"biasMin\":";jsonFloat(o,s.biasMin.load());o<<",\"biasMax\":";jsonFloat(o,s.biasMax.load());
    o<<",\"biasSamplers\":"<<s.biasSamplers.load()<<",\"foreignVtable\":"<<s.foreignVtable.load()
     <<",\"biasExcludedBindless\":["<<biasExcludedBindless[0]<<','<<biasExcludedBindless[1]<<"]}"
     <<",\"samplers\":";
    if(baselineReady.load(std::memory_order_acquire))o<<'['<<baselineDump<<']';else o<<"null";
    o<<",\"samplersAppliedLevels\":[";
    for(unsigned i=0;i<samplerCount;++i)o<<(i?",":"")<<s.appliedLevels[i].load();
    o<<"]}";
    const auto& l=lod;const auto frames=l.frames.load();
    std::array<std::uint64_t,triBuckets> hist{};for(unsigned i=0;i<triBuckets;++i)hist[i]=l.triHist[i].load();
    const auto triMax=l.triMax.load();
    o<<",\"engineLod\":{\"hooked\":"<<l.hooked.load()<<",\"enabled\":"<<l.enabled.load()<<",\"bounds\":[";
    jsonFloat(o,l.boundLo.load());o<<',';jsonFloat(o,l.boundHi.load());
    o<<"],\"triBounds\":["<<l.triLo.load()<<','<<l.triHi.load()<<"],\"frames\":"<<frames<<",\"foreignCalls\":"<<l.foreignCalls.load()
     <<",\"modifier\":{\"last\":";jsonFloat(o,l.modLast.load());
    o<<",\"mean\":";if(frames)o<<double(l.modSumMilli.load())/1000.0/double(frames);else o<<"null";
    o<<",\"atFloor\":"<<l.atFloor.load()<<",\"atCeil\":"<<l.atCeil.load()<<",\"between\":"<<l.between.load()<<",\"disabled\":"<<l.disabled.load()
     <<"},\"tri\":{\"last\":"<<l.triLast.load()<<",\"mean\":";if(frames)o<<double(l.triSum.load())/double(frames);else o<<"null";
    o<<",\"p50\":"<<triPercentile(hist.data(),50,triMax)<<",\"p95\":"<<triPercentile(hist.data(),95,triMax)<<",\"max\":"<<triMax<<"}}";
    return o.str();
}
}
