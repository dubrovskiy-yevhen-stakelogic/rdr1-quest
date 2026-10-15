#pragma once
// MELEE - physical VR melee for RDR1 (Gothic II VR port). Pure math (Gothic swing / contact,
// capsules, damage, hlthMsgInjure) lives in VrMeleeMath.h.
//
// Threads:
//  - XR thread   pushSample(ControllerSample): every locateHands sample into a lock-free tape (128 entries, LOCAL metres, both hands)
//  - main thread tick() via VrMainThread (ModuleMelee, the sagShellDerived::UpdateInputs wrapper: frame N's final pose, physics idle):
//                drains the tape, gates (menu / player state / Dead Eye / riding / cover / ...), measures the swing per tape sample in
//                OpenXR LOCAL (raw tracking + head-relative body frame = Gothic's two samples), maps the blade/fist into the world with
//                the newest camera plan (GameXr worldMapFn), scans actors (slot table, batched guarded reads), builds bone capsules,
//                runs Gothic's occupancy + swept blade / fist rays, and applies at most one strike per hand per tick through the
//                engine: aniMelee::StrikeDamageRemote (damage + AI + GOH 7 + strike audio + hearing + knife/punch FX) when the hit
//                component is VERIFIED (engine probe SweptSphereOneObject on that victim or the frag child of the capsule's bone, bounds-
//                checked against fragType+0x1b3: GetHitEffect / SetFromTestProbe index the frag children WITHOUT a check),
//                else aActor::Broadcast(hlthMsgInjure) + rdr2GOH::AddNewEvent(7) + MeleeAudio::DoStrikeAudio (no FX).
//  - packet hook sagPlayerInputFoot::SetupActionTreePacket (GOT 0x2e53710; caller's thread): after the original, STRIKE (+0x98) and
//                FIRE (+0x3c) are cleared while a knife or an armed (closed, empty) fist is in a VR hand; latched until released.
//  - any thread  report() ("vr":{"melee":...}), status note for the VR page row MELEE.
// Never: flag 0x80000 (instant kill past unkillable); damage to invulnerable (health+0x150 bit 0x4) victims; engine calls off the main
// thread. Kill switch debug.rdrvr.vr.melee 0 (no hook, no tick, row PROP, MELEE forced NATIVE). Every engine address is verified
// (whole-function prints, GOT / vtable words) in install(); any failure leaves melee disabled (row UNAVAILABLE) and nothing else.
#include "VrMeleeMath.h"
#include "VrHolsters.h"
#include "VrSettings.h"
#include "diagnostics/GuardedReadVector.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif
#if defined(__GNUC__)
#define RDRVR_MELEE_NOINLINE __attribute__((noinline))
#else
#define RDRVR_MELEE_NOINLINE
#endif

