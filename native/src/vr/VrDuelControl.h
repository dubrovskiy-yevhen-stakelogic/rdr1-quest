#pragma once
// VR DUEL: the game does not put the pistol in the hand, the player really draws it
// from the holster and shoots instead of buttons. The engine duel machine and the Dueling script stay; the draw and the shots become physical.
//
// Engine facts (librdr.so 1.58 arm64, vaddr; all.s). aniDuel = actor +0x38 +0xab8 +0x20: +0x30 state, +0x34 opponent guid, +0x4c hostage guid,
// +0x54 result (1 win, 2 disarm, 3 lost, 4 hostage), +0x10c timer, +0x110 timer length, +0x168 no disarm, +0x1a0 time the target drew, +0x1a9 ammo filled.
//  - The auto-select is RequestDuel 0x2166ce0 (right after HolsterItems) + the per-frame retry of state 1 at 0x216623c, both SelectItem(position 1)
//    0x2663a7c through its PLT; RequestDuel runs for the NPC too at the same call sites. Nothing in aniDuel's state 2 / 3 path dereferences the
//    on-hand weapon (DuelCleanUp checks it), so the machine runs with empty hands.
//  - UpdatePlayerDrawInput 0x2168a6c (state 2, player only) = stick flick or draw input -> aniDuel::Draw 0x21670ec. Draw only needs state 2.
//  - Draw -> sagPlayerHandle::SetDeadEyeDueling(true, ..) 0x26a9750 sets player+0x3be bit2 + BeginDeadEyeCommon; with that bit sagPlayer::UpdateAttackInput
//    0x269a010 never fires (0x269a080) and CalculateDuelScore 0x269d2ec scores locks. Bit clear = the ordinary fire path, score 0 (timeout = lost, stock).
//  - DuelMachine state 3 0x2165c98: reticle (actor+0xb8) +0x6060 == +0x6080 hides the reticule and SetTimeWarp(2,10,10,t/2) every frame (timer 5x faster),
//    so the max count (SetMaxNumLocks 0x26ca71c, normally set by BeginDeadEyeCommon's caller) must not stay equal to the lock count.
//  - sagPlayerStates::AllowAttackWeapon 0x26ac2e4: duel state != 0 -> allowed only if the game / marketing camera is active (0x26ac3b4).
//  - health +0x150 bit 0x100 = one shot death (sagActorScript::GetActorOneShotDeathStatus 0x1cab558): hlthHealthComponent::HandleInjure 0x1e8429c branches to
//    Kill at 0x1e84724; the stock duel sets it on the loser before the winner's shot lands (0x2166a94 / 0x2166698). msg +0x80 = hit zone (CalculateDamageModifierByZone 0x1e81d44,
//    7 / 8 cripple 0x400 / 0x200 = the LEGS: "LLegImpaired", aniRide::CanMountFromCurrentPosition), msg +0xd4 = attacker guid.
//  - HolsterItems 0x2667f68 only moves the model (item +0x1c = 2); inv+0x80 stays the drawn item until PutAwayItem. A weapon in the hand when the duel starts goes back by the
//    holsters' own put-away while the machine is before state 2 (VrHolsters.h DUEL_NO_DRAW), or it would count as the draw at state 2 and the standoff would not have empty hands.
//  - State 5 with result 1 waits for aniAim+0x3bc == 0 (0x2165950): never true with a force-aimed VR gun. Result 2 takes the same exit without the wait.
//  - Events 0x26 / 0x27 / 0x28 / 0x29 reach the script (win / disarm / lost / hostage) in state 4; event 0x28 makes it kill a surviving player.
//
// Hooks (GOT slots, whole-function prints + GOT words at install, any mismatch = no hook at all = the stock duel; each runs at the engine's own call site, so on its thread):
//   DuelMachine 0x2e3c390            player's duel: pre / post logic below (NPCs: straight through)
//   UpdatePlayerDrawInput 0x2e3c398  route: the original is skipped (no stick / pad / arrow draw); a grab -> Draw
//   SetDeadEyeDueling 0x2e487c8      route, on = true: skipped, max locks 26 instead (no Dead Eye, no painting)
//   SelectItem(position) 0x2e47710   LR 0x2166ce4 / 0x2166240 + the player's inventory: route latched at the first, auto draw blocked at both
//   HandleInjure 0x2e32578           route: the player's hit on the opponent = one shot death, or a disarm on an arm zone
//   AllowAttackWeapon 0x2e52eb8      route, states >= 3: a false becomes true (the duel's camera gate)
//   CheckIsDueling 0x2e52f40         route, LR 0x2b3e144 (DuelingContext::IsContextActive): false, no touch arrow widgets
//   AllocateMessage 0x2e47a80        route, keys "Dueling_Tutorial_Draw*" (DrawNow: Dueling script 0x4b0, shown in any duel when the opponent draws; Draw / Draw_Force: the tutorial script): no message (every caller checks the null)
// The rest of the route is engine::WeaponContext::duel = false (VrDuelRoute.h): the mod's whole normal gun applies; a holster grab of the sidearm in state 2 = Draw.
//
// Result (DuelMachine pre, state 3, result still 0): opponent dead -> 1, disarm pending -> 2, hostage dead -> 4, the same writes as the stock verdict 0x2166a60..0x2166ad8,
// then timer = -1 and CancelCurrentTimeWarp(true) (state 5 would block on the warp). No verdict by the timeout: score 0 -> the stock "lost".
// Levers (read per duel at the latch): debug.rdrvr.vr.duel.vr 0 = stock route; .disarm 0 = arm hits kill too; .hidetouch 0 = keep the touch widgets and the draw prompts; .slowmo 0 = real time
// (the warp cancelled, the timers rescaled to the same real window); .fire 0 = never the direct path, 1 = auto (a forwarded trigger edge with no engine shot within 400 ms ->
// aniAim::RequestFire(false) as the NPC's 0x2166130), 2 = always direct. Report vr.holsters.duel.vr.
#include "VrDuelRoute.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace rdrvr::vr::duelctl {
using ReadFn=bool(*)(void*,std::uintptr_t,void*,std::size_t);
using MachineFn=void(*)(void*);using DrawInputFn=void(*)(void*);using DrawFn=bool(*)(void*);
using SetDuelingFn=void(*)(void* handle,bool on,bool hostage);
using SelectFn=bool(*)(void* inventory,int position,bool draw,bool animated);
using InjureFn=void(*)(void* health,const void* msg);
using StatesFn=bool(*)(const void* states);
using MaxLocksFn=void(*)(void* reticle,int count);
using OnHandFn=void*(*)(void* inventory);using PerClipFn=float(*)(const void* weapon);using SetAmmoFn=void(*)(void* weapon,void* ammo,float count);
using ChamberFn=void(*)(void* weapon,float seconds);using CancelWarpFn=void(*)(void* tod,bool end);using UiObjectFn=void*(*)(void* manager,const char* name);
using RequestFireFn=void(*)(void* aim,bool);
using MessageFn=void*(*)(void* ui,const char* key,float seconds,bool raw,int flags,bool unique,void* speech,bool last);

