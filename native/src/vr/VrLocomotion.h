#pragma once
// Free VR locomotion and suppression of the engine's "stoppers" for the local player.
//
// What it does (engine main thread unless noted):
//  - JUMP_SLOT hook of mvrMoverInst::Update(float const& dt, Vector3 const& linear, Vector3
//    const& angular, bool isPlayer, mvrMoverData const&) (GOT 0x2e5a780 -> 0x288e65c). ABI
//    (0x2888200..0x2888268): x0 = inst (mover+0x20), x1 = &dt (= frameDt/N, game time), x2 =
//    &linear (16-byte Vector3, world-space displacement for this substep, already scaled by
//    scale*mover+0xb44), x3 = &angular, w4 = isPlayer ((~actor+0x118 & 3) == 0), x5 = mover+0xb00.
//    Only the call from mvrMoverComponent::Update's normal path (return 0x288826c) with isPlayer
//    is touched; every other mover/path is a plain tail call. In LOCOMOTION FREE (and all gates
//    open) the XZ of a COPY of `linear` is replaced while the mod moves the capsule: stick part
//    v_vr*dt on every physics substep (sagWorld::sm_PhysicsStepCount, normally 2) plus the
//    roomscale head part (VrBodyLink requestHead, from CAMERA) spread evenly over the frame's
//    substeps. Y, w and angular stay native (ground, steps, gravity, body yaw/animation untouched).
//    After the mod's last motion the native XZ stays zeroed for releaseSeconds (no run-stop
//    root-motion slide, stop nudges below), then the hook is FULLY TRANSPARENT while the stick is
//    neutral and no head part is requested: the mover gets exactly the engine's own linear.
//    The engine skips its velocity update when |linear|^2 <= 1e-12 (0x288eaf8..0x288eb88), which
//    would let an old velocity coast on: right after the mod's own motion a 2e-6 m nudge forces
//    the "stop" impulse (instant stop, no drift).
//  - AssistedMovementControl::ms_bAssistedMovementEnabled (0x2f3e800) = 0 while FREE, restored when leaving FREE.
//  - Optional "immovable" flag mover+0x84e bit4 (PreComputeImpacts -> SetMassInvScales) while FREE
//    + BUMP OFF, opt-in only (debug.rdrvr.vr.immovable 1). The bit20 branch (0x28903b8..0x289041c)
//    zeroes the player's inverse mass in EVERY manifold - static walls, props, wagons, horses, NPCs -
//    with no actor check (unlike bit19 at 0x2890260), and the engine only ever sets it on a
//    stationary hogtied lasso victim (aniLasso::SetLassoVictimState 0x2177594).
//  - BUMP REACTIONS OFF: JUMP_SLOT hook of aActor::Broadcast(mvrMsgBumpedInto const&) (GOT 0x2e335d0 -> 0x1e48bb8): the
//    message PreComputeImpacts (return 0x2891dc0) sends to the player's OWN actor (x0) is dropped.
//  - STUMBLE OFF: JUMP_SLOT hooks of aniNMAnim::StartNM(char const*,MessageParamsBase*,bool) (GOT 0x2e3cf50, refusal = return
//    false, the engine's own "not started" path), NMStartUp (GOT 0x2e3cf58, count only) and StartNM() (GOT 0x2e3cf60, count only).
//    Refused for the live, on-foot player whose NM is not running yet and who took no damage in the last 1.5 s: bodyBalance,
//    balancerCollisionsReaction, staggerFall, stayUpright, armsWindmill*, lean*, hipsLean*. Everything else passes.
//  - CAMERA YAW HOLD (VrYawHold.h, installed by install() below, gate yawHoldGate()): the stock follow camera does
//    not turn the VR world (= the stick's world direction) while the player walks; without the hold,
//    stick back/sideways walks an arc.
//    Report: "direction" (per stick sector commanded-vs-achieved angle + commanded world-direction turn rate) and "cameraYawHold".
// Settings arrive through setConfig() (integration maps rdrvr::vr::settings()); the stick through onStick() (GameXr
// mapMovement, the same ControllerSample the head-relative mapping uses). Report: report() -> live report key "vr.loco".
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "VrBodyLink.h"
#include "VrYawHold.h" // Stock follow camera yaw hold (installed from install(), reported in vr.loco.cameraYawHold)
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace rdrvr::vr::loco {

// ---- ELF identities (librdr.so 1.58 arm64, .text file offset = vaddr - 0x4000) ----
inline constexpr std::uintptr_t moverInstUpdateOffset=0x288e65c,moverInstUpdateGotOffset=0x2e5a780;  // mvrMoverInst::Update
inline constexpr std::uintptr_t moverInstUpdateReturnOffset=0x288826c;   // normal path in mvrMoverComponent::Update
inline constexpr std::uintptr_t moverInstReplayReturnOffset=0x288805c;   // EnableReplayMove path (culled actors): untouched
inline constexpr std::uintptr_t moverUpdateSiteOffset=0x2888200,moverReplaySiteOffset=0x2888020,desiredMotionGateOffset=0x2888df4;
inline constexpr std::uintptr_t physLevelStepsOffset=0x2aac450;           // sagWorld::UpdatePhysLevel substep loop
inline constexpr std::uintptr_t stepCountOffset=0x2f84c40,stepCountGotOffset=0x2e04510;     // sagWorld::sm_PhysicsStepCount (int32)
inline constexpr std::uintptr_t currentStepOffset=0x2f84c4c,currentStepGotOffset=0x2e04518; // sagWorld::sm_CurrentPhysicsStep (int32)
inline constexpr std::uintptr_t assistedOffset=0x2f3e800,assistedGotOffset=0x2e0c3a0;       // ms_bAssistedMovementEnabled (u8)
inline constexpr std::uintptr_t gameDataOffset=0x4e75c4b8,gameDataGotOffset=0x2ddc550;      // GAMEDATA (state u32 +0, u8 +0x15/+0x16)
inline constexpr std::uintptr_t simVtableOffset=0x2db79e0;               // mvrMoverSimComponent vtable address point
inline constexpr std::uintptr_t simPhysicalSlotOffset=0x2db7a70,simPhysicalUpdateOffset=0x2892d58; // AP+0x90 -> PerformPhysicalUpdate
inline constexpr std::uintptr_t broadcastBumpOffset=0x1e48bb8,broadcastBumpGotOffset=0x2e335d0;    // aActor::Broadcast(mvrMsgBumpedInto)
inline constexpr std::uintptr_t bumpSiteOffset=0x2891da0,bumpReturnOffset=0x2891dc0;                 // PreComputeImpacts call
inline constexpr std::uintptr_t startNMNamedOffset=0x217f92c,startNMNamedGotOffset=0x2e3cf50;      // aniNMAnim::StartNM(char const*,..)
inline constexpr std::uintptr_t startNMTailOffset=0x217fa30;
inline constexpr std::uintptr_t startNMVoidOffset=0x217faf8,startNMVoidGotOffset=0x2e3cf60;        // aniNMAnim::StartNM()
inline constexpr std::uintptr_t nmStartUpOffset=0x217fb20,nmStartUpGotOffset=0x2e3cf58;            // aniNMAnim::NMStartUp()
inline constexpr std::uintptr_t nmStartUpNamedReturnOffset=0x217fa64,nmStartUpVoidReturnOffset=0x217fb14;
inline constexpr std::uintptr_t nmTaskStartOffset=0x2284ba4;             // NMTaskBase::StartNM: task+0x20 only on true
// Exact bytes (prologue/arg spills, both call sites, gates, substep loop, whole Broadcast/StartNM()/NMStartUp head).
inline constexpr std::array<unsigned char,140> moverInstUpdateFingerprint{
#include "gamecode/VrLocomotion_288e65c_140_8d.inc"
};
inline constexpr std::array<unsigned char,112> moverUpdateSiteFingerprint{
#include "gamecode/VrLocomotion_2888200_112_8d.inc"
};
inline constexpr std::array<unsigned char,64> moverReplaySiteFingerprint{
#include "gamecode/VrLocomotion_2888020_64_8d.inc"
};
inline constexpr std::array<unsigned char,56> desiredMotionGateFingerprint{
#include "gamecode/VrLocomotion_2888df4_56_8d.inc"
};
inline constexpr std::array<unsigned char,148> physLevelStepsFingerprint{
#include "gamecode/VrLocomotion_2aac450_148_8d.inc"
};
inline constexpr std::array<unsigned char,88> broadcastBumpFingerprint{
#include "gamecode/VrLocomotion_1e48bb8_88_8d.inc"
};
inline constexpr std::array<unsigned char,36> bumpSiteFingerprint{
#include "gamecode/VrLocomotion_2891da0_36_8d.inc"
};
inline constexpr std::array<unsigned char,116> startNMNamedFingerprint{
#include "gamecode/VrLocomotion_217f92c_116_8d.inc"
};
inline constexpr std::array<unsigned char,200> startNMTailFingerprint{
#include "gamecode/VrLocomotion_217fa30_200_8d.inc"
};
inline constexpr std::array<unsigned char,40> startNMVoidFingerprint{
#include "gamecode/VrLocomotion_217faf8_40_8d.inc"
};
inline constexpr std::array<unsigned char,64> nmStartUpFingerprint{
#include "gamecode/VrLocomotion_217fb20_64_8d.inc"
};
inline constexpr std::array<unsigned char,116> nmTaskStartFingerprint{
#include "gamecode/VrLocomotion_2284ba4_116_8d.inc"
};

// ---- engine field offsets ----
namespace field {
inline constexpr std::size_t moverActor=0x10,moverPos=0x70,moverAttachment=0x830,moverSuspend=0x846,moverFrozen=0x848,moverInstFlags=0x84c,
    moverInstFlagsHigh=0x84e,moverLedge=0x928,moverCover=0xaca,moverSpecial=0xaf0,moverRagdoll=0xb8c,moverBlock=0xb90;
inline constexpr std::size_t actorFlags112=0x112,actorFlags=0x118,actorHealth=0x60,healthValue=0x20,actorBlock=0x120;
inline constexpr std::size_t pmPlayer=0xa20;
inline constexpr std::size_t playerActive=0x5f1,playerState=0x5d4,playerInputLocks=0x104,playerMoveLocks=0x108,playerLock1=0x10c,
    playerLock2=0x10d,playerFoot=0x118,playerScriptedGait=0x390,playerBlock=0x5f8;
inline constexpr std::size_t footPlayer=0x40,footDisabled=0x15c,footNav=0x160,footMode=0x190,footBlock=0x198;
inline constexpr std::size_t nmAnimator=0x8,animatorActor=0x10,nmRunning=0x414;
inline constexpr std::uint8_t immovableBit=0x10; // mover+0x84e bit4 = inst+0x82c word bit20
// Jump flag: mvrMoverInst::StartJump sets inst+0x82c |= 0x8000 (0x288c820), EndJump clears it (0x288c8a8); inst = mover+0x20
// -> mover+0x84c bit15 = "ballistic jump in progress" (mvrMoverInst::Update takes its airborne path, 0x288ebf0).
inline constexpr std::uint32_t jumpingBit=0x8000;
}

inline constexpr float stickDeadZone=0.15f;   // radial
inline constexpr float walkBandEnd=0.65f;     // normalized deflection where walking ends (1.6 m/s) and running starts
inline constexpr float walkMinSpeed=0.5f,walkSpeed=1.6f,runSpeed=3.6f,sprintSpeed=5.5f; // m/s at MOVE SPEED 100%
inline constexpr std::uint64_t stickMaxAgeNs=150'000'000ull;       // older stick sample = transparent (native)
inline constexpr std::uint64_t headRequestMaxAgeNs=100'000'000ull; // older roomscale request = dropped
inline constexpr std::uint64_t damageWindowNs=1'500'000'000ull;    // NM: everything allowed this long after a health drop
inline constexpr int stopWindowSubsteps=16;                        // nudges after the mod's last non-zero command
inline constexpr float releaseSeconds=0.40f;                       // Zeroed native XZ after the mod's motion, then transparent
inline constexpr float headMaxPerFrame=0.10f,headMaxSpeed=2.0f;    // Hard roomscale limits (m per frame, m/s of game time)
inline constexpr float stopNudge=2e-6f;                            // |linear|^2 = 4e-12 > 1e-12: engine applies the stop impulse

inline std::uint64_t nowNs() noexcept {
    const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return ns>0?std::uint64_t(ns):0;}

// ---- settings / kill switches ----
// teleport = LOCOMOTION TELEPORT (report only: the TELEPORT branch of GameXr mapMovement sets free = true so roomscale and the L3
// escape walk keep the mover path, and feeds a zero stick while TELEPORT owns it; speedScale carries the escape walk's .walk factor).
struct Config {bool free{true};float speedScale{1.f};bool bumpOff{true};bool stumbleOff{true};bool teleport{false};};      // Menu defaults
struct Switches {bool loco{true},bump{true},stumble{true},immovable{false},assist{true};};             // false = not installed/used
inline std::atomic<bool> cfgFree{true},cfgBumpOff{true},cfgStumbleOff{true},cfgTeleport{false};
inline std::atomic<float> cfgSpeed{1.f};
inline void setConfig(const Config& c) noexcept {
    cfgFree.store(c.free,std::memory_order_relaxed);cfgBumpOff.store(c.bumpOff,std::memory_order_relaxed);
    cfgStumbleOff.store(c.stumbleOff,std::memory_order_relaxed);cfgTeleport.store(c.teleport,std::memory_order_relaxed);
    cfgSpeed.store(std::isfinite(c.speedScale)&&c.speedScale>0.1f&&c.speedScale<4.f?c.speedScale:1.f,std::memory_order_relaxed);}
inline Config config() noexcept {
    return {cfgFree.load(std::memory_order_relaxed),cfgSpeed.load(std::memory_order_relaxed),cfgBumpOff.load(std::memory_order_relaxed),
        cfgStumbleOff.load(std::memory_order_relaxed),cfgTeleport.load(std::memory_order_relaxed)};}
inline std::atomic<bool> swImmovable{false},swAssist{true};  // Immovable opt-in
inline std::array<std::atomic<bool>,3> swInstalled{}; // loco, bump, stumble switches seen at install (report)

// ---- stick -> v_vr (main thread, NativePadDriver movement provider) ----
// m = normalized deflection past the dead zone. Walk band: 0.5 -> 1.6 m/s, run band 1.6 -> 3.6 m/s; the game's sprint gait
// (InputFoot nav == 4) in the run band = 5.5 m/s. Instant start/stop (no ramp).
inline float stickSpeed(float magnitude,bool sprint) noexcept {
    if(!std::isfinite(magnitude)||magnitude<stickDeadZone)return 0.f;
    float m=(magnitude-stickDeadZone)/(1.f-stickDeadZone);if(m>1.f)m=1.f;
    if(m<=walkBandEnd)return walkMinSpeed+(walkSpeed-walkMinSpeed)*(m/walkBandEnd);
    if(sprint)return sprintSpeed;
    return walkSpeed+(runSpeed-walkSpeed)*((m-walkBandEnd)/(1.f-walkBandEnd));
}
struct StickState {float v[2]{};float speed{};float raw[2]{};std::uint64_t ns{};std::uint32_t valid{};std::uint32_t reason{};};
enum StickReason : std::uint32_t {StickOk=0,StickNoYaw=1,StickBad=2};
// x = right, y = forward (OpenXR thumbstick). yaw in the VrBodyLink convention.
inline StickState stickVelocity(float x,float y,bool haveYaw,float yaw,bool sprint,float scale,std::uint64_t now) noexcept {
    StickState s{};s.ns=now;s.raw[0]=x;s.raw[1]=y;
    if(!std::isfinite(x)||!std::isfinite(y)){s.reason=StickBad;return s;}
    const float r=std::hypot(x,y),speed=stickSpeed(r,sprint)*scale;
    if(speed<=0.f){s.valid=1;return s;} // centred stick needs no direction: valid zero (stop)
    if(!haveYaw||!std::isfinite(yaw)){s.reason=StickNoYaw;return s;}
    float f[2],rt[2];body::yawBasis(yaw,f,rt);
    const float dx=(x*rt[0]+y*f[0])/r,dz=(x*rt[1]+y*f[1])/r;
    s.v[0]=dx*speed;s.v[1]=dz*speed;s.speed=speed;s.valid=1;return s;
}
inline body::Seqlock<StickState> stick;
inline std::atomic<bool> sprintGait{false};
inline std::atomic<bool> sprintRequest{false}; // L3 sprint toggle active (VrControls.h) -> sprint speed in the run band
inline std::atomic<std::uint64_t> stickSamples{},stickNoYaw{};
inline void publishStick(float x,float y,std::uint64_t now) noexcept {
    float yaw{};const bool haveYaw=body::headYaw(yaw,now);
    const auto s=stickVelocity(x,y,haveYaw,yaw,sprintGait.load(std::memory_order_relaxed)||sprintRequest.load(std::memory_order_relaxed),
        cfgSpeed.load(std::memory_order_relaxed),now);
    if(s.reason==StickNoYaw)stickNoYaw.fetch_add(1,std::memory_order_relaxed);
    stickSamples.fetch_add(1,std::memory_order_relaxed);stick.store(s);
}

// ---- gates ----
enum class Gate : unsigned {Ok,ReadFailed,NoPlayer,NotSim,Ragdoll,Mounted,Suspended,Skipped,NotInGame,Scripted,Dead,NotOnFoot,Locked,Ladder,
    Cover,BadSteps,Count};
inline constexpr unsigned gateCount=unsigned(Gate::Count);
// Panel glyphs only (A-Z 0-9 space . : / - % ? < >): these double as the LOCOMOTION row note.
inline constexpr std::array<const char*,gateCount> gateNames{"ACTIVE","READ FAILED","NO PLAYER","NOT SIM","RAGDOLL","MOUNTED","SUSPENDED",
    "SKIPPED","NOT IN GAME","SCRIPTED","DEAD","NOT ON FOOT","LOCKED","LADDER","COVER","BAD STEPS"};
struct MoverFields {std::uintptr_t vptr{},actor{},attachment{};std::int16_t suspend{},frozen{};std::uint32_t instFlags{};
    std::uint32_t ledge{},special{},ragdoll{};std::uint8_t cover{};float pos[3]{};};
struct FrameFields {bool read{},player{},foot{};std::uint32_t gameState{};std::uint8_t gd15{},gd16{};std::uint16_t actorFlags112{};
    std::uint32_t actorFlags{};float hp{};std::uint32_t playerState{};std::uint16_t inputLocks{},moveLocks{};std::uint8_t lock1{},lock2{};
    std::int32_t scriptedGait{-1};std::uint32_t footMode{},nav{};std::uint8_t footDisabled{};};
// Order: unreadable -> mover state the engine itself special-cases -> game/player state -> foot-only modes.
inline Gate evaluateGate(bool moverRead,const MoverFields& m,std::uintptr_t simVtable,const FrameFields& f) noexcept {
    if(!moverRead||!f.read)return Gate::ReadFailed;
    if(m.vptr!=simVtable)return Gate::NotSim;
    if(m.ragdoll)return Gate::Ragdoll;                            // BecomeRagdoll/HandleHasDied: +0xb8c = 1
    if(m.attachment)return Gate::Mounted;                         // SetAttached: +0x830 (horse, wagon, carried)
    if(m.suspend>0||m.frozen>0)return Gate::Suspended;           // kinematic Teleport branch, no sweep (0x288eae0)
    if(m.instFlags&1u)return Gate::Skipped;                       // inst+0x82c bit0: Update does nothing (0x288eadc)
    if(!f.player||(f.actorFlags&3u)!=3u)return Gate::NoPlayer;
    if(f.gameState!=3)return Gate::NotInGame;
    if(f.gd15&&(f.actorFlags112&4u)&&!f.gd16)return Gate::Scripted; // GetDesiredMotion's own zero-motion gate (0x2888e00)
    if(!std::isfinite(f.hp)||f.hp<=0.f)return Gate::Dead;
    if(f.playerState!=2||!f.foot)return Gate::NotOnFoot;
    if(f.inputLocks||f.lock1||f.lock2||f.moveLocks||f.scriptedGait>=0)return Gate::Locked;
    if(m.ledge||m.special||f.footMode||(f.footDisabled&1u))return Gate::Ladder;
    if(m.cover)return Gate::Cover;
    return Gate::Ok;
}

// ---- per-substep controller (pure; main thread) ----
struct Vec4 {float x{},y{},z{},w{};};
struct SubstepInput {
    int step{},steps{};float dt{};float pos[2]{};Vec4 native{};
    bool free{},gateOk{},stickValid{};float v[2]{};
    bool head{};float headXZ[2]{};std::uint32_t headId{};std::uint64_t now{}; // head = this physics frame's request (whole frame)
    bool allowNudge{}; // !apply substep whose mover still runs the normal velocity path (see playerSubstep)
    bool jumping{};    // Mover jump flag (field::jumpingBit) - native jump/fall motion is never cancelled
    float raw[2]{};    // Raw stick (x right, y forward) of the sample that made v - direction statistics by stick sector
};
inline constexpr unsigned dirSectors=8;
inline constexpr std::array<const char*,dirSectors> dirSectorNames{"F","FR","R","BR","B","BL","L","FL"};
inline constexpr float dirMinCommand=0.005f,dirMinAchieved=0.002f; // metres per frame
inline int stickSector(float x,float y) noexcept {
    if(!std::isfinite(x)||!std::isfinite(y)||std::hypot(x,y)<stickDeadZone)return -1;
    float deg=std::atan2(x,y)*57.2957795f;if(deg<0)deg+=360.f;
    return int(std::floor((deg+22.5f)/45.f))%int(dirSectors);
}
struct DirSector {
    std::uint64_t frames{};float angleSumDeg{},angleMaxDeg{};std::uint32_t hist[4]{}; // |angle| < 5, < 15, < 45, >= 45 deg
    float turnSumDeg{},turnSeconds{},turnMaxDegPerSec{};std::uint32_t turnFrames{};
};
struct DirStats {std::array<DirSector,dirSectors> s{};std::uint64_t frames{},skippedShort{};};
class DirTracker {
    DirStats st_{};int lastSector_{-1};float lastDir_{NAN};
public:
    const DirStats& stats() const noexcept {return st_;}
    void reset() noexcept {lastSector_=-1;lastDir_=NAN;}
    // One closed physics frame in which the stick moved the capsule. cmd/ach = world XZ metres of the frame, seconds = frame time.
    void frame(int sector,const float cmd[2],const float ach[2],float seconds) noexcept {
        if(sector<0||sector>=int(dirSectors)){reset();return;}
        const float lc=std::hypot(cmd[0],cmd[1]);
        if(!std::isfinite(lc)||lc<dirMinCommand){reset();++st_.skippedShort;return;}
        auto& s=st_.s[unsigned(sector)];++st_.frames;
        const float dc=std::atan2(cmd[0],cmd[1]);
        const float la=std::hypot(ach[0],ach[1]);
        if(std::isfinite(la)&&la>=dirMinAchieved){
            float d=std::fabs(yawhold::wrapPi(std::atan2(ach[0],ach[1])-dc))*57.2957795f;
            ++s.frames;s.angleSumDeg+=d;if(d>s.angleMaxDeg)s.angleMaxDeg=d;
            ++s.hist[d<5.f?0:d<15.f?1:d<45.f?2:3];
        }
        if(sector==lastSector_&&std::isfinite(lastDir_)&&std::isfinite(seconds)&&seconds>0.f&&seconds<0.25f){
            const float t=std::fabs(yawhold::wrapPi(dc-lastDir_))*57.2957795f;
            if(t<20.f){ // larger = a snap turn / sector flip, not a drift
                s.turnSumDeg+=t;s.turnSeconds+=seconds;++s.turnFrames;
                const float rate=t/seconds;if(rate>s.turnMaxDegPerSec)s.turnMaxDegPerSec=rate;
            }
        }
        lastSector_=sector;lastDir_=dc;
    }
};
struct SubstepOutput {bool replace{};Vec4 linear{};bool last{};bool publish{};body::FrameRecord record{};};
struct ControllerStats {
    std::uint64_t frames{},calls{},applied{},nudges{},headApplied{};std::uint32_t lastFrameCalls{},lastFrameApplied{};
    std::uint32_t integratedYes{},integratedNo{};float achievedStick[2]{},commandedStick[2]{},lastHead[2]{};bool achievedValid{};
    std::uint64_t transparent{},held{},headClamped{};float achievedFrame[2]{};
    std::uint64_t jumpNative{},jumpAirControl{}; // Jumping substeps left native / steered by the stick
};
class FrameController {
    body::FrameRecord rec_{};bool inFrame_{};int lastStep_{-1};int stopWindow_{};float hold_{};std::uint32_t calls_{},applied_{};
    ControllerStats stats_{};
    DirTracker dir_{};int frameSector_{-1};float frameSeconds_{};
    void closeFrame(const float pos[2]) noexcept {
        if(!inFrame_)return;
        stats_.lastFrameCalls=calls_;stats_.lastFrameApplied=applied_;
        if((rec_.flags&body::recordApplied)&&std::isfinite(pos[0])&&std::isfinite(pos[1])){
            // Total motion of the frame (entry of the next frame's substep 0 - C0). A substep's command reaches the
            // position one substep later, so a per-substep stick/head split is not meaningful: achievedStick is
            // the frame total when no head part was applied (otherwise it keeps its last value).
            stats_.achievedFrame[0]=pos[0]-rec_.c0[0];stats_.achievedFrame[1]=pos[1]-rec_.c0[1];
            if(!(rec_.flags&body::recordHead)){stats_.achievedStick[0]=stats_.achievedFrame[0];stats_.achievedStick[1]=stats_.achievedFrame[1];}
            stats_.achievedValid=true;
            const float n=float(rec_.steps>0?rec_.steps:1);
            stats_.commandedStick[0]=rec_.stickSub[0]*n;stats_.commandedStick[1]=rec_.stickSub[1]*n;
            // Commanded vs achieved direction of a pure stick frame (no roomscale part mixed in).
            if((rec_.flags&body::recordStick)&&!(rec_.flags&body::recordHead))
                dir_.frame(frameSector_,stats_.commandedStick,stats_.achievedFrame,frameSeconds_);
            else dir_.reset();
            // Whether capsule positions are integrated between substeps, sampled only from frames
            // that commanded stick motion on substeps 0..N-2. Exactly equal C0/C1 while the whole
            // frame moved = matrix only synced once per frame.
            if(rec_.steps>=2){
                const float cmd=std::hypot(rec_.stickSub[0],rec_.stickSub[1])*float(rec_.steps-1);
                if(cmd>0.002f){
                    const float early=std::hypot(rec_.c1[0]-rec_.c0[0],rec_.c1[1]-rec_.c0[1]);
                    const float total=std::hypot(pos[0]-rec_.c0[0],pos[1]-rec_.c0[1]);
                    if(early>0.2f*cmd)++stats_.integratedYes;
                    else if(rec_.c1[0]==rec_.c0[0]&&rec_.c1[1]==rec_.c0[1]&&total>0.5f*cmd)++stats_.integratedNo;
                }
            }
        }else dir_.reset();
        inFrame_=false;
    }
public:
    bool integrated() const noexcept {return stats_.integratedYes>=3&&stats_.integratedYes>2u*stats_.integratedNo;}
    const ControllerStats& stats() const noexcept {return stats_;}
    const DirStats& dirStats() const noexcept {return dir_.stats();}
    SubstepOutput substep(const SubstepInput& in) noexcept {
        SubstepOutput out{};out.linear=in.native;
        if(in.step==0||!inFrame_||in.step<=lastStep_){
            closeFrame(in.pos);
            rec_={};rec_.frame=std::uint32_t(++stats_.frames);rec_.c0[0]=rec_.c1[0]=in.pos[0];rec_.c0[1]=rec_.c1[1]=in.pos[1];
            inFrame_=true;calls_=applied_=0;frameSector_=-1;frameSeconds_=0.f;
        }
        lastStep_=in.step;++calls_;++stats_.calls;rec_.steps=in.steps;
        out.last=in.step>=in.steps-1;
        if(out.last){rec_.c1[0]=in.pos[0];rec_.c1[1]=in.pos[1];}
        // usable = FREE, gates open, a fresh valid stick sample and a sane substep: only then
        // may the mod own the capsule's XZ.
        const bool usable=in.free&&in.gateOk&&in.stickValid&&in.steps>=1&&std::isfinite(in.dt)&&in.dt>0.f&&in.dt<0.5f;
        float hx=0.f,hz=0.f;bool headMoving=false;
        if(usable&&!in.jumping&&in.head&&std::isfinite(in.headXZ[0])&&std::isfinite(in.headXZ[1])&&(in.headXZ[0]!=0.f||in.headXZ[1]!=0.f)){
            // Hard limits (CAMERA limits too): <= headMaxPerFrame and <= headMaxSpeed * frame time.
            float x=in.headXZ[0],z=in.headXZ[1];
            const float l=std::hypot(x,z),lim=std::min(headMaxPerFrame,headMaxSpeed*in.dt*float(in.steps));
            if(l>lim){x*=lim/l;z*=lim/l;if(!(rec_.flags&body::recordHead))++stats_.headClamped;}
            hx=x/float(in.steps);hz=z/float(in.steps);headMoving=true;
            if(!(rec_.flags&body::recordHead)){
                rec_.flags|=body::recordHead;rec_.head[0]=x;rec_.head[1]=z;rec_.headId=in.headId;
                stats_.lastHead[0]=x;stats_.lastHead[1]=z;++stats_.headApplied;
            }
        }
        const bool stickMoving=usable&&(in.v[0]!=0.f||in.v[1]!=0.f)&&std::isfinite(in.v[0])&&std::isfinite(in.v[1]);
        const bool nativeZero=in.native.x*in.native.x+in.native.y*in.native.y+in.native.z*in.native.z<=1e-12f;
        if(in.jumping&&!stickMoving){
            // Jump / ballistic flight with a neutral stick: the engine's own XZ (launch velocity, arc root
            // motion) stays untouched - no release hold, no stop nudge, and none queued for the landing either.
            hold_=0.f;stopWindow_=0;++stats_.jumpNative;
        }else if(stickMoving||headMoving){
            if(in.jumping)++stats_.jumpAirControl; // Stick held in the air = air control at the stick speed (Y native)
            // The mod moves the capsule: stick v*dt per substep + 1/N of the frame's head part.
            const float sx=stickMoving?in.v[0]*in.dt:0.f,sz=stickMoving?in.v[1]*in.dt:0.f;
            rec_.stickSub[0]=sx;rec_.stickSub[1]=sz;if(stickMoving)rec_.flags|=body::recordStick;
            if(stickMoving){frameSector_=stickSector(in.raw[0],in.raw[1]);frameSeconds_=in.dt*float(in.steps);}
            out.linear.x=sx+hx;out.linear.z=sz+hz;out.replace=true;rec_.flags|=body::recordApplied;++applied_;++stats_.applied;
            hold_=releaseSeconds;stopWindow_=stopWindowSubsteps;
        }else if(usable&&hold_>0.f){
            // Release hold: native XZ (run-stop root motion) stays zeroed; stop nudge while the engine would skip its velocity
            // update (|linear|^2 <= 1e-12) so the mod's last velocity cannot coast on.
            hold_-=in.dt;float x=0.f;
            if(in.native.y*in.native.y<=1e-12f&&stopWindow_>0){--stopWindow_;x=stopNudge;++stats_.nudges;}
            out.linear.x=x;out.linear.z=0.f;out.replace=true;rec_.flags|=body::recordApplied;++applied_;++stats_.applied;++stats_.held;
        }else if(stopWindow_>0&&nativeZero&&(usable||in.allowNudge)){
            // Gates closed / stick stale / NATIVE (allowNudge = the mover keeps its normal velocity
            // path), or a very short hold: the native linear is exactly 0 right after the mod's
            // motion -> same stop nudge (not recordApplied).
            if(!usable)hold_=0.f;
            --stopWindow_;out.linear.x=stopNudge;out.linear.z=0.f;out.replace=true;++stats_.nudges;
        }else{
            // Native motion owns the capsule: exactly the engine's own linear.
            hold_=0.f;stopWindow_=0;
            if(usable)++stats_.transparent;
        }
        if(out.last){
            rec_.ns=in.now;
            if(integrated())rec_.flags|=body::recordIntegrated;else rec_.flags&=~body::recordIntegrated;
            out.publish=true;out.record=rec_;
        }
        return out;
    }
};

// ---- NM (stumble) policy ----
inline bool startsWith(const char* s,const char* p) noexcept {return std::strncmp(s,p,std::strlen(p))==0;}
// "Balance while walking" behaviours only.
inline bool balanceBehaviour(const char* name) noexcept {
    if(!name||!*name)return false;
    return !std::strcmp(name,"bodyBalance")||!std::strcmp(name,"balancerCollisionsReaction")||!std::strcmp(name,"staggerFall")||
        !std::strcmp(name,"stayUpright")||startsWith(name,"armsWindmill")||startsWith(name,"lean")||startsWith(name,"hipsLean");
}
struct NmInput {bool stumbleOff{},alive{},onFoot{},nmRunning{},recentDamage{};const char* name{};};
enum class NmVerdict {Pass,Refuse,PassDamage,PassRunning,PassNotStanding,PassNative};
// Called only for the local player's aniNMAnim.
inline NmVerdict nmVerdict(const NmInput& in) noexcept {
    if(!balanceBehaviour(in.name))return NmVerdict::Pass;
    if(!in.stumbleOff)return NmVerdict::PassNative;
    if(in.nmRunning)return NmVerdict::PassRunning;          // already a ragdoll: refusing would leave it behaviour-less
    if(!in.alive||!in.onFoot)return NmVerdict::PassNotStanding;
    if(in.recentDamage)return NmVerdict::PassDamage;
    return NmVerdict::Refuse;
}
inline bool healthDropped(float before,float now) noexcept {
    return std::isfinite(before)&&std::isfinite(now)&&now<before-std::fmax(0.01f,std::fabs(before)*0.001f);}

// Per-name counters (top 16 distinct names, then "<other>"). Lock-free; any thread.
class NameCounters {
    struct Slot {std::atomic<std::uint32_t> hash{0};std::array<std::atomic<std::uint64_t>,4> name{};std::atomic<bool> ready{false};
        std::atomic<std::uint32_t> refused{0},allowed{0};};
    std::array<Slot,16> slots_{};std::atomic<std::uint32_t> otherRefused_{0},otherAllowed_{0};
    static std::uint32_t hash(const char* s) noexcept {std::uint32_t h=0;for(;*s;++s)h=h*0x1003fu+std::uint32_t(std::uint8_t(*s));return h?h:1u;}
public:
    void count(const char* name,bool refused) noexcept {
        if(!name)name="";
        const auto h=hash(name);
        for(auto& s:slots_){
            std::uint32_t cur=s.hash.load(std::memory_order_acquire);
            if(!cur){
                std::uint32_t expected=0;
                if(s.hash.compare_exchange_strong(expected,h,std::memory_order_acq_rel)){
                    std::array<std::uint64_t,4> raw{};std::memcpy(raw.data(),name,std::min<std::size_t>(std::strlen(name),31));
                    for(unsigned i=0;i<4;++i)s.name[i].store(raw[i],std::memory_order_relaxed);
                    s.ready.store(true,std::memory_order_release);cur=h;
                }else cur=expected;
            }
            if(cur==h){(refused?s.refused:s.allowed).fetch_add(1,std::memory_order_relaxed);return;}
        }
        (refused?otherRefused_:otherAllowed_).fetch_add(1,std::memory_order_relaxed);
    }
    // "nmRefused":{name:n,...},"nmAllowed":{name:n,...}
    void json(std::string& out) const {
        for(int pass=0;pass<2;++pass){
            out+=pass?",\"nmAllowed\":{":"\"nmRefused\":{";bool first=true;
            for(const auto& s:slots_){
                if(!s.ready.load(std::memory_order_acquire))continue;
                const auto n=(pass?s.allowed:s.refused).load(std::memory_order_relaxed);if(!n)continue;
                std::array<std::uint64_t,4> raw{};for(unsigned i=0;i<4;++i)raw[i]=s.name[i].load(std::memory_order_relaxed);
                char name[33]{};std::memcpy(name,raw.data(),32);name[32]=0;
                for(char* p=name;*p;++p){const char c=*p;
                    if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='<'||c=='>'||c=='-'))*p='?';}
                char b[96];std::snprintf(b,sizeof b,"%s\"%s\":%u",first?"":",",name,unsigned(n));out+=b;first=false;
            }
            const auto other=(pass?otherAllowed_:otherRefused_).load(std::memory_order_relaxed);
            if(other){char b[48];std::snprintf(b,sizeof b,"%s\"<other>\":%u",first?"":",",unsigned(other));out+=b;}
            out+="}";
        }
    }
};

