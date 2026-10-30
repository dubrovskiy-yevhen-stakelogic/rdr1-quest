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
//   CLEAR WANTED  A        script globals + faction/child-script calls: what the law scripts do when a chase ends + what pay_bounty does (see LAW below)
//   NO WANTED     OFF|ON   gLawDogs flag 0x100 (the scripts' own "law" console switch: CREATE_CRIME_EVENT refuses, HUD meter off), any raise cleared at once
//
// Threads: rows' callbacks run on the XR thread (menu): they only flip atomics or post VrMainThread commands. Every engine
// call runs on the engine main thread inside the sagShellDerived::UpdateInputs wrapper (VrMainThread module 2), after
// whole-function fingerprints of every called function and GOT checks of every global (install()). A row whose function
// did not verify shows UNAVAILABLE and never calls the engine. Kill switch: debug.rdrvr.vr.cheats 0 (no rows, no engine use).
#include "VrMenuPages.h"
#include "VrMainThread.h"
#include "VrDeadEye.h" // DEAD EYE: state for the mapper / XR tint, HALF drain
#include "VrDuelStart.h" // DUEL ON LOOK: the game's own duel script on the person looked at
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
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace rdrvr::vr::cheats {
namespace mt=::rdrvr::vr::mainthread;

// ---- engine facts (librdr.so 1.58, vaddr; .text file offset = vaddr - 0x4000) ----
inline constexpr std::uintptr_t invulnerableOffset=0x1cab2a4,setHitPointsOffset=0x1e83500,scriptInfiniteDeadEyeOffset=0x1caf890,
    deadEyePointsOffset=0x2696bd0,ammoInfiniteOffset=0x2540594,ammoAmountOffset=0x2540528,giveWeaponOffset=0x267984c,
    weaponEnumNameOffset=0x254ea64,scriptTimeOfDayOffset=0x2abaa10,accelerationOffset=0x2398b9c,changeWeatherOffset=0x2aaa018,
    getWeatherOffset=0x2aaa1d4,timeWarpOffset=0x23997a0,cancelWarpOffset=0x23997f4,sprintStaminaOffset=0x2420498,
    horseFreshnessOffset=0x245070c;
// WEAPONS and the HOLSTERS menu (which revolver / rifle... is in each holster):
// byte [enum+0x180] is WeaponEnumBase::IsLocked (0x254b090), NOT a redirect: FindWeaponEnumOrCategory gives a random UNLOCKED weapon
// of the category for a locked enum. Every enum is listed, including the 25 locked ones (story progress); a give of a
// locked one first calls WeaponEnumBase::SetLocked(false) (0x254b098: also re-adds it to its category, like the game's own unlock).
// HOLSTERS: HasEnumeratedWeapon(actor,enum) 0x267a498 (owned), HasEnumeratedWeaponReady(actor,enum) 0x2679c88 (= the one in its
// holster: invInventoryComponent::HasNamedItemEquipped), ReadyEnumeratedWeapon(actor,enum) 0x26799b4 (EquipItem(name) = put it into
// its holster slot + SetNextUseSlotFromWeaponEnum) - the script natives the game uses for the same thing.
inline constexpr std::uintptr_t setLockedOffset=0x254b098,hasWeaponOffset=0x267a498,readyWeaponOffset=0x26799b4,hasReadyOffset=0x2679c88;
// SAVE GUARD: a save holding RIFLE_ANTITANK (mission weapon) or BOW_SHORTBOW, which older ALL WEAPONS versions gave, never leaves the
// loading screen (world sim runs, no scene ever renders). Such weapons are not
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
// DEAD EYE meter (VrDeadEye.h): sagPlayerStates+0x1c = level (ldr w, GetDeadEyeTimeRemaining 0x269d6dc), player+0x3c0 = points
// (SetDeadEyePoints 0x2696c24 str s0)
inline constexpr std::size_t statesDeadEyeLevel=0x1c,playerDeadEyePoints=0x3c0;
// DEAD EYE MODE SLOW-MO (VrDeadEye.h): player+0x3bd = multi-lock enable (u8 bool, sagPlayerHandle::SetDeadEyeMultiLockEnable
// 0x26a9700 strb; UpdateWeaponSpecialMove 0x269a808 ldrb: 0 and no duel -> engine Dead Eye level 1), player+0x3be bit2 = duel
// (sagPlayerHandle::GetDeadEyeDueling 0x26a976c ubfx #2: a duel is level 2 whatever 0x3bd says - never touched in a duel).
inline constexpr std::size_t playerDeadEyeMultiLock=0x3bd,playerDeadEyeFlags=0x3be;
inline constexpr std::uint8_t deadEyeDuelBit=0x4;
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

// LAW (CLEAR WANTED / NO WANTED cheats). The law system is script only: there is no engine
// function for it, the state is the script global block (rage::scrProgram::sm_Globals 0x4e6ebd60, GOT 0x2de22b0; sm_GlobalSize 0x4e6ebd58,
// GOT 0x2de22a0; layout signature at 0x4e6ebd74 read by GetGlobalsHash 0x1be5298 = word 0 of every SCR header). scrThread::Run: GLOBAL_U16 =
// base + index*4 (0x1be0b04), its load/store are 32 bit (0x1be1764), IADD_U8 adds BYTES (0x1be1420), array element = array + 8 + i*stride*8
// with the count at array+0 (0x1be1390). Names of the save blocks: script 0002, rdr2SaveGameManager::RegisterData (native 0x1bf05d8):
// 0x9bf2 "gLawDogs" (0x138 bytes), 0x9c7c "gPlayerBounties" (0x1170 bytes). Both are SAVED, see lawMarkerBit.
//   0x19f6 wanted flag (HUD script 0189 shows the WantedMeter on it; scripts 0186/0190 SET_WANTED func 0x174d1/0x124c5)
//   0x19f7 read with it by 121 scripts (never written by a readable script: reported only), 0x19f8 cleared with it
//   0x5447 HUD law state, -1 = hide the meter; 0x5426 HUD meter (0..1, report only)
//   0x9c40 law faction of the region, +0x8 current territory
//   0x9c7c bounty[3]; +0x20 pending bounty[3] (func 0x20b96 moves it to bounty); +0x44 crimes[3][36][4] (snapshot source only, never written here)
//   0xa0d8 snapshot taken on clear: +0 bounty, +4 array[36] = crimes[territory][i][3] where crime definition 0xa124[i] (stride 0xb) is set
//   gLawDogs +0x58 flags: 0x100 law off (tested by CREATE_CRIME_EVENT's gate, func 0xf389 in main.sc and every town script, and by the HUD; main.sc's
//   console command "law" toggles exactly this bit, func 0xfbcd; bounty missions set it too), 0x20000000 HUD notice "law_bounty_cleared" (HUD script 0xd0da..d163)
//   gLawDogs +0x60 / +0x68 guids of the law child scripts, ended by SET_WANTED(false)
// SET_WANTED(false) (0190 func 0x124c5, called by the mission launcher when a mission starts), mirrored by detail::lawClear: 0x19f6 = 0,
// 0x19f8 = 0, snapshot, SetStatusTwoWay(lawFaction,2,2) + (8,2,2) + (10,2,2), TerminateAllNamedChildScripts + AddToTerminationList for
// both guids, 0x5447 = -1; it does nothing when the flag is already 0. Clearing the bounty is the mod's addition.
// The raise and the chase live in .wsc resources of content_android.rpf (AES, then zstd; code pages through the table in the program header). Numbers are
// the rpf toc entries; every fact below was read from them:
//   2508, module "LAW" (func 0x2eb2c, runs on the script scheduler): with crime objects of "CurrentCrimes" in state 3 (reported) in this territory it calls
//   SET_WANTED(true) and starts the chase script (+0x60). The only gate is func 0x226d2, which is false while 0x100 is set: so NO WANTED stops raises at the source,
//   and clearing the flag alone is undone on the LAW module's next tick: the reported crime is still there.
//   0189..0208 (chase scripts) end with SET_WANTED(false), flag 0x400 and 0x5426 = 1.0. LAW answers 0x400 (func 0x342f7) with: every crime to state 1 + expired
//   (func 0x31055 then destroys it), ends the witness script (+0x68), takes 0x400 back. This is what the mod sends.
//   0193 (witness script) sets 0x19f7 on start and only its own exit code zeroes it: ended from outside it stays 1 for good (scripts read it as "being reported",
//   crimes are not destroyed while it is set), so the mod zeroes it. Its exit code also restores the player from the road report cutscene, as does any scripted
//   scene: nothing is ended while 0x1834a (scripted scene running) is set.
//   0196 (pay_bounty, pardon letter): bounty[t] = 0, stat 0x114c[t] and total stat 0xde = 0 (table 0xd346, float), 0x5447 = -1, flag 0x800000 off,
//   crimes[t][i][3] = 0 (rap sheet), snapshot, notice 0x20000000. The mod does all of it for the 3 territories.
// Any layout doubt = row UNAVAILABLE or lastError, never a write. All code runs on the engine main thread (scripts run on that thread too, in
// sagScriptContext::UpdateAllScripts, not nested in the UpdateInputs wrapper).
inline constexpr std::uintptr_t scriptGlobalsGotOffset=0x2de22b0,scriptGlobalsOffset=0x4e6ebd60,scriptGlobalSizeGotOffset=0x2de22a0,scriptGlobalSizeOffset=0x4e6ebd58,
    scriptGlobalsHashOffset=0x4e6ebd74,globalsHashFnOffset=0x1be5298,vmGlobalRefOffset=0x1be0b04,vmGlobalStoreOffset=0x1be1764,vmArrayElementOffset=0x1be1390,
    vmIaddU8Offset=0x1be1420;
inline constexpr std::uintptr_t factionMgrGotOffset=0x2deb220,factionMgrOffset=0x4e70c4f8,isFactionValidOffset=0x1e51834,setStatusTwoWayOffset=0x1e51284,
    nativeSetStatusOffset=0x2324c7c,scriptGuidCountGotOffset=0x2e01298,scriptGuidCountOffset=0x4e6ecfe0,scriptGuidSlotsGotOffset=0x2e012a0,scriptGuidSlotsOffset=0x4e6ecfc8,
    terminateChildrenOffset=0x1bec19c,addToTerminationOffset=0x1c04f48,nativeTerminateOffset=0x1bee408,nativeScriptValidOffset=0x1bee2ac;
inline constexpr std::int32_t lawGlobalWords=0x225b1;
inline constexpr std::uint32_t lawGlobalsSignature=0xf9ae02c6u;
inline constexpr std::uint32_t lawWantedWord=0x19f6,lawWanted2Word=0x19f7,lawMeterFlagWord=0x19f8,lawHudMeterWord=0x5426,lawHudStateWord=0x5447,lawDogsWord=0x9bf2,
    lawFactionWord=0x9c40,bountiesWord=0x9c7c,lawSnapshotWord=0xa0d8,crimeDefsWord=0xa124,lawStatsWord=0xd346,lawSceneWord=0x1834a;
inline constexpr std::size_t lawDogsFlags=0x58,lawDogsChildA=0x60,lawDogsChildB=0x68,lawTerritory=0x8,bountiesPending=0x20,bountiesCrimes=0x44,lawSnapshotList=0x4,bountiesStats=0x114c;
inline constexpr std::uint32_t lawOffBit=0x100u,lawNoticeBit=0x20000000u,lawChaseEndBit=0x400u,lawReportedBit=0x800000u;
// gLawDogs is in the save game: a save made while NO WANTED holds 0x100 would keep the law off for good after the cheat is gone. So the mod's 0x100
// always comes with this marker (no readable script uses bit 31 of the flags and they only ever test masks): marker set + cheat off = the mod takes both back.
inline constexpr std::uint32_t lawMarkerBit=0x80000000u;
inline constexpr int lawPlayerFaction=2,lawCalmFeeling=2,lawScriptLayer=2,lawOtherFactionA=8,lawOtherFactionB=10,lawTerritories=3,lawCrimeTypes=36,lawBountyTotalStat=0xde;
inline constexpr unsigned lawHudHoldFrames=45,lawPressHoldFrames=180;
inline constexpr std::array<unsigned char,20> isFactionValidFingerprint{
#include "gamecode/VrCheats_1e51834_20_8d.inc"
};
inline constexpr std::array<unsigned char,144> setStatusTwoWayFingerprint{
#include "gamecode/VrCheats_1e51284_144_8d.inc"
};
inline constexpr std::array<unsigned char,244> terminateChildrenFingerprint{
#include "gamecode/VrCheats_1bec19c_244_8d.inc"
};
inline constexpr std::array<unsigned char,428> addToTerminationFingerprint{
#include "gamecode/VrCheats_1c04f48_428_8d.inc"
};
inline constexpr std::array<unsigned char,36> vmGlobalRefFingerprint{
#include "gamecode/VrCheats_1be0b04_36_8d.inc"
};
inline constexpr std::array<unsigned char,44> vmGlobalStoreFingerprint{
#include "gamecode/VrCheats_1be1764_44_8d.inc"
};
inline constexpr std::array<unsigned char,16> vmArrayElementFingerprint{
#include "gamecode/VrCheats_1be1390_16_8d.inc"
};
inline constexpr std::array<unsigned char,44> vmIaddU8Fingerprint{
#include "gamecode/VrCheats_1be1420_44_8d.inc"
};
inline constexpr std::array<unsigned char,140> nativeSetStatusFingerprint{
#include "gamecode/VrCheats_2324c7c_140_8d.inc"
};
inline constexpr std::array<unsigned char,128> nativeTerminateFingerprint{
#include "gamecode/VrCheats_1bee408_128_8d.inc"
};
inline constexpr std::array<unsigned char,80> nativeScriptValidFingerprint{
#include "gamecode/VrCheats_1bee2ac_80_8d.inc"
};
inline constexpr std::array<unsigned char,12> globalsHashFingerprint{40,88,38,208,0,117,77,185,192,3,95,214};

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
using IsFactionValidFn=bool(*)(const void* manager,int faction);
using SetStatusTwoWayFn=void(*)(void* manager,int a,int b,int feeling,int layer);
using TerminateChildrenFn=void(*)(const char* name,void* context);
using AddToTerminationFn=void(*)(void* context);
struct Functions {
    InvulnerableFn invulnerable{};SetHitPointsFn setHitPoints{};ScriptInfiniteDeadEyeFn infiniteDeadEye{};DeadEyePointsFn deadEyePoints{};
    AmmoFlagFn ammoFlag{};AmmoAmountFn ammoAmount{};GiveWeaponFn giveWeapon{};EnumNameFn enumName{};ScriptTimeOfDayFn timeOfDay{};
    AccelerationFn acceleration{};ChangeWeatherFn changeWeather{};GetWeatherFn getWeather{};TimeWarpFn timeWarp{};CancelWarpFn cancelWarp{};
    SprintStaminaFn sprintStamina{};HorseFreshnessFn horseFreshness{};
    SetLockedFn setLocked{};HasWeaponFn hasWeapon{};ReadyWeaponFn readyWeapon{};HasReadyFn hasReady{};DeleteWeaponFn deleteWeapon{};
    IsFactionValidFn isFactionValid{};SetStatusTwoWayFn setStatus{};TerminateChildrenFn terminateChildren{};AddToTerminationFn addToTermination{};
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
    // LAW (each zero unless its GOT word and the prints of the code that encodes the layout verified):
    std::uintptr_t scriptGlobals{},scriptGlobalSize{},scriptGlobalsHash{},factionMgr{},scriptGuidCount{},scriptGuidSlots{};
    bool lawGate{true};                // false: debug.rdrvr.vr.wantedgate 0 = NO WANTED only clears each frame, never writes the law-off flag
    WriteFn write{&processWrite};
    Functions fn{};
};

// ---- rows ----
enum Row : unsigned {RowGod,RowHeal,RowDeadEye,RowAmmo,RowRefill,RowGive,RowAll,RowWeapons,RowTime,RowFreeze,RowWeather,RowSlow,RowStamina,RowDuel,RowClearWanted,RowNoWanted,RowCount};
inline constexpr std::array<const char*,RowCount> rowLabels{"GOD MODE","HEAL","INF DEAD EYE","INF AMMO","REFILL AMMO","GIVE WEAPON",
    "ALL WEAPONS","WEAPONS BY TYPE","TIME","FREEZE CLOCK","WEATHER","SLOW MOTION","INF STAMINA","DUEL ON LOOK","CLEAR WANTED","NO WANTED"};
inline constexpr std::array<const char*,RowCount> rowKeys{"god","heal","deadEye","infAmmo","refillAmmo","giveWeapon","allWeapons","weaponsByType","time",
    "freezeClock","weather","slowMotion","infStamina","duelOnLook","clearWanted","noWanted"};
// weapon groups (first word of the enum name, e.g. WEAPON_REVOLVER_CATTLEMAN) = the holster each one goes to.
enum Group : std::uint8_t {GroupSidearm,GroupRepeater,GroupRifle,GroupShotgun,GroupThrown,GroupMelee,GroupOther,GroupCount};
inline constexpr std::array<const char*,GroupCount> groupNames{"REVOLVERS + PISTOLS","REPEATERS","RIFLES","SHOTGUNS","THROWN","MELEE","OTHER"};
inline constexpr std::array<const char*,GroupCount> holsterLabels{"REVOLVER","REPEATER","RIFLE","SHOTGUN","THROWN","MELEE","OTHER"};
inline constexpr unsigned holsterRows=6; // the groups with a holster (OTHER: bow / lasso / ... stay in the GIVE lists only)
enum Toggle : unsigned {ToggleGod,ToggleDeadEye,ToggleAmmo,ToggleFreeze,ToggleStamina,ToggleNoWanted,ToggleCount};
enum Op : std::uint16_t {OpHeal=1,OpRefill=2,OpGive=3,OpAll=4,OpTime=5,OpWeather=6,OpSlow=7,OpReady=8,OpClearWanted=10,OpDuel=duellink::opPress};
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
// LAW (main thread writes, menu/report read). wantedLevel: -1 unknown, else the wanted flag; wantedBounty: bounty + pending of all territories.
inline std::atomic<int> wantedLevel{-1},wantedFlag2{-1},wantedBounty{-1},wantedPending{-1},wantedHudState{-1},wantedGlobalWords{-1},wantedScene{-1};
inline std::atomic<std::uint32_t> wantedFlags{0},wantedSignature{0};
inline std::atomic<float> wantedHudMeter{-1.f};
inline std::atomic<std::uint64_t> wantedClearPresses{},wantedCleared{},wantedBlockedRaises{},wantedReraised{},wantedChildEnds{},wantedFactionCalls{},wantedGateSets{},wantedGateReleases{};
inline std::atomic<const char*> wantedError{""};

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
    // SAVE GUARD: remove a non-player weapon an older ALL WEAPONS gave (it blocks the next save load)
    if(actor&&api.fn.hasWeapon&&api.fn.deleteWeapon)for(unsigned i=0;i<bad;++i)
        if(api.fn.hasWeapon(reinterpret_cast<const void*>(actor),badIds[i])){scrubFound.fetch_add(1,std::memory_order_relaxed);
            api.fn.deleteWeapon(reinterpret_cast<void*>(actor),badIds[i]);engineCalls.fetch_add(1);
            if(!api.fn.hasWeapon(reinterpret_cast<const void*>(actor),badIds[i]))scrubbed.fetch_add(1,std::memory_order_relaxed);}
    if(n){weaponPublished.store(n|(back<<16),std::memory_order_release);weaponCount.store(n,std::memory_order_release);listPublishes.fetch_add(1,std::memory_order_relaxed);}
}
// SAVE GUARD: the item being equipped, when it is a non-player weapon (name checked like the list), is dropped from inv+0x458.
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
// ---- LAW: CLEAR WANTED / NO WANTED (facts: LAW block at the top) ----
struct LawState {unsigned pressHold{},hudHold{};std::uint64_t ticks{};bool noWantedSeen{};};
inline LawState lawState{};
inline bool lawApi() noexcept {
    const auto& f=api.fn;
    return api.scriptGlobals&&api.scriptGlobalSize&&api.scriptGlobalsHash&&api.factionMgr&&api.scriptGuidCount&&api.scriptGuidSlots&&
        f.isFactionValid&&f.setStatus&&f.terminateChildren&&f.addToTermination;
}
// The global block only after main script 0002 made it, with exactly the layout the indices above were read from.
inline std::uintptr_t lawGlobals() noexcept {
    std::uintptr_t globals{};std::int32_t words{};std::uint32_t signature{};
    if(!rd(api.scriptGlobals,0,globals)||!rd(api.scriptGlobalSize,0,words)||!rd(api.scriptGlobalsHash,0,signature)){wantedError.store("GLOBALS UNREADABLE",std::memory_order_relaxed);return 0;}
    wantedGlobalWords.store(words,std::memory_order_relaxed);wantedSignature.store(signature,std::memory_order_relaxed);
    if(!globals||globals%4){wantedError.store("NO GLOBALS YET",std::memory_order_relaxed);return 0;}
    if(words!=lawGlobalWords||signature!=lawGlobalsSignature){wantedError.store("GLOBALS MISMATCH",std::memory_order_relaxed);return 0;}
    wantedError.store("",std::memory_order_relaxed);
    return globals;
}
inline std::uintptr_t lawWord(std::uintptr_t globals,std::uint32_t word) noexcept {return globals+std::uintptr_t(word)*4;}
// scrThread::Run array element: bounds as the VM (count at +0), element at +8.
inline bool lawElement(std::uintptr_t array,unsigned stride,int i,std::uintptr_t& out) noexcept {
    std::int32_t n{};if(i<0||!rd(array,0,n)||i>=n)return false;
    out=array+8+std::uintptr_t(unsigned(i))*stride*8;return true;
}
// Sum of bounty + pending; zero = true also zeroes them.
inline std::int64_t lawBounties(std::uintptr_t globals,bool zero,std::int64_t* pending=nullptr) noexcept {
    const auto bounties=lawWord(globals,bountiesWord);std::int64_t total=0,later=0;
    for(int t=0;t<lawTerritories;++t){
        std::uintptr_t e{};std::int32_t v{};
        if(lawElement(bounties,1,t,e)&&rd(e,0,v)&&v){total+=v;if(zero)wr(e,0,std::int32_t{0});}
        if(lawElement(bounties+bountiesPending,1,t,e)&&rd(e,0,v)&&v){later+=v;if(zero)wr(e,0,std::int32_t{0});}
    }
    if(pending)*pending=later;
    return total+later;
}
// pay_bounty func 0xcdb, all territories: the rap sheet (bounty hunters and marshals are picked by these counts).
inline void lawZeroRapSheet(std::uintptr_t globals) noexcept {
    const auto crimes=lawWord(globals,bountiesWord)+bountiesCrimes;
    for(int t=0;t<lawTerritories;++t){
        std::uintptr_t perType{};if(!lawElement(crimes,0xb5,t,perType))continue;
        for(int i=0;i<lawCrimeTypes;++i){std::uintptr_t type{},count{};if(lawElement(perType,5,i,type)&&lawElement(type,1,3,count))wr(count,0,std::int32_t{0});}
    }
}
// pay_bounty func 0x1ec3: the stats that mirror the bounty (stats page). Stat 0 is the money: an unset table word (0) is never written.
inline void lawZeroBountyStats(std::uintptr_t globals) noexcept {
    const auto stats=lawWord(globals,lawStatsWord),ids=lawWord(globals,bountiesWord)+bountiesStats;
    const auto zero=[&](std::int32_t id)noexcept{std::uintptr_t e{};if(id>0&&lawElement(stats,1,id,e))wr(e,0,0.f);};
    for(int t=0;t<lawTerritories;++t){std::uintptr_t e{};std::int32_t id{};if(lawElement(ids,1,t,e)&&rd(e,0,id))zero(id);}
    zero(lawBountyTotalStat);
}
// Func 0x12554: the snapshot SET_WANTED(false) takes (bounty, then the crime counts of the current territory).
inline void lawSnapshot(std::uintptr_t globals) noexcept {
    std::int32_t territory{-1};if(!rd(lawWord(globals,lawFactionWord),lawTerritory,territory))return;
    const auto snapshot=lawWord(globals,lawSnapshotWord),bounties=lawWord(globals,bountiesWord);
    std::uintptr_t e{};std::int32_t bounty{};
    if(territory>=0&&territory<lawTerritories&&lawElement(bounties,1,territory,e))rd(e,0,bounty);
    wr(snapshot,0,bounty);
    std::uintptr_t perType{};if(!lawElement(bounties+bountiesCrimes,0xb5,territory,perType))return;
    const auto list=snapshot+lawSnapshotList;std::int32_t n{};if(!rd(list,0,n))return;
    n=std::clamp(n,0,lawCrimeTypes);
    for(int i=0;i<n;++i){
        std::uintptr_t def{},type{},count{},to{};std::int32_t set{},value{};
        if(lawElement(lawWord(globals,crimeDefsWord),0xb,i,def)&&rd(def,0,set)&&set&&lawElement(perType,5,i,type)&&lawElement(type,1,3,count)&&rd(count,0,value)&&
           lawElement(list,1,i,to))wr(to,0,value);
    }
}
inline void lawCalmFactions(std::uintptr_t globals) noexcept {
    auto* manager=reinterpret_cast<void*>(api.factionMgr);
    std::int32_t faction{-1};rd(lawWord(globals,lawFactionWord),0,faction);
    if(api.fn.isFactionValid(manager,faction)){api.fn.setStatus(manager,faction,lawPlayerFaction,lawCalmFeeling,lawScriptLayer);wantedFactionCalls.fetch_add(1,std::memory_order_relaxed);}
    // The native behind the script's two extra calls checks validity first (0x2324c7c), so does the mod.
    if(api.fn.isFactionValid(manager,lawPlayerFaction))for(int other:{lawOtherFactionA,lawOtherFactionB})if(api.fn.isFactionValid(manager,other)){
        api.fn.setStatus(manager,other,lawPlayerFaction,lawCalmFeeling,lawScriptLayer);wantedFactionCalls.fetch_add(1,std::memory_order_relaxed);}
    engineCalls.fetch_add(1,std::memory_order_relaxed);
}
// Natives 0x1bee2ac + 0x1bee408: guid -> sagScriptContext through the aGuidGeneral registry (slot = index*16, generation at +8).
inline bool lawEndChild(std::uint32_t guid) noexcept {
    std::uintptr_t table{},slots{},context{};std::uint16_t count{},generation{};
    const std::uintptr_t index=guid&0xffffu;
    if(!rd(api.scriptGuidCount,0,table)||!table||!rd(table,0,count)||index>=count||!rd(api.scriptGuidSlots,0,slots)||!slots||slots%8||
       !rd(slots,index*16+8,generation)||generation!=(guid>>16)||!rd(slots,index*16,context)||!context)return false;
    api.fn.terminateChildren(nullptr,reinterpret_cast<void*>(context));api.fn.addToTermination(reinterpret_cast<void*>(context));
    engineCalls.fetch_add(1,std::memory_order_relaxed);wantedChildEnds.fetch_add(1,std::memory_order_relaxed);
    return true;
}
inline bool lawBusy(std::uintptr_t globals) noexcept {std::int32_t scene{};return rd(lawWord(globals,lawSceneWord),0,scene)&&scene!=0;}
// The end of a chase + pay_bounty (LAW block at the top). The crimes go through flag 0x400 even when nothing else is set: a crime the mod cannot see would raise
// the flag again. Returns true when there was anything to clear. The caller checks lawBusy first.
inline bool lawClear(std::uintptr_t globals,bool& wasWanted,std::int64_t& hadBounty) noexcept {
    const auto flag=lawWord(globals,lawWantedWord),witnessFlag=lawWord(globals,lawWanted2Word),dogs=lawWord(globals,lawDogsWord);
    std::int32_t wanted{},witness{};std::uint32_t flags{};
    wasWanted=false;hadBounty=0;
    if(!rd(flag,0,wanted)||!rd(witnessFlag,0,witness)||!rd(dogs,lawDogsFlags,flags)){wantedError.store("WANTED FLAG UNREADABLE",std::memory_order_relaxed);return false;}
    hadBounty=lawBounties(globals,true);
    lawZeroBountyStats(globals);lawZeroRapSheet(globals);
    if(wanted){
        wasWanted=true;
        wr(flag,0,std::int32_t{0});wr(lawWord(globals,lawMeterFlagWord),0,std::int32_t{0});wr(lawWord(globals,lawHudMeterWord),0,1.f);
        lawCalmFactions(globals);
    }
    lawSnapshot(globals);
    bool ended=false;
    for(std::size_t field:{lawDogsChildA,lawDogsChildB}){std::uint32_t guid{};if(rd(dogs,field,guid)&&guid&&lawEndChild(guid))ended=true;}
    if(witness)wr(witnessFlag,0,std::int32_t{0});
    if(hadBounty>0)flags|=lawNoticeBit;
    wr(dogs,lawDogsFlags,(flags|lawChaseEndBit)&~lawReportedBit);
    if(!wasWanted&&!witness&&!ended&&!hadBounty)return false;
    wr(lawWord(globals,lawHudStateWord),0,std::int32_t{-1});
    wantedCleared.fetch_add(1,std::memory_order_relaxed);
    lawState.hudHold=lawHudHoldFrames; // the HUD script only hides its meter / shows the notice while 0x100 is clear
    return true;
}
inline void lawPublish(std::uintptr_t globals) noexcept {
    std::int32_t v{};float meter{};std::int64_t pending{};
    if(rd(lawWord(globals,lawWantedWord),0,v))wantedLevel.store(v,std::memory_order_relaxed);
    if(rd(lawWord(globals,lawWanted2Word),0,v))wantedFlag2.store(v,std::memory_order_relaxed);
    if(rd(lawWord(globals,lawHudStateWord),0,v))wantedHudState.store(v,std::memory_order_relaxed);
    if(rd(lawWord(globals,lawSceneWord),0,v))wantedScene.store(v,std::memory_order_relaxed);
    if(rd(lawWord(globals,lawHudMeterWord),0,meter)&&std::isfinite(meter))wantedHudMeter.store(meter,std::memory_order_relaxed);
    const auto total=lawBounties(globals,false,&pending);
    wantedBounty.store(int(std::clamp<std::int64_t>(total,-1,1000000000)),std::memory_order_relaxed);wantedPending.store(int(std::clamp<std::int64_t>(pending,-1,1000000000)),std::memory_order_relaxed);
}
// Every main tick, two reads while it matters; idle = nothing but the report refresh every maintainEveryFrames.
inline void maintainLaw() noexcept {
    if(!lawApi())return;
    auto& st=lawState;++st.ticks;
    const bool wantOn=want[ToggleNoWanted].load(std::memory_order_acquire);
    const bool refresh=st.ticks%maintainEveryFrames==0;
    if(!wantOn&&!st.noWantedSeen&&!st.pressHold&&!st.hudHold&&!refresh)return;
    std::uint32_t state{};
    if(!rd(api.gameData,0,state)||state!=3){wantedLevel.store(-1,std::memory_order_relaxed);wantedBounty.store(-1,std::memory_order_relaxed);active[ToggleNoWanted].store(false);return;}
    const auto globals=lawGlobals();if(!globals){wantedLevel.store(-1,std::memory_order_relaxed);active[ToggleNoWanted].store(false);return;}
    const auto dogs=lawWord(globals,lawDogsWord);
    std::int32_t wanted{},witness{};std::uint32_t flags{};
    if(!rd(lawWord(globals,lawWantedWord),0,wanted)||!rd(lawWord(globals,lawWanted2Word),0,witness)||!rd(dogs,lawDogsFlags,flags)){wantedError.store("LAW READ FAILED",std::memory_order_relaxed);return;}
    // The bounty is read every refresh only: a raise of the flag is caught at once, a bounty alone may wait a quarter second.
    const bool bounty=refresh&&(wantOn||st.pressHold)&&lawBounties(globals,false)!=0;
    // Switching on also ends what is already under way: a crime reported before the switch would raise the flag the moment 0x100 is lifted.
    const bool rising=wantOn&&!st.noWantedSeen,falling=!wantOn&&st.noWantedSeen;
    if((rising||((wantOn||st.pressHold)&&(wanted||witness||bounty)))&&!lawBusy(globals)){
        bool wasWanted{};std::int64_t hadBounty{};
        if(lawClear(globals,wasWanted,hadBounty)){
            if(wantOn&&!rising)wantedBlockedRaises.fetch_add(1,std::memory_order_relaxed);
            if(st.pressHold)wantedReraised.fetch_add(1,std::memory_order_relaxed);
        }
        if(!rd(dogs,lawDogsFlags,flags))return;
        if(wantOn)st.noWantedSeen=true;
    }
    if(falling)st.noWantedSeen=false;
    if(st.pressHold)--st.pressHold;
    const bool hud=st.hudHold>0;if(hud)--st.hudHold;
    const bool gate=wantOn&&api.lawGate;
    // Only 0x100 the mod set itself (marker) is ever taken back; without the cheat a marker from an old save heals here.
    std::uint32_t next=flags;
    if((flags&lawMarkerBit)&&(!gate||hud))next&=~lawOffBit;
    if(!gate&&(flags&lawMarkerBit))next&=~lawMarkerBit;
    if(gate&&!hud&&!(flags&lawOffBit))next|=lawOffBit|lawMarkerBit;
    if(falling)next|=lawChaseEndBit; // A crime from the moments 0x100 was lifted is not left to raise the flag now
    if(next!=flags&&wr(dogs,lawDogsFlags,next)){
        if(next&lawOffBit)wantedGateSets.fetch_add(1,std::memory_order_relaxed);
        else wantedGateReleases.fetch_add(1,std::memory_order_relaxed);
        flags=next;
    }
    wantedFlags.store(flags,std::memory_order_relaxed);
    active[ToggleNoWanted].store(wantOn&&(!api.lawGate||hud||(flags&lawOffBit)));
    if(refresh)lawPublish(globals);
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
// DEAD EYE (VrDeadEye.h): every main tick. Not running: a 2-read probe on the last validated player (a full sample every
// maintainEveryFrames); running: a full sample every tick (Dead Eye lasts seconds), publishes the state for the mapper / XR tint and
// applies the HALF drain through the engine's own SetDeadEyePoints (only on the player this tick just validated).
inline std::uintptr_t deadEyeProbePlayer{};inline ::rdrvr::vr::deadeye::Drain deadEyeDrain{};inline std::uint64_t deadEyeTicks{};
inline void maintainDeadEyeMeter() noexcept {
    namespace de=::rdrvr::vr::deadeye;
    ++deadEyeTicks;
    if(!de::active.load(std::memory_order_relaxed)&&deadEyeTicks%maintainEveryFrames){
        std::uintptr_t states{};std::uint32_t eye{};
        if(!deadEyeProbePlayer||!rd(deadEyeProbePlayer,playerStates,states)||!states||!rd(states,statesDeadEye,eye)||!eye)return;
    }
    Player p{};
    if(sample(p)){deadEyeProbePlayer=0;deadEyeDrain={};de::observeNone();return;}
    deadEyeProbePlayer=p.player;
    std::uintptr_t states{};std::uint32_t eye{};std::int32_t lvl{-1};float pts{-1.f},max{-1.f};std::uint8_t inf{};
    if(!rd(p.player,playerStates,states)||!states||!rd(states,statesDeadEye,eye)){de::bump(de::sampleFailures);de::observeNone();deadEyeDrain={};return;}
    if(!rd(states,statesDeadEyeLevel,lvl))lvl=-1;
    if(!rd(p.player,playerDeadEyePoints,pts))pts=-1.f;
    if(!rd(p.player,playerDeadEyeMax,max))max=-1.f;
    const bool on=eye!=0&&p.hp>0;
    de::observe(on,int(lvl),pts,max);
    const bool infinite=rd(p.player,playerInfiniteDeadEye,inf)&&inf;
    if(on&&infinite)de::bump(de::drainInfinite);
    const float target=pts>=0.f?de::drainTarget(deadEyeDrain,on,pts,infinite,de::halfTime.load(std::memory_order_relaxed)):-1.f;
    if(target>=0.f&&std::isfinite(target)&&api.fn.deadEyePoints){
        api.fn.deadEyePoints(reinterpret_cast<void*>(p.player),target,false);engineCalls.fetch_add(1,std::memory_order_relaxed);de::bump(de::drainCalls);
        float after{};
        if(rd(p.player,playerDeadEyePoints,after)&&std::isfinite(after)){
            if(after<pts)de::drainedExtra.store(de::drainedExtra.load(std::memory_order_relaxed)+(pts-after),std::memory_order_relaxed);
            deadEyeDrain.prev=after;de::points.store(after,std::memory_order_relaxed);}
    }
}
// DEAD EYE MODE SLOW-MO (VrDeadEye.h): every main tick, after maintainDeadEyeMeter. Nothing wanted and nothing held = no read.
// A cheap 3-byte probe on the last fully validated player answers the steady case (byte already as wanted); every WRITE happens only
// right after a full sample() of that same player (never on a stale pointer). The value the hold replaced (the story's 1) is written
// back when the hold ends (gun out of the hands and Dead Eye over, GAME, a duel), only if the byte still holds the mod's 0; a new
// sagPlayer is never written for the old one (its constructor / ResetData store their own 1).
// Gated by the verified SetDeadEyePoints print (the same sagPlayer Dead Eye layout the meter reads); without it SLOW-MO stays inert.
// Known limit: the script command SET_DEADEYE_MULTILOCK_ENABLE (sagActorScript 0x1cb34ac) can store 0 while the byte
// is held at 0 - invisible here, so the restore gives the saved 1 back. Only GAME / a duel ever read it afterwards (a duel is level 2
// anyway) and the next script call / ResetData sets it again; nothing saves it.
struct SlowMoHold {std::uintptr_t player{};std::uint8_t saved{1};bool on{};};
inline SlowMoHold slowMoHold{};inline std::uintptr_t slowMoPlayer{};
inline void maintainSlowMo() noexcept {
    namespace de=::rdrvr::vr::deadeye;
    const bool running=de::active.load(std::memory_order_relaxed);
    // the mode comes from the store itself, not from the holster pad filter's publish (debug.rdrvr.vr.holsters 0 installs no
    // such filter -> the atomic keeps its SLOW-MO default and GAME would get writes). Before vr.cfg is loaded, or with the row's
    // feature killed (the row reads PROP), nothing is written = the game's own level.
    namespace vs=::rdrvr::vr;
    const bool chosen=vs::vrSettings.loaded()&&!vs::keyKilled(vs::Key::DeadEyeMode)&&vs::vrSettings.requested().deadEyeSlowMo();
    de::slowMo.store(chosen,std::memory_order_relaxed);
    const bool mode=chosen&&api.fn.deadEyePoints;
    const bool candidate=mode&&(running||de::gunInHand.load(std::memory_order_relaxed));
    if(running&&slowMoHold.on)de::bump(de::slowMoRunningTicks);
    if(!candidate&&!slowMoHold.on){slowMoPlayer=0;return;}
    if(slowMoPlayer){ // steady case: the byte already says what we want -> nothing to write
        std::uintptr_t table{};std::uint8_t on{},ml{},flags{};
        if(rd(slowMoPlayer,0,table)&&table==api.playerTable&&rd(slowMoPlayer,playerActive,on)&&on==1&&
           rd(slowMoPlayer,playerDeadEyeMultiLock,ml)&&rd(slowMoPlayer,playerDeadEyeFlags,flags)&&ml<=1){
            const bool hold=de::slowMoWanted(mode,de::gunInHand.load(std::memory_order_relaxed),running,(flags&deadEyeDuelBit)!=0);
            de::multiLock.store(ml,std::memory_order_relaxed);
            if(hold&&ml==0)return;           // held (or the story's own level 1: nothing to hold, nothing to give back)
            if(!hold&&!slowMoHold.on)return; // a duel with nothing held
        }
    }
    Player p{};
    // no valid player (loading / death screen): no write; the hold record stays, so the SAME player validating again gets its byte back
    if(sample(p)){slowMoPlayer=0;de::multiLock.store(-1,std::memory_order_relaxed);return;}
    if(slowMoHold.on&&slowMoHold.player!=p.player){slowMoHold={};de::bump(de::slowMoDropped);} // another sagPlayer: its own byte
    slowMoPlayer=p.player;
    std::uint8_t ml{},flags{};
    if(!rd(p.player,playerDeadEyeMultiLock,ml)||!rd(p.player,playerDeadEyeFlags,flags)){de::bump(de::sampleFailures);return;}
    if(ml>1){ // not the bool the engine stores: layout mismatch -> never write
        de::bump(de::slowMoBadValue);slowMoHold={};slowMoPlayer=0;de::slowMoHeld.store(false,std::memory_order_relaxed);
        de::multiLock.store(int(ml),std::memory_order_relaxed);return;}
    const bool dueling=(flags&deadEyeDuelBit)!=0;
    if(dueling&&candidate)de::bump(de::slowMoDuel);
    const bool hold=de::slowMoWanted(mode,de::gunInHand.load(std::memory_order_relaxed),running,dueling);
    // never switch the level under a level-2 execute (eye state 3 = shooting the marks; level 1 has no state 3 handler): wait it out
    std::uintptr_t states{};std::uint32_t eye{};
    const bool executing=rd(p.player,playerStates,states)&&states&&rd(states,statesDeadEye,eye)&&eye==3;
    if(hold&&ml!=0&&executing)de::bump(de::slowMoExecuteWaits);
    else if(hold&&ml!=0){
        if(wr(p.player,playerDeadEyeMultiLock,std::uint8_t{0})){
            de::bump(slowMoHold.on?de::slowMoReasserts:de::slowMoHolds);
            slowMoHold.saved=ml;slowMoHold.player=p.player;slowMoHold.on=true;ml=0;}
    }else if(!hold&&slowMoHold.on){
        if(ml==0&&wr(p.player,playerDeadEyeMultiLock,slowMoHold.saved))ml=slowMoHold.saved;
        de::bump(de::slowMoRestores);slowMoHold={};
    }
    de::slowMoHeld.store(slowMoHold.on,std::memory_order_relaxed);de::multiLock.store(int(ml),std::memory_order_relaxed);
}
} // namespace detail