struct Print {std::uintptr_t offset;std::uint32_t size;std::uint64_t fnv;};
inline constexpr Print machinePrint{0x21658c0,0x1224,0xe8741db7cce8f668ull},drawInputPrint{0x2168a6c,0x230,0x763308a35cfa4e5eull},drawPrint{0x21670ec,0x580,0xa4ed29adcdc98f6aull},
    setDuelingPrint{0x26a9750,0x10,0xa6e0986cfc4a784bull},selectPrint{0x2663a7c,0xf8,0xe05764e2dbfff306ull},injurePrint{0x1e8429c,0xc98,0x6dedf88f31dccd6eull},
    allowPrint{0x26ac2e4,0x130,0x24af01279b9bcff8ull},isDuelingPrint{0x26ab7a0,0x40,0xef72e9a0b1aaa1b4ull},maxLocksPrint{0x26ca71c,0x18,0x0531079739d6be56ull},
    onHandPrint{0x26605e8,0x34,0xaed6622a467a963aull},perClipPrint{0x254608c,0x84,0xe0866f9c9f361867ull},setAmmoPrint{0x2542f08,0x1d8,0xa413a6a7020dd996ull},
    chamberPrint{0x254627c,0x8,0x2cad579f03bfe12full},cancelWarpPrint{0x23997f4,0x38,0x22458e1d5cf92568ull},uiObjectPrint{0x1a61314,0x9c,0xf9f7fb4516b9a31eull},
    requestFirePrint{0x1cc29d0,0xd4c,0xb5c8770cf32a0beeull},messagePrint{0x2564588,0x124,0x2825c01c4b8d22ffull};
// Never called, the layouts above depend on them: sagPlayer::SetDeadEyeDueling, DuelingContext::IsContextActive, DuelCleanUp, CancelDuel, UpdateAttackInput, SetTargetDrawn.
inline constexpr std::array<Print,6> witnessPrints{{{0x269d1e4,0x108,0xd05852cead25f3baull},{0x2b3e124,0x3c,0xc897180937fd18aaull},{0x216806c,0x304,0xfc16cc9dffd97f6cull},
    {0x2166b0c,0xbc,0xe43c20437b7eb957ull},{0x269a010,0x214,0xc4277e7dc77ec03cull},{0x2167c40,0xec,0x950a073a73359d94ull}}};
inline constexpr std::uintptr_t machineGot=0x2e3c390,drawInputGot=0x2e3c398,setDuelingGot=0x2e487c8,selectGot=0x2e47710,injureGot=0x2e32578,allowGot=0x2e52eb8,isDuelingGot=0x2e52f40,messageGot=0x2e47a80;
inline constexpr char drawPromptPrefix[]="Dueling_Tutorial_Draw";
// A machine that stopped ticking while the player is alive in game (polls, not time: a pause stops both): the route lets go, no stuck grips / R3 / holster mask.
inline constexpr std::uint32_t machineStalledPolls=600;
// sm_Instance / sm_GlobalManager / actor guid tables: the GOT words the engine code above loads
inline constexpr std::uintptr_t uiManagerGot=0x2ddc2f0,uiManagerOffset=0x2f88a90,todGot=0x2e014f8,todOffset=0x4e75cb30,timeGot=0x2ddf340,timeOffset=0x4e6d6510,
    actorManagerGot=0x2deaba8,actorManagerOffset=0x4e6f6b40,actorSlotsGot=0x2deabb0,actorSlotsOffset=0x4e6f6b38;
inline constexpr std::uintptr_t lrRequestDuel=0x2166ce4,lrStateOne=0x2166240,lrTouchContext=0x2b3e144;
namespace off {
inline constexpr std::size_t duelAnim=0x8,duelData=0x10,duelState=0x30,duelOpponent=0x34,duelHostage=0x4c,duelTargetCount=0x50,duelResult=0x54,duelTarget=0x70,duelTimer=0x10c,duelTimerLen=0x110,
    duelNoDisarm=0x168,duelTargetDrew=0x1a0,dataScale=0x0,dataChamber=0x18,
    animActor=0x10,animBlock=0xab8,blockAim=0x8,blockDuel=0x20,actorGuid=0x8,actorHealth=0x60,actorInventory=0x70,actorReticle=0xb8,actorFlags=0x118,
    invOwner=0x10,invAmmo=0x410,invSlotEnabled=0x48e,invSlotItem=0xa8,invSlotStride=0x70,healthPoints=0x20,healthFlags=0x150,msgZone=0x80,msgAttacker=0xd4,
    reticleTarget=0x3810,reticleLocks=0x6060,reticleMaxLocks=0x6080,timeScale=0x84,todWarping=0x30;
inline constexpr std::uint32_t healthDead=0x40,healthOneShot=0x100,healthDisarmed=0x10008,healthNoHarm=0x4;
inline constexpr int sidearmPosition=1;
}
enum Result : std::int32_t {ResultNone=0,ResultWin=1,ResultDisarm=2,ResultLost=3,ResultHostage=4};

