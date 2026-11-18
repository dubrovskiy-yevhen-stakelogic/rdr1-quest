#pragma once
// NO AUTO AIM: the stock hard lock and soft lock never turn the camera or the body while a VR weapon is in hand.
//
// Stock chain (librdr.so 1.58 arm64, vaddr). Frame order sagShellDerived::UpdateCamera: camManager::PreUpdate (camGlueCMXGame::PreUpdate)
// -> sagPlayerMgr::PreCameraUpdate -> camManager::Update -> PostCameraUpdate.
//  - sagPlayer::PreCameraUpdate 0x269ba3c: UpdateZoomState, then a tail call (b 0x2c951b0, the only reference to that stub) of
//    tgtReticleComponent::PreUpdate(bool) on the player actor's reticle ([actor +0xb8]).
//  - UpdateZoomState 0x269c03c..0x269c0c0: the desired zoom is 2 while player +0x244 is 1 / 2 or force aim (player +0x3bc bit 1, which the
//    holsters set the moment a firearm or a throwable is in a VR hand); a change of sagPlayerStates +0x14 calls SetLockOnActive(zoom == 2)
//    (0x269bcf4): reticle +0x5818 = arg, then LockOnTarget (0x26ca704).
//  - PreUpdate 0x26c12ac: FindBestHardLockTarget -> SetHardLockTarget writes the candidate +0x5780 (0x26c1f9c / 0x26c1ff4); its tail
//    (0x26c1848..0x26c1880) calls LockOnTarget while +0x5818 != 0. LockOnTarget state 1 (0x26c212c..): lock target +0x577c = the candidate,
//    state 2 (0x26c20fc..0x26c210c).
//  - camGlueCMXGame::UpdateReticleParams 0x263329c (called from camGlueCMXGame::PreUpdate 0x262f318 and UpdateCammachParameters 0x262fb6c,
//    both through the stub 0x2c92880 / JUMP_SLOT 0x2e51aa0): the CMX parameter set [[glue +0x620] +0x30] (GetParameterSet 0x25c1c54), its
//    slot table [set +8]: +0x350 / +0x358 soft-lock influence x / y (CalculateSoftLockInfluence), +0x360 aim ramp, +0x320 acquisition point,
//    +0x338 lock target (0 in combat mode 1, glue +0x8f8), +0x340 candidate, +0x328 lock state (+0x5818), +0x348 stick bumped (a byte).
//  - The camera message "OrientPlayerToTarget" sets reticle +0x57f0 (OnCameraNotify 0x26c0ec4..0x26c0ec8, a byte); aniNavigation::UpdateTurns
//    0x1e77738..0x1e77750 turns the body toward the lock target +0x577c while it is set.
// The VR world yaw is the stock camera's heading (VrCamera), so a lock turns the world in the headset and the body with it.
//
// Fix: three GOT hooks, all on the engine main thread, each calling the original first.
//  H1 tgtReticleComponent::PreUpdate (JUMP_SLOT 0x2e52f38): while the gate is open, lock state +0x5818, lock target +0x577c, candidate
//     +0x5780 and the orient flag +0x57f0 are cleared after the call; the camera update and UpdateTurns that follow see no lock, and the
//     next aim-start LockOnTarget finds no candidate.
//  H3 tgtReticleComponent::SetLockOnActive (JUMP_SLOT 0x2e52f78, the original first): its last argument is the stock lock-on request
//     (UpdateZoomState calls it on every zoom change with zoom == 2, 0x269c0c0 -> 0x269bcf0). The stock word +0x5818 is otherwise only
//     rewritten by LockOnTarget / PreUpdate (2 locked, 0 nothing to lock: 0x26c210c, 0x26c2288, 0x26c2298, 0x26c17e8), never set back to
//     1 while the zoom stays 2. So when H1 stops clearing (gate closed, lever stock / observe) after it zeroed a set word, the word is set
//     back to 1 (SetLockOnActive's own store) when the last request was on: a weapon -> lasso swap with aim held, Dead Eye with lever
//     deadeye 0 or a lever change keep the stock lock-on; the next PreUpdate tail runs LockOnTarget.
//  H2 camGlueCMXGame::UpdateReticleParams (JUMP_SLOT 0x2e51aa0): while the gate is open (and H1 saw the player's reticle within 100 ms)
//     the soft-lock influence (+0x350 / +0x358) is 0.0, stick bumped (+0x348) 0, lock state / target / candidate (+0x328 / +0x338 / +0x340)
//     0 after the call.
// The VR shot's aim comes from the barrel vector (VrWeapon), the reticle's acquisition point +0x3810 is untouched.
// Gate (holsters atomics, gateOpen): a firearm or a throwable in a VR hand, or the holsters' own force aim; never for the lasso (its catch
// uses the lock), a duel (native lock and multi-lock; the default VR duel leaves the holsters' duel flag off, where UpdateZoomState keeps
// zoom 0 while CheckIsDueling, 0x269bba0 -> 0x269bcd4, so no hard lock forms), a melee weapon; Dead Eye only with lever deadeye 1 (its
// ray painting uses the multi-lock fields, not these; the hard-lock auto mark of UpdateTargetData, AddMultiLockTarget from +0x577c, is
// lost). The VR slot decides before the engine weapon class (heldKind lags a draw or a swap: taking throwing knives right after the
// knife still reads melee for a few frames).
// Levers: debug.rdrvr.vr.noautoaim 1 = suppress (default), 2 = observe (counted, nothing written), 0 = stock (not installed when 0 at start;
// re-read every 2 s); debug.rdrvr.vr.noautoaim.deadeye 1 = suppress in Dead Eye too (default). Any print or GOT word mismatch: nothing
// hooked. Report vr.noAutoAim.
#include <algorithm>
#include <array>
#include <cerrno>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include "engine/WeaponTypeReader.h"

