#pragma once
// STATIC LOD (debug.rdrvr.lod.static): distant static world geometry draws its AUTHORED low-LOD
// drawable, switched through the engine's own hi->lo cross-fade path. One JUMP_SLOT wrapper
// (rdrDrawableInstance::AddToDrawListFade), nothing else is patched. librdr 1.58 arm64, module-relative:
//  * Static instances (rdrDrawableInstance, sector StaticGeometryNode lists, stride 0x120) are culled and LOD-coded by
//    CachedVisibility::IsNotTooSmall(vp, sphere, a, b, c, d, float* fade) @0x2a74dcc (JUMP_SLOT 0x2e5fb90): D = vp+0x3d0 plane . c + r,
//    culled 0 when r*b < D*a; w8 = r*b < D*(a+2c) (double, 0x2a74ee8); beyond sm_MinLodModDist (80 m, GOT 0x2de09a8) w9 = r*b <
//    D*(a+c*sm_LodCostModifier) else w9 = w8; codes (0x2a74f84): w9 ? (w8 ? 2 : 6) : 1, or 5/3 when r*b < D*a*d; fade out =
//    clamp01((r*b - D*(a+c)) * sm_StaticGeoFadeThreshold 10.0 @.data 0x2f7cfc4) when the pointer is non-null.
//  * a/b/c: renVisibilityListMgr::CreateVisibilityListShaftGen @0x2abe27c (also
//    CalcScreenSizeConstant @0x2abd968) stores req+0x100 b = 0.596 (0x3f188b62 = 0.5/tan 40 deg,
//    FOV independent unless sniping/camera-cut bit 30), req+0x104 a = req+0x108 c =
//    GetThreshold(13)*0.5 = 0.0043 (sm_ScreenSpaceThreshold 0.0086). The lodTerm c is NOT 0 at the
//    MAIN sites. With q = r*b/(D*a): cull q < 1, lo q < 3; one q unit = 2*(a/b)*689 = 9.9 Quest 3
//    eye px (689 px per tan unit, stock phone 698), so the engine shows the lo drawable only for
//    10..30 px objects and pops hi->lo at ~30 px.
//  * MAIN (list 13, req+0xcc set by KickOffVisibilityRequest 0x2abcb88): CachedVisibility::AddToDrawList (0x2a70040..0x2a70064)
//    -> outdoors (req+0xa0 == 0 or req+0x30 == 0) CreateDrawListsOutDoorSS site bl 0x2a71730 (c = req+0x108, d 1.0, fade out
//    x29-0x54; entry {inst, alpha, fade, code} 0x2a71688, the computed fade only on path B 0x2a71c60), indoors (portal view)
//    CreateDrawListsOutDoor site bl 0x2a707e0 (c = req+0x108, d 0, no fade out; entry fade = sector node fade, 0x2a70a5c).
//    CreateVisibilityListPost -> DrawStaticObjects -> DrawStaticObj @0x2a72920: list >= 7 and not 8..11 ->
//    AddToDrawListFade(inst, shaft = req+0x150, alpha, fade, code) bl 0x2a7298c (return 0x2a72990). Sun-shadow lists 8..11 use
//    AddBestToDrawList and are not touched.
//  * AddToDrawListFade @0x2a78634 (JUMP_SLOT 0x2e5fbe0): returns on inst+0x2d bit 0; primary DrawInstance(lod 0, dissolve
//    min(1-fade+max(2*alpha-1,0),1), code) when inst+0xe8 != 0; secondary DrawInstance(lod 1, dissolve -1 = opaque, code -1, depth
//    bias -1e-7) when min(fade, max(1-2*alpha,0)) < 1 and inst+0xf0 != 0. DrawInstance @0x2a78874 (0x2a788ac): code 2/5/6 -> the lo
//    drawable inst+0xf0 with ITS LOD-1 triangle count lo+0xb4, any other code -> hi inst+0xe8 with hi+0xb0 (rmcDrawable per-LOD
//    triangle counts, CalculateTriCounts @0x1d9b400; added to the engine tri budget container+0x9c). renDrawBucketContainer::
//    AddToDrawList skips a dissolve >= 1 (0x2a266d0); a dissolve other than 0 / -1 sets the dissolve (dither) flag (0x2a26948).
//    DrawInstance's lod-1 ENTRY (0x2a78890: cmp w1,#1; ldrh w8,[x19,#0xd0]; mov w9,#9; bics wzr,w9,w8; b.ne epilogue)
//    drops the draw unless m_Flags (inst+0xd0, u16; 1 for runtime-built instances, serialized otherwise) has bits 0 AND 3. Bit 0 is
//    guaranteed by both MAIN loops (0x2a716d0, 0x2a70788 skip the instance without it), bit 3 is not. The secondary lo of the
//    cross-fade (and AddMediumToDrawList / AddBestToDrawList lo draws) go through that entry; code 2 enters with lod 0 and skips it.
//    => code 1 with fade f in (0,1) = the hi drawable dithered out by 1-f over the opaque lo = the engine's own hi->lo cross-fade,
//       but ONLY with m_Flags bit 3 (else the hi is dithered over nothing: see-through). Natively it is live only near the camera:
//       the code switches at q 3 but the fade reaches 1 at q = 2 + 0.1/(D*a), which is above 3 only for D < 23 m; beyond that the
//       engine pops hi->lo at q 3 (~30 px).
// Lever: for the exact MAIN caller (return 0x2a72990, req = shaft-0x150, req+0xcc == 13) and an instance the engine draws HI (code 1)
// in steady state (alpha 0, fade 1, no sector transition) whose lo drawable has LOD-1 triangles fewer than the hi LOD-0 triangles:
// px = 2*689*r/(|c-cam|+r) (projected diameter in eye px; RADIAL distance from the union camera shaft+0x110, so turning the head never
// flips a LOD) against T = debug.rdrvr.lod.static: px < T -> code 2 (lo, opaque), else unchanged (hard switch, as the engine
// itself switches beyond 23 m). Opt-in band (debug.rdrvr.lod.static.band, default 0): T <= px < T*(1+band) -> code 1 + fade
// (px/T-1)/band (engine cross-fade; only where the engine itself cross-fades: inst+0x2d bit 3 and model hash != 0xa63e7316, as
// OutDoorSS path B, AND m_Flags bits 0+3 so the secondary lo is really drawn; any other instance switches hard at T). The band is
// spatial, not temporal: every band object is a dithered hi + an opaque lo every frame (more GPU than the native opaque hi, and a
// screen-space dither that differs per eye), hence off by default. An instance without m_Flags bit 3 (the flag
// the engine requires for every lod-1 draw) is not switched at all by default (outcome loFlagOff; debug.rdrvr.lod.static.flag 0 lets
// it switch hard, never band; pin 1 overrides). Nothing whose nearest point is closer than debug.rdrvr.lod.static.mindist
// (default = the engine's sm_MinLodModDist, 80 m) changes. "<n>m" = distance rule: lo when the nearest point is beyond n m.
// 0 = kill switch (pure pass-through, one relaxed load). Properties re-read once per second (sceneBegin).
// Default ON, 60 px, hard switch: the engine's own triangle-budget LOD (sm_LodCostModifier 2..10 over 4.5..6 M tris, beyond 80 m:
// lo below q = 1+m) fed with the stereo frame's 2 x 2.0..2.7 M triangles would pick m 2..6.8 = lo below 30..78 px; 60 px lies
// inside the range the designers let the engine use, beyond the same 80 m, and it pops exactly like the engine's own q 3 switch.
// pin (debug.rdrvr.lod.static.pin): 1 every MAIN static with lo triangles draws lo at any
// distance, m_Flags bit 3 or not, 2 every one draws hi (native 10..30 px lo band off: the reference
// picture), 3 observe (decide and count only).
// Report graphics.staticLod: per-frame outcome counts, MAIN static triangles native vs drawn, px histogram of the eligible objects
// (count, hi and lo triangles per bucket: the triangles a threshold T would save = sum of hi-lo over the buckets below T).
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
#define RDRVR_LOD_NOINLINE __declspec(noinline)
#define RDRVR_LOD_RETURN_ADDRESS() reinterpret_cast<std::uintptr_t>(_ReturnAddress())
#else
#define RDRVR_LOD_NOINLINE __attribute__((noinline))
#define RDRVR_LOD_RETURN_ADDRESS() reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))
#endif