// ---- live state / counters (report) ----
inline FrameController controller;                 // main thread only
inline NameCounters nmNames;
inline std::atomic<bool> moverInstalled{},bumpInstalled{},nmInstalled{};
inline std::atomic<const char*> moverInstall{"NOT ATTEMPTED"},bumpInstall{"NOT ATTEMPTED"},nmInstall{"NOT ATTEMPTED"};
inline std::atomic<unsigned> lastGate{unsigned(Gate::ReadFailed)};
inline std::array<std::atomic<std::uint64_t>,gateCount> gateRejects{};
inline std::atomic<std::uint64_t> playerCalls{},replayCalls{},staleStick{},noStick{},headStale{},readFailures{},hookExceptions{};
inline std::atomic<std::uint64_t> lastCallNs{};
inline std::atomic<int> lastSteps{},lastStep{-1};inline std::atomic<float> lastDt{};
inline std::atomic<float> statVx{},statVz{},statSpeed{},statAchX{},statAchZ{},statCmdX{},statCmdZ{},statHeadX{},statHeadZ{};
inline std::atomic<std::uint64_t> statFrames{},statApplied{},statNudges{},statHeadApplied{};
inline std::atomic<std::uint64_t> statTransparent{},statHeld{},statHeadClamped{};
inline std::atomic<float> statFrameX{},statFrameZ{};
inline std::atomic<std::uint64_t> statJumpNative{},statJumpAir{};
inline std::atomic<std::uint32_t> statFrameCalls{},statFrameApplied{},statIntYes{},statIntNo{};
inline std::atomic<bool> statAchValid{};
inline body::Seqlock<DirStats> statDir; // Direction statistics (main thread writes, report reads)
inline std::atomic<std::uint64_t> bumpSeen{},bumpDropped{},bumpOtherCallers{};
inline std::atomic<std::uint64_t> nmRefused{},nmPassDamage{},nmPassRunning{},nmPassNotStanding{},nmPassNative{},nmPassOther{};
inline std::atomic<std::uint64_t> nmStartUps{},nmStartUpsNamed{},nmStartUpsUnnamed{},nmConfigure{};
inline std::atomic<std::uint64_t> assistForced{},immovableSet{},immovableCleared{};
inline std::atomic<bool> assistOwned{},immovableOwned{};inline std::atomic<int> assistSaved{-1};
// cached player (main thread writes in the mover hook; any thread reads)
inline std::atomic<std::uintptr_t> playerActor{},playerMover{};
inline std::atomic<float> playerHp{NAN};inline std::atomic<std::uint64_t> lastDamageNs{},moverFrameNs{};
inline std::atomic<bool> playerAlive{},playerOnFoot{};

