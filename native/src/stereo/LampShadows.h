#pragma once
// LAMP SHADOWS = local-light (lamp) shadow maps of rage::CCubeShadowMap. librdr 1.58 arm64:
//  * sagRenderer::DrawCubeShadowMaps @0x2a54144 (bl @0x2a53468 in RenderScene_RenderThread, PLT 0x2cae470, GOT 0x2e5f898,
//    return 0x2a5346c, single caller): copies the RT set j of the owned renResource into slot j (+0x88 cube, +0x98 spot,
//    +0xa0 HQ cascade only for a spot while count < CCubeShadowMap::NumUseCascade, else 0), calls RenderIntoCubeShadowMap,
//    then waits on the 5 cube visibility lists and clears slot+0x40 of a slot whose caster list is empty.
//  * rage::CCubeShadowMap::RenderIntoCubeShadowMap @0x294d148 (bl @0x2a5430c, PLT 0x2c38d70, GOT 0x2e24d18, return 0x2a54310,
//    single caller; x0 = CCubeShadowMap* = rmlLightingMgr::sm_CurrentMgr+0x20): per slot with +0x40 != 0 and per face
//    {6,1,5}[+0x68] visible in grcViewport::sm_Current: CCubeShadowMap::DrawShadow @0x294cac8 (bl @0x294d3f0, PLT 0x2c38d30,
//    GOT 0x2e24cf8, return 0x294d3f4, single caller; w1 face, w2 slot): Lock + Clear + casters + Unlock, then for a spot with
//    +0xa0 the "Spot Shadow Cascade" pass. Faces outside the view are not redrawn (the engine keeps their old content).
//  * Slot RT set j is fixed (renResource::CreateRenderTargets @0x2a5baa4, one resource of 5 sets): slots 0/1 cube 256 / spot
//    512, slots 2..4 cube 128 / spot 256, cascade 512 (pass keys "256/128/512x512 c1 d129").
// LAMP SHADOWS levers (preset row + debug.rdrvr.* properties):
//  * update EVERY / 1/2 / 1/3: per-slot staggered skip of DrawShadow for faces already drawn for the SAME light (slot bytes
//    +0x00..+0x87 except the +0x44 fade, light id +0x40, type +0x68 and the 3 RT pointers bit-identical). Forced render: no
//    snapshot, key change, a RenderScene without RenderIntoCubeShadowMap (scope/binoculars own the resource, menus), camera
//    cut > 5 m, a face that was never drawn for this light. Slots are split into phases by estimated GPU cost.
//  * spot cascade ON/OFF: NumUseCascade (.data 0x2f57d84, native 3) = 0 for the duration of the exact DrawCubeShadowMaps,
//    compare-and-restore (scripts keep their value: SetMaximumShadowSpotlightCascadeCount @0x2ab0da4).
//  * cull x1/x2/x4: CachedVisibility::sm_CubeShadowScreenSpaceThreshold (.data 0x2f7cfe0, 0.014; reader GetThreshold
//    lists 0..5 @0x2a6ebf8) scaled, persistent compare-and-restore (a foreign value suspends it).
// Readout: slot table, faces drawn/skipped, casters per slot, renLightPrePassLightsMgr list counts and HQ flags.
//  * POINT SHADOW CAP: int sagRenderer+0x1fbc (SelectionParams+0x1c, native 5). sagRenderer::sm_Instance .bss 0x4e85f110
//    (GLOB_DAT GOT 0x2e01660) = the renderer: sagShellDerived::Execute (ldr x0,[GOT sm_Instance] @0x23a043c) -> sagRenderer::Draw
//    (single caller) -> CreateSceneVisibility -> PrepareCubeShadowVisibilityRequests @0x2a52714 (x0 = this; it also reads
//    *sm_Instance+0x1fd4). DrawCubeShadowMaps has NO usable `this` (never reads x0; x0 at the bl @0x2a53468 is DrawShadowMaps'
//    leftover), so the cap is addressed through sm_Instance only. The block +0x1fa0..+0x1fd3 is
//    written only by the ctor (@0x2a4b83c..0x2a4b878: {1.3,0.55,0.45,0.2} {0.8,3.0} {0.0f, int 5} {1.2,0.9,0.025,20.0} 1.4; no
//    other store to +0x1fa0..+0x1fd0 in the binary). Prepare (main thread, every frame) copies it (ldr q0/q1/q2/w10
//    @0x2a52764..0x2a52770) into SelectionParams for SelectShadows @0x2a4a658, whose slot fill counts only POINT lights (type
//    +0xc0 != 1) against +0x1c (`ldr w10,[x19,#0x1c]; cmp w28,w10; b.ge next` @0x2a4add8) and then zeroes +0x40 of every unused
//    slot (@0x2a4b030..0x2a4b074): cap 0 = no shadowed points, spots are never capped. Written from the render-thread
//    DrawCubeShadowMaps wrapper with compare-and-restore (applyCap, as applyCull); read by the next frame's Prepare.
//  * OFF also zeroes slot +0x40 inside the exact RenderIntoCubeShadowMap before the native call (spots, or a suspended cap):
//    the native loop skips the slot and the receivers find no map (CCubeShadowMap::GetShadowIndex(light id) scans +0x40,
//    @0x294c8a4), exactly what DrawCubeShadowMaps itself does for an empty caster list.
//  * presets 4 1/3 CAP 2 (preset 3 + cap 2), 5 OFF (preset 3 + cap 0 + slot clear); debug.rdrvr.lampcap 0..3 (0 native =
//    kill switch, 1 cap 2, 2 cap 1, 3 OFF) beats the row; signature _ls<cap><update><cascade><cull> (cap digit only when != 0).
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace rdrvr::engine::lampshadow {
// ---- librdr 1.58 arm64 (module-relative) ----
inline constexpr std::uintptr_t drawCubeOffset=0x2a54144,drawCubeGotOffset=0x2e5f898,drawCubeReturnOffset=0x2a5346c;
inline constexpr std::uintptr_t renderIntoOffset=0x294d148,renderIntoGotOffset=0x2e24d18,renderIntoReturnOffset=0x2a54310;
inline constexpr std::uintptr_t drawShadowOffset=0x294cac8,drawShadowGotOffset=0x2e24cf8,drawShadowReturnOffset=0x294d3f4;
inline constexpr std::array<unsigned char,32> drawCubeFingerprint{
#include "gamecode/LampShadows_2a54144_32_8d.inc"
};
inline constexpr std::array<unsigned char,32> renderIntoFingerprint{
#include "gamecode/LampShadows_294d148_32_8d.inc"
};
inline constexpr std::array<unsigned char,32> drawShadowFingerprint{
#include "gamecode/LampShadows_1843e28_32_8d.inc"
};
inline constexpr std::array<unsigned char,4> drawCubeCallFingerprint{2,108,9,148};      // bl 0x2cae470 @0x2a53468
inline constexpr std::array<unsigned char,4> renderIntoCallFingerprint{153,146,7,148};  // bl 0x2c38d70 @0x2a5430c
inline constexpr std::array<unsigned char,4> drawShadowCallFingerprint{80,174,11,148};  // bl 0x2c38d30 @0x294d3f0
inline constexpr std::uintptr_t slotEnableLoadOffset=0x294d1f0;                          // ldr w10,[x10,#0x40]
inline constexpr std::array<unsigned char,4> slotEnableLoadFingerprint{74,65,64,185};
inline constexpr std::uintptr_t cascadeReaderOffset=0x2a54220;                           // ldr x14,[x14,#0x8f0] (GOT NumUseCascade)
inline constexpr std::array<unsigned char,4> cascadeReaderFingerprint{206,121,68,249};
inline constexpr std::uintptr_t cascadeLoadOffset=0x2a542f0;                             // ldr w1,[x14]
inline constexpr std::array<unsigned char,4> cascadeLoadFingerprint{193,1,64,185};
inline constexpr std::uintptr_t numUseCascadeOffset=0x2f57d84,numUseCascadeGotOffset=0x2de18f0,cascadeFovOffset=0x2f57d88;
inline constexpr std::uint32_t numUseCascadeNative=3,cascadeFovBits=0x3f0ccccdu;          // 3, 0.55f
inline constexpr std::uintptr_t cubeThresholdOffset=0x2f7cfe0,cubeThresholdGotOffset=0x2e10dd8,cubeThresholdCodeOffset=0x2a6ebf8;
inline constexpr std::array<unsigned char,12> cubeThresholdCodeFingerprint{9,29,0,208,41,237,70,249,33,1,64,189}; // adrp x9; ldr x9,[x9,#0xdd8]; ldr s1,[x9]
inline constexpr std::uint32_t cubeThresholdNativeBits=0x3c656042u;                      // 0.014f
inline constexpr std::uintptr_t lightingMgrOffset=0x4e8276a0,lightingMgrGotOffset=0x2de6d70; // rage::rmlLightingMgr::sm_CurrentMgr
inline constexpr std::uintptr_t resMgrOffset=0x4e85f020,resMgrGotOffset=0x2e10b48;         // g_resmgr (renResourceManager)
inline constexpr std::uintptr_t lightsMgrOffset=0x4e85e728,lightsMgrGotOffset=0x2e0a860;   // renLightPrePassLightsMgr::sm_Instance
inline constexpr std::uintptr_t currentViewportOffset=0x4e6e14d8,currentViewportGotOffset=0x2de0958; // grcViewport::sm_Current
// CCubeShadowMap / CShadowedPointLight / renResourceManager / renLightPrePassLightsMgr fields
inline constexpr std::uintptr_t cubeOfMgrField=0x20,cubeSlotsField=0x90,cubeCountField=0x98,cubePredField=0x30,cubeVolumeField=0x31,cubeVsdctField=0x158;
inline constexpr std::uintptr_t slotStride=0xd0,slotIdField=0x40,slotFadeField=0x44,slotTypeField=0x68,slotCubeRtField=0x88,slotSpotRtField=0x98,slotCascadeRtField=0xa0;
inline constexpr std::size_t slotKeyBytes=0x88;
inline constexpr std::uintptr_t resEntriesField=0x0,resCountField=0x8,resStride=0x178,resOwnerField=0x168;
inline constexpr std::uintptr_t lightsBigCountField=0x23e8,lightsSmallCountField=0x3bf0,lightsFlagsField=0xb9d;
inline constexpr std::size_t lightsFlagsBytes=13; // +0xb9d..+0xba9 (stencil/geometry, HQ allowed b9f/ba1, HQ off ba0, ...)
inline constexpr std::uintptr_t viewportCameraField=0x40;
inline constexpr unsigned slotCount=5,faceCount=6;
inline constexpr float cutM=5.f;
// POINT SHADOW CAP (sagRenderer SelectionParams, see the header)
inline constexpr std::uintptr_t rendererOffset=0x4e85f110,rendererGotOffset=0x2e01660;          // sagRenderer::sm_Instance
inline constexpr std::uintptr_t selectionParamsField=0x1fa0,capField=0x1fbc;
inline constexpr std::size_t selectionParamsWords=13,capWord=7;                                   // +0x1fa0..+0x1fd3, cap = word 7
inline constexpr std::array<std::uint32_t,selectionParamsWords> selectionParamsNative{
    0x3fa66666u,0x3f0ccccdu,0x3ee66666u,0x3e4ccccdu,0x3f4ccccdu,0x40400000u,0u,5u,0x3f99999au,0x3f666666u,0x3ccccccdu,0x41a00000u,0x3fb33333u};
inline constexpr std::int32_t capNative=5;
inline constexpr std::uintptr_t prepareReaderOffset=0x2a52764,selectCapOffset=0x2a4add8;
inline constexpr std::array<unsigned char,16> prepareReaderFingerprint{
#include "gamecode/LampShadows_2a52764_16_8d.inc"
}; // ldr q0,q1,q2,[x0,#0x1fa0/b0/c0]; ldr w10,[x0,#0x1fd0]
inline constexpr std::array<unsigned char,12> selectCapFingerprint{106,30,64,185,159,3,10,107,202,254,255,84};               // ldr w10,[x19,#0x1c]; cmp w28,w10; b.ge

// ---- levers / presets ----
inline constexpr unsigned presetCount=6,updateLevels=3,cullLevels=3;
inline constexpr unsigned capLevels=4,capOff=3; // 0 NATIVE (5), 1 CAP 2, 2 CAP 1, 3 OFF (cap 0 + slot clear)
// cap is the last member, so 3-member brace-inits keep their meaning (cap 0 = native)
struct Levers {unsigned update{};bool cascadeOff{};unsigned cull{};unsigned cap{};bool operator==(const Levers&) const = default;};
// Menu row LAMP SHADOWS: NATIVE / 1/2 / 1/2 NO CASCADE / 1/3 NO CASC CULL X2 / 1/3 CAP 2 / OFF.
inline constexpr Levers presetLevers(unsigned p) noexcept {
    return p==1?Levers{1,false,0}:p==2?Levers{1,true,0}:p==3?Levers{2,true,1}:p==4?Levers{2,true,1,1}:p==5?Levers{2,true,1,capOff}:Levers{0,false,0};}
inline constexpr const char* updateName(unsigned v) noexcept {return v==1?"1/2":v==2?"1/3":"EVERY";}
inline constexpr const char* cullName(unsigned v) noexcept {return v==1?"X2":v==2?"X4":"NATIVE";}
inline constexpr const char* capName(unsigned v) noexcept {return v==1?"CAP 2":v==2?"CAP 1":v==3?"OFF":"NATIVE";}
inline constexpr std::int32_t capValue(unsigned v) noexcept {return v==1?2:v==2?1:v==3?0:capNative;}
inline constexpr unsigned interval(unsigned update) noexcept {return update==1?2u:update==2?3u:1u;}
inline constexpr float cullScale(unsigned l) noexcept {return l==1?2.f:l==2?4.f:1.f;}
inline constexpr std::uint32_t cullBits(unsigned l) noexcept {return std::bit_cast<std::uint32_t>(std::bit_cast<float>(cubeThresholdNativeBits)*cullScale(l));}
// Properties (re-read every propertyInterval RenderScenes; -1 = absent/invalid): an individual lever property beats the
// preset property, which beats the menu row.
struct Properties {int preset{-1},update{-1},cascade{-1},cull{-1},cap{-1};};
inline constexpr unsigned propertyInterval=64;
inline Levers resolve(unsigned menuPreset,const Properties& p) noexcept {
    const unsigned preset=p.preset>=0&&p.preset<int(presetCount)?unsigned(p.preset):menuPreset<presetCount?menuPreset:0u;
    Levers l=presetLevers(preset);
    if(p.update>=0&&p.update<int(updateLevels))l.update=unsigned(p.update);
    if(p.cascade==0||p.cascade==1)l.cascadeOff=p.cascade==0;
    if(p.cull>=0&&p.cull<int(cullLevels))l.cull=unsigned(p.cull);
    if(p.cap>=0&&p.cap<int(capLevels))l.cap=unsigned(p.cap); // debug.rdrvr.lampcap (0 = native: kill switch of presets 4/5)
    return l;
}
inline bool anyProperty(const Properties& p) noexcept {return p.preset>=0||p.update>=0||p.cascade>=0||p.cull>=0||p.cap>=0;}

// ---- pure per-slot planner (render thread only; host-tested) ----
struct SlotView {
    std::uint32_t id{},type{};bool cascade{};
    std::array<std::uintptr_t,3> rt{};
    std::array<unsigned char,slotKeyBytes> key{}; // slot bytes +0x00..+0x87 with the +0x44 fade zeroed
};
enum Reason : unsigned {ReasonEvery,ReasonNoSnapshot,ReasonKey,ReasonScheduled,ReasonSkipped,ReasonCount};
inline constexpr const char* reasonNames[ReasonCount]{"every","noSnapshot","key","scheduled","skipped"};
struct Planner {
    struct Track {bool valid{};std::uint32_t id{},type{};std::array<std::uintptr_t,3> rt{};std::array<unsigned char,slotKeyBytes> key{};
                  std::uint8_t faces{};unsigned age{};};
    std::array<Track,slotCount> track{};
    std::array<unsigned char,slotCount> phase{};
    std::array<std::uint8_t,slotCount> newFaces{};
    std::array<unsigned,slotCount> reason{};
    std::array<bool,slotCount> active{},changed{};
    std::uint64_t frame{},setSignature{~0ull};
    unsigned period{1},count{};
    std::uint8_t skipMask{};
    std::uint64_t newFaceDraws{},faceSkips{},faceDraws{};
    void invalidateAll() noexcept {for(auto& t:track)t.valid=false;}
    static double weight(unsigned slot,const SlotView& v) noexcept {
        return v.type==1?(slot<2?.67:.2)+(v.cascade?.67:0.):6.*(slot<2?.085:.1);}
    // Greedy split of the active slots into `period` phases by estimated GPU cost (heaviest first into the lightest phase).
    void assignPhases(const std::array<SlotView,slotCount>& v) noexcept {
        std::array<unsigned,slotCount> order{};unsigned n{};
        for(unsigned j=0;j<count;++j)if(v[j].id)order[n++]=j;
        std::stable_sort(order.begin(),order.begin()+n,[&](unsigned a,unsigned b){return weight(a,v[a])>weight(b,v[b]);});
        std::array<double,3> load{};
        for(unsigned k=0;k<n;++k){unsigned best=0;for(unsigned p=1;p<period;++p)if(load[p]<load[best])best=p;
            phase[order[k]]=static_cast<unsigned char>(best);load[best]+=weight(order[k],v[order[k]]);}
    }
    // Before the native RenderIntoCubeShadowMap. invalidate: gap or camera cut (all snapshots dropped).
    void plan(const std::array<SlotView,slotCount>& v,unsigned slots,unsigned periodNow,bool invalidate) noexcept {
        ++frame;count=std::min(slots,slotCount);
        if(invalidate)invalidateAll();
        std::uint64_t sig=periodNow;
        for(unsigned j=0;j<count;++j)sig=sig*1000003ull+(std::uint64_t(v[j].id)<<3)+(std::uint64_t(v[j].type&3u)<<1)+(v[j].cascade?1u:0u);
        if(periodNow!=period||sig!=setSignature){period=std::clamp(periodNow,1u,3u);setSignature=sig;assignPhases(v);}
        const unsigned now=unsigned(frame%period);
        skipMask=0;
        for(unsigned j=0;j<slotCount;++j){newFaces[j]=0;active[j]=false;changed[j]=false;reason[j]=ReasonSkipped;}
        for(unsigned j=0;j<count;++j){
            const auto& s=v[j];auto& t=track[j];
            if(!s.id)continue; // the engine skips the slot; its RT stays untouched
            active[j]=true;
            changed[j]=!t.valid||t.id!=s.id||t.type!=s.type||t.rt!=s.rt||t.key!=s.key;
            if(period<=1)reason[j]=ReasonEvery;
            else if(!t.valid)reason[j]=ReasonNoSnapshot;
            else if(changed[j])reason[j]=ReasonKey;
            else if(phase[j]==now||t.age+1>=period)reason[j]=ReasonScheduled;
            else {reason[j]=ReasonSkipped;skipMask|=std::uint8_t(1u<<j);}
        }
    }
    // DrawShadow(face, slot) inside the exact render: false = skip (the face holds this light's map already).
    bool drawFace(unsigned slot,unsigned face) noexcept {
        if(slot>=slotCount||face>=8)return true;
        const auto bit=std::uint8_t(1u<<face);
        if(skipMask&(1u<<slot)){
            if(track[slot].faces&bit){++faceSkips;return false;}
            ++newFaceDraws; // a face that was never drawn for this light: draw it now
        }
        newFaces[slot]|=bit;++faceDraws;return true;
    }
    // After the native render.
    void finish(const std::array<SlotView,slotCount>& v) noexcept {
        for(unsigned j=0;j<count;++j){
            if(!active[j])continue;
            auto& t=track[j];const auto& s=v[j];
            if(!(skipMask&(1u<<j))){
                t.faces=changed[j]?newFaces[j]:std::uint8_t(t.faces|newFaces[j]);
                t.valid=true;t.id=s.id;t.type=s.type;t.rt=s.rt;t.key=s.key;t.age=0;
            }else{t.faces|=newFaces[j];if(t.age<(1u<<30))++t.age;}
        }
    }
};

// ---- engine memory helpers (32-bit words of engine .data) ----
inline std::uint32_t loadWord(std::uintptr_t address) noexcept {
#if defined(__cpp_lib_atomic_ref)
    return std::atomic_ref<std::uint32_t>(*reinterpret_cast<std::uint32_t*>(address)).load(std::memory_order_relaxed);
#else
    return __atomic_load_n(reinterpret_cast<std::uint32_t*>(address),__ATOMIC_RELAXED);
#endif
}
inline bool exchangeWord(std::uintptr_t address,std::uint32_t& expected,std::uint32_t desired) noexcept {
#if defined(__cpp_lib_atomic_ref)
    return std::atomic_ref<std::uint32_t>(*reinterpret_cast<std::uint32_t*>(address)).compare_exchange_strong(expected,desired,std::memory_order_relaxed);
#else
    return __atomic_compare_exchange_n(reinterpret_cast<std::uint32_t*>(address),&expected,desired,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED);
#endif
}
template<class T> inline T peek(std::uintptr_t address) noexcept {T v{};if(address)std::memcpy(&v,reinterpret_cast<const void*>(address),sizeof v);return v;}

// ---- runtime state ----
using DrawCube=void(*)(void*);
using RenderInto=void(*)(void*);
using DrawShadow=void(*)(void*,int,int,bool,bool,bool,void*);
inline DrawCube originalDrawCube{};
inline RenderInto originalRenderInto{};
inline DrawShadow originalDrawShadow{};
struct Config { // absolute addresses, written once before the GOT slots go live
    std::uintptr_t drawCubeReturn{},renderIntoReturn{},drawShadowReturn{};
    std::uintptr_t numUseCascade{},cubeThreshold{},lightingMgr{},resMgr{},lightsMgr{},currentViewport{};
    bool cascadeOk{},cullOk{},lightsOk{};
    std::uintptr_t renderer{};bool capOk{}; // &sagRenderer::sm_Instance, POINT SHADOW CAP proven (GOT + Prepare/SelectShadows code)
};
inline Config config;
inline std::atomic<const char*> capFingerprint{"unconfigured"}; // "ok", "got", "prepareCode", "selectCode"
inline std::atomic<bool> hooked{};                       // DrawCubeShadowMaps + RenderIntoCubeShadowMap + DrawShadow live
inline std::atomic<const char*> fingerprint{"unconfigured"}; // "ok" or the first failed check
inline std::atomic<std::uint64_t> sceneSerial{};         // RenderScene counter (sceneBegin)
inline std::atomic<unsigned> menuPreset{};               // LAMP SHADOWS row, latched per RenderScene
inline std::atomic<int> propPreset{-1},propUpdate{-1},propCascade{-1},propCull{-1},propCap{-1};
inline std::atomic<std::uint64_t> propertyReads{};
inline Properties properties() noexcept {
    return {propPreset.load(std::memory_order_relaxed),propUpdate.load(std::memory_order_relaxed),
            propCascade.load(std::memory_order_relaxed),propCull.load(std::memory_order_relaxed),propCap.load(std::memory_order_relaxed)};}
inline Levers requested() noexcept {return resolve(menuPreset.load(std::memory_order_relaxed),properties());}
inline bool propertyActive() noexcept {return anyProperty(properties());}
inline bool ready() noexcept {return hooked.load(std::memory_order_relaxed);}
#if defined(__ANDROID__)
inline int readDigitProperty(const char* name,int count) noexcept {
    char value[PROP_VALUE_MAX]{};const int n=__system_property_get(name,value);
    return n==1&&value[0]>='0'&&value[0]<char('0'+count)?value[0]-'0':-1;
}
#endif
// Every RenderScene (TraceEntry observeScene): the LAMP SHADOWS row; properties every 64 scenes (scene 1, 65, ...).
inline void sceneBegin(unsigned row) noexcept {
    const auto scene=sceneSerial.fetch_add(1,std::memory_order_relaxed)+1;
    menuPreset.store(row<presetCount?row:0u,std::memory_order_relaxed);
#if defined(__ANDROID__)
    if((scene-1)%propertyInterval==0){
        const int saved=errno;
        propPreset.store(readDigitProperty("debug.rdrvr.lampshadow",int(presetCount)),std::memory_order_relaxed);
        propUpdate.store(readDigitProperty("debug.rdrvr.lampupdate",int(updateLevels)),std::memory_order_relaxed);
        propCascade.store(readDigitProperty("debug.rdrvr.spotcascade",2),std::memory_order_relaxed);
        propCull.store(readDigitProperty("debug.rdrvr.lampcull",int(cullLevels)),std::memory_order_relaxed);
        propCap.store(readDigitProperty("debug.rdrvr.lampcap",int(capLevels)),std::memory_order_relaxed);
        errno=saved;propertyReads.fetch_add(1,std::memory_order_relaxed);
    }
#else
    (void)scene;
#endif
}

struct SlotStats {std::atomic<std::uint64_t> activeFrames{},rendered{},skipped{},facesDrawn{},facesSkipped{},casters{};
                  std::atomic<std::uint32_t> lastId{},lastType{},lastCascade{},lastFaces{};};
struct StateStats {std::atomic<std::uint64_t> frames{},cpuUsSum{},facesDrawn{},facesSkipped{},slotsRendered{},slotsSkipped{},casters{};};
inline constexpr unsigned stateCount=updateLevels*2*cullLevels;
inline constexpr unsigned stateIndex(unsigned update,bool cascadeOn,unsigned cull) noexcept {return (update*2u+(cascadeOn?1u:0u))*cullLevels+cull;}
struct Stats {
    std::atomic<std::uint64_t> drawCubeCalls{},drawCubeForeign{},renderIntoCalls{},renderIntoForeign{},drawShadowCalls{},drawShadowForeign{};
    std::atomic<std::uint64_t> gaps{},cuts{},disabledFrames{},slotsActive{},slotsRendered{},slotsSkipped{},facesDrawn{},facesSkipped{},newFaceDraws{};
    std::array<std::atomic<std::uint64_t>,ReasonCount> reasons{};
    std::atomic<std::uint64_t> cpuUsSum{},cpuUsMax{},drawCubeUsSum{};
    std::atomic<const char*> disabledReason{nullptr};
    std::atomic<int> vsdct{-1},pred{-1},volume{-1},slotCountSeen{-1},resOwner{-1};
    std::array<SlotStats,slotCount> slots{};
    std::array<StateStats,stateCount> byState{};
    // spot cascade (transient) and cull (persistent) writes
    std::atomic<std::uint64_t> cascadeWrites{},cascadeRestores{},cascadeRestoreMisses{},cascadeForeign{};
    std::atomic<int> cascadeSeen{-1};std::atomic<unsigned> cascadeOnEffective{1};
    std::atomic<std::uint64_t> cullWrites{},cullRestores{},cullForeign{};std::atomic<bool> cullSuspended{};std::atomic<unsigned> cullEffective{};
    std::atomic<std::uint32_t> cullApplied{};
    // light manager readout (previous frame's RenderLights, read at DrawCubeShadowMaps time)
    std::atomic<std::uint64_t> lightSamples{},bigLightsSum{},smallLightsSum{},shadowedSum{};
    std::atomic<std::uint32_t> bigLightsMax{},smallLightsMax{};
    std::array<std::atomic<std::uint32_t>,lightsFlagsBytes> lightFlags{};
    // POINT SHADOW CAP (applyCap, persistent compare-and-restore) and the OFF slot clear (renderInto)
    std::atomic<std::uint64_t> capCalls{},capWrites{},capRestores{},capForeign{},capBlockMismatch{},capNoRenderer{};
    std::atomic<bool> capSuspended{};std::atomic<unsigned> capEffective{};std::atomic<std::int32_t> capApplied{-1};
    std::atomic<int> capMismatchWord{-1};std::atomic<std::uint32_t> capMismatchValue{};
    std::atomic<int> capLastFail{};  // Last applyCap: 0 ok (or capOk false), 1 no renderer, 3 SelectionParams mismatch
    std::atomic<std::uint64_t> offFrames{},offClears{},offRefused{};std::atomic<bool> offEffective{};
    std::array<std::atomic<std::uint64_t>,capLevels> capFrames{},capShadowedSum{},capFacesDrawn{},capRicUsSum{}; // by capDigit()
};
inline Stats stats;
inline Planner planner;                         // render thread only
inline std::array<SlotView,slotCount> views{};  // render thread only (this frame's slots)
inline std::uint64_t lastRenderIntoScene{};     // render thread only
inline bool lastPoseValid{};inline float lastPose[3]{};
inline std::uint32_t cullLastWritten=cubeThresholdNativeBits; // render thread only
inline std::atomic<bool> insideRender{},insideFace{};
inline std::atomic<unsigned> currentSlot{};
inline thread_local char threadToken{};
inline std::atomic<const void*> renderThread{};
inline bool onRenderThread() noexcept {return renderThread.load(std::memory_order_relaxed)==&threadToken;}
inline void storeMax(std::atomic<std::uint64_t>& a,std::uint64_t v) noexcept {if(v>a.load(std::memory_order_relaxed))a.store(v,std::memory_order_relaxed);}
inline void storeMax32(std::atomic<std::uint32_t>& a,std::uint32_t v) noexcept {if(v>a.load(std::memory_order_relaxed))a.store(v,std::memory_order_relaxed);}
inline std::uint64_t nowUs() noexcept {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
// Effective levers (signature _ls<update><cascade 1 on/0 off><cull> and report): what really acted last frame.
// Plus a leading cap digit when the cap acted (1 CAP 2, 2 CAP 1, 3 OFF): _ls1201 / _ls3201; NATIVE keeps _ls201 (GameXr %03u).
inline std::atomic<unsigned> updateEffective{};
inline unsigned capDigit() noexcept {return stats.offEffective.load(std::memory_order_relaxed)?capOff:stats.capEffective.load(std::memory_order_relaxed);}
inline unsigned signatureCode() noexcept {
    return capDigit()*1000u+updateEffective.load(std::memory_order_relaxed)*100u+stats.cascadeOnEffective.load(std::memory_order_relaxed)*10u+
           stats.cullEffective.load(std::memory_order_relaxed);}
// runtime::lampCapMenu (VisibleTrims.h): -1 not configured, 0 cap unavailable (install proof, or the last write attempt
// found a changed SelectionParams block), 1 usable, 2 suspended by a foreign value.
inline int capMenuState() noexcept {
    const char* f=capFingerprint.load(std::memory_order_relaxed);
    if(f&&!std::strcmp(f,"unconfigured"))return -1;
    const int fail=stats.capLastFail.load(std::memory_order_relaxed);
    return !config.capOk||fail>=2?0:stats.capSuspended.load(std::memory_order_relaxed)?2:1;}

// ---- install-time proof (TraceEntry): code = exec fingerprint check, read = guarded memory read ----
template<class Code,class Read> bool configure(std::uintptr_t base,Code code,Read read) {
    const auto got=[&](std::uintptr_t slot,std::uintptr_t target){std::uintptr_t v{};return read(nullptr,base+slot,&v,sizeof v)&&v==base+target;};
    const auto word=[&](std::uintptr_t offset,std::uint32_t want){std::uint32_t v{};return read(nullptr,base+offset,&v,sizeof v)&&v==want;};
    const char* result="ok";
    if(!code(drawCubeOffset,drawCubeFingerprint)||!code(renderIntoOffset,renderIntoFingerprint)||!code(drawShadowOffset,drawShadowFingerprint))result="code";
    else if(!code(drawCubeReturnOffset-4,drawCubeCallFingerprint)||!code(renderIntoReturnOffset-4,renderIntoCallFingerprint)||
            !code(drawShadowReturnOffset-4,drawShadowCallFingerprint)||!code(slotEnableLoadOffset,slotEnableLoadFingerprint))result="call";
    else if(!got(drawCubeGotOffset,drawCubeOffset)||!got(renderIntoGotOffset,renderIntoOffset)||!got(drawShadowGotOffset,drawShadowOffset))result="got";
    else if(!got(lightingMgrGotOffset,lightingMgrOffset)||!got(resMgrGotOffset,resMgrOffset)||!got(currentViewportGotOffset,currentViewportOffset))result="globals";
    config={base+drawCubeReturnOffset,base+renderIntoReturnOffset,base+drawShadowReturnOffset,base+numUseCascadeOffset,base+cubeThresholdOffset,
            base+lightingMgrOffset,base+resMgrOffset,base+lightsMgrOffset,base+currentViewportOffset,false,false,false};
    config.cascadeOk=got(numUseCascadeGotOffset,numUseCascadeOffset)&&code(cascadeReaderOffset,cascadeReaderFingerprint)&&
        code(cascadeLoadOffset,cascadeLoadFingerprint)&&word(cascadeFovOffset,cascadeFovBits);
    config.cullOk=got(cubeThresholdGotOffset,cubeThresholdOffset)&&code(cubeThresholdCodeOffset,cubeThresholdCodeFingerprint)&&
        word(cubeThresholdOffset,cubeThresholdNativeBits);
    config.lightsOk=got(lightsMgrGotOffset,lightsMgrOffset);
    { // POINT SHADOW CAP: GOT -> sm_Instance, the Prepare block copy and the SelectShadows point-cap compare in place.
      // The SelectionParams values are checked at every write (the renderer is created after install).
        const char* cap=!got(rendererGotOffset,rendererOffset)?"got":!code(prepareReaderOffset,prepareReaderFingerprint)?"prepareCode":
                        !code(selectCapOffset,selectCapFingerprint)?"selectCode":"ok";
        config.renderer=base+rendererOffset;config.capOk=cap[0]=='o'&&cap[1]=='k';capFingerprint.store(cap,std::memory_order_relaxed);
    }
    fingerprint.store(result,std::memory_order_relaxed);
    return result[0]=='o';
}

// ---- levers 2 and 3 ----
// Persistent compare-and-restore of the cube caster threshold: written only while it holds the native value
// or the mod's last write; anything else is foreign -> suspended (the mod's value goes back to native only
// while it is still in place).
inline unsigned applyCull(unsigned level) noexcept {
    auto& s=stats;if(!config.cullOk){s.cullEffective.store(0,std::memory_order_relaxed);return 0;}
    if(level>=cullLevels)level=0;
    const std::uint32_t want=cullBits(level);std::uint32_t now=loadWord(config.cubeThreshold);
    if(now!=cubeThresholdNativeBits&&now!=cullLastWritten&&now!=want){
        if(!s.cullSuspended.exchange(true,std::memory_order_relaxed))s.cullForeign.fetch_add(1,std::memory_order_relaxed);
        s.cullApplied.store(now,std::memory_order_relaxed);s.cullEffective.store(0,std::memory_order_relaxed);return 0;
    }
    s.cullSuspended.store(false,std::memory_order_relaxed);
    if(now!=want){std::uint32_t expected=now;
        if(exchangeWord(config.cubeThreshold,expected,want)){cullLastWritten=want;now=want;(level?s.cullWrites:s.cullRestores).fetch_add(1,std::memory_order_relaxed);}
        else now=expected;}
    s.cullApplied.store(now,std::memory_order_relaxed);
    const unsigned effective=now==want?level:0;s.cullEffective.store(effective,std::memory_order_relaxed);return effective;
}
// POINT SHADOW CAP: int renderer+0x1fbc (native 5), persistent compare-and-restore exactly like applyCull:
// written only while it holds the native value or the mod's last write, anything else is foreign ->
// suspended. The renderer is *sm_Instance (the object Prepare runs on, see the header; no `self` check,
// DrawCubeShadowMaps' x0 is not the renderer); the SelectionParams block (cap word excepted) must hold
// the ctor values.
// Read by the main thread's next PrepareCubeShadowVisibilityRequests (one frame later). Returns the cap level in force.
inline std::int32_t capLastWritten=capNative; // render thread only
inline unsigned applyCap(unsigned level) noexcept {
    auto& s=stats;s.capCalls.fetch_add(1,std::memory_order_relaxed);
    if(level>=capLevels)level=0;
    const auto none=[&]{s.capEffective.store(0,std::memory_order_relaxed);return 0u;};
    if(!config.capOk)return none();
    const auto renderer=peek<std::uintptr_t>(config.renderer);
    if(!renderer){s.capNoRenderer.fetch_add(1,std::memory_order_relaxed);s.capLastFail.store(1,std::memory_order_relaxed);return none();}
    std::array<std::uint32_t,selectionParamsWords> w{};std::memcpy(w.data(),reinterpret_cast<const void*>(renderer+selectionParamsField),sizeof w);
    for(unsigned i=0;i<selectionParamsWords;++i)if(i!=capWord&&w[i]!=selectionParamsNative[i]){
        s.capBlockMismatch.fetch_add(1,std::memory_order_relaxed);s.capMismatchWord.store(int(i),std::memory_order_relaxed);
        s.capMismatchValue.store(w[i],std::memory_order_relaxed);s.capLastFail.store(3,std::memory_order_relaxed);return none();}
    s.capLastFail.store(0,std::memory_order_relaxed);
    const std::int32_t want=capValue(level);std::int32_t now=std::int32_t(loadWord(renderer+capField));
    if(now!=capNative&&now!=capLastWritten&&now!=want){
        if(!s.capSuspended.exchange(true,std::memory_order_relaxed))s.capForeign.fetch_add(1,std::memory_order_relaxed);
        s.capApplied.store(now,std::memory_order_relaxed);return none();
    }
    s.capSuspended.store(false,std::memory_order_relaxed);
    if(now!=want){std::uint32_t expected=std::uint32_t(now);
        if(exchangeWord(renderer+capField,expected,std::uint32_t(want))){capLastWritten=want;now=want;(level?s.capWrites:s.capRestores).fetch_add(1,std::memory_order_relaxed);}
        else now=std::int32_t(expected);}
    s.capApplied.store(now,std::memory_order_relaxed);
    const unsigned effective=now==want?level:0;s.capEffective.store(effective,std::memory_order_relaxed);return effective;
}
// OFF: zero +0x40 of every enabled slot of the proven slot table (render thread, inside the exact RenderIntoCubeShadowMap,
// before the native call and before the planner reads the table). Returns the slots cleared.
inline unsigned clearSlots(std::uintptr_t cube,unsigned n) noexcept {
    const auto slots=peek<std::uintptr_t>(cube+cubeSlotsField);if(!slots)return 0;
    unsigned cleared{};
    for(unsigned j=0;j<n&&j<slotCount;++j){const auto a=slots+j*slotStride+slotIdField;std::uint32_t id=loadWord(a);
        if(id&&exchangeWord(a,id,0))++cleared;}
    return cleared;
}
struct CascadeScope { // NumUseCascade = 0 for the duration of the exact DrawCubeShadowMaps (spot cascade OFF)
    bool wrote{};std::uint32_t saved{};
    explicit CascadeScope(bool off) noexcept {
        auto& s=stats;if(!config.cascadeOk){s.cascadeOnEffective.store(1,std::memory_order_relaxed);return;}
        std::uint32_t v=loadWord(config.numUseCascade);s.cascadeSeen.store(int(v),std::memory_order_relaxed);
        if(v!=numUseCascadeNative)s.cascadeForeign.fetch_add(1,std::memory_order_relaxed); // a script changed it: still honoured when ON
        if(off&&v){std::uint32_t expected=v;
            if(exchangeWord(config.numUseCascade,expected,0)){wrote=true;saved=v;s.cascadeWrites.fetch_add(1,std::memory_order_relaxed);}
            else v=expected;}
        s.cascadeOnEffective.store(wrote||!v?0u:1u,std::memory_order_relaxed);
    }
    ~CascadeScope(){
        if(!wrote)return;std::uint32_t expected=0;
        if(exchangeWord(config.numUseCascade,expected,saved))stats.cascadeRestores.fetch_add(1,std::memory_order_relaxed);
        else stats.cascadeRestoreMisses.fetch_add(1,std::memory_order_relaxed); // a foreign write meanwhile: that value stays
    }
};

// ---- readout ----
inline std::uintptr_t cubeMap() noexcept {
    const auto mgr=peek<std::uintptr_t>(config.lightingMgr);return mgr?peek<std::uintptr_t>(mgr+cubeOfMgrField):0;}
inline unsigned readSlots(std::uintptr_t cube,std::array<SlotView,slotCount>& out) noexcept {
    out={};if(!cube)return 0;
    const auto slots=peek<std::uintptr_t>(cube+cubeSlotsField);const unsigned n=std::min<unsigned>(peek<std::uint16_t>(cube+cubeCountField),slotCount);
    if(!slots)return 0;
    for(unsigned j=0;j<n;++j){
        const auto s=slots+j*slotStride;auto& v=out[j];
        std::memcpy(v.key.data(),reinterpret_cast<const void*>(s),slotKeyBytes);
        std::memset(v.key.data()+slotFadeField,0,4);
        v.id=peek<std::uint32_t>(s+slotIdField);v.type=peek<std::uint32_t>(s+slotTypeField);
        v.rt={peek<std::uintptr_t>(s+slotCubeRtField),peek<std::uintptr_t>(s+slotSpotRtField),peek<std::uintptr_t>(s+slotCascadeRtField)};
        v.cascade=v.rt[2]!=0;
    }
    return n;
}
inline void readLights() noexcept {
    auto& s=stats;if(!config.lightsOk)return;
    const auto mgr=peek<std::uintptr_t>(config.lightsMgr);if(!mgr)return;
    const auto big=peek<std::int32_t>(mgr+lightsBigCountField),small=peek<std::int32_t>(mgr+lightsSmallCountField);
    if(big<0||big>768||small<0||small>768)return;
    s.lightSamples.fetch_add(1,std::memory_order_relaxed);s.bigLightsSum.fetch_add(unsigned(big),std::memory_order_relaxed);
    s.smallLightsSum.fetch_add(unsigned(small),std::memory_order_relaxed);
    storeMax32(s.bigLightsMax,unsigned(big));storeMax32(s.smallLightsMax,unsigned(small));
    std::array<unsigned char,lightsFlagsBytes> f{};std::memcpy(f.data(),reinterpret_cast<const void*>(mgr+lightsFlagsField),f.size());
    for(unsigned i=0;i<lightsFlagsBytes;++i)s.lightFlags[i].store(f[i],std::memory_order_relaxed);
}
inline int resourceOwner() noexcept {
    const auto entries=peek<std::uintptr_t>(config.resMgr+resEntriesField);const auto n=peek<std::uint16_t>(config.resMgr+resCountField);
    return entries&&n?int(peek<std::uint32_t>(entries+resOwnerField)):-1;}
inline bool cameraCut() noexcept {
    const auto vp=peek<std::uintptr_t>(config.currentViewport);if(!vp)return false;
    float m[16]{};std::memcpy(m,reinterpret_cast<const void*>(vp+viewportCameraField),sizeof m);
    const float p[3]{m[12],m[13],m[14]};
    if(!std::isfinite(p[0])||!std::isfinite(p[1])||!std::isfinite(p[2])){lastPoseValid=false;return true;}
    const bool cut=lastPoseValid&&std::sqrt((p[0]-lastPose[0])*(p[0]-lastPose[0])+(p[1]-lastPose[1])*(p[1]-lastPose[1])+(p[2]-lastPose[2])*(p[2]-lastPose[2]))>cutM;
    std::memcpy(lastPose,p,sizeof p);lastPoseValid=true;return cut;
}

// ---- GOT replacements (TraceEntry passes the return address) ----
inline void drawCube(void* self,std::uintptr_t caller) noexcept {
    auto& s=stats;s.drawCubeCalls.fetch_add(1,std::memory_order_relaxed);
    if(caller!=config.drawCubeReturn||!hooked.load(std::memory_order_relaxed)){s.drawCubeForeign.fetch_add(1,std::memory_order_relaxed);originalDrawCube(self);return;}
    renderThread.store(&threadToken,std::memory_order_relaxed);
    const Levers l=requested();
    applyCull(l.cull); // next frame's cube visibility build (main thread)
    applyCap(l.cap); // Next frame's SelectShadows (main thread); renderer = *sm_Instance (self is only passed through)
    if(l.cap!=capOff)s.offEffective.store(false,std::memory_order_relaxed); // renderInto sets it while OFF clears
    const auto t0=nowUs();
    {CascadeScope cascade(l.cascadeOff);originalDrawCube(self);}
    const auto t1=nowUs();s.drawCubeUsSum.fetch_add(t1>t0?t1-t0:0,std::memory_order_relaxed);
    // readout after the native call: slot table as the receivers will use it (+0x40 cleared for empty caster lists)
    std::array<SlotView,slotCount> after{};const unsigned n=readSlots(cubeMap(),after);
    unsigned shadowed{};
    for(unsigned j=0;j<n;++j){auto& st=s.slots[j];const auto& v=after[j];
        st.lastId.store(v.id,std::memory_order_relaxed);st.lastType.store(v.type,std::memory_order_relaxed);st.lastCascade.store(v.cascade?1u:0u,std::memory_order_relaxed);
        if(v.id)++shadowed;}
    s.shadowedSum.fetch_add(shadowed,std::memory_order_relaxed);
    {const unsigned cd=capDigit();s.capFrames[cd].fetch_add(1,std::memory_order_relaxed);s.capShadowedSum[cd].fetch_add(shadowed,std::memory_order_relaxed);}
    s.resOwner.store(resourceOwner(),std::memory_order_relaxed);
    readLights();
}
inline void renderInto(void* cube,std::uintptr_t caller) noexcept {
    auto& s=stats;s.renderIntoCalls.fetch_add(1,std::memory_order_relaxed);
    if(caller!=config.renderIntoReturn||!onRenderThread()){ // any other render may overwrite the maps
        s.renderIntoForeign.fetch_add(1,std::memory_order_relaxed);planner.invalidateAll();originalRenderInto(cube);return;}
    const auto c=reinterpret_cast<std::uintptr_t>(cube);
    const Levers l=requested();
    if(l.cap==capOff){ // OFF: no lamp shadows (spots too, and points while a foreign value suspends the cap)
        const unsigned count=std::min<unsigned>(peek<std::uint16_t>(c+cubeCountField),slotCount);
        if(count==slotCount&&c==cubeMap()){
            s.offClears.fetch_add(clearSlots(c,count),std::memory_order_relaxed);s.offFrames.fetch_add(1,std::memory_order_relaxed);
            s.offEffective.store(true,std::memory_order_relaxed);}
        else{s.offRefused.fetch_add(1,std::memory_order_relaxed);s.offEffective.store(false,std::memory_order_relaxed);}
    }
    const unsigned n=readSlots(c,views);
    const int vsdct=peek<std::uint8_t>(c+cubeVsdctField),pred=peek<std::uint8_t>(c+cubePredField),volume=peek<std::uint8_t>(c+cubeVolumeField);
    s.vsdct.store(vsdct,std::memory_order_relaxed);s.pred.store(pred,std::memory_order_relaxed);s.volume.store(volume,std::memory_order_relaxed);
    s.slotCountSeen.store(int(peek<std::uint16_t>(c+cubeCountField)),std::memory_order_relaxed);
    const char* disabled=vsdct?"vsdct":pred?"predication":volume?"casterVolume":n!=slotCount||c!=cubeMap()?"slots":nullptr;
    unsigned update=l.update;
    if(disabled){update=0;s.disabledFrames.fetch_add(1,std::memory_order_relaxed);s.disabledReason.store(disabled,std::memory_order_relaxed);}
    const auto scene=sceneSerial.load(std::memory_order_relaxed);
    const bool gap=lastRenderIntoScene&&lastRenderIntoScene+1!=scene;lastRenderIntoScene=scene;
    const bool cut=cameraCut();
    if(gap)s.gaps.fetch_add(1,std::memory_order_relaxed);
    if(cut)s.cuts.fetch_add(1,std::memory_order_relaxed);
    planner.plan(views,n,interval(update),gap||cut||disabled);
    updateEffective.store(update,std::memory_order_relaxed);
    const auto before=planner.faceDraws,beforeSkips=planner.faceSkips,beforeNew=planner.newFaceDraws;
    std::array<std::uint64_t,slotCount> castersBefore{};for(unsigned j=0;j<slotCount;++j)castersBefore[j]=s.slots[j].casters.load(std::memory_order_relaxed);
    const auto t0=nowUs();
    insideRender.store(true,std::memory_order_relaxed);
    originalRenderInto(cube);
    insideRender.store(false,std::memory_order_relaxed);
    const auto t1=nowUs();const auto us=t1>t0?t1-t0:0;
    planner.finish(views);
    unsigned active{},rendered{},skipped{};std::uint64_t casters{};
    for(unsigned j=0;j<n;++j){
        if(!planner.active[j])continue;++active;auto& st=s.slots[j];st.activeFrames.fetch_add(1,std::memory_order_relaxed);
        const unsigned r=planner.reason[j];s.reasons[r<ReasonCount?r:0].fetch_add(1,std::memory_order_relaxed);
        if(r==ReasonSkipped){++skipped;st.skipped.fetch_add(1,std::memory_order_relaxed);}else{++rendered;st.rendered.fetch_add(1,std::memory_order_relaxed);}
        st.lastFaces.store(planner.track[j].faces,std::memory_order_relaxed);
        casters+=st.casters.load(std::memory_order_relaxed)-castersBefore[j];
    }
    const auto drawn=planner.faceDraws-before,skippedFaces=planner.faceSkips-beforeSkips;
    s.slotsActive.fetch_add(active,std::memory_order_relaxed);s.slotsRendered.fetch_add(rendered,std::memory_order_relaxed);s.slotsSkipped.fetch_add(skipped,std::memory_order_relaxed);
    s.facesDrawn.fetch_add(drawn,std::memory_order_relaxed);s.facesSkipped.fetch_add(skippedFaces,std::memory_order_relaxed);
    s.newFaceDraws.fetch_add(planner.newFaceDraws-beforeNew,std::memory_order_relaxed);
    s.cpuUsSum.fetch_add(us,std::memory_order_relaxed);storeMax(s.cpuUsMax,us);
    {const unsigned cd=capDigit();s.capFacesDrawn[cd].fetch_add(drawn,std::memory_order_relaxed);s.capRicUsSum[cd].fetch_add(us,std::memory_order_relaxed);}
    auto& b=s.byState[stateIndex(update,s.cascadeOnEffective.load(std::memory_order_relaxed)!=0,s.cullEffective.load(std::memory_order_relaxed))];
    b.frames.fetch_add(1,std::memory_order_relaxed);b.cpuUsSum.fetch_add(us,std::memory_order_relaxed);b.facesDrawn.fetch_add(drawn,std::memory_order_relaxed);
    b.facesSkipped.fetch_add(skippedFaces,std::memory_order_relaxed);b.slotsRendered.fetch_add(rendered,std::memory_order_relaxed);
    b.slotsSkipped.fetch_add(skipped,std::memory_order_relaxed);b.casters.fetch_add(casters,std::memory_order_relaxed);
}
inline void drawShadow(void* self,int face,int slot,bool a,bool b,bool c,void* viewport,std::uintptr_t caller) noexcept {
    auto& s=stats;s.drawShadowCalls.fetch_add(1,std::memory_order_relaxed);
    const bool exact=caller==config.drawShadowReturn&&insideRender.load(std::memory_order_relaxed)&&onRenderThread()&&slot>=0&&slot<int(slotCount)&&face>=0&&face<int(faceCount);
    if(!exact){s.drawShadowForeign.fetch_add(1,std::memory_order_relaxed);originalDrawShadow(self,face,slot,a,b,c,viewport);return;}
    auto& st=s.slots[unsigned(slot)];
    if(!planner.drawFace(unsigned(slot),unsigned(face))){st.facesSkipped.fetch_add(1,std::memory_order_relaxed);return;}
    st.facesDrawn.fetch_add(1,std::memory_order_relaxed);
    currentSlot.store(unsigned(slot),std::memory_order_relaxed);insideFace.store(true,std::memory_order_relaxed);
    originalDrawShadow(self,face,slot,a,b,c,viewport);
    insideFace.store(false,std::memory_order_relaxed);
}
// DrawCommon hook (every thread): one relaxed load unless a lamp face is being drawn.
inline void casterDraw() noexcept {
    if(!insideFace.load(std::memory_order_relaxed)||!onRenderThread())return;
    const unsigned j=currentSlot.load(std::memory_order_relaxed);if(j<slotCount)stats.slots[j].casters.fetch_add(1,std::memory_order_relaxed);
}

inline std::string report() {
    const auto& s=stats;std::ostringstream o;
    const auto r=requested();const auto p=properties();
    const auto mean=[](std::uint64_t sum,std::uint64_t n){return n?double(sum)/double(n):-1.;};
    char sig[16];std::snprintf(sig,sizeof sig,"%03u",signatureCode()); // As GameXr's _ls%03u
    const auto ri=s.renderIntoCalls.load()-s.renderIntoForeign.load();
    o<<"\"localShadow\":{\"hooked\":"<<(hooked.load()?1:0)<<",\"fingerprint\":\""<<fingerprint.load()<<'"'
     <<",\"menuPreset\":"<<menuPreset.load()<<",\"properties\":{\"lampshadow\":"<<p.preset<<",\"lampupdate\":"<<p.update<<",\"spotcascade\":"<<p.cascade
     <<",\"lampcull\":"<<p.cull<<",\"lampcap\":"<<p.cap<<",\"reads\":"<<propertyReads.load()<<'}'
     <<",\"requested\":{\"update\":\""<<updateName(r.update)<<"\",\"cascade\":\""<<(r.cascadeOff?"OFF":"ON")<<"\",\"cull\":\""<<cullName(r.cull)
     <<"\",\"cap\":\""<<capName(r.cap)<<"\"}"
     <<",\"effective\":{\"update\":\""<<updateName(updateEffective.load())<<"\",\"cascadeOn\":"<<s.cascadeOnEffective.load()<<",\"cull\":"<<s.cullEffective.load()
     <<",\"cap\":"<<capDigit()<<",\"signature\":\"_ls"<<sig<<"\"}"
     <<",\"preconditions\":{\"vsdct\":"<<s.vsdct.load()<<",\"pred\":"<<s.pred.load()<<",\"volume\":"<<s.volume.load()<<",\"slots\":"<<s.slotCountSeen.load()
     <<",\"disabledFrames\":"<<s.disabledFrames.load()<<",\"disabledReason\":\""<<(s.disabledReason.load()?s.disabledReason.load():"")<<"\"}"
     <<",\"calls\":{\"drawCube\":"<<s.drawCubeCalls.load()<<",\"drawCubeForeign\":"<<s.drawCubeForeign.load()<<",\"renderInto\":"<<s.renderIntoCalls.load()
     <<",\"renderIntoForeign\":"<<s.renderIntoForeign.load()<<",\"drawShadow\":"<<s.drawShadowCalls.load()<<",\"drawShadowForeign\":"<<s.drawShadowForeign.load()<<'}'
     <<",\"frames\":"<<ri<<",\"gaps\":"<<s.gaps.load()<<",\"cuts\":"<<s.cuts.load()
     <<",\"slotsActivePerFrame\":"<<mean(s.slotsActive.load(),ri)<<",\"slotsRendered\":"<<s.slotsRendered.load()<<",\"slotsSkipped\":"<<s.slotsSkipped.load()
     <<",\"facesDrawn\":"<<s.facesDrawn.load()<<",\"facesSkipped\":"<<s.facesSkipped.load()<<",\"newFaceDraws\":"<<s.newFaceDraws.load()
     <<",\"facesDrawnPerFrame\":"<<mean(s.facesDrawn.load(),ri)
     <<",\"ricCpuMs\":{\"mean\":"<<mean(s.cpuUsSum.load(),ri)/1000.<<",\"max\":"<<double(s.cpuUsMax.load())/1000.<<'}'
     <<",\"drawCubeCpuMsMean\":"<<mean(s.drawCubeUsSum.load(),s.drawCubeCalls.load()-s.drawCubeForeign.load())/1000.;
    o<<",\"reasons\":{";for(unsigned i=0;i<ReasonCount;++i)o<<(i?",":"")<<'"'<<reasonNames[i]<<"\":"<<s.reasons[i].load();
    o<<"},\"slots\":[";
    for(unsigned j=0;j<slotCount;++j){const auto& st=s.slots[j];const auto af=st.activeFrames.load();
        o<<(j?",":"")<<"{\"slot\":"<<j<<",\"id\":"<<st.lastId.load()<<",\"type\":"<<st.lastType.load()<<",\"cascade\":"<<st.lastCascade.load()
         <<",\"faceMask\":"<<st.lastFaces.load()<<",\"activeFrames\":"<<af<<",\"rendered\":"<<st.rendered.load()<<",\"skipped\":"<<st.skipped.load()
         <<",\"facesDrawn\":"<<st.facesDrawn.load()<<",\"facesSkipped\":"<<st.facesSkipped.load()
         <<",\"castersPerFace\":"<<mean(st.casters.load(),st.facesDrawn.load())<<'}';}
    o<<"],\"byState\":{";bool first=true;
    for(unsigned u=0;u<updateLevels;++u)for(unsigned cs=0;cs<2;++cs)for(unsigned k=0;k<cullLevels;++k){
        const auto& b=s.byState[stateIndex(u,cs!=0,k)];const auto f=b.frames.load();if(!f)continue;
        o<<(first?"":",")<<"\"_ls"<<u<<cs<<k<<"\":{\"frames\":"<<f<<",\"ricCpuMsPerFrame\":"<<mean(b.cpuUsSum.load(),f)/1000.
         <<",\"facesDrawnPerFrame\":"<<mean(b.facesDrawn.load(),f)<<",\"facesSkippedPerFrame\":"<<mean(b.facesSkipped.load(),f)
         <<",\"slotsRendered\":"<<b.slotsRendered.load()<<",\"slotsSkipped\":"<<b.slotsSkipped.load()<<",\"castersPerFrame\":"<<mean(b.casters.load(),f)<<'}';first=false;}
    const auto ls=s.lightSamples.load();
    o<<"},\"cascade\":{\"ok\":"<<(config.cascadeOk?1:0)<<",\"seen\":"<<s.cascadeSeen.load()<<",\"writes\":"<<s.cascadeWrites.load()<<",\"restores\":"<<s.cascadeRestores.load()
     <<",\"restoreMisses\":"<<s.cascadeRestoreMisses.load()<<",\"foreign\":"<<s.cascadeForeign.load()<<'}'
     <<",\"cull\":{\"ok\":"<<(config.cullOk?1:0)<<",\"applied\":"<<std::bit_cast<float>(s.cullApplied.load()?s.cullApplied.load():cubeThresholdNativeBits)
     <<",\"writes\":"<<s.cullWrites.load()<<",\"restores\":"<<s.cullRestores.load()<<",\"foreign\":"<<s.cullForeign.load()<<",\"suspended\":"<<(s.cullSuspended.load()?1:0)<<'}'
     // POINT SHADOW CAP + OFF slot clear; capByLevel = per frame by the cap digit in force (shadowed = slots with a map)
     <<",\"cap\":{\"ok\":"<<(config.capOk?1:0)<<",\"fingerprint\":\""<<capFingerprint.load()<<"\",\"menu\":"<<capMenuState()
     <<",\"requested\":"<<r.cap<<",\"want\":"<<capValue(r.cap)<<",\"effective\":"<<s.capEffective.load()<<",\"applied\":"<<s.capApplied.load()
     <<",\"calls\":"<<s.capCalls.load()<<",\"writes\":"<<s.capWrites.load()<<",\"restores\":"<<s.capRestores.load()<<",\"foreign\":"<<s.capForeign.load()
     <<",\"suspended\":"<<(s.capSuspended.load()?1:0)<<",\"noRenderer\":"<<s.capNoRenderer.load()
     <<",\"blockMismatch\":"<<s.capBlockMismatch.load()<<",\"mismatchWord\":"<<s.capMismatchWord.load()<<",\"mismatchValue\":"<<s.capMismatchValue.load()
     <<",\"lastFail\":"<<s.capLastFail.load()<<'}'
     <<",\"off\":{\"effective\":"<<(s.offEffective.load()?1:0)<<",\"frames\":"<<s.offFrames.load()<<",\"slotsCleared\":"<<s.offClears.load()
     <<",\"refused\":"<<s.offRefused.load()<<'}'
     <<",\"capByLevel\":{";
    for(unsigned k=0;k<capLevels;++k){const auto f=s.capFrames[k].load();
        o<<(k?",":"")<<'"'<<capName(k)<<"\":{\"frames\":"<<f<<",\"shadowedPerFrame\":"<<mean(s.capShadowedSum[k].load(),f)
         <<",\"facesDrawnPerFrame\":"<<mean(s.capFacesDrawn[k].load(),f)<<",\"ricCpuMsPerFrame\":"<<mean(s.capRicUsSum[k].load(),f)/1000.<<'}';}
    o<<'}'
     <<",\"resourceOwner\":"<<s.resOwner.load()
     <<",\"lights\":{\"ok\":"<<(config.lightsOk?1:0)<<",\"samples\":"<<ls<<",\"bigMean\":"<<mean(s.bigLightsSum.load(),ls)<<",\"bigMax\":"<<s.bigLightsMax.load()
     <<",\"smallMean\":"<<mean(s.smallLightsSum.load(),ls)<<",\"smallMax\":"<<s.smallLightsMax.load()
     <<",\"shadowedMean\":"<<mean(s.shadowedSum.load(),s.drawCubeCalls.load()-s.drawCubeForeign.load())<<",\"flagsB9d\":[";
    for(unsigned i=0;i<lightsFlagsBytes;++i)o<<(i?",":"")<<s.lightFlags[i].load();
    o<<"]}}";
    return o.str();
}
}
