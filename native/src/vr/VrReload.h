#pragma once
// RELOAD: fully manual revolver reload + closing the cylinder by B / the free hand's push / a
// wrist flick / a wave.
//
// What the player does (SINGLE / FULL, the manual gate open = VR places the revolver and the context allows it):
//   B press -> the cylinder swings out toward the free hand (150 ms); RT is dead while open. Muzzle up + a downward shake (only while nothing was
//   seated since this open) or the free hand pushing the ejector rod back -> every chamber empties (live rounds back to the pool as far as its
//   max allows). The free hand squeezes its grip at the chest / belt -> a round (SINGLE) or a speedloader (FULL) between the fingers; its tip
//   to an EMPTY chamber mouth (relative test on the XR tape: moving the gun onto the round works too) -> seated (engine AddAmmo). Close: short
//   B, the free hand pushing the cylinder to the frame from outside, a short wrist flick, a chopping wave (CYLINDER CLOSE row). Trigger on an
//   EMPTY / SPENT chamber = dry click (+ haptic), the cylinder advances. Long B (600 ms): from CLOSED with nothing done = the GAME's reload
//   (one RequestReload allowed for 1 s, B unmasked until release); from OPEN = instant auto-load + close.
// Engine: the engine clip stays authoritative for the COUNT (AddAmmo / SetAmmo through the
// executor, main thread); the per-chamber model (LIVE / SPENT / EMPTY / FIRING, `next`) is the
// mod's own and is reconciled against the clip (fires, Dead Eye, duels, scripts, native reloads in
// vetoed contexts). The game's own reload is cut by ONE GOT hook on aniAim::RequestReload keyed on
// the player's on-hand weapon (FastDraw asks inside SelectItem, before the mod's hold is set);
// pass-through audit hooks on AddAmmo / SetAmmo attribute refills. Holster refill: the sidearm
// item's refill timer +0xd0 is held at -1.0f. Any print / hook / executor failure -> effective
// NATIVE (never a half mode).
//
// Threads: input (holsters InputBrain: brainSink before the grab loop, freeHandOwned / grabSink in it, padFilter = THE B edge owner,
// rtGate / rtHeld in route()), main (ModuleReload 6: ops, fires, reconcile, refill timer, hook words), any (apply() inside the held
// weapon's own weapWeapon::Update: the swing + the ChamberFrame; the RequestReload / audit hooks; noteEngineFire), render (overlay channel 2b).
// Kill switch: debug.rdrvr.vr.reload 0 (Feature::Reload): NATIVE forced, nothing installed, B = the game's.
#include "VrHolsters.h"
#include "VrReloadClose.h"
#include "VrSettings.h"
#include "VrTouch.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#if defined(__ANDROID__)
#include <unistd.h>
#include <sys/syscall.h>
#endif

