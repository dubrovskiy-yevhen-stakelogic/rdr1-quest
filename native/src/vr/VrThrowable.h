#pragma once
// THROWABLE: the tomahawk, throwing knives, dynamite and fire bottles (engine classes THROWN 7 / THROWN_EXPLODING 8) thrown with the VR
// hand THROUGH THE GAME'S OWN THROW: the projectile is the engine's (damage, sticking, fuse, fire, inventory count), the mod only decides
// WHEN (an RT pulse) and WHERE (the launch position / velocity of that projectile).
//
// Engine path (librdr 1.58, verified in the disassembly):
//   RT with a thrown weapon -> AimDoThrowTask::Begin 0x1d02594 -> aniAim::ScheduleThrow 0x1cba430 -> aniAim::Throw 0x1cba580 (the throw clip)
//   + invInventoryComponent::ThrowWeapon 0x2669ae0 -> invWeapon::ThrowWeapon 0x2676bb4; at the clip's release weapWeapon::OnFireProjectile
//   0x2541ea8 -> weapWeapon::FireProjectile 0x2546f38 -> projProjectile::Launch 0x2532bc8 (call site 0x2547ab4; x1 Matrix34, x3 velocity,
//   x5 muzzle). Launch copies the velocity to proj+0x1e0 / +0x1f0 and the matrix to +0x20 / position +0x1c0.
//   Gravity of a simple projectile: projProjectile::UpdateSimple 0x2534fe4: v.y -= projManager::GetGravity() (9.8, 0x253bdb0) * dt *
//   type+0x28 (projProjectileType at proj+0x288). The mod's Launch interposer (TraceEntry launchProjectile, already pinned to the
//   FireProjectile call site) substitutes matrix / velocity / muzzle for the local player's THROWN / THROWN_EXPLODING launch while a VR
//   throw waits for it.
//
// Gestures (the throwable held in a VR hand, HOLD grip):
//   PHYSICAL: open the grip during a fast swing -> the throw flies along the hand's release velocity x gain (the THROW / SPIN release
//     estimator: the XR-rate grip tape, peak-centred). A slow toss whose path passes the other (empty) hand is a HAND TOSS instead: the
//     gun THROW visual flight + catch (VrThrow.h, slot THROWN) - the item stays in the inventory and the other hand catches it.
//   AIMED: hold the holding hand's trigger -> the throw arc + landing marker (ArcChannel::Throw, the shared tracer with the engine's own
//     gravity once it is known); release the trigger -> it flies along the shown arc.
//   Hand to hand: the DUAL transfer (the free hand closes on the item) is class-agnostic; for a throwable its zone is the item
//     along the holding fist (handle / head / bottle, not only the gun point 6 cm ahead) and it wins over a socket under the hand (the
//     chest THROWN socket sits right where the hands meet).
//   TOSS: a release slower than MIN SWING is never
//     the game's throw; a gentle toss (>= 1 m/s, not downward, not in the socket, not on a horse) flies VISUALLY while the item stays the
//     engine's item in the throwing hand (no projectile, no inventory change): TossBrain (input thread, LOCAL ballistic + magnet + catch),
//     ModuleThrowable flies the held model along it (pose hook, the same override as the pre-launch flight). A fresh squeeze at the item
//     catches it: the throwing hand re-adopts it, the other hand takes it (OpTransfer); missed / hit a surface / 1.6 s = the plain put-away.
// While a throwable is in a VR hand the holding trigger never reaches the game (it is the aim gesture); RT is only the throw pulse.
// The pre-launch gap (the throw clip until its release frame) is bridged visually: the held model flies the VR arc (pose hook) and the
// engine projectile is launched from where that flight is at the launch frame (a surface hit before it = launched from the hit).
//
// Threads: input thread (holsters InputBrain: Brain::route / physical), engine main thread (ModuleThrowable tick in VrThrowableTick.h:
// arc, flight, request -> world), the Launch interposer (main thread, inside FireProjectile), weapon pose hooks (any thread: arcBox).
// Levers: the THROWING menu page (VrSettings.h keys Throw*) + startup properties debug.rdrvr.vr.throwable* (startup(); a property that is set
// overrides its menu value: .gain = THROW POWER, .minspeed = MIN SWING, .spin = THROW SPIN, .aimspeed = AIMED SPEED).
// THROWN_EXPLODING: .explodeattempts / .explodewait / .explodecheck / .explodeblind / .explodehold / .explodeextra (RT press chain),
// .lighttap 0 (a tap aims + throws again) / .lighttapms; report vr.throwable.explode (engine aim + per kind swings / presses / starts).
#include "VrMainThread.h"
#include "VrHaptics.h"
#include "VrThrow.h"
#include "VrSettings.h" // THROWING page (menu values; a set debug property wins)
#include "runtime/ControllerInput.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace rdrvr::vr::throwable {
namespace T=::rdrvr::vr::throwing;
using runtime::ControllerSample;using runtime::PadState;
using V3=T::V3;using M3=T::M3;using M4=T::M4;

// engine::WeaponClass labels (WeaponTypeReader.h): THROWN = tomahawk / throwing knives, THROWN_EXPLODING = dynamite / fire bottle.
inline constexpr int kindThrown=7,kindExploding=8;
inline constexpr bool throwableKind(int k) noexcept {return k==kindThrown||k==kindExploding;}
inline constexpr int slotThrown=2; // holsters::SlotThrown (chest centre)