inline void publishStats() noexcept {
    const auto& s=controller.stats();
    statFrames.store(s.frames,std::memory_order_relaxed);statApplied.store(s.applied,std::memory_order_relaxed);
    statNudges.store(s.nudges,std::memory_order_relaxed);statHeadApplied.store(s.headApplied,std::memory_order_relaxed);
    statFrameCalls.store(s.lastFrameCalls,std::memory_order_relaxed);statFrameApplied.store(s.lastFrameApplied,std::memory_order_relaxed);
    statIntYes.store(s.integratedYes,std::memory_order_relaxed);statIntNo.store(s.integratedNo,std::memory_order_relaxed);
    statAchX.store(s.achievedStick[0],std::memory_order_relaxed);statAchZ.store(s.achievedStick[1],std::memory_order_relaxed);
    statCmdX.store(s.commandedStick[0],std::memory_order_relaxed);statCmdZ.store(s.commandedStick[1],std::memory_order_relaxed);
    statHeadX.store(s.lastHead[0],std::memory_order_relaxed);statHeadZ.store(s.lastHead[1],std::memory_order_relaxed);
    statAchValid.store(s.achievedValid,std::memory_order_relaxed);
    statTransparent.store(s.transparent,std::memory_order_relaxed);statHeld.store(s.held,std::memory_order_relaxed);
    statHeadClamped.store(s.headClamped,std::memory_order_relaxed);
    statFrameX.store(s.achievedFrame[0],std::memory_order_relaxed);statFrameZ.store(s.achievedFrame[1],std::memory_order_relaxed);
    statJumpNative.store(s.jumpNative,std::memory_order_relaxed);statJumpAir.store(s.jumpAirControl,std::memory_order_relaxed);
    statDir.store(controller.dirStats());
}
// CAMERA YAW HOLD gate (VrYawHold.h, called from the stock camera update on the engine main thread): the VR player is
// on foot with the LOCO gates open (the same evaluation every physics substep, NATIVE or FREE) and a VR head yaw is being
// published (world stereo running). Anything else (cutscene, cover, ladder, mounted, ragdoll, theater) keeps the stock camera.
inline bool yawHoldGate() noexcept {
    if(!moverInstalled.load(std::memory_order_acquire))return false;
    const auto now=nowNs(),last=lastCallNs.load(std::memory_order_relaxed);
    if(!last||(now>last&&now-last>250'000'000ull))return false;
    if(lastGate.load(std::memory_order_relaxed)!=unsigned(Gate::Ok))return false;
    float yaw{};return body::headYaw(yaw,now);
}
// "direction":{...}: per stick sector commanded-vs-achieved angle and commanded world-direction turn rate.
inline void directionJson(std::string& out) {
    DirStats d{};const bool have=statDir.load(d);
    char b[320];std::snprintf(b,sizeof b,"\"direction\":{\"frames\":%llu,\"skippedShort\":%llu,\"sectors\":{",
        (unsigned long long)d.frames,(unsigned long long)d.skippedShort);out+=b;
    bool first=true;
    for(unsigned i=0;i<dirSectors&&have;++i){
        const auto& s=d.s[i];if(!s.frames&&!s.turnFrames)continue;
        std::snprintf(b,sizeof b,"%s\"%s\":{\"frames\":%llu,\"angleAvgDeg\":%.2f,\"angleMaxDeg\":%.1f,\"hist5_15_45\":[%u,%u,%u,%u],"
            "\"cmdTurnDegPerSec\":%.2f,\"cmdTurnMaxDegPerSec\":%.1f,\"turnFrames\":%u}",first?"":",",dirSectorNames[i],
            (unsigned long long)s.frames,s.frames?double(s.angleSumDeg)/double(s.frames):0.0,double(s.angleMaxDeg),
            unsigned(s.hist[0]),unsigned(s.hist[1]),unsigned(s.hist[2]),unsigned(s.hist[3]),
            s.turnSeconds>0.f?double(s.turnSumDeg/s.turnSeconds):0.0,double(s.turnMaxDegPerSec),unsigned(s.turnFrames));
        out+=b;first=false;
    }
    out+="}}";
}

