#pragma once
// LASSO (menu rows LASSO THROW / LASSO ASSIST): the LIGHT
// shared part of the lasso - everything the holsters InputBrain / the pad filters / the teleport module need without pulling the engine
// side (VrLasso.h: ModuleLasso tick, LASSO arc producer, hooks H1-H3 (+H4), install, report) into their include graph.
//   * config (debug.rdrvr.vr.lasso.* properties, read once at install), grades (H holsters-only, R reader, L launch, O off-screen);
//   * boxes between the threads (all on the engine main thread on the device; seqlocks anyway): PlayerLink (holsters poll -> lasso tick),
//     EngineFacts (tick -> brain), Intent (brain -> tick: arc wanted, commit, throw hand), ShownBox (tick -> brain: the arc that was SHOWN,
//     world units), Plan (brain -> H1, consumed once);
//   * the Brain (input thread = engine main thread inside InputSystem::Update, called by holsters InputBrain::route): states OFF / HELD /
//     READY / LAUNCH_PENDING / FLYING / CAUGHT / RECOVER, the pad RT / LT of BOTH hands while the lasso is the engine item, the latched aim
//     request (force aim keeps the rope: sagPlayer::UpdateZoomState 0x269bd00), keepHold (holsters deferHoldRelease / openHandCheck),
//     ownsHand (grip masks), the release rules per WEAPON GRIP mode, the re-grip grace, face remap / minigame routing flags, haptics;
//   * counters + the 32-entry transition trace (report in VrLasso.h).
// LASSO THROW = RELEASE (default) / SWING / NATIVE. A trigger press >= .lasso.aimon
// (.30) on EITHER hand opens READY at once (force aim + the arc + ring; that hand throws this throw). RELEASE: the let-go (< .lasso.letgo .15)
// throws along the shown arc. SWING: the commit freezes the arc during the swing; a forward hand swing >= .lasso.swing.launch (1.8 m/s) with the
// trigger held throws at its end; a let-go throws too (with or without a swing). A let-go before minReady (200 ms) throws when it elapses.
// No full press, no cancel rules, .holdtrigger read but unused. Rearm per hand. The NATIVE fallback
// (no grade L / no arc) keeps the let-go.
// Rope pull (hand over hand): after a catch the tick (VrLasso.h Module::rope) hit-tests both hands against
// the ENGINE's drawn rope (no second rope is drawn), a grip closed within .rope.radius grabs it, pulling that hand toward the body along the rope
// = native yanks (aniLasso::LassoYank, or the minigame's prompted input as a pad pulse here); the brain keeps the catch while a hand is on the
// rope and for .rope.slack after both let go (then the HOLD let-go applies); a TOGGLE squeeze that
// grabbed the rope never releases.
// Kill switch debug.rdrvr.vr.lasso 0 (Feature::Lasso): nothing here acts (holstersActive() / active() false).
#include "VrBodyLink.h"   // body::Seqlock
#include "VrHaptics.h"
#include "VrMainThread.h"
#include "VrSettings.h"
#include "VrThrow.h"      // ReleaseEstimator + the XR-rate grip tape (swing speed)
#include "runtime/ControllerInput.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <new>
#include <string>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace rdrvr::vr::lasso {
using runtime::ControllerSample;using runtime::PadState;
using V3=std::array<float,3>;
using Counter=std::atomic<std::uint64_t>;
inline void bump(Counter& c,std::uint64_t n=1) noexcept {c.fetch_add(n,std::memory_order_relaxed);}
inline std::uint64_t u64(const Counter& c) noexcept {return c.load(std::memory_order_relaxed);}
inline void maxOf(Counter& c,std::uint64_t v) noexcept {auto m=c.load(std::memory_order_relaxed);while(v>m&&!c.compare_exchange_weak(m,v,std::memory_order_relaxed)){}}
inline constexpr std::uint64_t msNs(std::uint64_t ms) noexcept {return ms*1'000'000ull;}
inline bool freshNs(std::uint64_t stamp,std::uint64_t now,std::uint64_t maxAge) noexcept {return stamp&&(now>=stamp?now-stamp<=maxAge:stamp-now<=maxAge);}

// ============================== engine state enums (sm_StateNames 0x2f3a538) ==============================
inline constexpr std::array<const char*,15> lassoStateNames{"UNEQUIPED","HOLSTERED","DRAWN","IN_FLIGHT","HIT_FAIL","ON_GROUND","HIT_SUCCEED",
    "BROKEN","ATTACHED_TARGET","ATTACHED_TARGET_ON_GROUND","ATTACHED_REELIN_TARGET","ATTACHED_MINI_GAME","ATTACHED_IN_HOGTYING","ATTACHED_DISMOUNTED","LASSOED"};
inline constexpr std::array<const char*,16> userStateNames{"NONE","UNATTACHED","THROWING","THROWN","FAIL_ATTACH","ATTACHED","TUG_WAR_IDLE",
    "TUG_WAR_WIN","TUG_WAR_LOSE","TUG_WAR_WINNER","TUG_WAR_LOSER","ATTACH_BROKEN","REELING_TARGET","REELING_EMPTY","DROP_LASSO","HOGTIE"};
inline constexpr std::array<const char*,14> victimStateNames{"NONE","UPRIGHT","TUG_WAR_IDLE","TUG_WAR_WIN","TUG_WAR_LOSE","TUG_WAR_WINNER",
    "TUG_WAR_LOSER","GROUNDED","GROUNDED_DRAGGED","SUBDUDED","BUCKED_OFF","FREE","HOGTIE","POST_HOGTIED"};
inline constexpr std::array<const char*,7> hogtieNames{"NONE","LIFT","LIFTED","ATTACHED_HUMAN","ATTACHED_ANIMAL","HOLDING","RAGDOLL"};
template<std::size_t N> inline const char* nameOf(const std::array<const char*,N>& t,std::uint32_t v) noexcept {return v<N?t[v]:"?";}
inline constexpr std::uint32_t lsDrawn=2,lsInFlight=3,lsHitSucceed=6,usUnattached=1,usThrowing=2,usThrown=3;
// State 6 = CAUGHT on its first frame; the attached sets (CAUGHT exit) and the miss sets (FLYING -> RECOVER).
inline constexpr bool lassoCaught(std::uint32_t s) noexcept {return s==6||(s>=8&&s<=13);}
inline constexpr bool userCaught(std::uint32_t u) noexcept {return (u>=5&&u<=10)||u==12||u==15;}
inline constexpr bool lassoMissed(std::uint32_t s) noexcept {return s==4||s==5||s==7;}
inline constexpr bool userMissed(std::uint32_t u) noexcept {return u==4||u==11||u==13;}
inline constexpr bool userTugWar(std::uint32_t u) noexcept {return u>=6&&u<=9;}

// ============================== holsters constants mirrored (VrHolsters.h static_asserts they match) ==============================
inline constexpr float gripClose=.65f,gripOpen=.30f,releaseTriggerGuard=.35f,triggerActive=.08f;
inline constexpr std::uint64_t releaseSettleNs=150'000'000ull;