struct Api {
    ReadFn read{};std::uintptr_t base{};
    MachineFn machine{};DrawInputFn drawInput{};DrawFn draw{};SetDuelingFn setDueling{};SelectFn select{};InjureFn injure{};StatesFn allow{},isDueling{};
    MaxLocksFn maxLocks{};OnHandFn onHand{};PerClipFn perClip{};SetAmmoFn setAmmo{};ChamberFn chamber{};CancelWarpFn cancelWarp{};UiObjectFn uiObject{};RequestFireFn requestFire{};MessageFn message{};
    std::uintptr_t uiManager{},tod{},time{},actorManager{},actorSlots{};
};
inline Api api{};

// ---- state of the running route: written by the hooks / the poll, read by the input brain and the report ----
inline constexpr const char* outcomeNone="NONE";
inline std::atomic<const char*> installState{"NOT_INSTALLED"},lastError{""},phase{"IDLE"},outcome{outcomeNone};
inline std::atomic<std::uintptr_t> playerDuel{},hostageHealth{},playerHealth{};
// The duel's targets (+0x34 + 4 * i, count +0x50: AddDueler can make more than one), resolved every machine tick for the injure hook.
inline constexpr unsigned maxTargets=6;
inline std::array<std::atomic<std::uintptr_t>,maxTargets> targetHealth{};
inline std::atomic<std::uint32_t> playerGuid{},pollsSinceTick{};
inline std::atomic<std::uintptr_t> filledWeapon{};
inline std::atomic<int> engineState{-1},engineResult{0},lastZone{-1},oppState{-1};
inline std::atomic<bool> drawWanted{},disarmPending{},drew{},timersScaled{};
// Written by the holsters every tick: their engine side is ready and the menu row is ON. Off = no holster grab can ever draw: the stock route (the stick draw stays).
inline std::atomic<bool> holstersOn{};
inline std::atomic<std::uint64_t> duels{},stockDuels{},noSidearmDuels{},grabs{},drawInputsSent{},autoDrawBlocked{},deadEyeBlocked{},allowOverrides{},touchHidden{},uiHides{},
    edges{},edgesForwarded{},shots{},directFires{},hits{},hitsTaken{},armHits{},lockFixes{},resultFixes{},ammoFills{},warpCancels{},targetWrites{},machineTicks{},promptsHidden{},handsCleared{};
inline std::atomic<std::uint64_t> standoffNs{},drawNs{},firstShotNs{},oppDrawNs{},lastForwardedNs{};
inline std::atomic<std::uint32_t> oppFlags{};
inline std::atomic<float> timerValue{},timerLength{},scaleValue{},dataScaleValue{};
inline std::atomic<bool> oppAlive{};
struct Levers {bool disarm{true},hideTouch{true},slowMo{true};int fire{1};};
inline Levers levers{};