// LOCOMOTION row note (panel glyphs). nullptr = no note.
inline const char* stateNote() noexcept {
    if(!moverInstalled.load(std::memory_order_acquire))return nullptr; // menu shows " - UNAVAILABLE" from the status code
    if(!cfgFree.load(std::memory_order_relaxed))return nullptr;
    const auto last=lastCallNs.load(std::memory_order_relaxed),now=nowNs();
    if(!last||(now>last&&now-last>250'000'000ull))return "NO PHYSICS";      // ragdoll/mounted/cutscene: the player's mover is not stepped
    const auto g=lastGate.load(std::memory_order_relaxed);
    if(g!=unsigned(Gate::Ok))return g<gateCount?gateNames[g]:"LOCKED";
    StickState s{};
    if(!stick.load(s)||now-s.ns>stickMaxAgeNs)return "NO STICK";
    if(!s.valid)return s.reason==StickNoYaw?"NO HEAD":"NO STICK";
    return "ACTIVE";
}

inline std::string report() {
    std::string out;char b[1024];
    const auto c=config();
    const char* note=stateNote();
    const char* state=note?note:(moverInstalled.load()?(c.free?"IDLE":"NATIVE"):"UNAVAILABLE");
    std::snprintf(b,sizeof b,"{\"hookInstalled\":%s,\"hooks\":{\"mover\":\"%s\",\"bump\":\"%s\",\"nm\":\"%s\"},"
        "\"switches\":{\"loco\":%s,\"bump\":%s,\"stumble\":%s,\"immovable\":%s,\"assist\":%s},"
        "\"config\":{\"free\":%s,\"speedScale\":%.2f,\"bumpOff\":%s,\"stumbleOff\":%s,\"teleport\":%s},\"state\":\"%s\",",
        moverInstalled.load()?"true":"false",moverInstall.load(),bumpInstall.load(),nmInstall.load(),
        swInstalled[0].load()?"true":"false",swInstalled[1].load()?"true":"false",swInstalled[2].load()?"true":"false",
        swImmovable.load()?"true":"false",swAssist.load()?"true":"false",
        c.free?"true":"false",double(c.speedScale),c.bumpOff?"true":"false",c.stumbleOff?"true":"false",c.teleport?"true":"false",state);
    out+=b;
    const auto now=nowNs(),last=lastCallNs.load();
    char achieved[64];
    if(statAchValid.load())std::snprintf(achieved,sizeof achieved,"[%.4f,%.4f]",double(statAchX.load()),double(statAchZ.load()));
    else std::snprintf(achieved,sizeof achieved,"null");
    std::snprintf(b,sizeof b,"\"calls\":{\"player\":%llu,\"replay\":%llu,\"perFrame\":%u,\"appliedPerFrame\":%u,\"lastAgeMs\":%lld},"
        "\"frames\":%llu,\"applied\":%llu,\"N\":%d,\"lastStep\":%d,\"dtSub\":%.5f,\"stopNudges\":%llu,"
        "\"vStick\":[%.3f,%.3f,%.3f],\"commandedStick\":[%.4f,%.4f],\"achievedStick\":%s,"
        "\"stick\":{\"samples\":%llu,\"noHeadYaw\":%llu,\"stale\":%llu,\"invalid\":%llu},",
        (unsigned long long)playerCalls.load(),(unsigned long long)replayCalls.load(),unsigned(statFrameCalls.load()),unsigned(statFrameApplied.load()),
        last&&now>=last?(long long)((now-last)/1000000ull):-1ll,
        (unsigned long long)statFrames.load(),(unsigned long long)statApplied.load(),lastSteps.load(),lastStep.load(),double(lastDt.load()),
        (unsigned long long)statNudges.load(),
        double(statVx.load()),double(statVz.load()),double(statSpeed.load()),double(statCmdX.load()),double(statCmdZ.load()),achieved,
        (unsigned long long)stickSamples.load(),(unsigned long long)stickNoYaw.load(),(unsigned long long)staleStick.load(),
        (unsigned long long)noStick.load());
    out+=b;
    const auto yes=statIntYes.load(),no=statIntNo.load();
    std::snprintf(b,sizeof b,"\"substepIntegrated\":{\"state\":\"%s\",\"yes\":%u,\"no\":%u},"
        "\"head\":{\"applied\":%llu,\"stale\":%llu,\"clamped\":%llu,\"last\":[%.4f,%.4f]},"
        "\"transparentSubsteps\":%llu,\"heldSubsteps\":%llu,\"achievedFrame\":[%.4f,%.4f],"
        "\"jumpSubsteps\":{\"native\":%llu,\"airControl\":%llu},\"sprintRequest\":%s,",
        yes>=3&&yes>2u*no?"YES":no>=3&&no>2u*yes?"NO":"UNKNOWN",unsigned(yes),unsigned(no),
        (unsigned long long)statHeadApplied.load(),(unsigned long long)headStale.load(),(unsigned long long)statHeadClamped.load(),
        double(statHeadX.load()),double(statHeadZ.load()),
        (unsigned long long)statTransparent.load(),(unsigned long long)statHeld.load(),double(statFrameX.load()),double(statFrameZ.load()),
        (unsigned long long)statJumpNative.load(),(unsigned long long)statJumpAir.load(),sprintRequest.load()?"true":"false");
    out+=b;
    out+="\"gateRejects\":{";bool first=true;
    for(unsigned i=1;i<gateCount;++i){const auto n=gateRejects[i].load(std::memory_order_relaxed);if(!n)continue;
        std::snprintf(b,sizeof b,"%s\"%s\":%llu",first?"":",",gateNames[i],(unsigned long long)n);out+=b;first=false;}
    out+="},";
    const auto dmg=lastDamageNs.load();
    std::snprintf(b,sizeof b,"\"bump\":{\"seen\":%llu,\"dropped\":%llu,\"otherCallers\":%llu},"
        "\"assisted\":{\"owned\":%s,\"saved\":%d,\"forced\":%llu},\"immovable\":{\"owned\":%s,\"set\":%llu,\"cleared\":%llu},"
        "\"player\":{\"hp\":%.2f,\"alive\":%s,\"onFoot\":%s,\"sprint\":%s,\"lastDamageAgeMs\":%lld},"
        "\"nm\":{\"refused\":%llu,\"passDamage\":%llu,\"passRunning\":%llu,\"passNotStanding\":%llu,\"passNative\":%llu,\"pass\":%llu,"
        "\"startUps\":%llu,\"startUpsNamed\":%llu,\"startUpsUnnamed\":%llu,\"configure\":%llu},",
        (unsigned long long)bumpSeen.load(),(unsigned long long)bumpDropped.load(),(unsigned long long)bumpOtherCallers.load(),
        assistOwned.load()?"true":"false",assistSaved.load(),(unsigned long long)assistForced.load(),
        immovableOwned.load()?"true":"false",(unsigned long long)immovableSet.load(),(unsigned long long)immovableCleared.load(),
        std::isfinite(playerHp.load())?double(playerHp.load()):-1.0,playerAlive.load()?"true":"false",playerOnFoot.load()?"true":"false",
        sprintGait.load()?"true":"false",dmg&&now>dmg?(long long)((now-dmg)/1000000ull):-1ll,
        (unsigned long long)nmRefused.load(),(unsigned long long)nmPassDamage.load(),(unsigned long long)nmPassRunning.load(),
        (unsigned long long)nmPassNotStanding.load(),(unsigned long long)nmPassNative.load(),(unsigned long long)nmPassOther.load(),
        (unsigned long long)nmStartUps.load(),(unsigned long long)nmStartUpsNamed.load(),(unsigned long long)nmStartUpsUnnamed.load(),
        (unsigned long long)nmConfigure.load());
    out+=b;
    nmNames.json(out);
    out+=",";directionJson(out);
    out+=",\"cameraYawHold\":"+yawhold::report();
    std::snprintf(b,sizeof b,",\"readFailures\":%llu,\"exceptions\":%llu}",(unsigned long long)readFailures.load(),(unsigned long long)hookExceptions.load());
    out+=b;
    return out;
}