namespace rdrvr::vr::melee {
namespace M=::rdrvr::vr::melee::math;
namespace H=::rdrvr::vr::holsters;
namespace mt=::rdrvr::vr::mainthread;
using M::Vec3;

// ============================== engine facts (librdr.so 1.58, vaddr; .text file offset = vaddr - 0x4000) ==============================
inline constexpr std::uintptr_t strikeDamageRemoteOffset=0x1e401a0, // aniMelee::StrikeDamageRemote(sagActor& V, sagActor& A, hlthMsgInjure&, Vector3&, bool, u16 comp, u16, bool evt7)
    broadcastInjureOffset=0x1e48a1c,     // rage::aActor::Broadcast(hlthMsgInjure const&)          x0 victim, x1 msg
    broadcastImpulseOffset=0x1e48c98,    // rage::aActor::Broadcast(projMsgApplyImpulse const&)    (M2)
    addNewEventOffset=0x24749f8,         // rdr2GOH::AddNewEvent(gohGUID, gohGUID, eGOHEventType)
    doStrikeAudioOffset=0x2311694,       // MeleeAudio::DoStrikeAudio(sagActor&, sagActor&, Vector3 const&)
    setFromTestProbeOffset=0x1e82374,    // hlthHitInfo::SetFromTestProbe(phIntersection const&, sagActor const&) (x0 = msg+0x10)
    sweptSphereOneOffset=0x217b098,      // SweptSphereOneObject(phSegment const&, float, int, phIntersection*, phInst const&) (world in/out)
    sweptSphereMultipleOffset=0x217af74, // SweptSphereMultipleExcludeInsts(seg, r, max, isect*, nExcl, excl, include, typeExclude, u8 state)
    weaponBaseTypeOffset=0x1e3f23c,      // aniMelee::GetWeaponBaseTypeForActor(sagActor const&) -> type (int16 enum at +8)
    doKnifeFxOffset=0x1e43154,           // DUAL: aniMelee::DoKnifeFX(hlthMsgInjure*, sagActor& A, sagActor& V, int comp) (x0 unread: 0x1e431b0 mov x0,x1)
    setupActionTreePacketOffset=0x26b65ec,setupActionTreePacketGotOffset=0x2e53710, // sagPlayerInputFoot::SetupActionTreePacket (JUMP_SLOT)
    hitEffectIndexOffset=0x1e559e4,      // sagCreatureComponent::GetHitEffect: frag child indexed by the component, no bound
    instSkeletonOffset=0x2913174,fragSkeletonOffset=0x1ea3ddc, // entEntityInst::GetSkeleton / rage::fragInst::GetSkeleton (replicated)
    receiveStrikeOffset=0x1e3dacc,       // aniMelee::ReceiveStrike = str w1,[x0,#0x550] (M2 reaction field)
    packetTablesOffset=0x12e8600,        // .rodata: SetupActionTreePacket input -> intention tables (Melee 13 -> 38, Fire 9 -> 15)
    // FIST BLOCK, both optional (a mismatch only disables FIST BLOCK):
    //  aniMelee::SetBlockState(bool) 0x1e3fc28 (whole, 56 B): +0x55c bit 0x80 = on, +0x134 = block start time (MeleeBlockStateTask Begin /
    //   TaskEnd are its only callers). Every attacker's MeleeMachine tests the TARGET's bit 0x80 (or +0xc0 == 2) at 0x1e41de8..0x1e41df8:
    //   blocked -> the target's block meter +0x130 += attacker +0x124 (> actionData+0x10c breaks the guard), SetBlockedImpact (target
    //   +0x55c |= 0x40), ApplyKnockback, Broadcast(animMsgMeleeBlocked), branch past the damage (0x1e41ee8 -> +0x1314).
    //  aniMelee::DeScheduleBlock() 0x1e3ff38 (whole, 88 B): clears +0x55c bit 0x40 (and a scheduled block anim when +0xc0 == 2: never called then).
    setBlockStateOffset=0x1e3fc28,deScheduleBlockOffset=0x1e3ff38;
inline constexpr std::uintptr_t actorManagerOffset=0x4e6f6b40,actorManagerGotOffset=0x2deaba8,actorSlotsOffset=0x4e6f6b38,actorSlotsGotOffset=0x2deabb0,
    lethalPunchOffset=0x4e70cfa1,lethalPunchGotOffset=0x2deabd8,ddaLevelOffset=0x4e70cfa4,ddaLevelGotOffset=0x2deabf8,
    aniMeleeVtableOffset=0x2d06bc0,aniMeleeVtableGotOffset=0x2def698,healthVtableOffset=0x2d08630,healthVtableGotOffset=0x2deabc0,
    entityVtableOffset=0x2dbf008,entityVtableGotOffset=0x2e0db80,creatureVtableOffset=0x2d07970,creatureVtableGotOffset=0x2deb138,
    footVtableOffset=0x2d8e1d8,footVtableGotOffset=0x2e081a0,
    isCreatureSlotOffset=0x2d07a48,isCreatureOffset=0x1e56d50,       // sagCreatureComponent vtable +0x10+0xc8 (ABS64) -> IsCreature
    instGetSkeletonSlotOffset=0x2dbf308,                              // entEntityInst vtable +0x10+0x1f0 (ABS64) -> instSkeletonOffset
    footGetInputSlotOffset=0x2d8e248,getInputOffset=0x26a7ce8;        // sagPlayerInputFoot vtable +0x10+0x60 -> sagPlayerInput::GetInput
inline constexpr std::uintptr_t vtableAddressPoint=0x10;
// Whole functions (small) or the indexing site (GetHitEffect); packet tables = .rodata (R segment, file offset = vaddr).
inline constexpr std::array<unsigned char,348> strikeDamageRemoteFingerprint{
#include "gamecode/VrMelee_1e401a0_348_8d.inc"
};
inline constexpr std::array<unsigned char,136> broadcastInjureFingerprint{
#include "gamecode/VrMelee_1e48a1c_136_8d.inc"
};
inline constexpr std::array<unsigned char,16> broadcastImpulseFingerprint{
#include "gamecode/VrMelee_1e48c98_16_8d.inc"
};
inline constexpr std::array<unsigned char,36> addNewEventFingerprint{
#include "gamecode/VrMelee_24749f8_36_8d.inc"
};
inline constexpr std::array<unsigned char,56> doStrikeAudioFingerprint{
#include "gamecode/VrMelee_2311694_56_8d.inc"
};
inline constexpr std::array<unsigned char,344> setFromTestProbeFingerprint{
#include "gamecode/VrMelee_1e82374_344_8d.inc"
};
inline constexpr std::array<unsigned char,212> sweptSphereOneFingerprint{
#include "gamecode/VrMelee_217b098_212_8d.inc"
};
inline constexpr std::array<unsigned char,292> sweptSphereMultipleFingerprint{
#include "gamecode/VrMelee_217af74_292_8d.inc"
};
inline constexpr std::array<unsigned char,60> weaponBaseTypeFingerprint{
#include "gamecode/VrMelee_1e3f23c_60_8d.inc"
};
inline constexpr std::array<unsigned char,64> setupActionTreePacketFingerprint{
#include "gamecode/VrMelee_26b65ec_64_8d.inc"
};
inline constexpr std::array<unsigned char,72> hitEffectIndexFingerprint{
#include "gamecode/VrMelee_1e559e4_72_8d.inc"
};
inline constexpr std::array<unsigned char,20> instSkeletonFingerprint{
#include "gamecode/VrMelee_2913174_20_8d.inc"
};
inline constexpr std::array<unsigned char,68> fragSkeletonFingerprint{
#include "gamecode/VrMelee_1ea3ddc_68_8d.inc"
};
inline constexpr std::array<unsigned char,8> receiveStrikeFingerprint{1,80,5,185,192,3,95,214};
// DUAL: 64-byte head of DoKnifeFX. Optional: a mismatch only drops the phantom knife's FX.
inline constexpr std::array<unsigned char,64> doKnifeFxFingerprint{
#include "gamecode/VrMelee_1e43154_64_8d.inc"
};
inline constexpr std::array<unsigned char,56> setBlockStateFingerprint{
#include "gamecode/VrMelee_1e3fc28_56_8d.inc"
};
inline constexpr std::array<unsigned char,88> deScheduleBlockFingerprint{
#include "gamecode/VrMelee_1e3ff38_88_8d.inc"
};
inline constexpr std::array<unsigned char,72> packetTablesFingerprint{
#include "gamecode/VrMelee_12e8600_72_8d.inc"
};

namespace off {
inline constexpr std::size_t actorGuid=0x8,actorAnim=0x38,actorEntity=0x50,actorHealth=0x60,actorMover=0x88,actorComponent=0xb0,actorFlags=0x118,
    actorGoh=0x188,actorBlock=0x190;
inline constexpr std::size_t compMatrix=0x18,compIsCreatureSlot=0xc8,compRiderLink=0x894,compMountLink=0x8a0,compRace=0xa30,moverInst=0x20;
inline constexpr std::size_t healthMax=0x1c,healthValue=0x20,healthKoMax=0x24,healthKo=0x2c,healthFlags=0x150,healthBlock=0x158;
inline constexpr std::uint32_t healthInvulnerable=0x4,healthDead=0x40,healthUnkillable=0x2000;
inline constexpr std::size_t entityInst=0x88,instTypeA=0x78,instCache=0x88,instTypeB=0x98,instIsFrag=0x190,instSkeletonSlot=0x1f0,
    cacheSkeleton=0x1a0,typeBits=0xc,typeChildren=0x100,typeChildCount=0x1b3,childBone=0x12;
inline constexpr std::uint32_t typeBitsResident=0x30000000;
inline constexpr std::size_t skelData=0x8,skelGlobals=0x28,dataBones=0x0,dataBoneCount=0x30,boneStride=0x110,matrixStride=0x40,matrixPos=0x30;
inline constexpr std::size_t animBlock=0xab8,blockMelee=0x80,meleeActionData=0x10,meleeAttackClass=0x138,meleeReaction=0x550,actionImpulse=0xd4;
inline constexpr std::size_t meleeFlags=0x55c,meleeBlockMeter=0x130,meleeBlockTime=0x134,meleeScheduled=0xc0;
inline constexpr std::uint16_t flagBlocking=0x80,flagBlockedImpact=0x40;
inline constexpr std::size_t pmPlayer=0xa20,playerFoot=0x118,playerStates=0x410,statesDeadEye=0x20,playerState=0x5d4,playerActive=0x5f1,
    playerGuid=0x5f4,playerBlock=0x5f8,footValues=0x20;
inline constexpr unsigned footInputs=30;
inline constexpr std::size_t isectSize=0x70,isectInst=0x0,isectLevel=0x8,isectPos=0x10,isectNormal=0x20,isectMark=0x64,isectComponent=0x6a,segSize=0x20;
inline constexpr unsigned packetStrike=38,packetFire=15,packetInts=49;
} // namespace off
inline constexpr int kindMelee=10; // engine::WeaponClass::Melee (== holsters::kindMelee)
static_assert(kindMelee==H::kindMelee);

// ============================== engine API ==============================
using ReadFn=bool(*)(void*,std::uintptr_t,void*,std::size_t);
using StrikeFn=void(*)(void* self,void* victim,void* attacker,void* msg,void* hitPos,bool unused,std::uint16_t component,std::uint16_t unused2,bool event7);
using BroadcastFn=void(*)(void* victim,const void* message);
using AddEventFn=void(*)(std::uint32_t victimGoh,std::uint32_t attackerGoh,int type);
using StrikeAudioFn=void(*)(void* attacker,void* victim,const void* position);
using HitInfoFn=void(*)(void* hitInfo,const void* intersection,const void* victim);
using ProbeOneFn=int(*)(const void* segment,float radius,int maxHits,void* intersections,const void* inst);
using ProbeLevelFn=int(*)(const void* segment,float radius,int maxHits,void* intersections,int excludeCount,const void* const* exclude,
                          int include,int typeExclude,std::uint8_t state);
using WeaponTypeFn=const void*(*)(const void* actor);
using SetupPacketFn=void(*)(const void* foot,void* packet,bool full);
// DUAL: StrikeDamageRemote 0x1e4029c..0x1e402ac calls it as (x1 msg, x2 attacker, x3 victim, w4 component); x0 is not read.
using KnifeFxFn=void(*)(void* self,void* msg,void* attacker,void* victim,int component);
using SetBlockStateFn=void(*)(void* self,bool on);   // aniMelee::SetBlockState(bool)
using DeScheduleBlockFn=void(*)(void* self);         // aniMelee::DeScheduleBlock()
struct EngineApi {
    ReadFn read{};std::uintptr_t base{};
    std::uintptr_t gameData{},playerManager{},playerManagerTable{},playerTable{},actorManager{},actorSlots{};
    std::uintptr_t aniMeleeVtable{},healthVtable{},entityVtable{},isCreatureFn{},instSkeletonFn{},footVtable{},lethalPunch{},ddaLevel{};
    StrikeFn strike{};BroadcastFn broadcast{},impulse{};AddEventFn addEvent{};StrikeAudioFn audio{};HitInfoFn hitInfo{};
    ProbeOneFn probeOne{};ProbeLevelFn probeLevel{};WeaponTypeFn weaponType{};
    H::ContextFn context{};void* contextUser{};
    KnifeFxFn knifeFx{}; // DUAL, optional (not part of complete()): knife FX of a phantom knife (the engine item is another weapon)
    SetBlockStateFn setBlockState{};DeScheduleBlockFn deScheduleBlock{}; // FIST BLOCK, optional (null = FIST BLOCK unavailable)
    bool complete() const noexcept {
        return read&&gameData&&playerManager&&playerManagerTable&&playerTable&&actorManager&&actorSlots&&aniMeleeVtable&&healthVtable&&
            entityVtable&&isCreatureFn&&instSkeletonFn&&footVtable&&lethalPunch&&ddaLevel&&strike&&broadcast&&impulse&&addEvent&&audio&&hitInfo&&
            probeOne&&probeLevel&&weaponType;
    }
};

// ============================== configuration (startup properties; written before install, read-only after) ==============================
struct Config {
    bool m2{false},fx{true},wall{true},probe{true},dry{false},inputs{true},horses{false};
    float bladeBase{.03f},bladeTip{.27f},bladeRadius{.02f},bladeStep{.05f};   // knife segment along the aim ray from the grip (m) [H]
    float fistTip{.08f},fistRadius{.04f},fistOffset{.04f};                   // Gothic: tip = grip + aim*8 cm, 4 rays r 4 cm
    float reach{2.5f},push{20.f},victimCooldownMs{150.f};
    M::DamageConfig damage{};
};
inline Config config{};
inline std::atomic<bool> started{};
inline std::string property(const char* name) {return H::property(name);}
// "a b c ..." -> up to n floats; false on garbage / range.
inline bool parseFloats(const std::string& text,float* out,int n,float lo,float hi) noexcept {
    if(text.empty())return false;
    const char* p=text.c_str();float v[8]{};int k=0;
    while(*p&&k<n){char* end=nullptr;const float x=std::strtof(p,&end);if(end==p)return false;if(!std::isfinite(x)||x<lo||x>hi)return false;
        v[k++]=x;p=end;while(*p==' '||*p==','||*p==';')++p;}
    if(k!=n||*p)return false;
    for(int i=0;i<n;++i)out[i]=v[i];return true;
}
// debug.rdrvr.vr.melee.{m2,fx,wall,probe,dry,inputs,horses} (0/1), .blade "base tip radius", .fist "tip radius", .step <m>,
// .reach <m>, .push <k>, .dmg "torso limb fistKo fistKoStrong", .cooldown <ms>.
inline void startup() noexcept {
    if(started.exchange(true))return;
    Config c{};
    const auto flag=[](const char* name,bool fallback){const auto v=property(name);return v.empty()?fallback:v[0]!='0';};
    c.m2=flag("debug.rdrvr.vr.melee.m2",false);c.fx=flag("debug.rdrvr.vr.melee.fx",true);c.wall=flag("debug.rdrvr.vr.melee.wall",true);
    c.probe=flag("debug.rdrvr.vr.melee.probe",true);c.dry=flag("debug.rdrvr.vr.melee.dry",false);c.inputs=flag("debug.rdrvr.vr.melee.inputs",true);
    c.horses=flag("debug.rdrvr.vr.melee.horses",false);
    float b[3]{};if(parseFloats(property("debug.rdrvr.vr.melee.blade"),b,3,0.f,1.f)&&b[1]>b[0]+.02f){c.bladeBase=b[0];c.bladeTip=b[1];c.bladeRadius=std::max(b[2],.005f);}
    float f[2]{};if(parseFloats(property("debug.rdrvr.vr.melee.fist"),f,2,0.f,.3f)){c.fistTip=f[0];c.fistRadius=std::max(f[1],.01f);c.fistOffset=c.fistRadius;}
    float one{};if(parseFloats(property("debug.rdrvr.vr.melee.step"),&one,1,.01f,.2f))c.bladeStep=one;
    if(parseFloats(property("debug.rdrvr.vr.melee.reach"),&one,1,1.f,6.f))c.reach=one;
    if(parseFloats(property("debug.rdrvr.vr.melee.push"),&one,1,0.f,500.f))c.push=one;
    if(parseFloats(property("debug.rdrvr.vr.melee.cooldown"),&one,1,0.f,1000.f))c.victimCooldownMs=one;
    float d[4]{};if(parseFloats(property("debug.rdrvr.vr.melee.dmg"),d,4,0.f,5.f)){c.damage.torso=d[0];c.damage.limb=d[1];c.damage.fistKo=d[2];c.damage.fistKoStrong=d[3];}
    c.damage.backstab=c.m2;
    config=c;
    // FIST BLOCK: debug.rdrvr.vr.melee.block 0 = no guard detection / engine block (report vr.melee.block.killed; the row keeps its value)
    guard::killed.store(!flag("debug.rdrvr.vr.melee.block",true),std::memory_order_relaxed);
}

// ============================== XR tape (XR thread -> main thread) ==============================
struct TapeSample {
    std::uint64_t ns{},seq{},epoch{},space{};
    std::array<std::array<float,3>,2> grip{},forward{};std::array<float,2> grips{};std::uint32_t valid{};std::uint32_t pad{};
};
inline LockFreeQueue<TapeSample,128> tape{};
inline std::atomic<bool> tapeEnabled{};
inline std::atomic<std::uint64_t> tapePushed{},tapeDropped{},tapeLastNs{},tapeStale{},tapeConsumed{};
// XR thread, every locateHands sample (XrHarness, next to hands::pushPose). LOCAL metres; forward = aim -Z.
inline void pushSample(const runtime::ControllerSample& s) noexcept {
    if(!tapeEnabled.load(std::memory_order_relaxed))return;
    TapeSample t{};t.ns=s.publishedNs;t.seq=s.sequence;t.epoch=s.epoch;t.space=s.referenceSpace;
    if(s.focused&&s.active)for(unsigned h=0;h<2;++h){
        if(!H::detail::poseOk(s.grip[h])||!H::detail::poseOk(s.aim[h]))continue;
        const auto f=H::detail::forwardOf(s.aim[h]);
        t.grip[h]={s.grip[h].position[0],s.grip[h].position[1],s.grip[h].position[2]};t.forward[h]={f[0],f[1],f[2]};t.valid|=1u<<h;
    }
    for(unsigned h=0;h<2;++h)t.grips[h]=std::isfinite(s.grips[h])?s.grips[h]:0.f;
    if(tape.push(t)){tapePushed.fetch_add(1,std::memory_order_relaxed);tapeLastNs.store(t.ns,std::memory_order_relaxed);}
    else tapeDropped.fetch_add(1,std::memory_order_relaxed);
}

// ============================== LOCAL -> world (GameXr provides it from the newest camera plan) ==============================
// world = worldOrigin + R * (local - localOrigin) * units; R row-major. head = HMD eye 0 (LOCAL + world).
struct WorldMap {
    std::array<float,9> r{1,0,0,0,1,0,0,0,1};Vec3 localOrigin{},worldOrigin{},headLocal{},headForwardLocal{0,0,-1},headWorld{};float units{1};
    std::uint64_t epoch{},space{},stampNs{};bool valid{};
};
using WorldMapFn=bool(*)(WorldMap& out,std::uint64_t now) noexcept;
inline std::atomic<WorldMapFn> worldMapFn{};
inline Vec3 rotate(const WorldMap& m,Vec3 v) noexcept {
    return {m.r[0]*v.x+m.r[1]*v.y+m.r[2]*v.z,m.r[3]*v.x+m.r[4]*v.y+m.r[5]*v.z,m.r[6]*v.x+m.r[7]*v.y+m.r[8]*v.z};}
inline Vec3 toWorld(const WorldMap& m,Vec3 local) noexcept {return m.worldOrigin+rotate(m,local-m.localOrigin)*m.units;}
// Rotation angle between two maps (degrees) - a snap turn / teleport / new plan basis invalidates the world "previous" segments.
inline float mapTurnDegrees(const WorldMap& a,const WorldMap& b) noexcept {
    const float tr=a.r[0]*b.r[0]+a.r[3]*b.r[3]+a.r[6]*b.r[6]+a.r[1]*b.r[1]+a.r[4]*b.r[4]+a.r[7]*b.r[7]+a.r[2]*b.r[2]+a.r[5]*b.r[5]+a.r[8]*b.r[8];
    return std::acos(std::clamp((tr-1.f)*.5f,-1.f,1.f))*57.29578f;
}

// ============================== counters / report state ==============================
enum Gate : unsigned {GateOk,GateNative,GateNoEngine,GateNoExecutor,GateNoHolsters,GateMenu,GateNoPlayer,GateNotInGame,GateDead,GateLocked,
    GateDeadEye,GateRiding,GateVehicle,GateDuel,GateMountedWeapon,GateCover,GateLinked,GateGringo,GateNotOnFoot,GateContext,GateCount};
inline constexpr std::array<const char*,GateCount> gateNames{"OK","NATIVE","NO_ENGINE","NO_EXECUTOR","NO_HOLSTERS","MENU","NO_PLAYER",
    "NOT_IN_GAME","DEAD","CONTROL_LOCKED","DEADEYE","RIDING","VEHICLE","DUEL","MOUNTED_WEAPON","COVER","LINKED","GRINGO","NOT_ON_FOOT","CONTEXT"};
inline constexpr std::array<const char*,GateCount> gateNotes{"ACTIVE","NATIVE","UNAVAILABLE","UNAVAILABLE","NO HOLSTERS","MENU","NO PLAYER",
    "NOT IN GAME","DEAD","LOCKED","DEAD EYE","RIDING","VEHICLE","DUEL","MOUNTED GUN","COVER","LINKED","GRINGO","NOT ON FOOT","BLOCKED"};
enum Reject : unsigned {RejectSlow,RejectNoTravel,RejectInside,RejectDiscontinuous,RejectNoPlan,RejectWall,RejectCooldown,RejectSameVictim,
    RejectNoComponent,RejectProbeMiss,RejectInvulnerable,RejectDead,RejectStale,RejectNoSkeleton,RejectMessage,RejectCount};
inline constexpr std::array<const char*,RejectCount> rejectNames{"SLOW","NO_TRAVEL","INSIDE","DISCONTINUOUS","NO_PLAN","WALL","COOLDOWN",
    "SAME_VICTIM","NO_COMPONENT","PROBE_MISS","INVULNERABLE","DEAD","STALE_VICTIM","NO_SKELETON","MESSAGE"};
enum Skip : unsigned {SkipPlayer,SkipMount,SkipDead,SkipRace,SkipHorse,SkipNotCreature,SkipFar,SkipBadSlot,SkipUnreadable,SkipCount};
inline constexpr std::array<const char*,SkipCount> skipNames{"PLAYER","MOUNT","DEAD","RACE","HORSE","NOT_CREATURE","FAR","BAD_SLOT","UNREADABLE"};
enum class Mode : std::uint8_t {None,Blade,Fist};
inline const char* modeName(Mode m) noexcept {return m==Mode::Blade?"BLADE":m==Mode::Fist?"FIST":"NONE";}
enum HandReset : unsigned {ResetMode,ResetSpace,ResetMap,ResetGate,ResetTracking,ResetCount};
inline constexpr std::array<const char*,ResetCount> resetNames{"MODE","SPACE","MAP","GATE","TRACKING"};
using Counter=std::atomic<std::uint64_t>;
struct HandStats {
    std::atomic<int> mode{0};std::atomic<float> speed{},maxSpeed{},contactTravel{};std::atomic<bool> continuous{},ready{},inside{};
    std::atomic<bool> phantom{}; // DUAL: the blade of this hand is the phantom knife (report hands[h].source PHANTOM)
    Counter samples{},strikes{},blocked{};std::array<Counter,ResetCount> resets{};
};
struct Stats {
    std::atomic<unsigned> gate{GateNoEngine};std::array<Counter,GateCount> gateTicks{};
    Counter ticks{},activeTicks{},exceptions{};std::atomic<double> tickUsMax{};Counter tickUsSum{};
    std::array<HandStats,2> hands{};
    Counter scans{},slotsScanned{},actorsSeen{},inRange{};std::atomic<unsigned> maxInRange{},candidates{};std::array<Counter,SkipCount> skipped{};
    Counter scanUsSum{};std::atomic<double> scanUsMax{};
    Counter skeletons{},skeletonFailures{},layoutsResolved{},bodiesBuilt{},humanoidBodies{},cloudBodies{},actorBodies{};
    std::array<Counter,M::BoneKeyCount> missing{};
    Counter hitsBlade{},hitsFist{},kills{},kos{},immune{},dryHits{};std::array<Counter,9> byZone{},byReaction{};
    std::array<Counter,RejectCount> rejects{};
    Counter strikeRemote{},broadcastOnly{},fxKnife{},fxPunch{},fxSkipped{},impulses{},reactions{},probeCalls{},probeHits{},probeMisses{},
        probeBadComponent{},probeInstMatch{},boneComponents{},wallCalls{},wallBlocked{},mapMisses{},mapTurns{};
    Counter hapticHit{},hapticKill{},hapticKo{},hapticImmune{};
    std::atomic<int> lethalPunch{-1},ddaLevel{-1};
    // FIST BLOCK (engine side, main thread)
    Counter blockWanted{},blockFrames{},blockEngages{},blockReasserts{},blockReleases{},blockDropped{},blockNoMelee{},blockNoApi{},
        blockNative{},blockHits{},blockImpactEdges{},blockMeterHits{},blockImpactClears{},blockHaptics{};
    std::atomic<float> blockMeter{},blockMeterMax{};std::atomic<int> blockFlags{-1};std::atomic<std::uint64_t> blockLastHitNs{};
};
inline Stats stats{};
inline void bump(Counter& c,std::uint64_t n=1) noexcept {c.fetch_add(n,std::memory_order_relaxed);}
inline void maxOf(std::atomic<float>& a,float v) noexcept {float m=a.load(std::memory_order_relaxed);while(v>m&&!a.compare_exchange_weak(m,v)){}}
inline void maxOf(std::atomic<double>& a,double v) noexcept {double m=a.load(std::memory_order_relaxed);while(v>m&&!a.compare_exchange_weak(m,v)){}}
struct LastStrike {
    std::uint64_t ns{};int hand{-1};Mode mode{};std::uint32_t victimGuid{};int race{-1},zone{-1},component{-1},reaction{0},face{0};
    float speed{},damage{},hpBefore{},hpAfter{-1},msgHpAfter{-1},koBefore{},koAfter{};std::uint32_t flags{};Vec3 hitPos{};
    bool probeHit{},instMatch{},fx{},dry{},kill{},ko{},phantom{};const char* source{"NONE"};char bone[17]{};
};
inline std::mutex lastMutex;inline LastStrike lastStrike{};
inline std::mutex dumpMutex;inline std::string boneDumpHuman,boneDumpOther,forwardRow;
inline std::atomic<const char*> installState{"NOT ATTEMPTED"};inline std::atomic<bool> engineReady{},hookInstalled{};
inline std::atomic<const char*> blockApiState{"NOT ATTEMPTED"}; // FIST BLOCK engine functions
// Input map (.inputs): which logical foot inputs (sagPlayerInput::GetInput 0..29, "(b0^b2)&0x80" = down) are down while each pad bit is held.
inline constexpr unsigned padBits=14; // 0..11 runtime::PadButton bits, 12 = RT > .5, 13 = LT > .5 (the pad the game received)
inline std::array<std::array<std::atomic<std::uint32_t>,off::footInputs>,padBits> inputPairs{};
inline std::array<std::atomic<std::uint32_t>,off::footInputs> inputDown{};inline Counter inputSamples{},inputFailures{};

// ============================== packet hook (sagPlayerInputFoot::SetupActionTreePacket) ==============================
inline std::atomic<SetupPacketFn> packetOriginal{};
// DUAL: STRIKE and FIRE are masked separately - a phantom knife next to an engine revolver masks STRIKE only (the revolver
// still fires). maskWanted = either (report).
inline std::atomic<bool> maskWanted{},maskStrikeWanted{},maskFireWanted{},latchStrike{},latchFire{};
inline Counter packetCalls{},maskedStrike{},maskedFire{},maskedFrames{},latchedFrames{},phantomKnifeFx{},phantomHits{};
// p = the int intention packet after the original. Clears STRIKE (38) / FIRE (15) while wanted; a value held when the mask turns
// off stays cleared until it reads 0 (released) - otherwise the unmask frame would deliver a fresh native attack.
inline void filterPacket(std::int32_t* p,bool strike,bool fire) noexcept {
    const auto one=[&](unsigned slot,std::atomic<bool>& latch,Counter& masked,bool want){
        const bool held=p[slot]!=0;const bool masking=want||latch.load(std::memory_order_relaxed);
        if(masking&&held){p[slot]=0;bump(masked);if(!want)bump(latchedFrames);}
        latch.store(masking&&held,std::memory_order_relaxed);
    };
    one(off::packetStrike,latchStrike,maskedStrike,strike);one(off::packetFire,latchFire,maskedFire,fire);
    if(strike||fire)bump(maskedFrames);
}
inline void filterPacket(std::int32_t* p,bool want) noexcept {filterPacket(p,want,want);}
// No weapon in a VR hand and the holsters able to provide one = the FIRE intention never reaches the
// action tree (a FIRE with a holstered weapon draws the current weapon). Second line behind the input brain's empty-trigger withhold;
// the one line when the engine's trigger punch is the fist path (nativeFistTrigger: RT passes for STRIKE).
inline std::atomic<bool> emptyFireMask{true};inline Counter emptyFireFrames{};
inline bool emptyHandsFire() noexcept {
    return emptyFireMask.load(std::memory_order_relaxed)&&!H::emptyTriggerNative.load(std::memory_order_relaxed)&&
        H::engineReady.load(std::memory_order_acquire)&&H::config().enabled&&H::contextAllowed.load(std::memory_order_relaxed)&&
        H::holdHand.load(std::memory_order_relaxed)<0&&H::phantomHand.load(std::memory_order_relaxed)<0;
}
RDRVR_MELEE_NOINLINE inline void setupPacketHook(const void* foot,void* packet,bool full) noexcept {
    if(const auto original=packetOriginal.load(std::memory_order_acquire))original(foot,packet,full);
    bump(packetCalls);
    if(!full||!packet)return;
    const bool empty=emptyHandsFire();if(empty)bump(emptyFireFrames);
    filterPacket(static_cast<std::int32_t*>(packet),maskStrikeWanted.load(std::memory_order_acquire),maskFireWanted.load(std::memory_order_acquire)||empty);
}
inline void setMasks(bool strike,bool fire) noexcept {
    maskStrikeWanted.store(strike,std::memory_order_release);maskFireWanted.store(fire,std::memory_order_release);
    maskWanted.store(strike||fire,std::memory_order_release);
}

// ============================== main-thread engine side ==============================
struct Player {
    std::uintptr_t player{},actor{},comp{},matrix{},foot{},anim{},entity{},mover{};std::uint32_t guid{},goh{},state{},riderLink{},mountLink{};
    bool alive{},deadEye{},locked{};Vec3 position{},forward{};
};
struct Chain { // victim's frag / skeleton chain (GetSkeleton + GetHitEffect replicated with guarded reads)
    std::uintptr_t ent{},inst{},type{},children{},skel{},data{},globals{};unsigned childCount{},bones{};bool ok{};
};
struct LayoutEntry {std::uintptr_t data{};unsigned bones{};M::BoneLayout layout{};bool humanoid{};};
struct Candidate {
    std::uintptr_t actor{},health{},comp{},matrix{},anim{};std::uint32_t guid{},goh{},slot{};int race{-1};float distance{};
    Vec3 position{},rowA{},rowC{};float hp{},maxHp{},ko{},maxKo{};std::uint32_t healthFlags{};
    Chain chain{};std::uint64_t chainTick{};int layout{-1};M::Body body{};bool bodyValid{},skip{};
};
struct PendingStrike {int hand{-1};Mode mode{};int victim{-1},part{-1};Vec3 point{},direction{};float speed{};};
inline constexpr unsigned maxSlots=1024,maxActors=256,maxInRange=16,maxCandidates=8,maxBones=128,maxLayouts=32;

class Engine {
public:
    void configure(const EngineApi& api) noexcept {api_=api;hands_={};candCount_=0;layoutCount_=0;}
    const EngineApi& api() const noexcept {return api_;}
    void tick(const mt::Frame& frame) noexcept;
private:
    struct Hand {Mode mode{};M::BladeHold blade{};M::FistHold fist{};std::uint64_t wallTick{~0ull};bool wallClear{true};};
    EngineApi api_{};std::array<Hand,2> hands_{};
    std::array<Candidate,maxCandidates> cand_{};unsigned candCount_{};
    std::array<LayoutEntry,maxLayouts> layouts_{};unsigned layoutCount_{};
    std::uint64_t tickIndex_{},lastScan_{},epoch_{},space_{};bool haveSpace_{};
    WorldMap map_{},lastMap_{};bool mapOk_{},haveLastMap_{};
    H::PlayerBlocks blocks_{};bool blocksValid_{};std::uint64_t blocksTick_{};
    int phantomKnife_{-1}; // DUAL: the hand holding the phantom knife this tick (-1 none)
    // FIST BLOCK: the player's aniMelee whose block bit this module set (only that one is ever cleared), the flags / meter of the last tick.
    std::uintptr_t blockMelee_{};bool blockOurs_{};std::uint16_t blockFlags_{};float blockMeter_{};std::uint64_t impactSinceNs_{};
    std::uintptr_t playerMelee(const Player& p) const noexcept;
    void blockTick(const Player& p,bool want,std::uint64_t now) noexcept;
    void blockRelease(std::uint64_t now) noexcept;
    struct Mark {std::uint32_t guid{};std::uint64_t ns{};};std::array<Mark,16> marks_{};
    // scratch (static storage: no allocation on the main thread)
    std::array<unsigned char,maxSlots*16> slotBuf_{};std::array<unsigned char,maxActors*off::actorBlock> actorBuf_{};
    std::array<unsigned char,maxActors*0x20> compBuf_{};std::array<unsigned char,maxActors*0x40> matBuf_{};
    std::array<unsigned char,maxCandidates*maxBones*off::matrixStride> boneBuf_{};
    std::array<diagnostics::ReadSegment,maxActors> segs_{};

