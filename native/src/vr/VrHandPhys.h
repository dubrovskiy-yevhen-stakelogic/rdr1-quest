#pragma once
// HAND PHYSICS: each empty VR hand is an active, velocity-driven rigid body inside the engine's own physics (the kind of body the player's
// mover is). The engine finds and solves the contacts; this module steers the body toward the controller and reads back where it stopped.
// HANDS page row HAND PHYSICS (OFF / MEASURE / ON), kill switch debug.rdrvr.vr.handphys 0.
//
// Engine side (librdr.so 1.58 arm64, vaddr), every called function and layout witness checked by a whole-function FNV-1a in install(),
// in groups: CORE (refuses everything), PALM (falls back to the capsule), ACT, DEACT, DOORX, CCD, REACH, BUMP, SWING, REST, RELEASE, TOUCH,
// GRAB, UNFREEZE, DOORGATE, GLASS, DRIVE, DIAG, SELF, NODE, RESETHOLD (each turns off its own feature):
//  - sagWorld::UpdatePhysLevel runs sm_PhysicsStepCount (2) substeps per frame on the main thread; each calls sagPhysicsLevel::UpdatePhysLevel
//    0x28a99a0: phSimulator::PreUpdateInstanceBehaviors (behaviour vtable +0x28 PreUpdate(dt), every entry of sim+0x60 / count +0x72) -> the
//    step (sim vtable +0x10) -> UpdateInstanceBehaviors (+0x30 Update(dt)).
//  - ProcessOverlaps: for an instance with flag +0x12 bit 0x100 its behaviour (sim+0x78[level index], or the sorted search) gets
//    CollideObjects(myInst, myCollider, otherInst, otherCollider, otherBehaviour) (+0x38) after the pair functor; false drops the pair.
//  - Body PALM: a phBoundComposite (0x1baa394, Init(3, false) 0x1baa3e4: parts +0xa0, part matrices +0xa8 (= +0xb0), part flags +0xc0 /
//    +0xc8 {type, include}, tested on both sides by ComputeCollision 0x1c6f8d4 / 0x1c6fa98) of three phBoundBox(short* verts, size)
//    0x1ba44d4 (no allocation with a vertex buffer while sm_CreateWorldSpaceVertsForCloth is 0; size 0x270), CalculateCompositeExtents
//    0x1bab1b8; SetAngInertia 0x1c84510 after SetMass. Sphere: phBoundSphere(float) 0x1bce294. Capsule: phBoundCapsule() 0x1ba87c4 +
//    SetCapsuleSize 0x1ba893c (local Y axis). phArchetypePhys() 0x1c83de8, SetBound 0x1c83974, SetMass 0x1c842b4, SetGravityFactor(0)
//    0x1c84458, type +0x20 / include +0x24, max angular speed +0x48 (4 pi by default), phInst() 0x1c82b88 + Init 0x1c82c64. Instance
//    flag +0x12 bit 0x2 = CCD for every pair of it (CollisionIntersection 0x1c29dc8). User data (+0x18) = a default sagInstData in our
//    storage (sagInstData(phInst*) 0x28a8eec: kind 0xff, audio slot 0xffff): rdrPhysicsAudioDispatch::PreApplyImpacts 0x227385c and
//    GetPhysicsAudioEntity 0x2273ed0 read ud+0x1a without a null check. It is never cleared.
//  - Own instance vtable: the stock phInst vtable (0x2ce6830, 36 words checked) with +0x100 PrepareForDeactivation = false:
//    sagSimulator::PreCollide 0x28abc64 deactivates slow active objects once the level's active count passes 200, through
//    DeactivateObject 0x1c303cc, which asks +0x100 first.
//  - Impact callbacks: phContactMgr::PerformImpactsCallbacks 0x1c38d24 runs the simulator's PreApplyImpacts functor (sim+0xe8, copied by
//    UpdateNew 0x1c36d88 after SolveConstraints) for both instances of every manifold, iterator +0x10 = the instance. The fn word
//    (sagSimulator::PreApplyImpacts_Game 0x28abbd4) is swapped for preApplyGuard, which returns for our instances (an empty phInst slot
//    and the audio dispatch) and forwards every other iterator to PreApplyImpacts_Game. PreComputeImpacts (sim+0xc8,
//    DefaultPreComputeImpacts 0x1c27c24) only calls the stock phInst +0xc8 (ret) for our instances and is checked, not wrapped.
//  - Insert: phSimulator::AddActiveObject(inst, alwaysActive = true) 0x1c2e934 (a pool collider, level index or 0xffff); the collider is
//    level+0x78[index] (DeleteObjectHelper 0x1c2fb94). Remove: DeleteObject(index, false) 0x1c2fd20. Respawn: TeleportObject 0x1c2f94c.
//    The behaviour list (+0x60 / +0x72 / +0x74 / dirty +0x70), the slot (+0x78[index]) and the instance flag 0x100 are written here exactly as
//    AddInstBehavior 0x1c28ad4 / RemoveInstBehavior 0x1c28b24 write them, but never for an instance outside the level (index 0xffff would
//    write past the slot array): in MEASURE the behaviours are in the list only (they time the step), in ON the slot follows the instance.
//  - Steering: phCollider::SetVelocity 0x1c65478, SetAngVelocity 0x1c654d8 (world), in our PreUpdate of every substep. Contact readback:
//    phCollider::UpdateImp 0x1c65198 integrates the position before the collision, so solved - (x + v dt) is the solver's push.
//  - Props: sagWorldScript::ForceActivation(fragInst*) 0x2acc4b0 = IsActivationDisabled -> SetDisableActivation(false) ->
//    phSimulator::ActivateObject(sim, inst, 0, 0) (state 1 and active count +0x14 < cap +0xc), for fragInstNav (GetClassType 0x286fe04,
//    not a door: +0x1be != 1) and rdrDeadBodyInst (0x2242c60), not with infinite mass vs movers (+0x82 bit 7, 0x1e98274).
//  - REACH WAKE: propPropIterator::StartCullSphere(centre, radius) 0x2870dd0 on our 0x408-byte iterator: g_GlobalPhysicsLock as a reader,
//    phLevelNew::GetFirstCulledObject / GetNextCulledObject on the level of GOT 0x2de2030 (sphere cull, states active and inactive, type
//    & 0x40003), classes 5 and 9..12 queued (at most 0x80, count +0x404). The objects' bounding spheres: level object +0x0 (centre, radius).
//  - NPC bump: aActor::Broadcast(mvrMsgBumpedInto const&) 0x1e48bb8 on the touched creature with {0x2f, the player's guid}, as
//    mvrMoverInst::PreComputeImpacts sends it (0x2891da8..0x2891dbc); aiMindComponent::HandleBumpedInto 0x1e49f44 takes the bumper's actor
//    from the slot table [GOT 0x2deabb0] by guid & 0xffff without a check, so both guids are verified in the slot table first.
//    entEntityInst::PrepareForActivation 0x2912184 returns null for a live creature: a physical shove is not possible.
//  - Doors: propPropManager::sm_Instance, GetDoor(fragInstNav*) 0x2871ac4 -> propDoorHandle::Get 0x286be08 -> state +0x58 ->
//    Open(pusher position, false) 0x286b30c (state 0, the sagVehicle::PreComputeImpacts precedent 0x28d104c), Close(false) 0x286b228.
//  - DOOR SWING (SWING): a pivot door (interface vptr PivotDoorInterface, GOT 0x2e10f30) is re-posed by propDoorInst::Update 0x286acc4
//    every frame from its phase iface+0x24 (ContinueOpenDerived 0x2a773bc / ContinueCloseDerived 0x2a774a0 -> ContinueActionShared
//    0x2a7710c: angle = ease(t) * cap iface+0x28[dir], Matrix34::Rotate + SetCurrentMatrix 0x286aa90). After Open(palm) the tick writes
//    speed iface+0x20 = 0 (the speed StartOpenDerived 0x2a772f8 wrote) and then the phase from the palm; a release writes the speed back
//    (the engine finishes the swing) or calls FastClose(false) 0x286a794 (state 0, the closed matrix through SetClosedDerived 0x2a773f8).
//    A speed the game wrote (SetActionSpeedScale 0x2a774dc), a state or interface change drops the hold without a write.
//  - Props: a kept pair with an inactive nav prop whose frag type locks activation (IsActivationDisabled 0x1e97ef4, no override) gets
//    SetDisableActivation(false) 0x1e97ed4 in CollideObjects, as phInstBehaviorSagExplosion::CollideObjects 0x2536648 does; the engine's
//    activation pass of the same CollideActive then wakes it with the hand as the other (sagFragInst::PrepareForActivation 0x2873bc0).
//  - NPC virtual body: VrMelee publishes bone capsules of up to 3 creatures near a palm (vbodies); PreUpdate clips the hand's command
//    out of them (no engine object), Update merges the clip into the contact estimate (the visual stop of hook A follows). A press sends
//    mvrMsgBumpedInto every tick (PressGate). TOUCH: an open hand's first substeps on a body classify a slap or a shove (touchVerdict), a
//    slower touch of the head opens a slap window decided when the hand leaves, a held palm is a press that escalates to a push once it
//    asked for pressPushM; VrMelee gets one TouchRequest per hand and tick (the reaction; the engine's reaction node of levers slapnode /
//    pushnode, else an injure message from the creature itself or a guid no consumer resolves, never from the player and never with
//    damage (openTouchPlan); a mover offset). A shove has a tier (pushTier): only a firm one (pushfirmmps / pushfirmcm / pusharmmps) sends
//    the message, its impulse along the push and its face from the push's direction; an escalated press and a medium shove only move the
//    creature (lever pushmedium 1: an UP reaction message for a medium one). Graded (lever pushgrade, the default): a palm driven into
//    the neck or the chest is never a slap, a second hand on the creature within pairms makes one pair push (no slap gate), a slap
//    verdict waits slapholdms for that hand; VrMelee answers a push by its effective approach: a stumble (offset only) or a knockdown
//    (SHOVED and a kick that sets the body's velocity along the push).
//  - DOORS (REST): a push counts only with a hand part on the leaf (VrHandPhysMath leafTouch). The game's own moves (states 2, 3, 5) are
//    never touched; a closed door is opened posed at the hand (openPosed: the engine's Open, the swing limit, the phase, SetState(4)), an
//    open door (state 4) is posed by its phase and ContinueActionShared (POSE4), its leaf following the hand's nearest part on either face;
//    released it rests at its angle. The leaf is never posed into the player's mover (a push stops where the leaf meets him, so it never
//    shuts on him); shut at doorshut (Close) only with nothing in the closing wedge. Without a sure leaf (no box, not upright, not a pivot
//    door) a closed door gets the plain Open.
//  - DOORGATE: the jump slot of sagPlayerInputDoor::FindDoor (GOT 0x2e53628) answers "no door" for the player's automatic walk-in open
//    while the hands are live on foot (lever doorauto 0); the mover then pushes a closed or part-open pivot leaf ahead of him (bodyDoor, the
//    leaf's thickness counted), a closed door he is at that no push opened gets the stock Open after doorstuckms (one timer per leaf), a
//    closing push held at the floor shuts the leaf so it opens away from him; the counting hooks of Open / Close (GOT 0x2e482b8 / 0x2e482b0) hand a
//    part-open door an NPC or a script opens back to the engine's opening. NPC, script, bullet and vehicle doors are never gated.
//  - GLASS: the hand's impact iterator (preApplyGuard) gives the pane contact; a hit at glassMps or more gets the engine's break
//    (sagFragInst::CollisionDamage with a null iterator, the projectile path's Collision).
//  - UNFREEZE: the size decides (the type's mass is shared data); a palm closing on such a prop makes it physical ahead of the contact
//    (approachTick); active, the prop's own collider gets a light mass, a press that met it first the hand's contact speed (kickProp).
//  - RELEASE: a small inactive nav prop under a pressing palm gets +0x80 bit 1 (BreakApart's write) before its ForceActivation.
//  - GRAB: a grip press of an open hand at a prop or a corpse part joins it to the hand body (phContactMgr::AttachObjects); VrGrabLink.h.
//    DRIVE: the held prop gets phCollider::ApplyForce at the grip each substep toward the hand's command (a ball-jointed hold: a damped
//    spring on the grip point, the hand body moving with the grip, drawn there by hook A); fixed rotation only up to
//    grabRotKg; a break only past grabbreakcm for grabbreakms; furniture whose type never moves is released by a deliberate grab.
//  - RESETHOLD (also needs UNFREEZE): the jump slot of ggoComponentProp::StartPropReset (GOT 0x2e4dc50): the gringo readiness poll
//    (ggoComponentProp::OnMsg 0x2000, return address 0x250c734) of a prop the hands made physical, more than 1 cm from its reset position,
//    answers "no reset started" without GracefulPropReset::Apply (the stock immediate path's own answer: the hotspot stays usable), so the
//    prop stays where it landed; the counting hooks of CommonReset /
//    Apply (GOT 0x2e4d0d0 / 0x2e4d0e0) see the stock resets the hold does not stop (resetHoldTick lets the prop go then).
//  - TWO HANDS: both hands on one ball-jointed prop drive it as a pair (pairStep: the grip line follows the line between the hands).
//  - PROPHIT: a held prop's impact iterator (preApplyGuard -> propScan) gives its contacts with creature mover capsules; a fast contact edge
//    is a PropHitRequest (VrMelee: the player's fist strike on that creature).
//  - CPU: the main thread's CPU time per frame and mode (CLOCK_THREAD_CPUTIME_ID at the tick).
// Threads: the tick (UpdateInputs, mainthread id 13), PreUpdate / Update / CollideObjects (inside the step) all run on the main thread;
// wrongThread counts a callback on any other thread (it then does nothing and drops the pair). preApplyGuard and instNoDeactivate (inside
// the step) read constants and bump relaxed counters only. Every engine write and call except the velocity sets (the build, inserts and
// removes, part flags, ForceActivation, the cull, door Open / Close) is made by the tick, outside the step. Hook A (VrBody.h, workers)
// only reads and writes the seqlocks of VrHandPhysMath.h.
// Contact = the solver's push or velocity change (VrHandPhysMath.h noteSubstep). It drives the drive clip, the visual stop, the haptic
// pulse, the part toggle, the prop wakes and door opens.
// Gates: a body only for ClassBody (an empty free hand on foot, VrHandPhysMath.h classify), a hook A target within staleMs, the player's
// mover by its vtable, mover flags read; carried over 12 m/s or a class change -> ghost, a ghost over 1 s -> removed. Any CORE print or
// word mismatch: nothing is created (stock physics).
#include "VrArc.h"
#include "VrHandPhysMath.h"
#include "VrHaptics.h"
#include "VrMainThread.h"
#include "VrSettings.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include "VrGrabLink.h"
#include "VrHandBlood.h" // BLOOD ON THE KNUCKLES: the glass break stamps the hand, HandsPass tints it
#include "runtime/HoldChord.h" // the manual-theater grip chord's diagnostics (the hold-end ring, heavy holds)
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace rdrvr::vr::handphys {
enum Group : std::uint8_t {GroupCore=0,GroupPalm,GroupAct,GroupDeact,GroupDoorx,GroupCcd,GroupReach,GroupBump,GroupSwing,GroupRest,GroupRelease,GroupTouch,
    GroupGrab,GroupUnfreeze,GroupDoorGate,GroupGlass,GroupDrive,GroupDiag,GroupSelf,GroupNode,GroupResetHold,GroupKick,GroupGround,GroupTree,GroupPick,GroupBlood,
    GroupTough,GroupPickFn,GroupWeld,GroupBloodRoute,
    GroupBloodLife,
    GroupGunGlass,
    GroupCount};
inline constexpr std::array<const char*,GroupCount> groupNames{"core","palm","act","deact","doorx","ccd","reach","bump","swing","rest","release","touch","grab",
    "unfreeze","doorgate","glass","drive","diag","self","node","resethold","kick","ground","tree","pick","blood","tough","pickfn","weld","bloodroute",
    "bloodlife",
    "gunglass"};
struct Print {std::uintptr_t offset;std::uint32_t size;std::uint64_t fnv;std::uint8_t group;};
// Called functions and layout witnesses (whole functions unless noted).
inline constexpr Print prints[]{
    {0x1c2e934,0x37c,0xad42c98fe366c24full,GroupCore}, // phSimulator::AddActiveObject(phInst*, bool)
    {0x1c28ad4,0x50,0xdb5d3249c3964371ull,GroupCore},  // phSimulator::AddInstBehavior: list +0x60 / +0x72 / +0x74, slot +0x78, flag 0x100, dirty +0x70
    {0x1c28b24,0x6c,0x5e605b5a5d1708a2ull,GroupCore},  // phSimulator::RemoveInstBehavior
    {0x1c28fd8,0x5c,0x0d77ac16cdc6fad3ull,GroupCore},  // phSimulator::PreUpdateInstanceBehaviors: vtable +0x28
    {0x1c29034,0x5c,0xe75bcd7137d16347ull,GroupCore},  // phSimulator::UpdateInstanceBehaviors: vtable +0x30
    {0x28a99a0,0x7c,0xaa325ec2c7d00523ull,GroupCore},  // sagPhysicsLevel::UpdatePhysLevel: PreUpdate -> step -> Update, sim = GOT 0x2de2098
    {0x1c2af8c,0x244,0x168a9ff8edc7e29dull,GroupCore}, // ProcessOverlaps 0x1c2af8c..0x1c2b1d0: behaviour lookup, CollideObjects +0x38 both ways
    {0x1c2fd20,0x330,0xce2b41b6591da21bull,GroupCore}, // phSimulator::DeleteObject(int, bool)
    {0x1c2fb94,0x18c,0xe13050367fe56b8eull,GroupCore}, // phSimulator::DeleteObjectHelper: collider = level+0x78[index], pool sim+0x40 stride 0x2f0
    {0x1c2f94c,0xec,0x4dccc893727264a7ull,GroupCore},  // phSimulator::TeleportObject
    {0x1c2fa38,0x15c,0x1b20cc64819dca7dull,GroupCore}, // phSimulator::MoveObject (an active collider is moved with it)
    {0x1c65478,0x48,0xafe354d5448cf5ceull,GroupCore},  // phCollider::SetVelocity
    {0x1c654d8,0xa8,0x00ddf6f078fda813ull,GroupCore},  // phCollider::SetAngVelocity
    {0x1c82b88,0x20,0x7e52e17b64048884ull,GroupCore},  // phInst::phInst()
    {0x1c82c64,0x7c,0x8b84fb1cdb84c11cull,GroupCore},  // phInst::Init(phArchetype&, Matrix34 const&)
    {0x1c8308c,0x10,0xa215ce03ad9c2b74ull,GroupCore},  // phInst::PrepareForActivation: this unless flag 0x8
    {0x1c83de8,0x84,0x6975b543b0502ed4ull,GroupCore},  // phArchetypePhys::phArchetypePhys(): max speed +0x44 = 500, max angular speed +0x48 = 4 pi
    {0x1c83974,0x54,0xcaa18f6d94bdb04aull,GroupCore},  // phArchetype::SetBound (bound refs +0x9c)
    {0x1c842b4,0xc0,0x8e185cd4fc4a6a74ull,GroupCore},  // phArchetypePhys::SetMass(float): bound +0x68, then arch +0x88
    {0x1c84458,0x8,0x630c6ba4f64ee74eull,GroupCore},   // phArchetypePhys::SetGravityFactor
    {0x1c83d1c,0x60,0x99e1ded388299f26ull,GroupCore},  // phArchetype::Release (refs +0x2a)
    {0x1ba2b4c,0x54,0x8052b13e65016db7ull,GroupCore},  // phBound::Release (refs +0x9c)
    {0x1bce294,0xd4,0xb321b37ace02e27bull,GroupCore},  // phBoundSphere::phBoundSphere(float)
    {0x1ba87c4,0xa4,0x9ad6da5780f5d578ull,GroupCore},  // phBoundCapsule::phBoundCapsule()
    {0x1ba893c,0x24,0x110583f956e11de8ull,GroupCore},  // phBoundCapsule::SetCapsuleSize(float, float)
    {0x1ba8b38,0x30,0xfb618e8f39480229ull,GroupCore},  // phBoundCapsule::CalculateExtents (start): the segment runs along local Y
    {0x28710d8,0x10,0x1852983f26225129ull,GroupCore},  // propPropManager::GetInstance (GOT 0x2e0c110)
    {0x2871ac4,0x144,0xd05b3f8a0e435f5full,GroupCore}, // propPropManager::GetDoor(fragInstNav*): a door only for nav+0x1be == 1
    {0x286be08,0x8,0xf2b7c489f0a3701aull,GroupCore},   // propDoorHandle::Get
    {0x286b8cc,0x10,0xfbbad732ee5e3a7dull,GroupCore},  // propDoorInst::IsClosed: state +0x58 < 2
    {0x286b30c,0x180,0x6bb426d13f8d5bbcull,GroupCore}, // propDoorInst::Open(Vector3 const&, bool): state 0 and +0x78 only
    {0x28d104c,0x58,0x67c384dbb8bee30bull,GroupCore},  // sagVehicle::PreComputeImpacts 0x28d104c..0x28d10a4: the door precedent
    {0x286fe04,0x8,0x8a191955c9462e80ull,GroupCore},   // fragInstNav::GetClassType = 0xa
    {0x28928f0,0x8,0x0767c345371752dbull,GroupCore},   // mvrMoverInst::GetClassType = 7
    {0x2242c60,0x8,0x1022b167e8fa67c0ull,GroupCore},   // rdrDeadBodyInst::GetClassType = 0xc
    {0x227385c,0x140,0x6bd9b9d773a854c2ull,GroupCore}, // rdrPhysicsAudioDispatch::PreApplyImpacts: ud+0x1a 0xffff -> kind -> class mask 0x3f70 -> ret
    {0x2273ed0,0x34,0x667941721f12da7aull,GroupCore},  // rdrPhysicsAudioDispatch::GetPhysicsAudioEntity(phInst*): slot 0xffff -> 0
    {0x28abbd4,0x90,0xe2eeffb4b012914cull,GroupCore},  // sagSimulator::PreApplyImpacts_Game: [it+0x10], vtable +0xd8, audio CB +0x18
    {0x28abb40,0x94,0xd9e6a2bd2b505d1dull,GroupCore},  // sagSimulator::sagSimulator: PreApply functor = PreApplyImpacts_Game, thunk GOT 0x2de20c0
    {0x1c287cc,0x10,0xb3ae0f606574e3c7ull,GroupCore},  // phSimulator::SetPreApplyImpactsFunc: sim+0xe8..+0x107
    {0x1c3111c,0x6c,0x9c686bbd507dd023ull,GroupCore},  // FunctionTranslator1<Iterator>::thunk: fn = [functor+0], x0 = iterator copy
    {0x1c38d24,0x3e0,0x13f12677f91a319eull,GroupCore}, // phContactMgr::PerformImpactsCallbacks: iterator +0x10 = inst, thunk [functor+0x18]
    {0x1c36d88,0x134,0x1fd8a59746cae2eeull,GroupCore}, // phContactMgr::UpdateNew: PreApply functor copied from sim+0xe8 (flag 0x2e83a6d)
    {0x1c370c0,0x168,0x62a0efab47dae798ull,GroupCore}, // phContactMgr::PreSolveCallbacks: PreCompute functor sim+0xc8 (flag 0x2e83a6c)
    {0x1c27c24,0x78,0xe3076d5963b30defull,GroupCore},  // phSimulator::DefaultPreComputeImpacts: inst vtable +0xc8
    {0x28a8eec,0x20,0x54ef62aab309e97dull,GroupCore},  // sagInstData::sagInstData(phInst*)
    {0x1c82fcc,0x8,0xef707f529cd21291ull,GroupCore},   // phInst::GetClassType = 0
    {0x1c8304c,0x4,0x2fe42bc03800fb45ull,GroupCore},   // phInst::NotifyImpulse (ret)
    {0x1c83058,0x8,0x86f1867dbd153efbull,GroupCore},   // phInst::GetNMAgentID = -1
    {0x1c83060,0x8,0x7a2d5abd09e0a71bull,GroupCore},   // phInst::ShouldFindImpacts = true
    {0x1c83078,0x4,0x2fe42bc03800fb45ull,GroupCore},   // phInst::PreComputeImpacts (ret)
    {0x1c83080,0x4,0x2fe42bc03800fb45ull,GroupCore},   // phInst::PreApplyImpacts (ret)
    {0x1c65198,0x2e0,0x74c02cfb9c21cd2cull,GroupCore}, // phCollider::UpdateImp: position += v dt (+0x100) before the collision
    {0x1c2e5b8,0x37c,0xa7a9bbe3dd79950aull,GroupCore}, // phSimulator::PostCollide: SetVelocity(col, col+0x110), Move
    {0x1c2928c,0x364,0xe8f63a7b98d91aadull,GroupCore}, // phSimulator::PreCollide: UpdateOneCollider
    {0x1c3f60c,0x2c8,0x5def57468a5a5e85ull,GroupCore}, // ApplyImpulseAndPushMovAndFix: no contact with depth < 0
    // PALM
    {0x1ba44d4,0x320,0xee023df2eea33c0full,GroupPalm}, // phBoundBox::phBoundBox(short*, Vector3 const&): verts +0xe0 = the buffer, polygons inline
    {0x1ba4954,0x39c,0x14939999cd70bb26ull,GroupPalm}, // phBoundBox::SetBoxSize(Vector3 const&): full size +0x110
    {0x1baa394,0x50,0xe8e6285860a60295ull,GroupPalm},  // phBoundComposite::phBoundComposite()
    {0x1baa3e4,0x174,0x18ec451cadfea4a5ull,GroupPalm}, // phBoundComposite::Init(int, bool): +0xa0 parts, +0xa8 matrices (= +0xb0), +0xb8, +0xd0 / +0xd2
    {0x1baa61c,0xac,0xc09249a91e2483aeull,GroupPalm},  // phBoundComposite::SetBound(int, phBound*, bool)
    {0x1bab1b8,0x230,0xd925fc06ec3f6573ull,GroupPalm}, // phBoundComposite::CalculateCompositeExtents(bool)
    {0x1c84510,0x78,0xcde8bf4a38ce2b8dull,GroupPalm},  // phArchetypePhys::SetAngInertia(Vector3 const&): +0x50, 1/I +0x60
    {0x1ba47f4,0x160,0x0faf7a6dabb697eaull,GroupPalm}, // phBoundBox::SetBoxVertices
    {0x1ba58e0,0x1e4,0x27a2d2430b6f9d9cull,GroupPalm}, // phBoundBox::CalculateExtents
    {0x1ba5548,0x80,0x93eaa502dc260a8eull,GroupPalm},  // phBoundBox::GetComputeAngularInertia
    {0x1babb78,0xc0,0xafe289db378313d0ull,GroupPalm},  // phBoundComposite::GetComputeAngularInertia
    {0x1babd68,0x50,0x70b7d41f3832bf69ull,GroupPalm},  // phBoundComposite::AllocateTypeAndIncludeFlags: n x {type, include} at +0xc0 / +0xc8
    {0x1c6e564,0x1d9c,0xb45ea84c58e8aa4aull,GroupPalm},// ComputeCollision: a part whose flags AND to 0 on either side is skipped
    {0x1ba2a78,0x9c,0x5e32ec5ee27303daull,GroupPalm},  // phBound::phBound()
    {0x1ba2d94,0x3c,0xfdde0bf479b0e912ull,GroupPalm},  // phBound::SetCGOffset (+0x70; the composite never writes it)
    // ACT
    {0x2acc4b0,0x48,0x10921f46263a8566ull,GroupAct},   // sagWorldScript::ForceActivation(fragInst*)
    {0x1e97ef4,0x54,0x23cb4aad410a1c78ull,GroupAct},   // fragInst::IsActivationDisabled
    {0x1e97ed4,0x20,0x77cd8ccf370c77b7ull,GroupAct},   // fragInst::SetDisableActivation: +0x80 bit 8 set, bit 7 = the value
    {0x1c2c864,0x18,0x97ca24ce09505e3bull,GroupAct},   // phSimulator::ActivateObject(phInst*, ...): index = inst+0x10
    {0x1c300b4,0x250,0x529c6b207a0d1488ull,GroupAct},  // phSimulator::ActivateObject(int, ...): state 1, active +0x14 < cap +0xc
    {0x2873bc0,0x580,0xeacdf61b16cb3f86ull,GroupAct},  // sagFragInst::PrepareForActivation
    {0x1ea1ef8,0x1c0,0x09ad383fdd45e9d6ull,GroupAct},  // fragInst::PrepareForActivation: null while disabled by the type
    {0x286f294,0x7c,0xb4259714e2bca842ull,GroupAct},   // fragInstNav::PrepareForActivation
    {0x22405bc,0x78,0x0a8fda1d4c0a3361ull,GroupAct},   // rdrDeadBodyInst::PrepareForActivation
    {0x1c29698,0x730,0x36b4d31fbda07fb3ull,GroupAct},  // phSimulator::CollideActive
    {0x1e98274,0xc,0x34477dfc67a5efa5ull,GroupAct},    // fragInst::GetHasInfiniteMassVsMovers: +0x82 bit 7
    {0x2536648,0xa2c,0x0df4f0906e05bedcull,GroupAct},  // phInstBehaviorSagExplosion::CollideObjects: the unlock precedent 0x25367a4..0x25367c8
    // DEACT
    {0x28abc64,0x17c,0x9ac5de509d60d6d1ull,GroupDeact},// sagSimulator::PreCollide: slow objects deactivated over 200 active
    {0x1c303cc,0xdc,0xb1372213331c7bb2ull,GroupDeact}, // phSimulator::DeactivateObject(int): inst +0x100 first
    {0x1c30304,0xc8,0xc25782082e56ec8dull,GroupDeact}, // phSimulator::DeactivateObject(int, phInst*)
    {0x1c304a8,0xcc,0xa94b22ff8c2dc1b0ull,GroupDeact}, // phSimulator::DeactivateObject(phInst*)
    {0x1c830a0,0x8,0xeacbcb54a8a83ddbull,GroupDeact},  // phInst::PrepareForDeactivation = managed
    // DOORX
    {0x286b228,0xe4,0x8b9e0dea19639943ull,GroupDoorx}, // propDoorInst::Close(bool): state 4 only
    {0x286a368,0x10,0x5a1b15efb2d2e4e9ull,GroupDoorx}, // propDoorInst::IsLocked: state 1
    {0x286b8dc,0x10,0x0b1e52f43c4fe4e5ull,GroupDoorx}, // propDoorInst::IsOpened: state 4
    // CCD
    {0x1c29dc8,0x5f4,0xee81fb241f7e2970ull,GroupCcd},  // phSimulator::CollisionIntersection: CCD for an instance with flag +0x12 bit 0x2
    // REACH
    {0x2870dd0,0x200,0xf1f3d0b74c2a4527ull,GroupReach},// propPropIterator::StartCullSphere(Vector3 const&, float)
    {0x2870dc0,0xc,0xb77333e773dfab76ull,GroupReach},  // propPropIterator::propPropIterator(): +0x400 = -1, +0x404 = 0
    {0x1c24254,0x180,0x433876e617d45369ull,GroupReach},// phLevelNew::GetFirstCulledObject
    {0x1c243d4,0x348,0x20ca03e2a52881cdull,GroupReach},// phLevelNew::GetNextCulledObject
    {0x1c25aa8,0x248,0xd8dbcc416c9a13feull,GroupReach},// phLevelNew::CachedObjectDataCheckInstance: object +0x0 centre / radius, +0x20 state
    {0x1c27080,0xa0,0x0db1862178d2dc7full,GroupReach}, // phMultiReaderLockToken::WaitAsReader (re-entrant per thread)
    {0x1c80c88,0x4,0x2fe42bc03800fb45ull,GroupReach},  // phIterator::Clear (ret)
    // BUMP
    {0x1e48bb8,0x58,0x9e2273d631845ac5ull,GroupBump},  // aActor::Broadcast(mvrMsgBumpedInto const&): mind +0x28, animator +0x38, behaviour +0x40
    {0x1e49f44,0xe4,0x85f01b5ca33cecb2ull,GroupBump},  // aiMindComponent::HandleBumpedInto: slot [GOT 0x2deabb0] + (guid & 0xffff) * 16
    {0x1e189cc,0x108,0xaabd682afc706f4aull,GroupBump}, // animAnimatorComponent::HandleBumpedIntoMessage
    {0x201699c,0x4,0x2fe42bc03800fb45ull,GroupBump},   // bhBehaviorComponent::HandleBumpedIntoMessage (ret)
    {0x2912184,0xf8,0x75f04d53866b8d8full,GroupBump},  // entEntityInst::PrepareForActivation: null for a live creature
    {0x1e1698c,0x38,0x3b33ce2af0573a64ull,GroupBump},  // animAnimatorComponent::AfterActionTreeUpdate: bump time +0xb0c builds per update
    // SWING
    {0x286acc4,0x44c,0x522aa9111eebc108ull,GroupSwing},// propDoorInst::Update: states 2/3 vtable +0x10, 5 +0x30; t >= 0.8 -> 3; false -> 4 / 0
    {0x286a794,0xcc,0x2fe840ad11ca536bull,GroupSwing}, // propDoorInst::FastClose(bool): not state 1; state 0, OnDoorClosed, vtable +0x20
    {0x2a773bc,0x3c,0xb465d51d0041133bull,GroupSwing}, // PivotDoorInterface::ContinueOpenDerived: t = min(t + speed dt, 1)
    {0x2a774a0,0x3c,0x18e9aef4bc33e49bull,GroupSwing}, // PivotDoorInterface::ContinueCloseDerived: t = max(t - speed dt, 0)
    {0x2a7710c,0x1ec,0x8ba0c577109e397full,GroupSwing},// PivotDoorInterface::ContinueActionShared: ease, cap +0x28[dir], Rotate, SetCurrentMatrix
    {0x2a772f8,0xc4,0x6a4bd061459b8594ull,GroupSwing}, // PivotDoorInterface::StartOpenDerived: +0x20 speed, +0x10 axis (t not reset)
    {0x2a77490,0x10,0x7db87bc5729a3e14ull,GroupSwing}, // PivotDoorInterface::StartCloseDerived: +0x20 speed
    {0x2a773f8,0x98,0x84b53727920ac557ull,GroupSwing}, // PivotDoorInterface::SetClosedDerived
    {0x2a76f24,0x38,0x2da0fa77d26c0108ull,GroupSwing}, // PivotDoorInterface(): +0x8 1, +0x20 speed 1, +0x24 t 0, +0x28 / +0x2c cap -1
    {0x2a774dc,0x8,0xded0dfa5aa2f08aeull,GroupSwing},  // PivotDoorInterface::SetActionSpeedScale: +0x20
    {0x286c14c,0xc8,0xa46bae5d07d170cbull,GroupSwing}, // propDoorManager::Update
    {0x286a91c,0x100,0x613a61eaf91bd913ull,GroupSwing},// propDoorInst::Init: frag +0x78, closed matrix +0x0..+0x30, iface +0x50, state +0x58
    {0x286abdc,0x18,0xd8818780b8d86f97ull,GroupSwing}, // propDoorInst::GetPosition: frag +0x50
    {0x286aa90,0x8c,0x8b5cc6a9d9801326ull,GroupSwing}, // propDoorInst::SetCurrentMatrix: TeleportObject + AddMovingInstance
    // REST (also needs SWING and DOORX)
    {0x286a120,0x3c,0x44625be3f8b238d1ull,GroupRest},  // propDoorInst::SetState(state, bool): SetAudioState(new, old), +0xa1 = bool, +0x58 = new
    {0x286a15c,0x1bc,0x18fddd92b478df97ull,GroupRest}, // propDoorInst::SetAudioState: 4 <- 2 stops the loop, one-shot +0x10
    {0x286e3e4,0x218,0x492abe74c65f6197ull,GroupRest}, // fragInstNav::UpdatePathObjectState
    {0x286b8fc,0x10,0x10982cc5826d1979ull,GroupRest},  // propDoorInst::IsClosing: state 5 (smoothstep in ContinueActionShared)
    {0x286b8ec,0x10,0xa3f01f9be5c9230dull,GroupRest},  // propDoorInst::IsOpening
    {0x286abf4,0x28,0xf3b0c4edb59beb29ull,GroupRest},  // propDoorInst::UseDelayDoorExecute: +0xa0 / +0x84 / +0x88, clock +0x3c
    {0x1fb0884,0x2ac,0x276fb7b05f75eb6cull,GroupRest}, // rage::Matrix34::Rotate (ContinueActionShared)
    {0x286a5a0,0x1ac,0x1516b8474a622ecfull,GroupRest}, // propDoorInst::ReSyncDoorState (witness)
    // RELEASE (also needs ACT): who sets / clears fragInst +0x80 bit 1
    {0x1ea3708,0x20c,0x6976054abb75c64bull,GroupRelease}, // fragInst::ReportTypePagedIn: bit 1 when type+0x1c0 == 0 or type bit 1
    {0x1ea3bac,0x230,0x3c244df67145bb38ull,GroupRelease}, // fragInst::LostCacheEntry: bit 1 cleared again
    {0x1ea31d4,0x4d0,0x2f077c0b2d6a8d52ull,GroupRelease}, // fragInst::BreakApart(phBreakData const&): orr #0x2, ActivateObject
    // TOUCH (VrMelee's open-hand slap / shove / press)
    {0x288aa2c,0x228,0x6a119c32c4a4782full,GroupTouch}, // mvrMoverComponent::ApplyTranslationOffset(Vector3 const&, bool)
    {0x1e1a378,0x24,0xf2e80d5b0b82101eull,GroupTouch},  // animAnimatorComponent::ApplyMoverRotation(float): mover +0xb3c
    {0x1e49eb8,0x8c,0x048bd24a34d57340ull,GroupTouch},  // aiMindComponent::HandleInjure: ReportWasInjured only for damage > 0
    {0x1e4d49c,0x124,0x85a20b67bbea77e9ull,GroupTouch}, // aiMind::ReportWasHitMelee: guid bound and generation checked
    {0x1e4d5c0,0x13c,0xedd727cc0b11d102ull,GroupTouch}, // aiMind::ReportWasInjured
    {0x1e58eb4,0x134,0xe2d74df87212563eull,GroupTouch}, // sagPhysicalComponent::HandleInjure: guid checked
    {0x1e182ec,0x520,0x0267317dbf956221ull,GroupTouch}, // animAnimatorComponent::NotifyInjured: guid checked
    {0x1e82864,0x7d4,0xe4207c13a9c927e2ull,GroupTouch}, // hlthHealthComponent::Injure: generation checked, NULL on a mismatch
    {0x1e8429c,0xc98,0x6dedf88f31dccd6eull,GroupTouch}, // hlthHealthComponent::HandleInjure: invulnerable returns, KO <= 100 no kill
    {0x1e3dacc,0x8,0xf6eb23c1b33b8c1eull,GroupTouch},   // aniMelee::ReceiveStrike: +0x550
    {0x2892d58,0xb8,0xe21f79bb2aa91bacull,GroupTouch},  // mvrMoverSimComponent::PerformPhysicalUpdate: +0xb8c != 0 -> Teleport, SetTranslationOffset(0)
    {0x2892d28,0x30,0x542c9d6dd834c52full,GroupTouch},  // mvrMoverSimComponent::Update: deferred to PerformPhysicalUpdate
    {0x2888d84,0x110,0x4184957c05d84e9eull,GroupTouch}, // mvrMoverComponent::GetDesiredMotion (head): +0x860..+0x868 added to the motion
    // GRAB
    {0x1c35b74,0x2dc,0x462e982fdfe4dd45ull,GroupGrab}, // phContactMgr::AttachObjects(tag, posA, posB, instA, instB, compA, compB, maxSep, bool, constraint*)
    {0x1c353d8,0xf4,0xc44c18cf5f5ad9c1ull,GroupGrab},  // phContactMgr::ReleaseConstraint(phConstraint*)
    {0x1c35290,0x148,0x15f6e5579f2dc1ebull,GroupGrab}, // phContactMgr::DeactivateConstraintInternal
    {0x1c36a38,0x18c,0xb277778f7d3dc620ull,GroupGrab}, // phContactMgr::DeleteObject
    {0x1c77c9c,0x1e8,0x7b06d731dce9773eull,GroupGrab}, // phConstraint::AttachObjects: inst +0x8 / +0x10, comp +0xb0 / +0xb4, maxSep +0x90
    {0x1c777e4,0x8c,0x9c70318528ae3a2full,GroupGrab},  // phConstraint::phConstraint
    {0x1c7ba58,0x384,0x7460a823a86b8fa9ull,GroupGrab}, // phConstraint::SetFixedRotation()
    {0x1c37f10,0xb4,0x87ae808dbba23f07ull,GroupGrab},  // phContactMgr::BreakConstraints
    {0x1c77948,0x16c,0x85ef24abf7ce09c4ull,GroupGrab}, // phConstraint::DisableConstraint: +0x141
    {0x268d850,0x14c,0xb43fbe058a27bcf9ull,GroupGrab}, // invLassoWeapon::BuildConstraintToInstance: the AttachObjects precedent, mgr = [sim+0x10]
    {0x2240ee4,0x280,0x9515c5eb53fc1730ull,GroupGrab}, // rdrDeadBodyInst::SetConstraintFromComponents
    {0x1c66d80,0x94,0x766e14e9f191ec0bull,GroupGrab},  // phCollider::SetInertia(float const& mass, float32x4 I): +0xa0 (I, mass), +0xb0 (1 / x)
    // UNFREEZE (also needs ACT): the never-activate flag, who sets it, the stock release of it, the bound and mass layout read
    {0x2160e0c,0x27c,0x52752a6df3ff9707ull,GroupUnfreeze}, // aniAnimProp::ReleaseProp: minMove < 0 refused, +0x12 bit 3 cleared, unlock, +0x80 |= 2, ActivateObject
    {0x1e96528,0x1f0,0xdf631e5d83fbb8c4ull,GroupUnfreeze}, // fragInst::Insert: type +0x130 vtable +0xd0 false -> SetInstFlag(8, true)
    {0x1bac48c,0x78,0x31b64612ffa73004ull,GroupUnfreeze},  // phBoundComposite::CanBecomeActive: +0xd4, parts +0xa0, count +0xd2
    {0x1baa61c,0x54,0x97242d7dba779d52ull,GroupUnfreeze},  // phBoundComposite::SetBound (head): a part of type 0xa (BVH) -> +0xd4
    {0x1c842b4,0x38,0x9466d0f3d7c90f55ull,GroupUnfreeze},  // phArchetypePhys::SetMass (head): mass / 1 / mass at +0x38 / +0x3c
    {0x244c6c0,0x90,0x16572700d97de1c2ull,GroupUnfreeze},  // rdr2scripting::prop::IsPropFixed: type +0x1c0 < 0, level state 2, +0x12 bit 3
    {0x244c750,0x14,0xa70dd056acbd38f0ull,GroupUnfreeze},  // rdr2scripting::prop::SetPropFixed: SetInstFlag(8, b) only (the restore)
    {0x1c23ae4,0x3c,0x05037e6e9059de09ull,GroupUnfreeze},  // phLevelNew::CanBecomeActive(int): level state 2 or +0x12 bit 3 -> false
    {0x24c94cc,0xe8,0xed37f814093cd882ull,GroupUnfreeze},  // gohProp::InitFragInst: fragInstNav, sagInstDataProp user data, gohProp +0xe0
    {0x24c9824,0x2c,0x03faafabbe64a46bull,GroupUnfreeze},  // gohProp::OnCreate +0x130: SetInstFlag(8, gohProp +0xf0 & 1)
    {0x1c82ce0,0x24,0x920996da5ba71961ull,GroupUnfreeze},  // phInst::SetInstFlag: a plain or / bic on +0x12
    {0x1ba5b2c,0x8,0x7a2d5abd09e0a71bull,GroupUnfreeze},   // phBoundBox::CanBecomeActive = true
    {0x1bce854,0x8,0x7a2d5abd09e0a71bull,GroupUnfreeze},   // phBoundSphere::CanBecomeActive = true
    {0x1ba9a90,0x8,0x7a2d5abd09e0a71bull,GroupUnfreeze},   // phBoundCapsule::CanBecomeActive = true
    {0x1bd2bc0,0x8,0x7a2d5abd09e0a71bull,GroupUnfreeze},   // phBoundTaperedCapsule::CanBecomeActive = true
    {0x1bb73a4,0x8,0x7a2d5abd09e0a71bull,GroupUnfreeze},   // phBoundGeometry::CanBecomeActive = true
    {0x1c66550,0xb4,0x979e43f7a2fbafafull,GroupUnfreeze},  // phCollider::Init: mass / inertia copied from [inst+0x8] to +0xa0 / +0xac / +0xb0 / +0xbc
    {0x1c66d80,0x94,0x766e14e9f191ec0bull,GroupUnfreeze},  // phCollider::SetInertia (one collider; the archetype is never written)
    // DOORGATE: the player's walk-in open (the FindDoor hook) and the counting hooks of Open / Close (also needs REST)
    {0x26af868,0x1a8,0x497b2d5e4bfe4450ull,GroupDoorGate}, // sagPlayerInputDoor::FindDoor(handle x1, bool& x2, sagActor& x3): true = a door VerifyDoor passed
    {0x26af554,0x314,0xe4971e616228d2c6ull,GroupDoorGate}, // AddUseContext: bl FindDoor 0x26af5d4, false -> 0x26af680; mode word [[[a+0x38]+0xab8]+0x18]+0x18
    {0x26aecd0,0x180,0x741c2a07a8c64157ull,GroupDoorGate}, // VerifyDoor (head): the same mode word loads 0x26aee34..0x26aee44
    {0x2c95f90,0x10,0xbe48da40c5f8356bull,GroupDoorGate},  // FindDoor PLT: GOT 0x2e53628
    {0x2c7f8b0,0x10,0xf2e7c25e3bf3141aull,GroupDoorGate},  // propDoorInst::Open PLT: GOT 0x2e482b8
    {0x2c7f8a0,0x10,0xc1764b9a51580c06ull,GroupDoorGate},  // propDoorInst::Close PLT: GOT 0x2e482b0
    // GLASS
    {0x28730b0,0x1a0,0x79186f6e7abf6646ull,GroupGlass},    // sagFragInst::CollisionDamage: glass + iterator -> IsBreakable, null iterator forwards
    {0x1e9bbe0,0x1670,0x54b215a6f5e31861ull,GroupGlass},   // fragInst::CollisionDamage: +0x34 * tune+0x20 > group+0x54, health entry+0x138, glass -> ShatterGlassOnBone
    {0x1e9ad00,0xdd8,0x6a452b746dce0f7full,GroupGlass},    // fragInst::PreApplyImpacts: iterator / manifold / contact layout, tune +0x2c / +0x30 gate
    {0x2534034,0x204,0x4831ffbc75747395ull,GroupGlass},    // projProjectile::ProcessInstComponentImpulses +0x610: Collision layout, vt+0x218 with null iterator
    {0x1ec13f8,0x64,0x1356b756fa59239cull,GroupGlass},     // fragCacheEntry::ShatterGlassOnBone
    {0x2aad950,0x4c,0x5fd0726ea2601fc5ull,GroupGlass},     // ComputeGlassCrackTypeForRDR2: a null +0x40 gives 0
    {0x2874140,0x98,0xfc2c2f1e3688470dull,GroupGlass},     // sagFragInst::IsBreakable
    {0x2874984,0x8,0x88a01a86a906ce9bull,GroupGlass},      // sagFragInst::GetClassType = 5
    {0x2aad8ac,0x30,0x81ffde22ee93d716ull,GroupGlass},     // InitGlassInstForRDR2: type 0x100000
    {0x23bacac,0x70,0xec90a8643485053full,GroupGlass},     // fragTuneStruct ctor (+0x20 / +0x2c / +0x30)
    {0x1e98144,0x54,0xf6a73745a1bea5c8ull,GroupGlass},     // fragInst::IsFragDamageDelayed: +0x80 bit 21 -> bit 20, else type +0x1ba sign (fragDamageDelayed)
    // DRIVE (also needs GRAB): the force on a held prop
    {0x1c66314,0x7c,0x92849ee50626d384ull,GroupDrive},     // phCollider::ApplyForce: type +0x8 0 / 2 -> +0x150 force, +0x160 torque about +0x100; 1 / 3 articulated
    {0x1c63324,0x124,0x4cf42104fd577fb4ull,GroupDrive},    // phArticulatedCollider::ApplyForceImp: the instance's vtable +0x98 first, the link of the component ([+0x370][comp])
    {0x1e9ac88,0x78,0x10814f0ba2bb6b90ull,GroupDrive},     // fragInst::NotifyForce (inst vtable +0x98): NotifyImpulse (vtable +0x90) with force / 30
    {0x1e9aa54,0x234,0xdd7a8321b01b2a17ull,GroupDrive},    // fragInst::NotifyImpulse: vtable +0x218 CollisionDamage (Collision +0x50 = 0), +0x12 bit 8 -> cloth impulse
    {0x1c67b80,0x4c,0x94d9309807e72e7cull,GroupDrive},     // phCollider::SetColliderMatrixFromInstance: rows +0xd0 / +0xe0 / +0xf0 = the instance's, +0x100 = its centre of mass
    // GROUND (reads and a write of the held prop's own accumulators): the player's mover queues its weight on the instance it stands on
    {0x288f7f0,0x80,0x9f9ff8157c129cdcull,GroupGround},    // mvrMoverInst::Update +0x1194: ground index +0x6c0 (0xffff none), generation +0x6c2 vs [level+0x68][index], IsGroundInstanceStableForForces -> InternalApplyGroundForce
    {0x288e474,0x6c,0xc00e46add9f6e5e9ull,GroupGround},    // mvrMoverInst::InternalApplyGroundForce: the same index / generation check, DeferredMoverManager::ApplyForce at +0x660, component +0x6e4
    {0x288fbb4,0x80,0x165de00c17df7c37ull,GroupGround},    // mvrMoverInst::IsGroundInstanceStableForForces: an active collider -> archetype +0x21 bit 0
    // TREE (reads only): how the creature's injury subtree picks a node (VrMelee's injuryTree dump)
    {0x28f2dd0,0x20,0x159dd06512bc55ecull,GroupTree},      // rage::ActionNodeBank::Match: the condition group at node +0x40 (count u16 +0x48), children +0x10 / +0x18
    {0x28f2e2c,0x30,0xc7b6a636c6898f9dull,GroupTree},      // ActionNodeBank::Match: child type byte +0x2f (3 bank, 4 playable, else the reference at +0x48)
    {0x2901054,0x14,0xf0f9f71efec2723dull,GroupTree},      // rage::ConditionGroup::Match: count u16 +0x8, array +0x0
    {0x29010c4,0x14,0x1b944b4a25961dbdull,GroupTree},      // ConditionGroup::Match: a condition's Match at vtable +0x68
    {0x1cce3e4,0x20,0x5a6148ce6261c8c1ull,GroupTree},      // ConditionInjuryFace::Match: condition +0x8 vs animator +0x8b8
    {0x1e89a8c,0x18,0xdc5ae5fd7492fad3ull,GroupTree},      // ConditionInjuryImpulseMag::Match: |NMState +0x30|^2 against condition +0xc^2, operator +0x8
    // PICK (VrMelee's knockdown behaviour pick: a write of ConditionRandom's shared roll, words below): the reuse / draw / match of the roll,
    // NMShot's bullet test, the centre-of-mass velocity staggerFall leans along
    {0x2900cb0,0x28,0x9ea047c721b7c805ull,GroupPick},      // rage::ConditionRandom::Match +0x10: the 0xdead guard, mode +0x8 (2 = the execute-mode path)
    {0x2900cd8,0x20,0x87125efd567651c9ull,GroupPick},      // ConditionRandom::Match +0x38: mode 0 -> GOT 0x2e0da38 (Used) / 0x2e0da40 (Num), Used 0 = reuse Num
    {0x2900dec,0x2c,0x783fcc8b953c6a49ull,GroupPick},      // ConditionRandom::Match +0x14c: Num % (+0x14) == (+0x10) -> Used = 1
    {0x1fa4a78,0x2c,0x909724fbbbcaca54ull,GroupPick},      // NmRsCBUShot::newHit_entry +0x1bc: dot(char +0x60, bullet)
    {0x1fa4aa4,0x50,0x4547d4507be6bcbcull,GroupPick},      // NmRsCBUShot::newHit_entry +0x1e8: <= 0 on one of the 5 parts at +0x2a0 -> shotFromBehind +0x47c
    {0x1f2b854,0x28,0xd833155c09ef45eaull,GroupPick},      // NmRsCharacter::calculateCoMValues +0xac: the COM velocity +0xb0 staggerFall's tick reads
    // TOUGH: the tough actor's bit (health +0x150 bit 0x8000 = +0x151 bit 7) as the meleehit subtree reads and the scripts write it
    {0x1ccec48,0x20,0xd389332f88f5e82eull,GroupTough},     // ConditionInjuryToughActor::Match: [[ctx+0x28]+0x60] +0x151 >> 7
    {0x1ccee48,0x48,0x6531649f639806b3ull,GroupTough},     // ConditionInjuryToughActorHits::Match: bit 7 clear -> 1, else the hit count compare
    {0x1cab354,0x2c,0x78629c8eff90675dull,GroupTough},     // sagActorScript::SetToughActor: 32-bit read-modify-write of health +0x150, bit 0x8000
    // PICKFN: ConditionRandom's draws through sm_RandFunc(ctx) and the g_ReplayRand step pickFnDraw repeats
    {0x2900ca0,0x188,0x4a910c9dd919276aull,GroupPickFn},   // rage::ConditionRandom::Match (both draw paths, the replay multiply-with-carry)
    {0x2900c60,0x40,0x5f8a260d9be1b823ull,GroupPickFn},    // rage::ConditionRandom::GetRandInt: sm_RandFunc(ctx) else the same step
    // BLOOD: the punch blood drop played on a bare hand that broke a pane (glassBlood; its failure leaves GLASS on)
    {0x295f7e0,0x238,0x1ac4d47c4ec3917bull,GroupBlood},    // CPtFxMelee::TriggerPtFxMelee: cull, GetEffectInst, offset matrix, size / wet, matrix pointer, +0x240 velocity, owner, Start
    {0x295fa18,0x84,0x2ddfd87672fe1716ull,GroupBlood},     // CPtFxMelee::TriggerPtFxPushPunchKick: hash 0xd3cd9f0e ("blood_push"), fx2 0 for race 0
    {0x1e43718,0x288,0xca9180877421f399ull,GroupBlood},    // aniMelee::DoPunchFX: an NPC victim -> PushPunchKick(bone matrix, local pos / n, unit dir x 3)
    {0x2942f48,0xe0,0xb778ab4714369421ull,GroupBlood},     // CPtFxManager::GetEffectInst: null when disabled / paused / noptfx / nofx (rmPtfx locks)
    {0x293d1a4,0x54,0xf86a7aecb9cf4fc4ull,GroupBlood},     // CPtFxAttach::SetMatrixPointerOwner: the owner is a key only
    {0x29662f0,0x198,0x5042ba3fbdee615dull,GroupBlood},    // CVfxHelper::CreateMatFromVecZ
    {0x2943028,0x3c,0xac4e25f44c01fba6ull,GroupBlood},     // CPtFxManager::GetPtxEffectRule(unsigned): no use of this, noptfx / nofx -> null
    {0x2340228,0x13c,0xa1d72a56535530adull,GroupBlood},    // rmPtfxManager::GetPtxEffectRule: binary searches, no lock
    {0x1d1e8b0,0x8,0x7c4769206966c6f2ull,GroupBlood},      // sagActor::GetCreatureComponent const = [actor+0xb0]
    {0x29665ac,0x20,0x72601c513a3e6543ull,GroupBlood},     // CVfxHelper::GetSizeEvoFromRace
    // SKIN BLOOD: the bullet / knife path (CreateFxOnBulletHit 0x291c1e0, DoKnifeFX 0x1e436ac): world pos -> the component's bone -> rest pose -> arm cylinder
    {0x29151cc,0x11c,0x66dfe4f905399b11ull,GroupBlood},    // sagBloodManager::RegisterHit(actor, comp, pos, dir, type, bool): +0x210, GetActorInfo, RegisterHitCommon
    {0x29152e8,0x298,0x726cb9016bb9f60full,GroupBlood},    // RegisterHitCommon: comp 7..9 -> zone 2, 11..13 -> zone 3; bone UnTransform, rest Transform; GetTarget; AddHit
    {0x2917550,0x7ec,0x659ed071623b3d8dull,GroupBlood},    // sagBloodTarget::AddHit: +0x3dc = 0, +0x3c4++, MapLocalPosToUV, v 0..1 (type != 2), AddStain
    // DIAG (reads only): the creature's action node, the frag type's tune name, a moved nav obstacle
    {0x1e5b148,0x8,0x7c4769206966c6f2ull,GroupDiag},       // sagActorComponent::GetCurrentActionNode = [comp+0xb0]
    {0x1d1e8a8,0x8,0x7c4769206966c6f2ull,GroupDiag},       // sagActor::GetCreatureComponent = [actor+0xb0]
    {0x1efc994,0x2c,0xccfe1623abe14b76ull,GroupDiag},      // fragType::DeclareStructT<datSerializeWrite> +0x384: "m_TuneName" at fragType +0xc0
    {0x286ef00,0x38,0x528c8294838dfbbaull,GroupDiag},      // fragInstNav::ReportMovedBySim -> UpdatePathObjectState
    // SELF: a touch message whose attacker is the creature itself (refused for a zombie: HandleInjure smites a zombie attacker's victim)
    {0x1e52764,0x60,0x6426305f0f388f6aull,GroupSelf},      // facFactionMgr::IsZombie(sagActor const*)
    {0x1e52758,0xc,0x2cf221c019f4eb1full,GroupSelf},       // facFactionMgr::GetInstance = [GOT 0x2deb220]
    // DIAG (the node watch's names): a node's name through its vtable +0x10 (CName: char* +0x0, atStringHash +0x8)
    {0x28f4b44,0x8,0x72ba487958824f32ull,GroupDiag},       // ActionNodeImplementation::GetName = node + 0x30 (Playable, Bank, FileReference share it)
    {0x28f282c,0x80,0xbf0ef933fbd692baull,GroupDiag},      // ActionNodeReference::GetName: the referenced node [node+0x48]
    {0x1e5b3d4,0x28,0x00e0c599c4d62171ull,GroupDiag},      // sagActorComponent::GetCurrentActionNodeID = [[comp+0xb0] GetName() + 8]
    // NODE: an open-hand reaction played as the scripts play a node (rdr2scripting::actor::SetNodeHelper 0x2423304: Find(path, null),
    // SetActionNode): no injure message, so no aggression
    {0x1e3abac,0x10c,0x4c090b814ab38f6cull,GroupNode},     // rdrActionNodeInterface::Find(char const*, ActionNode*): null root = the lookup cache under its lock
    {0x1e5a754,0x84,0xb21fb0a6a87ae417ull,GroupNode},      // sagActorComponent::SetActionNode(ActionNode*, bool): Play(comp+0xa8); true keeps +0x5a8
    {0x28efd98,0x348,0x1b6d2ad60c4fd6fdull,GroupNode},     // ActionController::Play: lock node +0x10, re-entry +0xe0, time +0x18 = 0, counter +0x1c + 1
    // DIAG (the node watch): a node's children (u16 +0x18, array +0x10; a reference defers to [node+0x48]), the impulse NotifyInjured
    // copies to aniNMStateData +0x30 and NMShotTask's bulletVel read of it
    {0x28f2ef8,0x14,0xd11fbf336efef7efull,GroupDiag},      // rage::ActionNode::GetChildCount = u16 +0x18 (GetChild 0x28f2f00: [[node+0x10] + 8 i])
    {0x28f290c,0x30,0x4edc8d780e28c216ull,GroupDiag},      // rage::ActionNodeReference::GetChildCount: [node+0x48] vtable +0x60
    {0x1e184b0,0x28,0xe0bf84a94c94a65bull,GroupDiag},      // NotifyInjured: msg +0xa0..+0xa8 -> [[anim+0xab8]+0x88] +0x30..+0x38
    {0x22eb7cc,0x44,0x9381a07975f53528ull,GroupDiag},      // NMShotTask::Begin: BuildMsgFromHitInfo (bulletVel = aniNMStateData +0x30)
    // RESETHOLD (also needs UNFREEZE): the gringo's readiness poll that resets a moved prop, the hold of it, the counting hooks
    {0x250bd68,0x7c,0xb2b83f36da74d37bull,GroupResetHold}, // ggoComponentProp::StartPropReset(ggoGringoInstance*, gohGUID): Apply, gringo +0x48 += 1, result & 1
    {0x2c8abe0,0x10,0xb77760c48089b19eull,GroupResetHold}, // StartPropReset PLT: GOT 0x2e4dc50
    {0x250c670,0xcc,0xce706a0e72d86cdfull,GroupResetHold}, // OnMsg 0x2000: GOH slot [0x2deabe0] / [0x2deabe8], frag +0x50 vs +0xb0 over 1 cm -> StartPropReset (ra 0x250c734), false -> 0x250cc24
    {0x250caf0,0x10,0xac6ac15eda2f90a0ull,GroupResetHold}, // OnMsg: StartPropReset true -> the ready byte cleared (the path the hold avoids)
    {0x24cb18c,0xc,0xab409b0b44ab527full,GroupResetHold},  // Apply: the immediate reset (CommonReset) returns 0, the answer the hold gives
    {0x2041b20,0x8,0x7ed728625b78a3b7ull,GroupResetHold},  // ggoMsgOnIsComponentReady::GetDerivedMsgType = 0x2000
    {0x203b628,0x108,0xde7deef9ee3ee806ull,GroupResetHold},// bhHelperFunctions::HotspotCanUse: +0x48 gate, the message, the ready byte
    {0x24caa54,0xf8,0xc05a42b4b5e6c38full,GroupResetHold}, // GracefulPropReset::CommonReset(gohGUID): GOH slot, frag vt +0x168 Reset, SetInstFlag(8, f0 & 1)
    {0x24caf34,0x298,0xa334dadc50629171ull,GroupResetHold},// GracefulPropReset::Apply(gohGUID, gohGUID, atDelegate*): 3.16 cm -> CommonReset now
    {0x24cabdc,0x358,0xbaa16823cbc986f1ull,GroupResetHold},// GracefulPropReset::CB: the graceful reset (out of view, timeout)
    {0x286ee38,0xb0,0x010a41e83de8b111ull,GroupResetHold}, // fragInstNav::Reset: the matrix from +0xb0, DeactivateObject
    {0x2c894e0,0x10,0xd8afbf315fe0fa8eull,GroupResetHold}, // CommonReset PLT: GOT 0x2e4d0d0
    {0x2c89500,0x10,0x1c211cf2aaafc956ull,GroupResetHold}, // Apply PLT: GOT 0x2e4d0e0
    // KICK (VrMelee's push kick: aActor::Broadcast(projMsgApplyImpulse) 0x1e48c98 -> [actor +0x50] -> these; VrMelee prints the broadcast)
    {0x290e2fc,0x8,0xdc83895a76e7390cull,GroupKick},   // entEntityComponent::HandleApplyProjectileImpulse: the instance +0x88
    {0x2911058,0x50,0xc7e488858df3ca6eull,GroupKick},  // entEntityInst::HandleApplyProjectileImpulse: FindFreeSlot (inst +0x1a0), SetImpulse(+0x10, +0x20, +0x30, +0x34, +0x38)
    {0x2913264,0x90,0x9df2fedb54a3c9f8ull,GroupKick},  // entPendingImpulse::SetImpulse: the point to the component's frame, start counter 8
    {0x29133b0,0x100,0x25316f1f5601c873ull,GroupKick}, // entPendingImpulse::Update: applied once while the object is active (state & 3 == 0)
    {0x29134b0,0x1a4,0x38f88f7c8981bd03ull,GroupKick}, // entPendingImpulse::ApplyImpulse: NM "applyImpulse" (part, impulse, local hit) else phSimulator::ApplyImpulse
    {0x2913a64,0x90,0xdb6b6f67ef935ad8ull,GroupKick},  // entPendingImpulseMgr::FindFreeSlot: 8 slots of 0x30, in use = word +0x28 != 0
    // WELD (a fixed-rotation hold as two constraints: lever grabweld)
    {0x1c78b90,0x1b4c,0xd0d271533e773d39ull,GroupWeld}, // phConstraint::Update: the position impulse and ComputeAndApplyPushes only for (+0xbc & 3) == 2 (0x1c7922c..0x1c79344), the rotation path after
    {0x1c348cc,0x290,0xbb3e14a84dfbd5e5ull,GroupWeld},  // phContactMgr::UpdateAndAddConstraintContacts: every active constraint updated on its own
    // BLOOD ROUTE (the hand's blood drop drawn in the high-res list: lever bloodhires)
    {0x23292d8,0xc4,0x5249d9bb7443bb5eull,GroupBloodRoute},  // ptxEffectRuleStd::InitInstance: rule +0x37d -> inst +0x338
    {0x233dfb4,0xcc,0x1b10151532c5fb5eull,GroupBloodRoute},  // rmPtfxManager::ActivateInstance: head of lists[+0x338] (+0x4f0 / count +0x4f8), list count +0x30
    {0x2340528,0x13c,0x253afdb4ad7a00edull,GroupBloodRoute}, // rmPtfxManager::GetEffectInst(rule): slot +0x170 = rule, slot +0x350 = slot, inst = slot + 0x20
    {0x2942be4,0x4c,0xcf8391f9f2d39c42ull,GroupBloodRoute},  // CPtFxManager::DrawLowres: nothing drawn while +0x11cf9 is 0
    {0x2941e9c,0x240,0x7bd051f26cd4833cull,GroupBloodRoute}, // CPtFxManager::DrawHighres: list 0 (list 2 only with +0x11dce, never set)
    {0x293f4dc,0x18,0x636ba920d6e88beaull,GroupBloodRoute},  // CPtFxManager::Init: +0x11cf9 = 0
    {0x2329440,0x4,0xf3d22faa879340dbull,GroupBloodRoute},   // ptxEffectRuleStd::Start: a store to rule +0x308 (the rule's memory is written at run time)
    // BLOOD LIFE: the reads of the drop's instance while it is watched (diagnostics only; a mismatch leaves the drop itself on)
    {0x232d2b8,0x6c,0xff7022b772bbc2ebull,GroupBloodLife},   // ptxTimeLine::Draw: count u16 +0x20, events +0x18, element data [inst +0x158] + i * 0x160, Draw at +0x60
    {0x232f1f8,0xa4,0x7d1d1fe7cb6a0c17ull,GroupBloodLife},   // ptxEventEmitter::Draw: nothing drawn while element +0x28 + buffer * 4 is 0
    {0x2358e9c,0x134,0x273d61250e66696dull,GroupBloodLife},  // ptxEffectInst::UpdatePos: mode +0x14c (2: matrix pointer +0x170), world position [+0x330] +0x3b0
    {0x2329fbc,0x1c,0xf4796cd4d4d5a495ull,GroupBloodLife},   // ptxEffectRuleStd::DrawEffect +0x1c4: the rule's time line at +0x300
    // GUN GLASS: the level probe VrMelee's wall test calls (include 1, state 7)
    {0x217af74,0x124,0xb7ed5d0958ea13caull,GroupGunGlass},   // SweptSphereMultipleExcludeInsts: Capsule test, exclude list, InitSweptSphere, TestInLevel(level GOT 0x2de2030)
};
namespace off {
inline constexpr std::uintptr_t simGot=0x2de2098,simVar=0x4e6f80b0,levelGot=0x2de2030,levelVar=0x4e6f6e98,propGot=0x2e0c110,propVar=0x4e7fea50;
inline constexpr std::uintptr_t instVtableGot=0x2de1fa8,instVtable=0x2ce6830,instTypeinfo=0x2ce6950;   // vtable for rage::phInst (address point +0x10)
inline constexpr std::uintptr_t behaviourTypeinfo=0x2d0c308,behaviourTypeinfoWord=0x2d0c350+8; // typeinfo for rage::phInstBehavior, its vtable's word
inline constexpr std::uintptr_t moverSimVtable=0x2db79e0,moverInstVtable=0x2db7890+0x10;      // mvrMoverSimComponent / mvrMoverInst address points
inline constexpr std::uintptr_t matchFlagsGot=0x2de2150,matchFlags=0x2e83a6f,shouldFindGot=0x2de2180,shouldFind=0x2e83a6b,fromArrayGot=0x2de2188,fromArray=0x2e83a74,
    delaySapGot=0x2de21b0,delaySap=0x4e6f80d4,stepCountGot=0x2e04510,stepCount=0x2f84c40,solverGot=0x2e113f0,solver=0x2f84c58,
    solverMinGot=0x2e113f8,solverMin=0x2f84c44,solverMaxGot=0x2e11400,solverMax=0x2f84c48;
// impact callbacks: the functions, their GOT words, the switches PreSolveCallbacks / UpdateNew read
inline constexpr std::uintptr_t preApplyGame=0x28abbd4,defaultPreCompute=0x1c27c24,impactThunk=0x1c3111c,instDataCtor=0x28a8eec,
    instDataVtableGot=0x2e0c880,instDataVtable=0x2db8968, // vtable for sagInstData (address point +0x10)
    preApplyFnGot=0x2e0c8d0,impactThunkGot=0x2de20c0,preComputeFnGot=0x2de20b8,
    preComputeFlagGot=0x2de2220,preComputeFlag=0x2e83a6c,preApplyFlagGot=0x2de2228,preApplyFlag=0x2e83a6d;
// PALM: the box / composite vtables, the flags the box constructor reads (both 0 for a constructor without allocation)
inline constexpr std::uintptr_t boxVtableGot=0x2de1aa0,boxVtable=0x2ce21f0,compVtableGot=0x2de1b08,compVtable=0x2ce26e0,
    clothGot=0x2de1aa8,clothFlag=0x4e6eb520,resCtorGot=0x2de1a88,resCtorFlag=0x4e6eb818;
// CCD / REACH / dump: rage::g_ForceCCD, rage::g_GlobalPhysicsLock; the solver tunables (file values 0.007 / 0.2)
inline constexpr std::uintptr_t forceCcdGot=0x2de2158,forceCcd=0x4e6f80d0,physLockGot=0x2de1fd0,physLock=0x4e6f6f10,allowedPen=0x2e84560,sepBias=0x2e83a94;
// BUMP: the actor manager (slot count u16 at +0) and the actor slot table (16-byte entries: actor +0, generation u16 +8)
inline constexpr std::uintptr_t actorManagerGot=0x2deaba8,actorManager=0x4e6f6b40,actorSlotsGot=0x2deabb0,actorSlots=0x4e6f6b38;
inline constexpr std::uint32_t msgBumpedInto=0x2f;
// Function entries outside the prints' own list
inline constexpr std::uintptr_t fnBoxCtor=0x1ba44d4,fnBoxSize=0x1ba4954,fnCompCtor=0x1baa394,fnCompInit=0x1baa3e4,fnCompSetBound=0x1baa61c,
    fnCompExtents=0x1bab1b8,fnSetAngInertia=0x1c84510,fnForceActivation=0x2acc4b0,fnDoorClose=0x286b228,fnPropCull=0x2870dd0,
    fnNavClass=0x286fe04,fnDeadClass=0x2242c60,fnBroadcastBump=0x1e48bb8,fnDoorFastClose=0x286a794,fnIsActDisabled=0x1e97ef4,fnSetDisableAct=0x1e97ed4;
// stock phInst vtable slot words (address point instVtable+0x10): GetClassType +0x10, NotifyImpulse +0x90, GetNMAgentID +0xa8,
// ShouldFindImpacts +0xb0, PreComputeImpacts +0xc8, PreApplyImpacts +0xd8
struct Word {std::uintptr_t slot,target;};
// SetOpen, StartOpen, ContinueOpen, GetDoorVector, SetClosed, StartClose, ContinueClose, SetActionSpeedScale (relocated words)
inline constexpr Word pivotSlots[]{{0x2dd37d0,0x2a770b4},{0x2dd37d8,0x2a772f8},{0x2dd37e0,0x2a773bc},{0x2dd37e8,0x2a77054},
    {0x2dd37f0,0x2a773f8},{0x2dd37f8,0x2a77490},{0x2dd3800,0x2a774a0},{0x2dd3808,0x2a774dc}};
inline constexpr Word instSlots[]{{0x2ce6850,0x1c82fcc},{0x2ce68d0,0x1c8304c},{0x2ce68e8,0x1c83058},{0x2ce68f0,0x1c83060},{0x2ce6908,0x1c83078},{0x2ce6918,0x1c83080}};
// The 34 slots of the stock phInst vtable, +0x00..+0x108 (DEACT: all of them must match before the copy is used)
inline constexpr std::uintptr_t instVtableSlots[34]{
    0x1c82ba8,0x1c82c40,0x1c82fcc,0x1c82fd4,0x1c82fd8,0x1c82fdc,0x1c82fe0,0x1c82da0,0x1c82dac,0x1c82db8,0x1c83034,0x1c8303c,0x1c82d14,0x1c82d20,
    0x1c82d3c,0x1c83044,0x1c83048,0x1c82d48,0x1c8304c,0x1c83050,0x1c83054,0x1c83058,0x1c83060,0x1c83068,0x1c83070,0x1c83078,0x1c8307c,0x1c83080,
    0x1c83084,0x1c83088,0x1c8308c,0x1c8309c,0x1c830a0,0x1c82e98};
// phSimulator functors (rage::Functor1, 0x20 bytes: fn +0x0, callee +0x10, thunk +0x18; +0x8 is never read by the thunk)
inline constexpr std::size_t simPreCompute=0xc8,simPreApply=0xe8,fnCallee=0x10,fnThunk=0x18,iterInst=0x10;
// sagInstData: vptr +0x0, +0x8 = 0, inst +0x10, u32 +0x18 = kind byte 0xff | audio slot u16 0xffff << 16 (+0x1a); +0x20 not written by the ctor
inline constexpr std::uint32_t userKindSlot=0xffff00ffu;inline constexpr std::size_t userBytes=0x30;
// phSimulator
inline constexpr std::size_t simLevel=0x8,simPoolCap=0x34,simPoolUsed=0x38,simBehList=0x60,simBehDirty=0x70,simBehCount=0x72,simBehCap=0x74,simBehSlots=0x78;
// phLevelNew: active cap +0xc / active count +0x14 (u16), object count +0xa (u16), objects +0x60 (stride 0x30: +0x0 centre / radius,
// +0x10 include, +0x14 type, +0x20 inst | state), active colliders +0x78
inline constexpr std::size_t levelCap=0xc,levelActive=0x14,levelCount=0xa,levelObjects=0x60,levelColliders=0x78,objStride=0x30,objInclude=0x10,objType=0x14,objInst=0x20;
// phInst / phArchetype / phBound / phCollider
inline constexpr std::size_t instArch=0x8,instIndex=0x10,instFlags=0x12,instUser=0x18,instMatrix=0x20,instAxisY=0x30,instPos=0x50;
inline constexpr std::size_t archType=0x20,archInclude=0x24,archRefs=0x2a,archMaxAngSpeed=0x48,boundRefs=0x9c,boundRadius=0x20,boundMargin=0x90;
inline constexpr std::size_t colliderInst=0x10,colliderPos=0x100,colliderVel=0x110,colliderRows=0xd0,colliderAngVel=0x120;
inline constexpr std::size_t compParts=0xa0,compMatrices=0xa8,compLastMatrices=0xb0,compFlags=0xc0,compLastFlags=0xc8,compCount=0xd0;
inline constexpr std::uint16_t instHasBehaviour=0x100,instCcd=0x2,noIndex=0xffff;
// fragInst: +0x82 bit 7 = infinite mass vs movers; fragInstNav +0x1be == 1 = a door; propDoorInst state +0x58
inline constexpr std::size_t fragInfinite=0x82,navDoor=0x1be,doorState=0x58;
// fragInst activation flags +0x80 (bit 8 override, bit 7 disabled), the type words IsActivationDisabled reads (+0x78 resident, +0x98),
// the type's +0x1ba; the archetype's bound +0x18 and its box (max +0x30, min +0x40)
inline constexpr std::size_t fragF80=0x80,fragType78=0x78,fragType98=0x98,fragTypeBits=0x1ba,fragCache=0x88,fragOther1c0=0x1c0,
    archBound=0x18,boundMax=0x30,boundMin=0x40;
// propDoorInst: the interface +0x50, the frag +0x78; PivotDoorInterface: side byte +0x8 (the open direction), speed +0x20, phase +0x24,
// cap +0x28[dir]
inline constexpr std::size_t doorIface=0x50,doorFrag=0x78,ifSide=0x8,ifSpeed=0x20,ifPhase=0x24,ifCap=0x28;
// the vtables (GOT 0x2e10f30 -> 0x2dd37c0, GOT 0x2e10f58 -> 0x2dd3860; address points +0x10) and the eight PivotDoorInterface slots
inline constexpr std::uintptr_t pivotGot=0x2e10f30,pivotVtable=0x2dd37c0,pivotVptr=0x2dd37d0,animGot=0x2e10f58,animVtable=0x2dd3860,animVptr=0x2dd3870;
// REST: propDoorInst +0xa0 (use pending), +0xa1 (state change notice flag), +0x84 (auto-close time); the clock [[GOT 0x2ddf340 =
// sysTimeManager::sm_GlobalManager]] + 0x3c; sm_OnDoorStateChange and camManager::sm_Instance (Update reads them: words only)
inline constexpr std::size_t doorUse=0xa0,doorNotice=0xa1,doorCloseAt=0x84,clockSeconds=0x3c;
inline constexpr std::uintptr_t clockGot=0x2ddf340,clockVar=0x4e6d6510,doorNoticeGot=0x2e09860,doorNoticeVar=0x4e7fe758,camGot=0x2deac20,camVar=0x4e6ecff8;
inline constexpr std::uintptr_t fnDoorSetState=0x286a120,fnPathState=0x286e3e4,fnContinueShared=0x2a7710c;
inline constexpr float doorRestDelayS=10.f,doorCloseSpeed=2.f; // the engine's own 2 -> 4 (clock + 10) and Close (+0x80 = 2.0)
inline constexpr std::uint32_t doorStateOpened=4,doorStateClosing=5;
// TOUCH: mvrMoverComponent / animAnimatorComponent vtables (address points +0x10)
inline constexpr std::uintptr_t moverCompGot=0x2e0c448,moverCompVtable=0x2db77d8,animCompGot=0x2defba0,animCompVtable=0x2d066f8,
    fnTranslate=0x288aa2c,fnMoverRotation=0x1e1a378,moverSimGot=0x2e0c468,moverSimSymbol=0x2db79d0;
// GRAB: the contact manager [sim+0x10]; phConstraint: inst A +0x8 / B +0x10, manifold +0x18, comp A +0xb0, disabled +0x141; phCollider
// +0xa0 = inertia (xyz) and mass (w); the bound type byte +0x1c (0xc = composite)
inline constexpr std::size_t simContactMgr=0x10,conInstA=0x8,conInstB=0x10,conManifold=0x18,conCompA=0xb0,conDisabled=0x141,colliderInertia=0xa0,boundType=0x1c;
inline constexpr std::size_t conFlags=0xbc; // phConstraint flags (u16): 2 after the constructor, SetFixedRotation ORs in 3
inline constexpr std::uint8_t boundComposite=0xc;
inline constexpr std::uintptr_t fnAttach=0x1c35b74,fnReleaseConstraint=0x1c353d8,fnFixedRotation=0x1c7ba58,fnSetInertia=0x1c66d80;
// UNFREEZE: sagInstDataProp's vtable (GOT 0x2e032b0; address point +0x10), its kind byte +0x18 (1) and gohProp +0x8; gohProp +0xe0 = its
// fragInstNav, +0xf0 bit 0 = fixed by the description; the frag type's composite +0x130 (+0xd2 count, +0xd4 a BVH part); a bound's
// CanBecomeActive slot +0xd0 and the five that return true; the archetype's mass +0x38
inline constexpr std::uintptr_t propUdGot=0x2e032b0,propUdVtable=0x2d5f270,compCanActive=0x1bac48c;
inline constexpr std::uintptr_t partCanActive[5]{0x1ba5b2c,0x1bce854,0x1ba9a90,0x1bd2bc0,0x1bb73a4};
inline constexpr std::size_t udKind=0x18,udParent=0x8,gohInst=0xe0,gohFlags=0xf0,typeComposite=0x130,compPartCount=0xd2,compBvh=0xd4,boundCanActive=0xd0,archMass=0x38;
// propPropIterator: 0x80 instance pointers, +0x400 cursor, +0x404 count
inline constexpr std::size_t iterBytes=0x408,iterCount=0x404;inline constexpr int iterMax=0x80;
// mvrMoverSimComponent: the mvrMoverInst at +0x20 (mvrMoverComponent() 0x2889cb8), its matrix row d at +0x70; user: actor +0x8, kind +0x18 (0 = actor)
inline constexpr std::size_t moverInst=0x20,moverPos=0x70,userActor=0x8,userKind=0x18;
inline constexpr std::size_t actorGuid=0x8; // sagActor guid (u32), the key ConfigureActorExcludeInsts takes
// sagActor health component +0x60: hp float +0x20, flags u32 +0x150 (0x40 dead); the layout VrMelee's scan reads
inline constexpr std::size_t actorHealth=0x60,healthValue=0x20,healthFlags=0x150;inline constexpr std::uint32_t healthDead=0x40;
// sagActor animator +0x38; animAnimatorComponent +0xb00 = the bumper's guid ConditionBumpedByActor::Match 0x1cd4d28 reads (stored by
// HandleBumpedIntoMessage 0x1e18aa4 only for a bumper moving over 0.5 m/s, sagMsgGetVelocity at 0x1e18a64..0x1e18a80)
inline constexpr std::size_t actorAnim=0x38,animBumper=0xb00;
// DOORGATE: the jump slots of FindDoor / Open / Close and their targets; the mode word chain from the actor (AddUseContext 0x26af5a8 ..
// 0x26af5f4); the callers' return addresses (Open: bhUsePacket::Perform, projectile, vehicle, two script commands; Close: Perform,
// VerifyDoor, AddUseContext, the save sync; the tail calls of the sync and the scripts count as other)
inline constexpr std::uintptr_t findDoorGot=0x2e53628,fnFindDoor=0x26af868,openGot=0x2e482b8,fnDoorOpen=0x286b30c,closeGot=0x2e482b0;
inline constexpr std::size_t modeAnim=0x38,modeBlock=0xab8,modeObj=0x18,modeWord=0x18;
inline constexpr std::uintptr_t openCallers[5]{0x201e2bc,0x2533504,0x28d10a4,0x2aca1dc,0x2acab88},closeCallers[4]{0x201e29c,0x26af510,0x26af860,0x2766eb0};
// GLASS: sagFragInst::CollisionDamage (vtable +0x218 of sagFragInst and fragInstNav), GetClassType of sagFragInst (5); the frag type's
// groups +0xf8, children +0x100, child count +0x1b3, group count +0x1b2, child -> group index +0x10; group +0x53 bit 1 glass, +0x54 the
// minimum damage, +0x58 the health floor; the cache entry +0x138 health per group; bgGlassManager::sm_CreateCount (GOT 0x2de7be8, at most
// 4 panes per frame) and the fragTune singleton (GOT 0x2de8688: +0x20 damage scale, +0x2c contact damage, +0x30 speed gate)
inline constexpr std::uintptr_t fnCollisionDamage=0x28730b0,fnFragClass=0x2874984,createCountGot=0x2de7be8,createCount=0x4e70dd2c,fragTuneGot=0x2de8688,fragTune=0x4e704c20;
inline constexpr std::size_t vtCollisionDamage=0x218,typeGroups=0xf8,typeChildren=0x100,typeChildCount=0x1b3,typeGroupCount=0x1b2,childGroup=0x10,
    groupFlags=0x53,groupMinDamage=0x54,groupFloor=0x58,cacheHealth=0x138;
// the iterator (PreApplyImpacts 0x1e9ad24..0x1e9add8): manifold arrays +0x0 / +0x8 and their counts +0x24 / +0x28; a manifold's sub
// manifolds [m+0x110+8k] (k >= 1, count +0x5ab), contacts +0x10 + 0xd0 j (count +0x5ac), active +0xce, depth +0xc, positions +0x30 (A) /
// +0x40 (B), normal +0x50 (A's), elements +0xc8 / +0xca; the owner manifold [c+0xc0]: instances +0x550 / +0x558, components +0x5a8 / +0x5a9
inline constexpr std::size_t itArrA=0x0,itArrB=0x8,itCountA=0x24,itCountB=0x28,mfSubs=0x110,mfSubCount=0x5ab,mfContacts=0x10,mfContactCount=0x5ac,
    ctStride=0xd0,ctActive=0xce,ctDepth=0xc,ctPosA=0x30,ctPosB=0x40,ctNormal=0x50,ctElemA=0xc8,ctElemB=0xca,ctOwner=0xc0,mfInstA=0x550,mfInstB=0x558,
    mfCompA=0x5a8,mfCompB=0x5a9;
// BLOOD: TriggerPtFxMelee, CPtFxManager::GetPtxEffectRule(unsigned); g_ptFxMelee (this, never read), sagBloodManager::smp_Instance
// (+0x210 = SET_BLOOD_EFFECTS_ENABLE's byte), g_ptFxManager, CVfxHelper::ms_gameCamPos / ms_fovScaleSqr through their GOT words; the
// creature component's race +0xa30 (TriggerPtFxMelee reads it through actor +0xb0)
inline constexpr std::uintptr_t fnMeleeFx=0x295f7e0,fnRuleByHash=0x2943028,ptFxMeleeGot=0x2e01648,ptFxMeleeVar=0x4e8284b9,bloodMgrGot=0x2e01640,bloodMgrVar=0x4e812728,
    ptFxMgrGot=0x2e015a0,ptFxMgrVar=0x4e815630,camPosGot=0x2e0e118,camPosVar=0x4e828f70,fovScaleGot=0x2e0e120,fovScaleVar=0x4e828fc4;
inline constexpr std::uintptr_t fnBloodHit=0x29151cc; // sagBloodManager::RegisterHit(mgr, actor, comp, Vector3* pos, Vector3* dir, type, bool)
// BLOOD ROUTE: atSingleton<rmPtfxManager>::sm_Instance (its GOT word), the ptxEffectRuleStd vtable (address point) and its InitInstance slot
// (+0xb0), the rule's draw list byte / cull radius / cull distance, rmPtfxManager's draw lists (+0x4f0, count u16 +0x4f8), a slot's rule
// (+0x170, the instance at slot +0x20), CPtFxManager's low-res switch (+0x11cf9)
inline constexpr std::uintptr_t rmPtfxGot=0x2de80e8,rmPtfxVar=0x4e731460,ruleStdVtable=0x2d494a8,fnInitInstance=0x23292d8;
inline constexpr std::size_t ruleVtInitInstance=0xb0,ruleDrawList=0x37d,ruleCullR=0x344,ruleCullD=0x348,rmLists=0x4f0,rmListCount=0x4f8,slotRule=0x170,
    slotInst=0x20,ptFxLowRes=0x11cf9;
inline constexpr std::size_t bloodEnabled=0x210,creatureRace=0xa30;
// BLOOD LIFE (reads only): ptxEffectInst::UpdatePos 0x2358e9c (mode +0x14c, 2 = the matrix pointer +0x170; the world position written to
// [inst +0x330] +0x3b0), ptxEffectRuleStd::DrawEffect 0x2329fbc (the rule's time line at +0x300) and CullEffect (inst +0x250 x +0x290),
// ptxTimeLine::Draw 0x232d2b8 (events: u16 count +0x20, array +0x18 of the time line = rule +0x320 / +0x318; per event the element data
// [inst +0x158] + i * 0x160, its Draw at vtable +0x60), ptxEventEmitter::Draw 0x232f1f8 (draws only while element +0x28 + buffer * 4 is
// not 0; both buffer words read)
inline constexpr std::uintptr_t fnEmitterDraw=0x232f1f8;
inline constexpr std::size_t ptxMode=0x14c,ptxAttach=0x170,ptxData=0x158,ptxElemStride=0x160,ptxElemCount=0x28,ptxWorldObj=0x330,ptxWorldPos=0x3b0,
    ptxFade=0x290,ptxAlpha=0x250,ruleEventCount=0x320,ruleEvents=0x318,eventDrawSlot=0x60;
// GUN GLASS: SweptSphereMultipleExcludeInsts (seg, r, maxHits, isect*, nExcl, excl, include, typeExclude, u8 state) -> TestInLevel on the
// level the simulator's GOT word holds; phIntersection 0x70 bytes: inst +0x0, level handle +0x8 (index u16, generation u16 +0xa), position
// +0x10, normal +0x20, part +0x64, component u16 +0x6a (FireEffect::ApplyDamageToFrag 0x2956494 loads these)
inline constexpr std::uintptr_t fnSweptMulti=0x217af74;
inline constexpr std::size_t isectSize=0x70,isectInst=0x0,isectLevel=0x8,isectPos=0x10,isectNormal=0x20,isectPart=0x64,isectComp=0x6a;
// DRIVE: phCollider::ApplyForce; the collider's type +0x8 (0 rigid, 2 constrained, 1 / 3 articulated), force accumulator +0x150,
// mass +0xac; the bound's part count +0xd0 (the articulated component map is indexed by it)
inline constexpr std::uintptr_t fnApplyForce=0x1c66314,fnNotifyForce=0x1e9ac88;inline constexpr std::size_t colliderType=0x8,colliderForce=0x150,colliderMass=0xac,
    vtNotifyForce=0x98,colliderTorque=0x160;
// GROUND: the mover instance's ground (level index u16 +0x6c0, 0xffff none; generation u16 +0x6c2), the level's generation array +0x68
inline constexpr std::size_t moverGroundIndex=0x6c0,moverGroundGen=0x6c2,levelGens=0x68;
// DIAG: sagActor +0xb0 the creature component, its +0xb0 the current action node, the node's CRC +0x28; the frag type's tune name +0xc0
inline constexpr std::size_t actorCreature=0xb0,compNode=0xb0,nodeCrc=0x28,typeTuneName=0xc0,typeSharedArch=0x120;
// SELF: facFactionMgr::IsZombie(sagActor const*) and the manager object (GOT 0x2deb220)
inline constexpr std::uintptr_t fnIsZombie=0x1e52764,factionGot=0x2deb220,factionVar=0x4e70c4f8;
// PICK: rage::ConditionRandom's statics through their GOT words (R_AARCH64_RELATIVE): sm_RandomUsed (bool), sm_RandomNum (int),
// sm_RandFunc (a function pointer; also read by rdr2NetManager)
inline constexpr std::uintptr_t randUsedGot=0x2e0da38,randUsedVar=0x2f414b4,randNumGot=0x2e0da40,randNumVar=0x4e812390,randFuncGot=0x2e0bd38,randFuncVar=0x4e8123b8;
// PICKFN: g_ReplayRand (the 64-bit multiply-with-carry state ConditionRandom draws from) through its GOT word
inline constexpr std::uintptr_t replayRandGot=0x2ddeea8,replayRandVar=0x4e6afc40;
// NODE: Find, SetActionNode(ActionNode*, bool), the vtable of sagCreatureComponent (actor +0xb0; address point +0x10)
inline constexpr std::uintptr_t fnFindNode=0x1e3abac,fnSetNode=0x1e5a754,creatureVtable=0x2d07970;
// RESETHOLD: the jump slots and their targets; the GOH slot table (count u16 at [[GOT 0x2deabe0]], 16-byte entries at [[GOT 0x2deabe8]]:
// gohBase +0, generation u16 +8); the return address of OnMsg's readiness call; Apply's callers (StartPropReset, ResetProp twice, the
// wrapper 0x24cb1fc) and CommonReset's (Apply's immediate path, the effector CB); a frag instance's reset position +0xb0
inline constexpr std::uintptr_t startResetGot=0x2e4dc50,fnStartReset=0x250bd68,commonResetGot=0x2e4d0d0,fnCommonReset=0x24caa54,applyResetGot=0x2e4d0e0,
    fnApplyReset=0x24caf34,gohCountGot=0x2deabe0,gohCountVar=0x4e6ecfd0,gohTableGot=0x2deabe8,gohTableVar=0x4e6ecfd8,readyPollRa=0x250c734;
inline constexpr std::uintptr_t applyCallers[4]{0x250bdac,0x250b9bc,0x250ba48,0x24cb200},commonCallers[2]{0x24cb194,0x24caca0};
inline constexpr std::size_t instResetPos=0xb0;
// PROPHIT: mvrMoverInst::GetClassType's word (a creature's or the player's mover capsule in a contact)
inline constexpr std::uintptr_t fnMoverClass=0x28928f0;
}
// Object storage (never freed: the engine never owns them; reference counts are pinned high).
inline constexpr std::size_t boundBytes=0x110,archBytes=0x80,instBytes=0x80,boxBytes=0x280,compBytes=0x100;
static_assert(boundBytes>=0x100&&archBytes>=0x70&&instBytes>=0x60&&boxBytes>=0x270&&compBytes>=0xe0);

using ReadFn=bool(*)(void*,std::uintptr_t,void*,std::size_t);
// phCollider::SetInertia takes the inertia in one vector register (q0): a NEON vector on the device, a stand-in on the host.
#if defined(__aarch64__)
using Quad=float32x4_t;
inline Quad quad(const float* f) noexcept {return vld1q_f32(f);}
#else
struct Quad {float v[4];};
inline Quad quad(const float* f) noexcept {Quad q{};std::memcpy(q.v,f,sizeof q.v);return q;}
#endif
struct Engine {
    std::uintptr_t base{};ReadFn read{};
    void (*archCtor)(void*){};void (*setBound)(void*,void*){};void (*setMass)(void*,float){};void (*setGravity)(void*,float){};
    void (*sphereCtor)(void*,float){};void (*capsuleCtor)(void*){};void (*capsuleSize)(void*,float,float){};
    void (*instCtor)(void*){};void (*instInit)(void*,void*,const void*){};
    int (*addActive)(void*,void*,bool){};void (*deleteObject)(void*,int,bool){};bool (*teleport)(void*,void*,const void*,const void*){};
    void (*setVelocity)(void*,const void*){};void (*setAngVelocity)(void*,const void*){};
    void* (*getDoor)(void*,void*){};void* (*doorGet)(const void*){};bool (*doorClosed)(const void*){};void (*doorOpen)(void*,const void*,bool){};
    void (*instDataCtor)(void*,void*){}; // sagInstData::sagInstData(phInst*)
    void (*preApplyGame)(const void*){}; // sagSimulator::PreApplyImpacts_Game(phImpact::Iterator): the iterator by reference to a copy
    void (*preComputeDefault)(const void*){}; // phSimulator::DefaultPreComputeImpacts(phImpact::Iterator) 0x1c27c24: the same convention
    // PALM
    void (*boxCtor)(void*,short*,const void*){};void (*boxSize)(void*,const void*){};
    void (*compCtor)(void*){};void (*compInit)(void*,int,bool){};void (*compSetBound)(void*,int,void*,bool){};void (*compExtents)(void*,bool){};
    void (*setAngInertia)(void*,const void*){};
    // ACT / DOORX / REACH
    void (*forceActivation)(void*){};void (*doorClose)(void*,bool){};void (*propCull)(void*,const void*,float){};
    void (*broadcastBump)(void*,const void*){}; // aActor::Broadcast(mvrMsgBumpedInto const&), called directly (not through the GOT)
    void (*doorFastClose)(void*,bool){};       // SWING: propDoorInst::FastClose(bool)
    bool (*isActDisabled)(const void*){};void (*setDisableAct)(void*,bool){}; // ACT: fragInst::IsActivationDisabled / SetDisableActivation
    std::uintptr_t navClassFn{},deadClassFn{}; // the GetClassType words of fragInstNav / rdrDeadBodyInst vtables (+0x10)
    // REST: propDoorInst::SetState(state, bool), fragInstNav::UpdatePathObjectState, PivotDoorInterface::ContinueActionShared(iface, door)
    void (*doorSetState)(void*,int,bool){};void (*pathState)(void*){};bool (*continueShared)(void*,void*){};
    // GRAB: phContactMgr::AttachObjects / ReleaseConstraint, phConstraint::SetFixedRotation, phCollider::SetInertia
    void* (*attach)(void*,std::uint32_t,const void*,const void*,void*,void*,int,int,float,bool,void*){};void (*releaseConstraint)(void*,void*){};
    void (*fixedRotation)(void*){};void (*setInertia)(void*,const float*,Quad){};
    // GLASS: sagFragInst::CollisionDamage(inst, Collision const&, Iterator*) (called only where the instance's vtable +0x218 holds it),
    // sagFragInst::GetClassType's word; DRIVE: phCollider::ApplyForce(collider, force, position, component)
    void (*collisionDamage)(void*,const void*,void*){};std::uintptr_t fragClassFn{};void (*applyForce)(void*,Quad,Quad,int){};
    // BLOOD: CPtFxMelee::TriggerPtFxMelee(self, actor, fx, fx2, mtx, pos, n, dir), CPtFxManager::GetPtxEffectRule(mgr, hash)
    void (*meleeFx)(void*,const void*,std::uint32_t,std::uint32_t,const void*,const void*,const void*,const void*){};void* (*ruleByHash)(void*,std::uint32_t){};
    bool (*bloodHit)(void*,const void*,int,const float*,const float*,int,bool){}; // sagBloodManager::RegisterHit(mgr, actor, comp, pos, dir, type, bool)
    // GUN GLASS: SweptSphereMultipleExcludeInsts(segment, radius, maxHits, isect*, nExcl, excl, include, typeExclude, state)
    int (*probeLevel)(const void*,float,int,void*,int,const void* const*,int,int,unsigned char){};
};
inline Engine engine{};
inline std::atomic<bool> ready{};
inline std::atomic<const char*> installState{"NOT ATTEMPTED"};
// The groups whose prints and words matched (GroupCore = installed).
inline std::array<bool,GroupCount> groupOk{};
// >= 0: replaces the row value (host tests).
inline std::atomic<int> modeForTests{-1};
// Host tests: lever values after the properties; the thread CPU clock.
inline std::atomic<void(*)(Levers&)> leverPatchForTests{};
inline std::atomic<std::uint64_t(*)()> cpuClockForTests{};

using arc::Counter;using arc::bump;using arc::maxOf;using arc::u64;
struct HandStats {
    Counter liveFrames{},contactFrames{},ghosts{},parkGhosts{},inPlaceReturns{},chestReturns{},respawns{},inserts{},insertFails{},insertWaits{},removes{},
        velClampFrames{},pulses{};
    Counter contactSubsteps{},contactDynSubsteps{},giveRises{},giveDrops{},pushContacts{},dvContacts{},normalFlips{},creepClips{},tangentClips{},partToggles{},comMismatch{};
    Counter devUmSum{},devUmMax{},devFrames{},pairs{},pairsDropped{},worldPairs{},propPairs{},doorPairs{},corpsePairs{},moverHits{},impactSkips{};
    Counter virtualSubsteps{}; // substeps whose contact came from the virtual body alone
};
struct DoorStats {Counter checks{},opens{},closes{},noHandle{},locked{},busy{},opened{},gated{},far{},notPressing{};
    Counter swingHolds{},takeovers{},phaseWrites{},finishes{},glides{},fastCloses{},reverses{},dropsGame{},dropsGone{},capPending{},
        geomAxis{},animOpens{},lockedPulses{},plainOpens{},maxMdeg{};
    std::array<Counter,3> geomStart{},geomHold{}; // the swing's leaf test's rejects (lever, height, bbox) at a start / while held
    // the leaf follow: checks by door state, holds by mode, plain Opens by reason, rests, shuts, remaps of a rested door's phase
    std::array<Counter,6> checksByState{};std::array<Counter,PlainCount> plainBy{};
    Counter open2Holds{},pose4Holds{},rests{},shutsOpen2{},shutsPose4{},restRemaps{},axisMismatch{},geomMismatch{},pose4Moves{},dropsGame4{},rearmGated{},
        releasesIdle{},releasesFar{},releasesOld{},releasesBody{},releasesSide{},swingHandovers{};
    // the part test: pushes refused by the nearest part's gap (doorGapBucket), by the contact normal, on a door the game moves (2 / 3 / 5,
    // left alone), side changes of a hold, pushes held where the leaf meets the mover, rested doors posed to doorRaiseDeg for him, ticks a
    // raise waited for the mover to leave the leaf's sweep
    std::array<Counter,7> refusedGap{};Counter notLeafNormal{},engineBusy{},sideFlips{},floorHolds{},raises{},raiseBlocked{},events{};
    // the approach: the FindDoor hook (calls, found, by reason, the found door's state and kind: pivot / anim / other), Open and Close by
    // caller (the counting hooks: Perform, projectile, vehicle, script, script, other / Perform, VerifyDoor, AddUseContext, sync, other)
    // and the door's state, a player Open (the pusher within 15 cm of his mover), Opens and Closes of a door the hands hold or rested,
    // the hand-backs of such a door to the engine's opening
    Counter fdCalls{},fdFound{};std::array<Counter,FindWhyCount> fdBy{};std::array<Counter,6> fdState{};std::array<Counter,3> fdKind{};
    std::array<Counter,6> openBy{},openState{};Counter openPlayer{},openOnOurs{},handBacks{},handBackFails{};std::array<Counter,5> closeBy{};Counter closeOnOurs{};
    // the mover's push: candidates judged, in the band, closed doors opened posed, open ones raised / pushed back, stuck opens (the
    // stock Open), refusals (unsure leaf, anim door, locked, no cap, a hand holds it, the game moves it), the deepest reach of his capsule
    // past the leaf (mdeg); the hand from a closed leaf: posed opens, the cap still pending (left to the stock opening), its first angle
    Counter bodyCands{},bodyInBand{},bodyOpens{},bodyRaises{},bodyCloses{},stuckOpens{},bodyMaxPenUm{};std::array<Counter,6> bodyRefused{};
    // the capsule's edge to a kept closed leaf (bodyGapBucket), in-band ticks the target did not move, ticks at a kept closed door (the
    // stuck timer running), closing pushes held at the floor that shut the leaf (it then opens away from him), opens refused for a swing
    // limit under 2 degrees (left to the stock opening)
    std::array<Counter,7> bodyGapHist{};Counter bodyStill{},stuckAtTicks{},bodyFlips{},capTiny{};
    Counter closedPoses{},capFallback{},closedFirstMdegMax{},posedFails{};
    // DOOR PRESS: press ticks of holds, hold ticks without a press, ticks where the solved part was over doorTouchM off the leaf while the
    // command pressed (the doorpress 0 gate would have frozen the leaf), straddling edge presses, start sides by source (the approach, the solved
    // part outside, the contact normal, the solved centre), the mover's push applied through a hold with no press, closed-door presses under
    // 1 cm, starts refused without a press (every tick; noted = those sampled into the reject ring, the tick ring and refusedGapCm, at most
    // one per hand and door per doorNoteNs), candidates without a recent 5 mm contact (not in checks / notPressing), press ticks with the
    // command over 2 cm through the leaf while the solved part touches its locked face (the leaf held back by the mover stop or the rate cap)
    Counter pressTicks{},noPressTicks{},solvedGateWouldRefuse{},edgeStraddles{},sideApproach{},sideSolved{},sideNormal{},sideCentre{},bodyViaHold{},closedShallow{},
        startNoPress{},startNoPressNoted{},pressCands{},throughTouch{};};
inline constexpr std::array<const char*,6> bodyRefusedNames{"unsure","anim","locked","noCap","handHeld","busy"};
// GLASS: hand contacts with a frag pane (in the step), their edges, distinct panes, the approach histogram (glassBin) and its maximum, the
// deepest contact; break calls asked, done (a pane created), no pane, refused by reason; a tracked pane's health dropped by the stock
// contact path (no call of ours); the scan's longest run
struct GlassStats {Counter contacts{},edges{},panes{},approachMaxMms{},depthMaxUm{},requests{},done{},noShatter{},stockDamage{},scanNsMax{};
    std::array<Counter,8> approachHist{};std::array<Counter,2> byClass{};std::array<Counter,GlassWhyCount> refused{};
    // GLASS BLOOD: drops played, the longest call (ns), refusals by reason
    Counter bloodCalls{},bloodCallNsMax{},bloodPricks{};std::array<Counter,BloodWhyCount> bloodRefused{};
    // BLOOD ROUTE: drops routed to list 0, drops found at their list's head after the call (made) or not (GetEffectInst gave none),
    // unrouted drops by reason (BloodRouteWhy), watches armed
    Counter bloodRouted{},bloodMade{},bloodNotMade{},bloodWatches{};std::array<Counter,RouteWhyCount> bloodRouteRefused{};
    // BLOOD PLACE: drops placed by the contact frame for want of an eye; BLOOD LIFE: ticks read, reads that failed, slots recycled
    Counter bloodEyeFallback{},bloodLifeTicks{},bloodLifeFails{},bloodLifeRecycled{};};
// GUN GLASS: ticks with a candidate hand, refusals by gate (GunWhy), probes by kind (the muzzle sweep, the barrel line, the free palm), the
// probe time, exceptions, hits by class (fragInstNav, sagFragInst, our own / the mover, other), probes that filled all 4 hits, hits judged
// by the glass checks (GlassWhy; ok = would break), would-breaks, break calls and their results, the approach of judged hits (glassBin);
// probes sent with the player's own instances excluded (his ConfigureActorExcludeInsts list) or without it (none read yet), the most excluded
struct GunGlassStats {Counter ticks{},probeNsSum{},probeNsMax{},exceptions{},wouldBreak{},breaks{},done{},noShatter{},glassHits{},approachMaxMms{},maxHitsFull{};
    Counter exclOwn{},exclNoOwn{},exclMax{};
    std::array<Counter,GunWhyCount> gated{};std::array<Counter,3> probes{};std::array<Counter,4> hits{};std::array<Counter,GlassWhyCount> judged{};
    std::array<Counter,8> approachHist{};};
struct PropStats {Counter candidates{},activated{},refused{},infinite{},alreadyActive{},gated{},gatedLru{},gatedHand{},skipped{},wakes{},wakeNsSum{},wakeNsMax{};
    Counter unlocks{},skipOverride{},skipNever{},skipInfinite{},skipFixed{},skipActive{},skipNotInLevel{},skipDoor{},skipCached{},skipNotLocked{};
    // the root release: candidates judged, released (bit 1 written), then active / still refused, by reason; contact wakes skipped for a fixed root
    Counter relCandidates{},released{},relActivated{},relRefusedAfter{},skipRoot{};std::array<Counter,ReleaseCount> relBy{};
    // UNFREEZE: never-activate props under a palm (candidates), made physical (done) then active / still refused, the flag back (restored),
    // entries dropped (gone from the level) or skipped (list full, the gap); by reason
    Counter ufCandidates{},ufDone{},ufActivated{},ufRefusedAfter{},ufRestored{},ufDropped{},ufFull{},ufCached{};std::array<Counter,UnfreezeCount> ufBy{};
    // UNFREEZE: shallow by reason (not live, the contact too old, too shallow and slow) and the depth histogram (<1, 1-2, 2-5, >=5 mm),
    // candidates by source (press, grab); the collider's mass set / set again after a reactivation, kicks; tracked props moved 2 cm,
    // tipped 30 deg, stuck (touched, active a second, under 5 mm), back where they started (snap), reactivations, touches while active
    std::array<Counter,3> ufShallowBy{};std::array<Counter,4> ufDepthHist{};std::array<Counter,UfSourceCount> ufSource{};
    Counter ufMassSet{},ufMassReapplied{},ufKicks{},ufMoved2cm{},ufTipped30{},ufStuck{},ufSnapBack{},ufReactivations{},ufTouchesActive{};
    // the approach: props armed near a closing palm (the reach query), fired ahead of the contact, met by a palm within 0.5 s after it, gone
    // to sleep before any contact; tracked props asleep again met by a palm (woken at once, no gates); the contact speed of the kicks (cm/s
    // sum); tracked props that went to sleep, whose flag came back from the engine or a script (not our restore)
    Counter ufNearArmed{},ufApproachFired{},ufApproachThenContact{},ufApproachSlept{},ufRewakes{},ufKickCmsSum{},ufSleeps{},ufRefrozen{};
    // the arming slots full of nearer props, armed entries kept by a resting palm (per tick), the palm's reach at a fire (the 5 cm floor,
    // 5-9, 9-12, 12+ cm); the first contact with an approach prop: the push speed given, a kick (the contact met it while fixed), too slow to kick
    Counter ufNearFull{},ufNearKept{},ufFirstYield{},ufFirstKick{},ufFirstKickLow{};std::array<Counter,4> ufNearReach{};
    // per cup (CupTrace): contact episodes before the first 2 cm move (1, 2, 3, 4+), the first contact to the first activation (<20, 20-50,
    // 50-100, >100 ms), the activation to the first move (<50, 50-150, 150-500, >500 ms)
    std::array<Counter,4> ufTouchesToMove{},ufContactToActMs{},ufActToMoveMs{};};
struct NpcStats {Counter scans{},touches{},bumps{},gated{},staleGuid{},notAlive{},fallbackTouches{};
    Counter vbTicks{},vbBodies{},vbCreatures{},vbHorses{},vbCapsules{},vbAgeUsSum{},vbAgeN{},vbDeepUm{},vbNsSum{},vbNsMax{},vbCalls{};
    Counter pressStarts{},pressBumps{},pressRests{};
    // after a bump: the creature's animator (actor +0x38) holds the player as its bumper (+0xb00, HandleBumpedIntoMessage) or not
    Counter bumpHeld{},bumpNotHeld{};
    // open-hand touches handed to VrMelee: slaps, shoves, presses (and press ticks), slap -> press, gated, by capsule kind, why none
    Counter slaps{},shoves{},presses{},pressTicks{},reclassified{},touchGated{},closedHand{},horse{},slow{},grabSkips{};std::array<Counter,8> byKind{};
    // slap windows opened / ending in a slap / in nothing, presses escalated to a push; the first gate an edge failed (slap, shove);
    // the edge's relative speed (0.25 m/s bins; [1] = edges that became a press), approach and swing; a press's ticks (1, 2-3, 4-7, 8-15,
    // 16-31, 32+), the push it asked (cm: <1, 1-3, 3-6, 6-12, 12-24, 24+), its deepest (cm: <1, 1-2, 2-3, 3-5, 5-8, 8+), why it ended
    // (shallow, facing, the hand left, another class)
    Counter windows{},windowSlaps{},windowNone{},escalations{},escalateStill{};std::array<Counter,SlapFailCount> slapFail{};std::array<Counter,ShoveFailCount> shoveFail{};
    std::array<std::array<Counter,16>,2> edgeSpeed{};std::array<Counter,16> edgeA{},edgeSwing{};
    std::array<Counter,6> pressTicksHist{},pressCmHist{},pressDepthHist{};std::array<Counter,4> pressEnd{};
    // a press's facing at its end and its lowest facing (facingBin: 8 bins over -1..1); edge slaps that passed only through the arm speed
    // (the swing sample under the gate) or with the facing under .25 (a fast head / neck hit)
    std::array<Counter,8> pressEndFacing{},pressFacingMin{};Counter slapByArm{},slapHeadFast{};
    // shoves sent by tier (pushTier: light = escalated presses, medium, firm)
    std::array<Counter,TierCount> tierSent{};
    // graded push (lever pushgrade): pair edges (touchPair), those that skipped the slap gate, slap verdicts turned into a pair push, slap
    // verdicts held for the other hand (slapHeld), sent after the hold (slapHeldSent) and dropped (slapDropped), push-like neck touches kept
    // from a slap, the gap between the two hands' edges on one creature (pairGapBin)
    Counter pairs{},pairGateBypass{},slapToPair{},slapHeld{},slapHeldSent{},slapDropped{},slapPushLike{};std::array<Counter,5> pairGapHist{};};
// PROPHIT: ticks a held prop was armed (heavy enough, creatures near), its impact iterators scanned (the longest scan, ns), creature
// contacts taken, their edges (by approach: propSpeedBin) and verdicts (PropHitWhy), hits sent (strong), ticks the prop moved at the hit
// speed with a creature's centre within 1.2 m of its centre of mass and no contact (a hit the scan may miss)
struct PropHitStats {Counter armedTicks{},scans{},scanNsMax{},contacts{},edges{},sent{},strong{},nearFast{};std::array<Counter,6> speedHist{};
    std::array<Counter,PropHitWhyCount> why{};};
struct ReachStats {Counter queries{},cullCalls{},queryNsSum{},queryNsMax{},found{},woken{},already{},skipped{},rateLimited{},lruHits{},outOfReach{},refused{},infinite{};
    Counter skipClass{},skipDoor{},skipOwn{},skipNotInLevel{},skipNotInactive{},skipRoot{},skipNever{},still{};};
// GRAB: starts, refusals and releases by reason, the worst lag while held, the fastest throw, the slowest AttachObjects
enum GrabRefuse : std::uint8_t {GrabNoCand=0,GrabFar,GrabClass,GrabDoor,GrabOwn,GrabInfinite,GrabHeavy,GrabInactive,GrabPool,GrabNoManifold,GrabGroup,GrabStale,GrabPair,
    GrabRefuseCount};
inline constexpr std::array<const char*,GrabRefuseCount> grabRefuseNames{"noCand","far","class","door","own","infinite","heavy","inactive","pool","noManifold","group","stale",
    "pair"};
// safety = a heavy hold whose prop stayed over grabSafetyM from the hands for grabBreakMs; teleport = the mover moved grabTeleportM or more
// in one tick while a heavy hold was on
enum GrabEnd : std::uint8_t {GrabEndOpen=0,GrabEndThrow,GrabEndLeash,GrabEndDetached,GrabEndRetarget,GrabEndDisabled,GrabEndDeactivated,GrabEndBody,GrabEndMode,
    GrabEndRefuse,GrabEndSafety,GrabEndTeleport,GrabEndCount};
inline constexpr std::array<const char*,GrabEndCount> grabEndNames{"open","throw","leash","detached","retarget","disabled","deactivated","body","mode","refuse",
    "safety","teleport"};
// misses: a fresh grab pair whose nearest part was not taken (own instance, no readable parts, too far: the distance histogram 0-2,
// 2-4, 4-6, 6-10, 10-20, 20+ cm and the squeeze then: <.3, .3-.65, >=.65); readyAfterLatch: a ready that came 0-25, 25-50, 50-100,
// 100-200 ms after a latch the grab did not get, latchNoReady = such a latch with no ready within 200 ms; latchClass = the classes the
// hand's kept pairs met in the 4 substeps before a latch; holdOver = a press taken with the candidate of an earlier tick.
// never: a prop no grab can make physical (judged once per instance: infinite mass vs movers, a fixed root whose type never moves
// (minMove < 0), a never-activate prop UNFREEZE refuses), so its grip press stays the game's. cacheFills = parts caches filled (each one
// reads the part list, the boxes and the never verdict).
enum GrabMiss : std::uint8_t {MissOwn=0,MissNoParts,MissFar,MissInfinite,MissNeverMove,MissNeverAct,MissCount};
struct GrabStats {Counter starts{},presses{},stalePresses{},releaseCalls{},maxLagUm{},throwMmsMax{},attachNsMax{},readyTicks{},holdOver{},holdOverFar{},latchNoReady{},cacheFills{};
    // GRAB WELD: fixed-rotation holds welded (both constraints), the second attach refused (the hold kept on the position constraint
    // alone), fixed-rotation holds on one constraint with SetFixedRotation (lever grabweld 0 or the WELD prints), rotation-constraint
    // releases, holds ended by their rotation constraint
    Counter welds{},rotRefused{},rotOnly{},rotReleaseCalls{},rotDetached{};
    std::array<Counter,GrabRefuseCount> refused{};std::array<Counter,GrabEndCount> ends{};std::array<Counter,MissCount> misses{};
    std::array<Counter,6> missDist{};std::array<Counter,3> missSqueeze{};std::array<Counter,4> readyAfterLatch{};std::array<Counter,16> latchClass{};
    // hold lengths (holdBucket), grip dips while held (under .5 and back, under .3 and back within the open debounce); the drive
    // (substeps with a force, capped, refused for an unconsumed accumulator, articulated colliders driven, holds whose drive went off, no
    // collider); furniture whose type never moves: by gate, released (+0x80 bit 1 written), then active / still refused
    std::array<Counter,5> holdHist{};Counter gripDips50{},gripDips30{};
    Counter driveApplied{},driveCapped{},driveStale{},driveArticulated{},driveOff{},driveNoCol{},driveShared{},driveArtRefused{};
    // the accumulator at the drive (lever grabstaleown): a foreign force (not the drive's own unconsumed one: counted, the drive goes on),
    // a foreign force over 3 x grabForceMaxN (the drive off); the player's mover standing on the held prop (lever grabgroundzero), its
    // queued force zeroed; ball-jointed substeps without the drive (the hand's first-order approach, grabfallbacktaums)
    Counter driveForeign{},driveRunaway{},groundHeld{},groundZeroed{},fallbackSubsteps{};
    // the drive's own unconsumed force taken back at the drive's end, a runaway accumulator cleared, substeps the upward force was capped
    // at the prop's weight (lever grabgroundlift)
    Counter staleCleared{},runawayCleared{},groundLiftCapped{};
    // RIGID CARRY (lever grabcarry): pair substeps driven by it, the other hand's substeps it covered, the force / torque at its cap, joins
    Counter carrySubsteps{},carryShared{},carryCapF{},carryCapTau{},carryJoins{},carryRefused{};
    // hand bodies moved at the grips' velocity after the carry's step (grabcarrypredict), pairs resumed with their join pose kept
    // (grabcarrykeepms), substeps whose collider mass differs over 2 % from the held mass (the carry drives the collider's)
    Counter carryPredicted{},carryKept{},carryMassDiff{};
    std::array<Counter,GrabMoveCount> moveBy{};Counter moveReleased{},moveActivated{},moveRefusedAfter{};
    // released furniture put back: after a failed wake, once it slept untouched, dropped (gone from the level), refused (list full)
    Counter moveRolledBack{},moveRestored{},moveDropped{},moveFull{};
    // TWO HANDS (pairStep): pairs joined / ended, substeps driven as a pair; the grip spacing d (cm: <20, 20-40, 40-60, 60-100, 100+), the
    // hands' stretch against it (|cmd B - cmd A| - d, cm: <5, 5-15, 15-30, 30-60, 60+), hand 0's weight share (<.3, .3-.45, .45-.55, .55-.7,
    // .7+), the spin about the grip line (rad/s: <.5, .5-1, 1-2, 2-4, 4+) and its reversals, the most both hands lifted (N) against the
    // prop's weight, substeps a hand hit its up / side cap, the prop's top speed while paired (cm/s)
    Counter pairJoins{},pairEnds{},pairSubsteps{},pairUpMaxN{},pairWeightN{},pairUpCapped{},pairSideCapped{},pairSpeedMaxCms{},pairRollFlips{};
    std::array<Counter,5> pairDHist{},pairStretchHist{},pairShareHist{},pairRollHist{};
    // JOIN: ticks the free hand was a join candidate, join presses tried (startGrab on the other hand's prop), joined, refused; the free
    // hand's distance to the held prop each tick the other hand holds a ball-jointed prop or a corpse (cm: <4, 4-6, 6-10, 10-20, 20-35,
    // 35+); while a heavy prop (propHitKg or more) is held: ticks, ticks with no creature body published (the prop hit cannot arm), the
    // nearest creature to a palm (m: <1.6, 1.6-3, 3-6, 6+, none), the prop's speed (m/s: <1, 1-2, 2-3, 3-4, 4+)
    Counter joinReadyTicks{},joinTries{},joinStarts{},joinRefused{},heavyHeldTicks{},heavyNoBodies{};
    std::array<Counter,6> joinDistHist{};std::array<Counter,5> heavyNearestHist{},heavySpeedHist{};
    // HEAVY HOLDS: ticks a hold ran as heavy (grabheavyhold), a held grip that fell from 0.85 or more to exactly 0 in one tick (a zeroed
    // sample: a real release passes through values in between), ticks held while the theater chord was zeroing the grips; a pair hand's
    // up cap bound by its share / by grabForceMaxN, its side cap
    Counter heavyTicks{},gripSnapZero{},heldChordConsuming{},pairUpCapShare{},pairUpCapForce{};};
// Per row mode (OFF / MEASURE / ON): the main loop's frame interval at our tick and the main thread's CPU time per frame (both only for
// frames of 5..100 ms in the same mode as the tick before) and our costs, so one saved log compares the three modes.
// Every cost sum (ticks, PreUpdate, Update, CollideObjects, the NPC clip, the tick's sections) belongs to the window it was spent in (one
// tick to the next): it is kept pending and committed to the counted sums when that window counts as a frame, else to idle (a loading
// screen's 1-2 ms ticks, stalls, a mode change), so every per-frame figure divides sums of counted frames by counted frames.
enum Section : std::uint8_t {SecUpkeep=0,SecHands,SecProps,SecGrab,SecDoors,SecReach,SecNpc,SecUnfreeze,SecCount};
inline constexpr std::array<const char*,SecCount> sectionNames{"upkeep","hands","props","grab","doors","reach","npc","unfreeze"};
struct ModeStats {Counter frames{},frameNsSum{},frameNsMax{},ticks{},tickNsSum{},preCalls{},preNsSum{},updCalls{},updNsSum{},spans{},spanNsSum{},vbCalls{},vbNsSum{};
    Counter cpuN{},cpuNsSum{},cpuNsMax{},shortDrops{},longDrops{};std::array<Counter,cpuBuckets> cpuHist{};
    Counter filterCalls{},filterNsSum{},idleTicks{},idleNsSum{};std::array<Counter,SecCount> secNsSum{};};
// The costs of the window in progress (main thread only).
struct PendingCost {std::uint64_t ticks{},tickNs{},preCalls{},preNs{},updCalls{},updNs{},spans{},spanNs{},vbCalls{},vbNs{},filterCalls{},filterNs{};
    std::array<std::uint64_t,SecCount> secNs{};};
// PASSIVE contact (lever npcpassive): hand-ticks on a body with the arm under activeMps (passiveTicks) and their touches (passiveContacts),
// contact substeps without velArm (armUnknown); presses whose depth asked a step the arm gave none (pressStepZeroArm), presses sent with an
// arm step (pressArmSent); escalations the arm refused (escalateRefusedArm), bumps skipped for a passive hand (bumpSkippedPassive), shove
// edges the arm gate refused (shoveRefusedArm); histograms (armBin) of armIn at the edge, armIn on press ticks, the player's locomotion into
// the body at the edge, the turn part at the edge (|vel - velArm|).
// PUSH FIX: shove edges whose facing gate passed only on the palm's facing toward the body's centre (an arm capsule, lever pushfacingbody),
// the facing failures by capsule kind, stroke peaks sent as a regrade / closed under 10 % / lost to a new edge (lever pushpeak), walk-in
// bumps sent for a waiting stumble / refused (stale, dead)
struct PassiveStats {Counter passiveContacts{},passiveTicks{},armUnknown{},pressStepZeroArm{},pressArmSent{},escalateRefusedArm{},bumpSkippedPassive{},shoveRefusedArm{},
    fallbackSkipped{},activeEdges{},pressArmHeld{},gradeLoco{};
    std::array<Counter,8> armInEdge{},armInPress{},locoInEdge{},turnEdge{};
    Counter facingRescued{},peakSent{},peakNone{},peakLost{},busyBumps{},busyBumpRefused{};std::array<Counter,8> facingFailKind{};};
// TWO HANDS statics and guard: pair substeps with the statics carry, its couple clamped, a grip line under 10 cm; the couple (N: <50, 50-150,
// 150-300, 300-450, 450+); guard trims (the spring's push into the player removed) and their sum (N); player contacts of a held heavy prop
// (propScan, every armed substep) and those that hit the 3 slots before.
// playerSteep = player contacts whose normal is mostly along up (the prop on top of the capsule): not used by the guard
struct PairStaticsStats {Counter statics{},clamped{},shortLine{},guardTrims{},guardTrimNSum{},playerContacts{},playerSteep{};std::array<Counter,5> coupleHist{};};
// PROPHIT (geometric): ticks armed, no part cache, read failures, parts boxed, sphere
// rejects, capsule tests, the cost (us), touches started, hits sent (strong), the verdicts (PropHitWhy), engine contact vs the shell (engine
// only, geometry only, both), at touch start the approach / swing / arm (bins .5 m/s: 0-.5, .5-1, 1-1.5, 1.5-2, 2-3, 3+), the engine
// contact's distance to that creature's nearest bone capsule surface (cm: <0, 0-5, 5-10, 10-15, 15-25, 25+).
// capRejects = capsules skipped by their distance to a part's sphere; noArmDir = noArm verdicts whose arm speed passed but not along the
// normal (a lift or a turn); at touch start the arm speed along the normal (armDirHist, the .5 m/s bins) and the age of its peak (ms: <50,
// 50-100, 100-150, 150-250, 250-400, 400+ or none).
// gapCmHist = per armed tick the nearest body's gap (the lower bound from the part and capsule bounds, exact where one segObbDist ran;
// cm: <0, 0-10, 10-25, 25-50, 50-100, 100-200, 200+); bodyWhy = per (body, armed tick) how far its test went: every part rejected by the
// body sphere, by the capsules, the exact distance beyond the shell, a touch; nearExact = the extra exact tests the gap ran (a pair under
// 30 cm that no exact test measured)
struct PropGeoStats {Counter armedTicks{},noCache{},readFails{},parts{},sphereRejects{},capTests{},usSum{},usN{},usMax{},touches{},sent{},strong{};
    std::array<Counter,PropHitWhyCount> why{};Counter engineOnly{},geoOnly{},both{};std::array<Counter,6> aHist{},swingHist{},armHist{},shellCm{};
    Counter capRejects{},noArmDir{};std::array<Counter,6> armDirHist{},armAgeHist{};std::array<Counter,7> gapCmHist{};std::array<Counter,4> bodyWhy{};Counter nearExact{};};
// PLAYER SKIP (lever grabplayerskip): the + 0xc8 functor word swapped in / given back, held-prop iterators the guard walked (and those in
// the linger after a hold), manifolds and contacts it switched off, the deepest of them (um: growing past 20 mm = the solver honours the
// byte and lets the prop into the capsule), the walk's cost (ns), the longest linger (ms), pair substeps whose guard fell back to the trim
struct PlayerSkipStats {Counter installs{},restores{},iterators{},lingerIters{},manifolds{},contacts{},depthMaxUm{},nsMax{},lingerMsMax{},guardFallback{};
    // lingers kept past 2 s because the prop was still 3 cm or more inside the capsule, lingers ended at 10 s, lingers dropped because the
    // level slot no longer holds the prop
    Counter lingerDeep{},lingerCapped{},lingerStale{};
    // a welded light hold the guard took on (grabSkipLight), its iterators, the contacts it switched off and the deepest (um), lingers
    // evicted by a third released prop
    Counter lightHolds{},lightIters{},lightContacts{},lightDepthMaxUm{},lingerEvicted{};};
// HELD MASS (lever grabpairkg): the pair's drive mass set on the prop's collider, given back, a restore that found another collider (its
// activation copied the archetype's mass back), a set refused (no collider at the index, no inertia read); velKept = collider mass changes
// (this one and the unfreeze's) that set the velocities again at the new mass (setColliderMass); the last mass before / set
struct HeldMassStats {Counter sets{},restores{},misses{},refused{},velKept{};std::atomic<float> kgBefore{-1.f},kgSet{-1.f};};
// THROW: open edges judged on nav holds; throws by kind (light, heavy, a pair) and the light holds grabStep let go fast (a flight without
// a written launch); verdicts that refused (ThrowVerdict) and MEASURE verdicts that would have thrown; launches written / not written (the
// prop faster already, no collider), spins capped; hand-prop pairs dropped after a throw; flights started and ended by FlightEnd; in-flight
// hits (sent as the player's strike, strong), light taps sent / refused (lever, zone, speed, slap gate) and verdicts judged by PropHitWhy
struct ThrowStats {Counter edges{},throwsLight{},throwsHeavy{},throwsPair{},lightGrabThrows{},midGrabThrows{},midGrabRefused{},measured{},velWrites{},velRefused{},spinClamped{},pairDrops{},
    flights{},hits{},hitsStrong{},taps{},tapRefused{},geoUsSum{},geoUsN{},geoUsMax{};
    std::array<Counter,ThrowVerdictCount> refused{};std::array<Counter,FlightEndCount> ends{};std::array<Counter,PropHitWhyCount> why{};};
// LIGHT GHOST (lever grabghost): ghosts started by a closing hand / at a fixed-rotation hold's end, pairs dropped, ends by reason (GhostEnd),
// holds started on a ghosted prop, the longest ghost (ms), first-contact kicks / pushes skipped for a closing hand.
// restingPairs: light-prop pairs of a hand in the closing band whose squeeze was not rising (they collide: pushes as an open hand).
// GRAB SEAT: pulls started, seated, timeouts, cancels (open, gone, inactive, mode), grabs already seated (within 5 mm), the pull's time
// (<50, <100, <200, <350 ms, timeout) and its start distance (<2, <5, <10, <15, 15+ cm), the fastest arrival relative to the hand (mm/s),
// the velocity writes, the pulls attached on a throw, the grip dips under .30 a pull rode through
struct SeatStats {Counter starts{},seated{},timeouts{},nearSkips{},arriveMmsMax{},velWrites{},refused{},thrown{},dipsKept{};std::array<Counter,4> cancels{};std::array<Counter,5> msHist{},d0CmHist{};};
struct GhostStats {std::array<Counter,GhostWhyCount> starts{};Counter drops{},grabbedWhileGhost{},msMax{},kicksSkipped{},yieldsSkipped{},restingPairs{};
    std::array<Counter,GhostEndCount> ends{};};
struct Stats {
    Counter ticks{},leverReads{},exceptions{},wrongThread{},noEngine{},noSim{},simResets{},inactive{},listFails{},listLost{},targetsStale{},targetsHeld{},
        moverBad{},flagsZero{},jumps{},carried{},noPlayerTicks{},moverReuse{};
    Counter tickNsSum{},tickNsMax{},preCalls{},preNsSum{},preNsMax{},updCalls{},updNsSum{},updNsMax{},filterCalls{},filterNsSum{},collideTrue{};
    Counter knockOffDrops{},knockOffKept{}; // KNOCKDOWN HANDS-OFF: class-5 pairs dropped / kept (lever knockoffms 0) inside a window
    Counter spans{},spanNsSum{},spanNsMax{},substeps{},ownReads{},ownFails{},guardInstalls{},guardRestores{},refusals{},deactRefused{},buildFails{};
    std::array<HandStats,2> hand{};std::array<Counter,16> otherClass{};
    DoorStats doors{};PropStats props{};ReachStats reach{};NpcStats npc{};GrabStats grab{};GlassStats glass{};SeatStats seat{};std::array<ModeStats,3> byMode{};PropHitStats propHit{};
    GunGlassStats gun{}; // GUN GLASS
    PassiveStats passive{};PairStaticsStats pairStatics{};PropGeoStats propGeo{};PlayerSkipStats playerSkip{};HeldMassStats heldMass{};
    ThrowStats throws{};GhostStats ghost{};
    Counter pairFfSubsteps{},pairFfMaxN{},pairDriveWrites{}; // PAIR DRIVE: substeps with the feed-forward, its largest force (N), ring writes
    std::atomic<unsigned> mode{ModeOff},type{},include{},moverType{},moverInclude{};
    std::atomic<int> inactiveLevelActive{-1},inactiveLevelCap{-1};std::atomic<float> inactiveCmdSpeed{-1.f};
};
inline Stats stats;
// Pair samples: distinct (class, type, include) of other instances met (first 8), and the stage-0 dump of engine values.
struct PairSample {int cls{-1};std::uint32_t type{},include{};std::uint64_t count{};std::array<std::uint32_t,3> states{};};
// A prop whose wake was refused: its activation fields (diagnostics, the tick's guarded reads).
struct RefusedSample {std::uintptr_t inst{};std::uint16_t flags12{},w1c0{},typeBits{};std::uint32_t f80{},archType{},archInclude{};bool cache{};int stateAfter{-1};bool reach{};
    float minMove{-1.f},radiusCm{-1.f};};
// UNFREEZE: a never-activate prop met by a press (8 distinct): its user data, description bit, composite and parts, mass, size, result;
// NEVERMOVE: a prop the root release refused for minMove < 0 (8 distinct).
struct UnfreezeSample {std::uintptr_t inst{};std::uint16_t w1c0{},typeBits{};std::uint32_t udVtRel{},archType{};int udKind{-1},gohF0{-1},compD4{-1},partCount{-1};
    std::array<int,4> partTypes{-1,-1,-1,-1};float massKg{-1.f},radiusCm{-1.f},minMove{};std::uint8_t why{};int stateAfter{-1};
    // the collider's mass before / as set, the composite's own radius, a cache entry, the archetype shared with the type, the
    // approach and the kick, the source (0 press, 1 grab), the frag type's tune name
    float colKgBefore{-1.f},kgSet{-1.f},compRadiusCm{-1.f},approachMps{},kickMps{};int cached{-1},sharedArch{-1};std::uint8_t source{};char tune[24]{};};
// A prop the press made physical: the instance, its gohProp, the last palm on it; the mass set on its collider, where it started and its
// up axis then, since when it is active, how far it moved / tipped, what was counted for it.
// GRAB: furniture whose type never moves that a grab released: its +0x80 bits the release wrote (the root bit 1, the activation override
// bits 7 / 8 of SetDisableActivation) as they were before, the last hold; put back once it sleeps untouched (or on leaving ON).
inline constexpr std::uint32_t movedBits=0x2u|0x180u;
struct MovedProp {std::uintptr_t inst{};std::uint32_t bits{};std::uint64_t touchNs{};};
// src = how it was made physical (UfSource), hand = the hand that did it; asleep = the level holds it inactive again (counted once per
// sleep); clearNs = when the flag was cleared (a flag that comes back without our restore is counted).
struct Unfrozen {std::uintptr_t inst{},goh{};std::uint64_t touchNs{};float kgSet{};V3 pos0{},axis0{};std::uint64_t activeNs{};float movedM{},tiltRad{};
    bool wasActive{},moved{},tipped{},stuck{},snapped{};std::uint8_t src{},hand{};bool asleep{},refrozen{},touchedSince{};std::uint64_t clearNs{},clearStep{};};
// UNFREEZE diagnostics, one per cup (8, the oldest replaced; kept across restores and new activations): contact episodes (a candidate
// after 150 ms without one) and those before the first 2 cm move, the first contact / activation / move clocks, activations by source,
// kicks with the last contact speed and kick, shallow refusals and gated wakes of this cup, sleeps (the last: ms after its activation and
// how far it had moved), touches while asleep, flags that came back without our restore (the last: ms after our clear, the level state,
// back within 5 mm of where it started).
struct CupTrace {std::uintptr_t inst{};std::uint32_t archType{};std::uint64_t firstContactNs{},lastContactNs{},firstActiveNs{},firstMoveNs{},activeNs{};
    std::uint16_t contacts{},touchesToMove{},kicks{},shallow{},gatedLru{},gatedHand{},sleeps{},touchedAsleep{},refrozen{};std::array<std::uint16_t,UfSourceCount> by{};
    float contactMps{-1.f},kickMps{-1.f},sleepMs{-1.f},sleepMovedCm{-1.f},refrozenMs{-1.f};int refrozenState{-1};bool refrozenHome{},moved{};
    // the last CommonReset of it while held: by whom (commonByNames, -1 none), its distance from home then (cm), ms after our clear
    int resetBy{-1};float resetDeltaCm{-1.f},resetMsSinceClear{-1.f};};
// RESETHOLD: a prop the hands made physical whose gringo reset waits: its gohProp and frag instance, since when, out of its 1 cm once.
struct HeldReset {std::uintptr_t goh{},inst{};std::uint64_t sinceNs{};bool left{};};
// A CommonReset of a held prop the tick took from the hooks' ring: the instance, when, by whom, its distance (cm).
struct ResetSeen {std::uintptr_t inst{};std::uint64_t ns{};std::uint8_t by{};float cm{};};
// The first 3 activations, 24 ticks each from the activation: the level state, moved (mm), speed from the position delta (mm/s), touched
// this tick, the touching hand's give (x100), contact age and creep clips since the last tick.
struct CupTick {std::int8_t state{-1};std::int16_t movedMm{},speedMms{};std::uint8_t touched{},give{},age{255},creep{};
    std::uint8_t squeeze{255},held{};std::int16_t handCms{-1};}; // the touching hand's grip x100, bit h = hand h holds the cup, that hand's speed (cm/s, mover frame)
struct CupLine {std::uintptr_t inst{};std::uint8_t src{},n{};std::array<CupTick,24> t{};V3 prev{};std::uint64_t prevNs{},creep0{};};
// A stock wake that made a prop active: its archetype type, level radius, archetype and collider mass (8 distinct).
struct ActivatedSample {std::uintptr_t inst{};std::uint32_t archType{};float radiusCm{-1.f},archKg{-1.f},colKg{-1.f};};
// GLASS: a hand's deepest contact with one frag component in the step (n toward the hand), pending for the tick (glassCands per hand),
// the pane tracked for stock damage.
inline constexpr std::size_t glassCands=4;
// hand / vel = the hand body's position and the controller's velocity (with the mover's) at the hit (GLASS BLOOD's frame)
struct GlassHit {std::uintptr_t inst{};int comp{-1},element{-1},cls{-1};V3 pos{},n{};float depth{},approach{};bool edge{},pending{};std::uint64_t ns{};V3 hand{},vel{};};
struct GlassSample {int cls{-1},comp{-1},group{-1},groups{-1};std::uint32_t archType{};std::uint8_t flags53{},result{},cache{};float minDamage{-1.f},healthBefore{-1.f},
    healthAfter{-1.f},floor{-1.f},approach{};bool set{};};
// GRAB: one finished hold (8 kept): the prop, the outcome, the motion, the drive.
struct HoldRecord {bool set{};int cls{-1};std::uint32_t archType{};std::uint16_t typeBits{};float minMove{},massKg{-1.f},radiusCm{-1.f};int colType{-1},parts{-1};
    bool fixedRot{},move{};char tune[24]{};std::uint64_t heldMs{};std::uint8_t end{};float gripEnd{},gripMin{1.f},triggerMax{},lagMaxCm{};std::array<std::uint32_t,5> lagHist{};
    float pivotMovedCm{},propSpeedMax{},moverSpeedAvg{},moverSpeedMax{},forceAvgN{},forceMaxN{},upMaxN{};std::uint32_t capped{},stale{};
    // the accumulator: foreign forces met, substeps the player's mover stood on the prop, forces zeroed for it, ms into the hold the drive
    // went off (-1 never); ball-jointed substeps without the drive; RIGID CARRY: substeps, the centre of mass's error (cm), the
    // orientation's (deg), the spin about the grip line against the target's (rad/s), the torque (N m), the inertia at the drive mass
    // (kg m^2, read once) and the collider's mass it was read at (kg); substeps the upward force was capped at the prop's weight (the
    // player's mover on it)
    std::uint32_t foreign{},groundHeld{},groundZeroed{},fallback{},carrySubsteps{};float driveOffMs{-1.f},comErrMaxCm{},angErrMaxDeg{},rollRateMax{},tauMaxNm{};
    std::array<float,3> inertia{-1.f,-1.f,-1.f};float carryKg{-1.f};std::uint32_t groundLift{};
    // a ball-jointed hold's grip drive: the hand body to the grip after each substep (mm: <2, 2-5, 5-10, 10-30, 30+), the force at the up /
    // down cap or linear, its reversals (a force against the last one), slew-capped substeps, substeps the hand followed the grip, the grip's
    // step reversals over 3 mm, the grip's speed relative to the centre of mass (the swing, cm/s average), the stall scale's lowest
    bool ball{};std::array<std::uint32_t,5> sepHist{};std::uint32_t vertUp{},vertDown{},vertLin{},flips{},slewed{},follow{},gripAlt{};float swingCms{},stallMin{1.f};
    // substeps driven as one of a pair (the other hand on the same prop), the partner's end when it let go first
    std::uint32_t pairSubsteps{};std::uint8_t partnerEnd{0xff};
    // a heavy hold (heavyHold), its pair substeps at the up cap by share / by grabForceMaxN / at the side cap, the slew's deficit (N summed
    // over slewed substeps), grips snapped to 0 from 0.85 or more in one tick, ticks the theater chord zeroed the grips
    bool heavy{};std::uint32_t upCapShare{},upCapForce{},sideCap{},snapZero{},chordTicks{};float slewDeficitSumN{};
    // the time the drive lag spent over 60 cm without a break (ms); the drive mass of its pair substeps (kg, -1 none)
    std::uint32_t over60Ms{};float driveKg{-1.f};};
// PROPHIT (geometric) per hold: armed ticks, ticks with a body near, the nearest gap and exact distance (m), its creature and clock, the
// last gap, the engine's contacts of the prop with a creature's mover (propScan)
struct HoldGeo {std::uint32_t ticks{},bodyTicks{},eng{};float minGapM{1e9f},minDistM{-1.f},lastGapM{-1.f};std::uint32_t guid{};std::uint64_t minNs{};};
// NPC: one open-hand edge (16 kept).
// arm = the controller's own speed then (m/s), axis = its direction . the player-to-creature line (horizontal), headCm = the contact's height
// over the head capsule's centre (-999 none), cap / capT = the capsule index and the contact's place along it (0..1).
struct EdgeSample {bool set{};int capKind{-1};float a{},speed{},swing{},facing{},side{},tangent{},depthCm{};std::uint8_t verdict{},slapFail{},shoveFail{};bool window{};
    float arm{},axis{},headCm{-999.f};int cap{-1};float capT{-1.f};std::uint8_t tier{0xff};
    // the hand, the creature, a / speed, the gap to the other hand's edge on it (ms, -1 none), a pair, the ungraded rule's verdict
    int hand{-1};std::uint32_t guid{};float ratio{-1.f},gapMs{-1.f};bool pair{};std::uint8_t oldVerdict{};};
// DOORS: the door the FindDoor hook last kept from the player (published by the hook, read by the tick), the last 8 distinct ones.
struct FoundDoor {std::uintptr_t door{},nav{};std::uint64_t ns{};std::uint32_t mode{},state{};std::uint8_t why{};};
inline ::rdrvr::vr::body::Seqlock<FoundDoor> foundDoor;
struct FindSample {bool set{};std::uint32_t state{},mode{};std::uint8_t why{},kind{};float distM{-1.f},heightM{-1.f},widthM{-1.f},cap0{},cap1{};std::uint8_t sideBits{};};
// A plain door Open whose leaf test failed: the palm in the leaf's frame, the box, the state, r and delta (first 4 doors).
struct DoorRejectSample {std::uintptr_t nav{};V3 local{},bmin{},bmax{};std::uint32_t state{};float r{},delta{};std::uint8_t why{};float gapCm{-1.f};int part{-1};bool normalOk{true};};
// A grab candidate met: its class, frag type bits and archetype type (first 8 distinct).
struct GrabSample {int cls{-1};std::uint16_t typeBits{};std::uint32_t archType{};std::uint64_t count{};};
// A refused grab start: why, the class, archetype type, +0x80, +0x12, the type bits and the level state after the wake.
struct GrabRefuseSample {std::uint8_t why{};int cls{-1};std::uint32_t archType{},f80{};std::uint16_t flags12{},typeBits{};int stateAfter{-1};bool set{};std::uint8_t releaseWhy{0xff};};
// The parts of a bound a hand met (GRAB): filled once per instance, archetype and bound; the part matrices are read again every tick.
// move: furniture whose type never moves that the grab may release (grabMoveWhy ok).
// judged = never holds grabNever's verdict (an entry made by a held prop's geometric hit skips it until a grab path reads the entry)
struct GrabCache {std::uintptr_t inst{},arch{},bound{},mats{};unsigned count{};std::uint64_t used{};std::array<GrabPart,24> parts{};std::uint8_t never{};bool move{},judged{};};
struct Dump {
    bool valid{};std::uint32_t moverType{},moverInclude{};std::uint16_t moverIndex{0xffff},moverInstFlags{};int moverState{-1};
    std::uint16_t behCount{},behCap{},levelCount{},levelActive{},levelCap{};std::int32_t poolUsed{},poolCap{};
    int matchFlagsEarly{-1},shouldFindImpacts{-1},behaviourFromArray{-1},delaySap{-1},stepCount{-1},solverIterations{-1},solverMin{-1},solverMax{-1};
    int preComputeImpacts{-1},preApplyImpacts{-1},forceCcd{-1};float allowedPen{-1.f},sepBias{-1.f};std::array<std::uint16_t,2> handFlags{};
};

struct alignas(16) Behaviour {const void* vptr{};void* inst{};std::uint64_t pad[6]{};};
struct alignas(16) HandObjects {
    alignas(16) unsigned char bound[boundBytes]{};
    alignas(16) unsigned char arch[archBytes]{};
    alignas(16) unsigned char inst[instBytes]{};
    alignas(16) unsigned char user[off::userBytes]{}; // the instance's sagInstData (inst+0x18)
    Behaviour beh{};
    // PALM: the composite and its boxes, constructed once per process (Init allocates the part arrays on the engine heap)
    alignas(16) unsigned char comp[compBytes]{};
    alignas(16) unsigned char box[3][boxBytes]{};
    alignas(16) short qverts[3][24]{};
    alignas(16) std::uint32_t partFlags[3][2]{};
    bool compReady{};
};
inline HandObjects objects[2]{};
inline void* behaviourTable[14]{}; // offset-to-top, typeinfo, then 12 slots (address point = &behaviourTable[2])
alignas(16) inline std::uintptr_t instTable[instTableWords]{}; // the own phInst vtable (address point = &instTable[2])
inline std::uintptr_t instVptr{};  // the vptr build() gives our instances (the own table or the stock one)
alignas(16) inline unsigned char reachIter[off::iterBytes]{}; // the propPropIterator REACH WAKE culls into (main thread only)

// vset = the hand's set velocity in the substep the pair first came (the push it brought; world units / s).
struct PropCand {std::uintptr_t inst{};std::uint64_t step{};V3 vset{};};
struct HandState {
    Machine machine{};bool present{},ghost{};std::uint16_t index{off::noIndex};
    std::uint16_t pendingIndex{off::noIndex}; // a delayed delete: our slot stays (its pairs dropped) until the level lets the instance go
    V3 command{},solvedPos{};Rows solvedRows{},cmdRows{};bool haveCommand{},haveSolved{};std::uint64_t solvedStep{},insertStep{};
    V3 prevMoverPos{},moverVel{};std::uint64_t prevMoverNs{};float prevDt{};
    V3 expected{},vset{};bool haveExpected{};float cmdSpeed{}; // where the commanded velocity alone takes the body this substep, that velocity
    ContactEst est{};std::uint64_t contactStep{};std::uint64_t contactNs{};bool edge{};float approach{}; // the contact estimate, a new contact
    std::uint64_t dynStep{}; // the last substep a kept pair met an active prop
    YieldTrack yield{};      // the give of the surface under the palm (yieldStep)
    std::uint64_t retryNs{}; // no insert before this clock after a refused one
    std::array<PropCand,4> props{};unsigned nextProp{};PropCand door{}; // candidates the kept pairs met (CollideObjects, values only)
    std::uint64_t wakeNs{};  // the last contact wake of this hand
    V3 parkPos{},parkN{};    // where a parked body stopped, its contact normal then
    bool fist{};             // PALM: the fist part on (else the fingers)
    float sqLag{-1.f};std::uint64_t sqLagNs{}; // LIGHT GHOST: the squeeze's lag (squeezeLagStep, once per tick), its last step
    bool doorFollow{};       // a door leaf follows this hand: no creep clip (the leaf keeps its clearance ahead of the command)
    std::uint64_t pulseNs{};float devM{};
    // the virtual body: the last substep the command was clipped, its normal / depth (world) / body / capsule, the approach speed
    // (m/s, the most of its first 2 substeps), the arm's own part of it (the hand's motion relative to the player, m/s: a slap is an
    // arm swing, not the player running or the creature walking into a hand), the creature touched last tick
    VirtualHit vhit{};std::uint64_t vhitStep{},vhitStart{};std::uint32_t vhitGuid{};float vhitApproach{},vhitSwing{};std::uint32_t npcTouchGuid{};
    bool edgeVirtual{}; // the pending contact edge came from the virtual body
    // the touch peaks of the same first 2 substeps: the relative speed (m/s), along the surface, the palm-out normal . into the body,
    // the hand's direction . the creature's row a (taken at the approach peak)
    float vhitSpeed{},vhitTangent{},vhitFacing{},vhitSide{};
    // the open-hand touch in progress: the creature, its class, when it started / was classified, a slap in it, the press's push so far
    std::uint32_t touchGuid{};std::uint8_t touchKind{};std::uint64_t touchStartNs{},touchClassNs{};bool touchSlapped{};float touchDepth{},pressDoneM{};
    // GRAB: the kept pairs of grab classes (values only; the prop wakes do not clear them), the last refusal buzz
    struct GrabCand {std::uintptr_t inst{};std::uint64_t step{};int cls{-1};};std::array<GrabCand,4> grabCands{};unsigned nextGrab{};std::uint64_t grabBuzzNs{};
    // the last substep each class was met by a kept pair, the last 4 distinct (class, archetype type) met (latch diagnostics)
    std::array<std::uint64_t,16> classStep{};struct Recent {int cls{-1};std::uint32_t archType{};std::uint64_t step{};};std::array<Recent,4> recent{};unsigned nextRecent{};
    // this tick's door push: the nav instance, the palm's depth past the door (m), the solved palm, contact = a push the 4-substep contact
    // rule counts (always for that rule; DOOR PRESS candidates without a recent 5 mm contact are false)
    struct DoorPush {std::uintptr_t nav{};float depthM{};V3 palm{};bool contact{true};} push{};
    std::uint64_t edgeStep{}; // the substep of the last contact edge (hs.approach is its speed)
    // GLASS: the deepest pane contact of this frame's substeps and its edge track; the pane tracked for stock damage (group health)
    std::array<GlassHit,glassCands> glass{};GlassTracks glassTracks{};std::uintptr_t glassPane{};int glassGroup{-1};float glassHealth{};std::uint64_t glassPaneNs{};
    // NPC: the slap window of the current touch (its creature), the press's escalation record and its diagnostics
    SlapWindow slapWin{};std::uint32_t slapWinGuid{};PressAsk pressAsk{};std::uint64_t pressTicksN{};float pressDepthMax{},pressAskedM{};int edgeSample{-1};float edgeSpeed{};
    // the arm's own speed in the first 2 substeps on the body (m/s); the press's facing now and its lowest
    float vhitArm{};float pressFacingLast{},pressFacingMin{1.f};
    V3 vhitVel{}; // the hand's velocity relative to the body at the approach peak (m/s), the push kick's direction
    // a slap verdict held for the other hand (lever slapholdms): its request, when it was decided
    struct SlapPend {bool set{};std::uint64_t ns{};TouchRequest r{};int edgeSample{-1};} slapPend{};
    // the stroke after a shove edge (lever pushpeak): its peak window and the edge's request (the regrade's copy)
    StrokePeak peak{};TouchRequest peakReq{};
    // UNFREEZE (approach): never-activate props that pass the gates near this palm (the reach query arms them); the controller's world
    // velocity of the last 8 ticks (world units / s)
    // (a fixed prop's level sphere does not move: centre / radius kept from the reach query, no reads until it fires)
    struct NearProp {std::uintptr_t inst{};std::uint64_t armedNs{};V3 centre{};float radius{},gap{};};std::array<NearProp,4> ufNear{};std::array<V3,8> velRing{};unsigned velNext{};
    // PASSIVE contact (lever npcpassive): the arm's own speed into the body (HandTarget::velArm along -n, m/s) at the touch's first 2
    // substeps (the most; vhitVelArm = velArm then, m/s), the player's locomotion into it then (moverVel), the turn part (|vel - velArm|);
    // this tick's contact substeps: the most armIn, the arm's drive past pressDeadMps (armStepM summed, metres), a substep without velArm
    float vhitArmIn{},vhitLocoIn{},vhitTurn{};V3 vhitVelArm{};bool vhitArmOk{};
    float armInTick{},armStepTickM{};bool armTick{},armUnknownTick{};std::uint64_t armPressNs{}; // armPressNs: the last tick of an arm-driven press
    // the touch in progress began with the arm driving into the body (armIn >= activeMps): its bumps may go
    bool touchActive{};
    // the contact episode (passive ring): the creature, its start, the peaks and sums, what was asked and sent
    struct PassiveEp {bool on{};std::uint32_t guid{};int capKind{-1};std::uint64_t startNs{};float aMax{},armInMax{},locoInMax{},turnMax{},depthMax{};double armInSum{};
        std::uint32_t n{};float armStepM{},depthStepM{};std::uint16_t bumpsSent{},bumpsSkipped{};std::uint8_t verdict{};V3 centre0{},centre1{};} pep{};
};
// PASSIVE: one contact episode of an open hand on a creature (8 kept): its creature, hand, capsule kind, length, the peaks of the edge
// approach (a, with the player's locomotion), the arm's own speed into the body (max, mean), the locomotion into it, the turn part, the
// depth, the verdict (TouchKind), the press's arm-limited and depth-law steps summed (cm), the bumps sent / skipped and the creature's
// virtual body centre moved over the episode (horizontal cm).
struct PassiveRec {bool set{};std::uint64_t ns{};std::uint32_t guid{};std::uint8_t hand{},verdict{};int capKind{-1};std::uint32_t ms{};
    float aMax{},armInMax{},armInAvg{},locoInMax{},turnMps{},depthMaxCm{},armStepCm{},depthStepCm{},npcMovedCm{};std::uint16_t bumpsSent{},bumpsSkipped{};};
// A pivot door the palm drives (at most 2). mode: the atan swing, OPEN2 (the door held in its opening state at speed 0, the phase from the
// palm) or POSE4 (an open door posed by the tick in state 4). q = the palm's side at the start (< 0 opens), kOpen = the open axis
// (verified = measured once the leaf turned), lastT = the phase the tick wrote, onNs = the palm last on the leaf, hand = the pushing hand.
enum HoldMode : std::uint8_t {HoldSwing=0,HoldOpen2,HoldPose4,HoldBody};
inline constexpr std::array<const char*,4> holdModeNames{"SWING","OPEN2","POSE4","BODY"};
struct DoorHoldState {std::uintptr_t nav{},door{},iface{},vptr{};DoorSwing swing{};float savedSpeed{1.f};bool dir{},active{};std::uint64_t ticks{};
    HoldMode mode{HoldSwing};int q{-1};V3 kOpen{};float lastT{},theta{};bool verified{},moved{};std::uint64_t onNs{},startNs{};unsigned hand{};
    std::uint32_t startState{};float startTheta{},centreGap{},partGap{},moverHinge{};std::uint64_t moves{};
    // DOOR PRESS: the face the hand works the leaf from (z along the leaf normal), the tick it last pressed, per hold: press ticks, ticks
    // a mover stop held the target, the largest solved-to-command distance (cm), side changes
    int sideZ{};std::uint64_t pressNs{};std::uint32_t presses{},floorHoldsN{},flips{};float maxLagCm{};};
// A door the tick left resting part-open in state 4 (its phase lastT is an opening phase): the hinge, the leaf's width and up, the closed
// leaf's direction and the open axis, its cap and angle (the raise for the mover needs no reads), the tick of its last full check.
struct RestedDoor {std::uintptr_t door{},iface{},nav{};float lastT{};V3 hinge{},up{0,1,0},L0{},k{};float width{},cap{},theta{};std::uint64_t checked{};};
// A door event (16 kept): the door state at the start, the hold mode, the side, the start / end angle, ticks, leaf moves, why it ended, the
// palm centre's and the nearest part's gap to the leaf, the mover's distance from the hinge.
enum DoorEnd : std::uint8_t {DoorEndOpen=0,DoorEndIdle,DoorEndFar,DoorEndOld,DoorEndBody,DoorEndShut,DoorEndGame,DoorEndGone,DoorEndGeom,DoorEndMode,DoorEndCount};
inline constexpr std::array<const char*,DoorEndCount> doorEndNames{"open","idle","far","old","body","shut","game","gone","geom","mode"};
struct DoorEvent {std::uint32_t state{};std::uint8_t mode{},end{};int side{};float startDeg{},endDeg{};std::uint64_t ticks{},moves{};float centreGapCm{},partGapCm{},moverHingeM{};bool set{};
    std::uint32_t presses{},floorHolds{},flips{};float maxLagCm{};};
// DOOR PRESS tick ring (64): a tick of a hold or a refused start: the clock, the hand, the door's low bits, the angle and target (deg), the
// side q, the command's and the solved part's gap (cm), the command part's r / y along the leaf (m), delta (deg), why (DoorTickWhy), the
// mover's angle psi, its margin (deg) and distance from the hinge (m), press = the hold tick's press rule (0 none, 1 the command on or in the
// leaf, 2 through it within 2 cm, 3 further through with the solved part touching the locked face)
enum DoorTickWhy : std::uint8_t {DtMoved=0,DtNoPress,DtOpenStop,DtCloseStop,DtSmall,DtRelease,DtStartNoPress,DtStartShallow,DtStartGated,DtStart,DtWhyCount};
inline constexpr std::array<const char*,DtWhyCount> doorTickWhyNames{"moved","noPress","openStop","closeStop","small","release","startNoPress","startShallow",
    "startGated","start"};
struct DoorTick {bool set{};std::uint64_t ns{};std::uint8_t hand{},why{};std::uint16_t door{};float deg{},targetDeg{};std::int8_t q{};float cmdGapCm{},solvedGapCm{},r{},y{},
    deltaDeg{},psiDeg{},marginDeg{},moverR{};std::uint8_t press{};};
// A grab: the held instance and its part, the constraint and its contact manager / simulator, the collider inertia before the grab.
// The drive: the prop's collider, its type and mass at the start, the substeps its accumulator was still full, off for the rest of the hold;
// rec = the hold's record (written to the ring at the release), the prop's position at the start, a grip dip in progress.
struct GrabHold {bool active{};std::uintptr_t prop{},con{},mgr{},sim{};int comp{-1},cls{-1};std::uint64_t startNs{};GrabHoldGate gate{};
    alignas(16) float savedInertia[4]{};bool inertiaSet{};V3 lastSpin{};float lagM{};
    std::uintptr_t propCol{};int colType{-1};float propKg{};unsigned staleRun{};bool driveOff{};
    std::uint16_t levelIndex{0xffff};int artParts{-1};std::uint64_t applyNs{}; // the prop's level index and part count at the start, the last force
    HoldRecord rec{};V3 pivot0{};bool havePivot{};std::uint64_t moverN{};double moverSum{},forceSum{};std::uint64_t forceN{};std::uint64_t dipNs{};float dipMin{1.f};
    // a ball-jointed hold (grabGrip): the grip in the prop collider's frame at the grab, the last grip force (N), the stall, the grip's last
    // position and step (the reversal count), the swing sum
    bool ball{};V3 gripLocal{},prevN{};GripStall stall{};V3 gripLast{},gripStep{};bool haveGrip{},haveStep{};double swingSum{};std::uint64_t swingN{};
    float lastUpN{}; // TWO HANDS: this hand's up force of the last substep (N)
    float lastDriveLagM{};std::uint8_t lastCapBits{}; // TWO HANDS: the last substep's drive lag (target - grip, m) and caps (1 side, 2 up, 4 down)
    // the last 6 grip / trigger samples of the hold (the hold-end ring), the last grip (the snap count)
    std::array<float,6> gripHist{},trigHist{};std::uint8_t histN{};float prevSq{-1.f};
    // a heavy hold: the mover at the last tick (the teleport end), the time over 60 cm (closed spans, the open span's start)
    V3 moverLast{};bool haveMoverLast{};std::uint64_t moverLastNs{},over60Ns{},over60Since{};
    // the last substep's drive (the pair drive ring): command - grip (world), the spring part before the guard, its trim, the carry,
    // the feed-forward (N), the stall scale
    V3 lastLagV{};float lastSpringN{},lastTrimN{},lastCarryN{},lastFfN{},lastStall{1.f};std::uint8_t lastBits{};
    HoldGeo geo{};
    // the accumulator (force +0x150, torque +0x160) right after the last force on this prop of either hand and its substep (the drive's
    // own force, unconsumed, reads the same at any later substep)
    V3 accAfter{},accTauAfter{};std::uint64_t accStep{};
    // GRAB SEAT / light holds: the prop collider's centre to the palm centre at the attach and its largest / last value per tick (cm), the
    // seat's start distance (cm, -1 none), the grab point that matched (0 palm, 1 / 2 fingers, -1 unknown), the pull's time (ms, -1 none)
    float offsetCm{-1.f},maxOffCm{-1.f},endOffCm{-1.f},seatD0Cm{-1.f};std::int8_t gripPoint{-1};float seatMs{-1.f};
    // GRAB WELD (a fixed-rotation hold): the rotation constraint (0 none), the weld (0 rotation only, 1 position only, 2 both), the flags
    // words +0xbc of both at the attach (0xffff unread), the prop's centre in the hand body's frame at the attach and its largest slip
    // from there since (palmSlip, cm; a turned hand reads 0), the prop-to-palm distance every 250 ms over the first 2 s (cm, -1 not sampled);
    // PLAYER SKIP: the player-capsule contacts the guard switched off for this hold and the deepest (mm)
    std::uintptr_t conRot{};std::uint8_t weld{};std::uint16_t flagsPos{0xffff},flagsRot{0xffff};V3 local0{};bool haveLocal0{};float slipCm{-1.f};
    std::uint32_t capContacts{};float capDepthMm{};
    std::array<float,8> offTrace{-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f};
};
// A hold's end (8 kept per hand): when, why (GrabEnd), heavy, the last 6 grip / trigger samples (oldest first), the hand's speed relative
// to the player (m/s), the lag (cm), held as a pair and the partner's end, the theater chord zeroing the grips at the end, the held ms.
struct HoldEnd {bool set{};std::uint64_t ns{};std::uint8_t end{},partnerEnd{0xff};bool heavy{},pair{},chord{};std::array<float,6> grip{},trig{};std::uint8_t n{};
    float handMps{},lagCm{};std::uint64_t heldMs{};std::uint32_t over60Ms{};HoldGeo geo{};std::uint64_t endNs{};};
// PROPHIT: a creature's mover capsule in the held prop's impact iterator (the deepest contact per capsule of this tick's substeps): the
// capsule, the hand, the contact point and normal (from the prop into the capsule), the prop's velocity at the point (world units / s).
struct PropContact {std::uintptr_t mover{};unsigned hand{};V3 pos{},n{},vPoint{};float depth{},vn{};};
// A contact edge the tick judged: when, the approach and speed (m/s), the verdict, strong, the creature's guid, n . up.
struct PropHitSample {std::uint64_t ns{};float approach{},swing{},speed{},nUp{};std::uint8_t why{0xff},strong{};std::uint32_t guid{};};
// TWO HANDS: the pair plan of one substep (pairStep, computed by the first hand to drive in it): the targets and limits per hand.
// carry = the statics carry per hand (lever grabpairstatics; statics = in use), guard = the player's capsule met in the last 2 substeps
// (lever grabplayerguard: guardN = the normal from the prop into it)
struct PairCache {std::uint64_t step{};bool ok{},wasOk{};PairPlan plan{};std::array<PairLimits,2> lim{};float share0{.5f},rollSign{};
    PairCarry carry{};bool statics{},guard{};V3 guardN{};
    // the drive mass (kg: the held mass while set, else the prop's), the acceleration cap it is driven at, the command accelerations
    // (lever grabfeedfwd) and their feed-forward forces (N), the pair's player skip on (no guard trim)
    float driveKg{},accel{};std::array<CmdAccel,2> acc{};std::array<V3,2> ffN{};bool skip{};
    // RIGID CARRY (lever grabcarry): the frame and its angular velocity (low-passed), the pose at the join, the plan of this substep
    // (carryDrive), the inertia at the drive mass (kg m^2), the substep the first hand applied it in (the other hand then follows its grip)
    bool carryOk{};CarryState cs{};CarryOut co{};V3 inertia{};std::uint64_t carryStep{};
    // the collider's mass the carry drives (+0xac, kg); the join pose's holds (both holds' start), collider and last carry substep's time
    float massKg{};std::uint64_t csStartA{},csStartB{},csNs{};std::uintptr_t csCol{};};
// The grip of a ball-jointed hold this substep, from the prop's collider (rows +0xd0, centre of mass +0x100, velocity +0x110, angular
// velocity +0x120): the point, its velocity, the centre of mass's (raw reads of a collider the level holds active at the grab's index).
struct GripNow {V3 grip{},vGrip{},vCom{},com{},w{};bool ok{};Rows r{};};
inline GripNow gripNow(std::uintptr_t col,V3 local) noexcept {
    GripNow g{};if(!col)return g;
    Rows r{};for(unsigned i=0;i<3;++i){float f[3];std::memcpy(f,reinterpret_cast<const void*>(col+off::colliderRows+i*0x10),12);r[i]={f[0],f[1],f[2]};}
    float o[3],vl[3],w[3];std::memcpy(o,reinterpret_cast<const void*>(col+off::colliderPos),12);std::memcpy(vl,reinterpret_cast<const void*>(col+off::colliderVel),12);
    std::memcpy(w,reinterpret_cast<const void*>(col+off::colliderAngVel),12);
    const V3 c{o[0],o[1],o[2]};g.r=r;g.grip=fromFrame(r,c,local);g.vCom={vl[0],vl[1],vl[2]};g.com=c;g.w={w[0],w[1],w[2]};g.vGrip=v::add(g.vCom,v::cross(g.w,v::sub(g.grip,c)));
    g.ok=v::finite(g.grip)&&v::finite(g.vGrip);return g;
}
struct State {
    unsigned mode{ModeOff};Levers lev{};bool leversRead{};std::uint64_t leverIndex{};
    std::uintptr_t sim{},level{};std::array<bool,2> listed{};bool built{};Levers builtWith{};float builtUpm{1};unsigned builtShape{};std::uint32_t archType{},archInclude{};
    std::array<HandState,2> hand{};
    std::uintptr_t mover{},moverInstAddr{},actor{};bool moverOk{};std::uint32_t moverType{},moverInclude{};
    float upm{1};V3 lastEye{};std::uint64_t lastEyeNs{};bool haveEye{};
    const void* mainKey{};std::uint64_t preNs{};bool preOpen{};std::uint64_t step{};std::uint64_t lastTickNs{};unsigned lastTickMode{3};
    Targets tg{};bool tgFresh{},tgUsable{};
    Solved out{};
    DoorGate doors{};WakeLru wakes{};std::uint64_t reachTick{};BumpGate bumps{};std::uint64_t nearbyNs{};
    std::array<PairSample,8> samples{};Dump dump{};std::uint64_t dumpNs{};
    // the player's own instances (ConfigureActorExcludeInsts through the ARC ledger: his mover, frag, inventory items, mount), compare-only
    std::array<std::uintptr_t,20> own{};int ownCount{};std::uint64_t ownIndex{};std::array<std::uint8_t,2> ownCls{};
    std::uintptr_t guardSim{}; // the simulator whose PreApplyImpacts fn word holds preApplyGuard
    bool flagsBad{};           // the derived or lever type / include is 0: no insert
    // DOOR SWING
    std::array<DoorHoldState,2> holds{};std::array<std::uintptr_t,4> gapNav{};std::array<std::uint64_t,4> gapUntil{};unsigned gapNext{};
    // DOOR PRESS: the rearm of a shut gates only the hand and face that shut it (-1 / 0: every hand / face); the face each hand works
    // a door from; the tick ring; per hand the door and clock of the last refused start sampled (startPress: one per doorNoteNs)
    std::array<std::int8_t,4> gapHand{-1,-1,-1,-1},gapSide{};std::array<DoorSideTrack,2> doorSide{};std::array<DoorTick,64> doorTicks{};unsigned doorTickNext{};
    struct DoorNote {std::uintptr_t nav{};std::uint64_t ns{};};std::array<DoorNote,2> doorNote{};
    std::uint64_t doorTickNs{},lockedPulseNs{};
    // props: the nav instances CollideObjects already judged for the unlock; refused wake samples
    std::array<std::uintptr_t,8> unlockSeen{};unsigned unlockNext{};std::array<RefusedSample,8> refusedSamples{};
    // UNFREEZE: the props made physical (their flag goes back), the verdicts that do not change (8), samples; NEVERMOVE samples
    std::array<Unfrozen,8> unfrozen{};std::array<MovedProp,4> moved{};struct UfVerdict {std::uintptr_t inst{};std::uint8_t why{};};std::array<UfVerdict,8> ufSeen{};unsigned ufSeenNext{};
    std::array<UnfreezeSample,8> ufSamples{},neverMoveSamples{};
    // NPC: the virtual bodies of this tick (count 0 = none), the press / slap gates, the last slap serial
    VBodies vb{};bool vbFresh{};PressGate press{};SlapGate slaps{};std::uint64_t touchSerial{},bumpAskSeen{};
    // DOORS: the rested part-open doors, the plain Opens' leaf samples
    std::array<RestedDoor,4> rested{};unsigned restedNext{};std::array<DoorRejectSample,4> doorRejects{};std::array<DoorEvent,16> doorEvents{};unsigned doorEventNext{};
    std::uint64_t restedTicks{};
    // GRAB: per physical hand the hold, the press serial seen, the candidate of this tick (instance, part, class); samples
    std::array<GrabHold,2> grab{};std::array<std::uint32_t,2> grabSerial{};std::array<std::uintptr_t,2> grabCand{};std::array<int,2> grabCandPart{-1,-1},grabCandCls{-1,-1};
    std::array<std::int8_t,2> grabCandPt{-1,-1}; // GRAB SEAT (diagnostics): the grab point of this tick's candidate
    std::array<GrabSample,8> grabSamples{};
    // GRAB: the parts cache per hand, the last candidate (the ready hold-over), the latch serial seen, a latch the grab did not take
    // (its clock, until a ready or 200 ms), refusal samples, the (class, archetype) met before latches
    std::array<std::array<GrabCache,4>,2> grabCache{};struct GrabLast {std::uintptr_t inst{};int part{-1},cls{-1};std::uint64_t ns{};};std::array<GrabLast,2> grabLast{};
    std::array<std::uint32_t,2> latchSeen{};std::array<std::uint64_t,2> latchPendingNs{};std::array<GrabRefuseSample,4> grabRefuses{};unsigned grabRefuseNext{};
    std::array<GrabSample,8> latchSamples{};
    // CPU per mode: the thread CPU clock and the wall clock of the last tick; the costs of the window in progress
    std::uint64_t cpuLast{};PendingCost pend{};
    // the mover verified for tg.mover at tick index moverCheckIdx (again every 16 ticks or for another mover); REACH WAKE: the palms of
    // the last query
    std::uintptr_t moverChecked{};std::uint64_t moverCheckIdx{};std::array<V3,2> reachPalm{};std::uint8_t reachLive{};
    // DOORS (approach): the last two distinct doors the hook kept from the player (double doors), the stuck timer, the hook's samples
    struct Kept {std::uintptr_t door{},nav{};std::uint64_t ns{};};std::array<Kept,2> kept{};StuckTimers stuck{},bodyFlip{};
    std::array<FindSample,8> findSamples{};unsigned findNext{};std::uintptr_t findLastDoor{};std::uint8_t findLastWhy{0xff};
    // GLASS: the per-pane rate gate, samples, the distinct panes met (count only)
    GlassRate glassRate{};std::array<GlassSample,8> glassSamples{};unsigned glassSampleNext{};
    // GLASS BLOOD: the gate and the last 4 drops (hand, clock, the frame passed, the camera distance, the rule found)
    // BLOOD ROUTE: the rule's draw list byte (-1 unread), its vtable the ptxEffectRuleStd one, the low-res switch (-1 unread), routed to
    // list 0, the instance found at its list's head after the call (1 made, 0 not, -1 unread), the rule's cull radius / distance (+0x344 /
    // +0x348, raw)
    struct BloodSample {bool set{};std::uint8_t hand{};std::uint64_t ns{};V3 pos{},n{},dir{};float camM{-1.f};bool rule{};
        std::int16_t list{-1};bool vtOk{};std::int16_t lowRes{-1};bool routed{};std::int8_t made{-1};float cullR{-1.f},cullD{-1.f};
        // BLOOD PLACE: the mode used (0 contact frame, 1 outside the outline, 2 contact frame for want of an eye), the drop's distance from
        // the eye (m), the face of the hand the eye sees, the contact frame's normal and velocity toward the eye (cosines), whether the
        // contact frame's point and the used point lie inside the hand's outline from the eye
        std::uint8_t place{};float eyeM{-1.f};std::int8_t side{};float facingN{},facingDir{};std::int8_t occlOld{-1},occlNew{-1};};
    // BLOOD LIFE: the last drops' instances read on the tick while watched (1.5 s): the slot still holding the rule, the attach (mode 2 on
    // the hand's matrix), the time line's events and emitters, the largest element count word seen, the world position's distance from the
    // eye (first / last read) and its travel, the smallest fade (+0x290) and the alpha (+0x250); stop: 1 expired, 2 slot recycled, 3 read failed
    struct BloodLife {bool set{};std::uint8_t hand{},stop{};std::uintptr_t slot{},rule{},matrix{};std::uint64_t armNs{},untilNs{},lastNs{};
        std::uint32_t reads{},events{},emitters{},maxCount{},liveReads{};std::int32_t mode{-1};bool attachOk{};float eyeFirstM{-1.f},eyeLastM{-1.f},travelM{},
        fadeMin{-1.f},alpha{-1.f};V3 first{};bool haveFirst{};};
    std::array<BloodLife,4> bloodLife{};unsigned bloodLifeNext{};
    // GUN GLASS: per hand the last tick's muzzle / palm (mover-relative), the gun placed since, the last gun handling, the panes' edges
    // (by this tick count); the sample ring (src 0 muzzle, 1 barrel, 2 free palm; result = GlassWhy, GlassWhyCount = no shatter)
    struct GunMem {bool haveTip{},havePalm{};V3 prevTip{},prevPalm{};std::uint64_t gunSince{},busyNs{};std::uint8_t busyBits{};GlassTracks tracks{};};
    std::array<GunMem,2> gunMem{};std::uint64_t gunStep{};
    struct GunSample {bool set{};std::uint64_t ns{};std::uint8_t src{},hand{},result{},lever{};std::int8_t kind{-1};int cls{-1},comp{-1},part{-1};float approach{},tipMps{},alongCm{};};
    std::array<GunSample,8> gunSamples{};unsigned gunSampleNext{};
    BloodGate bloodGate{};std::array<BloodSample,4> bloodSamples{};unsigned bloodSampleNext{};std::array<std::uintptr_t,16> glassPanes{};unsigned glassPaneNext{};
    // UNFREEZE / GRAB / NPC diagnostics
    std::array<ActivatedSample,8> activatedSamples{};std::array<HoldRecord,8> holdRecords{};unsigned holdNext{};
    std::array<EdgeSample,16> edgeSamples{};unsigned edgeNext{};
    float doorDt{}; // the doors tick's interval (s)
    TouchNodes nodes{}; // NPC: the reaction node levers as published to VrMelee
    // UNFREEZE: props whose gates passed (the approach judges a prop once), the cup traces and timelines
    std::array<std::uintptr_t,8> ufOk{};unsigned ufOkNext{};std::array<CupTrace,8> cups{};std::array<CupLine,3> cupLines{};unsigned cupLineNext{};
    // RESETHOLD: the held props (published to heldGoh), the ring's last clock taken, the CommonResets seen, the tick of the last check
    std::array<HeldReset,16> resetHold{};std::uint64_t resetRingNs{};std::array<ResetSeen,8> resetSeen{};unsigned resetSeenNext{};std::uint64_t resetCheckTick{};
    PairCache pair{}; // TWO HANDS
    // TWO HANDS diagnostics: the join attempts (8), the pair episodes (8) and the one in progress, the releases of a hand that left LIVE (4),
    // the last refusal per hand, the inputs of the body machine's want per hand (wantBits: mode on, targets usable, mover ok, arch, flags
    // ok, target valid, class body, on foot, not carried, jump)
    struct JoinRec {std::uint64_t ns{};std::uint8_t hand{},result{0xff},cls{};std::int32_t distMm{-1},part{-1};bool set{};};
    std::array<JoinRec,8> joinRing{};unsigned joinNext{};
    // a pair episode: its length, the 3-D share, rise / drop, speed, the summed up force's peak and mean, the weight; the grip line (spacing,
    // vertical and horizontal parts at the join, cm), the horizontal lever share, the statics couple (mean, max N), the substeps any hand sat
    // at its side / down cap, the prop's turn since the join (deg), the player's contacts (substeps, deepest mm), the mover's travel (cm),
    // the guard's trims, the first substep the drive lag passed 20 / 60 cm (ms from the join, -1 never)
    struct PairRec {std::uint64_t ns{},ms{};std::uint32_t substeps{};float shareAvg{},riseMaxCm{},dropMaxCm{},speedMaxCms{},upMaxN{},weightN{};bool set{};
        float upAvgN{},dCm{},vertCm{},horizCm{},shareHAvg{},coupleAvgN{},coupleMaxN{},turnMaxDeg{},playerDepthMm{},moverCm{},lag20Ms{-1.f},lag60Ms{-1.f};
        std::uint32_t sideCap{},downCap{},playerSubsteps{},guardTrims{};std::uint8_t statics{};unsigned start{0xffffffffu};};
    std::array<PairRec,8> pairRing{};unsigned pairNext{};
    struct PairEp {bool on{};std::uint64_t startNs{},lastNs{};float y0{};double shareSum{};std::uint32_t n{};float riseMax{},dropMax{},speedMax{},upMax{},weightN{-1.f};
        double upSum{},shareHSum{},coupleSum{};float dCm{},vertCm{},horizCm{},coupleMax{},turnMax{},playerDepth{},moverMax{},lag20Ms{-1.f},lag60Ms{-1.f};Rows r0{};V3 mover0{};
        std::uint32_t sideCap{},downCap{},playerSubsteps{},guardTrims{};bool statics{};unsigned start{0xffffffffu};} pairEp{};
    // the first 30 substeps of the last 8 pair episodes (pairStart: each the previous substep's lag A / B (cm), the centre of mass's rise
    // (mm), the turn since the join (deg), up force A / B (N / 10), the couple (N / 10), bits: side cap A 1 / B 2, up cap A 4 / B 8, down cap
    // A 16 / B 32, player contact 64)
    static constexpr unsigned pairStartN=30;
    struct PairStart {std::uint64_t ns{};std::uint32_t n{};std::array<std::array<std::int16_t,8>,pairStartN> s{};};
    std::array<PairStart,8> pairStarts{};unsigned pairStartNext{};
    // the player's own mover met by a held heavy prop (propScan): the substep, the normal from the prop into it, the depth (world units)
    std::uint64_t playerContactStep{};V3 playerContactN{};float playerContactDepth{};
    // PASSIVE: the contact episodes
    std::array<PassiveRec,8> passiveRing{};unsigned passiveNext{};
    // PROPHIT (geometric): the gate, the touch ring, the prop's last sample (centre of mass, velocity, angular velocity, clock), the holding
    // hands' arm velocity trails (m/s)
    PropGeoGate geoGate{};
    struct GeoRec {bool set{};std::uint64_t ns{};std::uint32_t guid{};int part{-1};std::uint8_t zone{},kind{},why{},strong{},engine{};
        float distCm{},a{},aNow{},aPrev{},swing{},arm{},armDir{},armAgeMs{-1.f},propMps{},moverMps{},bodyMps{};};
    std::array<GeoRec,8> geoRing{};unsigned geoNext{};
    struct PropPrev {std::uintptr_t prop{};V3 com{},v{},w{};std::uint64_t ns{};};std::array<PropPrev,2> propPrev{}; // per holding hand
    std::array<ArmTrail,2> armTrail{};
    std::array<std::array<HoldEnd,8>,2> holdEnds{};std::array<unsigned,2> holdEndNext{};std::array<float,2> lastHandMps{};
    struct BodyEnd {std::uint64_t ns{};std::uint8_t hand{},present{},state{},wantBits{};float devM{};bool set{};};
    std::array<BodyEnd,4> bodyEnds{};unsigned bodyEndNext{};
    std::array<std::uint8_t,2> grabLastRefuse{0xff,0xff};std::array<std::uint16_t,2> wantBits{};
    // the substep clock whichever hand's PreUpdate runs first in the sim's list (preSeen: the hands seen since it last moved)
    std::uint64_t substep{1};std::uint8_t preSeen{};
    // PROPHIT: the creature contacts the held prop's impact iterators gave since the last tick (the step fills, the tick takes), the gate,
    // the last 8 hits and refusals
    std::array<PropContact,3> propContacts{};PropHitGate propGate{};std::array<PropHitSample,8> propSamples{};unsigned propSampleNext{};std::uint64_t propSerial{};
    // THROW: each hand's open edge on a nav hold and the first edge of a pair's hand that waits for the other (0 none); the props in flight
    // (prop, simulator, level and its slot, the collider at launch, the real kg, the radius (world units), the throwing hand (its parts
    // cache) and the hands, launch time and speed, top speed (m/s), the start and last centre of mass, the last velocities and their tick,
    // slow since, hits, taps, the nearest gap to a body (cm), heavy); the creature contacts of each flight's impact iterator since the
    // last tick; the hand-prop pairs dropped after a throw (until, prop); the decide / flight / hit rings (8 each)
    std::array<ThrowEdge,2> throwEdge{};std::array<std::uint64_t,2> throwWait{};
    struct Flight {bool on{};std::uintptr_t prop{},sim{},level{},col{};std::uint16_t levelIndex{0xffff};float kg{},radiusW{};unsigned hand{};std::uint8_t hands{};
        std::uint64_t t0{},slowSince{},lastNs{};float launchMps{},maxMps{},gapCm{1e9f};V3 start{},lastCom{},lastV{},lastW{};std::uint8_t hits{},taps{};bool heavy{};};
    std::array<Flight,2> flights{};std::array<std::array<PropContact,3>,2> flightContacts{};
    std::array<std::uint64_t,2> throwDropUntil{};std::array<std::uintptr_t,2> throwDropProp{};
    struct ThrowDecide {std::uint64_t ns{};std::uint8_t hand{},hands{},verdict{},heavy{},pair{},written{},grabStep{};float ageMs{},grip{},gripOther{-1.f},peak{},peakEdgeMs{},est{},upFrac{},
        propMps{},launchMps{},kg{};};
    std::array<ThrowDecide,8> throwDecides{};unsigned throwDecideNext{};
    struct FlightRec {std::uint64_t ns{};float kg{},launchMps{},maxMps{},ms{},distCm{},gapCm{};std::uint8_t hits{},taps{},end{},heavy{};};
    std::array<FlightRec,8> flightRing{};unsigned flightRingNext{};
    struct ThrowHit {std::uint64_t ns{};std::uint32_t guid{};std::uint8_t zone{0xff},kind{0xff},why{},strong{},light{},engine{},sent{};float a{},kg{},ko{};};
    std::array<ThrowHit,8> throwHits{};unsigned throwHitNext{};
    // HELD MASS (lever grabpairkg): the collider whose mass the pair lowered: the prop, the collider, its level index, the simulator, the
    // mass before and the mass set (kg)
    struct HeldMass {bool set{};std::uintptr_t prop{},col{},sim{},level{};std::uint16_t levelIndex{0xffff};float kg0{},kg{};} heldMass{};
    // PLAYER SKIP: the functor word is ours; per hand the held prop the precompute guard serves and its level index, two linger slots
    // for released props (skipSlotsStep: the prop, its level index, its start, the last substep it switched a contact off and that
    // substep's deepest contact, world units; a new hold never ends one, a level slot that no longer holds its prop does); per held slot
    // the contacts switched off in the last substep (and that substep, its deepest contact)
    bool skipOurs{};SkipSlots skip{};std::uint64_t skipSeenNs{};std::array<std::uint64_t,2> skipStep{};std::array<unsigned,2> skipStepContacts{};
    std::array<float,2> skipStepDepth{};
    // PAIR DRIVE ring (64): every 4th pair substep and every one with a lag over 20 cm: [ms / 10 since the join, lag A mm, lag B mm,
    // the larger lag's vector in the mover's frame (forward, side, up mm), its spring N, trim N, carry N, aCmd (m/s^2 x 10), feed-forward N,
    // stall x 100, mover cm/s, player depth mm, contacts skipped in the substep, bits: 1 capped, 2 slewed, 4 guard, 8 skip, 16 drive mass]
    std::array<std::array<std::int16_t,16>,64> pairDriveRing{};unsigned pairDriveNext{};std::uint64_t pairDriveN{};
    // STALE RING (16): each substep the accumulator held a force at the drive: [ms into the hold, hand, substep & 0x7fff, accumulator x,
    // y, z (N), |the drive's own force after its last apply| (N), |accumulator - that| (N), kind (0 own unconsumed, 1 foreign, 2 runaway,
    // 3 zeroed for the player's ground), the mover's ground index, its generation, the prop's level index, the ground is the prop, stale
    // run, |torque accumulator| (N m), the player's mover's vertical speed (cm/s)]
    std::array<std::array<std::int32_t,16>,16> staleRing{};unsigned staleNext{};std::uint64_t staleN{};
    // CARRY RING (32): every 2nd carry substep and each with an error over 15 cm or 15 deg: [ms / 10 since the join, the centre of mass's
    // error forward / side / up (mm, the mover's frame), its speed against the target (cm/s), the orientation's error about the grip line /
    // y / z (deg x 10, the carry frame), the spin about the grip line against the target's (deg/s), |F| (N), |tau| (N m x 10), caps (1 force,
    // 2 torque, 4 upward force at the prop's weight), mover cm/s, the ground is the prop, the frame's turn this substep (deg x 10), the
    // mover's vertical speed (cm/s)]
    std::array<std::array<std::int16_t,16>,32> carryRing{};unsigned carryNext{};std::uint64_t carryN{},carryW{};
    // PROPHIT (geometric): the near ring (a creature's gap under 30 cm opens an entry, over 50 cm re-arms it): the clock, the creature,
    // the gap and exact distance (cm), the part, the capsule kind, how far the test went (bodyWhy), a, swing, arm, armDir, the prop's and
    // the mover's speed, the lag, ms into the hold, the hands, engine contacts of the hold
    struct GeoNear {bool set{};std::uint64_t ns{};std::uint32_t guid{};float gapCm{},distCm{-1.f};int part{-1};std::uint8_t kind{},why{},hands{};float a{},swing{},arm{},armDir{},
        propMps{},moverMps{},lagCm{};std::uint32_t msIn{},eng{};};
    std::array<GeoNear,8> geoNear{};unsigned geoNearNext{};std::array<std::uint32_t,8> geoNearOpen{};std::array<std::uint64_t,8> geoNearOpenNs{};
    // LIGHT GHOST: per hand the ghosted props (lightGhostFilter), the level they belong to
    std::array<std::array<LightGhost,2>,2> ghosts{};std::uintptr_t ghostLevel{};
    // LIGHT HOLDS (4, fixed-rotation holds only, chairs never push them out): ms held, end, the prop's top speed in the hold, the prop's and the
    // hand's speed at the release, the prop's top speed within 300 ms after it (m/s), a ghost armed at the release
    struct LightHold {bool set{};std::uint64_t ns{},heldMs{};std::uint8_t end{},ghost{},hand{};float kg{},propSpeedMax{},propMpsAtEnd{-1.f},handMpsAtEnd{-1.f},afterMaxMps{-1.f};
        std::uintptr_t prop{};std::uint16_t levelIndex{0xffff};std::uint64_t watchUntil{};
        float offsetCm{-1.f},maxOffCm{-1.f},endOffCm{-1.f},seatD0Cm{-1.f},seatMs{-1.f};std::int8_t gripPoint{-1};
        std::uint8_t weld{};std::uint16_t flagsPos{0xffff},flagsRot{0xffff};float slipCm{-1.f};std::array<float,8> offTrace{-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f,-1.f};
        std::uint32_t capContacts{};float capDepthMm{};};
    // GRAB SEAT: per hand, the prop being pulled into the palm before its attach (the collider at the start, its level index, the seat
    // point in the collider's frame, the part / class / grab point of the grab, start and distance)
    struct SeatPull {bool on{};std::uintptr_t prop{},col{},sim{};std::uint16_t levelIndex{0xffff};int part{-1},cls{-1};std::int8_t pt{-1};V3 seatLocal{};
        std::uint64_t startNs{},openNs{};float d0{};};
    std::array<SeatPull,2> seat{};
    std::array<LightHold,4> lightHolds{};unsigned lightHoldNext{};
};
namespace detail {
inline State& state() noexcept {static State s;return s;}
inline const void* threadKey() noexcept {static thread_local char key;return &key;}
template<class T> inline T raw(std::uintptr_t a) noexcept {T v;std::memcpy(&v,reinterpret_cast<const void*>(a),sizeof v);return v;}
template<class T> inline void put(std::uintptr_t a,T v) noexcept {std::memcpy(reinterpret_cast<void*>(a),&v,sizeof v);}
template<class T> inline bool get(std::uintptr_t a,T& v) noexcept {return engine.read&&a&&engine.read(nullptr,a,&v,sizeof v);}
inline std::uintptr_t addr(const void* p) noexcept {return reinterpret_cast<std::uintptr_t>(p);}
inline void addNs(Counter& sum,Counter& mx,std::uint64_t ns) noexcept {sum.fetch_add(ns,std::memory_order_relaxed);maxOf(mx,ns);}
inline ModeStats& modeStats(unsigned mode) noexcept {return stats.byMode[mode<3?mode:0];}
inline int handOf(const void* self) noexcept {return self==&objects[0].beh?0:self==&objects[1].beh?1:-1;}
inline std::uintptr_t instOf(unsigned h) noexcept {return addr(objects[h&1u].inst);}
inline V3 vec3(std::uintptr_t a) noexcept {float f[3];std::memcpy(f,reinterpret_cast<const void*>(a),sizeof f);return {f[0],f[1],f[2]};}
inline Rows rows3(std::uintptr_t m) noexcept {return {vec3(m),vec3(m+0x10),vec3(m+0x20)};}
inline bool palm(const State& s) noexcept {return s.builtShape==ShapePalm;}

// ---- DOORGATE hooks (the caller's thread; atomics, guarded reads and the original only) ----
using FindDoorFn=bool(*)(void*,void*,void*,void*);using DoorOpenFn=void(*)(void*,const void*,bool);using DoorCloseFn=void(*)(void*,bool);
inline std::atomic<FindDoorFn> findDoorOriginal{};inline std::atomic<DoorOpenFn> openOriginal{};inline std::atomic<DoorCloseFn> closeOriginal{};
// armedNs = the tick's clock while the hands are live on foot and the gate levers allow it (the hook passes through 200 ms after the last);
// autoLever = doorauto; ours = the doors the hands hold or rested; handBack = such doors an Open asked for (the tick hands them back);
// moverPub = the player's mover (a player Open's pusher)
inline std::atomic<std::uint64_t> doorArmedNs{};inline std::atomic<bool> doorAutoLever{true};
inline std::array<std::atomic<std::uintptr_t>,6> ourDoors{};inline std::array<std::atomic<std::uintptr_t>,4> handBack{};
struct MoverPub {V3 pos{};std::uint64_t ns{};};inline ::rdrvr::vr::body::Seqlock<MoverPub> moverPub;
inline bool ourDoor(std::uintptr_t d) noexcept {if(!d)return false;for(const auto& x:ourDoors)if(x.load(std::memory_order_relaxed)==d)return true;return false;}
// The hook's own return address (the PLT caller's bl + 4): taken in the hook's frame itself, never through a helper call.
#if defined(__GNUC__)||defined(__clang__)
#define RDRVR_HP_RETURN_ADDRESS() __builtin_return_address(0)
#else
#define RDRVR_HP_RETURN_ADDRESS() nullptr
#endif
// sagPlayerInputDoor::FindDoor(handle, bool&, actor): the original first (its flags and VerifyDoor stay stock), then "no door" for the
// automatic walk-in open (findDoorGate) of the player's foot input while armed. The door kept from him is published for the tick.
inline bool findDoorHook(void* self,void* handle,void* flag,void* actor) noexcept {
    const auto orig=findDoorOriginal.load(std::memory_order_acquire);
    if(!orig)return false;
    const bool found=orig(self,handle,flag,actor);
    try{
        auto& D=stats.doors;bump(D.fdCalls);if(found)bump(D.fdFound);
        const auto now=nowNs();const auto foot=playerFoot.load(std::memory_order_relaxed);
        const bool live=fresh(now,doorArmedNs.load(std::memory_order_relaxed),200'000'000ull);
        const bool onFoot=foot&&addr(self)==foot+footInputDoor;
        std::uint32_t mode{};bool modeRead=false;std::uintptr_t a{},b{},c{};
        if(found&&get(addr(actor)+off::modeAnim,a)&&a&&get(a+off::modeBlock,b)&&b&&get(b+off::modeObj,c)&&c&&get(c+off::modeWord,mode))modeRead=true;
        const FindDoorWhy why=findDoorGate(found,doorAutoLever.load(std::memory_order_relaxed),live,onFoot,modeRead,mode);
        bump(D.fdBy[why]);
        if(found&&engine.doorGet){
            FoundDoor f{};f.ns=now;f.mode=mode;f.why=std::uint8_t(why);
            f.door=addr(engine.doorGet(handle));
            if(f.door){get(f.door+off::doorFrag,f.nav);get(f.door+off::doorState,f.state);bump(D.fdState[f.state<6?f.state:5]);foundDoor.store(f);}}
        if(why==FindSuppress)return false;
    }catch(...){}
    return found;
}
inline unsigned callerIndex(const std::uintptr_t* list,unsigned n,void* ra) noexcept {
    const auto r=addr(ra);for(unsigned i=0;i<n;++i)if(r==engine.base+list[i])return i;return n;}
// propDoorInst::Open / Close through the PLT: counted by caller and state; an Open of a door the hands left part-open in state 4 (stock
// Open does nothing in state 4: an NPC would walk into it) is queued for the tick, which hands it back to the engine's opening.
inline void doorOpenHook(void* door,const void* pos,bool b) noexcept {
    void* ra=RDRVR_HP_RETURN_ADDRESS();
    try{auto& D=stats.doors;const auto d=addr(door);
        const unsigned c=callerIndex(off::openCallers,5,ra);bump(D.openBy[c]);
        std::uint32_t st{9};if(get(d+off::doorState,st))bump(D.openState[st<6?st:5]);
        if(c==0&&pos){MoverPub m{};float p[3]{};if(moverPub.load(m)&&get(addr(pos),p)&&v::length(v::sub(V3{p[0],p[1],p[2]},m.pos))<.15f)bump(D.openPlayer);}
        if(ourDoor(d)){bump(D.openOnOurs);
            if(st==off::doorStateOpened){bool q=false;for(auto& x:handBack){std::uintptr_t z=0;if(x.load(std::memory_order_relaxed)==d){q=true;break;}
                    if(x.compare_exchange_strong(z,d)){q=true;break;}}
                if(!q)bump(D.handBackFails);}}
    }catch(...){}
    if(const auto o=openOriginal.load(std::memory_order_acquire))o(door,pos,b);
}
inline void doorCloseHook(void* door,bool b) noexcept {
    void* ra=RDRVR_HP_RETURN_ADDRESS();
    try{auto& D=stats.doors;bump(D.closeBy[callerIndex(off::closeCallers,4,ra)]);if(ourDoor(addr(door)))bump(D.closeOnOurs);}catch(...){}
    if(const auto o=closeOriginal.load(std::memory_order_acquire))o(door,b);
}
// ---- RESETHOLD hooks (the caller's thread: atomics, guarded reads and the originals only) ----
// held = the gohProps of props the hands made physical (resetHoldTick publishes them); holdLive = the hold is on (ON, levers, groups).
using StartResetFn=bool(*)(void*,std::uint32_t);using CommonResetFn=void(*)(std::uint32_t);using ApplyResetFn=bool(*)(std::uint32_t,std::uint32_t,void*);
inline std::atomic<StartResetFn> startResetOriginal{};inline std::atomic<CommonResetFn> commonResetOriginal{};inline std::atomic<ApplyResetFn> applyResetOriginal{};
inline std::array<std::atomic<std::uintptr_t>,16> heldGoh{};inline std::atomic<bool> holdLive{};inline std::atomic<float> holdUpm{1.f};
inline bool heldHas(std::uintptr_t goh) noexcept {if(!goh)return false;for(const auto& x:heldGoh)if(x.load(std::memory_order_relaxed)==goh)return true;return false;}
// The gohBase of a gohGUID (index & 0xffff, generation >> 16) as CommonReset 0x24caa68..0x24caad0 finds it; 0 when the slot holds another.
inline std::uintptr_t gohOf(std::uint32_t guid) noexcept {
    const auto b=engine.base;std::uintptr_t cv{},tv{},goh{};std::uint16_t count{},gen{};const std::uint32_t i=guid&0xffffu;
    if(!b||!get(b+off::gohCountVar,cv)||!cv||!get(cv,count)||i>=count||!get(b+off::gohTableVar,tv)||!tv||!get(tv+std::uintptr_t(i)*16u+8u,gen)||gen!=(guid>>16)||
       !get(tv+std::uintptr_t(i)*16u,goh))return 0;
    return goh;}
// A held prop's frag instance (gohProp +0xe0) and its distance (world units) from its reset position (+0xb0); -1 unread.
inline float resetDist(std::uintptr_t goh,std::uintptr_t& inst) noexcept {
    inst=0;float p[3]{},r[3]{};
    if(!get(goh+off::gohInst,inst)||!inst||!get(inst+off::instPos,p)||!get(inst+off::instResetPos,r))return -1.f;
    const float d=v::length(v::sub(V3{p[0],p[1],p[2]},V3{r[0],r[1],r[2]}));return std::isfinite(d)?d:-1.f;}
// The CommonResets of held props (4 kept, any thread): the instance, by whom (0 Apply's immediate path, 1 the effector, 2 other), the
// distance from home before it ran (mm), the clock (0 while a writer fills the slot).
struct ResetSlot {std::atomic<std::uintptr_t> inst{};std::atomic<std::uint32_t> info{};std::atomic<std::uint64_t> ns{};};
inline std::array<ResetSlot,4> resetRing{};inline std::atomic<unsigned> resetNext{};
struct ResetStats {Counter calls{},held{},passed{},passedBroken{},passedOther{},applyN{},commonN{},commonOurs{};std::array<Counter,5> applyBy{},applyByOurs{};
    std::array<Counter,3> commonBy{},commonByOurs{};std::array<Counter,4> deltaCm{};Counter heldFirstNs{},heldLastNs{};
    // the tick's list: adds, drops by reason (resetDropNames), the most entries at once; a tracked prop's flag back (unfreezeTick) with a
    // CommonReset of it within 100 ms or without one
    Counter adds{},maxSize{};std::array<Counter,6> dropBy{};std::array<Counter,2> refrozenBy{};};
enum ResetDrop : std::uint8_t {DropGone=0,DropHome,DropStock,DropFar,DropFull,DropOff};
inline constexpr std::array<const char*,6> resetDropNames{"gone","home","stockReset","far","full","off"};
inline constexpr std::array<const char*,5> applyByNames{"startPropReset","resetProp","resetProp2","wrapper","other"};
inline constexpr std::array<const char*,3> commonByNames{"applyImmediate","effector","other"};
inline ResetStats resetStats{};
// ggoComponentProp::StartPropReset through its PLT. The readiness poll of OnMsg (return address 0x250c734) for a held prop more than 1 cm
// from home (a broken part at home gets the stock reset) answers false without the reset: Apply never runs, StartPropReset's +0x48 count
// is untouched (0x250bdac..0x250bdb8) and OnMsg leaves the ready byte as it was (0x250c734 -> 0x250cc24), the answer the stock immediate
// reset gives (Apply 0x24cb18c..0x24cb194 returns 0), so the gringo's hotspot stays usable. A true answer would send OnMsg to
// 0x250caf0..0x250caf8, which clears the ready byte that HotspotCanUse refuses on (0x203b724). Every other caller (scripts) and prop:
// the original.
// The hold's decision for one call (ra = the caller's return address, taken in the hook's own frame): true = hold.
inline bool resetHeldFor(std::uintptr_t ra,std::uint32_t guid) noexcept {
    auto& R=resetStats;bump(R.calls);
    if(!holdLive.load(std::memory_order_relaxed)||ra!=engine.base+off::readyPollRa){bump(R.passedOther);return false;}
    const auto goh=gohOf(guid);
    if(!heldHas(goh)){bump(R.passed);return false;}
    std::uintptr_t inst{};const float d=resetDist(goh,inst);
    if(!(d>.01f)){bump(R.passedBroken);return false;} // OnMsg's own gate: |+0x50 - +0xb0|^2 > 1e-4 (world units); at home only a broken part asks
    bump(R.held);const auto now=nowNs();std::uint64_t z=0;R.heldFirstNs.compare_exchange_strong(z,now?now:1);R.heldLastNs.store(now,std::memory_order_relaxed);
    return true;}
// The hook's body for one call (ra = its return address): a held prop answers false, the original never runs.
inline bool startResetFor(std::uintptr_t ra,void* gringo,std::uint32_t guid) noexcept {
    bool hold=false;try{hold=resetHeldFor(ra,guid);}catch(...){hold=false;}
    if(hold)return false;
    const auto o=startResetOriginal.load(std::memory_order_acquire);
    return o?o(gringo,guid):false;
}
inline bool startResetHook(void* gringo,std::uint32_t guid) noexcept {
    void* ra=RDRVR_HP_RETURN_ADDRESS();
    return startResetFor(addr(ra),gringo,guid);
}
// GracefulPropReset::CommonReset through its PLT (counting): by caller, a held prop's distance from home before it, the ring.
inline void commonResetHook(std::uint32_t guid) noexcept {
    void* ra=RDRVR_HP_RETURN_ADDRESS();
    try{auto& R=resetStats;bump(R.commonN);const unsigned by=callerIndex(off::commonCallers,2,ra);bump(R.commonBy[by]);
        const auto goh=gohOf(guid);
        if(heldHas(goh)){bump(R.commonOurs);bump(R.commonByOurs[by]);std::uintptr_t inst{};const float d=resetDist(goh,inst);
            const float cm=d*100.f/std::max(holdUpm.load(std::memory_order_relaxed),1e-3f);bump(R.deltaCm[!(cm>=1.f)?0u:cm<3.2f?1u:cm<10.f?2u:3u]);
            auto& slot=resetRing[resetNext.fetch_add(1,std::memory_order_relaxed)&3u];
            slot.ns.store(0,std::memory_order_relaxed);slot.inst.store(inst,std::memory_order_relaxed);
            slot.info.store((std::uint32_t(by)<<24)|std::uint32_t(std::clamp(cm*10.f,0.f,16000000.f)),std::memory_order_relaxed);
            const auto now=nowNs();slot.ns.store(now?now:1,std::memory_order_release);}
    }catch(...){}
    if(const auto o=commonResetOriginal.load(std::memory_order_acquire))o(guid);
}
// GracefulPropReset::Apply through its PLT (counting by caller).
inline bool applyResetHook(std::uint32_t guid,std::uint32_t other,void* delegate) noexcept {
    void* ra=RDRVR_HP_RETURN_ADDRESS();
    try{auto& R=resetStats;bump(R.applyN);const unsigned by=callerIndex(off::applyCallers,4,ra);bump(R.applyBy[by]);if(heldHas(gohOf(guid)))bump(R.applyByOurs[by]);}catch(...){}
    const auto o=applyResetOriginal.load(std::memory_order_acquire);
    return o?o(guid,other,delegate):false;
}

// ---- the behaviour list (main thread, outside the step): AddInstBehavior / RemoveInstBehavior split into the list and the slot ----
inline bool listHas(std::uintptr_t sim,const void* beh) noexcept {
    const auto count=raw<std::uint16_t>(sim+off::simBehCount);const auto list=raw<std::uintptr_t>(sim+off::simBehList);
    if(!list)return false;
    for(unsigned i=0;i<count;++i)if(raw<std::uintptr_t>(list+i*8u)==addr(beh))return true;
    return false;
}
inline bool listAdd(std::uintptr_t sim,const void* beh) noexcept {
    const auto count=raw<std::uint16_t>(sim+off::simBehCount),cap=raw<std::uint16_t>(sim+off::simBehCap);const auto list=raw<std::uintptr_t>(sim+off::simBehList);
    if(!list||count>=cap)return false;
    if(listHas(sim,beh))return true;
    put<std::uintptr_t>(list+count*8u,addr(beh));put<std::uint16_t>(sim+off::simBehCount,std::uint16_t(count+1u));put<std::uint8_t>(sim+off::simBehDirty,1);
    return true;
}
inline void listRemove(std::uintptr_t sim,const void* beh) noexcept {
    const auto count=raw<std::uint16_t>(sim+off::simBehCount);const auto list=raw<std::uintptr_t>(sim+off::simBehList);
    if(!list)return;
    for(unsigned i=0;i<count;++i)if(raw<std::uintptr_t>(list+i*8u)==addr(beh)){
        const unsigned last=count-1u;put<std::uintptr_t>(list+i*8u,raw<std::uintptr_t>(list+last*8u));
        put<std::uint16_t>(sim+off::simBehCount,std::uint16_t(last));put<std::uint8_t>(sim+off::simBehDirty,1);return;}
}
// The slot only for an instance in the level (index != 0xffff).
inline void slotSet(std::uintptr_t sim,std::uintptr_t inst,const void* beh) noexcept {
    const auto index=raw<std::uint16_t>(inst+off::instIndex);if(index==off::noIndex)return;
    const auto slots=raw<std::uintptr_t>(sim+off::simBehSlots);if(!slots)return;
    put<std::uintptr_t>(slots+std::uintptr_t(index)*8u,addr(beh));
    put<std::uint16_t>(inst+off::instFlags,std::uint16_t(raw<std::uint16_t>(inst+off::instFlags)|off::instHasBehaviour));put<std::uint8_t>(sim+off::simBehDirty,1);
}
// index = the level index the slot was set for; the slot is cleared only while it still holds our behaviour.
inline void slotClear(std::uintptr_t sim,std::uintptr_t inst,std::uint16_t index,const void* beh) noexcept {
    const auto slots=raw<std::uintptr_t>(sim+off::simBehSlots);
    if(index!=off::noIndex&&slots&&raw<std::uintptr_t>(slots+std::uintptr_t(index)*8u)==addr(beh))put<std::uintptr_t>(slots+std::uintptr_t(index)*8u,0);
    put<std::uint16_t>(inst+off::instFlags,std::uint16_t(raw<std::uint16_t>(inst+off::instFlags)&~off::instHasBehaviour));put<std::uint8_t>(sim+off::simBehDirty,1);
}
// Our instance at its level index, active (state bits 0), with its collider; collider = 0 when it is not.
inline std::uintptr_t verifiedCollider(std::uintptr_t level,std::uintptr_t inst,std::uint16_t& index) noexcept {
    index=raw<std::uint16_t>(inst+off::instIndex);
    if(!level||index==off::noIndex||index>=raw<std::uint16_t>(level+off::levelCount))return 0;
    const auto objects=raw<std::uintptr_t>(level+off::levelObjects),colliders=raw<std::uintptr_t>(level+off::levelColliders);
    if(!objects||!colliders)return 0;
    const auto word=raw<std::uintptr_t>(objects+std::uintptr_t(index)*off::objStride+off::objInst);
    if((word&~std::uintptr_t(0xf))!=inst||(word&3u)!=0)return 0;
    const auto col=raw<std::uintptr_t>(colliders+std::uintptr_t(index)*8u);
    return col&&raw<std::uintptr_t>(col+off::colliderInst)==inst?col:0;
}
// The level still holds our instance at index (any state).
inline bool inLevel(std::uintptr_t level,std::uintptr_t inst,std::uint16_t index) noexcept {
    if(!level||index==off::noIndex||index>=raw<std::uint16_t>(level+off::levelCount))return false;
    const auto objects=raw<std::uintptr_t>(level+off::levelObjects);if(!objects)return false;
    return (raw<std::uintptr_t>(objects+std::uintptr_t(index)*off::objStride+off::objInst)&~std::uintptr_t(0xf))==inst;
}
// Another instance's level state (0 active, 1 inactive, 2 fixed) at its own index; -1 when the level does not hold it there.
inline int otherState(std::uintptr_t level,std::uintptr_t inst) noexcept {
    std::uint16_t index{},count{};std::uintptr_t objects{},word{};
    if(!level||!get(inst+off::instIndex,index)||index==off::noIndex||!get(level+off::levelCount,count)||index>=count||
       !get(level+off::levelObjects,objects)||!objects||!get(objects+std::uintptr_t(index)*off::objStride+off::objInst,word))return -1;
    return (word&~std::uintptr_t(0xf))==inst?int(word&3u):-1;
}

// ---- behaviour callbacks (main thread, inside the step) ----
inline void behDtor(void*) noexcept {}
inline void behReset(void*) noexcept {}
inline bool behIsActive(const void*) noexcept {return true;}
inline bool behIsAsleep(const void*) noexcept {return false;}
inline bool behActivateWhenHit(const void*) noexcept {return false;}
inline bool behUpdateConstraint(void*,void*,float,void*) noexcept {return true;}
inline bool behIsForceField(const void*) noexcept {return false;}
inline bool behIsCloth(const void*) noexcept {return false;}
// Our instances' PrepareForDeactivation(managed) (own vtable slot +0x100): never put to sleep by the simulator.
inline bool instNoDeactivate(void*,bool) noexcept {bump(stats.deactRefused);return false;}
inline void zeroVelocity(std::uintptr_t col) noexcept {
    alignas(16) const float zero[4]{};engine.setVelocity(reinterpret_cast<void*>(col),zero);engine.setAngVelocity(reinterpret_cast<void*>(col),zero);
}
// Another instance's collider when the level holds it active at the level index stored at the grab (raw reads of the level's own arrays
// only: inside the step the instance pointer itself is never read before the level's slot proves it is still there).
inline std::uintptr_t activeColliderAt(std::uintptr_t level,std::uint16_t index,std::uintptr_t inst) noexcept {
    if(!level||!inst||index==off::noIndex||index>=raw<std::uint16_t>(level+off::levelCount))return 0;
    const auto objects=raw<std::uintptr_t>(level+off::levelObjects),cols=raw<std::uintptr_t>(level+off::levelColliders);if(!objects||!cols)return 0;
    const auto w=raw<std::uintptr_t>(objects+std::uintptr_t(index)*off::objStride+off::objInst);
    if((w&~std::uintptr_t(0xf))!=inst||(w&3u)!=0)return 0;
    const auto col=raw<std::uintptr_t>(cols+std::uintptr_t(index)*8u);
    return col&&raw<std::uintptr_t>(col+off::colliderInst)==inst?col:0;
}
// The mass a hold drives its prop at: the pair's held mass while it is set on this prop's collider (lever grabpairkg), else the prop's own.
inline float driveKgOf(const State& s,const GrabHold& g) noexcept {
    return s.heldMass.set&&s.heldMass.prop==g.prop&&s.heldMass.sim==s.sim&&s.heldMass.kg>0.f?s.heldMass.kg:g.propKg;}
// A heavy hold: a ball-jointed prop over grabLiftKg, or either hand of a pair (both hands on the same prop).
inline bool heavyHold(const State& s,unsigned h) noexcept {
    const auto& g=s.grab[h&1u];if(!g.active||!g.ball)return false;
    const auto& o=s.grab[(h&1u)^1u];
    return g.propKg>s.lev.grabLiftKg||(o.active&&o.prop==g.prop);}
// GRAB (PreUpdate of a held hand): the force on the held prop at the grip, toward the hand's command velocity (grabPropForce), through
// phCollider::ApplyForce on the prop's own collider while the level holds it active at its index. A rigid or constrained collider whose
// force accumulator is still full at our PreUpdate (UpdateImp consumes it in the step: 0x1c653d8..0x1c653f0) three substeps running turns
// the drive off for the hold, unless the other hand holds the same prop and filled it this substep (both hands drive it). An articulated
// collider (types 1 / 3) only with lever grabdriveart: phArticulatedCollider::ApplyForceImp 0x1c63324 first calls the instance's vtable
// +0x98, fragInst::NotifyForce -> NotifyImpulse, which runs CollisionDamage and the post-damage callback with a perpetrator 0 and, for an
// instance with a behaviour (+0x12 bit 8, cloth), phVerletCloth::ApplyImpulse (0x1e9aab8..0x1e9ac54); then only for that slot holding
// fragInst::NotifyForce, no behaviour bit and the held part within the part count read at the grab. Never a corpse.
// The player's mover's ground (mvrMoverInst +0x6c0 / +0x6c2, the checks mvrMoverInst::Update makes before InternalApplyGroundForce):
// index (-1 none), generation, and whether it is this hold's prop (its level index, the level's generation for it). Raw reads inside the
// step (the mover was verified by this tick); GROUND prints only.
struct GroundRead {std::int32_t index{-1},gen{-1};bool prop{};};
inline GroundRead groundOf(const State& s,const GrabHold& g) noexcept {
    GroundRead r{};if(!groupOk[GroupGround]||!s.moverOk||!s.moverInstAddr||!s.level)return r;
    const auto idx=raw<std::uint16_t>(s.moverInstAddr+off::moverGroundIndex);const auto gen=raw<std::uint16_t>(s.moverInstAddr+off::moverGroundGen);
    r.gen=gen;if(idx==off::noIndex)return r;r.index=idx;
    if(idx!=g.levelIndex||idx>=raw<std::uint16_t>(s.level+off::levelCount))return r;
    const auto gens=raw<std::uintptr_t>(s.level+off::levelGens);if(!gens)return r;
    r.prop=raw<std::uint16_t>(gens+std::uintptr_t(idx)*2u)==gen;return r;}
// The stale ring: one accumulator event of a hold's drive (State::staleRing).
// The player's mover's vertical speed (m/s, the tick's mover velocity).
inline float moverUpMps(const State& s,float u) noexcept {
    const float x=v::dot(s.hand[0].moverVel,v::normalized(s.tg.up))/std::max(u,1e-3f);return std::isfinite(x)?x:0.f;}
inline void noteAcc(State& s,const GrabHold& g,unsigned h,const float* acc,float ownN,float diffN,unsigned kind,const GroundRead& gr,float u) noexcept {
    const auto c=[](float x){return std::int32_t(std::isfinite(x)?std::clamp(x,-2e9f,2e9f):0.f);};const float k=1.f/std::max(u,1e-3f);
    const auto now=nowNs();const float tq=v::length(V3{acc[4],acc[5],acc[6]})*k*k;
    s.staleRing[s.staleNext]={c(g.startNs&&now>=g.startNs?float(double(now-g.startNs)*1e-6):-1.f),std::int32_t(h&1u),std::int32_t(s.substep&0x7fffu),c(acc[0]*k),c(acc[1]*k),c(acc[2]*k),
        c(ownN*k),c(diffN*k),std::int32_t(kind),gr.index,gr.gen,std::int32_t(g.levelIndex),gr.prop?1:0,std::int32_t(g.staleRun),c(tq),c(moverUpMps(s,u)*100.f)};
    s.staleNext=(s.staleNext+1u)%unsigned(s.staleRing.size());++s.staleN;}
inline void driveOffNow(GrabHold& g) noexcept {
    g.driveOff=true;bump(stats.grab.driveOff);const auto now=nowNs();g.rec.driveOffMs=g.startNs&&now>=g.startNs?float(double(now-g.startNs)*1e-6):0.f;}
// A ball-jointed hold (g.ball, gn read this substep): grabGripForce at the grip toward the command (vCmd = the command's velocity), with
// the stall scale; its diagnostics in the hold's record. True = a force was applied this substep (or the pair's rigid carry covered it).
// The accumulator at the drive (lever grabstaleown): phCollider::UpdateImp zeroes +0x150 / +0x160 every step (0x1c653ec..0x1c653f0), so
// the drive's own force reads back unchanged (force and torque, the last apply on this prop by either hand: markAcc keeps both hands'
// marks) only while steps skip the prop, however many substeps later: nothing is applied on top of it, and three such substeps running
// take it back (the accumulator less the mark, i.e. ~0, written to +0x150 / +0x160) and turn the drive off for the hold. Any other force
// there is foreign (the player's mover queues its weight on the instance it stands on: mvrMoverInst::Update 0x288f7f0..0x288f86c ->
// DeferredMoverManager::ApplyForce, applied before PreUpdateInstanceBehaviors): counted, the drive goes on; a force over 3 x
// grabForceMaxN or a torque over 3 x grabForceMaxN x 1 m clears the accumulator (nothing launches the prop at the next step) and turns
// the drive off. Lever grabgroundzero: when the mover's ground is the held prop, the queued force (+0x150 force, +0x160 torque) is zeroed
// before the drive; lever grabgroundlift: while it is, the drive's upward force is at most the prop's weight (its share for one hand of
// two). Lever grabstaleown 0: any force left there counts as stale.
// RIGID CARRY (pl set, lever grabcarry, pairStep's plan ok): the first hand of the substep applies the pair's force and torque as two
// forces (coupleForces); the other hand's call applies nothing and its body follows its grip.
inline bool grabDriveStep(State& s,unsigned h,V3 handPos,V3 cmd,float dt,float u,const HandState& hs,V3 vCmd={0,0,0},const GripNow* gn=nullptr,
                          const PairLimits* pl=nullptr) noexcept {
    auto& G=stats.grab;auto& g=s.grab[h&1u];auto& o=s.grab[(h&1u)^1u];
    if(!s.lev.grabDrive||!groupOk[GroupDrive]||!engine.applyForce||g.driveOff||g.cls==classDeadBody||!(g.propKg>0.f))return false;
    const auto col=activeColliderAt(s.level,g.levelIndex,g.prop);
    if(!col||col!=g.propCol){bump(G.driveNoCol);return false;}
    const auto now=nowNs();
    const int type=raw<std::int32_t>(col+off::colliderType);
    // the pair's rigid carry applied this substep (by the other hand, whatever this hand's own grip read): nothing more on the prop
    if(s.lev.grabCarry&&s.pair.ok&&s.pair.step==s.substep&&s.pair.carryOk&&s.pair.carryStep==s.substep&&o.active&&o.prop==g.prop&&(type==0||type==2)){
        bump(G.carryShared);++g.rec.pairSubsteps;++g.rec.carrySubsteps;return true;}
    PairCache* rc=pl&&s.lev.grabCarry&&s.pair.ok&&s.pair.step==s.substep&&s.pair.carryOk&&(type==0||type==2)&&gn&&gn->ok?&s.pair:nullptr;
    const bool pairHeld=o.active&&o.prop==g.prop;
    GroundRead gr{};
    if(type==0||type==2){
        gr=groundOf(s,g);
        // the other hand's force of this substep (its apply within 2 ms, or its mark of this substep)
        const bool shared=pairHeld&&((o.applyNs&&now>=o.applyNs&&now-o.applyNs<2'000'000ull)||(g.accStep&&g.accStep==s.substep));
        alignas(16) float acc[8];std::memcpy(acc,reinterpret_cast<const void*>(col+off::colliderForce),sizeof acc);
        if(shared)bump(G.driveShared);
        else{
            const float uu=std::max(u,1e-3f);
            if(gr.prop){bump(G.groundHeld);++g.rec.groundHeld;
                if(s.lev.grabGroundZero&&(acc[0]!=0.f||acc[1]!=0.f||acc[2]!=0.f||acc[4]!=0.f||acc[5]!=0.f||acc[6]!=0.f)){
                    noteAcc(s,g,h,acc,v::length(g.accAfter),0.f,3,gr,u);
                    alignas(16) const float zero[8]{};std::memcpy(reinterpret_cast<void*>(col+off::colliderForce),zero,sizeof zero);
                    for(auto& x:acc)x=0.f;bump(G.groundZeroed);++g.rec.groundZeroed;}}
            const V3 a3{acc[0],acc[1],acc[2]},t3{acc[4],acc[5],acc[6]};
            if(acc[0]!=0.f||acc[1]!=0.f||acc[2]!=0.f||acc[4]!=0.f||acc[5]!=0.f||acc[6]!=0.f){
                // the drive's own force still unconsumed: the accumulator as the last apply on this prop left it (either hand's mark), at any
                // later substep (steps that skip the prop leave it; the step that runs zeroes it)
                const float own=v::length(g.accAfter),diff=v::length(v::sub(a3,g.accAfter));
                const float ownT=v::length(g.accTauAfter),diffT=v::length(v::sub(t3,g.accTauAfter));
                const bool mine=g.accStep&&g.accStep<s.substep&&diff<=std::max(1e-3f*own,1e-3f*uu)&&diffT<=std::max(1e-3f*ownT,1e-3f*uu*uu);
                if(!s.lev.grabStaleOwn||mine){bump(G.driveStale);++g.rec.stale;++g.staleRun;noteAcc(s,g,h,acc,own,diff,0,gr,u);
                    if(g.staleRun>=3){
                        // the drive ends: its own unconsumed force is taken back (what is left is the read's difference from the mark)
                        if(s.lev.grabStaleOwn&&mine){const V3 rf=v::sub(a3,g.accAfter),rt=v::sub(t3,g.accTauAfter);
                            alignas(16) const float back[8]{rf[0],rf[1],rf[2],0.f,rt[0],rt[1],rt[2],0.f};
                            std::memcpy(reinterpret_cast<void*>(col+off::colliderForce),back,sizeof back);bump(G.staleCleared);}
                        driveOffNow(g);}
                    return false;}
                const float lim=3.f*s.lev.grabForceMaxN*uu;
                if(v::length(a3)>lim||v::length(t3)>lim*uu){bump(G.driveRunaway);noteAcc(s,g,h,acc,own,diff,2,gr,u);
                    alignas(16) const float zero[8]{};std::memcpy(reinterpret_cast<void*>(col+off::colliderForce),zero,sizeof zero);bump(G.runawayCleared);
                    driveOffNow(g);return false;}
                bump(G.driveForeign);++g.rec.foreign;g.staleRun=0;noteAcc(s,g,h,acc,own,diff,1,gr,u);}
            else g.staleRun=0;}
    }else if(type==1||type==3){
        if(!s.lev.grabDriveArt){bump(G.driveArtRefused);return false;}
        const auto vt=raw<std::uintptr_t>(g.prop);
        if(!vt||raw<std::uintptr_t>(vt+off::vtNotifyForce)!=engine.base+off::fnNotifyForce||(raw<std::uint16_t>(g.prop+off::instFlags)&0x100u)||
           g.comp<0||g.comp>=g.artParts){bump(G.driveArtRefused);return false;}
        bump(G.driveArticulated);
    }else{bump(G.driveNoCol);return false;}
    // the mark: the accumulator after this apply, for both hands holding the prop (the next read compares with the last writer's)
    const auto markAcc=[&]{alignas(16) float a[8];std::memcpy(a,reinterpret_cast<const void*>(col+off::colliderForce),sizeof a);
        const V3 f{a[0],a[1],a[2]},t{a[4],a[5],a[6]};g.accAfter=f;g.accTauAfter=t;g.accStep=s.substep;
        if(pairHeld){o.accAfter=f;o.accTauAfter=t;o.accStep=s.substep;}};
    // the upward force's cap while the player's mover stands on the held prop (lever grabgroundlift): the prop's weight, half each for two
    // hands (the carry takes its cap in pairStep)
    const bool liftCap=s.lev.grabGroundLift&&gr.prop;
    if(rc){
        const CarryOut& co=rc->co;const CoupleForces cf=coupleForces(co.fN,co.tauNm,gn->com,.2f,u);
        if(co.capUp){bump(G.groundLiftCapped);++g.rec.groundLift;}
        for(unsigned k=0;k<2;++k){const V3 fw=v::mul(cf.f[k],u);
            alignas(16) const float fv[4]{fw[0],fw[1],fw[2],0.f},pv[4]{cf.pos[k][0],cf.pos[k][1],cf.pos[k][2],0.f};
            engine.applyForce(reinterpret_cast<void*>(col),quad(fv),quad(pv),g.comp);}
        rc->carryStep=s.substep;g.applyNs=now?now:1;markAcc();
        bump(G.driveApplied);if(co.capF){bump(G.carryCapF);++g.rec.capped;}if(co.capTau)bump(G.carryCapTau);
        const float fn=v::length(co.fN),up=v::dot(co.fN,v::normalized(s.tg.up)),em=v::length(co.eM);
        g.forceSum+=double(fn);++g.forceN;auto& r=g.rec;r.forceMaxN=std::max(r.forceMaxN,fn);r.upMaxN=std::max(r.upMaxN,up);
        ++r.pairSubsteps;++r.carrySubsteps;r.driveKg=rc->driveKg;
        if(std::isfinite(em))r.comErrMaxCm=std::max(r.comErrMaxCm,em*100.f);
        {const float th=v::length(co.theta)*180.f/pi;if(std::isfinite(th))r.angErrMaxDeg=std::max(r.angErrMaxDeg,th);}
        {const float rr=std::fabs(v::dot(co.wRel,rc->cs.frame.x));if(std::isfinite(rr))r.rollRateMax=std::max(r.rollRateMax,rr);}
        {const float tm=v::length(co.tauNm);if(std::isfinite(tm))r.tauMaxNm=std::max(r.tauMaxNm,tm);}
        if(r.inertia[0]<0.f){r.inertia={rc->inertia[0],rc->inertia[1],rc->inertia[2]};r.carryKg=rc->massKg;}
        // both hands' last drive for the pair's episode diagnostics: the error and half the lift each
        for(unsigned i=0;i<2;++i){auto& x=s.grab[i];if(x.active&&x.prop==g.prop){x.lastDriveLagM=em;x.lastUpN=.5f*up;x.lastCapBits=std::uint8_t(co.capF?1u:0u);}}
        g.lastUpN=.5f*up;
        return true;}
    V3 at=handPos;GrabForce f{};
    if(g.ball&&gn&&gn->ok&&(type==0||type==2)){
        const V3 toCmd=v::sub(cmd,gn->grip);const float lag=v::length(toCmd)/u;
        const float closing=lag>1e-4f?v::dot(v::sub(gn->vGrip,vCmd),v::mul(toCmd,1.f/(lag*u)))/u:0.f;
        const float scale=gripStallStep(g.stall,lag,closing,dt);g.rec.stallMin=std::min(g.rec.stallMin,scale);
        // the pair's statics carry, the player guard, the drive mass and the feed-forward (pairStep's cache of this substep)
        const PairCache* pcs=pl&&s.pair.ok&&s.pair.step==s.substep?&s.pair:nullptr;
        const float kg=pcs?pcs->driveKg:driveKgOf(s,g),acc=pcs?pcs->accel:s.lev.grabAccel;
        const float slew=pl&&s.lev.grabPairSlew?pairSlewN(kg,s.lev.grabZeta,s.lev.grabWn,acc,dt,s.lev.grabSlewN):s.lev.grabSlewN;
        const V3* carry=pcs&&pcs->statics?&pcs->carry.carry[h&1u]:nullptr;const V3* guard=carry&&pcs->guard?&pcs->guardN:nullptr;float trim=0.f;
        const V3 ff=pcs?pcs->ffN[h&1u]:V3{0,0,0};
        const GripForce gf=pl?grabGripForcePair(cmd,gn->grip,vCmd,gn->vGrip,*pl,s.tg.up,s.lev.grabWn,s.lev.grabZeta,g.prevN,slew,scale,u,carry,guard,&trim,ff):
            grabGripForce(cmd,gn->grip,vCmd,gn->vGrip,kg,s.tg.up,s.lev.grabWn,s.lev.grabZeta,s.lev.grabForceMaxN,s.lev.grabAccel,s.lev.grabLiftKg,
                          s.lev.grabDownG,g.prevN,s.lev.grabSlewN,scale,u);
        {const V3 sp=carry?v::sub(gf.fN,*carry):gf.fN;g.lastLagV=toCmd;g.lastSpringN=v::length(sp);g.lastTrimN=trim;g.lastCarryN=carry?v::length(*carry):0.f;
         g.lastFfN=v::length(ff);g.lastStall=scale;
         g.lastBits=std::uint8_t((gf.capped?1u:0u)|(gf.slewed?2u:0u)|(guard?4u:0u)|(pcs&&pcs->skip?8u:0u)|(kg<g.propKg-1e-3f?16u:0u));
         if(pl&&kg>0.f)g.rec.driveKg=kg;
         if(g.lastFfN>0.f){bump(stats.pairFfSubsteps);maxOf(stats.pairFfMaxN,std::uint64_t(g.lastFfN));}}
        if(pl){++g.rec.pairSubsteps;if(gf.vert==1){bump(G.pairUpCapped);if(pl->upByForce){bump(G.pairUpCapForce);++g.rec.upCapForce;}else{bump(G.pairUpCapShare);++g.rec.upCapShare;}}
            const V3 un=v::normalized(s.tg.up);
            // the side cap: on the spring part under the statics carry (the carry's own couple may exceed it)
            const V3 sp=carry?v::sub(gf.fN,*carry):gf.fN;const float side=v::length(v::sub(sp,v::mul(un,v::dot(sp,un))));
            const bool sideCap=side>=.999f*pl->sideMaxN;if(sideCap){bump(G.pairSideCapped);++g.rec.sideCap;}
            g.lastDriveLagM=lag;g.lastCapBits=std::uint8_t((sideCap?1u:0u)|(gf.vert==1?2u:0u)|(gf.vert==2?4u:0u));
            if(trim>0.f){auto& PS=stats.pairStatics;bump(PS.guardTrims);bump(PS.guardTrimNSum,std::uint64_t(trim));++s.pairEp.guardTrims;}}
        if(gf.slewed&&std::isfinite(gf.slewDeficitN))g.rec.slewDeficitSumN+=gf.slewDeficitN;
        if(v::length(g.prevN)>1.f&&v::dot(gf.fN,g.prevN)<0.f)++g.rec.flips;
        if(gf.slewed)++g.rec.slewed;
        if(gf.vert==1)++g.rec.vertUp;else if(gf.vert==2)++g.rec.vertDown;else ++g.rec.vertLin;
        g.prevN=gf.fN;f.f=gf.f;f.capped=gf.capped;at=gn->grip;
        g.swingSum+=double(v::length(v::sub(gn->vGrip,gn->vCom))/u);++g.swingN;}
    else{const V3 vWant=velocityTo(handPos,cmd,dt,s.lev.maxSpeedMps*u,hs.moverVel).vel;
        // a fixed-rotation hold carries the prop's weight (lever grablightff): the velocity law alone settles g dt / gain under the hand
        f=grabPropForce(vWant,vec3(col+off::colliderVel),g.propKg,dt,s.tg.up,s.lev.grabGain,s.lev.grabForceMaxN,s.lev.grabAccel,s.lev.grabLiftKg,u,
                        g.rec.fixedRot&&s.lev.grabLightFf?g.propKg:0.f);}
    if(liftCap&&v::finite(f.f)){const V3 un=v::normalized(s.tg.up);const float kg=std::max(driveKgOf(s,g),0.f)*(pairHeld?.5f:1.f);
        const float cap=kg*9.81f*std::max(u,1e-3f),fu=v::dot(f.f,un);
        if(fu>cap){f.f=v::sub(f.f,v::mul(un,fu-cap));bump(G.groundLiftCapped);++g.rec.groundLift;}}
    if(!v::finite(f.f)||v::length(f.f)<=0.f)return false;
    alignas(16) const float fv[4]{f.f[0],f.f[1],f.f[2],0.f},pv[4]{at[0],at[1],at[2],0.f};
    engine.applyForce(reinterpret_cast<void*>(col),quad(fv),quad(pv),g.comp);g.applyNs=now?now:1;markAcc();
    bump(G.driveApplied);if(f.capped){bump(G.driveCapped);++g.rec.capped;}
    const float fn=v::length(f.f)/u,up=v::dot(f.f,v::normalized(s.tg.up))/u;
    g.forceSum+=double(fn);++g.forceN;g.rec.forceMaxN=std::max(g.rec.forceMaxN,fn);g.rec.upMaxN=std::max(g.rec.upMaxN,up);g.lastUpN=up;
    return true;
}
// The substep clock: a hand's PreUpdate that this hand already ran since the clock last moved starts a new substep (the list may hold
// either hand first; a lone hand moves it every call).
inline void noteSubstep(State& s,unsigned h) noexcept {
    const auto bit=std::uint8_t(1u<<(h&1u));if(s.preSeen&bit){s.preSeen=0;++s.substep;}s.preSeen=std::uint8_t(s.preSeen|bit);}
// TWO HANDS: both holds active on the same ball-jointed prop (the same collider at the same level index) with lever grab2. Computed once
// per substep (the substep clock) by the first hand to drive (pure math on the grips of the prop's pre-step pose and both hands'
// commands), reused by the other; any other substep the hands drive alone (a hand that lets go ends the pair at once; the slew cap spreads
// the force's change).
inline const PairCache* pairStep(State& s,std::uint64_t t0,float dt,V3 moverNow) noexcept {
    auto& pc=s.pair;
    if(pc.step==s.substep)return pc.ok?&pc:nullptr;
    auto& G=stats.grab;const bool was=pc.ok;pc.step=s.substep;pc.ok=false;
    const auto& a=s.grab[0];const auto& b=s.grab[1];
    std::uintptr_t col{};GripNow ga{},gb{};
    const bool pair=s.lev.grab2&&s.lev.grabDrive&&a.active&&b.active&&a.prop==b.prop&&a.ball&&b.ball&&a.levelIndex==b.levelIndex&&a.propCol==b.propCol&&
        !a.driveOff&&!b.driveOff&&a.propKg>0.f&&s.tg.hand[0].valid&&s.tg.hand[1].valid&&
        (col=activeColliderAt(s.level,a.levelIndex,a.prop))!=0&&col==a.propCol&&(ga=gripNow(col,a.gripLocal)).ok&&(gb=gripNow(col,b.gripLocal)).ok;
    if(!pair){if(was){bump(G.pairEnds);}
        // the episode's own last pair substep and weight (this call may come much later, with the holds already gone)
        if(s.pairEp.on){auto& e=s.pairEp;auto& r=s.pairRing[s.pairNext];r=State::PairRec{};r.set=true;r.ns=e.lastNs;r.ms=e.lastNs>=e.startNs?(e.lastNs-e.startNs)/1'000'000ull:0;
            r.substeps=e.n;r.shareAvg=e.n?float(e.shareSum/double(e.n)):0.f;r.riseMaxCm=e.riseMax*100.f;r.dropMaxCm=e.dropMax*100.f;r.speedMaxCms=e.speedMax*100.f;
            r.upMaxN=e.upMax;r.weightN=e.weightN;
            r.upAvgN=e.n?float(e.upSum/double(e.n)):0.f;r.dCm=e.dCm;r.vertCm=e.vertCm;r.horizCm=e.horizCm;r.shareHAvg=e.n?float(e.shareHSum/double(e.n)):0.f;
            r.coupleAvgN=e.n?float(e.coupleSum/double(e.n)):0.f;r.coupleMaxN=e.coupleMax;r.turnMaxDeg=e.turnMax;r.playerDepthMm=e.playerDepth;r.moverCm=e.moverMax;
            r.lag20Ms=e.lag20Ms;r.lag60Ms=e.lag60Ms;r.sideCap=e.sideCap;r.downCap=e.downCap;r.playerSubsteps=e.playerSubsteps;r.guardTrims=e.guardTrims;
            r.statics=e.statics?1:0;r.start=e.start;
            s.pairNext=(s.pairNext+1u)%unsigned(s.pairRing.size());e=State::PairEp{};}
        return nullptr;}
    const float u=std::max(s.upm,1e-3f);
    const float since=t0>s.tg.ns?float(double(t0-s.tg.ns)*1e-9):0.f;
    std::array<V3,2> cmd{},vc{};
    for(unsigned i=0;i<2;++i){const auto& hs=s.hand[i];cmd[i]=command(s.tg.hand[i],moverNow,hs.moverVel,since,dt);vc[i]=v::add(hs.moverVel,s.tg.hand[i].vel);}
    pc.plan=pairTargets(cmd[0],cmd[1],vc[0],vc[1],ga.grip,gb.grip,u);
    if(!v::finite(pc.plan.target[0])||!v::finite(pc.plan.target[1]))return nullptr;
    pc.share0=pairShare(ga.com,ga.grip,gb.grip);
    const float minShare=s.lev.grabPairFull?.5f:0.f;
    // the drive mass (lever grabpairkg: the held mass the tick set on the collider) and its acceleration cap (grabheldaccel; the prop's own
    // mass keeps grabaccel and the down cap downG M g / 2)
    pc.driveKg=driveKgOf(s,a);const bool light=pc.driveKg<a.propKg-1e-3f;pc.accel=light?s.lev.grabHeldAccel:s.lev.grabAccel;
    pc.lim[0]=pairLimits(pc.driveKg,pc.share0,s.lev.grabLift2Kg,s.lev.grabForceMaxN,pc.accel,s.lev.grabDownG,minShare,light?pc.accel:0.f);
    pc.lim[1]=pairLimits(pc.driveKg,1.f-pc.share0,s.lev.grabLift2Kg,s.lev.grabForceMaxN,pc.accel,s.lev.grabDownG,minShare,light?pc.accel:0.f);
    // the command accelerations (30 ms average) and their feed-forward (lever grabfeedfwd)
    if(!was)pc.acc={};
    for(unsigned i=0;i<2;++i){const V3 ac=cmdAccelStep(pc.acc[i],pc.plan.vTarget[i],dt,.03f,pc.accel,u);pc.ffN[i]=s.lev.grabFeedFwd?pairFeedFwdN(pc.driveKg,ac,u):V3{0,0,0};}
    // the statics carry (lever grabpairstatics) and the player guard (lever grabplayerguard: the player's capsule met in the last 2 substeps;
    // a contact the player skip switched off reads inactive in propScan, so with the skip on it arms only on a contact the skip missed)
    pc.skip=s.skipOurs&&skipHeldSlot(s.skip,a.prop)>=0;
    pc.statics=s.lev.grabPairStatics;pc.carry=PairCarry{};pc.guard=false;
    if(pc.statics){pc.carry=pairCarry(ga.com,ga.grip,gb.grip,s.tg.up,pc.driveKg,s.lev.grabLift2Kg,s.lev.grabForceMaxN,u);
        auto& PS=stats.pairStatics;bump(PS.statics);if(pc.carry.clamped)bump(PS.clamped);if(pc.carry.shortLine)bump(PS.shortLine);
        const float cn=pc.carry.coupleN;bump(PS.coupleHist[cn<50.f?0u:cn<150.f?1u:cn<300.f?2u:cn<450.f?3u:4u]);
        pc.guard=s.lev.grabPlayerGuard&&s.playerContactStep&&s.substep>=s.playerContactStep&&s.substep-s.playerContactStep<=2&&v::length(s.playerContactN)>.5f;
        if(pc.guard)bump(stats.playerSkip.guardFallback);
        pc.guardN=s.playerContactN;}
    pc.ok=true;
    if(!was)bump(G.pairJoins);
    // RIGID CARRY (lever grabcarry): the frame from the tracked hands' line (no extrapolation), the pose kept from the join (taken in the
    // frame of the grips' own line), this substep's force and torque at the drive mass and the collider's inertia (+0xa0, kg x world
    // units^2 at the mass setColliderMass set)
    // A pair resumed within grabCarryKeepMs with the same two holds on the same collider keeps its join pose (the frame resyncs to the
    // hands' line with no spin); any other start takes a new pose.
    pc.carryOk=false;
    if(!was){const bool keep=pc.cs.have&&s.lev.grabCarryKeepMs>0.f&&pc.csCol==col&&pc.csStartA==a.startNs&&pc.csStartB==b.startNs&&t0>=pc.csNs&&
                             double(t0-pc.csNs)<=double(s.lev.grabCarryKeepMs)*1e6;
        if(keep){pc.cs.resync=true;bump(G.carryKept);}else pc.cs=CarryState{};}
    if(s.lev.grabCarry){
        // the inertia and the mass it belongs to (+0xa0..+0xac, kg x world units^2 and kg): the carry drives the collider's own mass
        float in4[4]{};std::memcpy(in4,reinterpret_cast<const void*>(col+off::colliderInertia),sizeof in4);
        const V3 I{in4[0]/(u*u),in4[1]/(u*u),in4[2]/(u*u)};
        const float mc=std::isfinite(in4[3])&&in4[3]>.5f&&in4[3]<5000.f?in4[3]:pc.driveKg;
        if(!v::finite(I)||!(I[0]>0.f)||!(I[1]>0.f)||!(I[2]>0.f)||!(mc>0.f)){bump(G.carryRefused);pc.cs.resync=true;}
        else{
            if(!pc.cs.have&&carryJoin(pc.cs,ga.grip,gb.grip,ga.r,ga.com,s.tg.up,u))bump(G.carryJoins);
            if(pc.cs.have){
                if(std::fabs(mc-pc.driveKg)>.02f*std::max(pc.driveKg,1e-3f))bump(G.carryMassDiff);
                const CarryGains cg{s.lev.grabCarryWn,s.lev.grabCarryZeta,s.lev.grabCarryAccel,s.lev.grabRotWn,s.lev.grabRotZeta,s.lev.grabRotNm,s.lev.grabForceMaxN};
                // the player's mover standing on the prop (lever grabgroundlift): the upward force at most the prop's weight
                const float upMax=s.lev.grabGroundLift&&groundOf(s,a).prop?mc*9.81f:std::numeric_limits<float>::infinity();
                pc.co=carryTick(pc.cs,v::sub(s.tg.hand[1].rel,s.tg.hand[0].rel),v::mul(v::add(cmd[0],cmd[1]),.5f),v::mul(v::add(vc[0],vc[1]),.5f),ga.com,ga.vCom,
                                ga.r,ga.w,I,mc,s.lev.grabLift2Kg,s.tg.up,cg,dt,s.lev.grabLineTauMs*1e-3f,u,upMax);
                pc.inertia=I;pc.massKg=mc;pc.carryOk=pc.co.ok;
                if(pc.carryOk){pc.csCol=col;pc.csStartA=a.startNs;pc.csStartB=b.startNs;pc.csNs=t0;}
                if(pc.carryOk){bump(G.carrySubsteps);
                    // the carry ring: every 2nd carry substep, each with an error over 15 cm or 15 deg
                    const float em=v::length(pc.co.eM),th=v::length(pc.co.theta);++s.carryN;
                    if((s.carryN&1u)==0u||em>.15f||th>.2618f){const auto c16=[](float x){return std::int16_t(std::isfinite(x)?std::clamp(x,-32000.f,32000.f):0.f);};
                        const V3 un=v::normalized(s.tg.up);V3 fw{0,0,1};
                        if(s.moverOk&&s.moverInstAddr){const Rows mr=rows3(s.moverInstAddr+off::instMatrix);const V3 f=v::sub(mr[2],v::mul(un,v::dot(mr[2],un)));if(v::length(f)>.1f)fw=v::normalized(f);}
                        const V3 sd=v::normalized(v::cross(un,fw));const float d10=1800.f/pi;
                        const float ms=s.pairEp.on&&t0>=s.pairEp.startNs?float(double(t0-s.pairEp.startNs)*1e-6):0.f;
                        const GroundRead gr=groundOf(s,a);
                        s.carryRing[s.carryNext]={c16(ms*.1f),c16(v::dot(pc.co.eM,fw)*1000.f),c16(v::dot(pc.co.eM,sd)*1000.f),c16(v::dot(pc.co.eM,un)*1000.f),
                            c16(pc.co.relMps*100.f),c16(v::dot(pc.co.theta,pc.cs.frame.x)*d10),c16(v::dot(pc.co.theta,pc.cs.frame.y)*d10),c16(v::dot(pc.co.theta,pc.cs.frame.z)*d10),
                            c16(v::dot(pc.co.wRel,pc.cs.frame.x)*180.f/pi),c16(v::length(pc.co.fN)),c16(v::length(pc.co.tauNm)*10.f),
                            std::int16_t((pc.co.capF?1:0)|(pc.co.capTau?2:0)|(pc.co.capUp?4:0)),c16(v::length(s.hand[0].moverVel)/u*100.f),std::int16_t(gr.prop?1:0),
                            c16(pc.cs.lineStepRad*d10),c16(moverUpMps(s,u)*100.f)};
                        s.carryNext=(s.carryNext+1u)%unsigned(s.carryRing.size());++s.carryW;}}}}}
    // diagnostics: the spacing, the stretch, the share, the spin about the grip line, both hands' lift of the last substep, the speed
    bump(G.pairSubsteps);
    const float dcm=pc.plan.d/u*100.f,st=std::fabs(pc.plan.stretch)*100.f;
    bump(G.pairDHist[!(dcm<100.f)?4u:dcm<20.f?0u:dcm<40.f?1u:dcm<60.f?2u:3u]);bump(G.pairStretchHist[!(st<60.f)?4u:st<5.f?0u:st<15.f?1u:st<30.f?2u:3u]);
    bump(G.pairShareHist[pc.share0<.3f?0u:pc.share0<.45f?1u:pc.share0<.55f?2u:pc.share0<.7f?3u:4u]);
    {const float roll=v::dot(ga.w,v::normalized(v::sub(gb.grip,ga.grip)));const float r=std::fabs(roll);
     if(std::isfinite(r)){bump(G.pairRollHist[r<.5f?0u:r<1.f?1u:r<2.f?2u:r<4.f?3u:4u]);
        if(r>=.5f){const float sg=roll>0.f?1.f:-1.f;if(pc.rollSign!=0.f&&sg!=pc.rollSign)bump(G.pairRollFlips);pc.rollSign=sg;}}}
    maxOf(G.pairUpMaxN,std::uint64_t(std::max(0.f,a.lastUpN+b.lastUpN)));G.pairWeightN.store(std::uint64_t(pc.driveKg*9.81f),std::memory_order_relaxed);
    maxOf(G.pairSpeedMaxCms,std::uint64_t(std::max(0.f,v::length(ga.vCom)/u*100.f)));
    {auto& e=s.pairEp;const V3 un=v::normalized(s.tg.up);const float y=v::dot(ga.com,un)/u;
     if(!e.on){e=State::PairEp{};e.on=true;e.startNs=t0;e.y0=y;e.r0=ga.r;e.mover0=moverNow;e.statics=pc.statics;
        const V3 d=v::sub(ga.grip,gb.grip);const float dv=v::dot(d,un);e.dCm=v::length(d)/u*100.f;e.vertCm=std::fabs(dv)/u*100.f;
        e.horizCm=v::length(v::sub(d,v::mul(un,dv)))/u*100.f;
        e.start=s.pairStartNext;s.pairStarts[s.pairStartNext]=State::PairStart{};s.pairStarts[s.pairStartNext].ns=t0;
        s.pairStartNext=(s.pairStartNext+1u)%unsigned(s.pairStarts.size());}
     ++e.n;e.lastNs=t0;e.weightN=pc.driveKg*9.81f;e.shareSum+=double(pc.share0);if(std::isfinite(y)){e.riseMax=std::max(e.riseMax,y-e.y0);e.dropMax=std::max(e.dropMax,e.y0-y);}
     const float sp=v::length(ga.vCom)/u;if(std::isfinite(sp))e.speedMax=std::max(e.speedMax,sp);e.upMax=std::max(e.upMax,a.lastUpN+b.lastUpN);
     // the previous substep's drive (lastUpN / lastDriveLagM / lastCapBits are its values)
     e.upSum+=double(a.lastUpN+b.lastUpN);e.shareHSum+=double(pairShareH(ga.com,ga.grip,gb.grip,s.tg.up));e.coupleSum+=double(pc.carry.coupleN);
     e.coupleMax=std::max(e.coupleMax,pc.carry.coupleN);
     const std::uint8_t bits=std::uint8_t(a.lastCapBits|(b.lastCapBits<<1));
     // (the first substep of an episode holds the values of an earlier drive: not counted)
     if(e.n>1){if((a.lastCapBits|b.lastCapBits)&1u)++e.sideCap;if((a.lastCapBits|b.lastCapBits)&4u)++e.downCap;}
     float tr=0.f;for(unsigned i=0;i<3;++i)tr+=v::dot(e.r0[i],ga.r[i]);
     const float turn=std::acos(std::clamp((tr-1.f)*.5f,-1.f,1.f))*180.f/pi;if(std::isfinite(turn))e.turnMax=std::max(e.turnMax,turn);
     const bool player=pc.guard||(s.playerContactStep&&s.substep>=s.playerContactStep&&s.substep-s.playerContactStep<=1);
     if(player){++e.playerSubsteps;e.playerDepth=std::max(e.playerDepth,s.playerContactDepth/u*1000.f);}
     {const V3 dm=v::sub(moverNow,e.mover0);const float mv=v::length(v::sub(dm,v::mul(un,v::dot(dm,un))))/u*100.f;if(std::isfinite(mv))e.moverMax=std::max(e.moverMax,mv);}
     const float lagMax=std::max(a.lastDriveLagM,b.lastDriveLagM),ms=float(double(t0-e.startNs)*1e-6);
     if(e.n>1){if(e.lag20Ms<0.f&&lagMax>=.20f)e.lag20Ms=ms;if(e.lag60Ms<0.f&&lagMax>=.60f)e.lag60Ms=ms;}
     if(e.start<s.pairStarts.size()){auto& ps=s.pairStarts[e.start];
        if(ps.n<State::pairStartN){const auto c16=[](float x){return std::int16_t(std::isfinite(x)?std::clamp(x,-32000.f,32000.f):0.f);};
            const unsigned sideBits=(a.lastCapBits&1u)|((b.lastCapBits&1u)<<1),upBits=((a.lastCapBits>>1)&1u)|(((b.lastCapBits>>1)&1u)<<1),
                downBits=((a.lastCapBits>>2)&1u)|(((b.lastCapBits>>2)&1u)<<1);(void)bits;
            ps.s[ps.n]={c16(a.lastDriveLagM*100.f),c16(b.lastDriveLagM*100.f),c16((y-e.y0)*1000.f),c16(turn),c16(a.lastUpN*.1f),c16(b.lastUpN*.1f),
                c16(pc.carry.coupleN*.1f),std::int16_t(sideBits|(upBits<<2)|(downBits<<4)|(player?64u:0u))};++ps.n;}}
     // the pair drive ring (the previous substep's drive of the hand with the larger lag)
     if(e.n>1&&(e.n%4u==0u||lagMax>=.20f)){const auto c16=[](float x){return std::int16_t(std::isfinite(x)?std::clamp(x,-32000.f,32000.f):0.f);};
        const auto& g=a.lastDriveLagM>=b.lastDriveLagM?a:b;
        V3 fw{0,0,1};if(s.moverOk&&s.moverInstAddr){const Rows mr=rows3(s.moverInstAddr+off::instMatrix);const V3 f=v::sub(mr[2],v::mul(un,v::dot(mr[2],un)));if(v::length(f)>.1f)fw=v::normalized(f);}
        const V3 sd=v::normalized(v::cross(un,fw));
        const float ms10=ms*.1f,mvs=v::length(s.hand[0].moverVel)/u*100.f;
        const float aCmd=std::max(v::length(pc.acc[0].a),v::length(pc.acc[1].a))/u*10.f;
        const int ks=skipHeldSlot(s.skip,a.prop);
        const unsigned sk=ks<0?0u:s.skipStep[unsigned(ks)]+1u==s.substep||s.skipStep[unsigned(ks)]==s.substep?s.skipStepContacts[unsigned(ks)]:0u;
        s.pairDriveRing[s.pairDriveNext]={c16(ms10),c16(a.lastDriveLagM*1000.f),c16(b.lastDriveLagM*1000.f),c16(v::dot(g.lastLagV,fw)/u*1000.f),
            c16(v::dot(g.lastLagV,sd)/u*1000.f),c16(v::dot(g.lastLagV,un)/u*1000.f),c16(g.lastSpringN),c16(g.lastTrimN),c16(g.lastCarryN),c16(aCmd),c16(g.lastFfN),
            c16(g.lastStall*100.f),c16(mvs),c16(s.playerContactDepth/u*1000.f),c16(float(sk)),std::int16_t(g.lastBits)};
        s.pairDriveNext=(s.pairDriveNext+1u)%unsigned(s.pairDriveRing.size());++s.pairDriveN;bump(stats.pairDriveWrites);}}
    return &pc;
}
inline void preUpdate(void* self,float dt) noexcept {
    const auto t0=nowNs();
    try{
        auto& s=state();const int h=handOf(self);if(h<0)return;
        if(threadKey()!=s.mainKey){bump(stats.wrongThread);return;}
        if(h==0)++s.step;
        noteSubstep(s,unsigned(h));
        auto& hs=s.hand[unsigned(h)];hs.haveExpected=false;
        if(s.mode==ModeOn&&hs.present){
            const auto inst=instOf(unsigned(h));std::uint16_t index{};
            const auto col=verifiedCollider(s.level,inst,index);
            const auto& t=s.tg.hand[unsigned(h)];
            if(col&&hs.machine.state==BodyPark){zeroVelocity(col);hs.haveCommand=false;hs.prevMoverNs=0;} // parked: stays where it stopped
            else if(col&&s.tgUsable&&s.moverOk&&t.valid){
                const V3 moverNow=vec3(s.mover+off::moverPos);
                // the mover moved by one substep's dt since the last PreUpdate (a gap on the clock over 100 ms restarts the estimate)
                const bool gap=!hs.prevMoverNs||t0<hs.prevMoverNs||t0-hs.prevMoverNs>100'000'000ull;
                hs.moverVel=moverVelocity(hs.moverVel,hs.prevMoverPos,moverNow,hs.prevDt,gap);
                hs.prevMoverPos=moverNow;hs.prevMoverNs=t0;hs.prevDt=dt;
                const float sinceTarget=t0>s.tg.ns?float(double(t0-s.tg.ns)*1e-9):0.f;
                const V3 craw=command(t,moverNow,hs.moverVel,sinceTarget,dt);
                const Rows want=v::rotate(v::mul(t.angVel,aheadS(sinceTarget,dt)),t.rows);
                const V3 x=vec3(inst+off::instPos);
                const float u=s.upm;
                V3 c=craw;
                auto& gh=s.grab[unsigned(h)];const bool held=gh.active;
                // NPC virtual body: the command clipped out of the creatures' capsules (the raw command stays the hand's command); not for
                // a hand that holds a prop
                if(!held&&s.lev.npcBody&&s.vb.count&&(hs.machine.state==BodyLive||hs.machine.state==BodyTravel)){
                    const auto v0=nowNs();
                    const VirtualHit vh=pushOut(c,x,want,hs.fist,s.vb,t0>s.vb.ns?float(double(t0-s.vb.ns)*1e-9):0.f,u);
                    if(vh.hit&&vh.body>=0){
                        if(!hs.vhit.hit||s.step>hs.vhitStep+2){hs.vhitStart=s.step;hs.vhitApproach=0.f;hs.vhitSwing=0.f;hs.vhitSpeed=0.f;hs.vhitTangent=0.f;hs.vhitFacing=0.f;hs.vhitSide=0.f;hs.vhitArm=0.f;hs.vhitVel={};
                            hs.vhitArmIn=0.f;hs.vhitLocoIn=0.f;hs.vhitTurn=0.f;hs.vhitVelArm={};hs.vhitArmOk=false;}
                        const auto& b=s.vb.b[std::size_t(vh.body)];
                        hs.vhit=vh;hs.vhitStep=s.step;hs.vhitGuid=b.guid;
                        // PASSIVE: the arm's own speed into the body this substep (velArm along -n: no locomotion, no turn) and its drive past
                        // pressDeadMps (the press's arm step, summed over the tick)
                        const float uu=std::max(u,1e-3f);const bool armOk=t.armOk!=0;
                        const float armIn=armOk?-v::dot(t.velArm,vh.n)/uu:0.f;
                        if(!armOk)hs.armUnknownTick=true;
                        if(std::isfinite(armIn)){hs.armInTick=hs.armTick?std::max(hs.armInTick,armIn):armIn;hs.armTick=true;hs.armStepTickM+=armStepM(armIn,s.lev.pressDeadMps,dt);}
                        if(s.step<hs.vhitStart+2){
                            if(std::isfinite(armIn)&&armIn>=hs.vhitArmIn){hs.vhitArmIn=armIn;hs.vhitVelArm=v::mul(t.velArm,1.f/uu);hs.vhitArmOk=armOk;
                                const float lo=-v::dot(hs.moverVel,vh.n)/uu,tu=v::length(v::sub(t.vel,t.velArm))/uu;
                                hs.vhitLocoIn=std::isfinite(lo)?lo:0.f;hs.vhitTurn=armOk&&std::isfinite(tu)?tu:0.f;}
                            const V3 vrel=v::mul(v::sub(v::add(t.vel,hs.moverVel),b.vel),1.f/std::max(u,1e-3f));
                            const float a=-v::dot(vrel,vh.n);
                            if(std::isfinite(a)&&a>=hs.vhitApproach){hs.vhitApproach=a;hs.vhitSpeed=v::length(vrel);hs.vhitVel=vrel;
                                hs.vhitTangent=v::length(v::sub(vrel,v::mul(vh.n,v::dot(vrel,vh.n))));
                                hs.vhitFacing=v::dot(v::normalized(want[2]),v::mul(vh.n,-1.f));
                                hs.vhitSide=v::dot(v::normalized(vrel),v::normalized(b.rowA));}
                            // the arm's own swing into the body and its speed: the controller in the tracking space (lever npcpassive), else
                            // relative to the mover (a smooth turn counts there)
                            const bool pas=s.lev.npcPassive;
                            const float sw=pas?armIn:-v::dot(v::mul(t.vel,1.f/uu),vh.n);
                            if(std::isfinite(sw))hs.vhitSwing=std::max(hs.vhitSwing,sw);
                            const float arm=pas?(armOk?v::length(t.velArm)/uu:0.f):v::length(t.vel)/uu;if(std::isfinite(arm))hs.vhitArm=std::max(hs.vhitArm,arm);}
                        maxOf(stats.npc.vbDeepUm,std::uint64_t(std::max(0.f,vh.depth/std::max(u,1e-3f))*1e6f));
                    }
                    {const auto vd=nowNs()-v0;bump(stats.npc.vbCalls);addNs(stats.npc.vbNsSum,stats.npc.vbNsMax,vd);++s.pend.vbCalls;s.pend.vbNs+=vd;}
                }
                // GRAB: no contact drive; the velocity to the command reached with the hand's force over the held mass. A ball-jointed hold
                // whose grip drive applied this substep: the hand body moves with the grip (gripFollowVel), the grip force alone moves the prop
                SubstepDrive cd{};
                if(held){GripNow gn{};
                    if(gh.ball){const auto pcol=activeColliderAt(s.level,gh.levelIndex,gh.prop);if(pcol&&pcol==gh.propCol)gn=gripNow(pcol,gh.gripLocal);}
                    // two hands on the prop: this hand's pair target and limits (pairStep)
                    const PairCache* pc=gn.ok?pairStep(s,t0,dt,moverNow):nullptr;
                    const unsigned hi=unsigned(h)&1u;
                    const bool applied=pc?grabDriveStep(s,hi,x,pc->plan.target[hi],dt,u,hs,pc->plan.vTarget[hi],&gn,&pc->lim[hi]):
                                          grabDriveStep(s,hi,x,c,dt,u,hs,v::add(hs.moverVel,t.vel),gn.ok?&gn:nullptr);
                    if(gh.ball&&gn.ok&&applied){
                        // a carry's hand: the grip's velocity after the carry's step (lever grabcarrypredict: the joint then has no hand
                        // body to accelerate, the carry sees the prop's own mass and inertia)
                        V3 vg=gn.vGrip;
                        if(pc&&s.lev.grabCarryPredict&&pc->carryOk&&pc->carryStep==s.substep){vg=carryGripVel(gn.vGrip,gn.grip,gn.com,pc->co.aPred,pc->co.alpha,dt,u);
                            bump(stats.grab.carryPredicted);}
                        cd.vel=gripFollowVel(vg,gn.grip,x,dt);++gh.rec.follow;}
                    else{ // a ball-jointed hand without the drive (lever grabfallbacktaums): a first-order approach to its command
                        const bool slow=gh.ball&&s.lev.grabFallbackTauMs>0.f;
                        const V3 vcmd=slow?followTo(x,c,dt,s.lev.grabFallbackTauMs*1e-3f,s.lev.maxSpeedMps*u,hs.moverVel).vel:velocityTo(x,c,dt,s.lev.maxSpeedMps*u,hs.moverVel).vel;
                        if(gh.ball){bump(stats.grab.fallbackSubsteps);++gh.rec.fallback;}
                        cd.vel=grabDrive(vec3(col+off::colliderVel),vcmd,s.lev.grabForceN*dt/std::max(s.lev.grabMassKg,.1f)*u);}}
                else cd=substepDrive(x,c,dt,s.lev.maxSpeedMps*u,hs.moverVel,hs.est.n,hs.doorFollow?255u:(hs.yield.on?0u:hs.est.age),hs.yield.give,
                                     s.lev.creepMps*u,s.lev.pushSpeedMps*u,s.lev.tangentMps*u);
                alignas(16) float vel[4]{cd.vel[0],cd.vel[1],cd.vel[2],0.f};
                engine.setVelocity(reinterpret_cast<void*>(col),vel);
                // GRAB SEAT: the prop being seated moves to the palm centre with the hand (its collider's velocity; no spin written)
                if(!held&&s.seat[unsigned(h)&1u].on){const auto& sp=s.seat[unsigned(h)&1u];
                    const auto pcol=sp.sim==s.sim?activeColliderAt(s.level,sp.levelIndex,sp.prop):0;
                    if(pcol&&pcol==sp.col){const GripNow gn=gripNow(pcol,sp.seatLocal);
                        if(gn.ok){const V3 pv=seatPullVel(gn.grip,x,cd.vel,s.lev.pushSpeedMps*u,seatTauS(s.lev.creepMps));
                            alignas(16) const float pvv[4]{pv[0],pv[1],pv[2],0.f};if(v::finite(pv)){engine.setVelocity(reinterpret_cast<void*>(pcol),pvv);bump(stats.seat.velWrites);}}}}
                if(cd.clipped)bump(stats.hand[unsigned(h)].creepClips);
                if(cd.tangentCapped)bump(stats.hand[unsigned(h)].tangentClips);
                alignas(16) float spin[4]{};
                if(palm(s)){
                    const float cap=hs.est.age<=3?s.lev.contactSpinRadS:s.lev.maxAngRadS;
                    const V3 w=spinFrame(rows3(inst+off::instMatrix),want,dt,cap);spin[0]=w[0];spin[1]=w[1];spin[2]=w[2];
                }else if(s.builtShape==ShapeCapsule){const V3 w=spinTo(vec3(inst+off::instAxisY),t.rows[1],dt,s.lev.maxAngRadS);spin[0]=w[0];spin[1]=w[1];spin[2]=w[2];}
                if(held){ // the spin's change capped by the hand's torque over the held inertia
                    const float iMax=std::max(s.lev.grabMassKg,.1f)*std::max({palmInertiaPerKg[0],palmInertiaPerKg[1],palmInertiaPerKg[2]})*s.lev.inertiaK;
                    const V3 w=grabDrive(gh.lastSpin,V3{spin[0],spin[1],spin[2]},s.lev.grabTorqueNm*dt/std::max(iMax,1e-4f));
                    spin[0]=w[0];spin[1]=w[1];spin[2]=w[2];gh.lastSpin=w;}
                engine.setAngVelocity(reinterpret_cast<void*>(col),spin);
                hs.command=craw;hs.cmdRows=want;hs.haveCommand=true;if(cd.clamped)bump(stats.hand[unsigned(h)].velClampFrames);
                hs.vset=cd.vel;hs.cmdSpeed=v::length(v::sub(cd.vel,hs.moverVel))/std::max(u,1e-3f);
                hs.expected=v::add(x,v::mul(cd.vel,dt));hs.haveExpected=v::finite(hs.expected);
            }else if(col){
                // not steered this substep (targets older than staleMs, no mover, no target): the body stops instead of keeping its last velocity
                zeroVelocity(col);hs.haveCommand=false;hs.prevMoverNs=0;
            }
        }
        s.preOpen=true;
    }catch(...){bump(stats.exceptions);}
    const auto t1=nowNs();
    auto& s=state();s.preNs=t1;bump(stats.preCalls);addNs(stats.preNsSum,stats.preNsMax,t1-t0);
    if(threadKey()==s.mainKey){++s.pend.preCalls;s.pend.preNs+=t1-t0;}
}
inline void update(void* self,float) noexcept {
    const auto t0=nowNs();
    try{
        auto& s=state();const int h=handOf(self);if(h<0)return;
        if(threadKey()!=s.mainKey){bump(stats.wrongThread);return;}
        if(s.preOpen){s.preOpen=false;bump(stats.substeps);
            if(s.preNs&&t0>s.preNs){bump(stats.spans);addNs(stats.spanNsSum,stats.spanNsMax,t0-s.preNs);++s.pend.spans;s.pend.spanNs+=t0-s.preNs;}}
        auto& hs=s.hand[unsigned(h)];auto& o=s.out.hand[unsigned(h)];auto& st=stats.hand[unsigned(h)];
        o.state=std::uint8_t(hs.present?hs.machine.state:BodyAbsent);
        if(s.mode==ModeOn&&hs.present&&hs.haveCommand&&hs.haveExpected){
            const auto inst=instOf(unsigned(h));
            hs.solvedPos=vec3(inst+off::instPos);hs.solvedRows=rows3(inst+off::instMatrix);hs.haveSolved=true;hs.solvedStep=s.step;
            std::uint16_t index{};const auto col=verifiedCollider(s.level,inst,index);
            const float u=std::max(s.upm,1e-3f);
            const V3 p=v::sub(hs.solvedPos,hs.expected);
            const V3 dv=col?v::sub(vec3(col+off::colliderVel),hs.vset):V3{0,0,0};
            if(col&&v::length(v::sub(vec3(col+off::colliderPos),hs.solvedPos))>.001f*u)bump(st.comMismatch);
            const ContactNote cn=noteSubstep(hs.est,p,dv,hs.vset,.0005f*u,.05f*u);
            hs.est.dyn=hs.dynStep==s.step;
            {const float adv=std::max(0.f,-v::dot(hs.vset,hs.est.n))*hs.prevDt;const float g0=hs.yield.give;
             yieldStep(hs.yield,hs.est.dyn,cn.kind!=ContactNone||(hs.est.age<255&&v::dot(p,hs.est.n)>yieldTouchM*u),v::dot(hs.est.n,hs.solvedPos),adv,.02f*u);
             if(hs.yield.give>g0)bump(st.giveRises);else if(g0>0.f&&hs.yield.give==0.f)bump(st.giveDrops);}
            if(s.grab[unsigned(h)].active){ // a held prop: no contact edges or pulses; the lag for hook A and the leash
                auto& gh=s.grab[unsigned(h)];
                const float lag=v::length(v::sub(hs.solvedPos,hs.command))/u;gh.lagM=lag;o.grab=1;o.lagM=lag;
                maxOf(stats.grab.maxLagUm,std::uint64_t(std::max(0.f,lag)*1e6f));hs.edge=false;
                {auto& r=gh.rec;if(std::isfinite(lag)){++r.lagHist[lagBucket(lag)];r.lagMaxCm=std::max(r.lagMaxCm,lag*100.f);}}
                // a ball-jointed hold: the grip after the step (hook A draws the hand there), the hand body's gap to it, the grip's reversals
                o.ball=gh.ball?1:0;o.gripOk=0;o.heavy=s.lev.grabHeavyDraw&&heavyHold(s,unsigned(h))?1:0;
                if(gh.ball){const auto pcol=activeColliderAt(s.level,gh.levelIndex,gh.prop);
                    const GripNow gn=pcol&&pcol==gh.propCol?gripNow(pcol,gh.gripLocal):GripNow{};
                    if(gn.ok){o.grip=gn.grip;o.gripOk=1;
                        const float mm=v::length(v::sub(hs.solvedPos,gn.grip))/u*1000.f;++gh.rec.sepHist[!(mm<30.f)?4u:mm<2.f?0u:mm<5.f?1u:mm<10.f?2u:3u];
                        if(gh.haveGrip){const V3 st=v::sub(gn.grip,gh.gripLast);
                            if(gh.haveStep&&v::length(st)/u>.003f&&v::length(gh.gripStep)/u>.003f&&v::dot(st,gh.gripStep)<0.f)++gh.rec.gripAlt;
                            gh.gripStep=st;gh.haveStep=true;}
                        gh.gripLast=gn.grip;gh.haveGrip=true;}}
            }else if(cn.kind!=ContactNone){
                bump(st.contactSubsteps);bump(cn.kind==ContactPush?st.pushContacts:st.dvContacts);if(cn.flip)bump(st.normalFlips);
                if(hs.est.dyn)bump(st.contactDynSubsteps);
                hs.contactStep=s.step;hs.contactNs=t0;
                if(cn.prevAge>6){hs.edge=true;hs.edgeVirtual=false;hs.approach=-v::dot(hs.vset,hs.est.n)/u;hs.edgeStep=s.step;}
            }else if(hs.vhit.hit&&hs.vhitStep==s.step&&v::length(hs.vhit.n)>.5f){
                // the command was clipped out of a virtual body this substep: a contact for the drive, the visual stop and the pulse
                hs.est.n=hs.vhit.n;hs.est.age=0;hs.contactStep=s.step;hs.contactNs=t0;bump(st.virtualSubsteps);
                if(cn.prevAge>6){hs.edge=true;hs.edgeVirtual=true;hs.approach=hs.vhitApproach;hs.edgeStep=s.step;}
            }
            o.solved=hs.solvedPos;o.command=hs.command;o.rows=hs.solvedRows;o.cmdRows=hs.cmdRows;o.n=hs.est.n;o.plane=v::dot(hs.est.n,hs.solvedPos);
            o.age=std::uint8_t(std::min(hs.est.age,255u));o.dyn=hs.est.dyn?1:0;o.contactNs=hs.contactNs;
            if(!s.grab[unsigned(h)].active){o.grab=0;o.lagM=0.f;o.ball=0;o.gripOk=0;o.heavy=0;}
        }else{if(hs.est.age<255)++hs.est.age;o.age=255;o.grab=0;o.ball=0;o.gripOk=0;o.heavy=0;}
        s.out.serial=s.step;s.out.ns=t0;solved.store(s.out);
    }catch(...){bump(stats.exceptions);}
    {const auto d=nowNs()-t0;bump(stats.updCalls);addNs(stats.updNsSum,stats.updNsMax,d);auto& s=state();if(threadKey()==s.mainKey){++s.pend.updCalls;s.pend.updNs+=d;}}
}
// otherState with raw reads (inside the step: the other instance is in a pair of this substep).
inline int otherStateRaw(std::uintptr_t level,std::uintptr_t inst) noexcept {
    if(!level)return -1;
    const auto index=raw<std::uint16_t>(inst+off::instIndex);if(index==off::noIndex||index>=raw<std::uint16_t>(level+off::levelCount))return -1;
    const auto objects=raw<std::uintptr_t>(level+off::levelObjects);if(!objects)return -1;
    const auto w=raw<std::uintptr_t>(objects+std::uintptr_t(index)*off::objStride+off::objInst);
    return (w&~std::uintptr_t(0xf))==inst?int(w&3u):-1;
}
// A kept pair with a nav prop (CollideObjects, before the engine's activation pass of this substep): a lock that comes from the frag
// type alone is lifted. A final verdict is cached (8 instances); a prop that is not inactive now is judged again later.
inline void unlockPair(State& s,std::uintptr_t o) noexcept {
    auto& P=stats.props;
    if(!s.lev.props||!s.lev.unlock||!groupOk[GroupAct]||!engine.isActDisabled||!engine.setDisableAct)return;
    const auto table=raw<std::uintptr_t>(o);if(!table||raw<std::uintptr_t>(table+0x10)!=engine.navClassFn)return;
    for(const auto x:s.unlockSeen)if(x==o){bump(P.skipCached);return;}
    const auto f80=raw<std::uint32_t>(o+off::fragF80);const auto f12=raw<std::uint16_t>(o+off::instFlags);
    const auto inf=raw<std::uint8_t>(o+off::fragInfinite);const auto door=raw<std::uint8_t>(o+off::navDoor);const int st=otherStateRaw(s.level,o);
    const auto seen=[&]{s.unlockSeen[s.unlockNext]=o;s.unlockNext=(s.unlockNext+1u)%unsigned(s.unlockSeen.size());};
    if(door==1){bump(P.skipDoor);seen();return;}
    if(f80&0x100u){bump(P.skipOverride);seen();return;}
    if(f12&0x8u){bump(P.skipNever);seen();return;}
    if(inf&0x80u){bump(P.skipInfinite);seen();return;}
    if(st!=1){bump(st==0?P.skipActive:st==2?P.skipFixed:P.skipNotInLevel);if(st==2)seen();return;}
    if(!unlockWanted(f80,f12,inf,st,door))return;
    seen();
    if(!engine.isActDisabled(reinterpret_cast<const void*>(o))){bump(P.skipNotLocked);return;}
    engine.setDisableAct(reinterpret_cast<void*>(o),false);bump(P.unlocks);
}
// GRAB candidates: a class of the grabclass lever (not a door). Also noted for a pair the light ghost drops (the grab still latches).
inline void noteGrabCand(State& s,unsigned h,std::uintptr_t other,int cls) noexcept {
    auto& hs=s.hand[h];
    if(cls>=0&&cls<16&&(s.lev.grabClass>>unsigned(cls)&1u)&&!(cls==classNav&&raw<std::uint8_t>(other+off::navDoor)==1)){
        bool known=false;for(auto& c:hs.grabCands)if(c.inst==other){c.step=s.step;c.cls=cls;known=true;break;}
        if(!known){hs.grabCands[hs.nextGrab]={other,s.step,cls};hs.nextGrab=(hs.nextGrab+1u)%unsigned(hs.grabCands.size());}}
}
// ghost = a pair the light ghost dropped: counted and a grab candidate, but no contact wake / door / dynamic-contact bookkeeping.
inline void notePair(State& s,unsigned h,std::uintptr_t other,int cls,bool ghost=false) noexcept {
    auto& st=stats.hand[h];auto& hs=s.hand[h];bump(st.pairs);
    bump(stats.otherClass[unsigned(cls)&15u]);
    if(ghost&&(cls==classNav||cls==classDeadBody))bump(cls==classNav?st.propPairs:st.corpsePairs);
    else if(cls==classNav||cls==classDeadBody){
        // values only: the door byte of a nav prop, the other's level state (an active prop is pushed, not crept on)
        const bool door=cls==classNav&&raw<std::uint8_t>(other+off::navDoor)==1;
        if(door){bump(st.doorPairs);hs.door={other,s.step};}
        else{bump(cls==classNav?st.propPairs:st.corpsePairs);
            bool known=false;for(auto& c:hs.props)if(c.inst==other){c.step=s.step;known=true;break;}
            if(!known){hs.props[hs.nextProp]={other,s.step,hs.vset};hs.nextProp=(hs.nextProp+1u)%unsigned(hs.props.size());}}
        const std::uint16_t idx=raw<std::uint16_t>(other+off::instIndex);
        if(!door&&s.level&&idx!=off::noIndex&&idx<raw<std::uint16_t>(s.level+off::levelCount)){
            const auto objects=raw<std::uintptr_t>(s.level+off::levelObjects);
            if(objects){const auto w=raw<std::uintptr_t>(objects+std::uintptr_t(idx)*off::objStride+off::objInst);
                if((w&~std::uintptr_t(0xf))==other&&(w&3u)==0)hs.dynStep=s.step;}}
    }
    else if(cls==classMover)bump(st.moverHits);
    else bump(st.worldPairs);
    noteGrabCand(s,h,other,cls);
    const auto arch=raw<std::uintptr_t>(other+off::instArch);
    const std::uint32_t type=arch?raw<std::uint32_t>(arch+off::archType):0u,include=arch?raw<std::uint32_t>(arch+off::archInclude):0u;
    if(cls>=0&&cls<16){hs.classStep[unsigned(cls)]=s.step;
        bool seen=false;for(auto& r:hs.recent)if(r.cls==cls&&r.archType==type){r.step=s.step;seen=true;break;}
        if(!seen){hs.recent[hs.nextRecent]={cls,type,s.step};hs.nextRecent=(hs.nextRecent+1u)%unsigned(hs.recent.size());}}
    for(auto& p:s.samples){
        if(p.count&&p.cls==cls&&p.type==type&&p.include==include){++p.count;return;}
        if(!p.count){p.cls=cls;p.type=type;p.include=include;p.count=1;
            if(cls==0){const int st=otherStateRaw(s.level,other);if(st>=0&&st<3)++p.states[std::size_t(st)];}
            return;}
    }
}
inline bool collideObjects(void* self,void* mine,void*,void* other,void*,void*) noexcept {
    const auto t0=nowNs();bool ok=false;
    try{
        auto& s=state();const int h=handOf(self);
        if(h>=0&&threadKey()==s.mainKey&&other&&mine==objects[unsigned(h)].inst){
            const auto o=addr(other);PairIn p{};
            p.ghost=s.mode!=ModeOn||s.hand[unsigned(h)].ghost||!s.hand[unsigned(h)].present;
            p.player=s.moverOk&&o==s.moverInstAddr;p.otherHand=o==instOf(unsigned(h)^1u);p.npc=s.lev.npc;
            if(!p.ghost&&!p.player&&!p.otherHand){
                const auto user=raw<std::uintptr_t>(o+off::instUser);
                p.owned=user&&s.actor&&raw<std::uint8_t>(user+off::userKind)==0&&raw<std::uintptr_t>(user+off::userActor)==s.actor;
                for(int i=0;i<s.ownCount&&!p.owned;++i)p.owned=s.own[std::size_t(i)]==o;
                using ClassFn=int(*)(const void*);
                const auto table=raw<std::uintptr_t>(o);
                p.otherClass=table?reinterpret_cast<ClassFn>(raw<std::uintptr_t>(table+0x10))(other):-1;
            }
            ok=collide(p)&&!(s.grab[unsigned(h)].active&&o==s.grab[unsigned(h)].prop); // the held prop rides on the constraint, not on contacts
            // KNOCKDOWN HANDS-OFF: a knocked-down creature's frag (by instance or its user's actor) within its window: dropped (drop off:
            // kept and counted); the hand's speed toward the pusher while paired
            if(ok&&p.otherClass==5&&knockOffLive.load(std::memory_order_relaxed)){const auto at=s.lastTickNs?s.lastTickNs:nowNs();
                const auto user=raw<std::uintptr_t>(o+off::instUser);
                const auto ua=user&&raw<std::uint8_t>(user+off::userKind)==0?raw<std::uintptr_t>(user+off::userActor):std::uintptr_t{};
                const int k=knockOffMatch(o,ua,at);
                if(k>=0){auto& ko=knockOff[unsigned(k)];const auto armed=ko.armedNs.load(std::memory_order_relaxed);
                    (at>=armed&&at-armed<knockOffEarlyNs?ko.pairsEarly:ko.pairsLate).fetch_add(1,std::memory_order_relaxed);
                    const V3 dir{ko.dir[0].load(std::memory_order_relaxed),ko.dir[1].load(std::memory_order_relaxed),ko.dir[2].load(std::memory_order_relaxed)};
                    const float tw=knockOffToward(v::mul(s.hand[unsigned(h)].vset,1.f/std::max(s.upm,1e-3f)),dir);
                    const auto mms=std::uint32_t(std::min(tw*1000.f,1e6f));auto m=ko.towardMmsMax.load(std::memory_order_relaxed);
                    while(mms>m&&!ko.towardMmsMax.compare_exchange_weak(m,mms,std::memory_order_relaxed)){}
                    if(ko.drop.load(std::memory_order_relaxed)){ok=false;bump(stats.knockOffDrops);}else bump(stats.knockOffKept);}}
            // THROW: the prop just thrown leaves the hand without a contact of the opening hand
            if(ok&&s.throwDropProp[unsigned(h)]==o&&s.lastTickNs<s.throwDropUntil[unsigned(h)]){ok=false;bump(stats.throws.pairDrops);}
            // LIGHT GHOST: a closing hand passes through the light prop it closes on (no shove before the grab latches); the grab candidate
            // is still noted. Raw reads of the level's own arrays only (inside the step).
            if(ok&&s.lev.grabGhost&&p.otherClass==classNav&&!p.owned){
                auto& slots=s.ghosts[unsigned(h)];bool known=false;for(const auto& g:slots)known=known||g.inst==o;
                bool start=false;
                if(!known){const auto& t=s.tg.hand[unsigned(h)];
                    const auto& hs=s.hand[unsigned(h)];
                    if(lightGhostIntent(t.squeeze,hs.fist,true)){
                        const bool door=raw<std::uint8_t>(o+off::navDoor)==1;
                        const auto col=door?0:activeColliderAt(s.level,raw<std::uint16_t>(o+off::instIndex),o);
                        const float kg=col?raw<float>(col+off::colliderMass):-1.f;
                        const bool closing=lightGhostClosing(t.squeeze,hs.sqLag,hs.fist,true);
                        start=lightGhostStart(closing,true,door,kg,s.lev.grabRotKg);
                        if(!closing&&lightGhostStart(true,true,door,kg,s.lev.grabRotKg))bump(stats.ghost.restingPairs);}}
                bool started=false;const auto at=s.lastTickNs?s.lastTickNs:nowNs();
                if(lightGhostFilter(slots,o,start,at,&started)){ok=false;bump(stats.ghost.drops);unlockPair(s,o);notePair(s,unsigned(h),o,p.otherClass,true);
                    if(started){bump(stats.ghost.starts[GhostIntent]);s.ghostLevel=s.level;}}}
            if(ok){bump(stats.collideTrue);if(p.otherClass==classNav)unlockPair(s,o);notePair(s,unsigned(h),o,p.otherClass);}
            else bump(stats.hand[unsigned(h)].pairsDropped);
        }else if(h>=0&&threadKey()!=s.mainKey)bump(stats.wrongThread);
    }catch(...){bump(stats.exceptions);ok=false;}
    {const auto d=nowNs()-t0;bump(stats.filterCalls);stats.filterNsSum.fetch_add(d,std::memory_order_relaxed);
     auto& s=state();if(threadKey()==s.mainKey){++s.pend.filterCalls;s.pend.filterNs+=d;}}
    return ok;
}
// The simulator's PreApplyImpacts fn (sim+0xe8), called by the functor thunk 0x1c3111c inside the step with x0 = a copy of the
// 0x2c-byte iterator (passed by reference). Our instances: nothing (the stock phInst +0xd8 is a ret, the audio dispatch is skipped);
// every other iterator goes to PreApplyImpacts_Game with the same pointer. Reads constants only; safe on any thread.
// GLASS (inside the step, main thread, a live hand of ON with the lever on): the hand iterator's manifolds walked as
// fragInst::PreApplyImpacts walks them (at most 8 manifolds, 16 sub manifolds, 64 contacts each); per frag component (a window's frame and
// its panes are components of one frag) the deepest active contact whose other instance is a sagFragInst or a fragInstNav (their
// GetClassType words, no call), at most glassCands components, kept for the tick (it reads which of them is glass): the pane, its
// component and element, the pane's side of the contact, A's normal turned toward the hand, the approach of the controller (not the
// body) along it, the edge per component (GlassTracks). Raw reads of the structures the engine is walking in this same call.
inline bool glassOn(const State& s) noexcept {return s.lev.glass&&groupOk[GroupGlass]&&engine.collisionDamage&&s.mode==ModeOn;}
inline void glassScan(unsigned h,std::uintptr_t it) noexcept {
    auto& s=state();if(threadKey()!=s.mainKey||!glassOn(s))return;
    auto& hs=s.hand[h&1u];if(!hs.present||hs.machine.state!=BodyLive||hs.ghost)return;
    const auto t0=nowNs();
    const auto arrA=raw<std::uintptr_t>(it+off::itArrA),arrB=raw<std::uintptr_t>(it+off::itArrB);
    const auto cA=raw<std::int32_t>(it+off::itCountA),cB=raw<std::int32_t>(it+off::itCountB);
    if(cA<0||cB<0||cA>4096||cB>4096)return;
    const auto me=instOf(h);const int total=std::min(cA+cB,8);
    std::array<GlassHit,glassCands> found{};unsigned nf=0;
    for(int i=0;i<total;++i){
        const auto m=i<cA?(arrA?raw<std::uintptr_t>(arrA+std::uintptr_t(i)*8u):0):(arrB?raw<std::uintptr_t>(arrB+std::uintptr_t(i-cA)*8u):0);if(!m)continue;
        const unsigned subs=std::min<unsigned>(std::max<unsigned>(raw<std::uint8_t>(m+off::mfSubCount),1u),16u);
        for(unsigned k=0;k<subs;++k){
            const auto sub=k?raw<std::uintptr_t>(m+off::mfSubs+std::uintptr_t(k)*8u):m;if(!sub)continue;
            const unsigned nc=std::min<unsigned>(raw<std::uint8_t>(sub+off::mfContactCount),64u);
            for(unsigned j=0;j<nc;++j){const auto c=sub+off::mfContacts+std::uintptr_t(j)*off::ctStride;
                if(!raw<std::uint8_t>(c+off::ctActive))continue;
                const auto own=raw<std::uintptr_t>(c+off::ctOwner);if(!own)continue;
                const auto A=raw<std::uintptr_t>(own+off::mfInstA),B=raw<std::uintptr_t>(own+off::mfInstB);
                const auto other=A==me?B:B==me?A:0;if(!other)continue;
                const auto vt=raw<std::uintptr_t>(other);if(!vt)continue;const auto cf=raw<std::uintptr_t>(vt+0x10);
                const int cls=cf==engine.fragClassFn?5:cf==engine.navClassFn?classNav:-1;if(cls<0)continue;
                const float depth=raw<float>(c+off::ctDepth);if(!std::isfinite(depth))continue;
                const bool paneA=other==A;const int comp=raw<std::uint8_t>(own+(paneA?off::mfCompA:off::mfCompB));
                GlassHit* e=nullptr;for(unsigned q=0;q<nf;++q)if(found[q].inst==other&&found[q].comp==comp){e=&found[q];break;}
                if(!e){if(nf>=found.size())continue;e=&found[nf++];*e=GlassHit{};e->depth=-1e30f;}
                if(depth<=e->depth)continue;
                e->inst=other;e->cls=cls;e->depth=depth;e->pos=vec3(c+(paneA?off::ctPosA:off::ctPosB));
                const V3 n=vec3(c+off::ctNormal);e->n=paneA?n:v::mul(n,-1.f);
                e->comp=comp;e->element=raw<std::uint16_t>(c+(paneA?off::ctElemA:off::ctElemB));}}}
    {const auto d=nowNs()-t0;maxOf(stats.glass.scanNsMax,d);}
    auto& G=stats.glass;const float u=std::max(s.upm,1e-3f);const auto& t=s.tg.hand[h&1u];
    for(unsigned q=0;q<nf;++q){GlassHit best=found[q];
        if(!best.inst||!v::finite(best.pos)||!v::finite(best.n))continue;
        bump(G.contacts);bump(G.byClass[best.cls==5?0u:1u]);
        // the normal turned toward the hand (the face it hit; the Collision's normal)
        if(hs.haveSolved&&v::dot(best.n,v::sub(hs.solvedPos,best.pos))<0.f)best.n=v::mul(best.n,-1.f);
        const float approach=t.valid?-v::dot(v::add(t.vel,hs.moverVel),best.n)/u:0.f;
        best.approach=std::isfinite(approach)?approach:0.f;best.ns=s.preNs;best.hand=hs.solvedPos;best.vel=t.valid?v::add(t.vel,hs.moverVel):hs.moverVel;
        best.edge=hs.glassTracks.edge(best.inst,best.comp,s.step,s.preNs,best.approach,s.lev.glassMps);
        maxOf(G.depthMaxUm,std::uint64_t(std::max(0.f,best.depth/u)*1e6f));
        if(best.edge){bump(G.edges);bump(G.approachHist[glassBin(best.approach)]);maxOf(G.approachMaxMms,std::uint64_t(std::max(0.f,best.approach)*1000.f));
            bool seen=false;for(const auto p:s.glassPanes)seen=seen||p==best.inst;
            if(!seen){s.glassPanes[s.glassPaneNext]=best.inst;s.glassPaneNext=(s.glassPaneNext+1u)%unsigned(s.glassPanes.size());bump(G.panes);}}
        else{if(best.approach>=s.lev.glassMps)bump(G.refused[GlassNotEdge]);continue;} // a fast hand on a component it already touched
        // the request of a component keeps its edge and the fastest approach of the edge substeps
        GlassHit* slot=nullptr;for(auto& g:hs.glass)if(g.pending&&g.inst==best.inst&&g.comp==best.comp){slot=&g;break;}
        if(slot){if(best.approach>slot->approach){best.pending=true;*slot=best;}continue;}
        for(auto& g:hs.glass)if(!g.pending){slot=&g;break;}
        if(!slot){slot=&hs.glass[0];for(auto& g:hs.glass)if(g.approach<slot->approach)slot=&g;if(slot->approach>=best.approach)continue;}
        best.pending=true;*slot=best;}
}
// PROPHIT (inside the step, main thread): the held prop's impact iterator (propScanInst, set by the tick while armed) walked as glassScan
// walks the hand's: per mover capsule (the other instance's GetClassType word = mvrMoverInst's, no call) the active contact whose point
// moves fastest into the capsule over the tick's substeps (the first substep of a hit, before the solver slows the prop; a deeper later
// one reads the prop already stopped), its normal turned from the prop toward the capsule's centre, the prop's velocity at the point
// (collider +0x100 centre of mass, +0x110 /
// +0x120 velocities: the collider the level holds active at the hold's index). Raw reads of the structures the engine is walking in this
// same call; nothing is written to the engine.
inline std::array<std::uintptr_t,4> propScanInst{}; // [0..1] the held props, [2..3] THROW: the props in flight
// One contact of a capsule into its entry (mover 0 = empty): the deepest depth kept, the point, normal and velocity of the contact whose
// point moves fastest along its normal (vn).
inline void mergePropContact(PropContact& e,std::uintptr_t mover,unsigned hand,V3 pos,V3 n,V3 vp,float depth) noexcept {
    if(!v::finite(pos)||!v::finite(n)||!v::finite(vp)||!std::isfinite(depth)||v::length(n)<1e-6f)return;
    const V3 nn=v::normalized(n);const float vn=v::dot(vp,nn);
    if(!e.mover){e=PropContact{};e.depth=depth;}else e.depth=std::max(e.depth,depth);
    if(e.mover&&vn<=e.vn)return;
    e.mover=mover;e.hand=hand;e.pos=pos;e.n=nn;e.vPoint=vp;e.vn=vn;}
inline void propScan(std::uintptr_t prop,std::uintptr_t it) noexcept {
    auto& s=state();if(threadKey()!=s.mainKey||s.mode!=ModeOn)return;
    unsigned h=0;for(;h<2;++h)if(s.grab[h].active&&s.grab[h].prop==prop)break;
    int fl=-1; // THROW: a prop in flight (its own slot and collider, its own contacts, no player guard)
    if(h>=2){for(unsigned i=0;i<2;++i)if(s.flights[i].on&&s.flights[i].prop==prop){fl=int(i);break;}if(fl<0)return;h=s.flights[unsigned(fl)].hand&1u;}
    const std::uint16_t lidx=fl<0?s.grab[h].levelIndex:s.flights[unsigned(fl)].levelIndex;const std::uintptr_t want=fl<0?s.grab[h].propCol:s.flights[unsigned(fl)].col;
    const auto col=activeColliderAt(s.level,lidx,prop);if(!col||col!=want)return;
    auto& slots=fl<0?s.propContacts:s.flightContacts[unsigned(fl)];
    const auto t0=nowNs();auto& P=stats.propHit;bump(P.scans);
    const V3 com=vec3(col+off::colliderPos),vCom=vec3(col+off::colliderVel),w=vec3(col+off::colliderAngVel);
    const auto arrA=raw<std::uintptr_t>(it+off::itArrA),arrB=raw<std::uintptr_t>(it+off::itArrB);
    const auto cA=raw<std::int32_t>(it+off::itCountA),cB=raw<std::int32_t>(it+off::itCountB);
    if(cA<0||cB<0||cA>4096||cB>4096)return;
    const int total=std::min(cA+cB,8);
    for(int i=0;i<total;++i){
        const auto m=i<cA?(arrA?raw<std::uintptr_t>(arrA+std::uintptr_t(i)*8u):0):(arrB?raw<std::uintptr_t>(arrB+std::uintptr_t(i-cA)*8u):0);if(!m)continue;
        const unsigned subs=std::min<unsigned>(std::max<unsigned>(raw<std::uint8_t>(m+off::mfSubCount),1u),16u);
        for(unsigned k=0;k<subs;++k){
            const auto sub=k?raw<std::uintptr_t>(m+off::mfSubs+std::uintptr_t(k)*8u):m;if(!sub)continue;
            const unsigned nc=std::min<unsigned>(raw<std::uint8_t>(sub+off::mfContactCount),64u);
            for(unsigned j=0;j<nc;++j){const auto c=sub+off::mfContacts+std::uintptr_t(j)*off::ctStride;
                if(!raw<std::uint8_t>(c+off::ctActive))continue;
                const auto own=raw<std::uintptr_t>(c+off::ctOwner);if(!own)continue;
                const auto A=raw<std::uintptr_t>(own+off::mfInstA),B=raw<std::uintptr_t>(own+off::mfInstB);
                const auto other=A==prop?B:B==prop?A:0;if(!other)continue;
                const auto vt=raw<std::uintptr_t>(other);if(!vt||raw<std::uintptr_t>(vt+0x10)!=engine.base+off::fnMoverClass)continue;
                const float depth=raw<float>(c+off::ctDepth);if(!std::isfinite(depth))continue;
                // the player's own capsule: the guard's contact (its normal from the prop into the capsule across up), never a hit slot
                if(other==s.moverInstAddr&&fl>=0)continue;
                if(other==s.moverInstAddr){bump(stats.pairStatics.playerContacts);
                    const V3 pos=vec3(c+(other==A?off::ctPosB:off::ctPosA));V3 n=guardNormal(vec3(c+off::ctNormal),s.tg.up);
                    if(v::dot(n,v::sub(vec3(other+off::instPos),pos))<0.f)n=v::mul(n,-1.f);
                    if(!(v::length(n)>.5f))bump(stats.pairStatics.playerSteep);
                    if(v::finite(n)&&v::length(n)>.5f){if(s.playerContactStep!=s.substep||depth>s.playerContactDepth){s.playerContactN=n;s.playerContactDepth=depth;}
                        s.playerContactStep=s.substep;}
                    continue;}
                bump(P.contacts);for(auto& gg:s.grab)if(gg.active&&gg.prop==prop)++gg.geo.eng;
                PropContact* e=nullptr;for(auto& x:slots)if(x.mover==other){e=&x;break;}
                if(!e){for(auto& x:slots)if(!x.mover){e=&x;break;}if(!e)continue;*e=PropContact{};}
                const V3 pos=vec3(c+(other==A?off::ctPosB:off::ctPosA));V3 n=vec3(c+off::ctNormal);
                if(v::dot(n,v::sub(vec3(other+off::instPos),pos))<0.f)n=v::mul(n,-1.f);
                const V3 vp=v::add(vCom,v::cross(w,v::sub(pos,com)));
                mergePropContact(*e,other,h,pos,n,vp,depth);}}}
    maxOf(P.scanNsMax,nowNs()-t0);
}
inline void preApplyGuard(const void* it) noexcept {
    const auto inst=raw<std::uintptr_t>(addr(it)+off::iterInst);
    if(inst==instOf(0)){bump(stats.hand[0].impactSkips);glassScan(0,addr(it));return;}
    if(inst==instOf(1)){bump(stats.hand[1].impactSkips);glassScan(1,addr(it));return;}
    if(inst&&(inst==propScanInst[0]||inst==propScanInst[1]||inst==propScanInst[2]||inst==propScanInst[3]))propScan(inst,addr(it));
    if(const auto game=engine.preApplyGame)game(it);
}
inline std::uintptr_t guardWord() noexcept {return reinterpret_cast<std::uintptr_t>(&preApplyGuard);}
// PLAYER SKIP (lever grabplayerskip): the simulator's PreComputeImpacts fn (sim+0xc8, DefaultPreComputeImpacts 0x1c27c24), called by the
// same functor thunk 0x1c3111c from PerformImpactsCallbacks in PreSolveCallbacks (0x1c371ec), i.e. before SolveConstraints in
// phContactMgr::UpdateNew 0x1c36d88, for both instances of every manifold. For the iterator of a held prop in a skip slot (s.skip) the
// contacts with the player's own mover instance are switched off as phImpact::Iterator::DisableImpact 0x28b4290 does (contact +0xce = 0;
// the engine's own mvrMoverInst::PreComputeImpacts 0x2890544 / 0x28906a8 / 0x289132c makes the same write), so the solver neither pushes
// the prop out of the player's capsule nor the player off it; NPC contacts and every other iterator are untouched. Every iterator then
// goes to DefaultPreComputeImpacts unchanged.
inline void preComputeGuard(const void* it) noexcept {
    const auto inst=raw<std::uintptr_t>(addr(it)+off::iterInst);
    if(inst){auto& s=state();
        const int hk=skipHeldSlot(s.skip,inst),lk=hk<0?skipLingerSlot(s.skip,inst):-1;const bool held=hk>=0,linger=lk>=0;
        if((held||linger)&&s.moverOk&&s.moverInstAddr&&s.mode==ModeOn&&threadKey()==s.mainKey){
            const auto t0=nowNs();auto& K=stats.playerSkip;
            bump(linger?K.lingerIters:K.iterators);const bool light=held&&s.skip.light[unsigned(hk)];if(light)bump(K.lightIters);
            const ImpactLayout L{};
            const SkipOut o=skipPlayerContacts(addr(it),inst,s.moverInstAddr,L,
                [](std::uintptr_t a,void* out,std::size_t n){std::memcpy(out,reinterpret_cast<const void*>(a),n);},
                [](std::uintptr_t a,const void* in,std::size_t n){std::memcpy(reinterpret_cast<void*>(a),in,n);});
            if(o.contacts){bump(K.manifolds,o.manifolds);bump(K.contacts,o.contacts);maxOf(K.depthMaxUm,std::uint64_t(std::max(0.f,o.depthMax/std::max(s.upm,1e-3f))*1e6f));
                if(held){const unsigned k=unsigned(hk);s.skipSeenNs=t0;if(s.skipStep[k]!=s.substep){s.skipStep[k]=s.substep;s.skipStepContacts[k]=0;s.skipStepDepth[k]=0.f;}
                    s.skipStepContacts[k]+=o.contacts;s.skipStepDepth[k]=std::max(s.skipStepDepth[k],o.depthMax);
                    if(light){const float um=std::max(0.f,o.depthMax/std::max(s.upm,1e-3f))*1e6f;bump(K.lightContacts,o.contacts);maxOf(K.lightDepthMaxUm,std::uint64_t(um));
                        auto& g=s.grab[k];if(g.active&&g.prop==inst){g.capContacts+=o.contacts;g.capDepthMm=std::max(g.capDepthMm,um*1e-3f);}}}
                else{auto& l=s.skip.linger[unsigned(lk)];l.seenNs=t0;if(l.step!=s.substep){l.step=s.substep;l.depth=0.f;}l.depth=std::max(l.depth,o.depthMax);}}
            maxOf(K.nsMax,nowNs()-t0);}}
    if(const auto game=engine.preComputeDefault)game(it);
}
inline std::uintptr_t preComputeWord() noexcept {return reinterpret_cast<std::uintptr_t>(&preComputeGuard);}
inline void buildTable() noexcept {
    behaviourTable[0]=nullptr;behaviourTable[1]=reinterpret_cast<void*>(engine.base+off::behaviourTypeinfo);
    void* slots[12]{reinterpret_cast<void*>(&behDtor),reinterpret_cast<void*>(&behDtor),reinterpret_cast<void*>(&behReset),reinterpret_cast<void*>(&behIsActive),
        reinterpret_cast<void*>(&behIsAsleep),reinterpret_cast<void*>(&preUpdate),reinterpret_cast<void*>(&update),reinterpret_cast<void*>(&collideObjects),
        reinterpret_cast<void*>(&behActivateWhenHit),reinterpret_cast<void*>(&behUpdateConstraint),reinterpret_cast<void*>(&behIsForceField),reinterpret_cast<void*>(&behIsCloth)};
    for(unsigned i=0;i<12;++i)behaviourTable[2+i]=slots[i];
}

// ---- tick side ----
inline unsigned readMode() noexcept {
    const int forced=modeForTests.load(std::memory_order_relaxed);
    if(forced>=0)return unsigned(forced)>ModeOn?ModeOn:unsigned(forced);
    if(killed(Feature::HandPhys))return ModeOff;
    const unsigned v=vrSettings.value(Key::HandPhysics);return v>ModeOn?ModeOff:v;
}
inline bool leverText(const char* name,char* text,std::size_t size) noexcept {
    const auto fn=arc::propertyReader.load(std::memory_order_acquire);
    return fn&&fn(name,text,size)&&text[0];
}
inline float leverFloat(const char* name,float fallback,float scale) noexcept {
    char text[96]{};if(!leverText(name,text,sizeof text))return fallback;
    char* end{};const float v=std::strtof(text,&end);return end!=text&&!*end&&std::isfinite(v)?v*scale:fallback;
}
inline bool leverBool(const char* name,bool fallback) noexcept {char text[96]{};if(!leverText(name,text,sizeof text))return fallback;return text[0]!='0';}
inline unsigned leverUint(const char* name,unsigned fallback) noexcept {
    char text[96]{};if(!leverText(name,text,sizeof text))return fallback;
    char* end{};const unsigned long v=std::strtoul(text,&end,0);return end!=text&&!*end&&v<=0xffffu?unsigned(v):fallback;
}
// A small signed lever (-1 / 0 / 1 style values).
inline int leverInt(const char* name,int fallback) noexcept {
    char text[96]{};if(!leverText(name,text,sizeof text))return fallback;
    char* end{};const long v=std::strtol(text,&end,10);return end!=text&&!*end&&v>=-1000&&v<=1000?int(v):fallback;
}
inline std::uint32_t leverHex(const char* name) noexcept {
    char text[96]{};if(!leverText(name,text,sizeof text))return 0;
    char* end{};const unsigned long v=std::strtoul(text,&end,0);return end!=text&&!*end?std::uint32_t(v):0u;
}
// The shape the bodies get: PALM falls back to the capsule while its group is refused.
inline unsigned shapeFor(const Levers& l) noexcept {return l.shape==ShapePalm&&!groupOk[GroupPalm]?unsigned(ShapeCapsule):l.shape;}
// Hook A reads the levers with the shape the bodies get (its body centre and visual follow the same shape).
inline void publishLevers(const State& s) noexcept {Levers p=s.lev;p.shape=s.built?s.builtShape:shapeFor(s.lev);levers.store(p);}
inline void readLevers(State& s) noexcept {
    Levers l{};const Levers d{};
#define RDRVR_HP(n) "debug.rdrvr.vr.handphys." n
    l.shape=leverUint(RDRVR_HP("shape"),d.shape);
    l.radiusM=leverFloat(RDRVR_HP("radius"),d.radiusM,.01f);l.lengthM=leverFloat(RDRVR_HP("length"),d.lengthM,.01f);l.anchorFwdM=leverFloat(RDRVR_HP("anchor"),d.anchorFwdM,.01f);
    l.massKg=leverFloat(RDRVR_HP("mass"),d.massKg,1.f);l.inertiaK=leverFloat(RDRVR_HP("inertia"),d.inertiaK,1.f);
    l.maxSpeedMps=leverFloat(RDRVR_HP("maxspeed"),d.maxSpeedMps,1.f);l.maxAngRadS=leverFloat(RDRVR_HP("maxspin"),d.maxAngRadS,1.f);
    l.contactSpinRadS=leverFloat(RDRVR_HP("contactspin"),d.contactSpinRadS,1.f);
    l.creepMps=leverFloat(RDRVR_HP("creep"),d.creepMps,1.f);l.pushSpeedMps=leverFloat(RDRVR_HP("pushspeed"),d.pushSpeedMps,1.f);
    l.tangentMps=leverFloat(RDRVR_HP("tangent"),d.tangentMps,1.f);
    l.leashM=leverFloat(RDRVR_HP("leash"),d.leashM,.01f);l.leashMs=leverFloat(RDRVR_HP("leashms"),d.leashMs,1.f);
    l.returnM=leverFloat(RDRVR_HP("returncm"),d.returnM,.01f);l.staleMs=leverFloat(RDRVR_HP("stalems"),d.staleMs,1.f);
    l.visualMs=leverFloat(RDRVR_HP("visualms"),d.visualMs,1.f);l.holdMs=leverFloat(RDRVR_HP("holdms"),d.holdMs,1.f);
    l.rotCapDeg=leverFloat(RDRVR_HP("rotcap"),d.rotCapDeg,1.f);l.liftM=leverFloat(RDRVR_HP("lift"),d.liftM,.001f);
    l.reachM=leverFloat(RDRVR_HP("reachcm"),d.reachM,.01f);l.reachLeadS=leverFloat(RDRVR_HP("reachlead"),d.reachLeadS,.001f);
    l.wakeMax=leverUint(RDRVR_HP("wakemax"),d.wakeMax);
    l.npcBump=leverBool(RDRVR_HP("npcbump"),d.npcBump);l.npcScanTicks=leverUint(RDRVR_HP("npcscan"),d.npcScanTicks);
    l.ccd=leverBool(RDRVR_HP("ccd"),d.ccd);l.ownVt=leverBool(RDRVR_HP("ownvt"),d.ownVt);l.fist=leverBool(RDRVR_HP("fist"),d.fist);
    l.doors=leverBool(RDRVR_HP("doors"),d.doors);l.doorClose=leverBool(RDRVR_HP("doorclose"),d.doorClose);l.props=leverBool(RDRVR_HP("props"),d.props);
    l.reachWake=leverBool(RDRVR_HP("reachwake"),d.reachWake);l.npc=leverBool(RDRVR_HP("npc"),d.npc);
    l.visual=leverBool(RDRVR_HP("visual"),d.visual);l.haptic=leverBool(RDRVR_HP("haptic"),d.haptic);
    l.type=leverHex(RDRVR_HP("type"));l.include=leverHex(RDRVR_HP("include"));
    l.doorSwing=leverBool(RDRVR_HP("doorswing"),d.doorSwing);l.doorKeep=leverFloat(RDRVR_HP("doorkeep"),d.doorKeep,.01f);
    l.doorRateRadS=leverFloat(RDRVR_HP("doorrate"),d.doorRateRadS,1.f);l.doorIdleMs=leverFloat(RDRVR_HP("dooridlems"),d.doorIdleMs,1.f);
    l.unlock=leverBool(RDRVR_HP("unlock"),d.unlock);l.npcBody=leverBool(RDRVR_HP("npcbody"),d.npcBody);l.npcPress=leverBool(RDRVR_HP("npcpress"),d.npcPress);
    l.npcSlap=leverBool(RDRVR_HP("npcslap"),d.npcSlap);l.npcHorse=leverBool(RDRVR_HP("npchorse"),d.npcHorse);l.slapMps=leverFloat(RDRVR_HP("slapmps"),d.slapMps,1.f);
    l.doorFollow=leverBool(RDRVR_HP("doorfollow"),d.doorFollow);l.doorRest=leverBool(RDRVR_HP("doorrest"),d.doorRest);l.doorPlain=leverBool(RDRVR_HP("doorplain"),d.doorPlain);
    l.doorShutDeg=leverFloat(RDRVR_HP("doorshut"),d.doorShutDeg,1.f);l.doorRearmMs=leverFloat(RDRVR_HP("doorrearm"),d.doorRearmMs,1.f);
    l.release=leverBool(RDRVR_HP("release"),d.release);l.releaseRM=leverFloat(RDRVR_HP("releaser"),d.releaseRM,.01f);
    l.releaseDepthM=leverFloat(RDRVR_HP("releasedepth"),d.releaseDepthM,.001f);
    l.npcShove=leverBool(RDRVR_HP("npcshove"),d.npcShove);l.npcPush=leverBool(RDRVR_HP("npcpush"),d.npcPush);l.shoveMps=leverFloat(RDRVR_HP("shovemps"),d.shoveMps,1.f);
    l.pushGain=leverFloat(RDRVR_HP("pushgain"),d.pushGain,1.f);l.pushMaxM=leverFloat(RDRVR_HP("pushmax"),d.pushMaxM,.01f);l.pushCapM=leverFloat(RDRVR_HP("pushcap"),d.pushCapM,.01f);
    l.slapYawRad=leverFloat(RDRVR_HP("slapyaw"),d.slapYawRad,1.f);l.slapReact=leverBool(RDRVR_HP("slapreact"),d.slapReact);
    l.grab=leverBool(RDRVR_HP("grab"),d.grab);l.grabM=leverFloat(RDRVR_HP("grabcm"),d.grabM,.01f);l.grabForceN=leverFloat(RDRVR_HP("grabforce"),d.grabForceN,1.f);
    l.grabTorqueNm=leverFloat(RDRVR_HP("grabtorque"),d.grabTorqueNm,1.f);l.grabMassKg=leverFloat(RDRVR_HP("grabmass"),d.grabMassKg,1.f);
    l.grabMaxKg=leverFloat(RDRVR_HP("grabkg"),d.grabMaxKg,1.f);l.grabRot=leverBool(RDRVR_HP("grabrot"),d.grabRot);l.grabClass=leverUint(RDRVR_HP("grabclass"),d.grabClass);
    l.grabOpenMs=leverFloat(RDRVR_HP("grabopenms"),d.grabOpenMs,1.f);l.grabLeashM=leverFloat(RDRVR_HP("grabbreakcm"),leverFloat(RDRVR_HP("grableash"),d.grabLeashM,.01f),.01f);
    l.grabThrow=leverFloat(RDRVR_HP("grabthrow"),d.grabThrow,1.f);l.cpu=leverBool(RDRVR_HP("cpu"),d.cpu);
    l.unfreeze=leverBool(RDRVR_HP("unfreeze"),d.unfreeze);l.unfreezeRM=leverFloat(RDRVR_HP("unfreezer"),d.unfreezeRM,.01f);
    l.unfreezeKg=leverFloat(RDRVR_HP("unfreezekg"),d.unfreezeKg,1.f);l.grabReleaseRM=leverFloat(RDRVR_HP("grabreleaser"),d.grabReleaseRM,.01f);
    l.doorAuto=leverBool(RDRVR_HP("doorauto"),d.doorAuto);l.doorBody=leverBool(RDRVR_HP("doorbody"),d.doorBody);
    l.doorHandPose=leverBool(RDRVR_HP("doorhandpose"),d.doorHandPose);l.doorStuckMs=leverFloat(RDRVR_HP("doorstuckms"),d.doorStuckMs,1.f);
    l.grabSeat=leverBool(RDRVR_HP("grabseat"),d.grabSeat);l.grabWeld=leverBool(RDRVR_HP("grabweld"),d.grabWeld);
    l.grabLightFf=leverBool(RDRVR_HP("grablightff"),d.grabLightFf);l.bloodHires=leverBool(RDRVR_HP("bloodhires"),d.bloodHires);
    l.grabSkipLight=leverBool(RDRVR_HP("grabskiplight"),d.grabSkipLight);
    l.blood=leverBool(RDRVR_HP("blood"),d.blood);l.bloodCdMs=leverFloat(RDRVR_HP("bloodcdms"),d.bloodCdMs,1.f);
    l.glass=leverBool(RDRVR_HP("glass"),d.glass);l.glassMps=leverFloat(RDRVR_HP("glassmps"),d.glassMps,1.f);l.glassDmg=leverFloat(RDRVR_HP("glassdmg"),d.glassDmg,1.f);
    ::rdrvr::vr::handblood::enabled.store(leverBool(RDRVR_HP("bloodtint"),true),std::memory_order_relaxed);
    ::rdrvr::vr::handblood::holdS.store(std::clamp(leverFloat(RDRVR_HP("bloodtints"),60.f,1.f),1.f,3600.f),std::memory_order_relaxed);
    l.bloodPlace=leverBool(RDRVR_HP("bloodplace"),d.bloodPlace);l.bloodMps=leverFloat(RDRVR_HP("bloodmps"),d.bloodMps,1.f);
    l.doorPress=leverBool(RDRVR_HP("doorpress"),d.doorPress);
    l.gunGlass=leverInt(RDRVR_HP("gunglass"),d.gunGlass);l.gunGlassFree=leverInt(RDRVR_HP("gunglassfree"),d.gunGlassFree);
    l.gunGlassMps=leverFloat(RDRVR_HP("gunglassmps"),l.glassMps,1.f);l.gunGlassR=leverFloat(RDRVR_HP("gunglassr"),d.gunGlassR,1.f);
    l.gunGlassLong=leverBool(RDRVR_HP("gunglasslong"),d.gunGlassLong);
    l.unfreezeParts=leverUint(RDRVR_HP("unfreezeparts"),d.unfreezeParts);l.unfreezeSetKg=leverFloat(RDRVR_HP("unfreezesetkg"),d.unfreezeSetKg,1.f);
    l.unfreezeKick=leverBool(RDRVR_HP("unfreezekick"),d.unfreezeKick);
    l.unfreezeNear=leverBool(RDRVR_HP("unfreezenear"),d.unfreezeNear);l.unfreezeAheadS=leverFloat(RDRVR_HP("unfreezeaheadms"),d.unfreezeAheadS,.001f);
    l.grabDrive=leverBool(RDRVR_HP("grabdrive"),d.grabDrive);l.grabGain=leverFloat(RDRVR_HP("grabgain"),d.grabGain,1.f);
    l.grabForceMaxN=leverFloat(RDRVR_HP("grabforcemax"),d.grabForceMaxN,1.f);l.grabAccel=leverFloat(RDRVR_HP("grabaccel"),d.grabAccel,1.f);
    l.grabLiftKg=leverFloat(RDRVR_HP("grablift"),d.grabLiftKg,1.f);l.grabRotKg=leverFloat(RDRVR_HP("grabrotkg"),d.grabRotKg,1.f);
    l.grabBreakMs=leverFloat(RDRVR_HP("grabbreakms"),d.grabBreakMs,1.f);l.grabMove=leverBool(RDRVR_HP("grabmove"),d.grabMove);l.grabDriveArt=leverBool(RDRVR_HP("grabdriveart"),d.grabDriveArt);
    l.grabMoveKg=leverFloat(RDRVR_HP("grabmovekg"),d.grabMoveKg,1.f);l.grabMoveRM=leverFloat(RDRVR_HP("grabmover"),d.grabMoveRM,.01f);
    l.grabGrip=leverBool(RDRVR_HP("grabgrip"),d.grabGrip);l.grabWn=leverFloat(RDRVR_HP("grabwn"),d.grabWn,1.f);l.grabZeta=leverFloat(RDRVR_HP("grabzeta"),d.grabZeta,1.f);
    l.grabSlewN=leverFloat(RDRVR_HP("grabslew"),d.grabSlewN,1.f);l.grabDownG=leverFloat(RDRVR_HP("grabdowng"),d.grabDownG,1.f);
    l.resetHold=leverBool(RDRVR_HP("resethold"),d.resetHold);
    l.pushFirmMps=leverFloat(RDRVR_HP("pushfirmmps"),d.pushFirmMps,1.f);l.pushFirmM=leverFloat(RDRVR_HP("pushfirmcm"),d.pushFirmM,.01f);
    l.pushArmMps=leverFloat(RDRVR_HP("pusharmmps"),d.pushArmMps,1.f);l.pushImpulse=leverFloat(RDRVR_HP("pushimpulse"),d.pushImpulse,1.f);
    l.pushMedium=leverUint(RDRVR_HP("pushmedium"),d.pushMedium);l.pushKick=leverBool(RDRVR_HP("pushkick"),d.pushKick);
    l.pushKickK=leverFloat(RDRVR_HP("pushkickk"),d.pushKickK,1.f);l.pushKickMaxNs=leverFloat(RDRVR_HP("pushkickmax"),d.pushKickMaxNs,1.f);
    l.pushGrade=leverBool(RDRVR_HP("pushgrade"),d.pushGrade);l.pushDownMps=leverFloat(RDRVR_HP("pushdownmps"),d.pushDownMps,1.f);
    l.slapHoldMs=leverFloat(RDRVR_HP("slapholdms"),d.slapHoldMs,1.f);l.pairMs=leverFloat(RDRVR_HP("pairms"),d.pairMs,1.f);l.pushKick2=leverBool(RDRVR_HP("pushkick2"),d.pushKick2);
    l.pushKickServo=leverBool(RDRVR_HP("pushkickservo"),d.pushKickServo);l.pushKickEarly=leverBool(RDRVR_HP("pushkickearly"),d.pushKickEarly);
    l.grabChord=leverBool(RDRVR_HP("grabchord"),d.grabChord);l.grabHeldLatch=leverBool(RDRVR_HP("grabheldlatch"),d.grabHeldLatch);
    l.grabHoldLatch=leverBool(RDRVR_HP("grabholdlatch"),d.grabHoldLatch);l.grabGhost=leverBool(RDRVR_HP("grabghost"),d.grabGhost);
    l.grabHeavyHold=leverBool(RDRVR_HP("grabheavyhold"),d.grabHeavyHold);l.grabPairFull=leverBool(RDRVR_HP("grabpairfull"),d.grabPairFull);
    l.grabPairSlew=leverBool(RDRVR_HP("grabpairslew"),d.grabPairSlew);l.grabHeavyDraw=leverBool(RDRVR_HP("grabheavydraw"),d.grabHeavyDraw);
    l.grab2=leverBool(RDRVR_HP("grab2"),d.grab2);l.grabLift2Kg=leverFloat(RDRVR_HP("grablift2"),d.grabLift2Kg,1.f);
    l.grabJoinM=leverFloat(RDRVR_HP("grabjoincm"),d.grabJoinM,.01f);l.grabFirst=leverBool(RDRVR_HP("grabfirst"),d.grabFirst);
    l.heavyMask=leverBool(RDRVR_HP("heavymask"),d.heavyMask);l.heavyMaskM=leverFloat(RDRVR_HP("heavymaskcm"),d.heavyMaskM,.01f);
    l.propHit=leverBool(RDRVR_HP("prophit"),d.propHit);l.propHitMps=leverFloat(RDRVR_HP("prophitmps"),d.propHitMps,1.f);
    l.propHitStrongMps=leverFloat(RDRVR_HP("prophitstrong"),d.propHitStrongMps,1.f);l.propHitCoolMs=leverFloat(RDRVR_HP("prophitcoolms"),d.propHitCoolMs,1.f);
    l.propHitKg=leverFloat(RDRVR_HP("prophitkg"),d.propHitKg,1.f);l.propKo=leverFloat(RDRVR_HP("propko"),d.propKo,1.f);
    l.npcPassive=leverBool(RDRVR_HP("npcpassive"),d.npcPassive);l.activeMps=leverFloat(RDRVR_HP("npcactivemps"),d.activeMps,1.f);
    l.pressDeadMps=leverFloat(RDRVR_HP("pressdeadmps"),d.pressDeadMps,1.f);l.shoveArmMps=leverFloat(RDRVR_HP("shovearmmps"),d.shoveArmMps,1.f);
    l.grabPairStatics=leverBool(RDRVR_HP("grabpairstatics"),d.grabPairStatics);l.grabPlayerGuard=leverBool(RDRVR_HP("grabplayerguard"),d.grabPlayerGuard);
    l.propHitGeo=leverBool(RDRVR_HP("prophitgeo"),d.propHitGeo);l.propHitArmMps=leverFloat(RDRVR_HP("prophitarm"),d.propHitArmMps,1.f);
    l.propHitShellM=leverFloat(RDRVR_HP("prophitshellcm"),d.propHitShellM,.01f);l.propHitWinMs=leverFloat(RDRVR_HP("prophitwinms"),d.propHitWinMs,1.f);
    l.propHitArmMs=leverFloat(RDRVR_HP("prophitarmms"),d.propHitArmMs,1.f);l.propHitArmDirMps=leverFloat(RDRVR_HP("prophitarmdir"),d.propHitArmDirMps,1.f);
    l.slapAttacker=leverUint(RDRVR_HP("slapattacker"),d.slapAttacker);l.pushAttacker=leverUint(RDRVR_HP("pushattacker"),d.pushAttacker);
    l.grabHeavyLeash=leverBool(RDRVR_HP("grabheavyleash"),d.grabHeavyLeash);l.grabSafetyM=leverFloat(RDRVR_HP("grabsafetycm"),d.grabSafetyM,.01f);
    l.grabTeleportM=leverFloat(RDRVR_HP("grabteleportcm"),d.grabTeleportM,.01f);l.grabPlayerSkip=leverBool(RDRVR_HP("grabplayerskip"),d.grabPlayerSkip);
    l.grabSkipLingerMs=leverFloat(RDRVR_HP("grabskiplingerms"),d.grabSkipLingerMs,1.f);l.grabPairKg=leverFloat(RDRVR_HP("grabpairkg"),d.grabPairKg,1.f);
    l.grabHeldAccel=leverFloat(RDRVR_HP("grabheldaccel"),d.grabHeldAccel,1.f);l.grabFeedFwd=leverBool(RDRVR_HP("grabfeedfwd"),d.grabFeedFwd);
    l.pushBusyWait=leverBool(RDRVR_HP("pushbusywait"),d.pushBusyWait);l.pushBusyWaitMs=leverFloat(RDRVR_HP("pushbusywaitms"),d.pushBusyWaitMs,1.f);
    l.pushEscGrade=leverBool(RDRVR_HP("pushescgrade"),d.pushEscGrade);l.pushFacingBody=leverBool(RDRVR_HP("pushfacingbody"),d.pushFacingBody);
    l.pushPeak=leverBool(RDRVR_HP("pushpeak"),d.pushPeak);l.pushPeakMs=leverFloat(RDRVR_HP("pushpeakms"),d.pushPeakMs,1.f);
    l.pushFront=leverUint(RDRVR_HP("pushfront"),d.pushFront);l.pushFrontImp=leverFloat(RDRVR_HP("pushfrontimp"),d.pushFrontImp,1.f);
    l.pushKickHigh=leverBool(RDRVR_HP("pushkickhigh"),d.pushKickHigh);
    l.pushFrozenMsg=leverBool(RDRVR_HP("pushfrozenmsg"),d.pushFrozenMsg);l.pushBehindImp=leverFloat(RDRVR_HP("pushbehindimp"),d.pushBehindImp,1.f);
    l.pushSide=leverUint(RDRVR_HP("pushside"),d.pushSide);
    l.pushKnock=leverUint(RDRVR_HP("pushknock"),d.pushKnock);l.pushFrozenHold=leverBool(RDRVR_HP("pushfrozenhold"),d.pushFrozenHold);
    l.grabCarry=leverBool(RDRVR_HP("grabcarry"),d.grabCarry);l.grabCarryWn=leverFloat(RDRVR_HP("grabcarrywn"),d.grabCarryWn,1.f);
    l.grabCarryZeta=leverFloat(RDRVR_HP("grabcarryzeta"),d.grabCarryZeta,1.f);l.grabCarryAccel=leverFloat(RDRVR_HP("grabcarryaccel"),d.grabCarryAccel,1.f);
    l.grabRotWn=leverFloat(RDRVR_HP("grabrotwn"),d.grabRotWn,1.f);l.grabRotZeta=leverFloat(RDRVR_HP("grabrotzeta"),d.grabRotZeta,1.f);
    l.grabRotNm=leverFloat(RDRVR_HP("grabrotnm"),d.grabRotNm,1.f);l.grabLineTauMs=leverFloat(RDRVR_HP("grablinetaums"),d.grabLineTauMs,1.f);
    l.grabStaleOwn=leverBool(RDRVR_HP("grabstaleown"),d.grabStaleOwn);l.grabGroundZero=leverBool(RDRVR_HP("grabgroundzero"),d.grabGroundZero);
    l.grabFallbackTauMs=leverFloat(RDRVR_HP("grabfallbacktaums"),d.grabFallbackTauMs,1.f);
    l.grabGroundLift=leverBool(RDRVR_HP("grabgroundlift"),d.grabGroundLift);l.grabCarryPredict=leverBool(RDRVR_HP("grabcarrypredict"),d.grabCarryPredict);
    l.grabCarryKeepMs=leverFloat(RDRVR_HP("grabcarrykeepms"),d.grabCarryKeepMs,1.f);l.pushKnockFlip=leverUint(RDRVR_HP("pushknockflip"),d.pushKnockFlip);
    l.pushKnockPick=leverUint(RDRVR_HP("pushknockpick"),d.pushKnockPick);l.pushKnockPickFwd=leverFloat(RDRVR_HP("pushknockpickfwd"),d.pushKnockPickFwd,1.f);
    l.pushKnockSpine=leverBool(RDRVR_HP("pushknockspine"),d.pushKnockSpine);l.pushKnockPickMsg=leverUint(RDRVR_HP("pushknockpickmsg"),d.pushKnockPickMsg);
    l.pushKnockTough=leverBool(RDRVR_HP("pushknocktough"),d.pushKnockTough);l.pushKnockPickFunc=leverBool(RDRVR_HP("pushknockpickfunc"),d.pushKnockPickFunc);
    l.knockOffMs=leverFloat(RDRVR_HP("knockoffms"),d.knockOffMs,1.f);
    l.throwMode=leverUint(RDRVR_HP("throw"),d.throwMode);l.throwLight=leverUint(RDRVR_HP("throwlight"),d.throwLight);
    l.throwMps=leverFloat(RDRVR_HP("throwmps"),d.throwMps,1.f);l.throwMpsLight=leverFloat(RDRVR_HP("throwmpslight"),d.throwMpsLight,1.f);
    l.throwRelMps=leverFloat(RDRVR_HP("throwrelmps"),d.throwRelMps,1.f);l.throwRelMpsLight=leverFloat(RDRVR_HP("throwrelmpslight"),d.throwRelMpsLight,1.f);
    l.throwWinMs=leverFloat(RDRVR_HP("throwwinms"),d.throwWinMs,1.f);l.throwPairMs=leverFloat(RDRVR_HP("throwpairms"),d.throwPairMs,1.f);
    l.throwMaxMps=leverFloat(RDRVR_HP("throwmaxmps"),d.throwMaxMps,1.f);l.throwMaxMpsLight=leverFloat(RDRVR_HP("throwmaxmpslight"),d.throwMaxMpsLight,1.f);
    l.throwFlightMs=leverFloat(RDRVR_HP("throwflightms"),d.throwFlightMs,1.f);l.throwHitMps=leverFloat(RDRVR_HP("throwhitmps"),d.throwHitMps,1.f);
    l.throwStrongKgMps=leverFloat(RDRVR_HP("throwstrongkgmps"),d.throwStrongKgMps,1.f);l.throwKo=leverFloat(RDRVR_HP("throwko"),d.throwKo,1.f);
    l.pressPushM=leverFloat(RDRVR_HP("presspush"),d.pressPushM,.01f);l.slapWindowMs=leverFloat(RDRVR_HP("slapwindowms"),d.slapWindowMs,1.f);
    // NPC reaction nodes (text; published only when one changes)
    {TouchNodes n=s.nodes;char t[96]{};
     const auto take=[&](const char* name,char (&out)[64]){t[0]=0;if(!leverText(name,t,sizeof t))t[0]=0;t[sizeof t-1]=0;
        char v[64]{};std::size_t k=0;for(;k<sizeof v-1&&t[k];++k)v[k]=(t[k]>=0x20&&t[k]<=0x7e)?t[k]:'?';
        if(std::memcmp(v,out,sizeof v)!=0){std::memcpy(out,v,sizeof v);return true;}return false;};
     const bool a=take(RDRVR_HP("slapnode"),n.slap),b=take(RDRVR_HP("pushnode"),n.push);
     if(a||b||!s.nodes.serial){n.serial=s.nodes.serial+1u;s.nodes=n;touchNodes.store(n);}}
#undef RDRVR_HP
    if(const auto patch=leverPatchForTests.load(std::memory_order_relaxed))patch(l);
    s.lev=sanitized(l);publishLevers(s);bump(stats.leverReads);
    grablink::grabFirst.store(s.lev.grabFirst,std::memory_order_relaxed);grablink::heavyMask.store(s.lev.heavyMask,std::memory_order_relaxed);
    grablink::heavyNearM.store(s.lev.heavyMaskM,std::memory_order_relaxed);
    grablink::chordGuard.store(s.lev.grabChord,std::memory_order_relaxed);grablink::heldLatch.store(s.lev.grabHeldLatch,std::memory_order_relaxed);
    grablink::holdLatch.store(s.lev.grabHoldLatch,std::memory_order_relaxed);
}
inline bool sameShape(const State& s) noexcept {
    const Levers& a=s.lev;const Levers& b=s.builtWith;
    return shapeFor(a)==s.builtShape&&a.radiusM==b.radiusM&&a.lengthM==b.lengthM&&a.massKg==b.massKg&&a.inertiaK==b.inertiaK&&a.ccd==b.ccd&&
           a.ownVt==b.ownVt&&s.upm==s.builtUpm;
}
inline void removeBody(State& s,unsigned h) noexcept;
inline void finishHolds(State& s) noexcept;
inline void releaseGrab(State& s,unsigned h,std::uint8_t why,bool call=true) noexcept;
inline void heldMassRestore(State& s) noexcept;
inline void clearSkip(State& s) noexcept {s.skip=SkipSlots{};}
inline void unfreezeTick(State& s,std::uint64_t now,bool all) noexcept;
inline void resetHoldClear(State& s) noexcept;
// Permanent for this run: nothing is inserted again; the next tick runs OFF (bodies out, behaviours out of the list, the guard restored
// once the level no longer holds our instances).
inline void refuse(State& s,const char* why) noexcept {
    installState.store(why);ready.store(false,std::memory_order_release);bump(stats.refusals);
    finishHolds(s);unfreezeTick(s,0,true);resetHoldClear(s);for(unsigned h=0;h<2;++h)releaseGrab(s,h,GrabEndRefuse);
    heldMassRestore(s);clearSkip(s);
    for(unsigned h=0;h<2;++h)if(s.hand[h].present)removeBody(s,h);
}
// The instance's user data is our sagInstData with the engine constructor's layout, and the instance has the vptr build() gave it.
inline bool userOk(unsigned h) noexcept {
    const auto u=addr(objects[h&1u].user),inst=instOf(h);
    return raw<std::uintptr_t>(u)==engine.base+off::instDataVtable+0x10&&raw<std::uintptr_t>(u+0x8)==0&&raw<std::uintptr_t>(u+0x10)==inst&&
           raw<std::uint32_t>(u+0x18)==off::userKindSlot&&raw<std::uintptr_t>(inst+off::instUser)==u&&instVptr&&raw<std::uintptr_t>(inst)==instVptr;
}
inline void writePartFlags(HandObjects& o,bool fist) noexcept {
    o.partFlags[0][0]=o.partFlags[0][1]=0xffffffffu;
    o.partFlags[1][0]=o.partFlags[1][1]=fist?0u:0xffffffffu;
    o.partFlags[2][0]=o.partFlags[2][1]=fist?0xffffffffu:0u;
}
// The PALM composite of one hand: constructed and Init(3, false) once per process; later builds resize the boxes and rewrite the part
// matrices (identity rotation, row d = the part's offset) and the extents. False: the flags the box constructor reads are not 0, or the
// composite does not hold its parts.
inline bool buildPalm(State& s,HandObjects& o,bool fist) noexcept {
    const auto b=engine.base;std::uint8_t cloth{1},resCtor{1};
    if(!get(b+off::clothFlag,cloth)||cloth||!get(b+off::resCtorFlag,resCtor)||resCtor)return false;
    const float u=s.upm;
    if(!o.compReady){
        std::memset(o.comp,0,sizeof o.comp);std::memset(o.box,0,sizeof o.box);std::memset(o.qverts,0,sizeof o.qverts);
        for(unsigned i=0;i<3;++i){alignas(16) float size[4]{palmParts[i].size[0]*u,palmParts[i].size[1]*u,palmParts[i].size[2]*u,0.f};
            engine.boxCtor(o.box[i],o.qverts[i],size);
            if(raw<std::uintptr_t>(addr(o.box[i]))!=b+off::boxVtable+0x10)return false;
            put<std::uint32_t>(addr(o.box[i])+off::boundRefs,0x40000u);}
        engine.compCtor(o.comp);
        if(raw<std::uintptr_t>(addr(o.comp))!=b+off::compVtable+0x10)return false;
        engine.compInit(o.comp,3,false);
        for(unsigned i=0;i<3;++i)engine.compSetBound(o.comp,int(i),o.box[i],false);
        o.compReady=true;
    }else for(unsigned i=0;i<3;++i){alignas(16) float size[4]{palmParts[i].size[0]*u,palmParts[i].size[1]*u,palmParts[i].size[2]*u,0.f};engine.boxSize(o.box[i],size);}
    const auto c=addr(o.comp);const auto parts=raw<std::uintptr_t>(c+off::compParts),mats=raw<std::uintptr_t>(c+off::compMatrices);
    if(!parts||!mats||raw<std::uint16_t>(c+off::compCount)!=3)return false;
    for(unsigned i=0;i<3;++i){
        if(raw<std::uintptr_t>(parts+i*8u)!=addr(o.box[i]))return false;
        alignas(16) float m[16]{};writeMatrix({V3{1,0,0},V3{0,1,0},V3{0,0,1}},v::mul(palmParts[i].offset,u),m);
        std::memcpy(reinterpret_cast<void*>(mats+i*64u),m,sizeof m);
        put<std::uint32_t>(addr(o.box[i])+off::boundRefs,0x40000u);}
    writePartFlags(o,fist);
    put<std::uintptr_t>(c+off::compFlags,addr(o.partFlags));put<std::uintptr_t>(c+off::compLastFlags,addr(o.partFlags));
    engine.compExtents(o.comp,false);
    put<std::uint32_t>(c+off::boundRefs,0x40000u);
    return true;
}
// The bodies (main thread, both hands out of the level). Upm of this tick: the bound sizes are metres * upm.
inline void build(State& s) noexcept {
    unsigned shape=shapeFor(s.lev);
    const bool ownVt=s.lev.ownVt&&groupOk[GroupDeact]&&instTable[deactivateWord]==reinterpret_cast<std::uintptr_t>(&instNoDeactivate);
    instVptr=ownVt?addr(&instTable[2]):engine.base+off::instVtable+0x10;
    // PALM for both hands or for neither (a refused composite: the capsule from now on)
    if(shape==ShapePalm)for(unsigned h=0;h<2;++h)if(!buildPalm(s,objects[h],s.hand[h].fist)){bump(stats.buildFails);groupOk[GroupPalm]=false;shape=ShapeCapsule;break;}
    for(unsigned h=0;h<2;++h){
        auto& o=objects[h];
        std::memset(o.bound,0,sizeof o.bound);std::memset(o.arch,0,sizeof o.arch);std::memset(o.inst,0,sizeof o.inst);std::memset(o.user,0,sizeof o.user);
        const float r=s.lev.radiusM*s.upm;
        void* bound=shape==ShapePalm?static_cast<void*>(o.comp):static_cast<void*>(o.bound);
        if(shape==ShapeCapsule){engine.capsuleCtor(o.bound);engine.capsuleSize(o.bound,r,capsuleSegment(s.lev.lengthM,s.lev.radiusM)*s.upm);}
        else if(shape==ShapeSphere)engine.sphereCtor(o.bound,r);
        engine.archCtor(o.arch);engine.setBound(o.arch,bound);engine.setMass(o.arch,s.lev.massKg);
        if(shape==ShapePalm){
            const float k=s.lev.massKg*s.lev.inertiaK*s.upm*s.upm;
            alignas(16) float in[4]{palmInertiaPerKg[0]*k,palmInertiaPerKg[1]*k,palmInertiaPerKg[2]*k,0.f};engine.setAngInertia(o.arch,in);
            if(std::fabs(raw<float>(addr(o.arch)+off::archMaxAngSpeed)-4.f*pi)<1e-3f)put<float>(addr(o.arch)+off::archMaxAngSpeed,30.f);
            put<std::uint32_t>(addr(o.comp)+off::boundRefs,0x40000u);
        }
        engine.setGravity(o.arch,0.f);
        if(bound==o.bound)put<std::uint32_t>(addr(o.bound)+off::boundRefs,0x40000u);
        put<std::uint16_t>(addr(o.arch)+off::archRefs,0x4000u);
        put<std::uint32_t>(addr(o.arch)+off::archType,s.archType);put<std::uint32_t>(addr(o.arch)+off::archInclude,s.archInclude);
        alignas(16) float m[16]{};bodyMatrix({0,0,1},{0,0,0},m);
        engine.instCtor(o.inst);engine.instInit(o.inst,o.arch,m);
        // phInst() zeroes +0x18: the inst user data goes in after it
        engine.instDataCtor(o.user,o.inst);put<std::uintptr_t>(addr(o.inst)+off::instUser,addr(o.user));
        if(s.lev.ccd&&groupOk[GroupCcd])put<std::uint16_t>(addr(o.inst)+off::instFlags,std::uint16_t(raw<std::uint16_t>(addr(o.inst)+off::instFlags)|off::instCcd));
        if(ownVt&&raw<std::uintptr_t>(addr(o.inst))==engine.base+off::instVtable+0x10)put<std::uintptr_t>(addr(o.inst),instVptr);
        o.beh=Behaviour{};o.beh.vptr=&behaviourTable[2];o.beh.inst=o.inst;
    }
    s.built=true;s.builtWith=s.lev;s.builtUpm=s.upm;s.builtShape=shape;publishLevers(s);
    if(!userOk(0)||!userOk(1))refuse(s,"USER DATA MISMATCH");
}
inline bool flagsChanged(const State& s,std::uint32_t type,std::uint32_t include) noexcept {return s.archType!=type||s.archInclude!=include;}
inline void writeFlags(State& s,std::uint32_t type,std::uint32_t include) noexcept {
    s.archType=type;s.archInclude=include;
    for(auto& o:objects){put<std::uint32_t>(addr(o.arch)+off::archType,type);put<std::uint32_t>(addr(o.arch)+off::archInclude,include);}
}
// DeleteObject(index, false) may be delayed (sm_delaySAPAddRemove): the body is stopped first, and our slot stays until the level no
// longer holds the instance, so CollideObjects keeps dropping its pairs (present = false) until then.
inline void removeBody(State& s,unsigned h) noexcept {
    releaseGrab(s,h,GrabEndBody); // the constraint and the inertia first: DeleteObject comes after
    auto& hs=s.hand[h];const auto inst=instOf(h); // inst+0x18 stays: the level may still hold the instance
    const auto index=raw<std::uint16_t>(inst+off::instIndex);
    if(s.sim&&index!=off::noIndex&&inLevel(s.level,inst,index)){
        std::uint16_t ci{};
        if(const auto col=verifiedCollider(s.level,inst,ci))zeroVelocity(col);
        engine.deleteObject(reinterpret_cast<void*>(s.sim),int(index),false);bump(stats.hand[h].removes);}
    if(s.sim){if(index!=off::noIndex&&inLevel(s.level,inst,index))hs.pendingIndex=hs.index;else slotClear(s.sim,inst,hs.index,&objects[h].beh);}
    hs.present=false;hs.ghost=false;hs.index=off::noIndex;hs.haveCommand=false;hs.haveSolved=false;hs.est=ContactEst{};hs.edge=false;
}
// The old simulator is not touched again: our instances leave it on our side only (index, behaviour flag).
inline void forgetAll(State& s) noexcept {
    for(unsigned h=0;h<2;++h){const bool fist=s.hand[h].fist;s.hand[h]=HandState{};s.hand[h].fist=fist;const auto inst=instOf(h);
        if(s.built){put<std::uint16_t>(inst+off::instIndex,off::noIndex);put<std::uint16_t>(inst+off::instFlags,std::uint16_t(raw<std::uint16_t>(inst+off::instFlags)&~off::instHasBehaviour));}}
    s.listed={false,false};s.sim=0;s.level=0;s.holds={};s.unlockSeen={};s.rested={};s.unfrozen={};s.ufSeen={};
    // a hold of the old simulator: only the pointer goes (its pool may be gone)
    for(unsigned h=0;h<2;++h){if(s.grab[h].active){bump(stats.grab.ends[GrabEndDetached]);grablink::lastEnd[h].store(GrabEndDetached,std::memory_order_relaxed);
        grablink::setHolding(h,false,0);grablink::setHoldKind(h,grablink::HoldNone);}s.grab[h]=GrabHold{};}
    // the held mass record stays: the next heldMassTick restores it when the same simulator and level come back, else counts a miss
    clearSkip(s);s.pair=PairCache{};
}
inline void unregister(State& s) noexcept {
    for(unsigned h=0;h<2;++h){if(s.hand[h].present)removeBody(s,h);s.hand[h].machine=Machine{};}
    if(s.sim)for(unsigned h=0;h<2;++h)if(s.listed[h]){listRemove(s.sim,&objects[h].beh);s.listed[h]=false;}
}
inline bool insertBody(State& s,unsigned h,const float* m) noexcept {
    auto& hs=s.hand[h];const auto inst=instOf(h);
    // a delayed delete still holds the instance: wait; an index the level no longer holds is stale (a level reset): ours to clear
    const auto old=raw<std::uint16_t>(inst+off::instIndex);
    if(hs.pendingIndex!=off::noIndex){bump(stats.hand[h].insertWaits);return false;}
    if(old!=off::noIndex){if(inLevel(s.level,inst,old)){bump(stats.hand[h].insertWaits);return false;}put<std::uint16_t>(inst+off::instIndex,off::noIndex);}
    if(!ready.load(std::memory_order_acquire)||s.guardSim!=s.sim)return false;
    if(!userOk(h)){refuse(s,"USER DATA MISMATCH");return false;}
    std::memcpy(reinterpret_cast<void*>(inst+off::instMatrix),m,64);
    const int r=engine.addActive(reinterpret_cast<void*>(s.sim),reinterpret_cast<void*>(inst),true)&0xffff;
    if(raw<std::uintptr_t>(inst+off::instUser)!=addr(objects[h].user)){
        if(r!=off::noIndex&&inLevel(s.level,inst,std::uint16_t(r)))engine.deleteObject(reinterpret_cast<void*>(s.sim),r,false);
        refuse(s,"USER DATA MISMATCH");return false;}
    std::uint16_t index{};
    if(r==off::noIndex||!verifiedCollider(s.level,inst,index)||index!=std::uint16_t(r)){
        bump(stats.hand[h].insertFails);
        if(r!=off::noIndex&&inLevel(s.level,inst,std::uint16_t(r)))engine.deleteObject(reinterpret_cast<void*>(s.sim),r,false);
        return false;}
    slotSet(s.sim,inst,&objects[h].beh);
    hs.present=true;hs.ghost=false;hs.index=index;hs.insertStep=s.step;hs.haveCommand=false;hs.haveSolved=false;hs.prevMoverNs=0;hs.est=ContactEst{};
    bump(stats.hand[h].inserts);return true;
}
inline void respawnBody(State& s,unsigned h,const float* m) noexcept {
    const auto inst=instOf(h);std::uint16_t index{};
    if(!verifiedCollider(s.level,inst,index))return;
    engine.teleport(reinterpret_cast<void*>(s.sim),reinterpret_cast<void*>(inst),m,nullptr);
    auto& hs=s.hand[h];hs.ghost=false;hs.insertStep=s.step;hs.haveSolved=false;hs.haveCommand=false;hs.est=ContactEst{};
    bump(stats.hand[h].respawns);
}
// The tick (main thread, outside the step): the PreApplyImpacts functor of s.sim must be the game's (fn PreApplyImpacts_Game or our
// guard, no callee, the Iterator thunk) and PreComputeImpacts the default one; the fn word is then ours. UpdateNew copies the functor
// before each call, so the swap never meets a call in progress.
inline bool guardImpacts(State& s) noexcept {
    const auto b=engine.base;std::uintptr_t fn{},callee{},thunk{},pfn{},pcallee{},pthunk{};
    const auto a=off::simPreApply,c=off::simPreCompute;
    if(!get(s.sim+a,fn)||!get(s.sim+a+off::fnCallee,callee)||!get(s.sim+a+off::fnThunk,thunk)||
       !get(s.sim+c,pfn)||!get(s.sim+c+off::fnCallee,pcallee)||!get(s.sim+c+off::fnThunk,pthunk))return false;
    const bool pcOurs=pfn==preComputeWord();
    if(callee||thunk!=b+off::impactThunk||pcallee||pthunk!=b+off::impactThunk||(pfn!=b+off::defaultPreCompute&&!pcOurs))return false;
    if(fn!=guardWord()){
        if(fn!=b+off::preApplyGame)return false;
        put<std::uintptr_t>(s.sim+a,guardWord());bump(stats.guardInstalls);}
    s.guardSim=s.sim;
    // PLAYER SKIP: the + 0xc8 word ours while the lever is on (the default function forwarded), the default back when it is off
    const bool want=s.lev.grabPlayerSkip&&groupOk[GroupGrab]&&engine.preComputeDefault;
    if(want&&!pcOurs){put<std::uintptr_t>(s.sim+c,preComputeWord());bump(stats.playerSkip.installs);}
    else if(!want&&pcOurs){put<std::uintptr_t>(s.sim+c,b+off::defaultPreCompute);bump(stats.playerSkip.restores);}
    s.skipOurs=want;
    return true;
}
// The default PreComputeImpacts fn back on sim (only a word that is ours).
inline void releaseSkip(State& s,std::uintptr_t sim) noexcept {
    std::uintptr_t pfn{};if(get(sim+off::simPreCompute,pfn)&&pfn==preComputeWord()){put<std::uintptr_t>(sim+off::simPreCompute,engine.base+off::defaultPreCompute);bump(stats.playerSkip.restores);}
    s.skipOurs=false;clearSkip(s);}
// The game's fn back once neither instance is in the level (a delayed delete keeps the guard until the level lets it go).
// Another simulator is not touched: its word is forgotten. A word that is no longer ours is left as it is.
inline void releaseGuard(State& s,std::uintptr_t sim,std::uintptr_t level) noexcept {
    if(!s.guardSim)return;
    if(s.guardSim!=sim){s.guardSim=0;s.skipOurs=false;return;}
    releaseSkip(s,sim);
    std::uintptr_t fn{};if(!get(sim+off::simPreApply,fn))return;
    if(fn!=guardWord()){s.guardSim=0;return;}
    for(unsigned h=0;h<2;++h){const auto inst=instOf(h);
        if(inLevel(level,inst,raw<std::uint16_t>(inst+off::instIndex))||inLevel(level,inst,s.hand[h].pendingIndex))return;}
    put<std::uintptr_t>(sim+off::simPreApply,engine.base+off::preApplyGame);bump(stats.guardRestores);s.guardSim=0;
}
// The simulator (GOT -> variable -> object) and its level.
inline bool currentSim(std::uintptr_t& sim,std::uintptr_t& level) noexcept {
    std::uintptr_t simAddr{},levelAddr{},simLevel{};sim=0;level=0;
    if(!get(engine.base+off::simGot,simAddr)||simAddr!=engine.base+off::simVar||!get(simAddr,sim)||!sim||
       !get(engine.base+off::levelGot,levelAddr)||levelAddr!=engine.base+off::levelVar||!get(levelAddr,level)||!level)return false;
    return get(sim+off::simLevel,simLevel)&&simLevel==level;
}
inline void readDump(State& s,std::uint64_t now) noexcept {
    if(s.dumpNs&&now>=s.dumpNs&&now-s.dumpNs<1'000'000'000ull)return;
    s.dumpNs=now;Dump d{};const auto b=engine.base;
    const auto flag=[&](std::uintptr_t got,std::uintptr_t var,int& out){std::uintptr_t a{};std::uint8_t v{};if(get(b+got,a)&&a==b+var&&get(a,v))out=v;};
    const auto word=[&](std::uintptr_t got,std::uintptr_t var,int& out){std::uintptr_t a{};std::int32_t v{};if(get(b+got,a)&&a==b+var&&get(a,v))out=v;};
    flag(off::matchFlagsGot,off::matchFlags,d.matchFlagsEarly);flag(off::shouldFindGot,off::shouldFind,d.shouldFindImpacts);
    flag(off::fromArrayGot,off::fromArray,d.behaviourFromArray);flag(off::delaySapGot,off::delaySap,d.delaySap);
    word(off::stepCountGot,off::stepCount,d.stepCount);word(off::solverGot,off::solver,d.solverIterations);
    word(off::solverMinGot,off::solverMin,d.solverMin);word(off::solverMaxGot,off::solverMax,d.solverMax);
    flag(off::preComputeFlagGot,off::preComputeFlag,d.preComputeImpacts);flag(off::preApplyFlagGot,off::preApplyFlag,d.preApplyImpacts);
    flag(off::forceCcdGot,off::forceCcd,d.forceCcd);get(b+off::allowedPen,d.allowedPen);get(b+off::sepBias,d.sepBias);
    if(s.sim){get(s.sim+off::simBehCount,d.behCount);get(s.sim+off::simBehCap,d.behCap);get(s.sim+off::simPoolUsed,d.poolUsed);get(s.sim+off::simPoolCap,d.poolCap);}
    if(s.level){get(s.level+off::levelCount,d.levelCount);get(s.level+off::levelActive,d.levelActive);get(s.level+off::levelCap,d.levelCap);}
    if(s.built)for(unsigned h=0;h<2;++h)d.handFlags[h]=raw<std::uint16_t>(instOf(h)+off::instFlags);
    if(s.moverOk){
        std::uintptr_t arch{};const auto mi=s.moverInstAddr;
        if(get(mi+off::instArch,arch)&&arch){get(arch+off::archType,d.moverType);get(arch+off::archInclude,d.moverInclude);}
        get(mi+off::instIndex,d.moverIndex);get(mi+off::instFlags,d.moverInstFlags);
        std::uintptr_t objects{},w{};
        if(d.moverIndex!=off::noIndex&&s.level&&d.moverIndex<d.levelCount&&get(s.level+off::levelObjects,objects)&&objects&&
           get(objects+std::uintptr_t(d.moverIndex)*off::objStride+off::objInst,w)&&(w&~std::uintptr_t(0xf))==mi)d.moverState=int(w&3u);
        d.valid=true;
    }
    s.dump=d;
}

// ---- props (tick) ----
enum PropCheck : std::uint8_t {PropOk=0,PropNotInLevel,PropActive,PropNotInactive,PropDoor,PropClass,PropInfinite,PropOwn};
// A prop the hand may wake: a nav prop that is not a door or a dead body, inactive in the level at its index, not with infinite mass
// vs movers, not one of the player's own instances. centre / radius = its level bounding sphere.
inline PropCheck checkProp(const State& s,std::uintptr_t inst,V3& centre,float& radius) noexcept {
    std::uintptr_t vt{},fn{};
    if(!inst||!get(inst,vt)||!vt||!get(vt+0x10,fn))return PropClass;
    const bool nav=fn&&fn==engine.navClassFn,dead=fn&&fn==engine.deadClassFn;
    if(!nav&&!dead)return PropClass;
    std::uint8_t doorByte{};if(nav&&get(inst+off::navDoor,doorByte)&&doorByte==1)return PropDoor;
    for(int i=0;i<s.ownCount;++i)if(s.own[std::size_t(i)]==inst)return PropOwn;
    std::uintptr_t user{};std::uint8_t kind{0xff};std::uintptr_t actor{};
    if(s.actor&&get(inst+off::instUser,user)&&user&&get(user+off::userKind,kind)&&kind==0&&get(user+off::userActor,actor)&&actor==s.actor)return PropOwn;
    std::uint16_t index{},count{};std::uintptr_t objects{},word{};
    if(!s.level||!get(inst+off::instIndex,index)||index==off::noIndex||!get(s.level+off::levelCount,count)||index>=count||
       !get(s.level+off::levelObjects,objects)||!objects)return PropNotInLevel;
    const auto obj=objects+std::uintptr_t(index)*off::objStride;
    if(!get(obj+off::objInst,word)||(word&~std::uintptr_t(0xf))!=inst)return PropNotInLevel;
    float sphere[4]{};if(get(obj,sphere)){centre={sphere[0],sphere[1],sphere[2]};radius=sphere[3];}
    if((word&3u)==0)return PropActive;
    if((word&3u)!=1)return PropNotInactive;
    std::uint8_t inf{};if(!get(inst+off::fragInfinite,inf)||(inf&0x80u))return PropInfinite;
    return PropOk;
}
// A refused wake: the activation fields of that prop, 8 distinct instances (guarded reads). The type is resolved as
// IsActivationDisabled resolves it (+0x78 with both resident bits, else +0x98).
inline void noteRefused(State& s,std::uintptr_t inst,int stateAfter,bool reach,float radiusCm=-1.f) noexcept {
    RefusedSample* slot=nullptr;
    for(auto& r:s.refusedSamples){if(r.inst==inst)return;if(!r.inst&&!slot)slot=&r;}
    if(!slot)return;
    RefusedSample r{};r.inst=inst;r.stateAfter=stateAfter;r.reach=reach;
    get(inst+off::instFlags,r.flags12);get(inst+off::fragF80,r.f80);get(inst+off::fragOther1c0,r.w1c0);
    std::uintptr_t cache{},t78{},type{},arch{};std::uint32_t bits{};
    r.cache=get(inst+off::fragCache,cache)&&cache;
    if(get(inst+off::fragType78,t78)&&t78){if(get(t78+0xc,bits)&&(bits&0x30000000u)==0x30000000u)get(t78,type);}
    else get(inst+off::fragType98,type);
    if(type){get(type+off::fragTypeBits,r.typeBits);get(type+off::fragOther1c0,r.minMove);}
    if(get(inst+off::instArch,arch)&&arch){get(arch+off::archType,r.archType);get(arch+off::archInclude,r.archInclude);}
    r.radiusCm=radiusCm;
    *slot=r;
}
// sagWorldScript::ForceActivation, then the level state again: true = active now.
inline bool wakeProp(State& s,std::uintptr_t inst,std::uint64_t now,bool reach=false,float radiusCm=-1.f) noexcept {
    const auto t0=nowNs();
    engine.forceActivation(reinterpret_cast<void*>(inst));
    addNs(stats.props.wakeNsSum,stats.props.wakeNsMax,nowNs()-t0);bump(stats.props.wakes);
    s.wakes.note(inst,now);
    const int st=otherState(s.level,inst);
    if(st!=0)noteRefused(s,inst,st,reach,radiusCm);
    return st==0;
}
// The release inputs of a nav prop (guarded reads): +0x80, +0x12, the type by PrepareForActivation's rule, its bits and minMove.
inline bool releaseInputs(std::uintptr_t inst,ReleaseIn& r) noexcept {
    std::uintptr_t t78{},t98{},t78Type{},type{};std::uint32_t bits{};
    if(!get(inst+off::fragF80,r.f80)||!get(inst+off::instFlags,r.flags12)||!get(inst+off::fragType78,t78))return false;
    if(t78){if(!get(t78+0xc,bits)||!get(t78,t78Type))return false;}else if(!get(inst+off::fragType98,t98))return false;
    type=fragType(t78,bits,t78Type,t98);r.typeOk=type!=0;
    if(type&&(!get(type+off::fragTypeBits,r.typeBits)||!get(type+off::fragOther1c0,r.minMove)))return false;
    return true;
}
inline bool navInst(std::uintptr_t inst) noexcept {std::uintptr_t vt{},fn{};return inst&&get(inst,vt)&&vt&&get(vt+0x10,fn)&&fn==engine.navClassFn;}
// The palm's depth past the contact plane (metres; the doorContact formula).
inline float pressDepthM(const State& s,const HandState& hs) noexcept {
    if(!hs.haveSolved||!hs.haveCommand)return 0.f;
    return std::max(0.f,v::dot(v::sub(hs.solvedPos,hs.command),hs.est.n))/std::max(s.upm,1e-3f);
}
inline bool actOn(const State& s) noexcept {return s.lev.props&&groupOk[GroupAct]&&engine.forceActivation;}
// The frag type by PrepareForActivation's rule (0 when it does not resolve).
inline std::uintptr_t fragTypeOf(std::uintptr_t inst) noexcept {
    std::uintptr_t t78{},t98{},t78Type{};std::uint32_t bits{};
    if(!get(inst+off::fragType78,t78))return 0;
    if(t78){if(!get(t78+0xc,bits)||!get(t78,t78Type))return 0;}else if(!get(inst+off::fragType98,t98))return 0;
    return fragType(t78,bits,t78Type,t98);
}
// DIAG: the frag type's tune name (fragType +0xc0, a char*): up to 23 printable characters, else "?" (guarded reads).
inline void tuneName(std::uintptr_t type,char (&out)[24]) noexcept {
    std::memset(out,0,sizeof out);out[0]='?';
    std::uintptr_t p{};char t[24]{};
    if(!groupOk[GroupDiag]||!type||!get(type+off::typeTuneName,p)||!p||!engine.read(nullptr,p,t,sizeof t))return;
    unsigned n=0;for(;n<23&&t[n];++n)if(t[n]<0x20||t[n]>0x7e||t[n]=='"'||t[n]=='\\')return;
    if(!n)return;std::memcpy(out,t,n);out[n]=0;
}
// UNFREEZE inputs of a nav prop (guarded reads only; the sample gets every field read).
inline void unfreezeInputs(std::uintptr_t inst,UnfreezeIn& u,UnfreezeSample& x,std::uintptr_t& goh) noexcept {
    const auto b=engine.base;goh=0;x.inst=inst;
    get(inst+off::fragOther1c0,x.w1c0);
    const auto type=fragTypeOf(inst);u.typeOk=type!=0;
    if(type){get(type+off::fragTypeBits,x.typeBits);get(type+off::fragOther1c0,u.minMove);x.minMove=u.minMove;tuneName(type,x.tune);
        std::uintptr_t shared{},arch{};if(get(type+off::typeSharedArch,shared)&&get(inst+off::instArch,arch))x.sharedArch=shared&&shared==arch?1:0;}
    {std::uintptr_t cache{};if(get(inst+off::fragCache,cache))x.cached=cache?1:0;}
    std::uintptr_t ud{},udVt{},g{},back{};std::uint8_t kind{0xff},f0{};
    if(get(inst+off::instUser,ud)&&ud&&get(ud,udVt)){x.udVtRel=std::uint32_t(udVt-b);
        if(get(ud+off::udKind,kind))x.udKind=kind;
        if(udVt==b+off::propUdVtable+0x10&&kind==1&&get(ud+off::udParent,g)&&g&&get(g+off::gohInst,back)&&back==inst&&get(g+off::gohFlags,f0)){
            u.gohOk=true;u.gohF0=f0;x.gohF0=f0;goh=g;}}
    std::uintptr_t comp{},compVt{},slot{};std::uint8_t ctype{};
    if(type&&get(type+off::typeComposite,comp)&&comp&&get(comp+off::boundType,ctype)&&ctype==off::boundComposite&&get(comp,compVt)&&compVt&&
       get(compVt+off::boundCanActive,slot)&&slot==b+off::compCanActive){
        u.composite=true;get(comp+off::compBvh,u.compD4);x.compD4=u.compD4;std::uint16_t n{};get(comp+off::compPartCount,n);u.partCount=n;x.partCount=n;
        {float cr{-1.f};if(get(comp+off::boundRadius,cr)&&std::isfinite(cr)&&cr>0.f){u.compRadiusM=cr;x.compRadiusCm=cr*100.f;}}
        std::uintptr_t list{};std::array<std::uintptr_t,16> parts{};
        if(n>=1&&n<=16&&get(comp+off::compParts,list)&&list&&engine.read(nullptr,list,parts.data(),n*8u)){
            bool all=true;
            for(unsigned i=0;i<n;++i){if(!parts[i])continue;std::uintptr_t pv{},ps{};std::uint8_t pt{};
                if(i<4&&get(parts[i]+off::boundType,pt))x.partTypes[i]=pt;
                bool ok=get(parts[i],pv)&&pv&&get(pv+off::boundCanActive,ps);
                if(ok){ok=false;for(const auto f:off::partCanActive)ok=ok||ps==b+f;}
                all=all&&ok;}
            u.partsActive=all;}}
    std::uintptr_t arch{};if(get(inst+off::instArch,arch)&&arch){get(arch+off::archMass,u.massKg);get(arch+off::archType,x.archType);}
    x.massKg=u.massKg;
}
inline void noteUnfreezeSample(std::array<UnfreezeSample,8>& list,const UnfreezeSample& x) noexcept {
    for(auto& r:list){if(r.inst==x.inst)return;if(!r.inst){r=x;return;}}
}
inline bool unfreezeOn(const State& s) noexcept {return s.lev.props&&s.lev.unfreeze&&groupOk[GroupUnfreeze]&&groupOk[GroupAct]&&engine.forceActivation&&engine.isActDisabled&&engine.setDisableAct;}
inline std::uintptr_t activeCollider(const State& s,std::uintptr_t inst) noexcept;
inline CupTrace* cupTrace(State& s,std::uintptr_t inst,bool create) noexcept;
// A palm met an unfrozen prop again: its restore waits.
inline void touchUnfrozen(State& s,std::uintptr_t inst,std::uint64_t now) noexcept {
    for(auto& e:s.unfrozen)if(e.inst==inst){e.touchNs=now;if(e.wasActive)bump(stats.props.ufTouchesActive);}}
// The flag back on a prop the level still holds at its index whose gohProp still points at it (SetPropFixed(true) writes only that bit).
inline bool restoreUnfrozen(const State& s,const Unfrozen& e) noexcept {
    std::uintptr_t back{};std::uint16_t f12{};
    if(otherState(s.level,e.inst)<0||!get(e.goh+off::gohInst,back)||back!=e.inst||!get(e.inst+off::instFlags,f12))return false;
    put<std::uint16_t>(e.inst+off::instFlags,std::uint16_t(f12|0x8u));return true;
}
// The mass set on an active prop's own collider: SetInertia(col, kg, I * kg / m) as the grab sets the hand's (never the archetype, shared by
// the type); every activation copies the archetype's mass back into the collider (phCollider::Init 0x1c66550). True = written.
// SetInertia 0x1c66d80 writes only the mass and inertia (+0xa0) and their inverses (+0xb0); the momentum (+0x130, SetVelocityImp
// 0x1c654c0: velocity x mass) stays, and the next impulse sets the velocity from it (UpdateVelocityFromImpulseImp 0x1c659f8: +0x110 =
// momentum x inverse mass), so the speed would jump by old mass / new mass. The velocity and the angular velocity read before are set again
// at the new mass (SetVelocity / SetAngVelocity recompute both momenta).
inline bool setColliderMass(std::uintptr_t col,float kg) noexcept {
    alignas(16) float in[4]{};
    if(!col||!engine.setInertia||!(kg>0.f)||!engine.read(nullptr,col+off::colliderInertia,in,sizeof in)||!std::isfinite(in[3])||!(in[3]>0.f))return false;
    if(std::fabs(in[3]-kg)<=.02f*kg)return false;
    const float k=kg/in[3];alignas(16) const float out[4]{in[0]*k,in[1]*k,in[2]*k,0.f};
    if(!std::isfinite(out[0])||!std::isfinite(out[1])||!std::isfinite(out[2]))return false;
    alignas(16) float vl[4]{},w[4]{};
    const bool keep=engine.setVelocity&&engine.setAngVelocity&&engine.read(nullptr,col+off::colliderVel,vl,sizeof vl)&&engine.read(nullptr,col+off::colliderAngVel,w,sizeof w)&&
        std::isfinite(vl[0])&&std::isfinite(vl[1])&&std::isfinite(vl[2])&&std::isfinite(w[0])&&std::isfinite(w[1])&&std::isfinite(w[2]);
    engine.setInertia(reinterpret_cast<void*>(col),&kg,quad(out));
    if(keep){vl[3]=0.f;w[3]=0.f;engine.setVelocity(reinterpret_cast<void*>(col),vl);engine.setAngVelocity(reinterpret_cast<void*>(col),w);bump(stats.heldMass.velKept);}
    return true;
}
// Once per tick with entries: an active entry gets its collider mass back when a reactivation reset it and is measured (moved, tipped,
// stuck, back where it started); a prop asleep and untouched for a second gets its flag back; one the level no longer holds is forgotten.
// The +0x80 bits a furniture release wrote, put back (a read-modify-write of those bits only).
inline bool restoreMoved(const MovedProp& e) noexcept {
    std::uint32_t f80{};if(!get(e.inst+off::fragF80,f80))return false;
    put<std::uint32_t>(e.inst+off::fragF80,(f80&~movedBits)|(e.bits&movedBits));return true;}
// RESETHOLD (main thread): the list of props the hands made physical whose gringo reset the readiness hook holds back (16; published to
// heldGoh). An entry leaves when the level or its gohProp lets the prop go, when it is back home after it left (nothing to hold), when a
// stock reset the hook does not hold (a gringo deactivation, the end of a use, a script) ran on it, when the player is resetHoldFarM away
// (the next poll starts the stock graceful reset, out of view), for a nearer prop when the list is full, and all of them when the hold is
// off.
inline constexpr float resetHoldFarM=40.f;
inline bool resetHoldOn(const State& s) noexcept {return s.mode==ModeOn&&s.lev.resetHold&&unfreezeOn(s)&&groupOk[GroupResetHold];}
inline void publishHeld(const State& s) noexcept {for(std::size_t i=0;i<s.resetHold.size()&&i<heldGoh.size();++i)heldGoh[i].store(s.resetHold[i].goh,std::memory_order_relaxed);}
inline void resetHoldClear(State& s) noexcept {
    holdLive.store(false,std::memory_order_relaxed);
    bool any=false;for(auto& e:s.resetHold)if(e.goh){bump(resetStats.dropBy[DropOff]);e=HeldReset{};any=true;}
    if(any)publishHeld(s);}
inline void resetHoldAdd(State& s,std::uintptr_t goh,std::uintptr_t inst,std::uint64_t now) noexcept {
    if(!goh||!inst||!resetHoldOn(s))return;
    HeldReset* slot=nullptr;for(auto& e:s.resetHold)if(e.goh==goh){slot=&e;break;}
    if(!slot)for(auto& e:s.resetHold)if(!e.goh){slot=&e;break;}
    if(!slot){const V3 me=s.tg.moverPos;float far=-1.f;slot=&s.resetHold[0];
        for(auto& e:s.resetHold){float p[3]{};const float d=get(e.inst+off::instPos,p)?v::length(v::sub(V3{p[0],p[1],p[2]},me)):1e30f;if(d>far){far=d;slot=&e;}}
        bump(resetStats.dropBy[DropFull]);*slot=HeldReset{};}
    if(slot->goh!=goh){*slot=HeldReset{goh,inst,now?now:1,false};bump(resetStats.adds);}else slot->inst=inst;
    unsigned n=0;for(const auto& e:s.resetHold)n+=e.goh?1u:0u;maxOf(resetStats.maxSize,n);
    publishHeld(s);holdLive.store(true,std::memory_order_relaxed);}
inline void resetHoldTick(State& s) noexcept {
    const bool on=resetHoldOn(s);
    holdUpm.store(std::max(s.upm,1e-3f),std::memory_order_relaxed);
    if(!on){bool any=false;for(const auto& e:s.resetHold)any=any||e.goh;if(any||holdLive.load(std::memory_order_relaxed))resetHoldClear(s);return;}
    holdLive.store(true,std::memory_order_relaxed);
    bool changed=false;
    // the hooks' CommonResets of held props since the last tick: the cup's trace, the entry dropped (the stock reset won)
    {std::uint64_t newest=s.resetRingNs;
     for(auto& r:resetRing){const auto ns=r.ns.load(std::memory_order_acquire);if(!ns||ns<=s.resetRingNs)continue;
        const auto inst=r.inst.load(std::memory_order_relaxed);const auto info=r.info.load(std::memory_order_relaxed);
        if(r.ns.load(std::memory_order_acquire)!=ns||!inst)continue;
        newest=std::max(newest,ns);
        const ResetSeen x{inst,ns,std::uint8_t(info>>24),float(info&0xffffffu)/10.f};s.resetSeen[s.resetSeenNext]=x;s.resetSeenNext=(s.resetSeenNext+1u)%unsigned(s.resetSeen.size());
        if(CupTrace* c=cupTrace(s,inst,false)){c->resetBy=x.by;c->resetDeltaCm=x.cm;
            for(const auto& e:s.unfrozen)if(e.inst==inst&&e.clearNs&&ns>=e.clearNs)c->resetMsSinceClear=float(double(ns-e.clearNs)*1e-6);}
        for(auto& e:s.resetHold)if(e.goh&&e.inst==inst){bump(resetStats.dropBy[DropStock]);e=HeldReset{};changed=true;}}
     s.resetRingNs=newest;}
    // every 30 ticks: gone, home, far
    if((++s.resetCheckTick%30u)==0){const float u=std::max(s.upm,1e-3f);
        for(auto& e:s.resetHold){if(!e.goh)continue;
            std::uintptr_t back{};float p[3]{},r[3]{};
            if(otherState(s.level,e.inst)<0||!get(e.goh+off::gohInst,back)||back!=e.inst||!get(e.inst+off::instPos,p)||!get(e.inst+off::instResetPos,r)){
                bump(resetStats.dropBy[DropGone]);e=HeldReset{};changed=true;continue;}
            const V3 pos{p[0],p[1],p[2]};const float home=v::length(v::sub(pos,V3{r[0],r[1],r[2]}))/u;
            if(home>.01f)e.left=true;
            else{bool tracked=false;for(const auto& x:s.unfrozen)tracked=tracked||x.inst==e.inst;
                if(e.left||!tracked){bump(resetStats.dropBy[DropHome]);e=HeldReset{};changed=true;continue;}}
            if(s.tgUsable&&v::length(v::sub(pos,s.tg.moverPos))/u>resetHoldFarM){bump(resetStats.dropBy[DropFar]);e=HeldReset{};changed=true;}}}
    if(changed)publishHeld(s);
}
inline void unfreezeTick(State& s,std::uint64_t now,bool all) noexcept {
    auto& P=stats.props;
    for(auto& e:s.unfrozen){if(!e.inst)continue;
        const int st=otherState(s.level,e.inst);
        if(st<0){bump(P.ufDropped);e=Unfrozen{};continue;}
        if(st==0&&!all){
            if(!e.wasActive){e.wasActive=true;e.activeNs=now;}
            e.asleep=false;
            if(const auto col=activeCollider(s,e.inst)){float m{};std::int32_t type{-1};
                if(e.kgSet>0.f&&get(col+off::colliderType,type)&&type==0&&get(col+off::colliderMass,m)&&std::isfinite(m)&&std::fabs(m-e.kgSet)>.02f*e.kgSet&&setColliderMass(col,e.kgSet)){bump(P.ufMassReapplied);bump(P.ufReactivations);}}
            float p[3]{},a[3]{};
            if(get(e.inst+off::instPos,p)&&get(e.inst+off::instAxisY,a)){
                const float u=std::max(s.upm,1e-3f);const float moved=v::length(v::sub(V3{p[0],p[1],p[2]},e.pos0))/u;
                const float tilt=std::acos(std::clamp(v::dot(v::normalized(V3{a[0],a[1],a[2]}),v::normalized(e.axis0)),-1.f,1.f));
                if(std::isfinite(moved)){e.movedM=std::max(e.movedM,moved);if(!e.moved&&moved>=.02f){e.moved=true;bump(P.ufMoved2cm);
                        if(CupTrace* c=cupTrace(s,e.inst,false);c&&!c->moved){c->moved=true;c->firstMoveNs=now?now:1;const unsigned k=c->touchesToMove;
                            bump(P.ufTouchesToMove[k<=1?0u:k==2?1u:k==3?2u:3u]);
                            if(c->activeNs&&now>=c->activeNs){const auto ms=(now-c->activeNs)/1'000'000ull;bump(P.ufActToMoveMs[ms<50?0u:ms<150?1u:ms<500?2u:3u]);}}}
                    if(e.moved&&!e.snapped&&moved<.005f){e.snapped=true;bump(P.ufSnapBack);}}
                if(std::isfinite(tilt)){e.tiltRad=std::max(e.tiltRad,tilt);if(!e.tipped&&tilt>=pi/6.f){e.tipped=true;bump(P.ufTipped30);}}
                if(!e.stuck&&!e.moved&&now>=e.activeNs&&now-e.activeNs>=1'000'000'000ull&&now>=e.touchNs&&now-e.touchNs<1'000'000'000ull&&e.movedM<.005f){e.stuck=true;bump(P.ufStuck);}}}
        else if(st==1&&!all&&e.wasActive&&!e.asleep){ // asleep again (phSleep put it down)
            e.asleep=true;bump(P.ufSleeps);if(e.src==UfApproach&&!e.touchedSince)bump(P.ufApproachSlept);
            if(CupTrace* c=cupTrace(s,e.inst,false)){++c->sleeps;c->sleepMs=now>=e.activeNs?float(double(now-e.activeNs)*1e-6):-1.f;c->sleepMovedCm=e.movedM*100.f;}}
        // the flag back without our restore (GracefulPropReset, SetPropFixed, a script): counted once per clear
        if(!all&&!e.refrozen){std::uint16_t f12{};
            if(get(e.inst+off::instFlags,f12)&&(f12&0x8u)){e.refrozen=true;bump(P.ufRefrozen);
                {bool byReset=false;for(const auto& x:s.resetSeen)byReset=byReset||(x.inst==e.inst&&now>=x.ns&&now-x.ns<=100'000'000ull);
                 bump(resetStats.refrozenBy[byReset?0u:1u]);}
                if(CupTrace* c=cupTrace(s,e.inst,false)){++c->refrozen;c->refrozenMs=now>=e.clearNs?float(double(now-e.clearNs)*1e-6):-1.f;c->refrozenState=st;
                    float p[3]{};c->refrozenHome=get(e.inst+off::instPos,p)&&v::length(v::sub(V3{p[0],p[1],p[2]},e.pos0))<.005f*std::max(s.upm,1e-3f);}}}
        // the first activations' timelines
        if(!all)for(auto& l:s.cupLines){if(l.inst!=e.inst||l.n>=l.t.size())continue;
            CupTick k{};k.state=std::int8_t(st);float p[3]{};const float u=std::max(s.upm,1e-3f);
            if(get(e.inst+off::instPos,p)){const V3 q{p[0],p[1],p[2]};k.movedMm=std::int16_t(std::clamp(v::length(v::sub(q,e.pos0))/u*1000.f,0.f,32000.f));
                if(l.prevNs&&now>l.prevNs){const float mms=v::length(v::sub(q,l.prev))/u/float(double(now-l.prevNs)*1e-9)*1000.f;if(std::isfinite(mms))k.speedMms=std::int16_t(std::clamp(mms,0.f,32000.f));}
                l.prev=q;l.prevNs=now;}
            const auto& hs=s.hand[e.hand&1u];k.touched=e.touchNs==now?1:0;k.give=std::uint8_t(std::clamp(hs.yield.give*100.f,0.f,255.f));k.age=std::uint8_t(std::min(hs.est.age,255u));
            const auto cc=u64(stats.hand[e.hand&1u].creepClips);k.creep=std::uint8_t(std::min<std::uint64_t>(cc-std::min(cc,l.creep0),255u));l.creep0=cc;
            {const auto& t=s.tg.hand[e.hand&1u];k.squeeze=std::isfinite(t.squeeze)?std::uint8_t(std::clamp(t.squeeze*100.f,0.f,254.f)):255u;
             for(unsigned q=0;q<2;++q)if(s.grab[q].active&&s.grab[q].prop==e.inst)k.held=std::uint8_t(k.held|(1u<<q));
             if(t.valid){const float cms=v::length(t.vel)/u*100.f;if(std::isfinite(cms))k.handCms=std::int16_t(std::clamp(cms,0.f,32000.f));}}
            l.t[l.n++]=k;}
        if(!all&&!unfreezeRestoreDue(st,now,e.touchNs))continue;
        if(restoreUnfrozen(s,e))bump(P.ufRestored);else bump(P.ufDropped);
        e=Unfrozen{};}
    // released furniture: its +0x80 bits back once the level holds it inactive and no hand held it for a second
    for(auto& e:s.moved){if(!e.inst)continue;
        bool held=false;for(const auto& g:s.grab)held=held||(g.active&&g.prop==e.inst);if(held&&!all)continue;
        const int st=otherState(s.level,e.inst);
        if(st<0){bump(stats.grab.moveDropped);e=MovedProp{};continue;}
        if(!all&&!unfreezeRestoreDue(st,now,e.touchNs))continue;
        if(restoreMoved(e))bump(stats.grab.moveRestored);else bump(stats.grab.moveDropped);
        e=MovedProp{};}
}
// UNFREEZE diagnostics: the trace of a cup (create: a new one replaces a trace that never activated first, so a table under a resting palm
// never evicts a cup's trace; among equals the oldest contact).
inline CupTrace* cupTrace(State& s,std::uintptr_t inst,bool create) noexcept {
    if(!inst)return nullptr;
    CupTrace* old=nullptr;
    for(auto& c:s.cups){if(c.inst==inst)return &c;}
    if(!create)return nullptr;
    for(auto& c:s.cups){if(!c.inst){old=&c;break;}
        if(!old||(!c.firstActiveNs&&old->firstActiveNs)||(!c.firstActiveNs==!old->firstActiveNs&&c.lastContactNs<old->lastContactNs))old=&c;}
    *old=CupTrace{};old->inst=inst;std::uintptr_t arch{};if(get(inst+off::instArch,arch)&&arch)get(arch+off::archType,old->archType);
    return old;
}
// The first 3 cups touched after an activation get a timeline from that contact (a press: the activation; the approach: its first contact).
inline void startCupLine(State& s,unsigned h,std::uintptr_t inst,std::uint8_t src) noexcept {
    if(s.cupLineNext>=s.cupLines.size())return;
    for(const auto& l:s.cupLines)if(l.inst==inst)return;
    auto& l=s.cupLines[s.cupLineNext++];l=CupLine{};l.inst=inst;l.src=src;l.creep0=u64(stats.hand[h&1u].creepClips);}
// A palm's contact with a cup: an episode after 150 ms without one; the episodes before its first move.
inline void noteCupContact(State& s,std::uintptr_t inst,std::uint64_t now,bool create) noexcept {
    CupTrace* c=cupTrace(s,inst,create);if(!c)return;
    if(!c->lastContactNs||now<c->lastContactNs||now-c->lastContactNs>=150'000'000ull){++c->contacts;if(!c->moved)c->touchesToMove=c->contacts;}
    if(!c->firstContactNs)c->firstContactNs=now?now:1;
    c->lastContactNs=now?now:1;
    for(auto& e:s.unfrozen)if(e.inst==inst&&e.src==UfApproach&&!e.touchedSince){e.touchedSince=true;startCupLine(s,e.hand,inst,UfApproach);
        if(now>=e.clearNs&&now-e.clearNs<=500'000'000ull)bump(stats.props.ufApproachThenContact);}
}
// The contact speed of hand h toward a prop's centre (m/s, contactMps): the controller's closing speed over its last 8 ticks, the edge's
// approach within 4 substeps, the pair's first set velocity along the palm-to-centre line.
inline float handContactMps(const State& s,unsigned h,V3 centre,V3 pairVset) noexcept {
    const auto& hs=s.hand[h&1u];const float u=std::max(s.upm,1e-3f);
    if(!hs.haveSolved||!v::finite(centre))return 0.f;
    const V3 dir=v::normalized(v::sub(centre,hs.solvedPos));if(v::length(dir)<.5f)return 0.f;
    float closing=0.f,speed=0.f;
    for(const auto& w:hs.velRing){const float c=v::dot(w,dir)/u,l=v::length(w)/u;if(std::isfinite(c))closing=std::max(closing,c);if(std::isfinite(l))speed=std::max(speed,l);}
    const float edge=s.step>=hs.edgeStep&&s.step-hs.edgeStep<=4?hs.approach:std::numeric_limits<float>::quiet_NaN();
    const float pair=v::finite(pairVset)?v::dot(pairVset,dir)/u:std::numeric_limits<float>::quiet_NaN();
    return contactMps(closing,speed,edge,pair);
}
// The one-time velocity of a prop the palm met before it was active (ReleaseProp's SetVelocity / SetAngVelocity after ActivateObject,
// 0x2161058 / 0x216107c): unfreezeKickOf with the contact speed. True = set.
inline bool kickProp(State& s,unsigned h,std::uintptr_t inst,std::uintptr_t col,V3 centre,float mps,float propKg) noexcept {
    auto& hs=s.hand[h&1u];
    if(!s.lev.unfreezeKick||!engine.setVelocity||!hs.haveSolved||!col||!(mps>=unfreezeKickMinMps))return false;
    const Kick k=unfreezeKickOf(centre,hs.solvedPos,hs.solvedPos,hs.est.n,mps,s.lev.massKg,std::max(propKg,.05f),s.upm);
    if(!(k.mps>0.f)||!v::finite(k.vel))return false;
    alignas(16) const float vv[4]{k.vel[0],k.vel[1],k.vel[2],0.f},ww[4]{k.spin[0],k.spin[1],k.spin[2],0.f};
    engine.setVelocity(reinterpret_cast<void*>(col),vv);if(engine.setAngVelocity)engine.setAngVelocity(reinterpret_cast<void*>(col),ww);
    bump(stats.props.ufKicks);bump(stats.props.ufKickCmsSum,std::uint64_t(std::max(mps,0.f)*100.f));
    if(CupTrace* c=cupTrace(s,inst,false)){++c->kicks;c->contactMps=mps;c->kickMps=k.mps;}
    return true;
}
// A light prop the palm pushes: the drive into it starts at the push speed (the creep is for a static surface; this one is known to be
// dynamic and lighter than the hand).
inline void yieldToProp(HandState& hs) noexcept {
    if(!hs.haveSolved||v::length(hs.est.n)<.5f)return;
    const float d=v::dot(hs.est.n,hs.solvedPos);hs.yield=YieldTrack{};hs.yield.on=true;hs.yield.give=1.f;hs.yield.d0=d;hs.yield.prev=d;}
// The first activation of a cup's trace: the contact-to-activation histogram.
inline void noteCupActive(State& s,std::uintptr_t inst,std::uint64_t now,UfSource src) noexcept {
    CupTrace* c=cupTrace(s,inst,true);if(!c)return;
    ++c->by[src<UfSourceCount?src:0];c->activeNs=now?now:1;
    if(!c->firstActiveNs){c->firstActiveNs=now?now:1;
        if(c->firstContactNs&&now>=c->firstContactNs){const auto ms=(now-c->firstContactNs)/1'000'000ull;bump(stats.props.ufContactToActMs[ms<20?0u:ms<50?1u:ms<100?2u:3u]);}}
}
// A never-activate prop made physical: ReleaseProp's order (the flag cleared by a read-modify-write of +0x12, the lock lifted, +0x80 bit 1),
// then the existing wake; active: the mass set on its own collider. A press (the palm already met the fixed prop: the contact's impulse
// went into a fixed body) gives it the hand's contact speed once (kickProp) and the drive into it the push speed (yieldToProp); the
// approach source fires before any contact (no depth gate, no kick: the engine solves the first contact with the prop active). False =
// not made physical (counted by reason).
inline bool unfreezeProp(State& s,unsigned h,std::uintptr_t inst,float radiusW,std::uint64_t now,UfSource src=UfPress,V3 centreW={0,0,0},V3 pairVset={0,0,0},
                         bool noKick=false) noexcept {
    auto& P=stats.props;auto& hs=s.hand[h];const float um=std::max(s.upm,1e-3f);
    const bool press=src==UfPress;
    UnfreezeIn u{};std::uint16_t f12{};if(!get(inst+off::instFlags,f12))return false;
    u.flags12=f12;u.live=!press||hs.machine.state==BodyLive;u.age=press?hs.est.age:0u;u.depthM=press?pressDepthM(s,hs):1.f;u.radiusM=radiusW/um;
    u.approach=press?(s.step>=hs.edgeStep&&s.step-hs.edgeStep<=4?hs.approach:0.f):0.f;
    if(!(f12&0x8u))return false;
    bump(P.ufCandidates);bump(P.ufSource[src<UfSourceCount?src:0]);
    if(press){const float mm=u.depthM*1000.f;bump(P.ufDepthHist[!(mm>=1.f)?0u:mm<2.f?1u:mm<5.f?2u:3u]);}
    if(const unsigned sw=unfreezeShallowWhy(u)){bump(P.ufBy[UnfreezeShallow]);bump(P.ufShallowBy[std::min(sw,3u)-1u]);
        if(CupTrace* c=cupTrace(s,inst,false))++c->shallow;return false;}
    for(const auto& v:s.ufSeen)if(v.inst==inst){bump(P.ufCached);bump(P.ufBy[v.why<UnfreezeCount?v.why:0]);return false;}
    UnfreezeSample x{};std::uintptr_t goh{};unfreezeInputs(inst,u,x,goh);x.radiusCm=u.radiusM*100.f;x.source=std::uint8_t(src);x.approachMps=u.approach;
    if(u.compRadiusM>0.f){u.compRadiusM/=um;x.compRadiusCm=u.compRadiusM*100.f;}
    const UnfreezeWhy why=unfreezeWanted(u,s.lev.unfreezeRM,s.lev.unfreezeKg,s.lev.releaseDepthM,s.lev.unfreezeParts);x.why=std::uint8_t(why);
    bump(P.ufBy[why]);
    if(why!=UnfreezeOk){s.ufSeen[s.ufSeenNext]={inst,std::uint8_t(why)};s.ufSeenNext=(s.ufSeenNext+1u)%unsigned(s.ufSeen.size());noteUnfreezeSample(s.ufSamples,x);return false;}
    Unfrozen* slot=nullptr;for(auto& e:s.unfrozen)if(!e.inst||e.inst==inst){slot=&e;break;}
    if(!slot){bump(P.ufFull);return false;}
    std::uint16_t again{};if(!get(inst+off::instFlags,again)||again!=f12)return false;
    put<std::uint16_t>(inst+off::instFlags,std::uint16_t(f12&~0x8u));
    if(engine.isActDisabled(reinterpret_cast<const void*>(inst)))engine.setDisableAct(reinterpret_cast<void*>(inst),false);
    {std::uint32_t f80{};if(get(inst+off::fragF80,f80)&&!(f80&0x2u))put<std::uint32_t>(inst+off::fragF80,f80|0x2u);}
    const bool known=slot->inst==inst; // a tracked prop whose flag came back (engine or script): its entry and start kept
    Unfrozen e=known?*slot:Unfrozen{};e.inst=inst;e.goh=goh;e.touchNs=now;e.src=std::uint8_t(src);e.hand=std::uint8_t(h&1u);e.clearNs=now?now:1;
    e.clearStep=s.step;e.asleep=false;e.refrozen=false;e.touchedSince=press;e.wasActive=false;
    if(!known){float p[3]{},a[3]{0,1,0};get(inst+off::instPos,p);get(inst+off::instAxisY,a);e.pos0={p[0],p[1],p[2]};e.axis0={a[0],a[1],a[2]};}
    *slot=e;bump(P.ufDone);
    hs.wakeNs=now;const bool woke=wakeProp(s,inst,now,false,radiusW*100.f/um);
    bump(woke?P.ufActivated:P.ufRefusedAfter);bump(woke?P.activated:P.refused);
    if(woke){noteCupActive(s,inst,now,src);resetHoldAdd(s,goh,inst,now);
        if(press)startCupLine(s,h,inst,src);} // a press is its first contact
    if(woke)if(const auto col=activeCollider(s,inst)){
        std::int32_t type{-1};float m{-1.f};get(col+off::colliderType,type);
        if(get(col+off::colliderMass,m)&&std::isfinite(m))x.colKgBefore=m;
        const float kg=type==0?unfreezeMassKg(u.radiusM,s.lev.unfreezeSetKg,m):m;
        if(type==0&&kg<.98f*m&&setColliderMass(col,kg)){bump(P.ufMassSet);slot->kgSet=kg;x.kgSet=kg;}else x.kgSet=m;
        if(press&&noKick&&lightGhostKg(x.kgSet,s.lev.grabRotKg)){bump(stats.ghost.kicksSkipped);bump(stats.ghost.yieldsSkipped);} // LIGHT GHOST: a closing hand's press on a light prop
        else if(press){const V3 c=v::finite(centreW)&&v::length(centreW)>0.f?centreW:e.pos0;
            const float mps=handContactMps(s,h,c,pairVset);x.approachMps=mps;
            if(kickProp(s,h,inst,col,c,mps,x.kgSet))x.kickMps=std::min(mps*1.3f*s.lev.massKg/(s.lev.massKg+std::max(x.kgSet,.05f)),3.f);
            if(x.kgSet>0.f&&x.kgSet<s.lev.massKg)yieldToProp(hs);}}
    x.stateAfter=otherState(s.level,inst);noteUnfreezeSample(s.ufSamples,x);
    return true;
}
// A stock wake that made a prop active (8 distinct): its archetype type and mass, the level radius, its collider's mass.
inline void noteActivated(State& s,std::uintptr_t inst,float radiusW) noexcept {
    ActivatedSample* slot=nullptr;for(auto& a:s.activatedSamples){if(a.inst==inst)return;if(!a.inst&&!slot)slot=&a;}
    if(!slot)return;
    ActivatedSample a{};a.inst=inst;a.radiusCm=radiusW*100.f/std::max(s.upm,1e-3f);std::uintptr_t arch{};
    if(get(inst+off::instArch,arch)&&arch){get(arch+off::archType,a.archType);get(arch+off::archMass,a.archKg);}
    if(const auto col=activeCollider(s,inst))get(col+off::colliderMass,a.colKg);
    *slot=a;
}
// GLASS (tick): a hand's pending component hits, the fastest first, each refused in glassWhy order (guarded reads of the type, child,
// group and cache entry the engine's CollisionDamage indexes without a check; a frame component is notGlass and the next is judged), then
// for the first that passes (one call per hand and tick) the engine's own break: sagFragInst::CollisionDamage(inst, Collision, null)
// with the Collision the projectile path builds (0x2534034..0x2534234: position +0x0, normal +0x10 (toward the hand), +0x20..+0x33 left
// as the projectile leaves them, damage +0x34, component +0x38, element +0x3c, other +0x40 = null so the crack type is 0, the
// perpetrator guid +0x50 = the player (0 would make CollisionDamage report sm_CurMsgPerpGuid, the last shooter: 0x2873210..0x2873220),
// +0x54 = 0xff), only where the instance's vtable +0x218 holds that function and its damage is not delayed (IsFragDamageDelayed: the
// projectile then defers it through projManager::AddDeferredCollision, 0x25341f0..0x2534218); sm_CreateCount rising or the group's health
// falling = done. A pane touched slowly is tracked: its group's health falling without a call of ours counts as the stock contact path's
// damage.
inline void noteGlassSample(State& s,const GlassSample& x) noexcept {s.glassSamples[s.glassSampleNext]=x;s.glassSampleNext=(s.glassSampleNext+1u)%unsigned(s.glassSamples.size());}
inline std::uintptr_t slotActor(std::uint32_t guid) noexcept;
// BLOOD LIFE (main thread, the tick): the drop just made is read for 1.5 s (diagnostics; GroupBloodLife). Per tick, guarded reads only: the
// slot still holding the rule (else recycled: stop), the attach mode and pointer, the time line's events (at most 4; an event whose Draw at
// vtable +0x60 is ptxEventEmitter::Draw counts as an emitter, its element's two count words the largest kept), the world position (its
// distance from the eye at the first and the last read, the largest distance from the first), the fade and alpha. Nothing is written.
inline void bloodLifeArm(State& s,unsigned h,std::uintptr_t slot,std::uintptr_t rule,std::uintptr_t matrix,std::uint64_t now) noexcept {
    auto& e=s.bloodLife[s.bloodLifeNext];s.bloodLifeNext=(s.bloodLifeNext+1u)%unsigned(s.bloodLife.size());
    e=State::BloodLife{};e.set=true;e.hand=std::uint8_t(h&1u);e.slot=slot;e.rule=rule;e.matrix=matrix;e.armNs=now?now:1;e.untilNs=now+1'500'000'000ull;
    if(!groupOk[GroupBloodLife])e.stop=4;
}
inline void bloodLifeTick(State& s,std::uint64_t now) noexcept {
    if(!groupOk[GroupBloodLife])return;
    auto& G=stats.glass;const float um=std::max(s.upm,1e-3f);const auto b=engine.base;
    for(auto& e:s.bloodLife){if(!e.set||e.stop)continue;
        if(now>=e.untilNs){e.stop=1;continue;}
        bump(G.bloodLifeTicks);e.lastNs=now;
        std::uintptr_t r{};if(!get(e.slot+off::slotRule,r)){e.stop=3;bump(G.bloodLifeFails);continue;}
        if(r!=e.rule){e.stop=2;bump(G.bloodLifeRecycled);continue;}
        const auto inst=e.slot+off::slotInst;++e.reads;
        std::int32_t mode{-1};std::uintptr_t mtx{};if(get(inst+off::ptxMode,mode))e.mode=mode;
        e.attachOk=mode==2&&get(inst+off::ptxAttach,mtx)&&mtx==e.matrix;
        std::uint16_t n{};std::uintptr_t evs{},data{};
        if(get(e.rule+off::ruleEventCount,n)&&get(e.rule+off::ruleEvents,evs)&&evs&&get(inst+off::ptxData,data)&&data){
            e.events=std::max<std::uint32_t>(e.events,n);unsigned em=0;bool live=false;
            for(unsigned i=0;i<std::min<unsigned>(n,4u);++i){std::uintptr_t ev{},vt{},fn{};
                if(!get(evs+std::uintptr_t(i)*8u,ev)||!ev||!get(ev,vt)||!vt||!get(vt+off::eventDrawSlot,fn)||fn!=b+off::fnEmitterDraw)continue;
                ++em;std::uint32_t c[2]{};if(get(data+std::uintptr_t(i)*off::ptxElemStride+off::ptxElemCount,c)){e.maxCount=std::max({e.maxCount,c[0],c[1]});live=live||c[0]||c[1];}}
            e.emitters=std::max<std::uint32_t>(e.emitters,em);if(live)++e.liveReads;}
        std::uintptr_t wo{};float p[3]{};
        if(get(inst+off::ptxWorldObj,wo)&&wo&&get(wo+off::ptxWorldPos,p)){const V3 q{p[0],p[1],p[2]};
            if(v::finite(q)){if(!e.haveFirst){e.haveFirst=true;e.first=q;if(s.tgUsable)e.eyeFirstM=v::length(v::sub(s.tg.eye,q))/um;}
                if(s.tgUsable)e.eyeLastM=v::length(v::sub(s.tg.eye,q))/um;e.travelM=std::max(e.travelM,v::length(v::sub(q,e.first))/um);}}
        float fade{-1.f},alpha{-1.f};
        if(get(inst+off::ptxFade,fade)&&std::isfinite(fade))e.fadeMin=e.fadeMin<0.f?fade:std::min(e.fadeMin,fade);
        if(get(inst+off::ptxAlpha,alpha)&&std::isfinite(alpha))e.alpha=alpha;}
}
// GLASS BLOOD (main thread, the tick; a pane this hand's break call just broke): the engine's punch blood drop on the hand body's
// matrix (static mod memory: the pointer the effect keeps never dangles), placed just outside the hand's outline from the VR eye
// (BLOOD PLACE; without a usable eye or with lever bloodplace 0 at the contact relative to the hand at the hit). Only a hand with a body
// breaks glass this way (ClassBody: no weapon, no firearm support; a held knife or gun has none, GUN GLASS never plays blood), so a pistol
// or a bullet never gets here; a hand holding a prop is refused. No damage, message or AI event: TriggerPtFxMelee only creates the effect.
inline BloodWhy glassBlood(State& s,unsigned h,const GlassHit& hit,std::uint64_t now,bool broke=true) noexcept {
    auto& G=stats.glass;
    const auto refuse=[&](BloodWhy w){bump(G.bloodRefused[w]);return w;};
    if(!s.lev.blood||!groupOk[GroupBlood]||!engine.meleeFx||!engine.ruleByHash)return refuse(BloodOff);
    if(!broke)return refuse(BloodNoPane);
    if(s.grab[h&1u].active||s.seat[h&1u].on)return refuse(BloodHolding); // a hold, or a light prop being pulled into the palm
    ::rdrvr::vr::handblood::stamp(h); // the bare hand broke glass: its knuckles bleed (VrHandBlood.h), whatever the drop below does
    if(threadKey()!=s.mainKey)return refuse(BloodThread);
    const std::uint64_t cdNs=std::uint64_t(double(s.lev.bloodCdMs)*1e6);
    if(const auto w=s.bloodGate.allow(h,now,cdNs);w!=BloodOk)return refuse(w);
    const auto b=engine.base;
    {std::uintptr_t mgr{};std::uint8_t on{};if(!get(b+off::bloodMgrVar,mgr)||!mgr||!get(mgr+off::bloodEnabled,on)||!on)return refuse(BloodDisabled);}
    {std::uintptr_t comp{};std::int32_t race{};if(!s.actor||!get(s.actor+off::actorCreature,comp)||!comp||!get(comp+off::creatureRace,race))return refuse(BloodNoCreature);}
    // SKIN BLOOD: the game's bullet-wound call on the player (CreateFxOnBulletHit: RegisterHit(victim, comp, &pos, &dir, 1, b)) with the hand
    // component (VR hand 0 = Hand_Left 9 -> zone 2, 1 = Hand_Right 13 -> zone 3): a soak at the knuckles (the pane itself is gone and the
    // game has no blood-on-glass). The arm cylinder runs fingertip (v 0) -> shoulder (v 1) and AddHit drops a type-1 stain outside v 0..1,
    // so the contact is pulled to <= 6 cm from the hand body. Not PrickPlayerFinger: its fixed uv (0.25, 0.5) is a band at mid-arm.
    if(engine.bloodHit){std::uintptr_t mgr{};
        if(get(b+off::bloodMgrVar,mgr)&&mgr){const float um=std::max(s.upm,1e-3f);V3 d=v::sub(hit.pos,hit.hand);const float dl=v::length(d);
            if(dl>.06f*um)d=v::mul(d,.06f*um/dl);
            const V3 p=v::add(hit.hand,d);const float vl=v::length(hit.vel);const V3 c=vl>1e-4f?v::mul(hit.vel,.08f*um/vl):V3{0.f,0.f,0.f};
            alignas(16) float pp[4]{p[0],p[1],p[2],0.f},cc[4]{c[0],c[1],c[2],0.f};
            if(v::finite(p)&&v::finite(c)&&engine.bloodHit(reinterpret_cast<void*>(mgr),reinterpret_cast<const void*>(s.actor),(h&1u)?13:9,pp,cc,1,false))
                bump(G.bloodPricks);}}
    const auto ruleAddr=addr(engine.ruleByHash(reinterpret_cast<void*>(b+off::ptFxMgrVar),bloodPushFx));const bool rule=ruleAddr!=0;
    if(!rule)return refuse(BloodNoRule);
    const auto inst=instOf(h);float m[16]{};if(!inst||!get(inst+off::instMatrix,m))return refuse(BloodNoPose);
    const V3 ra{m[0],m[1],m[2]},rb{m[4],m[5],m[6]},rc{m[8],m[9],m[10]},at{m[12],m[13],m[14]};
    float cam[3]{},fov{};if(!get(b+off::camPosVar,cam)||!get(b+off::fovScaleVar,fov))return refuse(BloodNoPose);
    const V3 cp{cam[0],cam[1],cam[2]};if(!v::finite(at)||!v::finite(cp)||!std::isfinite(fov))return refuse(BloodNoPose);
    if(bloodCulled(cp,at,fov))return refuse(BloodFar);
    const BloodFrame f0=bloodFrame(ra,rb,rc,hit.pos,hit.hand,hit.n,hit.vel,s.upm);if(!f0.ok)return refuse(BloodNoPose);
    // BLOOD PLACE: the drop outside the hand's outline from the VR eye (the contact frame without a usable eye)
    const bool fist=s.hand[h&1u].fist;const float um=std::max(s.upm,1e-3f);const int thumb=handThumb(h,fist);
    BloodPlace bp{};if(s.lev.bloodPlace&&s.tgUsable)bp=bloodEyePlace(ra,rb,rc,at,s.tg.eye,s.tg.up,fist,s.lev.bloodMps,s.upm,thumb);
    if(s.lev.bloodPlace&&!bp.ok)bump(G.bloodEyeFallback);
    const BloodFrame f=bp.ok?bp.f:f0;
    alignas(16) float pos[4]{f.pos[0],f.pos[1],f.pos[2],0.f},nn[4]{f.n[0],f.n[1],f.n[2],0.f},dir[4]{f.dir[0],f.dir[1],f.dir[2],0.f};
    // BLOOD ROUTE: the rule's draw list (its byte +0x37d, read by InitInstance only) and the list heads before the call (guarded reads);
    // a list no pass draws -> the byte is 0 for this one call (the drop is created in list 0, the high-res list the eyes draw) and put back
    // right after it
    std::uintptr_t vt{},rm{},lists{};std::uint8_t list{},lowRes{};std::uint16_t count{};float cullR{-1.f},cullD{-1.f};
    const bool readOk=get(ruleAddr,vt)&&get(ruleAddr+off::ruleDrawList,list)&&get(b+off::ptFxMgrVar+off::ptFxLowRes,lowRes)&&get(b+off::rmPtfxVar,rm)&&rm&&
        get(rm+off::rmLists,lists)&&lists&&get(rm+off::rmListCount,count);
    get(ruleAddr+off::ruleCullR,cullR);get(ruleAddr+off::ruleCullD,cullD);
    const BloodRouteWhy rw=bloodRouteWhy(s.lev.bloodHires,groupOk[GroupBloodRoute],readOk,vt==b+off::ruleStdVtable,list,count,lowRes);
    const bool routed=rw==RouteGo;
    const unsigned listIdx=readOk?(routed?0u:ptfxListOf(list,count)):0u;
    std::uintptr_t listObj{},headBefore{};const bool haveHead=readOk&&get(lists+std::uintptr_t(listIdx)*8u,listObj)&&listObj&&get(listObj,headBefore);
    if(routed)put<std::uint8_t>(ruleAddr+off::ruleDrawList,0);
    const auto t0=nowNs();
    engine.meleeFx(reinterpret_cast<void*>(b+off::ptFxMeleeVar),reinterpret_cast<const void*>(s.actor),bloodPushFx,0u,reinterpret_cast<const void*>(inst+off::instMatrix),
                   pos,nn,dir);
    if(routed)put<std::uint8_t>(ruleAddr+off::ruleDrawList,list);
    maxOf(G.bloodCallNsMax,nowNs()-t0);bump(G.bloodCalls);s.bloodGate.note(h,now);
    if(routed)bump(G.bloodRouted);else bump(G.bloodRouteRefused[rw<RouteWhyCount?rw:RouteRead]);
    // the drop made: its slot at the head of the list it went to, holding this rule (GetEffectInst: slot +0x170 = rule); then watched
    std::int8_t made=-1;
    if(haveHead){std::uintptr_t head{},hr{};
        if(get(listObj,head)){made=head&&head!=headBefore&&get(head+off::slotRule,hr)&&hr==ruleAddr?1:0;
            if(made==1){bump(G.bloodMade);ptfxWatchArm(ptfxWatch,head+off::slotInst,now+1'500'000'000ull);bump(G.bloodWatches);
                bloodLifeArm(s,h,head,ruleAddr,inst+off::instMatrix,now);}else bump(G.bloodNotMade);}}
    auto& x=s.bloodSamples[s.bloodSampleNext];s.bloodSampleNext=(s.bloodSampleNext+1u)%unsigned(s.bloodSamples.size());
    x=State::BloodSample{};x.set=true;x.hand=std::uint8_t(h&1u);x.ns=now;x.pos=f.pos;x.n=f.n;x.dir=f.dir;x.rule=rule;
    x.camM=v::length(v::sub(cp,at))/std::max(s.upm,1e-3f);
    x.list=readOk?std::int16_t(list):std::int16_t(-1);x.vtOk=readOk&&vt==b+off::ruleStdVtable;x.lowRes=readOk?std::int16_t(lowRes):std::int16_t(-1);x.routed=routed;
    x.made=made;x.cullR=cullR;x.cullD=cullD;
    x.place=bp.ok?1u:s.lev.bloodPlace?2u:0u;
    if(s.tgUsable&&v::finite(s.tg.eye)){const V3 e=s.tg.eye;const V3 toEye=v::normalized(v::sub(e,at));
        const V3 pw=v::add(at,v::add(v::add(v::mul(ra,f.pos[0]),v::mul(rb,f.pos[1])),v::mul(rc,f.pos[2])));
        const V3 p0=v::add(at,v::add(v::add(v::mul(ra,f0.pos[0]),v::mul(rb,f0.pos[1])),v::mul(rc,f0.pos[2])));
        const V3 n0=v::add(v::add(v::mul(ra,f0.n[0]),v::mul(rb,f0.n[1])),v::mul(rc,f0.n[2]));
        x.eyeM=v::length(v::sub(e,pw))/um;x.side=std::int8_t(v::dot(rc,toEye)>=0.f?1:-1);
        x.facingN=v::dot(v::normalized(n0),toEye);x.facingDir=v::dot(v::normalized(f0.dir),toEye);
        x.occlOld=insideOutline(ra,rb,rc,at,e,p0,fist,s.upm,thumb)?1:0;x.occlNew=insideOutline(ra,rb,rc,at,e,pw,fist,s.upm,thumb)?1:0;}
    return BloodOk;
}
// The glass judge and break shared by the hand body and the gun (GUN GLASS): the checks in glassWhy order, then the engine's break.
// GlassUseHand = the hand body's path (the slow touch watched, a create cap retried for 100 ms, the sample ring, blood on a broken pane);
// GlassUseGunJudge = every check, the rate gate only peeked, no call (ok = would break); GlassUseGunBreak = the call, no blood, no ring.
// mps = the speed gate. called = the break was called; paneMade / broke as the hand path counts them.
enum GlassUse : std::uint8_t {GlassUseHand=0,GlassUseGunJudge,GlassUseGunBreak};
struct GlassVerdict {GlassWhy why{GlassOff};bool called{},broke{};};
inline GlassVerdict glassCore(State& s,unsigned h,const GlassHit& hit,std::uint64_t now,GlassUse use,float mps,std::array<Counter,GlassWhyCount>& refused) noexcept {
    auto& hs=s.hand[h&1u];auto& G=stats.glass;const bool hand=use==GlassUseHand;
    const auto refuse=[&](GlassWhy why){bump(refused[why]);GlassVerdict r{};r.why=why;return r;};
    if(!glassOn(s))return refuse(GlassOff);
    if(otherState(s.level,hit.inst)<0)return refuse(GlassGone);
    std::uintptr_t vt{},fn{};if(!get(hit.inst,vt)||!vt||!get(vt+off::vtCollisionDamage,fn)||fn!=engine.base+off::fnCollisionDamage)return refuse(GlassVt);
    const auto type=fragTypeOf(hit.inst);if(!type)return refuse(GlassTypeNull);
    std::uint8_t children{},groups{};std::uintptr_t childList{},groupList{},child{},group{};std::uint8_t gi{},flags{};
    if(!get(type+off::typeChildCount,children)||hit.comp<0||hit.comp>=int(children)||!get(type+off::typeChildren,childList)||!childList||
       !get(childList+std::uintptr_t(hit.comp)*8u,child)||!child||!get(child+off::childGroup,gi)||!get(type+off::typeGroupCount,groups)||gi>=groups||
       !get(type+off::typeGroups,groupList)||!groupList||!get(groupList+std::uintptr_t(gi)*8u,group)||!group)return refuse(GlassCompRange);
    GlassSample x{};x.set=true;x.cls=hit.cls;x.comp=hit.comp;x.group=gi;x.groups=groups;x.approach=hit.approach;
    {std::uintptr_t arch{};if(get(hit.inst+off::instArch,arch)&&arch)get(arch+off::archType,x.archType);}
    get(group+off::groupFlags,flags);x.flags53=flags;get(group+off::groupMinDamage,x.minDamage);get(group+off::groupFloor,x.floor);
    if(!(flags&0x2u))return refuse(GlassNotGlass);
    std::uintptr_t cache{},health{};float hv{};const bool haveHealth=get(hit.inst+off::fragCache,cache)&&cache&&get(cache+off::cacheHealth,health)&&health&&
        get(health+std::uintptr_t(gi)*4u,hv)&&std::isfinite(hv);
    x.cache=cache?1:0;x.healthBefore=haveHealth?hv:-1.f;
    if(haveHealth&&std::isfinite(x.floor)&&hv<=-x.floor){x.result=GlassDead;if(hand)noteGlassSample(s,x);return refuse(GlassDead);}
    // a slow touch is only watched (the stock path's own damage)
    if(!(hit.approach>=mps)){if(hand&&haveHealth){hs.glassPane=hit.inst;hs.glassGroup=gi;hs.glassHealth=hv;hs.glassPaneNs=now;}return refuse(GlassSlow);}
    if(!hit.edge)return refuse(GlassNotEdge);
    {std::uint32_t f80{};std::int16_t t1ba{};if(!get(hit.inst+off::fragF80,f80)||!get(type+off::fragTypeBits,t1ba)||fragDamageDelayed(f80,true,t1ba)){
        x.result=GlassDelayed;if(hand)noteGlassSample(s,x);return refuse(GlassDelayed);}}
    std::uint32_t player{};if(!s.actor||!get(s.actor+off::actorGuid,player)||!player||slotActor(player)!=s.actor)return refuse(GlassNoPlayer);
    std::int32_t created{};const bool countOk=get(engine.base+off::createCount,created);
    if(!countOk||created>=4){if(hand&&now>=hit.ns&&now-hit.ns<100'000'000ull)for(auto& g:hs.glass)if(!g.pending){g=hit;g.pending=true;break;} // retried for 100 ms
        return refuse(GlassCreateCap);}
    if(use==GlassUseGunJudge){if(!s.glassRate.peek(hit.inst,now))return refuse(GlassRateLimited);GlassVerdict r{};r.why=GlassOk;return r;}
    if(!s.glassRate.allow(hit.inst,now))return refuse(GlassRateLimited);
    alignas(16) unsigned char c[0x60]{};
    const float pos[4]{hit.pos[0],hit.pos[1],hit.pos[2],0.f},nn[4]{hit.n[0],hit.n[1],hit.n[2],0.f};const float dmg=s.lev.glassDmg;
    const std::int32_t comp=hit.comp,elem=std::max(hit.element,0);const std::uint8_t evt=0xff;
    std::memcpy(c,pos,16);std::memcpy(c+0x10,nn,16);std::memcpy(c+0x34,&dmg,4);
    std::memcpy(c+0x38,&comp,4);std::memcpy(c+0x3c,&elem,4);std::memcpy(c+0x50,&player,4);std::memcpy(c+0x54,&evt,1);
    GlassVerdict r{};r.why=GlassOk;r.called=true;
    if(hand){bump(G.requests);hs.glassPane=0;}
    engine.collisionDamage(reinterpret_cast<void*>(hit.inst),c,nullptr);
    std::int32_t after{};get(engine.base+off::createCount,after);
    if(haveHealth){float h2{};if(get(health+std::uintptr_t(gi)*4u,h2))x.healthAfter=h2;}
    // a pane made, or the group's health fell (bgGlassManager::UpdateBuffers may zero the count in between)
    if(after>created||(haveHealth&&std::isfinite(x.healthAfter)&&x.healthAfter<hv-1e-4f)){r.broke=true;
        if(hand){bump(G.done);x.result=GlassOk;
            glassBlood(s,h,hit,now,glassPaneBroke(after>created,haveHealth,x.healthAfter,x.floor));}} // blood only for a broken pane, not a crack
    else if(hand){bump(G.noShatter);x.result=GlassWhyCount;}
    if(hand)noteGlassSample(s,x);
    if(s.lev.haptic&&(hand||r.broke))haptics::pulse(h,.6f,40'000,now,haptics::SourceContact); // the gun buzzes only for a broken pane
    return r;
}
// One pending hit of the hand body: true = the break was called (no other hit of this hand this tick).
inline bool glassHit(State& s,unsigned h,const GlassHit& hit,std::uint64_t now) noexcept {
    if(!glassOn(s)||s.hand[h&1u].machine.state!=BodyLive){bump(stats.glass.refused[GlassOff]);return false;}
    return glassCore(s,h,hit,now,GlassUseHand,s.lev.glassMps,stats.glass.refused).called;
}
inline void glassTick(State& s,unsigned h,std::uint64_t now) noexcept {
    auto& hs=s.hand[h];auto& G=stats.glass;
    // the stock path: a tracked pane's group health fell since the last tick (no call of ours in between)
    if(hs.glassPane&&hs.glassGroup>=0){std::uintptr_t cache{},health{};float hv{};
        if(now>=hs.glassPaneNs&&now-hs.glassPaneNs<500'000'000ull&&get(hs.glassPane+off::fragCache,cache)&&cache&&get(cache+off::cacheHealth,health)&&health&&
           get(health+std::uintptr_t(hs.glassGroup)*4u,hv)&&std::isfinite(hv)){if(hv<hs.glassHealth-1e-4f)bump(G.stockDamage);hs.glassHealth=hv;}
        else hs.glassPane=0;}
    std::array<GlassHit,glassCands> pend{};unsigned n=0;
    for(auto& g:hs.glass)if(g.pending){pend[n++]=g;g.pending=false;}
    if(!n)return;
    std::sort(pend.begin(),pend.begin()+n,[](const GlassHit& a,const GlassHit& b){return a.approach>b.approach;});
    for(unsigned i=0;i<n;++i)if(glassHit(s,h,pend[i],now))break;
}
// Contact wakes: the props a kept pair met in the last 4 substeps, while the hand touched a surface in the last 4. The first contact with
// a prop the approach made physical (already active) gets the push speed and, when the solver met it while still fixed, the kick.
inline void contactProps(State& s,unsigned h,std::uint64_t now) noexcept {
    auto& hs=s.hand[h];auto& P=stats.props;
    // LIGHT GHOST: a closing (not fist, squeeze rising) hand gives a light prop (up to grabRotKg) no first-contact push or kick (the prop it
    // closes on stays put for the grab); heavier props get them as before
    const bool closing=lightGhostClosing(s.tg.hand[h].squeeze,hs.sqLag,hs.fist,s.lev.grabGhost);
    if(!actOn(s)||(hs.machine.state!=BodyLive&&hs.machine.state!=BodyTravel))return;
    if(hs.est.age>4)return;
    for(auto& c:hs.props){
        if(!c.inst||s.step<c.step||s.step-c.step>4)continue;
        const auto inst=c.inst;const V3 pairVset=c.vset;const auto contactStep=c.step;c.inst=0;bump(P.candidates);
        touchUnfrozen(s,inst,now);
        Unfrozen* tracked=nullptr;for(auto& e:s.unfrozen)if(e.inst==inst){tracked=&e;break;}
        // the first contact with a prop the approach made physical (judged before noteCupContact marks it touched)
        const FirstContact first=tracked?approachFirstContact(tracked->src==UfApproach,tracked->touchedSince,contactStep,tracked->clearStep):FirstContact{};
        std::uint16_t f12{};const bool never=navInst(inst)&&get(inst+off::instFlags,f12)&&(f12&0x8u);
        bool refused=false;for(const auto& v:s.ufSeen)refused=refused||v.inst==inst;
        noteCupContact(s,inst,now,tracked||(never&&!refused));
        V3 centre{};float radius{};const PropCheck pc=checkProp(s,inst,centre,radius);
        if(pc==PropActive){bump(P.alreadyActive);
            const bool closingLight=closing&&tracked&&lightGhostKg(tracked->kgSet>0.f?tracked->kgSet:s.lev.unfreezeSetKg,s.lev.grabRotKg);
            if(first.yield&&tracked->kgSet>0.f&&tracked->kgSet<s.lev.massKg){if(closingLight)bump(stats.ghost.yieldsSkipped);else{yieldToProp(hs);bump(P.ufFirstYield);}}
            if(first.kick&&closingLight)bump(stats.ghost.kicksSkipped);
            else if(first.kick){const float mps=handContactMps(s,h,centre,pairVset);
                if(!(mps>=unfreezeKickMinMps))bump(P.ufFirstKickLow);
                else if(const auto col=activeCollider(s,inst))if(kickProp(s,h,inst,col,centre,mps,tracked->kgSet>0.f?tracked->kgSet:s.lev.unfreezeSetKg))bump(P.ufFirstKick);}
            continue;}
        if(pc==PropInfinite){bump(P.infinite);continue;}
        if(pc!=PropOk){bump(P.skipped);continue;}
        // UNFREEZE: a tracked light prop that fell asleep (its flag still clear) is woken at once, its mass and the contact's speed
        // given again: neither the wake LRU nor the hand's gap holds it (both are for props the hand only brushes)
        if(tracked&&tracked->wasActive&&!never&&unfreezeOn(s)){bump(P.ufRewakes);if(CupTrace* t=cupTrace(s,inst,false))++t->touchedAsleep;
            hs.wakeNs=now;const bool woke=wakeProp(s,inst,now,false,radius*100.f/std::max(s.upm,1e-3f));bump(woke?P.activated:P.refused);
            if(woke){noteCupActive(s,inst,now,UfRewake);bump(P.ufSource[UfRewake]);tracked->asleep=false;
                if(const auto col=activeCollider(s,inst)){std::int32_t type{-1};
                    if(tracked->kgSet>0.f&&get(col+off::colliderType,type)&&type==0&&setColliderMass(col,tracked->kgSet))bump(P.ufMassReapplied);
                    if(closing&&lightGhostKg(tracked->kgSet>0.f?tracked->kgSet:s.lev.unfreezeSetKg,s.lev.grabRotKg)){bump(stats.ghost.kicksSkipped);bump(stats.ghost.yieldsSkipped);}
                    else{kickProp(s,h,inst,col,centre,handContactMps(s,h,centre,pairVset),tracked->kgSet>0.f?tracked->kgSet:s.lev.unfreezeSetKg);
                        if(tracked->kgSet>0.f&&tracked->kgSet<s.lev.massKg)yieldToProp(hs);}}}
            continue;}
        if(s.wakes.recent(inst,now)){bump(P.gated);bump(P.gatedLru);if(CupTrace* t=cupTrace(s,inst,false))++t->gatedLru;continue;}
        if(hs.wakeNs&&now>=hs.wakeNs&&now-hs.wakeNs<handWakeGapNs){bump(P.gated);bump(P.gatedHand);if(CupTrace* t=cupTrace(s,inst,false))++t->gatedHand;continue;}
        // UNFREEZE: a never-activate prop (+0x12 bit 3) refuses every wake; only a press that makes it physical wakes it
        if(never){if(unfreezeOn(s))unfreezeProp(s,h,inst,radius,now,UfPress,centre,pairVset,closing);else bump(P.skipped);continue;}
        // RELEASE: a nav prop whose root is still fixed (+0x80 bit 1 clear) is woken only once released; otherwise its wake is wasted
        bool released=false;
        if(navInst(inst)&&s.lev.release&&groupOk[GroupRelease]){
            ReleaseIn r{};
            if(releaseInputs(inst,r)){
                r.radius=radius/std::max(s.upm,1e-3f);r.depth=pressDepthM(s,hs);r.live=hs.machine.state==BodyLive;r.age=hs.est.age;
                const ReleaseWhy why=releaseWanted(r,s.lev.releaseRM,s.lev.releaseDepthM);
                if(why!=ReleaseNotNeeded){bump(P.relCandidates);bump(P.relBy[why]);}
                if(why==ReleaseNeverMove){bool seen=false;for(const auto& x:s.neverMoveSamples)seen=seen||x.inst==inst;
                    if(!seen){UnfreezeSample x{};UnfreezeIn u{};std::uintptr_t g{};unfreezeInputs(inst,u,x,g);x.radiusCm=radius*100.f/std::max(s.upm,1e-3f);x.compRadiusCm/=std::max(s.upm,1e-3f);
                        noteUnfreezeSample(s.neverMoveSamples,x);}}
                if(why==ReleaseOk){
                    std::uint32_t f80{};
                    if(get(inst+off::fragF80,f80)&&f80==r.f80){put<std::uint32_t>(inst+off::fragF80,f80|0x2u);released=true;bump(P.released);} // BreakApart's RMW
                }else if(why!=ReleaseNotNeeded){bump(P.skipRoot);continue;}
            }
        }
        hs.wakeNs=now;
        const bool woke=wakeProp(s,inst,now,false,radius*100.f/std::max(s.upm,1e-3f));
        bump(woke?P.activated:P.refused);
        if(woke)noteActivated(s,inst,radius);
        if(released)bump(woke?P.relActivated:P.relRefusedAfter);
    }
}
// UNFREEZE (approach): a never-activate prop the reach query found inside a live palm's reach capsule is judged once (unfreezePropWhy;
// a pass is kept in ufOk, a refusal in ufSeen; props outside every capsule are not judged) and, passing, armed on each palm whose capsule
// holds it (4 per hand; when full, the one farthest outside the capsule gives way to a nearer one: nearSlot).
inline void armNear(State& s,std::uintptr_t inst,V3 centre,float radius,const std::array<ReachSphere,2>& sp,unsigned live,std::uint64_t now) noexcept {
    std::array<float,2> gap{1e30f,1e30f};unsigned in=0;
    for(unsigned h=0;h<2;++h)if(live>>h&1u){gap[h]=reachGap(sp[h],centre,radius);if(gap[h]<=0.f)in|=1u<<h;}
    if(!in)return;
    for(const auto& v:s.ufSeen)if(v.inst==inst)return;
    bool ok=false;for(const auto x:s.ufOk)ok=ok||x==inst;
    if(!ok){const float um=std::max(s.upm,1e-3f);UnfreezeIn u{};UnfreezeSample x{};std::uintptr_t goh{};unfreezeInputs(inst,u,x,goh);
        u.radiusM=radius/um;if(u.compRadiusM>0.f)u.compRadiusM/=um;
        const UnfreezeWhy why=unfreezePropWhy(u,s.lev.unfreezeRM,s.lev.unfreezeKg,s.lev.unfreezeParts);
        if(why!=UnfreezeOk){s.ufSeen[s.ufSeenNext]={inst,std::uint8_t(why)};s.ufSeenNext=(s.ufSeenNext+1u)%unsigned(s.ufSeen.size());return;}
        s.ufOk[s.ufOkNext]=inst;s.ufOkNext=(s.ufOkNext+1u)%unsigned(s.ufOk.size());}
    for(unsigned h=0;h<2;++h){if(!(in>>h&1u))continue;
        auto& hs=s.hand[h];const int i=nearSlot(hs.ufNear,inst,gap[h]);
        if(i<0){bump(stats.props.ufNearFull);continue;}
        auto& n=hs.ufNear[unsigned(i)];if(n.inst!=inst){bump(stats.props.ufNearArmed);n=HandState::NearProp{};n.inst=inst;}
        n.armedNs=now?now:1;n.centre=centre;n.radius=radius;n.gap=gap[h];}
}
// Each tick, before the contact wakes: an armed prop is fired (unfreezeProp, source approach) once the palm closes on it within the lead
// (nearFire on the level sphere kept at the arming, the gap from the palm's reach along the line to its centre: nearReachM; the palm's
// world velocity along that line); only then is it read again (checkProp, the flag). An entry is kept while the palm rests within
// nearKeepM of it and dropped 1.5 s after its last arming or keep. No reads while it waits.
inline void approachTick(State& s,unsigned h,std::uint64_t now) noexcept {
    auto& hs=s.hand[h];const auto& t=s.tg.hand[h];
    if(!s.lev.unfreezeNear||!unfreezeOn(s))return;
    bool boxes=false;HandBoxes hb{};
    for(auto& n:hs.ufNear){if(!n.inst)continue;
        if(now<n.armedNs||now-n.armedNs>1'500'000'000ull){n={};continue;}
        if(hs.machine.state!=BodyLive||!hs.haveSolved||!t.valid)continue;
        const float u=std::max(s.upm,1e-3f);const V3 d=v::sub(n.centre,hs.solvedPos);const float dist=v::length(d);
        if(!(dist>1e-5f))continue;
        const V3 dn=v::mul(d,1.f/dist);
        if(!boxes){hb=handBoxes(hs.solvedPos,hs.solvedRows,hs.fist,s.upm);boxes=true;}
        const float reach=nearReachM(hb,hs.solvedPos,dn,s.upm);
        const float gap=(dist-n.radius)/u-reach,closing=v::dot(v::add(t.vel,hs.moverVel),dn)/u;
        if(gap<nearKeepM){n.armedNs=now?now:1;bump(stats.props.ufNearKept);}
        if(!nearFire(gap,closing,s.lev.unfreezeAheadS))continue;
        bump(stats.props.ufNearReach[reach<=nearPalmM+1e-4f?0u:reach<.09f?1u:reach<.12f?2u:3u]);
        const auto inst=n.inst;n={};
        V3 centre{};float radius{};const PropCheck pc=checkProp(s,inst,centre,radius);
        std::uint16_t f12{};if(pc!=PropOk||!get(inst+off::instFlags,f12)||!(f12&0x8u))continue;
        if(unfreezeProp(s,h,inst,radius,now,UfApproach,centre))bump(stats.props.ufApproachFired);}
}
// REACH WAKE (main thread, outside the step): every reachEveryTicks ticks, while a body is LIVE, the engine's prop cull round each live
// palm's reach capsule; the nearest inactive props in reach are woken (at most wakeMax per query, each at most once per second).
inline void reachWake(State& s,std::uint64_t now,std::uint64_t tick) noexcept {
    auto& R=stats.reach;
    if(!s.lev.reachWake||!actOn(s)||!groupOk[GroupReach]||!engine.propCull||s.mode!=ModeOn||!s.tgUsable)return;
    std::array<ReachSphere,2> sp{};unsigned live=0;
    const V3 chest=chestPoint(s.tg.eye,s.tg.up,s.upm);
    for(unsigned h=0;h<2;++h){const auto& hs=s.hand[h];const auto& t=s.tg.hand[h];
        if(!hs.present||hs.machine.state!=BodyLive||!hs.haveSolved||!t.valid||t.cls!=ClassBody)continue;
        sp[h]=reachSphere(hs.solvedPos,v::add(t.vel,hs.moverVel),chest,s.lev.reachM,s.lev.reachLeadS,s.upm);if(sp[h].ok)live|=1u<<h;}
    if(!live)return;
    if(s.reachTick&&tick>=s.reachTick&&tick-s.reachTick<reachEveryTicks)return;
    s.reachTick=tick?tick:1;
    {bool moved=false;
     for(unsigned h=0;h<2;++h)if(live>>h&1u)moved=moved||!(s.reachLive>>h&1u)||!(v::length(v::sub(s.hand[h].solvedPos,s.reachPalm[h]))<reachMoveM*s.upm);
     if(!moved){bump(R.still);return;}
     for(unsigned h=0;h<2;++h)s.reachPalm[h]=s.hand[h].solvedPos;s.reachLive=std::uint8_t(live);}
    bump(R.queries);
    const auto t0=nowNs();
    struct Found {std::uintptr_t inst{};float gap{},radius{};};
    std::array<Found,16> list{};unsigned n=0;
    std::array<std::uintptr_t,64> seen{};unsigned ns=0;
    for(unsigned h=0;h<2;++h){
        if(!(live>>h&1u))continue;
        put<std::int32_t>(addr(reachIter)+off::iterCount,0);
        alignas(16) float c[4]{sp[h].centre[0],sp[h].centre[1],sp[h].centre[2],0.f};
        engine.propCull(reachIter,c,sp[h].cullR);bump(R.cullCalls);
        const int count=std::clamp(raw<std::int32_t>(addr(reachIter)+off::iterCount),0,off::iterMax);
        for(int k=0;k<count;++k){
            const auto inst=raw<std::uintptr_t>(addr(reachIter)+std::size_t(k)*8u);
            bool dup=false;for(unsigned i=0;i<ns;++i)if(seen[i]==inst){dup=true;break;}
            if(dup)continue;
            if(ns<seen.size())seen[ns++]=inst;
            bump(R.found);
            V3 centre{};float radius{};const PropCheck pc=checkProp(s,inst,centre,radius);
            if(pc==PropActive){bump(R.already);continue;}
            if(pc==PropInfinite){bump(R.infinite);continue;}
            if(pc!=PropOk){bump(R.skipped);
                bump(pc==PropDoor?R.skipDoor:pc==PropOwn?R.skipOwn:pc==PropNotInLevel?R.skipNotInLevel:pc==PropNotInactive?R.skipNotInactive:R.skipClass);continue;}
            // a never-activate prop (+0x12 bit 3): ForceActivation would refuse it (PrepareForActivation null); a palm closing on it
            // makes it physical ahead of the contact (armNear: UNFREEZE's gates, then approachTick), else a press frees it
            {std::uint16_t f12{};if(get(inst+off::instFlags,f12)&&(f12&0x8u)){bump(R.skipped);bump(R.skipNever);
                if(navInst(inst)&&unfreezeOn(s)&&s.lev.unfreezeNear)armNear(s,inst,centre,radius,sp,live,now);continue;}}
            // a nav prop whose root is fixed (+0x80 bit 1 clear): ForceActivation would refuse it; only a palm's press releases it
            {std::uint32_t f80{};if(navInst(inst)&&get(inst+off::fragF80,f80)&&!(f80&0x2u)){bump(R.skipped);bump(R.skipRoot);continue;}}
            const float gap=std::min(reachGap(sp[0],centre,radius),reachGap(sp[1],centre,radius));
            if(!(gap<=0.f)){bump(R.outOfReach);continue;}
            if(s.wakes.recent(inst,now)){bump(R.lruHits);continue;}
            if(n<list.size())list[n++]={inst,gap,radius};else bump(R.rateLimited);
        }
    }
    std::sort(list.begin(),list.begin()+n,[](const Found& a,const Found& b){return a.gap<b.gap;});
    addNs(R.queryNsSum,R.queryNsMax,nowNs()-t0);
    for(unsigned i=0;i<n;++i){
        if(i>=s.lev.wakeMax){bump(R.rateLimited);continue;}
        bump(wakeProp(s,list[i].inst,now,true,list[i].radius*100.f/std::max(s.upm,1e-3f))?R.woken:R.refused);
    }
}

// ---- GUN GLASS (tick, main thread) ----
// A hand with a placed pistol has no body, so it never meets a pane in the step. Instead, on ticks where the muzzle moves fast (its
// tracking-space speed, not the stick or a turn), one level probe sweeps a sphere of gunGlassR from the muzzle's last place to now
// (SweptSphereMultipleExcludeInsts include 1, state 7: VrMelee's wall test), and only if that met no glass one more along the barrel
// (the bore point level with the grip to the muzzle, r 3 cm). The empty hand while a firearm is held (lever gunGlassFree) sweeps its palm
// (r 4 cm). Hits: our own, the mover's and the player's own instances excluded (gunExcludes), a sagFragInst / fragInstNav judged by the hand path's glass checks
// (glassCore: frag vtable +0x218 = CollisionDamage, the component's group with the glass bit, live, the speed along the normal turned
// toward the grip, an edge per pane, delayed damage, the player, the create cap and the pane rate); lever 0 counts would-breaks, 1 calls
// the break (the Collision the projectile builds, the element from isect +0x64). Never blood. Nothing else is written.
inline bool gunLevelOk(const State& s) noexcept {
    std::uintptr_t a{},l{};return s.level&&get(engine.base+off::levelGot,a)&&a==engine.base+off::levelVar&&get(a,l)&&l==s.level;}
// The probe's exclusions: the mover, our hand bodies, then the player's own instances (s.own: his ConfigureActorExcludeInsts list through
// the ARC ledger, the frag, ragdoll, inventory items such as the held gun, the mount; refreshed every 16 ticks and on a draw), no repeats.
// The shape test copies at most 64 (phShapeTest<phShapeCapsule>::SetExcludeInstanceList 0x1c9c734); the entries are only compared.
inline constexpr std::size_t gunExclMax=24;
inline int gunExcludes(const State& s,std::array<const void*,gunExclMax>& ex) noexcept {
    int n=0;
    const auto add=[&](std::uintptr_t p){if(!p||n>=int(ex.size()))return;for(int i=0;i<n;++i)if(addr(ex[std::size_t(i)])==p)return;ex[std::size_t(n++)]=reinterpret_cast<const void*>(p);};
    if(s.moverOk)add(s.moverInstAddr);
    for(unsigned k=0;k<2;++k)if(s.hand[k].present)add(instOf(k));
    for(int i=0;i<s.ownCount&&i<int(s.own.size());++i)add(s.own[std::size_t(i)]);
    return n;
}
inline bool gunOwnInst(const State& s,std::uintptr_t inst) noexcept {
    if((s.moverInstAddr&&inst==s.moverInstAddr)||inst==instOf(0)||inst==instOf(1))return true;
    for(int i=0;i<s.ownCount&&i<int(s.own.size());++i)if(s.own[std::size_t(i)]==inst)return true;
    return false;
}
inline int gunProbe(State& s,V3 a,V3 b,float r,unsigned char* isect,unsigned kind) noexcept {
    auto& GG=stats.gun;
    alignas(16) float seg[8]{a[0],a[1],a[2],0.f,b[0],b[1],b[2],0.f};
    std::memset(isect,0,4u*off::isectSize);
    for(unsigned i=0;i<4u;++i){const std::uint32_t mark=0xffffu;std::memcpy(isect+i*off::isectSize+off::isectLevel,&mark,4);std::memcpy(isect+i*off::isectSize+off::isectPart,&mark,4);}
    std::array<const void*,gunExclMax> ex{};const int nex=gunExcludes(s,ex);
    bump(s.ownCount>0?GG.exclOwn:GG.exclNoOwn);maxOf(GG.exclMax,std::uint64_t(nex));
    bump(GG.probes[kind<3?kind:0]);const auto t0=nowNs();int n=0;
    try{n=engine.probeLevel(seg,r,4,isect,nex,ex.data(),1,0,7);}catch(...){bump(GG.exceptions);n=0;}
    const auto d=nowNs()-t0;GG.probeNsSum.fetch_add(d,std::memory_order_relaxed);maxOf(GG.probeNsMax,d);
    if(n>=4)bump(GG.maxHitsFull);
    return std::clamp(n,0,4);
}
// The hits of one probe judged (the first glass verdict ends it). True = a pane was judged (glass), so the barrel probe is skipped.
inline bool gunHits(State& s,unsigned h,const unsigned char* isect,int n,unsigned src,V3 vel,V3 toward,V3 from,int lever,std::uint64_t now,std::int8_t kind) noexcept {
    auto& GG=stats.gun;auto& m=s.gunMem[h&1u];const float u=std::max(s.upm,1e-3f);bool glass=false;
    for(int i=0;i<n;++i){const ProbeHit ph=parseIsect(isect+std::size_t(i)*off::isectSize);
        if(!ph.inst){bump(GG.hits[3]);continue;}
        if(gunOwnInst(s,ph.inst)){bump(GG.hits[2]);continue;}
        std::uintptr_t vt{},cf{};if(!get(ph.inst,vt)||!vt||!get(vt+0x10,cf)){bump(GG.hits[3]);continue;}
        const int cls=cf==engine.fragClassFn?5:cf==engine.navClassFn?classNav:-1;if(cls<0){bump(GG.hits[3]);continue;}
        bump(GG.hits[cls==5?1u:0u]);
        GlassHit gh{};gh.inst=ph.inst;gh.cls=cls;gh.comp=ph.comp;gh.element=ph.part<0?0:ph.part;gh.pos=ph.pos;gh.ns=now;gh.hand=from;gh.vel=vel;
        if(!v::finite(ph.pos)){bump(GG.hits[3]);continue;}
        gh.approach=gunApproach(vel,ph.pos,ph.n,toward,s.upm,gh.n);if(v::length(gh.n)<.5f){bump(GG.hits[3]);continue;}
        gh.edge=m.tracks.edge(gh.inst,gh.comp,s.gunStep,now,gh.approach,s.lev.gunGlassMps);
        const GlassVerdict r=glassCore(s,h,gh,now,lever>=1?GlassUseGunBreak:GlassUseGunJudge,s.lev.gunGlassMps,GG.judged);
        const bool isGlass=r.why!=GlassGone&&r.why!=GlassVt&&r.why!=GlassTypeNull&&r.why!=GlassCompRange&&r.why!=GlassNotGlass;
        if(isGlass){bump(GG.glassHits);bump(GG.approachHist[glassBin(gh.approach)]);maxOf(GG.approachMaxMms,std::uint64_t(std::max(0.f,gh.approach)*1000.f));}
        if(r.why==GlassOk){bump(GG.judged[GlassOk]);bump(GG.wouldBreak);if(r.called){bump(GG.breaks);bump(r.broke?GG.done:GG.noShatter);}}
        if(isGlass){auto& x=s.gunSamples[s.gunSampleNext];s.gunSampleNext=(s.gunSampleNext+1u)%unsigned(s.gunSamples.size());
            x=State::GunSample{};x.set=true;x.ns=now;x.src=std::uint8_t(src);x.hand=std::uint8_t(h&1u);x.kind=kind;x.cls=cls;x.comp=gh.comp;x.part=ph.part;
            x.approach=gh.approach;x.tipMps=v::length(vel)/u;x.alongCm=v::length(v::sub(ph.pos,from))/u*100.f;x.lever=std::uint8_t(lever+1);
            x.result=std::uint8_t(r.why==GlassOk&&r.called&&!r.broke?GlassWhyCount:r.why);
            glass=true;break;}}
    return glass;
}
inline void gunGlassTick(State& s,std::uint64_t now) noexcept {
    const int L=s.lev.gunGlass,F=s.lev.gunGlassFree;
    if((L<0&&F<0)||!groupOk[GroupGunGlass]||!engine.probeLevel||!glassOn(s)||threadKey()!=s.mainKey)return;
    auto& GG=stats.gun;const float u=std::max(s.upm,1e-3f);++s.gunStep;
    bool levelChecked=false,levelOk=false;
    const auto level=[&]{if(!levelChecked){levelChecked=true;levelOk=gunLevelOk(s);}return levelOk;};
    const V3 mover=s.tg.moverPos;
    for(unsigned h=0;h<2;++h){auto& m=s.gunMem[h];const auto& t=s.tg.hand[h];const auto& o=s.tg.hand[h^1u];
        if(t.gun&&t.valid){if(!m.gunSince)m.gunSince=now?now:1;}else{m.gunSince=0;m.haveTip=false;}
        if(t.busy){m.busyNs=now?now:1;m.busyBits=t.busy;}
        const std::uint8_t recent=m.busyNs&&now>=m.busyNs&&now-m.busyNs<gunBusyHoldNs?m.busyBits:std::uint8_t(0);
        // the gun hand
        if(t.gun&&t.valid&&L>=0){bump(GG.ticks);
            GunGateIn g{};g.lever=L;g.gun=true;g.longOk=s.lev.gunGlassLong;g.held=t.cls==ClassHeld;g.onFoot=s.tg.onFoot!=0;g.bridged=s.tg.bridged!=0;
            g.fresh=s.tgFresh&&s.tgUsable;g.velOk=t.tipOk!=0;g.kind=t.kind;g.placedNs=m.gunSince;g.now=now;g.busy=t.busy;g.busyRecent=recent;
            g.sweepM=m.haveTip?v::length(v::sub(t.tipRel,m.prevTip))/u:0.f;g.speedMps=v::length(t.tipVelArm)/u;g.mps=s.lev.gunGlassMps;
            const GunWhy w=gunGate(g);const V3 tipW=v::add(mover,t.tipRel),boreW=v::add(mover,t.boreRel);
            if(w!=GunGo){bump(GG.gated[w]);}
            else if(!level())bump(GG.gated[GunStale]);
            else{alignas(16) unsigned char is[4*off::isectSize]{};
                const V3 from=m.haveTip?v::add(mover,m.prevTip):tipW;
                int n=gunProbe(s,from,tipW,s.lev.gunGlassR*u,is,0);
                if(!gunHits(s,h,is,n,0,t.tipVelArm,boreW,boreW,L,now,t.kind)&&v::length(v::sub(tipW,boreW))>.02f*u){
                    n=gunProbe(s,boreW,tipW,.03f*u,is,1);gunHits(s,h,is,n,1,t.tipVelArm,boreW,boreW,L,now,t.kind);}}
            m.prevTip=t.tipRel;m.haveTip=true;}
        // the empty hand while the other holds the firearm
        if(!t.gun&&o.gun&&o.valid&&t.valid&&F>=0){bump(GG.ticks);
            GunWhy w=GunGo;
            const float bore=segDistance(t.rel,o.boreRel,o.tipRel)/u;
            if(t.source!=1)w=GunNotFree;
            else if(!(bore>=gunNearBoreM))w=GunNearGun;
            else if(t.cls!=ClassHeld)w=GunClass;
            else if(!s.tg.onFoot)w=GunFoot;
            else if(s.tg.bridged)w=GunBridged;
            else if(!(s.tgFresh&&s.tgUsable)||!t.armOk)w=GunStale;
            else if(m.havePalm&&!(v::length(v::sub(t.rel,m.prevPalm))/u<=gunSweepMaxM))w=GunLong;
            else if(!(v::length(t.velArm)/u>=gunSlowFrac*s.lev.gunGlassMps))w=GunSlow;
            if(w!=GunGo)bump(GG.gated[w]);
            else if(!level())bump(GG.gated[GunStale]);
            else{alignas(16) unsigned char is[4*off::isectSize]{};const V3 pW=v::add(mover,t.rel);
                const V3 from=m.havePalm?v::add(mover,m.prevPalm):pW;
                const V3 back=v::sub(pW,v::mul(v::normalized(t.velArm),.10f*u)); // the palm's normal turned toward where it came from
                const int n=gunProbe(s,from,pW,.04f*u,is,2);gunHits(s,h,is,n,2,t.velArm,back,pW,F,now,-1);}
            m.prevPalm=t.rel;m.havePalm=true;}
        else m.havePalm=false;}
}
// ---- doors (tick) ----
// A door a kept pair met in the last 4 substeps while the hand touched a surface and the command is 5 mm or more past it: this tick's
// push of that hand (doorsTick acts on it).
// DOOR PRESS (with the leaf follow only): a door pair within 16 substeps is a candidate every tick (no contact age, no depth): startFollow
// judges the commanded parts against the leaf (the depth, from a contact of the last 4 substeps, still goes to the plain and anim paths).
// checks / notPressing count only the ticks the 4-substep contact rule above would have judged (contact), the rest pressCands. Without the
// follow (the swing path opens on any push, no depth) the contact rule above applies.
inline bool followOn(const State& s) noexcept;
inline void doorContact(State& s,unsigned h) noexcept {
    auto& hs=s.hand[h];auto& D=stats.doors;hs.push={};
    if(s.lev.doorPress&&followOn(s)){
        if(!s.lev.doors||!hs.door.inst||s.step<hs.door.step||s.step-hs.door.step>16)return;
        if(hs.machine.state!=BodyLive||!hs.haveSolved||!hs.haveCommand)return;
        const float u=std::max(s.upm,1e-3f);
        const bool recent=hs.est.age<=4&&s.step-hs.door.step<=4;
        float depth=hs.est.age<=4?std::max(0.f,v::dot(v::sub(hs.solvedPos,hs.command),hs.est.n))/u:0.f;if(!std::isfinite(depth))depth=0.f;
        const bool contact=recent&&depth>=.005f;
        bump(contact?D.checks:recent?D.notPressing:D.pressCands);
        hs.push={hs.door.inst,depth,hs.solvedPos,contact};return;}
    if(!s.lev.doors||!hs.door.inst||s.step<hs.door.step||s.step-hs.door.step>4)return;
    if(hs.est.age>4||hs.machine.state!=BodyLive||!hs.haveSolved||!hs.haveCommand)return;
    const float u=std::max(s.upm,1e-3f);
    const float depth=std::max(0.f,v::dot(v::sub(hs.solvedPos,hs.command),hs.est.n))/u;
    if(!(depth>=.005f)){bump(D.notPressing);return;}
    bump(D.checks);hs.push={hs.door.inst,depth,hs.solvedPos};
}
inline std::uintptr_t propManager() noexcept {
    std::uintptr_t a{},m{};return get(engine.base+off::propGot,a)&&a==engine.base+off::propVar&&get(a,m)?m:0;
}
// The door of a nav instance, resolved again every tick: the level holds the instance at its index, its door byte is 1 (GetDoor reads
// it unguarded), the manager has a handle, the door's frag is that instance.
struct DoorView {std::uintptr_t door{},iface{},vptr{};std::uint32_t state{};bool ok{};};
inline DoorView viewDoor(const State& s,std::uintptr_t nav) noexcept {
    DoorView d{};std::uint16_t idx{};std::uint8_t byte{};
    if(!nav||!get(nav+off::instIndex,idx)||!inLevel(s.level,nav,idx)||!get(nav+off::navDoor,byte)||byte!=1)return d;
    const auto mgr=propManager();if(!mgr)return d;
    void* handle=engine.getDoor(reinterpret_cast<void*>(mgr),reinterpret_cast<void*>(nav));
    void* inst=handle?engine.doorGet(handle):nullptr;std::uintptr_t frag{};
    if(!inst||!get(addr(inst)+off::doorState,d.state)||!get(addr(inst)+off::doorFrag,frag)||frag!=nav)return d;
    d.door=addr(inst);
    if(get(d.door+off::doorIface,d.iface)&&d.iface)get(d.iface,d.vptr);
    d.ok=true;return d;
}
// The palm in the leaf's frame (frag matrix rows a b c, origin +0x50 on the hinge axis) and the leaf's box from its bound.
struct DoorLocal {bool ok{},haveBox{};V3 local{},bmin{},bmax{},origin{};float axisUp{};};
inline DoorLocal doorLocal(const State& s,std::uintptr_t nav,V3 palm) noexcept {
    DoorLocal g{};float m[16]{};
    if(!get(nav+off::instMatrix,m))return g;
    const V3 a{m[0],m[1],m[2]},b{m[4],m[5],m[6]},c{m[8],m[9],m[10]},o{m[12],m[13],m[14]};
    if(!v::finite(a)||!v::finite(b)||!v::finite(c)||!v::finite(o)||!v::finite(palm))return g;
    const V3 d=v::sub(palm,o);g.local={v::dot(d,a),v::dot(d,b),v::dot(d,c)};g.origin=o;
    g.axisUp=std::fabs(v::dot(v::normalized(b),v::normalized(s.tg.up)));
    std::uintptr_t arch{},bound{};float mx[3]{},mn[3]{};
    if(get(nav+off::instArch,arch)&&arch&&get(arch+off::archBound,bound)&&bound&&get(bound+off::boundMax,mx)&&get(bound+off::boundMin,mn)){
        g.bmax={mx[0],mx[1],mx[2]};g.bmin={mn[0],mn[1],mn[2]};g.haveBox=v::finite(g.bmax)&&v::finite(g.bmin)&&mx[0]>mn[0]&&mx[1]>mn[1];}
    g.ok=true;return g;
}
inline void countGeom(DoorReject r,bool hold) noexcept {auto& g=hold?stats.doors.geomHold:stats.doors.geomStart;bump(g[r==DoorGeomLever?0u:r==DoorGeomHeight?1u:2u]);}
// hand / side < 0 / 0: any (an entry with a hand and a face gates only that hand on that face)
inline bool inGap(const State& s,std::uintptr_t nav,std::uint64_t now,int hand=-1,int side=0) noexcept {
    for(std::size_t i=0;i<s.gapNav.size();++i)if(s.gapNav[i]==nav&&now<s.gapUntil[i]){
        if(hand>=0&&s.gapHand[i]>=0&&s.gapHand[i]!=hand)continue;
        if(side&&s.gapSide[i]&&s.gapSide[i]!=side)continue;
        return true;}
    return false;
}
inline void setGap(State& s,std::uintptr_t nav,std::uint64_t until,int hand=-1,int side=0) noexcept {
    std::size_t slot=s.gapNext;for(std::size_t i=0;i<s.gapNav.size();++i)if(s.gapNav[i]==nav){slot=i;break;}
    if(slot==s.gapNext)s.gapNext=(s.gapNext+1u)%unsigned(s.gapNav.size());
    s.gapNav[slot]=nav;s.gapUntil[slot]=until;s.gapHand[slot]=std::int8_t(hand<0?-1:hand&1);s.gapSide[slot]=std::int8_t(side<0?-1:side>0?1:0);
}
inline bool swingOn(const State& s) noexcept {return s.lev.doors&&s.lev.doorSwing&&groupOk[GroupSwing]&&engine.doorFastClose;}
// The leaf follow (REST): SWING, DOORX and REST matched, levers doorswing and doorfollow on.
inline bool followOn(const State& s) noexcept {
    return swingOn(s)&&s.lev.doorFollow&&groupOk[GroupRest]&&groupOk[GroupDoorx]&&engine.doorSetState&&engine.pathState&&engine.continueShared&&engine.doorClose;}
// The hold still owns its door: same door, interface and vptr, an opening state (2, 3) or for a closing hold 5, the speed 0.
inline bool holdValid(const State& s,const DoorHoldState& hd,DoorView& d) noexcept {
    d=viewDoor(s,hd.nav);if(!d.ok||d.door!=hd.door||d.iface!=hd.iface)return false;
    float speed{1.f};
    return d.vptr==hd.vptr&&(hd.swing.closing?d.state==5:(d.state==2||d.state==3))&&get(d.iface+off::ifSpeed,speed)&&speed==0.f;
}
// A leaf-follow hold still owns its door: OPEN2 = state 2 / 3 at speed 0, POSE4 = state 4 with the phase the tick wrote.
inline bool followValid(const State& s,const DoorHoldState& hd,DoorView& d) noexcept {
    d=viewDoor(s,hd.nav);if(!d.ok||d.door!=hd.door||d.iface!=hd.iface||d.vptr!=hd.vptr)return false;
    float x{-1.f};
    return hd.mode==HoldPose4&&d.state==off::doorStateOpened&&get(d.iface+off::ifPhase,x)&&x==hd.lastT;
}
inline void addRested(State& s,const DoorHoldState& hd,float t,const Leaf* f=nullptr,float cap=0.f) noexcept {
    RestedDoor* r=nullptr;for(auto& x:s.rested)if(x.door==hd.door){r=&x;break;}
    if(!r){r=&s.rested[s.restedNext];s.restedNext=(s.restedNext+1u)%unsigned(s.rested.size());}
    *r=RestedDoor{hd.door,hd.iface,hd.nav,t};
    if(f){r->hinge=f->o;r->up=f->b;r->L0=f->L0;r->k=hd.kOpen;r->width=f->width;r->cap=cap;r->theta=easeOpen(std::clamp(t,0.f,1.f))*cap;}
}
inline void dropRested(State& s,std::uintptr_t door) noexcept {for(auto& x:s.rested)if(x.door==door)x=RestedDoor{};}
// Every hold back to the engine (mode change, refusal, levers off): the saved speed where an opening hold still owns its door; a posed
// door stays where it is.
inline void noteDoorEvent(State& s,const DoorHoldState& hd,DoorEnd end) noexcept;
inline void finishOne(State& s,DoorHoldState& hd) noexcept {
    if(!hd.active)return;DoorView d{};
    if(hd.mode==HoldPose4){if(hd.lastT<1.f&&followValid(s,hd,d))addRested(s,hd,hd.lastT);noteDoorEvent(s,hd,DoorEndMode);}
    else if(hd.mode==HoldSwing&&holdValid(s,hd,d)){put<float>(d.iface+off::ifSpeed,hd.savedSpeed);bump(stats.doors.finishes);}
    hd=DoorHoldState{};
}
inline void finishHolds(State& s) noexcept {for(auto& hd:s.holds)finishOne(s,hd);}
inline float savedSpeedOf(std::uintptr_t iface) noexcept {float v{};return get(iface+off::ifSpeed,v)&&std::isfinite(v)&&v>0.f?v:1.f;}
inline bool startHold(State& s,std::uintptr_t nav,const DoorView& d,bool closing,bool takeover,std::uint64_t now) noexcept {
    DoorHoldState* hd=nullptr;for(auto& x:s.holds)if(!x.active){hd=&x;break;}
    if(!hd)return false;
    std::uint8_t side{};float t{},cap{-1.f};
    if(!get(d.iface+off::ifSide,side)||!get(d.iface+off::ifPhase,t)||!std::isfinite(t))return false;
    const bool dir=side!=0;get(d.iface+off::ifCap+(dir?4u:0u),cap);
    DoorHoldState h{};h.nav=nav;h.door=d.door;h.iface=d.iface;h.vptr=d.vptr;h.dir=dir;h.active=true;h.savedSpeed=savedSpeedOf(d.iface);
    h.swing.startNs=now;h.swing.closing=closing;h.swing.keepOpen=takeover&&!closing;h.startNs=now;
    const float c=std::isfinite(cap)&&cap>0.f?cap:pi;const float tc=std::clamp(t,0.f,1.f);
    h.swing.theta=(closing?smooth(tc):easeOpen(std::min(tc,easeOpenTop)))*c;
    put<float>(d.iface+off::ifSpeed,0.f);
    *hd=h;return true;
}
// One held door: verify, then the swing step; the phase is written only when it moved (1e-4).
inline void stepHold(State& s,DoorHoldState& hd,const HandState::DoorPush* p,float dt,std::uint64_t now) noexcept {
    auto& D=stats.doors;DoorView d{};
    if(!holdValid(s,hd,d)){
        // the game took the door (a speed, a state or an interface of its own): left to it for a second
        if(d.ok&&d.door==hd.door){bump(D.dropsGame);setGap(s,hd.nav,now+doorGameGapNs);}else bump(D.dropsGone);
        hd=DoorHoldState{};return;}
    ++hd.ticks;
    float cap{-1.f},tNow{};std::uint8_t side{};
    get(d.iface+off::ifCap+(hd.dir?4u:0u),cap);
    if(!get(d.iface+off::ifPhase,tNow)||!get(d.iface+off::ifSide,side)){bump(D.dropsGone);hd=DoorHoldState{};return;}
    if(!(cap>=0.f)){if(hd.ticks<3){bump(D.capPending);return;}cap=pi;}
    bool push=false;float depth=0.f,lever=1.f;int face=1;
    if(p){const DoorLocal g=doorLocal(s,hd.nav,p->palm);
        if(g.ok&&g.axisUp>=.9f){const DoorGeom dg=doorGeom(g.local,g.bmin,g.bmax,g.haveBox,side?-1:1,s.upm);
            if(dg.ok){push=true;depth=p->depthM;lever=dg.leverM;face=dg.face;}else countGeom(dg.reject,true);}
        else if(g.ok)bump(D.geomAxis);}
    float npos[3]{};bool far=false;
    if(s.moverOk&&get(hd.nav+off::instPos,npos))far=v::length(v::sub(vec3(s.mover+off::moverPos),V3{npos[0],npos[1],npos[2]}))>doorFarM*std::max(s.upm,1e-3f);
    const bool glided=hd.swing.glide;float tOut=tNow;
    const DoorCmd cmd=doorSwingStep(hd.swing,push,depth,lever,face,cap,dt,now,far,s.lev,tOut);
    if(!glided&&hd.swing.glide)bump(D.glides);
    maxOf(D.maxMdeg,std::uint64_t(std::max(0.f,hd.swing.theta)*180000.f/pi));
    if(cmd==DoorCmdFinish){put<float>(d.iface+off::ifSpeed,hd.savedSpeed);bump(D.finishes);hd=DoorHoldState{};return;}
    if(cmd==DoorCmdFastClose){if(hd.swing.reversed)bump(D.reverses);engine.doorFastClose(reinterpret_cast<void*>(d.door),false);bump(D.fastCloses);
        setGap(s,hd.nav,now+doorGapNs);hd=DoorHoldState{};return;}
    if(std::fabs(tOut-tNow)>1e-4f&&std::isfinite(tOut)){put<float>(d.iface+off::ifPhase,tOut);bump(D.phaseWrites);}
}

// ---- the leaf follow ----
// The engine clock (seconds): [GOT 0x2ddf340] = &sysTimeManager::sm_GlobalManager (an object), its float +0x3c, as UseDelayDoorExecute
// and Update read it (one load of the GOT word, then the field).
inline bool clockSeconds(float& c) noexcept {return get(engine.base+off::clockVar+off::clockSeconds,c)&&std::isfinite(c);}
// The leaf frame of a door: the closed rows (propDoorInst +0x0..+0x20), the frag's matrix now, its bound's box.
inline bool readLeaf(const State& s,std::uintptr_t nav,std::uintptr_t door,Leaf& f) noexcept {
    float cm[12]{},m[16]{};
    if(!get(door,cm)||!get(nav+off::instMatrix,m))return false;
    const Rows closed{V3{cm[0],cm[1],cm[2]},V3{cm[4],cm[5],cm[6]},V3{cm[8],cm[9],cm[10]}},now{V3{m[0],m[1],m[2]},V3{m[4],m[5],m[6]},V3{m[8],m[9],m[10]}};
    if(!v::finite(closed)||!v::finite(now))return false;
    V3 bmin{},bmax{};bool box=false;std::uintptr_t arch{},bound{};float mx[3]{},mn[3]{};
    if(get(nav+off::instArch,arch)&&arch&&get(arch+off::archBound,bound)&&bound&&get(bound+off::boundMax,mx)&&get(bound+off::boundMin,mn)){
        bmax={mx[0],mx[1],mx[2]};bmin={mn[0],mn[1],mn[2]};box=true;}
    f=leafFrame(closed,now,V3{m[12],m[13],m[14]},bmin,bmax,box,s.tg.up,s.upm);
    return true;
}
struct FollowPalm {bool have{};V3 solved{},command{};};
inline FollowPalm followPalm(const State& s,unsigned h) noexcept {
    const auto& hs=s.hand[h&1u];FollowPalm p{};
    p.have=hs.present&&hs.machine.state==BodyLive&&hs.haveSolved&&hs.haveCommand&&!s.grab[h&1u].active;p.solved=hs.solvedPos;p.command=hs.command;
    return p;
}
inline void noteDoorReject(State& s,std::uintptr_t nav,const Leaf& f,V3 palm,std::uint32_t state,const LeafPalm& lp,PlainWhy why,float gapCm=-1.f,int part=-1,bool normalOk=true) noexcept {
    DoorRejectSample* slot=nullptr;
    for(auto& r:s.doorRejects){if(r.nav==nav)return;if(!r.nav&&!slot)slot=&r;}
    if(!slot)return;
    const V3 d=v::sub(palm,f.o);
    DoorRejectSample r{};r.nav=nav;r.local={v::dot(d,f.a),v::dot(d,f.b),v::dot(d,f.c)};r.state=state;r.r=lp.r;r.delta=lp.delta;r.why=std::uint8_t(why);
    r.gapCm=gapCm;r.part=part;r.normalOk=normalOk;
    std::uintptr_t arch{},bound{};float mx[3]{},mn[3]{};
    if(get(nav+off::instArch,arch)&&arch&&get(arch+off::archBound,bound)&&bound&&get(bound+off::boundMax,mx)&&get(bound+off::boundMin,mn)){
        r.bmax={mx[0],mx[1],mx[2]};r.bmin={mn[0],mn[1],mn[2]};}
    *slot=r;
}
inline void noteDoorEvent(State& s,const DoorHoldState& hd,DoorEnd end) noexcept {
    DoorEvent e{};e.set=true;e.state=hd.startState;e.mode=std::uint8_t(hd.mode);e.end=std::uint8_t(end);e.side=hd.q;e.startDeg=hd.startTheta*180.f/pi;
    e.endDeg=hd.theta*180.f/pi;e.ticks=hd.ticks;e.moves=hd.moves;const float u=std::max(s.upm,1e-3f);e.centreGapCm=hd.centreGap/u*100.f;e.partGapCm=hd.partGap/u*100.f;
    e.moverHingeM=hd.moverHinge/u;e.presses=hd.presses;e.floorHolds=hd.floorHoldsN;e.flips=hd.flips;e.maxLagCm=hd.maxLagCm;s.doorEvents[s.doorEventNext]=e;s.doorEventNext=(s.doorEventNext+1u)%unsigned(s.doorEvents.size());bump(stats.doors.events);
}
// The hand's parts (solved or commanded) and the mover's horizontal distance to a leaf's hinge.
inline HandBoxes solvedBoxes(const State& s,unsigned h) noexcept {const auto& hs=s.hand[h&1u];return handBoxes(hs.solvedPos,hs.solvedRows,hs.fist,s.upm);}
inline HandBoxes commandBoxes(const State& s,unsigned h) noexcept {const auto& hs=s.hand[h&1u];return handBoxes(hs.command,hs.cmdRows,hs.fist,s.upm);}
inline float moverHinge(const State& s,const Leaf& f) noexcept {
    if(!s.moverOk)return 1e9f;const V3 d=v::sub(vec3(s.mover+off::moverPos),f.o);const V3 flat=v::sub(d,v::mul(f.b,v::dot(d,f.b)));return v::length(flat);}
// The palm centre's gap to the leaf slab (diagnostics).
inline float centreGap(const Leaf& f,V3 b) noexcept {return std::fabs(v::dot(v::sub(b,f.o),f.c))-f.halfZ;}
// qForce / sideZ (DOOR PRESS): the side and face locked from the approach (0: from the part's centre)
inline bool beginPose4(State& s,DoorHoldState& slot,std::uintptr_t nav,const DoorView& d,const Leaf& f,const LeafTouch& lt,V3 k,bool verified,unsigned h,
                       std::uint64_t now,int qForce=0,int sideZ=0) noexcept {
    std::uint8_t side{};float t{},cap{-1.f};
    if(!get(d.iface+off::ifSide,side)||!get(d.iface+off::ifPhase,t)||!std::isfinite(t))return false;
    get(d.iface+off::ifCap+(side?4u:0u),cap);
    const LeafPalm lp=leafPalm(f,k,lt.centre,s.upm);
    DoorHoldState x{};x.nav=nav;x.door=d.door;x.iface=d.iface;x.vptr=d.vptr;x.dir=side!=0;x.active=true;x.mode=HoldPose4;x.q=lp.delta<0.f?-1:1;
    x.kOpen=k;x.verified=verified;x.lastT=t;x.onNs=now;x.startNs=now;x.hand=h;x.theta=easeOpen(std::clamp(t,0.f,1.f))*std::max(cap,0.f);
    x.swing.startNs=now;x.savedSpeed=savedSpeedOf(d.iface);x.startState=d.state;x.startTheta=x.theta;x.partGap=lt.gap;
    x.centreGap=centreGap(f,s.hand[h&1u].solvedPos);x.moverHinge=moverHinge(s,f);
    if(qForce)x.q=qForce<0?-1:1;
    x.sideZ=sideZ?sideZ:lt.side;
    if(sideZ){auto& tr=s.doorSide[h&1u];tr=DoorSideTrack{};tr.nav=nav;tr.side=sideZ;tr.ns=now;}
    slot=x;return true;
}
// POSE4 pushed shut (nothing in the closing wedge): the closing phase of that angle, then the engine's Close (state 4 only).
inline void shutPose4(State& s,DoorHoldState& hd,const DoorView& d,float theta,float cap,std::uint64_t now) noexcept {
    put<float>(d.iface+off::ifPhase,phaseFor(theta/std::max(cap,1e-3f),true));engine.doorClose(reinterpret_cast<void*>(d.door),false);
    hd.theta=theta;noteDoorEvent(s,hd,DoorEndShut);
    if(s.lev.doorPress&&hd.sideZ)setGap(s,hd.nav,now+std::uint64_t(double(s.lev.doorRearmMs)*1e6),int(hd.hand&1u),hd.sideZ);
    else setGap(s,hd.nav,now+std::uint64_t(double(s.lev.doorRearmMs)*1e6));
    dropRested(s,d.door);bump(stats.doors.shutsPose4);hd=DoorHoldState{};
}
// A closed pivot door opened posed (the hand's or the mover's push), all in one tick: the engine's Open(point) (nav open, side, axis and
// speed, its own bookkeeping), the phase 0 and ContinueActionShared (its first call resolves the swing limit +0x28[dir] through
// DoorRangeTest; the leaf stays shut), the phase of the target and ContinueActionShared again (SetCurrentMatrix: the leaf and its collision
// follow), SetState(4, false) (SetAudioState(4, 2) stops the loop), then the engine's own 2 -> 4 step (+0x84 = clock + 10, +0xa0 = 0,
// UpdatePathObjectState). 1 = posed (theta / cap out), 0 = the Open did not leave the door opening (nothing more written), -1 = the limit
// still pending: the door is left to the stock opening.
inline int openPosed(State& s,std::uintptr_t nav,const DoorView& d,V3 point,float target,float& theta,float& capOut) noexcept {
    auto& D=stats.doors;
    alignas(16) const float q[4]{point[0],point[1],point[2],0.f};
    engine.doorOpen(reinterpret_cast<void*>(d.door),q,false);bump(D.opens);
    const DoorView e=viewDoor(s,nav);std::uint8_t side{};
    if(!e.ok||e.door!=d.door||e.iface!=d.iface||e.vptr!=d.vptr||e.state!=2||!get(e.iface+off::ifSide,side)){bump(D.posedFails);return 0;}
    put<float>(e.iface+off::ifPhase,0.f);engine.continueShared(reinterpret_cast<void*>(e.iface),reinterpret_cast<void*>(e.door));
    float cap{-1.f};get(e.iface+off::ifCap+(side?4u:0u),cap);
    if(!(cap>=0.f)||!std::isfinite(cap)){bump(D.capFallback);return -1;}
    if(cap<2.f*pi/180.f){bump(D.capTiny);return -1;} // a swing limit the range test cut to nothing: the stock opening (no state 4 at 0 degrees)
    const float th=std::clamp(target,0.f,.9998f*cap),tOut=phaseFor(th/std::max(cap,1e-3f),false);
    put<float>(e.iface+off::ifPhase,tOut);engine.continueShared(reinterpret_cast<void*>(e.iface),reinterpret_cast<void*>(e.door));
    engine.doorSetState(reinterpret_cast<void*>(e.door),int(off::doorStateOpened),false);
    {float clock{};if(clockSeconds(clock))put<float>(e.door+off::doorCloseAt,clock+off::doorRestDelayS);}
    put<std::uint8_t>(e.door+off::doorUse,0);engine.pathState(reinterpret_cast<void*>(nav));
    theta=easeOpen(tOut)*cap;capOut=cap;return 1;
}
// One POSE4 hold per tick: verify (state 4 with the phase the tick wrote), the leaf frame and its open axis, the hand's nearest part
// (its face chosen every tick: the leaf follows it both ways), the target with that part's clearance, stopped where the leaf would meet
// the player's mover (closeStop / openStop), then the pose (the phase and ContinueActionShared), a shut (only with nothing in the closing
// wedge), or a release that leaves the door at its angle.
inline void stepFollow(State& s,DoorHoldState& hd,float dt,std::uint64_t now) noexcept {
    auto& D=stats.doors;DoorView d{};
    if(!followValid(s,hd,d)){
        if(d.ok&&d.door==hd.door){bump(D.dropsGame);bump(D.dropsGame4);setGap(s,hd.nav,now+doorGameGapNs);noteDoorEvent(s,hd,DoorEndGame);}
        else{bump(D.dropsGone);noteDoorEvent(s,hd,DoorEndGone);}
        hd=DoorHoldState{};return;}
    ++hd.ticks;
    const float u=std::max(s.upm,1e-3f);
    float t{},cap{-1.f};std::uint8_t side{};Leaf f{};
    const auto giveBack=[&](DoorEnd why){if(hd.moved){addRested(s,hd,hd.lastT,f.sure?&f:nullptr,cap);bump(D.rests);}noteDoorEvent(s,hd,why);hd=DoorHoldState{};};
    if(!get(d.iface+off::ifPhase,t)||!get(d.iface+off::ifSide,side)||!std::isfinite(t)){bump(D.dropsGone);giveBack(DoorEndGone);return;}
    get(d.iface+off::ifCap+(side?4u:0u),cap);
    if(!(cap>=0.f)){bump(D.capPending);giveBack(DoorEndGeom);return;}
    if(!readLeaf(s,hd.nav,hd.door,f)||!f.sure){bump(D.geomMismatch);giveBack(DoorEndGeom);return;}
    const float thetaNow=easeOpen(std::clamp(t,0.f,1.f))*cap;hd.theta=thetaNow;
    if(thetaNow>=doorOneDeg){const V3 k=openAxisMeasured(f.L0,f.L);
        if(std::fabs(v::dot(k,f.b))<.95f){bump(D.geomMismatch);giveBack(DoorEndGeom);return;}
        if(!hd.verified&&v::dot(k,hd.kOpen)<0.f){bump(D.axisMismatch);giveBack(DoorEndGeom);return;}
        hd.verified=true;hd.kOpen=k;}
    const FollowPalm fp=followPalm(s,hd.hand);
    float npos[3]{};bool far=false;
    if(s.moverOk&&get(hd.nav+off::instPos,npos))far=v::length(v::sub(vec3(s.mover+off::moverPos),V3{npos[0],npos[1],npos[2]}))>doorFarM*u;
    hd.moverHinge=moverHinge(s,f);
    const bool old=now>=hd.startNs&&now-hd.startNs>doorHoldMaxNs;
    bool haveTarget=false;float target=thetaNow;
    DoorTick tk{};tk.set=true;tk.ns=now;tk.hand=std::uint8_t(hd.hand&1u);tk.door=std::uint16_t(hd.nav>>4);tk.deg=thetaNow*180.f/pi;tk.targetDeg=tk.deg;
    if(fp.have&&s.lev.doorPress){
        // DOOR PRESS: the commanded parts against the leaf; the face locked (two outside samples on the other face turn it); the leaf
        // moves on every tick a commanded part is on or in it, the hold kept while one is within doorPressKeepM
        const HandBoxes cb=commandBoxes(s,hd.hand);const LeafTouch lc=leafTouch(f,cb,s.upm,true),ls=leafTouch(f,solvedBoxes(s,hd.hand),s.upm,true);
        auto& tr=s.doorSide[hd.hand&1u];
        if(!tr.fresh(hd.nav,now)&&hd.sideZ){tr=DoorSideTrack{};tr.nav=hd.nav;tr.side=hd.sideZ;tr.ns=now;}
        const int solvedSide=ls.ok?ls.side:0;
        tr.note(hd.nav,lc,s.upm,now,solvedSide);
        if(tr.fresh(hd.nav,now)&&tr.side!=hd.sideZ){if(hd.sideZ){bump(D.sideFlips);++hd.flips;}hd.sideZ=tr.side;}
        if(!hd.sideZ)hd.sideZ=lc.ok?lc.side:1;
        // through: the command past the far face while the solved part stays on the locked one (a leaf the mover stop or the rate cap
        // held): within 2 cm a press (the leaf catches up); further a press only while the solved part touches the locked face (within
        // doorTouchM, not straddling the free edge: the hand is still on the leaf, the leaf held back), else only kept (the leaf does not
        // move toward a hand that left it). The target stays rate capped either way.
        const bool through=lc.ok&&lc.gap>=0.f&&lc.side!=hd.sideZ&&solvedSide==hd.sideZ;
        const bool touchOn=through&&ls.ok&&!ls.straddle&&ls.gap<=doorTouchM*u;
        hd.q=doorQOf(f,hd.kOpen,hd.sideZ);
        hd.centreGap=centreGap(f,fp.solved);if(ls.ok)hd.partGap=ls.gap;
        {const float lag=v::length(v::sub(fp.solved,fp.command))/u*100.f;if(std::isfinite(lag))hd.maxLagCm=std::max(hd.maxLagCm,lag);}
        tk.cmdGapCm=lc.ok?lc.gap/u*100.f:-999.f;tk.solvedGapCm=ls.ok?ls.gap/u*100.f:-999.f;tk.r=lc.x/u;tk.y=lc.y/u;
        if((lc.ok&&lc.gap<=doorPressKeepM*u&&(lc.gap<=0.f||lc.side==hd.sideZ||through))||(ls.ok&&ls.gap<=doorKeepOnM*u&&ls.side==hd.sideZ))hd.onNs=now;
        if(lc.ok&&lc.part>=0&&(lc.gap<=0.f||(through&&(lc.gap<=.02f*u||touchOn)))){
            tk.press=std::uint8_t(lc.gap<=0.f?1:lc.gap<=.02f*u?2:3);if(tk.press==3)bump(D.throughTouch);
            bump(D.pressTicks);++hd.presses;hd.pressNs=now;if(lc.straddle)bump(D.edgeStraddles);
            if(!ls.ok||ls.gap>doorTouchM*u)bump(D.solvedGateWouldRefuse);
            const LeafPalm pc=leafPalm(f,hd.kOpen,cb.c[std::size_t(lc.part)],s.upm);tk.deltaDeg=pc.delta*180.f/pi;
            if(pc.r>=.5f*leafRMinM*u){target=doorTarget(hd.q,thetaNow,pc.delta,partClearance(f.halfZ,lc.ext,pc.r),cap,s.lev.doorRateRadS,dt);haveTarget=true;}}
        else bump(D.noPressTicks);}
    else if(fp.have){const LeafTouch lt=leafTouch(f,solvedBoxes(s,hd.hand),s.upm);
        hd.centreGap=centreGap(f,fp.solved);if(lt.ok)hd.partGap=lt.gap;tk.solvedGapCm=lt.ok?lt.gap/u*100.f:-999.f;
        if(lt.ok&&lt.gap<=doorKeepOnM*u)hd.onNs=now;
        if(lt.ok&&lt.gap<=doorTouchM*u){
            const LeafPalm ps=leafPalm(f,hd.kOpen,lt.centre,s.upm);const int q=ps.delta<0.f?-1:1;
            if(q!=hd.q){bump(D.sideFlips);++hd.flips;hd.q=q;}
            // the same part of the command: the leaf keeps that part's clearance from where the hand is going
            const HandBoxes cb=commandBoxes(s,hd.hand);
            if(cb.mask&&lt.part>=0){const LeafPalm pc=leafPalm(f,hd.kOpen,cb.c[std::size_t(lt.part)],s.upm);tk.deltaDeg=pc.delta*180.f/pi;tk.r=pc.r/u;
                if(pc.r>=.5f*leafRMinM*u){target=doorTarget(hd.q,thetaNow,pc.delta,partClearance(f.halfZ,lt.ext,pc.r),cap,s.lev.doorRateRadS,dt);haveTarget=true;}}}}
    tk.q=std::int8_t(hd.q);
    const auto ring=[&](DoorTickWhy why){tk.why=why;s.doorTicks[s.doorTickNext]=tk;s.doorTickNext=(s.doorTickNext+1u)%unsigned(s.doorTicks.size());};
    const bool idle=now>=hd.onNs&&double(now-hd.onNs)>double(s.lev.doorIdleMs)*1e6;
    if(!fp.have||far||old||idle){
        bump(!fp.have?D.releasesBody:far?D.releasesFar:old?D.releasesOld:D.releasesIdle);if(far)bump(D.far);ring(DtRelease);
        giveBack(!fp.have?DoorEndBody:far?DoorEndFar:old?DoorEndOld:DoorEndIdle);return;}
    if(!haveTarget){ring(DtNoPress);return;}
    // the player's mover against the leaf's move (unknown mover: nothing in the way, as before the hold)
    MoverAngle ma{};ma.ok=true;ma.beyond=true;
    if(s.moverOk)ma=moverAngle(vec3(s.mover+off::moverPos),f.o,f.b,f.L0,hd.kOpen,f.width,s.upm);
    tk.psiDeg=ma.psi*180.f/pi;tk.marginDeg=ma.margin*180.f/pi;tk.moverR=ma.r/u;
    const float shut=s.lev.doorShutDeg*pi/180.f;float stop=0.f;DoorTickWhy why=DtMoved;
    if(hd.q>0){stop=closeStop(ma,thetaNow,shut);
        if(target<stop){if(thetaNow>stop||target<thetaNow){bump(D.floorHolds);++hd.floorHoldsN;why=DtCloseStop;}target=std::max(target,std::min(stop,thetaNow));}}
    else{const float top=openStop(ma,thetaNow);if(target>top){bump(D.floorHolds);++hd.floorHoldsN;why=DtOpenStop;target=std::max(top,thetaNow);}}
    maxOf(D.maxMdeg,std::uint64_t(std::max(0.f,target)*180000.f/pi));tk.targetDeg=target*180.f/pi;
    if(hd.q>0&&stop<=0.f&&target<=shut){ring(DtMoved);shutPose4(s,hd,d,target,cap,now);return;}
    if(std::fabs(target-thetaNow)>.05f*pi/180.f){
        const float tOut=phaseFor(target/std::max(cap,1e-3f),false);
        put<float>(d.iface+off::ifPhase,tOut);engine.continueShared(reinterpret_cast<void*>(d.iface),reinterpret_cast<void*>(d.door));
        hd.lastT=tOut;hd.theta=target;hd.moved=true;++hd.moves;bump(D.pose4Moves);bump(D.phaseWrites);}
    else if(why==DtMoved)why=DtSmall;
    ring(why);
}
// The rested doors (state 4, part-open by a POSE4 hold). Every tick two reads (state, phase): the engine's own close (state 5, the phase
// still the one the tick wrote, or at most 0.15 below it after a frame of closing) gets the closing phase of the same angle once; another
// state or phase forgets it. A door under doorRaiseDeg is posed to it once the mover comes into its swing disc (it cannot be walked open
// in state 4), unless the wedge the leaf would sweep holds the mover (raiseClear; counted as raiseBlocked per tick). Every write and
// every 16th tick: the full door check first.
inline void restedTick(State& s) noexcept {
    ++s.restedTicks;
    for(auto& r:s.rested){if(!r.door)continue;
        std::uint32_t st{};float t{};
        if(!get(r.door+off::doorState,st)||!get(r.iface+off::ifPhase,t)||!std::isfinite(t)){r=RestedDoor{};continue;}
        const bool closing=st==off::doorStateClosing&&t<=r.lastT&&t>=r.lastT-.15f;
        bool raise=!s.lev.doorBody&&st==off::doorStateOpened&&t==r.lastT&&r.width>0.f&&r.cap>0.f&&r.theta<(doorRaiseDeg-1.f)*pi/180.f&&s.moverOk&&
                   inSwingDisc(vec3(s.mover+off::moverPos),r.hinge,r.up,r.width,s.upm);
        if(raise){const float to=std::min(doorRaiseDeg*pi/180.f,.9998f*r.cap);
            if(!raiseClear(vec3(s.mover+off::moverPos),r.hinge,r.up,r.L0,r.k,r.theta,r.width,to-r.theta,s.upm)){raise=false;bump(stats.doors.raiseBlocked);}}
        if(closing||raise||s.restedTicks>=r.checked+16){
            const DoorView d=viewDoor(s,r.nav);r.checked=s.restedTicks;
            if(!d.ok||d.door!=r.door||d.iface!=r.iface||d.state!=st){r=RestedDoor{};continue;}}
        if(closing){put<float>(r.iface+off::ifPhase,std::max(0.f,restRemapPhase(r.lastT)-(r.lastT-t)));bump(stats.doors.restRemaps);r=RestedDoor{};continue;}
        if(raise){const float tOut=phaseFor(std::min(doorRaiseDeg*pi/180.f,.9998f*r.cap)/r.cap,false);
            put<float>(r.iface+off::ifPhase,tOut);engine.continueShared(reinterpret_cast<void*>(r.iface),reinterpret_cast<void*>(r.door));
            r.lastT=tOut;r.theta=easeOpen(tOut)*r.cap;bump(stats.doors.raises);continue;}
        if(st!=off::doorStateOpened||t!=r.lastT)r=RestedDoor{};
    }
}
// DOOR PRESS start (a pivot door with a sure leaf, state 0 or 4, no hold on it). The face: the hand's approach track (fresh), else the
// solved part outside the slab, else the contact normal along the leaf's, else the solved part's centre. A press = the commanded part on
// or in the leaf (no normal test); a closed leaf needs it 1 cm past the face. The rearm after a shut gates only that hand on that face.
// Then the posed open (state 0) or the POSE4 hold (state 4) as before, with the side locked. The Open's point is the pressed part moved
// outside that face (the engine opens the leaf away from it).
inline void startPress(State& s,unsigned h,const HandState::DoorPush& p,const DoorView& d,const Leaf& f,float t,float cap,std::uint64_t now) noexcept {
    auto& D=stats.doors;const float u=std::max(s.upm,1e-3f);const auto& hs=s.hand[h&1u];
    const HandBoxes cb=commandBoxes(s,h);const LeafTouch lc=leafTouch(f,cb,s.upm,true),ls=leafTouch(f,solvedBoxes(s,h),s.upm,true);
    auto& tr=s.doorSide[h&1u];tr.note(p.nav,lc,s.upm,now,ls.ok?ls.side:0);
    int sideZ=0;unsigned src=0;
    if(tr.fresh(p.nav,now)){sideZ=tr.side;src=0;}
    else if(ls.ok&&ls.gap>=-.005f*u){sideZ=ls.side;src=1;}
    else if(hs.est.age<=16&&std::fabs(v::dot(hs.est.n,f.c))>=.5f){sideZ=v::dot(hs.est.n,f.c)>0.f?1:-1;src=2;}
    else{sideZ=ls.ok?ls.side:lc.ok?lc.side:1;src=3;}
    DoorTick tk{};tk.set=true;tk.ns=now;tk.hand=std::uint8_t(h&1u);tk.door=std::uint16_t(p.nav>>4);tk.cmdGapCm=lc.ok?lc.gap/u*100.f:-999.f;
    tk.solvedGapCm=ls.ok?ls.gap/u*100.f:-999.f;tk.r=lc.x/u;tk.y=lc.y/u;tk.q=std::int8_t(sideZ);
    {const float th=easeOpen(std::clamp(t,0.f,1.f))*std::max(cap,0.f);tk.deg=d.state==0?0.f:th*180.f/pi;tk.targetDeg=tk.deg;}
    const auto ring=[&](DoorTickWhy why){tk.why=why;s.doorTicks[s.doorTickNext]=tk;s.doorTickNext=(s.doorTickNext+1u)%unsigned(s.doorTicks.size());};
    const bool nearLeaf=lc.ok&&lc.gap<.06f*u;
    const float need=d.state==0?-.01f*u:0.f;
    if(!lc.ok||lc.gap>need){
        // a hand resting near a door is refused every tick: refusedGapCm / opened count only the contact pushes (the 4-substep rule's), the
        // tick ring and the reject ring get at most one sample per hand and door per doorNoteNs (the hold ticks stay in the ring)
        auto& nt=s.doorNote[h&1u];const bool note=nt.nav!=p.nav||now<nt.ns||now-nt.ns>=doorNoteNs;
        if(note){nt.nav=p.nav;nt.ns=now;}
        if(d.state==0&&lc.ok&&lc.gap<=0.f){bump(D.closedShallow);if(note)ring(DtStartShallow);}
        else{bump(D.startNoPress);if(p.contact)bump(D.refusedGap[doorGapBucket(lc.ok,lc.gap/u)]);
            if(note){bump(D.startNoPressNoted);if(nearLeaf)ring(DtStartNoPress);
                LeafPalm lp{};lp.r=lc.x;noteDoorReject(s,p.nav,f,p.palm,d.state,lp,PlainOffLeaf,lc.ok?lc.gap/u*100.f:-1.f,lc.part,true);}}
        if(d.state==off::doorStateOpened&&p.contact)bump(D.opened);
        return;}
    if(inGap(s,p.nav,now,int(h&1u),sideZ)){bump(D.gated);bump(D.rearmGated);ring(DtStartGated);return;}
    const auto countSide=[&]{bump(src==0?D.sideApproach:src==1?D.sideSolved:src==2?D.sideNormal:D.sideCentre);};
    // the pressed part moved outside the face it presses from
    const float zc=v::dot(v::sub(lc.centre,f.o),f.c);
    const V3 outside=v::add(lc.centre,v::mul(f.c,float(sideZ)*(f.halfZ+lc.ext+.02f*u)-zc));
    alignas(16) const float q[4]{outside[0],outside[1],outside[2],0.f};
    if(d.state==0){
        if(!s.doors.allow(p.nav,now)){bump(D.gated);ring(DtStartGated);return;}
        countSide();ring(DtStart);
        if(s.lev.doorHandPose){ // the leaf opened to the pressed part (its clearance), then held as POSE4
            const V3 k=openAxisPush(f.L0,f.c0,v::sub(outside,f.o));float target=2.f*pi/180.f;
            if(lc.part>=0){const LeafPalm pc=leafPalm(f,k,cb.c[std::size_t(lc.part)],s.upm);
                if(pc.r>=.5f*leafRMinM*u)target=std::max(target,doorTarget(-1,0.f,pc.delta,partClearance(f.halfZ,lc.ext,pc.r),pi,s.lev.doorRateRadS,std::max(s.doorDt,.011f)));}
            float th{},capOut{};const int r=openPosed(s,p.nav,d,outside,target,th,capOut);
            if(r>0){bump(D.closedPoses);maxOf(D.closedFirstMdegMax,std::uint64_t(th*180000.f/pi));
                const DoorView e=viewDoor(s,p.nav);DoorHoldState* slot=nullptr;for(auto& x:s.holds)if(!x.active){slot=&x;break;}
                if(e.ok&&slot&&beginPose4(s,*slot,p.nav,e,f,lc,k,false,h,now,doorQOf(f,k,sideZ),sideZ)){slot->startState=0;slot->startTheta=0.f;bump(D.pose4Holds);}
                else{DoorHoldState ev{};ev.nav=p.nav;ev.door=d.door;ev.iface=d.iface;ev.kOpen=k;addRested(s,ev,phaseFor(th/std::max(capOut,1e-3f),false),&f,capOut);}}
            DoorHoldState ev{};ev.startState=0;ev.mode=HoldPose4;ev.q=sideZ;ev.partGap=lc.gap;ev.centreGap=centreGap(f,hs.solvedPos);ev.moverHinge=moverHinge(s,f);ev.theta=th;
            noteDoorEvent(s,ev,DoorEndOpen);
            return;}
        engine.doorOpen(reinterpret_cast<void*>(d.door),q,false);bump(D.opens);bump(D.plainOpens);
        DoorHoldState ev{};ev.startState=0;ev.mode=HoldPose4;ev.q=sideZ;ev.partGap=lc.gap;ev.centreGap=centreGap(f,hs.solvedPos);ev.moverHinge=moverHinge(s,f);
        noteDoorEvent(s,ev,DoorEndOpen);
        return;}
    if(d.state==off::doorStateOpened){
        DoorHoldState* slot=nullptr;for(auto& x:s.holds)if(!x.active){slot=&x;break;}
        const float theta=easeOpen(std::clamp(t,0.f,1.f))*std::max(cap,0.f);
        V3 k=openAxisPush(f.L0,f.c0,v::sub(outside,f.o));bool verified=false;
        if(theta>=doorOneDeg){const V3 mk=openAxisMeasured(f.L0,f.L);if(std::fabs(v::dot(mk,f.b))>=.95f){k=mk;verified=true;}}
        if(s.lev.doorRest&&slot&&cap>=0.f&&beginPose4(s,*slot,p.nav,d,f,lc,k,verified,h,now,doorQOf(f,k,sideZ),sideZ)){countSide();ring(DtStart);bump(D.pose4Holds);return;}
        if(s.lev.doorClose&&engine.doorClose){if(!s.doors.allow(p.nav,now)){bump(D.gated);return;}engine.doorClose(reinterpret_cast<void*>(d.door),false);bump(D.closes);return;}
        bump(D.opened);return;}
    bump(D.busy);
}
// A push on a door no hold owns, with the leaf follow. The game's own door moves (states 2, 3, 5) are never touched; 1 -> a buzz. A
// pivot door with a sure leaf: the push counts only when a hand part is on the leaf (leafTouch) with the contact normal along the leaf's;
// then state 0 -> the engine's Open(palm), 4 -> a POSE4 hold. Without a sure leaf (no box, not upright, not a pivot door) state 0 gets the
// engine's Open (lever doorplain, 1 cm deep, one per second).
inline void startFollow(State& s,unsigned h,const HandState::DoorPush& p,const DoorView& d,std::uint64_t now) noexcept {
    auto& D=stats.doors;
    const bool pivot=d.vptr==engine.base+off::pivotVptr,anim=d.vptr==engine.base+off::animVptr;
    alignas(16) const float q[4]{p.palm[0],p.palm[1],p.palm[2],0.f};
    if(d.state==1){bump(D.locked);
        if(s.lev.haptic&&(!s.lockedPulseNs||now>=s.lockedPulseNs+1'000'000'000ull)&&haptics::pulse(h,.25f,15'000,now,haptics::SourceContact)){s.lockedPulseNs=now;bump(D.lockedPulses);}
        return;}
    if(d.state==2||d.state==3||d.state==5){bump(D.busy);bump(D.engineBusy);return;} // the game is moving it: its open / close finishes as stock
    if(anim){
        if(d.state==0&&p.depthM>=.02f){if(!s.doors.allow(p.nav,now)){bump(D.gated);return;}engine.doorOpen(reinterpret_cast<void*>(d.door),q,false);bump(D.opens);bump(D.animOpens);}
        else bump(d.state==off::doorStateOpened?D.opened:D.busy);
        return;}
    if(!s.lev.doorPress&&inGap(s,p.nav,now)){bump(D.gated);bump(D.rearmGated);return;} // DOOR PRESS: per hand and face (startPress)
    Leaf f{};PlainWhy why=PlainOffLeaf;float t{},cap{-1.f};std::uint8_t side{};
    const bool iface=pivot&&d.iface&&get(d.iface+off::ifPhase,t)&&get(d.iface+off::ifSide,side)&&std::isfinite(t);
    if(iface)get(d.iface+off::ifCap+(side?4u:0u),cap);
    bool sure=false;
    if(!iface)why=PlainNotPivot;
    else if(!readLeaf(s,p.nav,d.door,f))why=PlainNoBox;
    else if(!f.upright){why=PlainNotUpright;bump(D.geomAxis);}
    else if(!f.sure)why=PlainNoBox;
    else sure=true;
    if(!sure){ // not sure of the leaf: the engine's own Open of a closed door, nothing else
        if(d.state==0&&s.lev.doorPlain&&p.depthM>=.01f){if(!s.doors.allow(p.nav,now)){bump(D.gated);return;}
            engine.doorOpen(reinterpret_cast<void*>(d.door),q,false);bump(D.opens);bump(D.plainOpens);bump(D.plainBy[why]);}
        else if(d.state==off::doorStateOpened)bump(D.opened);
        return;}
    if(s.lev.doorPress){startPress(s,h,p,d,f,t,cap,now);return;}
    const auto& hs=s.hand[h&1u];const LeafTouch lt=leafTouch(f,solvedBoxes(s,h),s.upm);
    const bool normalOk=std::fabs(v::dot(hs.est.n,f.c))>=doorNormalCos;
    if(!lt.ok||lt.gap>doorTouchM*std::max(s.upm,1e-3f)||!normalOk){
        bump(D.refusedGap[doorGapBucket(lt.ok,lt.gap/std::max(s.upm,1e-3f))]);if(lt.ok&&lt.gap<=doorTouchM*std::max(s.upm,1e-3f))bump(D.notLeafNormal);
        LeafPalm lp{};lp.r=lt.x;noteDoorReject(s,p.nav,f,p.palm,d.state,lp,PlainOffLeaf,lt.ok?lt.gap/std::max(s.upm,1e-3f)*100.f:-1.f,lt.part,normalOk);
        if(d.state==off::doorStateOpened)bump(D.opened);
        return;}
    if(d.state==0){
        if(!(p.depthM>=.01f))return;
        if(!s.doors.allow(p.nav,now)){bump(D.gated);return;}
        if(s.lev.doorHandPose){ // the leaf opened to the hand's part (its clearance from the command), then held as POSE4
            const float u=std::max(s.upm,1e-3f);const V3 k=openAxisPush(f.L0,f.c0,v::sub(lt.centre,f.o));float target=2.f*pi/180.f;
            const HandBoxes cb=commandBoxes(s,h);
            if(cb.mask&&lt.part>=0){const LeafPalm pc=leafPalm(f,k,cb.c[std::size_t(lt.part)],s.upm);
                if(pc.r>=.5f*leafRMinM*u)target=std::max(target,doorTarget(-1,0.f,pc.delta,partClearance(f.halfZ,lt.ext,pc.r),pi,s.lev.doorRateRadS,std::max(s.doorDt,.011f)));}
            float th{},capOut{};const int r=openPosed(s,p.nav,d,p.palm,target,th,capOut);
            if(r>0){bump(D.closedPoses);maxOf(D.closedFirstMdegMax,std::uint64_t(th*180000.f/pi));
                const DoorView e=viewDoor(s,p.nav);DoorHoldState* slot=nullptr;for(auto& x:s.holds)if(!x.active){slot=&x;break;}
                if(e.ok&&slot&&beginPose4(s,*slot,p.nav,e,f,lt,k,false,h,now)){slot->startState=0;slot->startTheta=0.f;bump(D.pose4Holds);}
                else{DoorHoldState ev{};ev.nav=p.nav;ev.door=d.door;ev.iface=d.iface;ev.kOpen=k;addRested(s,ev,phaseFor(th/std::max(capOut,1e-3f),false),&f,capOut);}}
            DoorHoldState ev{};ev.startState=0;ev.mode=HoldPose4;ev.q=lt.side;ev.partGap=lt.gap;ev.centreGap=centreGap(f,hs.solvedPos);ev.moverHinge=moverHinge(s,f);ev.theta=th;
            noteDoorEvent(s,ev,DoorEndOpen);
            return;}
        engine.doorOpen(reinterpret_cast<void*>(d.door),q,false);bump(D.opens);bump(D.plainOpens);
        DoorHoldState ev{};ev.startState=0;ev.mode=HoldPose4;ev.q=lt.side;ev.partGap=lt.gap;ev.centreGap=centreGap(f,hs.solvedPos);ev.moverHinge=moverHinge(s,f);
        noteDoorEvent(s,ev,DoorEndOpen);
        return;}
    if(d.state==off::doorStateOpened){
        DoorHoldState* slot=nullptr;for(auto& x:s.holds)if(!x.active){slot=&x;break;}
        const float theta=easeOpen(std::clamp(t,0.f,1.f))*std::max(cap,0.f);
        V3 k=openAxisPush(f.L0,f.c0,v::sub(lt.centre,f.o));bool verified=false;
        if(theta>=doorOneDeg){const V3 mk=openAxisMeasured(f.L0,f.L);if(std::fabs(v::dot(mk,f.b))>=.95f){k=mk;verified=true;}}
        if(s.lev.doorRest&&slot&&cap>=0.f&&beginPose4(s,*slot,p.nav,d,f,lt,k,verified,h,now)){bump(D.pose4Holds);return;}
        if(s.lev.doorClose&&engine.doorClose){if(!s.doors.allow(p.nav,now)){bump(D.gated);return;}engine.doorClose(reinterpret_cast<void*>(d.door),false);bump(D.closes);return;}
        bump(D.opened);return;}
    bump(D.busy);
}
// A push on a door no hold owns (the atan swing): state 0 -> Open(palm) (a pivot door is then held), 2 / 3 / 5 -> taken over, 1 -> a buzz,
// 4 -> Close (lever doorclose). A push the leaf test rejects, a door that is not a pivot door or whose hinge is not upright: the plain
// Open at 1 cm, one per second (lever doorplain).
inline void startDoor(State& s,unsigned h,const HandState::DoorPush& p,std::uint64_t now) noexcept {
    auto& D=stats.doors;
    const DoorView d=viewDoor(s,p.nav);if(!d.ok){bump(D.noHandle);return;}
    if(p.contact)bump(D.checksByState[d.state<6?d.state:5]); // contact pushes only (DOOR PRESS candidates without one are pressCands)
    if(followOn(s)){startFollow(s,h,p,d,now);return;}
    const bool pivot=d.vptr==engine.base+off::pivotVptr,anim=d.vptr==engine.base+off::animVptr,swing=swingOn(s);
    std::uint8_t side{};get(d.iface+off::ifSide,side);
    const DoorLocal g=doorLocal(s,p.nav,p.palm);
    bool upright=g.ok&&g.axisUp>=.9f,leaf=false;
    if(g.ok&&!upright)bump(D.geomAxis);
    if(upright){const DoorGeom dg=doorGeom(g.local,g.bmin,g.bmax,g.haveBox,d.state==0?1:(side?-1:1),s.upm);leaf=dg.ok;if(!dg.ok)countGeom(dg.reject,false);}
    alignas(16) const float q[4]{p.palm[0],p.palm[1],p.palm[2],0.f};
    switch(doorAction(d.state,s.lev.doorClose&&groupOk[GroupDoorx]&&engine.doorClose)){
    case DoorDoOpen:{
        if(anim){if(p.depthM>=.02f){if(!s.doors.allow(p.nav,now)){bump(D.gated);return;}engine.doorOpen(reinterpret_cast<void*>(d.door),q,false);bump(D.opens);bump(D.animOpens);}return;}
        if(swing&&pivot&&upright&&leaf){
            if(inGap(s,p.nav,now)){bump(D.gated);return;}
            bool free=false;for(const auto& x:s.holds)free=free||!x.active;
            if(free){engine.doorOpen(reinterpret_cast<void*>(d.door),q,false);bump(D.opens);
                std::uint32_t st{};
                if(get(d.door+off::doorState,st)&&st==2&&startHold(s,p.nav,d,false,false,now))bump(D.swingHolds);
                return;}
        }
        if(!(p.depthM>=.01f)||(upright&&!leaf&&!s.lev.doorPlain))return;
        if(!s.doors.allow(p.nav,now)){bump(D.gated);return;}
        engine.doorOpen(reinterpret_cast<void*>(d.door),q,false);bump(D.opens);bump(D.plainOpens);
        bump(D.plainBy[!swing?PlainSwingOff:!pivot?PlainNotPivot:!upright?PlainNotUpright:!g.haveBox?PlainNoBox:PlainOffLeaf]);return;}
    case DoorBusy:
        if(swing&&pivot&&upright&&leaf&&(d.state==2||d.state==3||d.state==5)&&!inGap(s,p.nav,now)&&startHold(s,p.nav,d,d.state==5,true,now)){bump(D.takeovers);return;}
        bump(D.busy);return;
    case DoorLocked:
        bump(D.locked);
        if(s.lev.haptic&&(!s.lockedPulseNs||now>=s.lockedPulseNs+1'000'000'000ull)&&haptics::pulse(h,.25f,15'000,now,haptics::SourceContact)){s.lockedPulseNs=now;bump(D.lockedPulses);}
        return;
    case DoorDoClose:if(!s.doors.allow(p.nav,now)){bump(D.gated);return;}engine.doorClose(reinterpret_cast<void*>(d.door),false);bump(D.closes);return;
    case DoorOpened:bump(D.opened);return;
    }
}
// An unposable door within the mover's reach: his horizontal distance to the door instance's origin within the bound's horizontal reach
// (x / z of its box) plus the capsule and doorStuckReachM (guarded reads; an upright door assumed).
inline bool doorReach(const State& s,std::uintptr_t nav,V3 mover) noexcept {
    float p[3]{},mx[3]{},mn[3]{};std::uintptr_t arch{},bound{};
    if(!get(nav+off::instPos,p)||!get(nav+off::instArch,arch)||!arch||!get(arch+off::archBound,bound)||!bound||!get(bound+off::boundMax,mx)||!get(bound+off::boundMin,mn))return false;
    const float u=std::max(s.upm,1e-3f),ex=std::max(std::fabs(mx[0]),std::fabs(mn[0])),ez=std::max(std::fabs(mx[2]),std::fabs(mn[2]));
    const V3 un=v::normalized(s.tg.up),d=v::sub(mover,V3{p[0],p[1],p[2]});const V3 flat=v::length(un)>.5f?v::sub(d,v::mul(un,v::dot(d,un))):d;
    const float reach=std::sqrt(ex*ex+ez*ez)+(doorMoverRadiusM+doorStuckReachM)*u;
    return v::finite(flat)&&std::isfinite(reach)&&v::length(flat)<=reach;
}
// The stock Open of a closed door the mover has been at doorStuckMs that no push opened (stuckStep). True = the timer fired.
inline bool stuckOpen(State& s,std::uintptr_t nav,const DoorView& d,V3 mover,bool at,std::uint64_t now) noexcept {
    auto& D=stats.doors;
    if(!at){s.stuck.clear(d.door);return false;} // a slot is taken only by a door he is at
    bump(D.stuckAtTicks);
    if(!stuckStep(s.stuck.of(d.door),true,now,s.lev.doorStuckMs))return false;
    s.stuck.clear(d.door);
    if(!s.doors.allow(nav,now))return true;
    alignas(16) const float q[4]{mover[0],mover[1],mover[2],0.f};engine.doorOpen(reinterpret_cast<void*>(d.door),q,false);
    bump(D.opens);bump(D.stuckOpens);return true;
}
// The mover's push on one door (bodyDoorTarget, the leaf's half thickness included): a closed pivot door with a sure leaf is opened posed
// ahead of him, an open one (state 4, ours or the game's) moved ahead of him either way. A closed door the hook keeps from him that he is
// at (in the stuck band of a posable leaf or at its hinge, within the reach of another) gets the stock Open once no push opened it for
// doorStuckMs; a leaf his closing push holds at the floor for doorFlipMs is shut (the engine's Close), so the closed leaf then opens away
// from him. A door a hand holds, a locked one and the game's own moves (2, 3, 5) are left alone.
inline void bodyDoor(State& s,std::uintptr_t nav,V3 mover,std::uint64_t now,float dt,bool kept) noexcept {
    auto& D=stats.doors;bump(D.bodyCands);
    const DoorView d=viewDoor(s,nav);if(!d.ok)return;
    // DOOR PRESS: a POSE4 hold whose hand did not press this tick takes the mover's push through it (the phase and the hold's own record
    // written together, so the hold stays valid); any other hold keeps the door from him
    DoorHoldState* via=nullptr;
    for(auto& hd:s.holds)if(hd.active&&hd.nav==nav){
        if(s.lev.doorPress&&hd.mode==HoldPose4&&hd.pressNs!=now&&d.state==off::doorStateOpened&&hd.door==d.door){via=&hd;break;}
        bump(D.bodyRefused[4]);s.stuck.clear(d.door);return;}
    if(d.state==1){bump(D.bodyRefused[2]);return;}
    if(d.state!=0&&d.state!=off::doorStateOpened){bump(D.bodyRefused[5]);s.stuck.clear(d.door);return;}
    if(d.state!=0)s.stuck.clear(d.door);
    if(d.state!=off::doorStateOpened)s.bodyFlip.clear(d.door);
    const bool pivot=d.vptr==engine.base+off::pivotVptr;Leaf f{};
    const bool posable=pivot&&d.iface&&readLeaf(s,nav,d.door,f)&&f.sure;
    if(!posable){bump(D.bodyRefused[d.vptr==engine.base+off::animVptr?1u:0u]);
        if(d.state==0)stuckOpen(s,nav,d,mover,kept&&doorReach(s,nav,mover),now);
        return;}
    const float shut=s.lev.doorShutDeg*pi/180.f;
    std::uint8_t side{};float t{},capA{-1.f},capB{-1.f};
    if(!get(d.iface+off::ifSide,side)||!get(d.iface+off::ifPhase,t)||!std::isfinite(t))return;
    get(d.iface+off::ifCap,capA);get(d.iface+off::ifCap+4u,capB);
    const auto pen=[&](const BodyPush& bp){if(std::isfinite(bp.gapM)&&bp.gapM<0.f)maxOf(D.bodyMaxPenUm,std::uint64_t(-bp.gapM*1e6f));};
    if(d.state==0){
        if(inGap(s,nav,now)){s.stuck.clear(d.door);return;}
        const V3 k=openAxisPush(f.L0,f.c0,v::sub(mover,f.o));
        const MoverAngle ma=moverAngle(mover,f.o,f.b,f.L0,k,f.width,s.upm);
        const float capGuess=capA>=0.f&&capB>=0.f?std::min(capA,capB):pi;
        const BodyPush bp=bodyDoorTarget(ma,0.f,capGuess,s.lev.doorRateRadS,dt,shut,s.upm,f.halfZ);
        if(kept&&std::isfinite(bp.gapM))bump(D.bodyGapHist[bodyGapBucket(bp.gapM)]);
        if(stuckOpen(s,nav,d,mover,kept&&((ma.ok&&ma.atHinge)||bp.gapM<=doorStuckBandM),now))return;
        if(!bp.inBand)return;
        bump(D.bodyInBand);pen(bp);
        if(!(bp.target>.5f*pi/180.f)){bump(D.bodyStill);return;}
        if(!s.doors.allow(nav,now))return;
        float th{},cap{};const int r=openPosed(s,nav,d,mover,bp.target,th,cap);
        if(r>0){bump(D.bodyOpens);s.stuck.clear(d.door);DoorHoldState ev{};ev.nav=nav;ev.door=d.door;ev.iface=d.iface;ev.kOpen=k;
            addRested(s,ev,phaseFor(th/std::max(cap,1e-3f),false),&f,cap);
            ev.startState=0;ev.mode=HoldBody;ev.q=-1;ev.theta=th;ev.moverHinge=ma.r;noteDoorEvent(s,ev,DoorEndOpen);}
        return;}
    const float cap=side?capB:capA;if(!(cap>=0.f)){bump(D.bodyRefused[3]);return;}
    const float thetaNow=easeOpen(std::clamp(t,0.f,1.f))*cap;
    V3 k{};RestedDoor* rd=nullptr;for(auto& r:s.rested)if(r.door==d.door){rd=&r;break;}
    if(thetaNow>=doorOneDeg){k=openAxisMeasured(f.L0,f.L);if(std::fabs(v::dot(k,f.b))<.95f)return;}
    else if(via&&v::length(via->kOpen)>.5f)k=via->kOpen;
    else if(rd&&v::length(rd->k)>.5f)k=rd->k;
    else return;
    const MoverAngle ma=moverAngle(mover,f.o,f.b,f.L0,k,f.width,s.upm);
    const BodyPush bp=bodyDoorTarget(ma,thetaNow,cap,s.lev.doorRateRadS,dt,shut,s.upm,f.halfZ);
    // his closing push held at the floor (he walks back into a door that opened toward him): shut, then the closed leaf opens away
    if(via||!(bp.inBand&&bp.atFloor))s.bodyFlip.clear(d.door);
    else if(stuckStep(s.bodyFlip.of(d.door),true,now,doorFlipMs)){
        s.bodyFlip.clear(d.door);
        put<float>(d.iface+off::ifPhase,phaseFor(thetaNow/std::max(cap,1e-3f),true));engine.doorClose(reinterpret_cast<void*>(d.door),false);
        DoorHoldState ev{};ev.nav=nav;ev.door=d.door;ev.iface=d.iface;ev.startState=d.state;ev.mode=HoldBody;ev.q=1;ev.startTheta=thetaNow;ev.theta=thetaNow;
        ev.moverHinge=ma.r;noteDoorEvent(s,ev,DoorEndShut);dropRested(s,d.door);bump(D.bodyFlips);return;}
    if(!bp.inBand)return;
    bump(D.bodyInBand);pen(bp);
    if(!(std::fabs(bp.target-thetaNow)>.05f*pi/180.f)){bump(D.bodyStill);return;}
    const float tOut=phaseFor(bp.target/std::max(cap,1e-3f),false);
    put<float>(d.iface+off::ifPhase,tOut);engine.continueShared(reinterpret_cast<void*>(d.iface),reinterpret_cast<void*>(d.door));
    bump(bp.side<0?D.bodyRaises:D.bodyCloses);bump(D.phaseWrites);
    if(via){via->lastT=tOut;via->theta=bp.target;via->moved=true;bump(D.bodyViaHold);}
    DoorHoldState ev{};ev.nav=nav;ev.door=d.door;ev.iface=d.iface;ev.kOpen=k;addRested(s,ev,tOut,&f,cap);
    if(!rd){ev.startState=d.state;ev.mode=HoldBody;ev.q=bp.side;ev.startTheta=thetaNow;ev.theta=bp.target;ev.moverHinge=ma.r;noteDoorEvent(s,ev,DoorEndOpen);}
}
// The doors the mover may push this tick: the last two the hook kept from him (double doors, 2 s; kept = found within 250 ms), the rested.
// real = the clock the hook stamps (nowNs), now = the tick's frame clock.
inline void bodyDoors(State& s,std::uint64_t now,float dt,std::uint64_t real) noexcept {
    FoundDoor fd{};const bool kept=foundDoor.load(fd)&&fd.why==FindSuppress&&fresh(real,fd.ns,100'000'000ull)&&fd.nav&&fd.door;
    if(kept){if(s.kept[0].door==fd.door)s.kept[0].ns=now;
        else{if(s.kept[1].door==fd.door)std::swap(s.kept[0],s.kept[1]);else s.kept[1]=s.kept[0];s.kept[0]={fd.door,fd.nav,now};}}
    if(!s.lev.doorBody||!followOn(s)||!s.moverOk||s.mode!=ModeOn||!s.tg.onFoot)return;
    std::array<std::uintptr_t,6> navs{};unsigned n=0;
    const auto add=[&](std::uintptr_t nav){if(!nav)return;for(unsigned i=0;i<n;++i)if(navs[i]==nav)return;if(n<navs.size())navs[n++]=nav;};
    const V3 mover=vec3(s.mover+off::moverPos);
    for(const auto& k:s.kept)if(k.nav&&now>=k.ns&&now-k.ns<2'000'000'000ull)add(k.nav);
    // a rested door is judged only with the mover near its swing (its hinge and width are known: no reads for a far one)
    for(const auto& r:s.rested)if(r.nav&&(!(r.width>0.f)||inSwingDisc(mover,r.hinge,r.up,r.width+.5f*s.upm,s.upm)))add(r.nav);
    const auto keptNow=[&](std::uintptr_t nav){for(const auto& k:s.kept)if(k.nav==nav&&now>=k.ns&&now-k.ns<=250'000'000ull)return true;return false;};
    for(unsigned i=0;i<n;++i)bodyDoor(s,navs[i],mover,now,dt,keptNow(navs[i]));
}
// Doors the hands left part-open that an Open asked for (an NPC walking through, a script): back to the engine's opening from their angle
// (SetState(2, false): SetAudioState(2, 4) restarts the loop, Update finishes the opening at the interface's speed).
inline void handBacks(State& s) noexcept {
    auto& D=stats.doors;
    for(auto& x:handBack){const auto door=x.exchange(0,std::memory_order_relaxed);if(!door)continue;
        RestedDoor* r=nullptr;for(auto& y:s.rested)if(y.door==door){r=&y;break;}
        DoorView d{};if(r)d=viewDoor(s,r->nav);
        if(!r||!d.ok||d.door!=door||d.state!=off::doorStateOpened||!engine.doorSetState){bump(D.handBackFails);continue;}
        for(auto& hd:s.holds)if(hd.active&&hd.door==door)hd=DoorHoldState{};
        float sp{};if(!get(d.iface+off::ifSpeed,sp)||!std::isfinite(sp)||!(sp>0.f))put<float>(d.iface+off::ifSpeed,1.f);
        engine.doorSetState(reinterpret_cast<void*>(door),2,false);*r=RestedDoor{};bump(D.handBacks);}
}
// The hook's view for the report: the door it last answered for, sampled on a change of door or reason.
inline void sampleFound(State& s,std::uint64_t real) noexcept {
    FoundDoor fd{};if(!foundDoor.load(fd)||!fresh(real,fd.ns,100'000'000ull)||!fd.door)return;
    if(fd.door==s.findLastDoor&&fd.why==s.findLastWhy)return;
    s.findLastDoor=fd.door;s.findLastWhy=fd.why;
    FindSample x{};x.set=true;x.state=fd.state;x.mode=fd.mode;x.why=fd.why;
    std::uintptr_t iface{},vp{};if(get(fd.door+off::doorIface,iface)&&iface)get(iface,vp);
    x.kind=vp==engine.base+off::pivotVptr?0:vp==engine.base+off::animVptr?1:2;bump(stats.doors.fdKind[x.kind]);
    if(iface){get(iface+off::ifCap,x.cap0);get(iface+off::ifCap+4u,x.cap1);get(iface+off::ifSide,x.sideBits);}
    float np[3]{};if(s.moverOk&&get(fd.nav+off::instPos,np))x.distM=v::length(v::sub(vec3(s.mover+off::moverPos),V3{np[0],np[1],np[2]}))/std::max(s.upm,1e-3f);
    std::uintptr_t arch{},bound{};float mx[3]{},mn[3]{};
    if(get(fd.nav+off::instArch,arch)&&arch&&get(arch+off::archBound,bound)&&bound&&get(bound+off::boundMax,mx)&&get(bound+off::boundMin,mn)){x.heightM=mx[1]-mn[1];x.widthM=mx[0]-mn[0];}
    s.findSamples[s.findNext]=x;s.findNext=(s.findNext+1u)%unsigned(s.findSamples.size());
}
// Once per tick, after both hands: the rested doors, the held doors (the swing with the deeper push of either hand on them, the leaf
// follow with its own hand), then the pushes on other doors, the mover's push, the hand-backs; the hook armed while the hands are live.
inline void doorsTick(State& s,std::uint64_t now) noexcept {
    const float dt=s.doorTickNs&&now>s.doorTickNs?float(double(now-s.doorTickNs)*1e-9):0.f;s.doorTickNs=now;s.doorDt=dt;
    {bool body=false;for(const auto& hs:s.hand)body=body||hs.present;
     const bool armed=s.mode==ModeOn&&s.lev.doors&&!s.lev.doorAuto&&followOn(s)&&groupOk[GroupDoorGate]&&s.tg.onFoot&&s.moverOk&&body;
     const auto real=nowNs(); // the hooks stamp with nowNs
     doorAutoLever.store(s.lev.doorAuto,std::memory_order_relaxed);if(armed)doorArmedNs.store(real?real:1,std::memory_order_relaxed);
     if(s.moverOk)moverPub.store(MoverPub{vec3(s.mover+off::moverPos),real});
     sampleFound(s,real);}
    if(followOn(s))handBacks(s);
    // a hand a leaf follows drives to its command without the creep clip (the leaf keeps the clearance; the next substeps see it)
    const auto markFollow=[&]{for(auto& hs:s.hand)hs.doorFollow=false;
        for(const auto& hd:s.holds)if(hd.active&&hd.mode!=HoldSwing)s.hand[hd.hand&1u].doorFollow=true;};
    if(!swingOn(s))finishHolds(s);
    if(!s.lev.doors){markFollow();return;}
    const bool follow=followOn(s);
    if(follow)restedTick(s);
    const auto deepest=[&](std::uintptr_t nav)->const HandState::DoorPush*{
        const HandState::DoorPush* best=nullptr;
        for(const auto& hs:s.hand)if(hs.push.nav==nav&&(!best||hs.push.depthM>best->depthM))best=&hs.push;
        return best;};
    for(auto& hd:s.holds){if(!hd.active)continue;
        if(hd.mode==HoldSwing)stepHold(s,hd,deepest(hd.nav),dt,now);
        else if(follow)stepFollow(s,hd,dt,now);
        else finishOne(s,hd);}
    for(unsigned h=0;h<2;++h){
        const auto& p=s.hand[h].push;if(!p.nav||deepest(p.nav)!=&p)continue;
        bool held=false;for(const auto& hd:s.holds)held=held||(hd.active&&hd.nav==p.nav);
        if(held)continue;
        startDoor(s,h,p,now);
    }
    bodyDoors(s,now,dt,nowNs());
    markFollow();
    // the doors the hands hold or rested, for the Open / Close hooks
    {unsigned n=0;for(const auto& hd:s.holds)if(hd.active&&n<ourDoors.size())ourDoors[n++].store(hd.door,std::memory_order_relaxed);
     for(const auto& r:s.rested)if(r.door&&n<ourDoors.size())ourDoors[n++].store(r.door,std::memory_order_relaxed);
     for(;n<ourDoors.size();++n)ourDoors[n].store(0,std::memory_order_relaxed);}
}
// ---- GRAB (tick) ----
// A grip press of an open live hand at a prop or a corpse part (a kept pair of a grabclass class within 4 substeps, its nearest part
// within grabcm of the palm centre) holds it: phContactMgr::AttachObjects(mgr = [sim+0x10], tag, B, B, prop, our instance, part, 0, 0,
// false, null) as invLassoWeapon::BuildConstraintToInstance calls it, fixed rotation for a prop, a ball joint for a corpse part, our
// collider's mass / inertia raised to grabmass (SetInertia, the built values back on release). Released on an open grip (debounced), a
// throw, the leash, a constraint that is no longer ours, a deactivated prop, the body leaving LIVE, a class or mode change, a refusal or
// the body's removal; ReleaseConstraint exactly once while the constraint is still ours (its instances and part unchanged).
inline bool grabOn(const State& s) noexcept {
    return s.mode==ModeOn&&s.lev.grab&&groupOk[GroupGrab]&&engine.attach&&engine.releaseConstraint&&engine.fixedRotation&&engine.setInertia;}
inline std::uintptr_t contactMgr(std::uintptr_t sim) noexcept {std::uintptr_t m{};return sim&&get(sim+off::simContactMgr,m)?m:0;}
// An active instance's collider (level +0x78[index], its instance word checked); 0 otherwise.
inline std::uintptr_t activeCollider(const State& s,std::uintptr_t inst) noexcept {
    std::uint16_t index{},count{};std::uintptr_t objects{},word{},cols{},col{},back{};
    if(!s.level||!get(inst+off::instIndex,index)||index==off::noIndex||!get(s.level+off::levelCount,count)||index>=count||
       !get(s.level+off::levelObjects,objects)||!objects||!get(objects+std::uintptr_t(index)*off::objStride+off::objInst,word)||
       (word&~std::uintptr_t(0xf))!=inst||(word&3u)!=0||!get(s.level+off::levelColliders,cols)||!cols||!get(cols+std::uintptr_t(index)*8u,col)||!col||
       !get(col+off::colliderInst,back)||back!=inst)return 0;
    return col;
}
// The constraint still joins our hand to the held part (AttachObjects wrote inst A +0x8, inst B +0x10, comp A +0xb0).
inline bool conOk(std::uintptr_t con,std::uintptr_t prop,std::uintptr_t ours,int part) noexcept {
    std::uintptr_t a{},b{};std::int32_t comp{};
    return con&&get(con+off::conInstA,a)&&a==prop&&get(con+off::conInstB,b)&&b==ours&&get(con+off::conCompA,comp)&&comp==part;
}
inline bool grabConOk(const GrabHold& g,std::uintptr_t ours) noexcept {return conOk(g.con,g.prop,ours,g.comp);}
// GRAB WELD: the rotation constraint still ours (true when the hold has none)
inline bool grabRotOk(const GrabHold& g,std::uintptr_t ours) noexcept {return !g.conRot||conOk(g.conRot,g.prop,ours,g.comp);}
inline void releaseGrab(State& s,unsigned h,std::uint8_t why,bool call) noexcept {
    auto& g=s.grab[h&1u];
    if(s.seat[h&1u].on){s.seat[h&1u]=State::SeatPull{};bump(stats.seat.cancels[3]); // GRAB SEAT: a pull ends with the hold's own ends (mode, body)
        if(!g.active){grablink::setHolding(h,false,0);grablink::setHoldKind(h,grablink::HoldNone);}}
    if(!g.active)return;
    s.geoNearOpen.fill(0);s.geoNearOpenNs.fill(0); // the near ring's open table belongs to the hold
    // each constraint released once while it is still ours: the rotation constraint first, then the position constraint
    if(call&&g.sim&&g.sim==s.sim&&g.mgr==contactMgr(s.sim)){
        if(g.conRot&&conOk(g.conRot,g.prop,instOf(h),g.comp)){
            engine.releaseConstraint(reinterpret_cast<void*>(g.mgr),reinterpret_cast<void*>(g.conRot));bump(stats.grab.rotReleaseCalls);}
        if(grabConOk(g,instOf(h))){engine.releaseConstraint(reinterpret_cast<void*>(g.mgr),reinterpret_cast<void*>(g.con));bump(stats.grab.releaseCalls);}}
    if(g.inertiaSet&&g.sim==s.sim){std::uint16_t index{};
        if(const auto col=verifiedCollider(s.level,instOf(h),index))engine.setInertia(reinterpret_cast<void*>(col),&g.savedInertia[3],quad(g.savedInertia));}
    {auto r=g.rec;const std::uint64_t now=s.lastTickNs>=g.startNs?s.lastTickNs:g.startNs;r.heldMs=(now-g.startNs)/1'000'000ull;r.end=why;
     bump(stats.grab.holdHist[holdBucket(now-g.startNs)]);
     if(g.havePivot&&g.sim==s.sim){float pp[3]{};if(get(g.prop+off::instPos,pp))r.pivotMovedCm=v::length(v::sub(V3{pp[0],pp[1],pp[2]},g.pivot0))*100.f/std::max(s.upm,1e-3f);}
     r.moverSpeedAvg=g.moverN?float(g.moverSum/double(g.moverN)):0.f;r.forceAvgN=g.forceN?float(g.forceSum/double(g.forceN)):0.f;
     r.swingCms=g.swingN?float(g.swingSum/double(g.swingN))*100.f:0.f;
     {std::uint64_t o60=g.over60Ns;if(g.over60Since&&now>g.over60Since)o60+=now-g.over60Since;r.over60Ms=std::uint32_t(std::min<std::uint64_t>(o60/1'000'000ull,0xffffffffull));}
     if(r.set){s.holdRecords[s.holdNext]=r;s.holdNext=(s.holdNext+1u)%unsigned(s.holdRecords.size());}}
    {auto& e=s.holdEnds[h&1u][s.holdEndNext[h&1u]];e=HoldEnd{};e.set=true;e.ns=s.lastTickNs;e.end=why;e.heavy=heavyHold(s,h);
     const auto& o=s.grab[(h&1u)^1u];e.pair=o.active&&o.prop==g.prop;e.partnerEnd=g.rec.partnerEnd;
     e.chord=::rdrvr::runtime::theaterChordStats.consuming.load(std::memory_order_relaxed);
     const unsigned n=std::min<unsigned>(g.histN,6u);e.n=std::uint8_t(n);
     for(unsigned i=0;i<n;++i){const unsigned j=(unsigned(g.histN)+6u-n+i)%6u;e.grip[i]=g.gripHist[j];e.trig[i]=g.trigHist[j];}
     e.handMps=s.lastHandMps[h&1u];e.lagCm=g.lagM*100.f;e.heldMs=s.lastTickNs>=g.startNs?(s.lastTickNs-g.startNs)/1'000'000ull:0;
     {std::uint64_t o60=g.over60Ns;if(g.over60Since&&s.lastTickNs>g.over60Since)o60+=s.lastTickNs-g.over60Since;e.over60Ms=std::uint32_t(std::min<std::uint64_t>(o60/1'000'000ull,0xffffffffull));}
     e.geo=g.geo;e.endNs=s.lastTickNs;
     s.holdEndNext[h&1u]=(s.holdEndNext[h&1u]+1u)%unsigned(s.holdEnds[h&1u].size());}
    {auto& o=s.grab[(h&1u)^1u];if(o.active&&o.prop==g.prop)o.rec.partnerEnd=why;} // the other hand keeps the prop alone
    if(g.rec.fixedRot){ // LIGHT GHOST: the opening hand does not eject the light prop it still overlaps; LIGHT HOLDS ring
        const std::uint64_t now=s.lastTickNs?s.lastTickNs:nowNs();bool ghost=false;
        if(s.lev.grabGhost&&g.sim==s.sim&&lightGhostArm(s.ghosts[h&1u],g.prop,now,GhostRelease)){ghost=true;bump(stats.ghost.starts[GhostRelease]);s.ghostLevel=s.level;}
        auto& r=s.lightHolds[s.lightHoldNext];s.lightHoldNext=(s.lightHoldNext+1u)%unsigned(s.lightHolds.size());
        r=State::LightHold{};r.set=true;r.ns=now;r.heldMs=now>=g.startNs?(now-g.startNs)/1'000'000ull:0;r.end=why;r.ghost=ghost?1:0;r.hand=std::uint8_t(h&1u);
        r.kg=g.propKg;r.propSpeedMax=g.rec.propSpeedMax;r.handMpsAtEnd=s.lastHandMps[h&1u];r.prop=g.prop;r.levelIndex=g.levelIndex;
        r.offsetCm=g.offsetCm;r.maxOffCm=g.maxOffCm;r.endOffCm=g.endOffCm;r.seatD0Cm=g.seatD0Cm;r.seatMs=g.seatMs;r.gripPoint=g.gripPoint;
        r.weld=g.weld;r.flagsPos=g.flagsPos;r.flagsRot=g.flagsRot;r.slipCm=g.slipCm;r.offTrace=g.offTrace;r.capContacts=g.capContacts;r.capDepthMm=g.capDepthMm;
        if(g.sim==s.sim)if(const auto col=activeColliderAt(s.level,g.levelIndex,g.prop)){const float ps=v::length(vec3(col+off::colliderVel))/std::max(s.upm,1e-3f);
            if(std::isfinite(ps)){r.propMpsAtEnd=ps;r.afterMaxMps=ps;}}
        r.watchUntil=now+300'000'000ull;}
    bump(stats.grab.ends[why<GrabEndCount?why:GrabEndMode]);grablink::lastEnd[h&1u].store(why,std::memory_order_relaxed);
    g=GrabHold{};grablink::setHolding(h,false,0);grablink::setHoldKind(h,grablink::HoldNone);
}
// A composite whose parts can all become active (phBoundComposite::CanBecomeActive's own test: no BVH part, every part's CanBecomeActive
// one of the five that return true), up to 24 parts; the type's composite +0x130 (guarded reads).
inline bool compositeActivatable(std::uintptr_t type,std::uint8_t& d4,unsigned& count) noexcept {
    const auto b=engine.base;std::uintptr_t comp{},compVt{},slot{},list{};std::uint8_t ctype{};std::uint16_t n{};d4=1;count=0;
    if(!type||!get(type+off::typeComposite,comp)||!comp||!get(comp+off::boundType,ctype)||ctype!=off::boundComposite||!get(comp,compVt)||!compVt||
       !get(compVt+off::boundCanActive,slot)||slot!=b+off::compCanActive||!get(comp+off::compBvh,d4)||!get(comp+off::compPartCount,n)||!get(comp+off::compParts,list)||!list)return false;
    count=n;if(d4||n<1||n>24)return false;
    std::array<std::uintptr_t,24> parts{};if(!engine.read(nullptr,list,parts.data(),n*8u))return false;
    for(unsigned i=0;i<n;++i){if(!parts[i])continue;std::uintptr_t pv{},ps{};if(!get(parts[i],pv)||!pv||!get(pv+off::boundCanActive,ps))return false;
        bool ok=false;for(const auto f:off::partCanActive)ok=ok||ps==b+f;if(!ok)return false;}
    return true;
}
// Furniture whose type never moves (m_MinMoveForce < 0: FindBreakStrength 0, ReleaseProp refuses it, IsPropFixed true) held on purpose:
// fragInst::PrepareForActivation 0x1ea1ef8 never reads +0x1c0, so the release (+0x80 bit 1), the type lock lifted (SetDisableActivation)
// and a wake activate it; fragInstNav::ReportMovedBySim 0x286ef00 rebuilds a moved nav obstacle. Never with a creature body near.
inline GrabMoveWhy grabMoveGate(State& s,std::uintptr_t inst,const ReleaseIn& r) noexcept {
    GrabMoveIn g{};g.on=s.lev.grabMove&&actOn(s)&&s.lev.release&&groupOk[GroupRelease]&&engine.isActDisabled&&engine.setDisableAct;
    g.nav=navInst(inst);std::uint8_t door{},inf{};get(inst+off::navDoor,door);get(inst+off::fragInfinite,inf);g.door=door==1;g.infinite=(inf&0x80u)!=0;
    g.never=(r.flags12&0x8u)!=0;
    std::uintptr_t arch{};if(get(inst+off::instArch,arch)&&arch)get(arch+off::archMass,g.massKg);
    V3 c{};float rad{-1.f};checkProp(s,inst,c,rad);g.radiusM=rad/std::max(s.upm,1e-3f);
    unsigned parts{};g.composite=compositeActivatable(fragTypeOf(inst),g.compD4,parts);g.partsActive=g.composite;
    // a creature's bone capsule within grabMoveNpcM of the prop's level sphere (VrMelee's virtual bodies)
    if(s.vbFresh&&rad>0.f)for(std::uint32_t i=0;i<std::min<std::uint32_t>(s.vb.count,std::uint32_t(vbodyMax))&&!g.npcNear;++i){const auto& b=s.vb.b[i];
        if(!b.count||v::length(v::sub(b.centre,c))>b.radius+rad+grabMoveNpcM*s.upm)continue;
        for(unsigned k=0;k<std::min<unsigned>(b.count,unsigned(vbodyCaps));++k){const auto& cp=b.c[k];
            if(v::length(v::sub(c,closestOnSegment(cp.a,cp.b,c)))<cp.r+rad+grabMoveNpcM*s.upm){g.npcNear=true;break;}}}
    return grabMoveWhy(g,s.lev.grabMoveKg,s.lev.grabMoveRM);
}
// A prop no grab can make physical (0 = it may be; else the GrabMiss reason).
inline std::uint8_t grabNever(State& s,std::uintptr_t inst) noexcept {
    std::uintptr_t vt{},fn{};if(!get(inst,vt)||!vt||!get(vt+0x10,fn))return 0;
    if(fn!=engine.navClassFn&&fn!=engine.deadClassFn)return 0;
    std::uint8_t inf{};if(get(inst+off::fragInfinite,inf)&&(inf&0x80u))return MissInfinite;
    if(fn!=engine.navClassFn)return 0;
    ReleaseIn r{};if(!releaseInputs(inst,r))return 0;
    if(!(r.f80&0x2u)&&(!r.typeOk||!std::isfinite(r.minMove)||r.minMove<0.f)){
        // furniture whose type never moves: a deliberate grab may release it (grabMoveWhy); the verdict is cached with the parts, so a
        // creature near it (it changes every frame) leaves it a candidate: startGrab judges the gate again
        const GrabMoveWhy why=grabMoveGate(s,inst,r);bump(stats.grab.moveBy[why]);
        return why==GrabMoveOk||why==GrabMoveNpcNear?0:MissNeverMove;}
    if(r.flags12&0x8u){UnfreezeIn u{};UnfreezeSample x{};std::uintptr_t g{};unfreezeInputs(inst,u,x,g);
        V3 c{};float rad{-1.f};checkProp(s,inst,c,rad);u.radiusM=rad/std::max(s.upm,1e-3f);if(u.compRadiusM>0.f)u.compRadiusM/=std::max(s.upm,1e-3f);
        if(!unfreezeOn(s)||unfreezePropWhy(u,s.lev.unfreezeRM,s.lev.unfreezeKg,s.lev.unfreezeParts)!=UnfreezeOk)return MissNeverAct;}
    return 0;
}
// The parts of a bound (a composite: up to 24 parts, their matrices in the instance's frame; another bound: part 0) and the hand's grab
// points in the instance's frame -> the nearest part. The part list and boxes are read once per (instance, archetype, bound) and kept
// per hand (4 entries); every call reads the instance's archetype and matrix in one read (+0x8..+0x60), the bound word and a
// composite's part matrices (an articulated corpse moves its parts).
// propParts: the hand's cache entry of the instance with this call's part matrices and the instance matrix (m); null when unreadable.
// judgeNever = an entry takes grabNever's verdict when made or first read so (a held prop's geometric hit skips it: the prop is held); a
// never entry keeps its part matrices unread on the grab paths (the geometric hit always reads this tick's).
inline GrabCache* propParts(State& s,unsigned h,std::uintptr_t inst,float (&m)[16],bool judgeNever=true) noexcept {
    unsigned char blk[0x58]{};std::uintptr_t arch{},bound{};
    if(!engine.read||!inst||!engine.read(nullptr,inst+off::instArch,blk,sizeof blk))return nullptr;
    std::memcpy(&arch,blk,8);std::memcpy(m,blk+(off::instMatrix-off::instArch),64);
    if(!arch||!get(arch+off::archBound,bound)||!bound)return nullptr;
    auto& cache=s.grabCache[h&1u];GrabCache* c=nullptr;
    for(auto& x:cache)if(x.inst==inst&&x.arch==arch&&x.bound==bound){c=&x;break;}
    if(!c){c=&cache[0];for(auto& x:cache)if(x.used<c->used)c=&x;
        *c=GrabCache{};bump(stats.grab.cacheFills);std::uint8_t type{};if(!get(bound+off::boundType,type))return nullptr;
        if(type==off::boundComposite){
            std::uintptr_t list{},mats{};std::uint16_t cnt{};
            if(!get(bound+off::compParts,list)||!list||!get(bound+off::compMatrices,mats)||!mats||!get(bound+off::compCount,cnt))return nullptr;
            const unsigned count=std::min<unsigned>(cnt,unsigned(c->parts.size()));std::array<std::uintptr_t,24> pb{};
            if(!count||!engine.read(nullptr,list,pb.data(),count*8u))return nullptr;
            for(unsigned i=0;i<count;++i){float box[8]{};
                if(!pb[i]||!get(pb[i]+off::boundMax,box)){c->parts[i].bmin={1,1,1};c->parts[i].bmax={0,0,0};continue;}
                c->parts[i].bmax={box[0],box[1],box[2]};c->parts[i].bmin={box[4],box[5],box[6]};}
            c->mats=mats;c->count=count;
        }else{float box[8]{};if(!get(bound+off::boundMax,box))return nullptr;
            GrabPart& p=c->parts[0];p=GrabPart{};p.m[0]=p.m[5]=p.m[10]=p.m[15]=1.f;p.bmax={box[0],box[1],box[2]};p.bmin={box[4],box[5],box[6]};c->count=1;}
        c->inst=inst;c->arch=arch;c->bound=bound;c->never=judgeNever?grabNever(s,inst):0;c->judged=judgeNever;}
    else if(judgeNever&&!c->judged){c->never=grabNever(s,inst);c->judged=true;} // an entry the geometric hit made: judged at its first grab read
    c->used=s.step+1; // a never verdict is kept like any other entry (judging one again costs its whole set of reads)
    if(c->never&&judgeNever)return c;
    if(c->mats){alignas(16) float mm[24][16];
        if(!engine.read(nullptr,c->mats,mm,c->count*64u)){*c=GrabCache{};return nullptr;}
        for(unsigned i=0;i<c->count;++i)std::memcpy(c->parts[i].m,mm[i],64);}
    return c;
}
inline NearPart nearestPartOf(State& s,unsigned h,std::uintptr_t inst,const std::array<V3,3>& pts) noexcept {
    NearPart n{};float m[16]{};const GrabCache* c=propParts(s,h,inst,m);if(!c)return n;
    if(c->never){n.never=c->never;return n;}
    for(unsigned k=0;k<pts.size();++k){NearPart q=nearestPart(c->parts.data(),c->count,toLocal(m,pts[k]));if(q.index>=0&&q.dist<n.dist){q.pt=std::int8_t(k);n=q;}}
    return n;
}
inline void noteGrabSample(State& s,std::uintptr_t inst,int cls) noexcept {
    GrabSample g{};g.cls=cls;std::uintptr_t arch{};if(get(inst+off::instArch,arch)&&arch)get(arch+off::archType,g.archType);
    ReleaseIn r{};if(cls==classNav&&releaseInputs(inst,r))g.typeBits=r.typeBits;
    for(auto& x:s.grabSamples){if(x.count&&x.cls==g.cls&&x.archType==g.archType&&x.typeBits==g.typeBits){++x.count;return;}
        if(!x.count){x=g;x.count=1;return;}}
}
// This tick's grab candidate of a hand, open or closing: the holsters decide on the press in the first frame the grip reaches 0.65, from
// a ready stamp of an earlier tick, so the stamp must exist before the grip moves. The level state is checked at the start of the grab.
inline void grabCandidate(State& s,unsigned h,std::uint64_t now) noexcept {
    auto& hs=s.hand[h];const auto& t=s.tg.hand[h];s.grabCand[h]=0;s.grabCandPart[h]=-1;s.grabCandCls[h]=-1;s.grabCandPt[h]=-1;
    if(!hs.present||hs.machine.state!=BodyLive||!hs.haveSolved||!t.valid||t.cls!=ClassBody)return;
    const float u=std::max(s.upm,1e-3f);float best=1e30f;auto& G=stats.grab;
    const std::array<V3,3> pts=grabPoints(hs.solvedPos,hs.solvedRows,u);
    // TWO HANDS (join): the other hand holds a ball-jointed prop or a corpse alone; this hand's grab points within grabJoinM of its nearest
    // part make it this hand's candidate ahead of any other (no kept contact pair needed: the prop is known and held). The distance is
    // published every tick (grablink::setHeld: the holsters mask a free hand within heavymaskcm of held heavy furniture)
    {const auto& o=s.grab[h^1u];const auto ok=grablink::heldKind(h^1u);
     if(o.active&&o.sim==s.sim&&(ok==grablink::HoldHeavy||ok==grablink::HoldCorpse)){
        const NearPart np=nearestPartOf(s,h,o.prop,pts);
        const float dM=np.index>=0?np.dist/u:-1.f;grablink::setHeld(h,dM,now);
        if(np.index>=0){const float cm=dM*100.f;bump(G.joinDistHist[!(cm<35.f)?5u:cm<4.f?0u:cm<6.f?1u:cm<10.f?2u:cm<20.f?3u:4u]);}
        if(s.lev.grab2&&np.index>=0&&grablink::joinCandidate(ok,dM,s.lev.grabJoinM)){
            s.grabCand[h]=o.prop;s.grabCandPart[h]=np.index;s.grabCandCls[h]=o.cls;grablink::setJoin(h,true,now,dM);bump(G.joinReadyTicks);
            noteGrabSample(s,o.prop,o.cls);return;}}}
    for(const auto& c:hs.grabCands){
        if(!c.inst||s.step<c.step||s.step-c.step>4)continue;
        bool own=false;for(int i=0;i<s.ownCount;++i)own=own||s.own[std::size_t(i)]==c.inst;
        if(own){bump(G.misses[MissOwn]);continue;}
        const NearPart np=nearestPartOf(s,h,c.inst,pts);
        if(np.never){bump(G.misses[np.never<MissCount?np.never:MissNoParts]);continue;}
        if(np.index<0){bump(G.misses[MissNoParts]);continue;}
        if(!(np.dist<=s.lev.grabM*u)){bump(G.misses[MissFar]);bump(G.missDist[missDistBucket(np.dist/u)]);
            bump(G.missSqueeze[!std::isfinite(t.squeeze)||t.squeeze<grabOpenSqueeze?0u:t.squeeze<fistOn?1u:2u]);continue;}
        if(np.dist>=best)continue;
        best=np.dist;s.grabCand[h]=c.inst;s.grabCandPart[h]=np.index;s.grabCandCls[h]=c.cls;s.grabCandPt[h]=np.pt;}
    if(s.grabCand[h])noteGrabSample(s,s.grabCand[h],s.grabCandCls[h]);
}
inline void noteGrabRefuse(State& s,std::uintptr_t prop,int cls,GrabRefuse why) noexcept {
    GrabRefuseSample r{};r.why=std::uint8_t(why);r.cls=cls;r.set=true;r.stateAfter=prop?otherState(s.level,prop):-1;
    if(prop){std::uintptr_t arch{};if(get(prop+off::instArch,arch)&&arch)get(arch+off::archType,r.archType);
        if(cls==classNav||cls==classDeadBody){get(prop+off::fragF80,r.f80);get(prop+off::instFlags,r.flags12);ReleaseIn ri{};if(releaseInputs(prop,ri))r.typeBits=ri.typeBits;}}
    s.grabRefuses[s.grabRefuseNext]=r;s.grabRefuseNext=(s.grabRefuseNext+1u)%unsigned(s.grabRefuses.size());
}
inline void grabRefuse(State& s,unsigned h,GrabRefuse why,std::uint64_t now,std::uintptr_t prop=0,int cls=-1) noexcept {
    bump(stats.grab.refused[why]);auto& hs=s.hand[h];noteGrabRefuse(s,prop,cls,why);s.grabLastRefuse[h&1u]=std::uint8_t(why);
    if(s.lev.haptic&&(!hs.grabBuzzNs||now>=hs.grabBuzzNs+300'000'000ull)&&haptics::pulse(h,haptics::refuseAmplitude,haptics::refuseUs,now,haptics::SourceRefuse))hs.grabBuzzNs=now;
}
inline bool seatStart(State& s,unsigned h,std::uintptr_t prop,int part,int cls,std::uintptr_t propCol,std::uint64_t now) noexcept;
inline void attachHold(State& s,unsigned h,std::uint64_t now,std::uintptr_t prop,int part,int cls,std::uintptr_t propCol,float propKg,std::int32_t colType,bool moved,
                       const State::SeatPull* seat) noexcept;
inline void startGrab(State& s,unsigned h,std::uint64_t now) noexcept {
    auto& G=stats.grab;
    if(!grabOn(s)){grabRefuse(s,h,GrabGroup,now);return;}
    if(s.seat[h&1u].on)return; // a pull into the palm is on: its attach follows
    auto prop=s.grabCand[h];int part=s.grabCandPart[h],cls=s.grabCandCls[h];
    // the ready hold-over: no candidate this tick, the last one within readyMaxNs, its part again within grabcm of the hand
    if(!prop&&s.grabLast[h].inst&&now>=s.grabLast[h].ns&&now-s.grabLast[h].ns<=grablink::readyMaxNs){
        const auto& hs=s.hand[h];const float u=std::max(s.upm,1e-3f);
        const NearPart np=hs.haveSolved?nearestPartOf(s,h,s.grabLast[h].inst,grabPoints(hs.solvedPos,hs.solvedRows,u)):NearPart{};
        const auto& o=s.grab[h^1u];const bool joinProp=s.lev.grab2&&o.active&&o.prop==s.grabLast[h].inst; // the join's prop: its own distance
        if(np.index>=0&&np.dist<=(joinProp?std::max(s.lev.grabM,s.lev.grabJoinM):s.lev.grabM)*u){prop=s.grabLast[h].inst;part=np.index;cls=s.grabLast[h].cls;bump(G.holdOver);}
        else{bump(G.holdOverFar);grabRefuse(s,h,GrabFar,now,s.grabLast[h].inst,s.grabLast[h].cls);return;}}
    if(!prop||part<0){grabRefuse(s,h,GrabNoCand,now);return;}
    std::uint16_t ourIndex{};const auto ourCol=verifiedCollider(s.level,instOf(h),ourIndex);
    if(!ourCol){grabRefuse(s,h,GrabStale,now,prop,cls);return;}
    if(cls==classNav||cls==classDeadBody){std::uint8_t inf{};if(!get(prop+off::fragInfinite,inf)||(inf&0x80u)){grabRefuse(s,h,GrabInfinite,now,prop,cls);return;}}
    int st=otherState(s.level,prop);
    if(st<0){grabRefuse(s,h,GrabStale,now,prop,cls);return;}
    std::uint8_t relWhy=0xff;bool moved=false;std::uint32_t movedBefore{};
    if(st==1&&actOn(s)&&!s.wakes.recent(prop,now)){ // an inactive prop: woken first (a fixed root released as the palm's press does)
        std::uint16_t f12{};
        if(navInst(prop)&&get(prop+off::instFlags,f12)&&(f12&0x8u)){ // never-activate: UNFREEZE's gates, then its wake
            V3 c{};float rad{-1.f};checkProp(s,prop,c,rad);if(unfreezeOn(s))unfreezeProp(s,h,prop,rad,now,UfGrab);}
        else{
            if(navInst(prop)&&s.lev.release&&groupOk[GroupRelease]){ReleaseIn r{};
                V3 c{};float rad{-1.f}; // the level sphere; unread (not PropOk) = no release
                if(checkProp(s,prop,c,rad)==PropOk&&releaseInputs(prop,r)){r.radius=rad/std::max(s.upm,1e-3f);r.depth=1.f;r.live=true;r.age=0;
                    std::uint32_t f80{};const ReleaseWhy rw=releaseWanted(r,std::max(s.lev.releaseRM,s.lev.grabReleaseRM),0.f);relWhy=std::uint8_t(rw);
                    if(rw==ReleaseOk&&get(prop+off::fragF80,f80)&&f80==r.f80){put<std::uint32_t>(prop+off::fragF80,f80|0x2u);bump(stats.props.released);}
                    // furniture whose type never moves (also type-locked: the lock is lifted)
                    else if((rw==ReleaseNeverMove||rw==ReleaseTypeLocked)&&(!std::isfinite(r.minMove)||r.minMove<0.f)&&r.typeOk&&grabMoveGate(s,prop,r)==GrabMoveOk&&
                            get(prop+off::fragF80,f80)&&f80==r.f80){
                        movedBefore=f80;put<std::uint32_t>(prop+off::fragF80,f80|0x2u);moved=true;bump(G.moveReleased);
                        if(engine.isActDisabled(reinterpret_cast<const void*>(prop)))engine.setDisableAct(reinterpret_cast<void*>(prop),false);}}}
            wakeProp(s,prop,now);}
        st=otherState(s.level,prop);
        if(moved){bump(st==0?G.moveActivated:G.moveRefusedAfter);
            const MovedProp e{prop,movedBefore,now};
            if(st!=0){if(restoreMoved(e))bump(G.moveRolledBack);} // the wake failed: the furniture as it was
            else{MovedProp* slot=nullptr;for(auto& x:s.moved)if(x.inst==prop||!x.inst){slot=&x;break;}
                if(slot){if(slot->inst!=prop)*slot=e;else slot->touchNs=now;}
                else if(restoreMoved(e)){bump(G.moveRolledBack);bump(G.moveFull);grabRefuse(s,h,GrabInactive,now,prop,cls);return;}}}}
    if(st!=0){grabRefuse(s,h,GrabInactive,now,prop,cls);s.grabRefuses[(s.grabRefuseNext+3u)%4u].releaseWhy=relWhy;return;}
    const auto propCol=activeCollider(s,prop);float propKg{-1.f};std::int32_t colType{-1};
    if(propCol){get(propCol+off::colliderMass,propKg);get(propCol+off::colliderType,colType);}
    if(cls==classNav&&propCol&&std::isfinite(propKg)&&propKg>s.lev.grabMaxKg){grabRefuse(s,h,GrabHeavy,now,prop,cls);return;}
    // the other hand holds this prop: a second hand only on a ball-jointed hold (two fixed-rotation links would fight each other)
    {const auto& o=s.grab[(h&1u)^1u];
     if(o.active&&o.prop==prop){const bool fixedWould=cls!=classDeadBody&&s.lev.grabRot&&std::isfinite(propKg)&&propKg>0.f&&propKg<=s.lev.grabRotKg;
        if(o.rec.fixedRot||fixedWould){grabRefuse(s,h,GrabPair,now,prop,cls);return;}}
     // GRAB SEAT: a prop the other hand is pulling in is a fixed-rotation hold to come
     if(s.seat[(h&1u)^1u].on&&s.seat[(h&1u)^1u].prop==prop){grabRefuse(s,h,GrabPair,now,prop,cls);return;}}
    // GRAB SEAT: a light prop whose grip point is off the palm centre is pulled in first (seatTick attaches it)
    {const bool fixedWould=cls==classNav&&s.lev.grabRot&&std::isfinite(propKg)&&propKg>0.f&&propKg<=s.lev.grabRotKg;
     if(fixedWould&&s.lev.grabSeat&&propCol&&seatStart(s,h,prop,part,cls,propCol,now))return;}
    attachHold(s,h,now,prop,part,cls,propCol,propKg,colType,moved,nullptr);
}
// The hold itself (AttachObjects at the palm centre for both instances, fixed rotation for a light prop, the ball grip, the record, the held
// hand's inertia). seat = the pull that brought the prop into the palm (null: attached where it was).
inline void attachHold(State& s,unsigned h,std::uint64_t now,std::uintptr_t prop,int part,int cls,std::uintptr_t propCol,float propKg,std::int32_t colType,bool moved,
                       const State::SeatPull* seat) noexcept {
    auto& G=stats.grab;auto& g=s.grab[h];
    std::uint16_t ourIndex{};const auto ourCol=verifiedCollider(s.level,instOf(h),ourIndex);
    if(!ourCol){grabRefuse(s,h,GrabStale,now,prop,cls);return;}
    const auto mgr=contactMgr(s.sim);if(!mgr){grabRefuse(s,h,GrabPool,now,prop,cls);return;}
    const V3 b=s.hand[h].solvedPos;alignas(16) const float pos[4]{b[0],b[1],b[2],0.f};
    const auto t0=nowNs();
    void* c=engine.attach(reinterpret_cast<void*>(mgr),grabTag,pos,pos,reinterpret_cast<void*>(prop),reinterpret_cast<void*>(instOf(h)),part,0,0.f,false,nullptr);
    maxOf(G.attachNsMax,nowNs()-t0);
    if(!c){grabRefuse(s,h,GrabPool,now,prop,cls);return;}
    GrabHold x{};x.active=true;x.prop=prop;x.con=addr(c);x.mgr=mgr;x.sim=s.sim;x.comp=part;x.cls=cls;x.startNs=now;
    std::uintptr_t manifold{};
    if(!get(x.con+off::conManifold,manifold)||!manifold||!grabConOk(x,instOf(h))){
        if(grabConOk(x,instOf(h)))engine.releaseConstraint(reinterpret_cast<void*>(mgr),c);
        grabRefuse(s,h,GrabNoManifold,now,prop,cls);return;}
    // fixed rotation for a light prop; a corpse part and a heavy prop (a chair) hang on a ball joint and trail behind the grip
    const bool fixedRot=cls!=classDeadBody&&s.lev.grabRot&&std::isfinite(propKg)&&propKg>0.f&&propKg<=s.lev.grabRotKg;
    get(x.con+off::conFlags,x.flagsPos);
    if(fixedRot){
        // GRAB WELD: the position constraint keeps flags 2 (phConstraint::Update's position impulse needs (flags & 3) == 2), a second
        // constraint on the same pair takes the rotation lock (AttachDraftActor: AttachObjects, then AttachObjectsRotation); a refused
        // second attach keeps the position constraint alone (the prop stays at the palm point, free to turn about it)
        if(s.lev.grabWeld&&groupOk[GroupWeld]){
            void* c2=engine.attach(reinterpret_cast<void*>(mgr),grabTag,pos,pos,reinterpret_cast<void*>(prop),reinterpret_cast<void*>(instOf(h)),part,0,0.f,false,nullptr);
            std::uintptr_t m2{};
            if(c2&&get(addr(c2)+off::conManifold,m2)&&m2&&conOk(addr(c2),prop,instOf(h),part)){
                engine.fixedRotation(c2);x.conRot=addr(c2);x.weld=2;get(x.conRot+off::conFlags,x.flagsRot);bump(G.welds);}
            else{if(c2&&conOk(addr(c2),prop,instOf(h),part))engine.releaseConstraint(reinterpret_cast<void*>(mgr),c2);x.weld=1;bump(G.rotRefused);}}
        else{engine.fixedRotation(c);get(x.con+off::conFlags,x.flagsPos);x.weld=0;bump(G.rotOnly);}}
    x.propCol=propCol;x.propKg=std::isfinite(propKg)?propKg:-1.f;x.colType=colType;get(prop+off::instIndex,x.levelIndex);
    // a ball-jointed rigid prop with the grip drive: the grip (the joint's point, the hand's centre) in the collider's frame
    if(s.lev.grabGrip&&!fixedRot&&cls==classNav&&propCol&&(colType==0||colType==2)&&x.propKg>0.f){
        float rr[12]{},oo[4]{};
        if(engine.read(nullptr,propCol+off::colliderRows,rr,sizeof rr)&&engine.read(nullptr,propCol+off::colliderPos,oo,sizeof oo)){
            const Rows r{V3{rr[0],rr[1],rr[2]},V3{rr[4],rr[5],rr[6]},V3{rr[8],rr[9],rr[10]}};const V3 local=toFrame(r,V3{oo[0],oo[1],oo[2]},b);
            if(v::finite(local)&&v::length(local)<5.f*std::max(s.upm,1e-3f)){x.gripLocal=local;x.ball=true;}}}
    {std::uintptr_t arch{},bound{};std::uint8_t bt{};std::uint16_t n{};
     if((colType==1||colType==3)&&get(prop+off::instArch,arch)&&arch&&get(arch+off::archBound,bound)&&bound&&get(bound+off::boundType,bt)&&bt==off::boundComposite&&
        get(bound+off::compCount,n))x.artParts=int(n);}
    {auto& r=x.rec;r.set=true;r.cls=cls;r.massKg=x.propKg;r.colType=colType;r.fixedRot=fixedRot;r.move=moved;r.ball=x.ball;
     std::uintptr_t arch{};if(get(prop+off::instArch,arch)&&arch)get(arch+off::archType,r.archType);
     if(cls==classNav){ReleaseIn ri{};if(releaseInputs(prop,ri)){r.typeBits=ri.typeBits;r.minMove=ri.minMove;}tuneName(fragTypeOf(prop),r.tune);}
     V3 cc{};float rad{-1.f};checkProp(s,prop,cc,rad);r.radiusCm=rad*100.f/std::max(s.upm,1e-3f);
     for(const auto& gc:s.grabCache[h])if(gc.inst==prop){r.parts=int(gc.count);break;}
     float pp[3]{};if(get(prop+off::instPos,pp)){x.pivot0={pp[0],pp[1],pp[2]};x.havePivot=true;}}
    if(engine.read(nullptr,ourCol+off::colliderInertia,x.savedInertia,16)&&std::isfinite(x.savedInertia[3])&&x.savedInertia[3]>0.f){
        const float k=s.lev.grabMassKg/x.savedInertia[3];alignas(16) const float in[4]{x.savedInertia[0]*k,x.savedInertia[1]*k,x.savedInertia[2]*k,0.f};
        const float mass=s.lev.grabMassKg;engine.setInertia(reinterpret_cast<void*>(ourCol),&mass,quad(in));x.inertiaSet=true;}
    // LIGHT GHOST: the hold supersedes a ghost of this hand on its prop (a fixed-rotation hold arms a new one when it ends)
    for(auto& gh:s.ghosts[h&1u])if(gh.inst&&gh.inst==prop){bump(stats.ghost.grabbedWhileGhost);gh=LightGhost{};}
    // GRAB SEAT / light holds: the prop's centre to the palm centre now, the pull that seated it
    if(fixedRot&&propCol){const float u=std::max(s.upm,1e-3f);const V3 pc=vec3(propCol+off::colliderPos);const float d=v::length(v::sub(pc,b))/u*100.f;
        if(std::isfinite(d)){x.offsetCm=d;x.maxOffCm=d;x.endOffCm=d;x.offTrace[0]=d;}
        if(s.hand[h].haveSolved){const V3 l0=toFrame(s.hand[h].solvedRows,b,pc);if(v::finite(l0)){x.local0=l0;x.haveLocal0=true;x.slipCm=0.f;}}}
    x.gripPoint=seat?seat->pt:s.grabCandPt[h&1u];
    if(seat){x.seatD0Cm=seat->d0*100.f;x.seatMs=now>=seat->startNs?float(double(now-seat->startNs)/1e6):0.f;}
    g=x;grablink::setHolding(h,true,now);bump(G.starts);
    grablink::setHoldKind(h,cls==classDeadBody?grablink::HoldCorpse:fixedRot?grablink::HoldLight:grablink::HoldHeavy);
    if(s.lev.haptic)haptics::pulse(h,.4f,25'000,now,haptics::SourceGrab);
}
// GRAB SEAT: the pull armed (true) for a light prop whose grip point is more than seatDoneM off the palm centre; the hold flags set (the
// holsters see a light hold), a ghost on the prop (the palm does not push it as it comes in). False = attach now (within seatDoneM, or
// the parts / collider unreadable).
inline bool seatStart(State& s,unsigned h,std::uintptr_t prop,int part,int cls,std::uintptr_t propCol,std::uint64_t now) noexcept {
    auto& K=stats.seat;const auto& hs=s.hand[h&1u];const float u=std::max(s.upm,1e-3f);
    if(!hs.haveSolved||part<0){bump(K.refused);return false;}
    float m[16]{};const GrabCache* c=propParts(s,h,prop,m,false);
    if(!c||unsigned(part)>=c->count){bump(K.refused);return false;}
    const V3 b=hs.solvedPos;const SeatPoint sp=seatPoint(c->parts[std::size_t(part)],m,b);if(!sp.ok){bump(K.refused);return false;}
    const float d0=sp.dist/u;if(d0<=seatDoneM){bump(K.nearSkips);return false;}
    const GripNow g0=gripNow(propCol,V3{0,0,0});std::uint16_t li{};if(!g0.ok||!get(prop+off::instIndex,li)){bump(K.refused);return false;}
    const V3 local=toFrame(g0.r,g0.com,sp.world);if(!v::finite(local)||v::length(local)>2.f*u){bump(K.refused);return false;}
    auto& x=s.seat[h&1u];x=State::SeatPull{};x.on=true;x.prop=prop;x.col=propCol;x.sim=s.sim;x.levelIndex=li;x.part=part;x.cls=cls;x.pt=s.grabCandPt[h&1u];
    x.seatLocal=local;x.startNs=now?now:1;x.d0=d0;
    grablink::setHolding(h,true,now);grablink::setHoldKind(h,grablink::HoldLight);
    if(s.lev.grabGhost&&lightGhostArm(s.ghosts[h&1u],prop,now,GhostSeat)){bump(stats.ghost.starts[GhostSeat]);s.ghostLevel=s.level;}
    bump(K.starts);bump(K.d0CmHist[seatCmBucket(d0*100.f)]);return true;}
// GRAB SEAT, every tick while pulling: cancel (the hand opened, the prop gone or asleep, the mode / lever / hand gone: the hold flags off,
// the ghost ends on its own quiet rule), attach once seated or at the timeout.
inline void seatTick(State& s,unsigned h,std::uint64_t now) noexcept {
    auto& sp=s.seat[h&1u];auto& K=stats.seat;const auto& hs=s.hand[h&1u];const auto& t=s.tg.hand[h&1u];const float u=std::max(s.upm,1e-3f);
    grablink::stampHolding(h,now);grablink::setReady(h,false,now);s.grabSerial[h&1u]=grablink::pressSerial[h&1u].load(std::memory_order_acquire);
    const bool modeOk=grabOn(s)&&s.lev.grabSeat&&hs.present&&hs.machine.state==BodyLive&&hs.haveSolved&&t.valid&&t.cls==ClassBody&&sp.sim==s.sim;
    const int st=modeOk?otherState(s.level,sp.prop):-1;
    const auto col=modeOk&&st==0?activeColliderAt(s.level,sp.levelIndex,sp.prop):0;
    float d=std::numeric_limits<float>::quiet_NaN();GripNow gn{};
    if(col&&col==sp.col){gn=gripNow(col,sp.seatLocal);if(gn.ok)d=v::length(v::sub(gn.grip,hs.solvedPos))/u;}
    const std::uint64_t age=now>=sp.startNs?now-sp.startNs:0;
    // the grip's open debounce (the hold's grabOpenMs): a dip under .30 keeps pulling, back over it is a dip ridden through
    const bool open=!(std::isfinite(t.squeeze)&&t.squeeze>=grabOpenSqueeze);
    if(open){if(!sp.openNs)sp.openNs=now?now:1;}else if(sp.openNs){sp.openNs=0;bump(K.dipsKept);}
    const std::uint64_t openAge=sp.openNs&&now>=sp.openNs?now-sp.openNs:0;
    const SeatEnd e=seatStep(modeOk,t.squeeze,grabOpenSqueeze,st,col&&col==sp.col,d,seatDoneFor(hs.prevDt,seatTauS(s.lev.creepMps)),age,
                             std::uint64_t(double(s.lev.grabOpenMs)*1e6),openAge,std::uint64_t(double(s.lev.grabOpenMs)*1e6),v::length(t.vel)/u);
    if(e==SeatKeep)return;
    if(e>=SeatCancelOpen){bump(K.cancels[unsigned(e-SeatCancelOpen)&3u]);sp=State::SeatPull{};
        if(!s.grab[h&1u].active){grablink::setHolding(h,false,0);grablink::setHoldKind(h,grablink::HoldNone);}return;}
    if(e==SeatThrow)bump(K.thrown); // attached where it is: the hold's gate throws it on the next tick
    else{bump(e==SeatSeated?K.seated:K.timeouts);bump(K.msHist[e==SeatSeated?seatMsBucket(age):4u]);}
    if(e==SeatSeated&&gn.ok){const float rel=v::length(v::sub(gn.vGrip,v::add(t.vel,hs.moverVel)))/u;if(std::isfinite(rel))maxOf(K.arriveMmsMax,std::uint64_t(rel*1000.f));}
    const State::SeatPull done=sp;sp=State::SeatPull{};
    float kg{-1.f};std::int32_t ct{-1};get(col+off::colliderMass,kg);get(col+off::colliderType,ct);
    attachHold(s,h,now,done.prop,done.part,done.cls,col,kg,ct,false,&done);
    if(!s.grab[h&1u].active){grablink::setHolding(h,false,0);grablink::setHoldKind(h,grablink::HoldNone);}
}
// HELD MASS (lever grabpairkg; main thread, once per tick after the hands): while both hands hold the same ball-jointed prop as a pair,
// its collider's mass is pairDriveKg (SetInertia through setColliderMass, as the unfreeze sets a prop's; the archetype is never
// touched); the mass read before goes back when the pair ends (one hand let go, the lever, the mode), on the same collider only (a
// collider the level no longer holds at that index took the archetype's mass at its activation: a miss, nothing written).
inline void heldMassRestore(State& s) noexcept {
    auto& m=s.heldMass;if(!m.set)return;auto& H=stats.heldMass;
    const auto col=m.sim==s.sim&&m.level==s.level&&s.level?activeColliderAt(s.level,m.levelIndex,m.prop):0;
    if(col&&col==m.col){setColliderMass(col,m.kg0);bump(H.restores);}else bump(H.misses);
    m=State::HeldMass{};}
inline void heldMassTick(State& s) noexcept {
    const auto& a=s.grab[0];const auto& b=s.grab[1];auto& m=s.heldMass;auto& H=stats.heldMass;
    const bool pair=s.mode==ModeOn&&s.lev.grab2&&s.lev.grabDrive&&a.active&&b.active&&a.prop==b.prop&&a.ball&&b.ball&&a.levelIndex==b.levelIndex&&
        a.propCol==b.propCol&&a.cls!=classDeadBody&&groupOk[GroupGrab]&&engine.setInertia;
    const float want=pair?pairDriveKg(a.propKg,s.lev.grabPairKg):0.f;
    const bool lighter=pair&&want>0.f&&want<a.propKg-1e-3f;
    if(m.set&&!(lighter&&m.prop==a.prop&&m.sim==s.sim&&m.level==s.level&&std::fabs(m.kg-want)<1e-3f))heldMassRestore(s);
    if(m.set||!lighter)return;
    const auto col=activeColliderAt(s.level,a.levelIndex,a.prop);
    alignas(16) float in[4]{};
    if(!col||col!=a.propCol||!engine.read(nullptr,col+off::colliderInertia,in,sizeof in)||!std::isfinite(in[3])||!(in[3]>0.f)){bump(H.refused);return;}
    if(!(want<in[3]-1e-3f))return; // already this light (the archetype's own mass)
    if(!setColliderMass(col,want)){bump(H.refused);return;}
    m.set=true;m.prop=a.prop;m.col=col;m.sim=s.sim;m.level=s.level;m.levelIndex=a.levelIndex;m.kg0=in[3];m.kg=want;bump(H.sets);
    H.kgBefore.store(in[3],std::memory_order_relaxed);H.kgSet.store(want,std::memory_order_relaxed);}
// PLAYER SKIP (lever grabplayerskip; main thread, once per tick): the props the precompute guard serves are the held heavy props and,
// with grabSkipLight, the welded light holds (lightSkipHold); after the hold a released prop lingers in a slot of its own while it still
// overlaps the player's capsule (a contact switched off within grabSkipLingerMs), so the solver never throws a released prop out of the
// capsule at once: past 2 s only while its deepest contact is 3 cm or more, at most 10 s; a level slot that no longer holds the prop at
// its index (deleted, its address reused) ends it at once.
inline constexpr std::uint64_t skipLingerSoftNs=2'000'000'000ull,skipLingerHardNs=10'000'000'000ull;inline constexpr float skipLingerDeepM=.03f;
inline bool levelHolds(std::uintptr_t level,std::uint16_t index,std::uintptr_t inst) noexcept {
    std::uint16_t count{};std::uintptr_t objects{},w{};
    if(!level||!inst||index==off::noIndex||!get(level+off::levelCount,count)||index>=count||!get(level+off::levelObjects,objects)||!objects||
       !get(objects+std::uintptr_t(index)*off::objStride+off::objInst,w))return false;
    return (w&~std::uintptr_t(0xf))==inst;}
// The linger's verdict: 0 keep, 1 end (no overlap within quiet, or past the soft bound and shallow), 2 end at the hard bound; deep = kept
// past the soft bound.
inline int lingerVerdict(std::uint64_t startNs,std::uint64_t seenNs,float depthM,std::uint64_t now,std::uint64_t quietNs,bool* deep=nullptr) noexcept {
    if(deep)*deep=false;
    if(!startNs||now<startNs)return 1;
    const bool overlap=seenNs&&now>=seenNs&&now-seenNs<quietNs;if(!overlap)return 1;
    const auto age=now-startNs;if(age>=skipLingerHardNs)return 2;
    if(age<skipLingerSoftNs)return 0;
    const bool d=std::isfinite(depthM)&&depthM>=skipLingerDeepM;if(deep)*deep=d;return d?0:1;}
// A welded light hold (fixed rotation, weld 1 or 2): rigid at the palm, so the player's capsule would push it and the hand together.
inline bool lightSkipHold(const State& s,const GrabHold& g) noexcept {return s.lev.grabSkipLight&&g.active&&g.rec.fixedRot&&g.weld>0;}
inline void playerSkipTick(State& s,std::uint64_t now) noexcept {
    auto& K=stats.playerSkip;
    if(!s.skipOurs||s.mode!=ModeOn){clearSkip(s);return;}
    std::array<std::uintptr_t,2> held{};std::array<std::uint16_t,2> heldIndex{0xffff,0xffff};std::array<bool,2> light{};
    for(unsigned h=0;h<2;++h){const auto& g=s.grab[h];if(!g.active)continue;
        if(heavyHold(s,h)){held[h]=g.prop;heldIndex[h]=g.levelIndex;}
        else if(lightSkipHold(s,g)){held[h]=g.prop;heldIndex[h]=g.levelIndex;light[h]=true;if(!(s.skip.prop[h]==g.prop&&s.skip.light[h]))bump(K.lightHolds);}}
    if(const unsigned ev=skipSlotsStep(s.skip,held,heldIndex,light,now))bump(K.lingerEvicted,ev);
    const std::uint64_t quiet=std::uint64_t(double(s.lev.grabSkipLingerMs)*1e6);
    for(auto& l:s.skip.linger){if(!l.prop)continue;
        const auto endLinger=[&]{maxOf(K.lingerMsMax,now>=l.startNs?(now-l.startNs)/1'000'000ull:0);l=SkipLinger{};};
        if(!levelHolds(s.level,l.index,l.prop)){bump(K.lingerStale);endLinger();continue;}
        bool deep=false;const int v=lingerVerdict(l.startNs,l.seenNs,l.depth/std::max(s.upm,1e-3f),now,quiet,&deep);
        if(deep&&!l.deepSeen){l.deepSeen=true;bump(K.lingerDeep);}
        if(v==0)continue;
        if(v==2)bump(K.lingerCapped);
        endLinger();}}
// LIGHT GHOST (main thread, once per tick): a ghost ends when its pair has not overlapped for grabSkipLingerMs, at 10 s, with the lever off
// or on a new level; LIGHT HOLDS: the released prop's top speed for 300 ms after the release.
inline void ghostTick(State& s,std::uint64_t now) noexcept {
    for(unsigned h=0;h<2;++h){auto& hs=s.hand[h]; // the squeeze lag of the closing test (lightGhostClosing)
        hs.sqLag=squeezeLagStep(hs.sqLag,s.tg.hand[h].squeeze,hs.sqLagNs&&now>hs.sqLagNs?now-hs.sqLagNs:0);hs.sqLagNs=now;}
    const std::uint64_t quiet=std::uint64_t(double(s.lev.grabSkipLingerMs)*1e6);
    for(auto& slots:s.ghosts)for(auto& g:slots){if(!g.inst)continue;
        GhostEnd e=!s.lev.grabGhost||s.mode!=ModeOn?GhostLever:s.level!=s.ghostLevel?GhostLevel:lightGhostKeep(g,now,quiet,skipLingerHardNs);
        if(e==GhostKeep)continue;
        bump(stats.ghost.ends[e]);maxOf(stats.ghost.msMax,now>=g.startNs?(now-g.startNs)/1'000'000ull:0);g=LightGhost{};}
    for(auto& r:s.lightHolds){if(!r.set||!r.watchUntil)continue;
        if(now>r.watchUntil){r.watchUntil=0;continue;}
        if(const auto col=activeColliderAt(s.level,r.levelIndex,r.prop)){const float ps=v::length(vec3(col+off::colliderVel))/std::max(s.upm,1e-3f);
            if(std::isfinite(ps))r.afterMaxMps=std::max(r.afterMaxMps,ps);}}
}
// THROW: a prop starts its flight (slot: a free one, else the older); a flight ends (the flight ring, the scan word cleared).
inline void startFlight(State& s,std::uintptr_t prop,std::uintptr_t sim,std::uint16_t idx,std::uintptr_t col,float kg,float radiusW,unsigned hand,std::uint8_t hands,
                        float launchMps,std::uint64_t now) noexcept;
inline void endFlight(State& s,unsigned i,FlightEnd why,std::uint64_t now) noexcept {
    auto& f=s.flights[i&1u];if(!f.on)return;
    auto& r=s.flightRing[s.flightRingNext];s.flightRingNext=(s.flightRingNext+1u)%unsigned(s.flightRing.size());
    const float u=std::max(s.upm,1e-3f);
    r=State::FlightRec{};r.ns=now;r.kg=f.kg;r.launchMps=f.launchMps;r.maxMps=f.maxMps;r.ms=now>=f.t0?float(double(now-f.t0)*1e-6):0.f;
    r.distCm=v::length(v::sub(f.lastCom,f.start))/u*100.f;r.gapCm=f.gapCm<1e8f?f.gapCm:-1.f;r.hits=f.hits;r.taps=f.taps;r.end=std::uint8_t(why);r.heavy=f.heavy?1:0;
    bump(stats.throws.ends[why<FlightEndCount?why:0]);f=State::Flight{};s.flightContacts[i&1u]={};propScanInst[2u+(i&1u)]=0;}
inline void startFlight(State& s,std::uintptr_t prop,std::uintptr_t sim,std::uint16_t idx,std::uintptr_t col,float kg,float radiusW,unsigned hand,std::uint8_t hands,
                        float launchMps,std::uint64_t now) noexcept {
    if(!prop||!col)return;
    unsigned i=0;
    for(unsigned k=0;k<2;++k)if(s.flights[k].on&&s.flights[k].prop==prop){endFlight(s,k,FlightRegrab,now);}
    if(s.flights[0].on&&!s.flights[1].on)i=1;else if(s.flights[0].on&&s.flights[1].on){i=s.flights[0].t0<=s.flights[1].t0?0u:1u;endFlight(s,i,FlightTimeout,now);}
    auto& f=s.flights[i];f=State::Flight{};f.on=true;f.prop=prop;f.sim=sim;f.level=s.level;f.col=col;f.levelIndex=idx;f.kg=kg;f.radiusW=radiusW;f.hand=hand&1u;f.hands=hands;
    f.t0=now?now:1;f.launchMps=launchMps;f.maxMps=launchMps;f.heavy=kg>=s.lev.propHitKg;
    const V3 c=vec3(col+off::colliderPos);f.start=v::finite(c)?c:V3{0,0,0};f.lastCom=f.start;f.lastV=vec3(col+off::colliderVel);f.lastW=vec3(col+off::colliderAngVel);f.lastNs=now;
    s.flightContacts[i]={};bump(stats.throws.flights);}
// THROW (lever throw): a nav hold's open edge (throwEdgeStep; a pair's both hands within throwPairMs), the holding arms' estimate over
// throwWinMs before it (ArmTrail velArm: no locomotion, no turn), the verdict (throwVerdict). ON: every hand on the prop lets go
// (GrabEndThrow), the launch (the estimate plus the player's motion, capped) is written with phCollider::SetVelocity when the prop is not
// already faster along it, the spin capped with SetAngVelocity (both CORE prints), the opening hands' contacts with the prop are dropped
// for throwPairDropNs and the flight starts. MEASURE: the decide ring only. Every refusal leaves the hold to grabStep. true = this hand's
// hold ended here.
inline bool throwTick(State& s,unsigned h,const HandTarget& t,std::uint64_t now) noexcept {
    auto& g=s.grab[h];auto& T=stats.throws;
    if(s.lev.throwMode==0||g.cls!=classNav){s.throwEdge[h]=ThrowEdge{};s.throwWait[h]=0;return false;}
    const unsigned o=h^1u;const auto& go=s.grab[o];const bool pair=go.active&&go.prop==g.prop;
    if(!throwEdgeStep(s.throwEdge[h],t.squeeze,now)){
        // the first hand of a pair waiting for the second: past the window the pair stays held
        if(s.throwWait[h]&&(!pair||throwPairState(s.throwWait[h],0,0,now,s.lev.throwPairMs)==ThrowPairHeld)){if(pair)bump(T.refused[ThrowPairHeld]);s.throwWait[h]=0;}
        return false;}
    bump(T.edges);
    const std::uint64_t first=s.throwEdge[h].firstNs;std::uint8_t hands=std::uint8_t(1u<<h);std::uint64_t firstMin=first;
    auto& d=s.throwDecides[s.throwDecideNext];
    const auto note=[&](ThrowVerdict vd){d=State::ThrowDecide{};d.ns=now;d.hand=std::uint8_t(h);d.hands=hands;d.verdict=std::uint8_t(vd);d.pair=pair?1:0;
        d.ageMs=now>=g.startNs?float(double(now-g.startNs)*1e-6):0.f;d.grip=t.squeeze;d.gripOther=pair?s.tg.hand[o].squeeze:-1.f;d.kg=g.propKg;
        s.throwDecideNext=(s.throwDecideNext+1u)%unsigned(s.throwDecides.size());};
    if(pair){
        const std::uint64_t oFirst=s.throwWait[o];const auto& oe=s.throwEdge[o];
        const ThrowVerdict ps=throwPairState(first,oFirst,oFirst?oe.confirmNs:0,now,s.lev.throwPairMs);
        if(ps==ThrowPairWait){s.throwWait[h]=first;return false;}
        s.throwWait[o]=0;
        if(ps==ThrowPairHeld){bump(T.refused[ThrowPairHeld]);note(ThrowPairHeld);return false;}
        hands=std::uint8_t(hands|(1u<<o));firstMin=std::min(first,oFirst);}
    s.throwWait[h]=0;
    const float u=std::max(s.upm,1e-3f);
    const std::uint64_t win=std::uint64_t(double(s.lev.throwWinMs)*1e6),from=firstMin>win?firstMin-win:0;
    ThrowEst e{};V3 vsum{0,0,0};unsigned ne=0;
    for(unsigned k=0;k<2;++k)if(hands>>k&1u){const ThrowEst x=throwEstimate(s.armTrail[k],from,now);if(!x.n)continue;
        vsum=v::add(vsum,x.v);++ne;e.n+=x.n;if(x.peak>e.peak){e.peak=x.peak;e.peakAgeMs=x.peakAgeMs;}}
    if(ne){e.v=v::mul(vsum,1.f/float(ne));e.est=v::length(e.v);}
    const float edgeMs=now>=firstMin?float(double(now-firstMin)*1e-6):0.f;
    const bool heavyT=pair||g.propKg>=s.lev.propHitKg;
    const ThrowVerdict vd=throwVerdict(e,s.tg.up,heavyT?s.lev.throwMps:s.lev.throwMpsLight,heavyT?s.lev.throwRelMps:s.lev.throwRelMpsLight,s.lev.throwWinMs+edgeMs);
    const auto col=g.sim==s.sim?activeColliderAt(s.level,g.levelIndex,g.prop):0;
    const V3 vProp=col&&col==g.propCol?v::mul(vec3(col+off::colliderVel),1.f/u):V3{0,0,0};
    note(vd==ThrowGo&&s.lev.throwMode==1?ThrowMeasure:vd);
    d.heavy=heavyT?1:0;d.peak=e.peak;d.peakEdgeMs=e.peakAgeMs>=0.f?e.peakAgeMs-edgeMs:-1.f;d.est=e.est;
    {const V3 un=v::normalized(s.tg.up);d.upFrac=e.est>1e-3f&&v::length(un)>.5f?v::dot(e.v,un)/e.est:0.f;}d.propMps=v::length(vProp);
    if(vd!=ThrowGo){bump(T.refused[vd<ThrowVerdictCount?vd:0]);return false;}
    if(s.lev.throwMode==1){bump(T.measured);return false;}
    // ON: the prop's facts before the hold's record is cleared
    const std::uintptr_t prop=g.prop,sim=g.sim,pcol=g.propCol;const std::uint16_t idx=g.levelIndex;
    float kg=g.propKg;if(s.heldMass.set&&s.heldMass.prop==prop&&s.heldMass.kg0>0.f)kg=s.heldMass.kg0;
    const float radiusW=(g.rec.radiusCm>0.f?g.rec.radiusCm*.01f:.5f)*u;
    for(unsigned k=0;k<2;++k)if(s.grab[k].active&&s.grab[k].prop==prop){releaseGrab(s,k,GrabEndThrow);s.throwDropUntil[k]=now+throwPairDropNs;s.throwDropProp[k]=prop;
        s.throwEdge[k]=ThrowEdge{};s.throwWait[k]=0;hands=std::uint8_t(hands|(1u<<k));}
    const V3 launch=throwLaunch(e.v,v::mul(s.hand[h].moverVel,1.f/u),heavyT?s.lev.throwMaxMps:s.lev.throwMaxMpsLight);
    d.launchMps=v::length(launch);
    const auto colNow=s.sim==sim?activeColliderAt(s.level,idx,prop):0;
    if(colNow&&colNow==pcol&&throwWrites(vProp,launch)){const V3 vw=v::mul(launch,u);alignas(16) const float vv[4]{vw[0],vw[1],vw[2],0.f};
        if(v::finite(vw)){engine.setVelocity(reinterpret_cast<void*>(colNow),vv);bump(T.velWrites);d.written=1;}}
    else bump(T.velRefused);
    if(colNow&&colNow==pcol){const V3 w=vec3(colNow+off::colliderAngVel);const float wl=v::length(w);
        if(std::isfinite(wl)&&wl>throwSpinMaxRadS){const V3 wc=v::mul(w,throwSpinMaxRadS/wl);alignas(16) const float ww[4]{wc[0],wc[1],wc[2],0.f};
            engine.setAngVelocity(reinterpret_cast<void*>(colNow),ww);bump(T.spinClamped);}}
    if(colNow&&colNow==pcol)startFlight(s,prop,sim,idx,colNow,kg,radiusW,h,hands,std::max(d.launchMps,d.propMps),now);
    bump(pair?T.throwsPair:heavyT?T.throwsHeavy:T.throwsLight);
    return true;
}
inline void seatTick(State& s,unsigned h,std::uint64_t now) noexcept;
inline void grabTick(State& s,unsigned h,std::uint64_t now) noexcept {
    auto& hs=s.hand[h];auto& g=s.grab[h];const auto& t=s.tg.hand[h];
    if(!g.active&&s.seat[h&1u].on){seatTick(s,h,now);return;}
    if(g.active){
        grablink::stampHolding(h,now);grablink::setReady(h,false,now);grablink::setJoin(h,false,now,0.f);s.latchSeen[h]=grablink::latchSerial[h].load(std::memory_order_acquire);s.latchPendingNs[h]=0; // latches of a holding hand: not grab misses
        if(!grabOn(s)){releaseGrab(s,h,GrabEndMode);return;}
        if(!hs.present||hs.machine.state!=BodyLive){
            {auto& e=s.bodyEnds[s.bodyEndNext];e=State::BodyEnd{};e.set=true;e.ns=now;e.hand=std::uint8_t(h);e.present=hs.present?1:0;e.state=std::uint8_t(hs.machine.state);
             e.wantBits=std::uint8_t(s.wantBits[h]&0xffu);e.devM=hs.devM<1e8f?hs.devM:-1.f;s.bodyEndNext=(s.bodyEndNext+1u)%unsigned(s.bodyEnds.size());}
            releaseGrab(s,h,GrabEndBody);return;}
        if(!t.valid||t.cls!=ClassBody){releaseGrab(s,h,GrabEndMode);return;}
        if(g.sim!=s.sim){releaseGrab(s,h,GrabEndDetached,false);return;}
        // a constraint that is no longer ours ends the hold; the other one is released while it still is (releaseGrab checks each)
        if(!grabConOk(g,instOf(h))){releaseGrab(s,h,GrabEndDetached);return;}
        if(!grabRotOk(g,instOf(h))){bump(stats.grab.rotDetached);releaseGrab(s,h,GrabEndDetached);return;}
        std::uintptr_t manifold{};std::uint8_t disabled{};
        // the held prop deleted: phContactMgr::DeleteObject 0x1c36a38 hands back the manifold (+0x18 = 0) and drops the constraint from the
        // active list, inst A / B stay; ReleaseConstraint (DeactivateConstraintInternal finds nothing) clears them and pools it: one call
        if(!get(g.con+off::conManifold,manifold)||!manifold){releaseGrab(s,h,GrabEndDetached);return;}
        if(get(g.con+off::conDisabled,disabled)&&disabled){releaseGrab(s,h,GrabEndDisabled);return;}
        if(g.conRot){if(!get(g.conRot+off::conManifold,manifold)||!manifold){bump(stats.grab.rotDetached);releaseGrab(s,h,GrabEndDetached);return;}
            if(get(g.conRot+off::conDisabled,disabled)&&disabled){bump(stats.grab.rotDetached);releaseGrab(s,h,GrabEndDisabled);return;}}
        if(otherState(s.level,g.prop)!=0){releaseGrab(s,h,GrabEndDeactivated);return;}
        for(auto& e:s.moved)if(e.inst==g.prop)e.touchNs=now; // released furniture held: its restore waits
        const float u=std::max(s.upm,1e-3f),mps=v::length(t.vel)/u;
        {auto& r=g.rec;const float sq=std::isfinite(t.squeeze)?t.squeeze:1.f;r.gripMin=std::min(r.gripMin,sq);r.gripEnd=sq;
         if(std::isfinite(t.trigger))r.triggerMax=std::max(r.triggerMax,t.trigger);
         // grip dips that recover: under .5 and back, under .3 and back within the open debounce
         if(sq<.5f){if(!g.dipNs)g.dipNs=now?now:1;g.dipMin=std::min(g.dipMin,sq);}
         else if(g.dipNs){bump(stats.grab.gripDips50);if(g.dipMin<grabOpenSqueeze&&now>=g.dipNs&&double(now-g.dipNs)<double(s.lev.grabOpenMs)*1e6)bump(stats.grab.gripDips30);g.dipNs=0;g.dipMin=1.f;}
         const float mv=v::length(hs.moverVel)/u;if(std::isfinite(mv)){g.moverSum+=double(mv);++g.moverN;r.moverSpeedMax=std::max(r.moverSpeedMax,mv);}
         if(const auto col=activeCollider(s,g.prop)){const float ps=v::length(vec3(col+off::colliderVel))/u;if(std::isfinite(ps))r.propSpeedMax=std::max(r.propSpeedMax,ps);
            // a light hold: the prop's centre to the palm centre (growth = the joint slipped)
            if(r.fixedRot&&hs.haveSolved){const V3 rel=v::sub(vec3(col+off::colliderPos),hs.solvedPos);const float d=v::length(rel)/u*100.f;
                if(std::isfinite(d)){g.endOffCm=d;g.maxOffCm=std::max(g.maxOffCm,d);
                    const std::uint64_t k=now>=g.startNs?(now-g.startNs)/250'000'000ull:0;if(k<g.offTrace.size()&&g.offTrace[k]<0.f)g.offTrace[k]=d;}
                // the slip in the palm since the attach (the hand body's frame: a turned hand reads 0)
                if(g.haveLocal0){const float sl=palmSlip(hs.solvedRows,hs.solvedPos,vec3(col+off::colliderPos),g.local0);if(sl>=0.f)g.slipCm=std::max(g.slipCm,sl/u*100.f);}}}}
        // the hold-end ring's samples; a grip that fell from 0.85 or more to exactly 0 in one tick (a zeroed sample); a heavy hold's release
        {const float sq=std::isfinite(t.squeeze)?t.squeeze:-1.f;const unsigned j=g.histN%6u;g.gripHist[j]=sq;g.trigHist[j]=std::isfinite(t.trigger)?t.trigger:-1.f;
         g.histN=std::uint8_t(g.histN<250?g.histN+1:g.histN-5);s.lastHandMps[h]=mps;
         if(g.prevSq>=.85f&&sq==0.f){bump(stats.grab.gripSnapZero);++g.rec.snapZero;}g.prevSq=sq;
         if(::rdrvr::runtime::theaterChordStats.consuming.load(std::memory_order_relaxed)){bump(stats.grab.heldChordConsuming);++g.rec.chordTicks;}}
        const bool heavy=s.lev.grabHeavyHold&&heavyHold(s,h);if(heavy){bump(stats.grab.heavyTicks);g.rec.heavy=true;}
        // a heavy hold ends on an open hand (lever grabheavyleash off): a safety break far beyond the leash, a teleport of the mover
        const bool openOnly=heavy&&!s.lev.grabHeavyLeash;
        if(std::isfinite(g.lagM)&&g.lagM>.60f){if(!g.over60Since)g.over60Since=now?now:1;}
        else if(g.over60Since){if(now>g.over60Since)g.over60Ns+=now-g.over60Since;g.over60Since=0;}
        // (a teleport: grabTeleportM or more at 10 m/s or more since the last tick; a long frame of a sprint is no teleport)
        if(s.moverOk){const V3 mp=vec3(s.mover+off::moverPos);
            const float d=g.haveMoverLast&&v::finite(mp)?v::length(v::sub(mp,g.moverLast))/u:0.f;
            const float dtS=g.moverLastNs&&now>g.moverLastNs?float(double(now-g.moverLastNs)*1e-9):0.f;
            const bool jumped=openOnly&&d>=s.lev.grabTeleportM&&d>=10.f*std::max(dtS,1e-3f);
            g.moverLast=mp;g.haveMoverLast=v::finite(mp);g.moverLastNs=now;
            if(jumped){releaseGrab(s,h,GrabEndTeleport);return;}}
        if(throwTick(s,h,t,now))return;
        switch(grabStep(g.gate,t.squeeze,t.trigger,mps,g.lagM,s.lev.grabLeashM,s.lev.grabOpenMs,now,std::uint64_t(double(s.lev.grabBreakMs)*1e6),heavy,
                        openOnly?s.lev.grabSafetyM:0.f)){
        case GrabThrow:{
            maxOf(stats.grab.throwMmsMax,std::uint64_t(std::max(0.f,mps)*1000.f));
            // THROW: a nav prop let go fast by grabStep flies with the velocity the hold gave it (its flight is tracked for the hits / taps).
            // A prop of propHitKg or more (a one-hand hold that is not heavyHold: ball-jointed up to grabLiftKg) strikes in flight, so it
            // flies only on the throw rule of throwTick at the heavy thresholds (throwVerdict over throwWinMs before this open sample: a
            // put-down, a drop or a walk with the hand opening is refused); grabStep's 1.2 m/s alone starts a light prop's flight only.
            if(s.lev.throwMode==2&&g.cls==classNav)if(const auto col=g.sim==s.sim?activeColliderAt(s.level,g.levelIndex,g.prop):0;col&&col==g.propCol){
                const float sp=v::length(vec3(col+off::colliderVel))/u;bool fly=true;
                if(g.propKg>=s.lev.propHitKg){
                    const std::uint64_t win=std::uint64_t(double(s.lev.throwWinMs)*1e6);const ThrowEst e=throwEstimate(s.armTrail[h],now>win?now-win:0,now);
                    const ThrowVerdict vd=throwVerdict(e,s.tg.up,s.lev.throwMps,s.lev.throwRelMps,s.lev.throwWinMs);fly=vd==ThrowGo;
                    bump(fly?stats.throws.midGrabThrows:stats.throws.midGrabRefused);
                    auto& d=s.throwDecides[s.throwDecideNext];s.throwDecideNext=(s.throwDecideNext+1u)%unsigned(s.throwDecides.size());
                    d=State::ThrowDecide{};d.ns=now;d.hand=std::uint8_t(h);d.hands=std::uint8_t(1u<<h);d.verdict=std::uint8_t(vd);d.heavy=1;d.grabStep=1;
                    d.ageMs=now>=g.startNs?float(double(now-g.startNs)*1e-6):0.f;d.grip=t.squeeze;d.kg=g.propKg;d.peak=e.peak;d.peakEdgeMs=e.peakAgeMs;d.est=e.est;
                    {const V3 un=v::normalized(s.tg.up);d.upFrac=e.est>1e-3f&&v::length(un)>.5f?v::dot(e.v,un)/e.est:0.f;}d.propMps=std::isfinite(sp)?sp:0.f;}
                else bump(stats.throws.lightGrabThrows);
                if(fly)startFlight(s,g.prop,g.sim,g.levelIndex,col,g.propKg,(g.rec.radiusCm>0.f?g.rec.radiusCm*.01f:.2f)*u,h,std::uint8_t(1u<<h),std::isfinite(sp)?sp:0.f,now);}
            if(s.lev.grabThrow>0.f)if(const auto col=activeCollider(s,g.prop)){const V3 v0=vec3(col+off::colliderVel);const V3 v1=v::add(v0,v::mul(v::add(t.vel,hs.moverVel),s.lev.grabThrow));
                alignas(16) const float vv[4]{v1[0],v1[1],v1[2],0.f};if(v::finite(v1))engine.setVelocity(reinterpret_cast<void*>(col),vv);}
            releaseGrab(s,h,GrabEndThrow);return;}
        case GrabOpen:releaseGrab(s,h,GrabEndOpen);return;
        case GrabLeash:releaseGrab(s,h,GrabEndLeash);return;
        case GrabSafety:releaseGrab(s,h,GrabEndSafety);return;
        case GrabKeep:break;}
        return;
    }
    // a fresh latch of the holsters: the classes met in the last 4 substeps; one the grab did not get waits for a ready (200 ms)
    {const std::uint32_t ls=grablink::latchSerial[h].load(std::memory_order_acquire);
     if(ls!=s.latchSeen[h]){s.latchSeen[h]=ls;
        if(s.latchPendingNs[h])bump(stats.grab.latchNoReady);
        {const auto br=grablink::latchBranch[h].load(std::memory_order_relaxed);
         s.latchPendingNs[h]=br==grablink::LatchGrab||br==grablink::LatchJoin?0:grablink::latchNs[h].load(std::memory_order_relaxed);}
        for(unsigned c=0;c<16;++c)if(hs.classStep[c]&&s.step>=hs.classStep[c]&&s.step-hs.classStep[c]<=4)bump(stats.grab.latchClass[c]);
        for(const auto& r:hs.recent)if(r.cls>=0&&s.step>=r.step&&s.step-r.step<=4){
            bool done=false;for(auto& x:s.latchSamples){if(x.count&&x.cls==r.cls&&x.archType==r.archType){++x.count;done=true;break;}if(!x.count){x={r.cls,0,r.archType,1};done=true;break;}}
            (void)done;}}
     if(s.latchPendingNs[h]&&now>=s.latchPendingNs[h]&&now-s.latchPendingNs[h]>200'000'000ull){bump(stats.grab.latchNoReady);s.latchPendingNs[h]=0;}}
    const bool on=grabOn(s)&&hs.present&&hs.machine.state==BodyLive;
    if(on)grabCandidate(s,h,now);else{s.grabCand[h]=0;s.grabCandPart[h]=-1;s.grabCandCls[h]=-1;s.grabLast[h]={};}
    if(s.grabCand[h]){
        grablink::setReady(h,true,now);bump(stats.grab.readyTicks);s.grabLast[h]={s.grabCand[h],s.grabCandPart[h],s.grabCandCls[h],now};
        if(s.latchPendingNs[h]){const auto d=now>=s.latchPendingNs[h]?now-s.latchPendingNs[h]:0;
            bump(stats.grab.readyAfterLatch[d<25'000'000ull?0u:d<50'000'000ull?1u:d<100'000'000ull?2u:3u]);s.latchPendingNs[h]=0;}}
    else if(!on)grablink::setReady(h,false,now); // else the stamp of the last tick with a candidate holds until readyMaxNs
    if(!on||!s.grab[h^1u].active){grablink::setJoin(h,false,now,0.f);grablink::setHeld(h,-1.f,now);}
    const std::uint32_t serial=grablink::pressSerial[h].load(std::memory_order_acquire);
    if(serial!=s.grabSerial[h]){s.grabSerial[h]=serial;bump(stats.grab.presses);
        const auto ns=grablink::pressNs[h].load(std::memory_order_relaxed);
        if(!fresh(now,ns,grabPressMaxNs)){bump(stats.grab.stalePresses);return;}
        // a join try: the press meets the prop the other hand holds (candidate or hold-over)
        const auto& o=s.grab[h^1u];const std::uintptr_t want=s.grabCand[h]?s.grabCand[h]:s.grabLast[h].inst;
        const bool join=o.active&&want&&want==o.prop;
        s.grabLastRefuse[h]=0xff;startGrab(s,h,now);
        if(join){bump(stats.grab.joinTries);bump(s.grab[h].active?stats.grab.joinStarts:stats.grab.joinRefused);
            auto& r=s.joinRing[s.joinNext];r=State::JoinRec{};r.set=true;r.ns=now;r.hand=std::uint8_t(h);r.cls=std::uint8_t(o.cls&0xff);
            r.result=s.grab[h].active?0xfe:s.grabLastRefuse[h];r.part=s.grab[h].active?s.grab[h].comp:s.grabCandPart[h];
            r.distMm=grablink::joinAt(h,now)?grablink::joinMm[h].load(std::memory_order_relaxed):-1;s.joinNext=(s.joinNext+1u)%unsigned(s.joinRing.size());}}
}
// TOUCH: VrMelee consumes the touches (its prints and words matched in our install).
inline bool touchOn(const State&) noexcept {return groupOk[GroupTouch]&&touchReady.load(std::memory_order_acquire);}
inline void handTick(State& s,unsigned h,std::uint64_t now,bool jump,bool carried) noexcept {
    auto& hs=s.hand[h];auto& st=stats.hand[h];
    const auto& t=s.tg.hand[h];
    hs.push={}; // this tick's door push: none unless doorContact below finds one (a removed body leaves none behind)
    if(hs.pendingIndex!=off::noIndex&&!inLevel(s.level,instOf(h),hs.pendingIndex)){slotClear(s.sim,instOf(h),hs.pendingIndex,&objects[h].beh);hs.pendingIndex=off::noIndex;}
    // the engine removed our instance (a level reset, a deactivation): forget it, clear our slot
    if(hs.present){std::uint16_t index{};
        if(!verifiedCollider(s.level,instOf(h),index)||index!=hs.index){
            const bool held=inLevel(s.level,instOf(h),hs.index);
            bump(held?stats.inactive:stats.simResets);
            if(held){std::uint16_t a{},c{};if(get(s.level+off::levelActive,a)&&get(s.level+off::levelCap,c)){stats.inactiveLevelActive.store(a);stats.inactiveLevelCap.store(c);}
                stats.inactiveCmdSpeed.store(hs.cmdSpeed);removeBody(s,h);}
            else{slotClear(s.sim,instOf(h),hs.index,&objects[h].beh);hs.present=false;hs.ghost=false;hs.index=off::noIndex;}}}
    const bool want=s.mode==ModeOn&&s.tgUsable&&s.moverOk&&s.archType&&!s.flagsBad&&t.valid&&t.cls==ClassBody&&s.tg.onFoot&&!carried;
    s.wantBits[h]=std::uint16_t((s.mode==ModeOn?1u:0u)|(s.tgUsable?2u:0u)|(s.moverOk?4u:0u)|(s.archType?8u:0u)|(!s.flagsBad?16u:0u)|(t.valid?32u:0u)|
        (t.cls==ClassBody?64u:0u)|(s.tg.onFoot?128u:0u)|(!carried?256u:0u)|(jump?512u:0u));
    const V3 target=v::add(s.tg.moverPos,t.rel);
    const bool measured=hs.haveSolved&&hs.haveCommand&&hs.solvedStep>hs.insertStep;
    hs.devM=measured?v::length(v::sub(hs.solvedPos,hs.command))/std::max(s.upm,1e-3f):1e9f;
    MachineIn in{};in.want=want;in.present=hs.present;in.jump=jump;in.now=now;
    in.devM=s.grab[h].active?std::min(hs.devM,s.lev.leashM*.99f):hs.devM; // a held prop: its break owns the distance, never the park
    in.targetInBody=s.tgUsable&&insideBody(target,s.tg.eye,v::normalized(s.tg.up),s.upm);
    in.returnOk=hs.machine.state==BodyPark&&parkReturn(target,hs.parkPos,hs.parkN,s.lev.returnM,s.upm);
    const BodyState before=hs.machine.state;
    const Action a=step(hs.machine,in,s.lev);
    alignas(16) float m[16]{};
    if(palm(s))palmMatrix(t.rows,chestPoint(s.tg.eye,s.tg.up,s.upm),m);else bodyMatrix(t.rows[1],chestPoint(s.tg.eye,s.tg.up,s.upm),m);
    switch(a){
    case ActInsert:
        if(now<hs.retryNs){hs.machine.state=BodyAbsent;break;}
        if(!insertBody(s,h,m)){hs.machine.state=BodyAbsent;if(!hs.present&&hs.pendingIndex==off::noIndex)hs.retryNs=now+250'000'000ull;}
        break;
    case ActRespawn:if(before==BodyPark&&!jump)bump(st.chestReturns);respawnBody(s,h,m);break;
    case ActGhost:hs.ghost=true;bump(st.ghosts);break;
    case ActPark:hs.parkPos=hs.haveSolved?hs.solvedPos:target;hs.parkN=hs.est.age<=6?hs.est.n:V3{0,0,0};bump(st.parkGhosts);break;
    case ActUnpark:bump(st.inPlaceReturns);hs.est=ContactEst{};break;
    case ActRemove:removeBody(s,h);break;
    case ActNone:break;
    }
    hs.ghost=hs.present&&(hs.machine.state==BodyGhost||hs.machine.state==BodyPark);
    if(!hs.present){grablink::setReady(h,false,now);return;}
    const bool live=hs.machine.state==BodyLive;
    if(live)bump(st.liveFrames);
    if(live&&hs.devM<1e8f){const auto um=std::uint64_t(hs.devM*1e6f);st.devUmSum.fetch_add(um,std::memory_order_relaxed);maxOf(st.devUmMax,um);bump(st.devFrames);}
    const bool contact=live&&hs.est.age<=2;
    if(contact)bump(st.contactFrames);
    // a new contact: one pulse (not while the trigger is held), its strength from the commanded speed into the surface
    // (an open-hand hit on a virtual body fast enough for a slap: VrMelee's hit pulse instead)
    bool slapLikely=false;
    if(hs.edgeVirtual&&!hs.fist&&touchOn(s)){TouchIn ti{};ti.capKind=hs.vhit.kind;ti.a=hs.vhitApproach;ti.speed=hs.vhitSpeed;ti.swing=hs.vhitSwing;ti.facing=hs.vhitFacing;
        ti.side=hs.vhitSide;ti.tangent=hs.vhitTangent;ti.arm=hs.vhitArm;slapLikely=classifyTouch(ti,s.lev.slapMps,s.lev.shoveMps,s.lev.npcSlap,s.lev.npcShove)!=TouchNone;}
    if(hs.edge){hs.edge=false;
        if(!slapLikely&&(live||hs.machine.state==BodyTravel)&&s.lev.haptic&&t.trigger<=triggerBusy&&(!hs.pulseNs||now>=hs.pulseNs+180'000'000ull)&&
           haptics::pulse(h,pulseAmplitude(hs.approach),20'000,now,haptics::SourceContact)){hs.pulseNs=now;bump(st.pulses);}}
    // PALM: the fingers or the fist part, kept while touching
    if(palm(s)){const bool f=partWant(t.squeeze,hs.fist,hs.est.age<=6,s.lev.fist);
        if(f!=hs.fist){hs.fist=f;writePartFlags(objects[h],f);bump(st.partToggles);}}
    if(t.valid){hs.velRing[hs.velNext]=v::add(t.vel,hs.moverVel);hs.velNext=(hs.velNext+1u)%unsigned(hs.velRing.size());}
    {const auto a=nowNs();approachTick(s,h,now);contactProps(s,h,now);glassTick(s,h,now);const auto b=nowNs();s.pend.secNs[SecProps]+=b-a;
     doorContact(s,h);const auto c=nowNs();grabTick(s,h,now);s.pend.secNs[SecGrab]+=nowNs()-c;}
}
// The actor in the slot table for guid (index guid & 0xffff, generation guid >> 16); 0 when the slot holds another one.
inline std::uintptr_t slotActor(std::uint32_t guid) noexcept {
    const auto b=engine.base;std::uintptr_t manager{},slots{},actor{};std::uint16_t count{},gen{};const std::uint32_t index=guid&0xffffu;
    if(!guid||!get(b+off::actorManager,manager)||!manager||!get(manager,count)||index>=count||!get(b+off::actorSlots,slots)||!slots||
       !get(slots+std::uintptr_t(index)*16u,actor)||!get(slots+std::uintptr_t(index)*16u+8u,gen))return 0;
    return gen==(guid>>16)?actor:0;
}
// The creature is alive now: the nearby list can be up to 1 s old, and the engine sends mvrMsgBumpedInto only between live movers.
inline bool aliveNow(std::uintptr_t actor) noexcept {
    std::uintptr_t health{};float hp{};std::uint32_t flags{};
    return get(actor+off::actorHealth,health)&&health&&get(health+off::healthValue,hp)&&std::isfinite(hp)&&hp>0.f&&
           get(health+off::healthFlags,flags)&&!(flags&off::healthDead);
}
// PROPHIT (main thread, outside the step): a held prop of propHitKg or more is armed (its impact iterator scanned) while creatures are
// near (this tick's virtual bodies). Each creature capsule it met since the last tick: its actor (user data kind 0, the slot table, alive;
// never the player), the approach = the prop's velocity at the contact minus the creature's along the normal, the swing = the same minus
// the player's mover (the hands' own motion of the prop); PropHitGate decides on the smaller (an edge after 150 ms without contact,
// propHitMps, propHitCoolMs per creature) and a hit goes to VrMelee as a PropHitRequest (the player's strike; an open hand has no path
// to it: only a prop a hand holds is ever scanned).
// A held prop of propHitKg or more (a nav prop) whose geometric hit wants the creatures near it.
inline bool heavyGeoHold(const State& s,unsigned h) noexcept {
    const auto& g=s.grab[h&1u];return g.active&&g.cls==classNav&&g.propKg>=s.lev.propHitKg;}
// PROPHIT (geometric, lever prophitgeo): the held heavy prop's part boxes (the hand's GrabCache entry: the part bounds once, this tick's
// part matrices and instance matrix) against this tick's creature bodies (their bone capsules moved on by their velocity, at most 50 ms).
// A creature touches when a part comes within propHitShellM of a capsule's surface or the engine reported the prop on its mover; the
// point's speed (the prop's velocity there now or last tick, the larger along the normal: the solver has already slowed it when the scan
// reads it) against the creature (a) and against the player's mover (swing), and the holding hands' own arm speed (velArm: the peak of
// propHitArmMs, and the peak along the normal) decide (PropGeoGate, per creature and prop). A capsule farther from a part's centre than
// the part's radius + its own + the shell's hysteresis is skipped before the distance search. A hit is the same PropHitRequest the engine path sends (VrMelee: the player's strike). Reads only: two
// guarded reads per held prop and tick, no engine call, no write.
inline void propGeoTick(State& s,std::uint64_t now,const std::array<PropContact,3>& contacts) noexcept {
    auto& Q=stats.propGeo;const float u=std::max(s.upm,1e-3f);const auto t0=nowNs();
    const std::uint32_t nb=std::min<std::uint32_t>(s.vb.count,std::uint32_t(vbodyMax));
    const float age=s.vb.ns&&now>s.vb.ns?std::min(float(double(now-s.vb.ns)*1e-9),.05f):0.f;
    // the engine's contacts of the prop with each body's mover (the touch signal; the shell measured against the bone capsules)
    std::array<bool,vbodyMax> engineTouch{};
    for(const auto& c:contacts){if(!c.mover||c.mover==s.moverInstAddr)continue;
        std::uintptr_t ud{},actor{};std::uint8_t kind{0xff};
        if(!get(c.mover+off::instUser,ud)||!ud||!get(ud+off::userKind,kind)||kind!=0||!get(ud+off::userActor,actor)||!actor)continue;
        for(std::uint32_t i=0;i<nb;++i){const auto& b=s.vb.b[i];if(b.actor!=actor)continue;engineTouch[i]=true;
            const V3 off=v::mul(b.vel,age);float best=1e30f;
            for(unsigned k=0;k<std::min<unsigned>(b.count,unsigned(vbodyCaps));++k){const auto& cp=b.c[k];
                const float d=v::length(v::sub(c.pos,closestOnSegment(v::add(cp.a,off),v::add(cp.b,off),c.pos)))-cp.r;if(d<best)best=d;}
            if(best<1e29f){const float cm=best/u*100.f;bump(Q.shellCm[cm<0.f?0u:cm<5.f?1u:cm<10.f?2u:cm<15.f?3u:cm<25.f?4u:5u]);}}}
    const auto bin6=[](float x){return !(x>=.5f)?0u:x<1.f?1u:x<1.5f?2u:x<2.f?3u:x<3.f?4u:5u;};
    for(unsigned h=0;h<2;++h){
        if(!heavyGeoHold(s,h))continue;const auto& g=s.grab[h];
        if(h==1&&heavyGeoHold(s,0)&&s.grab[0].prop==g.prop)continue; // two hands on one prop: once
        bump(Q.armedTicks);
        const auto col=activeCollider(s,g.prop);if(!col){bump(Q.readFails);continue;}
        const V3 com=vec3(col+off::colliderPos),vc=vec3(col+off::colliderVel),w=vec3(col+off::colliderAngVel);
        float m[16]{};const GrabCache* gc=propParts(s,h,g.prop,m,false);
        auto& prev=s.propPrev[h];
        if(!gc){bump(Q.noCache);continue;}
        std::array<Obb,24> ob{};std::array<float,24> rad{};unsigned no=0;
        for(unsigned i=0;i<gc->count&&i<ob.size();++i)if(partObb(m,gc->parts[i].m,gc->parts[i].bmin,gc->parts[i].bmax,ob[no])){rad[no]=v::length(ob[no].h);++no;}
        bump(Q.parts,no);
        const bool prevOk=prev.prop==g.prop&&now>prev.ns&&now-prev.ns<50'000'000ull;
        std::uint8_t hands=0;for(unsigned k=0;k<2;++k)if(s.grab[k].active&&s.grab[k].prop==g.prop)hands=std::uint8_t(hands|(1u<<k));
        const std::uint64_t armWin=std::uint64_t(double(s.lev.propHitArmMs)*1e6);
        const V3 moverVel=s.hand[h].moverVel;const float hyst=(s.lev.propHitShellM+.03f)*u;
        // the diagnostics' nearest gap of this tick over the bodies (world units) and the hold's geo record (both holding hands)
        float tickGap=1e30f;bool anyBody=false;
        const auto geoAll=[&](auto&& f){for(unsigned k=0;k<2;++k)if(hands&(1u<<k))f(s.grab[k].geo);};
        geoAll([](HoldGeo& x){++x.ticks;});
        for(std::uint32_t i=0;i<nb;++i){const auto& b=s.vb.b[i];if(!b.guid||!b.count)continue;anyBody=true;
            const V3 off=v::mul(b.vel,age),bc=v::add(b.centre,off);
            float best=1e30f;int bestPart=-1,bestCap=-1;V3 boxPt{},capPt{};
            // the gap's lower bound (the part's and the capsule's bounds; exact where segObbDist ran) and its pair
            float gapLo=1e30f;int gapPart=-1,gapCap=-1;bool gapExact=false;unsigned passed=0,tested=0;
            const unsigned nc=std::min<unsigned>(b.count,unsigned(vbodyCaps));
            for(unsigned j=0;j<no;++j){
                const float sg=v::length(v::sub(ob[j].c,bc))-rad[j]-b.radius;
                if(sg>hyst){bump(Q.sphereRejects);
                    if(sg<1.f*u){for(unsigned k=0;k<nc;++k){const auto& cp=b.c[k];const V3 ca=v::add(cp.a,off),cb=v::add(cp.b,off);
                        const float cg=v::length(v::sub(ob[j].c,closestOnSegment(ca,cb,ob[j].c)))-rad[j]-cp.r;if(cg<gapLo){gapLo=cg;gapPart=int(j);gapCap=int(k);gapExact=false;}}}
                    else if(sg<gapLo){gapLo=sg;gapPart=int(j);gapCap=-1;gapExact=false;}
                    continue;}
                ++passed;
                for(unsigned k=0;k<nc;++k){const auto& cp=b.c[k];
                    // the part lies within rad of its centre: a capsule beyond rad + r + the hysteresis cannot touch it nor end a touch early
                    const V3 ca=v::add(cp.a,off),cb=v::add(cp.b,off);
                    const float cg=v::length(v::sub(ob[j].c,closestOnSegment(ca,cb,ob[j].c)))-rad[j]-cp.r;
                    if(cg>hyst){bump(Q.capRejects);if(cg<gapLo){gapLo=cg;gapPart=int(j);gapCap=int(k);gapExact=false;}continue;}
                    bump(Q.capTests);++tested;
                    const SegObb r=segObbDist(ca,cb,ob[j],12);const float d=r.d-cp.r;
                    if(d<gapLo){gapLo=d;gapPart=int(j);gapCap=int(k);gapExact=true;}
                    if(d<best){best=d;bestPart=int(j);bestCap=int(k);boxPt=r.boxPt;capPt=r.segPt;}}}
            // a pair under 30 cm that no exact test measured: one segObbDist for its exact distance (diagnostics only)
            if(!gapExact&&gapPart>=0&&gapCap>=0&&gapLo<.30f*u){const auto& cp=b.c[std::size_t(gapCap)];
                const SegObb r=segObbDist(v::add(cp.a,off),v::add(cp.b,off),ob[std::size_t(gapPart)],12);const float d=r.d-cp.r;
                if(std::isfinite(d)){gapLo=std::max(gapLo,d);gapExact=true;bump(Q.nearExact);}}
            const bool eng=engineTouch[i];const float shell=s.lev.propHitShellM*u;
            const bool geo=bestPart>=0&&best<=shell;
            {const std::uint8_t why=std::uint8_t(geo?3u:tested?2u:passed?1u:0u);bump(Q.bodyWhy[why]);
             if(gapLo<tickGap)tickGap=gapLo;
             const float gm=gapLo/u;
             geoAll([&](HoldGeo& x){if(gm<x.minGapM){x.minGapM=gm;x.guid=b.guid;x.minNs=now;x.minDistM=gapExact?gm:-1.f;}});
             // the near ring: a creature's gap under 30 cm opens an entry (once until it is over 50 cm again)
             bool open=false;unsigned slot=0;for(unsigned k=0;k<s.geoNearOpen.size();++k)if(s.geoNearOpen[k]==b.guid){open=true;slot=k;break;}
             if(open&&gm>.50f){s.geoNearOpen[slot]=0;s.geoNearOpenNs[slot]=0;}
             else if(!open&&gm<.30f){unsigned k0=0;for(unsigned k=0;k<s.geoNearOpen.size();++k){if(!s.geoNearOpen[k]){k0=k;break;}if(s.geoNearOpenNs[k]<s.geoNearOpenNs[k0])k0=k;}
                s.geoNearOpen[k0]=b.guid;s.geoNearOpenNs[k0]=now?now:1;
                auto& n=s.geoNear[s.geoNearNext];s.geoNearNext=(s.geoNearNext+1u)%unsigned(s.geoNear.size());
                n=State::GeoNear{};n.set=true;n.ns=now;n.guid=b.guid;n.gapCm=gm*100.f;n.distCm=gapExact?gm*100.f:-1.f;n.part=gapPart;
                n.kind=gapCap>=0?b.c[std::size_t(gapCap)].kind:0xff;n.why=why;n.hands=hands;
                n.propMps=v::length(vc)/u;n.moverMps=v::length(moverVel)/u;n.lagCm=std::max(s.grab[0].active?s.grab[0].lagM:0.f,s.grab[1].active?s.grab[1].lagM:0.f)*100.f;
                n.msIn=now>=g.startNs?std::uint32_t(std::min<std::uint64_t>((now-g.startNs)/1'000'000ull,0xffffffffull)):0u;n.eng=g.geo.eng;
                {const V3 nn=v::normalized(horizontal(v::sub(bc,com),s.tg.up));
                 const V3 vNow=v::add(vc,v::cross(w,v::sub(gapPart>=0?ob[std::size_t(gapPart)].c:com,com)));
                 n.a=v::dot(v::sub(vNow,b.vel),nn)/u;n.swing=v::dot(v::sub(vNow,moverVel),nn)/u;
                 for(unsigned k=0;k<2;++k)if(hands&(1u<<k)){const auto pk=s.armTrail[k].peak(nn,now,armWin);n.arm=std::max(n.arm,pk.mag);n.armDir=std::max(n.armDir,pk.along);}}}}
            const bool touch=geo||eng,apart=!eng&&!(bestPart>=0&&best<=shell+.03f*u);
            if(!touch&&!s.geoGate.known(b.guid,g.prop))continue; // no touch and no entry: nothing to judge or separate
            if(touch){if(eng&&!geo)bump(Q.engineOnly);else if(geo&&!eng)bump(Q.geoOnly);else bump(Q.both);}
            // the normal from the prop into the creature; the point's velocity now and last tick (the larger along it)
            V3 n=bestPart>=0?v::sub(capPt,boxPt):V3{};
            if(!(v::length(n)>1e-3f*u))n=horizontal(v::sub(bc,bestPart>=0?ob[std::size_t(bestPart)].c:com),s.tg.up);
            n=v::normalized(n);
            const V3 p=bestPart>=0?boxPt:com;
            const V3 vNow=v::add(vc,v::cross(w,v::sub(p,com)));
            const V3 vPrev=prevOk?v::add(prev.v,v::cross(prev.w,v::sub(p,com))):vNow;
            const float aNow=v::dot(v::sub(vNow,b.vel),n)/u,aPrev=v::dot(v::sub(vPrev,b.vel),n)/u;
            const V3 vP=v::dot(vPrev,n)>v::dot(vNow,n)?vPrev:vNow;
            const float a=v::dot(v::sub(vP,b.vel),n)/u,swing=v::dot(v::sub(vP,moverVel),n)/u;
            // the holding hands' arm peaks: the speed, the speed along n (the swing at the creature) and that peak's age
            float arm=0.f,armDir=0.f,armAge=-1.f;
            for(unsigned k=0;k<2;++k)if(hands&(1u<<k)){const auto pk=s.armTrail[k].peak(n,now,armWin);arm=std::max(arm,pk.mag);
                if(pk.alongAgeMs>=0.f&&(armAge<0.f||pk.along>armDir)){armDir=pk.along;armAge=pk.alongAgeMs;}}
            PropHitWhy fail{};const bool pass=v::finite(n)&&propGeoPass(a,swing,arm,armDir,s.lev.propHitMps,s.lev.propHitArmMps,s.lev.propHitArmDirMps,fail);
            const GeoVerdict gv=s.geoGate.step(b.guid,g.prop,now,touch,apart,pass,fail,std::uint64_t(double(s.lev.propHitWinMs)*1e6),std::uint64_t(double(s.lev.propHitCoolMs)*1e6));
            if(gv.start){bump(Q.touches);bump(Q.aHist[bin6(a)]);bump(Q.swingHist[bin6(swing)]);bump(Q.armHist[bin6(arm)]);bump(Q.armDirHist[bin6(armDir)]);
                bump(Q.armAgeHist[armAge<0.f?5u:armAge<50.f?0u:armAge<100.f?1u:armAge<150.f?2u:armAge<250.f?3u:armAge<400.f?4u:5u]);}
            if(gv.judged&&gv.why==PropHitNoArm&&arm>=s.lev.propHitArmMps)bump(Q.noArmDir);
            if(!gv.judged)continue;
            PropHitWhy why=gv.why;
            if(why==PropHitOk){
                if(b.race!=0)why=PropHitNotHuman;
                else if(b.actor==s.actor)why=PropHitPlayer;
                else if(slotActor(b.guid)!=b.actor)why=PropHitStale;
                else if(!aliveNow(b.actor))why=PropHitDead;}
            bump(Q.why[why<PropHitWhyCount?why:0]);
            const bool strong=why==PropHitOk&&std::min(a,swing)>=s.lev.propHitStrongMps;
            if(gv.start||why==PropHitOk){
                State::GeoRec* rec=nullptr;
                if(!gv.start)for(unsigned k=0;k<s.geoRing.size();++k){auto& x=s.geoRing[(s.geoNext+s.geoRing.size()-1u-k)%s.geoRing.size()];if(x.set&&x.guid==b.guid){rec=&x;break;}}
                if(!rec){rec=&s.geoRing[s.geoNext];s.geoNext=(s.geoNext+1u)%unsigned(s.geoRing.size());}
                *rec=State::GeoRec{};rec->set=true;rec->ns=now;rec->guid=b.guid;rec->part=bestPart>=0?bestPart:-1;
                if(bestCap>=0){rec->zone=b.c[std::size_t(bestCap)].zone;rec->kind=b.c[std::size_t(bestCap)].kind;}
                rec->why=std::uint8_t(why);rec->strong=strong?1:0;rec->engine=eng?1:0;rec->distCm=bestPart>=0?best/u*100.f:-1.f;rec->a=a;rec->aNow=aNow;rec->aPrev=aPrev;
                rec->swing=swing;rec->arm=arm;rec->armDir=armDir;rec->armAgeMs=armAge;rec->propMps=v::length(vc)/u;rec->moverMps=v::length(moverVel)/u;rec->bodyMps=v::length(b.vel)/u;}
            if(why!=PropHitOk)continue;
            V3 dir=horizontal(v::sub(vP,b.vel),s.tg.up);if(!(v::length(dir)>.5f))dir=horizontal(n,s.tg.up);
            PropHitRequest r{};r.serial=++s.propSerial;r.ns=now;r.actor=b.actor;r.guid=b.guid;r.point=p;r.n=n;r.dir=dir;r.speed=a;r.ko=s.lev.propKo;
            r.strong=strong?1:0;r.hand=int(h);r.hands=hands;
            publishPropHit(r);bump(Q.sent);if(strong)bump(Q.strong);}
        if(anyBody){geoAll([](HoldGeo& x){++x.bodyTicks;});
            const float cm=tickGap/u*100.f;bump(Q.gapCmHist[cm<0.f?0u:cm<10.f?1u:cm<25.f?2u:cm<50.f?3u:cm<100.f?4u:cm<200.f?5u:6u]);
            geoAll([&](HoldGeo& x){x.lastGapM=tickGap/u;});}
        prev={g.prop,com,vc,w,now};}
    const auto us=(nowNs()-t0)/1000u;bump(Q.usSum,us);bump(Q.usN);maxOf(Q.usMax,us);
}
// THROW: each flight every tick: the level slot still holds the prop and its collider is the launch's (else stale / inactive), no hand
// holds it again, the mode and simulator unchanged; then flightStep (rest, timeout, hits). The impact scan words of the live flights.
inline void flightTick(State& s,std::uint64_t now) noexcept {
    const float u=std::max(s.upm,1e-3f);
    for(unsigned i=0;i<2;++i){auto& f=s.flights[i];if(!f.on){propScanInst[2u+i]=0;continue;}
        FlightEnd why=FlightNone;std::uintptr_t col=0;
        if(s.mode!=ModeOn||s.lev.throwMode!=2)why=FlightMode;
        else if(f.sim!=s.sim||f.level!=s.level)why=FlightLevel;
        else if(!levelHolds(s.level,f.levelIndex,f.prop))why=FlightStale;
        else{for(const auto& g:s.grab)if(g.active&&g.prop==f.prop)why=FlightRegrab;
            if(why==FlightNone){col=activeColliderAt(s.level,f.levelIndex,f.prop);if(!col||col!=f.col)why=FlightInactive;}}
        if(why==FlightNone){const V3 c=vec3(col+off::colliderPos),vel=vec3(col+off::colliderVel);const float sp=v::length(vel)/u;
            if(std::isfinite(sp))f.maxMps=std::max(f.maxMps,sp);if(v::finite(c))f.lastCom=c;
            why=flightStep(f.slowSince,sp,f.t0,now,s.lev.throwFlightMs,f.hits);}
        if(why!=FlightNone){endFlight(s,i,why,now);continue;}
        propScanInst[2u+i]=f.prop;}
}
// THROW: the creatures near each prop in flight, the held prop's geometric test (part boxes against this tick's bone capsules, or the
// engine's contact of the prop with the creature's mover) with the flight's own pass: the point's speed into the creature (the larger of
// now and last tick along the normal) at throwHitMps or more (the arm was judged at the release). The gate is the held prop's (per
// creature and prop: separation edge, window, cool-down), so a held hit and the throw that follows never hit twice. A prop of propHitKg
// or more sends the player's strike (PropHitRequest, thrown: KO x throwKo x the mass scale, strong from kg x a); a lighter one never does:
// lever throwLight 1 = the open hand's slap reaction (thrownTouchRequest, thrown: the creature itself as the attacker, no damage, its speed
// at most throwTapMps; VrMelee starts no node watch and no haptic for it) on a head / neck / chest capsule at throwTapMps or more, through
// the slap gate.
inline void flightGeoTick(State& s,std::uint64_t now,const std::array<std::array<PropContact,3>,2>& contacts) noexcept {
    auto& T=stats.throws;const float u=std::max(s.upm,1e-3f);const auto t0=nowNs();
    const std::uint32_t nb=std::min<std::uint32_t>(s.vb.count,std::uint32_t(vbodyMax));
    const float age=s.vb.ns&&now>s.vb.ns?std::min(float(double(now-s.vb.ns)*1e-9),.05f):0.f;
    bool any=false;
    for(unsigned i=0;i<2;++i){auto& f=s.flights[i];if(!f.on)continue;
        const auto col=activeColliderAt(s.level,f.levelIndex,f.prop);if(!col||col!=f.col)continue;any=true;
        const V3 com=vec3(col+off::colliderPos),vc=vec3(col+off::colliderVel),w=vec3(col+off::colliderAngVel);
        const bool prevOk=f.lastNs&&now>f.lastNs&&now-f.lastNs<50'000'000ull;const V3 pv=f.lastV,pw=f.lastW;
        f.lastV=vc;f.lastW=w;f.lastNs=now;
        float m[16]{};const GrabCache* gc=propParts(s,f.hand,f.prop,m,false);if(!gc)continue;
        std::array<Obb,24> ob{};std::array<float,24> rad{};unsigned no=0;
        for(unsigned k=0;k<gc->count&&k<ob.size();++k)if(partObb(m,gc->parts[k].m,gc->parts[k].bmin,gc->parts[k].bmax,ob[no])){rad[no]=v::length(ob[no].h);++no;}
        std::array<bool,vbodyMax> engineTouch{};
        for(const auto& c:contacts[i]){if(!c.mover||c.mover==s.moverInstAddr)continue;std::uintptr_t ud{},actor{};std::uint8_t kind{0xff};
            if(!get(c.mover+off::instUser,ud)||!ud||!get(ud+off::userKind,kind)||kind!=0||!get(ud+off::userActor,actor)||!actor)continue;
            for(std::uint32_t b=0;b<nb;++b)if(s.vb.b[b].actor==actor)engineTouch[b]=true;}
        const float shell=s.lev.propHitShellM*u,hyst=(s.lev.propHitShellM+.03f)*u;
        for(std::uint32_t bi=0;bi<nb;++bi){const auto& b=s.vb.b[bi];if(!b.guid||!b.count)continue;
            const V3 off=v::mul(b.vel,age),bc=v::add(b.centre,off);const unsigned nc=std::min<unsigned>(b.count,unsigned(vbodyCaps));
            float best=1e30f;int bestPart=-1,bestCap=-1;V3 boxPt{},capPt{};
            for(unsigned j=0;j<no;++j){
                const float sg=v::length(v::sub(ob[j].c,bc))-rad[j]-b.radius;
                if(sg>hyst){const float cm=sg/u*100.f;if(cm<f.gapCm)f.gapCm=cm;continue;}
                for(unsigned k=0;k<nc;++k){const auto& cp=b.c[k];const V3 ca=v::add(cp.a,off),cb=v::add(cp.b,off);
                    const float cg=v::length(v::sub(ob[j].c,closestOnSegment(ca,cb,ob[j].c)))-rad[j]-cp.r;if(cg>hyst)continue;
                    const SegObb r=segObbDist(ca,cb,ob[j],12);const float dd=r.d-cp.r;
                    if(dd<best){best=dd;bestPart=int(j);bestCap=int(k);boxPt=r.boxPt;capPt=r.segPt;}}}
            if(bestPart>=0){const float cm=best/u*100.f;if(cm<f.gapCm)f.gapCm=cm;}
            const bool eng=engineTouch[bi],geo=bestPart>=0&&best<=shell;
            const bool touch=geo||eng,apart=!eng&&!(bestPart>=0&&best<=shell+.03f*u);
            if(!touch&&!s.geoGate.known(b.guid,f.prop))continue;
            V3 n=bestPart>=0?v::sub(capPt,boxPt):V3{};
            if(!(v::length(n)>1e-3f*u))n=horizontal(v::sub(bc,com),s.tg.up);
            n=v::normalized(n);
            const V3 p=bestPart>=0?boxPt:com;
            const V3 vNow=v::add(vc,v::cross(w,v::sub(p,com)));const V3 vPrev=prevOk?v::add(pv,v::cross(pw,v::sub(p,com))):vNow;
            const V3 vP=v::dot(vPrev,n)>v::dot(vNow,n)?vPrev:vNow;
            const float a=v::dot(v::sub(vP,b.vel),n)/u;
            const bool pass=v::finite(n)&&std::isfinite(a)&&a>=s.lev.throwHitMps;
            const GeoVerdict gv=s.geoGate.step(b.guid,f.prop,now,touch,apart,pass,PropHitSlow,std::uint64_t(double(s.lev.propHitWinMs)*1e6),
                                               std::uint64_t(double(s.lev.propHitCoolMs)*1e6));
            if(!gv.judged)continue;
            PropHitWhy why=gv.why;
            if(why==PropHitOk){if(b.race!=0)why=PropHitNotHuman;else if(b.actor==s.actor)why=PropHitPlayer;else if(slotActor(b.guid)!=b.actor)why=PropHitStale;
                else if(!aliveNow(b.actor))why=PropHitDead;}
            bump(T.why[why<PropHitWhyCount?why:0]);
            auto& hr=s.throwHits[s.throwHitNext];s.throwHitNext=(s.throwHitNext+1u)%unsigned(s.throwHits.size());
            hr=State::ThrowHit{};hr.ns=now;hr.guid=b.guid;hr.why=std::uint8_t(why);hr.a=a;hr.kg=f.kg;hr.light=f.heavy?0:1;hr.engine=eng?1:0;
            if(bestCap>=0){hr.zone=b.c[std::size_t(bestCap)].zone;hr.kind=b.c[std::size_t(bestCap)].kind;}
            if(why!=PropHitOk)continue;
            ++f.hits;
            V3 dir=horizontal(v::sub(vP,b.vel),s.tg.up);if(!(v::length(dir)>.5f))dir=horizontal(n,s.tg.up);
            if(f.heavy){const bool strong=throwStrong(f.kg,a,s.lev.propHitStrongMps,s.lev.throwStrongKgMps);
                PropHitRequest r{};r.serial=++s.propSerial;r.ns=now;r.actor=b.actor;r.guid=b.guid;r.point=p;r.n=n;r.dir=dir;r.speed=a;
                r.ko=s.lev.propKo*s.lev.throwKo*throwMassScale(f.kg,s.lev.propHitKg);r.strong=strong?1:0;r.hand=int(f.hand);r.hands=0;r.thrown=1;r.kg=f.kg;
                publishPropHit(r);bump(T.hits);if(strong)bump(T.hitsStrong);hr.strong=strong?1:0;hr.ko=r.ko;hr.sent=1;continue;}
            // a light prop: never the player's strike
            const int capKind=bestCap>=0?int(b.c[std::size_t(bestCap)].kind):-1;
            if(!s.lev.throwLight||!throwTapZone(capKind)||!(a>=throwTapMps)||!touchOn(s)||!s.slaps.allow(b.guid,now)){bump(T.tapRefused);continue;}
            TouchRequest r{};r.serial=++s.touchSerial;r.ns=now;r.actor=b.actor;r.guid=b.guid;r.kind=TouchSlap;r.capKind=std::uint8_t(capKind);r.part=bestCap;
            r.hand=int(f.hand);r.point=p;r.n=n;r.pivot=b.centre;r.a=throwTapA(a);r.speed=r.a;r.side=v::dot(dir,v::normalized(b.rowA));r.upm=s.upm;r.thrown=1;
            r.pushGain=s.lev.pushGain;r.pushMaxM=s.lev.pushMaxM;r.pushCapM=s.lev.pushCapM;r.slapYawRad=s.lev.slapYawRad;r.slapReact=s.lev.slapReact?1:0;
            r.push=s.lev.npcPush?1:0;r.attacker=std::uint8_t(std::min(s.lev.slapAttacker,unsigned(OpenMsgNone)));
            thrownTouchRequest.store(r);++f.taps;bump(T.taps);hr.sent=1;}}
    if(any){const auto us=(nowNs()-t0)/1000u;bump(T.geoUsSum,us);bump(T.geoUsN);maxOf(T.geoUsMax,us);}
}
inline void propHitTick(State& s,std::uint64_t now) noexcept {
    auto& P=stats.propHit;const float u=std::max(s.upm,1e-3f);
    const bool on=s.mode==ModeOn&&s.lev.propHit&&touchOn(s)&&s.vbFresh&&s.vb.count>0&&s.actor;
    // the scan is armed for the hit (creatures near) and, lever grabplayerguard, for every held heavy prop (the pair's guard reads the
    // player's capsule from it)
    const bool guard=s.mode==ModeOn&&s.lev.grabPlayerGuard;
    for(unsigned h=0;h<2;++h){const auto& g=s.grab[h];const bool heavy=g.active&&g.cls==classNav&&g.propKg>=s.lev.propHitKg;
        propScanInst[h]=(on||guard)&&heavy?g.prop:0;}
    if(on&&(propScanInst[0]||propScanInst[1]))bump(P.armedTicks);
    // the hands' arm velocities (velArm, m/s): the geometric hit's arm gate reads their peaks
    for(unsigned h=0;h<2;++h){const auto& t=s.tg.hand[h];s.armTrail[h].push(t.valid&&t.armOk?v::mul(t.velArm,1.f/u):V3{0,0,0},now);}
    {const float u0=std::max(s.upm,1e-3f);bool heavy=false;float sp=-1.f; // a heavy prop held: why the prop hit could not arm, its speed
     for(unsigned h=0;h<2;++h){const auto& g=s.grab[h];if(!(g.active&&g.cls==classNav&&g.propKg>=s.lev.propHitKg))continue;heavy=true;
        if(const auto col=activeCollider(s,g.prop)){const float x=v::length(vec3(col+off::colliderVel))/u0;if(std::isfinite(x))sp=std::max(sp,x);}}
     if(heavy){auto& G=stats.grab;bump(G.heavyHeldTicks);if(!(s.vbFresh&&s.vb.count>0))bump(G.heavyNoBodies);
        const float nm=s.vbFresh?s.vb.nearestM:-1.f;bump(G.heavyNearestHist[nm<0.f?4u:nm<1.6f?0u:nm<3.f?1u:nm<6.f?2u:3u]);
        if(sp>=0.f)bump(G.heavySpeedHist[sp<1.f?0u:sp<2.f?1u:sp<3.f?2u:sp<4.f?3u:4u]);}}
    const auto contacts=s.propContacts;s.propContacts={};
    flightTick(s,now);const auto fcontacts=s.flightContacts;s.flightContacts={};
    if(!on)return;
    if(s.lev.propHitGeo){propGeoTick(s,now,contacts);flightGeoTick(s,now,fcontacts);return;}
    bool any=false;
    for(const auto& c:contacts){if(!c.mover)continue;any=true;
        PropHitSample smp{};smp.ns=now;smp.nUp=v::dot(c.n,v::normalized(s.tg.up));
        const auto judged=[&](PropHitWhy why){bump(P.why[why]);smp.why=std::uint8_t(why);s.propSamples[s.propSampleNext]=smp;s.propSampleNext=(s.propSampleNext+1u)%unsigned(s.propSamples.size());};
        if(c.mover==s.moverInstAddr){judged(PropHitPlayer);continue;}
        std::uintptr_t ud{},actor{};std::uint8_t kind{0xff};std::uint32_t guid{};
        if(!get(c.mover+off::instUser,ud)||!ud||!get(ud+off::userKind,kind)||kind!=0||!get(ud+off::userActor,actor)||!actor||!get(actor+off::actorGuid,guid)||
           slotActor(guid)!=actor){judged(PropHitNoActor);continue;}
        smp.guid=guid;
        if(actor==s.actor){judged(PropHitPlayer);continue;}
        if(!aliveNow(actor)){judged(PropHitDead);continue;}
        const VBody* body=nullptr;for(std::uint32_t i=0;i<std::min<std::uint32_t>(s.vb.count,std::uint32_t(vbodyMax));++i)if(s.vb.b[i].actor==actor){body=&s.vb.b[i];break;}
        if(!body){judged(PropHitNotCand);continue;}
        if(body->race!=0){judged(PropHitNotHuman);continue;}
        const V3 rel=v::sub(c.vPoint,body->vel);const float approach=v::dot(rel,c.n)/u;smp.approach=approach;smp.speed=v::length(rel)/u;
        const float swing=v::dot(v::sub(c.vPoint,s.hand[c.hand&1u].moverVel),c.n)/u;smp.swing=swing;
        PropHitWhy why=s.propGate.step(guid,now,propHitGateSpeed(approach,swing),s.lev.propHitMps,std::uint64_t(double(s.lev.propHitCoolMs)*1e6));
        if(why==PropHitSlow)why=propHitSlowWhy(approach,swing,s.lev.propHitMps);
        if(why!=PropHitNotEdge){bump(P.edges);bump(P.speedHist[propSpeedBin(approach)]);}
        if(why!=PropHitOk){judged(why);continue;}
        V3 dir=horizontal(rel,s.tg.up);if(v::length(dir)<.5f)dir=horizontal(c.n,s.tg.up);
        PropHitRequest r{};r.serial=++s.propSerial;r.ns=now;r.actor=actor;r.guid=guid;r.point=c.pos;r.n=c.n;r.dir=dir;r.speed=approach;r.ko=s.lev.propKo;
        r.strong=approach>=s.lev.propHitStrongMps?1:0;r.hand=int(c.hand);
        for(unsigned h=0;h<2;++h)if(s.grab[h].active&&s.grab[h].prop==s.grab[c.hand&1u].prop)r.hands|=std::uint8_t(1u<<h);
        publishPropHit(r);bump(P.sent);if(r.strong)bump(P.strong);smp.strong=r.strong;judged(PropHitOk);}
    // no contact this tick while the prop moves at the hit speed with a creature's centre near it
    if(!any)for(unsigned h=0;h<2;++h){if(!propScanInst[h])continue;const auto& g=s.grab[h];
        const auto col=activeCollider(s,g.prop);if(!col)continue;const V3 com=vec3(col+off::colliderPos);
        if(!(v::length(vec3(col+off::colliderVel))/u>=s.lev.propHitMps))continue;
        for(std::uint32_t i=0;i<std::min<std::uint32_t>(s.vb.count,std::uint32_t(vbodyMax));++i)if(v::length(v::sub(s.vb.b[i].centre,com))/u<1.2f){bump(P.nearFast);break;}}
}
// NPC (main thread, outside the step). With VrMelee's virtual bodies: a hand touches the creature its command was clipped against in the
// last 2 substeps, a press is 1 cm or more into it. A press bumps every tick (PressGate), a brush on its rising edge (BumpGate); a new
// touch of an open hand on the head, neck or chest at slapMps or more asks VrMelee for a slap instead (SlapGate; never a horse). Without
// bodies: the coarse capsule above the feet of the nearby list (BumpGate).
// PASSIVE: an open hand's contact episode closed into the ring (PassiveRec).
inline void closePassive(State& s,unsigned h,std::uint64_t now) noexcept {
    auto& ep=s.hand[h&1u].pep;if(!ep.on)return;
    auto& r=s.passiveRing[s.passiveNext];r=PassiveRec{};r.set=true;r.ns=now;r.guid=ep.guid;r.hand=std::uint8_t(h&1u);r.verdict=ep.verdict;r.capKind=ep.capKind;
    r.ms=now>=ep.startNs?std::uint32_t(std::min<std::uint64_t>((now-ep.startNs)/1000000ull,0xffffffffull)):0u;
    r.aMax=ep.aMax;r.armInMax=ep.armInMax;r.armInAvg=ep.n?float(ep.armInSum/double(ep.n)):0.f;r.locoInMax=ep.locoInMax;r.turnMps=ep.turnMax;r.depthMaxCm=ep.depthMax*100.f;
    r.armStepCm=ep.armStepM*100.f;r.depthStepCm=ep.depthStepM*100.f;r.bumpsSent=ep.bumpsSent;r.bumpsSkipped=ep.bumpsSkipped;
    {const V3 up=v::normalized(s.tg.up);const V3 d=v::sub(ep.centre1,ep.centre0);const float m=v::length(v::sub(d,v::mul(up,v::dot(d,up))))/std::max(s.upm,1e-3f)*100.f;
     r.npcMovedCm=std::isfinite(m)?m:0.f;}
    s.passiveNext=(s.passiveNext+1u)%unsigned(s.passiveRing.size());ep=HandState::PassiveEp{};}
inline void npcTick(State& s,std::uint64_t now) noexcept {
    auto& N=stats.npc;auto& PV=stats.passive;
    // this tick's arm values of each hand on a body (preUpdate's substeps), taken and cleared
    std::array<float,2> armInT{},armStepT{};std::array<bool,2> armT{};
    for(unsigned h=0;h<2;++h){auto& hs=s.hand[h];armInT[h]=hs.armTick?hs.armInTick:0.f;armStepT[h]=hs.armStepTickM;armT[h]=hs.armTick;
        if(hs.armUnknownTick)bump(PV.armUnknown);hs.armTick=false;hs.armInTick=0.f;hs.armStepTickM=0.f;hs.armUnknownTick=false;}
    const bool pas=s.lev.npcPassive;
    const bool on=s.mode==ModeOn&&groupOk[GroupBump]&&engine.broadcastBump&&s.actor&&(s.lev.npcBump||s.lev.npcBody);
    bool live=false;for(const auto& hs:s.hand)live=live||(hs.present&&hs.machine.state==BodyLive&&hs.haveSolved);
    // a held heavy prop's geometric hit keeps the bodies near it (VrMelee: Palms::heavy)
    const bool flying=(s.flights[0].on&&(s.flights[0].heavy||s.lev.throwLight))||(s.flights[1].on&&(s.flights[1].heavy||s.lev.throwLight)); // THROW
    const bool heavyGeo=s.mode==ModeOn&&s.lev.propHit&&s.lev.propHitGeo&&s.lev.npcBody&&s.actor&&(heavyGeoHold(s,0)||heavyGeoHold(s,1)||flying);
    nearWanted.store((on&&live)||heavyGeo,std::memory_order_relaxed);nearScanTicks.store(s.lev.npcScanTicks,std::memory_order_relaxed);
    if(!on||!live)for(unsigned h=0;h<2;++h)closePassive(s,h,now);
    nearHorses.store(s.lev.npcBody&&s.lev.npcHorse,std::memory_order_relaxed);
    if(!on||!live)return;
    std::uint32_t player{};const bool playerOk=get(s.actor+off::actorGuid,player)&&slotActor(player)==s.actor;
    const auto send=[&](std::uintptr_t actor,std::uint32_t guid)->bool{
        if(!playerOk||slotActor(guid)!=actor){bump(N.staleGuid);return false;}
        if(!aliveNow(actor)){bump(N.notAlive);return false;}
        const std::uint32_t msg[2]{off::msgBumpedInto,player};
        engine.broadcastBump(reinterpret_cast<void*>(actor),msg);bump(N.bumps);bumpSentNs.store(now?now:1,std::memory_order_relaxed);
        {std::uintptr_t anim{};std::uint32_t bumper{};
         if(get(actor+off::actorAnim,anim)&&anim&&get(anim+off::animBumper,bumper))bump(bumper==player?N.bumpHeld:N.bumpNotHeld);}
        return true;};
    // a walk-in bump VrMelee asked for (a stumble waiting on a frozen or suspended mover): once per serial, within 300 ms
    {BumpAsk q{};if(bumpAsk.load(q)&&q.serial&&q.serial!=s.bumpAskSeen){s.bumpAskSeen=q.serial;
        if(fresh(now,q.ns,300'000'000ull)&&send(q.actor,q.guid))bump(PV.busyBumps);else bump(PV.busyBumpRefused);}}
    const float u=std::max(s.upm,1e-3f);
    if(s.lev.npcBody&&s.vbFresh){
        struct Touch {std::uint32_t guid{};bool press{},edge{};};std::array<Touch,2> ht{};
        for(unsigned h=0;h<2;++h){auto& hs=s.hand[h];
            if(hs.present&&hs.machine.state==BodyLive&&hs.vhit.hit&&s.step>=hs.vhitStep&&s.step-hs.vhitStep<=2){ht[h].guid=hs.vhitGuid;ht[h].press=hs.vhit.depth>=.01f*u;}
            if(s.grab[h].active&&ht[h].guid){bump(N.grabSkips);ht[h]={};} // a hand that holds a prop neither slaps nor presses
            ht[h].edge=ht[h].guid&&ht[h].guid!=hs.npcTouchGuid;hs.npcTouchGuid=ht[h].guid;}
        // PASSIVE: the contact episodes (the ring) and the hand-ticks on a body with the arm still
        for(unsigned h=0;h<2;++h){auto& hs=s.hand[h];auto& ep=hs.pep;
            if(ep.on&&ep.guid!=ht[h].guid)closePassive(s,h,now);
            if(!ht[h].guid)continue;
            const VBody* b=nullptr;for(std::uint32_t i=0;i<std::min<std::uint32_t>(s.vb.count,std::uint32_t(vbodyMax));++i)if(s.vb.b[i].guid==ht[h].guid){b=&s.vb.b[i];break;}
            if(!ep.on){ep=HandState::PassiveEp{};ep.on=true;ep.guid=ht[h].guid;ep.capKind=hs.vhit.kind;ep.startNs=now;if(b)ep.centre0=b->centre;ep.centre1=ep.centre0;}
            if(b)ep.centre1=b->centre;
            ep.aMax=std::max(ep.aMax,hs.vhitApproach);ep.locoInMax=std::max(ep.locoInMax,hs.vhitLocoIn);ep.turnMax=std::max(ep.turnMax,hs.vhitTurn);
            ep.depthMax=std::max(ep.depthMax,hs.vhit.depth/std::max(s.upm,1e-3f));
            if(armT[h]){ep.armInMax=std::max(ep.armInMax,armInT[h]);ep.armInSum+=double(armInT[h]);++ep.n;}
            if(pas&&!hs.touchActive&&!ht[h].edge)bump(PV.passiveTicks);}
        const bool tOn=touchOn(s);
        // one touch request of a hand: the peaks of its first substeps on the body, the push levers, the message's attacker; asked = a slap
        // or a shove of this tick (one request per hand and tick reaches VrMelee)
        std::array<bool,2> asked{};
        // a request is built (build) and handed over (sendReq: a new serial, the clock of the hand-over); a held slap is built at its edge
        // and handed over later
        const auto sendReq=[&](unsigned h,TouchRequest r){
            if(r.kind!=TouchPress)asked[h&1u]=true;
            r.serial=++s.touchSerial;r.ns=now;
            if(r.kind==TouchShove)bump(N.tierSent[r.tier<TierCount?r.tier:0]);
            touchRequest[h&1u].store(r);};
        const auto build=[&](unsigned h,const VBody& b,TouchKind k,bool both,bool strong,bool escalated,bool windowed,PushTier tier)->TouchRequest{
            const auto& hs=s.hand[h];TouchRequest r{};r.ns=now;r.actor=b.actor;r.guid=b.guid;r.kind=k;
            r.capKind=std::uint8_t(std::clamp(hs.vhit.kind,0,255));r.both=both?1:0;r.strong=strong?1:0;r.part=hs.vhit.cap;r.hand=int(h);
            r.point=hs.vhit.point;r.n=v::mul(hs.vhit.n,-1.f);r.pivot=b.centre;r.a=hs.vhitApproach;r.speed=windowed?std::max(hs.vhitSpeed,hs.edgeSpeed):hs.vhitSpeed;
            r.tangent=hs.vhitTangent;r.swing=hs.vhitSwing;r.facing=hs.vhitFacing;r.side=hs.vhitSide;r.depthM=hs.vhit.depth/u;r.upm=s.upm;
            r.pushGain=s.lev.pushGain;r.pushMaxM=s.lev.pushMaxM;r.pushCapM=s.lev.pushCapM;r.slapYawRad=s.lev.slapYawRad;
            r.slapReact=s.lev.slapReact?1:0;r.push=s.lev.npcPush?1:0;
            // the message wanted (OpenTouchMsg); VrMelee's openTouchPlan never sends one as the player nor with damage
            r.attacker=std::uint8_t(std::min(k==TouchSlap?s.lev.slapAttacker:s.lev.pushAttacker,unsigned(OpenMsgNone)));r.escalated=escalated?1:0;r.windowed=windowed?1:0;
            r.tier=std::uint8_t(tier);r.arm=hs.vhitArm;r.pushMedium=std::uint8_t(s.lev.pushMedium);r.pushImpulse=s.lev.pushImpulse;r.pushKick=s.lev.pushKick?1:0;
            r.vel=hs.vhitVel;r.pushKickK=s.lev.pushKickK;r.pushKickMaxNs=s.lev.pushKickMaxNs;
            r.pushGrade=s.lev.pushGrade?1:0;r.pushKick2=s.lev.pushKick2?1:0;r.pushDownMps=s.lev.pushDownMps;r.bodyVel=b.vel;
            r.pushEscGrade=s.lev.pushEscGrade?1:0;r.pushBusyWaitMs=s.lev.pushBusyWait?s.lev.pushBusyWaitMs:0.f;r.pushFront=std::uint8_t(s.lev.pushFront);
            r.pushFrontImp=s.lev.pushFrontImp;r.pushKickHigh=s.lev.pushKickHigh?1:0;
            r.touchNs=hs.touchStartNs;r.pushFrozenMsg=s.lev.pushFrozenMsg?1:0;r.pushSide=std::uint8_t(s.lev.pushSide);r.pushBehindImp=s.lev.pushBehindImp;
            r.pushKnock=std::uint8_t(s.lev.pushKnock);r.pushFrozenHoldMs=s.lev.pushFrozenHold?s.lev.pairMs:0.f;r.pushKnockFlip=std::uint8_t(s.lev.pushKnockFlip);
            r.pushKnockPick=std::uint8_t(s.lev.pushKnockPick);r.pushKnockPickFwd=s.lev.pushKnockPickFwd;r.pushKnockSpine=s.lev.pushKnockSpine?1:0;r.pushKnockPickMsg=std::uint8_t(s.lev.pushKnockPickMsg);
            r.pushKnockTough=s.lev.pushKnockTough?1:0;r.pushKnockPickFunc=s.lev.pushKnockPickFunc?1:0;r.knockOffMs=s.lev.knockOffMs;
            r.pushKickServo=s.lev.pushKickServo?1:0;r.pushKickEarly=s.lev.pushKickEarly?1:0;
            // PASSIVE: an escalated press or a press is graded on the arm's own approach; a shove or slap (the arm gate passed) on the arm's
            // plus the player's step into the body (passiveGradeA); the direction is the arm's motion
            if(pas){const bool deliberate=(k==TouchShove||k==TouchSlap)&&!escalated;r.a=passiveGradeA(r.a,hs.vhitArmIn,hs.vhitLocoIn,deliberate);r.vel=hs.vhitVelArm;}
            r.thrust=pushThrust(r.a,r.arm,s.lev.pushFirmMps,s.lev.pushArmMps)?1:0;
            return r;};
        const auto request=[&](unsigned h,const VBody& b,TouchKind k,bool both,bool strong,bool escalated,bool windowed,PushTier tier){
            sendReq(h,build(h,b,k,both,strong,escalated,windowed,tier));};
        // a held slap of hand h that the other hand joined: one pair push (the shove's message wanted, its tier from the slap's edge)
        const auto pairFromHeld=[&](unsigned h,float gapMs){
            auto& hs=s.hand[h];TouchRequest r=hs.slapPend.r;
            r.kind=TouchShove;r.pair=1;r.both=1;r.strong=0;r.slapHeld=1;r.pairGapMs=gapMs;
            r.attacker=std::uint8_t(std::min(s.lev.pushAttacker,unsigned(OpenMsgNone)));
            const PushTier tier=pushTier(false,r.a,r.depthM,r.arm,s.lev.pushFirmMps,s.lev.pushFirmM,s.lev.pushArmMps);r.tier=std::uint8_t(tier);
            if(hs.slapPend.edgeSample>=0){auto& e=s.edgeSamples[std::size_t(hs.slapPend.edgeSample)];if(e.guid==r.guid){e.verdict=std::uint8_t(TouchShove);e.tier=std::uint8_t(tier);e.pair=true;}}
            sendReq(h,r);hs.slapPend=HandState::SlapPend{};
            if(hs.touchGuid==r.guid&&hs.touchKind==TouchSlap){hs.touchKind=TouchShove;hs.touchClassNs=now;hs.touchSlapped=false;}
            bump(N.slapToPair);bump(N.shoves);};
        const auto findBody=[&](std::uint32_t guid)->const VBody*{
            for(std::uint32_t i=0;i<std::min<std::uint32_t>(s.vb.count,std::uint32_t(vbodyMax));++i)if(s.vb.b[i].guid==guid)return &s.vb.b[i];return nullptr;};
        // a press that ended: its length, the push it asked, its depth, why
        const auto endPress=[&](HandState& hs,unsigned why){
            const auto n=hs.pressTicksN;bump(N.pressTicksHist[n<2?0u:n<4?1u:n<8?2u:n<16?3u:n<32?4u:5u]);
            const float cm=hs.pressAskedM*100.f;bump(N.pressCmHist[cm<1.f?0u:cm<3.f?1u:cm<6.f?2u:cm<12.f?3u:cm<24.f?4u:5u]);
            const float d=hs.pressDepthMax*100.f;bump(N.pressDepthHist[d<1.f?0u:d<2.f?1u:d<3.f?2u:d<5.f?3u:d<8.f?4u:5u]);
            bump(N.pressEnd[std::min(why,3u)]);bump(N.pressEndFacing[facingBin(hs.pressFacingLast)]);bump(N.pressFacingMin[facingBin(hs.pressFacingMin)]);
            hs.touchKind=TouchNone;hs.pressTicksN=0;hs.pressAskedM=0.f;hs.pressDepthMax=0.f;hs.pressFacingMin=1.f;};
        // the stroke peaks (lever pushpeak): a shove's stroke that went on 10 % or more above its edge's graded approach regrades the push
        // (the same hand's request with peak = 1 and a = the peak; VrMelee raises the push's aEff, never lowers it)
        for(unsigned h=0;h<2;++h){auto& hs=s.hand[h];if(!hs.peak.on)continue;
            const bool touching=ht[h].guid==hs.peak.guid;
            const auto& tp=s.tg.hand[h];const float armNow=!tp.valid?-1.f:pas?(tp.armOk?v::length(tp.velArm)/u:-1.f):v::length(tp.vel)/u;
            const int pk=strokePeakStep(hs.peak,touching,armT[h],armInT[h],hs.vhitLocoIn,now,std::uint64_t(double(s.lev.pushPeakMs)*1e6),armNow);
            if(pk==2){bump(PV.peakNone);continue;}
            if(pk!=1)continue;
            if(ht[h].edge||asked[h]){bump(PV.peakLost);continue;}
            TouchRequest r=hs.peakReq;r.edgeA=r.a;r.a=hs.peak.peak;r.peak=1;r.arm=std::max(r.arm,hs.peak.armPeak);
            r.thrust=pushThrust(r.a,r.arm,s.lev.pushFirmMps,s.lev.pushArmMps)?1:0;
            sendReq(h,r);bump(PV.peakSent);}
        // the slap windows: a touch the edge did not decide becomes a slap when the hand leaves the head fast enough without pressing
        for(unsigned h=0;h<2;++h){auto& hs=s.hand[h];const auto& t=s.tg.hand[h];
            if(hs.touchKind==TouchPress&&ht[h].guid!=hs.touchGuid)endPress(hs,2u);
            if(!hs.slapWin.open)continue;
            const bool touching=ht[h].guid&&ht[h].guid==hs.slapWinGuid,pressing=hs.touchKind==TouchPress&&hs.touchGuid==hs.slapWinGuid;
            const float arm=pas?(t.valid&&t.armOk?v::length(t.velArm)/u:0.f):t.valid?v::length(t.vel)/u:0.f;
            const int w=slapWindowStep(hs.slapWin,touching,pressing,std::isfinite(arm)?arm:0.f,touching?hs.vhitFacing:0.f,now,s.lev.slapMps,s.lev.slapWindowMs);
            if(std::isfinite(arm))hs.edgeSpeed=std::max(hs.edgeSpeed,arm);
            // graded: no window slap while the other hand is on the creature or had its edge on it within pairms (a two-hand push)
            const auto& os=s.hand[h^1u];
            const bool otherPair=s.lev.pushGrade&&touchPair(ht[h^1u].guid==hs.slapWinGuid,os.touchGuid==hs.slapWinGuid,os.touchStartNs,now,
                std::uint64_t(double(s.lev.pairMs)*1e6));
            if(w==1&&otherPair){bump(N.windowNone);bump(N.slapToPair);}
            else if(w==1){const VBody* b=findBody(hs.slapWinGuid);
                if(b&&tOn&&s.slaps.allow(b->guid,now)){request(h,*b,TouchSlap,false,false,false,true,TierLight);hs.touchKind=TouchSlap;hs.touchClassNs=now;hs.touchSlapped=true;
                    bump(N.slaps);bump(N.windowSlaps);if(hs.edgeSample>=0)s.edgeSamples[std::size_t(hs.edgeSample)].verdict=std::uint8_t(TouchSlap);}
                else bump(N.touchGated);}
            else if(w==2)bump(N.windowNone);}
        // the held slaps: handed over once slapholdms passed with no second hand (the slap gate was passed at the edge); dropped when the
        // touches are off, the hand touched another creature or the same hand's own shove on this creature followed (slapDropped). A
        // follow-through that touches the same creature again (a new edge with no verdict) or rests on it (a press) keeps the slap.
        for(unsigned h=0;h<2;++h){auto& hs=s.hand[h];auto& pd=hs.slapPend;if(!pd.set)continue;
            if(slapHoldDrops(tOn&&s.lev.pushGrade,hs.touchGuid,pd.r.guid,hs.touchKind==TouchShove)){
                if(tOn&&s.lev.pushGrade)bump(N.slapDropped);pd=HandState::SlapPend{};continue;}
            // the other hand on the same creature while the slap waits (any touch of it): a pair push
            if(ht[h^1u].guid==pd.r.guid){const auto& os=s.hand[h^1u];
                const std::uint64_t g=os.touchGuid==pd.r.guid?(os.touchStartNs>=pd.ns?os.touchStartNs-pd.ns:pd.ns-os.touchStartNs):0ull;
                bump(N.pairs);pairFromHeld(h,os.touchGuid==pd.r.guid?float(double(g)*1e-6):-1.f);continue;}
            if(now>=pd.ns&&double(now-pd.ns)<double(s.lev.slapHoldMs)*1e6)continue;
            sendReq(h,pd.r);bump(N.slaps);bump(N.slapHeldSent);pd=HandState::SlapPend{};}
        for(std::uint32_t i=0;i<std::min<std::uint32_t>(s.vb.count,std::uint32_t(vbodyMax));++i){
            const auto& b=s.vb.b[i];if(!b.guid)continue;
            bool touch=false,press=false,sent=false;
            for(unsigned h=0;h<2;++h)if(ht[h].guid==b.guid){touch=true;press=press||ht[h].press;}
            const bool both=ht[0].guid==b.guid&&ht[1].guid==b.guid;
            for(unsigned h=0;h<2;++h){
                if(ht[h].guid!=b.guid)continue;
                auto& hs=s.hand[h];const auto& t=s.tg.hand[h];
                const bool open=!hs.fist&&(!std::isfinite(t.squeeze)||t.squeeze<fistOff);
                if(ht[h].edge){ // a new touch: slap, shove, a slap window or nothing (a bump)
                    hs.touchGuid=b.guid;hs.touchStartNs=now;hs.touchKind=TouchNone;hs.touchSlapped=false;hs.touchDepth=0.f;hs.pressDoneM=0.f;
                    hs.slapWin=SlapWindow{};hs.pressAsk=PressAsk{};hs.edgeSample=-1;hs.edgeSpeed=hs.vhitSpeed;
                    // PASSIVE: the touch is active when the arm itself drove into the body (its bumps may go)
                    hs.touchActive=!pas||hs.vhitArmIn>=s.lev.activeMps;
                    if(pas){bump(hs.touchActive?PV.activeEdges:PV.passiveContacts);bump(PV.armInEdge[armBin(hs.vhitArmIn)]);bump(PV.locoInEdge[armBin(hs.vhitLocoIn)]);
                        bump(PV.turnEdge[armBin(hs.vhitTurn)]);}
                    if(!open)bump(N.closedHand);
                    else if(b.race==1)bump(N.horse);
                    else if(tOn){
                        TouchIn ti{};ti.capKind=hs.vhit.kind;ti.a=hs.vhitApproach;ti.speed=hs.vhitSpeed;ti.swing=hs.vhitSwing;ti.facing=hs.vhitFacing;
                        ti.side=hs.vhitSide;ti.tangent=hs.vhitTangent;ti.arm=hs.vhitArm;ti.armGate=pas;ti.armIn=hs.vhitArmIn;ti.armInMin=s.lev.shoveArmMps;
                        if(s.lev.pushFacingBody){const float fb=v::dot(v::normalized(hs.cmdRows[2]),horizontal(v::sub(b.centre,hs.vhit.point),s.tg.up));
                            ti.facingBody=std::isfinite(fb)?fb:-2.f;}
                        const bool graded=s.lev.pushGrade;
                        const TouchVerdict tv=touchVerdict(ti,s.lev.slapMps,s.lev.shoveMps,s.lev.npcSlap,s.lev.npcShove,true,graded);
                        if(tv.facingRescued&&tv.kind==TouchShove)bump(PV.facingRescued);
                        if(tv.shoveFail==ShoveFacing)bump(PV.facingFailKind[std::size_t(std::clamp(ti.capKind,0,7))]);
                        if(tv.shoveFail==ShoveArm)bump(PV.shoveRefusedArm);
                        if(tv.slapFail==SlapPush)bump(N.slapPushLike);
                        // the other hand on this creature: touching it now, its edge on it and the gap to it
                        const unsigned oh=h^1u;const auto& os=s.hand[oh];
                        const bool otherOn=ht[oh].guid==b.guid,otherEdge=os.touchGuid==b.guid&&now>=os.touchStartNs;
                        const std::uint64_t gapNs=otherEdge?now-os.touchStartNs:~0ull;
                        if(otherEdge&&gapNs<=300'000'000ull)bump(N.pairGapHist[pairGapBin(gapNs)]);
                        const float gapMs=otherEdge&&gapNs<=10'000'000'000ull?float(double(gapNs)*1e-6):-1.f;
                        if(tv.kind==TouchSlap){TouchIn t2=ti;t2.arm=0.f;if(touchVerdict(t2,s.lev.slapMps,s.lev.shoveMps,s.lev.npcSlap,s.lev.npcShove,true,graded).kind!=TouchSlap)bump(N.slapByArm);
                            if(std::fabs(ti.facing)<.25f)bump(N.slapHeadFast);}
                        bump(N.slapFail[tv.slapFail<SlapFailCount?tv.slapFail:0]);bump(N.shoveFail[tv.shoveFail<ShoveFailCount?tv.shoveFail:0]);
                        const auto bin=[](float x){return std::isfinite(x)&&x>0.f?std::min(unsigned(x/.25f),15u):0u;};
                        bump(N.edgeSpeed[0][bin(ti.speed)]);bump(N.edgeA[bin(ti.a)]);bump(N.edgeSwing[bin(ti.swing)]);
                        {EdgeSample e{};e.set=true;e.capKind=ti.capKind;e.a=ti.a;e.speed=ti.speed;e.swing=ti.swing;e.facing=ti.facing;e.side=ti.side;e.tangent=ti.tangent;
                         e.depthCm=hs.vhit.depth/u*100.f;e.verdict=std::uint8_t(tv.kind);e.slapFail=tv.slapFail;e.shoveFail=tv.shoveFail;
                         const TouchKind k0=tv.kind;e.window=k0==TouchNone&&tv.slapZone&&s.lev.npcSlap&&s.lev.slapWindowMs>0.f;
                         e.arm=ti.arm;e.cap=hs.vhit.cap;e.hand=int(h);e.guid=b.guid;e.ratio=ti.speed>0.f?ti.a/ti.speed:-1.f;e.gapMs=gapMs;
                         e.oldVerdict=std::uint8_t(touchVerdict(ti,s.lev.slapMps,s.lev.shoveMps,s.lev.npcSlap,s.lev.npcShove,true,false).kind);
                         {const V3 ax=horizontal(v::sub(b.centre,s.tg.moverPos),s.tg.up),hv=v::normalized(t.vel);const float d=v::dot(hv,ax);e.axis=std::isfinite(d)?d:0.f;}
                         {const V3 up=v::normalized(s.tg.up);const unsigned nc=std::min<unsigned>(b.count,unsigned(vbodyCaps));
                          for(unsigned k=0;k<nc;++k)if(b.c[k].kind==0){
                            const float hc=v::dot(v::sub(hs.vhit.point,v::mul(v::add(b.c[k].a,b.c[k].b),.5f)),up)/u*100.f;if(std::isfinite(hc))e.headCm=hc;break;}
                          if(hs.vhit.cap>=0&&unsigned(hs.vhit.cap)<nc){const auto& c=b.c[std::size_t(hs.vhit.cap)];
                            const V3 ab=v::sub(c.b,c.a);const float l2=v::dot(ab,ab);if(l2>1e-12f)e.capT=std::clamp(v::dot(v::sub(hs.vhit.point,c.a),ab)/l2,0.f,1.f);}}
                         hs.edgeSample=int(s.edgeNext);s.edgeSamples[s.edgeNext]=e;s.edgeNext=(s.edgeNext+1u)%unsigned(s.edgeSamples.size());}
                        TouchKind k=tv.kind;
                        // a pair: graded, any slap or shove edge with the other hand on the creature now or its edge within pairms (one push,
                        // never a slap, past the slap gate); a held slap of the other hand on it joins as the pair's first hand. Ungraded: a
                        // shove whose other hand shoved the creature within 150 ms.
                        bool pair=false;
                        if(graded){
                            pair=k!=TouchNone&&touchPair(otherOn,otherEdge,os.touchStartNs,now,std::uint64_t(double(s.lev.pairMs)*1e6));
                            if(pair){bump(N.pairs);if(k==TouchSlap){k=TouchShove;bump(N.slapToPair);}
                                if(s.hand[oh].slapPend.set&&s.hand[oh].slapPend.r.guid==b.guid)pairFromHeld(oh,gapMs);}}
                        else pair=k==TouchShove&&os.touchGuid==b.guid&&os.touchKind==TouchShove&&now>=os.touchClassNs&&now-os.touchClassNs<=150'000'000ull;
                        if(hs.edgeSample>=0){auto& e=s.edgeSamples[std::size_t(hs.edgeSample)];e.pair=pair;e.verdict=std::uint8_t(k);}
                        hs.pep.verdict=std::uint8_t(k);
                        if(k==TouchNone){bump(N.slow);
                            if(tv.slapZone&&s.lev.npcSlap&&s.lev.slapWindowMs>0.f){
                                hs.slapWin=SlapWindow{};hs.slapWin.open=true;hs.slapWin.startNs=now;hs.slapWin.peak=std::isfinite(ti.swing)?std::max(ti.swing,0.f):0.f;hs.slapWin.zone=true;
                                hs.slapWin.facing=std::isfinite(ti.facing)&&std::fabs(ti.facing)>=.25f;hs.slapWinGuid=b.guid;bump(N.windows);}}
                        else if(!pair&&!s.slaps.allow(b.guid,now))bump(N.touchGated);
                        else if(graded&&k==TouchSlap&&s.lev.slapHoldMs>0.f){ // held for the other hand (slapholdms)
                            if(pair)bump(N.pairGateBypass);
                            hs.slapPend.set=true;hs.slapPend.ns=now;hs.slapPend.r=build(h,b,TouchSlap,both,false,false,false,TierLight);hs.slapPend.edgeSample=hs.edgeSample;
                            if(hs.edgeSample>=0)s.edgeSamples[std::size_t(hs.edgeSample)].tier=std::uint8_t(TierLight);
                            hs.touchKind=TouchSlap;hs.touchClassNs=now;hs.touchSlapped=true;sent=true;asked[h]=true;bump(N.slapHeld);
                            bump(N.byKind[std::size_t(std::clamp(hs.vhit.kind,0,7))]);
                            bool e{},g{};s.bumps.update(b.guid,true,now,&e,&g);if(e)bump(N.touches);}
                        else{const float aUse=pas?passiveGradeA(hs.vhitApproach,hs.vhitArmIn,hs.vhitLocoIn,k==TouchShove):hs.vhitApproach;
                            if(pas&&k==TouchShove&&aUse>std::min(hs.vhitApproach,hs.vhitArmIn)+1e-3f)bump(PV.gradeLoco);
                            const PushTier tier=k==TouchShove?pushTier(false,aUse,hs.vhit.depth/u,hs.vhitArm,s.lev.pushFirmMps,s.lev.pushFirmM,s.lev.pushArmMps):TierLight;
                            if(hs.edgeSample>=0)s.edgeSamples[std::size_t(hs.edgeSample)].tier=std::uint8_t(tier);
                            if(pair&&graded)bump(N.pairGateBypass);
                            TouchRequest rq=build(h,b,k,both,graded?false:pushStrong(tier,pair,aUse),false,false,tier);
                            rq.pair=pair?1:0;rq.pairGapMs=gapMs;sendReq(h,rq);
                            if(graded&&k==TouchShove&&s.lev.pushPeak){strokePeakStart(hs.peak,b.guid,rq.a,now,rq.arm);hs.peakReq=rq;}
                            hs.touchKind=k;hs.touchClassNs=now;hs.touchSlapped=k==TouchSlap;sent=true;
                            bump(k==TouchSlap?N.slaps:N.shoves);bump(N.byKind[std::size_t(std::clamp(hs.vhit.kind,0,7))]);
                            bool e{},g{};s.bumps.update(b.guid,true,now,&e,&g);if(e)bump(N.touches);}
                    }
                }
                // a press: the palm held in the body (a slap whose contact stays and deepens becomes one; a shove holds its class 200 ms); a
                // press that asked for pressPushM within 400 ms escalates to a push (once per touch)
                // (not on the tick this hand's slap or shove was asked: its request would be overwritten)
                const bool askedNow=asked[h];
                if(open&&b.race!=1&&tOn&&s.lev.npcPush&&hs.touchGuid==b.guid&&!askedNow){
                    const float depthM=hs.vhit.depth/u;
                    const float facing=v::dot(v::normalized(hs.cmdRows[2]),v::mul(hs.vhit.n,-1.f));
                    const bool classHeld=hs.touchKind==TouchShove&&now>=hs.touchClassNs&&now-hs.touchClassNs<200'000'000ull;
                    const bool deeper=hs.touchKind!=TouchSlap||depthM>hs.touchDepth;
                    // a fast arm (a slap's swing) waits out the slap hold: its raw command runs deep into the body without a press
                    const bool quick=pas?t.valid&&t.armOk&&v::length(t.velArm)/u>=.8f:t.valid&&v::length(t.vel)/u>=.8f;
                    // a press in progress holds down to pressKeepFacing (it starts at .5)
                    const bool going=hs.touchKind==TouchPress?pressHeld(facing,depthM):pressWanted(facing,depthM,now>=hs.touchStartNs?now-hs.touchStartNs:0,both,hs.touchSlapped||quick);
                    if(!classHeld&&deeper&&going){
                        if(hs.touchKind!=TouchPress){if(hs.touchKind==TouchSlap)bump(N.reclassified);bump(N.presses);hs.touchKind=TouchPress;hs.touchClassNs=now;
                            const float es=hs.edgeSpeed;bump(N.edgeSpeed[1][std::isfinite(es)&&es>0.f?std::min(unsigned(es/.25f),15u):0u]);
                            hs.pressTicksN=0;hs.pressAskedM=0.f;hs.pressDepthMax=0.f;}
                        // PASSIVE: the press moves the creature by at most the arm's own drive into it this tick (armStepM)
                        {TouchRequest rq=build(h,b,TouchPress,both,false,false,false,TierLight);if(pas)rq.armStepM=armStepT[h];sendReq(h,rq);}
                        bump(N.pressTicks);hs.pressFacingLast=facing;hs.pressFacingMin=std::min(hs.pressFacingMin,facing);
                        ++hs.pressTicksN;hs.pressDepthMax=std::max(hs.pressDepthMax,depthM);
                        const float step0=pressStepM(depthM,s.lev.pushGain,s.lev.pushMaxM,0.f,s.lev.pushCapM);
                        const float step=pas?pressArmStepM(step0,armStepT[h]):step0;hs.pressAskedM+=step;
                        hs.pep.depthStepM+=step0;hs.pep.armStepM+=step;hs.pep.verdict=std::uint8_t(TouchPress);
                        if(pas){bump(PV.armInPress[armBin(armInT[h])]);if(step0>0.f&&!(step>0.f))bump(PV.pressStepZeroArm);else if(step>0.f)bump(PV.pressArmSent);}
                        // the hand's own speed into the body: the arm's (lever npcpassive: no locomotion, no turn), else the controller's world
                        // speed (a creature walking into a still palm gives none)
                        const float armIn=pas?armInT[h]:t.valid?v::dot(v::add(t.vel,hs.moverVel),v::normalized(v::mul(hs.vhit.n,-1.f)))/u:0.f;
                        if(s.lev.npcShove&&pressEscalate(hs.pressAsk,step,std::isfinite(armIn)?armIn:0.f,now,s.lev.pressPushM)){
                            // an escalated press is a light push: the mover offset, no injure message (it never knocks the creature over)
                            TouchRequest rq=build(h,b,TouchShove,both,false,true,false,TierLight);
                            if(s.lev.pushEscGrade){rq.edgeA=rq.a;rq.a=escalatedA(hs.pressAsk);rq.thrust=0;}
                            sendReq(h,rq);bump(N.escalations);bump(N.shoves);}
                        else if(hs.pressAsk.still){bump(N.escalateStill);if(pas)bump(PV.escalateRefusedArm);}}
                    else if(hs.touchKind==TouchPress&&!pressHeld(facing,depthM)){hs.pressFacingLast=facing;endPress(hs,depthM<.01f?0u:1u);}
                    hs.touchDepth=depthM;
                }
            }
            if(sent)continue;
            // PASSIVE: a press bumps only while an arm drives it (an arm step this tick or within pressArmHoldNs), a touch only when its edge
            // was active
            bool armPress=false,active=false;
            for(unsigned h=0;h<2;++h)if(ht[h].guid==b.guid){auto& hh=s.hand[h];
                if(ht[h].press&&armStepT[h]>0.f)hh.armPressNs=now?now:1;
                const bool held=ht[h].press&&!(armStepT[h]>0.f)&&pressArmHeld(hh.armPressNs,now);if(held&&pas)bump(PV.pressArmHeld);
                armPress=armPress||(ht[h].press&&(armStepT[h]>0.f||held));active=active||hh.touchActive;}
            const auto epBump=[&](bool sentOk){for(unsigned h=0;h<2;++h)if(ht[h].guid==b.guid){auto& ep=s.hand[h].pep;
                if(sentOk)ep.bumpsSent=std::uint16_t(std::min(ep.bumpsSent+1u,65535u));else ep.bumpsSkipped=std::uint16_t(std::min(ep.bumpsSkipped+1u,65535u));}};
            const bool pressing=(pas?armPress:press)&&s.lev.npcPress;
            if(pas&&press&&!armPress&&s.lev.npcPress){bump(PV.bumpSkippedPassive);epBump(false);}
            const auto po=s.press.update(b.guid,pressing,now);
            if(po==PressGate::Start)bump(N.pressStarts);
            if(po==PressGate::Rest)bump(N.pressRests);
            if((po==PressGate::Start||po==PressGate::Send)&&send(b.actor,b.guid)){bump(N.pressBumps);epBump(true);}
            bool edge{},gated{};const bool go=s.bumps.update(b.guid,touch,now,&edge,&gated);
            if(edge)bump(N.touches);
            if(gated&&!pressing)bump(N.gated);
            if(go&&!pressing&&s.lev.npcBump){if(pas&&!active){bump(PV.bumpSkippedPassive);epBump(false);}else if(send(b.actor,b.guid))epBump(true);}
        }
        return;
    }
    for(unsigned h=0;h<2;++h)closePassive(s,h,now);
    if(!s.lev.npcBump)return;
    Nearby nb{};if(!nearby.load(nb)||!fresh(now,nb.ns,1'000'000'000ull))return;
    if(nb.ns!=s.nearbyNs){s.nearbyNs=nb.ns;bump(N.scans);}
    for(std::uint32_t i=0;i<std::min<std::uint32_t>(nb.count,std::uint32_t(nb.c.size()));++i){
        const auto& c=nb.c[i];const bool was=s.bumps.seen(c.guid);bool touch=false;
        for(unsigned h=0;h<2;++h){const auto& hs=s.hand[h];const auto& t=s.tg.hand[h];
            if(!(hs.present&&hs.machine.state==BodyLive&&hs.haveSolved&&touchesCreature(hs.solvedPos,c.pos,s.tg.up,was,s.upm)))continue;
            // PASSIVE: only a hand the arm moves (activeMps): a palm carried into a creature by walking bumps nothing
            if(pas&&!(t.valid&&t.armOk&&v::length(t.velArm)/u>=s.lev.activeMps)){bump(PV.fallbackSkipped);continue;}
            touch=true;}
        bool edge{},gated{};const bool go=s.bumps.update(c.guid,touch,now,&edge,&gated);
        if(edge){bump(N.touches);bump(N.fallbackTouches);}
        if(gated)bump(N.gated);
        if(go)send(c.actor,c.guid);
    }
}
// A window's costs to its mode: the counted sums (a frame) or idle.
inline void commitCost(ModeStats& M,const PendingCost& p,bool counted) noexcept {
    if(!counted){bump(M.idleTicks,p.ticks);bump(M.idleNsSum,p.tickNs+p.preNs+p.updNs+p.filterNs);return;}
    bump(M.ticks,p.ticks);bump(M.tickNsSum,p.tickNs);bump(M.preCalls,p.preCalls);bump(M.preNsSum,p.preNs);bump(M.updCalls,p.updCalls);bump(M.updNsSum,p.updNs);
    bump(M.spans,p.spans);bump(M.spanNsSum,p.spanNs);bump(M.vbCalls,p.vbCalls);bump(M.vbNsSum,p.vbNs);bump(M.filterCalls,p.filterCalls);bump(M.filterNsSum,p.filterNs);
    for(unsigned k=0;k<SecCount;++k)bump(M.secNsSum[k],p.secNs[k]);
}
// Nothing of ours to look after: no body in the level (or leaving it), no hold, no door held or rested, no unfrozen prop.
inline bool quiet(const State& s) noexcept {
    for(unsigned h=0;h<2;++h)if(s.hand[h].present||s.hand[h].pendingIndex!=off::noIndex||s.grab[h].active)return false;
    for(const auto& hd:s.holds)if(hd.active)return false;
    for(const auto& r:s.rested)if(r.door)return false;
    for(const auto& e:s.unfrozen)if(e.inst)return false;
    return true;
}
} // namespace detail

// The main thread's CPU clock (one syscall); 0 where there is none (the host without a test clock).
inline std::uint64_t threadCpuNs() noexcept {
    if(const auto f=cpuClockForTests.load(std::memory_order_relaxed))return f();
#if defined(__ANDROID__)||defined(__linux__)
    timespec ts{};if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&ts)!=0)return 0;
    return std::uint64_t(ts.tv_sec)*1'000'000'000ull+std::uint64_t(ts.tv_nsec);
#else
    return 0;
#endif
}
inline void tick(const mainthread::Frame& fr) noexcept {
    const auto t0=nowNs();
    try{
        using namespace detail;
        auto& s=state();s.mainKey=threadKey();
        const std::uint64_t cpu=s.lev.cpu?threadCpuNs():0;
        const unsigned mode=ready.load(std::memory_order_acquire)?readMode():ModeOff;
        if(mode!=s.mode){
            if(s.mode==ModeOn){finishHolds(s);unfreezeTick(s,fr.now,true);resetHoldClear(s);for(unsigned h=0;h<2;++h){releaseGrab(s,h,GrabEndMode);if(s.hand[h].present)removeBody(s,h);s.hand[h].machine=Machine{};}
                heldMassRestore(s);clearSkip(s);}
            if(mode==ModeOff){unregister(s);for(auto& e:arc::extraExcluded)e.store(nullptr,std::memory_order_relaxed);}
            s.mode=mode;liveMode.store(mode,std::memory_order_relaxed);stats.mode.store(mode,std::memory_order_relaxed);
        }
        // the frame interval and the main thread's CPU time since the last tick, per mode (5..100 ms frames of an unchanged mode); the
        // costs spent in that window to its mode's counted sums or to idle
        {bool counted=false;auto& M=modeStats(s.lastTickMode<3?s.lastTickMode:mode);
         if(s.lastTickNs&&fr.now>s.lastTickNs){
            const auto wall=fr.now-s.lastTickNs;
            switch(cpuWindow(s.lastTickMode==mode,wall)){
            case CpuCount:bump(M.frames);addNs(M.frameNsSum,M.frameNsMax,wall);counted=true;
                if(cpu&&s.cpuLast&&cpu>=s.cpuLast){const auto d=cpu-s.cpuLast;bump(M.cpuN);addNs(M.cpuNsSum,M.cpuNsMax,d);bump(M.cpuHist[cpuBucket(d)]);}
                break;
            case CpuShort:bump(M.shortDrops);break;
            case CpuLong:bump(M.longDrops);break;
            case CpuModeChange:break;}}
         commitCost(M,s.pend,counted);s.pend=PendingCost{};}
        s.lastTickNs=fr.now;s.lastTickMode=mode;s.cpuLast=cpu;
        if(mode==ModeOff){
            std::uintptr_t sim{},level{};
            if(s.guardSim&&currentSim(sim,level))releaseGuard(s,sim,level);
            if(!ready.load(std::memory_order_acquire))bump(stats.noEngine);return;}
        bump(stats.ticks);
        if(!s.leversRead||fr.index>=s.leverIndex+64||fr.index<s.leverIndex){readLevers(s);s.leversRead=true;s.leverIndex=fr.index;}
        // the simulator and the level: a new one = everything we registered lives in the old one, which is not touched again
        std::uintptr_t sim{},level{};
        if(!currentSim(sim,level)){if(s.sim)forgetAll(s);bump(stats.noSim);return;}
        if(sim!=s.sim||level!=s.level){if(s.sim){forgetAll(s);bump(stats.simResets);}s.sim=sim;s.level=level;}
        // this frame's hook A targets (fresh: 100 ms; usable: staleMs, the bodies keep steering to the last ones) and the player's mover
        Targets tg{};const bool loaded=targets.load(tg)&&tg.upm>.01f;
        s.tgFresh=loaded&&fresh(fr.now,tg.ns);
        s.tgUsable=loaded&&fresh(fr.now,tg.ns,std::max<std::uint64_t>(std::uint64_t(double(s.lev.staleMs)*1e6),100'000'000ull));
        if(!s.tgFresh){bump(stats.targetsStale);if(s.tgUsable)bump(stats.targetsHeld);}
        // no player (a loading screen): nothing of ours in the level, held or posed -> nothing to do this tick
        if(!s.tgUsable&&quiet(s)){bump(stats.noPlayerTicks);nearWanted.store(false,std::memory_order_relaxed);s.moverOk=false;s.moverChecked=0;
            if(mode!=ModeOn)releaseGuard(s,sim,level);return;}
        // impact callbacks: ON needs the guard before any body goes in; MEASURE gives the word back
        if(mode==ModeOn){if(!guardImpacts(s)){refuse(s,"IMPACT FUNCTOR MISMATCH");return;}}
        else releaseGuard(s,sim,level);
        if(s.tgUsable){s.tg=tg;s.upm=tg.upm;}
        // the mover: verified (vtables, archetype flags) for a new mover and every 16 ticks; in between the same mover of fresh targets
        // (hook A read it from the player this frame) is reused after one guarded read of its vtable (held targets: the full check)
        std::uintptr_t reuseVt{};
        if(s.tgFresh&&tg.mover&&tg.mover==s.moverChecked&&s.moverCheckIdx&&fr.index>=s.moverCheckIdx&&fr.index-s.moverCheckIdx<16&&
           get(tg.mover,reuseVt)&&reuseVt==engine.base+off::moverSimVtable){
            s.moverOk=true;s.actor=tg.actor;bump(stats.moverReuse);}
        else{s.moverOk=false;s.moverChecked=0;
            if(s.tgUsable&&tg.mover){
                std::uintptr_t vt{},ivt{},arch{};std::uint32_t type{},include{};
                if(get(tg.mover,vt)&&vt==engine.base+off::moverSimVtable&&get(tg.mover+off::moverInst,ivt)&&ivt==engine.base+off::moverInstVtable&&
                   get(tg.mover+off::moverInst+off::instArch,arch)&&arch&&get(arch+off::archType,type)&&get(arch+off::archInclude,include)){
                    s.mover=tg.mover;s.moverInstAddr=tg.mover+off::moverInst;s.actor=tg.actor;s.moverOk=true;s.moverType=type;s.moverInclude=include;
                    s.moverChecked=tg.mover;s.moverCheckIdx=fr.index?fr.index:1;
                    stats.moverType.store(type,std::memory_order_relaxed);stats.moverInclude.store(include,std::memory_order_relaxed);}
                else bump(stats.moverBad);}}
        readDump(s,fr.now);
        // the player's own instances (inventory items such as holstered guns and a held knife, his frag, the mount): every 16 ticks and
        // on a hand's class change (a draw, a holster; at most every 4 ticks); the addresses are only compared, never dereferenced
        if(mode==ModeOn&&s.moverOk&&s.actor){
            const std::array<std::uint8_t,2> cls{s.tg.hand[0].cls,s.tg.hand[1].cls};
            if(!s.ownIndex||fr.index>=s.ownIndex+16||fr.index<s.ownIndex||(cls!=s.ownCls&&fr.index>=s.ownIndex+4)){
                s.ownIndex=fr.index?fr.index:1;s.ownCls=cls;s.ownCount=0;bump(stats.ownReads);
                std::uint32_t guid{};int n=0;
                const void* const* list=get(s.actor+off::actorGuid,guid)&&guid?arc::ledger.excluded(guid,n):nullptr;
                if(list&&n>0&&n<=int(s.own.size())){for(int i=0;i<n;++i)s.own[std::size_t(i)]=addr(list[i]);s.ownCount=n;}
                else bump(stats.ownFails);}
        }else{s.ownCount=0;s.ownIndex=0;}
        // the behaviours: in the list while the row is not OFF (MEASURE times the step with them)
        if(!s.built||(!s.hand[0].present&&!s.hand[1].present&&!sameShape(s)&&
                      raw<std::uint16_t>(instOf(0)+off::instIndex)==off::noIndex&&raw<std::uint16_t>(instOf(1)+off::instIndex)==off::noIndex)){
            for(unsigned h=0;h<2;++h)if(s.listed[h]){listRemove(s.sim,&objects[h].beh);s.listed[h]=false;}
            build(s);if(!ready.load(std::memory_order_acquire))return;}
        // the mod's own casts (laser, arcs, teleport) exclude the hand bodies: a cast from a hand starts inside its body
        if(mode==ModeOn)for(unsigned h=0;h<2;++h)arc::extraExcluded[h].store(objects[h].inst,std::memory_order_relaxed);
        else for(auto& e:arc::extraExcluded)e.store(nullptr,std::memory_order_relaxed);
        for(unsigned h=0;h<2;++h){
            if(s.listed[h]&&!listHas(s.sim,&objects[h].beh)){s.listed[h]=false;bump(stats.listLost);}
            if(!s.listed[h]){if(listAdd(s.sim,&objects[h].beh))s.listed[h]=true;else bump(stats.listFails);}
        }
        // collision flags from the mover (or the levers), written while both bodies are out of the level
        if(s.moverOk){const std::uint32_t type=handType(s.moverType,s.lev.type),include=handInclude(s.moverInclude,s.lev.include);
            s.flagsBad=!type||!include;
            if(s.flagsBad)bump(stats.flagsZero);
            else if(flagsChanged(s,type,include)&&!s.hand[0].present&&!s.hand[1].present)writeFlags(s,type,include);
            stats.type.store(s.archType,std::memory_order_relaxed);stats.include.store(s.archInclude,std::memory_order_relaxed);}
        // eye jump (teleport, respawn), carried (a moving platform); a turn bridge moves the bodies with the plan and is not a jump
        bool jump=false,carried=false;
        if(s.tgFresh){
            const float moved=s.haveEye?v::length(v::sub(s.tg.eye,s.lastEye))/std::max(s.upm,1e-3f):0.f;
            if(moved>originJumpM)jump=true;
            else if(s.haveEye&&fr.now>s.lastEyeNs&&moved/std::max(float(double(fr.now-s.lastEyeNs)*1e-9),.001f)>carriedSpeedMps)carried=true;
            s.lastEye=s.tg.eye;s.lastEyeNs=fr.now;s.haveEye=true;}
        if(jump)bump(stats.jumps);
        if(carried)bump(stats.carried);
        // the virtual bodies VrMelee published this tick (it runs first), each one's sphere taken again
        s.vbFresh=false;s.vb.count=0;
        if(mode==ModeOn&&s.lev.npcBody){VBodies vb{};
            if(vbodies.load(vb)&&fresh(fr.now,vb.ns)){vb.count=std::min<std::uint32_t>(vb.count,std::uint32_t(vbodyMax));
                for(std::uint32_t i=0;i<vb.count;++i)finishBody(vb.b[i]);
                s.vb=vb;s.vbFresh=true;
                if(vb.count){auto& N=stats.npc;bump(N.vbTicks);bump(N.vbBodies,vb.count);
                    for(std::uint32_t i=0;i<vb.count;++i){bump(vb.b[i].race==1?N.vbHorses:N.vbCreatures);bump(N.vbCapsules,vb.b[i].count);}
                    if(fr.now>=vb.ns){bump(N.vbAgeUsSum,(fr.now-vb.ns)/1000u);bump(N.vbAgeN);}}}}
        auto ts=nowNs();s.pend.secNs[SecUpkeep]+=ts-t0;
        const auto section=[&](Section k){const auto x=nowNs();s.pend.secNs[k]+=x-ts;ts=x;};
        for(unsigned h=0;h<2&&ready.load(std::memory_order_acquire);++h)handTick(s,h,fr.now,jump,carried);
        if(ready.load(std::memory_order_acquire)){heldMassTick(s);playerSkipTick(s,fr.now);}
        if(mode==ModeOn){Palms pm{};pm.ns=fr.now;
            for(unsigned h=0;h<2;++h){const auto& hs=s.hand[h];if(hs.present&&hs.machine.state==BodyLive&&hs.haveSolved){pm.p[h]=hs.solvedPos;pm.live|=std::uint8_t(1u<<h);}}
            // a held heavy prop (lever prophitgeo): its centre of mass and radius, so the creatures near it get bodies without a live palm
            if(s.lev.propHit&&s.lev.propHitGeo&&s.lev.npcBody)for(unsigned h=0;h<2&&!pm.heavy;++h){if(!heavyGeoHold(s,h))continue;
                if(const auto col=activeCollider(s,s.grab[h].prop)){const V3 c=vec3(col+off::colliderPos);if(!v::finite(c))continue;
                    pm.prop=c;pm.propR=(s.grab[h].rec.radiusCm>0.f?s.grab[h].rec.radiusCm*.01f:1.f)*std::max(s.upm,1e-3f);pm.heavy=1;}}
            // THROW: a prop in flight (a light one with throwLight)
            if(s.lev.propHit&&s.lev.propHitGeo&&s.lev.npcBody)for(unsigned i=0;i<2&&!pm.heavy;++i){const auto& f=s.flights[i];
                if(!f.on||!(f.heavy||s.lev.throwLight))continue;
                if(const auto col=activeColliderAt(s.level,f.levelIndex,f.prop);col&&col==f.col){const V3 c=vec3(col+off::colliderPos);
                    if(v::finite(c)){pm.prop=c;pm.propR=f.radiusW;pm.heavy=1;}}}
            palms.store(pm);}
        section(SecHands);
        if(ready.load(std::memory_order_acquire)){doorsTick(s,fr.now);section(SecDoors);reachWake(s,fr.now,fr.index);section(SecReach);
            gunGlassTick(s,fr.now);
            bloodLifeTick(s,fr.now);
            npcTick(s,fr.now);propHitTick(s,fr.now);section(SecNpc);
            resetHoldTick(s);ghostTick(s,fr.now);
            {bool any=false;for(const auto& e:s.unfrozen)any=any||e.inst;for(const auto& e:s.moved)any=any||e.inst;if(any)unfreezeTick(s,fr.now,!unfreezeOn(s));}
            section(SecUnfreeze);}
    }catch(...){bump(stats.exceptions);}
    const auto d=nowNs()-t0;stats.tickNsSum.fetch_add(d,std::memory_order_relaxed);maxOf(stats.tickNsMax,d);
    {auto& p=detail::state().pend;++p.ticks;p.tickNs+=d;}
}

// Startup (TraceEntry, after the engine base is known): every print and word, then the function pointers. read = TraceEntry readMemory,
// got(slot, target) = the GOT word check. Nothing is registered with the engine here (the tick does that on the main thread).
// A CORE mismatch installs nothing; another group's mismatch turns off its own feature (PALM -> the capsule).
// replace(slot, expected, replacement) = TraceEntry replacePointer: the DOORGATE jump-slot hooks (FindDoor, Open, Close), all three or none.
template<class Got,class Replace>
inline bool install(ReadFn read,std::uintptr_t base,Got&& got,Replace&& replace) noexcept {
    const auto hash=[&](std::uintptr_t address,std::uint32_t size){
        std::uint64_t h=0xcbf29ce484222325ull;unsigned char chunk[256];
        for(std::uint32_t done=0;done<size;){const std::uint32_t n=std::min<std::uint32_t>(size-done,sizeof chunk);
            if(!read(nullptr,address+done,chunk,n))return std::uint64_t{};
            for(std::uint32_t i=0;i<n;++i)h=(h^chunk[i])*0x100000001b3ull;
            done+=n;}
        return h;};
    groupOk.fill(false);
    if(killed(Feature::HandPhys)){installState.store("PROPERTY OFF");return false;}
    std::array<bool,GroupCount> ok{};ok.fill(true);
    for(const auto& p:prints)if(ok[p.group]&&hash(base+p.offset,p.size)!=p.fnv)ok[p.group]=false;
    if(!ok[GroupCore]){installState.store("ENGINE MISMATCH");return false;}
    if(!got(off::simGot,off::simVar)||!got(off::levelGot,off::levelVar)||!got(off::propGot,off::propVar)||!got(off::instVtableGot,off::instVtable)){
        installState.store("WORD MISMATCH");return false;}
    std::uintptr_t word{};
    if(!read(nullptr,base+off::behaviourTypeinfoWord,&word,sizeof word)||word!=base+off::behaviourTypeinfo){installState.store("WORD MISMATCH");return false;}
    if(!got(off::instDataVtableGot,off::instDataVtable)||!got(off::preApplyFnGot,off::preApplyGame)||!got(off::impactThunkGot,off::impactThunk)||
       !got(off::preComputeFnGot,off::defaultPreCompute)||!got(off::preComputeFlagGot,off::preComputeFlag)||!got(off::preApplyFlagGot,off::preApplyFlag)){
        installState.store("WORD MISMATCH");return false;}
    for(const auto& w:off::instSlots)if(!got(w.slot,w.target)){installState.store("WORD MISMATCH");return false;}
    // group words
    if(!got(off::boxVtableGot,off::boxVtable)||!got(off::compVtableGot,off::compVtable)||!got(off::clothGot,off::clothFlag)||!got(off::resCtorGot,off::resCtorFlag))ok[GroupPalm]=false;
    if(!got(off::forceCcdGot,off::forceCcd))ok[GroupCcd]=false;
    if(!got(off::physLockGot,off::physLock))ok[GroupReach]=false;
    if(!ok[GroupAct])ok[GroupReach]=false;
    if(!got(off::actorManagerGot,off::actorManager)||!got(off::actorSlotsGot,off::actorSlots))ok[GroupBump]=false;
    if(!got(off::pivotGot,off::pivotVtable)||!got(off::animGot,off::animVtable))ok[GroupSwing]=false;
    for(const auto& w:off::pivotSlots)if(!got(w.slot,w.target))ok[GroupSwing]=false;
    if(!got(off::clockGot,off::clockVar)||!got(off::doorNoticeGot,off::doorNoticeVar)||!got(off::camGot,off::camVar)||!ok[GroupSwing]||!ok[GroupDoorx])ok[GroupRest]=false;
    if(!ok[GroupAct])ok[GroupRelease]=false;
    if(!ok[GroupAct]||!got(off::propUdGot,off::propUdVtable))ok[GroupUnfreeze]=false;
    if(!got(off::moverCompGot,off::moverCompVtable)||!got(off::animCompGot,off::animCompVtable)||!got(off::moverSimGot,off::moverSimSymbol))ok[GroupTouch]=false;
    if(!ok[GroupRest]||!ok[GroupSwing]||!got(off::findDoorGot,off::fnFindDoor)||!got(off::openGot,off::fnDoorOpen)||!got(off::closeGot,off::fnDoorClose))ok[GroupDoorGate]=false;
    if(!ok[GroupAct]||!got(off::createCountGot,off::createCount)||!got(off::fragTuneGot,off::fragTune))ok[GroupGlass]=false;
    if(!ok[GroupGrab])ok[GroupDrive]=false;
    if(!ok[GroupGlass]||!got(off::ptFxMeleeGot,off::ptFxMeleeVar)||!got(off::bloodMgrGot,off::bloodMgrVar)||!got(off::ptFxMgrGot,off::ptFxMgrVar)||
       !got(off::camPosGot,off::camPosVar)||!got(off::fovScaleGot,off::fovScaleVar))ok[GroupBlood]=false;
    if(!ok[GroupTouch]||!got(off::factionGot,off::factionVar))ok[GroupSelf]=false;
    if(!ok[GroupTouch]||!ok[GroupTree]||!ok[GroupDiag]||!got(off::randUsedGot,off::randUsedVar)||!got(off::randNumGot,off::randNumVar)||!got(off::randFuncGot,off::randFuncVar))
        ok[GroupPick]=false;
    if(!ok[GroupTouch])ok[GroupTough]=false;
    if(!ok[GroupGrab])ok[GroupWeld]=false;
    if(!ok[GroupBlood]||!got(off::rmPtfxGot,off::rmPtfxVar)||!got(off::ruleStdVtable+off::ruleVtInitInstance,off::fnInitInstance))ok[GroupBloodRoute]=false;
    if(!ok[GroupPick]||!got(off::replayRandGot,off::replayRandVar))ok[GroupPickFn]=false;
    if(!ok[GroupBloodRoute])ok[GroupBloodLife]=false;
    if(!ok[GroupGlass]||!got(off::levelGot,off::levelVar))ok[GroupGunGlass]=false;
    if(!ok[GroupUnfreeze]||!got(off::gohCountGot,off::gohCountVar)||!got(off::gohTableGot,off::gohTableVar)||!got(off::startResetGot,off::fnStartReset)||
       !got(off::commonResetGot,off::fnCommonReset)||!got(off::applyResetGot,off::fnApplyReset))ok[GroupResetHold]=false;
    // DEACT: the stock phInst vtable word for word (offset-to-top 0, typeinfo, 34 slots), copied into the own table
    {std::uintptr_t stock[instTableWords]{};
     if(!read(nullptr,base+off::instVtable,stock,sizeof stock)||stock[0]!=0||stock[1]!=base+off::instTypeinfo)ok[GroupDeact]=false;
     for(std::size_t i=0;i<34&&ok[GroupDeact];++i)if(stock[2+i]!=base+off::instVtableSlots[i])ok[GroupDeact]=false;
     if(ok[GroupDeact])buildInstTable(stock,reinterpret_cast<std::uintptr_t>(&detail::instNoDeactivate),instTable);}
    Engine e{};e.base=base;e.read=read;
    const auto fn=[&](auto& slot,std::uintptr_t offset){slot=reinterpret_cast<std::remove_reference_t<decltype(slot)>>(base+offset);};
    fn(e.archCtor,0x1c83de8);fn(e.setBound,0x1c83974);fn(e.setMass,0x1c842b4);fn(e.setGravity,0x1c84458);
    fn(e.sphereCtor,0x1bce294);fn(e.capsuleCtor,0x1ba87c4);fn(e.capsuleSize,0x1ba893c);fn(e.instCtor,0x1c82b88);fn(e.instInit,0x1c82c64);
    fn(e.addActive,0x1c2e934);fn(e.deleteObject,0x1c2fd20);fn(e.teleport,0x1c2f94c);fn(e.setVelocity,0x1c65478);fn(e.setAngVelocity,0x1c654d8);
    fn(e.getDoor,0x2871ac4);fn(e.doorGet,0x286be08);fn(e.doorClosed,0x286b8cc);fn(e.doorOpen,0x286b30c);
    fn(e.instDataCtor,off::instDataCtor);fn(e.preApplyGame,off::preApplyGame);fn(e.preComputeDefault,off::defaultPreCompute);
    e.navClassFn=base+off::fnNavClass;e.deadClassFn=base+off::fnDeadClass;
    if(ok[GroupPalm]){fn(e.boxCtor,off::fnBoxCtor);fn(e.boxSize,off::fnBoxSize);fn(e.compCtor,off::fnCompCtor);fn(e.compInit,off::fnCompInit);
        fn(e.compSetBound,off::fnCompSetBound);fn(e.compExtents,off::fnCompExtents);fn(e.setAngInertia,off::fnSetAngInertia);}
    if(ok[GroupAct]){fn(e.forceActivation,off::fnForceActivation);fn(e.isActDisabled,off::fnIsActDisabled);fn(e.setDisableAct,off::fnSetDisableAct);}
    if(ok[GroupSwing])fn(e.doorFastClose,off::fnDoorFastClose);
    if(ok[GroupDoorx])fn(e.doorClose,off::fnDoorClose);
    if(ok[GroupReach])fn(e.propCull,off::fnPropCull);
    if(ok[GroupBump])fn(e.broadcastBump,off::fnBroadcastBump);
    if(ok[GroupRest]){fn(e.doorSetState,off::fnDoorSetState);fn(e.pathState,off::fnPathState);fn(e.continueShared,off::fnContinueShared);}
    if(ok[GroupGrab]){fn(e.attach,off::fnAttach);fn(e.releaseConstraint,off::fnReleaseConstraint);fn(e.fixedRotation,off::fnFixedRotation);fn(e.setInertia,off::fnSetInertia);}
    // TOUCH: VrMelee's slap / shove / press calls (its own prints cover the injure message and the strike audio)
    if(ok[GroupGlass]){fn(e.collisionDamage,off::fnCollisionDamage);e.fragClassFn=base+off::fnFragClass;}
    if(ok[GroupDrive])fn(e.applyForce,off::fnApplyForce);
    if(ok[GroupBlood]){fn(e.meleeFx,off::fnMeleeFx);fn(e.ruleByHash,off::fnRuleByHash);fn(e.bloodHit,off::fnBloodHit);}
    if(ok[GroupGunGlass])fn(e.probeLevel,off::fnSweptMulti);
    if(ok[GroupTouch]){touchApi=TouchApi{base+off::fnTranslate,base+off::fnMoverRotation,base+off::moverCompVtable+0x10,base+off::animCompVtable+0x10,
                                         base+off::moverSimVtable,base};
        touchApi.nodeOk=ok[GroupDiag];
        if(ok[GroupNode]){touchApi.findNode=base+off::fnFindNode;touchApi.setNode=base+off::fnSetNode;touchApi.creatureVptr=base+off::creatureVtable+0x10;}
        if(ok[GroupSelf]){touchApi.zombie=base+off::fnIsZombie;touchApi.faction=base+off::factionVar;}
        touchApi.kickOk=ok[GroupKick];touchApi.treeOk=ok[GroupTree]&&ok[GroupDiag];
        if(ok[GroupPick]){touchApi.pickOk=true;touchApi.randUsed=base+off::randUsedVar;touchApi.randNum=base+off::randNumVar;touchApi.randFunc=base+off::randFuncVar;}
        touchApi.toughOk=ok[GroupTough];
        if(ok[GroupPickFn]){touchApi.pickFnOk=true;pickFn.replay.store(base+off::replayRandVar,std::memory_order_release);}
        touchReady.store(true,std::memory_order_release);}
    else touchReady.store(false,std::memory_order_release);
    engine=e;detail::buildTable();
    // DOORGATE: the originals first, then the three jump slots (all or none: a partial set is put back)
    if(ok[GroupDoorGate]){
        detail::findDoorOriginal.store(reinterpret_cast<detail::FindDoorFn>(base+off::fnFindDoor),std::memory_order_release);
        detail::openOriginal.store(reinterpret_cast<detail::DoorOpenFn>(base+off::fnDoorOpen),std::memory_order_release);
        detail::closeOriginal.store(reinterpret_cast<detail::DoorCloseFn>(base+off::fnDoorClose),std::memory_order_release);
        const std::uintptr_t slots[3]{off::findDoorGot,off::openGot,off::closeGot},targets[3]{off::fnFindDoor,off::fnDoorOpen,off::fnDoorClose};
        const std::uintptr_t hooks[3]{reinterpret_cast<std::uintptr_t>(&detail::findDoorHook),reinterpret_cast<std::uintptr_t>(&detail::doorOpenHook),
            reinterpret_cast<std::uintptr_t>(&detail::doorCloseHook)};
        unsigned n=0;
        for(;n<3;++n){bool r=false;try{r=replace(base+slots[n],base+targets[n],hooks[n]);}catch(...){r=false;}if(!r)break;}
        if(n<3){while(n){--n;try{replace(base+slots[n],hooks[n],base+targets[n]);}catch(...){}}ok[GroupDoorGate]=false;}}
    // RESETHOLD: the originals first, then StartPropReset / CommonReset / Apply's jump slots (all or none). The hold itself waits for
    // holdLive (the tick: ON, lever resethold, UNFREEZE); until then every hook only counts and calls the original.
    if(ok[GroupResetHold]){
        detail::startResetOriginal.store(reinterpret_cast<detail::StartResetFn>(base+off::fnStartReset),std::memory_order_release);
        detail::commonResetOriginal.store(reinterpret_cast<detail::CommonResetFn>(base+off::fnCommonReset),std::memory_order_release);
        detail::applyResetOriginal.store(reinterpret_cast<detail::ApplyResetFn>(base+off::fnApplyReset),std::memory_order_release);
        const std::uintptr_t slots[3]{off::startResetGot,off::commonResetGot,off::applyResetGot},targets[3]{off::fnStartReset,off::fnCommonReset,off::fnApplyReset};
        const std::uintptr_t hooks[3]{reinterpret_cast<std::uintptr_t>(&detail::startResetHook),reinterpret_cast<std::uintptr_t>(&detail::commonResetHook),
            reinterpret_cast<std::uintptr_t>(&detail::applyResetHook)};
        unsigned n=0;
        for(;n<3;++n){bool r=false;try{r=replace(base+slots[n],base+targets[n],hooks[n]);}catch(...){r=false;}if(!r)break;}
        if(n<3){while(n){--n;try{replace(base+slots[n],hooks[n],base+targets[n]);}catch(...){}}ok[GroupResetHold]=false;}}
    groupOk=ok;
    installState.store("INSTALLED");ready.store(true,std::memory_order_release);
    return true;
}
template<class Got>
inline bool install(ReadFn read,std::uintptr_t base,Got&& got) noexcept {
    return install(read,base,got,[](std::uintptr_t,std::uintptr_t,std::uintptr_t){return false;});}
// Startup (GameXr gameConfigureInput): the tick. Idempotent.
inline bool registerTick() noexcept {return mainthread::registerModule(mainthread::ModuleHandPhys,"handphys",&tick,nullptr);}
inline void resetForTests() noexcept {auto& s=detail::state();s=State{};liveMode.store(ModeOff);}

// Report object vr.handphys.
//  state (install), groups (prints per group), mode, hands[].state / index; type / include in use (derived from mover.type / mover.include
//  unless a lever); main = the main thread: tickUs (the module tick, with the prop wakes and the reach query), preUpdateUs / updateUs (our
//  behaviour callbacks, per call), filter = CollideObjects calls, stepSpanUs = our last PreUpdate to our first Update of a substep (the
//  engine's step between them: ON minus MEASURE = the hands' cost); hookA.visualUs = the hand part of hook A (workers); xr / render: nothing.
//  byMode.OFF / MEASURE / ON: frames and seconds in that mode, frameMsAvg / Max = the main loop's interval at our tick (the whole frame's
//    cost: compare the modes standing still), our tick / PreUpdate / Update / CollideObjects and the NPC body clip (inside PreUpdate) per
//    counted frame from the costs of counted frames only (ticksPerFrame 1), the tick's sections (props and grab are part of hands),
//    idleTicks / idleUsAvg = ticks of uncounted windows (loading screens, stalls, mode changes), stepSpanUsAvg per substep.
//  noPlayerTicks = ticks that returned at once (no targets, nothing of ours in the level); moverReuse = ticks that reused the mover check.
//  hands[]: liveFrames, contactFrames (ticks with a contact in the last 2 substeps), contactSubsteps (push / dv), contactDynSubsteps,
//    giveRises / giveDrops (the push into an active prop rising with its give, dropped back to creep),
//  normalFlips, creepClips / tangentClips (the drive into / along a surface clipped), devCm, offsetCm / rotFrames (the visual stop, hook
//  A), partToggles, ghosts, parkGhosts, inPlaceReturns, chestReturns, respawns, inserts, removes, pairs by kind (worldPairs = static
//  world and other classes), comMismatch (collider vs instance position).
//  deactRefused = PrepareForDeactivation refusals of the own vtable; inactive = the engine deactivated a body anyway (then the level's
//  active count / cap and the last commanded speed). props = contact wakes, reachWake = the wakes ahead of contact (queries, cull calls,
//  queryUs = the culls and the checks of one query, found, woken, already active, skipped (class, door, own, not in level), outOfReach,
//  lruHits, rateLimited), wakeUs = one ForceActivation. doors = contact door checks by outcome.
//  otherClass = pairs kept by phInst::GetClassType (7 mover, 10 nav prop / door, 12 dead body); samples = distinct class / type / include met.
//  dump = engine values (mover flags, behaviour count / cap, collider pool, level active / cap, solver iterations, the ProcessOverlaps
//  switches, g_ForceCCD, the allowed penetration and separation bias, the hands' instance flags).
//  own = the player's own instances CollideObjects drops (ConfigureActorExcludeInsts through the ARC ledger), ownReads / ownFails.
//  guard = preApplyGuard in the simulator's PreApplyImpacts fn word (active, installs, restores, refusals); hands[].impactSkips = impact
//  iterators of our instance it returned for; user = the instance's sagInstData as the engine constructor writes it, per hand.
//  doors: swingHolds (Open by the palm, then held), takeovers (a door already moving), phaseWrites, finishes (back to the engine),
//    glides, fastCloses, reverses (pushed shut from the far side), dropsGame / dropsGone (the game took the door / it is gone),
//    capPending, geomRejects (the palm off the leaf: lever, height, bbox; axis = a hinge that is not upright -> plain Open),
//    animOpens, plainOpens, lockedPulses, maxDeg, held[] (the live holds).
//  props: unlocks (a type lock lifted on a kept pair), unlockSkips (override, never, infinite, fixed, door, cached, notLocked),
//    gatedLru / gatedHand, refusedSamples[] (a refused wake's activation fields); reachWake.skippedBy; samples[].states (class 0).
//  npc: touches / gated per touch, fallbackTouches (the coarse capsule without bodies), virtual (bodies from VrMelee, substeps clipped
//    per hand, deepestCm, ageMsAvg, usAvg / usMax = pushOut per call), press (starts, bumps, rests), slap (requests and why not).
// A printable text as a JSON string body (quotes and backslashes escaped).
inline std::string safeText(const char* t) {
    std::string r;for(std::size_t i=0;t&&t[i]&&i<256;++i){const char c=t[i];if(c=='"'||c=='\\')r+='\\';r+=(c>=0x20&&c<=0x7e)?c:'?';}return r;}
inline std::string report() {
    using detail::resetStats;using detail::holdLive;using detail::applyByNames;using detail::commonByNames;using detail::resetDropNames;
    const auto& s=stats;auto& st=detail::state();
    const auto U=[](const Counter& c){return (unsigned long long)u64(c);};
    const auto avgUs=[](const Counter& sum,const Counter& n){const auto k=u64(n);return k?double(u64(sum))/double(k)/1e3:0.0;};
    std::string o;o.reserve(16384);char b[2048];
    const unsigned mode=s.mode.load(std::memory_order_relaxed);
    std::snprintf(b,sizeof b,"{\"state\":\"%s\",\"mode\":\"%s\",\"installed\":%s,\"groups\":{",installState.load(),keyInfo[unsigned(Key::HandPhysics)].names[mode<3?mode:0],
        mainthread::handlers[mainthread::ModuleHandPhys].tick.load()?"true":"false");o+=b;
    for(unsigned g=0;g<GroupCount;++g){o+=g?",":"";o+="\"";o+=groupNames[g];o+="\":";o+=groupOk[g]?"true":"false";}
    std::snprintf(b,sizeof b,"},\"shape\":\"%s\",\"type\":%u,\"include\":%u,\"mover\":{\"ok\":%s,\"type\":%u,\"include\":%u},"
        "\"ticks\":%llu,\"leverReads\":%llu,\"exceptions\":%llu,\"wrongThread\":%llu,\"noEngine\":%llu,\"noSim\":%llu,\"simResets\":%llu,\"inactive\":%llu,"
        "\"inactiveEvents\":{\"levelActive\":%d,\"levelCap\":%d,\"lastCmdSpeed\":%.2f},\"deactRefused\":%llu,\"buildFails\":%llu,"
        "\"listFails\":%llu,\"listLost\":%llu,\"targetsStale\":%llu,\"targetsHeld\":%llu,\"moverBad\":%llu,\"moverReuse\":%llu,\"noPlayerTicks\":%llu,"
        "\"flagsZero\":%llu,\"jumps\":%llu,\"carried\":%llu,"
        "\"own\":%d,\"ownReads\":%llu,\"ownFails\":%llu,\"guard\":{\"active\":%s,\"installs\":%llu,\"restores\":%llu,\"refusals\":%llu},"
        "\"user\":[%s,%s],",
        st.built?(st.builtShape==ShapePalm?"PALM":st.builtShape==ShapeCapsule?"CAPSULE":"SPHERE"):"NONE",
        s.type.load(std::memory_order_relaxed),s.include.load(std::memory_order_relaxed),st.moverOk?"true":"false",
        s.moverType.load(std::memory_order_relaxed),s.moverInclude.load(std::memory_order_relaxed),
        U(s.ticks),U(s.leverReads),U(s.exceptions),U(s.wrongThread),U(s.noEngine),U(s.noSim),U(s.simResets),U(s.inactive),
        s.inactiveLevelActive.load(),s.inactiveLevelCap.load(),double(s.inactiveCmdSpeed.load()),U(s.deactRefused),U(s.buildFails),
        U(s.listFails),U(s.listLost),U(s.targetsStale),U(s.targetsHeld),U(s.moverBad),U(s.moverReuse),U(s.noPlayerTicks),U(s.flagsZero),U(s.jumps),U(s.carried),
        st.ownCount,U(s.ownReads),U(s.ownFails),st.guardSim?"true":"false",U(s.guardInstalls),U(s.guardRestores),U(s.refusals),
        st.built&&detail::userOk(0)?"true":"false",st.built&&detail::userOk(1)?"true":"false");o+=b;
    std::snprintf(b,sizeof b,"\"main\":{\"tickUsAvg\":%.2f,\"tickUsMax\":%.1f,\"preUpdateUsAvg\":%.2f,\"preUpdateUsMax\":%.1f,\"updateUsAvg\":%.2f,\"updateUsMax\":%.1f,"
        "\"filterCalls\":%llu,\"filterUsAvg\":%.2f,\"filterKept\":%llu,\"substeps\":%llu,\"stepSpanUsAvg\":%.1f,\"stepSpanUsMax\":%.1f,"
        "\"knockOff\":{\"dropped\":%llu,\"kept\":%llu}},"
        "\"hookA\":{\"calls\":%llu,\"visualUsAvg\":%.2f,\"visualUsMax\":%.1f,\"targets\":%llu,\"solvedStale\":%llu},\"xr\":{\"usAdded\":0},\"render\":{\"usAdded\":0},",
        avgUs(s.tickNsSum,s.ticks),double(u64(s.tickNsMax))/1e3,avgUs(s.preNsSum,s.preCalls),double(u64(s.preNsMax))/1e3,avgUs(s.updNsSum,s.updCalls),double(u64(s.updNsMax))/1e3,
        U(s.filterCalls),avgUs(s.filterNsSum,s.filterCalls),U(s.collideTrue),U(s.substeps),avgUs(s.spanNsSum,s.spans),double(u64(s.spanNsMax))/1e3,
        U(s.knockOffDrops),U(s.knockOffKept),
        (unsigned long long)hookCalls.load(),hookCalls.load()?double(hookNsSum.load())/double(hookCalls.load())/1e3:0.0,double(hookNsMax.load())/1e3,
        (unsigned long long)hookTargets.load(),(unsigned long long)hookSolvedStale.load());o+=b;
    o+="\"byMode\":{";
    for(unsigned m=0;m<3;++m){const auto& M=s.byMode[m];const auto fr=u64(M.frames);
        const auto perFrame=[fr](const Counter& c){return fr?double(u64(c))/double(fr)/1e3:0.0;};
        std::array<std::uint64_t,cpuBuckets> hist{};for(unsigned i=0;i<cpuBuckets;++i)hist[i]=u64(M.cpuHist[i]);const auto cn=u64(M.cpuN);
        // ourUsPerFrame = tick + PreUpdate + Update + CollideObjects (the NPC clip runs inside PreUpdate)
        std::snprintf(b,sizeof b,"%s\"%s\":{\"frames\":%llu,\"seconds\":%.1f,\"frameMsAvg\":%.3f,\"frameMsMax\":%.1f,\"shortDrops\":%llu,\"longDrops\":%llu,"
            "\"cpuFrames\":%llu,\"cpuMsAvg\":%.3f,\"cpuMsP50\":%.2f,\"cpuMsP90\":%.2f,\"cpuMsMax\":%.2f,\"ticksPerFrame\":%.2f,\"tickUsPerFrame\":%.2f,\"preUpdateUsPerFrame\":%.2f,"
            "\"updateUsPerFrame\":%.2f,\"filterUsPerFrame\":%.2f,\"filterCallsPerFrame\":%.1f,\"ourUsPerFrame\":%.2f,\"substepsPerFrame\":%.2f,\"stepSpanUsAvg\":%.1f,"
            "\"npcVirtualUsPerFrame\":%.2f,\"idleTicks\":%llu,\"idleUsAvg\":%.2f,\"sectionsUsPerFrame\":{",m?",":"",keyInfo[unsigned(Key::HandPhysics)].names[m],
            U(M.frames),double(u64(M.frameNsSum))/1e9,fr?double(u64(M.frameNsSum))/double(fr)/1e6:0.0,double(u64(M.frameNsMax))/1e6,U(M.shortDrops),U(M.longDrops),
            (unsigned long long)cn,cn?double(u64(M.cpuNsSum))/double(cn)/1e6:0.0,histPercentile(hist.data(),cpuBuckets,cpuBucketMs,.5),
            histPercentile(hist.data(),cpuBuckets,cpuBucketMs,.9),double(u64(M.cpuNsMax))/1e6,fr?double(u64(M.ticks))/double(fr):0.0,
            perFrame(M.tickNsSum),perFrame(M.preNsSum),perFrame(M.updNsSum),perFrame(M.filterNsSum),fr?double(u64(M.filterCalls))/double(fr):0.0,
            perFrame(M.tickNsSum)+perFrame(M.preNsSum)+perFrame(M.updNsSum)+perFrame(M.filterNsSum),
            fr?double(u64(M.spans))/double(fr):0.0,avgUs(M.spanNsSum,M.spans),perFrame(M.vbNsSum),U(M.idleTicks),avgUs(M.idleNsSum,M.idleTicks));o+=b;
        for(unsigned k=0;k<SecCount;++k){std::snprintf(b,sizeof b,"%s\"%s\":%.2f",k?",":"",sectionNames[k],perFrame(M.secNsSum[k]));o+=b;}
        o+="}}";}
    o+="},";
    {const auto& P=s.props;const auto& R=s.reach;const auto& D=s.doors;const auto& N=s.npc;
     std::snprintf(b,sizeof b,"\"props\":{\"candidates\":%llu,\"activated\":%llu,\"refused\":%llu,\"infinite\":%llu,\"alreadyActive\":%llu,\"gated\":%llu,"
        "\"gatedLru\":%llu,\"gatedHand\":%llu,\"skipped\":%llu,\"wakes\":%llu,\"wakeUsAvg\":%.1f,\"wakeUsMax\":%.1f,\"unlocks\":%llu,"
        "\"unlockSkips\":{\"override\":%llu,\"never\":%llu,\"infinite\":%llu,\"active\":%llu,\"fixed\":%llu,\"notInLevel\":%llu,\"door\":%llu,\"cached\":%llu,\"notLocked\":%llu},",
        U(P.candidates),U(P.activated),U(P.refused),U(P.infinite),U(P.alreadyActive),U(P.gated),U(P.gatedLru),U(P.gatedHand),U(P.skipped),U(P.wakes),
        avgUs(P.wakeNsSum,P.wakes),double(u64(P.wakeNsMax))/1e3,U(P.unlocks),U(P.skipOverride),U(P.skipNever),U(P.skipInfinite),U(P.skipActive),U(P.skipFixed),
        U(P.skipNotInLevel),U(P.skipDoor),U(P.skipCached),U(P.skipNotLocked));o+=b;
     std::snprintf(b,sizeof b,"\"release\":{\"candidates\":%llu,\"released\":%llu,\"activated\":%llu,\"refusedAfter\":%llu,\"skipRoot\":%llu,\"by\":{",
        U(P.relCandidates),U(P.released),U(P.relActivated),U(P.relRefusedAfter),U(P.skipRoot));o+=b;
     for(unsigned i=1;i<ReleaseCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i>1?",":"",releaseNames[i],U(P.relBy[i]));o+=b;}
     o+="}},\"unfreeze\":{";
     std::snprintf(b,sizeof b,"\"candidates\":%llu,\"done\":%llu,\"activated\":%llu,\"refusedAfter\":%llu,\"restored\":%llu,\"dropped\":%llu,\"full\":%llu,\"cached\":%llu,\"by\":{",
        U(P.ufCandidates),U(P.ufDone),U(P.ufActivated),U(P.ufRefusedAfter),U(P.ufRestored),U(P.ufDropped),U(P.ufFull),U(P.ufCached));o+=b;
     for(unsigned i=0;i<UnfreezeCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",unfreezeNames[i],U(P.ufBy[i]));o+=b;}
     {unsigned n=0;for(const auto& e:st.unfrozen)if(e.inst)++n;std::snprintf(b,sizeof b,"},\"trackedNow\":%u,",n);o+=b;}
     const auto samples=[&](const char* name,const std::array<UnfreezeSample,8>& list){
        o+="\"";o+=name;o+="\":[";bool first=true;
        for(const auto& x:list){if(!x.inst)continue;
            std::snprintf(b,sizeof b,"%s{\"why\":\"%s\",\"w1c0\":\"0x%x\",\"typeBits\":\"0x%x\",\"udVtRel\":\"0x%x\",\"udKind\":%d,\"gohF0\":%d,\"compD4\":%d,"
                "\"partCount\":%d,\"partTypes\":[%d,%d,%d,%d],\"massKg\":%.2f,\"radiusCm\":%.1f,\"minMove\":%.3f,\"archType\":\"0x%x\",\"stateAfter\":%d,"
                "\"colKgBefore\":%.2f,\"kgSet\":%.2f,\"compRadiusCm\":%.1f,\"cached\":%d,\"sharedArch\":%d,\"approachMps\":%.2f,\"kickMps\":%.2f,\"source\":\"%s\",\"tune\":\"%s\"}",
                first?"":",",unfreezeNames[x.why<UnfreezeCount?x.why:0],unsigned(x.w1c0),unsigned(x.typeBits),x.udVtRel,x.udKind,x.gohF0,x.compD4,x.partCount,
                x.partTypes[0],x.partTypes[1],x.partTypes[2],x.partTypes[3],double(x.massKg),double(x.radiusCm),double(x.minMove),x.archType,x.stateAfter,
                double(x.colKgBefore),double(x.kgSet),double(x.compRadiusCm),x.cached,x.sharedArch,double(x.approachMps),double(x.kickMps),ufSourceNames[x.source<UfSourceCount?x.source:0],
                x.tune[0]?x.tune:"?");o+=b;first=false;}
        o+="]";};
     samples("samples",st.ufSamples);o+=",";samples("neverMoveSamples",st.neverMoveSamples);
     o+="},\"refusedSamples\":[";
     {bool first=true;for(const auto& r:st.refusedSamples){if(!r.inst)continue;
        std::snprintf(b,sizeof b,"%s{\"flags12\":\"0x%x\",\"f80\":\"0x%x\",\"cache\":%s,\"w1c0\":\"0x%x\",\"typeBits\":\"0x%x\",\"stateAfter\":%d,"
            "\"archType\":\"0x%x\",\"archInclude\":\"0x%x\",\"reach\":%s,\"minMove\":%.3f,\"radiusCm\":%.1f}",first?"":",",unsigned(r.flags12),r.f80,r.cache?"true":"false",unsigned(r.w1c0),
            unsigned(r.typeBits),r.stateAfter,r.archType,r.archInclude,r.reach?"true":"false",double(r.minMove),double(r.radiusCm));o+=b;first=false;}}
     std::snprintf(b,sizeof b,"]},\"reachWake\":{\"queries\":%llu,\"cullCalls\":%llu,\"queryUsAvg\":%.2f,\"queryUsMax\":%.1f,\"found\":%llu,\"woken\":%llu,\"refused\":%llu,\"already\":%llu,"
        "\"skipped\":%llu,\"skippedBy\":{\"class\":%llu,\"door\":%llu,\"own\":%llu,\"notInLevel\":%llu,\"notInactive\":%llu,\"root\":%llu,\"never\":%llu},\"skipRoot\":%llu,\"infinite\":%llu,\"outOfReach\":%llu,"
        "\"rateLimited\":%llu,\"lruHits\":%llu,\"still\":%llu},",
        U(R.queries),U(R.cullCalls),avgUs(R.queryNsSum,R.queries),double(u64(R.queryNsMax))/1e3,U(R.found),U(R.woken),U(R.refused),U(R.already),
        U(R.skipped),U(R.skipClass),U(R.skipDoor),U(R.skipOwn),U(R.skipNotInLevel),U(R.skipNotInactive),U(R.skipRoot),U(R.skipNever),U(R.skipRoot),U(R.infinite),U(R.outOfReach),U(R.rateLimited),U(R.lruHits),U(R.still));o+=b;
     std::snprintf(b,sizeof b,"\"doors\":{\"checks\":%llu,\"checksByState\":[%llu,%llu,%llu,%llu,%llu,%llu],\"opens\":%llu,\"closes\":%llu,\"noHandle\":%llu,\"locked\":%llu,"
        "\"busy\":%llu,\"opened\":%llu,\"gated\":%llu,\"rearmGated\":%llu,"
        "\"far\":%llu,\"notPressing\":%llu,\"swingHolds\":%llu,\"open2Holds\":%llu,\"pose4Holds\":%llu,\"takeovers\":%llu,\"phaseWrites\":%llu,\"pose4Moves\":%llu,\"finishes\":%llu,"
        "\"glides\":%llu,\"fastCloses\":%llu,\"reverses\":%llu,\"dropsGame\":%llu,\"dropsGame4\":%llu,\"dropsGone\":%llu,\"capPending\":%llu,",
        U(D.checks),U(D.checksByState[0]),U(D.checksByState[1]),U(D.checksByState[2]),U(D.checksByState[3]),U(D.checksByState[4]),U(D.checksByState[5]),
        U(D.opens),U(D.closes),U(D.noHandle),U(D.locked),U(D.busy),U(D.opened),U(D.gated),U(D.rearmGated),U(D.far),U(D.notPressing),
        U(D.swingHolds),U(D.open2Holds),U(D.pose4Holds),U(D.takeovers),U(D.phaseWrites),U(D.pose4Moves),U(D.finishes),U(D.glides),U(D.fastCloses),U(D.reverses),
        U(D.dropsGame),U(D.dropsGame4),U(D.dropsGone),U(D.capPending));o+=b;
     std::snprintf(b,sizeof b,"\"rests\":%llu,\"shuts\":{\"open2\":%llu,\"pose4\":%llu},\"restRemaps\":%llu,\"axisMismatch\":%llu,\"geomMismatch\":%llu,"
        "\"releases\":{\"idle\":%llu,\"far\":%llu,\"old\":%llu,\"body\":%llu,\"side\":%llu},\"swingHandovers\":%llu,"
        "\"geomRejects\":{\"start\":{\"lever\":%llu,\"height\":%llu,\"bbox\":%llu},\"hold\":{\"lever\":%llu,\"height\":%llu,\"bbox\":%llu},\"axis\":%llu},"
        "\"animOpens\":%llu,\"plainOpens\":%llu,\"plainBy\":{",
        U(D.rests),U(D.shutsOpen2),U(D.shutsPose4),U(D.restRemaps),U(D.axisMismatch),U(D.geomMismatch),U(D.releasesIdle),U(D.releasesFar),U(D.releasesOld),
        U(D.releasesBody),U(D.releasesSide),U(D.swingHandovers),U(D.geomStart[0]),U(D.geomStart[1]),U(D.geomStart[2]),U(D.geomHold[0]),U(D.geomHold[1]),U(D.geomHold[2]),U(D.geomAxis),
        U(D.animOpens),U(D.plainOpens));o+=b;
     for(unsigned i=0;i<PlainCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",plainNames[i],U(D.plainBy[i]));o+=b;}
     std::snprintf(b,sizeof b,"},\"lockedPulses\":%llu,\"maxDeg\":%.1f,\"rejectSamples\":[",U(D.lockedPulses),double(u64(D.maxMdeg))/1e3);o+=b;
     {bool first=true;for(const auto& r:st.doorRejects){if(!r.nav)continue;
        std::snprintf(b,sizeof b,"%s{\"why\":\"%s\",\"state\":%u,\"local\":[%.3f,%.3f,%.3f],\"bmin\":[%.3f,%.3f,%.3f],\"bmax\":[%.3f,%.3f,%.3f],\"r\":%.3f,\"deltaDeg\":%.1f,"
            "\"gapCm\":%.1f,\"part\":%d,\"normalOk\":%s}",
            first?"":",",plainNames[r.why<PlainCount?r.why:0],r.state,double(r.local[0]),double(r.local[1]),double(r.local[2]),double(r.bmin[0]),double(r.bmin[1]),
            double(r.bmin[2]),double(r.bmax[0]),double(r.bmax[1]),double(r.bmax[2]),double(r.r),double(r.delta*180.f/pi),double(r.gapCm),r.part,r.normalOk?"true":"false");o+=b;first=false;}}
     std::snprintf(b,sizeof b,"],\"refusedGapCm\":{\"inside\":%llu,\"to1_5\":%llu,\"to3\":%llu,\"to6\":%llu,\"to10\":%llu,\"over10\":%llu,\"offLeaf\":%llu},"
        "\"notLeafNormal\":%llu,\"engineBusy\":%llu,\"sideFlips\":%llu,\"floorHolds\":%llu,\"raises\":%llu,\"raiseBlocked\":%llu,\"events\":%llu,\"eventRing\":[",
        U(D.refusedGap[0]),U(D.refusedGap[1]),U(D.refusedGap[2]),U(D.refusedGap[3]),U(D.refusedGap[4]),U(D.refusedGap[5]),U(D.refusedGap[6]),U(D.notLeafNormal),
        U(D.engineBusy),U(D.sideFlips),U(D.floorHolds),U(D.raises),U(D.raiseBlocked),U(D.events));o+=b;
     {bool first=true;for(unsigned i=0;i<st.doorEvents.size();++i){const auto& e=st.doorEvents[(st.doorEventNext+i)%st.doorEvents.size()];if(!e.set)continue;
        std::snprintf(b,sizeof b,"%s{\"end\":\"%s\",\"state\":%u,\"mode\":\"%s\",\"side\":%d,\"startDeg\":%.1f,\"endDeg\":%.1f,\"ticks\":%llu,\"moves\":%llu,"
            "\"centreGapCm\":%.1f,\"partGapCm\":%.1f,\"moverHingeM\":%.2f,\"presses\":%u,\"floorHolds\":%u,\"flips\":%u,\"maxLagCm\":%.1f}",first?"":",",
            doorEndNames[e.end<DoorEndCount?e.end:0],e.state,holdModeNames[e.mode<4?e.mode:0],e.side,
            double(e.startDeg),double(e.endDeg),(unsigned long long)e.ticks,(unsigned long long)e.moves,double(e.centreGapCm),double(e.partGapCm),double(e.moverHingeM),
            e.presses,e.floorHolds,e.flips,double(e.maxLagCm));o+=b;first=false;}}
     // DOOR PRESS
     std::snprintf(b,sizeof b,"],\"press\":{\"lever\":%d,\"pressTicks\":%llu,\"noPressTicks\":%llu,\"solvedGateWouldRefuse\":%llu,\"edgeStraddles\":%llu,"
        "\"sideFrom\":{\"approach\":%llu,\"solved\":%llu,\"normal\":%llu,\"centre\":%llu},\"bodyViaHold\":%llu,\"closedShallow\":%llu,\"startNoPress\":%llu,\"startNoPressNoted\":%llu,\"pressCands\":%llu,\"throughTouch\":%llu,\"tickRing\":[",
        st.lev.doorPress?1:0,U(D.pressTicks),U(D.noPressTicks),U(D.solvedGateWouldRefuse),U(D.edgeStraddles),U(D.sideApproach),U(D.sideSolved),U(D.sideNormal),U(D.sideCentre),
        U(D.bodyViaHold),U(D.closedShallow),U(D.startNoPress),U(D.startNoPressNoted),U(D.pressCands),U(D.throughTouch));o+=b;
     {bool first=true;const auto nowS=nowNs();for(unsigned i=0;i<st.doorTicks.size();++i){const auto& x=st.doorTicks[(st.doorTickNext+i)%st.doorTicks.size()];if(!x.set)continue;
        std::snprintf(b,sizeof b,"%s{\"ageMs\":%.0f,\"hand\":%u,\"door\":%u,\"why\":\"%s\",\"deg\":%.1f,\"targetDeg\":%.1f,\"q\":%d,\"cmdGapCm\":%.1f,"
            "\"solvedGapCm\":%.1f,\"r\":%.2f,\"y\":%.2f,\"deltaDeg\":%.1f,\"psiDeg\":%.1f,\"marginDeg\":%.1f,\"moverR\":%.2f,\"press\":%u}",first?"":",",
            nowS>=x.ns?double(nowS-x.ns)/1e6:0.0,unsigned(x.hand),unsigned(x.door),doorTickWhyNames[x.why<DtWhyCount?x.why:0],double(x.deg),double(x.targetDeg),int(x.q),
            double(x.cmdGapCm),double(x.solvedGapCm),double(x.r),double(x.y),double(x.deltaDeg),double(x.psiDeg),double(x.marginDeg),double(x.moverR),unsigned(x.press));o+=b;first=false;}}
     o+="]}";
     {unsigned n=0;for(const auto& r:st.rested)if(r.door)++n;std::snprintf(b,sizeof b,",\"restedNow\":%u,\"held\":[",n);o+=b;}
     {bool first=true;for(const auto& hd:st.holds){if(!hd.active)continue;
        std::snprintf(b,sizeof b,"%s{\"mode\":\"%s\",\"q\":%d,\"closing\":%s,\"deg\":%.1f,\"lastT\":%.3f,\"omega\":%.2f,\"glide\":%s,\"ticks\":%llu}",first?"":",",
            holdModeNames[hd.mode<4?hd.mode:0],hd.q,hd.swing.closing?"true":"false",double((hd.mode==HoldSwing?hd.swing.theta:hd.theta)*180.f/pi),double(hd.lastT),
            double(hd.swing.omega),hd.swing.glide?"true":"false",(unsigned long long)hd.ticks);o+=b;first=false;}}
     const auto vn=u64(N.vbCalls),an=u64(N.vbAgeN);
     std::snprintf(b,sizeof b,"]},\"npc\":{\"scans\":%llu,\"touches\":%llu,\"bumps\":%llu,\"gated\":%llu,\"staleGuid\":%llu,\"notAlive\":%llu,\"fallbackTouches\":%llu,"
        "\"virtual\":{\"ticks\":%llu,\"bodies\":%llu,\"creatures\":%llu,\"horses\":%llu,\"capsules\":%llu,\"contactSubsteps\":[%llu,%llu],\"deepestCm\":%.1f,"
        "\"ageMsAvg\":%.1f,\"usAvg\":%.2f,\"usMax\":%.1f},\"press\":{\"starts\":%llu,\"bumps\":%llu,\"rests\":%llu},\"bumpHeld\":%llu,\"bumpNotHeld\":%llu,"
        "\"touch\":{\"ready\":%s,\"slaps\":%llu,\"shoves\":%llu,\"presses\":%llu,\"pressTicks\":%llu,\"reclassified\":%llu,\"gated\":%llu,\"closedHand\":%llu,"
        "\"horse\":%llu,\"slow\":%llu,\"grabSkips\":%llu,\"byKind\":[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu]}",
        U(N.scans),U(N.touches),U(N.bumps),U(N.gated),U(N.staleGuid),U(N.notAlive),U(N.fallbackTouches),
        U(N.vbTicks),U(N.vbBodies),U(N.vbCreatures),U(N.vbHorses),U(N.vbCapsules),U(s.hand[0].virtualSubsteps),U(s.hand[1].virtualSubsteps),
        double(u64(N.vbDeepUm))/1e4,an?double(u64(N.vbAgeUsSum))/double(an)/1e3:0.0,vn?double(u64(N.vbNsSum))/double(vn)/1e3:0.0,double(u64(N.vbNsMax))/1e3,
        U(N.pressStarts),U(N.pressBumps),U(N.pressRests),U(N.bumpHeld),U(N.bumpNotHeld),touchReady.load()?"true":"false",U(N.slaps),U(N.shoves),U(N.presses),U(N.pressTicks),U(N.reclassified),
        U(N.touchGated),U(N.closedHand),U(N.horse),U(N.slow),U(N.grabSkips),U(N.byKind[0]),U(N.byKind[1]),U(N.byKind[2]),U(N.byKind[3]),U(N.byKind[4]),
        U(N.byKind[5]),U(N.byKind[6]),U(N.byKind[7]));o+=b;
     // PASSIVE contact (lever npcpassive): counters, histograms (armBin: 0-.1 .1-.2 .2-.3 .3-.4 .4-.6 .6-.9 .9-1.2 1.2+ m/s), the episode ring
     {const auto& PV=s.passive;
      std::snprintf(b,sizeof b,",\"passive\":{\"on\":%d,\"passiveContacts\":%llu,\"activeEdges\":%llu,\"passiveTicks\":%llu,\"armUnknown\":%llu,\"pressStepZeroArm\":%llu,"
        "\"pressArmSent\":%llu,\"escalateRefusedArm\":%llu,\"bumpSkippedPassive\":%llu,\"shoveRefusedArm\":%llu,\"fallbackSkipped\":%llu,"
        "\"pressArmHeld\":%llu,\"gradeLoco\":%llu",st.lev.npcPassive?1:0,
        U(PV.passiveContacts),U(PV.activeEdges),U(PV.passiveTicks),U(PV.armUnknown),U(PV.pressStepZeroArm),U(PV.pressArmSent),U(PV.escalateRefusedArm),
        U(PV.bumpSkippedPassive),U(PV.shoveRefusedArm),U(PV.fallbackSkipped),U(PV.pressArmHeld),U(PV.gradeLoco));o+=b;
      const auto arr8=[&](const char* name,const std::array<Counter,8>& c){o+=",\"";o+=name;o+="\":[";for(unsigned i=0;i<8;++i){o+=i?",":"";o+=std::to_string(u64(c[i]));}o+="]";};
      arr8("armInEdge",PV.armInEdge);arr8("armInPress",PV.armInPress);arr8("locoInEdge",PV.locoInEdge);arr8("turnEdge",PV.turnEdge);
      std::snprintf(b,sizeof b,",\"pushFix\":{\"facingRescued\":%llu,\"peakSent\":%llu,\"peakNone\":%llu,\"peakLost\":%llu,\"busyBumps\":%llu,\"busyBumpRefused\":%llu",
        U(PV.facingRescued),U(PV.peakSent),U(PV.peakNone),U(PV.peakLost),U(PV.busyBumps),U(PV.busyBumpRefused));o+=b;
      arr8("facingFailByCapKind",PV.facingFailKind);o+="}";
      o+=",\"ring\":[";bool first=true;const auto now0=nowNs();
      for(unsigned i=0;i<st.passiveRing.size();++i){const auto& r=st.passiveRing[(st.passiveNext+i)%st.passiveRing.size()];if(!r.set)continue;
        std::snprintf(b,sizeof b,"%s{\"ageMs\":%.0f,\"guid\":%u,\"hand\":%u,\"capKind\":%d,\"ms\":%u,\"verdict\":\"%s\",\"aMax\":%.2f,\"armInMax\":%.2f,\"armInAvg\":%.2f,"
            "\"locoInMax\":%.2f,\"turnMps\":%.2f,\"depthMaxCm\":%.1f,\"armStepCm\":%.1f,\"depthStepCm\":%.1f,\"bumpsSent\":%u,\"bumpsSkipped\":%u,\"npcMovedCm\":%.1f}",
            first?"":",",now0>=r.ns?double(now0-r.ns)/1e6:-1.0,r.guid,unsigned(r.hand),r.capKind,r.ms,touchNames[r.verdict&3u],double(r.aMax),double(r.armInMax),double(r.armInAvg),
            double(r.locoInMax),double(r.turnMps),double(r.depthMaxCm),double(r.armStepCm),double(r.depthStepCm),unsigned(r.bumpsSent),unsigned(r.bumpsSkipped),double(r.npcMovedCm));
        o+=b;first=false;}
      o+="]}},";}
     const auto& G=s.grab;
     std::snprintf(b,sizeof b,"\"grab\":{\"starts\":%llu,\"presses\":%llu,\"stalePresses\":%llu,\"readyTicks\":%llu,\"holdsNow\":%u,\"releaseCalls\":%llu,\"maxLagCm\":%.1f,"
        "\"throwMpsMax\":%.2f,\"attachUsMax\":%.1f,\"holsterPresses\":[%llu,%llu],\"refusedBy\":{",U(G.starts),U(G.presses),U(G.stalePresses),U(G.readyTicks),
        unsigned(st.grab[0].active)+unsigned(st.grab[1].active),U(G.releaseCalls),double(u64(G.maxLagUm))/1e4,double(u64(G.throwMmsMax))/1e3,double(u64(G.attachNsMax))/1e3,
        (unsigned long long)grablink::pressesTaken[0].load(),(unsigned long long)grablink::pressesTaken[1].load());o+=b;
     for(unsigned i=0;i<GrabRefuseCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",grabRefuseNames[i],U(G.refused[i]));o+=b;}
     o+="},\"releases\":{";
     for(unsigned i=0;i<GrabEndCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",grabEndNames[i],U(G.ends[i]));o+=b;}
     o+="},\"candSamples\":[";
     {bool first=true;for(const auto& g:st.grabSamples){if(!g.count)continue;
        std::snprintf(b,sizeof b,"%s{\"class\":%d,\"typeBits\":\"0x%x\",\"archType\":\"0x%x\",\"count\":%llu}",first?"":",",g.cls,unsigned(g.typeBits),g.archType,
            (unsigned long long)g.count);o+=b;first=false;}}
     std::snprintf(b,sizeof b,"],\"cacheFills\":%llu,\"holdOver\":%llu,\"holdOverFar\":%llu,\"misses\":{\"own\":%llu,\"noParts\":%llu,\"far\":%llu,\"infinite\":%llu,\"neverMove\":%llu,\"neverAct\":%llu},\"missDistCm\":[%llu,%llu,%llu,%llu,%llu,%llu],"
        "\"missSqueeze\":[%llu,%llu,%llu],\"readyAfterLatchMs\":[%llu,%llu,%llu,%llu],\"latchNoReady\":%llu,\"latchClass\":[",U(G.cacheFills),U(G.holdOver),U(G.holdOverFar),
        U(G.misses[MissOwn]),U(G.misses[MissNoParts]),U(G.misses[MissFar]),U(G.misses[MissInfinite]),U(G.misses[MissNeverMove]),U(G.misses[MissNeverAct]),U(G.missDist[0]),U(G.missDist[1]),U(G.missDist[2]),U(G.missDist[3]),U(G.missDist[4]),U(G.missDist[5]),
        U(G.missSqueeze[0]),U(G.missSqueeze[1]),U(G.missSqueeze[2]),U(G.readyAfterLatch[0]),U(G.readyAfterLatch[1]),U(G.readyAfterLatch[2]),U(G.readyAfterLatch[3]),U(G.latchNoReady));o+=b;
     for(unsigned i=0;i<16;++i){o+=i?",":"";o+=std::to_string(u64(G.latchClass[i]));}
     o+="],\"latchSamples\":[";
     {bool first=true;for(const auto& g:st.latchSamples){if(!g.count)continue;
        std::snprintf(b,sizeof b,"%s{\"class\":%d,\"archType\":\"0x%x\",\"count\":%llu}",first?"":",",g.cls,g.archType,(unsigned long long)g.count);o+=b;first=false;}}
     o+="],\"refuseSamples\":[";
     {bool first=true;for(const auto& r:st.grabRefuses){if(!r.set)continue;
        std::snprintf(b,sizeof b,"%s{\"why\":\"%s\",\"class\":%d,\"archType\":\"0x%x\",\"f80\":\"0x%x\",\"flags12\":\"0x%x\",\"typeBits\":\"0x%x\",\"stateAfter\":%d,\"release\":\"%s\"}",
            first?"":",",grabRefuseNames[r.why<GrabRefuseCount?r.why:0],r.cls,r.archType,r.f80,unsigned(r.flags12),unsigned(r.typeBits),r.stateAfter,
            r.releaseWhy<ReleaseCount?releaseNames[r.releaseWhy]:"none");o+=b;first=false;}}
     o+="],\"latch\":[";
     for(unsigned hh=0;hh<2;++hh){o+=hh?",{":"{";o+="\"by\":{";bool first=true;
        for(unsigned k=0;k<grablink::LatchCount;++k){const auto n=grablink::latchBy[hh][k].load();if(!n)continue;
            std::snprintf(b,sizeof b,"%s\"%s\":%llu",first?"":",",grablink::latchNames[k],(unsigned long long)n);o+=b;first=false;}
        std::snprintf(b,sizeof b,"},\"readyAge\":{\"never\":%llu,\"ms0_25\":%llu,\"ms25_50\":%llu,\"ms50_100\":%llu,\"expired\":%llu},\"grip\":[%llu,%llu,%llu,%llu]}",
            (unsigned long long)grablink::latchReadyAge[hh][0].load(),(unsigned long long)grablink::latchReadyAge[hh][1].load(),(unsigned long long)grablink::latchReadyAge[hh][2].load(),
            (unsigned long long)grablink::latchReadyAge[hh][3].load(),(unsigned long long)grablink::latchReadyAge[hh][4].load(),(unsigned long long)grablink::latchGrip[hh][0].load(),
            (unsigned long long)grablink::latchGrip[hh][1].load(),(unsigned long long)grablink::latchGrip[hh][2].load(),(unsigned long long)grablink::latchGrip[hh][3].load());o+=b;}
     o+="]},";}
    // ---- the door approach, glass, cups, the grab drive and holds, the open-hand edges ----
    {const auto& D=s.doors;
     std::snprintf(b,sizeof b,"\"approach\":{\"hooks\":%s,\"armedAgeMs\":%.0f,\"findDoor\":{\"calls\":%llu,\"found\":%llu,\"by\":{",groupOk[GroupDoorGate]?"true":"false",
        detail::doorArmedNs.load()&&nowNs()>=detail::doorArmedNs.load()?double(nowNs()-detail::doorArmedNs.load())/1e6:-1.0,U(D.fdCalls),U(D.fdFound));o+=b;
     for(unsigned i=0;i<FindWhyCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",findDoorNames[i],U(D.fdBy[i]));o+=b;}
     std::snprintf(b,sizeof b,"},\"state\":[%llu,%llu,%llu,%llu,%llu,%llu],\"kind\":{\"pivot\":%llu,\"anim\":%llu,\"other\":%llu}},\"open\":{\"byCaller\":{\"perform\":%llu,"
        "\"bullet\":%llu,\"vehicle\":%llu,\"script\":%llu,\"script2\":%llu,\"other\":%llu},\"byState\":[%llu,%llu,%llu,%llu,%llu,%llu],\"player\":%llu,\"onOurs\":%llu,"
        "\"handBacks\":%llu,\"handBackFails\":%llu},\"close\":{\"byCaller\":{\"perform\":%llu,\"verifyDoor\":%llu,\"addUseContext\":%llu,\"sync\":%llu,\"other\":%llu},\"onOurs\":%llu},",
        U(D.fdState[0]),U(D.fdState[1]),U(D.fdState[2]),U(D.fdState[3]),U(D.fdState[4]),U(D.fdState[5]),U(D.fdKind[0]),U(D.fdKind[1]),U(D.fdKind[2]),
        U(D.openBy[0]),U(D.openBy[1]),U(D.openBy[2]),U(D.openBy[3]),U(D.openBy[4]),U(D.openBy[5]),U(D.openState[0]),U(D.openState[1]),U(D.openState[2]),U(D.openState[3]),
        U(D.openState[4]),U(D.openState[5]),U(D.openPlayer),U(D.openOnOurs),U(D.handBacks),U(D.handBackFails),
        U(D.closeBy[0]),U(D.closeBy[1]),U(D.closeBy[2]),U(D.closeBy[3]),U(D.closeBy[4]),U(D.closeOnOurs));o+=b;
     std::snprintf(b,sizeof b,"\"body\":{\"candidates\":%llu,\"inBand\":%llu,\"opens\":%llu,\"raises\":%llu,\"closes\":%llu,\"stuckOpens\":%llu,\"stuckAtTicks\":%llu,\"flips\":%llu,\"still\":%llu,"
        "\"maxPenCm\":%.1f,\"gapCm\":{\"<0\":%llu,\"0-2\":%llu,\"2-5\":%llu,\"5-10\":%llu,\"10-15\":%llu,\"15-30\":%llu,\"30+\":%llu},\"refused\":{",
        U(D.bodyCands),U(D.bodyInBand),U(D.bodyOpens),U(D.bodyRaises),U(D.bodyCloses),U(D.stuckOpens),U(D.stuckAtTicks),U(D.bodyFlips),U(D.bodyStill),
        double(u64(D.bodyMaxPenUm))/1e4,U(D.bodyGapHist[0]),U(D.bodyGapHist[1]),U(D.bodyGapHist[2]),U(D.bodyGapHist[3]),U(D.bodyGapHist[4]),U(D.bodyGapHist[5]),
        U(D.bodyGapHist[6]));o+=b;
     for(unsigned i=0;i<6;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",bodyRefusedNames[i],U(D.bodyRefused[i]));o+=b;}
     std::snprintf(b,sizeof b,"}},\"hand\":{\"closedPoses\":%llu,\"capFallback\":%llu,\"capTiny\":%llu,\"firstDegMax\":%.1f,\"posedFails\":%llu},\"ring\":[",
        U(D.closedPoses),U(D.capFallback),U(D.capTiny),double(u64(D.closedFirstMdegMax))/1e3,U(D.posedFails));o+=b;
     {bool first=true;for(unsigned i=0;i<st.findSamples.size();++i){const auto& x=st.findSamples[(st.findNext+i)%st.findSamples.size()];if(!x.set)continue;
        std::snprintf(b,sizeof b,"%s{\"why\":\"%s\",\"state\":%u,\"mode\":%u,\"kind\":%u,\"distM\":%.2f,\"heightM\":%.2f,\"widthM\":%.2f,\"cap\":[%.2f,%.2f],\"side\":%u}",
            first?"":",",findDoorNames[x.why<FindWhyCount?x.why:0],x.state,x.mode,unsigned(x.kind),double(x.distM),double(x.heightM),double(x.widthM),double(x.cap0),double(x.cap1),
            unsigned(x.sideBits));o+=b;first=false;}}
     o+="]},";}
    {const auto& G=s.glass;
     std::uintptr_t tune{};float t20{-1.f},t2c{-1.f},t30{-1.f};
     if(detail::get(engine.base+off::fragTune,tune)&&tune){detail::get(tune+0x20,t20);detail::get(tune+0x2c,t2c);detail::get(tune+0x30,t30);}
     std::snprintf(b,sizeof b,"\"glass\":{\"on\":%s,\"contacts\":%llu,\"edges\":%llu,\"panes\":%llu,\"byClass\":{\"frag\":%llu,\"nav\":%llu},\"approachMax\":%.2f,\"depthMaxMm\":%.1f,"
        "\"approachHist\":[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu],\"requests\":%llu,\"done\":%llu,\"noShatter\":%llu,\"stockDamage\":%llu,\"scanUsMax\":%.1f,"
        "\"tune\":{\"damageScale\":%.3f,\"contactDamage\":%.3f,\"speedGate\":%.3f},\"crackedPairable\":%s,\"refused\":{",
        st.lev.glass&&groupOk[GroupGlass]?"true":"false",U(G.contacts),U(G.edges),U(G.panes),U(G.byClass[0]),U(G.byClass[1]),double(u64(G.approachMaxMms))/1e3,
        double(u64(G.depthMaxUm))/1e3,U(G.approachHist[0]),U(G.approachHist[1]),U(G.approachHist[2]),U(G.approachHist[3]),U(G.approachHist[4]),U(G.approachHist[5]),
        U(G.approachHist[6]),U(G.approachHist[7]),U(G.requests),U(G.done),U(G.noShatter),U(G.stockDamage),double(u64(G.scanNsMax))/1e3,double(t20),double(t2c),double(t30),
        (s.include.load(std::memory_order_relaxed)&0x100000u)?"true":"false");o+=b;
     for(unsigned i=1;i<GlassWhyCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i>1?",":"",glassWhyNames[i],U(G.refused[i]));o+=b;}
     o+="},\"samples\":[";
     {bool first=true;for(unsigned i=0;i<st.glassSamples.size();++i){const auto& x=st.glassSamples[(st.glassSampleNext+i)%st.glassSamples.size()];if(!x.set)continue;
        std::snprintf(b,sizeof b,"%s{\"class\":%d,\"archType\":\"0x%x\",\"comp\":%d,\"group\":%d,\"groups\":%d,\"flags53\":\"0x%x\",\"minDamage\":%.2f,\"healthBefore\":%.2f,"
            "\"healthAfter\":%.2f,\"floor\":%.2f,\"cache\":%u,\"approach\":%.2f,\"result\":\"%s\"}",first?"":",",x.cls,x.archType,x.comp,x.group,x.groups,unsigned(x.flags53),
            double(x.minDamage),double(x.healthBefore),double(x.healthAfter),double(x.floor),unsigned(x.cache),double(x.approach),
            x.result==GlassOk?"done":x.result<GlassWhyCount?glassWhyNames[x.result]:"noShatter");o+=b;first=false;}}
     std::snprintf(b,sizeof b,"],\"blood\":{\"on\":%s,\"calls\":%llu,\"pricks\":%llu,\"callUsMax\":%.1f,\"refused\":{",st.lev.blood&&groupOk[GroupBlood]?"true":"false",
        U(G.bloodCalls),U(G.bloodPricks),double(u64(G.bloodCallNsMax))/1e3);o+=b;
     for(unsigned i=1;i<BloodWhyCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i>1?",":"",bloodWhyNames[i],U(G.bloodRefused[i]));o+=b;}
     o+="},\"samples\":[";
     {bool first=true;const auto nowS=nowNs();for(unsigned i=0;i<st.bloodSamples.size();++i){const auto& x=st.bloodSamples[(st.bloodSampleNext+i)%st.bloodSamples.size()];if(!x.set)continue;
        std::snprintf(b,sizeof b,"%s{\"hand\":%u,\"ageMs\":%.0f,\"localMm\":[%.0f,%.0f,%.0f],\"n\":[%.2f,%.2f,%.2f],\"dir\":[%.2f,%.2f,%.2f],\"camM\":%.1f,\"rule\":%s,"
            "\"list\":%d,\"vt\":%s,\"lowRes\":%d,\"routed\":%s,\"made\":%d,\"cullR\":%.2f,\"cullD\":%.2f,\"place\":%u,\"eyeM\":%.2f,\"side\":%d,"
            "\"facingN\":%.2f,\"facingDir\":%.2f,\"occlOld\":%d,\"occlNew\":%d}",
            first?"":",",unsigned(x.hand),nowS>=x.ns?double(nowS-x.ns)/1e6:0.0,double(x.pos[0]*1000.f),double(x.pos[1]*1000.f),double(x.pos[2]*1000.f),double(x.n[0]),double(x.n[1]),
            double(x.n[2]),double(x.dir[0]),double(x.dir[1]),double(x.dir[2]),double(x.camM),x.rule?"true":"false",int(x.list),x.vtOk?"true":"false",int(x.lowRes),
            x.routed?"true":"false",int(x.made),std::isfinite(x.cullR)?double(x.cullR):-1.0,std::isfinite(x.cullD)?double(x.cullD):-1.0,unsigned(x.place),
            double(x.eyeM),int(x.side),double(x.facingN),double(x.facingDir),int(x.occlOld),int(x.occlNew));o+=b;first=false;}}
     {const auto& W=ptfxWatch;
      std::snprintf(b,sizeof b,"],\"route\":{\"lever\":%d,\"group\":%s,\"routed\":%llu,\"made\":%llu,\"notMade\":%llu,\"refused\":{",st.lev.bloodHires?1:0,
        groupOk[GroupBloodRoute]?"true":"false",U(G.bloodRouted),U(G.bloodMade),U(G.bloodNotMade));o+=b;
      for(unsigned i=1;i<RouteWhyCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i>1?",":"",bloodRouteWhyNames[i],U(G.bloodRouteRefused[i]));o+=b;}
      std::snprintf(b,sizeof b,"},\"watch\":{\"arms\":%llu,\"seenEye\":%llu,\"droppedEye\":%llu,\"seenOther\":%llu,\"droppedOther\":%llu}}",
        (unsigned long long)W.arms.load(std::memory_order_relaxed),(unsigned long long)W.seenEye.load(std::memory_order_relaxed),
        (unsigned long long)W.droppedEye.load(std::memory_order_relaxed),(unsigned long long)W.seenOther.load(std::memory_order_relaxed),
        (unsigned long long)W.droppedOther.load(std::memory_order_relaxed));o+=b;}
     // BLOOD ON THE KNUCKLES (VrHandBlood.h)
     {namespace HB=::rdrvr::vr::handblood;std::snprintf(b,sizeof b,",\"tint\":{\"on\":%s,\"holdS\":%.0f,\"stamps\":%llu,\"drawnFrames\":%llu,\"now\":[%.2f,%.2f]}",
        HB::enabled.load()?"true":"false",double(HB::holdS.load()),U(HB::stamps),U(HB::drawnFrames),double(HB::intensity(0)),double(HB::intensity(1)));o+=b;}
     // BLOOD PLACE / BLOOD LIFE
     std::snprintf(b,sizeof b,",\"place\":{\"lever\":%d,\"mps\":%.2f,\"eyeFallback\":%llu},\"life\":{\"group\":%s,\"ticks\":%llu,\"fails\":%llu,\"recycled\":%llu,\"drops\":[",
        st.lev.bloodPlace?1:0,double(st.lev.bloodMps),U(G.bloodEyeFallback),groupOk[GroupBloodLife]?"true":"false",U(G.bloodLifeTicks),U(G.bloodLifeFails),
        U(G.bloodLifeRecycled));o+=b;
     {bool first=true;const auto nowS=nowNs();for(unsigned i=0;i<st.bloodLife.size();++i){const auto& e=st.bloodLife[(st.bloodLifeNext+i)%st.bloodLife.size()];if(!e.set)continue;
        std::snprintf(b,sizeof b,"%s{\"hand\":%u,\"ageMs\":%.0f,\"stop\":%u,\"reads\":%u,\"aliveMs\":%.0f,\"mode\":%d,\"attachOk\":%s,\"events\":%u,\"emitters\":%u,"
            "\"maxCount\":%u,\"liveReads\":%u,\"eyeFirstM\":%.2f,\"eyeLastM\":%.2f,\"travelM\":%.2f,\"fadeMin\":%.2f,\"alpha\":%.2f}",first?"":",",unsigned(e.hand),
            nowS>=e.armNs?double(nowS-e.armNs)/1e6:0.0,unsigned(e.stop),e.reads,e.lastNs>=e.armNs&&e.lastNs?double(e.lastNs-e.armNs)/1e6:0.0,int(e.mode),
            e.attachOk?"true":"false",e.events,e.emitters,e.maxCount,e.liveReads,double(e.eyeFirstM),double(e.eyeLastM),double(e.travelM),double(e.fadeMin),
            double(e.alpha));o+=b;first=false;}}
     o+="]}";
     o+="}},";}
    // GUN GLASS
    {const auto& GG=stats.gun;const auto pn=u64(GG.probes[0])+u64(GG.probes[1])+u64(GG.probes[2]);
     std::snprintf(b,sizeof b,"\"gunGlass\":{\"lever\":%d,\"freeLever\":%d,\"group\":%s,\"mps\":%.2f,\"rCm\":%.1f,\"long\":%d,\"ticks\":%llu,\"gated\":{",
       st.lev.gunGlass,st.lev.gunGlassFree,groupOk[GroupGunGlass]?"true":"false",double(st.lev.gunGlassMps),double(st.lev.gunGlassR*100.f),st.lev.gunGlassLong?1:0,U(GG.ticks));o+=b;
     for(unsigned i=1;i<GunWhyCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i>1?",":"",gunWhyNames[i],U(GG.gated[i]));o+=b;}
     std::snprintf(b,sizeof b,"},\"probes\":{\"tip\":%llu,\"bore\":%llu,\"free\":%llu},\"probeUsMax\":%.1f,\"probeUsAvg\":%.2f,\"exceptions\":%llu,"
       "\"hits\":{\"nav\":%llu,\"frag\":%llu,\"self\":%llu,\"other\":%llu,\"maxHitsFull\":%llu},\"excl\":{\"own\":%llu,\"noOwn\":%llu,\"max\":%llu},\"glassHits\":%llu,\"wouldBreak\":%llu,\"breaks\":%llu,"
       "\"done\":%llu,\"noShatter\":%llu,\"approachMax\":%.2f,\"approachHist\":[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu],\"judged\":{",
       U(GG.probes[0]),U(GG.probes[1]),U(GG.probes[2]),double(u64(GG.probeNsMax))/1e3,pn?double(u64(GG.probeNsSum))/1e3/double(pn):0.0,U(GG.exceptions),
       U(GG.hits[0]),U(GG.hits[1]),U(GG.hits[2]),U(GG.hits[3]),U(GG.maxHitsFull),U(GG.exclOwn),U(GG.exclNoOwn),U(GG.exclMax),U(GG.glassHits),U(GG.wouldBreak),U(GG.breaks),U(GG.done),U(GG.noShatter),
       double(u64(GG.approachMaxMms))/1e3,U(GG.approachHist[0]),U(GG.approachHist[1]),U(GG.approachHist[2]),U(GG.approachHist[3]),U(GG.approachHist[4]),
       U(GG.approachHist[5]),U(GG.approachHist[6]),U(GG.approachHist[7]));o+=b;
     for(unsigned i=0;i<GlassWhyCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",glassWhyNames[i],U(GG.judged[i]));o+=b;}
     o+="},\"samples\":[";
     {bool first=true;const auto nowS=nowNs();for(unsigned i=0;i<st.gunSamples.size();++i){const auto& x=st.gunSamples[(st.gunSampleNext+i)%st.gunSamples.size()];if(!x.set)continue;
       static constexpr const char* srcNames[3]{"tip","bore","free"};
       std::snprintf(b,sizeof b,"%s{\"src\":\"%s\",\"hand\":%u,\"kind\":%d,\"class\":%d,\"comp\":%d,\"part\":%d,\"approach\":%.2f,\"tipMps\":%.2f,\"alongCm\":%.1f,"
           "\"lever\":%d,\"result\":\"%s\",\"ageMs\":%.0f}",first?"":",",srcNames[x.src<3?x.src:0],unsigned(x.hand),int(x.kind),x.cls,x.comp,x.part,double(x.approach),
           double(x.tipMps),double(x.alongCm),int(x.lever)-1,x.result<GlassWhyCount?glassWhyNames[x.result]:"noShatter",nowS>=x.ns?double(nowS-x.ns)/1e6:0.0);o+=b;first=false;}}
      o+="]},";}
    {const auto& P=s.props;
     std::snprintf(b,sizeof b,"\"cups\":{\"shallowBy\":{\"notLive\":%llu,\"age\":%llu,\"depth\":%llu},\"depthHistMm\":[%llu,%llu,%llu,%llu],\"source\":{\"press\":%llu,\"grab\":%llu,\"approach\":%llu,\"rewake\":%llu},"
        "\"massSet\":%llu,\"massReapplied\":%llu,\"kicks\":%llu,\"moved2cm\":%llu,\"tipped30\":%llu,\"stuck\":%llu,\"snapBack\":%llu,\"reactivations\":%llu,\"touchesActive\":%llu,"
        "\"tracked\":[",U(P.ufShallowBy[0]),U(P.ufShallowBy[1]),U(P.ufShallowBy[2]),U(P.ufDepthHist[0]),U(P.ufDepthHist[1]),U(P.ufDepthHist[2]),U(P.ufDepthHist[3]),
        U(P.ufSource[0]),U(P.ufSource[1]),U(P.ufSource[2]),U(P.ufSource[3]),U(P.ufMassSet),U(P.ufMassReapplied),U(P.ufKicks),U(P.ufMoved2cm),U(P.ufTipped30),U(P.ufStuck),U(P.ufSnapBack),
        U(P.ufReactivations),U(P.ufTouchesActive));o+=b;
     {bool first=true;for(const auto& e:st.unfrozen){if(!e.inst)continue;
        std::snprintf(b,sizeof b,"%s{\"kgSet\":%.2f,\"movedCmMax\":%.1f,\"tiltDegMax\":%.1f,\"active\":%s}",first?"":",",double(e.kgSet),double(e.movedM*100.f),
            double(e.tiltRad*180.f/pi),e.wasActive?"true":"false");o+=b;first=false;}}
     {const auto kn=u64(P.ufKicks);
      std::snprintf(b,sizeof b,"],\"approach\":{\"armed\":%llu,\"fired\":%llu,\"thenContact\":%llu,\"sleptFirst\":%llu,\"full\":%llu,\"kept\":%llu,"
        "\"reachCmHist\":[%llu,%llu,%llu,%llu],\"firstYield\":%llu,\"firstKick\":%llu,\"firstKickLow\":%llu},\"rewakes\":%llu,\"kickContactMpsAvg\":%.2f,"
        "\"sleeps\":%llu,\"refrozen\":%llu,\"touchesToMove\":[%llu,%llu,%llu,%llu],\"contactToActMs\":[%llu,%llu,%llu,%llu],\"actToMoveMs\":[%llu,%llu,%llu,%llu],\"traces\":[",
        U(P.ufNearArmed),U(P.ufApproachFired),U(P.ufApproachThenContact),U(P.ufApproachSlept),U(P.ufNearFull),U(P.ufNearKept),U(P.ufNearReach[0]),
        U(P.ufNearReach[1]),U(P.ufNearReach[2]),U(P.ufNearReach[3]),U(P.ufFirstYield),U(P.ufFirstKick),U(P.ufFirstKickLow),U(P.ufRewakes),kn?double(u64(P.ufKickCmsSum))/double(kn)/100.0:0.0,
        U(P.ufSleeps),U(P.ufRefrozen),U(P.ufTouchesToMove[0]),U(P.ufTouchesToMove[1]),U(P.ufTouchesToMove[2]),U(P.ufTouchesToMove[3]),
        U(P.ufContactToActMs[0]),U(P.ufContactToActMs[1]),U(P.ufContactToActMs[2]),U(P.ufContactToActMs[3]),U(P.ufActToMoveMs[0]),U(P.ufActToMoveMs[1]),
        U(P.ufActToMoveMs[2]),U(P.ufActToMoveMs[3]));o+=b;}
     {bool first=true;for(const auto& c:st.cups){if(!c.inst)continue;
        const auto msBetween=[](std::uint64_t a,std::uint64_t z){return a&&z&&z>=a?double(z-a)/1e6:-1.0;};
        std::snprintf(b,sizeof b,"%s{\"archType\":\"0x%x\",\"contacts\":%u,\"touchesToMove\":%u,\"moved\":%s,\"msContactToActive\":%.0f,\"msActiveToMove\":%.0f,"
            "\"by\":[%u,%u,%u,%u],\"kicks\":%u,\"contactMps\":%.2f,\"kickMps\":%.2f,\"shallow\":%u,\"gatedLru\":%u,\"gatedHand\":%u,\"sleeps\":%u,\"sleepMs\":%.0f,"
            "\"sleepMovedCm\":%.1f,\"touchedAsleep\":%u,\"refrozen\":%u,\"refrozenMs\":%.0f,\"refrozenState\":%d,\"refrozenHome\":%s,\"resetBy\":\"%s\",\"resetDeltaCm\":%.1f,"
            "\"resetMsSinceClear\":%.0f}",first?"":",",c.archType,
            unsigned(c.contacts),unsigned(c.touchesToMove),c.moved?"true":"false",msBetween(c.firstContactNs,c.firstActiveNs),msBetween(c.firstActiveNs,c.firstMoveNs),
            unsigned(c.by[0]),unsigned(c.by[1]),unsigned(c.by[2]),unsigned(c.by[3]),unsigned(c.kicks),double(c.contactMps),double(c.kickMps),unsigned(c.shallow),
            unsigned(c.gatedLru),unsigned(c.gatedHand),unsigned(c.sleeps),double(c.sleepMs),double(c.sleepMovedCm),unsigned(c.touchedAsleep),unsigned(c.refrozen),
            double(c.refrozenMs),c.refrozenState,c.refrozenHome?"true":"false",c.resetBy>=0&&c.resetBy<3?commonByNames[unsigned(c.resetBy)]:"none",double(c.resetDeltaCm),
            double(c.resetMsSinceClear));o+=b;first=false;}}
     o+="],\"timelines\":[";
     {bool first=true;for(const auto& l:st.cupLines){if(!l.inst)continue;
        o+=first?"{":",{";first=false;o+="\"source\":\"";o+=ufSourceNames[l.src<UfSourceCount?l.src:0];o+="\",\"ticks\":[";
        for(unsigned i=0;i<l.n&&i<l.t.size();++i){const auto& k=l.t[i];
            std::snprintf(b,sizeof b,"%s[%d,%d,%d,%u,%u,%u,%u,%u,%u,%d]",i?",":"",int(k.state),int(k.movedMm),int(k.speedMms),unsigned(k.touched),unsigned(k.give),unsigned(k.age),unsigned(k.creep),
                unsigned(k.squeeze),unsigned(k.held),int(k.handCms));o+=b;}
        o+="]}";}}
     {const auto& Gh=stats.ghost;
      std::snprintf(b,sizeof b,"],\"ghost\":{\"starts\":[%llu,%llu],\"drops\":%llu,\"ends\":{\"quiet\":%llu,\"hard\":%llu,\"lever\":%llu,\"level\":%llu},"
        "\"grabbedWhileGhost\":%llu,\"msMax\":%llu,\"kicksSkipped\":%llu,\"yieldsSkipped\":%llu,\"restingPairs\":%llu},\"lightHolds\":[",
        U(Gh.starts[GhostIntent]),U(Gh.starts[GhostRelease]),U(Gh.drops),U(Gh.ends[GhostQuiet]),U(Gh.ends[GhostHard]),U(Gh.ends[GhostLever]),U(Gh.ends[GhostLevel]),
        U(Gh.grabbedWhileGhost),U(Gh.msMax),U(Gh.kicksSkipped),U(Gh.yieldsSkipped),U(Gh.restingPairs));o+=b;
      bool first=true;for(unsigned k=0;k<st.lightHolds.size();++k){const auto& r=st.lightHolds[(st.lightHoldNext+k)%st.lightHolds.size()];if(!r.set)continue;
        std::snprintf(b,sizeof b,"%s{\"heldMs\":%llu,\"end\":%u,\"kg\":%.2f,\"propSpeedMax\":%.2f,\"propMpsAtEnd\":%.2f,\"handMpsAtEnd\":%.2f,\"afterMaxMps\":%.2f,"
            "\"ghost\":%u,\"hand\":%u,\"offsetCm\":%.1f,\"maxOffCm\":%.1f,\"endOffCm\":%.1f,\"seatD0Cm\":%.1f,\"seatMs\":%.0f,\"gripPoint\":%d,"
            "\"weld\":%u,\"flagsPos\":%d,\"flagsRot\":%d,\"slipCm\":%.1f,\"offTrace\":[%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f],\"capsule\":[%u,%.1f]}",first?"":",",
            (unsigned long long)r.heldMs,unsigned(r.end),double(r.kg),double(r.propSpeedMax),double(r.propMpsAtEnd),
            double(r.handMpsAtEnd),double(r.afterMaxMps),unsigned(r.ghost),unsigned(r.hand),double(r.offsetCm),double(r.maxOffCm),double(r.endOffCm),double(r.seatD0Cm),
            double(r.seatMs),int(r.gripPoint),unsigned(r.weld),r.flagsPos==0xffff?-1:int(r.flagsPos),r.flagsRot==0xffff?-1:int(r.flagsRot),double(r.slipCm),
            double(r.offTrace[0]),double(r.offTrace[1]),double(r.offTrace[2]),double(r.offTrace[3]),double(r.offTrace[4]),double(r.offTrace[5]),
            double(r.offTrace[6]),double(r.offTrace[7]),unsigned(r.capContacts),double(r.capDepthMm));o+=b;first=false;}}
     {const auto& K=stats.seat;
      std::snprintf(b,sizeof b,"],\"weld\":{\"lever\":%d,\"group\":%s,\"welds\":%llu,\"rotRefused\":%llu,\"rotOnly\":%llu,\"rotReleaseCalls\":%llu,\"rotDetached\":%llu,"
        "\"lightFf\":%d}",st.lev.grabWeld?1:0,groupOk[GroupWeld]?"true":"false",U(stats.grab.welds),U(stats.grab.rotRefused),U(stats.grab.rotOnly),
        U(stats.grab.rotReleaseCalls),U(stats.grab.rotDetached),st.lev.grabLightFf?1:0);o+=b;
      std::snprintf(b,sizeof b,",\"seat\":{\"on\":%d,\"starts\":%llu,\"seated\":%llu,\"timeouts\":%llu,\"cancels\":{\"open\":%llu,\"gone\":%llu,\"inactive\":%llu,\"mode\":%llu},"
        "\"nearSkips\":%llu,\"refused\":%llu,\"msHist\":[%llu,%llu,%llu,%llu,%llu],\"d0CmHist\":[%llu,%llu,%llu,%llu,%llu],\"arriveMpsMax\":%.3f,\"velWrites\":%llu,"
        "\"ghostStarts\":%llu,\"thrown\":%llu,\"dipsKept\":%llu",st.lev.grabSeat?1:0,U(K.starts),U(K.seated),U(K.timeouts),U(K.cancels[0]),U(K.cancels[1]),U(K.cancels[2]),U(K.cancels[3]),U(K.nearSkips),
        U(K.refused),U(K.msHist[0]),U(K.msHist[1]),U(K.msHist[2]),U(K.msHist[3]),U(K.msHist[4]),U(K.d0CmHist[0]),U(K.d0CmHist[1]),U(K.d0CmHist[2]),U(K.d0CmHist[3]),
        U(K.d0CmHist[4]),double(u64(K.arriveMmsMax))/1e3,U(K.velWrites),U(stats.ghost.starts[GhostSeat]),U(K.thrown),U(K.dipsKept));o+=b;}
     o+="}";
     o+=",\"activatedSamples\":[";
     {bool first=true;for(const auto& a:st.activatedSamples){if(!a.inst)continue;
        std::snprintf(b,sizeof b,"%s{\"archType\":\"0x%x\",\"radiusCm\":%.1f,\"archKg\":%.2f,\"colKg\":%.2f}",first?"":",",a.archType,double(a.radiusCm),double(a.archKg),double(a.colKg));
        o+=b;first=false;}}
     // RESETHOLD: readyPoll = the StartPropReset hook (held / passed: a prop not held, passedBroken: held but home, other: another caller or the
     // hold off), applyBy / commonResetBy = the counting hooks (all and of held props), deltaCm = a held prop's distance from home at its
     // CommonReset, held = the tick's list, refrozenBy = a tracked prop's flag back with a CommonReset within 100 ms or without one
     {const auto& R=resetStats;unsigned held=0;for(const auto& e:st.resetHold)held+=e.goh?1u:0u;
      const auto hf=u64(R.heldFirstNs),hl=u64(R.heldLastNs);const double secs=hf&&hl>hf?double(hl-hf)*1e-9:0.0;
      std::snprintf(b,sizeof b,"],\"resets\":{\"on\":%s,\"readyPoll\":{\"calls\":%llu,\"held\":%llu,\"passed\":%llu,\"passedBroken\":%llu,\"other\":%llu,\"heldPerSec\":%.1f},"
        "\"applyBy\":{",holdLive.load(std::memory_order_relaxed)?"true":"false",U(R.calls),U(R.held),U(R.passed),U(R.passedBroken),U(R.passedOther),
        secs>0.0?double(u64(R.held))/secs:0.0);o+=b;
      for(unsigned i=0;i<5;++i){std::snprintf(b,sizeof b,"%s\"%s\":[%llu,%llu]",i?",":"",applyByNames[i],U(R.applyBy[i]),U(R.applyByOurs[i]));o+=b;}
      o+="},\"commonResetBy\":{";
      for(unsigned i=0;i<3;++i){std::snprintf(b,sizeof b,"%s\"%s\":[%llu,%llu]",i?",":"",commonByNames[i],U(R.commonBy[i]),U(R.commonByOurs[i]));o+=b;}
      std::snprintf(b,sizeof b,"},\"deltaCm\":[%llu,%llu,%llu,%llu],\"held\":{\"size\":%u,\"maxSize\":%llu,\"adds\":%llu,\"dropBy\":{",U(R.deltaCm[0]),U(R.deltaCm[1]),
        U(R.deltaCm[2]),U(R.deltaCm[3]),held,U(R.maxSize),U(R.adds));o+=b;
      for(unsigned i=0;i<6;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",resetDropNames[i],U(R.dropBy[i]));o+=b;}
      std::snprintf(b,sizeof b,"}},\"refrozenBy\":{\"commonReset\":%llu,\"other\":%llu}}",U(R.refrozenBy[0]),U(R.refrozenBy[1]));o+=b;}
     o+="},";}
    {const auto& G=s.grab;
     std::snprintf(b,sizeof b,"\"grabDrive\":{\"holdHist\":[%llu,%llu,%llu,%llu,%llu],\"gripDips\":{\"under50\":%llu,\"under30\":%llu},\"drive\":{\"applied\":%llu,\"capped\":%llu,"
        "\"stale\":%llu,\"shared\":%llu,\"articulated\":%llu,\"articulatedRefused\":%llu,\"off\":%llu,\"noCollider\":%llu",
        U(G.holdHist[0]),U(G.holdHist[1]),U(G.holdHist[2]),U(G.holdHist[3]),U(G.holdHist[4]),U(G.gripDips50),U(G.gripDips30),U(G.driveApplied),U(G.driveCapped),
        U(G.driveStale),U(G.driveShared),U(G.driveArticulated),U(G.driveArtRefused),U(G.driveOff),U(G.driveNoCol));o+=b;
     // the accumulator at the drive (foreign forces, runaways, the player's mover on the prop and its force zeroed, the first-order
     // fallback); the stale ring oldest first: [ms, hand, substep, acc x, y, z N, own N, |acc - own| N, kind (0 own, 1 foreign, 2 runaway,
     // 3 ground zeroed), ground index, ground generation, prop index, ground is the prop, stale run, |torque| N m, mover vertical cm/s]
     std::snprintf(b,sizeof b,",\"foreign\":%llu,\"runaway\":%llu,\"groundHeld\":%llu,\"groundZeroed\":%llu,\"fallback\":%llu,\"staleCleared\":%llu,"
        "\"runawayCleared\":%llu,\"groundLiftCapped\":%llu,\"staleRing\":[",U(G.driveForeign),
        U(G.driveRunaway),U(G.groundHeld),U(G.groundZeroed),U(G.fallbackSubsteps),U(G.staleCleared),U(G.runawayCleared),U(G.groundLiftCapped));o+=b;
     {const unsigned n=unsigned(std::min<std::uint64_t>(st.staleN,st.staleRing.size()));
      for(unsigned i=0;i<n;++i){const auto& q=st.staleRing[(st.staleNext+st.staleRing.size()-n+i)%st.staleRing.size()];
        std::snprintf(b,sizeof b,"%s[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]",i?",":"",q[0],q[1],q[2],q[3],q[4],q[5],q[6],q[7],q[8],q[9],q[10],q[11],q[12],q[13],
            q[14],q[15]);o+=b;}}
     o+="]},\"move\":{\"by\":{";
     for(unsigned i=0;i<GrabMoveCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",grabMoveNames[i],U(G.moveBy[i]));o+=b;}
     std::snprintf(b,sizeof b,"},\"released\":%llu,\"activated\":%llu,\"refusedAfter\":%llu,\"rolledBack\":%llu,\"restored\":%llu,\"dropped\":%llu,\"full\":%llu},\"holds\":[",
        U(G.moveReleased),U(G.moveActivated),U(G.moveRefusedAfter),U(G.moveRolledBack),U(G.moveRestored),U(G.moveDropped),U(G.moveFull));o+=b;
     {bool first=true;for(unsigned i=0;i<st.holdRecords.size();++i){const auto& r=st.holdRecords[(st.holdNext+i)%st.holdRecords.size()];if(!r.set)continue;
        std::snprintf(b,sizeof b,"%s{\"class\":%d,\"archType\":\"0x%x\",\"typeBits\":\"0x%x\",\"minMove\":%.2f,\"tune\":\"%s\",\"massKg\":%.2f,\"radiusCm\":%.1f,\"colType\":%d,"
            "\"parts\":%d,\"fixedRot\":%s,\"move\":%s,\"heldMs\":%llu,\"end\":\"%s\",\"gripEnd\":%.2f,\"gripMin\":%.2f,\"triggerMax\":%.2f,\"lagMaxCm\":%.1f,"
            "\"lagHist\":[%u,%u,%u,%u,%u],\"pivotMovedCm\":%.1f,\"propSpeedMax\":%.2f,\"moverSpeedAvg\":%.2f,\"moverSpeedMax\":%.2f,\"forceAvgN\":%.0f,\"forceMaxN\":%.0f,"
            "\"upMaxN\":%.0f,\"capped\":%u,\"stale\":%u",first?"":",",r.cls,r.archType,unsigned(r.typeBits),double(r.minMove),r.tune[0]?r.tune:"?",double(r.massKg),
            double(r.radiusCm),r.colType,r.parts,r.fixedRot?"true":"false",r.move?"true":"false",(unsigned long long)r.heldMs,grabEndNames[r.end<GrabEndCount?r.end:0],
            double(r.gripEnd),double(r.gripMin),double(r.triggerMax),double(r.lagMaxCm),r.lagHist[0],r.lagHist[1],r.lagHist[2],r.lagHist[3],r.lagHist[4],
            double(r.pivotMovedCm),double(r.propSpeedMax),double(r.moverSpeedAvg),double(r.moverSpeedMax),double(r.forceAvgN),double(r.forceMaxN),double(r.upMaxN),
            r.capped,r.stale);o+=b;first=false;
        std::snprintf(b,sizeof b,",\"ball\":%s,\"sepMmHist\":[%u,%u,%u,%u,%u],\"vert\":{\"up\":%u,\"down\":%u,\"linear\":%u},\"flips\":%u,\"slewed\":%u,\"follow\":%u,"
            "\"gripAlt\":%u,\"swingCms\":%.1f,\"stallMin\":%.2f,\"pairSubsteps\":%u,\"partnerEnd\":\"%s\",\"over60Ms\":%u,\"driveKg\":%.1f",r.ball?"true":"false",
            r.sepHist[0],r.sepHist[1],r.sepHist[2],
            r.sepHist[3],r.sepHist[4],r.vertUp,r.vertDown,r.vertLin,r.flips,r.slewed,r.follow,r.gripAlt,double(r.swingCms),double(r.stallMin),r.pairSubsteps,
            r.partnerEnd<GrabEndCount?grabEndNames[r.partnerEnd]:"none",r.over60Ms,double(r.driveKg));o+=b;
        std::snprintf(b,sizeof b,",\"foreign\":%u,\"groundHeld\":%u,\"groundZeroed\":%u,\"driveOffMs\":%.0f,\"fallback\":%u,\"carry\":{\"substeps\":%u,\"comErrMaxCm\":%.1f,"
            "\"angErrMaxDeg\":%.1f,\"rollRateMax\":%.2f,\"tauMaxNm\":%.1f,\"inertia\":[%.3f,%.3f,%.3f],\"kg\":%.1f},\"groundLift\":%u}",r.foreign,r.groundHeld,
            r.groundZeroed,double(r.driveOffMs),r.fallback,
            r.carrySubsteps,double(r.comErrMaxCm),double(r.angErrMaxDeg),double(r.rollRateMax),double(r.tauMaxNm),double(r.inertia[0]),double(r.inertia[1]),double(r.inertia[2]),
            double(r.carryKg),r.groundLift);o+=b;}}
     o+="],\"hook\":[";
     for(unsigned h=0;h<2;++h){const auto& k=grabHook[h];const auto L=[](const std::atomic<std::uint64_t>& a){return (unsigned long long)a.load(std::memory_order_relaxed);};
        std::snprintf(b,sizeof b,"%s{\"frames\":%llu,\"still\":%llu,\"gripDrawn\":%llu,\"heavyDrawn\":%llu,\"offGrip\":%llu,\"toggles\":%llu,\"jumpCm\":[%llu,%llu,%llu,%llu,%llu],\"alt1\":%llu,\"alt2\":%llu,"
            "\"altStill\":%llu,\"subs\":[%llu,%llu,%llu,%llu,%llu],\"altFrom\":{\"raw\":%llu,\"grip\":%llu,\"joint\":%llu,\"mover\":%llu,\"rot\":%llu},\"ring\":[",h?",":"",
            L(k.frames),L(k.still),L(k.gripDrawn),L(k.heavyDrawn),L(k.offGrip),L(k.toggles),L(k.jumps[0]),L(k.jumps[1]),L(k.jumps[2]),L(k.jumps[3]),L(k.jumps[4]),L(k.alt1),L(k.alt2),L(k.altStill),
            L(k.subs[0]),L(k.subs[1]),L(k.subs[2]),L(k.subs[3]),L(k.subs[4]),L(k.altFrom[0]),L(k.altFrom[1]),L(k.altFrom[2]),L(k.altFrom[3]),L(k.altFrom[4]));o+=b;
        // the ring oldest first: [offset x, y, z, lag, joint, grip step, controller step, mover step (mm), substeps, rotation (deg / 5)]
        const unsigned nx=k.ringNext.load(std::memory_order_relaxed);const unsigned n=std::min(nx,48u);bool f2=true;
        const auto sx=[](std::uint64_t w,int sh,int bits){const std::int64_t x=std::int64_t((w>>sh)&((std::uint64_t(1)<<bits)-1));return int(x>=(std::int64_t(1)<<(bits-1))?x-(std::int64_t(1)<<bits):x);};
        for(unsigned i=0;i<n;++i){const unsigned j=(nx-n+i)%48u;const auto w0=k.ring[j*2].load(std::memory_order_relaxed),w1=k.ring[j*2+1].load(std::memory_order_relaxed);
            std::snprintf(b,sizeof b,"%s[%d,%d,%d,%d,%d,%d,%d,%d,%u,%u]",f2?"":",",sx(w0,0,16),sx(w0,16,16),sx(w0,32,16),sx(w0,48,16),sx(w1,0,16),sx(w1,16,16),sx(w1,32,12),
                sx(w1,44,12),unsigned((w1>>56)&15u),unsigned((w1>>60)&15u));o+=b;f2=false;}
        o+="]}";}
     o+="]},";}
    // TWO HANDS (grabPair) and PROPHIT (propHit): see GrabStats / PropHitStats
    {const auto& G=s.grab;const auto& P=s.propHit;
     const auto arr=[&](const char* name,const Counter* c,unsigned n){o+=",\"";o+=name;o+="\":[";for(unsigned i=0;i<n;++i){o+=i?",":"";o+=std::to_string(u64(c[i]));}o+="]";};
     std::snprintf(b,sizeof b,"\"grabPair\":{\"joins\":%llu,\"ends\":%llu,\"substeps\":%llu,\"refusedLight\":%llu,\"upMaxN\":%llu,\"weightN\":%llu,\"upCapped\":%llu,"
        "\"sideCapped\":%llu,\"speedMaxCms\":%llu,\"rollFlips\":%llu",U(G.pairJoins),U(G.pairEnds),U(G.pairSubsteps),U(G.refused[GrabPair]),U(G.pairUpMaxN),U(G.pairWeightN),
        U(G.pairUpCapped),U(G.pairSideCapped),U(G.pairSpeedMaxCms),U(G.pairRollFlips));o+=b;
     arr("dCmHist",G.pairDHist.data(),5);arr("stretchCmHist",G.pairStretchHist.data(),5);arr("shareHist",G.pairShareHist.data(),5);arr("rollRadSHist",G.pairRollHist.data(),5);
     // the join: counters, distances, the attempts, the pair episodes, the heavy holds, the releases of a hand that left LIVE, the latch ring
     std::snprintf(b,sizeof b,",\"join\":{\"readyTicks\":%llu,\"tries\":%llu,\"starts\":%llu,\"refused\":%llu,\"holdKind\":[%u,%u]",U(G.joinReadyTicks),U(G.joinTries),
        U(G.joinStarts),U(G.joinRefused),unsigned(grablink::heldKind(0)),unsigned(grablink::heldKind(1)));o+=b;
     arr("distCmHist",G.joinDistHist.data(),6);o+=",\"ring\":[";
     {bool first=true;const auto now0=nowNs();for(unsigned i=0;i<st.joinRing.size();++i){const auto& r=st.joinRing[(st.joinNext+i)%st.joinRing.size()];if(!r.set)continue;
        std::snprintf(b,sizeof b,"%s{\"ageMs\":%.0f,\"hand\":%u,\"class\":%u,\"distMm\":%d,\"part\":%d,\"result\":\"%s\"}",first?"":",",now0>=r.ns?double(now0-r.ns)/1e6:-1.0,
            unsigned(r.hand),unsigned(r.cls),int(r.distMm),int(r.part),r.result==0xfe?"joined":r.result<GrabRefuseCount?grabRefuseNames[r.result]:"?");o+=b;first=false;}}
     o+="],\"pairs\":[";
     {bool first=true;for(unsigned i=0;i<st.pairRing.size();++i){const auto& r=st.pairRing[(st.pairNext+i)%st.pairRing.size()];if(!r.set)continue;
        std::snprintf(b,sizeof b,"%s{\"ms\":%llu,\"substeps\":%u,\"shareAvg\":%.2f,\"riseMaxCm\":%.1f,\"dropMaxCm\":%.1f,\"speedMaxCms\":%.0f,\"upMaxN\":%.0f,\"weightN\":%.0f,"
            "\"statics\":%u,\"upAvgN\":%.0f,\"dCm\":%.1f,\"vertCm\":%.1f,\"horizCm\":%.1f,\"shareHAvg\":%.2f,\"coupleAvgN\":%.0f,\"coupleMaxN\":%.0f,\"sideCap\":%u,\"downCap\":%u,"
            "\"turnMaxDeg\":%.0f,\"playerSubsteps\":%u,\"playerDepthMm\":%.0f,\"moverCm\":%.1f,\"guardTrims\":%u,\"lag20Ms\":%.0f,\"lag60Ms\":%.0f,\"start\":%d}",
            first?"":",",(unsigned long long)r.ms,r.substeps,double(r.shareAvg),double(r.riseMaxCm),double(r.dropMaxCm),double(r.speedMaxCms),double(r.upMaxN),double(r.weightN),
            unsigned(r.statics),double(r.upAvgN),double(r.dCm),double(r.vertCm),double(r.horizCm),double(r.shareHAvg),double(r.coupleAvgN),double(r.coupleMaxN),r.sideCap,r.downCap,
            double(r.turnMaxDeg),r.playerSubsteps,double(r.playerDepthMm),double(r.moverCm),r.guardTrims,double(r.lag20Ms),double(r.lag60Ms),
            r.start<st.pairStarts.size()?int(r.start):-1);o+=b;first=false;}}
     // the starts of the pair episodes: per substep [lagA cm, lagB cm, rise mm, turn deg, upA N/10, upB N/10, couple N/10, bits]
     o+="],\"pairStarts\":[";
     {bool first=true;for(unsigned i=0;i<st.pairStarts.size();++i){const auto& ps=st.pairStarts[i];if(!ps.ns)continue;
        std::snprintf(b,sizeof b,"%s{\"slot\":%u,\"n\":%u,\"s\":[",first?"":",",i,ps.n);o+=b;first=false;
        for(unsigned k=0;k<ps.n&&k<ps.s.size();++k){const auto& q=ps.s[k];
            std::snprintf(b,sizeof b,"%s[%d,%d,%d,%d,%d,%d,%d,%d]",k?",":"",q[0],q[1],q[2],q[3],q[4],q[5],q[6],q[7]);o+=b;}
        o+="]}";}}
     {const auto& PS=s.pairStatics;
      std::snprintf(b,sizeof b,"],\"statics\":{\"substeps\":%llu,\"clamped\":%llu,\"shortLine\":%llu,\"guardTrims\":%llu,\"guardTrimAvgN\":%.0f,\"playerContacts\":%llu,"
        "\"playerSteep\":%llu,\"coupleNHist\":[%llu,%llu,%llu,%llu,%llu]",U(PS.statics),U(PS.clamped),U(PS.shortLine),U(PS.guardTrims),
        u64(PS.guardTrims)?double(u64(PS.guardTrimNSum))/double(u64(PS.guardTrims)):0.0,U(PS.playerContacts),U(PS.playerSteep),
        U(PS.coupleHist[0]),U(PS.coupleHist[1]),U(PS.coupleHist[2]),U(PS.coupleHist[3]),U(PS.coupleHist[4]));o+=b;}
     o+="}";
     // CHAIR FIX: the player skip (lever grabplayerskip), the pair's drive mass (grabpairkg), the feed-forward, the pair drive ring
     {const auto& K=s.playerSkip;const auto& H=s.heldMass;
      std::snprintf(b,sizeof b,",\"playerSkip\":{\"on\":%d,\"ours\":%d,\"installs\":%llu,\"restores\":%llu,\"iterators\":%llu,\"lingerIters\":%llu,\"manifolds\":%llu,"
        "\"contacts\":%llu,\"depthMaxMm\":%.1f,\"nsMax\":%llu,\"lingerMsMax\":%llu,\"guardFallback\":%llu,\"lingerDeep\":%llu,\"lingerCapped\":%llu,\"lingerStale\":%llu,"
        "\"light\":{\"lever\":%d,\"holds\":%llu,\"iterators\":%llu,\"contacts\":%llu,\"depthMaxMm\":%.1f},\"lingerEvicted\":%llu},"
        "\"heldMass\":{\"sets\":%llu,\"restores\":%llu,\"misses\":%llu,\"refused\":%llu,\"velKept\":%llu,\"kgBefore\":%.1f,\"kgSet\":%.1f,\"now\":%d},\"feedFwd\":{\"substeps\":%llu,\"maxN\":%llu},\"pairDrive\":{\"writes\":%llu,\"ring\":[",
        st.lev.grabPlayerSkip?1:0,st.skipOurs?1:0,U(K.installs),U(K.restores),U(K.iterators),U(K.lingerIters),U(K.manifolds),U(K.contacts),double(u64(K.depthMaxUm))/1e3,U(K.nsMax),
        U(K.lingerMsMax),U(K.guardFallback),U(K.lingerDeep),U(K.lingerCapped),U(K.lingerStale),st.lev.grabSkipLight?1:0,U(K.lightHolds),U(K.lightIters),U(K.lightContacts),
        double(u64(K.lightDepthMaxUm))/1e3,U(K.lingerEvicted),U(H.sets),U(H.restores),U(H.misses),U(H.refused),U(H.velKept),double(H.kgBefore.load(std::memory_order_relaxed)),
        double(H.kgSet.load(std::memory_order_relaxed)),st.heldMass.set?1:0,U(s.pairFfSubsteps),U(s.pairFfMaxN),U(s.pairDriveWrites));o+=b;
      // oldest first: [ms/10, lagA mm, lagB mm, fwd mm, side mm, up mm, springN, trimN, carryN, aCmd x10, ffN, stall x100, mover cm/s, playerDepth mm, skipped, bits]
      const unsigned n=unsigned(std::min<std::uint64_t>(st.pairDriveN,st.pairDriveRing.size()));
      for(unsigned i=0;i<n;++i){const auto& q=st.pairDriveRing[(st.pairDriveNext+st.pairDriveRing.size()-n+i)%st.pairDriveRing.size()];
        std::snprintf(b,sizeof b,"%s[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]",i?",":"",q[0],q[1],q[2],q[3],q[4],q[5],q[6],q[7],q[8],q[9],q[10],q[11],q[12],q[13],q[14],q[15]);o+=b;}
      o+="]}";}
     // RIGID CARRY (lever grabcarry): substeps, the other hand's substeps it covered, force / torque caps, joins, refusals (no inertia), the
     // ring oldest first: [ms/10, error fwd, side, up mm, speed error cm/s, angle about the line, y, z (deg x 10), spin about the line vs
     // target deg/s, |F| N, |tau| N m x 10, caps (1 F, 2 tau, 4 up at the weight), mover cm/s, ground is the prop, frame turn deg x 10, mover
     // vertical cm/s]
     {std::snprintf(b,sizeof b,",\"carry\":{\"substeps\":%llu,\"shared\":%llu,\"capF\":%llu,\"capTau\":%llu,\"joins\":%llu,\"refused\":%llu,\"predicted\":%llu,"
        "\"kept\":%llu,\"massDiff\":%llu,\"ring\":[",U(G.carrySubsteps),
        U(G.carryShared),U(G.carryCapF),U(G.carryCapTau),U(G.carryJoins),U(G.carryRefused),U(G.carryPredicted),U(G.carryKept),U(G.carryMassDiff));o+=b;
      const unsigned n=unsigned(std::min<std::uint64_t>(st.carryW,st.carryRing.size()));
      for(unsigned i=0;i<n;++i){const auto& q=st.carryRing[(st.carryNext+st.carryRing.size()-n+i)%st.carryRing.size()];
        std::snprintf(b,sizeof b,"%s[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]",i?",":"",q[0],q[1],q[2],q[3],q[4],q[5],q[6],q[7],q[8],q[9],q[10],q[11],q[12],q[13],q[14],q[15]);o+=b;}
      o+="]}";}
     std::snprintf(b,sizeof b,",\"heavy\":{\"heldTicks\":%llu,\"noBodies\":%llu",U(G.heavyHeldTicks),U(G.heavyNoBodies));o+=b;
     arr("nearestMHist",G.heavyNearestHist.data(),5);arr("speedMpsHist",G.heavySpeedHist.data(),5);o+="},\"bodyEnds\":[";
     {bool first=true;for(unsigned i=0;i<st.bodyEnds.size();++i){const auto& r=st.bodyEnds[(st.bodyEndNext+i)%st.bodyEnds.size()];if(!r.set)continue;
        std::snprintf(b,sizeof b,"%s{\"hand\":%u,\"present\":%u,\"state\":\"%s\",\"want\":\"0x%x\",\"devM\":%.3f}",first?"":",",unsigned(r.hand),unsigned(r.present),
            bodyStateNames[r.state%5u],unsigned(r.wantBits),double(r.devM));o+=b;first=false;}}
     o+="],\"latchRing\":[";
     {std::array<grablink::LatchRec,16> lr{};unsigned ln{};{std::lock_guard lock(grablink::latchRingMutex);lr=grablink::latchRing;ln=grablink::latchRingNext;}
      bool first=true;const auto now0=nowNs();
      for(unsigned i=0;i<lr.size();++i){const auto& r=lr[(ln+i)%lr.size()];if(!r.ns)continue;
        std::snprintf(b,sizeof b,"%s{\"ageMs\":%.0f,\"hand\":%u,\"branch\":\"%s\",\"grip\":%.2f,\"readyMs\":%d,\"joinMm\":%d,\"heldMm\":%d,\"otherKind\":%u,\"inside\":%d,\"insideS\":%.2f,\"handsCm\":%.1f}",
            first?"":",",now0>=r.ns?double(now0-r.ns)/1e6:-1.0,unsigned(r.hand),grablink::latchNames[r.branch<grablink::LatchCount?r.branch:0],double(r.grip),int(r.readyMs),
            int(r.joinMm),int(r.heldMm),unsigned(r.otherKind),int(r.inside),double(r.insideS),double(r.handsCm));o+=b;first=false;}}
     o+="]";
     // HEAVY / TWO-HAND HOLDS: the theater chord (samples kept off by a grab, off for another owner, grips zeroed, fires), held ticks
     // while it zeroed, grips snapped to 0, heavy ticks, the pair's up caps by cause, held-hand re-latches (captured), the hold-end ring
     {const auto& C=::rdrvr::runtime::theaterChordStats;const auto L=[](const std::atomic<std::uint64_t>& a){return (unsigned long long)a.load(std::memory_order_relaxed);};
      std::snprintf(b,sizeof b,",\"chair\":{\"chord\":{\"grabBusy\":%llu,\"busy\":%llu,\"consumed\":%llu,\"fired\":%llu,\"consumingNow\":%s},\"heldChordConsuming\":%llu,"
        "\"gripSnapZero\":%llu,\"heavyTicks\":%llu,\"pairUpCap\":{\"share\":%llu,\"force\":%llu},\"relatchHeld\":[%llu,%llu],\"holds\":[",
        L(grablink::chordGrabBusy),L(C.busy),L(C.consumed),L(C.fired),C.consuming.load(std::memory_order_relaxed)?"true":"false",U(G.heldChordConsuming),
        U(G.gripSnapZero),U(G.heavyTicks),U(G.pairUpCapShare),U(G.pairUpCapForce),
        (unsigned long long)grablink::latchBy[0][grablink::LatchHeld].load(std::memory_order_relaxed),(unsigned long long)grablink::latchBy[1][grablink::LatchHeld].load(std::memory_order_relaxed));o+=b;
      {bool first=true;for(unsigned i=0;i<st.holdRecords.size();++i){const auto& r=st.holdRecords[(st.holdNext+i)%st.holdRecords.size()];if(!r.set||!r.ball)continue;
         std::snprintf(b,sizeof b,"%s{\"heavy\":%s,\"heldMs\":%llu,\"end\":\"%s\",\"pairSubsteps\":%u,\"upCapShare\":%u,\"upCapForce\":%u,\"sideCap\":%u,\"slewed\":%u,"
            "\"slewDeficitAvgN\":%.0f,\"snapZero\":%u,\"chordTicks\":%u}",first?"":",",r.heavy?"true":"false",(unsigned long long)r.heldMs,grabEndNames[r.end<GrabEndCount?r.end:0],
            r.pairSubsteps,r.upCapShare,r.upCapForce,r.sideCap,r.slewed,r.slewed?double(r.slewDeficitSumN)/double(r.slewed):0.0,r.snapZero,r.chordTicks);o+=b;first=false;}}
      o+="],\"holdEnds\":[";const auto now0=nowNs();
      for(unsigned h=0;h<2;++h){o+=h?",[":"[";bool first=true;
        for(unsigned i=0;i<st.holdEnds[h].size();++i){const auto& e=st.holdEnds[h][(st.holdEndNext[h]+i)%st.holdEnds[h].size()];if(!e.set)continue;
          std::snprintf(b,sizeof b,"%s{\"ageMs\":%.0f,\"end\":\"%s\",\"heldMs\":%llu,\"heavy\":%s,\"pair\":%s,\"partnerEnd\":\"%s\",\"chord\":%s,\"handMps\":%.2f,\"lagCm\":%.1f,\"grip\":[",
            first?"":",",now0>=e.ns?double(now0-e.ns)/1e6:-1.0,grabEndNames[e.end<GrabEndCount?e.end:0],(unsigned long long)e.heldMs,e.heavy?"true":"false",e.pair?"true":"false",
            e.partnerEnd<GrabEndCount?grabEndNames[e.partnerEnd]:"none",e.chord?"true":"false",double(e.handMps),double(e.lagCm));o+=b;first=false;
          for(unsigned k=0;k<e.n;++k){std::snprintf(b,sizeof b,"%s%.2f",k?",":"",double(e.grip[k]));o+=b;}
          o+="],\"trigger\":[";for(unsigned k=0;k<e.n;++k){std::snprintf(b,sizeof b,"%s%.2f",k?",":"",double(e.trig[k]));o+=b;}o+="]";
          {const auto& q=e.geo;
           std::snprintf(b,sizeof b,",\"over60Ms\":%u,\"geo\":{\"ticks\":%u,\"bodyTicks\":%u,\"minGapCm\":%.1f,\"minDistCm\":%.1f,\"guid\":%u,\"minAgoMs\":%.0f,"
            "\"lastGapCm\":%.1f,\"eng\":%u}}",e.over60Ms,q.ticks,q.bodyTicks,q.minGapM<1e8f?double(q.minGapM*100.f):-1.0,q.minDistM!=-1.f?double(q.minDistM*100.f):-1.0,
            q.guid,q.minNs&&e.endNs>=q.minNs?double(e.endNs-q.minNs)/1e6:-1.0,q.lastGapM!=-1.f?double(q.lastGapM*100.f):-1.0,q.eng);o+=b;}}
        o+="]";}
      o+="]";
      // HOLD-END GRIP LATCH: chord samples passed while unarmed / kept off by a captured grip, the brain's chord-zeroed samples and the
      // squeezes it captured, native latches after a hold end before the open (expected 0), the hold-end ring
      {const auto L2=[](const std::array<std::atomic<std::uint64_t>,2>& a,unsigned i){return (unsigned long long)a[i].load(std::memory_order_relaxed);};
       std::snprintf(b,sizeof b,",\"gripLatch\":{\"on\":%d,\"unarmedPassed\":%llu,\"capturedBusy\":%llu,\"capturedNow\":%u,\"chordZeroSamples\":[%llu,%llu],"
         "\"chordOwnedPresses\":[%llu,%llu],\"holdEndNativeLeak\":[%llu,%llu],\"rows\":[%llu,%llu],\"ring\":[",grablink::holdLatch.load(std::memory_order_relaxed)?1:0,
         L(C.unarmedPassed),L(grablink::chordCapturedBusy),unsigned(grablink::capturedMask.load(std::memory_order_relaxed)),L2(grablink::chordZeroSamples,0),
         L2(grablink::chordZeroSamples,1),L2(grablink::chordOwnedPresses,0),L2(grablink::chordOwnedPresses,1),L2(grablink::holdEndNativeLeak,0),L2(grablink::holdEndNativeLeak,1),
         L2(grablink::holdEndRows,0),L2(grablink::holdEndRows,1));o+=b;
       std::array<grablink::GripLatchRec,16> gr{};unsigned gn{};{std::lock_guard lock(grablink::gripLatchMutex);gr=grablink::gripLatchRing;gn=grablink::gripLatchNext;}
       bool first=true;
       for(unsigned i=0;i<gr.size();++i){const auto& r=gr[(gn+i)%gr.size()];if(!r.ns)continue;
         std::snprintf(b,sizeof b,"%s{\"ageMs\":%.0f,\"hand\":%u,\"end\":\"%s\",\"grip\":%.2f,\"captured\":%u,\"maskedFrames\":%u,\"openMs\":%d,\"chordZeroed\":%u,\"firstLatch\":\"%s\"}",
            first?"":",",now0>=r.ns?double(now0-r.ns)/1e6:-1.0,unsigned(r.hand),r.end<GrabEndCount?grabEndNames[r.end]:"none",double(r.grip),unsigned(r.captured),r.maskedFrames,
            int(r.openMs),r.chordZeroed,r.firstLatch>=0&&r.firstLatch<grablink::LatchCount?grablink::latchNames[unsigned(r.firstLatch)]:"none");o+=b;first=false;}
       o+="]}";}
      o+="}";}
     o+="}";
     std::snprintf(b,sizeof b,"},\"propHit\":{\"armedTicks\":%llu,\"scans\":%llu,\"scanUsMax\":%.1f,\"contacts\":%llu,\"edges\":%llu,\"sent\":%llu,\"strong\":%llu,"
        "\"nearFast\":%llu",U(P.armedTicks),U(P.scans),double(u64(P.scanNsMax))/1e3,U(P.contacts),U(P.edges),U(P.sent),U(P.strong),U(P.nearFast));o+=b;
     arr("approachHist",P.speedHist.data(),6);o+=",\"why\":{";
     for(unsigned i=0;i<PropHitWhyCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",propHitWhyNames[i],U(P.why[i]));o+=b;}
     o+="},\"ring\":[";
     {bool first=true;for(unsigned i=0;i<st.propSamples.size();++i){const auto& x=st.propSamples[(st.propSampleNext+i)%st.propSamples.size()];if(x.why==0xff)continue;
        std::snprintf(b,sizeof b,"%s{\"why\":\"%s\",\"approach\":%.2f,\"swing\":%.2f,\"speed\":%.2f,\"nUp\":%.2f,\"strong\":%u,\"guid\":\"0x%x\"}",first?"":",",
            propHitWhyNames[x.why<PropHitWhyCount?x.why:0],double(x.approach),double(x.swing),double(x.speed),double(x.nUp),unsigned(x.strong),x.guid);o+=b;first=false;}}
     o+="]},";
     // PROPHIT (geometric): see PropGeoStats; the ring of touch starts (and the hits)
     {const auto& Q=s.propGeo;const auto un=u64(Q.usN);
      std::snprintf(b,sizeof b,"\"propGeo\":{\"on\":%d,\"armedTicks\":%llu,\"noCache\":%llu,\"readFails\":%llu,\"parts\":%llu,\"sphereRejects\":%llu,\"capTests\":%llu,"
        "\"usAvg\":%.1f,\"usMax\":%llu,\"touches\":%llu,\"sent\":%llu,\"strong\":%llu,\"engineOnly\":%llu,\"geoOnly\":%llu,\"both\":%llu",st.lev.propHitGeo?1:0,
        U(Q.armedTicks),U(Q.noCache),U(Q.readFails),U(Q.parts),U(Q.sphereRejects),U(Q.capTests),un?double(u64(Q.usSum))/double(un):0.0,U(Q.usMax),U(Q.touches),U(Q.sent),
        U(Q.strong),U(Q.engineOnly),U(Q.geoOnly),U(Q.both));o+=b;
      arr("aHist",Q.aHist.data(),6);arr("swingHist",Q.swingHist.data(),6);arr("armHist",Q.armHist.data(),6);arr("shellCm",Q.shellCm.data(),6);
      arr("armDirHist",Q.armDirHist.data(),6);arr("armAgeHist",Q.armAgeHist.data(),6);
      std::snprintf(b,sizeof b,",\"capRejects\":%llu,\"noArmDir\":%llu,\"nearExact\":%llu,\"bodyWhy\":{\"sphere\":%llu,\"cap\":%llu,\"tested\":%llu,\"touch\":%llu}",
        U(Q.capRejects),U(Q.noArmDir),U(Q.nearExact),U(Q.bodyWhy[0]),U(Q.bodyWhy[1]),U(Q.bodyWhy[2]),U(Q.bodyWhy[3]));o+=b;
      arr("gapCmHist",Q.gapCmHist.data(),7);
      {o+=",\"near\":[";bool fn=true;const auto nw=nowNs();static constexpr const char* whyText[4]{"sphere","cap","tested","touch"};
       for(unsigned i=0;i<st.geoNear.size();++i){const auto& x=st.geoNear[(st.geoNearNext+i)%st.geoNear.size()];if(!x.set)continue;
        std::snprintf(b,sizeof b,"%s{\"ageMs\":%.0f,\"guid\":%u,\"gapCm\":%.1f,\"distCm\":%.1f,\"part\":%d,\"capKind\":%d,\"why\":\"%s\",\"a\":%.2f,\"swing\":%.2f,"
            "\"arm\":%.2f,\"armDir\":%.2f,\"propMps\":%.2f,\"moverMps\":%.2f,\"lagCm\":%.1f,\"msIntoHold\":%u,\"hands\":%u,\"eng\":%u}",fn?"":",",
            nw>=x.ns?double(nw-x.ns)/1e6:-1.0,x.guid,double(x.gapCm),double(x.distCm),x.part,x.kind==0xff?-1:int(x.kind),whyText[x.why&3u],double(x.a),double(x.swing),
            double(x.arm),double(x.armDir),double(x.propMps),double(x.moverMps),double(x.lagCm),x.msIn,unsigned(x.hands),x.eng);o+=b;fn=false;}
       o+="]";}
      o+=",\"why\":{";
      for(unsigned i=0;i<PropHitWhyCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",propHitWhyNames[i],U(Q.why[i]));o+=b;}
      o+="},\"ring\":[";bool first=true;const auto now0=nowNs();
      for(unsigned i=0;i<st.geoRing.size();++i){const auto& x=st.geoRing[(st.geoNext+i)%st.geoRing.size()];if(!x.set)continue;
        std::snprintf(b,sizeof b,"%s{\"ageMs\":%.0f,\"guid\":%u,\"part\":%d,\"zone\":%u,\"kind\":%u,\"distCm\":%.1f,\"a\":%.2f,\"aNow\":%.2f,\"aPrev\":%.2f,\"swing\":%.2f,"
            "\"arm\":%.2f,\"armDir\":%.2f,\"armAgeMs\":%.0f,\"propMps\":%.2f,\"moverMps\":%.2f,\"bodyMps\":%.2f,\"engine\":%u,\"verdict\":\"%s\",\"strong\":%u}",first?"":",",
            now0>=x.ns?double(now0-x.ns)/1e6:-1.0,x.guid,x.part,unsigned(x.zone),unsigned(x.kind),double(x.distCm),double(x.a),double(x.aNow),double(x.aPrev),double(x.swing),
            double(x.arm),double(x.armDir),double(x.armAgeMs),double(x.propMps),double(x.moverMps),double(x.bodyMps),unsigned(x.engine),propHitWhyNames[x.why<PropHitWhyCount?x.why:0],unsigned(x.strong));o+=b;first=false;}
      o+="]},";}
     }
    // THROW: edges, throws, refusals by verdict, launches, pair drops, flights and their ends, in-flight hits / taps by verdict, the cost;
    // the decide ring (one row per judged open edge), the flight ring, the hit ring
    {const auto& T=s.throws;
     std::snprintf(b,sizeof b,"\"throw\":{\"mode\":%u,\"edges\":%llu,\"throws\":{\"light\":%llu,\"heavy\":%llu,\"pair\":%llu,\"lightGrabStep\":%llu,\"midGrabStep\":%llu,\"midGrabRefused\":%llu},"
        "\"measured\":%llu,\"velWrites\":%llu,\"velRefused\":%llu,\"spinClamped\":%llu,\"pairDrops\":%llu,\"flights\":%llu,\"flying\":%u,\"hits\":%llu,\"hitsStrong\":%llu,\"taps\":%llu,"
        "\"tapRefused\":%llu,\"geoUsAvg\":%.1f,\"geoUsMax\":%llu,\"refused\":{",st.lev.throwMode,U(T.edges),U(T.throwsLight),U(T.throwsHeavy),U(T.throwsPair),
        U(T.lightGrabThrows),U(T.midGrabThrows),U(T.midGrabRefused),U(T.measured),U(T.velWrites),U(T.velRefused),U(T.spinClamped),U(T.pairDrops),U(T.flights),
        unsigned(st.flights[0].on?1:0)+unsigned(st.flights[1].on?1:0),U(T.hits),U(T.hitsStrong),U(T.taps),U(T.tapRefused),
        u64(T.geoUsN)?double(u64(T.geoUsSum))/double(u64(T.geoUsN)):0.0,U(T.geoUsMax));o+=b;
     for(unsigned i=0;i<ThrowVerdictCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",throwVerdictNames[i],U(T.refused[i]));o+=b;}
     o+="},\"ends\":{";
     for(unsigned i=1;i<FlightEndCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i>1?",":"",flightEndNames[i],U(T.ends[i]));o+=b;}
     o+="},\"why\":{";
     for(unsigned i=0;i<PropHitWhyCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",propHitWhyNames[i],U(T.why[i]));o+=b;}
     o+="},\"decide\":[";const auto nw=nowNs();bool f1=true;
     for(unsigned i=0;i<st.throwDecides.size();++i){const auto& x=st.throwDecides[(st.throwDecideNext+i)%st.throwDecides.size()];if(!x.ns)continue;
        std::snprintf(b,sizeof b,"%s{\"ageMs\":%.0f,\"hand\":%u,\"hands\":%u,\"pair\":%u,\"heavy\":%u,\"heldMs\":%.0f,\"grip\":[%.2f,%.2f],\"peak\":%.2f,\"peakEdgeMs\":%.0f,"
            "\"est\":%.2f,\"upFrac\":%.2f,\"propMps\":%.2f,\"verdict\":\"%s\",\"launchMps\":%.2f,\"written\":%u,\"kg\":%.1f,\"grabStep\":%u}",f1?"":",",nw>=x.ns?double(nw-x.ns)/1e6:-1.0,
            unsigned(x.hand),unsigned(x.hands),unsigned(x.pair),unsigned(x.heavy),double(x.ageMs),double(x.grip),double(x.gripOther),double(x.peak),double(x.peakEdgeMs),
            double(x.est),double(x.upFrac),double(x.propMps),throwVerdictNames[x.verdict<ThrowVerdictCount?x.verdict:0],double(x.launchMps),unsigned(x.written),double(x.kg),unsigned(x.grabStep));o+=b;f1=false;}
     o+="],\"flight\":[";f1=true;
     for(unsigned i=0;i<st.flightRing.size();++i){const auto& x=st.flightRing[(st.flightRingNext+i)%st.flightRing.size()];if(!x.ns)continue;
        std::snprintf(b,sizeof b,"%s{\"ageMs\":%.0f,\"kg\":%.1f,\"heavy\":%u,\"launchMps\":%.2f,\"maxMps\":%.2f,\"ms\":%.0f,\"distCm\":%.0f,\"gapCm\":%.1f,\"hits\":%u,"
            "\"taps\":%u,\"end\":\"%s\"}",f1?"":",",nw>=x.ns?double(nw-x.ns)/1e6:-1.0,double(x.kg),unsigned(x.heavy),double(x.launchMps),double(x.maxMps),double(x.ms),
            double(x.distCm),double(x.gapCm),unsigned(x.hits),unsigned(x.taps),flightEndNames[x.end<FlightEndCount?x.end:0]);o+=b;f1=false;}
     o+="],\"hits\":[";f1=true;
     for(unsigned i=0;i<st.throwHits.size();++i){const auto& x=st.throwHits[(st.throwHitNext+i)%st.throwHits.size()];if(!x.ns)continue;
        std::snprintf(b,sizeof b,"%s{\"ageMs\":%.0f,\"guid\":\"0x%x\",\"zone\":%d,\"kind\":%d,\"a\":%.2f,\"kg\":%.1f,\"kgA\":%.0f,\"light\":%u,\"strong\":%u,\"ko\":%.2f,"
            "\"engine\":%u,\"verdict\":\"%s\",\"sent\":%u}",f1?"":",",nw>=x.ns?double(nw-x.ns)/1e6:-1.0,x.guid,x.zone==0xff?-1:int(x.zone),x.kind==0xff?-1:int(x.kind),
            double(x.a),double(x.kg),double(x.kg*x.a),unsigned(x.light),unsigned(x.strong),double(x.ko),unsigned(x.engine),propHitWhyNames[x.why<PropHitWhyCount?x.why:0],
            unsigned(x.sent));o+=b;f1=false;}
     o+="]},";}
    {const auto& N=s.npc;
     std::snprintf(b,sizeof b,"\"npcEdges\":{\"windows\":%llu,\"windowSlaps\":%llu,\"windowNone\":%llu,\"escalations\":%llu,\"escalateStill\":%llu,\"slapFail\":{",U(N.windows),U(N.windowSlaps),
        U(N.windowNone),U(N.escalations),U(N.escalateStill));o+=b;
     for(unsigned i=0;i<SlapFailCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",slapFailNames[i],U(N.slapFail[i]));o+=b;}
     o+="},\"shoveFail\":{";
     for(unsigned i=0;i<ShoveFailCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",shoveFailNames[i],U(N.shoveFail[i]));o+=b;}
     const auto arr=[&](const char* name,const Counter* c,unsigned n){o+=",\"";o+=name;o+="\":[";for(unsigned i=0;i<n;++i){o+=i?",":"";o+=std::to_string(u64(c[i]));}o+="]";};
     o+="}";arr("edgeSpeed",N.edgeSpeed[0].data(),16);arr("edgeSpeedPressed",N.edgeSpeed[1].data(),16);arr("edgeA",N.edgeA.data(),16);arr("edgeSwing",N.edgeSwing.data(),16);
     arr("pressTicks",N.pressTicksHist.data(),6);arr("pressCm",N.pressCmHist.data(),6);arr("pressDepthCm",N.pressDepthHist.data(),6);
     arr("pressEndFacing",N.pressEndFacing.data(),8);arr("pressFacingMin",N.pressFacingMin.data(),8);
     std::snprintf(b,sizeof b,",\"slapByArm\":%llu,\"slapHeadFast\":%llu,\"tierSent\":{\"light\":%llu,\"medium\":%llu,\"firm\":%llu}",U(N.slapByArm),U(N.slapHeadFast),
        U(N.tierSent[0]),U(N.tierSent[1]),U(N.tierSent[2]));o+=b;
     std::snprintf(b,sizeof b,",\"graded\":{\"pairs\":%llu,\"pairGateBypass\":%llu,\"slapToPair\":%llu,\"slapHeld\":%llu,\"slapHeldSent\":%llu,\"slapDropped\":%llu,\"slapPushLike\":%llu}",
        U(N.pairs),U(N.pairGateBypass),U(N.slapToPair),U(N.slapHeld),U(N.slapHeldSent),U(N.slapDropped),U(N.slapPushLike));o+=b;
     arr("pairGapMsHist",N.pairGapHist.data(),5);
     std::snprintf(b,sizeof b,",\"pressEnd\":{\"shallow\":%llu,\"facing\":%llu,\"left\":%llu,\"other\":%llu},\"edges\":[",U(N.pressEnd[0]),U(N.pressEnd[1]),U(N.pressEnd[2]),U(N.pressEnd[3]));o+=b;
     {bool first=true;for(unsigned i=0;i<st.edgeSamples.size();++i){const auto& e=st.edgeSamples[(st.edgeNext+i)%st.edgeSamples.size()];if(!e.set)continue;
        std::snprintf(b,sizeof b,"%s{\"capKind\":%d,\"a\":%.2f,\"speed\":%.2f,\"swing\":%.2f,\"facing\":%.2f,\"side\":%.2f,\"tangent\":%.2f,\"depthCm\":%.1f,\"verdict\":\"%s\","
            "\"slapFail\":\"%s\",\"shoveFail\":\"%s\",\"window\":%s,\"arm\":%.2f,\"axis\":%.2f,\"headCm\":%.1f,\"cap\":%d,\"capT\":%.2f,\"tier\":\"%s\","
            "\"hand\":%d,\"guid\":%u,\"ratio\":%.2f,\"gapMs\":%.0f,\"pair\":%s,\"oldVerdict\":\"%s\"}",first?"":",",e.capKind,
            double(e.a),double(e.speed),double(e.swing),double(e.facing),double(e.side),
            double(e.tangent),double(e.depthCm),touchNames[e.verdict&3u],slapFailNames[e.slapFail<SlapFailCount?e.slapFail:0],shoveFailNames[e.shoveFail<ShoveFailCount?e.shoveFail:0],
            e.window?"true":"false",double(e.arm),double(e.axis),double(e.headCm),e.cap,double(e.capT),e.tier<TierCount?tierNames[e.tier]:"none",
            e.hand,e.guid,double(e.ratio),double(e.gapMs),e.pair?"true":"false",touchNames[e.oldVerdict&3u]);o+=b;first=false;}}
     o+="]},";}
    o+="\"hands\":[";
    for(unsigned h=0;h<2;++h){const auto& k=s.hand[h];const auto& hk=hookStats[h];const auto& hs=st.hand[h];
        const auto df=u64(k.devFrames),of=hk.offsetFrames.load();
        std::snprintf(b,sizeof b,"%s{\"state\":\"%s\",\"index\":%u,\"fist\":%s,\"liveFrames\":%llu,\"contactFrames\":%llu,\"contactSubsteps\":%llu,\"pushContacts\":%llu,"
            "\"dvContacts\":%llu,\"contactDynSubsteps\":%llu,\"giveRises\":%llu,\"giveDrops\":%llu,\"normalFlips\":%llu,\"creepClips\":%llu,\"tangentClips\":%llu,\"devCmAvg\":%.2f,\"devCmMax\":%.2f,"
            "\"offsetFrames\":%llu,\"offsetCmAvg\":%.2f,\"offsetCmMax\":%.2f,\"rotFrames\":%llu,\"rotOffDegMax\":%.1f,\"classFrames\":[%llu,%llu,%llu],"
            "\"partToggles\":%llu,\"ghosts\":%llu,\"parkGhosts\":%llu,\"inPlaceReturns\":%llu,\"chestReturns\":%llu,\"respawns\":%llu,"
            "\"inserts\":%llu,\"insertFails\":%llu,\"insertWaits\":%llu,\"removes\":%llu,\"velClampFrames\":%llu,\"pulses\":%llu,"
            "\"pairs\":%llu,\"pairsDropped\":%llu,\"worldPairs\":%llu,\"propPairs\":%llu,\"doorPairs\":%llu,\"corpsePairs\":%llu,\"moverHits\":%llu,\"comMismatch\":%llu,\"impactSkips\":%llu}",
            h?",":"",bodyStateNames[hs.present?hs.machine.state%5u:0u],unsigned(hs.index),hs.fist?"true":"false",U(k.liveFrames),U(k.contactFrames),U(k.contactSubsteps),U(k.pushContacts),
            U(k.dvContacts),U(k.contactDynSubsteps),U(k.giveRises),U(k.giveDrops),U(k.normalFlips),U(k.creepClips),U(k.tangentClips),
            df?double(u64(k.devUmSum))/double(df)/1e4:0.0,double(u64(k.devUmMax))/1e4,
            (unsigned long long)of,of?double(hk.offsetUmSum.load())/double(of)/1e4:0.0,double(hk.offsetUmMax.load())/1e4,
            (unsigned long long)hk.rotFrames.load(),double(hk.rotMdegMax.load())/1e3,
            (unsigned long long)hk.classFrames[0].load(),(unsigned long long)hk.classFrames[1].load(),(unsigned long long)hk.classFrames[2].load(),
            U(k.partToggles),U(k.ghosts),U(k.parkGhosts),U(k.inPlaceReturns),U(k.chestReturns),U(k.respawns),U(k.inserts),U(k.insertFails),U(k.insertWaits),U(k.removes),
            U(k.velClampFrames),U(k.pulses),U(k.pairs),U(k.pairsDropped),U(k.worldPairs),U(k.propPairs),U(k.doorPairs),U(k.corpsePairs),U(k.moverHits),U(k.comMismatch),U(k.impactSkips));o+=b;}
    o+="],\"otherClass\":[";
    for(unsigned i=0;i<16;++i){o+=i?",":"";o+=std::to_string(u64(s.otherClass[i]));}
    o+="],\"samples\":[";bool first=true;
    for(const auto& p:st.samples){if(!p.count)continue;
        std::snprintf(b,sizeof b,"%s{\"class\":%d,\"type\":\"0x%x\",\"include\":\"0x%x\",\"count\":%llu,\"states\":[%u,%u,%u]}",first?"":",",p.cls,p.type,p.include,
            (unsigned long long)p.count,p.states[0],p.states[1],p.states[2]);o+=b;first=false;}
    const auto& d=st.dump;const auto& l=st.lev;
    std::snprintf(b,sizeof b,"],\"dump\":{\"valid\":%s,\"moverType\":\"0x%x\",\"moverInclude\":\"0x%x\",\"moverIndex\":%u,\"moverInstFlags\":\"0x%x\",\"moverState\":%d,"
        "\"behCount\":%u,\"behCap\":%u,\"levelCount\":%u,\"levelActive\":%u,\"levelCap\":%u,\"poolUsed\":%d,\"poolCap\":%d,\"matchFlagsEarly\":%d,\"shouldFindImpacts\":%d,"
        "\"behaviourFromArray\":%d,\"delaySap\":%d,\"stepCount\":%d,\"solverIterations\":%d,\"solverMin\":%d,\"solverMax\":%d,\"preComputeImpacts\":%d,\"preApplyImpacts\":%d,"
        "\"forceCcd\":%d,\"allowedPen\":%.4f,\"sepBias\":%.3f,\"handFlags\":[\"0x%x\",\"0x%x\"]},",
        d.valid?"true":"false",d.moverType,d.moverInclude,unsigned(d.moverIndex),unsigned(d.moverInstFlags),d.moverState,unsigned(d.behCount),unsigned(d.behCap),
        unsigned(d.levelCount),unsigned(d.levelActive),unsigned(d.levelCap),d.poolUsed,d.poolCap,d.matchFlagsEarly,d.shouldFindImpacts,d.behaviourFromArray,d.delaySap,
        d.stepCount,d.solverIterations,d.solverMin,d.solverMax,d.preComputeImpacts,d.preApplyImpacts,d.forceCcd,double(d.allowedPen),double(d.sepBias),
        unsigned(d.handFlags[0]),unsigned(d.handFlags[1]));o+=b;
    std::snprintf(b,sizeof b,"\"levers\":{\"shape\":%u,\"massKg\":%.2f,\"inertia\":%.2f,\"maxSpeedMps\":%.1f,\"maxSpinRadS\":%.1f,\"contactSpinRadS\":%.1f,"
        "\"creepMps\":%.2f,\"pushSpeedMps\":%.2f,\"tangentMps\":%.2f,\"leashCm\":%.1f,\"leashMs\":%.0f,\"returnCm\":%.1f,\"staleMs\":%.0f,\"visualMs\":%.0f,\"holdMs\":%.0f,"
        "\"rotCapDeg\":%.1f,\"liftMm\":%.1f,\"reachWake\":%d,\"reachCm\":%.1f,\"reachLeadMs\":%.0f,\"wakeMax\":%u,\"ccd\":%d,\"ownVt\":%d,\"fist\":%d,\"doors\":%d,\"doorClose\":%d,"
        "\"props\":%d,\"npcBump\":%d,\"npcScan\":%u,\"npc\":%d,\"visual\":%d,\"haptic\":%d,\"radiusCm\":%.1f,\"lengthCm\":%.1f,\"anchorCm\":%.1f,\"type\":\"0x%x\",\"include\":\"0x%x\"",
        l.shape,double(l.massKg),double(l.inertiaK),double(l.maxSpeedMps),double(l.maxAngRadS),double(l.contactSpinRadS),double(l.creepMps),double(l.pushSpeedMps),double(l.tangentMps),
        double(l.leashM*100.f),double(l.leashMs),double(l.returnM*100.f),double(l.staleMs),double(l.visualMs),double(l.holdMs),double(l.rotCapDeg),double(l.liftM*1000.f),
        l.reachWake?1:0,double(l.reachM*100.f),double(l.reachLeadS*1000.f),l.wakeMax,l.ccd?1:0,l.ownVt?1:0,l.fist?1:0,l.doors?1:0,l.doorClose?1:0,l.props?1:0,l.npcBump?1:0,l.npcScanTicks,l.npc?1:0,
        l.visual?1:0,l.haptic?1:0,double(l.radiusM*100.f),double(l.lengthM*100.f),double(l.anchorFwdM*100.f),l.type,l.include);o+=b;
    std::snprintf(b,sizeof b,",\"doorSwing\":%d,\"doorKeep\":%.2f,\"doorRateRadS\":%.1f,\"doorIdleMs\":%.0f,\"unlock\":%d,\"npcBody\":%d,\"npcPress\":%d,"
        "\"npcSlap\":%d,\"npcHorse\":%d,\"slapMps\":%.1f,\"doorFollow\":%d,\"doorRest\":%d,\"doorPlain\":%d,\"doorShutDeg\":%.1f,\"doorRearmMs\":%.0f,"
        "\"release\":%d,\"releaseRCm\":%.1f,\"releaseDepthMm\":%.1f,\"npcShove\":%d,\"npcPush\":%d,\"shoveMps\":%.1f,\"pushGain\":%.2f,\"pushMaxCm\":%.1f,\"pushCapCm\":%.0f,"
        "\"slapYawRad\":%.2f,\"slapReact\":%d,",l.doorSwing?1:0,double(l.doorKeep),double(l.doorRateRadS),double(l.doorIdleMs),
        l.unlock?1:0,l.npcBody?1:0,l.npcPress?1:0,l.npcSlap?1:0,l.npcHorse?1:0,double(l.slapMps),l.doorFollow?1:0,l.doorRest?1:0,l.doorPlain?1:0,double(l.doorShutDeg),
        double(l.doorRearmMs),l.release?1:0,double(l.releaseRM*100.f),double(l.releaseDepthM*1000.f),l.npcShove?1:0,l.npcPush?1:0,double(l.shoveMps),double(l.pushGain),
        double(l.pushMaxM*100.f),double(l.pushCapM*100.f),double(l.slapYawRad),l.slapReact?1:0);o+=b;
    std::snprintf(b,sizeof b,"\"grab\":%d,\"grabCm\":%.1f,\"grabForceN\":%.0f,\"grabTorqueNm\":%.0f,\"grabMassKg\":%.1f,\"grabKg\":%.0f,\"grabRot\":%d,\"grabClass\":\"0x%x\","
        "\"grabOpenMs\":%.0f,\"grabLeashCm\":%.0f,\"grabThrow\":%.2f,\"cpu\":%d,\"unfreeze\":%d,\"unfreezeRCm\":%.1f,\"unfreezeKg\":%.2f,\"grabReleaseRCm\":%.0f},\"mvDrops\":%llu}",
        l.grab?1:0,double(l.grabM*100.f),double(l.grabForceN),double(l.grabTorqueNm),
        double(l.grabMassKg),double(l.grabMaxKg),l.grabRot?1:0,l.grabClass,double(l.grabOpenMs),double(l.grabLeashM*100.f),double(l.grabThrow),l.cpu?1:0,
        l.unfreeze?1:0,double(l.unfreezeRM*100.f),double(l.unfreezeKg),double(l.grabReleaseRM*100.f),
        (unsigned long long)mvDrops.load());o+=b;
    {const auto at=o.rfind("},\"mvDrops\"");if(at!=std::string::npos)o.resize(at);} // the levers object reopened for the approach, glass, cup, grab and touch levers
    std::snprintf(b,sizeof b,",\"blood\":%d,\"bloodCdMs\":%.0f,\"grabSeat\":%d,\"grabWeld\":%d,\"grabLightFf\":%d,\"bloodHires\":%d,\"grabSkipLight\":%d",l.blood?1:0,
        double(l.bloodCdMs),l.grabSeat?1:0,l.grabWeld?1:0,l.grabLightFf?1:0,l.bloodHires?1:0,l.grabSkipLight?1:0);o+=b;
    std::snprintf(b,sizeof b,",\"bloodPlace\":%d,\"bloodMps\":%.2f",l.bloodPlace?1:0,double(l.bloodMps));o+=b;
    std::snprintf(b,sizeof b,",\"doorPress\":%d",l.doorPress?1:0);o+=b;
    std::snprintf(b,sizeof b,",\"gunGlass\":%d,\"gunGlassFree\":%d,\"gunGlassMps\":%.2f,\"gunGlassR\":%.3f,\"gunGlassLong\":%d",l.gunGlass,l.gunGlassFree,
        double(l.gunGlassMps),double(l.gunGlassR),l.gunGlassLong?1:0);o+=b;
    std::snprintf(b,sizeof b,",\"doorAuto\":%d,\"doorBody\":%d,\"doorHandPose\":%d,\"doorStuckMs\":%.0f,\"glass\":%d,\"glassMps\":%.2f,\"glassDmg\":%.1f,"
        "\"unfreezeParts\":%u,\"unfreezeSetKg\":%.2f,\"unfreezeKick\":%d,\"unfreezeNear\":%d,\"unfreezeAheadMs\":%.0f,\"grabDrive\":%d,\"grabGain\":%.2f,\"grabForceMaxN\":%.0f,\"grabAccel\":%.1f,"
        "\"grabLiftKg\":%.1f,\"grabRotKg\":%.1f,\"grabBreakMs\":%.0f,\"grabDriveArt\":%d,\"grabMove\":%d,\"grabMoveKg\":%.1f,\"grabMoveRCm\":%.0f,"
        "\"grabGrip\":%d,\"grabWn\":%.1f,\"grabZeta\":%.2f,\"grabSlewN\":%.0f,\"grabDownG\":%.2f,\"slapAttacker\":%u,"
        "\"pushAttacker\":%u,\"pressPushCm\":%.1f,\"slapWindowMs\":%.0f,\"slapNode\":\"%s\",\"pushNode\":\"%s\"},\"mvDrops\":%llu}",l.doorAuto?1:0,l.doorBody?1:0,l.doorHandPose?1:0,double(l.doorStuckMs),
        l.glass?1:0,double(l.glassMps),double(l.glassDmg),l.unfreezeParts,double(l.unfreezeSetKg),l.unfreezeKick?1:0,l.unfreezeNear?1:0,double(l.unfreezeAheadS*1000.f),l.grabDrive?1:0,double(l.grabGain),
        double(l.grabForceMaxN),double(l.grabAccel),double(l.grabLiftKg),double(l.grabRotKg),double(l.grabBreakMs),l.grabDriveArt?1:0,l.grabMove?1:0,double(l.grabMoveKg),
        double(l.grabMoveRM*100.f),l.grabGrip?1:0,double(l.grabWn),double(l.grabZeta),double(l.grabSlewN),double(l.grabDownG),l.slapAttacker,l.pushAttacker,double(l.pressPushM*100.f),double(l.slapWindowMs),safeText(st.nodes.slap).c_str(),safeText(st.nodes.push).c_str(),
        (unsigned long long)mvDrops.load());o+=b;
    {const auto at=o.rfind("},\"mvDrops\"");if(at!=std::string::npos)o.resize(at);} // the levers object reopened: reset hold, push tiers, two hands, prop hits
    std::snprintf(b,sizeof b,",\"resetHold\":%d,\"pushFirmMps\":%.2f,\"pushFirmCm\":%.1f,\"pushArmMps\":%.2f,\"pushImpulse\":%.2f,\"pushMedium\":%u,\"pushKick\":%d,"
        "\"pushKickK\":%.1f,\"pushKickMaxNs\":%.1f,\"grab2\":%d,\"grabLift2Kg\":%.1f,\"grabJoinCm\":%.1f,\"grabFirst\":%d,\"heavyMask\":%d,\"heavyMaskCm\":%.1f,\"propHit\":%d,\"propHitMps\":%.2f,"
        "\"propHitStrongMps\":%.2f,\"propHitCoolMs\":%.0f,\"propHitKg\":%.1f,\"propKo\":%.2f},"
        "\"mvDrops\":%llu}",l.resetHold?1:0,double(l.pushFirmMps),double(l.pushFirmM*100.f),double(l.pushArmMps),double(l.pushImpulse),l.pushMedium,l.pushKick?1:0,
        double(l.pushKickK),double(l.pushKickMaxNs),l.grab2?1:0,double(l.grabLift2Kg),double(l.grabJoinM*100.f),l.grabFirst?1:0,l.heavyMask?1:0,double(l.heavyMaskM*100.f),l.propHit?1:0,double(l.propHitMps),
        double(l.propHitStrongMps),double(l.propHitCoolMs),double(l.propHitKg),double(l.propKo),(unsigned long long)mvDrops.load());o+=b;
    {const auto at=o.rfind("},\"mvDrops\"");if(at!=std::string::npos)o.resize(at);} // the levers object reopened: graded push
    std::snprintf(b,sizeof b,",\"pushGrade\":%d,\"pushDownMps\":%.2f,\"slapHoldMs\":%.0f,\"pairMs\":%.0f,\"pushKick2\":%d,\"grabChord\":%d,\"grabHeldLatch\":%d,"
        "\"grabHeavyHold\":%d,\"grabPairFull\":%d,\"grabPairSlew\":%d,\"grabHeavyDraw\":%d},\"mvDrops\":%llu}",
        l.pushGrade?1:0,double(l.pushDownMps),double(l.slapHoldMs),double(l.pairMs),l.pushKick2?1:0,l.grabChord?1:0,l.grabHeldLatch?1:0,l.grabHeavyHold?1:0,
        l.grabPairFull?1:0,l.grabPairSlew?1:0,l.grabHeavyDraw?1:0,(unsigned long long)mvDrops.load());o+=b;
    {const auto at=o.rfind("},\"mvDrops\"");if(at!=std::string::npos)o.resize(at);} // the levers object reopened: the hold-end grip latch
    std::snprintf(b,sizeof b,",\"grabHoldLatch\":%d,\"grabGhost\":%d},\"mvDrops\":%llu}",l.grabHoldLatch?1:0,l.grabGhost?1:0,(unsigned long long)mvDrops.load());o+=b;
    {const auto at=o.rfind("},\"mvDrops\"");if(at!=std::string::npos)o.resize(at);} // the levers object reopened: passive contact, pair statics, geometric prop hit
    std::snprintf(b,sizeof b,",\"npcPassive\":%d,\"activeMps\":%.2f,\"pressDeadMps\":%.2f,\"shoveArmMps\":%.2f,\"statics\":%d,\"guard\":%d,"
        "\"propHitGeo\":%d,\"propHitArmMps\":%.2f,\"propHitShellCm\":%.1f,\"propHitWinMs\":%.0f,\"propHitArmMs\":%.0f,\"propHitArmDirMps\":%.2f},\"mvDrops\":%llu}",
        l.npcPassive?1:0,double(l.activeMps),double(l.pressDeadMps),double(l.shoveArmMps),l.grabPairStatics?1:0,l.grabPlayerGuard?1:0,
        l.propHitGeo?1:0,double(l.propHitArmMps),double(l.propHitShellM*100.f),double(l.propHitWinMs),double(l.propHitArmMs),double(l.propHitArmDirMps),
        (unsigned long long)mvDrops.load());o+=b;
    {const auto at=o.rfind("},\"mvDrops\"");if(at!=std::string::npos)o.resize(at);} // the levers object reopened: heavy hold, player skip, pair mass, push fix
    std::snprintf(b,sizeof b,",\"grabHeavyLeash\":%d,\"grabSafetyCm\":%.0f,\"grabTeleportCm\":%.0f,\"grabPlayerSkip\":%d,\"grabSkipLingerMs\":%.0f,\"grabPairKg\":%.1f,"
        "\"grabHeldAccel\":%.1f,\"grabFeedFwd\":%d,\"pushBusyWait\":%d,\"pushBusyWaitMs\":%.0f,\"pushEscGrade\":%d,\"pushFacingBody\":%d,\"pushPeak\":%d,\"pushPeakMs\":%.0f,"
        "\"pushFront\":%u,\"pushFrontImp\":%.1f,\"pushKickHigh\":%d,\"pushFrozenMsg\":%d,\"pushBehindImp\":%.1f,\"pushSide\":%u",
        l.grabHeavyLeash?1:0,double(l.grabSafetyM*100.f),double(l.grabTeleportM*100.f),l.grabPlayerSkip?1:0,double(l.grabSkipLingerMs),double(l.grabPairKg),
        double(l.grabHeldAccel),l.grabFeedFwd?1:0,l.pushBusyWait?1:0,double(l.pushBusyWaitMs),l.pushEscGrade?1:0,l.pushFacingBody?1:0,l.pushPeak?1:0,double(l.pushPeakMs),
        l.pushFront,double(l.pushFrontImp),l.pushKickHigh?1:0,l.pushFrozenMsg?1:0,double(l.pushBehindImp),l.pushSide);o+=b;
    std::snprintf(b,sizeof b,",\"pushKnock\":%u,\"pushFrozenHold\":%d,\"grabCarry\":%d,\"grabCarryWn\":%.1f,\"grabCarryZeta\":%.2f,\"grabCarryAccel\":%.0f,\"grabRotWn\":%.1f,"
        "\"grabRotZeta\":%.2f,\"grabRotNm\":%.0f,\"grabLineTauMs\":%.0f,\"grabStaleOwn\":%d,\"grabGroundZero\":%d,\"grabFallbackTauMs\":%.0f,\"grabGroundLift\":%d,"
        "\"grabCarryPredict\":%d,\"grabCarryKeepMs\":%.0f,\"pushKnockFlip\":%u,\"pushKnockPick\":%u,\"pushKnockPickFwd\":%.2f,\"pushKnockSpine\":%d,\"pushKnockPickMsg\":%u,"
        "\"pushKnockTough\":%d,\"pushKnockPickFunc\":%d,\"knockOffMs\":%.0f,\"pushKickServo\":%d,\"pushKickEarly\":%d},"
        "\"mvDrops\":%llu}",
        l.pushKnock,l.pushFrozenHold?1:0,l.grabCarry?1:0,double(l.grabCarryWn),double(l.grabCarryZeta),double(l.grabCarryAccel),double(l.grabRotWn),double(l.grabRotZeta),
        double(l.grabRotNm),double(l.grabLineTauMs),l.grabStaleOwn?1:0,l.grabGroundZero?1:0,double(l.grabFallbackTauMs),l.grabGroundLift?1:0,l.grabCarryPredict?1:0,
        double(l.grabCarryKeepMs),l.pushKnockFlip,l.pushKnockPick,double(l.pushKnockPickFwd),l.pushKnockSpine?1:0,l.pushKnockPickMsg,l.pushKnockTough?1:0,
        l.pushKnockPickFunc?1:0,double(l.knockOffMs),l.pushKickServo?1:0,l.pushKickEarly?1:0,(unsigned long long)mvDrops.load());o+=b;
    {const auto at=o.rfind("},\"mvDrops\"");if(at!=std::string::npos)o.resize(at);} // the levers object reopened: the throw
    std::snprintf(b,sizeof b,",\"throw\":%u,\"throwLight\":%u,\"throwMps\":%.2f,\"throwMpsLight\":%.2f,\"throwRelMps\":%.2f,\"throwRelMpsLight\":%.2f,\"throwWinMs\":%.0f,"
        "\"throwPairMs\":%.0f,\"throwMaxMps\":%.1f,\"throwMaxMpsLight\":%.1f,\"throwFlightMs\":%.0f,\"throwHitMps\":%.2f,\"throwStrongKgMps\":%.0f,\"throwKo\":%.2f},"
        "\"mvDrops\":%llu}",l.throwMode,l.throwLight,double(l.throwMps),double(l.throwMpsLight),double(l.throwRelMps),double(l.throwRelMpsLight),double(l.throwWinMs),
        double(l.throwPairMs),double(l.throwMaxMps),double(l.throwMaxMpsLight),double(l.throwFlightMs),double(l.throwHitMps),double(l.throwStrongKgMps),double(l.throwKo),
        (unsigned long long)mvDrops.load());o+=b;
    return o;
}
} // namespace rdrvr::vr::handphys
