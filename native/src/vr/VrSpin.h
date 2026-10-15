#pragma once
// SPIN: the VR-held revolver hangs on / spins
// around the trigger finger. Open the grip (HOLD) while the index stays on the trigger (OpenXR trigger touch, VrTouch.h; without touch a
// light press 0.06..0.35) -> the gun is neither holstered nor thrown but pivots on the finger (hinge = trigger guard, axis = the finger).
// Hand motion swings / spins it (a wrist flick at the opening, small hand circles); the trigger brakes and never fires (RT block "SPIN");
// squeezing the grip catches it back into the palm (on the next pass when spinning fast). Tilt the gun toward the fingertip (right hand
// LEFT, left hand RIGHT) >= 30 deg for 90 ms -> it slides to the fingertip and a toss stroke within 500 ms launches it through the
// unchanged VrThrow flight (+ its own spin); no toss -> holster. Lifting the index: in the first 200 ms the decision of the opening
// frame (throw / holster), later the toss estimator at the lift (throw if fast, else holster). Hanging quietly 2 s -> holster.
// Release rules (supersede the entry / exit rules below): HOLD release of a revolver / SIDEARM-slot gun
//  - near its holster (inside the own holster zone, grip <= zoneRadius of the socket) -> the holster, both modes;
//  - GUN SPIN ON: anywhere else it ALWAYS hangs on the finger (no finger / flick / speed requirement, no toss on the opening). It never
//    falls by itself (no finger-off / rest / steep / pose-lost exit). Out: TILT off the finger (slides to the tip -> falls through the
//    VrThrow flight: toss = throw, else a drop with the current velocity), squeeze = catch, the own holster zone (quietly 300 ms) ->
//    holster, a refused context 250 ms / menu off / suspension -> holster. No hinge / pose -> it falls as OFF;
//  - GUN SPIN OFF: anywhere else it FALLS (a toss when fast, else a drop at the hand) - never vanishes;
//  - the decision log names every opening: SPIN (FINGER / FLICK_* / OPEN), THROW / DROP (SPIN_OFF / NO_HINGE / NO_POSE), HOLSTER (ZONE / ...).
//
// Threads:
//  - input thread (holsters InputBrain::frame): Brain::step per hand - entry, the hinge Sim replayed over the XR tape (VrThrow tapeRing),
//    tilt / slide, finger, toss, catch blend, exits (the holsters execute the posts). Publishes poseBox[h], fingerMask, aimSuppressedFlag.
//  - pose hooks (any thread, GameXr vrWeaponPose): notePlacement -> hingeBox[h]; evalPose -> the spin C (model space) composed onto the
//    held model; noteSpun -> the spun model for vrThrowStart (the flight starts where the gun is).
//  - main / input thread: publishStatus (menu row note GUN SPIN), report().
// Kill: debug.rdrvr.vr.spin 0 (startup; no touch action, no entry, holsters unchanged). Menu: HANDS / WEAPONS -> GUN SPIN OFF / ON (default ON).
//  - pivot = the weapon's 'trigger' bone (+ trigOff into the guard) moved ONTO the VR index finger (grip + fingerPt along the gun's axes);
//  - physics = rigid body pinned at the finger: gravity + the finger's linear acceleration + light friction (no assists);
//  - entry = touch (either mapping) OR trigger analog > 0.02 anywhere in the 1.5 s before the opening; untracked finger never "lifts";
//  - menu GUN SPIN note shows the last refusal ("MISS: ...") until the next entry.
//  - NO shift: the gun keeps its held pose; the pivot = the trigger bone + trigOff (1.5 cm along the barrel, 1.5 cm down = inside the
//    guard), expressed in the controller frame at the entry (rigid with the controller); axis = the gun's lateral axis at the entry;
//  - entry without the finger signal: a wrist FLICK (|rate about the lateral axis| >= flickOmega) or a hand FLICK (pivot speed >= flickV,
//    mostly perpendicular to the barrel) within flickMs before the opening, or within flickMs after it (PROBE: the gun stays rigid that
//    long, then a plain holster); a toss still throws unless a flick came with a hand slower than throwV;
//  - a fast hand turn in the spin's direction drags the gun up to its rate (flickPush: another flick adds speed); visc .3 / coul 1.0;
//  - the gun never vanishes mid-air: finger-off / tilt / steep / rest (3 s fully still) DROP it through the VrThrow flight (falls, can be
//    picked up, auto-returns); only the zone rule (in the holster) / pose lost / menu off holster it;
//  - report decisions[]: every revolver grip opening and every spin end with its branch (SPIN / THROW / HOLSTER / DROP / CATCH) and why.
// This header must not include VrHolsters.h (VrHolsters.h includes it).
#include "VrThrow.h"
#include "VrHaptics.h"
#include "VrTouch.h"
#include "VrSettings.h"
#include "runtime/ControllerInput.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