// ---- main thread (VrMainThread module 2) ----
inline void tick(const mt::Frame& frame) noexcept {
    using namespace detail;
    if(!engineReady.load(std::memory_order_acquire))return;
    try{
        ++owned.frame;
        maintainDeadEyeMeter(); // every tick (before the maintain throttle)
        maintainSlowMo();       // DEAD EYE MODE SLOW-MO: every tick (the hold must be in place before the R3 press)
        // DUEL ON LOOK: every 8th tick while the row is ON or its script runs (no reads otherwise)
        if(!(frame.index&7u)&&duel::available.load(std::memory_order_relaxed)&&(duellink::enabled.load(std::memory_order_relaxed)||duel::scriptRunning.load(std::memory_order_relaxed))){
            Player dp{};const bool dok=!sample(dp);duel::tick(frame.now,frame.index,dp.gameState==3,dok?dp.actor:0,dp.guid);}
        maintainLaw();          // NO WANTED: a raise is answered in the tick it is seen
        const bool wake=wakeups.exchange(0,std::memory_order_acq_rel)!=0;
        const bool storm=nowWeather.load(std::memory_order_relaxed)==weatherStormy||weatherOwned.state==weatherStormy;
        if(!wake&&!owned.allActive&&owned.frame%maintainEveryFrames&&frame.index<owned.nextList){if(storm)sampleLightning();return;} // No missed strikes
        Player p{};const char* reason=sample(p);const bool inGame=p.gameState==3;const bool ok=!reason;
        maintainWeather(inGame);   // Before publishing, so NOW shows the forced state as soon as the engine applied it
        publishWorld(inGame);
        maintainGod(p,ok);maintainDeadEye(p,ok);maintainAmmo(p,ok);maintainFreeze(inGame);maintainStamina(p,ok);
        // every ~2 s in game (0.25 s while the menu is open): locks, owned and holstered weapons change with the story
        if(inGame&&frame.index>=owned.nextList){owned.nextList=frame.index+(::rdrvr::vr::menuOpen.load(std::memory_order_relaxed)?15:120);
            if(ok&&p.hp>0)clearBadPending(p.inventory); // SAVE GUARD (before the delete inside the list build)
            buildWeaponList(ok&&p.hp>0?p.actor:0);}
        if(owned.allActive){if(ok&&p.hp>0)giveNext(p);else{owned.allActive=false;result(RowAll,reason?reason:"NOT ALIVE",false);}}
    }catch(...){exceptions.fetch_add(1,std::memory_order_relaxed);}
}
inline void execute(const mt::Command& c,const mt::Frame& frame) noexcept {
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
    case OpDuel:{ // DUEL ON LOOK (VrDuelStart.h)
        if(reason)return refuse(RowDuel,duel::refuseEarly(frame.now,reason));
        // the player's sidearm by the names of the weapon list (no list yet: nothing is given to him)
        const auto duelList=weaponView();bool armed=!duelList.n||!api.fn.hasWeapon;
        for(unsigned i=0;i<duelList.n&&!armed;++i)armed=duelList[i].group==GroupSidearm&&api.fn.hasWeapon(reinterpret_cast<const void*>(p.actor),duelList[i].id);
        const char* why=duel::press(frame.now,p.player,p.actor,p.guid,p.hp,deadEyeRunning(p),armed,api.fn.giveWeapon,api.fn.hasWeapon);
        if(why)return refuse(RowDuel,why);
        return result(RowDuel,"DUEL STARTING",true);}
    case OpClearWanted:{
        wantedClearPresses.fetch_add(1,std::memory_order_relaxed);
        if(!lawApi())return refuse(RowClearWanted,"UNAVAILABLE");
        if(!inGame)return refuse(RowClearWanted,reason?reason:"NOT IN GAME");
        const auto globals=lawGlobals();
        if(!globals)return refuse(RowClearWanted,wantedError.load(std::memory_order_relaxed));
        if(lawBusy(globals))return refuse(RowClearWanted,"IN CUTSCENE - AGAIN AFTER IT");
        bool wasWanted{};std::int64_t hadBounty{};
        const bool any=lawClear(globals,wasWanted,hadBounty);
        lawState.pressHold=lawPressHoldFrames; // a raise racing the press is answered too (report reraised)
        lawPublish(globals);
        if(!any)return result(RowClearWanted,"NOTHING TO CLEAR",true);
        static char msg[48];
        if(wasWanted&&hadBounty)std::snprintf(msg,sizeof msg,"WANTED CLEARED  BOUNTY %lld GONE",(long long)hadBounty);
        else if(wasWanted)std::snprintf(msg,sizeof msg,"WANTED CLEARED");
        else if(hadBounty)std::snprintf(msg,sizeof msg,"BOUNTY %lld CLEARED",(long long)hadBounty);
        else std::snprintf(msg,sizeof msg,"WITNESS STOPPED");
        ::rdrvr::vr::panelSanitize(msg);
        return result(RowClearWanted,msg,true);}
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
    return r==RowGod?ToggleGod:r==RowDeadEye?ToggleDeadEye:r==RowAmmo?ToggleAmmo:r==RowFreeze?ToggleFreeze:r==RowNoWanted?ToggleNoWanted:ToggleStamina;}
