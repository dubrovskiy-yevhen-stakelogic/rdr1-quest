#pragma once
//   GOD MODE      OFF|ON   sagActorScript::SetActorInvulnerable(sagActor*,bool) 0x1cab2a4 (health+0x150 bit 0x4), re-asserted
//                          every ~0.25 s while ON; cleared on OFF only if the mod set it.
//                          NOT covered by the engine: drowning (clears the bit in HandleInjure),
//                          train impact and scripted kills (hlthHealthComponent::Kill).
//   HEAL          A        hlthHealthComponent::SetHitPoints(max,false,true,null) 0x1e83500 + sagPlayer::SetDeadEyePoints(max)
//   INF DEAD EYE  OFF|ON   sagActorScript::SetInfiniteDeadEye(0,bool) 0x1caf890 (player+0x3a1), re-asserted (ResetData clears it)
//   INF AMMO      OFF|ON   AmmoComponent::SetInifiniteFlag(type,bool) 0x2540594 for every ammo type (inventory+0x410);
//                          on OFF only the types that were 0 before are cleared
//   REFILL AMMO   A        AmmoComponent::SetAmmoAmount(type,max) 0x2540528 for every type (stock, not the loaded clip)
//   GIVE WEAPON   <name>   GiveEnumeratedWeapon(actor,enum,0.0,false,true) 0x267984c; list = in-order walk of
//                          WeaponEnumManager::sm_EnumWeaponEnumTree (names: GetEnumName 0x254ea64), redirected enums
//                          (obj+0x180) and TURRET/CANNON names skipped. Inventory change is SAVED with the game.
//   ALL WEAPONS   A        the same call for every listed weapon (4 per frame, no equip)
//   TIME          <hh:00>  sagTimeOfDayScript::SetTimeOfDay(tod) 0x2abaa10 (day 0 = next occurrence: time only goes forward)
//   FREEZE CLOCK  OFF|ON   sagTimeOfDayManager::SetAccelerationFactor(0,force) 0x2398b9c; the saved factor is restored on OFF
//   WEATHER       <state>  WeatherManager::ChangeWeather(state,0.0,false) 0x2aaa018 (instant); NOW = GetWeather() 0x2aaa1d4
//
// Threads: rows' callbacks run on the XR thread (menu): they only flip atomics or post VrMainThread commands. Every engine
// call runs on the engine main thread inside the sagShellDerived::UpdateInputs wrapper (VrMainThread module 2), after
// whole-function fingerprints of every called function and GOT checks of every global (install()). A row whose function
// did not verify shows UNAVAILABLE and never calls the engine. Kill switch: debug.rdrvr.vr.cheats 0 (no rows, no engine use).
#include "VrMenuPages.h"
#include "VrMainThread.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