namespace rdrvr::vr::spin {
namespace TM=::rdrvr::vr::throwing::math;
using V3=::rdrvr::vr::throwing::V3;using M3=::rdrvr::vr::throwing::M3;using M4=::rdrvr::vr::throwing::M4;using Q=TM::Q;
using ::rdrvr::runtime::ControllerSample;

struct Config {
    // HAND-DRIVEN ONLY (a swing of the hand spins it, never the sim by itself): a rigid body pinned at the index finger - gravity
    // torque + the finger's linear acceleration (pseudo-force -m a, x drive) + light friction on the rate relative to the hand. No gravity
    // relief, no stir, no palm push, no finger drag. Simulated: a wrist snap of 90 deg / 120 ms gives ~1.1 rev,
    // a hard one ~2.
    // gravity .50 (a ~10 rad/s flick loops over the top), visc .30, coul 1.0 (2-4 revolutions from a flick)
    float gravity{.50f},drive{1.4f},visc{.30f},coul{1.0f},brake{25.f},brakev{5.f},b0Touch{.10f},b0Light{.35f},
        maxOmega{24.f},maxDegFrame{40.f},headMin{.65f},accGlitch{150.f},angGlitch{15.f},
        slideDeg{30.f},hystDeg{8.f},biasDeg{10.f},finger{.045f},slideV{.35f},backV{.25f},dropDeg{60.f},gyration{.075f},
        analogOn{.06f},analogMax{.35f},analogOff{.03f},
        entryAnalogMax{.50f},zoneRadius{.26f},
        nearRadius{.25f}; // The gun (guard pivot / CoM / its grip) this close to its own holster socket -> holster at once
        // zoneRadius .26 = the throw socket guard (OFF mode must not drop next to the holster)
        // entry light band .03..0.50; the spin's own-holster zone core (m)
    V3 com{0.f,.018f,-.030f};            // CoM - pivot, model axes, metres (Colt SAA balance)
    V3 pivot{};bool pivotSet{};          // .spin.pivot "x y z": absolute model metres (wins over the trigger bone / the fallback geometry)
    // PIVOT = THE INSIDE OF THE TRIGGER GUARD on the gun as held: the weapon's 'trigger' bone + trigOff (fwd along the barrel, down;
    // m). No shift: the gun keeps its held pose and turns through this point.
    V3 trigOff{.015f,.015f,0.f};
    // FLICK ENTRY (no finger signal needed), the push of a fast hand turn, "fully still" for the rest rule, the drop exits
    float flickOmega{4.f},flickV{1.f},flickPerp{.70f},throwV{3.f},flickPush{40.f},seedGain{1.2f},stillV{.15f},stillW{1.f},stillRel{1.f};
    unsigned flickMs{150};bool restDrop{true},dropOn{true};
    unsigned entryWindowMs{1500};        // Finger seen on the trigger (touch / analog > 0.02) anywhere in this window before the opening
    unsigned seedMs{80},tiltMs{90},unhookMs{500},recentMs{300},fingerOffMs{110},classicDebounceMs{40},lightOffMs{120},classicMs{200},
        restMs{3000},zoneMs{300},steepMs{150},substeps{4},entryBlendMs{80},
        entryLookbackMs{250},entryAnchorMaxMs{800},entryGraceMs{400}; // forgiving entry (see tryEnter)
    bool touch{true},zone{true},haptics{true};
    bool keepAim{true},anyHandTouch{true}; // keep the mod's force aim while spinning (engine auto-holsters off); the other hand's touch bit counts when that hand is free
};
inline Config config{};
inline std::atomic<bool> killed{},started{},menuOn{true};
inline bool active() noexcept {return started.load(std::memory_order_acquire)&&!killed.load(std::memory_order_relaxed)&&menuOn.load(std::memory_order_relaxed);}
inline void setMenu(bool on) noexcept {menuOn.store(on,std::memory_order_relaxed);}
// holsters constants mirrored (VrHolsters.h static_asserts them)
inline constexpr float gripOpen=.30f,gripClose=.65f,releaseTriggerGuard=.35f,triggerRearm=.15f;
inline constexpr std::uint64_t ms(unsigned v) noexcept {return std::uint64_t(v)*1'000'000ull;}

// =====================================================================================================================
// Small math (LOCAL tracking space, +Y up; quaternions x y z w).
namespace sm {
inline constexpr float pi=3.14159265358979f,twoPi=6.28318530717959f;
inline V3 rot(const Q& q,V3 v) noexcept {const V3 u{q[0],q[1],q[2]};const V3 t=TM::mul(TM::cross(u,v),2.f);return TM::add(TM::add(v,TM::mul(t,q[3])),TM::cross(u,t));}
inline V3 rotT(const Q& q,V3 v) noexcept {return rot(Q{-q[0],-q[1],-q[2],q[3]},v);}
inline float wrap(float a) noexcept {a=std::fmod(a+pi,twoPi);if(a<0)a+=twoPi;return a-pi;}
inline double wrapD(double a) noexcept {const double p=3.141592653589793,t=6.283185307179586;a=std::fmod(a+p,t);if(a<0)a+=t;return a-p;}
inline float clampf(float x,float lo,float hi) noexcept {return std::fmin(hi,std::fmax(lo,x));}
// rotation vector (LOCAL) of b * conj(a): the hand rotation from pose a to pose b
inline V3 rotvec(const Q& a,const Q& b) noexcept {
    Q d{b[3]*-a[0]+b[0]*a[3]+b[1]*-a[2]-b[2]*-a[1],b[3]*-a[1]-b[0]*-a[2]+b[1]*a[3]+b[2]*-a[0],
        b[3]*-a[2]+b[0]*-a[1]-b[1]*-a[0]+b[2]*a[3],b[3]*a[3]-b[0]*-a[0]-b[1]*-a[1]-b[2]*-a[2]};
    if(d[3]<0)for(float& x:d)x=-x;
    const float sh=std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
    return sh>1e-9f?TM::mul(V3{d[0],d[1],d[2]},2.f*std::atan2(sh,d[3])/sh):V3{0,0,0};
}
inline V3 rotAxis(V3 k,float angle,V3 v) noexcept { // Rodrigues, unit k
    const float c=std::cos(angle),s=std::sin(angle);const float d=TM::dot(k,v);
    return TM::add(TM::add(TM::mul(v,c),TM::mul(TM::cross(k,v),s)),TM::mul(k,d*(1.f-c)));
}
inline Q qnorm(Q q) noexcept {const float n=std::sqrt(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);if(!(n>0.f)||!std::isfinite(n))return {0,0,0,1};for(float& x:q)x/=n;return q;}
// palm-out axis of the controller grip (toward the index fingertip side): right hand -X_grip, left hand +X_grip
inline V3 palmOutGrip(unsigned hand) noexcept {return hand==1?V3{-1,0,0}:V3{1,0,0};}
// tilt (deg) = roll of the palm-out axis tip-down (the controller's axis: the weapons.cfg calibration roll never enters)
inline float tiltDeg(const Q& q,unsigned hand) noexcept {const V3 a=rot(q,palmOutGrip(hand));return std::asin(clampf(-a[1],-1.f,1.f))*57.2957795f;}
inline bool finiteQ(const Q& q) noexcept {return std::isfinite(q[0])&&std::isfinite(q[1])&&std::isfinite(q[2])&&std::isfinite(q[3]);}
} // namespace sm

// =====================================================================================================================
// Published state.
struct Hinge {                           // pose thread -> input thread, one per hand, refreshed every held frame
    std::uintptr_t weapon{};std::uint32_t generation{};std::int32_t hand{-1};
    V3 pivotG{},axisG{},comG{},centreG{}; // grip frame of the holding controller (metres): pivot, unit axis (fingertip side), CoM - pivot (perp), flight point
    V3 pivotM{},axisM{};                  // model space (model units)
    float lever{},inertia{},upm{1},pad0{}; // |comG| m, lever^2 + gyration^2, model units per metre
    std::uint64_t ns{},referenceSpace{};
    V3 shiftM{};std::uint32_t source{};   // model-space translation (always zero: no shift); PivotSource
    V3 fwdG{};float pad1{};               // The barrel direction in the grip frame (the hand flick's "perpendicular to the barrel")
};
enum PivotSource : std::uint32_t {PivotGeometry=0,PivotTriggerBone=1,PivotProperty=2};
inline constexpr std::array<const char*,3> pivotSourceNames{"GEOMETRY","TRIGGER_BONE","PROPERTY"};
static_assert(sizeof(Hinge)%8==0);
enum State : std::uint32_t {Off=0,Arming=1,OnFinger=2,Unhooked=3,Catching=4,Leaving=5};
inline constexpr std::array<const char*,6> stateNames{"OFF","ARMING","ON_FINGER","UNHOOKED","CATCHING","LEAVING"};
struct PoseState {                       // input thread -> pose hooks, one per hand
    std::uintptr_t weapon{};std::uint32_t generation{},state{};
    float theta{},rel{},slide{},slideV{};std::uint64_t simNs{};          // angle about axisM RELATIVE to the hand (rad, wrapped), its rate, m, m/s
    float blendFrom{},blendDelta{},blendSlope{},blendSlide{};std::uint64_t blendStartNs{},blendNs{}; // CATCHING Hermite
    std::uint64_t entryBlendNs{};                                       // ARMING -> ON_FINGER: entryBlendMs from the held pose to the sim
    V3 pivotM{},axisM{};float upm{1};std::uint32_t rigid{},catching{},pad{};
    V3 shiftM{};float pad2{};             // Guard pivot -> the VR finger (blended like theta)
};
static_assert(sizeof(PoseState)%8==0);
inline std::array<throwing::Box<Hinge>,2> hingeBox{};
inline std::array<throwing::Box<PoseState>,2> poseBox{};
inline std::array<std::atomic<std::uint64_t>,2> poseSeen{};       // pose hook evaluated an owned hand's gun (rigid or spun): the brain's watchdog
// keepMask bit h = hand h is in a KEEP state (ARMING / ON_FINGER / UNHOOKED / CATCHING; not LEAVING, whose
// exit posts the put-away / throw itself). While set the holsters post NO put-away for that hand (HOLD
// release, the intent + retries, the open-hand holster), the executor drops a queued OpPutAway for it, and
// weaponKeep keeps the engine item drawn (SetRDR1HoldMode keep every poll + the mod's force aim kept).
inline std::atomic<std::uint32_t> keepMask{};
inline bool keeps(int hand) noexcept {return hand>=0&&hand<2&&((keepMask.load(std::memory_order_acquire)>>unsigned(hand))&1u);}
// fingerMask / aimSuppressedFlag: declared in VrTouch.h.

// =====================================================================================================================
// Counters (report vr.spin; any thread).
// Exits incl. the DROP exits (the gun falls through the VrThrow flight instead of vanishing) and PLAIN_HOLSTER (a probe without a flick)
enum Exit : unsigned {ExitClassicThrow,ExitClassicHolster,ExitFingerOffThrow,ExitFingerOffHolster,ExitTiltThrow,ExitTiltHolster,ExitTiltSteep,
    ExitZoneHolster,ExitRestHolster,ExitPoseLost,ExitWeaponGone,ExitSuspend,ExitMenuOff,
    ExitFingerOffDrop,ExitTiltDrop,ExitSteepDrop,ExitRestDrop,ExitPlainHolster,
    ExitOpenDrop,ExitOpenThrow,ExitContextHolster,ExitNearHolster,ExitCount}; // The opening falls (GUN SPIN OFF / no hinge) or a context ends the hang
inline constexpr std::array<const char*,ExitCount> exitNames{"CLASSIC_THROW","CLASSIC_HOLSTER","FINGER_OFF_THROW","FINGER_OFF_HOLSTER","TILT_THROW",
    "TILT_HOLSTER","TILT_STEEP","ZONE_HOLSTER","REST_HOLSTER","POSE_LOST","WEAPON_GONE","SUSPEND","MENU_OFF",
    "FINGER_OFF_DROP","TILT_DROP","STEEP_DROP","REST_DROP","PLAIN_HOLSTER","OPEN_DROP","OPEN_THROW","CONTEXT_HOLSTER","NEAR_HOLSTER"};
inline bool dropExit(unsigned e) noexcept {return e==ExitFingerOffDrop||e==ExitTiltDrop||e==ExitSteepDrop||e==ExitRestDrop||e==ExitOpenDrop;}
enum Refused : unsigned {RefZone,RefContext,RefNotRevolver,RefNoHinge,RefToggle,RefCylinder,RefCount};
// Gate: the FIRST failing entry condition, once per grip opening (the grip <= 0.30 run of one hand)
enum Gate : unsigned {GateNotHoldHand,GateNotOwned,GateDeferred,GateInactive,GateFinger,GateToggle,GateNotRevolver,GateNoHinge,GateContext,
    GateCylinder,GateZone,GatePose,GateCount};
inline constexpr std::array<const char*,GateCount> gateNames{"NOT_HOLD_HAND","NOT_OWNED","DEFERRED","INACTIVE","FINGER","TOGGLE","NOT_REVOLVER",
    "NO_HINGE","CONTEXT","CYLINDER","ZONE","POSE"};
enum Source : unsigned {SrcTouchNow,SrcTouchRecent,SrcLightNow,SrcLightRecent,SrcCount};
inline constexpr std::array<const char*,SrcCount> sourceNames{"touchNow","touchRecent","lightNow","lightRecent"};
inline constexpr std::array<const char*,RefCount> refusedNames{"ZONE","CONTEXT","NOT_REVOLVER","NO_HINGE","TOGGLE","CYLINDER"};
using Counter=std::atomic<std::uint64_t>;
struct Stats {
    Counter entriesTouch,entriesAnalog,entriesArmed,entriesTossPending,releasesPlain,armingCancelled,armingFingerOff;
    std::array<Counter,RefCount> refused;std::array<Counter,ExitCount> exits;
    std::array<Counter,GateCount> firstFail;std::array<Counter,SrcCount> source;Counter fingerOtherHand,graceHeld;
    Counter unhookArmed,unhookRehooked,towardHead,regrabDone,regrabOnPass,regrabShortest,regrabFumbled,regrabBrake;
    Counter simSteps,simFallback,simGaps,simPasses,throwRequested,posePlaced,poseRigid,poseRigidFail,hingeRefreshes,hingeRefused;
    Counter rtBlockedSpin,bMasked,transferRefused,hapticEntry,hapticPass,hapticSlide,hapticReady,hapticRehook,hapticSlap;
    // finger mapping that matched at the entry (same hand / the other hand's touch bit), the put-away paths suppressed while kept,
    // the engine keep while kept, sidearms accepted by the slot (class not a revolver kind), WEAPON_GONE forensics
    Counter fingerSame,fingerSwapped,supHoldRelease,supIntent,supOpenHand,supExecutor,keepWhileSpin,aimKeptSpin,sidearmBySlot,
        engineHolsterWhileKept,goneHandChanged,goneGeneration;
    Counter autoHolsterNear; // The hanging / spinning gun brought to its holster -> holstered at once (NEAR_HOLSTER)
};
inline std::atomic<float> lastNearCm{-1.f}; // The gun point's distance to the socket at the last NEAR_HOLSTER (cm)
inline Stats stats{};
inline void bump(Counter& c,std::uint64_t n=1) noexcept {c.fetch_add(n,std::memory_order_relaxed);}
inline std::atomic<float> maxOmegaSeen{},maxSlideCm{},lastThrowSpeed{},lastThrowOwn{};
// the flick entry / push / drop counters and the decision log (every revolver grip opening + every spin end: branch and why)
struct FlickStats {Counter entryFinger,entryFlickAng,entryFlickLin,probes,probeFlick,probeFinger,probeHolster,tossWins,flickOverToss,pushSubsteps,drops,
    openSpin,openThrow,openHolster,openOther,openDrop,poseLostHeld,contextHolsters;};
inline FlickStats flick{};
inline std::atomic<float> lastFlickWa{},lastFlickLin{},lastFlickPerp{},maxFlickWa{};
struct Decision {std::uint64_t ns{};int hand{-1};char branch[10]{};char why[24]{};float wa{},lin{},perp{},trig{},rel{},revs{};};
inline std::mutex decisionMutex;inline std::array<Decision,12> decisions{};inline std::uint64_t decisionCount{};
inline void logDecision(std::uint64_t ns,unsigned hand,const char* branch,const char* why,float wa,float lin,float perp,float trig,float rel=0.f,float revs=0.f) noexcept {
    Decision d{};d.ns=ns;d.hand=int(hand);std::snprintf(d.branch,sizeof d.branch,"%s",branch?branch:"");std::snprintf(d.why,sizeof d.why,"%s",why?why:"");
    d.wa=std::isfinite(wa)?wa:0.f;d.lin=std::isfinite(lin)?lin:0.f;d.perp=std::isfinite(perp)?perp:0.f;d.trig=std::isfinite(trig)?trig:0.f;
    d.rel=std::isfinite(rel)?rel:0.f;d.revs=std::isfinite(revs)?revs:0.f;
    std::lock_guard lock(decisionMutex);decisions[std::size_t(decisionCount%decisions.size())]=d;++decisionCount;
}
inline std::atomic<int> lastFail{-1};inline std::atomic<float> lastFailTrigger{-1.f},lastFailTouchAgeMs{-1.f},lastFailLightAgeMs{-1.f},lastFailWindowMs{-1.f};
// the last NOT_REVOLVER refusal (class / slot / item name) and the last WEAPON_GONE (state, engine aim bits / hold timer of the put-away)
inline std::atomic<int> lastRefusedKind{-2},lastRefusedSlot{-2},lastGoneState{-1},lastGoneAimBits{-1};inline std::atomic<float> lastGoneTimer{-99.f};
inline std::mutex refusedNameMutex;inline char lastRefusedName[40]{};
inline void noteRefusedName(const char* s) noexcept {
    std::lock_guard lock(refusedNameMutex);std::snprintf(lastRefusedName,sizeof lastRefusedName,"%s",s?s:"");
    for(char& ch:lastRefusedName)if(ch=='"'||ch=='\\'||(ch&&static_cast<unsigned char>(ch)<0x20))ch='?';}
// the pivot facts (report pivot{}), the entry window facts of both hand mappings, the menu's last miss (cleared by an entry)
inline std::atomic<int> pivotSourceSeen{-1},menuFail{-1},lastEntryHand{-1};
inline std::atomic<float> pivotShiftCm{-1.f},pivotModelCmX{},pivotModelCmY{},pivotModelCmZ{},fingerGCmX{},fingerGCmY{},fingerGCmZ{};
inline std::atomic<float> lastFailOtherTouchAgeMs{-1.f},lastFailTrigAnyAgeMs{-1.f},lastEntryTouchAgeMs{-1.f},lastEntryOtherTouchAgeMs{-1.f},lastEntryTrigAnyAgeMs{-1.f};
inline std::atomic<std::uint64_t> shiftClamped{},triggerReads{},triggerFound{},triggerMissing{};
// The weapon's trigger bone in MODEL space (model units), read once per weapon by the pose glue (GameXr: skeleton root^-1 * global).
struct TriggerBone {std::uintptr_t weapon{};bool ok{};V3 posM{};char name[32]{};unsigned tries{};};
inline std::mutex triggerMutex;inline TriggerBone triggerBone{};
// pose glue: true = this weapon still needs a read (new weapon, or a failed read retried every 30th call, at most 20 times)
inline bool triggerWanted(std::uintptr_t weapon) noexcept {
    std::lock_guard lock(triggerMutex);
    if(triggerBone.weapon!=weapon){triggerBone=TriggerBone{};triggerBone.weapon=weapon;return true;}
    if(triggerBone.ok)return false;
    return ++triggerBone.tries%30u==0u&&triggerBone.tries<=600u;
}
inline void noteTrigger(std::uintptr_t weapon,bool ok,V3 posM,const char* name) noexcept {
    std::lock_guard lock(triggerMutex);if(triggerBone.weapon!=weapon)return;
    bump(triggerReads);bump(ok?triggerFound:triggerMissing);
    triggerBone.ok=ok&&TM::finite(posM);triggerBone.posM=posM;std::snprintf(triggerBone.name,sizeof triggerBone.name,"%s",name?name:"");
    for(char& ch:triggerBone.name)if(ch=='"'||ch=='\\'||(ch&&static_cast<unsigned char>(ch)<0x20))ch='?';
}
inline bool triggerFor(std::uintptr_t weapon,V3& out) noexcept {
    std::lock_guard lock(triggerMutex);if(!weapon||triggerBone.weapon!=weapon||!triggerBone.ok)return false;out=triggerBone.posM;return true;}
inline void fmaxAtomic(std::atomic<float>& a,float v) noexcept {float m=a.load(std::memory_order_relaxed);while(v>m&&!a.compare_exchange_weak(m,v)){}}
inline void pulse(unsigned hand,float amplitude,std::uint32_t us,std::uint64_t now,Counter& c) noexcept {
    if(!config.haptics)return;haptics::pulse(hand&1u,amplitude,us,now,haptics::SourceSpin);bump(c);}

// =====================================================================================================================
// Pose composition (pure): the spin C in model space = T(pivot + shift + axis * s) * Rot(axis, theta) * T(-pivot); C(0, 0, 0) = I.
// shift moves the guard pivot onto the VR index finger (the gun visibly orbits the finger, not the palm).
inline M4 composeC(const V3& pivotM,const V3& axisM,float theta,float sModel,const V3& shiftM={}) noexcept {
    const M3 R=TM::rotation(TM::mul(axisM,theta));const V3 Rp=TM::mulv(R,pivotM);
    M4 c{};for(unsigned col=0;col<3;++col)for(unsigned i=0;i<3;++i)c[col*4+i]=R[col*3+i];
    for(unsigned i=0;i<3;++i)c[12+i]=pivotM[i]+shiftM[i]+axisM[i]*sModel-Rp[i];c[15]=1;return c;
}
// Cubic Hermite of the catch blend: normalised start slope m0, end slope 0.
inline float hermite(float u,float m0) noexcept {u=sm::clampf(u,0.f,1.f);return m0*(u-2*u*u+u*u*u)+3*u*u-2*u*u*u;}
inline float hermiteSlope(float u,float m0) noexcept {u=sm::clampf(u,0.f,1.f);return m0*(1-4*u+3*u*u)+6*u-6*u*u;}
enum class Eval : unsigned {NotOwned,Rigid,Spun};
// Pose hooks (any thread): hand `hand`'s published state for this weapon / generation. Spun -> C (model space) to compose onto the held model.
inline Eval evalPose(int hand,std::uintptr_t weapon,std::uint32_t generation,std::uint64_t now,M4& C) noexcept {
    if(hand<0||hand>1||!weapon)return Eval::NotOwned;
    PoseState p{};if(!poseBox[std::size_t(hand)].load(p)||p.state==Off||p.weapon!=weapon||p.generation!=generation)return Eval::NotOwned;
    if(p.simNs&&now>p.simNs&&now-p.simNs>250'000'000ull)return Eval::NotOwned;
    poseSeen[std::size_t(hand)].fetch_add(1,std::memory_order_relaxed);
    if(p.rigid){bump(stats.poseRigid);return Eval::Rigid;}
    float theta=0.f,s=0.f,k=1.f; // k = the share of the finger shift
    if(p.catching){
        const float u=p.blendNs?float(double(now>p.blendStartNs?now-p.blendStartNs:0)/double(p.blendNs)):1.f;
        theta=p.blendFrom+p.blendDelta*hermite(u,p.blendSlope);s=p.blendSlide*(1.f-TM::smoothstep(u));k=1.f-TM::smoothstep(u);
    }else{
        const float dt=p.simNs&&now>p.simNs?std::fmin(float(double(now-p.simNs)*1e-9),.05f):0.f; // the RELATIVE rate: the hook has this frame's hand
        theta=p.theta+p.rel*dt;s=sm::clampf(p.slide+p.slideV*dt,0.f,config.finger);
        if(p.entryBlendNs&&now<p.entryBlendNs+ms(config.entryBlendMs)){
            const float u=TM::smoothstep(float(double(now>p.entryBlendNs?now-p.entryBlendNs:0)/double(ms(config.entryBlendMs))));theta=sm::wrap(theta)*u;s*=u;k=u;}
    }
    if(!std::isfinite(theta)||!std::isfinite(s)||!std::isfinite(k))return Eval::Rigid;
    C=composeC(p.pivotM,p.axisM,theta,s*p.upm,TM::mul(p.shiftM,k));
    return Eval::Spun;
}
// The last spun model (world) per weapon: vrThrowStart starts the flight exactly where the spinning gun is (any thread).
struct Spun {std::uintptr_t weapon{};std::uint64_t ns{};M4 model{};};
inline std::mutex spunMutex;inline Spun lastSpun{};
inline void noteSpun(std::uintptr_t weapon,const M4& model,std::uint64_t now) noexcept {std::lock_guard lock(spunMutex);lastSpun={weapon,now,model};}
inline void clearSpun(std::uintptr_t weapon) noexcept {std::lock_guard lock(spunMutex);if(lastSpun.weapon==weapon)lastSpun.ns=0;}
inline bool spunFor(std::uintptr_t weapon,std::uint64_t now,M4& out) noexcept {
    std::lock_guard lock(spunMutex);
    if(!weapon||lastSpun.weapon!=weapon||!lastSpun.ns||(now>lastSpun.ns&&now-lastSpun.ns>150'000'000ull))return false;
    out=lastSpun.model;return TM::finite(out);
}

// =====================================================================================================================
// Hinge publication (pose thread): the held placement -> the hinge in the holding grip frame.
struct PlacementIn {
    int hand{-1};std::uintptr_t weapon{};std::uint32_t generation{};std::uint64_t now{},referenceSpace{};
    M4 model{},map{};                    // the held placement's model (world) and its LOCAL metres -> world map
    V3 gripPos{};Q gripRot{0,0,0,1};     // the raw holding grip pose (LOCAL)
    V3 handM{},muzzleM{},upM{0,1,0};     // model space: the hand bone point (inverse locator origin), the muzzle origin, the gun's up
    V3 fwdM{0,0,-1};                     // Model space barrel direction (authored muzzle locator -Z)
    float upm{1};                        // world units per metre
};
inline bool notePlacement(const PlacementIn& in) noexcept {
    if(in.hand<0||in.hand>1||!in.weapon||!TM::finite(in.model)||!TM::finite(in.map)||!sm::finiteQ(in.gripRot)){bump(stats.hingeRefused);return false;}
    M4 inv{};if(!TM::inverseUniform(in.map,inv)){bump(stats.hingeRefused);return false;}
    const float sc=TM::scaleOf(in.model);const float k=sc>1e-9f&&in.upm>0.f?in.upm/sc:1.f; // model units per metre
    // pivot (model space): .spin.pivot, else the weapon's 'trigger' bone + trigOff (forward into the guard, down), else the fallback
    // geometry (18 % from the hand bone toward the muzzle, 1.5 cm under the barrel line)
    const V3 up=TM::normalized(in.upM);V3 fwd=TM::normalized(in.fwdM);fwd=TM::normalized(TM::sub(fwd,TM::mul(up,TM::dot(up,fwd))));
    if(!TM::finite(fwd)||TM::length(fwd)<.5f)fwd=V3{0,0,-1};
    V3 pivotM{};std::uint32_t source=PivotGeometry;V3 trig{};
    if(config.pivotSet){pivotM=TM::mul(config.pivot,k);source=PivotProperty;}
    else if(triggerFor(in.weapon,trig)){pivotM=TM::add(trig,TM::mul(TM::sub(TM::mul(fwd,config.trigOff[0]),TM::mul(up,config.trigOff[1])),k));source=PivotTriggerBone;}
    else pivotM=TM::sub(TM::add(in.handM,TM::mul(TM::sub(in.muzzleM,in.handM),.18f)),TM::mul(up,.015f*k));
    const Q q=sm::qnorm(in.gripRot);
    const auto toG=[&](V3 xM){const V3 w=TM::xform(in.model,xM);const V3 l=TM::xform(inv,w);return sm::rotT(q,TM::sub(l,in.gripPos));};
    const auto dirG=[&](V3 dM){const V3 w=TM::xformDir(in.model,dM);const V3 l=TM::xformDir(inv,w);return TM::normalized(sm::rotT(q,l));};
    // axis = the model's lateral axis X_M (perpendicular to the barrel plane = the finger's lateral axis), signed toward the controller's
    // palm-out side for THIS hand
    V3 axisM{1,0,0};V3 aG=dirG(axisM);
    if(TM::dot(aG,sm::palmOutGrip(unsigned(in.hand)))<0.f){axisM={-1,0,0};aG=TM::mul(aG,-1.f);}
    if(TM::length(aG)<.5f){bump(stats.hingeRefused);return false;}
    // NO shift - the guard point of the gun AS HELD, in the grip frame, is the finger anchor (rigid with the controller from the
    // entry on: the brain keeps the hinge of the entry frame); the gun turns through it about its own lateral axis
    const V3 guardG=toG(pivotM),fG=dirG(fwd);
    if(!TM::finite(guardG)||!TM::finite(fG)){bump(stats.hingeRefused);return false;}
    const V3 fingerG=guardG;
    Hinge h{};h.weapon=in.weapon;h.generation=in.generation;h.hand=in.hand;
    h.pivotM=pivotM;h.axisM=axisM;h.upm=k;h.pivotG=guardG;h.axisG=aG;h.shiftM=V3{};h.source=source;h.fwdG=fG;
    V3 c=TM::sub(toG(TM::add(pivotM,TM::mul(config.com,k))),guardG);c=TM::sub(c,TM::mul(aG,TM::dot(aG,c)));h.comG=c;
    pivotSourceSeen.store(int(source),std::memory_order_relaxed);pivotShiftCm.store(0.f,std::memory_order_relaxed);
    {const float d=k>1e-9f?100.f/k:0.f;pivotModelCmX.store(pivotM[0]*d,std::memory_order_relaxed);pivotModelCmY.store(pivotM[1]*d,std::memory_order_relaxed);
     pivotModelCmZ.store(pivotM[2]*d,std::memory_order_relaxed);}
    fingerGCmX.store(fingerG[0]*100.f,std::memory_order_relaxed);fingerGCmY.store(fingerG[1]*100.f,std::memory_order_relaxed);fingerGCmZ.store(fingerG[2]*100.f,std::memory_order_relaxed);
    h.lever=TM::length(c);h.inertia=h.lever*h.lever+config.gyration*config.gyration;
    h.centreG=toG(TM::add(in.handM,TM::mul(TM::sub(in.muzzleM,in.handM),.35f))); // = vrThrowStart's flight point
    h.ns=in.now;h.referenceSpace=in.referenceSpace;
    if(!TM::finite(h.pivotG)||!TM::finite(h.comG)||!TM::finite(h.centreG)||!(h.inertia>1e-6f)){bump(stats.hingeRefused);return false;}
    hingeBox[std::size_t(in.hand)].store(h);bump(stats.hingeRefreshes);
    return true;
}

// =====================================================================================================================
// The hinge simulation (pure; with per-interval glitch tests).
class Sim {
public:
    struct Pose {V3 p{};Q q{0,0,0,1};};
    void start(const Hinge& h,float omSeed,V3 vSeed) noexcept {
        *this=Sim{};h_=h;om_=std::isfinite(omSeed)?omSeed:0.f;vf_=TM::finite(vSeed)?vSeed:V3{};vs_=vf_;}
    V3 pivot(const Pose& x) const noexcept {return TM::add(x.p,sm::rot(x.q,h_.pivotG));}
    V3 axis(const Q& q) const noexcept {return TM::normalized(sm::rot(q,h_.axisG));}
    // One tape interval a -> b (DT seconds). gap = tracking gap > 60 ms / space change: gravity only, no drive, vf re-seeded at b.
    void interval(const Pose& a,const Pose& b,float DT,float trigger,float b0,float headDist,float fps,bool gap) noexcept {
        passed_=false;if(!(DT>0.f)||!std::isfinite(DT))return;
        const Config& c=config;
        const V3 vpi=TM::mul(TM::sub(pivot(b),pivot(a)),1.f/DT);const float waI=TM::dot(TM::mul(sm::rotvec(a.q,b.q),1.f/DT),axis(b.q));
        bool gv=false,gw=false;
        if(gap){
            ++gaps_;const float span=std::fmin(DT,.25f);const unsigned n=std::max(1u,unsigned(std::ceil(span/.004f)));
            for(unsigned j=0;j<n;++j)sub(span/float(n),pivot(b),pivot(b),b.q,b.q,trigger,b0,headDist,fps,true,true);
            vf_=vs_=TM::finite(vpi)?vpi:V3{};vBase_=vf_;haveVBase_=true;wBase_=std::isfinite(waI)?waI:0.f;haveWBase_=true;gRun_=wRun_=0;
            return;
        }
        if(haveVBase_&&TM::length(TM::sub(vpi,vBase_))/DT>c.accGlitch){
            gv=true;
            if(gRun_<3)gRunV_[gRun_++]=vpi;else{gRunV_[0]=gRunV_[1];gRunV_[1]=gRunV_[2];gRunV_[2]=vpi;}
            if(gRun_>=3&&TM::length(TM::sub(gRunV_[2],gRunV_[1]))/DT<=c.accGlitch&&TM::length(TM::sub(gRunV_[1],gRunV_[0]))/DT<=c.accGlitch){
                gv=false;vf_=vpi;gRun_=0;} // three consistent "glitches" = a real change: accept, re-seed (no impulse)
        }else gRun_=0;
        if(!gv){vBase_=vpi;haveVBase_=true;}
        if(haveWBase_&&std::fabs(waI-wBase_)>c.angGlitch){
            gw=true;
            if(wRun_<3)wRunV_[wRun_++]=waI;else{wRunV_[0]=wRunV_[1];wRunV_[1]=wRunV_[2];wRunV_[2]=waI;}
            if(wRun_>=3&&std::fabs(wRunV_[2]-wRunV_[1])<=c.angGlitch&&std::fabs(wRunV_[1]-wRunV_[0])<=c.angGlitch){gw=false;wRun_=0;}
        }else wRun_=0;
        if(!gw){wBase_=waI;haveWBase_=true;}
        const unsigned n=std::max(1u,std::min(c.substeps,32u));const float dt=DT/float(n);
        for(unsigned j=0;j<n;++j){
            const float u0=float(j)/float(n),u1=float(j+1)/float(n);
            const Pose pa=lerp(a,b,u0),pb=lerp(a,b,u1);
            sub(dt,pivot(pa),pivot(pb),pa.q,pb.q,trigger,b0,headDist,fps,gv,gw);
        }
    }
    // Fumble: the angle and the relative rate (Om = wa + rel).
    void set(double theta,float rel) noexcept {theta_=theta;om_=lastWa_+rel;lastRel_=rel;}
    double theta() const noexcept {return theta_;}
    float rel() const noexcept {return lastRel_;}
    V3 wh() const noexcept {return lastWh_;}
    V3 vf() const noexcept {return vf_;}
    // the hanging gun's CoM and its grip (the hand point at the entry), turned by theta about the finger axis (LOCAL)
    V3 com(const Pose& x) const noexcept {return TM::add(pivot(x),sm::rot(x.q,sm::rotAxis(h_.axisG,float(theta_),h_.comG)));}
    V3 gunGrip(const Pose& x) const noexcept {return TM::add(pivot(x),sm::rot(x.q,sm::rotAxis(h_.axisG,float(theta_),TM::mul(h_.pivotG,-1.f))));}
    bool passed() const noexcept {return passed_;}
    unsigned gaps() const noexcept {return gaps_;}
    unsigned takePushes() noexcept {const unsigned p=pushes_;pushes_=0;return p;}
    float om() const noexcept {return om_;}
    static float capFor(float headDist,float fps) noexcept {
        const Config& c=config;const float base=std::fmin(c.maxOmega,c.maxDegFrame*.0174532925f*fps);
        return base*(c.headMin+(1.f-c.headMin)*TM::smoothstep((headDist-.30f)/.15f));}
private:
    Hinge h_{};double theta_{};float om_{},lastWa_{},lastRel_{};V3 vf_{},vs_{},lastWh_{},vBase_{};float wBase_{};
    bool haveVBase_{},haveWBase_{},passed_{};long long kmax_{},kmin_{};unsigned gaps_{},pushes_{};
    unsigned gRun_{},wRun_{};std::array<V3,3> gRunV_{};std::array<float,3> wRunV_{};
    static Pose lerp(const Pose& a,const Pose& b,float u) noexcept {Pose r{};r.p=TM::add(a.p,TM::mul(TM::sub(b.p,a.p),u));r.q=TM::slerp(a.q,b.q,u);return r;}
    void sub(float dt,V3 P0,V3 P1,const Q& R0,const Q& R1,float trigger,float b0,float headDist,float fps,bool gv,bool gw) noexcept {
        const Config& c=config;
        const V3 g{0.f,-9.81f*c.gravity,0.f};const V3 a=axis(R1);
        const V3 vp=TM::mul(TM::sub(P1,P0),1.f/dt);V3 dv{};
        if(!gv){const float k=1.f-std::exp(-dt/.012f);const V3 vfn=TM::add(vf_,TM::mul(TM::sub(vp,vf_),k));dv=TM::sub(vfn,vf_);vf_=vfn;}
        const V3 wh=TM::mul(sm::rotvec(R0,R1),1.f/dt);lastWh_=wh;const float wa=gw?0.f:TM::dot(wh,a);lastWa_=wa;
        const V3 r=sm::rot(R1,sm::rotAxis(h_.axisG,float(theta_),h_.comG)); // CoM arm in LOCAL
        const float I=h_.inertia>1e-6f?h_.inertia:1e-6f;
        // a rigid body pinned at the finger (per unit mass): tau = r x (g - a_finger). Nothing else drives it - no gravity relief,
        // no hand-circle stir, no palm push, no finger drag; the hand's turn reaches the gun only through the friction below.
        om_+=TM::dot(TM::cross(r,g),a)/I*dt;                      // gravity: it hangs / swings under the finger
        om_-=TM::dot(TM::cross(r,dv),a)/I*c.drive;                // the finger's linear acceleration (a swing / flick of the hand)
        vs_=TM::add(vs_,TM::mul(TM::sub(vf_,vs_),1.f-std::exp(-dt/.040f)));
        const float bk=TM::smoothstep((trigger-b0)/std::fmax(1e-3f,1.f-b0));
        float rel=om_-wa;
        // PUSH: a fast hand turn (>= flickOmega) that overtakes the gun in the spin's direction (or a gun nearly at rest) drags the
        // gun up to the hand's rate - when the hand stops, the gun keeps that rate (a flick at the opening, another flick adds speed)
        if(c.flickPush>0.f&&std::fabs(wa)>=c.flickOmega&&rel*wa<0.f&&(om_*wa>=0.f||std::fabs(om_)<2.f)){rel*=std::exp(-c.flickPush*dt);++pushes_;}
        rel*=std::exp(-(c.visc+c.brakev*bk)*dt);const float cf=(c.coul+c.brake*bk)*dt;
        rel=std::fabs(rel)<=cf?0.f:rel-std::copysign(cf,rel);
        const float cap=capFor(headDist,fps);rel=sm::clampf(rel,-cap,cap);om_=wa+rel;
        if(!std::isfinite(om_)){om_=wa;rel=0.f;}
        theta_+=double(rel)*double(dt);lastRel_=rel;
        const double rev=theta_/6.283185307179586;const long long k=theta_>=0?(long long)std::floor(rev):(long long)std::ceil(rev);
        if(k>kmax_){kmax_=k;if(std::fabs(rel)>=4.f)passed_=true;}
        if(k<kmin_){kmin_=k;if(std::fabs(rel)>=4.f)passed_=true;}
    }
};

// =====================================================================================================================
// The brain (input thread; a member of holsters::InputBrain).
// toss = the holsters' tryThrow evaluation of THIS frame for hand h (estimator over the XR tape + every guard), no post, no counters.
using TossFn=bool(*)(void* ctx,unsigned hand,throwing::Release& out) noexcept;
struct StepIn {
    std::uint64_t now{};unsigned h{};const ControllerSample* in{};
    const float* head{};          // HMD position LOCAL (3 floats); nullptr = a head-only frame
    bool noEntry{};               // head-only frame: owned hands step, no entries, the zone rule pauses
    bool holdMode{true};          // WEAPON GRIP HOLD (TOGGLE = no spin)
    bool holdIsH{};               // the engine item is in hand h and held by the mod (holdHand == h && holdOurs)
    bool ownsHold{};              // ... held by the mod's own press (pressSequence == holdSequence)
    int heldKind{-1};std::uint32_t holdGeneration{};
    bool allowed{true};           // holsters contextAllowed
    bool cylinderBlocked{};       // fireBlockSink (revolver cylinder open)
    std::uint64_t lastShotNs{};
    bool deferWouldHold{};        // Side-effect-free peek: the trigger deferral / settle / lasso would keep the gun this frame
    bool insideOwnZone{};         // inside_[h] == current (the gun's own holster haptic zone)
    bool socketValid{};V3 socket{}; // The held gun's own holster socket (body-derived, the holsters' sockets_[current]; same space as grip)
    bool otherHandFree{};         // The other hand holds nothing (no engine item / phantom / foregrip / captured press)
    bool sidearmSlot{};int heldSlot{-1}; // The engine item is the SIDEARM slot's (every pistol-class sidearm spins); its slot
    const char* heldName{};       // The held item's type name (NOT_REVOLVER forensics)
    TossFn toss{};void* ctx{};
};
enum Post : unsigned {PostNone,PostThrow,PostHolster};
struct StepOut {
    bool owned{};Post post{PostNone};unsigned exit{ExitCount};
    bool regrab{},rtArmed{true},cancelled{};throwing::Release release{};
};
inline bool revolverKind(int k) noexcept {return k==1||k==2;}
struct HandView {std::atomic<std::uint32_t> state{};std::atomic<float> theta{},rel{},slide{},tilt{},revs{};};
inline std::array<HandView,2> handView{};

class Brain {
public:
    struct Smp {std::uint64_t ns{},space{};V3 p{};Q q{0,0,0,1};};
    void frameBegin(std::uint64_t now) noexcept {
        if(lastFrameNs_&&now>lastFrameNs_){const float dt=std::fmin(float(double(now-lastFrameNs_)*1e-9),.2f);
            frameDt_+=(dt-frameDt_)*.1f;fps_=sm::clampf(1.f/std::fmax(frameDt_,1e-3f),10.f,120.f);}
        lastFrameNs_=now;
    }
    StepOut step(const StepIn& x) noexcept {
        StepOut out{};if(x.h>1||!x.in)return out;
        const unsigned h=x.h;Hand& H=hands_[h];const auto& in=*x.in;
        if(in.triggers[h]>releaseTriggerGuard)lastSqueezeNs_[h]=x.now;
        { // Frame-rate copy of the XR-rate history (VrTouch.h note) - also covers a path without the XR note
            const std::uint64_t pub=in.publishedNs?in.publishedNs:x.now;
            if(in.triggers[h]>=touch::lightLo&&in.triggers[h]<=config.entryAnalogMax)lightFrameNs_[h]=pub;
            if(in.triggers[h]>touch::anyLo)trigFrameNs_[h]=pub;
            if(in.grips[h]>touch::gripHighLevel)gripFrameNs_[h]=pub;
            if(in.grips[h]>gripOpen){failCounted_[h]=false;logged_[h]=false;} // A new opening may count its first failing condition again
        }
        if(x.holdGeneration!=genSeen_[h]){genSeen_[h]=x.holdGeneration;genChangeNs_[h]=x.now;}
        if(H.state==Off){if(!x.noEntry)tryEnter(x,out);}
        else run(x,out);
        out.owned=H.state!=Off&&H.state!=Leaving;
        publish(h,x);
        return out;
    }
    bool owns(int h) const noexcept {return h>=0&&h<2&&hands_[std::size_t(h)].state!=Off&&hands_[std::size_t(h)].state!=Leaving;}
    bool blocksRt(int h) const noexcept {return h>=0&&h<2&&hands_[std::size_t(h)].state!=Off;}
    // A real suspension (menu, stale input, !world, recenter / space change), holsters off: every hand OFF (rigid), nothing posted
    // (the holsters' await-first-grip / open-hand rules apply).
    void reset(std::uint64_t now) noexcept {
        (void)now;bool any=false;
        for(unsigned h=0;h<2;++h){Hand& H=hands_[h];if(H.state==Off)continue;any=true;
            if(H.state!=Leaving)bump(stats.exits[ExitSuspend]);H=Hand{};poseBox[h].store(PoseState{});storeView(h);}
        if(any)publishMasks();
    }
private:
    struct Hand {
        State state{Off};Sim sim{};Hinge hinge{};std::uint32_t generation{};
        std::uint64_t entryNs{};Smp last{},prev{};bool haveLast{},havePrev{};
        bool lightPress{},pending{},tossPending{};throwing::Release classic{};
        std::uint64_t recentUntilNs{},tossUntilNs{},offSinceNs{},freezeNs{},entryBlendNs{};
        float bias{},slide{},slideV{},armT{},lastTilt{},slidePulsedCm{};std::uint64_t tipNs{},steepSinceNs{};bool atTip{};
        std::uint64_t catchStartNs{},catchNs{};float catchFrom{},catchDelta{},catchSlope{},catchSlide{},catchRel{};bool brakeSqueeze{};
        std::uint64_t quietSinceNs{},zoneSinceNs{},lastPassPulseNs{};unsigned passes{},passesAtQuiet{};
        std::uint64_t seenEval{};unsigned noEvalFrames{};std::uint64_t leaveNs{};bool leftRigid{};
        std::uint64_t blockedSinceNs{}; // The context refused since (the CONTEXT exit)
        std::uint64_t graceUntilNs{}; // A lifted index is ignored until then (entry by the recent history / a touch flicker)
        bool fingerTracked{};         // A live finger signal was seen (touch bit / analog >= analogOn): only then a lift exits
        unsigned touchHand{};         // The touch bit that matched at the entry (h, or h^1 = the other hand's mapping)
        // PROBE: entered without finger / flick - rigid until probeUntilNs; a flick (or the finger) meanwhile -> spin, else holster
        bool probe{},flickHit{};std::uint64_t probeUntilNs{};float flickWa{},flickLin{},flickPerp{},entryTrig{};
    };
    std::array<Hand,2> hands_{};float fps_{36.f},frameDt_{1.f/36.f};std::uint64_t lastFrameNs_{};
    std::array<std::uint64_t,2> lastSqueezeNs_{},genChangeNs_{},lastRegrabNs_{};std::array<std::uint32_t,2> genSeen_{};
    std::array<std::uint64_t,2> lightFrameNs_{},gripFrameNs_{},trigFrameNs_{};std::array<bool,2> failCounted_{};
    std::array<bool,2> logged_{}; // This opening's decision is in the log
    // the opening's decision (once per grip opening)
    void logOpen(unsigned h,const char* branch,const char* why,float wa,float lin,float perp,float trig,std::uint64_t now) noexcept {
        if(logged_[h])return;logged_[h]=true;
        const std::string_view b{branch};
        bump(b=="SPIN"?flick.openSpin:b=="THROW"?flick.openThrow:b=="HOLSTER"?flick.openHolster:b=="DROP"?flick.openDrop:flick.openOther);
        logDecision(now,h,branch,why,wa,lin,perp,trig);
    }
    // a gate refused the spin -> the release decides (peeked: the toss = THROW, else HOLSTER)
    void logRefused(const StepIn& x,const char* why) noexcept {
        if(logged_[x.h])return;bool t=false;
        if(x.toss){throwing::Release r{};try{t=x.toss(x.ctx,x.h,r);}catch(...){t=false;}}
        logOpen(x.h,t?"THROW":"HOLSTER",why,0.f,0.f,0.f,x.in->triggers[x.h],x.now);
    }
    // the flick facts of one tape interval a -> b (hand rate about the gun's lateral axis, the guard point's speed and how
    // perpendicular to the barrel it moves)
    static void flickOf(const Sim& probe,const Hinge& hg,const Smp& a,const Smp& b,float& wa,float& lin,float& perp) noexcept {
        wa=lin=perp=0.f;const float dt=float(double(b.ns-a.ns)*1e-9);if(b.ns<=a.ns||dt<=1e-3f||dt>.06f||a.space!=b.space)return;
        wa=TM::dot(TM::mul(sm::rotvec(a.q,b.q),1.f/dt),probe.axis(b.q));
        const V3 v=TM::mul(TM::sub(probe.pivot(pose(b)),probe.pivot(pose(a))),1.f/dt);lin=TM::length(v);
        if(lin>1e-4f){const V3 bl=TM::normalized(sm::rot(b.q,hg.fwdG));const float c=TM::dot(TM::mul(v,1.f/lin),bl);perp=std::sqrt(std::fmax(0.f,1.f-c*c));}
        if(!std::isfinite(wa))wa=0.f;if(!std::isfinite(lin)){lin=0.f;perp=0.f;}
    }
    static bool flickHitOf(float wa,float lin,float perp) noexcept {
        return std::fabs(wa)>=config.flickOmega||(lin>=config.flickV&&perp>=config.flickPerp);}
    static void noteFlick(Hand& H,float wa,float lin,float perp) noexcept {
        if(std::fabs(wa)>std::fabs(H.flickWa))H.flickWa=wa;
        if(lin>=config.flickV&&perp>=config.flickPerp){if(lin>H.flickLin){H.flickLin=lin;H.flickPerp=perp;}}
        else if(H.flickLin<config.flickV&&lin>H.flickLin){H.flickLin=lin;H.flickPerp=perp;}
        if(flickHitOf(wa,lin,perp))H.flickHit=true;
    }
    void fail(unsigned h,Gate g) noexcept {if(failCounted_[h])return;failCounted_[h]=true;bump(stats.firstFail[g]);lastFail.store(int(g),std::memory_order_relaxed);
        if(g!=GateNotHoldHand)menuFail.store(int(g),std::memory_order_relaxed);}
    static constexpr std::uint64_t tapeStaleNs=60'000'000ull,touchFreshNs=50'000'000ull;
    static constexpr unsigned contextMs=250; // A refused context this long ends the hang (holster)