enum class LaunchMode : std::uint8_t {Rt,Direct};
enum class HandRule : std::uint8_t {Other,Right,Left,Hold};   // default Other (the hand that did NOT take the coil)
enum class ArcShow : std::uint8_t {Aim,Always,Off};
enum class P0Mode : std::uint8_t {Vr,Engine};
enum class ProbeMode : std::uint8_t {Engine,Bullet};
enum class Unowned : std::uint8_t {Native,Strip};
struct Config {
    bool dry{};LaunchMode launch{LaunchMode::Rt};bool inflight{true},offscreen{true};HandRule throwHand{HandRule::Other};bool holdTriggerReady{};
    P0Mode p0{P0Mode::Vr};ArcShow arc{ArcShow::Aim};
    float swingMin{1.2f},swingCommit{.8f};bool swingGaze{};float swingWeight{0.f},swingBendDeg{0.f},kmin{1.f},kmax{1.f};
    unsigned rtPulse{3};std::uint32_t minReadyMs{200},planAgeMs{1500};
    float assistRadiusM{1.5f},assistAngleDeg{12.f},assistPitchDeg{8.f};
    ProbeMode probe{ProbeMode::Engine};Unowned unowned{Unowned::Native};std::uint32_t regripMs{2000};float deadzone{1.f};
    unsigned scanHz{4},scanMax{12};bool layout{true};bool yank{};int yankBtn{-1};
    // The trigger level that shows the arc (READY) on either hand, the let-go level, the forward hand speed (m/s) that throws in SWING
    float aimOn{.30f},letGo{.15f},swingLaunch{1.8f};
    // Rope pull (after a catch: grab the ENGINE's rope with either hand, pull it in hand over hand): .lasso.rope 0 = off,
    // .rope.radius (m, grab distance to the rope), .rope.step (m pulled per yank), .rope.minspeed (m/s for a yank before 2 steps), .rope.yank
    // engine (aniLasso::LassoYank 0x21798dc from the tick) / pad (.yankbtn, default A) / off, .rope.slack (ms the rope stays caught with both hands
    // open after a pull), .rope.gap (ms between yanks), .rope.follow draw (the drawn rope's hand end moved to the grabbing hand) / off.
    bool rope{true};float ropeRadiusM{.10f},pullStepM{.18f},pullMinSpeed{.35f};int ropeYank{0};std::uint32_t slackMs{2500},yankGapMs{300};
    bool ropeFollow{true};unsigned yankPadFrames{3};
    // Rope reel (each pull shortens the catch constraint, VrLasso.h elf::reelInOffset): .rope.reel 1/0, .rope.reel.step (m per pull),
    // .rope.reel.rate (m/s the length ramps down), .rope.reel.min (m, never shorter; the engine's own floor data+0x48 applies as well)
    bool reel{true};float reelStepM{.40f},reelRateMps{2.5f},reelMinM{1.0f};
};
inline constexpr std::array<const char*,4> handRuleNames{"OTHER","RIGHT","LEFT","HOLD"};
inline constexpr std::array<const char*,3> arcShowNames{"AIM","ALWAYS","OFF"};
using PropertyReader=bool(*)(const char* name,char* value,std::size_t size) noexcept;
#ifdef __ANDROID__
inline bool androidProperty(const char* name,char* value,std::size_t size) noexcept {
    char b[PROP_VALUE_MAX]{};const int n=__system_property_get(name,b);if(n<=0||!size)return false;std::snprintf(value,size,"%s",b);return true;}
inline std::atomic<PropertyReader> propertyReader{&androidProperty};
#else
inline std::atomic<PropertyReader> propertyReader{nullptr};
#endif
namespace detail {
inline bool text(const char* name,char* out,std::size_t n) noexcept {const auto f=propertyReader.load(std::memory_order_acquire);out[0]=0;return f&&f(name,out,n)&&out[0];}
inline long integer(const char* name,long fallback,long lo,long hi) noexcept {
    char t[96];if(!text(name,t,sizeof t))return fallback;char* e{};const long v=std::strtol(t,&e,10);return e!=t&&*e==0?std::clamp(v,lo,hi):fallback;}
inline float real(const char* name,float fallback,float lo,float hi) noexcept {
    char t[96];if(!text(name,t,sizeof t))return fallback;char* e{};const float v=std::strtof(t,&e);return e!=t&&*e==0&&std::isfinite(v)?std::clamp(v,lo,hi):fallback;}
inline int word(const char* name,std::initializer_list<const char*> words,int fallback) noexcept {
    char t[96];if(!text(name,t,sizeof t))return fallback;int i=0;for(const char* w:words){if(std::strcmp(t,w)==0)return i;++i;}return fallback;}
}
inline Config readConfig() noexcept {
    namespace d=detail;Config c{};
    c.dry=d::integer("debug.rdrvr.vr.lasso.dry",0,0,1)!=0;
    c.launch=d::word("debug.rdrvr.vr.lasso.launch",{"rt","direct"},0)==1?LaunchMode::Direct:LaunchMode::Rt;
    c.inflight=d::integer("debug.rdrvr.vr.lasso.inflight",1,0,1)!=0;c.offscreen=d::integer("debug.rdrvr.vr.lasso.offscreen",1,0,1)!=0;
    c.throwHand=HandRule(d::word("debug.rdrvr.vr.lasso.throwhand",{"other","right","left","hold"},0));
    c.holdTriggerReady=d::word("debug.rdrvr.vr.lasso.holdtrigger",{"off","ready"},0)==1;
    c.p0=d::word("debug.rdrvr.vr.lasso.p0",{"vr","engine"},0)==1?P0Mode::Engine:P0Mode::Vr;
    c.arc=ArcShow(d::word("debug.rdrvr.vr.lasso.arc",{"aim","always","off"},0));
    c.swingMin=d::real("debug.rdrvr.vr.lasso.swing.min",1.2f,.3f,10.f);c.swingCommit=d::real("debug.rdrvr.vr.lasso.swing.commit",.8f,.1f,10.f);
    c.swingGaze=d::word("debug.rdrvr.vr.lasso.swing.aim",{"hand","gaze"},0)==1;
    c.swingWeight=d::real("debug.rdrvr.vr.lasso.swing.weight",0.f,0.f,1.f);c.swingBendDeg=d::real("debug.rdrvr.vr.lasso.swing.bend",0.f,0.f,30.f);
    c.kmin=d::real("debug.rdrvr.vr.lasso.swing.kmin",1.f,.3f,2.f);c.kmax=d::real("debug.rdrvr.vr.lasso.swing.kmax",1.f,.3f,2.f);if(c.kmax<c.kmin)c.kmax=c.kmin;
    c.rtPulse=unsigned(d::integer("debug.rdrvr.vr.lasso.rtpulse",3,1,20));c.minReadyMs=std::uint32_t(d::integer("debug.rdrvr.vr.lasso.minready",200,0,2000));
    c.planAgeMs=std::uint32_t(d::integer("debug.rdrvr.vr.lasso.planage",1500,100,10000));
    c.assistRadiusM=d::real("debug.rdrvr.vr.lasso.assist.radius",1.5f,.1f,5.f);c.assistAngleDeg=d::real("debug.rdrvr.vr.lasso.assist.angle",12.f,1.f,60.f);
    c.assistPitchDeg=d::real("debug.rdrvr.vr.lasso.assist.pitch",8.f,0.f,30.f);
    c.probe=d::word("debug.rdrvr.vr.lasso.probe",{"engine","bullet"},0)==1?ProbeMode::Bullet:ProbeMode::Engine;
    c.unowned=d::word("debug.rdrvr.vr.lasso.unowned",{"native","strip"},0)==1?Unowned::Strip:Unowned::Native;
    c.regripMs=std::uint32_t(d::integer("debug.rdrvr.vr.lasso.regrip",2000,0,10000));c.deadzone=d::real("debug.rdrvr.vr.lasso.deadzone",1.f,0.f,2.f);
    c.scanHz=unsigned(d::integer("debug.rdrvr.vr.lasso.scan.hz",4,1,36));c.scanMax=unsigned(d::integer("debug.rdrvr.vr.lasso.scan.max",12,1,12));
    c.layout=d::integer("debug.rdrvr.vr.lasso.layout",1,0,1)!=0;c.yank=d::integer("debug.rdrvr.vr.lasso.yank",0,0,1)!=0;
    c.yankBtn=d::word("debug.rdrvr.vr.lasso.yankbtn",{"none","a","b","x","y","rt","lt"},0)-1;
    c.aimOn=d::real("debug.rdrvr.vr.lasso.aimon",.30f,.05f,.95f);c.letGo=std::min(d::real("debug.rdrvr.vr.lasso.letgo",.15f,.02f,.9f),c.aimOn-.02f);
    c.swingLaunch=d::real("debug.rdrvr.vr.lasso.swing.launch",1.8f,.5f,10.f);
    c.rope=d::integer("debug.rdrvr.vr.lasso.rope",1,0,1)!=0;c.ropeRadiusM=d::real("debug.rdrvr.vr.lasso.rope.radius",.10f,.03f,.5f);
    c.pullStepM=d::real("debug.rdrvr.vr.lasso.rope.step",.18f,.03f,1.f);c.pullMinSpeed=d::real("debug.rdrvr.vr.lasso.rope.minspeed",.35f,0.f,5.f);
    c.ropeYank=d::word("debug.rdrvr.vr.lasso.rope.yank",{"engine","pad","off"},0);
    c.slackMs=std::uint32_t(d::integer("debug.rdrvr.vr.lasso.rope.slack",2500,0,20000));c.yankGapMs=std::uint32_t(d::integer("debug.rdrvr.vr.lasso.rope.gap",300,50,5000));
    c.ropeFollow=d::word("debug.rdrvr.vr.lasso.rope.follow",{"draw","off"},0)==0;
    c.reel=d::integer("debug.rdrvr.vr.lasso.rope.reel",1,0,1)!=0;c.reelStepM=d::real("debug.rdrvr.vr.lasso.rope.reel.step",.40f,0.f,1.5f);
    c.reelRateMps=d::real("debug.rdrvr.vr.lasso.rope.reel.rate",2.5f,.2f,10.f);c.reelMinM=d::real("debug.rdrvr.vr.lasso.rope.reel.min",1.0f,.5f,10.f);
    return c;
}
inline Config config{};   // written once at install (before the grades are published) / by host tests

