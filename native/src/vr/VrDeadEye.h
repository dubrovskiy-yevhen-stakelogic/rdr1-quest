#pragma once
// DEAD EYE in VR: slow motion with the usual filter, normal aim, the Dead Eye
// time cut in half, and the game still counts it (missions depend on it).
//
// Engine facts (librdr.so 1.58 arm64, vaddr, from llvm-objdump):
//  - The game starts Dead Eye itself: sagPlayer::UpdateTargetingState 0x269e330 sets the WeaponSpecialRequest to 1 on a new press of player
//    input 29 (the DEADEYE action = d11 pad 19 = R3, VrControls.h binding decode) or while player+0x39a (forced); UpdateWeaponSpecialMove
//    0x269a344 (request 1) checks IsDeadEyeAvailable 0x269ca88 + the cooldown player+0x3e0 <= 0 and calls BeginDeadEyeCommon 0x269ce00
//    (time scale, post FX, audio mix, stats - everything the missions count). sagPlayerStates (player+0x410) +0x1c = the Dead Eye level,
//    +0x20 = running (the WeaponContextReader / holsters / cheats read).
//  - While it runs, UpdateWeaponSpecialMove cancels it (request 2 -> CancelDeadeye 0x269cbf0) when the player stops aiming: player+0x1c8
//    (targeting) clear, or player+0x3bc bit1 (sagPlayerHandle::SetForcePlayerAimMode 0x26a951c = the mod's FORCE AIM) clear with a weapon
//    state set (0x269a660..0x269a68c).
//  - Meter = player+0x3c0 points (max +0x3c4); GetDeadEyeTimeRemaining 0x269d6c8 = points / cost-per-second (tune data): the duration IS the
//    points. sagPlayer::SetDeadEyePoints(points,false) 0x2696bd0 stores any value <= the current one (clamped >= 0), unless infinite.
//
// Why the stock Dead Eye does not work in VR:
//  1. no way to press it: on foot R3 alone is CROUCH, the stock Dead Eye needs LT + R3 and in VR LT is the free hand's trigger (the
//     holsters block it to the game while a weapon is held) - nobody presses that chord;
//  2. even when pressed, a dropped force aim ends it the next frame: the holsters' context gate reads "DEAD EYE" -> weaponKeep()'s usable = false ->
//     the mod's FORCE AIM bit drops (VrWeapon gates: force = !deadEye -> heldFirearm false too) -> the engine sees "not aiming" and
//     cancels Dead Eye (fact above).
// What the mod does (this header = the light shared state; the engine work lives in VrCheats.h tick, the mapping in VrControls.h):
//  - R3 with a firearm in a VR hand = the game's own DEADEYE press (pad R3); R3 again while it runs = the stock "end it"; LT + R3 stays;
//    R3 without a gun = crouch. Game input -> the game's own Dead Eye: stats / missions / tutorials count it.
//  - the holsters KEEP their force-aim bit while Dead Eye runs (never newly set, never dropped by the Dead Eye gate);
//  - the VR gun keeps aiming in Dead Eye (VrWeapon gates: aim; the engine's tracked / painted shots stay native) - real-time hand aim,
//    the world in slow motion;
//  - DEAD EYE TIME HALF (default): the VR aim is not slowed, so the meter drains 2x (every drop of player+0x3c0 while it runs is
//    applied once more through SetDeadEyePoints); FULL = the stock drain;
//  - DEAD EYE FILTER ON: the game's sepia post FX never reaches the headset (both eyes are copied out before post processing,
//    PostFxSkip.h), so XrHarness tints the projection layer (XR_KHR_composition_layer_color_scale_bias, ~0 GPU cost) with a warm
//    Dead Eye grade while it runs (fade in / out).
//
// DEAD EYE MODE: by the stock rule Dead Eye ends after the very first shot, hit or miss.
// That is the game's own level 2/3 rule, read from sagPlayer::UpdateWeaponSpecialMove 0x269a344:
//  - the engine level (sagPlayerStates+0x1c) is re-decided EVERY aiming frame at 0x269a7fc..0x269a81c: 2 when player+0x3bd
//    (sagPlayerHandle::SetDeadEyeMultiLockEnable 0x26a9700 = the story's Dead Eye upgrade; ctor 0x2696ab8 / ResetData 0x2696cd4 store 1,
//    nothing saves it) or the duel bit player+0x3be bit2 is set, else 1;
//  - level 2, eye state 2 (+0x20, marking): a fire request (3) or an empty meter -> state 3 (0x269ae5c / 0x269b13c) = shoot the tracked
//    marks (GetNextTrackedLockTarget / aniAim::Fire / MarkNextTrackedLockTargetUsed), and with no mark left 0x269af18 winds the time warp
//    back and ends it (EndDeadEyeCommon) - one shot, zero marks = over at once;
//  - level 1, eye state 2 (0x269aba0): drains the meter and ends only on an empty meter, a new press (R3 again), a cancel request, the
//    time warp gone, or a fire with player+0x3be bit0 (no weapon item; cleared every aiming frame with one, 0x269a6d8) - so every shot is
//    a normal shot and Dead Eye keeps running. Same BeginDeadEyeCommon / EndDeadEyeCommon / states: the game still counts it.
// SLOW-MO (default) therefore holds player+0x3bd at 0 while a firearm is in a VR hand or Dead Eye runs (and no duel: a duel stays the
// duel's level 2 regardless of the byte), so the engine picks level 1 itself before the press; when the hold ends the saved value
// (the story's 1) goes back, unless something else changed the byte meanwhile. The byte is re-cleared if a script / ResetData sets it
// again; a hold never starts under a level-2 execute (eye state 3). GAME = no write at all. Only other readers: rdrMatrixAudioEntity::PlayWeaponSound 0x1e37c98 (the stock
// level-1 post-Dead-Eye mix switch on a shot) and touch-screen code (GetDeadEyeLevel, unused in VR). The engine side is VrCheats.h.
// Header-only, no engine, no OpenXR. Report "vr":{"deadEye":...}.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace rdrvr::vr::deadeye {
using Counter=std::atomic<std::uint64_t>;
inline void bump(Counter& c,std::uint64_t n=1) noexcept {c.fetch_add(n,std::memory_order_relaxed);}
inline std::uint64_t steadyNs() noexcept {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}