enum class AimMode : int {Held=0,Trigger=1,Off=2};
inline constexpr std::array<const char*,3> aimModeNames{"held","trigger","off"};
struct Config {
    bool enabled{true},launchOverride{true},visual{true},toss{true};
    // downDot .7 (= THROW's; .9 let a brisk downward put-away of dynamite leave at 3.7 m/s and blow up at John's feet);
    // THROWN_EXPLODING also never leaves more than 30 deg below the horizon (explodeDownDot .5 = sin 30)
    float gain{1.5f},maxSpeed{25.f},minSpeed{2.f},releaseSpeed{1.2f},downDot{.7f},explodeDownDot{.5f}; // physical: m/s (LOCAL), down share = a drop
    // an AIMED release is decided relWaitNs after the trigger let go (= the holsters' release settle): in HOLD the hand let go
    // of the item by then (grip <= openGrip = holsters gripOpen) or is clearly opening (openDrop below its peak since the trigger let go) =
    // a put-away, never a throw (a full fist lets the trigger go a frame before the grip); TOGGLE (item in an open hand) always throws
    float openGrip{.20f},openDrop{1.01f}; // only a hand really open cancels; openDrop off
    float aimSpeed{14.f},spin{1.f},tossSpeed{3.5f},tossRadius{.45f};               // aimed m/s, visual spin gain, hand toss limits
    float gravityMps2{9.8f};                                                         // until an engine launch told the type's scale
    // aim TRIGGER (default): force aim only while aiming and until a throw launched (+ settle) - the in-hand fuse timer of dynamite starts from
    // aniAim::SetWeaponStateChange 0x1cba8f8 (WeaponStateChangeTask), so a permanent aim stance with dynamite is avoided; HELD = always
    unsigned rtPulse{3};AimMode aim{AimMode::Trigger};
    std::uint32_t props{}; // PropGain.. = that lever was set by a property (overrides the menu)
    // RT pulse once the engine aim has been up aimLeadNs (the lasso needs ~200 ms of aim before its RT launch), at the latest after aimWaitNs
    // minAimNs: a debounce only (the ownership latch blocks a carried-over squeeze; a quick tap is a quick throw, as the native RT was)
    std::uint64_t aimLeadNs{150'000'000},aimWaitNs{400'000'000},maxDelayNs{1'500'000'000},settleNs{350'000'000},minAimNs{30'000'000};
    std::uint64_t relWaitNs{150'000'000};
    // TOSS: qualifies from tossMinSpeed peak / tossReleaseSpeed at the release (the gun THROW's 1.0 / 0.6), never below tossDownDot
    // (30 deg down = a put-away); catchRadius around the item (+-60 ms), magnet within pullRadius (pullRate 1/s, pullMaxNs in total),
    // missed after tossMaxNs or tossDropM below the release height; the throwing hand catches after tossGraceNs
    float tossMinSpeed{1.f},tossReleaseSpeed{.6f},tossDownDot{.5f},tossMaxMps{4.f},catchRadius{.30f},pullRadius{.12f},pullRate{4.f},tossDropM{1.2f};
    std::uint64_t tossMaxNs{1'600'000'000},tossGraceNs{200'000'000},pullMaxNs{700'000'000};
    // a THROWN_EXPLODING throw presses RT up to explodeAttempts times (1 tap, 2 held explodeHoldNs, 3 tap) until the engine starts
    // the throw (aniAim state 13 / the launch); each press waits for the engine aim to take a throw (aniAim::ScheduleThrow's own refusals,
    // EngineAim below) or explodeReadyWaitNs; a press without a start is retried after explodeCheckNs (explodeBlindNs when the engine aim is
    // unreadable); explodeExtraNs = the longer launch wait of that chain. lightTap: a holding-trigger tap < lightTapNs on an unlit
    // THROWN_EXPLODING lights it (the aim stance kept while held = the game's own lighting) instead of an aimed throw
    unsigned explodeAttempts{3};bool lightTap{true};
    std::uint64_t explodeReadyWaitNs{1'000'000'000},explodeCheckNs{350'000'000},explodeBlindNs{800'000'000},explodeHoldNs{450'000'000},
        explodeExtraNs{1'500'000'000},lightTapNs{250'000'000};
};
inline Config config{};
enum : std::uint32_t {PropGain=1,PropMinSpeed=2,PropRelease=4,PropSpin=8,PropAimSpeed=16};
inline std::atomic<bool> started{},killed{};
inline bool active() noexcept {return started.load(std::memory_order_acquire)&&!killed.load(std::memory_order_relaxed)&&config.enabled&&settings().throwablesVr();}
// Effective tuning: the THROWING page unless a property set the lever.
inline float gain() noexcept {return config.props&PropGain?config.gain:settings().throwPower();}
inline float minSpeed() noexcept {return config.props&PropMinSpeed?config.minSpeed:settings().throwMinSwing();}
inline float releaseSpeed() noexcept {return config.props&PropRelease?config.releaseSpeed:.6f*minSpeed();}
inline float spinGain() noexcept {return config.props&PropSpin?config.spin:settings().throwSpin();}
inline float aimSpeed() noexcept {return config.props&PropAimSpeed?config.aimSpeed:settings().throwAimSpeed();}
inline float gravityFactor() noexcept {return settings().throwGravity();}  // the projectile type's gravity scale x this (GameXr writes it)
inline float assist() noexcept {return settings().throwAssist();}
inline bool showArc() noexcept {return settings().throwShowArc();}

// ---- shared state ----
enum Mode : std::uint32_t {ModeNone=0,ModePhysical=1,ModeAimed=2};
inline constexpr std::array<const char*,3> modeNames{"NONE","PHYSICAL","AIMED"};
// A throw decided on the input thread (LOCAL metres of referenceSpace). Physical: v / w = the release estimate (gain applied by the tick).
struct Request {std::uint64_t seq{},ns{},referenceSpace{};V3 v{},w{};std::int32_t hand{-1};std::uint32_t mode{};float speedPeak{},speedRelease{};
    std::uint32_t flags{},pad{};};
static_assert(sizeof(Request)%8==0);
inline T::Box<Request> requestBox{};
inline std::atomic<std::uint64_t> requestSeq{},launchedSeq{},launchedNs{},expiredSeq{};
// the hand that OPENED on a physical throw, until the engine launched it (+ settle) or it expired - the reins brain may take
// that hand back (GameXr reins input: not busy although the engine item is still in it), the holsters never re-adopt the item into it
inline std::atomic<int> releasedHand{-1};inline std::atomic<std::uint64_t> releasedSeq{},releasedNs{};
inline std::atomic<int> aimHand{-1};          // the hand showing the aim arc (input thread)
inline std::atomic<bool> aimWanted{};         // input thread -> holsters poll: force aim for the throwable
inline std::atomic<int> heldHand{-1};         // input thread: the VR hand holding a throwable (AIM ASSIST target sampling)
// The pre-launch flight of the held model (main thread writes, pose hooks read): state != 0 while weapon w flies.
enum FlightState : std::uint32_t {FlightIdle=0,FlightFlying=1,FlightStuck=2};
inline T::Box<T::Arc> arcBox{};
inline std::atomic<std::uintptr_t> flightWeapon{};
inline bool poseActive(std::uintptr_t w) noexcept {return w&&flightWeapon.load(std::memory_order_acquire)==w;}
// the kind of the last posted throw (a THROWN_EXPLODING chain waits explodeExtraNs longer for its launch)
inline std::atomic<int> pendingKind{-1};
inline std::uint64_t maxDelayNs() noexcept {return config.maxDelayNs+(pendingKind.load(std::memory_order_relaxed)==kindExploding?config.explodeExtraNs:0ull);}
inline constexpr unsigned kindIndex(int k) noexcept {return k==kindExploding?1u:0u;}
// ENGINE AIM of the local player while a throwable is in a VR hand (holsters EngineSide poll, main thread; librdr 1.58 disassembly):
// actor+0x38 animator -> +0xab8 -> +0x8 aniAim (aniAim+0x8 = the animator back). aniAim::ScheduleThrow 0x1cba430 (AimDoThrowTask::Begin's
// call) REFUSES a throw while aniAim+0x3bc (state) is 0..4 or 13 (= throwing, aniAim::Throw 0x1cba580 writes 13), while actor+0x118 bit2,
// and with actor+0x118 bit0 while the arm-up blend aniAim+0x404 > 0 or (actor+0xb8 reticle)+0x6090 != 0. The fuse: aniAim+0x5e9 (u16) bit
// 0x4000 = the weapon state changed (aniAim::SetWeaponStateChange 0x1cba8f8: set -> weapWeapon::StartExplodeTimer, clear -> Stop for types
// 0x18 / 0x25); aniAim::ScheduleDraw 0x1cbeba8 sets it at the draw for weapon-type class 3 except types 0x18 / 0x25 (lit at the draw),
// the others are lit by the action tree (WeaponStateChangeTask 0x1cf0dc8). aniAim+0x5d4 bit 9 = a throw scheduled. typeId / typeClass =
// (weapon+0x28)+0x8 (s16) / +0x24 (report only: which item is which is NOT verified on the device).
struct EngineAim {std::uint64_t ns{},flags{};std::int32_t state{-1};float armUp{};std::uint32_t actorFlags{};std::uint16_t stateChange{};
    std::int16_t typeId{-1};std::int32_t typeClass{-1};std::uint8_t reticleBlock{},valid{},ownerMatch{},pad1{};};
static_assert(sizeof(EngineAim)%8==0);
inline T::Box<EngineAim> engineAimBox{};
inline std::atomic<std::uint64_t> engineThrowStartNs{},engineThrowStarts{},engineAimReads{},engineAimFailures{};
// |now - read| (the input thread's now is its sample's publish time - a main-thread read stamped after it is fresh, not
// unreadable: that one-frame miss dropped the lit stance's force aim for a frame)
inline bool engineAim(EngineAim& e,std::uint64_t now) noexcept {
    return engineAimBox.load(e)&&e.valid&&e.ns&&(now>=e.ns?now-e.ns:e.ns-now)<=150'000'000ull;}
inline bool aimLit(const EngineAim& e) noexcept {return (e.stateChange&0x4000u)!=0;}
// lit AT ITS DRAW by the engine (aniAim::ScheduleDraw 0x1cbeba8: type class 3 except types 0x18 / 0x25 - those are lit by
// the aim and cleared by SetWeaponStateChange(false) + StopExplodeTimer). Only such an item keeps the stance from the engine flag alone:
// a 0x4000 left over from the previous stick never raises the force aim (= lights) a freshly grabbed one. Unread type = never.
inline bool drawLit(const EngineAim& e) noexcept {return aimLit(e)&&e.typeClass==3&&e.typeId>=0&&e.typeId!=0x18&&e.typeId!=0x25;}
inline const char* aimRefusal(const EngineAim& e) noexcept { // nullptr = ScheduleThrow takes a throw now
    if(e.state<5)return "AIM_STATE";if(e.state==13)return "THROWING";if(e.actorFlags&4u)return "ACTOR_BIT2";
    if(e.actorFlags&1u){if(e.armUp>0.f)return "ARM_UP";if(e.reticleBlock)return "RETICLE";}
    return nullptr;}
inline bool aimReady(const EngineAim& e) noexcept {return !aimRefusal(e);}

// ---- counters (report) ----
using Counter=std::atomic<std::uint64_t>;
inline void bump(Counter& c,std::uint64_t n=1) noexcept {c.fetch_add(n,std::memory_order_relaxed);}
inline void maxOf(Counter& c,std::uint64_t v) noexcept {auto m=c.load(std::memory_order_relaxed);while(v>m&&!c.compare_exchange_weak(m,v)){}}
struct Stats {
    Counter routeFrames{},aimStarts{},aimThrows{},aimTooShort{},aimCancelled{},physicalThrows{},handTosses{},slow{},glitch{},down{},socket{},
        noTape{},pulses{},pulseWaitTimeouts{},busy{},pulsesDropped{},aimCarried{},aimOpenCancels{};
    Counter startStale{};
    Counter ticks{},requestsSeen{},startFailures{},mapFailures{},flights{},flightProbeHits{},expired{},arcPublished{},arcHidden{},
        noShown{},forceAimRequests{};
    Counter launchCalls{},launchesOverridden{},launchesNative{},launchesStale{},launchDeclined{},launchDelayMsSum{},launchDelayMsMax{}; Counter dirOnlyArmed{},launchesDirOnly{};
    Counter poseCalls{},posePlaced{},poseDeclined{};
    Counter assistProbes{},assistTargets{},assistApplied{},assistUnreachable{},gravityWrites{},gravityRefused{};
    // TOSS / transfer
    Counter tossUp{},tossStarts{},tossRefused{},tossCaughtSame{},tossCaughtOther{},tossMissed{},tossCancelled{},tossHits{},tossTimeouts{},
        tossPulls{},tossFarPresses{},tossVisuals{},tossNoStart{},transferZone{};
};
inline Stats stats;
// per kind [THROWN, THROWN_EXPLODING]: swings = HOLD grip opened on the item (physical()), releases = posted physical throws, the
// blocked reasons; presses = RT presses sent (ready / refused = the engine aim took throws then or not, blind = unreadable), started = the engine started the
// throw after this throw's first press (startedOn[attempt]), noStart = the chain ended without a start; launches = Launch calls of the kind
// (litLaunches = with the fuse flag set); lightTaps / litSeen = the trigger tap lit request / the fuse flag 0 -> 1 while held
struct KindStats {Counter swings{},releases{},slow{},down{},socket{},glitch{},busy{},tosses{},toOther{},presses{},pressesReady{},pressesRefused{},pressesBlind{},
    holdPresses{},started{},noStart{},launches{},litLaunches{},lightTaps{},litSeen{};std::array<Counter,4> startedOn{};};
inline std::array<KindStats,2> perKind{};
inline std::array<std::atomic<const char*>,2> lastBlock{}; // the last swing's outcome per kind
// The engine aim at the last RT press per kind (refusal = aimRefusal then, "UNREAD" without a fresh read)
struct PressNote {std::uint64_t ns{};std::int32_t state{-1},attempt{};float armUp{};std::uint32_t lit{},ready{},valid{},hold{},pad{};};
static_assert(sizeof(PressNote)%8==0);
inline std::array<T::Box<PressNote>,2> lastPress{};
inline std::array<std::atomic<const char*>,2> lastPressRefusal{};
inline void noteBlock(int kind,const char* why) noexcept {lastBlock[kindIndex(kind)].store(why,std::memory_order_relaxed);}
struct LastLaunch {std::uint64_t ns{},seq{};V3 nativeV{},v{},nativeP{},p{};float gravityScale{},delayMs{};std::uint32_t mode{},kind{};};
static_assert(sizeof(LastLaunch)%8==0);
inline T::Box<LastLaunch> lastLaunchBox{};
inline std::atomic<float> learnedGravityScale{0.f}; // ORIGINAL type+0x28 of the last thrown launch (0 = none yet)
inline std::atomic<float> lastSpeed{},lastNativeSpeed{};
// the game throws the tomahawk at 50 world units/s with type gravity x2, so a hand-speed throw (8.5 m/s) is a lob into the ground: the SPEED is the
// game's own throw speed of that kind (learned at every launch, default 40 m/s) x THROW POWER (1.5 = 100 %), the VR hand gives the
// direction (and, physical, the swing strength: hand speed / fullSwingMps, .35..1.2)
inline std::array<std::atomic<float>,2> nativeSpeed{}; // world units/s, [THROWN, THROWN_EXPLODING]
inline std::atomic<int> heldKindNow{-1};                // input thread: the kind of the held throwable
inline constexpr float defaultNativeMps=40.f,fullSwingMps=6.f;
inline float nativeLaunchSpeed(int kind,float upm) noexcept {
    const float s=kind==kindThrown||kind==kindExploding?nativeSpeed[kind==kindExploding?1u:0u].load(std::memory_order_relaxed):0.f;
    return (s>1.f&&std::isfinite(s)?s:defaultNativeMps*upm)*std::clamp(gain()/1.5f,.3f,3.f);}
inline float gravityMps2() noexcept {const float s=learnedGravityScale.load(std::memory_order_relaxed);return (s>0.f?9.8f*s:config.gravityMps2)*gravityFactor();}
// Any thread: hand `hand` opened on a physical throw that has not launched (+ settleNs) / expired yet.
inline bool releasePending(unsigned hand,std::uint64_t now) noexcept {
    if(releasedHand.load(std::memory_order_acquire)!=int(hand))return false;
    const auto seq=releasedSeq.load(std::memory_order_relaxed),at=releasedNs.load(std::memory_order_relaxed);
    if(!seq)return false;
    if(launchedSeq.load(std::memory_order_acquire)>=seq){const auto l=launchedNs.load(std::memory_order_relaxed);return now>=l&&now-l<config.settleNs;}
    return now>=at&&now-at<=maxDelayNs()+config.settleNs;
}
// GameXr glue (ModuleThrowable tick, main thread, every frame): THROW GRAVITY restored / re-applied on the types the mod wrote.
using GravitySyncFn=void(*)(bool active,float factor) noexcept;
inline std::atomic<GravitySyncFn> gravitySyncFn{};

// ---- TOSS: the visual hand toss of the held throwable (input thread decides, ModuleThrowable flies the model) ----
// LOCAL metres of referenceSpace: p(t) = p + v (t - t0) + 1/2 (0, g, 0) (t - t0)^2; re-seated (t0 = now) by the magnet.
enum TossState : std::uint32_t {TossIdle=0,TossFlying=1,TossCaughtSame=2,TossCaughtOther=3,TossMissed=4,TossCancelled=5};
inline constexpr std::array<const char*,6> tossStateNames{"IDLE","FLYING","CAUGHT_SAME","CAUGHT_OTHER","MISSED","CANCELLED"};
struct Toss {std::uint64_t seq{},t0{},startNs{},endNs{},referenceSpace{};V3 p{},v{},w{};float g{};std::int32_t hand{-1},catchHand{-1};std::uint32_t state{};};
static_assert(sizeof(Toss)%8==0);
inline T::Box<Toss> tossBox{};
inline std::atomic<std::uint64_t> tossSeq{},tossHitSeq{},tossFailSeq{}; // main thread: the toss hit a surface / has no visual start pose
// Pose hook relation of the tossed weapon (main thread writes at the toss start): attachment = model * k.
struct PoseK {std::uintptr_t weapon{};std::uint64_t ns{};M4 k{};};
static_assert(sizeof(PoseK)%8==0);
inline T::Box<PoseK> poseKBox{};
inline std::atomic<std::uintptr_t> tossWeapon{};
// GameXr glue (main thread): the held throwable of VR hand `hand` NOW - its model (world) and the attachment relation k, from the freshest
// of the BODY native hold / left hold (recorded by the pose hook) and the VR placement. False = no fresh pose (the toss is cancelled).
struct TossStart {M4 model{},k{};std::uintptr_t weapon{};std::uint64_t ns{};std::uint32_t actorGuid{},pad{};};
using TossStartFn=bool(*)(int hand,std::uint64_t now,TossStart& out) noexcept;
inline std::atomic<TossStartFn> tossStartFn{};

class TossBrain {
    Toss t_{};bool flying_{},pinned_{},pulled_{};std::uint64_t lastNs_{},pullUsedNs_{},endNs_{};float startY_{};std::uint32_t endState_{};int lastHand_{-1};
    void publish() noexcept {tossBox.store(t_);tossSeq.store(t_.seq,std::memory_order_release);}
    static float dtOf(std::uint64_t t0,std::uint64_t now) noexcept {return now>t0?std::fmin(float(double(now-t0)*1e-9),3.f):0.f;}
public:
    bool flying() const noexcept {return flying_;}
    int hand() const noexcept {return flying_?t_.hand:-1;}
    V3 at(std::uint64_t now) const noexcept {const float dt=dtOf(t_.t0,now);
        return {t_.p[0]+t_.v[0]*dt,t_.p[1]+t_.v[1]*dt+.5f*t_.g*dt*dt,t_.p[2]+t_.v[2]*dt};}
    V3 velocityAt(std::uint64_t now) const noexcept {const float dt=dtOf(t_.t0,now);return {t_.v[0],t_.v[1]+t_.g*dt,t_.v[2]};}
    bool graceOver(std::uint64_t now) const noexcept {return now>=t_.startNs&&now-t_.startNs>=config.tossGraceNs;}
    // The throwing hand's put-away waits: while the toss flies (self-expiring: a suspended input thread never strands it) and 600 ms after
    // the other hand caught it (its OpTransfer lands; refused = the put-away goes on)
    bool holds(unsigned h,std::uint64_t now) const noexcept {
        if(flying_)return t_.hand==int(h)&&now>=t_.startNs&&now-t_.startNs<=config.tossMaxNs+300'000'000ull;
        return endState_==TossCaughtOther&&lastHand_==int(h)&&now>=endNs_&&now-endNs_<600'000'000ull;}
    // the toss of hand h was missed < 1 s ago - its put-away is not cancelled by a fist still closed from a masked far press
    // (the RESQUEEZE re-adopted the fallen item: it fell for 1 s, then jumped back into the hand)
    bool missed(unsigned h,std::uint64_t now) const noexcept {
        return !flying_&&endState_==TossMissed&&lastHand_==int(h)&&now>=endNs_&&now-endNs_<1'000'000'000ull;}
    // Hand position q (LOCAL) at the flying item (now, +-60 ms).
    bool near(const V3& q,std::uint64_t now,float radius) const noexcept {
        if(!flying_)return false;
        const std::uint64_t w=60'000'000ull;for(const std::uint64_t t:{now,now+w,now>w?now-w:now})if(T::math::length(T::math::sub(q,at(t)))<=radius)return true;
        return false;}
    bool begin(unsigned h,const ControllerSample& in,const T::ReleaseEstimator::Estimate& e,std::uint64_t now) noexcept {
        using namespace T::math;
        if(h>1||!in.grip[h].positionValid||!e.valid||!finite(e.v))return false;
        V3 v=mul(e.v,T::config.gain);if(length(v)>config.tossMaxMps)v=mul(normalized(v),config.tossMaxMps);
        V3 w=mul(e.w,T::config.spin);if(!finite(w))w={};if(length(w)>T::config.maxSpin)w=mul(normalized(w),T::config.maxSpin);
        const std::uint64_t seq=t_.seq+1;t_=Toss{};t_.seq=seq;t_.t0=t_.startNs=now;t_.referenceSpace=in.referenceSpace;
        t_.p={in.grip[h].position[0],in.grip[h].position[1],in.grip[h].position[2]};t_.v=v;t_.w=w;t_.g=T::config.gravity();
        t_.hand=int(h);t_.state=TossFlying;
        flying_=true;pinned_=false;pulled_=false;lastNs_=now;pullUsedNs_=0;startY_=t_.p[1];endState_=TossIdle;lastHand_=int(h);
        publish();bump(stats.tossStarts);haptics::pulse(h,.25f,15'000,now,haptics::SourceReturn);return true;
    }
    void end(TossState s,std::uint64_t now,int catcher=-1) noexcept {
        if(!flying_)return;
        if(s==TossCaughtOther||s==TossCaughtSame){t_.p=at(now);t_.v={};t_.g=0.f;t_.w={};t_.t0=now;}
        flying_=false;pinned_=false;t_.state=s;t_.endNs=now;t_.catchHand=catcher;endState_=s;endNs_=now;lastHand_=t_.hand;publish();
        bump(s==TossCaughtSame?stats.tossCaughtSame:s==TossCaughtOther?stats.tossCaughtOther:s==TossMissed?stats.tossMissed:stats.tossCancelled);
    }
    // Every usable input frame: the end conditions and the magnet (bit h of freeMask = hand h may catch; the throwing hand once its grace
    // is over and the item comes down).
    void update(const ControllerSample& in,std::uint32_t freeMask,int hold,std::uint64_t now) noexcept {
        using namespace T::math;
        if(!flying_)return;
        if(hold!=t_.hand||in.referenceSpace!=t_.referenceSpace){end(TossCancelled,now);return;}
        if(tossFailSeq.load(std::memory_order_acquire)==t_.seq){bump(stats.tossNoStart);end(TossCancelled,now);return;}
        if(tossHitSeq.load(std::memory_order_acquire)==t_.seq){bump(stats.tossHits);end(TossMissed,now);return;}
        if(now<t_.startNs||now-t_.startNs>config.tossMaxNs){bump(stats.tossTimeouts);end(TossMissed,now);return;}
        const float frameDt=lastNs_&&now>lastNs_?std::fmin(float(double(now-lastNs_)*1e-9),.05f):.011f;lastNs_=now;
        const V3 p=at(now);
        if(p[1]<startY_-config.tossDropM){end(TossMissed,now);return;}
        const V3 vNow=velocityAt(now);
        int best=-1;float bd=config.pullRadius;V3 bp{};
        for(unsigned h=0;h<2;++h){
            if(!(freeMask>>h&1u)||!in.grip[h].positionValid)continue;
            if(int(h)==t_.hand&&(!graceOver(now)||(!pinned_&&vNow[1]>0.f)))continue; // the throwing hand: only once it comes down
            const V3 q{in.grip[h].position[0],in.grip[h].position[1],in.grip[h].position[2]};const V3 to=sub(q,p);const float d=length(to);
            if(d<bd&&(d<.15f||dot(vNow,to)>0.f||pinned_)){bd=d;best=int(h);bp=q;}
        }
        if(best>=0&&pullUsedNs_<config.pullMaxNs){
            pullUsedNs_+=std::uint64_t(double(frameDt)*1e9);if(!pulled_){pulled_=true;bump(stats.tossPulls);}
            if(bd<=.08f){t_.p=bp;t_.v={};t_.g=0.f;t_.w={};t_.t0=now;pinned_=true;} // in the palm: it rests there until the hand closes
            else{const V3 to=mul(sub(bp,p),1.f/bd);const float sp=std::fmax(1.5f,length(vNow));const float k=std::fmin(1.f,config.pullRate*frameDt);
                t_.p=p;t_.v=add(vNow,mul(sub(mul(to,sp),vNow),k));t_.g=.3f*T::config.gravity();t_.t0=now;}
            publish();
        }else if(pinned_||pulled_){ // out of the magnet (or its time is up): it falls again
            t_.p=p;t_.v=pinned_?V3{0,0,0}:vNow;t_.g=T::config.gravity();t_.t0=now;pinned_=false;pulled_=false;publish();
        }
    }
};

// =====================================================================================================================
// Input-thread brain (one per holsters InputBrain).
// toggle = WEAPON GRIP TOGGLE (the item stays in an open hand); reins = the reins brain holds the holding hand (it may take the
// throwing hand back while the engine still finishes the throw) - its trigger is the rein squeeze then, never an aim
struct RouteIn {const ControllerSample* in{};int hold{-1},kind{-1};bool menu{},aimUp{},toggle{},reins{},tossing{};std::uint64_t now{};};
class Brain {
    bool down_{},aiming_{},pulseWait_{};unsigned pulseLeft_{};std::uint64_t aimSince_{},pulseFrom_{},aimUpSince_{};
    std::uint64_t pendingSeq_{},pendingNs_{};int pendingHand_{-1};
    // ownership of the current hold (hand + kind): its first frame latches a carried-over squeeze; relPending_ = an AIMED
    // trigger release waiting relWaitNs for the grip (a put-away or a throw)
    bool pendingPhysical_{};
    bool owned_{},relPending_{};int ownHold_{-1},ownKind_{-1};std::uint64_t ownNs_{},relNs_{};float relPeak_{};
    // the RT press chain of the pending throw (attempt_ presses so far, holdUntil_ = a held press, checking_ = waiting for the engine
    // start since checkFrom_), firstPressNs_ = its first press; litStance_ = a LIGHT tap keeps the aim stance (the fuse burns in the hand)
    int pendingKind_{-1};unsigned attempt_{};bool checking_{},litStance_{},inPress_{};std::uint64_t holdUntil_{},checkFrom_{},firstPressNs_{};
    std::uint64_t litAfterSeq_{}; // the request seq at the LIGHT tap - only a LATER throw's launch ends the stance
    static constexpr std::uint64_t kindHoldNs=300'000'000,pulseLateNs=100'000'000;
    void stopAim(bool cancelled) noexcept {if(aiming_&&cancelled)bump(stats.aimCancelled);aiming_=false;aimHand.store(-1,std::memory_order_relaxed);}
    bool pressing() const noexcept {return pulseLeft_||holdUntil_;}
    void endChain() noexcept {pulseWait_=false;pulseLeft_=0;holdUntil_=0;checking_=false;inPress_=false;}
    std::uint64_t waitNs() const noexcept {return pendingKind_==kindExploding?config.explodeReadyWaitNs:config.aimWaitNs;}
    // One RT press of the pending throw (attempt 2 of a THROWN_EXPLODING chain is held explodeHoldNs: a 'hold to light, release to throw').
    void press(std::uint64_t now,bool timedOut) noexcept {
        ++attempt_;if(attempt_==1)firstPressNs_=now;pulseLeft_=config.rtPulse;checking_=false;inPress_=true;
        holdUntil_=pendingKind_==kindExploding&&attempt_==2&&config.explodeAttempts>1?now+config.explodeHoldNs:0;
        auto& K=perKind[kindIndex(pendingKind_)];bump(stats.pulses);bump(K.presses);if(holdUntil_)bump(K.holdPresses);
        EngineAim ea{};const bool eng=engineAim(ea,now);bump(eng?(aimReady(ea)?K.pressesReady:K.pressesRefused):K.pressesBlind);
        PressNote n{};n.ns=now;n.state=ea.state;n.attempt=int(attempt_);n.armUp=ea.armUp;n.lit=eng&&aimLit(ea);n.ready=eng&&aimReady(ea);
        n.valid=eng;n.hold=holdUntil_?1u:0u;n.pad=timedOut?1u:0u;lastPress[kindIndex(pendingKind_)].store(n);
        lastPressRefusal[kindIndex(pendingKind_)].store(!eng?"UNREAD":aimRefusal(ea)?aimRefusal(ea):"OK",std::memory_order_relaxed);
    }
    // The engine started this throw (aniAim state 13 seen since its first press) or launched it.
    bool startedNow() const noexcept {
        if(pendingSeq_&&launchedSeq.load(std::memory_order_acquire)>=pendingSeq_)return true;
        const auto ts=engineThrowStartNs.load(std::memory_order_acquire);return firstPressNs_&&ts>=firstPressNs_;}
public:
    bool owns(int hold,int kind) const noexcept {return active()&&hold>=0&&hold<=1&&throwableKind(kind);}
    // heldKind reads -1 for a frame on an inspect failure - the same hand keeps its throwable for kindHoldNs (no aim cancel,
    // no gun routing of the aim trigger as RT)
    int kindOf(int hold,int kind,std::uint64_t now) const noexcept {
        return kind<0&&owned_&&hold==ownHold_&&now>=ownNs_&&now-ownNs_<kindHoldNs?ownKind_:kind;}
    bool ownsNow(int hold,int kind,std::uint64_t now) const noexcept {return owns(hold,kindOf(hold,kind,now));}
    // The pad belongs to another brain this frame (lasso / duel): no aim, no pulse, no force aim from the throwable.
    void drop() noexcept {
        if(aiming_)stopAim(true);down_=false;owned_=false;relPending_=false;litStance_=false;
        if(pulseWait_||pressing())bump(stats.pulsesDropped);endChain();aimUpSince_=0;
        heldHand.store(-1,std::memory_order_relaxed);}
    // Holsters release(): the HOLD grip of the holding hand opened - a pending aimed release is a put-away.
    void cancelRelease() noexcept {if(relPending_){relPending_=false;stopAim(true);bump(stats.aimOpenCancels);}}
    // Hand h has a throw of this brain that is not launched (+ settle) / expired yet.
    bool pendingFor(unsigned h,std::uint64_t now) const noexcept {return pendingSeq_&&pendingHand_==int(h)&&!settled(now);}
    // One throw decided: publish it, start the RT pulse (after the engine aim is up or aimWaitNs), the hand's holster waits for the launch.
    std::uint64_t post(const Request& r0,std::uint64_t now,int kind) noexcept {
        if(pendingSeq_&&!settled(now)){bump(stats.busy);return 0;}
        Request r=r0;r.seq=requestSeq.load(std::memory_order_relaxed)+1;r.ns=r.ns?r.ns:now;r.flags=std::uint32_t(kind&0xff);
        releasedHand.store(-1,std::memory_order_relaxed); // a later throw never reopens an old physical release (physical() re-sets it)
        pendingKind_=kind;pendingKind.store(kind,std::memory_order_relaxed); // before the seq (the tick / Launch read the wait from it)
        requestBox.store(r);requestSeq.store(r.seq,std::memory_order_release);
        pendingSeq_=r.seq;pendingNs_=now;pendingHand_=r.hand;pulseWait_=true;pulseFrom_=now;pulseLeft_=0;pendingPhysical_=r.mode==ModePhysical;
        holdUntil_=0;checking_=false;inPress_=false;attempt_=0;firstPressNs_=0;
        haptics::pulse(unsigned(r.hand)&1u,.35f,18'000,now,haptics::SourceReturn);
        return r.seq;
    }
    // The pending throw of this brain is over: launched (+ settleNs for the clip's follow-through) or expired.
    bool settled(std::uint64_t now) const noexcept {
        if(!pendingSeq_)return true;
        // an OPENED hand (physical) is put away 60 ms after
        // the launch, before the engine re-shows the next item; an aimed throw (hand still closed) keeps the clip's settle
        if(launchedSeq.load(std::memory_order_acquire)>=pendingSeq_){const auto at=launchedNs.load(std::memory_order_relaxed);
            return now<at||now-at>=(pendingPhysical_?60'000'000ull:config.settleNs);}
        return now<pendingNs_||now-pendingNs_>config.maxDelayNs+(pendingKind_==kindExploding?config.explodeExtraNs:0ull)+config.settleNs;
    }
    // Holsters serviceHolster: hand h must not be put away yet (the engine would cancel the throw clip).
    bool holdsHolster(unsigned h,std::uint64_t now) noexcept {
        if(!pendingSeq_||pendingHand_!=int(h))return false;
        if(settled(now)){pendingSeq_=0;pendingHand_=-1;return false;}
        return true;
    }
    bool pending() const noexcept {return pendingSeq_!=0;}
    bool aiming() const noexcept {return aiming_;}
    // Force aim wanted by the throwable (holsters poll): HELD = whenever a throwable is in a VR hand, TRIGGER = while aiming / until a throw settled.
    bool wantsAim(bool throwableHeld,std::uint64_t now) const noexcept {
        if(!active()||!throwableHeld||config.aim==AimMode::Off)return false;
        // a tomahawk / knives
        // keep the aim stance while held, so a physical throw's RT starts the clip at once (no aim-in); dynamite keeps TRIGGER (fuse)
        // THROWN_EXPLODING too once it burns (a LIGHT tap, or the engine's fuse flag: a fire bottle lit at its draw) - the stance
        // starts no fuse then, and the physical swing's RT throws at once like the knife's; an unlit stick keeps TRIGGER (no fuse by holding)
        EngineAim ea{};
        const bool heldStance=owned_&&config.aim!=AimMode::Off&&(ownKind_==kindThrown||
            (ownKind_==kindExploding&&(litStance_||(engineAim(ea,now)&&drawLit(ea)))));
        return config.aim==AimMode::Held||heldStance||aiming_||pulseWait_||pressing()||checking_||!settled(now); // kept through the throw clip
    }
    // Pad routing while a throwable is in VR hand `hold`: the holding trigger = the aim gesture (never RT), RT = the throw pulse only.
    // False = not owned (the caller routes it normally).
    bool route(const RouteIn& r,PadState& pad) noexcept {
        const int kind=kindOf(r.hold,r.kind,r.now);
        if(!owns(r.hold,kind)){if(aiming_)stopAim(true);down_=false;owned_=false;relPending_=false;
            // a pending RT pulse never outlives its hand / item (the next throwable would be thrown at its draw); a one-frame
            // unknown kind (-1) keeps it
            if(r.hold<0||r.hold!=pendingHand_||(r.kind>=0&&!throwableKind(r.kind))){
                if(pulseWait_||pressing())bump(stats.pulsesDropped);endChain();aimUpSince_=0;}
            litStance_=false;heldHand.store(-1,std::memory_order_relaxed);return false;}
        bump(stats.routeFrames);heldHand.store(r.hold,std::memory_order_relaxed);heldKindNow.store(kind,std::memory_order_relaxed);
        const unsigned h=unsigned(r.hold);const float trig=r.in?r.in->triggers[h]:0.f;
        // (BLOCKER: a full-fist grab of dynamite lit the fuse and threw it on the trigger release): a squeeze carried over from
        // the grab / a transfer / a new item is no aim press - only a fresh 0 -> 1 aims
        if(!owned_||r.hold!=ownHold_||kind!=ownKind_){
            if(aiming_)stopAim(true);relPending_=false;owned_=true;ownHold_=r.hold;ownKind_=kind;ownNs_=r.now;litStance_=false;
            down_=trig>=.55f;if(down_)bump(stats.aimCarried);}
        if(r.kind>=0)ownNs_=r.now;
        // the lit stick left (a throw posted AFTER the tap - an older launched pendingSeq_ of an aimed throw, never cleared
        // by a put-away, would end the stance the frame after the tap)
        if(litStance_&&pendingSeq_>litAfterSeq_&&launchedSeq.load(std::memory_order_acquire)>=pendingSeq_)litStance_=false;
        // a pulse not sent by aimWaitNs (+ pulseLateNs) was not routed meanwhile (menu-less suspension, NATIVE, kill): dropped,
        // never fired late into a later item
        if(pulseWait_&&r.now>=pulseFrom_&&r.now-pulseFrom_>waitNs()+pulseLateNs){pulseWait_=false;bump(stats.pulsesDropped);}
        if((checking_||pressing())&&settled(r.now)){if(checking_)bump(perKind[kindIndex(pendingKind_)].noStart);endChain();} // never outlives its throw
        const bool down=trig>=(down_?.35f:.55f);
        if(r.menu||r.reins||r.tossing){if(aiming_)stopAim(true);relPending_=false;} // a menu frame / the rein squeeze / a toss in the air: never an aim
        else{
            if(down&&!down_){
                if(relPending_)relPending_=false; // squeezed again before the release was decided: still aiming
                else if(!pulseWait_&&!pressing()&&!checking_){aiming_=true;aimSince_=r.now;aimHand.store(r.hold,std::memory_order_relaxed);bump(stats.aimStarts);}
            }
            if(!down&&down_&&aiming_&&!relPending_){
                if(r.now<aimSince_||r.now-aimSince_<config.minAimNs){stopAim(false);bump(stats.aimTooShort);}
                // a tap on an unlit THROWN_EXPLODING = LIGHT (the
                // aim stance stays while held: the game lights it its own way), never an aimed throw; once lit a tap is the quick aimed throw
                // (never with FORCE AIM off - no stance there, the tap stays the quick throw)
                else if(kind==kindExploding&&config.lightTap&&config.aim!=AimMode::Off&&!litStance_&&r.now-aimSince_<config.lightTapNs){
                    EngineAim ea{};if(!(engineAim(ea,r.now)&&drawLit(ea))){stopAim(false);litStance_=true;
                        litAfterSeq_=requestSeq.load(std::memory_order_relaxed);bump(perKind[1].lightTaps);}
                    else{relPending_=true;relNs_=r.now;relPeak_=r.in?r.in->grips[h]:0.f;}}
                else{relPending_=true;relNs_=r.now;relPeak_=r.in?r.in->grips[h]:0.f;} // the arc stays up until decided
            }
            if(relPending_){
                const float g=r.in?r.in->grips[h]:0.f;relPeak_=std::fmax(relPeak_,g);
                if(r.now<relNs_||r.now-relNs_>=config.relWaitNs){relPending_=false;
                    if(!r.toggle&&(g<=config.openGrip||relPeak_-g>=config.openDrop)){stopAim(true);bump(stats.aimOpenCancels);} // a put-away
                    else{stopAim(false);
                        Request q{};q.ns=r.now;q.referenceSpace=r.in?r.in->referenceSpace:0;q.hand=r.hold;q.mode=ModeAimed;
                        if(post(q,r.now,kind))bump(stats.aimThrows);}}
            }
        }
        down_=down;
        if(!r.aimUp)aimUpSince_=0;else if(!aimUpSince_)aimUpSince_=r.now;
        const bool expl=pendingKind_==kindExploding;
        if(pulseWait_){
            bool ready=aimUpSince_&&r.now>=aimUpSince_&&r.now-aimUpSince_>=config.aimLeadNs;
            // THROWN_EXPLODING presses only once the engine aim takes a throw (ScheduleThrow's own refusals; unreadable = the lead alone)
            EngineAim ea{};if(ready&&expl&&engineAim(ea,r.now)&&!aimReady(ea))ready=false;
            if(ready||config.aim==AimMode::Off||r.now<pulseFrom_||r.now-pulseFrom_>=waitNs()){
                const bool late=!ready&&config.aim!=AimMode::Off;if(late)bump(stats.pulseWaitTimeouts);
                pulseWait_=false;press(r.now,late);}
        }
        // the press chain - a held press ends early once the engine started (never a second stick); no start within the check window
        // = the next press (THROWN_EXPLODING, up to explodeAttempts, once the engine aim is ready again), else the chain ends (noStart)
        if(holdUntil_&&(r.now>=holdUntil_||startedNow()))holdUntil_=0;
        if(checking_){
            if(startedNow()){checking_=false;auto& K=perKind[kindIndex(pendingKind_)];bump(K.started);bump(K.startedOn[std::min(attempt_,3u)]);}
            else{EngineAim ea{};const bool eng=engineAim(ea,r.now);const std::uint64_t win=expl&&eng?config.explodeCheckNs:config.explodeBlindNs;
                if(r.now<checkFrom_||r.now-checkFrom_>=win){
                    if(expl&&attempt_<config.explodeAttempts&&(!eng||aimReady(ea)||r.now-checkFrom_>=config.explodeBlindNs))press(r.now,false);
                    else if(!expl||attempt_>=config.explodeAttempts){checking_=false;bump(perKind[kindIndex(pendingKind_)].noStart);}}}
        }
        float rt=0.f;if(pressing()){rt=1.f;if(pulseLeft_)--pulseLeft_;}
        if(inPress_&&!pressing()){inPress_=false;checking_=true;checkFrom_=r.now;}
        pad.triggers[1]=rt;pad.triggers[0]=0.f;
        pad.buttons&=~(runtime::PadLeftTrigger|runtime::PadRightTrigger);if(rt>0.f)pad.buttons|=runtime::PadRightTrigger;
        return true;
    }
    // PHYSICAL (holsters release(): the HOLD grip of a throwable opened). e = the release estimate (tape preferred); hand / other hand grip
    // poses of this sample; otherFree = the other hand can catch; riding = on a horse (no toss). Returns: 1 = thrown (posted),
    // 2 = a hand toss (the caller runs the TossBrain), 0 = no throw (a plain holster).
    int physical(unsigned h,const ControllerSample& in,const T::ReleaseEstimator::Estimate& e,bool otherFree,bool riding,bool insideOwnSocket,int kind,std::uint64_t now) noexcept {
        if(!active())return 0;
        auto& K=perKind[kindIndex(kind)];bump(K.swings); // per kind, every outcome named (lastBlock)
        if(e.glitch){bump(stats.glitch);bump(K.glitch);noteBlock(kind,"GLITCH");return 0;}
        if(!e.valid||e.speedPeak<minSpeed()||e.speedRelease<releaseSpeed()){
            // below MIN SWING never the game's throw - a gentle
            // toss up / to the other hand is the visual toss (catchable), anything else the plain put-away
            if(tossable(h,in,e,otherFree,riding,insideOwnSocket,now)){bump(K.tosses);noteBlock(kind,"TOSS");return 2;}
            bump(stats.slow);bump(K.slow);noteBlock(kind,!e.valid?"NO_ESTIMATE":"SLOW");return 0;}
        if(insideOwnSocket){bump(stats.socket);bump(K.socket);noteBlock(kind,"SOCKET");return 0;}
        const float len=T::math::length(e.v);
        if(e.v[1]<-config.downDot*len||(kind==kindExploding&&e.v[1]<-config.explodeDownDot*len)){bump(stats.down);bump(K.down);noteBlock(kind,"DOWN");return 0;}
        if(!riding&&!(pendingSeq_&&!settled(now))&&tossToOther(h,in,e,otherFree)){bump(K.toOther);noteBlock(kind,"TOSS_TO_OTHER");return 2;}
        Request q{};q.ns=e.releaseNs?e.releaseNs:now;q.referenceSpace=in.referenceSpace;q.v=e.v;q.w=e.w;q.hand=int(h);q.mode=ModePhysical;
        q.speedPeak=e.speedPeak;q.speedRelease=e.speedRelease;
        const auto seq=post(q,now,kind);if(!seq){bump(K.busy);noteBlock(kind,"BUSY");return 0;}
        releasedNs.store(now,std::memory_order_relaxed);releasedSeq.store(seq,std::memory_order_relaxed);releasedHand.store(int(h),std::memory_order_release);
        bump(stats.physicalThrows);bump(K.releases);noteBlock(kind,"RELEASED");return 1;
    }
    // a gentle release that flies as the visual toss: towards the free other hand (tossToOther), or up / sideways from tossMinSpeed
    // (not downward = a put-away, not inside the item's own socket, never on a horse, never while a throw of this brain is pending).
    bool tossable(unsigned h,const ControllerSample& in,const T::ReleaseEstimator::Estimate& e,bool otherFree,bool riding,bool insideOwnSocket,
                  std::uint64_t now) const noexcept {
        if(!config.toss||riding||!e.valid||e.glitch||insideOwnSocket||!T::active())return false;
        if(pendingSeq_&&!settled(now))return false;
        if(tossToOther(h,in,e,otherFree))return true;
        if(e.speedPeak<config.tossMinSpeed||e.speedRelease<config.tossReleaseSpeed)return false;
        const float len=T::math::length(e.v);
        if(!(len>1e-3f)||e.v[1]<-config.tossDownDot*len)return false;
        bump(stats.tossUp);return true;
    }
    // A slow toss (<= tossSpeed) whose path (VrThrow's flight gravity) passes within tossRadius of the free other hand in the next 0.7 s.
    bool tossToOther(unsigned h,const ControllerSample& in,const T::ReleaseEstimator::Estimate& e,bool otherFree) const noexcept {
        if(!config.toss||!otherFree||!e.valid||T::math::length(e.v)>config.tossSpeed||!T::active())return false;
        const unsigned o=h^1u;const auto& a=in.grip[h];const auto& b=in.grip[o];
        if(!a.positionValid||!b.positionValid)return false;
        const V3 p0{a.position[0],a.position[1],a.position[2]},q{b.position[0],b.position[1],b.position[2]};
        // towards the other hand (within ~40 deg, hands < 0.9 m apart): a forward throw past an extended other hand is still a throw
        const V3 to=T::math::sub(q,p0);const float d=T::math::length(to);
        if(d>.9f||d<1e-3f||T::math::dot(T::math::normalized(e.v),T::math::mul(to,1.f/d))<.75f)return false;
        const float g=T::config.gravity();
        for(float t=.02f;t<=.70f;t+=.02f){const V3 p{p0[0]+e.v[0]*t,p0[1]+e.v[1]*t+.5f*g*t*t,p0[2]+e.v[2]*t};
            if(T::math::length(T::math::sub(p,q))<=config.tossRadius){bump(stats.handTosses);return true;}}
        return false;
    }
};

// ---- startup: debug.rdrvr.vr.throwable 0 (off = the native RT) + levers ----
inline void startup() noexcept {
    if(started.load(std::memory_order_relaxed))return;
    const auto prop=[](const char* k){return T::property(k);};
    const auto real=[&](const char* key,float& out,float lo,float hi,std::uint32_t bit=0){const auto s=prop(key);if(s.empty())return;char* e{};
        const float x=std::strtof(s.c_str(),&e);if(e!=s.c_str()&&std::isfinite(x)&&x>=lo&&x<=hi){out=x;config.props|=bit;}};
    const auto ms=[&](const char* key,std::uint64_t& out,float lo,float hi){float v=float(double(out)/1e6);real(key,v,lo,hi);out=std::uint64_t(double(v)*1e6);};
    const auto flag=[&](const char* key,bool& out){const auto s=prop(key);if(!s.empty())out=s[0]!='0';};
    flag("debug.rdrvr.vr.throwable",config.enabled);
    flag("debug.rdrvr.vr.throwable.override",config.launchOverride);flag("debug.rdrvr.vr.throwable.visual",config.visual);
    flag("debug.rdrvr.vr.throwable.toss",config.toss);
    real("debug.rdrvr.vr.throwable.gain",config.gain,.3f,5.f,PropGain);real("debug.rdrvr.vr.throwable.maxspeed",config.maxSpeed,3.f,60.f);
    real("debug.rdrvr.vr.throwable.minspeed",config.minSpeed,.5f,10.f,PropMinSpeed);real("debug.rdrvr.vr.throwable.release",config.releaseSpeed,.2f,10.f,PropRelease);
    real("debug.rdrvr.vr.throwable.downdot",config.downDot,.5f,1.01f);real("debug.rdrvr.vr.throwable.aimspeed",config.aimSpeed,3.f,60.f,PropAimSpeed);
    real("debug.rdrvr.vr.throwable.spin",config.spin,0.f,4.f,PropSpin);real("debug.rdrvr.vr.throwable.tossspeed",config.tossSpeed,0.f,8.f);
    real("debug.rdrvr.vr.throwable.tossradius",config.tossRadius,.1f,1.f);real("debug.rdrvr.vr.throwable.gravity",config.gravityMps2,1.f,30.f);
    {float p=float(config.rtPulse);real("debug.rdrvr.vr.throwable.rtpulse",p,1.f,20.f);config.rtPulse=unsigned(p);}
    ms("debug.rdrvr.vr.throwable.aimwait",config.aimWaitNs,0.f,2000.f);ms("debug.rdrvr.vr.throwable.aimlead",config.aimLeadNs,0.f,1000.f);ms("debug.rdrvr.vr.throwable.maxdelay",config.maxDelayNs,200.f,5000.f);
    ms("debug.rdrvr.vr.throwable.settle",config.settleNs,0.f,3000.f);ms("debug.rdrvr.vr.throwable.minaim",config.minAimNs,0.f,2000.f);
    ms("debug.rdrvr.vr.throwable.relwait",config.relWaitNs,0.f,500.f);real("debug.rdrvr.vr.throwable.opengrip",config.openGrip,0.f,.9f);
    real("debug.rdrvr.vr.throwable.opendrop",config.openDrop,.05f,1.01f);
    real("debug.rdrvr.vr.throwable.explodedowndot",config.explodeDownDot,0.f,1.01f);
    real("debug.rdrvr.vr.throwable.tossmin",config.tossMinSpeed,.3f,5.f);real("debug.rdrvr.vr.throwable.catchradius",config.catchRadius,.1f,.6f);
    real("debug.rdrvr.vr.throwable.pullradius",config.pullRadius,0.f,.8f);ms("debug.rdrvr.vr.throwable.tossmax",config.tossMaxNs,300.f,5000.f);
    {const auto s=prop("debug.rdrvr.vr.throwable.aim");for(int i=0;i<3;++i)if(s==aimModeNames[std::size_t(i)])config.aim=AimMode(i);}
    // THROWN_EXPLODING press chain + LIGHT tap
    {float a=float(config.explodeAttempts);real("debug.rdrvr.vr.throwable.explodeattempts",a,1.f,6.f);config.explodeAttempts=unsigned(a);}
    flag("debug.rdrvr.vr.throwable.lighttap",config.lightTap);ms("debug.rdrvr.vr.throwable.lighttapms",config.lightTapNs,50.f,1500.f);
    ms("debug.rdrvr.vr.throwable.explodewait",config.explodeReadyWaitNs,100.f,3000.f);ms("debug.rdrvr.vr.throwable.explodecheck",config.explodeCheckNs,100.f,2000.f);
    ms("debug.rdrvr.vr.throwable.explodeblind",config.explodeBlindNs,200.f,3000.f);ms("debug.rdrvr.vr.throwable.explodehold",config.explodeHoldNs,0.f,2000.f);
    ms("debug.rdrvr.vr.throwable.explodeextra",config.explodeExtraNs,0.f,5000.f);
    killed.store(!config.enabled,std::memory_order_relaxed);started.store(true,std::memory_order_release);
}

// "vr":{"throwable":<this>} (any thread)
inline std::string report() {
    std::string out;out.reserve(2048);char b[1400];const auto u=[](const Counter& c){return (unsigned long long)c.load(std::memory_order_relaxed);};
    const auto& s=stats;
    std::snprintf(b,sizeof b,"{\"enabled\":%s,\"active\":%s,\"config\":{\"override\":%s,\"visual\":%s,\"toss\":%s,\"aim\":\"%s\",\"gain\":%.2f,\"maxSpeed\":%.1f,"
        "\"minSpeed\":%.2f,\"releaseSpeed\":%.2f,\"downDot\":%.2f,\"aimSpeed\":%.1f,\"spin\":%.2f,\"tossSpeed\":%.2f,\"tossRadius\":%.2f,\"rtPulse\":%u,"
        "\"props\":%u,\"menu\":{\"vr\":%s,\"gain\":%.2f,\"minSpeed\":%.2f,\"aimSpeed\":%.1f,\"spin\":%.2f,\"gravityFactor\":%.2f,"
        "\"assist\":%.2f,\"showArc\":%s},\"aimLeadMs\":%.0f,\"aimWaitMs\":%.0f,\"maxDelayMs\":%.0f,\"settleMs\":%.0f,\"minAimMs\":%.0f,"
        "\"relWaitMs\":%.0f,\"openGrip\":%.2f,\"openDrop\":%.2f,\"explodeDownDot\":%.2f},\"gravityMps2\":%.2f,\"learnedGravityScale\":%.3f,",
        config.enabled?"true":"false",active()?"true":"false",config.launchOverride?"true":"false",config.visual?"true":"false",config.toss?"true":"false",
        aimModeNames[std::size_t(config.aim)],double(config.gain),double(config.maxSpeed),double(config.minSpeed),double(config.releaseSpeed),
        double(config.downDot),double(config.aimSpeed),double(config.spin),double(config.tossSpeed),double(config.tossRadius),config.rtPulse,
        config.props,settings().throwablesVr()?"true":"false",double(gain()),double(minSpeed()),double(aimSpeed()),double(spinGain()),
        double(gravityFactor()),double(assist()),showArc()?"true":"false",double(config.aimLeadNs)/1e6,double(config.aimWaitNs)/1e6,double(config.maxDelayNs)/1e6,double(config.settleNs)/1e6,double(config.minAimNs)/1e6,
        double(config.relWaitNs)/1e6,double(config.openGrip),double(config.openDrop),double(config.explodeDownDot),double(gravityMps2()),
        double(learnedGravityScale.load()));out+=b;
    std::snprintf(b,sizeof b,"\"input\":{\"routeFrames\":%llu,\"aimStarts\":%llu,\"aimThrows\":%llu,\"aimTooShort\":%llu,\"aimCancelled\":%llu,"
        "\"physicalThrows\":%llu,\"handTosses\":%llu,\"slow\":%llu,\"glitch\":%llu,\"down\":%llu,\"socket\":%llu,\"noTape\":%llu,\"pulses\":%llu,"
        "\"pulseWaitTimeouts\":%llu,\"busy\":%llu,\"pulsesDropped\":%llu,\"aimCarried\":%llu,\"aimOpenCancels\":%llu,\"releasedHand\":%d,"
        "\"aimHand\":%d,\"requestSeq\":%llu},",u(s.routeFrames),u(s.aimStarts),u(s.aimThrows),u(s.aimTooShort),
        u(s.aimCancelled),u(s.physicalThrows),u(s.handTosses),u(s.slow),u(s.glitch),u(s.down),u(s.socket),u(s.noTape),u(s.pulses),u(s.pulseWaitTimeouts),
        u(s.busy),u(s.pulsesDropped),u(s.aimCarried),u(s.aimOpenCancels),releasedHand.load(),aimHand.load(),(unsigned long long)requestSeq.load());out+=b;
    std::snprintf(b,sizeof b,"\"main\":{\"startStale\":%llu,\"ticks\":%llu,\"requestsSeen\":%llu,\"startFailures\":%llu,\"mapFailures\":%llu,\"flights\":%llu,"
        "\"flightProbeHits\":%llu,\"expired\":%llu,\"arcPublished\":%llu,\"arcHidden\":%llu,\"noShown\":%llu,\"forceAimRequests\":%llu,\"flying\":%s},",
        u(s.startStale),u(s.ticks),u(s.requestsSeen),u(s.startFailures),u(s.mapFailures),u(s.flights),u(s.flightProbeHits),u(s.expired),u(s.arcPublished),
        u(s.arcHidden),u(s.noShown),u(s.forceAimRequests),flightWeapon.load()?"true":"false");out+=b;
    const auto n=s.launchesOverridden.load();LastLaunch L{};const bool have=lastLaunchBox.load(L);
    std::snprintf(b,sizeof b,"\"launch\":{\"dirOnly\":[%llu,%llu],\"calls\":%llu,\"overridden\":%llu,\"native\":%llu,\"stale\":%llu,\"declined\":%llu,\"delayMsAvg\":%.0f,"
        "\"delayMsMax\":%llu,\"launchedSeq\":%llu,\"expiredSeq\":%llu,\"last\":{\"valid\":%s,\"mode\":\"%s\",\"kind\":%u,\"delayMs\":%.0f,"
        "\"gravityScale\":%.3f,\"speed\":%.2f,\"nativeSpeed\":%.2f,\"v\":[%.2f,%.2f,%.2f],\"nativeV\":[%.2f,%.2f,%.2f],\"p\":[%.2f,%.2f,%.2f],"
        "\"nativeP\":[%.2f,%.2f,%.2f]}},\"pose\":{\"calls\":%llu,\"placed\":%llu,\"declined\":%llu},\"assist\":{\"probes\":%llu,"
        "\"targets\":%llu,\"applied\":%llu,\"unreachable\":%llu},\"gravity\":{\"writes\":%llu,\"refused\":%llu},",
        u(s.dirOnlyArmed),u(s.launchesDirOnly),u(s.launchCalls),(unsigned long long)n,u(s.launchesNative),u(s.launchesStale),u(s.launchDeclined),n?double(s.launchDelayMsSum.load())/double(n):0.0,
        u(s.launchDelayMsMax),(unsigned long long)launchedSeq.load(),(unsigned long long)expiredSeq.load(),have?"true":"false",
        modeNames[L.mode<3?L.mode:0],L.kind,double(L.delayMs),double(L.gravityScale),double(lastSpeed.load()),double(lastNativeSpeed.load()),
        double(L.v[0]),double(L.v[1]),double(L.v[2]),double(L.nativeV[0]),double(L.nativeV[1]),double(L.nativeV[2]),
        double(L.p[0]),double(L.p[1]),double(L.p[2]),double(L.nativeP[0]),double(L.nativeP[1]),double(L.nativeP[2]),
        u(s.poseCalls),u(s.posePlaced),u(s.poseDeclined),u(s.assistProbes),u(s.assistTargets),u(s.assistApplied),u(s.assistUnreachable),
        u(s.gravityWrites),u(s.gravityRefused));out+=b;
    { // the engine aim now + per kind (swings -> releases / blocked, RT presses -> engine starts -> launches)
     EngineAim ea{};const bool he=engineAimBox.load(ea);const auto now=::rdrvr::vr::nowNs();const double age=he&&ea.ns&&now>=ea.ns?double(now-ea.ns)/1e6:-1.0;
     const char* refusal=he&&ea.valid?aimRefusal(ea):nullptr;
     std::snprintf(b,sizeof b,"\"explode\":{\"config\":{\"attempts\":%u,\"lightTap\":%s,\"lightTapMs\":%.0f,\"readyWaitMs\":%.0f,\"checkMs\":%.0f,"
        "\"blindMs\":%.0f,\"holdMs\":%.0f,\"extraMs\":%.0f},\"pendingKind\":%d,\"engineAim\":{\"valid\":%s,\"ageMs\":%.0f,\"state\":%d,\"armUp\":%.3f,"
        "\"lit\":%s,\"scheduled\":%s,\"actorFlags\":%u,\"reticleBlock\":%u,\"ownerMatch\":%u,\"drawLit\":%s,\"typeId\":%d,\"typeClass\":%d,\"refusal\":\"%s\",\"reads\":%llu,"
        "\"failures\":%llu},\"throwStarts\":%llu,\"perKind\":{",config.explodeAttempts,config.lightTap?"true":"false",double(config.lightTapNs)/1e6,
        double(config.explodeReadyWaitNs)/1e6,double(config.explodeCheckNs)/1e6,double(config.explodeBlindNs)/1e6,double(config.explodeHoldNs)/1e6,
        double(config.explodeExtraNs)/1e6,pendingKind.load(),he&&ea.valid?"true":"false",age,ea.state,double(ea.armUp),aimLit(ea)?"true":"false",
        (ea.flags&0x200u)?"true":"false",ea.actorFlags,unsigned(ea.reticleBlock),unsigned(ea.ownerMatch),drawLit(ea)?"true":"false",int(ea.typeId),ea.typeClass,he&&ea.valid?(refusal?refusal:"OK"):"UNREAD",
        u(engineAimReads),u(engineAimFailures),u(engineThrowStarts));out+=b;
     for(unsigned k=0;k<2;++k){const auto& K=perKind[k];PressNote n{};const bool hn=lastPress[k].load(n);
        const char* lb=lastBlock[k].load(std::memory_order_relaxed);const char* lr=lastPressRefusal[k].load(std::memory_order_relaxed);
        std::snprintf(b,sizeof b,"%s\"%s\":{\"swings\":%llu,\"releases\":%llu,\"slow\":%llu,\"down\":%llu,\"socket\":%llu,\"glitch\":%llu,\"busy\":%llu,"
            "\"tosses\":%llu,\"toOther\":%llu,\"lastBlock\":\"%s\",\"presses\":%llu,\"pressesReady\":%llu,\"pressesRefused\":%llu,\"pressesBlind\":%llu,\"holdPresses\":%llu,"
            "\"started\":%llu,\"startedOn\":[%llu,%llu,%llu],\"noStart\":%llu,\"launches\":%llu,\"litLaunches\":%llu,\"lightTaps\":%llu,\"litSeen\":%llu,"
            "\"lastPress\":{\"valid\":%s,\"attempt\":%d,\"engineRead\":%s,\"state\":%d,\"armUp\":%.3f,\"lit\":%s,\"ready\":%s,\"hold\":%s,\"late\":%s,"
            "\"refusal\":\"%s\"}}",k?",":"",k?"exploding":"thrown",u(K.swings),u(K.releases),u(K.slow),u(K.down),u(K.socket),u(K.glitch),u(K.busy),
            u(K.tosses),u(K.toOther),lb?lb:"NONE",u(K.presses),u(K.pressesReady),u(K.pressesRefused),u(K.pressesBlind),u(K.holdPresses),u(K.started),u(K.startedOn[1]),
            u(K.startedOn[2]),u(K.startedOn[3]),u(K.noStart),u(K.launches),u(K.litLaunches),u(K.lightTaps),u(K.litSeen),hn?"true":"false",n.attempt,
            n.valid?"true":"false",n.state,double(n.armUp),n.lit?"true":"false",n.ready?"true":"false",n.hold?"true":"false",n.pad?"true":"false",lr?lr:"NONE");
        out+=b;}
     out+="}},";}
    {Toss t{};const bool ht=tossBox.load(t);
     std::snprintf(b,sizeof b,"\"toss\":{\"up\":%llu,\"starts\":%llu,\"refused\":%llu,\"caughtSame\":%llu,\"caughtOther\":%llu,\"missed\":%llu,"
        "\"cancelled\":%llu,\"hits\":%llu,\"timeouts\":%llu,\"pulls\":%llu,\"farPresses\":%llu,\"visuals\":%llu,\"noStart\":%llu,"
        "\"transferZone\":%llu,\"minSpeed\":%.2f,\"catchRadius\":%.2f,\"last\":\"%s\"}}",u(s.tossUp),u(s.tossStarts),u(s.tossRefused),
        u(s.tossCaughtSame),u(s.tossCaughtOther),u(s.tossMissed),u(s.tossCancelled),u(s.tossHits),u(s.tossTimeouts),u(s.tossPulls),
        u(s.tossFarPresses),u(s.tossVisuals),u(s.tossNoStart),u(s.transferZone),double(config.tossMinSpeed),double(config.catchRadius),
        ht&&t.state<tossStateNames.size()?tossStateNames[t.state]:"IDLE");out+=b;}
    return out;
}
} // namespace rdrvr::vr::throwable
