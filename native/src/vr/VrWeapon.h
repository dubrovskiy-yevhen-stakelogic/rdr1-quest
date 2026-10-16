#pragma once
// The weapon the HOLSTERS module reports in a VR hand (holsters::holdHand >= 0) follows that controller:
//  - pose: the existing weapWeapon::Update(Matrix34 attachment, ...) boundary (GOT 0x2e48860, pinned caller invWeapon::OnUpdatePosition
//    ret 0x26770a4) gets the attachment ("hand bone") matrix placed at the holding controller's grip, barrel (authored muzzle
//    locator -Z) along the controller aim ray, roll from the grip, plus a per-class offset. Works on ANY thread (the engine updates
//    the player's weapon on job threads too, which an owner-thread gate would skip).
//  - model: for skinned/animated weapons (flags+0x120 bit5, e.g. the Cattleman revolver)
//    UpdateSkeleton moves the model with the PREVIOUS weapon matrix (SetTransform(weapon+0x80) at 0x2543df4..0x2543e0c, before the
//    new attachment is copied at 0x25437d0..); the existing SetTransform boundary (ret 0x2543e10) gets attachment*locator of THIS
//    call, so the model follows the controller without a frame of lag.
//  - aim: the existing weapWeapon::Aim(Vector3 ...) boundary (GOT 0x2e4e958) gets a FREE target on the barrel line
//    (muzzle + barrel * aimDistance): bullets leave along the gun barrel. Native fire / ammo / spread / RNG unchanged.
//  - tracking -> world: the SAME rigid LOCAL->world transform as the rendered eyes (plan.world[0] o eye0^-1 of the latest recorded
//    StereoCameraPlan, exactly mapControllerRay's math) plus the body-anchor shift since that plan's head sample (the plan is 1-2
//    frames older than the weapon update; the eyes of this frame use a newer anchor).
// Placement: the engine WRIST joint (derived from the actor skeleton) on the VR hand mesh wrist instead of the
// attachment bone on the controller grip; this frame's body anchor predicted at pose time from the capsule (no walking lag); a
// snap-turn bridge; the pose-time placement tagged with the head sample of the plan its eyes render and published to the hands pass
// (holding hand + laser drawn with exactly that pose); barrel-exact shots (vector aim forced over lock-on / aim-to-miss, zero spread).
// Kill switch: debug.rdrvr.vr.weapon 0 (startup) -> nothing here runs.
// Threads: observe() / noteAnchor() = engine main thread (PostCameraUpdate). pose/model/aim helpers = any thread (stats atomic,
// owner/anchor/placement behind short mutexes, the per-call pending model matrix is thread_local). report() = any thread.
#include "TrackedWeaponPose.h"
#include "WeaponContextReader.h"
#include "WeaponTypeReader.h"
#include "HeadCameraAnchor.h"
#include "VrHolsters.h"
#include "VrReload.h" // manual revolver reload (cylinder swing-out on B)
#include "VrHammer.h" // HAMMER: fanning / single action (claimFire, FAN SPREAD, report vr.weapon.hammer)
#include "VrSettings.h" // LASER SIGHT menu state in the report
#include "VrWeaponCalib.h" // WEAPON CALIBRATION (weapons.cfg), applied live on top of the placement
#include "VrTeleportLink.h" // TELEPORT: warp note -> the prediction's warp bridge
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <string>