    static float headDist(const StepIn& x,const V3& P) noexcept {if(!x.head)return 1.f;return TM::length(TM::sub(P,V3{x.head[0],x.head[1],x.head[2]}));}
    static bool touchUsable(std::uint64_t now) noexcept {
        if(!config.touch||!touch::bound.load(std::memory_order_relaxed))return false;
        const auto n=touch::ns.load(std::memory_order_acquire);
        return n&&(n>=now?n-now:now-n)<=touchFreshNs+100'000'000ull&&(touch::bits.load(std::memory_order_relaxed)&touch::touchActive);
    }
    static bool touchBit(unsigned h) noexcept {return ((touch::bits.load(std::memory_order_relaxed)>>h)&1u)!=0;}
    static bool hingeFresh(const StepIn& x,Hinge& out) noexcept {
        Hinge hg{};if(!hingeBox[x.h].load(hg))return false;
        if(hg.hand!=int(x.h)||hg.generation!=x.holdGeneration||!hg.weapon||hg.referenceSpace!=x.in->referenceSpace)return false;
        if(!hg.ns||(x.now>hg.ns&&x.now-hg.ns>250'000'000ull))return false;
        out=hg;return true;
    }
    // hand h's XR tape (VrThrow tapeRing, this space) over (since, upTo], time-ordered
    static unsigned gather(unsigned h,std::uint64_t space,std::uint64_t since,std::uint64_t upTo,std::array<Smp,throwing::tapeSize>& out) noexcept {
        const auto head=throwing::tapeHead.load(std::memory_order_acquire);unsigned n=0;
        for(std::uint64_t i=head>throwing::tapeSize?head-throwing::tapeSize:0;i<head&&n<throwing::tapeSize;++i){throwing::TapeEntry t{};
            if(!throwing::tapeRing[i%throwing::tapeSize].load(t)||t.space!=space||!((t.valid>>h)&1u)||t.ns<=since||t.ns>upTo)continue;
            Smp s{};s.ns=t.ns;s.space=t.space;s.p=t.p[h];s.q=sm::qnorm(t.q[h]);out[n++]=s;}
        for(unsigned i=1;i<n;++i){const Smp v=out[i];unsigned j=i;while(j>0&&out[j-1].ns>v.ns){out[j]=out[j-1];--j;}out[j]=v;}
        unsigned m=0;for(unsigned i=0;i<n;++i)if(!m||out[i].ns!=out[m-1].ns)out[m++]=out[i];
        return m;
    }
    static bool fromFrame(const StepIn& x,Smp& s) noexcept {
        const auto& g=x.in->grip[x.h];if(!g.positionValid||!g.orientationValid)return false;
        s=Smp{};s.ns=x.in->publishedNs;s.space=x.in->referenceSpace;s.p={g.position[0],g.position[1],g.position[2]};
        s.q=sm::qnorm({g.orientation[0],g.orientation[1],g.orientation[2],g.orientation[3]});
        return TM::finite(s.p)&&s.ns;
    }
    static Sim::Pose pose(const Smp& s) noexcept {return {s.p,s.q};}
    float b0(const Hand& H) const noexcept {return H.lightPress?config.b0Light:config.b0Touch;}

