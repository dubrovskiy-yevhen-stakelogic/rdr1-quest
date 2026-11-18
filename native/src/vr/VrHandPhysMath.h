#pragma once
// HAND PHYSICS: what VrBody hook A, the physics callbacks and the main-thread tick (VrHandPhys.h) and the AppSW hand parts (GameXr
// spaceWarpFrame) share, and the pure logic (host test tools/test-hand-physics.cpp). Header-only, lock-free, no engine access.
//
// Order inside one engine frame N (sagShellDerived::Execute): Update N runs the physics step (sagWorld::UpdatePhysLevel, sm_PhysicsStepCount
// substeps, each phSimulator::PreUpdateInstanceBehaviors -> the step -> UpdateInstanceBehaviors), then hook A on the post-physics workers,
// then UpdateInputs N (the tick). Hook A of frame N-1 publishes the targets the substeps of frame N steer to; the substeps publish the
// solved bodies hook A of frame N reads.
// World units = metres * upm; every constant below is in metres, the functions take world units unless named otherwise.
#include "VrBodyLink.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace rdrvr::vr::handphys {
using V3=std::array<float,3>;
using Rows=std::array<V3,3>; // a body's local axes in world: rows a, b, c of its matrix (palm: S, F, N)
enum Mode : unsigned {ModeOff=0,ModeMeasure=1,ModeOn=2};
// ClassBody: an empty free hand, it gets a body. ClassHeld: the hand holds or works an item (no body; the AppSW fit of the held capsule).
enum HandClass : std::uint8_t {ClassNone=0,ClassBody=1,ClassHeld=2};
// Absent: not in the physics level. Ghost: in the level, every pair dropped, follows the controller. Travel: on its way from the chest.
// Live: collides. Park: held back past the leash; every pair dropped, the body stands still where it stopped.
enum BodyState : std::uint8_t {BodyAbsent=0,BodyGhost=1,BodyLive=2,BodyTravel=3,BodyPark=4};
inline constexpr std::array<const char*,5> bodyStateNames{"ABSENT","GHOST","LIVE","TRAVEL","PARK"};
// phInst::GetClassType values of the other instance of a pair.
inline constexpr int classMover=7,classNav=0xa,classDeadBody=0xc;
// Body shapes (lever shape).
enum Shape : unsigned {ShapeSphere=0,ShapeCapsule=1,ShapePalm=2};

namespace v {
inline V3 add(V3 a,V3 b) noexcept {return {a[0]+b[0],a[1]+b[1],a[2]+b[2]};}
inline V3 sub(V3 a,V3 b) noexcept {return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
inline V3 mul(V3 a,float s) noexcept {return {a[0]*s,a[1]*s,a[2]*s};}
inline float dot(V3 a,V3 b) noexcept {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
inline V3 cross(V3 a,V3 b) noexcept {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
inline float length(V3 a) noexcept {return std::sqrt(dot(a,a));}
inline bool finite(V3 a) noexcept {return std::isfinite(a[0])&&std::isfinite(a[1])&&std::isfinite(a[2]);}
inline V3 normalized(V3 a) noexcept {const float l=length(a);return l>1e-9f&&std::isfinite(l)?mul(a,1.f/l):V3{0,0,0};}
// x rotated by the rotation vector r (axis * angle, radians; Rodrigues).
inline V3 rotate(V3 r,V3 x) noexcept {
    const float t=length(r);if(!(t>1e-9f)||!std::isfinite(t))return x;
    const V3 k=mul(r,1.f/t);const float c=std::cos(t),s=std::sin(t);
    return add(add(mul(x,c),mul(cross(k,x),s)),mul(k,dot(k,x)*(1.f-c)));
}
inline bool finite(const Rows& r) noexcept {return finite(r[0])&&finite(r[1])&&finite(r[2]);}
inline Rows rotate(V3 r,const Rows& m) noexcept {return {rotate(r,m[0]),rotate(r,m[1]),rotate(r,m[2])};}
}
inline constexpr float pi=3.14159265f;

// ============================== levers ==============================
// debug.rdrvr.vr.handphys.* (read by the tick every 64 ticks; VrHandPhys.h readLevers). type / include 0 = derived from the player's mover.
struct Levers {
    unsigned shape{ShapePalm};
    float radiusM{.03f},lengthM{.12f},anchorFwdM{.03f};               // sphere / capsule only
    float massKg{2.f},inertiaK{4.f},maxSpeedMps{6.f},maxAngRadS{20.f},contactSpinRadS{4.f};
    float creepMps{.10f},pushSpeedMps{1.5f},tangentMps{3.f};            // the drive into a surface: static, dynamic, along it
    float leashM{.40f},leashMs{600.f},returnM{.15f},staleMs{300.f};
    float visualMs{30.f},holdMs{80.f},rotCapDeg{30.f},liftM{.006f};
    float reachM{.25f},reachLeadS{.10f};unsigned wakeMax{2};            // REACH WAKE: the sphere round a live palm, the lead, wakes per query
    unsigned npcScanTicks{9};                                           // NPC bump: the melee scan period while a body is live
    bool ccd{true},ownVt{true},fist{true},visual{true},doors{true},doorClose{false},props{true},npcBump{true},npc{false},haptic{true},reachWake{true};
    std::uint32_t type{},include{};
    // DOOR SWING: the pivot door's phase follows the palm (keep = the open fraction a released door opens fully from, else it glides
    // shut; rate = the angular speed cap of the push; idle = no push and no swing for this long releases the door).
    bool doorSwing{true};float doorKeep{.35f},doorRateRadS{6.f},doorIdleMs{400.f};
    bool unlock{true};                                                  // props: the type's activation lock lifted on a kept pair
    // NPC virtual body: VrMelee's bone capsules of the creatures near a palm clip the hand's command; press = a bump every tick while
    // pressing, slap = an open-hand hit at slapMps or more on the head, neck or chest; horses only take the body and the press.
    bool npcBody{true},npcPress{true},npcSlap{true},npcHorse{true};float slapMps{1.4f};
    // DOORS (leaf follow): the leaf follows the palm both ways (0 = the atan swing), a released door rests part-open (0 = finish or
    // glide shut), pushed shut at doorShutDeg or less it closes, the same push cannot reopen it for doorRearmMs; a push the leaf test
    // rejects still opens the door the engine's way (doorPlain).
    bool doorFollow{true},doorRest{true},doorPlain{true};float doorShutDeg{2.f},doorRearmMs{600.f};
    // props: the root of a small inactive prop released under a pressing palm (radius <= releaseR, depth >= releaseDepth)
    bool release{true};float releaseRM{.35f},releaseDepthM{.005f};
    // NPC open hand: slap (a reaction, no damage), shove (a short push away), press (a held push); the push and yaw limits
    bool npcShove{true},npcPush{true};float shoveMps{1.2f},pushGain{1.f},pushMaxM{.025f},pushCapM{.50f},slapYawRad{.25f};bool slapReact{true};
    // GRAB: a closed open hand at a prop or a corpse part holds it through a constraint; force / torque / mass of the held hand, the prop
    // mass limit (rigid props), fixed rotation for props, the classes (bit = phInst class), the open debounce, the break distance
    // (grabLeashM, held grabBreakMs), the throw gain
    bool grab{true};float grabM{.04f},grabForceN{300.f},grabTorqueNm{40.f},grabMassKg{8.f},grabMaxKg{150.f};bool grabRot{true};
    unsigned grabClass{0x1400};float grabOpenMs{350.f},grabLeashM{.60f},grabThrow{0.f};
    bool cpu{true}; // per-mode main-thread CPU time per frame
    // UNFREEZE: a small gohProp kept still by its never-activate flag (+0x12 bit 3, from its description) is made physical under a
    // pressing palm as aniAnimProp::ReleaseProp does it (the flag back once it sleeps untouched); its size limits (level sphere radius,
    // part count) and a sanity cap on the archetype's data mass
    bool unfreeze{true};float unfreezeRM{.20f},unfreezeKg{50.f};
    // GRAB: a deliberate grab releases the fixed root of a prop up to this level sphere radius (the press: releaseRM); grabkg limits the mass
    float grabReleaseRM{1.f};
    // DOORS (approach): the player's own walk-in open (sagPlayerInputDoor::FindDoor) while the hands are live: doorAuto 0 = suppressed (the
    // hands and the mover's push open the door), 1 = stock; doorBody = the mover pushes a closed or part-open leaf ahead of it;
    // doorHandPose = a hand on a closed leaf opens it posed (state 4, the leaf at the hand) instead of the stock full swing; doorStuckMs =
    // the stock Open of a closed door the mover has been at that long that no push opened (doorStuckBandM / doorStuckReachM)
    bool doorAuto{false},doorBody{true},doorHandPose{true};float doorStuckMs{1500.f};
    // GLASS: a window pane hit by a hand at glassMps or more (the controller's approach) gets the engine's own break call with glassDmg
    bool glass{true};float glassMps{2.f},glassDmg{100.f};
    // GLASS BLOOD: a bare hand (a hand body, no prop held) that broke a pane plays the engine's punch blood drop on the hand, at most one
    // per hand per bloodCdMs (and 3 per 2 s); 0 = none
    bool blood{true};float bloodCdMs{600.f};
    // UNFREEZE: at most this many composite parts; the mass set on the prop's own collider (scaled by its size, 0 = the archetype's);
    // a one-time velocity from the hand's approach
    unsigned unfreezeParts{4};float unfreezeSetKg{.35f};bool unfreezeKick{true};
    // UNFREEZE (approach): a never-activate prop that passes the gates is made physical while a live palm closes on it, the lead time
    // ahead of the contact (unfreezeAheadS), so the engine solves the first contact with the prop already active
    bool unfreezeNear{true};float unfreezeAheadS{.12f};
    // GRAB: the force on the held prop at the grip (gain, total and acceleration caps, the weight it lifts), fixed rotation only up to
    // grabRotKg, the break time; furniture whose type never moves (minMove < 0) is grabbable up to grabMoveKg and grabMoveRM
    bool grabDrive{true};float grabGain{.5f},grabForceMaxN{900.f},grabAccel{20.f},grabLiftKg{25.f},grabRotKg{8.f},grabBreakMs{250.f};
    bool grabMove{true};float grabMoveKg{40.f},grabMoveRM{1.f};
    // GRAB: the drive on an articulated collider (its ApplyForce runs the instance's damage notification: off by default)
    bool grabDriveArt{false};
    // GRAB (a ball-jointed hold: a prop over grabRotKg): the drive on the grip point (grabGripForce: natural frequency grabWn rad/s, damping
    // ratio grabZeta, the force's change per substep at most grabSlewN, downward at most grabDownG x its weight), the hand body moving
    // with the grip, the hand drawn at the grip; 0 = the velocity law on the centre of mass and the hand drawn at its body
    bool grabGrip{true};float grabWn{8.f},grabZeta{1.f},grabSlewN{40.f},grabDownG{.5f};
    // NPC: the injure message of an open-hand slap / push (OpenTouchMsg: 0 the fake guid, 1 the creature itself, 2 none; never the
    // player, never damage: openTouchPlan), a press pushing this far within 400 ms escalates to a push, the slap window after a slow touch
    unsigned slapAttacker{1},pushAttacker{1};float pressPushM{.06f},slapWindowMs{120.f};
    // UNFREEZE: a prop the hands made physical stays where it lands: the gringo's readiness poll (ggoComponentProp::OnMsg 0x2000) does not
    // start GracefulPropReset for it while the player is near (resetHoldM)
    bool resetHold{true};
    // NPC push tiers (pushTier): firm = a shove edge at pushFirmMps or more, pushFirmM or more into the body and the arm's own speed at
    // pushArmMps or more (the injure message from the creature itself); a slower shove (medium) and every escalated press (light) only
    // move the creature. pushMedium: 0 the offset only, 1 an UP reaction message. pushImpulse: the message's impulse along the push (0 = the
    // player's punch value, aniMeleeActionData +0xd4); pushKick: a projMsgApplyImpulse along the palm (pushKickPlan) once the creature of a
    // firm shove went ragdoll, pushKickK N s per m/s of approach (at least 1 m/s), at most pushKickMaxNs
    float pushFirmMps{1.6f},pushFirmM{.01f},pushArmMps{1.5f},pushImpulse{0.f};unsigned pushMedium{0};bool pushKick{true};
    float pushKickK{20.f},pushKickMaxNs{150.f};
    // NPC push grading (pushGrade; 0 = the tiered response above): a palm driven into the neck or the chest is never a slap (pushLike), a
    // second hand on the same creature within pairMs makes one pair push, a slap verdict waits slapHoldMs for that second hand; the push
    // (effective approach aEff: pushMerge) is a stumble along it (mover offset stumbleDistanceM, no message) under pushDownMps, a
    // knockdown (SHOVED, no offset) at pushDownMps or more from a firm edge, kicked to knockdownMps along the push against the creature's
    // own velocity (kickImpulse; pushKick2: a second kick 100 ms later when the pelvis does not follow)
    bool pushGrade{true};float pushDownMps{2.8f},slapHoldMs{100.f},pairMs{150.f};bool pushKick2{true};
    // KNOCK SERVO (pushKickServo): after kick 1 a knockdown is steered on its measured centre-of-mass velocity (comProxy) with the kick's own
    // law until it moves along the push (kick2Wanted false), 300 ms, 4 more kicks or 250 N s in all; it replaces kick 2. pushKickEarly:
    // kick 1 queued on the knockdown message's tick (entPendingImpulse holds it until the ragdoll is active) instead of on the first
    // ragdoll tick. 0 / 0 = kick 1 at the first ragdoll tick and kick 2.
    bool pushKickServo{true},pushKickEarly{false};
    // GRAB (heavy and two-hand holds): grabChord = a hand holding a prop or at a grab / join candidate keeps the manual-theater grip chord
    // off (it zeroes both grips: the hold read an open hand); grabHeldLatch = a holding hand's re-latch is captured, never LB / RB;
    // grabHeavyHold = a heavy hold (a ball-jointed prop over grabLiftKg, or a pair) ends only on the grip under .30 for grabOpenMs (no
    // throw; the timer counts only under .30 and resets at .50); grabPairFull = each hand of a pair may lift at least half the pair's
    // weight plus grabAccel (the share keeps the carried weight); grabPairSlew = a pair's force may change as fast as the hand's
    // acceleration up to grabAccel asks (pairSlewN); grabHeavyDraw = the hand of a heavy hold is drawn at its grip up to grabLeashM
    bool grabChord{true},grabHeldLatch{true},grabHeavyHold{true},grabPairFull{true},grabPairSlew{true},grabHeavyDraw{true};
    // GRAB: two hands on one ball-jointed prop drive it as a pair (pairTargets / grabGripForcePair); the weight a pair lifts; the free hand
    // joins the prop the other hand holds within grabJoinM of its grab points (no contact needed); grabFirst: a ready grab beats the
    // holster sockets for every hand (off: only for a hand beside held heavy furniture); heavyMask: a free hand within heavyMaskM of the
    // heavy furniture (a ball-jointed nav prop) the other hand holds draws nothing
    bool grab2{true};float grabLift2Kg{70.f},grabJoinM{.10f};bool grabFirst{false},heavyMask{true};float heavyMaskM{.30f};
    // GRAB: a held prop of propHitKg or more swung into a creature at propHitMps or more (the contact's approach) strikes it as the player
    // (strong at propHitStrongMps); one hit per creature per propHitCoolMs; propKo scales the fist's KO damage
    bool propHit{true};float propHitMps{1.5f},propHitStrongMps{2.5f},propHitCoolMs{700.f},propHitKg{8.f},propKo{1.f};
    // NPC passive contact (npcPassive; 0 = any palm in a body presses, bumps and escalates): only the arm's own motion (HandTarget::velArm
    // along the contact normal, armIn) moves a creature. A press moves it by at most the distance the arm drove into it while armIn was
    // at least pressDeadMps (armStepM); a bump needs armIn >= activeMps at the touch edge or an arm-driven press (held 300 ms); an
    // escalation reads armIn; a shove needs armIn >= shoveArmMps; an escalated press is graded on min(a, armIn), a shove or slap that
    // passed the arm gate on min(a, armIn + the player's forward locomotion) (passiveGradeA). Walking or turning with a still palm
    // against a body moves nothing.
    bool npcPassive{true};float activeMps{.4f},pressDeadMps{.2f},shoveArmMps{.9f};
    // TWO HANDS: the pair's weight split by two-point statics (pairCarry: the carry balances the weight's torque about the centre of mass
    // across the grip line; 0 = the vertical share by the 3-D projection, pairShare); grabPlayerGuard: a pair's spring never pushes the prop further into the player's
    // own capsule (propScan's contact of the last 2 substeps)
    bool grabPairStatics{true},grabPlayerGuard{true};
    // PROPHIT (geometric, propHitGeo; 0 = the engine contact path): the held prop's part boxes against the creatures' bone capsules each
    // tick; a touch within propHitShellM after 150 ms apart is judged for propHitWinMs: the point's approach and swing at propHitMps, a
    // holding hand's own arm speed (velArm) at propHitArmMps and along the contact normal at propHitArmDirMps, both peaks of the last
    // propHitArmMs (the prop trails the hands: the grip drive's lag 2 zeta / wn is 250 ms at wn 8, zeta 1)
    bool propHitGeo{true};float propHitArmMps{1.2f},propHitShellM{.10f},propHitWinMs{60.f},propHitArmMs{400.f},propHitArmDirMps{.6f};
    // HEAVY HOLD (one or two hands on a heavy prop): grabHeavyLeash = such a hold still breaks at grabLeashM (0: it ends only on an open
    // hand, a safety break at grabSafetyM for grabBreakMs, or the mover moving grabTeleportM or more in one tick); grabPlayerSkip = the
    // held heavy prop's contacts with the player's own capsule are switched off before the solve (the + 0xc8 PreComputeImpacts functor,
    // the byte phImpact::Iterator::DisableImpact writes), and kept off grabSkipLingerMs after the hold while the prop still overlaps it;
    // grabPairKg = the collider mass while two hands hold the prop (0 = its own mass; only a heavier prop is lightened); grabHeldAccel =
    // the pair's acceleration cap at that mass (side, spring up and down per hand 0.5 M accel); grabFeedFwd = the command's acceleration
    // (30 ms average, at most grabHeldAccel) feeds the pair's spring as 0.5 M a per hand
    bool grabHeavyLeash{false};float grabSafetyM{2.f},grabTeleportM{1.5f};bool grabPlayerSkip{true};float grabSkipLingerMs{300.f};
    float grabPairKg{15.f},grabHeldAccel{80.f};bool grabFeedFwd{true};
    // PUSH (graded): pushBusyWait = a stumble on a creature whose mover is frozen or suspended (+0x848 / +0x846) waits up to pushBusyWaitMs
    // for it to come free, with one walk-in bump asked for it (0: dropped at the touch); pushEscGrade = an escalated press is graded as a
    // stumble on min(a, armIn) and merges with a live push on that creature (0: the fixed 30 cm push); pushFacingBody = a palm on an arm
    // capsule passes the shove's facing gate on the palm's facing toward the body's centre; pushPeak = the stroke's peak arm speed within
    // pushPeakMs of a shove edge regrades the push when it is 10 % or more above the edge's; pushFront = the knockdown of a push from the
    // front (dirFwd < 0) on a free mover (knockPlan row R1; with pushKnock 0 every front knockdown): 2 SHOVED face 1 at pushBehindImp with
    // the impulse toward the pusher, 1 SHOVED face 0 at pushFrontImp, 0 the message unchanged, 3 SHOVED face 1 at pushBehindImp with the
    // impulse along the push (lever pushknock 0 with pushfront 1 = the message of the push grading before the route table); pushKickHigh =
    // the kick goes to spine3 first
    bool pushBusyWait{true};float pushBusyWaitMs{600.f};bool pushEscGrade{true},pushFacingBody{true},pushPeak{true};float pushPeakMs{100.f};
    unsigned pushFront{2};float pushFrontImp{40.f};bool pushKickHigh{false};
    // pushFrozenMsg = a stumble on a creature whose mover is frozen (+0x848 > 0: StartFrozen's count, which no bump ends) sends the open-hand
    // UP reaction message (slapReaction by side, no offset, the impulse at 0.3) instead of a mover offset that cannot move it (the wait and
    // the bump stay for a suspended mover, +0x846); pushBehindImp = a knockdown from behind whose impulse fell back from the engine punch's k
    // (k unread) takes this size (k reads 75 in the device logs); pushSide = a knockdown along row a (|dirSide| > 0.7): 0 the message
    // unchanged, 1 KNCKBK_RGT for dirSide > 0 and KNCKBK_LFT for < 0, 2 the other way round (both unproven: a device test lever)
    bool pushFrozenMsg{true};float pushBehindImp{75.f};unsigned pushSide{0};
    // KNOCKDOWN ROUTE (knockPlan): pushKnock = the knockdown message by push side and the mover's frozen count at the message (1 the route
    // table, 2 the table's free-mover rows for every mover: every knockdown message is face 1, the default; 0 pushFrontPlan / pushSidePlan
    // alone); pushFrozenHold = the UP message of a
    // stumble on a frozen mover waits pairMs for the other hand (a knockdown within it drops the message; else it goes out at the deadline);
    // pushKnockFlip = how a row's reversed bullet is formed: 1 its forward part in the victim's frame mirrored (the sideways part kept:
    // the device knockdowns' table was recorded with it), 0 the whole impulse reversed (no device knockdown recorded)
    unsigned pushKnock{2};bool pushFrozenHold{true};unsigned pushKnockFlip{1};
    // KNOCK PICK (knockPickParity): the knockdown's NM behaviour chosen through the action tree's shared roll (ConditionRandom mode 0:
    // roll % 2, even = NMShot, odd = staggerFall): 1 staggerFall for dirFwd >= pushKnockPickFwd (the behind half), NMShot otherwise; 2
    // always NMShot; 3 always staggerFall (the default); 0 the engine's coin. Device table by landed node (kicked knockdowns): the face-1
    // staggerFall node #02d1adb2 fell along the push 4 of 5 (front, both sides), the face-0 staggerFall node #9974d115 1 of 4, NMShot
    // #bc3dfb68 1 of 5 (flung toward the pusher at 3.4-4.5 m/s within 50 ms of the kick). pushKnockPickMsg = the forced knockdown's
    // message: 0 the route table's (knockPlan: the messages of that table), 1 face 1, pushBehindImp, the bullet reversed (knockPickPlan,
    // no device record). pushKnockSpine = an NMShot knockdown's message goes to the torso component (spine0 / spine3 / pelvis) instead of
    // the touched part (a device test lever)
    unsigned pushKnockPick{3};float pushKnockPickFwd{0.f};bool pushKnockSpine{false};unsigned pushKnockPickMsg{0};
    // KNOCK TOUGH (pushKnockTough): a knockdown of a tough actor (health +0x150 bit 0x8000, ConditionInjuryToughActor) has the bit cleared
    // from the message until its node lands (120 ms at most), so the meleehit subtree reaches the knockdown nodes instead of the tough
    // stagger that never falls (0 = the engine's tough stagger, counted). pushKnockPickFunc: while a pick is held, ConditionRandom's draw
    // function (sm_RandFunc, null in single player) is ours: the engine's own replay draw, the chosen parity for the victim's own
    // conditions. knockOffMs: the hand bodies ignore a knocked-down creature's frag (its ragdoll) for this long after the knockdown
    // message, ended early once it is up again (0 = the pairs kept and only measured).
    bool pushKnockTough{true},pushKnockPickFunc{true};float knockOffMs{1500.f};
    // RIGID CARRY (two hands on one ball-jointed prop, lever grabCarry; 0 = the per-grip springs of pairStep): one force at the centre of
    // mass (grabCarryWn rad/s, damping ratio grabCarryZeta, at most grabCarryAccel m/s^2 plus the carried weight) and one torque (grabRotWn,
    // grabRotZeta, at most grabRotNm) toward the pose the prop had in the grip-line frame at the join; the grip line low-passed over
    // grabLineTauMs. grabStaleOwn = the drive turns off only for its own force left in the accumulator (a foreign force is counted, the
    // drive goes on); grabGroundZero = a held prop the player's mover stands on loses the mover's queued ground force before the drive;
    // grabGroundLift = while the player's mover stands on the held prop, the drive's upward force is at most the prop's weight (it never
    // lifts the player); grabFallbackTauMs = a ball-jointed hand whose drive did not apply approaches its command over this time constant
    // (0: in one substep); grabCarryPredict = the hand bodies of a carry move at the grips' velocity after the carry's step (the joints then
    // need not accelerate them: the drive sees the prop's own mass and inertia); grabCarryKeepMs = a pair broken for less than this keeps
    // its join pose (the same two holds)
    bool grabCarry{true};float grabCarryWn{10.f},grabCarryZeta{1.f},grabCarryAccel{40.f},grabRotWn{10.f},grabRotZeta{1.4f},grabRotNm{100.f},grabLineTauMs{50.f};
    bool grabStaleOwn{true},grabGroundZero{false},grabGroundLift{true};float grabFallbackTauMs{100.f};bool grabCarryPredict{true};float grabCarryKeepMs{200.f};
    // THROW (a held nav prop let go with a fast arm, throwVerdict): throwMode 0 off, 1 MEASURE (the decide ring only), 2 on. The arm's peak
    // speed within throwWinMs of the grip's open edge at least throwMps (props of propHitKg or more, or a pair) / throwMpsLight, the
    // estimate around the peak at least throwRelMps / throwRelMpsLight, never mostly downward; both hands of a pair open within
    // throwPairMs. The launch: the estimate plus the player's motion, at most throwMaxMps / throwMaxMpsLight. In flight (at most
    // throwFlightMs) the creatures near the prop are tested as for a held prop: a point at throwHitMps or more along the normal hits; a
    // prop of propHitKg or more is the player's strike (KO scaled by throwKo and the mass), strong from throwStrongKgMps (kg x m/s); a
    // lighter one never strikes: throwLight 1 = the open hand's slap reaction on a head / neck / chest hit at 2.5 m/s or more, 0 = none
    unsigned throwMode{2},throwLight{1};float throwMps{2.f},throwMpsLight{1.5f},throwRelMps{1.5f},throwRelMpsLight{1.2f},throwWinMs{250.f},throwPairMs{150.f};
    float throwMaxMps{7.f},throwMaxMpsLight{9.f},throwFlightMs{2000.f},throwHitMps{1.5f},throwStrongKgMps{100.f},throwKo{1.f};
    // HOLD-END GRIP LATCH (grablink::holdLatch): a grip the holsters captured stays masked until it opens whatever ends the hold (the theater
    // chord off while a grip is captured, a squeeze the chord zeroed captured, never read as an open hand)
    bool grabHoldLatch{true};
    // LIGHT GHOST (lightGhostFilter): a closing, non-fist hand (squeeze >= grabOpenSqueeze and rising) passes through a light active nav prop (its
    // collider mass up to grabRotKg, the fixed-rotation hold class): the palm does not shove the cup it is closing on, the grab still
    // latches; also armed on the opening hand when a fixed-rotation hold ends. Kept while the pair overlaps (grabSkipLingerMs quiet,
    // 10 s at most). 0 = contacts as before.
    bool grabGhost{true};
    // GRAB SEAT (seatPoint / seatPullVel): a fixed-rotation hold whose grip point (the matched part box's point nearest the palm centre)
    // is more than seatDoneM off the palm pulls the prop into the palm first (its collider's velocity: the hand's plus the gap over 50 ms,
    // at most pushSpeedMps relative) and attaches once it is there or after grabOpenMs; 0 = attached where it is (it stays at that offset).
    bool grabSeat{true};
    // GRAB WELD: a fixed-rotation hold is two constraints on the same pair as sagDraftVehicleComponent::AttachDraftActor makes them
    // (AttachObjects, then AttachObjectsRotation): the first keeps flags +0xbc = 2 (phConstraint::Update builds the position impulse only
    // for (flags & 3) == 2), the second takes SetFixedRotation (flags 3, the rotation lock); 0 = one constraint with SetFixedRotation
    // (rotation only: the prop hangs on the velocity drive and sinks under its weight).
    bool grabWeld{true};
    // GRAB LIGHT FF: the drive of a fixed-rotation hold carries the prop's own weight (m g up, under the up cap); 0 = the velocity law alone
    bool grabLightFf{true};
    // GRAB SKIP LIGHT: the player skip (grabplayerskip) also serves a welded light hold (weld 1 or 2) and lingers on its prop after the
    // release: the prop rigid in the palm never meets the player's own capsule; 0 = heavy holds only (a welded prop inside the capsule is
    // pushed out by the solver and drags the hand with it)
    bool grabSkipLight{true};
    // GLASS BLOOD HIRES: a blood_push rule whose draw list (rule +0x37d) is one no pass draws (list 1 with the low-res pass off, list 2) is
    // created in list 0 for the hand's drop (the byte set to 0 for the one call, then put back); 0 = the rule's own list
    bool bloodHires{true};
    // BLOOD PLACE: the hand's drop placed just outside the hand's outline as the eye sees it, sprayed out of that outline (bloodEyePlace,
    // bloodMps the inherited speed); 0 = the contact frame (bloodFrame: the contact point, the hand's direction at 3 m/s)
    bool bloodPlace{true};float bloodMps{.6f};
    // DOOR PRESS: a leaf moves whenever the commanded hand part is on or in it, toward the face it came from (the side locked from the
    // approach, the free edge a contact of its own), its hold kept while the command stays within 3 cm, the mover's push applied through a
    // hold with no press; 0 = the solved-part gate (1.5 cm), the side from the solved part every tick, the 45 degree normal test
    bool doorPress{true};
    // GUN GLASS: -1 = no probe, 0 = probe and judge only (would-break counted), 1 = break. The muzzle of a held pistol sweeping into a pane
    // (a sphere of gunGlassR from its last tick's place) and the barrel line, at gunGlassMps or more along the pane's normal; long guns only
    // with gunGlassLong; gunGlassFree the same for the empty hand while a firearm is held (its palm swept, r 4 cm). No blood on this path.
    int gunGlass{0},gunGlassFree{0};float gunGlassMps{2.f},gunGlassR{.025f};bool gunGlassLong{false};
};
inline ::rdrvr::vr::body::Seqlock<Levers> levers; // written by the tick, read by hook A
inline Levers sanitized(Levers l) noexcept {
    const auto fix=[](float x,float lo,float hi,float d){return std::isfinite(x)?std::clamp(x,lo,hi):d;};
    const Levers d{};
    l.shape=l.shape>ShapePalm?d.shape:l.shape;
    l.radiusM=fix(l.radiusM,.01f,.08f,d.radiusM);l.lengthM=fix(l.lengthM,2.f*l.radiusM,.40f,d.lengthM);l.anchorFwdM=fix(l.anchorFwdM,-.10f,.15f,d.anchorFwdM);
    l.massKg=fix(l.massKg,.2f,20.f,d.massKg);l.inertiaK=fix(l.inertiaK,.25f,20.f,d.inertiaK);l.maxSpeedMps=fix(l.maxSpeedMps,.5f,20.f,d.maxSpeedMps);
    l.maxAngRadS=fix(l.maxAngRadS,1.f,60.f,d.maxAngRadS);l.contactSpinRadS=fix(l.contactSpinRadS,.5f,60.f,d.contactSpinRadS);
    l.creepMps=fix(l.creepMps,0.f,1.f,d.creepMps);l.pushSpeedMps=fix(l.pushSpeedMps,0.f,10.f,d.pushSpeedMps);l.tangentMps=fix(l.tangentMps,.1f,20.f,d.tangentMps);
    l.leashM=fix(l.leashM,.05f,1.f,d.leashM);l.leashMs=fix(l.leashMs,50.f,5000.f,d.leashMs);l.returnM=fix(l.returnM,.02f,.5f,d.returnM);
    l.staleMs=fix(l.staleMs,0.f,2000.f,d.staleMs);l.visualMs=fix(l.visualMs,1.f,500.f,d.visualMs);l.holdMs=fix(l.holdMs,0.f,1000.f,d.holdMs);
    l.rotCapDeg=fix(l.rotCapDeg,0.f,90.f,d.rotCapDeg);l.liftM=fix(l.liftM,-.02f,.03f,d.liftM);
    l.reachM=fix(l.reachM,.05f,.5f,d.reachM);l.reachLeadS=fix(l.reachLeadS,0.f,.5f,d.reachLeadS);l.wakeMax=std::min(l.wakeMax,8u);
    l.npcScanTicks=std::clamp(l.npcScanTicks,1u,120u);
    l.doorKeep=fix(l.doorKeep,.05f,.95f,d.doorKeep);l.doorRateRadS=fix(l.doorRateRadS,.5f,20.f,d.doorRateRadS);
    l.doorIdleMs=fix(l.doorIdleMs,100.f,3000.f,d.doorIdleMs);l.slapMps=fix(l.slapMps,.8f,8.f,d.slapMps);
    l.doorShutDeg=fix(l.doorShutDeg,.5f,15.f,d.doorShutDeg);l.doorRearmMs=fix(l.doorRearmMs,0.f,3000.f,d.doorRearmMs);
    l.releaseRM=fix(l.releaseRM,.02f,1.f,d.releaseRM);l.releaseDepthM=fix(l.releaseDepthM,0.f,.05f,d.releaseDepthM);
    l.shoveMps=fix(l.shoveMps,.3f,8.f,d.shoveMps);l.pushGain=fix(l.pushGain,0.f,2.f,d.pushGain);l.pushMaxM=fix(l.pushMaxM,0.f,.05f,d.pushMaxM);
    l.pushCapM=fix(l.pushCapM,0.f,2.f,d.pushCapM);l.slapYawRad=fix(l.slapYawRad,0.f,.8f,d.slapYawRad);
    l.grabM=fix(l.grabM,.005f,.15f,d.grabM);l.grabForceN=fix(l.grabForceN,10.f,3000.f,d.grabForceN);l.grabTorqueNm=fix(l.grabTorqueNm,1.f,500.f,d.grabTorqueNm);
    l.grabMassKg=fix(l.grabMassKg,.5f,50.f,d.grabMassKg);l.grabMaxKg=fix(l.grabMaxKg,1.f,2000.f,d.grabMaxKg);l.grabClass&=0xffffu;
    l.grabOpenMs=fix(l.grabOpenMs,0.f,2000.f,d.grabOpenMs);l.grabLeashM=fix(l.grabLeashM,.05f,1.f,d.grabLeashM);l.grabThrow=fix(l.grabThrow,0.f,3.f,d.grabThrow);
    l.unfreezeRM=fix(l.unfreezeRM,.02f,.5f,d.unfreezeRM);l.unfreezeKg=fix(l.unfreezeKg,.05f,500.f,d.unfreezeKg);l.grabReleaseRM=fix(l.grabReleaseRM,.02f,3.f,d.grabReleaseRM);
    l.doorStuckMs=fix(l.doorStuckMs,200.f,10000.f,d.doorStuckMs);l.glassMps=fix(l.glassMps,.3f,10.f,d.glassMps);l.glassDmg=fix(l.glassDmg,.1f,10000.f,d.glassDmg);
    l.bloodCdMs=fix(l.bloodCdMs,0.f,10000.f,d.bloodCdMs);
    l.bloodMps=fix(l.bloodMps,0.f,5.f,d.bloodMps);
    l.gunGlass=std::clamp(l.gunGlass,-1,1);l.gunGlassFree=std::clamp(l.gunGlassFree,-1,1);
    l.gunGlassMps=fix(l.gunGlassMps,.3f,10.f,d.gunGlassMps);l.gunGlassR=fix(l.gunGlassR,.005f,.08f,d.gunGlassR);
    l.unfreezeParts=std::clamp(l.unfreezeParts,1u,16u);l.unfreezeSetKg=fix(l.unfreezeSetKg,0.f,5.f,d.unfreezeSetKg);
    l.unfreezeAheadS=fix(l.unfreezeAheadS,0.f,.25f,d.unfreezeAheadS);
    l.grabGain=fix(l.grabGain,0.f,2.f,d.grabGain);l.grabForceMaxN=fix(l.grabForceMaxN,0.f,5000.f,d.grabForceMaxN);l.grabAccel=fix(l.grabAccel,0.f,100.f,d.grabAccel);
    l.grabLiftKg=fix(l.grabLiftKg,0.f,200.f,d.grabLiftKg);l.grabRotKg=fix(l.grabRotKg,0.f,2000.f,d.grabRotKg);l.grabBreakMs=fix(l.grabBreakMs,0.f,5000.f,d.grabBreakMs);
    l.grabMoveKg=fix(l.grabMoveKg,0.f,500.f,d.grabMoveKg);l.grabMoveRM=fix(l.grabMoveRM,.05f,3.f,d.grabMoveRM);
    l.grabWn=fix(l.grabWn,1.f,20.f,d.grabWn);l.grabZeta=fix(l.grabZeta,.3f,3.f,d.grabZeta);l.grabSlewN=fix(l.grabSlewN,1.f,2000.f,d.grabSlewN);
    l.grabDownG=fix(l.grabDownG,0.f,2.f,d.grabDownG);
    l.slapAttacker=std::min(l.slapAttacker,2u);l.pushAttacker=std::min(l.pushAttacker,2u);l.pressPushM=fix(l.pressPushM,.01f,.5f,d.pressPushM);
    l.slapWindowMs=fix(l.slapWindowMs,0.f,500.f,d.slapWindowMs);
    l.pushFirmMps=fix(l.pushFirmMps,.3f,8.f,d.pushFirmMps);l.pushFirmM=fix(l.pushFirmM,0.f,.10f,d.pushFirmM);l.pushArmMps=fix(l.pushArmMps,0.f,8.f,d.pushArmMps);
    l.pushImpulse=fix(l.pushImpulse,0.f,1000.f,d.pushImpulse);l.pushMedium=std::min(l.pushMedium,1u);
    l.pushKickK=fix(l.pushKickK,0.f,200.f,d.pushKickK);l.pushKickMaxNs=fix(l.pushKickMaxNs,0.f,1000.f,d.pushKickMaxNs);
    l.pushDownMps=fix(l.pushDownMps,.5f,10.f,d.pushDownMps);l.slapHoldMs=fix(l.slapHoldMs,0.f,300.f,d.slapHoldMs);l.pairMs=fix(l.pairMs,0.f,300.f,d.pairMs);
    l.grabLift2Kg=fix(l.grabLift2Kg,0.f,500.f,d.grabLift2Kg);l.grabJoinM=fix(l.grabJoinM,.04f,.25f,d.grabJoinM);l.heavyMaskM=fix(l.heavyMaskM,.04f,1.f,d.heavyMaskM);
    l.propHitMps=fix(l.propHitMps,.5f,20.f,d.propHitMps);l.propHitStrongMps=fix(l.propHitStrongMps,.5f,30.f,d.propHitStrongMps);
    l.propHitCoolMs=fix(l.propHitCoolMs,100.f,5000.f,d.propHitCoolMs);l.propHitKg=fix(l.propHitKg,0.f,500.f,d.propHitKg);l.propKo=fix(l.propKo,0.f,3.f,d.propKo);
    l.activeMps=fix(l.activeMps,0.f,5.f,d.activeMps);l.pressDeadMps=fix(l.pressDeadMps,0.f,3.f,d.pressDeadMps);l.shoveArmMps=fix(l.shoveArmMps,0.f,8.f,d.shoveArmMps);
    l.propHitArmMps=fix(l.propHitArmMps,0.f,10.f,d.propHitArmMps);l.propHitShellM=fix(l.propHitShellM,0.f,.5f,d.propHitShellM);
    l.propHitWinMs=fix(l.propHitWinMs,10.f,500.f,d.propHitWinMs);l.propHitArmMs=fix(l.propHitArmMs,50.f,1000.f,d.propHitArmMs);
    l.propHitArmDirMps=fix(l.propHitArmDirMps,0.f,10.f,d.propHitArmDirMps);
    l.grabSafetyM=fix(l.grabSafetyM,.5f,10.f,d.grabSafetyM);l.grabTeleportM=fix(l.grabTeleportM,.3f,20.f,d.grabTeleportM);
    l.grabSkipLingerMs=fix(l.grabSkipLingerMs,0.f,2000.f,d.grabSkipLingerMs);l.grabPairKg=fix(l.grabPairKg,0.f,500.f,d.grabPairKg);
    l.grabHeldAccel=fix(l.grabHeldAccel,1.f,200.f,d.grabHeldAccel);
    l.pushBusyWaitMs=fix(l.pushBusyWaitMs,0.f,2000.f,d.pushBusyWaitMs);l.pushPeakMs=fix(l.pushPeakMs,20.f,300.f,d.pushPeakMs);
    l.pushFront=std::min(l.pushFront,3u);l.pushFrontImp=fix(l.pushFrontImp,1.f,200.f,d.pushFrontImp);
    l.pushBehindImp=fix(l.pushBehindImp,1.f,200.f,d.pushBehindImp);l.pushSide=std::min(l.pushSide,2u);
    l.pushKnock=std::min(l.pushKnock,2u);l.pushKnockFlip=std::min(l.pushKnockFlip,1u);
    l.pushKnockPick=std::min(l.pushKnockPick,3u);l.knockOffMs=fix(l.knockOffMs,0.f,5000.f,d.knockOffMs);l.pushKnockPickFwd=fix(l.pushKnockPickFwd,-1.f,1.f,d.pushKnockPickFwd);
    l.pushKnockPickMsg=std::min(l.pushKnockPickMsg,1u);
    l.grabCarryWn=fix(l.grabCarryWn,1.f,30.f,d.grabCarryWn);l.grabCarryZeta=fix(l.grabCarryZeta,.3f,3.f,d.grabCarryZeta);
    l.grabCarryAccel=fix(l.grabCarryAccel,1.f,200.f,d.grabCarryAccel);l.grabRotWn=fix(l.grabRotWn,1.f,30.f,d.grabRotWn);
    l.grabRotZeta=fix(l.grabRotZeta,.3f,3.f,d.grabRotZeta);l.grabRotNm=fix(l.grabRotNm,1.f,1000.f,d.grabRotNm);
    l.grabLineTauMs=fix(l.grabLineTauMs,0.f,500.f,d.grabLineTauMs);l.grabFallbackTauMs=fix(l.grabFallbackTauMs,0.f,1000.f,d.grabFallbackTauMs);
    l.grabCarryKeepMs=fix(l.grabCarryKeepMs,0.f,1000.f,d.grabCarryKeepMs);
    l.throwMode=std::min(l.throwMode,2u);l.throwLight=std::min(l.throwLight,1u);
    l.throwMps=fix(l.throwMps,.5f,10.f,d.throwMps);l.throwMpsLight=fix(l.throwMpsLight,.5f,10.f,d.throwMpsLight);
    l.throwRelMps=fix(l.throwRelMps,.3f,10.f,d.throwRelMps);l.throwRelMpsLight=fix(l.throwRelMpsLight,.3f,10.f,d.throwRelMpsLight);
    l.throwWinMs=fix(l.throwWinMs,50.f,500.f,d.throwWinMs);l.throwPairMs=fix(l.throwPairMs,0.f,400.f,d.throwPairMs);
    l.throwMaxMps=fix(l.throwMaxMps,1.f,12.f,d.throwMaxMps);l.throwMaxMpsLight=fix(l.throwMaxMpsLight,1.f,15.f,d.throwMaxMpsLight);
    l.throwFlightMs=fix(l.throwFlightMs,200.f,5000.f,d.throwFlightMs);l.throwHitMps=fix(l.throwHitMps,.5f,10.f,d.throwHitMps);
    l.throwStrongKgMps=fix(l.throwStrongKgMps,1.f,2000.f,d.throwStrongKgMps);l.throwKo=fix(l.throwKo,0.f,3.f,d.throwKo);
    return l;
}

// ============================== the palm frame ==============================
// The grip frame (columns fa fb fc = grip-local X Y Z in world, VrBody HandPose::frame) and the aim: N = the palm-out normal (OpenXR grip
// +X points into the right palm and out of the left one, VrArmIk HandInGrip), F = the aim without its N part (the default aim in grip
// (0, -0.7071, -0.7071) when that is under 0.2), S = F x N. W = the wrist (wristBack behind the grip along F, wristDepth towards the back
// of the hand), B = the palm centre palmFwdM in front of the wrist: the body's centre of mass. Rows a = S, b = F, c = N (S x F = N).
inline constexpr float palmFwdM=.05f;
struct PalmFrame {V3 S{1,0,0},F{0,1,0},N{0,0,1},W{},B{};bool valid{};};
inline PalmFrame handFrame(V3 grip,V3 fa,V3 fb,V3 fc,V3 aimFwd,bool right,float wristBack,float wristDepth,float upm) noexcept {
    PalmFrame p{};
    const V3 N=v::normalized(right?v::mul(fa,-1.f):fa);
    if(v::length(N)<.5f||!v::finite(grip))return p;
    V3 F=v::sub(aimFwd,v::mul(N,v::dot(aimFwd,N)));
    if(!v::finite(F)||!(v::length(F)>=.2f)){const V3 d=v::add(v::mul(fb,-.7071f),v::mul(fc,-.7071f));F=v::sub(d,v::mul(N,v::dot(d,N)));}
    F=v::normalized(F);if(v::length(F)<.5f)return p;
    p.N=N;p.F=F;p.S=v::cross(F,N);
    p.W=v::sub(v::sub(grip,v::mul(F,wristBack*upm)),v::mul(N,wristDepth*upm));
    p.B=v::add(p.W,v::mul(F,palmFwdM*upm));p.valid=v::finite(p.B)&&v::finite(p.S);
    return p;
}
// PALM body parts (metres; local X = S, Y = F, Z = N; full box sizes; offsets from B): the palm, the open fingers, the fist.
struct Part {V3 offset{},size{};};
inline constexpr std::array<Part,3> palmParts{{{{0.f,0.f,0.f},{.085f,.10f,.028f}},{{0.f,.09f,0.f},{.080f,.08f,.020f}},{{0.f,.04f,.017f},{.080f,.06f,.057f}}}};
// Principal inertia per kg of the palm box (about S, F, N), scaled by the inertia lever (a slower spin response than the box alone).
inline constexpr V3 palmInertiaPerKg{.0028f,.0007f,.0033f};
// The fingers part (open hand) or the fist part, with hysteresis; kept while the hand touches a surface.
inline constexpr float fistOn=.65f,fistOff=.45f;
inline bool partWant(float squeeze,bool fistNow,bool touching,bool fistLever) noexcept {
    if(!fistLever)return false;
    if(touching||!std::isfinite(squeeze))return fistNow;
    if(squeeze>fistOn)return true;
    if(squeeze<fistOff)return false;
    return fistNow;
}

// ============================== hook A -> substeps / tick ==============================
// rel = the body centre target (PALM: B; sphere / capsule: grip + aim * anchor) minus the player's mover position (mover inst matrix row d)
// at hook A, vel = d(rel)/dt (world units per second), rows = S F N (the capsule's axis = rows[1]), angVel = the frame's angular velocity
// (rad/s, world). Index = the PHYSICAL VR hand (0 left, 1 right).
// velArm = the palm's velocity in the OpenXR tracking space (the controller relative to the room), turned to world axes (world units per
// second): no stick locomotion, no smooth or snap turn, no head-lock shift; armOk = two samples of the same space within 100 ms.
// GUN GLASS: gun = this hand holds a placed firearm (hook A's engineHeld && firearm && placed); tipRel / boreRel = its muzzle and the bore
// point level with the holding grip, minus the mover; tipVelArm = the muzzle's velocity in the tracking space turned to world (as velArm),
// tipOk = two samples of one space; source = hook A's arm source (1 free, 2 derived, 3 support); kind = the held weapon's WeaponClass (-1
// none); busy = gun handling seen this frame (1 a shot within 250 ms, 2 the spin keeps the gun, 4 the cylinder not closed or closed
// within 400 ms).
inline constexpr std::uint8_t gunBusyShot=1,gunBusySpin=2,gunBusyReload=4;
struct HandTarget {V3 rel{},vel{};Rows rows{{{1,0,0},{0,1,0},{0,0,1}}};V3 angVel{};float trigger{},squeeze{};std::uint8_t cls{},valid{};
    V3 velArm{};std::uint8_t armOk{};
    V3 tipRel{},boreRel{},tipVelArm{};std::uint8_t gun{},tipOk{},source{},busy{};std::int8_t kind{-1};};
struct Targets {
    std::uint64_t serial{},ns{};          // hook A serial, its clock (vr::nowNs)
    std::uintptr_t mover{},actor{};       // the player's mvrMoverSimComponent (vtable checked by hook A) and actor
    V3 moverPos{},eye{},up{0,1,0};
    float upm{1};
    std::uint8_t bridged{},onFoot{};      // a turn bridge moved this frame's plan; bodyOnFoot
    std::array<HandTarget,2> hand{};
};
inline ::rdrvr::vr::body::Seqlock<Targets> targets;
// ============================== substeps -> hook A / tick ==============================
// solved = the body centre after the last substep, command = where that substep steered it, rows / cmdRows = the solved and the commanded
// axes, n = the contact normal (out of the surface), plane = n . solved, age = substeps since the last contact (255 = none), dyn = that
// contact was with an active prop, contactNs = the clock of the last substep in contact (world). grab = the hand holds a prop through a
// constraint (hook A draws it at the solved pose, no contact plane), lagM = |solved - command| then (metres).
// GRAB (a ball-jointed hold): grip = the held prop's grip point this substep (its collider's pose times the grip in its frame at the grab),
// gripOk = read this substep, ball = the hold is ball-jointed with the grip drive (hook A draws the hand at the grip).
struct HandSolved {V3 solved{},command{};Rows rows{},cmdRows{};V3 n{};float plane{};std::uint64_t contactNs{};std::uint8_t age{255},dyn{},state{},grab{};float lagM{};
    V3 grip{};std::uint8_t gripOk{},ball{};
    // a heavy hold (heavy furniture over grabLiftKg or a pair) with lever grabheavydraw: the hand is drawn at the grip up to grabLeashM
    std::uint8_t heavy{};};
struct Solved {std::uint64_t serial{},ns{};std::array<HandSolved,2> hand{};};
inline ::rdrvr::vr::body::Seqlock<Solved> solved;
// The effective row value. Written by the tick, read by hook A and the render thread.
inline std::atomic<unsigned> liveMode{ModeOff};

inline bool fresh(std::uint64_t now,std::uint64_t ns,std::uint64_t maxAgeNs=100'000'000ull) noexcept {
    return ns&&(now>=ns?now-ns<=maxAgeNs:ns-now<5'000'000ull);
}

// ============================== hand class ==============================
// gateOpen = BODY gate GateOk (no mover attachment, ladder, ragdoll, cutscene). On foot the seat mode is 1 (the hold ramped out), 2 = seated.
inline constexpr bool bodyOnFoot(bool gateOpen,bool mountedArm,int seatMode,bool third) noexcept {return gateOpen&&!mountedArm&&seatMode!=2&&!third;}
struct ClassIn {
    bool onFoot{},bridged{};         // no horse, no third person; a turn bridge moves the plan
    bool sourceFree{},sourceDerived{};
    bool heldHere{},phantomHere{};   // this hand holds the engine item / the phantom
    int hold{-1};                    // holsters::holdHand
    bool weapon{},firearm{},knife{},placed{}; // the verified engine item
    bool otherPhantom{},otherPhantomFirearm{};
};
// A body only for an empty hand on its own controller target. A held knife and the hands that work a firearm (reload, hammer, support, spin)
// stay ClassHeld: no body yet (the blade and the barrel are a later stage). The physics class is taken with bridged = false (a turn
// bridge moves the body with the plan; hook A); the AppSW hand box keeps it.
inline constexpr HandClass classify(const ClassIn& c) noexcept {
    if(!c.onFoot||c.bridged||c.phantomHere)return ClassNone;
    if(c.hold>=0&&!c.weapon)return ClassNone;      // a hold whose item is not verified this frame, lasso, thrown items
    if(c.weapon&&!c.firearm&&!c.knife)return ClassNone;
    if(c.otherPhantom&&!c.otherPhantomFirearm)return ClassNone;
    if(c.heldHere){
        if(c.firearm)return c.sourceDerived&&c.placed?ClassHeld:ClassNone;
        return c.sourceFree?ClassHeld:ClassNone;
    }
    if((c.weapon&&c.firearm)||c.otherPhantom)return ClassHeld;
    return c.sourceFree?ClassBody:ClassNone;
}

// ============================== targets ==============================
inline constexpr float maxAheadS=.05f;     // a target older than this is not extrapolated further
inline constexpr float knifeTipM=.27f;     // a held knife's length from the grip (the AppSW fit of its capsule)
inline constexpr float carriedSpeedMps=12.f,originJumpM=1.f;
// d(rel)/dt of two hook A frames, smoothed; a gap over 100 ms or a jump restarts at 0.
inline V3 targetVelocity(V3 prevVel,V3 prevRel,V3 rel,float dtS,bool restart) noexcept {
    if(restart||!(dtS>1e-4f)||dtS>.1f||!v::finite(prevRel)||!v::finite(rel))return {0,0,0};
    const V3 raw=v::mul(v::sub(rel,prevRel),1.f/dtS);
    const V3 out=v::finite(prevVel)?v::add(v::mul(prevVel,.5f),v::mul(raw,.5f)):raw;
    return v::finite(out)?out:V3{0,0,0};
}
// The player's mover velocity from two substeps (world units per second), smoothed; a gap over 100 ms restarts at 0. dtS = the physics
// dt of the substep between the two positions (the substeps of one frame run a few ms apart on the clock, each covering a full dt).
inline V3 moverVelocity(V3 prevVel,V3 prevPos,V3 pos,float dtS,bool restart) noexcept {return targetVelocity(prevVel,prevPos,pos,dtS,restart);}
// Where a substep of stepS steers the body: the mover's position at the end of the substep plus the hand's offset, extrapolated from the
// targets' clock by at most maxAheadS.
inline float aheadS(float sinceTargetS,float stepS) noexcept {return std::clamp(sinceTargetS+std::clamp(stepS,0.f,.1f),0.f,maxAheadS);}
inline V3 command(const HandTarget& t,V3 moverNow,V3 moverVel,float sinceTargetS,float stepS) noexcept {
    const float step=std::clamp(stepS,0.f,.1f);
    return v::add(v::add(moverNow,v::mul(moverVel,step)),v::add(t.rel,v::mul(t.vel,aheadS(sinceTargetS,stepS))));
}
struct Drive {V3 vel{};bool clamped{};};
// The velocity that takes the body from `from` to `to` in dtS (world units per second). The part relative to `base` (the player's
// mover velocity) is clamped to maxSpeed, so walking and running never trip the clamp.
inline Drive velocityTo(V3 from,V3 to,float dtS,float maxSpeed,V3 base={0,0,0}) noexcept {
    Drive d{};if(!(dtS>1e-5f)||!v::finite(from)||!v::finite(to))return d;
    if(!v::finite(base))base={0,0,0};
    V3 rel=v::sub(v::mul(v::sub(to,from),1.f/dtS),base);
    const float s=v::length(rel);
    if(!std::isfinite(s)){d.vel={0,0,0};return d;}
    if(s>maxSpeed){rel=v::mul(rel,maxSpeed/s);d.clamped=true;}
    d.vel=v::add(base,rel);
    return d;
}
// The drive against a surface met in the last 3 substeps (n = out of it): the speed into it is clipped to `creep` for a static surface
// (1.4 mm per 72 Hz substep, under the solver's 7 mm allowed penetration: the body rests on it instead of diving in and being popped
// out), rising to `pushSpeed` with give (0..1, yieldStep: the surface is an active prop that moves away); the part along the surface
// relative to the mover is capped at tangentCap. approach = the commanded speed into the surface before the clip.
struct ContactDrive {V3 vel{};bool clipped{},tangentCapped{};float approach{};};
inline ContactDrive contactDrive(V3 vel,V3 base,V3 n,unsigned age,float give,float creep,float pushSpeed,float tangentCap) noexcept {
    ContactDrive d{};d.vel=vel;
    if(age>3||v::length(n)<.5f||!v::finite(vel))return d;
    if(!v::finite(base))base={0,0,0};
    const float g=std::isfinite(give)?std::clamp(give,0.f,1.f):0.f;
    const float k=creep+(pushSpeed-creep)*g;const float u=v::dot(vel,n);d.approach=-u;
    if(u<-k){d.vel=v::sub(vel,v::mul(n,u+k));d.clipped=true;}
    V3 rel=v::sub(d.vel,base);const float un=v::dot(rel,n);V3 t=v::sub(rel,v::mul(n,un));const float tl=v::length(t);
    if(tl>tangentCap&&tl>0.f){t=v::mul(t,tangentCap/tl);rel=v::add(t,v::mul(n,un));d.vel=v::add(base,rel);d.tangentCapped=true;}
    return d;
}
// One substep's drive: the velocity to the command, its part into a surface clipped first (contactDrive), then the part relative to the
// mover clamped to maxSpeed. Clamped first, a deep press would scale the slide along the surface down with it (30 cm into a wall: to
// about a quarter), and the body would trail a 1 m/s slide along the wall by about 5 cm.
struct SubstepDrive {V3 vel{};bool clamped{},clipped{},tangentCapped{};};
inline SubstepDrive substepDrive(V3 from,V3 to,float dtS,float maxSpeed,V3 base,V3 n,unsigned age,float give,float creep,float pushSpeed,float tangentCap) noexcept {
    SubstepDrive r{};if(!v::finite(base))base={0,0,0};
    const Drive raw=velocityTo(from,to,dtS,std::numeric_limits<float>::max(),base);
    const ContactDrive cd=contactDrive(raw.vel,base,n,age,give,creep,pushSpeed,tangentCap);r.clipped=cd.clipped;r.tangentCapped=cd.tangentCapped;
    V3 rel=v::sub(cd.vel,base);const float s=v::length(rel);
    if(!std::isfinite(s))return r;
    if(s>maxSpeed){rel=v::mul(rel,maxSpeed/s);r.clamped=true;}
    r.vel=v::add(base,rel);
    return r;
}
// The rotation vector (axis * angle) that turns the rows `from` onto the rows `to` (R from_i = to_i): sum from_i x to_i = 2 sin(t) k,
// sum from_i . to_i = 1 + 2 cos(t); near pi the axis is the longest from_i + to_i.
inline V3 rotationBetween(const Rows& from,const Rows& to) noexcept {
    V3 vr{0,0,0};float tr=0.f;
    for(std::size_t i=0;i<3;++i){vr=v::add(vr,v::cross(from[i],to[i]));tr+=v::dot(from[i],to[i]);}
    const float l=v::length(vr),s=l*.5f,c=(tr-1.f)*.5f;
    if(!std::isfinite(s)||!std::isfinite(c))return {0,0,0};
    const float t=std::atan2(s,c);
    if(!(t>1e-6f))return {0,0,0};
    V3 k{};
    if(s>1e-3f||c>0.f)k=v::mul(vr,1.f/std::max(l,1e-12f));
    else{float best=-1.f;for(std::size_t i=0;i<3;++i){const V3 q=v::add(from[i],to[i]);const float ql=v::length(q);if(ql>best){best=ql;k=q;}}
        k=v::normalized(k);if(v::dot(k,vr)<0.f)k=v::mul(k,-1.f);} // the sign from the small cross sum (any sign at exactly pi)
    if(v::length(k)<.5f)return {0,0,0};
    return v::mul(k,t);
}
// The angular velocity (world, rad/s) that turns the body's rows onto the wanted rows in dtS, its magnitude capped.
inline V3 spinFrame(const Rows& now,const Rows& want,float dtS,float cap) noexcept {
    if(!(dtS>1e-5f)||!v::finite(now)||!v::finite(want))return {0,0,0};
    const V3 r=rotationBetween(now,want);const float t=v::length(r);
    if(!(t>1e-6f))return {0,0,0};
    return v::mul(r,std::min(t/dtS,cap)/t);
}
// The frame's angular velocity from two hook A frames, smoothed like targetVelocity.
inline V3 frameAngVel(const Rows& prev,const Rows& rows,float dtS,V3 prevW,bool restart) noexcept {
    if(restart||!(dtS>1e-4f)||dtS>.1f||!v::finite(prev)||!v::finite(rows))return {0,0,0};
    const V3 raw=v::mul(rotationBetween(prev,rows),1.f/dtS);
    const V3 out=v::finite(prevW)?v::add(v::mul(prevW,.5f),v::mul(raw,.5f)):raw;
    return v::finite(out)?out:V3{0,0,0};
}
// The angular velocity (world, rad/s) that turns axisNow onto axisWant in dtS, clamped; the roll about the axis is left alone (a capsule).
inline V3 spinTo(V3 axisNow,V3 axisWant,float dtS,float maxRad) noexcept {
    const V3 a=v::normalized(axisNow),b=v::normalized(axisWant);
    if(!(dtS>1e-5f)||v::length(a)<.5f||v::length(b)<.5f)return {0,0,0};
    const V3 c=v::cross(a,b);const float s=v::length(c),k=v::dot(a,b);
    const float angle=std::atan2(s,k);
    if(!(angle>1e-5f))return {0,0,0};
    V3 axis=s>1e-6f?v::mul(c,1.f/s):v::normalized(v::cross(a,std::fabs(a[1])<.9f?V3{0,1,0}:V3{1,0,0}));
    const float rate=std::min(angle/dtS,maxRad);
    return v::mul(axis,rate);
}
// The engine Matrix34 (rows a, b, c, d; 4 floats each, w unused) of a body at pos whose local Y runs along axis (phBoundCapsule's axis).
inline void writeMatrix(const Rows& r,V3 pos,float* out16) noexcept {
    const V3 rows[4]{r[0],r[1],r[2],pos};
    for(int i=0;i<4;++i){for(int k=0;k<3;++k)out16[i*4+k]=rows[i][std::size_t(k)];out16[i*4+3]=0.f;}
}
inline void bodyMatrix(V3 axis,V3 pos,float* out16) noexcept {
    V3 b=v::normalized(axis);if(v::length(b)<.5f)b={0,1,0};
    const V3 ref=std::fabs(b[1])<.9f?V3{0,1,0}:V3{1,0,0};
    const V3 a=v::normalized(v::cross(b,ref)),c=v::cross(a,b);
    writeMatrix({a,b,c},pos,out16);
}
// The palm body's matrix: rows S, F, N (orthonormalized; a degenerate frame falls back to the capsule frame of F).
inline void palmMatrix(const Rows& r,V3 pos,float* out16) noexcept {
    const V3 F=v::normalized(r[1]);V3 N=v::normalized(v::sub(r[2],v::mul(F,v::dot(r[2],F))));
    if(v::length(F)<.5f||v::length(N)<.5f){bodyMatrix(r[1],pos,out16);return;}
    writeMatrix({v::cross(F,N),F,N},pos,out16);
}
// The capsule's segment (the distance between its sphere centres) for a total length.
inline float capsuleSegment(float lengthM,float radiusM) noexcept {return std::max(0.f,lengthM-2.f*radiusM);}
// The point inside the body cylinder (eye - up * depth .. eye, radius): no surface between the chest and the hand.
inline constexpr float bodyRadiusM=.30f,bodyDepthM=1.f,chestBelowEyeM=.25f;
inline bool insideBody(V3 p,V3 eye,V3 up,float upm) noexcept {
    const V3 d=v::sub(p,eye);const float along=v::dot(d,up);
    const V3 flat=v::sub(d,v::mul(up,along));
    return v::length(flat)<bodyRadiusM*upm&&-along<bodyDepthM*upm&&along<.2f*upm;
}
inline V3 chestPoint(V3 eye,V3 up,float upm) noexcept {return v::sub(eye,v::mul(v::normalized(up),chestBelowEyeM*upm));}

// ============================== contact from the solver (Update, per substep) ==============================
// p = solved - expected (the solver's positional push: phCollider::UpdateImp integrates the position before the collision, and our
// velocity of the next substep replaces the solver's velocity change), dv = the collider's post-solve velocity - the velocity we set.
// Contact: |p| over pushEps (n = p / |p|), else |dv| over dvEps against the set velocity (n = dv / |dv|). A contact within 3 substeps
// of the last one averages the normals (a flip = the two normals over 60 degrees apart). age = substeps since the last contact.
enum ContactKind : std::uint8_t {ContactNone=0,ContactPush=1,ContactDv=2};
struct ContactEst {V3 n{};unsigned age{255};bool dyn{};};
struct ContactNote {ContactKind kind{ContactNone};bool flip{};unsigned prevAge{255};};
inline ContactNote noteSubstep(ContactEst& e,V3 p,V3 dv,V3 vset,float pushEps,float dvEps) noexcept {
    ContactNote r{};r.prevAge=e.age;V3 n{};
    const float pl=v::finite(p)?v::length(p):0.f;
    if(pl>pushEps){n=v::mul(p,1.f/pl);r.kind=ContactPush;}
    else if(v::finite(dv)&&v::finite(vset)){const float dl=v::length(dv);if(dl>dvEps&&v::dot(dv,vset)<0.f){n=v::mul(dv,1.f/dl);r.kind=ContactDv;}}
    if(r.kind==ContactNone){if(e.age<255)++e.age;return r;}
    if(e.age<=3&&v::length(e.n)>.5f){
        if(v::dot(e.n,n)<.5f)r.flip=true;
        const V3 m=v::normalized(v::add(e.n,n));if(v::length(m)>.5f)n=m;
    }
    e.n=n;e.age=0;return r;
}

// Give of the surface under the palm (Update, per substep). A pair with an active prop is only a broadphase overlap, so it cannot tell
// whether the push came from the prop or from a wall next to it. The drive into the surface stays at `creep` until the palm itself sank
// more than sinkM into the surface since the contact began while still advancing (the surface moves away), then give rises by a third
// per substep; two substeps that let through under 30 % of the commanded advance drop it to 0 and re-anchor (the prop stopped or a
// wall took over). A prop that moves away pushes back less than the contact threshold, so the caller also counts a push along n over
// yieldTouchM as a touch here; the track starts on one and lives on for idleMax substeps without one; while it lives the drive keeps
// the last normal. d = n . solved (n out of the surface), advance =
// the commanded move into the surface this substep (>= 0).
struct YieldTrack {float give{},d0{},prev{};unsigned slow{},idle{};bool on{};};
inline constexpr unsigned yieldIdleMax=8;inline constexpr float yieldTouchM=.00005f;
inline void yieldStep(YieldTrack& y,bool candidate,bool contact,float d,float advance,float sinkM) noexcept {
    if(!candidate||!std::isfinite(d)||!std::isfinite(advance)){y=YieldTrack{};return;}
    if(!y.on){if(contact){y=YieldTrack{};y.on=true;y.d0=d;y.prev=d;}return;}
    y.idle=contact?0u:y.idle+1u;
    if(y.idle>yieldIdleMax){y=YieldTrack{};return;}
    const float inc=y.prev-d;y.prev=d;
    if(!(advance>1e-5f))return;                                 // nothing commanded into the surface: hold
    if(inc<.3f*advance){if(++y.slow>=2){y.give=0.f;y.d0=d;y.slow=0;}return;}
    y.slow=0;
    if(y.d0-d>sinkM&&inc>.5f*advance)y.give=std::min(1.f,y.give+.34f);
}

// ============================== visual stop (hook A) ==============================
// The visual hand stops at the solved surface: want = n * clamp(plane + lift - n . B_now, 0, leash) while the body touched a surface in
// the last holdS (B_now = this frame's body centre target). A growing depth applies at once; a shrinking one follows with tau visualS;
// n is blended with the same tau. The rotation offset R_off = R_solved * R_cmd^T (capped) holds while in contact and decays with tau 40 ms.
// Applied to the wrist target: d' = d + offset + (R_off - I)(d - B_now), rot' = R_off * rot (VrBody hook A).
struct VisualMem {V3 n{};float h{};V3 rot{};};
struct VisualOut {V3 offset{},rot{};};
inline constexpr float rotDecayS=.04f;
inline VisualOut visualPlane(VisualMem& m,bool contact,V3 n,float plane,V3 bNow,const Rows& solvedRows,const Rows& cmdRows,float dtS,
                             float lift,float leash,float visualS,float rotCapRad,bool rotation) noexcept {
    VisualOut o{};const float dt=std::clamp(std::isfinite(dtS)?dtS:0.f,0.f,.2f);
    const float a=1.f-std::exp(-dt/std::max(visualS,1e-4f));
    const bool on=contact&&v::length(n)>.5f&&std::isfinite(plane)&&v::finite(bNow);
    float want=0.f;
    if(on){want=std::clamp(plane+lift-v::dot(n,bNow),0.f,std::max(leash,0.f));
        if(m.h<=1e-7f||v::length(m.n)<.5f)m.n=n;else{const V3 b=v::normalized(v::add(m.n,v::mul(v::sub(n,m.n),a)));m.n=v::length(b)>.5f?b:n;}}
    if(!std::isfinite(m.h))m.h=0.f;
    if(want>=m.h)m.h=want;else{m.h+=(want-m.h)*a;if(m.h<1e-6f)m.h=0.f;}
    if(on&&rotation&&v::finite(solvedRows)&&v::finite(cmdRows)){
        V3 r=rotationBetween(cmdRows,solvedRows);const float t=v::length(r);
        if(t>rotCapRad&&t>0.f)r=v::mul(r,rotCapRad/t);
        m.rot=r;
    }else{m.rot=v::mul(m.rot,std::exp(-dt/rotDecayS));if(!v::finite(m.rot)||v::length(m.rot)<1e-5f)m.rot={0,0,0};}
    o.offset=v::length(m.n)>.5f?v::mul(m.n,m.h):V3{0,0,0};o.rot=m.rot;
    return o;
}
// GRAB: the visual hand at the solved body (the held prop hangs on it): offset = solved - B_now capped at the leash, rotation offset
// R_solved R_cmd^T capped at rotCapRad; the contact plane memory is cleared.
inline VisualOut grabVisual(VisualMem& m,V3 solvedPos,V3 bNow,const Rows& solvedRows,const Rows& cmdRows,float leash,float rotCapRad) noexcept {
    VisualOut o{};m=VisualMem{};
    if(!v::finite(solvedPos)||!v::finite(bNow))return o;
    V3 d=v::sub(solvedPos,bNow);const float l=v::length(d);if(l>leash&&l>0.f)d=v::mul(d,leash/l);
    o.offset=v::finite(d)?d:V3{0,0,0};
    if(v::finite(solvedRows)&&v::finite(cmdRows)){V3 r=rotationBetween(cmdRows,solvedRows);const float t=v::length(r);if(t>rotCapRad&&t>0.f)r=v::mul(r,rotCapRad/t);o.rot=r;}
    return o;
}

// ============================== body state (tick) ==============================
inline constexpr std::uint64_t removeAfterNs=1'000'000'000ull,parkRemoveNs=5'000'000'000ull,travelNs=300'000'000ull,returnHoldNs=100'000'000ull;
inline constexpr float travelDoneM=.05f;
struct MachineIn {
    bool want{};          // mode ON, class body, on foot, not carried, targets fresh or within staleMs
    bool present{};       // the instance is in the physics level (an engine reset clears it)
    bool targetInBody{};  // the target is inside the body cylinder
    bool jump{};          // an eye jump over originJumpM
    bool returnOk{};      // parked: the target is back within returnM of the parked body, on its free side
    float devM{};         // |solved - command| of the last substep, metres (a large value before the first one)
    std::uint64_t now{};
};
enum Action : std::uint8_t {ActNone=0,ActInsert,ActRespawn,ActGhost,ActRemove,ActPark,ActUnpark};
inline constexpr std::array<const char*,7> actionNames{"NONE","INSERT","RESPAWN","GHOST","REMOVE","PARK","UNPARK"};
struct Machine {BodyState state{BodyAbsent};std::uint64_t sinceNs{},overNs{},ghostNs{},returnNs{};bool over{},leashGhost{},returning{};};
// Insert and respawn put the body at the chest point (inside the player's own mover, which it never meets) and let it travel to the target
// with collisions on: it cannot end up on the far side of a wall. A body further than the leash from its target for leashMs parks where it
// stopped (pairs dropped, still); it comes back in place once the target returns to it on its free side for 100 ms, or from the chest
// once the target is inside the body cylinder. A park over 5 s, or a ghost over 1 s, leaves the level.
inline Action step(Machine& m,const MachineIn& in,const Levers& l) noexcept {
    const auto since=[&](std::uint64_t t){return in.now>t?in.now-t:0ull;};
    if(!in.present&&m.state!=BodyAbsent)m.state=BodyAbsent;
    if(!in.want){
        m.over=false;m.returning=false;
        if(m.state==BodyLive||m.state==BodyTravel||m.state==BodyPark){m.state=BodyGhost;m.ghostNs=in.now;m.leashGhost=false;return ActGhost;}
        if(m.state==BodyGhost&&since(m.ghostNs)>=removeAfterNs){m.state=BodyAbsent;return ActRemove;}
        return ActNone;
    }
    const auto leashNs=std::uint64_t(double(l.leashMs)*1e6);
    switch(m.state){
    case BodyAbsent:
        if(m.leashGhost&&!in.targetInBody)return ActNone;
        m.state=BodyTravel;m.sinceNs=in.now;m.leashGhost=false;m.over=false;return ActInsert;
    case BodyGhost:
        m.state=BodyTravel;m.sinceNs=in.now;m.leashGhost=false;m.over=false;return ActRespawn;
    case BodyPark:
        if(in.jump){m.state=BodyTravel;m.sinceNs=in.now;m.returning=false;return ActRespawn;}
        if(in.returnOk){
            if(!m.returning){m.returning=true;m.returnNs=in.now;}
            if(since(m.returnNs)>=returnHoldNs){m.state=BodyLive;m.returning=false;m.over=false;return ActUnpark;}
        }else m.returning=false;
        if(in.targetInBody){m.state=BodyTravel;m.sinceNs=in.now;m.returning=false;return ActRespawn;}
        if(since(m.ghostNs)>=parkRemoveNs){m.state=BodyAbsent;m.leashGhost=true;m.returning=false;return ActRemove;}
        return ActNone;
    case BodyTravel:
        if(in.jump){m.sinceNs=in.now;return ActRespawn;}
        if(in.devM<travelDoneM||since(m.sinceNs)>=travelNs){m.state=BodyLive;m.over=false;}
        return ActNone;
    case BodyLive:
        if(in.jump){m.state=BodyTravel;m.sinceNs=in.now;m.over=false;return ActRespawn;}
        if(in.devM>l.leashM){
            if(!m.over){m.over=true;m.overNs=in.now;}
            if(since(m.overNs)>=leashNs){m.state=BodyPark;m.ghostNs=in.now;m.over=false;m.returning=false;return ActPark;}
        }else m.over=false;
        return ActNone;
    }
    return ActNone;
}
// The in-place return of a parked body: the target within returnM of it and not more than 1 cm behind its contact plane.
inline bool parkReturn(V3 target,V3 parkPos,V3 parkN,float returnM,float upm) noexcept {
    const V3 d=v::sub(target,parkPos);
    return v::finite(d)&&v::length(d)<returnM*upm&&v::dot(parkN,d)>=-.01f*upm;
}

// ============================== pair filter (CollideObjects) ==============================
struct PairIn {bool ghost{},player{},otherHand{},owned{};int otherClass{-1};bool npc{};};
// player = the player's mover instance; owned = an instance whose user is the player's actor (his body frag, his ragdoll);
// a live NPC or horse mover only with the npc lever.
inline constexpr bool collide(const PairIn& p) noexcept {return !p.ghost&&!p.player&&!p.otherHand&&!p.owned&&(p.otherClass!=classMover||p.npc);}

// ============================== collision flags ==============================
// A level object pairs with a probe or another object when (obj.include & test.type) and (obj.type & test.include) are both non-zero and
// (obj.type & test.typeExclude) == 0 (phLevelNew CheckInstance 0x1c25aa8, ProcessOverlaps the same mutual test).
// type = the mover's type without the forbidden bits, plus 0x1 (static world, doors and props pair through it) and 0x800000 (the bit the
// mover ground probe, foot and corpse probes exclude). Forbidden: 0x2 (camera include), every bit of the bullet include 0x4500208 and
// the reticle include 0x500208 (0x8, 0x200, 0x100000, 0x400000, 0x4000000), 0x1000 (player), 0x2000 (actor mover), 0x20000 (projectiles),
// 0x80000 (AI line of sight), 0x2000000 (water / terrain probes). A lever value replaces the derived one, masked the same way; 0 = no body.
// include = the mover's include without 0x8, 0x200 (rope), 0x20000 (projectiles), 0x400000 (water), 0x800000 and 0x1000, plus 0x800
// (dead bodies); a lever value replaces it.
inline constexpr std::uint32_t typeForbidden=0x2u|0x8u|0x200u|0x1000u|0x2000u|0x20000u|0x80000u|0x100000u|0x400000u|0x2000000u|0x4000000u,
    typeAdd=0x1u|0x800000u;
inline constexpr std::uint32_t includeDrop=0x8u|0x200u|0x1000u|0x20000u|0x400000u|0x800000u,includeAdd=0x800u;
inline constexpr std::uint32_t handType(std::uint32_t mover,std::uint32_t lever) noexcept {return lever?lever&~typeForbidden:(mover&~typeForbidden)|typeAdd;}
inline constexpr std::uint32_t handInclude(std::uint32_t mover,std::uint32_t lever) noexcept {return lever?lever:((mover&~includeDrop)|includeAdd);}

// ============================== haptics, doors, props ==============================
inline constexpr float triggerBusy=.5f;
inline float pulseAmplitude(float closingMps) noexcept {return .12f+.38f*std::clamp(std::isfinite(closingMps)?closingMps/1.5f:0.f,0.f,1.f);}
// Recently opened doors: one Open per door per gapNs.
struct DoorGate {
    static constexpr std::uint64_t gapNs=1'000'000'000ull;
    std::array<std::uintptr_t,4> door{};std::array<std::uint64_t,4> ns{};unsigned next{};
    bool allow(std::uintptr_t d,std::uint64_t now) noexcept {
        for(unsigned i=0;i<door.size();++i)if(door[i]==d&&now>=ns[i]&&now-ns[i]<gapNs)return false;
        door[next]=d;ns[next]=now;next=(next+1u)%unsigned(door.size());return true;
    }
};
// propDoorInst state +0x58 (IsClosed < 2, IsLocked 1, IsOpened 4; Open acts on 0, Close on 4).
enum DoorAct : std::uint8_t {DoorDoOpen=0,DoorDoClose,DoorLocked,DoorBusy,DoorOpened};
inline constexpr DoorAct doorAction(std::uint32_t state,bool closeLever) noexcept {
    return state==0?DoorDoOpen:state==1?DoorLocked:state==4?(closeLever?DoorDoClose:DoorOpened):DoorBusy;
}

// ============================== DOOR SWING (tick) ==============================
// A pivot door is kinematic: PivotDoorInterface::ContinueActionShared 0x2a7710c re-poses it every frame from the phase t (iface+0x24):
// angle = e(t) * cap (cap = iface+0x28[dir]), with e = the ease below while opening and smoothstep while closing (state 5). The
// opening phase advances by speed (iface+0x20) * dt (ContinueOpenDerived 0x2a773bc), so speed 0 holds the door at t and t written by
// the tick is the door's angle in the next update.
inline float easeOpen(float t) noexcept {return t*((t*t)*1.42f+t*-3.835f+3.409f);} // 0x3fb5c28f, 0xc07570a4, 0x405a2d0e
inline float smooth(float t) noexcept {return t*t*(t*-2.f+3.f);}
inline constexpr float easeOpenTop=.8f; // e is monotonic on [0, 0.8], e(0.8) = 0.9998; state 2 turns to 3 at t >= 0.8
// The phase whose ease is frac (24 bisection steps; opening on [0, 0.8], closing on [0, 1]).
inline float phaseFor(float frac,bool closing) noexcept {
    const float top=closing?1.f:easeOpenTop;const float hiE=closing?1.f:easeOpen(easeOpenTop);
    const float f=std::isfinite(frac)?std::clamp(frac,0.f,hiE):0.f;
    float lo=0.f,hi=top;
    for(int i=0;i<24;++i){const float mid=.5f*(lo+hi);if((closing?smooth(mid):easeOpen(mid))<f)lo=mid;else hi=mid;}
    return .5f*(lo+hi);
}
// The palm in the door's frame (local = R^T (palm - frag origin); the origin is on the hinge axis, local Y along it, local Z through
// the leaf) against the leaf's bounding box (world units, bound +0x40 min / +0x30 max), or without one a lever / height range.
// face = +1: a push from this side opens the door further (pushSign = -1 for a door opened towards +Z, +1 towards -Z).
enum DoorReject : std::uint8_t {DoorGeomOk=0,DoorGeomLever,DoorGeomHeight,DoorGeomBox};
struct DoorGeom {bool ok{};float leverM{},heightM{};int face{};DoorReject reject{DoorGeomOk};};
inline DoorGeom doorGeom(V3 local,V3 bmin,V3 bmax,bool haveBox,int pushSign,float upm) noexcept {
    DoorGeom g{};const float u=std::max(upm,1e-3f);
    if(!v::finite(local)){g.reject=DoorGeomBox;return g;}
    const float lever=std::sqrt(local[0]*local[0]+local[2]*local[2])/u;
    g.heightM=local[1]/u;g.leverM=std::max(lever,.15f);g.face=(local[2]>=0.f?1:-1)*(pushSign<0?-1:1);
    if(haveBox&&v::finite(bmin)&&v::finite(bmax)&&bmax[0]>bmin[0]&&bmax[1]>bmin[1]){
        const float zMax=std::max(std::fabs(bmin[2]),std::fabs(bmax[2]))+.20f*u;
        if(local[0]<bmin[0]-.10f*u||local[0]>bmax[0]+.10f*u||std::fabs(local[2])>zMax){g.reject=DoorGeomBox;return g;}
        if(local[1]<bmin[1]-.10f*u||local[1]>bmax[1]+.10f*u){g.reject=DoorGeomHeight;return g;}
    }else{
        if(lever<.10f||lever>1.60f){g.reject=DoorGeomLever;return g;}
        if(g.heightM<-.3f||g.heightM>3.f){g.reject=DoorGeomHeight;return g;}
    }
    g.ok=true;return g;
}
// One held door per tick. theta = the leaf's angle from closed (0..cap), omega = its angular speed (rad/s). A push turns it by the angle
// that takes the leaf to the palm (atan(depth / lever), capped at rate * dt) and gives it the push's speed; free, it coasts and slows
// (e^-3t). Released (no push and no swing for doorIdleMs, the player over 3 m away, or a hold over 20 s): a closing hold and a door
// open past keep * cap go back to the engine (Finish); a door below that glides shut at 1.2 rad/s, then closes (FastClose). Pushing an
// opening door shut from its far side closes it (FastClose; the next push opens it the other way). keepOpen (a door the game was
// already opening, taken over): released, it always goes back to the engine, which finishes opening it (no glide into the player).
enum DoorCmd : std::uint8_t {DoorCmdHold=0,DoorCmdFinish,DoorCmdFastClose};
struct DoorSwing {float theta{},omega{};std::uint64_t lastPushNs{},startNs{};bool closing{},glide{},reversed{},keepOpen{};};
inline constexpr float doorOneDeg=pi/180.f,doorGlideRadS=1.2f,doorFarM=3.f;
inline constexpr std::uint64_t doorHoldMaxNs=20'000'000'000ull,doorGapNs=150'000'000ull,doorGameGapNs=1'000'000'000ull;
inline constexpr std::uint64_t doorNoteNs=100'000'000ull; // DOOR PRESS: refused starts sampled at most once per hand and door per 100 ms
inline DoorCmd doorSwingStep(DoorSwing& s,bool push,float depthM,float leverM,int face,float cap,float dtS,std::uint64_t now,bool playerFar,
                             const Levers& l,float& tOut) noexcept {
    const float c=std::isfinite(cap)&&cap>1e-3f?cap:pi;
    const float dt=std::isfinite(dtS)?std::clamp(dtS,0.f,.1f):0.f;
    if(!std::isfinite(s.theta))s.theta=0.f;
    if(!std::isfinite(s.omega))s.omega=0.f;
    s.reversed=false;
    const bool pushing=push&&std::isfinite(depthM)&&depthM>0.f&&std::isfinite(leverM)&&leverM>0.f;
    if(pushing){
        float d=float(face<0?-1:1)*std::atan2(depthM,leverM);
        const float lim=l.doorRateRadS*dt;d=std::clamp(d,-lim,lim);
        s.theta+=d;s.omega=dt>1e-4f?.5f*s.omega+.5f*d/dt:s.omega;s.lastPushNs=now;s.glide=false;
    }else if(s.glide){s.theta-=doorGlideRadS*dt;s.omega=0.f;}
    else{s.theta+=s.omega*dt;s.omega*=std::exp(-3.f*dt);if(std::fabs(s.omega)<.05f)s.omega=0.f;}
    if(s.theta<=0.f){s.theta=0.f;s.omega=0.f;}
    if(s.theta>=c){s.theta=c;s.omega=0.f;}
    tOut=phaseFor(s.theta/c,s.closing);
    if(!s.closing&&pushing&&face<0&&s.theta<=doorOneDeg){s.reversed=true;return DoorCmdFastClose;}
    const std::uint64_t last=s.lastPushNs?s.lastPushNs:s.startNs;
    const bool idle=now>=last&&double(now-last)>double(l.doorIdleMs)*1e6&&s.omega==0.f;
    const bool old=now>=s.startNs&&now-s.startNs>doorHoldMaxNs;
    if(!s.glide&&(idle||playerFar||old)){
        if(s.closing||s.keepOpen||s.theta>=l.doorKeep*c)return DoorCmdFinish;
        s.glide=true;
    }
    if(s.glide&&!s.closing&&s.theta<=doorOneDeg)return DoorCmdFastClose;
    return DoorCmdHold;
}

// ============================== DOORS: the leaf follows the palm (tick) ==============================
// The leaf frame: closed rows a0 b0 c0 (propDoorInst +0x0..+0x20), current rows a b c and origin o (the frag matrix, o on the hinge
// axis), the box of the frag's bound (instance frame). The leaf runs along L = sx a from the hinge (sx = the side of the box centre);
// sure = a box, its centre 5 cm or more off the hinge, the hinge upright. width = the box's reach from the hinge along x, halfZ = its
// half thickness (3 cm without one).
struct Leaf {V3 o{},a{},b{},c{},L{},L0{},c0{};float sx{1.f},width{},yMin{},yMax{},halfZ{.03f};bool sure{},upright{},haveBox{};};
inline constexpr float leafRMinM=.12f,leafEdgeM=.03f,leafYMarginM=.05f,leafZMarginM=.06f,palmHalfM=.014f,leafCentreMinM=.05f;
inline Leaf leafFrame(const Rows& closed,const Rows& now,V3 o,V3 bmin,V3 bmax,bool haveBox,V3 up,float upm) noexcept {
    Leaf f{};const float u=std::max(upm,1e-3f);
    f.o=o;f.a=v::normalized(now[0]);f.b=v::normalized(now[1]);f.c=v::normalized(now[2]);f.c0=v::normalized(closed[2]);
    f.upright=std::fabs(v::dot(f.b,v::normalized(up)))>=.9f;
    f.haveBox=haveBox&&v::finite(bmin)&&v::finite(bmax)&&bmax[0]>bmin[0]&&bmax[1]>bmin[1];
    const float cx=f.haveBox?.5f*(bmin[0]+bmax[0]):0.f;
    f.sx=cx<0.f?-1.f:1.f;f.L=v::mul(f.a,f.sx);f.L0=v::mul(v::normalized(closed[0]),f.sx);
    if(f.haveBox){f.width=std::max(std::fabs(bmin[0]),std::fabs(bmax[0]));f.yMin=bmin[1];f.yMax=bmax[1];
        const float hz=std::max(std::fabs(bmin[2]),std::fabs(bmax[2]));f.halfZ=std::isfinite(hz)&&hz>0.f?hz:.03f*u;}
    f.sure=f.haveBox&&std::fabs(cx)>=leafCentreMinM*u&&f.upright&&v::finite(o)&&v::length(f.L)>.5f&&v::length(f.L0)>.5f&&v::length(f.c0)>.5f;
    return f;
}
// The palm relative to the leaf: r = its distance from the hinge axis, y = its height along it, delta = the angle from the leaf to the
// palm about the open axis kOpen (> 0: on the side the leaf opens towards). on = within the leaf's reach, height and thickness.
struct LeafPalm {float r{},y{},delta{};bool on{};};
inline LeafPalm leafPalm(const Leaf& f,V3 kOpen,V3 p,float upm) noexcept {
    LeafPalm lp{};const float u=std::max(upm,1e-3f);if(!v::finite(p))return lp;
    const V3 d=v::sub(p,f.o);lp.y=v::dot(d,f.b);const V3 pp=v::sub(d,v::mul(f.b,lp.y));lp.r=v::length(pp);
    lp.delta=std::atan2(v::dot(kOpen,v::cross(f.L,pp)),v::dot(f.L,pp));
    lp.on=std::isfinite(lp.delta)&&lp.r>=leafRMinM*u&&lp.r<=f.width+leafEdgeM*u&&lp.y>=f.yMin-leafYMarginM*u&&lp.y<=f.yMax+leafYMarginM*u&&
          std::fabs(lp.delta)<=std::atan((f.halfZ+leafZMarginM*u)/std::max(lp.r,1e-4f));
    return lp;
}
// The angle the palm's half thickness and the leaf's half thickness keep between the leaf and the palm centre.
inline float leafClearance(float halfZ,float r,float upm) noexcept {return std::atan((halfZ+palmHalfM*std::max(upm,1e-3f))/std::max(r,1e-4f));}
// The open axis: measured from the leaf's turn (L0 x L), or before it turned from the push (the leaf swings away from the pusher).
inline V3 openAxisMeasured(V3 L0,V3 L) noexcept {return v::normalized(v::cross(L0,L));}
inline V3 openAxisPush(V3 L0,V3 c0,V3 palmRel) noexcept {const float s=v::dot(palmRel,c0);return v::normalized(v::cross(L0,v::mul(c0,s>=0.f?-1.f:1.f)));}
// The leaf's target angle: q < 0 (the palm on the closed side) only opens it, q > 0 only closes it; the leaf keeps the clearance to the
// command, its change capped at rate * dt, in [0, 0.9998 cap].
inline float doorTarget(int q,float thetaNow,float deltaCmd,float clear,float cap,float rate,float dtS) noexcept {
    if(!std::isfinite(thetaNow)||!std::isfinite(deltaCmd)||!std::isfinite(clear))return thetaNow;
    float t=q<0?std::max(thetaNow,thetaNow+deltaCmd+clear):std::min(thetaNow,thetaNow+deltaCmd-clear);
    const float lim=std::max(0.f,rate)*(std::isfinite(dtS)?std::clamp(dtS,0.f,.1f):0.f);
    t=std::clamp(t,thetaNow-lim,thetaNow+lim);
    return std::clamp(t,0.f,.9998f*std::max(cap,0.f));
}
// A part-open door rests in state 4 with an opening phase; the engine's own close reads the phase as a smoothstep: the phase whose
// smoothstep is the same angle.
inline float restRemapPhase(float lastT) noexcept {return phaseFor(easeOpen(std::clamp(std::isfinite(lastT)?lastT:0.f,0.f,1.f)),true);}
// ============================== DOORS: the hand's parts against the leaf (tick) ==============================
// A door push counts only when one of the hand's PALM parts (the palm, the open fingers or the fist, VrHandPhysMath palmParts) is within
// doorTouchM of the leaf slab (its centre's distance to the leaf plane minus its box's support along the leaf normal minus the leaf's half
// thickness) inside the leaf's reach and height, and the contact normal is within 45 degrees of the leaf normal: a hand on the frame or
// the wall beside the door, or trailing a leaf the game swings away, pushes nothing. A posed leaf keeps its hold while a part is within
// doorKeepOnM. A posed leaf never moves into the player's mover (moverAngle / closeStop / openStop: a closing push stops where the leaf
// meets him, so it never shuts on him in the doorway; a mover on the face the leaf moves away from never stops it). The swing disc: the
// mover within the leaf's width + doorDiscM of the hinge; a door left part-open under doorRaiseDeg is posed to it once the mover comes
// into its disc and is not in the wedge that raise sweeps (state 4 cannot be opened by walking into it: propDoorInst::Open 0x286b320
// returns unless the state +0x58 is 0).
inline constexpr float doorTouchM=.015f,doorKeepOnM=.02f,doorNormalCos=.7071f,doorDiscM=.5f,doorRaiseDeg=45.f;
struct HandBoxes {std::array<V3,3> c{},half{};V3 S{1,0,0},F{0,1,0},N{0,0,1};unsigned mask{};};
inline HandBoxes handBoxes(V3 b,const Rows& rows,bool fist,float upm) noexcept {
    HandBoxes hb{};const V3 F=v::normalized(rows[1]);const V3 N=v::normalized(v::sub(rows[2],v::mul(F,v::dot(rows[2],F))));
    if(v::length(F)<.5f||v::length(N)<.5f||!v::finite(b))return hb;
    const V3 S=v::cross(F,N);hb.S=S;hb.F=F;hb.N=N;
    for(std::size_t i=0;i<3;++i){const auto& p=palmParts[i];
        hb.c[i]=v::add(b,v::add(v::add(v::mul(S,p.offset[0]*upm),v::mul(F,p.offset[1]*upm)),v::mul(N,p.offset[2]*upm)));hb.half[i]=v::mul(p.size,.5f*upm);}
    hb.mask=1u|(fist?4u:2u);return hb;
}
// A part's half extent along a unit direction (its box's support).
inline float boxExtent(const HandBoxes& hb,std::size_t i,V3 n) noexcept {
    return std::fabs(v::dot(hb.S,n))*hb.half[i][0]+std::fabs(v::dot(hb.F,n))*hb.half[i][1]+std::fabs(v::dot(hb.N,n))*hb.half[i][2];}
// The part nearest the leaf slab within its reach (x from the hinge along the leaf, y along the hinge): gap (world units, < 0 = inside),
// ext = its support along the leaf normal, side = the face it is on (-1 = -c, +1 = +c).
// edge (DOOR PRESS): a part whose centre is past the free edge (x > width) is measured to the edge: its near face still past it -> the
// distance to the edge line (hypot of the gap along the leaf and across it, never inside); straddling the edge -> the gap across the
// leaf with its support capped at the palm's half thickness (a hand pressing the edge face drives the leaf without a jump); straddle set.
struct LeafTouch {bool ok{};int part{-1};float gap{1e30f},ext{},x{},y{};V3 centre{};int side{};bool straddle{};};
inline LeafTouch leafTouch(const Leaf& f,const HandBoxes& hb,float upm,bool edge=false) noexcept {
    LeafTouch t{};const float u=std::max(upm,1e-3f);
    if(!hb.mask||v::length(f.c)<.5f||v::length(f.L)<.5f)return t;
    for(std::size_t i=0;i<3;++i){if(!(hb.mask>>i&1u))continue;
        const V3 d=v::sub(hb.c[i],f.o);const float z=v::dot(d,f.c),x=v::dot(d,f.L),y=v::dot(d,f.b);
        float ext=boxExtent(hb,i,f.c);const float extL=boxExtent(hb,i,f.L),extB=boxExtent(hb,i,f.b);
        if(!std::isfinite(z)||x<.5f*leafRMinM*u||x-extL>f.width+leafEdgeM*u||y+extB<f.yMin-leafYMarginM*u||y-extB>f.yMax+leafYMarginM*u)continue;
        float gap=std::fabs(z)-ext-f.halfZ;bool straddle=false;
        if(edge&&x>f.width){const float dx=x-extL-f.width;
            if(dx>0.f)gap=std::hypot(dx,std::max(0.f,gap));
            else{ext=std::min(ext,palmHalfM*u);gap=std::fabs(z)-ext-f.halfZ;straddle=true;}}
        if(gap<t.gap){t.ok=true;t.part=int(i);t.gap=gap;t.ext=ext;t.x=x;t.y=y;t.centre=hb.c[i];t.side=z<0.f?-1:1;t.straddle=straddle;}}
    return t;
}
// DOOR PRESS: the hold's side q (the sign leafPalm's delta has for a point on face sideZ of the leaf: z along the leaf normal c) for the
// open axis k: q = sideZ x sign(k . (L x c)).
inline int doorQOf(const Leaf& f,V3 k,int sideZ) noexcept {const float s=v::dot(k,v::cross(f.L,f.c));return (sideZ<0?-1:1)*(s<0.f?-1:1);}
// The face a hand works a leaf from, locked from its approach: the commanded part outside the slab (gap >= 0) within doorSideReachM
// sets it; inside the slab it is kept (and refreshed); the other face needs two outside samples in a row and the solved part (the body
// the leaf's collision keeps on its face) on that face too, or off the leaf (confirm 0): the hand went round the edge, not through a
// leaf that stopped. fresh = seen within doorSideFreshNs on this door.
inline constexpr float doorSideReachM=.25f,doorPressKeepM=.03f;inline constexpr std::uint64_t doorSideFreshNs=300'000'000ull;
struct DoorSideTrack {std::uintptr_t nav{};int side{};std::uint64_t ns{};int pend{};unsigned pendN{};
    bool fresh(std::uintptr_t n,std::uint64_t now) const noexcept {return nav&&nav==n&&side&&now>=ns&&now-ns<=doorSideFreshNs;}
    // true = the side changed (a switch of a fresh track)
    bool note(std::uintptr_t n,const LeafTouch& lt,float upm,std::uint64_t now,int confirm=0) noexcept {
        if(!lt.ok)return false;const float u=std::max(upm,1e-3f);
        if(lt.gap<0.f){if(fresh(n,now)){ns=now;pendN=0;}return false;}
        if(lt.gap>doorSideReachM*u)return false;
        if(!fresh(n,now)){nav=n;side=lt.side;ns=now;pendN=0;pend=0;return false;}
        ns=now;if(lt.side==side){pendN=0;return false;}
        if(pend==lt.side)++pendN;else{pend=lt.side;pendN=1;}
        if(pendN>=2&&(!confirm||confirm==lt.side)){side=lt.side;pendN=0;pend=0;return true;}
        return false;}
};
// The angle the leaf keeps from a part's centre: the leaf's and the part's half thickness at the part's distance from the hinge.
inline float partClearance(float halfZ,float ext,float r) noexcept {return std::atan((halfZ+ext)/std::max(r,1e-4f));}
// The player's mover within the swing disc of a leaf (horizontal distance to the hinge axis).
inline bool inSwingDisc(V3 mover,V3 hinge,V3 up,float width,float upm) noexcept {
    const V3 u=v::normalized(up),d=v::sub(mover,hinge);const V3 flat=v::sub(d,v::mul(u,v::dot(d,u)));
    return v::finite(flat)&&v::length(flat)<=width+doorDiscM*upm;}
// The leaf against the player's mover (a vertical capsule of doorMoverRadiusM round its centre): the mover's angle psi about the hinge
// from the closed leaf L0, in the opening sense of the open axis k ((-pi, pi], the leaf itself at theta), and the capsule's angular half
// width at its distance. atHinge = the capsule holds the hinge (every move of the leaf meets it); beyond = past the leaf's reach (no move
// meets it); ok false = no sure geometry (k not along the hinge).
inline constexpr float doorMoverRadiusM=.35f;
struct MoverAngle {bool ok{},atHinge{},beyond{};float psi{},margin{},r{};};
inline V3 leafAt(V3 L0,V3 k,float theta) noexcept {
    const V3 kn=v::normalized(k);return v::add(v::mul(L0,std::cos(theta)),v::mul(v::cross(kn,L0),std::sin(theta)));}
inline MoverAngle moverAngle(V3 mover,V3 hinge,V3 up,V3 L0,V3 k,float width,float upm) noexcept {
    MoverAngle m{};const float u=std::max(upm,1e-3f);const V3 un=v::normalized(up),kn=v::normalized(k);
    if(v::length(un)<.5f||v::length(kn)<.5f||std::fabs(v::dot(kn,un))<.95f||!std::isfinite(width))return m;
    const V3 d=v::sub(mover,hinge);const V3 p=v::sub(d,v::mul(un,v::dot(d,un)));const V3 l=v::normalized(v::sub(L0,v::mul(un,v::dot(L0,un))));
    if(!v::finite(p)||v::length(l)<.5f)return m;
    const float r=v::length(p),rho=doorMoverRadiusM*u;m.ok=true;m.r=r;
    if(r>width+rho){m.beyond=true;return m;}
    if(r<=rho){m.atHinge=true;return m;}
    m.psi=std::atan2(v::dot(kn,v::cross(l,p)),v::dot(l,p));m.margin=std::asin(std::min(1.f,rho/r));
    return m;
}
// The leaf posed from `from` to `to` (radians from the closed leaf) in one move meets the mover: opening, psi in [from, to + margin);
// closing, psi in (to - margin, from] (a mover on the face the leaf moves away from is never met). No sure geometry: met.
inline bool sweepMeets(const MoverAngle& m,float from,float to) noexcept {
    if(!m.ok||!std::isfinite(from)||!std::isfinite(to))return true;
    if(m.beyond)return false;
    if(m.atHinge)return to!=from;
    return to>from?(m.psi>=from&&m.psi<to+m.margin):to<from?(m.psi>to-m.margin&&m.psi<=from):false;
}
// The lowest angle a closing push from thetaNow may reach: the leaf stops where it meets a mover standing in its closing wedge (psi +
// margin, at least shutRad + 1 degree so it never shuts on him, at most thetaNow); 0 = nothing in the way (no geometry: thetaNow).
inline float closeStop(const MoverAngle& m,float thetaNow,float shutRad) noexcept {
    if(!m.ok)return std::max(thetaNow,0.f);
    if(m.beyond)return 0.f;
    if(m.atHinge)return std::max(thetaNow,0.f);
    if(m.psi>thetaNow||m.psi+m.margin<=0.f)return 0.f;
    return std::clamp(m.psi+m.margin,shutRad+pi/180.f,std::max(thetaNow,shutRad+pi/180.f));
}
// The highest angle an opening push from thetaNow may reach: the leaf stops where it meets a mover standing in its opening wedge
// (psi - margin, at least thetaNow); +inf = nothing in the way (no geometry: thetaNow).
inline float openStop(const MoverAngle& m,float thetaNow) noexcept {
    if(!m.ok||m.atHinge)return thetaNow;
    if(m.beyond||m.psi<thetaNow)return std::numeric_limits<float>::infinity();
    return std::max(thetaNow,m.psi-m.margin);
}
// The raise (a rested leaf posed from theta to theta + deltaRad in one tick) may go: the mover is not in the wedge it sweeps.
inline bool raiseClear(V3 mover,V3 hinge,V3 up,V3 L0,V3 k,float theta,float width,float deltaRad,float upm) noexcept {
    return !sweepMeets(moverAngle(mover,hinge,up,L0,k,width,upm),theta,theta+std::max(deltaRad,0.f));}
// ============================== DOORS: the player's approach and the mover's push (tick) ==============================
// sagPlayerInputDoor::AddUseContext 0x26af554 calls FindDoor 0x26af868 (GOT 0x2e53628) every frame on foot and on horseback; a door it
// returns with the mode word 0 ([[[actor+0x38]+0xab8]+0x18]+0x18) is the player's automatic walk-in open (UseDelayDoorExecute, then
// SetupUsePacket's OpenDoor, and the automatic Close of an open one at 0x26af85c); a word other than 0 adds the Open_Door prompt instead.
// The hook answers "no door" (AddUseContext 0x26af680: no context, no packet) only for that automatic case while the hands are live on
// foot; every other caller and case passes the original's answer through.
enum FindDoorWhy : std::uint8_t {FindSuppress=0,FindNotFound,FindLever,FindNotLive,FindNotFoot,FindNoMode,FindPrompt,FindWhyCount};
inline constexpr std::array<const char*,FindWhyCount> findDoorNames{"suppressed","notFound","lever","notLive","notFoot","noMode","prompt"};
inline constexpr FindDoorWhy findDoorGate(bool found,bool leverAuto,bool live,bool foot,bool modeRead,std::uint32_t mode) noexcept {
    return !found?FindNotFound:leverAuto?FindLever:!live?FindNotLive:!foot?FindNotFoot:!modeRead?FindNoMode:mode!=0?FindPrompt:FindSuppress;}
// The mover (the vertical capsule of doorMoverRadiusM) walking into a leaf moves it ahead of him: the leaf keeps doorBodyClearM between
// its face (halfZ from its centre plane) and the capsule's edge, so the door opens only as far as he walks. A push acts only with the
// capsule's edge within doorBodyLeadM of the face: from the closed side (psi < theta) it opens the leaf (at most 0.9998 cap), from the
// open side (psi > theta) it closes it, never below doorShut + 1 degree (the body never shuts a door: atFloor = a closing push held
// there). Each tick's change is capped at rate * dt. side: -1 opening, +1 closing, 0 = not in the band; gapM = the capsule's edge to the
// face (m, < 0 = into the leaf; +inf without geometry).
inline constexpr float doorBodyLeadM=.05f,doorBodyClearM=.03f;
struct BodyPush {bool inBand{},atFloor{};int side{};float target{},gapM{std::numeric_limits<float>::infinity()};};
// The capsule's edge to the leaf face (m): the capsule centre's distance from the leaf's centre plane (from its hinge line past 90
// degrees) less the capsule radius and the leaf's half thickness (world units halfZW).
inline float bodyGapM(const MoverAngle& m,float theta,float halfZW,float upm) noexcept {
    if(!m.ok||m.beyond||!std::isfinite(m.psi)||!std::isfinite(theta)||!(m.r>=0.f))return std::numeric_limits<float>::infinity();
    const float u=std::max(upm,1e-3f),a=std::fabs(theta-m.psi),hz=std::isfinite(halfZW)?std::max(halfZW,0.f):0.f;
    const float d=a<pi/2?m.r*std::sin(a):m.r;
    return (d-doorMoverRadiusM*u-hz)/u;
}
inline BodyPush bodyDoorTarget(const MoverAngle& m,float theta,float cap,float rate,float dtS,float shutRad,float upm,float halfZW=0.f) noexcept {
    BodyPush b{};b.target=theta;
    if(!m.ok||m.beyond||m.atHinge||!std::isfinite(m.psi)||!std::isfinite(theta)||!(m.r>0.f))return b;
    const float u=std::max(upm,1e-3f),hz=std::isfinite(halfZW)?std::max(halfZW,0.f):0.f;
    b.gapM=bodyGapM(m,theta,hz,upm);
    if(!(b.gapM<=doorBodyLeadM))return b;
    // the leaf angle that keeps the clearance: its face at rho + halfZ + clear from the capsule's centre
    const float keep=std::asin(std::min(1.f,(doorMoverRadiusM*u+hz+doorBodyClearM*u)/m.r));
    const float lim=std::max(0.f,rate)*(std::isfinite(dtS)?std::clamp(dtS,0.f,.1f):0.f),top=.9998f*std::max(cap,0.f);
    b.inBand=true;
    if(m.psi<theta){
        b.side=-1;const float want=m.psi+keep;
        if(want>theta)b.target=std::max(theta,std::min({want,theta+lim,top}));
    }else{
        b.side=1;const float want=m.psi-keep,floor=std::min(theta,shutRad+pi/180.f);
        if(want<theta)b.target=std::max({want,theta-lim,floor});
        b.atFloor=want<floor&&b.target<=floor+1e-6f;
    }
    return b;
}
// A closed door the hook keeps from the player that he is at (a posable leaf: his capsule's edge within doorStuckBandM of its face, or
// at its hinge; an unposable one: within its reach plus doorStuckReachM) gets the stock Open once he has been at it stuckMs and no push
// opened it; the same timer (doorFlipMs) shuts a leaf his closing push holds at the floor, so the closed leaf then opens away from him.
// One timer per leaf (the two leaves of a double door count apart); a gap over 250 ms restarts it.
inline constexpr float doorStuckBandM=.15f,doorStuckReachM=.10f,doorFlipMs=300.f;
struct StuckTimer {std::uintptr_t door{};std::uint64_t sinceNs{},lastNs{};};
inline bool stuckStep(StuckTimer& t,bool at,std::uint64_t now,float stuckMs) noexcept {
    if(!at){t.sinceNs=0;t.lastNs=0;return false;}
    if(!t.sinceNs||now<t.lastNs||now-t.lastNs>250'000'000ull)t.sinceNs=now?now:1;
    t.lastNs=now;
    return now>=t.sinceNs&&double(now-t.sinceNs)>=double(stuckMs)*1e6;
}
struct StuckTimers {
    std::array<StuckTimer,2> t{};
    // the leaf's timer (a free or the stalest slot taken over)
    StuckTimer& of(std::uintptr_t door) noexcept {
        for(auto& x:t)if(x.door==door)return x;
        StuckTimer* o=&t[0];for(auto& x:t)if(!x.door||x.lastNs<o->lastNs)o=&x;
        *o=StuckTimer{};o->door=door;return *o;}
    void clear(std::uintptr_t door) noexcept {for(auto& x:t)if(x.door==door)x=StuckTimer{};}
};
// The gap bucket (cm) of the capsule's edge to a kept closed leaf: < 0, 0-2, 2-5, 5-10, 10-15, 15-30, 30+ (its real radius against the
// assumed one: a capsule the leaf stops sits at the gap of the difference).
inline unsigned bodyGapBucket(float gapM) noexcept {
    return !(gapM<.30f)?6u:gapM<0.f?0u:gapM<.02f?1u:gapM<.05f?2u:gapM<.10f?3u:gapM<.15f?4u:5u;}
// A refused door push's nearest-part gap bucket: inside, 0-1.5, 1.5-3, 3-6, 6-10, 10+ cm, no part on the leaf.
inline unsigned doorGapBucket(bool ok,float gapM) noexcept {
    return !ok?6u:gapM<0.f?0u:gapM<.015f?1u:gapM<.03f?2u:gapM<.06f?3u:gapM<.10f?4u:5u;}
// Why a door push opened the door the engine's way (plain Open) instead of a held leaf.
enum PlainWhy : std::uint8_t {PlainNoBox=0,PlainOffLeaf,PlainNotUpright,PlainNotPivot,PlainCapPending,PlainSwingOff,PlainCount};
inline constexpr std::array<const char*,PlainCount> plainNames{"noBox","offLeaf","notUpright","notPivot","capPending","swingOff"};

// ============================== props: the type's activation lock (CollideObjects) ==============================
// fragInst::IsActivationDisabled 0x1e97ef4: an override (+0x80 bit 8) decides with bit 7, else the frag type's +0x1ba bit 5. A kept pair
// of the hand with an inactive nav prop lifts a type lock (SetDisableActivation(false), as phInstBehaviorSagExplosion::CollideObjects
// does), so the engine's own contact activation of that pair proceeds. Never for an explicit override (the placement or a script
// set it), a never-activate instance (+0x12 bit 3), infinite mass vs movers (+0x82 bit 7), a prop not inactive, or a door.
inline constexpr bool unlockWanted(std::uint32_t f80,std::uint16_t flags12,std::uint8_t inf82,int state,std::uint8_t doorByte) noexcept {
    return !(f80&0x100u)&&!(flags12&0x8u)&&!(inf82&0x80u)&&state==1&&doorByte!=1;
}
// ============================== props: the root release (tick) ==============================
// fragInst::PrepareForActivation 0x1ea1ef8 returns null while +0x80 bit 1 is clear (the root is fixed until a break); the engine sets it
// in ReportTypePagedIn (minMove == 0 or type bit 1), BreakApart (orr #0x2) and at the end of PrepareForActivation. A small inactive prop
// under a pressing palm gets the bit as BreakApart writes it; the next ForceActivation then activates it. The type is resolved as
// PrepareForActivation resolves it (+0x78 with both resident bits, else null; without +0x78 the type +0x98): with bit 1 set that function
// reads [type+0x1ba] unguarded, so a null type is never released.
inline constexpr std::uintptr_t fragType(std::uintptr_t t78,std::uint32_t t78Bits,std::uintptr_t t78Type,std::uintptr_t t98) noexcept {
    return t78?((t78Bits&0x30000000u)==0x30000000u?t78Type:0):t98;
}
enum ReleaseWhy : std::uint8_t {ReleaseOk=0,ReleaseNotNeeded,ReleaseNotInserted,ReleaseOverrideOff,ReleaseNever,ReleaseTypeNull,ReleaseTypeLocked,
    ReleaseNeverMove,ReleaseTooBig,ReleaseShallow,ReleaseCount};
inline constexpr std::array<const char*,ReleaseCount> releaseNames{"ok","notNeeded","notInserted","overrideOff","never","typeNull","typeLocked","neverMove",
    "tooBig","shallow"};
struct ReleaseIn {std::uint32_t f80{};std::uint16_t flags12{},typeBits{};bool typeOk{};float minMove{};float radius{};float depth{};bool live{};unsigned age{255};};
// radius / depth / maxR / minDepth in the same units. A radius of 0 or less is an unread level sphere: its size is unknown (too big).
inline ReleaseWhy releaseWanted(const ReleaseIn& r,float maxR,float minDepth) noexcept {
    if(!(r.f80&0x1u))return ReleaseNotInserted;
    if(r.f80&0x2u)return ReleaseNotNeeded;
    if((r.f80&0x180u)==0x180u)return ReleaseOverrideOff;
    if(r.flags12&0x8u)return ReleaseNever;
    if(!r.typeOk)return ReleaseTypeNull;
    if((r.f80&0x100u)?(r.f80&0x80u)!=0:(r.typeBits&0x20u)!=0)return ReleaseTypeLocked;
    if(!std::isfinite(r.minMove)||r.minMove<0.f)return ReleaseNeverMove;
    if(!std::isfinite(r.radius)||!(r.radius>0.f)||r.radius>maxR)return ReleaseTooBig;
    if(!r.live||r.age>2||!std::isfinite(r.depth)||r.depth<minDepth)return ReleaseShallow;
    return ReleaseOk;
}

// ============================== props: UNFREEZE (tick) ==============================
// +0x12 bit 3 is the instance's never-activate flag: sagFragInst / fragInst::PrepareForActivation return null while it is set, so no
// contact, wake or bullet activates the prop. fragInst::Insert 0x1e966c8 sets it for a bound that cannot become active (a BVH part:
// phBoundComposite +0xd4), gohProp::OnCreate 0x24c9848 from its description (gohProp +0xf0 bit 0), scripts through SetPropFixed. The one
// stock path that makes such a prop physical is aniAnimProp::ReleaseProp 0x2160f00: type minMove >= 0, the flag cleared, the activation
// lock lifted, +0x80 bit 1, ActivateObject. Only a gohProp fixed by its description qualifies here (its fragInstNav user data a
// sagInstDataProp of kind 1 whose gohProp points back at it), with a composite of box / sphere / capsule / tapered capsule / geometry
// parts (CanBecomeActive true; never a BVH), small and light, under a pressing palm. Reads only until the write.
enum UnfreezeWhy : std::uint8_t {UnfreezeOk=0,UnfreezeNotNever,UnfreezeShallow,UnfreezeTypeNull,UnfreezeMinMoveNeg,UnfreezeNoGoh,UnfreezeDescNotFixed,
    UnfreezeNotComposite,UnfreezeCompD4,UnfreezePartNotActive,UnfreezeTooBig,UnfreezeHeavy,UnfreezeCount};
inline constexpr std::array<const char*,UnfreezeCount> unfreezeNames{"ok","notNever","shallow","typeNull","minMoveNeg","noGoh","descNotFixed","notComposite",
    "compD4","partNotActive","tooBig","heavy"};
// The archetype's mass is the frag type's data value, shared by every instance of the type (a 7.5 cm cup reads 10 kg): it only caps
// obviously wrong data (maxKg); the size decides (the level sphere and the composite's own radius, the part count).
struct UnfreezeIn {std::uint16_t flags12{};bool typeOk{};float minMove{-1.f};bool gohOk{};std::uint8_t gohF0{};bool composite{};std::uint8_t compD4{1};
    unsigned partCount{};bool partsActive{};float radiusM{-1.f},massKg{-1.f},depthM{};bool live{};unsigned age{255};float compRadiusM{-1.f},approach{};};
// The prop's own fields (the order the reasons are counted in).
inline UnfreezeWhy unfreezePropWhy(const UnfreezeIn& u,float maxR,float maxKg,unsigned maxParts=4) noexcept {
    if(!u.typeOk)return UnfreezeTypeNull;
    if(!std::isfinite(u.minMove)||u.minMove<0.f)return UnfreezeMinMoveNeg;
    if(!u.gohOk)return UnfreezeNoGoh;
    if(!(u.gohF0&1u))return UnfreezeDescNotFixed;
    if(!u.composite||u.partCount<1||u.partCount>16)return UnfreezeNotComposite;
    if(u.compD4)return UnfreezeCompD4;
    if(!u.partsActive)return UnfreezePartNotActive;
    if(!std::isfinite(u.radiusM)||!(u.radiusM>0.f)||u.radiusM>maxR||u.partCount>maxParts||(std::isfinite(u.compRadiusM)&&u.compRadiusM>maxR))return UnfreezeTooBig;
    if(!std::isfinite(u.massKg)||!(u.massKg>0.f)||u.massKg>maxKg)return UnfreezeHeavy;
    return UnfreezeOk;
}
// The press: a live hand in contact within the last 4 substeps (the tick runs after them) that is 2 mm or more into the prop or met it
// at unfreezeFlickMps or more (a flick or a slap has already stopped at the creep when the tick runs). 0 = pressing, else 1 not live,
// 2 the contact too old, 3 too shallow and slow.
inline constexpr float unfreezeDepthM=.002f,unfreezeFlickMps=.3f;
inline constexpr unsigned unfreezeShallowWhy(const UnfreezeIn& u) noexcept {
    return !u.live?1u:u.age>4?2u:((std::isfinite(u.depthM)&&u.depthM>=unfreezeDepthM)||(std::isfinite(u.approach)&&u.approach>=unfreezeFlickMps))?0u:3u;}
// The press first (no reads for a hand that does not press), then the prop's own fields.
inline UnfreezeWhy unfreezeWanted(const UnfreezeIn& u,float maxR,float maxKg,float minDepth,unsigned maxParts=4) noexcept {
    (void)minDepth;
    if(!(u.flags12&0x8u))return UnfreezeNotNever;
    if(unfreezeShallowWhy(u))return UnfreezeShallow;
    return unfreezePropWhy(u,maxR,maxKg,maxParts);
}
// The mass set on the prop's own collider once it is active (never on the shared archetype): setKg at a 7.5 cm radius, scaled by the
// volume, in [0.15, 2.5] kg and never above the collider's own mass; setKg 0 keeps the collider's mass.
inline float unfreezeMassKg(float radiusM,float setKg,float colKg) noexcept {
    if(!(colKg>0.f)||!std::isfinite(colKg))return colKg;
    if(!(setKg>0.f)||!std::isfinite(setKg)||!std::isfinite(radiusM)||!(radiusM>0.f))return colKg;
    const float k=radiusM/.075f;return std::min(colKg,std::clamp(setKg*k*k*k,.15f,2.5f));
}
// The one-time velocity of a prop the hand made physical (world units / s): along the palm-to-prop line blended with the push into the
// surface (nOut = out of the prop), the speed the hand's approach gives a body of mProp (an elastic-ish 1.3 x the momentum share), at
// most 3 m/s; spin = (contact - centre) x v / |r|^2, at most 8 rad/s, so a hit above the centre tips it.
struct Kick {V3 vel{},spin{};float mps{};};
inline Kick unfreezeKickOf(V3 centre,V3 palm,V3 contact,V3 nOut,float approachMps,float mHand,float mProp,float upm) noexcept {
    Kick k{};const float u=std::max(upm,1e-3f);
    if(!v::finite(centre)||!v::finite(palm)||!v::finite(nOut)||!std::isfinite(approachMps)||!(approachMps>0.f)||!(mHand>0.f)||!(mProp>0.f))return k;
    const V3 dir=v::normalized(v::add(v::normalized(v::sub(centre,palm)),v::mul(v::normalized(nOut),-1.f)));
    if(v::length(dir)<.5f)return k;
    k.mps=std::min(approachMps*1.3f*mHand/(mHand+mProp),3.f);k.vel=v::mul(dir,k.mps*u);
    if(v::finite(contact)){const V3 r=v::sub(contact,centre);const float r2=v::dot(r,r);
        if(r2>1e-8f*u*u){V3 w=v::mul(v::cross(r,k.vel),1.f/r2);const float wl=v::length(w);if(wl>8.f&&wl>0.f)w=v::mul(w,8.f/wl);if(v::finite(w))k.spin=w;}}
    return k;
}
// UNFREEZE sources: a press (the contact), a grab, the approach (ahead of the contact), a tracked prop that fell asleep met again.
enum UfSource : std::uint8_t {UfPress=0,UfGrab,UfApproach,UfRewake,UfSourceCount};
inline constexpr std::array<const char*,UfSourceCount> ufSourceNames{"press","grab","approach","rewake"};
// The approach: a live palm closing on the prop at minMps or more (its world velocity along the palm-to-centre line) fires once the gap
// between the prop's level sphere and the palm is within max(minGapM, closing * aheadS): the prop is active (awake, 26 collider updates of
// rest before phSleep may put it down: maxMotionlessTicks 10 + maxAsleepTicks 16) when the palm arrives a frame or more later.
inline constexpr float nearPalmM=.05f,nearMinMps=.10f,nearMinGapM=.02f;
inline bool nearFire(float gapM,float closingMps,float aheadS) noexcept {
    return std::isfinite(gapM)&&std::isfinite(closingMps)&&closingMps>=nearMinMps&&gapM<=std::max(nearMinGapM,closingMps*std::max(aheadS,0.f));}
// The contact speed of a palm that met the prop before it was active (the solver already stopped the body at the fixed prop, so the body's
// own speed after the contact is the creep): the controller's own closing speed over its last ticks (ringClosing), and the set velocity at
// the contact edge or at the pair's first substep only as far as the controller's own speed then (ringSpeed) bears it out.
inline float contactMps(float ringClosing,float ringSpeed,float edgeApproach,float pairInto) noexcept {
    const float cap=std::isfinite(ringSpeed)?std::max(ringSpeed,0.f):0.f;
    float v=std::isfinite(ringClosing)?std::max(ringClosing,0.f):0.f;
    if(std::isfinite(edgeApproach))v=std::max(v,std::min(edgeApproach,cap));
    if(std::isfinite(pairInto))v=std::max(v,std::min(pairInto,cap));
    return v;}
// Below this the contact speed is noise: no kick.
inline constexpr float unfreezeKickMinMps=.05f;
// The palm's reach toward a prop along the unit direction d from its centre b (metres): the farthest support of its live parts (the palm,
// the open fingers or the fist) along d, never under nearPalmM. Fingers first, the open hand reaches 13 cm (fingers part: offset 9 cm,
// half length 4 cm); palm first, 1.4 cm.
inline float nearReachM(const HandBoxes& hb,V3 b,V3 d,float upm) noexcept {
    const float u=std::max(upm,1e-3f);float r=nearPalmM;
    if(!hb.mask||!v::finite(b)||!v::finite(d)||!(v::length(d)>.5f))return r;
    for(std::size_t i=0;i<3;++i){if(!(hb.mask>>i&1u))continue;
        const float x=(v::dot(v::sub(hb.c[i],b),d)+boxExtent(hb,i,d))/u;if(std::isfinite(x))r=std::max(r,x);}
    return r;
}
// The approach's arming slots of a hand: a prop already armed keeps its slot, else a free one, else the one farthest outside the palm's
// reach capsule when this prop is nearer (gap = reachGap, world units); -1 = every armed prop is nearer.
template<class E,std::size_t N> inline int nearSlot(const std::array<E,N>& a,std::uintptr_t inst,float gap) noexcept {
    if(!inst||!std::isfinite(gap))return -1;
    for(std::size_t i=0;i<N;++i)if(a[i].inst==inst)return int(i);
    for(std::size_t i=0;i<N;++i)if(!a[i].inst)return int(i);
    int w=-1;float worst=gap;for(std::size_t i=0;i<N;++i)if(a[i].gap>worst){worst=a[i].gap;w=int(i);}
    return w;
}
// The first contact with a prop the approach made physical: the drive into it starts at the push speed (yield; it is known light and
// active); a contact the solver met while it was still fixed (its substep at or before the activation's) also gets the contact's speed
// once (kick): that contact's impulse went into a fixed body.
struct FirstContact {bool yield{},kick{};};
inline FirstContact approachFirstContact(bool approach,bool touchedSince,std::uint64_t contactStep,std::uint64_t clearStep) noexcept {
    FirstContact f{};if(!approach||touchedSince)return f;
    f.yield=true;f.kick=contactStep<=clearStep;return f;}
// An armed prop stays armed while the palm rests within nearKeepM of it (the reach query runs only after the palm moved reachMoveM).
inline constexpr float nearKeepM=.25f;
// A prop the press made physical: its flag goes back once the level holds it inactive (asleep) and no palm met it for restoreNs.
inline constexpr std::uint64_t unfreezeRestoreNs=1'000'000'000ull;
inline constexpr bool unfreezeRestoreDue(int state,std::uint64_t now,std::uint64_t touchNs) noexcept {return state==1&&now>=touchNs&&now-touchNs>=unfreezeRestoreNs;}

// ============================== NPC virtual body (VrMelee tick -> substeps / tick) ==============================
// The creatures near a live palm as VrMelee's bone capsules (world units); a hand's command is clipped out of them in PreUpdate, so
// the hand stops at the body without any engine object. vel = the body's velocity (pelvis delta, world units per second); centre /
// radius = a sphere round all its capsules (finishBody).
struct VCap {V3 a{},b{};float r{};std::uint8_t zone{},kind{};};
inline constexpr std::size_t vbodyCaps=14,vbodyMax=3;
struct VBody {std::uintptr_t actor{};std::uint32_t guid{};int race{-1};V3 vel{},centre{},rowA{};float radius{};std::uint8_t count{};std::array<VCap,vbodyCaps> c{};};
// nearestM = the nearest creature of VrMelee's scan to a live palm (metres, also beyond the 1.6 m body range; -1 none).
struct VBodies {std::uint64_t ns{},serial{};std::uint32_t count{};std::array<VBody,vbodyMax> b{};float nearestM{-1.f};};
inline ::rdrvr::vr::body::Seqlock<VBodies> vbodies;
// The live palms (world), written by the handphys tick for VrMelee's choice of bodies.
// heavy = a held prop of propHitKg or more (lever prophitgeo): VrMelee also keeps the creatures within 1.6 m of its sphere (prop, propR).
struct Palms {std::uint64_t ns{};std::array<V3,2> p{};std::uint8_t live{};V3 prop{};float propR{};std::uint8_t heavy{};};
inline ::rdrvr::vr::body::Seqlock<Palms> palms;
// An open-hand touch for VrMelee (handphys tick -> VrMelee tick, one box per physical hand): a slap or a shove once per gate, a press
// refreshed every tick (a new serial each). part = the capsule index in the body, point = its surface, n = into the body, a = the approach
// speed and speed = the relative speed (m/s), tangent = the speed along the surface, swing = the arm's own part, facing = the palm-out
// normal . n, side = the hand's direction . the creature's row a, depthM = into the capsule (metres), pivot = the body's centre (the
// press yaw), upm = world units per metre; the push levers are copied. Never damage, never the player's guid (VrMelee).
enum TouchKind : std::uint8_t {TouchNone=0,TouchSlap,TouchShove,TouchPress};
inline constexpr std::array<const char*,4> touchNames{"NONE","SLAP","SHOVE","PRESS"};
struct TouchRequest {std::uint64_t serial{},ns{};std::uintptr_t actor{};std::uint32_t guid{};std::uint8_t kind{},capKind{},both{},strong{};int part{-1},hand{-1};
    V3 point{},n{},pivot{};float a{},speed{},tangent{},swing{},facing{},side{},depthM{},upm{1.f};
    float pushGain{},pushMaxM{},pushCapM{},slapYawRad{};std::uint8_t slapReact{},push{};
    // the message wanted (OpenTouchMsg: 0 the fake guid, 1 the creature itself, 2 none), a shove that a held press escalated to, a slap
    // the window decided (after the edge)
    std::uint8_t attacker{},escalated{},windowed{};
    // a shove's tier (PushTier) and the arm's own speed at its edge (m/s); the push levers pushMedium / pushImpulse / pushKick
    std::uint8_t tier{},pushMedium{},pushKick{};float arm{},pushImpulse{};
    // the hand's velocity relative to the body at the approach peak (m/s, world axes) and the kick levers pushKickK / pushKickMaxNs
    V3 vel{};float pushKickK{},pushKickMaxNs{};
    // graded push (lever pushgrade): a pair (the other hand on the creature now or within pairMs: touchPair), the gap to the other hand's
    // edge (ms, -1 none), the creature's velocity at the touch (world units per second, the virtual body's), the knockdown speed lever, the
    // second kick; slapHeld = a slap verdict that waited for the other hand; thrust = a deliberate arm thrust (pushThrust: the knockdown's
    // edge test)
    std::uint8_t pair{},pushGrade{},pushKick2{},slapHeld{},thrust{};float pairGapMs{-1.f},pushDownMps{2.8f};V3 bodyVel{};
    // a press's arm-driven step (lever npcpassive, metres: the distance the arm drove into the body past pressDeadMps this tick; VrMelee
    // moves the creature by at most this); -1 = no arm limit
    float armStepM{-1.f};
    // peak = the same hand's regrade of its shove (lever pushpeak: a = the stroke's peak, exempt from the repeat rule); the push levers
    // pushBusyWait (ms, 0 = off), pushEscGrade, pushFront / pushFrontImp, pushKickHigh; edgeA = the edge's own graded approach (a peak)
    std::uint8_t peak{},pushEscGrade{},pushFront{},pushKickHigh{};float pushBusyWaitMs{},pushFrontImp{40.f},edgeA{-1.f};
    // the touch this request belongs to (the hand's touch start clock: an escalated press finds the push of its own touch by it); the
    // levers pushFrozenMsg, pushSide and pushBehindImp
    std::uint64_t touchNs{};std::uint8_t pushFrozenMsg{},pushSide{};float pushBehindImp{75.f};
    // the knockdown route lever (pushKnock) and the frozen-message hold (ms, 0 = off: lever pushFrozenHold over pairMs)
    std::uint8_t pushKnock{2};float pushFrozenHoldMs{};std::uint8_t pushKnockFlip{1};
    // the knockdown's behaviour pick (lever pushKnockPick, knockPickParity) and its from-behind limit, the torso component lever, the
    // forced knockdown's message (lever pushKnockPickMsg)
    std::uint8_t pushKnockPick{3},pushKnockSpine{};float pushKnockPickFwd{0.f};std::uint8_t pushKnockPickMsg{};
    // the tough actor's window, the pick's draw function, the hands-off window (ms, 0 = measured only)
    std::uint8_t pushKnockTough{1},pushKnockPickFunc{1};float knockOffMs{1500.f};
    // the knockdown's servo and early kick 1 (levers pushKickServo / pushKickEarly)
    std::uint8_t pushKickServo{1},pushKickEarly{};
    // thrown = a light prop's tap in flight (thrownTouchRequest): a slap's reaction only, no node watch, no push ring, no haptic
    std::uint8_t thrown{};};
// The injure message of an open-hand touch. The player is never its attacker (StrikeDamageRemote and the player's own broadcast report
// aggression: aiMindComponent::HandleInjure -> ReportWasHitMelee for flags & 6, ReportWasInjured for damage > 0, both ReportAggression)
// and its damage (KO included) is always 0. Self = the creature itself (aiMind ignores its own guid; a zombie falls back to the fake guid,
// HandleInjure smites a zombie attacker's victim), fake = a guid no consumer resolves, none = no message.
enum OpenTouchMsg : std::uint8_t {OpenMsgFake=0,OpenMsgSelf=1,OpenMsgNone=2};
inline constexpr std::array<const char*,3> openMsgNames{"fake","self","none"};
struct OpenTouchPlan {OpenTouchMsg msg{OpenMsgNone};std::uint32_t attackerGuid{};float damage{};};
// requested = the request's attacker byte; victim / fake = the creature's guid and the fake guid (0 = none possible); zombie = the creature
// is a zombie or that is unknown. The player's guid is no input, so it can never come out.
inline constexpr OpenTouchPlan openTouchPlan(unsigned requested,std::uint32_t victim,std::uint32_t fake,bool zombie) noexcept {
    OpenTouchMsg m=requested==OpenMsgSelf?OpenMsgSelf:requested==OpenMsgFake?OpenMsgFake:OpenMsgNone;
    if(m==OpenMsgSelf&&(zombie||!victim))m=OpenMsgFake;
    if(m==OpenMsgFake&&!fake)m=OpenMsgNone;
    return {m,m==OpenMsgSelf?victim:m==OpenMsgFake?fake:0u,0.f};
}
// NPC reaction nodes (levers slapnode / pushnode: an action-tree path for rdrActionNodeInterface::Find; empty = none): written by the
// handphys tick, read by VrMelee; serial = a change of either text.
struct TouchNodes {std::uint32_t serial{};char slap[64]{},push[64]{};};
inline ::rdrvr::vr::body::Seqlock<TouchNodes> touchNodes;
inline std::array<::rdrvr::vr::body::Seqlock<TouchRequest>,2> touchRequest;
// A thrown light prop's tap (TouchRequest::thrown): a slot of its own, so a tap never overwrites a hand's touch of the same tick.
inline ::rdrvr::vr::body::Seqlock<TouchRequest> thrownTouchRequest;
// TOUCH (handphys install): the engine side of a touch matched its prints and words; VrMelee then calls these (base included).
// moverVptr / moverSimVptr: the two mover classes (mvrMoverComponent and its only subclass mvrMoverSimComponent, the class of every
// actor's mover +0x88); ApplyTranslationOffset is non-virtual and reads base-class fields only, so it takes either.
// nodeOk: sagActor::GetCreatureComponent (actor +0xb0) and sagActorComponent::GetCurrentActionNode (comp +0xb0) matched (the node watch
// reads [[actor+0xb0]+0xb0] and its CRC +0x28, no call); zombie / faction: facFactionMgr::IsZombie(sagActor const*) and the manager
// object (GOT 0x2deb220), the guard of a message whose attacker is the creature itself (HandleInjure smites a zombie attacker's victim).
// findNode / setNode: rdrActionNodeInterface::Find(char const*, ActionNode*) and sagActorComponent::SetActionNode(ActionNode*, bool)
// matched (group NODE); creatureVptr = sagCreatureComponent's address point, the class every play checks on actor +0xb0 first.
// kickOk: the projMsgApplyImpulse path matched (group KICK: entEntityComponent / entEntityInst::HandleApplyProjectileImpulse,
// entPendingImpulse::SetImpulse / Update / ApplyImpulse), the push kick's guard.
struct TouchApi {std::uintptr_t translate{},rotate{},moverVptr{},animVptr{},moverSimVptr{},base{};bool nodeOk{};std::uintptr_t zombie{},faction{};
    std::uintptr_t findNode{},setNode{},creatureVptr{};bool kickOk{};
    // TREE prints: the injury subtree's conditions may be read (node +0x40 / +0x48, condition vtable +0x68)
    bool treeOk{};
    // PICK prints and words: ConditionRandom's shared roll (rage::ConditionRandom::sm_RandomUsed byte, sm_RandomNum int) and its random
    // function pointer (sm_RandFunc: 0 = the engine's own LCG); absolute addresses, 0 = refused
    bool pickOk{};std::uintptr_t randUsed{},randNum{},randFunc{};
    // TOUGH prints (ConditionInjuryToughActor / ...Hits::Match, SetToughActor): the tough bit (health +0x150 bit 0x8000) may be cleared for a
    // knockdown's message; PICKFN prints (ConditionRandom::Match, GetRandInt) and the g_ReplayRand word: the pick's draw function may be set
    bool toughOk{},pickFnOk{};};
// The node watch's name words (handphys DIAG prints): ActionNodeImplementation::GetName (the CName at node +0x30: char* +0x0, hash +0x8)
// and ActionNodeReference::GetName (the node at +0x48).
inline constexpr std::uintptr_t nodeImplGetName=0x28f4b44,nodeRefGetName=0x28f282c;
// The global action-node root rage::ActionNode::Find (0x28f0f50..0x28f0f5c) starts a "/a/b" path from: the GOT slot and the variable it
// holds (R_AARCH64_RELATIVE 0x2e0d328 -> 0x4e811e00); a node path that ends at this root resolves through Find.
inline constexpr std::uintptr_t actionRootGot=0x2e0d328,actionRootVar=0x4e811e00;
// The clock of the last mvrMsgBumpedInto the hands sent (handphys tick): a walk-in bump of the player's mover is one with none near it.
inline std::atomic<std::uint64_t> bumpSentNs{};
// A walk-in bump VrMelee asks for (a graded stumble waiting on a creature whose mover is frozen or suspended): the handphys tick sends
// one mvrMsgBumpedInto from the player through its own checked path (the slot table, alive, the BUMP prints) for each new serial.
struct BumpAsk {std::uint64_t serial{},ns{};std::uintptr_t actor{};std::uint32_t guid{};};
inline ::rdrvr::vr::body::Seqlock<BumpAsk> bumpAsk;
inline TouchApi touchApi{};inline std::atomic<bool> touchReady{};
// The player's sagPlayerInputFoot (x0 of SetupActionTreePacket, published by VrMelee's packet hook): AddUseContexts passes its
// sagPlayerInputDoor at +0x1a8 to AddUseContext (0x26b43dc), the door hook's on-foot test.
inline std::atomic<std::uintptr_t> playerFoot{};inline constexpr std::size_t footInputDoor=0x1a8;
// The first two substeps of an open hand on a capsule decide the touch. capKind (VrMeleeMath CapsuleSpec): 0 head 1 neck 2 chest
// 3 belly 4 arm 5 leg 6 / 7 whole-body fallbacks. SLAP: the head / neck (the chest only fast), a fast arm swing, the palm or the back of
// the hand towards it, from the side or along the surface. SHOVE: the body (or the head face-on), the palm towards it, along the normal.
// armGate (lever npcpassive): a shove also needs the arm's own speed into the body (armIn, from HandTarget::velArm) at armInMin or more.
// facingBody (lever pushfacingbody): the palm normal . the horizontal direction from the hit point to the body's centre (-2 = none); a
// palm on an arm capsule (capKind 4) passes the shove's facing gate on the larger of it and the capsule's facing (a palm driven at the
// torso often meets the arm first, whose normal points sideways).
struct TouchIn {int capKind{-1};float a{},speed{},swing{},facing{},side{},tangent{};bool horse{};float arm{};bool armGate{};float armIn{},armInMin{};
    float facingBody{-2.f};};
// The first gate a touch failed, per class (diagnostics; 0 = passed).
enum SlapFail : std::uint8_t {SlapPass=0,SlapOff,SlapZone,SlapSpeed,SlapFacing,SlapSwing,SlapPush,SlapFailCount};
inline constexpr std::array<const char*,SlapFailCount> slapFailNames{"pass","off","zone","speed","facing","swing","push"};
enum ShoveFail : std::uint8_t {ShovePass=0,ShoveOff,ShoveZone,ShoveFacing,ShoveRatio,ShoveSpeed,ShoveSwing,ShoveArm,ShoveFailCount};
inline constexpr std::array<const char*,ShoveFailCount> shoveFailNames{"pass","off","zone","facing","ratio","speed","swing","arm"};
struct TouchVerdict {TouchKind kind{TouchNone};std::uint8_t slapFail{},shoveFail{};bool slapZone{},facingRescued{};};
// The slap zone: the head and the neck, the chest only at twice slapMps from the side or along it (a palm thrust into it is a shove). A slap needs the speed (the palm or the back of the hand,
// |facing| >= 0.25, the arm's own swing at half slapMps or more: a creature walking into a still hand is no slap); at the contact edge it
// is decided at once from slapFastMps (or slapMps when higher), a slower touch opens a window (slapWindowStep) that decides on the peak
// speed when the hand leaves without a press. A fast open hand at the head or the neck is a slap edge-on too (no facing gate). The swing
// gate takes the larger of the swing along the normal and the arm's own speed (arm = the controller relative to the player: the swing
// sample alone reads 0 at some fast edges). A shove: the body (or the head face-on), the palm towards it along the normal from an arm
// swing.
inline constexpr float slapFastMps=2.2f;
// A palm driven into the body: the approach along the normal is most of the relative speed (a / speed >= pushLikeRatio), the palm faces
// the body (facing >= pushLikeFacing) and the approach reaches shoveMps. Every slap edge of the device's two-hand chest pushes had a / speed
// 0.87..1.00 and facing 0.52..0.84 on the neck or the chest (the shoves 0.70..0.94, the slow touches that became nothing 0.30..0.33).
inline constexpr float pushLikeRatio=.75f,pushLikeFacing=.45f;
inline bool pushLike(const TouchIn& t,float shoveMps) noexcept {
    return std::isfinite(t.a)&&std::isfinite(t.speed)&&std::isfinite(t.facing)&&t.speed>0.f&&t.a/t.speed>=pushLikeRatio&&t.facing>=pushLikeFacing&&t.a>=shoveMps;}
// graded (lever pushgrade): the slap zone is the head and the neck only; a push-like touch on the neck is a push (SlapPush), the head keeps
// its slap; the shove's facing gate is pushLikeFacing. Not graded: the tiered rule (the chest a slap zone at twice slapMps from the side or
// along it, the shove's facing gate 0.6).
inline TouchVerdict touchVerdict(const TouchIn& t,float slapMps,float shoveMps,bool slapOn,bool shoveOn,bool atEdge,bool graded=true) noexcept {
    TouchVerdict v{};
    if(t.horse||!std::isfinite(t.a)||!std::isfinite(t.speed)||!std::isfinite(t.facing)){v.slapFail=SlapOff;v.shoveFail=ShoveOff;return v;}
    const float side=std::isfinite(t.side)?std::fabs(t.side):0.f,sw=std::max(std::isfinite(t.swing)?t.swing:0.f,std::isfinite(t.arm)?t.arm:0.f);
    const float tan=std::isfinite(t.tangent)?t.tangent:0.f;
    v.slapZone=t.capKind==0||t.capKind==1||(!graded&&t.capKind==2&&t.speed>=2.f*slapMps&&(side>=.45f||tan>=.6f*t.a));
    const bool pushy=graded&&t.capKind!=0&&pushLike(t,shoveMps);
    const float fast=atEdge?std::max(slapFastMps,slapMps):slapMps;
    const bool headFast=(t.capKind==0||t.capKind==1)&&t.speed>=std::max(slapFastMps,slapMps);
    v.slapFail=!slapOn?SlapOff:!v.slapZone?SlapZone:pushy?SlapPush:t.speed<fast?SlapSpeed:(!headFast&&std::fabs(t.facing)<.25f)?SlapFacing:sw<.5f*slapMps?SlapSwing:SlapPass;
    if(v.slapFail==SlapPass){v.kind=TouchSlap;return v;}
    const bool shoveZone=(t.capKind>=1&&t.capKind<=4)||t.capKind==6||t.capKind==7||(t.capKind==0&&side<.45f);
    const float facingMin=graded?pushLikeFacing:.6f;
    const bool armLow=t.armGate&&!(std::isfinite(t.armIn)&&t.armIn>=t.armInMin);
    const float fShove=t.capKind==4&&std::isfinite(t.facingBody)?std::max(t.facing,t.facingBody):t.facing;
    v.facingRescued=t.facing<facingMin&&fShove>=facingMin;
    v.shoveFail=!shoveOn?ShoveOff:!shoveZone?ShoveZone:fShove<facingMin?ShoveFacing:!(t.speed>0.f)||t.a/t.speed<.7f?ShoveRatio:t.a<shoveMps?ShoveSpeed:sw<.6f?ShoveSwing:
                armLow?ShoveArm:ShovePass;
    if(v.shoveFail==ShovePass)v.kind=TouchShove;
    return v;
}
inline TouchKind classifyTouch(const TouchIn& t,float slapMps,float shoveMps,bool slapOn,bool shoveOn,bool graded=true) noexcept {return touchVerdict(t,slapMps,shoveMps,slapOn,shoveOn,true,graded).kind;}
// Two hands on one creature: the other hand touches it now or had its edge on it within pairNs. A pair is one push (never a slap, never
// held back by the slap gate).
inline constexpr bool touchPair(bool otherOn,bool otherEdgeSame,std::uint64_t otherEdgeNs,std::uint64_t now,std::uint64_t pairNs) noexcept {
    return otherOn||(otherEdgeSame&&now>=otherEdgeNs&&now-otherEdgeNs<=pairNs);}
// The gap between the two hands' edges on one creature (ms bins: <25, 25-50, 50-100, 100-150, 150-300).
inline constexpr unsigned pairGapBin(std::uint64_t gapNs) noexcept {
    return gapNs<25'000'000ull?0u:gapNs<50'000'000ull?1u:gapNs<100'000'000ull?2u:gapNs<150'000'000ull?3u:4u;}
// The window of a touch the edge did not decide: its peak relative speed while the hand is on the body; a SLAP when the hand leaves
// (or the window ends) in the slap zone at slapMps or more without having become a press. Out: 0 nothing yet, 1 slap, 2 closed.
struct SlapWindow {bool open{};std::uint64_t startNs{};float peak{};bool zone{},pressed{},facing{};};
inline int slapWindowStep(SlapWindow& w,bool touching,bool pressing,float speed,float facing,std::uint64_t now,float slapMps,float windowMs) noexcept {
    if(!w.open)return 0;
    if(std::isfinite(speed))w.peak=std::max(w.peak,speed);
    if(std::isfinite(facing)&&std::fabs(facing)>=.25f)w.facing=true;
    if(pressing)w.pressed=true;
    const bool ended=!touching||now<w.startNs||double(now-w.startNs)>=double(windowMs)*1e6;
    if(!ended)return 0;
    const bool slap=!w.pressed&&w.zone&&w.facing&&w.peak>=slapMps;w=SlapWindow{};
    return slap?1:2;
}
// A press: the palm towards the body, 1 cm or more in, held 100 ms (150 ms after a slap of the same touch), with both hands on it, or
// 3 cm or more in at once (not after a slap).
inline bool pressWanted(float facing,float depthM,std::uint64_t heldNs,bool both,bool afterSlap) noexcept {
    return std::isfinite(facing)&&facing>=.5f&&std::isfinite(depthM)&&depthM>=.01f&&(both||(depthM>=.03f&&!afterSlap)||heldNs>=(afterSlap?150'000'000ull:100'000'000ull));
}
// A press in progress holds while the palm stays 1 cm in and faces the body at pressKeepFacing or more (it starts at .5: pressWanted).
inline constexpr float pressKeepFacing=.25f;
inline bool pressHeld(float facing,float depthM) noexcept {
    return std::isfinite(facing)&&facing>=pressKeepFacing&&std::isfinite(depthM)&&depthM>=.01f;
}
// A facing value's bin of 8 over [-1, 1].
inline unsigned facingBin(float f) noexcept {return std::isfinite(f)?unsigned(std::clamp((f+1.f)*4.f,0.f,7.f)):0u;}
// A press escalates to a push once the push it asked for within a pressWindowNs window reaches pressPushM and the hand itself drove into
// the body (the controller's world speed toward it summed over the window to half of pressPushM, or pressArmMps at that tick): a creature
// walking into a still palm is pressed, never shoved. One push per touch: the caller clears the record on a new touch. still = the asked
// push was there but the hand was not moving into the body.
inline constexpr std::uint64_t pressWindowNs=400'000'000ull;inline constexpr float pressArmMps=.3f;
// armPeak = the hand's peak speed into the body within the window (m/s): the escalated push's graded approach (escalatedA).
struct PressAsk {std::uint64_t startNs{},lastNs{};float askedM{},armM{},armPeak{};bool pushed{},still{};};
inline bool pressEscalate(PressAsk& p,float stepM,float armMps,std::uint64_t now,float pushM) noexcept {
    if(!p.startNs||now<p.startNs||now-p.startNs>pressWindowNs){p.startNs=now?now:1;p.askedM=0.f;p.armM=0.f;p.armPeak=0.f;p.lastNs=0;}
    if(std::isfinite(stepM)&&stepM>0.f)p.askedM+=stepM;
    const float arm=std::isfinite(armMps)?armMps:0.f;p.armPeak=std::max(p.armPeak,arm);
    const float dt=p.lastNs&&now>p.lastNs?float(std::min(double(now-p.lastNs)*1e-9,.1)):0.f;p.lastNs=now;
    if(arm>0.f)p.armM+=arm*dt;
    p.still=false;
    if(p.pushed||p.askedM<pushM)return false;
    if(p.armM<.5f*pushM&&arm<pressArmMps){p.still=true;return false;}
    p.pushed=true;return true;
}
// The escalated push's graded approach (m/s): the press's own peak drive into the body, never the touch's edge (a palm that rested on the
// body first and then pushed has a passive edge: a 1.53 approach with a 0.04 arm).
inline float escalatedA(const PressAsk& p) noexcept {return std::isfinite(p.armPeak)?std::max(p.armPeak,0.f):0.f;}
// PASSIVE contact (lever npcpassive): the arm's drive into a body over one substep of dtS (metres): armIn dtS once armIn reaches deadMps
// (a gate, not an offset: a deliberate slow press moves the creature at the arm's whole speed), else 0. A palm held still against a body
// (the player walking or turning into it, the creature walking into it) adds nothing.
inline float armStepM(float armInMps,float deadMps,float dtS) noexcept {
    if(!std::isfinite(armInMps)||!std::isfinite(dtS)||!(dtS>0.f))return 0.f;
    const float dead=std::max(std::isfinite(deadMps)?deadMps:0.f,0.f);
    return armInMps>0.f&&armInMps>=dead?armInMps*std::min(dtS,.1f):0.f;}
// A press stays arm-driven for pressArmHoldNs after its last tick with an arm step (a press near the gate whose step flickers keeps its
// bump gate running: PressGate's 2 s cap and rest apply to it).
inline constexpr std::uint64_t pressArmHoldNs=300'000'000ull;
inline bool pressArmHeld(std::uint64_t lastArmNs,std::uint64_t now) noexcept {return lastArmNs&&now>=lastArmNs&&now-lastArmNs<pressArmHoldNs;}
// A passive push's graded approach (m/s): an escalated press or a light push takes the arm's own speed into the body (min(a, armIn)); a
// deliberate shove or slap (it passed the arm gate) the arm's plus the player's forward locomotion into the body (a shove made while
// stepping in), never above the measured approach a. The player's turn and the creature's own walk grade nothing.
inline float passiveGradeA(float approach,float armIn,float locoIn,bool deliberate) noexcept {
    const float a=std::isfinite(approach)?approach:0.f,arm=std::isfinite(armIn)?armIn:0.f;
    if(!deliberate)return std::min(a,arm);
    const float lo=std::isfinite(locoIn)&&locoIn>0.f?locoIn:0.f;return std::min(a,arm+lo);}
// The palm's arm velocity: one sample whose step in the tracking space is over maxMps (a controller's tracking lost and found again, a
// recentre within the same space) is a jump, not motion: the estimate restarts.
inline constexpr float armJumpMps=12.f;
inline bool armJump(V3 prevLocal,V3 local,float dtS,float upm,float maxMps=armJumpMps) noexcept {
    if(!(dtS>1e-4f)||!v::finite(prevLocal)||!v::finite(local))return false;
    return v::length(v::sub(local,prevLocal))/std::max(upm,1e-3f)>maxMps*dtS;}
// The player-capsule guard's normal: the contact normal across up, unit; zero when its horizontal part is under half of it (a prop resting
// on top of the capsule: its normal has no stable horizontal direction).
inline V3 guardNormal(V3 n,V3 up) noexcept {
    const V3 u=v::normalized(up);const V3 hz=v::sub(n,v::mul(u,v::dot(n,u)));const float l=v::length(n),lh=v::length(hz);
    if(!std::isfinite(l)||!std::isfinite(lh)||!(l>1e-6f)||lh<.5f*l)return {0,0,0};
    return v::mul(hz,1.f/lh);}
// A press's step with the arm limit: the depth law's step, at most the arm's drive (armStep < 0: no limit).
inline float pressArmStepM(float depthStepM,float armStep) noexcept {
    const float d=std::isfinite(depthStepM)?std::max(depthStepM,0.f):0.f;return armStep<0.f||!std::isfinite(armStep)?d:std::min(d,armStep);}
// An arm-speed histogram bin (m/s): 0-.1, .1-.2, .2-.3, .3-.4, .4-.6, .6-.9, .9-1.2, 1.2+.
inline unsigned armBin(float x) noexcept {return !(x>=.1f)?0u:x<.2f?1u:x<.3f?2u:x<.4f?3u:x<.6f?4u:x<.9f?5u:x<1.2f?6u:7u;}
// The creature's reaction field (aniMelee +0x550, aniMeleeData::MeleeReactionTypes): UP_FOR 1, UP_RGT 2, UP_LFT 3, KNCKBK 4 / RGT 5 /
// LFT 6, SHOVED 7.
inline constexpr int slapReaction(float side) noexcept {return side>.5f?2:side<-.5f?3:1;}
inline constexpr int shoveReaction(float side,bool strong) noexcept {return strong?(side>.5f?5:side<-.5f?6:4):7;}
// A push's tier. LIGHT (a press, an escalated press): the mover offset only; MEDIUM (a shove edge under the firm gates): the offset, a
// message only with lever pushmedium; FIRM: the injure message (SHOVED, KNCKBK when strong). Firm = the approach a at firmMps or more, the
// edge depthM at firmM or more and the arm's own speed at armMps or more: a deliberate thrust, not a hand that brushes the body (the
// device's light shove edges: a 1.22 / 1.38 m/s, 0.2 / 0.5 cm deep; its firm ones: a 1.77..2.56 m/s, 1.3..5.4 cm, arm 1.77..2.82 m/s).
enum PushTier : std::uint8_t {TierLight=0,TierMedium,TierFirm,TierCount};
inline constexpr std::array<const char*,TierCount> tierNames{"light","medium","firm"};
inline PushTier pushTier(bool escalated,float a,float depthM,float arm,float firmMps,float firmM,float armMps) noexcept {
    if(escalated)return TierLight;
    const bool firm=std::isfinite(a)&&a>=firmMps&&std::isfinite(depthM)&&depthM>=firmM&&std::isfinite(arm)&&arm>=armMps;
    return firm?TierFirm:TierMedium;}
// A firm shove is strong (KNCKBK) for a two-hand pair or an approach of 3 m/s or more.
inline constexpr float pushStrongMps=3.f;
inline bool pushStrong(PushTier t,bool pair,float a) noexcept {return t==TierFirm&&(pair||(std::isfinite(a)&&a>=pushStrongMps));}
// The injure message's face (msg +0x88, ConditionInjuryFace): the engine's punch sets dot(victim row c, attacker row c) >= 0 (MeleeMachine
// 0x1e42014..0x1e42068), 1 = hit from behind; a push takes the push's own direction for the attacker's row (the player's body need not
// face along the push).
inline int pushFace(V3 victimRowC,V3 pushDir) noexcept {const float d=v::dot(victimRowC,pushDir);return std::isfinite(d)&&d>=0.f?1:0;}
// The message's impulse (msg +0xa0 -> aniNMStateData +0x30 -> NM shotNewBullet bulletVel): along the push (horizontal), magnitude lever
// when set, else the engine punch's value k, else (k at pushKMin or less: the player's melee holds no action outside an engine melee)
// fallback. NMShotNewBulletTask::IsHitInfoValid 0x22ec008 only asks bulletVel to be finite, so a zero vector reaches NM as it is and
// gives the fall no direction. The source: 0 lever, 1 k, 2 fallback.
inline constexpr float pushKMin=.01f;
struct PushImpulse {V3 v{};std::uint8_t src{};};
inline PushImpulse pushImpulse(V3 dir,float lever,float k,float fallback) noexcept {
    PushImpulse r{};float m=0.f;
    if(lever>0.f){m=lever;r.src=0;}
    else if(std::isfinite(k)&&k>pushKMin){m=k;r.src=1;}
    else{m=std::isfinite(fallback)&&fallback>0.f?fallback:0.f;r.src=2;}
    r.v=v::mul(v::normalized(dir),m);return r;}
inline V3 pushImpulseVec(V3 dir,float lever,float k,float fallback=0.f) noexcept {return pushImpulse(dir,lever,k,fallback).v;}
// The push kick (projMsgApplyImpulse once the creature of a firm shove is ragdoll; NMShotTask turns the message's bulletVel into no
// force, so the kick is what moves the body along the push): the direction is the palm's horizontal velocity relative to the body when it
// is at least pushKickPalmMps and within acos(pushKickAlign) of into (a palm sweeping sideways along the body still pushes into it),
// else into (the horizontal contact normal into the body). Size k x max(a, 1 m/s), capped at maxNs; 0 when either is unusable.
// src: 0 the palm, 1 into, 2 none.
inline constexpr float pushKickPalmMps=.5f,pushKickAlign=.3f;
struct PushKick {V3 dir{};float mag{};std::uint8_t src{2};float palmMps{};V3 palmDir{};};
inline PushKick pushKickPlan(V3 into,V3 palmVel,float a,float k,float maxNs) noexcept {
    PushKick r{};const V3 in{into[0],0.f,into[2]};const float il=v::length(in);
    if(!(il>.5f)||!std::isfinite(il))return r;
    const V3 i=v::mul(in,1.f/il);
    const V3 ph{palmVel[0],0.f,palmVel[2]};const float pl=v::length(ph);
    if(std::isfinite(pl)){r.palmMps=pl;if(pl>1e-4f)r.palmDir=v::mul(ph,1.f/pl);}
    if(std::isfinite(pl)&&pl>=pushKickPalmMps&&v::dot(r.palmDir,i)>=pushKickAlign){r.dir=r.palmDir;r.src=0;}
    else{r.dir=i;r.src=1;}
    const float m=(std::isfinite(k)?k:0.f)*std::max(std::isfinite(a)?a:0.f,1.f);
    r.mag=std::isfinite(maxNs)&&maxNs>0.f?std::clamp(m,0.f,maxNs):0.f;
    if(!(r.mag>0.f)){r.mag=0.f;r.src=2;}
    return r;}
// The push kick's arming gate at the push (in this order): off (lever, not a firm shove, a slap, no self message), not a human, the KICK
// prints or the broadcast unverified, the mover's ragdoll word set or unread at the push (ragAtPush 1 / -1: the kick fires only on the
// change 0 -> 1 after a shove, never on a creature already down), another watch of the same creature armed or sent a kick within
// pushKickPairNs (a two-hand shove kicks once), no torso bone (spine / pelvis) in the creature's layout or no frag instance, no size, the
// creature lost hp after an earlier kick; else armed.
enum KickGate : std::uint8_t {KickGateArmed=0,KickGateOff,KickGateHuman,KickGatePrint,KickGateAlready,KickGatePair,KickGateNoComp,KickGateNoMag,KickGateHpLock};
inline constexpr std::uint64_t pushKickPairNs=300'000'000ull,pushKickWindowNs=200'000'000ull;
inline constexpr KickGate pushKickGate(bool want,bool human,bool printsOk,int ragAtPush,bool pairKick,bool haveTorso,float mag,bool locked) noexcept {
    return !want?KickGateOff:!human?KickGateHuman:!printsOk?KickGatePrint:ragAtPush!=0?KickGateAlready:pairKick?KickGatePair:!haveTorso?KickGateNoComp:
        !(mag>0.f)?KickGateNoMag:locked?KickGateHpLock:KickGateArmed;}
// An armed kick fires on the first tick within pushKickWindowNs of the push that reads the mover's ragdoll word set.
inline constexpr bool pushKickFires(bool armed,bool kicked,std::uint64_t ageNs,bool ragRead,std::uint32_t rag) noexcept {
    return armed&&!kicked&&ageNs<=pushKickWindowNs&&ragRead&&rag!=0;}
// The kick window's age: measured from the message that armed it (baseNs = that message's age on the watch; a pair push upgraded to a
// knockdown by the second hand reuses the first hand's watch, whose t0 is up to pushRecNs older).
inline constexpr std::uint64_t kickAgeNs(std::uint64_t watchAgeNs,std::uint64_t baseNs) noexcept {return watchAgeNs>=baseNs?watchAgeNs-baseNs:0u;}
// The instance's pending impulses (entEntityInst +0x1a0, entPendingImpulseMgr::FindFreeSlot 0x2913a64): 8 slots of 0x30 bytes, a slot in
// use while its word +0x28 is nonzero (entPendingImpulse::SetImpulse stores the frame counter there). A kick is queued when a slot free
// before the call is in use after it.
inline constexpr unsigned kickSlots=8;inline constexpr std::size_t kickMgr=0x1a0,kickSlotStride=0x30,kickSlotWord=0x28;
inline constexpr unsigned kickFreeSlots(const std::uint32_t* w) noexcept {unsigned n=0;for(unsigned i=0;i<kickSlots;++i)n+=w[i]==0u?1u:0u;return n;}
inline constexpr bool kickQueued(const std::uint32_t* before,const std::uint32_t* after) noexcept {
    for(unsigned i=0;i<kickSlots;++i)if(before[i]==0u&&after[i]!=0u)return true;return false;}
// GRADED PUSH (lever pushgrade). The effective push of one or two hands: aEff = a for one hand, max(a0, a1) + 0.5 min(a0, a1) for a pair;
// its direction the approach-weighted mean of the hands' push directions (horizontal; the stronger hand's when they cancel).
struct PushSum {float aEff{};V3 dir{};};
inline PushSum pushMerge(float a0,V3 d0,float a1,V3 d1) noexcept {
    PushSum r{};const float x0=std::isfinite(a0)?std::max(a0,0.f):0.f,x1=std::isfinite(a1)?std::max(a1,0.f):0.f;
    r.aEff=std::max(x0,x1)+.5f*std::min(x0,x1);
    const V3 h0=v::normalized(V3{d0[0],0.f,d0[2]}),h1=v::normalized(V3{d1[0],0.f,d1[2]});
    const V3 sum=v::add(v::mul(h0,x0),v::mul(h1,x1));
    r.dir=v::finite(sum)&&v::length(sum)>1e-3f?v::normalized(sum):(x0>=x1?h0:h1);
    return r;}
inline PushSum pushSingle(float a,V3 d) noexcept {PushSum r{};r.aEff=std::isfinite(a)?std::max(a,0.f):0.f;r.dir=v::normalized(V3{d[0],0.f,d[2]});return r;}
// The response: a stumble (the mover offset along the push, no message, no sound) under downMps or without a firm edge (a deliberate
// thrust: pushTier), a knockdown (SHOVED, no offset, the kick) at downMps or more.
enum PushDecision : std::uint8_t {PushStumble=0,PushKnockdown,PushDecisionCount};
inline constexpr std::array<const char*,PushDecisionCount> pushDecisionNames{"stumble","knockdown"};
inline PushDecision pushDecide(float aEff,bool firm,float downMps) noexcept {return firm&&std::isfinite(aEff)&&aEff>=downMps?PushKnockdown:PushStumble;}
// The knockdown's edge test (graded): the approach a at firmMps or more and the arm's own speed at armMps or more (a deliberate thrust,
// not the player walking into the creature: arm 0.05 at a 2.71). The edge depth of one substep is no part of it (the device's 2.47 and
// 2.13 m/s thrusts read 0.5 / 0.9 cm deep, the slower ones 2..7 cm).
// A held slap (slapholdms) is dropped when the touches are off, the hand's last touch is another creature, or the same hand shoved this
// creature since (its push supersedes); a follow-through that touches the head again (a new edge with no verdict) or rests on it (a
// press) keeps it.
inline constexpr bool slapHoldDrops(bool touchesOn,std::uint32_t touchGuid,std::uint32_t slapGuid,bool touchShove) noexcept {
    return !touchesOn||(touchGuid&&touchGuid!=slapGuid)||(touchGuid==slapGuid&&touchShove);}
inline bool pushThrust(float a,float arm,float firmMps,float armMps) noexcept {
    return std::isfinite(a)&&a>=firmMps&&std::isfinite(arm)&&arm>=armMps;}
// A stumble's offset (metres): 8 cm + 12 cm per m/s over 1, 5..30 cm (the mover offset is delivered as asked: 16.0 cm asked, 17.7 moved).
inline float stumbleDistanceM(float aEff) noexcept {return std::clamp(.08f+.12f*((std::isfinite(aEff)?aEff:0.f)-1.f),.05f,.30f);}
// A knockdown's target speed of the body along the push (m/s): 0.5 at downMps, +0.6 per m/s over it, at most 1.5.
inline float knockdownMps(float aEff,float downMps) noexcept {return std::clamp(.5f+.6f*((std::isfinite(aEff)?aEff:0.f)-downMps),.5f,1.5f);}
// The creature's mass for the kick: the frag archetype's (+0x38) when it is 40..150 kg, else 75 kg.
inline float kickMassKg(float archKg) noexcept {return std::isfinite(archKg)&&archKg>=40.f&&archKg<=150.f?archKg:75.f;}
// The kick that sets the body's horizontal velocity to vDes along dir from v0 (NM staggerFall pitches the hips by the forward part of the
// body's centre-of-mass velocity, NmRsCBUStaggerFall::tick 0x1f92308..0x1f92378: the creature's own walk decides the fall unless the
// kicks replace it):
// J = m (max(0, vDes - v0 . dir) dir - v0 sideways), horizontal, |J| <= capNs. The part along the push is never negative: a body already
// moving along the push faster than vDes is not braked toward the pusher.
inline V3 kickImpulse(float massKg,float vDes,V3 dir,V3 v0,float capNs) noexcept {
    const V3 d=v::normalized(V3{dir[0],0.f,dir[2]});const V3 w{std::isfinite(v0[0])?v0[0]:0.f,0.f,std::isfinite(v0[2])?v0[2]:0.f};
    const float along=v::dot(w,d);const V3 side=v::sub(w,v::mul(d,along));
    const float want=std::isfinite(vDes)?std::max(0.f,vDes-along):0.f;
    V3 j=v::mul(v::sub(v::mul(d,want),side),std::isfinite(massKg)?massKg:0.f);
    const float l=v::length(j);const float cap=std::isfinite(capNs)?std::max(capNs,0.f):0.f;
    if(!std::isfinite(l))return V3{};
    if(l>cap)j=l>1e-6f?v::mul(j,cap/l):V3{};
    return j;}
// The second kick (100 ms after the first): when the pelvis's horizontal velocity vm along the push is under 0.6 vDes or its sideways part
// over 0.4 m/s.
inline constexpr std::uint64_t kick2DelayNs=100'000'000ull;inline constexpr float kick2CapNs=100.f;
inline bool kick2Wanted(V3 vm,V3 dir,float vDes) noexcept {
    const V3 d=v::normalized(V3{dir[0],0.f,dir[2]});const V3 w{vm[0],0.f,vm[2]};
    if(!v::finite(w)||!(v::length(d)>.5f))return false;
    const float along=v::dot(w,d);const float side=v::length(v::sub(w,v::mul(d,along)));
    return along<.6f*vDes||side>.4f;}
// The slot a kick took (the first slot free before the call and in use after it; -1 none).
inline constexpr int kickQueuedSlot(const std::uint32_t* before,const std::uint32_t* after) noexcept {
    for(unsigned i=0;i<kickSlots;++i)if(before[i]==0u&&after[i]!=0u)return int(i);return -1;}
// A queued kick's slot word consumed: entPendingImpulse::Update 0x29133b0 stores count - 1 first, then (object active) sets a count >= 1
// to -1 and applies GetScale(0) on the call that finds -1 (the word then reads -2), and frees the slot (0) once ~count reaches
// sm_iNumFrames. -2 or below, or 0, = applied (or counted out while the object stayed inactive).
inline constexpr bool kickSlotConsumed(std::int32_t w) noexcept {return w<=-2||w==0;}
// The slot word's outcome for one kick. A kick queued while the object is inactive (on the message tick, before the ragdoll) counts down
// one per update and frees its slot at 0 after sm_iCountdownNumFrames (8) inactive updates with nothing applied; one that meets an active
// update first reads -1, then is applied on the next (-2 and below, freed at 0). So: -2 or below = applied; 0 = applied when a negative
// word was read since it was queued, or when it was queued on an active object (ragdoll set), else counted out (freed, not applied).
enum KickSlotOutcome : std::uint8_t {SlotPending=0,SlotApplied,SlotFreed};
inline constexpr KickSlotOutcome kickSlotOutcome(std::int32_t w,bool sawNeg,bool queuedActive) noexcept {
    return w<=-2?SlotApplied:w!=0?SlotPending:(sawNeg||queuedActive)?SlotApplied:SlotFreed;}
// KNOCK TRACE rows: before the knockdown's message only the last tracePreRows ticks are kept (rolling), from the message on one row a
// tick until the trace is full. index = the row to write (-1 none), shift = move rows 1.. down by one first, n = the count after.
inline constexpr unsigned tracePreRows=2;
struct TraceRowPlan {int index{-1};bool shift{};unsigned n{};};
inline constexpr TraceRowPlan traceRowPlan(unsigned n,unsigned cap,bool messageSent) noexcept {
    if(!messageSent){if(n<tracePreRows&&n<cap)return {int(n),false,n+1u};return n==0u?TraceRowPlan{-1,false,n}:TraceRowPlan{int(n-1u),true,n};}
    if(n<cap)return {int(n),false,n+1u};return {-1,false,n};}
// A kick stamped on a trace row only when the row is this tick's own (its ms equals the tick's age in ms, clamped as the row is).
inline constexpr bool traceRowIsTick(std::int16_t rowMs,std::uint64_t ageNs) noexcept {
    return rowMs==std::int16_t(std::min<std::uint64_t>(ageNs/1'000'000ull,32000u));}
// KNOCK SERVO. The centre of mass of a falling body from 5 bones of its skeleton, weighted by Dempster's segment masses (fractions of the
// body: pelvis 0.281 = abdomen + pelvis, spine3 0.316 = thorax + both arms, head 0.081 = head + neck, each knee 0.161 = thigh + shank +
// foot); a missing bone's weight is left out (the rest renormalised), no pelvis or under half the weight = none. NmRsCBUStaggerFall::tick
// reads the character's centre-of-mass velocity (+0xb0), not the pelvis's (kick 1's pelvis read 28.7 kg of effective mass on a 77.7 kg
// body: the hips flex against the trunk).
inline constexpr unsigned comBones=5;
inline constexpr std::uint32_t staggerFallTaskVt=0x2d43850u; // NMStaggerFallTask's vtable (address point) as a librdr offset
inline constexpr std::array<float,comBones> comWeights{.281f,.316f,.081f,.161f,.161f}; // pelvis, spine3, head, knee L, knee R
struct ComRead {V3 p{};bool ok{};float weight{};};
inline ComRead comProxy(const V3* pts,const bool* have) noexcept {
    ComRead r{};if(!have[0]||!v::finite(pts[0]))return r;
    V3 sum{0,0,0};float w=0.f;
    for(unsigned i=0;i<comBones;++i)if(have[i]&&v::finite(pts[i])){sum=v::add(sum,v::mul(pts[i],comWeights[i]));w+=comWeights[i];}
    r.weight=w;if(!(w>=.5f))return r;r.p=v::mul(sum,1.f/w);r.ok=v::finite(r.p);return r;}
// The servo's caps: each kick at most servoKickCapNs, all kicks of a knockdown (kick 1 included) at most servoTotalCapNs (v16's kick 1 +
// kick 2: 150 + 100 N s, 43 sent, hp lost 0), at most servoMaxKicks after kick 1, within servoWindowNs of kick 1.
inline constexpr float servoKickCapNs=100.f,servoTotalCapNs=250.f,servoMinNs=1.f;inline constexpr unsigned servoMaxKicks=4;
inline constexpr std::uint64_t servoWindowNs=300'000'000ull;
enum ServoWhy : std::uint8_t {ServoRunning=0,ServoReached,ServoBudget,ServoMax,ServoTimeout,ServoSendFailed,ServoOff,ServoWhyCount};
inline constexpr std::array<const char*,ServoWhyCount> servoWhyNames{"running","reached","budget","max","timeout","sendFailed","off"};
// One servo step on a velocity measured over a whole frame after the last kick was consumed: no kick while the body moves along the push
// (kick2Wanted false: reached, the servo keeps watching until its window ends), stop out of budget or kicks; else the kick that sets the
// horizontal velocity to vDes along dir (kickImpulse) under the caps. vm in m/s, spentNs = the kicks so far (N s).
struct ServoStep {ServoWhy why{ServoRunning};V3 j{};};
inline ServoStep servoStep(float massKg,float vDes,V3 dir,V3 vm,float spentNs,unsigned kicks) noexcept {
    ServoStep r{};
    if(!kick2Wanted(vm,dir,vDes)){r.why=ServoReached;return r;}
    if(kicks>=servoMaxKicks){r.why=ServoMax;return r;}
    const float left=servoTotalCapNs-(std::isfinite(spentNs)?spentNs:servoTotalCapNs);
    const float cap=std::min(servoKickCapNs,left);if(!(cap>=servoMinNs)){r.why=ServoBudget;return r;}
    r.j=kickImpulse(massKg,vDes,dir,vm,cap);if(!(v::length(r.j)>=servoMinNs)){r.j={};r.why=ServoReached;}
    return r;}
// The shove's total push (metres) and its plan (at most 3 cm per tick, at least 6 ticks).
inline float shoveDistanceM(float a,bool strong) noexcept {
    const float d=std::clamp(.08f+.12f*((std::isfinite(a)?a:0.f)-1.f),.05f,.45f);return strong?d*1.6f:d;}
// A push's class in the victim's frame (dirFwd = push . row c, dirSide = push . row a): from behind (dirFwd >= 0), from the front, or
// along row a either way (|dirSide| > 0.7). The knockdown's reaction is chosen by the sign of dirFwd alone (pushFrontPlan).
enum PushClass : std::uint8_t {PushBehind=0,PushFront,PushSideA,PushSideB,PushClassCount};
inline constexpr std::array<const char*,PushClassCount> pushClassNames{"behind","front","sideA+","sideA-"};
inline PushClass pushClass(float dirFwd,float dirSide) noexcept {
    if(std::isfinite(dirSide)&&std::fabs(dirSide)>.7f)return dirSide>0.f?PushSideA:PushSideB;
    return std::isfinite(dirFwd)&&dirFwd<0.f?PushFront:PushBehind;}
// The knockdown message of a push from the front (lever pushfront; dirFwd < 0): SHOVED lands in staggerfall_front (face 0) or
// staggerfall_back (face 1) by the face, and NMStaggerFallTask carries the body forward along the victim's own facing either way (the
// behaviour reads no direction: NmRsCBUStaggerFall::tick 0x1f91fac..0x1f921ac), i.e. toward the pusher. 1 = face 0 with the impulse
// frontImp (an impulse size in 22..48 reached meleehit/#bc3dfb68, NMShotTask), 2 = face 1, 0 = the message unchanged. face / impulse
// -1 = unchanged.
struct FrontPlan {int face{-1};float impulse{-1.f};};
inline FrontPlan pushFrontPlan(unsigned lever,float dirFwd,float frontImp) noexcept {
    FrontPlan p{};if(lever==0||!std::isfinite(dirFwd)||!(dirFwd<0.f))return p;
    if(lever==1){p.face=0;p.impulse=std::isfinite(frontImp)&&frontImp>0.f?frontImp:40.f;return p;}
    p.face=1;return p;}
// The rest of a graded stumble after more push came in (a second hand, an escalated press, a stroke's peak): the distance for the larger
// effective approach less what was already applied (metres).
inline float stumbleRestM(float aEff,float appliedM) noexcept {return std::max(0.f,stumbleDistanceM(aEff)-(std::isfinite(appliedM)?std::max(appliedM,0.f):0.f));}
// STROKE PEAK (lever pushpeak): after a shove edge, the hand's peak arm speed into the body per tick (armIn plus the player's step in)
// for winNs while it stays on the creature. Out: 0 still open, 1 closed with the peak 10 % or more above the edge's a (a regrade), 2
// closed without one.
// armPeak = the arm's peak speed (|velArm|, m/s) over the stroke from the edge's: the regraded push's thrust test (pushThrust) takes it, so a
// stroke that starts slow and ends hard can be a knockdown.
struct StrokePeak {bool on{};std::uint32_t guid{};std::uint64_t startNs{};float edgeA{},peak{},armPeak{};};
inline void strokePeakStart(StrokePeak& p,std::uint32_t guid,float edgeA,std::uint64_t now,float edgeArm=0.f) noexcept {
    p=StrokePeak{};p.on=true;p.guid=guid;p.startNs=now?now:1;p.edgeA=std::isfinite(edgeA)?std::max(edgeA,0.f):0.f;p.peak=p.edgeA;
    p.armPeak=std::isfinite(edgeArm)?std::max(edgeArm,0.f):0.f;}
inline int strokePeakStep(StrokePeak& p,bool touching,bool armRead,float armIn,float locoIn,std::uint64_t now,std::uint64_t winNs,float armMps=-1.f) noexcept {
    if(!p.on)return 0;
    if(touching&&armRead&&std::isfinite(armIn)){const float x=armIn+(std::isfinite(locoIn)&&locoIn>0.f?locoIn:0.f);if(x>p.peak)p.peak=x;
        if(std::isfinite(armMps)&&armMps>p.armPeak)p.armPeak=armMps;}
    const bool ended=!touching||now<p.startNs||now-p.startNs>=winNs;
    if(!ended)return 0;
    p.on=false;return p.edgeA>0.f&&p.peak>=1.1f*p.edgeA?1:2;}
// A graded push's hands: each hand's graded approach (its edge, raised by its stroke peak) and push direction. The effective push is
// recomputed over the hands in it (pushMerge for two, pushSingle for one) whenever one of them changes, so a peak of either hand of a pair
// counts with the pair rule (max + 0.5 min), never with max alone; aEff never goes down.
struct PushHands {std::uint8_t hands{};float a[2]{};V3 d[2]{};};
inline PushSum pushHandsSum(const PushHands& p) noexcept {
    if((p.hands&3u)==3u)return pushMerge(p.a[0],p.d[0],p.a[1],p.d[1]);
    if(p.hands&1u)return pushSingle(p.a[0],p.d[0]);
    if(p.hands&2u)return pushSingle(p.a[1],p.d[1]);
    return PushSum{};}
// The input to a live push: a hand's edge (a new hand, or the same hand's first), its stroke peak (raises that hand's a), an escalated press
// (raises aEff to its own a at most: never merged with the pair rule, never a knockdown; the direction stays).
enum PushInput : std::uint8_t {PushInEdge=0,PushInPeak,PushInEsc};
inline PushSum pushRegrade(PushHands& p,float aEffPrev,V3 dirPrev,unsigned h,PushInput in,float a,V3 d) noexcept {
    h&=1u;const float x=std::isfinite(a)?std::max(a,0.f):0.f;const float prev=std::isfinite(aEffPrev)?std::max(aEffPrev,0.f):0.f;
    PushSum r{};
    if(in==PushInEsc){r.aEff=std::max(prev,x);r.dir=dirPrev;return r;}
    if(in==PushInPeak&&(p.hands>>h&1u))p.a[h]=std::max(p.a[h],x);
    else{p.a[h]=x;p.d[h]=v::length(V3{d[0],0.f,d[2]})>.5f?d:dirPrev;p.hands=std::uint8_t(p.hands|(1u<<h));}
    r=pushHandsSum(p);r.aEff=std::max(r.aEff,prev);
    if(!(v::length(r.dir)>.5f))r.dir=dirPrev;
    return r;}
// The decision after a regrade: an escalated press never turns a stumble into a knockdown; the others as pushDecide.
inline PushDecision pushRedecide(PushInput in,PushDecision was,float aEff,bool firm,float downMps) noexcept {
    if(was==PushKnockdown)return PushKnockdown;
    return in==PushInEsc?PushStumble:pushDecide(aEff,firm,downMps);}
// A graded stumble on a creature whose mover the offset cannot move: frozen (+0x848 > 0: mvrMoverInst::StartFrozen's count, ended only by
// EndFrozen from CleanupCustomAnim, aniAiCover, MoverManipulationTask::TaskEnd, the mover's own update or script SetMoverFrozen) takes the
// UP reaction message when pushFrozenMsg is on; suspended (+0x846 > 0) waits for the mover (waitMs > 0) or is dropped; a free mover is
// queued.
enum StumbleRoute : std::uint8_t {StumbleQueue=0,StumbleWait,StumbleMessage,StumbleDrop};
inline StumbleRoute stumbleRoute(bool pushable,bool busy,std::int16_t busy846,std::int16_t busy848,bool frozenMsg,float waitMs) noexcept {
    if(pushable)return StumbleQueue;
    if(!busy)return StumbleDrop;
    if(busy848>0&&frozenMsg)return StumbleMessage;
    return waitMs>0.f&&(busy846>0||busy848>0)?StumbleWait:StumbleDrop;}
// A knockdown's message by class (beside pushFrontPlan): from behind with the impulse's fallback size (the engine punch's k unread) the
// size behindImp; along row a (pushSide 1 / 2) the KNCKBK side reaction by the sign of dirSide. reaction / impulse -1 = unchanged.
struct SidePlan {int reaction{-1};float impulse{-1.f};};
inline SidePlan pushSidePlan(unsigned sideLever,float dirFwd,float dirSide,std::uint8_t impulseSrc,float behindImp) noexcept {
    SidePlan p{};
    if(sideLever&&std::isfinite(dirSide)&&std::fabs(dirSide)>.7f){const bool pos=dirSide>0.f;p.reaction=(sideLever==1)==pos?5:6;return p;}
    if(std::isfinite(dirFwd)&&dirFwd>=0.f&&impulseSrc==2&&std::isfinite(behindImp)&&behindImp>0.f)p.impulse=behindImp;
    return p;}
// KNOCKDOWN ROUTE (lever pushKnock): the knockdown message by the push's side in the victim's frame (dirFwd = push . row c: >= 0 from
// behind, < 0 from the front; a push along row a takes the row of its dirFwd sign, its sideways part comes from the kick) and the
// mover's frozen count at the message (+0x848 > 0). The engine's meleehit subtree picks the node from the message (ConditionInjuryFace
// +0x8b8; ConditionInjuryImpulseMag / VelocityMag compare |msg +0xa0| only, never its sign); the NM behaviour then falls in its own
// frame: staggerFall (task 0x2d43850) pitches the hips by the forward part of the centre of mass's velocity alone (NmRsCBUStaggerFall::tick
// 0x1f92308..0x1f92378: char +0xb0 against the COM matrix row +0x60 -> balancer +0xbf0; the sideways part, row +0x40, reaches only the
// spine lean; no vector from the message, so the body's own velocity and the kicks decide it), NMShot (task 0x2d42968) reads the bullet (+0x60 projected in fling_entry 0x1fa20e0;
// shotFromBehind when dot(char +0x60, bullet) <= 0 on a spine part, newHit_entry 0x1fa4a78). Rows (bullet: +1 along the push, -1 reversed):
//   R0 behind, free:  face 1, the impulse unchanged, +1   (staggerfall_back fell along the push for dirFwd 0.83 / 0.86; a diagonal push,
//                     dirFwd 0.48 / dirSide -0.88, fell backward in the victim's frame and sideways along it: unproven for side pushes,
//                     fallBy3 splits each row by push class; lever pushside 1 / 2 is the side test)
//   R1 front,  free:  pushFront 2: face 1, behindImp, -1  (the face-1 node of the free movers, the kick backward in the victim's frame;
//                     if NMShot comes it is the frozen front setup); 3: face 1, behindImp, +1; 1: face 0, frontImp; 0: unchanged (both
//                     fell toward the pusher)
//   R2 behind, frozen: face 0, frontImp, -1               (the front-frozen device setup mirrored: NMShot #731220e8 fell forward)
//   R3 front,  frozen: face 1, behindImp, -1              (the side-frozen device setup mirrored: NMShot #bc3dfb68 fell backward)
// The message keeps its kind (SHOVED, KO flag, damage 0, the creature itself as the attacker); the bullet's sign reaches only the
// impulse-size conditions and NM. Lever 2: frozen movers take the free rows. row 0xff = no plan (lever 0, an unread dirFwd).
// A reversed bullet (-1) mirrors the impulse's forward part in the victim's frame only (knockBulletFlip, lever pushKnockFlip 1): NMShot
// fell against the bullet's forward part and with its sideways part (one device knockdown: bullet +0.70 forward / +0.71 sideways, the
// pelvis -72 cm forward / +45 cm sideways), so a whole reversal would turn a diagonal push's sideways fall against the push.
// "frozen" is the push's own: +0x848 > 0 at the message, or the push sent or held the UP message of a frozen mover (that message clears
// +0x848 while the route stays NMShot: a device knockdown 0-27 ms after it read 0 and still ran NMShot).
enum KnockRow : std::uint8_t {KnockBehindFree=0,KnockFrontFree,KnockBehindFrozen,KnockFrontFrozen,KnockRowCount,KnockNone=0xff};
inline constexpr std::array<const char*,KnockRowCount> knockRowNames{"R0behindFree","R1frontFree","R2behindFrozen","R3frontFrozen"};
struct KnockPlan {std::uint8_t row{KnockNone};int face{-1};float impulse{-1.f};int bullet{0};};
inline KnockPlan knockPlan(unsigned lever,bool frozen,float dirFwd,unsigned frontLever,float frontImp,float behindImp) noexcept {
    KnockPlan p{};if(lever==0||lever>2||!std::isfinite(dirFwd))return p;
    const bool front=dirFwd<0.f,fz=frozen&&lever==1;
    const float fi=std::isfinite(frontImp)&&frontImp>0.f?frontImp:40.f,bi=std::isfinite(behindImp)&&behindImp>0.f?behindImp:75.f;
    if(!front&&!fz){p.row=KnockBehindFree;p.face=1;p.bullet=1;return p;}
    if(!front){p.row=KnockBehindFrozen;p.face=0;p.impulse=fi;p.bullet=-1;return p;}
    if(fz){p.row=KnockFrontFrozen;p.face=1;p.impulse=bi;p.bullet=-1;return p;}
    p.row=KnockFrontFree;
    if(frontLever==2){p.face=1;p.impulse=bi;p.bullet=-1;}
    else if(frontLever==3){p.face=1;p.impulse=bi;p.bullet=1;}
    else if(frontLever==1){p.face=0;p.impulse=fi;p.bullet=1;}
    else{p.bullet=1;}
    return p;}
// A row's reversed bullet: flip 1 mirrors the impulse b about the plane across the victim's facing (rowC made horizontal: b - 2 (b . c) c,
// the sideways and vertical parts kept), flip 0 (or no usable rowC) reverses all of b. The size is kept either way.
inline V3 knockBulletFlip(V3 b,V3 rowC,V3 up,unsigned flip) noexcept {
    if(!v::finite(b))return b;const V3 un=v::normalized(up);
    if(flip==0||!v::finite(rowC)||v::length(un)<.5f)return v::mul(b,-1.f);
    const V3 ch=v::sub(rowC,v::mul(un,v::dot(rowC,un)));const float l=v::length(ch);if(!(l>1e-3f))return v::mul(b,-1.f);
    const V3 c=v::mul(ch,1.f/l);return v::sub(b,v::mul(c,2.f*v::dot(b,c)));}
// KNOCK PICK. The face-1 children of the meleehit subtree's #1d19797e are matched in order #bc3dfb68 (Face 1, Random roll % 2 == 0, NOT
// Group: NMShot) then #02d1adb2 (Face 1, Random roll % 2 == 1, OR Face 0, Group: staggerFall); the face-0 pair #731220e8 / #9974d115 the
// same. rage::ConditionRandom::Match 0x2900ca0, mode 0 (+0x8): sm_RandomUsed == 0 reuses sm_RandomNum (0x2900ce0..0x2900cf4), else a new
// roll is drawn (sm_RandFunc or g_ReplayRand), stored, Used = 0 (0x2900cf8..0x2900de8); the match is roll % (+0x14) == (+0x10) and sets
// Used = 1 (0x2900dec..0x2900e10). A failed NMShot random leaves Used 0, so the staggerFall random reads the same roll: the parity picks.
// staggerFall falls forward in the victim's frame (along the push from behind, 9 of 9 device knockdowns; sideways from the front, 0 of 4
// along). NMShot by the bullet's sign (device rows): side pushes with the bullet reversed 2 of 2 along, with the bullet along the push 0 of
// 3; a pure front push with the bullet reversed 0 of 1 (sideways). The front has no recipe on record.
// The parity of a knockdown (-1 = the engine's coin): lever 1 staggerFall (1) for dirFwd >= fwdMin, NMShot (0) otherwise; 2 NMShot;
// 3 staggerFall; 0 or an unread dirFwd the coin.
inline constexpr int knockPickParity(unsigned lever,float dirFwd,float fwdMin) noexcept {
    return lever==2?0:lever==3?1:lever==1&&dirFwd==dirFwd&&dirFwd>=-1.01f&&dirFwd<=1.01f?(dirFwd>=fwdMin?1:0):-1;}
// The roll written: the stored one with its lowest bit set to the parity (kept in the engine's 31-bit range).
inline constexpr std::int32_t knockPickNum(std::int32_t num,int parity) noexcept {
    return std::int32_t((std::uint32_t(num)&0x7ffffffeu)|(parity&1));}
// The pick's own message (lever pushKnockPickMsg 1, any row): face 1 (the face-1 pair of nodes), the size behindImp, the bullet reversed
// (an A/B setup: no device knockdown recorded with it).
inline KnockPlan knockPickPlan(std::uint8_t row,float behindImp) noexcept {
    KnockPlan p{};p.row=row;p.face=1;p.impulse=std::isfinite(behindImp)&&behindImp>0.f?behindImp:75.f;p.bullet=-1;return p;}
// The knockdown's message: the pick's (knockPickPlan) only when the behaviour is forced (parity >= 0) and lever pushKnockPickMsg asks
// for it (the caller passes -1 otherwise), else the route table's own (base).
inline KnockPlan knockMessagePlan(const KnockPlan& base,int parity,float dirFwd,float behindImp) noexcept {
    if(parity<0||!std::isfinite(dirFwd))return base;
    return knockPickPlan(base.row!=KnockNone?base.row:std::uint8_t(dirFwd<0.f?KnockFrontFree:KnockBehindFree),behindImp);}
// The injury subtree's random words as the pick needs them (one child of #1d19797e: its name hash, a condition's Match offset and its
// words +0x8 / +0xc / +0x10 / +0x14): true = a ConditionRandom in mode 0 on roll % 2 with the remainder this child selects on.
inline constexpr std::uint32_t conditionRandomMatch=0x2900ca0u;
inline constexpr bool knockPickChildOk(std::uint32_t id,std::uint32_t fn,std::uint32_t mode,std::uint32_t value,std::uint32_t mod) noexcept {
    const int want=id==0x731220e8u||id==0xbc3dfb68u?0:id==0x9974d115u||id==0x02d1adb2u?1:-1;
    return want>=0&&fn==conditionRandomMatch&&mode==0u&&mod==2u&&value==std::uint32_t(want);}
// The behaviour of a landed node: 1 staggerFall (#02d1adb2 / #9974d115), 0 NMShot (#bc3dfb68 / #731220e8), -1 another node.
inline constexpr int knockNodeParity(std::uint32_t id) noexcept {
    return id==0x02d1adb2u||id==0x9974d115u?1:id==0xbc3dfb68u||id==0x731220e8u?0:-1;}
// The UP message of a stumble on a frozen mover held for the other hand (lever pushFrozenHold, one slot per hand): armed with the push's
// record and due at the touch plus holdMs (0 = off: sent at once); a knockdown of the same push (guid, record) before then drops it
// (superseded), else it is sent once due. Arming a slot that still holds another message replaces it (*replaced).
struct FrozenHoldSlot {bool on{};std::uint32_t guid{};std::uint64_t recNs{},due{};};
inline std::uint64_t frozenHoldDue(std::uint64_t now,float holdMs) noexcept {
    return std::isfinite(holdMs)&&holdMs>0.f?now+std::uint64_t(double(std::min(holdMs,1000.f))*1e6):0ull;}
inline bool frozenHoldArm(FrozenHoldSlot& s,std::uint32_t guid,std::uint64_t recNs,std::uint64_t now,float holdMs,bool* replaced=nullptr) noexcept {
    if(replaced)*replaced=false;const std::uint64_t due=frozenHoldDue(now,holdMs);if(!due||!guid)return false;
    if(replaced)*replaced=s.on;s=FrozenHoldSlot{true,guid,recNs,due};return true;}
inline bool frozenHoldDrop(FrozenHoldSlot& s,std::uint32_t guid,std::uint64_t recNs) noexcept {
    if(!s.on||s.guid!=guid||s.recNs!=recNs)return false;s=FrozenHoldSlot{};return true;}
inline bool frozenHoldDueNow(const FrozenHoldSlot& s,std::uint64_t now) noexcept {return s.on&&now>=s.due;}
// Two slots per hand: a new hold takes the first free one; with both taken nothing is held (the message goes out at once), so a held
// message is never replaced. -1 = none free.
inline int frozenHoldFree(const FrozenHoldSlot& a,const FrozenHoldSlot& b) noexcept {return !a.on?0:!b.on?1:-1;}
// The knockdown's frozen route: +0x848 > 0 at the message, or the push itself sent or held the frozen mover's UP message.
inline constexpr bool knockFrozen(std::int16_t b8,bool pushFrozen) noexcept {return b8>0||pushFrozen;}
// KNOCK TOUGH. ConditionInjuryToughActor::Match 0x1ccec48: [[ctx +0x28] +0x60] +0x151 bit 7 = health +0x150 bit 0x8000 (written only by
// sagActorScript::SetToughActor 0x1cab354 and gohActorEnum::SetToughActor 0x2464b78, 32-bit read-modify-writes). The meleehit subtree
// #1d19797e matches #8b669f52 (tough actor OR player controlled) ahead of the four knockdown nodes, so a tough actor never reaches them:
// its stagger leans back and never falls (13 of 24 ring knockdowns of one device log, all under #8b669f52). The window clears the bit for the
// knockdown's message only and puts it back once the node landed (120 ms at most, a later tick past 250 ms as the backstop).
inline constexpr std::uint32_t toughBit=0x8000u;
inline constexpr std::uint64_t toughHoldNs=120'000'000ull,toughLateNs=250'000'000ull;
// KNOCK LANDED: the index of the first node sample taken after the knockdown's message. The watches sample at the start of consumeTouch,
// before the tick's messages: a sample of the message's own tick has the message's age but predates it. atMsg = the sample count at the
// message (samples [0, atMsg) came before it); atMsg < 0 (not recorded) = the first sample at or after kms ms. -1 = none yet.
template<class Tr>
inline int knockFirstAfter(const Tr& tr,unsigned n,int atMsg,unsigned kms) noexcept {
    const unsigned cnt=std::min<unsigned>(n,unsigned(tr.size()));
    if(atMsg>=0)return unsigned(atMsg)<cnt?atMsg:-1;
    for(unsigned k=0;k<cnt;++k)if(tr[k].ms>=kms)return int(k);
    return -1;}
// The restore's verdict: the actor still in its slot with the same health block (else Lost), the bit still clear (else a script set it
// again in between: left as it is).
enum ToughRestore : std::uint8_t {ToughRestored=0,ToughScriptChanged,ToughLost};
inline constexpr ToughRestore toughRestoreVerdict(bool actorSame,bool healthSame,std::uint32_t word) noexcept {
    return !actorSame||!healthSame?ToughLost:(word&toughBit)?ToughScriptChanged:ToughRestored;}
// KNOCK PICK draw function (lever pushKnockPickFunc). rage::ConditionRandom::Match 0x2900ca0 draws a new roll through sm_RandFunc(ctx)
// when it is set (0x2900d04..0x2900d10, 0x2900d48..0x2900d54; GetRandInt 0x2900c60 the same), else through g_ReplayRand (GOT 0x2ddeea8):
// x = lo * 0x5cdcfaa7 + hi stored as 64 bits, the roll = lo & 0x7fffffff (0x2900dc0..0x2900de0). While a pick is held the function
// is pickFnDraw: the same step on the same variable (every other condition gets the engine's own value), the victim's own conditions
// (ctx +0x28 = the actor, as ConditionInjuryToughActor reads it) the chosen parity. Set only while sm_RandFunc is null (single player).
struct PickFnState {std::atomic<std::uintptr_t> victim{},replay{};std::atomic<int> parity{-1};std::atomic<std::uint64_t> victimDraws{},otherDraws{};};
inline PickFnState pickFn{};
inline constexpr std::uint64_t replayStep(std::uint64_t x) noexcept {return std::uint64_t(std::uint32_t(x))*0x5cdcfaa7ull+(x>>32);}
inline int pickFnDraw(const void* ctx) noexcept {
    const auto r=pickFn.replay.load(std::memory_order_relaxed);if(!r)return 0;
    auto* p=reinterpret_cast<volatile std::uint64_t*>(r);const std::uint64_t x=replayStep(*p);*p=x;
    std::int32_t n=std::int32_t(std::uint32_t(x)&0x7fffffffu);
    const auto victim=pickFn.victim.load(std::memory_order_acquire);
    if(victim&&ctx){std::uintptr_t a{};std::memcpy(&a,static_cast<const unsigned char*>(ctx)+0x28,sizeof a);
        const int par=pickFn.parity.load(std::memory_order_relaxed);
        if(a==victim&&par>=0){pickFn.victimDraws.fetch_add(1,std::memory_order_relaxed);return knockPickNum(n,par);}}
    pickFn.otherDraws.fetch_add(1,std::memory_order_relaxed);return n;}
// KNOCKDOWN HANDS-OFF (lever knockOffMs). The hand bodies' pairs with class-5 instances (sagFragInst / sagFragInstNM: a creature's frag,
// its ragdoll once NM runs) are kept by collide(); an active NM ragdoll then takes the hand's drive (2 kg, up to pushSpeedMps into a
// dynamic contact) along whatever normal the contact has, also while the hand returns after the push. VrMelee arms a slot at a
// knockdown's message (the frag instance of the kick, the actor); the collide filter drops a hand's pair with that instance (or an
// instance whose user is that actor) until `until`, or only counts it with drop off. The slot ends early once the mover's ragdoll word
// reads 0 again after 300 ms (the creature is up). Counters: pairs within 0-600 ms / later, the hand's highest speed toward the pusher
// while paired (mm/s, against the push direction).
struct KnockOffSlot {std::atomic<std::uintptr_t> inst{},actor{};std::atomic<std::uint64_t> until{},armedNs{};std::atomic<bool> drop{};
    std::array<std::atomic<float>,3> dir{};std::atomic<std::uint32_t> pairsEarly{},pairsLate{},towardMmsMax{};};
inline std::array<KnockOffSlot,4> knockOff{};
// Bit i = slot i armed (its until set): the collide filter's idle test (one relaxed load, no reads of the other instance with no window).
inline std::atomic<std::uint32_t> knockOffLive{};
inline void knockOffSetUntil(unsigned i,std::uint64_t until) noexcept {
    i&=3u;knockOff[i].until.store(until,std::memory_order_release);
    if(until)knockOffLive.fetch_or(1u<<i,std::memory_order_release);else knockOffLive.fetch_and(~(1u<<i),std::memory_order_release);}
inline constexpr std::uint64_t knockOffEarlyNs=600'000'000ull,knockOffUpNs=300'000'000ull;
// The slot of a class-5 pair live at now (inst = the other instance, userActor = its user's actor, 0 none): -1 none.
inline int knockOffMatch(std::uintptr_t inst,std::uintptr_t userActor,std::uint64_t now) noexcept {
    for(unsigned i=0;i<knockOff.size();++i){const auto& k=knockOff[i];const auto until=k.until.load(std::memory_order_acquire);
        if(!until||now>=until)continue;
        const auto si=k.inst.load(std::memory_order_relaxed),sa=k.actor.load(std::memory_order_relaxed);
        if((si&&inst==si)||(sa&&userActor&&userActor==sa))return int(i);}
    return -1;}
// The hand's speed toward the pusher (m/s): its velocity against the push direction (horizontal), 0 when it moves along the push.
inline float knockOffToward(V3 handVel,V3 pushDir) noexcept {
    const V3 d=v::normalized(V3{pushDir[0],0.f,pushDir[2]});if(v::length(d)<.5f||!v::finite(handVel))return 0.f;
    const float t=-v::dot(handVel,d);return t>0.f?t:0.f;}
struct PushPlan {float perTickM{};unsigned ticks{};};
inline PushPlan pushPlan(float distM) noexcept {
    PushPlan p{};if(!(distM>0.f)||!std::isfinite(distM))return p;
    p.ticks=std::max(6u,unsigned(std::ceil(distM/.03f-1e-4f)));p.perTickM=distM/float(p.ticks);return p;}
// One press tick (metres): gain * (depth - 1 cm), capped per tick and by what the press has left of its cap.
inline float pressStepM(float depthM,float gain,float maxM,float doneM,float capM) noexcept {
    if(!std::isfinite(depthM))return 0.f;
    const float s=std::clamp(gain*(depthM-.01f),0.f,std::max(maxM,0.f));return std::clamp(s,0.f,std::max(0.f,capM-doneM));}
// The press yaw (rad per tick): the torque of a push F at p about the pivot, along up, scaled and capped.
inline float pressYaw(V3 p,V3 pivot,V3 F,V3 up,float upm,float k=.15f,float cap=.05f) noexcept {
    const float y=v::dot(v::cross(v::mul(v::sub(p,pivot),1.f/std::max(upm,1e-3f)),F),v::normalized(up))*k;
    return std::isfinite(y)?std::clamp(y,-cap,cap):0.f;}
// The slap's yaw (rad, spread over 3 ticks): 6 % of the tangential speed, its sign from the side.
inline float slapYaw(float tangent,float side,float cap) noexcept {
    const float y=std::clamp(.06f*(std::isfinite(tangent)?tangent:0.f),0.f,std::max(cap,0.f));return side<0.f?-y:y;}
// The push guards of a creature's mover (VrMelee, aniMelee::ApplyMinDist's own checks plus ours): -1 = pushable, else the reason
// (0 not human (race / creature +0xa30), 1 mover vtable (neither mover class), 2 ragdoll (+0xb8c != 0: PerformPhysicalUpdate 0x2892d70
// teleports and SetTranslationOffset(0) drops any offset), 3 flags (+0xb85 & 0x2050000: ApplyTranslationOffset 0x288aa48 refuses with
// force false), 4 busy (+0x846 / +0x848 > 0: GetDesiredMotion routes the offset to the attachment)). The heading +0x18 is no refusal:
// the offset is pre-rotated (preRotate).
enum PushWhy : int {PushOk=-1,PushWhyNotHuman=0,PushWhyMoverVt,PushWhyRagdoll,PushWhyFlags,PushWhyBusy};
inline constexpr int pushGuard(int race,int creatureRace,bool moverVt,std::int16_t busyA,std::int16_t busyB,std::uint32_t ragdoll,std::uint32_t flags) noexcept {
    return race!=0||creatureRace!=0?PushWhyNotHuman:!moverVt?PushWhyMoverVt:ragdoll!=0?PushWhyRagdoll:busyA>0||busyB>0?PushWhyBusy:(flags&0x2050000u)?PushWhyFlags:PushOk;}
// ApplyTranslationOffset 0x288aac8..0x288aafc turns the offset about Y by the mover's heading h (+0x18) before it adds it:
// x' = x cos h + z sin h, z' = z cos h - x sin h (sincosf; skipped for h == 0, and for the mover word +0xb85 bit 0x4000000, whose branch
// 0x288aac4 -> 0x288ab50 keeps the larger of the stored and the new component unturned). preRotate applies the inverse, so the stored
// offset is the world vector v.
inline constexpr std::uint32_t moverNoTurnFlag=0x4000000u;
inline V3 preRotate(V3 v,float h,std::uint32_t flags=0) noexcept {
    if(!std::isfinite(h)||h==0.f||(flags&moverNoTurnFlag))return v;
    const float c=std::cos(h),s=std::sin(h);return {v[0]*c-v[2]*s,v[1],v[0]*s+v[2]*c};}
// The engine's own turn of an offset (for the host test of preRotate).
inline V3 engineHeadingTurn(V3 v,float h,std::uint32_t flags=0) noexcept {
    if(h==0.f||(flags&moverNoTurnFlag))return v;const float c=std::cos(h),s=std::sin(h);return {v[0]*c+v[2]*s,v[1],v[2]*c-v[0]*s};}
// The part of n across up (a push along the ground), unit or 0.
inline V3 horizontal(V3 n,V3 up) noexcept {const V3 u=v::normalized(up);return v::normalized(v::sub(n,v::mul(u,v::dot(n,u))));}
// The attacker guid of a touch: actor slot 0 with a generation it never has (slots[0].gen ^ 0x4000). aGuidManager::Init 0x290af84
// writes slot 0's generation as the constant 0x8000 and the actor manager's ids start at 2 (Init(128, placeholder, 2) 0x23b5d40), so
// slot 0 is never allocated and gen0 ^ 0x8000 is always 0. Every guid consumer of the injure message
// checks the generation and ignores this one (ReportWasHitMelee 0x1e4d4d0, HandleInjure 0x1e84828, NotifyInjured 0x1e1857c). Never slot 1
// (generation 0 resolves to the placeholder actor). 0 = none possible.
inline constexpr std::uint32_t fakeGuid(std::uint16_t gen0) noexcept {return std::uint16_t(gen0^0x4000u)?std::uint32_t(std::uint16_t(gen0^0x4000u))<<16:0u;}
inline std::atomic<bool> nearHorses{}; // horses take the virtual body (lever npchorse)
inline void finishBody(VBody& b) noexcept {
    const unsigned n=std::min<unsigned>(b.count,unsigned(vbodyCaps));b.count=std::uint8_t(n);
    V3 lo{1e30f,1e30f,1e30f},hi{-1e30f,-1e30f,-1e30f};float rmax=0.f;
    for(unsigned i=0;i<n;++i){const auto& c=b.c[i];rmax=std::max(rmax,c.r);
        for(int k=0;k<3;++k){lo[std::size_t(k)]=std::min({lo[std::size_t(k)],c.a[std::size_t(k)],c.b[std::size_t(k)]});
                             hi[std::size_t(k)]=std::max({hi[std::size_t(k)],c.a[std::size_t(k)],c.b[std::size_t(k)]});}}
    if(!n||!v::finite(lo)||!v::finite(hi)){b.count=0;b.radius=0.f;return;}
    b.centre=v::mul(v::add(lo,hi),.5f);float r=0.f;
    for(unsigned i=0;i<n;++i)r=std::max({r,v::length(v::sub(b.c[i].a,b.centre)),v::length(v::sub(b.c[i].b,b.centre))});
    b.radius=r+rmax;if(!v::finite(b.vel))b.vel={0,0,0};
}
inline V3 closestOnSegment(V3 a,V3 b,V3 p) noexcept {
    const V3 ab=v::sub(b,a);const float l2=v::dot(ab,ab);
    const float t=l2>1e-12f?std::clamp(v::dot(v::sub(p,a),ab)/l2,0.f,1.f):0.f;return v::add(a,v::mul(ab,t));
}
// The distance between the segments p1..q1 and p2..q2.
inline float segmentSegmentDistance(V3 p1,V3 q1,V3 p2,V3 q2) noexcept {
    const V3 d1=v::sub(q1,p1),d2=v::sub(q2,p2),r=v::sub(p1,p2);const float a=v::dot(d1,d1),e=v::dot(d2,d2),f=v::dot(d2,r);float s=0.f,t=0.f;
    if(a<=1e-12f&&e<=1e-12f)return v::length(r);
    if(a<=1e-12f)t=std::clamp(f/e,0.f,1.f);
    else{const float c=v::dot(d1,r);
        if(e<=1e-12f)s=std::clamp(-c/a,0.f,1.f);
        else{const float b=v::dot(d1,d2),den=a*e-b*b;s=den>1e-12f?std::clamp((b*f-c*e)/den,0.f,1.f):0.f;t=(b*s+f)/e;
            if(t<0.f){t=0.f;s=std::clamp(-c/a,0.f,1.f);}else if(t>1.f){t=1.f;s=std::clamp((b-c)/a,0.f,1.f);}}}
    return v::length(v::sub(v::add(p1,v::mul(d1,s)),v::add(p2,v::mul(d2,t))));
}
// The hand's probes against the bodies: the palm centre (r 3.5 cm) and the open fingers (B + F 9 cm, r 3 cm) or the fist (B + F 4 cm
// + N 1.7 cm, r 3.5 cm), each kept 5 mm outside every capsule it would enter on its way from the body's position `from` to the command
// c (two passes). The clip is the capsule's tangent plane on the side of `from`, so a fast hand whose command lies past the capsule's
// axis stops on its near side instead of being pushed out through the far one. The capsules are moved on by vel * min(age, 50 ms); a
// body whose sphere is over 30 cm from c is skipped. c: in raw, out clipped; depth = the deepest probe penetration of the raw command
// (world units), n = out of that capsule towards the hand, point = its surface.
struct VirtualHit {bool hit{};V3 n{},point{};float depth{};int body{-1},cap{-1},kind{-1},zone{-1};};
inline VirtualHit pushOut(V3& c,V3 from,const Rows& rows,bool fist,const VBodies& vb,float ageS,float upm) noexcept {
    VirtualHit h{};if(!v::finite(c)||!vb.count)return h;
    if(!v::finite(from))from=c;
    const float u=std::max(upm,1e-3f);const float age=std::isfinite(ageS)?std::clamp(ageS,0.f,.05f):0.f;
    const V3 F=v::normalized(rows[1]),N=v::normalized(rows[2]);
    const std::array<V3,2> off{V3{0,0,0},fist?v::add(v::mul(F,.04f*u),v::mul(N,.017f*u)):v::mul(F,.09f*u)};
    const std::array<float,2> pr{.035f*u,fist?.035f*u:.03f*u};
    const float skin=.005f*u;
    const unsigned nb=std::min<unsigned>(vb.count,unsigned(vbodyMax));
    for(int pass=0;pass<2;++pass)for(unsigned bi=0;bi<nb;++bi){
        const auto& b=vb.b[bi];if(!b.count)continue;
        const V3 shift=v::finite(b.vel)?v::mul(b.vel,age):V3{0,0,0};
        if(v::length(v::sub(v::add(b.centre,shift),c))>b.radius+.30f*u)continue;
        for(unsigned k=0;k<std::min<unsigned>(b.count,unsigned(vbodyCaps));++k){
            const auto& cap=b.c[k];const V3 a=v::add(cap.a,shift),e=v::add(cap.b,shift);
            const V3 cm=v::mul(v::add(a,e),.5f);const float ch=.5f*v::length(v::sub(e,a));
            for(std::size_t i=0;i<off.size();++i){
                const float R=cap.r+pr[i]+skin;
                const V3 p=v::add(c,off[i]),pf=v::add(from,off[i]);
                // the spheres round the capsule's segment and the probe's move first (most capsules of a body are far from the hand)
                if(v::length(v::sub(cm,v::mul(v::add(pf,p),.5f)))>ch+.5f*v::length(v::sub(p,pf))+R)continue;
                if(!(segmentSegmentDistance(pf,p,a,e)<R))continue;
                const V3 qf=closestOnSegment(a,e,pf);const V3 df=v::sub(pf,qf);const float dfl=v::length(df);
                V3 n=dfl>1e-6f*u?v::mul(df,1.f/dfl):v::normalized(v::sub(p,closestOnSegment(a,e,p)));
                if(v::length(n)<.5f)n=v::normalized(v::sub(p,v::add(b.centre,shift)));
                if(v::length(n)<.5f)n={0,1,0};
                const float d=R-v::dot(v::sub(p,qf),n);
                if(!(d>0.f)||!std::isfinite(d))continue;
                if(pass==0&&d>h.depth){h.hit=true;h.depth=d;h.n=n;h.point=v::add(qf,v::mul(n,cap.r));h.body=int(bi);h.cap=int(k);h.kind=cap.kind;h.zone=cap.zone;}
                c=v::add(c,v::mul(n,d));
            }
        }
    }
    return h;
}
// The props woken recently (by a contact or a reach): a prop is woken again only gapNs after its last wake. The oldest entry is replaced.
struct WakeLru {
    static constexpr std::size_t size=16;
    static constexpr std::uint64_t gapNs=1'000'000'000ull;
    std::array<std::uintptr_t,size> inst{};std::array<std::uint64_t,size> ns{};
    bool recent(std::uintptr_t p,std::uint64_t now) const noexcept {
        for(std::size_t i=0;i<size;++i)if(inst[i]==p&&p)return now>=ns[i]&&now-ns[i]<gapNs;
        return false;
    }
    void note(std::uintptr_t p,std::uint64_t now) noexcept {
        std::size_t slot=0;
        for(std::size_t i=0;i<size;++i){if(inst[i]==p){slot=i;break;}if(!inst[i]){slot=i;break;}if(ns[i]<ns[slot])slot=i;}
        inst[slot]=p;ns[slot]=now;
    }
};
// Per-hand spacing of contact wakes.
inline constexpr std::uint64_t handWakeGapNs=250'000'000ull;

// ============================== REACH WAKE (tick) ==============================
// The inactive props within reach of a live palm are woken before the hand meets them: the capsule from the palm B to B + vel * lead
// (lead capped at reachMaxLeadM), radius reachM, both ends kept within reachChestM of the chest point. The engine cull takes the sphere
// round that capsule; each object found is checked again against the capsule with its level bounding sphere, its radius taken as at
// most reachObjMaxM (a long bench, a fence or a rail has a sphere of metres round its middle: such a prop is woken by a touch, not by
// passing within metres of its centre).
inline constexpr float reachChestM=.90f,reachMaxLeadM=.40f,reachObjMaxM=.40f;
// A query every reachEveryTicks ticks, and only once a live palm moved reachMoveM since the last one (a still hand finds what it found).
inline constexpr unsigned reachEveryTicks=6;inline constexpr float reachMoveM=.05f;
struct ReachSphere {V3 a{},b{},centre{};float r{},cullR{},objMax{};bool ok{};};
inline V3 clampToSphere(V3 p,V3 c,float r) noexcept {const V3 d=v::sub(p,c);const float l=v::length(d);return l>r&&l>0.f?v::add(c,v::mul(d,r/l)):p;}
inline ReachSphere reachSphere(V3 palm,V3 vel,V3 chest,float reachM,float leadS,float upm) noexcept {
    ReachSphere s{};if(!v::finite(palm)||!v::finite(chest))return s;
    V3 lead=v::finite(vel)?v::mul(vel,leadS):V3{0,0,0};
    const float ll=v::length(lead),cap=reachMaxLeadM*upm;if(ll>cap&&ll>0.f)lead=v::mul(lead,cap/ll);
    s.a=clampToSphere(palm,chest,reachChestM*upm);s.b=clampToSphere(v::add(palm,lead),chest,reachChestM*upm);
    s.r=reachM*upm;s.objMax=reachObjMaxM*upm;s.centre=v::mul(v::add(s.a,s.b),.5f);s.cullR=s.r+v::length(v::sub(s.b,s.a))*.5f;s.ok=v::finite(s.centre)&&std::isfinite(s.cullR);
    return s;
}
// The distance from p to the segment a..b.
inline float segmentDistance(V3 a,V3 b,V3 p) noexcept {
    const V3 ab=v::sub(b,a);const float l2=v::dot(ab,ab);
    const float t=l2>1e-12f?std::clamp(v::dot(v::sub(p,a),ab)/l2,0.f,1.f):0.f;
    return v::length(v::sub(p,v::add(a,v::mul(ab,t))));
}
// How far an object's bounding sphere (radius capped at objMax) is outside the reach capsule (<= 0: within reach).
inline float reachGap(const ReachSphere& s,V3 objCentre,float objRadius) noexcept {
    if(!s.ok||!v::finite(objCentre)||!std::isfinite(objRadius))return 1e30f;
    return segmentDistance(s.a,s.b,objCentre)-s.r-std::clamp(objRadius,0.f,s.objMax);
}

// ============================== NPC bump (tick) ==============================
// VrMelee's scan (ModuleMelee runs before ModuleHandPhys, both on the main thread) publishes the creatures near the player; the tick
// sends mvrMsgBumpedInto to the one a live palm touches. pos = the creature's matrix row d (its feet).
struct NearCreature {std::uintptr_t actor{};std::uint32_t guid{};V3 pos{};int race{-1};};
struct Nearby {std::uint64_t ns{};std::uint32_t count{};std::array<NearCreature,8> c{};};
inline ::rdrvr::vr::body::Seqlock<Nearby> nearby;
inline std::atomic<bool> nearWanted{};         // a body is live and the npcbump lever is on: VrMelee scans while unarmed
inline std::atomic<unsigned> nearScanTicks{9};
// A coarse creature capsule: within 0.35 m (0.32 to start) of its axis, 0.2..2.0 m above its feet.
inline bool touchesCreature(V3 palm,V3 feet,V3 up,bool touching,float upm) noexcept {
    const V3 u=v::normalized(up),d=v::sub(palm,feet);const float h=v::dot(d,u);const float r=v::length(v::sub(d,v::mul(u,h)));
    return v::finite(d)&&h>=.2f*upm&&h<=2.f*upm&&r<(touching?.35f:.32f)*upm;
}
// One bump when a touch starts, at most 2 repeats 1 s apart while it lasts, a 2 s cooldown per creature after it ends, 250 ms between
// any two bumps. seen = the raw touch (gated or not): edge = its rising edge, gated = that edge did not send.
struct BumpGate {
    static constexpr std::uint64_t repeatNs=1'000'000'000ull,coolNs=2'000'000'000ull,globalNs=250'000'000ull;
    static constexpr unsigned maxSends=3;
    struct Entry {std::uint32_t guid{};bool touching{},seen{};unsigned sent{};std::uint64_t lastNs{},coolUntil{},seenNs{};};
    std::array<Entry,8> e{};std::uint64_t lastAny{};
    bool touching(std::uint32_t guid) const noexcept {for(const auto& x:e)if(x.guid==guid&&guid)return x.touching;return false;}
    bool seen(std::uint32_t guid) const noexcept {for(const auto& x:e)if(x.guid==guid&&guid)return x.seen;return false;}
    // true = send a bump to this creature now
    bool update(std::uint32_t guid,bool touch,std::uint64_t now,bool* edge=nullptr,bool* gated=nullptr) noexcept {
        if(edge)*edge=false;
        if(gated)*gated=false;
        if(!guid)return false;
        Entry* x=nullptr;for(auto& y:e)if(y.guid==guid){x=&y;break;}
        if(!x){if(!touch)return false;x=&e[0];for(auto& y:e){if(!y.guid){x=&y;break;}if(!y.touching&&!y.seen&&y.seenNs<x->seenNs)x=&y;}*x=Entry{};x->guid=guid;}
        x->seenNs=now;
        const bool rising=touch&&!x->seen;x->seen=touch;
        if(edge)*edge=rising;
        const bool global=!lastAny||now<lastAny||now-lastAny>=globalNs;
        if(!touch){if(x->touching){x->touching=false;x->coolUntil=now+coolNs;}return false;}
        if(!x->touching){
            if(now<x->coolUntil||!global){if(gated)*gated=rising;return false;}
            x->touching=true;x->sent=1;x->lastNs=now;lastAny=now;return true;}
        if(x->sent<maxSends&&now>=x->lastNs+repeatNs&&global){++x->sent;x->lastNs=now;lastAny=now;return true;}
        return false;
    }
};
// A press (the palm 1 cm or more into a virtual body): a bump every tick for at most 2 s, then a 1.5 s rest (a held press starts again
// after it). The animator only builds its bump time from a bump in every one of its updates.
struct PressGate {
    static constexpr std::uint64_t maxNs=2'000'000'000ull,restNs=1'500'000'000ull;
    enum Out : std::uint8_t {None=0,Start,Send,Rest};
    struct Entry {std::uint32_t guid{};bool pressing{},resting{};std::uint64_t start{},restUntil{},seenNs{};};
    std::array<Entry,8> e{};
    Out update(std::uint32_t guid,bool press,std::uint64_t now) noexcept {
        if(!guid)return None;
        Entry* x=nullptr;for(auto& y:e)if(y.guid==guid){x=&y;break;}
        if(!x){if(!press)return None;x=&e[0];for(auto& y:e){if(!y.guid){x=&y;break;}if(!y.pressing&&y.seenNs<x->seenNs)x=&y;}*x=Entry{};x->guid=guid;}
        x->seenNs=now;
        if(!press){x->pressing=false;return None;}
        if(now<x->restUntil)return None;
        if(!x->pressing){x->pressing=true;x->start=now;return Start;}
        if(now>=x->start&&now-x->start>=maxNs){x->pressing=false;x->restUntil=now+restNs;return Rest;}
        return Send;
    }
};
// Slaps: one per creature per 800 ms, one per 300 ms overall.
struct SlapGate {
    static constexpr std::uint64_t perNs=800'000'000ull,globalNs=300'000'000ull;
    std::array<std::uint32_t,8> guid{};std::array<std::uint64_t,8> ns{};std::uint64_t lastAny{};unsigned next{};
    bool allow(std::uint32_t g,std::uint64_t now) noexcept {
        if(!g)return false;
        if(lastAny&&now>=lastAny&&now-lastAny<globalNs)return false;
        for(std::size_t i=0;i<guid.size();++i)if(guid[i]==g&&now>=ns[i]&&now-ns[i]<perNs)return false;
        std::size_t slot=next;for(std::size_t i=0;i<guid.size();++i)if(guid[i]==g){slot=i;break;}
        if(slot==next)next=(next+1u)%unsigned(guid.size());
        guid[slot]=g;ns[slot]=now;lastAny=now;return true;
    }
};

// ============================== GRAB (tick / substeps) ==============================
// A point in a matrix's frame (rows a b c orthonormal, origin d; 16 floats as the engine lays a Matrix34 out).
inline V3 toLocal(const float* m16,V3 p) noexcept {
    const V3 d=v::sub(p,V3{m16[12],m16[13],m16[14]});
    return {v::dot(d,V3{m16[0],m16[1],m16[2]}),v::dot(d,V3{m16[4],m16[5],m16[6]}),v::dot(d,V3{m16[8],m16[9],m16[10]})};}
// The distance from a point to a box (0 inside).
inline float boxDistance(V3 p,V3 bmin,V3 bmax) noexcept {
    float s=0.f;for(std::size_t k=0;k<3;++k){const float d=std::max({bmin[k]-p[k],0.f,p[k]-bmax[k]});s+=d*d;}return std::sqrt(s);}
// The nearest part of a bound to the palm: parts = their matrices in the instance's frame and their boxes (a single bound = part 0
// with the identity). local = the palm in the instance's frame. index -1 = none finite.
struct GrabPart {float m[16]{};V3 bmin{},bmax{};};
struct NearPart {int index{-1};float dist{1e30f};std::uint8_t never{};std::int8_t pt{-1};}; // pt: the grab point that matched (0 palm, 1 / 2 fingers)
inline NearPart nearestPart(const GrabPart* parts,unsigned count,V3 local) noexcept {
    NearPart n{};if(!v::finite(local))return n;
    for(unsigned i=0;i<count;++i){const auto& p=parts[i];
        if(!v::finite(p.bmin)||!v::finite(p.bmax)||p.bmax[0]<p.bmin[0]||p.bmax[1]<p.bmin[1]||p.bmax[2]<p.bmin[2])continue;
        const float d=boxDistance(toLocal(p.m,local),p.bmin,p.bmax);if(std::isfinite(d)&&d<n.dist){n.dist=d;n.index=int(i);}}
    return n;
}
// The hand's grab points (world units): the palm centre B and two points along the fingers (B + F 6 cm, B + F 12 cm), so a
// fingers-first touch with the palm centre 9..13 cm off the surface still finds the part within grabcm.
inline std::array<V3,3> grabPoints(V3 b,const Rows& rows,float upm) noexcept {
    V3 f=v::normalized(rows[1]);if(v::length(f)<.5f)f={0,0,0};
    return {b,v::add(b,v::mul(f,.06f*upm)),v::add(b,v::mul(f,.12f*upm))};}
// GRAB SEAT. startGrab welds a fixed-rotation prop where it is (AttachObjects at the palm centre B for both instances, then
// SetFixedRotation 0x1c7ba58, which freezes the relative pose of that moment), and a candidate is any part within grabcm of B, B + 6 cm or
// B + 12 cm along the fingers: a fingertip grab holds a cup with its surface up to ~16 cm past the palm and swings it round the wrist.
// The seat: the matched part's point nearest B (part box in its own frame, the part in the instance's, the instance in the world's),
// pulled to B by the prop collider's velocity (the hand's plus (B - seat) / tau, tau = seatDoneM / creepMps = 50 ms, the relative part
// at most capMps: the arrival speed is about the creep speed, no jolt); attached once within seatDoneM (the joint then starts with no
// error) or at the timeout (blocked by a table: attached where it is, the old hold).
// The hold's release thresholds (grabStep below; the seat's release takes the same): a grip under grabOpenSqueeze opens, a hand at
// grabThrowMps or more throws.
inline constexpr float grabOpenSqueeze=.30f,grabThrowMps=1.2f;inline constexpr std::uint64_t grabLeashNs=150'000'000ull;
inline constexpr float seatDoneM=.005f;
inline V3 fromLocal(const float* m16,V3 l) noexcept {
    return {m16[12]+m16[0]*l[0]+m16[4]*l[1]+m16[8]*l[2],m16[13]+m16[1]*l[0]+m16[5]*l[1]+m16[9]*l[2],m16[14]+m16[2]*l[0]+m16[6]*l[1]+m16[10]*l[2]};}
struct SeatPoint {V3 world{};float dist{-1.f};bool ok{};};
inline SeatPoint seatPoint(const GrabPart& part,const float* instM16,V3 b) noexcept {
    SeatPoint r{};if(!v::finite(b)||!v::finite(part.bmin)||!v::finite(part.bmax))return r;
    for(int k=0;k<3;++k)if(part.bmax[std::size_t(k)]<part.bmin[std::size_t(k)])return r;
    const V3 li=toLocal(instM16,b),lp=toLocal(part.m,li);if(!v::finite(lp))return r;
    V3 q{};for(std::size_t k=0;k<3;++k)q[k]=std::clamp(lp[k],part.bmin[k],part.bmax[k]);
    r.world=fromLocal(instM16,fromLocal(part.m,q));r.dist=v::length(v::sub(b,r.world));r.ok=v::finite(r.world)&&std::isfinite(r.dist);return r;}
inline float seatTauS(float creepMps) noexcept {return std::isfinite(creepMps)&&creepMps>1e-3f?seatDoneM/creepMps:.05f;}
// The seated distance: seatDoneM plus the sag gravity holds against the pull (the step adds g dt to the velocity written before it, the
// pull answers gap / tau: a gap of g dt tau, 6.8 mm at 1/72 s substeps), at most 2 cm.
inline float seatDoneFor(float substepS,float tauS) noexcept {
    const float dt=std::isfinite(substepS)&&substepS>0.f?std::min(substepS,.05f):0.f,tau=std::isfinite(tauS)&&tauS>0.f?tauS:.05f;
    return std::min(seatDoneM+9.8f*dt*tau,.02f);}
// The prop's velocity this substep: the hand's plus the gap over tau, the gap's part capped at capSpeed (world units / s).
inline V3 seatPullVel(V3 seatW,V3 b,V3 vHand,float capSpeed,float tauS) noexcept {
    const V3 vh=v::finite(vHand)?vHand:V3{0,0,0};if(!v::finite(seatW)||!v::finite(b))return vh;
    const float tau=std::isfinite(tauS)&&tauS>1e-3f?tauS:.05f;V3 rel=v::mul(v::sub(b,seatW),1.f/tau);
    const float l=v::length(rel),cap=std::isfinite(capSpeed)&&capSpeed>0.f?capSpeed:0.f;if(l>cap&&l>0.f)rel=v::mul(rel,cap/l);
    return v::add(vh,rel);}
// The seat's verdict each tick (in this order): the mode / lever / hand gone, the prop gone (state < 0 or another collider), inactive;
// a grip under openSqueeze takes the hold's own release rule (grabStep): at handMps >= grabThrowMps the throw (attached now, the hold's
// gate throws it on the next tick), under openSqueeze for openNs (openAgeNs = time since the grip went under, the dip debounce) the
// cancel, a shorter dip keeps pulling and never attaches; then seated (within doneM), the timeout, else keep pulling.
enum SeatEnd : std::uint8_t {SeatKeep=0,SeatSeated,SeatTimeout,SeatThrow,SeatCancelOpen,SeatCancelGone,SeatCancelInactive,SeatCancelMode,SeatEndCount};
inline SeatEnd seatStep(bool modeOk,float squeeze,float openSqueeze,int state,bool colSame,float distM,float doneM,std::uint64_t ageNs,std::uint64_t timeoutNs,
                        std::uint64_t openAgeNs=0,std::uint64_t openNs=0,float handMps=0.f) noexcept {
    if(!modeOk)return SeatCancelMode;
    if(state<0||(state==0&&!colSame))return SeatCancelGone;
    if(state!=0)return SeatCancelInactive;
    if(!(std::isfinite(squeeze)&&squeeze>=openSqueeze)){
        if(std::isfinite(handMps)&&handMps>=grabThrowMps)return SeatThrow;
        return openAgeNs>=openNs?SeatCancelOpen:SeatKeep;}
    if(std::isfinite(distM)&&distM<=(std::isfinite(doneM)&&doneM>0.f?doneM:seatDoneM))return SeatSeated;
    if(ageNs>=timeoutNs)return SeatTimeout;
    return SeatKeep;}
inline unsigned seatMsBucket(std::uint64_t ns) noexcept {const double m=double(ns)/1e6;return m<50.?0u:m<100.?1u:m<200.?2u:m<350.?3u:4u;}
inline unsigned seatCmBucket(float cm) noexcept {return !(cm<15.f)?4u:cm<2.f?0u:cm<5.f?1u:cm<10.f?2u:3u;}
// A grab miss's distance bucket (metres): 0-2, 2-4, 4-6, 6-10, 10-20, 20+ cm.
inline unsigned missDistBucket(float m) noexcept {return !(m<.2f)?5u:m<.02f?0u:m<.04f?1u:m<.06f?2u:m<.10f?3u:4u;}
// The held hand's drive: the velocity to the command reached with at most maxDv of change per substep (force * dt / mass).
inline V3 grabDrive(V3 vcur,V3 vcmd,float maxDv) noexcept {
    if(!v::finite(vcur))vcur={0,0,0};if(!v::finite(vcmd))return vcur;
    V3 dv=v::sub(vcmd,vcur);const float l=v::length(dv);if(l>maxDv&&l>0.f)dv=v::mul(dv,maxDv/l);return v::add(vcur,dv);}
// Release of a hold: the grip under .30 for openMs (Quest grip dips last 0.1..0.3 s), at once when the hand moves 1.2 m/s or more relative
// to the player (a throw); never while the trigger is pressed (a heavy hold: the trigger plays no part); a break when the body stays over leashM (the break distance) from its
// command for breakNs. A heavy hold with heavyBreakM > 0 (lever grabheavyleash off) takes heavyBreakM as its break distance instead and
// ends there as GrabSafety: walking or swinging with the prop trailing the hands never ends the hold, only an open hand does.
enum GrabRelease : std::uint8_t {GrabKeep=0,GrabOpen,GrabThrow,GrabLeash,GrabSafety};
// The drawn hand of a hold stays within grabVisualM of the controller (the break distance grabLeashM is longer: a snag never draws the
// hand torn off).
inline constexpr float grabVisualM=.25f;
// A heavy hold (heavy = true: a ball-jointed prop over grabLiftKg, or either hand of a pair) never throws (grabThrow adds nothing to a
// ball-jointed prop) and opens only once the grip spent openMs under .30 since it was last at grabRegrip or more: between .30 and
// grabRegrip the time neither counts nor resets (a loose grip of a heavy chair is no release; a full dip under .30 of up to openMs is).
inline constexpr float grabRegrip=.50f;
struct GrabHoldGate {std::uint64_t openNs{},leashNs{},heavyLastNs{},heavyOpenNs{};};
// LIGHT GHOST: per hand, the light props whose contact pairs the collide filter drops. A ghost starts on a pair that would collide when
// the hand is closing (squeeze >= grabOpenSqueeze, not the fist part, and rising: at least ghostRiseMin over its 150 ms lag, so fingers
// resting on the grip push and knock props as an open hand does) and the other is a nav prop (not a door) whose active collider weighs
// more than 0 and at most rotKg; or on the prop of a fixed-rotation hold that ended (why release). Once on, the start conditions are not
// checked again (the hand becomes a fist a few substeps after it closes); every filter call of that pair refreshes seenNs.
enum GhostWhy : std::uint8_t {GhostIntent,GhostRelease,GhostSeat,GhostWhyCount};
enum GhostEnd : std::uint8_t {GhostKeep,GhostQuiet,GhostHard,GhostLever,GhostLevel,GhostEndCount};
struct LightGhost {std::uintptr_t inst{};std::uint64_t startNs{},seenNs{};std::uint8_t why{};};
inline bool lightGhostIntent(float squeeze,bool fist,bool lever) noexcept {return lever&&std::isfinite(squeeze)&&squeeze>=grabOpenSqueeze&&!fist;}
// The squeeze's lag (first order, tau ghostLagNs; < 0 = no history yet), stepped once per tick.
inline constexpr std::uint64_t ghostLagNs=150'000'000ull;inline constexpr float ghostRiseMin=.05f;
inline float squeezeLagStep(float lag,float squeeze,std::uint64_t dtNs) noexcept {
    if(!std::isfinite(squeeze))return lag;
    if(!(lag>=0.f)||!std::isfinite(lag))return squeeze;
    const float a=1.f-std::exp(-float(double(std::min<std::uint64_t>(dtNs,1'000'000'000ull))/double(ghostLagNs)));
    return lag+(squeeze-lag)*a;}
// A hand closing on something: in the closing band and the squeeze rising over its lag.
inline bool lightGhostClosing(float squeeze,float lag,bool fist,bool lever) noexcept {
    return lightGhostIntent(squeeze,fist,lever)&&lag>=0.f&&std::isfinite(lag)&&squeeze-lag>=ghostRiseMin;}
// A light prop for the ghost and the skipped first-contact push: active collider mass in (0, rotKg].
inline bool lightGhostKg(float colKg,float rotKg) noexcept {return std::isfinite(colKg)&&colKg>0.f&&std::isfinite(rotKg)&&colKg<=rotKg;}
inline bool lightGhostStart(bool closing,bool nav,bool door,float colKg,float rotKg) noexcept {return closing&&nav&&!door&&lightGhostKg(colKg,rotKg);}
// 0 keep, else why it ends: no overlap within quietNs, past hardNs.
inline GhostEnd lightGhostKeep(const LightGhost& g,std::uint64_t now,std::uint64_t quietNs,std::uint64_t hardNs) noexcept {
    if(!g.inst||!g.startNs||now<g.startNs)return GhostQuiet;
    if(now-g.startNs>=hardNs)return GhostHard;
    if(!g.seenNs||(now>=g.seenNs&&now-g.seenNs>=quietNs))return GhostQuiet;
    return GhostKeep;}
// Arms a ghost on `inst` in the hand's slots (an existing one is refreshed; a full set gives up the one seen longest ago). Returns the slot.
template<std::size_t N>
inline LightGhost* lightGhostArm(std::array<LightGhost,N>& slots,std::uintptr_t inst,std::uint64_t now,std::uint8_t why) noexcept {
    if(!inst)return nullptr;LightGhost* pick=nullptr;
    for(auto& g:slots)if(g.inst==inst){g.seenNs=now;return &g;}
    for(auto& g:slots)if(!g.inst){pick=&g;break;}
    if(!pick){pick=&slots[0];for(auto& g:slots)if(g.seenNs<pick->seenNs)pick=&g;}
    *pick=LightGhost{inst,now?now:1,now?now:1,why};return pick;}
// The collide filter's step for (hand, other): true = drop the pair. start = this pair may start an intent ghost (lightGhostStart);
// started (optional) = this call armed it.
template<std::size_t N>
inline bool lightGhostFilter(std::array<LightGhost,N>& slots,std::uintptr_t other,bool start,std::uint64_t now,bool* started=nullptr) noexcept {
    if(started)*started=false;if(!other)return false;
    for(auto& g:slots)if(g.inst==other){g.seenNs=now;return true;}
    if(!start)return false;
    if(lightGhostArm(slots,other,now,GhostIntent)){if(started)*started=true;return true;}
    return false;}
inline GrabRelease grabStep(GrabHoldGate& g,float squeeze,float trigger,float handMps,float lagM,float leashM,float openMs,std::uint64_t now,
                            std::uint64_t breakNs=grabLeashNs,bool heavy=false,float heavyBreakM=0.f) noexcept {
    const bool safety=heavy&&heavyBreakM>0.f;const float breakM=safety?heavyBreakM:leashM;
    if(std::isfinite(lagM)&&lagM>breakM){if(!g.leashNs)g.leashNs=now?now:1;if(now>=g.leashNs&&now-g.leashNs>=breakNs)return safety?GrabSafety:GrabLeash;}else g.leashNs=0;
    if(!heavy&&std::isfinite(trigger)&&trigger>triggerBusy){g.openNs=0;return GrabKeep;}
    if(heavy){
        const float sq=std::isfinite(squeeze)?squeeze:1.f;
        if(sq>=grabRegrip){g.heavyOpenNs=0;g.heavyLastNs=now;return GrabKeep;}
        if(sq<grabOpenSqueeze&&g.heavyLastNs&&now>g.heavyLastNs)g.heavyOpenNs+=std::min<std::uint64_t>(now-g.heavyLastNs,100'000'000ull);
        g.heavyLastNs=now?now:1;
        return double(g.heavyOpenNs)>=double(openMs)*1e6?GrabOpen:GrabKeep;}
    if(!std::isfinite(squeeze)||squeeze>=grabOpenSqueeze){g.openNs=0;return GrabKeep;}
    if(std::isfinite(handMps)&&handMps>=grabThrowMps)return GrabThrow;
    if(!g.openNs)g.openNs=now?now:1;
    return now>=g.openNs&&double(now-g.openNs)>=double(openMs)*1e6?GrabOpen:GrabKeep;
}
// The force on the held prop at the grip each substep (world units): gain * M * (vWant - vProp) / dt toward the hand's command
// velocity, its part across up capped at min(forceMax, M * accel), upward at min(M, liftKg) * (g + accel) (a heavier prop tilts and
// drags on its own weight), downward at M * accel. capped = a cap cut it.
struct GrabForce {V3 f{};bool capped{};};
// weightKg > 0 (a fixed-rotation hold, lever grablightff): + weightKg g up before the caps (the prop's weight carried: without it the
// velocity law settles at g dt / gain under the hand, 0.27 m/s at gain 0.5 and 72 substeps per second).
inline GrabForce grabPropForce(V3 vWant,V3 vProp,float massKg,float dtS,V3 up,float gain,float forceMaxN,float accel,float liftKg,float upm,float weightKg=0.f) noexcept {
    GrabForce g{};const float u=std::max(upm,1e-3f);
    if(!v::finite(vWant)||!v::finite(vProp)||!(massKg>0.f)||!std::isfinite(massKg)||!(dtS>1e-5f)||!std::isfinite(dtS))return g;
    const V3 un=v::normalized(up);if(v::length(un)<.5f)return g;
    V3 f=v::mul(v::sub(vWant,vProp),std::max(gain,0.f)*massKg/dtS);
    if(std::isfinite(weightKg)&&weightKg>0.f)f=v::add(f,v::mul(un,std::min(weightKg,massKg)*9.81f*u));
    const float fu=v::dot(f,un);V3 fh=v::sub(f,v::mul(un,fu));
    const float hMax=std::min(forceMaxN,massKg*accel)*u,hl=v::length(fh);
    if(hl>hMax){fh=hl>0.f?v::mul(fh,hMax/hl):V3{0,0,0};g.capped=true;}
    const float upMax=std::min(massKg,std::max(liftKg,0.f))*(9.81f+accel)*u,downMax=massKg*accel*u;
    float fv=fu;if(fv>upMax){fv=upMax;g.capped=true;}if(fv<-downMax){fv=-downMax;g.capped=true;}
    g.f=v::add(fh,v::mul(un,fv));if(!v::finite(g.f))g=GrabForce{};
    return g;
}
// A ball-jointed hold's force at the grip (world units): a critically damped spring on the grip point, F = M (wn^2 (cmd - grip) +
// 2 zeta wn (vCmd - vGrip)) + the weight it carries (min(M, liftKg) g up), scaled by the stall; across up capped at min(forceMax, M accel),
// up at liftKg's (g + accel) for a prop of liftKg or less; a heavier one tilts and is never lifted: up at min(liftKg (g + accel),
// max(liftKg g, M g / 2)), under its weight (gripUpMaxN); down at downG M g; its change per substep capped at slewN (prevN = the last
// force, N). The damping reads
// the grip's own velocity (the centre of mass's would leave the prop's swing about it undamped: the velocity law on the centre of mass
// swings a prop held at its edge every 3-4 substeps). vert: 0 linear, 1 at the up cap, 2 at the down cap.
struct GripForce {V3 f{},fN{};bool capped{},slewed{};std::uint8_t vert{};float slewDeficitN{};};
inline float gripUpMaxN(float massKg,float liftKg,float accel) noexcept {
    const float lift=std::max(liftKg,0.f),a=std::max(accel,0.f);
    if(!(massKg>lift))return std::max(massKg,0.f)*(9.81f+a);
    return std::min(lift*(9.81f+a),std::max(lift*9.81f,.5f*massKg*9.81f));}
inline GripForce grabGripForce(V3 cmd,V3 grip,V3 vCmd,V3 vGrip,float massKg,V3 up,float wn,float zeta,float forceMaxN,float accel,float liftKg,float downG,
                               V3 prevN,float slewN,float scale,float upm) noexcept {
    GripForce g{};const float u=std::max(upm,1e-3f);
    if(!v::finite(cmd)||!v::finite(grip)||!v::finite(vCmd)||!v::finite(vGrip)||!(massKg>0.f)||!std::isfinite(massKg))return g;
    const V3 un=v::normalized(up);if(v::length(un)<.5f)return g;
    const float k=std::max(scale,0.f),carry=std::min(massKg,std::max(liftKg,0.f));
    const V3 e=v::mul(v::sub(cmd,grip),1.f/u),dv=v::mul(v::sub(vCmd,vGrip),1.f/u);
    V3 f=v::add(v::mul(v::add(v::mul(e,wn*wn),v::mul(dv,2.f*zeta*wn)),massKg*k),v::mul(un,carry*9.81f));
    const float fu=v::dot(f,un);V3 fh=v::sub(f,v::mul(un,fu));
    const float hMax=std::min(forceMaxN,massKg*accel),hl=v::length(fh);
    if(hl>hMax){fh=hl>0.f?v::mul(fh,hMax/hl):V3{0,0,0};g.capped=true;}
    const float upMax=gripUpMaxN(massKg,liftKg,accel),downMax=std::max(downG,0.f)*massKg*9.81f;
    float fv=fu;if(fv>upMax){fv=upMax;g.capped=true;g.vert=1;}if(fv<-downMax){fv=-downMax;g.capped=true;g.vert=2;}
    f=v::add(fh,v::mul(un,fv));
    if(v::finite(prevN)&&slewN>0.f){const V3 d=v::sub(f,prevN);const float l=v::length(d);if(l>slewN){f=v::add(prevN,v::mul(d,slewN/l));g.slewed=true;}}
    if(!v::finite(f))return GripForce{};
    g.fN=f;g.f=v::mul(f,u);return g;
}
// TWO HANDS on one ball-jointed prop: each hand's spring drives half the mass (the two springs add up to one critically damped spring for
// the translation), each carries its share of the weight (pairShare: where the centre of mass lies along the grip line), up to
// min(M, lift2Kg) for the pair; per hand: up share * min(M, lift2Kg) * (g + accel), across 0.5 M accel, down 0.5 downG M g, each within
// forceMax.
// minShare (lever grabpairfull: 0.5): each hand's up cap is taken at max(share, minShare) of the pair's lift (the carried weight stays at
// the share, so the weight's torque about the grip line stays balanced); at 59.5 kg 887 N per hand against the v9 share-clamped 266 N.
// upByForce = forceMax bound the up cap.
// forceMaxN = the per-hand total cap; upSpringN = the spring's own up cap under the statics carry (0.5 lift accel: the carry holds the weight).
// downAccel > 0 (a pair driven at its drive mass, grabPairKg): the down cap is at least 0.5 M downAccel (a swing down is as quick as one
// across; 0.5 downG M g alone is 37 N at 15 kg).
struct PairLimits {float springKg{},carryKg{},upMaxN{},sideMaxN{},downMaxN{};bool upByForce{};float forceMaxN{},upSpringN{};};
inline PairLimits pairLimits(float massKg,float share,float lift2Kg,float forceMaxN,float accel,float downG,float minShare=0.f,float downAccel=0.f) noexcept {
    PairLimits l{};if(!(massKg>0.f)||!std::isfinite(massKg))return l;
    const float sh=std::clamp(std::isfinite(share)?share:.5f,0.f,1.f),lift=std::min(massKg,std::max(lift2Kg,0.f)),a=std::max(accel,0.f),fm=std::max(forceMaxN,0.f);
    const float upShare=std::max(sh,std::clamp(std::isfinite(minShare)?minShare:0.f,0.f,1.f));
    const float upWant=upShare*lift*(9.81f+a);
    l.springKg=.5f*massKg;l.carryKg=sh*lift;l.upMaxN=std::min(fm,upWant);l.upByForce=fm<upWant;l.sideMaxN=std::min(fm,.5f*massKg*a);l.downMaxN=.5f*std::max(downG,0.f)*massKg*9.81f;
    if(std::isfinite(downAccel)&&downAccel>0.f)l.downMaxN=std::min(fm,std::max(l.downMaxN,.5f*massKg*downAccel));
    l.forceMaxN=fm;l.upSpringN=std::min(fm,.5f*lift*a);
    return l;}
// TWO-POINT STATICS (lever grabpairstatics): each grip's carry so that the pair holds the carried weight Wc = min(M, lift2Kg) g up with no
// torque about the centre of mass across the grip line (the part along the line is left free: the prop hangs and rolls under it).
// rA, rB = the grips from the centre of mass, d = A - B, tau0 = ((rA + rB) / 2) x Wc (the even split's torque); the couple f = (t x d) / |d|^2
// with t = -(tau0 - d^ (d^ . tau0)) is the smallest force with d x f = t; carryA = Wc / 2 + f, carryB = Wc / 2 - f (N). A level line: the
// horizontal lever law; a centre of mass beyond a grip: the far hand pushes down; a steep line: a horizontal couple. |f| is kept within
// forceMax - |Wc| / 2 (the hand's total cap); under 10 cm of grip spacing f = 0 (the even split). Positions in world units, forces in N.
struct PairCarry {V3 carry[2]{};float coupleN{},wN{};bool clamped{},shortLine{};};
inline PairCarry pairCarry(V3 com,V3 gripA,V3 gripB,V3 up,float massKg,float lift2Kg,float forceMaxN,float upm) noexcept {
    PairCarry c{};const V3 un=v::normalized(up);
    if(!(massKg>0.f)||!std::isfinite(massKg)||v::length(un)<.5f||!v::finite(com)||!v::finite(gripA)||!v::finite(gripB))return c;
    const float lift=std::min(massKg,std::max(std::isfinite(lift2Kg)?lift2Kg:0.f,0.f));c.wN=lift*9.81f;
    const V3 W=v::mul(un,c.wN),half=v::mul(W,.5f);c.carry[0]=half;c.carry[1]=half;
    const float u=std::max(upm,1e-3f);
    const V3 d=v::mul(v::sub(gripA,gripB),1.f/u);const float d2=v::dot(d,d);
    if(!(d2>=.01f)||!std::isfinite(d2)){c.shortLine=true;return c;}
    const V3 rM=v::mul(v::sub(v::mul(v::add(gripA,gripB),.5f),com),1.f/u);
    const V3 tau0=v::cross(rM,W);const V3 dh=v::mul(d,1.f/std::sqrt(d2));
    const V3 t=v::mul(v::sub(tau0,v::mul(dh,v::dot(dh,tau0))),-1.f);
    V3 f=v::mul(v::cross(t,d),1.f/d2);
    const float fl=v::length(f),fMax=std::max(0.f,(std::isfinite(forceMaxN)?forceMaxN:0.f)-.5f*c.wN);
    if(fl>fMax){f=fl>0.f?v::mul(f,fMax/fl):V3{0,0,0};c.clamped=true;}
    if(!v::finite(f))return c;
    c.carry[0]=v::add(half,f);c.carry[1]=v::sub(half,f);c.coupleN=v::length(f);return c;}
// The share of the weight hand A carries by the horizontal lever law (the statics on a level line; diagnostics): where the centre of
// mass lies between the grips across up, kept within -1 .. 2.
inline float pairShareH(V3 com,V3 gripA,V3 gripB,V3 up) noexcept {
    const V3 un=v::normalized(up);const auto flat=[&](V3 x){return v::sub(x,v::mul(un,v::dot(x,un)));};
    const V3 ab=flat(v::sub(gripA,gripB));const float d2=v::dot(ab,ab);
    if(!(d2>1e-8f)||!std::isfinite(d2))return .5f;
    const float s=v::dot(flat(v::sub(com,gripB)),ab)/d2;return std::isfinite(s)?std::clamp(s,-1.f,2.f):.5f;}
// Hand A's share of the weight: the centre of mass projected on the grip line B -> A (0 at B, 1 at A), kept within .15 .. .85.
inline float pairShare(V3 com,V3 gripA,V3 gripB) noexcept {
    const V3 ab=v::sub(gripA,gripB);const float d2=v::dot(ab,ab);
    if(!(d2>1e-8f)||!std::isfinite(d2))return .5f;
    const float s=v::dot(v::sub(com,gripB),ab)/d2;return std::isfinite(s)?std::clamp(s,.15f,.85f):.5f;}
// The pair's targets: the grips' spacing d (fixed on the prop) laid along the line between the two commands, centred on their middle, so
// the prop's grip line follows the hands (aimed and swung like a club) and pulling the hands apart stretches nothing. The target velocities:
// the middle's plus each command's velocity across that line (the stretch rate dropped). Under 10 cm of spacing the grips keep their
// offsets from the middle (no aim).
struct PairPlan {V3 target[2]{},vTarget[2]{};float d{},stretch{};bool aimed{};};
inline PairPlan pairTargets(V3 cmdA,V3 cmdB,V3 vA,V3 vB,V3 gripA,V3 gripB,float upm) noexcept {
    PairPlan p{};const float u=std::max(upm,1e-3f);
    const V3 mid=v::mul(v::add(cmdA,cmdB),.5f),vMid=v::mul(v::add(vA,vB),.5f);
    const V3 gm=v::mul(v::add(gripA,gripB),.5f);p.d=v::length(v::sub(gripB,gripA));
    V3 dir=v::normalized(v::sub(cmdB,cmdA));p.stretch=(v::length(v::sub(cmdB,cmdA))-p.d)/u;
    if(p.d<.10f*u||v::length(dir)<.5f){p.target[0]=v::add(mid,v::sub(gripA,gm));p.target[1]=v::add(mid,v::sub(gripB,gm));p.vTarget[0]=vA;p.vTarget[1]=vB;return p;}
    p.aimed=true;
    p.target[0]=v::sub(mid,v::mul(dir,.5f*p.d));p.target[1]=v::add(mid,v::mul(dir,.5f*p.d));
    const auto across=[&](V3 w){const V3 r=v::sub(w,vMid);return v::add(vMid,v::sub(r,v::mul(dir,v::dot(r,dir))));};
    p.vTarget[0]=across(vA);p.vTarget[1]=across(vB);
    return p;}
// One hand of a pair: grabGripForce's spring and caps with the pair's mass, carried weight and limits (N).
// carryN (lever grabpairstatics): this hand's statics carry (pairCarry, N); the caps then bound the spring part only (across sideMaxN, up
// upSpringN, down downMaxN), and |carry + spring| <= forceMaxN shrinks the spring along itself (the carry keeps priority). guardN (lever
// grabplayerguard): the unit normal from the prop into the player's capsule; the position part of the spring's push along it is removed
// (trimN = how much; the damping part stays, so the prop is still braked). ffN (lever grabfeedfwd): the command's acceleration feed-forward
// (0.5 M aCmd, N), added to the spring part before its caps.
inline GripForce grabGripForcePair(V3 cmd,V3 grip,V3 vCmd,V3 vGrip,const PairLimits& L,V3 up,float wn,float zeta,V3 prevN,float slewN,float scale,float upm,
                                   const V3* carryN=nullptr,const V3* guardN=nullptr,float* trimN=nullptr,V3 ffN={0,0,0}) noexcept {
    GripForce g{};const float u=std::max(upm,1e-3f);if(trimN)*trimN=0.f;
    if(!v::finite(cmd)||!v::finite(grip)||!v::finite(vCmd)||!v::finite(vGrip)||!(L.springKg>0.f)||!std::isfinite(L.springKg))return g;
    const V3 un=v::normalized(up);if(v::length(un)<.5f)return g;
    const float k=std::max(scale,0.f);
    const V3 e=v::mul(v::sub(cmd,grip),1.f/u),dv=v::mul(v::sub(vCmd,vGrip),1.f/u);
    const V3 ff=v::finite(ffN)?ffN:V3{0,0,0};
    if(carryN&&v::finite(*carryN)){
        V3 sp=v::add(v::mul(v::add(v::mul(e,wn*wn),v::mul(dv,2.f*zeta*wn)),L.springKg*k),ff);
        const float su=v::dot(sp,un);V3 sh=v::sub(sp,v::mul(un,su));const float shl=v::length(sh);
        if(shl>L.sideMaxN){sh=shl>0.f?v::mul(sh,std::max(L.sideMaxN,0.f)/shl):V3{0,0,0};g.capped=true;}
        float sv=su;if(sv>L.upSpringN){sv=L.upSpringN;g.capped=true;g.vert=1;}if(sv<-L.downMaxN){sv=-L.downMaxN;g.capped=true;g.vert=2;}
        sp=v::add(sh,v::mul(un,sv));
        if(guardN&&v::finite(*guardN)&&v::length(*guardN)>.5f){const V3 n=v::normalized(*guardN);const float in=v::dot(sp,n);
            const float pos=v::dot(e,n)*wn*wn*L.springKg*k,cut=std::min(in,pos);
            if(in>0.f&&cut>0.f){sp=v::sub(sp,v::mul(n,cut));if(trimN)*trimN=cut;}}
        // |c + s sp| <= fm: the largest s in [0, 1] (the carry alone within fm: pairCarry's clamp)
        const V3 c=*carryN;const float fm=std::max(L.forceMaxN,0.f);V3 f=v::add(c,sp);
        if(v::length(f)>fm){const float a=v::dot(sp,sp),b=2.f*v::dot(c,sp),cc=v::dot(c,c)-fm*fm;float s=0.f;
            if(a>1e-9f&&cc<0.f){const float disc=b*b-4.f*a*cc;s=std::clamp((-b+std::sqrt(std::max(disc,0.f)))/(2.f*a),0.f,1.f);}
            f=v::add(c,v::mul(sp,s));g.capped=true;
            if(cc>=0.f){const float cl=v::length(c);f=cl>fm&&cl>0.f?v::mul(c,fm/cl):c;}}
        if(v::finite(prevN)&&slewN>0.f){const V3 d=v::sub(f,prevN);const float l=v::length(d);if(l>slewN){f=v::add(prevN,v::mul(d,slewN/l));g.slewed=true;g.slewDeficitN=l-slewN;}}
        if(!v::finite(f))return GripForce{};
        g.fN=f;g.f=v::mul(f,u);return g;}
    V3 f=v::add(v::add(v::mul(v::add(v::mul(e,wn*wn),v::mul(dv,2.f*zeta*wn)),L.springKg*k),ff),v::mul(un,std::max(L.carryKg,0.f)*9.81f));
    const float fu=v::dot(f,un);V3 fh=v::sub(f,v::mul(un,fu));
    const float hl=v::length(fh);
    if(hl>L.sideMaxN){fh=hl>0.f?v::mul(fh,std::max(L.sideMaxN,0.f)/hl):V3{0,0,0};g.capped=true;}
    float fv=fu;if(fv>L.upMaxN){fv=L.upMaxN;g.capped=true;g.vert=1;}if(fv<-L.downMaxN){fv=-L.downMaxN;g.capped=true;g.vert=2;}
    f=v::add(fh,v::mul(un,fv));
    if(v::finite(prevN)&&slewN>0.f){const V3 d=v::sub(f,prevN);const float l=v::length(d);if(l>slewN){f=v::add(prevN,v::mul(d,slewN/l));g.slewed=true;g.slewDeficitN=l-slewN;}}
    if(!v::finite(f))return GripForce{};
    g.fN=f;g.f=v::mul(f,u);return g;
}
// A pair's slew per substep (N): at least slewN, else what the spring's damping asks for the hand accelerating at accel over dt
// (0.5 M x 2 zeta wn x accel x dt: 132 N at 59.5 kg, wn 8, zeta 1, 20 m/s^2, 13.9 ms; the fixed 40 N held the force back on every substep
// of the v9 chair holds).
inline float pairSlewN(float massKg,float zeta,float wn,float accel,float dtS,float slewN) noexcept {
    const float s=.5f*(std::isfinite(massKg)?std::max(massKg,0.f):0.f)*2.f*std::max(zeta,0.f)*std::max(wn,0.f)*std::max(accel,0.f)*(std::isfinite(dtS)?std::clamp(dtS,0.f,.05f):0.f);
    return std::max(slewN,std::isfinite(s)?s:0.f);}
// A stalled hold (the grip 15 cm or more from the command and not closing at 5 cm/s for 300 ms: the prop is against something): the grip
// force ramps to half over a second, back to full in half a second once it moves again.
struct GripStall {float since{},scale{1.f};};
inline float gripStallStep(GripStall& s,float lagM,float closingMps,float dtS) noexcept {
    const float dt=std::clamp(std::isfinite(dtS)?dtS:0.f,0.f,.1f);
    const bool stuck=std::isfinite(lagM)&&lagM>.15f&&!(std::isfinite(closingMps)&&closingMps>=.05f);
    s.since=stuck?s.since+dt:0.f;
    s.scale=s.since>=.3f?std::max(.5f,s.scale-.5f*dt):std::min(1.f,s.scale+dt);
    return s.scale;
}
// The hand body of a ball-jointed hold moves with the grip (its velocity the grip's, plus a fifth of the gap per substep): the joint then
// carries no load from the hand, the grip force alone moves the prop.
inline V3 gripFollowVel(V3 vGrip,V3 grip,V3 x,float dtS) noexcept {
    if(!v::finite(vGrip)||!v::finite(grip)||!v::finite(x)||!(dtS>1e-5f))return vGrip;
    const V3 r=v::add(vGrip,v::mul(v::sub(grip,x),.2f/dtS));return v::finite(r)?r:vGrip;}
// A pair's command acceleration (lever grabfeedfwd): d(vCmd)/dt per substep, averaged over tauS (an exponential average: the hand's
// velocity changes in steps at each frame), at most capMps2 (world units: upm per metre). A gap over 100 ms or a non-finite input restarts it.
struct CmdAccel {V3 vPrev{},a{};bool have{};};
inline V3 cmdAccelStep(CmdAccel& c,V3 vCmd,float dtS,float tauS,float capMps2,float upm) noexcept {
    const float u=std::max(upm,1e-3f);
    if(!v::finite(vCmd)||!(dtS>1e-5f)||!std::isfinite(dtS)||dtS>.1f){c=CmdAccel{};if(v::finite(vCmd)){c.vPrev=vCmd;c.have=true;}return {0,0,0};}
    if(!c.have){c.vPrev=vCmd;c.have=true;c.a={0,0,0};return c.a;}
    const V3 raw=v::mul(v::sub(vCmd,c.vPrev),1.f/dtS);c.vPrev=vCmd;
    const float k=std::clamp(dtS/std::max(tauS,dtS),0.f,1.f);
    V3 a=v::add(c.a,v::mul(v::sub(raw,c.a),k));
    const float l=v::length(a),cap=std::max(capMps2,0.f)*u;if(l>cap)a=l>0.f?v::mul(a,cap/l):V3{0,0,0};
    c.a=v::finite(a)?a:V3{0,0,0};return c.a;}
// The feed-forward force of one hand of a pair (N): 0.5 M a, a in world units per s^2.
inline V3 pairFeedFwdN(float driveKg,V3 aCmd,float upm) noexcept {
    const V3 f=v::mul(aCmd,.5f*(std::isfinite(driveKg)?std::max(driveKg,0.f):0.f)/std::max(upm,1e-3f));return v::finite(f)?f:V3{0,0,0};}
// The drive mass of a pair (lever grabpairkg): the lighter of the prop's own mass and pairKg (0 = the prop's own).
inline float pairDriveKg(float propKg,float pairKg) noexcept {
    if(!(propKg>0.f)||!std::isfinite(propKg))return 0.f;return pairKg>0.f&&std::isfinite(pairKg)&&pairKg<propKg?pairKg:propKg;}
// RIGID CARRY (lever grabCarry): two hands on one ball-jointed prop drive it as one rigid body. Forces at the two grips act on points of
// the grip line, so rolling about that line moves neither grip: per-grip springs leave the roll undamped and control yaw and pitch only
// through a couple across the 20-35 cm grip spacing. The carry instead drives the centre of mass and the orientation directly.
// The frame: x = the grip line (hand A -> hand B) low-passed with gain k per substep; z = up made perpendicular to x, blended each
// substep from the previous z (moved onto the new x) by s = 1 - (x . up)^2, so roll is held against gravity while the line is level and
// carried over continuously when it is steep; y = z x x.
struct CarryFrame {V3 x{1,0,0},y{0,1,0},z{0,0,1};};
inline Rows carryRows(const CarryFrame& f) noexcept {return {f.x,f.y,f.z};}
inline CarryFrame carryFrameStep(const CarryFrame& prev,bool have,V3 line,V3 up,float k) noexcept {
    const V3 raw=v::normalized(line);const V3 un=v::normalized(up);
    if(v::length(raw)<.5f||v::length(un)<.5f)return prev;
    const float kk=std::isfinite(k)?std::clamp(k,0.f,1.f):1.f;
    V3 x=have?v::normalized(v::add(prev.x,v::mul(v::sub(raw,prev.x),kk))):raw;if(v::length(x)<.5f)x=raw;
    const auto perp=[&](V3 a){return v::sub(a,v::mul(x,v::dot(a,x)));};
    const V3 zr=perp(un);const float s=std::clamp(v::dot(zr,zr),0.f,1.f);V3 zrn=v::normalized(zr);
    const V3 zt=have?v::normalized(perp(prev.z)):V3{0,0,0};
    if(v::length(zrn)<.5f)zrn=v::normalized(perp(std::fabs(x[0])<.9f?V3{1,0,0}:V3{0,0,1}));
    V3 z=v::length(zt)<.5f?zrn:v::normalized(v::add(zt,v::mul(v::sub(zrn,zt),s)));
    if(v::length(z)<.5f)z=zrn;
    CarryFrame f{};f.x=x;f.z=z;f.y=v::cross(z,x);return f;}
// The prop's pose in the carry frame at the join: its axes and its centre of mass from the grips' middle (metres), in frame components.
struct CarryPose {Rows rel{};V3 off{};bool ok{};};
inline CarryPose carryPose(const CarryFrame& f,const Rows& r,V3 com,V3 gripMid,float upm) noexcept {
    CarryPose p{};const float u=std::max(upm,1e-3f);
    for(unsigned i=0;i<3;++i)p.rel[i]={v::dot(r[i],f.x),v::dot(r[i],f.y),v::dot(r[i],f.z)};
    const V3 d=v::mul(v::sub(com,gripMid),1.f/u);p.off={v::dot(d,f.x),v::dot(d,f.y),v::dot(d,f.z)};
    p.ok=v::finite(p.rel)&&v::finite(p.off);return p;}
inline V3 inFrame(const CarryFrame& f,V3 c) noexcept {return v::add(v::add(v::mul(f.x,c[0]),v::mul(f.y,c[1])),v::mul(f.z,c[2]));}
// The world inertia applied to x: sum I_i (r_i . x) r_i (r = the body's unit axes, I = the principal moments, kg m^2).
inline V3 inertiaApply(const Rows& r,V3 I,V3 x) noexcept {
    V3 o{0,0,0};for(unsigned i=0;i<3;++i){const V3 a=v::normalized(r[i]);o=v::add(o,v::mul(a,I[i]*v::dot(a,x)));}return o;}
struct CarryGains {float wn{10.f},zeta{1.f},accel{40.f},wr{10.f},zetaR{1.4f},tauMax{100.f},forceMax{900.f};};
// One substep of the carry (SI out: N, N m; positions in world units, upm per metre): the centre of mass's target = the commands' middle
// plus the pose's offset in the frame, its velocity = vMid + wT x (target - mid); a = wn^2 e + 2 zeta wn (vT - v) at most accel; F = M a
// + up g min(M, liftKg) at most forceMax. The axes' target = the pose's axes in the frame; tau = I (wr^2 theta + 2 zetaR wr (wT - w)) at
// most tauMax (theta = rotationBetween(axes, target), I applied in the body's axes). wRel = wT - w; relMps = |vT - v|.
// upMaxN: the force's upward part at most this (N; the player's mover standing on the prop: its weight, so the drive never lifts the
// player). aPred = the prop's acceleration under F and gravity (m/s^2, massKg the collider's own mass), alpha = I^-1 tau (rad/s^2): the
// grips' velocity after the step for the hand bodies (carryGripVel).
struct CarryOut {V3 fN{},tauNm{},eM{},theta{},wRel{},aPred{},alpha{};float relMps{};bool capF{},capTau{},capUp{},ok{};};
// I^-1 x in world axes (r = the body's unit axes, I = the principal moments, kg m^2; a moment <= 0 gives 0 on its axis).
inline V3 inertiaSolve(const Rows& r,V3 I,V3 x) noexcept {
    V3 o{0,0,0};for(unsigned i=0;i<3;++i){if(!(I[i]>0.f))continue;const V3 a=v::normalized(r[i]);o=v::add(o,v::mul(a,v::dot(a,x)/I[i]));}return o;}
inline CarryOut carryDrive(const CarryFrame& f,V3 wT,const CarryPose& p,V3 mid,V3 vMid,V3 com,V3 vCom,const Rows& r,V3 w,V3 inertia,float massKg,float liftKg,
                           V3 up,const CarryGains& g,float upm,float upMaxN=std::numeric_limits<float>::infinity()) noexcept {
    CarryOut o{};const float u=std::max(upm,1e-3f);const V3 un=v::normalized(up);
    if(!p.ok||!(massKg>0.f)||!std::isfinite(massKg)||v::length(un)<.5f||!v::finite(mid)||!v::finite(vMid)||!v::finite(com)||!v::finite(vCom)||!v::finite(w)||
       !v::finite(r)||!v::finite(wT)||!v::finite(inertia))return o;
    const V3 offW=inFrame(f,p.off);const V3 comT=v::add(mid,v::mul(offW,u));
    o.eM=v::mul(v::sub(comT,com),1.f/u);
    const V3 vT=v::add(v::mul(vMid,1.f/u),v::cross(wT,offW));const V3 dv=v::sub(vT,v::mul(vCom,1.f/u));o.relMps=v::length(dv);
    V3 a=v::add(v::mul(o.eM,g.wn*g.wn),v::mul(dv,2.f*g.zeta*g.wn));const float al=v::length(a),am=std::max(g.accel,0.f);
    if(al>am){a=al>0.f?v::mul(a,am/al):V3{0,0,0};o.capF=true;}
    const float lift=std::min(massKg,std::max(std::isfinite(liftKg)?liftKg:0.f,0.f));
    V3 F=v::add(v::mul(a,massKg),v::mul(un,lift*9.81f));const float fl=v::length(F),fm=std::max(g.forceMax,0.f);
    if(fl>fm){F=fl>0.f?v::mul(F,fm/fl):V3{0,0,0};o.capF=true;}
    {const float fu=v::dot(F,un);if(!std::isnan(upMaxN)&&fu>upMaxN){F=v::sub(F,v::mul(un,fu-std::max(upMaxN,0.f)));o.capUp=true;}}
    Rows rt{};for(unsigned i=0;i<3;++i)rt[i]=inFrame(f,p.rel[i]);
    o.theta=rotationBetween(r,rt);o.wRel=v::sub(wT,w);
    const V3 alpha=v::add(v::mul(o.theta,g.wr*g.wr),v::mul(o.wRel,2.f*g.zetaR*g.wr));
    V3 tau=inertiaApply(r,inertia,alpha);const float tl=v::length(tau),tm=std::max(g.tauMax,0.f);
    if(tl>tm){tau=tl>0.f?v::mul(tau,tm/tl):V3{0,0,0};o.capTau=true;}
    if(!v::finite(F)||!v::finite(tau))return CarryOut{};
    o.fN=F;o.tauNm=tau;o.aPred=v::sub(v::mul(F,1.f/massKg),v::mul(un,9.81f));o.alpha=inertiaSolve(r,inertia,tau);
    if(!v::finite(o.aPred))o.aPred={0,0,0};if(!v::finite(o.alpha))o.alpha={0,0,0};
    o.ok=true;return o;}
// The carry's state over a pair episode: the frame (low-passed over tauS) and its angular velocity, the pose from the join.
// carryJoin takes the pose in the frame of the grips' own line (no step at the join); carryTick moves the frame toward the hands' line
// (tracked positions, hand A -> hand B), wT = the low-passed frame's turn over the substep (smooth: no second filter, which would carry
// the target past a step), then carryDrive.
// resync: the next tick takes the hands' line as the frame at once with no frame spin (a pair resumed after a short break keeps its pose,
// while the filter's state is stale).
struct CarryState {bool have{};CarryFrame frame{};V3 wT{};CarryPose pose{};float lineStepRad{};bool resync{};};
// The gains for a substep of dt s: the drive is explicit (the engine integrates F and tau over dt), so the spring and damping terms keep
// wn dt <= 0.5 and zeta wn dt <= 0.5 (the damping's per-step gain 2 zeta wn dt <= 1, well under the explicit limit of 2) whatever the
// levers' range.
inline CarryGains carryGainsFor(CarryGains g,float dtS) noexcept {
    if(!(dtS>1e-5f)||!std::isfinite(dtS))return g;const float lim=.5f/dtS;
    const auto fix=[&](float& wn,float& z){if(!(wn>0.f)||!std::isfinite(wn))wn=1.f;if(!(z>0.f)||!std::isfinite(z))z=1.f;wn=std::min(wn,lim);z=std::min(z,lim/wn);};
    fix(g.wn,g.zeta);fix(g.wr,g.zetaR);return g;}
inline bool carryJoin(CarryState& c,V3 gripA,V3 gripB,const Rows& r,V3 com,V3 up,float upm) noexcept {
    c=CarryState{};const CarryFrame f0=carryFrameStep(CarryFrame{},false,v::sub(gripB,gripA),up,1.f);
    c.pose=carryPose(f0,r,com,v::mul(v::add(gripA,gripB),.5f),upm);c.frame=f0;c.have=c.pose.ok;return c.have;}
inline CarryOut carryTick(CarryState& c,V3 line,V3 mid,V3 vMid,V3 com,V3 vCom,const Rows& r,V3 w,V3 inertia,float massKg,float liftKg,V3 up,
                          const CarryGains& g,float dtS,float tauS,float upm,float upMaxN=std::numeric_limits<float>::infinity()) noexcept {
    if(!c.have)return CarryOut{};
    const float dt=std::isfinite(dtS)?std::clamp(dtS,0.f,.1f):0.f;
    const float k=c.resync?1.f:std::isfinite(tauS)&&tauS>0.f?std::clamp(dt/(tauS+dt),0.f,1.f):1.f;
    const CarryFrame f1=carryFrameStep(c.frame,true,line,up,k);
    const V3 rot=rotationBetween(carryRows(c.frame),carryRows(f1));c.lineStepRad=v::length(rot);
    const V3 wRaw=dt>1e-5f&&!c.resync?v::mul(rot,1.f/dt):V3{0,0,0};
    c.wT=v::finite(wRaw)?wRaw:V3{0,0,0};c.frame=f1;c.resync=false;
    return carryDrive(c.frame,c.wT,c.pose,mid,vMid,com,vCom,r,w,inertia,massKg,liftKg,up,carryGainsFor(g,dt),upm,upMaxN);}
// A carry's hand body velocity: the grip's velocity after the carry's step, vGrip + (aPred + alpha x (grip - com)) dt (world units), so
// the ball joint has no acceleration of the hand to supply (the hand bodies take no gravity); then gripFollowVel's position term.
inline V3 carryGripVel(V3 vGrip,V3 grip,V3 com,V3 aPred,V3 alpha,float dtS,float upm) noexcept {
    if(!v::finite(vGrip)||!v::finite(grip)||!v::finite(com)||!v::finite(aPred)||!v::finite(alpha)||!(dtS>1e-5f)||!std::isfinite(dtS))return vGrip;
    const V3 a=v::add(v::mul(aPred,std::max(upm,1e-3f)),v::cross(alpha,v::sub(grip,com)));const V3 o=v::add(vGrip,v::mul(a,dtS));return v::finite(o)?o:vGrip;}
// A force F (N) at the centre of mass plus a pure torque tau (N m) as two forces (phCollider::ApplyForce adds F to +0x150 and
// (pos - com) x F to +0x160): F / 2 + c at com + a n and F / 2 - c at com - a n, n a unit vector across tau, c = (tau x n) / (2 a) (then
// 2 a n x c = tau). Out: forces in N, positions in world units.
struct CoupleForces {std::array<V3,2> f{},pos{};};
inline CoupleForces coupleForces(V3 F,V3 tau,V3 com,float armM,float upm) noexcept {
    CoupleForces c{};const float u=std::max(upm,1e-3f),a=std::max(std::isfinite(armM)?armM:.2f,.05f);
    const float tl=v::length(tau);const V3 th=tl>1e-9f?v::mul(tau,1.f/tl):V3{0,0,1};
    const V3 ax=std::fabs(th[0])<=std::fabs(th[1])&&std::fabs(th[0])<=std::fabs(th[2])?V3{1,0,0}:std::fabs(th[1])<=std::fabs(th[2])?V3{0,1,0}:V3{0,0,1};
    const V3 n=v::normalized(v::cross(th,ax));
    const V3 cpl=tl>1e-9f?v::mul(v::cross(tau,n),1.f/(2.f*a)):V3{0,0,0};const V3 h=v::mul(F,.5f);
    c.f[0]=v::add(h,cpl);c.f[1]=v::sub(h,cpl);c.pos[0]=v::add(com,v::mul(n,a*u));c.pos[1]=v::sub(com,v::mul(n,a*u));return c;}
// A ball-jointed hand whose drive did not apply this substep: the mover's velocity plus (cmd - x - vMover dt) / tau (cmd is where the
// hand should be after this substep, the mover's own step included), the part relative to the mover at most maxSpeed: the body moves
// with the mover and closes the gap by dt / tau per substep, a first-order approach with no overshoot while the cap does not bind;
// tau <= dt: velocityTo over one substep.
inline Drive followTo(V3 x,V3 cmd,float dtS,float tauS,float maxSpeed,V3 moverVel) noexcept {
    if(!(std::isfinite(tauS)&&tauS>dtS)||!(dtS>1e-5f))return velocityTo(x,cmd,dtS,maxSpeed,moverVel);
    Drive d{};if(!v::finite(x)||!v::finite(cmd))return d;
    const V3 base=v::finite(moverVel)?moverVel:V3{0,0,0};
    V3 rel=v::mul(v::sub(v::sub(cmd,x),v::mul(base,dtS)),1.f/tauS);const float s=v::length(rel);
    if(!std::isfinite(s))return d;
    if(s>maxSpeed){rel=v::mul(rel,maxSpeed/s);d.clamped=true;}
    d.vel=v::add(base,rel);return d;}
// PLAYER SKIP (lever grabplayerskip): one impact iterator of the held heavy prop (iterator +0x10 = the prop) walked as the engine's own
// phImpact::Iterator::DisableImpact 0x28b4290 addresses a contact: manifold i (array A for i < countA, else array B), its sub-manifold k
// (k >= 1: [manifold + 0x110 + 8 k]), contact j at sub + 0x10 + 0xd0 j; an active contact (+0xce) whose manifold and owner (contact
// +0xc0, as propScan reads it) join the prop and the player's mover instance gets +0xce = 0, the write DisableImpact makes. At most 8 manifolds, 16 subs, 64 contacts
// each. rd / wr: guarded reads and the byte write (raw memory on the device, a fake layout in the host tests).
struct ImpactLayout {std::size_t arrA{0x0},arrB{0x8},countA{0x24},countB{0x28},subs{0x110},subCount{0x5ab},contacts{0x10},contactCount{0x5ac},
    stride{0xd0},active{0xce},depth{0xc},owner{0xc0},instA{0x550},instB{0x558};};
struct SkipOut {unsigned manifolds{},contacts{};float depthMax{};};
template<class Rd,class Wr>
inline SkipOut skipPlayerContacts(std::uintptr_t it,std::uintptr_t prop,std::uintptr_t mover,const ImpactLayout& L,Rd&& rd,Wr&& wr) noexcept {
    SkipOut o{};if(!it||!prop||!mover||prop==mover)return o;
    const auto word=[&](std::uintptr_t a)->std::uintptr_t{std::uintptr_t x{};rd(a,&x,sizeof x);return x;};
    std::int32_t cA{},cB{};rd(it+L.countA,&cA,4);rd(it+L.countB,&cB,4);if(cA<0||cB<0||cA>4096||cB>4096)return o;
    const auto arrA=word(it+L.arrA),arrB=word(it+L.arrB);const int total=std::min(cA+cB,8);
    for(int i=0;i<total;++i){
        const auto m=i<cA?(arrA?word(arrA+std::uintptr_t(i)*8u):0):(arrB?word(arrB+std::uintptr_t(i-cA)*8u):0);if(!m)continue;
        const auto a=word(m+L.instA),b=word(m+L.instB);
        if(!((a==prop&&b==mover)||(a==mover&&b==prop)))continue;
        std::uint8_t sc{};rd(m+L.subCount,&sc,1);const unsigned subs=std::min<unsigned>(std::max<unsigned>(sc,1u),16u);bool any=false;
        for(unsigned k=0;k<subs;++k){
            const auto sub=k?word(m+L.subs+std::uintptr_t(k)*8u):m;if(!sub)continue;
            std::uint8_t ncb{};rd(sub+L.contactCount,&ncb,1);const unsigned nc=std::min<unsigned>(ncb,64u);
            for(unsigned j=0;j<nc;++j){const auto c=sub+L.contacts+std::uintptr_t(j)*L.stride;
                std::uint8_t act{};rd(c+L.active,&act,1);if(!act)continue;
                {const auto own=word(c+L.owner);if(!own)continue;const auto oa=word(own+L.instA),ob=word(own+L.instB);
                 if(!((oa==prop&&ob==mover)||(oa==mover&&ob==prop)))continue;}
                float d{};rd(c+L.depth,&d,4);if(std::isfinite(d))o.depthMax=std::max(o.depthMax,d);
                const std::uint8_t zero=0;wr(c+L.active,&zero,1);++o.contacts;any=true;}}
        if(any)++o.manifolds;}
    return o;}
// A point in a frame (rows a b c, origin o): local -> world and back.
inline V3 fromFrame(const Rows& r,V3 o,V3 l) noexcept {return v::add(o,v::add(v::add(v::mul(r[0],l[0]),v::mul(r[1],l[1])),v::mul(r[2],l[2])));}
inline V3 toFrame(const Rows& r,V3 o,V3 p) noexcept {const V3 d=v::sub(p,o);return {v::dot(d,r[0]),v::dot(d,r[1]),v::dot(d,r[2])};}
// A welded prop's slip in the palm: its centre in the hand body's frame now against the same at the attach (world units; the hand's
// turn moves both together, so a turned hand reads 0 and only a prop that moved in the palm reads more); -1 when not finite.
inline float palmSlip(const Rows& r,V3 handPos,V3 propPos,V3 local0) noexcept {
    const float d=v::length(v::sub(toFrame(r,handPos,propPos),local0));return std::isfinite(d)?d:-1.f;}
// A ball-jointed hold drawn at the grip: offset = grip - B_now within the leash, no rotation offset (the hand keeps the controller's
// rotation; the prop turns on the joint).
inline VisualOut grabVisualGrip(VisualMem& m,V3 grip,V3 bNow,float leash) noexcept {
    VisualOut o{};m=VisualMem{};
    if(!v::finite(grip)||!v::finite(bNow))return o;
    V3 d=v::sub(grip,bNow);const float l=v::length(d);if(l>leash&&l>0.f)d=v::mul(d,leash/l);
    o.offset=v::finite(d)?d:V3{0,0,0};return o;
}
// GRAB (hook A, per hand while a hold is drawn): the drawn offset's change frame to frame. frames / still (the controller's body centre and
// the mover each moved under 3 mm), jumps |dO| (<.5, .5-1, 1-2, 2-5, 5+ cm), alternations (|dO| over 2 cm (alt2) / 1 cm (alt1) two frames
// running with the second reversed: cos < -.5; altStill = alt2 on a still frame), substeps since the last frame (0..4+), what reversed with
// an alt2 (the raw solved - B, the grip - B (prop), solved - grip (joint), the mover's step, a rotation offset over 5 deg), the hold's draw
// path toggles (grabbed on / off) and a 48-frame ring per hand (two words per frame).
struct GrabHookStats {std::atomic<std::uint64_t> frames{},still{},alt1{},alt2{},altStill{},toggles{},gripDrawn{},heavyDrawn{},offGrip{};std::array<std::atomic<std::uint64_t>,5> jumps{},subs{},altFrom{};
    std::array<std::atomic<std::uint64_t>,96> ring{};std::atomic<unsigned> ringNext{};};
inline std::array<GrabHookStats,2> grabHook{};
struct GrabHookMem {V3 o{},dO{},raw{},gripRel{},joint{},mover{},centre{};std::uint64_t serial{};bool have{},haveD{},grabbed{};};
inline unsigned cmBin(float cm) noexcept {return !(cm<5.f)?4u:cm<.5f?0u:cm<1.f?1u:cm<2.f?2u:3u;}
inline std::int64_t packMm(float m,int bits) noexcept {const float lim=float((1<<(bits-1))-1);const float v=std::isfinite(m)?std::clamp(m*1000.f,-lim,lim):0.f;return std::int64_t(v)&((std::int64_t(1)<<bits)-1);}
// One hook A frame of a holding hand. o = the drawn offset, raw = solved - B, gripRel = grip - B, joint = solved - grip, centre = B, mover =
// the mover's position, serial = the solved serial, rotDeg = the rotation offset drawn (all world units / upm metres).
inline void noteGrabHook(unsigned hand,GrabHookMem& m,bool grabbed,V3 o,V3 raw,V3 gripRel,V3 joint,V3 centre,V3 mover,std::uint64_t serial,float rotDeg,float lagM,float upm) noexcept {
    auto& S=grabHook[hand&1u];const float u=std::max(upm,1e-3f);
    if(grabbed!=m.grabbed){S.toggles.fetch_add(1,std::memory_order_relaxed);m.grabbed=grabbed;m.have=false;m.haveD=false;}
    if(!grabbed)return;
    S.frames.fetch_add(1,std::memory_order_relaxed);
    if(m.have){
        const V3 dO=v::mul(v::sub(o,m.o),1.f/u);const float jump=v::length(dO)*100.f;
        const bool still=v::length(v::sub(centre,m.centre))/u<.003f&&v::length(v::sub(mover,m.mover))/u<.003f;
        if(still)S.still.fetch_add(1,std::memory_order_relaxed);
        S.jumps[cmBin(jump)].fetch_add(1,std::memory_order_relaxed);
        const std::uint64_t sub=serial>=m.serial?serial-m.serial:0;S.subs[std::min<std::uint64_t>(sub,4u)].fetch_add(1,std::memory_order_relaxed);
        if(m.haveD){const float a=v::length(dO),b=v::length(m.dO);const float c=a>0.f&&b>0.f?v::dot(dO,m.dO)/(a*b):1.f;
            if(c<-.5f&&a>.01f&&b>.01f)S.alt1.fetch_add(1,std::memory_order_relaxed);
            if(c<-.5f&&a>.02f&&b>.02f){S.alt2.fetch_add(1,std::memory_order_relaxed);if(still)S.altStill.fetch_add(1,std::memory_order_relaxed);
                const auto rev=[&](V3 now,V3 before){const V3 d1=v::sub(now,before);return v::length(d1)/u>.01f&&v::dot(d1,v::mul(m.dO,u))<0.f;};
                if(rev(raw,m.raw))S.altFrom[0].fetch_add(1,std::memory_order_relaxed);
                if(rev(gripRel,m.gripRel))S.altFrom[1].fetch_add(1,std::memory_order_relaxed);
                if(rev(joint,m.joint))S.altFrom[2].fetch_add(1,std::memory_order_relaxed);
                if(rev(mover,m.mover))S.altFrom[3].fetch_add(1,std::memory_order_relaxed);
                if(rotDeg>5.f)S.altFrom[4].fetch_add(1,std::memory_order_relaxed);}}
        m.dO=dO;m.haveD=true;
        // the ring: w0 = offset x / y / z, lag (mm, 16 bits each); w1 = joint |solved - grip| (16), the grip's step (16), the controller's
        // step (12), the mover's step (12), substeps (4, capped 15), rotation (4 bits: degrees / 5, capped 15)
        const auto w0=std::uint64_t(packMm(o[0]/u,16))|std::uint64_t(packMm(o[1]/u,16))<<16|std::uint64_t(packMm(o[2]/u,16))<<32|std::uint64_t(packMm(lagM,16))<<48;
        const auto w1=std::uint64_t(packMm(v::length(joint)/u,16))|std::uint64_t(packMm(v::length(v::sub(gripRel,m.gripRel))/u,16))<<16|
            std::uint64_t(packMm(v::length(v::sub(centre,m.centre))/u,12))<<32|std::uint64_t(packMm(v::length(v::sub(mover,m.mover))/u,12))<<44|
            std::uint64_t(std::min<std::uint64_t>(sub,15u))<<56|std::uint64_t(std::min(unsigned(std::max(rotDeg,0.f)/5.f),15u))<<60;
        const unsigned i=S.ringNext.fetch_add(1,std::memory_order_relaxed)%48u;S.ring[i*2].store(w0,std::memory_order_relaxed);S.ring[i*2+1].store(w1,std::memory_order_relaxed);}
    m.o=o;m.raw=raw;m.gripRel=gripRel;m.joint=joint;m.centre=centre;m.mover=mover;m.serial=serial;m.have=true;
}
// Furniture whose type never moves (minMove < 0) held on purpose: a nav prop that is not a door, not infinite vs movers, not
// never-activate, its data mass up to maxKg, its level sphere up to maxR, a composite of activatable parts, no creature body near.
// a creature's bone capsule this near the prop's level sphere (m) refuses the release
inline constexpr float grabMoveNpcM=.15f;
enum GrabMoveWhy : std::uint8_t {GrabMoveOk=0,GrabMoveOff,GrabMoveClass,GrabMoveInfinite,GrabMoveNever,GrabMoveHeavy,GrabMoveBig,GrabMoveBound,GrabMoveNpcNear,GrabMoveCount};
inline constexpr std::array<const char*,GrabMoveCount> grabMoveNames{"ok","off","class","infinite","never","heavy","big","bound","npcNear"};
struct GrabMoveIn {bool on{},nav{},door{},infinite{},never{},composite{},partsActive{},npcNear{};std::uint8_t compD4{1};float massKg{-1.f},radiusM{-1.f};};
inline constexpr GrabMoveWhy grabMoveWhy(const GrabMoveIn& g,float maxKg,float maxR) noexcept {
    return !g.on?GrabMoveOff:(!g.nav||g.door)?GrabMoveClass:g.infinite?GrabMoveInfinite:g.never?GrabMoveNever:
        (!(g.massKg>0.f)||g.massKg>maxKg)?GrabMoveHeavy:(!(g.radiusM>0.f)||g.radiusM>maxR)?GrabMoveBig:(!g.composite||g.compD4||!g.partsActive)?GrabMoveBound:
        g.npcNear?GrabMoveNpcNear:GrabMoveOk;}
// A hold's length bucket: 0-250 ms, 0.25-1 s, 1-3 s, 3-10 s, 10 s+; a lag bucket (cm): 0-10, 10-20, 20-35, 35-60, 60+.
inline constexpr unsigned holdBucket(std::uint64_t ns) noexcept {return ns<250'000'000ull?0u:ns<1'000'000'000ull?1u:ns<3'000'000'000ull?2u:ns<10'000'000'000ull?3u:4u;}
inline unsigned lagBucket(float m) noexcept {return !(m<.6f)?4u:m<.10f?0u:m<.20f?1u:m<.35f?2u:3u;}
inline constexpr std::uint32_t grabTag=0x67525672u; // the constraint tag of a hand grab
inline constexpr std::uint64_t grabPressMaxNs=100'000'000ull,grabWakeNs=150'000'000ull;

// ============================== GLASS (step / tick) ==============================
// A window pane is a frag glass group (group +0x53 bit 1); the stock contact path breaks it only when the post-solve speed along the
// contact passes fragTune +0x30 (0.1 m/s by default), and the hand's own drive holds a touched surface at that speed, so a hand breaks
// glass at random. A pane met by a hand at glassMps or more (the controller's approach, not the clipped body) gets the engine's own break
// call (sagFragInst::CollisionDamage with a null iterator, as a bullet calls it); a slower touch never gets that call (the stock contact
// path keeps its own gate). Refusals in this order.
enum GlassWhy : std::uint8_t {GlassOk=0,GlassOff,GlassGone,GlassVt,GlassTypeNull,GlassCompRange,GlassNotGlass,GlassDead,GlassSlow,GlassNotEdge,GlassRateLimited,
    GlassCreateCap,GlassDelayed,GlassNoPlayer,GlassWhyCount};
inline constexpr std::array<const char*,GlassWhyCount> glassWhyNames{"ok","off","gone","vtMismatch","typeNull","compRange","notGlass","groupDead","slow",
    "notEdge","rate","createCap","delayed","noPlayer"};
// The approach histogram bins (m/s): 0-.5, .5-1, 1-1.5, 1.5-2, 2-3, 3-4, 4-6, 6+.
inline unsigned glassBin(float mps) noexcept {
    return !(mps>=.5f)?0u:mps<1.f?1u:mps<1.5f?2u:mps<2.f?3u:mps<3.f?4u:mps<4.f?5u:mps<6.f?6u:7u;}
// A pane: one break call per 500 ms and at most two per second.
struct GlassRate {
    std::array<std::uintptr_t,8> inst{};std::array<std::uint64_t,8> last{},prev{};unsigned next{};
    bool allow(std::uintptr_t p,std::uint64_t now) noexcept {
        for(std::size_t i=0;i<inst.size();++i)if(inst[i]==p&&p){
            if(now<last[i]||now-last[i]<500'000'000ull||(prev[i]&&now>=prev[i]&&now-prev[i]<1'000'000'000ull))return false;
            prev[i]=last[i];last[i]=now;return true;}
        inst[next]=p;last[next]=now;prev[next]=0;next=(next+1u)%unsigned(inst.size());return true;}
    // allow() without recording (a judge that does not break)
    bool peek(std::uintptr_t p,std::uint64_t now) const noexcept {
        for(std::size_t i=0;i<inst.size();++i)if(inst[i]==p&&p)
            return !(now<last[i]||now-last[i]<500'000'000ull||(prev[i]&&now>=prev[i]&&now-prev[i]<1'000'000'000ull));
        return true;}
};
// GLASS BLOOD: a bare hand that broke a pane plays the engine's own punch blood drop at the contact, attached to the hand body's matrix.
// The drop is "blood_push" (aniMelee::DoPunchFX 0x1e43718 -> CPtFxMelee::TriggerPtFxPushPunchKick 0x295fa18 -> TriggerPtFxMelee
// 0x295f7e0 with this hash for an NPC victim); TriggerPtFxMelee(self, actor, fx, fx2, mtx, pos, n, dir): pos / n in mtx's frame
// (CreateMatFromVecZ), dir the inherited velocity (16 bytes read, stored at inst +0x240), the owner key = actor; it skips the effect when
// |ms_gameCamPos - mtx +0x30|^2 * ms_fovScaleSqr > 100, and GetEffectInst returns null when effects are off, paused or the pool is full.
inline constexpr std::uint32_t joaat(const char* s) noexcept {
    std::uint32_t h=0;for(;*s;++s){const char c=*s>='A'&&*s<='Z'?char(*s-'A'+'a'):*s;h+=std::uint8_t(c);h+=h<<10;h^=h>>6;}
    h+=h<<3;h^=h>>11;h+=h<<15;return h;}
inline constexpr std::uint32_t bloodPushFx=0xd3cd9f0eu;
static_assert(joaat("blood_push")==bloodPushFx,"the punch blood drop's effect hash");
// Refusals, checked in this order.
// noPane (checked right after off): the call made no pane and left the group above its break floor (only a crack).
enum BloodWhy : std::uint8_t {BloodOk=0,BloodOff,BloodHolding,BloodThread,BloodCooldown,BloodBudget,BloodDisabled,BloodNoCreature,BloodNoRule,BloodFar,BloodNoPose,
    BloodNoPane,BloodWhyCount};
inline constexpr std::array<const char*,BloodWhyCount> bloodWhyNames{"ok","off","holding","thread","cooldown","budget","disabled","noCreature","noRule","far","noPose",
    "noPane"};
// The pane broke under the break call: a pane made (sm_CreateCount rose), or the group's health read after the call at or under -floor
// (a broken group reads 1 -> -199 with floor 1; the count may have been zeroed by bgGlassManager::UpdateBuffers in between). A health
// that only fell and stays above -floor is a crack.
inline bool glassPaneBroke(bool paneMade,bool haveHealth,float healthAfter,float floor) noexcept {
    return paneMade||(haveHealth&&std::isfinite(healthAfter)&&std::isfinite(floor)&&healthAfter<=-floor);}
// One drop per hand per cooldown, at most budgetN drops in budgetNs over both hands. allow() only checks; note() records a played drop.
struct BloodGate {
    static constexpr unsigned budgetN=3;static constexpr std::uint64_t budgetNs=2'000'000'000ull;
    std::array<std::uint64_t,2> last{};std::array<std::uint64_t,budgetN> recent{};
    BloodWhy allow(unsigned h,std::uint64_t now,std::uint64_t cdNs) const noexcept {
        const auto l=last[h&1u];if(l&&now>=l&&now-l<cdNs)return BloodCooldown;
        unsigned n=0;for(const auto t:recent)if(t&&now>=t&&now-t<budgetNs)++n;
        return n>=budgetN?BloodBudget:BloodOk;}
    void note(unsigned h,std::uint64_t now) noexcept {
        last[h&1u]=now?now:1;std::size_t o=0;for(std::size_t i=1;i<recent.size();++i)if(recent[i]<recent[o])o=i;recent[o]=now?now:1;}
};
// The drop's frame in the hand matrix's rows a / b / c (world vectors): the contact relative to the hand at the hit (its length at most
// 0.10 m, world units = upm per metre), the normal pointing out of the hand (-n, n = the pane's normal toward the hand), the inherited
// velocity = the hand's direction at 3 m/s (the punch: the injure direction normalised x 3), -n under 0.3 m/s.
struct BloodFrame {V3 pos{},n{},dir{};bool ok{};};
inline BloodFrame bloodFrame(V3 a,V3 b,V3 c,V3 hitPos,V3 handAtHit,V3 nTowardHand,V3 vel,float upm) noexcept {
    BloodFrame f{};const float u=std::isfinite(upm)&&upm>1e-3f?upm:1.f;
    if(!v::finite(a)||!v::finite(b)||!v::finite(c)||!v::finite(hitPos)||!v::finite(handAtHit)||!v::finite(nTowardHand))return f;
    V3 d=v::sub(hitPos,handAtHit);const float dl=v::length(d);if(dl>.10f*u)d=v::mul(d,.10f*u/dl);
    const V3 out=v::mul(v::normalized(nTowardHand),-1.f);if(v::length(out)<.5f)return f;
    f.pos={v::dot(a,d),v::dot(b,d),v::dot(c,d)};f.n={v::dot(a,out),v::dot(b,out),v::dot(c,out)};
    const V3 vv=v::finite(vel)?vel:V3{0,0,0};const float vl=v::length(vv);
    f.dir=v::mul(vl>=.3f*u?v::mul(vv,1.f/vl):out,3.f*u);
    f.ok=v::finite(f.pos)&&v::finite(f.n)&&v::finite(f.dir);return f;}
// BLOOD PLACE. The VR hands are drawn per eye after the particles, tested against the world depth only (HandsPass: the hand mesh never
// sees a particle, which writes no depth), so a drop anywhere inside the hand's outline as the eye sees it is painted over. The hand's
// reach from its centre B along a direction w across the view: its box S +-0.0425, F from -0.35 (the forearm) to +0.13 (open fingers) /
// +0.08 (fist), N from -0.014 (the back) to +0.014 / +0.0455 (the fist's curled fingers), metres x upm. An open hand's thumb adds
// bloodThumbM on its side of S (thumb +1: +S, the right hand; -1: -S, the left hand; 0: none, a fist's thumb lies over the fingers):
// S = F x N with N out of the palm, so for a right hand palm down, fingers forward, S points to its left, where its thumb is.
inline constexpr float bloodStandM=.015f,bloodSpanS=.0425f,bloodArmM=.35f,bloodFingersM=.13f,bloodFistM=.08f,bloodBackM=.014f,bloodFistNM=.0455f,
    bloodThumbM=.035f;
inline int handThumb(unsigned hand,bool fist) noexcept {return fist?0:(hand&1u)?1:-1;}
inline float handReach(V3 S,V3 F,V3 N,V3 w,bool fist,float upm,int thumb=0) noexcept {
    const float u=std::max(upm,1e-3f),ws=v::dot(w,S),wf=v::dot(w,F),wn=v::dot(w,N);
    const float span=std::fabs(ws)*bloodSpanS+(thumb&&ws*float(thumb)>0.f?std::fabs(ws)*bloodThumbM:0.f);
    return u*(span+(wf>=0.f?wf*(fist?bloodFistM:bloodFingersM):-wf*bloodArmM)+(wn>=0.f?wn*(fist?bloodFistNM:bloodBackM):-wn*bloodBackM));}
// A world point inside the hand's outline from the eye (its offset from B across the view within the reach that way).
inline bool insideOutline(V3 S,V3 F,V3 N,V3 b,V3 eye,V3 q,bool fist,float upm,int thumb=0) noexcept {
    const V3 e=v::sub(eye,b);const float el=v::length(e);if(!(el>1e-4f)||!v::finite(q))return false;
    const V3 vv=v::mul(e,1.f/el),d=v::sub(q,b),dp=v::sub(d,v::mul(vv,v::dot(d,vv)));const float l=v::length(dp);
    if(!(l>1e-6f))return true;
    return l<=handReach(S,F,N,v::mul(dp,1.f/l),fist,upm,thumb);}
// The drop outside that outline: of the directions across the view made from S, -S, F, N, -N and up (each without its part along the view
// ray), the one the hand reaches least along (a downward one 3 cm worse), the point B + w (reach + bloodStandM), at most 0.12 m from B;
// its normal (the effect's Z) w turned 30 % toward the eye and 20 % up, its inherited velocity along w and up at mps (the drops fly out of
// the outline and arc down beside it, not into the pane). pos / n in the hand's rows S F N, dir in the world; side = the face of the hand
// the eye sees (sign of N . toward the eye). ok false: no eye (unusable, non-finite or within 5 cm of B).
struct BloodPlace {BloodFrame f{};V3 w{},world{};float reach{},eyeM{};int side{};bool ok{};};
inline BloodPlace bloodEyePlace(V3 S,V3 F,V3 N,V3 b,V3 eye,V3 up,bool fist,float mps,float upm,int thumb=0) noexcept {
    BloodPlace p{};const float u=std::max(upm,1e-3f);
    if(!v::finite(S)||!v::finite(F)||!v::finite(N)||!v::finite(b)||!v::finite(eye))return p;
    const V3 e=v::sub(eye,b);const float el=v::length(e);if(!(el>=.05f*u)||!std::isfinite(el))return p;
    const V3 vv=v::mul(e,1.f/el);V3 un=v::normalized(up);if(!(v::length(un)>.5f))un={0,1,0};
    const std::array<V3,6> cand{S,v::mul(S,-1.f),F,N,v::mul(N,-1.f),un};
    float best=1e30f;V3 bw{};float br=0.f;
    for(const auto& c:cand){V3 w=v::sub(c,v::mul(vv,v::dot(c,vv)));const float l=v::length(w);if(!(l>=.3f))continue;w=v::mul(w,1.f/l);
        const float r=handReach(S,F,N,w,fist,upm,thumb),score=r/u+(v::dot(w,un)<-.1f?.03f:0.f)-.005f*v::dot(w,un);
        if(score<best){best=score;bw=w;br=r;}}
    if(!(best<1e29f))return p;
    V3 d=v::mul(bw,br+bloodStandM*u);const float dl=v::length(d);if(dl>.12f*u)d=v::mul(d,.12f*u/dl);
    const V3 nw=v::normalized(v::add(v::add(bw,v::mul(vv,.3f)),v::mul(un,.2f)));
    const V3 dw=v::mul(v::normalized(v::add(v::mul(bw,.6f),v::mul(un,.8f))),std::max(mps,0.f)*u);
    p.f.pos={v::dot(S,d),v::dot(F,d),v::dot(N,d)};p.f.n={v::dot(S,nw),v::dot(F,nw),v::dot(N,nw)};p.f.dir=dw;
    p.f.ok=v::finite(p.f.pos)&&v::finite(p.f.n)&&v::finite(p.f.dir)&&v::length(nw)>.5f;
    p.w=bw;p.world=v::add(b,d);p.reach=br;p.eyeM=v::length(v::sub(eye,p.world))/u;p.side=v::dot(N,vv)>=0.f?1:-1;p.ok=p.f.ok;
    return p;}
// GUN GLASS gates, checked in this order (each refusal counted; a refusal restarts the sweep: the next tick probes from where the muzzle
// is then). off = lever -1; notGun = no placed firearm in this hand; kind = not a pistol (WeaponClass 1 / 2) without gunGlassLong; class =
// not ClassHeld; foot = not on foot; bridged = a turn bridge this frame; stale = no fresh targets or no tracking-space velocity; settling =
// the gun placed under 300 ms; shot / spin / reload = that handling this frame or within 300 ms; long = the sweep over 0.3 m (a snap turn,
// a teleport); slow = the muzzle under 0.8 x gunGlassMps. The free hand (another hand holds the firearm): notFree = not on its own free
// target, nearGun = within 15 cm of the bore (a support, reload or fan hand), then the same class / foot / bridged / stale / long / slow.
enum GunWhy : std::uint8_t {GunGo=0,GunOff,GunNotGun,GunKind,GunClass,GunFoot,GunBridged,GunStale,GunSettling,GunShot,GunSpin,GunReload,GunLong,GunSlow,
    GunNotFree,GunNearGun,GunWhyCount};
inline constexpr std::array<const char*,GunWhyCount> gunWhyNames{"go","off","notGun","kind","class","foot","bridged","stale","settling","shot","spin","reload",
    "long","slow","notFree","nearGun"};
struct GunGateIn {int lever{-1};bool gun{},longOk{},held{},onFoot{},bridged{},fresh{},velOk{};int kind{-1};std::uint64_t placedNs{},busyNs{},now{};
    std::uint8_t busy{},busyRecent{};float sweepM{},speedMps{},mps{2.f};};
inline constexpr std::uint64_t gunSettleNs=300'000'000ull,gunBusyHoldNs=300'000'000ull;inline constexpr float gunSweepMaxM=.30f,gunSlowFrac=.8f,gunNearBoreM=.15f;
inline GunWhy gunGate(const GunGateIn& g) noexcept {
    if(g.lever<0)return GunOff;
    if(!g.gun)return GunNotGun;
    if(!g.longOk&&g.kind!=1&&g.kind!=2)return GunKind;
    if(!g.held)return GunClass;
    if(!g.onFoot)return GunFoot;
    if(g.bridged)return GunBridged;
    if(!g.fresh||!g.velOk)return GunStale;
    if(!g.placedNs||g.now<g.placedNs||g.now-g.placedNs<gunSettleNs)return GunSettling;
    const std::uint8_t b=std::uint8_t(g.busy|g.busyRecent);
    if(b&gunBusyShot)return GunShot;
    if(b&gunBusySpin)return GunSpin;
    if(b&gunBusyReload)return GunReload;
    if(!(g.sweepM<=gunSweepMaxM))return GunLong;
    if(!(g.speedMps>=gunSlowFrac*g.mps))return GunSlow;
    return GunGo;}
// The distance from a point to the segment a..b.
inline float segDistance(V3 p,V3 a,V3 b) noexcept {
    const V3 ab=v::sub(b,a);const float l2=v::dot(ab,ab);const float t=l2>1e-12f?std::clamp(v::dot(v::sub(p,a),ab)/l2,0.f,1.f):0.f;
    return v::length(v::sub(p,v::add(a,v::mul(ab,t))));}
// One phIntersection (0x70 bytes, the level probe's output): the instance, position, normal, part (u16 +0x64, 0xffff = none) and component.
struct ProbeHit {std::uintptr_t inst{};V3 pos{},n{};int part{-1},comp{-1};};
inline ProbeHit parseIsect(const unsigned char* p) noexcept {
    ProbeHit h{};std::uintptr_t inst{};float pos[3]{},n[3]{};std::uint16_t part{},comp{};
    std::memcpy(&inst,p+0x0,sizeof inst);std::memcpy(pos,p+0x10,12);std::memcpy(n,p+0x20,12);std::memcpy(&part,p+0x64,2);std::memcpy(&comp,p+0x6a,2);
    h.inst=inst;h.pos={pos[0],pos[1],pos[2]};h.n={n[0],n[1],n[2]};h.part=part==0xffffu?-1:int(part);h.comp=int(comp);return h;}
// The approach of a point moving at vel into a surface whose normal n is turned toward `toward` (m/s; > 0 = into it); n out = the turned normal.
inline float gunApproach(V3 vel,V3 hitPos,V3 n,V3 toward,float upm,V3& nOut) noexcept {
    V3 nn=v::normalized(n);if(v::length(nn)<.5f){nOut=V3{};return 0.f;}
    if(v::dot(nn,v::sub(toward,hitPos))<0.f)nn=v::mul(nn,-1.f);nOut=nn;
    const float a=-v::dot(vel,nn)/std::max(upm,1e-3f);return std::isfinite(a)?a:0.f;}
// The engine's own distance cull of TriggerPtFxMelee (true = the effect would be skipped).
inline bool bloodCulled(V3 cam,V3 at,float fovScaleSqr) noexcept {
    const V3 d=v::sub(cam,at);const float q=v::dot(d,d)*fovScaleSqr;return !(q<=100.f);}
// BLOOD ROUTE. An effect instance is linked into the draw list its rule names: ptxEffectRuleStd::InitInstance 0x23292d8 copies rule
// +0x37d to inst +0x338, rmPtfxManager::ActivateInstance 0x233dfb4 puts the instance's slot at the head of lists[+0x338] (rm +0x4f0,
// count +0x4f8; an index at or over the count takes list 0). CPtFxManager::InitRMPTFX names the lists 0 "High-res FX", 1 "Low-res FX",
// 2 "UnSorted FX", 3 "UnderWater FX"; DrawHighres draws list 0 (list 2 only with +0x11dce, which nothing sets), DrawLowres draws nothing
// while +0x11cf9 is 0 (CPtFxManager::Init writes 0, nothing else writes it), so an effect in list 1 or 2 is created, updated and never
// drawn. The hand's drop is routed to list 0 when its rule's list is one of those.
enum BloodRouteWhy : std::uint8_t {RouteGo=0,RouteLever,RouteGroup,RouteRead,RouteVt,RouteDrawn,RouteWhyCount};
inline constexpr std::array<const char*,RouteWhyCount> bloodRouteWhyNames{"go","lever","group","read","vt","drawn"};
// The list an instance of a rule with this list byte ends in (ActivateInstance's own index rule).
inline constexpr unsigned ptfxListOf(unsigned list,unsigned count) noexcept {return list<count?list:0u;}
// A list no pass draws: 1 while the low-res switch is 0, 2 always (an index over the count lands in list 0 and is drawn).
inline constexpr bool ptfxListDead(unsigned list,unsigned count,unsigned lowRes) noexcept {
    const unsigned l=ptfxListOf(list,count);return (l==1u&&lowRes==0u)||l==2u;}
inline constexpr BloodRouteWhy bloodRouteWhy(bool lever,bool group,bool read,bool vtOk,unsigned list,unsigned count,unsigned lowRes) noexcept {
    return !lever?RouteLever:!group?RouteGroup:!read?RouteRead:!vtOk?RouteVt:!ptfxListDead(list,count,lowRes)?RouteDrawn:RouteGo;}
// The watch on the last drop's instance (inst = slot + 0x20, the CullEffect argument) until its deadline: TraceEntry's CullEffect hook
// counts the calls for it inside the eye scope (and those that culled it) and outside. One relaxed load per CullEffect call; the clock
// only on a match.
struct PtfxWatch {std::atomic<std::uintptr_t> inst{};std::atomic<std::uint64_t> until{};
    std::atomic<std::uint64_t> seenEye{},droppedEye{},seenOther{},droppedOther{},arms{};};
inline PtfxWatch ptfxWatch{};
inline bool ptfxWatchHitAt(const PtfxWatch& w,std::uintptr_t inst,std::uint64_t now) noexcept {
    const auto x=w.inst.load(std::memory_order_relaxed);return x&&x==inst&&now<w.until.load(std::memory_order_relaxed);}
inline bool ptfxWatchHit(std::uintptr_t inst) noexcept {
    const auto x=ptfxWatch.inst.load(std::memory_order_relaxed);if(!x||x!=inst)return false;
    const auto now=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    return ptfxWatchHitAt(ptfxWatch,inst,now);}
inline void ptfxWatchNote(PtfxWatch& w,bool eye,bool dropped) noexcept {
    (eye?w.seenEye:w.seenOther).fetch_add(1,std::memory_order_relaxed);
    if(dropped)(eye?w.droppedEye:w.droppedOther).fetch_add(1,std::memory_order_relaxed);}
inline void ptfxWatchArm(PtfxWatch& w,std::uintptr_t inst,std::uint64_t until) noexcept {
    w.inst.store(0,std::memory_order_relaxed);w.until.store(until,std::memory_order_relaxed);w.inst.store(inst,std::memory_order_release);
    w.arms.fetch_add(1,std::memory_order_relaxed);}
// The edge of a glass contact, per pane component (a window's frame and its panes are components of one frag): the first 3 substeps of
// a contact with a component the hand did not touch in the previous 150 ms, or the approach rising to fastMps from under half of it (a
// hand resting on the pane then hitting it). Four components tracked per hand (the stalest taken over).
struct GlassTrack {std::uintptr_t inst{};int comp{-1};std::uint64_t firstStep{},lastNs{};float lastApproach{};};
struct GlassTracks {
    std::array<GlassTrack,4> t{};
    bool edge(std::uintptr_t inst,int comp,std::uint64_t step,std::uint64_t now,float approach,float fastMps) noexcept {
        const float a=std::isfinite(approach)?approach:0.f;
        GlassTrack* e=nullptr;for(auto& x:t)if(x.inst==inst&&x.comp==comp&&inst&&now>=x.lastNs&&now-x.lastNs<=150'000'000ull&&step>=x.firstStep){e=&x;break;}
        bool edge=false;
        if(!e){e=&t[0];for(auto& x:t)if(x.lastNs<e->lastNs)e=&x;*e=GlassTrack{};e->inst=inst;e->comp=comp;e->firstStep=step;edge=true;}
        else edge=step-e->firstStep<3||(a>=fastMps&&e->lastApproach<.5f*fastMps);
        e->lastNs=now;e->lastApproach=a;return edge;}
};
// Delayed frag damage (fragInst::IsFragDamageDelayed 0x1e98144): an override (+0x80 bit 21) decides with bit 20, else the frag type's
// +0x1ba sign bit (a null type: not delayed).
inline constexpr bool fragDamageDelayed(std::uint32_t f80,bool typeOk,std::int16_t type1ba) noexcept {
    return (f80>>21&1u)?(f80>>20&1u)!=0:(typeOk&&type1ba<0);}

// ============================== per-mode CPU (tick) ==============================
// A frame counts for its mode only with the same mode as the tick before (not the first after a change) and a wall interval of
// 5..100 ms (a loading screen ticks every 1-2 ms, a stall over 100 ms is not a frame).
enum CpuWindow : std::uint8_t {CpuCount=0,CpuShort,CpuLong,CpuModeChange};
inline constexpr CpuWindow cpuWindow(bool sameMode,std::uint64_t wallNs) noexcept {
    return !sameMode?CpuModeChange:wallNs<5'000'000ull?CpuShort:wallNs>100'000'000ull?CpuLong:CpuCount;}
inline constexpr unsigned cpuBuckets=80;inline constexpr double cpuBucketMs=.5;
inline unsigned cpuBucket(std::uint64_t ns) noexcept {return unsigned(std::min<std::uint64_t>(ns/500'000ull,cpuBuckets-1));}
// The q-quantile of a histogram (bucket centres, ms); 0 without samples.
inline double histPercentile(const std::uint64_t* b,unsigned n,double bucketMs,double q) noexcept {
    std::uint64_t total=0;for(unsigned i=0;i<n;++i)total+=b[i];if(!total)return 0.0;
    const double target=q*double(total);std::uint64_t acc=0;
    for(unsigned i=0;i<n;++i){acc+=b[i];if(double(acc)>=target&&b[i])return (double(i)+.5)*bucketMs;}
    return (double(n)-.5)*bucketMs;
}

// ============================== own phInst vtable ==============================
// The stock phInst vtable copied (offset-to-top, typeinfo, 34 slots +0x00..+0x108) with slot +0x100 PrepareForDeactivation replaced:
// sagSimulator::PreCollide deactivates slow active objects once the level's active count passes 200 (phSimulator::DeactivateObject asks
// +0x100 first), and a hand body must not be put to sleep under the player's hand.
inline constexpr std::size_t instTableWords=36,deactivateWord=2+0x100/8;
inline void buildInstTable(const std::uintptr_t* stock,std::uintptr_t fn,std::uintptr_t* out) noexcept {
    for(std::size_t i=0;i<instTableWords;++i)out[i]=stock[i];
    out[deactivateWord]=fn;
}

// ============================== hook A / render thread counters ==============================
struct HandHookStats {std::array<std::atomic<std::uint64_t>,3> classFrames{};std::atomic<std::uint64_t> offsetFrames{},offsetUmSum{},offsetUmMax{},rotFrames{},rotMdegMax{};};
inline std::array<HandHookStats,2> hookStats{};
inline std::atomic<std::uint64_t> mvDrops{},hookTargets{},hookSolvedStale{},hookNsSum{},hookNsMax{},hookCalls{};
inline void noteHook(unsigned hand,HandClass c,float offsetM,float rotRad=0.f) noexcept {
    auto& s=hookStats[hand&1u];s.classFrames[unsigned(c)%3u].fetch_add(1,std::memory_order_relaxed);
    if(rotRad>1e-4f){s.rotFrames.fetch_add(1,std::memory_order_relaxed);const auto md=std::uint64_t(rotRad*180000.f/pi);
        auto m=s.rotMdegMax.load(std::memory_order_relaxed);while(md>m&&!s.rotMdegMax.compare_exchange_weak(m,md,std::memory_order_relaxed)){}}
    if(!(offsetM>0.f))return;
    const auto um=std::uint64_t(offsetM*1e6f);s.offsetFrames.fetch_add(1,std::memory_order_relaxed);s.offsetUmSum.fetch_add(um,std::memory_order_relaxed);
    auto m=s.offsetUmMax.load(std::memory_order_relaxed);while(um>m&&!s.offsetUmMax.compare_exchange_weak(m,um,std::memory_order_relaxed)){}
}
inline void noteHookNs(std::uint64_t ns) noexcept {
    hookCalls.fetch_add(1,std::memory_order_relaxed);hookNsSum.fetch_add(ns,std::memory_order_relaxed);
    auto m=hookNsMax.load(std::memory_order_relaxed);while(ns>m&&!hookNsMax.compare_exchange_weak(m,ns,std::memory_order_relaxed)){}
}

// ============================== a held prop swung into a creature ==============================
// The handphys tick's verdict on a held prop's contact with a creature's mover capsule (the contact taken in the prop's impact iterator):
// one request per hit, consumed by VrMelee (the player's strike). point = the contact, n = from the prop into the creature, dir = the
// prop's horizontal velocity there (the swing), speed = its approach along n (m/s), strong = at propHitStrongMps or more, hands = the
// holding hands (bit per hand), hand = the hand that holds it (the first), ko = the propko lever.
struct PropHitRequest {std::uint64_t serial{},ns{};std::uintptr_t actor{};std::uint32_t guid{};V3 point{},n{},dir{};float speed{},ko{1.f};
    std::uint8_t strong{},hands{};int hand{-1};
    // a thrown prop in flight (THROW: hands 0, hand = the throwing hand) and its real mass (kg)
    std::uint8_t thrown{};float kg{-1.f};};
// The requests (serial 1, 2, ...): slot serial & 3, propHitLatest = the newest serial published. Two creatures hit in one tick, or ticks
// that run before VrMelee consumes, each keep their request; the consumer walks the serials it has not seen (the 4 newest at most).
inline constexpr unsigned propHitSlots=4;
inline std::array<::rdrvr::vr::body::Seqlock<PropHitRequest>,propHitSlots> propHitRing;
inline std::atomic<std::uint64_t> propHitLatest{};
inline void publishPropHit(const PropHitRequest& r) noexcept {
    if(!r.serial)return;propHitRing[r.serial%propHitSlots].store(r);
    std::uint64_t l=propHitLatest.load(std::memory_order_relaxed);while(r.serial>l&&!propHitLatest.compare_exchange_weak(l,r.serial,std::memory_order_release,std::memory_order_relaxed)){}}
// The serials a consumer that has seen up to seen walks when the newest is latest: [first, latest], skipped = those already overwritten.
// A producer that started again (latest < seen) is walked from its oldest kept slot.
struct PropHitWalk {std::uint64_t first{},last{},skipped{};};
inline PropHitWalk propHitWalk(std::uint64_t seen,std::uint64_t latest) noexcept {
    PropHitWalk w{};if(!latest||latest==seen)return w;
    std::uint64_t from=latest<seen?1u:seen+1u;const std::uint64_t oldest=latest>=propHitSlots?latest-propHitSlots+1u:1u;
    if(from<oldest){w.skipped=latest<seen?0u:oldest-from;from=oldest;}
    w.first=from;w.last=latest;return w;}
// The gate of a creature's contacts with the held prop: an edge after propHitEdgeNs without contact, at minMps or more, coolNs after the
// last hit of that creature. Per creature (4 kept, the oldest replaced). The tick passes the smaller of the approach (the prop against the
// creature) and the swing (the prop against the player's mover): a creature running into a still prop, or a prop carried at a run, is
// no swing (notSwung).
enum PropHitWhy : std::uint8_t {PropHitOk=0,PropHitSlow,PropHitCooldown,PropHitNotEdge,PropHitPlayer,PropHitNoActor,PropHitDead,PropHitNotCand,PropHitNotHuman,
    PropHitInvulnerable,PropHitStale,PropHitNotSwung,PropHitNoArm,PropHitWhyCount};
inline constexpr std::array<const char*,PropHitWhyCount> propHitWhyNames{"ok","slow","cooldown","notEdge","player","noActor","dead","notCandidate","notHuman",
    "invulnerable","stale","notSwung","noArm"};
inline constexpr std::uint64_t propHitEdgeNs=150'000'000ull;
struct PropHitGate {
    struct E {std::uint32_t guid{};std::uint64_t contactNs{},hitNs{};};std::array<E,4> e{};
    // contact of guid at now with approach a: Ok = a hit (its clock kept), else why not; every contact refreshes the edge clock
    PropHitWhy step(std::uint32_t guid,std::uint64_t now,float a,float minMps,std::uint64_t coolNs) noexcept {
        E* x=nullptr;for(auto& q:e)if(q.guid==guid){x=&q;break;}
        if(!x){x=&e[0];for(auto& q:e)if(q.contactNs<x->contactNs)x=&q;*x=E{guid,0,0};}
        const bool edge=!x->contactNs||now<x->contactNs||now-x->contactNs>=propHitEdgeNs;x->contactNs=now?now:1;
        if(!edge)return PropHitNotEdge;
        if(!(std::isfinite(a)&&a>=minMps))return PropHitSlow;
        if(x->hitNs&&now>=x->hitNs&&now-x->hitNs<coolNs)return PropHitCooldown;
        x->hitNs=now?now:1;return PropHitOk;}
};
// A speed's bin (m/s): 0-1, 1-2, 2-3, 3-4, 4-6, 6+.
// The gate's speed and the reason a slow one gets: approach and swing (m/s) along the contact normal.
inline float propHitGateSpeed(float approach,float swing) noexcept {
    if(!std::isfinite(approach)||!std::isfinite(swing))return 0.f;return std::min(approach,swing);}
inline PropHitWhy propHitSlowWhy(float approach,float swing,float minMps) noexcept {
    return std::isfinite(approach)&&approach>=minMps&&!(std::isfinite(swing)&&swing>=minMps)?PropHitNotSwung:PropHitSlow;}
inline unsigned propSpeedBin(float a) noexcept {return !(a>=1.f)?0u:a<2.f?1u:a<3.f?2u:a<4.f?3u:a<6.f?4u:5u;}
// ============================== PROPHIT (geometric) ==============================
// A part of the held prop as an oriented box in world: centre, unit axes, half extents (world units).
struct Obb {V3 c{},ax[3]{{1,0,0},{0,1,0},{0,0,1}},h{};};
// A point of a 4x4 row-axis matrix (rows 0..2 = the axes, row 3 = the origin; the engine's Mat34V layout) and a direction through it.
inline V3 matPoint(const float* m,V3 p) noexcept {return {m[12]+m[0]*p[0]+m[4]*p[1]+m[8]*p[2],m[13]+m[1]*p[0]+m[5]*p[1]+m[9]*p[2],m[14]+m[2]*p[0]+m[6]*p[1]+m[10]*p[2]};}
inline V3 matDir(const float* m,V3 d) noexcept {return {m[0]*d[0]+m[4]*d[1]+m[8]*d[2],m[1]*d[0]+m[5]*d[1]+m[9]*d[2],m[2]*d[0]+m[6]*d[1]+m[10]*d[2]};}
// The box of part bounds bmin..bmax in the part's matrix (the instance's frame) under the instance's matrix. ok = finite, non-empty.
inline bool partObb(const float* inst,const float* part,V3 bmin,V3 bmax,Obb& o) noexcept {
    if(!v::finite(bmin)||!v::finite(bmax)||bmax[0]<bmin[0]||bmax[1]<bmin[1]||bmax[2]<bmin[2])return false;
    const V3 lc=v::mul(v::add(bmin,bmax),.5f);o.c=matPoint(inst,matPoint(part,lc));o.h=v::mul(v::sub(bmax,bmin),.5f);
    const V3 e[3]{{1,0,0},{0,1,0},{0,0,1}};
    for(unsigned i=0;i<3;++i){o.ax[i]=v::normalized(matDir(inst,matDir(part,e[i])));if(v::length(o.ax[i])<.5f)return false;}
    return v::finite(o.c)&&v::finite(o.h);}
// The point of the box nearest p.
inline V3 obbClamp(const Obb& o,V3 p) noexcept {
    const V3 d=v::sub(p,o.c);V3 q=o.c;
    for(unsigned i=0;i<3;++i){const float x=std::clamp(v::dot(d,o.ax[i]),-o.h[i],o.h[i]);q=v::add(q,v::mul(o.ax[i],x));}
    return q;}
// The distance between the segment a..b and the box (0 when they meet): the squared distance of a point to a convex box is convex along
// the segment, so a golden-section search over the segment's parameter finds its minimum (iters steps: 0.618^iters of the length).
// boxPt / segPt = the nearest pair found.
struct SegObb {float d{1e30f};V3 boxPt{},segPt{};};
inline SegObb segObbDist(V3 a,V3 b,const Obb& o,unsigned iters=16) noexcept {
    SegObb r{};const V3 ab=v::sub(b,a);
    const auto f=[&](float t){const V3 p=v::add(a,v::mul(ab,t));const V3 d=v::sub(p,obbClamp(o,p));return v::dot(d,d);};
    constexpr float g=.618034f;float lo=0.f,hi=1.f,x1=hi-g*(hi-lo),x2=lo+g*(hi-lo),f1=f(x1),f2=f(x2);
    for(unsigned k=0;k<iters;++k){if(f1<=f2){hi=x2;x2=x1;f2=f1;x1=hi-g*(hi-lo);f1=f(x1);}else{lo=x1;x1=x2;f1=f2;x2=lo+g*(hi-lo);f2=f(x2);}}
    float t=.5f*(lo+hi);const float fa=f(0.f),fb=f(1.f),fm=f(t);if(fa<fm&&fa<=fb)t=0.f;else if(fb<fm)t=1.f;
    const V3 p=v::add(a,v::mul(ab,t)),q=obbClamp(o,p);r.d=v::length(v::sub(p,q));r.boxPt=q;r.segPt=p;if(!std::isfinite(r.d))r.d=1e30f;return r;}
// Per creature: a touch (within the shell, or an engine contact of the prop with its mover) armed by 150 ms apart (beyond the shell + 3 cm,
// no engine contact) is judged every tick for winNs; the first tick whose speeds pass (pass) and outside coolNs of the last hit is Ok.
// After the window, or after a verdict, NotEdge until it separates again (a slow rest no longer uses up the next swing). An entry
// unseen for 300 ms counts as apart since then. judged = a verdict this tick (why), start = the touch began this tick.
struct GeoVerdict {bool judged{},start{};PropHitWhy why{PropHitNotEdge};};
inline constexpr std::uint64_t propGeoSepNs=150'000'000ull,propGeoStaleNs=300'000'000ull;
// Entries are kept per (creature, prop): two props held one per hand have their own touch states.
struct PropGeoGate {
    struct E {std::uint32_t guid{};std::uintptr_t prop{};bool touching{},armed{},done{};std::uint64_t touchNs{},sepNs{},hitNs{},seenNs{};};std::array<E,6> e{};
    GeoVerdict step(std::uint32_t guid,std::uintptr_t prop,std::uint64_t now,bool touch,bool apart,bool pass,PropHitWhy failWhy,std::uint64_t winNs,
                    std::uint64_t coolNs) noexcept {
        GeoVerdict r{};if(!guid)return r;
        E* x=nullptr;for(auto& q:e)if(q.guid==guid&&q.prop==prop){x=&q;break;}
        if(!x){x=&e[0];for(auto& q:e)if(q.seenNs<x->seenNs)x=&q;*x=E{};x->guid=guid;x->prop=prop;}
        else if(x->seenNs&&now>=x->seenNs&&now-x->seenNs>=propGeoStaleNs){if(x->touching)x->sepNs=x->seenNs;x->touching=false;}
        x->seenNs=now?now:1;
        if(apart){if(x->touching){x->touching=false;x->sepNs=now?now:1;}return r;}
        if(!touch)return r; // between the shell and its hysteresis: the touch state holds
        if(!x->touching){x->touching=true;x->touchNs=now;r.start=true;x->done=false;
            x->armed=!x->sepNs||(now>=x->sepNs&&now-x->sepNs>=propGeoSepNs);}
        r.judged=true;
        if(!x->armed||x->done){r.why=PropHitNotEdge;return r;}
        if(now<x->touchNs||now-x->touchNs>winNs){x->done=true;r.why=PropHitNotEdge;return r;}
        if(!pass){r.why=failWhy;return r;}
        x->done=true;
        if(x->hitNs&&now>=x->hitNs&&now-x->hitNs<coolNs){r.why=PropHitCooldown;return r;}
        x->hitNs=now?now:1;r.why=PropHitOk;return r;}
    bool known(std::uint32_t guid,std::uintptr_t prop) const noexcept {for(const auto& q:e)if(guid&&q.guid==guid&&q.prop==prop)return true;return false;}
};
// The geometric verdict's speed test: the prop's point against the creature (a) and against the player's mover (swing) along n, a holding
// hand's own arm speed (arm, m/s) and its arm speed along n (armDir: a lift or a turn with the prop moves the arms across the normal, a
// swing at the creature along it); fail = why not (slow, notSwung, noArm).
inline bool propGeoPass(float a,float swing,float arm,float armDir,float minMps,float armMps,float armDirMps,PropHitWhy& fail) noexcept {
    const bool aOk=std::isfinite(a)&&a>=minMps,sOk=std::isfinite(swing)&&swing>=minMps,armOk=std::isfinite(arm)&&arm>=armMps,
               dirOk=std::isfinite(armDir)&&armDir>=armDirMps;
    fail=!aOk?PropHitSlow:!sOk?PropHitNotSwung:!(armOk&&dirOk)?PropHitNoArm:PropHitOk;return aOk&&sOk&&armOk&&dirOk;}
// A holding hand's arm velocities of the last ticks (m/s, world axes; zero without velArm): the arm gate's peaks over a window, the speed
// and the speed along a normal (never below 0) with the age of that peak (ms; -1 = no sample). The prop trails the hands, so the peaks
// reach back past the touch.
struct ArmTrail {
    static constexpr unsigned N=48;std::array<V3,N> v{};std::array<std::uint64_t,N> ns{};unsigned next{};
    void push(V3 x,std::uint64_t now) noexcept {v[next]=v::finite(x)?x:V3{0,0,0};ns[next]=now?now:1;next=(next+1u)%N;}
    struct Peak {float mag{},along{},alongAgeMs{-1.f};};
    Peak peak(V3 n,std::uint64_t now,std::uint64_t winNs) const noexcept {
        Peak p{};bool any=false;
        for(unsigned i=0;i<N;++i){if(!ns[i]||ns[i]>now||now-ns[i]>winNs)continue;
            const float m=v::length(v[i]),a=v::dot(v[i],n);
            if(std::isfinite(m))p.mag=std::max(p.mag,m);
            if(std::isfinite(a)&&(!any||a>p.along)){p.along=a;p.alongAgeMs=float(double(now-ns[i])*1e-6);any=true;}}
        p.along=std::max(p.along,0.f);return p;}
};

// ============================== THROW (held props) ==============================
// A held nav prop let go with a fast arm flies: the open edge of each holding hand (throwEdgeStep), the arm's estimate over the window
// before it (throwEstimate, the knife throw's method: the peak sample averaged with its contiguous neighbours at 85 % of it or more), the
// verdict (throwVerdict). Every refusal leaves the release to grabStep (a heavy hold's 350 ms open, a light hold's own rules).
enum ThrowVerdict : std::uint8_t {ThrowGo=0,ThrowSlow,ThrowDown,ThrowStale,ThrowPairWait,ThrowPairHeld,ThrowNoArm,ThrowMeasure,ThrowVerdictCount};
inline constexpr std::array<const char*,ThrowVerdictCount> throwVerdictNames{"throw","slow","down","stale","pairWait","pairHeld","noArm","measure"};
inline constexpr float throwDownDot=.7f,throwSpinMaxRadS=10.f,throwRestMps=.5f,throwTapMps=2.5f,throwMassFullKg=25.f;
inline constexpr std::uint64_t throwPairDropNs=250'000'000ull,throwRestNs=100'000'000ull;inline constexpr unsigned throwMaxHits=2;
// The open edge: the grip at grabRegrip or more arms it; two ticks in a row under grabOpenSqueeze confirm it (a single zeroed sample never
// does). firstNs = the first of those ticks. Returns true on the confirming tick (once per arm).
struct ThrowEdge {bool armed{};std::uint8_t under{};std::uint64_t firstNs{},confirmNs{};};
inline bool throwEdgeStep(ThrowEdge& e,float squeeze,std::uint64_t now) noexcept {
    if(!std::isfinite(squeeze))return false;
    if(squeeze>=grabRegrip){e.armed=true;e.under=0;e.firstNs=0;return false;}
    if(squeeze>=grabOpenSqueeze){e.under=0;e.firstNs=0;return false;}
    if(!e.armed)return false;
    if(!e.under)e.firstNs=now?now:1;
    if(++e.under<2)return false;
    e.armed=false;e.under=0;e.confirmNs=now?now:1;return true;}
// The arm's estimate over [from, now] of a trail (m/s, world axes): the peak sample, its age (ms), the mean of the peak and its contiguous
// neighbours in time at 85 % of its speed or more (v, its length est), the samples seen.
struct ThrowEst {float peak{},peakAgeMs{-1.f},est{};V3 v{};unsigned n{};};
inline ThrowEst throwEstimate(const ArmTrail& t,std::uint64_t from,std::uint64_t now) noexcept {
    ThrowEst e{};std::array<unsigned,ArmTrail::N> idx{};unsigned n=0;
    for(unsigned k=0;k<ArmTrail::N;++k){const unsigned i=(t.next+k)%ArmTrail::N;if(!t.ns[i]||t.ns[i]>now||t.ns[i]<from)continue;idx[n++]=i;} // oldest first
    e.n=n;if(!n)return e;
    unsigned pk=0;float best=-1.f;for(unsigned j=0;j<n;++j){const float m=v::length(t.v[idx[j]]);if(std::isfinite(m)&&m>best){best=m;pk=j;}}
    if(!(best>0.f))return e;
    e.peak=best;e.peakAgeMs=float(double(now-t.ns[idx[pk]])*1e-6);
    V3 sum=t.v[idx[pk]];unsigned c=1;
    for(int j=int(pk)-1;j>=0&&v::length(t.v[idx[unsigned(j)]])>=.85f*best;--j){sum=v::add(sum,t.v[idx[unsigned(j)]]);++c;}
    for(unsigned j=pk+1;j<n&&v::length(t.v[idx[j]])>=.85f*best;++j){sum=v::add(sum,t.v[idx[j]]);++c;}
    e.v=v::mul(sum,1.f/float(c));e.est=v::length(e.v);return e;}
// The verdict of an estimate: stale (the peak older than the window), slow (peak or estimate under its minimum), down (mostly downward:
// a drop or a put-down), else a throw.
inline ThrowVerdict throwVerdict(const ThrowEst& e,V3 up,float peakMin,float relMin,float winMs) noexcept {
    if(!e.n||!(e.peak>0.f))return ThrowNoArm;
    if(e.peakAgeMs>winMs)return ThrowStale;
    if(!(e.peak>=peakMin)||!(e.est>=relMin))return ThrowSlow;
    const V3 u=v::normalized(up);if(v::length(u)>.5f&&v::dot(e.v,u)<-throwDownDot*e.est)return ThrowDown;
    return ThrowGo;}
// A pair: both hands' confirmed edges within pairMs of each other decide; the first hand waits up to pairMs for the second (PairWait),
// after that the pair is held (PairHeld: no throw, the open hand's own release). otherConfirmNs 0 = the other hand has not opened.
inline ThrowVerdict throwPairState(std::uint64_t firstNs,std::uint64_t otherFirstNs,std::uint64_t otherConfirmNs,std::uint64_t now,float pairMs) noexcept {
    const std::uint64_t win=std::uint64_t(double(std::max(pairMs,0.f))*1e6);
    if(otherConfirmNs&&otherFirstNs){const std::uint64_t d=firstNs>=otherFirstNs?firstNs-otherFirstNs:otherFirstNs-firstNs;return d<=win?ThrowGo:ThrowPairHeld;}
    return now>=firstNs&&now-firstNs>win?ThrowPairHeld:ThrowPairWait;}
// The launch velocity (m/s): the estimate plus the player's motion, at most maxMps.
inline V3 throwLaunch(V3 estMps,V3 moverMps,float maxMps) noexcept {
    V3 x=v::add(v::finite(estMps)?estMps:V3{0,0,0},v::finite(moverMps)?moverMps:V3{0,0,0});
    const float l=v::length(x);if(std::isfinite(maxMps)&&maxMps>0.f&&l>maxMps)x=v::mul(x,maxMps/l);return x;}
// The launch is written only when it is faster along its direction than the prop already moves (the prop keeps a faster swing of its own).
inline bool throwWrites(V3 propMps,V3 launchMps) noexcept {
    const float l=v::length(launchMps);if(!(l>1e-3f)||!v::finite(propMps))return false;return v::dot(propMps,v::mul(launchMps,1.f/l))<l;}
// A thrown prop's hit: the KO scale by mass (propHitKg -> 0.5 .. throwMassFullKg -> 1), strong from strongMps and kg x a >= kgMps; a
// light prop's tap zone (head, neck, chest capsules).
inline float throwMassScale(float kg,float hitKg) noexcept {
    if(!std::isfinite(kg))return .5f;const float span=std::max(throwMassFullKg-hitKg,1e-3f);return std::clamp(.5f+.5f*(kg-hitKg)/span,.5f,1.f);}
inline bool throwStrong(float kg,float a,float strongMps,float kgMps) noexcept {return std::isfinite(kg)&&std::isfinite(a)&&a>=strongMps&&kg*a>=kgMps;}
inline constexpr bool throwTapZone(int capKind) noexcept {return capKind>=0&&capKind<=2;}
// The approach speed a light prop's tap hands to the slap path (its reaction impulse grows with it): at most throwTapMps, inside a
// hand slap's range, whatever the prop's own speed (up to throwMaxMpsLight).
inline float throwTapA(float a) noexcept {return std::isfinite(a)?std::clamp(a,0.f,throwTapMps):0.f;}
// A flight's end (FlightNone = it goes on): the prop came to rest (under throwRestMps for throwRestNs), its collider left the level's
// active set (asleep), the level slot no longer holds it, the time ran out, a hand took it again, the mode / simulator changed, it hit
// throwMaxHits creatures.
enum FlightEnd : std::uint8_t {FlightNone=0,FlightRest,FlightInactive,FlightStale,FlightTimeout,FlightRegrab,FlightMode,FlightLevel,FlightHits,FlightEndCount};
inline constexpr std::array<const char*,FlightEndCount> flightEndNames{"none","rest","inactive","stale","timeout","regrab","mode","level","hits"};
inline FlightEnd flightStep(std::uint64_t& slowSince,float speedMps,std::uint64_t t0,std::uint64_t now,float flightMs,unsigned hits) noexcept {
    if(hits>=throwMaxHits)return FlightHits;
    if(now>=t0&&double(now-t0)>=double(flightMs)*1e6)return FlightTimeout;
    if(std::isfinite(speedMps)&&speedMps<throwRestMps){if(!slowSince)slowSince=now?now:1;if(now>=slowSince&&now-slowSince>=throwRestNs)return FlightRest;}
    else slowSince=0;
    return FlightNone;}
// ============================== player skip slots ==============================
// PLAYER SKIP: per hand the held prop the precompute guard serves (a heavy hold, or a welded light hold with grabSkipLight; both hands on
// one prop serve it in both slots), and two linger slots for released props. A prop that leaves the held slots lingers in a free linger
// slot (or in the one that started first, evicted); a prop held again leaves its linger.
struct SkipLinger {std::uintptr_t prop{};std::uint16_t index{0xffff};std::uint64_t startNs{},seenNs{},step{};float depth{};bool deepSeen{},light{};};
struct SkipSlots {std::array<std::uintptr_t,2> prop{};std::array<std::uint16_t,2> index{0xffff,0xffff};std::array<bool,2> light{};std::array<SkipLinger,2> linger{};};
// One tick of the slots; returns the lingers evicted.
inline unsigned skipSlotsStep(SkipSlots& k,const std::array<std::uintptr_t,2>& held,const std::array<std::uint16_t,2>& heldIndex,
                              const std::array<bool,2>& light,std::uint64_t now) noexcept {
    unsigned evicted=0;
    for(unsigned i=0;i<2;++i){const auto p=k.prop[i];if(!p||p==held[0]||p==held[1])continue;
        if(k.linger[0].prop==p||k.linger[1].prop==p)continue;
        const unsigned j=!k.linger[0].prop?0u:!k.linger[1].prop?1u:k.linger[0].startNs<=k.linger[1].startNs?0u:1u;
        if(k.linger[j].prop)++evicted;
        k.linger[j]=SkipLinger{};k.linger[j].prop=p;k.linger[j].index=k.index[i];k.linger[j].startNs=now?now:1;k.linger[j].seenNs=now;k.linger[j].light=k.light[i];}
    for(unsigned i=0;i<2;++i){k.prop[i]=held[i];k.index[i]=held[i]?heldIndex[i]:std::uint16_t(0xffff);k.light[i]=held[i]&&light[i];}
    for(auto& l:k.linger)if(l.prop&&(l.prop==held[0]||l.prop==held[1]))l=SkipLinger{};
    return evicted;}
// The held slot an instance is served by (-1 none), the linger slot (-1 none).
inline int skipHeldSlot(const SkipSlots& k,std::uintptr_t inst) noexcept {return !inst?-1:inst==k.prop[0]?0:inst==k.prop[1]?1:-1;}
inline int skipLingerSlot(const SkipSlots& k,std::uintptr_t inst) noexcept {return !inst?-1:inst==k.linger[0].prop?0:inst==k.linger[1].prop?1:-1;}

} // namespace rdrvr::vr::handphys
