#pragma once
// Menu HANDS / WEAPONS -> HAMMER OFF / FAN / FAN W/ TRIGGER / SINGLE ACTION (default FAN), FAN SPREAD OFF / LOW / HIGH (default LOW).
//   FAN            the holding trigger fires normally (one RT pulse per press - holding never repeats) + a fast swipe of the open free palm back
//                  over the hammer spur = a shot.
//   FAN W/ TRIGGER the swipe counts only while the holding trigger is squeezed.
//   SINGLE ACTION  only a cocked hammer fires; the swipe (slow is fine) cocks it; after the shot it is down again.
// The menu row is HAMMER CASUAL / REALISTIC (cfg "hammerStyle"; stored 4-value settings map SINGLE ACTION -> REALISTIC, the
// rest -> CASUAL). CASUAL = the FAN engine above. REALISTIC = SINGLE ACTION for single-action revolvers (Cattleman, Schofield, LeMat, ...;
// an unrecognised revolver name counts as single-action): a dry trigger does nothing (faint click), a drag / swipe to full cock cocks, the
// trigger fires the cocked hammer; the trigger HELD while the free hand drags / slaps the hammer to full cock fires at once (fanning, one
// shot per full cock). A double-action revolver (hand item name contains DOUBLE) or a non-revolver pistol under REALISTIC = CASUAL.
// A shot is ALWAYS a virtual-pad RT pulse (1 frame pressed, >= 1 frame released) routed by VrHolsters::route() - the engine sees an ordinary
// press (UpdateAttackInput -> packet -> aniAim::RequestFire). No direct engine fire calls, no retries (one action = one pulse).
// HAMMER decides WHEN (stroke, cock, pulse shape, tempo, spread); RELOAD decides WHETHER the chamber fires (reload::gate):
// manual RELOAD -> its gate (dry click inside); otherwise the empty rule here: the physical trigger on an empty clip goes RAW to the game
// (the native reload / weapon switch - never a soft-lock), a swipe on an empty clip is dry (haptic only).
// Tempo: the engine limits shots by weapWeapon::Fire's ChamberTime (game time T) and the fire animation's event frame; during a FAN series
// the mod sets the duel's override weapon+0x138 = .hammer.chamber (130 ms), restored first whenever the context changes.
//
// Threads:
//   XR thread (XrHarness::locateHands via holsters::hammerSinks->xrSample, inside inputReady): THE ONLY owner of the Detector; the stroke
//     haptic starts in the same XR frame; strokes go through a lock-free queue.
//   input thread (= main, holsters InputBrain): frameBegin (FIRST line of InputBrain::frame, every path: Arm seqlock, stroke drain, in-flight
//     expiry), ownsRt / step (route: requests -> reload::gate / empty rule -> tempo -> pulse), marker (H3 ring).
//   main thread, PostCameraUpdate (vrWeaponObserve): observe = engine facts in ONE readMemoryVector (pointer chain cached), the override write
//     (restore FIRST), Facts seqlock. ManagerUpdate entry (VrWeapon forceBarrelAim): spreadFor.
//   any thread: claimFire (FireProjectile via VrWeapon noteFire), pose (inside the held weapon's own weapWeapon::Update: Zone, the native
//     hammer trace, the self-gated visual H2).
// Kill switch debug.rdrvr.vr.hammer 0 (Feature::Hammer): no sinks registered, nothing read or written, route() behaves as without HAMMER.
// Fingerprints (elf::prints, classes P / R / O / V + the time GOT + the aniAim vtable): P mismatch -> UNAVAILABLE (HAMMER never owns RT);
// R mismatch -> no engine facts (fixed 300 ms in-flight window, no empty rule, no tempo forecast); O mismatch -> no override write; V -> no H2.
#include "VrReload.h"
#include "diagnostics/GuardedReadVector.h"
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

