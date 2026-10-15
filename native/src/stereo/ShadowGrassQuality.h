#pragma once
// GRAPHICS rows SHADOW MAP (cascade shadow-map extent, applied when the engine creates the target =
// after restart) and GRASS DISTANCE (grass draw distance, live), both engine-native knobs of librdr 1.58
// (arm64). There is no SHADOW QUALITY row: the device already runs blur mode 0 / cascade blend 0, so every
// level would be a no-op. FilterScope is an inert observer (qualityLevel is always 0: it only records
// the native fields). SHADOW MAP levels are NATIVE/640/512 (1024/800 are not below the device's 800).
// SHADOW UPDATE, SHADOW UPDATE INDOOR and CASCADE CULL live in ShadowUpdate.h; the indoor signal is in
// IndoorState.h (report section graphics.indoor, appended after shadowUpdate below).
//
// SHADOW QUALITY: renLightPrePassLightsMgr::SetupConstants (called at the start of every RenderDirectionalShadow)
//   uploads three manager fields into the receiver's $Globals every call:
//     +0xae0 int  -> ShadowBlurMode         (GraphicsOptions ShadowSoftness; ctor 3 = contact-hardening: blocker search + PCF)
//     +0xae8 int  -> ShadowCascadeBlendMode (GraphicsOptions ShadowBlend;    ctor 2 = sample the next cascade too, every pixel)
//     +0xab4 int4 -> ShadowBlurSamples      (ctor 16,12,8,4 per cascade)
//   PS_DirectionalShadow3Tile taps per lit pixel: mode 3 = 2*(N_c+N_c'), mode 2 = N_c (+N_c' with blend 2),
//   mode 1 = 22 (fixed kernels), mode 0 = 4 (2x2 PCF); blend 0/1 = one cascade, blend 2 = two. Texel offsets come from
//   uniforms/ConstOffset (no baked map size). The override is written around the original call and restored after it
//   (compare-and-restore), so the engine and its options UI keep their own values; it never raises the cost.
// SHADOW MAP: CShadowMap::CreateRenderTargets picks the square extent from GraphicsOptions::ShadowQuality (+0x28):
//   {800,1600,2048,4096}[q] for q<=3, else CShadowMap::m_PCShadowmapSizeX/Y; it writes the chosen extent back to
//   m_PCShadowmapSizeX/Y, which every consumer reads (SetShadowMapConstants gShadow_MapSize + texel uniform,
//   BeginTileAroundSphere texel snapping, RenderShadowCasters viewport, atlas windows from the target). The hook
//   (GOT slot of the PLT call from rmlLightingMgr::InitShadowSystem) selects the m_PCShadowmapSize path with the
//   requested extent for the duration of the call and restores the quality byte. Only smaller extents are applied.
// GRASS DISTANCE: grassManager::Update writes the grass LOD distances every frame:
//   +0x74 collector limit 100+e, +0x78 cull 180+e (CalcVisibleList radius and grassField::Draw cull),
//   +0x7c/+0x80 thinning start 40+e/10+e, +0x84/+0x88 thinning end 180+e/120+e   (e = m_ExtraGrassStreamingDistance =
//   GraphicsOptions GrassStreamingDistance). Density thins from start to end; the hook (GOT slot of the PLT call from
//   sagGrassManager::UpdatePushPositions) scales cull/limit/ends by the level factor right after the native Update and
//   keeps the thinning starts (clamped below the new end), so far fields are culled and thin out before the new end.
#include "ShadowUpdate.h" // SHADOW UPDATE + receiver diagnostics (report section shadowUpdate)
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>