inline bool isToggle(Row r) noexcept {return r==RowGod||r==RowDeadEye||r==RowAmmo||r==RowFreeze||r==RowStamina||r==RowNoWanted;}
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
    case RowDuel:duel::rowText(out,size);break;
    case RowClearWanted:{const int level=wantedLevel.load(std::memory_order_relaxed),bounty=wantedBounty.load(std::memory_order_relaxed);
        if(level<0)std::snprintf(out,size,"PRESS A");
        else if(level>0)std::snprintf(out,size,"WANTED  BOUNTY %d  PRESS A",std::max(bounty,0));
        else if(bounty>0)std::snprintf(out,size,"BOUNTY %d  PRESS A",bounty);
        else if(wantedFlag2.load(std::memory_order_relaxed)>0)std::snprintf(out,size,"WITNESS  PRESS A");
        else std::snprintf(out,size,"NOT WANTED  PRESS A");
        break;}
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
        std::snprintf(out,size,"%s",w?(a?"ON":"ON - PENDING"):(a&&r!=RowStamina&&r!=RowNoWanted?"OFF - GAME ON":"OFF"));
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
    case RowDuel:duel::toggle(direction>0);break;
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
    case RowDuel:duel::toggle(!duellink::enabled.load(std::memory_order_acquire));break;
    case RowClearWanted:post(OpClearWanted,0);break;
    default:if(isToggle(r)){auto& w=want[toggleOf(r)];w.store(!w.load(std::memory_order_acquire),std::memory_order_release);wakeups.fetch_add(1);
        ::rdrvr::vr::cheatNote.store(nullptr,std::memory_order_release);}break;
    }
}
} // namespace rows