    // ---- entry ----
    void tryEnter(const StepIn& x,StepOut& out) noexcept {
        (void)out;const unsigned h=x.h;const auto& in=*x.in;
        if(in.grips[h]>gripOpen)return; // not an opening
        // not a HOLD release frame of the engine item
        if(!x.holdIsH){fail(h,GateNotHoldHand);return;}
        if(!x.ownsHold){fail(h,GateNotOwned);logOpen(h,"OTHER","NOT_OWNED",0.f,0.f,0.f,in.triggers[h],x.now);return;}
        if(x.deferWouldHold){fail(h,GateDeferred);return;} // (a later frame of the same opening may still enter)
        // FORGIVING ENTRY: the finger counts as ON when the index touched the trigger (bound touch; XR-rate
        // history (VrTouch.h note) + this brain's frames) at any sample of the whole entryWindowMs
        // (1.5 s) before the opening, and ANY trigger analog above 0.02 (touch::anyLo, any depth = the finger not fully off the trigger)
        // counts like a touch.
        // The finger is OPTIONAL. Without it a FLICK enters (wrist turn about the gun's lateral axis >= flickOmega,
        // or the guard point >= flickV mostly perpendicular to the barrel) within flickMs before the opening, or
        // within flickMs after it (PROBE, the gun rigid meanwhile); else a plain holster.
        const bool touchMode=touchUsable(x.now);const float trig=in.triggers[h];
        const std::uint64_t pub=in.publishedNs?in.publishedNs:x.now;
        const std::uint64_t from=pub>ms(config.entryWindowMs)?pub-ms(config.entryWindowMs):0,upTo=pub+5'000'000ull;
        const auto within=[&](std::uint64_t t){return t&&t>=from&&t<=upTo;};
        const auto ageMs=[&](std::uint64_t t){return t&&pub>=t?float(double(pub-t)*1e-6):-1.f;};
        const bool touchBound=config.touch&&touch::bound.load(std::memory_order_relaxed);
        const std::uint64_t tOn=touchBound?touch::touchOnNs[h].load(std::memory_order_relaxed):0;
        const std::uint64_t tOther=touchBound?touch::touchOnNs[h^1u].load(std::memory_order_relaxed):0;
        const std::uint64_t lOn=std::max({touch::lightOnNs[h].load(std::memory_order_relaxed),lightFrameNs_[h],
            touch::trigAnyNs[h].load(std::memory_order_relaxed),trigFrameNs_[h]});
        bool touchNow=touchMode&&touchBit(h);const bool lightNow=trig>touch::anyLo;
        bool touchRecent=within(tOn);const bool lightRecent=within(lOn);
        // the touch bit of EITHER hand mapping counts while the other hand is free (otherHandFree)
        unsigned touchHand=h;
        if(!touchNow&&!touchRecent&&config.anyHandTouch&&touchBound&&x.otherHandFree){
            const unsigned o=h^1u;const bool oNow=touchMode&&touchBit(o),oRecent=within(touch::touchOnNs[o].load(std::memory_order_relaxed));
            if(oNow||oRecent){touchNow=oNow;touchRecent=oRecent;touchHand=o;}
        }
        const bool on=touchNow||touchRecent||lightNow||lightRecent;
        if(!on&&!failCounted_[h]){ // the field facts of a finger miss (report entries.lastFail*)
            lastFailTrigger.store(trig,std::memory_order_relaxed);lastFailWindowMs.store(float(double(pub-from)*1e-6),std::memory_order_relaxed);
            lastFailTouchAgeMs.store(ageMs(tOn),std::memory_order_relaxed);lastFailLightAgeMs.store(ageMs(lOn),std::memory_order_relaxed);
            lastFailOtherTouchAgeMs.store(ageMs(tOther),std::memory_order_relaxed); // Both mappings logged
            lastFailTrigAnyAgeMs.store(ageMs(std::max(touch::trigAnyNs[h].load(std::memory_order_relaxed),trigFrameNs_[h])),std::memory_order_relaxed);
            if(within(tOther))bump(stats.fingerOtherHand); // the OTHER hand's touch was on: a swapped-hand symptom
        }
        if(!x.holdMode){bump(stats.refused[RefToggle]);fail(h,GateToggle);logRefused(x,"TOGGLE");return;}
        // every pistol-class sidearm spins - the revolver kinds OR the SIDEARM slot's item (the class read can
        // lag the draw: heldKind -1 / stale); the refusal records what it saw
        if(!revolverKind(x.heldKind)){
            if(x.sidearmSlot){bump(stats.sidearmBySlot);}
            else{bump(stats.refused[RefNotRevolver]);lastRefusedKind.store(x.heldKind,std::memory_order_relaxed);lastRefusedSlot.store(x.heldSlot,std::memory_order_relaxed);
                noteRefusedName(x.heldName);fail(h,GateNotRevolver);logRefused(x,"NOT_REVOLVER");return;}
        }
        // Opened NEAR its holster (the own holster zone core, zoneRadius of the socket) -> the holster in BOTH
        // modes; a refused context / the reload's cylinder (open, closing, PUSH follow, < 400 ms after a close) -> the release (holster)
        if(x.insideOwnZone){bump(stats.refused[RefZone]);fail(h,GateZone);logRefused(x,"ZONE");return;}
        if(!x.allowed){bump(stats.refused[RefContext]);fail(h,GateContext);logRefused(x,"CONTEXT");return;}
        if(x.cylinderBlocked){bump(stats.refused[RefCylinder]);fail(h,GateCylinder);logRefused(x,"CYLINDER");return;}
        // GUN SPIN OFF: anywhere else the revolver FALLS (fast hand = the toss, else a drop at the hand) - never vanishes
        if(!active()){fail(h,GateInactive);openFall(x,out,"SPIN_OFF");return;}
        Hinge hg{};if(!hingeFresh(x,hg)){bump(stats.refused[RefNoHinge]);fail(h,GateNoHinge);openFall(x,out,"NO_HINGE");return;}
        std::array<Smp,throwing::tapeSize> tape{};
        const unsigned n=gather(h,in.referenceSpace,x.now>450'000'000ull?x.now-450'000'000ull:0,x.now+5'000'000ull,tape);
        Smp ks{};
        if(n&&tape[n-1].ns+tapeStaleNs>=x.now)ks=tape[n-1];
        else{if(!fromFrame(x,ks)){fail(h,GatePose);openFall(x,out,"NO_POSE");return;}bump(stats.simFallback);}
        // seed = the largest |hand rate about the finger| in the seedMs before the opening (peak hold: the grip sensor lags the decision),
        // unless the hand already turns the other way faster than half of it; vSeed = the pivot velocity over the last 40 ms
        // the flick facts over flickMs before the opening (peak |rate about the lateral axis|, the fastest guard point motion and
        // how perpendicular to the barrel); a flick seeds with its own peak (x seedGain)
        Sim probe{};probe.start(hg,0.f,{});
        float pk=0.f,wk=0.f;bool haveWk=false;V3 vSeed{};
        Hand F{};const std::uint64_t seedWin=ms(std::max(config.seedMs,config.flickMs));
        for(unsigned i=1;i<n;++i){const auto& a=tape[i-1];const auto& b=tape[i];if(b.ns>ks.ns||b.ns+seedWin<ks.ns)continue;
            float wa=0.f,lin=0.f,perp=0.f;flickOf(probe,hg,a,b,wa,lin,perp);
            const float dt=float(double(b.ns-a.ns)*1e-9);if(dt<=1e-3f||dt>.06f)continue;
            if(b.ns+ms(config.flickMs)>=ks.ns)noteFlick(F,wa,lin,perp);
            if(b.ns+ms(config.seedMs)<ks.ns&&!(std::fabs(wa)>=config.flickOmega))continue; // the seed window (a flick peak counts over flickMs)
            if(std::fabs(wa)>std::fabs(pk))pk=wa;if(b.ns==ks.ns){wk=wa;haveWk=true;}}
        for(unsigned i=0;i<n;++i)if(tape[i].ns<ks.ns&&tape[i].ns+40'000'000ull>=ks.ns){
            const float dt=float(double(ks.ns-tape[i].ns)*1e-9);if(dt>1e-3f)vSeed=TM::mul(TM::sub(probe.pivot(pose(ks)),probe.pivot(pose(tape[i]))),1.f/dt);break;}
        float seed=(haveWk&&pk*wk<0.f&&std::fabs(wk)>.5f*std::fabs(pk))?wk:pk;
        const bool flickAng=std::fabs(F.flickWa)>=config.flickOmega,flickLin=F.flickLin>=config.flickV&&F.flickPerp>=config.flickPerp;
        lastFlickWa.store(F.flickWa,std::memory_order_relaxed);lastFlickLin.store(F.flickLin,std::memory_order_relaxed);
        lastFlickPerp.store(F.flickPerp,std::memory_order_relaxed);fmaxAtomic(maxFlickWa,std::fabs(F.flickWa));
        // GUN SPIN ON: every opening away from the holster HANGS on the finger - no finger / flick / speed requirement,
        // no toss-throw on the opening frame, no probe. The finger / flick facts only name the entry (and a flick seeds the spin).
        const bool probeEntry=false;const char* why="FINGER";
        if(!on){
            if(flickAng){why="FLICK_ANG";bump(flick.entryFlickAng);}
            else if(flickLin){why="FLICK_LIN";bump(flick.entryFlickLin);}
            else why="OPEN";
            if(flickAng)seed=sm::clampf(F.flickWa*config.seedGain,-config.maxOmega,config.maxOmega); // the flick's own rate seeds the spin
        }else bump(flick.entryFinger);
        Hand& H=hands_[h];H=Hand{};
        H.hinge=hg;H.generation=x.holdGeneration;H.entryNs=x.now;H.lightPress=!touchMode;H.graceUntilNs=x.now+ms(config.entryGraceMs);
        H.touchHand=touchHand;if(touchNow||touchRecent)bump(touchHand==h?stats.fingerSame:stats.fingerSwapped);
        // the finger is TRACKED (its lift may end the spin) only once a live signal was seen: entered on a live touch / analog,
        // or later in fingerState. Entered from the history alone, the gun stays on the finger (exits: catch / toss / rest / zone).
        H.fingerTracked=touchNow||lightNow;
        H.flickWa=F.flickWa;H.flickLin=F.flickLin;H.flickPerp=F.flickPerp;H.entryTrig=trig;
        H.probe=probeEntry;H.probeUntilNs=x.now+ms(config.flickMs);
        lastEntryHand.store(int(touchHand),std::memory_order_relaxed);lastEntryTouchAgeMs.store(ageMs(tOn),std::memory_order_relaxed);
        lastEntryOtherTouchAgeMs.store(ageMs(tOther),std::memory_order_relaxed);
        lastEntryTrigAnyAgeMs.store(ageMs(std::max(touch::trigAnyNs[h].load(std::memory_order_relaxed),trigFrameNs_[h])),std::memory_order_relaxed);
        H.lastTilt=sm::tiltDeg(ks.q,h);H.bias=sm::clampf(H.lastTilt,0.f,config.biasDeg);
        H.sim.start(hg,seed,vSeed);H.last=ks;H.haveLast=true;H.seenEval=poseSeen[h].load(std::memory_order_relaxed);
        std::uint64_t recent=0;
        for(const auto t:{lastSqueezeNs_[h],x.lastShotNs,genChangeNs_[h],lastRegrabNs_[h]})
            if(t&&t<=x.now+5'000'000ull&&x.now-std::min(t,x.now)<ms(config.recentMs))recent=std::max(recent,t);
        if(recent)H.recentUntilNs=recent+ms(config.recentMs);
        H.pending=true; // the next frame confirms the open grip (a one-sample dip never spins: grip back > 0.35 = nothing happened)
        H.state=Arming;
        if(probeEntry){logged_[h]=true;return;} // decided (and logged) when the probe resolves; the menu miss stays until then
        menuFail.store(-1,std::memory_order_relaxed);
        logOpen(h,"SPIN",why,F.flickWa,F.flickLin,F.flickPerp,trig,x.now);
        if(on){
            bump(touchNow||touchRecent?stats.entriesTouch:stats.entriesAnalog);
            bump(stats.source[touchNow?SrcTouchNow:touchRecent?SrcTouchRecent:lightNow?SrcLightNow:SrcLightRecent]);
        }
    }
    // an opening that does NOT hang (GUN SPIN OFF, or no hinge / pose to hang on): the revolver FALLS through the VrThrow flight -
    // a fast hand = the toss (THROW), else a DROP at the hand with the grip's velocity over the last ~40 ms (falls, lands, can be
    // picked up, auto-returns). Posted by the holsters' spin glue (OpThrow, holster fallback when refused). .spin.drop 0: a plain holster.
    void openFall(const StepIn& x,StepOut& out,const char* why) noexcept {
        const unsigned h=x.h;const float trig=x.in->triggers[h];
        throwing::Release r{};bool t=false;
        if(x.toss){try{t=x.toss(x.ctx,h,r);}catch(...){t=false;}}
        if(t){out.release=r;out.post=PostThrow;out.exit=ExitOpenThrow;bump(stats.exits[ExitOpenThrow]);bump(stats.throwRequested);
            lastThrowSpeed.store(r.speedPeak,std::memory_order_relaxed);
            logged_[h]=false;logOpen(h,"THROW",why,0.f,r.speedPeak,0.f,trig,x.now);logged_[h]=true;return;}
        if(!config.dropOn){logOpen(h,"HOLSTER",why,0.f,0.f,0.f,trig,x.now);return;} // The release() holsters it
        std::array<Smp,throwing::tapeSize> tape{};
        const unsigned n=gather(h,x.in->referenceSpace,x.now>300'000'000ull?x.now-300'000'000ull:0,x.now+5'000'000ull,tape);
        Smp ks{};bool have=false;
        if(n&&tape[n-1].ns+tapeStaleNs>=x.now){ks=tape[n-1];have=true;}else have=fromFrame(x,ks);
        V3 v{},w{};
        if(have)for(unsigned i=0;i<n;++i)if(tape[i].ns<ks.ns&&tape[i].ns+40'000'000ull>=ks.ns&&tape[i].space==ks.space){
            const float dt=float(double(ks.ns-tape[i].ns)*1e-9);
            if(dt>1e-3f){v=TM::mul(TM::sub(ks.p,tape[i].p),1.f/dt);w=TM::mul(sm::rotvec(tape[i].q,ks.q),1.f/dt);}break;}
        if(!TM::finite(v))v={};if(!TM::finite(w))w={};
        r=throwing::Release{};r.releaseNs=r.peakNs=have&&ks.ns?ks.ns:x.now;r.referenceSpace=x.in->referenceSpace;
        r.v=v;r.w=w;r.speedPeak=r.speedRelease=TM::length(v);r.samples=1;r.hand=int(h);
        out.release=r;out.post=PostThrow;out.exit=ExitOpenDrop;bump(stats.exits[ExitOpenDrop]);bump(stats.throwRequested);bump(flick.drops);
        lastThrowSpeed.store(r.speedPeak,std::memory_order_relaxed);
        logged_[h]=false;logOpen(h,"DROP",why,0.f,r.speedPeak,0.f,trig,x.now);logged_[h]=true;
    }
    // the probe (no finger, no flick at the opening) resolves - a flick / the finger within flickMs -> spin, else a plain holster
    // (the gun was rigid in the hand all along). true = still probing (the caller returns).
    bool resolveProbe(const StepIn& x,Hand& H,StepOut& out) noexcept {
        if(!H.probe)return false;
        const unsigned h=x.h;const float trig=x.in->triggers[h];
        const bool fingerNow=trig>=config.analogOn||(touchUsable(x.now)&&(touchBit(h)||touchBit(H.touchHand)));
        if(H.flickHit||fingerNow){
            H.probe=false;bump(H.flickHit?flick.probeFlick:flick.probeFinger);if(fingerNow)H.fingerTracked=true;
            menuFail.store(-1,std::memory_order_relaxed);
            logged_[h]=false;logOpen(h,"SPIN",H.flickHit?"FLICK_LATE":"FINGER_LATE",H.flickWa,H.flickLin,H.flickPerp,trig,x.now);
            return false;}
        if(x.now<H.probeUntilNs)return true;
        H.probe=false;bump(flick.probeHolster);bump(stats.releasesPlain);fail(h,GateFinger);
        logged_[h]=false;logOpen(h,"HOLSTER","NO_FINGER_NO_FLICK",H.flickWa,H.flickLin,H.flickPerp,trig,x.now);
        leave(x,H,out,ExitPlainHolster,PostHolster,false);
        return true;
    }

