#pragma once
// BODY - the player's own body in VR instead of floating hands. Pure math: VrArmIk.h (port of
// gta5-quest ArmIk.h / HandRig.h / VrMath.h).
//
//  - Body visible in the eyes: GameXr's eye BodyDrawFilter scopes get a creature that never matches (bodyVisible()); the hat stays
//    hidden. The floating VR hand meshes are not drawn while the module owns the arms (status::bodyHidesHands).
//  - Head collapse (hook C, GOT 0x2e22ad0 grmMatrixSet::Update, only the skinning task's call ret 0x2a2a0f4 and only the player's
//    crSkeleton): the neck subtree (neck, head, face, hair, *_Attachment) is scaled to ~0 about the neck joint, the original builds
//    the skin matrices, the globals are restored bit-exact. Shadow loses the head too (shared matrix set).
//  - Arms (hook A, GOT 0x2e33718 aActor::Broadcast(stdMsgUpdateAttachments), BEFORE the original, rets 0x1d3d484 post-physics /
//    0x1d3d234 compose, player actor only): body yaw towards the head yaw (smoothed), then both arms IK-driven to the controllers
//    (two-bone IK + clavicle assist + soft stretch + pole + forearm roll share + finger curl) over the clavicle subtree, locals and
//    globals written (the same rule as crSkeleton::PartialUpdate). The engine then attaches the weapon to the IK'd hand.
//    Targets: a VR-held animated FIREARM -> the wrist derived from this frame's BARREL placement A (the attachment-bone frame VrWeapon
//    computes; the weapon pose of the same hook scope reuses A, so hand and gun coincide); knife / thrown / lasso / rigid items ->
//    the free-hand mapping (wrist 7 cm behind the grip origin) and the engine's NATIVE attachment (the item hangs in the hand);
//    two-hand long gun support hand -> the foregrip; empty hands -> the free-hand mapping + finger curl from grip / trigger.
//  - Hook B (GOT 0x2e33510 aActor::Broadcast(animMsgUpdateIK), after the original, rets 0x1d3d49c / 0x1d3d24c): the engine's
//    aim / ledge / rein IK may have moved the arms -> drift counted and (default) the arm subtree re-asserted.
//  - Gates (native pose, weights ramp 0.2 s): ragdoll / NM (mover +0xb8c, actor +0x146), mounted / attached, suspended, ledge /
//    ladder, cover, cutscene (linked animation, scripted, locks), dead, not on foot, vehicle, mounted gun, duel.
// Threads: tick() = engine main thread (VrMainThread executor): player, gates, skeleton map -> Seqlock publication. Hooks A / B =
// post-physics sysTask workers / motion-tree workers / main (lock-free for every other actor: one relaxed atomic compare; the
// player path uses the same short mutexes as the VR weapon pose for the controller -> world map). Hook C = the MatrixSetUpdate
// task (bounded spin only between two skin tasks of the PLAYER skeleton). report() any thread.
// Kill switch debug.rdrvr.vr.body 0 (no hooks). Fails closed: any print / GOT word mismatch ->
// UNAVAILABLE, native behaviour.
#include "VrArmIk.h"
#include "VrSettings.h"
#include "VrMainThread.h"
#include "VrHolsters.h"
#include "VrReins.h" // REINS: both arms follow the controllers on a horse
#include "VrReinRope.h" // REINS: the game's rein rope to the VR hand(s) that hold the reins (rein+0x190, hook A)
#include "VrBodyLink.h"
#include "VrLocomotion.h"
#include "VrHandBox.h" // ARC: handBox (the IK hands of this frame for arc::Context::startPoint)
#include "VrTeleportLink.h" // TELEPORT: warp::bodySerial (head-lock / freeze snap after a warp)
#include "VrForearm.h" // Left forearm in the grip frame -> HUD wrist map
#include "VrCutscene.h" // Cutscene presentation -> arms gate CUTSCENE, head collapse off unless FIRST PERSON
#include "VrRideSteadyLink.h" // RIDE STEADY: the horse under the eye (VrMotionActors)
#include "VrMotionBodyLink.h" // John's AppSW parts + this frame's LOCAL -> world map (VrMotionActors)
#include "VrHorseChains.h" // the horse part chains (legs / neck / head)
#include "VrSeat.h" // SEAT: John's upper body seated upright under the VR head on a horse (before the arm IK)
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#if defined(__ANDROID__) || defined(__linux__)
#include <unistd.h>
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define RDRVR_BODY_CALLER() reinterpret_cast<std::uintptr_t>(_ReturnAddress())
#define RDRVR_BODY_NOINLINE __declspec(noinline)
#else
#define RDRVR_BODY_CALLER() reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))
#define RDRVR_BODY_NOINLINE __attribute__((noinline))
#endif