// ---- SAVE GUARD 2: invInventoryComponent::AddItem watcher (JUMP_SLOT 0x2e48600 -> 0x26615b8) ----
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
        ::rdrvr::vr::panelSanitize(label);::rdrvr::vr::appendRow(out,r==selected,label,value,::rdrvr::vr::pageInks[::rdrvr::vr::PageGiveWeapons]);});
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
        ::rdrvr::vr::panelSanitize(value);::rdrvr::vr::appendRow(out,r==selected,holsterLabels[r],value,::rdrvr::vr::pageInks[::rdrvr::vr::PageHolsters]);
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
    rowAvailable[RowDuel].store(player&&f.giveWeapon&&f.hasWeapon&&duel::available.load(std::memory_order_acquire));
    rowAvailable[RowClearWanted].store(player&&detail::lawApi());rowAvailable[RowNoWanted].store(player&&detail::lawApi());
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
    duel::install(api.read,base,api.actorManagerGlobal,api.actorSlotsGlobal,got); // DUEL ON LOOK: its own prints, UNAVAILABLE if any differs
    // LAW: the global block's addressing, then each engine call with the script native that shows its arguments.
    if(code(vmGlobalRefOffset,vmGlobalRefFingerprint)&&code(vmGlobalStoreOffset,vmGlobalStoreFingerprint)&&code(vmArrayElementOffset,vmArrayElementFingerprint)&&
       code(vmIaddU8Offset,vmIaddU8Fingerprint)&&code(globalsHashFnOffset,globalsHashFingerprint)&&got(scriptGlobalsGotOffset,scriptGlobalsOffset)&&
       got(scriptGlobalSizeGotOffset,scriptGlobalSizeOffset)){
        api.scriptGlobals=base+scriptGlobalsOffset;api.scriptGlobalSize=base+scriptGlobalSizeOffset;api.scriptGlobalsHash=base+scriptGlobalsHashOffset;}
    if(got(factionMgrGotOffset,factionMgrOffset)&&code(nativeSetStatusOffset,nativeSetStatusFingerprint)){
        api.factionMgr=base+factionMgrOffset;
        fn(isFactionValidOffset,isFactionValidFingerprint,f.isFactionValid);fn(setStatusTwoWayOffset,setStatusTwoWayFingerprint,f.setStatus);}
    if(got(scriptGuidCountGotOffset,scriptGuidCountOffset)&&got(scriptGuidSlotsGotOffset,scriptGuidSlotsOffset)&&
       code(nativeTerminateOffset,nativeTerminateFingerprint)&&code(nativeScriptValidOffset,nativeScriptValidFingerprint)){
        api.scriptGuidCount=base+scriptGuidCountOffset;api.scriptGuidSlots=base+scriptGuidSlotsOffset;
        fn(terminateChildrenOffset,terminateChildrenFingerprint,f.terminateChildren);fn(addToTerminationOffset,addToTerminationFingerprint,f.addToTermination);}