    // ---- per frame (owned) ----
    void run(const StepIn& x,StepOut& out) noexcept {
        const unsigned h=x.h;Hand& H=hands_[h];
        if(H.state==Leaving){
            processSamples(x,H);
            if(!x.holdIsH||x.holdGeneration!=H.generation||x.ownsHold||x.now>=H.leaveNs+400'000'000ull)endHand(h);
            return;}
        if(!x.holdIsH||x.holdGeneration!=H.generation){bump(stats.exits[ExitWeaponGone]);out.exit=ExitWeaponGone;
            bump(x.holdGeneration!=H.generation?stats.goneGeneration:stats.goneHandChanged); // (engine-side put-away: engineHolsterWhileKept)
            lastGoneState.store(int(H.state),std::memory_order_relaxed);
            logDecision(x.now,h,"GONE",x.holdGeneration!=H.generation?"GENERATION":"HAND_CHANGED",0.f,0.f,0.f,x.in->triggers[h],H.sim.rel());
            endHand(h);return;}
        if(!active()||!x.holdMode){leave(x,H,out,ExitMenuOff,PostHolster);return;}
        { // Watchdog: the pose hook stopped evaluating this gun (vehicle, mount, linked anim, ...) -> the gun is simply HELD
          // (no exit; a refused context ends it below, the engine's own put-away = WEAPON_GONE)
            const auto a=poseSeen[h].load(std::memory_order_relaxed);
            if(a!=H.seenEval){H.seenEval=a;H.noEvalFrames=0;}
            else if(++H.noEvalFrames==8)bump(flick.poseLostHeld);
        }
        // CONTEXT exit (cutscene / control lock / death / dead eye / mounted gun / unreadable inventory...): refused for contextMs -> holster
        if(!x.allowed&&H.state!=Catching){if(!H.blockedSinceNs)H.blockedSinceNs=x.now;
            if(x.now-H.blockedSinceNs>=ms(contextMs)){bump(flick.contextHolsters);leave(x,H,out,ExitContextHolster,PostHolster);return;}}
        else H.blockedSinceNs=0;
        processSamples(x,H);
        frameChecks(x,H,out);
    }
    void processSamples(const StepIn& x,Hand& H) noexcept {
        std::array<Smp,throwing::tapeSize> s{};
        unsigned n=gather(x.h,x.in->referenceSpace,H.haveLast?H.last.ns:0,x.now+5'000'000ull,s);
        if(!n){Smp f{};if(fromFrame(x,f)&&(!H.haveLast||f.ns>H.last.ns+tapeStaleNs)){s[0]=f;n=1;bump(stats.simFallback);}}
        for(unsigned i=0;i<n;++i)sample(x,H,s[i]);
    }
    // One new sample (time-ordered).
    void sample(const StepIn& x,Hand& H,const Smp& s) noexcept {
        const unsigned h=x.h;
        if(!H.haveLast){H.last=s;H.haveLast=true;return;}
        if(s.ns<=H.last.ns)return;
        const float dt=float(double(s.ns-H.last.ns)*1e-9);
        if(H.state==Catching){H.prev=H.last;H.havePrev=true;H.last=s;return;} // the display is the catch blend
        const bool gap=s.ns-H.last.ns>tapeStaleNs||s.space!=H.last.space;
        if(H.state==Arming&&H.probe&&!gap){float wa=0.f,lin=0.f,perp=0.f;flickOf(H.sim,H.hinge,H.last,s,wa,lin,perp);noteFlick(H,wa,lin,perp);}
        const V3 P=H.sim.pivot(pose(s));
        H.sim.interval(pose(H.last),pose(s),dt,x.in->triggers[h],b0(H),headDist(x,P),fps_,gap);
        bump(stats.simSteps);if(gap)bump(stats.simGaps);
        if(const unsigned pu=H.sim.takePushes())bump(flick.pushSubsteps,pu);
        if(H.sim.passed()){++H.passes;bump(stats.simPasses);
            if(H.state!=Arming&&H.state!=Leaving&&(!H.lastPassPulseNs||s.ns-H.lastPassPulseNs>=60'000'000ull)){H.lastPassPulseNs=s.ns;pulse(h,.20f,12'000,x.now,stats.hapticPass);}}
        fmaxAtomic(maxOmegaSeen,std::fabs(H.sim.rel()));
        const float tilt=sm::tiltDeg(s.q,h);H.lastTilt=tilt;const float tiltRel=tilt-H.bias;
        if(H.state==OnFinger||H.state==Unhooked){
            if(tiltRel>=config.slideDeg)H.armT+=dt;else H.armT=0.f;
            if(H.state==OnFinger){
                H.slideV=H.slide>0.f?-config.backV:0.f;H.slide=std::fmax(0.f,H.slide-config.backV*dt);
                if(H.armT*1000.f>=float(config.tiltMs)){H.state=Unhooked;H.atTip=false;H.tipNs=0;H.steepSinceNs=0;bump(stats.unhookArmed);}
            }else{
                if(tiltRel<=config.slideDeg-config.hystDeg){H.state=OnFinger;H.atTip=false;H.tipNs=0;H.steepSinceNs=0;bump(stats.unhookRehooked);
                    pulse(h,.15f,10'000,x.now,stats.hapticRehook);}
                else{
                    H.slideV=H.slide<config.finger?config.slideV:0.f;H.slide=std::fmin(config.finger,H.slide+config.slideV*dt);
                    fmaxAtomic(maxSlideCm,H.slide*100.f);
                    while(H.slidePulsedCm+1.f<=H.slide*100.f+1e-4f){H.slidePulsedCm+=1.f;pulse(h,.20f,10'000,x.now,stats.hapticSlide);}
                    if(!H.atTip&&H.slide>=config.finger-1e-6f){H.atTip=true;H.tipNs=s.ns;pulse(h,.30f,20'000,x.now,stats.hapticReady);}
                    if(tiltRel>=config.dropDeg){if(!H.steepSinceNs)H.steepSinceNs=s.ns;}else H.steepSinceNs=0;
                }
            }
            if(H.slidePulsedCm>H.slide*100.f)H.slidePulsedCm=std::floor(H.slide*100.f);
        }
        if(H.state==OnFinger){
            const bool quiet=TM::length(H.sim.vf())<config.stillV&&TM::length(H.sim.wh())<config.stillW&&std::fabs(H.sim.rel())<config.stillRel;
            if(!quiet||H.passes!=H.passesAtQuiet){H.quietSinceNs=quiet?s.ns:0;H.passesAtQuiet=H.passes;}
            else if(!H.quietSinceNs)H.quietSinceNs=s.ns;
        }else H.quietSinceNs=0;
        H.prev=H.last;H.havePrev=true;H.last=s;
    }
    // the finger on the trigger this frame: TOUCH (touch or analog >= 0.06; a stale / silent touch keeps the last state <= 200 ms),
    // LIGHT PRESS (analog >= 0.03)
    void fingerState(const StepIn& x,Hand& H) noexcept {
        const unsigned h=x.h;const float trig=x.in->triggers[h];bool known=true,on=false;
        if(!H.lightPress){
            if(!touchUsable(x.now)){if(!H.freezeNs)H.freezeNs=x.now;if(x.now-H.freezeNs<=200'000'000ull)known=false;else on=trig>=config.analogOn;}
            else{H.freezeNs=0;on=touchBit(h)||touchBit(H.touchHand)||trig>=config.analogOff;}
        }else on=trig>=config.analogOff;
        // a live signal (touch bit of either mapping / analog >= analogOn) makes the finger TRACKED; untracked, a "lift" is unknown
        // (the runtime's touch may never report: the gun stays on the finger - catch / toss / rest / zone end it)
        if(!H.fingerTracked&&known&&(trig>=config.analogOn||(touchUsable(x.now)&&(touchBit(h)||touchBit(H.touchHand)))))H.fingerTracked=true;
        if(!H.fingerTracked&&!on&&!(H.tossPending&&x.now<H.tossUntilNs)){H.offSinceNs=0;return;}
        // grace: right after the entry / a fumble the gun hangs on the finger even if the index already lifted (entered by the
        // recent history) - not while the opening frame's toss is pending (the classic window must still throw on a lifted finger)
        if(!on&&!H.tossPending&&x.now<H.graceUntilNs){if(H.offSinceNs==0)bump(stats.graceHeld);known=false;}
        if(!known)return;
        if(on)H.offSinceNs=0;else if(!H.offSinceNs)H.offSinceNs=x.now;
    }
    // The toss at this frame + the gun's own spin; false = no qualifying toss or toward the head (angle <= 60 deg)
    bool toss(const StepIn& x,const Hand& H,throwing::Release& r) const noexcept {
        if(!x.toss)return false;bool ok=false;try{ok=x.toss(x.ctx,x.h,r);}catch(...){ok=false;}
        if(!ok)return false;
        if(x.head&&H.haveLast){const V3 P=H.sim.pivot(pose(H.last));const V3 toHead=TM::sub(V3{x.head[0],x.head[1],x.head[2]},P);
            if(TM::dot(TM::normalized(r.v),TM::normalized(toHead))>=.5f){bump(stats.towardHead);return false;}}
        if(H.haveLast)r.own=TM::mul(H.sim.axis(H.last.q),H.sim.rel());
        r.flags|=throwing::releaseFromSpin;
        return true;
    }
    // ---- frame-level decisions ----
    void frameChecks(const StepIn& x,Hand& H,StepOut& out) noexcept {
        const unsigned h=x.h;const auto& in=*x.in;
        fingerState(x,H);
        switch(H.state){
        case Arming:
            if(in.grips[h]>gripOpen+.05f){bump(stats.armingCancelled);out.cancelled=true; // nothing happened: HOLD kept
                if(H.probe){logged_[h]=false;logOpen(h,"KEEP","REGRIP",H.flickWa,H.flickLin,H.flickPerp,in.triggers[h],x.now);logged_[h]=true;}
                endHand(h);return;}
            if(nearHolster(x,H,out))return;
            if(resolveProbe(x,H,out))return;
            // no finger-off exit (the gun hangs regardless of the finger)
            if(H.pending&&x.now>H.entryNs)H.pending=false; // a later frame without the grip closing back confirms the opening
            if(!H.pending&&x.now>=H.recentUntilNs&&x.now>=H.tossUntilNs){
                H.state=OnFinger;H.entryBlendNs=x.now;H.slide=0.f;bump(stats.entriesArmed);pulse(h,.20f,12'000,x.now,stats.hapticEntry);}
            return;
        case Catching:{
            if(in.grips[h]<=gripOpen){ // fumble: back on the finger from the blended state
                const float u=H.catchNs?sm::clampf(float(double(x.now-H.catchStartNs)/double(H.catchNs)),0.f,1.f):1.f;
                const float th=H.catchFrom+H.catchDelta*hermite(u,H.catchSlope);
                const float rel=H.catchNs?H.catchDelta*hermiteSlope(u,H.catchSlope)/float(double(H.catchNs)*1e-9):0.f;
                H.sim.set(th,rel);H.slide=H.catchSlide*(1.f-TM::smoothstep(u));H.slideV=0.f;H.state=OnFinger;H.entryNs=x.now;
                H.offSinceNs=0;H.graceUntilNs=x.now+ms(config.entryGraceMs);bump(stats.regrabFumbled);return;}
            if(x.now>=H.catchStartNs+H.catchNs){
                bump(stats.regrabDone);pulse(h,std::fmin(.6f,.25f+.02f*std::fabs(H.catchRel)),18'000,x.now,stats.hapticSlap);
                out.regrab=true;out.rtArmed=!H.brakeSqueeze;if(H.brakeSqueeze)bump(stats.regrabBrake);
                logDecision(x.now,h,"CATCH",H.brakeSqueeze?"REGRAB_BRAKE":"REGRAB",0.f,0.f,0.f,in.triggers[h],H.catchRel);
                lastRegrabNs_[h]=x.now;endHand(h);}
            return;}
        case OnFinger:case Unhooked:break;
        default:return;
        }
        if(nearHolster(x,H,out))return; // brought close to the holster -> it goes in by itself
        if(in.grips[h]>=gripClose){beginCatch(x,H);return;}
        // The gun NEVER falls by itself - no finger-off, rest / still, steep or pose-lost exit. The ways out: the catch
        // (above), the TILT off the finger (below), the own holster zone (below), a context / menu exit (run()).
        if(H.state==Unhooked){
            // the stroke has peaked (never the early, slow part): the estimator's peak is older than the newest sample and the hand's
            // newest interval is <= 0.9 x the peak
            throwing::Release r{};
            if(x.allowed&&toss(x,H,r)&&r.peakNs<r.releaseNs){
                float vNew=0.f;if(H.havePrev&&H.last.ns>H.prev.ns)vNew=TM::length(TM::sub(H.last.p,H.prev.p))/float(double(H.last.ns-H.prev.ns)*1e-9);
                if(vNew<=.9f*r.speedPeak){out.release=r;leave(x,H,out,ExitTiltThrow,PostThrow);return;}
            }
            // slid off the fingertip -> it FALLS now with the current velocity (a fast hand = the toss estimator: it flies; else a drop)
            if(H.atTip){
                if(x.allowed&&toss(x,H,r)){out.release=r;leave(x,H,out,ExitTiltThrow,PostThrow);return;}
                drop(x,H,out,ExitTiltDrop,ExitTiltHolster);return;}
        }
        // the zone rule (head frames): held quietly in the gun's own holster zone -> holster (hanging or sliding)
        if((H.state==OnFinger||H.state==Unhooked)&&x.head&&x.insideOwnZone&&std::fabs(H.sim.rel())<3.f){
            if(!H.zoneSinceNs)H.zoneSinceNs=x.now;
            if(x.now-H.zoneSinceNs>=ms(config.zoneMs)){leave(x,H,out,ExitZoneHolster,PostHolster);return;}
        }else H.zoneSinceNs=0;
    }
    // NEAR HOLSTER (the spinning / hanging gun brought close to its holster goes in by itself):
    // any spin state but the catch (ARMING / ON_FINGER / UNHOOKED) - the gun (guard pivot, CoM, its grip) or the hand within nearRadius
    // of its own holster socket -> holster NOW (no quiet time, no speed limit); the put-away executor gives the normal holster haptic.
    bool nearHolster(const StepIn& x,Hand& H,StepOut& out) noexcept {
        if(!x.head||!x.socketValid||!H.haveLast||!(config.nearRadius>0.f))return false;
        if(H.last.space!=x.in->referenceSpace)return false;
        const Sim::Pose p=pose(H.last);
        const V3 pts[4]{H.sim.pivot(p),H.sim.com(p),H.sim.gunGrip(p),p.p};
        float d=1e9f;for(const V3& q:pts){if(!TM::finite(q))continue;d=std::fmin(d,TM::length(TM::sub(q,x.socket)));}
        if(!(d<=config.nearRadius))return false;
        bump(stats.autoHolsterNear);lastNearCm.store(d*100.f,std::memory_order_relaxed);H.zoneSinceNs=0;
        leave(x,H,out,ExitNearHolster,PostHolster);return true;
    }
    // DROP: the gun leaves the finger through the VrThrow flight with the guard point's velocity and its own spin (falls, lands, can
    // be picked up, auto-returns) - never an instant holster mid-air. .spin.drop 0 / no pose: a plain holster exit.
    void drop(const StepIn& x,Hand& H,StepOut& out,unsigned dropE,unsigned holsterExit) noexcept {
        if(!config.dropOn||!H.haveLast||H.state==Arming){leave(x,H,out,holsterExit,PostHolster);return;}
        throwing::Release r{};r.releaseNs=r.peakNs=H.last.ns?H.last.ns:x.now;r.referenceSpace=H.last.space?H.last.space:x.in->referenceSpace;
        V3 v=H.sim.vf();if(!TM::finite(v))v={};V3 w=H.sim.wh();if(!TM::finite(w))w={};
        r.v=v;r.w=w;r.speedPeak=r.speedRelease=TM::length(v);r.samples=1;r.hand=int(x.h);
        r.own=TM::mul(H.sim.axis(H.last.q),H.sim.rel());if(!TM::finite(r.own))r.own={};
        r.flags=throwing::releaseFromSpin;
        out.release=r;bump(flick.drops);leave(x,H,out,dropE,PostThrow);
    }
    void exitClassic(const StepIn& x,Hand& H,StepOut& out) noexcept {
        if(H.tossPending&&x.allowed){out.release=H.classic;leave(x,H,out,ExitClassicThrow,PostThrow);}
        else if(H.state==Arming)leave(x,H,out,ExitClassicHolster,PostHolster); // Still rigid in the hand: the holster
        else drop(x,H,out,ExitFingerOffDrop,ExitClassicHolster);             // Already on the finger -> it falls
    }
    void beginCatch(const StepIn& x,Hand& H) noexcept {
        const unsigned h=x.h;const auto& in=*x.in;
        const float ext=H.last.ns&&x.now>H.last.ns?std::fmin(float(double(x.now-H.last.ns)*1e-9),.05f):0.f;
        const double th0=H.sim.theta()+double(H.sim.rel())*ext;const float rel=H.sim.rel();
        const double turns=std::round(th0/6.283185307179586);const float from=float(th0-turns*6.283185307179586);
        float d=0.f,T=0.f,m0=0.f;
        if(std::fabs(rel)>=3.f){ // ON PASS: the next crossing of 0 mod 2pi in the direction of motion, never reversing
            const double tw=6.283185307179586;double target=rel>0?std::floor(double(from)/tw+1.0)*tw:std::ceil(double(from)/tw-1.0)*tw;
            if(std::fabs(target-double(from))<1e-4)target+=rel>0?tw:-tw;
            d=float(target-double(from));T=sm::clampf(std::fabs(d)/std::fabs(rel),.05f,.25f);m0=sm::clampf(rel*T/d,0.f,3.f);bump(stats.regrabOnPass);
        }else{
            d=-sm::wrap(from);T=.08f+.12f*std::fabs(d)/sm::pi;m0=std::fabs(d)>1e-4f?sm::clampf(rel*T/d,-1.f,2.f):0.f;bump(stats.regrabShortest);}
        H.catchFrom=from;H.catchDelta=d;H.catchSlope=m0;H.catchNs=std::uint64_t(double(T)*1e9);H.catchStartNs=x.now;H.catchSlide=H.slide;H.catchRel=rel;
        H.brakeSqueeze=in.triggers[h]>triggerRearm;H.entryBlendNs=0;
        H.state=Catching;
    }
    // an exit that posts: the gun keeps its spun look while the engine command lands (LEAVING; the holsters own the hand again)
    void leave(const StepIn& x,Hand& H,StepOut& out,unsigned exit,Post post,bool log=true) noexcept {
        if(exit<ExitCount)bump(stats.exits[exit]);out.exit=exit;out.post=post;
        if(log)logDecision(x.now,x.h,post==PostThrow?(dropExit(exit)?"DROP":"THROW"):"HOLSTER",exit<ExitCount?exitNames[exit]:"?",0.f,
            TM::length(H.sim.vf()),0.f,x.in->triggers[x.h],H.sim.rel(),float(H.sim.theta()/6.283185307179586));
        if(post==PostThrow){bump(stats.throwRequested);lastThrowSpeed.store(out.release.speedPeak,std::memory_order_relaxed);
            lastThrowOwn.store(TM::length(out.release.own),std::memory_order_relaxed);}
        H.leftRigid=H.state==Arming;H.state=Leaving;H.leaveNs=x.now;
    }
    void endHand(unsigned h) noexcept {hands_[h].state=Off;}
    void publishMasks() noexcept {
        std::uint32_t m=0,k=0;bool sup=false;
        for(unsigned h=0;h<2;++h){const State s=hands_[h].state;if(s!=Off)m|=1u<<h;if(s!=Off&&s!=Catching&&s!=Leaving)sup=true;
            if(s!=Off&&s!=Leaving)k|=1u<<h;}
        fingerMask.store(m,std::memory_order_release);aimSuppressedFlag.store(sup,std::memory_order_relaxed);keepMask.store(k,std::memory_order_release);
    }
    void storeView(unsigned h) noexcept {
        const Hand& H=hands_[h];auto& v=handView[h];
        v.state.store(H.state,std::memory_order_relaxed);v.theta.store(float(sm::wrapD(H.sim.theta())),std::memory_order_relaxed);
        v.rel.store(H.sim.rel(),std::memory_order_relaxed);v.slide.store(H.slide,std::memory_order_relaxed);v.tilt.store(H.lastTilt,std::memory_order_relaxed);
        v.revs.store(float(H.sim.theta()/6.283185307179586),std::memory_order_relaxed);
    }
    std::array<bool,2> publishedOff_{true,true};
    void publish(unsigned h,const StepIn& x) noexcept {
        (void)x;Hand& H=hands_[h];
        if(H.state==Off){if(!publishedOff_[h]){poseBox[h].store(PoseState{});publishedOff_[h]=true;storeView(h);}publishMasks();return;}
        publishedOff_[h]=false;storeView(h);
        PoseState p{};p.weapon=H.hinge.weapon;p.generation=H.generation;p.state=H.state;
        p.theta=float(sm::wrapD(H.sim.theta()));p.rel=H.sim.rel();p.slide=H.slide;p.slideV=H.slideV;p.simNs=H.last.ns;
        p.rigid=(H.state==Arming||(H.state==Leaving&&H.leftRigid))?1u:0u;
        if(H.state==Catching){p.catching=1;p.blendFrom=H.catchFrom;p.blendDelta=H.catchDelta;p.blendSlope=H.catchSlope;p.blendSlide=H.catchSlide;
            p.blendStartNs=H.catchStartNs;p.blendNs=H.catchNs;}
        p.entryBlendNs=H.entryBlendNs;p.pivotM=H.hinge.pivotM;p.axisM=H.hinge.axisM;p.upm=H.hinge.upm;p.shiftM=H.hinge.shiftM;
        poseBox[h].store(p);publishMasks();
    }
};

// =====================================================================================================================
// Startup (properties, once; holsters startupEnabled) and the menu row.
inline std::string property(const char* key) noexcept {return throwing::property(key);}
inline void parseV3(const std::string& s,V3& out,bool& set) noexcept {
    if(s.empty())return;float v[3]{};char* e{};const char* p=s.c_str();
    for(int i=0;i<3;++i){v[i]=std::strtof(p,&e);if(e==p||!std::isfinite(v[i])||std::fabs(v[i])>1.f)return;p=e;}
    out={v[0],v[1],v[2]};set=true;
}
inline void startup() noexcept {
    if(started.load(std::memory_order_acquire))return;
    const auto kill=property("debug.rdrvr.vr.spin");killed.store(!kill.empty()&&kill[0]=='0',std::memory_order_relaxed);
    const auto real=[&](const char* key,float& out,float lo,float hi){const auto s=property(key);if(s.empty())return;char* e{};const float x=std::strtof(s.c_str(),&e);
        if(e!=s.c_str()&&std::isfinite(x)&&x>=lo&&x<=hi)out=x;};
    const auto msv=[&](const char* key,unsigned& out,unsigned hi){float f=float(out);real(key,f,0.f,float(hi));out=unsigned(f);};
    const auto flag=[&](const char* key,bool& out){const auto s=property(key);if(!s.empty())out=s[0]!='0';};
    Config& c=config;
    real("debug.rdrvr.vr.spin.gravity",c.gravity,.2f,1.5f);real("debug.rdrvr.vr.spin.drive",c.drive,0.f,2.f);
    real("debug.rdrvr.vr.spin.visc",c.visc,0.f,5.f);real("debug.rdrvr.vr.spin.coul",c.coul,0.f,20.f);
    real("debug.rdrvr.vr.spin.brake",c.brake,0.f,200.f);real("debug.rdrvr.vr.spin.brakev",c.brakev,0.f,50.f);
    real("debug.rdrvr.vr.spin.maxomega",c.maxOmega,5.f,60.f);real("debug.rdrvr.vr.spin.slidedeg",c.slideDeg,5.f,80.f);
    real("debug.rdrvr.vr.spin.hyst",c.hystDeg,0.f,30.f);real("debug.rdrvr.vr.spin.finger",c.finger,.005f,.12f);
    real("debug.rdrvr.vr.spin.dropdeg",c.dropDeg,20.f,90.f);real("debug.rdrvr.vr.spin.gyration",c.gyration,.01f,.3f);
    real("debug.rdrvr.vr.spin.analog",c.analogOn,.02f,.30f);
    real("debug.rdrvr.vr.spin.entrymax",c.entryAnalogMax,.10f,.90f);real("debug.rdrvr.vr.spin.zoneradius",c.zoneRadius,0.f,.50f);
    real("debug.rdrvr.vr.spin.nearradius",c.nearRadius,0.f,.60f);
    msv("debug.rdrvr.vr.spin.lookback",c.entryLookbackMs,500);msv("debug.rdrvr.vr.spin.anchormax",c.entryAnchorMaxMs,2000);msv("debug.rdrvr.vr.spin.grace",c.entryGraceMs,2000);
    msv("debug.rdrvr.vr.spin.seedms",c.seedMs,300);msv("debug.rdrvr.vr.spin.tiltms",c.tiltMs,2000);msv("debug.rdrvr.vr.spin.unhookms",c.unhookMs,10000);
    msv("debug.rdrvr.vr.spin.recent",c.recentMs,2000);msv("debug.rdrvr.vr.spin.fingeroff",c.fingerOffMs,1000);msv("debug.rdrvr.vr.spin.classic",c.classicMs,1000);
    msv("debug.rdrvr.vr.spin.rest",c.restMs,60000);msv("debug.rdrvr.vr.spin.zonems",c.zoneMs,5000);
    {float s=float(c.substeps);real("debug.rdrvr.vr.spin.substeps",s,1.f,16.f);c.substeps=unsigned(s);}
    flag("debug.rdrvr.vr.spin.touch",c.touch);flag("debug.rdrvr.vr.spin.zone",c.zone);flag("debug.rdrvr.vr.spin.haptics",c.haptics);
    flag("debug.rdrvr.vr.spin.keepaim",c.keepAim);flag("debug.rdrvr.vr.spin.anyhand",c.anyHandTouch);
    parseV3(property("debug.rdrvr.vr.spin.pivot"),c.pivot,c.pivotSet);
    {bool s=false;parseV3(property("debug.rdrvr.vr.spin.com"),c.com,s);}
    {bool s=false;parseV3(property("debug.rdrvr.vr.spin.trigoff"),c.trigOff,s);}   // "fwd down 0" (m)
    msv("debug.rdrvr.vr.spin.window",c.entryWindowMs,5000);
    // the flick entry, the push, "fully still", the drop exits
    real("debug.rdrvr.vr.spin.flickomega",c.flickOmega,1.f,30.f);real("debug.rdrvr.vr.spin.flickv",c.flickV,.2f,5.f);
    real("debug.rdrvr.vr.spin.flickperp",c.flickPerp,0.f,1.f);real("debug.rdrvr.vr.spin.throwv",c.throwV,.5f,20.f);
    real("debug.rdrvr.vr.spin.push",c.flickPush,0.f,200.f);real("debug.rdrvr.vr.spin.seedgain",c.seedGain,0.f,3.f);
    real("debug.rdrvr.vr.spin.stillv",c.stillV,.01f,1.f);real("debug.rdrvr.vr.spin.stillw",c.stillW,.05f,10.f);real("debug.rdrvr.vr.spin.stillrel",c.stillRel,.05f,10.f);
    msv("debug.rdrvr.vr.spin.flickms",c.flickMs,1000);flag("debug.rdrvr.vr.spin.restdrop",c.restDrop);flag("debug.rdrvr.vr.spin.drop",c.dropOn);
    if(!killed.load(std::memory_order_relaxed))rigidFn.store(&rigidFromMask,std::memory_order_release); // the real spin::rigid (reload / hammer)
    started.store(true,std::memory_order_release);
}
// ---- menu row GUN SPIN (Feature::Spin): PROP / UNAVAILABLE / HOLD ONLY / the rule notes ----
// ON = "HANGS ON FINGER  TILT TO DROP" (every opening away from the holster hangs; tilt it off to drop / throw),
// OFF = "RELEASE DROPS" (every opening away from the holster falls / flies). Near the holster both holster.
inline const char* statusNote(bool menu,bool toggle) noexcept {
    if(toggle)return "HOLD ONLY";
    return menu?"HANGS ON FINGER  TILT TO DROP":"RELEASE DROPS";
}
// holstersReady = the holsters engine runs; menu = the GUN SPIN row value
inline void publishStatus(bool holstersReady,bool menu,bool toggle) noexcept {
    setMenu(menu);
    if(killed.load(std::memory_order_relaxed)||::rdrvr::vr::killed(Feature::Spin)){status::publish(Feature::Spin,status::Property);return;}
    if(!holstersReady||!started.load(std::memory_order_acquire)){status::publish(Feature::Spin,status::Unavailable);return;}
    status::publish(Feature::Spin,status::Ready,statusNote(menu,toggle));
}

// =====================================================================================================================
// Report "vr":{"spin":...} (any thread).
inline std::string report() {
    std::string out;out.reserve(4096);char b[1536];
    const auto u=[](const Counter& a){return (unsigned long long)a.load(std::memory_order_relaxed);};
    const auto now=nowNs();const auto tn=touch::ns.load();
    std::snprintf(b,sizeof b,"{\"enabled\":%s,\"killed\":%s,\"menu\":%s,\"fingerMask\":%u,\"aimSuppressed\":%s,"
        "\"touch\":{\"enabled\":%d,\"bound\":%s,\"retried\":%s,\"bindings\":%u,\"reads\":%llu,\"activeReads\":%llu,\"maskedReads\":%llu,\"touchedReads\":%llu,"
        "\"bits\":%u,\"ageMs\":%.0f},",
        active()?"true":"false",killed.load()?"true":"false",menuOn.load()?"true":"false",fingerMask.load(),aimSuppressedFlag.load()?"true":"false",
        touch::enabledState.load(),touch::bound.load()?"true":"false",touch::retried.load()?"true":"false",touch::bindingCount.load(),
        (unsigned long long)touch::reads.load(),(unsigned long long)touch::activeReads.load(),(unsigned long long)touch::maskedReads.load(),
        (unsigned long long)touch::touchedReads.load(),touch::bits.load(),tn&&now>=tn?double(now-tn)/1e6:-1.0);out+=b;
    out+="\"hands\":[";
    for(unsigned h=0;h<2;++h){const auto& v=handView[h];
        std::snprintf(b,sizeof b,"%s{\"state\":\"%s\",\"theta\":%.2f,\"rel\":%.2f,\"slideCm\":%.1f,\"revolutions\":%.2f,\"tiltDeg\":%.1f}",h?",":"",
            stateNames[v.state.load()%6],double(v.theta.load()),double(v.rel.load()),double(v.slide.load())*100.0,double(v.revs.load()),double(v.tilt.load()));out+=b;}
    std::snprintf(b,sizeof b,"],\"entries\":{\"touch\":%llu,\"analog\":%llu,\"armed\":%llu,\"tossPending\":%llu,\"plainReleases\":%llu,\"refused\":{",
        u(stats.entriesTouch),u(stats.entriesAnalog),u(stats.entriesArmed),u(stats.entriesTossPending),u(stats.releasesPlain));out+=b;
    for(unsigned i=0;i<RefCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",refusedNames[i],u(stats.refused[i]));out+=b;}
    out+="},\"firstFail\":{";
    for(unsigned i=0;i<GateCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",gateNames[i],u(stats.firstFail[i]));out+=b;}
    out+="},\"source\":{";
    for(unsigned i=0;i<SrcCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",sourceNames[i],u(stats.source[i]));out+=b;}
    {const int lf=lastFail.load();
     std::snprintf(b,sizeof b,"},\"lastFail\":\"%s\",\"lastFailTrigger\":%.3f,\"lastFailTouchAgeMs\":%.0f,\"lastFailLightAgeMs\":%.0f,\"lastFailWindowMs\":%.0f,"
        "\"fingerOtherHand\":%llu,\"graceHeld\":%llu,\"lightReads\":%llu",
        lf>=0&&lf<int(GateCount)?gateNames[unsigned(lf)]:"NONE",double(lastFailTrigger.load()),double(lastFailTouchAgeMs.load()),double(lastFailLightAgeMs.load()),
        double(lastFailWindowMs.load()),u(stats.fingerOtherHand),u(stats.graceHeld),(unsigned long long)touch::lightReads.load());out+=b;}
    {
        std::string name;{std::lock_guard lock(refusedNameMutex);name=lastRefusedName;}
        std::snprintf(b,sizeof b,",\"fingerMap\":{\"same\":%llu,\"swapped\":%llu},\"sidearmBySlot\":%llu,\"lastRefused\":{\"kind\":%d,\"slot\":%d,\"name\":\"%s\"}",
            u(stats.fingerSame),u(stats.fingerSwapped),u(stats.sidearmBySlot),lastRefusedKind.load(),lastRefusedSlot.load(),name.c_str());out+=b;}
    { // The 1.5 s window facts of both hand mappings (the last miss / the last entry) + the menu's miss note
        const int mf=menuFail.load();
        std::snprintf(b,sizeof b,",\"windowMs\":%u,\"lastFailOtherTouchAgeMs\":%.0f,\"lastFailTrigAnyAgeMs\":%.0f,\"lastEntry\":{\"touchHand\":%d,"
            "\"touchAgeMs\":%.0f,\"otherTouchAgeMs\":%.0f,\"trigAnyAgeMs\":%.0f},\"menuMiss\":\"%s\"",config.entryWindowMs,
            double(lastFailOtherTouchAgeMs.load()),double(lastFailTrigAnyAgeMs.load()),lastEntryHand.load(),double(lastEntryTouchAgeMs.load()),
            double(lastEntryOtherTouchAgeMs.load()),double(lastEntryTrigAnyAgeMs.load()),mf>=0&&mf<int(GateCount)?gateNames[unsigned(mf)]:"NONE");out+=b;}
    std::snprintf(b,sizeof b,"},\"arming\":{\"cancelled\":%llu,\"fingerOff\":%llu},\"exits\":{",u(stats.armingCancelled),u(stats.armingFingerOff));out+=b;
    for(unsigned i=0;i<ExitCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",exitNames[i],u(stats.exits[i]));out+=b;}
    std::snprintf(b,sizeof b,",\"autoHolsterNear\":%llu,\"nearRadiusCm\":%.0f,\"lastNearCm\":%.1f",u(stats.autoHolsterNear),double(config.nearRadius*100.f),
        double(lastNearCm.load()));out+=b;
    { // The keep (no put-away while a hand is kept) and the WEAPON_GONE forensics
        const int gs=lastGoneState.load();
        std::snprintf(b,sizeof b,"},\"keep\":{\"mask\":%u,\"keepAim\":%s,\"suppressed\":{\"holdRelease\":%llu,\"intent\":%llu,\"openHand\":%llu,\"executor\":%llu},"
            "\"engineKeep\":%llu,\"aimKept\":%llu,\"engineHolsterWhileKept\":%llu},\"gone\":{\"generation\":%llu,\"handChanged\":%llu,\"lastState\":\"%s\","
            "\"lastAimBits\":%d,\"lastTimer\":%.2f",
            keepMask.load(),config.keepAim?"true":"false",u(stats.supHoldRelease),u(stats.supIntent),u(stats.supOpenHand),u(stats.supExecutor),
            u(stats.keepWhileSpin),u(stats.aimKeptSpin),u(stats.engineHolsterWhileKept),u(stats.goneGeneration),u(stats.goneHandChanged),
            gs>=0&&gs<6?stateNames[unsigned(gs)]:"NONE",lastGoneAimBits.load(),double(lastGoneTimer.load()));out+=b;}
    std::snprintf(b,sizeof b,"},\"unhook\":{\"armed\":%llu,\"rehooked\":%llu},\"towardHead\":%llu,"
        "\"regrab\":{\"done\":%llu,\"onPass\":%llu,\"shortest\":%llu,\"fumbled\":%llu,\"brakeRearm\":%llu},"
        "\"sim\":{\"steps\":%llu,\"fallback\":%llu,\"gaps\":%llu,\"passes\":%llu,\"maxOmega\":%.1f,\"maxSlideCm\":%.1f},"
        "\"throw\":{\"requested\":%llu,\"lastSpeed\":%.2f,\"lastOwn\":%.2f},\"pose\":{\"placed\":%llu,\"rigid\":%llu,\"rigidFail\":%llu,\"seen\":[%llu,%llu]},"
        "\"hinge\":{\"refreshes\":%llu,\"refused\":%llu},\"blocks\":{\"rtBlockedSpin\":%llu,\"bMasked\":%llu,\"transferRefused\":%llu},"
        "\"haptics\":{\"entry\":%llu,\"pass\":%llu,\"slide\":%llu,\"ready\":%llu,\"rehook\":%llu,\"slap\":%llu}}",
        u(stats.unhookArmed),u(stats.unhookRehooked),u(stats.towardHead),u(stats.regrabDone),u(stats.regrabOnPass),u(stats.regrabShortest),
        u(stats.regrabFumbled),u(stats.regrabBrake),u(stats.simSteps),u(stats.simFallback),u(stats.simGaps),u(stats.simPasses),
        double(maxOmegaSeen.load()),double(maxSlideCm.load()),u(stats.throwRequested),double(lastThrowSpeed.load()),double(lastThrowOwn.load()),
        u(stats.posePlaced),u(stats.poseRigid),u(stats.poseRigidFail),(unsigned long long)poseSeen[0].load(),(unsigned long long)poseSeen[1].load(),
        u(stats.hingeRefreshes),u(stats.hingeRefused),u(stats.rtBlockedSpin),u(stats.bMasked),u(stats.transferRefused),
        u(stats.hapticEntry),u(stats.hapticPass),u(stats.hapticSlide),u(stats.hapticReady),u(stats.hapticRehook),u(stats.hapticSlap));out+=b;
    { // The pivot (source, model cm, the VR finger in the grip frame cm, the shift guard -> finger cm) + the physics in use
        out.pop_back(); // reopen the root object
        const int ps=pivotSourceSeen.load();TriggerBone tb{};{std::lock_guard lock(triggerMutex);tb=triggerBone;}
        std::snprintf(b,sizeof b,",\"pivot\":{\"source\":\"%s\",\"modelCm\":[%.1f,%.1f,%.1f],\"fingerGripCm\":[%.1f,%.1f,%.1f],\"shiftCm\":%.1f,"
            "\"shiftClamped\":%llu,\"trigger\":{\"bone\":\"%s\",\"ok\":%s,\"reads\":%llu,\"found\":%llu,\"missing\":%llu}},"
            "\"physics\":{\"gravity\":%.2f,\"drive\":%.2f,\"visc\":%.2f,\"coul\":%.2f,\"maxOmega\":%.1f}}",
            ps>=0&&ps<3?pivotSourceNames[unsigned(ps)]:"NONE",double(pivotModelCmX.load()),double(pivotModelCmY.load()),double(pivotModelCmZ.load()),
            double(fingerGCmX.load()),double(fingerGCmY.load()),double(fingerGCmZ.load()),double(pivotShiftCm.load()),
            (unsigned long long)shiftClamped.load(),tb.name,tb.ok?"true":"false",(unsigned long long)triggerReads.load(),
            (unsigned long long)triggerFound.load(),(unsigned long long)triggerMissing.load(),
            double(config.gravity),double(config.drive),double(config.visc),double(config.coul),double(config.maxOmega));out+=b;}
    { // The flick entry / push / drop counters and the decision log (newest last; ageMs = now - the decision)
        out.pop_back();
        std::snprintf(b,sizeof b,",\"flick\":{\"cfg\":{\"omega\":%.1f,\"v\":%.2f,\"perp\":%.2f,\"ms\":%u,\"throwV\":%.1f,\"push\":%.0f,\"seedGain\":%.2f,"
            "\"restMs\":%u,\"restDrop\":%s,\"drop\":%s,\"trigOffCm\":[%.1f,%.1f]},\"entries\":{\"finger\":%llu,\"flickAng\":%llu,\"flickLin\":%llu,"
            "\"probes\":%llu,\"probeFlick\":%llu,\"probeFinger\":%llu,\"probeHolster\":%llu},\"tossWins\":%llu,\"flickOverToss\":%llu,"
            "\"pushSubsteps\":%llu,\"drops\":%llu,\"openings\":{\"spin\":%llu,\"throw\":%llu,\"holster\":%llu,\"drop\":%llu,\"other\":%llu},"
            "\"poseLostHeld\":%llu,\"contextHolsters\":%llu,"
            "\"last\":{\"wa\":%.2f,\"lin\":%.2f,\"perp\":%.2f},\"maxWa\":%.2f},\"decisions\":[",
            double(config.flickOmega),double(config.flickV),double(config.flickPerp),config.flickMs,double(config.throwV),double(config.flickPush),
            double(config.seedGain),config.restMs,config.restDrop?"true":"false",config.dropOn?"true":"false",
            double(config.trigOff[0])*100.0,double(config.trigOff[1])*100.0,
            u(flick.entryFinger),u(flick.entryFlickAng),u(flick.entryFlickLin),u(flick.probes),u(flick.probeFlick),u(flick.probeFinger),
            u(flick.probeHolster),u(flick.tossWins),u(flick.flickOverToss),u(flick.pushSubsteps),u(flick.drops),
            u(flick.openSpin),u(flick.openThrow),u(flick.openHolster),u(flick.openDrop),u(flick.openOther),u(flick.poseLostHeld),u(flick.contextHolsters),
            double(lastFlickWa.load()),double(lastFlickLin.load()),double(lastFlickPerp.load()),double(maxFlickWa.load()));out+=b;
        std::array<Decision,12> d{};std::uint64_t cnt=0;{std::lock_guard lock(decisionMutex);d=decisions;cnt=decisionCount;}
        const std::uint64_t first=cnt>d.size()?cnt-d.size():0;
        for(std::uint64_t i=first;i<cnt;++i){const Decision& e=d[std::size_t(i%d.size())];
            std::snprintf(b,sizeof b,"%s{\"n\":%llu,\"ageMs\":%.0f,\"hand\":%d,\"branch\":\"%s\",\"why\":\"%s\",\"wa\":%.2f,\"lin\":%.2f,\"perp\":%.2f,"
                "\"trig\":%.3f,\"rel\":%.2f,\"revs\":%.2f}",i>first?",":"",(unsigned long long)i,e.ns&&now>=e.ns?double(now-e.ns)/1e6:-1.0,e.hand,
                e.branch,e.why,double(e.wa),double(e.lin),double(e.perp),double(e.trig),double(e.rel),double(e.revs));out+=b;}
        out+="]}";}
    return out;
}
} // namespace rdrvr::vr::spin