namespace rdrvr::vr::noautoaim {

// ---- engine identities ----
struct Print {std::uintptr_t offset;std::uint32_t size;std::uint64_t fnv;};
inline constexpr std::uintptr_t preUpdateOffset=0x26c12ac,preUpdateGotOffset=0x2e52f38,paramsOffset=0x263329c,paramsGotOffset=0x2e51aa0;
inline constexpr std::uintptr_t paramsReturnPre=0x262f31c,paramsReturnCammach=0x262fb70; // the two calls' return addresses
inline constexpr std::uintptr_t lockOnOffset=0x26ca704,lockOnGotOffset=0x2e52f78;
// PreUpdate (head, the tail that locks), PreCameraUpdate (the only caller), UpdateZoomState (the zoom and lock-on edge), SetLockOnActive,
// LockOnTarget (head, state 1 -> 2), SetHardLockTarget, OnCameraNotify (the orient flag), UpdateTurns (its read), UpdateReticleParams
// (head, the soft-lock and lock writes, the stick-bump write), GetParameterSet, both PLT stubs, both call sites.
inline constexpr Print prints[]{{0x26c12ac,0x40,0x3d324c01f18c4cd8ull},{0x26c1848,0x3c,0x0383395c16a047a4ull},{0x269ba3c,0x64,0xecc392788b1ea3f6ull},
    {0x269c03c,0x88,0xd7cf81c33377168eull},{0x269bcd4,0x30,0x568e61925d5b3606ull},{0x26ca704,0x18,0xead8bfcbc5c25399ull},
    {0x26c1ffc,0x74,0xcca31d04c857cb6aull},{0x26c20fc,0x18,0x68e23147e522d267ull},{0x26c1ee8,0x114,0x7c4236be484ced14ull},
    {0x26c0e88,0x44,0x3550d7c86002e682ull},{0x1e77738,0x18,0x98820e4560bd9d9bull},{0x263329c,0x74,0xd38f21ee6aa071e7ull},
    {0x2633310,0xd0,0xd2f09823396f2e28ull},{0x2633440,0x50,0x0e9730251ddf0984ull},{0x26335fc,0x24,0x531546e51c6eef25ull},
    {0x25c1c54,0x8,0x72927e959b42cf32ull},{0x2c951b0,0x10,0x61ded2f83dcbdd1bull},{0x2c92880,0x10,0xf081b8e539636a5full},
    {0x262f314,0x8,0xf4c71f0bce0aa1e5ull},{0x262fb68,0x8,0x4f1b07996058553cull},{0x2c95230,0x10,0xac59a4fda1923be6ull}}; // + SetLockOnActive's stub
namespace field {
inline constexpr std::size_t lockTarget=0x577c,candidate=0x5780,orient=0x57f0,lockState=0x5818; // tgtReticleComponent
inline constexpr std::size_t glueCamera=0x620,cameraParams=0x30,paramsTable=0x8; // camGlueCMXGame -> camCMXCamera -> parameter set
inline constexpr std::size_t slotLockState=0x328,slotLockTarget=0x338,slotCandidate=0x340,slotBumped=0x348,slotSoftX=0x350,slotSoftY=0x358;
inline constexpr std::size_t glueCombatMode=0x8f8,glueCombatPad=0x8fc,glueCombatKbm=0x900,glueCombatTouch=0x904;
}

// ---- the gate (pure, host-tested) ----
// The holsters' state as atomics: a VR hand holds an item (holdHand >= 0), its engine::WeaponClass (heldKind, -1 unknown), the slot
// (currentSlot: 2 thrown, 3 lasso), a firearm the VR pipeline attaches, the holsters' own force aim, a duel, Dead Eye running.
struct GateIn {bool holding{};int kind{-1},slot{-1};bool firearm{},forceOurs{},duel{},deadEye{},deadEyeLever{true};};
enum Gate : std::uint8_t {GateOpen=0,GateNoWeapon,GateLasso,GateDuel,GateMelee,GateDeadEye,GateCount};
inline constexpr std::array<const char*,GateCount> gateNames{"open","noWeapon","lasso","duel","melee","deadEye"};
// engine::WeaponClass (engine/WeaponTypeReader.h) and the holsters' Slot (VrHolsters.h; tied by static_asserts where both are included)
inline constexpr int kindMelee=10,kindLasso=11,slotMelee=0,slotThrown=2,slotLasso=3;
// engine::WeaponClass 1..9: pistols, rifles, repeater, sniper, shotgun, thrown, thrown exploding, bow
inline constexpr bool aimKind(int k) noexcept {return k>=1&&k<=9;}
static_assert(kindMelee==int(::rdrvr::engine::WeaponClass::Melee)&&kindLasso==int(::rdrvr::engine::WeaponClass::Lasso)&&
              int(::rdrvr::engine::WeaponClass::Pistol)==1&&int(::rdrvr::engine::WeaponClass::Bow)==9);
// the holsters' slots of an aimed weapon: sidearm 1, thrown 2, sniper 4, long arm 5, shotgun 6
inline constexpr bool aimSlot(int s) noexcept {return s==1||s==2||(s>=4&&s<=6);}
// The VR slot decides when it is known (slot >= 0); the engine class only stands in for an unknown slot.
inline constexpr Gate gateOpen(const GateIn& g) noexcept {
    if(g.duel)return GateDuel;
    const bool slotKnown=g.slot>=0;
    if(slotKnown?g.slot==slotLasso:g.kind==kindLasso)return GateLasso;
    if(g.holding&&(slotKnown?g.slot==slotMelee:g.kind==kindMelee))return GateMelee;
    if(g.deadEye&&!g.deadEyeLever)return GateDeadEye;
    const bool weapon=g.holding&&(g.firearm||aimSlot(g.slot)||aimKind(g.kind));
    return weapon||g.forceOurs?GateOpen:GateNoWeapon;}
// H3's restore: after H1 zeroed a set lock word, the first call that does not clear writes 1 back when the stock request is on and the
// word still reads 0 (anything else: the stock state stands).
inline constexpr bool restoreLockOn(bool zeroed,bool requestKnown,bool requestOn,std::uint32_t wordNow) noexcept {
    return zeroed&&requestKnown&&requestOn&&wordNow==0u;}
// The lever value: 0 stock, 1 suppress, 2 observe (anything else: suppress).
enum Mode : std::uint8_t {ModeStock=0,ModeSuppress=1,ModeObserve=2};
inline constexpr Mode modeOf(long v) noexcept {return v==0?ModeStock:v==2?ModeObserve:ModeSuppress;}
// The reticle's lock fields as found after the original PreUpdate, and what H1 writes (suppress) or would write (observe).
struct LockFields {std::uint32_t state{},target{},candidate{};std::uint8_t orient{};};
inline constexpr bool anyLock(const LockFields& f) noexcept {return f.state||f.target||f.candidate||f.orient;}

// ---- the world yaw (published by VrCamera every PostCameraUpdate; the edge ring's turn measure) ----
inline std::atomic<float> psiNow{0.f};inline std::atomic<bool> psiValid{};inline std::atomic<std::uint64_t> psiNs{};
inline void publishPsi(float psi,bool valid,std::uint64_t now) noexcept {
    if(valid&&std::isfinite(psi))psiNow.store(psi,std::memory_order_relaxed);psiValid.store(valid&&std::isfinite(psi),std::memory_order_relaxed);
    psiNs.store(now,std::memory_order_relaxed);}
inline float wrapDeg(float d) noexcept {if(!std::isfinite(d))return d;while(d>180.f)d-=360.f;while(d<-180.f)d+=360.f;return d;}

// ---- counters (main thread writes, any thread reads) ----
inline std::atomic<const char*> installState{"NOT ATTEMPTED"};inline std::atomic<bool> installed{};
inline std::atomic<std::uint8_t> mode{ModeSuppress};inline std::atomic<bool> deadEyeLever{true};
struct Stats {
    // H1: calls, gate open, per gate verdict, lock fields found set (state, target, candidate, orient) and cleared, calls in Dead Eye
    std::atomic<std::uint64_t> reticleCalls{},reticleOpen{},foundState{},foundTarget{},foundCandidate{},foundOrient{},cleared{},inDeadEye{};
    // H3: SetLockOnActive calls (true / false), lock words H1 zeroed, restores written, restores not wanted (request off, word set, other
    // reticle)
    std::atomic<std::uint64_t> lockOnOn{},lockOnOff{},stateZeroed{},restored{},restoreNot{};
    std::array<std::atomic<std::uint64_t>,GateCount> gate{};
    // H2: calls, gate open, other callers, no fresh reticle, unreadable slots, soft influence zeroed (non-zero found), stick bump zeroed,
    // lock params zeroed (non-zero found); the largest soft influence found (|x|, |y| x 1000)
    std::atomic<std::uint64_t> cameraCalls{},cameraOpen{},cameraOther{},cameraNoReticle{},cameraSlotBad{},softZeroed{},bumpZeroed{},lockParamZeroed{};
    std::atomic<std::uint64_t> softMaxMilli{};
    // the combat mode words of camGlueCMXGame (+0x8f8 current, +0x8fc pad, +0x900 keyboard / mouse, +0x904 touch; -1 unread)
    std::atomic<int> combat{-1},combatPad{-1},combatKbm{-1},combatTouch{-1};
};
inline Stats stats{};
// The edge ring (8): one entry per gate opening (a VR weapon taken) and per stock lock found forming while open: ms (steady clock), why
// (0 gate opened, 1 lock formed), the weapon kind and slot, the lock fields found, the largest soft influence of the next 300 ms (x 1000),
// the world yaw then and its change after 300 / 1000 ms (deg), suppressed (mode 1).
struct Edge {bool set{};std::uint64_t ns{};std::uint8_t why{};int kind{-1},slot{-1};LockFields found{};std::uint32_t softMilli{};float psi{},d300{NAN},d1000{NAN};
    bool psiOk{},suppressed{};};
inline std::mutex edgeMutex;inline std::array<Edge,8> edges{};inline unsigned edgeNext{};

inline std::string report() {
    const auto u=[](const std::atomic<std::uint64_t>& a){return (unsigned long long)a.load(std::memory_order_relaxed);};
    const auto& S=stats;std::string o;char b[900];
    const unsigned m=mode.load(std::memory_order_relaxed);
    std::snprintf(b,sizeof b,"{\"hook\":\"%s\",\"mode\":\"%s\",\"deadEye\":%s,\"reticle\":{\"calls\":%llu,\"gateOpen\":%llu,\"found\":{\"state\":%llu,\"target\":%llu,"
        "\"candidate\":%llu,\"orient\":%llu},\"cleared\":%llu,\"inDeadEye\":%llu,",installState.load(),m==ModeStock?"stock":m==ModeObserve?"observe":"suppress",
        deadEyeLever.load()?"true":"false",u(S.reticleCalls),u(S.reticleOpen),u(S.foundState),u(S.foundTarget),u(S.foundCandidate),u(S.foundOrient),u(S.cleared),u(S.inDeadEye));o+=b;
    std::snprintf(b,sizeof b,"\"lockOn\":{\"on\":%llu,\"off\":%llu,\"stateZeroed\":%llu,\"restored\":%llu,\"restoreNot\":%llu},\"gate\":{",u(S.lockOnOn),u(S.lockOnOff),
        u(S.stateZeroed),u(S.restored),u(S.restoreNot));o+=b;
    for(unsigned i=0;i<GateCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",gateNames[i],u(S.gate[i]));o+=b;}
    std::snprintf(b,sizeof b,"}},\"camera\":{\"calls\":%llu,\"gateOpen\":%llu,\"otherCaller\":%llu,\"noReticle\":%llu,\"slotBad\":%llu,\"softZeroed\":%llu,"
        "\"softMax\":%.3f,\"bumpZeroed\":%llu,\"lockParamZeroed\":%llu},\"combatMode\":{\"current\":%d,\"pad\":%d,\"kbm\":%d,\"touch\":%d},\"edges\":[",
        u(S.cameraCalls),u(S.cameraOpen),u(S.cameraOther),u(S.cameraNoReticle),u(S.cameraSlotBad),u(S.softZeroed),double(S.softMaxMilli.load())/1000.0,
        u(S.bumpZeroed),u(S.lockParamZeroed),S.combat.load(),S.combatPad.load(),S.combatKbm.load(),S.combatTouch.load());o+=b;
    std::array<Edge,8> e{};unsigned nx{};{std::lock_guard lock(edgeMutex);e=edges;nx=edgeNext;}
    const auto now=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    bool first=true;
    for(unsigned i=0;i<e.size();++i){const auto& x=e[(nx+i)%e.size()];if(!x.set)continue;
        std::snprintf(b,sizeof b,"%s{\"ageMs\":%.0f,\"why\":\"%s\",\"kind\":%d,\"slot\":%d,\"lockState\":%u,\"lockTarget\":%u,\"candidate\":%u,\"orient\":%u,\"softMax\":%.3f,"
            "\"psiOk\":%s,\"psiDeg\":%.1f,\"psiDelta300\":%.1f,\"psiDelta1000\":%.1f,\"suppressed\":%s}",first?"":",",now>=x.ns?double(now-x.ns)/1e6:-1.0,
            x.why?"lockFormed":"weaponTaken",x.kind,x.slot,x.found.state,x.found.target,x.found.candidate,unsigned(x.found.orient),double(x.softMilli)/1000.0,
            x.psiOk?"true":"false",double(x.psi),std::isfinite(x.d300)?double(x.d300):-999.0,std::isfinite(x.d1000)?double(x.d1000):-999.0,x.suppressed?"true":"false");o+=b;first=false;}
    o+="]}";return o;
}

#if defined(__ANDROID__)
// ======================================= engine side (Android only) =======================================
} // namespace rdrvr::vr::noautoaim
#include <sys/system_properties.h>
namespace rdrvr::vr::noautoaim {
using ReadFn=bool(*)(void*,std::uintptr_t,void*,std::size_t);
using GateFn=GateIn(*)() noexcept;
using PreUpdateFn=void(*)(void*,bool);
using ParamsFn=void(*)(void*);
using LockOnFn=void(*)(void*,bool);
namespace detail {
inline ReadFn read{};inline std::uintptr_t base{};inline GateFn gate{};inline PreUpdateFn originalPre{};inline ParamsFn originalParams{};
inline LockOnFn originalLockOn{};
// H3's record of the stock lock-on request (the reticle it went to, its value) and H1's mark that it zeroed a set lock word
inline std::uintptr_t lockOnReticle{};inline bool lockOnKnown{},lockOnWanted{},stateZeroed{};
inline std::uintptr_t lastReticle{};inline std::uint64_t lastReticleNs{},leverNs{};inline bool wasOpen{},hadLock{};inline std::uint32_t softMilliWindow{};
template<class T> inline bool get(std::uintptr_t p,std::size_t o,T& out) noexcept {
    return read&&p&&p<=UINTPTR_MAX-o&&p+o<=UINTPTR_MAX-sizeof(T)&&read(nullptr,p+o,&out,sizeof(T));}
inline std::uint64_t now() noexcept {
    const auto n=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();return n>0?std::uint64_t(n):1;}
inline long property(const char* name,long fallback) noexcept {
    char v[PROP_VALUE_MAX]{};if(__system_property_get(name,v)<=0)return fallback;char* end{};errno=0;const long x=std::strtol(v,&end,10);
    return errno||end==v?fallback:x;}
// The levers, every 2 s (main thread).
inline void levers(std::uint64_t t) noexcept {
    if(leverNs&&t>=leverNs&&t-leverNs<2'000'000'000ull)return;leverNs=t;
    mode.store(modeOf(property("debug.rdrvr.vr.noautoaim",1)),std::memory_order_relaxed);
    deadEyeLever.store(property("debug.rdrvr.vr.noautoaim.deadeye",1)!=0,std::memory_order_relaxed);}
inline void noteEdge(std::uint8_t why,const GateIn& g,const LockFields& f,bool suppressed,std::uint64_t t) noexcept {
    Edge e{};e.set=true;e.ns=t;e.why=why;e.kind=g.kind;e.slot=g.slot;e.found=f;e.suppressed=suppressed;
    e.psiOk=psiValid.load(std::memory_order_relaxed);e.psi=psiNow.load(std::memory_order_relaxed)*57.2957795f;softMilliWindow=0;
    try{std::lock_guard lock(edgeMutex);edges[edgeNext]=e;edgeNext=(edgeNext+1u)%unsigned(edges.size());}catch(...){}}
// The open edges' turn after 300 / 1000 ms and the soft influence of their first 300 ms.
inline void tickEdges(std::uint64_t t) noexcept {
    const bool ok=psiValid.load(std::memory_order_relaxed);const float psi=psiNow.load(std::memory_order_relaxed)*57.2957795f;
    try{std::lock_guard lock(edgeMutex);
        for(auto& e:edges){if(!e.set||t<e.ns)continue;const auto age=t-e.ns;
            if(age<300'000'000ull)e.softMilli=std::max(e.softMilli,softMilliWindow);
            if(!e.psiOk||!ok)continue;
            if(std::isnan(e.d300)&&age>=300'000'000ull&&age<600'000'000ull)e.d300=wrapDeg(psi-e.psi);
            if(std::isnan(e.d1000)&&age>=1'000'000'000ull&&age<1'500'000'000ull)e.d1000=wrapDeg(psi-e.psi);}}catch(...){}}
} // namespace detail
// H3: the original first (its store and, when on, its LockOnTarget tail call); then the request recorded.
__attribute__((noinline)) inline void lockOnHook(void* self,bool on) noexcept {
    detail::originalLockOn(self,on);
    detail::lockOnReticle=reinterpret_cast<std::uintptr_t>(self);detail::lockOnKnown=true;detail::lockOnWanted=on;
    (on?stats.lockOnOn:stats.lockOnOff).fetch_add(1,std::memory_order_relaxed);}
// The first H1 call that does not clear after H1 zeroed a set lock word: the stock request written back (restoreLockOn).
inline void restoreLockWord(void* self) noexcept {
    const auto s=reinterpret_cast<std::uintptr_t>(self);detail::stateZeroed=false;
    std::uint32_t w=1;const bool read=detail::get(s,field::lockState,w);
    if(read&&restoreLockOn(true,detail::lockOnKnown&&detail::lockOnReticle==s,detail::lockOnWanted,w)){
        const std::uint32_t one=1;std::memcpy(static_cast<unsigned char*>(self)+field::lockState,&one,4);stats.restored.fetch_add(1,std::memory_order_relaxed);}
    else stats.restoreNot.fetch_add(1,std::memory_order_relaxed);}
// H1: the original first; then, gate open, the lock fields cleared (suppress) or counted (observe).
__attribute__((noinline)) inline void preUpdateHook(void* self,bool arg) noexcept {
    detail::originalPre(self,arg);
    const auto s=reinterpret_cast<std::uintptr_t>(self);if(!s)return;
    auto& S=stats;S.reticleCalls.fetch_add(1,std::memory_order_relaxed);
    const auto t=detail::now();detail::levers(t);detail::lastReticle=s;detail::lastReticleNs=t;
    const Mode m=Mode(mode.load(std::memory_order_relaxed));
    const GateIn g=detail::gate?detail::gate():GateIn{};
    GateIn gl=g;gl.deadEyeLever=deadEyeLever.load(std::memory_order_relaxed);
    const Gate v=m==ModeStock?GateNoWeapon:gateOpen(gl);S.gate[v<GateCount?v:GateNoWeapon].fetch_add(1,std::memory_order_relaxed);
    const bool open=v==GateOpen;
    detail::tickEdges(t);
    if(detail::stateZeroed&&!(open&&m==ModeSuppress))restoreLockWord(self);
    if(!open){detail::wasOpen=false;detail::hadLock=false;return;}
    S.reticleOpen.fetch_add(1,std::memory_order_relaxed);if(g.deadEye)S.inDeadEye.fetch_add(1,std::memory_order_relaxed);
    LockFields f{};
    if(!detail::get(s,field::lockState,f.state)||!detail::get(s,field::lockTarget,f.target)||!detail::get(s,field::candidate,f.candidate)||
       !detail::get(s,field::orient,f.orient)){detail::wasOpen=true;return;}
    if(f.state)S.foundState.fetch_add(1,std::memory_order_relaxed);if(f.target)S.foundTarget.fetch_add(1,std::memory_order_relaxed);
    if(f.candidate)S.foundCandidate.fetch_add(1,std::memory_order_relaxed);if(f.orient)S.foundOrient.fetch_add(1,std::memory_order_relaxed);
    const bool suppress=m==ModeSuppress;
    const bool lock=f.state||f.target;
    if(!detail::wasOpen)detail::noteEdge(0,g,f,suppress,t);
    else if(lock&&!detail::hadLock)detail::noteEdge(1,g,f,suppress,t); // a lock formed (suppress: cleared the same frame)
    detail::wasOpen=true;detail::hadLock=lock&&!suppress;
    if(!suppress||!anyLock(f))return;
    // the object is live (its own update just ran with it): plain stores
    auto* p=static_cast<unsigned char*>(self);const std::uint32_t z=0;const std::uint8_t zb=0;
    std::memcpy(p+field::lockState,&z,4);std::memcpy(p+field::lockTarget,&z,4);std::memcpy(p+field::candidate,&z,4);std::memcpy(p+field::orient,&zb,1);
    if(f.state){detail::stateZeroed=true;S.stateZeroed.fetch_add(1,std::memory_order_relaxed);}
    S.cleared.fetch_add(1,std::memory_order_relaxed);
}
// H2: the original first; then, gate open and the player's reticle seen by H1 within 100 ms, the slots the original just wrote are read
// (guarded, aligned) and zeroed (suppress).
__attribute__((noinline)) inline void paramsHook(void* glue) noexcept {
    detail::originalParams(glue);
    auto& S=stats;S.cameraCalls.fetch_add(1,std::memory_order_relaxed);
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    if(caller!=detail::base+paramsReturnPre&&caller!=detail::base+paramsReturnCammach){S.cameraOther.fetch_add(1,std::memory_order_relaxed);return;}
    const auto gp=reinterpret_cast<std::uintptr_t>(glue);
    {std::int32_t c{-1},cp{-1},ck{-1},ct{-1};
     if(detail::get(gp,field::glueCombatMode,c))S.combat.store(c,std::memory_order_relaxed);if(detail::get(gp,field::glueCombatPad,cp))S.combatPad.store(cp,std::memory_order_relaxed);
     if(detail::get(gp,field::glueCombatKbm,ck))S.combatKbm.store(ck,std::memory_order_relaxed);if(detail::get(gp,field::glueCombatTouch,ct))S.combatTouch.store(ct,std::memory_order_relaxed);}
    const Mode m=Mode(mode.load(std::memory_order_relaxed));if(m==ModeStock)return;
    GateIn g=detail::gate?detail::gate():GateIn{};g.deadEyeLever=deadEyeLever.load(std::memory_order_relaxed);
    if(gateOpen(g)!=GateOpen)return;
    S.cameraOpen.fetch_add(1,std::memory_order_relaxed);
    const auto t=detail::now();
    if(!detail::lastReticle||t<detail::lastReticleNs||t-detail::lastReticleNs>100'000'000ull){S.cameraNoReticle.fetch_add(1,std::memory_order_relaxed);return;}
    std::uintptr_t cam{},set{},table{};
    if(!detail::get(gp,field::glueCamera,cam)||!cam||!detail::get(cam,field::cameraParams,set)||!set||!detail::get(set,field::paramsTable,table)||!table||(table&7u)){
        S.cameraSlotBad.fetch_add(1,std::memory_order_relaxed);return;}
    std::uintptr_t sx{},sy{},bump{},ls{},lt{},lc{};
    if(!detail::get(table,field::slotSoftX,sx)||!detail::get(table,field::slotSoftY,sy)||!detail::get(table,field::slotBumped,bump)||
       !detail::get(table,field::slotLockState,ls)||!detail::get(table,field::slotLockTarget,lt)||!detail::get(table,field::slotCandidate,lc)||
       !sx||!sy||!bump||!ls||!lt||!lc||((sx|sy|ls|lt|lc)&3u)){S.cameraSlotBad.fetch_add(1,std::memory_order_relaxed);return;}
    float fx{},fy{};std::uint8_t bb{};std::uint32_t a{},b{},c{};
    if(!detail::get(sx,0,fx)||!detail::get(sy,0,fy)||!detail::get(bump,0,bb)||!detail::get(ls,0,a)||!detail::get(lt,0,b)||!detail::get(lc,0,c)){
        S.cameraSlotBad.fetch_add(1,std::memory_order_relaxed);return;}
    {const float mx=std::max(std::isfinite(fx)?std::fabs(fx):0.f,std::isfinite(fy)?std::fabs(fy):0.f);const auto milli=std::uint64_t(std::min(mx,1e6f)*1000.f);
     auto cur=S.softMaxMilli.load(std::memory_order_relaxed);while(milli>cur&&!S.softMaxMilli.compare_exchange_weak(cur,milli,std::memory_order_relaxed)){}
     detail::softMilliWindow=std::max(detail::softMilliWindow,std::uint32_t(std::min<std::uint64_t>(milli,0xffffffffu)));}
    if(m!=ModeSuppress)return;
    // the original just stored to these slots: plain stores
    const float zf=0.f;const std::uint32_t zu=0;const std::uint8_t zb=0;
    if(fx!=0.f||fy!=0.f){std::memcpy(reinterpret_cast<void*>(sx),&zf,4);std::memcpy(reinterpret_cast<void*>(sy),&zf,4);S.softZeroed.fetch_add(1,std::memory_order_relaxed);}
    if(bb){std::memcpy(reinterpret_cast<void*>(bump),&zb,1);S.bumpZeroed.fetch_add(1,std::memory_order_relaxed);}
    if(a||b||c){std::memcpy(reinterpret_cast<void*>(ls),&zu,4);std::memcpy(reinterpret_cast<void*>(lt),&zu,4);std::memcpy(reinterpret_cast<void*>(lc),&zu,4);
        S.lockParamZeroed.fetch_add(1,std::memory_order_relaxed);}
}
// Startup: every print and the three GOT words checked, then H3, H1, H2 (H2 alone may fail: RETICLE ONLY). read = guarded read, got(slot,target) = word check,
// replace(address,expected,replacement) = TraceEntry replacePointer.
template<class Got,class Replace>
inline bool install(ReadFn read,std::uintptr_t base,GateFn gate,Got&& got,Replace&& replace) noexcept {
    const long lever=detail::property("debug.rdrvr.vr.noautoaim",1);mode.store(modeOf(lever));
    deadEyeLever.store(detail::property("debug.rdrvr.vr.noautoaim.deadeye",1)!=0);
    if(lever==0){installState.store("KILLED");return false;}
    if(!read||!base||!gate){installState.store("NO ENGINE API");return false;}
    const auto hash=[&](std::uintptr_t address,std::uint32_t size){
        std::uint64_t h=0xcbf29ce484222325ull;unsigned char chunk[256];
        for(std::uint32_t done=0;done<size;){const std::uint32_t n=std::min<std::uint32_t>(size-done,sizeof chunk);
            if(!read(nullptr,address+done,chunk,n))return std::uint64_t{};
            for(std::uint32_t i=0;i<n;++i)h=(h^chunk[i])*0x100000001b3ull;
            done+=n;}
        return h;};
    for(const auto& p:prints)if(hash(base+p.offset,p.size)!=p.fnv){installState.store("ENGINE MISMATCH");return false;}
    if(!got(preUpdateGotOffset,preUpdateOffset)||!got(paramsGotOffset,paramsOffset)||!got(lockOnGotOffset,lockOnOffset)){installState.store("WORD MISMATCH");return false;}
    detail::read=read;detail::base=base;detail::gate=gate;
    detail::originalPre=reinterpret_cast<PreUpdateFn>(base+preUpdateOffset);detail::originalParams=reinterpret_cast<ParamsFn>(base+paramsOffset);
    detail::originalLockOn=reinterpret_cast<LockOnFn>(base+lockOnOffset);
    // H3 before H1: H1 zeroes the lock word only with the request recorded (no H3: nothing hooked)
    if(!replace(base+lockOnGotOffset,base+lockOnOffset,reinterpret_cast<std::uintptr_t>(&lockOnHook))){installState.store("HOOK FAILED");return false;}
    if(!replace(base+preUpdateGotOffset,base+preUpdateOffset,reinterpret_cast<std::uintptr_t>(&preUpdateHook))){installState.store("HOOK FAILED");return false;}
    if(!replace(base+paramsGotOffset,base+paramsOffset,reinterpret_cast<std::uintptr_t>(&paramsHook))){
        // H1 alone still clears the hard lock; the soft lock stays stock
        installState.store("RETICLE ONLY");installed.store(true,std::memory_order_release);return true;}
    installState.store("INSTALLED");installed.store(true,std::memory_order_release);return true;
}
#endif // __ANDROID__

} // namespace rdrvr::vr::noautoaim