#ifdef __ANDROID__
    {char v[PROP_VALUE_MAX]{};api.lawGate=!(__system_property_get("debug.rdrvr.vr.wantedgate",v)>0&&v[0]=='0');}
#endif
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
    { // "wanted" = CLEAR WANTED / NO WANTED (LAW block at the top)
        char lb[768];
        std::snprintf(lb,sizeof lb,",\"wanted\":{\"api\":%s,\"clearPresses\":%llu,\"cleared\":%llu,\"noWanted\":%s,\"blockedRaises\":%llu,\"reraised\":%llu,"
            "\"level\":%d,\"bounty\":%d,\"pending\":%d,\"flag2\":%d,\"scene\":%d,\"hudState\":%d,\"hudMeter\":%.3f,\"lawFlags\":\"0x%08x\",\"gate\":%s,\"gateSets\":%llu,"
            "\"gateReleases\":%llu,\"childEnds\":%llu,\"factionCalls\":%llu,\"globalWords\":%d,\"signature\":\"0x%08x\",\"lastError\":\"%s\"}",
            detail::lawApi()?"true":"false",(unsigned long long)wantedClearPresses.load(),(unsigned long long)wantedCleared.load(),want[ToggleNoWanted].load()?"true":"false",
            (unsigned long long)wantedBlockedRaises.load(),(unsigned long long)wantedReraised.load(),wantedLevel.load(),wantedBounty.load(),wantedPending.load(),wantedFlag2.load(),wantedScene.load(),
            wantedHudState.load(),double(wantedHudMeter.load()),wantedFlags.load(),detail::api.lawGate?"true":"false",(unsigned long long)wantedGateSets.load(),
            (unsigned long long)wantedGateReleases.load(),(unsigned long long)wantedChildEnds.load(),(unsigned long long)wantedFactionCalls.load(),wantedGlobalWords.load(),
            wantedSignature.load(),wantedError.load());
        out.insert(out.size()-1,lb);
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
    names+=",\"duelStart\":"+duel::report();
    names+=",\"weaponNames\":[";
    for(unsigned i=0;i<w.n;++i){const auto& e=w[i];if(i)names+=',';names+="[";names+=std::to_string(e.id);names+=",\"";names+=e.name;names+="\",";
        names+=std::to_string(e.group);names+=e.locked?",1":",0";names+=e.owned?",1":",0";names+=e.ready?",1]":",0]";}
    names+="]";
    out.insert(out.size()-1,names);
    return out;
}
} // namespace rdrvr::vr::cheats