namespace rdrvr::vr::bodyik {
namespace A=::rdrvr::vr::armik;namespace H=::rdrvr::vr::holsters;namespace mt=::rdrvr::vr::mainthread;namespace L=::rdrvr::vr::loco;

inline constexpr std::uintptr_t broadcastAttachOffset=0x1e495a4,broadcastAttachGotOffset=0x2e33718, // aActor::Broadcast(stdMsgUpdateAttachments)
    attachReturnPostPhysOffset=0x1d3d484,attachReturnComposeOffset=0x1d3d234,
    makeHardStopOffset=0x269461c,makeHardStopSimGot=0x2e07110,horseComponentVtable=0x2d24f10,                                                                    // rdrHorseScriptControl::MakeHardStop(sagActor*)
    broadcastIkOffset=0x1e486e4,broadcastIkGotOffset=0x2e33510,                                     // aActor::Broadcast(animMsgUpdateIK)
    ikReturnPostPhysOffset=0x1d3d49c,ikReturnComposeOffset=0x1d3d24c,
    matrixSetUpdateOffset=0x1b02c08,matrixSetUpdateGotOffset=0x2e22ad0,                              // grmMatrixSet::Update
    matrixSetTaskReturnOffset=0x2a2a0f4,matrixSetSiteOffset=0x2a2a0c0,                                // MatrixSetUpdate task call site
    updateAttachmentsAndIkOffset=0x1d3d444,composeAttachSiteOffset=0x1d3d218,updateActorSkelSiteOffset=0x1d2aabc,
    skeletonPartialUpdateOffset=0x1b04b00,skeletonUpdateOffset=0x1b04904;                             // layout proof (not called)
inline constexpr std::array<unsigned char,16> broadcastAttachFingerprint{
#include "gamecode/VrBody_1e495a4_16_8d.inc"
};
inline constexpr std::array<unsigned char,16> broadcastIkFingerprint{
#include "gamecode/VrBody_1e486e4_16_8d.inc"
};
inline constexpr std::array<unsigned char,144> updateAttachmentsAndIkFingerprint{
#include "gamecode/VrBody_1d3d444_144_8d.inc"
};
inline constexpr std::array<unsigned char,64> composeAttachSiteFingerprint{
#include "gamecode/VrBody_1d3d218_64_8d.inc"
};
inline constexpr std::array<unsigned char,284> skeletonPartialUpdateFingerprint{
#include "gamecode/VrBody_1b04b00_284_8d.inc"
};
inline constexpr std::array<unsigned char,64> matrixSetUpdateFingerprint{
#include "gamecode/VrBody_1b02c08_64_8d.inc"
};
inline constexpr std::array<unsigned char,56> matrixSetSiteFingerprint{
#include "gamecode/VrBody_2a2a0c0_56_8d.inc"
};
inline constexpr std::array<unsigned char,64> skeletonUpdateFingerprint{
#include "gamecode/VrBody_1b04904_64_8d.inc"
};
inline constexpr std::array<unsigned char,96> updateActorSkelSiteFingerprint{
#include "gamecode/VrBody_1d2aabc_96_8d.inc"
};

namespace off {
inline constexpr std::size_t actorGuid=0x8,actorAnim=0x38,actorEntity=0x50,actorHealth=0x60,actorMover=0x88,actorFlags112=0x112,actorFlags=0x118,
    actorRagdoll=0x146,actorBlock=0x150;
inline constexpr std::size_t entityInst=0x88,instCache=0x88,instIsFrag=0x190,instSkeletonSlot=0x1f0,cacheSkeleton=0x1a0;
inline constexpr std::size_t skelData=0x8,skelRoot=0x10,skelLocals=0x18,skelCount=0x20,skelGlobals=0x28;
inline constexpr std::size_t dataBones=0x0,dataParents=0x8,dataDefaults=0x20,dataCount=0x30,boneStride=0x110,boneName=0x0,boneIndex=0x28;
inline constexpr std::size_t localStride=0x60,globalStride=0x40,defaultStride=0x40;
inline constexpr std::size_t pmPlayer=0xa20,playerBlock=0x5f8,playerActive=0x5f1,playerState=0x5d4,playerGuid=0x5f4,playerFoot=0x118,
    playerInputLocks=0x104,playerMoveLocks=0x108,playerLock1=0x10c,playerLock2=0x10d,playerScriptedGait=0x390;
inline constexpr std::size_t healthValue=0x20;
} // namespace off
inline constexpr int maxBones=256,maxCollapse=64;

// ============================== configuration (startup properties) ==============================
enum class HeadRoot : std::uint8_t {Neck,Head};
struct Config {
    bool head{true},yaw{true},arms{true},fingers{true},reassert{true},clavicle{true},derive{true},dry{false},mountedArm{true};
    // head lock under the HMD (max 75 cm, linear up to the knee at 0.8 = 60 cm, the camera's lean cap),
    // holster sockets on the body.
    bool lock{true},sockets{true};float lockMaxCm{75.f};
    HeadRoot headRoot{HeadRoot::Neck};
    float stretchCm{10.f},yawTauMs{80.f},yawMaxDeg{180.f},wristBack{A::kWristBack},wristDepth{A::kWristDepth},collapseEps{1e-3f},rampS{.2f};
    float elbowOut{.35f},elbowDown{1.f},elbowBack{.3f},elbowWrist{.4f},rollShare{.5f},clavicleDeg{15.f};
    // the lock references the HMD NECK pivot (standard VR neck model: the eyes 8 cm in front of /
    // 7.5 cm above the point the head pitches about), so nodding moves no body;
    // yaw + lock freeze while looking down (< freezePitchDeg, released above unfreezePitchDeg + freezeHoldS) or while a hand is in a belt
    // holster zone / half-grips near one; a re-engage blends over unfreezeS; frame drop-outs hold the last target holdS.
    float neckBack{.08f},neckDown{.075f};
    // the look-down freeze starts at -50 deg (a higher threshold also freezes while the player looks
    // at the hands / the gun) and ends above -42; a freeze NEVER blocks a real turn or a real step:
    // head heading vs the body yaw target > breakDeg, or the live lock target > lockBreakCm from the
    // frozen one -> breakaway (the body follows with the re-engage smoothing) until the body is back
    // within reengageDeg and the lock blend has finished. The belt-hand freeze needs the hand within
    // beltCoreM of the belt socket (the grab zones are 0.32 m: a hand resting at the hip must not
    // freeze the body).
    bool freeze{true},freezeHands{true};float freezePitchDeg{-50.f},unfreezePitchDeg{-42.f},freezeHoldS{.3f},unfreezeS{.35f};
    float breakDeg{40.f},reengageDeg{8.f},lockBreakCm{15.f},beltCoreM{.22f};
    float nearSocketM{.30f},halfGripLo{.15f},halfGripHi{.65f},holdS{.5f},lockKnee{.8f};
    bool headShadow{true}; // Head back in the shadow (collapsed only for the eye draws); .headshadow 0 = no head in the shadow
    // Support hand on a long gun: palm roll towards the gun's middle, the fore-end centre below the bore line, the hand slides along
    // the barrel between these fractions of grip -> muzzle. LEFT hold: the right hand's animated grip mirrored onto the left fingers.
    float supportTiltDeg{35.f},supportYawDeg{40.f},supportDropM{.025f},supportMin{.2f},supportMax{.6f};bool leftMirror{true};
    bool supportLock{true},supportWorldUp{true}; // .body.supportlock 0 = the hand slides with the controller, .body.supportworldup 0 = gun up
    // RIDING: reinsArm = both arms follow the controllers with REINS on (0 = rein hands); mountRel = the mounted arm memory in
    // the rider frame (0 = world, no hold / crossfade); a missing hand pose holds mountHoldMs, a target-source switch crossfades mountFadeMs;
    // rope = the rein rope mode follows the holding hands (VrReinRope.h: 0 never touch, 1 LEFT / RIGHT / BOTH, 2 no BOTH); ropeFree = no VR
    // hand on the reins -> NONE (the rope on the saddle) instead of the game's own value.
    bool reinsArm{true},mountRel{true};float mountHoldMs{200.f},mountFadeMs{120.f};int rope{1};bool ropeFree{true};
};
inline Config config{};
// HEAD SHADOW state (see matrixSetHook): live = hook C keeps the player set uncollapsed; fillInstalled = GameXr wired the eye-scope fill
inline std::atomic<bool> headShadowLive{},headFillInstalled{};inline std::atomic<const char*> headShadowState{"OFF"};
inline std::atomic<bool> started{};
inline bool parseFloat(const std::string& t,float lo,float hi,float& out) noexcept {
    if(t.empty())return false;char* e{};const float v=std::strtof(t.c_str(),&e);
    if(e==t.c_str()||!std::isfinite(v)||v<lo||v>hi)return false;out=v;return true;
}
// debug.rdrvr.vr.body.{head,yaw,arms,fingers,reassert,clavicle,derive} 0/1, .dry 1 (solve + measure, write nothing), .headroot head,
// .stretch <cm 0..30>, .yawtau <ms>, .yawmax <deg>, .wrist "back depth" (cm), .elbow "out down back wrist", .roll <0..1>, .clavdeg;
// .lock 0 (no head lock under the HMD), .lockmax <cm 0..100> (75), .sockets 0 (holster sockets stay head-relative).
inline void startup() noexcept {
    if(started.exchange(true))return;
    Config c{};
    const auto flag=[](const char* n,bool d){const auto v=H::property(n);return v.empty()?d:v[0]!='0';};
    c.head=flag("debug.rdrvr.vr.body.head",true);c.yaw=flag("debug.rdrvr.vr.body.yaw",true);c.arms=flag("debug.rdrvr.vr.body.arms",true);c.mountedArm=flag("debug.rdrvr.vr.body.mountedarm",true);
    c.fingers=flag("debug.rdrvr.vr.body.fingers",true);c.reassert=flag("debug.rdrvr.vr.body.reassert",true);
    c.clavicle=flag("debug.rdrvr.vr.body.clavicle",true);c.derive=flag("debug.rdrvr.vr.body.derive",true);c.dry=flag("debug.rdrvr.vr.body.dry",false);
    c.lock=flag("debug.rdrvr.vr.body.lock",true);c.sockets=flag("debug.rdrvr.vr.body.sockets",true);
    parseFloat(H::property("debug.rdrvr.vr.body.lockmax"),0.f,100.f,c.lockMaxCm);
    // .freeze 0 (no look-down / holster freeze), .freezehands 0, .freezepitch <deg -80..-10>, .neckmodel "back down" (cm)
    c.freeze=flag("debug.rdrvr.vr.body.freeze",true);c.freezeHands=flag("debug.rdrvr.vr.body.freezehands",true);
    if(parseFloat(H::property("debug.rdrvr.vr.body.freezepitch"),-80.f,-10.f,c.freezePitchDeg))c.unfreezePitchDeg=c.freezePitchDeg+8.f;
    // .freezebreak <deg 10..120> (40: head vs body yaw that always turns the body), .lockbreak <cm 3..60> (15), .beltcore <cm 5..40>
    parseFloat(H::property("debug.rdrvr.vr.body.freezebreak"),10.f,120.f,c.breakDeg);c.reengageDeg=std::min(c.reengageDeg,c.breakDeg*.5f);
    parseFloat(H::property("debug.rdrvr.vr.body.lockbreak"),3.f,60.f,c.lockBreakCm);
    {float cm=c.beltCoreM*100.f;if(parseFloat(H::property("debug.rdrvr.vr.body.beltcore"),5.f,40.f,cm))c.beltCoreM=cm*.01f;}
    c.headShadow=flag("debug.rdrvr.vr.body.headshadow",true); // 0 = the head collapsed in the skinning task (no head in the shadow)
    // .support "tiltDeg yawDeg dropCm" (35 40 2.5), .leftmirror 0 (left hold: procedural grip curl instead of the mirrored right-hand pose)
    {float v[3]{};const auto t=H::property("debug.rdrvr.vr.body.support");
     if(std::sscanf(t.c_str(),"%f %f %f",&v[0],&v[1],&v[2])==3&&std::fabs(v[0])<=90&&std::fabs(v[1])<=90&&v[2]>=-5&&v[2]<=10){
        c.supportTiltDeg=v[0];c.supportYawDeg=v[1];c.supportDropM=v[2]*.01f;}}
    c.leftMirror=flag("debug.rdrvr.vr.body.leftmirror",true);
    c.supportLock=flag("debug.rdrvr.vr.body.supportlock",true);c.supportWorldUp=flag("debug.rdrvr.vr.body.supportworldup",true);
    // RIDING: .reins.arm 0 (rein hands), .body.mountrel 0 (world arm memory on a horse), .body.mounthold <ms 0..1000> (200,
    // 0 = no drop-out hold), .body.mountfade <ms 0..1000> (120, 0 = instant source switch), .reins.rope 0 / 1 / 2, .reins.ropefree 1
    c.reinsArm=flag("debug.rdrvr.vr.reins.arm",true);c.mountRel=flag("debug.rdrvr.vr.body.mountrel",true);
    parseFloat(H::property("debug.rdrvr.vr.body.mounthold"),0.f,1000.f,c.mountHoldMs);parseFloat(H::property("debug.rdrvr.vr.body.mountfade"),0.f,1000.f,c.mountFadeMs);
    {const auto v=H::property("debug.rdrvr.vr.reins.rope");c.rope=v.empty()?1:v[0]=='0'?0:v[0]=='2'?2:1;}
    c.ropeFree=flag("debug.rdrvr.vr.reins.ropefree",true); // the rope rests on the horse until a VR hand grabs it, the left hand never holds the reins by itself; 0 = the native value comes back
    {float v[2]{};const auto t=H::property("debug.rdrvr.vr.body.neckmodel");
     if(std::sscanf(t.c_str(),"%f %f",&v[0],&v[1])==2&&v[0]>=0&&v[0]<=20&&v[1]>=0&&v[1]<=30){c.neckBack=v[0]*.01f;c.neckDown=v[1]*.01f;}}
    { // SEAT (VrSeat.h): .seat 0 (off), .seat.ref 1 (capture), .seat.follow <0..1>, .seat.lean <deg 0..60>, .seat.twist <deg 0..90>,
      // .seat.tau <ms 0..1000>, .seat.ramp <ms 0..2000>, .seat.wait <ms 0..20000, 0 = forever> (a capture not taken by then -> the bind),
      // .seat.reassert 0, .seat.eye 0 (the riding eye keeps a fixed 0.10 m seat)
        namespace S=::rdrvr::vr::seat;S::Config s{};
        s.on=flag("debug.rdrvr.vr.body.seat",true);s.capture=H::property("debug.rdrvr.vr.body.seat.ref")=="1";
        s.reassert=flag("debug.rdrvr.vr.body.seat.reassert",true);s.eye=flag("debug.rdrvr.vr.body.seat.eye",true);
        parseFloat(H::property("debug.rdrvr.vr.body.seat.follow"),0.f,1.f,s.follow);parseFloat(H::property("debug.rdrvr.vr.body.seat.lean"),0.f,60.f,s.leanDeg);
        parseFloat(H::property("debug.rdrvr.vr.body.seat.twist"),0.f,90.f,s.twistDeg);parseFloat(H::property("debug.rdrvr.vr.body.seat.tau"),0.f,1000.f,s.tauMs);
        {float ms=s.rampS*1000.f;if(parseFloat(H::property("debug.rdrvr.vr.body.seat.ramp"),0.f,2000.f,ms))s.rampS=ms*.001f;}
        {float ms=s.waitS*1000.f;if(parseFloat(H::property("debug.rdrvr.vr.body.seat.wait"),0.f,20000.f,ms))s.waitS=ms*.001f;}
        S::config=s;}
    if(H::property("debug.rdrvr.vr.body.headroot")=="head")c.headRoot=HeadRoot::Head;
    parseFloat(H::property("debug.rdrvr.vr.body.stretch"),0.f,30.f,c.stretchCm);
    parseFloat(H::property("debug.rdrvr.vr.body.yawtau"),0.f,2000.f,c.yawTauMs);
    parseFloat(H::property("debug.rdrvr.vr.body.yawmax"),0.f,180.f,c.yawMaxDeg);
    parseFloat(H::property("debug.rdrvr.vr.body.roll"),0.f,1.f,c.rollShare);
    parseFloat(H::property("debug.rdrvr.vr.body.clavdeg"),0.f,45.f,c.clavicleDeg);
    {float v[4]{};const auto t=H::property("debug.rdrvr.vr.body.wrist");
     if(std::sscanf(t.c_str(),"%f %f",&v[0],&v[1])==2&&v[0]>=0&&v[0]<=20&&v[1]>=-10&&v[1]<=10){c.wristBack=v[0]*.01f;c.wristDepth=v[1]*.01f;}}
    {float v[4]{};const auto t=H::property("debug.rdrvr.vr.body.elbow");
     if(std::sscanf(t.c_str(),"%f %f %f %f",&v[0],&v[1],&v[2],&v[3])==4){bool ok=true;for(float x:v)ok=ok&&std::isfinite(x)&&std::fabs(x)<=5.f;
        if(ok){c.elbowOut=v[0];c.elbowDown=v[1];c.elbowBack=v[2];c.elbowWrist=v[3];}}}
    config=c;
    headShadowLive.store(c.headShadow,std::memory_order_relaxed);headShadowState.store(c.headShadow?"ARMED":"OFF",std::memory_order_relaxed);
}

// ============================== engine API (TraceEntry) ==============================
using ReadFn=bool(*)(void*,std::uintptr_t,void*,std::size_t);
// x0 is passed through (u64) so a caller that reads a return value sees the original's, whatever the real return type is.
using BroadcastFn=std::uint64_t(*)(void* actor,const void* message);
using MatrixSetUpdateFn=std::uint64_t(*)(void* set,const void* previous,const void* skeleton,bool a,bool b);
struct EngineApi {
    ReadFn read{};std::uintptr_t base{};
    std::uintptr_t gameData{},playerManager{},playerManagerTable{},playerTable{},actorManager{},actorSlots{},entityVtable{},instSkeletonFn{},
        simVtable{},footTable{};
    H::ContextFn context{};void* contextUser{};
    bool complete() const noexcept {return read&&base&&gameData&&playerManager&&playerManagerTable&&playerTable&&actorManager&&actorSlots&&
        entityVtable&&instSkeletonFn;}
};

// ============================== frame provider (GameXr vrBodyFrame, any thread) ==============================
// World poses of this frame through the VR weapon's map (VrWeapon.h preparePlan + mapRay): the same plan / anchor shift / capsule
// prediction / turn bridge, so the arms and the gun do not lag each other. VR hands: 0 left, 1 right.
// frame = the XR grip pose's axes in world (columns X, Y, Z), gripPos its origin, aimFwd the aim ray (-Z of the aim pose),
// squeeze / trigger the analog grip and trigger.
struct HandPose {bool valid{};A::Mat33 frame{};A::Vec3 gripPos{},aimFwd{};float squeeze{},trigger{};};
struct Frame {
    bool valid{};float upm{1};A::Vec3 up{0,1,0},eye{},headFwd{};bool headValid{};
    std::array<HandPose,2> hand{};
    // the head lock / body offsets (VrBody beforeAttachments from the menu: CAMERA BODY, BODY OFFSET UP / BACK) and the
    // OpenXR LOCAL -> world map of the same shifted plan (GameXr: VrWeapon planMap, scale included) for the holster sockets.
    bool cameraBody{true};float eyeUp{.08f},eyeBack{.10f};
    bool mapValid{};A::Mat34 localToWorld{};std::uint64_t space{};
    bool handsInput{true}; // false = no fresh controller sample (head / plan only: the lock and yaw keep working, the arms have no target)
    std::uint64_t headSampledNs{}; // The plan's head sample = the camera anchor key (sampledNs) the eye comes from; 0 unknown
    int hold{-1},support{-1};std::uintptr_t weapon{};bool firearm{},barrel{},placed{},alt{};
    A::Mat34 attachment{};A::Vec3 muzzle{},barrelDir{};
    // DUAL: the phantom (second item) in the other VR hand, its BARREL placement under the same side rule (GameXr bodyPrePhantom)
    int phantomHand{-1};std::uintptr_t phantomWeapon{};bool phantomFirearm{},phantomBarrel{},phantomPlaced{},phantomAlt{};A::Mat34 phantomAttachment{};
    const char* why{""};
};
using FrameFn=bool(*)(Frame& out,std::uintptr_t actor,std::uint64_t serial,std::uint64_t now) noexcept;
inline std::atomic<FrameFn> frameFn{};

// Hook scope (thread_local, set while hook A's original runs): the weapon pose of the same scope reuses the BODY placement
// (VrWeapon bodyPre) or leaves a HAND-held item to the engine's native attachment.
// leftWeapon: a HAND-held item in the LEFT VR hand - the engine attaches every item to its RIGHT bone, so the weapon pose of the scope
// hands it leftAttachment = the IK'd left wrist_l_attachment global (the item hangs in the left skeletal hand like a native hold).
struct Scope {std::uint64_t serial{};std::uintptr_t nativeWeapon{},leftWeapon{};A::Mat34 leftAttachment{};};
inline thread_local Scope scope{};
inline thread_local std::uint64_t lastAttachSerial{}; // hook A of this thread -> hook B
inline std::uint64_t scopeSerial() noexcept {return scope.serial;}
inline bool nativeHold(std::uintptr_t weapon) noexcept {return scope.serial&&weapon&&scope.nativeWeapon==weapon;}
inline bool leftHold(std::uintptr_t weapon,A::Mat34& attachment) noexcept {
    if(!scope.serial||!weapon||scope.leftWeapon!=weapon)return false;attachment=scope.leftAttachment;return true;}

// ============================== state ==============================
enum Gate : unsigned {GateOk,GateOff,GateNotInstalled,GateNotInGame,GateNoPlayer,GateDead,GateRagdoll,GateMounted,GateSuspended,GateLadder,
    GateCover,GateLocked,GateScripted,GateNotOnFoot,GateCutscene,GateVehicle,GateMountedWeapon,GateGringo,GateDuel,GateContext,GateNoSkeleton,
    GateNoRig,GateStale,GateCount};
inline constexpr std::array<const char*,GateCount> gateNames{"OK","OFF","NOT_INSTALLED","NOT_IN_GAME","NO_PLAYER","DEAD","RAGDOLL","MOUNTED",
    "SUSPENDED","LADDER","COVER","LOCKED","SCRIPTED","NOT_ON_FOOT","CUTSCENE","VEHICLE","MOUNTED_WEAPON","GRINGO","DUEL","CONTEXT","NO_SKELETON",
    "NO_RIG","STALE"};
// Panel glyphs (BODY row note).
inline constexpr std::array<const char*,GateCount> gateNotes{"ACTIVE","OFF","UNAVAILABLE","NOT IN GAME","NO PLAYER","DEAD","RAGDOLL","MOUNTED",
    "SUSPENDED","LADDER","COVER","LOCKED","SCRIPTED","NOT ON FOOT","CUTSCENE","VEHICLE","MOUNTED GUN","GRINGO","DUEL","CONTEXT","NO SKELETON",
    "NO RIG","STALE"};

// Skeleton map, resolved on the main thread per crSkeletonData; published by pointer (4 slots, rewritten only on a new skeleton).
struct Rig {
    std::uintptr_t data{};int count{};std::uint32_t generation{};
    std::array<std::int16_t,maxBones> parent{};
    A::BodyBones bones{};
    std::array<A::ArmRig,2> arm{};std::array<bool,2> armOk{};std::array<float,2> upperCm{},foreCm{};std::array<int,2> fingerChains{};
    bool fingerMirror{}; // The finger chains are mirror images (VrArmIk FingersMirror): the left hold copies the right hand's pose
    int collapseFirst{-1},collapseEnd{-1},collapsePivot{-1};
    // the character's real eyes (facial rig eyePivot_l / _r) and their bind offset from
    // the head joint (head frame, metres)
    std::array<int,2> eyePivot{-1,-1};A::Vec3 eyeBind{};bool eyeBindValid{};
    // The character's RIGHT axis in the head joint's frame (bind pose): up x right = where the head faces, a nod does not change it
    // (FIRST PERSON cutscenes, GameXr cutsceneJohn). headRightFrom = the bones it came from; bindFwdEyeCm = the eyes' bind offset along that forward.
    A::Vec3 headRight{};bool headRightValid{};const char* headRightFrom{"NONE"};float bindFwdEyeCm{};
    ::rdrvr::vr::seat::Chain seat{}; // SEAT: spine00..neck, the bind reference, the spine00 subtree (VrSeat.h BuildChain)
    ::rdrvr::runtime::spacewarp::BoneKeyMap mvKeys{}; // the AppSW part joints (SpaceWarpBones.h names: head .. toe_r)
    const char* state{"NOT_RUN"};
};
inline std::array<Rig,4> rigSlots{};inline std::atomic<unsigned> rigNext{};
struct Published {
    std::uintptr_t actor{},skel{},data{};const Rig* rig{};std::uint32_t count{},generation{};unsigned gate{GateNotInstalled};
    bool armsWanted{},collapse{};std::uint64_t ns{};
    bool mountedArm{}; // RIDING: on a horse the arm whose VR hand holds a weapon follows its controller (REINS on = both arms)
    // SEAT: seat = hold John's upper body seated (a horse after the mount animation); seatCut = the native pose at once (ragdoll,
    // cutscene, dead, no player / skeleton) instead of the ramp out
    bool seat{},seatCut{};
};
inline ::rdrvr::vr::body::Seqlock<Published> published;
inline std::atomic<std::uintptr_t> hookActor{},collapseSkeleton{};
inline std::atomic<bool> installed{},visible{};
inline std::atomic<const char*> installState{"NOT ATTEMPTED"};
inline std::atomic<unsigned> gateNow{GateNotInstalled};
// True while the body is shown in the eyes (menu BODY ON + every hook installed): GameXr gives the eye BodyDrawFilter scopes a
// creature that never matches (the hat node stays hidden).
inline bool bodyVisible() noexcept {return visible.load(std::memory_order_acquire);}
inline constexpr std::uintptr_t showBodyCreature=1; // never a crCreature address

using Counter=std::atomic<std::uint64_t>;
inline void bump(Counter& c,std::uint64_t n=1) noexcept {c.fetch_add(n,std::memory_order_relaxed);}
inline void maxOf(std::atomic<float>& a,float v) noexcept {float m=a.load(std::memory_order_relaxed);while(v>m&&!a.compare_exchange_weak(m,v)){}}
struct ArmStats {
    Counter applied{},clamped{},stretched{},clavicle{},derived{},support{},free{},nativeHold{},noTarget{},poseFailed{},fingers{};
    Counter holdMirror{},holdGrip{},itemAttach{}; // LEFT hold: fingers mirrored / procedural grip, item on the left attachment bone
    std::atomic<float> weight{},clampCmMax{},stretchCmMax{},clavicleDegMax{},targetErrMmMax{},lastClampCm{},lastTwistDeg{};
    Counter driftFrames{},driftOver1cm{},reasserted{};std::atomic<float> driftCmMax{},driftDegMax{},lastDriftCm{};
    // RIDING (rider-frame memory only): wanted but no hand pose, held, holds that ran out, blend-out frames, crossfades + their frames
    Counter mountNone{},mountHold{},mountHoldExpired{},mountBlendOut{},mountFades{},mountFadeFrames{};
};
// oscillation of a signal (cm) in 2 s windows: peak-to-peak and frequency (crossings of its 1 s running mean, 0.3 cm hysteresis).
struct OscOut {std::atomic<float> ptpCm{},hz{},ptpMaxCm{},meanCm{},lastCm{};Counter windows{};};
struct Osc {
    bool init{};float mean{},lo{1e30f},hi{-1e30f},t{},last{};int crossings{},side{};
    static constexpr float window=2.f,hysteresis=.3f,meanTau=1.f;
    void add(float v,float dt,OscOut& out) noexcept {
        if(!std::isfinite(v)||!std::isfinite(dt)||dt<0.f)return;
        last=v;if(!init){mean=v;init=true;}
        mean+=(v-mean)*A::SmoothFactor(dt,meanTau);
        const float d=v-mean;const int s=d>hysteresis?1:d<-hysteresis?-1:0;
        if(s&&s!=side){if(side)++crossings;side=s;}
        lo=std::min(lo,v);hi=std::max(hi,v);t+=dt;
        out.lastCm.store(v,std::memory_order_relaxed);out.meanCm.store(mean,std::memory_order_relaxed);
        if(t>=window){const float ptp=hi-lo;out.ptpCm.store(ptp,std::memory_order_relaxed);out.hz.store(float(crossings)/(2.f*t),std::memory_order_relaxed);
            maxOf(out.ptpMaxCm,ptp);bump(out.windows);lo=1e30f;hi=-1e30f;t=0.f;crossings=0;}
    }
};
enum OscSignal : unsigned {OscSwayFwd,OscSwayRight,OscHeadRootFwd,OscRootPivotFwd,OscResidualFwd,OscCount};
inline constexpr std::array<const char*,OscCount> oscNames{"swayFwd","swayRight","headVsRootFwd","rootVsPivotFwd","residualFwd"};
struct Stats {
    std::array<Counter,GateCount> gates{};
    // body-vs-camera oscillation, head lock, eye vs shoulders, holster sockets, yaw freeze looking down
    std::array<OscOut,OscCount> osc{};
    Counter lockApplied{},lockClamped{},lockNoPivot{};std::atomic<float> lockLastCm{},lockCmMax{},lockWeight{};
    std::atomic<float> shoulderBelowEyeCm{},shoulderBehindEyeCm{},neckBelowEyeCm{},headAboveRootCm{};
    Counter socketPublishes{},socketItemFrames{};std::atomic<unsigned> socketMask{},socketFromItem{};
    Counter yawPitchFrozen{};Counter ticks{},exceptions{},rigResolves{},rigFailures{};
    // freeze (look down / hand at a belt holster), hold through frame drop-outs, head-only frames (no controller sample),
    // upright placement means (eye vs head joint / shoulders / neck, |pitch| < 20 deg, 2 s EMA)
    Counter freezeFrames{},freezeEnters{},freezeByPitch{},freezeByHand{},lockHeld{},headOnlyFrames{},uprightSamples{};
    Counter charEyeFrames{};std::atomic<float> charEyeAboveHeadCm{},charEyeFwdOfHeadCm{},charEyeAboveRootCm{}; // the character's eye pivots
    std::atomic<float> lastPitchDeg{},eyeAboveHeadMean{},eyeFwdHeadMean{},eyeAboveShoulderMean{},eyeAboveNeckMean{};std::atomic<bool> frozenNow{};
    // breakaway (a freeze never blocks a real turn / step), head heading vs the body yaw target, the pitch latch itself
    Counter freezeBreaks{},breakByYaw{},breakByLock{},breakFrames{},latchClears{};std::atomic<float> headVsBodyDeg{},headVsBodyDegMax{};
    Counter warpSnaps{}; // TELEPORT: warps whose head lock / freeze was snapped (once per serial)
    Counter warpSnapFrames{},warpSnapTimeouts{}; // Hook-A frames held in the snap (until the head is post-B-snap), 300 ms caps
    std::atomic<bool> breakNow{},pitchLatchedNow{};
    Counter callsPostPhys{},callsCompose{},callsOtherLR{},playerCalls{},busy{},skelMismatch{},ragdollByte{},frameOk{},frameFail{},dryFrames{},mountedArmFrames{};
    std::atomic<const char*> frameWhy{""};
    std::array<ArmStats,2> arm{};
    Counter yawApplied{},yawSticky{},yawUndo{};std::atomic<float> yawLastDeg{},yawMaxDeg{},yawWeight{};
    Counter ikCalls{},ikOtherLR{};
    Counter headCalls{},headApplied{},headOtherLR{},headContended{},headSkipped{},headRestoreMismatch{};
    Counter eyeSamples{};std::atomic<float> eyeCm{},eyeFwdCm{},eyeUpCm{},eyeRightCm{},eyeCmMax{};
    Counter aNsSum{},aCount{},bNsSum{},bCount{},cNsSum{},cCount{},tickNsSum{};std::atomic<float> aUsMax{},bUsMax{},cUsMax{},tickUsMax{};
    std::array<std::atomic<std::uint64_t>,8> threads{};
    Counter weaponDerivedMismatch{},weaponNoPlacement{};
};
inline Stats stats{};
inline void noteThread() noexcept {
#if defined(__ANDROID__) || defined(__linux__)
    const auto id=std::uint64_t(::gettid());
#else
    const auto id=std::uint64_t(std::hash<std::thread::id>{}(std::this_thread::get_id()))|1ull;
#endif
    for(auto& t:stats.threads){auto v=t.load(std::memory_order_relaxed);if(v==id)return;
        if(!v&&t.compare_exchange_strong(v,id,std::memory_order_relaxed))return;if(v==id)return;}
}

// ============================== per-frame IK state (one writer: the thread that runs the player's hook A) ==============================
// RIDING: the target source (a switch crossfades on a horse); Hold = a drop-out hold or a blend-out of the last target
enum ArmSource : unsigned {SourceNone,SourceFree,SourceDerived,SourceSupport,SourceHold};
struct ArmMemory {float weight{};A::Vec3 lastPerp{},hingeLocal{};A::Mat34 lastTarget{};A::FingerMode lastFingers{A::FingerMode::Native};bool haveLast{};
    // the last target in the rider frame (yawFrame), the last live frame, the source + its crossfade start (rider frame), grip
    A::Mat34 lastRel{},fadeFrom{};std::uint64_t liveNs{};unsigned source{SourceNone},liveSource{SourceNone};float fadeT{1.f},lastGrip{},lastTrigger{};
    bool haveRel{},holding{},wanted{};};
struct Saved { // what hook A wrote, for hook B
    std::uint64_t serial{};std::uintptr_t skel{};bool valid[2]{};int first[2]{},count[2]{};
    std::array<std::array<A::Mat34,A::kMaxArm>,2> local{};std::array<A::Mat34,2> wrist{};
    ::rdrvr::vr::seat::Saved seat{}; // SEAT: the chain locals hook A wrote
};
struct IkState {
    std::uint64_t lastNs{};std::array<ArmMemory,2> arm{};float yawWeight{};
    bool haveYaw{};A::Vec3 smoothFwd{};
    // body yaw bookkeeping (VrBody.h applyYaw): the L0 hook A wrote and the rotation baked into it
    // (when the engine does not rewrite L0)
    std::uintptr_t yawSkel{};bool haveL0{};A::Mat34 writtenL0{};float acc{};
    // head lock: the horizontal translation baked (sticky L0), its weight, the last animation->target offset (blend-out)
    float lockWeight{};A::Vec3 bakedT{},lastLockDv{};bool haveLockDv{};
    // the lock target (relative to the root), freeze state (pitch latch, frozen offset, re-engage blend), head drop-out hold,
    // last frame's belt sockets (world) for the half-grip freeze, upright placement means
    std::uint64_t lastHeadNs{},freezeWantNs{};bool haveTarget{},frozen{},pitchLatched{};A::Vec3 lastTargetRel{},frozenRel{};float unfreezeT{1.f};
    bool breakaway{}; // The freeze is overridden (large turn / step) until the body caught up
    std::uint64_t warpSeen{}; // TELEPORT: the last warp serial this state snapped for
    std::uint64_t warpSinceNs{};bool warpPending{}; // The snap holds until this frame's head is drawn from the camera's snap
    std::array<A::Vec3,7> socketWorld{};unsigned socketWorldMask{};bool haveUpright{};float upHead{},fwdHead{},upShoulder{},upNeck{};
    std::array<Osc,OscCount> osc{};
    // sockets: the final pelvis of the previous frame (the holstered items were attached to that skeleton) and each slot's
    // holstered-item offset in the pelvis frame (smoothed; kept while the item is drawn).
    bool havePrevPelvis{};A::Mat34 prevPelvis{};std::array<A::Vec3,7> itemOffset{};std::array<bool,7> itemValid{};std::uintptr_t socketSkel{};
    Saved saved{};
};
inline IkState ik{};
inline std::atomic<bool> busy{};
inline std::atomic<std::uint64_t> serialCounter{1};

namespace detail {
inline EngineApi api{};
inline BroadcastFn attachOriginal{},ikOriginal{};inline MatrixSetUpdateFn matrixOriginal{};
template<class T> inline bool get(std::uintptr_t p,std::size_t o,T& out) noexcept {
    return api.read&&p&&p<=UINTPTR_MAX-o-sizeof(T)&&api.read(nullptr,p+o,&out,sizeof(T));}
inline bool block(std::uintptr_t p,void* out,std::size_t n) noexcept {return api.read&&p&&p<=UINTPTR_MAX-n&&api.read(nullptr,p,out,n);}
template<class T> inline T at(const unsigned char* b,std::size_t o) noexcept {T v{};std::memcpy(&v,b+o,sizeof v);return v;}
// Plain loads of live engine memory (hook threads; pointers the engine itself is using for this actor right now).
template<class T> inline T raw(std::uintptr_t p) noexcept {T v{};std::memcpy(&v,reinterpret_cast<const void*>(p),sizeof v);return v;}
inline std::uint64_t now() noexcept {return ::rdrvr::vr::nowNs();}
inline void addTime(Counter& sum,Counter& n,std::atomic<float>& mx,std::uint64_t ns) noexcept {
    bump(sum,ns);bump(n);maxOf(mx,float(double(ns)/1000.0));}
} // namespace detail

// ============================== main-thread tick: player, gates, skeleton map ==============================
struct PlayerRead {std::uintptr_t player{},actor{},mover{};std::uint32_t guid{},state{};bool alive{};};
inline unsigned readPlayer(PlayerRead& p) noexcept {
    using namespace detail;const auto& ea=detail::api;
    std::uint32_t gs{};if(!get(ea.gameData,0,gs))return GateNotInGame;if(gs!=3)return GateNotInGame;
    std::uintptr_t table{};
    if(!get(ea.playerManager,0,table)||table!=ea.playerManagerTable||!get(ea.playerManager,off::pmPlayer,p.player)||!p.player)return GateNoPlayer;
    alignas(8) static thread_local unsigned char pb[off::playerBlock];
    if(!block(p.player,pb,sizeof pb)||at<std::uintptr_t>(pb,0)!=ea.playerTable||pb[off::playerActive]!=1)return GateNoPlayer;
    p.guid=at<std::uint32_t>(pb,off::playerGuid);p.state=at<std::uint32_t>(pb,off::playerState);
    std::uintptr_t manager{},slots{};std::uint16_t n{};std::uint8_t entry[16]{};
    if(!get(ea.actorManager,0,manager)||!get(manager,0,n)||!get(ea.actorSlots,0,slots)||!slots)return GateNoPlayer;
    const std::uint32_t index=p.guid&0xffffu;
    if(index>=n||!block(slots+index*16,entry,sizeof entry))return GateNoPlayer;
    p.actor=at<std::uintptr_t>(entry,0);if(!p.actor||at<std::uint16_t>(entry,8)!=(p.guid>>16))return GateNoPlayer;
    std::uint32_t flags{},guid{};if(!get(p.actor,off::actorFlags,flags)||(flags&3u)!=3u||!get(p.actor,off::actorGuid,guid)||guid!=p.guid)return GateNoPlayer;
    std::uintptr_t health{};float hp{};p.alive=get(p.actor,off::actorHealth,health)&&health&&get(health,off::healthValue,hp)&&std::isfinite(hp)&&hp>0;
    return GateOk;
}
// Mover / foot / lock gates with LOCO's field offsets - the same facts the LOCO gate evaluates.
inline unsigned moverGate(const PlayerRead& p) noexcept {
    using namespace detail;namespace F=L::field;const auto& ea=detail::api;
    std::uintptr_t mover{};
    {const auto lm=L::playerMover.load(std::memory_order_relaxed);if(lm&&L::playerActor.load(std::memory_order_relaxed)==p.actor)mover=lm;}
    std::uintptr_t vptr{},owner{};
    if(!mover){std::uintptr_t m{};if(get(p.actor,off::actorMover,m)&&m)mover=m;}
    bool moverOk=mover&&ea.simVtable&&get(mover,0,vptr)&&vptr==ea.simVtable&&get(mover,F::moverActor,owner)&&owner==p.actor;
    if(moverOk){
        std::uint32_t ragdoll{},ledge{},special{};std::uintptr_t attachment{};std::int16_t suspend{},frozen{};std::uint8_t cover{};
        if(get(mover,F::moverRagdoll,ragdoll)&&ragdoll)return GateRagdoll;
        if(get(mover,F::moverAttachment,attachment)&&attachment)return GateMounted;
        if((get(mover,F::moverSuspend,suspend)&&suspend>0)||(get(mover,F::moverFrozen,frozen)&&frozen>0))return GateSuspended;
        if((get(mover,F::moverLedge,ledge)&&ledge)||(get(mover,F::moverSpecial,special)&&special))return GateLadder;
        if(get(mover,F::moverCover,cover)&&cover)return GateCover;
    }
    std::uint8_t rag{};if(get(p.actor,off::actorRagdoll,rag)&&(rag&3u))return GateRagdoll;
    std::uint8_t gd15{},gd16{};std::uint16_t f112{};
    if(get(ea.gameData,0x15,gd15)&&get(ea.gameData,0x16,gd16)&&get(p.actor,off::actorFlags112,f112)&&gd15&&(f112&4u)&&!gd16)return GateScripted;
    if(p.state!=2)return GateNotOnFoot;
    std::uint16_t inputLocks{},moveLocks{};std::uint8_t lock1{},lock2{};std::int32_t gait{-1};
    if(get(p.player,off::playerInputLocks,inputLocks)&&get(p.player,off::playerMoveLocks,moveLocks)&&get(p.player,off::playerLock1,lock1)&&
       get(p.player,off::playerLock2,lock2)&&get(p.player,off::playerScriptedGait,gait)&&(inputLocks||moveLocks||lock1||lock2||gait>=0))return GateLocked;
    std::uintptr_t foot{},footTable{},footPlayer{};std::uint32_t footMode{};std::uint8_t footDisabled{};
    if(ea.footTable&&get(p.player,off::playerFoot,foot)&&foot&&get(foot,0,footTable)&&footTable==ea.footTable&&get(foot,F::footPlayer,footPlayer)&&
       footPlayer==p.player&&get(foot,F::footMode,footMode)&&get(foot,F::footDisabled,footDisabled)&&(footMode||(footDisabled&1u)))return GateLadder;
    return GateOk;
}
inline unsigned contextGate(const H::PlayerBlocks& b) noexcept {
    if(!b.alive)return GateDead;
    if(b.linkedAnimation)return GateCutscene;
    if(b.controlDisabled)return GateLocked;
    if(b.riding||b.mountingHorse)return GateMounted;
    if(b.ridingVehicle||b.vehicleTransition)return GateVehicle;
    if(b.mountedWeapon)return GateMountedWeapon;
    if(b.usingGringo)return GateGringo;
    if(b.duel)return GateDuel;
    if(b.cover)return GateCover;
    return GateOk;
}
// Skeleton chain (VrWeapon deriveHandBone / VrMelee deriveChain): actor+0x50 entity -> +0x88 inst -> +0x88 cache -> +0x1a0 crSkeleton.
struct SkelRead {std::uintptr_t skel{},data{},locals{},globals{},root{};std::uint32_t count{};};
// The fields of a crSkeleton (s.skel set).
inline bool readSkeletonFields(SkelRead& s) noexcept {
    using namespace detail;std::uint16_t dataCount{};
    if(!get(s.skel,off::skelData,s.data)||!s.data||!get(s.skel,off::skelLocals,s.locals)||!s.locals||!get(s.skel,off::skelGlobals,s.globals)||!s.globals||
       !get(s.skel,off::skelRoot,s.root)||!s.root||!get(s.skel,off::skelCount,s.count)||!get(s.data,off::dataCount,dataCount))return false;
    return s.count>0&&s.count<=std::uint32_t(maxBones)&&dataCount==s.count;
}
inline bool readSkeleton(std::uintptr_t actor,SkelRead& s) noexcept {
    using namespace detail;const auto& ea=detail::api;
    std::uintptr_t ent{},vt{},inst{},slot{},cache{};std::uint8_t frag{};
    if(!get(actor,off::actorEntity,ent)||!ent||!get(ent,0,vt)||vt!=ea.entityVtable)return false;
    if(!get(ent,off::entityInst,inst)||!inst||!get(inst,0,vt)||!vt||!get(vt,off::instSkeletonSlot,slot)||slot!=ea.instSkeletonFn)return false;
    if(!get(inst,off::instIsFrag,frag)||!frag||!get(inst,off::instCache,cache)||!cache||!get(cache,off::cacheSkeleton,s.skel)||!s.skel)return false;
    return readSkeletonFields(s);
}
// Default (bind) transform of `bone` in the frame of `ancestor` (composed default locals). ancestor -1 = the model frame: bone 0 is the top whatever
// its parent index says (crSkeleton::Update 0x1b04904 never reads parents[0]; a weapon skeleton's is 0, a self loop, log vr.weapon.reload.bones).
inline bool composeTo(int bone,int ancestor,const Rig& r,const A::Mat34* defaults,A::Mat34& out) noexcept {
    out=A::Mat34{};int b=bone;
    for(int guard=0;guard<16;++guard){if(b==ancestor)return true;if(b<0||b>=r.count)return false;out=defaults[b]*out;b=b==0?-1:r.parent[std::size_t(b)];}
    return false;
}
inline bool resolveRig(const SkelRead& s,Rig& r) noexcept {
    using namespace detail;
    r=Rig{};r.data=s.data;r.count=int(s.count);r.state="READING";
    std::uintptr_t bones{},parents{},defaultsPtr{};
    if(!get(s.data,off::dataBones,bones)||!bones||!get(s.data,off::dataParents,parents)||!parents||!get(s.data,off::dataDefaults,defaultsPtr)||!defaultsPtr){
        r.state="DATA";return false;}
    static thread_local std::array<std::array<char,32>,maxBones> names{};
    static thread_local std::array<std::int32_t,maxBones> parentRaw{};
    static thread_local std::array<std::array<float,16>,maxBones> defaultRaw{};
    if(!block(parents,parentRaw.data(),sizeof(std::int32_t)*std::size_t(r.count))||
       !block(defaultsPtr,defaultRaw.data(),sizeof(float)*16*std::size_t(r.count))){r.state="PARENTS";return false;}
    bool indexOrder=true;
    for(int i=0;i<r.count;++i){
        const auto bd=bones+std::uintptr_t(i)*off::boneStride;std::uintptr_t name{};std::uint16_t index{};
        names[std::size_t(i)].fill(0);
        if(get(bd,off::boneIndex,index)&&index!=i)indexOrder=false;
        if(get(bd,off::boneName,name)&&name){char t[32]{};if(block(name,t,24)||block(name,t,8)){t[24]=0;std::memcpy(names[std::size_t(i)].data(),t,32);}}
        const int p=parentRaw[std::size_t(i)];r.parent[std::size_t(i)]=std::int16_t(p>=-1&&p<r.count?p:-2);
    }
    const auto parentAt=[&](int i){return i>=0&&i<r.count?int(r.parent[std::size_t(i)]):-1;};
    const auto nameAt=[&](int i)->const char*{return i>=0&&i<r.count&&names[std::size_t(i)][0]?names[std::size_t(i)].data():nullptr;};
    r.bones=A::ResolveBones(r.count,nameAt,parentAt);r.bones.indexOrder=indexOrder;
    {static thread_local std::array<const char*,maxBones> ptrs{};for(int i=0;i<r.count;++i)ptrs[std::size_t(i)]=nameAt(i); // part joints
     r.mvKeys=::rdrvr::runtime::spacewarp::resolveBoneKeys(ptrs.data(),unsigned(r.count));}
    if(!indexOrder){r.state="INDEX_ORDER";return false;}
    static thread_local std::array<A::Mat34,maxBones> defaults{};
    for(int i=0;i<r.count;++i)defaults[std::size_t(i)]=A::LoadMat34(defaultRaw[std::size_t(i)].data());
    for(int s2=0;s2<2;++s2){
        const auto& b=r.bones.arm[std::size_t(s2)];auto& rig=r.arm[std::size_t(s2)];
        if(!A::BuildArmRig(b,parentAt,rig))continue;
        const bool right=s2==0;rig.armRel=b.arm-b.clavicle;
        A::Mat34 e{},w{};
        if(composeTo(b.elbow,b.arm,r,defaults.data(),e))r.upperCm[std::size_t(s2)]=A::Length(e.d)*100.f;
        if(composeTo(b.wrist,b.elbow,r,defaults.data(),w)){r.foreCm[std::size_t(s2)]=A::Length(w.d)*100.f;rig.rollAxis=A::Normalize(w.d,{1,0,0});}
        // hand frame from the bind positions of the finger bases in the wrist frame
        A::Mat34 th{},ix{},mi{},pk{};
        const bool thumb=b.finger[A::kThumb][0]>=0&&composeTo(b.finger[A::kThumb][0],b.wrist,r,defaults.data(),th);
        const bool middle=b.finger[A::kMiddle][0]>=0&&composeTo(b.finger[A::kMiddle][0],b.wrist,r,defaults.data(),mi);
        const bool ip=b.finger[A::kIndexFinger][0]>=0&&b.finger[A::kPinky][0]>=0&&composeTo(b.finger[A::kIndexFinger][0],b.wrist,r,defaults.data(),ix)&&
            composeTo(b.finger[A::kPinky][0],b.wrist,r,defaults.data(),pk);
        if(thumb&&middle)rig.hand=A::MeasureHandFrame(th.d,ip,ix.d,mi.d,pk.d,right);
        // finger curl (bind locals, parent rotation in the wrist frame)
        A::FingerJointBind fb[A::kFingers][A::kJoints]{};int chains=0;
        for(int f=0;f<A::kFingers;++f){bool ok=true;
            for(int j=0;j<A::kJoints;++j){const int bone=b.finger[f][j];auto& jb=fb[f][j];
                A::Mat34 pos{},par{};
                if(bone<0||!composeTo(bone,b.wrist,r,defaults.data(),pos)||!composeTo(r.parent[std::size_t(bone)],b.wrist,r,defaults.data(),par)){ok=false;continue;}
                jb.valid=true;jb.bindLocal=A::ToQuat(A::Orthonormalize(defaults[std::size_t(bone)].Rot()));jb.parentInHand=A::Orthonormalize(par.Rot());jb.posInHand=pos.d;}
            chains+=ok?1:0;}
        if(rig.hand.valid){rig.curl=A::BuildFingerCurl(fb,rig.hand.palmOut);rig.fingersValid=chains>0;}
        r.fingerChains[std::size_t(s2)]=chains;
        const bool lengths=r.upperCm[std::size_t(s2)]>=15.f&&r.upperCm[std::size_t(s2)]<=45.f&&r.foreCm[std::size_t(s2)]>=15.f&&r.foreCm[std::size_t(s2)]<=45.f;
        r.armOk[std::size_t(s2)]=lengths&&rig.hand.valid;
    }
    r.fingerMirror=r.arm[0].fingersValid&&r.arm[1].fingersValid&&A::FingersMirror(r.arm[0].curl,r.arm[1].curl,12.f);
    // the eye pivots (left, right) by name; their bind midpoint in the head joint's frame
    for(int i=0;i<r.count;++i){const char* n=nameAt(i);if(!n)continue;
        if(std::strcmp(n,"eyePivot_l")==0||(r.eyePivot[0]<0&&std::strcmp(n,"eye_l")==0))r.eyePivot[0]=i;
        if(std::strcmp(n,"eyePivot_r")==0||(r.eyePivot[1]<0&&std::strcmp(n,"eye_r")==0))r.eyePivot[1]=i;}
    if(r.eyePivot[0]>=0&&r.eyePivot[1]>=0&&r.bones.head>=0){A::Mat34 el{},er{};
        if(composeTo(r.eyePivot[0],r.bones.head,r,defaults.data(),el)&&composeTo(r.eyePivot[1],r.bones.head,r,defaults.data(),er)){
            r.eyeBind=(el.d+er.d)*.5f;r.eyeBindValid=A::Finite(r.eyeBind);}}
    // the head's right axis: the eyes' left-right line of the bind pose, else the clavicles'. Sign from the anatomy, not from a name or a
    // bone axis: up (pelvis / neck -> head) x right must point to the side of the head joint the eyes are on.
    if(r.bones.head>=0){A::Mat34 hm{},pa{},pb{},from{};A::Vec3 right{};const int cr=r.bones.arm[0].clavicle,cl=r.bones.arm[1].clavicle;
        const int upBone=r.bones.pelvis>=0?r.bones.pelvis:r.bones.neck;const auto* d=defaults.data();
        const bool head=composeTo(r.bones.head,-1,r,d,hm),eyes=head&&r.eyePivot[0]>=0&&r.eyePivot[1]>=0&&composeTo(r.eyePivot[1],-1,r,d,pa)&&composeTo(r.eyePivot[0],-1,r,d,pb);
        if(eyes&&A::Length(pa.d-pb.d)>.01f){right=pa.d-pb.d;r.headRightFrom="EYES";}
        else if(head&&cr>=0&&cl>=0&&composeTo(cr,-1,r,d,pa)&&composeTo(cl,-1,r,d,pb)&&A::Length(pa.d-pb.d)>.01f){right=pa.d-pb.d;r.headRightFrom="CLAVICLES";}
        if(A::Length(right)>.01f&&A::Finite(right)){right=A::Normalize(right);
            if(eyes&&upBone>=0&&composeTo(upBone,-1,r,d,from)){A::Mat34 el{},er{};
                if(composeTo(r.eyePivot[0],-1,r,d,el)&&composeTo(r.eyePivot[1],-1,r,d,er)){
                    float fwd=A::Dot(A::Normalize(A::Cross(hm.d-from.d,right)),(el.d+er.d)*.5f-hm.d);
                    if(fwd<0.f){right=right*-1.f;fwd=-fwd;r.headRightFrom="EYES_FLIPPED";}
                    r.bindFwdEyeCm=fwd*100.f;}}
            r.headRight=A::Normalize(A::Transpose(hm.Rot())*right);r.headRightValid=A::Finite(r.headRight);}}
    // SEAT: the head's ancestors down to the root bone / pelvis (spine00..neck), their bind reference, the spine00 subtree
    ::rdrvr::vr::seat::BuildChain(r.count,r.bones.head,r.bones.neck,r.bones.pelvis,r.bones.hip[0],r.bones.hip[1],r.bones.arm[0].clavicle,r.bones.arm[1].clavicle,
                                  parentAt,defaults.data(),r.seat);
    // head collapse range: the neck subtree about the neck joint (default) or the head subtree about the head joint
    const bool neck=config.headRoot==HeadRoot::Neck&&r.bones.neck>=0&&r.bones.head>r.bones.neck&&r.bones.head<r.bones.neckEnd;
    if(neck){r.collapseFirst=r.bones.neck;r.collapseEnd=r.bones.neckEnd;r.collapsePivot=r.bones.neck;}
    else if(r.bones.head>=0){r.collapseFirst=r.bones.head;r.collapseEnd=r.bones.headEnd;r.collapsePivot=r.bones.head;}
    if(r.collapseEnd-r.collapseFirst>maxCollapse||r.collapseFirst<0||r.collapseEnd<=r.collapseFirst){r.collapseFirst=r.collapseEnd=r.collapsePivot=-1;}
    r.state=!r.armOk[0]&&!r.armOk[1]?"NO_ARMS":r.collapseFirst<0?"NO_HEAD":"OK";
    return r.armOk[0]||r.armOk[1];
}
inline const Rig* currentRig{};inline std::uint32_t rigGeneration{};
inline H::PlayerBlocks lastBlocks{};inline bool blocksValid{};inline std::uint64_t blocksTick{},tickIndex{};
inline void publishStatus(unsigned gate) noexcept {
    using namespace ::rdrvr::vr;
    if(killed(Feature::Body)){status::publish(Feature::Body,status::Property);return;}
    if(!installed.load(std::memory_order_acquire)){status::publish(Feature::Body,status::Unavailable);return;}
    status::publish(Feature::Body,status::Ready,settings().body()?gateNotes[gate<GateCount?gate:GateContext]:nullptr);
}
inline void tick(const mt::Frame& frame) noexcept {
    const auto t0=detail::now();++tickIndex;bump(stats.ticks);
    { // hard-stop decel boost (VrReins.h hsSoftDecel): the mount's brake tune x .hardmult while the hard stop runs, put back after
        namespace rn=::rdrvr::vr::reins;static std::uintptr_t boostTune{};static float boostOrig{};static float mult{15.f};static std::uint64_t multNs{};
        if(!multNs||t0-multNs>1'000'000'000ull){multNs=t0;char v[PROP_VALUE_MAX]{};mult=__system_property_get("debug.rdrvr.vr.reins.hardmult",v)>0?
            std::clamp(float(std::atof(v)),1.f,40.f):15.f;} // x15: at x4 (1.0 -> 4.0) a stop from a gallop takes over 2 s
        const auto until=rn::hardStopUntil.load(std::memory_order_relaxed);const bool on=until&&t0<until;
        const auto horse=::rdrvr::vr::steadylink::mountActor.load(std::memory_order_relaxed);
        if(on&&horse&&detail::api.base&&!boostTune){const auto comp=detail::raw<std::uintptr_t>(horse+0x68);
            if(comp&&detail::raw<std::uintptr_t>(comp)==detail::api.base+horseComponentVtable+0x10){const auto tune=detail::raw<std::uintptr_t>(comp+0x1170);
                const float soft=tune?detail::raw<float>(tune):-1.f,hard=tune?detail::raw<float>(tune+4):-1.f;
                rn::hsSoftDecel.store(soft,std::memory_order_relaxed);rn::hsHardDecel.store(hard,std::memory_order_relaxed);
                if(tune&&std::isfinite(hard)&&hard>0.f&&hard<200.f){boostTune=tune;boostOrig=hard;const float w=hard*mult;std::memcpy(reinterpret_cast<void*>(tune+4),&w,4);
                    rn::hsHardUsed.store(w,std::memory_order_relaxed);rn::hsTuneBoosts.fetch_add(1,std::memory_order_relaxed);}else rn::hsTuneBad.fetch_add(1,std::memory_order_relaxed);}
            else rn::hsTuneBad.fetch_add(1,std::memory_order_relaxed);}
        if(!on&&boostTune){std::memcpy(reinterpret_cast<void*>(boostTune+4),&boostOrig,4);boostTune=0;}
        // DECAY DELAY: without this the brake starts a couple of seconds after the gesture.
        // hrsSpeedControl::Update 0x21f62a8 (the speed control = hrsHorseComponent+0x50, tune [+0x10] = [comp+0x60] = hrsSimTune+0x10):
        // released input sets the decay timer +0x30 = tune fDecayDelay (+0x10, the game's 2.0 s) and while it is > 0 the speed is NOT
        // decayed at all. While the stop runs: the timer 0 every frame and fDecayDelay 0 (put back after) - the brake starts at the gesture.
        static std::uintptr_t decayTune{};static float decayOrig{};
        if(on&&horse&&detail::api.base){const auto comp=detail::raw<std::uintptr_t>(horse+0x68);
            if(comp&&detail::raw<std::uintptr_t>(comp)==detail::api.base+horseComponentVtable+0x10){
                const auto stune=detail::raw<std::uintptr_t>(comp+0x60);const float dd=stune?detail::raw<float>(stune+0x10):-1.f;
                if(!decayTune&&stune&&std::isfinite(dd)&&dd>=0.f&&dd<20.f){decayTune=stune;decayOrig=dd;rn::hsDecayDelay.store(dd,std::memory_order_relaxed);
                    const float z=0.f;std::memcpy(reinterpret_cast<void*>(stune+0x10),&z,4);}
                const float timer=detail::raw<float>(comp+0x50+0x30);
                if(std::isfinite(timer)&&timer>0.f&&timer<20.f){const float z=0.f;std::memcpy(reinterpret_cast<void*>(comp+0x50+0x30),&z,4);
                    rn::hsDecayTimerZeroed.fetch_add(1,std::memory_order_relaxed);rn::hsDecayTimerSeen.store(timer,std::memory_order_relaxed);}}}
        if(!on&&decayTune){std::memcpy(reinterpret_cast<void*>(decayTune+0x10),&decayOrig,4);decayTune=0;}}
    { // HARD STOP (VrReins.h hardStopUntil): the game's rdrHorseScriptControl::MakeHardStop on the mount, every main-thread frame
        namespace rn=::rdrvr::vr::reins;const auto until=rn::hardStopUntil.load(std::memory_order_relaxed);
        if(until&&t0<until){const auto horse=::rdrvr::vr::steadylink::mountActor.load(std::memory_order_relaxed);
            const auto player=hookActor.load(std::memory_order_relaxed);
            if((horse||player)&&detail::api.base){using Fn=void(*)(void*);const auto fn=reinterpret_cast<Fn>(detail::api.base+makeHardStopOffset);
                // the rider too - MakeHardStop accepts either guid the player horse sim keeps (+0xad8 / +0xadc)
                if(horse)fn(reinterpret_cast<void*>(horse));if(player)fn(reinterpret_cast<void*>(player));
                rn::hardStopCalls.fetch_add(1,std::memory_order_relaxed);
                const auto got=detail::raw<std::uintptr_t>(detail::api.base+makeHardStopSimGot);const auto sim=got?detail::raw<std::uintptr_t>(got):0;
                rn::hsSim.store(sim,std::memory_order_relaxed);
                if(sim){rn::hsSimAd8.store(detail::raw<std::uint32_t>(sim+0xad8),std::memory_order_relaxed);rn::hsSimAdc.store(detail::raw<std::uint32_t>(sim+0xadc),std::memory_order_relaxed);
                    if(detail::raw<std::uint8_t>(sim+0xb74)&1u)rn::hardStopBitSeen.fetch_add(1,std::memory_order_relaxed);}
                if(horse)rn::hsHorseGuid.store(detail::raw<std::uint32_t>(horse+8),std::memory_order_relaxed);
                if(player)rn::hsPlayerGuid.store(detail::raw<std::uint32_t>(player+8),std::memory_order_relaxed);}
            else rn::hardStopNoHorse.fetch_add(1,std::memory_order_relaxed);}}
    const bool on=::rdrvr::vr::settings().body()&&installed.load(std::memory_order_acquire);
    ::rdrvr::vr::status::bodyHidesHands.store(on,std::memory_order_relaxed);
    Published p{};p.ns=frame.now;unsigned gate=GateOk;
    PlayerRead pl{};SkelRead sk{};
    if(!installed.load(std::memory_order_acquire))gate=GateNotInstalled;
    else if((gate=readPlayer(pl))==GateOk){
        if(!pl.alive)gate=GateDead;
        if(readSkeleton(pl.actor,sk)){
            static std::uint64_t resolvedTick{};
            const bool readFailure=currentRig&&(std::strcmp(currentRig->state,"DATA")==0||std::strcmp(currentRig->state,"PARENTS")==0);
            if(!currentRig||currentRig->data!=sk.data||currentRig->count!=int(sk.count)||(readFailure&&tickIndex-resolvedTick>=120)){
                resolvedTick=tickIndex;
                auto& slot=rigSlots[rigNext.fetch_add(1,std::memory_order_relaxed)%rigSlots.size()];
                bump(stats.rigResolves);if(!resolveRig(sk,slot))bump(stats.rigFailures);
                slot.generation=++rigGeneration;currentRig=&slot;
            }
            p.actor=pl.actor;p.skel=sk.skel;p.data=sk.data;p.count=sk.count;p.rig=currentRig;p.generation=currentRig->generation;
            // no head collapse in cutscenes unless the FIRST PERSON stereo cutscene needs John's head hidden (CINEMATIC / SCREEN /
            // the debug .headshadow 0 skinning-task collapse must never show a headless John)
            p.collapse=on&&config.head&&currentRig->collapseFirst>=0&&!::rdrvr::vr::cutscene::suppressCollapse.load(std::memory_order_relaxed)&&
                !(::rdrvr::vr::settings().rideThirdPerson()&&::rdrvr::vr::steadylink::thirdActive(frame.now)); // John's head shows in THIRD PERSON
        }else if(gate==GateOk)gate=GateNoSkeleton;
        if(gate==GateOk&&currentRig&&!currentRig->armOk[0]&&!currentRig->armOk[1])gate=GateNoRig;
        if(gate==GateOk)gate=moverGate(pl);
        const auto refreshBlocks=[&]{
            if(!blocksValid||tickIndex-blocksTick>=4){
                H::PlayerBlocks b{};bool ok=false;const auto& api=detail::api;
                try{ok=api.context&&api.context(api.contextUser,pl.player,pl.actor,pl.guid,b);}catch(...){ok=false;}
                blocksValid=ok;blocksTick=tickIndex;if(ok)lastBlocks=b;
            }};
        if(gate==GateOk){refreshBlocks();gate=blocksValid?contextGate(lastBlocks):GateContext;}
        // SEAT: a HORSE only (player state 5; a wagon / stagecoach seat is 3 and its mover is attached too) after the mount animation,
        // outside cutscenes / linked animations. moverGate stops at MOUNTED (the mover attachment) before the context gate, so the seat
        // reads the player blocks itself (same cache, every 4th tick).
        if(gate==GateMounted&&on&&::rdrvr::vr::seat::config.on&&currentRig&&currentRig->seat.ok&&pl.state==5u){refreshBlocks();
            const auto& b=lastBlocks;
            if(blocksValid&&b.linkedAnimation)p.seatCut=true;
            p.seat=blocksValid&&b.alive&&b.riding&&!b.mountingHorse&&!b.linkedAnimation&&!b.ridingVehicle&&!b.vehicleTransition&&!b.mountedWeapon;}
    }
    if(gate==GateOk&&::rdrvr::vr::cutscene::inCutscene.load(std::memory_order_relaxed))gate=GateCutscene; // Native pose in cutscenes
    if(gate==GateOk&&!on)gate=GateOff;
    p.gate=gate;p.armsWanted=gate==GateOk&&config.arms;p.mountedArm=gate==GateMounted&&config.arms&&config.mountedArm;
    { // SEAT: the hold cuts at once (no ramp) where any write would fight the engine or show in a cutscene
        const bool cs=::rdrvr::vr::cutscene::inCutscene.load(std::memory_order_relaxed);if(cs)p.seat=false;
        p.seatCut=p.seatCut||cs||gate==GateRagdoll||gate==GateCutscene||gate==GateDead||gate==GateNotInGame||gate==GateNoPlayer||gate==GateNoSkeleton||
            gate==GateNoRig||gate==GateNotInstalled||gate==GateStale;}
    published.store(p);
    hookActor.store(p.actor,std::memory_order_release);
    collapseSkeleton.store(p.collapse?p.skel:0,std::memory_order_release);
    // The body is shown in the eyes only while its head can be collapsed (BODY ON, hooks in, the player's head range known);
    // debug.rdrvr.vr.body.head 0 keeps the head and therefore also keeps the body hidden.
    visible.store(on&&p.collapse,std::memory_order_release);
    // the camera's AUTO eye height ignores the (collapsed) native head while the collapse is armed; VrBody's head height then.
    ::rdrvr::vr::status::bodyCollapsing.store(p.collapse,std::memory_order_relaxed);
    if(!p.collapse)::rdrvr::vr::status::bodyHeadMm.store(-1,std::memory_order_relaxed);
    gateNow.store(gate,std::memory_order_relaxed);bump(stats.gates[gate<GateCount?gate:GateContext]);
    publishStatus(gate);
    const auto ns=detail::now()-t0;bump(stats.tickNsSum,ns);maxOf(stats.tickUsMax,float(double(ns)/1000.0));
}
inline void tickThunk(const mt::Frame& f) noexcept {try{tick(f);}catch(...){bump(stats.exceptions);}}

// ============================== hook A: arms (pure core, testable) ==============================
// The world target of a wrist from a controller grip (free-hand mapping, HandRig.h HandInGrip).
inline A::Mat34 freeHandTarget(const HandPose& h,const A::HandFrame& hf,bool right,float upm,const Config& c) noexcept {
    const A::Vec3 aimInGrip{A::Dot(h.aimFwd,h.frame.a),A::Dot(h.aimFwd,h.frame.b),A::Dot(h.aimFwd,h.frame.c)};
    A::Mat34 local=A::HandInGrip(hf,aimInGrip,right,c.wristBack,c.wristDepth);local.d=local.d*upm;
    const A::Mat34 grip=A::MakeMat34(h.frame,h.gripPos);
    return grip*local;
}
// The body basis (world): up, the character's right from the clavicles, forward = up x right (right-handed world, VrBodyLink).
struct Basis {A::Vec3 right{1,0,0},forward{0,0,1},up{0,1,0};bool valid{};};
inline Basis bodyBasis(const A::Mat34* g,const Rig& r,A::Vec3 up) noexcept {
    Basis b{};b.up=A::Normalize(up,{0,1,0});
    const int cr=r.bones.arm[0].clavicle,cl=r.bones.arm[1].clavicle;
    if(cr<0||cl<0)return b;
    A::Vec3 rt=g[cr].d-g[cl].d;rt=rt-b.up*A::Dot(rt,b.up);
    if(A::Length(rt)<1e-3f)return b;
    b.right=A::Normalize(rt);b.forward=A::Normalize(A::Cross(b.up,b.right));b.valid=true;return b;
}
// Body yaw (whole skeleton about the root's vertical axis): every global rotated, the root bone's local re-derived from the entity
// matrix (G0 = Root * L0, Gi = G[parent] * Li unchanged). acc = the rotation baked into L0 when the engine does not rewrite it.
// Returns the applied rotation (radians).
inline float applyYaw(A::Mat34* g,A::Mat34& l0,const A::Mat34& root,int count,const Rig& r,A::Vec3 up,A::Vec3 targetFwd,float weight,
                      float maxRadians,bool sticky,float& acc) noexcept {
    const Basis b=bodyBasis(g,r,up);
    if(!b.valid){acc=sticky?acc:0.f;return 0.f;}
    const float bodyToTarget=A::SignedAngleAbout(b.forward,targetFwd,b.up);   // current (incl. baked acc) -> target
    const float anim=sticky?acc:0.f;                                          // rotation already in the incoming pose
    float want=A::WrapAngle(bodyToTarget+anim);                               // animation pose -> target
    want=std::clamp(want,-maxRadians,maxRadians)*A::Clamp01(weight);
    const float delta=A::WrapAngle(want-anim);
    acc=want;
    if(!(std::fabs(delta)>1e-6f)||!std::isfinite(delta))return 0.f;
    const A::Mat33 rot=A::FromQuat(A::AxisAngle(b.up,delta));
    const A::Vec3 pivot=root.d;
    for(int i=0;i<count;++i)g[i]=A::RotateAbout(g[i],pivot,rot);
    l0=A::InverseAffine(root)*g[0];
    return delta;
}
// One arm: load [clavicle, end) from the skeleton memory, solve, write back. Returns false (nothing written) on any failure.
struct ArmJob {
    int side{};A::Mat34 target{};float weight{};A::FingerMode fingers{A::FingerMode::Native};float grip{},trigger{};bool write{true};
    const A::Quat* fingerPose{};std::uint16_t fingerPoseMask{}; // per-joint finger locals replacing the curl (ArmPoseInput)
};
// g = this frame's globals (after the body yaw), the source of the solve; globals / locals = the skeleton memory written.
inline bool solveArm(const Rig& r,int side,const A::Mat34* g,float* globals,unsigned char* locals,const ArmJob& job,ArmMemory& mem,const Basis& basis,
                     const Config& c,A::ArmPoseOutput& out,Saved& saved) noexcept {
    const auto& b=r.bones.arm[std::size_t(side)];const auto& rig=r.arm[std::size_t(side)];
    const int base=b.clavicle,n=rig.count;
    if(n<4||n>A::kMaxArm)return false;
    std::array<A::Mat34,A::kMaxArm> obj{},loc{};std::array<std::uint8_t,A::kMaxArm> modified{};
    for(int k=0;k<n;++k){obj[std::size_t(k)]=g[std::size_t(base+k)];
        loc[std::size_t(k)]=A::LoadMat34(reinterpret_cast<const float*>(locals+std::size_t(base+k)*off::localStride));}
    const int pc=r.parent[std::size_t(base)];if(pc<0)return false;
    A::ArmPoseInput in{};in.clavicleParent=g[std::size_t(pc)];in.target=job.target;in.weight=job.weight;
    auto& p=in.params;p.side=side==0?1.f:-1.f;p.elbowOut=c.elbowOut;p.elbowDown=c.elbowDown;p.elbowBack=c.elbowBack;p.elbowWrist=c.elbowWrist;
    p.rollShare=c.rollShare;p.stretchMax=c.stretchCm*.01f;p.right=basis.right;p.forward=basis.forward;p.up=basis.up;
    in.lastPerp=mem.lastPerp;in.hingeLocal=mem.hingeLocal;in.clavicleReach=c.clavicle;in.clavicleMaxRadians=c.clavicleDeg*A::kDegToRad;
    in.fingers=c.fingers?job.fingers:A::FingerMode::Native;in.grip=job.grip;in.trigger=job.trigger;
    in.fingerPose=job.fingerPose;in.fingerPoseMask=job.fingerPoseMask;
    if(!A::PoseArm(rig,in,obj.data(),loc.data(),modified.data(),out))return false;
    mem.lastPerp=out.ik.perp;mem.hingeLocal=out.ik.hingeLocal;
    if(job.write){
        for(int k=0;k<n;++k){A::StoreMat34(globals+std::size_t(base+k)*16,obj[std::size_t(k)]);
            if(modified[std::size_t(k)])A::StoreMat34(reinterpret_cast<float*>(locals+std::size_t(base+k)*off::localStride),loc[std::size_t(k)]);}
    }
    saved.valid[side]=job.write;saved.first[side]=base;saved.count[side]=n;
    for(int k=0;k<n;++k)saved.local[std::size_t(side)][std::size_t(k)]=loc[std::size_t(k)];
    saved.wrist[std::size_t(side)]=obj[std::size_t(rig.wristRel)];
    return true;
}
// Wrist target derived from the weapon attachment A: the wrist -> attachment-bone relation of this frame's skeleton kept, so
// the engine's own attachment (G_attach) of the IK'd wrist lands exactly on A.
inline A::Mat34 wristFromAttachment(const A::Mat34& attachment,const A::Mat34& wrist,const A::Mat34& attach) noexcept {
    const A::Mat34 rel=A::InverseAffine(wrist)*attach;   // attachment bone in the wrist frame
    return attachment*A::InverseAffine(rel);
}
// Support hand on a long gun: the hand under the fore-end in the gun's frame (VrArmIk SupportWrist: index side along the barrel,
// palm up towards the bore, rolled towards the gun's middle; the controller's own rotation is not used - a free-hand wrist moved
// onto the barrel put the fist across it). Where along the barrel: the support controller's grip projected onto the barrel line,
// kept between supportMin..supportMax of grip -> muzzle; the gun's up = the holding controller's grip up normal to the barrel
// (the BARREL hold takes its roll from that grip).
inline A::Mat34 supportTarget(const HandPose& h,const A::HandFrame& hf,bool right,const Frame& f,const Config& c) noexcept {
    const A::Vec3 dir=A::Normalize(f.barrelDir,{0,0,1});
    // slide along the BORE (authored muzzle + barrel dir),
    // not the attachment-bone line through the holding palm - that line runs the gun's grip->bore height (sniper 10 cm, rifles ~8 cm)
    // under the bore, so the support hand would hang that far under the fore-end
    const float len=std::max(A::Dot(f.muzzle-f.attachment.d,dir),0.f); // holding grip -> muzzle along the bore
    const A::Vec3 grip=f.muzzle-dir*len;                                 // the bore point level with the holding grip
    float along=std::clamp(A::Dot(h.gripPos-grip,dir),len*c.supportMin,len*c.supportMax);
    // the place along the barrel is latched when the support grip
    // starts (as a fraction of grip -> muzzle) and kept while it holds; a new grip / another gun / a gap > 250 ms latches again
    {struct Lock {std::uintptr_t weapon{};std::uint64_t ns{};float frac{};};static Lock lock[2]{};auto& k=lock[right?1:0];
     const std::uint64_t now=f.headSampledNs;
     if(c.supportLock&&len>1e-4f&&now){
        if(k.weapon!=f.weapon||!k.ns||now<k.ns||now-k.ns>250'000'000ull)k.frac=along/len;
        along=k.frac*len;k.weapon=f.weapon;k.ns=now;}}
    // the palm faces WORLD up under the barrel (a
    // cradling hand), not the gun's own up - a canted gun does not twist the support wrist with it
    A::Vec3 up=f.up;
    if(!c.supportWorldUp&&f.hold>=0&&f.hold<2&&f.hand[std::size_t(f.hold)].valid)up=f.hand[std::size_t(f.hold)].frame.b;
    if(std::fabs(A::Dot(A::Normalize(up,f.up),dir))>.95f)up=f.up;
    up=A::Normalize(up-dir*A::Dot(up,dir),f.up);
    const float s=std::max(f.upm,1e-3f);
    const A::Vec3 fore=grip+dir*along-up*(c.supportDropM*s);
    return A::SupportWrist(hf,dir,up,fore,right,c.supportTiltDeg*A::kDegToRad,c.supportYawDeg*A::kDegToRad,c.wristBack*s,c.wristDepth*s);
}
inline float stepWeight(float w,bool wanted,float dt,float seconds) noexcept {
    const float step=seconds>0.f?std::max(dt,0.f)/seconds:1.f;return A::Clamp01(wanted?w+step:w-step);}
// RIDING: the rider frame of the mounted arm memory = the skeleton root's translation + heading about `up` only (the rider matrix
// pitches / rolls with the gait; a held hand must not bob with it - GameXr's riding eye refuses that matrix for the same reason). The
// root axis closest to `up` is its up, the next one (cyclic) its heading: stable while the rider tilts < 45 deg.
inline A::Mat34 yawFrame(const A::Mat34& m,A::Vec3 up) noexcept {
    const A::Vec3 ax[3]{m.a,m.b,m.c};int k=0;float best=-1.f;
    for(int i=0;i<3;++i){const float d=std::fabs(A::Dot(A::Normalize(ax[i],up),up));if(d>best){best=d;k=i;}}
    A::Vec3 h=ax[(k+1)%3];h=h-up*A::Dot(h,up);
    if(A::Length(h)<1e-4f){h=ax[(k+2)%3];h=h-up*A::Dot(h,up);}
    h=A::Normalize(h,{0,0,1});
    return A::MakeMat34(A::Mat33{A::Cross(up,h),up,h},A::Finite(m.d)?m.d:A::Vec3{});
}
// crossfade of two wrist targets (position lerp, rotation slerp, column lengths lerped); a mirrored / broken input -> b
inline A::Mat34 blendTarget(const A::Mat34& a,const A::Mat34& b,float t) noexcept {
    const auto det=[](const A::Mat34& m){return A::Dot(m.a,A::Cross(m.b,m.c));};
    if(!(det(a)>1e-9f)||!(det(b)>1e-9f))return b;
    const A::Mat33 r=A::FromQuat(A::Slerp(A::ToQuat(A::Orthonormalize(a.Rot())),A::ToQuat(A::Orthonormalize(b.Rot())),t));
    const auto len=[t](A::Vec3 x,A::Vec3 y){return A::Length(x)+(A::Length(y)-A::Length(x))*t;};
    const A::Mat34 out=A::MakeMat34(A::Mat33{r.a*len(a.a,b.a),r.b*len(a.b,b.b),r.c*len(a.c,b.c)},A::Lerp(a.d,b.d,t));
    return A::Finite(out)?out:b;
}
// where the character's head joint belongs, horizontally. The HMD is modelled as a rigid head that
// pitches / yaws about a NECK pivot P = eye - fwd * neckBack - headUp * neckDown (the standard VR neck model, 8 / 7.5 cm). A pivot at
// eye - fwd * BACK - up * BODY LOWER would move ~5-10 cm forward when looking down 50 deg, and the
// whole body with it. The head joint goes BACK behind the eye along the head's heading:
// target = P + heading * (neckBack - BACK) (upright: exactly eye - heading * BACK).
// heading = horizontal(fwd), the smoothed forward within 6 deg of straight up / down. horizontal(fwd + headUp) with headUp
// made from world up = (cos - sin) * forward would flip 180 deg looking UP past 45 deg (a 2 * (BACK - neckBack) target jump; the seat's twist
// reads it too); identical direction for every other pitch.
inline bool neckTarget(const Frame& f,A::Vec3 smoothFwd,const Config& c,A::Vec3& out) noexcept {
    const A::Vec3 up=A::Normalize(f.up,{0,1,0});const A::Vec3 fw=A::Normalize(f.headFwd,{0,0,1});
    A::Vec3 hu=up-fw*A::Dot(fw,up);
    if(A::Length(hu)<.1f){hu=smoothFwd-up*A::Dot(smoothFwd,up);if(A::Length(hu)<1e-3f)hu=up;}
    hu=A::Normalize(hu,up);
    A::Vec3 heading=fw-up*A::Dot(fw,up);
    if(A::Length(heading)<.1f){heading=smoothFwd-up*A::Dot(smoothFwd,up);}
    heading=A::Normalize(heading,{0,0,1});
    const float s=std::max(f.upm,1e-3f);
    const A::Vec3 pivot=f.eye-fw*(c.neckBack*s)-hu*(c.neckDown*s);
    out=pivot+heading*((c.neckBack-f.eyeBack)*s);
    return A::Finite(out);
}
// the lock's travel limit with a soft knee (linear to knee*max, then a tanh saturation to max): no slope break, no clamp jumps.
inline float softLimit(float len,float maxLen,float knee) noexcept {
    if(!(maxLen>0.f))return 0.f;const float k=std::clamp(knee,0.f,.99f)*maxLen;
    if(len<=k)return len;const float r=maxLen-k;return k+r*std::tanh((len-k)/r);
}
// a hand at a BELT holster (knife / revolver / lasso: the ones the player looks down for): inside its zone (the input brain's
// enter / leave radius), or half-gripping within nearSocketM of one (last frame's sockets on this body).
inline constexpr std::uint32_t beltSlots=0b0001011u; // MELEE, SIDEARM, LASSO
inline bool handAtBelt(const Frame& f,bool frameOk,const Config& c) noexcept {
    // the grab zones are large (VrHolsters bodyGrabRadius 0.32): inside one only freezes while the hand is also within beltCoreM of
    // the socket on this body (last frame's, world); without a world socket the published zone alone.
    const bool world=frameOk&&f.handsInput;const float core=c.beltCoreM*std::max(f.upm,1e-3f);
    for(unsigned h=0;h<2;++h){const int in=H::insidePublished[h].load(std::memory_order_relaxed);if(!(in>=0&&in<7&&(beltSlots>>unsigned(in)&1u)))continue;
        if(world&&f.hand[h].valid&&(ik.socketWorldMask>>unsigned(in)&1u)){if(A::Length(f.hand[h].gripPos-ik.socketWorld[std::size_t(in)])<=core)return true;}
        else return true;}
    if(!world)return false;
    const float r=c.nearSocketM*std::max(f.upm,1e-3f);
    for(unsigned h=0;h<2;++h){const auto& hp=f.hand[h];if(!hp.valid||hp.squeeze<c.halfGripLo||hp.squeeze>c.halfGripHi)continue;
        for(unsigned k=0;k<7;++k)if((ik.socketWorldMask&beltSlots)>>k&1u&&A::Length(hp.gripPos-ik.socketWorld[k])<=r)return true;}
    return false;
}
// holster sockets (world) from the posed skeleton: a bone + an offset in the body basis (right, up, forward; metres). The
// one-handed slots prefer where their holstered item really hangs (VrHolsters holsteredBox: weapon+0x80 of the previous frame, kept
// as an offset in the previous frame's pelvis frame, so it also marks the empty holster while the item is drawn).
struct SocketSpec {int anchor;float right,up,forward;}; // anchor 0 pelvis, 1 hip_r, 2 hip_l, 3 chest, 4 neck, 5 clavicle_r, 6 clavicle_l
inline constexpr std::array<SocketSpec,7> socketSpecs{{
    {0,-.12f, .02f, .12f},  // MELEE: left front of the belt (knife, cross draw)
    {1, .05f,-.08f, .02f},  // SIDEARM: right thigh below the hip joint (revolver holster)
    {3, .00f,-.05f, .16f},  // THROWN: chest front
    {2,-.05f,-.08f, .02f},  // LASSO: left thigh
    {4, .00f, .12f,-.16f},  // SNIPER: behind the head, high
    {5, .02f, .08f,-.16f},  // LONGARM: behind the right shoulder (reach over the shoulder)
    {6,-.02f, .08f,-.16f},  // SHOTGUN: behind the left shoulder
}};
// THROWN uses the chest spec - the
// game does not draw a holstered thrown item, its 'holstered' point is not at the chest
inline constexpr std::uint32_t socketItemSlots=0b0001011u; // MELEE / SIDEARM / LASSO: the holstered item's own position
inline void publishSockets(const Rig& r,const A::Mat34* g,int count,const Frame& f,bool open,std::uint64_t now,float dt,std::uintptr_t skel) noexcept {
    if(ik.socketSkel!=skel){ik.socketSkel=skel;ik.itemValid={};ik.havePrevPelvis=false;}
    const auto bone=[&](int i){return i>=0&&i<count;};
    const int pelvis=bone(r.bones.pelvis)?r.bones.pelvis:0;
    if(!open||count<=0){ik.havePrevPelvis=false;ik.socketWorldMask=0;return;}
    if(!f.mapValid){ik.havePrevPelvis=false;ik.socketWorldMask=0;return;}
    ik.socketWorldMask=0;
    const Basis b=bodyBasis(g,r,f.up);if(!b.valid){ik.havePrevPelvis=false;return;}
    const A::Mat34& pel=g[std::size_t(pelvis)];const float s=std::max(f.upm,1e-3f);
    // holstered items -> pelvis-frame offsets (the items of the previous frame hung on the previous frame's skeleton)
    H::HolsteredWorld hw{};
    if(ik.havePrevPelvis&&H::holsteredBox.load(hw)&&hw.mask&&(hw.ns>=now?hw.ns-now:now-hw.ns)<250'000'000ull){
        const A::Mat34 inv=A::InverseAffine(ik.prevPelvis);bump(stats.socketItemFrames);
        for(int k=0;k<7;++k){if(!(hw.mask>>unsigned(k)&1u)||!(socketItemSlots>>unsigned(k)&1u))continue;
            const auto& p=hw.position[std::size_t(k)];const A::Vec3 o=A::TransformPoint(inv,{p[0],p[1],p[2]});
            if(!A::Finite(o)||A::Length(o)>.8f*s)continue; // not on this body (stale, teleport)
            auto& io=ik.itemOffset[std::size_t(k)];
            if(!ik.itemValid[std::size_t(k)]){io=o;ik.itemValid[std::size_t(k)]=true;}else io=io+(o-io)*A::SmoothFactor(dt,.25f);}
    }
    ik.prevPelvis=pel;ik.havePrevPelvis=true;
    const int anchors[7]{pelvis,bone(r.bones.hip[0])?r.bones.hip[0]:pelvis,bone(r.bones.hip[1])?r.bones.hip[1]:pelvis,
        bone(r.bones.chest)?r.bones.chest:pelvis,bone(r.bones.neck)?r.bones.neck:pelvis,
        bone(r.bones.arm[0].clavicle)?r.bones.arm[0].clavicle:pelvis,bone(r.bones.arm[1].clavicle)?r.bones.arm[1].clavicle:pelvis};
    const A::Mat34 toLocal=A::InverseAffine(f.localToWorld);
    H::BodySockets out{};out.ns=now;out.referenceSpace=f.space;
    for(int k=0;k<7;++k){
        A::Vec3 w{};
        if(ik.itemValid[std::size_t(k)]){w=A::TransformPoint(pel,ik.itemOffset[std::size_t(k)]);out.fromItem|=1u<<unsigned(k);}
        else{const auto& sp=socketSpecs[std::size_t(k)];w=g[std::size_t(anchors[sp.anchor])].d+(b.right*sp.right+b.up*sp.up+b.forward*sp.forward)*s;}
        if(A::Finite(w)){ik.socketWorld[std::size_t(k)]=w;ik.socketWorldMask|=1u<<unsigned(k);} // The half-grip freeze
        const A::Vec3 l=A::TransformPoint(toLocal,w);
        if(!A::Finite(l))continue;
        out.position[std::size_t(k)]={l.x,l.y,l.z};out.mask|=1u<<unsigned(k);
    }
    if(!out.mask)return;
    H::bodySocketBox.store(out);bump(stats.socketPublishes);
    stats.socketMask.store(out.mask,std::memory_order_relaxed);stats.socketFromItem.store(out.fromItem,std::memory_order_relaxed);
}

// SEAT (VrSeat.h): John's upper body seated upright under the VR head on a horse, BEFORE the arm IK (the arms solve from the new
// shoulders). mode 2 = hold, 1 = ramp out (dismount / gate change), 0 = cut (the native pose at once, nothing written). g = this frame's
// globals (updated in place for the arm IK / sockets); the chain locals + the spine00 subtree globals are written. True = written.
inline bool seatStep(const Rig& r,A::Mat34* g,float* globals,unsigned char* locals,const A::Mat34& root,int count,const Frame& f,bool frameOk,
                     float dt,std::uint64_t now,int mode,const Config& c) noexcept {
    namespace S=::rdrvr::vr::seat;auto& st=S::state;auto& ss=S::stats;const auto& sc=S::config;const auto& ch=r.seat;
    const auto t0=detail::now();
    if(st.data!=r.data||st.generation!=r.generation){st=S::State{};st.data=r.data;st.generation=r.generation;} // a new skeleton: from scratch
    const auto idle=[&](bool cut){if(cut&&st.weight>0.f)bump(ss.cuts);if(cut){st.weight=0.f;st.haveFollow=false;st.haveAim=false;}
        ss.active.store(false,std::memory_order_relaxed);ss.wanted.store(false,std::memory_order_relaxed);ss.weight.store(st.weight,std::memory_order_relaxed);};
    if(!sc.on&&mode>1)mode=1;
    if(mode!=2){if(st.lastWantNs&&!st.offNs)st.offNs=now?now:1;}else if(st.offNs&&now>=st.offNs&&now-st.offNs<=1'000'000'000ull)st.offNs=0; // < 1 s off: the same ride
    if(mode==0||!ch.ok){idle(true);return false;}
    if(mode==1&&st.weight<=0.f){idle(false);return false;}
    for(int k=0;k<ch.n;++k)if(ch.bone[std::size_t(k)]>=count){idle(true);ss.skipWhy.store("BONE_RANGE",std::memory_order_relaxed);bump(ss.skipped);return false;}
    if(ch.head>=count||ch.baseParent<0||ch.baseParent>=count){idle(true);ss.skipWhy.store("BONE_RANGE",std::memory_order_relaxed);bump(ss.skipped);return false;}
    // a new mount (the hold wanted again after it was seen off for > 1 s - a dismount, not a pause / menu gap where hook A does not run):
    // the capture, the eye latch, the held upright frame and the follow start over
    if(mode==2){if(!st.lastWantNs||st.offNs){bump(ss.mounts);st.captured=false;st.bindFallback=false;st.maxHead=-1e9f;st.calmS=0.f;st.waitS=0.f;
            st.eyeLatched=false;st.eyeSum=0.f;st.eyeS=0.f;S::eyeFwdOk.store(false,std::memory_order_relaxed);st.haveF=false;st.haveAim=false;st.followK=1.f;
            st.twistSide=0.f;}
        st.lastWantNs=now;st.offNs=0;}
    const A::Vec3 up=A::Normalize(f.up,{0,1,0});const float s=std::max(f.upm,1e-3f),cmPer=100.f/s;
    // the upright frame (the root's b onto up); past 70 deg of tilt (a rear / a buck) the last one of this mount is held
    float upDot=1.f;A::Mat33 F=S::UprightFrame(root,up,upDot);
    {const float tilt=std::acos(std::clamp(upDot,-1.f,1.f))/A::kDegToRad;S::put(ss.tiltDeg,tilt);if(std::isfinite(tilt))S::maxOf(ss.tiltDegMax,tilt);}
    if(!(upDot>S::tiltHoldDot)){bump(ss.tiltFrames);
        if(!st.haveF){idle(true);ss.skipWhy.store("TILT",std::memory_order_relaxed);bump(ss.skipped);return false;}F=A::FromQuat(st.lastF);}
    else{st.lastF=A::ToQuat(F);st.haveF=true;}
    const A::Mat33 Ft=A::Transpose(F);const A::Vec3 fwd=S::RiderForward(F,up),side=A::Cross(up,fwd);
    const int b0=ch.bone[0],top=ch.bone[std::size_t(ch.n-1)];
    const auto rel=[&](A::Vec3 p,std::atomic<float>& fo,std::atomic<float>* so,std::atomic<float>* uo){const A::Vec3 d=(p-root.d)*cmPer;
        S::put(fo,A::Dot(d,fwd));if(so)S::put(*so,A::Dot(d,side));if(uo)S::put(*uo,A::Dot(d,up));};
    rel(g[std::size_t(ch.head)].d,ss.nativeFwd,&ss.nativeSide,&ss.nativeUp);rel(g[std::size_t(b0)].d,ss.baseFwd,&ss.baseSide,nullptr);
    // reference: the vertical bind, or the capture (.seat.ref 1, or a bind > 26 deg off the root up) - no capture within .seat.wait -> the bind
    const bool wantCapture=sc.capture||!ch.bindUpright;
    { // the calm detector: rider speed (root, horizontal) < 0.3 m/s, the native head within 3 cm of the highest CALM head this mount, centred, 0.5 s
        if(st.haveRoot&&dt>1e-4f){A::Vec3 v=(root.d-st.prevRoot)*(1.f/dt);v=v-up*A::Dot(v,up);const float sp=A::Length(v)/s;
            if(std::isfinite(sp))st.speedS+=(std::min(sp,20.f)-st.speedS)*A::SmoothFactor(dt,.3f);}
        st.prevRoot=root.d;st.haveRoot=true;
        if(wantCapture&&!st.bindFallback&&!st.captured&&mode==2&&st.weight<=0.f){ // only the genuine native pose (nothing of ours in it yet)
            bump(ss.captureWait);st.waitS+=dt;
            const A::Vec3 hd=(g[std::size_t(ch.head)].d-root.d)*(1.f/s);const float h=A::Dot(hd,up),hf=A::Dot(hd,fwd),hs=A::Dot(hd,side);
            const bool calm=st.speedS<.3f;if(calm)st.maxHead=std::max(st.maxHead,h); // the moving upright seat is higher than every standing idle
            // centred (standstill idles reach 36 cm sideways / 52 cm forward, the look-back is the highest steady head):
            // the head over the saddle's centre line (|side| < 6 cm, forward -5..+20 cm of the root), the shoulders square to the rider (< 15 deg)
            bool square=true;
            {const int ar=r.bones.arm[0].arm,al=r.bones.arm[1].arm;
             if(ar>=0&&al>=0&&ar<count&&al<count){A::Vec3 sl=g[std::size_t(ar)].d-g[std::size_t(al)].d;sl=sl-up*A::Dot(sl,up);const float L=A::Length(sl);
                 square=L>1e-3f*s&&std::fabs(A::Dot(sl,fwd))<.26f*L;}}
            const bool high=calm&&h>st.maxHead-.03f,centred=std::fabs(hs)<.06f&&hf>-.05f&&hf<.20f&&square;
            if(high&&!centred)bump(ss.captureRejects);
            st.calmS=high&&centred?st.calmS+dt:0.f;
            if(st.calmS>=.5f){ // its spine00 -> head line made vertical as well (an UPRIGHT seat), kept in the upright frame
                const A::Vec3 sp=A::Normalize(Ft*(g[std::size_t(ch.head)].d-g[std::size_t(b0)].d),{0,1,0});
                st.capBase=A::Normalize(A::FromTo(sp,{0,1,0})*A::ToQuat(Ft*A::Orthonormalize(g[std::size_t(b0)].Rot())));
                for(int k=1;k<ch.n;++k)st.capLocal[std::size_t(k)]=A::ToQuat(A::Orthonormalize(A::LoadMat34(reinterpret_cast<const float*>(locals+std::size_t(ch.bone[std::size_t(k)])*off::localStride)).Rot()));
                st.captured=true;bump(ss.captures);}
            else if(sc.waitS>0.f&&st.waitS>=sc.waitS){st.bindFallback=true;bump(ss.captureTimeouts);}}
    }
    const bool useCapture=wantCapture&&!st.bindFallback,refReady=!useCapture||st.captured;
    ss.refSource.store(!wantCapture?"BIND":!useCapture?"BIND_AFTER_WAIT":st.captured?(sc.capture?"CAPTURE":"CAPTURE_BIND_NOT_UPRIGHT"):"WAIT_CAPTURE",
                       std::memory_order_relaxed);
    ss.refOk.store(refReady,std::memory_order_relaxed);
    const bool wanted=mode==2&&refReady;ss.wanted.store(wanted,std::memory_order_relaxed);if(wanted)bump(ss.wantedFrames);
    st.weight=stepWeight(st.weight,wanted,dt,sc.rampS);ss.weight.store(st.weight,std::memory_order_relaxed);
    if(st.weight<=0.f){idle(false);return false;}
    if(!wanted)bump(ss.rampOutFrames);
    // this frame's reference, else (a ramp out whose capture was reset by a new mount) the last one applied - never an unset one
    A::Quat refBase{};
    if(refReady){refBase=useCapture?st.capBase:ch.bindBase;st.useLocal=useCapture?st.capLocal:ch.bindLocal;st.useBase=refBase;st.haveUse=true;}
    else if(st.haveUse){refBase=st.useBase;bump(ss.lastRefFrames);}
    else{idle(true);ss.skipWhy.store("NO_REF",std::memory_order_relaxed);bump(ss.skipped);return false;}
    const A::Quat* ref=st.useLocal.data();
    // native inputs: bone[0]'s parent global (native: the root bone / pelvis stay), the chain locals, the head joint in the neck frame
    S::Pose pose{};pose.parent=g[std::size_t(ch.baseParent)];
    for(int k=0;k<ch.n;++k)pose.local[std::size_t(k)]=A::LoadMat34(reinterpret_cast<const float*>(locals+std::size_t(ch.bone[std::size_t(k)])*off::localStride));
    pose.headLocal=A::TransformPoint(A::InverseAffine(g[std::size_t(top)]),g[std::size_t(ch.head)].d);
    const A::Mat33 base=F*A::FromQuat(refBase);
    S::Out o{};S::Fk(ch,pose,base,ref,up,0.f,{},1.f,o);const A::Vec3 refHead=o.head; // the rigid upright reference
    rel(refHead,ss.refFwd,nullptr,&ss.refUp);
    // ---- follow the VR head: the twist toward the HMD heading, the lean that puts the head joint on the neck-model target ----
    const bool headNow=frameOk&&f.headValid;A::Vec3 target{};bool haveTarget=false;A::Vec3 heading=fwd;
    if(headNow){
        heading=S::HeadHeading(f.headFwd,up,fwd);
        // VrBody's neck-model target of the head joint (BODY BACK behind the eye), less HORSE CAM FWD (that setting moves the camera, not John)
        if(neckTarget(f,ik.smoothFwd,c,target)){target=target-fwd*(::rdrvr::vr::settings().rideFwdMeters()*s);haveTarget=A::Finite(target);}
    }
    bool tc=false,lc=false;
    if(haveTarget){
        // .seat.tau smooths the TARGET in the rider frame (F about the root: it turns / moves with the horse, also through a hold); the lean
        // below is solved every frame against the native spine base, so the gait's hip motion is cancelled without lag
        const A::Vec3 tr=Ft*(target-root.d);const float k=st.haveAim?A::SmoothFactor(dt,sc.tauMs*.001f):1.f;
        st.aimR=st.haveAim?st.aimR+(tr-st.aimR)*k:tr;st.followK=st.haveAim?st.followK+(1.f-st.followK)*A::SmoothFactor(dt,.3f):1.f;st.haveAim=true;
        const float twistWant=S::TwistFor(A::SignedAngleAbout(fwd,heading,up),sc.twistDeg*A::kDegToRad,st.twistSide,tc)*sc.follow;
        st.twist+=(twistWant-st.twist)*(st.haveFollow?k:1.f);st.haveFollow=true;st.lastHeadNs=now;
    }else{
        bump(ss.noHead);
        // no head this frame: the follow (rider-frame target, twist) held holdS, then it relaxes to the upright reference (tau 0.3 s)
        if(st.haveAim&&st.lastHeadNs&&now>=st.lastHeadNs&&now-st.lastHeadNs<std::uint64_t(double(c.holdS)*1e9))bump(ss.holdFrames);
        else{const float k=A::SmoothFactor(dt,.3f);st.twist-=st.twist*k;st.followK-=st.followK*k;}
    }
    if(!std::isfinite(st.twist)||!A::Finite(st.aimR)||!std::isfinite(st.followK)){st.twist=0.f;st.aimR={};st.followK=1.f;st.haveFollow=st.haveAim=false;}
    A::Vec3 lean{};
    if(st.haveAim){const A::Vec3 aim=refHead+(root.d+F*st.aimR-refHead)*(sc.follow*std::clamp(st.followK,0.f,1.f));
        lean=S::SolveLean(ch,pose,base,ref,up,st.twist,aim,sc.leanDeg*A::kDegToRad,lc);}
    if(tc)bump(ss.twistClamped);if(lc)bump(ss.leanClamped);if(tc||lc)bump(ss.clampFrames);
    st.lean=lean;
    S::Fk(ch,pose,base,ref,up,st.twist,lean,st.weight,o);
    bool finite=A::Finite(o.head);for(int k=0;k<ch.n;++k)finite=finite&&A::Finite(o.global[std::size_t(k)])&&A::Finite(o.local[std::size_t(k)]);
    if(!finite){idle(true);ss.skipWhy.store("NOT_FINITE",std::memory_order_relaxed);bump(ss.skipped);return false;}
    // ---- the riding eye's forward seat: the reference head forward of the root LATCHED once per mount (the mean of the first 1 s at full
    // weight; a running mean creeps with the native hip motion) + BODY BACK (metres) ----
    if(wanted&&st.weight>=1.f&&!st.eyeLatched&&dt>0.f){const float lf=A::Dot(refHead-root.d,fwd)/s;
        if(std::isfinite(lf)){st.eyeSum+=lf*dt;st.eyeS+=dt;if(st.eyeS>=1.f){st.eyeRef=st.eyeSum/st.eyeS;st.eyeLatched=true;bump(ss.eyeLatches);}}}
    if(st.eyeLatched&&std::isfinite(st.eyeRef+f.eyeBack)){S::eyeFwdM.store(st.eyeRef+f.eyeBack,std::memory_order_relaxed);S::eyeFwdOk.store(true,std::memory_order_release);}
    // ---- write: the chain (locals + globals), the rest of the spine00 subtree carried rigidly with its nearest chain bone ----
    const bool write=!c.dry;
    if(write){
        std::array<A::Mat34,S::maxChain> delta{};
        for(int k=0;k<ch.n;++k){const int b=ch.bone[std::size_t(k)];delta[std::size_t(k)]=o.global[std::size_t(k)]*A::InverseAffine(g[std::size_t(b)]);
            g[std::size_t(b)]=o.global[std::size_t(k)];A::StoreMat34(globals+std::size_t(b)*16,o.global[std::size_t(k)]);
            A::StoreMat34(reinterpret_cast<float*>(locals+std::size_t(b)*off::localStride),o.local[std::size_t(k)]);}
        for(int i=0;i<ch.subCount;++i){const int b=ch.sub[std::size_t(i)];if(b<0||b>=count)continue;
            g[std::size_t(b)]=delta[std::size_t(ch.anchor[std::size_t(i)])]*g[std::size_t(b)];A::StoreMat34(globals+std::size_t(b)*16,g[std::size_t(b)]);}
        auto& sv=ik.saved.seat;sv.valid=true;sv.local=o.local;
        S::activeNs.store(now?now:1,std::memory_order_release);
    }else bump(ss.dryFrames);
    // ---- report ----
    bump(ss.frames);ss.active.store(true,std::memory_order_relaxed);
    S::put(ss.leanDeg,A::Length(lean)/A::kDegToRad);S::put(ss.twistDeg,st.twist/A::kDegToRad);
    rel(o.head,ss.headFwd,&ss.headSide,&ss.headUp);
    if(haveTarget){A::Vec3 e=o.head-target;e=e-up*A::Dot(e,up);const float cm=A::Length(e)*cmPer;S::put(ss.neckErrCm,cm);
        if(st.weight>=1.f&&std::isfinite(cm))S::maxOf(ss.neckErrCmMax,cm);}
    if(headNow){const A::Vec3 d=f.eye-o.global[std::size_t(ch.n-1)].d;S::put(ss.stumpBelow,A::Dot(d,up)*cmPer);S::put(ss.stumpBehind,A::Dot(d,heading)*cmPer);}
    S::maxOf(ss.usMax,float(double(detail::now()-t0)/1000.0));
    return write;
}
// SEAT hook B: the engine IK (aim / rein 'spine+' chains) may have bent the held chain after hook A -> drift counted and (default
// .seat.reassert 1) the chain re-asserted, the rest of the spine00 subtree carried rigidly from where the engine left it (the arm re-assert
// below then starts from the restored shoulders).
inline void seatAfterEngineIk(const Rig& r,float* globals,unsigned char* locals,const Config& c) noexcept {
    namespace S=::rdrvr::vr::seat;const auto& sv=ik.saved.seat;const auto& ch=r.seat;auto& ss=S::stats;
    if(!sv.valid||!ch.ok||ch.baseParent<0)return;
    // the wanted chain from its saved locals under the CURRENT base parent (the root bone / pelvis are the engine's: never detached)
    std::array<A::Mat34,S::maxChain> now{},want{};float cm=0.f,deg=0.f;A::Mat34 gp=A::LoadMat34(globals+std::size_t(ch.baseParent)*16);
    for(int k=0;k<ch.n;++k){now[std::size_t(k)]=A::LoadMat34(globals+std::size_t(ch.bone[std::size_t(k)])*16);want[std::size_t(k)]=gp*sv.local[std::size_t(k)];gp=want[std::size_t(k)];
        cm=std::max(cm,A::Length(now[std::size_t(k)].d-want[std::size_t(k)].d)*100.f);deg=std::max(deg,S::AngleDeg(now[std::size_t(k)].Rot(),want[std::size_t(k)].Rot()));}
    if(!(cm>.05f||deg>.2f))return;
    bump(ss.driftFrames);if(cm>1.f)bump(ss.driftOver1cm);S::maxOf(ss.driftCmMax,cm);S::maxOf(ss.driftDegMax,deg);
    if(!S::config.reassert||c.dry)return;
    std::array<A::Mat34,S::maxChain> delta{};
    for(int k=0;k<ch.n;++k){const int b=ch.bone[std::size_t(k)];delta[std::size_t(k)]=want[std::size_t(k)]*A::InverseAffine(now[std::size_t(k)]);
        A::StoreMat34(globals+std::size_t(b)*16,want[std::size_t(k)]);A::StoreMat34(reinterpret_cast<float*>(locals+std::size_t(b)*off::localStride),sv.local[std::size_t(k)]);}
    for(int i=0;i<ch.subCount;++i){const int b=ch.sub[std::size_t(i)];float* m=globals+std::size_t(b)*16;
        A::StoreMat34(m,delta[std::size_t(ch.anchor[std::size_t(i)])]*A::LoadMat34(m));}
    bump(ss.reasserted);
}

// The whole hook-A body for the player (no engine calls). frameOk = the provider gave this frame's poses.
struct ApplyResult {bool wrote{};int arms{};std::uintptr_t nativeWeapon{},leftWeapon{};A::Mat34 leftAttachment{};};
// ============================== RIDE STEADY ==============================
// The riding eye stays level while the body and the horse shake, which makes players motion sick.
// The riding eye is Holt-smoothed (GameXr ridingAnchorSample, 350 ms) while John's root (on the saddle) and the horse's back bounce
// ~18 cm p-p at a gallop. Each hook A call shifts the WHOLE skeleton of John (applyBody, mounted, before the seat hold) and of the horse
// (attachHook, mountActor from VrMotionActors: the race-1 actor under the eye) along up by (smoothed - raw) of its own reference height:
// John = the root (entity) height, the horse = the bone nearest under John's root, > 0.8 m above the horse root. Globals + L0 as the
// head lock (sticky L0 = the shift persisted: the measured bone height minus the baked shift is the raw one). Attachments (hat,
// holstered guns, the rein rope) follow in the engine's own update after hook A; skinning / shadows read these globals.
// debug.rdrvr.vr.riding.steady 0 = off (unwinds), .steadyms (Holt tau, ms, 350), .steadycm (max shift, cm, 15).
namespace steady {
inline std::atomic<std::uintptr_t>& mountActor=::rdrvr::vr::steadylink::mountActor; // VrMotionActors: the horse under the eye (0 = none)
inline std::atomic<std::uint64_t> riderNs{0};           // the last mounted rider shift (the horse shifts only while it is fresh)
inline std::atomic<float> riderX{0.f},riderZ{0.f};      // John's root (horizontal) for the horse's reference bone
inline std::atomic<std::uint64_t> riderFrames{0},horseFrames{0},horseNoBone{0},horseMismatch{0};
inline std::atomic<std::uint64_t> riderHorseBase{0},riderHeldBase{0},riderOwnBase{0}; // which height John's steady is measured against
inline std::atomic<float> riderShiftCm{0.f},horseShiftCm{0.f},riderBobCm{0.f},horseBobCm{0.f}; // bob = 1 s mean of |shift|
inline void noteBob(std::atomic<float>& a,float cm,float dt) noexcept {const float k=dt>0.f?1.f-std::exp(-dt/1.f):0.f;
    a.store(a.load(std::memory_order_relaxed)+(std::fabs(cm)-a.load(std::memory_order_relaxed))*k,std::memory_order_relaxed);}
struct Lever{bool on{true};float tau{.35f},maxM{.15f};};
inline Lever lever(std::uint64_t now) noexcept {
    static std::atomic<std::uint64_t> readNs{0};static std::atomic<int> on{1},tauMs{350},maxCm{15};
    const auto last=readNs.load(std::memory_order_relaxed);
    if(!last||now<last||now-last>1'000'000'000ull){readNs.store(now,std::memory_order_relaxed);char v[PROP_VALUE_MAX]{};
        on.store(!(__system_property_get("debug.rdrvr.vr.riding.steady",v)>0&&v[0]=='0'),std::memory_order_relaxed);
        tauMs.store(__system_property_get("debug.rdrvr.vr.riding.steadyms",v)>0?std::clamp(std::atoi(v),50,3000):350,std::memory_order_relaxed);
        maxCm.store(__system_property_get("debug.rdrvr.vr.riding.steadycm",v)>0?std::clamp(std::atoi(v),0,40):15,std::memory_order_relaxed);}
    // HORSE > HORSE SHAKE ON = the game's bounce (no ride steady)
    return {on.load(std::memory_order_relaxed)!=0&&!::rdrvr::vr::settings().rideShake(),float(tauMs.load(std::memory_order_relaxed))*1e-3f,
        float(maxCm.load(std::memory_order_relaxed))*.01f};
}
struct State{std::uintptr_t locals{};bool haveL0{};A::Mat34 writtenL0{};float baked{};bool have{};float y{},vy{};std::uint64_t ns{};
    int bone{-1};std::uintptr_t boneSkel{};std::uint64_t boneNs{};int head{-1};std::uint64_t headNs{};};
// One call: the new shift (m along up) for the shift-free reference height raw, Holt-smoothed; repeated calls within 2 ms keep the
// filter (the post-physics + compose calls of one frame)
inline float target(State& s,float raw,bool want,std::uint64_t now,const Lever& lv) noexcept {
    if(!want||!lv.on||!std::isfinite(raw)){s.have=false;return 0.f;}
    const bool fresh=!s.have||now<s.ns||now-s.ns>500'000'000ull;
    if(fresh){s.y=raw;s.vy=0.f;s.ns=now;s.have=true;}
    else if(now-s.ns>=2'000'000ull){const float dt=float(double(now-s.ns)*1e-9);s.ns=now;
        const float ka=1.f-std::exp(-dt/lv.tau),kb=1.f-std::exp(-dt/(2.f*lv.tau));const float pr=s.y+s.vy*dt,level=pr+(raw-pr)*ka;
        s.vy+=((level-s.y)/std::max(dt,1e-4f)-s.vy)*kb;s.y=level;}
    // rider / horse / head shifts can sit at the clamp: an error past the clamp is a real height change (a
    // mount, a dismount, a step) - the filter re-seeds there instead of holding the body off by the clamp for seconds
    if(!std::isfinite(s.y)||!std::isfinite(s.vy)||std::fabs(s.y-raw)>lv.maxM){s.y=raw;s.vy=0.f;}
    return std::clamp(s.y-raw,-lv.maxM,lv.maxM);
}
// Moves every global by (want - baked) along up and rewrites L0 = inverse(root) * g0; g (optional) = the caller's copy of the globals
inline bool apply(State& s,float* globals,unsigned char* locals,const A::Mat34& root,int count,const A::Vec3& upv,float want,A::Mat34* g) noexcept {
    const float d=want-s.baked;
    if(std::fabs(d)<1e-5f||count<=0)return false;
    const A::Vec3 dv=upv*d;
    for(int i=0;i<count;++i){float* m=globals+std::size_t(i)*16;A::Mat34 x=A::LoadMat34(m);x.d=x.d+dv;A::StoreMat34(m,x);if(g)g[i]=x;}
    float* l0p=reinterpret_cast<float*>(locals);const A::Mat34 g0=A::LoadMat34(globals);A::StoreMat34(l0p,A::InverseAffine(root)*g0);
    s.baked=want;s.writtenL0=A::LoadMat34(l0p);s.haveL0=true;s.locals=reinterpret_cast<std::uintptr_t>(locals);return true;
}
// At entry: the shift still baked into this skeleton's L0 (else the engine rebuilt the pose: nothing baked)
inline void entry(State& s,unsigned char* locals) noexcept {
    const A::Mat34 l0=A::LoadMat34(reinterpret_cast<const float*>(locals));
    if(!(s.haveL0&&s.locals==reinterpret_cast<std::uintptr_t>(locals)&&std::memcmp(&l0,&s.writtenL0,sizeof l0)==0)){s.baked=0.f;s.haveL0=false;}
}
inline State rider{},horse{},horseHead{};
inline std::atomic<float> headShiftCm{0.f},headBobCm{0.f};
// HORSE PARTS (VrRideSteadyLink.h, debug.rdrvr.vr.riding.horseparts 1):
// the mount's 4 legs, neck and head as capsules along its skeleton CHAINS (VrHorseChains.h: the selection, from the current pose of a
// STANDING horse), chosen ONCE per skeleton data (bone indices; retried every 0.5 s until it works). Per frame: the capsules from this
// frame's globals (the drawn pose after the steady shifts / head smoothing), world, each carried by its upper joint's bone (bone id = the
// engine index: a re-selection is a layout change for SpaceWarp); a segment longer than x1.5 its selected length drops the whole frame.
struct HorseChains {std::uintptr_t data{};int count{};std::uint64_t retryNs{};::rdrvr::vr::horsechains::Chains c{};};
inline HorseChains horseChains{}; // under horsePartsBusy
inline std::atomic<bool> horsePartsBusy{false};
inline std::atomic<const char*> horseChainState{"NOT_RUN"};
inline std::atomic<std::uint64_t> horseSelects{0},horseSelectFails{0},horsePartFrames{0},horsePartDrops{0};
inline std::atomic<unsigned> horseLegSegs{0},horseNeckSegs{0};
inline bool selectHorseChains(std::uintptr_t data,int count,const float* globals,const A::Mat34& root,std::uint64_t now) noexcept {
    namespace hcn=::rdrvr::vr::horsechains;auto& hc=horseChains;
    if(hc.data==data&&hc.count==count&&(hc.c.ok||now<hc.retryNs))return hc.c.ok;
    hc=HorseChains{};hc.data=data;hc.count=count;hc.retryNs=now+500'000'000ull;
    static std::array<std::int32_t,hcn::maxBones> par{};std::uintptr_t parents{};
    if(count<8||count>hcn::maxBones||!detail::get(data,off::dataParents,parents)||!parents||
       !detail::block(parents,par.data(),sizeof(std::int32_t)*std::size_t(count)))hc.c.why="PARENTS";
    else hc.c=hcn::select(par.data(),count,globals,root);
    horseChainState.store(hc.c.why,std::memory_order_relaxed);
    if(!hc.c.ok){horseSelectFails.fetch_add(1,std::memory_order_relaxed);return false;}
    horseLegSegs.store(hc.c.legSegs,std::memory_order_relaxed);horseNeckSegs.store(hc.c.neckSegs,std::memory_order_relaxed);
    horseSelects.fetch_add(1,std::memory_order_relaxed);return true;
}
inline void horseMotionParts(std::uintptr_t actor,std::uintptr_t data,const float* globals,int count,const A::Mat34& root,std::uint64_t now) noexcept {
    namespace sl=::rdrvr::vr::steadylink;
    if(!::rdrvr::vr::bodylink::horseProp.load(std::memory_order_relaxed)||!data)return;
    bool f=false;if(!horsePartsBusy.compare_exchange_strong(f,true,std::memory_order_acquire))return;
    struct Release{~Release(){horsePartsBusy.store(false,std::memory_order_release);}} release;
    if(!selectHorseChains(data,count,globals,root,now))return;
    const auto& hc=horseChains.c;static thread_local sl::HorseParts out{};out.ns=now;out.actor=actor;out.count=0; // ~1.2 KB: off the stack
    for(unsigned k=0;k<hc.segs&&k<sl::maxHorseParts;++k){const auto& sg=hc.seg[k];
        if(sg.a<0||sg.b<0||sg.a>=count||sg.b>=count){horsePartDrops.fetch_add(1,std::memory_order_relaxed);return;}
        const A::Mat34 G=A::LoadMat34(globals+std::size_t(sg.a)*16);const A::Vec3 a=G.d;A::Vec3 b=A::LoadMat34(globals+std::size_t(sg.b)*16).d;
        // bones do not stretch: longer than selected = a swapped / broken pose (a merged segment may fold shorter: a tucked hoof)
        const float l=A::Length(b-a);if(!(l<1.5f*sg.len+.02f)||!A::Finite(G)||!A::Finite(b)){horsePartDrops.fetch_add(1,std::memory_order_relaxed);return;}
        if(sg.extend>0.f&&l>.01f)b=b+(b-a)*(sg.extend/l);
        auto& p=out.part[out.count++];p.a[0]=a.x;p.a[1]=a.y;p.a[2]=a.z;p.b[0]=b.x;p.b[1]=b.y;p.b[2]=b.z;p.radius=sg.radius;p.bone=sg.a;
        const A::Vec3 rows[4]{G.a,G.b,G.c,G.d};for(int r=0;r<4;++r){p.m[r*3]=rows[r].x;p.m[r*3+1]=rows[r].y;p.m[r*3+2]=rows[r].z;}}
    sl::publishParts(out);horsePartFrames.fetch_add(1,std::memory_order_relaxed);
}
// the mount's root height read at the CALLER's hook time (a published stamp races across hook threads, and a cross-thread
// value lets John jitter against the saddle on a slope) - false when the mount is unknown or its root is > 1 m from John horizontally
inline bool mountRootY(float rx,float rz,float& y) noexcept {
    // guarded reads only (another actor: it may be gone - VrMotionActors validates it once per main tick, this runs on hook threads)
    const auto actor=mountActor.load(std::memory_order_relaxed);if(!actor)return false;
    std::uintptr_t ent{},inst{},cache{},skel{},rp{};float m[16]{};
    if(!detail::get(actor,off::actorEntity,ent)||!ent||!detail::get(ent,off::entityInst,inst)||!inst||!detail::get(inst,off::instCache,cache)||!cache||
       !detail::get(cache,off::cacheSkeleton,skel)||!skel||!detail::get(skel,off::skelRoot,rp)||!rp||!detail::block(rp,m,sizeof m))return false;
    const A::Mat34 root=A::LoadMat34(m);if(!A::Finite(root))return false;
    const float dx=root.d.x-rx,dz=root.d.z-rz;if(dx*dx+dz*dz>1.f)return false;
    y=root.d.y;return true;
}
// The horse (attachHook, before the original): its own chain, no Published check
inline void horseStep(std::uintptr_t actor,std::uint64_t now) noexcept {
    using detail::raw;
    const auto rn=riderNs.load(std::memory_order_relaxed);const Lever lv=lever(now);
    const bool want=rn&&now>=rn&&now-rn<300'000'000ull;
    const auto ent=raw<std::uintptr_t>(actor+off::actorEntity);
    const auto inst=ent?raw<std::uintptr_t>(ent+off::entityInst):0;
    const auto cache=inst?raw<std::uintptr_t>(inst+off::instCache):0;
    const auto skel=cache?raw<std::uintptr_t>(cache+off::cacheSkeleton):0;
    if(!skel){horseMismatch.fetch_add(1,std::memory_order_relaxed);return;}
    const auto gp=raw<std::uintptr_t>(skel+off::skelGlobals),lp=raw<std::uintptr_t>(skel+off::skelLocals),rp=raw<std::uintptr_t>(skel+off::skelRoot);
    const int count=int(raw<std::uint32_t>(skel+off::skelCount));
    if(!gp||!lp||!rp||count<=0||count>400){horseMismatch.fetch_add(1,std::memory_order_relaxed);return;}
    float* globals=reinterpret_cast<float*>(gp);unsigned char* locals=reinterpret_cast<unsigned char*>(lp);
    const A::Mat34 root=A::LoadMat34(reinterpret_cast<const float*>(rp));if(!A::Finite(root))return;
    auto& s=horse;entry(s,locals);
    const A::Vec3 upv{0.f,1.f,0.f};
    if(s.boneSkel!=skel||s.bone<0||s.bone>=count||now<s.boneNs||now-s.boneNs>2'000'000'000ull){ // the back bone under John
        const float rx=riderX.load(std::memory_order_relaxed),rz=riderZ.load(std::memory_order_relaxed);int best=-1;float bd=.25f; // (0.5 m)^2
        for(int i=0;i<count;++i){const float* m=globals+std::size_t(i)*16;const float dx=m[12]-rx,dz=m[14]-rz,h=m[13]-s.baked-root.d.y;
            const float d2=dx*dx+dz*dz;if(h>.8f&&d2<bd&&std::isfinite(d2)){bd=d2;best=i;}}
        s.bone=best;s.boneSkel=skel;s.boneNs=now;}
    const auto data=raw<std::uintptr_t>(skel+off::skelData); // horse parts: the chain selection's key
    if(s.bone<0){horseNoBone.fetch_add(1,std::memory_order_relaxed);apply(s,globals,locals,root,count,upv,0.f,nullptr);
        horseMotionParts(actor,data,globals,count,root,now);return;}
    const float rawY=globals[std::size_t(s.bone)*16+13]-s.baked-root.d.y; // above the horse root (hills)
    const float t=target(s,rawY,want,now,lv);
    apply(s,globals,locals,root,count,upv,t,nullptr);
    // the neck nods on top of the back's bounce. The head = the bone farthest
    // from John horizontally (> 0.5 m above the horse root); every bone ahead of John by > 0.4 m gets w * (smoothed - raw) of the head
    // height, w ramping 0..1 to the head (the neck bends smoothly). Globals only, fresh each frame (the engine rebuilds them from locals).
    {const float rx=riderX.load(std::memory_order_relaxed),rz=riderZ.load(std::memory_order_relaxed);
        if(s.head<0||s.head>=count||s.boneNs!=s.headNs){int hb=-1;float hd=.8f; // AHEAD along the root's forward (-c), never the tail
            float ax=-root.c.x,az=-root.c.z;const float al=std::sqrt(ax*ax+az*az);if(al>1e-3f){ax/=al;az/=al;}else{ax=0.f;az=1.f;}
            for(int i=0;i<count;++i){const float* m=globals+std::size_t(i)*16;const float f=(m[12]-rx)*ax+(m[14]-rz)*az,h=m[13]-root.d.y;
                if(h>.5f&&f>hd&&std::isfinite(f)){hd=f;hb=i;}}
            s.head=hb;s.headNs=s.boneNs;}
        if(s.head>=0){const float* hm=globals+std::size_t(s.head)*16;float fx=hm[12]-rx,fz=hm[14]-rz;const float fl=std::sqrt(fx*fx+fz*fz);
            if(fl>.8f&&fl<4.f){fx/=fl;fz/=fl;
                const Lever hl{lv.on,lv.tau,lv.maxM+.05f};const float e=target(horseHead,hm[13]-root.d.y,want,now,hl); // above the root
                // HEAD POSE: the height shift above leaves the NOD (a rotation) in.
                // Every bone ahead of John (the neck / head chain) keeps a low-passed copy of its whole pose in the horse ROOT frame (position +
                // axes, tau .riding.headms 250 ms) and is drawn at w * smoothed + (1 - w) * live, w 0..1 along the neck - the head moves with
                // the body, the gallop nod is gone. debug.rdrvr.vr.riding.headsteady 0 = the height-only shift.
                static std::array<A::Mat34,400> relS{};static std::array<bool,400> haveS{};static std::uintptr_t sKey{};static std::uint64_t sNs{};
                static int headOn{1},headMs{250};static std::uint64_t propNs{};
                if(!propNs||now-propNs>1'000'000'000ull){propNs=now;char v[PROP_VALUE_MAX]{};
                    headOn=!(__system_property_get("debug.rdrvr.vr.riding.headsteady",v)>0&&v[0]=='0');
                    headMs=__system_property_get("debug.rdrvr.vr.riding.headms",v)>0?std::clamp(std::atoi(v),30,3000):250;}
                const std::uintptr_t key=reinterpret_cast<std::uintptr_t>(globals);
                // HORSE SHAKE ON (lv.on false) = the head's own nod back
                // too; the cached poses are dropped while it is on (toggling OFF again must not resume from minutes-old poses)
                const bool headLive=headOn&&lv.on;
                if(key!=sKey||!want||!headLive||now<sNs||now-sNs>500'000'000ull){haveS.fill(false);sKey=key;}
                const float hdt=sNs&&now>sNs?std::min(float(double(now-sNs)*1e-9),.1f):0.f;sNs=now;
                const float k=headLive&&hdt>0.f?1.f-std::exp(-hdt/(float(headMs)*1e-3f)):1.f;
                const A::Mat34 inv=A::InverseAffine(root);
                const auto ortho=[](A::Mat34 m,const A::Mat34& ref){m.a=A::Normalize(m.a,ref.a);m.b=A::Normalize(m.b-m.a*A::Dot(m.a,m.b),ref.b);
                    A::Vec3 c=A::Cross(m.a,m.b);if(A::Dot(c,ref.c)<0.f)c=-c;m.c=c*A::Length(ref.c);return m;};
                for(int i=0;i<count&&i<400;++i){float* m=globals+std::size_t(i)*16;const float f=(m[12]-rx)*fx+(m[14]-rz)*fz;
                    const float w=std::clamp((f-.4f)/(fl-.4f),0.f,1.f);if(w<=0.f){haveS[std::size_t(i)]=false;continue;}
                    if(!headLive){m[13]+=w*e;continue;}
                    const A::Mat34 g=A::LoadMat34(m),rel=inv*g;auto& r=relS[std::size_t(i)];
                    if(!haveS[std::size_t(i)]){r=rel;haveS[std::size_t(i)]=true;}
                    else{r.d=r.d+(rel.d-r.d)*k;r.a=r.a+(rel.a-r.a)*k;r.b=r.b+(rel.b-r.b)*k;r.c=r.c+(rel.c-r.c)*k;r=ortho(r,rel);}
                    if(!A::Finite(r))r=rel; // one bad frame never poisons the rest of the ride
                    A::Mat34 o=root*r;o.d=g.d+(o.d-g.d)*w;o.a=g.a+(o.a-g.a)*w;o.b=g.b+(o.b-g.b)*w;o.c=g.c+(o.c-g.c)*w;o=ortho(o,g);
                    if(A::Finite(o))A::StoreMat34(m,o);}
                headShiftCm.store(e*100.f,std::memory_order_relaxed);
                {static std::uint64_t last{};const float dt=last&&now>last?std::min(float(double(now-last)*1e-9),.1f):0.f;last=now;noteBob(headBobCm,e*100.f,dt);}}}}
    horseMotionParts(actor,data,globals,count,root,now); // after every write of this call (the drawn pose)
    horseFrames.fetch_add(1,std::memory_order_relaxed);horseShiftCm.store(t*100.f,std::memory_order_relaxed);
    {static std::uint64_t last{};const float dt=last&&now>last?std::min(float(double(now-last)*1e-9),.1f):0.f;last=now;noteBob(horseBobCm,t*100.f,dt);}
}
} // namespace steady
inline ApplyResult applyBody(const Rig& r,float* globals,unsigned char* locals,const A::Mat34& root,int count,const Frame& f,bool frameOk,
                             bool gateOpen,std::uint64_t now,std::uint64_t serial,const Config& c,bool mountedArm=false,int seatMode=0) noexcept {
    ApplyResult res{};
    const float dt=ik.lastNs&&now>ik.lastNs?std::min(float(double(now-ik.lastNs)/1e9),.1f):0.f;ik.lastNs=now;
    const bool write=!c.dry;
    static thread_local std::array<A::Mat34,maxBones> g{};
    for(int i=0;i<count;++i)g[std::size_t(i)]=A::LoadMat34(globals+std::size_t(i)*16);
    ik.saved=Saved{};ik.saved.serial=serial;
    const float cmPer=100.f/std::max(f.upm,1e-3f);
    const A::Vec3 upv=A::Normalize(f.up,{0,1,0});
    // the animated head joint above the skeleton root (= the capsule base) -> the camera's AUTO eye height (VrCamera; the native
    // head query sees the collapsed head). Height is invariant under the yaw / the horizontal lock below.
    // where the character's own eyes are (animated eye pivots, before the module's yaw / lock: heights are invariant) relative
    // to the head joint and above the root - BODY LOWER = eye above the head joint, so "BODY LOWER x" = the character's eyes + (x - this).
    if(gateOpen&&r.bones.head>=0&&r.eyePivot[0]>=0&&r.eyePivot[1]>=0&&r.eyePivot[0]<count&&r.eyePivot[1]<count){
        const A::Vec3 e=(g[std::size_t(r.eyePivot[0])].d+g[std::size_t(r.eyePivot[1])].d)*.5f,h=g[std::size_t(r.bones.head)].d;
        const Basis eb=bodyBasis(g.data(),r,f.up);
        const float up=A::Dot(e-h,upv)*cmPer,fw=eb.valid?A::Dot(e-h,eb.forward)*cmPer:0.f,ar=A::Dot(e-root.d,upv)*cmPer;
        if(std::isfinite(up+fw+ar)&&std::fabs(up)<40.f){const float k=stats.charEyeFrames.load(std::memory_order_relaxed)?A::SmoothFactor(dt,1.f):1.f;
            const auto ema=[&](std::atomic<float>& a,float v){a.store(a.load(std::memory_order_relaxed)+(v-a.load(std::memory_order_relaxed))*k,std::memory_order_relaxed);};
            ema(stats.charEyeAboveHeadCm,up);ema(stats.charEyeFwdOfHeadCm,fw);ema(stats.charEyeAboveRootCm,ar);bump(stats.charEyeFrames);}}
    if(gateOpen&&r.bones.head>=0){const float h=A::Dot(g[std::size_t(r.bones.head)].d-root.d,upv)*cmPer;
        if(std::isfinite(h)&&h>50.f&&h<250.f){stats.headAboveRootCm.store(h,std::memory_order_relaxed);
            ::rdrvr::vr::status::bodyHeadMm.store(int(std::lround(h*10.f)),std::memory_order_relaxed);}}
    // ---- body yaw, then the head lock: one write of all globals + L0 ----
    {
        // the head (plan) of this frame, or a recent one (a hitch / a stale controller sample must not ramp the body out and back)
        const bool headNow=frameOk&&f.headValid;
        // TELEPORT: the camera's B snap after a warp moves the lock target relative to the root by the player's lean;
        // a frozen lock would break away (> lockBreakCm) and slide the body back under the head over unfreezeS. After
        // the warp: no freeze, no breakaway, no blend, the target is re-taken from the live head. Hook A runs ~2 ms
        // BEFORE PostCameraUpdate, so the warp frame (and usually the next) still carries the head of a plan
        // drawn before the camera's B snap; a freeze re-latched there would keep the pre-snap
        // lean and break away (+ the 0.35 s slide) once the post-snap head arrives. The snap is
        // therefore re-applied on EVERY hook-A frame until this frame's head comes from a camera
        // sample at/after the camera's warp snap of this serial (plan headSampledNs >=
        // CameraResult.key, the pair-identity key K); at most 300 ms (camera killed / no snap /
        // no plan key: warpSnapTimeouts).
        {namespace tw=::rdrvr::vr::tploco::warp;
         const auto ws=tw::bodySerial.load(std::memory_order_acquire);
         if(ws&&ws!=ik.warpSeen){ik.warpSeen=ws;ik.warpPending=true;ik.warpSinceNs=now;
             bump(stats.warpSnaps);tw::bodySnapped.store(ws,std::memory_order_release);::rdrvr::vr::tploco::linkBump(tw::bodySnaps);}
         if(ik.warpPending){
             ik.frozen=false;ik.unfreezeT=1.f;ik.breakaway=false;ik.haveTarget=false;ik.freezeWantNs=0;bump(stats.warpSnapFrames);
             tw::CameraResult cr{};
             const bool keyed=frameOk&&f.headSampledNs&&tw::camera.load(cr)&&cr.serial==ik.warpSeen&&cr.snapped&&cr.key&&f.headSampledNs>=cr.key;
             if(keyed)ik.warpPending=false;   // this frame re-takes the target (a latched freeze: its frozen offset) from the post-snap head
             else if(now<ik.warpSinceNs||now-ik.warpSinceNs>300'000'000ull){ik.warpPending=false;bump(stats.warpSnapTimeouts);}
         }}
        if(headNow)ik.lastHeadNs=now;
        const bool headRecent=headNow||(ik.lastHeadNs&&now>=ik.lastHeadNs&&now-ik.lastHeadNs<std::uint64_t(double(c.holdS)*1e9));
        if(headNow&&!f.handsInput)bump(stats.headOnlyFrames);
        // freeze yaw + lock while looking down (hysteresis) or while a hand is at a belt holster; held freezeHoldS
        bool byPitch=false,byHand=false;
        // pitch = elevation of the HMD forward above the horizontal plane of the world up (looking down = negative; the same `upv`
        // gives headAboveRootCm > 0, so the sign is the world's). Non-finite = no update.
        if(headNow){const A::Vec3 hf3=A::Normalize(f.headFwd,{0,0,1});const float pitch=std::asin(std::clamp(A::Dot(hf3,upv),-1.f,1.f))/A::kDegToRad;
            if(std::isfinite(pitch)){stats.lastPitchDeg.store(pitch,std::memory_order_relaxed);
                if(!gateOpen){} // The latch is only earned while the body is driven
                else if(ik.pitchLatched){if(pitch>c.unfreezePitchDeg)ik.pitchLatched=false;}else if(pitch<c.freezePitchDeg)ik.pitchLatched=true;}}
        // a latch never survives a gap (gate closed / no head for holdS): it is re-earned by looking down again
        if((!gateOpen||!headRecent)&&ik.pitchLatched){ik.pitchLatched=false;bump(stats.latchClears);}
        stats.pitchLatchedNow.store(ik.pitchLatched,std::memory_order_relaxed);
        if(c.freeze&&gateOpen){byPitch=ik.pitchLatched;byHand=c.freezeHands&&handAtBelt(f,frameOk,c);}
        if(byPitch||byHand)ik.freezeWantNs=now;
        const bool frozenNow=byPitch||byHand||(ik.frozen&&ik.freezeWantNs&&now>=ik.freezeWantNs&&now-ik.freezeWantNs<std::uint64_t(double(c.freezeHoldS)*1e9));
        stats.frozenNow.store(frozenNow,std::memory_order_relaxed);
        if(frozenNow){bump(stats.freezeFrames);if(byPitch)bump(stats.freezeByPitch);if(byHand)bump(stats.freezeByHand);}
        // breakaway - the head heading vs the body yaw target. A freeze only damps small corrections: past breakDeg the
        // body turns anyway (re-engage smoothing) until it is back within reengageDeg and the lock blend finished. Evaluated only while the
        // forward has a usable horizontal part (|pitch| < ~69 deg); looking straight down keeps the previous decision.
        if(headNow&&ik.haveYaw){
            const A::Vec3 hf=f.headFwd-upv*A::Dot(f.headFwd,upv),bf=ik.smoothFwd-upv*A::Dot(ik.smoothFwd,upv);
            if(A::Length(hf)>=.35f&&A::Length(bf)>=.1f){
                const float diff=std::fabs(A::SignedAngleAbout(bf,hf,upv))/A::kDegToRad;
                if(std::isfinite(diff)){stats.headVsBodyDeg.store(diff,std::memory_order_relaxed);maxOf(stats.headVsBodyDegMax,diff);
                    if(ik.breakaway){if(diff<c.reengageDeg&&ik.unfreezeT>=1.f)ik.breakaway=false;}
                    else if(frozenNow&&diff>c.breakDeg){ik.breakaway=true;bump(stats.freezeBreaks);bump(stats.breakByYaw);}}}
        }
        if(!headRecent||!gateOpen)ik.breakaway=false;
        if(!frozenNow&&ik.unfreezeT>=1.f)ik.breakaway=false; // nothing to override any more
        const bool frozenYaw=frozenNow&&!ik.breakaway;
        if(frozenNow&&ik.breakaway)bump(stats.breakFrames);
        const bool want=gateOpen&&headRecent&&c.yaw;
        ik.yawWeight=stepWeight(ik.yawWeight,want,dt,c.rampS);stats.yawWeight.store(ik.yawWeight,std::memory_order_relaxed);
        float* l0p=reinterpret_cast<float*>(locals);A::Mat34 l0=A::LoadMat34(l0p);
        const bool sticky=ik.haveL0&&ik.yawSkel==reinterpret_cast<std::uintptr_t>(locals)&&std::memcmp(&l0,&ik.writtenL0,sizeof l0)==0;
        if(!sticky){ik.acc=0.f;ik.bakedT={};}else bump(stats.yawSticky);
        bool moved=false,active=false;
        if(headNow){
            // looking (nearly) straight down the horizontal part of the view direction is noise -> the target freezes
            // frozen = no update at all; after a freeze the yaw re-engages slowly (unfreezeS) instead of snapping in 80 ms
            const A::Vec3 hf=f.headFwd-upv*A::Dot(f.headFwd,upv);const float hl=A::Length(hf);
            const float tau=ik.unfreezeT<1.f?std::max(c.yawTauMs*.001f,c.unfreezeS):c.yawTauMs*.001f;
            const float k=frozenYaw?0.f:A::SmoothFactor(dt,tau)*A::Clamp01((hl-.15f)/.35f);
            if(hl<.5f)bump(stats.yawPitchFrozen);
            ik.smoothFwd=ik.haveYaw?A::Normalize(A::Lerp(ik.smoothFwd,f.headFwd,k),ik.smoothFwd):f.headFwd;ik.haveYaw=true;
        }
        if((ik.yawWeight>0.f||ik.acc!=0.f)&&ik.haveYaw){
            active=true;
            const float d=applyYaw(g.data(),l0,root,count,r,f.up,ik.smoothFwd,ik.yawWeight,c.yawMaxDeg*A::kDegToRad,sticky,ik.acc);
            if(d!=0.f){moved=true;
                bump(ik.yawWeight>0.f?stats.yawApplied:stats.yawUndo);
                stats.yawLastDeg.store(ik.acc/A::kDegToRad,std::memory_order_relaxed);maxOf(stats.yawMaxDeg,std::fabs(ik.acc/A::kDegToRad));
            }
        }
        // ---- head lock: the head joint horizontally on the neck-model target BACK behind the eye. The body camera is the capsule +
        // eye height; the animation (idle sway, lean, the yaw about the root) moves the head relative to it.
        const int hb=r.bones.head;
        if(hb>=0&&hb<count){
            const bool lockWant=gateOpen&&headRecent&&c.lock&&f.cameraBody;
            ik.lockWeight=stepWeight(ik.lockWeight,lockWant,dt,c.rampS);stats.lockWeight.store(ik.lockWeight,std::memory_order_relaxed);
            const Basis bb=bodyBasis(g.data(),r,f.up);
            const A::Vec3 animHead=g[std::size_t(hb)].d-ik.bakedT; // the head of the animation (+ yaw) without the baked translation
            A::Vec3 live{};const bool haveLive=headNow&&neckTarget(f,ik.smoothFwd,c,live);
            A::Vec3 target{};bool haveTarget=false;
            // a real step / lean while frozen (the live target > lockBreakCm from the frozen one) breaks the freeze too
            if(frozenNow&&!ik.breakaway&&ik.frozen&&haveLive){A::Vec3 dl=live-(root.d+ik.frozenRel);dl=dl-upv*A::Dot(dl,upv);
                if(A::Length(dl)*cmPer>c.lockBreakCm){ik.breakaway=true;bump(stats.freezeBreaks);bump(stats.breakByLock);}}
            if(frozenNow&&!ik.breakaway&&(haveLive||ik.haveTarget)){
                if(!ik.frozen){ik.frozen=true;bump(stats.freezeEnters);ik.frozenRel=ik.haveTarget?ik.lastTargetRel:live-root.d;}
                ik.unfreezeT=0.f;target=root.d+ik.frozenRel;haveTarget=true;
            }else{
                ik.frozen=false;
                if(haveLive){
                    target=live;
                    if(ik.unfreezeT<1.f){ik.unfreezeT=c.unfreezeS>0.f?std::min(1.f,ik.unfreezeT+dt/c.unfreezeS):1.f; // blend back to the live target
                        const float t=ik.unfreezeT*ik.unfreezeT*(3.f-2.f*ik.unfreezeT);target=A::Lerp(root.d+ik.frozenRel,live,t);}
                    haveTarget=true;
                }else if(ik.haveTarget&&headRecent){target=root.d+ik.lastTargetRel;haveTarget=true;if(lockWant)bump(stats.lockHeld);} // drop-out: hold
            }
            if(haveTarget){ik.lastTargetRel=target-root.d;ik.haveTarget=true;
                A::Vec3 dv=target-animHead;dv=dv-upv*A::Dot(dv,upv);ik.lastLockDv=dv;ik.haveLockDv=true;
                if(bb.valid&&headNow){const A::Vec3 sw=(animHead-target)*cmPer,rh=(animHead-root.d)*cmPer,rp=(root.d-target)*cmPer;
                    ik.osc[OscSwayFwd].add(A::Dot(sw,bb.forward),dt,stats.osc[OscSwayFwd]);ik.osc[OscSwayRight].add(A::Dot(sw,bb.right),dt,stats.osc[OscSwayRight]);
                    ik.osc[OscHeadRootFwd].add(A::Dot(rh,bb.forward),dt,stats.osc[OscHeadRootFwd]);
                    ik.osc[OscRootPivotFwd].add(A::Dot(rp,bb.forward),dt,stats.osc[OscRootPivotFwd]);}
            }else if(lockWant)bump(stats.lockNoPivot);
            if(!headRecent){ik.haveTarget=false;ik.frozen=false;ik.unfreezeT=1.f;ik.breakaway=false;}
            stats.breakNow.store(ik.breakaway,std::memory_order_relaxed);
            if((ik.lockWeight>0.f&&ik.haveLockDv)||A::Length(ik.bakedT)>0.f){
                active=true;
                A::Vec3 wantT=ik.haveLockDv?ik.lastLockDv*ik.lockWeight:A::Vec3{};
                const float maxLen=c.lockMaxCm/cmPer,len=A::Length(wantT);
                if(len>c.lockKnee*maxLen&&len>0.f){wantT=wantT*(softLimit(len,maxLen,c.lockKnee)/len);bump(stats.lockClamped);}
                const A::Vec3 delta=wantT-ik.bakedT;ik.bakedT=wantT;
                if(A::Length(delta)>1e-6f*std::max(f.upm,1e-3f)&&A::Finite(delta)){
                    for(int i=0;i<count;++i)g[std::size_t(i)].d=g[std::size_t(i)].d+delta;
                    l0=A::InverseAffine(root)*g[0];moved=true;
                }
                if(ik.lockWeight>0.f)bump(stats.lockApplied);
                stats.lockLastCm.store(A::Length(wantT)*cmPer,std::memory_order_relaxed);maxOf(stats.lockCmMax,A::Length(wantT)*cmPer);
            }
            if(haveTarget&&headNow&&bb.valid){const A::Vec3 res2=(g[std::size_t(hb)].d-target)*cmPer;ik.osc[OscResidualFwd].add(A::Dot(res2,bb.forward),dt,stats.osc[OscResidualFwd]);}
        }
        if(moved&&write){for(int i=0;i<count;++i)A::StoreMat34(globals+std::size_t(i)*16,g[std::size_t(i)]);A::StoreMat34(l0p,l0);res.wrote=true;}
        if(active&&write){ik.writtenL0=A::LoadMat34(l0p);ik.haveL0=true;ik.yawSkel=reinterpret_cast<std::uintptr_t>(locals);}
        else{ik.haveL0=false;ik.acc=0.f;ik.bakedT={};} // dry / idle: nothing was baked into L0
    }
    // ---- eye vs head / shoulders (the view must sit at the character's eyes: shoulders ~20-25 cm below, ~10 cm behind) ----
    if(frameOk&&r.bones.head>=0){
        const Basis b=bodyBasis(g.data(),r,f.up);
        if(b.valid){const A::Vec3 d=(f.eye-g[std::size_t(r.bones.head)].d)*cmPer;
            bump(stats.eyeSamples);stats.eyeCm.store(A::Length(d),std::memory_order_relaxed);maxOf(stats.eyeCmMax,A::Length(d));
            stats.eyeFwdCm.store(A::Dot(d,b.forward),std::memory_order_relaxed);stats.eyeUpCm.store(A::Dot(d,b.up),std::memory_order_relaxed);
            stats.eyeRightCm.store(A::Dot(d,b.right),std::memory_order_relaxed);
            const int ar=r.bones.arm[0].arm,al=r.bones.arm[1].arm;
            if(ar>=0&&al>=0&&ar<count&&al<count){const A::Vec3 sh=(g[std::size_t(ar)].d+g[std::size_t(al)].d)*.5f;const A::Vec3 e=(f.eye-sh)*cmPer;
                A::Vec3 hf=f.headFwd-upv*A::Dot(f.headFwd,upv);hf=A::Normalize(hf,b.forward);
                stats.shoulderBelowEyeCm.store(A::Dot(e,upv),std::memory_order_relaxed);stats.shoulderBehindEyeCm.store(A::Dot(e,hf),std::memory_order_relaxed);}
            if(r.bones.neck>=0&&r.bones.neck<count)stats.neckBelowEyeCm.store(A::Dot(f.eye-g[std::size_t(r.bones.neck)].d,upv)*cmPer,std::memory_order_relaxed);
            // the same as 2 s means over upright frames (|pitch| < 20 deg, gate open): the last-frame values above can be the frame the
            // headset came off. These are the numbers to tune BODY LOWER / BACK from.
            const float pitch=std::asin(std::clamp(A::Dot(A::Normalize(f.headFwd,{0,0,1}),upv),-1.f,1.f))/A::kDegToRad;
            if(gateOpen&&f.headValid&&std::fabs(pitch)<20.f&&ar>=0&&al>=0&&ar<count&&al<count&&r.bones.neck>=0&&r.bones.neck<count){
                const A::Vec3 sh=(g[std::size_t(ar)].d+g[std::size_t(al)].d)*.5f;
                const float uh=A::Dot(d,upv),fh=A::Dot(d,b.forward),us=A::Dot(f.eye-sh,upv)*cmPer,un=A::Dot(f.eye-g[std::size_t(r.bones.neck)].d,upv)*cmPer;
                if(std::isfinite(uh+fh+us+un)){
                    if(!ik.haveUpright){ik.upHead=uh;ik.fwdHead=fh;ik.upShoulder=us;ik.upNeck=un;ik.haveUpright=true;}
                    else{const float k=A::SmoothFactor(dt,2.f);ik.upHead+=(uh-ik.upHead)*k;ik.fwdHead+=(fh-ik.fwdHead)*k;ik.upShoulder+=(us-ik.upShoulder)*k;ik.upNeck+=(un-ik.upNeck)*k;}
                    bump(stats.uprightSamples);stats.eyeAboveHeadMean.store(ik.upHead,std::memory_order_relaxed);stats.eyeFwdHeadMean.store(ik.fwdHead,std::memory_order_relaxed);
                    stats.eyeAboveShoulderMean.store(ik.upShoulder,std::memory_order_relaxed);stats.eyeAboveNeckMean.store(ik.upNeck,std::memory_order_relaxed);}
            }}
    }
    // ---- RIDE STEADY (steady:: above): John's skeleton follows the smoothed saddle height - before the seat hold ----
    // John's ROOT is smooth on the saddle, his bounce is the riding animation's PELVIS: the reference
    // is the pelvis joint height minus the shift baked into it (the root when the rig has no pelvis)
    {namespace st=steady;const auto lv=st::lever(now);const bool riding=seatMode==2||mountedArm;
        st::entry(st::rider,locals);
        // the pelvis height ABOVE THE ROOT - a slope moves the root, the
        // gait bounce the pelvis; filtering the world height would make a hill a ramp the filter chases (and re-seeds past the clamp)
        // John's pelvis moves rigidly with HIS root, which rides the saddle, so when seated the height is measured above the HORSE's root
        // (the frame the horse steady and the head pose use; a slope moves both
        // roots), John's own root when no fresh horse root (on foot, a wagon)
        // the horse base only when SEATED (seatMode 2); the climb (mountedArm, seatMode 1) keeps John's own root; the
        // mount's root read now (st::mountRootY), raw .y for both heights; a short miss (<= 300 ms) keeps the last horse-vs-John offset (a base
        // flip re-seeds the filter = a one-frame snap); the read only while the shift runs
        static float lastOff{};static std::uint64_t lastOffNs{};float hy=0.f;bool useHorse=false;
        if(seatMode==2&&riding&&write&&lv.on){
            if(st::mountRootY(root.d.x,root.d.z,hy)){useHorse=true;lastOff=hy-root.d.y;lastOffNs=now;st::riderHorseBase.fetch_add(1,std::memory_order_relaxed);}
            else if(lastOffNs&&now>=lastOffNs&&now-lastOffNs<300'000'000ull){useHorse=true;hy=root.d.y+lastOff;st::riderHeldBase.fetch_add(1,std::memory_order_relaxed);}
            else st::riderOwnBase.fetch_add(1,std::memory_order_relaxed);}
        const int pb=r.bones.pelvis;
        const float refY=pb<0||pb>=count?0.f:useHorse?g[std::size_t(pb)].d.y-st::rider.baked*upv.y-hy:A::Dot(g[std::size_t(pb)].d,upv)-st::rider.baked-A::Dot(root.d,upv);
        const float t=st::target(st::rider,refY,riding&&write,now,lv);st::noteBob(st::riderBobCm,t*100.f,dt);
        if(st::apply(st::rider,globals,locals,root,count,upv,t,g.data()))res.wrote=true;
        if(riding){st::riderNs.store(now,std::memory_order_relaxed);st::riderX.store(root.d.x,std::memory_order_relaxed);st::riderZ.store(root.d.z,std::memory_order_relaxed);
            st::riderFrames.fetch_add(1,std::memory_order_relaxed);}
        st::riderShiftCm.store(st::rider.baked*100.f,std::memory_order_relaxed);} // the shift in the skeleton now (0 once unwound)
    // ---- SEAT: the upper body seated under the VR head (horse; ramp out after) - before the arms, which solve from its shoulders ----
    if(seatStep(r,g.data(),globals,locals,root,count,f,frameOk,dt,now,seatMode,c))res.wrote=true;
    // ---- arms ----
    const Basis basis=bodyBasis(g.data(),r,f.up);
    // RIDING: on a horse the arm memory (last target, drop-out hold, blend-out, source crossfade) lives in the RIDER frame (root
    // translation + heading): a held / blending target rides along with the horse instead of staying behind in world (~20-30 cm per
    // frame at a canter, the "yank"). On foot (and .body.mountrel 0): the world memory, no hold, no crossfade.
    const bool riderRel=mountedArm&&c.mountRel;
    const A::Mat34 rider=yawFrame(root,upv),riderInv=A::InverseOrtho(rider);
    const std::uint64_t holdNs=std::uint64_t(double(c.mountHoldMs)*1e6);const float fadeS=c.mountFadeMs*.001f;
    HandBox box{};bool boxAny=false; // ARC: published below when an arm was solved
    // LEFT hold: the engine holds every item in its RIGHT hand (the right arm's animation grips it, the left one's does not), so the
    // left fingers get the right hand's animated pose mirrored (read before the right arm's own finger curl overwrites it).
    std::array<A::Quat,A::kFingers*A::kJoints> leftPose{};std::uint16_t leftPoseMask=0;
    if(frameOk&&c.fingers&&c.leftMirror&&r.fingerMirror&&((f.hold==0&&f.weapon)||(f.hold!=0&&f.phantomHand==0&&f.phantomWeapon))){
        const auto& br=r.bones.arm[0];
        for(int fi=0;fi<A::kFingers;++fi)for(int j=0;j<A::kJoints;++j){const int bone=br.finger[fi][j];if(bone<0||bone>=count)continue;
            const A::Quat q=A::MirrorLocalX(A::ToQuat(A::Orthonormalize(A::LoadMat34(reinterpret_cast<const float*>(locals+std::size_t(bone)*off::localStride)).Rot())));
            if(std::isfinite(q.x+q.y+q.z+q.w)){leftPose[std::size_t(fi*A::kJoints+j)]=q;leftPoseMask=std::uint16_t(leftPoseMask|(1u<<unsigned(fi*A::kJoints+j)));}}
    }
    for(int side=0;side<2;++side){
        auto& mem=ik.arm[std::size_t(side)];auto& st=stats.arm[std::size_t(side)];
        const int vr=side==0?1:0; // VR hand of this engine side (VR 0 = left)
        ArmJob job{};job.side=side;job.write=write;bool have=false;
        const auto& b=r.bones.arm[std::size_t(side)];const auto& rig=r.arm[std::size_t(side)];
        const bool armOk=r.armOk[std::size_t(side)]&&c.arms;
        // RIDING: the gun arm only (the other hand keeps the reins).
        // with REINS on BOTH arms always follow their controllers (free hand / derived weapon / support exactly as on foot) and the game's rope
        // comes to the holding hand(s) instead (ropeStep: rein+0x190 - the rope reads the rider's LIVE wrist_l / wrist_r after this hook).
        // .reins.arm 0 = rein hands: the right hand on the reins next to the native left hand (10 cm to the rider's right, the controller's
        // rotation), a left hand on the reins = native pose.
        const bool reinsOn=mountedArm&&::rdrvr::vr::reins::armsFollow(now);
        const bool reinsRight=!c.reinsArm&&reinsOn&&side==0&&::rdrvr::vr::reins::holds(1u)&&r.bones.arm[1].wrist>=0&&basis.valid;
        const bool weaponArm=mountedArm&&((f.hold==vr&&f.weapon)||(c.reinsArm?reinsOn:(reinsRight||(reinsOn&&!::rdrvr::vr::reins::holds(unsigned(vr))))));
        // the arm should be driven - a missing hand pose then holds instead of ramping out (a failed frame keeps last frame's wish)
        const bool wanted=armOk&&(gateOpen||weaponArm||(riderRel&&!frameOk&&mem.wanted));mem.wanted=wanted;
        unsigned source=SourceNone;
        if(armOk&&frameOk&&(gateOpen||weaponArm)){
            const auto& h=f.hand[std::size_t(vr)];
            // what this hand holds: the engine item or the DUAL phantom (the same rules)
            const bool engineHeld=f.hold==vr&&f.weapon,phantomHeld=!engineHeld&&f.phantomHand==vr&&f.phantomWeapon;
            const bool holding=engineHeld||phantomHeld;
            const bool placed=engineHeld?f.placed:phantomHeld&&f.phantomPlaced,alt=engineHeld?f.alt:f.phantomAlt;
            const A::Mat34& attachment=engineHeld?f.attachment:f.phantomAttachment;
            if(weaponArm)bump(stats.mountedArmFrames);
            if(reinsRight&&!holding&&h.valid){
                job.target=freeHandTarget(h,rig.hand,true,f.upm,c);job.target.d=g[std::size_t(r.bones.arm[1].wrist)].d+basis.right*(.10f*f.upm);
                job.fingers=A::FingerMode::Free;job.grip=h.squeeze;job.trigger=h.trigger;have=true;bump(st.free);source=SourceFree;
            }else if(holding&&placed&&c.derive&&b.attach>=0&&alt==(side==1)){ // the weapon's own attachment side = this arm
                job.target=wristFromAttachment(attachment,g[std::size_t(b.wrist)],g[std::size_t(b.attach)]);
                job.fingers=A::FingerMode::Native;have=true;bump(st.derived);source=SourceDerived;
            }else if(h.valid){
                if(f.support==vr&&f.placed&&f.hold>=0&&f.hold!=vr){job.target=supportTarget(h,rig.hand,side==0,f,c);job.fingers=A::FingerMode::Support;bump(st.support);source=SourceSupport;}
                else{job.target=freeHandTarget(h,rig.hand,side==0,f.upm,c);job.fingers=holding?A::FingerMode::Native:A::FingerMode::Free;bump(holding?st.nativeHold:st.free);source=SourceFree;}
                job.grip=h.squeeze;job.trigger=h.trigger;have=true;
                if(holding&&placed&&!(b.attach>=0&&alt==(side==1)))bump(stats.weaponDerivedMismatch);
            }else bump(st.noTarget);
            // LEFT hold (derived firearm or a hand-held item): the left animation does not grip it - the right hand's grip mirrored,
            // else a procedural grip (the index on the trigger of a firearm, curled round a knife / lasso / thrown item)
            if(have&&holding&&side==1&&c.fingers){
                const bool gun=engineHeld?f.firearm:f.phantomFirearm;
                job.fingers=A::FingerMode::Grip;job.trigger=gun?(h.valid?h.trigger:0.f):1.f;
                if(leftPoseMask){job.fingerPose=leftPose.data();job.fingerPoseMask=leftPoseMask;bump(st.holdMirror);}else bump(st.holdGrip);
            }
        }
        const bool live=have; // ARC: solved to this frame's controller target (not the blend-out of the last one)
        bool held=false;
        if(riderRel){
            if(have){ // crossfade over mountFadeMs: a new target source (free / derived / support), or back from a hold / blend-out
                      // of > 100 ms since the last live frame (a 1-2 frame drop-out at 36 Hz resumes directly: a fade there would only make
                      // the hand lag the controller)
                const bool switched=mem.liveSource!=SourceNone&&source!=mem.liveSource;
                const bool resumed=mem.source==SourceHold&&now>=mem.liveNs&&now-mem.liveNs>100'000'000ull;
                if(fadeS>0.f&&mem.haveRel&&(switched||resumed)){mem.fadeFrom=mem.lastRel;mem.fadeT=0.f;bump(st.mountFades);}
                mem.source=source;
                if(mem.fadeT<1.f){mem.fadeT=fadeS>0.f?std::min(1.f,mem.fadeT+dt/fadeS):1.f;const float t=mem.fadeT*mem.fadeT*(3.f-2.f*mem.fadeT);
                    job.target=blendTarget(rider*mem.fadeFrom,job.target,t);bump(st.mountFadeFrames);}
            }else{
                if(wanted)bump(st.mountNone);
                // hold: a missing hand pose (no frame / no controller sample / mapRay miss) keeps the weight and the rider-relative last
                // target for mountHoldMs, only then the arm ramps out (still rider-relative)
                if(wanted&&mem.haveLast&&mem.haveRel&&holdNs&&now>=mem.liveNs&&now-mem.liveNs<holdNs){
                    job.target=rider*mem.lastRel;job.fingers=mem.lastFingers;job.grip=mem.lastGrip;job.trigger=mem.lastTrigger;held=true;bump(st.mountHold);}
                else{if(mem.holding)bump(st.mountHoldExpired);if(armOk&&mem.weight>0.f&&mem.haveLast)bump(st.mountBlendOut);}
                if(mem.haveLast)mem.source=SourceHold;
            }
            mem.holding=held;
        }else{mem.fadeT=1.f;mem.holding=false;mem.source=have?source:SourceNone;} // on foot: no hold / crossfade (unchanged)
        if(!held)mem.weight=stepWeight(mem.weight,have,dt,c.rampS);
        st.weight.store(mem.weight,std::memory_order_relaxed);
        if(have){mem.lastTarget=job.target;mem.lastRel=riderInv*job.target;mem.haveRel=true;mem.lastFingers=job.fingers;mem.lastGrip=job.grip;
            mem.lastTrigger=job.trigger;mem.liveNs=now;mem.liveSource=source;mem.haveLast=true;}
        else if(held)have=true;
        else if(armOk&&mem.weight>0.f&&mem.haveLast){ // blend out from the last target (on a horse: rider-relative, with the last grip)
            if(riderRel&&mem.haveRel){job.target=rider*mem.lastRel;job.grip=mem.lastGrip;job.trigger=mem.lastTrigger;}else job.target=mem.lastTarget;
            job.fingers=mem.lastFingers;have=true;}
        if(!armOk||mem.weight<=0.f||!have){
            if(mem.weight<=0.f){mem.haveLast=mem.haveRel=mem.holding=false;mem.source=mem.liveSource=SourceNone;mem.fadeT=1.f;}continue;}
        job.weight=mem.weight;
        A::ArmPoseOutput out{};
        if(!solveArm(r,side,g.data(),globals,locals,job,mem,basis,c,out,ik.saved)){bump(st.poseFailed);continue;}
        bump(st.applied);++res.arms;res.wrote=res.wrote||write;
        const float clampCm=out.ik.clampedBy*100.f/std::max(f.upm,1e-3f);st.lastClampCm.store(clampCm,std::memory_order_relaxed);
        if(clampCm>.1f){bump(st.clamped);maxOf(st.clampCmMax,clampCm);}
        if(live&&write&&f.hand[std::size_t(vr)].valid){ // ARC: the controller grip origin carried onto the IK'd hand (target -> solved hand)
            const A::Vec3 rel=A::TransformPoint(A::InverseAffine(job.target),f.hand[std::size_t(vr)].gripPos);
            const A::Vec3 gp=A::TransformPoint(out.ik.hand,rel),wp=out.ik.hand.d;
            if(A::Finite(gp)&&A::Finite(wp)){box.ikDriven[std::size_t(vr)]=mem.weight>=1.f?1:0;box.grip[std::size_t(vr)]={gp.x,gp.y,gp.z};
                box.wrist[std::size_t(vr)]={wp.x,wp.y,wp.z};box.clampCm[std::size_t(vr)]=clampCm;boxAny=true;}
        }
        if(live&&write&&vr==0&&f.hand[0].valid&&b.elbow>=0&&b.wrist>=0){ // The solved LEFT forearm relative to its controller grip
            const auto& h=f.hand[0];const A::Mat33 F=h.frame,Ft=A::Transpose(F);const float inv=1.f/std::max(f.upm,1e-3f);
            const A::Mat34 E=A::LoadMat34(globals+std::size_t(b.elbow)*16),W=A::LoadMat34(globals+std::size_t(b.wrist)*16);
            forearm::InGrip fg{};
            const A::Vec3 e=Ft*(E.d-h.gripPos)*inv,w=Ft*(W.d-h.gripPos)*inv;
            fg.elbow={e.x,e.y,e.z};fg.wrist={w.x,w.y,w.z};
            if(b.wristroll>=0){
                auto ortho=[](A::Mat33 m){m.a=A::Normalize(m.a,{1,0,0});m.b=A::Normalize(m.b-m.a*A::Dot(m.b,m.a),{0,1,0});m.c=A::Cross(m.a,m.b);return m;};
                const A::Mat33 T=Ft*ortho(A::LoadMat34(globals+std::size_t(b.wristroll)*16).Rot()); // the roll bone's axes in the grip frame
                fg.roll={T.a.x,T.b.x,T.c.x,T.a.y,T.b.y,T.c.y,T.a.z,T.b.z,T.c.z};fg.haveRoll=A::Finite(T.a)&&A::Finite(T.b)&&A::Finite(T.c);
            }
            fg.ns=now;fg.valid=A::Finite(e)&&A::Finite(w);forearm::publish(fg);
        }
        if(out.ik.stretch>0.f){bump(st.stretched);maxOf(st.stretchCmMax,out.ik.stretch*100.f);}
        if(out.clavicleMoved){bump(st.clavicle);maxOf(st.clavicleDegMax,out.clavicleAngle/A::kDegToRad);}
        if(out.fingersPosed)bump(st.fingers);
        st.lastTwistDeg.store(out.twist/A::kDegToRad,std::memory_order_relaxed);
        if(mem.weight>=1.f)maxOf(st.targetErrMmMax,A::Length(out.ik.hand.d-job.target.d)*1000.f/std::max(f.upm,1e-3f)-out.ik.clampedBy*1000.f/std::max(f.upm,1e-3f));
        // The engine attaches a native-held item to its RIGHT attachment bone, so only the right VR hand may use the native hold; a
        // LEFT-hand item gets the IK'd left wrist_l_attachment global (just written) as its attachment in the weapon pose of this scope:
        // it hangs in the left skeletal hand exactly like the right hand's native hold (the VrWeapon HAND placement onto the controller
        // disagreed with the free-hand mapped skeletal hand by the anatomy roll + a few cm).
        if(frameOk&&f.hold==vr&&f.weapon&&!f.barrel&&side==0)res.nativeWeapon=f.weapon; // HAND-held item: native attachment to this (IK'd) hand
        if(frameOk&&write&&side==1&&b.attach>=0&&b.attach<count){
            const std::uintptr_t item=f.hold==vr&&f.weapon?(f.barrel?0:f.weapon):f.phantomHand==vr&&f.phantomWeapon&&!f.phantomBarrel?f.phantomWeapon:0;
            const A::Mat34 at=A::LoadMat34(globals+std::size_t(b.attach)*16);
            if(item&&A::Finite(at)){res.leftWeapon=item;res.leftAttachment=at;bump(st.itemAttach);}
        }
    }
    if(boxAny){box.serial=serial;box.ns=now;box.referenceSpace=f.space;box.upm=f.upm;handBox.store(box);bump(handBoxPublished);}
    if(!write)bump(stats.dryFrames);
    // ---- holster sockets on the shown body ----
    if(c.sockets)publishSockets(r,g.data(),count,f,frameOk&&gateOpen,now,dt,reinterpret_cast<std::uintptr_t>(locals));
    ik.saved.skel=0;
    return res;
}

// Hook B core: drift of the wrists the engine IK caused after hook A's write, re-assert the arm
// subtree (locals + globals from the clavicle's current parent).
inline void afterEngineIk(const Rig& r,float* globals,unsigned char* locals,const Config& c) noexcept {
    seatAfterEngineIk(r,globals,locals,c); // SEAT: the held chain first (the arms re-assert from its shoulders)
    for(int side=0;side<2;++side){
        const auto& s=ik.saved;if(!s.valid[side])continue;
        auto& st=stats.arm[std::size_t(side)];
        const int wrist=r.bones.arm[std::size_t(side)].wrist;
        const A::Mat34 now=A::LoadMat34(globals+std::size_t(wrist)*16),want=s.wrist[std::size_t(side)];
        const float cm=A::Length(now.d-want.d)*100.f;
        const A::Mat33 d=A::Orthonormalize(now.Rot())*A::Transpose(A::Orthonormalize(want.Rot()));
        const float deg=std::acos(std::clamp((d.a.x+d.b.y+d.c.z-1.f)*.5f,-1.f,1.f))/A::kDegToRad;
        st.lastDriftCm.store(cm,std::memory_order_relaxed);
        if(!(cm>.05f||deg>.2f))continue;
        bump(st.driftFrames);if(cm>1.f)bump(st.driftOver1cm);maxOf(st.driftCmMax,cm);maxOf(st.driftDegMax,deg);
        if(!c.reassert||c.dry)continue;
        const int base=s.first[side],n=s.count[side];const int pc=r.parent[std::size_t(base)];if(pc<0)continue;
        const auto& rig=r.arm[std::size_t(side)];
        std::array<A::Mat34,A::kMaxArm> obj{};
        obj[0]=A::LoadMat34(globals+std::size_t(pc)*16)*s.local[std::size_t(side)][0];
        for(int k=1;k<n;++k)obj[std::size_t(k)]=obj[std::size_t(rig.parentRel[k])]*s.local[std::size_t(side)][std::size_t(k)];
        for(int k=0;k<n;++k){A::StoreMat34(globals+std::size_t(base+k)*16,obj[std::size_t(k)]);
            A::StoreMat34(reinterpret_cast<float*>(locals+std::size_t(base+k)*off::localStride),s.local[std::size_t(side)][std::size_t(k)]);}
        bump(st.reasserted);
    }
}

// ============================== hooks ==============================
inline bool hookSkeleton(std::uintptr_t actor,const Published& p,float*& globals,unsigned char*& locals,A::Mat34& root) noexcept {
    using detail::raw;
    const auto ent=raw<std::uintptr_t>(actor+off::actorEntity);if(!ent)return false;
    const auto inst=raw<std::uintptr_t>(ent+off::entityInst);if(!inst)return false;
    const auto cache=raw<std::uintptr_t>(inst+off::instCache);if(!cache)return false;
    const auto skel=raw<std::uintptr_t>(cache+off::cacheSkeleton);
    if(!skel||skel!=p.skel||raw<std::uintptr_t>(skel+off::skelData)!=p.data||raw<std::uint32_t>(skel+off::skelCount)!=p.count)return false;
    const auto g=raw<std::uintptr_t>(skel+off::skelGlobals),l=raw<std::uintptr_t>(skel+off::skelLocals),rt=raw<std::uintptr_t>(skel+off::skelRoot);
    if(!g||!l||!rt)return false;
    globals=reinterpret_cast<float*>(g);locals=reinterpret_cast<unsigned char*>(l);root=A::LoadMat34(reinterpret_cast<const float*>(rt));
    return A::Finite(root);
}
// Hook A body for the player (path 0 post-physics, 1 compose). Sets the hook scope; the caller runs the original and clears it.
// REIN ROPE (VrReinRope.h): after the arm IK of the post-physics hook A (under `busy`), the rein mode follows the VR hands that
// hold the reins (only on a horse with REINS on and the arms following the controllers); anything else puts an overridden mode back.
inline void ropeStep(std::uintptr_t actor,const Published& p,const float* globals,bool mountedArm,float upm,std::uint64_t now) noexcept {
    namespace rr=::rdrvr::vr::reinrope;namespace rn=::rdrvr::vr::reins;
    if(config.rope<=0||!rr::available.load(std::memory_order_acquire))return;
    rr::Input in{};in.read=detail::api.read;in.base=detail::api.base;in.guid=detail::raw<std::uint32_t>(actor+off::actorGuid);in.skel=p.skel;
    in.globals=globals;in.bones=p.count;in.upm=upm;in.now=now;in.freeNone=config.ropeFree;
    // a rein hand counts only while its arm is IK'd to the controller (BODY arms on, rig arm OK, weight > .5 after this
    // frame's applyBody): a wrist left in the native pose (.body.arms 0 / no rig arm / a drop-out past the hold + ramp) is not where the VR
    // hand is - the rope must not go there. Engine side 1 = VR left (bit 0), side 0 = VR right (bit 1).
    const auto driven=[&](int side){return config.arms&&p.rig&&p.rig->armOk[std::size_t(side)]&&ik.arm[std::size_t(side)].weight>.5f;};
    const unsigned drivenMask=(driven(1)?1u:0u)|(driven(0)?2u:0u);
    in.heldRaw=(rn::holds(0u)?1u:0u)|(rn::holds(1u)?2u:0u);in.held=in.heldRaw&drivenMask;
    in.want=mountedArm&&config.reinsArm&&!config.dry&&rn::armsFollow(now)&&drivenMask!=0u;
    rr::rope.step(in);
}
// this frame's LOCAL -> world map (VrMotionBodyLink.h: the one the arms were just placed with) and - with debug.rdrvr.mvbody 1 -
// John's AppSW parts in world: the pose applyBody just wrote (head lock, seat, ride steady, IK'd arms); <10 parts = the map only.
inline void publishMotionParts(const Rig& r,const float* globals,int count,const A::Mat34& root,const Frame& f,std::uint64_t serial,std::uint64_t now) noexcept {
    namespace bl=::rdrvr::vr::bodylink;namespace sw=::rdrvr::runtime::spacewarp;
    if(!f.mapValid){bump(bl::noMap);return;}
    static thread_local bl::Record rec{}; // ~1.4 KB: never on a hook thread's stack
    static thread_local std::array<std::array<float,12>,sw::KeyCount> km{};
    rec.ns=now;rec.serial=serial;rec.third=::rdrvr::vr::steadylink::thirdActive(now);rec.parts.count=0;
    const A::Vec3 rows[4]{f.localToWorld.a,f.localToWorld.b,f.localToWorld.c,f.localToWorld.d};
    for(int k=0;k<4;++k){rec.map[k*3]=rows[k].x;rec.map[k*3+1]=rows[k].y;rec.map[k*3+2]=rows[k].z;}
    if(bl::bodyProp.load(std::memory_order_relaxed)){
        auto& b=rec.parts;
        if(r.mvKeys.humanoid()){
            std::array<const float*,sw::KeyCount> mp{};std::array<std::int16_t,sw::KeyCount> bi{};bi.fill(-1);
            for(unsigned k=0;k<sw::KeyCount;++k){const int bone=r.mvKeys.index[k];if(bone<0||bone>=count)continue;const float* g=globals+std::size_t(bone)*16;
                for(int row=0;row<4;++row)for(int col=0;col<3;++col)km[k][std::size_t(row*3+col)]=g[row*4+col]; // w lanes are padding
                mp[k]=km[k].data();bi[k]=std::int16_t(bone);}
            const float rootPos[3]{root.d.x,root.d.y,root.d.z};
            sw::makeParts(mp,bi,rootPos,f.upm,b);
            unsigned w=0; // first person: the head is collapsed around the eyes (no head pixels; its capsule would hold the camera)
            for(unsigned k=0;k<b.count;++k){if(b.part[k].spec==0&&!rec.third)continue;if(w!=k)b.part[w]=b.part[k];++w;}
            b.count=w>=10?w:0u;b.guid=sw::playerBodyGuid;b.layout=sw::playerBodyLayout;}
        if(!b.count)bump(bl::noParts);
        bl::lastParts.store(b.count,std::memory_order_relaxed);}
    bl::publish(rec);
}
inline void beforeAttachments(std::uintptr_t actor,bool postPhys=true) noexcept {
    const auto t0=detail::now();
    bump(stats.playerCalls);noteThread();
    Published p{};
    if(!published.load(p)||p.actor!=actor||!p.rig||p.rig->generation!=p.generation||p.rig->data!=p.data)return;
    const bool bodyOn=::rdrvr::vr::settings().body()||ik.arm[0].weight>0.f||ik.arm[1].weight>0.f||ik.yawWeight>0.f||ik.acc!=0.f||
        ::rdrvr::vr::seat::state.weight>0.f; // SEAT: BODY off ramps the seat hold out too
    if(!bodyOn&&!(postPhys&&::rdrvr::vr::reinrope::overriding()))return; // BODY off still puts an overridden rope mode back
    if(busy.exchange(true,std::memory_order_acquire)){bump(stats.busy);return;}
    struct Release{~Release(){busy.store(false,std::memory_order_release);}} release;
    float* globals{};unsigned char* locals{};A::Mat34 root{};
    if(!hookSkeleton(actor,p,globals,locals,root)){bump(stats.skelMismatch);return;}
    if(!bodyOn){ropeStep(actor,p,globals,false,1.f,t0);return;}
    bool gateOpen=p.armsWanted&&(t0<p.ns||t0-p.ns<300'000'000ull); // a gate older than 300 ms counts as closed
    if(gateOpen&&(detail::raw<std::uint8_t>(actor+off::actorRagdoll)&3u)){gateOpen=false;bump(stats.ragdollByte);}
    const std::uint64_t serial=serialCounter.fetch_add(1,std::memory_order_relaxed);
    scope.serial=serial;scope.nativeWeapon=0;scope.leftWeapon=0;
    Frame f{};bool frameOk=false;
    const bool fresh=t0<p.ns||t0-p.ns<300'000'000ull,ragdoll=(detail::raw<std::uint8_t>(actor+off::actorRagdoll)&3u)!=0;
    const bool mountedArm=!gateOpen&&p.mountedArm&&fresh&&!ragdoll;
    // SEAT: 2 = hold (horse, after the mount animation), 1 = ramp out (dismount / gate change / BODY off), 0 = cut at once (stale gate,
    // ragdoll byte, cutscene, dead, no skeleton: the native pose, nothing written)
    const int seatMode=!fresh||ragdoll||p.seatCut?0:p.seat&&::rdrvr::vr::settings().body()?2:1;
    const bool seatRun=seatMode==2||(seatMode==1&&::rdrvr::vr::seat::state.weight>0.f);
    if(gateOpen||mountedArm||seatRun){const auto fn=frameFn.load(std::memory_order_acquire);
        try{frameOk=fn&&fn(f,actor,serial,t0)&&f.valid;}catch(...){frameOk=false;}
        if(frameOk)bump(stats.frameOk);else{bump(stats.frameFail);stats.frameWhy.store(f.why?f.why:"",std::memory_order_relaxed);}}
    { // The head lock only under the BODY camera (HEAD camera = the head bone itself: a lock would chase its own view)
        using namespace ::rdrvr::vr;const auto s=settings();
        f.cameraBody=s.bodyCamera()&&enabled(Feature::Camera);f.eyeUp=s.bodyEyeUpMeters();f.eyeBack=s.bodyBackMeters();}
    const auto res=applyBody(*p.rig,globals,locals,root,int(p.count),f,frameOk,gateOpen,t0,serial,config,mountedArm,seatMode);
    scope.nativeWeapon=res.nativeWeapon;scope.leftWeapon=res.leftWeapon;scope.leftAttachment=res.leftAttachment;
    if(frameOk&&::rdrvr::vr::bodylink::wanted())publishMotionParts(*p.rig,globals,int(p.count),root,f,serial,t0); // AppSW
    ik.saved.skel=p.skel;
    // the rope after this frame's IK (its wrists = the next check); only the post-physics path (the compose callback runs elsewhere)
    if(postPhys)ropeStep(actor,p,globals,mountedArm,frameOk?f.upm:1.f,t0);
    detail::addTime(stats.aNsSum,stats.aCount,stats.aUsMax,detail::now()-t0);
}
RDRVR_BODY_NOINLINE inline std::uint64_t attachHook(void* actor,const void* message) noexcept {
    const auto original=detail::attachOriginal;
    const auto a=reinterpret_cast<std::uintptr_t>(actor);
    if(a&&a==steady::mountActor.load(std::memory_order_relaxed)&&a!=hookActor.load(std::memory_order_relaxed)){ // RIDE STEADY: the horse
        const auto c=RDRVR_BODY_CALLER();const auto base=detail::api.base;
        if(c==base+attachReturnPostPhysOffset||c==base+attachReturnComposeOffset){try{steady::horseStep(a,detail::now());}catch(...){bump(stats.exceptions);}}
        return original(actor,message);}
    if(!a||a!=hookActor.load(std::memory_order_relaxed))return original(actor,message);
    const auto caller=RDRVR_BODY_CALLER();const auto base=detail::api.base;
    if(caller==base+attachReturnPostPhysOffset)bump(stats.callsPostPhys);
    else if(caller==base+attachReturnComposeOffset)bump(stats.callsCompose);
    else{bump(stats.callsOtherLR);return original(actor,message);}
    try{beforeAttachments(a,caller==base+attachReturnPostPhysOffset);}catch(...){bump(stats.exceptions);}
    const auto result=original(actor,message);
    lastAttachSerial=scope.serial;scope=Scope{};
    return result;
}
RDRVR_BODY_NOINLINE inline std::uint64_t ikHook(void* actor,const void* message) noexcept {
    const auto original=detail::ikOriginal;
    const auto a=reinterpret_cast<std::uintptr_t>(actor);
    const auto result=original(actor,message);
    if(!a||a!=hookActor.load(std::memory_order_relaxed))return result;
    const auto caller=RDRVR_BODY_CALLER();const auto base=detail::api.base;
    if(caller!=base+ikReturnPostPhysOffset&&caller!=base+ikReturnComposeOffset){bump(stats.ikOtherLR);return result;}
    bump(stats.ikCalls);
    const auto serial=lastAttachSerial;lastAttachSerial=0;
    if(!serial||ik.saved.serial!=serial||(!ik.saved.valid[0]&&!ik.saved.valid[1]&&!ik.saved.seat.valid))return result;
    const auto t0=detail::now();
    if(busy.exchange(true,std::memory_order_acquire)){bump(stats.busy);return result;}
    struct Release{~Release(){busy.store(false,std::memory_order_release);}} release;
    try{
        Published p{};float* globals{};unsigned char* locals{};A::Mat34 root{};
        if(published.load(p)&&p.rig&&p.rig->generation==p.generation&&ik.saved.skel==p.skel&&hookSkeleton(a,p,globals,locals,root))
            afterEngineIk(*p.rig,globals,locals,config);
    }catch(...){bump(stats.exceptions);}
    detail::addTime(stats.bNsSum,stats.bCount,stats.bUsMax,detail::now()-t0);
    return result;
}
// Hook C core: collapse [first, end) about the pivot joint, run `call`, restore the exact bytes.
template<class Call>
inline bool collapseAround(float* globals,int first,int end,int pivot,float eps,Call&& call) noexcept {
    const int n=end-first;
    if(n<=0||n>maxCollapse||pivot<first||pivot>=end){call();return false;}
    alignas(16) static thread_local float saved[maxCollapse][16];
    std::memcpy(saved,globals+std::size_t(first)*16,sizeof(float)*16*std::size_t(n));
    const A::Vec3 p=A::LoadMat34(globals+std::size_t(pivot)*16).d;
    for(int i=0;i<n;++i){float* m=globals+std::size_t(first+i)*16;A::StoreMat34(m,A::CollapseAbout(A::LoadMat34(m),p,eps));}
    call();
    std::memcpy(globals+std::size_t(first)*16,saved,sizeof(float)*16*std::size_t(n));
    return true;
}
inline std::atomic<bool> collapseLock{};
// ============================== HEAD SHADOW ==============================
// Hook C runs the original grmMatrixSet::Update TWICE for the player: collapsed (the head rows are captured: 12 floats per bone, the set's
// own skin-row format) and then normally, so the set the engine keeps is UNCOLLAPSED (sun / character shadow, cube shadows, water
// reflection: the head is back). The eye scopes (BodyDrawFilter, render thread) bind a copy of the set with the captured head rows
// (headFill below). Layout facts (librdr 1.58): u8 bone count at set+0, rows at
// set+0xdc (current n x 48 B, then previous n x 48 B), current d11::ConstantBuffer_c at set+0x68 with its data pointer == set+0xdc,
// previous at set+0xa0 with data == set+0xdc+48n. Every one of these is checked on every use (hook C and the render thread), any mismatch
// = collapse only (in the skinning task, kind 2 slot -> the eye draw takes the set as is).
// Fallback: a layout mismatch in hook C, or eye misses (no slot for the bound set) > 30 and > 10 % of the hits -> headShadowLive false
// (collapsed in the skinning task: no head in the shadow, the hat hidden there too).
inline constexpr std::size_t setCount=0x0,setCbCurrent=0x68,setCbPrevious=0xa0,setRowsOffset=0xdc,setRowBytes=48;
struct HeadSlot {std::atomic<std::uint64_t> seq{};std::uintptr_t set{},prevSet{};std::uint32_t kind{},n{},first{},end{};float rows[maxCollapse][12]{};};
inline std::array<HeadSlot,8> headSlots{};inline std::atomic<unsigned> headSlotNext{};
inline Counter headCaptures{},headCollapsedSets{},headLayoutFails{},headFillHits{},headFillMisses{},headFillInvalid{},headFillAsIs{},headPrevFromSlot{};
// True when `set` looks like a grmMatrixSet of n bones (count byte + both constant buffers pointing at its own rows).
inline bool setLayoutOk(std::uintptr_t set,std::uint32_t n) noexcept {
    using detail::raw;if(!set||!n||n>255)return false;
    return raw<std::uint8_t>(set+setCount)==n&&raw<std::uintptr_t>(set+setCbCurrent)==set+setRowsOffset&&
           raw<std::uintptr_t>(set+setCbPrevious)==set+setRowsOffset+std::size_t(n)*setRowBytes;
}
// Single writer (hook C under collapseLock).
inline void publishHeadSlot(std::uintptr_t set,std::uintptr_t prevSet,std::uint32_t kind,std::uint32_t n,int first,int end,const float* rows) noexcept {
    auto& s=headSlots[headSlotNext.fetch_add(1,std::memory_order_relaxed)%headSlots.size()];
    const auto q=s.seq.load(std::memory_order_relaxed);s.seq.store(q+1,std::memory_order_relaxed);std::atomic_thread_fence(std::memory_order_release);
    s.set=set;s.prevSet=prevSet;s.kind=kind;s.n=n;s.first=std::uint32_t(first);s.end=std::uint32_t(end);
    if(rows&&end>first)std::memcpy(s.rows,rows,std::size_t(end-first)*setRowBytes);
    s.seq.store(q+2,std::memory_order_release);
}
struct HeadView {std::uintptr_t prevSet{};std::uint32_t kind{},n{},first{},end{};float rows[maxCollapse][12];};
// Any thread: the newest consistent slot of `set`.
inline bool findHeadSlot(std::uintptr_t set,HeadView& out) noexcept {
    if(!set)return false;
    const unsigned next=headSlotNext.load(std::memory_order_acquire);
    for(unsigned k=0;k<headSlots.size();++k){auto& s=headSlots[(next+headSlots.size()-1-k)%headSlots.size()];
        for(int attempt=0;attempt<3;++attempt){
            const auto q0=s.seq.load(std::memory_order_acquire);if(q0&1u)continue;
            if(s.set!=set)break;
            out.prevSet=s.prevSet;out.kind=s.kind;out.n=s.n;out.first=s.first;out.end=s.end;
            if(out.end>out.first&&out.end-out.first<=std::uint32_t(maxCollapse))std::memcpy(out.rows,s.rows,std::size_t(out.end-out.first)*setRowBytes);
            std::atomic_thread_fence(std::memory_order_acquire);
            if(s.seq.load(std::memory_order_relaxed)==q0)return out.kind==2||(out.kind==1&&out.end>out.first&&out.end-out.first<=std::uint32_t(maxCollapse)&&out.end<=out.n);
        }
    }
    return false;
}
// Render thread (BodyDrawFilter eye scope, the player's record): the set's rows with the head collapsed into `scratch`.
inline std::int32_t headFill(std::uintptr_t set,float* scratch,std::size_t capacityFloats) noexcept {
    try{
        HeadView v{};
        if(!findHeadSlot(set,v)){bump(headFillMisses);
            const auto m=headFillMisses.load(std::memory_order_relaxed),h=headFillHits.load(std::memory_order_relaxed);
            if(m>30&&m*10>h&&headShadowLive.exchange(false,std::memory_order_relaxed))headShadowState.store("FALLBACK_MISSES",std::memory_order_relaxed);
            return 0;}
        if(v.kind==2){bump(headFillAsIs);return -1;}
        const std::uint32_t n=v.n;
        if(std::size_t(n)*2*12>capacityFloats||!setLayoutOk(set,n)){bump(headFillInvalid);return 0;}
        std::memcpy(scratch,reinterpret_cast<const void*>(set+setRowsOffset),std::size_t(n)*2*setRowBytes);
        const std::uint32_t k=v.end-v.first;
        std::memcpy(scratch+std::size_t(v.first)*12,v.rows,std::size_t(k)*setRowBytes);
        // previous rows (motion vectors / TAA): the previous set's captured head, else this frame's
        HeadView pv{};const bool prev=findHeadSlot(v.prevSet,pv)&&pv.kind==1&&pv.first==v.first&&pv.end==v.end&&pv.n==n;
        if(prev)bump(headPrevFromSlot);
        std::memcpy(scratch+(std::size_t(n)+v.first)*12,prev?pv.rows:v.rows,std::size_t(k)*setRowBytes);
        bump(headFillHits);return std::int32_t(n);
    }catch(...){bump(headFillInvalid);return 0;}
}
RDRVR_BODY_NOINLINE inline std::uint64_t matrixSetHook(void* set,const void* previous,const void* skeleton,bool a,bool b) noexcept {
    const auto original=detail::matrixOriginal;
    const auto s=reinterpret_cast<std::uintptr_t>(skeleton);
    if(!s||s!=collapseSkeleton.load(std::memory_order_relaxed))return original(set,previous,skeleton,a,b);
    const auto caller=RDRVR_BODY_CALLER();
    if(caller!=detail::api.base+matrixSetTaskReturnOffset){bump(stats.headOtherLR);return original(set,previous,skeleton,a,b);}
    const auto t0=detail::now();bump(stats.headCalls);
    bool locked=false; // bounded: only two skin tasks of the PLAYER skeleton can meet here
    for(int spin=0;spin<200;++spin){if(!collapseLock.exchange(true,std::memory_order_acquire)){locked=true;break;}std::this_thread::yield();}
    if(!locked){bump(stats.headContended);return original(set,previous,skeleton,a,b);}
    struct Release{~Release(){collapseLock.store(false,std::memory_order_release);}} release;
    Published p{};bool ok=published.load(p)&&p.collapse&&p.skel==s&&p.rig&&p.rig->generation==p.generation;
    float* globals{};
    if(ok){ok=detail::raw<std::uintptr_t>(s+off::skelData)==p.data&&detail::raw<std::uint32_t>(s+off::skelCount)==p.count;
        if(ok)globals=reinterpret_cast<float*>(detail::raw<std::uintptr_t>(s+off::skelGlobals));ok=ok&&globals;}
    if(!ok){bump(stats.headSkipped);return original(set,previous,skeleton,a,b);}
    const auto& r=*p.rig;
    std::uint64_t result{};
    const auto st=reinterpret_cast<std::uintptr_t>(set),pr=reinterpret_cast<std::uintptr_t>(previous);
    const int hf=r.collapseFirst,he=r.collapseEnd;
    if(headShadowLive.load(std::memory_order_relaxed)&&headFillInstalled.load(std::memory_order_acquire)){ // HEAD SHADOW: capture the collapsed head rows, keep the set uncollapsed
        if(hf>=0&&he>hf&&he-hf<=maxCollapse&&std::uint32_t(he)<=p.count&&setLayoutOk(st,p.count)){
            if(collapseAround(globals,hf,he,r.collapsePivot,config.collapseEps,[&]{result=original(set,previous,skeleton,a,b);})){
                alignas(16) static thread_local float rows[maxCollapse][12];
                std::memcpy(rows,reinterpret_cast<const void*>(st+setRowsOffset+std::size_t(hf)*setRowBytes),std::size_t(he-hf)*setRowBytes);
                result=original(set,previous,skeleton,a,b); // the set the engine keeps: uncollapsed (identical inputs: deterministic)
                publishHeadSlot(st,pr,1,p.count,hf,he,&rows[0][0]);bump(headCaptures);bump(stats.headApplied);
                detail::addTime(stats.cNsSum,stats.cCount,stats.cUsMax,detail::now()-t0);
                return result;
            }
        }else if(headShadowLive.exchange(false,std::memory_order_relaxed)){bump(headLayoutFails);headShadowState.store("FALLBACK_LAYOUT",std::memory_order_relaxed);}
    }
    if(collapseAround(globals,r.collapseFirst,r.collapseEnd,r.collapsePivot,config.collapseEps,[&]{result=original(set,previous,skeleton,a,b);})){
        bump(stats.headApplied);
        if(config.headShadow){publishHeadSlot(st,pr,2,p.count,0,0,nullptr);bump(headCollapsedSets);} // kind 2: the eye draws take it as is
    }
    detail::addTime(stats.cNsSum,stats.cCount,stats.cUsMax,detail::now()-t0);
    return result;
}

// ============================== install (TraceEntry; once) ==============================
// code(offset, print, size) = executable bytes match; got(slot, target) = *(base+slot) == base+target; replace(address, expected, new).
template<class Code,class Got,class Replace>
inline bool install(EngineApi api,Code&& code,Got&& got,Replace&& replace) noexcept {
    using namespace ::rdrvr::vr;
    startup();
    if(killed(Feature::Body)){installState.store("KILLED");status::publish(Feature::Body,status::Property);return false;}
    const auto fail=[&](const char* why){installState.store(why);installed.store(false,std::memory_order_release);
        status::publish(Feature::Body,status::Unavailable);return false;};
    if(!api.complete())return fail("NO ENGINE API");
    const auto b=api.base;
    struct Print {std::uintptr_t offset;const unsigned char* bytes;std::size_t size;const char* name;};
#define RDRVR_BODY_PRINT(o,p,n) Print{o,p.data(),p.size(),n}
    const Print prints[]{RDRVR_BODY_PRINT(broadcastAttachOffset,broadcastAttachFingerprint,"PRINT Broadcast(UpdateAttachments)"),
        RDRVR_BODY_PRINT(broadcastIkOffset,broadcastIkFingerprint,"PRINT Broadcast(UpdateIK)"),
        RDRVR_BODY_PRINT(updateAttachmentsAndIkOffset,updateAttachmentsAndIkFingerprint,"PRINT UpdateAttachmentsAndIK"),
        RDRVR_BODY_PRINT(composeAttachSiteOffset,composeAttachSiteFingerprint,"PRINT ComposeCallback site"),
        RDRVR_BODY_PRINT(updateActorSkelSiteOffset,updateActorSkelSiteFingerprint,"PRINT UpdateActor site"),
        RDRVR_BODY_PRINT(skeletonPartialUpdateOffset,skeletonPartialUpdateFingerprint,"PRINT crSkeleton::PartialUpdate"),
        RDRVR_BODY_PRINT(skeletonUpdateOffset,skeletonUpdateFingerprint,"PRINT crSkeleton::Update"),
        RDRVR_BODY_PRINT(matrixSetUpdateOffset,matrixSetUpdateFingerprint,"PRINT grmMatrixSet::Update"),
        RDRVR_BODY_PRINT(matrixSetSiteOffset,matrixSetSiteFingerprint,"PRINT MatrixSetUpdate site")};
#undef RDRVR_BODY_PRINT
    for(const auto& pr:prints){bool ok=false;try{ok=code(pr.offset,pr.bytes,pr.size);}catch(...){ok=false;}if(!ok)return fail(pr.name);}
    struct Word {std::uintptr_t slot,target;const char* name;};
    const Word words[]{{broadcastAttachGotOffset,broadcastAttachOffset,"GOT Broadcast(UpdateAttachments)"},
        {broadcastIkGotOffset,broadcastIkOffset,"GOT Broadcast(UpdateIK)"},{matrixSetUpdateGotOffset,matrixSetUpdateOffset,"GOT grmMatrixSet::Update"}};
    for(const auto& wd:words){bool ok=false;try{ok=got(wd.slot,wd.target);}catch(...){ok=false;}if(!ok)return fail(wd.name);}
    detail::api=api;
    detail::attachOriginal=reinterpret_cast<BroadcastFn>(b+broadcastAttachOffset);detail::ikOriginal=reinterpret_cast<BroadcastFn>(b+broadcastIkOffset);
    detail::matrixOriginal=reinterpret_cast<MatrixSetUpdateFn>(b+matrixSetUpdateOffset);
    if(!mt::registerModule(mt::ModuleBody,"body",tickThunk,nullptr))return fail("REGISTER FAILED");
    // All three or none: a later failure puts the earlier slots back.
    const auto put=[&](std::uintptr_t slot,std::uintptr_t target,std::uintptr_t hook){bool ok=false;try{ok=replace(b+slot,b+target,hook);}catch(...){}return ok;};
    const auto back=[&](std::uintptr_t slot,std::uintptr_t target,std::uintptr_t hook){try{replace(b+slot,hook,b+target);}catch(...){}};
    const auto hA=reinterpret_cast<std::uintptr_t>(&attachHook),hB=reinterpret_cast<std::uintptr_t>(&ikHook),hC=reinterpret_cast<std::uintptr_t>(&matrixSetHook);
    if(!put(matrixSetUpdateGotOffset,matrixSetUpdateOffset,hC))return fail("HOOK C NOT INSTALLED");
    if(!put(broadcastIkGotOffset,broadcastIkOffset,hB)){back(matrixSetUpdateGotOffset,matrixSetUpdateOffset,hC);return fail("HOOK B NOT INSTALLED");}
    if(!put(broadcastAttachGotOffset,broadcastAttachOffset,hA)){back(broadcastIkGotOffset,broadcastIkOffset,hB);
        back(matrixSetUpdateGotOffset,matrixSetUpdateOffset,hC);return fail("HOOK A NOT INSTALLED");}
    // REIN ROPE: variant-1 words (sm_Instance GLOB_DAT, the sagVerletRein vtable typeinfo + two slots); a mismatch (variant 2 or
    // any other engine) = UNAVAILABLE: the rope is never read or written, the arms work as without it
    ::rdrvr::vr::reinrope::verifyEngine(config.rope,[&](std::uintptr_t slot,std::uintptr_t target){bool ok=false;try{ok=got(slot,target);}catch(...){ok=false;}return ok;});
    installed.store(true,std::memory_order_release);
    installState.store(config.dry?"INSTALLED DRY":"INSTALLED");
    publishStatus(GateOk);
    return true;
}

// ============================== report "vr":{"body":...} (any thread) ==============================
inline std::string armJson(const Rig* r,int side) {
    std::string out;char b[1024];
    if(!r){out="null";return out;}
    const auto& a=r->bones.arm[std::size_t(side)];const auto& rig=r->arm[std::size_t(side)];
    std::snprintf(b,sizeof b,"{\"why\":\"%s\",\"ok\":%s,\"clavicle\":%d,\"arm\":%d,\"armroll\":%d,\"elbow\":%d,\"wristroll\":%d,\"wrist\":%d,\"attach\":%d,"
        "\"subtreeEnd\":%d,\"bones\":%d,\"rollInChain\":%s,\"clavicleParent\":%s,\"upperCm\":%.1f,\"foreCm\":%.1f,\"fingerChains\":%d,\"fingersValid\":%s,"
        "\"hand\":{\"valid\":%s,\"knuckle\":[%.2f,%.2f,%.2f],\"palmOut\":[%.2f,%.2f,%.2f],\"thumbSide\":%.2f}}",a.why,r->armOk[std::size_t(side)]?"true":"false",
        a.clavicle,a.arm,a.armroll,a.elbow,a.wristroll,a.wrist,a.attach,a.end,rig.count,rig.rollRel>0?"true":"false",rig.clavicleIsParent?"true":"false",
        double(r->upperCm[std::size_t(side)]),double(r->foreCm[std::size_t(side)]),r->fingerChains[std::size_t(side)],rig.fingersValid?"true":"false",
        rig.hand.valid?"true":"false",double(rig.hand.knuckle.x),double(rig.hand.knuckle.y),double(rig.hand.knuckle.z),double(rig.hand.palmOut.x),
        double(rig.hand.palmOut.y),double(rig.hand.palmOut.z),double(rig.hand.thumbSide));
    out=b;return out;
}
inline std::string report() {
    using namespace ::rdrvr::vr;
    std::string out;out.reserve(6144);char b[2048];
    const auto& c=config;Published p{};const bool havePub=published.load(p);const Rig* r=havePub?p.rig:nullptr;
    const unsigned g=gateNow.load();
    std::snprintf(b,sizeof b,"{\"enabled\":%s,\"menu\":\"%s\",\"installed\":%s,\"installState\":\"%s\",\"visible\":%s,\"hidesHands\":%s,"
        "\"hooks\":{\"attach\":\"GOT 0x2e33718 pre\",\"ik\":\"GOT 0x2e33510 post\",\"matrixSet\":\"GOT 0x2e22ad0 task\"},"
        "\"config\":{\"head\":%s,\"headRoot\":\"%s\",\"yaw\":%s,\"arms\":%s,\"fingers\":%s,\"reassert\":%s,\"clavicle\":%s,\"derive\":%s,\"dry\":%s,"
        "\"stretchCm\":%.1f,\"yawTauMs\":%.0f,\"yawMaxDeg\":%.0f,\"wristCm\":[%.1f,%.1f],\"elbow\":[%.2f,%.2f,%.2f,%.2f],\"roll\":%.2f,\"clavicleDeg\":%.1f},",
        killed(Feature::Body)?"false":"true",settings().body()?"ON":"OFF",installed.load()?"true":"false",installState.load(),bodyVisible()?"true":"false",
        status::bodyHidesHands.load()?"true":"false",c.head?"true":"false",c.headRoot==HeadRoot::Neck?"NECK":"HEAD",c.yaw?"true":"false",c.arms?"true":"false",
        c.fingers?"true":"false",c.reassert?"true":"false",c.clavicle?"true":"false",c.derive?"true":"false",c.dry?"true":"false",double(c.stretchCm),
        double(c.yawTauMs),double(c.yawMaxDeg),double(c.wristBack*100.f),double(c.wristDepth*100.f),double(c.elbowOut),double(c.elbowDown),double(c.elbowBack),
        double(c.elbowWrist),double(c.rollShare),double(c.clavicleDeg));out+=b;
    std::snprintf(b,sizeof b,"\"gate\":{\"state\":\"%s\",\"published\":%s,\"ageMs\":%.1f,\"byGate\":{",gateNames[g<GateCount?g:0],havePub?"true":"false",
        havePub&&p.ns?double(::rdrvr::vr::nowNs()-p.ns)/1e6:-1.0);out+=b;
    {bool first=true;for(unsigned i=0;i<GateCount;++i){const auto n=stats.gates[i].load();if(!n)continue;
        std::snprintf(b,sizeof b,"%s\"%s\":%llu",first?"":",",gateNames[i],(unsigned long long)n);out+=b;first=false;}}
    std::snprintf(b,sizeof b,"}},\"bones\":{\"state\":\"%s\",\"resolves\":%llu,\"failures\":%llu,\"count\":%d,\"depthFirst\":%s,\"indexOrder\":%s,\"head\":%d,\"neck\":%d,"
        "\"headEnd\":%d,\"neckEnd\":%d,\"firstBad\":[%d,%d],\"collapse\":[%d,%d,%d],\"right\":",r?r->state:"NONE",(unsigned long long)stats.rigResolves.load(),
        (unsigned long long)stats.rigFailures.load(),r?r->count:0,r&&r->bones.depthFirst?"true":"false",r&&r->bones.indexOrder?"true":"false",
        r?r->bones.head:-1,r?r->bones.neck:-1,r?r->bones.headEnd:-1,r?r->bones.neckEnd:-1,r?r->bones.firstBad:-1,r?r->bones.firstBadParent:-2,r?r->collapseFirst:-1,r?r->collapseEnd:-1,r?r->collapsePivot:-1);out+=b;
    out+=armJson(r,0);out+=",\"left\":";out+=armJson(r,1);out+="},";
    std::snprintf(b,sizeof b,"\"path\":{\"postPhys\":%llu,\"compose\":%llu,\"otherLR\":%llu,\"playerCalls\":%llu,\"busy\":%llu,\"skelMismatch\":%llu,"
        "\"ragdollByte\":%llu,\"frameOk\":%llu,\"frameFail\":%llu,\"frameWhy\":\"%s\",\"dryFrames\":%llu,\"mountedArmFrames\":%llu,\"threads\":[",(unsigned long long)stats.callsPostPhys.load(),
        (unsigned long long)stats.callsCompose.load(),(unsigned long long)stats.callsOtherLR.load(),(unsigned long long)stats.playerCalls.load(),
        (unsigned long long)stats.busy.load(),(unsigned long long)stats.skelMismatch.load(),(unsigned long long)stats.ragdollByte.load(),
        (unsigned long long)stats.frameOk.load(),(unsigned long long)stats.frameFail.load(),stats.frameWhy.load(),(unsigned long long)stats.dryFrames.load(),(unsigned long long)stats.mountedArmFrames.load());out+=b;
    {bool first=true;for(const auto& t:stats.threads){const auto v=t.load();if(!v)continue;std::snprintf(b,sizeof b,"%s%llu",first?"":",",(unsigned long long)v);out+=b;first=false;}}
    out+="]},\"arms\":[";
    for(int s=0;s<2;++s){const auto& a=stats.arm[std::size_t(s)];
        std::snprintf(b,sizeof b,"%s{\"side\":\"%s\",\"weight\":%.2f,\"applied\":%llu,\"targets\":{\"derived\":%llu,\"support\":%llu,\"free\":%llu,\"nativeHold\":%llu,"
            "\"none\":%llu},\"clamped\":%llu,\"clampCmMax\":%.1f,\"lastClampCm\":%.1f,\"stretched\":%llu,\"stretchCmMax\":%.1f,\"clavicle\":%llu,\"clavicleDegMax\":%.1f,"
            "\"targetErrMmMax\":%.2f,\"twistDeg\":%.1f,\"fingers\":%llu,\"poseFailed\":%llu,\"drift\":{\"frames\":%llu,\"over1cm\":%llu,\"cmMax\":%.2f,\"degMax\":%.1f,"
            "\"lastCm\":%.2f,\"reasserted\":%llu},\"hold\":{\"mirror\":%llu,\"grip\":%llu,\"itemAttach\":%llu},"
            "\"mounted\":{\"none\":%llu,\"hold\":%llu,\"holdExpired\":%llu,\"blendOut\":%llu,\"fades\":%llu,\"fadeFrames\":%llu}}",s?",":"",s?"LEFT":"RIGHT",
            double(a.weight.load()),(unsigned long long)a.applied.load(),
            (unsigned long long)a.derived.load(),(unsigned long long)a.support.load(),(unsigned long long)a.free.load(),(unsigned long long)a.nativeHold.load(),
            (unsigned long long)a.noTarget.load(),(unsigned long long)a.clamped.load(),double(a.clampCmMax.load()),double(a.lastClampCm.load()),
            (unsigned long long)a.stretched.load(),double(a.stretchCmMax.load()),(unsigned long long)a.clavicle.load(),double(a.clavicleDegMax.load()),
            double(a.targetErrMmMax.load()),double(a.lastTwistDeg.load()),(unsigned long long)a.fingers.load(),(unsigned long long)a.poseFailed.load(),
            (unsigned long long)a.driftFrames.load(),(unsigned long long)a.driftOver1cm.load(),double(a.driftCmMax.load()),double(a.driftDegMax.load()),
            double(a.lastDriftCm.load()),(unsigned long long)a.reasserted.load(),(unsigned long long)a.holdMirror.load(),
            (unsigned long long)a.holdGrip.load(),(unsigned long long)a.itemAttach.load(),(unsigned long long)a.mountNone.load(),
            (unsigned long long)a.mountHold.load(),(unsigned long long)a.mountHoldExpired.load(),(unsigned long long)a.mountBlendOut.load(),
            (unsigned long long)a.mountFades.load(),(unsigned long long)a.mountFadeFrames.load());out+=b;}
    std::snprintf(b,sizeof b,"],\"weapon\":{\"derivedSideMismatch\":%llu,\"fingerMirror\":%s,\"leftMirror\":%s,\"support\":[%.0f,%.0f,%.1f]},"
        "\"engineIk\":{\"calls\":%llu,\"otherLR\":%llu},"
        "\"yaw\":{\"applied\":%llu,\"undo\":%llu,\"sticky\":%llu,\"weight\":%.2f,\"lastDeg\":%.1f,\"maxDeg\":%.1f},"
        "\"head\":{\"calls\":%llu,\"applied\":%llu,\"otherLR\":%llu,\"contended\":%llu,\"skipped\":%llu,\"collapsing\":%s},"
        "\"eyeVsHead\":{\"samples\":%llu,\"cm\":%.1f,\"cmMax\":%.1f,\"fwdCm\":%.1f,\"upCm\":%.1f,\"rightCm\":%.1f},",
        (unsigned long long)stats.weaponDerivedMismatch.load(),r&&r->fingerMirror?"true":"false",c.leftMirror?"true":"false",
        double(c.supportTiltDeg),double(c.supportYawDeg),double(c.supportDropM*100.f),
        (unsigned long long)stats.ikCalls.load(),(unsigned long long)stats.ikOtherLR.load(),
        (unsigned long long)stats.yawApplied.load(),(unsigned long long)stats.yawUndo.load(),(unsigned long long)stats.yawSticky.load(),double(stats.yawWeight.load()),
        double(stats.yawLastDeg.load()),double(stats.yawMaxDeg.load()),(unsigned long long)stats.headCalls.load(),(unsigned long long)stats.headApplied.load(),
        (unsigned long long)stats.headOtherLR.load(),(unsigned long long)stats.headContended.load(),(unsigned long long)stats.headSkipped.load(),
        collapseSkeleton.load()?"true":"false",(unsigned long long)stats.eyeSamples.load(),double(stats.eyeCm.load()),double(stats.eyeCmMax.load()),
        double(stats.eyeFwdCm.load()),double(stats.eyeUpCm.load()),double(stats.eyeRightCm.load()));out+=b;
    { // Placement of the body under the eyes, its oscillation, holster sockets
        std::snprintf(b,sizeof b,"\"placement\":{\"shoulderBelowEyeCm\":%.1f,\"shoulderBehindEyeCm\":%.1f,\"neckBelowEyeCm\":%.1f,\"headAboveRootCm\":%.1f,"
            "\"eyeUpCm\":%.0f,\"eyeBackCm\":%.0f,\"headMm\":%d,\"collapsing\":%s},\"lock\":{\"enabled\":%s,\"weight\":%.2f,\"applied\":%llu,\"clamped\":%llu,"
            "\"noPivot\":%llu,\"lastCm\":%.1f,\"cmMax\":%.1f,\"maxCm\":%.0f},\"yawPitchFrozen\":%llu,\"sway\":{",
            double(stats.shoulderBelowEyeCm.load()),double(stats.shoulderBehindEyeCm.load()),double(stats.neckBelowEyeCm.load()),
            double(stats.headAboveRootCm.load()),double(settings().bodyEyeUpMeters()*100.f),double(settings().bodyBackMeters()*100.f),
            status::bodyHeadMm.load(),status::bodyCollapsing.load()?"true":"false",c.lock?"true":"false",double(stats.lockWeight.load()),
            (unsigned long long)stats.lockApplied.load(),(unsigned long long)stats.lockClamped.load(),(unsigned long long)stats.lockNoPivot.load(),
            double(stats.lockLastCm.load()),double(stats.lockCmMax.load()),double(c.lockMaxCm),(unsigned long long)stats.yawPitchFrozen.load());out+=b;
        for(unsigned i=0;i<OscCount;++i){const auto& o=stats.osc[i];
            std::snprintf(b,sizeof b,"%s\"%s\":{\"ptpCm\":%.2f,\"hz\":%.2f,\"ptpMaxCm\":%.2f,\"meanCm\":%.1f,\"lastCm\":%.1f,\"windows\":%llu}",i?",":"",oscNames[i],
                double(o.ptpCm.load()),double(o.hz.load()),double(o.ptpMaxCm.load()),double(o.meanCm.load()),double(o.lastCm.load()),(unsigned long long)o.windows.load());out+=b;}
        std::snprintf(b,sizeof b,"},\"sockets\":{\"enabled\":%s,\"published\":%llu,\"itemFrames\":%llu,\"mask\":%u,\"fromItem\":%u},",c.sockets?"true":"false",
            (unsigned long long)stats.socketPublishes.load(),(unsigned long long)stats.socketItemFrames.load(),stats.socketMask.load(),stats.socketFromItem.load());out+=b;
        // neck-model lock, freeze, drop-out hold, upright placement means
        std::snprintf(b,sizeof b,"\"neckModelCm\":[%.1f,%.1f],\"freeze\":{\"enabled\":%s,\"hands\":%s,\"pitchDeg\":[%.0f,%.0f],\"active\":%s,\"frames\":%llu,"
            "\"enters\":%llu,\"byPitch\":%llu,\"byHand\":%llu,\"lastPitchDeg\":%.1f},\"hold\":{\"lockHeld\":%llu,\"headOnlyFrames\":%llu,\"holdMs\":%.0f},"
            "\"upright\":{\"samples\":%llu,\"eyeAboveHeadCm\":%.1f,\"eyeFwdOfHeadCm\":%.1f,\"eyeAboveShoulderCm\":%.1f,\"eyeAboveNeckCm\":%.1f},"
            "\"charEye\":{\"bones\":[%d,%d],\"frames\":%llu,\"aboveHeadCm\":%.1f,\"fwdOfHeadCm\":%.1f,\"aboveRootCm\":%.1f,\"bindInHeadCm\":[%.1f,%.1f,%.1f],"
            "\"cameraAboveCharEyeCm\":%.1f},",
            double(c.neckBack*100.f),double(c.neckDown*100.f),c.freeze?"true":"false",c.freezeHands?"true":"false",double(c.freezePitchDeg),double(c.unfreezePitchDeg),
            stats.frozenNow.load()?"true":"false",(unsigned long long)stats.freezeFrames.load(),(unsigned long long)stats.freezeEnters.load(),
            (unsigned long long)stats.freezeByPitch.load(),(unsigned long long)stats.freezeByHand.load(),double(stats.lastPitchDeg.load()),
            (unsigned long long)stats.lockHeld.load(),(unsigned long long)stats.headOnlyFrames.load(),double(c.holdS*1000.f),
            (unsigned long long)stats.uprightSamples.load(),double(stats.eyeAboveHeadMean.load()),double(stats.eyeFwdHeadMean.load()),
            double(stats.eyeAboveShoulderMean.load()),double(stats.eyeAboveNeckMean.load()),
            r?r->eyePivot[0]:-1,r?r->eyePivot[1]:-1,(unsigned long long)stats.charEyeFrames.load(),double(stats.charEyeAboveHeadCm.load()),
            double(stats.charEyeFwdOfHeadCm.load()),double(stats.charEyeAboveRootCm.load()),r&&r->eyeBindValid?double(r->eyeBind.x*100.f):0.0,
            r&&r->eyeBindValid?double(r->eyeBind.y*100.f):0.0,r&&r->eyeBindValid?double(r->eyeBind.z*100.f):0.0,
            stats.charEyeFrames.load()?double(settings().bodyEyeUpMeters()*100.f-stats.charEyeAboveHeadCm.load()):0.0);out+=b;
        // breakaway of the freeze, the pitch latch itself, head heading vs the body yaw target
        // head in the shadow
        {const bool live=headShadowLive.load();const char* hs=live?(headCaptures.load()?"LIVE":"ARMED"):headShadowState.load();
         std::snprintf(b,sizeof b,"\"headShadow\":{\"enabled\":%s,\"state\":\"%s\",\"fillInstalled\":%s,\"captures\":%llu,\"collapsedSets\":%llu,"
            "\"layoutFails\":%llu,\"fillHits\":%llu,\"fillMisses\":%llu,\"fillInvalid\":%llu,\"fillAsIs\":%llu,\"prevFromSlot\":%llu},",
            c.headShadow?"true":"false",hs,headFillInstalled.load()?"true":"false",(unsigned long long)headCaptures.load(),
            (unsigned long long)headCollapsedSets.load(),(unsigned long long)headLayoutFails.load(),(unsigned long long)headFillHits.load(),
            (unsigned long long)headFillMisses.load(),(unsigned long long)headFillInvalid.load(),(unsigned long long)headFillAsIs.load(),
            (unsigned long long)headPrevFromSlot.load());out+=b;}
        std::snprintf(b,sizeof b,"\"freezeBreak\":{\"breakDeg\":%.0f,\"reengageDeg\":%.0f,\"lockBreakCm\":%.0f,\"beltCoreCm\":%.0f,\"breaks\":%llu,\"byYaw\":%llu,"
            "\"byLock\":%llu,\"breakFrames\":%llu,\"breakNow\":%s,\"latched\":%s,\"latchClears\":%llu,\"headVsBodyDeg\":%.1f,\"headVsBodyDegMax\":%.1f,"
            "\"lockKnee\":%.2f,\"warpSnaps\":%llu,\"warpSnapFrames\":%llu,\"warpSnapTimeouts\":%llu},",
            double(c.breakDeg),double(c.reengageDeg),double(c.lockBreakCm),double(c.beltCoreM*100.f),(unsigned long long)stats.freezeBreaks.load(),
            (unsigned long long)stats.breakByYaw.load(),(unsigned long long)stats.breakByLock.load(),(unsigned long long)stats.breakFrames.load(),
            stats.breakNow.load()?"true":"false",stats.pitchLatchedNow.load()?"true":"false",(unsigned long long)stats.latchClears.load(),
            double(stats.headVsBodyDeg.load()),double(stats.headVsBodyDegMax.load()),double(c.lockKnee),(unsigned long long)stats.warpSnaps.load(),
            (unsigned long long)stats.warpSnapFrames.load(),(unsigned long long)stats.warpSnapTimeouts.load());out+=b;
    }
    // RIDING: the mounted arm config + the rein rope (VrReinRope.h)
    std::snprintf(b,sizeof b,"\"riding\":{\"reinsArm\":%s,\"mountRel\":%s,\"mountHoldMs\":%.0f,\"mountFadeMs\":%.0f,\"ropeFree\":%s,\"rope\":",
        c.reinsArm?"true":"false",c.mountRel?"true":"false",double(c.mountHoldMs),double(c.mountFadeMs),c.ropeFree?"true":"false");out+=b;out+=::rdrvr::vr::reinrope::report();out+="},";
    out+="\"seat\":";out+=::rdrvr::vr::seat::report(r?&r->seat:nullptr);out+=","; // SEAT (VrSeat.h)
    {char sb[768];std::snprintf(sb,sizeof sb,"\"steady\":{\"riderBase\":[%llu,%llu,%llu],\"partsPub\":%llu,\"partsUsed\":%llu,\"partsStale\":%llu,\"partsBusy\":%llu,\"rider\":%llu,\"horse\":%llu,\"noBone\":%llu,\"mismatch\":%llu,\"riderCm\":%.1f,\"horseCm\":%.1f,\"riderBobCm\":%.1f,\"horseBobCm\":%.1f,\"headCm\":%.1f,\"headBobCm\":%.1f,\"mount\":%s},",
        (unsigned long long)steady::riderHorseBase.load(),(unsigned long long)steady::riderHeldBase.load(),(unsigned long long)steady::riderOwnBase.load(),
        (unsigned long long)::rdrvr::vr::steadylink::partsPublished.load(),(unsigned long long)::rdrvr::vr::steadylink::partsUsed.load(),
        (unsigned long long)::rdrvr::vr::steadylink::partsStale.load(),(unsigned long long)::rdrvr::vr::steadylink::partsBusy.load(),(unsigned long long)steady::riderFrames.load(),(unsigned long long)steady::horseFrames.load(),(unsigned long long)steady::horseNoBone.load(),
        (unsigned long long)steady::horseMismatch.load(),steady::riderShiftCm.load(),steady::horseShiftCm.load(),steady::riderBobCm.load(),steady::horseBobCm.load(),steady::headShiftCm.load(),steady::headBobCm.load(),steady::mountActor.load()?"true":"false");out+=sb;}
    {namespace bl=::rdrvr::vr::bodylink;char sb[640]; // hook A side of the AppSW player body / rig map / horse chains (VrMotionBodyLink.h)
     std::snprintf(sb,sizeof sb,"\"mvParts\":{\"body\":%d,\"horse\":%d,\"keys\":%s,\"published\":%llu,\"busy\":%llu,\"noMap\":%llu,\"noParts\":%llu,\"lastParts\":%u,"
        "\"horseChains\":\"%s\",\"horseSelects\":%llu,\"horseSelectFails\":%llu,\"horseLegSegs\":%u,\"horseNeckSegs\":%u,\"horseFrames\":%llu,\"horseDrops\":%llu},",
        bl::bodyProp.load(),bl::horseProp.load(),r&&r->mvKeys.humanoid()?"true":"false",(unsigned long long)bl::published.load(),(unsigned long long)bl::busy.load(),
        (unsigned long long)bl::noMap.load(),(unsigned long long)bl::noParts.load(),bl::lastParts.load(),steady::horseChainState.load(),
        (unsigned long long)steady::horseSelects.load(),(unsigned long long)steady::horseSelectFails.load(),steady::horseLegSegs.load(),steady::horseNeckSegs.load(),
        (unsigned long long)steady::horsePartFrames.load(),(unsigned long long)steady::horsePartDrops.load());out+=sb;}
    const auto avg=[](const Counter& s,const Counter& n){const auto k=n.load();return k?double(s.load())/double(k)/1000.0:0.0;};
    std::snprintf(b,sizeof b,"\"timing\":{\"aUsAvg\":%.1f,\"aUsMax\":%.1f,\"bUsAvg\":%.1f,\"bUsMax\":%.1f,\"cUsAvg\":%.1f,\"cUsMax\":%.1f,\"tickUsAvg\":%.1f,"
        "\"tickUsMax\":%.1f},\"exceptions\":%llu}",avg(stats.aNsSum,stats.aCount),double(stats.aUsMax.load()),avg(stats.bNsSum,stats.bCount),
        double(stats.bUsMax.load()),avg(stats.cNsSum,stats.cCount),double(stats.cUsMax.load()),avg(stats.tickNsSum,stats.ticks),double(stats.tickUsMax.load()),
        (unsigned long long)stats.exceptions.load());out+=b;
    return out;
}

} // namespace rdrvr::vr::bodyik