namespace rdrvr::engine::shadowgrass {
// ---- librdr 1.58 arm64 offsets (module-relative) ----
inline constexpr std::uintptr_t lightsInstanceOffset=0x4e85e728;     // renLightPrePassLightsMgr::sm_Instance
inline constexpr std::uintptr_t lightsInstanceGotOffset=0x2e0a860;
inline constexpr std::uintptr_t softnessField=0xae0,blendField=0xae8,samplesField=0xab4;
inline constexpr std::uintptr_t lightConstantsFilterOffset=0x2a3817c;  // add x21,x19,#0xab4  (ShadowBlurSamples source)
inline constexpr std::array<unsigned char,4> lightConstantsFilterFingerprint{117,210,42,145};
inline constexpr std::uintptr_t lightConstantsSamplesOffset=0x2a381ec; // ldr q0,[x21]
inline constexpr std::array<unsigned char,4> lightConstantsSamplesFingerprint{160,2,192,61};
inline constexpr std::uintptr_t lightConstantsSoftnessOffset=0x2a38290; // ldr s0,[x19,#0xae0] (-> ShadowBlurMode)
inline constexpr std::array<unsigned char,4> lightConstantsSoftnessFingerprint{96,226,74,189};
inline constexpr std::uintptr_t lightConstantsBlendOffset=0x2a382bc;  // ldr s0,[x19,#0xae8] (-> ShadowCascadeBlendMode)
inline constexpr std::array<unsigned char,4> lightConstantsBlendFingerprint{96,234,74,189};

inline constexpr std::uintptr_t shadowMapCreateOffset=0x29461a0;     // rage::CShadowMap::CreateRenderTargets()
inline constexpr std::uintptr_t shadowMapCreateGotOffset=0x2e24aa0;  // R_AARCH64_JUMP_SLOT
inline constexpr std::array<unsigned char,112> shadowMapCreateFingerprint{
#include "gamecode/ShadowGrassQuality_29461a0_112_8d.inc"
};
inline constexpr std::uintptr_t shadowMapTableOffset=0x1256890;      // u32 {800,1600,2048,4096}
inline constexpr std::array<unsigned char,16> shadowMapTableFingerprint{
#include "gamecode/ShadowGrassQuality_1256890_16_8d.inc"
};
inline constexpr std::array<std::uint32_t,4> shadowMapTable{
#include "gamecode/ShadowGrassQuality_1256890_16_32d.inc"
};
inline constexpr std::uintptr_t graphicsOptionsOffset=0x4e6d7380,graphicsOptionsGotOffset=0x2ddfcf0; // rage::GraphicsOptions::s_graphicsOptions
inline constexpr std::uintptr_t shadowQualityField=0x28;
inline constexpr std::uintptr_t pcSizeXOffset=0x2f48204,pcSizeXGotOffset=0x2de1740; // rage::CShadowMap::m_PCShadowmapSizeX
inline constexpr std::uintptr_t pcSizeYOffset=0x2f48208,pcSizeYGotOffset=0x2de1748;

inline constexpr std::uintptr_t grassUpdateOffset=0x230078c;          // rage::grassManager::Update()
inline constexpr std::uintptr_t grassUpdateGotOffset=0x2e2e300;       // R_AARCH64_JUMP_SLOT
inline constexpr std::array<unsigned char,72> grassUpdateFingerprint{
#include "gamecode/ShadowGrassQuality_230078c_72_8d.inc"
};
inline constexpr std::uintptr_t grassUpdateBaseOffset=0x1255e80;      // float4 {100,180,40,10}
inline constexpr std::array<unsigned char,16> grassUpdateBaseFingerprint{
#include "gamecode/ShadowGrassQuality_1255e80_16_8d.inc"
};
inline constexpr std::uintptr_t grassDrawPairsOffset=0x2300d30;       // DrawVisList: ldur s0,[x23,#-4]; ldr s1,[x19,#0x74]
inline constexpr std::array<unsigned char,12> grassDrawPairsFingerprint{224,194,95,188,97,118,64,189,0,32,33,30};
inline constexpr std::uintptr_t grassInstanceOffset=0x4e6fd478;       // atSingleton<rage::grassManager>::sm_Instance
inline constexpr std::uintptr_t grassExtraOffset=0x4e732ff0;          // grassManager::m_ExtraGrassStreamingDistance
inline constexpr std::uintptr_t grassLodField=0x74;                   // 6 floats: limit,cull,start0,start1,end0,end1
inline constexpr std::uintptr_t grassPairsField=0xb8,grassPairCountField=0xa0b8; // GrassSortPair[2560] {u8,float dist,field*}
inline constexpr int grassPairCapacity=2560;

// ---- levels ----
inline constexpr unsigned qualityLevels=4,mapLevels=3,grassLevels=4;
inline const char* qualityName(unsigned v) noexcept {return v==1?"MEDIUM":v==2?"LOW":v==3?"LOWEST":"HIGH";}
// SHADOW MAP levels: NATIVE/640/512 (the table minimum 800 is this device's native extent).
// CreateRenderTargets writes the chosen extent back to m_PCShadowmapSizeX/Y; BeginTileAroundSphere
// (0x2946d18) and SetShadowMapConstants (0x2947f70) upload {X,Y,1/X,1/Y} from those two globals and the
// receiver shader's PCF offset is 0.25/gShadow_MapSize, so every consumer follows the smaller atlas; the
// 2x2 tile windows are normalized (SetNormWindow) and scale with it.
inline const char* mapName(unsigned v) noexcept {return v==1?"640":v==2?"512":"NATIVE";}
inline constexpr std::uint32_t mapExtent(unsigned v) noexcept {return v==1?640u:v==2?512u:0u;}
inline const char* grassName(unsigned v) noexcept {return v==1?"75%":v==2?"50%":v==3?"35%":"FULL";}
inline constexpr float grassFactor(unsigned v) noexcept {return v==1?.75f:v==2?.5f:v==3?.35f:1.f;}

// Receiver filter plan. Cost rank of ShadowBlurMode: 0 (4 taps) < 2 (N) < 1 (22) < 3 (2N); >=4 = no shadow, untouched.
inline constexpr int softnessRank(int mode) noexcept {return mode==0?0:mode==2?1:mode==1?2:mode==3?3:-1;}
struct FilterPlan {int softness{},blend{};bool change{};};
inline constexpr FilterPlan planFilter(unsigned level,int softness,int blend) noexcept {
    FilterPlan p{softness,blend,false};
    if(!level||level>=qualityLevels)return p;
    if(blend==2){p.blend=1;p.change=true;}                          // dithered cascade transition: one cascade per pixel
    const int target=level==2?2:level==3?0:-1;                       // LOW: Vogel PCF without blocker search; LOWEST: 2x2
    if(target>=0&&softnessRank(softness)>softnessRank(target)){p.softness=target;p.change=true;}
    return p;
}
inline constexpr bool plausibleFilter(int softness,int blend,const std::int32_t* samples) noexcept {
    if(softness<0||softness>8||blend<0||blend>8)return false;
    for(int i=0;i<4;++i)if(samples[i]<1||samples[i]>64)return false;
    return true;
}
// Native extent CreateRenderTargets will choose (before MaxShadowMapSize).
inline constexpr std::uint32_t nativeMapExtent(std::uint8_t quality,std::uint32_t pcX,std::uint32_t pcY) noexcept {
    return quality<=3?shadowMapTable[quality]:std::min(pcX,pcY);
}
// grassManager::Update output layout check: {100,180,40,10,180,120}+e.
inline bool grassLayout(const float* v) noexcept {
    for(int i=0;i<6;++i)if(!std::isfinite(v[i]))return false;
    const auto near=[](float a,float b){return std::fabs(a-b)<=.01f+std::fabs(b)*1e-5f;};
    return near(v[1],v[0]+80.f)&&near(v[2],v[0]-60.f)&&near(v[3],v[0]-90.f)&&near(v[4],v[1])&&near(v[5],v[0]+20.f);
}
// Scales cull/limit/ends; keeps the thinning starts below half the new end. False when nothing sensible to do.
inline bool grassScale(float* v,float k) noexcept {
    if(!(k>0.f&&k<1.f)||!(v[1]>=20.f))return false;
    const float cull=v[1]*k,end1=std::max(v[5]*k,1.f);
    v[0]=std::max(v[0]*k,1.f);v[1]=cull;v[4]=cull;v[5]=end1;
    v[2]=std::min(v[2],.5f*cull);v[3]=std::min(v[3],.5f*end1);
    return true;
}

// ---- runtime state ----
struct Config {
    std::uintptr_t lightsInstance{},graphicsOptions{},pcSizeX{},pcSizeY{},grassInstance{},grassExtra{};
    bool filter{},map{},grass{},grassPairs{};
};
inline Config config; // written once while hooks are installed (before the engine renders)
using ShadowMapCreate=void(*)(void*);
using GrassUpdate=void(*)(void*);
inline ShadowMapCreate originalShadowMapCreate{};
inline GrassUpdate originalGrassUpdate{};
struct State {
    std::atomic<unsigned> qualityLevel{},grassLevel{},mapRequestedLevel{};
    std::atomic<std::uint64_t> filterCalls{},filterApplied{},filterUnchanged{},filterDeclined{},filterRestoreSkipped{};
    std::atomic<int> nativeSoftness{-1},nativeBlend{-1},appliedSoftness{-1},appliedBlend{-1};
    std::array<std::atomic<int>,4> nativeSamples{};
    std::atomic<bool> mapHook{},grassHook{};
    std::atomic<unsigned> mapCreates{},mapIntercepted{},nativeQuality{255},nativeExtent{},createdExtent{},requestedExtent{};
    std::atomic<std::uint64_t> grassUpdates{},grassScaled{},grassLayoutMismatch{},grassForeign{};
    std::atomic<float> extraDistance{},baseCull{},appliedCull{};
    std::atomic<std::uint64_t> grassDraws{},visibleFieldsSum{},fieldsBeyondSum{},pairsInvalid{};
    std::atomic<int> visibleFieldsLast{-1};
};
inline State state;
inline void* loadPointer(std::uintptr_t address) noexcept {void* p{};if(address)std::memcpy(&p,reinterpret_cast<const void*>(address),sizeof p);return p;}

// RAII around the native RenderDirectionalShadow (exact eye caller only).
class FilterScope {
    unsigned char* manager_{};int softness_{},blend_{},written_[2]{};bool active_{};
public:
    FilterScope(void* manager,bool exact) noexcept {
        if(!exact||!manager||!config.filter)return;
        auto& s=state;s.filterCalls.fetch_add(1,std::memory_order_relaxed);
        if(loadPointer(config.lightsInstance)!=manager){s.filterDeclined.fetch_add(1,std::memory_order_relaxed);return;}
        auto* m=static_cast<unsigned char*>(manager);
        int softness{},blend{};std::int32_t samples[4]{};
        std::memcpy(&softness,m+softnessField,4);std::memcpy(&blend,m+blendField,4);std::memcpy(samples,m+samplesField,sizeof samples);
        if(!plausibleFilter(softness,blend,samples)){s.filterDeclined.fetch_add(1,std::memory_order_relaxed);return;}
        s.nativeSoftness.store(softness,std::memory_order_relaxed);s.nativeBlend.store(blend,std::memory_order_relaxed);
        for(int i=0;i<4;++i)s.nativeSamples[i].store(samples[i],std::memory_order_relaxed);
        const auto plan=planFilter(s.qualityLevel.load(std::memory_order_relaxed),softness,blend);
        s.appliedSoftness.store(plan.softness,std::memory_order_relaxed);s.appliedBlend.store(plan.blend,std::memory_order_relaxed);
        if(!plan.change){s.filterUnchanged.fetch_add(1,std::memory_order_relaxed);return;}
        manager_=m;softness_=softness;blend_=blend;written_[0]=plan.softness;written_[1]=plan.blend;active_=true;
        std::memcpy(m+softnessField,&plan.softness,4);std::memcpy(m+blendField,&plan.blend,4);
        s.filterApplied.fetch_add(1,std::memory_order_relaxed);
    }
    ~FilterScope(){
        if(!active_)return;
        // Restore only what still holds the override value (an options change during the call wins).
        int now{};bool skipped=false;
        std::memcpy(&now,manager_+softnessField,4);if(now==written_[0])std::memcpy(manager_+softnessField,&softness_,4);else skipped=true;
        std::memcpy(&now,manager_+blendField,4);if(now==written_[1])std::memcpy(manager_+blendField,&blend_,4);else skipped=true;
        if(skipped)state.filterRestoreSkipped.fetch_add(1,std::memory_order_relaxed);
    }
    FilterScope(const FilterScope&)=delete;FilterScope& operator=(const FilterScope&)=delete;
};

// GOT replacement for CShadowMap::CreateRenderTargets (engine init; HandleDeferResize is the only other caller).
inline void shadowMapCreateHook(void* shadowMap) noexcept {
    auto& s=state;s.mapCreates.fetch_add(1,std::memory_order_relaxed);
    shadowupdate::foreignRender.store(true,std::memory_order_relaxed); // A (re)created atlas has no valid content to keep
    const std::uint32_t requested=mapExtent(s.mapRequestedLevel.load(std::memory_order_relaxed));
    s.requestedExtent.store(requested,std::memory_order_relaxed);
    if(!config.map){originalShadowMapCreate(shadowMap);return;}
    auto* quality=reinterpret_cast<std::uint8_t*>(config.graphicsOptions+shadowQualityField);
    auto* sizeX=reinterpret_cast<std::uint32_t*>(config.pcSizeX);auto* sizeY=reinterpret_cast<std::uint32_t*>(config.pcSizeY);
    const std::uint8_t q=*quality;const std::uint32_t native=nativeMapExtent(q,*sizeX,*sizeY);
    s.nativeQuality.store(q,std::memory_order_relaxed);s.nativeExtent.store(native,std::memory_order_relaxed);
    if(!requested||requested>=native){originalShadowMapCreate(shadowMap);s.createdExtent.store(*sizeX,std::memory_order_relaxed);return;}
    const bool tablePath=q<=3;
    *sizeX=requested;*sizeY=requested;
    if(tablePath)*quality=4; // >3: the m_PCShadowmapSizeX/Y path (scale 1.0, same as the 800/1600 table entries)
    originalShadowMapCreate(shadowMap);
    if(tablePath&&*quality==4)*quality=q; // the device/user ShadowQuality stays as loaded
    s.createdExtent.store(*sizeX,std::memory_order_relaxed);s.mapIntercepted.fetch_add(1,std::memory_order_relaxed);
}

// GOT replacement for grassManager::Update (once per frame, any thread).
inline void grassUpdateHook(void* manager) noexcept {
    originalGrassUpdate(manager);
    auto& s=state;s.grassUpdates.fetch_add(1,std::memory_order_relaxed);
    if(!config.grass||!manager)return;
    const void* instance=loadPointer(config.grassInstance);
    if(instance&&instance!=manager){s.grassForeign.fetch_add(1,std::memory_order_relaxed);return;}
    auto* lod=static_cast<unsigned char*>(manager)+grassLodField;
    float v[6]{};std::memcpy(v,lod,sizeof v);
    if(!grassLayout(v)){s.grassLayoutMismatch.fetch_add(1,std::memory_order_relaxed);return;}
    s.baseCull.store(v[1],std::memory_order_relaxed);s.extraDistance.store(v[0]-100.f,std::memory_order_relaxed);
    if(!grassScale(v,grassFactor(s.grassLevel.load(std::memory_order_relaxed)))){s.appliedCull.store(v[1],std::memory_order_relaxed);return;}
    std::memcpy(lod,v,sizeof v);s.appliedCull.store(v[1],std::memory_order_relaxed);s.grassScaled.fetch_add(1,std::memory_order_relaxed);
}

// Called before the native DrawVisList(0) of the exact eye: visible grass fields and those beyond the current cull.
inline void observeGrassDraw(void* manager,int pass,bool exact) noexcept {
    if(!exact||pass!=0||!manager||!config.grassPairs)return;
    auto& s=state;
    if(loadPointer(config.grassInstance)!=manager){s.pairsInvalid.fetch_add(1,std::memory_order_relaxed);return;}
    const auto* m=static_cast<const unsigned char*>(manager);
    int count{};std::memcpy(&count,m+grassPairCountField,4);
    if(count<0||count>grassPairCapacity){s.pairsInvalid.fetch_add(1,std::memory_order_relaxed);return;}
    float cull{};std::memcpy(&cull,m+grassLodField+4,4);
    unsigned beyond{};
    for(int i=0;i<count;++i){float d{};std::memcpy(&d,m+grassPairsField+16*std::size_t(i)+4,4);if(d>cull)++beyond;}
    s.grassDraws.fetch_add(1,std::memory_order_relaxed);s.visibleFieldsSum.fetch_add(unsigned(count),std::memory_order_relaxed);
    s.fieldsBeyondSum.fetch_add(beyond,std::memory_order_relaxed);s.visibleFieldsLast.store(count,std::memory_order_relaxed);
}

inline std::string report() {
    const auto& s=state;std::ostringstream o;
    const auto f=[](float x){return std::isfinite(x)?x:-1.f;};
    const auto draws=s.grassDraws.load();
    o<<"\"shadowQuality\":{\"inert\":true,\"level\":"<<s.qualityLevel.load()<<",\"ready\":"<<config.filter
     <<",\"calls\":"<<s.filterCalls.load()<<",\"applied\":"<<s.filterApplied.load()<<",\"unchanged\":"<<s.filterUnchanged.load()
     <<",\"declined\":"<<s.filterDeclined.load()<<",\"restoreSkipped\":"<<s.filterRestoreSkipped.load()
     <<",\"nativeBlurMode\":"<<s.nativeSoftness.load()<<",\"nativeCascadeBlend\":"<<s.nativeBlend.load()
     <<",\"nativeBlurSamples\":["<<s.nativeSamples[0].load()<<','<<s.nativeSamples[1].load()<<','<<s.nativeSamples[2].load()<<','<<s.nativeSamples[3].load()<<']'
     <<",\"appliedBlurMode\":"<<s.appliedSoftness.load()<<",\"appliedCascadeBlend\":"<<s.appliedBlend.load()
     <<",\"map\":{\"ready\":"<<config.map<<",\"hook\":"<<s.mapHook.load()<<",\"requestedLevel\":"<<s.mapRequestedLevel.load()
     <<",\"requestedExtent\":"<<s.requestedExtent.load()<<",\"creates\":"<<s.mapCreates.load()<<",\"intercepted\":"<<s.mapIntercepted.load()
     <<",\"nativeQuality\":"<<s.nativeQuality.load()<<",\"nativeExtent\":"<<s.nativeExtent.load()
     <<",\"cascadeExtent\":"<<s.createdExtent.load()<<",\"skippedCascadeFrames\":"<<shadowupdate::stats.skipped.load()<<"}}"
     <<",\"grassDistance\":{\"level\":"<<s.grassLevel.load()<<",\"name\":\""<<grassName(s.grassLevel.load())<<"\",\"ready\":"<<config.grass
     <<",\"hook\":"<<s.grassHook.load()<<",\"factor\":"<<grassFactor(s.grassLevel.load())<<",\"updates\":"<<s.grassUpdates.load()
     <<",\"scaled\":"<<s.grassScaled.load()<<",\"layoutMismatch\":"<<s.grassLayoutMismatch.load()<<",\"foreign\":"<<s.grassForeign.load()
     <<",\"extraStreamingDistance\":"<<f(s.extraDistance.load())<<",\"nativeCullM\":"<<f(s.baseCull.load())<<",\"appliedCullM\":"<<f(s.appliedCull.load())
     <<",\"colorDraws\":"<<draws<<",\"visibleFieldsLast\":"<<s.visibleFieldsLast.load()
     <<",\"visibleFieldsPerDraw\":"<<(draws?double(s.visibleFieldsSum.load())/double(draws):0.)
     <<",\"fieldsBeyondCullPerDraw\":"<<(draws?double(s.fieldsBeyondSum.load())/double(draws):0.)<<",\"pairsInvalid\":"<<s.pairsInvalid.load()<<'}';
    o<<','<<shadowupdate::report();
    o<<','<<indoor::report();
    return o.str();
}
}