namespace detail {
inline std::uint64_t steadyNs() noexcept {timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return std::uint64_t(t.tv_sec)*1'000'000'000ull+std::uint64_t(t.tv_nsec);}
template<class T> inline T at(std::uintptr_t p,std::size_t o) noexcept {return *reinterpret_cast<const T*>(p+o);}
template<class T> inline void put(std::uintptr_t p,std::size_t o,T v) noexcept {*reinterpret_cast<T*>(p+o)=v;}
inline int prop(const char* name,int fallback) noexcept {
#if defined(__ANDROID__)
    char v[PROP_VALUE_MAX]{};if(__system_property_get(name,v)>0)return std::atoi(v);
#else
    (void)name;
#endif
    return fallback;
}
inline std::uint64_t codeHash(std::uintptr_t address,std::uint32_t size) noexcept {
    std::uint64_t h=0xcbf29ce484222325ull;unsigned char chunk[256];
    for(std::uint32_t done=0;done<size;){const std::uint32_t n=std::min<std::uint32_t>(size-done,sizeof chunk);
        if(!api.read(nullptr,address+done,chunk,n))return 0;
        for(std::uint32_t i=0;i<n;++i)h=(h^chunk[i])*0x100000001b3ull;
        done+=n;}
    return h;
}
// The engine's own guid lookup (slot + generation). Engine threads only: the tables are live while the machine runs.
inline std::uintptr_t resolve(std::uint32_t guid) noexcept {
    const auto manager=at<std::uintptr_t>(api.actorManager,0),slots=at<std::uintptr_t>(api.actorSlots,0);
    if(!manager||!slots||(guid&0xffffu)>=at<std::uint16_t>(manager,0)||at<std::uint16_t>(slots,std::size_t(guid&0xffffu)*16+8)!=(guid>>16))return 0;
    return at<std::uintptr_t>(slots,std::size_t(guid&0xffffu)*16);
}
inline std::uintptr_t duelOf(std::uintptr_t actor) noexcept {
    const auto anim=at<std::uintptr_t>(actor,0x38);if(!anim)return 0;
    const auto block=at<std::uintptr_t>(anim,off::animBlock);return block?at<std::uintptr_t>(block,off::blockDuel):0;
}
inline std::uintptr_t actorOfDuel(std::uintptr_t duel) noexcept {return at<std::uintptr_t>(at<std::uintptr_t>(duel,off::duelAnim),off::animActor);}
inline bool isPlayerActor(std::uintptr_t actor) noexcept {return actor&&(at<std::uint32_t>(actor,off::actorFlags)&3u)==3u;}
inline bool dead(std::uintptr_t health) noexcept {return health&&(!(at<float>(health,off::healthPoints)>0.f)||(at<std::uint32_t>(health,off::healthFlags)&off::healthDead));}
inline void orFlags(std::uintptr_t health,std::uint32_t bits) noexcept {if(health)put<std::uint32_t>(health,off::healthFlags,at<std::uint32_t>(health,off::healthFlags)|bits);}
inline void* todInstance() noexcept {return at<void*>(api.tod,0);}
inline void hideUi() noexcept {
    const auto manager=at<void*>(api.uiManager,0);if(!manager)return;
    for(const char* name:{"DuelReticule","PlayerScore","OpponentScore"}){
        const auto object=reinterpret_cast<std::uintptr_t>(api.uiObject(manager,name));if(!object)continue;
        reinterpret_cast<void(*)(void*)>(at<std::uintptr_t>(at<std::uintptr_t>(object,0),0x168))(reinterpret_cast<void*>(object));}
    uiHides.fetch_add(1,std::memory_order_relaxed);
}
inline void finish() noexcept {
    if(!route.exchange(false,std::memory_order_relaxed))return;
    playerDuel.store(0,std::memory_order_relaxed);hostageHealth.store(0,std::memory_order_relaxed);playerHealth.store(0,std::memory_order_relaxed);
    for(auto& t:targetHealth)t.store(0,std::memory_order_relaxed);
    drawWanted.store(false,std::memory_order_relaxed);disarmPending.store(false,std::memory_order_relaxed);engineState.store(0,std::memory_order_relaxed);
    if(outcome.load(std::memory_order_relaxed)==outcomeNone)outcome.store("CANCELLED",std::memory_order_relaxed);
    phase.store("ENDED",std::memory_order_relaxed);
}
// Route decision at the first SelectItem of the player's RequestDuel.
inline void latch(std::uintptr_t inventory,std::uintptr_t actor) noexcept {
    finish();
    const auto duel=duelOf(actor);if(!duel)return;
    if(prop("debug.rdrvr.vr.duel.vr",1)==0){stockDuels.fetch_add(1,std::memory_order_relaxed);phase.store("STOCK",std::memory_order_relaxed);return;}
    if(!holstersOn.load(std::memory_order_relaxed)){stockDuels.fetch_add(1,std::memory_order_relaxed);lastError.store("HOLSTERS OFF - STOCK ROUTE",std::memory_order_relaxed);phase.store("STOCK",std::memory_order_relaxed);return;}
    // the same test as SelectItem: the slot enabled and holding an item. No sidearm = nothing to draw: the stock route for this duel.
    const int pos=off::sidearmPosition;
    if(!(at<std::uint8_t>(inventory+std::size_t(pos>>3),off::invSlotEnabled)>>(pos&7)&1u)||!at<std::uintptr_t>(inventory+std::size_t(pos)*off::invSlotStride,off::invSlotItem)){
        noSidearmDuels.fetch_add(1,std::memory_order_relaxed);lastError.store("NO SIDEARM - STOCK ROUTE",std::memory_order_relaxed);phase.store("STOCK",std::memory_order_relaxed);return;}
    levers=Levers{prop("debug.rdrvr.vr.duel.disarm",1)!=0,prop("debug.rdrvr.vr.duel.hidetouch",1)!=0,prop("debug.rdrvr.vr.duel.slowmo",1)!=0,prop("debug.rdrvr.vr.duel.fire",1)};
    playerDuel.store(duel,std::memory_order_relaxed);playerGuid.store(at<std::uint32_t>(actor,off::actorGuid),std::memory_order_relaxed);
    playerHealth.store(at<std::uintptr_t>(actor,off::actorHealth),std::memory_order_relaxed);
    filledWeapon.store(0,std::memory_order_relaxed);pollsSinceTick.store(0,std::memory_order_relaxed);
    drew.store(false,std::memory_order_relaxed);timersScaled.store(false,std::memory_order_relaxed);drawWanted.store(false,std::memory_order_relaxed);disarmPending.store(false,std::memory_order_relaxed);
    outcome.store(outcomeNone,std::memory_order_relaxed);lastError.store("",std::memory_order_relaxed);phase.store("STANDOFF",std::memory_order_relaxed);lastZone.store(-1,std::memory_order_relaxed);
    standoffNs.store(0,std::memory_order_relaxed);drawNs.store(0,std::memory_order_relaxed);firstShotNs.store(0,std::memory_order_relaxed);oppDrawNs.store(0,std::memory_order_relaxed);lastForwardedNs.store(0,std::memory_order_relaxed);
    duels.fetch_add(1,std::memory_order_relaxed);
    route.store(true,std::memory_order_relaxed);
}
// The stock verdict's writes (0x2166a04..0x2166ad8, 0x2166788..0x2166828), which the timeout branch would make; the machine goes to state 4 this very tick.
inline void verdict(std::uintptr_t duel,Result r,const std::array<std::uintptr_t,maxTargets>& actors,std::uintptr_t hostage) noexcept {
    put<std::int32_t>(duel,off::duelResult,r);
    for(const auto oppActor:actors)if(oppActor){
        if(const auto d=duelOf(oppActor);d&&!at<std::int32_t>(d,off::duelResult))put<std::int32_t>(d,off::duelResult,ResultLost);
        if(r==ResultWin)orFlags(at<std::uintptr_t>(oppActor,off::actorHealth),off::healthOneShot);
        else if(r==ResultDisarm)orFlags(at<std::uintptr_t>(oppActor,off::actorHealth),off::healthDisarmed);
    }
    if(r==ResultHostage)orFlags(hostage,off::healthOneShot);else orFlags(hostage,off::healthNoHarm);
    put<float>(duel,off::duelTimer,-1.f);
    api.cancelWarp(todInstance(),true);warpCancels.fetch_add(1,std::memory_order_relaxed);
}
inline void pre(std::uintptr_t duel) noexcept {
    machineTicks.fetch_add(1,std::memory_order_relaxed);
    const int state=at<std::int32_t>(duel,off::duelState);const int result=at<std::int32_t>(duel,off::duelResult);
    const auto hostageGuid=at<std::uint32_t>(duel,off::duelHostage);
    const auto hostageActor=(hostageGuid&0xffffu)?resolve(hostageGuid):0;
    const auto hh=hostageActor?at<std::uintptr_t>(hostageActor,off::actorHealth):0;
    hostageHealth.store(hh,std::memory_order_relaxed);
    // Every target dead = the win (a target the guid table no longer knows counts as gone). Stock scores the first one only: duels with more are rare, the rule stays the same.
    std::array<std::uintptr_t,maxTargets> actors{};const unsigned targets=unsigned(std::clamp(at<std::int32_t>(duel,off::duelTargetCount),0,int(maxTargets)));
    bool allDead=targets>0;
    for(unsigned i=0;i<maxTargets;++i){
        std::uintptr_t oh=0;
        if(i<targets){actors[i]=resolve(at<std::uint32_t>(duel,off::duelOpponent+i*4));if(actors[i])oh=at<std::uintptr_t>(actors[i],off::actorHealth);
            if(oh&&!dead(oh))allDead=false;}
        targetHealth[i].store(oh,std::memory_order_relaxed);}
    if(state==2&&!standoffNs.load(std::memory_order_relaxed))standoffNs.store(steadyNs(),std::memory_order_relaxed);
    if(state==2&&at<float>(duel,off::duelTargetDrew)>0.f&&!oppDrawNs.load(std::memory_order_relaxed))oppDrawNs.store(steadyNs(),std::memory_order_relaxed);
    if(state>=2&&state<=3&&!levers.slowMo){ // REAL TIME: no warp; the timers keep the same real window (first tick of state 2: x 1 / data scale)
        const float scale=at<float>(at<std::uintptr_t>(duel,off::duelData),off::dataScale);
        if(!timersScaled.exchange(true,std::memory_order_relaxed)&&scale>.001f&&scale<1.f)
            for(unsigned i=0;i<=maxTargets;++i){const std::uintptr_t d=i==maxTargets?duel:actors[i]?duelOf(actors[i]):0;if(!d)continue;
                put<float>(d,off::duelTimerLen,at<float>(d,off::duelTimerLen)/scale);put<float>(d,off::duelTimer,at<float>(d,off::duelTimer)/scale);}
        if(at<std::uint8_t>(reinterpret_cast<std::uintptr_t>(todInstance()),off::todWarping)){api.cancelWarp(todInstance(),true);warpCancels.fetch_add(1,std::memory_order_relaxed);}
    }
    if(state==3){
        const auto reticle=at<std::uintptr_t>(actorOfDuel(duel),off::actorReticle);
        if(reticle&&at<std::int32_t>(reticle,off::reticleLocks)==at<std::int32_t>(reticle,off::reticleMaxLocks)){
            api.maxLocks(reinterpret_cast<void*>(reticle),26);lockFixes.fetch_add(1,std::memory_order_relaxed);}
        if(!result){
            if(allDead)verdict(duel,ResultWin,actors,hh);
            else if(disarmPending.load(std::memory_order_relaxed))verdict(duel,ResultDisarm,actors,hh);
            else if(hh&&dead(hh))verdict(duel,ResultHostage,actors,hh);
        }
    }else if(state==5&&result==ResultWin){ // the stock wait for aniAim state 0 never ends with a force-aimed VR gun; result 2 = the same exit without it (0x2165944)
        put<std::int32_t>(duel,off::duelResult,ResultDisarm);resultFixes.fetch_add(1,std::memory_order_relaxed);}
}
inline void post(std::uintptr_t duel) noexcept {
    const int state=at<std::int32_t>(duel,off::duelState);const int result=at<std::int32_t>(duel,off::duelResult);
    const int was=engineState.exchange(state,std::memory_order_relaxed);engineResult.store(result,std::memory_order_relaxed);
    if(!state){finish();return;}
    const auto actor=actorOfDuel(duel);
    const auto inventory=at<std::uintptr_t>(actor,off::actorInventory);
    if(state==3){
        phase.store("DRAWN",std::memory_order_relaxed);
        if(const auto weapon=reinterpret_cast<std::uintptr_t>(api.onHand(reinterpret_cast<void*>(inventory)))){
            if(weapon!=filledWeapon.load(std::memory_order_relaxed)){ // what state 1 does for the stock's auto drawn gun (0x2165f40..0x2165f68); by weapon: +0x1a9 can be set by another weapon held when the duel began (0x2166e1c)
                api.setAmmo(reinterpret_cast<void*>(weapon),reinterpret_cast<void*>(inventory+off::invAmmo),api.perClip(reinterpret_cast<void*>(weapon)));
                api.chamber(reinterpret_cast<void*>(weapon),at<float>(at<std::uintptr_t>(duel,off::duelData),off::dataChamber));
                filledWeapon.store(weapon,std::memory_order_relaxed);ammoFills.fetch_add(1,std::memory_order_relaxed);}
            // aniAim::UpdateInternalStates takes its aim point from here in a duel (0x1cbbd38, the reticle copy 0x26c43ec): the barrel point of the reticle Update hook, not the machine's own target
            if(const auto reticle=at<std::uintptr_t>(actor,off::actorReticle)){
                const float t[3]{at<float>(reticle,off::reticleTarget),at<float>(reticle,off::reticleTarget+4),at<float>(reticle,off::reticleTarget+8)};
                put<float>(duel,off::duelTarget,t[0]);put<float>(duel,off::duelTarget+4,t[1]);put<float>(duel,off::duelTarget+8,t[2]);targetWrites.fetch_add(1,std::memory_order_relaxed);}
            // FIRE: the ordinary path gave no shot for this forwarded edge (.fire 1), or the direct path is forced (.fire 2): the NPC's own call
            if(levers.fire>0){const auto edge=lastForwardedNs.load(std::memory_order_relaxed);
                if(edge&&(levers.fire>=2||steadyNs()-edge>400'000'000ull)){lastForwardedNs.store(0,std::memory_order_relaxed);
                    api.requestFire(at<void*>(at<std::uintptr_t>(at<std::uintptr_t>(duel,off::duelAnim),off::animBlock),off::blockAim),false);directFires.fetch_add(1,std::memory_order_relaxed);}}
        }
    }else if(state==4||state==5){
        phase.store("RESULT",std::memory_order_relaxed);
        if(outcome.load(std::memory_order_relaxed)==outcomeNone)
            outcome.store(result==ResultWin?"WIN":result==ResultDisarm?"DISARM":result==ResultHostage?"HOSTAGE":drew.load(std::memory_order_relaxed)?"LOSE_TIMEOUT":"LOSE_NPC_FIRST",std::memory_order_relaxed);
    }
    if((state==2||state==3)&&(state!=was||!(machineTicks.load(std::memory_order_relaxed)&15u)))hideUi();
    // report only
    timerValue.store(at<float>(duel,off::duelTimer),std::memory_order_relaxed);timerLength.store(at<float>(duel,off::duelTimerLen),std::memory_order_relaxed);
    scaleValue.store(at<float>(api.time,off::timeScale),std::memory_order_relaxed);
    dataScaleValue.store(at<float>(at<std::uintptr_t>(duel,off::duelData),off::dataScale),std::memory_order_relaxed);
    if(const auto oh=targetHealth[0].load(std::memory_order_relaxed)){oppFlags.store(at<std::uint32_t>(oh,off::healthFlags),std::memory_order_relaxed);oppAlive.store(!dead(oh),std::memory_order_relaxed);}
    if(const auto oa=at<std::uint32_t>(duel,off::duelOpponent)?resolve(at<std::uint32_t>(duel,off::duelOpponent)):0)if(const auto od=duelOf(oa))oppState.store(at<std::int32_t>(od,off::duelState),std::memory_order_relaxed);
}
} // namespace detail

// ---- hooks ----
inline void machineHook(void* self) noexcept {
    const auto duel=reinterpret_cast<std::uintptr_t>(self);
    if(!live()||duel!=playerDuel.load(std::memory_order_relaxed)){api.machine(self);return;}
    pollsSinceTick.store(0,std::memory_order_relaxed);
    detail::pre(duel);api.machine(self);detail::post(duel);
}
inline void drawInputHook(void* self) noexcept {
    if(!live()||reinterpret_cast<std::uintptr_t>(self)!=playerDuel.load(std::memory_order_relaxed)){api.drawInput(self);return;}
    if(!drawWanted.exchange(false,std::memory_order_relaxed))return;
    drew.store(true,std::memory_order_relaxed);drawNs.store(detail::steadyNs(),std::memory_order_relaxed);
    if(api.draw(self))drawInputsSent.fetch_add(1,std::memory_order_relaxed);else lastError.store("DRAW REFUSED",std::memory_order_relaxed);
}
inline void setDuelingHook(void* handle,bool on,bool hostage) noexcept {
    if(!on||!live()){api.setDueling(handle,on,hostage);return;}
    deadEyeBlocked.fetch_add(1,std::memory_order_relaxed);
    const auto duel=playerDuel.load(std::memory_order_relaxed);
    if(const auto reticle=duel?detail::at<std::uintptr_t>(detail::actorOfDuel(duel),off::actorReticle):0)api.maxLocks(reinterpret_cast<void*>(reticle),26);
}
inline bool selectHook(void* inventory,int position,bool draw,bool animated) noexcept {
    const auto lr=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))-api.base;
    if(lr==lrRequestDuel||lr==lrStateOne){
        const auto inv=reinterpret_cast<std::uintptr_t>(inventory);const auto actor=detail::at<std::uintptr_t>(inv,off::invOwner);
        if(detail::isPlayerActor(actor)&&detail::at<std::uintptr_t>(actor,off::actorInventory)==inv){
            if(lr==lrRequestDuel)detail::latch(inv,actor);
            if(live()){autoDrawBlocked.fetch_add(1,std::memory_order_relaxed);return false;}
        }
    }
    return api.select(inventory,position,draw,animated);
}
inline void injureHook(void* health,const void* msg) noexcept {
    if(live()){
        const auto h=reinterpret_cast<std::uintptr_t>(health),m=reinterpret_cast<std::uintptr_t>(msg);
        if(h==playerHealth.load(std::memory_order_relaxed))hitsTaken.fetch_add(1,std::memory_order_relaxed);
        else if(detail::at<std::uint32_t>(m,off::msgAttacker)==playerGuid.load(std::memory_order_relaxed)&&engineState.load(std::memory_order_relaxed)==3){
            bool target=false;for(const auto& t:targetHealth)target=target||(h&&t.load(std::memory_order_relaxed)==h);
            if(target){
                hits.fetch_add(1,std::memory_order_relaxed);const int zone=detail::at<std::int32_t>(m,off::msgZone);lastZone.store(zone,std::memory_order_relaxed);
                const auto duel=playerDuel.load(std::memory_order_relaxed);
                // Damage modifier groups of CalculateDamageModifierByZone: 5 / 6 share one group, 7 / 8 cripple the legs: 5 / 6 read as the arms. The report's lastZone proves it.
                if((zone==5||zone==6)&&levers.disarm&&duel&&!detail::at<std::uint8_t>(duel,off::duelNoDisarm)){armHits.fetch_add(1,std::memory_order_relaxed);disarmPending.store(true,std::memory_order_relaxed);}
                else detail::orFlags(h,off::healthOneShot);
            }else if(h==hostageHealth.load(std::memory_order_relaxed))detail::orFlags(h,off::healthOneShot);
        }
    }
    api.injure(health,msg);
}
inline bool allowHook(const void* states) noexcept {
    if(live()&&levers.fire>=2)return false; // .fire 2: only the direct path shoots
    const bool r=api.allow(states);
    if(!r&&live()&&engineState.load(std::memory_order_relaxed)>=3){allowOverrides.fetch_add(1,std::memory_order_relaxed);return true;}
    return r;
}
inline bool isDuelingHook(const void* states) noexcept {
    if(live()&&levers.hideTouch&&reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))-api.base==lrTouchContext){touchHidden.fetch_add(1,std::memory_order_relaxed);return false;}
    return api.isDueling(states);
}