namespace rdrvr::engine::staticlod {
// ---- librdr 1.58 arm64 (module-relative) ----
inline constexpr std::uintptr_t fadeOffset=0x2a78634,fadeGotOffset=0x2e5fbe0;        // rdrDrawableInstance::AddToDrawListFade
inline constexpr std::uintptr_t mainReturnOffset=0x2a72990;                          // DrawStaticObj: bl AddToDrawListFade@plt @0x2a7298c
inline constexpr std::uintptr_t drawInstanceCodeOffset=0x2a78890,bucketFadeSkipOffset=0x2a266cc,drawStaticObjOffset=0x2a72940;
inline constexpr std::uintptr_t fadeSecondaryOffset=0x2a7881c;                       // Secondary lo DrawInstance(lod 1, code -1)
inline constexpr std::uintptr_t addToDrawListBranchOffset=0x2a70040;                 // InDoor, then OutDoorSS (no room) / OutDoor(2.0)
inline constexpr std::uintptr_t outdoorSsFadeOutOffset=0x2a716e8,outdoorSsSiteOffset=0x2a71714,outdoorSsEntryOffset=0x2a71688,outdoorSsPathBOffset=0x2a71c60;
inline constexpr std::uintptr_t outdoorSiteOffset=0x2a707c8,outdoorEntryOffset=0x2a70a5c;
inline constexpr std::uintptr_t notTooSmallDepthOffset=0x2a74dcc,notTooSmallLoOffset=0x2a74eb0,notTooSmallCodeOffset=0x2a74f84;
inline constexpr std::uintptr_t thresholdStoreOffset=0x2abe27c,listIndexStoreOffset=0x2abcb88;
inline constexpr std::uintptr_t minLodModDistOffset=0x2e736c8,minLodModDistGotOffset=0x2de09a8; // rage::grcDevice::sm_MinLodModDist (80.0)
// AddToDrawListFade 0x2a78634..0x2a7871f: prologue, +0x2d hidden test, dissolve math, primary (+0xe8, w2 kept), secondary gate (+0xf0)
inline constexpr std::array<unsigned char,236> fadeFingerprint{
#include "gamecode/StaticLod_2a78634_236_8d.inc"
};
// DrawInstance 0x2a78890: cmp w1,#1; stur canary; b.ne;
// ldrh w8,[x19,#0xd0]; mov w9,#9; bics wzr,w9,w8; b.ne 0x2a78a38 (m_Flags bits 0+3 or no lod-1 draw); then 0x2a788ac: cmp w2,#6;
// b.hi; (1<<w2)&0x64 -> lo drawable +0xf0 (code 2/5), code 6 keeps the hi tri count
inline constexpr std::array<unsigned char,68> drawInstanceCodeFingerprint{
#include "gamecode/StaticLod_2a78890_68_8d.inc"
};
// AddToDrawListFade 0x2a7881c..0x2a78867: secondary lo draw: dissolve -min(1-fade+max(1-2*alpha,0),1) (-1 = opaque at alpha 0);
// w1 = 1 (lod 1 -> the m_Flags gate); depth bias; w2 = -1; b DrawInstance@plt
inline constexpr std::array<unsigned char,76> fadeSecondaryFingerprint{
#include "gamecode/StaticLod_2a7881c_76_8d.inc"
};
// renDrawBucketContainer::AddToDrawList 0x2a266cc: params+0x40 (dissolve) >= 1.0 -> not drawn
inline constexpr std::array<unsigned char,16> bucketFadeSkipFingerprint{
#include "gamecode/StaticLod_2a266cc_16_8d.inc"
};
// DrawStaticObj 0x2a72940: list >= 7, not 8..11 -> AddToDrawListFade(entry+0, req+0x150, entry+8, entry+0xc, entry+0x10), bl @0x2a7298c
inline constexpr std::array<unsigned char,80> drawStaticObjFingerprint{
#include "gamecode/StaticLod_2a72940_80_8d.inc"
};
// CachedVisibility::AddToDrawList 0x2a70040: after InDoor, req+0xa0==0 or req+0x30==0 -> OutDoorSS, else OutDoor(2.0)
inline constexpr std::array<unsigned char,40> addToDrawListBranchFingerprint{
#include "gamecode/StaticLod_2a70040_40_8d.inc"
};
// OutDoorSS 0x2a716e8: sub x2,x29,#0x54 (fade out)
inline constexpr std::array<unsigned char,4> outdoorSsFadeOutFingerprint{162,83,1,209};
// OutDoorSS 0x2a71714: fmov s2,<c> (c = req+0x108); stur 1.0,[x29,#-0x54]; fmov s3,#1.0 (d); ldp s1,s0,[sp,#0x28] (b, a)
inline constexpr std::array<unsigned char,28> outdoorSsSiteFingerprint{
#include "gamecode/StaticLod_2a71714_28_8d.inc"
};
// OutDoorSS 0x2a71688: str s0,[x8,#8] (alpha); str w0,[x8,#0x10]! (code)
inline constexpr std::array<unsigned char,8> outdoorSsEntryFingerprint{0,9,0,189,0,13,1,184};
// OutDoorSS 0x2a71c60: computed fade only when inst+0x2d bit 3 and inst+0xf0 and the model hash is not 0xa63e7316
inline constexpr std::array<unsigned char,36> outdoorSsPathBFingerprint{
#include "gamecode/StaticLod_2a71c60_36_8d.inc"
};
// OutDoor 0x2a707c8: ldr s2,[sp,#0x1c] (c = req+0x108); d 0; ldp s1,s0,[sp,#0x30] (b, a); bl IsNotTooSmall@plt (x2 = 0)
inline constexpr std::array<unsigned char,28> outdoorSiteFingerprint{
#include "gamecode/StaticLod_2a707c8_28_8d.inc"
};
// OutDoor 0x2a70a5c: entry {inst, fade = [sp+0x70] sector node fade, alpha = node+0x40, code = w0}
inline constexpr std::array<unsigned char,44> outdoorEntryFingerprint{
#include "gamecode/StaticLod_2a70a5c_44_8d.inc"
};
// IsNotTooSmall 0x2a74dcc: D = vp+0x3d0 plane . c + r, first side plane
inline constexpr std::array<unsigned char,52> notTooSmallDepthFingerprint{
#include "gamecode/StaticLod_2a74dcc_52_8d.inc"
};
// IsNotTooSmall 0x2a74eb0: 10 m exemption, w8 = D*(a+2c) > r*b (double), w9 beyond sm_MinLodModDist with sm_LodCostModifier
inline constexpr std::array<unsigned char,140> notTooSmallLoFingerprint{
#include "gamecode/StaticLod_2a74eb0_140_8d.inc"
};
// IsNotTooSmall 0x2a74f84: codes w9 ? (w8 ? 2 : 6) : 1, or 5/3 below d
inline constexpr std::array<unsigned char,56> notTooSmallCodeFingerprint{
#include "gamecode/StaticLod_2a74f84_56_8d.inc"
};
// CreateVisibilityListShaftGen 0x2abe27c: req+0x100 = b, +0x104 = a, +0x108 = a, +0x10c = GetFadeThreshold
inline constexpr std::array<unsigned char,40> thresholdStoreFingerprint{
#include "gamecode/StaticLod_2abe27c_40_8d.inc"
};
// KickOffVisibilityRequest 0x2abcb88: str w1,[x10,#0xcc] (list index)
inline constexpr std::array<unsigned char,4> listIndexStoreFingerprint{65,205,0,185};
// rdrDrawableInstance / rmcDrawable / VisibilityListRequest / spdShaft fields
inline constexpr std::uintptr_t instFlagsField=0x2d,instBoundsField=0xa0,instHashField=0xc0,instHiField=0xe8,instLoField=0xf0; // bounds: min.xyz r(=min.w) max.xyz
inline constexpr std::uintptr_t instLodFlagsField=0xd0;     // m_Flags u16; DrawInstance lod-1 entry needs bits 0+3
inline constexpr std::uint16_t lodFlagsLoDraw=9u;
inline constexpr std::uint32_t noFadeModelHash=0xa63e7316u; // OutDoorSS path B: this model never gets the computed fade
inline constexpr std::uintptr_t hiTrisField=0xb0,loTrisField=0xb4;                    // rmcDrawable LOD-0 / LOD-1 triangle counts
inline constexpr std::uintptr_t shaftOfRequest=0x150,shaftCameraField=0x110,requestListField=0xcc,requestScaleField=0x100,requestThresholdField=0x104;
inline constexpr std::int32_t mainList=13;
// request entry sections (DrawStaticObjects 0x2a72778 / 0x2a728a8, DrawDynamicObjects 0x2a72498 / 0x2a72524): +0x40 portal, +0x44 vlow
// (sector very-low proxies, drawn directly), +0x3c dynamic, +0x38 static entries
inline constexpr std::uintptr_t requestStaticCountField=0x38,requestDynamicCountField=0x3c,requestPortalCountField=0x40,requestVlowCountField=0x44;
inline constexpr float pxPerTan=689.f; // Quest 3 eye: 1680 px over a 2.437 tan span (lodDiagnostics); stock phone 1374/1.968 = 698
inline constexpr float defaultPx=60.f,defaultBand=0.f,engineMinDistFallback=80.f; // Band 0 (hard switch) by default

// ---- pure decision (host-tested) ----
// loFlagOff = the rule would switch it, but m_Flags lacks bit 3 (needLoFlag): left hi.
enum Outcome : unsigned {Hidden,NoLo,NativeLo,OtherCode,Transition,Invalid,LoNoTris,LoNotCheaper,Near,KeptHi,Band,Switched,SwitchedNoFade,PinLo,PinHi,
    LoFlagOff,OutcomeCount};
inline constexpr const char* outcomeNames[OutcomeCount]{"hidden","noLo","nativeLo","otherCode","transition","invalid","loNoTris","loNotCheaper",
    "near","keptHi","band","switched","switchedNoFade","pinLo","pinHi","loFlagOff"};
struct Rule {
    bool on{true};float px{defaultPx};float metres{};float minDist{engineMinDistFallback};float band{defaultBand};unsigned pin{};
    bool needLoFlag{true}; // Switch only instances whose m_Flags has bits 0+3 (debug.rdrvr.lod.static.flag 0 = any, hard only)
    bool operator==(const Rule&) const = default;
};
struct Object { // what AddToDrawListFade gets and reads
    unsigned char flags{};bool hi{},lo{};std::uint32_t hiTris{},loTris{};
    bool fadeable{true}; // the engine's own fade policy (OutDoorSS path B 0x2a71c60): inst+0x2d bit 3 and model hash != 0xa63e7316
    bool loFlag{true};   // (m_Flags inst+0xd0 & 9) == 9, else DrawInstance drops every lod-1 draw (the band's secondary lo)
    float c[3]{};float r{};float cam[3]{};float alpha{};float fade{1.f};int code{1};
};
struct Decision {Outcome what{};int code{};float fade{1.f};float px{-1.f};float near{-1.f};bool apply{};bool eligible{};};
inline constexpr bool loCode(int code) noexcept {return code==2||code==5||code==6;}
inline Decision decide(const Rule& rule,const Object& o) noexcept {
    Decision d{};d.code=o.code;d.fade=o.fade;
    if(o.flags&1u){d.what=Hidden;return d;}                       // the engine returns before drawing
    if(!o.hi||!o.lo){d.what=NoLo;return d;}
    if(loCode(o.code)){                                             // the engine already draws lo (10..30 px band)
        if(rule.pin==2&&o.hiTris){d.what=PinHi;d.code=1;d.fade=1.f;d.apply=true;return d;}
        d.what=NativeLo;return d;
    }
    if(o.code!=1&&o.code!=3){d.what=OtherCode;return d;}           // 0 (inside 2 m) / -1 (rdrDrawableInstance::Draw)
    if(o.alpha!=0.f){d.what=Transition;return d;}                   // sector alpha: the engine owns the fade
    const bool ramp=!(o.fade>=1.f);                                 // the native hi->lo ramp (only < 23 m, see the header)
    if(ramp&&rule.pin==2){d.what=PinHi;d.code=1;d.fade=1.f;d.apply=true;return d;}
    if(ramp&&rule.pin!=1){d.what=Transition;return d;}
    const float dx=o.c[0]-o.cam[0],dy=o.c[1]-o.cam[1],dz=o.c[2]-o.cam[2];const float dist=std::sqrt(dx*dx+dy*dy+dz*dz);
    if(!std::isfinite(dist)||!std::isfinite(o.r)||!(o.r>0.f)||o.r>1e5f){d.what=Invalid;return d;}
    d.px=2.f*pxPerTan*o.r/(dist+o.r);d.near=dist-o.r;
    if(!o.loTris){d.what=LoNoTris;return d;}                       // no LOD-1 geometry in the lo drawable: never switch
    if(rule.pin==1){d.what=PinLo;d.code=2;d.fade=1.f;d.apply=true;return d;}
    if(o.loTris>=o.hiTris){d.what=LoNotCheaper;return d;}
    if(d.near<rule.minDist){d.what=Near;return d;}
    const bool gated=rule.needLoFlag&&!o.loFlag; // Never switched (and not in the histogram) while the flag is required
    d.eligible=!gated;
    if(rule.pin==2){d.what=KeptHi;return d;}
    const float s=rule.metres>0.f?rule.metres/std::max(d.near,1e-3f):d.px/std::max(rule.px,1e-3f); // >= 1: still hi
    if(s>=1.f+rule.band){d.what=KeptHi;return d;}
    if(gated){d.what=LoFlagOff;return d;}
    const bool fadeable=o.fadeable&&o.loFlag; // the secondary lo of a cross-fade is drawn only with m_Flags bits 0+3
    if(s<1.f){d.what=fadeable?Switched:SwitchedNoFade;d.code=2;d.fade=1.f;d.apply=true;return d;}
    if(!fadeable){d.what=KeptHi;return d;} // no cross-fade for this instance: hard switch at T
    d.what=Band;d.code=1;d.fade=std::clamp((s-1.f)/rule.band,0.f,1.f);d.apply=true;return d;
}
// Triangles of MAIN statics as the engine would draw them and as they are drawn after the decision (primary + secondary lo).
inline std::uint32_t nativeTris(const Object& o) noexcept {return loCode(o.code)&&o.lo?o.loTris:o.hi?o.hiTris:0u;}
inline std::uint32_t drawnTris(const Object& o,const Decision& d) noexcept {
    if(!d.apply)return nativeTris(o);
    if(d.what==Band)return o.hiTris+o.loTris;
    return loCode(d.code)?o.loTris:o.hiTris;
}
// Engine terms for the report: q = 1 (cull) and q = 3 (native lo) in eye px from the request's a / b.
inline float enginePxPerQ(float a,float b) noexcept {return a>0.f&&b>0.f?2.f*(a/b)*pxPerTan:0.f;}

// ---- properties ----
// debug.rdrvr.lod.static: "" -> default 60 px; "0"/"off" -> kill; "<n>" or "<n>px" -> px rule (1..4000); "<n>m" -> distance rule
// (1..20000 m); anything else -> default (counted as a bad value). .mindist <m> (0..5000, default engine 80), .band <percent> (0..200,
// default 0 = hard switch), .pin 0..3, .flag 0/1 (default 1 = only instances with m_Flags bits 0+3 switch).
struct Parsed {bool valid{};bool kill{};float px{};float metres{};};
inline Parsed parseLevel(const char* s) noexcept {
    Parsed p{};if(!s||!*s)return p;
    if(!std::strcmp(s,"off")||!std::strcmp(s,"OFF")){p.valid=p.kill=true;return p;}
    char* end{};const float v=std::strtof(s,&end);
    if(end==s||!std::isfinite(v))return p;
    if(!*end||!std::strcmp(end,"px")){if(v==0.f){p.valid=p.kill=true;return p;}if(v>=1.f&&v<=4000.f){p.valid=true;p.px=v;}return p;}
    if(!std::strcmp(end,"m")){if(v==0.f){p.valid=p.kill=true;return p;}if(v>=1.f&&v<=20000.f){p.valid=true;p.metres=v;}return p;}
    return p;
}
inline bool parseNumber(const char* s,float lo,float hi,float& out) noexcept {
    if(!s||!*s)return false;char* end{};const float v=std::strtof(s,&end);
    if(end==s||*end||!std::isfinite(v)||v<lo||v>hi)return false;out=v;return true;
}
struct PropertyText {std::string level,minDist,band,pin,flag;};
struct ResolvedRule {Rule rule;bool levelBad{},minDistBad{},bandBad{},pinBad{},flagBad{};};
inline ResolvedRule resolve(const PropertyText& t,float engineMinDist) noexcept {
    ResolvedRule r{};r.rule.minDist=engineMinDist;
    const Parsed level=parseLevel(t.level.c_str());
    if(!t.level.empty()&&!level.valid)r.levelBad=true;
    if(level.valid){r.rule.on=!level.kill;r.rule.px=level.px>0.f?level.px:defaultPx;r.rule.metres=level.metres;}
    float v{};
    if(parseNumber(t.minDist.c_str(),0.f,5000.f,v))r.rule.minDist=v;else if(!t.minDist.empty())r.minDistBad=true;
    if(parseNumber(t.band.c_str(),0.f,200.f,v))r.rule.band=v/100.f;else if(!t.band.empty())r.bandBad=true;
    if(parseNumber(t.pin.c_str(),0.f,3.f,v)&&v==std::floor(v))r.rule.pin=unsigned(v);else if(!t.pin.empty())r.pinBad=true;
    if(parseNumber(t.flag.c_str(),0.f,1.f,v)&&v==std::floor(v))r.rule.needLoFlag=v!=0.f;else if(!t.flag.empty())r.flagBad=true;
    return r;
}

// ---- runtime state ----
inline constexpr unsigned histBuckets=8;
inline constexpr std::array<float,histBuckets-1> histEdgesPx{30.f,45.f,60.f,80.f,110.f,150.f,250.f};
inline unsigned histBucket(float px) noexcept {unsigned i=0;while(i<histEdgesPx.size()&&px>=histEdgesPx[i])++i;return i;}
struct Counters { // cumulative; MAIN statics only unless named otherwise
    std::array<std::atomic<std::uint64_t>,OutcomeCount> outcomes{};
    std::array<std::atomic<std::uint64_t>,histBuckets> hist{},histHiTris{},histLoTris{}; // eligible objects: count, hi / lo triangles per px bucket
    std::atomic<std::uint64_t> main{},notMain{},otherCaller{},applied{},trisNative{},trisDrawn{};
};
struct Window { // per-frame means over the last ~1 s of RenderScenes
    double scenes{},main{},notMain{},otherCaller{},applied{},trisNative{},trisDrawn{};
    std::array<double,OutcomeCount> outcomes{};std::array<double,histBuckets> hist{},histHiTris{},histLoTris{};
};
struct Config {std::uintptr_t mainReturn{};float engineMinDist{engineMinDistFallback};bool engineMinDistRead{};};
using Fade=void(*)(void*,const void*,float,float,int);
inline Config config;
inline Fade originalFade{};
inline std::atomic<bool> installed{};
inline std::atomic<const char*> fingerprint{"unconfigured"};
inline Counters counters;
// The rule the hook reads (relaxed; a property change may tear for one call, harmless).
inline std::atomic<bool> ruleOn{true};
inline std::atomic<float> rulePx{defaultPx},ruleMetres{0.f},ruleMinDist{engineMinDistFallback},ruleBand{defaultBand};
inline std::atomic<unsigned> rulePin{};
inline std::atomic<bool> ruleNeedLoFlag{true};
inline std::atomic<float> lastA{},lastB{};                       // the MAIN request's thresholds (engine terms in the report)
inline std::atomic<float> lastSwitchPx{-1.f},lastSwitchNear{-1.f};
inline std::array<std::atomic<std::uint32_t>,4> lastSections{}; // MAIN request: static, dynamic, portal, vlow entries
inline Rule currentRule() noexcept {
    Rule r{};r.on=ruleOn.load(std::memory_order_relaxed);r.px=rulePx.load(std::memory_order_relaxed);r.metres=ruleMetres.load(std::memory_order_relaxed);
    r.minDist=ruleMinDist.load(std::memory_order_relaxed);r.band=ruleBand.load(std::memory_order_relaxed);r.pin=rulePin.load(std::memory_order_relaxed);
    r.needLoFlag=ruleNeedLoFlag.load(std::memory_order_relaxed);return r;}
inline void publishRule(const Rule& r) noexcept {
    rulePx.store(r.px,std::memory_order_relaxed);ruleMetres.store(r.metres,std::memory_order_relaxed);ruleMinDist.store(r.minDist,std::memory_order_relaxed);
    ruleBand.store(r.band,std::memory_order_relaxed);rulePin.store(r.pin,std::memory_order_relaxed);ruleNeedLoFlag.store(r.needLoFlag,std::memory_order_relaxed);
    ruleOn.store(r.on,std::memory_order_relaxed);}
inline void add(std::atomic<std::uint64_t>& a,std::uint64_t v=1) noexcept {a.fetch_add(v,std::memory_order_relaxed);}

// ---- engine memory (the engine itself reads every one of these fields in the same call) ----
inline float f32(std::uintptr_t a) noexcept {float v;std::memcpy(&v,reinterpret_cast<const void*>(a),sizeof v);return v;}
inline std::uint32_t u32(std::uintptr_t a) noexcept {std::uint32_t v;std::memcpy(&v,reinterpret_cast<const void*>(a),sizeof v);return v;}
inline std::uintptr_t ptr(std::uintptr_t a) noexcept {std::uintptr_t v;std::memcpy(&v,reinterpret_cast<const void*>(a),sizeof v);return v;}
inline Object readObject(std::uintptr_t inst,std::uintptr_t shaft,float alpha,float fade,int code) noexcept {
    Object o{};o.flags=*reinterpret_cast<const unsigned char*>(inst+instFlagsField);o.alpha=alpha;o.fade=fade;o.code=code;
    const std::uintptr_t hi=ptr(inst+instHiField),lo=ptr(inst+instLoField);o.hi=hi!=0;o.lo=lo!=0;
    if(o.flags&1u||!hi||!lo)return o;
    o.hiTris=u32(hi+hiTrisField);o.loTris=u32(lo+loTrisField);o.fadeable=(o.flags&8u)&&u32(inst+instHashField)!=noFadeModelHash;
    { std::uint16_t mf;std::memcpy(&mf,reinterpret_cast<const void*>(inst+instLodFlagsField),sizeof mf);o.loFlag=(mf&lodFlagsLoDraw)==lodFlagsLoDraw; }
    const std::uintptr_t b=inst+instBoundsField;
    for(unsigned i=0;i<3;++i){o.c[i]=0.5f*(f32(b+4*i)+f32(b+0x10+4*i));o.cam[i]=f32(shaft+shaftCameraField+4*i);}
    o.r=f32(b+0xc);return o;
}

// ---- the hook (GOT replacement of AddToDrawListFade) ----
inline void fadeAt(void* inst,const void* shaft,float alpha,float fade,int code,std::uintptr_t caller) {
    if(!ruleOn.load(std::memory_order_relaxed)){originalFade(inst,shaft,alpha,fade,code);return;} // kill switch: one relaxed load
    if(caller!=config.mainReturn||!inst||!shaft){add(counters.otherCaller);originalFade(inst,shaft,alpha,fade,code);return;}
    const auto req=reinterpret_cast<std::uintptr_t>(shaft)-shaftOfRequest;
    if(std::int32_t(u32(req+requestListField))!=mainList){add(counters.notMain);originalFade(inst,shaft,alpha,fade,code);return;}
    lastA.store(f32(req+requestThresholdField),std::memory_order_relaxed);lastB.store(f32(req+requestScaleField),std::memory_order_relaxed);
    for(unsigned i=0;i<4;++i)lastSections[i].store(u32(req+requestStaticCountField+4*i),std::memory_order_relaxed);
    const Rule rule=currentRule();
    const Object o=readObject(reinterpret_cast<std::uintptr_t>(inst),reinterpret_cast<std::uintptr_t>(shaft),alpha,fade,code);
    Decision d=decide(rule,o);
    if(rule.pin==3)d.apply=false; // observe
    add(counters.main);add(counters.outcomes[d.what]);
    if(d.eligible){const unsigned h=histBucket(d.px);add(counters.hist[h]);add(counters.histHiTris[h],o.hiTris);add(counters.histLoTris[h],o.loTris);}
    if(o.hi&&o.lo&&!(o.flags&1u)){add(counters.trisNative,nativeTris(o));add(counters.trisDrawn,drawnTris(o,d));}
    if(d.apply){
        add(counters.applied);
        if(d.what==Switched||d.what==SwitchedNoFade){lastSwitchPx.store(d.px,std::memory_order_relaxed);lastSwitchNear.store(d.near,std::memory_order_relaxed);}
        originalFade(inst,shaft,alpha,d.fade,d.code);return;
    }
    originalFade(inst,shaft,alpha,fade,code);
}
RDRVR_LOD_NOINLINE inline void fadeHook(void* inst,const void* shaft,float alpha,float fade,int code) {
    fadeAt(inst,shaft,alpha,fade,code,RDRVR_LOD_RETURN_ADDRESS());}

// ---- install (TraceEntry): code = exec fingerprint (module offset), read = guarded read (absolute), replace = GOT swap ----
template<class Code,class Read,class Replace>
bool install(std::uintptr_t base,const Code& code,const Read& read,const Replace& replace) {
    const auto got=[&](std::uintptr_t slot,std::uintptr_t target){std::uintptr_t v{};return read(base+slot,&v,sizeof v)&&v==base+target;};
    const char* r="ok";
    if(!code(fadeOffset,fadeFingerprint)||!code(fadeSecondaryOffset,fadeSecondaryFingerprint)||!code(drawInstanceCodeOffset,drawInstanceCodeFingerprint)||
       !code(bucketFadeSkipOffset,bucketFadeSkipFingerprint))r="consumer";
    else if(!code(drawStaticObjOffset,drawStaticObjFingerprint)||!code(listIndexStoreOffset,listIndexStoreFingerprint))r="caller";
    else if(!code(addToDrawListBranchOffset,addToDrawListBranchFingerprint)||!code(outdoorSsFadeOutOffset,outdoorSsFadeOutFingerprint)||
            !code(outdoorSsSiteOffset,outdoorSsSiteFingerprint)||!code(outdoorSsEntryOffset,outdoorSsEntryFingerprint)||
            !code(outdoorSsPathBOffset,outdoorSsPathBFingerprint)||!code(outdoorSiteOffset,outdoorSiteFingerprint)||
            !code(outdoorEntryOffset,outdoorEntryFingerprint))r="sites";
    else if(!code(notTooSmallDepthOffset,notTooSmallDepthFingerprint)||!code(notTooSmallLoOffset,notTooSmallLoFingerprint)||
            !code(notTooSmallCodeOffset,notTooSmallCodeFingerprint)||!code(thresholdStoreOffset,thresholdStoreFingerprint))r="engineRule";
    else if(!got(fadeGotOffset,fadeOffset))r="got";
    if(r[0]!='o'){fingerprint.store(r,std::memory_order_relaxed);return false;}
    float minLod{};
    if(got(minLodModDistGotOffset,minLodModDistOffset)&&read(base+minLodModDistOffset,&minLod,sizeof minLod)&&std::isfinite(minLod)&&minLod>0.f&&minLod<1000.f){
        config.engineMinDist=minLod;config.engineMinDistRead=true;}
    config.mainReturn=base+mainReturnOffset;originalFade=reinterpret_cast<Fade>(base+fadeOffset);
    { Rule initial{};initial.minDist=config.engineMinDist;publishRule(initial); } // default ON until the first property read
    const bool ok=replace(base+fadeGotOffset,base+fadeOffset,reinterpret_cast<std::uintptr_t>(&fadeHook));
    fingerprint.store(ok?"ok":"replace",std::memory_order_relaxed);installed.store(ok,std::memory_order_relaxed);
    return ok;
}

// ---- once-per-second property read + per-frame window (TraceEntry observeScene, render thread) ----
struct Runtime {
    std::mutex mutex;Window window;PropertyText text;ResolvedRule resolved;std::uint64_t propertyReads{},sceneCount{};
    std::uint64_t windowStartNs{},windowScenes{};
    std::array<std::uint64_t,OutcomeCount> lastOutcomes{};std::array<std::uint64_t,histBuckets> lastHist{},lastHistHi{},lastHistLo{};
    std::uint64_t lastMain{},lastNotMain{},lastOther{},lastApplied{},lastTrisNative{},lastTrisDrawn{};
};
inline Runtime runtimeState;
inline void applyProperties(const PropertyText& t) {
    const ResolvedRule r=resolve(t,config.engineMinDist);publishRule(r.rule);
    std::lock_guard lock(runtimeState.mutex);runtimeState.text=t;runtimeState.resolved=r;++runtimeState.propertyReads;
}
inline void rollWindow() {
    auto& s=runtimeState;const double n=double(s.windowScenes);if(!s.windowScenes)return;
    const auto take=[&](const std::atomic<std::uint64_t>& a,std::uint64_t& last){const auto v=a.load(std::memory_order_relaxed);const double d=double(v-last)/n;last=v;return d;};
    Window w{};w.scenes=n;
    w.main=take(counters.main,s.lastMain);w.notMain=take(counters.notMain,s.lastNotMain);w.otherCaller=take(counters.otherCaller,s.lastOther);
    w.applied=take(counters.applied,s.lastApplied);w.trisNative=take(counters.trisNative,s.lastTrisNative);w.trisDrawn=take(counters.trisDrawn,s.lastTrisDrawn);
    for(unsigned i=0;i<OutcomeCount;++i)w.outcomes[i]=take(counters.outcomes[i],s.lastOutcomes[i]);
    for(unsigned i=0;i<histBuckets;++i){w.hist[i]=take(counters.hist[i],s.lastHist[i]);w.histHiTris[i]=take(counters.histHiTris[i],s.lastHistHi[i]);
        w.histLoTris[i]=take(counters.histLoTris[i],s.lastHistLo[i]);}
    s.window=w;s.windowScenes=0;
}
// One RenderScene. Returns true when the 1 s window rolled (the caller then re-reads the properties).
inline bool sceneTick(std::uint64_t nowNs) {
    auto& s=runtimeState;std::lock_guard lock(s.mutex);++s.sceneCount;++s.windowScenes;
    if(!s.windowStartNs){s.windowStartNs=nowNs;return true;}
    if(nowNs-s.windowStartNs<1'000'000'000ull)return false;
    rollWindow();s.windowStartNs=nowNs;return true;
}
#if defined(__ANDROID__)
inline std::string propertyText(const char* name) {char v[PROP_VALUE_MAX]{};const int n=__system_property_get(name,v);return n>0?std::string(v,std::size_t(n)):std::string();}
#endif
inline void sceneBegin() {
    if(!installed.load(std::memory_order_relaxed))return;
    const auto now=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    if(!sceneTick(now))return;
#if defined(__ANDROID__)
    const int saved=errno;
    applyProperties({propertyText("debug.rdrvr.lod.static"),propertyText("debug.rdrvr.lod.static.mindist"),
                     propertyText("debug.rdrvr.lod.static.band"),propertyText("debug.rdrvr.lod.static.pin"),propertyText("debug.rdrvr.lod.static.flag")});
    errno=saved;
#endif
}

// ---- report: "staticLod":{...} (member of graphics) ----
inline void jsonText(std::ostringstream& o,const std::string& s) {o<<'"';for(char ch:s){if(ch=='"'||ch=='\\')o<<'\\';if(ch>=' ')o<<ch;}o<<'"';}
inline std::string report() {
    std::ostringstream o;o.setf(std::ios::fixed);o.precision(2);
    const Rule r=currentRule();
    const float a=lastA.load(std::memory_order_relaxed),b=lastB.load(std::memory_order_relaxed),q=enginePxPerQ(a,b);
    o<<"\"staticLod\":{\"installed\":"<<(installed.load()?1:0)<<",\"fingerprint\":\""<<fingerprint.load()<<'"';
    o<<",\"rule\":{\"on\":"<<(r.on?1:0)<<",\"px\":"<<r.px<<",\"metres\":"<<r.metres<<",\"minDist\":"<<r.minDist<<",\"bandPct\":"<<r.band*100.f
     <<",\"pin\":"<<r.pin<<",\"needLoFlag\":"<<(r.needLoFlag?1:0)<<",\"pxPerTan\":"<<pxPerTan<<'}';
    o<<",\"engine\":{\"minLodModDist\":"<<config.engineMinDist<<",\"minLodModDistRead\":"<<(config.engineMinDistRead?1:0);
    o.precision(5);o<<",\"a\":"<<a<<",\"b\":"<<b;o.precision(2);
    o<<",\"cullPx\":"<<q<<",\"loPx\":"<<3.f*q<<'}';
    {
        std::lock_guard lock(runtimeState.mutex);const auto& s=runtimeState;
        o<<",\"property\":{\"level\":";jsonText(o,s.text.level);o<<",\"mindist\":";jsonText(o,s.text.minDist);o<<",\"band\":";jsonText(o,s.text.band);
        o<<",\"pin\":";jsonText(o,s.text.pin);o<<",\"flag\":";jsonText(o,s.text.flag);
        o<<",\"bad\":["<<(s.resolved.levelBad?1:0)<<','<<(s.resolved.minDistBad?1:0)<<','<<(s.resolved.bandBad?1:0)<<','<<(s.resolved.pinBad?1:0)
         <<','<<(s.resolved.flagBad?1:0)<<"],\"reads\":"<<s.propertyReads<<'}';
        o<<",\"scenes\":"<<s.sceneCount;
        const auto& w=s.window;
        o<<",\"perFrame\":{\"windowScenes\":"<<w.scenes<<",\"main\":"<<w.main<<",\"applied\":"<<w.applied<<",\"notMain\":"<<w.notMain
         <<",\"otherCaller\":"<<w.otherCaller<<",\"trisNative\":"<<w.trisNative<<",\"trisDrawn\":"<<w.trisDrawn;
        for(unsigned i=0;i<OutcomeCount;++i)o<<",\""<<outcomeNames[i]<<"\":"<<w.outcomes[i];
        o<<",\"histPx\":[";for(unsigned i=0;i<histBuckets;++i)o<<(i?",":"")<<w.hist[i];
        o<<"],\"histHiTris\":[";for(unsigned i=0;i<histBuckets;++i)o<<(i?",":"")<<w.histHiTris[i];
        o<<"],\"histLoTris\":[";for(unsigned i=0;i<histBuckets;++i)o<<(i?",":"")<<w.histLoTris[i];o<<"]}";
    }
    o<<",\"histEdgesPx\":[";for(unsigned i=0;i<histEdgesPx.size();++i)o<<(i?",":"")<<histEdgesPx[i];o<<']';
    o<<",\"cum\":{\"main\":"<<counters.main.load()<<",\"applied\":"<<counters.applied.load()<<",\"notMain\":"<<counters.notMain.load()
     <<",\"otherCaller\":"<<counters.otherCaller.load()<<",\"trisNative\":"<<counters.trisNative.load()<<",\"trisDrawn\":"<<counters.trisDrawn.load();
    for(unsigned i=0;i<OutcomeCount;++i)o<<",\""<<outcomeNames[i]<<"\":"<<counters.outcomes[i].load();
    o<<'}';
    o<<",\"mainList\":{\"static\":"<<lastSections[0].load()<<",\"dynamic\":"<<lastSections[1].load()<<",\"portal\":"<<lastSections[2].load()
     <<",\"vlow\":"<<lastSections[3].load()<<'}';
    o<<",\"lastSwitch\":{\"px\":"<<lastSwitchPx.load()<<",\"near\":"<<lastSwitchNear.load()<<"}}";
    return o.str();
}
} // namespace rdrvr::engine::staticlod
