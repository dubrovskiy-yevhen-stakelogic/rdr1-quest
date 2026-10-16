#pragma once
// HOLSTERS - VR body holsters for RDR1's native equip slots, weapon follows the grabbing hand,
// two-hand aim, haptics.
//
// Threads:
//  - input thread (= engine main thread, inside d11::InputSystem_c::Update via NativePadDriver's pad filter):
//    padFrame() - body frame + slot sockets in OpenXR LOCAL, grip latches, grab/put-away decisions (posted as
//    VrMainThread commands), grip masking, trigger routing, two-hand support, holster markers (seqlock).
//  - engine main thread (UpdateInputs wrapper, VrMainThread executor): tick() polls the player inventory with guarded
//    block reads and publishes atomics; execute() runs SelectItem / PutAwayWeapon behind the native gates.
//  - weapon hooks (main thread): weaponInput() substitutes logical weapon hand 1 with the physical holding hand
//    (PhysicalSidearm::poseInput approach) and applies the two-hand barrel rotation.
//  - render thread (HANDS): markers(), handHolding(). XR thread: noteMenu(); haptics via VrHaptics.h.
// Kill switch: debug.rdrvr.vr.holsters 0 (read once at startup): nothing is configured, every entry point is pass-through.
// DUAL: hand-to-hand transfer (grab the item in the other hand), a second "phantom" item in the other
// hand (knife + revolver, revolver + rifle; the engine keeps it holstered, its holstered Update is re-posed), promotion when the engine
// hand lets go, swap-fire of a phantom firearm. Menu DUAL WIELD / debug.rdrvr.vr.dual 0 = the single-weapon behaviour above.
// THROW: a fast HOLD release of a sidearm throws it (OpThrow = the put-away + the visual flight),
// an empty hand closes on the flying / lying gun (OpCatch = a draw of that slot).
// (1) EMPTY HANDS: no trigger ever reaches the game as LT / RT while no weapon is in a VR hand and the
// holsters can provide one (the engine's FIRE / TARGET with a holstered weapon draws the current weapon; RT -> inputs
// 9 FIRE + 13 MELEE); weapons come only from the holsters, fists are the physical melee. (2) A weapon the GAME drew (wheel, script, ...)
// is adopted into the VR HOLD: opening that hand holsters it (a closed hand at the draw = its hold press; an open one = the next squeeze,
// or the open hand holsters it after gameDrawOpenNs). (3) FIST BLOCK guard pose (VrGuard.h): detected here, raised fists' grips masked.
#include "VrMainThread.h"
#include "VrHaptics.h"
#include "VrThrow.h"
#include "VrGuard.h"
#include "VrLassoLink.h" // LASSO: brain (route / keepHold / ownsHand / dwell), grade H rules, PlayerLink
#include "VrSpin.h"      // SPIN: the revolver on the trigger finger (brain, hinge sim, exits)
#include "VrFireDiagnostics.h"
#include "runtime/ControllerInput.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <type_traits>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace rdrvr::vr::holsters {
using runtime::ControllerSample;
using runtime::PadState;

// ---- engine identities (librdr.so sha256 3f80845e..., .text file offset = vaddr - 0x4000) ----
inline constexpr std::uintptr_t selectItemOffset=0x2664610;      // invInventoryComponent::SelectItem(invBaseItem&, bool draw, bool animated, bool)
inline constexpr std::uintptr_t putAwayWeaponOffset=0x269ff74;   // sagPlayer::PutAwayWeapon(invInventoryComponent*) = UpdateWeaponSpecialMove(2) + PutAwayItem
inline constexpr std::uintptr_t canDrawOffset=0x269f228;         // sagPlayer::CanDrawWeaponOfType(invWeaponType const*) const
inline constexpr std::uintptr_t putAwayItemOffset=0x26666f0;     // invInventoryComponent::PutAwayItem() (tail of PutAwayWeapon; head fingerprint)
inline constexpr std::uintptr_t startItemChangeOffset=0x2666994; // invInventoryComponent::StartItemChange(...) (called by SelectItem; head fingerprint)
inline constexpr std::uintptr_t inventoryTableOffset=0x2d8c068;  // vtable for invInventoryComponent + 0x10 (== TraceSymbols weaponInventoryTableOffset)
inline constexpr std::uintptr_t weaponItemTableOffset=0x2d8d248; // vtable for invWeapon + 0x10 (== weaponItemTableOffset)
inline constexpr std::uintptr_t lassoItemTableOffset=0x2d8d498;  // vtable for invLassoWeapon + 0x10
inline constexpr std::uintptr_t weaponTypeTableOffset=0x2d8d3b0; // vtable for invWeaponType + 0x10
inline constexpr std::uintptr_t lassoTypeTableOffset=0x2d67158;  // vtable for invLassoWeaponType + 0x10
// Whole SelectItem(item&), CanDrawWeaponOfType and PutAwayWeapon; 64-byte heads of PutAwayItem and StartItemChange.
inline constexpr std::array<unsigned char,500> selectItemFingerprint{
#include "gamecode/VrHolsters_2664610_500_8d.inc"
};
inline constexpr std::array<unsigned char,248> canDrawFingerprint{
#include "gamecode/VrHolsters_269f228_248_8d.inc"
};
inline constexpr std::array<unsigned char,40> putAwayWeaponFingerprint{
#include "gamecode/VrHolsters_269ff74_40_8d.inc"
};
inline constexpr std::array<unsigned char,64> putAwayItemFingerprint{
#include "gamecode/VrHolsters_26666f0_64_8d.inc"
};
inline constexpr std::array<unsigned char,64> startItemChangeFingerprint{
#include "gamecode/VrHolsters_2666994_64_8d.inc"
};
// WEAPON KEEP. Whole functions, both verified in librdr.so:
//  sagPlayer::SetRDR1HoldMode(bool, InputDisableFlag) 0x2698da4: if [[player+0x410]+9]==0 -> player+0x3e0 =
//   PlayerTuneData+0xf8 (HoldModeTimeOutTune, parser_Register 0x26ad368/0x26ad380); bool true also ORs the flag
//   into +0x3e4. SelectItem calls it with (false,2) on every draw (0x26647f0). UpdateWeaponSelection (0x269f320)
//   state 1 counts +0x3e0 down (0x269f7e4..0x269f7f4) and, with (+0x3bc & 0xa) == 8 (RDR1 aim mode bit3 set,
//   force-aim bit1 clear), not aiming and the draw/targeting input up, calls PutAwayItem once it reaches 0
//   (0x269fbcc..0x269fd48): the RDR1 "trigger puts it back" auto-holster.
//  sagPlayerHandle::SetForcePlayerAimMode(bool) 0x26a951c: player(PLAYERMGR+0xa20)+0x3bc bit1 (x0 unused). bit1
//   makes GetDesiredZoomModeAnalog return 2 (0x269cbd8) -> foot SetupActionTreePacket aims (aniAim+0x5e9 |= 0x100,
//   0x26b684c..0x26b6880), and disables both RDR1-mode auto-holsters (0x269fbb4, 0x269fbd0). With bit1 set and
//   NOTHING in hand the same machine draws the next weapon (state 3, 0x269fb48 -> 0x269f8e4), so the mod sets
//   the bit only while a weapon is in the VR hand (cleared every tick the hand is empty and before every put-away).
inline constexpr std::uintptr_t setHoldModeOffset=0x2698da4,setForceAimOffset=0x26a951c;
inline constexpr std::array<unsigned char,56> setHoldModeFingerprint{
#include "gamecode/VrHolsters_2698da4_56_8d.inc"
};
inline constexpr std::array<unsigned char,44> setForceAimFingerprint{
#include "gamecode/VrHolsters_26a951c_44_8d.inc"
};
inline constexpr std::size_t playerAimBits=0x3bc,playerHoldTimer=0x3e0,playerSelectState=0x504,playerZoom=0x244;
inline constexpr std::uint8_t aimBitForce=0x02,aimBitRdr1=0x08;
// DUAL (verified against librdr.so):
//  weapWeapon::SetShouldRender(bool) 0x2548614 (whole, 232 B): weapon+0x120 bit6 + entity flags 0x100 - the phantom is drawn even when
//   its holster hides it; invWeapon::RefreshWeaponRender() 0x2676c38 (whole, 140 B): back to the holster rule (OnReHolster's formula).
//  Proof only (never called): invWeapon::OnUpdatePosition tail 0x2677060 (68 B: bl weapWeapon::Update,
//   ret 0x26770a4 = the mod's pinned pose caller) and invInventoryComponent::UpdateHolsteredItemPositions
//   0x266981c (204 B: vt+0x78 for all 8 slots, every frame).
//   A mismatch disables DUAL ("ENGINE MISMATCH"); the transfer between hands does not need them and stays available.
inline constexpr std::uintptr_t setShouldRenderOffset=0x2548614,refreshWeaponRenderOffset=0x2676c38,onUpdatePositionCallOffset=0x2677060,
    updateHolsteredOffset=0x266981c;
inline constexpr std::array<unsigned char,232> setShouldRenderFingerprint{
#include "gamecode/VrHolsters_2548614_232_8d.inc"
};
inline constexpr std::array<unsigned char,140> refreshWeaponRenderFingerprint{
#include "gamecode/VrHolsters_2676c38_140_8d.inc"
};
inline constexpr std::array<unsigned char,68> onUpdatePositionCallFingerprint{
#include "gamecode/VrHolsters_2677060_68_8d.inc"
};
inline constexpr std::array<unsigned char,204> updateHolsteredFingerprint{
#include "gamecode/VrHolsters_266981c_204_8d.inc"
};
inline constexpr std::size_t holsteredMatrixOffset=0x80; // weapWeapon world Matrix34 (VrWeapon.h: SetTransform(weapon+0x80), PhantomFix)
inline constexpr std::size_t itemWeapon=0xa0,weaponFlags=0x120;inline constexpr std::uint32_t weaponRenderBit=0x40;

inline constexpr std::size_t invOwner=0x10,invReady=0x18,invHand=0x80,invSlotItem=0xa8,invSlotStride=0x70,invEffectors=0x428,
    invFlags=0x48c,invSlotMask=0x48e,invBlock=0x490;
inline constexpr std::uint16_t forceHolsterSoft=0x20,forceHolsterHard=0x40;
inline constexpr std::size_t itemState=0x1c,itemOwnerGuid=0x20,itemType=0x30,itemBlock=0x38,typeName=0x8,typeWeaponData=0x50,typeBlock=0x58;
inline constexpr std::size_t playerLocks=0x104,playerLock1=0x10c,playerLock2=0x10d,playerStates=0x410,playerFoot=0x5d4,playerActive=0x5f1,
    playerGuid=0x5f4,playerBlock=0x5f8,statesDeadEye=0x20;
inline constexpr std::size_t actorHealth=0x60,actorInventory=0x70,actorFlags=0x118,actorBlock=0x120,healthValue=0x20;
inline constexpr std::uint32_t footOnFoot=2,footHorse=5;

// ---- slots (invBaseType::EquipPositions) and sockets (metres; right / up / forward from the upright head) ----
enum Slot : int {SlotMelee=0,SlotSidearm=1,SlotThrown=2,SlotLasso=3,SlotSniper=4,SlotLongarm=5,SlotShotgun=6};
inline constexpr int slotCount=7;
struct SlotSpec {const char* name;float right,up,forward;bool twoHanded;};
inline constexpr std::array<SlotSpec,slotCount> slotSpecs{{
    {"MELEE",  -.17f,-.42f, .14f,false},  // chest left / buckle (knife, cross draw)
    {"SIDEARM", .22f,-.58f, .05f,false},  // right hip (SIDEARM_RGT)
    {"THROWN",  .00f,-.30f, .16f,false},  // chest centre (dynamite, bottles, throwing knives)
    {"LASSO",  -.22f,-.58f, .05f,false},  // left hip
    {"SNIPER",  .00f, .08f,-.14f,true },  // behind the head, high
    {"LONGARM", .20f,-.15f,-.15f,true },  // behind the right shoulder
    {"SHOTGUN",-.20f,-.15f,-.15f,true },  // behind the left shoulder
}};
inline constexpr float neckDown=.10f,neckBack=.08f;              // eye -> neck pivot (head pitch does not swing the belt)
inline constexpr float grabRadius=.18f,leaveRadius=.22f,nearRadius=.45f,foregripReach=.35f,supportRadius=.18f;
inline constexpr float gripClose=.65f,gripOpen=.30f,menuLatchGrip=.45f,triggerRearm=.15f,stickDeadZone=.15f;
inline constexpr float yawDeadZone=35.f*3.14159265f/180.f,yawCatchUp=90.f*3.14159265f/180.f,yawSettle=3.f*3.14159265f/180.f;
inline constexpr float moveYawWindow=45.f*3.14159265f/180.f,twoHandMaxAngle=90.f*3.14159265f/180.f;
inline constexpr std::uint64_t menuFreezeNs=300'000'000,drawConfirmNs=2'500'000'000,busyRetryNs=600'000'000,inputMaxAgeNs=150'000'000;
// HOLD release of the holding hand while its trigger is squeezed (Quest: pulling the trigger relaxes the middle finger)
// waits for the trigger to be released, then for the grip to stay open releaseSettleNs before holstering.
inline constexpr float releaseTriggerGuard=.35f,triggerActive=.08f;
inline constexpr std::uint64_t releaseSettleNs=150'000'000,triggerAimHoldNs=1'500'000'000;
// LASSO: VrLassoLink.h mirrors these (it must not include this header)
static_assert(lasso::gripClose==gripClose&&lasso::gripOpen==gripOpen&&lasso::releaseTriggerGuard==releaseTriggerGuard&&lasso::triggerActive==triggerActive&&
              lasso::releaseSettleNs==releaseSettleNs,"lasso mirror of the holster grip constants");
static_assert(spin::gripClose==gripClose&&spin::gripOpen==gripOpen&&spin::releaseTriggerGuard==releaseTriggerGuard&&spin::triggerRearm==triggerRearm,
              "spin mirror of the holster grip constants");
// LASSO: the lasso socket's "rope" buzz (instead of the 25 ms belt tick), the DUAL
// dwell of a free-hand socket grab while the rope matters.
inline constexpr float lassoEnterAmplitude=.30f;inline constexpr std::uint32_t lassoEnterUs=45'000;inline constexpr std::uint64_t lassoDwellNs=300'000'000;
// DUAL: OpTransfer = the engine item changes hands (grab at the item),
// OpPhantomSet = a second item shown in the other hand (the engine keeps it holstered; its own holstered weapWeapon::Update is
// re-posed), OpPhantomClear = the phantom goes back to its holster (no engine call), OpDrawKeep = a draw that keeps the previous engine
// item as the phantom in hand arg[3] (-1 none) / promotes the phantom to the engine item; arg[2] bit0 = swap-fire.
// THROW: OpThrow arg[0] slot hint, arg[1] hand, arg[2] Release box seq, arg[3] 1 = the phantom; OpCatch arg[0] the thrown slot,
// arg[1] the catching hand, arg[2] 0 air / 1 closed hand / 2 ground, arg[3] the flight generation.
enum Op : std::uint16_t {OpDraw=1,OpPutAway=2,OpTransfer=3,OpPhantomSet=4,OpPhantomClear=5,OpDrawKeep=6,OpThrow=7,OpCatch=8};
enum PutAwayReason : std::int32_t {PutAwaySlot=0,PutAwayHoldRelease=1};

// ---- configuration (VR page rows HOLSTERS / WEAPON GRIP / HOLSTER MARKERS / TWO HAND AIM / DUAL WIELD; any thread) ----
enum class GripMode : std::uint8_t {Hold,Toggle};
enum class MarkerMode : std::uint8_t {Near,Always,Off};
struct Config {bool enabled{true};GripMode grip{GripMode::Hold};MarkerMode markers{MarkerMode::Near};bool twoHand{true};bool dual{true};}; // dual = menu row DUAL WIELD
inline std::atomic<std::uint32_t> configBits{1u|(0u<<1)|(0u<<2)|(1u<<4)|(1u<<5)};
inline void setConfig(const Config& c) noexcept {
    configBits.store((c.enabled?1u:0u)|(c.grip==GripMode::Toggle?2u:0u)|(std::uint32_t(c.markers)<<2)|(c.twoHand?16u:0u)|(c.dual?32u:0u),std::memory_order_relaxed);
}
inline Config config() noexcept {
    const auto b=configBits.load(std::memory_order_relaxed);Config c{};
    c.enabled=b&1u;c.grip=(b&2u)?GripMode::Toggle:GripMode::Hold;const auto m=(b>>2)&3u;c.markers=m==1?MarkerMode::Always:m==2?MarkerMode::Off:MarkerMode::Near;
    c.twoHand=(b&16u)!=0;c.dual=(b&32u)!=0;return c;
}

// ---- state published by the main thread (poll / executor), read by the input brain and the weapon hooks ----
inline std::atomic<bool> killedByProperty{},engineConfigured{},engineReady{},inventoryValid{},contextAllowed{};
inline std::atomic<const char*> engineState{"NOT_CONFIGURED"},blockReason{"NO SAMPLE"};
inline std::atomic<std::uint32_t> ownedMask{},grabMask{};
inline std::atomic<int> currentSlot{-1},holdHand{-1};
inline std::atomic<bool> holdOurs{};
inline std::atomic<std::uint32_t> holdSequence{},holdGeneration{};
// ---- state published by the input brain ----
inline std::atomic<int> supportHand{-1};
inline std::atomic<std::uint32_t> attachmentGeneration{}; // changes whenever the physical weapon hand or the support grip changes
inline std::atomic<std::uint64_t> menuSeenNs{};
inline std::atomic<float> bodyYawPublished{};
inline std::array<std::atomic<int>,2> insidePublished{{{-1},{-1}}}; // -1 before the first brain frame (0 = MELEE socket)

// ---- counters (report) ----
enum Refusal : unsigned {RefuseNotReady,RefuseNoPlayer,RefuseNotInGame,RefuseDead,RefuseLocked,RefuseDeadEye,RefuseCover,RefuseContext,
    RefuseMounted,RefuseNotOnFoot,RefuseForceHolster,RefuseSlotDisabled,RefuseEmptySlot,RefuseNotWeapon,RefuseBusy,RefuseGameRefused,
    RefuseSelectFailed,RefuseNotDrawn,RefuseException,RefuseCount};
inline constexpr std::array<const char*,RefuseCount> refusalNames{"NOT_READY","NO_PLAYER","NOT_IN_GAME","DEAD","CONTROL_LOCKED","DEAD_EYE",
    "COVER","CONTEXT","MOUNTED","NOT_ON_FOOT","FORCE_HOLSTER","SLOT_DISABLED","EMPTY_SLOT","NOT_A_WEAPON","BUSY","GAME_REFUSED","SELECT_FAILED",
    "NOT_DRAWN","EXCEPTION"};
inline std::array<std::atomic<std::uint32_t>,RefuseCount> refusals{};
inline std::atomic<const char*> lastRefusal{"NONE"};
inline std::atomic<std::uint64_t> backMasked{},mouthWhistles{};
inline std::atomic<std::uint64_t> inputFrames{},suspendedFrames{},grabPresses{},putAwayPresses{},holdReleases{},nativeGripPresses{},
    supportEngaged{},swappedFrames{},rtHeldFrames{},postFailures{},resumes{},twoHandApplied{},handSubstituted{},
    draws{},drawsConfirmed{},alreadyInHand{},putAways{},putAwayEmpty{},polls{},pollFailures{},contextReads{},retries{},superseded{},gameDraws{};
// HOLD release -> holster, always (InputBrain::serviceHolster): intents, re-posts, waits while holsters are gated, cancels by a
// re-squeeze, completions (hand no longer holds it), voids (gone before any post), open-hand holsters after a suspension.
inline std::atomic<std::uint64_t> holsterIntents{},holsterRetries{},holsterWaitFrames{},holsterCancelled{},holsterDone{},holsterVoid{},openHandHolsters{};
inline std::atomic<std::uint32_t> holsterAttemptsMax{};
inline constexpr std::uint64_t holsterRetryNs=1'200'000'000,openHandSettleNs=400'000'000;
// hold kept across an unreadable poll (gapRestores), a game draw adopted into the hand that held before (adoptPrevHand), free-hand
// presses at a held long gun masked (freeHandMasked), a hold whose press the hand no longer has (pressMismatch: open hand -> holster).
inline std::atomic<std::uint64_t> gapRestores{},adoptPrevHand{},freeHandMasked{},pressMismatch{};
inline constexpr std::uint64_t gapRestoreNs=3'000'000'000,recentHandNs=2'000'000'000;
// EVENTS: the last 16 holster events (any thread writes: slot index + relaxed atomics; an entry being rewritten may read torn, never a
// data race). reason = static text; aux = detail (GONE: aimBits | selectState << 8 | ours << 16).
enum HolsterEvent : std::uint32_t {EvGrab,EvDrawn,EvRelease,EvPutAway,EvRefuse,EvAdopt,EvSwap,EvGone,EvCount};
inline constexpr std::array<const char*,EvCount> eventNames{"GRAB","DRAWN","RELEASE","PUTAWAY","REFUSE","ADOPT","SWAP","GONE"};
struct EventSlot {std::atomic<std::uint64_t> ns{},packed{};std::atomic<const char*> reason{};};
inline constexpr std::uint32_t eventRingSize=16;
inline std::array<EventSlot,eventRingSize> eventRing{};inline std::atomic<std::uint32_t> eventCount{};
inline void noteEvent(HolsterEvent e,int slot,int hand,const char* reason,std::uint64_t now,std::uint32_t aux=0) noexcept {
    auto& s=eventRing[eventCount.fetch_add(1,std::memory_order_relaxed)%eventRingSize];
    s.packed.store(std::uint64_t(e)|(std::uint64_t(std::uint8_t(slot))<<8)|(std::uint64_t(std::uint8_t(hand))<<16)|(std::uint64_t(aux)<<32),std::memory_order_relaxed);
    s.reason.store(reason?reason:"",std::memory_order_relaxed);s.ns.store(now,std::memory_order_release);
    firediag::Event d{};d.ns=now;d.stage="HOLSTER";d.reason=eventNames[e];d.hint=reason?reason:"";
    d.slot=slot;d.hand=hand;firediag::record(d);
}

// ---- WEAPON KEEP: configuration (set once at startup by vr::weapon::startup) and live engine facts ----
enum class ForceAim : int {Held=0,Trigger=1,Off=2};
inline std::atomic<int> forceAimMode{int(ForceAim::Held)};
inline std::atomic<bool> keepDrawn{true};
// Published by the weapon observer (engine main thread, PostCameraUpdate) while a weapon is in the VR hand: true = a firearm the
// VR pipeline attaches (pistol/rifle/repeater/sniper/shotgun/bow). Force aim is applied only to these.
inline std::atomic<bool> heldFirearm{};
// Input brain: holding-hand trigger (Trigger mode) and the pad state the game received (engine put-away forensics).
inline std::atomic<float> holdTriggerValue{};inline std::atomic<std::uint64_t> holdTriggerNs{};
inline std::atomic<std::uint32_t> lastPadButtons{};inline std::atomic<float> lastPadRt{},lastPadLt{};
// Engine side (main thread tick): player+0x3bc / +0x3e0 / +0x504 / +0x244 of the last successful poll.
inline std::atomic<int> engineAimBits{-1},engineSelectState{-1},engineZoom{-1};inline std::atomic<float> engineHoldTimer{};
inline std::atomic<bool> keepApiReady{},forceAimApiReady{},forceAimOurs{};
inline std::atomic<std::uint64_t> keepCalls{},forceAimSet{},forceAimCleared{},forceAimForeign{},forceAimLostExternally{},
    engineHolsters{},engineHolstersTimer{},engineHolstersInput{},engineHolstersOther{},releaseDeferredFrames{},releaseDeferred{};
// The hold-mode keep also runs while a context gate (cover, linked animation, context unreadable, ...)
// blocks the holsters; otherwise the RDR1 hold-mode timer holsters the VR weapon.
// OTHER put-aways are split: FORCE_HOLSTER (script bits inv+0x48c) / GATED (a gate was closed).
inline std::atomic<std::uint64_t> keepWhileGated{},engineHolstersForced{},engineHolstersGated{},menuResumes{};
struct EngineHolster {std::uint64_t ns{};std::uint32_t buttons{};float rt{},lt{},holdTrigger{},timer{};int aimBits{-1},state{-1},hand{-1};bool ours{},forced{};const char* kind{"NONE"};
    std::uint16_t invFlags{};const char* gate{"OK"};};
inline std::mutex engineHolsterMutex;inline EngineHolster lastEngineHolster{};

// ---- weapon: trigger policy while a weapon is in a VR hand ----
// heldKind = engine::WeaponClass of the VR-held weapon (published by the weapon observer, -1 = none / unknown).
inline constexpr int kindMelee=10;
inline std::atomic<int> heldKind{-1};
// ltNative: the non-holding trigger reaches the game as LT. Default false: while a weapon is in a VR hand
// the non-holding trigger never reaches the game (no aim / lock-on / knife attack from the free hand; force aim replaces LT).
// meleeTrigger: the holding trigger of a VR-held knife reaches the game as RT (attack). Default false.
inline std::atomic<bool> ltNative{},meleeTrigger{};
// Sinks registered by VrWeapon / VrReload (input thread calls; null = none).
using TriggerSink=void(*)(int hand,bool forwarded,const char* block,std::uint64_t now) noexcept; // holding-trigger press edge
using ButtonSink=bool(*)(const ControllerSample& in,int hold,std::uint64_t now) noexcept;          // true = mask B from the game
using BlockSink=bool(*)(std::uint64_t now) noexcept;                                                 // true = RT blocked (cylinder open)
inline std::atomic<TriggerSink> triggerSink{};inline std::atomic<ButtonSink> bButtonSink{};inline std::atomic<BlockSink> fireBlockSink{};
// ---- RELOAD: sink slots only - VrReload.h registers them,
// this header never includes it (VrReload.h includes this one). Input thread unless noted. ----
// reloadBrainSink: once per normal input frame, before the grab loop (not in the suspended / head-only paths): the facts the reload brain
// needs from this brain (sockets_, captures, phantom / support hands, the resume flag).
struct ReloadFacts {
    const ControllerSample* in{};const Pose* head{};int hold{-1},phantom{-1},support{-1};
    std::array<std::array<float,3>,slotCount> sockets{};std::array<bool,2> captured{},latched{};
    std::uint64_t referenceSpace{},now{};bool resumed{},bodySockets{};
};
using ReloadBrainSink=void(*)(const ReloadFacts&) noexcept;
using HandOwnedSink=bool(*)(unsigned hand,std::uint64_t now) noexcept;   // the free hand belongs to the open cylinder (+ 300 ms after a close)
using HandGrabSink=void(*)(unsigned hand,std::uint64_t now) noexcept;    // a fresh squeeze of an owned free hand (captured, pressSequence 0)
using RtGateSink=int(*)(int source,int hold,std::uint64_t now) noexcept; // one call per physical RT edge -> reload::Verdict (0 Pass, 1 PassNative,
                                                                          // 2 BlockEmpty, 3 BlockSpent, 4 BlockOneShot)
using RtHeldSink=bool(*)(std::uint64_t now) noexcept;                    // false = the passed press already fired (ONE_SHOT for the rest of it)
using PhaseSink=int(*)(std::uint64_t now) noexcept;                       // cylinder phase: 0 closed, 1 opening, 2 open, 3 closing
inline std::atomic<ReloadBrainSink> reloadBrainSink{};inline std::atomic<HandOwnedSink> freeHandOwnedSink{};inline std::atomic<HandGrabSink> reloadGrabSink{};
inline std::atomic<RtGateSink> rtGateSink{};inline std::atomic<RtHeldSink> rtHeldSink{};inline std::atomic<PhaseSink> cylinderPhaseSink{};
// written by the reload: the last CLOSED stamp (GuardRecentClose, any thread), the PUSH visual following the hand, a long press of B that the
// game must see (routeB adds B while it is physically down: the press began masked).
inline std::atomic<std::uint64_t> cylinderClosedNs{};inline std::atomic<bool> cylinderFollow{},bForce{};
inline std::atomic<std::uint64_t> reloadCaptures{},reloadCapturePresses{},rtGateCalls{},rtDeferredEdges{},rtDeferredReplayed{},rtGateLatched{},recentCloseGuards{},
    bForcedFrames{};
inline constexpr std::uint64_t recentCloseNs=400'000'000,deferredEdgeNs=200'000'000;
// ---- HAMMER: sink slots + atomics only - VrHammer.h registers
// them (it includes VrReload.h -> this header). hammerSinks == nullptr (killed / not started) = route() and frame() without HAMMER. ----
// frameBegin: the FIRST line of InputBrain::frame on every path (Arm seqlock for the XR detector, the stroke queue drained, in-flight expiry);
// suspended = !HOLSTERS || !engine || !executor || menu || !usable || !pad.ready (strokes discarded, the XR detector disarmed).
// ownsRt: HAMMER owns the holding trigger (mode != OFF, the VR-held revolver with a hammer zone, P prints OK, the context allows it).
// step: inside route() after NOT_REARMED / MELEE / CYLINDER_OPEN (block = that reason: strokes dropped) -> the requests of this frame -> the
// reload gate / the empty rule -> tempo -> the RT pulse (out.rt / rtBit), the raw pass or an edge block (HAMMER_DOWN, EMPTY_CHAMBER..).
// marker: the ring above the spur (MarkerWeapon, palette 7; drawn whatever HANDS / HOLSTER MARKERS say). xrSample: the XR thread's detector.
struct MarkerSet; // below (markers for the HANDS module)
struct HammerFrame {std::uint64_t now{};const ControllerSample* in{};const Pose* head{};int hold{-1};bool suspended{},menu{};};
struct HammerIn  {std::uint64_t now{};int hold{-1};bool physEdge{},physDown{},offEmpty{};float trigger{};std::uint32_t generation{};const char* block{};};
struct HammerOut {float rt{};bool rtBit{},rawPass{};const char* edgeBlock{};};
struct HammerSinks {
    void (*frameBegin)(const HammerFrame&) noexcept;
    bool (*ownsRt)(int hold,std::uint64_t now) noexcept;
    void (*step)(const ControllerSample&,const HammerIn&,HammerOut&) noexcept;
    void (*marker)(MarkerSet&,const ControllerSample&,int hold,std::uint64_t now) noexcept;
    void (*xrSample)(const ControllerSample&,bool graphicsInput) noexcept; // XR thread (XrHarness::locateHands, inside inputReady)
};
inline std::atomic<const HammerSinks*> hammerSinks{};
// written by the XR detector (contact phase of the free hand at the spur, steady ns: the STRIKE only - the open palm moving back through the strike
// window, never mere presence) / by frameBegin (hold guard, FORCE AIM TRIGGER, SA cocked)
inline std::array<std::atomic<std::uint64_t>,2> hammerContactNs{};
inline std::atomic<std::uint64_t> hammerHoldGuardUntilNs{},hammerAimNs{},hammerTransferBlockNs{300'000'000};inline std::atomic<bool> hammerCocked{};
inline std::atomic<std::uint64_t> hammerReleaseDeferred{},hammerTransferSuppressed{},hammerRearmLatched{};
inline std::atomic<std::uint64_t> triggerEdges{},triggerForwarded{},ltBlockedFrames{},rtBlockedMelee{},rtBlockedCylinder{},rtBlockedRearm{},
    bMaskedFrames{};