#if defined(__ANDROID__)
// ======================================= engine side (Android only) =======================================
using ReadFn=bool(*)(void*,std::uintptr_t,void*,std::size_t);
using MoverUpdateFn=void(*)(void* inst,const float* dt,const Vec4* linear,const Vec4* angular,bool isPlayer,const void* data);
using BroadcastBumpFn=void(*)(void* actor,const void* msg);
using StartNMNamedFn=bool(*)(void* nm,const char* name,void* params,bool flag);
using StartNMVoidFn=bool(*)(void* nm);
using NMStartUpFn=void(*)(void* nm);
// read = TraceEntry readMemory (guarded; direct copy inside librdr PT_LOAD). Player/foot identities as for teleport/locomotion.
struct EngineApi {ReadFn read{};std::uintptr_t base{};std::uintptr_t playerManager{},playerManagerTable{},playerTable{},footTable{};};

namespace detail {
inline EngineApi api{};
inline MoverUpdateFn moverOriginal{};inline BroadcastBumpFn bumpOriginal{};
inline StartNMNamedFn startNMNamedOriginal{};inline StartNMVoidFn startNMVoidOriginal{};inline NMStartUpFn nmStartUpOriginal{};
inline std::uint32_t lastHeadId{};inline int lastSeenStep{-1};inline FrameFields frame{};inline float trackedHp{NAN};
inline body::HeadRequest frameHead{}; // This physics frame's roomscale request (id 0 = none)
template<class T> inline bool get(std::uintptr_t p,std::size_t o,T& out) noexcept {
    return api.read&&p&&p<=UINTPTR_MAX-o&&p+o<=UINTPTR_MAX-sizeof(T)&&api.read(nullptr,p+o,&out,sizeof(T));}
template<class T> inline T at(const unsigned char* block,std::size_t o) noexcept {T v{};std::memcpy(&v,block+o,sizeof(T));return v;}
inline bool readMover(std::uintptr_t mover,MoverFields& m) noexcept {
    static thread_local std::array<unsigned char,field::moverBlock> block{};
    if(!get(mover,0,block))return false;
    const auto* p=block.data();
    m.vptr=at<std::uintptr_t>(p,0);m.actor=at<std::uintptr_t>(p,field::moverActor);m.attachment=at<std::uintptr_t>(p,field::moverAttachment);
    m.suspend=at<std::int16_t>(p,field::moverSuspend);m.frozen=at<std::int16_t>(p,field::moverFrozen);
    m.instFlags=std::uint32_t(at<std::uint16_t>(p,field::moverInstFlags))|(std::uint32_t(at<std::uint8_t>(p,field::moverInstFlagsHigh))<<16);
    m.ledge=at<std::uint32_t>(p,field::moverLedge);m.special=at<std::uint32_t>(p,field::moverSpecial);
    m.ragdoll=at<std::uint32_t>(p,field::moverRagdoll);m.cover=at<std::uint8_t>(p,field::moverCover);
    for(unsigned i=0;i<3;++i)m.pos[i]=at<float>(p,field::moverPos+4*i);
    return std::isfinite(m.pos[0])&&std::isfinite(m.pos[2]);
}
inline bool readHp(std::uintptr_t actor,float& hp) noexcept {std::uintptr_t h{};return get(actor,field::actorHealth,h)&&h&&get(h,field::healthValue,hp);}
// Once per physics frame (substep 0): game/player/foot state for the gates.
inline FrameFields readFrame(std::uintptr_t actor) noexcept {
    FrameFields f{};
    std::array<unsigned char,field::actorBlock> a{};
    if(!get(api.base+gameDataOffset,0,f.gameState)||!get(api.base+gameDataOffset,0x15,f.gd15)||!get(api.base+gameDataOffset,0x16,f.gd16)||
       !actor||!get(actor,0,a))return f;
    f.actorFlags112=at<std::uint16_t>(a.data(),field::actorFlags112);f.actorFlags=at<std::uint32_t>(a.data(),field::actorFlags);
    if(!readHp(actor,f.hp))return f;
    f.read=true;
    std::uintptr_t table{},player{};std::array<unsigned char,field::playerBlock> pl{};
    if(!get(api.playerManager,0,table)||table!=api.playerManagerTable||!get(api.playerManager,field::pmPlayer,player)||!player||
       !get(player,0,pl)||at<std::uintptr_t>(pl.data(),0)!=api.playerTable||at<std::uint8_t>(pl.data(),field::playerActive)!=1)return f;
    f.player=true;
    f.playerState=at<std::uint32_t>(pl.data(),field::playerState);f.inputLocks=at<std::uint16_t>(pl.data(),field::playerInputLocks);
    f.moveLocks=at<std::uint16_t>(pl.data(),field::playerMoveLocks);f.lock1=at<std::uint8_t>(pl.data(),field::playerLock1);
    f.lock2=at<std::uint8_t>(pl.data(),field::playerLock2);f.scriptedGait=at<std::int32_t>(pl.data(),field::playerScriptedGait);
    const auto foot=at<std::uintptr_t>(pl.data(),field::playerFoot);std::array<unsigned char,field::footBlock> ft{};
    if(f.playerState==2&&foot&&get(foot,0,ft)&&at<std::uintptr_t>(ft.data(),0)==api.footTable&&at<std::uintptr_t>(ft.data(),field::footPlayer)==player){
        f.foot=true;f.footDisabled=at<std::uint8_t>(ft.data(),field::footDisabled);f.nav=at<std::uint32_t>(ft.data(),field::footNav);
        f.footMode=at<std::uint32_t>(ft.data(),field::footMode);}
    return f;
}
// ms_bAssistedMovementEnabled: forced 0 while wanted, original restored after. Main thread only.
inline void manageAssisted(bool want) noexcept {
    if(!api.base)return;
    const auto address=api.base+assistedOffset;std::uint8_t cur{};if(!get(address,0,cur))return;
    auto* byte=reinterpret_cast<volatile std::uint8_t*>(address);
    if(want){
        if(!assistOwned.load(std::memory_order_relaxed)){assistSaved.store(cur,std::memory_order_relaxed);assistOwned.store(true,std::memory_order_relaxed);}
        if(cur){*byte=0;assistForced.fetch_add(1,std::memory_order_relaxed);}
    }else if(assistOwned.load(std::memory_order_relaxed)){
        const int saved=assistSaved.load(std::memory_order_relaxed);*byte=std::uint8_t(saved>=0?saved:1);assistOwned.store(false,std::memory_order_relaxed);}
}
// mover+0x84e bit4 for the mover just validated (vptr read in this call). Main thread, before the original Update.
inline void manageImmovable(std::uintptr_t mover,std::uint32_t instFlags,bool want) noexcept {
    auto* byte=reinterpret_cast<volatile std::uint8_t*>(mover+field::moverInstFlagsHigh);
    const bool set=(instFlags>>16)&field::immovableBit;
    if(want){if(!set){*byte=std::uint8_t(*byte|field::immovableBit);immovableSet.fetch_add(1,std::memory_order_relaxed);}
        immovableOwned.store(true,std::memory_order_relaxed);}
    else if(immovableOwned.load(std::memory_order_relaxed)){
        if(set){*byte=std::uint8_t(*byte&~field::immovableBit);immovableCleared.fetch_add(1,std::memory_order_relaxed);}
        immovableOwned.store(false,std::memory_order_relaxed);}
}
// Release an owned immovable flag when the player's mover stops being stepped (mounted/ragdoll/cutscene): re-validate the
// cached mover (Sim vtable + local-player actor) with guarded reads first.
inline void releaseIdleImmovable(std::uint64_t now) noexcept {
    if(!immovableOwned.load(std::memory_order_relaxed))return;
    const auto last=moverFrameNs.load(std::memory_order_relaxed);if(last&&now-last<100'000'000ull)return;
    const auto mover=playerMover.load(std::memory_order_relaxed);std::uintptr_t vptr{},actor{};std::uint32_t flags{};std::uint8_t high{};
    if(mover&&get(mover,0,vptr)&&vptr==api.base+simVtableOffset&&get(mover,field::moverActor,actor)&&actor&&get(actor,field::actorFlags,flags)&&
       (flags&3u)==3u&&get(mover,field::moverInstFlagsHigh,high))manageImmovable(mover,std::uint32_t(high)<<16,false);
    else immovableOwned.store(false,std::memory_order_relaxed); // object gone: no owned flag left to clear
}
// The player's substep (normal path). Fills `replaced` and returns true when the copy must be passed instead.
inline bool playerSubstep(std::uintptr_t inst,const float* dt,const Vec4* linear,Vec4& replaced) noexcept {
    const auto now=nowNs();
    playerCalls.fetch_add(1,std::memory_order_relaxed);lastCallNs.store(now,std::memory_order_relaxed);
    const std::uintptr_t mover=inst-0x20;
    std::int32_t steps{},step{};
    const bool stepsRead=get(api.base+stepCountOffset,0,steps)&&get(api.base+currentStepOffset,0,step);
    MoverFields m{};const bool moverRead=readMover(mover,m);
    if(!stepsRead||!moverRead)readFailures.fetch_add(1,std::memory_order_relaxed);
    const bool newFrame=step==0||step<=lastSeenStep;lastSeenStep=step;
    if(newFrame||!frame.read){
        frame=readFrame(moverRead?m.actor:0);
        if(frame.read){
            if(healthDropped(trackedHp,frame.hp))lastDamageNs.store(now,std::memory_order_relaxed);
            trackedHp=frame.hp;playerHp.store(frame.hp,std::memory_order_relaxed);
        }
        playerAlive.store(frame.read&&std::isfinite(frame.hp)&&frame.hp>0.f,std::memory_order_relaxed);
        playerOnFoot.store(frame.player&&frame.playerState==2&&!m.ragdoll,std::memory_order_relaxed);
        sprintGait.store(frame.foot&&frame.nav==4,std::memory_order_relaxed);
        moverFrameNs.store(now,std::memory_order_relaxed);
        if(moverRead&&m.vptr==api.base+simVtableOffset){playerActor.store(m.actor,std::memory_order_relaxed);playerMover.store(mover,std::memory_order_relaxed);}
        manageAssisted(swAssist.load(std::memory_order_relaxed)&&cfgFree.load(std::memory_order_relaxed));
    }
    Gate gate=evaluateGate(moverRead&&stepsRead,m,api.base+simVtableOffset,frame);
    if(gate==Gate::Ok&&(steps<1||steps>16||step<0||step>=steps))gate=Gate::BadSteps;
    lastGate.store(unsigned(gate),std::memory_order_relaxed);
    if(gate!=Gate::Ok)gateRejects[unsigned(gate)].fetch_add(1,std::memory_order_relaxed);
    lastSteps.store(steps,std::memory_order_relaxed);lastStep.store(step,std::memory_order_relaxed);lastDt.store(*dt,std::memory_order_relaxed);
    const auto cfg=config();
    if(moverRead&&m.vptr==api.base+simVtableOffset)
        manageImmovable(mover,m.instFlags,swImmovable.load(std::memory_order_relaxed)&&cfg.free&&cfg.bumpOff&&gate==Gate::Ok);
    StickState s{};bool stickValid=false;
    if(!stick.load(s)||(now>s.ns&&now-s.ns>stickMaxAgeNs)){if(cfg.free)staleStick.fetch_add(1,std::memory_order_relaxed);}
    else if(!s.valid){if(cfg.free)noStick.fetch_add(1,std::memory_order_relaxed);}
    else stickValid=true;
    if(stickValid){statVx.store(s.v[0],std::memory_order_relaxed);statVz.store(s.v[1],std::memory_order_relaxed);statSpeed.store(s.speed,std::memory_order_relaxed);}
    SubstepInput in{};in.step=step;in.steps=steps;in.dt=*dt;in.pos[0]=m.pos[0];in.pos[1]=m.pos[2];in.native=*linear;
    in.free=cfg.free;in.gateOk=gate==Gate::Ok;in.stickValid=stickValid;in.v[0]=s.v[0];in.v[1]=s.v[1];in.now=now;
    // Stop nudges after the mod's motion only where the mover keeps its normal velocity path; never for ragdoll, mounted,
    // suspended/kinematic, skipped, not-Sim, unreadable, dead/off-foot or ladder/ledge movers.
    in.allowNudge=gate==Gate::Ok||gate==Gate::Scripted||gate==Gate::Locked||gate==Gate::Cover;
    in.jumping=moverRead&&(m.instFlags&field::jumpingBit)!=0;
    in.raw[0]=s.raw[0];in.raw[1]=s.raw[1];
    // The roomscale request is taken once per physics frame (its first substep) and spread over all its substeps.
    if(newFrame){
        frameHead={};
        body::HeadRequest h{};
        if(in.free&&in.gateOk&&stickValid&&body::takeHead(lastHeadId,h)){
            if((now>=h.ns&&now-h.ns<=headRequestMaxAgeNs)||(h.ns>now&&h.ns-now<=headRequestMaxAgeNs))frameHead=h;
            else headStale.fetch_add(1,std::memory_order_relaxed);
        }
    }
    if(frameHead.id){in.head=true;in.headXZ[0]=frameHead.x;in.headXZ[1]=frameHead.z;in.headId=frameHead.id;}
    const auto out=controller.substep(in);
    if(out.publish)body::publishRecord(out.record);
    if(newFrame||out.last)publishStats();
    if(!out.replace)return false;
    replaced=out.linear;return true;
}
inline bool isPlayerActor(std::uintptr_t actor) noexcept {
    const auto cached=playerActor.load(std::memory_order_relaxed);if(!cached||actor!=cached)return false;
    std::uint32_t flags{};return get(actor,field::actorFlags,flags)&&(flags&3u)==3u;
}
inline bool nmActor(void* nm,std::uintptr_t& actor) noexcept {
    std::uintptr_t animator{};return get(reinterpret_cast<std::uintptr_t>(nm),field::nmAnimator,animator)&&animator&&
        get(animator,field::animatorActor,actor)&&actor;
}
inline void readName(const char* name,char (&out)[48]) noexcept {
    std::memset(out,0,sizeof out);if(!name)return;
    for(std::size_t o=0;o<sizeof out-1;o+=8){ // 8-byte steps: never reads far past the terminator
        char chunk[8]{};if(!get(reinterpret_cast<std::uintptr_t>(name),o,chunk))return;
        for(std::size_t i=0;i<8&&o+i<sizeof out-1;++i){if(!chunk[i])return;out[o+i]=chunk[i];}
    }
}
} // namespace detail