inline void* messageHook(void* ui,const char* key,float seconds,bool raw,int flags,bool unique,void* speech,bool last) noexcept {
    if(live()&&levers.hideTouch&&key&&!std::strncmp(key,drawPromptPrefix,sizeof drawPromptPrefix-1)){promptsHidden.fetch_add(1,std::memory_order_relaxed);return nullptr;}
    return api.message(ui,key,seconds,raw,flags,unique,speech,last);
}

// ---- input brain / holsters ----
// The holster grab mask while the route runs: only the given sidearm bit, only from the moment the machine can take a draw (state 2) on.
inline std::uint32_t grabMaskFor(std::uint32_t sidearmBit) noexcept {const int s=engineState.load(std::memory_order_relaxed);return s==2||s==3?sidearmBit:0u;}
// Main thread: the holsters' OpDraw of the sidearm went through (also the already-in-hand path). The grab IS the draw input.
inline void noteGrab() noexcept {
    if(!live())return;
    grabs.fetch_add(1,std::memory_order_relaxed);
    if(engineState.load(std::memory_order_relaxed)==2)drawWanted.store(true,std::memory_order_relaxed);
}
// Before the standoff (machine state < 2) the hands must be empty: a weapon still in the hand goes back to its holster.
inline bool standoffPending() noexcept {return live()&&engineState.load(std::memory_order_relaxed)<2;}
inline void noteHandCleared() noexcept {handsCleared.fetch_add(1,std::memory_order_relaxed);}
// Main thread, every holsters poll: a gun that reached the VR hand by any path in state 2 is a draw too. The route ends with the machine, when the player's actor no longer owns this
// duel (respawn / load: the pointer is stale, its state means nothing) and when the machine stopped ticking with the player alive in game. actor 0 = no player this poll.
inline void poll(bool gunInVrHand,std::uintptr_t actor=0,bool inGame=false) noexcept {
    if(!live())return;
    const auto duel=playerDuel.load(std::memory_order_relaxed);std::int32_t state{};
    if(!duel||!api.read||!api.read(nullptr,duel+off::duelState,&state,sizeof state)||!state){detail::finish();return;}
    if(actor){
        std::uintptr_t anim{},block{},owned{};
        if(!api.read(nullptr,actor+0x38,&anim,sizeof anim)||!anim||!api.read(nullptr,anim+off::animBlock,&block,sizeof block)||!block||
           !api.read(nullptr,block+off::blockDuel,&owned,sizeof owned)||owned!=duel){lastError.store("ACTOR CHANGED",std::memory_order_relaxed);detail::finish();return;}
    }
    if(inGame&&pollsSinceTick.fetch_add(1,std::memory_order_relaxed)>=machineStalledPolls){lastError.store("MACHINE STOPPED",std::memory_order_relaxed);detail::finish();return;}
    if(state==2&&gunInVrHand)drawWanted.store(true,std::memory_order_relaxed);
}
// Input thread: a physical trigger edge of the VR-held gun while the route runs.
inline void noteTriggerEdge(bool forwarded) noexcept {
    if(!live())return;
    edges.fetch_add(1,std::memory_order_relaxed);
    if(forwarded){edgesForwarded.fetch_add(1,std::memory_order_relaxed);lastForwardedNs.store(detail::steadyNs(),std::memory_order_relaxed);}
}
// weapWeapon::FireProjectile of the VR-held weapon.
inline void noteShot() noexcept {
    if(!live())return;
    shots.fetch_add(1,std::memory_order_relaxed);lastForwardedNs.store(0,std::memory_order_relaxed);
    if(!firstShotNs.load(std::memory_order_relaxed))firstShotNs.store(detail::steadyNs(),std::memory_order_relaxed);
}