enum Grade : unsigned {GradeH=1u,GradeR=2u,GradeL=4u,GradeO=8u};
inline std::atomic<unsigned> grades{0};
inline std::atomic<const char*> installState{"NOT ATTEMPTED"};
// grade H (holsters-only: dead zone, mounted grab, markers / teach-once, rope haptic, DUAL rules) needs nothing beyond the holsters prints:
// always on unless debug.rdrvr.vr.lasso 0.
inline bool holstersActive() noexcept {return !killed(Feature::Lasso);}
// grade R: the brain runs (latch, release rules, face remap, minigame routing, ropeBusy, the arc in native style).
inline bool active() noexcept {return !killed(Feature::Lasso)&&(grades.load(std::memory_order_acquire)&GradeR)!=0;}
// grade L: SWING / TRIGGER with the mod's own launch (H1 + H2 installed).
inline bool launchReady() noexcept {return active()&&(grades.load(std::memory_order_acquire)&GradeL)!=0;}

// ============================== boxes between the threads ==============================
// Holsters EngineSide::tick (ModuleHolsters, main thread, before ModuleLasso) after every successful inventory poll.
struct PlayerLink {
    std::uint64_t ns{};std::uintptr_t player{},actor{},inventory{},handItem{},slotItem{};std::uint32_t guid{},foot{},gameState{};
    std::uint8_t alive{},playerFlags{},owned{},inHand{},riding{},valid{};
};
inline body::Seqlock<PlayerLink> playerBox;
// ModuleLasso tick -> brain (and the report): what the engine says this frame.
struct EngineFacts {
    std::uint64_t ns{};std::uint32_t lassoState{},flight{},userState{},victimState{},hogtie{},promptCode{32};
    std::uint8_t valid{},item{},owned{},inGame{},alive{},minigame{},attached{},mounted{};
    float launchVelocity{},maxThrowDistance{},probeRadius{};
};
inline body::Seqlock<EngineFacts> engineBox;
// brain -> tick: what the LASSO arc should show this frame (the tick maps the throw hand's aim ray itself; the commit freezes it).
struct Intent {
    std::uint64_t ns{},referenceSpace{},commitSeq{};std::uint32_t state{},mode{},throwHand{},holdHand{},committed{},arcWanted{},gaze{},frozen{};
    float headFwd[3]{};
};
inline body::Seqlock<Intent> intentBox;
// Tick -> brain: the arc that was SHOWN, world units. p0Rel / shownArc relative to the player root of that tick.
struct ShownBox {
    std::uint64_t ns{},key{},seq{},referenceSpace{};std::uint32_t mode{},valid{},hasTarget{},committed{},shownCount{},shownStride{};
    float p0[3]{},p0Rel[3]{},dir[3]{},root[3]{},k{1.f},upm{1.f},dtS{0.f};
    std::uint32_t targetGoh{0xffffffffu},targetActor{0xffffffffu};float targetPoint[3]{},targetUp{};
    float shownArc[3*8]{};
};
inline body::Seqlock<ShownBox> shownBox;
struct Plan {
    std::uint64_t seq{},ns{},referenceSpace{};std::uint32_t mode{};
    float dir[3]{};float k{1.f};std::uint32_t p0Mode{};float p0Rel[3]{};
    std::uint32_t targetGoh{0xffffffffu},targetActor{0xffffffffu};float targetUp{},targetPoint[3]{};
    float shownArc[3*8]{};std::uint32_t shownCount{},shownStride{};float swingPeak{},upm{1.f},dtS{0.f};
};
inline body::Seqlock<Plan> planBox;
inline std::atomic<std::uint64_t> planConsumedSeq{0},directRequestSeq{0},directDoneSeq{0};

// ============================== published flags (any thread) ==============================
enum class BrainState : std::uint8_t {Off,Held,Ready,LaunchPending,Flying,Caught,Recover,Count};
inline constexpr std::array<const char*,7> brainStateNames{"OFF","HELD","READY","LAUNCH_PENDING","FLYING","CAUGHT","RECOVER"};
inline constexpr std::array<const char*,3> modeNames{"RELEASE","SWING","NATIVE"}; // LASSO THROW values (index 2 = NATIVE)
inline std::atomic<bool> aimRequestFlag{false};    // the latched aim request (holsters weaponKeep -> force aim)
inline std::atomic<bool> faceRemapFlag{false};     // CAUGHT: Touch A -> pad A, X -> X on foot (VrControls)
inline std::atomic<bool> minigameFlag{false};      // tick: minigame focus while the lasso is in hand or attached
inline std::atomic<int> promptCodeFlag{32};        // tick: swfMiniGameProxy::GetCurrentInput (32 = none)
inline std::atomic<bool> ropeBusyFlag{false};      // FLYING / CAUGHT / RECOVER-attached / minigame (teleport gate 4)
inline std::atomic<unsigned> stateFlag{0};         // BrainState
inline std::atomic<int> holdHandFlag{-1},throwHandFlag{-1};
inline std::atomic<bool> arcUsable{true};          // tick: the arc status gate (false -> native aim, note NO ARC)
inline std::atomic<bool> forceAimMirror{false};    // holsters: the mod's force-aim bit is set (trace / stuckAimFrames)
inline std::atomic<std::uint64_t> notOwnedNs{0};   // holsters: the not-owned dead zone was squeezed (note NOT OWNED for a while)
inline std::atomic<std::uint64_t> brainNs{0};      // last route() call
inline std::atomic<bool> taught{false};            // the lasso was taken once this session (teach-once marker)
inline bool aimRequest() noexcept {return aimRequestFlag.load(std::memory_order_relaxed);}
inline bool faceRemap() noexcept {return faceRemapFlag.load(std::memory_order_relaxed);}
inline bool minigameFocus() noexcept {return minigameFlag.load(std::memory_order_relaxed);}
inline int promptCode() noexcept {return promptCodeFlag.load(std::memory_order_relaxed);}
inline bool ropeBusy() noexcept {return ropeBusyFlag.load(std::memory_order_relaxed);}
// Rope pull (tick -> brain): the hands holding the engine rope (bit per physical hand), the last tick any hand held it (the slack window),
// the grab edge per hand (the TOGGLE squeeze-and-open guard), the native yank as a pad pulse (seq + action, the brain presses it for yankPadFrames)
enum YankPad : int {YankA,YankB,YankX,YankY,YankRt,YankLt,YankL1,YankR1,YankAlt};   // YankA..YankLt = .yankbtn a,b,x,y,rt,lt
inline std::atomic<unsigned> ropeGrabMask{0};
inline std::atomic<std::uint64_t> ropeEngagedNs{0};
inline std::array<std::atomic<std::uint64_t>,2> ropeGrabNs{};
inline std::atomic<std::uint64_t> yankPadSeq{0};
inline std::atomic<int> yankPadAction{-1};
// keepHold / grace: a hand on the rope, or both hands opened less than .rope.slack ago after a pull = the rope stays caught (slack, not let go)
inline bool ropeHolds(std::uint64_t now) noexcept {
    if(!config.rope)return false;if(ropeGrabMask.load(std::memory_order_relaxed))return true;
    const auto e=ropeEngagedNs.load(std::memory_order_relaxed);return e&&config.slackMs&&now>=e&&now-e<msNs(config.slackMs);}