// ---- hooks (installed into the GOT; signatures from the call sites) ----
__attribute__((noinline)) inline void moverUpdateHook(void* inst,const float* dt,const Vec4* linear,const Vec4* angular,bool isPlayer,const void* data) noexcept {
    const auto original=detail::moverOriginal;
    if(!isPlayer){original(inst,dt,linear,angular,isPlayer,data);return;}
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    if(caller!=detail::api.base+moverInstUpdateReturnOffset){replayCalls.fetch_add(1,std::memory_order_relaxed);original(inst,dt,linear,angular,isPlayer,data);return;}
    alignas(16) Vec4 replaced{};
    const Vec4* use=linear;
    if(dt&&linear&&detail::playerSubstep(reinterpret_cast<std::uintptr_t>(inst),dt,linear,replaced))use=&replaced;
    original(inst,dt,use,angular,isPlayer,data);
}
__attribute__((noinline)) inline void bumpHook(void* actor,const void* msg) noexcept {
    const auto original=detail::bumpOriginal;
    const auto a=reinterpret_cast<std::uintptr_t>(actor);
    if(a&&a==playerActor.load(std::memory_order_relaxed)){
        const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        if(caller==detail::api.base+bumpReturnOffset){
            bumpSeen.fetch_add(1,std::memory_order_relaxed);
            if(cfgBumpOff.load(std::memory_order_relaxed)&&detail::isPlayerActor(a)){bumpDropped.fetch_add(1,std::memory_order_relaxed);return;}
        }else bumpOtherCallers.fetch_add(1,std::memory_order_relaxed);
    }
    original(actor,msg);
}
__attribute__((noinline)) inline bool startNMNamedHook(void* nm,const char* name,void* params,bool flag) noexcept {
    const auto original=detail::startNMNamedOriginal;
    std::uintptr_t actor{};
    if(!nm||!detail::nmActor(nm,actor)||!detail::isPlayerActor(actor))return original(nm,name,params,flag);
    char text[48];detail::readName(name,text);
    const auto now=nowNs();
    float hp{NAN};const bool hpRead=detail::readHp(actor,hp);
    const float before=playerHp.load(std::memory_order_relaxed);
    const auto dmg=lastDamageNs.load(std::memory_order_relaxed);
    std::uint32_t running{};const bool runningRead=detail::get(reinterpret_cast<std::uintptr_t>(nm),field::nmRunning,running);
    NmInput in{};in.name=text;in.stumbleOff=cfgStumbleOff.load(std::memory_order_relaxed);
    in.alive=hpRead&&std::isfinite(hp)&&hp>0.f;in.onFoot=playerOnFoot.load(std::memory_order_relaxed);
    in.nmRunning=!runningRead||running!=0; // unreadable = do not interfere
    in.recentDamage=(hpRead&&healthDropped(before,hp))||(dmg&&now>=dmg&&now-dmg<damageWindowNs);
    const auto verdict=nmVerdict(in);
    switch(verdict){
        case NmVerdict::Refuse:nmRefused.fetch_add(1,std::memory_order_relaxed);nmNames.count(text,true);return false; // engine's "not started"
        case NmVerdict::PassDamage:nmPassDamage.fetch_add(1,std::memory_order_relaxed);break;
        case NmVerdict::PassRunning:nmPassRunning.fetch_add(1,std::memory_order_relaxed);break;
        case NmVerdict::PassNotStanding:nmPassNotStanding.fetch_add(1,std::memory_order_relaxed);break;
        case NmVerdict::PassNative:nmPassNative.fetch_add(1,std::memory_order_relaxed);break;
        case NmVerdict::Pass:nmPassOther.fetch_add(1,std::memory_order_relaxed);break;
    }
    nmNames.count(text,false);
    return original(nm,name,params,flag);
}
__attribute__((noinline)) inline bool startNMVoidHook(void* nm) noexcept {
    std::uintptr_t actor{};
    if(nm&&detail::nmActor(nm,actor)&&detail::isPlayerActor(actor)){nmConfigure.fetch_add(1,std::memory_order_relaxed);nmNames.count("<configure>",false);}
    return detail::startNMVoidOriginal(nm);
}
__attribute__((noinline)) inline void nmStartUpHook(void* nm) noexcept {
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    std::uintptr_t actor{};
    if(nm&&detail::nmActor(nm,actor)&&detail::isPlayerActor(actor)){
        nmStartUps.fetch_add(1,std::memory_order_relaxed);
        if(caller==detail::api.base+nmStartUpNamedReturnOffset)nmStartUpsNamed.fetch_add(1,std::memory_order_relaxed);
        else if(caller==detail::api.base+nmStartUpVoidReturnOffset)nmStartUpsUnnamed.fetch_add(1,std::memory_order_relaxed);
    }
    detail::nmStartUpOriginal(nm);
}