// Startup (arm thread). Nothing is patched before every print and GOT word checked out; a failed patch rolls back.
template<class Replace>
inline void install(ReadFn read,std::uintptr_t base,Replace&& replace) noexcept {
    using namespace detail;
    api=Api{};api.read=read;api.base=base;
    if(!read){installState.store("NO READ",std::memory_order_relaxed);return;}
    const auto ok=[&](const Print& p){return codeHash(base+p.offset,p.size)==p.fnv;};
    const auto got=[&](std::uintptr_t slot,std::uintptr_t target){std::uintptr_t v{};return read(nullptr,base+slot,&v,sizeof v)&&v==base+target;};
    bool all=ok(machinePrint)&&ok(drawInputPrint)&&ok(drawPrint)&&ok(setDuelingPrint)&&ok(selectPrint)&&ok(injurePrint)&&ok(allowPrint)&&ok(isDuelingPrint)&&ok(maxLocksPrint)&&
        ok(onHandPrint)&&ok(perClipPrint)&&ok(setAmmoPrint)&&ok(chamberPrint)&&ok(cancelWarpPrint)&&ok(uiObjectPrint)&&ok(requestFirePrint)&&ok(messagePrint);
    for(const auto& p:witnessPrints)all=all&&ok(p);
    all=all&&got(uiManagerGot,uiManagerOffset)&&got(todGot,todOffset)&&got(timeGot,timeOffset)&&got(actorManagerGot,actorManagerOffset)&&got(actorSlotsGot,actorSlotsOffset);
    struct Slot {std::uintptr_t got,target,hook;};
    const std::array<Slot,8> slots{{{machineGot,machinePrint.offset,reinterpret_cast<std::uintptr_t>(&machineHook)},{drawInputGot,drawInputPrint.offset,reinterpret_cast<std::uintptr_t>(&drawInputHook)},
        {setDuelingGot,setDuelingPrint.offset,reinterpret_cast<std::uintptr_t>(&setDuelingHook)},{selectGot,selectPrint.offset,reinterpret_cast<std::uintptr_t>(&selectHook)},
        {injureGot,injurePrint.offset,reinterpret_cast<std::uintptr_t>(&injureHook)},{allowGot,allowPrint.offset,reinterpret_cast<std::uintptr_t>(&allowHook)},
        {isDuelingGot,isDuelingPrint.offset,reinterpret_cast<std::uintptr_t>(&isDuelingHook)},{messageGot,messagePrint.offset,reinterpret_cast<std::uintptr_t>(&messageHook)}}};
    for(const auto& s:slots)all=all&&got(s.got,s.target);
    if(!all){installState.store("ENGINE MISMATCH",std::memory_order_relaxed);lastError.store("ENGINE MISMATCH",std::memory_order_relaxed);return;}
    api.machine=reinterpret_cast<MachineFn>(base+machinePrint.offset);api.drawInput=reinterpret_cast<DrawInputFn>(base+drawInputPrint.offset);api.draw=reinterpret_cast<DrawFn>(base+drawPrint.offset);
    api.setDueling=reinterpret_cast<SetDuelingFn>(base+setDuelingPrint.offset);api.select=reinterpret_cast<SelectFn>(base+selectPrint.offset);api.injure=reinterpret_cast<InjureFn>(base+injurePrint.offset);
    api.allow=reinterpret_cast<StatesFn>(base+allowPrint.offset);api.isDueling=reinterpret_cast<StatesFn>(base+isDuelingPrint.offset);api.maxLocks=reinterpret_cast<MaxLocksFn>(base+maxLocksPrint.offset);
    api.onHand=reinterpret_cast<OnHandFn>(base+onHandPrint.offset);api.perClip=reinterpret_cast<PerClipFn>(base+perClipPrint.offset);api.setAmmo=reinterpret_cast<SetAmmoFn>(base+setAmmoPrint.offset);
    api.chamber=reinterpret_cast<ChamberFn>(base+chamberPrint.offset);api.cancelWarp=reinterpret_cast<CancelWarpFn>(base+cancelWarpPrint.offset);api.uiObject=reinterpret_cast<UiObjectFn>(base+uiObjectPrint.offset);
    api.requestFire=reinterpret_cast<RequestFireFn>(base+requestFirePrint.offset);api.message=reinterpret_cast<MessageFn>(base+messagePrint.offset);
    api.uiManager=base+uiManagerOffset;api.tod=base+todOffset;api.time=base+timeOffset;api.actorManager=base+actorManagerOffset;api.actorSlots=base+actorSlotsOffset;
    std::size_t patched=0;
    for(;patched<slots.size();++patched)if(!replace(base+slots[patched].got,base+slots[patched].target,slots[patched].hook))break;
    if(patched!=slots.size()){
        while(patched){--patched;replace(base+slots[patched].got,slots[patched].hook,base+slots[patched].target);}
        installState.store("PATCH FAILED",std::memory_order_relaxed);lastError.store("PATCH FAILED",std::memory_order_relaxed);return;}
    installState.store("READY",std::memory_order_relaxed);
}