namespace rdrvr::vr::hammer {
namespace H=::rdrvr::vr::holsters;namespace L=::rdrvr::vr::reload;
using V3=std::array<float,3>;using Q=std::array<float,4>;

// =====================================================================================================================
// Engine identities (librdr.so sha256 3f80845e..., .text file offset = vaddr - 0x4000).
namespace elf {
enum Class : unsigned {ClassNone=0,ClassP=1,ClassR=2,ClassO=4,ClassV=8};
struct PrintRef {std::uintptr_t offset;const unsigned char* bytes;std::size_t size;unsigned classes;const char* name;};
// aniAim::CanFire aim bit22|23 gate 0x1cbb684..0x1cbb698 (20 B)
inline constexpr std::array<unsigned char,20> canFireAimGatePrint{
#include "gamecode/VrHammer_1cbb684_20_8d.inc"
};
// aniAim::CanFire bit33|34 bypass of the fire-anim phase 0x1cbb710..0x1cbb730 (32 B)
inline constexpr std::array<unsigned char,32> canFireRapidBypassPrint{
#include "gamecode/VrHammer_1cbb710_32_8d.inc"
};
// aniAim::Fire reset (+0x28,+0x2c)=(0,1) from 0x1259550 0x1cbb908..0x1cbb920 (24 B)
inline constexpr std::array<unsigned char,24> fireResetRatePrint{
#include "gamecode/VrHammer_1cbb908_24_8d.inc"
};
// aniAim::Fire success/fail clear of bit3 0x1cbb9bc..0x1cbb9cc (16 B)
inline constexpr std::array<unsigned char,16> fireClearBit3Print{
#include "gamecode/VrHammer_1cbb9bc_16_8d.inc"
};
// aniAim::UpdateInternalStates drop of bit3 when bit22 clear 0x1cbc23c..0x1cbc270 (52 B)
inline constexpr std::array<unsigned char,52> internalStatesBit3Print{
#include "gamecode/VrHammer_1cbc23c_52_8d.inc"
};
// aniAim::Update window decrement aim+0x30 / +0x454 0x1cc0110..0x1cc0134 (36 B)
inline constexpr std::array<unsigned char,36> windowDecrementPrint{
#include "gamecode/VrHammer_1cc0110_36_8d.inc"
};
// aniAim::Update window expiry clears bits 3|5 0x1cc0e1c..0x1cc0e38 (28 B)
inline constexpr std::array<unsigned char,28> windowExpiryPrint{
#include "gamecode/VrHammer_1cc0e1c_28_8d.inc"
};
// aniAim::Update phase advance phase+=step*[aim+0x2c] 0x1cc1590..0x1cc15a0 (16 B)
inline constexpr std::array<unsigned char,16> phaseAdvancePrint{
#include "gamecode/VrHammer_1cc1590_16_8d.inc"
};
// aniAim::Update event-frame crossing + chamber decision + retime 0x1cc15cc..0x1cc16b8 (236 B)
inline constexpr std::array<unsigned char,236> eventDecisionPrint{
#include "gamecode/VrHammer_1cc15cc_236_8d.inc"
};
// aniAim::Update end of fire anim -> auto RequestReload 0x1cc16b8..0x1cc1700 (72 B)
inline constexpr std::array<unsigned char,72> animEndReloadPrint{
#include "gamecode/VrHammer_1cc16b8_72_8d.inc"
};
// aniAim::RequestFire IsOutOfAmmo(vt+0x150) drop of bit3 / SelectNextWeaponWithAmmo 0x1cc2c58..0x1cc2cd0 (120 B)
inline constexpr std::array<unsigned char,120> requestFireEmptyPrint{
#include "gamecode/VrHammer_1cc2c58_120_8d.inc"
};
// aniAim::RequestFire bit3 set + aim+0x30 window + TriggerDown 0x1cc2d1c..0x1cc2d68 (76 B)
inline constexpr std::array<unsigned char,76> requestFireBit3Print{
#include "gamecode/VrHammer_1cc2d1c_76_8d.inc"
};
// weapWeapon::Update aniAim chain [[[actor+0x38]+0xab8]+0x8] -> bl aniAim::Fire 0x2543768..0x2543798 (48 B)
inline constexpr std::array<unsigned char,48> updateAimChainPrint{
#include "gamecode/VrHammer_2543768_48_8d.inc"
};
// weapWeapon::Update ActionController Play/Update before UpdateAnimation 0x25438a0..0x25438ec (76 B)
inline constexpr std::array<unsigned char,76> actionControllerPosePrint{
#include "gamecode/VrHammer_25438a0_76_8d.inc"
};
// weapWeapon::Fire(bool,int,bool) whole 0x2544354..0x2544608 (692 B)
inline constexpr std::array<unsigned char,692> weaponFirePrint{
#include "gamecode/VrHammer_2544354_692_8d.inc"
};
// weapWeapon::GetChamberTime() const whole 0x2544608..0x25446a0 (152 B)
inline constexpr std::array<unsigned char,152> getChamberTimePrint{
#include "gamecode/VrHammer_2544608_152_8d.inc"
};
// weapWeapon::DryFire()+Trigger() whole 0x25446a0..0x25446e0 (64 B)
inline constexpr std::array<unsigned char,64> dryFireTriggerPrint{
#include "gamecode/VrHammer_25446a0_64_8d.inc"
};
// weapWeapon::ChamberTimeExpired() const whole 0x2545fcc..0x2546084 (184 B)
inline constexpr std::array<unsigned char,184> chamberTimeExpiredPrint{
#include "gamecode/VrHammer_2545fcc_184_8d.inc"
};
// weapWeapon::GetAmmoCount() const whole 0x2546084..0x254608c (8 B)
inline constexpr std::array<unsigned char,8> getAmmoCountPrint{0,44,65,189,192,3,95,214};
// weapWeapon::SetChamberTimeOverride(float) whole 0x254627c..0x2546284 (8 B)
inline constexpr std::array<unsigned char,8> setChamberOverridePrint{0,56,1,189,192,3,95,214};
// weapWeapon::ManagerUpdate rearm block (bit1 consumed / bit2 set) 0x2549958..0x254998c (52 B)
inline constexpr std::array<unsigned char,52> managerRearmPrint{
#include "gamecode/VrHammer_2549958_52_8d.inc"
};
// weapOperationalMode::Load ChamberTime store (+0xd4) 0x2552578..0x2552590 (24 B)
inline constexpr std::array<unsigned char,24> loadChamberStorePrint{
#include "gamecode/VrHammer_2552578_24_8d.inc"
};
// weapOperationalMode::Load AutoFireDelay store (+0xe4) 0x2552648..0x2552660 (24 B)
inline constexpr std::array<unsigned char,24> loadAutoFireStorePrint{
#include "gamecode/VrHammer_2552648_24_8d.inc"
};
// weapBaseWeaponType::GetOpMode const whole (type+0x30+i*8) 0x25592b8..0x25592c4 (12 B)
inline constexpr std::array<unsigned char,12> getOpModePrint{8,204,33,139,0,25,64,249,192,3,95,214};
// weapBaseWeaponType::SetTrigHoldMode whole (type+0x1b0) 0x255940c..0x2559414 (8 B)
inline constexpr std::array<unsigned char,8> setTrigHoldModePrint{1,176,1,185,192,3,95,214};
// sagPlayer::UpdateAttackInput edge logic (+0x240 1/2/3) 0x269a114..0x269a190 (124 B)
inline constexpr std::array<unsigned char,124> attackInputEdgePrint{
#include "gamecode/VrHammer_269a114_124_8d.inc"
};
// sagPlayer::UpdateAttackInput empty clip -> +0x248 reload 0x269a1a0..0x269a224 (132 B)
inline constexpr std::array<unsigned char,132> attackInputEmptyPrint{
#include "gamecode/VrHammer_269a1a0_132_8d.inc"
};
// sagPlayer::SetupAttackPacket press/held/auto block 0x26a0d48..0x26a0e98 (336 B)
inline constexpr std::array<unsigned char,336> attackPacketPressPrint{
#include "gamecode/VrHammer_26a0d48_336_8d.inc"
};
inline constexpr std::array<PrintRef,28> prints{{{0x1cbb684,canFireAimGatePrint.data(),canFireAimGatePrint.size(),1,"canFireAimGate"},{0x1cbb710,canFireRapidBypassPrint.data(),canFireRapidBypassPrint.size(),4,"canFireRapidBypass"},{0x1cbb908,fireResetRatePrint.data(),fireResetRatePrint.size(),0,"fireResetRate"},{0x1cbb9bc,fireClearBit3Print.data(),fireClearBit3Print.size(),2,"fireClearBit3"},{0x1cbc23c,internalStatesBit3Print.data(),internalStatesBit3Print.size(),0,"internalStatesBit3"},{0x1cc0110,windowDecrementPrint.data(),windowDecrementPrint.size(),2,"windowDecrement"},{0x1cc0e1c,windowExpiryPrint.data(),windowExpiryPrint.size(),2,"windowExpiry"},{0x1cc1590,phaseAdvancePrint.data(),phaseAdvancePrint.size(),0,"phaseAdvance"},{0x1cc15cc,eventDecisionPrint.data(),eventDecisionPrint.size(),4,"eventDecision"},{0x1cc16b8,animEndReloadPrint.data(),animEndReloadPrint.size(),0,"animEndReload"},{0x1cc2c58,requestFireEmptyPrint.data(),requestFireEmptyPrint.size(),0,"requestFireEmpty"},{0x1cc2d1c,requestFireBit3Print.data(),requestFireBit3Print.size(),3,"requestFireBit3"},{0x2543768,updateAimChainPrint.data(),updateAimChainPrint.size(),3,"updateAimChain"},{0x25438a0,actionControllerPosePrint.data(),actionControllerPosePrint.size(),8,"actionControllerPose"},{0x2544354,weaponFirePrint.data(),weaponFirePrint.size(),6,"weaponFire"},{0x2544608,getChamberTimePrint.data(),getChamberTimePrint.size(),4,"getChamberTime"},{0x25446a0,dryFireTriggerPrint.data(),dryFireTriggerPrint.size(),0,"dryFireTrigger"},{0x2545fcc,chamberTimeExpiredPrint.data(),chamberTimeExpiredPrint.size(),0,"chamberTimeExpired"},{0x2546084,getAmmoCountPrint.data(),getAmmoCountPrint.size(),2,"getAmmoCount"},{0x254627c,setChamberOverridePrint.data(),setChamberOverridePrint.size(),4,"setChamberOverride"},{0x2549958,managerRearmPrint.data(),managerRearmPrint.size(),1,"managerRearm"},{0x2552578,loadChamberStorePrint.data(),loadChamberStorePrint.size(),2,"loadChamberStore"},{0x2552648,loadAutoFireStorePrint.data(),loadAutoFireStorePrint.size(),2,"loadAutoFireStore"},{0x25592b8,getOpModePrint.data(),getOpModePrint.size(),2,"getOpMode"},{0x255940c,setTrigHoldModePrint.data(),setTrigHoldModePrint.size(),2,"setTrigHoldMode"},{0x269a114,attackInputEdgePrint.data(),attackInputEdgePrint.size(),1,"attackInputEdge"},{0x269a1a0,attackInputEmptyPrint.data(),attackInputEmptyPrint.size(),0,"attackInputEmpty"},{0x26a0d48,attackPacketPressPrint.data(),attackPacketPressPrint.size(),1,"attackPacketPress"}}};
// rage::sysTimeManager::sm_GlobalManager (GOT GLOB_DAT) + 0x3c = the game time T weapWeapon::Fire compares (0x254440c).
inline constexpr std::uintptr_t timeGotOffset=0x2ddf340,timeGlobalOffset=0x4e6d6510;inline constexpr std::size_t timeField=0x3c;
// vtable for aniAim (0x2ce7040) + 0x10 = the vptr; its ABS64 slots D2, D0, Init, Reset, Update.
inline constexpr std::uintptr_t aimVptrOffset=0x2ce7050;
inline constexpr std::array<std::uintptr_t,5> aimSlots{0x1cc3ddc,0x1cc3fb8,0x1cb8ce0,0x1cb8cf0,0x1cc00a4};
// Fields: weapWeapon, weapBaseWeaponType, weapOperationalMode, aniAim, sagPlayer.
inline constexpr std::size_t wType=0x28,wOpIndex=0x30,wOwnerGuid=0x20,wFlags=0x120,wAmmo=0x12c,wLastFire=0x130,wOverride=0x138,
    wAnimClip=0x2e0,wAnimPhasePtr=0x330,tTrigHold=0x1b0,tOpModes=0x30,oPerClip=0xc4,oChamber=0xd4,oAutoFire=0xe4,
    aActorAnim=0x38,anBlock=0xab8,blkAim=0x08,aimOwner=0x08,aimPhase=0x24,aimEvent=0x114,aimState=0x3bc,aimBits=0x5d4,
    pAttack=0x240,pReload=0x248;
inline constexpr std::uint64_t bitRequest=1ull<<3,bitAim22=1ull<<22,bitAim23=1ull<<23,bitRapid33=1ull<<33,bitRapid34=1ull<<34;
} // namespace elf

// =====================================================================================================================
// Configuration (startup properties debug.rdrvr.vr.hammer.*; written once before any thread uses them).
enum Mode : unsigned {ModeOff=0,ModeFan=1,ModeFanTrigger=2,ModeSingle=3};
inline const char* modeName(unsigned m) noexcept {return m==ModeFan?"FAN":m==ModeFanTrigger?"FAN W/ TRIGGER":m==ModeSingle?"SINGLE ACTION":"OFF";}
struct Config {
    bool enabled{true};                                      // debug.rdrvr.vr.hammer (Feature::Hammer)
    float chamberS{.130f};                                   // .chamber <ms> (0 = never write weapon+0x138)
    std::uint64_t streakNs{700'000'000};                     // .streak <ms>
    float vmin{.55f},cockVmin{.25f},vabs{.20f},cockVabs{.12f};// .vmin / .cockvmin / .vabs (m/s)
    float halfW{.07f},yMin{.02f},yMax{.16f};                 // .zone "halfW ymin ymax" (cm) - the window ABOVE the spur
    float elevDeg{-40.f},fwdM{.12f};                         // .present "elevDeg fwdCm"
    std::uint64_t parkedNs{250'000'000};                     // .parked <ms>
    V3 offset{0.f,.015f,-.010f};                             // .offset "x y z" (cm, model right / up / barrel): the spur from the bone origin
    float fallbackUp{.020f},fallbackBack{.035f};             // .fallback "up back" (cm) from the cylinder bone (no hammer bone)
    V3 palm{0.f,0.f,.03f};                                   // .palm "x y z" (cm, the free grip frame): the striking point
    float grip{.85f};                                        // .grip: the free hand's grip must stay <= this
    float capR{.12f},capFront{-.01f},capBack{.08f},capUp{.03f};// .capsule "r front back up" (cm): axis 3 cm above the spur, 1..8 cm behind it, r 12 cm
    float dominance{.55f};std::uint64_t refractoryNs{90'000'000}; // .dominance (backward share of the relative speed) / .refractory <ms>
    int retry{0};unsigned pulseFrames{1};                    // .retry (diagnostic only) / .pulse <frames>
    bool drawCocked{},thumbCock{};                           // .drawcocked 1 / .thumbcock 1 (SA)
    int visual{1};float visualForceDeg{};                    // .visual 0 | force <deg> (H2; 1 = auto, self-gated)
    bool ring{false};                                        // .ring 1 = the yellow hint ring while cocking (default off)
    int boneIndex{-1};std::array<char,32> boneName{};        // .bone <i|name> (default: the first bone whose name contains "hammer")
    std::uint64_t transferBlockNs{300'000'000};              // .transferblock <ms>
    std::uint64_t closeGraceNs{400'000'000};                 // strokes refused this long after a cylinder close
    // DRAG (.drag "travelCm maxDeg sign", .dragmisc "returnMs rearmCm absShare tickMm"): travel 0 = off (the swipe only)
    float dragTravel{.025f},dragDeg{45.f},dragSign{1.f},dragReturnS{.06f},dragRearm{.010f},dragAbsShare{.15f},dragTickM{.004f};
};
inline Config config{};
// the spur offset of the held model (reload.geom.<KEY> spur, mm), else.offset (tuned on the Cattleman)
inline V3 spurOffset(std::uintptr_t w) noexcept {const auto& s=L::specOf(w);return s.geom?V3{s.spur[0]*.001f,s.spur[1]*.001f,s.spur[2]*.001f}:config.offset;}
inline std::atomic<bool> started{};
// The menu style (0 CASUAL, 1 REALISTIC) and the held sidearm's action, classified from the engine
// hand item's type name (holsters::handName, refreshed by Brain::frameBegin). Unknown = single-action.
enum Style : unsigned {StyleCasual=0,StyleRealistic=1};
inline const char* styleName(unsigned s) noexcept {return s==StyleRealistic?"REALISTIC":"CASUAL";}
enum Action : int {ActionUnknown=0,ActionSingle=1,ActionDouble=2,ActionNotRevolver=3};
inline const char* actionName(int a) noexcept {return a==ActionSingle?"SINGLE":a==ActionDouble?"DOUBLE":a==ActionNotRevolver?"NOT_REVOLVER":"UNKNOWN";}
inline std::atomic<int> heldAction{ActionUnknown};
inline bool containsUpper(const char* s,const char* upperNeedle) noexcept {
    if(!s||!upperNeedle||!*upperNeedle)return false;
    for(;*s;++s){const char* a=s;const char* b=upperNeedle;
        while(*a&&*b){char c=*a;if(c>='a'&&c<='z')c=char(c-'a'+'A');if(c=='_'||c==' '||c=='-'){++a;continue;}if(c!=*b)break;++a;++b;}
        if(!*b)return true;}
    return false;}
// names like "base_pistol_REVOLVER_Cattleman" / "WEAPON_REVOLVER_DOUBLEACTION" ('_', ' ', '-' are skipped while matching)
inline int classifyAction(const char* name) noexcept {
    if(!name||!*name||!std::strcmp(name,"?"))return ActionUnknown;
    if(containsUpper(name,"DOUBLE"))return ActionDouble;
    // REALISTIC (cock, then fire) only for the calibrated Cattleman; every other revolver fires on the trigger like CASUAL
    if(containsUpper(name,"CATTLEMAN"))return ActionSingle;
    if(containsUpper(name,"REVOLVER")||containsUpper(name,"SCHOFIELD")||containsUpper(name,"LEMAT")||containsUpper(name,"COLT"))return ActionDouble;
    if(containsUpper(name,"SEMIAUTO")||containsUpper(name,"MAUSER")||containsUpper(name,"VOLCANIC")||containsUpper(name,"HIGHPOWER")||containsUpper(name,"AUTOMATIC"))
        return ActionNotRevolver;
    return ActionUnknown;}
inline bool realisticAction(int a) noexcept {return a==ActionUnknown||a==ActionSingle;}
inline unsigned modeOf(const ::rdrvr::vr::Snapshot& s) noexcept {
    return s.hammerStyle()==StyleRealistic&&realisticAction(heldAction.load(std::memory_order_relaxed))?ModeSingle:ModeFan;}
inline unsigned style() noexcept {return settings().hammerStyle();}
inline unsigned mode() noexcept {
    if(!config.enabled||killed(Feature::Hammer)||!started.load(std::memory_order_acquire))return ModeOff;
    return modeOf(settings());}
inline float sigmaDeg() noexcept {return settings().fanSpreadDeg();}

// =====================================================================================================================
// Engine environment (TraceEntry configureEngine; host tests pass fakes).
using ReadFn=bool(*)(void*,std::uintptr_t,void*,std::size_t);
using WriteFn=bool(*)(std::uintptr_t,const void*,std::size_t);
struct Env {
    ReadFn read{};WriteFn write{&L::processWrite};
    std::uintptr_t base{},weaponTable{},timeAddr{},aimVptr{};
    template<class T> bool get(std::uintptr_t p,std::size_t off,T& out) const noexcept {
        return read&&p&&p<=UINTPTR_MAX-off-sizeof(T)&&read(nullptr,p+off,&out,sizeof(T));}
    bool vread(const diagnostics::ReadSegment* s,std::size_t n) const noexcept {
        try{return diagnostics::guardedReadVector.read(read,nullptr,s,n);}catch(...){return false;}}
};
inline Env env{};
// Engine classes: OK bits of the prints (elf::Class) + the time GOT + the aniAim vtable slots. 0 = not configured (UNCHECKED).
inline std::atomic<unsigned> classesOk{},classesChecked{};inline std::atomic<bool> gotOk{},vtableOk{},configured{};
inline std::atomic<std::uint32_t> printsFailed{};inline std::atomic<const char*> firstFailedPrint{"NONE"};
inline bool classOk(unsigned c) noexcept {return configured.load(std::memory_order_acquire)&&(classesOk.load(std::memory_order_relaxed)&c)==c;}
inline bool pOk() noexcept {return classOk(elf::ClassP);}
inline bool rOk() noexcept {return classOk(elf::ClassR)&&gotOk.load(std::memory_order_relaxed)&&vtableOk.load(std::memory_order_relaxed);}
inline bool oOk() noexcept {return classOk(elf::ClassO)&&gotOk.load(std::memory_order_relaxed);}
inline bool vOk() noexcept {return classOk(elf::ClassV);}
inline const char* classText(bool ok) noexcept {return !configured.load(std::memory_order_acquire)?"UNCHECKED":ok?"OK":"MISMATCH";}
inline const char* engineText() noexcept {
    if(!configured.load(std::memory_order_acquire))return "UNCHECKED";
    return pOk()&&rOk()&&oOk()?"OK":"MISMATCH";}
// TraceEntry: every print compared (mapping PROT_EXEC + bytes; `matched` = per print), the GOT word, the aniAim vtable slots.
inline void configureEngine(const Env& e,const std::array<bool,elf::prints.size()>& matched,bool got,bool vtable) noexcept {
    unsigned bad=0,checked=0;std::uint32_t failed=0;const char* first="NONE";
    for(std::size_t i=0;i<elf::prints.size();++i){checked|=elf::prints[i].classes;
        if(!matched[i]){bad|=elf::prints[i].classes;++failed;if(!std::strcmp(first,"NONE"))first=elf::prints[i].name;}}
    env=e;classesChecked.store(checked,std::memory_order_relaxed);classesOk.store(checked&~bad,std::memory_order_relaxed);
    gotOk.store(got,std::memory_order_relaxed);vtableOk.store(vtable,std::memory_order_relaxed);printsFailed.store(failed,std::memory_order_relaxed);
    firstFailedPrint.store(first,std::memory_order_relaxed);configured.store(true,std::memory_order_release);
}

// =====================================================================================================================
// Shared state (seqlocks / queue / atomics). Times = steady_clock ns (vr::nowNs, ControllerSample::publishedNs, the pad driver's now).
// Zone: pose thread -> XR / input. The spur in the HOLDING grip frame (metres), barrel / up there (unit). source 1 BONE, 2 FALLBACK.
enum ZoneSource : std::uint32_t {SourceNone=0,SourceBone=1,SourceFallback=2};
struct Zone {
    std::uint64_t weapon{},ns{},referenceSpace{};
    std::uint32_t generation{},source{};std::int32_t hand{-1},typeId{-1};
    V3 hammer{},fwd{},up{},boneModel{};
};
static_assert(sizeof(Zone)%8==0&&std::is_trivially_copyable_v<Zone>);
inline H::Seqlock<Zone> zoneBox{};
inline constexpr std::uint64_t zoneMaxAgeNs=500'000'000;
// Arm: input -> XR, every InputBrain::frame (all paths). armedUntilNs 0 = off.
enum ArmFlag : std::uint8_t {ArmWeapon=1,ArmNeedTrigger=2,ArmTorso=4,ArmContext=8,ArmSaDown=16};
struct Arm {std::uint64_t armedUntilNs{},readyAtNs{},graceNs{},suppressUntilNs{};std::uint32_t generation{};std::uint8_t mode{},hold{},off{},flags{};};
static_assert(sizeof(Arm)%8==0&&std::is_trivially_copyable_v<Arm>);
inline H::Seqlock<Arm> armBox{};
inline constexpr std::uint64_t armLeaseNs=60'000'000;
// Stroke: XR -> input.
enum StrokeKind : std::uint8_t {StrokeNone=0,StrokeHit=1,StrokeSlip=2,StrokeCock=3};
struct Stroke {std::uint64_t ns{};std::uint32_t generation{};std::uint8_t hand{},kind{},context{},trigger{};float vRel{},x{},y{},vAbs{};}; // trigger: the holding trigger was held at the stroke (REALISTIC fanning)
inline LockFreeQueue<Stroke,16> strokeQueue{};
inline constexpr std::uint64_t strokeMaxAgeNs=60'000'000;
// Facts: main (observe) -> input / report.
enum FactFlag : std::uint32_t {FactRead=1,FactAim=2,FactForce=4,FactPose=8,FactRiding=16,FactSingle=32,FactHeld=64,FactEligible=128,FactTime=256,
    FactOurs=512,FactRevolver=1024};
struct Facts {
    std::uint64_t ns{},weapon{},aimBits{};
    float T{},dtFrame{},lastFireT{},chamberUsed{},nativeChamber{},overrideS{},ammo{},perClip{},autoFireDelay{},aimPhase{},aimElapsed{},aimRate{},aimWindow{};
    std::int32_t aimEvent{},aimState{},attack240{},reload248{};
    std::uint32_t trigHold{},weaponFlags{},flags{},typeId{},pad{};
};
static_assert(sizeof(Facts)%8==0&&std::is_trivially_copyable_v<Facts>);
inline H::Seqlock<Facts> factsBox{};
inline constexpr std::uint64_t factsMaxAgeNs=250'000'000;
inline bool freshFacts(Facts& f,std::uint64_t now) noexcept {
    if(!factsBox.load(f)||!f.ns)return false;return (now>=f.ns?now-f.ns:f.ns-now)<=factsMaxAgeNs;}
inline bool freshZone(Zone& z,std::uint64_t now) noexcept {
    if(!zoneBox.load(z)||!z.ns||!z.source)return false;return (now>=z.ns?now-z.ns:z.ns-now)<=zoneMaxAgeNs;}
// XR -> input / render: the palm near the spur (FORCE AIM TRIGGER, H3 highlight), within 15 cm (H3 shown), the contact phase per hand
// (holsters hold guard / transfer block), the approach fraction (H2).
inline std::atomic<std::uint64_t> nearNs{},ringNearNs{},approachNs{};inline std::atomic<float> approachK{};
inline std::atomic<std::uint64_t> lastStrokeXrNs{};
// DRAG (XR -> pose / H2): the hammer fraction the free hand dragged (0 down .. 1 cocked) + stamp; FAN W/ TRIGGER held cocked (stamp
// refreshed every sample while waiting for the trigger); the last full cock (SA / wait: shown cocked until the input's state takes over).
inline std::atomic<float> dragKWord{};inline std::atomic<std::uint64_t> dragNs{},cockWaitNs{},cockShowNs{};
// Streak (input writes, main reads): the FAN series ends at streakUntilNs. SA "series" = SA active with the revolver in hand.
inline std::atomic<std::uint64_t> streakUntilNs{};inline std::atomic<bool> saSeries{};
inline std::atomic<std::uint64_t> lastFireNs{},lastClaimNs{};

// =====================================================================================================================
// Counters (report vr.weapon.hammer).
inline void bump(std::atomic<std::uint64_t>& a,std::uint64_t n=1) noexcept {a.fetch_add(n,std::memory_order_relaxed);}
inline void maxTo(std::atomic<std::uint64_t>& a,std::uint64_t v) noexcept {auto m=a.load(std::memory_order_relaxed);while(v>m&&!a.compare_exchange_weak(m,v)){}}
inline void minTo(std::atomic<std::uint64_t>& a,std::uint64_t v) noexcept {auto m=a.load(std::memory_order_relaxed);while((m==0||v<m)&&!a.compare_exchange_weak(m,v)){}}
enum Reject : unsigned {RejectNone,RejectNotArmed,RejectArmStale,RejectMenu,RejectTracking,RejectLateral,RejectHeight,RejectNoApproach,RejectParked,
    RejectPose,RejectSlow,RejectGunThrust,RejectGrip,RejectNoTrigger,RejectRearm,RejectNoForceAim,RejectCloseGrace,RejectCount};
inline constexpr std::array<const char*,RejectCount> rejectNames{"none","notArmed","armStale","menu","tracking","lateral","height","noApproach","parked",
    "pose","slow","gunThrust","grip","noTrigger","rearm","noForceAim","closeGrace"};
struct Counters {
    // XR
    std::atomic<std::uint64_t> xrSamples{},menuSkips{},crossNear{},crossFar{},strokesFan{},strokesCock{},strokesSlip{},xrUsSum{},xrUsMax{},queueFull{};
    std::array<std::atomic<std::uint64_t>,RejectCount> rejected{};std::array<std::atomic<std::uint64_t>,10> yHist{};
    // Arm / ownership
    std::atomic<std::uint64_t> armPublished{},armSuspended{},armStaleSeen{},ownershipFlips{},rearmLatched{},frames{};
    // strokes at the input
    std::atomic<std::uint64_t> strokesQueued{},strokesConsumed{},strokesStale{},strokesDiscarded{};
    // FAN
    std::atomic<std::uint64_t> pulses{},fired{},lost{},lostTrigger{},tooFast{},graced{},hitButDropped{},retries{},triggerPulses{},triggerFired{},
        dryEmpty{},dryCylinder{},dryContext{},dryNotRearmed{},dryReload{},emptyTriggerPassed{},latencyNsSum{},latencyNsMax{},latencyN{},
        intervalNsMin{},streaks{},noForceAim{},aimWaited{},merged{},
        mergedExpired{},triggerMerged{},claimsStale{}, // Open pulses across a claimed fire that expired (FAN / R1), fires claimed by a non-fresh pulse
        edgesMerged{}; // Forwarded R1 / SA edges that will not get a shot of their own (no pulse room, or their pulse merged) -> vr.weapon.fire
    // SA
    std::atomic<std::uint64_t> saCocks{},saThumbCocks{},saShots{},saDownPresses{},saRefused{},saAutoCockDual{},saDry{};
    std::atomic<std::uint64_t> dryTriggers{},fanShots{},cockedShots{},fanDry{}; // REALISTIC: trigger on a down hammer / trigger-held fan shots / cocked-trigger shots / fan strokes refused (empty / reload gate)
    // override
    std::atomic<std::uint64_t> chamberWrites{},chamberRestores{},chamberForeign{},chamberInvalid{},chamberSkippedNative{};
    // spread
    std::atomic<std::uint64_t> spreadFrames{},spreadShots{},spreadTagged{},naturalN{},naturalMdegSum{},naturalMdegMax{};
    std::array<std::atomic<std::uint64_t>,4> spreadHist{};
    // pose / visual
    std::atomic<std::uint64_t> poseCalls{},zonePublished{},zoneRefreshes{},zoneJumps{},zoneFailures{},nativeFrames{},nativeMoving{},changedWithoutClip{},
        visualApplied{},visualRestored{},visualDeclined{},visualPending{},visualRestoredPending{},visualPendingDropped{},ringShown{},ringBlinks{},observeCalls{},observeReads{},observeFailures{},ptrRefreshes{},
        observeUsSum{},observeUsMax{},poseUsSum{},poseUsMax{},triggerEdgesOwned{},stepCalls{},blockedSteps{};
    std::atomic<std::uint64_t> dragStarts{},dragCocks{},dragStrokes{},dragWaits{},dragWaitFired{},dragReturns{},dragTicks{},dragRejected{},dragMaxPermille{},
        visualDragFrames{},visualFallbackAxis{};
    // the zone filter (raw pose jumps, samples skipped while the grip moved fast / implausible / outliers) and the clean hammer rest
    std::atomic<std::uint64_t> zoneRawJumps{},zoneMovingSkips{},zoneOutliers{},zoneImplausible{},zoneRingResets{},restFromType{},restRejected{},restLearned{};
    std::array<std::atomic<std::uint64_t>,12> attempts{}; // Closed approach episodes by their final reason (Attempt)
};
inline Counters ctr{};
inline std::atomic<const char*> lastRestoreReason{"NONE"},lastVisualDecline{"NONE"},lastZoneFailure{"NONE"},eligibleWhy{"NO_WEAPON"};
inline std::atomic<float> spreadLastDeg{},naturalLastDeg{};inline std::atomic<std::uint64_t> spreadLastNs{};
inline std::atomic<bool> strokeSeenSession{};

// =====================================================================================================================
// Math (LOCAL metres, quaternions (x,y,z,w) as holsters::detail).
namespace m {
using namespace H::detail;
inline V3 lerp(V3 a,V3 b,float t) noexcept {return {a[0]+(b[0]-a[0])*t,a[1]+(b[1]-a[1])*t,a[2]+(b[2]-a[2])*t};}
inline V3 normalize(V3 v) noexcept {const float l=length(v);return l>1e-6f?mul(v,1.f/l):V3{};}
inline Q conjugate(Q q) noexcept {return {-q[0],-q[1],-q[2],q[3]};}
inline V3 toFrame(const Q& q,V3 v) noexcept {return rotate(conjugate(q),v);} // world -> frame of q
} // namespace m

// =====================================================================================================================
// Detector (pure, XR thread only): a backward SWIPE of any part of the free controller (grip origin, palm,
// front ring, top) through the capsule along the gun's rear top. Speed >= 0.55 m/s relative to the gun, the
// backward component dominant (>= 55 % of the relative speed), open-ish grip (<= .85). One stroke per swipe:
// re-armed once the hand slows below 0.15 m/s or leaves the capsule (+3 cm) AND 90 ms passed. `touch` = a
// swipe started (light contact haptic).
struct DetHand {V3 p{};Q q{0,0,0,1};bool valid{};};
struct DetIn {std::uint64_t ns{},space{};std::array<DetHand,2> grip{};std::array<float,2> grips{},triggers{};};
struct DetArm {bool armed{};int hold{-1};unsigned mode{ModeOff};bool needTrigger{},torsoOk{true};std::uint64_t readyAtNs{},graceNs{};};
struct DetZone {V3 hammer{},fwd{0,0,-1},up{0,1,0};bool valid{};};
struct DetOut {
    bool stroke{};std::uint8_t kind{StrokeNone};float vRel{},vAbs{};V3 x{};unsigned reason{RejectNone};bool crossing{},nearBox{};
    bool near{},ring{},contact{};float approachK{-1.f};V3 q{};bool computed{};
    bool touch{};
    bool dragStart{},dragTick{},dragCock{},dragWait{},dragWaitFired{},dragReturn{};
    bool contactEnd{}; // The drag contact ended (left the region / tracking lost)
    float dist{1e9f},distNF{1e9f};V3 qNF{};std::uint8_t startWhy{};bool handLost{}; // Capsule distance (floor / no floor), the start reason
};
// Contact report record, one per APPROACH of the free hand to the gun (every attempt, started or not; hammer frame:
// x right, y up, z barrel; metres): why = Attempt (the start reason, else the reject
// reason at the closest point), q = first sample near, qMin = closest (minDist from the capsule axis, no floor), q0 = where the drag started,
// grip = the max free grip, cocked 1 = drag full cock, 2 = swipe stroke, armFlags = the Arm flags at the closest point.
struct ContactRec {std::uint64_t ns{},endNs{},dragNs{};V3 q{},q0{},qEnd{},qMin{};std::uint8_t probe{},cocked{},why{},armFlags{};float maxK{},dist{},travel{},minDist{1e9f},grip{};};
enum Attempt : std::uint8_t {AttNone=0,AttStartIn=1,AttStartEnter=2,AttStartLatched=3,AttOutside=4,AttBelow=5,AttNotArmed=6,AttArmStale=7,AttZoneStale=8,
    AttZoneMismatch=9,AttMenu=10,AttHandLost=11,AttCount=12};
inline constexpr std::array<const char*,AttCount> attemptNames{"none","startIn","startEnter","startLatched","outside","belowFloor","notArmed","armStale",
    "zoneStale","zoneMismatch","menu","handLost"};
inline bool attemptStarted(unsigned w) noexcept {return w==AttStartIn||w==AttStartEnter||w==AttStartLatched;}
struct DetCfg {
    float halfW{.07f},yMin{.02f},yMax{.16f},vmin{.55f},cockVmin{.25f},vabs{.20f},cockVabs{.12f},grip{.85f},minElev{-.6427876f};
    std::uint64_t parkedNs{250'000'000},approachNs{150'000'000},rearmNs{60'000'000},maxGapNs{60'000'000};V3 palm{0.f,0.f,.03f};
    float contactVmin{.25f}; // The contact phase needs the open palm moving BACK at least this fast (min(vmin, cockVmin), >= .15)
    float capR{.12f},capFront{-.01f},capBack{.08f},capUp{.03f},capBelow{.04f},dominance{.55f};std::uint64_t refractoryNs{90'000'000};
    float fwdScale{2.5f},swipeR{.09f},downShare{.6f}; // Forward reach divided by fwdScale (clear of the cylinder); the fast swipe / force-aim near radius; the thumb arc's down weight
    float dragTravel{.025f},dragReturnS{.06f},dragRearm{.010f},dragAbsShare{.15f},dragTickM{.004f}; // DRAG (travel 0 = off)
    // The drag starts in the capsule (any grip) or up to enterMargin outside it while the hand moves back >= enterVmin relative to the
    // gun; ends beyond capR + leaveMargin; survives tracking gaps up to dragGapNs (progress re-seeded, never a jump); floor below the spur
    float enterMargin{.04f},enterVmin{.05f},leaveMargin{.05f},arcFloor{-.03f};std::uint64_t dragGapNs{300'000'000};
};
struct Sample16 {V3 q{};float vRel{},vAbs{},grip{};std::uint32_t valid{};};
struct Candidate {std::uint64_t ns{};unsigned reason{};std::array<Sample16,16> s{};unsigned n{};};
class Detector {
public:
    DetCfg cfg{};
    void reset() noexcept {have_=false;havePrev_=false;swipe_=false;swipeStroked_=false;lastHold_=-1;dropDrag();cockedWait_=false;}
    void dropDrag() noexcept {drag_=false;latched_=false;dragK_=0.f;dragTicks_=0;progress_=0.f;probeNs_=0;}
    const ContactRec& contact() const noexcept {return contact_;}
    float dragK() const noexcept {return dragK_;}
    bool cockedWait() const noexcept {return cockedWait_;}
    bool dragging() const noexcept {return drag_;}
    // distance of a hammer-frame point to the capsule axis (huge under the floor)
    float capsuleDist(const V3& q,bool floor=true) const noexcept {
        if(floor&&q[1]<-cfg.capBelow)return 1e9f;
        const float lo=std::min(-cfg.capBack,cfg.capFront),hi=std::max(-cfg.capBack,cfg.capFront);
        const float zc=std::clamp(q[2],lo,hi);const float dy=q[1]-cfg.capUp;float dz=q[2]-zc;
        if(dz>0.f)dz*=std::max(1.f,cfg.fwdScale); // Ahead of the region (toward the cylinder) counts fwdScale x
        return std::sqrt(q[0]*q[0]+dy*dy+dz*dz);}
    // One XR sample. Never throws.
    DetOut push(const DetIn& s,const DetZone& z,const DetArm& a) noexcept {
        DetOut o{};
        if(!a.armed||a.hold<0||a.hold>1||!z.valid){reset();o.reason=RejectNotArmed;return o;}
        const unsigned hold=unsigned(a.hold),off=1u-hold;
        if(!s.grip[hold].valid||!s.grip[off].valid){ // A short tracking loss (the controllers occlude each other at the gun) keeps the drag
            have_=false;havePrev_=false;swipe_=false;swipeStroked_=false;o.handLost=true;
            if(drag_&&(!probeNs_||s.ns<probeNs_||s.ns-probeNs_>cfg.dragGapNs)){drag_=false;dragK_=0.f;o.contactEnd=true;contact_.endNs=s.ns;}
            return o;}
        const auto& G=s.grip[hold];const auto& F=s.grip[off];
        const V3 c=m::add(G.p,m::rotate(G.q,z.hammer)),f=m::normalize(m::rotate(G.q,z.fwd)),u0=m::normalize(m::rotate(G.q,z.up));
        const V3 r=m::normalize(m::cross(f,u0)),u=m::cross(r,f);
        const auto toQ=[&](V3 w){const V3 d=m::sub(w,c);return V3{m::dot(d,r),m::dot(d,u),m::dot(d,f)};};
        const V3 p=F.p;const V3 q0=toQ(p); // the free grip origin = the velocity reference
        float dist=1e9f;V3 qn=q0;unsigned ni=0;
        const std::array<V3,4> probes{V3{0.f,0.f,0.f},cfg.palm,V3{0.f,0.f,-.06f},V3{0.f,.04f,0.f}};
        std::array<V3,4> qs{},pw{};
        for(unsigned i=0;i<probes.size();++i){pw[i]=m::add(F.p,m::rotate(F.q,probes[i]));qs[i]=toQ(pw[i]);const float dd=capsuleDist(qs[i]);if(dd<dist){dist=dd;qn=qs[i];ni=i;}
            const float dn=capsuleDist(qs[i],false);if(dn<o.distNF){o.distNF=dn;o.qNF=qs[i];}}
        o.q=qn;o.computed=true;o.dist=dist;
        const bool fan=a.mode!=ModeSingle;
        if(have_&&(s.space!=space_||a.hold!=lastHold_)){have_=false;havePrev_=false;swipe_=false;swipeStroked_=false;drag_=false;probeNs_=0;}
        else if(have_&&(s.ns<=lastNs_||s.ns-lastNs_>cfg.maxGapNs)){have_=false;havePrev_=false;swipe_=false;swipeStroked_=false;} // The drag re-seeds
        // tracking jumps: > 15 cm in one sample (either hand) = a glitch, never a stroke
        if(have_&&(m::length(m::sub(p,lastPalm_))>.15f||m::length(m::sub(G.p,lastHoldP_))>.15f)){
            if(dist<=cfg.capR+.05f){o.reason=RejectTracking;o.nearBox=true;}
            have_=false;havePrev_=false;swipe_=false;swipeStroked_=false;} // The drag re-seeds its probe (own 15 cm guard)
        const bool inCap=dist<=cfg.capR,open=s.grips[off]<=cfg.grip;
        const bool inSwipe=dist<=std::min(cfg.capR,cfg.swipeR); // The fast swipe / force-aim near use swipeR 9 cm (the drag region is capR 12 cm)
        o.near=inSwipe&&open;
        o.ring=m::length(qn)<=.15f&&open;
        if(std::fabs(qn[0])<=cfg.capR&&qn[1]>=-cfg.capBelow&&qn[2]>=0.f&&qn[2]<=.04f)o.approachK=(.04f-qn[2])/.04f;
        float vRelI=0.f,vAbsI=0.f,dt=0.f;bool moving=false;
        if(have_){
            dt=float(double(s.ns-lastNs_)*1e-9);
            vRelI=dt>0.f?-(q0[2]-lastQ_[2])/dt:0.f;vAbsI=dt>0.f?-m::dot(m::sub(p,lastPalm_),f)/dt:0.f;
            const float span=havePrev_?dt+prevDt_:dt;const V3 dq=m::sub(q0,havePrev_?prevQ_:lastQ_);
            const float vBack=span>0.f?-dq[2]/span:0.f,vLen=span>0.f?m::length(dq)/span:0.f;
            if(!rearmed_&&(vBack<.15f||dist>cfg.capR+.03f)&&s.ns-strokeNs_>=cfg.refractoryNs)rearmed_=true;
            moving=inSwipe&&vBack>=cfg.contactVmin;
            if(moving){
                if(!swipe_){swipe_=true;swipeStroked_=false;o.touch=true;swipeReason_=RejectSlow;swipeV_=0.f;swipeX_=qn;swipeAbs_=0.f;}
                unsigned why=RejectNone;
                if(!rearmed_||latched_)why=RejectRearm; // One shot per full cock (a drag's cock latches the swipe too)
                else if(vBack>8.f)why=RejectTracking;
                else if(fan&&!(f[1]>=cfg.minElev&&a.torsoOk))why=RejectPose; // LOCAL +Y is up
                else if(vBack<(fan?cfg.vmin:cfg.cockVmin))why=RejectSlow;
                else if(vBack<cfg.dominance*vLen)why=RejectLateral;          // the backward component is not dominant
                else if(vAbsI<(fan?cfg.vabs:cfg.cockVabs))why=RejectGunThrust; // the gun thrust forward under a still hand
                else if(!open)why=RejectGrip;
                else if(fan&&a.needTrigger&&s.triggers[hold]<=.55f)why=RejectNoTrigger;
                if(vBack>swipeV_){swipeV_=vBack;swipeX_=qn;swipeAbs_=vAbsI;}
                if(why==RejectNone){
                    rearmed_=false;strokeNs_=s.ns;swipeStroked_=true;latched_=true;latchZ_=1e9f;progress_=0.f;dragK_=0.f;dragTicks_=0; // = a full drag in one frame
                    o.stroke=true;o.crossing=true;o.nearBox=true;o.x=qn;o.vRel=vBack;o.vAbs=vAbsI;o.reason=RejectNone;
                    o.kind=fan?(a.readyAtNs&&s.ns+a.graceNs<a.readyAtNs?StrokeSlip:StrokeHit):StrokeCock;
                }else if(!swipeStroked_)swipeReason_=why;
            }else if(swipe_){ // the swipe ended without a stroke: one rejection report (near-miss ring, counters)
                if(!swipeStroked_){o.crossing=true;o.nearBox=true;o.reason=swipeReason_;o.x=swipeX_;o.vRel=swipeV_;o.vAbs=swipeAbs_;}
                swipe_=false;swipeStroked_=false;}
        }
        // DRAG. Full travel = cocked: FAN -> a HIT stroke and the hammer snaps forward; SINGLE ACTION -> a
        // COCK stroke (it stays cocked until the trigger, input state); FAN W/ TRIGGER -> a HIT when the
        // trigger is held, else it stays cocked (cockedWait) until the trigger is pulled (the R1 path fires
        // it). One shot per full cock: latched until the hand comes .dragRearm forward of its most-backward
        // point, or leaves. Leaving early: the hammer springs back over .dragReturn. The drag follows
        // the probe that touched first (a thumb-like wrist arc moves the front ring / top, not the grip origin), progress = its travel BACK along
        // the barrel relative to the gun + .downShare x its travel DOWN while on top of the gun (y > -1 cm, not moving forward) - the cocking arc.
        // Forward travel takes progress back; full cock at .dragTravel (2.5 cm); no speed limit; the gun-thrust guard keeps a .dragAbsShare share.
        if(cfg.dragTravel>0.f){
            const bool trig=s.triggers[hold]>.55f;
            if(cockedWait_&&(trig&&!trigWas_)){cockedWait_=false;o.dragWaitFired=true;}
            if(cockedWait_&&!(fan&&a.needTrigger))cockedWait_=false;
            trigWas_=trig;
            const float dtD=have_&&dt>0.f?dt:1.f/72.f;
            // ANY contact of the free controller starts the drag - any grip value, on the first sample in the capsule,
            // or up to .enterMargin outside while moving back.
            const bool leave=dist>cfg.capR+cfg.leaveMargin;
            const bool enter=dist<=cfg.capR+cfg.enterMargin&&vRelI>=cfg.enterVmin;
            bool started=false;
            const auto endContact=[&]{o.contactEnd=true;contact_.endNs=s.ns;contact_.qEnd=qs[probe_<qs.size()?probe_:0u];};
            if(leave){latched_=false;if(drag_){drag_=false;if(dragK_>.2f)o.dragReturn=true;endContact();}} // left the region (+5 cm): re-armed
            else if(!drag_&&(inCap||enter)){drag_=true;started=true;probe_=ni;progress_=0.f;dragTicks_=0;probeNs_=0;o.dragStart=true;
                o.startWhy=latched_?AttStartLatched:inCap?AttStartIn:AttStartEnter;
                contact_=ContactRec{};contact_.ns=s.ns;contact_.dragNs=s.ns;contact_.q=qn;contact_.q0=q0;contact_.probe=std::uint8_t(ni);contact_.dist=dist;
                contact_.why=o.startWhy;contact_.grip=s.grips[off];}
            if(drag_){
                const V3 qp=qs[probe_],wp=pw[probe_];
                // progress needs the previous sample of the same probe (within .dragGapNs, no 15 cm world jump): a gap / glitch only re-seeds
                const bool cont=!started&&probeNs_&&s.ns>probeNs_&&s.ns-probeNs_<=cfg.dragGapNs&&m::length(m::sub(wp,probeW_))<=.15f;
                if(cont){
                    const V3 dq=m::sub(qp,probeQ_),dw=m::sub(wp,probeW_);
                    const float back=-dq[2],down=-dq[1];
                    const bool arc=qp[1]>cfg.arcFloor&&back>-.002f; // the down-back cocking arc over the top-rear
                    float step=back+(arc?cfg.downShare*std::max(0.f,down):0.f);
                    const float absStep=-m::dot(dw,f)+(arc?cfg.downShare*std::max(0.f,-m::dot(dw,u)):0.f);
                    // the gun thrust forward under a still hand: no progress - FAN only (a full drag there is a SHOT); REALISTIC cocks on any
                    // relative backward travel (the hammer still needs the trigger)
                    if(fan&&step>0.f&&absStep<cfg.dragAbsShare*step)step=0.f;
                    if(std::fabs(step)>.05f)step=0.f;                     // > 5 cm in one sample: a glitch
                    if(latched_){latchZ_=std::min(latchZ_,qp[2]);if(qp[2]>=latchZ_+cfg.dragRearm){latched_=false;progress_=0.f;}}
                    else progress_=std::clamp(progress_+step,0.f,cfg.dragTravel);
                    if(!latched_&&step>0.f)contact_.travel+=step;
                }
                probeQ_=qp;probeW_=wp;probeNs_=s.ns;
                const float k=latched_||o.stroke?0.f:std::clamp(progress_/cfg.dragTravel,0.f,1.f);
                const int tick=int(k*cfg.dragTravel/std::max(cfg.dragTickM,.001f));
                if(tick>dragTicks_&&k<1.f)o.dragTick=true;dragTicks_=tick;
                dragK_=k;contact_.maxK=std::max(contact_.maxK,k);contact_.grip=std::max(contact_.grip,s.grips[off]);
                if(!latched_&&!o.stroke&&k>=1.f){ // full cock
                    unsigned why=RejectNone;
                    if(fan&&!(f[1]>=cfg.minElev&&a.torsoOk))why=RejectPose;
                    latched_=true;latchZ_=qp[2];progress_=0.f;rearmed_=false;strokeNs_=s.ns;dragK_=0.f;dragTicks_=0;if(swipe_)swipeStroked_=true;contact_.cocked=1;
                    o.crossing=true;o.nearBox=true;o.x=qn;o.vRel=vRelI;o.vAbs=vAbsI;o.dragCock=true;
                    if(why!=RejectNone)o.reason=why;
                    else if(fan&&a.needTrigger&&!trig){cockedWait_=true;o.dragWait=true;o.reason=RejectNone;}
                    else{o.stroke=true;o.reason=RejectNone;
                        o.kind=fan?(a.readyAtNs&&s.ns+a.graceNs<a.readyAtNs?StrokeSlip:StrokeHit):StrokeCock;}
                }
            }else if(dragK_>0.f){dragK_=std::max(0.f,dragK_-dtD/std::max(cfg.dragReturnS,.005f));dragTicks_=0;}
        }
        // the contact phase (holsters: the hold guard, the transfer block): only a STRIKE - the open hand moving back through the capsule, or a
        // stroke. Never mere presence near the gun (a support hand resting at it, a free hand reaching for the grip: DUAL transfer).
        o.contact=o.stroke||(open&&moving)||(drag_&&dragK_>0.f)||o.dragCock; // A drag in progress is a contact
        hist_[histNext_]=Sample16{qn,vRelI,vAbsI,s.grips[off],1u};histNext_=(histNext_+1)%16u;if(histN_<16)++histN_;
        if(have_){prevQ_=lastQ_;prevDt_=dt;havePrev_=true;}
        have_=true;lastNs_=s.ns;space_=s.space;lastQ_=q0;lastPalm_=p;lastHoldP_=G.p;lastHold_=a.hold;
        return o;
    }
    bool nearBoxQ(const V3& q) const noexcept {return capsuleDist(q)<=cfg.capR+.05f;}
    Candidate candidate(std::uint64_t ns,unsigned reason) const noexcept {
        Candidate c{};c.ns=ns;c.reason=reason;c.n=histN_;
        for(unsigned i=0;i<histN_;++i)c.s[i]=hist_[(histNext_+16u-histN_+i)%16u];
        return c;}
    bool rearmed() const noexcept {return rearmed_;}
private:
    bool drag_{},latched_{},cockedWait_{},trigWas_{};float latchZ_{},dragK_{},progress_{};int dragTicks_{};
    unsigned probe_{};V3 probeQ_{},probeW_{};std::uint64_t probeNs_{};ContactRec contact_{}; // The tracked probe (hammer frame / world), the contact report
    bool have_{},havePrev_{},rearmed_{true},swipe_{},swipeStroked_{};std::uint64_t lastNs_{},space_{},strokeNs_{};int lastHold_{-1};
    V3 lastQ_{},prevQ_{},lastPalm_{},lastHoldP_{},swipeX_{};float prevDt_{},swipeV_{},swipeAbs_{};unsigned swipeReason_{RejectSlow};
    std::array<Sample16,16> hist_{};unsigned histNext_{},histN_{};
};
// near-miss ring (XR writes under try_lock, the report reads)
inline std::mutex nearMissMutex;inline std::array<Candidate,6> nearMiss{};inline unsigned nearMissNext{};
inline Detector detector{};
// contact report ring (XR writes, the report reads): the last 16 APPROACHES of the free hand (started or rejected, attemptNote)
inline std::mutex contactMutex;inline std::array<ContactRec,16> contacts{};inline unsigned contactNext{};
inline DetCfg detCfgFrom(const Config& c) noexcept {
    DetCfg d{};d.halfW=c.halfW;d.yMin=c.yMin;d.yMax=c.yMax;d.vmin=c.vmin;d.cockVmin=c.cockVmin;d.vabs=c.vabs;d.cockVabs=c.cockVabs;d.grip=c.grip;
    d.minElev=std::sin(c.elevDeg*3.14159265f/180.f);d.parkedNs=c.parkedNs;d.palm=c.palm;d.contactVmin=std::max(.15f,std::min(c.vmin,c.cockVmin));
    d.capR=c.capR;d.capFront=c.capFront;d.capBack=c.capBack;d.capUp=c.capUp;d.dominance=c.dominance;d.refractoryNs=c.refractoryNs;
    d.capBelow=.04f;
    d.dragTravel=c.dragTravel;d.dragReturnS=c.dragReturnS;d.dragRearm=c.dragRearm;d.dragAbsShare=c.dragAbsShare;d.dragTickM=c.dragTickM;return d;}
inline DetHand handOf(const ::rdrvr::Pose& p) noexcept {
    DetHand h{};h.valid=H::detail::poseOk(p);if(!h.valid)return h;h.p={p.position[0],p.position[1],p.position[2]};h.q=p.orientation;return h;}

// Haptics (SourceHammer 9; refusals SourceRefuse).
inline void pulse(int hand,float amplitude,std::uint32_t us,std::uint64_t now,std::uint16_t source=haptics::SourceHammer) noexcept {
    if(hand==0||hand==1)haptics::pulse(unsigned(hand),amplitude,us,now,source);}

// Approach log (XR thread only; the ring under contactMutex): one record per approach of the free hand to the held gun - opened when
// its closest probe comes within capR + 10 cm of the capsule axis (no floor), closed 200 ms after it was last that near. why = the start reason,
// else the reject reason at the closest point (outside / below the floor / not armed / arm stale / zone stale or mismatched / menu); armFlags
// bit 0x80 = the free hand lost tracking during the approach.
struct AttemptState {bool open{};unsigned slot{~0u};std::uint64_t lastNearNs{};ContactRec rec{};};
inline AttemptState attemptState{};
inline constexpr float attemptNearM=.10f;inline constexpr std::uint64_t attemptCloseNs=200'000'000;
inline void attemptWrite(bool blocking) noexcept {
    auto& A=attemptState;if(A.slot>=contacts.size())return;
    if(blocking){std::lock_guard lock(contactMutex);contacts[A.slot]=A.rec;return;}
    std::unique_lock lock(contactMutex,std::try_to_lock);if(lock.owns_lock())contacts[A.slot]=A.rec;}
inline void attemptClose() noexcept {
    auto& A=attemptState;if(!A.open)return;
    A.rec.endNs=A.lastNearNs;attemptWrite(true);bump(ctr.attempts[std::min<unsigned>(A.rec.why,AttCount-1)]);A=AttemptState{};}
inline void attemptNote(std::uint64_t ns,bool haveQ,const V3& q,float distNF,unsigned why,float grip,unsigned armFlags,const DetOut* o) noexcept {
    auto& A=attemptState;
    if(why==AttHandLost){if(A.open)A.rec.armFlags=std::uint8_t(A.rec.armFlags|0x80u);}
    const bool near=haveQ&&distNF<=detector.cfg.capR+attemptNearM;
    if(!near){if(A.open&&(ns<A.lastNearNs||ns-A.lastNearNs>attemptCloseNs))attemptClose();return;}
    if(!A.open){
        {std::lock_guard lock(contactMutex);A.slot=contactNext;contactNext=(contactNext+1)%unsigned(contacts.size());}
        A.open=true;A.rec=ContactRec{};A.rec.ns=ns;A.rec.q=q;A.rec.why=std::uint8_t(why);A.rec.armFlags=std::uint8_t(armFlags&0x7fu);}
    A.lastNearNs=ns;A.rec.qEnd=q;A.rec.grip=std::max(A.rec.grip,grip);
    const bool start=o&&o->dragStart;
    if(start){A.rec.why=o->startWhy;A.rec.dragNs=ns;A.rec.armFlags=std::uint8_t((A.rec.armFlags&0x80u)|(armFlags&0x7fu));}
    else if(!attemptStarted(A.rec.why)&&distNF<A.rec.minDist){A.rec.why=std::uint8_t(why);A.rec.armFlags=std::uint8_t((A.rec.armFlags&0x80u)|(armFlags&0x7fu));}
    if(distNF<A.rec.minDist){A.rec.minDist=distNF;A.rec.qMin=q;}
    if(o&&(start||detector.dragging()||o->dragCock||o->contactEnd)){const auto& c=detector.contact();
        if(attemptStarted(A.rec.why)&&c.ns>=A.rec.ns){A.rec.q0=c.q;A.rec.probe=c.probe;A.rec.travel=c.travel;A.rec.dist=c.dist;
            A.rec.maxK=std::max(A.rec.maxK,c.maxK);if(c.cocked)A.rec.cocked=1;}}
    if(o&&o->stroke&&!o->dragCock&&!A.rec.cocked)A.rec.cocked=2;
    attemptWrite(start||(o&&(o->dragCock||o->stroke)));
}
// the free hand's closest probe in the hammer frame of a (possibly stale) zone - for the approaches the detector never sees (not armed, ...)
inline bool attemptQ(const runtime::ControllerSample& s,const Zone& z,V3& q,float& distNF) noexcept {
    if(z.hand!=0&&z.hand!=1)return false;const auto G=handOf(s.grip[unsigned(z.hand)]),F=handOf(s.grip[unsigned(1-z.hand)]);if(!G.valid||!F.valid)return false;
    const V3 c=m::add(G.p,m::rotate(G.q,z.hammer)),f=m::normalize(m::rotate(G.q,z.fwd)),u0=m::normalize(m::rotate(G.q,z.up));
    const V3 r=m::normalize(m::cross(f,u0)),u=m::cross(r,f);
    const std::array<V3,4> probes{V3{0.f,0.f,0.f},detector.cfg.palm,V3{0.f,0.f,-.06f},V3{0.f,.04f,0.f}};
    distNF=1e9f;
    for(const auto& pr:probes){const V3 d=m::sub(m::add(F.p,m::rotate(F.q,pr)),c);const V3 qq{m::dot(d,r),m::dot(d,u),m::dot(d,f)};
        const float dd=detector.capsuleDist(qq,false);if(dd<distNF){distNF=dd;q=qq;}}
    return distNF<1e8f;}

// XR thread (XrHarness, inside inputReady, after throwing::pushSample): the detector, the instant haptic, the stroke queue.
inline void xrSample(const runtime::ControllerSample& s,bool graphicsInput) noexcept {
    try{
        if(!started.load(std::memory_order_acquire))return;
        const auto t0=nowNs();bump(ctr.xrSamples);
        // an approach the detector never sees is still logged (the last zone, if the gun was held within 1.5 s)
        const auto gateLog=[&](unsigned why){Zone lz{};V3 q{};float d=1e9f;bool ok=false;float g=0.f;
            if(zoneBox.load(lz)&&lz.ns&&lz.source&&s.publishedNs>=lz.ns&&s.publishedNs-lz.ns<=1'500'000'000ull){ok=attemptQ(s,lz,q,d);if(ok)g=s.grips[unsigned(1-lz.hand)];}
            attemptNote(s.publishedNs,ok,q,d,why,g,0u,nullptr);};
        const auto menuNs=H::menuSeenNs.load(std::memory_order_relaxed);
        if(graphicsInput||(menuNs&&(s.publishedNs<menuNs||s.publishedNs-menuNs<H::menuFreezeNs))){detector.reset();bump(ctr.menuSkips);gateLog(AttMenu);return;}
        Arm a{};if(!armBox.load(a)||!a.armedUntilNs){detector.reset();gateLog(AttNotArmed);return;}
        if(s.publishedNs>a.armedUntilNs){detector.reset();bump(ctr.armStaleSeen);gateLog(AttArmStale);return;}
        Zone z{};if(!freshZone(z,s.publishedNs)){detector.reset();gateLog(AttZoneStale);return;}
        if(z.hand!=int(a.hold)||z.generation!=a.generation||(z.referenceSpace&&z.referenceSpace!=s.referenceSpace)){detector.reset();gateLog(AttZoneMismatch);return;}
        DetIn in{};in.ns=s.publishedNs;in.space=s.referenceSpace;for(unsigned h=0;h<2;++h){in.grip[h]=handOf(s.grip[h]);in.grips[h]=s.grips[h];in.triggers[h]=s.triggers[h];}
        DetZone dz{};dz.hammer=z.hammer;dz.fwd=z.fwd;dz.up=z.up;dz.valid=true;
        DetArm da{};da.armed=(a.flags&ArmWeapon)!=0&&a.mode!=ModeOff;da.hold=int(a.hold);da.mode=a.mode;da.needTrigger=(a.flags&ArmNeedTrigger)!=0;
        da.torsoOk=(a.flags&ArmTorso)!=0;da.readyAtNs=a.readyAtNs;da.graceNs=a.graceNs;
        const bool suppressed=a.suppressUntilNs&&s.publishedNs<a.suppressUntilNs;
        const auto o=detector.push(in,dz,da);
        const unsigned off=1u-a.hold;
        if(!da.armed)gateLog(AttNotArmed);
        else{ // This sample's start / reject reason for the approach log
            const unsigned why=o.dragStart?o.startWhy:detector.dragging()?AttStartIn:o.handLost?AttHandLost:o.dist>=1e8f?AttBelow:AttOutside;
            attemptNote(s.publishedNs,o.computed&&o.distNF<1e8f,o.qNF,o.distNF,why,in.grips[off&1u],a.flags,&o);}
        const bool saFan=a.mode==ModeSingle&&in.triggers[a.hold&1u]>.55f; // REALISTIC: a cock with the trigger held = a fan shot (the hammer drops)
        if(o.near)nearNs.store(s.publishedNs,std::memory_order_relaxed);
        if(o.ring)ringNearNs.store(s.publishedNs,std::memory_order_relaxed);
        if(o.contact)H::hammerContactNs[off].store(s.publishedNs,std::memory_order_relaxed);
        if(o.touch&&!o.stroke&&!suppressed&&(a.flags&ArmContext))pulse(int(off),.20f,12'000,s.publishedNs); // Contact tick (swipe started)
        { // DRAG: the visual fraction (H2), the ratchet texture, the counters
            const float k=detector.dragK();dragKWord.store(k,std::memory_order_relaxed);dragNs.store(s.publishedNs,std::memory_order_relaxed);
            if(detector.cockedWait())cockWaitNs.store(s.publishedNs,std::memory_order_relaxed);else if(o.dragWaitFired)cockWaitNs.store(0,std::memory_order_relaxed);
            maxTo(ctr.dragMaxPermille,std::uint64_t(k*1000.f));
            const bool feel=!suppressed&&(a.flags&ArmContext);
            if(o.dragStart){bump(ctr.dragStarts);if(feel&&!o.touch)pulse(int(off),.30f,12'000,s.publishedNs);}
            if(o.dragTick){bump(ctr.dragTicks);if(feel)pulse(int(off),.35f+.35f*k,9'000,s.publishedNs);} // A strong ratchet
            if(o.dragReturn)bump(ctr.dragReturns);
            if(o.dragWaitFired)bump(ctr.dragWaitFired);
            if(o.dragCock){bump(ctr.dragCocks);if(o.stroke)bump(ctr.dragStrokes);else if(o.dragWait)bump(ctr.dragWaits);else bump(ctr.dragRejected);
                if(o.stroke&&o.kind==StrokeCock&&!saFan)cockShowNs.store(s.publishedNs,std::memory_order_relaxed);
                if(o.dragWait){cockShowNs.store(s.publishedNs,std::memory_order_relaxed);if(feel)pulse(int(off),.90f,32'000,s.publishedNs);}} // Full cock click
        }
        if(o.approachK>=0.f){approachK.store(o.approachK,std::memory_order_relaxed);approachNs.store(s.publishedNs,std::memory_order_relaxed);}
        if(o.crossing){
            if(!o.nearBox)bump(ctr.crossFar);
            else{bump(ctr.crossNear);const int bin=int(std::floor((o.x[1]+.05f)/.03f));if(bin>=0&&bin<10)bump(ctr.yHist[unsigned(bin)]);}
        }
        if((o.crossing&&o.nearBox)||o.reason==RejectTracking){
            unsigned why=o.reason;if(why==RejectNone&&suppressed)why=RejectCloseGrace;
            if(why!=RejectNone&&why<RejectCount)bump(ctr.rejected[why]);
            if(o.crossing&&o.nearBox){std::unique_lock lock(nearMissMutex,std::try_to_lock);
                if(lock.owns_lock()){nearMiss[nearMissNext]=detector.candidate(s.publishedNs,why);nearMissNext=(nearMissNext+1)%unsigned(nearMiss.size());}}
        }
        if(o.stroke&&!suppressed){
            const bool ctxOk=(a.flags&ArmContext)!=0; // a vetoed context (Dead Eye, cover, horse, ...): queued for the input's dry.context, no haptic
            if(o.kind==StrokeSlip){bump(ctr.strokesSlip);bump(ctr.tooFast);if(ctxOk)pulse(int(off),.15f,25'000,s.publishedNs);}
            else{
                if(o.kind==StrokeCock)bump(ctr.strokesCock);else bump(ctr.strokesFan);
                strokeSeenSession.store(true,std::memory_order_relaxed);lastStrokeXrNs.store(s.publishedNs,std::memory_order_relaxed);
                if(ctxOk){if(o.kind==StrokeCock&&!saFan)pulse(int(off),.90f,32'000,s.publishedNs);else pulse(int(off),.80f,30'000,s.publishedNs);} // Full cock click
                Stroke k{};k.ns=s.publishedNs;k.generation=a.generation;k.hand=std::uint8_t(off);k.kind=o.kind;k.context=ctxOk?1:0;k.trigger=saFan?1:0;
                k.vRel=o.vRel;k.x=o.x[0];k.y=o.x[1];k.vAbs=o.vAbs;
                if(strokeQueue.push(k))bump(ctr.strokesQueued);else bump(ctr.queueFull);
            }
        }
        const auto us=(nowNs()-t0)/1000ull;bump(ctr.xrUsSum,us);maxTo(ctr.xrUsMax,us);
    }catch(...){}
}

// =====================================================================================================================
// In-flight pulses (input thread pushes / expires, any thread claims; tiny critical sections).
enum PulseKind : std::uint8_t {KindNone=0,KindTrigger=1,KindFan=2,KindSa=3};
inline const char* kindName(unsigned k) noexcept {return k==KindTrigger?"TRIGGER":k==KindFan?"FAN":k==KindSa?"SA":"NONE";}
// acrossFire: the pulse was still open when an engine fire of its weapon was claimed by an older pulse - it most likely merged into
// that shot (one request bit). A later fire prefers a FRESH pulse (not across a fire, this hold generation, a kind the current mode makes); an
// acrossFire pulse that expires is counted as merged, not lost.
struct Flight {bool on{},bit3Seen{},spread{},retried{},acrossFire{};std::uint8_t kind{};std::uint32_t generation{};std::uint64_t ns{},edgeNs{};std::uintptr_t weapon{};};
// mergedEdges: the physical edges (route()'s `now`, = the ns VrWeapon's pendingShot carries) of R1 / SA
// presses that were forwarded but get no shot of their own - no room in the pulse queue (merged at step),
// or their Trigger pulse stayed open across a shot another pulse claimed (triggerMerged). VrWeapon closes
// such a press as `mergedHammer`, not as a NO_ENGINE_FIRE refusal. Written by the input thread only.
inline std::array<std::atomic<std::uint64_t>,8> mergedEdges{};inline std::atomic<unsigned> mergedEdgeNext{};
inline void noteMergedEdge(std::uint64_t ns) noexcept {
    if(!ns)return;const auto i=mergedEdgeNext.fetch_add(1,std::memory_order_relaxed);mergedEdges[i%mergedEdges.size()].store(ns,std::memory_order_release);
    ctr.edgesMerged.fetch_add(1,std::memory_order_relaxed);}
inline bool edgeMerged(std::uint64_t ns) noexcept {if(!ns)return false;for(const auto& e:mergedEdges)if(e.load(std::memory_order_acquire)==ns)return true;return false;}
struct Lost {std::uint64_t ns{},bits{};std::int32_t state{},attack240{},reload248{};float phase{},window{},sinceFireGameS{},chamber{},ammo{};
    std::uint32_t weapon120{};bool bit3Seen{},valid{};std::uint8_t kind{};};
inline std::mutex flightMutex;inline std::array<Flight,4> flights{};inline Lost lostLast{};
inline std::atomic<std::uint32_t> saFiredSeq{},saRefusedSeq{};
struct Claim {bool claimed{};unsigned kind{KindNone};bool spread{};};
inline bool spreadTagged(std::uint64_t now) noexcept {const auto s=spreadLastNs.load(std::memory_order_relaxed);return s&&now>=s&&now-s<=60'000'000ull;}
inline bool kindFits(unsigned kind,unsigned md) noexcept {return md==ModeSingle?kind==KindSa:(md==ModeFan||md==ModeFanTrigger)&&(kind==KindTrigger||kind==KindFan);}
// FireProjectile of `weapon` (any thread, VrWeapon noteFire): the oldest FRESH open pulse of that
// weapon, else the oldest open one (FIFO). A FAN pulse merged into an earlier shot stays open until it
// expires; an R1 / SA shot inside that window must not be claimed by it.
inline Claim claimFire(std::uintptr_t weapon,std::uint64_t now) noexcept {
    Claim c{};if(!weapon||!started.load(std::memory_order_acquire))return c;
    Flight f{};
    {const unsigned md=mode();const auto gen=H::holdGeneration.load(std::memory_order_relaxed);
        std::lock_guard lock(flightMutex);Flight* best=nullptr;bool bestFresh=false;
        for(auto& x:flights){if(!x.on||x.weapon!=weapon)continue;
            const bool fresh=!x.acrossFire&&x.generation==gen&&kindFits(x.kind,md);
            if(!best||(fresh&&!bestFresh)||(fresh==bestFresh&&x.ns<best->ns)){best=&x;bestFresh=fresh;}}
        if(!best)return c;f=*best;best->on=false;if(!bestFresh)bump(ctr.claimsStale);
        for(auto& x:flights)if(x.on&&x.weapon==weapon&&x.ns<=now)x.acrossFire=true;}
    c.claimed=true;c.kind=f.kind;c.spread=f.spread||spreadTagged(now);
    const auto prev=lastClaimNs.exchange(now,std::memory_order_relaxed);
    if(prev&&now>prev&&now-prev<1'000'000'000ull&&(f.kind==KindFan||f.kind==KindSa||now<streakUntilNs.load(std::memory_order_relaxed)))minTo(ctr.intervalNsMin,now-prev);
    lastFireNs.store(now,std::memory_order_relaxed);
    if(f.kind==KindFan){bump(ctr.fired);if(now>=f.ns){const auto l=now-f.ns;bump(ctr.latencyNsSum,l);bump(ctr.latencyN);maxTo(ctr.latencyNsMax,l);}}
    else if(f.kind==KindSa){bump(ctr.saShots);saFiredSeq.fetch_add(1,std::memory_order_release);}
    else bump(ctr.triggerFired);
    if(c.spread){bump(ctr.spreadTagged);bump(ctr.spreadShots);}
    { // the barrel's own wander during the series (two latest observed barrels)
        const float n=naturalLastDeg.load(std::memory_order_relaxed);
        if(f.kind==KindFan&&n>=0.f){bump(ctr.naturalN);const auto md=std::uint64_t(n*1000.f);bump(ctr.naturalMdegSum,md);maxTo(ctr.naturalMdegMax,md);}
    }
    return c;
}

// =====================================================================================================================
// Main thread (PostCameraUpdate, vrWeaponObserve after publishOwner; also on its early return with weapon 0): the engine facts of the
// VR-held revolver in ONE readMemoryVector (the pointer chain weapon -> type / opMode, actor -> anim -> block -> aim is cached per weapon and
// refreshed every 30 calls), the override restore FIRST, then the override write, the Facts seqlock.
struct ObserveIn {
    std::uintptr_t weapon{},actor{},player{};std::uint32_t actorGuid{};int kind{-1};int typeId{-1};
    bool held{},forceAllowed{},poseAllowed{},riding{};
    V3 barrel{};std::uint64_t barrelNs{};bool barrelValid{};
    std::uint64_t now{};
};
struct Ptrs {std::uintptr_t weapon{},actor{},type{},opMode{},anim{},block{},aim{};std::uint32_t opIndex{};unsigned age{};bool valid{};};
inline Ptrs ptrs{};                                    // main thread only
inline std::uintptr_t oursWeapon{};inline float oursValue{};inline std::uint32_t oursGuid{}; // main thread only: the module's own +0x138 write
inline std::atomic<bool> chamberActive{};inline std::atomic<std::uint64_t> chamberActiveSinceNs{};
inline float prevT{},dtFrame{1.f/36.f};inline V3 barrelPrev{};inline std::uint64_t barrelPrevNs{};
inline bool refreshPtrs(std::uintptr_t weapon,std::uintptr_t actor) noexcept {
    namespace F=elf;ctr.ptrRefreshes.fetch_add(1,std::memory_order_relaxed);
    Ptrs p{};p.weapon=weapon;p.actor=actor;std::uint32_t idx{};
    {diagnostics::ReadSegment s[]{{weapon+F::wType,&p.type,8},{weapon+F::wOpIndex,&idx,4},{actor+F::aActorAnim,&p.anim,8}};
        if(!env.vread(s,3)||!p.type||!p.anim||idx>15)return false;}
    p.opIndex=idx;
    {diagnostics::ReadSegment s[]{{p.type+F::tOpModes+std::uintptr_t(idx)*8,&p.opMode,8},{p.anim+F::anBlock,&p.block,8}};
        if(!env.vread(s,2)||!p.opMode||!p.block)return false;}
    if(!env.get(p.block,F::blkAim,p.aim)||!p.aim)return false;
    p.valid=true;ptrs=p;return true;
}
// [weapon] == vtable weapWeapon and [weapon+0x20] == the player's GUID (the phantom check in GameXr): a stale pointer is never written.
inline bool weaponValid(std::uintptr_t w,std::uint32_t guid) noexcept {
    std::uintptr_t t{};std::uint32_t g{};return w&&env.weaponTable&&env.get(w,0,t)&&t==env.weaponTable&&env.get(w,elf::wOwnerGuid,g)&&g==guid;}
inline bool streakActive(std::uint64_t now) noexcept {return now<streakUntilNs.load(std::memory_order_relaxed)||saSeries.load(std::memory_order_relaxed);}
inline void restoreOverride(const char* why) noexcept {
    if(!oursWeapon)return;
    float v=-1.f;
    if(weaponValid(oursWeapon,oursGuid)&&env.get(oursWeapon,elf::wOverride,v)&&v==oursValue){const float zero=0.f;
        if(env.write&&env.write(oursWeapon+elf::wOverride,&zero,sizeof zero)){bump(ctr.chamberRestores);lastRestoreReason.store(why,std::memory_order_relaxed);}}
    else if(!weaponValid(oursWeapon,oursGuid))bump(ctr.chamberInvalid);
    oursWeapon=0;oursValue=0.f;chamberActive.store(false,std::memory_order_relaxed);
}
inline void observe(const ObserveIn& o) noexcept {
    namespace F=elf;
    try{
        if(!started.load(std::memory_order_acquire))return;
        const auto t0=nowNs();bump(ctr.observeCalls);
        const unsigned md=mode();
        const bool revolver=L::revolverKind(o.kind)&&L::modelMechanics(o.weapon); // Pistols / untrusted revolvers = NOT_REVOLVER (plain trigger)
        // 1. restore first (Dead Eye / cover / duel / linked anim, OFF, the weapon left the hand, the series ended, O mismatch)
        if(oursWeapon){
            const char* why=nullptr;
            if(!oOk())why="ENGINE";else if(md==ModeOff)why="OFF";else if(!o.weapon||o.weapon!=oursWeapon||!o.held)why="WEAPON_LEFT";
            else if(!o.forceAllowed)why="CONTEXT";else if(o.riding)why="HORSE";else if(!streakActive(o.now))why="STREAK_END";
            if(why)restoreOverride(why);
        }
        Facts f{};f.ns=o.now;f.weapon=o.weapon;f.typeId=std::uint32_t(o.typeId);
        if(o.forceAllowed)f.flags|=FactForce;if(o.poseAllowed)f.flags|=FactPose;if(o.riding)f.flags|=FactRiding;if(o.held)f.flags|=FactHeld;
        if(revolver)f.flags|=FactRevolver;
        // the barrel's own wander (spread.naturalDeg): angle between the two latest placements ~ one frame apart
        if(o.barrelValid&&o.barrelNs!=barrelPrevNs){
            if(barrelPrevNs&&o.barrelNs>barrelPrevNs&&o.barrelNs-barrelPrevNs<80'000'000ull){
                const float c=std::clamp(m::dot(m::normalize(o.barrel),m::normalize(barrelPrev)),-1.f,1.f);naturalLastDeg.store(std::acos(c)*57.29578f,std::memory_order_relaxed);}
            barrelPrev=o.barrel;barrelPrevNs=o.barrelNs;}
        const bool readable=o.weapon&&o.held&&revolver&&(rOk()||oOk())&&env.read;
        if(readable){
            if(!ptrs.valid||ptrs.weapon!=o.weapon||ptrs.actor!=o.actor||++ptrs.age>=30u){if(!refreshPtrs(o.weapon,o.actor))ptrs=Ptrs{};}
            if(ptrs.valid){
                std::uintptr_t wt{},aimVt{},aimAnim{};std::uint32_t guid{},flags{},trig{};float ammo{},lastFire{},ovr{},perClip{},chamber{},autoFire{},T{};
                float aimF[4]{};std::int32_t ev{},st{},a240{};std::uint8_t r248{};std::uint64_t bits{};
                diagnostics::ReadSegment s[20]{};std::size_t n=0;
                const auto add=[&](std::uintptr_t a,void* t,std::size_t z){s[n++]=diagnostics::ReadSegment{a,t,z};};
                add(o.weapon,&wt,8);add(o.weapon+F::wOwnerGuid,&guid,4);add(o.weapon+F::wFlags,&flags,4);add(o.weapon+F::wAmmo,&ammo,4);
                add(o.weapon+F::wLastFire,&lastFire,4);add(o.weapon+F::wOverride,&ovr,4);add(ptrs.type+F::tTrigHold,&trig,4);
                add(ptrs.opMode+F::oPerClip,&perClip,4);add(ptrs.opMode+F::oChamber,&chamber,4);add(ptrs.opMode+F::oAutoFire,&autoFire,4);
                add(ptrs.aim,&aimVt,8);add(ptrs.aim+F::aimOwner,&aimAnim,8);add(ptrs.aim+F::aimPhase,aimF,16);add(ptrs.aim+F::aimEvent,&ev,4);
                add(ptrs.aim+F::aimState,&st,4);add(ptrs.aim+F::aimBits,&bits,8);
                if(env.timeAddr)add(env.timeAddr,&T,4);
                if(o.player){add(o.player+F::pAttack,&a240,4);add(o.player+F::pReload,&r248,1);}
                bump(ctr.observeReads);
                if(env.vread(s,n)&&(!env.weaponTable||wt==env.weaponTable)&&guid==o.actorGuid){
                    const bool aimOk=env.aimVptr&&aimVt==env.aimVptr&&aimAnim==ptrs.anim;
                    if(!aimOk)ptrs.age=30u; // re-resolve the chain next call
                    f.T=T;f.lastFireT=lastFire;f.overrideS=ovr;f.nativeChamber=chamber;f.chamberUsed=ovr!=0.f?ovr:chamber;f.ammo=ammo;f.perClip=perClip;
                    f.autoFireDelay=autoFire;f.trigHold=trig;f.weaponFlags=flags;f.attack240=a240;f.reload248=r248;
                    if(env.timeAddr&&std::isfinite(T)){f.flags|=FactTime;const float dt=T-prevT;if(prevT>0.f&&dt>.001f&&dt<.2f)dtFrame=dt;prevT=T;}
                    f.dtFrame=dtFrame;
                    if(aimOk){f.flags|=FactAim;f.aimBits=bits;f.aimPhase=aimF[0];f.aimElapsed=aimF[1];f.aimRate=aimF[2];f.aimWindow=aimF[3];f.aimEvent=ev;f.aimState=st;}
                    if(rOk())f.flags|=FactRead;
                    if(trig==0u)f.flags|=FactSingle;
                }else bump(ctr.observeFailures);
            }else bump(ctr.observeFailures);
        }else if(!o.weapon||!o.held)ptrs=Ptrs{};
        // eligibility (ownership needs it; the trigger hold mode only with readable facts)
        const char* why=nullptr;
        if(!o.weapon||!o.held)why="NO_WEAPON";else if(!revolver)why="NOT_REVOLVER";else if(!o.forceAllowed)why="CONTEXT";else if(o.riding)why="HORSE";
        else if((f.flags&FactRead)&&!(f.flags&FactSingle))why="NOT_SINGLE";
        if(!why)f.flags|=FactEligible;
        eligibleWhy.store(why?why:"OK",std::memory_order_relaxed);
        // 2. the override (FAN series / SA active): only the VR-held revolver, never over a foreign value (a duel),
        // never above the native chamber
        if(oOk()&&!why&&md!=ModeOff&&config.chamberS>0.f&&streakActive(o.now)&&(f.flags&FactTime)){
            const float fan=config.chamberS;
            if(!(f.nativeChamber>fan)){bump(ctr.chamberSkippedNative);if(oursWeapon)restoreOverride("NATIVE_FASTER");}
            else if(f.overrideS==0.f||(oursWeapon==o.weapon&&f.overrideS==oursValue)){
                if(f.overrideS!=fan||oursWeapon!=o.weapon){
                    if(weaponValid(o.weapon,o.actorGuid)&&env.write&&env.write(o.weapon+F::wOverride,&fan,sizeof fan)){
                        bump(ctr.chamberWrites);oursWeapon=o.weapon;oursValue=fan;oursGuid=o.actorGuid;f.overrideS=fan;f.chamberUsed=fan;
                        if(!chamberActive.exchange(true,std::memory_order_relaxed))chamberActiveSinceNs.store(o.now,std::memory_order_relaxed);}
                    else bump(ctr.chamberInvalid);
                }
            }else{bump(ctr.chamberForeign);streakUntilNs.store(0,std::memory_order_relaxed);}
        }
        if(oursWeapon==o.weapon&&oursWeapon)f.flags|=FactOurs;
        factsBox.store(f);
        const auto us=(nowNs()-t0)/1000ull;bump(ctr.observeUsSum,us);maxTo(ctr.observeUsMax,us);
    }catch(...){}
}

// =====================================================================================================================
// FAN SPREAD (main thread, VrWeapon forceBarrelAim at the ManagerUpdate entry): during a FAN series every frame turns the barrel direction the
// engine aims along by a fresh draw of the cone yaw, pitch ~ N(0, sigma), clipped at 3 sigma (xorshift64, seeded by the call count).
struct Spread {bool active{};float yawDeg{},pitchDeg{};};
inline std::uint64_t spreadState{0x9e3779b97f4a7c15ull};
inline double uniform01() noexcept {spreadState^=spreadState<<13;spreadState^=spreadState>>7;spreadState^=spreadState<<17;return double((spreadState>>11)+1)*(1.0/9007199254740993.0);}
inline float gaussClipped(float sigma) noexcept {
    const double u1=uniform01(),u2=uniform01();const double g=std::sqrt(-2.0*std::log(u1))*std::cos(6.283185307179586*u2);
    return float(std::clamp(g,-3.0,3.0))*sigma;}
inline void seedSpread(std::uint64_t seed) noexcept {spreadState=seed?seed:0x9e3779b97f4a7c15ull;}
inline Spread spreadFor(std::uint64_t now) noexcept {
    Spread s{};
    const unsigned md=mode();if(md!=ModeFan&&md!=ModeFanTrigger)return s;
    const float sigma=sigmaDeg();if(!(sigma>0.f))return s;
    if(now>=streakUntilNs.load(std::memory_order_relaxed))return s;
    Facts f{};if(!freshFacts(f,now)||!(f.flags&FactEligible))return s;
    s.active=true;s.yawDeg=gaussClipped(sigma);s.pitchDeg=gaussClipped(sigma);
    const float deg=std::sqrt(s.yawDeg*s.yawDeg+s.pitchDeg*s.pitchDeg);spreadLastDeg.store(deg,std::memory_order_relaxed);
    spreadLastNs.store(now,std::memory_order_relaxed);bump(ctr.spreadFrames);
    bump(ctr.spreadHist[deg<1.f?0u:deg<2.f?1u:deg<4.f?2u:3u]);
    return s;
}

// =====================================================================================================================
// Pose (any thread, inside the VR-held weapon's own weapWeapon::Update after vrWeaponReload): the Zone (the spur in the holding grip frame
// of the placement this Update used), the native trace of the hammer bone, the self-gated visual H2.
struct VisualType {int typeId{-1};bool learned{};L::Mat down{},cocked{};float cockedDeg{};};
struct PoseRecord {
    std::uintptr_t weapon{},skeleton{};unsigned count{};int hammer{-1},cylinder{-1};bool children{};std::array<int,L::maxBones> parents{};
    std::array<std::array<char,32>,L::maxBones> names{};std::uint32_t generation{},restGeneration{};bool restValid{},written{},havePrev{};L::Mat rest{},last{},prevLocal{};
    float maxDeg{};V3 maxAxisModel{},restAxisModel{};std::uint64_t moving{};std::uint64_t lastNs{};V3 lastHammerGrip{};bool haveZone{};
    float clipExtremeDeg{};L::Mat clipExtreme{};std::uint64_t clipFireNs{};bool clipOpen{};
    // zone filter: the raw spur / barrel / up in the grip frame of the last zoneRing still samples; the published zone = their
    // component-wise median (the gun is rigid in the hand: the raw value only moves when the bone pose and the placement disagree in motion)
    std::array<V3,31> zh{},zf{},zu{};unsigned zn{},zNext{},zOut{};int zHand{-1};V3 rawLast{};bool haveRaw{};
    ::rdrvr::Pose gripPrev{};std::uint64_t gripPrevNs{};bool haveGripPrev{};
};
inline std::mutex poseMutex;inline PoseRecord poseRecord{};inline std::array<VisualType,6> visualTypes{};
// poseRecord follows ONE weapon. When another held weapon's Update reaches pose() first (a DUAL
// OpDrawKeep that leaves the old revolver as the visible phantom, a fast holster + draw), the old
// record's H2 write is parked here instead of dropped: written(old) stays true, so the old weapon's own
// next Update (pose held=false) puts its rest local + global back - inside its own write window, never
// from another weapon's Update. A mismatching skeleton / bone count on that Update drops the entry
// without writing (fail closed). poseMutex.
struct PendingRestore {std::uintptr_t weapon{},skeleton{};unsigned count{};int hammer{-1},parent{-1};L::Mat rest{};};
inline std::array<PendingRestore,2> pendingRestores{};
// Every H2 write stays absolute from that clean rest. poseMutex.
struct HammerInst {std::uintptr_t weapon{},skeleton{};int bone{-1};L::Mat rest{},last{};bool dirty{};std::uint64_t useNs{};};
struct HammerType {std::uintptr_t data{};unsigned count{};int bone{-1};L::Mat rest{};};
inline std::array<HammerInst,8> hammerInsts{};inline std::array<HammerType,8> hammerTypes{};
inline constexpr float restTolDeg=3.f;
struct TracePoint {float ms{},deg{};};
inline std::array<TracePoint,20> trace{};inline unsigned traceN{};inline std::uint64_t traceFireNs{};
struct BonesByType {int typeId{-1};char names[96]{};};
inline std::array<BonesByType,6> bonesByType{};
inline std::atomic<int> zoneBone{-1};inline std::atomic<const char*> visualState{"UNLEARNED"};inline std::atomic<float> visualCockedDeg{};
inline std::array<char,32> zoneBoneName{};inline std::atomic<std::uint32_t> zoneSourceSeen{};inline V3 zoneBoneModel{}; // poseMutex
enum ZoneState : int {ZoneUnknown=0,ZoneBone=1,ZoneFallback=2,ZoneNoBone=3};
inline std::atomic<int> zoneState{ZoneUnknown};
// the DRAG fallback axis (parent space): the hammer bone's own rest axis closest to the model's lateral axis (-1 = the lateral itself),
// sign-checked so +k swings the spur BACK (d spur / d angle = axis x spur, its share along -barrel must be > 0)
inline std::atomic<int> visualAxisCol{-2};inline std::atomic<bool> visualAxisFlipped{};inline V3 visualAxisParent{}; // poseMutex // the VR-held revolver's last pose: which bone the spur comes from (status NO HAMMER / FALLBACK)
inline int chooseHammer(const PoseRecord& r) noexcept {
    if(config.boneIndex>=0)return unsigned(config.boneIndex)<r.count?config.boneIndex:-1;
    if(config.boneName[0]){for(unsigned i=0;i<r.count;++i)if(!std::strcmp(r.names[i].data(),config.boneName.data()))return int(i);return -1;}
    for(unsigned i=0;i<r.count;++i)if(L::containsCi(r.names[i].data(),"hammer"))return int(i);
    return -1;
}
inline int chooseCylinder(const PoseRecord& r) noexcept {
    for(unsigned i=0;i<r.count;++i)if(L::containsCi(r.names[i].data(),"cyl"))return int(i);
    for(unsigned i=0;i<r.count;++i)if(L::containsCi(r.names[i].data(),"drum"))return int(i);
    return -1;
}
// rotation angle (deg) and unit axis of the 3x3 part of a * b^T (columns normalised) - the local's turn from rest.
inline float turnOf(const L::Mat& a,const L::Mat& b,V3& axis) noexcept {
    double R[3][3]{};
    const auto colN=[](const L::Mat& mm,unsigned c){V3 v{mm[c*4],mm[c*4+1],mm[c*4+2]};return m::normalize(v);};
    const V3 a0=colN(a,0),a1=colN(a,1),a2=colN(a,2),b0=colN(b,0),b1=colN(b,1),b2=colN(b,2);
    // R = A * B^T (row i, col j) = sum_k A[i][k] B[j][k]; A[i][k] = column k, row i
    const V3 A[3]{a0,a1,a2},B[3]{b0,b1,b2};
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j){double s=0;for(unsigned k=0;k<3;++k)s+=double(A[k][i])*double(B[k][j]);R[i][j]=s;}
    const double tr=std::clamp((R[0][0]+R[1][1]+R[2][2]-1.0)*.5,-1.0,1.0);const double ang=std::acos(tr);
    V3 ax{float(R[2][1]-R[1][2]),float(R[0][2]-R[2][0]),float(R[1][0]-R[0][1])};const float l=m::length(ax);
    axis=l>1e-6f?m::mul(ax,1.f/l):V3{};return float(ang*57.29577951308232);
}
inline L::Mat axisAngleMat(V3 axis,float radians) noexcept {
    L::Mat r{};r[15]=1;const float c=std::cos(radians),s=std::sin(radians),t=1.f-c;axis=m::normalize(axis);const float x=axis[0],y=axis[1],z=axis[2];
    r[0]=t*x*x+c;r[1]=t*x*y+s*z;r[2]=t*x*z-s*y;r[4]=t*x*y-s*z;r[5]=t*y*y+c;r[6]=t*y*z+s*x;r[8]=t*x*z+s*y;r[9]=t*y*z-s*x;r[10]=t*z*z+c;return r;}
struct PoseIn {std::uintptr_t weapon{},skeleton{};bool held{};int typeId{-1};std::uint32_t generation{};std::uint64_t now{};L::Mat modelMuzzle{};const L::GripMap* grip{};};
// The H2 target of this Update (input / XR facts): false = nothing to show (restore the rest pose when written).
inline std::atomic<int> saStateWord{0}; // input thread: 0 DOWN, 1 COCKED, 2 FIRING (H2 + report)
inline bool dragVisual() noexcept {return config.dragTravel>0.f;}
inline bool dragFresh(const std::atomic<std::uint64_t>& a,std::uint64_t now,std::uint64_t maxNs) noexcept {
    const auto t=a.load(std::memory_order_relaxed);return t&&(now>=t?now-t:t-now)<=maxNs;}
inline bool visualTarget(const VisualType& t,float& k,std::uint64_t now) noexcept {
    const unsigned md=mode();if(md==ModeOff)return false;
    if(dragVisual()){ // DRAG: the module drives the hammer while held - down at rest, following the free hand, cocked while SA / FAN W/ TRIGGER wait
        const float kd=dragFresh(dragNs,now,100'000'000ull)?std::clamp(dragKWord.load(std::memory_order_relaxed),0.f,1.f):0.f;
        const bool showCock=dragFresh(cockShowNs,now,150'000'000ull);
        if(md==ModeSingle){const int st=saStateWord.load(std::memory_order_relaxed);k=st==1?1.f:st==2?0.f:showCock?1.f:kd;return true;}
        if(md==ModeFanTrigger&&(dragFresh(cockWaitNs,now,100'000'000ull)||showCock)){k=1.f;return true;}
        k=kd;return true;}
    if(md==ModeSingle){const int st=saStateWord.load(std::memory_order_relaxed);if(st==2)return false;k=st==1?1.f:0.f;return true;}
    const auto a=approachNs.load(std::memory_order_relaxed);
    if(a&&now>=a&&now-a<=50'000'000ull){k=std::clamp(approachK.load(std::memory_order_relaxed),0.f,1.f);return true;}
    (void)t;return false;
}
// ROUNDS PROBE (whether the rounds the game's revolver cylinder already carries can be drawn or hidden):
// read-only dump of the held revolver's drawable (once per type): weapWeapon -> goh (aGuidGeneral slots) -> fragInst (gohProp::GetFragInst =
// goh+0xd0) -> fragType -> rmcDrawable -> lod 0..3 models (matrix index = bone, skin flag, geometries: shader effect name, vertex / primitive
// counts, bone palette, AABB). It shows whether the rounds are their own model (-> the per-instance model mask at the AddToDrawList hook), their
// own geometry (-> a per-instance drawable clone) or baked into the cylinder (-> keep the mod's own). librdr vaddrs.
namespace probe {
inline constexpr std::uintptr_t managerGot=0x2deabe0,slotsGot=0x2deabe8,getFragInstProp=0x24cb260,fragDrawableVptr=0x2d0bec0;
inline std::string text;inline std::array<int,6> types{-1,-1,-1,-1,-1,-1};inline std::atomic<std::uint64_t> runs{},failures{};
inline std::atomic<const char*> lastFailure{"NONE"};
}
inline void probeDrawable(const PoseRecord& r,std::uintptr_t weapon,int typeId) noexcept { // poseMutex held
    try{
        for(int t:probe::types)if(t==typeId)return;
        for(auto& t:probe::types)if(t<0){t=typeId;break;}
        probe::runs.fetch_add(1,std::memory_order_relaxed);
        const auto fail=[](const char* why){probe::failures.fetch_add(1,std::memory_order_relaxed);probe::lastFailure.store(why,std::memory_order_relaxed);};
        const auto& E=env;if(!E.read||!E.base){fail("NO_ENV");return;}
        const auto P=[&](std::uintptr_t a,std::size_t off,std::uintptr_t& out){return E.get(a,off,out)&&out&&(out&7u)==0;};
        std::uint16_t idx{},gen{},cnt{};std::uintptr_t mgrVar{},mgr{},slotsVar{},slots{},goh{},vt{},fn{},F{},node{},ce{},T{},D{},SG{};
        if(!E.get(weapon,0x2bc,idx)||!E.get(weapon,0x2be,gen)||!P(E.base+probe::managerGot,0,mgrVar)||!P(mgrVar,0,mgr)||!E.get(mgr,0,cnt)||idx>=cnt){fail("GUID");return;}
        if(!P(E.base+probe::slotsGot,0,slotsVar)||!P(slotsVar,0,slots)){fail("SLOTS");return;}
        std::uint16_t eg{};if(!E.get(slots+std::uintptr_t(idx)*16u,8,eg)||eg!=gen||!P(slots+std::uintptr_t(idx)*16u,0,goh)){fail("SLOT_GEN");return;}
        if(!P(goh,0,vt)||!E.get(vt,0x70,fn)||fn!=E.base+probe::getFragInstProp||!P(goh,0xd0,F)){fail("FRAGINST");return;}
        std::uint32_t nodeFlags{},ceFlags{};std::uintptr_t back{};if(P(F,0x110,node)){E.get(node,0x2c,nodeFlags);E.get(node,0xc0,back);}
        if(E.get(F,0x78,ce)&&ce&&E.get(ce,0xc,ceFlags)&&(ceFlags&0x30000000u)==0x30000000u){if(!P(ce,0,T)){fail("TYPE_CE");return;}}
        else if(!P(F,0x98,T)){fail("TYPE");return;}
        std::uintptr_t Dv{},alts{};std::int32_t altSel{};std::uint8_t breakables{};
        if(!P(T,0xc8,D)||!E.get(D,0,Dv)){fail("DRAWABLE");return;}
        E.get(T,0xd0,alts);E.get(T,0xe4,altSel);E.get(T,0x29d,breakables);
        if(!P(D,0x10,SG)){fail("SHADERGROUP");return;}
        std::uintptr_t shaders{};std::uint16_t nSh{};P(SG,0x40,shaders);E.get(SG,0x48,nSh);
        const auto str=[&](std::uintptr_t p){std::array<char,32> o{};if(!p)return std::string("?");
            for(std::size_t n:{std::size_t(31),std::size_t(15),std::size_t(7)})if(E.read(nullptr,p,o.data(),n)){o[n]=0;break;}
            std::string t(o.data());for(char& c:t)if(c=='"'||c=='\\'||static_cast<unsigned char>(c)<0x20||static_cast<unsigned char>(c)>0x7e)c='?';return t;};
        const auto fx=[&](unsigned i){std::uintptr_t S{},ef{},nm{};if(!shaders||i>=nSh||!P(shaders,i*8u,S)||!P(S,0x10,ef)||!E.get(ef,0x138,nm))return std::string("?");return str(nm);};
        char b[512];std::string o;
        std::snprintf(b,sizeof b,"{\"typeId\":%d,\"vptrOk\":%s,\"nodeFlags\":%u,\"nodeBackOk\":%s,\"breakables\":%u,\"alts\":%s,\"altSel\":%d,\"shaders\":[",typeId,
            Dv==E.base+probe::fragDrawableVptr?"true":"false",nodeFlags,back==F?"true":"false",unsigned(breakables),alts?"true":"false",altSel);o+=b;
        for(unsigned i=0;i<nSh&&i<48;++i){std::uintptr_t S{};std::uint8_t bucket{};if(shaders&&P(shaders,i*8u,S))E.get(S,0x19,bucket);
            std::snprintf(b,sizeof b,"%s[\"%s\",%u]",i?",":"",fx(i).c_str(),unsigned(bucket));o+=b;}
        o+="],\"lods\":[";
        for(unsigned lod=0;lod<4;++lod){std::uintptr_t Lp{},models{};std::uint16_t nM{};
            if(!E.get(D,0x50+8u*lod,Lp)||!Lp||!P(Lp,0,models)||!E.get(Lp,8,nM)){o+=lod?",null":"null";continue;}
            std::snprintf(b,sizeof b,"%s[",lod?",":"");o+=b;
            for(unsigned mi=0;mi<nM&&mi<24;++mi){std::uintptr_t M{},geoms{},sidx{},aabbs{};std::array<std::uint8_t,8> mb{};
                if(!P(models,mi*8u,M)||!E.read(nullptr,M+0x28,mb.data(),8)){o+=mi?",null":"null";continue;}
                P(M,8,geoms);E.get(M,0x20,sidx);E.get(M,0x18,aabbs);const unsigned nG=unsigned(mb[6])|(unsigned(mb[7])<<8);const unsigned bone=mb[3];
                std::snprintf(b,sizeof b,"%s{\"m\":%u,\"b28\":\"%02x%02x%02x%02x%02x%02x%02x%02x\",\"bone\":%u,\"boneName\":\"%s\",\"skin\":%u,\"geoms\":%u,\"g\":[",mi?",":"",mi,
                    mb[0],mb[1],mb[2],mb[3],mb[4],mb[5],mb[6],mb[7],bone,bone<r.count?r.names[bone].data():"?",unsigned(mb[5]),nG);o+=b;
                for(unsigned g=0;g<nG&&g<16;++g){std::uintptr_t G{},vb{},pal{};std::uint16_t sh{},vc{},nPal{};std::uint32_t prims{};std::uint8_t pt{};std::array<float,8> bb{};
                    if(!geoms||!P(geoms,g*8u,G)){o+=g?",null":"null";continue;}
                    if(sidx)E.get(sidx,g*2u,sh);if(P(G,0x18,vb))E.get(vb,8,vc);E.get(G,0x5c,prims);E.get(G,0x62,pt);E.get(G,0x68,pal);E.get(G,0x72,nPal);
                    const bool box=aabbs&&E.read(nullptr,aabbs+0x20u*(nG>1?1u+g:0u),bb.data(),sizeof bb);
                    std::array<std::uint16_t,8> ids{};if(pal&&nPal)E.read(nullptr,pal,ids.data(),2u*std::min<unsigned>(nPal,8u));
                    std::snprintf(b,sizeof b,"%s{\"sh\":%u,\"fx\":\"%s\",\"verts\":%u,\"prims\":%u,\"ptype\":%u,\"pal\":%u,\"palIds\":[%u,%u,%u,%u],\"aabbCm\":[%.1f,%.1f,%.1f,%.1f,%.1f,%.1f]}",
                        g?",":"",unsigned(sh),fx(sh).c_str(),unsigned(vc),prims,unsigned(pt),unsigned(nPal),unsigned(ids[0]),unsigned(ids[1]),unsigned(ids[2]),unsigned(ids[3]),
                        box?double(bb[0]*100.f):0.0,box?double(bb[1]*100.f):0.0,box?double(bb[2]*100.f):0.0,box?double(bb[4]*100.f):0.0,box?double(bb[5]*100.f):0.0,box?double(bb[6]*100.f):0.0);o+=b;}
                o+="]}";}
            o+="]";}
        o+="]}";
        if(probe::text.size()<12000){if(!probe::text.empty())probe::text+=",";probe::text+=o;}
    }catch(...){probe::failures.fetch_add(1,std::memory_order_relaxed);probe::lastFailure.store("EXCEPTION",std::memory_order_relaxed);}
}
inline HammerInst* hammerInstFor(std::uintptr_t w,std::uintptr_t skel,int bone) noexcept { // poseMutex
    for(auto& i:hammerInsts)if(i.weapon&&i.weapon==w&&i.skeleton==skel&&i.bone==bone)return &i;return nullptr;}
inline HammerType* hammerTypeFor(std::uintptr_t data,unsigned count,int bone) noexcept { // poseMutex
    for(auto& t:hammerTypes)if(t.data&&t.data==data&&t.count==count&&t.bone==bone)return &t;return nullptr;}
inline void noteHammerWrite(std::uintptr_t w,std::uintptr_t skel,int bone,const L::Mat& m,bool dirty,std::uint64_t now) noexcept { // poseMutex
    if(auto* i=hammerInstFor(w,skel,bone)){i->dirty=dirty;i->last=m;i->useNs=now;}}
// the clean rest of this instance's hammer bone (poseMutex): never the module's leftover write, never a cocked
// local when the type's clean rest is known
inline L::Mat cleanRest(std::uintptr_t w,std::uintptr_t skel,const L::Skeleton& k,int bone,const L::Mat& local,std::uint64_t now) noexcept {
    HammerType* t=hammerTypeFor(k.data,k.count,bone);
    const auto offType=[&](const L::Mat& c){V3 ax{};return t&&turnOf(c,t->rest,ax)>restTolDeg;};
    HammerInst* i=hammerInstFor(w,skel,bone);
    if(i){i->useNs=now;
        if(i->dirty&&local==i->last){bump(ctr.restRejected);return i->rest;} // the module's write is still on the bone
        if(local!=i->rest&&!offType(local)){i->rest=local;i->dirty=false;}    // native rewrote it (still DOWN): the new clean rest
        else if(offType(local))bump(ctr.restRejected);
        return i->rest;}
    // first sight of this instance (or evicted): its local unless it is turned off the type's rest
    HammerInst* n=nullptr;for(auto& x:hammerInsts)if(!x.weapon){n=&x;break;}
    if(!n){for(auto& x:hammerInsts)if(!x.dirty&&(!n||x.useNs<n->useNs))n=&x;}
    if(!n){for(auto& x:hammerInsts)if(!n||x.useNs<n->useNs)n=&x;}
    *n=HammerInst{};n->weapon=w;n->skeleton=skel;n->bone=bone;n->useNs=now;n->rest=local;
    if(offType(local)){n->rest=t->rest;bump(ctr.restRejected);bump(ctr.restFromType);}
    else if(!t){HammerType* slot=nullptr;for(auto& x:hammerTypes)if(!x.data){slot=&x;break;}if(!slot)slot=&hammerTypes[0];
        *slot=HammerType{k.data,k.count,bone,local};bump(ctr.restLearned);}
    else bump(ctr.restFromType);
    return n->rest;}
// component-wise median of the first n entries
inline V3 medianOf(const std::array<V3,31>& a,unsigned n) noexcept {
    V3 o{};if(!n)return o;std::array<float,31> t{};
    for(unsigned c=0;c<3;++c){for(unsigned i=0;i<n;++i)t[i]=a[i][c];std::nth_element(t.begin(),t.begin()+n/2,t.begin()+n);o[c]=t[n/2];}
    return o;}
inline void pose(const L::Engine& e,const PoseIn& in) noexcept {
    try{
        if(!started.load(std::memory_order_acquire)||!config.enabled)return;
        const auto t0=nowNs();bump(ctr.poseCalls);
        std::lock_guard lock(poseMutex);auto& r=poseRecord;
        const bool ours=r.weapon==in.weapon;
        // put a written hammer bone back to its rest local (+ global under its parent / the root); false = the skeleton does not match
        const auto restoreBone=[&](std::uintptr_t skel,unsigned count,int hammer,int p,const L::Mat& rest){
            L::Skeleton k{};if(hammer<0||in.skeleton!=skel||!e.skeleton(in.skeleton,k)||k.count!=count)return false;const unsigned hb=unsigned(hammer);
            if(!e.store(k.locals+hb*L::localStride,rest))return false;
            L::Mat parent{};
            if((p>=0&&unsigned(p)<k.count&&hb&&e.matrix(k.globals+unsigned(p)*L::globalStride,parent))||e.matrix(k.root,parent))
                e.store(k.globals+hb*L::globalStride,L::multiply(parent,rest));
            bump(ctr.visualRestored);return true;};
        for(auto& pr:pendingRestores)if(pr.weapon&&pr.weapon==in.weapon){ // This weapon's parked H2 write (held again, or not)
            if(!restoreBone(pr.skeleton,pr.count,pr.hammer,pr.parent,pr.rest)){bump(ctr.visualDeclined);lastVisualDecline.store("PENDING_SKELETON",std::memory_order_relaxed);}
            else bump(ctr.visualRestoredPending);
            pr=PendingRestore{};}
        if(!in.held||!L::modelMechanics(in.weapon)){ // A weapon we wrote, seen no longer held: put its rest local back once
            if(ours&&r.written&&r.hammer>=0)restoreBone(r.skeleton,r.count,r.hammer,r.parents[unsigned(r.hammer)],r.rest);
            if(ours)r.written=false;
            return;}
        if(!ours||r.skeleton!=in.skeleton){
            if(!ours&&r.weapon&&r.written&&r.hammer>=0&&r.restValid){ // Park the old weapon's restore (never dropped)
                PendingRestore* slot=&pendingRestores[0];for(auto& pr:pendingRestores)if(!pr.weapon||pr.weapon==r.weapon){slot=&pr;break;}
                if(slot->weapon&&slot->weapon!=r.weapon)bump(ctr.visualPendingDropped); // both slots busy: the oldest (slot 0) is overwritten
                *slot=PendingRestore{r.weapon,r.skeleton,r.count,r.hammer,r.parents[unsigned(r.hammer)],r.rest};bump(ctr.visualPending);}
            r=PoseRecord{};r.weapon=in.weapon;r.skeleton=in.skeleton;}
        L::Skeleton k{};if(!in.skeleton||!e.skeleton(in.skeleton,k)){bump(ctr.zoneFailures);lastZoneFailure.store("NO_SKELETON",std::memory_order_relaxed);return;}
        if(!r.count){
            for(unsigned i=0;i<k.count;++i){e.name(k,i,r.names[i]);if(!e.parent(k,i,r.parents[i])){lastZoneFailure.store("PARENTS",std::memory_order_relaxed);bump(ctr.zoneFailures);return;}}
            r.count=k.count;r.hammer=chooseHammer(r);r.cylinder=chooseCylinder(r);
            if(r.hammer>=0)for(unsigned i=0;i<r.count;++i)if(int(i)!=r.hammer&&r.parents[i]==r.hammer)r.children=true;
            zoneBone.store(r.hammer>=0?r.hammer:r.cylinder,std::memory_order_relaxed);
            {const auto& nm=r.hammer>=0?r.names[unsigned(r.hammer)]:r.cylinder>=0?r.names[unsigned(r.cylinder)]:std::array<char,32>{};zoneBoneName=nm;}
            // bones by type (report): which revolvers have no hammer bone
            bool known=false;for(const auto& b:bonesByType)if(b.typeId==in.typeId)known=true;
            if(!known)for(auto& b:bonesByType)if(b.typeId<0){b.typeId=in.typeId;std::size_t at=0;
                for(unsigned i=0;i<r.count&&at+2<sizeof b.names;++i){for(std::size_t j=0;r.names[i][j]&&at+2<sizeof b.names;++j)b.names[at++]=r.names[i][j];if(at+2<sizeof b.names)b.names[at++]=' ';}
                if(at)b.names[at-1]=0;break;}
            probeDrawable(r,in.weapon,in.typeId); // Once per type
        }else if(r.count!=k.count){r=PoseRecord{};lastZoneFailure.store("BONE_COUNT",std::memory_order_relaxed);bump(ctr.zoneFailures);return;}
        const int bone=r.hammer>=0?r.hammer:r.cylinder;
        zoneState.store(r.hammer>=0?ZoneBone:r.cylinder>=0?ZoneFallback:ZoneNoBone,std::memory_order_relaxed);
        if(bone<0){Zone z{};z.weapon=in.weapon;z.ns=in.now;z.source=SourceNone;z.hand=in.grip?in.grip->hand:-1;z.typeId=in.typeId;zoneBox.store(z);
            lastZoneFailure.store("NO_HAMMER",std::memory_order_relaxed);return;}
        // the spur in world: the bone's global origin + the offset along the weapon's own axes (authored muzzle locator: barrel -Z, up +Y)
        L::Mat root{},global{};
        if(!e.matrix(k.root,root)||!e.matrix(k.globals+unsigned(bone)*L::globalStride,global)){bump(ctr.zoneFailures);lastZoneFailure.store("GLOBAL_UNREADABLE",std::memory_order_relaxed);return;}
        const L::Mat mw=L::multiply(root,in.modelMuzzle);
        const V3 barrelW=L::normalized(L::mul(L::col(mw,2),-1.f)),upW0=L::normalized(L::col(mw,1));
        const V3 rightW=L::normalized(L::cross(barrelW,upW0)),upW=L::cross(rightW,barrelW);
        const float s=L::length(L::col(root,0));if(!(s>1e-6f)||!std::isfinite(s)){bump(ctr.zoneFailures);return;}
        V3 spur=L::col(global,3);
        const V3 so=spurOffset(in.weapon); // The held model's spur
        if(r.hammer>=0)spur=L::add(spur,L::mul(L::add(L::add(L::mul(rightW,so[0]),L::mul(upW,so[1])),L::mul(barrelW,so[2])),s));
        else spur=L::add(spur,L::mul(L::sub(L::mul(upW,config.fallbackUp),L::mul(barrelW,config.fallbackBack)),s));
        // model-space position of the bone (report)
        {L::Mat rootInv{};if(L::affineInverse(root,rootInv)){const V3 bm=L::point(rootInv,L::col(global,3));zoneBoneModel={bm[0]*100.f,bm[1]*100.f,bm[2]*100.f};}}
        if(in.grip&&in.grip->valid){
            const L::Mat gripToWorld=L::multiply(in.grip->map,L::poseMat(in.grip->grip));L::Mat inv{};
            if(L::affineInverse(gripToWorld,inv)){
                Zone z{};z.weapon=in.weapon;z.ns=in.now;z.referenceSpace=in.grip->referenceSpace;z.generation=in.generation;z.hand=in.grip->hand;
                z.typeId=in.typeId;z.source=r.hammer>=0?SourceBone:SourceFallback;
                z.hammer=L::point(inv,spur);z.fwd=L::normalized(L::direction(inv,barrelW));z.up=L::normalized(L::direction(inv,upW));z.boneModel=zoneBoneModel;
                bool finite=true;for(const auto& v:{z.hammer,z.fwd,z.up})for(float x:v)finite=finite&&std::isfinite(x);
                // the gun is rigid in the grip, so the spur in the grip
                // frame is a constant per hold; the raw value only moves when the bone pose (last frame's placement) and this placement disagree
                // while the hand moves. Published = the median of the raw samples taken while the grip was still (< .35 m/s, < 1.5 rad/s); a
                // sample > 4 cm off the median is an outlier (16 in a row = a real change: the ring restarts); implausible (> 25 cm) never used.
                if(finite&&m::length(z.hammer)<.25f){
                    if(r.haveRaw&&m::length(m::sub(z.hammer,r.rawLast))>.02f)bump(ctr.zoneRawJumps);
                    r.rawLast=z.hammer;r.haveRaw=true;
                    if(r.zHand!=z.hand){r.zn=0;r.zNext=0;r.zOut=0;r.zHand=z.hand;}
                    bool still=true;
                    {const auto& gp=in.grip->grip;
                        if(r.haveGripPrev&&in.now>r.gripPrevNs){const float dt=float(double(in.now-r.gripPrevNs)*1e-9);
                            if(dt>0.f&&dt<.2f){const V3 d{gp.position[0]-r.gripPrev.position[0],gp.position[1]-r.gripPrev.position[1],gp.position[2]-r.gripPrev.position[2]};
                                float dot=0.f;for(unsigned i=0;i<4;++i)dot+=gp.orientation[i]*r.gripPrev.orientation[i];
                                const float ang=2.f*std::acos(std::clamp(std::fabs(dot),0.f,1.f));
                                still=m::length(d)/dt<.35f&&ang/dt<1.5f;}}
                        r.gripPrev=gp;r.gripPrevNs=in.now;r.haveGripPrev=true;}
                    bool use=still; // moving: not sampled (the first zone of a hold is the raw value until a still sample arrives)
                    if(use&&r.zn>=5){const V3 med=medianOf(r.zh,r.zn);
                        if(m::length(m::sub(z.hammer,med))>.04f){bump(ctr.zoneOutliers);use=false;if(++r.zOut>=16){r.zn=0;r.zNext=0;r.zOut=0;bump(ctr.zoneRingResets);use=true;}}
                        else r.zOut=0;}
                    if(!still)bump(ctr.zoneMovingSkips);
                    if(use){r.zh[r.zNext]=z.hammer;r.zf[r.zNext]=z.fwd;r.zu[r.zNext]=z.up;r.zNext=(r.zNext+1)%unsigned(r.zh.size());if(r.zn<r.zh.size())++r.zn;}
                    if(r.zn){z.hammer=medianOf(r.zh,r.zn);z.fwd=m::normalize(medianOf(r.zf,r.zn));z.up=m::normalize(medianOf(r.zu,r.zn));}
                    if(r.haveZone&&r.generation==in.generation&&m::length(m::sub(z.hammer,r.lastHammerGrip))>.02f)bump(ctr.zoneJumps);
                    if(!r.haveZone||r.generation!=in.generation)bump(ctr.zoneRefreshes);
                    r.lastHammerGrip=z.hammer;r.haveZone=true;r.generation=in.generation;zoneBox.store(z);bump(ctr.zonePublished);
                    zoneSourceSeen.store(z.source,std::memory_order_relaxed);
                }else if(finite){bump(ctr.zoneImplausible);lastZoneFailure.store("IMPLAUSIBLE",std::memory_order_relaxed);}
                else{bump(ctr.zoneFailures);lastZoneFailure.store("NOT_FINITE",std::memory_order_relaxed);}
            }
        }
        // ---- the native trace of the hammer bone + the self-gated visual ----
        if(r.hammer>=0){
            const unsigned hb=unsigned(r.hammer);L::Mat local{};std::uintptr_t clip{},phasePtr{};
            if(e.matrix(k.locals+hb*L::localStride,local)){
                e.get(in.weapon,elf::wAnimClip,clip);e.get(in.weapon,elf::wAnimPhasePtr,phasePtr);
                const bool ourWrite=r.written&&local==r.last;
                if(!r.restValid||(r.restGeneration!=in.generation&&!ourWrite)){ // Never the module's own (cocked) write as the rest
                    r.rest=cleanRest(in.weapon,in.skeleton,k,int(hb),local,in.now);
                    r.restValid=true;r.restGeneration=in.generation;r.maxDeg=0;r.clipOpen=false;r.havePrev=false;}
                if(!ourWrite){
                    bump(ctr.nativeFrames);
                    V3 ax{};const float deg=turnOf(local,r.rest,ax);
                    const L::Mat parentToModel=[&]{L::Mat rootInv{},pg{};const int p=r.parents[hb];
                        if(!L::affineInverse(root,rootInv))return L::Mat{};
                        if(p>=0&&unsigned(p)<k.count&&e.matrix(k.globals+unsigned(p)*L::globalStride,pg))return L::multiply(rootInv,pg);
                        return L::Mat{};}();
                    const V3 axModel=m::normalize(L::direction(parentToModel,ax));
                    if(deg>1.f)bump(ctr.nativeMoving);
                    if(deg>r.maxDeg){r.maxDeg=deg;r.maxAxisModel=axModel;}
                    if(r.havePrev&&local!=r.prevLocal&&!clip&&!phasePtr)bump(ctr.changedWithoutClip); // ActionController (a change without a clip)
                    r.prevLocal=local;r.havePrev=true;
                    // the clip after a shot: learn the extreme (rest = DOWN or COCKED by the turn about r = f x u)
                    const auto fire=lastFireNs.load(std::memory_order_relaxed);
                    if(fire&&fire!=r.clipFireNs&&in.now>=fire&&in.now-fire<600'000'000ull){r.clipFireNs=fire;r.clipOpen=true;r.clipExtremeDeg=0;}
                    if(r.clipOpen){
                        if(deg>r.clipExtremeDeg){r.clipExtremeDeg=deg;r.clipExtreme=local;}
                        if(fire&&traceFireNs!=fire){traceFireNs=fire;traceN=0;}
                        if(traceN<trace.size())trace[traceN++]=TracePoint{float(double(in.now-fire)/1e6),deg};
                        if(in.now-r.clipFireNs>=600'000'000ull){
                            r.clipOpen=false;V3 ea{};const float ed=turnOf(r.clipExtreme,r.rest,ea);
                            const V3 eaModel=m::normalize(L::direction(parentToModel,ea));
                            const V3 rModel=m::normalize(m::cross(m::normalize(V3{-in.modelMuzzle[8],-in.modelMuzzle[9],-in.modelMuzzle[10]}),
                                                                   m::normalize(V3{in.modelMuzzle[4],in.modelMuzzle[5],in.modelMuzzle[6]})));
                            const float along=m::dot(eaModel,rModel);
                            if(std::fabs(along)>=.9f&&ed>=10.f&&ed<=70.f){
                                for(auto& t:visualTypes)if(t.typeId==in.typeId||t.typeId<0){t.typeId=in.typeId;t.learned=true;
                                    t.cocked=along>0.f?r.clipExtreme:r.rest;t.down=along>0.f?r.rest:r.clipExtreme;t.cockedDeg=ed;break;}
                                visualState.store("LEARNED",std::memory_order_relaxed);visualCockedDeg.store(ed,std::memory_order_relaxed);
                            }
                        }
                    }
                    r.restAxisModel=axModel;
                }
                // H2 write: learned + no native movement without a clip this session + no children + the property
                const VisualType* vt=nullptr;for(const auto& t:visualTypes)if(t.learned&&t.typeId==in.typeId)vt=&t;
                const char* decline=nullptr;float kf=0.f;L::Mat target{};
                if(config.visual==0)decline="OFF";
                else if(!vOk())decline="ENGINE";
                else if(r.children)decline="CHILDREN";
                // DRAG: written every held Update after the native pose (as RELOAD writes the cylinder) - the ActionController gate would
                // keep the hammer still forever after one native change without a clip
                else if(!dragVisual()&&ctr.changedWithoutClip.load(std::memory_order_relaxed))decline="ACTION_CONTROLLER";
                else if(config.visual==2){ // probe: .hammer.visual force <deg> about the model's r axis
                    const V3 rModel=m::normalize(m::cross(m::normalize(V3{-in.modelMuzzle[8],-in.modelMuzzle[9],-in.modelMuzzle[10]}),
                                                          m::normalize(V3{in.modelMuzzle[4],in.modelMuzzle[5],in.modelMuzzle[6]})));
                    L::Mat modelToParent{};L::Mat rootInv{},pg{};const int p=r.parents[hb];
                    if(L::affineInverse(root,rootInv)&&p>=0&&e.matrix(k.globals+unsigned(p)*L::globalStride,pg)&&L::affineInverse(L::multiply(rootInv,pg),modelToParent)){
                        const V3 axP=m::normalize(L::direction(modelToParent,rModel));
                        target=L::multiply(axisAngleMat(axP,config.visualForceDeg*3.14159265f/180.f),r.rest);
                        for(unsigned i=0;i<3;++i)target[12+i]=r.rest[12+i];
                    }else decline="FORCE_AXES";
                }
                else if(!vt&&!dragVisual())decline="UNLEARNED";
                else if(!visualTarget(vt?*vt:VisualType{},kf,in.now))decline="IDLE";
                else if(vt){ // slerp DOWN -> COCKED by kf (rotation about the learned axis, translation lerp); DRAG: .drag maxDeg (0 = the learned angle)
                    V3 ax{};const float learned=turnOf(vt->cocked,vt->down,ax);const float deg=dragVisual()&&config.dragDeg>0.f?config.dragDeg:learned;
                    const float tk=learned>1e-3f?std::min(1.f,kf*deg/learned):kf;
                    target=L::multiply(axisAngleMat(ax,kf*deg*3.14159265f/180.f),vt->down);
                    for(unsigned i=0;i<3;++i)target[12+i]=vt->down[12+i]+(vt->cocked[12+i]-vt->down[12+i])*tk;
                    if(dragVisual())bump(ctr.visualDragFrames);
                }else{ // DRAG before the type was learned (no shot yet): the rest (= DOWN) turned about the model's r axis by sign * maxDeg * k
                    const V3 rModel=m::normalize(m::cross(m::normalize(V3{-in.modelMuzzle[8],-in.modelMuzzle[9],-in.modelMuzzle[10]}),
                                                          m::normalize(V3{in.modelMuzzle[4],in.modelMuzzle[5],in.modelMuzzle[6]})));
                    L::Mat modelToParent{};L::Mat rootInv{},pg{};const int p=r.parents[hb];
                    if(L::affineInverse(root,rootInv)&&p>=0&&e.matrix(k.globals+unsigned(p)*L::globalStride,pg)&&L::affineInverse(L::multiply(rootInv,pg),modelToParent)){
                        const V3 latP=m::normalize(L::direction(modelToParent,rModel));
                        // the hammer bone's own lateral axis (its rest column closest to the model's right), sign-checked against the spur
                        V3 axP=latP;int col=-1;float best=.7f;
                        for(unsigned c=0;c<3;++c){const V3 bc=m::normalize(V3{r.rest[c*4],r.rest[c*4+1],r.rest[c*4+2]});const float d=m::dot(bc,latP);
                            if(std::fabs(d)>best){best=std::fabs(d);col=int(c);axP=d<0.f?m::mul(bc,-1.f):bc;}}
                        const V3 barrelM=m::normalize(V3{-in.modelMuzzle[8],-in.modelMuzzle[9],-in.modelMuzzle[10]}),upM=m::normalize(V3{in.modelMuzzle[4],in.modelMuzzle[5],in.modelMuzzle[6]});
                        const V3 backP=m::normalize(L::direction(modelToParent,m::mul(barrelM,-1.f)));
                        const V3 so=spurOffset(in.weapon);
                        const V3 spurM=m::add(m::add(m::mul(rModel,so[0]),m::mul(upM,std::max(so[1],.01f))),m::mul(barrelM,so[2]));
                        const V3 spurP=L::direction(modelToParent,spurM);
                        const bool flip=m::dot(m::cross(axP,spurP),backP)<0.f;if(flip)axP=m::mul(axP,-1.f);
                        visualAxisCol.store(col,std::memory_order_relaxed);visualAxisFlipped.store(flip,std::memory_order_relaxed);visualAxisParent=axP;
                        target=L::multiply(axisAngleMat(axP,config.dragSign*config.dragDeg*kf*3.14159265f/180.f),r.rest);
                        for(unsigned i=0;i<3;++i)target[12+i]=r.rest[12+i];bump(ctr.visualFallbackAxis);bump(ctr.visualDragFrames);
                    }else decline="FORCE_AXES";
                }
                if(!decline&&L::finiteMat(target)){
                    if(e.store(k.locals+hb*L::localStride,target)){
                        L::Mat parent=root;const int p=r.parents[hb];
                        if(p>=0&&unsigned(p)<k.count)e.matrix(k.globals+unsigned(p)*L::globalStride,parent);
                        e.store(k.globals+hb*L::globalStride,L::multiply(parent,target));
                        L::Mat back{};r.last=e.matrix(k.locals+hb*L::localStride,back)?back:target;r.written=true;bump(ctr.visualApplied);
                        noteHammerWrite(in.weapon,in.skeleton,int(hb),r.last,true,in.now);
                    }else{bump(ctr.visualDeclined);lastVisualDecline.store("WRITE_FAILED",std::memory_order_relaxed);}
                }else{
                    if(decline&&std::strcmp(decline,"IDLE")&&std::strcmp(decline,"OFF")){bump(ctr.visualDeclined);lastVisualDecline.store(decline,std::memory_order_relaxed);}
                    if(r.written&&r.restValid){ // nothing to show: back to the rest pose once
                        if(e.store(k.locals+hb*L::localStride,r.rest)){L::Mat parent=root;const int p=r.parents[hb];
                            if(p>=0&&unsigned(p)<k.count)e.matrix(k.globals+unsigned(p)*L::globalStride,parent);
                            e.store(k.globals+hb*L::globalStride,L::multiply(parent,r.rest));bump(ctr.visualRestored);
                            noteHammerWrite(in.weapon,in.skeleton,int(hb),r.rest,false,in.now);}
                        r.written=false;}
                }
            }
        }
        const auto us=(nowNs()-t0)/1000ull;bump(ctr.poseUsSum,us);maxTo(ctr.poseUsMax,us);
    }catch(...){}
}
// A weapon whose hammer bone was written, seen in a later Update while not VR-held: restore it (GameXr calls pose for it).
inline bool written(std::uintptr_t weapon) noexcept {
    std::lock_guard lock(poseMutex);if(!weapon)return false;if(poseRecord.weapon==weapon&&poseRecord.written)return true;
    for(const auto& pr:pendingRestores)if(pr.weapon==weapon)return true;
    return false;}

// =====================================================================================================================
// Input thread (holsters InputBrain through holsters::hammerSinks): frameBegin (first line of every frame), ownsRt, step (route), marker.
enum SaState : int {SaDown=0,SaCocked=1,SaFiring=2};
inline const char* saName(int s) noexcept {return s==SaCocked?"COCKED":s==SaFiring?"FIRING":"DOWN";}
enum class Verdict : std::uint8_t {Live,Dry,Empty};
// reload::Verdict codes through the holsters' RtGateSink (0 Pass, 1 PassNative, 2 BlockEmpty, 3 BlockSpent, 4 BlockOneShot); sources 0 Trigger,
// 1 FanStroke, 2 SaTrigger (= reload::GateSource).
inline constexpr int gateTrigger=0,gateFan=1,gateSa=2;
inline const char* gateBlockName(int v) noexcept {return v==3?"SPENT_CHAMBER":v==4?"ONE_SHOT":"EMPTY_CHAMBER";}
struct Waiting {bool on{};std::uint64_t ns{},deadline{};bool graced{},aim{};};
class Brain {
public:
    // ---- frame level (every InputBrain::frame, every path) ----
    void frameBegin(const H::HammerFrame& f) noexcept {
        now_=f.now;bump(ctr.frames);
        if(lastFrameNs_&&f.now>lastFrameNs_){const auto d=f.now-lastFrameNs_;if(d>=8'000'000ull&&d<=100'000'000ull)frameNs_=(frameNs_*7+d)/8;}
        lastFrameNs_=f.now;
        { // The held sidearm's action from the engine hand item's type name (written by the holsters' inventory sample)
            const auto name=H::handName.load();heldAction.store(f.hold>=0?classifyAction(name.c_str()):ActionUnknown,std::memory_order_relaxed);}
        const unsigned md=mode();
        const int hold=f.hold;const auto gen=H::holdGeneration.load(std::memory_order_relaxed);
        if(gen!=lastGen_||hold!=lastHold_){lastGen_=gen;lastHold_=hold;sa_=config.drawCocked?SaCocked:SaDown;cancelPulses();waiting_={};rawPass_=false;thumbWas_=false;}
        if(md!=ModeSingle||f.menu)sa_=SaDown;
        {const auto fs=saFiredSeq.load(std::memory_order_acquire);if(fs!=saFiredSeen_){saFiredSeen_=fs;if(sa_==SaFiring)sa_=SaDown;}}
        Facts facts{};const bool haveFacts=freshFacts(facts,f.now);
        expireFlights(facts,haveFacts,f.now);
        {const auto rs=saRefusedSeq.load(std::memory_order_acquire);if(rs!=saRefusedSeen_){saRefusedSeen_=rs;if(sa_==SaFiring)sa_=SaCocked;}}
        const auto e=eligibility(hold,f.now,&facts,haveFacts);
        cacheHold_=hold;cacheNs_=f.now;cacheOwns_=e.owns;cacheBase_=e.base;cacheValid_=true;
        if(owned_!=e.owns){owned_=e.owns;bump(ctr.ownershipFlips);if(!owned_){cancelPulses();waiting_={};rawPass_=false;}}
        // SA auto-cock: a DUAL phantom in the free hand (no free palm to cock with)
        if(md==ModeSingle&&e.base&&sa_==SaDown){const int ph=H::phantomHand.load(std::memory_order_relaxed);
            if(ph>=0&&ph==1-hold){sa_=SaCocked;bump(ctr.saAutoCockDual);L::cockAdvance(hold,f.now);}} // CHAMBERS REALISTIC turns the cylinder
        // torso line (FAN "presented"): the holding grip >= .present fwd ahead of the neck along the head's yaw
        if(f.head&&f.in&&hold>=0&&hold<=1&&H::detail::poseOk(*f.head)&&H::detail::poseOk(f.in->grip[unsigned(hold)])){
            const auto& hd=*f.head;const V3 neck=m::add(V3{hd.position[0],hd.position[1],hd.position[2]},m::rotate(hd.orientation,{0.f,-H::neckDown,H::neckBack}));
            V3 fw=H::detail::forwardOf(hd);fw[1]=0.f;const float l=m::length(fw);
            if(l>.2f){fw=m::mul(fw,1.f/l);const auto& g=f.in->grip[unsigned(hold)].position;
                torsoOk_=m::dot(m::sub(V3{g[0],g[1],g[2]},neck),fw)>=config.fwdM;}
        }
        // strokes of this frame
        nReqs_=0;Stroke k{};
        while(strokeQueue.pop(k)){
            if(f.suspended){bump(ctr.strokesDiscarded);continue;}
            const auto age=f.now>=k.ns?f.now-k.ns:0;
            if(age>strokeMaxAgeNs||k.generation!=gen){bump(ctr.strokesStale);continue;}
            if(!e.base){bump(ctr.strokesDiscarded);continue;}
            if(!k.context||!e.ctx){bump(ctr.dryContext);continue;}   // Dead Eye / cover / duel / horse: the swipe does nothing
            if(nReqs_<reqs_.size()){reqs_[nReqs_++]=k;bump(ctr.strokesConsumed);}else bump(ctr.strokesDiscarded);
        }
        // Arm (XR): off when suspended / not eligible / the free hand busy
        Arm a{};
        if(!f.suspended&&e.base&&offEmpty_&&md!=ModeOff){
            a.armedUntilNs=f.now+armLeaseNs;a.generation=gen;a.mode=std::uint8_t(md);a.hold=std::uint8_t(hold);a.off=std::uint8_t(1-hold);
            a.flags=std::uint8_t(ArmWeapon|(md==ModeFanTrigger?ArmNeedTrigger:0)|(torsoOk_?ArmTorso:0)|(e.ctx?ArmContext:0)|(md==ModeSingle&&sa_==SaDown?ArmSaDown:0));
            a.graceNs=frameNs_+10'000'000ull;a.readyAtNs=readyAt(facts,haveFacts,f.now);
            const auto closed=L::closedAtNs();if(closed)a.suppressUntilNs=closed+config.closeGraceNs;
            bump(ctr.armPublished);
        }else if(f.suspended)bump(ctr.armSuspended);
        armBox.store(a);
        // holsters glue: FORCE AIM TRIGGER (palm near the spur keeps force aim), the hold guard, the SA cocked bit
        const bool fanMode=md==ModeFan||md==ModeFanTrigger;
        if(fanMode&&e.base&&!f.suspended){const auto n=nearNs.load(std::memory_order_relaxed);if(n&&f.now>=n&&f.now-n<=100'000'000ull)H::hammerAimNs.store(f.now,std::memory_order_relaxed);}
        // The hold guard: max(the FAN series, the free palm's contact phase + 300 ms) - the contact part
        // in EVERY mode (an SA cocking swipe shakes the holding hand too); SA COCKED by itself never holds
        // (a cocked revolver is still put away by opening the hand)
        std::uint64_t guard=0;
        if(md!=ModeOff&&e.base&&hold>=0&&hold<=1){
            const auto c=H::hammerContactNs[unsigned(1-hold)].load(std::memory_order_relaxed);
            guard=c?c+300'000'000ull:0ull;
            if(fanMode)guard=std::max<std::uint64_t>(guard,streakUntilNs.load(std::memory_order_relaxed));}
        H::hammerHoldGuardUntilNs.store(guard,std::memory_order_relaxed);
        H::hammerCocked.store(md==ModeSingle&&e.base&&sa_==SaCocked,std::memory_order_relaxed);
        saSeries.store(md==ModeSingle&&e.base&&e.ctx,std::memory_order_relaxed);
        saStateWord.store(sa_,std::memory_order_relaxed);
    }
    // ---- route() ----
    bool ownsRt(int hold,std::uint64_t now) noexcept {
        if(cacheValid_&&cacheHold_==hold&&cacheNs_==now)return cacheOwns_;
        Facts f{};const bool have=freshFacts(f,now);return eligibility(hold,now,&f,have).owns;}
    void step(const runtime::ControllerSample& in,const H::HammerIn& hi,H::HammerOut& out) noexcept {
        out=H::HammerOut{};bump(ctr.stepCalls);
        const unsigned md=mode();const auto now=hi.now;const int hold=hi.hold;
        offEmpty_=hi.offEmpty;
        if(hi.block){ // NOT_REARMED / MELEE / CYLINDER_OPEN: the caller zeroes RT, strokes dropped
            const bool cyl=!std::strcmp(hi.block,"CYLINDER_OPEN");
            for(unsigned i=0;i<nReqs_;++i)bump(cyl?ctr.dryCylinder:ctr.dryNotRearmed);
            for(auto& w:waiting_)if(w.on){bump(cyl?ctr.dryCylinder:ctr.dryNotRearmed);w.on=false;}
            nReqs_=0;cancelPulses();rawPass_=false;if(cyl&&md==ModeSingle&&sa_!=SaFiring)sa_=SaDown;
            bump(ctr.blockedSteps);saStateWord.store(sa_,std::memory_order_relaxed);return;}
        const auto closed=L::closedAtNs();const bool closeGrace=closed&&now>=closed&&now-closed<config.closeGraceNs;
        if(rawPass_){ // The physical trigger on an empty clip: RT follows it until released (native reload / weapon switch)
            if(hi.physDown&&!hi.physEdge){out.rawPass=true;dropRequests(closeGrace);finish(out);return;}
            rawPass_=false;}
        // SA cock (never calls the gate, never moves the chamber). REALISTIC fanning: a full cock with the holding trigger held
        // (at the XR stroke or now) fires at once - one gate call, one SA pulse, the hammer drops (each slap = one shot, one per full cock).
        bool fanned=false;
        if(md==ModeSingle){
            for(unsigned i=0;i<nReqs_;++i){if(reqs_[i].kind!=StrokeCock)continue;
                if(closeGrace){bump(ctr.dryCylinder);continue;}
                if(reqs_[i].trigger||hi.physDown){
                    if(emptyRaw(now)){sa_=SaDown;bump(ctr.fanDry);bump(ctr.dryEmpty);pulse(hold,.20f,30'000,now,haptics::SourceRefuse);blink(now);continue;}
                    if(!pulseRoom()){bump(ctr.merged);continue;}
                    if(sa_!=SaCocked)L::cockAdvance(hold,now); // The full cock turns the cylinder before the fall (CHAMBERS REALISTIC)
                    H::HammerOut dummy{};const auto v=verdict(gateSa,hold,now,dummy);
                    if(v==Verdict::Live){sa_=SaFiring;startPulse(KindSa,now);bump(ctr.fanShots);fanned=true;}
                    else{sa_=SaDown;bump(ctr.fanDry);if(v==Verdict::Dry)bump(ctr.saDry);else{bump(ctr.dryEmpty);pulse(hold,.20f,30'000,now,haptics::SourceRefuse);blink(now);}}
                    continue;}
                if(sa_==SaDown){sa_=SaCocked;bump(ctr.saCocks);pulse(hold,.30f,25'000,now);L::cockAdvance(hold,now);}}
            if(config.thumbCock&&hold==1){const bool back=in.sticks[3]<=-.7f;
                if(back&&!thumbWas_&&sa_==SaDown){sa_=SaCocked;bump(ctr.saCocks);bump(ctr.saThumbCocks);pulse(hold,.30f,25'000,now);L::cockAdvance(hold,now);}thumbWas_=back;}
        }
        // R1: the physical trigger edge
        if(hi.physEdge){
            bump(ctr.triggerEdgesOwned);
            // the gate (RELOAD turns the chamber on a Pass) only for a pulse that is really sent - no room in the pulse queue = merged
            // (counted, no gate call); the empty rule still passes the trigger raw
            const bool room=pulseRoom()||emptyRaw(now);
            if(md==ModeSingle){
                if(sa_==SaCocked){
                    if(!room){bump(ctr.merged);noteMergedEdge(now);}
                    else{const auto v=verdict(gateSa,hold,now,out);
                        if(v==Verdict::Live){sa_=SaFiring;startPulse(KindSa,now);bump(ctr.cockedShots);}
                        else if(v==Verdict::Dry){sa_=SaDown;bump(ctr.saDry);}
                        else{sa_=SaDown;rawPass_=true;out.rawPass=true;bump(ctr.emptyTriggerPassed);}}
                }else if(fanned){} // This press fires through the fan stroke of this frame (no edgeBlock: it would zero the pulse's RT)
                else if(emptyRaw(now)){rawPass_=true;out.rawPass=true;bump(ctr.emptyTriggerPassed);} // empty + not manual: never a soft-lock
                else{ // REALISTIC: a dry trigger on a down hammer does nothing but a faint click (+ the ring blink)
                    out.edgeBlock="HAMMER_DOWN";bump(ctr.saDownPresses);bump(ctr.dryTriggers);pulse(hold,.10f,12'000,now,haptics::SourceRefuse);blink(now);}
            }else if(!room){bump(ctr.merged);noteMergedEdge(now);}
            else{
                const auto v=verdict(gateTrigger,hold,now,out);
                if(v==Verdict::Live){startPulse(KindTrigger,now);bump(ctr.triggerPulses);}
                else if(v==Verdict::Empty){rawPass_=true;out.rawPass=true;bump(ctr.emptyTriggerPassed);}
            }
        }
        // R2: FAN strokes (the waiting ones first: one frame of tempo grace / force aim up to graceNs)
        if(md==ModeFan||md==ModeFanTrigger){
            for(auto& w:waiting_)if(w.on)fanTry(w,hold,now);
            if(retryPending_){retryPending_=false;if(config.retry&&rOk()&&!pressLeft_&&!gapLeft_&&!queued_){begin(KindFan,now,true);bump(ctr.retries);}}
            for(unsigned i=0;i<nReqs_;++i){if(reqs_[i].kind!=StrokeHit)continue;
                if(closeGrace){bump(ctr.dryCylinder);continue;}
                // the empty clip (no manual RELOAD) from the Facts first - no gate call; RELOAD's gate (Pass turns the chamber, a dry
                // chamber clicks) only in fanFire, when the pulse is really started (never for a stroke the tempo / force aim / queue drops)
                if(emptyRaw(now)){bump(ctr.dryEmpty);pulse(hold,.20f,30'000,now,haptics::SourceRefuse);blink(now);continue;}
                Waiting w{};w.on=true;w.ns=reqs_[i].ns;w.deadline=reqs_[i].ns+frameNs_+10'000'000ull;fanTry(w,hold,now);
                if(w.on){Waiting* slot=nullptr;for(auto& x:waiting_)if(!x.on){slot=&x;break;}if(slot)*slot=w;else{bump(ctr.hitButDropped);}}
            }
        }else for(unsigned i=0;i<nReqs_;++i)if(reqs_[i].kind==StrokeHit)bump(ctr.strokesDiscarded);
        nReqs_=0;
        finish(out);saStateWord.store(sa_,std::memory_order_relaxed);H::hammerCocked.store(md==ModeSingle&&sa_==SaCocked,std::memory_order_relaxed);
    }
    // ---- markers (H3 ring above the spur, palette style 7; drawn with any HANDS / BODY / HOLSTER MARKERS value) ----
    void marker(H::MarkerSet& set,const runtime::ControllerSample& in,int hold,std::uint64_t now) noexcept {
        if(!config.ring||set.count>=set.markers.size()||hold<0||hold>1||!cacheValid_)return;
        const unsigned md=mode();if(md==ModeOff)return;
        Zone z{};if(!freshZone(z,now)||z.hand!=hold||!H::detail::poseOk(in.grip[unsigned(hold)]))return;
        if(!cacheBase_||cacheHold_!=hold)return;
        const auto rn=ringNearNs.load(std::memory_order_relaxed),nn=nearNs.load(std::memory_order_relaxed);
        const bool nearRing=rn&&now>=rn&&now-rn<=100'000'000ull,approach=nn&&now>=nn&&now-nn<=100'000'000ull;
        const bool saShow=md==ModeSingle&&sa_==SaDown&&in.triggers[unsigned(hold)]>.55f;
        const bool blinking=blinkNs_&&now>=blinkNs_&&now-blinkNs_<300'000'000ull;
        if(!nearRing&&!saShow&&!blinking)return;
        const auto& g=in.grip[unsigned(hold)];
        const V3 p=m::add(V3{g.position[0],g.position[1],g.position[2]},m::rotate(g.orientation,z.hammer));
        auto& mk=set.markers[set.count++];mk.position={p[0],p[1],p[2]};mk.radius=.012f;mk.slot=-1;mk.kind=3;
        mk.flags=1u|(7u<<8); // MarkerWeapon | palette style 7 (HandsMath.h)
        mk.highlight=blinking?(((now-blinkNs_)/62'500'000ull)%2u?0.f:1.f):approach?1.f:0.f;
        bump(ctr.ringShown);
    }
    int saState() const noexcept {return sa_;}
    bool owned() const noexcept {return owned_;}
    void resetForTests() noexcept {*this=Brain{};}
private:
    struct Elig {bool base{},ctx{},owns{};};
    Elig eligibility(int hold,std::uint64_t now,const Facts* f,bool haveFacts) noexcept {
        Elig e{};
        if(!started.load(std::memory_order_acquire)||mode()==ModeOff||!pOk())return e;
        if(hold<0||hold>1||H::holdHand.load(std::memory_order_relaxed)!=hold||!L::revolverKind(H::heldKind.load(std::memory_order_relaxed)))return e;
        if(!haveFacts||!f||!f->weapon||!(f->flags&FactHeld)||!(f->flags&FactRevolver))return e;
        Zone z{};if(!freshZone(z,now)||z.weapon!=f->weapon||z.hand!=hold)return e;
        if(!::rdrvr::vr::spin::rigid(hold))return e;
        e.base=true;
        e.ctx=(f->flags&FactForce)&&!(f->flags&FactRiding)&&(!(f->flags&FactRead)||(f->flags&FactSingle));
        e.owns=e.ctx;return e;
    }
    // readyAtNs for the XR HIT / SLIP split: when the engine accepts the next shot, in game time projected on this clock.
    std::uint64_t readyAt(const Facts& f,bool have,std::uint64_t now) const noexcept {
        if(!have||!(f.flags&FactRead)||!(f.flags&FactTime)||!(f.dtFrame>0.f))return 0;
        const double dt=double(f.dtFrame);const double k=1.0+std::floor(double(now>=f.ns?now-f.ns:0)/double(frameNs_));
        const double wait=double(f.lastFireT)+double(f.chamberUsed)-(double(f.T)+dt*k);
        if(!(wait>0.0))return 0;
        return now+std::uint64_t(std::ceil(wait/dt-1e-6))*frameNs_;
    }
    bool emptyRaw(std::uint64_t now) const noexcept {
        if(L::manualActive(now))return false;
        Facts f{};return freshFacts(f,now)&&(f.flags&FactRead)&&f.ammo<1.f;}
    Verdict verdict(int source,int hold,std::uint64_t now,H::HammerOut& out) noexcept {
        if(const auto g=H::rtGateSink.load(std::memory_order_acquire)){
            int v=1;try{v=g(source,hold,now);}catch(...){v=1;}
            H::rtGateCalls.fetch_add(1,std::memory_order_relaxed);
            if(v==0)return Verdict::Live;
            if(v>=2){out.edgeBlock=gateBlockName(v);return Verdict::Dry;}
        }
        Facts f{};if(freshFacts(f,now)&&(f.flags&FactRead)&&f.ammo<1.f)return Verdict::Empty;
        return Verdict::Live;
    }
    // FAN stroke: force aim (wait up to graceNs) -> the tempo forecast in game time (one frame of grace) -> the pulse.
    void fanTry(Waiting& w,int hold,std::uint64_t now) noexcept {
        const int bits=H::engineAimBits.load(std::memory_order_relaxed);
        const bool aimOk=H::forceAimOurs.load(std::memory_order_relaxed)||(bits>=0&&(bits&H::aimBitForce));
        if(!aimOk){
            if(now<w.deadline){if(!w.aim){w.aim=true;bump(ctr.aimWaited);}return;}
            bump(ctr.noForceAim);bump(ctr.rejected[RejectNoForceAim]);w.on=false;return;}
        Facts f{};const bool have=freshFacts(f,now);
        if(have&&(f.flags&FactRead)&&(f.flags&FactTime)&&f.dtFrame>0.f){
            const double dt=double(f.dtFrame);const double k=1.0+std::floor(double(now>=f.ns?now-f.ns:0)/double(frameNs_));
            const double tNext=double(f.T)+dt*k,need=double(f.lastFireT)+double(f.chamberUsed)-1e-4;
            if(tNext>=need){w.on=false;fanFire(hold,now);return;}
            if(!w.graced&&tNext+dt>=need){w.graced=true;bump(ctr.graced);return;} // ready next frame: hold it one frame
            w.on=false;bump(ctr.hitButDropped);return;
        }
        // no engine facts: only an interval of >= 2 frames (steady clock)
        if(!lastFanPulseNs_||now-lastFanPulseNs_>=2*frameNs_-2'000'000ull){w.on=false;fanFire(hold,now);return;}
        if(!w.graced){w.graced=true;bump(ctr.graced);return;}
        w.on=false;bump(ctr.hitButDropped);
    }
    // A FAN stroke the tempo / force aim accepted: room in the pulse queue -> ONE gate call -> the
    // pulse, or RELOAD's dry click / the empty rule's dry haptic
    void fanFire(int hold,std::uint64_t now) noexcept {
        if(!pulseRoom()){bump(ctr.merged);return;}
        H::HammerOut dummy{};const auto v=verdict(gateFan,hold,now,dummy);
        if(v==Verdict::Dry){bump(ctr.dryReload);return;}           // RELOAD clicked + buzzed (one gate call per sent stroke)
        if(v==Verdict::Empty){bump(ctr.dryEmpty);pulse(hold,.20f,30'000,now,haptics::SourceRefuse);blink(now);return;}
        startPulse(KindFan,now);
    }
    bool pulseRoom() const noexcept {return queued_<queue_.size();}
    void startPulse(std::uint8_t kind,std::uint64_t now) noexcept {
        if(!pressLeft_&&!gapLeft_&&!queued_){begin(kind,now,false,now);return;}
        if(queued_<queue_.size()){queueNs_[queued_]=now;queue_[queued_++]=kind;return;}
        bump(ctr.merged); // two pulses before a shot merge into one shot anyway (one request bit)
        if(kind!=KindFan)noteMergedEdge(now);
    }
    // edgeNs: the physical edge that asked for this pulse (R1 / SA: route()'s now; a queued pulse begins later), 0 for FAN / a retry
    void begin(std::uint8_t kind,std::uint64_t now,bool retried=false,std::uint64_t edgeNs=0) noexcept {
        pressLeft_=std::max(1u,config.pulseFrames);
        Facts f{};freshFacts(f,now);
        {std::lock_guard lock(flightMutex);Flight* slot=nullptr;for(auto& x:flights)if(!x.on){slot=&x;break;}
            if(!slot){slot=&flights[0];for(auto& x:flights)if(x.ns<slot->ns)slot=&x;}
            *slot=Flight{};slot->on=true;slot->kind=kind;slot->ns=now;slot->edgeNs=kind==KindFan?0:edgeNs;slot->generation=lastGen_;slot->weapon=std::uintptr_t(f.weapon);
            slot->spread=kind!=KindSa&&spreadTagged(now);slot->retried=retried;}
        if(kind==KindFan){bump(ctr.pulses);const auto until=streakUntilNs.load(std::memory_order_relaxed);if(now>=until)bump(ctr.streaks);
            streakUntilNs.store(now+config.streakNs,std::memory_order_relaxed);lastFanPulseNs_=now;}
    }
    void finish(H::HammerOut& out) noexcept {
        if(!pressLeft_&&!gapLeft_&&queued_){const auto k=queue_[0];const auto e=queueNs_[0];queue_[0]=queue_[1];queueNs_[0]=queueNs_[1];--queued_;begin(k,now_,false,e);}
        out.rt=pressLeft_?1.f:0.f;out.rtBit=pressLeft_>0;
        if(pressLeft_){if(!--pressLeft_)gapLeft_=1;}else if(gapLeft_)--gapLeft_;
    }
    void cancelPulses() noexcept {pressLeft_=0;gapLeft_=0;queued_=0;}
    void dropRequests(bool closeGrace) noexcept {for(unsigned i=0;i<nReqs_;++i)bump(closeGrace?ctr.dryCylinder:ctr.dryEmpty);nReqs_=0;}
    void blink(std::uint64_t now) noexcept {blinkNs_=now;bump(ctr.ringBlinks);}
    // in-flight expiry (frameBegin): R facts + the aniAim vtable = while the request bit stays (cap 500 ms); else a fixed 300 ms. No retries.
    void expireFlights(const Facts& f,bool have,std::uint64_t now) noexcept {
        std::lock_guard lock(flightMutex);
        for(auto& x:flights){if(!x.on)continue;
            if(have&&f.ns>x.ns&&(f.flags&FactAim)&&(f.aimBits&elf::bitRequest))x.bit3Seen=true;
            const auto age=now>=x.ns?now-x.ns:0;const bool rWin=have&&(f.flags&FactRead)&&(f.flags&FactAim);
            const bool expire=rWin?(age>=500'000'000ull||(age>=150'000'000ull&&f.ns>=x.ns+150'000'000ull&&!(f.aimBits&elf::bitRequest))):age>=300'000'000ull;
            if(!expire)continue;
            x.on=false;
            // open across a claimed fire of its weapon = merged into that shot (one request bit), not lost; never retried, no snapshot
            if(x.acrossFire&&x.kind!=KindSa){if(x.kind==KindFan){bump(ctr.merged);bump(ctr.mergedExpired);}else{bump(ctr.triggerMerged);noteMergedEdge(x.edgeNs);}continue;}
            if(x.kind==KindFan){bump(ctr.lost);if(config.retry&&rOk()&&!x.bit3Seen&&!x.retried)retryPending_=true;} // a retry is never retried
            else if(x.kind==KindSa){bump(ctr.saRefused);saRefusedSeq.fetch_add(1,std::memory_order_release);}
            else bump(ctr.lostTrigger);
            Lost l{};l.ns=now;l.kind=x.kind;l.bit3Seen=x.bit3Seen;l.valid=have;
            if(have){l.bits=f.aimBits;l.state=f.aimState;l.attack240=f.attack240;l.reload248=f.reload248;l.phase=f.aimPhase;l.window=f.aimWindow;
                l.sinceFireGameS=f.T-f.lastFireT;l.chamber=f.chamberUsed;l.ammo=f.ammo;l.weapon120=f.weaponFlags;}
            lostLast=l;
        }
    }
    std::array<Stroke,4> reqs_{};unsigned nReqs_{};
    std::array<Waiting,2> waiting_{};
    std::uint64_t now_{},lastFrameNs_{},frameNs_{27'777'777ull},blinkNs_{},lastFanPulseNs_{},cacheNs_{};
    int sa_{SaDown},lastHold_{-2},cacheHold_{-3};std::uint32_t lastGen_{},saFiredSeen_{},saRefusedSeen_{};
    unsigned pressLeft_{},gapLeft_{},queued_{};std::array<std::uint8_t,2> queue_{};std::array<std::uint64_t,2> queueNs_{};
    bool rawPass_{},owned_{},offEmpty_{},torsoOk_{true},thumbWas_{},retryPending_{},cacheOwns_{},cacheBase_{},cacheValid_{};
};
inline Brain brain{};

// ---- holsters sinks (static table; registered by startup) ----
inline void frameBeginSink(const H::HammerFrame& f) noexcept {try{brain.frameBegin(f);}catch(...){}}
inline bool ownsRtSink(int hold,std::uint64_t now) noexcept {try{return brain.ownsRt(hold,now);}catch(...){return false;}}
inline void stepSink(const runtime::ControllerSample& in,const H::HammerIn& hi,H::HammerOut& out) noexcept {try{brain.step(in,hi,out);}catch(...){out=H::HammerOut{};}}
inline void markerSink(H::MarkerSet& set,const runtime::ControllerSample& in,int hold,std::uint64_t now) noexcept {try{brain.marker(set,in,hold,now);}catch(...){}}
inline void xrSink(const runtime::ControllerSample& s,bool graphicsInput) noexcept {xrSample(s,graphicsInput);}
inline constexpr H::HammerSinks sinks{&frameBeginSink,&ownsRtSink,&stepSink,&markerSink,&xrSink};

// =====================================================================================================================
// Menu row HAMMER (Feature::Hammer, input thread every frame from the GameXr settings glue): PROP (killed), UNAVAILABLE (not started / P
// mismatch / not configured), READY + note NO HAMMER / FALLBACK / NEEDS AIM / COCKED / DOWN / SWIPE OVER HAMMER. FAN SPREAD: no note.
inline void publishStatus(std::uint64_t at=0) noexcept {
    namespace S=status;
    if(!config.enabled||killed(Feature::Hammer)){S::publish(Feature::Hammer,S::Property);return;}
    if(!started.load(std::memory_order_acquire)||!configured.load(std::memory_order_acquire)||!pOk()){S::publish(Feature::Hammer,S::Unavailable);return;}
    const unsigned md=mode();const char* note=nullptr;const auto now=at?at:nowNs();
    const int hold=H::holdHand.load(std::memory_order_relaxed);
    if(md!=ModeOff&&hold>=0&&L::revolverKind(H::heldKind.load(std::memory_order_relaxed))){
        Zone z{};const bool zone=freshZone(z,now);
        if(!L::modelMechanics(L::ownerWeapon.load(std::memory_order_relaxed)))note="TRIGGER ONLY"; // No trusted hammer geometry
        else if(zoneState.load(std::memory_order_relaxed)==ZoneNoBone)note="NO HAMMER";
        else if(md==ModeSingle)note=brain.saState()==SaCocked?"COCKED":"COCK THEN TRIGGER"; // REALISTIC hint (34 value columns; the full hint is the row footer)
        else if((md==ModeFan||md==ModeFanTrigger)&&H::ForceAim(H::forceAimMode.load(std::memory_order_relaxed))==H::ForceAim::Off)note="NEEDS AIM";
        else if(style()==StyleRealistic&&heldAction.load(std::memory_order_relaxed)==ActionDouble)note="DOUBLE ACTION";
        else if(!strokeSeenSession.load(std::memory_order_relaxed))note=dragVisual()?"DRAG HAMMER BACK":"SWIPE OVER HAMMER";
        else if(zone&&z.source==SourceFallback)note="FALLBACK";
    }
    S::publish(Feature::Hammer,S::Ready,note);
}

// =====================================================================================================================
// Startup (VrWeapon startup; any thread, once). Properties debug.rdrvr.vr.hammer.*.
inline Config readConfig() noexcept {
    const auto prop=[](const char* suffix){return H::property((std::string("debug.rdrvr.vr.hammer")+suffix).c_str());};
    const auto off=[](const std::string& v){return !v.empty()&&v[0]=='0';};
    const auto on=[](const std::string& v){return !v.empty()&&v[0]=='1';};
    const auto num=[&](const char* s,float lo,float hi,float& out){const auto t=prop(s);if(t.empty())return;char* e{};const float v=std::strtof(t.c_str(),&e);
        if(e!=t.c_str()&&std::isfinite(v)&&v>=lo&&v<=hi)out=v;};
    const auto ms=[&](const char* s,float lo,float hi,std::uint64_t& out){float v=-1.f;num(s,lo,hi,v);if(v>=0.f)out=std::uint64_t(double(v)*1e6);};
    Config c{};
    c.enabled=!off(prop(""))&&!killed(Feature::Hammer);
    {float v=-1.f;num(".chamber",0.f,1000.f,v);if(v>=0.f)c.chamberS=v/1000.f;}
    ms(".streak",100.f,5000.f,c.streakNs);num(".vmin",.2f,6.f,c.vmin);num(".cockvmin",.05f,3.f,c.cockVmin);num(".vabs",0.f,4.f,c.vabs);
    {float v[3]{c.halfW*100.f,c.yMin*100.f,c.yMax*100.f};int n=0;
        if(L::parseFloats(prop(".zone"),v,3,n)&&n==3&&v[0]>=1.f&&v[0]<=20.f&&v[1]>=-10.f&&v[2]>v[1]&&v[2]<=40.f){c.halfW=v[0]*.01f;c.yMin=v[1]*.01f;c.yMax=v[2]*.01f;}}
    {float v[2]{c.elevDeg,c.fwdM*100.f};int n=0;
        if(L::parseFloats(prop(".present"),v,2,n)&&n==2&&v[0]>=-90.f&&v[0]<=45.f&&v[1]>=-40.f&&v[1]<=60.f){c.elevDeg=v[0];c.fwdM=v[1]*.01f;}}
    ms(".parked",0.f,5000.f,c.parkedNs);
    {float v[3]{};int n=0;if(L::parseFloats(prop(".offset"),v,3,n)&&n==3&&std::fabs(v[0])<=10.f&&std::fabs(v[1])<=10.f&&std::fabs(v[2])<=10.f)c.offset={v[0]*.01f,v[1]*.01f,v[2]*.01f};}
    {float v[2]{};int n=0;if(L::parseFloats(prop(".fallback"),v,2,n)&&n==2&&std::fabs(v[0])<=10.f&&std::fabs(v[1])<=15.f){c.fallbackUp=v[0]*.01f;c.fallbackBack=v[1]*.01f;}}
    {float v[3]{};int n=0;if(L::parseFloats(prop(".palm"),v,3,n)&&n==3&&std::fabs(v[0])<=15.f&&std::fabs(v[1])<=15.f&&std::fabs(v[2])<=15.f)c.palm={v[0]*.01f,v[1]*.01f,v[2]*.01f};}
    num(".grip",.1f,1.f,c.grip);
    {float v[4]{c.capR*100.f,c.capFront*100.f,c.capBack*100.f,c.capUp*100.f};int n=0;
        if(L::parseFloats(prop(".capsule"),v,4,n)&&n==4&&v[0]>=2.f&&v[0]<=20.f&&v[1]>=-10.f&&v[1]<=20.f&&v[2]>=-10.f&&v[2]<=25.f&&std::fabs(v[3])<=15.f){
            c.capR=v[0]*.01f;c.capFront=v[1]*.01f;c.capBack=v[2]*.01f;c.capUp=v[3]*.01f;}}
    num(".dominance",0.f,1.f,c.dominance);ms(".refractory",0.f,1000.f,c.refractoryNs);
    c.retry=on(prop(".retry"))?1:0;{float v=-1.f;num(".pulse",1.f,4.f,v);if(v>=1.f)c.pulseFrames=unsigned(v);}
    c.drawCocked=on(prop(".drawcocked"));c.thumbCock=on(prop(".thumbcock"));
    {const auto v=prop(".visual");if(off(v))c.visual=0;else if(v.rfind("force",0)==0){float d=0.f;int n=0;
        if(L::parseFloats(v.substr(5),&d,1,n)&&n==1&&std::fabs(d)<=80.f){c.visual=2;c.visualForceDeg=d;}}}
    c.ring=prop(".ring")=="1";
    {const auto b=prop(".bone");if(!b.empty()){char* end=nullptr;const long v=std::strtol(b.c_str(),&end,10);
        if(end&&*end==0&&v>=0&&v<long(L::maxBones))c.boneIndex=int(v);else for(std::size_t i=0;i<b.size()&&i<31;++i)c.boneName[i]=b[i];}}
    ms(".transferblock",0.f,2000.f,c.transferBlockNs);
    {const auto t=prop(".drag");if(off(t))c.dragTravel=0.f;else{float v[3]{c.dragTravel*100.f,c.dragDeg,c.dragSign};int n=0;
        if(L::parseFloats(t,v,3,n)&&n>=1&&v[0]>=1.f&&v[0]<=12.f){c.dragTravel=v[0]*.01f;if(n>=2&&v[1]>=0.f&&v[1]<=80.f)c.dragDeg=v[1];if(n>=3&&v[2]!=0.f)c.dragSign=v[2]<0.f?-1.f:1.f;}}}
    {float v[4]{c.dragReturnS*1000.f,c.dragRearm*100.f,c.dragAbsShare,c.dragTickM*1000.f};int n=0;
        if(L::parseFloats(prop(".dragmisc"),v,4,n)&&n==4&&v[0]>=5.f&&v[0]<=1000.f&&v[1]>=0.f&&v[1]<=10.f&&v[2]>=0.f&&v[2]<=1.f&&v[3]>=1.f&&v[3]<=40.f){
            c.dragReturnS=v[0]*.001f;c.dragRearm=v[1]*.01f;c.dragAbsShare=v[2];c.dragTickM=v[3]*.001f;}}
    return c;
}
inline void startup() noexcept {
    if(started.load())return;
    config=readConfig();detector.cfg=detCfgFrom(config);
    if(config.enabled){
        H::hammerTransferBlockNs.store(config.transferBlockNs,std::memory_order_relaxed);
        started.store(true,std::memory_order_release);
        H::hammerSinks.store(&sinks,std::memory_order_release);
    }
}
// Host tests: back to a clean module (keeps the startup config, the sinks stay as they were).
inline void resetForTests() noexcept {
    brain.resetForTests();detector=Detector{};detector.cfg=detCfgFrom(config);
    {Stroke k{};while(strokeQueue.pop(k)){}}
    zoneBox.store(Zone{});armBox.store(Arm{});factsBox.store(Facts{});
    {std::lock_guard lock(flightMutex);flights={};lostLast=Lost{};}
    for(auto& e:mergedEdges)e.store(0);mergedEdgeNext.store(0);
    {std::lock_guard lock(poseMutex);probe::text.clear();probe::types={-1,-1,-1,-1,-1,-1};poseRecord=PoseRecord{};pendingRestores={};hammerInsts={};hammerTypes={};visualTypes={};trace={};traceN=0;traceFireNs=0;bonesByType={};}
    {std::lock_guard lock(nearMissMutex);nearMiss={};nearMissNext=0;}
    {std::lock_guard lock(contactMutex);contacts={};contactNext=0;}attemptState=AttemptState{};
    ptrs=Ptrs{};oursWeapon=0;oursValue=0.f;oursGuid=0;prevT=0.f;dtFrame=1.f/36.f;barrelPrev={};barrelPrevNs=0;
    nearNs.store(0);ringNearNs.store(0);approachNs.store(0);approachK.store(0.f);lastStrokeXrNs.store(0);streakUntilNs.store(0);saSeries.store(false);
    lastFireNs.store(0);lastClaimNs.store(0);saFiredSeq.store(0);saRefusedSeq.store(0);chamberActive.store(false);spreadLastNs.store(0);
    strokeSeenSession.store(false);saStateWord.store(0);dragKWord.store(0.f);dragNs.store(0);cockWaitNs.store(0);cockShowNs.store(0);ctr.changedWithoutClip.store(0); // the session gate of H2
    zoneBone.store(-1);zoneSourceSeen.store(0);zoneState.store(ZoneUnknown);heldAction.store(ActionUnknown);
    H::hammerHoldGuardUntilNs.store(0);H::hammerAimNs.store(0);H::hammerCocked.store(false);for(auto& c:H::hammerContactNs)c.store(0);
}

// =====================================================================================================================
// Report "vr":{"weapon":{"hammer":<this>}} (any thread).
inline std::string report() {
    std::string out;out.reserve(8192);char b[2048];const auto now=nowNs();
    const auto u=[](const std::atomic<std::uint64_t>& a){return (unsigned long long)a.load(std::memory_order_relaxed);};
    const auto v3=[](const V3& v,float s){char t[96];std::snprintf(t,sizeof t,"[%.2f,%.2f,%.2f]",double(v[0]*s),double(v[1]*s),double(v[2]*s));return std::string(t);};
    const unsigned md=mode();Facts f{};const bool facts=factsBox.load(f)&&f.ns;Zone z{};const bool zone=zoneBox.load(z)&&z.ns;
    const auto aimMode=H::ForceAim(H::forceAimMode.load());
    std::snprintf(b,sizeof b,"{\"style\":\"%s\",\"action\":\"%s\",\"handName\":\"%s\",",styleName(style()),actionName(heldAction.load()),H::handName.load().c_str());out+=b;
    std::snprintf(b,sizeof b,"\"enabled\":%s,\"killed\":%s,\"started\":%s,\"mode\":\"%s\",\"spreadMenu\":%.1f,\"engine\":\"%s\",\"engineClasses\":{\"P\":\"%s\",\"R\":\"%s\",\"O\":\"%s\","
        "\"V\":\"%s\",\"got\":\"%s\",\"aimVtable\":\"%s\",\"printsFailed\":%u,\"firstFailed\":\"%s\"},\"owned\":%s,\"eligible\":\"%s\",",
        config.enabled?"true":"false",killed(Feature::Hammer)?"true":"false",started.load()?"true":"false",modeName(md),double(sigmaDeg()),engineText(),
        classText(classOk(elf::ClassP)),classText(classOk(elf::ClassR)),classText(classOk(elf::ClassO)),classText(vOk()),
        classText(gotOk.load()),classText(vtableOk.load()),printsFailed.load(),firstFailedPrint.load(),brain.owned()?"true":"false",eligibleWhy.load());out+=b;
    { // weapon facts
        const auto bits=f.aimBits;char bstr[8]{};bstr[0]=(bits&elf::bitAim22)?'1':'0';bstr[1]=(bits&elf::bitAim23)?'1':'0';
        std::array<char,32> bn{};{std::lock_guard lock(poseMutex);bn=zoneBoneName;}for(char& c:bn)if(c=='"'||c=='\\'||(c&&static_cast<unsigned char>(c)<0x20))c='?';
        std::snprintf(b,sizeof b,"\"weapon\":{\"typeId\":%d,\"ageMs\":%.0f,\"bone\":%d,\"boneName\":\"%s\",\"boneModelCm\":%s,\"zone\":{\"valid\":%s,\"source\":\"%s\","
            "\"hand\":%d,\"ageMs\":%.0f,\"hammerGripCm\":%s,\"fwdGrip\":%s,\"upGrip\":%s,\"published\":%llu,\"refreshes\":%llu,\"jumps\":%llu,\"rawJumps\":%llu,\"movingSkips\":%llu,\"outliers\":%llu,\"implausible\":%llu,\"ringResets\":%llu,\"failures\":%llu,\"lastFailure\":\"%s\"},"
            "\"rest\":{\"learned\":%llu,\"fromType\":%llu,\"rejected\":%llu},"
            "\"trigHold\":\"%s\",\"chamberS\":%.3f,\"chamberFramesAt36\":%.1f,\"nativeChamberS\":%.3f,\"autoFireDelayS\":%.3f,\"ammo\":%.0f,\"ammoPerClip\":%.0f,"
            "\"overrideS\":%.3f,\"lastFireAgeGameS\":%.3f,\"dtFrameMs\":%.1f,\"forceAim\":\"%s\",\"flags\":%u,"
            "\"aim\":{\"vtableOk\":%s,\"firePending\":%s,\"bits22_23\":\"%s\",\"rapidBit34\":%s,\"phase\":%.2f,\"rate\":%.2f,\"eventFrame\":%d,\"fireWindowS\":%.3f,"
            "\"state\":%d},\"player\":{\"attack240\":%d,\"reload248\":%d}},",
            facts?int(f.typeId):-1,facts&&now>=f.ns?double(now-f.ns)/1e6:-1.0,zoneBone.load(),bn.data(),v3(z.boneModel,1.f).c_str(),zone&&z.source?"true":"false",
            z.source==SourceBone?"BONE":z.source==SourceFallback?"FALLBACK":"NONE",z.hand,zone&&now>=z.ns?double(now-z.ns)/1e6:-1.0,v3(z.hammer,100.f).c_str(),
            v3(z.fwd,1.f).c_str(),v3(z.up,1.f).c_str(),u(ctr.zonePublished),u(ctr.zoneRefreshes),u(ctr.zoneJumps),u(ctr.zoneRawJumps),u(ctr.zoneMovingSkips),u(ctr.zoneOutliers),u(ctr.zoneImplausible),
            u(ctr.zoneRingResets),u(ctr.zoneFailures),lastZoneFailure.load(),u(ctr.restLearned),u(ctr.restFromType),u(ctr.restRejected),
            !(f.flags&FactRead)?"UNKNOWN":f.trigHold==0?"Single":f.trigHold==1?"Auto":f.trigHold==2?"Charge":"?",double(f.chamberUsed),double(f.chamberUsed)*36.0,
            double(f.nativeChamber),double(f.autoFireDelay),double(f.ammo),double(f.perClip),double(f.overrideS),double(f.T-f.lastFireT),double(f.dtFrame)*1000.0,
            aimMode==H::ForceAim::Held?"HELD":aimMode==H::ForceAim::Trigger?"TRIGGER":"OFF",f.flags,(f.flags&FactAim)?"true":"false",
            (f.aimBits&elf::bitRequest)?"true":"false",bstr,(f.aimBits&elf::bitRapid34)?"true":"false",double(f.aimPhase),double(f.aimRate),f.aimEvent,
            double(f.aimWindow),f.aimState,f.attack240,f.reload248);out+=b;
        out+="\"bonesByType\":[";{std::lock_guard lock(poseMutex);bool first=true;
            for(const auto& t:bonesByType){if(t.typeId<0)continue;std::string n(t.names);for(char& c:n)if(c=='"'||c=='\\'||static_cast<unsigned char>(c)<0x20)c='?';
                std::snprintf(b,sizeof b,"%s{\"typeId\":%d,\"names\":\"%s\"}",first?"":",",t.typeId,n.c_str());out+=b;first=false;}}
        out+="],";
    }
    std::snprintf(b,sizeof b,"\"arm\":{\"published\":%llu,\"suspended\":%llu,\"stale\":%llu,\"ownershipFlips\":%llu,\"rearmLatched\":%llu,\"frames\":%llu,\"steps\":%llu,"
        "\"blockedSteps\":%llu,\"triggerEdges\":%llu},",u(ctr.armPublished),u(ctr.armSuspended),u(ctr.armStaleSeen),u(ctr.ownershipFlips),
        (unsigned long long)H::hammerRearmLatched.load(),u(ctr.frames),u(ctr.stepCalls),u(ctr.blockedSteps),u(ctr.triggerEdgesOwned));out+=b;
    const auto xs=ctr.xrSamples.load();
    std::snprintf(b,sizeof b,"\"detector\":{\"xrSamples\":%llu,\"menuSkips\":%llu,\"crossings\":{\"near\":%llu,\"far\":%llu},\"strokes\":{\"fan\":%llu,\"cock\":%llu,\"slip\":%llu},"
        "\"queueFull\":%llu,\"config\":{\"zoneCm\":[%.1f,%.1f,%.1f],\"vmin\":%.2f,\"cockVmin\":%.2f,\"vabs\":%.2f,\"present\":[%.0f,%.0f],\"parkedMs\":%.0f,\"palmCm\":%s,\"grip\":%.2f,\"capsuleCm\":[%.1f,%.1f,%.1f,%.1f],\"dominance\":%.2f,\"refractoryMs\":%.0f},\"rejected\":{",
        (unsigned long long)xs,u(ctr.menuSkips),u(ctr.crossNear),u(ctr.crossFar),u(ctr.strokesFan),u(ctr.strokesCock),u(ctr.strokesSlip),u(ctr.queueFull),
        double(config.halfW*100.f),double(config.yMin*100.f),double(config.yMax*100.f),double(config.vmin),double(config.cockVmin),double(config.vabs),
        double(config.elevDeg),double(config.fwdM*100.f),double(config.parkedNs)/1e6,v3(config.palm,100.f).c_str(),double(config.grip),
        double(config.capR*100.f),double(config.capFront*100.f),double(config.capBack*100.f),double(config.capUp*100.f),double(config.dominance),double(config.refractoryNs)/1e6);out+=b;
    for(unsigned i=1;i<RejectCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i>1?",":"",rejectNames[i],u(ctr.rejected[i]));out+=b;}
    out+="},\"yHist\":[";for(unsigned i=0;i<ctr.yHist.size();++i){std::snprintf(b,sizeof b,"%s%llu",i?",":"",u(ctr.yHist[i]));out+=b;}
    out+="],\"nearMiss\":[";
    {std::lock_guard lock(nearMissMutex);bool first=true;
        for(unsigned c=0;c<nearMiss.size();++c){const auto& n=nearMiss[(nearMissNext+c)%nearMiss.size()];if(!n.ns)continue;
            std::snprintf(b,sizeof b,"%s{\"ageMs\":%.0f,\"reason\":\"%s\",\"samples\":[",first?"":",",now>=n.ns?double(now-n.ns)/1e6:-1.0,rejectNames[std::min<unsigned>(n.reason,RejectCount-1)]);out+=b;first=false;
            for(unsigned i=0;i<n.n;++i){const auto& sm=n.s[i];
                std::snprintf(b,sizeof b,"%s{\"q\":[%.1f,%.1f,%.1f],\"vRel\":%.2f,\"vAbs\":%.2f,\"grip\":%.2f}",i?",":"",double(sm.q[0]*100.f),double(sm.q[1]*100.f),
                    double(sm.q[2]*100.f),double(sm.vRel),double(sm.vAbs),double(sm.grip));out+=b;}
            out+="]}";}}
    std::snprintf(b,sizeof b,"]},\"strokes\":{\"queued\":%llu,\"consumed\":%llu,\"stale\":%llu,\"discarded\":%llu},",
        u(ctr.strokesQueued),u(ctr.strokesConsumed),u(ctr.strokesStale),u(ctr.strokesDiscarded));out+=b;
    const auto ln=ctr.latencyN.load(),imin=ctr.intervalNsMin.load();
    std::snprintf(b,sizeof b,"\"fan\":{\"pulses\":%llu,\"fired\":%llu,\"lost\":%llu,\"tooFast\":%llu,\"graced\":%llu,\"hitButDropped\":%llu,\"retries\":%llu,\"merged\":%llu,"
        "\"aimWaited\":%llu,\"noForceAim\":%llu,\"dry\":{\"empty\":%llu,\"cylinder\":%llu,\"context\":%llu,\"notRearmed\":%llu,\"reloadDry\":%llu},\"emptyTriggerPassed\":%llu,"
        "\"latencyMsAvg\":%.1f,\"latencyMsMax\":%.1f,\"intervalMsMin\":%.1f,\"rateHzPeak\":%.2f,\"streaks\":%llu,\"streakActive\":%s,\"transferSuppressed\":%llu,"
        "\"releaseDeferred\":%llu,\"triggerPulses\":%llu,\"triggerFired\":%llu,\"triggerLost\":%llu,\"mergedExpired\":%llu,\"triggerMerged\":%llu,\"claimsStale\":%llu,"
        "\"edgesMerged\":%llu},",
        u(ctr.pulses),u(ctr.fired),u(ctr.lost),u(ctr.tooFast),u(ctr.graced),u(ctr.hitButDropped),u(ctr.retries),u(ctr.merged),u(ctr.aimWaited),u(ctr.noForceAim),
        u(ctr.dryEmpty),u(ctr.dryCylinder),u(ctr.dryContext),u(ctr.dryNotRearmed),u(ctr.dryReload),u(ctr.emptyTriggerPassed),
        ln?double(ctr.latencyNsSum.load())/double(ln)/1e6:0.0,double(ctr.latencyNsMax.load())/1e6,double(imin)/1e6,imin?1e9/double(imin):0.0,u(ctr.streaks),
        now<streakUntilNs.load()?"true":"false",(unsigned long long)H::hammerTransferSuppressed.load(),(unsigned long long)H::hammerReleaseDeferred.load(),
        u(ctr.triggerPulses),u(ctr.triggerFired),u(ctr.lostTrigger),u(ctr.mergedExpired),u(ctr.triggerMerged),u(ctr.claimsStale),u(ctr.edgesMerged));out+=b;
    {Lost l{};{std::lock_guard lock(flightMutex);l=lostLast;}
        char bits[6]{};bits[0]=(l.bits&elf::bitRequest)?'1':'0';bits[1]=(l.bits&elf::bitAim22)?'1':'0';bits[2]=(l.bits&elf::bitAim23)?'1':'0';
        bits[3]=(l.bits&elf::bitRapid33)?'1':'0';bits[4]=(l.bits&elf::bitRapid34)?'1':'0';
        std::snprintf(b,sizeof b,"\"lost\":{\"last\":{\"ageMs\":%.0f,\"kind\":\"%s\",\"factsValid\":%s,\"bits3_22_23_33_34\":\"%s\",\"state\":%d,\"phase\":%.2f,\"window\":%.3f,"
            "\"attack240\":%d,\"reload248\":%d,\"weapon120\":%u,\"sinceFireGameS\":%.3f,\"chamber\":%.3f,\"ammo\":%.0f,\"bit3Seen\":%s}},",
            l.ns&&now>=l.ns?double(now-l.ns)/1e6:-1.0,kindName(l.kind),l.valid?"true":"false",bits,l.state,double(l.phase),double(l.window),l.attack240,l.reload248,
            l.weapon120,double(l.sinceFireGameS),double(l.chamber),double(l.ammo),l.bit3Seen?"true":"false");out+=b;}
    std::snprintf(b,sizeof b,"\"sa\":{\"state\":\"%s\",\"cocks\":%llu,\"thumbCocks\":%llu,\"shots\":%llu,\"downPresses\":%llu,\"refused\":%llu,\"dry\":%llu,\"autoCockDual\":%llu,"
        "\"drawCocked\":%s,\"thumbCock\":%s,\"dryTriggers\":%llu,\"fanShots\":%llu,\"cockedShots\":%llu,\"fanDry\":%llu},",saName(saStateWord.load()),u(ctr.saCocks),u(ctr.saThumbCocks),
        u(ctr.saShots),u(ctr.saDownPresses),u(ctr.saRefused),u(ctr.saDry),u(ctr.saAutoCockDual),config.drawCocked?"true":"false",config.thumbCock?"true":"false",
        u(ctr.dryTriggers),u(ctr.fanShots),u(ctr.cockedShots),u(ctr.fanDry));out+=b;
    std::snprintf(b,sizeof b,"\"drag\":{\"enabled\":%s,\"travelCm\":%.1f,\"maxDeg\":%.0f,\"sign\":%.0f,\"returnMs\":%.0f,\"rearmCm\":%.1f,\"absShare\":%.2f,\"tickMm\":%.1f,"
        "\"starts\":%llu,\"cocks\":%llu,\"strokes\":%llu,\"waits\":%llu,\"waitFired\":%llu,\"rejected\":%llu,\"returns\":%llu,\"ticks\":%llu,\"maxK\":%.3f,\"k\":%.3f,"
        "\"kAgeMs\":%.0f,\"visualFrames\":%llu,\"fallbackAxisFrames\":%llu},",dragVisual()?"true":"false",double(config.dragTravel*100.f),double(config.dragDeg),
        double(config.dragSign),double(config.dragReturnS*1000.f),double(config.dragRearm*100.f),double(config.dragAbsShare),double(config.dragTickM*1000.f),
        u(ctr.dragStarts),u(ctr.dragCocks),u(ctr.dragStrokes),u(ctr.dragWaits),u(ctr.dragWaitFired),u(ctr.dragRejected),u(ctr.dragReturns),u(ctr.dragTicks),
        double(ctr.dragMaxPermille.load())/1000.0,double(dragKWord.load()),dragNs.load()&&now>=dragNs.load()?double(now-dragNs.load())/1e6:-1.0,
        u(ctr.visualDragFrames),u(ctr.visualFallbackAxis));out+=b;
    { // Where the free hand touched (hammer frame cm: x right, y up, z barrel; probe 0 grip origin, 1 palm, 2 front ring, 3 top)
        V3 axP{};{std::lock_guard lk(poseMutex);axP=visualAxisParent;}
        std::snprintf(b,sizeof b,"\"dragAxis\":{\"col\":%d,\"flipped\":%s,\"parent\":%s},\"contacts\":[",visualAxisCol.load(),visualAxisFlipped.load()?"true":"false",
            v3(axP,1.f).c_str());out+=b;
        std::lock_guard lock(contactMutex);bool first=true;
        // every approach (hammer frame cm: x right, y up, z barrel): why, firstCm / closestCm (minDistCm from the capsule axis, no floor) /
        // dragCm (where the drag started, probe) / endCm, the max free grip, the Arm flags (1 weapon 2 needTrigger 4 torso 8 context 16 saDown,
        // 128 hand lost), travel / maxK, cocked (drag / swipe / no); durMs -1 = still open
        for(unsigned c=0;c<contacts.size();++c){const auto& n=contacts[(contactNext+c)%contacts.size()];if(!n.ns)continue;
            std::snprintf(b,sizeof b,"%s{\"ageMs\":%.0f,\"durMs\":%.0f,\"why\":\"%s\",\"firstCm\":%s,\"closestCm\":%s,\"minDistCm\":%.1f,\"dragAfterMs\":%.0f,"
                "\"dragCm\":%s,\"probe\":%u,\"startDistCm\":%.1f,\"endCm\":%s,\"grip\":%.2f,\"armFlags\":%u,\"travelCm\":%.1f,\"maxK\":%.2f,\"cocked\":\"%s\"}",
                first?"":",",now>=n.ns?double(now-n.ns)/1e6:-1.0,n.endNs>=n.ns?double(n.endNs-n.ns)/1e6:-1.0,attemptNames[std::min<unsigned>(n.why,AttCount-1)],
                v3(n.q,100.f).c_str(),v3(n.qMin,100.f).c_str(),n.minDist<1e8f?double(n.minDist*100.f):-1.0,n.dragNs>=n.ns&&n.dragNs?double(n.dragNs-n.ns)/1e6:-1.0,
                v3(n.q0,100.f).c_str(),unsigned(n.probe),double(n.dist*100.f),v3(n.qEnd,100.f).c_str(),double(n.grip),unsigned(n.armFlags),double(n.travel*100.f),
                double(n.maxK),n.cocked==1?"DRAG":n.cocked==2?"SWIPE":"NO");out+=b;first=false;}
        out+="],\"attempts\":{";
        for(unsigned i=1;i<AttCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i>1?",":"",attemptNames[i],u(ctr.attempts[i]));out+=b;}
        out+="},";}
    std::snprintf(b,sizeof b,"\"chamber\":{\"configS\":%.3f,\"writes\":%llu,\"restores\":%llu,\"restoreReason\":\"%s\",\"foreign\":%llu,\"invalidPtr\":%llu,\"skippedNative\":%llu,"
        "\"active\":%s},",double(config.chamberS),u(ctr.chamberWrites),u(ctr.chamberRestores),lastRestoreReason.load(),u(ctr.chamberForeign),u(ctr.chamberInvalid),
        u(ctr.chamberSkippedNative),chamberActive.load()?"true":"false");out+=b;
    const auto nn=ctr.naturalN.load();
    std::snprintf(b,sizeof b,"\"spread\":{\"sigmaDeg\":%.1f,\"frames\":%llu,\"shots\":%llu,\"tagged\":%llu,\"lastDeg\":%.2f,\"hist\":[%llu,%llu,%llu,%llu],"
        "\"naturalDegAvg\":%.2f,\"naturalDegMax\":%.2f,\"naturalLastDeg\":%.2f},",double(sigmaDeg()),u(ctr.spreadFrames),u(ctr.spreadShots),u(ctr.spreadTagged),
        double(spreadLastDeg.load()),u(ctr.spreadHist[0]),u(ctr.spreadHist[1]),u(ctr.spreadHist[2]),u(ctr.spreadHist[3]),
        nn?double(ctr.naturalMdegSum.load())/double(nn)/1000.0:0.0,double(ctr.naturalMdegMax.load())/1000.0,double(naturalLastDeg.load()));out+=b;
    {std::lock_guard lock(poseMutex);const auto& r=poseRecord;
        std::snprintf(b,sizeof b,"\"native\":{\"restAxisModel\":%s,\"maxDeg\":%.1f,\"maxAxisModel\":%s,\"frames\":%llu,\"framesMoving\":%llu,\"changedWithoutClip\":%llu,\"trace\":[",
            v3(r.restAxisModel,1.f).c_str(),double(r.maxDeg),v3(r.maxAxisModel,1.f).c_str(),u(ctr.nativeFrames),u(ctr.nativeMoving),u(ctr.changedWithoutClip));out+=b;
        for(unsigned i=0;i<traceN&&i<trace.size();++i){std::snprintf(b,sizeof b,"%s[%.0f,%.1f]",i?",":"",double(trace[i].ms),double(trace[i].deg));out+=b;}
        std::snprintf(b,sizeof b,"]},\"visual\":{\"config\":\"%s\",\"forceDeg\":%.0f,\"state\":\"%s\",\"cockedDeg\":%.1f,\"applied\":%llu,\"restored\":%llu,\"declined\":%llu,"
            "\"lastDecline\":\"%s\",\"written\":%s,\"pending\":{\"parked\":%llu,\"restored\":%llu,\"dropped\":%llu,\"open\":%u}},",
            config.visual==0?"OFF":config.visual==2?"FORCE":"AUTO",double(config.visualForceDeg),visualState.load(),
            double(visualCockedDeg.load()),u(ctr.visualApplied),u(ctr.visualRestored),u(ctr.visualDeclined),lastVisualDecline.load(),r.written?"true":"false",
            u(ctr.visualPending),u(ctr.visualRestoredPending),u(ctr.visualPendingDropped),unsigned((pendingRestores[0].weapon?1:0)+(pendingRestores[1].weapon?1:0)));out+=b;}
    {std::lock_guard lock(poseMutex);
        std::snprintf(b,sizeof b,"\"drawableProbe\":{\"runs\":%llu,\"failures\":%llu,\"lastFailure\":\"%s\",\"types\":[",u(probe::runs),u(probe::failures),probe::lastFailure.load());
        out+=b;out+=probe::text;out+="]},";}
    const auto oc=ctr.observeReads.load(),pc=ctr.poseCalls.load();
    std::snprintf(b,sizeof b,"\"ring\":{\"enabled\":%s,\"shown\":%llu,\"blinks\":%llu},\"perf\":{\"observeCalls\":%llu,\"observeReads\":%llu,\"observeFailures\":%llu,"
        "\"ptrRefreshes\":%llu,\"observeUsAvg\":%.1f,\"observeUsMax\":%llu,\"poseCalls\":%llu,\"poseUsAvg\":%.1f,\"poseUsMax\":%llu,\"xrUsAvg\":%.2f,\"xrUsMax\":%llu},"
        "\"holsters\":{\"holdGuardActive\":%s,\"cocked\":%s,\"aimAgeMs\":%.0f}}",config.ring?"true":"false",u(ctr.ringShown),u(ctr.ringBlinks),u(ctr.observeCalls),
        (unsigned long long)oc,u(ctr.observeFailures),u(ctr.ptrRefreshes),ctr.observeCalls.load()?double(ctr.observeUsSum.load())/double(ctr.observeCalls.load()):0.0,
        u(ctr.observeUsMax),(unsigned long long)pc,pc?double(ctr.poseUsSum.load())/double(pc):0.0,u(ctr.poseUsMax),xs?double(ctr.xrUsSum.load())/double(xs):0.0,u(ctr.xrUsMax),
        now<H::hammerHoldGuardUntilNs.load()?"true":"false",H::hammerCocked.load()?"true":"false",
        H::hammerAimNs.load()&&now>=H::hammerAimNs.load()?double(now-H::hammerAimNs.load())/1e6:-1.0);out+=b;
    return out;
}
} // namespace rdrvr::vr::hammer