inline BrainState brainState() noexcept {return BrainState(std::min<unsigned>(stateFlag.load(std::memory_order_relaxed),unsigned(BrainState::Count)-1));}

// ============================== counters (report vr.lasso, VrLasso.h) ==============================
enum Drop : unsigned {DropGrip,DropEngine,DropDead,DropCutscene,DropCount};
inline constexpr std::array<const char*,DropCount> dropNames{"grip","engine","dead","cutscene"};
struct Stats {
    // brain
    Counter frames{},ready{},committed{},strongTicks{},cancelSlow{},cancelDown{},cancelGlitch{},cancelTracking{},estimateFallbacks{},rtPulses{},
        aimRequestFrames{},aimLatchedSuspendedFrames{},stuckAimFrames{},maskedGrip{},holdTriggerWithheld{},openHandThrows{},keepHoldFrames{},
        regripGraces{},regripSaved{},recoverTimeouts{},rearmWaits{},nativeFallbackFrames{},flightCapHits{},toggleReleases{},catchPulses{},missPulses{};
    std::array<Counter,DropCount> dropped{};
    // holsters (grade H)
    Counter notOwnedMasked{},mountedLassoGrabs{},transferRefusedLasso{},phantomRefusedLasso{},socketGrabDeferred{},socketGrabDwelled{},lassoZoneEnters{};
    // launch (brain + H1 + H4)
    Counter plans{},consumed{},rejectedPlan{},timeouts{},rtToLaunchUsSum{},rtToLaunchUsMax{},directLaunches{},directRefused{},p0EngineVsVrUmSum{},
        p0EngineVsVrUmMax{},p0Samples{},p0Clamped{},p0WallHits{},v0Written{},ownerMismatch{},noPlan{},dryCalls{},stalePlan{};
    // flight (H2 / H3)
    Counter steps{},tokenFlights{},flightsNative{},homingOn{},homingOff{},targetLost{},offscreenOverrides{},predErrUmSum{},predErrUmMax{},predSamples{},
        shownVsThrownUmSum{},shownVsThrownUmMax{},shownSamples{},hits{},fails{},rangeFails{},catches{},catchesAfterDirect{},restored40c{},stripped{},
        tokensRetired{};
    // arc (producer)
    Counter arcFrames{},arcPublished{},arcHidden{},targets{},pitchSolved{},pitchUnsolved{},outOfRange{},landings{},arcCasts{},probeFallback{},rings{},
        targetSwitches{},noArcTicks{},arcArms{},noLaunchVelocity{},solves{},solvesReused{};
    // scan / tick
    Counter scans{},candidates{},scanUsSum{},scanUsMax{},tickUsSum{},tickUsMax{},ticks{},readsPerFrameSum{},skipped{},exceptions{},scanReads{},scansSkipped{};
    // catch / controls
    Counter caught{},released{},tugWar{},hogtie{},faceRemapFrames{},minigameFrames{},minigameRtReleases{},promptUnpressable{},involuntaryMotionFrames{},
        warpWhileBusy{},stockFaceFrames{},minigameTriggerFrames{};
    std::array<Counter,33> promptCodesSeen{};
    Counter hookMain{},hookOther{};
    Counter ropeGrabs{},pulls{},pullCm{},yankPulses{},yankEngine{},yankPad{},yankRefused{},yankUnsupportedCode{},grabMisses{},ropeFrames{},
        ropeGeomEngine{},ropeGeomLine{},ropeGeomBad{},ropeGeomNone{},followWrites{},slackKeeps{},slackStarts{},toggleGuarded{},graceSavedByRope{};
    Counter reelPulls{},reelRefused{},reelAtMin{},reelWrites{},reelRebuilt{},reelStopped{},reelGainCm{};
};
inline Stats stats;
inline std::atomic<const char*> lastRefusal{"NONE"};
inline std::atomic<float> lastSwingPeak{0.f};

struct TraceEntry {std::uint64_t ns{};std::uint32_t brain{},lasso{},user{},victim{},hogtie{};std::uint8_t forceAimOurs{},aimRequest{},remap{},pad{};
                   float rt{},lt{};char reason[16]{};};