// Report object vr.holsters.duel.vr (no leading comma).
inline std::string report() {
    const auto u=[](const std::atomic<std::uint64_t>& a){return (unsigned long long)a.load(std::memory_order_relaxed);};
    const auto ms=[](std::uint64_t from,std::uint64_t to){return from&&to&&to>=from?double(to-from)/1e6:-1.0;};
    const auto so=standoffNs.load(),dn=drawNs.load(),fs=firstShotNs.load(),od=oppDrawNs.load();
    char b[2048];
    std::snprintf(b,sizeof b,"{\"route\":%s,\"installState\":\"%s\",\"lastError\":\"%s\",\"phase\":\"%s\",\"outcome\":\"%s\",\"state\":%d,\"result\":%d,\"duels\":%llu,\"stockDuels\":%llu,\"noSidearmDuels\":%llu,"
        "\"grabs\":%llu,\"drawInputsSent\":%llu,\"autoDrawBlocked\":%llu,\"deadEyeBlocked\":%llu,\"allowOverrides\":%llu,\"touchHidden\":%llu,\"promptsHidden\":%llu,\"handsCleared\":%llu,\"uiHides\":%llu,\"triggerEdges\":%llu,\"edgesForwarded\":%llu,"
        "\"shots\":%llu,\"directFires\":%llu,\"hits\":%llu,\"armHits\":%llu,\"hitsTaken\":%llu,\"lastZone\":%d,\"drawMs\":%.0f,\"oppDrawMs\":%.0f,\"firstShotMs\":%.0f,"
        "\"timer\":%.3f,\"timerLen\":%.3f,\"timeScale\":%.3f,\"data0\":%.3f,\"oppAlive\":%s,\"oppHealthFlags\":%u,\"oppState\":%d,\"lockFixes\":%llu,\"resultFixes\":%llu,\"ammoFills\":%llu,\"warpCancels\":%llu,"
        "\"targetWrites\":%llu,\"machineTicks\":%llu,\"levers\":{\"disarm\":%s,\"hideTouch\":%s,\"slowMo\":%s,\"fire\":%d}}",
        live()?"true":"false",installState.load(),lastError.load(),phase.load(),outcome.load(),engineState.load(),engineResult.load(),u(duels),u(stockDuels),u(noSidearmDuels),
        u(grabs),u(drawInputsSent),u(autoDrawBlocked),u(deadEyeBlocked),u(allowOverrides),u(touchHidden),u(promptsHidden),u(handsCleared),u(uiHides),u(edges),u(edgesForwarded),
        u(shots),u(directFires),u(hits),u(armHits),u(hitsTaken),lastZone.load(),ms(so,dn),ms(so,od),ms(dn,fs),
        double(timerValue.load()),double(timerLength.load()),double(scaleValue.load()),double(dataScaleValue.load()),oppAlive.load()?"true":"false",oppFlags.load(),oppState.load(),
        u(lockFixes),u(resultFixes),u(ammoFills),u(warpCancels),u(targetWrites),u(machineTicks),levers.disarm?"true":"false",levers.hideTouch?"true":"false",levers.slowMo?"true":"false",levers.fire);
    return b;
}
} // namespace rdrvr::vr::duelctl