// ---- main-thread stick entry (GameXr mapMovement, every pad update) ----
inline void onStick(float x,float y) noexcept {
    const auto now=nowNs();
    publishStick(x,y,now);
    if(!detail::api.base)return;
    // Leaving FREE restores assisted movement even while the player's mover is not stepped.
    if(!(swAssist.load(std::memory_order_relaxed)&&cfgFree.load(std::memory_order_relaxed)))detail::manageAssisted(false);
    detail::releaseIdleImmovable(now);
}

inline bool propertyOn(const char* name) noexcept;
// ---- install (engine init, after the teleport/UpdateInputs block). code(offset, fingerprint) = mapped executable bytes match;
// got(slot, target) = *(base+slot) == base+target; replace(address, expected, replacement) = TraceEntry replacePointer. ----
template<class Code,class Got,class Replace>
inline void install(const EngineApi& api,const Switches& sw,Code&& code,Got&& got,Replace&& replace) noexcept {
    swInstalled[0].store(sw.loco);swInstalled[1].store(sw.bump);swInstalled[2].store(sw.stumble);
    swImmovable.store(sw.immovable);swAssist.store(sw.assist);
    if(!api.read||!api.base||!api.playerManager||!api.playerManagerTable||!api.playerTable||!api.footTable){
        moverInstall.store("NO ENGINE API");bumpInstall.store("NO ENGINE API");nmInstall.store("NO ENGINE API");return;}
    detail::api=api;
    const auto b=api.base;
    if(!sw.loco)moverInstall.store("KILLED");
    else if(!(code(moverInstUpdateOffset,moverInstUpdateFingerprint)&&code(moverUpdateSiteOffset,moverUpdateSiteFingerprint)&&
              code(moverReplaySiteOffset,moverReplaySiteFingerprint)&&code(desiredMotionGateOffset,desiredMotionGateFingerprint)&&
              code(physLevelStepsOffset,physLevelStepsFingerprint)&&got(moverInstUpdateGotOffset,moverInstUpdateOffset)&&
              got(stepCountGotOffset,stepCountOffset)&&got(currentStepGotOffset,currentStepOffset)&&got(gameDataGotOffset,gameDataOffset)&&
              got(simPhysicalSlotOffset,simPhysicalUpdateOffset)&&got(assistedGotOffset,assistedOffset)))moverInstall.store("ENGINE MISMATCH");
    else{
        detail::moverOriginal=reinterpret_cast<MoverUpdateFn>(b+moverInstUpdateOffset);
        if(replace(b+moverInstUpdateGotOffset,b+moverInstUpdateOffset,reinterpret_cast<std::uintptr_t>(&moverUpdateHook))){
            moverInstalled.store(true,std::memory_order_release);moverInstall.store("INSTALLED");}
        else moverInstall.store("HOOK NOT INSTALLED");
    }
    if(!sw.bump)bumpInstall.store("KILLED");
    else if(!(code(broadcastBumpOffset,broadcastBumpFingerprint)&&code(bumpSiteOffset,bumpSiteFingerprint)&&
              got(broadcastBumpGotOffset,broadcastBumpOffset)))bumpInstall.store("ENGINE MISMATCH");
    else{
        detail::bumpOriginal=reinterpret_cast<BroadcastBumpFn>(b+broadcastBumpOffset);
        if(replace(b+broadcastBumpGotOffset,b+broadcastBumpOffset,reinterpret_cast<std::uintptr_t>(&bumpHook))){
            bumpInstalled.store(true,std::memory_order_release);bumpInstall.store("INSTALLED");}
        else bumpInstall.store("HOOK NOT INSTALLED");
    }
    if(!sw.stumble)nmInstall.store("KILLED");
    else if(!(code(startNMNamedOffset,startNMNamedFingerprint)&&code(startNMTailOffset,startNMTailFingerprint)&&
              code(startNMVoidOffset,startNMVoidFingerprint)&&code(nmStartUpOffset,nmStartUpFingerprint)&&
              code(nmTaskStartOffset,nmTaskStartFingerprint)&&got(startNMNamedGotOffset,startNMNamedOffset)&&
              got(startNMVoidGotOffset,startNMVoidOffset)&&got(nmStartUpGotOffset,nmStartUpOffset)))nmInstall.store("ENGINE MISMATCH");
    else{
        detail::startNMNamedOriginal=reinterpret_cast<StartNMNamedFn>(b+startNMNamedOffset);
        detail::startNMVoidOriginal=reinterpret_cast<StartNMVoidFn>(b+startNMVoidOffset);
        detail::nmStartUpOriginal=reinterpret_cast<NMStartUpFn>(b+nmStartUpOffset);
        const std::uintptr_t slots[3]{b+nmStartUpGotOffset,b+startNMVoidGotOffset,b+startNMNamedGotOffset};
        const std::uintptr_t originals[3]{b+nmStartUpOffset,b+startNMVoidOffset,b+startNMNamedOffset};
        const std::uintptr_t hooks[3]{reinterpret_cast<std::uintptr_t>(&nmStartUpHook),reinterpret_cast<std::uintptr_t>(&startNMVoidHook),
            reinterpret_cast<std::uintptr_t>(&startNMNamedHook)};
        unsigned done=0;while(done<3&&replace(slots[done],originals[done],hooks[done]))++done;
        if(done==3){nmInstalled.store(true,std::memory_order_release);nmInstall.store("INSTALLED");}
        else{bool rolled=true;while(done){--done;rolled=replace(slots[done],hooks[done],originals[done])&&rolled;}
            nmInstall.store(rolled?"HOOK NOT INSTALLED":"HOOK ROLLBACK FAILED");}
    }
    // CAMERA YAW HOLD (VrYawHold.h). Needs the mover hook for its gate; own fingerprints/GOT words; kill switch
    // debug.rdrvr.vr.yawhold 0 (with debug.rdrvr.vr.loco 0 it is not installed either).
    if(!moverInstalled.load(std::memory_order_acquire)){yawhold::switchOn.store(sw.loco&&propertyOn("debug.rdrvr.vr.yawhold"));
        yawhold::installState.store(sw.loco?"NO LOCO HOOK":"KILLED");}
    else yawhold::install(api.read,b,propertyOn("debug.rdrvr.vr.yawhold"),&yawHoldGate,code,got,replace);
}
// Property helper for the extra switches (debug.rdrvr.vr.immovable / debug.rdrvr.vr.assist): "0..." = off.
inline bool propertyOn(const char* name) noexcept {
    char value[PROP_VALUE_MAX]{};return __system_property_get(name,value)<=0||value[0]!='0';}
// Opt-in switches (debug.rdrvr.vr.immovable): only an explicit "1..." turns it on; unset = off.
inline bool propertyExplicitOn(const char* name) noexcept {
    char value[PROP_VALUE_MAX]{};return __system_property_get(name,value)>0&&value[0]=='1';}
#endif // __ANDROID__

} // namespace rdrvr::vr::loco