inline std::array<body::Seqlock<TraceEntry>,32> traceRing{};
inline std::atomic<std::uint64_t> traceHead{0};
inline void noteTrace(std::uint32_t brain,const EngineFacts& e,float rt,float lt,const char* reason,std::uint64_t now) noexcept {
    TraceEntry t{};t.ns=now;t.brain=brain;t.lasso=e.lassoState;t.user=e.userState;t.victim=e.victimState;t.hogtie=e.hogtie;
    t.forceAimOurs=forceAimMirror.load(std::memory_order_relaxed);t.aimRequest=aimRequestFlag.load(std::memory_order_relaxed);
    t.remap=faceRemapFlag.load(std::memory_order_relaxed);t.rt=rt;t.lt=lt;
    if(reason)for(std::size_t i=0;i<sizeof t.reason-1&&reason[i];++i){const char c=reason[i];t.reason[i]=(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'?c:'_';}
    const auto h=traceHead.fetch_add(1,std::memory_order_relaxed);traceRing[h%traceRing.size()].store(t);
}

// ============================== the brain (input thread) ==============================
struct RouteIn {
    const ControllerSample* in{};int hold{-1};bool lassoInHand{};bool menu{};bool toggle{};std::uint64_t now{};
    bool headValid{};std::array<float,3> headFwd{};std::array<int,2> inside{{-1,-1}};
};
struct RouteOut {bool owned{};bool putAway{};};
namespace bdetail {
inline bool poseOk(const Pose& p) noexcept {
    if(!p.positionValid||!p.orientationValid)return false;
    for(float x:p.position)if(!std::isfinite(x)||std::fabs(x)>1000.f)return false;
    float n=0;for(float x:p.orientation){if(!std::isfinite(x))return false;n+=x*x;}
    return std::fabs(n-1.f)<.01f;
}
inline float clamp01(float v) noexcept {return std::isfinite(v)?std::clamp(v,0.f,1.f):0.f;}
}
class Brain {
public:
    // Called by holsters InputBrain::route on every routed pad update (also menu frames: the latch never moves there). owned = the lasso
    // branch wrote pad RT / LT (holsters return); false = not the lasso / not active.
    RouteOut route(const RouteIn& r,PadState& pad) noexcept;
    // holsters deferHoldRelease (before its trigger guard) and openHandCheck: true = keep the HOLD item although the grip reads open.
    bool keepHold(unsigned h,const ControllerSample& in,std::uint64_t now) noexcept;
    // holsters applyMasks: this hand's grip never reaches the game (throw hand; the holding hand in CAUGHT TOGGLE).
    bool ownsHand(unsigned h) const noexcept;
    // DUAL rule 3: a free-hand socket grab needs a 300 ms dwell.
    bool dwellRequired() const noexcept;
    BrainState state() const noexcept {return st_;}
    int holdHand() const noexcept {return hold_;}
    int throwHand() const noexcept {return throwHand_;}
    unsigned mode() const noexcept {return mode_;}
    bool committed() const noexcept {return committed_;}
    std::uint64_t planSeq() const noexcept {return planSeq_;}
    std::uint64_t graceUntil() const noexcept {return graceUntil_;}
    void reset() noexcept {*this=Brain{};}
private:
    BrainState st_{BrainState::Off};int hold_{-1},throwHand_{-1};unsigned mode_{0};bool toggle_{};bool inHand_{};
    std::uint64_t enterNs_{},readySince_{},lowSince_{},cancelAimUntil_{},launchEdgeNs_{},catchNs_{},detachSince_{},graceUntil_{},nextGracePulse_{},
        openSince_{},toggleClosedNs_{},secondPulseNs_{},slowSince_{},lastRouteNs_{},lastSpace_{};
    std::uint64_t planSeq_{},commitSeq_{};int lastCode_{32};
    // The seq of the plan THIS launch stored (0 = a plan-less launch: rejected plan) - LAUNCH_PENDING leaves on CONSUMED only
    // for it (planSeq_ still names the previous, consumed plan after a rejection)
    std::uint64_t pendingSeq_{};
    bool committed_{},strong_{},trigDown_{},nativeDown_{},flightSeen_{},lastMenu_{},released_{},holdClosed_{};
    float swingPeak_{};unsigned rtPulseLeft_{},rtGap_{};unsigned secondPulseHand_{};
    int activeHand_{-1};std::array<bool,2> needRest_{};std::array<float,2> lastTrig_{};bool pendingRelease_{},releaseThrow_{true};float fwdPeak_{};
    std::array<throwing::ReleaseEstimator,2> est_{};
    EngineFacts facts_{};bool factsOk_{};
    // Rope pull: the pad yank pulse (seq seen, action, frames left, the alternating-trigger side)
    std::uint64_t yankSeen_{};int yankAct_{-1};unsigned yankLeft_{};bool yankAltRt_{};
    void enter(BrainState s,std::uint64_t now,const char* why) noexcept {
        if(s==st_)return;
        if(s==BrainState::Ready||s==BrainState::Caught)released_=false; // `released` is per catch (a new throw / catch clears it)
        st_=s;enterNs_=now;stateFlag.store(unsigned(s),std::memory_order_relaxed);noteTrace(unsigned(s),facts_,0.f,0.f,why,now);}
    void setAim(bool on) noexcept {aimRequestFlag.store(on,std::memory_order_relaxed);}
    void drop(Drop why) noexcept {if(aimRequestFlag.load(std::memory_order_relaxed))bump(stats.dropped[why]);setAim(false);}
    static void pulse(int hand,float a,std::uint32_t us,std::uint64_t now) noexcept {if(hand==0||hand==1)haptics::pulse(unsigned(hand),a,us,now,haptics::SourceLasso);}
    void toOff(std::uint64_t now,const char* why) noexcept {
        if(st_==BrainState::Flying||st_==BrainState::Caught||st_==BrainState::LaunchPending||st_==BrainState::Ready)drop(DropGrip);else setAim(false);
        faceRemapFlag.store(false,std::memory_order_relaxed);graceUntil_=0;openSince_=0;rtPulseLeft_=0;committed_=false;activeHand_=-1;pendingRelease_=false;
        enter(BrainState::Off,now,why);holdHandFlag.store(-1,std::memory_order_relaxed);throwHandFlag.store(-1,std::memory_order_relaxed);
        Intent i{};i.ns=now;intentBox.store(i);
    }
    void toHeld(std::uint64_t now,const char* why,bool cancelDelay) noexcept {
        // Back from a throw (miss / rope back, victim free, release, launch timeout) a throw trigger still held never starts the
        // next throw by itself (TRIGGER would re-launch 200 ms later, NATIVE would see a fresh RT edge): the rearm rule applies again
        if(st_==BrainState::LaunchPending||st_==BrainState::Flying||st_==BrainState::Caught||st_==BrainState::Recover)
            for(unsigned h=0;h<2;++h)if(lastTrig_[h]>=.10f&&!needRest_[h]){needRest_[h]=true;bump(stats.rearmWaits);}
        activeHand_=-1;pendingRelease_=false;fwdPeak_=0.f;
        cancelAimUntil_=cancelDelay?now+msNs(150):0;committed_=false;strong_=false;swingPeak_=0.f;rtPulseLeft_=0;graceUntil_=0;openSince_=0;
        faceRemapFlag.store(false,std::memory_order_relaxed);enter(BrainState::Held,now,why);
    }
    void release(Drop why,std::uint64_t now,const char* reason) noexcept {
        drop(why);bump(stats.released);graceUntil_=0;openSince_=0;released_=true;toHeld(now,reason,false);
    }
    void startGrace(std::uint64_t now) noexcept {
        if(!config.regripMs)return;graceUntil_=now+msNs(config.regripMs);nextGracePulse_=now;bump(stats.regripGraces);}
    throwing::ReleaseEstimator::Estimate swing(unsigned h,const ControllerSample& in,std::uint64_t now) noexcept {
        throwing::ReleaseEstimator xr{};const bool tape=throwing::fromTape(h,in.referenceSpace,now,xr,throwing::config.tapeMaxAgeNs);
        return (tape?xr:est_[h]).estimate(250'000'000ull,.5f,120'000'000ull);
    }
    bool launch(const ControllerSample& in,std::uint64_t now,bool fallback) noexcept;
};
inline Brain brain;

inline bool Brain::launch(const ControllerSample& in,std::uint64_t now,bool fallback) noexcept {
    // The plan copies the arc SHOWN by the last tick - no cast in the brain
    ShownBox s{};Plan p{};bool ok=shownBox.load(s)&&s.valid&&freshNs(s.ns,now,msNs(200))&&s.referenceSpace==in.referenceSpace;
    if(ok){
        float n=0;for(float x:s.dir)n+=x*x;n=std::sqrt(n);
        for(float x:s.dir)ok=ok&&std::isfinite(x);for(float x:s.p0Rel)ok=ok&&std::isfinite(x);
        ok=ok&&std::fabs(n-1.f)<=1e-3f&&std::isfinite(s.k)&&s.k>0.f;
    }
    if(!ok){bump(stats.rejectedPlan);lastRefusal.store("REJECTED_PLAN",std::memory_order_relaxed);}
    else{
        p.seq=++planSeq_;p.ns=now;p.referenceSpace=s.referenceSpace;p.mode=mode_;for(unsigned i=0;i<3;++i){p.dir[i]=s.dir[i];p.p0Rel[i]=s.p0Rel[i];p.targetPoint[i]=s.targetPoint[i];}
        // opt-in (.lasso.swing.kmin / .kmax, default 1 / 1 = WYSIWYG): the swing peak scales
        // the launch speed
        const float kf=config.kmax>config.kmin?std::clamp(config.kmin+(config.kmax-config.kmin)*(swingPeak_-config.swingMin)/std::max(config.swingMin,.1f),config.kmin,config.kmax):config.kmin;
        p.k=s.k*kf;p.p0Mode=unsigned(config.p0);p.targetGoh=s.hasTarget?s.targetGoh:0xffffffffu;p.targetActor=s.hasTarget?s.targetActor:0xffffffffu;
        p.targetUp=s.targetUp;std::memcpy(p.shownArc,s.shownArc,sizeof p.shownArc);p.shownCount=s.shownCount;p.shownStride=s.shownStride;
        p.swingPeak=swingPeak_;p.upm=s.upm;p.dtS=s.dtS;
        planBox.store(p);bump(stats.plans);
    }
    pendingSeq_=ok?p.seq:0;
    launchEdgeNs_=now;flightSeen_=false;rtGap_=0;
    if(config.dry)rtPulseLeft_=0;
    else if(config.launch==LaunchMode::Direct&&ok&&mode_!=2){rtPulseLeft_=0;directRequestSeq.store(p.seq,std::memory_order_release);}
    else rtPulseLeft_=config.rtPulse;
    if(fallback)bump(stats.estimateFallbacks);
    lastSwingPeak.store(swingPeak_,std::memory_order_relaxed);
    enter(BrainState::LaunchPending,now,ok?"LAUNCH":"LAUNCH_NO_PLAN");
    return ok;
}

inline RouteOut Brain::route(const RouteIn& r,PadState& pad) noexcept {
    RouteOut out{};const auto now=r.now;
    brainNs.store(now,std::memory_order_relaxed);
    factsOk_=engineBox.load(facts_)&&facts_.valid&&freshNs(facts_.ns,now,msNs(250));
    if(!factsOk_)facts_.valid=0;
    const bool act=active()&&r.lassoInHand&&r.in&&(r.hold==0||r.hold==1);
    inHand_=act;
    if(!act){if(st_!=BrainState::Off)toOff(now,r.lassoInHand?"INACTIVE":"NOT_IN_HAND");return out;}
    if(factsOk_&&!facts_.alive){drop(DropDead);if(st_!=BrainState::Off)toOff(now,"DEAD");return out;}
    // not in game (a scripted / presentation change with the lasso still in hand): a suspended frame like the VR menu - triggers withheld, the latch
    // unchanged (the tick drops it when the engine stops reporting attached), no READY / release decisions
    const bool suspended=r.menu||(factsOk_&&!facts_.inGame);
    const auto& in=*r.in;
    bump(stats.frames);
    hold_=r.hold;toggle_=r.toggle;
    int cfgTh=1-hold_;
    switch(config.throwHand){case HandRule::Right:cfgTh=1;break;case HandRule::Left:cfgTh=0;break;case HandRule::Hold:cfgTh=hold_;break;default:break;}
    // READY is opened by the trigger of EITHER hand; that hand throws (arc origin, swing) until the brain is back in HELD
    throwHand_=activeHand_>=0?activeHand_:cfgTh;
    holdHandFlag.store(hold_,std::memory_order_relaxed);throwHandFlag.store(throwHand_,std::memory_order_relaxed);
    unsigned th=unsigned(throwHand_);const unsigned hh=unsigned(hold_);
    const unsigned userMode=std::min(settings().lassoThrowMode(),2u); // 0 RELEASE, 1 SWING, 2 NATIVE
    unsigned requested=userMode;
    if(requested!=2&&(!launchReady()||!arcUsable.load(std::memory_order_relaxed))){requested=2;bump(stats.nativeFallbackFrames);}
    // a running throw keeps its mode; the NATIVE fallback of RELEASE / SWING (no grade L / no arc) still throws on the let-go (RT pulse)
    if(st_==BrainState::Off||st_==BrainState::Held){mode_=requested;releaseThrow_=userMode!=2;}
    for(unsigned h=0;h<2;++h)est_[h].note(in,h);
    // resume after a menu / suspension / recenter
    const bool resumed=lastRouteNs_&&!suspended&&(lastMenu_||now-lastRouteNs_>msNs(150)||in.referenceSpace!=lastSpace_);
    lastRouteNs_=now;lastMenu_=suspended;lastSpace_=in.referenceSpace;
    if(st_==BrainState::Off){
        released_=false;
        for(unsigned h=0;h<2;++h){needRest_[h]=bdetail::clamp01(in.triggers[h])>=.10f;if(needRest_[h])bump(stats.rearmWaits);} // rearm rule per hand
        toHeld(now,"DRAWN",false);
    }
    // per-hand trigger after the rearm rule (a trigger that must rest first reads 0 until it was < .05 on a usable frame; a menu's zeroed sample is no rest)
    std::array<float,2> trig{};
    for(unsigned h=0;h<2;++h){float v=bdetail::clamp01(in.triggers[h]);if(!suspended)lastTrig_[h]=v;
        if(needRest_[h]){if(v<.05f&&!suspended)needRest_[h]=false;else v=0.f;}trig[h]=v;}
    float tt=trig[th];const float ht=bdetail::clamp01(in.triggers[hh]);
    if(th!=hh&&ht>triggerActive)bump(stats.holdTriggerWithheld);
    if(th!=hh&&in.grips[th]>=gripClose)bump(stats.maskedGrip);
    if(in.grips[hh]>gripOpen){openSince_=0;}
    const bool minigame=factsOk_&&facts_.minigame;const int code=factsOk_?int(facts_.promptCode):32;
    const std::uint32_t ls=factsOk_?facts_.lassoState:0,us=factsOk_?facts_.userState:0;
    const bool attached=factsOk_&&(lassoCaught(ls)||userCaught(us));
    if(minigame&&code!=lastCode_&&((code==2&&hh==0)||(code==3&&hh==1)))bump(stats.promptUnpressable); // L1 / R1 on the holding hand (its grip holds the rope)
    lastCode_=minigame?code:32;
    float rt=0.f,lt=0.f;
    if(secondPulseNs_&&now>=secondPulseNs_){secondPulseNs_=0;pulse(int(secondPulseHand_),.25f,30'000,now);}
    if(suspended&&(st_==BrainState::Flying||st_==BrainState::Caught))bump(stats.aimLatchedSuspendedFrames);
    if(suspended&&st_==BrainState::Ready){toHeld(now,"MENU",false);setAim(false);} // the menu zeroes the sample: never a release edge
    switch(st_){
    case BrainState::Held:{
        setAim(cancelAimUntil_&&now<cancelAimUntil_);
        if(suspended)break;
        // A trigger press (>= aimOn) on either hand shows the arc at once (READY + force aim); the configured throw hand wins a tie
        const int pick=trig[unsigned(cfgTh)]>=config.aimOn?cfgTh:(trig[unsigned(1-cfgTh)]>=config.aimOn?1-cfgTh:-1);
        if(pick>=0){
            activeHand_=pick;throwHand_=pick;th=unsigned(pick);tt=trig[th];throwHandFlag.store(throwHand_,std::memory_order_relaxed);
            readySince_=now;lowSince_=0;swingPeak_=0.f;fwdPeak_=0.f;strong_=false;committed_=false;pendingRelease_=false;trigDown_=true;nativeDown_=false;slowSince_=0;
            bump(stats.ready);enter(BrainState::Ready,now,"READY");setAim(true);pulse(pick,.12f,12'000,now); // a light tick: the arc is up
        }
        break;}
    case BrainState::Ready:{
        setAim(true);
        if(mode_==2&&!releaseThrow_){ // NATIVE: the engine aims (force aim) and throws on a full press (RT) of the throw hand
            const bool down=tt>=(nativeDown_?.45f:.55f);nativeDown_=down;if(down)rt=tt;
            if(tt<.05f){if(!lowSince_)lowSince_=now;if(now-lowSince_>=msNs(150)){toHeld(now,"AIM_END",false);setAim(false);}}
            else lowSince_=0;
            if(factsOk_&&attached){launchEdgeNs_=now;catchNs_=now;bump(stats.catchPulses);pulse(hold_,.8f,60'000,now);bump(stats.caught);detachSince_=0;
                enter(BrainState::Caught,now,"CAUGHT");}
            else if(factsOk_&&(ls==lsInFlight||us==usThrowing||us==usThrown||facts_.flight)){launchEdgeNs_=now;flightSeen_=false;enter(BrainState::Flying,now,"NATIVE_THROW");}
            break;
        }
        // RELEASE / SWING (and their NATIVE fallback): the arc is up while the trigger is held; the let-go throws along the shown arc
        if(mode_==1){ // SWING: the commit freezes the shown arc during the swing; a forward swing with the trigger held throws at its end
            const auto e=swing(th,in,now);
            if(e.valid&&!e.glitch)swingPeak_=std::max(swingPeak_,e.speedPeak);
            const float speed=e.valid&&!e.glitch?e.speedRelease:0.f;
            if(!committed_&&speed>config.swingCommit){committed_=true;++commitSeq_;bump(stats.committed);noteTrace(unsigned(st_),facts_,0,0,"COMMIT",now);}
            else if(committed_){if(speed<.4f){if(!slowSince_)slowSince_=now;else if(now-slowSince_>=msNs(250)){committed_=false;++commitSeq_;slowSince_=0;}}else slowSince_=0;}
            if(!strong_&&swingPeak_>=config.swingMin){strong_=true;bump(stats.strongTicks);pulse(throwHand_,.20f,15'000,now);}
            if(!pendingRelease_&&tt>=config.letGo&&e.valid&&!e.glitch&&r.headValid){
                float fx=r.headFwd[0],fz=r.headFwd[2];const float fl=std::sqrt(fx*fx+fz*fz);
                const float vl=std::sqrt(e.v[0]*e.v[0]+e.v[1]*e.v[1]+e.v[2]*e.v[2]);
                if(fl>.2f&&std::isfinite(vl)){fx/=fl;fz/=fl;
                    const float fwd=e.v[1]<-throwing::config.downDot*vl?0.f:e.v[0]*fx+e.v[2]*fz; // a downward-dominant motion is no throw
                    if(fwd>fwdPeak_)fwdPeak_=fwd;
                    if(fwdPeak_>=config.swingLaunch&&fwd<.5f*fwdPeak_){pendingRelease_=true;noteTrace(unsigned(st_),facts_,0,0,"SWING_THROW",now);}}
            }
        }
        if(!pendingRelease_&&tt<config.letGo){pendingRelease_=true;noteTrace(unsigned(st_),facts_,0,0,"LET_GO",now);}
        // the throw waits for minReady after READY (the engine's force aim + a shown arc); no cancel, no full press, no dwell
        if(pendingRelease_&&now-readySince_>=msNs(config.minReadyMs)){
            if(!committed_){committed_=true;++commitSeq_;bump(stats.committed);}
            trigDown_=false;launch(in,now,false);
        }
        break;}
    case BrainState::LaunchPending:{
        setAim(true);
        if(pendingSeq_&&planConsumedSeq.load(std::memory_order_acquire)==pendingSeq_){flightSeen_=false;enter(BrainState::Flying,now,"CONSUMED");}
        else if(factsOk_&&attached){catchNs_=now;bump(stats.catchPulses);pulse(hold_,.8f,60'000,now);bump(stats.caught);enter(BrainState::Caught,now,"CAUGHT");}
        else if(factsOk_&&(ls==lsInFlight||facts_.flight)){flightSeen_=false;enter(BrainState::Flying,now,"ENGINE_FLIGHT");}
        else if(now-launchEdgeNs_>msNs(config.planAgeMs)){bump(stats.timeouts);lastRefusal.store("LAUNCH_TIMEOUT",std::memory_order_relaxed);toHeld(now,"LAUNCH_TIMEOUT",false);}
        break;}
    case BrainState::Flying:{
        setAim(true);
        if(factsOk_&&facts_.flight)flightSeen_=true;
        if(factsOk_&&attached){
            catchNs_=now;bump(stats.catchPulses);pulse(hold_,.8f,60'000,now);bump(stats.caught);detachSince_=0;
            if(!toggle_&&in.grips[hh]<=gripOpen){bump(stats.openHandThrows);startGrace(now);}
            enter(BrainState::Caught,now,"CAUGHT");
        }else if(factsOk_&&(lassoMissed(ls)||userMissed(us)||(flightSeen_&&!facts_.flight&&ls!=lsInFlight))){
            bump(stats.missPulses);pulse(hold_,.25f,30'000,now);enter(BrainState::Recover,now,"MISS");setAim(false);
        }else if(now-launchEdgeNs_>msNs(4000)){bump(stats.flightCapHits);enter(BrainState::Recover,now,"FLIGHT_CAP");setAim(false);}
        break;}
    case BrainState::Caught:{
        if(!released_)setAim(true);
        if(factsOk_&&!attached){if(!detachSince_)detachSince_=now; // the engine let go (victim broke free / released): debounced 300 ms
            if(now-detachSince_>=msNs(300)){
                if(minigame&&(code==0||code==1||code==24)&&in.triggers[1]>=.5f)bump(stats.minigameRtReleases); // RT passed natively when the catch was lost
                drop(DropEngine);detachSince_=0;enter(BrainState::Recover,now,"DETACHED");faceRemapFlag.store(false,std::memory_order_relaxed);break;}}
        else detachSince_=0;
        if(suspended)break; // the latch holds through the menu: no release decision on a menu frame
        if(resumed&&!toggle_&&in.grips[hh]<=gripOpen&&!graceUntil_)startGrace(now);
        if(graceUntil_){
            if(in.grips[hh]>=gripClose){graceUntil_=0;bump(stats.regripSaved);noteTrace(unsigned(st_),facts_,0,0,"REGRIP",now);}
            else if(config.rope&&ropeGrabMask.load(std::memory_order_relaxed)){graceUntil_=0;bump(stats.regripSaved);bump(stats.graceSavedByRope);
                noteTrace(unsigned(st_),facts_,0,0,"REGRIP_ROPE",now);} // A hand on the rope saves the catch too
            else if(now>=graceUntil_){release(DropGrip,now,"GRACE_OVER");break;}
            else if(now>=nextGracePulse_){nextGracePulse_=now+msNs(250);pulse(hold_,.35f,20'000,now);}
        }
        if(toggle_){ // squeeze-and-open of the holding hand (closed >= 0.65, open within 600 ms, outside sockets) releases
            if(in.grips[hh]>=gripClose){if(!holdClosed_){holdClosed_=true;toggleClosedNs_=now;}}
            else if(in.grips[hh]<=gripOpen){
                if(holdClosed_&&now-toggleClosedNs_<=msNs(600)&&r.inside[hh]<0){
                    // A squeeze that grabbed the rope (hand over hand) is no release
                    if(config.rope&&ropeGrabNs[hh].load(std::memory_order_relaxed)+msNs(60)>=toggleClosedNs_){bump(stats.toggleGuarded);holdClosed_=false;break;}
                    bump(stats.toggleReleases);out.putAway=true;holdClosed_=false;release(DropGrip,now,"TOGGLE_RELEASE");break;}
                holdClosed_=false;}
        }
        break;}
    case BrainState::Recover:{
        setAim(attached);faceRemapFlag.store(false,std::memory_order_relaxed);
        if(factsOk_&&ls==lsDrawn&&us==usUnattached){pulse(hold_,.20f,20'000,now);toHeld(now,"ROPE_BACK",false);}
        else if(now-enterNs_>msNs(5000)){bump(stats.recoverTimeouts);toHeld(now,"RECOVER_TIMEOUT",false);}
        break;}
    default:break;
    }
    // the launch RT pulse (rtpulse frames, from the launch frame on; then RT 0)
    if(st_==BrainState::LaunchPending&&rtPulseLeft_){rt=1.f;if(--rtPulseLeft_==0)bump(stats.rtPulses);}
    faceRemapFlag.store(st_==BrainState::Caught&&!released_&&config.layout,std::memory_order_relaxed);
    // Minigame routing: triggers native only for the trigger prompts (0 L2, 1 R2, 24 alternating);
    // never the launch RT pulse there
    if(minigame&&(st_==BrainState::Caught||st_==BrainState::Recover||attached)){
        bump(stats.minigameFrames);
        if(code==0||code==1||code==24){lt=bdetail::clamp01(in.triggers[0]);rt=bdetail::clamp01(in.triggers[1]);bump(stats.minigameTriggerFrames);}
        else rt=lt=0.f;
    }else if(st_==BrainState::Caught||st_==BrainState::Recover||suspended)rt=lt=0.f; // an RT here would reach OnUseItem -> ReleaseAndReelInLasso
    { // Rope pull: a pull the tick turned into a pad yank (the minigame's prompted input, or .yankbtn) is pressed for yankPadFrames
        const auto ys=yankPadSeq.load(std::memory_order_acquire);
        if(ys!=yankSeen_){yankSeen_=ys;yankAct_=yankPadAction.load(std::memory_order_relaxed);
            yankLeft_=(st_==BrainState::Caught||attached)?config.yankPadFrames:0u;if(yankAct_==YankAlt)yankAltRt_=!yankAltRt_;}
        if(yankLeft_&&!suspended&&(st_==BrainState::Caught||attached)){--yankLeft_;
            switch(yankAct_){case YankA:pad.buttons|=runtime::PadA;break;case YankB:pad.buttons|=runtime::PadB;break;case YankX:pad.buttons|=runtime::PadX;break;
                case YankY:pad.buttons|=runtime::PadY;break;case YankRt:rt=1.f;break;case YankLt:lt=1.f;break;
                case YankL1:pad.buttons|=runtime::PadLeftGrip;break;case YankR1:pad.buttons|=runtime::PadRightGrip;break;
                case YankAlt:if(yankAltRt_)rt=1.f;else lt=1.f;break;default:break;}}
        else if(st_!=BrainState::Caught&&!attached)yankLeft_=0;
    }
    if(aimRequestFlag.load(std::memory_order_relaxed))bump(stats.aimRequestFrames);
    pad.triggers[0]=lt;pad.triggers[1]=rt;
    pad.buttons&=~(runtime::PadLeftTrigger|runtime::PadRightTrigger);
    if(lt>=.5f)pad.buttons|=runtime::PadLeftTrigger;
    if(rt>=.5f)pad.buttons|=runtime::PadRightTrigger;
    { // intent for the tick (the LASSO arc)
        Intent i{};i.ns=now;i.referenceSpace=in.referenceSpace;i.state=unsigned(st_);i.mode=mode_;i.throwHand=th;i.holdHand=hh;i.committed=committed_;
        i.commitSeq=commitSeq_;i.gaze=config.swingGaze;i.frozen=st_==BrainState::LaunchPending;
        i.arcWanted=config.arc!=ArcShow::Off&&(st_==BrainState::Ready||st_==BrainState::LaunchPending||(config.arc==ArcShow::Always&&st_==BrainState::Held));
        if(r.headValid)for(unsigned k=0;k<3;++k)i.headFwd[k]=r.headFwd[k];
        intentBox.store(i);
    }
    out.owned=true;return out;
}

inline bool Brain::keepHold(unsigned h,const ControllerSample& in,std::uint64_t now) noexcept {
    if(!active()||!inHand_||st_==BrainState::Off||int(h)!=hold_)return false;
    bool keep=false;
    switch(st_){
    case BrainState::Ready:keep=throwHand_==hold_;break; // one-handed throw: the release edge decides in route() this frame
    case BrainState::LaunchPending:keep=now-launchEdgeNs_<=msNs(config.planAgeMs);break;
    case BrainState::Flying:keep=now-launchEdgeNs_<=msNs(4000);break;
    case BrainState::Caught:
        if(released_){keep=false;break;}
        if(toggle_){keep=true;break;}
        // Rope pull: the holding hand opens to re-grab further along the rope (hand over hand) / both hands slack after a pull: kept
        if(ropeHolds(now)){openSince_=0;bump(stats.slackKeeps);keep=true;break;}
        if(graceUntil_){if(now<graceUntil_){keep=true;break;}release(DropGrip,now,"GRACE_OVER");keep=false;break;}
        if(in.triggers[h]>releaseTriggerGuard){openSince_=0;keep=true;break;}
        if(!openSince_){openSince_=now?now:1;keep=true;break;}
        if(now-openSince_<releaseSettleNs){keep=true;break;}
        release(DropGrip,now,"LET_GO");keep=false;break;
    default:break;
    }
    if(keep)bump(stats.keepHoldFrames);
    return keep;
}
inline bool Brain::ownsHand(unsigned h) const noexcept {
    if(!active()||!inHand_||st_==BrainState::Off||h>1)return false;
    if(int(h)==throwHand_&&throwHand_!=hold_){
        const int code=promptCodeFlag.load(std::memory_order_relaxed);
        if(minigameFlag.load(std::memory_order_relaxed)&&((code==2&&h==0)||(code==3&&h==1)))return false; // the prompted L1 / R1 passes
        return true;
    }
    if(config.rope&&st_==BrainState::Caught){ // Rope pull: a hand on the rope / the free hand never sends its grip (LB / RB) to the game
        const int code=promptCodeFlag.load(std::memory_order_relaxed);
        if(minigameFlag.load(std::memory_order_relaxed)&&((code==2&&h==0)||(code==3&&h==1)))return false;
        if(int(h)!=hold_||(ropeGrabMask.load(std::memory_order_relaxed)&(1u<<h)))return true;
    }
    return int(h)==hold_&&toggle_&&st_==BrainState::Caught;
}
inline bool Brain::dwellRequired() const noexcept {
    if(!active()||!inHand_)return false;
    switch(st_){case BrainState::Ready:case BrainState::LaunchPending:case BrainState::Flying:case BrainState::Caught:return true;
        case BrainState::Recover:return aimRequestFlag.load(std::memory_order_relaxed);default:return false;}
}
// Free functions for the holsters
inline bool keepHold(unsigned h,const ControllerSample& in,std::uint64_t now) noexcept {return brain.keepHold(h,in,now);}
inline bool ownsHand(unsigned h) noexcept {return brain.ownsHand(h);}
// The NOT OWNED note stays 3 s after a dead-zone squeeze.
inline void noteNotOwned(std::uint64_t now) noexcept {notOwnedNs.store(now?now:1,std::memory_order_relaxed);}
inline void resetForTests() noexcept {
    brain.reset();aimRequestFlag.store(false);faceRemapFlag.store(false);minigameFlag.store(false);promptCodeFlag.store(32);ropeBusyFlag.store(false);
    stateFlag.store(0);holdHandFlag.store(-1);throwHandFlag.store(-1);arcUsable.store(true);forceAimMirror.store(false);notOwnedNs.store(0);brainNs.store(0);
    taught.store(false);planConsumedSeq.store(0);directRequestSeq.store(0);directDoneSeq.store(0);lastRefusal.store("NONE");traceHead.store(0);
    ropeGrabMask.store(0);ropeEngagedNs.store(0);for(auto& g:ropeGrabNs)g.store(0);yankPadSeq.store(0);yankPadAction.store(-1);
    playerBox.store(PlayerLink{});engineBox.store(EngineFacts{});intentBox.store(Intent{});shownBox.store(ShownBox{});planBox.store(Plan{});
    stats.~Stats();::new(&stats) Stats{};
}
} // namespace rdrvr::vr::lasso