namespace rdrvr::vr::reload {
namespace H=::rdrvr::vr::holsters;
namespace C=::rdrvr::vr::reload::close;
using Mat=std::array<float,16>; // column-major affine, translation at 12..14 (= RAGE Matrix34 rows a,b,c,d with w lanes)
using V3=std::array<float,3>;
using Q=std::array<float,4>;
using CloseSource=C::Source;

// =====================================================================================================================
// Engine identities (librdr.so sha256 3f80845e..., .text file offset = vaddr - 0x4000). Prints (PrintRef): the byte
// fingerprints of the hooked code, compared against librdr.so, + the AmmoComponent getters the executor's
// direct pool reads rely on (stride 12, +4 max, +8 infinite).
namespace elf {
struct PrintRef {std::uintptr_t offset;const unsigned char* bytes;std::size_t size;};
inline constexpr std::uintptr_t requestReloadGotOffset=0x2e3c0d8,requestReloadPltOffset=0x2c674f0;   // JUMP_SLOT aniAim::RequestReload
inline constexpr std::uintptr_t addAmmoGotOffset=0x2e474f0,addAmmoPltOffset=0x2c7dd20;               // JUMP_SLOT weapWeapon::AddAmmo
inline constexpr std::uintptr_t setAmmoGotOffset=0x2e476e0,setAmmoPltOffset=0x2c7e100;               // JUMP_SLOT weapWeapon::SetAmmo
// object layout (all covered by the prints: RequestReload 0x1cbfd4c / 0x1cbfd54 / 0x1cbfe2c, GetOnHandWeapon +0x80, invWeapon::GetWeapon
// +0xa0, GetAmmoCount +0x12c, RequestReload +0x9c6, SetAmmo enum [w+0x28]+0x10, pool getters stride 12, invWeapon::OnUpdate +0xd0)
inline constexpr std::size_t aimOwner=0x08,ownerActor=0x10,actorFlags=0x118,actorInventory=0x70,invHand=0x80,itemWeapon=0xa0,weaponType=0x28,
    typeAmmoEnum=0x10,weaponClip=0x12c,weaponNativeReloading=0x9c6,invPool=0x410,poolArray=0x08,poolCount=0x10,poolEntry=12,itemRefillTimer=0xd0;
// LR -> cause (tail calls mapped to the tail-caller's caller)
enum Cause : unsigned {CauseUnknown,CauseBOrTrigger,CauseAutoAfterShot,CauseFireTask,CauseFastDraw,CauseReloadTask,CauseDeadEye,CauseCancelReload,
    CauseNativeAnim,CauseHolster,CauseCount};
inline constexpr std::array<const char*,CauseCount> causeNames{"UNKNOWN","B_OR_TRIGGER","AUTO_AFTER_SHOT","FIRE_TASK","FASTDRAW","RELOAD_TASK",
    "DEAD_EYE","CANCEL_RELOAD","NATIVE_ANIM","HOLSTER"};
struct LrCause {std::uintptr_t lr;Cause cause;};
inline constexpr std::array<LrCause,19> lrCauses{{
    {0x200d014,CauseBOrTrigger},{0x1cc1700,CauseAutoAfterShot},{0x1cf23e0,CauseFireTask},                     // RequestReload callers
    {0x1cbf8d8,CauseFastDraw},{0x1cc07d4,CauseFastDraw},{0x1cc095c,CauseFastDraw},{0x1cc0a38,CauseFastDraw},{0x2667f10,CauseFastDraw}, // FastDraw's callers (tail b)
    {0x1cf2394,CauseFireTask},{0x1d01ef0,CauseReloadTask},{0x269a91c,CauseDeadEye},{0x269ad54,CauseDeadEye},   // SetAmmo callers
    {0x269cf20,CauseCancelReload},{0x1d01f00,CauseCancelReload},{0x1d0209c,CauseCancelReload},              // CancelReload's callers (tail b AddAmmo)
    {0x1cc1074,CauseNativeAnim},{0x1cc116c,CauseNativeAnim},{0x1cc13b8,CauseNativeAnim},{0x1cc1a58,CauseNativeAnim}}}; // state 0xe / BowReloadCheck AddAmmo
inline Cause causeOf(std::uintptr_t lrOffset) noexcept {for(const auto& c:lrCauses)if(c.lr==lrOffset)return c.cause;return CauseUnknown;}
// requestReload 0x1cbfcc8, 472 B
inline constexpr std::uintptr_t requestReloadOffset=0x1cbfcc8;
inline constexpr std::array<unsigned char,472> requestReloadFingerprint{
#include "gamecode/VrReload_1cbfcc8_472_8d.inc"
};
// getAmmoCount 0x2546084, 8 B
inline constexpr std::uintptr_t getAmmoCountOffset=0x2546084;
inline constexpr std::array<unsigned char,8> getAmmoCountFingerprint{0,44,65,189,192,3,95,214};
// invWeaponGetWeapon 0x2676618, 8 B
inline constexpr std::uintptr_t invWeaponGetWeaponOffset=0x2676618;
inline constexpr std::array<unsigned char,8> invWeaponGetWeaponFingerprint{0,80,64,249,192,3,95,214};
// getOnHandWeapon 0x26605e8, 52 B
inline constexpr std::uintptr_t getOnHandWeaponOffset=0x26605e8;
inline constexpr std::array<unsigned char,52> getOnHandWeaponFingerprint{
#include "gamecode/VrReload_26605e8_52_8d.inc"
};
// setInfiniteFlag 0x2540594, 24 B
inline constexpr std::uintptr_t setInfiniteFlagOffset=0x2540594;
inline constexpr std::array<unsigned char,24> setInfiniteFlagFingerprint{
#include "gamecode/VrReload_2540594_24_8d.inc"
};
// addAmmo 0x2545c80, 580 B
inline constexpr std::uintptr_t addAmmoOffset=0x2545c80;
inline constexpr std::array<unsigned char,580> addAmmoFingerprint{
#include "gamecode/VrReload_2545c80_580_8d.inc"
};
// setAmmo 0x2542f08, 472 B
inline constexpr std::uintptr_t setAmmoOffset=0x2542f08;
inline constexpr std::array<unsigned char,472> setAmmoFingerprint{
#include "gamecode/VrReload_2542f08_472_8d.inc"
};
// dryFire 0x25446a0, 16 B
inline constexpr std::uintptr_t dryFireOffset=0x25446a0;
inline constexpr std::array<unsigned char,16> dryFireFingerprint{
#include "gamecode/VrReload_25446a0_16_8d.inc"
};
// removeAmmo 0x254054c, 72 B
inline constexpr std::uintptr_t removeAmmoOffset=0x254054c;
inline constexpr std::array<unsigned char,72> removeAmmoFingerprint{
#include "gamecode/VrReload_254054c_72_8d.inc"
};
// ammoPerClip 0x254608c, 132 B
inline constexpr std::uintptr_t ammoPerClipOffset=0x254608c;
inline constexpr std::array<unsigned char,132> ammoPerClipFingerprint{
#include "gamecode/VrReload_254608c_132_8d.inc"
};
// invWeaponOnUpdate 0x2676d10, 348 B
inline constexpr std::uintptr_t invWeaponOnUpdateOffset=0x2676d10;
inline constexpr std::array<unsigned char,348> invWeaponOnUpdateFingerprint{
#include "gamecode/VrReload_2676d10_348_8d.inc"
};
// invWeaponOnReHolster 0x2676718, 220 B
inline constexpr std::uintptr_t invWeaponOnReHolsterOffset=0x2676718;
inline constexpr std::array<unsigned char,220> invWeaponOnReHolsterFingerprint{
#include "gamecode/VrReload_2676718_220_8d.inc"
};
// fireDecrement 0x2542278, 24 B
inline constexpr std::uintptr_t fireDecrementOffset=0x2542278;
inline constexpr std::array<unsigned char,24> fireDecrementFingerprint{
#include "gamecode/VrReload_2542278_24_8d.inc"
};
// discardAmmo 0x2545ec4, 32 B
inline constexpr std::uintptr_t discardAmmoOffset=0x2545ec4;
inline constexpr std::array<unsigned char,32> discardAmmoFingerprint{
#include "gamecode/VrReload_2545ec4_32_8d.inc"
};
// poolGetAmmoCount 0x25404a4, 20 B
inline constexpr std::uintptr_t poolGetAmmoCountOffset=0x25404a4;
inline constexpr std::array<unsigned char,20> poolGetAmmoCountFingerprint{
#include "gamecode/VrReload_25404a4_20_8d.inc"
};
// poolGetMaxAmmoCount 0x25404b8, 24 B
inline constexpr std::uintptr_t poolGetMaxAmmoCountOffset=0x25404b8;
inline constexpr std::array<unsigned char,24> poolGetMaxAmmoCountFingerprint{
#include "gamecode/VrReload_25404b8_24_8d.inc"
};
// poolGetInfiniteFlag 0x25404d0, 24 B
inline constexpr std::uintptr_t poolGetInfiniteFlagOffset=0x25404d0;
inline constexpr std::array<unsigned char,24> poolGetInfiniteFlagFingerprint{
#include "gamecode/VrReload_25404d0_24_8d.inc"
};
inline constexpr std::array<PrintRef,17> prints{{{requestReloadOffset,requestReloadFingerprint.data(),requestReloadFingerprint.size()},{getAmmoCountOffset,getAmmoCountFingerprint.data(),getAmmoCountFingerprint.size()},{invWeaponGetWeaponOffset,invWeaponGetWeaponFingerprint.data(),invWeaponGetWeaponFingerprint.size()},{getOnHandWeaponOffset,getOnHandWeaponFingerprint.data(),getOnHandWeaponFingerprint.size()},{setInfiniteFlagOffset,setInfiniteFlagFingerprint.data(),setInfiniteFlagFingerprint.size()},{addAmmoOffset,addAmmoFingerprint.data(),addAmmoFingerprint.size()},{setAmmoOffset,setAmmoFingerprint.data(),setAmmoFingerprint.size()},{dryFireOffset,dryFireFingerprint.data(),dryFireFingerprint.size()},{removeAmmoOffset,removeAmmoFingerprint.data(),removeAmmoFingerprint.size()},{ammoPerClipOffset,ammoPerClipFingerprint.data(),ammoPerClipFingerprint.size()},{invWeaponOnUpdateOffset,invWeaponOnUpdateFingerprint.data(),invWeaponOnUpdateFingerprint.size()},{invWeaponOnReHolsterOffset,invWeaponOnReHolsterFingerprint.data(),invWeaponOnReHolsterFingerprint.size()},{fireDecrementOffset,fireDecrementFingerprint.data(),fireDecrementFingerprint.size()},{discardAmmoOffset,discardAmmoFingerprint.data(),discardAmmoFingerprint.size()},{poolGetAmmoCountOffset,poolGetAmmoCountFingerprint.data(),poolGetAmmoCountFingerprint.size()},{poolGetMaxAmmoCountOffset,poolGetMaxAmmoCountFingerprint.data(),poolGetMaxAmmoCountFingerprint.size()},{poolGetInfiniteFlagOffset,poolGetInfiniteFlagFingerprint.data(),poolGetInfiniteFlagFingerprint.size()}}};
} // namespace elf

// =====================================================================================================================
// Small math (column-major affine Mat, unit vectors).
inline V3 add(V3 a,V3 b) noexcept {return {a[0]+b[0],a[1]+b[1],a[2]+b[2]};}
inline V3 sub(V3 a,V3 b) noexcept {return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
inline V3 mul(V3 a,float s) noexcept {return {a[0]*s,a[1]*s,a[2]*s};}
inline float dot(V3 a,V3 b) noexcept {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
inline V3 cross(V3 a,V3 b) noexcept {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
inline float length(V3 a) noexcept {return std::sqrt(dot(a,a));}
inline V3 col(const Mat& m,unsigned c) noexcept {return {m[c*4],m[c*4+1],m[c*4+2]};}
inline V3 normalized(V3 v) noexcept {const float l=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);if(l>1e-6f)for(float& x:v)x/=l;return v;}
inline Mat multiply(const Mat& a,const Mat& b) noexcept { // affine a*b (w lanes forced 0/1)
    Mat r{};for(unsigned c=0;c<4;++c)for(unsigned row=0;row<3;++row){
        float s=a[row]*b[c*4]+a[4+row]*b[c*4+1]+a[8+row]*b[c*4+2];if(c==3)s+=a[12+row];r[c*4+row]=s;}
    r[15]=1;return r;
}
inline V3 point(const Mat& m,V3 p) noexcept {return {m[0]*p[0]+m[4]*p[1]+m[8]*p[2]+m[12],m[1]*p[0]+m[5]*p[1]+m[9]*p[2]+m[13],m[2]*p[0]+m[6]*p[1]+m[10]*p[2]+m[14]};}
inline V3 direction(const Mat& m,V3 v) noexcept {return {m[0]*v[0]+m[4]*v[1]+m[8]*v[2],m[1]*v[0]+m[5]*v[1]+m[9]*v[2],m[2]*v[0]+m[6]*v[1]+m[10]*v[2]};}
// General affine inverse (3x3 by cofactors). False when singular / not finite.
inline bool affineInverse(const Mat& m,Mat& out) noexcept {
    const float a=m[0],b=m[4],c=m[8],d=m[1],e=m[5],f=m[9],g=m[2],h=m[6],i=m[10];
    const float A=e*i-f*h,B=-(d*i-f*g),Cc=d*h-e*g,det=a*A+b*B+c*Cc;
    if(!std::isfinite(det)||std::fabs(det)<1e-18f)return false;
    const float k=1.f/det;Mat r{};
    r[0]=A*k;r[4]=-(b*i-c*h)*k;r[8]=(b*f-c*e)*k;r[1]=B*k;r[5]=(a*i-c*g)*k;r[9]=-(a*f-c*d)*k;r[2]=Cc*k;r[6]=-(a*h-b*g)*k;r[10]=(a*e-b*d)*k;
    const V3 t=direction(r,{m[12],m[13],m[14]});r[12]=-t[0];r[13]=-t[1];r[14]=-t[2];r[15]=1;out=r;
    for(float x:out)if(!std::isfinite(x))return false;return true;
}
inline V3 toLocal(const Mat& g,V3 v) noexcept {const V3 x=normalized(col(g,0)),y=normalized(col(g,1)),z=normalized(col(g,2));return normalized({dot(x,v),dot(y,v),dot(z,v)});}
inline V3 axisRotate(V3 v,V3 a,float r) noexcept {
    const float c=std::cos(r),s=std::sin(r),d=dot(a,v);const V3 x{a[1]*v[2]-a[2]*v[1],a[2]*v[0]-a[0]*v[2],a[0]*v[1]-a[1]*v[0]};
    return {v[0]*c+x[0]*s+a[0]*d*(1-c),v[1]*c+x[1]*s+a[1]*d*(1-c),v[2]*c+x[2]*s+a[2]*d*(1-c)};
}
inline V3 perp(V3 v,V3 unitAxis) noexcept {return sub(v,mul(unitAxis,dot(v,unitAxis)));}
// The swung local: rotate base (in its parent's frame) by 'radians' about 'axis' (parent frame, unit, = barrel) through the pivot
// = base origin - up*pivotDown + (axis x up)*pivotSide (crane below the cylinder axis). Pure; host-tested.
inline Mat swingLocal(const Mat& base,V3 axis,V3 up,float radians,float pivotDown,float pivotSide) noexcept {
    axis=normalized(axis);up=normalized(up);
    const V3 side=normalized({axis[1]*up[2]-axis[2]*up[1],axis[2]*up[0]-axis[0]*up[2],axis[0]*up[1]-axis[1]*up[0]});
    const V3 t=col(base,3);V3 pivot{};for(unsigned i=0;i<3;++i)pivot[i]=t[i]-up[i]*pivotDown+side[i]*pivotSide;
    Mat out=base;
    for(unsigned c=0;c<3;++c){const auto v=axisRotate(col(base,c),axis,radians);for(unsigned i=0;i<3;++i)out[c*4+i]=v[i];}
    V3 d{};for(unsigned i=0;i<3;++i)d[i]=t[i]-pivot[i];const auto r=axisRotate(d,axis,radians);
    for(unsigned i=0;i<3;++i)out[12+i]=pivot[i]+r[i];
    out[3]=out[7]=out[11]=0;out[15]=1;return out;
}
inline V3 swingPivot(const Mat& base,V3 axis,V3 up,float pivotDown,float pivotSide) noexcept {
    axis=normalized(axis);up=normalized(up);const V3 side=normalized(cross(axis,up));const V3 t=col(base,3);
    return add(sub(t,mul(up,pivotDown)),mul(side,pivotSide));
}
inline bool finiteMat(const Mat& m) noexcept {for(float x:m)if(!std::isfinite(x))return false;return true;}
// Pose (LOCAL metres) -> 4x4.
inline Mat poseMat(const ::rdrvr::Pose& p) noexcept {
    const Q q{p.orientation[0],p.orientation[1],p.orientation[2],p.orientation[3]};
    const V3 x=C::rot(q,{1,0,0}),y=C::rot(q,{0,1,0}),z=C::rot(q,{0,0,1});
    return {x[0],x[1],x[2],0,y[0],y[1],y[2],0,z[0],z[1],z[2],0,p.position[0],p.position[1],p.position[2],1};
}

// =====================================================================================================================
// Configuration (startup properties debug.rdrvr.vr.reload.*; written once before any thread uses them).
enum class Mode : int {Single=0,Full=1,Native=2};
inline const char* modeName(Mode m) noexcept {return m==Mode::Single?"SINGLE":m==Mode::Full?"FULL":"NATIVE";}
struct Config {
    bool enabled{true};                                    // debug.rdrvr.vr.reload (Feature::Reload): 0 = NATIVE forced, nothing installed
    float swingDeg{80.f};std::uint64_t swingNs{150'000'000};float pivotDown{.018f},pivotSide{0.f},sign{-1.f}; // .swing "deg down side sign", .ms
    int boneIndex{-1};std::array<char,32> boneName{};      // .bone
    int modeOverride{-1};                                  // .mode single|full|native (-1 = the menu row)
    int side{0};                                           // .side auto (0) | left (1: the configured sign for both hands) | right (2: mirrored)
    float chamberMm{-1.f},rearMm{-19.f};int rotSign{1};    // .chamber "radiusMm rearMm" (radius -1 = auto from the bore), .rotsign
    float insertMm{30.f},insertDeg{85.f},frontMm{15.f},fullMm{30.f},fullDeg{50.f}; // .insert "mm deg frontMm", .full "mm deg"
    V3 pinch{.065f,.015f,0.f};                             // .pinch "fwd up side" (m, the free grip frame along its measured aim)
    bool zoneAny{};float zoneChest{.28f},zoneBelt{.22f};   // .zone body|any, .zoneChest, .zoneBelt (m)
    float shake{.7f};std::uint64_t tiltNs{120'000'000};bool rod{true},keepLive{true},autoIndex{true},holsterRefill{}; // .shake .tiltMs .rod .keepLive .autoIndex .holsterRefill
    float tiltDeg{35.f};bool brass{true};                // .tiltDeg (muzzle up -> ALL rounds fall out, also after seating), .brass 0 = no falling rounds
    std::uint64_t longNs{600'000'000},shotTimeoutNs{300'000'000};                    // .longMs (0 = off), .shotTimeoutMs
    bool block{true},dry{true},audit{true},debug{},forceOpen{};                       // .block .dry .audit .debug .forceOpen
    std::uint64_t ownerFreshNs{250'000'000};                                           // the owner / context freshness of the manual gate
    std::uint64_t hookGraceNs{1'000'000'000};                                          // .hookGraceMs: an UNMANAGED on-hand revolver is blocked this long
    C::Config close{};
    // ROLL (turning the cylinder by hand): .roll "0|1 reachCm gain"
    bool roll{true};float rollReach{.05f},rollGain{1.f};
    // CHAMBERS REALISTIC cylinder spin (.cylSpin "tauS coulomb maxRadS startRadS stopRadS gain"): a released swipe keeps the cylinder
    // turning at the hand's surface speed (bone rad/s, x gain), viscous decay tau + a constant (Coulomb) deceleration; below stop it clicks into the
    // detent ahead. A 0.4 m/s swipe ~ 1.5 s / 2 rev, a hard 1.5 m/s flick ~ 2.5 s / 9 rev.
    float spinTau{.8f},spinCoulomb{3.f},spinMax{80.f},spinStart{6.f},spinStop{2.5f},spinGain{1.f};
};
inline Config config{};
inline std::atomic<bool> started{};
// The clock of the entry points that get no `now` (VrControls consumeB, the hooks, the status / report): steady_clock = CLOCK_MONOTONIC on
// Android (the same clock as the input thread's / the executor's now). Host tests replace it.
using ClockFn=std::uint64_t(*)() noexcept;
inline std::atomic<ClockFn> clockFn{&::rdrvr::vr::nowNs};
inline std::uint64_t clockNow() noexcept {return clockFn.load(std::memory_order_relaxed)();}
// WeaponClass values (engine::WeaponClass: Unknown, Pistol, DualPistol, ...) published in holsters::heldKind.
inline constexpr int kindPistol=1,kindDualPistol=2;
inline bool revolverKind(int k) noexcept {return k==kindPistol||k==kindDualPistol;}

// ---- install (TraceEntry): any missing piece -> effective NATIVE, reason in the report / row UNAVAILABLE ----
enum class Install : int {NotAttempted,Installed,Killed,EngineMismatch,NoHook,NoExecutor,NoHolsters};
inline const char* installName(Install s) noexcept {
    switch(s){case Install::NotAttempted:return "NOT ATTEMPTED";case Install::Installed:return "INSTALLED";case Install::Killed:return "KILLED";
    case Install::EngineMismatch:return "ENGINE MISMATCH";case Install::NoHook:return "NO HOOK";case Install::NoExecutor:return "NO EXECUTOR";
    case Install::NoHolsters:return "NO HOLSTERS";}return "?";}
inline std::atomic<int> installState{int(Install::NotAttempted)};
inline std::atomic<bool> hookInstalled{},auditInstalled{},printsOk{},executorOk{};
inline bool installed() noexcept {return installState.load(std::memory_order_acquire)==int(Install::Installed);}
// Effective mode: kill / install / the property / the menu row.
inline Mode effectiveMode() noexcept {
    if(!config.enabled||killed(Feature::Reload)||!installed())return Mode::Native;
    if(config.modeOverride>=0&&config.modeOverride<=2)return Mode(config.modeOverride);
    return Mode(settings().reloadMode());
}
inline const char* modeSource() noexcept {
    if(!config.enabled||killed(Feature::Reload))return "KILLED";if(!installed())return "INSTALL";return config.modeOverride>=0?"PROP":"MENU";}
inline unsigned closeRow() noexcept {return settings().cylinderCloseMode();} // 0 B ONLY, 1 B AND HAND, 2 ALL
// CHAMBERS REALISTIC (menu row; the manual reload not NATIVE): the chamber aligned with the barrel decides every shot, no AUTO INDEX,
// cocking / the trigger turns the cylinder one chamber first, the closed cylinder spins under a swipe and shows its roll.
inline bool realisticChambers() noexcept {return effectiveMode()!=Mode::Native&&settings().realisticChambers();}

// =====================================================================================================================
// Cylinder phase (input thread mostly; engine threads read the eased amount). One mutex, tiny critical sections.
enum class Phase : int {Closed,Opening,Open,Closing};
inline const char* phaseName(Phase p) noexcept {
    switch(p){case Phase::Closed:return "CLOSED";case Phase::Opening:return "OPENING";case Phase::Open:return "OPEN";case Phase::Closing:return "CLOSING";}
    return "?";}
struct State {
    Phase phase{Phase::Closed};std::uint64_t startNs{},spanNs{},openedNs{},closedNs{};std::uint32_t closeSeq{};
    CloseSource lastClose{CloseSource::None};std::uint64_t lastGestureCloseNs{};int closeHand{-1};
};
inline std::mutex stateMutex;inline State state{};
inline std::atomic<std::uint32_t> closeSeqWord{};inline std::atomic<std::uint64_t> closedAtWord{},openedAtWord{};
inline std::atomic<std::uint64_t> opens{},opensRefusedBusy{},snaps{},closeCounts[unsigned(CloseSource::Count)]{};
inline std::atomic<const char*> lastSnapReason{"NONE"};
// visual follow of PUSH (VrReloadClose.h): the smoothed amount + stamp; stale (> 100 ms) = 1
inline std::atomic<float> followV{1.f};inline std::atomic<std::uint64_t> followNs{};
inline std::atomic<float> rollWord{};inline std::atomic<std::uint64_t> rollNs{};
inline std::atomic<std::uint64_t> rollSignFlips{};inline std::atomic<int> rollLastSign{0},rollBoneSense{0}; // The self-calibrating visual sign
inline std::atomic<std::uint64_t> rollContacts{},rollDetents{},rollSteps{};
inline std::atomic<std::uint64_t> rollActivations{},rollBlockSamples{},rollBlockedFires{};inline std::atomic<int> rollWorldSense{1},rollGripSense{1};
// CHAMBERS REALISTIC: shots decided by the aligned chamber (live / dry), cylinder advances (all / by a cock), spins (released swipes with
// momentum), the revolutions they turned (milli-rev total, last), the free-spin speed (bone rad/s), the advance animation ticks for the brain
inline std::atomic<std::uint64_t> chLiveShots{},chDryClicks{},chAdvances{},chCockAdvances{},cylSpins{},cylSpinMilliRevs{},cylSpinsStopped{};
inline std::atomic<float> cylLastSpinRevs{},cylOmega{};inline std::atomic<int> advanceTicks{};
inline float smooth(float x) noexcept {x=std::clamp(x,0.f,1.f);return x*x*(3.f-2.f*x);}
// Inverse of smooth on [0,1]: 3 Newton steps from x (monotone, error < 1e-5).
inline float smoothInv(float y) noexcept {y=std::clamp(y,0.f,1.f);float x=y;
    for(int i=0;i<3;++i){const float f=x*x*(3.f-2.f*x)-y,d=6.f*x*(1.f-x);if(d<1e-4f)break;x=std::clamp(x-f/d,0.f,1.f);}
    for(int i=0;i<8&&std::fabs(x*x*(3.f-2.f*x)-y)>1e-6f;++i){const float f=x*x*(3.f-2.f*x)-y,d=std::max(6.f*x*(1.f-x),1e-3f);x=std::clamp(x-f/d,0.f,1.f);}
    return x;}
inline void markClosedLocked(std::uint64_t at) noexcept {
    state.phase=Phase::Closed;state.closedNs=at;++state.closeSeq;state.startNs=0;
    closeSeqWord.store(state.closeSeq,std::memory_order_release);closedAtWord.store(at,std::memory_order_release);
    H::cylinderClosedNs.store(at,std::memory_order_release);H::cylinderFollow.store(false,std::memory_order_relaxed);
}
// Advances Opening->Open / Closing->Closed (caller holds the mutex). Returns the linear open fraction 0..1.
inline float settleLocked(std::uint64_t now) noexcept {
    const std::uint64_t spanNs=state.spanNs?state.spanNs:std::max<std::uint64_t>(config.swingNs,1);const float span=float(spanNs);
    const float t=state.startNs&&now>state.startNs?float(now-state.startNs)/span:0.f;
    switch(state.phase){
        case Phase::Closed:return 0.f;
        case Phase::Open:return 1.f;
        case Phase::Opening:if(t>=1.f){state.phase=Phase::Open;state.openedNs=state.startNs+spanNs;openedAtWord.store(state.openedNs,std::memory_order_relaxed);return 1.f;}
            return std::clamp(t,0.f,1.f);
        case Phase::Closing:if(t>=1.f){markClosedLocked(state.startNs+spanNs);return 0.f;}return 1.f-std::clamp(t,0.f,1.f);
    }
    return 0.f;
}
inline Phase phase(std::uint64_t now) noexcept {std::lock_guard lock(stateMutex);settleLocked(now);return state.phase;}
inline State stateSnapshot(std::uint64_t now) noexcept {std::lock_guard lock(stateMutex);settleLocked(now);return state;}
// Eased amount for apply() (any thread): smooth(l) by the phase; in OPEN the PUSH follow value when fresh.
inline float visualAmount(std::uint64_t now) noexcept {
    std::lock_guard lock(stateMutex);const float l=settleLocked(now);
    if(state.phase==Phase::Open){const auto ns=followNs.load(std::memory_order_relaxed);
        if(ns&&now>=ns&&now-ns<=100'000'000ull){const float f=followV.load(std::memory_order_relaxed);if(f<1.f)return std::clamp(f,0.f,1.f);}}
    return smooth(l);
}
inline float amount(std::uint64_t now) noexcept {return visualAmount(now);}
inline bool cylinderClosed(std::uint64_t now) noexcept {return phase(now)==Phase::Closed;}
inline std::uint64_t closedAtNs() noexcept {return closedAtWord.load(std::memory_order_acquire);}
inline std::uint32_t closeSeq() noexcept {return closeSeqWord.load(std::memory_order_acquire);}
// True while firing must be blocked (cylinder not fully closed).
inline bool fireBlocked(std::uint64_t now) noexcept {return phase(now)!=Phase::Closed;}
inline int phaseCode(std::uint64_t now) noexcept {return int(phase(now));} // holsters PhaseSink: 0 closed 1 opening 2 open 3 closing
inline void open(std::uint64_t now) noexcept { // CLOSED / CLOSING -> OPENING (continuous when reversed mid-swing)
    std::lock_guard lock(stateMutex);const float a=settleLocked(now);
    if(state.phase==Phase::Open||state.phase==Phase::Opening)return;
    const auto span=config.swingNs;state.spanNs=span;
    state.phase=Phase::Opening;state.startNs=now-std::uint64_t(double(a)*double(span));opens.fetch_add(1,std::memory_order_relaxed);
}
// Any thread. fromAmount = the current SMOOTHED visible amount (PUSH follow) or < 0 = continue from the phase. true = a close stroke began.
inline bool requestClose(CloseSource src,int hand,std::uint64_t now,float fromAmount=-1.f) noexcept {
    std::lock_guard lock(stateMutex);const float l=settleLocked(now);
    if(state.phase!=Phase::Open&&state.phase!=Phase::Opening)return false;
    const float l0=fromAmount>=0.f?smoothInv(fromAmount):l;
    const std::uint64_t S=(src==CloseSource::B||src==CloseSource::AutoLoad)?config.swingNs:config.close.slamNs;
    state.spanNs=std::max<std::uint64_t>(S,1);state.phase=Phase::Closing;
    state.startNs=now-std::uint64_t(double(1.f-l0)*double(state.spanNs));
    state.lastClose=src;state.closeHand=hand;
    if(src==CloseSource::Flick||src==CloseSource::Wave||src==CloseSource::Push)state.lastGestureCloseNs=now;
    if(unsigned(src)<unsigned(CloseSource::Count))closeCounts[unsigned(src)].fetch_add(1,std::memory_order_relaxed);
    if(fromAmount>=0.f){followV.store(1.f,std::memory_order_relaxed);followNs.store(0,std::memory_order_relaxed);}
    return true;
}
// Weapon left the hand / the manual gate closed / mode change / fallback: instant close (the engine side restores the base pose).
inline void snapClosed(std::uint64_t now,const char* why) noexcept {
    std::lock_guard lock(stateMutex);settleLocked(now);
    if(state.phase==Phase::Closed)return;
    state.lastClose=CloseSource::Snap;markClosedLocked(now);snaps.fetch_add(1,std::memory_order_relaxed);
    closeCounts[unsigned(CloseSource::Snap)].fetch_add(1,std::memory_order_relaxed);lastSnapReason.store(why,std::memory_order_relaxed);
}

// =====================================================================================================================
// Context word + owner (main thread, vrWeaponObserve BEFORE its holdHand early return; the hook / the gate read them, any thread).
// Gap: why the observer had no owner + context. GapNone = World with nothing (ordinary) on hand: a FRESH
// word without a veto (a draw's synchronous FastDraw stays blocked). GapNoContext = an on-hand weapon whose context is unreadable, GapNoWorld =
// the presentation is not World (THEATER chord, CameraOverride / Transition, Unknown) or the observation is not ready: 'not observed' vetoes
// (the hook passes CONTEXT = the game's reload works; the manual gate stays closed). Never a stale word while the observer runs.
enum ContextGap : int {GapNone=0,GapNoContext=1,GapNoWorld=2};
struct Context {
    std::uintptr_t weapon{};int kind{-1};bool valid{};
    bool alive{},controlDisabled{},deadEye{},riding{},ridingVehicle{},vehicleTransition{},duel{},mountedWeapon{},mountingHorse{},
         linkedAnimation{},usingGringo{},cover{};
    int gap{GapNone};                                  // only when !valid
};
enum VetoBit : unsigned {VetoDead=1,VetoControl=2,VetoVehicle=4,VetoTransition=8,VetoMountedWeapon=16,VetoMounting=32,VetoLinked=64,
    VetoGringo=128,VetoDeadEye=256,VetoDeadEyeTail=512,VetoDuel=1024,VetoNoContext=2048,VetoNoWorld=4096};
inline std::atomic<std::uint64_t> contextWord{};       // {ms:48 | veto bits:16}
inline std::atomic<std::uintptr_t> ownerWeapon{};inline std::atomic<std::uint64_t> ownerNs{};inline std::atomic<bool> ownerPoseAllowed{};
inline std::atomic<int> ownerKind{-1};inline std::atomic<std::uint64_t> deadEyeSeenNs{},contextNotes{};
inline std::atomic<std::uint64_t> contextEmptyNotes{},contextNoContextNotes{},contextNoWorldNotes{};
inline constexpr std::uint64_t deadEyeTailNs=3'000'000'000ull;
inline std::uint32_t vetoOf(const Context& c,std::uint64_t now) noexcept {
    std::uint32_t v=0;
    if(!c.alive)v|=VetoDead;if(c.controlDisabled)v|=VetoControl;if(c.ridingVehicle)v|=VetoVehicle;if(c.vehicleTransition)v|=VetoTransition;
    if(c.mountedWeapon)v|=VetoMountedWeapon;if(c.mountingHorse)v|=VetoMounting;if(c.linkedAnimation)v|=VetoLinked;if(c.usingGringo)v|=VetoGringo;
    if(c.deadEye)v|=VetoDeadEye;if(c.duel)v|=VetoDuel;
    const auto seen=deadEyeSeenNs.load(std::memory_order_relaxed);if(!c.deadEye&&seen&&now>=seen&&now-seen<deadEyeTailNs)v|=VetoDeadEyeTail;
    return v;
}
inline void noteContext(const Context& c,std::uint64_t now) noexcept {
    contextNotes.fetch_add(1,std::memory_order_relaxed);
    if(!c.valid||!c.weapon){ // No owner: the word stays FRESH - empty World = no veto (+ the Dead Eye tail), not observed = a veto
        std::uint32_t veto=c.gap==GapNoWorld?std::uint32_t(VetoNoWorld):c.gap==GapNoContext?std::uint32_t(VetoNoContext):0u;
        const auto seen=deadEyeSeenNs.load(std::memory_order_relaxed);if(seen&&now>=seen&&now-seen<deadEyeTailNs)veto|=VetoDeadEyeTail;
        (c.gap==GapNoWorld?contextNoWorldNotes:c.gap==GapNoContext?contextNoContextNotes:contextEmptyNotes).fetch_add(1,std::memory_order_relaxed);
        contextWord.store(((now/1'000'000ull)<<16)|veto,std::memory_order_release);
        ownerWeapon.store(0,std::memory_order_relaxed);ownerNs.store(now,std::memory_order_relaxed);ownerPoseAllowed.store(false,std::memory_order_relaxed);return;}
    if(c.deadEye)deadEyeSeenNs.store(now,std::memory_order_relaxed);
    const std::uint32_t veto=vetoOf(c,now);
    contextWord.store(((now/1'000'000ull)<<16)|veto,std::memory_order_release);
    // VrWeapon gates().pose without the weapon-kind term (cover and riding are NOT vetoes)
    const bool pose=c.alive&&!c.controlDisabled&&!c.ridingVehicle&&!c.vehicleTransition&&!c.mountedWeapon&&!c.mountingHorse&&!c.linkedAnimation&&!c.usingGringo;
    ownerKind.store(c.kind,std::memory_order_relaxed);ownerPoseAllowed.store(pose,std::memory_order_relaxed);
    ownerWeapon.store(c.weapon,std::memory_order_relaxed);ownerNs.store(now,std::memory_order_release);
}
inline bool contextFresh(std::uint64_t word,std::uint64_t now,std::uint64_t maxNs) noexcept { // |now - stamp| (the input thread may run a bit earlier)
    const std::uint64_t ms=word>>16,n=now/1'000'000ull;return word&&(n>=ms?n-ms:ms-n)<=maxNs/1'000'000ull;}
inline std::uint32_t contextVeto(std::uint64_t now) noexcept {const auto w=contextWord.load(std::memory_order_acquire);
    return contextFresh(w,now,config.ownerFreshNs)?std::uint32_t(w&0xffffu):0u;}

// ---- hook words (executor writes, the hook reads; independent atomics, never a multi-field word) ----
inline std::atomic<bool> armedWord{};
inline std::array<std::atomic<std::uintptr_t>,4> manualWeapons{},fallbackWeapons{};
inline std::array<std::atomic<std::uintptr_t>,8> cylinderTypes{};
inline std::atomic<std::uint64_t> allowanceUntilNs{};inline std::atomic<int> allowance{};
// The on-hand manual revolver the hook would block although the manual process does not run for it (no record yet, not VR-held,
// geometry / cylinder never FOUND, ...), stamped by the executor tick (since first, then the weapon; cleared weapon first). The hook blocks it
// only for config.hookGraceNs after `since` (the draw transient: FastDraw inside selectItem, the record a few frames later), then passes UNMANAGED.
inline std::atomic<std::uintptr_t> unmanagedWeapon{};inline std::atomic<std::uint64_t> unmanagedSinceNs{};
inline bool inList(const std::array<std::atomic<std::uintptr_t>,4>& a,std::uintptr_t w) noexcept {
    if(!w)return false;for(const auto& x:a)if(x.load(std::memory_order_acquire)==w)return true;return false;}
inline bool typeKnown(std::uintptr_t t) noexcept {if(!t)return false;for(const auto& x:cylinderTypes)if(x.load(std::memory_order_acquire)==t)return true;return false;}
// armed = feature on, mode != NATIVE, hook + prints + executor OK, holsters enabled + engineReady (placement enabled is the owner's job)
inline bool armedNow() noexcept {
    return effectiveMode()!=Mode::Native&&H::engineReady.load(std::memory_order_acquire)&&H::config().enabled&&!H::killedByProperty.load(std::memory_order_relaxed);}

// =====================================================================================================================
// Chamber model (per weapon record) - ONE owner of the mutation: gate() (input thread, RT decision) and the executor tick
// (main thread: acks, fires, timeouts, reconcile) under modelMutex. The Brain applies its own ops (insert / eject / auto-load) under it too.
enum class ChamberState : std::uint8_t {Empty,Live,Spent,Firing};
inline char chamberChar(ChamberState s) noexcept {return s==ChamberState::Live?'L':s==ChamberState::Spent?'S':s==ChamberState::Firing?'F':'E';}
inline constexpr int maxChambers=9;
struct Record {
    std::uintptr_t weapon{},type{};int n{};std::array<ChamberState,maxChambers> st{};int next{};
    std::uint64_t createdNs{},seenNs{};bool fallback{};std::uint32_t autoIndexedSeq{};
    bool cockAdvanced{}; // REALISTIC: a cock already turned the cylinder for the coming shot (cleared by the shot / a close)
    int live() const noexcept {int k=0;for(int i=0;i<n;++i)k+=st[std::size_t(i)]==ChamberState::Live;return k;}
    int count(ChamberState s) const noexcept {int k=0;for(int i=0;i<n;++i)k+=st[std::size_t(i)]==s;return k;}
};
enum class Verdict : std::uint8_t {Pass,PassNative,BlockEmpty,BlockSpent,BlockOneShot};
enum class GateSource : std::uint8_t {Trigger,FanStroke,SaTrigger};
using Source=GateSource;
inline const char* verdictName(Verdict v) noexcept {
    switch(v){case Verdict::Pass:return "PASS";case Verdict::PassNative:return "PASS_NATIVE";case Verdict::BlockEmpty:return "EMPTY_CHAMBER";
    case Verdict::BlockSpent:return "SPENT_CHAMBER";case Verdict::BlockOneShot:return "ONE_SHOT";}return "?";}
struct PendingShot {bool on{};int chamber{-1};std::uint64_t ns{};std::uint32_t press{};std::uintptr_t weapon{};int next{};std::uint32_t decision{};};
enum OpKind : std::uint16_t {OpInsert=1,OpEject=2,OpDry=3};
struct InFlight {
    bool on{},acked{},applied{},ok{};std::uint32_t id{},seq{};std::uint16_t kind{};std::uintptr_t weapon{};std::uint16_t chambers{};int n{},live{};
    std::uint64_t postedNs{},maxAgeNs{};int added{};float returned{},liveLostCap{};const char* fail{};
};
inline constexpr std::uint32_t opMaxAgeMs=250;
inline std::mutex modelMutex;
inline std::array<Record,4> records{};            // LRU by seenNs
inline std::array<PendingShot,8> pendingShots{};
inline std::array<InFlight,16> inFlight{};
inline std::uint32_t nextOpId{1},pressCounter{},currentPress{},shotPress{},decisions{};
inline std::array<std::atomic<std::uint64_t>,4> fireCounts{};  // engine fires per record slot (noteEngineFire, any thread)
inline std::array<std::uint64_t,4> firesApplied{};             // model lock
inline Record* recordForLocked(std::uintptr_t w) noexcept {if(!w)return nullptr;for(auto& r:records)if(r.weapon==w&&r.n)return &r;return nullptr;}
inline int slotOfLocked(const Record* r) noexcept {return r?int(r-records.data()):-1;}
// ---- counters (report) ----
struct Counters {
    std::atomic<std::uint64_t> shotPasses{},shotConfirmed{},shotRefused{},shotUnseen{},shotWithoutLive{},oneShotBlocks{},
        reconcileDrains{},reconcileRefills{},reconcileDebounced{},reconcileSkippedRace{},mismatchMax{},
        opsPosted{},opsPostFailed{},opsDone{},opsFailed{},opsExpired{},opsInserted{},opsRollbacks{},ejectReturned{},liveLostCap{},liveLost{},
        rtEdges{},rtForwarded{},rtDry{},rtBlockedOpen{},rtNative{},autoIndexed{},recordsCreated{},recordsDropped{},holsterRefillBlocked{},
        fallbackNoFrame{},longNative{},longAutoload{},presses{},pressesB{},bAfterGesture{},bSpin{},bEdgesPad{},consumeOwned{},consumeCalls{},
        opens{},rtWhileOpen{},handBackMasked{},grabsSingle{},grabsFull{},grabNoAmmo{},grabNoEmpty{},grabBusy{},grabOutside{},inserts{},roundsSeated{},
        insertOccupied{},insertSpent{},drops{},zoneEnters{},ejectShake{},ejectRod{},ejectTilt{},ejectAfterSeat{},ejectNoSpent{},ejectNothing{},
        ejectsAfterSeat{},ejectedSpent{},ejectedLive{},tiltAborted{},openAbandoned{},reloadMsCount{},reloadMsSum{},reloadMsMax{},
        frames{},frameStale{},framePublished{},overlayPublished{},overlayUnavailable{},refuseTicks{},closeTicks{},touchTicks{},magnetTicks{},
        latestMiss{}, // GameXr: a held Update without a matching fresh VrWeapon placement (no ChamberFrame this Update)
        unmanagedStarts{},longAfterRefusal{}; // The hook's UNMANAGED grace started; a long press after a refused open (HAND_BUSY / B_AFTER_GESTURE)
    std::array<std::atomic<std::uint64_t>,elf::CauseCount> refillCause{};
    std::array<std::atomic<std::uint64_t>,C::RefuseCount> refused{};
    std::array<std::atomic<std::uint64_t>,8> nearMissMm{},nearMissDeg{};
    std::atomic<std::uint64_t> refillOpExpired{},refillLongPress{},refillContext{},refillDuel{};
    std::atomic<float> grabOutsideLastM{},radiusMm{},rearMmUsed{};std::atomic<bool> radiusAuto{};
    std::array<std::atomic<float>,8> shakeSub{};std::atomic<unsigned> shakeSubN{};
};
inline Counters ctr{};
inline void bump(std::atomic<std::uint64_t>& a,std::uint64_t n=1) noexcept {a.fetch_add(n,std::memory_order_relaxed);}
inline void maxTo(std::atomic<std::uint64_t>& a,std::uint64_t v) noexcept {auto m=a.load(std::memory_order_relaxed);while(v>m&&!a.compare_exchange_weak(m,v)){}}

// ---- engine ammo of the owner weapon (main tick -> input thread), seqlock ----
struct EngineAmmo {std::uint64_t ns{};std::uintptr_t weapon{};float clip{},perClip{},pool{},poolMax{};std::int32_t ammoEnum{-1};std::uint32_t infinite{},nativeReloading{},valid{};};
static_assert(sizeof(EngineAmmo)%8==0);
inline H::Seqlock<EngineAmmo> ammoBox{};
inline bool ammoFor(std::uintptr_t w,EngineAmmo& out,std::uint64_t now) noexcept {
    EngineAmmo a{};if(!w||!ammoBox.load(a)||!a.valid||a.weapon!=w||(now>a.ns&&now-a.ns>500'000'000ull))return false;out=a;return true;}

// The chamber the cylinder is indexed to after a close (AUTO INDEX): the first LIVE in rotation order (caller holds modelMutex).
inline void autoIndexLocked(Record& r,std::uint32_t seq) noexcept {
    if(r.autoIndexedSeq==seq)return;r.autoIndexedSeq=seq;
    if(realisticChambers()){r.cockAdvanced=false;return;} // The roll / spin set the aligned chamber; a close lowers the hammer
    if(!config.autoIndex||!r.n||r.st[std::size_t(r.next)]==ChamberState::Live)return;
    for(int i=1;i<r.n;++i){const int c=(r.next+i)%r.n;if(r.st[std::size_t(c)]==ChamberState::Live){r.next=c;bump(ctr.autoIndexed);return;}}
}

// =====================================================================================================================
// Engine API (TraceEntry install; host tests pass fakes). Main thread calls only (executor tick / execute), except read (any thread, hooks).
using ReadFn=bool(*)(void*,std::uintptr_t,void*,std::size_t);
using WriteFn=bool(*)(std::uintptr_t,const void*,std::size_t);
inline bool processWrite(std::uintptr_t address,const void* data,std::size_t n) noexcept {
    if(!address)return false;std::memcpy(reinterpret_cast<void*>(address),data,n);return true;}
using AddAmmoFn=void(*)(void* weapon,void* pool,float n);        // weapWeapon::AddAmmo(AmmoComponent*, float)
using SetAmmoFn=void(*)(void* weapon,void* pool,float x);        // weapWeapon::SetAmmo(AmmoComponent*, float)
using RemoveAmmoFn=float(*)(void* pool,int ammoEnum,float n);    // AmmoComponent::RemoveAmmo(int, float)
using PerClipFn=float(*)(void* weapon);                          // weapWeapon::GetAmmoPerClip()
using DryFireFn=void(*)(void* weapon);                           // weapWeapon::DryFire()
using RequestReloadFn=bool(*)(void* aim);                        // aniAim::RequestReload()
struct Env {
    ReadFn read{};void* context{};WriteFn write{&processWrite};
    std::uintptr_t base{},weaponItemTable{};                     // engineBase (LR offsets), vtable for invWeapon + 0x10
    AddAmmoFn addAmmo{};SetAmmoFn setAmmo{};RemoveAmmoFn removeAmmo{};PerClipFn perClip{};DryFireFn dryFire{};
    RequestReloadFn requestReloadOriginal{};AddAmmoFn addAmmoOriginal{};SetAmmoFn setAmmoOriginal{};
    template<class T> bool get(std::uintptr_t p,std::size_t off,T& out) const noexcept {
        return read&&p&&p<=UINTPTR_MAX-off-sizeof(T)&&read(context,p+off,&out,sizeof(T));}
    explicit operator bool() const noexcept {return read&&weaponItemTable&&addAmmo&&setAmmo&&removeAmmo&&perClip&&dryFire;}
};
inline Env env{};inline std::atomic<bool> envReady{};
inline std::uint64_t threadId() noexcept {
#if defined(__ANDROID__)
    return std::uint64_t(::syscall(SYS_gettid));
#else
    return 0;
#endif
}
struct TidSet {std::array<std::atomic<std::uint64_t>,4> ids{};
    void note(std::uint64_t t) noexcept {for(auto& x:ids){auto v=x.load(std::memory_order_relaxed);if(v==t)return;if(!v&&x.compare_exchange_strong(v,t))return;}}};
inline TidSet hookTids{},auditTids{},fireTids{};

// ---- ops (input thread posts under modelMutex, main thread executes / acks / expires) ----
inline std::uint32_t postOpLocked(OpKind kind,std::uintptr_t w,int n,int live,std::uint16_t chambers,std::uint64_t now) noexcept {
    InFlight* slot=nullptr;for(auto& f:inFlight)if(!f.on){slot=&f;break;}
    std::uint32_t id=nextOpId++;if(!id)id=nextOpId++;
    mainthread::Command c{};c.module=mainthread::ModuleReload;c.op=std::uint16_t(kind);
    c.arg[0]=std::int32_t(std::uint32_t(std::uint64_t(w)&0xffffffffu));c.arg[1]=std::int32_t(std::uint32_t(std::uint64_t(w)>>32));
    c.arg[2]=kind==OpEject?live:n;c.arg[3]=std::int32_t(id);c.value[0]=config.keepLive?1.f:0.f;c.postedNs=now;c.maxAgeMs=opMaxAgeMs;
    if(kind!=OpDry&&!slot){bump(ctr.opsPostFailed);return 0;}
    const auto seq=mainthread::post(c,now);
    if(!seq){bump(ctr.opsPostFailed);return 0;}
    bump(ctr.opsPosted);
    if(kind!=OpDry&&slot){*slot=InFlight{};slot->on=true;slot->id=id;slot->seq=seq;slot->kind=kind;slot->weapon=w;slot->chambers=chambers;slot->n=n;slot->live=live;
        slot->postedNs=now;slot->maxAgeNs=std::uint64_t(opMaxAgeMs)*1'000'000ull;}
    return id;
}
// p = sum of unacked ops already applied to the model (OpInsert +n, OpEject -live) for weapon w (caller holds modelMutex)
inline int unackedLocked(std::uintptr_t w) noexcept {int p=0;for(const auto& f:inFlight)if(f.on&&!f.applied&&f.weapon==w)p+=f.kind==OpInsert?f.n:-f.live;return p;}

// =====================================================================================================================
// The shared RT gate (one call per physical edge from the holsters route(), or HAMMER's stroke). Input thread.
inline std::atomic<std::uint32_t> rtPressWord{};      // the press id whose shot the executor confirmed (ONE_SHOT)
inline std::atomic<std::uint32_t> rtCurrentPress{};   // the press id of the last Pass
// Haptic helper.
inline void pulse(int hand,float amplitude,std::uint32_t us,std::uint64_t now,std::uint16_t source=haptics::SourceReload) noexcept {
    if(hand==0||hand==1)haptics::pulse(unsigned(hand),amplitude,us,now,source);}
inline bool manualActive(std::uint64_t now) noexcept; // defined below (needs the frame / ammo facts)
inline Verdict gate(GateSource source,int hold,std::uint64_t now) noexcept {
    bump(ctr.rtEdges);
    if(!manualActive(now)){bump(ctr.rtNative);return Verdict::PassNative;}
    const auto w=ownerWeapon.load(std::memory_order_acquire);const auto seq=closeSeq();
    Verdict v=Verdict::PassNative;bool dry=false;
    {std::lock_guard lock(modelMutex);
        Record* r=recordForLocked(w);if(!r||r->fallback){bump(ctr.rtNative);return Verdict::PassNative;}
        autoIndexLocked(*r,seq);++decisions;
        const std::uint32_t press=source==GateSource::Trigger?++pressCounter:pressCounter;
        if(realisticChambers()){ // The cylinder turns one chamber BEFORE the fall (unless a cock already turned it), then the aligned
            // chamber decides: LIVE fires (engine fire as SIMPLE, the chamber becomes SPENT on the confirmed shot and STAYS aligned), SPENT / EMPTY =
            // a dry click only (no engine fire, no ammo use, the trigger edge never reaches the game: no native auto-reload either)
            if(r->cockAdvanced)r->cockAdvanced=false;
            else{r->next=(r->next+1)%r->n;bump(chAdvances);advanceTicks.fetch_add(1,std::memory_order_relaxed);}
            auto& c=r->st[std::size_t(r->next)];
            if(c==ChamberState::Live){
                c=ChamberState::Firing;PendingShot* ps=nullptr;for(auto& p:pendingShots)if(!p.on){ps=&p;break;}
                if(!ps){ps=&pendingShots[0];for(auto& p:pendingShots)if(p.ns<ps->ns)ps=&p;}
                *ps=PendingShot{true,r->next,now,press,w,r->next,decisions};
                rtCurrentPress.store(press,std::memory_order_release);bump(ctr.shotPasses);bump(ctr.rtForwarded);bump(chLiveShots);v=Verdict::Pass;
            }else{
                v=c==ChamberState::Spent?Verdict::BlockSpent:Verdict::BlockEmpty;
                if(config.dry)postOpLocked(OpDry,w,0,0,0,now);
                bump(ctr.rtDry);bump(chDryClicks);dry=true;
            }
        }else{
        auto& c=r->st[std::size_t(r->next)];
        if(c==ChamberState::Live){
            c=ChamberState::Firing;PendingShot* ps=nullptr;for(auto& p:pendingShots)if(!p.on){ps=&p;break;}
            if(!ps){ps=&pendingShots[0];for(auto& p:pendingShots)if(p.ns<ps->ns)ps=&p;} // the oldest is overwritten (it times out anyway)
            *ps=PendingShot{true,r->next,now,press,w,(r->next+1)%r->n,decisions};
            r->next=(r->next+1)%r->n;rtCurrentPress.store(press,std::memory_order_release);bump(ctr.shotPasses);bump(ctr.rtForwarded);v=Verdict::Pass;
        }else{
            v=c==ChamberState::Spent?Verdict::BlockSpent:Verdict::BlockEmpty;
            if(config.dry)postOpLocked(OpDry,w,0,0,0,now);
            r->next=(r->next+1)%r->n;bump(ctr.rtDry);dry=true;
        }
        }
    }
    if(dry)pulse(hold,.25f,10'000,now);
    return v;
}
// CHAMBERS REALISTIC: HAMMER's cock (a drag / thumb to full cock, the DUAL auto-cock, a fanned cock + fire) turns the cylinder one chamber
// BEFORE the fall; the next gate() of any source then fires the aligned chamber without turning again. Input thread. false = not applied.
inline bool cockAdvance(int hold,std::uint64_t now) noexcept {
    (void)hold;if(!realisticChambers()||!manualActive(now))return false;
    const auto w=ownerWeapon.load(std::memory_order_acquire);
    {std::lock_guard lock(modelMutex);Record* r=recordForLocked(w);if(!r||r->fallback||r->n<2)return false;
        autoIndexLocked(*r,closeSeq()); // a close since the last decision drops a stale cock first
        r->next=(r->next+1)%r->n;r->cockAdvanced=true;}
    bump(chAdvances);bump(chCockAdvances);advanceTicks.fetch_add(1,std::memory_order_relaxed);return true;
}
// holsters RtHeldSink: false once the executor confirmed the shot of the press that passed (ONE_SHOT: an auto-repeating opMode never
// fires a second chamber from one press).
inline bool rtHeld(std::uint64_t) noexcept {
    const auto cur=rtCurrentPress.load(std::memory_order_acquire);
    if(cur&&rtPressWord.load(std::memory_order_acquire)==cur){bump(ctr.oneShotBlocks);return false;}return true;}
inline int rtGateSink(int source,int hold,std::uint64_t now) noexcept {return int(gate(GateSource(source),hold,now));}
// Engine fire of a tracked revolver (weapWeapon::FireProjectile entry, any thread): only counted - the executor applies it.
inline void noteEngineFire(std::uintptr_t weapon,std::uint64_t) noexcept {
    if(!weapon)return;for(unsigned i=0;i<manualWeapons.size();++i)if(manualWeapons[i].load(std::memory_order_acquire)==weapon){fireCounts[i].fetch_add(1,std::memory_order_acq_rel);fireTids.note(threadId());return;}}

// =====================================================================================================================
// RequestReload hook (any thread, lock-free; every load guarded; any failure -> pass). Keyed on the
// player's ON-HAND weapon. Blocks only on a FRESH context word without a veto (STALE = not observed
// -> pass; THEATER / no context publish a veto -> CONTEXT) and not for an UNMANAGED on-hand weapon
// older than the grace.
enum HookPass : unsigned {PassNotArmed,PassNotPlayer,PassNotWeapon,PassUnknownWeapon,PassFallback,PassLongPress,PassContext,PassStale,PassUnmanaged,PassCount};
inline constexpr std::array<const char*,PassCount> hookPassNames{"NOT_ARMED","NOT_PLAYER","NOT_WEAPON","UNKNOWN_WEAPON","FALLBACK","LONG_PRESS","CONTEXT",
    "STALE","UNMANAGED"};
inline std::atomic<std::uint64_t> hookCalls{},hookBlocked{};inline std::array<std::atomic<std::uint64_t>,PassCount> hookPassed{};
struct LrEntry {std::atomic<std::uint64_t> lr{},lr2{},n{};};
inline std::array<LrEntry,8> hookLrTop{};
inline void noteLr(std::array<LrEntry,8>& top,std::uint64_t lr,std::uint64_t lr2) noexcept {
    for(auto& e:top){auto v=e.lr.load(std::memory_order_relaxed);if(v==lr){e.n.fetch_add(1,std::memory_order_relaxed);return;}
        if(!v&&e.lr.compare_exchange_strong(v,lr)){e.lr2.store(lr2,std::memory_order_relaxed);e.n.fetch_add(1,std::memory_order_relaxed);return;}}}
struct HookDecision {bool block{};HookPass why{PassCount};std::uintptr_t weapon{};};
inline HookDecision hookDecide(std::uintptr_t aim,std::uint64_t now) noexcept {
    HookDecision d{};
    if(!armedWord.load(std::memory_order_acquire)||!envReady.load(std::memory_order_acquire)||!config.block){d.why=PassNotArmed;return d;}
    std::uintptr_t owner{},actor{},inv{},item{},table{},w{},type{};std::uint32_t flags{};
    if(!env.get(aim,elf::aimOwner,owner)||!env.get(owner,elf::ownerActor,actor)||!env.get(actor,elf::actorFlags,flags)||(flags&3u)!=3u){d.why=PassNotPlayer;return d;}
    if(!env.get(actor,elf::actorInventory,inv)||!env.get(inv,elf::invHand,item)||!env.get(item,0,table)||table!=env.weaponItemTable||
       !env.get(item,elf::itemWeapon,w)||!w){d.why=PassNotWeapon;return d;}
    env.get(w,elf::weaponType,type);d.weapon=w;
    if(!inList(manualWeapons,w)&&!typeKnown(type)){d.why=PassUnknownWeapon;return d;}
    if(inList(fallbackWeapons,w)){d.why=PassFallback;return d;}
    if(allowanceUntilNs.load(std::memory_order_acquire)>=now&&allowance.exchange(0,std::memory_order_acq_rel)>0){d.why=PassLongPress;return d;}
    // block only on a FRESH word (the observer refreshes it every frame, World or not); stale = not observed -> the game's reload
    const auto word=contextWord.load(std::memory_order_acquire);
    if(!contextFresh(word,now,config.ownerFreshNs)){d.why=PassStale;return d;}
    if(word&0xffffu){d.why=PassContext;return d;}
    // an on-hand revolver the manual process has not taken within the grace -> the game's reload (B / auto / trigger stay usable)
    if(unmanagedWeapon.load(std::memory_order_acquire)==w){const auto since=unmanagedSinceNs.load(std::memory_order_acquire);
        if(since&&now>=since&&now-since>=config.hookGraceNs){d.why=PassUnmanaged;return d;}}
    d.block=true;return d;
}
inline std::uint64_t lrOffset(std::uintptr_t lr) noexcept {return lr&&env.base&&lr>=env.base?std::uint64_t(lr-env.base):std::uint64_t(lr);}
#if defined(__GNUC__)||defined(__clang__)
#define RDRVR_RELOAD_RETURN_ADDRESS() reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))
#define RDRVR_RELOAD_FRAME_ADDRESS() reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0))
#else
#define RDRVR_RELOAD_RETURN_ADDRESS() std::uintptr_t(0)
#define RDRVR_RELOAD_FRAME_ADDRESS() std::uintptr_t(0)
#endif
// lr2 = [[x29]+8] of the caller (the next frame of the frame-pointer chain: tail calls attribute to the tail-caller's caller).
inline std::uintptr_t lr2Of(std::uintptr_t fp) noexcept {std::uintptr_t callerFp{},lr2{};if(!fp||!env.get(fp,0,callerFp)||!env.get(callerFp,8,lr2))return 0;return lr2;}
inline bool requestReloadHook(void* aim) {
    hookCalls.fetch_add(1,std::memory_order_relaxed);hookTids.note(threadId());
    const auto lr=RDRVR_RELOAD_RETURN_ADDRESS();
    HookDecision d{};try{d=hookDecide(reinterpret_cast<std::uintptr_t>(aim),clockNow());}catch(...){d=HookDecision{};d.why=PassNotArmed;}
    if(d.block){hookBlocked.fetch_add(1,std::memory_order_relaxed);noteLr(hookLrTop,lrOffset(lr),lrOffset(lr2Of(RDRVR_RELOAD_FRAME_ADDRESS())));return false;}
    if(d.why<PassCount)hookPassed[d.why].fetch_add(1,std::memory_order_relaxed);
    const auto original=env.requestReloadOriginal;return original?original(aim):false;
}
// ---- audit hooks on weapWeapon::AddAmmo / SetAmmo (pass-through; only tracked revolvers are recorded) ----
struct AuditEntry {std::uint64_t ns{},lr{},lr2{};std::uintptr_t weapon{};float value{},before{},after{};std::uint32_t op{},cause{};};
inline std::mutex auditMutex;inline std::array<AuditEntry,16> auditRing{};inline unsigned auditNext{};
inline std::atomic<std::uint64_t> auditAdd{},auditSet{};
inline bool tracked(std::uintptr_t w) noexcept {
    if(inList(manualWeapons,w))return true;std::uintptr_t type{};return env.get(w,elf::weaponType,type)&&typeKnown(type);}
inline void auditRecord(std::uint32_t op,void* weapon,float value,float before,std::uintptr_t lr,std::uintptr_t fp) noexcept {
    const auto w=reinterpret_cast<std::uintptr_t>(weapon);float after{};env.get(w,elf::weaponClip,after);
    AuditEntry e{};e.ns=clockNow();e.lr=lrOffset(lr);e.lr2=lrOffset(lr2Of(fp));e.weapon=w;e.value=value;e.before=before;e.after=after;e.op=op;
    e.cause=elf::causeOf(e.lr);if(e.cause==elf::CauseUnknown)e.cause=elf::causeOf(e.lr2);
    std::lock_guard lock(auditMutex);auditRing[auditNext%auditRing.size()]=e;++auditNext;
}
inline void addAmmoHook(void* weapon,void* pool,float n) {
    const auto lr=RDRVR_RELOAD_RETURN_ADDRESS();const auto fp=RDRVR_RELOAD_FRAME_ADDRESS();
    const auto w=reinterpret_cast<std::uintptr_t>(weapon);float before{};const bool rec=config.audit&&tracked(w)&&env.get(w,elf::weaponClip,before);
    if(const auto o=env.addAmmoOriginal)o(weapon,pool,n);
    if(rec){auditAdd.fetch_add(1,std::memory_order_relaxed);auditTids.note(threadId());auditRecord(1,weapon,n,before,lr,fp);}
}
inline void setAmmoHook(void* weapon,void* pool,float x) {
    const auto lr=RDRVR_RELOAD_RETURN_ADDRESS();const auto fp=RDRVR_RELOAD_FRAME_ADDRESS();
    const auto w=reinterpret_cast<std::uintptr_t>(weapon);float before{};const bool rec=config.audit&&tracked(w)&&env.get(w,elf::weaponClip,before);
    if(const auto o=env.setAmmoOriginal)o(weapon,pool,x);
    if(rec){auditSet.fetch_add(1,std::memory_order_relaxed);auditTids.note(threadId());auditRecord(2,weapon,x,before,lr,fp);}
}
// The newest audited refill cause of weapon w within the last 2 s (reconcile attribution).
inline elf::Cause recentAuditCause(std::uintptr_t w,std::uint64_t now) noexcept {
    std::lock_guard lock(auditMutex);elf::Cause c=elf::CauseUnknown;std::uint64_t best=0;
    for(const auto& e:auditRing)if(e.weapon==w&&e.ns>best&&now>=e.ns&&now-e.ns<2'000'000'000ull&&e.after>e.before){best=e.ns;c=elf::Cause(e.cause);}
    return c;
}

// =====================================================================================================================
// ChamberFrame (published from the held weapon's Update, any thread): everything in the HOLDING grip frame
// (metres) = inverse(latest.map * pose(latest.localGrip)) of the placement this Update used -> rigid with the engine-drawn gun.
// slot k = the k-th chamber position after the one under the hammer in rotation order (chamber i sits in slot (i - next) mod n).
// *Open = the analytic FULL-OPEN constants (insertion, gestures; the visual follow never moves them), *Cur = this frame's visual pose
// (overlay markers).
struct ChamberFrame {
    std::uint64_t weapon{},ns{},referenceSpace{};std::int32_t hand{-1},n{};float swingSign{-1.f},amount{};
    std::array<V3,maxChambers> slotOpen{},slotCur{};
    V3 axis{},up{},rearOpen{},rodOpen{},rearCur{},rodCur{},openCentre{},closedCentre{},pivot{},tangent{},toFrame{};
    float radius{},rear{};std::uint32_t radiusAuto{},valid{};std::int16_t worldSense{1},gripSense{1};
    float rollUsed{};std::int32_t slotDir{1}; // The visual roll sense (world / holding grip frame: -1 = a mirrored parent / grip map) and the roll in slotCur
    // the MEASURED effect of the roll the code writes, holding grip frame: a point P moves by cross(rollOmega, P) + rollT per +1 rad
    // of roll (finite difference of the actual bone matrices); slotDir = the sense of slot k -> k+1 about `axis` (measured from slotOpen)
    V3 rollOmega{},rollT{};
};
static_assert(sizeof(ChamberFrame)%8==0&&std::is_trivially_copyable_v<ChamberFrame>);
inline H::Seqlock<ChamberFrame> chamberBox{};
inline constexpr std::uint64_t frameMaxAgeNs=150'000'000;
// Inputs of the frame (world, engine units; gripToWorld carries the uniform scale = units per metre).
struct FrameIn {
    Mat closed{},current{},full{};   // the cylinder bone's global: closed base, this frame, fully open
    V3 pivot{};                      // the crane pivot (world)
    V3 muzzle{},barrel{},up{};       // bore line: the muzzle locator position, barrel forward (unit), weapon up (unit)
    Mat gripToWorld{};               // latest.map * pose(latest.localGrip)
    int n{6};float swingSign{-1.f},amount{};
    int worldSense{1},gripSense{1};float rollUsed{};
    V3 rollOmega{},rollT{}; // holding grip frame, per +1 rad of roll
    bool modelGeom{};float chamberMm{-1.f},rearMm{-19.f}; // The held model's own chamber ring (false = config, the Cattleman's)
};
inline C::Geometry geometryOf(const ChamberFrame& f) noexcept {
    C::Geometry g{};g.valid=f.valid!=0;g.axis=f.axis;g.up=f.up;g.openCentre=f.openCentre;g.closedCentre=f.closedCentre;g.tangent=f.tangent;
    g.toFrame=f.toFrame;g.radius=f.radius;g.half=std::fabs(f.rear);g.swingSign=f.swingSign;return g;}
// Pure (host-tested). false = degenerate input.
inline bool buildFrame(const FrameIn& in,ChamberFrame& out) noexcept {
    Mat inv{},closedInv{};if(!affineInverse(in.gripToWorld,inv)||!affineInverse(in.closed,closedInv))return false;
    const float s=length(col(in.gripToWorld,0));if(!(s>1e-6f)||!std::isfinite(s))return false;
    const Mat Tfull=multiply(in.full,closedInv),Tcur=multiply(in.current,closedInv);
    const V3 b=normalized(in.barrel),c=col(in.closed,3);
    const V3 off=perp(sub(in.muzzle,c),b);const float rWorld=length(off);
    float r=rWorld/s;std::uint32_t autoR=1;
    const float chamberMm=in.modelGeom?in.chamberMm:config.chamberMm,rearMm=in.modelGeom?in.rearMm:config.rearMm;
    if(chamberMm>0.f){r=chamberMm*.001f;autoR=0;}
    else if(!(r>=.008f&&r<=.030f)){r=.0137f;autoR=0;}
    V3 u0=rWorld>1e-6f*s?mul(off,1.f/rWorld):normalized(perp(in.up,b));
    if(length(u0)<.5f)return false;
    const float rear=rearMm*.001f;const int n=std::clamp(in.n,1,maxChambers);
    ChamberFrame f{};f.n=n;f.swingSign=in.swingSign;f.amount=in.amount;f.radius=r;f.rear=rear;f.radiusAuto=autoR;
    f.worldSense=std::int16_t(in.worldSense<0?-1:1);f.gripSense=std::int16_t(in.gripSense<0?-1:1);f.rollUsed=in.rollUsed;
    f.rollOmega=in.rollOmega;f.rollT=in.rollT;
    const auto g=[&](V3 world){return point(inv,world);};
    for(int i=0;i<n;++i){
        const V3 dir=axisRotate(u0,b,float(config.rotSign)*float(i)*6.2831853f/float(n));
        const V3 mouth=add(add(c,mul(dir,r*s)),mul(b,rear*s));
        f.slotOpen[std::size_t(i)]=g(point(Tfull,mouth));f.slotCur[std::size_t(i)]=g(point(Tcur,mouth));
    }
    const V3 rearC=add(c,mul(b,rear*s)),rod=add(c,mul(b,(std::fabs(rear)+.025f)*s));
    f.rearOpen=g(point(Tfull,rearC));f.rodOpen=g(point(Tfull,rod));f.rearCur=g(point(Tcur,rearC));f.rodCur=g(point(Tcur,rod));
    f.openCentre=g(point(Tfull,c));f.closedCentre=g(c);f.pivot=g(in.pivot);
    f.axis=normalized(direction(inv,b));f.up=normalized(perp(normalized(direction(inv,in.up)),f.axis));
    f.toFrame=normalized(perp(sub(f.closedCentre,f.openCentre),f.axis));
    // closing tangent at O: the swing angle runs sign*deg -> 0, i.e. angular velocity -sign about the barrel (right-hand rule, swingLocal)
    f.tangent=normalized(mul(cross(f.axis,sub(f.openCentre,f.pivot)),-in.swingSign));
    { // The measured slot order sense about f.axis (n = 2: the analytic rotSign x grip-map handedness)
        const float k=n>=3?dot(cross(sub(f.slotOpen[0],f.rearOpen),sub(f.slotOpen[1],f.rearOpen)),f.axis):0.f;
        const float gm=dot(col(in.gripToWorld,0),cross(col(in.gripToWorld,1),col(in.gripToWorld,2)));
        f.slotDir=std::fabs(k)>1e-8f?(k<0.f?-1:1):(config.rotSign*(gm<0.f?-1:1));}
    for(const auto& v:{f.axis,f.up,f.openCentre,f.closedCentre,f.tangent,f.rodOpen,f.rearOpen})if(!C::finite(v))return false;
    f.valid=1;out=f;return true;
}

// =====================================================================================================================
// Engine side of the swing (any thread, inside the VR-held weapon's own weapWeapon::Update = the engine's write window for that skeleton).
// read = guarded read (TraceEntry readMemory); write = in-process store (only inside the weapon's Update).
inline constexpr unsigned maxBones=64;
inline constexpr std::size_t localStride=0x60,globalStride=0x40,boneDataStride=0x110;
struct Skeleton {std::uintptr_t skeleton{},data{},root{},locals{},globals{},parents{},bones{};unsigned count{};};
struct Engine {
    ReadFn read{};void* context{};WriteFn write{&processWrite};
    template<class T> bool get(std::uintptr_t p,std::size_t off,T& out) const noexcept {
        return read&&p&&p<=UINTPTR_MAX-off-sizeof(T)&&read(context,p+off,&out,sizeof(T));}
    bool matrix(std::uintptr_t p,Mat& m) const noexcept {
        if(!get(p,0,m))return false;m[3]=m[7]=m[11]=0;m[15]=1;return finiteMat(m);}
    bool skeleton(std::uintptr_t s,Skeleton& out) const noexcept {
        Skeleton k{};k.skeleton=s;std::uint16_t dataCount{};
        if(!get(s,8,k.data)||!get(s,0x10,k.root)||!get(s,0x18,k.locals)||!get(s,0x20,k.count)||!get(s,0x28,k.globals)||
           !k.count||k.count>maxBones||!get(k.data,0,k.bones)||!get(k.data,8,k.parents)||!get(k.data,0x30,dataCount)||dataCount!=k.count||
           !k.root||!k.locals||!k.globals||!k.parents||!k.bones)return false;
        out=k;return true;
    }
    bool name(const Skeleton& k,unsigned i,std::array<char,32>& out) const noexcept {
        out={};std::uintptr_t text{};if(!get(k.bones+i*boneDataStride,0,text)||!text)return false;
        for(std::size_t n:{std::size_t(31),std::size_t(15),std::size_t(7)})if(read(context,text,out.data(),n)){out[n]=0;return true;}
        return false;
    }
    bool parent(const Skeleton& k,unsigned i,int& p) const noexcept {std::int32_t v{};if(!get(k.parents,i*4,v))return false;p=v;return true;}
    // Write the 12 meaningful lanes of an affine matrix (w lanes untouched).
    bool store(std::uintptr_t p,const Mat& m) const noexcept {
        for(unsigned c=0;c<4;++c)if(!write(p+c*16,&m[c*4],12))return false;return true;}
};
// Bone selection: property index/name, else the first bone whose name contains "cyl" (case-insensitive).
inline bool containsCi(const char* hay,const char* needle) noexcept {
    for(std::size_t i=0;hay[i];++i){std::size_t j=0;while(needle[j]&&hay[i+j]&&std::tolower((unsigned char)hay[i+j])==std::tolower((unsigned char)needle[j]))++j;
        if(!needle[j])return true;}
    return false;
}
inline int chooseBone(const std::array<std::array<char,32>,maxBones>& names,unsigned count,const Config& c) noexcept {
    if(c.boneIndex>=0)return unsigned(c.boneIndex)<count?c.boneIndex:-1;
    if(c.boneName[0]){for(unsigned i=0;i<count;++i)if(!std::strcmp(names[i].data(),c.boneName.data()))return int(i);return -1;}
    for(unsigned i=0;i<count;++i)if(containsCi(names[i].data(),"cyl"))return int(i);
    for(unsigned i=0;i<count;++i)if(containsCi(names[i].data(),"drum"))return int(i);
    return -1;
}
// PER-MODEL HANDGUNS (user, 100% save: on every handgun but the Cattleman "the hammer is offset somewhere completely wrong, the rounds
// are not on the cylinder"). Per model the engine gives the skeleton (bone names + rest locals; dumps: Cattleman "root body cylinder hammer
// flap trigger", type 7 "root body hammer hinge cylinder trigger" = cylinder under a hinge, pistols "body slide (hammer) trigger (lever)"), the
// muzzle locator (bore line -> chamber radius) and the clip (-> chambers). NOT given: the chamber mouth plane, the crane swing, the spur on the
// hammer bone - Cattleman-tuned constants. The model = the hand item's type name ("base_pistol_REVOLVER_Cattleman"), bound to its weapon by
// VrWeapon publishOwner. Only `manual` revolvers run the manual reload / chamber overlay / hammer zone + visual; the rest: the game's reload
// (B) and a plain trigger. Device calibration: debug.rdrvr.vr.reload.models "all" | "none" | "SCHOFIELD,LEMAT" (trust)
// .geom.<KEY> "rearMm chamberMm spurX spurY spurZ" (mm; chamberMm -1 = auto from the bore; spur = hammer .offset of that model).
enum Model : int {ModelUnknown,ModelCattleman,ModelSchofield,ModelDoubleAction,ModelLeMat,ModelVolcanic,ModelSemiAuto,ModelHighPower,ModelMauser,
    ModelOtherRevolver,ModelOtherPistol,ModelCount};
struct ModelSpec {const char* key;const char* needle;bool revolver,doubleAction,manual,geom;float rearMm,chamberMm;V3 spur;};
inline std::array<ModelSpec,ModelCount> models{{ // readConfig (startup) only, read-only after; geom false = config (.chamber / hammer .offset)
    {"UNKNOWN","",false,false,false,false,-19.f,-1.f,{0.f,15.f,-10.f}},
    {"CATTLEMAN","CATTLEMAN",true,false,true,false,-19.f,-1.f,{0.f,15.f,-10.f}},
    {"SCHOFIELD","SCHOFIELD",true,false,false,false,-19.f,-1.f,{0.f,15.f,-10.f}},     // TODO device: values = Cattleman; break-top? (hinge)
    {"DOUBLEACTION","DOUBLE",true,true,false,false,-19.f,-1.f,{0.f,15.f,-10.f}},      // TODO device: values = Cattleman
    {"LEMAT","LEMAT",true,false,false,false,-19.f,-1.f,{0.f,15.f,-10.f}},             // TODO device: values = Cattleman; 9 chambers
    {"VOLCANIC","VOLCANIC",false,false,false,false,-19.f,-1.f,{0.f,15.f,-10.f}},      // lever action, tube magazine: game reload
    {"SEMIAUTO","SEMIAUTO",false,false,false,false,-19.f,-1.f,{0.f,15.f,-10.f}},      // magazine pistols: game reload, trigger fires
    {"HIGHPOWER","HIGHPOWER",false,false,false,false,-19.f,-1.f,{0.f,15.f,-10.f}},
    {"MAUSER","MAUSER",false,false,false,false,-19.f,-1.f,{0.f,15.f,-10.f}},
    {"REVOLVER","_REVOLVER_",true,false,false,false,-19.f,-1.f,{0.f,15.f,-10.f}},     // any other revolver: game reload
    {"PISTOL","_PISTOL_",false,false,false,false,-19.f,-1.f,{0.f,15.f,-10.f}}}};      // last: every handgun name has "base_pistol_"
inline int classifyModel(const char* name) noexcept {
    if(!name||!*name)return ModelUnknown;
    for(int m=1;m<ModelCount;++m){const char* n=models[std::size_t(m)].needle;if(*n&&containsCi(name,n))return m;}
    return ModelUnknown;}
// main thread (VrWeapon publishOwner): the model of the on-hand weapon (name nullptr = its item is not the holsters' hand item yet = unknown)
inline std::atomic<std::uintptr_t> modelWeapon{};inline std::atomic<int> modelWord{ModelUnknown};
inline void publishModel(std::uintptr_t weapon,const char* itemTypeName) noexcept {
    const int m=classifyModel(itemTypeName);
    if(modelWeapon.load(std::memory_order_relaxed)==weapon&&modelWord.load(std::memory_order_relaxed)==m)return;
    modelWeapon.store(0,std::memory_order_release);modelWord.store(m,std::memory_order_relaxed);modelWeapon.store(weapon,std::memory_order_release);}
inline int modelOf(std::uintptr_t weapon) noexcept {return weapon&&modelWeapon.load(std::memory_order_acquire)==weapon?modelWord.load(std::memory_order_relaxed):int(ModelUnknown);}
inline const ModelSpec& specOf(std::uintptr_t weapon) noexcept {return models[std::size_t(std::clamp(modelOf(weapon),0,ModelCount-1))];}
// any thread: the manual reload / chamber overlay / hammer zone + visual run for this weapon (a revolver with trusted geometry)
inline bool modelMechanics(std::uintptr_t weapon) noexcept {const auto& s=specOf(weapon);return s.revolver&&s.manual;}
// Cylinder availability of the current VR-held weapon (engine side): 0 unknown, 1 found, 2 missing (no bone named *cyl*).
enum class Cylinder : int {Unknown,Found,Missing};
inline std::atomic<int> cylinder{int(Cylinder::Unknown)};inline std::atomic<std::uintptr_t> cylinderWeapon{};
// Per-weapon engine record of the swing (engine threads, behind a mutex: at most a few calls per frame).
struct SwingRecord {
    std::uintptr_t weapon{},skeleton{};int bone{-1};unsigned count{};bool baseValid{},written{};Mat base{},last{};std::uint64_t lastNs{};
    std::array<std::array<char,32>,maxBones> names{};std::array<int,maxBones> parents{};
    std::uint64_t enforceNs{};std::uint32_t gen{};   // The (re)draw enforcement window end, the hold generation it was armed for
};
inline std::mutex recordMutex;inline SwingRecord record{};
inline std::atomic<std::uint64_t> applyCalls{},applied{},restored{},baseRefreshed{},declined{},writeFailures{},frameFailures{},lastFrameNs{};
// Clean base: the record can be reset while the bone still carries the mod's swing / roll (another
// weapon drawn in between, a holster / throw / drop mid-reload whose Update did not restore the bone
// within 1 s). A base re-captured from such a bone gives an offset closed pose with every later
// swing stacked on it. So the clean base is kept per INSTANCE (read before the mod's first write,
// never re-captured from the mod's own write) with its dirty state (the mod's write still on the
// bone), plus a per TYPE clean base (skeleton data + bone, the first clean read) as the guard for a
// lost instance entry: a candidate base whose origin is off the type base (a swing moves the cylinder
// centre about the crane pivot; native turns it about its own axis only) is rejected.
// Every write stays absolute: base -> roll -> swing. recordMutex.
struct InstBase {std::uintptr_t weapon{},skeleton{};unsigned count{};int bone{-1};bool dirty{};Mat base{},last{};std::uint64_t useNs{};
    std::array<int,maxBones> parents{};};
struct TypeBase {std::uintptr_t data{};unsigned count{};int bone{-1};Mat base{};};
inline std::array<InstBase,8> instBases{};inline std::array<TypeBase,8> typeBases{};
inline std::atomic<std::uint64_t> baseRestores{},staleOpenFixed{},staleBaseRejected{},instEvicted{},drawEnforce{};
inline constexpr float staleTolM=.001f;                       // 1 mm of the cylinder centre (a 10 deg swing about the crane ~3 mm)
inline constexpr std::uint64_t enforceWindowNs=1'000'000'000ull; // after a (re)draw: a stale bone is put back on its base (written)
inline InstBase* instForLocked(std::uintptr_t w,std::uintptr_t skel) noexcept {
    if(!w)return nullptr;for(auto& i:instBases)if(i.weapon==w&&i.skeleton==skel)return &i;return nullptr;}
inline bool instDirtyLocked(std::uintptr_t w) noexcept {if(!w)return false;for(const auto& i:instBases)if(i.weapon==w&&i.dirty)return true;return false;}
inline InstBase& instNewLocked() noexcept { // an empty slot, else the least recently used clean one, else the least recently used (evicted)
    InstBase* best=nullptr;
    for(auto& i:instBases)if(!i.weapon)return i;
    for(auto& i:instBases)if(!i.dirty&&(!best||i.useNs<best->useNs))best=&i;
    if(!best){for(auto& i:instBases)if(!best||i.useNs<best->useNs)best=&i;instEvicted.fetch_add(1,std::memory_order_relaxed);}
    return *best;}
inline TypeBase* typeForLocked(std::uintptr_t data,unsigned count,int bone) noexcept {
    for(auto& t:typeBases)if(t.data&&t.data==data&&t.count==count&&t.bone==bone)return &t;return nullptr;}
inline void learnTypeLocked(std::uintptr_t data,unsigned count,int bone,const Mat& base) noexcept {
    if(!data||typeForLocked(data,count,bone))return;for(auto& t:typeBases)if(!t.data){t=TypeBase{data,count,bone,base};return;}
    typeBases[0]=TypeBase{data,count,bone,base};}
inline float originDelta(const Mat& a,const Mat& b) noexcept {
    const float x=a[12]-b[12],y=a[13]-b[13],z=a[14]-b[14];const float d=std::sqrt(x*x+y*y+z*z);return std::isfinite(d)?d:1e9f;}
inline std::atomic<const char*> lastDecline{"NONE"};inline std::atomic<float> lastAngleDeg{};
inline bool declineWith(const char* why) noexcept {declined.fetch_add(1,std::memory_order_relaxed);lastDecline.store(why,std::memory_order_relaxed);return false;}
// The swing sign actually applied (side auto mirrors it by the holding hand: the cylinder opens toward the free hand).
inline float swingSignFor(int hold) noexcept {
    const float s0=config.sign<0?-1.f:1.f;
    if(config.side==1)return s0;if(config.side==2)return -s0;
    return hold==0?-s0:s0;
}
// Recompose global[i] and every descendant from locals (index order: parents precede children, SkeletonPoseAudit).
inline bool recomposeP(const Engine& e,const Skeleton& k,const std::array<int,maxBones>& parents,unsigned bone) noexcept {
    std::array<bool,maxBones> dirty{};dirty[bone]=true;
    for(unsigned j=bone;j<k.count;++j){
        if(j!=bone){const int p=parents[j];if(p<0||!dirty[unsigned(p)])continue;dirty[j]=true;}
        Mat local{},parent{};
        if(!e.matrix(k.locals+j*localStride,local))return false;
        const int p=parents[j];
        if(j==0||p<0){if(!e.matrix(k.root,parent))return false;}
        else if(!e.matrix(k.globals+unsigned(p)*globalStride,parent))return false;
        const auto g=multiply(parent,local);if(!finiteMat(g)||!e.store(k.globals+j*globalStride,g))return false;
    }
    return true;
}
inline bool recompose(const Engine& e,const Skeleton& k,const SwingRecord& r,unsigned bone) noexcept {return recomposeP(e,k,r.parents,bone);}
// Puts the mod's leftover write on a weapon that is not the record back, inside that weapon's own
// Update (holstered / thrown / dropped / the dual phantom). The local must still be exactly the
// mod's last write (else native owns it again and nothing is left to restore).
inline bool restoreInstLocked(const Engine& e,InstBase& ib,const Skeleton& k) noexcept {
    if(!ib.dirty||ib.bone<0||unsigned(ib.bone)>=k.count||k.count!=ib.count)return false;const unsigned b=unsigned(ib.bone);
    Mat cur{};if(!e.matrix(k.locals+b*localStride,cur))return false;
    if(cur!=ib.last){ib.dirty=false;return false;}
    if(!e.store(k.locals+b*localStride,ib.base)||!recomposeP(e,k,ib.parents,b)){writeFailures.fetch_add(1,std::memory_order_relaxed);return false;}
    ib.dirty=false;baseRestores.fetch_add(1,std::memory_order_relaxed);return true;}
// The placement the engine-drawn gun used this Update (VrWeapon latestPlacement of THIS weapon / hand): LOCAL metres -> world + the grip.
struct GripMap {bool valid{};Mat map{};::rdrvr::Pose grip{};int hand{-1};std::uint64_t referenceSpace{},ns{};};
inline int chamberCountFor(std::uintptr_t w) noexcept {std::lock_guard lock(modelMutex);const Record* r=recordForLocked(w);return r?r->n:0;}
// Called inside the VR-held weapon's Update (after the native pose of this frame): apply the eased swing, or restore the base; publish the
// ChamberFrame when `grip` is given. modelMuzzle = model^-1 * muzzle (VrWeapon geometry): barrel = -Z, up = +Y of the authored muzzle locator.
inline bool apply(const Engine& e,std::uintptr_t weapon,std::uintptr_t skeletonPtr,const Mat& modelMuzzle,bool held,std::uint64_t now,
                  const GripMap* grip=nullptr) noexcept {
    applyCalls.fetch_add(1,std::memory_order_relaxed);
    if(!config.enabled)return declineWith("DISABLED");
    held=held&&modelMechanics(weapon); // Another model = not ours (our leftover write is restored, no ChamberFrame)
    try{
        std::lock_guard lock(recordMutex);
        if(record.weapon!=weapon||record.skeleton!=skeletonPtr){
            // a weapon that is not the record and no longer VR-held (holstered / thrown / dropped
            // mid-reload, a dual phantom): its instance entry still marks the mod's write -> put the
            // clean base back in its own Update. Never takes the record.
            if(!held){
                InstBase* ib=instForLocked(weapon,skeletonPtr);if(!ib||!ib->dirty)return declineWith("NOT_RECORD");
                Skeleton k{};if(!skeletonPtr||!e.skeleton(skeletonPtr,k))return declineWith("NO_SKELETON");
                ib->useNs=now;return restoreInstLocked(e,*ib,k)?true:declineWith("INST_RESTORE");
            }
            // new held weapon (or the same weapon with a new skeleton): the previous one's leftover
            // lives on in its instance entry (no 1 s wait)
            record=SwingRecord{};record.weapon=weapon;record.skeleton=skeletonPtr;record.enforceNs=now+enforceWindowNs;
            record.gen=H::holdGeneration.load(std::memory_order_relaxed);
        }
        if(held){const auto g=H::holdGeneration.load(std::memory_order_relaxed);if(g!=record.gen){record.gen=g;record.enforceNs=now+enforceWindowNs;}} // a (re)draw
        record.lastNs=now;
        Skeleton k{};if(!skeletonPtr||!e.skeleton(skeletonPtr,k)){if(held)cylinderWeapon.store(weapon);return declineWith("NO_SKELETON");}
        if(!record.count){
            for(unsigned i=0;i<k.count;++i){e.name(k,i,record.names[i]);if(!e.parent(k,i,record.parents[i]))return declineWith("PARENTS_UNREADABLE");
                if(i&&(record.parents[i]<0||unsigned(record.parents[i])>=i))return declineWith("PARENTS_ORDER");}
            record.count=k.count;record.bone=chooseBone(record.names,k.count,config);
        }else if(record.count!=k.count)return declineWith("BONE_COUNT_CHANGED");
        if(held){cylinderWeapon.store(weapon);cylinder.store(int(record.bone>=0?Cylinder::Found:Cylinder::Missing));}
        if(record.bone<0)return declineWith("NO_CYLINDER_BONE");
        const unsigned b=unsigned(record.bone);
        Mat current{};if(!e.matrix(k.locals+b*localStride,current))return declineWith("LOCAL_UNREADABLE");
        const float a=held?visualAmount(now):0.f;
        // the clean closed base of this instance (never re-captured from the mod's own write).
        bool fixStale=false;
        {
            InstBase* ib=instForLocked(weapon,skeletonPtr);
            if(ib&&(ib->count!=k.count||ib->bone!=int(b))){*ib=InstBase{};ib=nullptr;}
            const TypeBase* tb=typeForLocked(k.data,k.count,int(b));
            const bool window=held&&now<record.enforceNs;
            const auto offType=[&](const Mat& m){return tb&&originDelta(m,tb->base)>staleTolM;};
            if(!ib){ // first sight of this instance (or its entry was evicted): its local is the clean base unless it carries a swing
                InstBase& n=instNewLocked();n=InstBase{};n.weapon=weapon;n.skeleton=skeletonPtr;n.count=k.count;n.bone=int(b);n.parents=record.parents;
                n.base=current;
                if(offType(current)){n.base=tb->base;staleBaseRejected.fetch_add(1,std::memory_order_relaxed);
                    if(window){n.dirty=true;n.last=current;fixStale=true;}}
                else if(!tb)learnTypeLocked(k.data,k.count,int(b),current);
                ib=&n;
            }else if(ib->dirty&&current==ib->last){ // the mod's write is still on the bone
                if(!record.written)fixStale=true; // ...but the record is fresh (a redraw after a holster / throw / drop mid-reload): put it back
            }else if(current!=ib->base){ // native rewrote the local (fire / reload clip) or the bone drifted: the new base unless it looks swung
                if(offType(current)){
                    if(ib->base!=tb->base){ib->base=tb->base;staleBaseRejected.fetch_add(1,std::memory_order_relaxed);}
                    if(window){ib->dirty=true;ib->last=current;fixStale=true;}else ib->dirty=false;
                }else{ib->base=current;ib->dirty=false;if(record.written)baseRefreshed.fetch_add(1,std::memory_order_relaxed);}
            }
            ib->useNs=now;ib->parents=record.parents;
            record.base=ib->base;record.baseValid=true;
            if(fixStale){record.written=true;record.last=current;staleOpenFixed.fetch_add(1,std::memory_order_relaxed);
                if(window)drawEnforce.fetch_add(1,std::memory_order_relaxed);}
            else if(!ib->dirty&&record.written&&current!=record.last)record.written=false; // native owns the bone again
        }
        Mat root{};if(!e.matrix(k.root,root))return declineWith("ROOT_UNREADABLE");
        Mat parent=root;const int p=record.parents[b];
        if(b&&p>=0&&!e.matrix(k.globals+unsigned(p)*globalStride,parent))return declineWith("PARENT_UNREADABLE");
        // Barrel / up of the weapon in the cylinder's parent frame.
        const V3 mb=normalized({-modelMuzzle[8],-modelMuzzle[9],-modelMuzzle[10]}),mu=normalized(col(modelMuzzle,1));
        const auto toWorld=[&](V3 v){const V3 x=normalized(col(root,0)),y=normalized(col(root,1)),z=normalized(col(root,2));
            return normalized(V3{x[0]*v[0]+y[0]*v[1]+z[0]*v[2],x[1]*v[0]+y[1]*v[1]+z[1]*v[2],x[2]*v[0]+y[2]*v[1]+z[2]*v[2]});};
        const V3 axis=toLocal(parent,toWorld(mb)),up=toLocal(parent,toWorld(mu));
        if(!std::isfinite(axis[0]+axis[1]+axis[2]+up[0]+up[1]+up[2])||std::fabs(dot(axis,axis)-1.f)>.01f)return declineWith("AXES");
        const int hold=H::holdHand.load(std::memory_order_relaxed);const float sign=swingSignFor(grip&&grip->valid?grip->hand:hold);
        Mat written=record.base;
        // ROLL: the cylinder turned about its own axis (the bone origin = the cylinder centre, frame.radiusMm auto) while it is out
        // CHAMBERS REALISTIC also turns the CLOSED cylinder (the spin, the cock / trigger advance)
        float roll=0.f;{const auto rn=rollNs.load(std::memory_order_relaxed);if(held&&(a>0.f||realisticChambers())&&rn&&now>=rn&&now-rn<=200'000'000ull)roll=rollWord.load(std::memory_order_relaxed);}
        if(!std::isfinite(roll))roll=0.f;
        const Mat rolled=std::fabs(roll)>1e-5f?swingLocal(record.base,axis,up,roll,0.f,0.f):record.base;
        if(a<=0.f&&std::fabs(roll)>1e-5f){ // Closed + rolled (no swing)
            written=rolled;
            if(!finiteMat(written))return declineWith("NOT_FINITE");
            if(!e.store(k.locals+b*localStride,written)||!recompose(e,k,record,b)){writeFailures.fetch_add(1,std::memory_order_relaxed);return declineWith("WRITE_FAILED");}
            Mat back{};if(e.matrix(k.locals+b*localStride,back))record.last=back;else record.last=written;
            record.written=true;applied.fetch_add(1,std::memory_order_relaxed);
            if(InstBase* ib=instForLocked(weapon,skeletonPtr)){ib->dirty=true;ib->last=record.last;}
            lastAngleDeg.store(0.f,std::memory_order_relaxed);
        }else if(a<=0.f){
            if(record.written){
                if(!e.store(k.locals+b*localStride,record.base)||!recompose(e,k,record,b)){writeFailures.fetch_add(1,std::memory_order_relaxed);return declineWith("WRITE_FAILED");}
                record.written=false;restored.fetch_add(1,std::memory_order_relaxed);baseRestores.fetch_add(1,std::memory_order_relaxed);
                lastAngleDeg.store(0.f,std::memory_order_relaxed);
                if(InstBase* ib=instForLocked(weapon,skeletonPtr))ib->dirty=false;}
        }else{
            const float deg=sign*config.swingDeg*a;
            written=swingLocal(rolled,axis,up,deg*3.14159265f/180.f,config.pivotDown,config.pivotSide);
            if(!finiteMat(written))return declineWith("NOT_FINITE");
            if(!e.store(k.locals+b*localStride,written)||!recompose(e,k,record,b)){writeFailures.fetch_add(1,std::memory_order_relaxed);return declineWith("WRITE_FAILED");}
            // What the engine reads back (w lanes as stored): compare against this next time.
            Mat back{};if(e.matrix(k.locals+b*localStride,back))record.last=back;else record.last=written;
            record.written=true;applied.fetch_add(1,std::memory_order_relaxed);
            if(InstBase* ib=instForLocked(weapon,skeletonPtr)){ib->dirty=true;ib->last=record.last;}
            lastAngleDeg.store(deg,std::memory_order_relaxed);
        }
        if(held&&grip&&grip->valid){ // the ChamberFrame of this Update (rigid with the drawn gun)
            const int n=chamberCountFor(weapon);
            FrameIn in{};in.closed=multiply(parent,record.base);in.current=multiply(parent,written);
            in.full=multiply(parent,swingLocal(record.base,axis,up,sign*config.swingDeg*3.14159265f/180.f,config.pivotDown,config.pivotSide));
            in.pivot=point(parent,swingPivot(record.base,axis,up,config.pivotDown,config.pivotSide));
            const Mat mw=multiply(root,modelMuzzle);in.muzzle=col(mw,3);in.barrel=normalized(mul(col(mw,2),-1.f));in.up=normalized(col(mw,1));
            in.gripToWorld=multiply(grip->map,poseMat(grip->grip));in.n=n?n:6;in.swingSign=sign;in.amount=a;
            if(const auto& ms=specOf(weapon);ms.geom){in.modelGeom=true;in.chamberMm=ms.chamberMm;in.rearMm=ms.rearMm;}
            { // The roll sense - swingLocal turns about the barrel in the PARENT frame; a mirrored parent (det < 0) turns the world the
              // other way, a mirrored grip map turns the holding grip frame the other way (left / right hand placements)
                const auto det3=[](const Mat& m_){return dot(col(m_,0),cross(col(m_,1),col(m_,2)));};
                in.worldSense=det3(parent)<0.f?-1:1;in.gripSense=det3(in.gripToWorld)<0.f?-in.worldSense:in.worldSense;in.rollUsed=roll;
                rollWorldSense.store(in.worldSense,std::memory_order_relaxed);rollGripSense.store(in.gripSense,std::memory_order_relaxed);}
            { // The measured effect of +1 rad of roll in the holding grip frame - the very composition written above (roll, then the
              // swing), finite difference d = .05 rad -> M = G(roll+d) * G(roll)^-1 (rigid), omega from its skew part, t its translation
                Mat ginv{},g0inv{};const float d=.05f,swing=a>0.f?sign*config.swingDeg*a*3.14159265f/180.f:0.f;
                const auto at=[&](float r){const Mat rl=std::fabs(r)>1e-5f?swingLocal(record.base,axis,up,r,0.f,0.f):record.base;
                    return multiply(ginv,multiply(parent,a>0.f?swingLocal(rl,axis,up,swing,config.pivotDown,config.pivotSide):rl));};
                if(affineInverse(in.gripToWorld,ginv)&&affineInverse(at(roll),g0inv)){const Mat M=multiply(at(roll+d),g0inv);
                    const V3 om{(M[6]-M[9])*.5f/d,(M[8]-M[2])*.5f/d,(M[1]-M[4])*.5f/d},tt{M[12]/d,M[13]/d,M[14]/d};
                    if(C::finite(om)&&C::finite(tt)){in.rollOmega=om;in.rollT=tt;}}}
            ChamberFrame f{};
            if(buildFrame(in,f)){f.weapon=weapon;f.ns=now;f.referenceSpace=grip->referenceSpace;f.hand=grip->hand;chamberBox.store(f);
                lastFrameNs.store(now,std::memory_order_relaxed);bump(ctr.framePublished);
                ctr.radiusMm.store(f.radius*1000.f,std::memory_order_relaxed);ctr.radiusAuto.store(f.radiusAuto!=0,std::memory_order_relaxed);
                ctr.rearMmUsed.store(f.rear*1000.f,std::memory_order_relaxed);}
            else frameFailures.fetch_add(1,std::memory_order_relaxed);
        }
        return true;
    }catch(...){return declineWith("EXCEPTION");}
}
// A weapon whose cylinder bone was modified, seen in a later Update while no longer VR-held (holstered / dropped): restore it.
inline bool modified(std::uintptr_t weapon) noexcept {std::lock_guard lock(recordMutex);return (record.weapon==weapon&&record.written)||instDirtyLocked(weapon);}

// =====================================================================================================================
// The manual gate: the manual process (B ownership, the RT chamber gate, gestures, the hook blocking) runs only while VR places the
// revolver and the context allows it. Any thread (atomics only).
inline bool manualActive(std::uint64_t now) noexcept {
    if(!armedNow())return false;
    const int hold=H::holdHand.load(std::memory_order_acquire);if(hold<0||hold>1||!revolverKind(H::heldKind.load(std::memory_order_relaxed)))return false;
    const auto w=ownerWeapon.load(std::memory_order_acquire),ns=ownerNs.load(std::memory_order_acquire);
    if(!w||!ns||(now>ns?now-ns:ns-now)>config.ownerFreshNs||!ownerPoseAllowed.load(std::memory_order_relaxed))return false;
    if(!modelMechanics(w))return false;
    const auto word=contextWord.load(std::memory_order_acquire);if(!contextFresh(word,now,config.ownerFreshNs)||(word&0xffffu))return false;
    if(!inList(manualWeapons,w)||inList(fallbackWeapons,w))return false;
    EngineAmmo a{};if(ammoFor(w,a,now)&&a.nativeReloading)return false;
    return true;
}
// Debug lever (`.reload.forceOpen 1`): B swings the cylinder of a VR-held revolver with a cylinder
// bone even when the manual process is not armed (no model / insertion; the swing + the debug
// overlay only), to check the swing, its side and the markers.
inline bool forceVisual(std::uint64_t now) noexcept {
    if(!config.forceOpen||!config.enabled||killed(Feature::Reload))return false;
    const int hold=H::holdHand.load(std::memory_order_acquire);if(hold<0||!revolverKind(H::heldKind.load(std::memory_order_relaxed)))return false;
    const auto w=ownerWeapon.load(std::memory_order_acquire);(void)now;
    return w&&modelMechanics(w)&&cylinder.load(std::memory_order_relaxed)==int(Cylinder::Found)&&cylinderWeapon.load(std::memory_order_relaxed)==w;
}
inline bool visualGate(std::uint64_t now) noexcept {return manualActive(now)||forceVisual(now);}

// =====================================================================================================================
// Executor (main thread, ModuleReload 6; ticks before the commands of the same run()).
inline std::atomic<std::uint64_t> ticks{},tickExceptions{},executeCalls{};inline std::atomic<const char*> lastOpFail{"NONE"};
inline std::uint64_t lastOpExpiredNs{};
inline std::atomic<std::uint64_t> lastLongPressWord{};  // the brain's long press (native allowance) stamp; reconcile attributes NATIVE_LONGPRESS
// The input path's last call (Brain::frame / Brain::pad). The executor snaps an OPEN cylinder shut when the manual gate closed or
// the input path stopped (HOLSTERS OFF mid-reload, the holsters engine gone, input unusable for long): RT would reach the game unrouted.
inline std::atomic<std::uint64_t> inputSeenNs{};inline constexpr std::uint64_t inputStaleNs=1'000'000'000ull;
inline std::atomic<std::uint64_t> execSnapsGate{},execSnapsNoInput{};
struct TickState {std::int64_t lastD{};std::uint64_t lastMark{};bool pending{};std::uint64_t gcCounter{};};
inline TickState tickState{};
inline bool readAmmo(std::uintptr_t w,const H::InventoryLink& link,EngineAmmo& out,std::uint64_t now) noexcept {
    EngineAmmo a{};a.ns=now;a.weapon=w;std::uintptr_t table{},iw{},type{};
    if(!link.valid||!link.handItem||!env.get(link.handItem,0,table)||table!=env.weaponItemTable||!env.get(link.handItem,elf::itemWeapon,iw)||iw!=w)return false;
    if(!env.get(w,elf::weaponClip,a.clip)||!std::isfinite(a.clip))return false;
    try{a.perClip=env.perClip?env.perClip(reinterpret_cast<void*>(w)):0.f;}catch(...){a.perClip=0.f;}
    std::int16_t e=-1;if(env.get(w,elf::weaponType,type))env.get(type,elf::typeAmmoEnum,e);a.ammoEnum=e;
    std::uint8_t nr{};env.get(w,elf::weaponNativeReloading,nr);a.nativeReloading=nr?1u:0u;
    const std::uintptr_t pool=link.inventory+elf::invPool;std::uintptr_t arr{};std::uint16_t cnt{};
    if(e>=0&&env.get(pool,elf::poolArray,arr)&&env.get(pool,elf::poolCount,cnt)&&std::uint16_t(e)<cnt){
        const std::uintptr_t entry=arr+std::size_t(e)*elf::poolEntry;std::uint8_t inf{};
        env.get(entry,0,a.pool);env.get(entry,4,a.poolMax);env.get(entry,8,inf);a.infinite=inf?1u:0u;}
    a.valid=1;out=a;return true;
}
inline bool linkFresh(H::InventoryLink& link,std::uint64_t now) noexcept {
    return H::inventoryBox.load(link)&&link.valid&&(now>link.ns?now-link.ns:link.ns-now)<=500'000'000ull;}
// Ops acks / expiry (caller holds modelMutex).
inline void processOpsLocked(std::uint64_t now) noexcept {
    for(auto& f:inFlight){
        if(!f.on)continue;
        Record* r=recordForLocked(f.weapon);
        if(f.acked){
            if(f.kind==OpInsert){
                if(f.ok&&f.added>0)bump(ctr.opsInserted,std::uint64_t(f.added));
                int missing=f.n-std::max(0,f.added);
                if(missing>0&&r){for(int i=r->n-1;i>=0&&missing>0;--i)if((f.chambers>>i&1u)&&r->st[std::size_t(i)]==ChamberState::Live){r->st[std::size_t(i)]=ChamberState::Empty;--missing;}
                    bump(ctr.opsRollbacks);}
            }
            (f.ok?ctr.opsDone:ctr.opsFailed).fetch_add(1,std::memory_order_relaxed);if(!f.ok&&f.fail)lastOpFail.store(f.fail,std::memory_order_relaxed);
            f.on=false;continue;
        }
        if(now>f.postedNs&&now-f.postedNs>f.maxAgeNs){ // run() drops it after the ticks with this same frame.now (strict >): it never executes
            if(f.kind==OpInsert&&r)for(int i=0;i<r->n;++i)if((f.chambers>>i&1u)&&r->st[std::size_t(i)]==ChamberState::Live)r->st[std::size_t(i)]=ChamberState::Empty;
            bump(ctr.opsExpired);lastOpExpiredNs=now;f.on=false;
        }
    }
}
inline void applyFiresLocked(Record& r,int slot,std::uint64_t now) noexcept {
    const auto total=fireCounts[std::size_t(slot)].load(std::memory_order_acquire);
    while(firesApplied[std::size_t(slot)]<total){
        ++firesApplied[std::size_t(slot)];
        PendingShot* best=nullptr;for(auto& p:pendingShots)if(p.on&&p.weapon==r.weapon&&(!best||p.ns<best->ns))best=&p;
        if(best){if(r.st[std::size_t(best->chamber)]==ChamberState::Firing)r.st[std::size_t(best->chamber)]=ChamberState::Spent;
            rtPressWord.store(best->press,std::memory_order_release);best->on=false;bump(ctr.shotConfirmed);continue;}
        bool found=false; // a shot nobody decided (Dead Eye, a vetoed context, the fire task, a script): the first LIVE from next
        for(int i=0;i<r.n;++i){const int c=(r.next+i)%r.n;if(r.st[std::size_t(c)]==ChamberState::Live){r.st[std::size_t(c)]=ChamberState::Spent;r.next=realisticChambers()?c:(c+1)%r.n;found=true;break;}}
        bump(found?ctr.shotUnseen:ctr.shotWithoutLive);
    }
    for(auto& p:pendingShots){ // a pass that never became an engine shot: FIRING -> LIVE, `next` rewound when nothing decided after it
        if(!p.on||p.weapon!=r.weapon||now<p.ns||now-p.ns<=config.shotTimeoutNs)continue;
        if(r.st[std::size_t(p.chamber)]==ChamberState::Firing)r.st[std::size_t(p.chamber)]=ChamberState::Live;
        if(r.next==p.next&&decisions==p.decision)r.next=p.chamber;
        p.on=false;bump(ctr.shotRefused);
    }
}
// reconcile: d = E - (L - p), debounced over two ticks with nothing in between.
inline void reconcileLocked(Record& r,int slot,const EngineAmmo& a,std::uint64_t fireA,std::uint64_t fireB,std::uint64_t now) noexcept {
    if(fireA!=fireB){bump(ctr.reconcileSkippedRace);tickState.pending=false;return;}
    int E=int(std::lround(a.clip));if(E<0)E=0;
    if(E>r.n){maxTo(ctr.mismatchMax,std::uint64_t(E-r.n));E=r.n;}
    const int L=r.live(),firing=r.count(ChamberState::Firing),p=unackedLocked(r.weapon)-firing;
    const int d=E-(L-p);
    maxTo(ctr.mismatchMax,std::uint64_t(std::abs(d)));
    const std::uint64_t mark=std::uint64_t(decisions)*1315423911ull^fireA^(std::uint64_t(nextOpId)<<20)^ctr.opsDone.load(std::memory_order_relaxed)^(ctr.opsFailed.load()<<7);
    if(!d){tickState.pending=false;return;}
    if(!tickState.pending||tickState.lastD!=d||tickState.lastMark!=mark){tickState.pending=true;tickState.lastD=d;tickState.lastMark=mark;bump(ctr.reconcileDebounced);return;}
    tickState.pending=false;(void)slot;
    if(d<0){int m=-d;for(int i=0;i<r.n&&m>0;++i){const int c=(r.next+i)%r.n;if(r.st[std::size_t(c)]==ChamberState::Live){r.st[std::size_t(c)]=ChamberState::Spent;--m;}}
        bump(ctr.reconcileDrains);return;}
    int m=d;
    for(int pass=0;pass<2&&m>0;++pass){const ChamberState from=pass?ChamberState::Spent:ChamberState::Empty;
        for(int i=0;i<r.n&&m>0;++i){const int c=(r.next+i)%r.n;if(r.st[std::size_t(c)]==from){r.st[std::size_t(c)]=ChamberState::Live;--m;}}}
    bump(ctr.reconcileRefills);
    // cause: the audit ring, the long-press allowance, the context, a recently expired op
    const auto cause=recentAuditCause(r.weapon,now);
    if(cause==elf::CauseDeadEye||cause==elf::CauseFireTask||cause==elf::CauseReloadTask||cause==elf::CauseHolster)bump(ctr.refillCause[cause]);
    else if(const auto lp=lastLongPressWord.load(std::memory_order_relaxed);lp&&now>=lp&&now-lp<5'000'000'000ull)bump(ctr.refillLongPress);
    else if(const auto v=contextVeto(now);v&VetoDuel)bump(ctr.refillDuel);
    else if(contextVeto(now)||cause==elf::CauseCancelReload||cause==elf::CauseNativeAnim)bump(ctr.refillContext);
    else if(lastOpExpiredNs&&now>=lastOpExpiredNs&&now-lastOpExpiredNs<2'000'000'000ull)bump(ctr.refillOpExpired);
    else bump(ctr.refillCause[elf::CauseUnknown]);
}
inline void publishWordsLocked() noexcept {
    for(std::size_t i=0;i<records.size();++i){
        manualWeapons[i].store(records[i].n?records[i].weapon:0,std::memory_order_release);
        fallbackWeapons[i].store(records[i].n&&records[i].fallback?records[i].weapon:0,std::memory_order_release);}
    armedWord.store(armedNow(),std::memory_order_release);
}
inline void learnType(std::uintptr_t t) noexcept {
    if(!t||typeKnown(t))return;for(auto& x:cylinderTypes){std::uintptr_t z=0;if(x.compare_exchange_strong(z,t))return;}
    cylinderTypes[0].store(t,std::memory_order_release);}
inline void executorTick(const mainthread::Frame& fr) noexcept {
    const auto now=fr.now;ticks.fetch_add(1,std::memory_order_relaxed);
    // the input brain is not the only closer: an OPEN cylinder whose gate closed / whose input path stopped is snapped here
    if(phase(now)!=Phase::Closed){
        const auto seen=inputSeenNs.load(std::memory_order_acquire);const bool noInput=!seen||(now>seen&&now-seen>inputStaleNs);
        if(noInput||!visualGate(now)){snapClosed(now,noInput?"NO_INPUT":"GATE_CLOSED_EXEC");bump(noInput?execSnapsNoInput:execSnapsGate);}
    }
    if(!installed()){armedWord.store(false,std::memory_order_release);unmanagedWeapon.store(0,std::memory_order_release);return;}
    H::InventoryLink link{};const bool linkOk=linkFresh(link,now);
    const auto w=ownerWeapon.load(std::memory_order_acquire);const auto wNs=ownerNs.load(std::memory_order_acquire);
    const bool ownerOk=w&&wNs&&(now>wNs?now-wNs:wNs-now)<=config.ownerFreshNs;
    const int hold=H::holdHand.load(std::memory_order_acquire);
    EngineAmmo a{};const bool ammoOk=ownerOk&&linkOk&&readAmmo(w,link,a,now);
    if(ammoOk)ammoBox.store(a);
    std::lock_guard lock(modelMutex);
    processOpsLocked(now);
    // records: create for a VR-held revolver with a cylinder bone and a clip of 2..9
    const bool candidate=ammoOk&&hold>=0&&revolverKind(H::heldKind.load(std::memory_order_relaxed))&&modelMechanics(w)&&effectiveMode()!=Mode::Native&&
        cylinder.load(std::memory_order_relaxed)==int(Cylinder::Found)&&cylinderWeapon.load(std::memory_order_relaxed)==w;
    const int n=int(std::lround(a.perClip));
    Record* r=recordForLocked(w);
    if(candidate&&n>=2&&n<=maxChambers&&(!r||r->n!=n)){
        if(!r){r=&records[0];for(auto& x:records)if(!x.n||x.seenNs<r->seenNs)r=&x;if(r->n)bump(ctr.recordsDropped);}
        const int slot=slotOfLocked(r);
        *r=Record{};r->weapon=w;r->n=n;r->createdNs=now;std::uintptr_t type{};env.get(w,elf::weaponType,type);r->type=type;learnType(type);
        const int clip=std::clamp(int(std::lround(a.clip)),0,n);for(int i=0;i<n;++i)r->st[std::size_t(i)]=i<clip?ChamberState::Live:ChamberState::Empty;
        r->autoIndexedSeq=closeSeq();firesApplied[std::size_t(slot)]=fireCounts[std::size_t(slot)].load(std::memory_order_acquire);
        bump(ctr.recordsCreated);
    }
    if(r){r->seenNs=now;
        const int slot=slotOfLocked(r);
        applyFiresLocked(*r,slot,now);
        if(ammoOk&&a.weapon==r->weapon){
            const auto fireA=fireCounts[std::size_t(slot)].load(std::memory_order_acquire);
            EngineAmmo b=a;const bool again=env.get(w,elf::weaponClip,b.clip)&&std::isfinite(b.clip); // E read between the two fire reads
            const auto fireB=fireCounts[std::size_t(slot)].load(std::memory_order_acquire);
            if(again)reconcileLocked(*r,slot,b,fireA,fireB,now);
        }
    }
    for(std::size_t i=0;i<records.size();++i)if(records[i].n&&records[i].weapon!=w)applyFiresLocked(records[i],int(i),now);
    // holster refill timer of the SIDEARM item (invWeapon::OnUpdate +0xd0; -1.0f = the engine's own "off")
    if(linkOk&&!config.holsterRefill&&effectiveMode()!=Mode::Native){
        const auto item=link.items[std::size_t(H::SlotSidearm)];std::uintptr_t table{},iw{},type{};float timer{};
        if(item&&env.get(item,0,table)&&table==env.weaponItemTable&&env.get(item,elf::itemWeapon,iw)&&iw&&
           (recordForLocked(iw)||(env.get(iw,elf::weaponType,type)&&typeKnown(type)))&&env.get(item,elf::itemRefillTimer,timer)&&timer>=0.f){
            const float off=-1.f;if(env.write&&env.write(item+elf::itemRefillTimer,&off,sizeof off))bump(ctr.holsterRefillBlocked);}
    }
    // GC: a record whose weapon is neither in hand nor in a slot (every 32 ticks)
    if(linkOk&&(++tickState.gcCounter%32u)==0u){
        std::array<std::uintptr_t,H::slotCount+1> live{};for(int s=0;s<H::slotCount;++s)if(link.items[std::size_t(s)])env.get(link.items[std::size_t(s)],elf::itemWeapon,live[std::size_t(s)]);
        live[H::slotCount]=w;
        for(auto& x:records){if(!x.n)continue;bool seen=false;for(auto v:live)seen=seen||v==x.weapon;if(!seen&&now>x.seenNs+10'000'000'000ull){x=Record{};bump(ctr.recordsDropped);}}
    }
    publishWordsLocked();
    // the on-hand weapon (the holsters' inventory link, else the observer's owner) the hook would block - armed, a fresh word without
    // a veto, a record or a learned type - while the manual process does not run for it: UNMANAGED since the first such tick
    std::uintptr_t handW{};
    if(linkOk&&link.handItem){std::uintptr_t table{};if(env.get(link.handItem,0,table)&&table==env.weaponItemTable)env.get(link.handItem,elf::itemWeapon,handW);}
    else if(!linkOk&&ownerOk)handW=w;
    bool unmanaged=false;
    if(handW&&armedWord.load(std::memory_order_relaxed)){
        const auto word=contextWord.load(std::memory_order_acquire);std::uintptr_t type{};
        const bool relevant=contextFresh(word,now,config.ownerFreshNs)&&!(word&0xffffu)&&
            (inList(manualWeapons,handW)||(env.get(handW,elf::weaponType,type)&&typeKnown(type)));
        unmanaged=relevant&&!(manualActive(now)&&w==handW);
    }
    if(!unmanaged)unmanagedWeapon.store(0,std::memory_order_release);
    else if(unmanagedWeapon.load(std::memory_order_relaxed)!=handW){unmanagedSinceNs.store(now,std::memory_order_release);unmanagedWeapon.store(handW,std::memory_order_release);bump(ctr.unmanagedStarts);}
}
inline void executorTickThunk(const mainthread::Frame& f) noexcept {try{executorTick(f);}catch(...){tickExceptions.fetch_add(1,std::memory_order_relaxed);}}
inline void ackLocked(std::uint32_t id,bool ok,int added,const char* fail,float returned=0.f,float lostCap=0.f) noexcept {
    for(auto& f:inFlight)if(f.on&&f.id==id){f.acked=true;f.ok=ok;f.added=added;f.fail=fail;f.returned=returned;f.liveLostCap=lostCap;return;}
}
inline void executorExecute(const mainthread::Command& c,const mainthread::Frame& fr) noexcept {
    executeCalls.fetch_add(1,std::memory_order_relaxed);(void)fr;
    const auto w=std::uintptr_t(std::uint64_t(std::uint32_t(c.arg[0]))|(std::uint64_t(std::uint32_t(c.arg[1]))<<32));
    const std::uint32_t id=std::uint32_t(c.arg[3]);
    const char* fail=nullptr;
    H::InventoryLink link{};std::uintptr_t table{},iw{},type{};std::uint32_t flags{};std::int16_t e=-1;std::uint8_t nr{};std::uint16_t cnt{};
    if(!installed())fail="NOT_INSTALLED";
    else if(!linkFresh(link,fr.now))fail="NO_LINK";
    else if(w!=ownerWeapon.load(std::memory_order_acquire))fail="OWNER_CHANGED";
    else if(!link.handItem||!env.get(link.handItem,0,table)||table!=env.weaponItemTable||!env.get(link.handItem,elf::itemWeapon,iw)||iw!=w)fail="NOT_IN_HAND";
    else if(!env.get(link.actor,elf::actorFlags,flags)||(flags&3u)!=3u)fail="NOT_PLAYER";
    else if(!env.get(w,elf::weaponType,type)||!env.get(type,elf::typeAmmoEnum,e)||e<0||!env.get(link.inventory+elf::invPool,elf::poolCount,cnt)||std::uint16_t(e)>=cnt)fail="AMMO_ENUM";
    else if(!env.get(w,elf::weaponNativeReloading,nr)||nr)fail="NATIVE_RELOADING";
    void* const weapon=reinterpret_cast<void*>(w);void* const pool=reinterpret_cast<void*>(link.inventory+elf::invPool);
    if(c.op==OpDry){if(!fail){try{env.dryFire(weapon);}catch(...){}}return;}
    int added=0;float returned=0.f,lostCap=0.f;
    if(!fail){
        try{
            if(c.op==OpInsert){
                float before{},after{};env.get(w,elf::weaponClip,before);env.addAmmo(weapon,pool,float(std::max(0,c.arg[2])));env.get(w,elf::weaponClip,after);
                added=int(std::lround(after-before));if(added<c.arg[2])fail="SHORT";
            }else if(c.op==OpEject){
                std::uintptr_t arr{};float poolBefore{},poolAfter{},clipBefore{};std::uint8_t inf{};
                const bool poolOk=env.get(link.inventory+elf::invPool,elf::poolArray,arr);
                if(poolOk){env.get(arr+std::size_t(e)*elf::poolEntry,0,poolBefore);env.get(arr+std::size_t(e)*elf::poolEntry,8,inf);}
                env.get(w,elf::weaponClip,clipBefore);
                env.setAmmo(weapon,pool,0.f);
                if(poolOk)env.get(arr+std::size_t(e)*elf::poolEntry,0,poolAfter);
                returned=std::max(0.f,poolAfter-poolBefore);lostCap=inf?0.f:std::max(0.f,clipBefore-returned);
                if(c.value[0]<.5f&&returned>0.f){try{env.removeAmmo(pool,e,std::min(returned,float(std::max(0,c.arg[2]))));}catch(...){}ctr.liveLost.fetch_add(std::uint64_t(std::min(returned,float(std::max(0,c.arg[2])))),std::memory_order_relaxed);}
                ctr.ejectReturned.fetch_add(std::uint64_t(returned+.5f),std::memory_order_relaxed);ctr.liveLostCap.fetch_add(std::uint64_t(lostCap+.5f),std::memory_order_relaxed);
            }
        }catch(...){fail="EXCEPTION";}
    }
    std::lock_guard lock(modelMutex);ackLocked(id,!fail||(fail&&!std::strcmp(fail,"SHORT")),added,fail,returned,lostCap);
}

// =====================================================================================================================
// Reload overlay: own seqlock in the hands pass; GRIP items are mapped through the WeaponFrame of the plan's
// head sample (rigid with the engine-drawn gun), LOCAL items in plan space. Style = the marker palette (params[0]): 1 EMPTY, 2 SPENT
// (pulsing rod tip), 3 LIVE (debug), 4 held tip, 5 zone, 6 rod / rear (debug). Same layout as hands::ReloadOverlay (GameXr copies it).
struct OverlayItem {float pos[3]{};float radius{};float highlight{};std::uint32_t style{};std::uint32_t frame{};std::uint32_t pad{};}; // frame 0 GRIP, 1 LOCAL
struct Overlay {std::uint64_t referenceSpace{},ns{};std::int32_t hand{-1};std::uint32_t count{};std::array<OverlayItem,16> items{};};
static_assert(sizeof(OverlayItem)==32&&sizeof(Overlay)==24+16*32);
enum OverlayStyle : std::uint32_t {StyleEmpty=1,StyleSpent=2,StyleLive=3,StyleHeld=4,StyleZone=5,StyleDebug=6,
    // Procedural cartridges (the hands pass's cartridge pipeline).
    // pos = the rim's rear point, radius = the case radius, highlight = the case length (m), pad = the axis (rim -> bullet) packed 3 x 10-bit snorm.
    // StyleCylinder (first, not drawn) = the open cylinder as an analytic occluder for the chambered rounds that follow it (frame | FrameInCylinder):
    // pos = the rear-face centre, radius = the outer radius, highlight = the length, pad = the axis.
    StyleCartLive=8,StyleCartSpent=9,StyleCylinder=10};
inline constexpr std::uint32_t FrameInCylinder=0x100u; // OverlayItem.frame bit: a chambered round (occluded by the preceding StyleCylinder item)
inline std::uint32_t packAxis(V3 a) noexcept {
    a=normalized(a);std::uint32_t w=0;
    for(int i=0;i<3;++i){const float v=std::clamp(a[std::size_t(i)],-1.f,1.f);w|=(std::uint32_t(std::lround((v+1.f)*511.5f))&1023u)<<(10*i);}
    return w;}
using OverlaySink=void(*)(const Overlay&) noexcept;
inline std::atomic<OverlaySink> overlaySink{};
inline H::Seqlock<Overlay> overlayBox{};
inline std::atomic<std::uint64_t> overlayLastCount{};

// =====================================================================================================================
// Input brain (input thread only): B, the free hand (zones, grab, held round, insertion on the XR tape), ejects, the close detectors,
// fallback, overlay, haptics. Called from holsters InputBrain::frame (brainSink) before the grab loop; padFilter from routeB.
enum class HeldKind : std::uint8_t {None,Single,Full};
inline const char* heldName(HeldKind h) noexcept {return h==HeldKind::Single?"SINGLE":h==HeldKind::Full?"FULL":"NONE";}
// The brain's report snapshot (input thread writes at the end of every frame, the report thread copies; tiny mutex, uncontended).
struct BrainReport {HeldKind held{HeldKind::None};int heldCount{},freeHand{-1};const char* gateWhy{"NOT_ARMED"};bool frameFresh{};
    std::array<std::array<float,3>,2> aimFwd{};C::Stats stats{};};
inline std::mutex brainReportMutex;inline BrainReport brainReport{};
class Brain {
public:
    C::Detector detector{};
    // ---- B (THE edge owner): deduped by press, long press ----
    bool pad(const runtime::ControllerSample& in,int hold,std::uint64_t now) noexcept {
        inputSeenNs.store(now,std::memory_order_release);
        const bool down=(in.buttons&runtime::PadB)!=0,edge=down&&!bWas_,release=!down&&bWas_;bWas_=down;
        if(edge)bump(ctr.bEdgesPad);
        if(hold>=0&&!spin::rigid(hold)){
            if(edge){bump(ctr.bSpin);bump(ctr.refused[C::RefuseBSpin]);}pressActive_=longDone_=false;H::bForce.store(false,std::memory_order_relaxed);return true;}
        if(!visualGate(now)){
            if(phase(now)!=Phase::Closed)snapClosed(now,"GATE_CLOSED");
            pressActive_=longDone_=false;H::bForce.store(false,std::memory_order_relaxed);return false;
        }
        if(edge){
            bump(ctr.pressesB);pressActive_=true;pressStartNs_=now;longDone_=false;openRefused_=false;pressMark_=actions_;
            const auto st=stateSnapshot(now);pressFromClosed_=st.phase==Phase::Closed||st.phase==Phase::Closing;
            const int free=hold==0||hold==1?1-hold:-1;
            if(pressFromClosed_){
                if(st.lastGestureCloseNs&&now>=st.lastGestureCloseNs&&now-st.lastGestureCloseNs<200'000'000ull){bump(ctr.bAfterGesture);bump(ctr.refused[C::RefuseBAfterGesture]);openRefused_=true;}
                else if(free>=0&&H::phantomHand.load(std::memory_order_relaxed)==free){bump(opensRefusedBusy);openRefused_=true;
                    pulse(hold,.2f,25'000,now);schedule(hold,.2f,25'000,now+90'000'000ull);}
                else{open(now);bump(ctr.opens);pulse(hold,.35f,15'000,now);}
            }
        }
        // A refused open (HAND_BUSY / B_AFTER_GESTURE) only refuses the OPEN - the long press
        // still runs for that press
        if(pressActive_&&down&&!longDone_&&config.longNs&&now>=pressStartNs_+config.longNs){
            longDone_=true;
            if(pressFromClosed_){
                if(actions_==pressMark_){ // nothing done during the press: the game's reload (one RequestReload within 1 s), B unmasked until release
                    snapClosed(now,"LONG_PRESS");allowanceUntilNs.store(now+1'000'000'000ull,std::memory_order_release);allowance.store(1,std::memory_order_release);
                    lastLongPressWord.store(now,std::memory_order_relaxed);H::bForce.store(true,std::memory_order_relaxed);bump(ctr.longNative);pulse(hold,.3f,40'000,now);
                    if(openRefused_)bump(ctr.longAfterRefusal);}
            }else{autoLoad(hold,now);bump(ctr.longAutoload);}
        }
        if(release&&pressActive_){ // a short press that began OPEN closes on release (`.longMs 0` = no long press: every such release closes)
            if(!pressFromClosed_&&!longDone_&&(config.longNs==0||now<pressStartNs_+config.longNs))requestClose(CloseSource::B,hold,now);
            pressActive_=longDone_=false;
        }
        if(!down)H::bForce.store(false,std::memory_order_relaxed);
        return !H::bForce.load(std::memory_order_relaxed);
    }
    // ---- per input frame (normal path only) ----
    void frame(const H::ReloadFacts& f) noexcept;
    bool owned(unsigned h,std::uint64_t now) const noexcept {
        if(int(h)!=freeHand_||!freeEligible_)return false;
        return (openish_&&gate_)||now<tailUntil_;
    }
    void grab(unsigned h,std::uint64_t now) noexcept;
    void flushPulses(std::uint64_t now) noexcept {for(auto& p:pulses_)if(p.on&&now>=p.at){p.on=false;pulse(p.hand,p.amp,p.us,now);}}
    // report
    HeldKind held() const noexcept {return held_;}
    int heldCount() const noexcept {return heldCount_;}
    int freeHand() const noexcept {return freeHand_;}
    bool gateOpen() const noexcept {return gate_;}
    const char* gateWhy() const noexcept {return gateWhy_;}
    bool frameFresh() const noexcept {return frameFresh_;}
    std::array<std::array<float,3>,2> aimFwd() const noexcept {return {aimFwd_[0],aimFwd_[1]};}
    std::uint64_t resumeNs() const noexcept {return resumeNs_;}
    void resetForTests() noexcept {*this=Brain{};}
    void publishReport() const noexcept {
        BrainReport r{};r.held=held_;r.heldCount=heldCount_;r.freeHand=freeHand_;r.gateWhy=gateWhy_;r.frameFresh=frameFresh_;r.aimFwd={aimFwd_[0],aimFwd_[1]};
        r.stats=detector.stats;std::lock_guard lock(brainReportMutex);brainReport=r;}
private:
    // B
    bool bWas_{},pressActive_{},pressFromClosed_{},openRefused_{},longDone_{};std::uint64_t pressStartNs_{},actions_{},pressMark_{};
    // frame facts
    int lastHold_{-2},freeHand_{-1};std::uintptr_t lastWeapon_{};std::uint32_t lastGen_{},seenCloseSeq_{};bool gate_{},openish_{},freeEligible_{},frameFresh_{};
    const char* gateWhy_{"NOT_ARMED"};Mode lastMode_{Mode::Native};bool modeSeen_{};std::uint64_t tailUntil_{},lastFrameOkNs_{},openStartNs_{},resumeNs_{},lastBrainNs_{};
    Phase lastPhase_{Phase::Closed};bool seatedThisOpen_{},ejectedThisOpen_{},anyActionThisOpen_{};
    std::uint64_t lastEjectNs_{},lastSeatNs_{},lastGrabNs_{},lastDropNs_{},lastRefuseTickNs_{};
    HeldKind held_{HeldKind::None};int heldCount_{},heldHand_{-1};
    std::array<V3,2> aimFwd_{{{0,-.7071f,-.7071f},{0,-.7071f,-.7071f}}},aimUp_{{{0,.7071f,-.7071f},{0,.7071f,-.7071f}}};
    std::array<std::array<float,3>,H::slotCount> sockets_{};std::array<V3,2> handPos_{};std::array<bool,2> handValid_{};bool inZone_{},zoneTicked_{};
    std::uint64_t lastTapeNs_{},tapeSpace_{};bool prevTipValid_{},prevPinchValid_{};V3 prevTip_{},prevPinch_{};std::uint64_t prevTipTau_{},prevPinchTau_{};V3 tipLocal_{};bool tipLocalValid_{};
    V3 tipAxisLocal_{0.f,0.f,-1.f}; // the held round's axis (LOCAL, rim -> bullet = the free hand's measured aim)
    // falling rounds after an eject (LOCAL, simulated on the input thread for ~1 s; drawn as cartridges, nothing else)
    struct Brass {bool on{},live{};V3 p{},v{},axis{},spinAxis{};float spin{},radius{.0057f};std::uint64_t t0{},last{};};
    std::array<Brass,8> brass_{};unsigned brassNext_{};
    // the tape entry being processed (eject() spawns the falling rounds from it)
    const ChamberFrame* ejectCf_{};V3 ejectHoldP_{},ejectHoldV_{};Q ejectHoldQ_{0,0,0,1};bool ejectHoldValid_{};
    void spawnBrass(const Record& r,std::uint64_t now) noexcept;
    // ROLL: the free hand turns the open cylinder (radians about the barrel, around the current detent; a detent crossing moves `next`)
    bool rollTouch_{};V3 rollPrev_{};std::uint64_t rollPrevTau_{},rollMovedNs_{},rollStepNs_{};
    // rollVis_ = the continuous visual roll (bone radians, = rollWord) following the hand 1:1; rollDet_ = the detents already moved
    // into `next` (so the drawn rounds turn by the residual only); rollSign_ = the self-calibrated hand -> visual sign (0 = not yet chosen);
    // released: eases to rollTarget_ (never back against rollLastDir_)
    float rollVis_{},rollTarget_{},rollAlpha_{1.0471976f};int rollDet_{},rollSign_{},rollLastDir_{},rollDisagree_{},rollIdxDir_{1},rollN_{};
    bool rollSettle_{};std::uintptr_t rollWeapon_{};
    // CHAMBERS REALISTIC: the free spin (bone rad/s; 0 = none), the visual angle it started at, the hand's surface speed while touching
    // (bone rad/s, EMA), the cock / trigger advance animation (bone rad, decays to 0), the net axial travel of a contact (closed: a hammer stroke
    // runs along the barrel - never a spin)
    float spinOmega_{},spinStartVis_{},omegaEst_{},advanceVis_{},rollAxial_{};
    void spinEnd(std::uint64_t now) noexcept {
        if(spinOmega_==0.f)return;spinOmega_=0.f;const float revs=std::fabs(rollVis_-spinStartVis_)/6.2831853f;
        cylLastSpinRevs.store(revs,std::memory_order_relaxed);bump(cylSpinMilliRevs,std::uint64_t(revs*1000.f+.5f));bump(cylSpinsStopped);(void)now;}
    float rollResidual() const noexcept {return rollVis_-float(rollDet_)*rollAlpha_;}
    void rollReset() noexcept {rollVis_=rollTarget_=0.f;rollDet_=0;rollSettle_=false;rollTouch_=false;rollActive_=false;rollDisagree_=0;
        spinOmega_=omegaEst_=advanceVis_=rollAxial_=0.f;}
    void rollCross(int ff,int hold,std::uint64_t now) noexcept;
    // the sense of +1 rad of roll about cf.axis in the holding grip frame, MEASURED from the bone (cf.rollOmega); the analytic one as a fallback
    static int boneSense(const ChamberFrame& cf) noexcept {const float w=dot(cf.rollOmega,cf.axis),l=length(cf.rollOmega);
        return l>.2f&&std::fabs(w)>=.5f*l?(w<0.f?-1:1):(cf.gripSense<0?-1:1);}
    // the probe locked at the contact start (0 pinch, 1 grip origin), ROLLING (tangential travel dominant) + its end (the gesture block)
    int rollProbe_{};bool rollActive_{};float rollTang_{},rollFrame_{};std::uint64_t rollEndNs_{};
    void rollRelease(std::uint64_t now) noexcept {
        if(rollTouch_&&rollActive_){rollEndNs_=now;
            // CHAMBERS REALISTIC: a swipe that leaves the cylinder still moving keeps it turning (momentum = the hand's surface speed)
            if(realisticChambers()&&rollMovedNs_&&now<=rollMovedNs_+80'000'000ull&&std::fabs(omegaEst_)>=config.spinStart){
                spinOmega_=std::clamp(omegaEst_*config.spinGain,-config.spinMax,config.spinMax);spinStartVis_=rollVis_;rollSettle_=false;bump(cylSpins);}}
        rollTouch_=false;rollActive_=false;omegaEst_=0.f;}
    bool rollBlocking(std::uint64_t now) const noexcept {return rollActive_||(rollEndNs_&&now<rollEndNs_+300'000'000ull);}
    V3 slotAt(const ChamberFrame& cf,int slot,bool open) const noexcept { // slot position (holding grip frame) turned by the live roll
        // slotCur already carries the roll apply() used (cf.rollUsed): only the difference to the live
        // roll is added (all of it would count the roll twice); the sense is the visual's in the
        // holding grip frame (cf.gripSense)
        const std::size_t i=std::size_t(std::clamp(slot,0,maxChambers-1));const V3 p=open?cf.slotOpen[i]:cf.slotCur[i];
        // chamber c sits in slot (c - next) and `next` already carries rollDet_ whole chambers: the rounds turn by the residual only,
        // in the MEASURED sense of the bone (boneSense) - the drawn rounds stay in the drawn chambers whatever the bone axis convention
        const float res=rollResidual()+advanceVis_;const float turn=float(boneSense(cf))*(open?res:res-cf.rollUsed); // + the advance animation
        if(std::fabs(turn)<1e-5f)return p;const V3 c=open?cf.rearOpen:cf.rearCur;return add(c,axisRotate(sub(p,c),cf.axis,turn));}
    void rollStep(const ChamberFrame& cf,const Record& snap,V3 pinch,V3 gripP,std::uint64_t tau,int hold,unsigned ff,float outerR,std::uint64_t now,bool closed=false) noexcept;
    int magnetChamber_{-1},occupiedChamber_{-1};std::uint64_t shakeArmTau_{};float shakePeak_{};std::uint64_t tiltSince_{};
    V3 prevHoldP_{};std::uint64_t prevHoldTau_{};bool prevHoldValid_{};float followShown_{1.f};std::uint64_t followStepNs_{};
    bool trigDown_{};std::uint64_t overlayCount_{};bool overlayWasVisible_{};
    struct Pending {bool on{};int hand{-1};float amp{};std::uint32_t us{};std::uint64_t at{};};
    std::array<Pending,6> pulses_{};
    void schedule(int hand,float amp,std::uint32_t us,std::uint64_t at) noexcept {for(auto& p:pulses_)if(!p.on){p={true,hand,amp,us,at};return;}}
    void action(std::uint64_t now) noexcept {++actions_;anyActionThisOpen_=true;(void)now;}
    void autoLoad(int hold,std::uint64_t now) noexcept;
    void eject(int hold,const char* how,std::uint64_t now) noexcept;
    bool insert(int chamberMaskOrIndex,bool full,std::uint64_t now) noexcept;
    bool inAmmoZone(V3 hand,V3 gun) noexcept;
    void tapeStep(const runtime::ControllerSample& in,int hold,const ChamberFrame& cf,const State& st,std::uint64_t now) noexcept;
    void publishOverlay(const runtime::ControllerSample& in,int hold,const ChamberFrame* cf,const State& st,std::uint64_t now) noexcept;
};
inline Brain brain{};

inline bool Brain::inAmmoZone(V3 hand,V3 gun) noexcept {
    const float hys=inZone_?.05f:0.f;
    if(config.zoneAny)return length(sub(hand,gun))>.25f-hys*.5f;
    const auto sock=[&](int s){const auto& p=sockets_[std::size_t(s)];return V3{p[0],p[1],p[2]};};
    if(length(sub(hand,sock(H::SlotThrown)))<=config.zoneChest+hys)return true;
    const auto capsule=[&](V3 a,V3 b,float r){const V3 ab=sub(b,a);const float l2=dot(ab,ab);float t=l2>1e-9f?dot(sub(hand,a),ab)/l2:0.f;t=std::clamp(t,0.f,1.f);
        return length(sub(hand,add(a,mul(ab,t))))<=r;};
    return capsule(sock(H::SlotMelee),sock(H::SlotLasso),config.zoneBelt+hys)||capsule(sock(H::SlotMelee),sock(H::SlotSidearm),config.zoneBelt+hys);
}
inline void Brain::autoLoad(int hold,std::uint64_t now) noexcept {
    const auto w=ownerWeapon.load(std::memory_order_acquire);EngineAmmo a{};const bool ammo=ammoFor(w,a,now);
    {std::lock_guard lock(modelMutex);Record* r=recordForLocked(w);
        if(r){for(int i=0;i<r->n;++i)if(r->st[std::size_t(i)]==ChamberState::Spent)r->st[std::size_t(i)]=ChamberState::Empty;
            int avail=ammo&&a.infinite?r->n:ammo?int(std::floor(std::max(0.f,a.pool))):0;std::uint16_t mask=0;int m=0;
            for(int i=0;i<r->n&&avail>0;++i){const int c=(r->next+i)%r->n;if(r->st[std::size_t(c)]==ChamberState::Empty){r->st[std::size_t(c)]=ChamberState::Live;mask|=std::uint16_t(1u<<c);++m;--avail;}}
            if(m&&!postOpLocked(OpInsert,w,m,0,mask,now)){for(int c=0;c<r->n;++c)if(mask>>c&1u)r->st[std::size_t(c)]=ChamberState::Empty;}
            if(m)bump(ctr.roundsSeated,std::uint64_t(m));}}
    action(now);held_=HeldKind::None;
    requestClose(CloseSource::AutoLoad,hold,now);
    for(int h=0;h<2;++h)pulse(h,.6f,35'000,now);
}
inline void Brain::eject(int hold,const char* how,std::uint64_t now) noexcept {
    const auto w=ownerWeapon.load(std::memory_order_acquire);int live=0,spent=0;
    {std::lock_guard lock(modelMutex);Record* r=recordForLocked(w);if(!r)return;
        live=r->live();spent=r->count(ChamberState::Spent);
        if(!live&&!spent){bump(ctr.ejectNothing);return;}
        const auto saved=r->st;const Record before=*r;
        for(int i=0;i<r->n;++i)if(r->st[std::size_t(i)]==ChamberState::Live||r->st[std::size_t(i)]==ChamberState::Spent)r->st[std::size_t(i)]=ChamberState::Empty;
        if(live&&!postOpLocked(OpEject,w,0,live,0,now)){r->st=saved;return;}
        spawnBrass(before,now);}
    if(!std::strcmp(how,"SHAKE"))bump(ctr.ejectShake);else if(!std::strcmp(how,"ROD"))bump(ctr.ejectRod);else bump(ctr.ejectTilt);
    if(seatedThisOpen_)bump(ctr.ejectsAfterSeat);
    bump(ctr.ejectedSpent,std::uint64_t(spent));bump(ctr.ejectedLive,std::uint64_t(live));
    ejectedThisOpen_=true;lastEjectNs_=now;action(now);detector.blockPush(C::BlockEject);
    seatedThisOpen_=false; // Re-loading starts over (the next tilt / shake empties the cylinder again)
    pulse(hold,.5f,40'000,now);
}
// Every LIVE / SPENT round of `r` falls out of its chamber (LOCAL, the holding pose of the tape entry being processed).
inline void Brain::spawnBrass(const Record& r,std::uint64_t now) noexcept {
    if(!config.brass||!ejectCf_||!ejectHoldValid_||!r.n)return;
    const ChamberFrame& cf=*ejectCf_;
    const V3 axisW=normalized(C::rot(ejectHoldQ_,cf.axis));
    const float spacing=2.f*cf.radius*std::sin(3.14159265f/float(std::max(r.n,2)));const float caseR=std::clamp(.42f*spacing,.0052f,.0062f);
    for(int c=0;c<r.n&&c<maxChambers;++c){const auto st=r.st[std::size_t(c)];if(st!=ChamberState::Live&&st!=ChamberState::Spent)continue;
        const V3 mouth=slotAt(cf,(c-r.next+r.n)%r.n,false);
        Brass& b=brass_[brassNext_%brass_.size()];++brassNext_;
        const float k=float(c)*2.399963f; // golden-angle scatter, deterministic
        V3 side=normalized(cross(axisW,V3{0.f,1.f,0.f}));if(length(side)<.5f)side=V3{1.f,0.f,0.f};const V3 side2=cross(axisW,side);
        const V3 lateral=add(mul(side,.10f*std::cos(k)),mul(side2,.10f*std::sin(k)));
        b.on=true;b.live=st==ChamberState::Live;b.radius=caseR;b.t0=b.last=now;
        b.p=add(ejectHoldP_,C::rot(ejectHoldQ_,sub(mouth,mul(cf.axis,.0012f))));
        b.v=add(add(mul(ejectHoldV_,.6f),mul(axisW,-.45f-.05f*float(c%3))),lateral); // out of the rear of the chamber
        b.axis=axisW;b.spinAxis=normalized(add(mul(side,std::cos(k*1.7f)),mul(side2,std::sin(k*1.7f))));b.spin=7.f+2.f*float(c%4);}
}
// SINGLE: index = the chamber; FULL: the speedloader at the rear centre fills every EMPTY chamber (up to heldCount_).
inline bool Brain::insert(int index,bool full,std::uint64_t now) noexcept {
    const auto w=ownerWeapon.load(std::memory_order_acquire);int seated=0;
    {std::lock_guard lock(modelMutex);Record* r=recordForLocked(w);if(!r)return false;
        std::uint16_t mask=0;
        if(full){int left=heldCount_;for(int i=0;i<r->n&&left>0;++i){const int c=(r->next+i)%r->n;if(r->st[std::size_t(c)]==ChamberState::Empty){mask|=std::uint16_t(1u<<c);--left;}}}
        else if(index>=0&&index<r->n&&r->st[std::size_t(index)]==ChamberState::Empty)mask=std::uint16_t(1u<<index);
        if(!mask)return false;
        for(int c=0;c<r->n;++c)if(mask>>c&1u){r->st[std::size_t(c)]=ChamberState::Live;++seated;}
        if(!postOpLocked(OpInsert,w,seated,0,mask,now)){for(int c=0;c<r->n;++c)if(mask>>c&1u)r->st[std::size_t(c)]=ChamberState::Empty;return false;}}
    bump(ctr.inserts);bump(ctr.roundsSeated,std::uint64_t(seated));seatedThisOpen_=true;lastSeatNs_=now;action(now);detector.blockPush(C::BlockSeat);
    const int hold=1-heldHand_;
    if(full){pulse(hold,.6f,35'000,now);pulse(heldHand_,.6f,35'000,now);}else{pulse(hold,.45f,18'000,now);pulse(heldHand_,.35f,12'000,now);}
    held_=HeldKind::None;heldCount_=0;prevTipValid_=false;magnetChamber_=occupiedChamber_=-1;return true;
}
inline void Brain::grab(unsigned h,std::uint64_t now) noexcept {
    if(int(h)!=freeHand_||!freeEligible_)return;
    if(!gate_||!openish_){return;} // the 300 ms tail after a close: a capture without an action
    if(held_!=HeldKind::None){bump(ctr.grabBusy);return;}
    if(!handValid_[h]||!inAmmoZone(handPos_[h],handPos_[1-h])){bump(ctr.grabOutside);
        const auto& c=sockets_[std::size_t(H::SlotThrown)];ctr.grabOutsideLastM.store(length(sub(handPos_[h],V3{c[0],c[1],c[2]})),std::memory_order_relaxed);return;}
    const auto w=ownerWeapon.load(std::memory_order_acquire);EngineAmmo a{};const bool ammo=ammoFor(w,a,now);
    const bool inf=ammo&&a.infinite;const int pool=ammo?int(std::floor(std::max(0.f,a.pool))):0;
    const Mode mode=effectiveMode();int empty=0,n=0;
    {std::lock_guard lock(modelMutex);if(const Record* r=recordForLocked(w)){empty=r->count(ChamberState::Empty);n=r->n;}}
    if(mode==Mode::Full){
        if(!empty){bump(ctr.grabNoEmpty);for(int i=0;i<3;++i)schedule(int(h),.2f,12'000,now+std::uint64_t(i)*70'000'000ull);return;}
        const int count=std::min(empty,inf?n:pool);
        if(count<=0){bump(ctr.grabNoAmmo);pulse(int(h),.15f,60'000,now);schedule(int(h),.15f,60'000,now+110'000'000ull);return;}
        held_=HeldKind::Full;heldCount_=count;bump(ctr.grabsFull);
    }else{
        if(!inf&&pool<1){bump(ctr.grabNoAmmo);pulse(int(h),.15f,60'000,now);schedule(int(h),.15f,60'000,now+110'000'000ull);return;}
        held_=HeldKind::Single;heldCount_=1;bump(ctr.grabsSingle);
    }
    heldHand_=int(h);prevTipValid_=false;magnetChamber_=occupiedChamber_=-1;lastGrabNs_=now;action(now);detector.blockPush(C::BlockGrab);
    pulse(int(h),.3f,12'000,now);
}
// ROLL: one tape entry (the free hand's pinch / grip origin in the holding grip frame). Contact =
// within .rollReach (5 cm) of the open cylinder's surface over its length (+2.5 cm); the probe nearer
// the surface is LOCKED for the contact (a probe switch would be a step of several cm in any
// direction); the tangential travel about the cylinder's own axis (cf.axis, this frame) turns it 1:1
// at the SURFACE (angle = travel / outer radius; the push toward the frame does not count), in the
// visual's sense in the holding grip frame (cf.gripSense: mirrored placements), with no jitter floor
// (slow rolls count). A detent crossing moves `next` by the WORLD sense (cf.worldSense) so the
// chambers stay continuous. ROLLING (>= 3 mm tangential, dominant over the toward-the-frame travel)
// blocks the close gestures until 300 ms after the contact ends.
// `closed` (CHAMBERS REALISTIC, the cylinder CLOSED / CLOSING): the same contact about the CURRENT
// rear centre (cf.rearCur); it turns the cylinder only once ROLLING (>= 1 cm tangential, dominant over
// the travel along the barrel: a hammer stroke never spins it); the surface speed (omegaEst_) becomes
// the free spin when the hand leaves still moving (rollRelease).
inline void Brain::rollStep(const ChamberFrame& cf,const Record& snap,V3 pinch,V3 gripP,std::uint64_t tau,int hold,unsigned ff,float outerR,std::uint64_t now,bool closed) noexcept {
    const V3 ax=cf.axis;const float len=std::max(.030f,2.f*std::fabs(cf.rear));const V3 centre=closed?cf.rearCur:cf.rearOpen;
    const auto geo=[&](V3 x,float& rho,float& axial){const V3 rel=sub(x,centre);axial=dot(rel,ax);rho=length(perp(rel,ax));};
    const auto inside=[&](float rho,float axial){return rho>=.004f&&rho<=outerR+config.rollReach&&axial>=-.025f&&axial<=len+.025f;};
    float rP{},aP{},rG{},aG{};geo(pinch,rP,aP);geo(gripP,rG,aG);
    if(!rollTouch_){
        const bool inP=inside(rP,aP),inG=inside(rG,aG);if(!inP&&!inG)return;
        rollProbe_=inP&&(!inG||std::fabs(rP-outerR)<=std::fabs(rG-outerR))?0:1;
        rollTouch_=true;rollActive_=false;rollTang_=rollFrame_=rollAxial_=omegaEst_=0.f;rollPrev_=rollProbe_?gripP:pinch;rollPrevTau_=tau;bump(rollContacts);return;}
    const V3 x=rollProbe_?gripP:pinch;const float rho=rollProbe_?rG:rP,axial=rollProbe_?aG:aP;
    if(!inside(rho,axial)){rollRelease(now);return;}
    if(tau<=rollPrevTau_||tau-rollPrevTau_>60'000'000ull){rollPrev_=x;rollPrevTau_=tau;return;}
    const float dtS=std::max(.002f,float(double(tau-rollPrevTau_)*1e-9)); // The surface speed
    const V3 step=sub(x,rollPrev_);rollPrev_=x;rollPrevTau_=tau;
    if(length(step)>.05f)return; // a tracking jump
    // the hand INSIDE the cylinder (< half its outer radius from the axis) has no defined "surface under the hand" (the side flips
    // as the hand crosses the axis = the roll reversed mid-stroke): no roll there
    const V3 d=perp(sub(x,centre),ax);const float dl=length(d);if(dl<std::max(.004f,.5f*outerR))return;
    const V3 dh=mul(d,1.f/dl),tang=cross(ax,dh);const float s=dot(step,tang);
    rollTang_+=s;rollFrame_+=dot(step,cf.toFrame);rollAxial_+=dot(step,ax);
    if(!rollActive_&&!closed&&std::fabs(rollTang_)>=.003f&&std::fabs(rollTang_)>=.9f*std::fabs(rollFrame_)){rollActive_=true;bump(rollActivations);}
    if(!rollActive_&&closed&&std::fabs(rollTang_)>=.010f&&std::fabs(rollTang_)>=1.2f*std::fabs(rollAxial_)){rollActive_=true;bump(rollActivations);}
    if(closed&&!rollActive_)return; // A brush / a hammer stroke never turns the closed cylinder
    if(rollActive_&&spinOmega_!=0.f){spinEnd(now);rollSettle_=false;} // The hand catches a spinning cylinder
    if(std::fabs(s)<.0001f)return;
    // The roll follows the hand's direction (a fixed sign is right for some poses, wrong for others): the
    // SELF-CALIBRATING sign. The point of the cylinder surface under the hand moves by pred = cross(omega, P) + t per +1 rad of roll (cf: measured
    // from the bone matrices apply() writes); the visual increment rollSign_ * s / R moves it along the hand's tangent by rollSign_ * s * along / R
    // -> it follows the hand iff rollSign_ * along > 0. 3 disagreeing moving samples in a row flip the sign (signFlips); while they disagree the
    // cylinder holds still (never driven against the hand). Hand down = surface down whatever the bone axis / parent / grip-map convention.
    if(!rollSign_)rollSign_=boneSense(cf); // the measured sense; flips only if the live check disagrees
    {const float ac=std::clamp(dot(sub(x,cf.rearOpen),ax),0.f,std::max(.030f,2.f*std::fabs(cf.rear)));
        const V3 P=add(add(centre,mul(ax,ac)),mul(dh,outerR));const V3 pred=add(cross(cf.rollOmega,P),cf.rollT);
        const float along=dot(pred,tang),pl=length(pred);
        if(pl>.002f&&std::fabs(along)>=.3f*pl&&std::fabs(s)>=.0002f){
            if(float(rollSign_)*along<0.f){if(++rollDisagree_>=3){rollSign_=-rollSign_;rollDisagree_=0;bump(rollSignFlips);}}
            else rollDisagree_=0;}
        rollLastSign.store(rollSign_,std::memory_order_relaxed);rollBoneSense.store(boneSense(cf),std::memory_order_relaxed);}
    if(rollDisagree_)return;
    { // The surface speed in bone rad/s (unclamped), EMA over ~3 samples
        const float wInst=float(rollSign_)*config.rollGain*s/std::max(outerR,.008f)/dtS;if(std::isfinite(wInst))omegaEst_+=.35f*(wInst-omegaEst_);}
    rollMovedNs_=now;bump(rollSteps);
    // 1:1 at the surface: the visual angle is the CONTINUOUS accumulated angle (no snapping / easing to a detent while touching); detents only
    // click and move the logical chamber index (rollCross), in the bone's measured sense x the measured slot order (the drawn rounds stay put)
    const float dV=float(rollSign_)*std::clamp(config.rollGain*s/std::max(outerR,.008f),-.6f,.6f);
    if(rollDet_==0&&std::fabs(rollResidual())<1e-6f){rollAlpha_=6.2831853f/float(snap.n);rollN_=snap.n;}
    rollVis_+=dV;rollLastDir_=dV>0.f?1:-1;rollSettle_=false;
    rollWeapon_=std::uintptr_t(cf.weapon);rollIdxDir_=boneSense(cf)*(cf.slotDir<0?-1:1);
    rollCross(int(ff),hold,now);
}
// Detent crossings of the continuous visual (touching or easing after the release): click + the logical chamber index. +1 detent of
// roll (bone) turns the cylinder by boneSense * alpha about cf.axis = slot k -> k + boneSense * slotDir; chamber c in slot (c - next) ->
// next -= dir * rollIdxDir_ (the visual and the chamber order turn the same way by construction).
inline void Brain::rollCross(int ff,int hold,std::uint64_t now) noexcept {
    for(int guard=0;guard<64&&std::fabs(rollResidual())>.5f*rollAlpha_;++guard){
        const int dir=rollResidual()>0.f?1:-1;rollDet_+=dir;
        {std::lock_guard lock(modelMutex);if(Record* r=recordForLocked(rollWeapon_);r&&r->n==rollN_)r->next=((r->next-dir*rollIdxDir_)%r->n+r->n)%r->n;}
        bump(rollDetents);
        if(!rollTouch_&&realisticChambers())pulse(hold,.16f,5'000,now); // The free spin's / settle's ratchet (the free hand is off the cylinder)
        else{pulse(ff,.40f,12'000,now);pulse(hold,.18f,8'000,now);}
        action(now);
    }
}
// XR tape entries newer than the last processed (and than the resume): ejects, insertion (relative sweep), close detectors.
inline void Brain::tapeStep(const runtime::ControllerSample& in,int hold,const ChamberFrame& cf,const State& st,std::uint64_t now) noexcept {
    namespace T=throwing;
    const unsigned hh=unsigned(hold),ff=1u-hh;
    const auto head=T::tapeHead.load(std::memory_order_acquire);
    std::array<T::TapeEntry,T::tapeSize> got{};unsigned n=0;std::uint64_t newest=0;
    for(std::uint64_t i=head>T::tapeSize?head-T::tapeSize:0;i<head;++i){T::TapeEntry t{};
        if(!T::tapeRing[i%T::tapeSize].load(t)||t.space!=in.referenceSpace)continue;newest=std::max(newest,t.ns);
        if(t.ns<=lastTapeNs_||t.ns<=resumeNs_||t.ns>now+5'000'000ull)continue;got[n++]=t;}
    for(unsigned i=1;i<n;++i){T::TapeEntry x=got[i];unsigned j=i;while(j>0&&got[j-1].ns>x.ns){got[j]=got[j-1];--j;}got[j]=x;}
    const bool stale=!newest||now>newest+60'000'000ull;
    if(stale){++detector.stats.staleFrames;}
    const C::Geometry g=geometryOf(cf);
    Record snap{};bool haveRec=false;
    {std::lock_guard lock(modelMutex);if(const Record* r=recordForLocked(cf.weapon)){snap=*r;haveRec=true;}}
    const unsigned row=closeRow();
    ejectCf_=&cf;ejectHoldValid_=false; // The falling rounds of an eject in this call
    for(unsigned e=0;e<n;++e){
        const auto& t=got[e];lastTapeNs_=std::max(lastTapeNs_,t.ns);
        if(stale)continue;
        const bool hv=t.valid>>hh&1u,fv=t.valid>>ff&1u;
        const std::uint64_t tau=t.poseNs?t.poseNs:t.ns;
        // ---- the holding hand's linear velocity: the runtime's (RUNTIME) or the pose difference over poseNs (POSE) ----
        V3 v{};bool vOk=false;
        if(hv){
            if((t.valid&T::tapeLinear(hh))&&!config.close.velocityPose){v=t.v[hh];vOk=true;}
            else if(prevHoldValid_&&tau>=prevHoldTau_+4'000'000ull&&tau<=prevHoldTau_+60'000'000ull){
                v=mul(sub(t.p[hh],prevHoldP_),1.f/float(double(tau-prevHoldTau_)*1e-9));vOk=true;}
            prevHoldP_=t.p[hh];prevHoldTau_=tau;prevHoldValid_=true;
            ejectHoldP_=t.p[hh];ejectHoldQ_=t.q[hh];ejectHoldV_=vOk&&length(v)<20.f?v:V3{};ejectHoldValid_=true;
        }else{prevHoldValid_=false;ejectHoldValid_=false;}
        // ---- muzzle elevation / SHAKE / TILT (holding hand) ----
        if(hv&&vOk&&st.phase==Phase::Open){
            const V3 barrel=C::rot(t.q[hh],cf.axis);const float up=barrel[1];
            const bool muzzleUp=up>=std::sin(config.close.ejectUpDeg*3.14159265f/180.f);
            const float speed=length(v);
            if(speed>20.f){shakeArmTau_=0;}
            else if(muzzleUp&&v[1]<=-config.shake){if(!shakeArmTau_){shakeArmTau_=tau;shakePeak_=0;}shakePeak_=std::max(shakePeak_,-v[1]);}
            else if(shakeArmTau_){
                if(tau>shakeArmTau_+200'000'000ull)shakeArmTau_=0;
                else if(speed<=.3f||v[1]>0.f){shakeArmTau_=0;
                    if(haveRec&&snap.count(ChamberState::Empty)==snap.n)bump(ctr.ejectNothing); // Also after seating (re-loading starts over)
                    else{eject(hold,"SHAKE",now);std::lock_guard lock(modelMutex);if(const Record* r=recordForLocked(cf.weapon))snap=*r;}}
            }else if(muzzleUp&&v[1]<0.f&&-v[1]>=.4f*config.shake){const unsigned i=ctr.shakeSubN.fetch_add(1,std::memory_order_relaxed)%8u;ctr.shakeSub[i].store(-v[1],std::memory_order_relaxed);}
            if(config.tiltNs){ // TILT: muzzle >= .tiltDeg (35) up for .tiltMs (120)
                // -> ALL rounds fall out (spent and live; live back to the pool), also after seating; re-armed while the muzzle stays up
                if(up>=std::sin(config.tiltDeg*3.14159265f/180.f)){if(!tiltSince_)tiltSince_=tau;
                    else if(tau>=tiltSince_+config.tiltNs){tiltSince_=0;
                        if(haveRec&&snap.count(ChamberState::Empty)<snap.n){eject(hold,"TILT",now);std::lock_guard lock(modelMutex);if(const Record* r=recordForLocked(cf.weapon))snap=*r;}}}
                else if(tiltSince_){tiltSince_=0;bump(ctr.tiltAborted);}
            }
        }
        // ---- the free hand: pinch / tip in the holding grip frame ----
        if(hv&&fv){
            const V3 fwd=normalized(C::rot(t.q[ff],aimFwd_[ff]));V3 up=C::rot(t.q[ff],aimUp_[ff]);up=normalized(perp(up,fwd));
            const V3 side=cross(fwd,up);
            const V3 pinchW=add(add(add(t.p[ff],mul(fwd,config.pinch[0])),mul(up,config.pinch[1])),mul(side,config.pinch[2]));
            const V3 pinch=C::toFrame(t.p[hh],t.q[hh],pinchW);
            // ROD eject: the empty free hand's pinch at the rod tip, moving back along the barrel (relative to the gun)
            if(config.rod&&st.phase==Phase::Open&&held_==HeldKind::None&&freeEligible_&&prevPinchValid_&&tau>prevPinchTau_&&tau-prevPinchTau_<=60'000'000ull){
                const float dt=float(double(tau-prevPinchTau_)*1e-9);const V3 vel=mul(sub(pinch,prevPinch_),1.f/dt);
                if(length(sub(pinch,cf.rodOpen))<=.030f&&dot(vel,mul(cf.axis,-1.f))>=.25f&&length(vel)<20.f){
                    if(haveRec&&snap.count(ChamberState::Empty)==snap.n)bump(ctr.ejectNothing);
                    else{eject(hold,"ROD",now);std::lock_guard lock(modelMutex);if(const Record* r=recordForLocked(cf.weapon))snap=*r;}
                    prevPinchValid_=false;}
            }
            prevPinch_=pinch;prevPinchTau_=tau;prevPinchValid_=true;
            // ROLL: the empty free hand touching the open cylinder and moving round it turns it (detent clicks per chamber)
            // CHAMBERS REALISTIC: also the CLOSED / CLOSING cylinder (an open free palm swiping across its side spins it)
            const bool closedRoll=realisticChambers()&&(st.phase==Phase::Closed||st.phase==Phase::Closing)&&in.grips[ff]<=.5f;
            if(config.roll&&held_==HeldKind::None&&freeEligible_&&haveRec&&snap.n>=2&&
               ((st.phase==Phase::Open&&(rollActive_||(detector.pushState()!=C::PushArmed&&detector.pushState()!=C::PushLatch)))||closedRoll)){ // ROLLING wins over PUSH
                const float spacing=2.f*cf.radius*std::sin(3.14159265f/float(snap.n));const float outerR=cf.radius+std::clamp(.42f*spacing,.0052f,.0062f)+.0025f;
                const V3 gripP=C::toFrame(t.p[hh],t.q[hh],t.p[ff]);
                // the probe nearer the cylinder: the pinch (fingers) or the grip origin (palm)
                rollStep(cf,snap,pinch,gripP,tau,hold,ff,outerR,now,st.phase!=Phase::Open); // The probe is locked per contact inside
                {std::lock_guard lock(modelMutex);if(const Record* r=recordForLocked(cf.weapon))snap=*r;}
            }else rollRelease(now);
            if(held_!=HeldKind::None&&int(ff)==heldHand_&&st.phase==Phase::Open&&haveRec){
                const V3 tip=add(pinch,mul(C::rot(C::conj(t.q[hh]),fwd),held_==HeldKind::Full?.020f:.016f));
                const V3 heldAxis=C::rot(C::conj(t.q[hh]),fwd);
                tipLocal_=add(pinchW,mul(fwd,held_==HeldKind::Full?.020f:.016f));tipLocalValid_=true;tipAxisLocal_=fwd;
                if(prevTipValid_&&tau>prevTipTau_&&tau-prevTipTau_<=100'000'000ull){
                    const V3 a=prevTip_,b=tip,ab=sub(b,a);const float l2=dot(ab,ab);
                    const auto closest=[&](V3 m){float u=l2>1e-12f?dot(sub(m,a),ab)/l2:1.f;u=std::clamp(u,0.f,1.f);return add(a,mul(ab,u));};
                    const float ang=std::acos(std::clamp(dot(normalized(heldAxis),cf.axis),-1.f,1.f))*57.29578f;
                    if(held_==HeldKind::Full){
                        const V3 cp=closest(cf.rearOpen);const float d=length(sub(cp,cf.rearOpen));
                        if(d<=config.fullMm*.001f&&ang<=std::max(config.fullDeg,config.insertDeg)){insert(-1,true,now);std::lock_guard lock(modelMutex);if(const Record* r=recordForLocked(cf.weapon))snap=*r;}
                        else if(d<=2.f*config.fullMm*.001f&&magnetChamber_!=99){magnetChamber_=99;pulse(int(ff),.08f,6'000,now);bump(ctr.magnetTicks);}
                    }else{
                        int best=-1,nearest=-1;float bestD=1e9f,nearestD=1e9f;
                        for(int c=0;c<snap.n;++c){
                            const V3 m=slotAt(cf,(c-snap.next+snap.n)%snap.n,true);const V3 cp=closest(m);const float d=length(sub(cp,m));
                            if(d<nearestD){nearestD=d;nearest=c;}
                            if(snap.st[std::size_t(c)]!=ChamberState::Empty)continue;
                            if(d<=config.insertMm*.001f&&dot(sub(cp,m),cf.axis)<=config.frontMm*.001f&&ang<=config.insertDeg&&d<bestD){bestD=d;best=c;}
                        }
                        if(best>=0){insert(best,false,now);std::lock_guard lock(modelMutex);if(const Record* r=recordForLocked(cf.weapon))snap=*r;}
                        else if(nearest>=0){
                            const auto s=snap.st[std::size_t(nearest)];
                            if(nearestD<=config.insertMm*.001f&&s!=ChamberState::Empty){
                                if(occupiedChamber_!=nearest){occupiedChamber_=nearest;pulse(int(ff),.1f,20'000,now);bump(s==ChamberState::Spent?ctr.insertSpent:ctr.insertOccupied);}}
                            else if(nearestD>config.insertMm*.001f)occupiedChamber_=-1;
                            if(nearestD<=2.f*config.insertMm*.001f){
                                const std::size_t bin=std::min<std::size_t>(std::size_t(nearestD*1000.f/7.f),7);ctr.nearMissMm[bin].fetch_add(1,std::memory_order_relaxed);
                                ctr.nearMissDeg[std::min<std::size_t>(std::size_t(ang/15.f),7)].fetch_add(1,std::memory_order_relaxed);
                                if(s==ChamberState::Empty&&magnetChamber_!=nearest){magnetChamber_=nearest;pulse(int(ff),.08f,6'000,now);bump(ctr.magnetTicks);}}
                            else if(magnetChamber_==nearest)magnetChamber_=-1;
                        }
                    }
                }
                prevTip_=tip;prevTipTau_=tau;prevTipValid_=held_!=HeldKind::None;
            }
        }
        // ---- the close detectors (OPEN only; the CYLINDER CLOSE row) ----
        if(st.phase==Phase::Open||st.phase==Phase::Opening){
            C::Gate gt{};gt.open=st.phase==Phase::Open;gt.pushRow=row>=1&&config.close.push;gt.flickRow=row>=2&&config.close.flick;gt.waveRow=row>=2&&config.close.wave;
            gt.frame=frameFresh_;gt.rigid=spin::rigid(hold);gt.freeHeld=held_!=HeldKind::None;gt.freeFree=freeEligible_&&held_==HeldKind::None;
            gt.openedNs=st.openedNs;gt.resumeNs=resumeNs_;gt.lastEjectNs=lastEjectNs_;gt.lastSeatNs=lastSeatNs_;gt.lastGrabNs=lastGrabNs_;gt.lastDropNs=lastDropNs_;
            gt.now=now;gt.freeGrip=in.grips[ff];
            gt.rollBlock=rollBlocking(now);if(gt.rollBlock)bump(rollBlockSamples);
            const auto d=detector.step(t,hh,g,aimFwd_[ff],gt);
            if(d.rollBlocked)bump(rollBlockedFires);
            if(d.refusal<C::RefuseCount&&(d.refuseTick||d.refusal!=C::RefuseDwell))bump(ctr.refused[d.refusal]);
            if(d.fire!=CloseSource::None){
                if(requestClose(d.fire,hold,now,d.fire==CloseSource::Push?d.followV:-1.f)){
                    pulse(hold,.55f,25'000,now);if(d.fire==CloseSource::Push)pulse(int(ff),.35f,15'000,now);bump(ctr.closeTicks);}
                break;
            }
            if(d.refuseTick&&now>=lastRefuseTickNs_+500'000'000ull){lastRefuseTickNs_=now;pulse(hold,.10f,8'000,now);bump(ctr.refuseTicks);}
            if(detector.touch_){detector.touch_=false;pulse(int(ff),.10f,8'000,now);bump(ctr.touchTicks);}
        }
    }
    ejectCf_=nullptr; // The frame reference ends with this call
    // the PUSH follow visual (apply() reads it in the weapon's Update)
    // the follow amount while PUSH is armed; when the hand leaves it eases back to 1 over 120 ms (no snap)
    const float fv=detector.followV();
    const float dtS=followStepNs_&&now>followStepNs_?float(double(now-followStepNs_)*1e-9):0.f;followStepNs_=now;
    if(st.phase==Phase::Open&&fv<1.f&&detector.pushState()==C::PushArmed)followShown_=fv;
    else if(st.phase==Phase::Open&&followShown_<1.f)followShown_=std::min(1.f,followShown_+dtS/.12f);
    else followShown_=1.f;
    if(followShown_<1.f){followV.store(followShown_,std::memory_order_relaxed);followNs.store(now,std::memory_order_relaxed);H::cylinderFollow.store(true,std::memory_order_relaxed);}
    else H::cylinderFollow.store(false,std::memory_order_relaxed);
}
inline void Brain::publishOverlay(const runtime::ControllerSample& in,int hold,const ChamberFrame* cf,const State& st,std::uint64_t now) noexcept {
    Overlay o{};o.referenceSpace=in.referenceSpace;o.ns=now;o.hand=hold;
    const bool open=st.phase!=Phase::Closed;
    const bool visible=(open||held_!=HeldKind::None||config.debug)&&(cf||held_!=HeldKind::None);
    const auto addItem=[&](V3 p,float r,std::uint32_t style,float hl,std::uint32_t frame){
        if(o.count>=o.items.size()||!C::finite(p))return;auto& it=o.items[o.count++];it.pos[0]=p[0];it.pos[1]=p[1];it.pos[2]=p[2];it.radius=r;it.style=style;it.highlight=hl;it.frame=frame;};
    const auto addCart=[&](V3 p,float r,std::uint32_t style,float len,std::uint32_t frame,V3 axis){ // A cartridge / the cylinder occluder
        if(o.count>=o.items.size()||!C::finite(p)||!C::finite(axis)||length(axis)<.5f)return;
        addItem(p,r,style,len,frame);o.items[o.count-1].pad=packAxis(axis);};
    if(visible){
        const float pulseV=.5f+.5f*std::sin(float(double(now%1'000'000'000ull)*1e-9)*6.2831853f*2.f);
        if(cf){
            Record snap{};bool have=false;{std::lock_guard lock(modelMutex);if(const Record* r=recordForLocked(cf->weapon)){snap=*r;have=true;}}
            if(have){const bool spentLeft=snap.count(ChamberState::Spent)>0;
                if(config.debug&&open&&spentLeft&&!ejectedThisOpen_)addItem(cf->rodCur,.007f,StyleSpent,pulseV,0);else if(config.debug)addItem(cf->rodCur,.006f,StyleDebug,0.f,0);
                // The chambers show real cartridges (no chamber rings / dots). The open cylinder goes
                // first as an analytic occluder, then one procedural round per LIVE (brass case + lead
                // bullet) / SPENT (dull case, struck primer, open mouth) chamber, rim 1.2 mm proud of
                // the rear face; EMPTY = nothing.
                const float spacing=2.f*cf->radius*std::sin(3.14159265f/float(std::max(snap.n,2)));const float caseR=std::clamp(.42f*spacing,.0052f,.0062f);
                const float cylLen=std::max(.030f,2.f*std::fabs(cf->rear)),rimT=.0012f;
                int rounds=0;for(int c=0;c<snap.n;++c){const auto s=snap.st[std::size_t(c)];rounds+=s==ChamberState::Live||s==ChamberState::Spent;}
                if(rounds&&(open||config.debug)){
                    addCart(cf->rearCur,cf->radius+caseR+.0025f,StyleCylinder,cylLen,0,cf->axis);
                    for(int c=0;c<snap.n;++c){const auto s=snap.st[std::size_t(c)];if(s!=ChamberState::Live&&s!=ChamberState::Spent)continue;
                        const V3 m=slotAt(*cf,(c-snap.next+snap.n)%snap.n,false);
                        addCart(sub(m,mul(cf->axis,rimT)),caseR,s==ChamberState::Live?StyleCartLive:StyleCartSpent,.80f*cylLen,FrameInCylinder,cf->axis);}
                }
                if(config.debug)for(int c=0;c<snap.n;++c)if(snap.st[std::size_t(c)]==ChamberState::Empty) // calibration only (.reload.debug 1)
                    addItem(slotAt(*cf,(c-snap.next+snap.n)%snap.n,false),.0065f,StyleEmpty,magnetChamber_==c?1.f:0.f,0);
            }else if(config.debug){for(int c=0;c<cf->n&&c<maxChambers;++c)addItem(cf->slotCur[std::size_t(c)],.006f,StyleDebug,c==0?1.f:0.f,0);}
            if(config.debug)addItem(cf->rearCur,.006f,StyleDebug,0.f,0);
        }
        if(held_!=HeldKind::None&&tipLocalValid_){ // The held round is a cartridge (nose at the tip, along the free hand's aim); FULL = a ring
            const V3 ax=normalized(tipAxisLocal_);const float r=.0057f,len=.030f,oal=len+1.45f*r;const V3 base=sub(tipLocal_,mul(ax,oal));
            if(held_==HeldKind::Single)addCart(base,r,StyleCartLive,len,1,ax);
            else{const float rr=cf&&cf->radius>0.f?cf->radius:.0137f;const int slots=cf&&cf->n>0?cf->n:6;
                V3 e1=normalized(cross(ax,V3{0.f,1.f,0.f}));if(length(e1)<.5f)e1=normalized(cross(ax,V3{1.f,0.f,0.f}));const V3 e2=cross(ax,e1);
                for(int k=0;k<heldCount_&&k<slots;++k){const float a=6.2831853f*float(k)/float(slots);
                    addCart(add(base,add(mul(e1,rr*std::cos(a)),mul(e2,rr*std::sin(a)))),r,StyleCartLive,len,1,ax);}}
        }
        if(config.debug&&st.phase==Phase::Open&&held_==HeldKind::None&&freeEligible_&&!config.zoneAny){ // zones only with .reload.debug 1
            const auto& c=sockets_[std::size_t(H::SlotThrown)];addItem({c[0],c[1],c[2]},config.zoneChest,StyleZone,inZone_?1.f:0.f,1);
            if(freeHand_>=0&&handValid_[std::size_t(freeHand_)]){ // the belt: the nearest point of the two belt capsules to the free hand
                const auto sock=[&](int s){const auto& p=sockets_[std::size_t(s)];return V3{p[0],p[1],p[2]};};
                const V3 hpos=handPos_[std::size_t(freeHand_)];V3 best{};float bd=1e9f;
                for(const auto& seg:{std::array<int,2>{H::SlotMelee,H::SlotLasso},std::array<int,2>{H::SlotMelee,H::SlotSidearm}}){
                    const V3 a=sock(seg[0]),ab=sub(sock(seg[1]),a);const float l2=dot(ab,ab);float t=l2>1e-9f?dot(sub(hpos,a),ab)/l2:0.f;
                    const V3 p=add(a,mul(ab,std::clamp(t,0.f,1.f)));const float d=length(sub(hpos,p));if(d<bd){bd=d;best=p;}}
                if(bd<.6f)addItem(best,config.zoneBelt,StyleZone,inZone_?1.f:0.f,1);
            }
        }
    }
    for(auto& b:brass_){ // The falling rounds of the last eject(s): gravity for ~1 s (LOCAL, +Y up), tumbling; drawn last (dropped first)
        if(!b.on)continue;
        if(now<b.last||now-b.t0>1'000'000'000ull){b.on=false;continue;}
        const float dt=std::min(.05f,float(double(now-b.last)*1e-9));b.last=now;
        b.v[1]-=9.81f*dt;b.p=add(b.p,mul(b.v,dt));
        const float ang=b.spin*dt,cs=std::cos(ang),sn=std::sin(ang);const V3 k=b.spinAxis;
        b.axis=normalized(add(add(mul(b.axis,cs),mul(cross(k,b.axis),sn)),mul(k,dot(k,b.axis)*(1.f-cs))));
        addCart(b.p,b.radius,b.live?StyleCartLive:StyleCartSpent,.030f,1,b.axis);
    }
    if(!o.count&&!overlayWasVisible_)return;
    overlayWasVisible_=o.count>0;
    overlayBox.store(o);overlayLastCount.store(o.count,std::memory_order_relaxed);bump(ctr.overlayPublished);
    if(const auto sink=overlaySink.load(std::memory_order_acquire)){try{sink(o);}catch(...){}}else if(o.count)bump(ctr.overlayUnavailable);
}
inline void Brain::frame(const H::ReloadFacts& f) noexcept {
    if(!f.in)return;const auto& in=*f.in;const auto now=f.now;const int hold=f.hold;
    inputSeenNs.store(now,std::memory_order_release);bump(ctr.frames);flushPulses(now);
    if(f.resumed||(lastBrainNs_&&now>lastBrainNs_+100'000'000ull)){resumeNs_=now;detector.reset(C::BlockResume);++detector.stats.resumes;prevTipValid_=prevPinchValid_=false;}
    lastBrainNs_=now;
    sockets_=f.sockets;
    for(unsigned h=0;h<2;++h){handValid_[h]=H::detail::poseOk(in.grip[h]);if(handValid_[h])handPos_[h]={in.grip[h].position[0],in.grip[h].position[1],in.grip[h].position[2]};
        if(handValid_[h]&&H::detail::poseOk(in.aim[h])){ // the aim forward / up in the grip frame (filter 0.2)
            const Q gq{in.grip[h].orientation[0],in.grip[h].orientation[1],in.grip[h].orientation[2],in.grip[h].orientation[3]};
            const Q aq{in.aim[h].orientation[0],in.aim[h].orientation[1],in.aim[h].orientation[2],in.aim[h].orientation[3]};
            const V3 fw=C::rot(C::conj(gq),C::rot(aq,{0,0,-1})),uw=C::rot(C::conj(gq),C::rot(aq,{0,1,0}));
            aimFwd_[h]=normalized(add(mul(aimFwd_[h],.8f),mul(fw,.2f)));aimUp_[h]=normalized(add(mul(aimUp_[h],.8f),mul(uw,.2f)));}}
    const Mode mode=effectiveMode();
    const auto w=ownerWeapon.load(std::memory_order_acquire);const auto gen=H::holdGeneration.load(std::memory_order_relaxed);
    State st=stateSnapshot(now);
    // a weapon / hold change: snap, the held round vanishes, the fallback of the weapon that left the hand is cleared
    if(lastHold_==-2){lastHold_=hold;lastWeapon_=w;lastGen_=gen;} // the first frame only learns the hold
    if(hold!=lastHold_||w!=lastWeapon_||gen!=lastGen_){
        if(st.phase!=Phase::Closed){snapClosed(now,"WEAPON_LEFT_HAND");st=stateSnapshot(now);}
        held_=HeldKind::None;detector.reset(C::BlockOpen);prevTipValid_=prevPinchValid_=false;rollReset();rollSign_=0;rollLastDir_=0;
        if(lastWeapon_&&lastWeapon_!=w){std::lock_guard lock(modelMutex);if(Record* r=recordForLocked(lastWeapon_))r->fallback=false;}
        {std::lock_guard lock(modelMutex);if(Record* r=recordForLocked(lastWeapon_))r->cockAdvanced=false;}
        lastHold_=hold;lastWeapon_=w;lastGen_=gen;
    }
    if(!modeSeen_){modeSeen_=true;lastMode_=mode;} // the first frame only learns the mode (nothing can be open yet)
    if(mode!=lastMode_){ // RELOAD MODE changed: closed, fallbacks cleared
        if(st.phase!=Phase::Closed){snapClosed(now,"MODE");st=stateSnapshot(now);}
        {std::lock_guard lock(modelMutex);for(auto& r:records)r.fallback=false;}
        lastMode_=mode;
    }
    gate_=manualActive(now);const bool visual=gate_||forceVisual(now);
    const auto veto=contextVeto(now);
    gateWhy_=gate_?"OPEN":!armedNow()?(mode==Mode::Native?"NATIVE":"NOT_ARMED"):hold<0?"NOT_HELD":!revolverKind(H::heldKind.load(std::memory_order_relaxed))?"NOT_REVOLVER":
        !modelMechanics(w)?"MODEL_NATIVE":(veto&VetoNoWorld)?"NO_WORLD":(veto&VetoNoContext)?"NO_CONTEXT":veto?"CONTEXT":
        !contextFresh(contextWord.load(std::memory_order_acquire),now,config.ownerFreshNs)?"STALE":
        inList(fallbackWeapons,w)?"FALLBACK":!inList(manualWeapons,w)?"NO_RECORD":"OWNER";
    freeHand_=hold==0||hold==1?1-hold:-1;
    freeEligible_=freeHand_>=0&&handValid_[std::size_t(freeHand_)]&&f.phantom!=freeHand_&&f.support!=freeHand_;
    // closes seen (B / AutoLoad haptic, AUTO INDEX, the held round vanishes, the free hand's 300 ms tail)
    if(st.closeSeq!=seenCloseSeq_){
        seenCloseSeq_=st.closeSeq;
        if(st.lastClose==CloseSource::B||st.lastClose==CloseSource::AutoLoad)pulse(hold,.55f,25'000,now);
        {std::lock_guard lock(modelMutex);if(Record* r=recordForLocked(w))autoIndexLocked(*r,st.closeSeq);}
        if(held_!=HeldKind::None){held_=HeldKind::None;bump(ctr.drops);}
        if(freeHand_>=0&&f.captured[std::size_t(freeHand_)]&&in.grips[std::size_t(freeHand_)]>H::gripOpen)bump(ctr.handBackMasked);
        if(openStartNs_){
            if(anyActionThisOpen_){const std::uint64_t ms=(now-openStartNs_)/1'000'000ull;bump(ctr.reloadMsCount);bump(ctr.reloadMsSum,ms);maxTo(ctr.reloadMsMax,ms);}
            else if(st.lastClose!=CloseSource::Snap)bump(ctr.openAbandoned);
        }
        openStartNs_=0;tailUntil_=st.closedNs+300'000'000ull;detector.clear(C::BlockOpen);
    }
    if(st.phase!=Phase::Closed&&lastPhase_==Phase::Closed){openStartNs_=now;seatedThisOpen_=ejectedThisOpen_=anyActionThisOpen_=false;detector.newOpen();
        lastTapeNs_=std::max<std::uint64_t>(lastTapeNs_,now>40'000'000ull?now-40'000'000ull:0);prevHoldValid_=prevPinchValid_=prevTipValid_=false;shakeArmTau_=tiltSince_=0;
        lastFrameOkNs_=now;inZone_=zoneTicked_=false;magnetChamber_=occupiedChamber_=-1;}
    lastPhase_=st.phase;openish_=st.phase==Phase::Open||st.phase==Phase::Opening;
    // the RT edge while open: a refuse tick (the holsters' route zeroes RT)
    if(hold==0||hold==1){const bool down=in.triggers[std::size_t(hold)]>=(trigDown_?.45f:.55f);
        if(down&&!trigDown_&&openish_){bump(ctr.rtWhileOpen);pulse(hold,.12f,30'000,now);}trigDown_=down;}
    if(!visual){
        lastTapeNs_=std::max(lastTapeNs_,now); // nothing recorded while the gate is closed is ever replayed later
        if(st.phase!=Phase::Closed){snapClosed(now,gateWhy_);st=stateSnapshot(now);}
        if(held_!=HeldKind::None){held_=HeldKind::None;bump(ctr.drops);}
        rollReset();rollWord.store(0.f,std::memory_order_relaxed);advanceTicks.store(0,std::memory_order_relaxed);cylOmega.store(0.f,std::memory_order_relaxed);
        ChamberFrame cf{};frameFresh_=false;
        const bool cfOk=config.debug&&chamberBox.load(cf)&&cf.valid&&cf.weapon==w&&cf.hand==hold&&cf.referenceSpace==in.referenceSpace&&now<=cf.ns+frameMaxAgeNs+50'000'000ull;
        publishOverlay(in,hold,cfOk?&cf:nullptr,st,now);return;
    }
    ChamberFrame cf{};
    const bool wasFresh=frameFresh_;
    frameFresh_=chamberBox.load(cf)&&cf.valid&&cf.weapon==w&&cf.hand==hold&&cf.referenceSpace==in.referenceSpace&&(now>cf.ns?now-cf.ns:cf.ns-now)<=frameMaxAgeNs;
    if(frameFresh_)lastFrameOkNs_=now;else bump(ctr.frameStale);
    // a new reference space / a lost ChamberFrame resets the detectors (the tape before it is never read again)
    if(in.referenceSpace!=tapeSpace_||(wasFresh&&!frameFresh_)){
        if(tapeSpace_&&st.phase!=Phase::Closed){detector.reset(C::BlockResume);resumeNs_=now;prevTipValid_=prevPinchValid_=prevHoldValid_=false;}
        tapeSpace_=in.referenceSpace;}
    // fallback NO_FRAME: open without a fresh frame for > 1 s -> closed, this weapon NATIVE until it leaves the hand / the mode changes
    if(gate_&&st.phase!=Phase::Closed&&now>std::max(lastFrameOkNs_,openStartNs_)+1'000'000'000ull){
        snapClosed(now,"NO_FRAME");st=stateSnapshot(now);bump(ctr.fallbackNoFrame);
        {std::lock_guard lock(modelMutex);if(Record* r=recordForLocked(w))r->fallback=true;publishWordsLocked();}
    }
    // held round: the free grip opened elsewhere -> dropped (visual only: nothing was taken from the pool yet)
    if(held_!=HeldKind::None&&heldHand_>=0&&in.grips[std::size_t(heldHand_)]<=H::gripOpen){
        held_=HeldKind::None;heldCount_=0;bump(ctr.drops);lastDropNs_=now;detector.blockPush(C::BlockDrop);prevTipValid_=false;tipLocalValid_=false;}
    if(held_==HeldKind::None)tipLocalValid_=false;
    // the ammo zone: enter tick (free + open)
    if(st.phase==Phase::Open&&freeEligible_&&held_==HeldKind::None&&handValid_[std::size_t(freeHand_)]){
        const bool z=inAmmoZone(handPos_[std::size_t(freeHand_)],handPos_[std::size_t(hold)]);
        if(z&&!inZone_){bump(ctr.zoneEnters);pulse(freeHand_,.25f,15'000,now);}
        inZone_=z;
    }else inZone_=false;
    if(frameFresh_&&gate_)tapeStep(in,hold,cf,st,now);
    else if(gate_&&st.phase==Phase::Open)++detector.stats.staleFrames;
    st=stateSnapshot(now);
    { // ROLL: the detent spring (no contact / no motion for 80 ms / not open) + the word apply() turns the cylinder bone by
        const float dt=rollStepNs_&&now>rollStepNs_?std::min(.1f,float(double(now-rollStepNs_)*1e-9)):0.f;rollStepNs_=now;
        const bool open=st.phase==Phase::Open;const bool realistic=realisticChambers();
        if(!open&&!realistic)rollRelease(now);
        if(realistic){
            // a shot / a cock turned the cylinder one chamber (already in `next`): a spin stops there, the bone animates the step (+-alpha -> 0 =
            // the same picture at the start for an n-fold cylinder, then it visibly turns one chamber in the sense the index moved)
            const int k=advanceTicks.exchange(0,std::memory_order_relaxed);
            if(k){spinEnd(now);rollVis_=rollTarget_=float(rollDet_)*rollAlpha_;rollSettle_=false;
                const int n=chamberCountFor(w);
                if(n>=2){const float alpha=6.2831853f/float(n);const int idx=frameFresh_?boneSense(cf)*(cf.slotDir<0?-1:1):rollIdxDir_;
                    advanceVis_=std::clamp(advanceVis_+float(k*idx)*alpha,-2.f*alpha,2.f*alpha);}}
            if(advanceVis_!=0.f){advanceVis_*=std::exp(-dt/.025f);if(std::fabs(advanceVis_)<1e-3f)advanceVis_=0.f;}
            if(spinOmega_!=0.f&&!rollTouch_){ // the free spin: viscous decay (tau) + Coulomb friction; below `stop` it clicks into the detent ahead
                const float w0=spinOmega_;float w1=w0*std::exp(-dt/std::max(config.spinTau,.05f));const float dec=config.spinCoulomb*dt;
                w1=w1>0.f?std::max(0.f,w1-dec):std::min(0.f,w1+dec);
                rollVis_+=.5f*(w0+w1)*dt;rollLastDir_=w0>0.f?1:-1;rollSettle_=false;
                if(std::fabs(w1)<config.spinStop)spinEnd(now);else spinOmega_=w1;
                rollCross(freeHand_,hold,now);}
        }
        // 1:1 while touching (the cylinder stays where the hand leaves it); the visual is continuous (rollVis_).
        // Released, it eases to a detent along the way that does NOT reverse the last motion: the detent ahead in
        // rollLastDir_ - the one just passed only when it is < 2 deg behind (a click into place, not a visible
        // reversal); monotone exponential = no overshoot, crossings click + index.
        // Closed: the bone is back on its base (apply), the roll bookkeeping restarts (the whole
        // chambers already live in `next`).
        if(st.phase==Phase::Closed&&!realistic)rollReset();
        else if(rollTouch_)rollSettle_=false;
        else if(spinOmega_!=0.f){} // Spinning freely (no detent spring)
        else if(rollSettle_||std::fabs(rollResidual())>1e-5f){
            if(!rollSettle_){const float res=rollResidual();int k=rollDet_;
                if(rollLastDir_&&res*float(rollLastDir_)>.035f)k+=rollLastDir_;
                rollTarget_=float(k)*rollAlpha_;rollSettle_=true;}
            rollVis_=rollTarget_+(rollVis_-rollTarget_)*std::exp(-dt/.05f);
            if(std::fabs(rollVis_-rollTarget_)<1e-4f){rollVis_=rollTarget_;rollSettle_=false;}
            rollCross(freeHand_,hold,now);
            if(!rollSettle_&&std::fabs(rollResidual())<=1e-4f)rollVis_=float(rollDet_)*rollAlpha_;}
        else if(realistic&&st.phase==Phase::Closed&&rollDet_!=0){rollVis_=rollTarget_=0.f;rollDet_=0;} // Settled shut - the whole chambers
            // already live in `next`, the bone back on its base (the same picture for an n-fold cylinder; no endless closed writes)
        rollWord.store(rollVis_+advanceVis_,std::memory_order_relaxed);rollNs.store(now,std::memory_order_relaxed);
        cylOmega.store(spinOmega_,std::memory_order_relaxed);
    }
    publishOverlay(in,hold,frameFresh_?&cf:nullptr,st,now);
}

// ---- sinks registered in the holsters (input thread) ----
inline bool padFilter(const runtime::ControllerSample& in,int hold,std::uint64_t now) noexcept {try{return brain.pad(in,hold,now);}catch(...){return false;}}
inline void brainSink(const H::ReloadFacts& f) noexcept {try{brain.frame(f);brain.publishReport();}catch(...){}}
inline bool freeHandOwned(unsigned h,std::uint64_t now) noexcept {return brain.owned(h,now);}
inline void grabSink(unsigned h,std::uint64_t now) noexcept {try{brain.grab(h,now);}catch(...){}}
// VrControls ReloadConsumer (before the holster filter): ownership only - the edge belongs to padFilter.
inline bool ownsB() noexcept {const auto now=clockNow();return visualGate(now)&&!H::bForce.load(std::memory_order_relaxed);}
inline bool consumeB(bool pressedEdge) noexcept {
    bump(ctr.consumeCalls);const bool own=ownsB();if(own&&pressedEdge)bump(ctr.consumeOwned);return own;}
inline bool freeHandOwnedQuery(unsigned h,std::uint64_t now) noexcept {return freeHandOwned(h,now);}
inline ChamberState nextState() noexcept {
    std::lock_guard lock(modelMutex);const Record* r=recordForLocked(ownerWeapon.load(std::memory_order_acquire));
    return r?r->st[std::size_t(r->next)]:ChamberState::Empty;}

// =====================================================================================================================
// Menu row RELOAD MODE (Feature::Reload; input thread every frame from the GameXr settings glue): PROP (killed), UNAVAILABLE (install
// failed: the reason is in the report), READY + note OPEN / "4/6" / NO AMMO / NO CYLINDER / NO FRAME / CONTEXT. CYLINDER CLOSE: no note.
inline const char* fractionText(int live,int n) noexcept {
    static const auto table=[]{std::array<std::array<char,8>,100> t{};for(int a=0;a<10;++a)for(int b=0;b<10;++b)std::snprintf(t[std::size_t(a*10+b)].data(),8,"%d/%d",a,b);return t;}();
    return table[std::size_t(std::clamp(live,0,9)*10+std::clamp(n,0,9))].data();
}
inline void publishStatus() noexcept {
    namespace S=status;
    if(!config.enabled||killed(Feature::Reload)){S::publish(Feature::Reload,S::Property);return;}
    if(!installed()){S::publish(Feature::Reload,S::Unavailable);return;}
    const auto now=clockNow();const char* note=nullptr;
    if(effectiveMode()!=Mode::Native){
        const int hold=H::holdHand.load(std::memory_order_relaxed);const auto w=ownerWeapon.load(std::memory_order_acquire);
        if(hold>=0&&revolverKind(H::heldKind.load(std::memory_order_relaxed))){
            if(phase(now)!=Phase::Closed)note="OPEN";
            else if(!modelMechanics(w))note="GAME RELOAD"; // This model has no trusted geometry
            else if(cylinder.load(std::memory_order_relaxed)==int(Cylinder::Missing)&&cylinderWeapon.load(std::memory_order_relaxed)==w)note="NO CYLINDER";
            else if(inList(fallbackWeapons,w))note="NO FRAME";
            else if(contextVeto(now))note="CONTEXT";
            else{std::lock_guard lock(modelMutex);if(const Record* r=recordForLocked(w)){
                EngineAmmo a{};const int live=r->live();
                note=(!live&&ammoFor(w,a,now)&&!a.infinite&&a.pool<1.f)?"NO AMMO":fractionText(live,r->n);}}
        }
    }
    S::publish(Feature::Reload,S::Ready,note);
}

// =====================================================================================================================
// Startup (VrWeapon startup; any thread, once). Properties debug.rdrvr.vr.reload.*.
inline bool parseFloats(const std::string& text,float* out,int maxN,int& n) noexcept {
    n=0;const char* p=text.c_str();while(*p&&n<maxN){char* end=nullptr;const float x=std::strtof(p,&end);if(end==p||!std::isfinite(x))break;out[n++]=x;p=end;while(*p==' '||*p==','||*p==';')++p;}
    return n>0;}
inline Config readConfig() noexcept {
    const auto prop=[](const char* suffix){return H::property((std::string("debug.rdrvr.vr.reload")+suffix).c_str());};
    const auto off=[](const std::string& v){return !v.empty()&&v[0]=='0';};
    const auto on=[](const std::string& v){return !v.empty()&&v[0]=='1';};
    const auto num=[&](const char* s,float lo,float hi,float& out){const auto t=prop(s);if(t.empty())return;char* e{};const float v=std::strtof(t.c_str(),&e);
        if(e!=t.c_str()&&std::isfinite(v)&&v>=lo&&v<=hi)out=v;};
    const auto ms=[&](const char* s,float lo,float hi,std::uint64_t& out){float v=-1.f;num(s,lo,hi,v);if(v>=0.f)out=std::uint64_t(double(v)*1e6);};
    Config c{};
    c.enabled=!off(prop(""))&&!killed(Feature::Reload);
    { // "deg [pivotDown [pivotSide [sign]]]" (degrees, metres, metres, +1/-1)
        float v[4]{c.swingDeg,c.pivotDown,c.pivotSide,c.sign};int n=0;const auto t=prop(".swing");
        if(!t.empty()&&parseFloats(t,v,4,n)){
            if(n>=1&&std::fabs(v[0])<=170.f)c.swingDeg=std::fabs(v[0]);if(n>=2&&std::fabs(v[1])<=.1f)c.pivotDown=v[1];
            if(n>=3&&std::fabs(v[2])<=.1f)c.pivotSide=v[2];if(n>=4&&v[3]!=0.f)c.sign=v[3]<0?-1.f:1.f;}}
    ms(".ms",20.f,2000.f,c.swingNs);
    {const auto bone=prop(".bone");if(!bone.empty()){char* end=nullptr;const long v=std::strtol(bone.c_str(),&end,10);
        if(end&&*end==0&&v>=0&&v<long(maxBones))c.boneIndex=int(v);else for(std::size_t i=0;i<bone.size()&&i<31;++i)c.boneName[i]=bone[i];}}
    {const auto m=prop(".mode");if(m=="single")c.modeOverride=0;else if(m=="full")c.modeOverride=1;else if(m=="native")c.modeOverride=2;}
    {const auto s=prop(".side");if(s=="left")c.side=1;else if(s=="right")c.side=2;}
    {float v[2]{-1.f,c.rearMm};int n=0;if(parseFloats(prop(".chamber"),v,2,n)){if(v[0]>=4.f&&v[0]<=40.f)c.chamberMm=v[0];if(n>=2&&std::fabs(v[1])<=60.f)c.rearMm=v[1];}}
    {const auto r=prop(".rotsign");if(r=="-1")c.rotSign=-1;}
    {float v[3]{c.insertMm,c.insertDeg,c.frontMm};int n=0;if(parseFloats(prop(".insert"),v,3,n)){if(v[0]>=3.f&&v[0]<=80.f)c.insertMm=v[0];
        if(n>=2&&v[1]>=10.f&&v[1]<=180.f)c.insertDeg=v[1];if(n>=3&&v[2]>=0.f&&v[2]<=60.f)c.frontMm=v[2];}}
    {float v[2]{c.fullMm,c.fullDeg};int n=0;if(parseFloats(prop(".full"),v,2,n)){if(v[0]>=5.f&&v[0]<=100.f)c.fullMm=v[0];if(n>=2&&v[1]>=10.f&&v[1]<=180.f)c.fullDeg=v[1];}}
    {float v[3]{c.pinch[0],c.pinch[1],c.pinch[2]};int n=0;if(parseFloats(prop(".pinch"),v,3,n)&&n==3&&std::fabs(v[0])<=.2f&&std::fabs(v[1])<=.2f&&std::fabs(v[2])<=.2f)c.pinch={v[0],v[1],v[2]};}
    c.zoneAny=prop(".zone")=="any";num(".zoneChest",.05f,.6f,c.zoneChest);num(".zoneBelt",.05f,.6f,c.zoneBelt);
    { // Per-model handguns: .models (trust list) and.geom.<KEY> "rearMm chamberMm spurX spurY spurZ" (mm)
        std::string u=prop(".models");for(char& ch:u)ch=char(std::toupper((unsigned char)ch));
        for(std::size_t m=1;m<models.size();++m){auto& s=models[m];
            if(u=="NONE")s.manual=false;else if(!u.empty()&&(u=="ALL"||u.find(s.key)!=std::string::npos))s.manual=s.revolver;
            float v[5]{s.rearMm,s.chamberMm,s.spur[0],s.spur[1],s.spur[2]};int n=0;
            if(parseFloats(prop((std::string(".geom.")+s.key).c_str()),v,5,n)&&std::fabs(v[0])<=60.f&&(v[1]<0.f||(v[1]>=4.f&&v[1]<=40.f))&&
               std::fabs(v[2])<=60.f&&std::fabs(v[3])<=60.f&&std::fabs(v[4])<=60.f){s.geom=true;s.rearMm=v[0];s.chamberMm=v[1]<0.f?-1.f:v[1];s.spur={v[2],v[3],v[4]};}}}
    num(".shake",.2f,5.f,c.shake);num(".ejectUp",0.f,85.f,c.close.ejectUpDeg);ms(".tiltMs",0.f,5000.f,c.tiltNs);num(".tiltDeg",10.f,85.f,c.tiltDeg);c.brass=!off(prop(".brass"));
    c.rod=!off(prop(".rod"));c.keepLive=!off(prop(".keepLive"));c.autoIndex=!off(prop(".autoIndex"));c.holsterRefill=on(prop(".holsterRefill"));
    {float v=-1.f;num(".longMs",0.f,5000.f,v);if(v>=0.f)c.longNs=std::uint64_t(double(v)*1e6);}
    ms(".shotTimeoutMs",50.f,3000.f,c.shotTimeoutNs);ms(".hookGraceMs",0.f,10000.f,c.hookGraceNs);
    c.block=!off(prop(".block"));c.dry=!off(prop(".dry"));c.audit=!off(prop(".audit"));c.debug=on(prop(".debug"));c.forceOpen=on(prop(".forceOpen"));
    auto& z=c.close;z.flick=!off(prop(".flick"));z.wave=!off(prop(".wave"));z.push=!off(prop(".push"));
    num(".flickRoll",2.f,40.f,z.flickRoll);num(".flickRollUp",2.f,40.f,z.flickRollUp);num(".flickBig",2.f,60.f,z.flickBig);num(".flickNet",20.f,120.f,z.flickNetDeg);
    ms(".flickWindow",40.f,150.f,z.flickWindowNs);z.flickClose=prop(".flickDir")=="close";
    num(".waveClose",.8f,4.f,z.waveClose);{const auto a=prop(".waveAxes");z.waveAxes=a=="lateral"?1:a=="perp"?2:0;}z.waveClose_=prop(".waveDir")=="close";
    num(".waveMuzzle",0.f,85.f,z.waveMuzzleDeg);
    num(".pushCm",.5f,8.f,z.pushCm);num(".pushZone",2.f,15.f,z.pushZoneCm);num(".pushR",.5f,6.f,z.pushRCm);
    {float v[2]{z.pushHeelCm,z.pushTipsCm};int n=0;if(parseFloats(prop(".pushReach"),v,2,n)&&n==2&&v[0]>=0.f&&v[0]<=15.f&&v[1]>=0.f&&v[1]<=25.f){z.pushHeelCm=v[0];z.pushTipsCm=v[1];}}
    ms(".pushRefractory",0.f,2000.f,z.pushRefractoryNs);ms(".closeDwell",0.f,2000.f,z.closeDwellNs);ms(".slam",20.f,150.f,z.slamNs);
    z.follow=!off(prop(".follow"));num(".followMax",0.f,1.f,z.followMax);z.velocityPose=prop(".velocity")=="pose";z.shadow=on(prop(".closeShadow"));
    {float v[3]{1.f,c.rollReach*100.f,c.rollGain};int n=0;if(parseFloats(prop(".roll"),v,3,n)){c.roll=v[0]!=0.f;
        if(n>=2&&v[1]>=0.f&&v[1]<=15.f)c.rollReach=v[1]*.01f;if(n>=3&&v[2]>=.1f&&v[2]<=4.f)c.rollGain=v[2];}}
    {float v[6]{c.spinTau,c.spinCoulomb,c.spinMax,c.spinStart,c.spinStop,c.spinGain};int n=0; // .cylSpin "tauS coulomb max start stop gain"
        if(parseFloats(prop(".cylSpin"),v,6,n)){
            if(n>=1&&v[0]>=.05f&&v[0]<=10.f)c.spinTau=v[0];if(n>=2&&v[1]>=0.f&&v[1]<=50.f)c.spinCoulomb=v[1];if(n>=3&&v[2]>=5.f&&v[2]<=300.f)c.spinMax=v[2];
            if(n>=4&&v[3]>=.5f&&v[3]<=100.f)c.spinStart=v[3];if(n>=5&&v[4]>=.1f&&v[4]<=50.f)c.spinStop=v[4];if(n>=6&&v[5]>=.1f&&v[5]<=5.f)c.spinGain=v[5];}}
    return c;
}
inline void startup() noexcept {
    if(started.load())return;
    config=readConfig();brain.detector.cfg=config.close;
    if(config.enabled){
        H::bButtonSink.store(&padFilter,std::memory_order_release);H::fireBlockSink.store(&fireBlocked,std::memory_order_release);
        H::cylinderPhaseSink.store(&phaseCode,std::memory_order_release);H::rtGateSink.store(&rtGateSink,std::memory_order_release);
        H::rtHeldSink.store(&rtHeld,std::memory_order_release);H::reloadBrainSink.store(&brainSink,std::memory_order_release);
        H::freeHandOwnedSink.store(&freeHandOwned,std::memory_order_release);H::reloadGrabSink.store(&grabSink,std::memory_order_release);
    }else installState.store(int(Install::Killed),std::memory_order_release);
    started.store(true);
}
// TraceEntry (after the holsters engine + the executor). prints = every print of elf::prints matched (the caller compares the bytes) and the
// three GOT words hold their JUMP_SLOT targets. Order: env (before any hook can run) -> ModuleReload -> RequestReload GOT -> audit GOTs
// (optional). Any failure of the required parts -> effective NATIVE (no half mode).
using Replace=bool(*)(std::uintptr_t address,std::uintptr_t expected,std::uintptr_t replacement);
struct Slots {std::uintptr_t requestGot{},requestOriginal{},addGot{},addOriginal{},setGot{},setOriginal{};};
inline Install install(const Env& e,bool prints,bool gots,const Slots& s,Replace replace) noexcept {
    const auto fail=[](Install why){installState.store(int(why),std::memory_order_release);return why;};
    if(!config.enabled||killed(Feature::Reload)||!started.load(std::memory_order_acquire))return fail(Install::Killed); // not started = the weapon module is off
    printsOk.store(prints&&gots,std::memory_order_relaxed);
    if(!prints||!gots||!e)return fail(Install::EngineMismatch);
    if(!mainthread::executorInstalled())return fail(Install::NoExecutor);
    if(!H::engineReady.load(std::memory_order_acquire))return fail(Install::NoHolsters);
    env=e;env.requestReloadOriginal=reinterpret_cast<RequestReloadFn>(s.requestOriginal);
    env.addAmmoOriginal=reinterpret_cast<AddAmmoFn>(s.addOriginal);env.setAmmoOriginal=reinterpret_cast<SetAmmoFn>(s.setOriginal);
    envReady.store(true,std::memory_order_release);
    if(!mainthread::registerModule(mainthread::ModuleReload,"reload",&executorTickThunk,&executorExecute))return fail(Install::NoExecutor);
    executorOk.store(true,std::memory_order_relaxed);
    if(s.requestGot&&replace){
        if(!replace(s.requestGot,s.requestOriginal,reinterpret_cast<std::uintptr_t>(&requestReloadHook)))return fail(Install::NoHook);
        hookInstalled.store(true,std::memory_order_relaxed);
    }else if(s.requestGot)return fail(Install::NoHook);
    else hookInstalled.store(true,std::memory_order_relaxed); // host tests: the hook is called directly
    if(config.audit&&replace&&s.addGot&&s.setGot){
        const bool a=replace(s.addGot,s.addOriginal,reinterpret_cast<std::uintptr_t>(&addAmmoHook));
        const bool b=a&&replace(s.setGot,s.setOriginal,reinterpret_cast<std::uintptr_t>(&setAmmoHook));
        if(a&&!b)replace(s.addGot,reinterpret_cast<std::uintptr_t>(&addAmmoHook),s.addOriginal); // all or nothing
        auditInstalled.store(a&&b,std::memory_order_relaxed);
    }
    installState.store(int(Install::Installed),std::memory_order_release);return Install::Installed;
}
// Host tests: back to a clean, uninstalled module (keeps the startup config).
inline void resetForTests() noexcept {
    {std::lock_guard lock(modelMutex);records={};pendingShots={};inFlight={};firesApplied={};nextOpId=1;pressCounter=currentPress=shotPress=decisions=0;}
    for(auto& f:fireCounts)f.store(0);for(auto& x:manualWeapons)x.store(0);for(auto& x:fallbackWeapons)x.store(0);for(auto& x:cylinderTypes)x.store(0);
    {std::lock_guard lock(stateMutex);state=State{};}
    modelWeapon.store(0);modelWord.store(ModelUnknown); // Tests publish a model (publishModel(w,"base_pistol_REVOLVER_Cattleman"))
    closeSeqWord.store(0);closedAtWord.store(0);openedAtWord.store(0);followV.store(1.f);followNs.store(0);
    contextWord.store(0);ownerWeapon.store(0);ownerNs.store(0);ownerPoseAllowed.store(false);deadEyeSeenNs.store(0);
    armedWord.store(false);allowanceUntilNs.store(0);allowance.store(0);rtPressWord.store(0);rtCurrentPress.store(0);
    installState.store(int(Install::NotAttempted));hookInstalled.store(false);auditInstalled.store(false);printsOk.store(false);executorOk.store(false);
    envReady.store(false);env=Env{};tickState=TickState{};lastOpExpiredNs=0;lastLongPressWord.store(0);
    unmanagedWeapon.store(0);unmanagedSinceNs.store(0);inputSeenNs.store(0);
    brain.resetForTests();brain.detector.cfg=config.close;{std::lock_guard lock(brainReportMutex);brainReport=BrainReport{};}ammoBox.store(EngineAmmo{});chamberBox.store(ChamberFrame{});overlayBox.store(Overlay{});
    {std::lock_guard lock(recordMutex);record=SwingRecord{};instBases={};typeBases={};}cylinder.store(int(Cylinder::Unknown));cylinderWeapon.store(0);
    H::bForce.store(false);H::cylinderClosedNs.store(0);H::cylinderFollow.store(false);rollWord.store(0.f);rollNs.store(0);
    for(auto* a:{&chLiveShots,&chDryClicks,&chAdvances,&chCockAdvances,&cylSpins,&cylSpinMilliRevs,&cylSpinsStopped})a->store(0);
    cylLastSpinRevs.store(0.f);cylOmega.store(0.f);advanceTicks.store(0);
}

// =====================================================================================================================
// Report "vr":{"weapon":{"reload":<this>}} (any thread).
inline std::string report() {
    std::string out;out.reserve(8192);char b[1536];const auto now=clockNow();
    const auto u=[](const std::atomic<std::uint64_t>& a){return (unsigned long long)a.load(std::memory_order_relaxed);};
    const auto st=stateSnapshot(now);const float a=amount(now);const auto w=ownerWeapon.load();
    Record r{};bool have=false;int unacked=0;{std::lock_guard lock(modelMutex);if(const Record* p=recordForLocked(w)){r=*p;have=true;unacked=unackedLocked(w);}}
    char states[maxChambers+1]{};for(int i=0;i<r.n&&i<maxChambers;++i)states[i]=chamberChar(r.st[std::size_t(i)]);
    EngineAmmo ea{};const bool ammo=ammoFor(w,ea,now);
    const Mode mode=effectiveMode();const unsigned row=closeRow();
    BrainReport br{};{std::lock_guard lock(brainReportMutex);br=brainReport;}
    std::snprintf(b,sizeof b,"{\"enabled\":%s,\"started\":%s,\"mode\":\"%s\",\"modeSource\":\"%s\",\"effectiveMode\":\"%s\",\"note\":\"%s\","
        "\"install\":{\"state\":\"%s\",\"hook\":%s,\"audit\":%s,\"prints\":%s,\"executor\":%s},\"ownsB\":%s,\"gate\":{\"open\":%s,\"why\":\"%s\",\"armed\":%s,"
        "\"veto\":%u,\"ownerAgeMs\":%.0f,\"poseAllowed\":%s,\"forceOpen\":%s},\"phase\":\"%s\",\"amount\":%.2f,\"cylinder\":\"%s\",\"bone\":%d,"
        "\"side\":\"%s\",\"swingSign\":%.0f,\"config\":{\"swingDeg\":%.1f,\"swingMs\":%.0f,\"pivotDown\":%.3f,\"pivotSide\":%.3f,\"sign\":%.0f,"
        "\"insert\":[%.0f,%.0f,%.0f],\"full\":[%.0f,%.0f],\"shake\":%.2f,\"longMs\":%.0f,\"debug\":%s,\"keepLive\":%s,\"autoIndex\":%s,\"holsterRefill\":%s,\"zone\":\"%s\"},",
        config.enabled?"true":"false",started.load()?"true":"false",modeName(mode==Mode::Native&&installed()?Mode(settings().reloadMode()):mode),modeSource(),modeName(mode),
        status::noteOf(Feature::Reload)?status::noteOf(Feature::Reload):"",installName(Install(installState.load())),hookInstalled.load()?"true":"false",
        auditInstalled.load()?"true":"false",printsOk.load()?"true":"false",executorOk.load()?"true":"false",ownsB()?"true":"false",
        manualActive(now)?"true":"false",br.gateWhy,armedWord.load()?"true":"false",contextVeto(now),
        ownerNs.load()&&now>=ownerNs.load()?double(now-ownerNs.load())/1e6:-1.0,ownerPoseAllowed.load()?"true":"false",config.forceOpen?"true":"false",
        phaseName(st.phase),double(a),cylinder.load()==int(Cylinder::Found)?"FOUND":cylinder.load()==int(Cylinder::Missing)?"MISSING":"UNKNOWN",record.bone,
        config.side==1?"LEFT":config.side==2?"RIGHT":"AUTO",double(swingSignFor(H::holdHand.load())),double(config.swingDeg),double(config.swingNs)/1e6,
        double(config.pivotDown),double(config.pivotSide),double(config.sign),double(config.insertMm),double(config.insertDeg),double(config.frontMm),
        double(config.fullMm),double(config.fullDeg),double(config.shake),double(config.longNs)/1e6,config.debug?"true":"false",config.keepLive?"true":"false",
        config.autoIndex?"true":"false",config.holsterRefill?"true":"false",config.zoneAny?"ANY":"BODY");out+=b;
    {const auto ow=ownerWeapon.load();const auto& ms=specOf(ow);
        std::snprintf(b,sizeof b,"\"model\":{\"key\":\"%s\",\"bound\":%s,\"mechanics\":%s,\"geom\":%s,\"rearMm\":%.1f,\"chamberMm\":%.1f},",ms.key,
            ow&&modelWeapon.load()==ow?"true":"false",modelMechanics(ow)?"true":"false",ms.geom?"true":"false",double(ms.rearMm),double(ms.chamberMm));out+=b;}
    { // The context word (fresh / stale, raw veto incl. NO_CONTEXT 2048 / NO_WORLD 4096), the hook's UNMANAGED grace, the executor snaps
        const auto word=contextWord.load();const auto uw=unmanagedWeapon.load();const auto us=unmanagedSinceNs.load();
        std::snprintf(b,sizeof b,"\"context\":{\"word\":\"%s\",\"vetoRaw\":%u,\"notes\":%llu,\"empty\":%llu,\"noContext\":%llu,\"noWorld\":%llu},"
            "\"unmanaged\":{\"active\":%s,\"owner\":%s,\"ageMs\":%.0f,\"graceMs\":%.0f,\"starts\":%llu},\"execSnaps\":{\"GATE_CLOSED\":%llu,\"NO_INPUT\":%llu},"
            "\"longAfterRefusal\":%llu,",
            !word?"NONE":contextFresh(word,now,config.ownerFreshNs)?"FRESH":"STALE",unsigned(word&0xffffu),u(contextNotes),u(contextEmptyNotes),
            u(contextNoContextNotes),u(contextNoWorldNotes),uw?"true":"false",uw&&uw==w?"true":"false",uw&&us&&now>=us?double(now-us)/1e6:-1.0,
            double(config.hookGraceNs)/1e6,u(ctr.unmanagedStarts),u(execSnapsGate),u(execSnapsNoInput),u(ctr.longAfterRefusal));out+=b;}
    std::snprintf(b,sizeof b,"\"chambers\":{\"n\":%d,\"states\":\"%s\",\"next\":%d,\"live\":%d,\"spent\":%d,\"empty\":%d,\"firing\":%d,\"autoIndexed\":%llu,\"unacked\":%d},"
        "\"engine\":{\"clip\":%.2f,\"perClip\":%.1f,\"pool\":%.1f,\"poolMax\":%.1f,\"infinite\":%s,\"ammoEnum\":%d,\"nativeReloading\":%s,\"holsterRefillBlocked\":%llu,"
        "\"ops\":{\"posted\":%llu,\"postFailed\":%llu,\"done\":%llu,\"failed\":%llu,\"expired\":%llu,\"inserted\":%llu,\"ejectReturned\":%llu,\"liveLostCap\":%llu,"
        "\"liveLost\":%llu,\"rollbacks\":%llu,\"lastFail\":\"%s\"},\"ticks\":%llu,\"tickExceptions\":%llu,\"executes\":%llu},",
        r.n,states,r.next,have?r.live():0,have?r.count(ChamberState::Spent):0,have?r.count(ChamberState::Empty):0,have?r.count(ChamberState::Firing):0,u(ctr.autoIndexed),unacked,
        ammo?double(ea.clip):-1.0,ammo?double(ea.perClip):-1.0,ammo?double(ea.pool):-1.0,ammo?double(ea.poolMax):-1.0,ammo&&ea.infinite?"true":"false",ammo?ea.ammoEnum:-1,
        ammo&&ea.nativeReloading?"true":"false",u(ctr.holsterRefillBlocked),u(ctr.opsPosted),u(ctr.opsPostFailed),u(ctr.opsDone),u(ctr.opsFailed),u(ctr.opsExpired),u(ctr.opsInserted),
        u(ctr.ejectReturned),u(ctr.liveLostCap),u(ctr.liveLost),u(ctr.opsRollbacks),lastOpFail.load(),u(ticks),u(tickExceptions),u(executeCalls));out+=b;
    { // CHAMBERS: firingChamber = the chamber aligned with the barrel (REALISTIC: the next shot turns one chamber first unless cocked)
        const bool rc=realisticChambers();const float om=cylOmega.load();
        std::snprintf(b,sizeof b,"\"chambersMode\":\"%s\",\"realistic\":{\"firingChamber\":%d,\"firingState\":\"%c\",\"cocked\":%s,\"liveShots\":%llu,"
            "\"dryClicks\":%llu,\"advances\":%llu,\"cockAdvances\":%llu,\"spins\":%llu,\"spinsStopped\":%llu,\"spinRevs\":%.2f,\"lastSpinRevs\":%.2f,"
            "\"spinning\":%s,\"omega\":%.1f,\"spinCfg\":[%.2f,%.1f,%.0f,%.1f,%.1f,%.2f]},",
            rc?"REALISTIC":"SIMPLE",have?r.next:-1,have&&r.n?chamberChar(r.st[std::size_t(r.next)]):'-',have&&r.cockAdvanced?"true":"false",
            u(chLiveShots),u(chDryClicks),u(chAdvances),u(chCockAdvances),u(cylSpins),u(cylSpinsStopped),double(cylSpinMilliRevs.load())/1000.0,
            double(cylLastSpinRevs.load()),om!=0.f?"true":"false",double(om),double(config.spinTau),double(config.spinCoulomb),double(config.spinMax),
            double(config.spinStart),double(config.spinStop),double(config.spinGain));out+=b;}
    out+="\"hook\":{\"calls\":"+std::to_string(hookCalls.load())+",\"blocked\":"+std::to_string(hookBlocked.load())+",\"passed\":{";
    for(unsigned i=0;i<PassCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",hookPassNames[i],u(hookPassed[i]));out+=b;}
    out+="},\"lrTop\":[";{bool first=true;for(const auto& e:hookLrTop){const auto lr=e.lr.load();if(!lr)continue;
        std::snprintf(b,sizeof b,"%s{\"lr\":\"0x%llx\",\"lr2\":\"0x%llx\",\"n\":%llu,\"cause\":\"%s\"}",first?"":",",(unsigned long long)lr,(unsigned long long)e.lr2.load(),
            u(e.n),elf::causeNames[elf::causeOf(lr)]);out+=b;first=false;}}
    out+="],\"tids\":[";{bool first=true;for(const auto& t:hookTids.ids){const auto v=t.load();if(!v)continue;out+=(first?"":",")+std::to_string(v);first=false;}}
    out+="]},\"audit\":{\"addAmmo\":"+std::to_string(auditAdd.load())+",\"setAmmo\":"+std::to_string(auditSet.load())+",\"tids\":[";
    {bool first=true;for(const auto& t:auditTids.ids){const auto v=t.load();if(!v)continue;out+=(first?"":",")+std::to_string(v);first=false;}}
    out+="],\"ring\":[";{std::lock_guard lock(auditMutex);bool first=true;
        for(unsigned i=0;i<auditRing.size();++i){const auto& e=auditRing[(auditNext+i)%auditRing.size()];if(!e.ns)continue;
            std::snprintf(b,sizeof b,"%s{\"op\":\"%s\",\"lr\":\"0x%llx\",\"lr2\":\"0x%llx\",\"cause\":\"%s\",\"value\":%.1f,\"before\":%.1f,\"after\":%.1f}",first?"":",",
                e.op==1?"ADD":"SET",(unsigned long long)e.lr,(unsigned long long)e.lr2,elf::causeNames[e.cause<elf::CauseCount?e.cause:0],double(e.value),double(e.before),double(e.after));
            out+=b;first=false;}}
    out+="]},\"shots\":{\"fireTids\":[";{bool first=true;for(const auto& t:fireTids.ids){const auto v=t.load();if(!v)continue;out+=(first?"":",")+std::to_string(v);first=false;}}
    std::snprintf(b,sizeof b,"],\"passes\":%llu,\"confirmed\":%llu,\"refused\":%llu,\"unseen\":%llu,\"withoutLive\":%llu,\"oneShotBlocks\":%llu},"
        "\"reconcile\":{\"drains\":%llu,\"refills\":%llu,\"debounced\":%llu,\"skippedRace\":%llu,\"mismatchMax\":%llu,\"refillCause\":{\"DEAD_EYE\":%llu,\"FIRE_TASK\":%llu,"
        "\"RELOAD_TASK\":%llu,\"NATIVE_CONTEXT\":%llu,\"NATIVE_LONGPRESS\":%llu,\"OP_EXPIRED\":%llu,\"DUEL\":%llu,\"HOLSTER\":%llu,\"UNKNOWN\":%llu}},",
        u(ctr.shotPasses),u(ctr.shotConfirmed),u(ctr.shotRefused),u(ctr.shotUnseen),u(ctr.shotWithoutLive),u(ctr.oneShotBlocks),u(ctr.reconcileDrains),u(ctr.reconcileRefills),
        u(ctr.reconcileDebounced),u(ctr.reconcileSkippedRace),u(ctr.mismatchMax),u(ctr.refillCause[elf::CauseDeadEye]),u(ctr.refillCause[elf::CauseFireTask]),
        u(ctr.refillCause[elf::CauseReloadTask]),u(ctr.refillContext),u(ctr.refillLongPress),u(ctr.refillOpExpired),u(ctr.refillDuel),u(ctr.refillCause[elf::CauseHolster]),
        u(ctr.refillCause[elf::CauseUnknown]));out+=b;
    std::snprintf(b,sizeof b,"\"gestures\":{\"opens\":%llu,\"opensRefused\":{\"HAND_BUSY\":%llu},\"closes\":{\"B\":%llu,\"FLICK\":%llu,\"WAVE\":%llu,\"PUSH\":%llu,"
        "\"AUTOLOAD\":%llu,\"SNAP\":%llu},\"lastSnap\":\"%s\",\"ejects\":{\"SHAKE\":%llu,\"ROD\":%llu,\"TILT\":%llu},\"ejectBlocked\":{\"AFTER_SEAT\":%llu,\"NO_SPENT\":%llu,"
        "\"NOTHING\":%llu},\"ejectsAfterSeat\":%llu,\"shakeSubThreshold\":[%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f],\"tiltAborted\":%llu,\"ejectedSpent\":%llu,"
        "\"ejectedLive\":%llu,\"grabs\":{\"SINGLE\":%llu,\"FULL\":%llu},\"grabRefused\":{\"NO_AMMO\":%llu,\"NO_EMPTY\":%llu,\"HAND_BUSY\":%llu},"
        "\"grabOutsideZone\":{\"n\":%llu,\"lastM\":%.2f},\"inserts\":%llu,\"roundsSeated\":%llu,\"insertRefused\":{\"OCCUPIED\":%llu,\"SPENT\":%llu},",
        u(ctr.opens),u(opensRefusedBusy),u(closeCounts[unsigned(CloseSource::B)]),u(closeCounts[unsigned(CloseSource::Flick)]),u(closeCounts[unsigned(CloseSource::Wave)]),
        u(closeCounts[unsigned(CloseSource::Push)]),u(closeCounts[unsigned(CloseSource::AutoLoad)]),u(closeCounts[unsigned(CloseSource::Snap)]),lastSnapReason.load(),
        u(ctr.ejectShake),u(ctr.ejectRod),u(ctr.ejectTilt),u(ctr.ejectAfterSeat),u(ctr.ejectNoSpent),u(ctr.ejectNothing),u(ctr.ejectsAfterSeat),
        double(ctr.shakeSub[0].load()),double(ctr.shakeSub[1].load()),double(ctr.shakeSub[2].load()),double(ctr.shakeSub[3].load()),double(ctr.shakeSub[4].load()),
        double(ctr.shakeSub[5].load()),double(ctr.shakeSub[6].load()),double(ctr.shakeSub[7].load()),u(ctr.tiltAborted),u(ctr.ejectedSpent),u(ctr.ejectedLive),
        u(ctr.grabsSingle),u(ctr.grabsFull),u(ctr.grabNoAmmo),u(ctr.grabNoEmpty),u(ctr.grabBusy),u(ctr.grabOutside),double(ctr.grabOutsideLastM.load()),u(ctr.inserts),u(ctr.roundsSeated),
        u(ctr.insertOccupied),u(ctr.insertSpent));out+=b;
    out+="\"insertNearMiss\":{\"distMm7\":[";for(unsigned i=0;i<8;++i)out+=(i?",":"")+std::to_string(ctr.nearMissMm[i].load());
    out+="],\"angleDeg15\":[";for(unsigned i=0;i<8;++i)out+=(i?",":"")+std::to_string(ctr.nearMissDeg[i].load());
    const auto rc=ctr.reloadMsCount.load();
    std::snprintf(b,sizeof b,"]},\"drops\":%llu,\"zoneEnters\":%llu,\"rtWhileOpen\":%llu,\"openAbandoned\":%llu,\"reloadMs\":{\"n\":%llu,\"avg\":%.0f,\"max\":%llu},"
        "\"handBackMasked\":%llu,\"magnetTicks\":%llu,\"held\":\"%s\",\"heldCount\":%d,\"freeHand\":%d},",
        u(ctr.drops),u(ctr.zoneEnters),u(ctr.rtWhileOpen),u(ctr.openAbandoned),(unsigned long long)rc,rc?double(ctr.reloadMsSum.load())/double(rc):0.0,u(ctr.reloadMsMax),
        u(ctr.handBackMasked),u(ctr.magnetTicks),heldName(br.held),br.heldCount,br.freeHand);out+=b;
    {
        const auto& s=br.stats;const auto& z=config.close;
        const auto ago=st.closedNs&&now>=st.closedNs?double(now-st.closedNs)/1e6:-1.0;
        std::snprintf(b,sizeof b,"\"close\":{\"row\":\"%s\",\"shadow\":%s,\"lastSource\":\"%s\",\"lastAgoMs\":%.0f,\"slamMs\":%.0f,\"followV\":%.2f,\"refused\":{",
            row==0?"B ONLY":row==1?"B AND HAND":"ALL",z.shadow?"true":"false",C::sourceNames[unsigned(st.lastClose)],ago,double(z.slamNs)/1e6,double(followV.load()));out+=b;
        for(unsigned i=0;i<C::RefuseCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",C::refusalNames[i],u(ctr.refused[i]));out+=b;}
        const auto vs0=s.velSamples[0]?double(s.velValid[0])/double(s.velSamples[0]):0.0,vs1=s.velSamples[1]?double(s.velValid[1])/double(s.velSamples[1]):0.0;
        std::snprintf(b,sizeof b,"},\"refuseTicks\":%llu,\"timing\":{\"velocity\":\"%s\",\"runtimeSamples\":%llu,\"poseSamples\":%llu,\"poseNsValid\":%llu,\"snapFallback\":%llu,"
            "\"velValidRate\":[%.2f,%.2f],\"stampErrMaxUs\":%llu,\"stampErrUs\":[",u(ctr.refuseTicks),z.velocityPose?"POSE":"RUNTIME",(unsigned long long)s.runtimeSamples,
            (unsigned long long)s.poseSamples,(unsigned long long)s.poseNsValid,(unsigned long long)s.snapFallback,vs0,vs1,(unsigned long long)s.stampErrMaxUs);out+=b;
        for(unsigned i=0;i<8;++i)out+=(i?",":"")+std::to_string(s.stampErrHist[i]);out+="],\"intervalUs\":[";
        for(unsigned i=0;i<8;++i)out+=(i?",":"")+std::to_string(s.intervalHist[i]);
        std::snprintf(b,sizeof b,"]},\"detector\":{\"tapeSamples\":%llu,\"staleFrames\":%llu,\"resets\":%llu,\"resumes\":%llu,\"glitches\":%llu,\"angularOutliers\":%llu},"
            "\"flick\":{\"thr\":%.1f,\"thrUp\":%.1f,\"big\":%.1f,\"net\":%.0f,\"dir\":\"%s\",\"rollJMax\":%.2f,\"rollJLast\":%.2f,\"rollNetLast\":%.1f,\"rollNear\":%llu,"
            "\"rollNetRejected\":%llu,\"rollClose\":%llu,\"rollOpen\":%llu},\"wave\":{\"thr\":%.2f,\"axes\":\"%s\",\"dir\":\"%s\",\"dvLast\":%.2f,\"near\":%llu,"
            "\"dvMax\":{\"tangentClose\":%.2f,\"tangentOpen\":%.2f,\"lateral\":%.2f,\"axial\":%.2f}},",
            (unsigned long long)s.tapeSamples,(unsigned long long)s.staleFrames,(unsigned long long)s.resets,(unsigned long long)s.resumes,(unsigned long long)s.glitches,
            (unsigned long long)s.angularOutliers,double(z.flickRoll),double(z.flickRollUp),double(z.flickBig),double(z.flickNetDeg),z.flickClose?"CLOSE":"ANY",
            double(s.rollJMax),double(s.rollJLast),double(s.rollNetLast),(unsigned long long)s.rollNear,(unsigned long long)s.rollNetRejected,(unsigned long long)s.rollClose,
            (unsigned long long)s.rollOpen,double(z.waveClose),z.waveAxes==1?"LATERAL":z.waveAxes==2?"PERP":"TANGENT",z.waveClose_?"CLOSE":"ANY",double(s.dvLast),
            (unsigned long long)s.waveNear,double(s.dvTangentClose),double(s.dvTangentOpen),double(s.dvLateral),double(s.dvAxial));out+=b;
        const auto& af=br.aimFwd;
        std::snprintf(b,sizeof b,"\"push\":{\"entries\":%llu,\"wrongSide\":%llu,\"blocked\":{",(unsigned long long)s.pushEntries,(unsigned long long)s.pushWrongSide);out+=b;
        for(unsigned i=0;i<C::BlockCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",C::pushBlockNames[i],(unsigned long long)s.pushBlocked[i]);out+=b;}
        std::snprintf(b,sizeof b,"},\"armed\":%llu,\"latched\":%llu,\"aborted\":%llu,\"glitchRejected\":%llu,\"axialSkipped\":%llu,\"trackedGaps\":%llu,\"gapResets\":%llu,"
            "\"maxCm\":%.2f,\"followFrames\":%llu,\"lastRelSpeed\":%.2f,\"ctrlDistCmAtLatch\":%.1f,\"gripAtLatch\":{\"OPEN\":%llu,\"HALF\":%llu,\"FIST\":%llu},"
            "\"aimFwdInGrip\":[[%.3f,%.3f,%.3f],[%.3f,%.3f,%.3f]]},\"shadow\":{\"FLICK\":%llu,\"WAVE\":%llu,\"PUSH\":%llu},"
            "\"b\":{\"edgesPad\":%llu,\"presses\":%llu,\"consumeCalls\":%llu,\"consumeOwned\":%llu,\"afterGesture\":%llu,\"spin\":%llu}},",
            (unsigned long long)s.pushArmed,(unsigned long long)s.pushLatched,(unsigned long long)s.pushAborted,(unsigned long long)s.pushGlitchRejected,
            (unsigned long long)s.pushAxialSkipped,(unsigned long long)s.pushTrackedGaps,(unsigned long long)s.pushGapResets,double(s.pushMaxCm),
            (unsigned long long)s.pushFollowFrames,double(s.pushLastRelSpeed),double(s.pushCtrlDistCm),(unsigned long long)s.pushGripAtLatch[0],
            (unsigned long long)s.pushGripAtLatch[1],(unsigned long long)s.pushGripAtLatch[2],double(af[0][0]),double(af[0][1]),double(af[0][2]),double(af[1][0]),
            double(af[1][1]),double(af[1][2]),(unsigned long long)s.shadow[unsigned(CloseSource::Flick)],(unsigned long long)s.shadow[unsigned(CloseSource::Wave)],
            (unsigned long long)s.shadow[unsigned(CloseSource::Push)],u(ctr.bEdgesPad),u(ctr.pressesB),u(ctr.consumeCalls),u(ctr.consumeOwned),u(ctr.bAfterGesture),u(ctr.bSpin));out+=b;
    }
    const int ist=installState.load();
    std::snprintf(b,sizeof b,"\"longPress\":{\"native\":%llu,\"autoload\":%llu},\"fallback\":{\"NO_FRAME\":%llu,\"NO_HOOK\":%d,\"NO_EXECUTOR\":%d,\"MISMATCH\":%d,\"install\":\"%s\"},"
        "\"rt\":{\"edges\":%llu,\"forwarded\":%llu,\"dry\":%llu,\"native\":%llu,\"whileOpen\":%llu},"
        "\"frame\":{\"published\":%llu,\"fresh\":%s,\"stale\":%llu,\"failures\":%llu,\"latestMiss\":%llu,\"radiusMm\":%.1f,\"radiusAuto\":%s,\"rearMm\":%.1f,\"ageMs\":%.0f},"
        "\"overlay\":{\"published\":%llu,\"unavailable\":%llu,\"items\":%llu},\"records\":{\"created\":%llu,\"dropped\":%llu},"
        "\"holsters\":{\"captures\":%llu,\"capturePresses\":%llu,\"rtGateCalls\":%llu,\"rtDeferred\":%llu,\"rtReplayed\":%llu,\"rtLatched\":%llu,\"recentCloseGuards\":%llu},",
        u(ctr.longNative),u(ctr.longAutoload),u(ctr.fallbackNoFrame),ist==int(Install::NoHook)?1:0,ist==int(Install::NoExecutor)||ist==int(Install::NoHolsters)?1:0,
        ist==int(Install::EngineMismatch)?1:0,installName(Install(ist)),u(ctr.rtEdges),u(ctr.rtForwarded),u(ctr.rtDry),u(ctr.rtNative),u(ctr.rtWhileOpen),
        u(ctr.framePublished),br.frameFresh?"true":"false",u(ctr.frameStale),u(frameFailures),u(ctr.latestMiss),double(ctr.radiusMm.load()),ctr.radiusAuto.load()?"true":"false",
        double(ctr.rearMmUsed.load()),lastFrameNs.load()&&now>=lastFrameNs.load()?double(now-lastFrameNs.load())/1e6:-1.0,u(ctr.overlayPublished),u(ctr.overlayUnavailable),
        u(overlayLastCount),u(ctr.recordsCreated),u(ctr.recordsDropped),u(H::reloadCaptures),u(H::reloadCapturePresses),u(H::rtGateCalls),u(H::rtDeferredEdges),
        u(H::rtDeferredReplayed),u(H::rtGateLatched),u(H::recentCloseGuards));out+=b;
    std::snprintf(b,sizeof b,"\"roll\":{\"enabled\":%s,\"reachCm\":%.1f,\"gain\":%.2f,\"contacts\":%llu,\"steps\":%llu,\"detents\":%llu,\"deg\":%.1f,"
        "\"rolling\":%llu,\"gestureBlockSamples\":%llu,\"gestureBlockedFires\":%llu,\"worldSense\":%d,\"gripSense\":%d,"
        "\"visualDeg\":%.1f,\"signFlips\":%llu,\"lastSign\":%d,\"boneSense\":%d},",
        config.roll?"true":"false",double(config.rollReach*100.f),double(config.rollGain),u(rollContacts),u(rollSteps),u(rollDetents),double(rollWord.load())*57.29578,
        u(rollActivations),u(rollBlockSamples),u(rollBlockedFires),rollWorldSense.load(),rollGripSense.load(),
        double(rollWord.load())*57.29578,u(rollSignFlips),rollLastSign.load(),rollBoneSense.load());out+=b;
    std::snprintf(b,sizeof b,"\"engineOld\":{\"applyCalls\":%llu,\"applied\":%llu,\"restored\":%llu,\"baseRefreshed\":%llu,\"declined\":%llu,\"writeFailures\":%llu,"
        "\"lastDecline\":\"%s\",\"angleDeg\":%.1f,\"baseRestores\":%llu,\"staleOpenFixed\":%llu,\"staleBaseRejected\":%llu,\"drawEnforce\":%llu,"
        "\"instEvicted\":%llu,\"instDirty\":%u},\"bones\":[",u(applyCalls),u(applied),u(restored),u(baseRefreshed),u(declined),u(writeFailures),lastDecline.load(),
        double(lastAngleDeg.load()),u(baseRestores),u(staleOpenFixed),u(staleBaseRejected),u(drawEnforce),u(instEvicted),
        [&]{unsigned n=0;std::lock_guard lock(recordMutex);for(const auto& i:instBases)n+=i.dirty?1u:0u;return n;}());out+=b;
    SwingRecord sr{};{std::lock_guard lock(recordMutex);sr=record;}
    for(unsigned i=0;i<sr.count&&i<maxBones;++i){std::string nm(sr.names[i].data());for(char& ch:nm)if(ch=='"'||ch=='\\'||(unsigned char)ch<0x20||(unsigned char)ch>0x7e)ch='?';
        std::snprintf(b,sizeof b,"%s{\"name\":\"%s\",\"parent\":%d}",i?",":"",nm.c_str(),sr.parents[i]);out+=b;}
    out+="]}";return out;
}
} // namespace rdrvr::vr::reload