namespace rdrvr::vr::weapon {
namespace E=::rdrvr::engine;namespace RT=::rdrvr::runtime;

// ---- weapon: SetProjectileLaunchOverride ----
// sagPlayer::UpdateAttack (0x2699d78, called from sagPlayer::PostCameraUpdate 0x269c1d8) calls, EVERY frame, for the player's
// weapon whose type has flags+0x408 bit0 and weapon+0x9c8 == 0: weapWeapon::SetProjectileLaunchOverride(true, reticle camera
// matrix [tgtReticleComponent+0x3710..0x3838]) -> weapon+0x9c2 = 1 and the matrix at weapon+0xd0..0x108 (0x25446e0..0x254476c).
// With +0x9c2 set, weapWeapon::ComputeMainMuzzlePosition (0x254632c, used by aniAim::Fire 0x1cbbae4 and weapSimpleAimer::Aim)
// returns the CAMERA position (+0x100..0x108) instead of the muzzle: shots leave the head, not the barrel.
// +0x9c2 is also the WeaponContext "launchOverride" field, so an aim gate on that field would decline every
// Aim. For the VR-held weapon the mod calls the same setter with false (+0x9c2 = 0 unless +0x9c3, the
// scripted DrV override) before the weapon's Update / Aim: bullets launch from the VR muzzle along the barrel.
using SetLaunchOverrideFn=void(*)(void* weapon,bool on,const void* matrix);
inline constexpr std::uintptr_t setLaunchOverrideOffset=0x25446e0; // weapWeapon::SetProjectileLaunchOverride(bool, Matrix34 const&), whole function
inline constexpr std::array<unsigned char,144> setLaunchOverrideFingerprint{
#include "gamecode/VrWeapon_25446e0_144_8d.inc"
};
inline constexpr std::size_t weaponLaunchOverride=0x9c2,weaponLaunchOverrideDrv=0x9c3;
inline std::atomic<SetLaunchOverrideFn> setLaunchOverride{};
enum LaunchSite : unsigned {LaunchPose,LaunchAim,LaunchManager,LaunchFire,LaunchSiteCount};
inline std::array<std::atomic<std::uint64_t>,LaunchSiteCount> launchSeen{},launchCleared{};inline std::atomic<std::uint64_t> launchDrv{},launchUnreadable{};

// ---- configuration (startup properties; written once before any hook can run) ----
struct Offset {float right{},up{},forward{},pitchDeg{},rollDeg{};}; // metres / degrees in the controller grip frame (+fwd = barrel)
inline constexpr std::size_t classCount=14; // engine::WeaponClass Unknown..Cannon
// Anchor: WRIST = the engine hand bone's WRIST is put on the VR hand mesh's wrist (grip - wristM *
//   aim forward, the same point HandsMath::handModel puts the mesh wrist ring at); GRIP = hand bone
//   origin on the controller grip origin.
// Predict: CAPSULE = the body anchor of THIS frame (eyes) is predicted at pose time from the
//   capsule's motion since the newest anchor (weapWeapon::Update runs ~2 ms before
//   PostCameraUpdate, after the physics step); VELOCITY = mover velocity * dt; OFF = plan anchor ->
//   newest anchor only (one frame of walking lag).
enum class AnchorMode : std::uint8_t {Grip,Wrist};
enum class Predict : std::uint8_t {Off,Capsule,Velocity};
// Hold: BARREL = authored muzzle -Z along the controller aim, roll from the grip; HAND = the engine
// hand bone onto the VR hand by anatomy (wrist joint -> VR wrist, wrist->finger base -> VR forward,
// thumb side -> VR thumb side): the weapon sits in the VR hand exactly as the authored locator puts
// it in the character's hand (knife, tomahawk, lasso, rigid guns without a muzzle locator).
enum class HoldMode : std::uint8_t {Auto,Barrel,Hand};
struct Config {
    bool enabled{true},latency{true},melee{true},launch{true};float aimDistanceM{150.f};std::uint64_t planMaxAgeNs{250'000'000};
    std::array<Offset,classCount> offsets{};
    AnchorMode anchor{AnchorMode::Wrist};float wristM{0.055f};bool handBone{true};
    Predict predict{Predict::Capsule};bool turnBridge{true};int turnSign{0}; // 0 = learn from the first post-turn plan (start +1)
    bool aimFix{true},spread{false};float laserLengthM{50.f},laserWidthMm{2.5f};unsigned laserProbe{1};
    // Hold mode per weapon (AUTO = BARREL when the weapon has an authored muzzle locator, i.e. it is
    // animated, else HAND), thrown / lasso held too, the last placement reused for frames the pipeline
    // declines, calibration, rigidity repair.
    HoldMode hold{HoldMode::Auto};bool thrown{true},lasso{true},holdLast{true},calibration{true};std::uint64_t holdLastNs{300'000'000};
    bool knifeFlip{false}; // debug.rdrvr.vr.weapon.knifeflip 1 = turn HAND-held melee 180 deg (diagnostic)
    float handRollDeg{}; // debug.rdrvr.vr.weapon.handroll: extra roll of the engine thumb axis about wrist->knuckle (HAND, -90..90)
};
inline Config config{};
inline std::atomic<bool> started{};

inline bool firearm(E::WeaponClass k) noexcept {
    using W=E::WeaponClass;
    return k==W::Pistol||k==W::DualPistol||k==W::Rifle||k==W::Repeater||k==W::SniperRifle||k==W::Shotgun||k==W::Bow;
}
// Thrown weapons (tomahawk, dynamite, throwing knives) and the lasso are held by the VR hand too.
// Turrets / cannons are never hand-held.
inline bool attachable(E::WeaponClass k,const Config& c) noexcept {
    using W=E::WeaponClass;
    return firearm(k)||(c.melee&&k==W::Melee)||(c.thrown&&(k==W::Thrown||k==W::ThrownExploding))||(c.lasso&&k==W::Lasso);
}
inline const char* classKey(E::WeaponClass k) noexcept {
    constexpr const char* keys[]{"unknown","pistol","dualpistol","rifle","repeater","sniper","shotgun","thrown","explosive","bow","melee",
        "lasso","turret","cannon"};
    const auto i=unsigned(k);return i<classCount?keys[i]:keys[0];
}
// "x y z [pitch [roll]]" (metres, degrees). False on garbage (the default stays).
inline bool parseOffset(const std::string& text,Offset& out) noexcept {
    if(text.empty())return false;
    float v[5]{};int n=0;const char* p=text.c_str();
    while(*p&&n<5){char* end=nullptr;const float x=std::strtof(p,&end);if(end==p)break;if(!std::isfinite(x))return false;v[n++]=x;p=end;
        while(*p==' '||*p==','||*p==';')++p;}
    if(n<3||*p)return false;
    for(int i=0;i<3;++i)if(std::fabs(v[i])>.5f)return false;
    if(std::fabs(v[3])>90.f||std::fabs(v[4])>180.f)return false;
    out={v[0],v[1],v[2],v[3],v[4]};return true;
}
inline void noteTrigger(int hand,bool forwarded,const char* block,std::uint64_t now) noexcept;
// debug.rdrvr.vr.weapon 0 = off; .aim held|trigger|off (engine force-aim while a VR-held firearm is out); .keep 0 (no hold-mode
// timer refresh); .latency 0 (no anchor shift); .melee 0 (knife stays in the animated hand); .offset / .offset.<class> "x y z p r".
// With BODY the BARREL placement uses the locator / hand bone of the VR HOLDING side (left VR hand =
// the alt-handed locator +0x100 and wrist_l_attachment) instead of the engine's alt flag (the engine attaches the revolver on its right
// attachment bone even in the left VR hand, which would leave the left wrist on the free-hand mapping and the gun off the
// hand). debug.rdrvr.vr.body.leftalt 0 = use the engine's alt flag.
inline std::atomic<bool> sideAltEnabled{true};
inline void startup() noexcept {
    if(started.exchange(true))return;
    namespace H=::rdrvr::vr::holsters;
    {const auto v=H::property("debug.rdrvr.vr.body.leftalt");sideAltEnabled.store(v.empty()||v[0]!='0',std::memory_order_relaxed);}
    const auto off=[](const std::string& v){return !v.empty()&&v[0]=='0';};
    Config c{};
    c.enabled=!off(H::property("debug.rdrvr.vr.weapon"));
    c.latency=!off(H::property("debug.rdrvr.vr.weapon.latency"));
    c.melee=!off(H::property("debug.rdrvr.vr.weapon.melee"));
    Offset all{};if(parseOffset(H::property("debug.rdrvr.vr.weapon.offset"),all))c.offsets.fill(all);
    for(unsigned k=1;k<classCount;++k){Offset o{};
        if(parseOffset(H::property((std::string("debug.rdrvr.vr.weapon.offset.")+classKey(E::WeaponClass(k))).c_str()),o))c.offsets[k]=o;}
    c.launch=!off(H::property("debug.rdrvr.vr.weapon.launch")); // 0 = keep the native camera launch override
    { // Anchor / prediction / turn bridge / aim fix / spread / laser
        const auto anchor=H::property("debug.rdrvr.vr.weapon.anchor");if(anchor=="grip")c.anchor=AnchorMode::Grip;
        const auto num=[](const std::string& t,float lo,float hi,float& out){if(t.empty())return;char* e{};const float v=std::strtof(t.c_str(),&e);
            if(e!=t.c_str()&&std::isfinite(v)&&v>=lo&&v<=hi)out=v;};
        num(H::property("debug.rdrvr.vr.weapon.wrist"),0.f,.2f,c.wristM);
        c.handBone=!off(H::property("debug.rdrvr.vr.weapon.handbone"));
        const auto predict=H::property("debug.rdrvr.vr.weapon.predict");
        if(predict=="off"||predict=="0")c.predict=Predict::Off;else if(predict=="velocity")c.predict=Predict::Velocity;
        c.turnBridge=!off(H::property("debug.rdrvr.vr.weapon.turn"));
        const auto sign=H::property("debug.rdrvr.vr.weapon.turnsign");if(sign=="1"||sign=="+1")c.turnSign=1;else if(sign=="-1")c.turnSign=-1;
        c.aimFix=!off(H::property("debug.rdrvr.vr.weapon.aimfix"));
        c.spread=H::property("debug.rdrvr.vr.weapon.spread")=="1";
        num(H::property("debug.rdrvr.vr.weapon.laser.length"),2.f,300.f,c.laserLengthM);
        num(H::property("debug.rdrvr.vr.weapon.laser.width"),.5f,20.f,c.laserWidthMm);
        float probe=float(c.laserProbe);num(H::property("debug.rdrvr.vr.weapon.laser.probe"),0.f,60.f,probe);c.laserProbe=unsigned(probe);
    }
    {
        const auto hold=H::property("debug.rdrvr.vr.weapon.hold");
        if(hold=="barrel")c.hold=HoldMode::Barrel;else if(hold=="hand")c.hold=HoldMode::Hand;
        c.thrown=!off(H::property("debug.rdrvr.vr.weapon.thrown"));c.lasso=!off(H::property("debug.rdrvr.vr.weapon.lasso"));
        c.holdLast=!off(H::property("debug.rdrvr.vr.weapon.holdlast"));c.calibration=!off(H::property("debug.rdrvr.vr.weapon.calibration"));
        c.knifeFlip=H::property("debug.rdrvr.vr.weapon.knifeflip")=="1";
        {const auto t=H::property("debug.rdrvr.vr.weapon.handroll");if(!t.empty()){char* e{};const float v=std::strtof(t.c_str(),&e);
            if(e!=t.c_str()&&std::isfinite(v)&&v>=-90.f&&v<=90.f)c.handRollDeg=v;}}
    }
    config=c;
    // Free trigger as LT ("native"), knife trigger as RT attack ("1"); manual reload; trigger telemetry.
    H::ltNative.store(H::property("debug.rdrvr.vr.weapon.lt")=="native",std::memory_order_relaxed);
    H::meleeTrigger.store(H::property("debug.rdrvr.vr.weapon.meleeTrigger")=="1",std::memory_order_relaxed);
    // "native" = triggers of empty VR hands reach the game
    H::emptyTriggerNative.store(H::property("debug.rdrvr.vr.weapon.emptytrig")=="native",std::memory_order_relaxed);
    H::triggerSink.store(c.enabled?&noteTrigger:nullptr,std::memory_order_release);
    if(c.enabled){::rdrvr::vr::reload::startup();::rdrvr::vr::hammer::startup();} // HAMMER: after the reload (it uses the reload's gate sink)
    calib::store.ensureLoaded(); // weapons.cfg next to vr.cfg (retried lazily by the menu / owner if vr.cfg is not loaded yet)
    const auto aim=H::property("debug.rdrvr.vr.weapon.aim");
    H::forceAimMode.store(int(!c.enabled||aim=="off"||aim=="0"?H::ForceAim::Off:aim=="trigger"?H::ForceAim::Trigger:H::ForceAim::Held),std::memory_order_relaxed);
    H::keepDrawn.store(c.enabled&&!off(H::property("debug.rdrvr.vr.weapon.keep")),std::memory_order_relaxed);
}

// ---- gates / reasons (vr.weapon report: why a call was not attached) ----
enum Reason : unsigned {Ok,Disabled,NotHeld,NoOwner,StaleOwner,Context,Class,NoInput,NoPlan,MapScene,MapInput,MapEpoch,MapInputStale,
    MapPlanStale,MapPose,MapWorld,Reach,NoGeometry,BadArgument,Place,Exception,ModelScope,ModelMatrix,ModelUsed,AimBlocked,ReasonCount};
inline constexpr std::array<const char*,ReasonCount> reasonNames{"OK","DISABLED","NOT_HELD","NO_OWNER","STALE_OWNER","CONTEXT","CLASS",
    "NO_INPUT","NO_PLAN","MAP_SCENE","MAP_INPUT","MAP_EPOCH","MAP_INPUT_STALE","MAP_PLAN_STALE","MAP_POSE","MAP_WORLD","REACH",
    "NO_GEOMETRY","BAD_ARGUMENT","PLACE","EXCEPTION","MODEL_SCOPE","MODEL_MATRIX","MODEL_USED","AIM_BLOCKED"};
enum Channel : unsigned {ChannelPose,ChannelModel,ChannelAim,ChannelCount};
struct Stats {
    std::array<std::atomic<std::uint64_t>,ReasonCount> reasons{};std::atomic<std::uint64_t> calls{},done{};
    std::atomic<unsigned> last{Ok};std::atomic<std::uint64_t> lastNs{};
};
inline std::array<Stats,ChannelCount> stats{};
inline bool decline(Channel c,Reason why,std::uint64_t now) noexcept {
    auto& s=stats[c];s.reasons[why].fetch_add(1,std::memory_order_relaxed);s.last.store(why,std::memory_order_relaxed);
    s.lastNs.store(now,std::memory_order_relaxed);return false;
}
inline void accepted(Channel c,std::uint64_t now) noexcept {
    auto& s=stats[c];s.done.fetch_add(1,std::memory_order_relaxed);s.last.store(Ok,std::memory_order_relaxed);s.lastNs.store(now,std::memory_order_relaxed);
}
inline std::atomic<std::uint64_t> poseCompleted{},poseCompletedNative{},aimFromPose{},aimFromController{},modelSkipped{};

// ---- weapon geometry (main thread, cached per weapon) ----
// modelMuzzle = model^-1 * muzzle from ONE skeleton state: GetWeaponMatrix(false) (skeleton root or weapon+0x80, 0x25466c4) and
// GetMuzzleLocationMatrix(false) (muzzle locator over the same skeleton, or weapon+0x80 rows + authored offset, 0x2546428).
// locator[alt] = the authored attachment->model locator weapWeapon::Update multiplies (0x2543838.. / 0x25438ec..):
// animated: [weapon+0x338]+0xc0 (alt-handed +0x100); rigid: [weapon+0x338]+0x80; none: identity.
// Hand bone: the engine wrist joint in the frame of the bone the weapon is attached to (invBaseItem::SetBoneIDs: item+0x24 = the
// type's hand bone, item+0x28 = "wrist_l_attachment"), metres along that bone's unit axes. Zero when the attachment bone IS the
// wrist; the "_attachment" helper bones may sit off the wrist (palm). Derived from the actor skeleton (deriveHandBone).
// + the hand anatomy in the same frame (knuckle = finger base bone, thumb = thumb base bone) for the HAND hold mode, and
// how the bone was found (engine id->index map / direct index / by name).
struct HandBone {bool valid{};RT::Vec3 wrist{};int attach{-1},wristIndex{-1};std::uint32_t boneId{};
    bool anatomy{};RT::Vec3 knuckle{},thumb{};int knuckleIndex{-1},thumbIndex{-1};std::uint8_t found{}; // 0 none, 1 map, 2 direct, 3 name
};
struct Geometry {
    bool valid{},animated{};std::uintptr_t weapon{},base{},locatorPtr{};std::uint32_t modelGuid{},flags{};std::uint64_t stampNs{};
    RT::Mat4 modelMuzzle{};std::array<RT::Mat4,2> locator{};
    std::array<HandBone,2> bone{}; // [0] = the hand bone (item+0x24), [1] = alt-handed (item+0x28)
};
inline constexpr RT::Mat4 identity4{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
inline RT::Mat4 affine(RT::Mat4 m) noexcept {m[3]=m[7]=m[11]=0;m[15]=1;return m;}
inline bool rigidUnit(const RT::Mat4& m) noexcept {return E::weapon_pose_detail::rigid(m);}

// ---- owner published by the main thread ----
struct Owner {
    std::uintptr_t weapon{},actor{},item{};std::uint32_t actorGuid{},contextFlags{};E::WeaponClass kind{};std::int16_t typeId{};
    std::uint64_t stampNs{};bool poseAllowed{},aimAllowed{},forceAllowed{};Geometry geometry{};
};
inline std::mutex ownerMutex;inline Owner owner{};
inline std::atomic<std::uint64_t> observed{},geometryRefreshes{},geometryFailures{};inline std::atomic<const char*> geometryState{"NONE"};
inline Owner ownerSnapshot() {std::lock_guard lock(ownerMutex);return owner;}
// Per-class record (report "classes"): geometry facts from publishOwner, placement facts from notePlacementMode.
struct ClassSeen {
    bool seen{},animated{},hand{},calibrated{},geometry{};int typeId{};std::uint64_t poses{},handPoses{},lastNs{};const char* handWhy{""};
    char name[32]{};HoldMode hold{};std::array<HandBone,2> bone{};RT::Vec3 modelMuzzle{};float muzzleRotDeg{-1};RT::Vec3 anchorMove{};
    std::uint32_t rigidErrMicro{};
};
inline std::mutex classSeenMutex;inline std::array<ClassSeen,classCount> classSeen{};
inline void clearOwner() {{std::lock_guard lock(ownerMutex);owner=Owner{};}
    calib::publishReleased();calib::menuHoldHand.store(-1,std::memory_order_relaxed);} // The menu keeps the last weapon
// Context gates from engine::WeaponContext (computed by the caller on the owner thread).
// pose = the weapon is placed at the controller; force = engine force-aim is kept (holding-hand trigger
// fires; NOT derived from per-frame weapon fields, so it cannot flip on the engine's own reaction to
// aiming); aim = the barrel target replaces the native one. Not gates: launchOverride (+0x9c2, set EVERY
// frame by sagPlayer::UpdateAttack for camera-launch weapons; the mod clears it), aimDirectionOverride
// (+0x9c1, latched by weapWeapon::Fire, 0x25443f4 - per shot, not a script override). trackedShot
// (+0x188 bit5, Aim's own flag) only counts within 3 s of Dead Eye.
struct ContextGates {bool pose{},aim{},force{};};
inline std::atomic<std::uint64_t> deadEyeSeenNs{};
inline constexpr std::uint64_t deadEyeTailNs=3'000'000'000ull;
inline ContextGates gates(const E::WeaponContext& c,E::WeaponClass kind,const Config& cfg,std::uint64_t now=0) noexcept {
    ContextGates g{};
    g.pose=attachable(kind,cfg)&&c.alive&&!c.controlDisabled&&!c.ridingVehicle&&!c.vehicleTransition&&!c.mountedWeapon&&
        !c.mountingHorse&&!c.linkedAnimation&&!c.usingGringo;
    g.force=g.pose&&firearm(kind)&&!c.deadEye&&!c.duel&&!c.cover;
    const auto seen=deadEyeSeenNs.load(std::memory_order_relaxed);
    const bool deadEyeTail=c.deadEye||(seen&&now>=seen&&now-seen<deadEyeTailNs);
    // Dead Eye / duel / cover / blind fire / scripted DrV launch keep the native target (they aim and fire by themselves).
    g.aim=g.force&&!c.blindFiring&&!c.driveLaunchOverride&&!(c.trackedShot&&deadEyeTail);
    return g;
}
// Per-flag counters while a weapon is in a VR hand (WeaponContext::flags() bit order) - which native field is set.
inline constexpr std::array<const char*,18> contextFlagNames{"dead","controlDisabled","deadEye","riding","ridingVehicle","vehicleTransition",
    "duel","mountedWeapon","mountingHorse","blindFiring","cover","linkedAnimation","usingGringo","trackedShot","aimDirectionOverride",
    "launchOverride","driveLaunchOverride","animationFirePending"};
inline std::array<std::atomic<std::uint64_t>,18> contextFlagCounts{};inline std::atomic<std::uint64_t> gateForceFalse{},gateAimFalse{};
// Publish (main thread). geometryFn(old,out) refreshes the geometry when the weapon/model/locator key changed or every 30 calls.
template<class GeometryFn>
inline void publishOwner(const E::LocalWeapon& w,const E::WeaponContext& context,std::uint64_t now,GeometryFn&& geometryFn) {
    const Config& cfg=config;Owner next=ownerSnapshot();
    if(next.weapon!=w.weapon)next=Owner{};
    next.weapon=w.weapon;next.actor=w.actor;next.item=w.item;next.actorGuid=w.actorGuid;next.kind=w.type.kind;next.typeId=w.type.id;next.stampNs=now;next.contextFlags=context.flags();
    if(context.deadEye)deadEyeSeenNs.store(now,std::memory_order_relaxed);
    const auto g=gates(context,w.type.kind,cfg,now);next.poseAllowed=g.pose;next.aimAllowed=g.aim;next.forceAllowed=g.force;
    for(unsigned i=0;i<contextFlagCounts.size();++i)if(next.contextFlags>>i&1u)contextFlagCounts[i].fetch_add(1,std::memory_order_relaxed);
    if(firearm(w.type.kind)){if(!g.force)gateForceFalse.fetch_add(1,std::memory_order_relaxed);if(!g.aim)gateAimFalse.fetch_add(1,std::memory_order_relaxed);}
    static thread_local unsigned ordinal{};
    if(!next.geometry.valid||next.geometry.weapon!=w.weapon||++ordinal%30u==0){
        Geometry fresh{};
        if(geometryFn(next.geometry,fresh)){
            // A refresh without a hand-bone result keeps the weapon's last one (the skeleton walk may decline once).
            if(next.geometry.weapon==w.weapon)for(unsigned h=0;h<2;++h)if(!fresh.bone[h].valid&&next.geometry.bone[h].valid)fresh.bone[h]=next.geometry.bone[h];
            fresh.stampNs=now;next.geometry=fresh;geometryRefreshes.fetch_add(1,std::memory_order_relaxed);}
        else{geometryFailures.fetch_add(1,std::memory_order_relaxed);if(next.geometry.weapon!=w.weapon)next.geometry=Geometry{};}
    }
    {std::lock_guard lock(ownerMutex);owner=next;}
    observed.fetch_add(1,std::memory_order_relaxed);
    { // Per-model handguns (VrReload.h Model): the hand item's type name, bound to this weapon while its item is the holsters' hand item
        namespace H=::rdrvr::vr::holsters;namespace L=::rdrvr::vr::reload;
        static thread_local std::uintptr_t modelItem{},modelGun{};static thread_local unsigned modelAge{};
        H::InventoryLink l{};const bool same=w.item&&H::inventoryBox.load(l)&&l.valid&&l.handItem==w.item;
        if(!same){modelItem=modelGun=0;L::publishModel(w.weapon,nullptr);}
        else if(modelItem!=w.item||modelGun!=w.weapon||++modelAge%15u==0u){modelItem=w.item;modelGun=w.weapon;modelAge=0;
            const auto name=H::handName.load();L::publishModel(w.weapon,name.c_str());}
    }
    { // WEAPON CALIBRATION menu (current weapon, holding hand), weapons.cfg load, per-class geometry record
        namespace H=::rdrvr::vr::holsters;
        const int hand=H::holdHand.load(std::memory_order_relaxed);calib::menuHoldHand.store(hand,std::memory_order_relaxed);
        static thread_local std::uintptr_t namedWeapon{};static thread_local int namedHand{-2};static thread_local std::uint64_t namedRefresh{};
        if(namedWeapon!=w.weapon||namedHand!=hand||next.geometry.stampNs!=namedRefresh){
            namedWeapon=w.weapon;namedHand=hand;namedRefresh=next.geometry.stampNs;
            const auto name=H::handName.load();calib::publishCurrent(int(w.type.kind),calib::typeKey(int(w.type.kind),int(w.type.id)),hand,name.c_str());
            calib::store.ensureLoaded();
            std::lock_guard lock(classSeenMutex);auto& c=classSeen[unsigned(w.type.kind)%classCount];
            c.seen=true;c.typeId=w.type.id;std::snprintf(c.name,sizeof c.name,"%s",name.c_str());
            for(char& ch:c.name)if(ch=='"'||ch=='\\'||(ch&&static_cast<unsigned char>(ch)<0x20))ch='?';
            const auto& ng=next.geometry;c.geometry=ng.valid;c.animated=ng.animated;c.bone=ng.bone;
            c.modelMuzzle={ng.modelMuzzle[12],ng.modelMuzzle[13],ng.modelMuzzle[14]};
            const float tr=(ng.modelMuzzle[0]+ng.modelMuzzle[5]+ng.modelMuzzle[10]-1.f)*.5f;
            c.muzzleRotDeg=ng.valid?std::acos(std::clamp(tr,-1.f,1.f))*57.29578f:-1.f; // 0 = no muzzle locator (rigid: model axes)
        }
    }
    ::rdrvr::vr::holsters::heldFirearm.store(firearm(w.type.kind)&&g.force,std::memory_order_relaxed);
    ::rdrvr::vr::holsters::heldKind.store(int(w.type.kind),std::memory_order_relaxed);
}
// Pure: modelMuzzle from a coherent (root, muzzle) pair + locators. False = unusable (non-rigid, absurd offset).
inline bool buildGeometry(const RT::Mat4& root,const RT::Mat4& muzzle,const RT::Mat4& locator,const RT::Mat4& altLocator,Geometry& out) noexcept {
    RT::Mat4 local{};float scale{};
    if(!E::deriveModelMuzzle(affine(root),affine(muzzle),local,scale))return false;
    const float reach=std::sqrt(local[12]*local[12]+local[13]*local[13]+local[14]*local[14]);
    if(!std::isfinite(reach)||reach>3.f)return false;
    if(!rigidUnit(affine(locator))||!rigidUnit(affine(altLocator)))return false;
    out.modelMuzzle=local;out.locator={affine(locator),affine(altLocator)};out.valid=true;return true;
}

// ---- body-anchor shift (plan anchor -> newest anchor), same composition as CameraAdapter ----
// world_e = base * [R(inverseYaw) | -R(inverseYaw)*origin*s] * E_e, base translation = anchor.position (prepareHeadCamera):
// LOCAL->world translation T = anchor - Rmap*origin*s  =>  dT = dAnchor - Rmap*dOrigin*s  (same origin serial, same heading).
struct AnchorRecord {std::uint64_t sampledNs{},originSerial{};RT::Vec3 position{},origin{};bool originValid{};
    RT::Vec3 up{0,1,0},capsule{},velocity{};bool capsuleValid{},velocityValid{}; // Mover facts of the same sample
};
class AnchorRing {
    mutable std::mutex mutex_;std::array<AnchorRecord,16> ring_{};unsigned next_{};
public:
    void note(const AnchorRecord& r) {std::lock_guard lock(mutex_);ring_[next_]=r;next_=(next_+1)%unsigned(ring_.size());}
    bool find(std::uint64_t sampledNs,AnchorRecord& out) const {
        std::lock_guard lock(mutex_);if(!sampledNs)return false;
        for(const auto& r:ring_)if(r.sampledNs==sampledNs){out=r;return true;}
        return false;
    }
    bool newest(AnchorRecord& out) const {
        std::lock_guard lock(mutex_);bool any=false;
        for(const auto& r:ring_)if(r.sampledNs&&(!any||r.sampledNs>out.sampledNs)){out=r;any=true;}
        return any;
    }
    // The newest record strictly older than `ns` (frame interval for the velocity prediction).
    bool previous(std::uint64_t ns,AnchorRecord& out) const {
        std::lock_guard lock(mutex_);bool any=false;
        for(const auto& r:ring_)if(r.sampledNs&&r.sampledNs<ns&&(!any||r.sampledNs>out.sampledNs)){out=r;any=true;}
        return any;
    }
    void clear() {std::lock_guard lock(mutex_);ring_={};next_=0;}
};
inline AnchorRing anchors{};
inline AnchorRecord anchorRecord(const E::HeadCameraAnchor& a,const E::BodyAnchorFacts* facts=nullptr) noexcept {
    AnchorRecord r{};r.sampledNs=a.sampledNs;r.position=a.position;r.origin=a.origin.position;r.originSerial=a.origin.serial;r.originValid=a.origin.valid;
    r.up=a.up;
    if(facts&&facts->valid){r.capsule=facts->capsule;r.capsuleValid=true;
        if(facts->velocityKnown){r.velocity=facts->velocity;r.velocityValid=true;}}
    return r;
}
inline void tagFrame(const AnchorRecord& anchor) noexcept; // defined below: tags this frame's placement with its anchor
inline void tagPhantom(const AnchorRecord& anchor) noexcept; // DUAL (defined below): the same for the phantom's placement
inline void noteAnchor(const E::HeadCameraAnchor& a,const E::BodyAnchorFacts* facts=nullptr) {
    const auto r=anchorRecord(a,facts);anchors.note(r);tagFrame(r);tagPhantom(r);
}
// LOCAL->world rotation of a plan applied to v (exactly mapControllerRay's toWorld).
inline RT::Vec3 planRotate(const E::StereoCameraPlan& plan,RT::Vec3 v) noexcept {
    const auto& w=plan.world[0];const auto q=plan.tracking.eyes[0].orientation;
    v=RT::rotate(RT::Quat{-q[0],-q[1],-q[2],q[3]},v);
    return {w[0]*v[0]+w[4]*v[1]+w[8]*v[2],w[1]*v[0]+w[5]*v[1]+w[9]*v[2],w[2]*v[0]+w[6]*v[1]+w[10]*v[2]};
}
inline bool anchorShift(const E::StereoCameraPlan& plan,const AnchorRecord& planAnchor,const AnchorRecord& latest,RT::Vec3& shift) noexcept {
    shift={};
    if(!planAnchor.sampledNs||!latest.sampledNs||latest.sampledNs<planAnchor.sampledNs)return false;
    if(planAnchor.originValid!=latest.originValid||(latest.originValid&&latest.originSerial!=planAnchor.originSerial))return false;
    RT::Vec3 d{};for(unsigned i=0;i<3;++i)d[i]=latest.origin[i]-planAnchor.origin[i];
    if(!latest.originValid)d={};
    const auto r=planRotate(plan,d);double len2{};
    for(unsigned i=0;i<3;++i){shift[i]=(latest.position[i]-planAnchor.position[i])-r[i]*plan.unitsPerMetre;len2+=double(shift[i])*shift[i];}
    if(!std::isfinite(len2)||len2>1.0){shift={};return false;}
    return true;
}

// ---- controller -> world (mapControllerRay with reasons, a relaxed plan age and the anchor shift) ----
inline Reason mapRay(const E::StereoCameraPlan& plan,const RT::ControllerSample& input,unsigned hand,bool gripPose,std::uint64_t now,
                     std::uint64_t planMaxAgeNs,const RT::Vec3& shift,E::ControllerRay& out) noexcept {
    using namespace RT;
    if(hand>1)return MapPose;
    const auto& pose=gripPose?input.grip[hand]:input.aim[hand];const auto& track=plan.tracking;
    if(!plan.sceneId||!track.frameId)return MapScene;
    if(!validController(input)||input.sequence<track.frameId)return MapInput;
    if(!track.epochs.tracking||!track.epochs.referenceSpace||input.epoch!=track.epochs.tracking||input.referenceSpace!=track.epochs.referenceSpace)return MapEpoch;
    if(!input.publishedNs||now<input.publishedNs||now-input.publishedNs>TrackedViewsMailbox::maxAgeNs)return MapInputStale;
    if(!track.publishedNs||now<track.publishedNs||now-track.publishedNs>planMaxAgeNs)return MapPlanStale;
    if(!::rdrvr::validPose(track.eyes[0])||!::rdrvr::validPose(pose))return MapPose;
    if(!std::isfinite(plan.unitsPerMetre)||plan.unitsPerMetre<.01f||plan.unitsPerMetre>100)return MapWorld;
    const auto& world=plan.world[0];
    for(float x:world)if(!std::isfinite(x))return MapWorld;
    if(world[3]!=0||world[7]!=0||world[11]!=0||world[15]!=1)return MapWorld;
    const auto dot=[](Vec3 a,Vec3 b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    const Vec3 x{world[0],world[1],world[2]},y{world[4],world[5],world[6]},z{world[8],world[9],world[10]};
    const Vec3 cross{x[1]*y[2]-x[2]*y[1],x[2]*y[0]-x[0]*y[2],x[0]*y[1]-x[1]*y[0]};
    if(std::abs(dot(x,x)-1)>.005f||std::abs(dot(y,y)-1)>.005f||std::abs(dot(z,z)-1)>.005f||
       std::abs(dot(x,y))>.005f||std::abs(dot(x,z))>.005f||std::abs(dot(y,z))>.005f||dot(cross,z)<.995f)return MapWorld;
    Vec3 offset{};for(unsigned i=0;i<3;++i)offset[i]=pose.position[i]-track.eyes[0].position[i];
    offset=planRotate(plan,offset);
    E::ControllerRay ray{};ray.unitsPerMetre=plan.unitsPerMetre;ray.hand=hand;ray.gripPose=gripPose;
    for(unsigned i=0;i<3;++i){ray.viewOrigin[i]=(plan.world[0][12+i]+plan.world[1][12+i])*.5f+shift[i];if(!std::isfinite(ray.viewOrigin[i]))return MapWorld;}
    ray.right=planRotate(plan,rotate(pose.orientation,{1,0,0}));ray.up=planRotate(plan,rotate(pose.orientation,{0,1,0}));
    ray.direction=planRotate(plan,rotate(pose.orientation,{0,0,-1}));
    const auto length=std::sqrt(dot(ray.direction,ray.direction));
    if(!std::isfinite(length)||length<.99f||length>1.01f)return MapPose;
    for(unsigned i=0;i<3;++i){ray.origin[i]=world[12+i]+offset[i]*plan.unitsPerMetre+shift[i];ray.direction[i]/=length;
        if(!std::isfinite(ray.origin[i]))return MapWorld;}
    ray.sceneId=plan.sceneId;ray.controllerSequence=input.sequence;ray.trackingEpoch=input.epoch;ray.referenceSpaceEpoch=input.referenceSpace;
    out=ray;return Ok;
}

// ---- placement (pure) ----
struct Placement {
    RT::Mat4 attachment{},model{},muzzle{};RT::Vec3 origin{},direction{},grip{};float unitsPerMetre{1};
    RT::Vec3 wrist{},anchorMove{};bool wristAnchored{},boneUsed{}; // VR wrist point, attachment move from the grip anchor
    // HAND hold used (else BARREL) and why HAND was not possible, calibration applied, rigidity before the repair (micro)
    bool hand{},calibrated{};const char* handWhy{""};std::uint32_t rigidErrMicro{};
};
// Where the hand bone goes. Grip = bone origin on the controller grip origin. Wrist = the engine wrist joint (bone
// origin + bone.wrist in the bone frame) on the VR hand mesh's wrist (grip origin - wristM * controller aim forward).
struct AnchorSpec {AnchorMode mode{AnchorMode::Grip};float wristM{.055f};HandBone bone{};
    bool hasForward{};RT::Vec3 forward{}; // the holding controller's RAW aim forward (world) when two-hand aim bent frame.aim
    HoldMode hold{HoldMode::Barrel};HandBone handBone{};unsigned handLocator{};bool calibrate{};calib::Entry calib{};
    bool flipBlade{}; // HAND hold, turn the item 180 deg about the palm normal through the grip (reverse -> forward grip)
};
inline RT::Vec3 axisRotate(RT::Vec3 v,RT::Vec3 axis,float radians) noexcept { // Rodrigues, unit axis
    const float c=std::cos(radians),s=std::sin(radians);const float d=axis[0]*v[0]+axis[1]*v[1]+axis[2]*v[2];
    const RT::Vec3 x{axis[1]*v[2]-axis[2]*v[1],axis[2]*v[0]-axis[0]*v[2],axis[0]*v[1]-axis[1]*v[0]};
    RT::Vec3 r{};for(unsigned i=0;i<3;++i)r[i]=v[i]*c+x[i]*s+axis[i]*d*(1-c);return r;
}
inline void normalize(RT::Vec3& v) noexcept {const float l=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);if(l>1e-6f)for(float& x:v)x/=l;}
inline RT::Vec3 cross3(const RT::Vec3& a,const RT::Vec3& b) noexcept {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
inline float dot3(const RT::Vec3& a,const RT::Vec3& b) noexcept {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}

// Rigidity repair: WeaponPoseCallBoundary only commits the attachment when
// deriveModelMuzzle(A, A) accepts it (columns unit length and orthogonal to 1e-4 after dividing by
// |col0|). An attachment built from the plan rotation (rigid only to 5e-3 in mapRay), the grip / aim rays
// and the authored locator * muzzle (each accepted at 1e-4) can drift past 1e-4 in some orientations; the
// engine then uses its OWN animated attachment for that frame (the gun jumps to the invisible body's hand).
// The final 3x3 is therefore re-orthonormalised in double (Gram-Schmidt, uniform native scale) and checked
// with the boundary's own test before it is returned.
inline std::atomic<std::uint64_t> rigidRepaired{},rigidRejected{},rigidErrMicroMax{};
inline std::uint32_t rigidError(const RT::Mat4& m) noexcept {
    double c[3][3];for(unsigned k=0;k<3;++k)for(unsigned i=0;i<3;++i)c[k][i]=m[k*4+i];
    const double s=std::sqrt(c[0][0]*c[0][0]+c[0][1]*c[0][1]+c[0][2]*c[0][2]);if(!(s>1e-6)||!std::isfinite(s))return UINT32_MAX;
    double e=0;
    for(unsigned a=0;a<3;++a)for(unsigned b=a;b<3;++b){double d=0;for(unsigned i=0;i<3;++i)d+=c[a][i]*c[b][i];d/=s*s;e=std::max(e,std::fabs(a==b?d-1:d));}
    return e*1e6<4e9?std::uint32_t(e*1e6):UINT32_MAX;
}
inline bool rigidize(RT::Mat4& m,float scale) noexcept {
    double c[3][3];for(unsigned k=0;k<3;++k)for(unsigned i=0;i<3;++i)c[k][i]=m[k*4+i];
    const auto norm=[](double (&v)[3]){const double l=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);if(!(l>1e-9)||!std::isfinite(l))return false;for(double& x:v)x/=l;return true;};
    if(!norm(c[0]))return false;
    const double d=c[1][0]*c[0][0]+c[1][1]*c[0][1]+c[1][2]*c[0][2];for(unsigned i=0;i<3;++i)c[1][i]-=d*c[0][i];if(!norm(c[1]))return false;
    const double z[3]{c[0][1]*c[1][2]-c[0][2]*c[1][1],c[0][2]*c[1][0]-c[0][0]*c[1][2],c[0][0]*c[1][1]-c[0][1]*c[1][0]};
    if(z[0]*c[2][0]+z[1]*c[2][1]+z[2]*c[2][2]<=0)return false; // a reflection is not a small error
    for(unsigned i=0;i<3;++i)c[2][i]=z[i];
    for(unsigned k=0;k<3;++k)for(unsigned i=0;i<3;++i)m[k*4+i]=float(c[k][i]*double(scale));
    m[3]=m[7]=m[11]=0;m[15]=1;return true;
}

// HAND hold: the engine hand (attachment bone frame) onto the VR hand by anatomy. Engine: fe = wrist->knuckle, te = the
// thumb side (wrist->thumb base without its fe part), ne = fe x te. VR (HandsMath::handModel): F = aim forward, X = grip +X without
// its F part, Tv = X x F (the mesh's thumb axis), Nv = F x Tv. R = [F Tv Nv][fe te ne]^T; the engine wrist joint lands on the VR mesh
// wrist (grip - wristM * F). Both sides are the SAME hand (left VR hand -> wrist_l / finger_l / thumb_01_l bones), so R is a rotation.
inline std::atomic<std::uint64_t> bladeFlips{}; // Report: vr.weapon.hold.bladeFlips
inline bool placeHand(const E::ControllerWeaponFrame& frame,const AnchorSpec& a,float scale,Placement& p,const char*& why) noexcept {
    const auto& b=a.handBone;
    if(!b.valid){why="NO_BONE";return false;}
    if(!b.anatomy){why="NO_ANATOMY";return false;}
    if(!std::isfinite(a.wristM)||a.wristM<0.f||a.wristM>.3f){why="BAD_WRIST";return false;}
    RT::Vec3 F=frame.aim.direction;normalize(F);RT::Vec3 X=frame.grip.right;{const float k=dot3(X,F);for(unsigned i=0;i<3;++i)X[i]-=k*F[i];}
    if(std::sqrt(dot3(X,X))<.05f){why="DEGENERATE";return false;}
    normalize(X);RT::Vec3 Tv=cross3(X,F);normalize(Tv);const RT::Vec3 Nv=cross3(F,Tv);
    RT::Vec3 fe{},te{};for(unsigned i=0;i<3;++i){fe[i]=b.knuckle[i]-b.wrist[i];te[i]=b.thumb[i]-b.wrist[i];}
    normalize(fe);{const float k=dot3(te,fe);for(unsigned i=0;i<3;++i)te[i]-=k*fe[i];}
    if(std::sqrt(dot3(te,te))<.005f){why="ANATOMY_AXIS";return false;}
    normalize(te);if(config.handRollDeg!=0.f){te=axisRotate(te,fe,config.handRollDeg*3.14159265f/180.f);normalize(te);}
    const RT::Vec3 ne=cross3(fe,te);
    const float s=frame.grip.unitsPerMetre;
    RT::Mat4 A{};A[15]=1;RT::Vec3 Rw{};
    for(unsigned c=0;c<3;++c){RT::Vec3 col{};for(unsigned i=0;i<3;++i)col[i]=F[i]*fe[c]+Tv[i]*te[c]+Nv[i]*ne[c];
        for(unsigned i=0;i<3;++i){A[c*4+i]=col[i]*scale;Rw[i]+=col[i]*b.wrist[c]*s;}}
    for(unsigned i=0;i<3;++i){p.wrist[i]=frame.grip.origin[i]-a.wristM*s*F[i];A[12+i]=p.wrist[i]-Rw[i];
        p.anchorMove[i]=A[12+i]-frame.grip.origin[i];p.grip[i]=frame.grip.origin[i];}
    if(a.flipBlade){ // 180 deg about Nv (palm normal) through the grip centre: v -> 2(n.v)n - v
        const auto turn=[&](RT::Vec3 v){const float k=2.f*dot3(Nv,v);return RT::Vec3{k*Nv[0]-v[0],k*Nv[1]-v[1],k*Nv[2]-v[2]};};
        for(unsigned c=0;c<3;++c){const auto v=turn({A[c*4],A[c*4+1],A[c*4+2]});for(unsigned i=0;i<3;++i)A[c*4+i]=v[i];}
        const auto t=turn({A[12]-frame.grip.origin[0],A[13]-frame.grip.origin[1],A[14]-frame.grip.origin[2]});
        for(unsigned i=0;i<3;++i){A[12+i]=frame.grip.origin[i]+t[i];p.anchorMove[i]=A[12+i]-frame.grip.origin[i];}
        bladeFlips.fetch_add(1,std::memory_order_relaxed);
    }
    p.attachment=A;p.direction=F;p.hand=true;p.wristAnchored=p.boneUsed=true;p.unitsPerMetre=s;
    why="OK";return true;
}

// CALIBRATION: rotation about the grip point (yaw about aim up = +LEFT, then pitch about the turned aim right = +UP, then roll
// about the turned barrel = +CW seen from behind), then the offset along aim right / up / forward. Attachment, barrel and origin move
// together (the laser and the bullets follow the calibrated barrel).
inline void applyCalibration(const E::ControllerWeaponFrame& frame,const calib::Entry& e,Placement& p) noexcept {
    if(e.zero())return;
    constexpr float deg=3.14159265f/180.f;const float s=frame.grip.unitsPerMetre;
    RT::Vec3 f=frame.aim.direction;normalize(f);RT::Vec3 u=frame.aim.up;{const float k=dot3(u,f);for(unsigned i=0;i<3;++i)u[i]-=k*f[i];}normalize(u);
    const RT::Vec3 r=cross3(f,u);const RT::Vec3 pivot=frame.grip.origin;
    const float yaw=e.degrees(calib::Yaw)*deg,pitch=e.degrees(calib::Pitch)*deg,roll=e.degrees(calib::Roll)*deg;
    const RT::Vec3 r1=axisRotate(r,u,yaw);RT::Vec3 f2=axisRotate(axisRotate(f,u,yaw),r1,pitch);normalize(f2);
    const auto rot=[&](RT::Vec3 v){return axisRotate(axisRotate(axisRotate(v,u,yaw),r1,pitch),f2,roll);};
    auto& A=p.attachment;
    for(unsigned c=0;c<3;++c){const auto v=rot({A[c*4],A[c*4+1],A[c*4+2]});for(unsigned i=0;i<3;++i)A[c*4+i]=v[i];}
    {const auto t=rot({A[12]-pivot[0],A[13]-pivot[1],A[14]-pivot[2]});for(unsigned i=0;i<3;++i)A[12+i]=pivot[i]+t[i];}
    p.direction=rot(p.direction);normalize(p.direction);
    for(unsigned i=0;i<3;++i)A[12+i]+=(r[i]*e.metres(calib::OffsetX)+u[i]*e.metres(calib::OffsetY)+f[i]*e.metres(calib::OffsetZ))*s;
    p.calibrated=true;
}

// frame = aim + grip rays of the HOLDING hand in world. BARREL: attachment origin at the grip, authored
// muzzle -Z along the (pitched) aim ray, roll from the (rolled) grip up. HAND: placeHand (falls back to
// BARREL, reason in handWhy). Then the offset (grip frame, metres), the calibration, the rigidity repair.
// model = attachment * locator (weapWeapon::Update), muzzle = model * modelMuzzle.
inline Reason place(E::ControllerWeaponFrame frame,const Geometry& g,bool alt,float scale,const Offset& off,Placement& out,
                    const AnchorSpec& anchor={}) noexcept {
    if(!g.valid)return NoGeometry;
    if(!std::isfinite(scale)||scale<.001f||scale>1000.f)return BadArgument;
    constexpr float deg=3.14159265f/180.f;
    const RT::Vec3 controllerForward=anchor.hasForward?anchor.forward:frame.aim.direction; // The mesh wrist uses the raw aim forward
    if(off.pitchDeg!=0.f){auto& a=frame.aim;a.direction=axisRotate(a.direction,a.right,off.pitchDeg*deg);a.up=axisRotate(a.up,a.right,off.pitchDeg*deg);
        normalize(a.direction);normalize(a.up);}
    if(off.rollDeg!=0.f){auto& gr=frame.grip;gr.up=axisRotate(gr.up,frame.aim.direction,-off.rollDeg*deg);gr.right=axisRotate(gr.right,frame.aim.direction,-off.rollDeg*deg);
        normalize(gr.up);normalize(gr.right);}
    const auto& gr=frame.grip;const float s=frame.grip.unitsPerMetre;
    RT::Vec3 d{};for(unsigned i=0;i<3;++i)d[i]=(gr.right[i]*off.right+gr.up[i]*off.up+gr.direction[i]*off.forward)*s;
    Placement p{};const char* handWhy="";bool hand=false;
    if(anchor.hold==HoldMode::Hand){
        if(!(frame.aim.sceneId&&frame.aim.sceneId==frame.grip.sceneId&&!frame.aim.gripPose&&frame.grip.gripPose&&frame.aim.hand==frame.grip.hand))return Place;
        hand=placeHand(frame,anchor,scale,p,handWhy);}
    const auto& L=g.locator[hand?(anchor.handLocator?1u:0u):(alt?1u:0u)];
    if(!hand){
        const auto attachmentMuzzle=RT::multiply(L,g.modelMuzzle);
        E::TrackedWeaponPose pose{};
        if(!E::prepareTrackedWeaponPose(frame,attachmentMuzzle,scale,pose))return Place;
        p=Placement{};
        if(anchor.mode==AnchorMode::Wrist){ // Engine wrist joint onto the VR mesh wrist
            if(!std::isfinite(anchor.wristM)||anchor.wristM<0.f||anchor.wristM>.3f)return BadArgument;
            RT::Vec3 target{};for(unsigned i=0;i<3;++i){p.wrist[i]=gr.origin[i]-anchor.wristM*s*controllerForward[i];target[i]=p.wrist[i];}
            if(anchor.bone.valid){
                const auto& A=pose.attachmentWorld;
                for(unsigned c=0;c<3;++c){RT::Vec3 axis{A[c*4],A[c*4+1],A[c*4+2]};normalize(axis); // unit bone axes (the attachment keeps the native scale)
                    for(unsigned i=0;i<3;++i)target[i]-=axis[i]*anchor.bone.wrist[c]*s;}
                p.boneUsed=true;
            }
            for(unsigned i=0;i<3;++i)p.anchorMove[i]=target[i]-pose.attachmentWorld[12+i];
            p.wristAnchored=true;
        }
        p.attachment=pose.attachmentWorld;
        for(unsigned i=0;i<3;++i){p.attachment[12+i]+=p.anchorMove[i];p.direction[i]=pose.muzzleRay.direction[i];p.grip[i]=gr.origin[i];}
    }
    p.handWhy=handWhy;
    for(unsigned i=0;i<3;++i)p.attachment[12+i]+=d[i];
    if(anchor.calibrate)applyCalibration(frame,anchor.calib,p);
    // Rigidity repair: exactly what the WeaponPoseCallBoundary accepts.
    p.rigidErrMicro=rigidError(p.attachment);
    if(!rigidize(p.attachment,scale)){rigidRejected.fetch_add(1,std::memory_order_relaxed);return Place;}
    if(p.rigidErrMicro>50u)rigidRepaired.fetch_add(1,std::memory_order_relaxed);
    {auto m=rigidErrMicroMax.load(std::memory_order_relaxed);while(p.rigidErrMicro!=UINT32_MAX&&p.rigidErrMicro>m&&!rigidErrMicroMax.compare_exchange_weak(m,p.rigidErrMicro)){}}
    {RT::Mat4 unit{};float sc{};if(!E::deriveModelMuzzle(p.attachment,p.attachment,unit,sc)){rigidRejected.fetch_add(1,std::memory_order_relaxed);return Place;}}
    p.model=RT::multiply(p.attachment,L);p.muzzle=RT::multiply(p.model,g.modelMuzzle);p.unitsPerMetre=s;
    for(unsigned i=0;i<3;++i)p.origin[i]=p.muzzle[12+i];
    for(float x:p.anchorMove)if(!std::isfinite(x)||std::fabs(x)>.5f*s)return Place;
    for(float x:p.attachment)if(!std::isfinite(x))return Place;
    for(float x:p.model)if(!std::isfinite(x))return Place;
    for(float x:p.direction)if(!std::isfinite(x))return Place;
    out=p;return Ok;
}
// Barrel-line target (free aim point for weapWeapon::Aim).
inline RT::Vec3 aimPoint(const RT::Vec3& origin,const RT::Vec3& direction,float metres,float unitsPerMetre) noexcept {
    RT::Vec3 p{};for(unsigned i=0;i<3;++i)p[i]=origin[i]+direction[i]*metres*unitsPerMetre;return p;
}

// ---- per-call pending model matrix (thread_local: pose prepare -> UpdateSkeleton SetTransform -> pose complete) ----
struct Pending {std::uint64_t serial{};std::uintptr_t weapon{};RT::Mat4 model{};bool valid{},modelUsed{},animated{};
    bool phantom{};RT::Mat4 passed{}; // The phantom's attachment as passed to Update (holster-locator correction)
    bool thrown{};};                  // THROW: the same measurement for the flying (holstered) weapon -> throwFix
// DUAL: a holstered (phantom) item's Update copies attachment * HOLSTER locator into weapon+0x80, not
// attachment * hand locator. After each phantom Update, K = inverse(weapon+0x80) * passed attachment
// (= inverse of the effective locator) is measured and the next frame passes model * K, so the engine ends
// at the intended model. If the model was already the intended one, K = inverse(hand locator) and
// model * K = the attachment (no change).
struct PhantomFix {std::mutex m;std::uintptr_t weapon{};RT::Mat4 k{};bool valid{};};
inline PhantomFix phantomFix;
// THROW (VrThrow.h): the thrown weapon flies through ITS holstered Update too -> the same K, kept apart (a DUAL phantom may be
// re-posed in the other hand at the same time); the thrown item's owner (geometry for the catching-hand pose) copied at the throw.
inline PhantomFix throwFix;
inline std::mutex throwOwnerMutex;inline Owner throwOwner{};
inline Owner throwOwnerSnapshot() {std::lock_guard lock(throwOwnerMutex);return throwOwner;}
inline void setThrowOwner(const Owner& o) {std::lock_guard lock(throwOwnerMutex);throwOwner=o;}
inline std::atomic<std::uint64_t> phantomFixMeasured{},phantomFixApplied{},phantomFixReadFailures{},phantomFixBad{},phantomFixChangedMicro{};
inline bool affineInverse(const RT::Mat4& m,RT::Mat4& out) noexcept { // column-major 3x4 affine
    const double a=m[0],b=m[4],c=m[8],d=m[1],e=m[5],f=m[9],g=m[2],h=m[6],i=m[10];
    const double A=e*i-f*h,B=-(d*i-f*g),C=d*h-e*g,det=a*A+b*B+c*C;
    if(!std::isfinite(det)||std::fabs(det)<1e-12)return false;
    const double inv[9]{A/det,-(b*i-c*h)/det,(b*f-c*e)/det, B/det,(a*i-c*g)/det,-(a*f-c*d)/det, C/det,-(a*h-b*g)/det,(a*e-b*d)/det}; // row-major
    out={};out[15]=1;
    for(unsigned r=0;r<3;++r)for(unsigned col=0;col<3;++col)out[col*4+r]=float(inv[r*3+col]);
    for(unsigned r=0;r<3;++r)out[12+r]=float(-(inv[r*3]*m[12]+inv[r*3+1]*m[13]+inv[r*3+2]*m[14]));
    return true;
}
inline thread_local Pending pending{};
// Latest placement (aim + report).
struct Latest {std::uintptr_t weapon{};std::uint64_t ns{};Placement placement{};RT::Vec3 shift{};bool shifted{};float planAgeMs{},inputAgeMs{};int hand{-1};
    // What the render thread needs to draw the held hand and the laser rigidly with this placement
    RT::Mat4 map{};                 // LOCAL metres -> world used for this placement (plan + anchor shift + prediction [+ turn])
    ::rdrvr::Pose localAim{},localGrip{};std::uint64_t referenceSpace{},epoch{},sequence{}; // raw sample of the physical holding hand
    RT::Vec3 predictedAnchor{},extra{};bool predicted{},turnBridged{};unsigned predictKind{};bool firearm{};
};
inline std::mutex latestMutex;inline Latest latest{};
inline void publishPlacement(const Latest& l) {std::lock_guard lock(latestMutex);latest=l;}
inline Latest latestPlacement() {std::lock_guard lock(latestMutex);return latest;}
inline std::atomic<std::uint64_t> shiftApplied{},shiftSkipped{};
// HOLD LAST: a frame the pipeline cannot place (no plan / input, map refusal, reach, place) while the weapon is in a VR hand
// keeps the last placement (<= 300 ms) instead of handing the weapon back to the engine's animated hand for that frame (a jump to
// the invisible body = "flies in the air"). Context / class refusals (Dead Eye, cover, mounted, ...) stay native on purpose.
inline std::atomic<std::uint64_t> holdLastUsed{},holdLastStale{};inline std::array<std::atomic<std::uint64_t>,ReasonCount> holdLastByReason{};
inline bool holdLastPlacement(std::uintptr_t weapon,Reason why,std::uint64_t now,Latest& out) noexcept {
    if(!config.holdLast)return false;
    switch(why){
    case NoInput:case NoPlan:case MapScene:case MapInput:case MapEpoch:case MapInputStale:case MapPlanStale:case MapPose:case MapWorld:
    case Reach:case BadArgument:case Place:case Exception:break;
    default:return false;}
    const auto l=latestPlacement();
    if(!weapon||l.weapon!=weapon||!l.ns||now<l.ns||now-l.ns>config.holdLastNs){holdLastStale.fetch_add(1,std::memory_order_relaxed);return false;}
    out=l;holdLastUsed.fetch_add(1,std::memory_order_relaxed);holdLastByReason[why].fetch_add(1,std::memory_order_relaxed);return true;
}

// ---- DUAL: the phantom = the second item in the other VR hand ----
// The engine keeps it holstered; its own holstered weapWeapon::Update (same pinned caller ret 0x26770a4)
// gets the placement of the phantom hand from the SAME pipeline as the engine item (computePlacement:
// HAND/BARREL hold mode, per-class/per-weapon calibration, rigidity repair). Its owner / placement live
// apart from `owner` / `latest` (aim, aimFix, laser tag, shot telemetry stay the engine item's).
struct PhantomOwner : Owner {int hand{-1};};
inline std::mutex phantomMutex;inline PhantomOwner phantom{};
inline PhantomOwner phantomSnapshot() {std::lock_guard lock(phantomMutex);return phantom;}
inline void clearPhantomOwner() {std::lock_guard lock(phantomMutex);phantom=PhantomOwner{};}
inline std::atomic<std::uint64_t> phantomObserved{},phantomCalls{},phantomPlaced{},phantomHoldLast{},phantomGeometryRefreshes{},phantomGeometryFailures{},
    phantomModels{},phantomTags{},phantomLaserTags{},phantomAdopted{};
inline std::array<std::atomic<std::uint64_t>,ReasonCount> phantomDeclines{};
inline std::atomic<int> phantomLastHand{-1};inline std::atomic<const char*> phantomGeometryState{"NONE"};inline std::atomic<std::uint64_t> phantomLastNs{};
inline bool phantomDecline(Reason why,std::uint64_t now) noexcept {phantomDeclines[why].fetch_add(1,std::memory_order_relaxed);(void)now;return false;}
// Main thread (vrWeaponObserve, after publishOwner): w = the phantom item's weapon (type read by the caller), context = the player's.
template<class GeometryFn>
inline void publishPhantom(const E::LocalWeapon& w,const E::WeaponContext& context,int hand,std::uint64_t now,GeometryFn&& geometryFn) {
    PhantomOwner next=phantomSnapshot();
    if(next.weapon!=w.weapon){next=PhantomOwner{};}
    next.weapon=w.weapon;next.actor=w.actor;next.item=w.item;next.actorGuid=w.actorGuid;next.kind=w.type.kind;next.typeId=w.type.id;next.stampNs=now;
    next.contextFlags=context.flags();next.hand=hand;
    const auto g=gates(context,w.type.kind,config,now);next.poseAllowed=g.pose;next.aimAllowed=false;next.forceAllowed=false; // never aims / fires
    static thread_local unsigned ordinal{};
    if(!next.geometry.valid||next.geometry.weapon!=w.weapon||++ordinal%30u==0){
        Geometry fresh{};
        if(geometryFn(next.geometry,fresh)){
            if(next.geometry.weapon==w.weapon)for(unsigned h=0;h<2;++h)if(!fresh.bone[h].valid&&next.geometry.bone[h].valid)fresh.bone[h]=next.geometry.bone[h];
            fresh.stampNs=now;next.geometry=fresh;phantomGeometryRefreshes.fetch_add(1,std::memory_order_relaxed);}
        else{phantomGeometryFailures.fetch_add(1,std::memory_order_relaxed);if(next.geometry.weapon!=w.weapon)next.geometry=Geometry{};}
    }
    {std::lock_guard lock(phantomMutex);phantom=next;}
    phantomObserved.fetch_add(1,std::memory_order_relaxed);
}
// The phantom's placement (pose hook, any thread) -> tagged per head sample for the hands pass (held hand + laser of a phantom firearm).
inline std::mutex phantomLatestMutex;inline Latest phantomLatest{};
inline void publishPhantomPlacement(const Latest& l) {std::lock_guard lock(phantomLatestMutex);phantomLatest=l;}
inline Latest phantomPlacement() {std::lock_guard lock(phantomLatestMutex);return phantomLatest;}
inline bool phantomHoldLastPlacement(std::uintptr_t weapon,Reason why,std::uint64_t now,Latest& out) noexcept { // HOLD LAST, same rules
    if(!config.holdLast)return false;
    switch(why){
    case NoInput:case NoPlan:case MapScene:case MapInput:case MapEpoch:case MapInputStale:case MapPlanStale:case MapPose:case MapWorld:
    case Reach:case BadArgument:case Place:case Exception:break;
    default:return false;}
    const auto l=phantomPlacement();
    if(!weapon||l.weapon!=weapon||!l.ns||now<l.ns||now-l.ns>config.holdLastNs)return false;
    out=l;phantomHoldLast.fetch_add(1,std::memory_order_relaxed);return true;
}
// Pose-hook target (any thread, every weapWeapon::Update): the phantom FIRST - during a swap the old engine item is already the
// published phantom while the owner snapshot still names it - then the engine item. Lock-free (atomics only) for every other weapon.
// THROW: the thrown weapon (flying / lying / being caught / flying back) is checked before everything else.
enum class PoseTarget : std::uint8_t {None,Owner,Phantom,Thrown};
inline PoseTarget poseTarget(std::uintptr_t callWeapon,std::uintptr_t ownerWeapon) noexcept {
    namespace H=::rdrvr::vr::holsters;
    if(!callWeapon)return PoseTarget::None;
    if(::rdrvr::vr::throwing::poseState(callWeapon)!=::rdrvr::vr::throwing::StateIdle)return PoseTarget::Thrown;
    if(H::phantomWeapon.load(std::memory_order_acquire)==callWeapon&&H::phantomHand.load(std::memory_order_acquire)>=0)return PoseTarget::Phantom;
    return ownerWeapon==callWeapon?PoseTarget::Owner:PoseTarget::None;
}

// =====================================================================================================================
// Grip anchor from the skeleton, walking/turning lag, render tagging, barrel-exact shots.
// =====================================================================================================================
inline float len3(const RT::Vec3& v) noexcept {return std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);}
inline bool lowerEquals(const char* a,const char* b) noexcept {
    for(;*a&&*b;++a,++b){char x=*a,y=*b;if(x>='A'&&x<='Z')x=char(x-'A'+'a');if(y>='A'&&y<='Z')y=char(y-'A'+'a');if(x!=y)return false;}
    return !*a&&!*b;
}
// ---- hand bone: the engine wrist joint in the frame of the weapon's attachment bone (actor skeleton, guarded reads) ----
// Chain = entEntityComponent::GetSkeleton -> entEntityInst::GetSkeleton -> fragInst::GetSkeleton:
// actor+0x50 entity (vtable == entityVtable) -> +0x88 inst (vtable slot +0x1f0 == instSkeletonFn, frag flag +0x190) -> +0x88 cache
// -> +0x1a0 crSkeleton: +0x08 crSkeletonData (+0x00 crBoneData[] stride 0x110: +0x00 name, +0x2a bone id; +0x30 count), +0x28 globals
// (stride 0x40, rows a/b/c/d). The attachment bone = the bone whose id == item+0x24 (alt item+0x28; invBaseItem::SetBoneIDs
// 0x2660af8: FindBoneData(name)->+0x2a), else by name wrist_r_attachment / wrist_l_attachment; the wrist = "wrist_r" / "wrist_l".
inline std::atomic<const char*> handBoneState{"NOT_RUN"};inline std::atomic<std::uint64_t> handBoneRuns{},handBoneOk{};
inline std::atomic<const char*> anatomyState{"NOT_RUN"};inline std::atomic<std::uint64_t> anatomyOk{};
// Bone id -> skeleton index exactly as invWeapon::OnUpdatePosition 0x2676f2c..0x2676fc4 does it: crSkeletonData+0x38 bit1
// set = binary search of the (u16 id, u16 index) map at +0x40 (count u16 +0x48); clear = the id IS the index (< count +0x30).
// The global matrix of index i is globals + (crBoneData[i]+0x28 u16) * 0x40 (0x2676ff8..0x2677004), not globals + i * 0x40.
template<class Read>
inline int boneIndexOfId(Read&& read,std::uintptr_t data,std::uint16_t count,std::uint32_t id,std::uint8_t& how) noexcept {
    const auto get=[&](std::uintptr_t p,std::size_t o,auto& v){return p&&p<=UINTPTR_MAX-o-sizeof(v)&&read(p+o,&v,sizeof v);};
    how=0;std::uint8_t flags{};if(!get(data,0x38,flags))return -1;
    const std::uint32_t key=id&0xffffu;
    if(!(flags&2u)){if(key<count){how=2;return int(key);}return -1;}
    std::uintptr_t map{};std::uint16_t n{};if(!get(data,0x40,map)||!map||!get(data,0x48,n)||!n)return -1;
    int lo=0,hi=int(n)-1;
    for(int guard=0;lo<=hi&&guard<20;++guard){const int mid=(lo+hi)>>1;std::uint16_t e[2]{};
        if(!read(map+std::uintptr_t(mid)*4,e,sizeof e))return -1;
        if(e[0]==key){if(e[1]>=count)return -1;how=1;return int(e[1]);}
        if(e[0]>key)hi=mid-1;else lo=mid+1;}
    return -1;
}
template<class Read>
inline bool deriveHandBone(Read&& read,std::uintptr_t actor,std::uintptr_t entityVtable,std::uintptr_t instSkeletonFn,std::uint32_t boneId,bool alt,
                           HandBone& out,const char*& why) noexcept {
    out=HandBone{};why="NO_API";
    if(!entityVtable||!instSkeletonFn)return false;
    why="NO_ACTOR";if(!actor)return false;
    const auto get=[&](std::uintptr_t p,std::size_t o,auto& v){return p&&p<=UINTPTR_MAX-o-sizeof(v)&&read(p+o,&v,sizeof v);};
    std::uintptr_t ent{},vt{},inst{},slot{},cache{},skel{},data{},globals{},bones{};std::uint8_t frag{};std::uint16_t count{};
    if(!get(actor,0x50,ent)||!ent||!get(ent,0,vt)||vt!=entityVtable){why="ENTITY";return false;}
    if(!get(ent,0x88,inst)||!inst||!get(inst,0,vt)||!vt||!get(vt,0x1f0,slot)||slot!=instSkeletonFn){why="INSTANCE";return false;}
    if(!get(inst,0x190,frag)||!frag||!get(inst,0x88,cache)||!cache||!get(cache,0x1a0,skel)||!skel){why="SKELETON";return false;}
    if(!get(skel,8,data)||!data||!get(skel,0x28,globals)||!globals||!get(data,0x30,count)||!count||count>512||!get(data,0,bones)||!bones){
        why="SKELETON_DATA";return false;}
    const char* wristName=alt?"wrist_l":"wrist_r";const char* attachName=alt?"wrist_l_attachment":"wrist_r_attachment";
    // Hand anatomy (RDR1 mitten hands: finger_r = the fingers' base, FINGER_12_R / finger_23_r further out; THUMB_01_R).
    static constexpr const char* knuckleR[]{"finger_r","finger_12_r","finger_23_r"};static constexpr const char* knuckleL[]{"finger_l","finger_12_l","finger_23_l"};
    static constexpr const char* thumbR[]{"thumb_01_r","thumb_r","thumb_12_r"};static constexpr const char* thumbL[]{"thumb_01_l","thumb_l","thumb_12_l"};
    const auto& knuckleNames=alt?knuckleL:knuckleR;const auto& thumbNames=alt?thumbL:thumbR;
    std::uint8_t how{};int attach=boneId?boneIndexOfId(read,data,count,boneId,how):-1;
    int byName=-1,wrist=-1,knuckle=-1,thumb=-1,knuckleRank=9,thumbRank=9;const unsigned n=std::min<unsigned>(count,256u);
    for(unsigned i=0;i<n;++i){
        const auto bd=bones+std::uintptr_t(i)*0x110;std::uintptr_t name{};
        if(!get(bd,0,name))continue;
        char text[32]{};
        if(!name||!(read(name,text,24)||read(name,text,8)))continue;
        text[24]=0;
        if(lowerEquals(text,wristName))wrist=int(i);else if(lowerEquals(text,attachName))byName=int(i);
        for(int r=0;r<3;++r){if(r<knuckleRank&&lowerEquals(text,knuckleNames[r])){knuckle=int(i);knuckleRank=r;}
                             if(r<thumbRank&&lowerEquals(text,thumbNames[r])){thumb=int(i);thumbRank=r;}}
    }
    if(attach<0&&byName>=0){attach=byName;how=3;}
    if(attach<0){why="NO_ATTACH_BONE";return false;}
    if(wrist<0){why="NO_WRIST_BONE";return false;}
    const auto global=[&](int index,float (&m)[16]){std::uint16_t g{};
        return get(bones+std::uintptr_t(index)*0x110,0x28,g)&&g<count&&read(globals+std::uintptr_t(g)*0x40,m,sizeof m);};
    float a[16]{},w[16]{};
    if(!global(attach,a)||!global(wrist,w)){why="GLOBALS";return false;}
    for(unsigned c=0;c<4;++c)for(unsigned r=0;r<3;++r)if(!std::isfinite(a[c*4+r])||!std::isfinite(w[c*4+r])){why="NOT_FINITE";return false;}
    RT::Vec3 axis[3]{};for(unsigned c=0;c<3;++c){axis[c]={a[c*4],a[c*4+1],a[c*4+2]};const float l=len3(axis[c]);
        if(!(l>.5f&&l<2.f)){why="NOT_RIGID";return false;}for(float& x:axis[c])x/=l;}
    for(unsigned i=0;i<3;++i)for(unsigned j=i+1;j<3;++j)if(std::fabs(axis[i][0]*axis[j][0]+axis[i][1]*axis[j][1]+axis[i][2]*axis[j][2])>.05f){why="NOT_RIGID";return false;}
    const auto local=[&](const float (&m)[16]){const RT::Vec3 d{m[12]-a[12],m[13]-a[13],m[14]-a[14]};RT::Vec3 r{};
        for(unsigned c=0;c<3;++c)r[c]=axis[c][0]*d[0]+axis[c][1]*d[1]+axis[c][2]*d[2];return r;};
    HandBone hb{};hb.wrist=local(w);
    if(!(len3(hb.wrist)<=.2f)){why="TOO_FAR";return false;}
    hb.valid=true;hb.attach=attach;hb.wristIndex=wrist;hb.boneId=boneId;hb.found=how;
    // Anatomy: knuckle 3..16 cm and thumb 1..14 cm from the wrist, the thumb off the wrist->knuckle line (> 20 deg).
    const char* anatomy="NO_FINGER_BONE";
    if(knuckle>=0&&thumb<0)anatomy="NO_THUMB_BONE";
    if(knuckle>=0&&thumb>=0){float k[16]{},t[16]{};anatomy="GLOBALS";
        if(global(knuckle,k)&&global(thumb,t)){
            hb.knuckle=local(k);hb.thumb=local(t);RT::Vec3 f{},s{};
            for(unsigned i=0;i<3;++i){f[i]=hb.knuckle[i]-hb.wrist[i];s[i]=hb.thumb[i]-hb.wrist[i];}
            const float lf=len3(f),ls=len3(s);anatomy="PROPORTIONS";
            if(std::isfinite(lf)&&std::isfinite(ls)&&lf>=.03f&&lf<=.16f&&ls>=.01f&&ls<=.14f){
                const float c=(f[0]*s[0]+f[1]*s[1]+f[2]*s[2])/(lf*ls);
                if(c<std::cos(20.f*3.14159265f/180.f)){hb.anatomy=true;hb.knuckleIndex=knuckle;hb.thumbIndex=thumb;anatomy="OK";}
                else anatomy="THUMB_ON_AXIS";
            }
        }}
    anatomyState.store(anatomy,std::memory_order_relaxed);if(hb.anatomy)anatomyOk.fetch_add(1,std::memory_order_relaxed);
    out=hb;why="OK";return true;
}
// Main thread (geometry refresh): both hands, cached per (skeleton data, bone ids) - the wrist/attachment relation is rigid.
template<class Read>
inline void refreshHandBones(Read&& read,std::uintptr_t actor,std::uintptr_t item,std::uintptr_t entityVtable,std::uintptr_t instSkeletonFn,
                             std::array<HandBone,2>& out) noexcept {
    const auto get=[&](std::uintptr_t p,std::size_t o,auto& v){return p&&p<=UINTPTR_MAX-o-sizeof(v)&&read(p+o,&v,sizeof v);};
    std::uint32_t ids[2]{};if(item){get(item,0x24,ids[0]);get(item,0x28,ids[1]);}
    // The walk reads every bone name (~100+ guarded reads): once per (actor, item, bone ids); a failure retries after 300 calls.
    struct Cache {std::uintptr_t actor{},item{};std::uint32_t ids[2]{};std::array<HandBone,2> bone{};bool ok{};unsigned age{};};
    static thread_local Cache cache{};
    if(cache.actor==actor&&cache.item==item&&cache.ids[0]==ids[0]&&cache.ids[1]==ids[1]&&(cache.ok||++cache.age<300u)){
        for(unsigned h=0;h<2;++h)if(cache.bone[h].valid)out[h]=cache.bone[h];
        return;}
    handBoneRuns.fetch_add(1,std::memory_order_relaxed);
    const char* why="NO_ITEM";bool any=false;
    for(unsigned h=0;h<2;++h){HandBone b{};const char* w{};
        if(deriveHandBone(read,actor,entityVtable,instSkeletonFn,ids[h]&0xffffu,h==1,b,w)){out[h]=b;any=true;}
        if(h==0||!any)why=w;}
    if(any)handBoneOk.fetch_add(1,std::memory_order_relaxed);
    handBoneState.store(why,std::memory_order_relaxed);
    cache=Cache{};cache.actor=actor;cache.item=item;cache.ids[0]=ids[0];cache.ids[1]=ids[1];cache.bone=out;cache.ok=any;
}

// ---- walking lag: capsule read at pose time + prediction of this frame's anchor ----
// Pose runs ~2 ms BEFORE PostCameraUpdate of the same frame (weaponCallTrace: Pose/PoseComplete with reticle ordinal k,
// the Reticle record k+1 2 ms later), i.e. after the physics step: the capsule already holds the position the eyes' anchor of
// this frame will be built from (VrCamera BODY: P = capsule + up * eyeHeight). Newest anchor (frame N-1) + capsule motion since
// then = anchor N. If the capsule did not move while the mover velocity says it should (the update ran before physics), the
// velocity * frame interval is used instead (counted as capsuleStill).
template<class Read>
inline bool readCapsule(Read&& read,std::uintptr_t actor,RT::Vec3& out) noexcept {
    std::uintptr_t comp{},matrix{};float p[3]{};
    const auto get=[&](std::uintptr_t b,std::size_t o,auto& v){return b&&b<=UINTPTR_MAX-o-sizeof(v)&&read(b+o,&v,sizeof v);};
    if(!get(actor,0xb0,comp)||!comp||!get(comp,0x18,matrix)||!matrix||!get(matrix,0x30,p))return false;
    for(float x:p)if(!std::isfinite(x))return false;
    out={p[0],p[1],p[2]};return true;
}
enum PredictKind : unsigned {PredictNone,PredictCapsule,PredictVelocity,PredictKindCount};
inline constexpr std::array<const char*,PredictKindCount> predictNames{"none","capsule","velocity"};
inline std::array<std::atomic<std::uint64_t>,PredictKindCount> predictKinds{};
inline std::atomic<std::uint64_t> capsuleReads{},capsuleFails{},capsuleStill{},predictRejected{};
inline std::atomic<std::uint64_t> predictWarpBridged{};
// TELEPORT warp bridge: a capsule jump > 1 m is a teleport and is normally rejected (one frame mapped
// through the pre-warp plan = stretched arms / the gun 10 m away, visible in INSTANT). It is accepted as
// PredictCapsule when the mod's own warp was noted <= 400 ms ago and the jump matches its delta within
// 0.5 m (the camera's B snap moves the eye, not the capsule: it is not part of e).
inline bool warpBridge(const RT::Vec3& e,std::uint64_t now,float upm) noexcept {
    if(!now)return false;
    tploco::warp::Note n{};if(!tploco::warp::note.load(n)||!n.serial||now<n.ns||now-n.ns>400'000'000ull)return false;
    float d2=0.f;for(unsigned i=0;i<3;++i){const float d=e[i]-n.delta[i];d2+=d*d;}
    if(std::isfinite(d2)&&d2<(.5f*upm)*(.5f*upm)){predictWarpBridged.fetch_add(1,std::memory_order_relaxed);tploco::linkBump(tploco::warp::weaponBridged);return true;}
    tploco::linkBump(tploco::warp::weaponBridgeRejected);return false;
}
inline unsigned predictExtra(const AnchorRecord& newest,const RT::Vec3* capsuleNow,std::uint64_t intervalNs,float unitsPerMetre,Predict mode,
                             RT::Vec3& extra,std::uint64_t now=0) noexcept {
    extra={};if(mode==Predict::Off)return PredictNone;
    const float dt=float(std::clamp<std::uint64_t>(intervalNs?intervalNs:28'000'000ull,5'000'000ull,60'000'000ull))/1e9f;
    RT::Vec3 v{};for(unsigned i=0;i<3;++i)v[i]=newest.velocity[i]*dt;
    const float vlen=newest.velocityValid?len3(v):0.f,upm=unitsPerMetre>0.f?unitsPerMetre:1.f;
    const bool velocityOk=newest.velocityValid&&std::isfinite(vlen)&&vlen<1.f*upm;
    if(mode==Predict::Capsule&&capsuleNow&&newest.capsuleValid){
        RT::Vec3 e{};for(unsigned i=0;i<3;++i)e[i]=(*capsuleNow)[i]-newest.capsule[i];
        const float elen=len3(e);
        if(std::isfinite(elen)&&elen>1.f*upm&&warpBridge(e,now,upm)){extra=e;return PredictCapsule;}
        if(!std::isfinite(elen)||elen>1.f*upm){predictRejected.fetch_add(1,std::memory_order_relaxed);return PredictNone;} // teleport
        if(velocityOk&&vlen>.002f*upm&&elen<.2f*vlen){capsuleStill.fetch_add(1,std::memory_order_relaxed);extra=v;return PredictVelocity;}
        extra=e;return PredictCapsule;
    }
    if(velocityOk&&mode!=Predict::Off){extra=v;return PredictVelocity;}
    return PredictNone;
}

// ---- plan helpers ----
// LOCAL metres -> world of a plan exactly as mapRay maps a pose (shift added): world = world0.t + shift + R*(x - eye0)*s.
inline RT::Mat4 planMap(const E::StereoCameraPlan& plan,const RT::Vec3& shift={}) noexcept {
    const float s=plan.unitsPerMetre;RT::Mat4 m{};
    for(unsigned c=0;c<3;++c){RT::Vec3 e{};e[c]=1.f;const auto r=planRotate(plan,e);for(unsigned i=0;i<3;++i)m[c*4+i]=r[i]*s;}
    const auto& eye=plan.tracking.eyes[0].position;const auto re=planRotate(plan,{eye[0],eye[1],eye[2]});
    for(unsigned i=0;i<3;++i)m[12+i]=plan.world[0][12+i]+shift[i]-re[i]*s;
    m[15]=1;return m;
}
inline RT::Vec3 mapPoint(const RT::Mat4& m,const RT::Vec3& x) noexcept {
    return {m[0]*x[0]+m[4]*x[1]+m[8]*x[2]+m[12],m[1]*x[0]+m[5]*x[1]+m[9]*x[2]+m[13],m[2]*x[0]+m[6]*x[1]+m[10]*x[2]+m[14]};}
inline void shiftPlan(E::StereoCameraPlan& plan,const RT::Vec3& shift) noexcept {for(auto& w:plan.world)for(unsigned i=0;i<3;++i)w[12+i]+=shift[i];}
// Both eyes' world matrices rotated about `up` through `pivot` (right-hand rule): a native heading change of the base camera.
inline void rotatePlan(E::StereoCameraPlan& plan,const RT::Vec3& pivot,RT::Vec3 up,float radians) noexcept {
    normalize(up);
    for(auto& w:plan.world){
        for(unsigned c=0;c<3;++c){const auto r=axisRotate({w[c*4],w[c*4+1],w[c*4+2]},up,radians);for(unsigned i=0;i<3;++i)w[c*4+i]=r[i];}
        const auto t=axisRotate({w[12]-pivot[0],w[13]-pivot[1],w[14]-pivot[2]},up,radians);for(unsigned i=0;i<3;++i)w[12+i]=pivot[i]+t[i];
    }
}
// Signed heading change (radians, right-hand about up) of plan b relative to plan a (LOCAL forward through each plan's map).
inline float headingDelta(const E::StereoCameraPlan& a,const E::StereoCameraPlan& b,RT::Vec3 up) noexcept {
    normalize(up);auto fa=planRotate(a,{0,0,-1}),fb=planRotate(b,{0,0,-1});
    for(auto* f:{&fa,&fb}){const float d=(*f)[0]*up[0]+(*f)[1]*up[1]+(*f)[2]*up[2];for(unsigned i=0;i<3;++i)(*f)[i]-=d*up[i];normalize(*f);}
    const RT::Vec3 c{fa[1]*fb[2]-fa[2]*fb[1],fa[2]*fb[0]-fa[0]*fb[2],fa[0]*fb[1]-fa[1]*fb[0]};
    return std::atan2(c[0]*up[0]+c[1]*up[1]+c[2]*up[2],fa[0]*fb[0]+fa[1]*fb[1]+fa[2]*fb[2]);
}

// ---- snap / teleport-view turn bridge ----
// A native turn invalidates ControllerAimSpace until a plan sampled after it is rendered (~2 frames). For those frames the
// last plan before the turn is rotated by the turn about the body anchor. The engine's sign of `radians` is learned from
// the first plan after a turn.
struct TurnState {std::uint64_t ns{};float radians{};bool pending{},verified{},haveBefore{};E::StereoCameraPlan before{};};
inline std::mutex turnMutex;inline TurnState turnState{};inline E::StereoCameraPlan lastPlan{};inline bool haveLastPlan{};
inline std::atomic<int> learnedSign{0};
inline std::atomic<std::uint64_t> turnsNoted{},turnBridged{},turnVerified{},turnSignFlips{},turnNoBefore{},turnExpired{},turnMeasureRejected{};
inline unsigned turnContradictions{};
inline std::atomic<float> turnLastDeg{},turnMeasuredDeg{};
inline constexpr std::uint64_t turnWindowNs=500'000'000;
inline int turnSign() noexcept {const int s=config.turnSign?config.turnSign:learnedSign.load(std::memory_order_relaxed);return s?s:1;}
// Main thread: the native camera turn was applied (SnapTurn / teleport view).
inline void noteTurn(float radians,std::uint64_t ns) noexcept {
    if(!std::isfinite(radians)||radians==0.f)return;
    turnsNoted.fetch_add(1,std::memory_order_relaxed);turnLastDeg.store(radians*57.29578f,std::memory_order_relaxed);
    std::lock_guard lock(turnMutex);
    turnState.ns=ns;turnState.radians=radians;turnState.pending=true;turnState.verified=false;turnState.haveBefore=haveLastPlan;
    if(haveLastPlan)turnState.before=lastPlan;else turnNoBefore.fetch_add(1,std::memory_order_relaxed);
}
// Any thread (pose): a real rendered plan. Keeps it for the bridge; the first plan sampled after a turn measures the turn's sign.
inline void notePlan(const E::StereoCameraPlan& plan,const RT::Vec3& up) noexcept {
    std::lock_guard lock(turnMutex);
    if(turnState.pending&&turnState.haveBefore&&plan.headSampledNs>turnState.ns){
        const float measured=headingDelta(turnState.before,plan,up);turnMeasuredDeg.store(measured*57.29578f,std::memory_order_relaxed);
        // A wrong learned sign shows as a hands residual of twice the turn (60 deg for +-30 deg turns). A
        // measurement counts only when its size matches the turn (0.6..1.4) - body/camera heading drift between the pre-turn plan
        // and this one must not decide the sign - and a learned sign flips only after two agreeing contradictions in a row.
        const float ratio=std::fabs(turnState.radians)>1e-4f?std::fabs(measured)/std::fabs(turnState.radians):0.f;
        if(ratio>=.6f&&ratio<=1.4f){
            const int s=measured*turnState.radians>0.f?1:-1;const int prev=learnedSign.load(std::memory_order_relaxed);
            if(!prev||prev==s){learnedSign.store(s,std::memory_order_relaxed);turnContradictions=0;}
            else if(++turnContradictions>=2){learnedSign.store(s,std::memory_order_relaxed);turnSignFlips.fetch_add(1,std::memory_order_relaxed);turnContradictions=0;}
            turnVerified.fetch_add(1,std::memory_order_relaxed);
        }else turnMeasureRejected.fetch_add(1,std::memory_order_relaxed);
        turnState.pending=false;turnState.verified=true;
    }
    lastPlan=plan;haveLastPlan=true; // ControllerAimSpace serves no pre-turn plan once a turn is in flight (afterNativeTurn)
}
// Any thread (pose, no current plan): the pre-turn plan + the signed turn angle; false = no turn in flight.
inline bool turnBefore(std::uint64_t now,E::StereoCameraPlan& out,float& radians) noexcept {
    if(!config.turnBridge)return false;
    std::lock_guard lock(turnMutex);
    if(!turnState.pending||!turnState.haveBefore)return false;
    if(now<turnState.ns||now-turnState.ns>turnWindowNs){turnState.pending=false;turnExpired.fetch_add(1,std::memory_order_relaxed);return false;}
    out=turnState.before;radians=float(turnSign())*turnState.radians;return true;
}

inline constexpr std::array<const char*,6> handWhyNames{"NO_BONE","NO_ANATOMY","BAD_WRIST","DEGENERATE","ANATOMY_AXIS","OTHER"};
inline std::atomic<std::uint64_t> modeBarrel{},modeHand{},calibApplied{};inline std::array<std::atomic<std::uint64_t>,6> handFallbacks{};
inline void notePlacementMode(const Owner& o,const AnchorSpec& a,const Placement& p) noexcept {
    (p.hand?modeHand:modeBarrel).fetch_add(1,std::memory_order_relaxed);if(p.calibrated)calibApplied.fetch_add(1,std::memory_order_relaxed);
    if(a.hold==HoldMode::Hand&&!p.hand){unsigned i=0;while(i+1<handWhyNames.size()&&std::strcmp(handWhyNames[i],p.handWhy?p.handWhy:"")!=0)++i;
        handFallbacks[i].fetch_add(1,std::memory_order_relaxed);}
    std::lock_guard lock(classSeenMutex);auto& c=classSeen[unsigned(o.kind)%classCount];
    c.seen=true;c.typeId=o.typeId;++c.poses;if(p.hand)++c.handPoses;c.hand=p.hand;c.hold=a.hold;c.handWhy=p.handWhy?p.handWhy:"";c.calibrated=p.calibrated;
    c.anchorMove=p.anchorMove;c.rigidErrMicro=p.rigidErrMicro;c.lastNs=::rdrvr::vr::nowNs();
}
// Firearms held by the barrel: where would the HAND hold point the barrel? (a check of the anatomy mapping:
// a revolver held by HAND should point within ~20 deg of the controller aim.) Every 4th pose.
inline std::atomic<std::uint64_t> handCompareFrames{},handCompareMdegSum{},handCompareMdegMax{},handCompareMmSum{};
inline std::atomic<float> handCompareLastDeg{-1.f};
// The HAND hold's barrel relative to the BARREL hold in the aim frame (+pitch = up, +yaw = left, +roll = CW, calibration
// convention): a HAND weapon needs about minus these to point like the revolver does with BARREL.
inline std::atomic<float> handComparePitch{},handCompareYaw{},handCompareRoll{};
template<class Request>
inline void compareHandHold(const E::ControllerWeaponFrame& frame,const Owner& o,const AnchorSpec& a,const Request& r,const Placement& barrel) noexcept {
    static std::atomic<unsigned> ordinal{};if(ordinal.fetch_add(1,std::memory_order_relaxed)%4u)return;
    AnchorSpec h=a;h.hold=HoldMode::Hand;h.calibrate=false;Placement p{};
    if(place(frame,o.geometry,r.alt,r.scale,{},p,h)!=Ok||!p.hand)return;
    RT::Vec3 z{-p.muzzle[8],-p.muzzle[9],-p.muzzle[10]};normalize(z);
    const float deg=std::acos(std::clamp(dot3(z,barrel.direction),-1.f,1.f))*57.29578f;
    RT::Vec3 d{};for(unsigned i=0;i<3;++i)d[i]=p.attachment[12+i]-barrel.attachment[12+i];
    const float mm=len3(d)/(p.unitsPerMetre>0.f?p.unitsPerMetre:1.f)*1000.f;
    if(!std::isfinite(deg)||!std::isfinite(mm))return;
    { // decomposition in the barrel placement's aim frame
        RT::Vec3 f=barrel.direction;normalize(f);RT::Vec3 u=frame.aim.up;{const float k=dot3(u,f);for(unsigned i=0;i<3;++i)u[i]-=k*f[i];}normalize(u);const RT::Vec3 side=cross3(f,u);
        handComparePitch.store(std::asin(std::clamp(dot3(z,u),-1.f,1.f))*57.29578f,std::memory_order_relaxed);
        handCompareYaw.store(std::atan2(-dot3(z,side),dot3(z,f))*57.29578f,std::memory_order_relaxed);
        RT::Vec3 yb{barrel.muzzle[4],barrel.muzzle[5],barrel.muzzle[6]},yh{p.muzzle[4],p.muzzle[5],p.muzzle[6]};normalize(yb);normalize(yh);
        handCompareRoll.store(std::atan2(dot3(cross3(yb,yh),f),dot3(yb,yh))*57.29578f,std::memory_order_relaxed);
    }
    handCompareFrames.fetch_add(1,std::memory_order_relaxed);const auto md=std::uint64_t(deg*1000.f);handCompareMdegSum.fetch_add(md,std::memory_order_relaxed);
    handCompareMmSum.fetch_add(std::uint64_t(std::min(mm,1e6f)),std::memory_order_relaxed);handCompareLastDeg.store(deg,std::memory_order_relaxed);
    auto m=handCompareMdegMax.load(std::memory_order_relaxed);while(md>m&&!handCompareMdegMax.compare_exchange_weak(m,md)){}
}

// ---- placement pipeline (any thread; GameXr vrWeaponPose supplies the engine inputs) ----
struct PoseRequest {
    const E::StereoCameraPlan* plan{};          // nullptr: ControllerAimSpace has none (native turn in flight)
    RT::ControllerSample raw{},input{};         // raw sample / after holsters::weaponInput (logical hand 1 = the holding hand)
    int hold{-1};bool alt{};float scale{1};const Owner* owner{};
    bool capsuleValid{};RT::Vec3 capsule{};std::uint64_t now{};
};
// BODY: the plan part of computePlacement (plan / turn bridge, anchor shift, this frame's capsule prediction) as its own
// step, so the BODY arm IK (VrBody.h, GameXr vrBodyFrame) maps the controllers with exactly the transform the weapon uses.
// `plan` = the shifted (and bridged) plan: mapRay(plan, ..., shift {}) gives the weapon's world poses.
struct PlanPrep {RT::Vec3 total{},extra{},pivot{};bool shifted{},bridged{};unsigned kind{PredictNone};AnchorRecord latest{};bool haveLatest{};};
inline Reason preparePlan(const PoseRequest& r,E::StereoCameraPlan& plan,PlanPrep& out) noexcept {
    const Config& cfg=config;
    out=PlanPrep{};plan=E::StereoCameraPlan{};float turn=0.f;bool bridged=false;
    AnchorRecord pa{},la{},prev{};bool haveLatest=false;
    {RT::Vec3 up{0,1,0};if(anchors.newest(la)){haveLatest=true;up=la.up;}
     if(r.plan){plan=*r.plan;notePlan(plan,up);}
     else if(turnBefore(r.now,plan,turn))bridged=true;
     else return NoPlan;}
    RT::Vec3 shift{},extra{};bool shifted=false;unsigned kind=PredictNone;
    if(cfg.latency){
        shifted=haveLatest&&anchors.find(plan.headSampledNs,pa)&&anchorShift(plan,pa,la,shift);
        (shifted?shiftApplied:shiftSkipped).fetch_add(1,std::memory_order_relaxed);
        if(shifted){
            std::uint64_t interval=0;
            if(anchors.previous(la.sampledNs,prev)&&la.sampledNs>prev.sampledNs)interval=la.sampledNs-prev.sampledNs; // frame interval
            kind=predictExtra(la,r.capsuleValid?&r.capsule:nullptr,interval,plan.unitsPerMetre,cfg.predict,extra,r.now);
            predictKinds[kind].fetch_add(1,std::memory_order_relaxed);
        }
    }
    RT::Vec3 total{};for(unsigned i=0;i<3;++i)total[i]=shift[i]+extra[i];
    shiftPlan(plan,total);
    RT::Vec3 pivot=la.position;for(unsigned i=0;i<3;++i)pivot[i]+=extra[i];
    if(bridged){
        if(!haveLatest)return NoPlan;
        rotatePlan(plan,pivot,la.up,turn);turnBridged.fetch_add(1,std::memory_order_relaxed);
    }
    out.total=total;out.extra=extra;out.pivot=pivot;out.shifted=shifted;out.bridged=bridged;out.kind=kind;out.latest=la;out.haveLatest=haveLatest;
    return Ok;
}
inline Reason computePlacement(const PoseRequest& r,Latest& out) noexcept {
    if(!r.owner)return NoOwner;
    const auto& o=*r.owner;const Config& cfg=config;
    E::StereoCameraPlan plan{};PlanPrep prep{};
    if(const auto why=preparePlan(r,plan,prep))return why;
    const RT::Vec3 total=prep.total,extra=prep.extra,pivot=prep.pivot;const bool shifted=prep.shifted,bridged=prep.bridged;const unsigned kind=prep.kind;
    E::ControllerWeaponFrame frame{};
    if(const auto why=mapRay(plan,r.input,1,false,r.now,cfg.planMaxAgeNs,{},frame.aim))return why;
    if(const auto why=mapRay(plan,r.input,1,true,r.now,cfg.planMaxAgeNs,{},frame.grip))return why;
    double reach2{};for(unsigned i=0;i<3;++i){const double d=(double(frame.grip.origin[i])-frame.grip.viewOrigin[i])/frame.grip.unitsPerMetre;reach2+=d*d;}
    if(!std::isfinite(reach2)||reach2>2.5*2.5)return Reach;
    AnchorSpec anchor{};anchor.mode=cfg.anchor;anchor.wristM=cfg.wristM;
    if(cfg.handBone)anchor.bone=o.geometry.bone[r.alt?1u:0u];
    if(r.hold==0||r.hold==1){E::ControllerRay raw{}; // the hand mesh's own forward (two-hand aim bends the logical aim only)
        if(mapRay(plan,r.raw,unsigned(r.hold),false,r.now,cfg.planMaxAgeNs,{},raw)==Ok){anchor.hasForward=true;anchor.forward=raw.direction;}}
    // Hold mode (AUTO: an animated weapon has its authored muzzle locator -> BARREL; a rigid one's "muzzle" is only the model
    // frame + an offset (GetMuzzleLocationMatrix 0x25464e4..) -> its axes say nothing about the blade/barrel -> HAND), the engine hand
    // of the HOLDING side (left VR hand = the engine's left hand bones), the calibration of this weapon.
    const unsigned side=r.hold==0?1u:0u;
    anchor.hold=cfg.hold==HoldMode::Auto?(o.geometry.animated?HoldMode::Barrel:HoldMode::Hand):cfg.hold;
    anchor.handBone=o.geometry.bone[side];anchor.handLocator=side;
    anchor.flipBlade=cfg.knifeFlip&&o.kind==E::WeaponClass::Melee;
    if(cfg.calibration){const auto e=calib::store.effective(int(o.kind),calib::typeKey(int(o.kind),int(o.typeId)));anchor.calibrate=e.source!=calib::SourceNone;anchor.calib=e.entry;}
    Placement placed{};
    if(const auto why=place(frame,o.geometry,r.alt,r.scale,cfg.offsets[unsigned(o.kind)%classCount],placed,anchor))return why;
    notePlacementMode(o,anchor,placed);
    if(firearm(o.kind)&&!placed.hand&&anchor.handBone.anatomy)compareHandHold(frame,o,anchor,r,placed);
    Latest l{};l.weapon=o.weapon;l.ns=r.now;l.placement=placed;l.shift=total;l.shifted=shifted;l.hand=r.hold;
    l.planAgeMs=r.now>=plan.tracking.publishedNs?float(double(r.now-plan.tracking.publishedNs)/1e6):-1.f;
    l.inputAgeMs=r.now>=r.input.publishedNs?float(double(r.now-r.input.publishedNs)/1e6):-1.f;
    l.map=planMap(plan);
    if(r.hold==0||r.hold==1){l.localAim=r.raw.aim[unsigned(r.hold)];l.localGrip=r.raw.grip[unsigned(r.hold)];}
    l.referenceSpace=r.raw.referenceSpace;l.epoch=r.raw.epoch;l.sequence=r.raw.sequence;
    l.predictedAnchor=pivot;l.extra=extra;l.predicted=shifted;l.turnBridged=bridged;l.predictKind=kind;l.firearm=firearm(o.kind);
    out=l;return Ok;
}

// ---- render tagging: this frame's placement + the head sample (plan) its eyes render with ----
struct Tagged {std::uint64_t headNs{};Latest latest{};float laserM{};bool laserHit{},laserProbed{};};
inline std::mutex tagMutex;inline std::array<Tagged,8> tagRing{};inline unsigned tagNext{};inline std::uint64_t lastTagNs{};
inline std::atomic<std::uint64_t> tagsMade{},tagsStale{},residualFrames{},residualUmSum{},residualUmMax{};inline std::atomic<float> residualLastCm{};
inline void tagFrame(const AnchorRecord& anchor) noexcept {
    const auto l=latestPlacement();
    std::lock_guard lock(tagMutex);
    const auto since=lastTagNs;lastTagNs=anchor.sampledNs;
    if(::rdrvr::vr::holsters::holdHand.load(std::memory_order_relaxed)<0||!l.weapon)return;
    if(!l.ns||l.ns<=since||l.ns>anchor.sampledNs){tagsStale.fetch_add(1,std::memory_order_relaxed);return;}
    Tagged t{};t.headNs=anchor.sampledNs;t.latest=l;tagRing[tagNext]=t;tagNext=(tagNext+1)%unsigned(tagRing.size());
    tagsMade.fetch_add(1,std::memory_order_relaxed);
    if(l.predicted){ // where the eyes' anchor really is vs where this frame's placement assumed it
        RT::Vec3 d{};for(unsigned i=0;i<3;++i)d[i]=anchor.position[i]-l.predictedAnchor[i];
        const float s=l.placement.unitsPerMetre>0.f?l.placement.unitsPerMetre:1.f,cm=len3(d)/s*100.f;
        if(std::isfinite(cm)&&cm<100.f){const auto um=std::uint64_t(cm*1e4f);residualFrames.fetch_add(1,std::memory_order_relaxed);
            residualUmSum.fetch_add(um,std::memory_order_relaxed);residualLastCm.store(cm,std::memory_order_relaxed);
            auto m=residualUmMax.load(std::memory_order_relaxed);while(um>m&&!residualUmMax.compare_exchange_weak(m,um)){}}
    }
}
inline bool findTag(std::uint64_t headNs,Tagged& out) noexcept {
    std::lock_guard lock(tagMutex);if(!headNs)return false;
    for(const auto& t:tagRing)if(t.headNs==headNs){out=t;return true;}
    return false;
}
// DUAL: the phantom's placement of this frame, tagged with the same anchor (its own ring; no residual statistics).
inline std::mutex phantomTagMutex;inline std::array<Tagged,8> phantomTagRing{};inline unsigned phantomTagNext{};inline std::uint64_t lastPhantomTagNs{};
inline void tagPhantom(const AnchorRecord& anchor) noexcept {
    const auto l=phantomPlacement();
    std::lock_guard lock(phantomTagMutex);
    const auto since=lastPhantomTagNs;lastPhantomTagNs=anchor.sampledNs;
    const int ph=::rdrvr::vr::holsters::phantomHand.load(std::memory_order_relaxed);
    if(ph<0||!l.weapon||l.weapon!=::rdrvr::vr::holsters::phantomWeapon.load(std::memory_order_relaxed))return;
    if(!l.ns||l.ns<=since||l.ns>anchor.sampledNs)return;
    Tagged t{};t.headNs=anchor.sampledNs;t.latest=l;phantomTagRing[phantomTagNext]=t;phantomTagNext=(phantomTagNext+1)%unsigned(phantomTagRing.size());
    phantomTags.fetch_add(1,std::memory_order_relaxed);
}
inline bool findPhantomTag(std::uint64_t headNs,Tagged& out) noexcept {
    std::lock_guard lock(phantomTagMutex);if(!headNs)return false;
    for(const auto& t:phantomTagRing)if(t.headNs==headNs){out=t;return true;}
    return false;
}
// Main thread, right after tagFrame: the laser length of that frame (probe result).
inline void setTagLaser(std::uint64_t headNs,float metres,bool hit,bool probed) noexcept {
    std::lock_guard lock(tagMutex);
    for(auto& t:tagRing)if(t.headNs==headNs){t.laserM=metres;t.laserHit=hit;t.laserProbed=probed;}
}
inline std::atomic<std::uint64_t> laserTags{},laserProbes{},laserHits{},laserProbeFails{},laserProbeNsSum{},laserProbeNsMax{},laserPublished{};
inline std::atomic<float> laserLastM{};

// ---- barrel-exact shot (weapWeapon::ManagerUpdate entry, main thread) ----
// Engine: invWeapon::OnAim 0x26768c0 picks one of FOUR setters from the aim message: Aim(Vector3),
// Aim(gohGUID) (lock-on / soft-lock target: +0x188 bit3, ManagerAim aims the aimer at the target BONE),
// AimToMiss(Vector3) / AimToMiss(gohGUID) (bit1). ManagerAim 0x2545158 (inside ManagerUpdate, before UpdateOpMode fires) turns
// target (+0x160) - ComputeMainMuzzlePosition into the launch direction +0x930; FireProjectile 0x2546f38 adds GetRandomAccuracy
// 0x2547e84 (gaussian, sigma max(+0x1d0,+0x1d4), cached per frame in +0x2b4/+0x2b8 while +0x9c0 is set). For the VR-held firearm:
// target = the barrel point, vector mode (bit0 set; bit1, bit3, bit6 cleared) and, unless debug.rdrvr.vr.weapon.spread 1, the
// per-frame accuracy cache = (0,0).
inline constexpr std::size_t weaponAimTarget=0x160,weaponAimFlags=0x188,weaponAccuracy=0x2b4,weaponAccuracyCached=0x9c0,weaponLaunchDir=0x930;
inline std::atomic<std::uint64_t> aimFixCalls{},aimFixApplied{},aimFixGuid{},aimFixMiss{},aimFixNoAim{},aimFixStale{},aimFixFailed{},spreadZeroed{},spreadApplied{};
template<class Read,class Write>
inline bool forceBarrelAim(Read&& read,Write&& write,std::uintptr_t weapon,const Latest& l,std::uint64_t now,bool zeroSpread) noexcept {
    aimFixCalls.fetch_add(1,std::memory_order_relaxed);
    if(!weapon||l.weapon!=weapon||!l.ns||now<l.ns||now-l.ns>150'000'000ull){aimFixStale.fetch_add(1,std::memory_order_relaxed);return false;}
    // HAMMER FAN SPREAD: during a FAN series the barrel line is turned by a fresh cone draw every frame (yaw about the
    // barrel's up, pitch about its right - the authored muzzle locator: barrel -Z, up +Y, right +X); the laser keeps the true barrel.
    RT::Vec3 dir=l.placement.direction;
    if(const auto sp=::rdrvr::vr::hammer::spreadFor(now);sp.active){
        constexpr float deg=3.14159265f/180.f;const auto& M=l.placement.muzzle;
        RT::Vec3 up{M[4],M[5],M[6]},right{M[0],M[1],M[2]};normalize(up);normalize(right);
        dir=axisRotate(dir,up,sp.yawDeg*deg);dir=axisRotate(dir,right,sp.pitchDeg*deg);normalize(dir);spreadApplied.fetch_add(1,std::memory_order_relaxed);}
    const auto p=aimPoint(l.placement.origin,dir,config.aimDistanceM,l.placement.unitsPerMetre);
    for(float x:p)if(!std::isfinite(x)){aimFixFailed.fetch_add(1,std::memory_order_relaxed);return false;}
    std::uint32_t flags{};
    if(!read(weapon+weaponAimFlags,&flags,sizeof flags)){aimFixFailed.fetch_add(1,std::memory_order_relaxed);return false;}
    if(flags&0x8u)aimFixGuid.fetch_add(1,std::memory_order_relaxed);
    if(flags&0x2u)aimFixMiss.fetch_add(1,std::memory_order_relaxed);
    if(!(flags&0x1u))aimFixNoAim.fetch_add(1,std::memory_order_relaxed);
    const std::uint32_t next=(flags&~(0x2u|0x8u|0x40u))|0x1u;const float target[3]{p[0],p[1],p[2]};
    if(!write(weapon+weaponAimTarget,target,sizeof target)||!write(weapon+weaponAimFlags,&next,sizeof next)){
        aimFixFailed.fetch_add(1,std::memory_order_relaxed);return false;}
    if(zeroSpread){const float zero[2]{};const std::uint8_t one=1;
        if(write(weapon+weaponAccuracy,zero,sizeof zero)&&write(weapon+weaponAccuracyCached,&one,1))spreadZeroed.fetch_add(1,std::memory_order_relaxed);}
    aimFixApplied.fetch_add(1,std::memory_order_relaxed);return true;
}
inline bool processWrite(std::uintptr_t address,const void* data,std::size_t n) noexcept {
    if(!address)return false;std::memcpy(reinterpret_cast<void*>(address),data,n);return true;}
// weapWeapon::FireProjectile entry of the VR-held weapon: the direction the engine launches with (+0x930, before the accuracy
// offset) vs this frame's barrel, the aim mode bits, the accuracy cache and the camera launch override.
struct ShotRecord {std::uint64_t ns{};RT::Vec3 dir{},barrel{},target{};float errDeg{-1},rootFromBoreCm{-1};std::uint32_t flags{};float accuracy[2]{};
    std::uint8_t cached{},launch{};};
inline std::mutex shotRecordMutex;inline ShotRecord lastShotRecord{};
inline std::atomic<std::uint64_t> shotObserved{},shotDirSamples{},shotErrMdegSum{},shotErrMdegMax{},shotGuidMode{},shotAccuracyNonZero{},shotLaunchSet{},shotStale{},
    shotSpreadExcluded{};
template<class Read>
inline void observeShot(Read&& read,std::uintptr_t weapon,const Latest& l,std::uint64_t now) noexcept {
    ShotRecord s{};s.ns=now;float d[3]{},t[3]{};
    if(!read(weapon+weaponLaunchDir,d,sizeof d)||!read(weapon+weaponAimTarget,t,sizeof t)||!read(weapon+weaponAimFlags,&s.flags,4)||
       !read(weapon+weaponAccuracy,s.accuracy,sizeof s.accuracy)||!read(weapon+weaponAccuracyCached,&s.cached,1)||!read(weapon+weaponLaunchOverride,&s.launch,1))return;
    shotObserved.fetch_add(1,std::memory_order_relaxed);
    s.dir={d[0],d[1],d[2]};s.target={t[0],t[1],t[2]};
    if(s.flags&0x8u)shotGuidMode.fetch_add(1,std::memory_order_relaxed);
    if(s.launch)shotLaunchSet.fetch_add(1,std::memory_order_relaxed);
    if(!s.cached||s.accuracy[0]!=0.f||s.accuracy[1]!=0.f)shotAccuracyNonZero.fetch_add(1,std::memory_order_relaxed);
    // HAMMER: a shot of a FAN series launched along the spread cone - not an aim error (excluded from dirErr)
    const bool spreadShot=::rdrvr::vr::hammer::spreadTagged(now);if(spreadShot)shotSpreadExcluded.fetch_add(1,std::memory_order_relaxed);
    if(!spreadShot&&l.weapon==weapon&&l.ns&&now>=l.ns&&now-l.ns<=150'000'000ull){
        s.barrel=l.placement.direction;const float dl=len3(s.dir);
        // FireProjectile launches either from the muzzle locator or (projectile+0x288 vt+0x48) from weapon+0xb0 (model origin):
        // how far that origin is from the bore line = the worst-case parallel offset of the bullet from the drawn laser.
        float root[3]{};
        if(read(weapon+0xb0,root,sizeof root)){RT::Vec3 v{};for(unsigned i=0;i<3;++i)v[i]=root[i]-l.placement.origin[i];
            const float along=v[0]*s.barrel[0]+v[1]*s.barrel[1]+v[2]*s.barrel[2];for(unsigned i=0;i<3;++i)v[i]-=along*s.barrel[i];
            const float u=l.placement.unitsPerMetre>0.f?l.placement.unitsPerMetre:1.f;s.rootFromBoreCm=len3(v)/u*100.f;}
        if(dl>1e-4f&&std::isfinite(dl)){const float c=std::clamp((s.dir[0]*s.barrel[0]+s.dir[1]*s.barrel[1]+s.dir[2]*s.barrel[2])/dl,-1.f,1.f);
            s.errDeg=std::acos(c)*57.29578f;const auto md=std::uint64_t(s.errDeg*1000.f);shotDirSamples.fetch_add(1,std::memory_order_relaxed);
            shotErrMdegSum.fetch_add(md,std::memory_order_relaxed);auto m=shotErrMdegMax.load(std::memory_order_relaxed);
            while(md>m&&!shotErrMdegMax.compare_exchange_weak(m,md)){}}
    }else if(!spreadShot)shotStale.fetch_add(1,std::memory_order_relaxed);
    std::lock_guard lock(shotRecordMutex);lastShotRecord=s;
}

// ---- weapon: per-shot telemetry (trigger press edge -> RT forwarded -> engine FireProjectile of the VR-held weapon) ----
// Input thread: noteTrigger (holsters route, every press edge of the holding trigger). Engine threads: noteFire
// (weapWeapon::FireProjectile entry). A forwarded press with no FireProjectile of the held weapon within shotWindowNs is a
// refusal; its reason is the state captured at the press.
enum ShotReason : unsigned {ShotNotRearmed,ShotMelee,ShotCylinderOpen,ShotOtherBlock,ShotNotHeld,ShotNotFirearm,ShotNoOwner,ShotContext,
    ShotNoForceAim,ShotNoEngineFire,ShotEmptyChamber,ShotSpentChamber,ShotOneShot,ShotHammerDown,ShotReasonCount}; // Reload: + the chamber gate's blocks; hammer: + HAMMER_DOWN
inline constexpr std::array<const char*,ShotReasonCount> shotReasonNames{"NOT_REARMED","MELEE","CYLINDER_OPEN","BLOCKED","NOT_HELD",
    "NOT_FIREARM","NO_OWNER","NATIVE_CONTEXT","NO_FORCE_AIM","NO_ENGINE_FIRE","EMPTY_CHAMBER","SPENT_CHAMBER","ONE_SHOT","HAMMER_DOWN"};
inline constexpr std::uint64_t shotWindowNs=700'000'000;
struct Shot {std::uint64_t ns{};int hand{-1};unsigned reason{ShotNoEngineFire};bool open{};std::uintptr_t weapon{};int aimBits{-1},kind{-1};
    firediag::Event diagnostic{};};
inline std::mutex shotMutex;inline firediag::Pending<Shot> pendingShots{};
inline std::atomic<std::uint64_t> shotEdges{},shotForwarded{},shotFired{},shotLatencyNsSum{},shotLatencyNsMax{},firesHeld{},firesHeldUnmatched{},
    firesOther{},shotMergedHammer{};
inline std::array<std::atomic<std::uint64_t>,ShotReasonCount> shotRefusals{};
inline std::atomic<unsigned> lastShotReason{ShotReasonCount};inline std::atomic<std::uint64_t> lastShotNs{};
inline void refuseShot(unsigned why,std::uint64_t now) noexcept {
    shotRefusals[std::min<unsigned>(why,ShotReasonCount-1)].fetch_add(1,std::memory_order_relaxed);
    lastShotReason.store(why,std::memory_order_relaxed);lastShotNs.store(now,std::memory_order_relaxed);
}
// Caller holds shotMutex: a pending press past its window is a refusal - unless HAMMER merged that very press into another pulse's shot
// (no pulse room, or its R1 pulse stayed open across a shot a FAN pulse claimed): then it is `mergedHammer`.
inline void expireShotLocked(std::uint64_t now) noexcept {
    pendingShots.expire(now,shotWindowNs,[now](const Shot& s){
        auto d=s.diagnostic;d.ns=now;
        if(s.reason==ShotNoEngineFire&&::rdrvr::vr::hammer::edgeMerged(s.ns)){
            shotMergedHammer.fetch_add(1,std::memory_order_relaxed);d.stage="MERGED";d.reason="HAMMER_MERGED";
        }else{refuseShot(s.reason,now);d.stage="NO_ENGINE_FIRE";d.reason="NO_PROJECTILE_WITHIN_700MS";}
        // hint is the state at the press, not a proven engine refusal reason.
        firediag::record(d);
    });
}
inline void noteTrigger(int hand,bool forwarded,const char* block,std::uint64_t now) noexcept {
    namespace H=::rdrvr::vr::holsters;
    shotEdges.fetch_add(1,std::memory_order_relaxed);
    std::lock_guard lock(shotMutex);expireShotLocked(now);
    Owner o{};{std::lock_guard ownerLock(ownerMutex);o=owner;}
    Shot s{};s.ns=now;s.hand=hand;s.open=true;s.weapon=o.weapon;s.kind=int(o.kind);s.aimBits=H::engineAimBits.load(std::memory_order_relaxed);
    auto& d=s.diagnostic;d.ns=d.attemptNs=now;d.hand=hand;d.heldHand=H::holdHand.load();d.slot=H::currentSlot.load();
    d.generation=int(H::holdGeneration.load());d.weapon=o.weapon;d.kind=s.kind;d.aimBits=s.aimBits;
    d.selectState=H::engineSelectState.load();d.forceAim=H::forceAimOurs.load()?1:0;
    const auto cfg=settings();d.preset=int(weaponPresetOf(cfg));d.hammer=int(cfg.hammerStyle());d.reload=int(cfg.reloadMode());
    const auto name=H::handName.load();std::snprintf(d.name.data(),d.name.size(),"%s",name.c_str());
    ::rdrvr::vr::reload::EngineAmmo ammo{};
    if(::rdrvr::vr::reload::ammoFor(o.weapon,ammo,now)){d.ammoKnown=1;d.clip=ammo.clip;d.pool=ammo.pool;d.nativeReloading=int(ammo.nativeReloading);}
    if(!forwarded){
        const std::string b=block?block:"";
        refuseShot(b=="NOT_REARMED"?ShotNotRearmed:b=="MELEE"?ShotMelee:b=="CYLINDER_OPEN"?ShotCylinderOpen:b=="EMPTY_CHAMBER"?ShotEmptyChamber:
            b=="SPENT_CHAMBER"?ShotSpentChamber:b=="ONE_SHOT"?ShotOneShot:b=="HAMMER_DOWN"?ShotHammerDown:ShotOtherBlock,now);
        // ROUTE already records the exact gate (including SPIN) and raw controller values.
        d.stage="BLOCKED";d.reason=block?block:"UNKNOWN_GATE";firediag::record(d);return;
    }
    shotForwarded.fetch_add(1,std::memory_order_relaxed);
    const auto mode=H::ForceAim(H::forceAimMode.load(std::memory_order_relaxed));
    if(H::holdHand.load(std::memory_order_relaxed)<0)s.reason=ShotNotHeld;
    else if(!o.weapon)s.reason=ShotNoOwner;
    else if(!firearm(o.kind))s.reason=ShotNotFirearm;
    else if(!o.forceAllowed)s.reason=ShotContext;
    else if(mode!=H::ForceAim::Off&&!H::forceAimOurs.load(std::memory_order_relaxed)&&!(s.aimBits>=0&&(s.aimBits&H::aimBitForce)))s.reason=ShotNoForceAim;
    else s.reason=ShotNoEngineFire;
    d.stage="WAIT_ENGINE";d.reason="ROUTED_NOT_YET_CONFIRMED";d.hint=shotReasonNames[s.reason];firediag::record(d);
    pendingShots.add(s,[now](const Shot& old){auto lost=old.diagnostic;lost.ns=now;lost.stage="TRACKING_OVERFLOW";
        lost.reason="OUTCOME_UNKNOWN";firediag::record(lost);});
}
// weapWeapon::FireProjectile entry (any thread).
// HAMMER: hammer::claimFire first - a FAN stroke / SA pulse is counted in vr.weapon.hammer (not an
// unmatched engine fire); a pulse of a trigger press (R1) continues with the pendingShot match below.
// An SA pulse IS the trigger press that route() forwarded (it opened the pendingShot): engineFiresHammer
// AND the press closes as fired.
inline std::atomic<std::uint64_t> firesHammer{};
inline bool matchShotLocked(std::uintptr_t weapon,std::uint64_t now) noexcept { // caller holds shotMutex
    expireShotLocked(now);
    const auto* shot=pendingShots.match(weapon,now,shotWindowNs);if(!shot)return false;
    shotFired.fetch_add(1,std::memory_order_relaxed);const auto l=now-shot->ns;
    auto d=shot->diagnostic;d.ns=now;d.stage="FIRED";d.reason="FIRE_PROJECTILE_CALLED";firediag::record(d);
    shotLatencyNsSum.fetch_add(l,std::memory_order_relaxed);
    auto m=shotLatencyNsMax.load(std::memory_order_relaxed);while(l>m&&!shotLatencyNsMax.compare_exchange_weak(m,l)){}
    return true;
}
inline void noteFire(std::uintptr_t weapon,std::uint64_t now) noexcept {
    std::uintptr_t held{};{std::lock_guard lock(ownerMutex);held=owner.weapon;}
    if(!weapon||weapon!=held){firesOther.fetch_add(1,std::memory_order_relaxed);
        // A delayed projectile can arrive after holstering or switching weapons.
        if(weapon){std::lock_guard lock(shotMutex);matchShotLocked(weapon,now);}return;}
    firesHeld.fetch_add(1,std::memory_order_relaxed);
    {const auto c=::rdrvr::vr::hammer::claimFire(weapon,now);
        if(c.claimed&&c.kind!=::rdrvr::vr::hammer::KindTrigger){firesHammer.fetch_add(1,std::memory_order_relaxed);
            if(c.kind==::rdrvr::vr::hammer::KindSa){std::lock_guard lock(shotMutex);matchShotLocked(weapon,now);}
            return;}}
    std::lock_guard lock(shotMutex);
    if(!matchShotLocked(weapon,now))firesHeldUnmatched.fetch_add(1,std::memory_order_relaxed); // auto-fire repeats, engine-initiated shots
}

// ---- BODY ----
// The BODY hook (aActor::Broadcast(stdMsgUpdateAttachments), before the engine attaches the weapon) computes this frame's BARREL
// placement and derives the holding wrist's IK target from it; the weapon pose of the SAME thread inside that hook scope (bodyik
// scope serial) reuses exactly that placement, so the skeletal hand and the gun coincide by construction. HAND-held items (knife,
// thrown, lasso, rigid) whose hand the BODY IK drove are left to the engine's native attachment (the item hangs in the IK'd hand).
struct BodyPre {std::uint64_t serial{};std::uintptr_t weapon{};bool alt{};float scale{1};Latest latest{};};
inline thread_local BodyPre bodyPre{};
inline std::atomic<bool> lastPoseAlt{};inline std::atomic<float> lastPoseScale{0.f}; // the last weapon Update's alt flag / attachment scale
inline std::atomic<std::uint64_t> bodyPreComputed{},bodyPreUsed{},bodyPreMismatch{},bodyPreFailed{},bodyNativeHold{};
// The engine's own alt flag of the last Update (lastPoseAlt is the EFFECTIVE alt the placement used), and the
// side-alt counters: placements whose locator side differs from the engine's flag, the attachment handed back converted to the
// engine's locator (model * inverse(locator[native])), conversions that failed (the placement's own attachment is passed then).
inline std::atomic<bool> lastNativeAlt{};
inline std::atomic<std::uint64_t> sideAltPoses{},sideAltConverted{},sideAltConvertFailed{},sideAltFrames{};
// The alt flag a placement should use: BODY active + an animated weapon held in a VR hand -> the VR holding side (0 = left = alt);
// otherwise the engine's own flag.
inline bool effectiveAlt(bool native,int hold,const Geometry& g,bool body) noexcept {
    if(!body||!sideAltEnabled.load(std::memory_order_relaxed)||!g.valid||!g.animated||(hold!=0&&hold!=1))return native;
    return hold==0;
}
// The attachment matrix that the engine's Update (with ITS locator, native alt) turns into `model`: model * inverse(locator[native]),
// rigidized at the call's scale. False = keep the placement's own attachment.
inline bool engineAttachment(const Geometry& g,bool native,const RT::Mat4& model,float scale,RT::Mat4& out) noexcept {
    RT::Mat4 inv{};if(!affineInverse(g.locator[native?1u:0u],inv))return false;
    RT::Mat4 a=RT::multiply(model,inv);
    for(float x:a)if(!std::isfinite(x))return false;
    if(!rigidize(a,scale))return false;
    out=a;return true;
}
inline std::string bodyReport() {
    char b[512];std::snprintf(b,sizeof b,"{\"preComputed\":%llu,\"preUsed\":%llu,\"preMismatch\":%llu,\"preFailed\":%llu,\"nativeHold\":%llu,"
        "\"lastAlt\":%s,\"lastNativeAlt\":%s,\"lastScale\":%.4f,\"sideAlt\":{\"enabled\":%s,\"frames\":%llu,\"poses\":%llu,\"converted\":%llu,\"convertFailed\":%llu}}",
        (unsigned long long)bodyPreComputed.load(),(unsigned long long)bodyPreUsed.load(),
        (unsigned long long)bodyPreMismatch.load(),(unsigned long long)bodyPreFailed.load(),(unsigned long long)bodyNativeHold.load(),
        lastPoseAlt.load()?"true":"false",lastNativeAlt.load()?"true":"false",double(lastPoseScale.load()),sideAltEnabled.load()?"true":"false",
        (unsigned long long)sideAltFrames.load(),(unsigned long long)sideAltPoses.load(),(unsigned long long)sideAltConverted.load(),
        (unsigned long long)sideAltConvertFailed.load());
    return b;
}

// ---- report "vr":{"weapon":...} ----
inline std::string vec(const RT::Vec3& v) {char b[96];std::snprintf(b,sizeof b,"[%.3f,%.3f,%.3f]",double(v[0]),double(v[1]),double(v[2]));return b;}
inline std::string report() {
    namespace H=::rdrvr::vr::holsters;
    std::string out;out.reserve(8192);char b[2048];const auto o=ownerSnapshot();const auto l=latestPlacement();
    const auto now=::rdrvr::vr::nowNs();
    const auto mode=H::ForceAim(H::forceAimMode.load());
    std::snprintf(b,sizeof b,"{\"enabled\":%s,\"started\":%s,\"config\":{\"aim\":\"%s\",\"keep\":%s,\"latency\":%s,\"melee\":%s,\"aimDistanceM\":%.0f,"
        "\"planMaxAgeMs\":%.0f},\"holdHand\":%d,\"ours\":%s,",
        config.enabled?"true":"false",started.load()?"true":"false",mode==H::ForceAim::Held?"HELD":mode==H::ForceAim::Trigger?"TRIGGER":"OFF",
        H::keepDrawn.load()?"true":"false",config.latency?"true":"false",config.melee?"true":"false",double(config.aimDistanceM),
        double(config.planMaxAgeNs)/1e6,H::holdHand.load(),H::holdOurs.load()?"true":"false");out+=b;
    std::snprintf(b,sizeof b,"\"owner\":{\"weapon\":%s,\"class\":\"%s\",\"typeId\":%d,\"actorGuid\":%u,\"ageMs\":%.1f,\"poseAllowed\":%s,"
        "\"aimAllowed\":%s,\"contextFlags\":%u,\"observed\":%llu},",
        o.weapon?"true":"false",E::weaponClassName(o.kind),int(o.typeId),o.actorGuid,o.stampNs&&now>=o.stampNs?double(now-o.stampNs)/1e6:-1.0,
        o.poseAllowed?"true":"false",o.aimAllowed?"true":"false",o.contextFlags,(unsigned long long)observed.load());out+=b;
    const auto& g=o.geometry;const RT::Vec3 mz{g.modelMuzzle[12],g.modelMuzzle[13],g.modelMuzzle[14]};
    std::snprintf(b,sizeof b,"\"geometry\":{\"valid\":%s,\"animated\":%s,\"locator\":%s,\"flags\":%u,\"refreshes\":%llu,\"failures\":%llu,"
        "\"state\":\"%s\",\"modelMuzzle\":%s},",g.valid?"true":"false",g.animated?"true":"false",g.locatorPtr?"true":"false",g.flags,
        (unsigned long long)geometryRefreshes.load(),(unsigned long long)geometryFailures.load(),geometryState.load(),vec(mz).c_str());out+=b;
    const char* names[ChannelCount]{"pose","model","aim"};
    for(unsigned c=0;c<ChannelCount;++c){
        const auto& s=stats[c];
        std::snprintf(b,sizeof b,"\"%s\":{\"calls\":%llu,\"done\":%llu,\"last\":\"%s\",\"lastAgeMs\":%.1f,\"declines\":{",names[c],
            (unsigned long long)s.calls.load(),(unsigned long long)s.done.load(),reasonNames[std::min<unsigned>(s.last.load(),ReasonCount-1)],
            s.lastNs.load()&&now>=s.lastNs.load()?double(now-s.lastNs.load())/1e6:-1.0);out+=b;
        bool first=true;for(unsigned r=1;r<ReasonCount;++r){const auto n=s.reasons[r].load();if(!n)continue;
            std::snprintf(b,sizeof b,"%s\"%s\":%llu",first?"":",",reasonNames[r],(unsigned long long)n);out+=b;first=false;}
        out+="}";
        if(c==ChannelPose){std::snprintf(b,sizeof b,",\"completed\":%llu,\"completedNativeTrue\":%llu",(unsigned long long)poseCompleted.load(),
            (unsigned long long)poseCompletedNative.load());out+=b;}
        if(c==ChannelModel){std::snprintf(b,sizeof b,",\"rigidSkipped\":%llu",(unsigned long long)modelSkipped.load());out+=b;}
        if(c==ChannelAim){std::snprintf(b,sizeof b,",\"fromPose\":%llu,\"fromController\":%llu",(unsigned long long)aimFromPose.load(),
            (unsigned long long)aimFromController.load());out+=b;}
        out+="},";
    }
    std::snprintf(b,sizeof b,"\"placement\":{\"ageMs\":%.1f,\"hand\":%d,\"grip\":%s,\"muzzle\":%s,\"barrel\":%s,\"shift\":%s,\"shifted\":%s,"
        "\"shiftApplied\":%llu,\"shiftSkipped\":%llu,\"planAgeMs\":%.1f,\"inputAgeMs\":%.1f},",
        l.ns&&now>=l.ns?double(now-l.ns)/1e6:-1.0,l.hand,vec(l.placement.grip).c_str(),vec(l.placement.origin).c_str(),vec(l.placement.direction).c_str(),
        vec(l.shift).c_str(),l.shifted?"true":"false",(unsigned long long)shiftApplied.load(),(unsigned long long)shiftSkipped.load(),
        double(l.planAgeMs),double(l.inputAgeMs));out+=b;
    const int bits=H::engineAimBits.load();
    std::snprintf(b,sizeof b,"\"engine\":{\"keepApi\":%s,\"forceAimApi\":%s,\"aimBits\":%d,\"rdr1AimMode\":%s,\"forceAim\":%s,\"forceAimOurs\":%s,"
        "\"holdTimer\":%.3f,\"selectState\":%d,\"zoom\":%d,\"heldFirearm\":%s,\"keepCalls\":%llu,\"forceAimSet\":%llu,\"forceAimCleared\":%llu,"
        "\"forceAimForeign\":%llu,\"forceAimLostExternally\":%llu,\"engineHolsters\":%llu,\"byKind\":{\"HOLD_MODE_TIMEOUT\":%llu,\"PAD_INPUT\":%llu,\"OTHER\":%llu,",
        H::keepApiReady.load()?"true":"false",H::forceAimApiReady.load()?"true":"false",bits,bits>=0&&(bits&H::aimBitRdr1)?"true":"false",
        bits>=0&&(bits&H::aimBitForce)?"true":"false",H::forceAimOurs.load()?"true":"false",double(H::engineHoldTimer.load()),H::engineSelectState.load(),
        H::engineZoom.load(),H::heldFirearm.load()?"true":"false",(unsigned long long)H::keepCalls.load(),(unsigned long long)H::forceAimSet.load(),
        (unsigned long long)H::forceAimCleared.load(),(unsigned long long)H::forceAimForeign.load(),(unsigned long long)H::forceAimLostExternally.load(),
        (unsigned long long)H::engineHolsters.load(),(unsigned long long)H::engineHolstersTimer.load(),(unsigned long long)H::engineHolstersInput.load(),
        (unsigned long long)H::engineHolstersOther.load());out+=b;
    std::snprintf(b,sizeof b,"\"FORCE_HOLSTER\":%llu,\"GATED\":%llu},\"keepWhileGated\":%llu,\"menuResumes\":%llu,",
        (unsigned long long)H::engineHolstersForced.load(),(unsigned long long)H::engineHolstersGated.load(),(unsigned long long)H::keepWhileGated.load(),
        (unsigned long long)H::menuResumes.load());out+=b;
    H::EngineHolster e{};{std::lock_guard lock(H::engineHolsterMutex);e=H::lastEngineHolster;}
    std::snprintf(b,sizeof b,"\"lastEngineHolster\":{\"kind\":\"%s\",\"ageMs\":%.0f,\"padButtons\":%u,\"padRt\":%.2f,\"padLt\":%.2f,\"holdTrigger\":%.2f,"
        "\"timerBefore\":%.3f,\"aimBitsBefore\":%d,\"selectStateBefore\":%d,\"hand\":%d,\"ours\":%s,\"forceAim\":%s,\"invFlags\":%u,\"gate\":\"%s\"}},",
        e.kind,e.ns&&now>=e.ns?double(now-e.ns)/1e6:-1.0,e.buttons,double(e.rt),double(e.lt),double(e.holdTrigger),double(e.timer),e.aimBits,e.state,
        e.hand,e.ours?"true":"false",e.forced?"true":"false",unsigned(e.invFlags),e.gate?e.gate:"");out+=b;
    // Gates per native field, launch override, per-shot telemetry, trigger policy, reload.
    std::snprintf(b,sizeof b,"\"gates\":{\"forceAllowed\":%s,\"forceFalse\":%llu,\"aimFalse\":%llu,\"flags\":{",o.forceAllowed?"true":"false",
        (unsigned long long)gateForceFalse.load(),(unsigned long long)gateAimFalse.load());out+=b;
    {bool first=true;for(unsigned i=0;i<contextFlagCounts.size();++i){const auto n=contextFlagCounts[i].load();if(!n)continue;
        std::snprintf(b,sizeof b,"%s\"%s\":%llu",first?"":",",contextFlagNames[i],(unsigned long long)n);out+=b;first=false;}}
    std::snprintf(b,sizeof b,"}},\"launch\":{\"api\":%s,\"enabled\":%s,\"sites\":\"pose,aim,manager,fire\",\"seen\":[%llu,%llu,%llu,%llu],\"cleared\":[%llu,%llu,%llu,%llu],\"drv\":%llu,"
        "\"unreadable\":%llu},",setLaunchOverride.load()?"true":"false",config.launch?"true":"false",
        (unsigned long long)launchSeen[0].load(),(unsigned long long)launchSeen[1].load(),(unsigned long long)launchSeen[2].load(),
        (unsigned long long)launchSeen[3].load(),(unsigned long long)launchCleared[0].load(),(unsigned long long)launchCleared[1].load(),
        (unsigned long long)launchCleared[2].load(),(unsigned long long)launchCleared[3].load(),
        (unsigned long long)launchDrv.load(),(unsigned long long)launchUnreadable.load());out+=b;
    {std::lock_guard lock(shotMutex);expireShotLocked(now);}
    const auto fired=shotFired.load();const auto lr=lastShotReason.load();
    std::snprintf(b,sizeof b,"\"fire\":{\"triggerEdges\":%llu,\"forwarded\":%llu,\"fired\":%llu,\"latencyMsAvg\":%.1f,\"latencyMsMax\":%.1f,"
        "\"engineFiresHeld\":%llu,\"engineFiresUnmatched\":%llu,\"engineFiresOther\":%llu,\"engineFiresHammer\":%llu,\"mergedHammer\":%llu,\"lastRefusal\":\"%s\",\"lastRefusalAgeMs\":%.0f,\"refusals\":{",
        (unsigned long long)shotEdges.load(),(unsigned long long)shotForwarded.load(),(unsigned long long)fired,
        fired?double(shotLatencyNsSum.load())/double(fired)/1e6:0.0,double(shotLatencyNsMax.load())/1e6,(unsigned long long)firesHeld.load(),
        (unsigned long long)firesHeldUnmatched.load(),(unsigned long long)firesOther.load(),(unsigned long long)firesHammer.load(),(unsigned long long)shotMergedHammer.load(),
        lr<ShotReasonCount?shotReasonNames[lr]:"NONE",
        lastShotNs.load()&&now>=lastShotNs.load()?double(now-lastShotNs.load())/1e6:-1.0);out+=b;
    {bool first=true;for(unsigned i=0;i<ShotReasonCount;++i){const auto n=shotRefusals[i].load();if(!n)continue;
        std::snprintf(b,sizeof b,"%s\"%s\":%llu",first?"":",",shotReasonNames[i],(unsigned long long)n);out+=b;first=false;}}
    out+="}},\"diagnostics\":"+firediag::report()+',';
    std::snprintf(b,sizeof b,"\"triggers\":{\"ltNative\":%s,\"meleeTrigger\":%s,\"heldKind\":%d,\"edges\":%llu,\"forwarded\":%llu,"
        "\"ltBlockedFrames\":%llu,\"rtBlockedMelee\":%llu,\"rtBlockedCylinder\":%llu,\"rtBlockedRearm\":%llu,\"bMaskedFrames\":%llu,"
        "\"empty\":{\"native\":%s,\"strikeOnly\":%s,\"edgesWithheld\":%llu,\"framesWithheld\":%llu,\"strikePassed\":%llu,\"passedGated\":%llu}},",
        H::ltNative.load()?"true":"false",H::meleeTrigger.load()?"true":"false",H::heldKind.load(),(unsigned long long)H::triggerEdges.load(),
        (unsigned long long)H::triggerForwarded.load(),(unsigned long long)H::ltBlockedFrames.load(),(unsigned long long)H::rtBlockedMelee.load(),
        (unsigned long long)H::rtBlockedCylinder.load(),(unsigned long long)H::rtBlockedRearm.load(),(unsigned long long)H::bMaskedFrames.load(),
        H::emptyTriggerNative.load()?"true":"false",H::nativeFistTrigger.load()?"true":"false",(unsigned long long)H::emptyTriggerEdges.load(),
        (unsigned long long)H::emptyTriggerFrames.load(),(unsigned long long)H::emptyStrikePassed.load(),(unsigned long long)H::emptyTriggerPassedGated.load());out+=b;
    out+="\"reload\":";out+=::rdrvr::vr::reload::report();out+=",";
    out+="\"hammer\":";out+=::rdrvr::vr::hammer::report();out+=",";
    {
        const auto& c=config;const auto& gb=o.geometry.bone;
        std::snprintf(b,sizeof b,"\"anchor\":{\"mode\":\"%s\",\"wristM\":%.3f,\"handBone\":{\"enabled\":%s,\"state\":\"%s\",\"runs\":%llu,\"ok\":%llu,"
            "\"valid\":[%s,%s],\"wrist\":[%s,%s],\"attach\":[%d,%d],\"wristIndex\":[%d,%d],\"boneId\":[%u,%u]},\"wristAnchored\":%s,\"boneUsed\":%s,"
            "\"move\":%s,\"vrWrist\":%s},",c.anchor==AnchorMode::Wrist?"WRIST":"GRIP",double(c.wristM),c.handBone?"true":"false",handBoneState.load(),
            (unsigned long long)handBoneRuns.load(),(unsigned long long)handBoneOk.load(),gb[0].valid?"true":"false",gb[1].valid?"true":"false",
            vec(gb[0].wrist).c_str(),vec(gb[1].wrist).c_str(),gb[0].attach,gb[1].attach,gb[0].wristIndex,gb[1].wristIndex,gb[0].boneId,gb[1].boneId,
            l.placement.wristAnchored?"true":"false",l.placement.boneUsed?"true":"false",vec(l.placement.anchorMove).c_str(),vec(l.placement.wrist).c_str());out+=b;
        const auto rf=residualFrames.load();
        std::snprintf(b,sizeof b,"\"lag\":{\"predict\":\"%s\",\"kinds\":{\"none\":%llu,\"capsule\":%llu,\"velocity\":%llu},\"capsuleReads\":%llu,"
            "\"capsuleFails\":%llu,\"capsuleStill\":%llu,\"rejected\":%llu,\"predictWarpBridged\":%llu,\"lastKind\":\"%s\",\"lastExtraCm\":%.2f,\"residualFrames\":%llu,"
            "\"residualCmAvg\":%.2f,\"residualCmMax\":%.2f,\"residualLastCm\":%.2f},",
            c.predict==Predict::Off?"OFF":c.predict==Predict::Velocity?"VELOCITY":"CAPSULE",(unsigned long long)predictKinds[0].load(),
            (unsigned long long)predictKinds[1].load(),(unsigned long long)predictKinds[2].load(),(unsigned long long)capsuleReads.load(),
            (unsigned long long)capsuleFails.load(),(unsigned long long)capsuleStill.load(),(unsigned long long)predictRejected.load(),
            (unsigned long long)predictWarpBridged.load(),
            predictNames[std::min<unsigned>(l.predictKind,PredictKindCount-1)],
            double(len3(l.extra)/(l.placement.unitsPerMetre>0.f?l.placement.unitsPerMetre:1.f)*100.f),
            (unsigned long long)rf,rf?double(residualUmSum.load())/double(rf)/1e4:0.0,double(residualUmMax.load())/1e4,double(residualLastCm.load()));out+=b;
        std::snprintf(b,sizeof b,"\"turn\":{\"bridge\":%s,\"noted\":%llu,\"bridged\":%llu,\"verified\":%llu,\"signFlips\":%llu,\"noBefore\":%llu,"
            "\"expired\":%llu,\"sign\":%d,\"learnedSign\":%d,\"lastTurnDeg\":%.1f,\"measuredDeg\":%.1f},",c.turnBridge?"true":"false",
            (unsigned long long)turnsNoted.load(),(unsigned long long)turnBridged.load(),(unsigned long long)turnVerified.load(),
            (unsigned long long)turnSignFlips.load(),(unsigned long long)turnNoBefore.load(),(unsigned long long)turnExpired.load(),turnSign(),
            learnedSign.load(),double(turnLastDeg.load()),double(turnMeasuredDeg.load()));out+=b;
        std::snprintf(b,sizeof b,"\"aimFix\":{\"enabled\":%s,\"spread\":\"%s\",\"calls\":%llu,\"applied\":%llu,\"guidOverridden\":%llu,\"missOverridden\":%llu,"
            "\"noAimBit\":%llu,\"stale\":%llu,\"failed\":%llu,\"spreadZeroed\":%llu,\"fanSpreadApplied\":%llu},",c.aimFix?"true":"false",c.spread?"NATIVE":"ZERO",
            (unsigned long long)aimFixCalls.load(),(unsigned long long)aimFixApplied.load(),(unsigned long long)aimFixGuid.load(),
            (unsigned long long)aimFixMiss.load(),(unsigned long long)aimFixNoAim.load(),(unsigned long long)aimFixStale.load(),
            (unsigned long long)aimFixFailed.load(),(unsigned long long)spreadZeroed.load(),(unsigned long long)spreadApplied.load());out+=b;
        ShotRecord s{};{std::lock_guard lock(shotRecordMutex);s=lastShotRecord;}
        const auto ds=shotDirSamples.load();
        std::snprintf(b,sizeof b,"\"shot\":{\"observed\":%llu,\"dirSamples\":%llu,\"dirErrDegAvg\":%.3f,\"dirErrDegMax\":%.3f,\"guidMode\":%llu,"
            "\"accuracyNonZero\":%llu,\"launchOverrideSet\":%llu,\"stale\":%llu,\"spreadExcluded\":%llu,\"last\":{\"ageMs\":%.0f,\"errDeg\":%.3f,\"dir\":%s,\"barrel\":%s,"
            "\"flags\":%u,\"accuracy\":[%.4f,%.4f],\"cached\":%u,\"launch\":%u,\"rootFromBoreCm\":%.2f}},",(unsigned long long)shotObserved.load(),(unsigned long long)ds,
            ds?double(shotErrMdegSum.load())/double(ds)/1000.0:0.0,double(shotErrMdegMax.load())/1000.0,(unsigned long long)shotGuidMode.load(),
            (unsigned long long)shotAccuracyNonZero.load(),(unsigned long long)shotLaunchSet.load(),(unsigned long long)shotStale.load(),
            (unsigned long long)shotSpreadExcluded.load(),s.ns&&now>=s.ns?double(now-s.ns)/1e6:-1.0,double(s.errDeg),vec(s.dir).c_str(),vec(s.barrel).c_str(),s.flags,double(s.accuracy[0]),
            double(s.accuracy[1]),unsigned(s.cached),unsigned(s.launch),double(s.rootFromBoreCm));out+=b;
        const auto lp=laserProbes.load();
        std::snprintf(b,sizeof b,"\"laser\":{\"menu\":%s,\"lengthM\":%.1f,\"widthMm\":%.1f,\"probeEvery\":%u,\"tagged\":%llu,\"published\":%llu,\"probes\":%llu,"
            "\"hits\":%llu,\"probeFails\":%llu,\"probeUsAvg\":%.1f,\"probeUsMax\":%.1f,\"lastLengthM\":%.2f},\"tag\":{\"made\":%llu,\"stale\":%llu},",
            ::rdrvr::vr::settings().laser()&&::rdrvr::vr::enabled(::rdrvr::vr::Feature::Laser)?"true":"false",double(c.laserLengthM),
            double(c.laserWidthMm),c.laserProbe,(unsigned long long)laserTags.load(),(unsigned long long)laserPublished.load(),(unsigned long long)lp,
            (unsigned long long)laserHits.load(),(unsigned long long)laserProbeFails.load(),lp?double(laserProbeNsSum.load())/double(lp)/1e3:0.0,
            double(laserProbeNsMax.load())/1e3,double(laserLastM.load()),(unsigned long long)tagsMade.load(),(unsigned long long)tagsStale.load());out+=b;
    }
    {
        const auto& c=config;
        std::snprintf(b,sizeof b,"\"hold\":{\"config\":\"%s\",\"thrown\":%s,\"lasso\":%s,\"barrel\":%llu,\"hand\":%llu,\"handFallback\":{",
            c.hold==HoldMode::Barrel?"BARREL":c.hold==HoldMode::Hand?"HAND":"AUTO",c.thrown?"true":"false",c.lasso?"true":"false",
            (unsigned long long)modeBarrel.load(),(unsigned long long)modeHand.load());out+=b;
        {bool first=true;for(unsigned i=0;i<handWhyNames.size();++i){const auto n=handFallbacks[i].load();if(!n)continue;
            std::snprintf(b,sizeof b,"%s\"%s\":%llu",first?"":",",handWhyNames[i],(unsigned long long)n);out+=b;first=false;}}
        const auto hc=handCompareFrames.load();
        std::snprintf(b,sizeof b,"},\"anatomy\":{\"state\":\"%s\",\"ok\":%llu},\"handVsBarrel\":{\"frames\":%llu,\"degAvg\":%.1f,\"degMax\":%.1f,\"lastDeg\":%.1f,"
            "\"originMmAvg\":%.1f,\"lastPitchDeg\":%.1f,\"lastYawDeg\":%.1f,\"lastRollDeg\":%.1f},\"handRollDeg\":%.1f},",anatomyState.load(),(unsigned long long)anatomyOk.load(),(unsigned long long)hc,
            hc?double(handCompareMdegSum.load())/double(hc)/1000.0:0.0,double(handCompareMdegMax.load())/1000.0,double(handCompareLastDeg.load()),
            hc?double(handCompareMmSum.load())/double(hc):0.0,double(handComparePitch.load()),double(handCompareYaw.load()),double(handCompareRoll.load()),
            double(c.handRollDeg));out+=b;
        std::snprintf(b,sizeof b,"\"rigid\":{\"poseDone\":%llu,\"repaired\":%llu,\"rejected\":%llu,\"errMicroMax\":%llu},\"holdLast\":{\"enabled\":%s,\"used\":%llu,"
            "\"stale\":%llu,\"byReason\":{",(unsigned long long)stats[ChannelPose].done.load(),(unsigned long long)rigidRepaired.load(),
            (unsigned long long)rigidRejected.load(),(unsigned long long)rigidErrMicroMax.load(),c.holdLast?"true":"false",
            (unsigned long long)holdLastUsed.load(),(unsigned long long)holdLastStale.load());out+=b;
        {bool first=true;for(unsigned r=0;r<ReasonCount;++r){const auto n=holdLastByReason[r].load();if(!n)continue;
            std::snprintf(b,sizeof b,"%s\"%s\":%llu",first?"":",",reasonNames[r],(unsigned long long)n);out+=b;first=false;}}
        std::snprintf(b,sizeof b,"}},\"turnFix\":{\"measureRejected\":%llu},\"calibration\":{\"enabled\":%s,\"appliedFrames\":%llu,\"store\":",
            (unsigned long long)turnMeasureRejected.load(),c.calibration?"true":"false",(unsigned long long)calibApplied.load());out+=b;
        out+=calib::store.report();out+="},\"classes\":{";
        std::array<ClassSeen,classCount> seen{};{std::lock_guard lock(classSeenMutex);seen=classSeen;}
        bool first=true;
        for(unsigned k=0;k<classCount;++k){const auto& s=seen[k];if(!s.seen)continue;
            std::snprintf(b,sizeof b,"%s\"%s\":{\"typeId\":%d,\"name\":\"%s\",\"geometry\":%s,\"animated\":%s,\"muzzleRotDeg\":%.1f,\"modelMuzzle\":%s,"
                "\"hold\":\"%s\",\"handUsed\":%s,\"handWhy\":\"%s\",\"poses\":%llu,\"handPoses\":%llu,\"calibrated\":%s,\"anchorMove\":%s,\"rigidErrMicro\":%u,"
                "\"ageMs\":%.0f,\"bones\":[",first?"":",",classKey(E::WeaponClass(k)),s.typeId,s.name,s.geometry?"true":"false",s.animated?"true":"false",
                double(s.muzzleRotDeg),vec(s.modelMuzzle).c_str(),s.hold==HoldMode::Hand?"HAND":"BARREL",s.hand?"true":"false",s.handWhy,
                (unsigned long long)s.poses,(unsigned long long)s.handPoses,s.calibrated?"true":"false",vec(s.anchorMove).c_str(),s.rigidErrMicro,
                s.lastNs&&now>=s.lastNs?double(now-s.lastNs)/1e6:-1.0);out+=b;first=false;
            for(unsigned h=0;h<2;++h){const auto& hb=s.bone[h];
                std::snprintf(b,sizeof b,"%s{\"valid\":%s,\"found\":%u,\"boneId\":%u,\"attach\":%d,\"wristIndex\":%d,\"wrist\":%s,\"anatomy\":%s,\"knuckle\":%s,\"thumb\":%s}",
                    h?",":"",hb.valid?"true":"false",unsigned(hb.found),hb.boneId,hb.attach,hb.wristIndex,vec(hb.wrist).c_str(),hb.anatomy?"true":"false",
                    vec(hb.knuckle).c_str(),vec(hb.thumb).c_str());out+=b;}
            out+="]}";
        }
        out+="},";
    }
    { // DUAL: the phantom's pose (calls = its holstered Update calls seen while it is the phantom)
        const auto p=phantomSnapshot();const auto pl=phantomPlacement();
        std::snprintf(b,sizeof b,"\"phantom\":{\"weapon\":%s,\"class\":\"%s\",\"typeId\":%d,\"hand\":%d,\"poseAllowed\":%s,\"geometry\":%s,\"animated\":%s,"
            "\"geometryState\":\"%s\",\"geometryRefreshes\":%llu,\"geometryFailures\":%llu,\"observed\":%llu,\"calls\":%llu,\"placed\":%llu,\"models\":%llu,"
            "\"holdLast\":%llu,\"lastHand\":%d,\"lastAgeMs\":%.1f,\"placement\":{\"hand\":%d,\"handHold\":%s,\"calibrated\":%s,\"firearm\":%s},"
            "\"tags\":%llu,\"laserTags\":%llu,\"adopted\":%llu,\"declines\":{",
            p.weapon?"true":"false",E::weaponClassName(p.kind),int(p.typeId),p.hand,p.poseAllowed?"true":"false",p.geometry.valid?"true":"false",
            p.geometry.animated?"true":"false",phantomGeometryState.load(),(unsigned long long)phantomGeometryRefreshes.load(),
            (unsigned long long)phantomGeometryFailures.load(),(unsigned long long)phantomObserved.load(),(unsigned long long)phantomCalls.load(),
            (unsigned long long)phantomPlaced.load(),(unsigned long long)phantomModels.load(),(unsigned long long)phantomHoldLast.load(),
            phantomLastHand.load(),phantomLastNs.load()&&now>=phantomLastNs.load()?double(now-phantomLastNs.load())/1e6:-1.0,pl.hand,
            pl.placement.hand?"true":"false",pl.placement.calibrated?"true":"false",pl.firearm?"true":"false",(unsigned long long)phantomTags.load(),
            (unsigned long long)phantomLaserTags.load(),(unsigned long long)phantomAdopted.load());out+=b;
        bool first=true;for(unsigned r=1;r<ReasonCount;++r){const auto n=phantomDeclines[r].load();if(!n)continue;
            std::snprintf(b,sizeof b,"%s\"%s\":%llu",first?"":",",reasonNames[r],(unsigned long long)n);out+=b;first=false;}
        std::snprintf(b,sizeof b,"},\"holsterFix\":{\"measured\":%llu,\"applied\":%llu,\"readFailures\":%llu,\"bad\":%llu,\"changedMaxMicro\":%llu,\"bladeFlips\":%llu}},",
            (unsigned long long)phantomFixMeasured.load(),(unsigned long long)phantomFixApplied.load(),(unsigned long long)phantomFixReadFailures.load(),
            (unsigned long long)phantomFixBad.load(),(unsigned long long)phantomFixChangedMicro.load(),(unsigned long long)bladeFlips.load());out+=b;
    }
    out+="\"body\":";out+=bodyReport();out+=",";
    std::snprintf(b,sizeof b,"\"release\":{\"deferredFrames\":%llu,\"deferred\":%llu,\"holdReleases\":%llu},\"trigger\":{\"hold\":%.2f,\"padRt\":%.2f,\"padLt\":%.2f,\"padButtons\":%u}}",
        (unsigned long long)H::releaseDeferredFrames.load(),(unsigned long long)H::releaseDeferred.load(),(unsigned long long)H::holdReleases.load(),
        double(H::holdTriggerValue.load()),double(H::lastPadRt.load()),double(H::lastPadLt.load()),H::lastPadButtons.load());out+=b;
    return out;
}
} // namespace rdrvr::vr::weapon