namespace rdrvr::vr::cheats {
namespace mt=::rdrvr::vr::mainthread;

// ---- engine facts (librdr.so 1.58, vaddr; .text file offset = vaddr - 0x4000) ----
inline constexpr std::uintptr_t invulnerableOffset=0x1cab2a4,setHitPointsOffset=0x1e83500,scriptInfiniteDeadEyeOffset=0x1caf890,
    deadEyePointsOffset=0x2696bd0,ammoInfiniteOffset=0x2540594,ammoAmountOffset=0x2540528,giveWeaponOffset=0x267984c,
    weaponEnumNameOffset=0x254ea64,scriptTimeOfDayOffset=0x2abaa10,accelerationOffset=0x2398b9c,changeWeatherOffset=0x2aaa018,
    getWeatherOffset=0x2aaa1d4,timeWarpOffset=0x23997a0,cancelWarpOffset=0x23997f4,sprintStaminaOffset=0x2420498,
    horseFreshnessOffset=0x245070c;
// WEAPONS (user: "GIVE WEAPON lists only 7"; "a HOLSTERS menu: which revolver / rifle... Is in each holster"):
// byte [enum+0x180] is WeaponEnumBase::IsLocked (0x254b090), NOT a redirect: FindWeaponEnumOrCategory gives a random UNLOCKED weapon
// of the category for a locked enum. The old list skipped the 25 locked enums (story progress). Now every enum is listed; a give of a
// locked one first calls WeaponEnumBase::SetLocked(false) (0x254b098: also re-adds it to its category, like the game's own unlock).
// HOLSTERS: HasEnumeratedWeapon(actor,enum) 0x267a498 (owned), HasEnumeratedWeaponReady(actor,enum) 0x2679c88 (= the one in its
// holster: invInventoryComponent::HasNamedItemEquipped), ReadyEnumeratedWeapon(actor,enum) 0x26799b4 (EquipItem(name) = put it into
// its holster slot + SetNextUseSlotFromWeaponEnum) - the script natives the game uses for the same thing.
inline constexpr std::uintptr_t setLockedOffset=0x254b098,hasWeaponOffset=0x267a498,readyWeaponOffset=0x26799b4,hasReadyOffset=0x2679c88;
// SAVE FIX: the old ALL WEAPONS gave RIFLE_ANTITANK (mission weapon) and BOW_SHORTBOW; a save holding them never leaves the
// loading screen (user's autosave "New Friends, Old Problems": world sim runs, no scene ever renders). Such weapons are no longer
// listed, and whenever the player owns one it is removed with DeleteEnumeratedWeapon(actor,enum) 0x267a9c8 (invManager::DeleteItem).
inline constexpr std::uintptr_t deleteWeaponOffset=0x267a9c8;
// invInventoryComponent::DeleteItem 0x2661e68 refuses the item at inv+0x458 (the item being equipped; GiveEnumeratedWeapon also waits
// for it to be empty). After loading that save it is the bow (its assets never arrive = the endless loading screen), so for a
// non-player weapon there the slot is cleared first - the same store DeleteItem itself does at 0x2661ff4 once it gets that far.
inline constexpr std::size_t invPendingItem=0x458,itemTypeField=0x30,typeNameField=0x8;
inline constexpr std::size_t invNextUseSlot=0x450; // invInventoryComponent next-use slot (SetNextUseSlot 0x26637a0, -1 = none)
inline constexpr std::size_t enumLocked=0x180,enumObjectKey=0x8;
// GLOB_DAT slots -> symbols (llvm-readelf -r): all are checked before use.
inline constexpr std::uintptr_t gameDataGotOffset=0x2ddc550,gameDataOffset=0x4e75c4b8;            // GAMEDATA (u32 state, 3 = in game)
inline constexpr std::uintptr_t todInstanceGotOffset=0x2e014f8,todInstanceOffset=0x4e75cb30;      // sagTimeOfDayManager::sm_Instance (ptr)
inline constexpr std::uintptr_t weatherInstanceGotOffset=0x2e014e8,weatherInstanceOffset=0x4e8a5be8; // WeatherManager::sm_instance (ptr)
inline constexpr std::uintptr_t enumTreeGotOffset=0x2e047e0,enumTreeOffset=0x4e7b4e00;            // WeaponEnumManager::sm_EnumWeaponEnumTree (root ptr)
inline constexpr std::uintptr_t horseFlagGotOffset=0x2deb548,horseFlagOffset=0x4e72da10;          // hrsFreshnessControl::State::sm_bInfiniteFreshness
inline constexpr std::size_t playerActive=0x5f1,playerGuid=0x5f4,playerInfiniteDeadEye=0x3a1,playerDeadEyeMax=0x3c4,playerStates=0x410,
    statesDeadEye=0x20,actorHealth=0x60,actorInventory=0x70,actorFlags=0x118,actorComponent=0xb0,healthMax=0x1c,healthValue=0x20,
    healthFlags=0x150,inventoryAmmo=0x410,ammoElements=0x8,ammoCount=0x10,ammoStride=12,ammoMaxField=4,ammoInfiniteField=8,
    todAcceleration=0x8,todValue=0x5c,todWarpScale=0xc,todWarpDuration=0x28,todWarpActive=0x30,enumNodeObject=0,enumNodeKey=8,
    enumNodeParent=0x10,enumNodeLeft=0x18,enumNodeRight=0x20,enumRedirected=0x180,inventoryOwner=0x10;
inline constexpr std::uint32_t invulnerableBit=0x4;
// WEATHER. Rain/thunder are not driven by the forced weather TYPE but by the EFFECTIVE weather:
//   WeatherManager::ProcessWeather 0x2aaa768 copies the base weights (+0x5c, what ChangeWeather writes) to +0xb8 and then
//   rdrWeatherSphereManager::OverrideWeather 0x2ac011c (sagEnvironment+0x170) lets every active weather sphere replace them
//   (exclusion spheres map RAINY/STORMY -> their *IfExcluded weather, others inject their own weather at fade 1.0);
//   UpdateComputedConstants 0x2aaa964 then writes argmax(+0xb8) -> +0x54 (effective) and argmax(+0x5c) -> +0x58 (base).
//   CVfxWeather::Update 0x297fbe4 (g_vfxWeather) starts the GPU rain (+0x150c) and the wetness (+0x151c) only when +0x54 is
//   RAINY(3)/STORMY(4) and the script rain amount +0x1500 != 0; fork lightning + thunder (UpdateLightning 0x29806fc ->
//   CreateLightningSequence -> audWeatherAudioEntity::TriggerLightning) only when +0x54 == STORMY and +0x1504 != 0.
// So while a weather is FORCED the mod keeps the base weather (re-assert after a script change),
// switches the spheres off (EnableWeatherSphere's own flag, +0x5e; restored on GAME) and lifts a
// script rain/lightning amount of 0 to -1 (Reset default).
inline constexpr std::uintptr_t vfxWeatherGotOffset=0x2e014f0,vfxWeatherOffset=0x4e829580;           // g_vfxWeather (object)
inline constexpr std::uintptr_t environmentGotOffset=0x2e01610,environmentOffset=0x4e8a51a8;         // sagEnvironment::sm_instance (ptr)
inline constexpr std::uintptr_t noRainGotOffset=0x2e0e7e8,noRainOffset=0x4e829540;                   // PARAM_noRain (sysParam, +0x18 set)
inline constexpr std::uintptr_t updateComputedConstantsOffset=0x2aaa964,isLightningEnabledOffset=0x297fb80,vfxRenderOffset=0x2980f64,
    enableWeatherSphereOffset=0x2abf444,overrideWeatherOffset=0x2ac011c,vfxUpdateOffset=0x297fbe4;  // layout witnesses (never called)
inline constexpr std::size_t weatherEffective=0x54,weatherBase=0x58,weatherBaseWeights=0x5c,weatherWeightsEffective=0xb8,weatherTransition=0x230,
    vfxRainAmount=0x1500,vfxLightningAmount=0x1504,vfxRain=0x150c,vfxWetness=0x151c,vfxRainTarget=0x1520,vfxStrikes=0x9c,
    environmentSpheres=0x170,spheresFirst=0x10,spheresCount=0xfd0,sphereStride=0x60,sphereFade=0x50,sphereWeather=0x54,
    sphereExclusion=0x5c,sphereEnabled=0x5e,noRainValue=0x18;
inline constexpr std::array<std::size_t,4> vfxCloudSlots{0x1c4,0x2ec,0x414,0x53c};                  // cloud-lightning sequences
inline constexpr unsigned maxSpheres=42;                                                             // atFixedArray<rdrWeatherSphere,42>
inline constexpr int weatherRainy=3,weatherStormy=4;

inline constexpr std::array<unsigned char,92> invulnerableFingerprint{
#include "gamecode/VrCheats_1cab2a4_92_8d.inc"
};
// setHitPoints @0x1e83500: function size 468 bytes, fingerprint 468 bytes (whole)
inline constexpr std::array<unsigned char,468> setHitPointsFingerprint{
#include "gamecode/VrCheats_1e83500_468_8d.inc"
};
// scriptInfiniteDeadEye @0x1caf890: function size 120 bytes, fingerprint 120 bytes (whole)
inline constexpr std::array<unsigned char,120> scriptInfiniteDeadEyeFingerprint{
#include "gamecode/VrCheats_1caf890_120_8d.inc"
};
// deadEyePoints @0x2696bd0: function size 92 bytes, fingerprint 92 bytes (whole)
inline constexpr std::array<unsigned char,92> deadEyePointsFingerprint{
#include "gamecode/VrCheats_2696bd0_92_8d.inc"
};
// ammoInfinite @0x2540594: function size 24 bytes, fingerprint 24 bytes (whole)
inline constexpr std::array<unsigned char,24> ammoInfiniteFingerprint{
#include "gamecode/VrCheats_2540594_24_8d.inc"
};
// ammoAmount @0x2540528: function size 36 bytes, fingerprint 36 bytes (whole)
inline constexpr std::array<unsigned char,36> ammoAmountFingerprint{
#include "gamecode/VrCheats_2540528_36_8d.inc"
};
// giveWeapon @0x267984c: function size 224 bytes, fingerprint 224 bytes (whole)
inline constexpr std::array<unsigned char,224> giveWeaponFingerprint{
#include "gamecode/VrCheats_267984c_224_8d.inc"
};
// weaponEnumName @0x254ea64: function size 92 bytes, fingerprint 92 bytes (whole)
inline constexpr std::array<unsigned char,92> weaponEnumNameFingerprint{
#include "gamecode/VrCheats_254ea64_92_8d.inc"
};
// scriptTimeOfDay @0x2abaa10: function size 92 bytes, fingerprint 92 bytes (whole)
inline constexpr std::array<unsigned char,92> scriptTimeOfDayFingerprint{
#include "gamecode/VrCheats_2abaa10_92_8d.inc"
};
// acceleration @0x2398b9c: function size 116 bytes, fingerprint 116 bytes (whole)
inline constexpr std::array<unsigned char,116> accelerationFingerprint{
#include "gamecode/VrCheats_2398b9c_116_8d.inc"
};
// changeWeather @0x2aaa018: function size 444 bytes, fingerprint 444 bytes (whole)
inline constexpr std::array<unsigned char,444> changeWeatherFingerprint{
#include "gamecode/VrCheats_2aaa018_444_8d.inc"
};
// getWeather @0x2aaa1d4: function size 404 bytes, fingerprint 404 bytes (whole)
inline constexpr std::array<unsigned char,404> getWeatherFingerprint{
#include "gamecode/VrCheats_2aaa1d4_404_8d.inc"
};
// timeWarp @0x23997a0: function size 84 bytes, fingerprint 84 bytes (whole)
inline constexpr std::array<unsigned char,84> timeWarpFingerprint{
#include "gamecode/VrCheats_23997a0_84_8d.inc"
};
// cancelWarp @0x23997f4: function size 56 bytes, fingerprint 56 bytes (whole)
inline constexpr std::array<unsigned char,56> cancelWarpFingerprint{
#include "gamecode/VrCheats_23997f4_56_8d.inc"
};
// sprintStamina @0x2420498: function size 104 bytes, fingerprint 104 bytes (whole)
inline constexpr std::array<unsigned char,104> sprintStaminaFingerprint{
#include "gamecode/VrCheats_2420498_104_8d.inc"
};
// horseFreshness @0x245070c: function size 16 bytes, fingerprint 16 bytes (whole)
inline constexpr std::array<unsigned char,16> horseFreshnessFingerprint{
#include "gamecode/VrCheats_245070c_16_8d.inc"
};
// updateComputedConstants @0x2aaa964: function size 100 bytes, fingerprint 100 bytes (whole)
inline constexpr std::array<unsigned char,100> updateComputedConstantsFingerprint{
#include "gamecode/VrCheats_2aaa964_100_8d.inc"
};
// isLightningEnabled @0x297fb80: function size 44 bytes, fingerprint 44 bytes (whole)
inline constexpr std::array<unsigned char,44> isLightningEnabledFingerprint{
#include "gamecode/VrCheats_297fb80_44_8d.inc"
};
// vfxRender @0x2980f64: function size 24 bytes, fingerprint 24 bytes (whole)
inline constexpr std::array<unsigned char,24> vfxRenderFingerprint{
#include "gamecode/VrCheats_2980f64_24_8d.inc"
};
// enableWeatherSphere @0x2abf444: function size 48 bytes, fingerprint 48 bytes (whole)
inline constexpr std::array<unsigned char,48> enableWeatherSphereFingerprint{
#include "gamecode/VrCheats_2abf444_48_8d.inc"
};
// overrideWeather @0x2ac011c: function size 1140 bytes, fingerprint 1140 bytes (whole)
inline constexpr std::array<unsigned char,1140> overrideWeatherFingerprint{
#include "gamecode/VrCheats_2ac011c_1140_8d.inc"
};
// vfxUpdate @0x297fbe4: function size 1012 bytes, fingerprint 1012 bytes (whole)
inline constexpr std::array<unsigned char,1012> vfxUpdateFingerprint{
#include "gamecode/VrCheats_297fbe4_1012_8d.inc"
};

// setLocked @0x254b098: function size 108 bytes, fingerprint 108 bytes (whole)
// deleteWeapon @0x267a9c8: function size 132 bytes, fingerprint 132 bytes (whole)
inline constexpr std::array<unsigned char,132> deleteWeaponFingerprint{
#include "gamecode/VrCheats_267a9c8_132_8d.inc"
};
inline constexpr std::array<unsigned char,108> setLockedFingerprint{
#include "gamecode/VrCheats_254b098_108_8d.inc"
};
// hasWeapon @0x267a498: function size 124 bytes, fingerprint 124 bytes (whole)
inline constexpr std::array<unsigned char,124> hasWeaponFingerprint{
#include "gamecode/VrCheats_267a498_124_8d.inc"
};
// readyWeapon @0x26799b4: function size 224 bytes, fingerprint 224 bytes (whole)
inline constexpr std::array<unsigned char,224> readyWeaponFingerprint{
#include "gamecode/VrCheats_26799b4_224_8d.inc"
};
// hasReady @0x2679c88: function size 152 bytes, fingerprint 152 bytes (whole)
inline constexpr std::array<unsigned char,152> hasReadyFingerprint{
#include "gamecode/VrCheats_2679c88_152_8d.inc"
};

// ---- ABI (AAPCS64: integer/pointer args in x/w by order, floats in s by order) ----
using ReadFn=bool(*)(void*,std::uintptr_t,void*,std::size_t);
using WriteFn=bool(*)(std::uintptr_t,const void*,std::size_t);
// In-process store (same rule as VrReload.h processWrite); used only on addresses that were just read successfully.
inline bool processWrite(std::uintptr_t address,const void* data,std::size_t n) noexcept {
    if(!address)return false;std::memcpy(reinterpret_cast<void*>(address),data,n);return true;}
using InvulnerableFn=void(*)(void* actor,bool on);
using SetHitPointsFn=void(*)(void* health,float hp,bool,bool,const void* injure);
using ScriptInfiniteDeadEyeFn=void(*)(int player,bool on);
using DeadEyePointsFn=void(*)(void* player,float points,bool force);
using AmmoFlagFn=void(*)(void* ammo,int type,bool on);
using AmmoAmountFn=void(*)(void* ammo,int type,float amount);
using GiveWeaponFn=void(*)(void* actor,int weaponEnum,float bias,bool,bool equip);
using EnumNameFn=const char*(*)(int weaponEnum);
using SetLockedFn=void(*)(void* weaponEnum,bool locked);
using HasWeaponFn=bool(*)(const void* actor,int weaponEnum);
using ReadyWeaponFn=void(*)(void* actor,int weaponEnum);
using HasReadyFn=bool(*)(void* actor,int weaponEnum);
using DeleteWeaponFn=void(*)(void* actor,int weaponEnum);
using ScriptTimeOfDayFn=void(*)(std::uint32_t tod);
using AccelerationFn=void(*)(void* manager,float factor,bool force);
using ChangeWeatherFn=void(*)(void* manager,int state,float hours,bool force);
using GetWeatherFn=int(*)(const void* manager);
using TimeWarpFn=void(*)(void* manager,float scale,float rampIn,float rampOut,float duration);
using CancelWarpFn=void(*)(void* manager,bool smooth);
using SprintStaminaFn=void(*)(std::uint32_t guid,bool on);
using HorseFreshnessFn=void(*)(bool on);
struct Functions {
    InvulnerableFn invulnerable{};SetHitPointsFn setHitPoints{};ScriptInfiniteDeadEyeFn infiniteDeadEye{};DeadEyePointsFn deadEyePoints{};
    AmmoFlagFn ammoFlag{};AmmoAmountFn ammoAmount{};GiveWeaponFn giveWeapon{};EnumNameFn enumName{};ScriptTimeOfDayFn timeOfDay{};
    AccelerationFn acceleration{};ChangeWeatherFn changeWeather{};GetWeatherFn getWeather{};TimeWarpFn timeWarp{};CancelWarpFn cancelWarp{};
    SprintStaminaFn sprintStamina{};HorseFreshnessFn horseFreshness{};
    SetLockedFn setLocked{};HasWeaponFn hasWeapon{};ReadyWeaponFn readyWeapon{};HasReadyFn hasReady{};DeleteWeaponFn deleteWeapon{};
};
// Addresses are absolute (engineBase applied). A zero global / null function = that part did not verify.
struct EngineApi {
    ReadFn read{};
    std::uintptr_t gameData{},playerManager{},playerManagerTable{},playerTable{},actorManagerGlobal{},actorSlotsGlobal{};
    std::uintptr_t todInstance{},weatherInstance{},enumTree{};
    std::uintptr_t inventoryTable{};   // invInventoryComponent vtable address point (vtable 0x2d8c058 + 0x10), as HOLSTERS checks it
    // WEATHER layout (each zero/false unless its GOT word AND its layout witnesses verified):
    bool weatherFields{};              // WeatherManager +0x54/+0x58/+0xb8 (UpdateComputedConstants print)
    std::uintptr_t vfxWeather{};       // g_vfxWeather object (CVfxWeather::Update / IsLightningEnabled / Render prints)
    std::uintptr_t environment{};      // sagEnvironment::sm_instance global (EnableWeatherSphere / OverrideWeather prints)
    std::uintptr_t noRainParam{};      // PARAM_noRain object (report only)
    WriteFn write{&processWrite};
    Functions fn{};
};

// ---- rows ----
enum Row : unsigned {RowGod,RowHeal,RowDeadEye,RowAmmo,RowRefill,RowGive,RowAll,RowWeapons,RowTime,RowFreeze,RowWeather,RowSlow,RowStamina,RowCount};
inline constexpr std::array<const char*,RowCount> rowLabels{"GOD MODE","HEAL","INF DEAD EYE","INF AMMO","REFILL AMMO","GIVE WEAPON",
    "ALL WEAPONS","WEAPONS BY TYPE","TIME","FREEZE CLOCK","WEATHER","SLOW MOTION","INF STAMINA"};
inline constexpr std::array<const char*,RowCount> rowKeys{"god","heal","deadEye","infAmmo","refillAmmo","giveWeapon","allWeapons","weaponsByType","time",
    "freezeClock","weather","slowMotion","infStamina"};
// weapon groups (first word of the enum name, e.g. WEAPON_REVOLVER_CATTLEMAN) = the holster each one goes to.
enum Group : std::uint8_t {GroupSidearm,GroupRepeater,GroupRifle,GroupShotgun,GroupThrown,GroupMelee,GroupOther,GroupCount};
inline constexpr std::array<const char*,GroupCount> groupNames{"REVOLVERS + PISTOLS","REPEATERS","RIFLES","SHOTGUNS","THROWN","MELEE","OTHER"};
inline constexpr std::array<const char*,GroupCount> holsterLabels{"REVOLVER","REPEATER","RIFLE","SHOTGUN","THROWN","MELEE","OTHER"};
inline constexpr unsigned holsterRows=6; // the groups with a holster (OTHER: bow / lasso / ... stay in the GIVE lists only)
enum Toggle : unsigned {ToggleGod,ToggleDeadEye,ToggleAmmo,ToggleFreeze,ToggleStamina,ToggleCount};
enum Op : std::uint16_t {OpHeal=1,OpRefill=2,OpGive=3,OpAll=4,OpTime=5,OpWeather=6,OpSlow=7,OpReady=8};
inline constexpr std::array<const char*,23> weatherNames{"CLEAR","FAIR","CLOUDY","RAINY","STORMY","SNOWY","IN CLEAR","IN FAIR",
    "IN CLOUDY","IN RAINY","IN STORMY","IN SNOWY","CAVE","THIEVES","FOREST","LOC A","LOC B","LOC C","IN THIEVES","IN FOREST",
    "IN LOC A","IN LOC B","IN LOC C"};
// The menu forces only the outdoor base states 0..5; choice 6 (GAME) gives the weather back to the game.
inline constexpr unsigned weatherChoices=7,weatherGameChoice=6;
inline constexpr std::array<const char*,weatherChoices> weatherPickNames{"CLEAR","FAIR","CLOUDY","RAINY","STORMY","SNOWY","GAME"};
inline constexpr std::array<float,3> slowScales{1.f,.5f,.25f};
inline constexpr std::array<const char*,3> slowNames{"OFF","50%","25%"};
inline constexpr float warpRamp=10.f,warpDuration=100000.f;
inline constexpr unsigned maxWeapons=160,weaponNameChars=24,maintainEveryFrames=15,allPerFrame=4,maxAmmoTypes=64;

// ---- shared state (XR thread writes the wishes, the main thread everything else) ----
inline std::atomic<bool> engineReady{},installed{};
inline std::atomic<const char*> installState{"NOT_CONFIGURED"};
inline std::array<std::atomic<bool>,RowCount> rowAvailable{};
inline std::array<std::atomic<bool>,ToggleCount> want{};
inline std::array<std::atomic<bool>,ToggleCount> active{};                // what the engine state is (main thread view)
inline std::array<std::atomic<const char*>,RowCount> rowNote{};          // static text in panel glyphs, nullptr = none
inline std::atomic<unsigned> weaponPick{0},timePick{12},weatherPick{0},slowPick{1};
inline std::atomic<int> nowTimeMinutes{-1},nowWeather{-1},nowSlow{-1};    // -1 unknown; nowSlow: 0 off, 1/2 the mod's, 3 another warp
// Weather diagnostics (main thread writes, menu/report read). nowWeather = effective (+0x54 / GetWeather), nowBaseWeather =
// what ChangeWeather/scripts set (+0x58); they differ while a weather sphere overrides the region.
inline std::atomic<int> weatherForced{-1},nowBaseWeather{-1},spheresTotal{-1},spheresEnabled{-1},spheresActive{-1},sphereTopWeather{-1},
    sphereTopExclusion{-1},noRain{-1};
inline std::atomic<float> nowRain{-1.f},nowRainTarget{-1.f},nowWetness{-1.f},rainAmount{-2.f},lightningAmount{-2.f},stormyWeight{-1.f},
    rainyWeight{-1.f},sphereTopFade{-1.f};
inline std::atomic<std::uint64_t> weatherReasserts{},sphereWrites{},sphereRestores{},amountWrites{},lightningEvents{},cloudLightningEvents{},
    lightningSamples{};
inline std::atomic<std::uint32_t> weaponCount{0};
// rebuilt every ~2 s in game (locks / owned / holstered change) into the back buffer, then published with ONE atomic
// (count | buffer << 16), so the XR thread never reads a half-written list.
struct WeaponEntry {std::int32_t id{};char name[weaponNameChars+1]{};std::uintptr_t object{};std::uint8_t group{GroupOther};bool locked{},owned{},ready{};};
inline std::array<std::array<WeaponEntry,maxWeapons>,2> weaponBuffers{};
inline std::atomic<std::uint32_t> weaponPublished{0};
struct WeaponView {const WeaponEntry* list;unsigned n;const WeaponEntry& operator[](unsigned i) const noexcept {return list[i<maxWeapons?i:0];}};
inline WeaponView weaponView() noexcept {const auto v=weaponPublished.load(std::memory_order_acquire);
    return {weaponBuffers[(v>>16)&1u].data(),std::min<unsigned>(v&0xffffu,maxWeapons)};}
inline std::array<std::atomic<std::uint64_t>,GroupCount> groupGives{};
inline std::atomic<std::uint64_t> unlocks{},readies{},listPublishes{},scrubbed{},scrubFound{};
inline constexpr unsigned maxBad=16;
inline std::atomic<std::uint64_t> pendingCleared{},nextUseRestores{},nextUseCleared{};inline std::atomic<const char*> pendingName{""};
inline std::array<char,48> pendingText{};
inline std::array<std::int32_t,maxBad> badIds{};inline std::atomic<unsigned> badCount{0}; // main thread only (count read by the report)
inline std::atomic<std::uint64_t> commands{},executed{},refused{},engineCalls{},reasserts{},listBuilds{},listNodes{},listSkipped{},readFailures{},exceptions{};
inline std::atomic<std::uint64_t> wakeups{};                              // XR thread: maintain soon
inline std::atomic<const char*> lastResult{"NONE"};

namespace detail {
inline EngineApi api{};
template<class T> inline bool rd(std::uintptr_t p,std::size_t o,T& out) noexcept {
    if(!p||!api.read)return false;
    if(!api.read(nullptr,p+o,&out,sizeof out)){readFailures.fetch_add(1,std::memory_order_relaxed);return false;}
    return true;
}
struct Player {int gameState{-1};std::uintptr_t player{},actor{},health{},inventory{};std::uint32_t guid{};float hp{},hpMax{};};
// Teleport.h samplePlayer rules (tables, active flag, actor registry generation) + health/inventory components.
inline const char* sample(Player& p) noexcept {
    std::uint32_t gs{};if(!rd(api.gameData,0,gs))return "NO GAME DATA";
    p.gameState=int(gs);if(gs!=3)return "NOT IN GAME";
    std::uintptr_t table{},manager{},slots{};std::uint16_t count{},generation{};std::uint8_t on{};
    if(!rd(api.playerManager,0,table)||table!=api.playerManagerTable||!rd(api.playerManager,0xa20,p.player)||!p.player||
       !rd(p.player,0,table)||table!=api.playerTable||!rd(p.player,playerActive,on)||on!=1||!rd(p.player,playerGuid,p.guid))return "NO PLAYER";
    if(!rd(api.actorManagerGlobal,0,manager)||!rd(api.actorSlotsGlobal,0,slots)||!slots||slots%8||!rd(manager,0,count))return "NO PLAYER";
    const std::uintptr_t index=p.guid&0xffffu;
    if(index>=count||!rd(slots,index*16,p.actor)||!p.actor||!rd(slots,index*16+8,generation)||generation!=(p.guid>>16))return "NO PLAYER";
    std::uint32_t flags{};std::uintptr_t component{};
    if(!rd(p.actor,actorFlags,flags)||(flags&3u)!=3u||!rd(p.actor,actorComponent,component)||!component)return "NO PLAYER";
    if(!rd(p.actor,actorHealth,p.health)||!p.health||!rd(p.health,healthValue,p.hp)||!rd(p.health,healthMax,p.hpMax))return "NO HEALTH";
    // The ammo setters write [[comp+8]+i*12] unchecked - only a verified inventory component of THIS actor is used
    // (vtable address point + owner at +0x10, same rule as VrHolsters); anything else (death/respawn/loading) = no inventory.
    std::uintptr_t vt{},invOwner{};
    if(!rd(p.actor,actorInventory,p.inventory)||!p.inventory||!api.inventoryTable||!rd(p.inventory,0,vt)||vt!=api.inventoryTable||
       !rd(p.inventory,inventoryOwner,invOwner)||invOwner!=p.actor)p.inventory=0;
    return nullptr;
}
inline bool deadEyeRunning(const Player& p) noexcept {
    std::uintptr_t states{};std::uint32_t eye{};return rd(p.player,playerStates,states)&&states&&rd(states,statesDeadEye,eye)&&eye!=0;
}
inline std::uintptr_t singleton(std::uintptr_t global) noexcept {std::uintptr_t v{};return rd(global,0,v)?v:0;}
// Main-thread bookkeeping of what the mod changed (undo only its own changes).
struct Owned {
    std::uintptr_t godActor{};bool godOwned{};
    bool deadEyeOwned{};
    std::uintptr_t ammoComponent{};std::array<bool,maxAmmoTypes> ammoOwned{};
    bool freezeOwned{};float savedAcceleration{};
    bool staminaOwned{};std::uint32_t staminaGuid{};
    unsigned allNext{};bool allActive{};unsigned allGroup{GroupCount};unsigned allGiven{};
    std::uint64_t frame{},nextList{};
};
inline Owned owned{};
inline void note(Row r,const char* text) noexcept {rowNote[r].store(text,std::memory_order_release);}
inline void result(Row r,const char* text,bool ok) noexcept {
    note(r,ok?nullptr:text);lastResult.store(text,std::memory_order_relaxed);
    ::rdrvr::vr::cheatNote.store(text,std::memory_order_release);
    (ok?executed:refused).fetch_add(1,std::memory_order_relaxed);
}
inline bool fieldEquals(const char* name,const char* needle) noexcept {return std::strstr(name,needle)!=nullptr;}
// Display form: upper case, '_' -> ' ', "WEAPON " prefix dropped, panel glyphs only, at most weaponNameChars.
inline void displayName(const char* raw,char* out) noexcept {
    char tmp[64]{};std::size_t n=0;
    for(;raw[n]&&n+1<sizeof tmp;++n){char c=raw[n];if(c>='a'&&c<='z')c=char(c-'a'+'A');if(c=='_')c=' ';tmp[n]=c;}
    const char* s=tmp;if(!std::strncmp(s,"WEAPON ",7))s+=7;
    std::snprintf(out,weaponNameChars+1,"%s",s);::rdrvr::vr::panelSanitize(out);
}
// In-order walk of the weapon enum tree (guarded reads, bounded), names through the engine's own GetEnumName.
// No explicit stack - rage::atBinTreeBase::Insert (0x254e588) is a plain unbalanced BST (no rotations), so the depth
// depends on AddWeaponEnum order and may exceed any fixed stack. Walk by the parent link the engine keeps at node+0x10
// (0x254e680 / 0x254e6f4; root parent = 0): successor = leftmost of the right subtree, else climb while coming from a right
// child. Read-only (no Morris threading on the engine's tree). A read failure ends the walk; what was collected is published.
inline bool skipName(const char* display) noexcept {
    for(const char* bad:{"TURRET","CANNON","GATLING","ANTITANK","NULL","BOW"})if(std::strstr(display,bad))return true;
    return !*display;
}
inline Group groupOf(const char* display) noexcept {
    const auto starts=[&](const char* w){return std::strncmp(display,w,std::strlen(w))==0;};
    if(starts("REVOLVER")||starts("PISTOL"))return GroupSidearm;
    if(starts("REPEATER"))return GroupRepeater;
    if(starts("RIFLE")||starts("SNIPER"))return GroupRifle;
    if(starts("SHOTGUN"))return GroupShotgun;
    if(starts("THROWN")||starts("THROWING")||starts("EXPLOSIVE")||starts("DYNAMITE")||starts("MOLOTOV"))return GroupThrown;
    if(starts("MELEE")||starts("KNIFE"))return GroupMelee;
    if(std::strstr(display,"SHOTGUN"))return GroupShotgun;
    if(std::strstr(display,"REPEATER"))return GroupRepeater;
    if(std::strstr(display,"REVOLVER")||std::strstr(display,"PISTOL"))return GroupSidearm;
    return GroupOther;
}
// every ~2 s in game: all enums (locked ones too), plus owned / holstered for the HOLSTERS page (actor = 0: not alive).
inline void buildWeaponList(std::uintptr_t actor=0) noexcept {
    if(!api.fn.enumName||!api.enumTree)return;
    listBuilds.fetch_add(1,std::memory_order_relaxed);
    const unsigned back=((weaponPublished.load(std::memory_order_relaxed)>>16)&1u)^1u;auto& out=weaponBuffers[back];
    constexpr unsigned maxSteps=1024;   // per descent/climb and for visited nodes (the tree holds ~100 enums)
    const auto leftmost=[](std::uintptr_t& x)noexcept{
        for(unsigned g=0;g<maxSteps;++g){std::uintptr_t l{};if(!rd(x,enumNodeLeft,l))return false;if(!l)return true;x=l;}
        return false;};
    std::uintptr_t node{};if(!rd(api.enumTree,0,node)||!node||!leftmost(node))return;
    unsigned visited=0,n=0,skipped=0,bad=0;
    while(node&&visited<maxSteps){
        ++visited;
        std::uintptr_t object{};std::int32_t key{};std::uint8_t locked{0};
        if(rd(node,enumNodeObject,object)&&object&&rd(node,enumNodeKey,key)&&key>=0&&rd(object,enumLocked,locked)&&n<maxWeapons){
            const char* name=api.fn.enumName(key);char raw[48]{};
            const auto p=reinterpret_cast<std::uintptr_t>(name);
            bool ok=p&&(api.read(nullptr,p,raw,sizeof raw-1)||api.read(nullptr,p,raw,16));raw[sizeof raw-1]=0;
            WeaponEntry e{};if(ok)displayName(raw,e.name);
            if(ok&&!skipName(e.name)){
                e.id=key;e.object=object;e.locked=locked!=0;e.group=groupOf(e.name);
                if(actor&&api.fn.hasWeapon){e.owned=api.fn.hasWeapon(reinterpret_cast<const void*>(actor),key);
                    if(e.owned&&api.fn.hasReady)e.ready=api.fn.hasReady(reinterpret_cast<void*>(actor),key);}
                out[n++]=e;
            }else{++skipped;if(ok&&*e.name&&!std::strstr(e.name,"NULL")&&bad<maxBad)badIds[bad++]=key;} // Never in an inventory
        }else ++skipped;
        std::uintptr_t right{};if(!rd(node,enumNodeRight,right))break;
        if(right){node=right;if(!leftmost(node))break;continue;}
        std::uintptr_t child=node;node=0;bool ok=false;
        for(unsigned g=0;g<maxSteps;++g){
            std::uintptr_t parent{},parentRight{};
            if(!rd(child,enumNodeParent,parent))break;
            if(!parent){ok=true;break;}                                   // climbed past the root: walk complete
            if(!rd(parent,enumNodeRight,parentRight))break;
            if(parentRight!=child){node=parent;ok=true;break;}            // came from the left: parent is next
            child=parent;
        }
        if(!ok)break;
    }
    listNodes.store(visited,std::memory_order_relaxed);listSkipped.store(skipped,std::memory_order_relaxed);badCount.store(bad,std::memory_order_relaxed);
    // SAVE FIX: remove a non-player weapon the old ALL WEAPONS gave (it blocks the next save load)
    if(actor&&api.fn.hasWeapon&&api.fn.deleteWeapon)for(unsigned i=0;i<bad;++i)
        if(api.fn.hasWeapon(reinterpret_cast<const void*>(actor),badIds[i])){scrubFound.fetch_add(1,std::memory_order_relaxed);
            api.fn.deleteWeapon(reinterpret_cast<void*>(actor),badIds[i]);engineCalls.fetch_add(1);
            if(!api.fn.hasWeapon(reinterpret_cast<const void*>(actor),badIds[i]))scrubbed.fetch_add(1,std::memory_order_relaxed);}
    if(n){weaponPublished.store(n|(back<<16),std::memory_order_release);weaponCount.store(n,std::memory_order_release);listPublishes.fetch_add(1,std::memory_order_relaxed);}
}
// SAVE FIX: the item being equipped, when it is a non-player weapon (name checked like the list), is dropped from inv+0x458.
inline void clearBadPending(std::uintptr_t inventory) noexcept {
    if(!inventory)return;
    std::uintptr_t item{},type{},namePtr{};char raw[48]{};
    if(!rd(inventory,invPendingItem,item)||!item){pendingName.store("",std::memory_order_relaxed);return;}
    if(!rd(item,itemTypeField,type)||!type||!rd(type,typeNameField,namePtr)||!namePtr)return;
    if(!api.read(nullptr,namePtr,raw,31)&&!api.read(nullptr,namePtr,raw,15))return;raw[47]=0;
    char upper[48]{};for(unsigned i=0;i<47&&raw[i];++i){char c=raw[i];upper[i]=c>='a'&&c<='z'?char(c-'a'+'A'):c;}
    std::snprintf(pendingText.data(),pendingText.size(),"%s",upper);pendingName.store(pendingText.data(),std::memory_order_relaxed);
    bool bad=false;for(const char* b:{"ANTITANK","CANNON","TURRET","GATLING","BOW_"})bad=bad||std::strstr(upper,b)!=nullptr;
    if(!bad)return;
    const std::uintptr_t zero=0;std::uintptr_t again{};
    if(rd(inventory,invPendingItem,again)&&again==item&&api.write&&api.write(inventory+invPendingItem,&zero,sizeof zero))
        pendingCleared.fetch_add(1,std::memory_order_relaxed);
}
// the enum object of entry e, re-checked (key at +8) before an engine call that writes it.
inline bool enumObjectOk(const WeaponEntry& e) noexcept {std::int32_t key{-1};return e.object&&rd(e.object,enumObjectKey,key)&&key==e.id;}
inline void unlockIfLocked(const WeaponEntry& e) noexcept {
    std::uint8_t locked{};if(!api.fn.setLocked||!enumObjectOk(e)||!rd(e.object,enumLocked,locked)||!locked)return;
    api.fn.setLocked(reinterpret_cast<void*>(e.object),false);unlocks.fetch_add(1,std::memory_order_relaxed);engineCalls.fetch_add(1);
}
inline void giveOne(const Player& p,const WeaponEntry& e,bool equip) noexcept {
    unlockIfLocked(e);api.fn.giveWeapon(reinterpret_cast<void*>(p.actor),e.id,0.f,false,equip);engineCalls.fetch_add(1);
}
// ---- sticky toggles: apply / re-assert / undo ----
inline void maintainGod(const Player& p,bool ok) noexcept {
    const bool wantOn=want[ToggleGod].load(std::memory_order_acquire);
    if(!api.fn.invulnerable){active[ToggleGod].store(false);return;}
    if(!ok){if(wantOn)note(RowGod,"WAITING");return;}
    std::uint32_t flags{};if(!rd(p.health,healthFlags,flags))return;
    const bool bit=(flags&invulnerableBit)!=0;
    if(owned.godActor!=p.actor){owned.godActor=p.actor;owned.godOwned=false;}
    // A bit the game set itself (cutscenes, missions) is never cleared: it is "owned" only after the mod set it.
    if(wantOn&&!bit){api.fn.invulnerable(reinterpret_cast<void*>(p.actor),true);owned.godOwned=true;reasserts.fetch_add(1);engineCalls.fetch_add(1);}
    else if(!wantOn&&bit&&owned.godOwned){api.fn.invulnerable(reinterpret_cast<void*>(p.actor),false);owned.godOwned=false;engineCalls.fetch_add(1);}
    std::uint32_t after{};rd(p.health,healthFlags,after);
    active[ToggleGod].store((after&invulnerableBit)!=0);note(RowGod,nullptr);
}
inline void maintainDeadEye(const Player& p,bool ok) noexcept {
    const bool wantOn=want[ToggleDeadEye].load(std::memory_order_acquire);
    if(!api.fn.infiniteDeadEye)return;
    if(!ok){if(wantOn)note(RowDeadEye,"WAITING");return;}
    std::uint8_t flag{};if(!rd(p.player,playerInfiniteDeadEye,flag))return;
    if(wantOn&&!flag){api.fn.infiniteDeadEye(0,true);owned.deadEyeOwned=true;reasserts.fetch_add(1);engineCalls.fetch_add(1);}
    else if(!wantOn&&flag&&owned.deadEyeOwned){api.fn.infiniteDeadEye(0,false);owned.deadEyeOwned=false;engineCalls.fetch_add(1);}
    std::uint8_t after{};rd(p.player,playerInfiniteDeadEye,after);active[ToggleDeadEye].store(after!=0);note(RowDeadEye,nullptr);
}
inline bool ammoComponent(const Player& p,std::uintptr_t& component,unsigned& count,std::uintptr_t& elements) noexcept {
    std::uint16_t n{};
    if(!p.inventory)return false;component=p.inventory+inventoryAmmo;
    if(!rd(component,ammoElements,elements)||!elements||!rd(component,ammoCount,n)||!n)return false;
    count=std::min<unsigned>(n,maxAmmoTypes);return true;
}
inline void maintainAmmo(const Player& p,bool ok) noexcept {
    const bool wantOn=want[ToggleAmmo].load(std::memory_order_acquire);
    if(!api.fn.ammoFlag)return;
    if(!wantOn&&std::none_of(owned.ammoOwned.begin(),owned.ammoOwned.end(),[](bool b){return b;})){active[ToggleAmmo].store(false);return;} // idle: no reads
    std::uintptr_t component{},elements{};unsigned count{};
    if(!ok||!ammoComponent(p,component,count,elements)){if(wantOn)note(RowAmmo,"WAITING");return;}
    if(owned.ammoComponent!=component){owned.ammoComponent=component;owned.ammoOwned={};}
    bool all=true;
    for(unsigned i=0;i<count;++i){
        std::uint8_t flag{};if(!rd(elements,i*ammoStride+ammoInfiniteField,flag)){all=false;continue;}
        if(wantOn&&!flag){api.fn.ammoFlag(reinterpret_cast<void*>(component),int(i),true);owned.ammoOwned[i]=true;engineCalls.fetch_add(1);}
        else if(!wantOn&&flag&&owned.ammoOwned[i]){api.fn.ammoFlag(reinterpret_cast<void*>(component),int(i),false);owned.ammoOwned[i]=false;engineCalls.fetch_add(1);}
        all=all&&(wantOn?true:!owned.ammoOwned[i]);
    }
    active[ToggleAmmo].store(wantOn&&all);note(RowAmmo,nullptr);
}
inline void maintainFreeze(bool inGame) noexcept {
    const bool wantOn=want[ToggleFreeze].load(std::memory_order_acquire);
    if(!api.fn.acceleration||!api.todInstance)return;
    const auto manager=singleton(api.todInstance);float factor{};
    if(!inGame||!manager||!rd(manager,todAcceleration,factor)||!std::isfinite(factor)){if(wantOn)note(RowFreeze,"WAITING");return;}
    if(wantOn&&factor!=0.f){owned.savedAcceleration=factor;owned.freezeOwned=true;api.fn.acceleration(reinterpret_cast<void*>(manager),0.f,true);
        reasserts.fetch_add(1);engineCalls.fetch_add(1);}
    else if(!wantOn&&owned.freezeOwned){
        if(factor==0.f&&owned.savedAcceleration>0.f){api.fn.acceleration(reinterpret_cast<void*>(manager),owned.savedAcceleration,true);engineCalls.fetch_add(1);}
        owned.freezeOwned=false;
    }
    float after{};rd(manager,todAcceleration,after);active[ToggleFreeze].store(wantOn&&after==0.f);note(RowFreeze,nullptr);
}
inline void maintainStamina(const Player& p,bool ok) noexcept {
    const bool wantOn=want[ToggleStamina].load(std::memory_order_acquire);
    if(!api.fn.sprintStamina&&!api.fn.horseFreshness)return;
    if(!ok){if(wantOn)note(RowStamina,"WAITING");return;}
    if(wantOn){ // cheap writes, re-asserted every maintain pass (scripts may write the horse flag back)
        if(api.fn.sprintStamina)api.fn.sprintStamina(p.guid,true);
        if(api.fn.horseFreshness)api.fn.horseFreshness(true);
        owned.staminaOwned=true;owned.staminaGuid=p.guid;engineCalls.fetch_add(1);
    }else if(owned.staminaOwned){
        if(api.fn.sprintStamina)api.fn.sprintStamina(owned.staminaGuid,false);
        if(api.fn.horseFreshness)api.fn.horseFreshness(false);
        owned.staminaOwned=false;engineCalls.fetch_add(1);
    }
    active[ToggleStamina].store(wantOn);note(RowStamina,nullptr);
}
// ---- WEATHER: forced weather that really rains ----
struct WeatherOwned {
    int state{-1};                              // forced weather 0..5, -1 = the game's
    std::uintptr_t environment{};               // the sagEnvironment the sphere marks belong to
    std::array<bool,maxSpheres> sphere{};       // spheres the mod switched off (restored on GAME)
    bool rainAmount{},lightningAmount{};float savedRain{},savedLightning{};
    std::int32_t lastStrikes{};std::array<std::int32_t,vfxCloudSlots.size()> lastCloud{};bool sampled{};
};
inline WeatherOwned weatherOwned{};
template<class T> inline bool wr(std::uintptr_t p,std::size_t o,const T& v) noexcept {return p&&api.write&&api.write(p+o,&v,sizeof v);}
inline std::uintptr_t sphereManager(std::uintptr_t& environment) noexcept {
    environment=api.environment?singleton(api.environment):0;return environment?environment+environmentSpheres:0;}
inline unsigned sphereCount(std::uintptr_t manager) noexcept {std::int32_t n{};return rd(manager,spheresCount,n)&&n>0&&n<=int(maxSpheres)?unsigned(n):0u;}
inline std::uintptr_t sphereAt(std::uintptr_t manager,unsigned i) noexcept {return manager+spheresFirst+std::uintptr_t(i)*sphereStride;}
// Base weights +0x5c hold exactly the forced state (ChangeWeather(state,0) writes a one-hot set and clears the transition byte
// +0x230); a script SetWeather/SetWeatherComplex changes them (or starts a transition) and is answered by a re-assert.
inline bool baseHolds(std::uintptr_t manager,int state) noexcept {
    float weight{};std::uint8_t transition{1};
    return rd(manager,weatherBaseWeights+std::size_t(state)*4,weight)&&rd(manager,weatherTransition,transition)&&weight>=.999f&&!transition;
}
inline void releaseWeather() noexcept {
    auto& o=weatherOwned;std::uintptr_t environment{};
    if(const auto manager=sphereManager(environment);manager&&environment==o.environment){
        const unsigned n=sphereCount(manager);
        for(unsigned i=0;i<n;++i)if(o.sphere[i]){std::uint8_t on{1};const auto s=sphereAt(manager,i);
            if(rd(s,sphereEnabled,on)&&!on&&wr(s,sphereEnabled,std::uint8_t{1}))sphereRestores.fetch_add(1,std::memory_order_relaxed);}
    }
    if(api.vfxWeather){float v{};   // only a value still at the mod's -1 goes back (a script may have written its own since)
        if(o.rainAmount&&rd(api.vfxWeather,vfxRainAmount,v)&&v==-1.f)wr(api.vfxWeather,vfxRainAmount,o.savedRain);
        if(o.lightningAmount&&rd(api.vfxWeather,vfxLightningAmount,v)&&v==-1.f)wr(api.vfxWeather,vfxLightningAmount,o.savedLightning);
    }
    o.sphere={};o.environment=0;o.rainAmount=o.lightningAmount=false;o.state=-1;weatherForced.store(-1,std::memory_order_relaxed);
}
// Main thread, every maintain pass while a weather is forced (and right after the menu command).
inline void maintainWeather(bool inGame) noexcept {
    auto& o=weatherOwned;
    if(o.state<0||!inGame||!api.fn.changeWeather)return;  // loading/menus: kept, re-applied once in game
    const auto manager=singleton(api.weatherInstance);if(!manager)return;
    if(!baseHolds(manager,o.state)){api.fn.changeWeather(reinterpret_cast<void*>(manager),o.state,0.f,false);
        weatherReasserts.fetch_add(1,std::memory_order_relaxed);reasserts.fetch_add(1,std::memory_order_relaxed);engineCalls.fetch_add(1,std::memory_order_relaxed);}
    std::uintptr_t environment{};
    if(const auto spheres=sphereManager(environment)){
        if(o.environment!=environment){o.sphere={};o.environment=environment;}
        const unsigned n=sphereCount(spheres);
        for(unsigned i=0;i<n;++i){const auto s=sphereAt(spheres,i);std::uint8_t on{};
            // Same flag as sagWeatherScript::EnableWeatherSphere (script index k = sphere k+8); the fade (+0x50, rewritten by
            // OverrideWeather every frame) is zeroed so the forced weather is effective on the next ProcessWeather.
            if(rd(s,sphereEnabled,on)&&on&&wr(s,sphereEnabled,std::uint8_t{0})){o.sphere[i]=true;wr(s,sphereFade,0.f);
                sphereWrites.fetch_add(1,std::memory_order_relaxed);}}
    }
    if(api.vfxWeather&&(o.state==weatherRainy||o.state==weatherStormy)){
        float v{};   // a script SetRainAmount(0)/SetLightningAmount(0) blocks rain/strikes; -1 = CVfxWeather::Reset default ("auto")
        if(rd(api.vfxWeather,vfxRainAmount,v)&&v==0.f){if(!o.rainAmount){o.savedRain=v;o.rainAmount=true;}
            wr(api.vfxWeather,vfxRainAmount,-1.f);amountWrites.fetch_add(1,std::memory_order_relaxed);}
        if(o.state==weatherStormy&&rd(api.vfxWeather,vfxLightningAmount,v)&&v==0.f){if(!o.lightningAmount){o.savedLightning=v;o.lightningAmount=true;}
            wr(api.vfxWeather,vfxLightningAmount,-1.f);amountWrites.fetch_add(1,std::memory_order_relaxed);}
    }
}
// Lightning: rising edges of CVfxWeather +0x9c (strikes left in a fork-lightning sequence; every new sequence also triggers the
// thunder in CreateLightningSequence) and of the four cloud-lightning slots. Every frame while it storms, else per maintain pass.
inline void sampleLightning() noexcept {
    if(!api.vfxWeather)return;auto& o=weatherOwned;lightningSamples.fetch_add(1,std::memory_order_relaxed);
    std::int32_t strikes{};
    if(rd(api.vfxWeather,vfxStrikes,strikes)){if(o.sampled&&strikes>0&&o.lastStrikes<=0)lightningEvents.fetch_add(1,std::memory_order_relaxed);o.lastStrikes=strikes;}
    for(std::size_t k=0;k<vfxCloudSlots.size();++k){std::int32_t on{};
        if(rd(api.vfxWeather,vfxCloudSlots[k],on)){if(o.sampled&&on&&!o.lastCloud[k])cloudLightningEvents.fetch_add(1,std::memory_order_relaxed);o.lastCloud[k]=on;}}
    o.sampled=true;
}
inline void publishWeather() noexcept {
    if(api.weatherFields)if(const auto manager=singleton(api.weatherInstance)){
        std::int32_t base{};float rainy{},stormy{};
        nowBaseWeather.store(rd(manager,weatherBase,base)&&base>=0&&base<int(weatherNames.size())?base:-1,std::memory_order_relaxed);
        rainyWeight.store(rd(manager,weatherWeightsEffective+4*weatherRainy,rainy)&&std::isfinite(rainy)?rainy:-1.f,std::memory_order_relaxed);
        stormyWeight.store(rd(manager,weatherWeightsEffective+4*weatherStormy,stormy)&&std::isfinite(stormy)?stormy:-1.f,std::memory_order_relaxed);
    }
    if(api.vfxWeather){float v{};
        nowRain.store(rd(api.vfxWeather,vfxRain,v)&&std::isfinite(v)?v:-1.f,std::memory_order_relaxed);
        nowRainTarget.store(rd(api.vfxWeather,vfxRainTarget,v)&&std::isfinite(v)?v:-1.f,std::memory_order_relaxed);
        nowWetness.store(rd(api.vfxWeather,vfxWetness,v)&&std::isfinite(v)?v/3.f:-1.f,std::memory_order_relaxed);   // = GetCurrWetnessDueToRain
        rainAmount.store(rd(api.vfxWeather,vfxRainAmount,v)&&std::isfinite(v)?v:-2.f,std::memory_order_relaxed);
        lightningAmount.store(rd(api.vfxWeather,vfxLightningAmount,v)&&std::isfinite(v)?v:-2.f,std::memory_order_relaxed);
    }
    if(api.noRainParam){std::uintptr_t set{};noRain.store(rd(api.noRainParam,noRainValue,set)?(set?1:0):-1,std::memory_order_relaxed);}
    std::uintptr_t environment{};
    if(const auto spheres=sphereManager(environment)){
        const unsigned n=sphereCount(spheres);int enabled=0,live=0,top=-1,topExclusion=-1;float topFade=0.f;
        for(unsigned i=0;i<n;++i){const auto s=sphereAt(spheres,i);std::uint8_t on{},exclusion{};float fade{};std::int32_t weather{-1};
            if(rd(s,sphereEnabled,on)&&on)++enabled;
            if(rd(s,sphereFade,fade)&&std::isfinite(fade)&&fade>0.f){++live;if(fade>topFade&&rd(s,sphereWeather,weather)&&rd(s,sphereExclusion,exclusion)){
                topFade=fade;top=weather;topExclusion=exclusion?1:0;}}}
        spheresTotal.store(int(n),std::memory_order_relaxed);spheresEnabled.store(enabled,std::memory_order_relaxed);
        spheresActive.store(live,std::memory_order_relaxed);sphereTopWeather.store(top,std::memory_order_relaxed);
        sphereTopExclusion.store(topExclusion,std::memory_order_relaxed);sphereTopFade.store(live?topFade:0.f,std::memory_order_relaxed);
    }
    sampleLightning();
}
inline void publishWorld(bool inGame) noexcept {
    if(!inGame){nowTimeMinutes.store(-1);nowWeather.store(-1);nowSlow.store(-1);nowBaseWeather.store(-1);return;}
    if(const auto manager=singleton(api.todInstance)){
        std::uint32_t tod{};std::uint8_t warp{};float scale{},duration{};
        if(rd(manager,todValue,tod))nowTimeMinutes.store(int(((tod>>15)&31u)*60u+((tod>>20)&63u)));
        if(rd(manager,todWarpActive,warp)&&rd(manager,todWarpScale,scale)&&rd(manager,todWarpDuration,duration)){
            int s=0;if(warp){s=3;for(unsigned i=1;i<slowScales.size();++i)if(std::fabs(scale-slowScales[i])<1e-4f&&duration>warpDuration*.5f)s=int(i);}
            nowSlow.store(s);
        }
    }
    if(api.fn.getWeather)if(const auto manager=singleton(api.weatherInstance)){
        const int w=api.fn.getWeather(reinterpret_cast<const void*>(manager));nowWeather.store(w>=0&&w<int(weatherNames.size())?w:-1);engineCalls.fetch_add(1);}
    publishWeather();
}
inline void giveNext(const Player& p) noexcept { // ALL WEAPONS / a group: a few per frame
    const auto w=weaponView();
    for(unsigned k=0;k<allPerFrame&&owned.allNext<w.n;++owned.allNext){
        const auto& e=w[owned.allNext];if(owned.allGroup<GroupCount&&e.group!=owned.allGroup)continue;
        giveOne(p,e,false);++owned.allGiven;++k;}
    if(owned.allNext>=w.n){owned.allActive=false;owned.nextList=0; // Re-list now (owned / unlocked)
        static char msg[40];std::snprintf(msg,sizeof msg,"%u %s GIVEN",owned.allGiven,owned.allGroup<GroupCount?groupNames[owned.allGroup]:"WEAPONS");
        ::rdrvr::vr::panelSanitize(msg);result(RowAll,msg,true);}
}
} // namespace detail

// ---- main thread (VrMainThread module 2) ----
inline void tick(const mt::Frame& frame) noexcept {
    using namespace detail;
    if(!engineReady.load(std::memory_order_acquire))return;
    try{
        ++owned.frame;
        const bool wake=wakeups.exchange(0,std::memory_order_acq_rel)!=0;
        const bool storm=nowWeather.load(std::memory_order_relaxed)==weatherStormy||weatherOwned.state==weatherStormy;
        if(!wake&&!owned.allActive&&owned.frame%maintainEveryFrames&&frame.index<owned.nextList){if(storm)sampleLightning();return;} // No missed strikes
        Player p{};const char* reason=sample(p);const bool inGame=p.gameState==3;const bool ok=!reason;
        maintainWeather(inGame);   // Before publishing, so NOW shows the forced state as soon as the engine applied it
        publishWorld(inGame);
        maintainGod(p,ok);maintainDeadEye(p,ok);maintainAmmo(p,ok);maintainFreeze(inGame);maintainStamina(p,ok);
        // every ~2 s in game (0.25 s while the menu is open): locks, owned and holstered weapons change with the story
        if(inGame&&frame.index>=owned.nextList){owned.nextList=frame.index+(::rdrvr::vr::menuOpen.load(std::memory_order_relaxed)?15:120);
            if(ok&&p.hp>0)clearBadPending(p.inventory); // SAVE FIX (before the delete inside the list build)
            buildWeaponList(ok&&p.hp>0?p.actor:0);}
        if(owned.allActive){if(ok&&p.hp>0)giveNext(p);else{owned.allActive=false;result(RowAll,reason?reason:"NOT ALIVE",false);}}
    }catch(...){exceptions.fetch_add(1,std::memory_order_relaxed);}
}
inline void execute(const mt::Command& c,const mt::Frame&) noexcept {
    using namespace detail;
    if(!engineReady.load(std::memory_order_acquire))return;
    Player p{};const char* reason=sample(p);const bool inGame=p.gameState==3;
    const auto refuse=[&](Row r,const char* why){result(r,why,false);};
    switch(c.op){
    case OpHeal:{
        if(!api.fn.setHitPoints)return refuse(RowHeal,"UNAVAILABLE");
        if(reason)return refuse(RowHeal,reason);
        if(!(p.hp>0)||!std::isfinite(p.hpMax)||!(p.hpMax>0))return refuse(RowHeal,"NOT ALIVE");
        api.fn.setHitPoints(reinterpret_cast<void*>(p.health),p.hpMax,false,true,nullptr);engineCalls.fetch_add(1);
        float hp{};if(rd(p.health,healthValue,hp)&&hp<p.hpMax&&hp<=p.hp){api.fn.setHitPoints(reinterpret_cast<void*>(p.health),p.hpMax,true,true,nullptr);engineCalls.fetch_add(1);}
        float eyeMax{};if(api.fn.deadEyePoints&&rd(p.player,playerDeadEyeMax,eyeMax)&&std::isfinite(eyeMax)&&eyeMax>0){
            api.fn.deadEyePoints(reinterpret_cast<void*>(p.player),eyeMax,false);engineCalls.fetch_add(1);}
        return result(RowHeal,"HEALED",true);}
    case OpRefill:{
        if(!api.fn.ammoAmount)return refuse(RowRefill,"UNAVAILABLE");
        if(reason)return refuse(RowRefill,reason);
        std::uintptr_t component{},elements{};unsigned count{};
        if(!ammoComponent(p,component,count,elements))return refuse(RowRefill,"NO AMMO DATA");
        for(unsigned i=0;i<count;++i){float max{};
            if(rd(elements,i*ammoStride+ammoMaxField,max)&&std::isfinite(max)&&max>0){api.fn.ammoAmount(reinterpret_cast<void*>(component),int(i),max);engineCalls.fetch_add(1);}}
        return result(RowRefill,"AMMO REFILLED",true);}
    case OpGive:{
        if(!api.fn.giveWeapon)return refuse(RowGive,"UNAVAILABLE");
        if(reason)return refuse(RowGive,reason);
        if(!(p.hp>0))return refuse(RowGive,"NOT ALIVE");
        const auto w=weaponView();const auto i=unsigned(c.arg[0]);
        if(i>=w.n||w[i].id!=c.arg[1])return refuse(RowGive,"LIST CHANGED - AGAIN");
        giveOne(p,w[i],true);owned.nextList=0;
        return result(RowGive,"WEAPON GIVEN",true);}
    case OpReady:{ // HOLSTERS: put this owned weapon into its holster
        if(!api.fn.readyWeapon)return refuse(RowGive,"UNAVAILABLE");
        if(reason)return refuse(RowGive,reason);
        if(!(p.hp>0))return refuse(RowGive,"NOT ALIVE");
        const auto w=weaponView();const auto i=unsigned(c.arg[0]);
        if(i>=w.n||w[i].id!=c.arg[1])return refuse(RowGive,"LIST CHANGED - AGAIN");
        if(api.fn.hasWeapon&&!api.fn.hasWeapon(reinterpret_cast<const void*>(p.actor),w[i].id))return refuse(RowGive,"NOT OWNED");
        // ReadyEnumeratedWeapon ends with SetNextUseSlotFromWeaponEnum, which queues that slot in inv+0x450 (the engine draws it at
        // its next chance: field "always the sniper rifle comes out" after choosing a RIFLE here). The holster choice must not draw
        // anything, so the queued slot is restored afterwards.
        std::int32_t nextBefore{-1};const bool haveNext=p.inventory&&rd(p.inventory,invNextUseSlot,nextBefore);
        api.fn.readyWeapon(reinterpret_cast<void*>(p.actor),w[i].id);engineCalls.fetch_add(1);readies.fetch_add(1,std::memory_order_relaxed);
        std::int32_t nextAfter{-1};
        if(haveNext&&rd(p.inventory,invNextUseSlot,nextAfter)&&nextAfter!=nextBefore&&api.write&&api.write(p.inventory+invNextUseSlot,&nextBefore,sizeof nextBefore))
            nextUseRestores.fetch_add(1,std::memory_order_relaxed);
        owned.nextList=0;return result(RowGive,"HOLSTER SET",true);}
    case OpAll:{
        if(!api.fn.giveWeapon)return refuse(RowAll,"UNAVAILABLE");
        if(reason)return refuse(RowAll,reason);
        if(!weaponView().n)return refuse(RowAll,"NO WEAPON LIST");
        owned.allActive=true;owned.allNext=0;owned.allGiven=0;owned.allGroup=unsigned(std::clamp(c.arg[0],0,int(GroupCount)));
        if(owned.allGroup<GroupCount)groupGives[owned.allGroup].fetch_add(1,std::memory_order_relaxed);
        note(RowAll,"GIVING");::rdrvr::vr::cheatNote.store("GIVING",std::memory_order_release);return;}
    case OpTime:{
        if(!api.fn.timeOfDay||!api.todInstance)return refuse(RowTime,"UNAVAILABLE");
        if(!inGame)return refuse(RowTime,reason?reason:"NOT IN GAME");
        if(!singleton(api.todInstance))return refuse(RowTime,"NO CLOCK");
        const auto hour=std::uint32_t(std::clamp(c.arg[0],0,23));
        api.fn.timeOfDay(hour<<15);engineCalls.fetch_add(1);wakeups.fetch_add(1);
        return result(RowTime,"TIME SET",true);}
    case OpWeather:{
        if(!api.fn.changeWeather||!api.weatherInstance)return refuse(RowWeather,"UNAVAILABLE");
        if(!inGame)return refuse(RowWeather,reason?reason:"NOT IN GAME");
        const auto manager=singleton(api.weatherInstance);if(!manager)return refuse(RowWeather,"NO WEATHER");
        const int state=std::clamp(c.arg[0],0,int(weatherChoices)-1);
        if(state==int(weatherGameChoice)){ // Give the weather back (spheres and script amounts restored; the state stays)
            const bool was=weatherOwned.state>=0;releaseWeather();wakeups.fetch_add(1);
            return result(RowWeather,was?"WEATHER BACK TO GAME":"WEATHER ALREADY GAME",true);}
        api.fn.changeWeather(reinterpret_cast<void*>(manager),state,0.f,false);engineCalls.fetch_add(1);wakeups.fetch_add(1);
        // Forced from now on - re-asserted against scripts, spheres off, script rain/lightning amount 0 lifted.
        weatherOwned.state=state;weatherForced.store(state,std::memory_order_relaxed);maintainWeather(inGame);
        return result(RowWeather,(state==weatherRainy||state==weatherStormy)&&!api.vfxWeather?"WEATHER SET - NO RAIN API":"WEATHER FORCED",true);}
    case OpSlow:{
        if(!api.fn.timeWarp||!api.fn.cancelWarp||!api.todInstance)return refuse(RowSlow,"UNAVAILABLE");
        if(reason)return refuse(RowSlow,reason);
        const auto manager=singleton(api.todInstance);std::uint8_t warp{};float scale{},duration{};
        if(!manager||!rd(manager,todWarpActive,warp)||!rd(manager,todWarpScale,scale)||!rd(manager,todWarpDuration,duration))return refuse(RowSlow,"NO CLOCK");
        bool ours=false;for(unsigned i=1;i<slowScales.size();++i)ours=ours||(warp&&std::fabs(scale-slowScales[i])<1e-4f&&duration>warpDuration*.5f);
        const unsigned level=unsigned(std::clamp(c.arg[0],0,int(slowScales.size())-1));
        if(!level){
            if(!ours)return refuse(RowSlow,warp?"GAME WARP - LEFT ON":"ALREADY OFF");
            api.fn.cancelWarp(reinterpret_cast<void*>(manager),false);engineCalls.fetch_add(1);wakeups.fetch_add(1);return result(RowSlow,"SLOW MOTION OFF",true);
        }
        if(deadEyeRunning(p))return refuse(RowSlow,"DEAD EYE ACTIVE");
        if(warp&&!ours)return refuse(RowSlow,"GAME WARP ACTIVE");
        api.fn.timeWarp(reinterpret_cast<void*>(manager),slowScales[level],warpRamp,warpRamp,warpDuration);engineCalls.fetch_add(1);wakeups.fetch_add(1);
        return result(RowSlow,"SLOW MOTION ON",true);}
    default:return;
    }
}

// ---- XR thread: menu rows ----
namespace rows {
inline void post(std::uint16_t op,int arg,int arg1=0) noexcept {
    mt::Command c{};c.module=mt::ModuleCheats;c.op=op;c.arg[0]=arg;c.arg[1]=arg1;
    commands.fetch_add(1,std::memory_order_relaxed);
    if(!mt::executorInstalled()){::rdrvr::vr::cheatNote.store("NO MAIN THREAD HOOK",std::memory_order_release);return;}
    if(!mt::post(c))::rdrvr::vr::cheatNote.store("BUSY - TRY AGAIN",std::memory_order_release);
    else ::rdrvr::vr::cheatNote.store("WORKING",std::memory_order_release);
}
inline Row rowOf(void* context) noexcept {return Row(reinterpret_cast<std::uintptr_t>(context));}
inline Toggle toggleOf(Row r) noexcept {
    return r==RowGod?ToggleGod:r==RowDeadEye?ToggleDeadEye:r==RowAmmo?ToggleAmmo:r==RowFreeze?ToggleFreeze:ToggleStamina;}
inline bool isToggle(Row r) noexcept {return r==RowGod||r==RowDeadEye||r==RowAmmo||r==RowFreeze||r==RowStamina;}
inline void suffix(char* out,std::size_t size,Row r) noexcept {
    const char* n=rowNote[r].load(std::memory_order_acquire);
    if(n&&*n){const auto used=std::strlen(out);if(used+3<size)std::snprintf(out+used,size-used," - %s",n);}
}
inline void valueText(void* context,char* out,std::size_t size) noexcept {
    const Row r=rowOf(context);if(r>=RowCount||!size)return;
    if(!rowAvailable[r].load(std::memory_order_acquire)){std::snprintf(out,size,"UNAVAILABLE");return;}
    out[0]=0;
    const auto clock=[](int minutes,char* b,std::size_t s){if(minutes<0)std::snprintf(b,s,"--:--");else std::snprintf(b,s,"%02d:%02d",minutes/60,minutes%60);};
    switch(r){
    case RowHeal:case RowRefill:case RowAll:std::snprintf(out,size,"PRESS A");break;
    case RowWeapons:std::snprintf(out,size,"A: OPEN >");break;
    case RowGive:{
        const auto w=weaponView();
        if(!w.n){std::snprintf(out,size,"NO LIST YET");break;}
        const unsigned i=weaponPick.load(std::memory_order_relaxed)%w.n;
        std::snprintf(out,size,"< %s > %u/%u",w[i].name,i+1,w.n);break;}
    case RowTime:{char now[8];clock(nowTimeMinutes.load(std::memory_order_relaxed),now,sizeof now);
        std::snprintf(out,size,"SET %02u:00  NOW %s",timePick.load(std::memory_order_relaxed)%24,now);break;}
    case RowWeather:{const int w=nowWeather.load(std::memory_order_relaxed);const float rain=nowRain.load(std::memory_order_relaxed);
        std::snprintf(out,size,"SET %s  NOW %s",weatherPickNames[weatherPick.load(std::memory_order_relaxed)%weatherChoices],w>=0?weatherNames[unsigned(w)]:"?");
        // The GPU rain intensity (0.5 RAINY / 1.0 STORMY at full, ramps ~1/30 per s) - shows that drops are simulated
        if(rain>0.f){const auto used=std::strlen(out);if(used+11<size)std::snprintf(out+used,size-used," RAIN %d%%",int(std::lround(std::min(rain,9.99f)*100.f)));}
        break;}
    case RowSlow:{const int s=nowSlow.load(std::memory_order_relaxed);
        std::snprintf(out,size,"SET %s  NOW %s",slowNames[slowPick.load(std::memory_order_relaxed)%slowNames.size()],s<0?"?":s==3?"GAME":slowNames[unsigned(s)]);break;}
    default:{
        const Toggle t=toggleOf(r);const bool w=want[t].load(std::memory_order_acquire),a=active[t].load(std::memory_order_acquire);
        std::snprintf(out,size,"%s",w?(a?"ON":"ON - PENDING"):(a&&r!=RowStamina?"OFF - GAME ON":"OFF"));
        // One load - the main thread may clear the note between a check and the snprintf argument.
        const char* n=rowNote[r].load(std::memory_order_acquire);
        if(w&&a)suffix(out,size,r);else if(w&&!a&&n&&*n)std::snprintf(out,size,"ON - %s",n);
        return;}
    }
    // Action/choice rows show no stale refusal: the CHEATS footer (cheatNote) carries the last result.
}
inline void onAdjust(void* context,int direction) noexcept {
    const Row r=rowOf(context);if(r>=RowCount||!rowAvailable[r].load(std::memory_order_acquire))return;
    const auto step=[&](std::atomic<unsigned>& v,unsigned n){if(!n)return;const unsigned cur=v.load(std::memory_order_relaxed)%n;
        v.store((cur+(direction>0?1u:n-1u))%n,std::memory_order_relaxed);};
    switch(r){
    case RowGive:step(weaponPick,weaponView().n);break;
    case RowTime:step(timePick,24);break;
    case RowWeather:step(weatherPick,weatherChoices);break;
    case RowSlow:step(slowPick,unsigned(slowNames.size()));break;
    default:if(isToggle(r)){want[toggleOf(r)].store(direction>0,std::memory_order_release);wakeups.fetch_add(1);}break;
    }
}
inline void onActivate(void* context) noexcept {
    const Row r=rowOf(context);if(r>=RowCount)return;
    if(!rowAvailable[r].load(std::memory_order_acquire)){::rdrvr::vr::cheatNote.store("UNAVAILABLE",std::memory_order_release);return;}
    switch(r){
    case RowHeal:post(OpHeal,0);break;
    case RowRefill:post(OpRefill,0);break;
    case RowGive:{const auto w=weaponView();
        if(!w.n){::rdrvr::vr::cheatNote.store("NO WEAPON LIST YET",std::memory_order_release);break;}
        const unsigned i=weaponPick.load(std::memory_order_relaxed)%w.n;post(OpGive,int(i),w[i].id);break;}
    case RowAll:post(OpAll,int(GroupCount));break;
    case RowWeapons: ::rdrvr::vr::pageRequest.store(int(::rdrvr::vr::PageGiveWeapons),std::memory_order_release);break;
    case RowTime:post(OpTime,int(timePick.load(std::memory_order_relaxed)%24));break;
    case RowWeather:post(OpWeather,int(weatherPick.load(std::memory_order_relaxed)%weatherChoices));break;
    case RowSlow:post(OpSlow,int(slowPick.load(std::memory_order_relaxed)%slowNames.size()));break;
    default:if(isToggle(r)){auto& w=want[toggleOf(r)];w.store(!w.load(std::memory_order_acquire),std::memory_order_release);wakeups.fetch_add(1);
        ::rdrvr::vr::cheatNote.store(nullptr,std::memory_order_release);}break;
    }
}
} // namespace rows

// ---- SAVE FIX 2: invInventoryComponent::AddItem watcher (JUMP_SLOT 0x2e48600 -> 0x26615b8) ----
// simpleperf of the endless loading screen: the main thread spins in a script native (0x267b644, AddItem by type hash) that the save
// restore keeps calling. Every AddItem is counted per item type (report vr.cheats.addItem). A NON-PLAYER weapon (name like the list's
// skip set: antitank / cannon / turret / gatling / bow) that is added again and again (>= blockAfter calls = a retry loop, never a single
// mission give) is answered "added" without adding it, which ends the loop and drops it from the inventory.
inline constexpr std::uintptr_t addItemSlotOffset=0x2e48600,addItemOffset=0x26615b8;
inline constexpr std::uint32_t addItemBlockAfter=30;
using AddItemFn=bool(*)(void* inventory,const void* type,bool,bool);
inline std::atomic<AddItemFn> addItemOriginal{};
struct AddItemStat {std::atomic<const void*> type{};char name[40]{};std::atomic<std::uint32_t> calls{},fails{},blocked{};bool bad{};};
inline std::array<AddItemStat,24> addItemStats{};inline std::atomic<std::uint64_t> addItemCalls{},addItemOverflow{};
inline bool badItemName(const char* upper) noexcept {
    for(const char* b:{"ANTITANK","CANNON","TURRET","GATLING","BOW_"})if(std::strstr(upper,b))return true;return false;}
inline AddItemStat* addItemStat(const void* type) noexcept { // main thread (scripts / inventory) only writes
    for(auto& st:addItemStats){const void* t=st.type.load(std::memory_order_relaxed);if(t==type)return &st;
        if(!t){const char* name=nullptr;std::memcpy(&name,static_cast<const char*>(type)+0x8,sizeof name);
            if(name)for(unsigned i=0;i<sizeof st.name-1&&name[i];++i){char c=name[i];st.name[i]=c>='a'&&c<='z'?char(c-'a'+'A'):c;}
            st.bad=badItemName(st.name);st.type.store(type,std::memory_order_release);return &st;}}
    addItemOverflow.fetch_add(1,std::memory_order_relaxed);return nullptr;
}
inline bool addItemHook(void* inventory,const void* type,bool a,bool b) noexcept {
    addItemCalls.fetch_add(1,std::memory_order_relaxed);
    AddItemStat* st=type?addItemStat(type):nullptr;
    if(st){const auto n=st->calls.fetch_add(1,std::memory_order_relaxed)+1;
        if(st->bad&&n>=addItemBlockAfter){st->blocked.fetch_add(1,std::memory_order_relaxed);return true;}}
    const bool r=addItemOriginal.load(std::memory_order_relaxed)(inventory,type,a,b);
    if(st&&!r)st->fails.fetch_add(1,std::memory_order_relaxed);
    return r;
}
inline std::string addItemReport() {
    std::string o=",\"addItem\":{\"hooked\":";o+=addItemOriginal.load()?"true":"false";
    o+=",\"calls\":"+std::to_string(addItemCalls.load())+",\"overflow\":"+std::to_string(addItemOverflow.load())+",\"types\":[";
    bool first=true;for(auto& st:addItemStats){if(!st.type.load())continue;if(!first)o+=',';first=false;
        char b[160];std::snprintf(b,sizeof b,"[\"%s\",%u,%u,%u]",st.name,st.calls.load(),st.fails.load(),st.blocked.load());o+=b;}
    return o+"]}";
}

namespace pages {
inline bool ready() noexcept {return engineReady.load(std::memory_order_acquire)&&rowAvailable[RowGive].load(std::memory_order_acquire);}
inline void footer(std::string& out,const char* fallback) {
    char shown[64];const char* note=::rdrvr::vr::cheatNote.load(std::memory_order_acquire);
    std::snprintf(shown,sizeof shown,"%s",note&&*note?note:fallback);::rdrvr::vr::panelSanitize(shown);shown[::rdrvr::vr::menuColumns]=0;out+=shown;
}
template<class Row> inline void body(std::string& out,unsigned selected,unsigned rows,const Row& row) {
    const unsigned first=::rdrvr::vr::firstVisibleRow(selected,rows);
    for(unsigned r=first;r<first+::rdrvr::vr::menuBodyRows;++r){if(r>=rows){out+='\n';continue;}row(r);}
}
// GIVE WEAPONS: ALL, one row per group, then every weapon (A = give it; locked story weapons are unlocked first).
inline unsigned giveRows() noexcept {return 1u+GroupCount+weaponView().n;}
inline void giveInput(unsigned row,int adjust,bool activate) noexcept {
    if(!activate&&!adjust)return;
    if(!ready()){::rdrvr::vr::cheatNote.store("UNAVAILABLE",std::memory_order_release);return;}
    if(row==0){rows::post(OpAll,int(GroupCount));return;}
    if(row<=GroupCount){rows::post(OpAll,int(row-1));return;}
    const auto w=weaponView();const unsigned i=row-1-GroupCount;if(i<w.n)rows::post(OpGive,int(i),w[i].id);
}
inline void giveText(std::string& out,unsigned selected) {
    const auto w=weaponView();const unsigned rows=1u+GroupCount+w.n;
    body(out,selected,rows,[&](unsigned r){
        char label[48],value[32]{};
        if(r==0){std::snprintf(label,sizeof label,"GIVE ALL WEAPONS");std::snprintf(value,sizeof value,"%u",w.n);}
        else if(r<=GroupCount){unsigned n=0,have=0;for(unsigned i=0;i<w.n;++i)if(w[i].group==r-1){++n;have+=w[i].owned?1u:0u;}
            std::snprintf(label,sizeof label,"ALL %s",groupNames[r-1]);std::snprintf(value,sizeof value,"%u/%u OWNED",have,n);}
        else{const auto& e=w[r-1-GroupCount];std::snprintf(label,sizeof label,"%s",e.name);
            std::snprintf(value,sizeof value,"%s",e.ready?"IN HOLSTER":e.owned?"OWNED":e.locked?"LOCKED":"");}
        ::rdrvr::vr::panelSanitize(label);::rdrvr::vr::appendRow(out,r==selected,label,value);});
    footer(out,"A / TRIGGER: GIVE  B: BACK");
}
// HOLSTERS: one row per holster; triggers / A step through the owned weapons of that kind and put the chosen one into the holster.
inline void holsterIndex(const WeaponView& w,unsigned group,int& count,int& current) noexcept {
    count=0;current=-1;for(unsigned i=0;i<w.n;++i)if(w[i].group==group&&w[i].owned){if(w[i].ready)current=count;++count;}
}
inline void holsterInput(unsigned row,int adjust,bool activate) noexcept {
    if(row>=holsterRows||(!adjust&&!activate))return;
    if(!ready()||!detail::api.fn.readyWeapon){::rdrvr::vr::cheatNote.store("UNAVAILABLE",std::memory_order_release);return;}
    const auto w=weaponView();int count{},current{};holsterIndex(w,row,count,current);
    if(!count){::rdrvr::vr::cheatNote.store("NONE OWNED - CHEATS > WEAPONS BY TYPE",std::memory_order_release);return;}
    const int dir=adjust<0?-1:1;const int next=current<0?(dir>0?0:count-1):(current+dir+count)%count;
    int k=0;for(unsigned i=0;i<w.n;++i)if(w[i].group==row&&w[i].owned){if(k==next){rows::post(OpReady,int(i),w[i].id);return;}++k;}
}
inline void holsterText(std::string& out,unsigned selected) {
    const auto w=weaponView();
    for(unsigned r=0;r<holsterRows;++r){
        int count{},current{};holsterIndex(w,r,count,current);char value[48];
        const WeaponEntry* now=nullptr;{int k=0;for(unsigned i=0;i<w.n;++i)if(w[i].group==r&&w[i].owned){if(k==current)now=&w[i];++k;}}
        if(!count)std::snprintf(value,sizeof value,"NONE OWNED");
        else std::snprintf(value,sizeof value,"< %s > %d/%d",now?now->name:"-",current+1,count);
        ::rdrvr::vr::panelSanitize(value);::rdrvr::vr::appendRow(out,r==selected,holsterLabels[r],value);
    }
    out+="\n";
    for(const char* l:{"  REVOLVER: RIGHT HIP   REPEATER: RIGHT SHOULDER","  RIFLE: BEHIND THE HEAD   SHOTGUN: LEFT SHOULDER",
                       "  THROWN: CHEST   MELEE: BELT","","  ONLY WEAPONS YOU OWN ARE LISTED - GET MORE IN","  CHEATS > WEAPONS BY TYPE"}){out+=l;out+='\n';}
    for(unsigned r=holsterRows+7;r<::rdrvr::vr::menuBodyRows;++r)out+='\n';
    footer(out,"TRIGGERS / A: CHANGE  B: BACK");
}
inline unsigned holsterRowsFn() noexcept {return holsterRows;}
inline const ::rdrvr::vr::ExtPage givePage{&giveRows,&giveInput,&giveText};
inline const ::rdrvr::vr::ExtPage holsterPage{&holsterRowsFn,&holsterInput,&holsterText};
} // namespace pages

inline void tickThunk(const mt::Frame& f) noexcept {tick(f);}
inline void executeThunk(const mt::Command& c,const mt::Frame& f) noexcept {execute(c,f);}

// Startup (arm thread, once). Registers the rows (every row, UNAVAILABLE where its engine part did not verify) and the
// main-thread module. Host tests call it with fake functions/reads; the device goes through install() below.
inline bool configure(const EngineApi& api) noexcept {
    if(::rdrvr::vr::killed(::rdrvr::vr::Feature::Cheats)){installState.store("KILLED");
        ::rdrvr::vr::status::publish(::rdrvr::vr::Feature::Cheats,::rdrvr::vr::status::Property);return false;}
    if(installed.exchange(true))return engineReady.load();
    detail::api=api;const auto& f=api.fn;
    const bool player=api.read&&api.gameData&&api.playerManager&&api.playerManagerTable&&api.playerTable&&api.actorManagerGlobal&&api.actorSlotsGlobal;
    const bool tod=player&&api.todInstance;
    rowAvailable[RowGod].store(player&&f.invulnerable);rowAvailable[RowHeal].store(player&&f.setHitPoints);
    rowAvailable[RowDeadEye].store(player&&f.infiniteDeadEye);rowAvailable[RowAmmo].store(player&&api.inventoryTable&&f.ammoFlag);
    rowAvailable[RowRefill].store(player&&api.inventoryTable&&f.ammoAmount);
    rowAvailable[RowGive].store(player&&f.giveWeapon&&f.enumName&&api.enumTree);rowAvailable[RowAll].store(rowAvailable[RowGive].load());
    rowAvailable[RowWeapons].store(rowAvailable[RowGive].load());
    rowAvailable[RowTime].store(tod&&f.timeOfDay);rowAvailable[RowFreeze].store(tod&&f.acceleration);
    rowAvailable[RowWeather].store(player&&f.changeWeather&&api.weatherInstance);
    rowAvailable[RowSlow].store(tod&&f.timeWarp&&f.cancelWarp);rowAvailable[RowStamina].store(player&&(f.sprintStamina||f.horseFreshness));
    unsigned registered=0;
    for(unsigned r=0;r<RowCount;++r)registered+=::rdrvr::vr::registerCheatRow({rowLabels[r],&rows::valueText,&rows::onAdjust,&rows::onActivate,
        reinterpret_cast<void*>(std::uintptr_t(r))})?1u:0u;
    bool any=false;for(const auto& a:rowAvailable)any=any||a.load();
    if(!player||!any){installState.store(player?"ENGINE MISMATCH":"NO PLAYER API");
        ::rdrvr::vr::status::publish(::rdrvr::vr::Feature::Cheats,::rdrvr::vr::status::Unavailable);return false;}
    if(!mt::registerModule(mt::ModuleCheats,"cheats",&tickThunk,&executeThunk)){installState.store("REGISTER FAILED");return false;}
    engineReady.store(true,std::memory_order_release);
    ::rdrvr::vr::extPages[::rdrvr::vr::PageGiveWeapons].store(&pages::givePage,std::memory_order_release);
    ::rdrvr::vr::extPages[::rdrvr::vr::PageHolsters].store(&pages::holsterPage,std::memory_order_release);
    installState.store(registered==RowCount?"READY":"READY - ROWS REJECTED");
    ::rdrvr::vr::status::publish(::rdrvr::vr::Feature::Cheats,::rdrvr::vr::status::Ready);
    return true;
}
// Device install: whole-function fingerprints (code(offset, print)) and GOT words (got(slot, target)) before any use.
template<class Code,class Got>
inline bool install(EngineApi api,std::uintptr_t base,Code&& code,Got&& got) noexcept {
    if(::rdrvr::vr::killed(::rdrvr::vr::Feature::Cheats))return configure(api);
    if(!got(gameDataGotOffset,gameDataOffset))api.gameData=0;
    api.todInstance=got(todInstanceGotOffset,todInstanceOffset)?base+todInstanceOffset:0;
    api.weatherInstance=got(weatherInstanceGotOffset,weatherInstanceOffset)?base+weatherInstanceOffset:0;
    api.enumTree=got(enumTreeGotOffset,enumTreeOffset)?base+enumTreeOffset:0;
    auto& f=api.fn;f={};
    const auto fn=[&](std::uintptr_t offset,const auto& print,auto& out){
        if(code(offset,print))out=reinterpret_cast<std::remove_reference_t<decltype(out)>>(base+offset);};
    fn(invulnerableOffset,invulnerableFingerprint,f.invulnerable);
    fn(setHitPointsOffset,setHitPointsFingerprint,f.setHitPoints);
    fn(scriptInfiniteDeadEyeOffset,scriptInfiniteDeadEyeFingerprint,f.infiniteDeadEye);
    fn(deadEyePointsOffset,deadEyePointsFingerprint,f.deadEyePoints);
    fn(ammoInfiniteOffset,ammoInfiniteFingerprint,f.ammoFlag);
    fn(ammoAmountOffset,ammoAmountFingerprint,f.ammoAmount);
    fn(giveWeaponOffset,giveWeaponFingerprint,f.giveWeapon);
    fn(weaponEnumNameOffset,weaponEnumNameFingerprint,f.enumName);
    fn(scriptTimeOfDayOffset,scriptTimeOfDayFingerprint,f.timeOfDay);
    fn(accelerationOffset,accelerationFingerprint,f.acceleration);
    fn(changeWeatherOffset,changeWeatherFingerprint,f.changeWeather);
    fn(getWeatherOffset,getWeatherFingerprint,f.getWeather);
    fn(timeWarpOffset,timeWarpFingerprint,f.timeWarp);
    fn(cancelWarpOffset,cancelWarpFingerprint,f.cancelWarp);
    fn(sprintStaminaOffset,sprintStaminaFingerprint,f.sprintStamina);
    if(got(horseFlagGotOffset,horseFlagOffset))fn(horseFreshnessOffset,horseFreshnessFingerprint,f.horseFreshness);
    fn(setLockedOffset,setLockedFingerprint,f.setLocked);fn(hasWeaponOffset,hasWeaponFingerprint,f.hasWeapon);
    fn(readyWeaponOffset,readyWeaponFingerprint,f.readyWeapon);fn(hasReadyOffset,hasReadyFingerprint,f.hasReady);
    fn(deleteWeaponOffset,deleteWeaponFingerprint,f.deleteWeapon);
    // WEATHER layout: data only (never called) - each part needs its GOT word and the prints of the code that encodes its fields.
    api.weatherFields=api.weatherInstance&&code(updateComputedConstantsOffset,updateComputedConstantsFingerprint);
    api.vfxWeather=api.weatherFields&&got(vfxWeatherGotOffset,vfxWeatherOffset)&&code(vfxUpdateOffset,vfxUpdateFingerprint)&&
        code(isLightningEnabledOffset,isLightningEnabledFingerprint)&&code(vfxRenderOffset,vfxRenderFingerprint)?base+vfxWeatherOffset:0;
    api.environment=got(environmentGotOffset,environmentOffset)&&code(enableWeatherSphereOffset,enableWeatherSphereFingerprint)&&
        code(overrideWeatherOffset,overrideWeatherFingerprint)?base+environmentOffset:0;
    api.noRainParam=got(noRainGotOffset,noRainOffset)?base+noRainOffset:0;
    return configure(api);
}

// Report object for vr.cheats.
inline std::string report() {
    std::string out="{\"state\":\"";out+=installState.load();out+="\",\"engineReady\":";out+=engineReady.load()?"true":"false";
    out+=",\"rows\":{";
    for(unsigned r=0;r<RowCount;++r){
        if(r)out+=',';out+='"';out+=rowKeys[r];out+="\":{\"available\":";out+=rowAvailable[r].load()?"true":"false";
        const char* n=rowNote[r].load();out+=",\"note\":\"";out+=n?n:"";out+='"';
        if(rows::isToggle(Row(r))){const auto t=rows::toggleOf(Row(r));out+=",\"want\":";out+=want[t].load()?"true":"false";
            out+=",\"active\":";out+=active[t].load()?"true":"false";}
        out+='}';
    }
    char b[640];
    std::snprintf(b,sizeof b,"},\"weapons\":%u,\"picks\":{\"weapon\":%u,\"hour\":%u,\"weather\":%u,\"slow\":%u},\"now\":{\"minutes\":%d,\"weather\":%d,\"slow\":%d},"
        "\"commands\":%llu,\"executed\":%llu,\"refused\":%llu,\"engineCalls\":%llu,\"reasserts\":%llu,\"listBuilds\":%llu,\"listNodes\":%llu,"
        "\"listSkipped\":%llu,\"readFailures\":%llu,\"exceptions\":%llu,\"lastResult\":\"%s\"}",
        weaponCount.load(),weaponPick.load(),timePick.load(),weatherPick.load(),slowPick.load(),nowTimeMinutes.load(),nowWeather.load(),nowSlow.load(),
        (unsigned long long)commands.load(),(unsigned long long)executed.load(),(unsigned long long)refused.load(),(unsigned long long)engineCalls.load(),
        (unsigned long long)reasserts.load(),(unsigned long long)listBuilds.load(),(unsigned long long)listNodes.load(),(unsigned long long)listSkipped.load(),
        (unsigned long long)readFailures.load(),(unsigned long long)exceptions.load(),lastResult.load());
    out+=b;
    { // "weather" = why it does / does not rain
        const auto& a=detail::api;char wb[1024];
        std::snprintf(wb,sizeof wb,",\"weather\":{\"api\":{\"fields\":%s,\"vfx\":%s,\"spheres\":%s,\"noRainParam\":%s},\"forced\":%d,\"base\":%d,"
            "\"effective\":%d,\"weights\":{\"rainy\":%.3f,\"stormy\":%.3f},\"rain\":%.3f,\"rainTarget\":%.3f,\"wetness\":%.3f,"
            "\"rainAmount\":%.3f,\"lightningAmount\":%.3f,\"noRain\":%d,\"spheres\":{\"total\":%d,\"enabled\":%d,\"active\":%d,"
            "\"topWeather\":%d,\"topExclusion\":%d,\"topFade\":%.3f,\"disabledByUs\":%llu,\"restored\":%llu},\"reasserts\":%llu,"
            "\"amountWrites\":%llu,\"lightningEvents\":%llu,\"cloudLightningEvents\":%llu,\"lightningSamples\":%llu}",
            a.weatherFields?"true":"false",a.vfxWeather?"true":"false",a.environment?"true":"false",a.noRainParam?"true":"false",
            weatherForced.load(),nowBaseWeather.load(),nowWeather.load(),double(rainyWeight.load()),double(stormyWeight.load()),double(nowRain.load()),
            double(nowRainTarget.load()),double(nowWetness.load()),double(rainAmount.load()),double(lightningAmount.load()),noRain.load(),
            spheresTotal.load(),spheresEnabled.load(),spheresActive.load(),sphereTopWeather.load(),sphereTopExclusion.load(),double(sphereTopFade.load()),
            (unsigned long long)sphereWrites.load(),(unsigned long long)sphereRestores.load(),(unsigned long long)weatherReasserts.load(),
            (unsigned long long)amountWrites.load(),(unsigned long long)lightningEvents.load(),(unsigned long long)cloudLightningEvents.load(),
            (unsigned long long)lightningSamples.load());
        out.insert(out.size()-1,wb);
    }
    const auto w=weaponView();const auto& fa=detail::api.fn;
    std::string names=",\"weaponApi\":{\"setLocked\":";names+=fa.setLocked?"true":"false";names+=",\"hasWeapon\":";names+=fa.hasWeapon?"true":"false";
    names+=",\"readyWeapon\":";names+=fa.readyWeapon?"true":"false";names+=",\"hasReady\":";names+=fa.hasReady?"true":"false";
    names+=",\"unlocks\":"+std::to_string(unlocks.load())+",\"readies\":"+std::to_string(readies.load())+",\"publishes\":"+std::to_string(listPublishes.load())+
        ",\"deleteWeapon\":"+(fa.deleteWeapon?"true":"false")+",\"bad\":"+std::to_string(badCount.load())+",\"scrubFound\":"+std::to_string(scrubFound.load())+
        ",\"scrubbed\":"+std::to_string(scrubbed.load())+",\"pendingCleared\":"+std::to_string(pendingCleared.load())+",\"nextUseRestores\":"+std::to_string(nextUseRestores.load())+
        ",\"nextUseCleared\":"+std::to_string(nextUseCleared.load())+
        ",\"pending\":\""+pendingName.load()+"\"}";
    names+=addItemReport();
    names+=",\"weaponNames\":[";
    for(unsigned i=0;i<w.n;++i){const auto& e=w[i];if(i)names+=',';names+="[";names+=std::to_string(e.id);names+=",\"";names+=e.name;names+="\",";
        names+=std::to_string(e.group);names+=e.locked?",1":",0";names+=e.owned?",1":",0";names+=e.ready?",1]":",0]";}
    names+="]";
    out.insert(out.size()-1,names);
    return out;
}
} // namespace rdrvr::vr::cheats