// ---- triggers of EMPTY VR hands ----
// emptyTriggerNative (debug.rdrvr.vr.weapon.emptytrig native): the native pad passes (LT aim / RT fire draw a weapon).
// nativeFistTrigger (VrMelee tick, every frame): the engine's own trigger punch is the only fist path (MELEE NATIVE
// or MELEE FISTS OFF) -> RT passes as the punch (STRIKE) while VrMelee's packet hook drops its FIRE intention (no
// draw); LT is withheld either way. Default false = both triggers withheld (physical fists; also when the
// melee module is not running: never a weapon draw from a trigger).
inline std::atomic<bool> emptyTriggerNative{},nativeFistTrigger{};
inline std::atomic<std::uint64_t> emptyTriggerEdges{},emptyTriggerFrames{},emptyTriggerLatched{},emptyStrikePassed{},emptyTriggerPassedGated{};
inline constexpr std::uint32_t gameDrawSequenceFlag=0x80000000u;
inline std::atomic<bool> adoptGameDraws{true};                  // debug.rdrvr.vr.holsters.adopt 0: game draws are not adopted
inline std::atomic<std::uint64_t> gameDrawOpenNs{1'500'000'000}; // debug.rdrvr.vr.holsters.gamedrawopen <ms> (0 = only a squeeze + release)
inline std::atomic<std::uint64_t> gameDrawsAdopted{},adoptClosed{},adoptOpen{},adoptSqueezed{},adoptOpenHolsters{};

// ---- DUAL: hand-to-hand transfer, a second ("phantom") item in the other hand, swap-fire ----
// RDR1 holds exactly one item in hand (inv+0x80). Every slot item has its own live weapWeapon that
// the engine re-poses every frame through invWeapon::OnUpdatePosition -> weapWeapon::Update (ret
// 0x26770a4, the call the pose boundary already pins) - also while it is holstered
// (UpdateHolsteredItemPositions 0x266981c). The phantom = such a holstered item whose Update
// attachment is replaced by the second VR hand (GameXr vrPhantomPose); the engine never sees it in
// hand (no physics: SetDrawn(false) deleted its phInst). Sub-switches: .dual.transfer 0,
// .dual.swapfire 0, .dual.render 0, .dual.radius <m 0.06..0.25>.
inline constexpr float transferAhead=.06f;                       // item point = grip + aim forward * 6 cm
inline constexpr std::uint64_t swapSettleNs=150'000'000,swapTimeoutNs=800'000'000;
inline std::atomic<bool> dualKilled{},dualUnavailableFlag{},transferEnabled{true},swapFire{true},forceRender{true};
inline std::atomic<float> transferRadius{.12f};
inline std::atomic<const char*> dualState{"READY"};
inline bool dualActive() noexcept {
    return !dualKilled.load(std::memory_order_relaxed)&&!dualUnavailableFlag.load(std::memory_order_relaxed)&&config().dual;}
// The hand-to-hand transfer needs no engine proof (no engine call): only the kill switch, the menu row and .dual.transfer.
inline bool transferActive() noexcept {
    return !dualKilled.load(std::memory_order_relaxed)&&config().dual&&transferEnabled.load(std::memory_order_relaxed);}
// Published by the main thread (EngineSide); the input brain, the weapon pose hook, melee and the hands bridge read them.
inline std::atomic<int> phantomHand{-1},phantomSlot{-1};
inline std::atomic<std::uintptr_t> phantomItem{},phantomWeapon{};
inline std::atomic<std::uint32_t> phantomSequence{},phantomGeneration{},swapLanded{};
inline std::atomic<bool> phantomRenderForced{};inline std::atomic<std::uint64_t> phantomSinceNs{};
// Phantom candidates: the knife and the firearms (lasso / thrown items stay engine-only: their use needs the engine hand).
inline constexpr bool phantomFirearmSlot(int slot) noexcept {return slot==SlotSidearm||slot==SlotSniper||slot==SlotLongarm||slot==SlotShotgun;}
inline constexpr bool phantomableSlot(int slot) noexcept {return slot==SlotMelee||phantomFirearmSlot(slot);}
enum PhantomClear : unsigned {ClearRelease,ClearSocket,ClearPromote,ClearContext,ClearGameDraw,ClearSlotChanged,ClearInHand,ClearNoWeapon,
    ClearKill,ClearReplace,ClearCount};
inline constexpr std::array<const char*,ClearCount> phantomClearNames{"RELEASE","SOCKET","PROMOTE","CONTEXT","GAME_DRAW","SLOT_CHANGED",
    "IN_HAND","NO_WEAPON","KILL","REPLACE"};
enum DualRefusal : unsigned {DualNotInHand,DualNotPhantomable,DualHandBusy,DualNoWeapon,DualOff,DualRefusalCount};
inline constexpr std::array<const char*,DualRefusalCount> dualRefusalNames{"NOT_IN_HAND","NOT_PHANTOMABLE","HAND_BUSY","NO_WEAPON","DUAL_OFF"};
inline std::array<std::atomic<std::uint64_t>,ClearCount> phantomClears{};
inline std::array<std::atomic<std::uint64_t>,DualRefusalCount> dualRefusals{};
inline std::atomic<const char*> lastPhantomClear{"NONE"};
inline std::atomic<std::uint64_t> transfersRequested{},transfersDone{},transfersRefused{},phantomSetRequests{},phantomSets{},phantomMoves{},
    drawKeepRequests{},drawKeeps{},promoteRequests{},promotes{},swapsRequested{},swapsLanded{},swapsFiredAfter{},swapLatencyNsSum{},
    swapLatencyNsMax{},swapTimeouts{},swapBlocked{},swapsDone{},offhandTriggerEdges{},offhandDry{},renderForcedCalls{},renderRestored{},
    phantomResumes{},phantomTicks{};

namespace detail {
using V3=std::array<float,3>;using Q=std::array<float,4>;
inline constexpr float pi=3.14159265358979f;
inline V3 add(V3 a,V3 b){return {a[0]+b[0],a[1]+b[1],a[2]+b[2]};}
inline V3 sub(V3 a,V3 b){return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
inline V3 mul(V3 a,float s){return {a[0]*s,a[1]*s,a[2]*s};}
inline float dot(V3 a,V3 b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
inline V3 cross(V3 a,V3 b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
inline float length(V3 a){return std::sqrt(dot(a,a));}
inline float distance(V3 a,V3 b){return length(sub(a,b));}
inline V3 rotate(const Q& q,V3 v){ // q = (x,y,z,w)
    const V3 u{q[0],q[1],q[2]};const V3 t=mul(cross(u,v),2.f);return add(add(v,mul(t,q[3])),cross(u,t));}
inline Q product(const Q& a,const Q& b){return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
    a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};}
inline Q normalized(Q q){const float n=std::sqrt(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);if(!(n>0))return {0,0,0,1};for(float& v:q)v/=n;return q;}
inline float wrap(float a){a=std::fmod(a+pi,2*pi);if(a<0)a+=2*pi;return a-pi;}
// Rotation about +Y by yaw (positive = turning left, OpenXR: +X right, +Y up, -Z forward).
inline V3 yawRotate(float yaw,V3 v){const float c=std::cos(yaw),s=std::sin(yaw);return {v[0]*c+v[2]*s,v[1],-v[0]*s+v[2]*c};}
inline float yawOf(V3 forward){return std::atan2(-forward[0],-forward[2]);}
inline bool finite(V3 v){return std::isfinite(v[0])&&std::isfinite(v[1])&&std::isfinite(v[2]);}
inline bool poseOk(const Pose& p){
    if(!p.positionValid||!p.orientationValid||!finite(p.position))return false;
    float n=0;for(float x:p.orientation){if(!std::isfinite(x))return false;n+=x*x;}
    return std::fabs(n-1.f)<.01f&&std::fabs(p.position[0])<1000.f&&std::fabs(p.position[1])<1000.f&&std::fabs(p.position[2])<1000.f;
}
inline V3 forwardOf(const Pose& p){return rotate(p.orientation,{0,0,-1});}
inline std::uint32_t gripBit(unsigned hand){return hand?runtime::PadRightGrip:runtime::PadLeftGrip;}
// Shortest-arc rotation that turns 'from' towards 'to', limited to maxAngle. False when degenerate.
inline bool limitedArc(V3 from,V3 to,float maxAngle,Q& out){
    const float lf=length(from),lt=length(to);if(!(lf>1e-4f)||!(lt>1e-4f))return false;
    from=mul(from,1/lf);to=mul(to,1/lt);
    V3 axis=cross(from,to);const float s=length(axis),c=std::fmax(-1.f,std::fmin(1.f,dot(from,to)));
    if(s<1e-5f)return false; // parallel (nothing to do) or antiparallel (axis undefined)
    axis=mul(axis,1/s);const float angle=std::fmin(std::atan2(s,c),maxAngle);
    const float h=.5f*angle;out={axis[0]*std::sin(h),axis[1]*std::sin(h),axis[2]*std::sin(h),std::cos(h)};return true;
}
} // namespace detail

// ---- markers for the HANDS module (LOCAL space, render thread reads with a seqlock) ----
// flags bit0 MarkerWeapon (hammer ring), bits 8-15 palette style; carried to hands::Marker.
struct Marker {std::array<float,3> position{};float radius{};float highlight{};std::int32_t slot{};std::uint32_t kind{};std::uint32_t flags{};}; // kind 0 socket, 1 foregrip
struct MarkerSet {std::array<Marker,8> markers{};std::uint32_t count{};std::uint32_t pad{};std::uint64_t sampleNs{},referenceSpace{};}; // referenceSpace = ControllerSample::referenceSpace
template<class T> class Seqlock {
    static_assert(std::is_trivially_copyable_v<T>&&sizeof(T)%8==0);
    static constexpr std::size_t words=sizeof(T)/8;
    std::atomic<std::uint64_t> sequence_{0};std::array<std::atomic<std::uint64_t>,words> data_{};
public:
    void store(const T& value) noexcept { // single writer
        std::array<std::uint64_t,words> raw{};std::memcpy(raw.data(),&value,sizeof(T));
        const auto s=sequence_.load(std::memory_order_relaxed);sequence_.store(s+1,std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for(std::size_t i=0;i<words;++i)data_[i].store(raw[i],std::memory_order_relaxed);
        sequence_.store(s+2,std::memory_order_release);
    }
    bool load(T& out) const noexcept { // any reader, never blocks (bounded retries)
        for(int attempt=0;attempt<8;++attempt){
            const auto s0=sequence_.load(std::memory_order_acquire);if(s0&1u)continue;
            std::array<std::uint64_t,words> raw{};for(std::size_t i=0;i<words;++i)raw[i]=data_[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if(sequence_.load(std::memory_order_relaxed)==s0){if(!s0)return false;std::memcpy(&out,raw.data(),sizeof(T));return true;}
        }
        return false;
    }
};
inline Seqlock<MarkerSet> markerBox{};
// BODY: holster sockets where the weapons visibly hang on the shown body. VrBody hook A computes
// them from the posed skeleton (+ the holstered items' own world positions, `holsteredBox`) and maps them to OpenXR LOCAL with the
// same plan the arms use; the input brain prefers them over the fixed head-relative offsets while fresh and in the same space.
struct BodySockets {std::array<std::array<float,3>,7> position{};std::uint32_t mask{};std::uint32_t fromItem{};std::uint64_t ns{},referenceSpace{};};
inline Seqlock<BodySockets> bodySocketBox{};
inline constexpr std::uint64_t bodySocketMaxAgeNs=250'000'000;
inline constexpr float bodyGrabRadius=.24f,bodyLeaveRadius=.30f; // Generous: the visible holster is the target
// The BELT holsters (knife, revolver, lasso: the ones reached
// blind at the hip) get big zones that are taller than wide: distance = |(dx, dy * beltVertical, dz)| (OpenXR LOCAL, +Y up), so a hand
// anywhere near the hip at belt height on that side counts (0.32 m around, ~0.43 m up/down). Leave radius 0.40 (hysteresis). Among
// several zones the one with the smallest distance / radius wins. Fixed (head-relative) sockets: 0.28 / 0.35. Entering a zone pulses
// the hand (beltEnterAmplitude, a bit stronger than the other zones).
inline constexpr std::uint32_t beltZoneSlots=(1u<<0)|(1u<<1)|(1u<<3); // MELEE, SIDEARM, LASSO
inline constexpr float beltGrabRadius=.32f,beltLeaveRadius=.40f,beltFixedGrab=.28f,beltFixedLeave=.35f,beltVertical=.75f;
inline constexpr float beltEnterAmplitude=.40f;inline constexpr std::uint32_t beltEnterUs=25'000;
inline constexpr float socketGuardRadius=.26f; // THROW socket guard: released this close to its own holster = holster
// Holstered items' world positions (weapon+0x80 translation = the matrix the holstered weapWeapon::Update wrote last frame), main tick.
struct HolsteredWorld {std::array<std::array<float,3>,7> position{};std::uint32_t mask{};std::uint32_t pad{};std::uint64_t ns{};};
inline Seqlock<HolsteredWorld> holsteredBox{};
// RELOAD: the player / inventory facts of the last successful poll (main thread) for the reload executor (ops,
// the holster refill timer of the slot items, ammo reads) - no second player chain read.
struct InventoryLink {std::uint64_t ns{};std::uintptr_t player{},actor{},inventory{},handItem{};std::array<std::uintptr_t,slotCount> items{};std::uint32_t guid{},valid{};};
static_assert(sizeof(InventoryLink)%8==0);
inline Seqlock<InventoryLink> inventoryBox{};
inline std::atomic<std::uint64_t> bodySocketFrames{},bodySocketStale{},holsteredReads{},holsteredRejects{};
inline bool hammerActive() noexcept {return hammerSinks.load(std::memory_order_acquire)!=nullptr;}
// Optional push sinks (integration bridge to the HANDS module: hands::publishMarkers / hands::setHeldHand). Called on the input
// thread from padFrame: the marker sink every input frame, the held-hand sink when the physical weapon hand changes.
using MarkerSink=void(*)(const MarkerSet&) noexcept;
using HeldHandSink=void(*)(int hand) noexcept;
using HeldMaskSink=void(*)(unsigned mask) noexcept; // DUAL: bit h = hand h holds an item (engine item or phantom)
inline std::atomic<MarkerSink> markerSink{};
inline std::atomic<HeldHandSink> heldHandSink{};
inline std::atomic<HeldMaskSink> heldMaskSink{}; // Preferred over heldHandSink when set
inline bool markers(MarkerSet& out) noexcept {return markerBox.load(out);}
// HANDS finger pose: true while the hand holds a weapon (hold hand), the phantom or the foregrip (support hand).
inline bool handHolding(unsigned hand) noexcept {
    return hand<2&&(holdHand.load(std::memory_order_relaxed)==int(hand)||supportHand.load(std::memory_order_relaxed)==int(hand)||
        phantomHand.load(std::memory_order_relaxed)==int(hand));
}
// XR thread (ControllerActions theater chord): the holster brain owns a grip - a held weapon, a foregrip support or a
// hand over a live socket. Consuming (zeroing) the grips then would read as a release: HOLD put-away, support dropped.
inline bool gripsEngaged() noexcept {
    if(!config().enabled||!engineReady.load(std::memory_order_acquire))return false;
    if(holdHand.load(std::memory_order_relaxed)>=0||supportHand.load(std::memory_order_relaxed)>=0||phantomHand.load(std::memory_order_relaxed)>=0)return true;
    // FIST BLOCK: a boxing guard (both grips squeezed hard for seconds) must never toggle the manual theater
    if(guard::state.load(std::memory_order_relaxed)!=guard::StateIdle||guard::premaskAny.load(std::memory_order_relaxed))return true;
    return insidePublished[0].load(std::memory_order_relaxed)>=0||insidePublished[1].load(std::memory_order_relaxed)>=0;
}

// Small fixed text published by the main thread for the report (atomic words: torn text, never a data race).
template<std::size_t N> class AtomicText {
    static_assert(N%8==0);std::array<std::atomic<std::uint64_t>,N/8> words_{};
public:
    void store(const char* text) noexcept {std::array<char,N> raw{};for(std::size_t i=0;text&&i<N-1&&text[i];++i)raw[i]=text[i];
        for(std::size_t i=0;i<N/8;++i){std::uint64_t w{};std::memcpy(&w,raw.data()+i*8,8);words_[i].store(w,std::memory_order_relaxed);}}
    std::string load() const {std::array<char,N> raw{};for(std::size_t i=0;i<N/8;++i){const auto w=words_[i].load(std::memory_order_relaxed);std::memcpy(raw.data()+i*8,&w,8);}
        raw[N-1]=0;std::string out(raw.data());for(char& c:out)if(c=='"'||c=='\\'||static_cast<unsigned char>(c)<0x20||static_cast<unsigned char>(c)>0x7e)c='?';return out;}
};
inline std::array<AtomicText<32>,slotCount> slotNames{};
inline AtomicText<32> handName{};

// XR thread: call every synced frame with the VR menu state (graphicsMenu_.update result). While the menu absorbs input
// the controller sample is zeroed; holsters freeze instead of reading that as a grip release.
inline void noteMenu(bool open,std::uint64_t now) noexcept {if(open)menuSeenNs.store(now?now:nowNs(),std::memory_order_relaxed);}

// =====================================================================================================================
// Input brain (input thread only).
class InputBrain {
    std::array<bool,2> latched_{true,true},captured_{},preMasked_{};
    std::array<int,2> inside_{-1,-1};
    std::array<std::uint32_t,2> pressSequence_{};
    std::array<bool,2> logical_{};
    int support_{-1},lastHold_{-1};
    bool awaitFirstGrip_{},bodyValid_{},catching_{},rtArmed_{true},suspended_{true},menuSuspended_{};
    // RELOAD: the RT gate verdict latched for the physical press, the CLOSING edge replayed at CLOSED
    bool gateActive_{};int gateVerdict_{-1};const char* gateBlock_{};std::uint64_t pendingEdgeNs_{};
    float bodyYaw_{},headYaw_{};
    std::uint32_t lastGeneration_{};
    std::uint64_t lastNs_{},epoch_{},space_{},continuity_{};
    std::array<detail::V3,slotCount> sockets_{};
    std::array<bool,2> releaseHeld_{};std::array<std::uint64_t,2> releaseFreeNs_{}; // HOLD release deferral
    // true = keep the HOLD weapon this frame although its grip reads open: the holding trigger is squeezed (a trigger pull
    // relaxes the middle finger on Touch controllers), or it was and the grip has not stayed open for releaseSettleNs since.
    // A trigger can therefore never put the weapon away; only an open grip with a released trigger does (HOLD).
    bool deferHoldRelease(unsigned h,const ControllerSample& in,const Config& cfg,int hold,std::uint64_t now) noexcept {
        // LASSO: lasso::keepHold first - the rope's holding hand stays while a throw is launching / flying,
        // during a re-grip grace and until the CAUGHT release rule (open >= 150 ms, trigger released) is met. HOLD only (TOGGLE never holsters here).
        if(cfg.grip==GripMode::Hold&&hold==int(h)&&lasso::holstersActive()&&lasso::keepHold(h,in,now)){releaseDeferredFrames.fetch_add(1,std::memory_order_relaxed);return true;}
        const bool engineHold=hold==int(h)&&holdOurs.load(std::memory_order_relaxed)&&pressSequence_[h]==holdSequence.load(std::memory_order_relaxed);
        const bool phantomHold=phantomHand.load(std::memory_order_relaxed)==int(h)&&pressSequence_[h]==phantomSequence.load(std::memory_order_relaxed);
        if(cfg.grip!=GripMode::Hold||!captured_[h]||!pressSequence_[h]||!(engineHold||phantomHold)){releaseHeld_[h]=false;return false;}
        if(in.triggers[h]>releaseTriggerGuard){releaseHeld_[h]=true;releaseFreeNs_[h]=0;releaseDeferredFrames.fetch_add(1,std::memory_order_relaxed);return true;}
        // HAMMER: during a FAN series / 300 ms after the free palm touched the spur the
        // holding grip is treated as a squeezed trigger (a swipe shakes the holding hand; grip < .30 must not holster / throw); then the usual settle.
        if(engineHold&&now<hammerHoldGuardUntilNs.load(std::memory_order_relaxed)){releaseHeld_[h]=true;releaseFreeNs_[h]=0;
            releaseDeferredFrames.fetch_add(1,std::memory_order_relaxed);hammerReleaseDeferred.fetch_add(1,std::memory_order_relaxed);return true;}
        if(releaseHeld_[h]){
            if(!releaseFreeNs_[h])releaseFreeNs_[h]=now;
            if(now-releaseFreeNs_[h]<releaseSettleNs){releaseDeferredFrames.fetch_add(1,std::memory_order_relaxed);return true;}
            releaseHeld_[h]=false;releaseDeferred.fetch_add(1,std::memory_order_relaxed);
        }
        return false;
    }
    void gripClosed(unsigned h) noexcept {releaseHeld_[h]=false;releaseFreeNs_[h]=0;}
    void setSupport(int hand) noexcept {
        if(support_==hand)return;support_=hand;supportHand.store(hand,std::memory_order_relaxed);
        attachmentGeneration.fetch_add(1,std::memory_order_relaxed);if(hand>=0)supportEngaged.fetch_add(1,std::memory_order_relaxed);
    }
    // HOLD: after a menu, recenter, focus loss or blocked release the held weapon stays until the holding hand squeezes again
    // (an open grip read in those moments is not a release).
    void startAwait() noexcept {
        if(holdOurs.load(std::memory_order_relaxed)&&holdHand.load(std::memory_order_relaxed)>=0&&config().grip==GripMode::Hold){awaitFirstGrip_=true;pressSequence_={};}
    }
    int sunkHeld_{-2};HeldHandSink sunkSink_{};unsigned sunkMask_{~0u};HeldMaskSink sunkMaskSink_{};
    void publish(const MarkerSet& set) noexcept {
        markerBox.store(set);
        if(const auto sink=markerSink.load(std::memory_order_acquire)){try{sink(set);}catch(...){}}
    }
    void publishHeld() noexcept {
        const int held=holdHand.load(std::memory_order_relaxed),ph=phantomHand.load(std::memory_order_relaxed);
        const unsigned mask=(held==0||held==1?1u<<unsigned(held):0u)|(ph==0||ph==1?1u<<unsigned(ph):0u); // DUAL: phantom hand too
        const auto sink=heldHandSink.load(std::memory_order_acquire);const auto maskSink=heldMaskSink.load(std::memory_order_acquire);
        if(held==sunkHeld_&&sink==sunkSink_&&mask==sunkMask_&&maskSink==sunkMaskSink_)return;
        const bool heldChanged=held!=sunkHeld_||sink!=sunkSink_;sunkHeld_=held;sunkSink_=sink;sunkMask_=mask;sunkMaskSink_=maskSink;
        if(maskSink){try{maskSink(mask);}catch(...){}}
        else if(sink&&heldChanged){try{sink(held);}catch(...){}}
    }
    void publishEmpty(std::uint64_t now) noexcept {MarkerSet set{};set.sampleNs=now;publish(set);}
    void suspend(std::uint64_t now,bool keepCaptures,bool await=true) noexcept {
        latched_={true,true};inside_={-1,-1};preMasked_={};setSupport(-1);dropGuard(); // No guard across a suspension
        dwellSlot_={-1,-1};
        if(!keepCaptures)captured_={};
        if(await)startAwait();
        for(unsigned h=0;h<2;++h)insidePublished[h].store(-1,std::memory_order_relaxed);
        if(!suspended_)publishEmpty(now);suspended_=true;suspendedFrames.fetch_add(1,std::memory_order_relaxed);publishHeld();
    }
    bool post(std::uint16_t op,int slot,unsigned hand,std::int32_t reason,std::uint32_t maxAgeMs,std::uint64_t now,std::uint32_t& sequence,
              std::int32_t keep=-1) noexcept { // arg[3] = keep hand (OpDrawKeep) / the hand giving the item (OpTransfer, phantom move)
        mainthread::Command c{};c.module=mainthread::ModuleHolsters;c.op=op;c.arg[0]=slot;c.arg[1]=int(hand);c.arg[2]=reason;c.arg[3]=keep;c.maxAgeMs=maxAgeMs;
        sequence=mainthread::post(c,now);if(!sequence)postFailures.fetch_add(1,std::memory_order_relaxed);return sequence!=0;
    }
    void updateBody(const ControllerSample& in,const Pose& head,std::uint64_t now) noexcept {
        using namespace detail;
        const V3 f=forwardOf(head);
        if(f[0]*f[0]+f[2]*f[2]>.04f)headYaw_=yawOf(f);
        const float dt=lastNs_&&now>lastNs_?std::fmin(float(now-lastNs_)*1e-9f,.1f):0.f;
        if(!bodyValid_){bodyYaw_=headYaw_;bodyValid_=true;catching_=false;}
        else{
            float target=headYaw_;bool moving=false;
            const float x=in.sticks[0],y=in.sticks[1];
            if(x*x+y*y>stickDeadZone*stickDeadZone){const float a=std::atan2(x,y);if(std::fabs(a)<=moveYawWindow){target=wrap(headYaw_-a);moving=true;}}
            const float diff=wrap(target-bodyYaw_),step=yawCatchUp*dt;
            if(moving)catching_=true;
            else if(!catching_&&std::fabs(diff)>yawDeadZone)catching_=true;
            if(catching_){
                bodyYaw_=wrap(bodyYaw_+std::fmax(-step,std::fmin(step,diff)));
                if(!moving&&std::fabs(wrap(target-bodyYaw_))<yawSettle)catching_=false;
            }
        }
        bodyYawPublished.store(bodyYaw_,std::memory_order_relaxed);
        const V3 neck=add(V3{head.position[0],head.position[1],head.position[2]},rotate(head.orientation,{0,-neckDown,neckBack}));
        neck_=neck; // BACK: the slot choice around the neck pivot
        for(int k=0;k<slotCount;++k){const auto& s=slotSpecs[std::size_t(k)];
            sockets_[std::size_t(k)]=add(neck,yawRotate(bodyYaw_,{s.right,s.up+neckDown,-(s.forward+neckBack)}));}
        // BODY: the sockets of the shown body (VrBody hook A, LOCAL) replace the fixed offsets while fresh and in this space.
        bodySocketsUsed_=false;BodySockets b{};
        if(bodySocketBox.load(b)&&b.mask){
            const bool fresh=b.referenceSpace==in.referenceSpace&&(b.ns>=now?b.ns-now<bodySocketMaxAgeNs:now-b.ns<bodySocketMaxAgeNs);
            if(!fresh)bodySocketStale.fetch_add(1,std::memory_order_relaxed);
            else{for(int k=0;k<slotCount;++k)if(b.mask>>unsigned(k)&1u){const auto& p=b.position[std::size_t(k)];
                    if(finite({p[0],p[1],p[2]}))sockets_[std::size_t(k)]={p[0],p[1],p[2]};}
                bodySocketsUsed_=true;bodySocketFrames.fetch_add(1,std::memory_order_relaxed);}
        }
    }
    bool bodySocketsUsed_{};
    float enterRadius() const noexcept {return bodySocketsUsed_?bodyGrabRadius:grabRadius;}
    float exitRadius() const noexcept {return bodySocketsUsed_?bodyLeaveRadius:leaveRadius;}
    // Per-slot zones (belt holsters bigger and taller, see beltZoneSlots)
    static bool beltZone(int k) noexcept {return k>=0&&k<slotCount&&(beltZoneSlots>>unsigned(k)&1u);}
    float enterRadiusOf(int k) const noexcept {return beltZone(k)?(bodySocketsUsed_?beltGrabRadius:beltFixedGrab):enterRadius();}
    float exitRadiusOf(int k) const noexcept {return beltZone(k)?(bodySocketsUsed_?beltLeaveRadius:beltFixedLeave):exitRadius();}
    float zoneDistance(int k,const detail::V3& hand) const noexcept {
        detail::V3 d=detail::sub(hand,sockets_[std::size_t(k)]);if(beltZone(k))d[1]*=beltVertical;return detail::length(d);}
    // BACK: the three back sockets are one
    // region (any back zone, or behind the neck pivot above the shoulder blades - a reach that misses the small zones must not leak LB / RB);
    // the zone comes from the hand around the neck pivot in the body-yaw frame: centred (|x| <= 8 cm), not below the pivot, not in front =
    // SNIPER, else the shoulder side (right LONGARM, left SHOTGUN). -1 = not at the back. Hysteresis on the previous zone.
    static bool backSlot(int k) noexcept {return k==SlotSniper||k==SlotLongarm||k==SlotShotgun;}
    std::array<int,2> backZone_{{-1,-1}};detail::V3 neck_{};
    int backZoneOf(unsigned h,const detail::V3& p) noexcept {
        using namespace detail;
        const int was=backZone_[h];bool region=false;
        for(int k=SlotSniper;k<=SlotShotgun&&!region;++k)region=zoneDistance(k,p)<=(was>=0?exitRadiusOf(k):enterRadiusOf(k));
        const V3 r=sub(p,neck_);const float x=dot(r,yawRotate(bodyYaw_,{1,0,0})),f=dot(r,yawRotate(bodyYaw_,{0,0,-1})),y=r[1];
        if(!region)region=f<=(was>=0?0.f:-.03f)&&y>=(was>=0?-.24f:-.20f)&&y<=.45f&&std::fabs(x)<=.40f;
        int z=-1;
        if(region){const bool s=was==SlotSniper;
            if(std::fabs(x)<=(s?.10f:.08f)&&y>=(s?-.04f:0.f)&&f<=(s?.05f:.02f))z=SlotSniper;
            else if(std::fabs(x)<.03f&&(was==SlotLongarm||was==SlotShotgun))z=was; // no left / right flicker at the spine
            else z=x>0.f?SlotLongarm:SlotShotgun;}
        backZone_[h]=z;return z;
    }
    // Grip of hand h opened (<= 0.30): end its capture; HOLD: the grip that drew the held weapon holsters it.
    void release(unsigned h,const ControllerSample& in,const Config& cfg,int hold,int current,bool allowed,std::uint64_t now) noexcept {
        // SPIN: a hand the spin keeps (ARMING.. CATCHING) never releases its engine item here (its exits post their own command)
        if(hold==int(h)&&spin::keeps(int(h))){spin::bump(spin::stats.supHoldRelease);return;}
        latched_[h]=false;preMasked_[h]=false;dwellSlot_[h]=-1; // LASSO: an open grip ends a pending dwell
        if(captured_[h]){
            captured_[h]=false;
            const int ph=phantomHand.load(std::memory_order_relaxed),ps=phantomSlot.load(std::memory_order_relaxed);
            if(cfg.grip==GripMode::Hold&&pressSequence_[h]&&ph==int(h)&&pressSequence_[h]==phantomSequence.load(std::memory_order_relaxed)){
                // DUAL: the phantom hand opened -> the phantom goes back to its holster (no engine call)
                // THROW: ... unless the hand moved fast: the phantom flies (EngineSide clears it itself when the throw is refused)
                std::uint32_t seq{};noteEvent(EvRelease,ps,int(h),"PHANTOM_OPEN",now);
                if(tryThrow(h,in,cfg,allowed,true,ps,now))holdReleases.fetch_add(1,std::memory_order_relaxed);
                else if(post(OpPhantomClear,ps,h,ClearRelease,1000,now,seq))holdReleases.fetch_add(1,std::memory_order_relaxed);
            }else if(cfg.grip==GripMode::Hold&&pressSequence_[h]&&pressSequence_[h]==holdSequence.load(std::memory_order_relaxed)&&
               holdOurs.load(std::memory_order_relaxed)&&hold==int(h)){
                // The release is a standing intent, serviced every frame until the weapon is gone (serviceHolster): a refused
                // context (Dead Eye, cover, ...) waits for it to end, a failed post / an engine refusal is retried, a re-squeeze cancels.
                // THROW: a fast release posts OpThrow first; the holster intent waits for its acknowledgement (refused / lost ->
                // the plain put-away, so the gun never stays in an open hand).
                const auto seq=tryThrow(h,in,cfg,allowed,false,current,now);
                noteEvent(EvRelease,current,int(h),seq?"THROW":"GRIP_OPEN",now);
                beginHolster(h,now);
                if(seq){throwPending_[h]=seq;throwPostNs_[h]=now;holdReleases.fetch_add(1,std::memory_order_relaxed);}
                else serviceHolster(h,0.f,cfg,hold,current,allowed,now);
            }
            pressSequence_[h]=0;
        }
        if(support_==int(h))setSupport(-1);
        (void)current;(void)allowed;
    }
    // ---- THROW (VrThrow.h): release velocity per hand, holding-trigger press edges, the OpThrow acknowledgement, catch zones ----
    std::array<throwing::ReleaseEstimator,2> est_{};
    std::array<bool,2> trigDown_{},catchInside_{},magnetNear_{};std::array<std::uint64_t,2> trigEdgeNs_{},throwPostNs_{};
    std::array<std::uint32_t,2> throwPending_{};std::uint64_t releaseSeq_{};
    void noteThrowInput(const ControllerSample& in,std::uint64_t now) noexcept {
        for(unsigned h=0;h<2;++h){est_[h].note(in,h);
            const bool down=in.triggers[h]>=(trigDown_[h]?.45f:.55f);if(down&&!trigDown_[h])trigEdgeNs_[h]=now;trigDown_[h]=down;}
    }
    // A HOLD release of the engine item (phantom: the phantom) while the hand moves fast -> OpThrow. 0 = no throw (a normal holster).
    std::uint32_t tryThrow(unsigned h,const ControllerSample& in,const Config& cfg,bool allowed,bool phantom,int slotHint,std::uint64_t now) noexcept {
        namespace T=throwing;
        // The XR-rate tape (2-3x the game frame rate) when fresh, else this brain's game-rate history
        T::ReleaseEstimator xr{};const bool tape=T::fromTape(h,in.referenceSpace,now,xr,T::config.tapeMaxAgeNs);
        T::bump(tape?T::tapeUsed:T::tapeFallback);
        const auto e=(tape?xr:est_[h]).estimate(T::config.windowNs,std::fmax(.8f*T::config.minSpeed,.5f),T::config.releaseWindowNs);
        if(e.glitch){if(T::active())T::guard(T::GuardGlitch);return 0;}
        if(!e.valid||e.speedPeak<T::config.minSpeed||e.speedRelease<T::config.releaseSpeed){if(T::active())T::guard(T::GuardSlow);return 0;}
        // Opened inside its own holster zone = putting it away (never a throw); a downward-dominant release is a drop / a
        // holstering swing, never a toss (the recoil of an upward toss is redirected by the estimator first)
        // The socket guard keeps the zone size (0.26 m sphere): the bigger belt GRAB zones (0.32 / 0.40, taller) must not
        // turn a toss that starts at the hip into a holster
        if(slotHint>=0&&slotHint<slotCount&&inside_[h]==slotHint&&detail::poseOk(in.grip[h])&&
           detail::distance({in.grip[h].position[0],in.grip[h].position[1],in.grip[h].position[2]},sockets_[std::size_t(slotHint)])<=socketGuardRadius){
            if(T::active())T::guard(T::GuardSocket);return 0;}
        if(e.v[1]<-T::config.downDot*throwing::math::length(e.v)){if(T::active())T::guard(T::GuardDown);return 0;}
        if(!T::active()){T::guard(T::GuardMenu);return 0;}
        if(cfg.grip!=GripMode::Hold){T::guard(T::GuardNotHold);return 0;}
        if(!allowed){T::guard(T::GuardNotAllowed);return 0;}
        if(T::busy()){T::guard(T::GuardBusy);return 0;}
        if(phantom?!T::slotThrowable(slotHint):!T::kindThrowable(heldKind.load(std::memory_order_relaxed))){T::guard(T::GuardNotThrowable);return 0;}
        if(in.triggers[h]>releaseTriggerGuard||(trigEdgeNs_[h]&&now>=trigEdgeNs_[h]&&now-trigEdgeNs_[h]<200'000'000ull)){T::guard(T::GuardTrigger);return 0;}
        const auto shot=T::lastShotNs.load(std::memory_order_relaxed);
        if(shot&&now>=shot&&now-shot<250'000'000ull){T::guard(T::GuardRecentShot);return 0;}
        if(!phantom){bool cylinder=false;if(const auto s=fireBlockSink.load(std::memory_order_acquire)){try{cylinder=s(now);}catch(...){}}
            if(cylinder){T::guard(T::GuardCylinder);return 0;}}
        { // RELOAD: a release < 400 ms after any cylinder close, or while the PUSH
          // visual follows the hand, is a holster - never a toss (wave-close + let-go)
            const auto cc=cylinderClosedNs.load(std::memory_order_acquire);
            if((cc&&now>=cc&&now-cc<recentCloseNs)||cylinderFollow.load(std::memory_order_relaxed)){T::guard(T::GuardRecentClose);
                recentCloseGuards.fetch_add(1,std::memory_order_relaxed);return 0;}}
        T::Release r{};r.seq=++releaseSeq_;r.releaseNs=e.releaseNs;r.peakNs=e.peakNs;r.referenceSpace=in.referenceSpace;r.v=e.v;r.w=e.w;
        r.speedPeak=e.speedPeak;r.speedRelease=e.speedRelease;r.samples=e.samples;r.hand=int(h);
        r.flags=(tape?T::releaseFromTape:0u)|(e.redirected?T::releaseRedirected:0u);r.downPeak=e.downPeak;if(e.redirected)T::bump(T::redirects);
        T::releaseBox.store(r);
        std::uint32_t seq{};
        if(!post(OpThrow,slotHint,h,std::int32_t(r.seq&0x7fffffffu),300,now,seq,phantom?1:0))return 0;
        T::bump(T::throwsRequested);return seq;
    }
    void postCatch(unsigned h,const throwing::Target& tg,bool closed,const Config& cfg,std::uint64_t now) noexcept {
        captured_[h]=true;latched_[h]=true;preMasked_[h]=false;catchInside_[h]=false;std::uint32_t seq{};
        const std::int32_t kind=closed?1:(tg.state==std::uint32_t(throwing::StateResting)?2:0);
        if(post(OpCatch,tg.slot,h,kind,500,now,seq,std::int32_t(tg.generation&0x7fffffffu))){
            pressSequence_[h]=cfg.grip==GripMode::Hold?seq:0;throwing::bump(throwing::catchesPosted);}
    }
    // ---- SPIN: the revolver hangs / spins on the trigger finger ----
    spin::Brain spin_{};
    // The reload's cylinder state for the spin (entry refused / toss refused): OPENING / OPEN /
    // CLOSING (phase sink, else the fireBlockSink) or the PUSH visual still following the hand.
    static bool spinCylinderBlocked(std::uint64_t now) noexcept {
        int ph=-1;if(const auto ps=cylinderPhaseSink.load(std::memory_order_acquire)){try{ph=ps(now);}catch(...){ph=-1;}}
        if(ph>0)return true;
        if(ph<0){bool c=false;if(const auto s=fireBlockSink.load(std::memory_order_acquire)){try{c=s(now);}catch(...){}}if(c)return true;}
        return cylinderFollow.load(std::memory_order_relaxed);
    }
    // tryThrow's evaluation for the engine item (the same estimator, guards and order) without counters and without a post: the spin's
    // classic snapshot, its finger-off and tilt tosses.
    bool spinTossDecision(unsigned h,const ControllerSample& in,const Config& cfg,bool allowed,int slotHint,std::uint64_t now,throwing::Release& r) const noexcept {
        namespace T=throwing;
        T::ReleaseEstimator xr{};const bool tape=T::fromTape(h,in.referenceSpace,now,xr,T::config.tapeMaxAgeNs);
        const auto e=(tape?xr:est_[h]).estimate(T::config.windowNs,std::fmax(.8f*T::config.minSpeed,.5f),T::config.releaseWindowNs);
        if(e.glitch||!e.valid||e.speedPeak<T::config.minSpeed||e.speedRelease<T::config.releaseSpeed)return false;
        if(slotHint>=0&&slotHint<slotCount&&inside_[h]==slotHint&&detail::poseOk(in.grip[h])&&
           detail::distance({in.grip[h].position[0],in.grip[h].position[1],in.grip[h].position[2]},sockets_[std::size_t(slotHint)])<=socketGuardRadius)return false;
        if(e.v[1]<-T::config.downDot*throwing::math::length(e.v))return false;
        if(!T::active()||cfg.grip!=GripMode::Hold||!allowed||T::busy()||!T::kindThrowable(heldKind.load(std::memory_order_relaxed)))return false;
        if(in.triggers[h]>releaseTriggerGuard||(trigEdgeNs_[h]&&now>=trigEdgeNs_[h]&&now-trigEdgeNs_[h]<200'000'000ull))return false;
        const auto shot=T::lastShotNs.load(std::memory_order_relaxed);
        if(shot&&now>=shot&&now-shot<250'000'000ull)return false;
        if(spinCylinderBlocked(now))return false;
        {const auto cc=cylinderClosedNs.load(std::memory_order_acquire);if(cc&&now>=cc&&now-cc<recentCloseNs)return false;} // RELOAD guard (as tryThrow)
        r=T::Release{};r.releaseNs=e.releaseNs;r.peakNs=e.peakNs;r.referenceSpace=in.referenceSpace;r.v=e.v;r.w=e.w;
        r.speedPeak=e.speedPeak;r.speedRelease=e.speedRelease;r.samples=e.samples;r.hand=int(h);
        r.flags=(tape?T::releaseFromTape:0u)|(e.redirected?T::releaseRedirected:0u);r.downPeak=e.downPeak;
        return true;
    }
    struct SpinCtx {const InputBrain* self{};const ControllerSample* in{};Config cfg{};bool allowed{};int current{-1};std::uint64_t now{};};
    static bool spinTossThunk(void* c,unsigned h,throwing::Release& r) noexcept {
        const auto& x=*static_cast<const SpinCtx*>(c);return x.self->spinTossDecision(h,*x.in,x.cfg,x.allowed,x.current,x.now,r);}
    std::uint32_t spinPostThrow(unsigned h,throwing::Release r,int current,std::uint64_t now) noexcept {
        r.seq=++releaseSeq_;r.hand=int(h);throwing::releaseBox.store(r);std::uint32_t seq{};
        if(!post(OpThrow,current,h,std::int32_t(r.seq&0x7fffffffu),300,now,seq,0))return 0;
        throwing::bump(throwing::throwsRequested);return seq;
    }
    // Side-effect-free peek of deferHoldRelease for the engine item (incl. the FAN hold guard)
    bool spinDeferPeek(unsigned h,const ControllerSample& in,int hold,std::uint64_t now) const noexcept {
        if(in.triggers[h]>releaseTriggerGuard)return true;
        if(hold==int(h)&&now<hammerHoldGuardUntilNs.load(std::memory_order_relaxed))return true; // A FAN swipe shakes the holding hand
        if(releaseHeld_[h])return !releaseFreeNs_[h]||now-releaseFreeNs_[h]<releaseSettleNs;
        return false;
    }
    // One hand's spin step, before the release branch. True = the hand belongs to the spin this
    // frame (owned, or it just executed its exit): the caller skips release / socket / transfer /
    // catch for it.
    bool spinStep(unsigned h,const ControllerSample& in,const Config& cfg,int hold,int current,bool allowed,std::uint64_t now,const Pose* head,bool headOnly) noexcept {
        if(spin::killed.load(std::memory_order_relaxed)||!spin::started.load(std::memory_order_acquire))return false;
        spin::StepIn x{};x.now=now;x.h=h;x.in=&in;
        float hp[3]{};if(head){hp[0]=head->position[0];hp[1]=head->position[1];hp[2]=head->position[2];x.head=hp;}
        x.noEntry=headOnly;x.holdMode=cfg.grip==GripMode::Hold;
        x.holdIsH=hold==int(h)&&holdOurs.load(std::memory_order_relaxed);
        x.ownsHold=x.holdIsH&&pressSequence_[h]&&pressSequence_[h]==holdSequence.load(std::memory_order_relaxed);
        x.heldKind=heldKind.load(std::memory_order_relaxed);x.holdGeneration=holdGeneration.load(std::memory_order_relaxed);x.allowed=allowed;
        x.cylinderBlocked=spinCylinderBlocked(now); // The reload's open cylinder refuses the entry (RefCylinder)
        // Also blocked < 400 ms after a cylinder close
        {const auto cc=cylinderClosedNs.load(std::memory_order_acquire);if(cc&&now>=cc&&now-cc<recentCloseNs)x.cylinderBlocked=true;}
        x.lastShotNs=throwing::lastShotNs.load(std::memory_order_relaxed);
        x.deferWouldHold=spinDeferPeek(h,in,hold,now);
        // The spin's zone is the CORE of the own holster zone (the BODY belt zone is .32 / .40 m - a hip-height spin read as
        // "holster intent"): inside_ == current AND the grip within spin::config.zoneRadius of the socket
        x.insideOwnZone=!headOnly&&current>=0&&current<slotCount&&inside_[h]==current&&detail::poseOk(in.grip[h])&&
            detail::distance({in.grip[h].position[0],in.grip[h].position[1],in.grip[h].position[2]},sockets_[std::size_t(current)])<=spin::config.zoneRadius;
        if(!headOnly&&bodyValid_&&current>=0&&current<slotCount&&detail::finite(sockets_[std::size_t(current)])){ // The own socket (NEAR_HOLSTER)
            const auto& k=sockets_[std::size_t(current)];x.socketValid=true;x.socket={k[0],k[1],k[2]};}
        { // The other hand free (its touch bit may count), the sidearm slot (every pistol-class sidearm spins), the held name (forensics)
            const unsigned o=h^1u;
            x.otherHandFree=hold!=int(o)&&phantomHand.load(std::memory_order_relaxed)!=int(o)&&support_!=int(o)&&!(captured_[o]&&pressSequence_[o]);
            x.heldSlot=current;x.sidearmSlot=current==SlotSidearm;
        }
        char heldName[40]{}; // only on an opening (the entry runs there): no string per frame
        if(in.grips[h]<=gripOpen){try{std::snprintf(heldName,sizeof heldName,"%s",handName.load().c_str());}catch(...){heldName[0]=0;}}
        x.heldName=heldName;
        SpinCtx ctx{this,&in,cfg,allowed,current,now};x.toss=&spinTossThunk;x.ctx=&ctx;
        const auto o=spin_.step(x);
        if(o.regrab){gripClosed(h);if(!o.rtArmed)rtArmed_=false;} // the catch keeps the HOLD press; a brake squeeze through it must re-arm
        if(o.post!=spin::PostNone){ // the gun leaves the finger: throw (OpThrow + holster fallback) or the holster intent, as release() does
            latched_[h]=false;preMasked_[h]=false;dwellSlot_[h]=-1;captured_[h]=false;
            if(hold==int(h)&&holdOurs.load(std::memory_order_relaxed)&&cfg.grip==GripMode::Hold){
                const std::uint32_t seq=o.post==spin::PostThrow&&allowed?spinPostThrow(h,o.release,current,now):0u;
                beginHolster(h,now);
                if(seq){throwPending_[h]=seq;throwPostNs_[h]=now;holdReleases.fetch_add(1,std::memory_order_relaxed);}
                else serviceHolster(h,0.f,cfg,hold,current,allowed,now);
            }
            pressSequence_[h]=0;return true;
        }
        if(o.owned)preMasked_[h]=false;
        return o.owned;
    }
    std::array<bool,2> wantHolster_{};std::array<std::uint64_t,2> holsterNextNs_{},openSinceNs_{};std::array<std::uint32_t,2> holsterAttempts_{};
    void beginHolster(unsigned h,std::uint64_t now) noexcept {
        if(wantHolster_[h])return;
        wantHolster_[h]=true;holsterNextNs_[h]=now;holsterAttempts_[h]=0;holsterIntents.fetch_add(1,std::memory_order_relaxed);
    }
    // Every frame (and from release()): the engine item still in hand h after its HOLD grip opened -> post the put-away (first: the DUAL
    // promote of the other hand's phantom), again every holsterRetryNs while it stays; done when the hand no longer holds it.
    void serviceHolster(unsigned h,float grip,const Config& cfg,int hold,int current,bool allowed,std::uint64_t now) noexcept {
        if(!wantHolster_[h])return;
        // SPIN: the standing put-away intent (+ its retries) ends while the spin keeps this hand's gun on the finger
        if(spin::keeps(int(h))&&!throwPending_[h]){wantHolster_[h]=false;holsterAttempts_[h]=0;spin::bump(spin::stats.supIntent);return;}
        bool waitThrow=false;
        if(throwPending_[h]){ // THROW: OpThrow in flight - started = the hand is empty (done), refused / no answer = holster now
            const auto a=throwing::ack.load(std::memory_order_acquire);
            if(std::uint32_t(a>>1)==throwPending_[h]){
                throwPending_[h]=0;
                if(a&1u){wantHolster_[h]=false;holsterDone.fetch_add(1,std::memory_order_relaxed);return;}
                throwing::bump(throwing::holsterFallbacks);holsterNextNs_[h]=now;
            }else if(now<throwPostNs_[h]||now-throwPostNs_[h]>throwing::config.ackTimeoutNs){
                throwPending_[h]=0;throwing::bump(throwing::ackTimeouts);throwing::bump(throwing::holsterFallbacks);holsterNextNs_[h]=now;
            }else waitThrow=true;
        }
        if(cfg.grip!=GripMode::Hold||hold!=int(h)||!holdOurs.load(std::memory_order_relaxed)){
            wantHolster_[h]=false;throwPending_[h]=0;if(holsterAttempts_[h])holsterDone.fetch_add(1,std::memory_order_relaxed);else holsterVoid.fetch_add(1,std::memory_order_relaxed);return;}
        if(grip>=gripClose){ // squeezed again before the weapon left: keep it, this press is the hold press again
            wantHolster_[h]=false;throwPending_[h]=0;captured_[h]=true;latched_[h]=true;preMasked_[h]=false;pressSequence_[h]=holdSequence.load(std::memory_order_relaxed);
            if(hold==int(h))awaitFirstGrip_=false;
            noteEvent(EvAdopt,current,int(h),"RESQUEEZE",now);
            holsterCancelled.fetch_add(1,std::memory_order_relaxed);return;}
        if(waitThrow)return;
        if(!allowed){holsterWaitFrames.fetch_add(1,std::memory_order_relaxed);return;}
        if(now<holsterNextNs_[h])return;
        const int ph=phantomHand.load(std::memory_order_relaxed),ps=phantomSlot.load(std::memory_order_relaxed);
        std::uint32_t seq{};bool posted=false;
        if(!holsterAttempts_[h]&&ph>=0&&ph!=int(h)&&ps>=0&&dualActive()){ // DUAL: engine hand opened, the other hand keeps its phantom -> promote it
            posted=post(OpDrawKeep,ps,unsigned(ph),2,1000,now,seq,-1);if(posted)promoteRequests.fetch_add(1,std::memory_order_relaxed);
        }else posted=post(OpPutAway,current,h,PutAwayHoldRelease,3000,now,seq);
        if(!posted){holsterNextNs_[h]=now+50'000'000ull;return;}
        (holsterAttempts_[h]?holsterRetries:holdReleases).fetch_add(1,std::memory_order_relaxed);
        ++holsterAttempts_[h];holsterNextNs_[h]=now+holsterRetryNs;
        std::uint32_t m=holsterAttemptsMax.load(std::memory_order_relaxed);while(holsterAttempts_[h]>m&&!holsterAttemptsMax.compare_exchange_weak(m,holsterAttempts_[h])){}
    }
    // After a suspension (menu, stale input, recenter, ...) the hold press is unknown (awaitFirstGrip_).
    // A holding grip that reads OPEN (trigger released) on usable frames for openHandSettleNs was let go
    // meanwhile: holster instead of waiting for a squeeze.
    void openHandCheck(unsigned h,const ControllerSample& in,const Config& cfg,int hold,std::uint64_t now) noexcept {
        if(spin::keeps(int(h))){if(openSinceNs_[h])spin::bump(spin::stats.supOpenHand);openSinceNs_[h]=0;return;} // SPIN: no open-hand holster while kept
        if(!awaitFirstGrip_||wantHolster_[h]||cfg.grip!=GripMode::Hold||hold!=int(h)||!holdOurs.load(std::memory_order_relaxed)||
           in.grips[h]>gripOpen||in.triggers[h]>releaseTriggerGuard){openSinceNs_[h]=0;return;}
        if(lasso::holstersActive()&&lasso::keepHold(h,in,now)){openSinceNs_[h]=0;return;} // LASSO: the rope keeps the hand (grace / caught)
        // A weapon the game drew into an open hand gets gameDrawOpenNs (0 = never: only the squeeze + release puts it away)
        const std::uint64_t settle=gameDrawAwait_?gameDrawOpenNs.load(std::memory_order_relaxed):openHandSettleNs;
        if(gameDrawAwait_&&!settle){openSinceNs_[h]=0;return;}
        if(!openSinceNs_[h]){openSinceNs_[h]=now;return;}
        if(now-openSinceNs_[h]<settle)return;
        noteEvent(EvRelease,currentSlot.load(std::memory_order_relaxed),int(h),gameDrawAwait_?"GAME_DRAW_OPEN":"OPEN_HAND",now);
        openSinceNs_[h]=0;awaitFirstGrip_=false;openHandHolsters.fetch_add(1,std::memory_order_relaxed);
        if(gameDrawAwait_){gameDrawAwait_=false;adoptOpenHolsters.fetch_add(1,std::memory_order_relaxed);}
        beginHolster(h,now);
    }
    std::uint32_t adoptGeneration_{};bool gameDrawAwait_{};
    void adoptGameDraw(const ControllerSample& in,const Config& cfg,int hold,std::uint64_t now) noexcept {
        const auto gen=holdGeneration.load(std::memory_order_relaxed);
        if(gen==adoptGeneration_)return;
        adoptGeneration_=gen;gameDrawAwait_=false;
        const auto seq=holdSequence.load(std::memory_order_relaxed);
        // a hold of ours whose press this hand no longer has (opened before the draw landed, restored after a poll gap, ...): the next
        // squeeze adopts it, an open hand holsters it after openHandSettleNs - never a gun left in an open hand
        if(hold>=0&&hold<=1&&seq&&!(seq&gameDrawSequenceFlag)&&holdOurs.load(std::memory_order_relaxed)&&cfg.grip==GripMode::Hold&&
           pressSequence_[unsigned(hold)]!=seq&&!wantHolster_[unsigned(hold)]&&!spin::keeps(hold)){
            awaitFirstGrip_=true;openSinceNs_[unsigned(hold)]=0;pressMismatch.fetch_add(1,std::memory_order_relaxed);
            noteEvent(EvAdopt,currentSlot.load(std::memory_order_relaxed),hold,"PRESS_MISMATCH",now);return;}
        if(hold<0||hold>1||!(seq&gameDrawSequenceFlag)||!holdOurs.load(std::memory_order_relaxed)||cfg.grip!=GripMode::Hold)return;
        const unsigned h=unsigned(hold);
        if(in.grips[h]>=gripClose&&detail::poseOk(in.grip[h])){
            captured_[h]=true;latched_[h]=true;preMasked_[h]=false;pressSequence_[h]=seq;awaitFirstGrip_=false;
            adoptClosed.fetch_add(1,std::memory_order_relaxed);noteEvent(EvAdopt,currentSlot.load(std::memory_order_relaxed),hold,"GAME_DRAW_CLOSED",now);
        }else{
            pressSequence_[h]=0;awaitFirstGrip_=true;gameDrawAwait_=true;openSinceNs_[h]=0;adoptOpen.fetch_add(1,std::memory_order_relaxed);
            noteEvent(EvAdopt,currentSlot.load(std::memory_order_relaxed),hold,"GAME_DRAW_OPEN_WAIT",now);
        }
    }
    // ---- EMPTY VR hands: LT / RT never reach the game while the holsters can provide a weapon ----
    // (hold < 0: no engine item in a VR hand; a phantom alone does not fire either). Held presses stay
    // withheld until released (latch: no fresh native press when the gate changes under a held trigger).
    // nativeFistTrigger: RT passes as the engine's punch.
    std::array<bool,2> emptyLatch_{},emptyDown_{};
    void traceRoute(const ControllerSample& in,const PadState& pad,int hand,int hold,std::uint64_t now,
                    const char* reason,bool menu=false,const char* stage="ROUTE") const noexcept {
        firediag::Event d{};d.ns=d.attemptNs=now;d.reason=reason;d.stage=stage;d.hand=hand;d.heldHand=hold;
        d.slot=currentSlot.load();d.kind=heldKind.load();d.generation=int(holdGeneration.load());
        const auto s=settings();d.preset=int(weaponPresetOf(s));d.hammer=int(s.hammerStyle());d.reload=int(s.reloadMode());
        d.armed=rtArmed_?1:0;d.menu=menu?1:0;d.trigger=in.triggers[unsigned(hand)];d.grip=in.grips[unsigned(hand)];
        d.sentRt=pad.triggers[1];d.sentLt=pad.triggers[0];d.aimBits=engineAimBits.load();d.selectState=engineSelectState.load();
        d.forceAim=forceAimOurs.load()?1:0;const auto name=handName.load();std::snprintf(d.name.data(),d.name.size(),"%s",name.c_str());
        firediag::record(d);
    }
    void routeEmpty(const ControllerSample& in,PadState& pad,std::uint64_t now,bool menu) noexcept {
        const bool withhold=!emptyTriggerNative.load(std::memory_order_relaxed)&&contextAllowed.load(std::memory_order_relaxed);
        const bool strikeOnly=nativeFistTrigger.load(std::memory_order_relaxed);
        for(unsigned t=0;t<2;++t){ // t: 0 = LT (left controller), 1 = RT (right controller) - the native mapping of an empty pad
            const std::uint32_t bit=t?runtime::PadRightTrigger:runtime::PadLeftTrigger;
            const bool down=in.triggers[t]>=(emptyDown_[t]?.45f:.55f);const bool edge=down&&!emptyDown_[t];emptyDown_[t]=down;
            const bool padDown=(pad.buttons&bit)||pad.triggers[t]>triggerActive;
            bool cut=withhold&&!(t==1&&strikeOnly);
            if(emptyLatch_[t]){if(!padDown&&!down)emptyLatch_[t]=false;else cut=true;}
            if(cut){
                if(padDown||down){emptyLatch_[t]=true;emptyTriggerFrames.fetch_add(1,std::memory_order_relaxed);}
                if(edge)emptyTriggerEdges.fetch_add(1,std::memory_order_relaxed);
                pad.triggers[t]=0;pad.buttons&=~bit;
            }else if(edge){
                if(withhold)emptyStrikePassed.fetch_add(1,std::memory_order_relaxed);else emptyTriggerPassedGated.fetch_add(1,std::memory_order_relaxed);
            }
            if(edge)traceRoute(in,pad,int(t),-1,now,cut?"NO_VR_WEAPON":strikeOnly&&t==1?"NATIVE_PUNCH":"EMPTY_HAND_PASSTHROUGH",menu);
        }
        if(emptyLatch_[0]||emptyLatch_[1])emptyTriggerLatched.fetch_add(1,std::memory_order_relaxed);
    }
    // ---- FIST BLOCK (VrGuard.h): the guard pose from both empty hands; raised empty fists never press LB / RB ----
    guard::Detector guard_{};std::array<bool,2> guardMask_{};
    void updateGuard(const ControllerSample& in,const Pose& head,int hold,int ph,std::uint64_t now) noexcept {
        guardMask_={};
        const bool enabled=guard::active.load(std::memory_order_relaxed)&&!guard::killed.load(std::memory_order_relaxed)&&
            contextAllowed.load(std::memory_order_relaxed);
        std::array<guard::HandIn,2> hands{};
        for(unsigned h=0;h<2;++h){auto& x=hands[h];x.valid=detail::poseOk(in.grip[h])&&detail::poseOk(in.aim[h]);
            x.empty=hold!=int(h)&&ph!=int(h)&&support_!=int(h)&&!(captured_[h]&&pressSequence_[h]);
            if(!x.valid)continue;x.pos={in.grip[h].position[0],in.grip[h].position[1],in.grip[h].position[2]};
            x.aimForward=detail::forwardOf(in.aim[h]);x.grip=in.grips[h];}
        const auto o=guard_.update(enabled,{head.position[0],head.position[1],head.position[2]},headYaw_,hands,now);
        if(enabled){guardMask_=o.premask;guard::bump(guard::frames);
            if(o.premask[0]||o.premask[1])guard::bump(guard::premaskedFrames);}
        if(o.state==guard::StateArming)guard::bump(guard::armingFrames);
        if(o.state==guard::StateEngaged)guard::bump(guard::engagedFrames);
        if(o.engagedEdge){guard::bump(guard::engages);guard::engagedSinceNs.store(now,std::memory_order_relaxed);
            for(unsigned h=0;h<2;++h)haptics::pulse(h,.20f,20'000,now,haptics::SourceParry);}
        if(o.releasedEdge)guard::bump(guard::releases);
        guard::state.store(o.state,std::memory_order_release);guard::stampNs.store(now,std::memory_order_relaxed);
        guard::premaskAny.store(guardMask_[0]||guardMask_[1],std::memory_order_relaxed);
    }
    void dropGuard() noexcept {
        if(guard_.current()==guard::StateEngaged)guard::bump(guard::releases);
        guard_.reset();guardMask_={};guard::state.store(guard::StateIdle,std::memory_order_release);guard::premaskAny.store(false,std::memory_order_relaxed);
    }
    bool physDown_{}; // Holding trigger pressed (raw, .55/.45 hysteresis) for the press-edge telemetry
    // DUAL: phantom-hand trigger edges and swap-fire (the phantom firearm becomes the engine item, then this press fires).
    bool physDownPh_{},swapPress_{};std::uint32_t lastPhantomGeneration_{},swapSeq_{};int swapHand_{-1};std::uint64_t swapNs_{},swapLandNs_{};
    void routePhantom(const ControllerSample& in,int hold,std::uint64_t now) noexcept {
        const int ph=phantomHand.load(std::memory_order_acquire);const auto generation=phantomGeneration.load(std::memory_order_relaxed);
        if(ph<0||ph>1||ph==hold){physDownPh_=false;lastPhantomGeneration_=generation;return;}
        const unsigned o=unsigned(ph);
        if(generation!=lastPhantomGeneration_){lastPhantomGeneration_=generation;physDownPh_=in.triggers[o]>=.55f;return;} // carried-over squeeze
        const bool down=in.triggers[o]>=(physDownPh_?.45f:.55f);
        if(down&&!physDownPh_){
            offhandTriggerEdges.fetch_add(1,std::memory_order_relaxed);
            const int ps=phantomSlot.load(std::memory_order_relaxed);
            firediag::Event d{};d.ns=d.attemptNs=now;d.stage="OFFHAND_REQUEST";d.hand=ph;d.heldHand=hold;d.slot=ps;
            d.generation=int(generation);d.trigger=in.triggers[o];d.grip=in.grips[o];d.reason="NOT_FIREARM";
            const auto cfg=settings();d.preset=int(weaponPresetOf(cfg));d.hammer=int(cfg.hammerStyle());d.reload=int(cfg.reloadMode());
            if(phantomFirearmSlot(ps)){
                bool cylinder=false;if(const auto s=fireBlockSink.load(std::memory_order_acquire)){try{cylinder=s(now);}catch(...){}}
                if(hold>=0&&swapFire.load(std::memory_order_relaxed)&&dualActive()&&!swapSeq_&&!cylinder){
                    std::uint32_t seq{};
                    if(post(OpDrawKeep,ps,o,1,1000,now,seq,hold)){swapSeq_=seq;swapHand_=ph;swapNs_=now;swapLandNs_=0;swapsRequested.fetch_add(1,std::memory_order_relaxed);
                        d.reason="SWAP_FIRE_QUEUED";}else d.reason="COMMAND_QUEUE_REJECTED";
                }else{ // swap-fire off / blocked (cylinder open, swap in flight): a dry click
                    d.reason=cylinder?"CYLINDER_OPEN":hold<0?"NO_ENGINE_HAND":!swapFire.load()?"SWAP_FIRE_DISABLED":!dualActive()?"DUAL_DISABLED":"SWAP_PENDING";
                    if(cylinder)swapBlocked.fetch_add(1,std::memory_order_relaxed);
                    offhandDry.fetch_add(1,std::memory_order_relaxed);haptics::pulse(o,.15f,20'000,now,haptics::SourceRefuse);
                }
            } // knife phantom: the trigger does nothing (physical melee)
            firediag::record(d);
        }
        physDownPh_=down;
    }
    // DUAL: a landed swap arms RT for the press that asked for it (after force aim is back, or swapSettleNs).
    // Returns true on the frame it armed (route() then never lets the HAMMER ownership latch undo it); the
    // press counts in swapsFiredAfter only when route() really forwards its edge (swapPress_).
    bool settleSwap(const ControllerSample& in,int hold,std::uint64_t now) noexcept {
        if(!swapSeq_)return false;
        if(swapLanded.load(std::memory_order_acquire)==swapSeq_&&hold==swapHand_){
            if(!swapLandNs_)swapLandNs_=now;
            if(forceAimOurs.load(std::memory_order_relaxed)||now-swapLandNs_>=swapSettleNs){
                rtArmed_=true;physDown_=false;swapSeq_=0;swapLandNs_=0;
                swapPress_=hold>=0&&in.triggers[unsigned(hold)]>=.45f;
                return true;
            }
        }else if(now>swapNs_&&now-swapNs_>swapTimeoutNs){swapSeq_=0;swapLandNs_=0;swapTimeouts.fetch_add(1,std::memory_order_relaxed);}
        return false;
    }
    // Weapon: RT = the HOLDING hand's trigger (either hand); LT = nothing while a weapon is in a VR hand (ltNative: the
    // free trigger); RT withheld until re-armed after the grab, for a VR-held knife (meleeTrigger off) and
    // while the revolver cylinder is open. Nothing is routed while no weapon is in a VR hand (native pad).
    // LASSO: menu = a VR-menu frame (the latch never moves there); headFwd_ of the last usable frame (gaze aim option).
    bool headValid_{};std::array<float,3> headFwd_{};
    // HAMMER: the free hand o can fan / cock - no phantom, no foregrip support, not captured by a press of this brain,
    // not a raised FIST BLOCK fist, not the reload's (cylinder open).
    bool hammerOwned_{};
    bool hammerOffEmpty(unsigned o,std::uint64_t now) const noexcept {
        if(phantomHand.load(std::memory_order_relaxed)==int(o)||support_==int(o)||(captured_[o]&&pressSequence_[o])||guardMask_[o])return false;
        if(const auto own=freeHandOwnedSink.load(std::memory_order_acquire)){bool owned=false;try{owned=own(o,now);}catch(...){owned=false;}if(owned)return false;}
        return true;
    }
    void route(const ControllerSample& in,int hold,PadState& pad,std::uint64_t now,bool menu=false) noexcept {
        const auto generation=holdGeneration.load(std::memory_order_relaxed);
        const bool changed=generation!=lastGeneration_||hold!=lastHold_;
        if(changed){
            lastGeneration_=generation;if(hold!=lastHold_)attachmentGeneration.fetch_add(1,std::memory_order_relaxed);
            lastHold_=hold;rtArmed_=hold<0;logical_={};physDown_=hold>=0&&in.triggers[unsigned(hold)]>=.55f; // a carried-over squeeze is no edge
            gateActive_=false;gateBlock_=nullptr;gateVerdict_=-1;pendingEdgeNs_=0;
            swapPress_=false;
        }
        const bool swapArmed=settleSwap(in,hold,now);routePhantom(in,hold,now);
        // LASSO: while the lasso is the engine item in a VR hand and the module runs, the lasso brain owns
        // pad RT and LT of BOTH hands (holding trigger withheld, RT only as the brain's launch pulse, never in CAUGHT / RECOVER / a minigame
        // other than the trigger prompts). The brain is also called with the lasso out of hand so that it drops to OFF.
        if(lasso::holstersActive()){
            lasso::RouteIn r{};r.in=&in;r.hold=hold;r.lassoInHand=hold>=0&&currentSlot.load(std::memory_order_relaxed)==SlotLasso;r.menu=menu;
            r.toggle=config().grip==GripMode::Toggle;r.now=now;r.headValid=headValid_;r.headFwd=headFwd_;r.inside={inside_[0],inside_[1]};
            const auto o=lasso::brain.route(r,pad);
            if(o.putAway&&hold>=0){std::uint32_t seq{};post(OpPutAway,SlotLasso,unsigned(hold),PutAwayHoldRelease,3000,now,seq);} // TOGGLE squeeze-and-open
            if(o.owned){emptyLatch_={};emptyDown_={in.triggers[0]>=.55f,in.triggers[1]>=.55f};physDown_=hold>=0&&in.triggers[unsigned(hold)]>=.55f;return;}
        }
        if(hold<0){routeEmpty(in,pad,now,menu);return;} // Empty VR hands never draw a weapon from a trigger
        emptyLatch_={};emptyDown_={in.triggers[0]>=.55f,in.triggers[1]>=.55f};
        const unsigned h=unsigned(hold),o=1u-h;
        // Full CASUAL preset: let the game own firearm trigger duration and cadence.
        // In particular, do not shorten a held RT to a hammer pulse, or retain a
        // manual-reload/rearm latch after drawing, transferring or changing preset.
        // Empty hands, lasso, melee, menu/focus and native gameplay rules still own their paths.
        const bool casualFire=!menu&&weaponPresetOf(settings())==PresetCasual&&phantomFirearmSlot(currentSlot.load());
        // HAMMER: who owns the holding trigger. A change of owner while the trigger is squeezed
        // latches NOT_REARMED until it is released (else the engine would see a fresh 0->1 edge:
        // an extra shot / a Dead Eye paint). Never on the frame a DUAL swap settled: that press is
        // meant to be a fresh edge for the new item. Counted only when it disarms (rtArmed_ was set).
        const auto* hsk=hammerSinks.load(std::memory_order_acquire);
        bool hammerOwns=false;if(!casualFire&&hsk&&hsk->ownsRt){try{hammerOwns=hsk->ownsRt(hold,now);}catch(...){hammerOwns=false;}}
        if(hammerOwns!=hammerOwned_){hammerOwned_=hammerOwns;
            if(in.triggers[h]>triggerRearm&&rtArmed_&&!swapArmed){rtArmed_=false;hammerRearmLatched.fetch_add(1,std::memory_order_relaxed);}}
        if(!rtArmed_&&in.triggers[h]<=triggerRearm)rtArmed_=true;
        if(casualFire){rtArmed_=true;gateActive_=false;gateBlock_=nullptr;gateVerdict_=-1;pendingEdgeNs_=0;}
        const float fire=std::fmin(std::fmax(in.triggers[h],0.f),1.f),other=std::fmin(std::fmax(in.triggers[o],0.f),1.f);
        pad.triggers[1]=fire;logical_[1]=fire>=(logical_[1]?.45f:.55f);
        if(ltNative.load(std::memory_order_relaxed)){pad.triggers[0]=other;logical_[0]=other>=(logical_[0]?.45f:.55f);}
        else{pad.triggers[0]=0;logical_[0]=false;if(other>triggerActive)ltBlockedFrames.fetch_add(1,std::memory_order_relaxed);}
        pad.buttons&=~(runtime::PadLeftTrigger|runtime::PadRightTrigger);
        if(logical_[0])pad.buttons|=runtime::PadLeftTrigger;
        if(logical_[1])pad.buttons|=runtime::PadRightTrigger;
        if(hold==0)swappedFrames.fetch_add(1,std::memory_order_relaxed);
        const char* block=nullptr;
        const bool melee=heldKind.load(std::memory_order_relaxed)==kindMelee||currentSlot.load(std::memory_order_relaxed)==SlotMelee;
        const bool down=in.triggers[h]>=(physDown_?.45f:.55f),edge=down&&!physDown_;bool replay=false,hammerStepped=false;
        // SPIN: SPIN first (the gun on the trigger finger never fires: no hammer step / reload gate call for it), then the order below.
        // RELOAD: RT block reasons in order NOT_REARMED > MELEE > CYLINDER_OPEN > reload::gate. With the gate
        // sink registered, a press is decided ONCE at its physical edge and the verdict holds until release (a press blocked while the cylinder
        // is open stays blocked after it closes; an edge seen while CLOSING is replayed at CLOSED within 200 ms if the trigger is still >= .45).
        // Without the sink (reload killed / not started): the per-frame CYLINDER_OPEN rule.
        const auto gateSink=rtGateSink.load(std::memory_order_acquire);
        if(!down){gateActive_=false;gateBlock_=nullptr;gateVerdict_=-1;pendingEdgeNs_=0;}
        if(casualFire){} // Preserve the physical held state above; no VR firearm gate or pulse shaping.
        else if(spin_.blocksRt(hold)){block="SPIN";if(fire>triggerActive)spin::bump(spin::stats.rtBlockedSpin);} // SPIN: never fires while on the finger
        else if(!rtArmed_){block="NOT_REARMED";rtHeldFrames.fetch_add(1,std::memory_order_relaxed);}
        else if(melee&&!meleeTrigger.load(std::memory_order_relaxed)){block="MELEE";if(fire>triggerActive)rtBlockedMelee.fetch_add(1,std::memory_order_relaxed);}
        else{
            int ph=-1;if(const auto ps=cylinderPhaseSink.load(std::memory_order_acquire)){try{ph=ps(now);}catch(...){ph=-1;}}
            bool open=false;
            if(ph>=0)open=ph!=0;else if(const auto s=fireBlockSink.load(std::memory_order_acquire);s&&s(now))open=true;
            if(open){
                block="CYLINDER_OPEN";if(fire>triggerActive)rtBlockedCylinder.fetch_add(1,std::memory_order_relaxed);
                if(gateSink&&edge){
                    if(ph==3){pendingEdgeNs_=now;rtDeferredEdges.fetch_add(1,std::memory_order_relaxed);}
                    else{gateActive_=true;gateBlock_="CYLINDER_OPEN";gateVerdict_=-1;rtGateLatched.fetch_add(1,std::memory_order_relaxed);}}
            }else if(hammerOwns){
                // HAMMER: R1 / FAN strokes / SA through hammer::step (it calls reload::gate itself: exactly
                // one gate call per edge); the CLOSING edge is replayed at CLOSED through it; a press
                // latched blocked while OPEN stays blocked.
                if(pendingEdgeNs_&&down&&!gateActive_){
                    if(now>=pendingEdgeNs_&&now-pendingEdgeNs_<=deferredEdgeNs){replay=true;rtDeferredReplayed.fetch_add(1,std::memory_order_relaxed);}
                    else{gateActive_=true;gateBlock_="CYLINDER_OPEN";gateVerdict_=-1;rtGateLatched.fetch_add(1,std::memory_order_relaxed);}
                    pendingEdgeNs_=0;}
                if(gateActive_&&gateBlock_&&down)block=gateBlock_;
                else{
                    HammerIn hi{};hi.now=now;hi.hold=hold;hi.physEdge=edge||replay;hi.physDown=down;hi.offEmpty=hammerOffEmpty(o,now);hi.trigger=fire;hi.generation=generation;
                    HammerOut ho{};try{hsk->step(in,hi,ho);}catch(...){ho=HammerOut{};}
                    if(!ho.rawPass){pad.triggers[1]=ho.rt;logical_[1]=ho.rtBit;pad.buttons&=~runtime::PadRightTrigger;if(ho.rtBit)pad.buttons|=runtime::PadRightTrigger;}
                    if(edge||replay)block=ho.edgeBlock;
                    hammerStepped=true;
                }
            }else if(gateSink){
                if(pendingEdgeNs_&&down&&!gateActive_){
                    if(now>=pendingEdgeNs_&&now-pendingEdgeNs_<=deferredEdgeNs){replay=true;rtDeferredReplayed.fetch_add(1,std::memory_order_relaxed);}
                    else{gateActive_=true;gateBlock_="CYLINDER_OPEN";gateVerdict_=-1;rtGateLatched.fetch_add(1,std::memory_order_relaxed);} // too late: stays blocked
                    pendingEdgeNs_=0;}
                if((edge||replay)&&!gateActive_){
                    int v=1;try{v=gateSink(0,hold,now);}catch(...){v=1;}rtGateCalls.fetch_add(1,std::memory_order_relaxed);
                    gateActive_=true;gateVerdict_=v;gateBlock_=v==2?"EMPTY_CHAMBER":v==3?"SPENT_CHAMBER":v==4?"ONE_SHOT":nullptr;
                }
                if(gateActive_&&down){
                    if(gateBlock_)block=gateBlock_;
                    else if(gateVerdict_==0){if(const auto hs=rtHeldSink.load(std::memory_order_acquire);hs&&!hs(now))block="ONE_SHOT";}
                }
            }
        }
        if(hammerOwns&&!hammerStepped){ // blocked (SPIN / NOT_REARMED / MELEE / CYLINDER_OPEN / a latched press): strokes dropped, pulses cancelled
            HammerIn hi{};hi.now=now;hi.hold=hold;hi.physDown=down;hi.offEmpty=hammerOffEmpty(o,now);hi.trigger=fire;hi.generation=generation;hi.block=block?block:"NOT_REARMED";
            HammerOut ho{};try{hsk->step(in,hi,ho);}catch(...){}
        }
        if(block){pad.triggers[1]=0;pad.buttons&=~runtime::PadRightTrigger;}
        if(changed&&down&&!edge)traceRoute(in,pad,hold,hold,now,"TRIGGER_HELD_AT_DRAW",menu);
        if(swapPress_){ // DUAL swap-fire: the press that asked for the swap fired only if its edge is forwarded
            if(edge||replay){swapPress_=false;if(!block)swapsFiredAfter.fetch_add(1,std::memory_order_relaxed);}
            else if(in.triggers[h]<.45f)swapPress_=false;}
        if(edge||replay){
            traceRoute(in,pad,hold,hold,now,block?block:"ROUTED",menu);
            triggerEdges.fetch_add(1,std::memory_order_relaxed);if(!block)triggerForwarded.fetch_add(1,std::memory_order_relaxed);
            else if(!std::strcmp(block,"NOT_REARMED"))rtBlockedRearm.fetch_add(1,std::memory_order_relaxed);
            if(const auto s=triggerSink.load(std::memory_order_acquire)){try{s(hold,!block,block?block:"",now);}catch(...){}}
        }
        physDown_=down;
    }
    // B while a revolver/pistol is in a VR hand belongs to the manual reload (VrReload.h): masked from the game.
    static void routeB(const ControllerSample& in,int hold,PadState& pad,std::uint64_t now) noexcept {
        if(hold>=0&&hold<2&&((spin::fingerMask.load(std::memory_order_acquire)>>unsigned(hold))&1u)){ // SPIN: B never opens a spinning cylinder
            if(pad.buttons&runtime::PadB)spin::bump(spin::stats.bMasked);
            pad.buttons&=~runtime::PadB;return;}
        if(const auto s=bButtonSink.load(std::memory_order_acquire)){
            bool owned=false;try{owned=s(in,hold,now);}catch(...){}
            if(owned){if(pad.buttons&runtime::PadB)bMaskedFrames.fetch_add(1,std::memory_order_relaxed);pad.buttons&=~runtime::PadB;}
            // RELOAD long press: the press began masked (VrControls latched it away); the game must see B now
            else if(bForce.load(std::memory_order_relaxed)&&(in.buttons&runtime::PadB)){pad.buttons|=runtime::PadB;bForcedFrames.fetch_add(1,std::memory_order_relaxed);}
        }
    }
    void applyMasks(PadState& pad) noexcept {
        for(unsigned h=0;h<2;++h)if(captured_[h]||preMasked_[h]||(lasso::holstersActive()&&lasso::ownsHand(h)))pad.buttons&=~detail::gripBit(h); // + lasso grip masks
    }
    // Publishes the holding-hand trigger (ForceAim::Trigger) and the pad the game receives (engine put-away
    // forensics, vr.weapon).
    static void publishPad(const ControllerSample& in,int hold,const PadState& pad,std::uint64_t now) noexcept {
        const float t=hold>=0&&hold<2?in.triggers[unsigned(hold)]:0.f;holdTriggerValue.store(t,std::memory_order_relaxed);
        if(t>triggerActive)holdTriggerNs.store(now,std::memory_order_relaxed);
        lastPadButtons.store(pad.buttons,std::memory_order_relaxed);lastPadRt.store(pad.triggers[1],std::memory_order_relaxed);
        lastPadLt.store(pad.triggers[0],std::memory_order_relaxed);
    }
    std::array<int,2> dwellSlot_{{-1,-1}};std::array<std::uint64_t,2> dwellStart_{};
    // A socket grab that becomes a draw. Rule 1: the lasso socket is always a plain OpDraw (never kept as / next
    // to a phantom); rule 2: while the lasso is the engine item no phantom is set (refusal buzz).
    void socketDraw(unsigned h,int inside,int hold,int ph,int phs,bool phantom,bool dual,bool lassoEngine,const Config& cfg,int current,std::uint64_t now) noexcept {
        (void)phs;std::uint32_t seq{};const bool lassoH=lasso::holstersActive();
        // DUAL: which command the socket grab becomes. The knife is always the phantom when
        // something else is held; otherwise the newly taken item is the engine item ("live = last taken") and the previous
        // engine item stays in its hand as the phantom.
        std::uint16_t op=OpDraw;std::int32_t keep=-1;
        if(dual&&!(lassoH&&inside==SlotLasso)){
            if(phantom&&ph==int(h)){op=inside==SlotMelee?std::uint16_t(OpPhantomSet):std::uint16_t(OpDrawKeep);keep=hold>=0&&hold!=int(h)?hold:-1;} // phantom hand swaps its item
            else if(hold==int(h))op=OpDrawKeep;                                                                       // engine hand (TOGGLE): phantom stays
            else if(hold>=0&&inside!=current){if(inside==SlotMelee)op=OpPhantomSet;else{op=OpDrawKeep;keep=hold;}}     // free hand, the other holds
            else if(hold<0&&phantom)op=OpDrawKeep;                                                                    // engine empty, phantom stays
        }
        if(lassoH&&lassoEngine&&op==OpPhantomSet){
            lasso::bump(lasso::stats.phantomRefusedLasso);lasso::lastRefusal.store("PHANTOM_WITH_LASSO",std::memory_order_relaxed);
            haptics::pulse(h,haptics::refuseAmplitude,haptics::refuseUs,now,haptics::SourceRefuse);pressSequence_[h]=0;return;}
        if(post(op,inside,h,0,1000,now,seq,keep)){
            grabPresses.fetch_add(1,std::memory_order_relaxed);pressSequence_[h]=cfg.grip==GripMode::Hold?seq:0;
            noteEvent(EvGrab,inside,int(h),op==OpPhantomSet?"PHANTOM_SET":op==OpDrawKeep?"DRAW_KEEP":"DRAW",now);
            if(op==OpPhantomSet)phantomSetRequests.fetch_add(1,std::memory_order_relaxed);
            else if(op==OpDrawKeep)drawKeepRequests.fetch_add(1,std::memory_order_relaxed);
            if(support_>=0)setSupport(-1);
        }
    }
public:
    void reset() noexcept {*this=InputBrain{};}
    // pad.ready false = native neutral state: nothing is routed. input==nullptr / head==nullptr / !world suspend holsters.
    std::array<bool,2> diagnosticDown_{};
    void frame(const ControllerSample* input,const Pose* head,bool world,std::uint64_t now,PadState& pad) noexcept {
        using namespace detail;
        inputFrames.fetch_add(1,std::memory_order_relaxed);
        const Config cfg=config();
        const bool gated=!cfg.enabled||!engineReady.load(std::memory_order_acquire)||!mainthread::executorInstalled();
        const int hold=holdHand.load(std::memory_order_acquire);
        const auto menuNs=menuSeenNs.load(std::memory_order_relaxed);
        const bool menu=menuNs&&(now<menuNs||now-menuNs<menuFreezeNs);
        const bool usable=world&&input&&runtime::validController(*input)&&now>=input->publishedNs&&now-input->publishedNs<=inputMaxAgeNs&&
            input->referenceSpace&&input->continuity;
        // Record raw edges even when they never reach route(). These are observed context states, not engine verdicts.
        if(input)for(unsigned h=0;h<2;++h){const bool down=input->triggers[h]>=(diagnosticDown_[h]?.45f:.55f);
            if(down&&!diagnosticDown_[h]&&(gated||!usable||!pad.ready||menu))
                traceRoute(*input,pad,int(h),hold,now,gated?"HOLSTERS_BYPASSED":!usable?"INPUT_NOT_USABLE":!pad.ready?"PAD_NOT_READY":"MENU_OPEN",menu,"INPUT_CONTEXT");
            diagnosticDown_[h]=down;}
        if(const auto* hsk=hammerSinks.load(std::memory_order_acquire);hsk&&hsk->frameBegin){ // HAMMER: FIRST, on every path
            HammerFrame hf{};hf.now=now;hf.in=usable?input:nullptr;hf.head=head&&poseOk(*head)?head:nullptr;hf.hold=hold;hf.menu=menu;
            hf.suspended=gated||menu||!usable||!pad.ready;try{hsk->frameBegin(hf);}catch(...){}}
        spin_.frameBegin(now); // SPIN: the brain's frame-rate EMA (per-displayed-frame spin cap)
        if(gated){
            spin_.reset(now);
            suspend(now,false);lastNs_=now;return; // pure pass-through
        }
        if(!usable||menu||!head||!poseOk(*head)){
            // Head pose missing only (input fine): no sockets this frame, but releases still count and HOLD is not suspended.
            const bool headOnly=usable&&!menu;
            if(!headOnly)spin_.reset(now); // SPIN: a real suspension ends the spin (rigid); a head-only frame keeps it
            // Captures survive every in-world suspension (stale sample, world() false for a frame, transition), as for
            // the menu: a grip still held on resume stays masked (no fresh L1/R1 leaking to the game); the first usable frame
            // with the grip open drops it through release().
            suspend(now,true,!headOnly);
            if(menu)menuSuspended_=true; // A holding grip still squeezed when the menu closes resumes the hold directly
            if(headOnly&&input->epoch==epoch_&&input->referenceSpace==space_&&input->continuity==continuity_){
                const int current=currentSlot.load(std::memory_order_relaxed);const bool allowed=contextAllowed.load(std::memory_order_relaxed);
                noteThrowInput(*input,now);
                for(unsigned h=0;h<2;++h){serviceHolster(h,input->grips[h],cfg,hold,current,allowed,now);
                    if(spinStep(h,*input,cfg,hold,current,allowed,now,nullptr,true))continue; // SPIN: owned hands stay owned (no entries)
                    if(input->grips[h]>gripOpen){gripClosed(h);continue;}
                    if(!deferHoldRelease(h,*input,cfg,hold,now))release(h,*input,cfg,hold,current,allowed,now);}
            }
            if(usable&&pad.ready){route(*input,hold,pad,now,menu);routeB(*input,hold,pad,now);applyMasks(pad);publishPad(*input,hold,pad,now);}
            lastNs_=now;return;
        }
        const ControllerSample& in=*input;
        if(epoch_!=in.epoch||space_!=in.referenceSpace||continuity_!=in.continuity){ // recenter / new space / focus: re-latch
            epoch_=in.epoch;space_=in.referenceSpace;continuity_=in.continuity;bodyValid_=false;
            latched_={true,true};inside_={-1,-1};setSupport(-1);startAwait();
            spin_.reset(now);
        }
        const bool wasSuspended=suspended_; // RELOAD: the reload brain's resume fact
        suspended_=false;
        const bool menuResume=menuSuspended_;menuSuspended_=false; // First usable frame after the menu
        updateBody(in,*head,now);
        headValid_=true;{const auto f=forwardOf(*head);headFwd_={f[0],f[1],f[2]};} // LASSO: gaze aim option
        adoptGameDraw(in,cfg,hold,now); // A weapon the game drew joins the VR HOLD (opening the hand holsters it)
        noteThrowInput(in,now); // THROW: grip pose history + holding-trigger edges
        // THROW: the flying / lying gun (LOCAL, this reference space, fresh) - an empty hand can catch it.
        throwing::Target tg{};
        const bool tgOk=throwing::active()&&throwing::targetBox.load(tg)&&(tg.state==std::uint32_t(throwing::StateFlying)||tg.state==std::uint32_t(throwing::StateResting))&&
            tg.referenceSpace&&tg.referenceSpace==in.referenceSpace&&(tg.ns>=now?tg.ns-now:now-tg.ns)<=250'000'000ull;
        const int current=currentSlot.load(std::memory_order_relaxed);
        const bool allowed=contextAllowed.load(std::memory_order_relaxed);
        const std::uint32_t grabbable=allowed?grabMask.load(std::memory_order_relaxed):0u;
        const bool ours=holdOurs.load(std::memory_order_relaxed);
        // LASSO grade H: the lasso as the engine item (DUAL rules), the not-owned dead zone, teach-once
        const bool lassoH=lasso::holstersActive();
        const bool lassoEngine=lassoH&&hold>=0&&current==SlotLasso;
        const bool lassoOwned=(ownedMask.load(std::memory_order_relaxed)>>unsigned(SlotLasso))&1u;
        if(lassoEngine)lasso::taught.store(true,std::memory_order_relaxed);
        const auto holdSeq=holdSequence.load(std::memory_order_relaxed);
        std::array<bool,2> valid{};std::array<V3,2> hand{};
        for(unsigned h=0;h<2;++h){valid[h]=poseOk(in.grip[h]);if(valid[h])hand[h]={in.grip[h].position[0],in.grip[h].position[1],in.grip[h].position[2]};}
        // Two-hand candidate: a long gun in the hold hand, the other hand free.
        const bool longGun=hold>=0&&current>=0&&current<slotCount&&slotSpecs[std::size_t(current)].twoHanded;
        V3 foregrip{},barrel{};bool foregripValid=false,barrelValid=false; // + the barrel axis (support along it, free-hand mask)
        if(longGun&&valid[unsigned(hold)]&&poseOk(in.aim[unsigned(hold)])){barrel=forwardOf(in.aim[unsigned(hold)]);barrelValid=true;}
        if(cfg.twoHand&&allowed&&barrelValid){
            foregrip=add(hand[unsigned(hold)],mul(barrel,foregripReach));foregripValid=true;
        }
        // DUAL: the phantom (second item) and its hand; the transfer zone around an item held in the other hand.
        const bool dual=dualActive();
        const int ph=phantomHand.load(std::memory_order_acquire),phs=phantomSlot.load(std::memory_order_relaxed);
        const bool phantom=ph>=0&&ph<=1&&phs>=0&&phs<slotCount;
        if(support_>=0&&(!foregripValid||support_==hold||support_==ph||!valid[unsigned(support_)]))setSupport(-1);
        if(const auto rs=reloadBrainSink.load(std::memory_order_acquire)){ // RELOAD: before the guard / grab loop
            ReloadFacts rf{};rf.in=&in;rf.head=head;rf.hold=hold;rf.phantom=phantom?ph:-1;rf.support=support_;rf.sockets=sockets_;rf.captured=captured_;
            rf.latched=latched_;rf.referenceSpace=in.referenceSpace;rf.now=now;rf.resumed=wasSuspended||(lastNs_&&now>lastNs_+100'000'000ull);
            rf.bodySockets=bodySocketsUsed_;try{rs(rf);}catch(...){}
        }
        updateGuard(in,*head,hold,phantom?ph:-1,now);
        for(unsigned h=0;h<2;++h){
            const float g=in.grips[h];
            serviceHolster(h,g,cfg,hold,current,allowed,now);openHandCheck(h,in,cfg,hold,now); // Release = holster, always
            // RELOAD: the free hand of an open cylinder (+ 300 ms after any close) is
            // captured by the reload: no socket, transfer, catch, guard or native grip; a fresh squeeze latches + captures (pressSequence 0) and
            // goes to the reload brain; the grip stays masked until it opens, whenever OPEN ends.
            if(const auto own=freeHandOwnedSink.load(std::memory_order_acquire);own&&valid[h]&&hold!=int(h)&&ph!=int(h)&&support_!=int(h)){
                bool owned=false;try{owned=own(h,now);}catch(...){owned=false;}
                if(owned){
                    inside_[h]=-1;insidePublished[h].store(-1,std::memory_order_relaxed);catchInside_[h]=false;magnetNear_[h]=false;dwellSlot_[h]=-1;
                    reloadCaptures.fetch_add(1,std::memory_order_relaxed);
                    if(g<=gripOpen){release(h,in,cfg,hold,current,allowed,now);continue;}
                    gripClosed(h);
                    if(latched_[h]&&!captured_[h]){captured_[h]=true;pressSequence_[h]=0;} // already squeezed when the ownership began: masked until it opens
                    preMasked_[h]=!latched_[h];
                    if(!latched_[h]&&g>=gripClose){latched_[h]=true;captured_[h]=true;preMasked_[h]=false;pressSequence_[h]=0;
                        reloadCapturePresses.fetch_add(1,std::memory_order_relaxed);
                        if(const auto gs=reloadGrabSink.load(std::memory_order_acquire)){try{gs(h,now);}catch(...){}}}
                    continue;
                }
            }
            std::uint32_t mask=grabbable;if(allowed&&hold==int(h)&&current>=0)mask|=1u<<unsigned(current);
            if(phantom){mask&=~(1u<<unsigned(phs));if(allowed&&ph==int(h))mask|=1u<<unsigned(phs);} // The phantom's socket is its hand's
            int transferFrom=-1; // DUAL: this (empty) hand closes on the item held by the other hand
            bool transferLasso=false; // LASSO DUAL rule 2: never a transfer of the lasso (the cowboy grip at the coil)
            bool transferHammer=false; // HAMMER: transfer refused right after a swipe; the press stays captured (masked, buzz)
            if(transferActive()&&valid[h]&&hold!=int(h)&&ph!=int(h)){
                const unsigned x=1u-h;
                if((hold==int(x)||(phantom&&ph==int(x)))&&valid[x]&&poseOk(in.aim[x])){
                    const V3 point=add(hand[x],mul(forwardOf(in.aim[x]),transferAhead));
                    if(distance(hand[h],point)<=transferRadius.load(std::memory_order_relaxed))transferFrom=int(x);
                    if(transferFrom>=0&&spin_.blocksRt(transferFrom)){transferFrom=-1;spin::bump(spin::stats.transferRefused);}
                }
                if(transferFrom>=0&&lassoEngine&&transferFrom==hold){transferFrom=-1;transferLasso=true;}
                // HAMMER: no transfer of the revolver right after this hand touched its hammer (a swipe ends there)
                if(transferFrom>=0&&transferFrom==hold){const auto hc=hammerContactNs[h].load(std::memory_order_relaxed);
                    if(hc&&now>=hc&&now-hc<hammerTransferBlockNs.load(std::memory_order_relaxed)){transferFrom=-1;transferHammer=true;}}
            }
            // Socket under the hand, with hysteresis (per slot: belt 0.32 / 0.40 taller than wide, others 0.24 / 0.30 on the body).
            int inside=-1;bool deadZone=false,backDead=false,mouth=false; // + empty back socket, whistle at the mouth
            if(valid[h]){
                const int was=inside_[h];
                // best = smallest distance / the slot's enter radius (<= 1); the previous zone is kept up to its leave radius unless another
                // zone is now a better fit (big zones overlap: a hand moving from the knife to the lasso must switch, not stick)
                int best=-1;float bestS=1.f;
                for(int k=0;k<slotCount;++k)if(mask>>unsigned(k)&1u){const float s=zoneDistance(k,hand[h])/enterRadiusOf(k);if(s<=bestS){bestS=s;best=k;}}
                const bool wasOk=was>=0&&(mask>>unsigned(was)&1u)&&zoneDistance(was,hand[h])<=exitRadiusOf(was);
                if(best>=0&&(!wasOk||best==was||bestS<zoneDistance(was,hand[h])/enterRadiusOf(was)))inside=best;
                else if(wasOk)inside=was;
                // Where the knife and lasso zones overlap,
                // the LEFT hand takes the lasso and the RIGHT hand takes the knife (cross draw).
                if((inside==SlotMelee||inside==SlotLasso)&&(mask>>unsigned(SlotMelee)&1u)&&(mask>>unsigned(SlotLasso)&1u)){
                    const bool inKnife=zoneDistance(SlotMelee,hand[h])<=exitRadiusOf(SlotMelee),inLasso=zoneDistance(SlotLasso,hand[h])<=exitRadiusOf(SlotLasso);
                    if(inKnife&&inLasso)inside=h==0?SlotLasso:SlotMelee;
                }
                // LASSO: the lasso socket of a NOT OWNED lasso is a probe zone (no grab, no marker): a squeeze there
                // is masked (no LB -> no weapon wheel) with a refusal buzz and the note NOT OWNED - only when no grabbable zone fits better.
                if(lassoH&&allowed&&!lassoOwned&&lasso::config.deadzone>0.f){
                    const float ds=zoneDistance(SlotLasso,hand[h])/(enterRadiusOf(SlotLasso)*lasso::config.deadzone);
                    if(ds<=1.f&&(inside<0||ds<zoneDistance(inside,hand[h])/enterRadiusOf(inside))){deadZone=true;inside=-1;}
                }
                // BACK (user: behind the left shoulder the game's weapon wheel opened - its SHOTGUN slot was empty, the grip went native
                // as LB): a hand at a back socket whose slot is empty takes another owned long gun (right shoulder: LONGARM, SNIPER, SHOTGUN;
                // left: SHOTGUN, LONGARM, SNIPER; behind the head: SNIPER, LONGARM, SHOTGUN); with none the squeeze is masked (no wheel).
                // the zone is chosen by backZoneOf (position, not the nearest socket); the back replaces any back pick of `best`
                if(allowed&&!deadZone&&(inside<0||backSlot(inside))){
                    static constexpr int backOrder[3][3]{{SlotSniper,SlotLongarm,SlotShotgun},{SlotLongarm,SlotSniper,SlotShotgun},{SlotShotgun,SlotLongarm,SlotSniper}};
                    int z=backZoneOf(h,hand[h]);
                    if(z>=0&&hold==int(h)&&backSlot(current))z=current; // the long gun in this hand goes back anywhere on the back
                    else if(z>=0&&phantom&&ph==int(h)&&backSlot(phs))z=phs;
                    if(z>=0){inside=-1;for(int alt:backOrder[z-SlotSniper])if(mask>>unsigned(alt)&1u){inside=alt;break;}
                        if(inside<0)backDead=true;}
                }else backZone_[h]=-1;
                if(inside<0&&!deadZone&&!backDead){const std::uint32_t ownedNow=ownedMask.load(std::memory_order_relaxed)&~mask;
                    for(int k=0;k<slotCount&&!backDead;++k)if((ownedNow>>unsigned(k)&1u)&&zoneDistance(k,hand[h])<=enterRadiusOf(k)&&!(k==SlotLasso&&lassoH))backDead=true;}
                // WHISTLE (user): an EMPTY hand at the mouth (eye + 7 cm forward - 8 cm down, 15 cm) squeezing the grip = the whistle
                if(inside<0&&!deadZone&&!backDead&&hold!=int(h)&&ph!=int(h)&&support_!=int(h)&&head){
                    const auto f=forwardOf(*head);
                    const V3 m{head->position[0]+f[0]*.07f,head->position[1]+f[1]*.07f-.08f,head->position[2]+f[2]*.07f};
                    if(distance(hand[h],m)<.15f)mouth=true;
                }
                if(inside>=0&&inside!=was&&(!latched_[h]||in.grips[h]<=gripOpen)&&!spin_.blocksRt(int(h))){ // SPIN: no slot pulses for a spinning hand
                    if(lassoH&&inside==SlotLasso){haptics::pulse(h,lassoEnterAmplitude,lassoEnterUs,now,haptics::SourceSlotEnter);lasso::bump(lasso::stats.lassoZoneEnters);}
                    else if(beltZone(inside))haptics::pulse(h,beltEnterAmplitude,beltEnterUs,now,haptics::SourceSlotEnter);
                    else haptics::pulse(h,haptics::enterAmplitude,haptics::enterUs,now,haptics::SourceSlotEnter);}
            }
            inside_[h]=inside;insidePublished[h].store(inside,std::memory_order_relaxed);
            // the support grip anywhere along the barrel (25..60 cm ahead of the holding grip; was the 35 cm point only); a free hand
            // within nearRadius of it never presses LB / RB
            float barrelD=1e9f;
            if(barrelValid&&int(h)!=hold&&int(h)!=ph&&valid[h]){const V3& g0=hand[unsigned(hold)];
                const float t=std::fmin(std::fmax(dot(sub(hand[h],g0),barrel),.25f),.60f);barrelD=distance(hand[h],add(g0,mul(barrel,t)));}
            const bool nearForegrip=foregripValid&&barrelD<=supportRadius,nearLongGun=foregripValid&&barrelD<=nearRadius; // TWO HAND AIM off: native
            // THROW: catch zone of an empty hand (air: sphere around the gun, extrapolated <= 60 ms; ground: a cylinder above it);
            // the throwing hand only after the grace. A magnet tick once per approach.
            bool catchZone=false,catchEnter=false;
            if(tgOk&&valid[h]&&hold!=int(h)&&ph!=int(h)&&support_!=int(h)){
                V3 pos=tg.pos;bool near=false;
                if(tg.state==std::uint32_t(throwing::StateFlying)){
                    const std::uint64_t age=std::min<std::uint64_t>(now>tg.ns?now-tg.ns:0,60'000'000ull);pos=add(pos,mul(tg.vel,float(double(age)*1e-9)));
                    const float d=distance(hand[h],pos);catchZone=d<=tg.radius;near=d<=throwing::config.magnetRadius;
                }else{
                    const float dx=hand[h][0]-pos[0],dz=hand[h][2]-pos[2],dy=hand[h][1]-pos[1],r=std::sqrt(dx*dx+dz*dz);
                    catchZone=r<=tg.groundRadius&&dy>=-.10f&&dy<=tg.groundHeight;near=r<=throwing::config.magnetRadius&&dy>=-.20f&&dy<=tg.groundHeight+.20f;
                }
                if(int(h)==tg.throwHand&&tg.throwNs&&now>=tg.throwNs&&now-tg.throwNs<throwing::config.graceNs){catchZone=false;near=false;}
                catchEnter=catchZone&&!catchInside_[h];catchInside_[h]=catchZone;
                if(near&&!magnetNear_[h]){magnetNear_[h]=true;haptics::pulse(h,.12f,10'000,now,haptics::SourceSlotEnter);throwing::bump(throwing::magnetTicks);}
                else if(!near)magnetNear_[h]=false;
            }else{catchInside_[h]=false;magnetNear_[h]=false;}
            if(spinStep(h,in,cfg,hold,current,allowed,now,head,false))continue; // SPIN: the gun on the trigger finger (before the release branch)
            if(!valid[h]){latched_[h]=true;preMasked_[h]=false;continue;}
            if(g<=gripOpen){if(!deferHoldRelease(h,in,cfg,hold,now))release(h,in,cfg,hold,current,allowed,now);continue;}
            gripClosed(h);
            // THROW: a hand already closed when the flying gun arrives catches it (no other capture holds it)
            if(catchEnter&&tg.state==std::uint32_t(throwing::StateFlying)&&throwing::config.closedCatch&&latched_[h]&&!captured_[h]&&g>=gripClose){
                postCatch(h,tg,true,cfg,now);continue;}
            // The menu was used while this hand kept holding the weapon (calibration): the grip never opened, so adopt it
            // as the hold press again - opening it later holsters as usual instead of leaving the gun in an open hand.
            if(menuResume&&awaitFirstGrip_&&hold==int(h)&&ours&&cfg.grip==GripMode::Hold&&captured_[h]&&g>=gripClose){
                pressSequence_[h]=holdSeq;awaitFirstGrip_=false;resumes.fetch_add(1,std::memory_order_relaxed);menuResumes.fetch_add(1,std::memory_order_relaxed);}
            if(menuResume&&phantom&&ph==int(h)&&cfg.grip==GripMode::Hold&&captured_[h]&&g>=gripClose&&!pressSequence_[h]){ // Same for the phantom
                pressSequence_[h]=phantomSequence.load(std::memory_order_relaxed);phantomResumes.fetch_add(1,std::memory_order_relaxed);menuResumes.fetch_add(1,std::memory_order_relaxed);}
            // Grip starting inside a socket / on the foregrip / on the other hand's item never reaches the game, not even its first frames.
            // A raised empty fist in the guard box (guardMask_) never presses LB (weapon wheel) / RB (cover) either
            preMasked_[h]=!latched_[h]&&(inside>=0||nearForegrip||transferFrom>=0||transferHammer||catchZone||guardMask_[h]||deadZone||backDead||mouth||nearLongGun);
            if(dwellSlot_[h]>=0){ // LASSO DUAL rule 3: a deliberate 300 ms dwell inside the zone with the grip closed, then the normal grab
                if(inside!=dwellSlot_[h]||!lasso::holstersActive())dwellSlot_[h]=-1;
                else if(now>=dwellStart_[h]&&now-dwellStart_[h]>=lassoDwellNs){const int k=dwellSlot_[h];dwellSlot_[h]=-1;lasso::bump(lasso::stats.socketGrabDwelled);
                    socketDraw(h,k,hold,ph,phs,phantom,dual,lassoEngine,cfg,current,now);}
            }
            if(latched_[h]||g<gripClose)continue;
            latched_[h]=true;preMasked_[h]=false; // fresh press
            if(catchZone){postCatch(h,tg,false,cfg,now);continue;} // THROW: the catch wins over sockets / foregrip / transfer
            if(backDead){captured_[h]=true;pressSequence_[h]=0;backMasked.fetch_add(1,std::memory_order_relaxed); // Empty back, no long gun owned
                noteEvent(EvRefuse,backZone_[h],int(h),"BACK_EMPTY",now);
                haptics::pulse(h,haptics::refuseAmplitude,haptics::refuseUs,now,haptics::SourceRefuse);continue;}
            if(mouth){captured_[h]=true;pressSequence_[h]=0;::rdrvr::vr::controls::whistleRequest.store(true,std::memory_order_relaxed);
                mouthWhistles.fetch_add(1,std::memory_order_relaxed);haptics::pulse(h,haptics::enterAmplitude,haptics::enterUs,now,haptics::SourceSlotEnter);continue;}
            if(deadZone){ // LASSO: the not-owned lasso socket - masked, refusal buzz, note NOT OWNED (no weapon wheel)
                captured_[h]=true;pressSequence_[h]=0;lasso::bump(lasso::stats.notOwnedMasked);lasso::noteNotOwned(now);lasso::lastRefusal.store("NOT_OWNED",std::memory_order_relaxed);
                haptics::pulse(h,haptics::refuseAmplitude,haptics::refuseUs,now,haptics::SourceRefuse);continue;}
            if(inside>=0){
                captured_[h]=true;std::uint32_t seq{};
                if(hold==int(h)&&inside==current){ // the weapon's own socket: holster it
                    if(dual&&phantom&&ph!=int(h)){ // DUAL: ... and the phantom of the other hand becomes the engine item (promote)
                        if(post(OpDrawKeep,phs,unsigned(ph),2,1000,now,seq,-1)){putAwayPresses.fetch_add(1,std::memory_order_relaxed);promoteRequests.fetch_add(1,std::memory_order_relaxed);}
                    }else if(post(OpPutAway,current,h,PutAwaySlot,1000,now,seq))putAwayPresses.fetch_add(1,std::memory_order_relaxed);
                    pressSequence_[h]=0;noteEvent(EvRelease,current,int(h),"SOCKET_PRESS",now);
                }else if(phantom&&ph==int(h)&&inside==phs){ // DUAL: the phantom's own socket: back to the holster
                    if(post(OpPhantomClear,phs,h,ClearSocket,1000,now,seq))putAwayPresses.fetch_add(1,std::memory_order_relaxed);
                    pressSequence_[h]=0;
                }else if(lassoEngine&&int(h)!=hold&&inside==SlotLasso){ // LASSO: the free hand at the lasso's own socket never moves the rope
                    pressSequence_[h]=0;lasso::bump(lasso::stats.transferRefusedLasso);
                    haptics::pulse(h,haptics::refuseAmplitude,haptics::refuseUs,now,haptics::SourceRefuse);
                }else if(lassoH&&int(h)!=hold&&lasso::brain.dwellRequired()){ // LASSO DUAL rule 3: the first squeeze only buzzes, a 300 ms dwell grabs
                    dwellSlot_[h]=inside;dwellStart_[h]=now;pressSequence_[h]=0;lasso::bump(lasso::stats.socketGrabDeferred);
                    haptics::pulse(h,haptics::refuseAmplitude,haptics::refuseUs,now,haptics::SourceRefuse);
                }else socketDraw(h,inside,hold,ph,phs,phantom,dual,lassoEngine,cfg,current,now);
                if(hold==int(h))awaitFirstGrip_=false;
            }else if(nearForegrip&&support_<0){
                captured_[h]=true;setSupport(int(h));
            }else if(transferFrom>=0){ // DUAL: take the item out of the other hand (engine item: OpTransfer; phantom: OpPhantomSet move)
                captured_[h]=true;std::uint32_t seq{};const unsigned x=unsigned(transferFrom);const bool fromPhantom=phantom&&ph==transferFrom;
                if(post(fromPhantom?OpPhantomSet:OpTransfer,fromPhantom?phs:current,h,fromPhantom?1:0,1000,now,seq,int(x))){
                    transfersRequested.fetch_add(1,std::memory_order_relaxed);pressSequence_[h]=cfg.grip==GripMode::Hold?seq:0;
                    noteEvent(EvGrab,fromPhantom?phs:current,int(h),fromPhantom?"PHANTOM_MOVE":"TRANSFER",now);
                    // The giving hand keeps its press: after a done transfer holdHand/holdSequence moved to the taker, so its
                    // release does nothing; after a refused one it still holsters.
                    if(hold==int(x))awaitFirstGrip_=false;
                }
            }else if(transferHammer){
                // game either (it would be LB weapon wheel / RB cover): masked until it opens, a refusal buzz; a squeeze after the block transfers
                captured_[h]=true;pressSequence_[h]=0;hammerTransferSuppressed.fetch_add(1,std::memory_order_relaxed);
                haptics::pulse(h,haptics::refuseAmplitude,haptics::refuseUs,now,haptics::SourceRefuse);
            }else if(awaitFirstGrip_&&hold==int(h)&&ours&&cfg.grip==GripMode::Hold){
                captured_[h]=true;pressSequence_[h]=holdSeq;awaitFirstGrip_=false;resumes.fetch_add(1,std::memory_order_relaxed);
                if(gameDrawAwait_){gameDrawAwait_=false;adoptSqueezed.fetch_add(1,std::memory_order_relaxed);}
                noteEvent(EvAdopt,current,int(h),"SQUEEZE",now);
            }else if(phantom&&ph==int(h)&&cfg.grip==GripMode::Hold&&!pressSequence_[h]&&phantomSequence.load(std::memory_order_relaxed)){
                // DUAL: after a menu / recenter / blocked release the phantom hand re-adopts its item on the next squeeze
                captured_[h]=true;pressSequence_[h]=phantomSequence.load(std::memory_order_relaxed);phantomResumes.fetch_add(1,std::memory_order_relaxed);
            }else if(guardMask_[h]){ // FIST BLOCK: a raised empty fist - masked, no command (the guard / the physical fist use it)
                captured_[h]=true;pressSequence_[h]=0;guard::bump(guard::maskedPresses);
            }else if(nearLongGun){ // The free hand at a held long gun (missed the barrel) - masked, no command, never a put-away
                captured_[h]=true;pressSequence_[h]=0;freeHandMasked.fetch_add(1,std::memory_order_relaxed);noteEvent(EvRefuse,current,int(h),"FREE_HAND_MASKED",now);
            }else{
                if(transferLasso)lasso::bump(lasso::stats.transferRefusedLasso); // LASSO: the throw hand at the coil (the grip is the lasso's: masked)
                nativeGripPresses.fetch_add(1,std::memory_order_relaxed);noteEvent(EvGrab,-1,int(h),"NATIVE_LB_RB",now);
            }
        }
        if(tgOk&&tg.state==std::uint32_t(throwing::StateFlying)){ // THROW magnet: the free hands (LOCAL), the main-thread flight pulls the gun
            throwing::CatchHands ch{};ch.ns=now;ch.referenceSpace=in.referenceSpace;
            for(unsigned h=0;h<2;++h){if(!valid[h]||hold==int(h)||ph==int(h)||support_==int(h))continue;
                ch.pos[h]={hand[h][0],hand[h][1],hand[h][2]};ch.grip[h]=in.grips[h];ch.mask|=1u<<h;}
            throwing::handsBox.store(ch);
        }
        if(hold<0||!ours)awaitFirstGrip_=false;
        if(pad.ready){route(in,hold,pad,now);routeB(in,hold,pad,now);applyMasks(pad);publishPad(in,hold,pad,now);}
        // Markers.
        publishHeld();
        MarkerSet set{};set.sampleNs=now;set.referenceSpace=in.referenceSpace;
        // No holster markers while the BODY sockets are in use
        // (user: yellow zones appeared on a horse - no body sockets there): never any holster markers while BODY is ON
        if(cfg.markers!=MarkerMode::Off&&allowed&&!bodySocketsUsed_&&!::rdrvr::vr::settings().body()){
            std::uint32_t shown=grabbable;if(hold>=0&&current>=0)shown|=1u<<unsigned(current);
            if(phantom)shown|=1u<<unsigned(phs); // Where the phantom goes back
            // LASSO: with BODY ON (the shown body's sockets), NEAR shows only the zone under a hand plus the lasso
            // marker until its first grab this session (teach-once); BODY OFF keeps the NEAR look (0.45 m).
            const bool bodyNear=lassoH&&cfg.markers==MarkerMode::Near&&bodySocketsUsed_;
            for(int k=0;k<slotCount&&set.count<set.markers.size();++k){
                if(!(shown>>unsigned(k)&1u))continue;
                float nearest=1e9f;for(unsigned h=0;h<2;++h)if(valid[h])nearest=std::fmin(nearest,distance(hand[h],sockets_[std::size_t(k)]));
                if(bodyNear){if(inside_[0]!=k&&inside_[1]!=k&&!(k==SlotLasso&&!lasso::taught.load(std::memory_order_relaxed)))continue;}
                else if(cfg.markers==MarkerMode::Near&&nearest>nearRadius)continue;
                auto& m=set.markers[set.count++];const auto& p=sockets_[std::size_t(k)];m.position={p[0],p[1],p[2]};m.radius=enterRadiusOf(k);
                m.highlight=(inside_[0]==k||inside_[1]==k)?1.f:0.f;m.slot=k;m.kind=0;
            }
            if(foregripValid&&set.count<set.markers.size()){
                const unsigned other=1u-unsigned(hold);const float d=valid[other]?distance(hand[other],foregrip):1e9f;
                if(support_>=0||cfg.markers==MarkerMode::Always||d<=nearRadius){
                    auto& m=set.markers[set.count++];m.position={foregrip[0],foregrip[1],foregrip[2]};m.radius=supportRadius;
                    m.highlight=(support_>=0||d<=supportRadius)?1.f:0.f;m.slot=-1;m.kind=1;
                }
            }
        }
        if(const auto* hsk=hammerSinks.load(std::memory_order_acquire);hsk&&hsk->marker&&hold>=0&&set.count<set.markers.size()){
            // HAMMER: the ring above the spur, only with the cylinder CLOSED
            int cylPhase=-1;if(const auto ps=cylinderPhaseSink.load(std::memory_order_acquire)){try{cylPhase=ps(now);}catch(...){cylPhase=-1;}}
            if(cylPhase<=0){try{hsk->marker(set,in,hold,now);}catch(...){}}
        }
        publish(set);
        lastNs_=now;
    }
    float bodyYaw() const noexcept {return bodyYaw_;}
    const std::array<detail::V3,slotCount>& sockets() const noexcept {return sockets_;}
    bool awaitingFirstGrip() const noexcept {return awaitFirstGrip_;}
};
inline InputBrain brain{};
// NativePadDriver pad filter body (input thread). head = HMD pose in OpenXR LOCAL (same space as the controller poses).
inline void padFrame(const ControllerSample* input,const Pose* head,bool world,std::uint64_t now,PadState& pad) noexcept {
    try{brain.frame(input,head,world,now,pad);}catch(...){}
}

// Weapon hooks (main thread): logical weapon hand 1 = the physical hand that holds the weapon, plus the two-hand barrel.
// Call on every ControllerSample the weapon pipeline consumes (wrap NativePadDriver::outputInput). Returns true if changed.
inline bool weaponInput(ControllerSample& in) noexcept {
    using namespace detail;
    if(!engineReady.load(std::memory_order_acquire)||!config().enabled)return false;
    const int hold=holdHand.load(std::memory_order_acquire);if(hold<0||hold>1)return false;
    const unsigned h=unsigned(hold);
    Pose grip=in.grip[h],aim=in.aim[h];bool rotated=false;
    const int s=supportHand.load(std::memory_order_relaxed);
    if(s>=0&&s!=hold&&config().twoHand&&in.grips[unsigned(s)]>gripOpen&&poseOk(grip)&&poseOk(aim)&&poseOk(in.grip[unsigned(s)])){
        const V3 from=forwardOf(aim);
        const V3 to=sub(V3{in.grip[unsigned(s)].position[0],in.grip[unsigned(s)].position[1],in.grip[unsigned(s)].position[2]},
                        V3{grip.position[0],grip.position[1],grip.position[2]});
        Q arc{};
        if(length(to)>.05f&&limitedArc(from,to,twoHandMaxAngle,arc)){
            aim.orientation=normalized(product(arc,aim.orientation));grip.orientation=normalized(product(arc,grip.orientation));
            rotated=true;twoHandApplied.fetch_add(1,std::memory_order_relaxed);
        }
    }
    if(h==1&&!rotated)return false;
    in.grip[1]=grip;in.aim[1]=aim;if(h==0)handSubstituted.fetch_add(1,std::memory_order_relaxed);return true;
}
// DUAL (any thread, the phantom's pose hook): logical weapon hand 1 = the physical hand holding the phantom (no two-hand bend).
inline bool phantomInput(ControllerSample& in) noexcept {
    const int ph=phantomHand.load(std::memory_order_acquire);if(ph<0||ph>1)return false;
    in.grip[1]=in.grip[unsigned(ph)];in.aim[1]=in.aim[unsigned(ph)];return true;
}

// =====================================================================================================================
// Engine side (main thread only).
using SelectItemFn=bool(*)(void* inventory,void* item,bool draw,bool animated,bool selectFlag); // x0 this, x1 item, w2..w4
using PutAwayWeaponFn=void(*)(void* player,void* inventory);                                     // x0 sagPlayer*, x1 inventory
using CanDrawFn=bool(*)(const void* player,const void* weaponType);                              // x0 sagPlayer*, x1 invWeaponType*
using SetHoldModeFn=void(*)(void* player,bool on,int flag);                                        // x0 sagPlayer*, w1, w2 InputDisableFlag
using SetForceAimFn=void(*)(const void* handle,bool on);                                           // x0 unused (sagPlayerHandle*), w1
using SetShouldRenderFn=void(*)(void* weapon,bool on);                                             // DUAL: weapWeapon::SetShouldRender
using RefreshRenderFn=void(*)(void* item);                                                         // DUAL: invWeapon::RefreshWeaponRender
struct PlayerBlocks {
    bool alive{},controlDisabled{},deadEye{},riding{},ridingVehicle{},vehicleTransition{},duel{},mountedWeapon{},mountingHorse{},
         blindFiring{},cover{},linkedAnimation{},usingGringo{};
};
// Adapter over engine::WeaponContextReader::inspectPlayer (integration). False = context unreadable.
using ContextFn=bool(*)(void* user,std::uintptr_t player,std::uintptr_t actor,std::uint32_t actorGuid,PlayerBlocks& out);
struct EngineApi {
    bool (*read)(void*,std::uintptr_t,void*,std::size_t){};
    std::uintptr_t gameData{},playerManager{},playerManagerTable{},playerTable{},actorManagerGlobal{},actorSlotsGlobal{};
    std::uintptr_t gohManagerGlobal{},gohSlotsGlobal{},inventoryTable{};
    std::array<std::uintptr_t,2> itemTables{},typeTables{};
    SelectItemFn selectItem{};PutAwayWeaponFn putAwayWeapon{};CanDrawFn canDraw{};
    ContextFn context{};void* contextUser{};
    bool animatedDraw{};
    SetHoldModeFn setHoldMode{};SetForceAimFn setForceAim{}; // Optional (weapon keep / force aim); null = feature off
    SetShouldRenderFn setShouldRender{};RefreshRenderFn refreshRender{}; // DUAL optional: null = the phantom is never force-rendered
    explicit operator bool() const {
        return read&&gameData&&playerManager&&playerManagerTable&&playerTable&&actorManagerGlobal&&actorSlotsGlobal&&gohManagerGlobal&&
            gohSlotsGlobal&&inventoryTable&&itemTables[0]&&itemTables[1]&&typeTables[0]&&typeTables[1]&&selectItem&&putAwayWeapon&&canDraw&&context;
    }
};
struct Sample {
    std::uintptr_t player{},actor{},inventory{},handItem{};std::uint32_t guid{},foot{},gameState{};
    std::array<std::uintptr_t,slotCount> items{},types{},rawItems{};std::uint32_t owned{};int handSlot{-1};
    std::uint16_t invFlags{};std::uint8_t slotEnabled{};bool alive{},locked{},deadEye{},playerFlags{};
    std::uint8_t aimBits{};float holdTimer{};std::int32_t selectState{-1},zoom{-1};
};

class EngineSide {
    EngineApi api_{};
    mainthread::Command pending_{};bool hasPending_{};
    std::uintptr_t expected_{},lastHandItem_{};std::uint64_t expectedNs_{};
    std::array<std::uintptr_t,slotCount> namedItems_{};std::uintptr_t namedHand_{};
    PlayerBlocks blocks_{};bool blocksValid_{};std::uint64_t pollIndex_{};
    // Validated slot items: the poll re-validates an unchanged item pointer only every 30 polls (each guarded read can be
    // a syscall on the CPU-bound main thread); command execution always validates afresh.
    struct SlotCache {std::uintptr_t item{},type{};bool ok{};std::uint64_t poll{};};
    std::array<SlotCache,slotCount> slotCache_{};
    std::array<std::uintptr_t,slotCount> lastSlotItem_{},lastSlotType_{}; // The item last seen in each slot
    // Weapon keep / force aim (main thread only).
    bool forceOurs_{},foreignSeen_{},prevOurs_{};std::uintptr_t prevHand_{};int prevHold_{-1};float prevTimer_{};int prevAimBits_{-1},prevState_{-1};
    std::uint64_t lastOurPutAwayNs_{};
    const char* prevGate_{}; // The context gate reason of the previous poll (nullptr = open)
    int lastThrownSlot_{-1}; // The slot of the last flight (holsteredBox skips it right after the flight)
    std::uint32_t gameDrawCounter_{}; // Adopted game draws (hold sequence = gameDrawSequenceFlag | counter)
    // the hold dropped by an unreadable poll (the same item back within gapRestoreNs = the same hold, not a game draw into the right
    // hand), the hand whose weapon the engine took / the gap dropped (a game draw within recentHandNs goes to it), the command's slot (events)
    struct Gap {int hand{-1};bool ours{};std::uint32_t seq{};std::uintptr_t item{};std::uint64_t ns{};};
    Gap gap_{};int recentHand_{-1};std::uint64_t recentNs_{};int cmdSlot_{-1};
    // DUAL: the phantom (main thread owns it, the atomics above publish it) and the swap-fire in flight.
    struct Phantom {int slot{-1},hand{-1};std::uintptr_t item{},weapon{};std::uint32_t sequence{};std::uint64_t since{};bool renderForced{};};
    Phantom phantom_{};
    std::uint32_t swapSeq_{};std::uint64_t swapStartNs_{};std::uintptr_t swapItem_{};
    std::uintptr_t weaponOf(std::uintptr_t item) const noexcept {std::uintptr_t w{};return item&&get(item,itemWeapon,w)?w:0;}
    void publishPhantom(int slot,std::uintptr_t item,std::uintptr_t weapon,int hand,std::uint32_t sequence,std::uint64_t now) noexcept {
        const bool same=phantom_.item==item;
        phantom_=Phantom{slot,hand,item,weapon,sequence,same?phantom_.since:now,same&&phantom_.renderForced};
        phantomSlot.store(slot,std::memory_order_relaxed);phantomItem.store(item,std::memory_order_relaxed);phantomWeapon.store(weapon,std::memory_order_relaxed);
        phantomSequence.store(sequence,std::memory_order_relaxed);phantomSinceNs.store(phantom_.since,std::memory_order_relaxed);
        phantomRenderForced.store(phantom_.renderForced,std::memory_order_relaxed);
        phantomHand.store(hand,std::memory_order_release);phantomGeneration.fetch_add(1,std::memory_order_release);
    }
    // why == ClearCount: silent (the phantom became the engine item by a swap). restore: hand the holster its own render rule back
    // (only while the item stays holstered and is still in its slot).
    void clearPhantom(unsigned why,bool restore=true) noexcept {
        if(!phantom_.item&&phantom_.hand<0)return;
        if(phantom_.renderForced&&restore&&api_.refreshRender){
            try{api_.refreshRender(reinterpret_cast<void*>(phantom_.item));}catch(...){}renderRestored.fetch_add(1,std::memory_order_relaxed);}
        phantom_=Phantom{};
        phantomHand.store(-1,std::memory_order_release);phantomSlot.store(-1,std::memory_order_relaxed);phantomItem.store(0,std::memory_order_relaxed);
        phantomWeapon.store(0,std::memory_order_relaxed);phantomSequence.store(0,std::memory_order_relaxed);phantomRenderForced.store(false,std::memory_order_relaxed);
        phantomSinceNs.store(0,std::memory_order_relaxed);phantomGeneration.fetch_add(1,std::memory_order_release);
        if(why<ClearCount){phantomClears[why].fetch_add(1,std::memory_order_relaxed);lastPhantomClear.store(phantomClearNames[why],std::memory_order_relaxed);}
    }
    void dualRefuse(DualRefusal why,unsigned hand,std::uint64_t now) noexcept {
        dualRefusals[why].fetch_add(1,std::memory_order_relaxed);lastRefusal.store(dualRefusalNames[why],std::memory_order_relaxed);
        noteEvent(EvRefuse,cmdSlot_,int(hand&1u),dualRefusalNames[why],now);
        haptics::pulse(hand&1u,haptics::refuseAmplitude,haptics::refuseUs,now,haptics::SourceRefuse);
    }
    // Every successful poll while a phantom lives: Dead Eye / cover keep it (melee is gated anyway).
    void checkPhantom(const Sample& s,std::uint64_t now) noexcept {
        if(!dualActive()||!config().enabled){clearPhantom(ClearKill);return;}
        phantomTicks.fetch_add(1,std::memory_order_relaxed);
        const auto& b=blocks_;const bool riding=s.foot==footHorse||(blocksValid_&&b.riding);
        if(s.gameState!=3||!s.playerFlags||!s.alive||s.locked||(s.foot!=footOnFoot&&s.foot!=footHorse)||(riding&&!phantomFirearmSlot(phantom_.slot))||
           (blocksValid_&&(b.controlDisabled||b.ridingVehicle||b.vehicleTransition||b.duel||b.mountedWeapon||b.mountingHorse||b.linkedAnimation||b.usingGringo))){
            clearPhantom(ClearContext);return;}
        const int k=phantom_.slot;
        if(k<0||k>=slotCount||s.rawItems[std::size_t(k)]!=phantom_.item){clearPhantom(ClearSlotChanged,false);return;}
        if(!expected_&&s.handItem==phantom_.item){clearPhantom(ClearInHand,false);return;}
        if(s.handItem&&holdHand.load(std::memory_order_relaxed)==phantom_.hand){clearPhantom(ClearGameDraw);return;} // both items in one hand
        const auto w=weaponOf(phantom_.item);
        if(!w||w!=phantom_.weapon){clearPhantom(ClearNoWeapon,false);return;}
        if(forceRender.load(std::memory_order_relaxed)&&api_.setShouldRender){ // a knife holster may hide its weapon
            std::uint32_t flags{};
            if(get(w,weaponFlags,flags)&&!(flags&weaponRenderBit)){
                try{api_.setShouldRender(reinterpret_cast<void*>(w),true);}catch(...){}
                phantom_.renderForced=true;phantomRenderForced.store(true,std::memory_order_relaxed);renderForcedCalls.fetch_add(1,std::memory_order_relaxed);}
        }
        (void)now;
    }
    // Drop the mod's force-aim bit (before any put-away, when the hand empties, when a gate closes). With the weapon still in hand the
    // hold-mode timer is restarted first: bit1's branch zeroes +0x3e0 (0x269f6cc..0x269f6dc), so clearing bit1 alone would let the
    // RDR1-mode timeout holster on the very next UpdateWeaponSelection.
    void dropForceAim(const Sample& s,bool stillInHand) noexcept {
        if(!forceOurs_)return;
        if(stillInHand&&api_.setHoldMode&&keepDrawn.load(std::memory_order_relaxed)){
            api_.setHoldMode(reinterpret_cast<void*>(s.player),false,2);keepCalls.fetch_add(1,std::memory_order_relaxed);}
        if(api_.setForceAim)api_.setForceAim(reinterpret_cast<const void*>(s.player),false);
        forceOurs_=false;forceAimOurs.store(false,std::memory_order_relaxed);forceAimCleared.fetch_add(1,std::memory_order_relaxed);
    }
    // Every successful poll (every frame while anything is in hand): forensics of engine put-aways, hold-mode keep, force aim.
    void weaponKeep(const Sample& s,const char* reason,std::uint64_t now) noexcept {
        const int hold=holdHand.load(std::memory_order_relaxed);
        if(prevHand_&&!s.handItem&&prevHold_>=0&&!expected_){ // the mod's own PutAwayWeapon zeroes prevHand_ (attempt)
            EngineHolster e{};e.ns=now;e.buttons=lastPadButtons.load(std::memory_order_relaxed);e.rt=lastPadRt.load(std::memory_order_relaxed);
            e.lt=lastPadLt.load(std::memory_order_relaxed);e.holdTrigger=holdTriggerValue.load(std::memory_order_relaxed);
            e.timer=prevTimer_;e.aimBits=prevAimBits_;e.state=prevState_;e.hand=prevHold_;e.ours=prevOurs_;e.forced=forceOurs_;
            e.invFlags=s.invFlags;e.gate=prevGate_?prevGate_:"OK";
            if(prevAimBits_>=0&&(prevAimBits_&aimBitRdr1)&&!(prevAimBits_&aimBitForce)&&prevTimer_<=.1f){e.kind="HOLD_MODE_TIMEOUT";engineHolstersTimer.fetch_add(1,std::memory_order_relaxed);}
            else if(e.buttons||e.rt>.5f||e.lt>.5f){e.kind="PAD_INPUT";engineHolstersInput.fetch_add(1,std::memory_order_relaxed);}
            else if(s.invFlags&(forceHolsterSoft|forceHolsterHard)){e.kind="FORCE_HOLSTER";engineHolstersForced.fetch_add(1,std::memory_order_relaxed);}
            else if(prevGate_){e.kind="GATED";engineHolstersGated.fetch_add(1,std::memory_order_relaxed);}
            else{e.kind="OTHER";engineHolstersOther.fetch_add(1,std::memory_order_relaxed);}
            engineHolsters.fetch_add(1,std::memory_order_relaxed);{std::lock_guard lock(engineHolsterMutex);lastEngineHolster=e;}
            { // GONE = the engine put the VR-held item away itself (slot = where it hangs now); a game draw soon after goes to this hand
                int gk=-1;for(int k=0;k<slotCount&&gk<0;++k)if(s.rawItems[std::size_t(k)]==prevHand_)gk=k;
                noteEvent(EvGone,gk,prevHold_,e.kind,now,std::uint32_t(prevAimBits_&0xff)|(std::uint32_t(prevState_&0xff)<<8)|(prevOurs_?1u<<16:0u));
                if(prevOurs_){recentHand_=prevHold_;recentNs_=now;}
            }
            if(prevSpinKept_||spin::keeps(prevHold_)){spin::bump(spin::stats.engineHolsterWhileKept); // The engine put the spinning gun away itself
                spin::lastGoneAimBits.store(prevAimBits_,std::memory_order_relaxed);spin::lastGoneTimer.store(prevTimer_,std::memory_order_relaxed);}
        }
        const bool inHand=s.handItem&&hold>=0;
        if(forceOurs_&&!(s.aimBits&aimBitForce)){forceOurs_=false;forceAimOurs.store(false,std::memory_order_relaxed);forceAimLostExternally.fetch_add(1,std::memory_order_relaxed);}
        const bool usable=config().enabled&&!reason&&inHand;
        void* const player=reinterpret_cast<void*>(s.player);
        // The keep (hold-mode timer restart, the same call SelectItem makes on every draw) runs whenever the VR hand holds
        // the item in game, also behind a closed context gate; only "not in game / not the player / dead" skip it.
        const bool keepable=config().enabled&&inHand&&s.gameState==3&&s.playerFlags&&s.alive;
        // SPIN: while the spin keeps the holding hand the keep runs on EVERY poll (also under force aim): the engine item stays drawn
        const bool spinKept=inHand&&spin::keeps(hold);
        if(keepable&&(!forceOurs_||spinKept)&&api_.setHoldMode&&keepDrawn.load(std::memory_order_relaxed)){api_.setHoldMode(player,false,2);keepCalls.fetch_add(1,std::memory_order_relaxed);
            if(reason)keepWhileGated.fetch_add(1,std::memory_order_relaxed);if(spinKept)spin::bump(spin::stats.keepWhileSpin);}
        const auto mode=ForceAim(forceAimMode.load(std::memory_order_relaxed));
        bool want=usable&&api_.setForceAim&&mode!=ForceAim::Off&&heldFirearm.load(std::memory_order_relaxed);
        if(want&&mode==ForceAim::Trigger){const auto t=holdTriggerNs.load(std::memory_order_relaxed);
            want=holdTriggerValue.load(std::memory_order_relaxed)>triggerActive||(t&&now>=t&&now-t<triggerAimHoldNs);
            // HAMMER: the free palm at the spur (FAN) wants aim like a trigger press
            const auto ha=hammerAimNs.load(std::memory_order_relaxed);if(!want&&ha&&now>=ha&&now-ha<triggerAimHoldNs)want=true;}
        // SPIN: the mod's force aim already set when
        // the spin took the hand is KEPT while it keeps the gun (bit1 disables both RDR1-mode auto-holsters); none is newly set (no aim stance)
        if(spinKept&&usable&&forceOurs_&&spin::config.keepAim){if(!want)spin::bump(spin::stats.aimKeptSpin);want=true;}
        else if(want&&spin::aimSuppressedFlag.load(std::memory_order_relaxed))want=false; // SPIN: no aim stance while the gun spins (back at the catch)
        // LASSO: the latched lasso aim request keeps force aim = keeps the rope
        // (sagPlayer::UpdateZoomState 0x269bd00 releases a catch the frame the bit drops); gated by `keepable` only (context gates, menu,
        // dropouts never drop it), never over a foreign bit1 (the foreign check below).
        if(!want&&keepable&&api_.setForceAim&&lasso::holstersActive()&&lasso::aimRequest())want=true;
        if(want&&!forceOurs_){
            if(s.aimBits&aimBitForce){if(!foreignSeen_){foreignSeen_=true;forceAimForeign.fetch_add(1,std::memory_order_relaxed);}} // a script owns it
            else{api_.setForceAim(player,true);forceOurs_=true;forceAimOurs.store(true,std::memory_order_relaxed);forceAimSet.fetch_add(1,std::memory_order_relaxed);}
        }else if(!want&&forceOurs_)dropForceAim(s,inHand);
        if(!(s.aimBits&aimBitForce))foreignSeen_=false;
        lasso::forceAimMirror.store(forceOurs_,std::memory_order_relaxed); // LASSO: trace / stuckAimFrames
        engineAimBits.store(s.aimBits,std::memory_order_relaxed);engineHoldTimer.store(s.holdTimer,std::memory_order_relaxed);
        engineSelectState.store(s.selectState,std::memory_order_relaxed);engineZoom.store(s.zoom,std::memory_order_relaxed);
        prevHand_=s.handItem;prevHold_=hold;prevOurs_=holdOurs.load(std::memory_order_relaxed);prevTimer_=s.holdTimer;prevAimBits_=s.aimBits;prevState_=s.selectState;
        prevGate_=reason;
        prevSpinKept_=spinKept;
    }
    bool prevSpinKept_{}; // The previous poll's holding hand was kept by the spin (engine put-away forensics)
    template<class T> bool get(std::uintptr_t base,std::size_t offset,T& out) const noexcept {
        return api_.read&&base&&base<=UINTPTR_MAX-offset-sizeof(T)&&api_.read(nullptr,base+offset,&out,sizeof(T));
    }
    bool block(std::uintptr_t base,void* out,std::size_t size) const noexcept {return api_.read&&base&&base<=UINTPTR_MAX-size&&api_.read(nullptr,base,out,size);}
    template<class T> static T at(const unsigned char* raw,std::size_t offset) noexcept {T v{};std::memcpy(&v,raw+offset,sizeof(T));return v;}
    bool tableIn(std::uintptr_t table,const std::array<std::uintptr_t,2>& set) const noexcept {return table&&(table==set[0]||table==set[1]);}
    void refuse(Refusal why,unsigned hand,std::uint64_t now) noexcept {
        refusals[why].fetch_add(1,std::memory_order_relaxed);lastRefusal.store(refusalNames[why],std::memory_order_relaxed);
        noteEvent(EvRefuse,cmdSlot_,int(hand&1u),refusalNames[why],now);
        haptics::pulse(hand&1u,haptics::refuseAmplitude,haptics::refuseUs,now,haptics::SourceRefuse);
    }
    void setHold(int hand,bool ours,std::uint32_t sequence) noexcept {
        holdOurs.store(ours,std::memory_order_relaxed);holdSequence.store(sequence,std::memory_order_relaxed);
        holdHand.store(hand,std::memory_order_release);holdGeneration.fetch_add(1,std::memory_order_relaxed);
    }
    void readName(std::uintptr_t type,AtomicText<32>& out) const noexcept {
        std::uintptr_t name{};char text[32]{};
        if(!type||!get(type,typeName,name)||!name){out.store(type?"?":"");return;}
        for(std::size_t n:{std::size_t(31),std::size_t(15),std::size_t(7)})if(block(name,text,n)){text[n]=0;out.store(text);return;}
        out.store("?");
    }
    // Reads only. Returns nullptr when player + inventory were sampled (flags in Sample), else the reason.
    const char* sample(Sample& s,bool fresh) noexcept {
        std::uint32_t gs{};if(!get(api_.gameData,0,gs))return "NO GAMEDATA";
        s.gameState=gs;
        std::uintptr_t table{};
        if(!get(api_.playerManager,0,table)||table!=api_.playerManagerTable||!get(api_.playerManager,0xa20,s.player)||!s.player)return "NO PLAYER";
        alignas(8) unsigned char p[playerBlock]{};
        if(!block(s.player,p,sizeof p)||at<std::uintptr_t>(p,0)!=api_.playerTable||p[playerActive]!=1)return "NO PLAYER";
        s.guid=at<std::uint32_t>(p,playerGuid);s.foot=at<std::uint32_t>(p,playerFoot);
        s.aimBits=p[playerAimBits];s.holdTimer=at<float>(p,playerHoldTimer);s.selectState=at<std::int32_t>(p,playerSelectState);s.zoom=at<std::int32_t>(p,playerZoom);
        s.locked=at<std::uint16_t>(p,playerLocks)||p[playerLock1]||p[playerLock2];
        std::uintptr_t manager{},slots{};std::uint16_t count{};
        if(!get(api_.actorManagerGlobal,0,manager)||!get(api_.actorSlotsGlobal,0,slots)||!slots||slots%8||!get(manager,0,count))return "NO ACTOR REGISTRY";
        const std::uintptr_t index=s.guid&0xffffu;std::uint8_t entry[16]{};
        if(index>=count||!block(slots+index*16,entry,sizeof entry))return "ACTOR GENERATION";
        s.actor=at<std::uintptr_t>(entry,0);if(!s.actor||at<std::uint16_t>(entry,8)!=(s.guid>>16))return "ACTOR GENERATION";
        alignas(8) unsigned char a[actorBlock]{};
        if(!block(s.actor,a,sizeof a))return "NO ACTOR";
        s.playerFlags=(at<std::uint32_t>(a,actorFlags)&3u)==3u;
        float hp{};const auto health=at<std::uintptr_t>(a,actorHealth);
        s.alive=health&&get(health,healthValue,hp)&&std::isfinite(hp)&&hp>0;
        const auto states=at<std::uintptr_t>(p,playerStates);std::uint32_t eye{};
        s.deadEye=states&&get(states,statesDeadEye,eye)&&eye!=0;
        s.inventory=at<std::uintptr_t>(a,actorInventory);
        alignas(8) unsigned char inv[invBlock]{};
        if(!s.inventory||!block(s.inventory,inv,sizeof inv)||at<std::uintptr_t>(inv,0)!=api_.inventoryTable||at<std::uintptr_t>(inv,invOwner)!=s.actor)return "NO INVENTORY";
        if(!inv[invReady])return "INVENTORY NOT READY";
        s.handItem=at<std::uintptr_t>(inv,invHand);s.invFlags=at<std::uint16_t>(inv,invFlags);s.slotEnabled=inv[invSlotMask];
        for(int k=0;k<slotCount;++k){
            const auto item=at<std::uintptr_t>(inv,invSlotItem+std::size_t(k)*invSlotStride);
            auto& c=slotCache_[std::size_t(k)];s.rawItems[std::size_t(k)]=item;
            if(!item){c=SlotCache{};
                // the item in the hand that last sat in slot k still belongs to slot k.
                if(s.handItem&&s.handItem==lastSlotItem_[std::size_t(k)]&&lastSlotType_[std::size_t(k)]){
                    s.items[std::size_t(k)]=s.handItem;s.types[std::size_t(k)]=lastSlotType_[std::size_t(k)];s.owned|=1u<<unsigned(k);s.handSlot=k;}
                continue;}
            if(fresh||c.item!=item||pollIndex_-c.poll>=30){
                c=SlotCache{};c.item=item;c.poll=pollIndex_;
                alignas(8) unsigned char it[itemBlock]{};std::uintptr_t typeTable{},data{};
                if(block(item,it,sizeof it)&&tableIn(at<std::uintptr_t>(it,0),api_.itemTables)&&at<std::uint32_t>(it,itemOwnerGuid)==s.guid){
                    c.type=at<std::uintptr_t>(it,itemType);
                    c.ok=c.type&&get(c.type,0,typeTable)&&tableIn(typeTable,api_.typeTables)&&get(c.type,typeWeaponData,data)&&data;
                }
            }
            if(!c.ok){
                if(s.handItem&&s.handItem==lastSlotItem_[std::size_t(k)]&&lastSlotType_[std::size_t(k)]){
                    s.items[std::size_t(k)]=s.handItem;s.types[std::size_t(k)]=lastSlotType_[std::size_t(k)];s.owned|=1u<<unsigned(k);s.handSlot=k;}
                continue;}
            s.items[std::size_t(k)]=item;s.types[std::size_t(k)]=c.type;s.owned|=1u<<unsigned(k);
            lastSlotItem_[std::size_t(k)]=item;lastSlotType_[std::size_t(k)]=c.type;
            if(item==s.handItem)s.handSlot=k;
        }
        return nullptr;
    }
    // A live "NewWeapToHand" effector on any of the 8 equip slots (GUIDs inv+0x428, checked like IsItemSwitching
    // 0x2662a90.. and StartItemChange 0x2666a50..: index < count, generation match, live object). GUID 0 = never set
    // (the constructor zeroes the array, 0x2661130). SelectItem itself copes with a running change (it reuses the
    // effector), so this only avoids stacking requests; an unreadable registry counts as busy.
    bool switching(const Sample& s) noexcept {
        std::uint32_t guids[8]{};std::uintptr_t manager{},slots{};std::uint16_t count{};
        if(!block(s.inventory+invEffectors,guids,sizeof guids)||!get(api_.gohManagerGlobal,0,manager)||!get(api_.gohSlotsGlobal,0,slots)||
           !slots||!get(manager,0,count))return true;
        for(const auto g:guids){
            if(!g)continue;
            const std::uintptr_t index=g&0xffffu;std::uint8_t entry[16]{};
            if(index<count&&block(slots+index*16,entry,sizeof entry)&&at<std::uintptr_t>(entry,0)&&at<std::uint16_t>(entry,8)==(g>>16))return true;
        }
        return false;
    }
    bool readBlocks(const Sample& s,PlayerBlocks& out) noexcept {
        contextReads.fetch_add(1,std::memory_order_relaxed);
        try{return api_.context&&api_.context(api_.contextUser,s.player,s.actor,s.guid,out);}catch(...){return false;}
    }
    // nullptr = the strict draw/put-away gate passes.
    Refusal gate(const Sample& s,const PlayerBlocks& b,int slot,bool draw) const noexcept {
        if(s.gameState!=3)return RefuseNotInGame;
        if(!s.playerFlags)return RefuseNoPlayer;
        if(!s.alive||!b.alive)return RefuseDead;
        if(s.locked||b.controlDisabled)return RefuseLocked;
        if(s.deadEye||b.deadEye)return RefuseDeadEye;  // holsters are inactive in Dead Eye (never cancel it)
        if(b.cover)return RefuseCover;
        if(b.ridingVehicle||b.vehicleTransition||(s.foot!=footOnFoot&&s.foot!=footHorse))return RefuseNotOnFoot;
        if(b.duel||b.mountedWeapon||b.mountingHorse||b.blindFiring||b.linkedAnimation||b.usingGringo)return RefuseContext;
        // LASSO: the lasso may be drawn in the saddle (the engine's CanDrawWeaponOfType only refuses it on a train)
        // long guns and thrown items too (RDR1 uses them from the saddle; the game's own CanDrawWeaponOfType still decides)
        if(draw&&(b.riding||s.foot==footHorse)&&slot!=SlotSidearm&&slot!=SlotSniper&&slot!=SlotLongarm&&slot!=SlotShotgun&&slot!=SlotThrown&&
           !(slot==SlotLasso&&lasso::holstersActive()))return RefuseMounted;
        if(draw&&(s.invFlags&forceHolsterHard))return RefuseForceHolster;
        return RefuseCount;
    }
    enum class Result {Done,Retry};
    // THROW: OpThrow refused / not possible -> acknowledged as not started (the brain's holster intent proceeds); a phantom that
    // was released is cleared to its holster right here (nothing ever stays in an open hand).
    Result refuseThrow(const mainthread::Command& c,unsigned hand,throwing::Refuse why,std::uint64_t now) noexcept {
        throwing::bump(throwing::refusals[why]);throwing::lastRefusal.store(throwing::refuseNames[why],std::memory_order_relaxed);
        throwing::ack.store(std::uint64_t(c.sequence)<<1,std::memory_order_release);
        if(c.arg[3]==1&&phantom_.item&&phantom_.hand==int(hand)){clearPhantom(ClearRelease);
            haptics::pulse(hand,haptics::returnAmplitude,haptics::returnUs,now,haptics::SourceReturn);}
        return Result::Done;
    }
    Result catchRefuse(unsigned hand,throwing::CatchRefuse why,std::uint64_t now) noexcept {
        throwing::bump(throwing::catchRefusals[why]);haptics::pulse(hand,haptics::refuseAmplitude,haptics::refuseUs,now,haptics::SourceRefuse);
        return Result::Done;
    }
    // THROW: the flying weapon's render rule (forced while it flies if its holster hides it).
    bool forceRenderFor(std::uintptr_t w) noexcept {
        std::uint32_t flags{};
        if(!api_.setShouldRender||!w||!get(w,weaponFlags,flags)||(flags&weaponRenderBit))return false;
        try{api_.setShouldRender(reinterpret_cast<void*>(w),true);}catch(...){return false;}
        throwing::bump(throwing::renderForcedCalls);return true;
    }
    Result attempt(const mainthread::Command& c,std::uint64_t now) noexcept {
        const unsigned hand=unsigned(c.arg[1])&1u;const int slot=c.arg[0];cmdSlot_=slot;
        // THROW: a draw of the thrown slot (its socket grabbed while the gun flies / lies) = recall: the flight ends first
        if((c.op==OpDraw||c.op==OpDrawKeep||c.op==OpPhantomSet)&&throwing::busy()&&slot==throwing::slot.load(std::memory_order_relaxed)&&
           throwing::currentState()!=throwing::StateCatching)throwing::machine.end(throwing::EndRecall,now);
        if(c.op==OpPhantomClear){ // DUAL: no engine call, needs no sample
            if(phantom_.item&&phantom_.hand==int(hand)){
                clearPhantom(c.arg[2]>=0&&unsigned(c.arg[2])<ClearCount?unsigned(c.arg[2]):unsigned(ClearRelease));
                haptics::pulse(hand,haptics::returnAmplitude,haptics::returnUs,now,haptics::SourceReturn);}
            return Result::Done;
        }
        if(c.op==OpThrow&&c.arg[3]==1){ // THROW of the DUAL phantom: no engine call (it is holstered for the engine already)
            namespace T=throwing;
            if(!T::active())return refuseThrow(c,hand,T::RefuseOff,now);
            if(T::busy())return refuseThrow(c,hand,T::RefuseBusy,now);
            if(!phantom_.item||phantom_.hand!=int(hand))return refuseThrow(c,hand,T::RefuseNotInHand,now);
            if(!T::slotThrowable(phantom_.slot))return refuseThrow(c,hand,T::RefuseNotSidearm,now);
            T::Prepared pr{};
            if(const auto r=T::machine.prepare({phantom_.weapon,phantom_.item,int(hand),true,now},std::uint32_t(c.arg[2]),pr);r!=T::RefuseCount)
                return refuseThrow(c,hand,r,now);
            const auto it=phantom_.item,w=phantom_.weapon;const int k=phantom_.slot;const bool forced=phantom_.renderForced;
            clearPhantom(ClearCount,false); // silent: the item flies; its render rule comes back when the flight ends
            T::ack.store((std::uint64_t(c.sequence)<<1)|1u,std::memory_order_release);
            T::machine.launch(pr,k,it,w,forced,now);return Result::Done;
        }
        Sample s{};
        if(const char* why=sample(s,true)){
            const bool notReady=!std::strcmp(why,"NO GAMEDATA")||!std::strcmp(why,"NO INVENTORY")||!std::strcmp(why,"INVENTORY NOT READY");
            if(c.op==OpThrow)return refuseThrow(c,hand,throwing::RefuseGate,now);
            if(c.op==OpCatch)return catchRefuse(hand,throwing::CatchGate,now);
            refuse(notReady?RefuseNotReady:RefuseNoPlayer,hand,now);return Result::Done;}
        PlayerBlocks b{};if(!readBlocks(s,b)){
            if(c.op==OpThrow)return refuseThrow(c,hand,throwing::RefuseGate,now);
            if(c.op==OpCatch)return catchRefuse(hand,throwing::CatchGate,now);
            refuse(RefuseContext,hand,now);return Result::Done;}
        if(c.op==OpThrow){ // THROW of the engine item: prepare (start pose) -> the HOLD put-away -> launch from its holster slot
            namespace T=throwing;
            if(!T::active())return refuseThrow(c,hand,T::RefuseOff,now);
            if(T::busy())return refuseThrow(c,hand,T::RefuseBusy,now);
            if(gate(s,b,SlotSidearm,false)!=RefuseCount)return refuseThrow(c,hand,T::RefuseGate,now);
            if(!s.handItem||holdHand.load(std::memory_order_relaxed)!=int(hand))return refuseThrow(c,hand,T::RefuseNotInHand,now);
            if(expected_&&now-expectedNs_<drawConfirmNs)return Result::Retry; // a draw is landing
            if(switching(s))return Result::Retry;
            if(s.handSlot>=0&&!T::slotThrowable(s.handSlot))return refuseThrow(c,hand,T::RefuseNotSidearm,now);
            const auto it=s.handItem,w=weaponOf(it);if(!w)return refuseThrow(c,hand,T::RefuseNoPlacement,now);
            T::Prepared pr{};
            if(const auto r=T::machine.prepare({w,it,int(hand),false,now},std::uint32_t(c.arg[2]),pr);r!=T::RefuseCount)return refuseThrow(c,hand,r,now);
            dropForceAim(s,false);lastOurPutAwayNs_=now;prevHand_=0; // exactly OpPutAway
            api_.putAwayWeapon(reinterpret_cast<void*>(s.player),reinterpret_cast<void*>(s.inventory));
            expected_=0;putAways.fetch_add(1,std::memory_order_relaxed);noteEvent(EvPutAway,s.handSlot,int(hand),"THROW",now);
            Sample after{};int k=-1;
            if(!sample(after,true))for(int j=0;j<slotCount;++j)if(after.rawItems[std::size_t(j)]==it){k=j;break;}
            if(k<0||after.handItem==it||!T::slotThrowable(k)){ // not where a holstered item lives: it is simply put away
                T::bump(T::refusals[T::RefuseNoSlot]);T::lastRefusal.store(T::refuseNames[T::RefuseNoSlot],std::memory_order_relaxed);
                T::ack.store(std::uint64_t(c.sequence)<<1,std::memory_order_release);
                haptics::pulse(hand,haptics::returnAmplitude,haptics::returnUs,now,haptics::SourceReturn);return Result::Done;}
            const bool forced=forceRenderFor(w);
            T::ack.store((std::uint64_t(c.sequence)<<1)|1u,std::memory_order_release);
            T::machine.launch(pr,k,it,w,forced,now);
            // DUAL: the other hand's phantom becomes the engine item (as the HOLD release promote); refused = it stays the phantom
            bool promoted=false;
            if(phantom_.item&&phantom_.hand>=0&&phantom_.hand!=int(hand)&&dualActive()&&phantom_.slot>=0&&phantom_.slot<slotCount&&
               after.items[std::size_t(phantom_.slot)]==phantom_.item){
                const auto pit=phantom_.item;const auto type=after.types[std::size_t(phantom_.slot)];const int ph=phantom_.hand;
                const std::uint32_t seq=phantom_.sequence?phantom_.sequence:holdSequence.load(std::memory_order_relaxed);
                if(api_.canDraw(reinterpret_cast<const void*>(after.player),reinterpret_cast<const void*>(type))&&
                   api_.selectItem(reinterpret_cast<void*>(after.inventory),reinterpret_cast<void*>(pit),true,api_.animatedDraw,true)){
                    clearPhantom(ClearPromote,false);promotes.fetch_add(1,std::memory_order_relaxed);T::bump(T::promotes);
                    expected_=pit;expectedNs_=now;setHold(ph,true,seq);draws.fetch_add(1,std::memory_order_relaxed);promoted=true;}
            }
            if(!promoted){setHold(-1,false,0);lastHandItem_=after.handItem;}
            return Result::Done;
        }
        if(c.op==OpCatch){ // THROW: an empty hand closed on the flying / lying gun = a draw of its slot into that hand
            namespace T=throwing;
            const int st=T::currentState();const int k=T::slot.load(std::memory_order_relaxed);const auto it=T::item.load(std::memory_order_relaxed);
            if(!T::active()||(st!=T::StateFlying&&st!=T::StateResting)||std::uint32_t(c.arg[3])!=(T::generation.load(std::memory_order_relaxed)&0x7fffffffu)||
               k<0||k>=slotCount)return catchRefuse(hand,T::CatchState,now);
            if(const auto r=gate(s,b,k,true);r!=RefuseCount){refusals[r].fetch_add(1,std::memory_order_relaxed);lastRefusal.store(refusalNames[r],std::memory_order_relaxed);
                return catchRefuse(hand,T::CatchGate,now);}
            if(!(s.slotEnabled>>unsigned(k)&1u)||s.items[std::size_t(k)]!=it)return catchRefuse(hand,T::CatchSlot,now);
            if(holdHand.load(std::memory_order_relaxed)==int(hand)&&s.handItem)return catchRefuse(hand,T::CatchBusy,now);
            const auto counted=[&]{const int kind=c.arg[2];T::bump(kind==1?T::catchesClosed:kind==2?T::catchesGround:T::catchesAir);
                haptics::pulse(hand,kind==1?.9f:.8f,kind==1?60'000u:45'000u,now,haptics::SourceGrab);};
            if(s.handItem==it){ // already in hand (should not happen while it flies): only the holder changes
                expected_=0;lastHandItem_=it;setHold(int(hand),true,c.sequence);T::machine.catching(int(hand),true,now);counted();return Result::Done;}
            if(switching(s))return Result::Retry;
            if(!api_.canDraw(reinterpret_cast<const void*>(s.player),reinterpret_cast<const void*>(s.types[std::size_t(k)])))return catchRefuse(hand,T::CatchGameRefused,now);
            // DUAL: the other hand's engine item stays in that hand as the phantom ("live = last taken")
            Phantom next{};bool keepIt=false;const int other=holdHand.load(std::memory_order_relaxed);
            if(dualActive()&&other>=0&&other!=int(hand)&&s.handItem&&s.handSlot>=0&&phantomableSlot(s.handSlot)){
                if(const auto w=weaponOf(s.handItem)){next=Phantom{s.handSlot,other,s.handItem,w,holdSequence.load(std::memory_order_relaxed),now,false};keepIt=true;}}
            if(!api_.selectItem(reinterpret_cast<void*>(s.inventory),reinterpret_cast<void*>(it),true,false,true))return catchRefuse(hand,T::CatchSelectFailed,now);
            if(keepIt){if(phantom_.item)clearPhantom(ClearReplace);publishPhantom(next.slot,next.item,next.weapon,next.hand,next.sequence,now);}
            else if(phantom_.item&&phantom_.hand==int(hand))clearPhantom(ClearReplace);
            std::uintptr_t inHand{};const bool landed=get(s.inventory,invHand,inHand)&&inHand==it;
            expected_=it;expectedNs_=now;setHold(int(hand),true,c.sequence);draws.fetch_add(1,std::memory_order_relaxed);
            noteEvent(EvDrawn,k,int(hand),"CATCH",now);
            T::machine.catching(int(hand),landed,now);counted();
            return Result::Done;
        }
        if(c.op==OpDraw){
            if(slot<0||slot>=slotCount){refuse(RefuseEmptySlot,hand,now);return Result::Done;}
            if(const auto r=gate(s,b,slot,true);r!=RefuseCount){refuse(r,hand,now);return Result::Done;}
            if(!(s.slotEnabled>>unsigned(slot)&1u)){refuse(RefuseSlotDisabled,hand,now);return Result::Done;}
            const auto item=s.items[std::size_t(slot)],type=s.types[std::size_t(slot)];
            if(!item){refuse(s.rawItems[std::size_t(slot)]?RefuseNotWeapon:RefuseEmptySlot,hand,now);return Result::Done;}
            if(item==s.handItem){ // already in hand (e.g. drawn by the wheel): only the holding hand changes
                expected_=0;lastHandItem_=s.handItem;setHold(int(hand),true,c.sequence);alreadyInHand.fetch_add(1,std::memory_order_relaxed);
                noteEvent(EvDrawn,slot,int(hand),"ALREADY_IN_HAND",now);
                haptics::pulse(hand,haptics::grabAmplitude,haptics::grabUs,now,haptics::SourceGrab);return Result::Done;
            }
            if(switching(s))return Result::Retry;
            if(!api_.canDraw(reinterpret_cast<const void*>(s.player),reinterpret_cast<const void*>(type))){refuse(RefuseGameRefused,hand,now);return Result::Done;}
            if(slot==SlotLasso&&lasso::holstersActive()){ // LASSO DUAL rule 1: the lasso draw puts any phantom back to its holster first
                if(phantom_.item)clearPhantom(ClearReplace);
                if(s.foot==footHorse||(blocksValid_&&blocks_.riding))lasso::bump(lasso::stats.mountedLassoGrabs);}
            if(!api_.selectItem(reinterpret_cast<void*>(s.inventory),reinterpret_cast<void*>(item),true,api_.animatedDraw,true)){refuse(RefuseSelectFailed,hand,now);return Result::Done;}
            expected_=item;expectedNs_=now;setHold(int(hand),true,c.sequence);draws.fetch_add(1,std::memory_order_relaxed);
            noteEvent(EvDrawn,slot,int(hand),"SELECT",now);
            haptics::pulse(hand,haptics::grabAmplitude,haptics::grabUs,now,haptics::SourceGrab);
            return Result::Done;
        }
        if(c.op==OpPutAway){
            // SPIN: a put-away queued for the hand whose gun the spin keeps on the finger is dropped (its exits post after LEAVING)
            if(holdHand.load(std::memory_order_relaxed)==int(hand)&&spin::keeps(int(hand))){spin::bump(spin::stats.supExecutor);return Result::Done;}
            if(expected_&&now-expectedNs_<drawConfirmNs)return Result::Retry; // let a just-started draw land first (HOLD tap)
            if(!s.handItem){putAwayEmpty.fetch_add(1,std::memory_order_relaxed);noteEvent(EvPutAway,slot,int(hand),"ALREADY_EMPTY",now);return Result::Done;}
            if(const auto r=gate(s,b,slot,false);r!=RefuseCount){refuse(r,hand,now);return Result::Done;}
            if(switching(s))return Result::Retry;
            dropForceAim(s,false);lastOurPutAwayNs_=now;prevHand_=0; // Force-aim bit1 never outlives the weapon (auto-draw, 0x269fb48)
            api_.putAwayWeapon(reinterpret_cast<void*>(s.player),reinterpret_cast<void*>(s.inventory));
            expected_=0;putAways.fetch_add(1,std::memory_order_relaxed);
            noteEvent(EvPutAway,s.handSlot,int(hand),c.arg[2]==PutAwayHoldRelease?"HOLD_RELEASE":"SLOT_PRESS",now);
            haptics::pulse(hand,haptics::returnAmplitude,haptics::returnUs,now,haptics::SourceReturn);
            return Result::Done;
        }
        const auto give=[&](unsigned taker,int giver){ // transfer feel: taker 0.6/40 ms, giver 0.25/15 ms
            haptics::pulse(taker,haptics::grabAmplitude,haptics::grabUs,now,haptics::SourceGrab);
            if((giver==0||giver==1)&&unsigned(giver)!=taker)haptics::pulse(unsigned(giver),haptics::enterAmplitude,haptics::enterUs,now,haptics::SourceReturn);};
        const auto alive=[&]()->bool{ // transfers / phantom moves: only "not in game / not the player / dead" refuse (no draw gates)
            if(s.gameState!=3||!s.playerFlags){refuse(RefuseNotInGame,hand,now);return false;}
            if(!s.alive||!b.alive){refuse(RefuseDead,hand,now);return false;}
            return true;};
        if(c.op==OpTransfer){ // the engine item changes hands; no engine call (the pose hook follows holdHand)
            if(!alive()){transfersRefused.fetch_add(1,std::memory_order_relaxed);return Result::Done;}
            if(expected_&&s.handItem!=expected_&&now-expectedNs_<drawConfirmNs)return Result::Retry; // a draw is landing
            if(!s.handItem){dualRefuse(DualNotInHand,hand,now);transfersRefused.fetch_add(1,std::memory_order_relaxed);return Result::Done;}
            if(phantom_.item&&phantom_.hand==int(hand)){dualRefuse(DualHandBusy,hand,now);transfersRefused.fetch_add(1,std::memory_order_relaxed);return Result::Done;}
            expected_=0;lastHandItem_=s.handItem;setHold(int(hand),true,c.sequence);transfersDone.fetch_add(1,std::memory_order_relaxed);
            noteEvent(EvSwap,s.handSlot,int(hand),"TRANSFER",now);
            give(hand,c.arg[3]);return Result::Done;
        }
        if(c.op==OpPhantomSet){
            if(!dualActive()){dualRefuse(DualOff,hand,now);return Result::Done;}
            if(slot<0||slot>=slotCount){refuse(RefuseEmptySlot,hand,now);return Result::Done;}
            const auto item=s.items[std::size_t(slot)];
            const bool engineHere=holdHand.load(std::memory_order_relaxed)==int(hand)&&s.handItem;
            if(item&&item==phantom_.item){ // the phantom changes hands
                if(!alive()){transfersRefused.fetch_add(1,std::memory_order_relaxed);return Result::Done;}
                if(engineHere){dualRefuse(DualHandBusy,hand,now);transfersRefused.fetch_add(1,std::memory_order_relaxed);return Result::Done;}
                publishPhantom(phantom_.slot,item,phantom_.weapon,int(hand),c.sequence,now);phantomMoves.fetch_add(1,std::memory_order_relaxed);
                if(c.arg[2]&1)transfersDone.fetch_add(1,std::memory_order_relaxed);
                give(hand,c.arg[3]);return Result::Done;
            }
            if(const auto r=gate(s,b,slot,true);r!=RefuseCount){refuse(r,hand,now);return Result::Done;}
            if(!(s.slotEnabled>>unsigned(slot)&1u)){refuse(RefuseSlotDisabled,hand,now);return Result::Done;}
            if(!item){refuse(s.rawItems[std::size_t(slot)]?RefuseNotWeapon:RefuseEmptySlot,hand,now);return Result::Done;}
            if(c.arg[2]&1){dualRefuse(DualNotInHand,hand,now);transfersRefused.fetch_add(1,std::memory_order_relaxed);return Result::Done;} // a move, the phantom is gone
            if(!phantomableSlot(slot)){dualRefuse(DualNotPhantomable,hand,now);return Result::Done;}
            if(item==s.handItem){ // the engine already holds it: only the holding hand changes (as OpDraw)
                if(phantom_.hand==int(hand))clearPhantom(ClearReplace);
                expected_=0;lastHandItem_=s.handItem;setHold(int(hand),true,c.sequence);alreadyInHand.fetch_add(1,std::memory_order_relaxed);
                give(hand,-1);return Result::Done;
            }
            if(engineHere){dualRefuse(DualHandBusy,hand,now);return Result::Done;}
            const auto weapon=weaponOf(item);if(!weapon){dualRefuse(DualNoWeapon,hand,now);return Result::Done;}
            if(phantom_.item)clearPhantom(ClearReplace);
            publishPhantom(slot,item,weapon,int(hand),c.sequence,now);phantomSets.fetch_add(1,std::memory_order_relaxed);
            noteEvent(EvDrawn,slot,int(hand),"PHANTOM_SET",now);
            give(hand,-1);return Result::Done;
        }
        if(c.op==OpDrawKeep){ // a draw that keeps the previous engine item as the phantom (keep hand) / promotes the phantom
            const int keep=c.arg[3];const bool fire=(c.arg[2]&1)!=0,releaseFallback=(c.arg[2]&2)!=0;
            if(slot<0||slot>=slotCount){refuse(RefuseEmptySlot,hand,now);return Result::Done;}
            const auto item=s.items[std::size_t(slot)],type=s.types[std::size_t(slot)];
            const bool fromPhantom=phantom_.item&&item==phantom_.item;
            // Promotion refused (the engine hand opened / took its own socket): holster the engine item anyway, the phantom stays.
            const auto fallback=[&](Refusal why){refuse(why,hand,now);
                if(!releaseFallback||!s.handItem||switching(s))return Result::Done;
                if(gate(s,b,slot,false)!=RefuseCount)return Result::Done;
                dropForceAim(s,false);lastOurPutAwayNs_=now;prevHand_=0;
                api_.putAwayWeapon(reinterpret_cast<void*>(s.player),reinterpret_cast<void*>(s.inventory));
                expected_=0;putAways.fetch_add(1,std::memory_order_relaxed);noteEvent(EvPutAway,s.handSlot,int(hand),"PROMOTE_FALLBACK",now);return Result::Done;};
            if(const auto r=gate(s,b,slot,true);r!=RefuseCount)return fallback(r);
            if(!(s.slotEnabled>>unsigned(slot)&1u))return fallback(RefuseSlotDisabled);
            if(!item)return fallback(s.rawItems[std::size_t(slot)]?RefuseNotWeapon:RefuseEmptySlot);
            if(item==s.handItem){ // already the engine item: only the holding hand changes
                if(phantom_.hand==int(hand)&&!fromPhantom)clearPhantom(ClearReplace);
                expected_=0;lastHandItem_=s.handItem;setHold(int(hand),true,c.sequence);alreadyInHand.fetch_add(1,std::memory_order_relaxed);
                give(hand,-1);return Result::Done;
            }
            if(switching(s))return Result::Retry;
            if(!api_.canDraw(reinterpret_cast<const void*>(s.player),reinterpret_cast<const void*>(type)))return fallback(RefuseGameRefused);
            Phantom next{};bool keepIt=false; // the current engine item stays in the keep hand as the phantom
            if(dualActive()&&(keep==0||keep==1)&&keep!=int(hand)&&s.handItem&&s.handSlot>=0&&phantomableSlot(s.handSlot)){
                if(const auto w=weaponOf(s.handItem)){next=Phantom{s.handSlot,keep,s.handItem,w,holdSequence.load(std::memory_order_relaxed),now,false};keepIt=true;}}
            const std::uint32_t sequence=fromPhantom&&phantom_.sequence?phantom_.sequence:c.sequence; // the item keeps its hand's press
            if(!api_.selectItem(reinterpret_cast<void*>(s.inventory),reinterpret_cast<void*>(item),true,api_.animatedDraw,true))return fallback(RefuseSelectFailed);
            if(fromPhantom){clearPhantom(keepIt?unsigned(ClearCount):unsigned(ClearPromote),false);if(!keepIt)promotes.fetch_add(1,std::memory_order_relaxed);}
            else if(phantom_.item&&(phantom_.hand==int(hand)||keepIt))clearPhantom(ClearReplace);
            if(keepIt)publishPhantom(next.slot,next.item,next.weapon,next.hand,next.sequence,now);
            expected_=item;expectedNs_=now;setHold(int(hand),true,sequence);draws.fetch_add(1,std::memory_order_relaxed);drawKeeps.fetch_add(1,std::memory_order_relaxed);
            noteEvent(fire&&fromPhantom?EvSwap:EvDrawn,slot,int(hand),fire&&fromPhantom?"SWAP_FIRE":fromPhantom?"PROMOTE":"DRAW_KEEP",now,keepIt?1u:0u);
            if(fire&&fromPhantom){swapSeq_=c.sequence;swapStartNs_=now;swapItem_=item;swapsDone.fetch_add(1,std::memory_order_relaxed);}
            haptics::pulse(hand,haptics::grabAmplitude,haptics::grabUs,now,haptics::SourceGrab);
            return Result::Done;
        }
        return Result::Done;
    }
    void run(const mainthread::Command& c,std::uint64_t now) noexcept {
        try{
            if(attempt(c,now)==Result::Retry){
                const std::uint64_t limit=c.op==OpPutAway?drawConfirmNs+busyRetryNs:busyRetryNs;
                if(now>c.postedNs&&now-c.postedNs>limit){hasPending_=false;refuse(RefuseBusy,unsigned(c.arg[1])&1u,now);}
                else{if(!hasPending_||pending_.sequence!=c.sequence)retries.fetch_add(1,std::memory_order_relaxed);pending_=c;hasPending_=true;}
            }else if(hasPending_&&pending_.sequence==c.sequence)hasPending_=false;
        }catch(...){hasPending_=false;refuse(RefuseException,unsigned(c.arg[1])&1u,now);}
    }
public:
    void configure(const EngineApi& api) noexcept {*this=EngineSide{};api_=api;
        phantomHand.store(-1);phantomSlot.store(-1);phantomItem.store(0);phantomWeapon.store(0);phantomSequence.store(0);phantomRenderForced.store(false);
        keepApiReady.store(api.setHoldMode!=nullptr,std::memory_order_relaxed);forceAimApiReady.store(api.setForceAim!=nullptr,std::memory_order_relaxed);}
    bool configured() const noexcept {return bool(api_);}
    void tick(const mainthread::Frame& frame) noexcept {
        if(!api_)return;
        const bool busy=expected_||hasPending_||forceOurs_||prevHand_||holdHand.load(std::memory_order_relaxed)>=0||phantom_.item|| // Every frame while held
            throwing::busy(); // Every frame while the gun flies / lies / returns
        if(!busy&&!config().enabled&&(frame.index%30u))return;  // menu OFF: keep the report fresh only
        if(!busy&&(frame.index&1u))return;                        // every 2nd frame when idle (guarded reads are syscalls)
        polls.fetch_add(1,std::memory_order_relaxed);++pollIndex_;
        Sample s{};const char* why=nullptr;
        try{why=sample(s,false);}catch(...){why="EXCEPTION";}
        if(why){
            pollFailures.fetch_add(1,std::memory_order_relaxed);inventoryValid.store(false,std::memory_order_relaxed);
            blockReason.store(why,std::memory_order_relaxed);contextAllowed.store(false,std::memory_order_relaxed);
            ownedMask.store(0,std::memory_order_relaxed);grabMask.store(0,std::memory_order_relaxed);currentSlot.store(-1,std::memory_order_relaxed);
            if(const int hh=holdHand.load(std::memory_order_relaxed);hh>=0&&!expected_){ // Remember the hold across the gap
                gap_=Gap{hh,holdOurs.load(std::memory_order_relaxed),holdSequence.load(std::memory_order_relaxed),lastHandItem_,frame.now};
                if(gap_.ours){recentHand_=hh;recentNs_=frame.now;}
                noteEvent(EvGone,-1,hh,"POLL_FAIL",frame.now);setHold(-1,false,0);lastHandItem_=0;}
            prevHand_=0;prevHold_=-1; // No put-away forensics across an unreadable gap
            if(phantom_.item)clearPhantom(ClearContext,false); // DUAL: inventory unreadable (load, death screen, ...)
            {lasso::PlayerLink pl{};pl.ns=frame.now;lasso::playerBox.store(pl);} // LASSO: no player this poll
            {InventoryLink il{};il.ns=frame.now;inventoryBox.store(il);} // RELOAD: no inventory this poll
            throwing::machine.observeInvalid(frame.now); // Nothing to keep in the air - the gun is in its holster
        }else{
            // Expensive context (cover, duel, mounted gun, ...) every 10th poll; cheap bits every poll.
            if(!blocksValid_||pollIndex_%10u==1u){PlayerBlocks b{};blocksValid_=readBlocks(s,b);if(blocksValid_)blocks_=b;}
            const char* reason=nullptr;
            if(s.gameState!=3)reason="NOT IN GAME";else if(!s.playerFlags)reason="NOT PLAYER";else if(!s.alive)reason="DEAD";
            else if(s.locked)reason="CONTROL LOCKED";else if(s.deadEye)reason="DEAD EYE";else if(!blocksValid_)reason="CONTEXT UNAVAILABLE";
            else if(const auto r=gate(s,blocks_,SlotSidearm,false);r!=RefuseCount)reason=refusalNames[r];
            std::uint32_t grab=s.owned&s.slotEnabled;
            // LASSO: mounted, the lasso socket stays grabbable next to the sidearm
            // (user: a rifle could not be taken on a horse - the grip went to the game, which drew the revolver): RDR1 fires long guns
            // and throws from the saddle, so the back slots and THROWN are grabbable mounted too (the knife stays on foot).
            if(s.foot==footHorse||blocks_.riding)grab&=(1u<<SlotSidearm)|(1u<<SlotSniper)|(1u<<SlotLongarm)|(1u<<SlotShotgun)|(1u<<SlotThrown)|
                (lasso::holstersActive()?1u<<SlotLasso:0u);
            if(s.invFlags&forceHolsterHard)grab=0;
            contextAllowed.store(!reason,std::memory_order_relaxed);blockReason.store(reason?reason:"OK",std::memory_order_relaxed);
            ownedMask.store(s.owned,std::memory_order_relaxed);grabMask.store(reason?0u:grab,std::memory_order_relaxed);
            currentSlot.store(s.handItem?s.handSlot:-1,std::memory_order_relaxed);inventoryValid.store(true,std::memory_order_relaxed);
            for(int k=0;k<slotCount;++k)if(namedItems_[std::size_t(k)]!=s.items[std::size_t(k)]){
                namedItems_[std::size_t(k)]=s.items[std::size_t(k)];readName(s.types[std::size_t(k)],slotNames[std::size_t(k)]);}
            if(namedHand_!=s.handItem){namedHand_=s.handItem;std::uintptr_t type{};
                if(s.handItem&&get(s.handItem,itemType,type))readName(type,handName);else handName.store(s.handItem?"?":"");}
            // Which physical hand holds the item: the brain's choice until the game changes it; the game's own draws are the right hand.
            bool settled=true;
            if(expected_){
                if(s.handItem==expected_){expected_=0;drawsConfirmed.fetch_add(1,std::memory_order_relaxed);lastHandItem_=s.handItem;
                    noteEvent(EvDrawn,s.handSlot,holdHand.load(std::memory_order_relaxed),"CONFIRMED",frame.now);
                    if(swapSeq_&&s.handItem==swapItem_){ // DUAL swap-fire landed: the brain forwards the trigger now
                        const auto dt=frame.now>swapStartNs_?frame.now-swapStartNs_:0;swapLatencyNsSum.fetch_add(dt,std::memory_order_relaxed);
                        auto m=swapLatencyNsMax.load(std::memory_order_relaxed);while(dt>m&&!swapLatencyNsMax.compare_exchange_weak(m,dt)){}
                        swapsLanded.fetch_add(1,std::memory_order_relaxed);swapLanded.store(swapSeq_,std::memory_order_release);swapSeq_=0;}}
                else if(frame.now-expectedNs_>drawConfirmNs){ // the draw never landed: re-derive the holder from the game state
                    expected_=0;lastHandItem_=~std::uintptr_t(0);refuse(RefuseNotDrawn,unsigned(holdHand.load()>0),frame.now);
                    if(swapSeq_){swapSeq_=0;swapTimeouts.fetch_add(1,std::memory_order_relaxed);}}
                else settled=false;
            }
            if(settled&&s.handItem!=lastHandItem_){
                lastHandItem_=s.handItem;
                if(!s.handItem)setHold(-1,false,0);
                else if(gap_.item&&s.handItem==gap_.item&&frame.now>=gap_.ns&&frame.now-gap_.ns<gapRestoreNs){ // The same item after an
                    // unreadable poll = the same hold (hand + press); was re-adopted as a game draw into the RIGHT hand (open: holstered 1.5 s later)
                    setHold(gap_.hand,gap_.ours,gap_.seq);gapRestores.fetch_add(1,std::memory_order_relaxed);
                    noteEvent(EvSwap,s.handSlot,gap_.hand,"GAP_RESTORE",frame.now);
                }else{ // The game's own draw is adopted into our HOLD (gameDrawSequenceFlag): opening that hand holsters it
                    const bool adopt=adoptGameDraws.load(std::memory_order_relaxed)&&config().enabled;
                    // into the hand that held a weapon just before (the engine swapped / re-drew it), else the right hand (wheel, script)
                    const int was=holdHand.load(std::memory_order_relaxed);int into=1;
                    if(was==0||was==1)into=was;else if(recentHand_>=0&&recentHand_<=1&&frame.now>=recentNs_&&frame.now-recentNs_<recentHandNs)into=recentHand_;
                    if(into!=1)adoptPrevHand.fetch_add(1,std::memory_order_relaxed);
                    setHold(into,adopt,adopt?(gameDrawSequenceFlag|(++gameDrawCounter_&0x7fffffffu)):0u);gameDraws.fetch_add(1,std::memory_order_relaxed);
                    if(adopt)gameDrawsAdopted.fetch_add(1,std::memory_order_relaxed);
                    noteEvent(EvAdopt,s.handSlot,into,adopt?"GAME_DRAW":"GAME_DRAW_NOT_OURS",frame.now,was>=0?1u:0u);
                    if(phantom_.item)clearPhantom(ClearGameDraw,s.handItem!=phantom_.item);} // DUAL: wheel / script changed the item
                gap_.item=0;recentHand_=-1;
            }else if(settled&&!s.handItem&&holdHand.load(std::memory_order_relaxed)>=0)setHold(-1,false,0);
            if(phantom_.item)checkPhantom(s,frame.now);
            weaponKeep(s,reason,frame.now);
            { // LASSO: the player / inventory facts the lasso tick (ModuleLasso, later this run) builds on (no second player chain read)
                lasso::PlayerLink pl{};pl.ns=frame.now;pl.player=s.player;pl.actor=s.actor;pl.inventory=s.inventory;pl.handItem=s.handItem;
                pl.slotItem=s.items[std::size_t(SlotLasso)];pl.guid=s.guid;pl.foot=s.foot;pl.gameState=s.gameState;pl.alive=s.alive;pl.playerFlags=s.playerFlags;
                pl.owned=(s.owned>>unsigned(SlotLasso))&1u;pl.inHand=s.handItem&&s.handSlot==SlotLasso;pl.riding=s.foot==footHorse||(blocksValid_&&blocks_.riding);
                pl.valid=1;lasso::playerBox.store(pl);
            }
            { // RELOAD: the inventory facts for the reload executor (ops / ammo reads / the holster refill timer)
                InventoryLink il{};il.ns=frame.now;il.player=s.player;il.actor=s.actor;il.inventory=s.inventory;il.handItem=s.handItem;il.items=s.rawItems;il.guid=s.guid;il.valid=1;
                inventoryBox.store(il);
            }
            if(throwing::busy()){ // THROW: inventory invariants of the flight (draw by the game, slot change, context)
                const int tk=throwing::slot.load(std::memory_order_relaxed);const auto& bb=blocks_;lastThrownSlot_=tk;
                throwing::Facts f{};f.valid=true;f.handItem=s.handItem;
                f.slotItem=tk>=0&&tk<slotCount?s.rawItems[std::size_t(tk)]:0;f.slotWeapon=weaponOf(f.slotItem);
                f.context=s.gameState==3&&s.playerFlags&&s.alive&&!s.locked&&s.foot==footOnFoot&&!(blocksValid_&&(bb.riding||bb.controlDisabled||bb.ridingVehicle||
                    bb.vehicleTransition||bb.duel||bb.mountedWeapon||bb.mountingHorse||bb.linkedAnimation||bb.usingGringo));
                throwing::machine.observe(f,frame.now);
            }
            if(const auto r=throwing::machine.takeRefresh();r&&r!=s.handItem&&api_.refreshRender){ // The flown item's holster render rule back
                try{api_.refreshRender(reinterpret_cast<void*>(r));}catch(...){}throwing::bump(throwing::renderRestored);}
            { // BODY: where each holstered item hangs (weapon+0x80 = the world matrix its holstered weapWeapon::Update wrote last
              // frame; rows a,b,c,d, translation at +0x30). The item in hand and the phantom (re-posed to the hand) are not holstered,
              // nor is the thrown item while it flies / lies / returns (and 100 ms after: its last override is still in weapon+0x80).
                HolsteredWorld hw{};hw.ns=frame.now;
                const auto thrown=throwing::item.load(std::memory_order_relaxed);const auto ended=throwing::endNs.load(std::memory_order_relaxed);
                const bool recent=ended&&frame.now>=ended&&frame.now-ended<100'000'000ull;
                for(int k=0;k<slotCount;++k){
                    const auto item=s.items[std::size_t(k)];if(!item||item==s.handItem||item==phantom_.item)continue;
                    if((thrown&&item==thrown)||(recent&&k==lastThrownSlot_))continue;
                    float m[16]{};const auto w=weaponOf(item);
                    if(!w||!block(w+holsteredMatrixOffset,m,sizeof m)||!std::isfinite(m[12])||!std::isfinite(m[13])||!std::isfinite(m[14])||
                       std::fabs(m[12])>1e6f||std::fabs(m[13])>1e6f||std::fabs(m[14])>1e6f){holsteredRejects.fetch_add(1,std::memory_order_relaxed);continue;}
                    hw.position[std::size_t(k)]={m[12],m[13],m[14]};hw.mask|=1u<<unsigned(k);
                }
                holsteredBox.store(hw);holsteredReads.fetch_add(1,std::memory_order_relaxed);
            }
        }
        if(hasPending_){const auto c=pending_;run(c,frame.now);}
    }
    void execute(const mainthread::Command& c,const mainthread::Frame& frame) noexcept {
        if(!api_||c.op<OpDraw||c.op>OpCatch)return;
        if(hasPending_&&pending_.sequence!=c.sequence){superseded.fetch_add(1,std::memory_order_relaxed);hasPending_=false;}
        run(c,frame.now);
    }
};
inline EngineSide engineSide{};

// ---- startup (TraceEntry, before/after the hook install; any thread, once) ----
inline std::string property(const char* name) noexcept {
#if defined(__ANDROID__)
    char value[PROP_VALUE_MAX]{};const int saved=errno;const int n=__system_property_get(name,value);errno=saved;
    return n>0?std::string(value,std::size_t(n)):std::string();
#else
    (void)name;return {};
#endif
}
// DUAL: debug.rdrvr.vr.dual 0 (off), .dual.transfer 0, .dual.swapfire 0, .dual.render 0,
// .dual.radius <m, 0.06..0.25>. Read once with the holsters property (startupEnabled).
inline void startupDual() noexcept {
    const auto off=[](const std::string& v){return !v.empty()&&v[0]=='0';};
    const bool killed=off(property("debug.rdrvr.vr.dual"));dualKilled.store(killed,std::memory_order_relaxed);
    transferEnabled.store(!off(property("debug.rdrvr.vr.dual.transfer")),std::memory_order_relaxed);
    swapFire.store(!off(property("debug.rdrvr.vr.dual.swapfire")),std::memory_order_relaxed);
    forceRender.store(!off(property("debug.rdrvr.vr.dual.render")),std::memory_order_relaxed);
    const auto r=property("debug.rdrvr.vr.dual.radius");
    if(!r.empty()){char* e{};const float v=std::strtof(r.c_str(),&e);if(e!=r.c_str()&&std::isfinite(v)&&v>=.06f&&v<=.25f)transferRadius.store(v,std::memory_order_relaxed);}
    if(killed)dualState.store("DISABLED_BY_PROPERTY",std::memory_order_relaxed);
}
// TraceEntry: the DUAL engine proofs (UpdateHolsteredItemPositions / OnUpdatePosition tail) did not match: no phantom, transfer stays.
inline void dualUnavailable(const char* why) noexcept {dualUnavailableFlag.store(true,std::memory_order_relaxed);dualState.store(why,std::memory_order_relaxed);}
// debug.rdrvr.vr.holsters 0 = the whole module off (no engine calls, pass-through input, menu row "- PROP").
inline bool startupEnabled() noexcept {
    startupDual();
    throwing::startup(); // THROW: debug.rdrvr.vr.throw (kill) + debug.rdrvr.vr.throw.* tuning
    spin::startup();     // SPIN: debug.rdrvr.vr.spin (kill) + debug.rdrvr.vr.spin.* tuning (+ the real spin::rigid for reload / hammer)
    { // debug.rdrvr.vr.holsters.adopt 0 (game draws are not adopted), .gamedrawopen <ms 0..10000> (0 = no open-hand holster)
        const auto a=property("debug.rdrvr.vr.holsters.adopt");if(!a.empty())adoptGameDraws.store(a[0]!='0',std::memory_order_relaxed);
        const auto g=property("debug.rdrvr.vr.holsters.gamedrawopen");
        if(!g.empty()){char* e{};const float ms=std::strtof(g.c_str(),&e);
            if(e!=g.c_str()&&std::isfinite(ms)&&ms>=0.f&&ms<=10000.f)gameDrawOpenNs.store(std::uint64_t(double(ms)*1e6),std::memory_order_relaxed);}
    }
    const auto value=property("debug.rdrvr.vr.holsters");const bool on=value.empty()||value[0]!='0'; // same rule as VrSettings.h
    killedByProperty.store(!on,std::memory_order_relaxed);
    if(!on)engineState.store("DISABLED_BY_PROPERTY",std::memory_order_relaxed);return on;
}
// debug.rdrvr.vr.holsters.animated 1 = animated native draw clip instead of the instant FastDraw path.
inline bool startupAnimatedDraw() noexcept {return property("debug.rdrvr.vr.holsters.animated")=="1";}
// engineReady (release, stored after configure + registerModule) gates the main thread like CHEATS does, so the
// EngineSide/api_/holsterContext state written on the arm thread is visible before the first tick/execute uses it.
inline void tickThunk(const mainthread::Frame& f) noexcept {if(engineReady.load(std::memory_order_acquire))engineSide.tick(f);}
inline void executeThunk(const mainthread::Command& c,const mainthread::Frame& f) noexcept {
    if(engineReady.load(std::memory_order_acquire))engineSide.execute(c,f);}
inline void throwTickThunk(const mainthread::Frame& f) noexcept {if(engineReady.load(std::memory_order_acquire))throwing::tickThunk(f);}
inline bool configureEngine(const EngineApi& api) noexcept {
    if(killedByProperty.load(std::memory_order_relaxed)||!api){engineReady.store(false,std::memory_order_release);
        if(!killedByProperty.load())engineState.store("API_INCOMPLETE",std::memory_order_relaxed);return false;}
    engineSide.configure(api);engineConfigured.store(true,std::memory_order_relaxed);
    if(!mainthread::registerModule(mainthread::ModuleHolsters,"holsters",tickThunk,executeThunk)){engineState.store("REGISTER_FAILED");return false;}
    mainthread::registerModule(mainthread::ModuleThrow,"throw",throwTickThunk,nullptr); // THROW: the flight (after the holsters poll)
    engineState.store(api.animatedDraw?"READY_ANIMATED":"READY",std::memory_order_relaxed);engineReady.store(true,std::memory_order_release);return true;
}
inline void engineUnavailable(const char* why) noexcept {engineReady.store(false,std::memory_order_release);engineState.store(why,std::memory_order_relaxed);}

// ---- menu status (VR page row HOLSTERS suffix) ----
enum class Status : std::uint8_t {Unavailable,Property,Off,NoExecutor,Waiting,Blocked,Active,Holding};
inline Status status() noexcept {
    if(killedByProperty.load(std::memory_order_relaxed))return Status::Property;
    if(!engineReady.load(std::memory_order_acquire))return Status::Unavailable;
    if(!mainthread::executorInstalled())return Status::NoExecutor;
    if(!config().enabled)return Status::Off;
    if(!inventoryValid.load(std::memory_order_relaxed))return Status::Waiting;
    if(!contextAllowed.load(std::memory_order_relaxed))return Status::Blocked;
    return holdHand.load(std::memory_order_relaxed)>=0?Status::Holding:Status::Active;
}
// Panel glyphs only (A-Z 0-9 space . : / - % ? < >).
inline const char* statusText(Status s) noexcept {
    switch(s){case Status::Unavailable:return "UNAVAILABLE";case Status::Property:return "PROP";case Status::Off:return "OFF";
        case Status::NoExecutor:return "UNAVAILABLE";case Status::Waiting:return "WAITING";case Status::Blocked:return "BLOCKED";
        case Status::Active:return "ACTIVE";case Status::Holding:return holdHand.load(std::memory_order_relaxed)==0?"LEFT HAND":"RIGHT HAND";}
    return "UNAVAILABLE";
}

// ---- DUAL: menu row DUAL WIELD suffix ----
enum class DualStatus : std::uint8_t {Property,Unavailable,Off,Ready,Dual};
inline DualStatus dualStatus() noexcept {
    if(dualKilled.load(std::memory_order_relaxed))return DualStatus::Property;
    if(killedByProperty.load(std::memory_order_relaxed)||!engineReady.load(std::memory_order_acquire)||dualUnavailableFlag.load(std::memory_order_relaxed))
        return DualStatus::Unavailable;
    const Config c=config();if(!c.enabled||!c.dual)return DualStatus::Off;
    return phantomHand.load(std::memory_order_relaxed)>=0?DualStatus::Dual:DualStatus::Ready;
}
inline const char* dualStatusText(DualStatus s) noexcept {
    switch(s){case DualStatus::Property:return "PROP";case DualStatus::Unavailable:return "UNAVAILABLE";case DualStatus::Off:return "OFF";
        case DualStatus::Ready:return "READY";case DualStatus::Dual:return "DUAL";}
    return "UNAVAILABLE";
}
// "vr":{"holsters":{"dual":<this>}} (any thread)
inline std::string dualReport() {
    std::string out;out.reserve(1536);char b[1024];
    const int ph=phantomHand.load(),hold=holdHand.load();const auto since=phantomSinceNs.load();const auto now=nowNs();
    const auto landed=swapsLanded.load();
    std::snprintf(b,sizeof b,"{\"enabled\":%s,\"killed\":%s,\"unavailable\":%s,\"state\":\"%s\",\"status\":\"%s\",\"transfer\":%s,\"swapFire\":%s,"
        "\"forceRender\":%s,\"radius\":%.3f,\"mode\":\"%s\",\"phantom\":{\"slot\":%d,\"item\":%s,\"hand\":%d,\"weapon\":%s,\"ageMs\":%.0f,"
        "\"renderForced\":%s,\"generation\":%u,\"name\":\"%s\"},",
        dualActive()?"true":"false",dualKilled.load()?"true":"false",dualUnavailableFlag.load()?"true":"false",dualState.load(),
        dualStatusText(dualStatus()),transferActive()?"true":"false",swapFire.load()?"true":"false",forceRender.load()?"true":"false",
        double(transferRadius.load()),ph>=0&&hold>=0?"DUAL":hold>=0||ph>=0?"ONE":"EMPTY",phantomSlot.load(),phantomItem.load()?"true":"false",ph,
        phantomWeapon.load()?"true":"false",since&&now>=since?double(now-since)/1e6:-1.0,phantomRenderForced.load()?"true":"false",
        phantomGeneration.load(),phantomSlot.load()>=0&&phantomSlot.load()<slotCount?slotNames[std::size_t(phantomSlot.load())].load().c_str():"");out+=b;
    std::snprintf(b,sizeof b,"\"transfers\":{\"requested\":%llu,\"done\":%llu,\"refused\":%llu},\"phantomSets\":{\"requested\":%llu,\"done\":%llu},"
        "\"phantomMoves\":%llu,\"drawKeeps\":{\"requested\":%llu,\"done\":%llu},\"promotes\":{\"requested\":%llu,\"done\":%llu},"
        "\"swaps\":{\"requested\":%llu,\"executed\":%llu,\"landed\":%llu,\"firedAfter\":%llu,\"latencyMsAvg\":%.1f,\"latencyMsMax\":%.1f,\"timeouts\":%llu,"
        "\"blocked\":%llu},\"offhandTriggerEdges\":%llu,\"offhandDry\":%llu,\"renderForcedCalls\":%llu,\"renderRestored\":%llu,\"resumes\":%llu,"
        "\"ticks\":%llu,\"lastClear\":\"%s\",\"phantomClears\":{",
        (unsigned long long)transfersRequested.load(),(unsigned long long)transfersDone.load(),(unsigned long long)transfersRefused.load(),
        (unsigned long long)phantomSetRequests.load(),(unsigned long long)phantomSets.load(),(unsigned long long)phantomMoves.load(),
        (unsigned long long)drawKeepRequests.load(),(unsigned long long)drawKeeps.load(),(unsigned long long)promoteRequests.load(),
        (unsigned long long)promotes.load(),(unsigned long long)swapsRequested.load(),(unsigned long long)swapsDone.load(),(unsigned long long)landed,
        (unsigned long long)swapsFiredAfter.load(),landed?double(swapLatencyNsSum.load())/double(landed)/1e6:0.0,double(swapLatencyNsMax.load())/1e6,
        (unsigned long long)swapTimeouts.load(),(unsigned long long)swapBlocked.load(),(unsigned long long)offhandTriggerEdges.load(),
        (unsigned long long)offhandDry.load(),(unsigned long long)renderForcedCalls.load(),(unsigned long long)renderRestored.load(),
        (unsigned long long)phantomResumes.load(),(unsigned long long)phantomTicks.load(),lastPhantomClear.load());out+=b;
    for(unsigned i=0;i<ClearCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",phantomClearNames[i],(unsigned long long)phantomClears[i].load());out+=b;}
    out+="},\"refusals\":{";
    for(unsigned i=0;i<DualRefusalCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",dualRefusalNames[i],(unsigned long long)dualRefusals[i].load());out+=b;}
    out+="}}";return out;
}

// ---- report: "vr":{"holsters":<this>} (any thread) ----
inline std::string report() {
    std::string out;out.reserve(4096);char b[1024];const Config c=config();
    std::snprintf(b,sizeof b,"{\"state\":\"%s\",\"engineState\":\"%s\",\"killedByProperty\":%s,\"engineReady\":%s,\"executor\":%s,"
        "\"config\":{\"enabled\":%s,\"grip\":\"%s\",\"markers\":\"%s\",\"twoHand\":%s},\"inventoryValid\":%s,\"contextAllowed\":%s,\"blockReason\":\"%s\",",
        statusText(status()),engineState.load(),killedByProperty.load()?"true":"false",engineReady.load()?"true":"false",
        mainthread::executorInstalled()?"true":"false",c.enabled?"true":"false",c.grip==GripMode::Toggle?"TOGGLE":"HOLD",
        c.markers==MarkerMode::Always?"ALWAYS":c.markers==MarkerMode::Off?"OFF":"NEAR",c.twoHand?"true":"false",
        inventoryValid.load()?"true":"false",contextAllowed.load()?"true":"false",blockReason.load());out+=b;
    out+="\"slots\":{";
    const auto owned=ownedMask.load(),grab=grabMask.load();
    for(int k=0;k<slotCount;++k){
        std::snprintf(b,sizeof b,"%s\"%d\":{\"name\":\"%s\",\"owned\":%s,\"grabbable\":%s,\"item\":\"%s\"}",k?",":"",k,slotSpecs[std::size_t(k)].name,
            (owned>>unsigned(k)&1u)?"true":"false",(grab>>unsigned(k)&1u)?"true":"false",slotNames[std::size_t(k)].load().c_str());out+=b;}
    const int hold=holdHand.load();
    std::snprintf(b,sizeof b,"},\"current\":{\"slot\":%d,\"item\":\"%s\",\"holdHand\":%d,\"ours\":%s,\"holdSequence\":%u,\"generation\":%u,"
        "\"supportHand\":%d,\"attachmentGeneration\":%u,\"inside\":[%d,%d],\"bodyYawDeg\":%.1f},",
        currentSlot.load(),handName.load().c_str(),hold,holdOurs.load()?"true":"false",holdSequence.load(),holdGeneration.load(),supportHand.load(),
        attachmentGeneration.load(),insidePublished[0].load(),insidePublished[1].load(),double(bodyYawPublished.load())*57.29577951308232);out+=b;
    std::snprintf(b,sizeof b,"\"input\":{\"frames\":%llu,\"suspended\":%llu,\"grabPresses\":%llu,\"putAwayPresses\":%llu,\"holdReleases\":%llu,"
        "\"nativeGripPresses\":%llu,\"backMasked\":%llu,\"mouthWhistles\":%llu,\"resumes\":%llu,\"supportEngaged\":%llu,\"swappedFrames\":%llu,\"rtHeldFrames\":%llu,\"postFailures\":%llu,"
        "\"releaseDeferredFrames\":%llu,\"releaseDeferred\":%llu},",
        (unsigned long long)inputFrames.load(),(unsigned long long)suspendedFrames.load(),(unsigned long long)grabPresses.load(),
        (unsigned long long)putAwayPresses.load(),(unsigned long long)holdReleases.load(),(unsigned long long)nativeGripPresses.load(),(unsigned long long)backMasked.load(),(unsigned long long)mouthWhistles.load(),
        (unsigned long long)resumes.load(),(unsigned long long)supportEngaged.load(),(unsigned long long)swappedFrames.load(),
        (unsigned long long)rtHeldFrames.load(),(unsigned long long)postFailures.load(),
        (unsigned long long)releaseDeferredFrames.load(),(unsigned long long)releaseDeferred.load());out+=b;
    { // Release -> holster state machine, body sockets
        BodySockets bs{};const bool haveBs=bodySocketBox.load(bs);HolsteredWorld hw{};const bool haveHw=holsteredBox.load(hw);
        std::snprintf(b,sizeof b,"\"release\":{\"intents\":%llu,\"retries\":%llu,\"waitFrames\":%llu,\"cancelled\":%llu,\"done\":%llu,\"void\":%llu,"
            "\"openHand\":%llu,\"attemptsMax\":%u},\"bodySockets\":{\"frames\":%llu,\"stale\":%llu,\"mask\":%u,\"fromItem\":%u,\"radius\":[%.2f,%.2f],\"beltRadius\":[%.2f,%.2f,%.2f],"
            "\"holstered\":{\"reads\":%llu,\"rejects\":%llu,\"mask\":%u}},",
            (unsigned long long)holsterIntents.load(),(unsigned long long)holsterRetries.load(),(unsigned long long)holsterWaitFrames.load(),
            (unsigned long long)holsterCancelled.load(),(unsigned long long)holsterDone.load(),(unsigned long long)holsterVoid.load(),
            (unsigned long long)openHandHolsters.load(),unsigned(holsterAttemptsMax.load()),(unsigned long long)bodySocketFrames.load(),
            (unsigned long long)bodySocketStale.load(),haveBs?unsigned(bs.mask):0u,haveBs?unsigned(bs.fromItem):0u,double(bodyGrabRadius),double(bodyLeaveRadius),double(beltGrabRadius),double(beltLeaveRadius),double(beltVertical),
            (unsigned long long)holsteredReads.load(),(unsigned long long)holsteredRejects.load(),haveHw?unsigned(hw.mask):0u);out+=b;
    }
    std::snprintf(b,sizeof b,"\"weapon\":{\"handSubstituted\":%llu,\"twoHandApplied\":%llu},\"grabs\":%llu,\"grabsConfirmed\":%llu,\"alreadyInHand\":%llu,"
        "\"putAways\":%llu,\"putAwayEmpty\":%llu,\"gameDraws\":%llu,\"retries\":%llu,\"superseded\":%llu,\"polls\":%llu,\"pollFailures\":%llu,"
        "\"contextReads\":%llu,\"lastRefusal\":\"%s\",\"refusals\":{",
        (unsigned long long)handSubstituted.load(),(unsigned long long)twoHandApplied.load(),(unsigned long long)draws.load(),
        (unsigned long long)drawsConfirmed.load(),(unsigned long long)alreadyInHand.load(),(unsigned long long)putAways.load(),
        (unsigned long long)putAwayEmpty.load(),(unsigned long long)gameDraws.load(),(unsigned long long)retries.load(),
        (unsigned long long)superseded.load(),(unsigned long long)polls.load(),(unsigned long long)pollFailures.load(),
        (unsigned long long)contextReads.load(),lastRefusal.load());out+=b;
    bool first=true;
    for(unsigned i=0;i<RefuseCount;++i){const auto n=refusals[i].load();if(!n)continue;
        std::snprintf(b,sizeof b,"%s\"%s\":%u",first?"":",",refusalNames[i],n);out+=b;first=false;}
    MarkerSet m{};const bool haveMarkers=markers(m);
    std::snprintf(b,sizeof b,"},\"markers\":%u,",haveMarkers?m.count:0u);out+=b;
    { // Empty-hand triggers, adopted game draws, FIST BLOCK guard pose
        const auto u=[](const std::atomic<std::uint64_t>& a){return (unsigned long long)a.load(std::memory_order_relaxed);};
        std::snprintf(b,sizeof b,"\"emptyTriggers\":{\"mode\":\"%s\",\"edgesWithheld\":%llu,\"framesWithheld\":%llu,\"latchedFrames\":%llu,"
            "\"strikePassed\":%llu,\"passedGated\":%llu},\"gameDraw\":{\"adopt\":%s,\"openMs\":%.0f,\"adopted\":%llu,\"closed\":%llu,\"open\":%llu,"
            "\"squeezed\":%llu,\"openHolsters\":%llu},",
            emptyTriggerNative.load()?"NATIVE":nativeFistTrigger.load()?"STRIKE_ONLY":"WITHHELD",u(emptyTriggerEdges),u(emptyTriggerFrames),
            u(emptyTriggerLatched),u(emptyStrikePassed),u(emptyTriggerPassedGated),adoptGameDraws.load()?"true":"false",
            double(gameDrawOpenNs.load())/1e6,u(gameDrawsAdopted),u(adoptClosed),u(adoptOpen),u(adoptSqueezed),u(adoptOpenHolsters));out+=b;
        namespace G=guard;const auto since=G::engagedSinceNs.load(),stamp=G::stampNs.load(),now=nowNs();
        std::snprintf(b,sizeof b,"\"guard\":{\"active\":%s,\"killed\":%s,\"state\":\"%s\",\"ageMs\":%.0f,\"engagedForMs\":%.0f,\"frames\":%llu,"
            "\"armingFrames\":%llu,\"engagedFrames\":%llu,\"engages\":%llu,\"releases\":%llu,\"aborted\":%llu,\"maskedPresses\":%llu,"
            "\"premaskedFrames\":%llu,\"last\":{\"forearm\":[%.2f,%.2f],\"forward\":[%.2f,%.2f],\"up\":[%.2f,%.2f],\"side\":[%.2f,%.2f]}},",
            G::active.load()?"true":"false",G::killed.load()?"true":"false",G::stateName(G::state.load()),stamp&&now>=stamp?double(now-stamp)/1e6:-1.0,
            G::state.load()==G::StateEngaged&&since&&now>=since?double(now-since)/1e6:0.0,u(G::frames),u(G::armingFrames),u(G::engagedFrames),
            u(G::engages),u(G::releases),u(G::aborted),u(G::maskedPresses),u(G::premaskedFrames),double(G::lastForearm[0].load()),
            double(G::lastForearm[1].load()),double(G::lastForward[0].load()),double(G::lastForward[1].load()),double(G::lastUp[0].load()),
            double(G::lastUp[1].load()),double(G::lastSide[0].load()),double(G::lastSide[1].load()));out+=b;
    }
    { // Hold continuity counters + the event ring (oldest first; tMs = the writer's clock, ageMs vs now)
        const auto n=eventCount.load(std::memory_order_acquire);const auto now=nowNs();const std::uint32_t count=n<eventRingSize?n:eventRingSize;
        std::snprintf(b,sizeof b,"\"holdTracking\":{\"gapRestores\":%llu,\"adoptPrevHand\":%llu,\"freeHandMasked\":%llu,\"pressMismatch\":%llu,\"eventsTotal\":%u},\"events\":[",
            (unsigned long long)gapRestores.load(),(unsigned long long)adoptPrevHand.load(),(unsigned long long)freeHandMasked.load(),
            (unsigned long long)pressMismatch.load(),unsigned(n));out+=b;
        for(std::uint32_t j=0;j<count;++j){const auto& e=eventRing[(n-count+j)%eventRingSize];
            const auto ns=e.ns.load(std::memory_order_acquire);const auto p=e.packed.load(std::memory_order_relaxed);const char* r=e.reason.load(std::memory_order_relaxed);
            const unsigned ev=unsigned(p&0xffu);
            std::snprintf(b,sizeof b,"%s{\"tMs\":%.1f,\"ageMs\":%.0f,\"event\":\"%s\",\"slot\":%d,\"hand\":%d,\"reason\":\"%s\",\"aux\":%u}",j?",":"",double(ns)/1e6,
                now>=ns?double(now-ns)/1e6:-1.0,ev<EvCount?eventNames[ev]:"?",int(std::int8_t(std::uint8_t(p>>8))),int(std::int8_t(std::uint8_t(p>>16))),r?r:"",
                unsigned(p>>32));out+=b;}
        out+="],";
    }
    out+="\"dual\":";out+=dualReport();out+=",";
    out+="\"haptics\":";out+=haptics::report();out+=",\"mainThread\":";out+=mainthread::report();out+="}";
    return out;
}
} // namespace rdrvr::vr::holsters