// ---- config (menu -> main / XR thread) ----
inline std::atomic<bool> halfTime{true};   // DEAD EYE TIME HALF
inline std::atomic<bool> tintOn{true};     // DEAD EYE FILTER ON
inline std::atomic<bool> vrAim{true};      // debug.rdrvr.vr.deadeye.aim native -> false (the native Dead Eye target)
inline std::atomic<bool> slowMo{true};     // DEAD EYE MODE SLOW-MO (false = GAME); report only, VrCheats maintainSlowMo stores
                                           // what it acts on (vr.cfg loaded, row not killed, SLOW-MO chosen)
// ---- pad thread (GameXr pad mapper) -> main thread ----
inline std::atomic<bool> gunInHand{false};  // a firearm is in a VR hand or LT is held (= R3 would start Dead Eye): SLOW-MO arms before the press

// ---- state (main thread writes) ----
inline std::atomic<bool> active{false};    // sagPlayerStates+0x20 != 0 (local player, validated sample)
inline std::atomic<int> level{-1};         // sagPlayerStates+0x1c
inline std::atomic<float> points{-1.f},pointsMax{-1.f};
inline std::atomic<std::uint64_t> changeNs{0}; // steadyNs() of the last active flip (tint fade)
inline Counter activations,endings,activeTicks,drainCalls,drainInfinite,sampleFailures;
inline std::atomic<float> drainedExtra{0.f};  // points removed by the HALF drain (total)
// ---- input thread / holsters / weapon (counters only) ----
inline Counter gunPresses,ltPresses,endPresses,forceAimKept,vrAimFrames,tintFrames;
// ---- SLOW-MO (main thread) ----
inline std::atomic<bool> slowMoHeld{false}; // player+0x3bd is held at 0 by the mod
inline std::atomic<int> multiLock{-1};      // last read player+0x3bd (-1 unknown)
inline Counter slowMoHolds,slowMoRestores,slowMoReasserts,slowMoDropped,slowMoDuel,slowMoBadValue,slowMoRunningTicks,slowMoExecuteWaits;

// SLOW-MO hold decision (pure): the byte is held at 0 while SLOW-MO is on and a gun is in a VR hand or Dead Eye runs, never in a duel.
inline bool slowMoWanted(bool mode,bool gun,bool running,bool dueling) noexcept {return mode&&!dueling&&(gun||running);}