    template<class T> bool get(std::uintptr_t base,std::size_t o,T& out) const noexcept {
        return api_.read&&base&&base<=UINTPTR_MAX-o-sizeof(T)&&api_.read(nullptr,base+o,&out,sizeof(T));}
    bool block(std::uintptr_t p,void* out,std::size_t n) const noexcept {return api_.read&&p&&p<=UINTPTR_MAX-n&&api_.read(nullptr,p,out,n);}
    bool vread(const diagnostics::ReadSegment* s,std::size_t n) const noexcept {
        try{return diagnostics::guardedReadVector.read(api_.read,nullptr,s,n);}catch(...){return false;}}
    template<class T> static T at(const unsigned char* b,std::size_t o) noexcept {T v{};std::memcpy(&v,b+o,sizeof v);return v;}
    static Vec3 vec(const unsigned char* b,std::size_t o) noexcept {return {at<float>(b,o),at<float>(b,o+4),at<float>(b,o+8)};}

    Gate readPlayer(Player& p) noexcept;
    Gate gate(const Player& p,bool& freshContext) noexcept;
    bool slotActor(std::uint32_t guid,std::uintptr_t& actor) const noexcept;
    void scan(const Player& p,const std::array<Vec3,2>& handsWorld,unsigned armed) noexcept;
    bool deriveChain(std::uintptr_t actor,Chain& c) noexcept;
    int layoutFor(const Chain& c) noexcept;
    void buildBodies(const std::array<Vec3,2>& handsWorld) noexcept;
    bool validComponent(const Chain& c,int comp) const noexcept;
    int componentForBone(const Chain& c,int bone) const noexcept;
    bool wallClear(Hand& h,unsigned hand,const Player& p,const Candidate* victim,Vec3 from,Vec3 to) noexcept;
    void resetHand(unsigned h,HandReset why) noexcept {hands_[h].blade=M::BladeHold{};hands_[h].fist=M::FistHold{};bump(stats.hands[h].resets[why]);}
    void apply(const PendingStrike& s,const Player& p,std::uint64_t now) noexcept;
    void logInputs(const Player& p) noexcept;
};
inline Engine engine{};

inline bool Engine::slotActor(std::uint32_t guid,std::uintptr_t& actor) const noexcept {
    std::uintptr_t manager{},slots{};std::uint16_t count{};std::uint8_t entry[16]{};
    if(!get(api_.actorManager,0,manager)||!get(manager,0,count)||!get(api_.actorSlots,0,slots)||!slots)return false;
    const std::uint32_t index=guid&0xffffu;
    if(index>=count||!block(slots+index*16,entry,sizeof entry))return false;
    actor=at<std::uintptr_t>(entry,0);return actor&&at<std::uint16_t>(entry,8)==(guid>>16);
}

inline Gate Engine::readPlayer(Player& p) noexcept {
    std::uint32_t gs{};if(!get(api_.gameData,0,gs))return GateNotInGame;
    if(gs!=3)return GateNotInGame;
    std::uintptr_t table{};
    if(!get(api_.playerManager,0,table)||table!=api_.playerManagerTable||!get(api_.playerManager,off::pmPlayer,p.player)||!p.player)return GateNoPlayer;
    alignas(8) static thread_local unsigned char pb[off::playerBlock];
    if(!block(p.player,pb,sizeof pb)||at<std::uintptr_t>(pb,0)!=api_.playerTable||pb[off::playerActive]!=1)return GateNoPlayer;
    p.guid=at<std::uint32_t>(pb,off::playerGuid);p.state=at<std::uint32_t>(pb,off::playerState);p.foot=at<std::uintptr_t>(pb,off::playerFoot);
    const auto states=at<std::uintptr_t>(pb,off::playerStates);std::uint32_t eye{};p.deadEye=states&&get(states,off::statesDeadEye,eye)&&eye!=0;
    if(!slotActor(p.guid,p.actor))return GateNoPlayer;
    alignas(8) unsigned char a[off::actorBlock]{};
    if(!block(p.actor,a,sizeof a)||(at<std::uint32_t>(a,off::actorFlags)&3u)!=3u||at<std::uint32_t>(a,off::actorGuid)!=p.guid)return GateNoPlayer;
    p.goh=at<std::uint32_t>(a,off::actorGoh);p.comp=at<std::uintptr_t>(a,off::actorComponent);p.anim=at<std::uintptr_t>(a,off::actorAnim);
    p.entity=at<std::uintptr_t>(a,off::actorEntity);p.mover=at<std::uintptr_t>(a,off::actorMover);
    const auto health=at<std::uintptr_t>(a,off::actorHealth);float hp{};
    p.alive=health&&get(health,off::healthValue,hp)&&std::isfinite(hp)&&hp>0;
    if(!p.comp||!get(p.comp,off::compMatrix,p.matrix)||!p.matrix)return GateNoPlayer;
    unsigned char m[0x40]{};if(!block(p.matrix,m,sizeof m))return GateNoPlayer;
    p.position=vec(m,0x30);p.forward=vec(m,0x20);
    std::uint32_t links[4]{};if(block(p.comp+off::compRiderLink,links,sizeof links)){p.riderLink=links[0];p.mountLink=links[3];}
    return GateOk;
}

inline Gate Engine::gate(const Player& p,bool& freshContext) noexcept {
    freshContext=false;
    if(!p.alive)return GateDead;
    if(p.deadEye)return GateDeadEye;
    if(p.state!=2)return GateNotOnFoot; // Player state 2 = on foot
    if(!blocksValid_||tickIndex_-blocksTick_>=10){
        H::PlayerBlocks b{};bool ok=false;
        try{ok=api_.context&&api_.context(api_.contextUser,p.player,p.actor,p.guid,b);}catch(...){ok=false;}
        blocksValid_=ok;blocksTick_=tickIndex_;if(ok){blocks_=b;freshContext=true;}
    }
    if(!blocksValid_)return GateContext;
    const auto& b=blocks_;
    if(!b.alive)return GateDead;
    if(b.controlDisabled)return GateLocked;
    if(b.deadEye)return GateDeadEye;
    if(b.riding||b.mountingHorse)return GateRiding;
    if(b.ridingVehicle||b.vehicleTransition)return GateVehicle;
    if(b.duel)return GateDuel;
    if(b.mountedWeapon)return GateMountedWeapon;
    if(b.cover)return GateCover;
    if(b.linkedAnimation)return GateLinked;
    if(b.usingGringo)return GateGringo;
    return GateOk;
}

// ============================== FIST BLOCK ==============================
// The player's aniMelee = [[actor+0x38]+0xab8]+0x80, vtable-checked (0 = unreadable / not an aniMelee).
inline std::uintptr_t Engine::playerMelee(const Player& p) const noexcept {
    std::uintptr_t block{},melee{},vt{};
    if(!p.anim||!get(p.anim,off::animBlock,block)||!block||!get(block,off::blockMelee,melee)||!melee||!get(melee,0,vt)||vt!=api_.aniMeleeVtable)return 0;
    return melee;
}
// Every gate-open tick. want = the guard pose is engaged (input brain, fresh), FIST BLOCK on, both VR hands empty. Sets the engine block
// (SetBlockState(true): the bit every NPC attacker's MeleeMachine reads) and keeps it set; clears ONLY a block it set. Blocked strikes
// show as the target-side "blocked impact" bit 0x40 rising and / or the block meter +0x130 growing (both written by the attacker's
// MeleeMachine): counted + a haptic on both hands. Bit 0x40 is cleared by DeScheduleBlock after impactClearNs (a native block anim's
// lifetime) so the next blocked strike is seen again, and when the guard ends (never while a native block anim is scheduled, +0xc0 == 2).
inline constexpr std::uint64_t impactClearNs=400'000'000;
inline void Engine::blockTick(const Player& p,bool want,std::uint64_t now) noexcept {
    if(want)bump(stats.blockWanted);
    if(!want&&!blockOurs_)return;
    const std::uintptr_t m=playerMelee(p);
    if(!m){if(want)bump(stats.blockNoMelee);if(blockOurs_){blockOurs_=false;blockMelee_=0;bump(stats.blockDropped);}return;}
    if(blockOurs_&&blockMelee_!=m){blockOurs_=false;blockMelee_=0;bump(stats.blockDropped);} // another aniMelee: never touch the old one
    std::uint16_t flags{};float meter{};std::int32_t scheduled{};
    if(!get(m,off::meleeFlags,flags)||!get(m,off::meleeBlockMeter,meter)||!get(m,off::meleeScheduled,scheduled)){
        if(blockOurs_){blockOurs_=false;blockMelee_=0;bump(stats.blockDropped);}return;}
    stats.blockFlags.store(int(flags),std::memory_order_relaxed);if(std::isfinite(meter)){stats.blockMeter.store(meter,std::memory_order_relaxed);maxOf(stats.blockMeterMax,meter);}
    if(!want){ // guard over: the module's block off, the blocked-impact bit back to the native "no block anim" state
        if(api_.setBlockState&&(flags&off::flagBlocking)){try{api_.setBlockState(reinterpret_cast<void*>(m),false);}catch(...){bump(stats.exceptions);}}
        if(api_.deScheduleBlock&&(flags&off::flagBlockedImpact)&&scheduled!=2){try{api_.deScheduleBlock(reinterpret_cast<void*>(m));}catch(...){bump(stats.exceptions);}}
        blockOurs_=false;blockMelee_=0;impactSinceNs_=0;bump(stats.blockReleases);return;}
    if(!api_.setBlockState){bump(stats.blockNoApi);return;}
    if(!blockOurs_&&(flags&off::flagBlocking)){bump(stats.blockNative);return;} // the engine's own block is running: leave it alone
    if(!(flags&off::flagBlocking)){
        try{api_.setBlockState(reinterpret_cast<void*>(m),true);}catch(...){bump(stats.exceptions);return;}
        if(!blockOurs_){blockOurs_=true;blockMelee_=m;blockFlags_=std::uint16_t(flags|off::flagBlocking);blockMeter_=meter;impactSinceNs_=0;bump(stats.blockEngages);}
        else bump(stats.blockReasserts); // something cleared it under the held guard
        flags=std::uint16_t(flags|off::flagBlocking);
    }
    bump(stats.blockFrames);
    // blocked strikes since the last tick
    const bool impactEdge=(flags&off::flagBlockedImpact)&&!(blockFlags_&off::flagBlockedImpact);
    const bool meterUp=std::isfinite(meter)&&std::isfinite(blockMeter_)&&meter>blockMeter_+1e-3f;
    if(impactEdge)bump(stats.blockImpactEdges);
    if(meterUp)bump(stats.blockMeterHits);
    if(impactEdge||meterUp){bump(stats.blockHits);stats.blockLastHitNs.store(now,std::memory_order_relaxed);
        for(unsigned h=0;h<2;++h)haptics::pulse(h,.60f,50'000,now,haptics::SourceParry);bump(stats.blockHaptics);}
    if(flags&off::flagBlockedImpact){
        if(!impactSinceNs_)impactSinceNs_=now?now:1;
        else if(now>=impactSinceNs_&&now-impactSinceNs_>=impactClearNs&&api_.deScheduleBlock&&scheduled!=2){
            try{api_.deScheduleBlock(reinterpret_cast<void*>(m));}catch(...){bump(stats.exceptions);}
            flags=std::uint16_t(flags&~off::flagBlockedImpact);impactSinceNs_=0;bump(stats.blockImpactClears);}
    }else impactSinceNs_=0;
    blockFlags_=flags;blockMeter_=meter;
}
// A gate closed (menu, NATIVE, dead, riding, not in game, ...): release the module's block if the player is still readable and still owns that
// aniMelee; otherwise just forget it (never a write into an object that cannot be verified).
inline void Engine::blockRelease(std::uint64_t now) noexcept {
    if(!blockOurs_)return;
    Player p{};
    if(readPlayer(p)==GateOk&&playerMelee(p)==blockMelee_){blockTick(p,false,now);return;}
    blockOurs_=false;blockMelee_=0;impactSinceNs_=0;bump(stats.blockDropped);
}

// Replicates entEntityComponent::GetSkeleton -> entEntityInst::GetSkeleton (0x2913174) -> fragInst::GetSkeleton (0x1ea3ddc) and the
// frag type selection of GetHitEffect / SetFromTestProbe (inst+0x78 resident bits, else inst+0x98).
inline bool Engine::deriveChain(std::uintptr_t actor,Chain& c) noexcept {
    c=Chain{};std::uintptr_t vt{},slot{},cache{},a{};std::uint8_t frag{};std::uint32_t bits{};std::uint16_t nb{};std::uint8_t children{};
    if(!get(actor,off::actorEntity,c.ent)||!c.ent||!get(c.ent,0,vt)||vt!=api_.entityVtable)return false;
    if(!get(c.ent,off::entityInst,c.inst)||!c.inst||!get(c.inst,0,vt)||!vt||!get(vt,off::instSkeletonSlot,slot)||slot!=api_.instSkeletonFn)return false;
    if(!get(c.inst,off::instIsFrag,frag)||!frag||!get(c.inst,off::instCache,cache)||!cache)return false;
    if(!get(c.inst,off::instTypeA,a))return false;
    if(a){if(!get(a,off::typeBits,bits)||(bits&off::typeBitsResident)!=off::typeBitsResident||!get(a,0,c.type))return false;}
    else if(!get(c.inst,off::instTypeB,c.type))return false;
    if(!c.type||!get(cache,off::cacheSkeleton,c.skel)||!c.skel||!get(c.skel,off::skelData,c.data)||!c.data||!get(c.skel,off::skelGlobals,c.globals)||!c.globals)return false;
    if(!get(c.data,off::dataBoneCount,nb)||!nb||nb>512)return false;
    if(!get(c.type,off::typeChildCount,children)||!get(c.type,off::typeChildren,c.children))return false;
    c.bones=nb;c.childCount=children;c.ok=true;return true;
}
// A component GetHitEffect / SetFromTestProbe may index: < child count, child pointer present, child bone inside the skeleton.
inline bool Engine::validComponent(const Chain& c,int comp) const noexcept {
    if(!c.ok||comp<0||unsigned(comp)>=c.childCount||!c.children)return false;
    std::uintptr_t child{};std::int16_t bone{};
    return get(c.children,std::size_t(comp)*8,child)&&child&&get(child,off::childBone,bone)&&bone>=0&&unsigned(bone)<c.bones;
}
// NMBodyInfo::FindComponentFromBone (0x1e86c2c) equivalent: the frag child whose bone is `bone` (-1 none).
inline int Engine::componentForBone(const Chain& c,int bone) const noexcept {
    if(!c.ok||bone<0||!c.children||!c.childCount)return -1;
    std::array<std::uintptr_t,256> ptr{};const unsigned n=std::min(c.childCount,256u);
    if(!block(c.children,ptr.data(),n*8))return -1;
    std::array<std::int16_t,256> bones{};std::array<diagnostics::ReadSegment,256> s{};unsigned k=0;
    for(unsigned i=0;i<n;++i){if(!ptr[i]){bones[i]=-1;continue;}s[k++]={ptr[i]+off::childBone,&bones[i],2};}
    if(!vread(s.data(),k))return -1;
    for(unsigned i=0;i<n;++i)if(ptr[i]&&bones[i]==bone)return validComponent(c,int(i))?int(i):-1;
    return -1;
}
inline int Engine::layoutFor(const Chain& c) noexcept {
    for(unsigned i=0;i<layoutCount_;++i)if(layouts_[i].data==c.data&&layouts_[i].bones==c.bones)return int(i);
    if(layoutCount_>=maxLayouts)return -1;
    const unsigned n=std::min(c.bones,unsigned(maxBones));
    std::array<std::uintptr_t,maxBones> names{};std::array<diagnostics::ReadSegment,maxBones> s{};
    std::uintptr_t bones{};if(!get(c.data,off::dataBones,bones)||!bones)return -1;
    for(unsigned i=0;i<n;++i)s[i]={bones+i*off::boneStride,&names[i],8};
    if(!vread(s.data(),n))return -1;
    static thread_local std::array<std::array<char,24>,maxBones> text{};
    std::array<const char*,maxBones> ptrs{};
    for(unsigned i=0;i<n;++i){text[i]={};ptrs[i]=nullptr;if(!names[i])continue;
        if(block(names[i],text[i].data(),23)||block(names[i],text[i].data(),8)){text[i][23]=0;ptrs[i]=text[i].data();}}
    LayoutEntry e{};e.data=c.data;e.bones=c.bones;e.layout=M::resolveBones(ptrs.data(),n);e.humanoid=e.layout.humanoid();
    for(unsigned k=0;k<M::BoneKeyCount;++k)if(!e.layout.has(k))bump(stats.missing[k]);
    {std::lock_guard lock(dumpMutex);auto& dump=e.humanoid?boneDumpHuman:boneDumpOther;
     if(dump.empty()){dump="[";for(unsigned i=0;i<n;++i){if(i)dump+=',';dump+='"';
         for(const char* q=ptrs[i]?ptrs[i]:"?";*q;++q)dump+=(*q=='"'||*q=='\\'||static_cast<unsigned char>(*q)<0x20||static_cast<unsigned char>(*q)>0x7e)?'?':*q;
         dump+='"';}dump+=']';}}
    layouts_[layoutCount_]=e;bump(stats.layoutsResolved);return int(layoutCount_++);
}

inline void Engine::scan(const Player& p,const std::array<Vec3,2>& hw,unsigned armed) noexcept {
    const auto t0=nowNs();bump(stats.scans);
    std::uintptr_t manager{},slots{};std::uint16_t count{};
    if(!get(api_.actorManager,0,manager)||!get(manager,0,count)||!get(api_.actorSlots,0,slots)||!slots){bump(stats.skipped[SkipUnreadable]);candCount_=0;return;}
    const unsigned n=std::min<unsigned>(count,maxSlots);
    if(!n||!block(slots,slotBuf_.data(),std::size_t(n)*16)){candCount_=0;return;}
    bump(stats.slotsScanned,n);
    // actors (one 0x190 block each, batched)
    std::array<std::uint32_t,maxActors> index{};unsigned m=0;
    for(unsigned i=0;i<n&&m<maxActors;++i){const auto a=at<std::uintptr_t>(slotBuf_.data(),std::size_t(i)*16);if(!a)continue;
        index[m]=i;segs_[m]={a,actorBuf_.data()+std::size_t(m)*off::actorBlock,off::actorBlock};++m;}
    if(!vread(segs_.data(),m)){ // one unreadable actor fails the batch: per-actor fallback
        for(unsigned k=0;k<m;++k)if(!block(segs_[k].source,segs_[k].target,segs_[k].size)){std::memset(segs_[k].target,0,off::actorBlock);bump(stats.skipped[SkipUnreadable]);}}
    struct Pre {std::uintptr_t actor{},health{},comp{},anim{};std::uint32_t guid{},goh{},slot{};};
    std::array<Pre,maxActors> pre{};unsigned q=0;
    for(unsigned k=0;k<m;++k){
        const unsigned char* a=actorBuf_.data()+std::size_t(k)*off::actorBlock;
        const auto guid=at<std::uint32_t>(a,off::actorGuid);const auto gen=at<std::uint16_t>(slotBuf_.data(),std::size_t(index[k])*16+8);
        if((guid&0xffffu)!=index[k]||(guid>>16)!=gen){bump(stats.skipped[SkipBadSlot]);continue;}
        if((at<std::uint32_t>(a,off::actorFlags)&1u)||guid==p.guid){bump(stats.skipped[SkipPlayer]);continue;}
        Pre r{};r.actor=segs_[k].source;r.guid=guid;r.slot=index[k];r.health=at<std::uintptr_t>(a,off::actorHealth);
        r.comp=at<std::uintptr_t>(a,off::actorComponent);r.anim=at<std::uintptr_t>(a,off::actorAnim);r.goh=at<std::uint32_t>(a,off::actorGoh);
        if(!r.comp||!r.health){bump(stats.skipped[SkipNotCreature]);continue;}
        pre[q++]=r;
    }
    bump(stats.actorsSeen,q);
    // component heads (vtable, matrix pointer) then matrices
    for(unsigned k=0;k<q;++k)segs_[k]={pre[k].comp,compBuf_.data()+std::size_t(k)*0x20,0x20};
    if(!vread(segs_.data(),q))for(unsigned k=0;k<q;++k)if(!block(segs_[k].source,segs_[k].target,0x20))std::memset(segs_[k].target,0,0x20);
    unsigned w=0;std::array<unsigned,maxActors> live{};
    for(unsigned k=0;k<q;++k){const auto mtx=at<std::uintptr_t>(compBuf_.data(),std::size_t(k)*0x20+off::compMatrix);
        if(!mtx){bump(stats.skipped[SkipUnreadable]);continue;}live[w]=k;segs_[w]={mtx,matBuf_.data()+std::size_t(k)*0x40,0x40};++w;}
    if(!vread(segs_.data(),w))for(unsigned k=0;k<w;++k)if(!block(segs_[k].source,segs_[k].target,0x40))std::memset(segs_[k].target,0xff,0x40);
    struct Near {unsigned k{};float d{};};std::array<Near,maxActors> near{};unsigned nn=0;
    for(unsigned j=0;j<w;++j){
        const unsigned k=live[j];const auto pos=vec(matBuf_.data(),std::size_t(k)*0x40+0x30);if(!M::finite(pos)){bump(stats.skipped[SkipUnreadable]);continue;}
        float d=1e9f;for(unsigned h=0;h<2;++h)if(armed>>h&1u)d=std::min(d,(pos+Vec3(0,1.f,0)-hw[h]).length());
        if(d>config.reach){bump(stats.skipped[SkipFar]);continue;}
        near[nn++]={k,d};
    }
    std::sort(near.begin(),near.begin()+nn,[](const Near& a,const Near& b){return a.d<b.d;});
    nn=std::min(nn,maxInRange);bump(stats.inRange,nn);
    {unsigned mx=stats.maxInRange.load(std::memory_order_relaxed);while(nn>mx&&!stats.maxInRange.compare_exchange_weak(mx,nn)){}}
    std::array<Candidate,maxCandidates> next{};unsigned nc=0;
    for(unsigned j=0;j<nn&&nc<maxCandidates;++j){
        const unsigned k=near[j].k;const auto& r=pre[k];const unsigned char* cb=compBuf_.data()+std::size_t(k)*0x20;const unsigned char* mb=matBuf_.data()+std::size_t(k)*0x40;
        const auto vt=at<std::uintptr_t>(cb,0);std::uintptr_t isCreature{};
        if(!vt||!get(vt,off::compIsCreatureSlot,isCreature)||isCreature!=api_.isCreatureFn){bump(stats.skipped[SkipNotCreature]);continue;}
        std::int32_t race{-1};std::uint32_t links[4]{};
        if(!get(r.comp,off::compRace,race)||!block(r.comp+off::compRiderLink,links,sizeof links)){bump(stats.skipped[SkipUnreadable]);continue;}
        if(race<0||race>17){bump(stats.skipped[SkipRace]);continue;}
        const std::uint32_t rider=links[0],mount=links[3];
        if(r.guid==p.mountLink||r.guid==p.riderLink||rider==p.guid||mount==p.guid){bump(stats.skipped[SkipMount]);continue;}
        if(race==1&&!config.horses){bump(stats.skipped[SkipHorse]);continue;}
        alignas(8) unsigned char hb[off::healthBlock]{};
        if(!block(r.health,hb,sizeof hb)||at<std::uintptr_t>(hb,0)!=api_.healthVtable){bump(stats.skipped[SkipUnreadable]);continue;}
        Candidate c{};c.actor=r.actor;c.guid=r.guid;c.goh=r.goh;c.slot=r.slot;c.health=r.health;c.comp=r.comp;c.anim=r.anim;c.race=race;
        c.matrix=at<std::uintptr_t>(cb,off::compMatrix);c.position=vec(mb,0x30);c.rowA=vec(mb,0);c.rowC=vec(mb,0x20);c.distance=near[j].d;
        c.maxHp=at<float>(hb,off::healthMax);c.hp=at<float>(hb,off::healthValue);c.maxKo=at<float>(hb,off::healthKoMax);c.ko=at<float>(hb,off::healthKo);
        c.healthFlags=at<std::uint32_t>(hb,off::healthFlags);
        if(!(c.hp>0)||(c.healthFlags&off::healthDead)){bump(stats.skipped[SkipDead]);continue;}
        for(unsigned o=0;o<candCount_;++o)if(cand_[o].actor==c.actor&&cand_[o].guid==c.guid&&cand_[o].chain.ok&&tickIndex_-cand_[o].chainTick<30){
            c.chain=cand_[o].chain;c.chainTick=cand_[o].chainTick;c.layout=cand_[o].layout;}
        next[nc++]=c;
    }
    cand_=next;candCount_=nc;stats.candidates.store(nc,std::memory_order_relaxed);
    const double us=double(nowNs()-t0)/1000.0;bump(stats.scanUsSum,std::uint64_t(us));maxOf(stats.scanUsMax,us);
}

inline void Engine::buildBodies(const std::array<Vec3,2>& hw) noexcept {
    std::array<diagnostics::ReadSegment,maxCandidates> s{};std::array<unsigned,maxCandidates> who{};unsigned n=0;
    for(unsigned i=0;i<candCount_;++i){
        auto& c=cand_[i];c.bodyValid=false;c.skip=false;
        if(!c.chain.ok||tickIndex_-c.chainTick>=30){
            if(deriveChain(c.actor,c.chain)){c.chainTick=tickIndex_;bump(stats.skeletons);c.layout=layoutFor(c.chain);}
            else{bump(stats.skeletonFailures);c.chain.ok=false;}
        }
        if(c.chain.ok){who[n]=i;s[n]={c.chain.globals,boneBuf_.data()+std::size_t(n)*maxBones*off::matrixStride,
            std::size_t(std::min(c.chain.bones,unsigned(maxBones)))*off::matrixStride};++n;}
        else{c.body.count=0;c.body.capsules[c.body.count++]=M::actorCapsule(c.position);c.bodyValid=true;bump(stats.actorBodies);}
    }
    if(!vread(s.data(),n))for(unsigned k=0;k<n;++k)if(!block(s[k].source,s[k].target,s[k].size)){cand_[who[k]].chain.ok=false;s[k].size=0;}
    for(unsigned k=0;k<n;++k){
        auto& c=cand_[who[k]];if(!s[k].size){c.body.count=0;c.body.capsules[c.body.count++]=M::actorCapsule(c.position);c.bodyValid=true;bump(stats.actorBodies);continue;}
        const unsigned char* b=static_cast<const unsigned char*>(s[k].target);const unsigned nb=unsigned(s[k].size/off::matrixStride);
        const auto pos=[&](unsigned i){return vec(b,std::size_t(i)*off::matrixStride+off::matrixPos);};
        bool built=false;
        if(c.layout>=0&&layouts_[unsigned(c.layout)].humanoid){
            const auto& l=layouts_[unsigned(c.layout)].layout;
            built=M::buildBody(l,[&](unsigned key,Vec3& out){const int i=l.index[key];if(i<0||unsigned(i)>=nb)return false;out=pos(unsigned(i));
                return (out-c.position).length()<4.f;},c.body)>=2;
            if(built)bump(stats.humanoidBodies);
        }
        if(!built){
            std::array<Vec3,maxBones> cloud{};unsigned used=0;
            for(unsigned i=0;i<nb;++i){const auto q=pos(i);if(M::finite(q)&&(q-c.position).length()<4.f)cloud[used++]=q;}
            M::Capsule cc{};c.body.count=0;
            if(M::cloudCapsule(cloud.data(),used,cc)){c.body.capsules[c.body.count++]=cc;bump(stats.cloudBodies);}
            else{c.body.capsules[c.body.count++]=M::actorCapsule(c.position);bump(stats.actorBodies);}
        }
        c.bodyValid=true;bump(stats.bodiesBuilt);
    }
    (void)hw;
}

// Gothic clearPath(head -> grip): one level probe per hand per tick (SweptSphereMultipleExcludeInsts include 1 = world, state 7 - the
// native melee LOS of MeleeMachine 0x1e41944 uses include 1 / state 7), excluding the player's entity + mover and the victim's entity.
inline bool Engine::wallClear(Hand& h,unsigned hand,const Player& p,const Candidate* victim,Vec3 from,Vec3 to) noexcept {
    if(!config.wall||config.dry||!api_.probeLevel)return true;
    if(h.wallTick==tickIndex_)return h.wallClear;
    h.wallTick=tickIndex_;h.wallClear=true;
    const Vec3 d=to-from;const float l=d.length();if(!(l>.05f)||l>3.f)return true;
    const Vec3 end=from+d*((l-.03f)/l); // Gothic: 3 cm end margin
    alignas(16) float seg[8]{from.x,from.y,from.z,0,end.x,end.y,end.z,0};
    alignas(16) unsigned char isect[off::isectSize]{};std::uint32_t mark=0xffff;std::memcpy(isect+off::isectLevel,&mark,4);std::memcpy(isect+off::isectMark,&mark,4);
    std::array<const void*,3> ex{};int nex=0;
    if(p.entity){std::uintptr_t inst{};if(get(p.entity,off::entityInst,inst)&&inst)ex[std::size_t(nex++)]=reinterpret_cast<const void*>(inst);}
    if(p.mover)ex[std::size_t(nex++)]=reinterpret_cast<const void*>(p.mover+off::moverInst);
    if(victim&&victim->chain.ok&&victim->chain.inst)ex[std::size_t(nex++)]=reinterpret_cast<const void*>(victim->chain.inst);
    bump(stats.wallCalls);int hits=0;
    try{hits=api_.probeLevel(seg,.01f,1,isect,nex,ex.data(),1,0,7);}catch(...){hits=0;}
    h.wallClear=hits<=0;if(!h.wallClear){bump(stats.wallBlocked);bump(stats.hands[hand].blocked);}
    return h.wallClear;
}

inline void Engine::logInputs(const Player& p) noexcept {
    if(!config.inputs||!p.foot)return;
    std::uintptr_t vt{},holder{},values{};
    if(!get(p.foot,0,vt)||vt!=api_.footVtable||!get(p.foot,off::footValues,holder)||!holder||!get(holder,0,values)||!values){bump(inputFailures);return;}
    unsigned char v[off::footInputs*16]{};if(!block(values,v,sizeof v)){bump(inputFailures);return;}
    std::uint32_t down{};for(unsigned i=0;i<off::footInputs;++i)if((v[i*16]^v[i*16+2])&0x80u)down|=1u<<i;
    std::uint32_t pad=H::lastPadButtons.load(std::memory_order_relaxed)&0xfffu;
    if(H::lastPadRt.load(std::memory_order_relaxed)>.5f)pad|=1u<<12;if(H::lastPadLt.load(std::memory_order_relaxed)>.5f)pad|=1u<<13;
    bump(inputSamples);
    for(unsigned i=0;i<off::footInputs;++i){if(!(down>>i&1u))continue;inputDown[i].fetch_add(1,std::memory_order_relaxed);
        for(unsigned b=0;b<padBits;++b)if(pad>>b&1u)inputPairs[b][i].fetch_add(1,std::memory_order_relaxed);}
}

inline void Engine::apply(const PendingStrike& s,const Player& p,std::uint64_t now) noexcept {
    if(s.victim<0||unsigned(s.victim)>=candCount_)return;
    Candidate& v=cand_[unsigned(s.victim)];
    // 1. the victim is still the same live actor (slot generation), health component verified
    std::uintptr_t actor{};if(!slotActor(v.guid,actor)||actor!=v.actor){bump(stats.rejects[RejectStale]);return;}
    alignas(8) unsigned char hb[off::healthBlock]{};
    if(!block(v.health,hb,sizeof hb)||at<std::uintptr_t>(hb,0)!=api_.healthVtable){bump(stats.rejects[RejectStale]);return;}
    const float hpBefore=at<float>(hb,off::healthValue),maxHp=at<float>(hb,off::healthMax),koBefore=at<float>(hb,off::healthKo),maxKo=at<float>(hb,off::healthKoMax);
    const std::uint32_t hflags=at<std::uint32_t>(hb,off::healthFlags);
    if(!(hpBefore>0)||(hflags&off::healthDead)){bump(stats.rejects[RejectDead]);return;}
    // 2. per-victim cool-down across both hands (two-hand double touch)
    for(const auto& mk:marks_)if(mk.guid==v.guid&&now>=mk.ns&&double(now-mk.ns)<double(config.victimCooldownMs)*1e6){bump(stats.rejects[RejectSameVictim]);return;}
    // 3. invulnerable: no damage message (Gothic: immortal Cavalorn still got feedback) - weak buzz so it is not read as a detector miss
    if(hflags&off::healthInvulnerable){
        bump(stats.immune);bump(stats.rejects[RejectInvulnerable]);bump(stats.hapticImmune);
        haptics::pulse(unsigned(s.hand),.3f,30'000,now,haptics::SourceMeleeHit);
        {auto& mk=*std::min_element(marks_.begin(),marks_.end(),[](const Mark& a,const Mark& b){return a.ns<b.ns;});mk={v.guid,now};}
        return;
    }
    const bool fist=s.mode==Mode::Fist;
    const M::Capsule cap=s.part>=0&&unsigned(s.part)<v.body.count?v.body.capsules[unsigned(s.part)]:M::actorCapsule(v.position);
    const Vec3 dir=M::normalized(s.direction,M::normalized(v.position+Vec3(0,1,0)-p.position));
    const Vec3 axis=M::capsuleAxisPoint(cap,s.point);
    Vec3 hitPos=s.point,normal=M::normalized(s.point-axis,-dir);
    // 4. component: engine probe on THIS victim, else the frag child of the capsule's bone; both bounds-checked
    Chain chain{};const bool chainOk=deriveChain(v.actor,chain);
    int comp=-1;const char* source="NONE";bool probeHit=false,instMatch=false;
    if(chainOk&&config.probe&&!config.dry&&api_.probeOne){
        alignas(16) float seg[8]{};const Vec3 a=axis+normal*(cap.radius+.15f);seg[0]=a.x;seg[1]=a.y;seg[2]=a.z;seg[4]=axis.x;seg[5]=axis.y;seg[6]=axis.z;
        alignas(16) unsigned char isect[off::isectSize]{};std::uint32_t mark=0xffff;std::memcpy(isect+off::isectLevel,&mark,4);std::memcpy(isect+off::isectMark,&mark,4);
        bump(stats.probeCalls);int n=0;
        try{n=api_.probeOne(seg,.03f,1,isect,reinterpret_cast<const void*>(chain.inst));}catch(...){n=0;}
        if(n>0){
            probeHit=true;bump(stats.probeHits);
            const int c=at<std::uint16_t>(isect,off::isectComponent);instMatch=at<std::uintptr_t>(isect,off::isectInst)==chain.inst;if(instMatch)bump(stats.probeInstMatch);
            if(validComponent(chain,c)){comp=c;source="PROBE";const Vec3 hp=vec(isect,off::isectPos),hn=vec(isect,off::isectNormal);
                if(M::finite(hp)&&(hp-s.point).length()<1.f)hitPos=hp;if(M::finite(hn)&&hn.length()>.5f)normal=M::normalized(hn);}
            else bump(stats.probeBadComponent);
        }else{bump(stats.probeMisses);bump(stats.rejects[RejectProbeMiss]);}
    }
    if(comp<0&&chainOk){const int c=componentForBone(chain,cap.bone);if(c>=0){comp=c;source="BONE";bump(stats.boneComponents);}}
    if(comp<0)bump(stats.rejects[RejectNoComponent]);
    if(!chainOk)bump(stats.rejects[RejectNoSkeleton]);
    // 5. hlthMsgInjure
    std::uintptr_t playerMelee{},block2{},vt{};
    const bool meleeOk=p.anim&&get(p.anim,off::animBlock,block2)&&block2&&get(block2,off::blockMelee,playerMelee)&&playerMelee&&get(playerMelee,0,vt)&&vt==api_.aniMeleeVtable;
    float impulseScale=0;if(meleeOk){std::uintptr_t data{};float k{};if(get(playerMelee,off::meleeActionData,data)&&data&&get(data,off::actionImpulse,k)&&std::isfinite(k)&&k>=0&&k<1e4f)impulseScale=k;}
    int weapon=-1;
    // DUAL: a cut of the phantom knife carries the knife's own enum ([[item+0x30 type]+0x50 weapon data]+8, what
    // GetWeaponBaseTypeForActor returns for the item in hand) - the engine item may be the revolver.
    const bool phantomCut=!fist&&s.hand>=0&&s.hand==phantomKnife_;
    if(phantomCut&&!config.dry){
        const auto item=H::phantomItem.load(std::memory_order_acquire);std::uintptr_t type{},data{};std::int16_t e{-1};
        if(item&&get(item,H::itemType,type)&&type&&get(type,H::typeWeaponData,data)&&data&&get(data,8,e)&&e>=0)weapon=e;}
    else if(!fist&&!config.dry&&api_.weaponType){const void* t=nullptr;try{t=api_.weaponType(reinterpret_cast<const void*>(p.actor));}catch(...){t=nullptr;}
        std::int16_t e{-1};if(t&&get(reinterpret_cast<std::uintptr_t>(t),8,e)&&e>=0)weapon=e;}
    M::msg::Fields f{};f.hitPos=hitPos;f.normal=normal;f.impulse=dir*impulseScale;f.attackerPosPtr=p.matrix+0x30;f.attackerGuid=p.guid;
    f.flags=fist?M::msg::flagKo:M::msg::flagLethal;f.zone=cap.zone;f.face=M::faceFromForwards(v.rowC,p.forward);f.component=comp;f.weapon=weapon;f.damage=0;
    alignas(16) unsigned char m[M::msg::size];
    if(!M::msg::build(m,f)){bump(stats.rejects[RejectMessage]);return;}
    if(comp>=0&&!config.dry&&api_.hitInfo){ // engine hit info: bone name, zone (heart!), local position/normal of that component
        alignas(16) unsigned char isect[off::isectSize]{};M::msg::putVec(isect,off::isectPos,hitPos);M::msg::putVec(isect,off::isectNormal,normal);
        const std::uint16_t c16=std::uint16_t(comp);std::memcpy(isect+off::isectComponent,&c16,2);
        std::memcpy(isect+off::isectInst,&chain.inst,sizeof chain.inst);
        try{api_.hitInfo(m+M::msg::localPos,isect,reinterpret_cast<const void*>(v.actor));}catch(...){}
    }
    int zone=M::msg::get<std::int32_t>(m,M::msg::zone);if(zone<0||zone>8)zone=cap.zone;
    const float damage=fist?M::fistDamage(config.damage,maxKo,s.speed):M::bladeDamage(config.damage,zone,maxHp,s.speed,f.face==1);
    M::msg::put<float>(m,M::msg::damage,damage);
    if(M::msg::get<std::uint32_t>(m,M::msg::flags)&M::msg::flagInstantKill){bump(stats.rejects[RejectMessage]);return;} // never
    const int reaction=M::reactionType(fist,dir,v.rowA,s.speed,config.damage.fistStrongSpeed);
    // 6. engine
    bool fx=false;
    if(config.dry)bump(stats.dryHits);
    else{
        if(config.m2){std::uintptr_t vb{},vm{},vvt{};
            if(v.anim&&get(v.anim,off::animBlock,vb)&&vb&&get(vb,off::blockMelee,vm)&&vm&&get(vm,0,vvt)&&vvt==api_.aniMeleeVtable){
                *reinterpret_cast<std::int32_t*>(vm+off::meleeReaction)=reaction;bump(stats.reactions);bump(stats.byReaction[unsigned(reaction)]);}}
        alignas(16) float pos16[4]{hitPos.x,hitPos.y,hitPos.z,0};
        void* victim=reinterpret_cast<void*>(v.actor);void* attacker=reinterpret_cast<void*>(p.actor);
        try{
            std::int32_t attackClass{-1};
            if(phantomCut){ // DUAL: StrikeDamageRemote would pick the punch FX (the item in hand is not the knife): its knife path by hand
                api_.broadcast(victim,m);api_.addEvent(v.goh,p.goh,7);api_.audio(attacker,victim,pos16);bump(phantomHits);
                if(comp>=0&&config.fx&&meleeOk&&api_.knifeFx&&get(playerMelee,off::meleeAttackClass,attackClass)&&attackClass==0){
                    api_.knifeFx(reinterpret_cast<void*>(playerMelee),m,attacker,victim,comp);fx=true;bump(stats.fxKnife);bump(phantomKnifeFx);}
                else{bump(stats.broadcastOnly);bump(stats.fxSkipped);}
            }else if(comp>=0&&config.fx&&meleeOk&&api_.strike){
                api_.strike(reinterpret_cast<void*>(playerMelee),victim,attacker,m,pos16,false,std::uint16_t(comp),0,true);
                fx=true;bump(stats.strikeRemote);bump(fist?stats.fxPunch:stats.fxKnife);
            }else{
                api_.broadcast(victim,m);api_.addEvent(v.goh,p.goh,7);api_.audio(attacker,victim,pos16);
                bump(stats.broadcastOnly);bump(stats.fxSkipped);
            }
        }catch(...){bump(stats.exceptions);}
        if(config.m2&&api_.impulse&&(fist?s.speed>=4.5f:damage>=config.damage.lethal)){
            alignas(16) unsigned char im[M::impulse::size];
            if(M::impulse::build(im,dir*(config.push*std::max(s.speed,1.f)),hitPos,comp)){try{api_.impulse(victim,im);bump(stats.impulses);}catch(...){}}
        }
    }
    // 7. outcome, feedback
    {auto& mk=*std::min_element(marks_.begin(),marks_.end(),[](const Mark& a,const Mark& b){return a.ns<b.ns;});mk={v.guid,now};}
    alignas(8) unsigned char after[off::healthBlock]{};float hpAfter=-1,koAfter=-1;std::uint32_t flagsAfter=0;
    if(!config.dry&&block(v.health,after,sizeof after)){hpAfter=at<float>(after,off::healthValue);koAfter=at<float>(after,off::healthKo);flagsAfter=at<std::uint32_t>(after,off::healthFlags);}
    const bool kill=!config.dry&&hpBefore>0&&(hpAfter<=0||(flagsAfter&off::healthDead));
    const bool ko=!config.dry&&fist&&koBefore>0&&koAfter>=0&&koAfter<=0;
    bump(fist?stats.hitsFist:stats.hitsBlade);bump(stats.byZone[unsigned(std::clamp(zone,0,8))]);if(kill)bump(stats.kills);if(ko)bump(stats.kos);
    bump(stats.hands[unsigned(s.hand)].strikes);
    const float amp=kill?1.f:ko?.8f:fist?.55f:.8f;const std::uint32_t us=kill?90'000u:ko?70'000u:fist?45'000u:65'000u;
    haptics::pulse(unsigned(s.hand),amp,us,now,kill?haptics::SourceMeleeKill:haptics::SourceMeleeHit);
    bump(kill?stats.hapticKill:ko?stats.hapticKo:stats.hapticHit);
    LastStrike l{};l.ns=now;l.hand=s.hand;l.mode=s.mode;l.victimGuid=v.guid;l.race=v.race;l.zone=zone;l.component=comp;l.reaction=reaction;l.face=f.face;
    l.speed=s.speed;l.damage=damage;l.hpBefore=hpBefore;l.hpAfter=hpAfter;l.msgHpAfter=M::msg::get<float>(m,M::msg::hpAfter);l.koBefore=koBefore;l.koAfter=koAfter;
    l.flags=M::msg::get<std::uint32_t>(m,M::msg::flags);l.hitPos=hitPos;l.probeHit=probeHit;l.instMatch=instMatch;l.fx=fx;l.dry=config.dry;l.kill=kill;l.ko=ko;
    l.source=source;std::memcpy(l.bone,m+M::msg::boneName,16);l.bone[16]=0;l.phantom=phantomCut;
    {std::lock_guard lock(lastMutex);lastStrike=l;}
    {char b[256];const Vec3 to=M::normalized(p.position-v.position);
     std::snprintf(b,sizeof b,"{\"a\":[%.2f,%.2f,%.2f],\"c\":[%.2f,%.2f,%.2f],\"toPlayer\":[%.2f,%.2f,%.2f],\"dotA\":%.2f,\"dotC\":%.2f}",
        double(v.rowA.x),double(v.rowA.y),double(v.rowA.z),double(v.rowC.x),double(v.rowC.y),double(v.rowC.z),double(to.x),double(to.y),double(to.z),
        double(M::dot(v.rowA,to)),double(M::dot(v.rowC,to)));
     std::lock_guard lock(dumpMutex);forwardRow=b;}
}

inline void Engine::tick(const mt::Frame& frame) noexcept {
    const auto t0=nowNs();++tickIndex_;bump(stats.ticks);const std::uint64_t now=frame.now;
    // tape: always drained (a full queue would drop the NEWEST samples)
    std::array<TapeSample,64> samples{};unsigned ns=0;TapeSample t{};
    while(tape.pop(t)){bump(tapeConsumed);if(now>t.ns+150'000'000ull){bump(tapeStale);continue;}
        if(ns==samples.size()){std::move(samples.begin()+1,samples.end(),samples.begin());--ns;}samples[ns++]=t;}
    const auto vs=::rdrvr::vr::settings();
    // Physical fists = the fist path -> empty-hand triggers fully withheld; MELEE NATIVE / FISTS OFF -> the
    // engine's trigger punch is the only one: RT passes (STRIKE), FIRE dropped in the packet hook. FIST BLOCK needs the physical fists.
    const bool physicalFists=vs.physicalMelee()&&vs.meleeFists();
    H::nativeFistTrigger.store(!physicalFists&&hookInstalled.load(std::memory_order_relaxed),std::memory_order_relaxed);
    guard::active.store(physicalFists&&vs.fistBlock()&&api_.setBlockState!=nullptr,std::memory_order_relaxed);
    const auto setGate=[&](Gate g){stats.gate.store(g,std::memory_order_relaxed);bump(stats.gateTicks[g]);
        for(unsigned h=0;h<2;++h)if(hands_[h].mode!=Mode::None){resetHand(h,ResetGate);hands_[h].mode=Mode::None;stats.hands[h].mode.store(0);}
        setMasks(false,false);blockRelease(now);};
    if(!vs.physicalMelee()){setGate(GateNative);return;}
    if(!H::engineReady.load(std::memory_order_acquire)){setGate(GateNoHolsters);return;}
    const auto menuNs=H::menuSeenNs.load(std::memory_order_relaxed);
    if(menuNs&&(now<menuNs||now-menuNs<300'000'000ull)){setGate(GateMenu);return;}
    Player p{};
    if(const Gate g=readPlayer(p);g!=GateOk){setGate(g);return;}
    bool fresh=false;
    if(const Gate g=gate(p,fresh);g!=GateOk){setGate(g);return;}
    stats.gate.store(GateOk,std::memory_order_relaxed);bump(stats.gateTicks[GateOk]);
    if(config.inputs&&(tickIndex_&1u))logInputs(p);
    if(api_.lethalPunch){std::uint8_t lp{};if(get(api_.lethalPunch,0,lp))stats.lethalPunch.store(lp,std::memory_order_relaxed);}
    if(api_.ddaLevel&&(tickIndex_%30u)==0){std::int32_t d{};if(get(api_.ddaLevel,0,d))stats.ddaLevel.store(d,std::memory_order_relaxed);}
    // hand modes (holsters state; fist = empty VR hands + closed grip, Gothic vrgameplay.cpp:693)
    const int hold=H::holdHand.load(std::memory_order_acquire);
    const bool knife=H::heldKind.load(std::memory_order_relaxed)==kindMelee||H::currentSlot.load(std::memory_order_relaxed)==H::SlotMelee;
    const bool fistsOn=vs.meleeFists();
    const float speedSetting=vs.meleeSpeed();
    // DUAL: the phantom knife (second item, holstered for the engine) cuts with the same physical melee; fists only with both
    // hands empty.
    const int phantomHand=H::phantomHand.load(std::memory_order_acquire);
    const bool phantomKnife=phantomHand>=0&&H::phantomSlot.load(std::memory_order_relaxed)==H::SlotMelee&&H::dualActive();
    phantomKnife_=phantomKnife?phantomHand:-1;
    const auto modeFor=[&](unsigned h,float grip){
        if(hold==int(h)&&knife)return Mode::Blade;
        if(phantomKnife&&phantomHand==int(h))return Mode::Blade;
        if(fistsOn&&hold<0&&phantomHand<0&&grip>.65f)return Mode::Fist;
        return Mode::None;};
    for(unsigned h=0;h<2;++h)stats.hands[h].phantom.store(phantomKnife&&phantomHand==int(h),std::memory_order_relaxed);
    // FIST BLOCK: the guard pose (input brain) -> the engine block while it holds (both VR hands empty, FIST BLOCK on)
    blockTick(p,guard::active.load(std::memory_order_relaxed)&&!guard::killed.load(std::memory_order_relaxed)&&hold<0&&phantomHand<0&&
        guard::engagedFresh(now),now);
    // world map (newest plan) for every sample of this tick
    mapOk_=false;
    if(const auto fn=worldMapFn.load(std::memory_order_acquire)){WorldMap wm{};bool ok=false;try{ok=fn(wm,now);}catch(...){ok=false;}if(ok&&wm.valid){map_=wm;mapOk_=true;}}
    if(!mapOk_)bump(stats.mapMisses);
    if(mapOk_&&haveLastMap_&&(mapTurnDegrees(map_,lastMap_)>5.f||(map_.worldOrigin-lastMap_.worldOrigin).length()>1.f)){
        bump(stats.mapTurns);for(unsigned h=0;h<2;++h)resetHand(h,ResetMap);}
    if(mapOk_){lastMap_=map_;haveLastMap_=true;}
    // arm state from the newest sample (scan / mask), modes per sample below
    unsigned armed=0;std::array<Vec3,2> handsWorld{};
    if(ns){const auto& last=samples[ns-1];
        for(unsigned h=0;h<2;++h)if(modeFor(h,last.grips[h])!=Mode::None&&(last.valid>>h&1u)){armed|=1u<<h;
            if(mapOk_)handsWorld[h]=toWorld(map_,{last.grip[h][0],last.grip[h][1],last.grip[h][2]});}}
    else for(unsigned h=0;h<2;++h)if(hands_[h].mode!=Mode::None)armed|=1u<<h;
    // Knife in a VR hand or an armed fist: native strike off; FIRE off only for the engine knife / fists (DUAL: a phantom knife
    // next to an engine firearm keeps its trigger)
    setMasks(armed!=0||(hold>=0&&knife),(hold>=0&&knife)||(hold<0&&armed!=0));
    if(!armed){for(unsigned h=0;h<2;++h)if(hands_[h].mode!=Mode::None){resetHand(h,ResetMode);hands_[h].mode=Mode::None;stats.hands[h].mode.store(0);}
        if(tickIndex_%30u==0&&mapOk_)scan(p,{p.position,p.position},3u); // keep vr.melee.scan fresh for the report
        return;}
    bump(stats.activeTicks);
    if(mapOk_&&(tickIndex_-lastScan_>=3||!candCount_)){lastScan_=tickIndex_;scan(p,handsWorld,armed);}
    if(mapOk_&&candCount_)buildBodies(handsWorld);
    std::array<M::BodyRef,maxCandidates> refs{};for(unsigned i=0;i<candCount_;++i)refs[i]={&cand_[i].body,!cand_[i].bodyValid||cand_[i].skip};
    const auto query=[&](Vec3 a,Vec3 b,float r){return M::firstEntry(a,b,r,refs.data(),candCount_);};
    std::array<PendingStrike,2> strikes{};
    M::BodyFrame body{};if(mapOk_)body.update(map_.headLocal,map_.headForwardLocal);
    for(unsigned i=0;i<ns;++i){
        const auto& s=samples[i];
        if(!haveSpace_||s.epoch!=epoch_||s.space!=space_){if(haveSpace_)for(unsigned h=0;h<2;++h)resetHand(h,ResetSpace);epoch_=s.epoch;space_=s.space;haveSpace_=true;}
        if(mapOk_&&(s.epoch!=map_.epoch||s.space!=map_.space))continue; // a sample of another LOCAL space than the plan's
        for(unsigned h=0;h<2;++h){
            auto& hand=hands_[h];auto& hs=stats.hands[h];
            const Mode mode=modeFor(h,s.grips[h]);
            if(mode!=hand.mode){resetHand(h,ResetMode);hand.mode=mode;hs.mode.store(int(mode));}
            if(mode==Mode::None)continue;
            if(!(s.valid>>h&1u)){resetHand(h,ResetTracking);continue;}
            if(!mapOk_){bump(stats.rejects[RejectNoPlan]);continue;}
            bump(hs.samples);
            const Vec3 g{s.grip[h][0],s.grip[h][1],s.grip[h][2]},fw=M::normalized({s.forward[h][0],s.forward[h][1],s.forward[h][2]});
            const std::uint64_t us=s.ns/1000ull;
            const Candidate* nearest=candCount_?&cand_[0]:nullptr;
            if(mode==Mode::Blade){
                const Vec3 baseL=g+fw*config.bladeBase,tipL=g+fw*config.bladeTip;
                const M::WorldPose w{toWorld(map_,baseL),toWorld(map_,tipL),toWorld(map_,g),map_.headWorld};
                const Vec3 previousTip=hand.blade.previousTip;const bool hadPrevious=hand.blade.swing.valid;
                auto clear=[&](Vec3 from,Vec3 to){return !(from.x==w.head.x&&from.y==w.head.y&&from.z==w.head.z)||wallClear(hand,h,p,nearest,from,to);};
                const auto r=M::bladeStep(hand.blade,w,tipL,body,1.f,map_.units,us,true,speedSetting,tipL,g,config.bladeRadius,config.bladeStep,query,query,clear);
                const auto& sw=hand.blade.swing;
                if(r.rejectedEntry)bump(stats.rejects[!sw.continuous?RejectDiscontinuous:sw.speed<speedSetting?RejectSlow:RejectNoTravel]);
                if(!r.entry&&r.overlap.hit()&&sw.contactReady)bump(stats.rejects[RejectInside]);
                if(r.wallBlocked)bump(stats.rejects[RejectWall]);
                if(r.struck&&strikes[h].hand<0&&hadPrevious)
                    strikes[h]={int(h),Mode::Blade,r.hit.target,r.hit.part,r.hit.point,w.tip-previousTip,sw.speed};
                hs.speed.store(sw.speed,std::memory_order_relaxed);maxOf(hs.maxSpeed,sw.speed);hs.contactTravel.store(sw.contactTravel,std::memory_order_relaxed);
                hs.continuous.store(sw.continuous,std::memory_order_relaxed);hs.ready.store(sw.contactReady,std::memory_order_relaxed);
                hs.inside.store(hand.blade.insideTarget,std::memory_order_relaxed);
            }else{
                const Vec3 tipL=g+fw*config.fistTip;const Vec3 tipW=toWorld(map_,tipL);
                const M::WorldPose w{tipW,tipW,toWorld(map_,g),map_.headWorld};
                const Vec3 previousTip=hand.fist.previousTip;
                auto clear=[&](Vec3 from,Vec3 to){return wallClear(hand,h,p,nearest,from,to);};
                const auto r=M::fistStep(hand.fist,w,tipL,body,1.f,map_.units,us,true,tipL,config.fistOffset,config.fistRadius,query,clear);
                const auto& sw=hand.fist.swing;
                if(r.wallBlocked)bump(stats.rejects[RejectWall]);
                if(!r.windowOpen&&sw.continuous&&sw.speed>=1.6f&&query(previousTip,tipW,config.fistRadius*map_.units).hit())bump(stats.rejects[RejectCooldown]);
                if(r.struck&&strikes[h].hand<0)strikes[h]={int(h),Mode::Fist,r.hit.target,r.hit.part,r.hit.point,tipW-previousTip,sw.speed};
                hs.speed.store(sw.speed,std::memory_order_relaxed);maxOf(hs.maxSpeed,sw.speed);hs.contactTravel.store(sw.contactTravel,std::memory_order_relaxed);
                hs.continuous.store(sw.continuous,std::memory_order_relaxed);hs.ready.store(r.windowOpen,std::memory_order_relaxed);hs.inside.store(false);
            }
        }
    }
    for(const auto& s:strikes)if(s.hand>=0){try{apply(s,p,now);}catch(...){bump(stats.exceptions);}}
    const double us=double(nowNs()-t0)/1000.0;bump(stats.tickUsSum,std::uint64_t(us));maxOf(stats.tickUsMax,us);
    (void)fresh;
}

// ============================== status (VR page row MELEE) ==============================
inline void publishStatus() noexcept {
    using namespace ::rdrvr::vr;
    if(killed(Feature::Melee)){status::publish(Feature::Melee,status::Property);return;}
    if(!engineReady.load(std::memory_order_acquire)){status::publish(Feature::Melee,status::Unavailable);return;}
    const unsigned g=stats.gate.load(std::memory_order_relaxed);
    status::publish(Feature::Melee,status::Ready,g==GateNative?nullptr:gateNotes[g<GateCount?g:GateContext]);
}
inline void tickThunk(const mt::Frame& f) noexcept {
    if(!engineReady.load(std::memory_order_acquire))return;
    try{engine.tick(f);}catch(...){bump(stats.exceptions);}
    publishStatus();
}

// ============================== install (TraceEntry, arm; once) ==============================
// code(offset, print) = executable bytes match; data(offset, print) = readable bytes match; got(slot, target) = *(base+slot) ==
// base+target (GOT entries and relocated vtable slots); replace(address, expected, replacement) = TraceEntry replacePointer.
template<class Code,class Data,class Got,class Replace>
inline bool install(EngineApi api,Code&& code,Data&& data,Got&& got,Replace&& replace) noexcept {
    using namespace ::rdrvr::vr;
    startup();
    if(killed(Feature::Melee)){installState.store("KILLED");status::publish(Feature::Melee,status::Property);return false;}
    const auto fail=[&](const char* why){installState.store(why);engineReady.store(false,std::memory_order_release);
        status::publish(Feature::Melee,status::Unavailable);return false;};
    if(!api.read||!api.base||!api.gameData||!api.playerManager||!api.playerManagerTable||!api.playerTable)return fail("NO ENGINE API");
    const auto b=api.base;
    struct Print {std::uintptr_t offset;const unsigned char* bytes;std::size_t size;const char* name;};
#define RDRVR_MELEE_PRINT(o,p,n) Print{o,p.data(),p.size(),n}
    const Print prints[]{RDRVR_MELEE_PRINT(strikeDamageRemoteOffset,strikeDamageRemoteFingerprint,"PRINT StrikeDamageRemote"),
        RDRVR_MELEE_PRINT(broadcastInjureOffset,broadcastInjureFingerprint,"PRINT Broadcast(Injure)"),
        RDRVR_MELEE_PRINT(broadcastImpulseOffset,broadcastImpulseFingerprint,"PRINT Broadcast(Impulse)"),
        RDRVR_MELEE_PRINT(addNewEventOffset,addNewEventFingerprint,"PRINT AddNewEvent"),
        RDRVR_MELEE_PRINT(doStrikeAudioOffset,doStrikeAudioFingerprint,"PRINT DoStrikeAudio"),
        RDRVR_MELEE_PRINT(setFromTestProbeOffset,setFromTestProbeFingerprint,"PRINT SetFromTestProbe"),
        RDRVR_MELEE_PRINT(sweptSphereOneOffset,sweptSphereOneFingerprint,"PRINT SweptSphereOneObject"),
        RDRVR_MELEE_PRINT(sweptSphereMultipleOffset,sweptSphereMultipleFingerprint,"PRINT SweptSphereMultiple"),
        RDRVR_MELEE_PRINT(weaponBaseTypeOffset,weaponBaseTypeFingerprint,"PRINT GetWeaponBaseType"),
        RDRVR_MELEE_PRINT(setupActionTreePacketOffset,setupActionTreePacketFingerprint,"PRINT SetupActionTreePacket"),
        RDRVR_MELEE_PRINT(hitEffectIndexOffset,hitEffectIndexFingerprint,"PRINT GetHitEffect"),
        RDRVR_MELEE_PRINT(instSkeletonOffset,instSkeletonFingerprint,"PRINT entEntityInst::GetSkeleton"),
        RDRVR_MELEE_PRINT(fragSkeletonOffset,fragSkeletonFingerprint,"PRINT fragInst::GetSkeleton"),
        RDRVR_MELEE_PRINT(receiveStrikeOffset,receiveStrikeFingerprint,"PRINT ReceiveStrike")};
#undef RDRVR_MELEE_PRINT
    for(const auto& pr:prints){bool ok=false;try{ok=code(pr.offset,pr.bytes,pr.size);}catch(...){ok=false;}if(!ok)return fail(pr.name);}
    {bool ok=false;try{ok=data(packetTablesOffset,packetTablesFingerprint.data(),packetTablesFingerprint.size());}catch(...){}if(!ok)return fail("DATA packet tables");}
    struct Word {std::uintptr_t slot,target;const char* name;};
    const Word words[]{{actorManagerGotOffset,actorManagerOffset,"GOT actor manager"},{actorSlotsGotOffset,actorSlotsOffset,"GOT actor slots"},
        {lethalPunchGotOffset,lethalPunchOffset,"GOT lethal punch"},{ddaLevelGotOffset,ddaLevelOffset,"GOT dda level"},
        {aniMeleeVtableGotOffset,aniMeleeVtableOffset,"GOT aniMelee vtable"},{healthVtableGotOffset,healthVtableOffset,"GOT health vtable"},
        {entityVtableGotOffset,entityVtableOffset,"GOT entity vtable"},{creatureVtableGotOffset,creatureVtableOffset,"GOT creature vtable"},
        {footVtableGotOffset,footVtableOffset,"GOT foot vtable"},{isCreatureSlotOffset,isCreatureOffset,"VTABLE IsCreature"},
        {instGetSkeletonSlotOffset,instSkeletonOffset,"VTABLE GetSkeleton"},{footGetInputSlotOffset,getInputOffset,"VTABLE GetInput"},
        {setupActionTreePacketGotOffset,setupActionTreePacketOffset,"GOT SetupActionTreePacket"}};
    for(const auto& wd:words){bool ok=false;try{ok=got(wd.slot,wd.target);}catch(...){ok=false;}if(!ok)return fail(wd.name);}
    api.actorManager=b+actorManagerOffset;api.actorSlots=b+actorSlotsOffset;api.lethalPunch=b+lethalPunchOffset;api.ddaLevel=b+ddaLevelOffset;
    api.aniMeleeVtable=b+aniMeleeVtableOffset+vtableAddressPoint;api.healthVtable=b+healthVtableOffset+vtableAddressPoint;
    api.entityVtable=b+entityVtableOffset+vtableAddressPoint;api.isCreatureFn=b+isCreatureOffset;api.instSkeletonFn=b+instSkeletonOffset;
    api.footVtable=b+footVtableOffset+vtableAddressPoint;
    api.strike=reinterpret_cast<StrikeFn>(b+strikeDamageRemoteOffset);api.broadcast=reinterpret_cast<BroadcastFn>(b+broadcastInjureOffset);
    api.impulse=reinterpret_cast<BroadcastFn>(b+broadcastImpulseOffset);api.addEvent=reinterpret_cast<AddEventFn>(b+addNewEventOffset);
    api.audio=reinterpret_cast<StrikeAudioFn>(b+doStrikeAudioOffset);api.hitInfo=reinterpret_cast<HitInfoFn>(b+setFromTestProbeOffset);
    api.probeOne=reinterpret_cast<ProbeOneFn>(b+sweptSphereOneOffset);api.probeLevel=reinterpret_cast<ProbeLevelFn>(b+sweptSphereMultipleOffset);
    api.weaponType=reinterpret_cast<WeaponTypeFn>(b+weaponBaseTypeOffset);
    if(!api.complete())return fail("API INCOMPLETE");
    { // DUAL: optional knife FX for the phantom knife (64-byte head); a mismatch keeps melee and only drops that FX
        bool ok=false;try{ok=code(doKnifeFxOffset,doKnifeFxFingerprint.data(),doKnifeFxFingerprint.size());}catch(...){ok=false;}
        api.knifeFx=ok?reinterpret_cast<KnifeFxFn>(b+doKnifeFxOffset):nullptr;}
    { // FIST BLOCK: optional (whole functions); a mismatch keeps melee and only makes FIST BLOCK unavailable
        bool ok=false;try{ok=code(setBlockStateOffset,setBlockStateFingerprint.data(),setBlockStateFingerprint.size());}catch(...){ok=false;}
        api.setBlockState=ok?reinterpret_cast<SetBlockStateFn>(b+setBlockStateOffset):nullptr;
        ok=false;try{ok=api.setBlockState&&code(deScheduleBlockOffset,deScheduleBlockFingerprint.data(),deScheduleBlockFingerprint.size());}catch(...){ok=false;}
        api.deScheduleBlock=ok?reinterpret_cast<DeScheduleBlockFn>(b+deScheduleBlockOffset):nullptr;
        blockApiState.store(api.setBlockState?(api.deScheduleBlock?"OK":"NO DESCHEDULE"):"PRINT SetBlockState",std::memory_order_relaxed);}
    engine.configure(api);
    if(!mt::registerModule(mt::ModuleMelee,"melee",tickThunk,nullptr))return fail("REGISTER FAILED");
    packetOriginal.store(reinterpret_cast<SetupPacketFn>(b+setupActionTreePacketOffset),std::memory_order_release);
    bool hooked=false;try{hooked=replace(b+setupActionTreePacketGotOffset,b+setupActionTreePacketOffset,reinterpret_cast<std::uintptr_t>(&setupPacketHook));}catch(...){}
    if(!hooked)return fail("HOOK NOT INSTALLED");
    hookInstalled.store(true,std::memory_order_release);
    tapeEnabled.store(true,std::memory_order_release);
    installState.store(config.dry?"INSTALLED DRY":"INSTALLED");engineReady.store(true,std::memory_order_release);
    status::publish(Feature::Melee,status::Ready,nullptr);
    return true;
}

// ============================== report "vr":{"melee":...} (any thread) ==============================
inline std::string report() {
    using namespace ::rdrvr::vr;
    std::string out;out.reserve(8192);char b[2048];const auto now=nowNs();
    const auto vs=settings();const auto& c=config;const unsigned g=stats.gate.load();
    const auto ticks=stats.ticks.load(),scans=stats.scans.load();
    std::snprintf(b,sizeof b,"{\"enabled\":%s,\"mode\":\"%s\",\"api\":{\"ok\":%s,\"state\":\"%s\",\"hook\":%s},"
        "\"config\":{\"speed\":%.1f,\"fists\":%s,\"blade\":[%.3f,%.3f,%.3f],\"step\":%.3f,\"fist\":[%.3f,%.3f],\"dmg\":{\"torso\":%.2f,\"limb\":%.2f,"
        "\"fistKo\":%.2f,\"fistKoStrong\":%.2f,\"lethal\":%.0f},\"horses\":%s,\"push\":%.1f,\"wall\":%s,\"fx\":%s,\"probe\":%s,\"m2\":%s,\"dry\":%s,"
        "\"reach\":%.1f,\"cooldownMs\":%.0f},",
        killed(Feature::Melee)?"false":"true",vs.physicalMelee()?"PHYSICAL":"NATIVE",engineReady.load()?"true":"false",installState.load(),
        hookInstalled.load()?"true":"false",double(vs.meleeSpeed()),vs.meleeFists()?"true":"false",double(c.bladeBase),double(c.bladeTip),
        double(c.bladeRadius),double(c.bladeStep),double(c.fistTip),double(c.fistRadius),double(c.damage.torso),double(c.damage.limb),
        double(c.damage.fistKo),double(c.damage.fistKoStrong),double(c.damage.lethal),c.horses?"true":"false",double(c.push),c.wall?"true":"false",
        c.fx?"true":"false",c.probe?"true":"false",c.m2?"true":"false",c.dry?"true":"false",double(c.reach),double(c.victimCooldownMs));out+=b;
    std::snprintf(b,sizeof b,"\"gate\":{\"state\":\"%s\",\"reason\":\"%s\",\"byReason\":{",g==GateOk?"OPEN":"BLOCKED",gateNames[g<GateCount?g:0]);out+=b;
    {bool first=true;for(unsigned i=0;i<GateCount;++i){const auto n=stats.gateTicks[i].load();if(!n)continue;
        std::snprintf(b,sizeof b,"%s\"%s\":%llu",first?"":",",gateNames[i],(unsigned long long)n);out+=b;first=false;}}
    const auto last=tapeLastNs.load();
    std::snprintf(b,sizeof b,"}},\"tape\":{\"enabled\":%s,\"pushed\":%llu,\"dropped\":%llu,\"consumed\":%llu,\"stale\":%llu,\"lastAgeMs\":%.1f},"
        "\"tick\":{\"ticks\":%llu,\"active\":%llu,\"usAvg\":%.1f,\"usMax\":%.1f,\"exceptions\":%llu,\"mapMisses\":%llu,\"mapTurns\":%llu},\"hands\":[",
        tapeEnabled.load()?"true":"false",(unsigned long long)tapePushed.load(),(unsigned long long)tapeDropped.load(),
        (unsigned long long)tapeConsumed.load(),(unsigned long long)tapeStale.load(),last&&now>=last?double(now-last)/1e6:-1.0,
        (unsigned long long)ticks,(unsigned long long)stats.activeTicks.load(),stats.activeTicks.load()?double(stats.tickUsSum.load())/double(stats.activeTicks.load()):0.0,
        stats.tickUsMax.load(),(unsigned long long)stats.exceptions.load(),(unsigned long long)stats.mapMisses.load(),(unsigned long long)stats.mapTurns.load());out+=b;
    for(unsigned h=0;h<2;++h){const auto& hs=stats.hands[h];
        std::snprintf(b,sizeof b,"%s{\"mode\":\"%s\",\"speed\":%.2f,\"maxSpeed\":%.2f,\"continuous\":%s,\"contactTravel\":%.3f,\"ready\":%s,\"inside\":%s,"
            "\"samples\":%llu,\"strikes\":%llu,\"wallBlocked\":%llu,\"source\":\"%s\",\"resets\":{",h?",":"",modeName(Mode(hs.mode.load())),double(hs.speed.load()),
            double(hs.maxSpeed.load()),hs.continuous.load()?"true":"false",double(hs.contactTravel.load()),hs.ready.load()?"true":"false",
            hs.inside.load()?"true":"false",(unsigned long long)hs.samples.load(),(unsigned long long)hs.strikes.load(),(unsigned long long)hs.blocked.load(),
            hs.phantom.load()?"PHANTOM":"ENGINE");out+=b;
        for(unsigned r=0;r<ResetCount;++r){std::snprintf(b,sizeof b,"%s\"%s\":%llu",r?",":"",resetNames[r],(unsigned long long)hs.resets[r].load());out+=b;}
        out+="}}";}
    std::snprintf(b,sizeof b,"],\"scan\":{\"scans\":%llu,\"slots\":%llu,\"actorsSeen\":%llu,\"inRange\":%llu,\"maxInRange\":%u,\"candidates\":%u,"
        "\"usAvg\":%.1f,\"usMax\":%.1f,\"skipped\":{",(unsigned long long)scans,(unsigned long long)stats.slotsScanned.load(),
        (unsigned long long)stats.actorsSeen.load(),(unsigned long long)stats.inRange.load(),stats.maxInRange.load(),stats.candidates.load(),
        scans?double(stats.scanUsSum.load())/double(scans):0.0,stats.scanUsMax.load());out+=b;
    for(unsigned i=0;i<SkipCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",skipNames[i],(unsigned long long)stats.skipped[i].load());out+=b;}
    std::snprintf(b,sizeof b,"}},\"bones\":{\"skeletons\":%llu,\"failures\":%llu,\"cachedLayouts\":%llu,\"bodies\":%llu,\"humanoid\":%llu,\"cloud\":%llu,"
        "\"actorCapsule\":%llu,\"missing\":{",(unsigned long long)stats.skeletons.load(),(unsigned long long)stats.skeletonFailures.load(),
        (unsigned long long)stats.layoutsResolved.load(),(unsigned long long)stats.bodiesBuilt.load(),(unsigned long long)stats.humanoidBodies.load(),
        (unsigned long long)stats.cloudBodies.load(),(unsigned long long)stats.actorBodies.load());out+=b;
    {bool first=true;for(unsigned k=0;k<M::BoneKeyCount;++k){const auto n=stats.missing[k].load();if(!n)continue;
        std::snprintf(b,sizeof b,"%s\"%s\":%llu",first?"":",",M::boneKeyNames[k],(unsigned long long)n);out+=b;first=false;}}
    {std::lock_guard lock(dumpMutex);out+="},\"names\":{\"human\":";out+=boneDumpHuman.empty()?"[]":boneDumpHuman;
     out+=",\"other\":";out+=boneDumpOther.empty()?"[]":boneDumpOther;out+="},\"forwardRow\":";out+=forwardRow.empty()?"null":forwardRow;out+="},";}
    std::snprintf(b,sizeof b,"\"hits\":{\"blade\":%llu,\"fist\":%llu,\"kills\":%llu,\"kos\":%llu,\"immune\":%llu,\"dry\":%llu,\"byZone\":[",
        (unsigned long long)stats.hitsBlade.load(),(unsigned long long)stats.hitsFist.load(),(unsigned long long)stats.kills.load(),
        (unsigned long long)stats.kos.load(),(unsigned long long)stats.immune.load(),(unsigned long long)stats.dryHits.load());out+=b;
    for(unsigned i=0;i<9;++i){std::snprintf(b,sizeof b,"%s%llu",i?",":"",(unsigned long long)stats.byZone[i].load());out+=b;}
    out+="],\"byReaction\":[";
    for(unsigned i=0;i<9;++i){std::snprintf(b,sizeof b,"%s%llu",i?",":"",(unsigned long long)stats.byReaction[i].load());out+=b;}
    out+="]},\"rejects\":{";
    for(unsigned i=0;i<RejectCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",rejectNames[i],(unsigned long long)stats.rejects[i].load());out+=b;}
    std::snprintf(b,sizeof b,"},\"engine\":{\"strikeRemote\":%llu,\"broadcastOnly\":%llu,\"fxKnife\":%llu,\"fxPunch\":%llu,\"fxSkipped\":%llu,"
        "\"impulses\":%llu,\"reactions\":%llu,\"probe\":{\"calls\":%llu,\"hits\":%llu,\"misses\":%llu,\"badComponent\":%llu,\"instMatch\":%llu,"
        "\"boneComponents\":%llu},\"wall\":{\"calls\":%llu,\"blocked\":%llu},\"packet\":{\"calls\":%llu,\"maskWanted\":%s,\"maskStrike\":%s,\"maskFire\":%s,"
        "\"maskedFrames\":%llu,\"masked\":{\"strike\":%llu,\"fire\":%llu},\"latch\":{\"strike\":%s,\"fire\":%s,\"frames\":%llu}},\"lethalPunchFlag\":%d,\"ddaLevel\":%d,"
        "\"phantomKnifeFx\":%llu,\"phantomHits\":%llu,\"knifeFxApi\":%s,"
        "\"holsters\":{\"holdHand\":%d,\"heldKind\":%d,\"slot\":%d,\"rtBlockedMelee\":%llu,\"phantomHand\":%d,\"phantomSlot\":%d}},",
        (unsigned long long)stats.strikeRemote.load(),(unsigned long long)stats.broadcastOnly.load(),(unsigned long long)stats.fxKnife.load(),
        (unsigned long long)stats.fxPunch.load(),(unsigned long long)stats.fxSkipped.load(),(unsigned long long)stats.impulses.load(),
        (unsigned long long)stats.reactions.load(),(unsigned long long)stats.probeCalls.load(),(unsigned long long)stats.probeHits.load(),
        (unsigned long long)stats.probeMisses.load(),(unsigned long long)stats.probeBadComponent.load(),(unsigned long long)stats.probeInstMatch.load(),
        (unsigned long long)stats.boneComponents.load(),(unsigned long long)stats.wallCalls.load(),(unsigned long long)stats.wallBlocked.load(),
        (unsigned long long)packetCalls.load(),maskWanted.load()?"true":"false",maskStrikeWanted.load()?"true":"false",maskFireWanted.load()?"true":"false",
        (unsigned long long)maskedFrames.load(),
        (unsigned long long)maskedStrike.load(),(unsigned long long)maskedFire.load(),latchStrike.load()?"true":"false",latchFire.load()?"true":"false",
        (unsigned long long)latchedFrames.load(),stats.lethalPunch.load(),stats.ddaLevel.load(),(unsigned long long)phantomKnifeFx.load(),
        (unsigned long long)phantomHits.load(),engine.api().knifeFx?"true":"false",H::holdHand.load(),H::heldKind.load(),
        H::currentSlot.load(),(unsigned long long)H::rtBlockedMelee.load(),H::phantomHand.load(),H::phantomSlot.load());out+=b;
    LastStrike l{};{std::lock_guard lock(lastMutex);l=lastStrike;}
    for(char& ch:l.bone)if(ch&&(ch=='"'||ch=='\\'||static_cast<unsigned char>(ch)<0x20||static_cast<unsigned char>(ch)>0x7e))ch='?';
    std::snprintf(b,sizeof b,"\"last\":{\"ageMs\":%.0f,\"hand\":%d,\"mode\":\"%s\",\"victimGuid\":%u,\"race\":%d,\"zone\":%d,\"zoneName\":\"%s\",\"bone\":\"%s\","
        "\"component\":%d,\"source\":\"%s\",\"speed\":%.2f,\"damageIn\":%.1f,\"flags\":%u,\"hpBefore\":%.1f,\"hpAfter\":%.1f,\"msgHpAfter\":%.1f,"
        "\"koBefore\":%.1f,\"koAfter\":%.1f,\"kill\":%s,\"ko\":%s,\"reaction\":\"%s\",\"face\":%d,\"hitPos\":[%.2f,%.2f,%.2f],"
        "\"probe\":{\"hit\":%s,\"instMatch\":%s},\"fx\":%s,\"dry\":%s,\"phantom\":%s},",
        l.ns&&now>=l.ns?double(now-l.ns)/1e6:-1.0,l.hand,modeName(l.mode),l.victimGuid,l.race,l.zone,l.zone>=0&&l.zone<=8?M::zoneNames[unsigned(l.zone)]:"NONE",
        l.bone,l.component,l.source,double(l.speed),double(l.damage),l.flags,double(l.hpBefore),double(l.hpAfter),double(l.msgHpAfter),
        double(l.koBefore),double(l.koAfter),l.kill?"true":"false",l.ko?"true":"false",M::reactionNames[unsigned(std::clamp(l.reaction,0,8))],l.face,
        double(l.hitPos.x),double(l.hitPos.y),double(l.hitPos.z),l.probeHit?"true":"false",l.instMatch?"true":"false",l.fx?"true":"false",l.dry?"true":"false",
        l.phantom?"true":"false");out+=b;
    { // FIST BLOCK + empty-hand FIRE mask
        const auto u=[](const Counter& c){return (unsigned long long)c.load(std::memory_order_relaxed);};const auto lh=stats.blockLastHitNs.load();
        std::snprintf(b,sizeof b,"\"block\":{\"enabled\":%s,\"killed\":%s,\"api\":\"%s\",\"guard\":\"%s\",\"wanted\":%llu,\"frames\":%llu,\"engages\":%llu,"
            "\"reasserts\":%llu,\"releases\":%llu,\"dropped\":%llu,\"noMelee\":%llu,\"noApi\":%llu,\"nativeBlock\":%llu,\"hitsBlocked\":%llu,"
            "\"impactEdges\":%llu,\"meterHits\":%llu,\"impactClears\":%llu,\"haptics\":%llu,\"lastHitAgeMs\":%.0f,\"flags\":%d,\"meter\":%.2f,"
            "\"meterMax\":%.2f},\"emptyFire\":{\"enabled\":%s,\"frames\":%llu},",
            guard::active.load()?"true":"false",guard::killed.load()?"true":"false",blockApiState.load(),guard::stateName(guard::state.load()),
            u(stats.blockWanted),u(stats.blockFrames),u(stats.blockEngages),u(stats.blockReasserts),u(stats.blockReleases),u(stats.blockDropped),
            u(stats.blockNoMelee),u(stats.blockNoApi),u(stats.blockNative),u(stats.blockHits),u(stats.blockImpactEdges),u(stats.blockMeterHits),
            u(stats.blockImpactClears),u(stats.blockHaptics),lh&&now>=lh?double(now-lh)/1e6:-1.0,stats.blockFlags.load(),double(stats.blockMeter.load()),
            double(stats.blockMeterMax.load()),emptyFireMask.load()?"true":"false",u(emptyFireFrames));out+=b;
    }
    std::snprintf(b,sizeof b,"\"haptics\":{\"hit\":%llu,\"kill\":%llu,\"ko\":%llu,\"immune\":%llu},\"inputs\":{\"samples\":%llu,\"failures\":%llu,\"down\":{",
        (unsigned long long)stats.hapticHit.load(),(unsigned long long)stats.hapticKill.load(),(unsigned long long)stats.hapticKo.load(),
        (unsigned long long)stats.hapticImmune.load(),(unsigned long long)inputSamples.load(),(unsigned long long)inputFailures.load());out+=b;
    {bool first=true;for(unsigned i=0;i<off::footInputs;++i){const auto n=inputDown[i].load();if(!n)continue;
        std::snprintf(b,sizeof b,"%s\"%u\":%u",first?"":",",i,n);out+=b;first=false;}}
    out+="},\"pairs\":{";
    {static constexpr const char* padNames[padBits]{"A","B","X","Y","MENU","L3","R3","LGRIP","RGRIP","LT_BIT","RT_BIT","DPAD_UP","RT","LT"};
     bool first=true;for(unsigned p=0;p<padBits;++p){bool any=false;
        for(unsigned i=0;i<off::footInputs;++i){const auto n=inputPairs[p][i].load();if(!n)continue;
            if(!any){std::snprintf(b,sizeof b,"%s\"%s\":{",first?"":",",padNames[p]);out+=b;first=false;any=true;}else out+=',';
            std::snprintf(b,sizeof b,"\"%u\":%u",i,n);out+=b;}
        if(any)out+='}';}}
    out+="}}}";
    return out;
}
} // namespace rdrvr::vr::melee