// Main thread: one validated sample of the local player (on = running).
inline void observe(bool on,int lvl,float pts,float max) noexcept {
    level.store(lvl,std::memory_order_relaxed);points.store(std::isfinite(pts)?pts:-1.f,std::memory_order_relaxed);
    pointsMax.store(std::isfinite(max)?max:-1.f,std::memory_order_relaxed);
    if(on)bump(activeTicks);
    if(active.exchange(on,std::memory_order_acq_rel)!=on){changeNs.store(steadyNs(),std::memory_order_release);bump(on?activations:endings);}
}
// Main thread: no player / not in game -> Dead Eye is over for the tint and the mapper.
inline void observeNone() noexcept {
    if(active.exchange(false,std::memory_order_acq_rel)){changeNs.store(steadyNs(),std::memory_order_release);bump(endings);}
}

// HALF drain (pure, main thread). prev = the meter after the last tick of this Dead Eye (< 0 = none yet). Returns the value to store with
// SetDeadEyePoints (< 0 = nothing to do): every drop the game made since the last tick is applied once more (= 2x drain); a rise (kill /
// headshot bonus) is never touched.
struct Drain {float prev{-1.f};};
inline float drainTarget(Drain& d,bool on,float cur,bool infinite,bool half) noexcept {
    if(!on||!std::isfinite(cur)){d.prev=-1.f;return -1.f;}
    if(infinite||!half){d.prev=cur;return -1.f;}
    float out=-1.f;
    if(d.prev>=0.f&&cur<d.prev){const float drop=d.prev-cur;out=std::max(0.f,cur-drop);}
    d.prev=out>=0.f?out:cur;
    return out;
}

// XR thread: the filter strength 0..1 (fade in 250 ms, out 400 ms).
inline constexpr float fadeInS=.25f,fadeOutS=.40f;
inline float tintAlpha(std::uint64_t now) noexcept {
    if(!tintOn.load(std::memory_order_relaxed))return 0.f;
    const bool on=active.load(std::memory_order_acquire);const auto t=changeNs.load(std::memory_order_acquire);
    const float dt=t&&now>t?float(double(now-t)*1e-9):1e3f; // no flip yet = settled
    return on?std::clamp(dt/fadeInS,0.f,1.f):1.f-std::clamp(dt/fadeOutS,0.f,1.f);
}
// Warm Dead Eye grade at strength a: scale (1, .80, .52), bias (+.05, +.02, 0) (a sepia-like push; scale/bias cannot desaturate).
inline void tintScaleBias(float a,float scale[3],float bias[3]) noexcept {
    constexpr float s[3]{1.f,.80f,.52f},b[3]{.05f,.02f,0.f};a=std::clamp(a,0.f,1.f);
    for(int i=0;i<3;++i){scale[i]=1.f+(s[i]-1.f)*a;bias[i]=b[i]*a;}
}

inline std::string report() {
    char b[1400];const auto u=[](const Counter& c){return (unsigned long long)c.load(std::memory_order_relaxed);};
    std::snprintf(b,sizeof b,"{\"active\":%s,\"level\":%d,\"points\":%.2f,\"pointsMax\":%.2f,\"halfTime\":%s,\"tint\":%s,\"vrAim\":%s,"
        "\"activations\":%llu,\"endings\":%llu,\"activeTicks\":%llu,\"drainCalls\":%llu,\"drainInfinite\":%llu,"
        "\"drainedExtra\":%.2f,\"sampleFailures\":%llu,\"presses\":{\"gun\":%llu,\"lt\":%llu,\"end\":%llu},\"forceAimKept\":%llu,"
        "\"vrAimFrames\":%llu,\"tintFrames\":%llu,\"mode\":\"%s\",\"slowMo\":{\"held\":%s,\"multiLock\":%d,\"holds\":%llu,"
        "\"restores\":%llu,\"reasserts\":%llu,\"dropped\":%llu,\"duel\":%llu,\"badValue\":%llu,\"runningTicks\":%llu,\"executeWaits\":%llu}}",
        active.load()?"true":"false",level.load(),double(points.load()),double(pointsMax.load()),halfTime.load()?"true":"false",
        tintOn.load()?"true":"false",vrAim.load()?"true":"false",u(activations),u(endings),u(activeTicks),u(drainCalls),u(drainInfinite),
        double(drainedExtra.load()),u(sampleFailures),u(gunPresses),u(ltPresses),u(endPresses),u(forceAimKept),u(vrAimFrames),
        u(tintFrames),slowMo.load()?"SLOW-MO":"GAME",slowMoHeld.load()?"true":"false",multiLock.load(),u(slowMoHolds),u(slowMoRestores),
        u(slowMoReasserts),u(slowMoDropped),u(slowMoDuel),u(slowMoBadValue),u(slowMoRunningTicks),u(slowMoExecuteWaits));
    return b;
}
} // namespace rdrvr::vr::deadeye
