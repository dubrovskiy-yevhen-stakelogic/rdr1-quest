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
#include "VrHandPhysMath.h"
#include "VrGrabLink.h"
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
    if(parseFloats(property("debug.rdrvr.vr.melee.slapko"),&one,1,0.f,1.f))c.damage.slapKo=one;
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
    RejectNoComponent,RejectProbeMiss,RejectInvulnerable,RejectDead,RejectStale,RejectNoSkeleton,RejectMessage,RejectHandOnly,RejectCount};
inline constexpr std::array<const char*,RejectCount> rejectNames{"SLOW","NO_TRAVEL","INSIDE","DISCONTINUOUS","NO_PLAN","WALL","COOLDOWN",
    "SAME_VICTIM","NO_COMPONENT","PROBE_MISS","INVULNERABLE","DEAD","STALE_VICTIM","NO_SKELETON","MESSAGE","HAND_ONLY"};
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
inline constexpr unsigned nodeResultCount=18; // NodeCount (static_assert at NodeResult)
struct Stats {
    std::atomic<unsigned> gate{GateNoEngine};std::array<Counter,GateCount> gateTicks{};
    Counter ticks{},activeTicks{},exceptions{};std::atomic<double> tickUsMax{};Counter tickUsSum{};
    std::array<HandStats,2> hands{};
    Counter scans{},slotsScanned{},actorsSeen{},inRange{};std::atomic<unsigned> maxInRange{},candidates{};std::array<Counter,SkipCount> skipped{};
    Counter scanUsSum{};std::atomic<double> scanUsMax{};
    Counter skeletons{},skeletonFailures{},layoutsResolved{},bodiesBuilt{},humanoidBodies{},cloudBodies{},actorBodies{};
    std::array<Counter,M::BoneKeyCount> missing{};
    Counter hitsBlade{},hitsFist{},kills{},kos{},immune{},dryHits{};std::array<Counter,9> byZone{},byReaction{};
    // the fist gate (M::FistGate): samples of a closed grip still waiting for its hold, fists opened by a slow closing / by the hold;
    // per fist strike, how long the fist had been closed (<100, 100-200, 200-500, 500+ ms) and the grip's lowest value in the 300 ms
    // before it (<.3, .3-.5, .5-.65, over .65)
    Counter fistWaiting{},fistOpenSlow{},fistOpenHeld{};std::array<Counter,4> fistClosedMsHist{},fistMinGripHist{};
    std::array<Counter,RejectCount> rejects{};
    Counter strikeRemote{},broadcastOnly{},fxKnife{},fxPunch{},fxSkipped{},impulses{},reactions{},probeCalls{},probeHits{},probeMisses{},
        probeBadComponent{},probeInstMatch{},boneComponents{},wallCalls{},wallBlocked{},mapMisses{},mapTurns{};
    Counter hapticHit{},hapticKill{},hapticKo{},hapticImmune{};
    std::atomic<int> lethalPunch{-1},ddaLevel{-1};
    // FIST BLOCK (engine side, main thread)
    Counter blockWanted{},blockFrames{},blockEngages{},blockReasserts{},blockReleases{},blockDropped{},blockNoMelee{},blockNoApi{},
        blockNative{},blockHits{},blockImpactEdges{},blockMeterHits{},blockImpactClears{},blockHaptics{};
    // HAND PHYSICS: the virtual bodies (bone reads for the creatures near a palm) and the open-hand touches it hands over
    Counter hpBodyTicks{},hpBuilt{},hpReadUsSum{},hpReadN{},hpHorses{},hpPublished{};std::atomic<double> hpReadUsMax{};
    Counter hpPropOnly{}; // body ticks kept only for a held heavy prop (no live palm)
    Counter touchThrown{},touchThrownSent{},touchConsumed{},touchStale{},touchNotFound{},touchHandOnly{},touchGateClosed{},touchNotReady{},touchInvulnerable{},touchMsgSent{},touchFakeGuidFail{};
    Counter touchSlaps{},touchShoves{},touchPresses{},pushApplied{},pushTicks{},yawTicks{},offsetUmSum{},maxPressUm{};
    // presses whose step the arm limit cut (armStepM under the depth law's step) and those it cut to nothing (lever npcpassive)
    Counter pressArmLimited{},pressArmZero{};
    std::array<Counter,6> pushRefused{};std::array<Counter,9> touchReaction{};
    // the push diagnostics: the mover class met (base / sim / another, its vtable - base), offsets turned by a heading (the last
    // heading), offsets stored at +0x860 after the call / not, consumed by the next tick / not, a finished push job's asked and moved
    // distance (horizontal); the injure message reaching the animator (NotifyInjured copied msg +0x10..+0x6f to +0x840) or not
    Counter moverBase{},moverSim{},moverOther{},headingTurned{},pushStored{},pushNotStored{},pushConsumed{},pushNotConsumed{},
        jobsMeasured{},jobAskedUmSum{},jobMovedUmSum{},msgLanded{},msgNotLanded{};
    std::atomic<std::uint64_t> moverOtherRel{};std::atomic<float> headingLast{},jobAskedLastCm{-1.f},jobMovedLastCm{-1.f};
    std::atomic<float> blockMeter{},blockMeterMax{};std::atomic<int> blockFlags{-1};std::atomic<std::uint64_t> blockLastHitNs{};
    // open-hand touches by variant (fake, self, none, node) and path (strike, player broadcast: both never for an open hand; self, fake,
    // none, node), self messages refused for a zombie (or no check), the message's component (probed, from the bone, none); node plays by
    // result; the node watch: started, dropped, walk bumps watched, and per kind (slap, push, fist, press, walk) and variant (fake, self,
    // none, node, player) whether the creature's action node changed within watchNs
    Counter touchSelfRefused{},touchProbeComp{},touchBoneComp{},touchNoComp{},watchStarted{},watchDropped{},walkBumps{};
    std::array<Counter,5> touchByVariant{};std::array<Counter,6> touchPath{};std::array<Counter,nodeResultCount> nodeBy{};
    std::array<std::array<Counter,5>,6> watchReacted{},watchNotReacted{};
    // open-hand shoves by tier (light: escalated presses, medium, firm) and the injure messages each sent; projMsgApplyImpulse kicks after a
    // firm push's ragdoll (lever pushkick); the player's punch impulse value (aniMeleeActionData +0xd4) the last fist strike / push read;
    // the push messages' impulse by source (lever, k, the fallback)
    std::array<Counter,3> shoveTier{},shoveTierMsg{},pushImpulseBy{};Counter pushKicks{};std::atomic<float> punchImpulseK{-1.f};
    // the push watches: pushes / slaps that found no watch slot (no kick, no ring entry), the kick's end state per watched push (KickState),
    // kicks after which the creature lost hp (the kick then stays off for it), armed kicks by direction source (palm, into, none)
    Counter pushDropped{},kickHpLoss{};std::array<Counter,15> kickBy{};std::array<Counter,3> kickSrc{};
    // graded pushes (lever pushgrade): stumbles, knockdowns, stumbles a second hand turned into a knockdown, second hands merged into one
    // push, a hand's repeat on the same creature within 300 ms (dropped), stumbles re-planned to a pair's distance; second kicks sent /
    // not wanted (the pelvis followed) / refused or failed
    Counter gradeStumbles{},gradeKnockdowns{},gradeUpgrades{},gradePairMerged{},gradeRepeat{},gradeReplanned{},kick2Sent{},kick2NotWanted{},kick2Failed{};
    // PUSH FIX: stumbles queued to wait on a frozen / suspended mover (lever pushbusywait), waits that ended with the mover free / at the
    // deadline, walk-in bumps asked for them; jobs merged into a running job of the same push / refused for a full queue; escalated presses
    // graded (lever pushescgrade) and merged into a live push; stroke peaks that regraded a push / upgraded it to a knockdown / found no push;
    // knockdowns by class (behind, front, side a+, side a-) x node (staggerfall_back, staggerfall_front, #bc3dfb68, other) x outcome
    // (along: fall600 >= .5 and 20 cm or more, against: <= -.5, side, none)
    Counter stumbleWaits{},stumbleWaitOk{},stumbleBusyTimeout{},bumpAsks{},jobMerged{},jobNoSlot{},escGraded{},escMerged{},peakRegrades{},peakUpgrades{},peakNoRec{};
    std::array<Counter,64> fallBy{};
    // stumbles on a frozen mover sent as the UP reaction message (lever pushfrozenmsg), a second one on the same push (nothing sent), a
    // stumble offset dropped on a frozen mover; escalated presses that found the push of their own touch, escalations on a knockdown (nothing
    // added); knockdowns sent with the side reaction (lever pushside) / the behind impulse in place of the fallback (lever pushbehindimp)
    Counter frozenMsg{},frozenRepeat{},frozenDrop{},escKeyed{},escOnKnock{},sideReaction{},behindImp{};
    // KNOCKDOWN ROUTE (lever pushknock): knockdowns by hp::KnockRow; the frozen-mover UP message held for the other hand (lever
    // pushfrozenhold): held, dropped for a knockdown of the same push (superseded), sent at the deadline, its push record gone at the
    // deadline (lost), the mover free at the deadline (the stumble's offset instead), both of the hand's hold slots taken (the message went
    // out at once)
    std::array<Counter,4> knockRows{};Counter frozenHeld{},frozenSuperseded{},frozenDeferredSent{},frozenHoldLost{},frozenHoldFreed{},frozenHoldFull{};
    // knockdowns by row x push class (behind, front, sideA+, sideA-) x outcome (along, against, side, none): a row's proof per side
    std::array<Counter,64> fallBy3{};
    // knockdowns by row x the first node after the knockdown message (staggerfall_back, staggerfall_front, #bc3dfb68, #731220e8, under
    // #ad400c7a, other) x outcome (along, against, side, none); the predicted route (free, frozen) x the first NM task after it
    // (staggerFall 0x2d43850, NMShot 0x2d42968, none of them)
    std::array<Counter,112> fallBy2{};std::array<Counter,6> routeMatrix{};Counter injuryTreeReads{},injuryTreeRefused{};
    // KNOCK PICK: knockdowns by KnockPickWhy; the forced ones by the landed node (as chosen, the other behaviour, another node, none in the
    // watch); roll re-writes; the fall by the behaviour that landed (staggerFall, NMShot, other) x push class x outcome against the
    // message's direction (along, against, side, none); the roll's churn sampled every tick while PICK is armed (ticks, changes of the
    // roll, Used 0 -> 1 flips, the most changes in one second, the random function seen set)
    std::array<Counter,8> pickBy{};std::array<Counter,4> pickLanded{};Counter pickRewrites{};std::array<Counter,48> fallByPick{};
    Counter churnTicks{},churnNum{},churnUsed{},churnMaxPerSec{},churnFuncSet{};
    // KNOCK TOUGH (lever pushknocktough): knockdowns of tough actors (health +0x150 bit 0x8000), the bit cleared for the message (window) or
    // kept (lever 0, TOUGH prints refused, no slot / unreadable), put back at the landed node or 120 ms / on a later tick past 250 ms (late),
    // found set again by a script in between (left), the actor gone; the first node after the message by the tough state (none, kept,
    // window) x (under #8b669f52 = the tough stagger, elsewhere)
    Counter toughSeen{},toughCleared{},toughKeptLever{},toughKeptPrints{},toughKeptOther{},toughRestored{},toughRestoreLate{},toughScriptChanged{},toughLost{};
    std::array<Counter,6> toughLanded{};
    // PICK FUNC (lever pushknockpickfunc): the draw function set for a held pick, refused (sm_RandFunc not null, PICKFN prints), busy (another
    // pick holds it), put back to null (ours), found replaced at the end (left); forced knockdowns of tough actors by landed (as chosen,
    // other behaviour, other node, none)
    Counter pickFnInstalled{},pickFnRefused{},pickFnBusy{},pickFnUninstalled{},pickFnReplaced{};std::array<Counter,4> pickLandedTough{};
    // KNOCK OFF (lever knockoffms): windows armed (pairs dropped / measured only), ended once up (the ragdoll word 0 after 300 ms), by time,
    // no free slot
    Counter knockOffArmed{},knockOffMeasure{},knockOffEndedUp{},knockOffExpired{},knockOffFull{};
    // LATE FALL: knockdowns followed to 1500 ms (pelvis at 900 / 1200 / 1500 ms, head height drop at 600 / 1500 ms), fell (the head 60 cm
    // or more lower at 1500 ms) or not, by the tough state (none, kept, window) x (fell, not); lost (the creature or its ring record gone)
    Counter lateFollowed{},lateFell{},lateNotFell{},lateLost{},lateNoBones{};std::array<Counter,6> lateFellByTough{};
    // KNOCK SERVO: knockdowns steered, kicks sent / lost, the servo's ends (hp::ServoWhy); kick 1 queued on the message tick (pushKickEarly)
    // and its failures; the fall at 1500 ms by servo lever (0 / 1) x push class x (along the push, toward the pusher, sideways, not moved)
    Counter servoArmed{},servoKicks{},servoLost{},earlyKicks{},earlyFailed{},trackReads{};
    // an early kick 1 not sent (left to the first ragdoll tick), an early kick 1 counted out unapplied (its slot freed while the object
    // stayed inactive: queued again at the first ragdoll tick, the servo's budget given back)
    Counter earlyRetried{},earlyFreed{};std::array<Counter,8> servoEnds{};std::array<Counter,32> lateByServo{};
    // the subtree check (-1 not read, 0 refused, 1 ok) and sm_RandFunc as last read (once a second: -1 unread, 0 null, 1 set)
    std::atomic<int> pickTree{-1},randFuncState{-1};
    // a held prop swung into a creature (PropHitRequest): consumed, struck (apply), refused by reason (handphys PropHitWhy), the haptic
    // of the second holding hand, requests skipped (overwritten in the ring before this tick read them)
    Counter propConsumed{},propStruck{},propSecondPulse{},propSkipped{};std::array<Counter,16> propRefused{};
    // THROW: the hits of a thrown prop in flight among them (consumed, struck), the unarmed scans that took the prop's point
    Counter propThrownConsumed{},propThrownStruck{},propScanPoint{},propScanPointArmed{};
    // the meleehit subtree (node ids two levels under it, read once from a watched creature's tree)
    Counter meleeTreeReads{};
};
inline Stats stats{};
static_assert(std::tuple_size<decltype(Stats::propRefused)>::value>=::rdrvr::vr::handphys::PropHitWhyCount,"a refusal counter per PropHitWhy");
inline void bump(Counter& c,std::uint64_t n=1) noexcept {c.fetch_add(n,std::memory_order_relaxed);}
inline void maxOf(std::atomic<float>& a,float v) noexcept {float m=a.load(std::memory_order_relaxed);while(v>m&&!a.compare_exchange_weak(m,v)){}}
inline void maxOf(std::atomic<double>& a,double v) noexcept {double m=a.load(std::memory_order_relaxed);while(v>m&&!a.compare_exchange_weak(m,v)){}}
struct LastStrike {
    std::uint64_t ns{};int hand{-1};Mode mode{};std::uint32_t victimGuid{};int race{-1},zone{-1},component{-1},reaction{0},face{0};
    float speed{},damage{},hpBefore{},hpAfter{-1},msgHpAfter{-1},koBefore{},koAfter{};std::uint32_t flags{};Vec3 hitPos{};
    bool probeHit{},instMatch{},fx{},dry{},kill{},ko{},phantom{};const char* source{"NONE"};char bone[17]{};
    float grip{-1.f},gripMin{-1.f},closedMs{-1.f},closeMps{-1.f}; // a fist: the grip at the strike, its lowest in 300 ms, closed for, closing speed
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
    ::rdrvr::vr::handphys::playerFoot.store(reinterpret_cast<std::uintptr_t>(foot),std::memory_order_relaxed); // the door hook's on-foot test
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
    bool handOnly{}; // a horse kept only for the hand-physics body (no strikes)
};
// prop = a held prop's hit (consumePropHit: the face from the swing, the fist's KO damage times koScale, the impulse when strong).
struct PendingStrike {int hand{-1};Mode mode{};int victim{-1},part{-1};Vec3 point{},direction{};float speed{};float grip{-1.f},gripMin{-1.f},closedMs{-1.f},closeMps{-1.f};
    bool prop{},strong{};float koScale{1.f};bool thrown{};};
// HAND PHYSICS open-hand touches: the push refusals (TOUCH), the last touch's health before / after (never a damage).
enum PushRefuse : unsigned {PushNotHuman=0,PushMoverVt,PushRagdoll,PushFlags,PushBusy,PushStale,PushRefuseCount};
inline constexpr std::array<const char*,PushRefuseCount> pushRefuseNames{"notHuman","moverVt","ragdoll","flags","busy","stale"};
// gen0 = actor slot 0's generation, fakeResolved = the fake guid resolved to an actor (then never sent); landed = the animator holds the
// message (NotifyInjured ran), animDamage / animInjury = animator +0xaf0 (CalculateDamage, the damage the injury tree compares) / +0xaec
// (msg +0xc8) after it; koDamage = the KO the message carried.
struct LastTouch {std::uint64_t ns{};int kind{},hand{-1},reaction{};std::uint32_t victimGuid{},fake{};float hpBefore{-1},hpAfter{-1},koBefore{-1},koAfter{-1},a{},distCm{};bool msg{};
    int gen0{-1};bool fakeResolved{},landed{};float animDamage{-1.f},koDamage{};std::uint32_t animInjury{};
    int attacker{-1},path{-1},comp{-1},zone{-1};bool escalated{},windowed{};char bone[17]{};int node{-1};};
// The node watch (diagnostics, Engine::readNode): a creature's action node before the action and every change within watchNs after;
// reacted = the node or the controller's play counter changed. kind: slap, push, fist, a press without a message (the control group), a
// walk-in bump of the player's mover (the stock non-hostile reaction); variant: the open hand's message (fake, self, none), a node played,
// the player (the fist).
enum WatchKind : unsigned {WatchSlap=0,WatchPush,WatchFist,WatchPress,WatchWalk,WatchProp,WatchKindCount};
enum WatchVariant : unsigned {WatchVariantFake=0,WatchVariantSelf,WatchVariantNone,WatchVariantNode,WatchVariantPlayer,WatchVariantCount};
inline constexpr std::array<const char*,WatchKindCount> watchKindNames{"slap","push","fist","press","walk","prop"};
static_assert(WatchKindCount==6&&WatchVariantCount==5,"Stats::watchReacted is [6][5]");
inline constexpr std::array<const char*,WatchVariantCount> watchVariantNames{"fake","self","none","node","player"};
inline constexpr std::uint64_t watchNs=600'000'000ull;
// ms after the start, ok = read, id = the node's name hash (0 unknown), parents' hashes, counter / timeIn = the controller's play counter
// and time in the node, react = the injury react level, the mover's ragdoll / busy words, the first 2 running tasks (vtable - base), name.
// path = the node's names up its parents to the top of its tree: "/a/b/c" when that top is the global root (the text the slapnode /
// pushnode levers take: Find resolves it), "!top/a/b/c" for another tree's top ("?" when the root was not read), a leading "..." when
// deeper than 10 levels; rooted = 1 / 0 / -1 unknown; f112 = actor +0x112 (bit 6 full update), f915 = creature component +0x915.
struct NodeSample {std::uint16_t ms{};bool ok{};std::uint32_t id{};std::array<std::uint32_t,3> parents{};std::uint32_t counter{};float timeIn{-1.f};int react{-1};
    std::uint32_t ragdoll{};std::int16_t busyA{},busyB{};std::array<std::uint32_t,2> tasks{};char name[24]{};
    std::uintptr_t sfTask{}; // NMStaggerFallTask among the first 4 running tasks (0 none)
    std::uint16_t f112{0xffff};std::uint8_t f915{0xff};std::int8_t rooted{-1};char path[128]{};};
// A push or a prop hit under watch (tier 0xff = none): its tier (hp::PushTier; a prop hit 3), the reaction written (-1 none), the face
// sent (-1 no message), the impulse sent (|msg +0xa0|) and the punch value k read, dot(push, the creature's row c), dot(the player's
// heading, push), the push direction, the hit point and component, the mover's start position; read back: aniNMStateData +0x30 one tick
// later (the bulletVel NM takes), the mover's ragdoll word 150 ms after, the creature's horizontal displacement over the watch along the
// push (fallDot, -2 unread) and its length (cm), hp before / after; kick = a projMsgApplyImpulse along the push once ragdoll (pushkick).
// The kick of a watched push (KickState): off (lever, not a firm shove, a slap, no self message), armed (waits for the ragdoll), sent (a
// pending-impulse slot free before the call is in use after it), noRagdoll (not ragdoll within 200 ms), noComp (no torso bone in the
// layout, no frag child for it at the kick, or its position unreadable), print (KICK prints or the broadcast unverified), build (the
// message was not finite or the call threw), hpLock (this creature lost hp after an earlier kick), noMag (size 0), human (not a human),
// alreadyRagdoll (the mover's ragdoll word set or unread at the push), pair (another watch of the creature armed or sent a kick within
// 300 ms), slotsFull (all 8 pending-impulse slots in use: no call), lost (called, no slot taken), instChanged (the actor's entity or frag
// instance differs from the push's, or its slot words unreadable: no call).
enum KickState : std::uint8_t {KickOff=0,KickArmed,KickSent,KickNoRagdoll,KickNoComp,KickPrint,KickBuild,KickHpLock,KickNoMag,KickHuman,KickAlready,KickPair,
    KickFull,KickLost,KickInst,KickStateCount};
inline constexpr std::array<const char*,KickStateCount> kickStateNames{"off","armed","sent","noRagdoll","noComp","print","build","hpLock","noMag","human",
    "alreadyRagdoll","pair","slotsFull","lost","instChanged"};
static_assert(KickStateCount==15,"stats.kickBy holds one counter per KickState");
// hp::KickGate -> KickState
inline constexpr std::array<KickState,9> kickFromGate{KickArmed,KickOff,KickHuman,KickPrint,KickAlready,KickPair,KickNoComp,KickNoMag,KickHpLock};
static_assert(kickFromGate[::rdrvr::vr::handphys::KickGateArmed]==KickArmed&&kickFromGate[::rdrvr::vr::handphys::KickGateAlready]==KickAlready&&
              kickFromGate[::rdrvr::vr::handphys::KickGateHpLock]==KickHpLock&&kickFromGate[::rdrvr::vr::handphys::KickGatePair]==KickPair);
struct PushInfo {std::uint8_t tier{0xff};std::int8_t reaction{-1},face{-1},impulseSrc{-1};float a{},depthCm{},arm{},impulse{},impulseK{-1.f},dirFwd{-2.f},headDir{-2.f};
    Vec3 dir{},start{},hit{};int comp{-1};bool haveStart{},bulletRead{};Vec3 bullet{};std::int32_t ragdoll150{-1};float fallDot{-2.f},movedCm{-1.f};
    bool kick{},kicked{};float kickMag{},hpBefore{-1.f},hpAfter{-1.f};
    // the push ring: a slap, strong; the creature's rows a / c at t0 (dirSide = dir . row a); the palm (horizontal direction, m/s); the kick
    // (KickState, its direction and source: 0 palm, 1 into; ms after t0); the message as sent (local hit after SetFromTestProbe, world
    // normal, bulletVel msg +0xa0, zone, bone); what the animator held one tick later (+0x8b8 face, aniMelee +0x550 reaction, +0xaec
    // injury type, the react level); the first tick the mover was ragdoll (ms); the pelvis and head bones (skeleton globals) at t0, 300 ms
    // and 600 ms -> fall300 / fall600 = the pelvis's horizontal move . dir (cm: its length), headFall600 = (head - pelvis) . dir and
    // headFwd600 = (head - pelvis) . row c at t0 (horizontal, normalised; +1 = on its face)
    bool slap{},strong{};Vec3 rowA{},rowC{},palmDir{},kickDir{};float palmMps{-1.f},dirSide{-2.f};std::uint8_t kickState{KickOff},kickSrc{2};std::int32_t kickMs{-1};
    Vec3 hitLocal{},normal{},bulletSent{};int zone{-1};char bone[17]{};
    bool landedRead{};std::int32_t faceLanded{-99},reactionLanded{-99},reactLevel{-99};std::uint32_t injuryLanded{};std::int32_t ragdollMs{-1};
    std::uintptr_t globals{};std::int32_t pelvisBone{-1},headBone{-1};std::uint32_t boneCount{};std::array<Vec3,3> pelvis{},head{};std::array<bool,3> haveBones{};
    float fall300{-2.f},fall600{-2.f},fallCm300{-1.f},fallCm600{-1.f},headFall600{-2.f},headFwd600{-2.f};
    // the kick's target: the mover's ragdoll word at the push (-1 unread), the frag instance at the push ([[actor +0x50] +0x88]), the torso
    // bones tried in order (spine0, spine3, pelvis); at the kick: the component and bone it went to and the bone's world position, the
    // free pending-impulse slots before the call (-1 unread); globalsMoved = the skeleton globals changed between t0 and a fall sample
    std::int8_t ragAtPush{-1};std::uintptr_t inst{};std::array<std::int32_t,3> torsoBones{-1,-1,-1};std::int32_t kickComp{-1},kickBone{-1};Vec3 kickAt{};
    std::int8_t kickFree{-1};bool globalsMoved{};
    // graded push (lever pushgrade): the decision (hp::PushDecision, 0xff = an ungraded push), aEff, a pair (and the gap between the hands'
    // edges, ms), world units per metre; the creature's horizontal velocity at the push (m/s: the vector, along the push, along its row c);
    // the kick's mass (kg) and target speed (m/s), kick 1 (N s, world); the stumble asked / applied (cm) and the job's refusals (busy,
    // ragdoll, flags, other); kick 2 (KickState, ms after t0, N s) and its lever; the pelvis at kick 1 and its velocity 100 ms later along /
    // across the push (m/s), effMass = (J1 . dir) / the velocity change along dir (kg); the pelvis's move in the creature's frame at t0
    // (cm along row c / row a) at 300 and 600 ms
    std::uint8_t decision{0xff};bool graded{},pair{},kick2Lever{},kick2Done{},haveKickPelvis{};float aEff{-1.f},pairGapMs{-1.f},upm{1.f};
    Vec3 v0{};float v0Dir{},v0Fwd{},massKg{-1.f},vDes{};Vec3 kickJ{};float askedCm{-1.f},appliedCm{-1.f};std::array<std::uint16_t,4> jobRefused{};
    std::uint8_t kick2State{KickOff};std::int32_t kick2Ms{-1};Vec3 kick2J{};Vec3 kickPelvis{};std::uint64_t kickPelvisAge{};
    // the kick window's start (ns after t0: the message that armed the kick; a pair upgraded by the second hand arms it then)
    std::uint64_t kickBase{};
    float vmAlong{-9.f},vmSide{-9.f},effMass{-1.f},fwdCm300{-999.f},sideCm300{-999.f},fwdCm600{-999.f},sideCm600{-999.f};
    // PUSH FIX: the class in the victim's frame (hp::PushClass), the front lever as sent, the mover's suspend / frozen counters at the push
    // (+0x846 / +0x848, -1 unread), the stumble refused at the touch (busy, ragdoll, flags, other), its wait (ms, -1 none) and the bump asked,
    // an escalated press, jobs merged, the edge's a and the stroke peak's (-1 none)
    std::uint8_t pushClass{0xff},frontLever{0xff};std::int16_t busyA{-1},busyB{-1};std::array<std::uint16_t,4> refusedAtTouch{};std::int32_t waitMs{-1};
    bool bumpAsked{},escalated{};std::uint16_t jobMerged{};float edgeA{-1.f},peakA{-1.f};
    // the stumble sent as the UP reaction message on a frozen mover; each hand's graded approach in the push (-1 none)
    bool frozenMsg{};std::array<float,2> aHand{-1.f,-1.f};
    // KNOCK (the knockdown message): the route table's row (hp::KnockRow, 0xff none), the class / direction / mover counters at the
    // message (+0x846 / +0x848), the face, impulse size and bullet sign sent, its time (ns after t0, -1 none), the other injure messages to
    // the creature within 200 ms of it; one tick after it the animator's face (+0x8b8), aniMelee's reaction and NM's bullet; the pelvis's
    // velocity over 0-50, 50-100 and 100-200 ms after kick 1 (m/s along row c / row a at t0); the UP message held for the other hand at
    // this push, dropped for the knockdown
    std::uint8_t knockRow{0xff},knockClass{0xff};std::int8_t knockFace{-1},knockBullet{};std::int16_t knockB6{-1},knockB8{-1};
    float knockDirFwd{-2.f},knockDirSide{-2.f},knockImp{-1.f};std::int64_t knockAge{-1};std::uint64_t knockAt{};std::uint8_t msgNear{};
    bool knockRead{};std::int32_t kFace{-99},kReaction{-99};Vec3 kBullet{};
    std::array<float,3> kvFwd{-99.f,-99.f,-99.f},kvSide{-99.f,-99.f,-99.f};std::array<bool,3> kvDone{};Vec3 kvPrev{};std::uint64_t kvPrevAge{};
    // the route the message took (hp::knockFrozen: +0x848 > 0 at it, or the push sent / held the frozen mover's UP message; -1 none)
    bool frozenHeld{},frozenSuperseded{};std::int8_t knockFz{-1};
    // KNOCK PICK (lever pushknockpick): the behaviour chosen (1 staggerFall, 0 NMShot, -1 the coin), why (KnockPickWhy), the shared roll and
    // its used flag before the message, after the call and at the landed node (-1 unread), the re-writes until the landed node or 120 ms
    // (ticks re-checked), done; the message's direction (the push the knockdown sent) and the fall against it and against the kick at 600 ms;
    // the pelvis velocity of the 2 ticks before the mover went ragdoll (m/s along row c / row a at t0)
    std::int8_t pickParity{-1};std::uint8_t pickWhy{0};bool pickDone{};std::uint16_t pickWrites{},pickTicks{};
    std::int32_t rollBefore{-1},rollAfter{-1},rollLanded{-1};std::int8_t usedBefore{-1},usedAfter{-1},usedLanded{-1};
    Vec3 knockDir{};float fall600Msg{-2.f},fall600Kick{-2.f};
    std::array<float,2> preFwd{-99.f,-99.f},preSide{-99.f,-99.f};Vec3 prePelvis{};std::uint64_t prePelvisAge{};bool havePrePelvis{},preDone{};
    // KNOCK TOUGH: 0 not tough, 1 tough kept, 2 the window (the bit cleared for the message); its slot and whether it was put back
    std::int8_t tough{},toughSlot{-1};bool toughRestored{};
    std::int8_t knockN{-1}; // the watch's node sample count at the knockdown's message (the samples from it on came after the message)
    // KNOCK SERVO (lever pushKickServo): on, its kicks (N s, world axes in metres; ms after t0; KickState), the N s spent (kick 1 included),
    // why it ended (hp::ServoWhy, 0 running); the slot of the last kick (-1 unknown) and the watch age its word read consumed (-1 not yet);
    // the age of kick 1; kick 1 queued on the message tick (pushKickEarly: 1 sent, 0 refused or lost, -1 not tried, 2 sent but counted
    // out unapplied and queued again at the first ragdoll tick)
    bool servoLever{},earlyLever{};std::uint8_t servoN{},servoWhy{0xff};float servoSpent{};std::array<Vec3,4> servoJ{};std::array<std::int16_t,4> servoMs{-1,-1,-1,-1};
    std::array<std::uint8_t,4> servoState{};std::int8_t kickSlot{-1};std::int64_t consumedAge{-1},kick1Consumed{-1};std::uint64_t kick1Age{};std::int8_t early{-1};
    bool servoGoal{}; // the last step found the body moving along the push (no kick)
    // the last kick's slot: a negative word read since it was queued, queued while the object was active (ragdoll seen; hp::kickSlotOutcome)
    bool slotSawNeg{},kickQueuedActive{true};
    // the centre of mass (hp::comProxy: pelvis, spine3, head, knees) per tick: the bones, the last sample and its age, the velocity of the
    // last whole tick (m/s, world) and the age its interval started
    std::array<std::int32_t,::rdrvr::vr::handphys::comBones> comBone{-1,-1,-1,-1,-1};Vec3 comPrev{};std::uint64_t comPrevAge{};bool haveComPrev{};
    Vec3 comVel{};std::uint64_t comVelFrom{};bool haveComVel{};
    // KNOCK TRACE: the last 2 ticks before the message, then one row a tick to 300 ms after the message (or kick 1), 24 rows
    // (hp::traceRowPlan): ms after t0, the mover's ragdoll word (-1 unread), the pelvis and the centre of mass (cm along row c / row a at t0
    // from the start), the last kick's slot word (INT32_MIN none), a kick sent this tick (N s along row c / row a)
    struct TraceRow {std::int16_t ms{};std::int8_t rag{-1};std::int16_t pelF{-9999},pelS{-9999},comF{-9999},comS{-9999};std::int32_t slot{INT32_MIN};
        std::int16_t jF{},jS{};};
    std::array<TraceRow,24> trace{};std::uint8_t traceN{};
    // the landed staggerFall's track (task +0x28: +0x14..+0x87 as 29 words; read once): 1 read, 0 refused, -1 not seen
    std::int8_t trackOk{-1};std::array<std::uint32_t,29> track{};
    int knockFrozen() const noexcept {return knockRow==0xff?-1:knockFz;}};
struct NodeWatch {bool set{};std::uint8_t kind{},variant{};std::uint32_t guid{};std::uintptr_t actor{};std::uint64_t t0{};NodeSample before{};
    std::array<NodeSample,4> tr{};std::uint8_t n{};std::uintptr_t lastNode{};std::uint32_t lastCounter{};bool reacted{};PushInfo push{};};
// The meleehit node's subtree (read once): depth (1 child, 2 grandchild), name hash, child count.
struct TreeNode {std::uint8_t depth{};std::uint32_t id{};std::uint16_t children{};};
inline std::mutex treeMutex;inline std::array<TreeNode,32> meleeTree{};inline unsigned meleeTreeCount{};inline std::uint32_t meleeTreeRoot{};
// The injury subtree's selection (TREE prints, read once per parent node on a knockdown's landed path: meleehit and the next two nodes
// down it): per child its name hash, the type byte +0x2f (3 bank, 4 playable, else a reference: its target's hash), its child count, its
// condition group (node +0x40 array, +0x48 count) with per condition the Match function (vtable +0x68, a librdr offset) and the words
// +0x8 / +0xc / +0x10 / +0x14 (ConditionInjuryFace: +0x8 the face; ConditionInjuryImpulseMag: +0x8 the operator, +0xc the size).
struct TreeCond {std::uint32_t fn{};std::array<std::uint32_t,4> w{};};
struct TreeChild {std::uint32_t id{},refId{};std::uint8_t type{0xff};std::int16_t children{-1};std::uint16_t conds{};std::array<TreeCond,6> c{};};
struct TreeDump {std::uintptr_t node{};std::uint32_t id{};std::uint16_t count{};std::uint8_t n{};std::array<TreeChild,8> ch{};};
inline std::array<TreeDump,4> injuryTree{};inline unsigned injuryTreeN{};inline std::uintptr_t meleeTreeNode{};
// A node play's result (Engine::playNode).
// read = a guard's read failed; f915 / f118 / race / mover = that guard refused (actor +0x112 bit 6, the full-update state the engine
// sets for a creature near the player, is recorded, never a refusal).
enum NodeResult : unsigned {NodePlayed=0,NodeNoPath,NodeOff,NodeComp,NodeState,NodeLocked,NodeScript,NodeRagdoll,NodeBusy,NodeRecent,NodeNotFound,NodeOtherTree,
    NodeNoChange,NodeException,NodeF915,NodeF118,NodeRace,NodeMover,NodeCount};
static_assert(NodeCount==nodeResultCount);
inline constexpr std::array<const char*,NodeCount> nodeResultNames{"played","noPath","off","comp","read","locked","script","ragdoll","busy","recent","notFound",
    "otherTree","noChange","exception","f915","f118","race","mover"};
inline std::mutex watchMutex;inline std::array<NodeWatch,8> watchRing{};inline unsigned watchNext{};
// The push ring: finished push and slap watches only (fists, presses and walk bumps never overwrite it).
inline std::mutex pushRingMutex;inline std::array<NodeWatch,8> pushRing{};inline unsigned pushRingNext{};
// KNOCK PICK: why a knockdown's behaviour was or was not forced
enum KnockPickWhy : std::uint8_t {PickOff=0,PickForced,PickNoApi,PickNoTree,PickTreeBad,PickRandFunc,PickReadFail,PickNoWatch,PickWhyCount};
inline constexpr std::array<const char*,PickWhyCount> pickWhyNames{"off","forced","noApi","noTree","treeBad","randFunc","readFail","noWatch"};
// The knockdown ring (24, knockdowns only): the push, the pick, the rolls, the landed node, the message and kick in the victim's frame (row c
// / row a at t0), the pelvis velocity before ragdoll and over 0-50 / 50-100 / 100-200 ms after kick 1, its move at 300 / 600 ms and the fall
// at 600 ms against the first hand's direction, the message's push and the kick.
struct KnockRec {std::uint64_t ns{};std::uint32_t guid{};std::uint8_t cls{0xff},row{0xff},why{};std::int8_t parity{-1},landedParity{-2};float dirFwd{-2.f},dirSide{-2.f};
    std::int32_t rollBefore{-1},rollAfter{-1},rollLanded{-1};std::int8_t usedBefore{-1},usedAfter{-1},usedLanded{-1};std::uint16_t writes{},ticks{};
    std::uint32_t landedId{},landedTask{};std::int32_t landedMs{-1};float sFwd{},sSide{},jFwd{},jSide{};int comp{-1};char bone[17]{};
    std::array<float,2> preFwd{-99.f,-99.f},preSide{-99.f,-99.f};std::array<float,3> kvFwd{-99.f,-99.f,-99.f},kvSide{-99.f,-99.f,-99.f};
    float fwdCm300{-999.f},sideCm300{-999.f},fwdCm600{-999.f},sideCm600{-999.f},fall600{-2.f},fall600Msg{-2.f},fall600Kick{-2.f},cm600{-1.f};std::int8_t face{-1};float imp{-1.f};
    // the tough state (0 none, 1 kept, 2 window); LATE FALL (written at 1500 ms): the pelvis in the creature's frame at t0 (cm along row c /
    // row a) at 900 / 1200 / 1500 ms, the head height drop (cm) at 600 / 1500 ms, fell (-1 unknown); KNOCK OFF: the hand pairs with its
    // frag within 0-600 ms / later, the hands' highest speed toward the pusher while paired (m/s), dropped (1) or only measured (0), -1 none
    std::int8_t tough{};std::array<float,3> fwdCmLate{-999.f,-999.f,-999.f},sideCmLate{-999.f,-999.f,-999.f};float headDrop600{-999.f},headDrop1500{-999.f};std::int8_t fell{-1};
    std::uint32_t offEarly{},offLate{};float offTowardMps{-1.f};std::int8_t offDrop{-1};
    std::int8_t servo{-1}; // KNOCK SERVO: the lever at the knockdown (-1 not graded)
};
inline std::mutex knockRingMutex;inline std::array<KnockRec,24> knockRing{};inline unsigned knockRingNext{};
// KNOCK TRACE (8, knockdowns only; the knock ring keeps its size): the push class, the levers, kick 1 (ms, J along row c / row a in N s, the
// age its slot read consumed), the message and ragdoll ages, the servo's kicks and end, the per-tick trace, the staggerFall track, the
// fall at 600 ms against the message's push.
struct KnockTrace {std::uint64_t ns{};std::uint32_t guid{};std::uint8_t cls{0xff};bool servo{},early{};std::int8_t earlySent{-1};std::int32_t kickMs{-1},ragMs{-1},knockMs{-1},
    kick1LandMs{-1};float j1F{},j1S{};std::uint8_t servoN{},servoWhy{0xff};float servoSpent{};std::array<float,4> sjF{},sjS{};std::array<std::int16_t,4> sMs{-1,-1,-1,-1};
    std::array<std::uint8_t,4> sState{};decltype(PushInfo::trace) rows{};std::uint8_t n{};std::int8_t trackOk{-1};std::array<std::uint32_t,29> track{};float fall600Msg{-2.f};};
inline std::mutex knockTraceMutex;inline std::array<KnockTrace,8> knockTrace{};inline unsigned knockTraceNext{};
inline std::mutex touchMutex;inline LastTouch lastTouch{};
inline constexpr unsigned maxSlots=1024,maxActors=256,maxInRange=16,maxCandidates=8,maxBones=128,maxLayouts=32;

class Engine {
public:
    void configure(const EngineApi& api) noexcept {api_=api;hands_={};candCount_=0;layoutCount_=0;}
    const EngineApi& api() const noexcept {return api_;}
    void tick(const mt::Frame& frame) noexcept;
private:
    struct Hand {Mode mode{};M::BladeHold blade{};M::FistHold fist{};std::uint64_t wallTick{~0ull};bool wallClear{true};M::FistGate gate{};bool gateOpen{};};
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
    void scan(const Player& p,const std::array<Vec3,2>& handsWorld,unsigned armed,const Vec3* extra=nullptr) noexcept;
    bool deriveChain(std::uintptr_t actor,Chain& c) noexcept;
    int layoutFor(const Chain& c) noexcept;
    void buildBodies(const std::array<Vec3,2>& handsWorld) noexcept;
    bool validComponent(const Chain& c,int comp) const noexcept;
    int componentForBone(const Chain& c,int bone) const noexcept;
    bool wallClear(Hand& h,unsigned hand,const Player& p,const Candidate* victim,Vec3 from,Vec3 to) noexcept;
    void resetHand(unsigned h,HandReset why) noexcept {hands_[h].blade=M::BladeHold{};hands_[h].fist=M::FistHold{};bump(stats.hands[h].resets[why]);}
    void apply(const PendingStrike& s,const Player& p,std::uint64_t now) noexcept;
    void logInputs(const Player& p) noexcept;
    // HAND PHYSICS: bodies of the creatures near a live palm, published for the hands; the touches it hands over (the serial seen per
    // hand), the pushes and yaws queued per creature, the press's push so far per creature
    struct Prev {std::uint32_t guid{};Vec3 pos{};std::uint64_t ns{};};std::array<Prev,4> prev_{};std::array<std::uint64_t,3> touchSerial_{}; // [2] = the thrown slot
    float nearestPalmM_{-1.f}; // the nearest candidate to a live palm this tick (published with the bodies)
    // recNs = the graded push's record (its clock; 0 none), waitUntilNs = a stumble waiting on a frozen / suspended mover until then
    // (0 = no wait), waitStartNs = its first refused tick
    struct PushJob {std::uint32_t guid{};std::uintptr_t actor{};Vec3 dir{};float perTick{};unsigned ticks{};float yawPerTick{};unsigned yawTicks{};
        Vec3 start{};bool haveStart{},measure{};float askedM{};std::uint64_t recNs{},waitUntilNs{},waitStartNs{};};
    std::array<PushJob,4> pushes_{};
    // the movers given an offset this tick: checked for its consumption at the next tick
    std::array<std::uintptr_t,4> pushedMovers_{};unsigned pushedCount_{};
    struct PressDone {std::uint32_t guid{};float doneM{};std::uint64_t ns{};};std::array<PressDone,4> pressDone_{};
    void nearBodies(std::uint64_t now) noexcept;
    void publishBodies(const std::array<unsigned,3>& who,unsigned n,std::uint64_t now) noexcept;
    bool pushTarget(std::uintptr_t actor,int race,std::uintptr_t& mover,std::uintptr_t& anim,int* whyOut=nullptr,bool count=true) noexcept;
    void refreshFallChain(NodeWatch& w) noexcept;KickState kickNow(NodeWatch& w,Vec3 impulse) noexcept;bool readPelvisNow(NodeWatch& w,Vec3& out) noexcept;
    bool readComNow(NodeWatch& w,Vec3& out,Vec3* pelvis=nullptr) noexcept;bool readKickSlot(NodeWatch& w,std::int32_t& out) noexcept;void knockTraceTick(NodeWatch& w,std::uint64_t age,bool ragRead,std::uint32_t rag) noexcept;
    void servoTick(NodeWatch& w,std::uint64_t age,bool ragRead,std::uint32_t rag) noexcept;void readTrack(NodeWatch& w) noexcept;
    // graded pushes: one record per creature pushed within pushRecNs (the hands in it, the first hand's approach and direction, the merged
    // push, a firm edge, the decision, its watch's t0, world units per metre, the stumble asked / applied (world units), the job's refusals)
    // v0 = the creature's horizontal velocity at the first hand's edge (m/s), before any stumble offset of this push moves its body
    struct PushRec {std::uint32_t guid{};std::uint64_t ns{};std::uint8_t hands{};float a0{};::rdrvr::vr::handphys::V3 d0{},v0{};float aEff{};
        ::rdrvr::vr::handphys::V3 dir{};bool firm{};std::uint8_t decision{};std::uint64_t watchT0{};float upm{1.f},askedW{},appliedW{};std::array<std::uint16_t,4> refused{};
        std::array<std::uint16_t,4> refusedAtTouch{};bool bumpAsked{};std::int32_t waitMs{-1};std::uint16_t jobMerged{};float peakA{-1.f};
        // the hands' graded approaches and directions (hp::pushRegrade), each hand's touch clock (an escalated press of that touch finds this
        // push by it), the UP reaction message sent for a frozen mover
        ::rdrvr::vr::handphys::PushHands ph{};std::array<std::uint64_t,2> touchNs{};bool frozenMsg{};
        // the UP message of a frozen mover held for the other hand (lever pushfrozenhold); the push sent or held that message at any time
        bool frozenHeld{},frozenSeen{};};
    // pushEscRecNs: how long an escalated press still finds the push of its own touch
    std::array<PushRec,4> pushRecs_{};static constexpr std::uint64_t pushRecNs=300'000'000ull,pushEscRecNs=3'000'000'000ull;
    PushRec* pushRecFor(std::uint32_t guid,std::uint64_t t0) noexcept {for(auto& x:pushRecs_)if(x.guid&&x.guid==guid&&x.watchT0==t0)return &x;return nullptr;}
    void translate(std::uintptr_t mover,Vec3 world) noexcept;
    void checkPushed() noexcept;
    void queuePush(std::uint32_t guid,std::uintptr_t actor,Vec3 dir,float perTick,unsigned ticks,float yawPerTick,unsigned yawTicks,std::uint64_t recNs=0,
                   std::uint64_t waitUntilNs=0) noexcept;
    void consumeTouch(const Player& p,std::uint64_t now) noexcept;
    void runPushes(std::uint64_t now) noexcept;std::uint64_t bumpAskSerial_{};
    void dropTouch() noexcept;
    std::array<NodeWatch,4> watches_{};std::uint64_t controlNs_{},walkNs_{};
    // the node watch's paths by node (action nodes are tree objects: a node's path is read once, then its name hash confirms the entry)
    struct PathEntry {std::uintptr_t node{};std::uint32_t id{};std::array<std::uint32_t,3> parents{};std::int8_t rooted{-1};char path[128]{};};
    mutable std::array<PathEntry,16> paths_{};mutable unsigned pathNext_{};
    struct NodeCool {std::uint32_t guid{};std::uint64_t ns{};};std::array<NodeCool,4> nodeCool_{};
    bool nodeName(std::uintptr_t node,std::uint32_t& id,char* text,std::size_t n,std::uintptr_t* parent=nullptr) const noexcept;
    bool nodeChange(std::uintptr_t actor,std::uintptr_t& node,std::uint32_t& counter) const noexcept;
    bool readNode(std::uintptr_t actor,NodeSample& x,std::uintptr_t& node) const noexcept;
    bool nodeChildren(std::uintptr_t node,std::uintptr_t& list,std::uint16_t& count) const noexcept;
    void readMeleeTree(std::uintptr_t node) const noexcept;
    void readInjuryTree(std::uintptr_t node) noexcept;void dumpTreeLevel(std::uintptr_t parent) noexcept;
    // the frozen-mover UP message held for the other hand (lever pushfrozenhold): two slots per hand (slot i is hand i & 1), each the
    // request, its push record and the deadline
    struct FrozenHold {::rdrvr::vr::handphys::FrozenHoldSlot s{};::rdrvr::vr::handphys::TouchRequest r{};};
    std::array<FrozenHold,4> frozenHold_{};
    // the last injure messages of the open hands (guid, when): the other messages within 200 ms of a knockdown's
    struct MsgLog {std::uint32_t guid{};std::uint64_t ns{};};std::array<MsgLog,8> msgLog_{};unsigned msgLogNext_{};
    NodeWatch* startWatch(unsigned kind,unsigned variant,std::uintptr_t actor,std::uint32_t guid,std::uint64_t now) noexcept;
    void watchPush(NodeWatch& w,std::uint64_t age) noexcept;
    void finishPush(NodeWatch& w) noexcept;
    bool readFallBones(PushInfo& x,unsigned k) const noexcept;
    std::array<std::uint32_t,8> kickLocked_{};unsigned kickLockNext_{};
    bool kickLocked(std::uint32_t guid) const noexcept {for(const auto g:kickLocked_)if(g&&g==guid)return true;return false;}
    // KNOCK PICK: the injury subtree's random words checked once read (-1 not read yet, 0 refused, 1 ok; the dump count it was judged on),
    // the roll's churn (last values, this second's changes); the shared roll read / written (parity < 0: read only)
    int pickTree_{-1};unsigned pickTreeN_{};std::int32_t churnNum_{-1};std::int8_t churnUsed_{-1};std::uint64_t churnSecNs_{},churnSecCount_{};
    // KNOCK TOUGH: the cleared bits to put back (the actor, its health block, guid, when)
    struct ToughSlot {bool on{};std::uintptr_t actor{},health{};std::uint32_t guid{};std::uint64_t ns{};};
    std::array<ToughSlot,4> tough_{};
    bool toughClear(const Candidate& v,std::uint64_t now,int& slot) noexcept;
    void toughRestore(unsigned i,bool late) noexcept;
    // PICK FUNC: the pick holding the draw function (guid, since)
    std::uint32_t pickFnGuid_{};std::uint64_t pickFnNs_{};
    void pickFnStart(std::uintptr_t actor,std::uint32_t guid,int parity,std::uint64_t now) noexcept;
    void pickFnEnd() noexcept;
    // KNOCK OFF: the windows this side armed (the handphys slot of the same index)
    struct KnockOffOwn {bool on{};std::uintptr_t actor{};std::uint32_t guid{};std::uint64_t ns{},until{};};
    std::array<KnockOffOwn,4> koOwn_{};
    int knockOffArm(std::uintptr_t actor,std::uint32_t guid,std::uintptr_t inst,const Vec3& dir,float ms,std::uint64_t now) noexcept;
    // LATE FALL: a knockdown followed to 1500 ms after its watch's start
    struct Follow {bool on{};std::uintptr_t actor{};std::uint32_t guid{};std::uint64_t t0{};Vec3 pelvis0{},head0{},rowA{},rowC{},start{};bool haveStart{};float upm{1.f};
        std::int32_t pelvisBone{-1},headBone{-1};std::uint32_t boneCount{};std::uint8_t done{};std::array<float,3> fwd{-999.f,-999.f,-999.f},side{-999.f,-999.f,-999.f};
        float headDrop1500{-999.f};std::int8_t tough{};int offSlot{-1};std::uint64_t offArmed{};};
    std::array<Follow,4> follow_{};
    void followArm(const NodeWatch& w,int offSlot,std::uint64_t now) noexcept;
    // the knockdown upkeep that runs every tick whatever the gate (the tough bits, the hands-off windows, the draw function, the follow-ups)
    void knockUpkeep(std::uint64_t now) noexcept;
    KnockPickWhy pickReady() noexcept;
    bool pickRoll(int parity,std::int32_t& num,std::int8_t& used,bool& wrote) noexcept;
    void churnTick(std::uint64_t now) noexcept;
    void consumePropHit(const Player& p,std::uint64_t now) noexcept;std::uint64_t propSerial_{};
    void propHit(const Player& p,const ::rdrvr::vr::handphys::PropHitRequest& r,std::uint64_t now) noexcept;
    void tickWatches(std::uint64_t now) noexcept;
    void walkBumps(const Player& p,std::uint64_t now) noexcept;
    NodeResult playNode(const Candidate& v,const char* path,std::uint64_t now) noexcept;
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
    if(b.duel||b.duelRaw)return GateDuel;
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

// The distance from the live palms to a creature's vertical segment (feet .. feet + 2 m).
inline float palmDistance(const ::rdrvr::vr::handphys::Palms& pm,Vec3 feet) noexcept {
    float best=1e9f;
    for(unsigned h=0;h<2;++h){if(!(pm.live>>h&1u))continue;const Vec3 q{pm.p[h][0],pm.p[h][1],pm.p[h][2]};
        const float y=std::clamp(q.y-feet.y,0.f,2.f);best=std::min(best,(q-Vec3(feet.x,feet.y+y,feet.z)).length());}
    return best;
}
// The distance that keeps a creature's body for the hands: to the nearest live palm, or (a held heavy prop, Palms::heavy) to the prop's
// sphere surface; 1e9 when neither applies.
inline float nearDistance(const ::rdrvr::vr::handphys::Palms& pm,Vec3 feet) noexcept {
    float best=pm.live?palmDistance(pm,feet):1e9f;
    if(pm.heavy){const Vec3 c{pm.prop[0],pm.prop[1],pm.prop[2]};const float y=std::clamp(c.y-feet.y,0.f,2.f);
        const float d=(c-Vec3(feet.x,feet.y+y,feet.z)).length()-std::max(pm.propR,0.f);if(std::isfinite(d))best=std::min(best,std::max(d,0.f));}
    return best;
}
inline void Engine::scan(const Player& p,const std::array<Vec3,2>& hw,unsigned armed,const Vec3* extra) noexcept {
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
        if(extra)d=std::min(d,(pos+Vec3(0,1.f,0)-*extra).length()); // a held heavy prop / a thrown prop in flight (Palms::heavy)
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
        const bool handHorse=race==1&&!config.horses&&::rdrvr::vr::handphys::nearWanted.load(std::memory_order_relaxed)&&
            ::rdrvr::vr::handphys::nearHorses.load(std::memory_order_relaxed);
        if(race==1&&!config.horses&&!handHorse){bump(stats.skipped[SkipHorse]);continue;}
        alignas(8) unsigned char hb[off::healthBlock]{};
        if(!block(r.health,hb,sizeof hb)||at<std::uintptr_t>(hb,0)!=api_.healthVtable){bump(stats.skipped[SkipUnreadable]);continue;}
        Candidate c{};c.actor=r.actor;c.guid=r.guid;c.goh=r.goh;c.slot=r.slot;c.health=r.health;c.comp=r.comp;c.anim=r.anim;c.race=race;c.handOnly=handHorse;
        c.matrix=at<std::uintptr_t>(cb,off::compMatrix);c.position=vec(mb,0x30);c.rowA=vec(mb,0);c.rowC=vec(mb,0x20);c.distance=near[j].d;
        c.maxHp=at<float>(hb,off::healthMax);c.hp=at<float>(hb,off::healthValue);c.maxKo=at<float>(hb,off::healthKoMax);c.ko=at<float>(hb,off::healthKo);
        c.healthFlags=at<std::uint32_t>(hb,off::healthFlags);
        if(!(c.hp>0)||(c.healthFlags&off::healthDead)){bump(stats.skipped[SkipDead]);continue;}
        for(unsigned o=0;o<candCount_;++o)if(cand_[o].actor==c.actor&&cand_[o].guid==c.guid&&cand_[o].chain.ok&&tickIndex_-cand_[o].chainTick<30){
            c.chain=cand_[o].chain;c.chainTick=cand_[o].chainTick;c.layout=cand_[o].layout;}
        next[nc++]=c;
    }
    cand_=next;candCount_=nc;stats.candidates.store(nc,std::memory_order_relaxed);
    {namespace hp=::rdrvr::vr::handphys; // HAND PHYSICS NPC bump: the creatures near the player (main thread, read by the handphys tick)
     hp::Nearby nb{};nb.ns=nowNs();
     for(unsigned i=0;i<nc&&nb.count<nb.c.size();++i)nb.c[nb.count++]={next[i].actor,next[i].guid,{next[i].position.x,next[i].position.y,next[i].position.z},next[i].race};
     hp::nearby.store(nb);}
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
            if(c.race==1?M::upperCloudCapsule(cloud.data(),used,cc):M::cloudCapsule(cloud.data(),used,cc)){c.body.capsules[c.body.count++]=cc;bump(stats.cloudBodies);}
            else{c.body.capsules[c.body.count++]=M::actorCapsule(c.position);bump(stats.actorBodies);}
        }
        c.bodyValid=true;bump(stats.bodiesBuilt);
    }
    (void)hw;
    // HAND PHYSICS: the same bodies for the hands (no extra reads), the nearest to a live palm
    namespace hp=::rdrvr::vr::handphys;
    if(hp::nearWanted.load(std::memory_order_relaxed)){
        hp::Palms pm{};const auto now=nowNs();
        if(hp::palms.load(pm)&&(pm.live||pm.heavy)&&hp::fresh(now,pm.ns)){
            struct D {unsigned i{};float d{};};std::array<D,maxCandidates> near{};unsigned nn=0;
            for(unsigned i=0;i<candCount_;++i){if(!cand_[i].bodyValid)continue;
                const float d=nearDistance(pm,cand_[i].position);if(d<=1.6f)near[nn++]={i,d};}
            std::sort(near.begin(),near.begin()+nn,[](const D& a,const D& b){return a.d<b.d;});
            std::array<unsigned,3> who3{};const unsigned n3=std::min(nn,3u);for(unsigned k=0;k<n3;++k)who3[k]=near[k].i;
            publishBodies(who3,n3,now);}
    }
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
    if(v.handOnly){bump(stats.rejects[RejectHandOnly]);return;}
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
    if(fist)stats.punchImpulseK.store(impulseScale,std::memory_order_relaxed);
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
    f.flags=fist?M::msg::flagKo:M::msg::flagLethal;f.zone=cap.zone;f.component=comp;f.weapon=weapon;f.damage=0;
    f.face=s.prop?::rdrvr::vr::handphys::pushFace({v.rowC.x,v.rowC.y,v.rowC.z},{dir.x,dir.y,dir.z}):M::faceFromForwards(v.rowC,p.forward);
    alignas(16) unsigned char m[M::msg::size];
    if(!M::msg::build(m,f)){bump(stats.rejects[RejectMessage]);return;}
    if(comp>=0&&!config.dry&&api_.hitInfo){ // engine hit info: bone name, zone (heart!), local position/normal of that component
        alignas(16) unsigned char isect[off::isectSize]{};M::msg::putVec(isect,off::isectPos,hitPos);M::msg::putVec(isect,off::isectNormal,normal);
        const std::uint16_t c16=std::uint16_t(comp);std::memcpy(isect+off::isectComponent,&c16,2);
        std::memcpy(isect+off::isectInst,&chain.inst,sizeof chain.inst);
        try{api_.hitInfo(m+M::msg::localPos,isect,reinterpret_cast<const void*>(v.actor));}catch(...){}
    }
    int zone=M::msg::get<std::int32_t>(m,M::msg::zone);if(zone<0||zone>8)zone=cap.zone;
    const float damage=(fist?M::fistDamage(config.damage,maxKo,s.speed):M::bladeDamage(config.damage,zone,maxHp,s.speed,f.face==1))*(s.prop?std::max(s.koScale,0.f):1.f);
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
        if(fist){NodeWatch* w=startWatch(s.prop?WatchProp:WatchFist,WatchVariantPlayer,v.actor,v.guid,now); // the node of a punch that is known to animate, sampled before it
            if(w&&s.prop&&w->guid==v.guid){auto& x=w->push;x.tier=3;x.reaction=std::int8_t(reaction);x.face=std::int8_t(f.face);x.a=s.speed;x.dir=dir;x.hit=hitPos;x.comp=comp;
                x.dirFwd=M::dot(dir,v.rowC);x.impulse=s.strong?config.push*std::max(s.speed,1.f):0.f;x.impulseK=impulseScale;x.hpBefore=hpBefore;
                std::uintptr_t m{};float pos[3]{};if(get(v.actor,off::actorMover,m)&&m&&block(m+0x70,pos,sizeof pos)){x.start={pos[0],pos[1],pos[2]};x.haveStart=true;}}}
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
        if(config.m2&&api_.impulse&&(s.prop?s.strong:fist?s.speed>=4.5f:damage>=config.damage.lethal)){
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
    bump(fist?stats.hitsFist:stats.hitsBlade);bump(stats.byZone[unsigned(std::clamp(zone,0,8))]);if(kill)bump(stats.kills);if(ko)bump(stats.kos);if(s.prop)bump(stats.propStruck);if(s.thrown)bump(stats.propThrownStruck);
    bump(stats.hands[unsigned(s.hand)].strikes);
    const float amp=kill?1.f:ko?.8f:fist?.55f:.8f;const std::uint32_t us=kill?90'000u:ko?70'000u:fist?45'000u:65'000u;
    haptics::pulse(unsigned(s.hand),amp,us,now,kill?haptics::SourceMeleeKill:haptics::SourceMeleeHit);
    bump(kill?stats.hapticKill:ko?stats.hapticKo:stats.hapticHit);
    LastStrike l{};l.ns=now;l.hand=s.hand;l.mode=s.mode;l.victimGuid=v.guid;l.race=v.race;l.zone=zone;l.component=comp;l.reaction=reaction;l.face=f.face;
    l.speed=s.speed;l.damage=damage;l.hpBefore=hpBefore;l.hpAfter=hpAfter;l.msgHpAfter=M::msg::get<float>(m,M::msg::hpAfter);l.koBefore=koBefore;l.koAfter=koAfter;
    l.flags=M::msg::get<std::uint32_t>(m,M::msg::flags);l.hitPos=hitPos;l.probeHit=probeHit;l.instMatch=instMatch;l.fx=fx;l.dry=config.dry;l.kill=kill;l.ko=ko;
    l.source=source;std::memcpy(l.bone,m+M::msg::boneName,16);l.bone[16]=0;l.phantom=phantomCut;
    if(fist){l.grip=s.grip;l.gripMin=s.gripMin;l.closedMs=s.closedMs;l.closeMps=s.closeMps;
        bump(stats.fistClosedMsHist[s.closedMs<100.f?0u:s.closedMs<200.f?1u:s.closedMs<500.f?2u:3u]);
        bump(stats.fistMinGripHist[s.gripMin<.3f?0u:s.gripMin<.5f?1u:s.gripMin<=M::fistGrip?2u:3u]);}
    {std::lock_guard lock(lastMutex);lastStrike=l;}
    {char b[256];const Vec3 to=M::normalized(p.position-v.position);
     std::snprintf(b,sizeof b,"{\"a\":[%.2f,%.2f,%.2f],\"c\":[%.2f,%.2f,%.2f],\"toPlayer\":[%.2f,%.2f,%.2f],\"dotA\":%.2f,\"dotC\":%.2f}",
        double(v.rowA.x),double(v.rowA.y),double(v.rowA.z),double(v.rowC.x),double(v.rowC.y),double(v.rowC.z),double(to.x),double(to.y),double(to.z),
        double(M::dot(v.rowA,to)),double(M::dot(v.rowC,to)));
     std::lock_guard lock(dumpMutex);forwardRow=b;}
}

// HAND PHYSICS (unarmed hands): bodies for up to 3 candidates within 1.6 m of a live palm, every tick while hand physics asks for them.
// A humanoid reads its 17 key bones (one batched read), another body its bones (up to 128) for the cloud capsule (a horse: the upper
// cloud). Published even when none is near (count 0: the hands then meet nothing).
inline void Engine::nearBodies(std::uint64_t now) noexcept {
    namespace hp=::rdrvr::vr::handphys;
    if(!hp::nearWanted.load(std::memory_order_relaxed))return;
    hp::Palms pm{};if(!hp::palms.load(pm)||!(pm.live||pm.heavy)||!hp::fresh(now,pm.ns))return;
    const auto t0=nowNs();bump(stats.hpBodyTicks);if(!pm.live)bump(stats.hpPropOnly);
    struct D {unsigned i{};float d{};};std::array<D,maxCandidates> near{};unsigned nn=0;
    nearestPalmM_=-1.f;
    for(unsigned i=0;i<candCount_;++i){const float d=nearDistance(pm,cand_[i].position);if(std::isfinite(d)&&(nearestPalmM_<0.f||d<nearestPalmM_))nearestPalmM_=d;
        if(d<=1.6f)near[nn++]={i,d};}
    std::sort(near.begin(),near.begin()+nn,[](const D& a,const D& b){return a.d<b.d;});
    std::array<unsigned,3> who{};unsigned n=0;
    for(unsigned k=0;k<nn&&n<3;++k){
        auto& c=cand_[near[k].i];c.bodyValid=false;
        // the scan may be up to nearScanTicks old: a creature that left its slot (despawned) gives no body
        {std::uintptr_t a{};if(!slotActor(c.guid,a)||a!=c.actor){c.chain.ok=false;continue;}}
        if(!c.chain.ok||tickIndex_-c.chainTick>=30){
            if(deriveChain(c.actor,c.chain)){c.chainTick=tickIndex_;bump(stats.skeletons);c.layout=layoutFor(c.chain);}
            else{bump(stats.skeletonFailures);c.chain.ok=false;}}
        bool built=false;
        if(c.chain.ok&&c.layout>=0&&layouts_[unsigned(c.layout)].humanoid){
            const auto& l=layouts_[unsigned(c.layout)].layout;
            std::array<Vec3,M::BoneKeyCount> pos{};std::array<float,M::BoneKeyCount*3> raw{};std::array<diagnostics::ReadSegment,M::BoneKeyCount> sg{};
            std::array<unsigned,M::BoneKeyCount> key{};unsigned ns=0;
            for(unsigned b=0;b<M::BoneKeyCount;++b){const int i=l.index[b];if(i<0||unsigned(i)>=c.chain.bones)continue;
                key[ns]=b;sg[ns]={c.chain.globals+std::uintptr_t(i)*off::matrixStride+off::matrixPos,&raw[std::size_t(ns)*3],12};++ns;}
            if(ns&&vread(sg.data(),ns)){
                std::array<bool,M::BoneKeyCount> have{};
                for(unsigned j=0;j<ns;++j){pos[key[j]]={raw[std::size_t(j)*3],raw[std::size_t(j)*3+1],raw[std::size_t(j)*3+2]};have[key[j]]=true;}
                built=M::buildBody(l,[&](unsigned b,Vec3& out){if(!have[b])return false;out=pos[b];return (out-c.position).length()<4.f;},c.body)>=2;
                if(built)bump(stats.humanoidBodies);}
        }
        if(!built&&c.chain.ok){
            const unsigned nb=std::min(c.chain.bones,unsigned(maxBones));
            if(block(c.chain.globals,boneBuf_.data(),std::size_t(nb)*off::matrixStride)){
                std::array<Vec3,maxBones> cloud{};unsigned used=0;
                for(unsigned i=0;i<nb;++i){const auto q=vec(boneBuf_.data(),std::size_t(i)*off::matrixStride+off::matrixPos);if(M::finite(q)&&(q-c.position).length()<4.f)cloud[used++]=q;}
                M::Capsule cc{};c.body.count=0;
                if(c.race==1?M::upperCloudCapsule(cloud.data(),used,cc):M::cloudCapsule(cloud.data(),used,cc)){c.body.capsules[c.body.count++]=cc;built=true;bump(stats.cloudBodies);}
            }
        }
        if(!built){c.body.count=0;c.body.capsules[c.body.count++]=M::actorCapsule(c.position);bump(stats.actorBodies);}
        c.bodyValid=true;bump(stats.hpBuilt);if(c.race==1)bump(stats.hpHorses);
        who[n++]=near[k].i;
    }
    publishBodies(who,n,now);
    const double us=double(nowNs()-t0)/1000.0;bump(stats.hpReadUsSum,std::uint64_t(us));bump(stats.hpReadN);maxOf(stats.hpReadUsMax,us);
}
// The bodies of these candidates as hand-physics virtual bodies; vel = the pelvis (else the first capsule's middle) moved since the
// last tick of the same creature.
inline void Engine::publishBodies(const std::array<unsigned,3>& who,unsigned n,std::uint64_t now) noexcept {
    namespace hp=::rdrvr::vr::handphys;
    hp::VBodies vb{};vb.ns=now;vb.serial=tickIndex_;vb.nearestM=nearestPalmM_;
    for(unsigned k=0;k<n&&k<who.size();++k){
        const auto& c=cand_[who[k]];if(!c.bodyValid||!c.body.count)continue;
        hp::VBody& b=vb.b[vb.count];b=hp::VBody{};b.actor=c.actor;b.guid=c.guid;b.race=c.race;b.rowA={c.rowA.x,c.rowA.y,c.rowA.z};
        const unsigned nc=std::min<unsigned>(c.body.count,unsigned(hp::vbodyCaps));
        Vec3 ref{};bool haveRef=false;
        for(unsigned i=0;i<nc;++i){const auto& q=c.body.capsules[i];
            b.c[i]={{q.a.x,q.a.y,q.a.z},{q.b.x,q.b.y,q.b.z},q.radius,std::uint8_t(std::clamp(q.zone,0,255)),q.kind};
            if(!haveRef&&(q.kind==3||q.kind==2)){ref=q.b;haveRef=true;}}
        if(!haveRef)ref=(c.body.capsules[0].a+c.body.capsules[0].b)*.5f;
        b.count=std::uint8_t(nc);
        Prev* pv=nullptr;for(auto& x:prev_)if(x.guid==c.guid){pv=&x;break;}
        if(pv&&now>pv->ns&&now-pv->ns<100'000'000ull){const float dt=float(double(now-pv->ns)*1e-9);const Vec3 v=(ref-pv->pos)*(1.f/dt);
            if(M::finite(v)&&v.length()<15.f)b.vel={v.x,v.y,v.z};}
        if(!pv){pv=&prev_[0];for(auto& x:prev_)if(x.ns<pv->ns)pv=&x;}
        *pv={c.guid,ref,now};
        hp::finishBody(b);if(b.count)++vb.count;
    }
    hp::vbodies.store(vb);bump(stats.hpPublished);
}
// HAND PHYSICS open-hand touches (handphys TOUCH matched: hp::touchReady). Never apply(), the player as an attacker or any damage
// (consumeTouch): the engine's own reaction node of the kind (levers slapnode / pushnode, playNode), else the injure message
// openTouchPlan allows: the creature itself (never a zombie), a guid no consumer resolves (actor slot 0, its generation flipped:
// ReportWasHitMelee, sagPhysicalComponent / animAnimatorComponent and hlthHealthComponent::Injure check it) or none:
//  - SLAP: the victim's reaction field (aniMelee +0x550: UP_FOR / UP_RGT / UP_LFT), the node or a 0-damage message, the strike audio, a
//    small yaw and a 3 cm offset;
//  - SHOVE: by tier (handphys pushTier): a firm one SHOVED (KNCKBK by side when strong), its node or the same message (the impulse along
//    the push, the face from the push's direction); a medium one only the push (an UP reaction message with lever pushmedium 1), an
//    escalated press (light) only the push; a push of 5..45 cm along the ground over 6+ ticks (30 cm when a press escalated); a firm
//    shove's kick (lever pushkick): NMShotTask takes the message's bulletVel for no force (newHit_entry 0x1fa48bc: the hit part and
//    shotFromBehind only) and the mover push stops at the ragdoll, so when the mover turns ragdoll (0 at the push, set within 200 ms) one
//    projMsgApplyImpulse along the palm (hp::pushKickPlan) at the torso moves the body (entPendingImpulse::ApplyImpulse: NM
//    "applyImpulse"); one kick per creature for a two-hand shove; it carries no attacker guid;
//  - PRESS: every tick a push of gain * (depth - 1 cm) (capped per tick and per press) and the yaw of that push about the body's centre.
// The pushes go through mvrMoverComponent::ApplyTranslationOffset (force false), as aniMelee::ApplyMinDist does, only for a human whose
// mover (either mover class) is not busy (+0x846 / +0x848), not a ragdoll (+0xb8c) and whose flags (+0xb85) allow it; the offset is
// pre-rotated by the mover's heading (+0x18), which the call turns it by.
inline constexpr int PushWhyBusyCode=::rdrvr::vr::handphys::PushWhyBusy,PushWhyRagdollCode=::rdrvr::vr::handphys::PushWhyRagdoll,
    PushWhyFlagsCode=::rdrvr::vr::handphys::PushWhyFlags;
inline bool Engine::pushTarget(std::uintptr_t actor,int race,std::uintptr_t& mover,std::uintptr_t& anim,int* whyOut,bool count) noexcept {
    namespace hp=::rdrvr::vr::handphys;
    mover=0;anim=0;std::uintptr_t comp{},vt{},m{};std::int32_t creatureRace{-1};std::int16_t busyA{1},busyB{1};std::uint32_t flags{~0u},ragdoll{1};
    if(race==0&&get(actor,off::actorComponent,comp)&&comp)get(comp,off::compRace,creatureRace);
    const bool have=race==0&&creatureRace==0&&get(actor,off::actorMover,m)&&m&&get(m,0,vt);
    bool vtOk=false;
    if(have){
        if(vt&&vt==hp::touchApi.moverSimVptr){vtOk=true;bump(stats.moverSim);}
        else if(vt&&vt==hp::touchApi.moverVptr){vtOk=true;bump(stats.moverBase);}
        else{bump(stats.moverOther);stats.moverOtherRel.store(vt-hp::touchApi.base,std::memory_order_relaxed);}}
    if(vtOk&&!(get(m,0x846,busyA)&&get(m,0x848,busyB)&&get(m,0xb85,flags)&&get(m,0xb8c,ragdoll))){busyA=1;}
    const int why=hp::pushGuard(race,creatureRace,vtOk,busyA,busyB,ragdoll,flags);if(whyOut)*whyOut=why;
    if(why>=0){static constexpr PushRefuse map[5]{PushNotHuman,PushMoverVt,PushRagdoll,PushFlags,PushBusy};if(count)bump(stats.pushRefused[map[why]]);return false;}
    mover=m;std::uintptr_t a{},avt{};if(get(actor,off::actorAnim,a)&&a&&get(a,0,avt)&&avt==hp::touchApi.animVptr)anim=a;
    return true;
}
// One offset: pre-rotated by the heading the call turns it by (not for a mover whose +0xb85 word has bit 0x4000000: that branch stores
// it unturned), then +0x860..+0x868 read back (stored = its horizontal part grew by half the asked horizontal length or more: the call
// zeroes y for a mover not swimming underwater); the mover is checked for the consumption at the next tick.
inline void Engine::translate(std::uintptr_t mover,Vec3 world) noexcept {
    namespace hp=::rdrvr::vr::handphys;
    using TranslateFn=void(*)(void*,const float*,bool);
    float h{0.f};if(!get(mover,0x18,h)||!std::isfinite(h))h=0.f;
    std::uint32_t flags{hp::moverNoTurnFlag};get(mover,0xb85,flags); // unread: no turn assumed (the push is left unturned, never doubled)
    if(h!=0.f&&!(flags&hp::moverNoTurnFlag)){bump(stats.headingTurned);stats.headingLast.store(h,std::memory_order_relaxed);}
    const hp::V3 v=hp::preRotate({world.x,world.y,world.z},h,flags);
    float before[3]{},after[3]{};const bool b0=block(mover+0x860,before,sizeof before);
    alignas(16) const float a[4]{v[0],v[1],v[2],0.f};
    try{reinterpret_cast<TranslateFn>(hp::touchApi.translate)(reinterpret_cast<void*>(mover),a,false);}catch(...){bump(stats.exceptions);}
    if(b0&&block(mover+0x860,after,sizeof after)){
        const float dx=after[0]-before[0],dz=after[2]-before[2];
        bump(std::sqrt(dx*dx+dz*dz)>=.5f*std::sqrt(v[0]*v[0]+v[2]*v[2])?stats.pushStored:stats.pushNotStored);}
    bool known=false;for(unsigned i=0;i<pushedCount_;++i)known=known||pushedMovers_[i]==mover;
    if(!known&&pushedCount_<pushedMovers_.size())pushedMovers_[pushedCount_++]=mover;
}
// The movers given an offset since the last tick: +0x860..+0x868 back to 0 = GetDesiredMotion took it (consumed).
inline void Engine::checkPushed() noexcept {
    for(unsigned i=0;i<pushedCount_;++i){float o[3]{1.f,1.f,1.f};
        if(block(pushedMovers_[i]+0x860,o,sizeof o))bump(o[0]==0.f&&o[1]==0.f&&o[2]==0.f?stats.pushConsumed:stats.pushNotConsumed);}
    pushedCount_=0;
}
// A job of the same graded push (recNs) still running is never replaced: its remaining distance becomes the larger of what is left and
// the new job's (the new plan and direction when the new one is longer; a wait keeps the later deadline). Any other job of the creature
// is replaced; a full queue is counted.
inline void Engine::queuePush(std::uint32_t guid,std::uintptr_t actor,Vec3 dir,float perTick,unsigned ticks,float yawPerTick,unsigned yawTicks,std::uint64_t recNs,
                              std::uint64_t waitUntilNs) noexcept {
    PushJob* j=nullptr;for(auto& x:pushes_)if(x.guid==guid){j=&x;break;}
    if(j&&recNs&&j->recNs==recNs&&j->actor==actor&&(j->ticks||j->yawTicks)){
        const float rem=std::fabs(j->perTick)*float(j->ticks),add=std::fabs(perTick)*float(ticks);
        if(add>rem){j->askedM+=add-rem;j->dir=dir;j->perTick=perTick;j->ticks=ticks;}
        if(yawTicks){j->yawPerTick=yawPerTick;j->yawTicks=yawTicks;}
        j->waitUntilNs=std::max(j->waitUntilNs,waitUntilNs);bump(stats.jobMerged);
        for(auto& x:pushRecs_)if(x.guid==guid&&x.ns==recNs&&x.jobMerged<0xffffu)++x.jobMerged;
        return;}
    if(!j)for(auto& x:pushes_)if(!x.ticks&&!x.yawTicks&&!x.measure){j=&x;break;}
    if(!j){bump(stats.jobNoSlot);return;}
    *j={guid,actor,dir,perTick,ticks,yawPerTick,yawTicks};j->recNs=recNs;j->waitUntilNs=waitUntilNs;
    // the start position (mover inst matrix row d, mover +0x70) for the moved distance once the job ends
    std::uintptr_t m{};float pos[3]{};if(get(actor,off::actorMover,m)&&m&&block(m+0x70,pos,sizeof pos)){j->start={pos[0],pos[1],pos[2]};j->haveStart=true;}
    j->askedM=std::fabs(perTick)*float(ticks);
}
// The queued pushes and yaws, one step each per tick; every step checks the creature and its mover again. A job whose steps are done
// is measured one tick later (the horizontal move of its mover since the start against the asked distance, world units).
inline void Engine::runPushes(std::uint64_t now) noexcept {
    namespace hp=::rdrvr::vr::handphys;
    using RotateFn=void(*)(void*,float);
    for(auto& j:pushes_){
        if(j.measure&&!j.ticks&&!j.yawTicks){
            std::uintptr_t m{};float pos[3]{};
            if(j.haveStart&&get(j.actor,off::actorMover,m)&&m&&block(m+0x70,pos,sizeof pos)){
                const float dx=pos[0]-j.start.x,dz=pos[2]-j.start.z,moved=std::sqrt(dx*dx+dz*dz);
                if(std::isfinite(moved)){bump(stats.jobsMeasured);bump(stats.jobAskedUmSum,std::uint64_t(j.askedM*1e6f));bump(stats.jobMovedUmSum,std::uint64_t(moved*1e6f));
                    stats.jobAskedLastCm.store(j.askedM*100.f,std::memory_order_relaxed);stats.jobMovedLastCm.store(moved*100.f,std::memory_order_relaxed);}}
            j=PushJob{};continue;}
        if(!j.ticks&&!j.yawTicks)continue;
        std::uintptr_t a{},mover{},anim{};
        int why=-1;
        const bool live=hp::touchReady.load(std::memory_order_acquire)&&slotActor(j.guid,a)&&a==j.actor;
        const bool waiting=j.waitUntilNs&&now<j.waitUntilNs;
        if(!live||!pushTarget(j.actor,0,mover,anim,&why,!waiting)){
            // a stumble waiting on a frozen / suspended mover keeps its place until the deadline
            if(live&&waiting&&why==PushWhyBusyCode){if(!j.waitStartNs)j.waitStartNs=now?now:1;continue;}
            if(live&&j.waitUntilNs&&why==PushWhyBusyCode)bump(stats.stumbleBusyTimeout);
            bump(stats.pushRefused[PushStale]);
            for(auto& x:pushRecs_)if(x.guid==j.guid&&(!j.recNs||x.ns==j.recNs)){const unsigned k=why==PushWhyBusyCode?0u:why==PushWhyRagdollCode?1u:why==PushWhyFlagsCode?2u:3u;if(x.refused[k]<0xffffu)++x.refused[k];}
            j=PushJob{};continue;}
        if(j.waitStartNs){bump(stats.stumbleWaitOk);const auto ms=now>=j.waitStartNs?std::int32_t(std::min<std::uint64_t>((now-j.waitStartNs)/1'000'000ull,1'000'000ull)):0;
            for(auto& x:pushRecs_)if(x.guid==j.guid&&x.ns==j.recNs)x.waitMs=ms;
            j.waitStartNs=0;}
        j.waitUntilNs=0;
        if(j.ticks){for(auto& x:pushRecs_)if(x.guid==j.guid&&(!j.recNs||x.ns==j.recNs))x.appliedW+=std::fabs(j.perTick);
            translate(mover,j.dir*j.perTick);bump(stats.pushTicks);bump(stats.offsetUmSum,std::uint64_t(std::fabs(j.perTick)*1e6f));--j.ticks;if(!j.ticks)j.measure=true;}
        if(j.yawTicks){if(anim){try{reinterpret_cast<RotateFn>(hp::touchApi.rotate)(reinterpret_cast<void*>(anim),j.yawPerTick);}catch(...){}bump(stats.yawTicks);}--j.yawTicks;}
    }
}
inline void Engine::dropTouch() noexcept {
    namespace hp=::rdrvr::vr::handphys;
    for(unsigned h=0;h<2;++h){hp::TouchRequest r{};if(hp::touchRequest[h].load(r)&&r.serial&&r.serial!=touchSerial_[h]){touchSerial_[h]=r.serial;bump(stats.touchGateClosed);}}
    {hp::TouchRequest r{};if(hp::thrownTouchRequest.load(r)&&r.serial&&r.serial!=touchSerial_[2]){touchSerial_[2]=r.serial;bump(stats.touchGateClosed);}}
    pushes_={};frozenHold_={};
}
// The node watch: guarded reads only (no call); the names through the GetName words the handphys DIAG prints matched.
// A node's name: vtable +0x10 = ActionNodeImplementation::GetName (CName at node +0x30) or ActionNodeReference::GetName (the node at
// +0x48, followed once); any other word = unknown (id 0).
// One read of the node's first 0x50 bytes (vtable +0x0, parent +0x8, name text +0x30, hash +0x38, a reference's node +0x48), one of the
// vtable slot; the text only when asked. parent = the node's parent (0 none).
// The release data keeps a node's CName text pointer null (CName::DeclareStruct 0x28ff574 reads it; GetString 0x28fe9f0 then prints the
// hash): a name is its atStringHash (0x1a52d88, seed 0, lowercase) - the proven ones by text, any other as "#hash".
inline const char* knownNodeName(std::uint32_t id) noexcept {
    struct K {std::uint32_t id;const char* name;};
    static constexpr K known[]{{0xdb982272u,"global"},{0xfae7d49cu,"injury"},{0x003a574bu,"meleehit"},{0x89f230a2u,"FRONT"},{0x37418674u,"BACK"},
        {0x6fa34840u,"LEFT"},{0xb8ccc339u,"RIGHT"},{0x6d6bc7b7u,"INTERRUPT"},{0x1597ee16u,"gped"},{0xbd701184u,"default_character_Main"},
        {0x94cb9bc1u,"defaultgait"},{0x95f3abacu,"default_ai"}};
    for(const auto& k:known)if(k.id==id)return k.name;
    return nullptr;}
inline void hashName(std::uint32_t id,char* text,std::size_t n) noexcept {
    if(!text||n<2||!id)return;
    if(const char* k=knownNodeName(id)){std::size_t i=0;for(;i+1<n&&k[i];++i)text[i]=k[i];text[i]=0;return;}
    std::snprintf(text,n,"#%08x",id);}
inline bool Engine::nodeName(std::uintptr_t node,std::uint32_t& id,char* text,std::size_t n,std::uintptr_t* parent) const noexcept {
    namespace hp=::rdrvr::vr::handphys;
    id=0;if(text&&n)text[0]=0;if(parent)*parent=0;
    const auto b=hp::touchApi.base;
    for(int hop=0;hop<2&&node;++hop){
        alignas(8) unsigned char nb[0x50]{};std::uintptr_t fn{};
        if(!block(node,nb,sizeof nb))return false;
        const auto vt=at<std::uintptr_t>(nb,0);if(!vt||!get(vt,0x10,fn))return false;
        if(hop==0&&parent)*parent=at<std::uintptr_t>(nb,0x8);
        if(fn==b+hp::nodeImplGetName){
            const auto str=at<std::uintptr_t>(nb,0x30);id=at<std::uint32_t>(nb,0x38);
            if(text&&n>1&&str){char t[32]{};const bool got=block(str,t,sizeof t)||block(str,t,16); // 16 bytes when 32 cross into an unmapped page
                if(got){std::size_t k=0;for(;k+1<n&&k<sizeof t-1&&t[k];++k)text[k]=(t[k]>=0x20&&t[k]<=0x7e&&t[k]!='"'&&t[k]!='\\')?t[k]:'?';text[k]=0;}}
            if(text&&n>1&&!text[0])hashName(id,text,n);
            return true;}
        if(fn!=b+hp::nodeRefGetName)return false;
        node=at<std::uintptr_t>(nb,0x48);}
    return false;
}
// One sample of a creature's action controller (sagActorComponent +0xa8): the current node (comp +0xb0 = controller +0x8), its name, its
// path (the names up its parents, node +0x8, to its tree's top; rage::ActionNode::Find 0x28f0e9c walks a "/a/b" path down from the global
// root matching each child's name hash, a bare word only among the root's children), the play counter (+0x1c, +1 on every Play and node
// change) and the time in the node (+0x18), the injury react level ([[[anim +0xab8] +0x78] +0x4c], InjuryTask::Begin), the mover's ragdoll
// / busy words, the first 2 running tasks (+0xc8, next +0x8; vtables as base offsets), actor +0x112 and component +0x915. Only for the
// sample before an action and on a change (the watch's per-tick check is nodeChange).
inline bool Engine::readNode(std::uintptr_t actor,NodeSample& x,std::uintptr_t& node) const noexcept {
    namespace hp=::rdrvr::vr::handphys;
    x=NodeSample{};node=0;std::uintptr_t comp{},vt{};
    if(!hp::touchApi.nodeOk||!get(actor,off::actorComponent,comp)||!comp||!get(comp,0,vt)||!vt)return false;
    if(hp::touchApi.creatureVptr&&vt!=hp::touchApi.creatureVptr)return false;
    const std::uintptr_t ac=comp+0xa8;
    if(!get(comp,0xb0,node))return false;
    get(ac,0x1c,x.counter);get(ac,0x18,x.timeIn);
    get(actor,0x112,x.f112);get(comp,0x915,x.f915);
    if(node){ // the node, then (a node not in paths_) its parents (node +0x8) up to the top of its tree (parent 0), 10 levels at most
        {std::uintptr_t p{};nodeName(node,x.id,x.name,sizeof x.name,&p);}
        bool cached=false;
        for(const auto& e:paths_)if(e.node==node&&e.id==x.id&&x.id){x.parents=e.parents;x.rooted=e.rooted;std::memcpy(x.path,e.path,sizeof x.path);cached=true;break;}
        if(!cached){char seg[10][32]{};std::uintptr_t cur=node,top=0;unsigned k=0;
            for(;k<10&&cur;++k){std::uintptr_t p{};std::uint32_t id{};if(!nodeName(cur,id,seg[k],sizeof seg[k],&p))seg[k][0]=0;
                if(k==0)x.id=id;else if(k<=3)x.parents[k-1]=id;
                top=cur;cur=p;}
            const bool deeper=cur!=0;
            std::memcpy(x.name,seg[0],sizeof x.name-1);x.name[sizeof x.name-1]=0;
            {std::uintptr_t g{},root{};const auto b=hp::touchApi.base;
             if(b&&get(b+hp::actionRootGot,0,g)&&g==b+hp::actionRootVar&&get(g,0,root)&&root)x.rooted=!deeper&&top==root?1:0;}
            x.path[0]=0;std::size_t w=0;const auto add=[&](const char* t){for(;*t&&w+1<sizeof x.path;++t)x.path[w++]=*t;x.path[w]=0;};
            const unsigned shown=x.rooted==1?k-1u:k; // the root's own name is no part of a Find path; another tree keeps its top's name
            if(deeper)add("...");else if(x.rooted==0)add("!");else if(x.rooted<0)add("?");
            for(unsigned i=shown;i-- >0;){if(i+1u<shown||x.rooted==1||deeper)add("/");add(seg[i][0]?seg[i]:"?");}
            if(x.id&&x.rooted>=0){auto& e=paths_[pathNext_];pathNext_=(pathNext_+1u)%unsigned(paths_.size());
                e.node=node;e.id=x.id;e.parents=x.parents;e.rooted=x.rooted;std::memcpy(e.path,x.path,sizeof e.path);}}}
    {std::uintptr_t anim{},blk{},rx{};std::int32_t lvl{-1};
     if(get(actor,off::actorAnim,anim)&&anim&&get(anim,off::animBlock,blk)&&blk&&get(blk,0x78,rx)&&rx&&get(rx,0x4c,lvl))x.react=lvl;}
    {std::uintptr_t m{};if(get(actor,off::actorMover,m)&&m){get(m,0xb8c,x.ragdoll);get(m,0x846,x.busyA);get(m,0x848,x.busyB);}}
    {std::uintptr_t t{};get(ac,0xc8,t);
     for(unsigned i=0;i<4&&t;++i){std::uintptr_t tv{};if(!get(t,0,tv))break;const std::uint32_t o=tv>=hp::touchApi.base?std::uint32_t(tv-hp::touchApi.base):0u;
        if(i<2)x.tasks[i]=o;if(o==hp::staggerFallTaskVt&&!x.sfTask)x.sfTask=t;if(!get(t,0x8,t))break;}}
    x.ok=true;return true;
}
// A node's children as rage::ActionNode::GetChildCount / GetChild read them (u16 +0x18, array +0x10), only for a node whose vtable +0x60
// holds that function or ActionNodeReference::GetChildCount (then the node at +0x48, once): guarded reads, no call.
inline constexpr std::uintptr_t nodeChildCountFn=0x28f2ef8,nodeRefChildCountFn=0x28f290c;
inline bool Engine::nodeChildren(std::uintptr_t node,std::uintptr_t& list,std::uint16_t& count) const noexcept {
    namespace hp=::rdrvr::vr::handphys;
    list=0;count=0;const auto b=hp::touchApi.base;
    for(int hop=0;hop<2&&node;++hop){std::uintptr_t vt{},fn{};
        if(!get(node,0,vt)||!vt||!get(vt,0x60,fn))return false;
        if(fn==b+nodeRefChildCountFn){if(!get(node,0x48,node))return false;continue;}
        if(fn!=b+nodeChildCountFn)return false;
        return get(node,0x18,count)&&get(node,0x10,list)&&(list||!count);}
    return false;}
// The meleehit node's children and grandchildren (ids, child counts; 32 at most), read once (diagnostics: the stagger nodes FRONT / BACK /
// LEFT / RIGHT and the knockdowns under it).
inline void Engine::readMeleeTree(std::uintptr_t node) const noexcept {
    std::array<TreeNode,32> out{};unsigned n=0;std::uintptr_t list{};std::uint16_t count{};
    if(!nodeChildren(node,list,count))return;
    for(unsigned i=0;i<std::min<unsigned>(count,12u)&&n<out.size();++i){std::uintptr_t c{};if(!get(list,i*8u,c)||!c)continue;
        std::uint32_t id{};nodeName(c,id,nullptr,0);std::uintptr_t l2{};std::uint16_t c2{};const bool ok=nodeChildren(c,l2,c2);
        out[n++]={1,id,ok?c2:std::uint16_t(0xffff)};
        for(unsigned j=0;ok&&j<std::min<unsigned>(c2,6u)&&n<out.size();++j){std::uintptr_t g{};if(!get(l2,j*8u,g)||!g)continue;
            std::uint32_t gid{};nodeName(g,gid,nullptr,0);std::uintptr_t l3{};std::uint16_t c3{};out[n++]={2,gid,nodeChildren(g,l3,c3)?c3:std::uint16_t(0xffff)};}}
    bump(stats.meleeTreeReads);
    std::lock_guard lock(treeMutex);meleeTree=out;meleeTreeCount=n;meleeTreeRoot=0x003a574bu;meleeTreeNode=node;}
// A knockdown's landed node: up its parents (node +0x8, 8 levels) to the meleehit node; the meleehit subtree read from that node (the
// one on the landed path, at most once per meleehit node), then the selection of meleehit's children and of the two nodes below it on
// the path (dumpTreeLevel; each parent once, 4 at most). TREE prints only (refused and counted otherwise); guarded reads, no call.
inline void Engine::readInjuryTree(std::uintptr_t node) noexcept {
    namespace hp=::rdrvr::vr::handphys;
    if(!hp::touchApi.treeOk){bump(stats.injuryTreeRefused);return;}
    std::array<std::uintptr_t,8> chain{};unsigned n=0;int mh=-1;std::uintptr_t cur=node;
    for(;n<chain.size()&&cur;++n){std::uintptr_t p{};std::uint32_t id{};if(!nodeName(cur,id,nullptr,0,&p))break;chain[n]=cur;if(id==0x003a574bu){mh=int(n);break;}cur=p;}
    if(mh<1)return;
    {std::uintptr_t done{};{std::lock_guard lock(treeMutex);done=meleeTreeNode;}if(done!=chain[unsigned(mh)])readMeleeTree(chain[unsigned(mh)]);}
    for(int k=mh;k>=1&&k>=mh-2;--k)dumpTreeLevel(chain[unsigned(k)]);}
inline void Engine::dumpTreeLevel(std::uintptr_t parent) noexcept {
    namespace hp=::rdrvr::vr::handphys;
    {std::lock_guard lock(treeMutex);if(injuryTreeN>=injuryTree.size())return;for(unsigned i=0;i<injuryTreeN;++i)if(injuryTree[i].node==parent)return;}
    TreeDump d{};d.node=parent;std::uintptr_t list{};std::uint16_t count{};
    if(!nodeName(parent,d.id,nullptr,0)||!nodeChildren(parent,list,count))return;
    d.count=count;const auto b=hp::touchApi.base;
    for(unsigned i=0;i<count&&d.n<d.ch.size();++i){std::uintptr_t c{};if(!get(list,i*8u,c)||!c)continue;auto& x=d.ch[d.n++];
        nodeName(c,x.id,nullptr,0);get(c,0x2f,x.type);
        {std::uintptr_t l2{};std::uint16_t c2{};if(nodeChildren(c,l2,c2))x.children=std::int16_t(std::min<unsigned>(c2,0x7fffu));}
        if(x.type!=3&&x.type!=4){std::uintptr_t t{};if(get(c,0x48,t)&&t)nodeName(t,x.refId,nullptr,0);continue;}
        std::uintptr_t arr{};std::uint16_t nc{};if(!get(c,0x40,arr)||!get(c,0x48,nc))continue;x.conds=nc;
        for(unsigned j=0;j<nc&&j<x.c.size()&&arr;++j){std::uintptr_t cond{},vt{},fn{};if(!get(arr,j*8u,cond)||!cond||!get(cond,0,vt)||!vt||!get(vt,0x68,fn))continue;
            auto& q=x.c[j];q.fn=fn>=b&&fn-b<0x4000000u?std::uint32_t(fn-b):0xffffffffu;
            alignas(4) unsigned char wb[16]{};if(block(cond+0x8,wb,sizeof wb))std::memcpy(q.w.data(),wb,sizeof wb);}}
    bump(stats.injuryTreeReads);
    std::lock_guard lock(treeMutex);if(injuryTreeN<injuryTree.size())injuryTree[injuryTreeN++]=d;}
// A watch: a sample before the action, then every tick for watchNs the node changes (up to 4). A slot of the same creature is taken over
// only from a press control watch; a push or slap also takes a press or walk watch of another creature (a slap, push, fist or prop hit in
// progress is never overwritten).
inline NodeWatch* Engine::startWatch(unsigned kind,unsigned variant,std::uintptr_t actor,std::uint32_t guid,std::uint64_t now) noexcept {
    NodeWatch w{};w.set=true;w.kind=std::uint8_t(std::min(kind,WatchKindCount-1u));w.variant=std::uint8_t(std::min(variant,WatchVariantCount-1u));
    w.guid=guid;w.actor=actor;w.t0=now;
    if(!readNode(actor,w.before,w.lastNode))return nullptr;
    w.lastCounter=w.before.counter;
    NodeWatch* slot=nullptr;
    for(auto& x:watches_)if(!x.set){slot=&x;break;}
    if(!slot&&w.kind!=WatchPress)for(auto& x:watches_)if(x.guid==guid&&x.kind==WatchPress){slot=&x;break;}
    // a push or a slap (its kick and ring entry) takes over any press or walk watch, never a fist, a prop hit or another push
    const bool pushKind=w.kind==WatchPush||w.kind==WatchSlap;
    if(!slot&&pushKind)for(auto& x:watches_)if(x.kind==WatchPress||x.kind==WatchWalk){slot=&x;break;}
    if(!slot){bump(stats.watchDropped);if(pushKind)bump(stats.pushDropped);return nullptr;}
    *slot=w;bump(stats.watchStarted);return slot;
}
// The change signal of a watch: the controller's node and play counter in one read (comp +0xb0 .. +0xc8: node, lock, time, counter).
inline bool Engine::nodeChange(std::uintptr_t actor,std::uintptr_t& node,std::uint32_t& counter) const noexcept {
    std::uintptr_t comp{};alignas(8) unsigned char cb[0x18]{};
    if(!get(actor,off::actorComponent,comp)||!comp||!block(comp+0xb0,cb,sizeof cb))return false;
    node=at<std::uintptr_t>(cb,0);counter=at<std::uint32_t>(cb,0x14);return true;
}
// The pelvis and head bones of a watched push (skeleton globals of t0, one guarded vector read) into sample k (0 t0, 1 300 ms, 2 600 ms);
// a position beyond 4 m of the mover's start is no reading.
inline bool Engine::readFallBones(PushInfo& x,unsigned k) const noexcept {
    if(k>=3||!x.globals||x.pelvisBone<0||x.headBone<0||unsigned(x.pelvisBone)>=x.boneCount||unsigned(x.headBone)>=x.boneCount)return false;
    float raw[6]{};diagnostics::ReadSegment sg[2]{{x.globals+std::uintptr_t(x.pelvisBone)*off::matrixStride+off::matrixPos,&raw[0],12},
        {x.globals+std::uintptr_t(x.headBone)*off::matrixStride+off::matrixPos,&raw[3],12}};
    if(!vread(sg,2))return false;
    const Vec3 p{raw[0],raw[1],raw[2]},h{raw[3],raw[4],raw[5]};
    if(!M::finite(p)||!M::finite(h)||(x.haveStart&&((p-x.start).length()>4.f||(h-x.start).length()>4.f)))return false;
    x.pelvis[k]=p;x.head[k]=h;x.haveBones[k]=true;return true;
}
// A watched push or prop hit, every tick: on the first tick after it the impulse NotifyInjured stored for NM (aniNMStateData +0x30,
// [[anim +0xab8] +0x88]) and what the animator holds (+0x8b8 face, +0xaec injury type, aniMelee +0x550 reaction, the react level); the
// mover's ragdoll word (its first tick, the value at 150 ms); the bones at 300 ms; the kick (lever pushkick: projMsgApplyImpulse along the
// palm on the first tick the mover is ragdoll within 200 ms; entPendingImpulse::Update applies it only while the object is active).
inline void Engine::watchPush(NodeWatch& w,std::uint64_t age) noexcept {
    auto& x=w.push;
    if(!x.bulletRead&&age>0){x.bulletRead=true;std::uintptr_t anim{},blk{},nm{};float b3[3]{};
        if(get(w.actor,off::actorAnim,anim)&&anim&&get(anim,off::animBlock,blk)&&blk&&get(blk,0x88,nm)&&nm&&block(nm+0x30,b3,sizeof b3))x.bullet={b3[0],b3[1],b3[2]};
        if(anim){x.landedRead=true;std::int32_t f{};std::uint32_t inj{};if(get(anim,0x8b8,f))x.faceLanded=f;if(get(anim,0xaec,inj))x.injuryLanded=inj;
            std::uintptr_t rx{},vm{},vvt{};std::int32_t lvl{},re{};
            if(blk&&get(blk,0x78,rx)&&rx&&get(rx,0x4c,lvl))x.reactLevel=lvl;
            if(blk&&get(blk,off::blockMelee,vm)&&vm&&get(vm,0,vvt)&&vvt==api_.aniMeleeVtable&&get(vm,off::meleeReaction,re))x.reactionLanded=re;}}
    // the knockdown message's own read, one tick after it (a push a second hand raised to a knockdown sent it after the first read)
    if(x.knockAge>=0&&!x.knockRead&&age>std::uint64_t(x.knockAge)){x.knockRead=true;std::uintptr_t anim{},blk{},nm{},vm{},vvt{};float b3[3]{};std::int32_t f{},re{};
        if(get(w.actor,off::actorAnim,anim)&&anim){if(get(anim,0x8b8,f))x.kFace=f;
            if(get(anim,off::animBlock,blk)&&blk){if(get(blk,0x88,nm)&&nm&&block(nm+0x30,b3,sizeof b3))x.kBullet={b3[0],b3[1],b3[2]};
                if(get(blk,off::blockMelee,vm)&&vm&&get(vm,0,vvt)&&vvt==api_.aniMeleeVtable&&get(vm,off::meleeReaction,re))x.kReaction=re;}}}
    // the pelvis's velocity over 0-50, 50-100 and 100-200 ms after kick 1, in the creature's frame at t0
    if(x.graded&&x.haveKickPelvis){static constexpr std::uint64_t marks[3]{50'000'000ull,100'000'000ull,200'000'000ull};
        for(unsigned k=0;k<3;++k){if(x.kvDone[k]||age<x.kickPelvisAge+marks[k])continue;x.kvDone[k]=true;
            if(k==0){x.kvPrev=x.kickPelvis;x.kvPrevAge=x.kickPelvisAge;} // a failed read leaves the last good sample as the next start
            Vec3 pv{};const float dt=age>x.kvPrevAge?float(double(age-x.kvPrevAge)*1e-9):0.f;const float u=x.upm>1e-3f?x.upm:1.f;
            if(!(dt>.005f)||!readPelvisNow(w,pv))continue;
            const Vec3 vel=(pv-x.kvPrev)*(1.f/(dt*u));const Vec3 c{x.rowC.x,0.f,x.rowC.z},a{x.rowA.x,0.f,x.rowA.z};const float cl=c.length(),al=a.length();
            if(cl>1e-3f&&al>1e-3f){x.kvFwd[k]=M::dot(vel,c*(1.f/cl));x.kvSide[k]=M::dot(vel,a*(1.f/al));}
            x.kvPrev=pv;x.kvPrevAge=age;}}
    std::uintptr_t m{};std::uint32_t rag{};const bool ragRead=get(w.actor,off::actorMover,m)&&m&&get(m,0xb8c,rag);
    // the pelvis's velocity over the last 2 ticks before the mover went ragdoll after the knockdown message (row c / row a at t0)
    if(x.knockAge>=0&&!x.preDone&&age>=std::uint64_t(x.knockAge)){
        if((ragRead&&rag)||age>std::uint64_t(x.knockAge)+300'000'000ull)x.preDone=true;
        else{Vec3 pv{};if(readPelvisNow(w,pv)){const float u=x.upm>1e-3f?x.upm:1.f;
            if(x.havePrePelvis&&age>x.prePelvisAge){const float dt=float(double(age-x.prePelvisAge)*1e-9);
                const Vec3 c{x.rowC.x,0.f,x.rowC.z},a{x.rowA.x,0.f,x.rowA.z};const float cl=c.length(),al=a.length();
                if(dt>.002f&&cl>1e-3f&&al>1e-3f){const Vec3 vel=(pv-x.prePelvis)*(1.f/(dt*u));x.preFwd[0]=x.preFwd[1];x.preSide[0]=x.preSide[1];
                    x.preFwd[1]=M::dot(vel,c*(1.f/cl));x.preSide[1]=M::dot(vel,a*(1.f/al));}}
            x.prePelvis=pv;x.prePelvisAge=age;x.havePrePelvis=true;}}}
    if(ragRead&&rag&&x.ragdollMs<0)x.ragdollMs=std::int32_t(std::min<std::uint64_t>(age/1'000'000ull,1'000'000ull));
    if(x.ragdoll150<0&&age>=150'000'000ull&&ragRead)x.ragdoll150=std::int32_t(rag!=0);
    if(!x.haveBones[1]&&age>=300'000'000ull&&x.globals){refreshFallChain(w);readFallBones(x,1);}
    const std::uint64_t kickAge=::rdrvr::vr::handphys::kickAgeNs(age,x.kickBase);
    const bool knockGraded=x.graded&&(x.decision==::rdrvr::vr::handphys::PushKnockdown||x.knockAge>=0);
    if(knockGraded)knockTraceTick(w,age,ragRead,rag);
    if(::rdrvr::vr::handphys::pushKickFires(x.kick,x.kicked,kickAge,ragRead,rag)){x.kicked=true;
        x.kickState=kickNow(w,x.graded?x.kickJ*x.upm:x.kickDir*x.kickMag);
        if(x.kickState==KickSent||x.kickState==KickLost)x.kickMs=std::int32_t(age/1'000'000ull);
        if(x.graded&&x.kickState==KickSent){x.kick1Age=age;Vec3 pv{};if(readPelvisNow(w,pv)){x.kickPelvis=pv;x.kickPelvisAge=age;x.haveKickPelvis=true;}
            if(x.servoLever){x.servoWhy=0;x.servoSpent=x.kickJ.length();bump(stats.servoArmed);}}}
    if(x.graded&&x.servoLever&&x.kickState==KickSent&&x.servoWhy==0)servoTick(w,age,ragRead,rag);
    if(knockGraded&&x.trackOk<0&&x.knockAge>=0&&age>=std::uint64_t(x.knockAge))readTrack(w);
    // the second kick of a knockdown (lever pushkick2): 100 ms after the first, while ragdoll, when the pelvis does not move along the push at
    // 0.6 of the target speed or slides sideways over 0.4 m/s (hp::kick2Wanted); at most 100 N s; no attacker guid travels with it
    if(x.graded&&x.kickState==KickSent&&!x.kick2Done&&x.haveKickPelvis&&age>=x.kickPelvisAge+::rdrvr::vr::handphys::kick2DelayNs){
        namespace hp=::rdrvr::vr::handphys;x.kick2Done=true;Vec3 pv{};
        const float u=x.upm>1e-3f?x.upm:1.f;const float dt=float(double(age-x.kickPelvisAge)*1e-9);
        if(!readPelvisNow(w,pv)||!(dt>.01f)){x.kick2State=KickNoComp;bump(stats.kick2Failed);}
        else{const Vec3 vm=(pv-x.kickPelvis)*(1.f/(dt*u));const hp::V3 vmh{vm.x,0.f,vm.z};const hp::V3 d=hp::v::normalized(hp::V3{x.dir.x,0.f,x.dir.z});
            const float along=hp::v::dot(vmh,d);x.vmAlong=along;x.vmSide=hp::v::length(hp::v::sub(vmh,hp::v::mul(d,along)));
            const float jAlong=hp::v::dot(hp::V3{x.kickJ.x,0.f,x.kickJ.z},d),dv=along-x.v0Dir;
            if(std::fabs(dv)>.05f&&std::isfinite(jAlong))x.effMass=jAlong/dv;
            if(!x.kick2Lever||x.servoLever)x.kick2State=KickOff; // the servo replaces kick 2
            else if(!(ragRead&&rag)){x.kick2State=KickNoRagdoll;bump(stats.kick2Failed);}
            else if(!hp::kick2Wanted(vmh,d,x.vDes)){x.kick2State=KickOff;bump(stats.kick2NotWanted);}
            else{const hp::V3 J=hp::kickImpulse(x.massKg,x.vDes,d,vmh,hp::kick2CapNs);x.kick2J={J[0],J[1],J[2]};
                const int comp=x.kickComp,bone=x.kickBone;const Vec3 at=x.kickAt;const std::int8_t fr=x.kickFree; // kick 1's record stays
                x.kick2State=kickNow(w,x.kick2J*u);x.kick2Ms=std::int32_t(age/1'000'000ull);
                x.kickComp=comp;x.kickBone=bone;x.kickAt=at;x.kickFree=fr;
                bump(x.kick2State==KickSent?stats.kick2Sent:stats.kick2Failed);}}}
    if(x.kick&&!x.kicked&&kickAge>::rdrvr::vr::handphys::pushKickWindowNs){x.kick=false;x.kickState=KickNoRagdoll;}
}
// The fall samples' skeleton: the chain derived again (the globals buffer may move while the creature is ragdoll); a new buffer with the
// push's bone count replaces the push's (globalsMoved), else the push's stays.
inline void Engine::refreshFallChain(NodeWatch& w) noexcept {
    auto& x=w.push;Chain c{};
    if(!deriveChain(w.actor,c)||!c.globals||c.bones!=x.boneCount)return;
    if(c.globals!=x.globals){x.globals=c.globals;x.globalsMoved=true;}
}
// The kick (the mover's ragdoll word just went 0 -> 1): the chain derived again with the push's frag instance (the handler dereferences
// [[actor +0x50] +0x88] with no check), the first torso bone with a frag child and its world position now (skeleton globals, within 4 m of
// the mover's start), the 8 pending-impulse slot words read before (all in use: no call) and after the call (a slot taken: sent).
// The pelvis of a watched push now (the chain derived again: the globals buffer may move while ragdoll), within 4 m of the mover's start.
inline bool Engine::readPelvisNow(NodeWatch& w,Vec3& out) noexcept {
    auto& x=w.push;if(!x.globals||x.pelvisBone<0)return false;
    refreshFallChain(w);if(unsigned(x.pelvisBone)>=x.boneCount)return false;
    float raw[3]{};diagnostics::ReadSegment sg{x.globals+std::uintptr_t(x.pelvisBone)*off::matrixStride+off::matrixPos,raw,12};
    if(!vread(&sg,1))return false;
    const Vec3 p{raw[0],raw[1],raw[2]};if(!M::finite(p)||(x.haveStart&&(p-x.start).length()>4.f))return false;
    out=p;return true;}
inline KickState Engine::kickNow(NodeWatch& w,Vec3 impulse) noexcept {
    namespace hp=::rdrvr::vr::handphys;
    auto& x=w.push;Chain c{};
    if(!deriveChain(w.actor,c)||!c.inst||c.inst!=x.inst)return KickInst;
    int comp=-1,bone=-1;
    for(const int b:x.torsoBones)if(b>=0&&unsigned(b)<c.bones){comp=componentForBone(c,b);if(comp>=0){bone=b;break;}}
    if(comp<0)return KickNoComp;
    float raw[3]{};diagnostics::ReadSegment bs{c.globals+std::uintptr_t(bone)*off::matrixStride+off::matrixPos,raw,12};
    if(!vread(&bs,1))return KickNoComp;
    const Vec3 at{raw[0],raw[1],raw[2]};
    if(!M::finite(at)||(x.haveStart&&(at-x.start).length()>4.f))return KickNoComp;
    x.kickComp=comp;x.kickBone=bone;x.kickAt=at;
    std::array<std::uint32_t,hp::kickSlots> before{},after{};std::array<diagnostics::ReadSegment,hp::kickSlots> sg{};
    const auto slotWords=[&](std::array<std::uint32_t,hp::kickSlots>& out){
        for(unsigned i=0;i<hp::kickSlots;++i)sg[i]={c.inst+hp::kickMgr+std::uintptr_t(i)*hp::kickSlotStride+hp::kickSlotWord,&out[i],4};
        return vread(sg.data(),hp::kickSlots);};
    if(!slotWords(before))return KickInst;
    const unsigned freeN=hp::kickFreeSlots(before.data());x.kickFree=std::int8_t(freeN);
    if(!freeN)return KickFull;
    alignas(16) unsigned char im[M::impulse::size];
    if(!M::finite(impulse)||!M::impulse::build(im,impulse,at,comp))return KickBuild;
    try{api_.impulse(reinterpret_cast<void*>(w.actor),im);}catch(...){bump(stats.exceptions);return KickBuild;}
    bump(stats.pushKicks);
    if(!slotWords(after)){x.kickSlot=-1;return KickLost;}
    x.kickSlot=std::int8_t(hp::kickQueuedSlot(before.data(),after.data()));x.consumedAge=-1;x.slotSawNeg=false;x.kickQueuedActive=true;
    return x.kickSlot>=0?KickSent:KickLost;
}
// KNOCK SERVO / TRACE: the centre of mass now (hp::comProxy over the bones kept at the push; the chain derived again), within 4 m of the
// mover's start.
inline bool Engine::readComNow(NodeWatch& w,Vec3& out,Vec3* pelvis) noexcept {
    namespace hp=::rdrvr::vr::handphys;auto& x=w.push;if(!x.globals||x.comBone[0]<0)return false;
    refreshFallChain(w);
    std::array<float,3*hp::comBones> raw{};std::array<diagnostics::ReadSegment,hp::comBones> sg{};std::array<unsigned,hp::comBones> idx{};unsigned n=0;
    for(unsigned i=0;i<hp::comBones;++i)if(x.comBone[i]>=0&&unsigned(x.comBone[i])<x.boneCount){
        sg[n]={x.globals+std::uintptr_t(x.comBone[i])*off::matrixStride+off::matrixPos,&raw[3*i],12};idx[n]=i;++n;}
    if(!n||!vread(sg.data(),n))return false;
    std::array<hp::V3,hp::comBones> pts{};bool have[hp::comBones]{};
    for(unsigned k=0;k<n;++k){const unsigned i=idx[k];pts[i]={raw[3*i],raw[3*i+1],raw[3*i+2]};
        const Vec3 p{pts[i][0],pts[i][1],pts[i][2]};have[i]=M::finite(p)&&!(x.haveStart&&(p-x.start).length()>4.f);}
    const hp::ComRead c=hp::comProxy(pts.data(),have);if(!c.ok)return false;
    out={c.p[0],c.p[1],c.p[2]};if(pelvis)*pelvis={pts[0][0],pts[0][1],pts[0][2]};return true;}
// The word +0x28 of the last kick's pending-impulse slot (the frag instance checked against the push's).
inline bool Engine::readKickSlot(NodeWatch& w,std::int32_t& out) noexcept {
    namespace hp=::rdrvr::vr::handphys;auto& x=w.push;if(x.kickSlot<0||!x.inst)return false;
    std::uint32_t v{};diagnostics::ReadSegment sg{x.inst+hp::kickMgr+std::uintptr_t(x.kickSlot)*hp::kickSlotStride+hp::kickSlotWord,&v,4};
    if(!vread(&sg,1))return false;out=std::int32_t(v);return true;}
// KNOCK TRACE + the servo's centre-of-mass velocity, every tick of a graded knockdown from the watch's first tick to 300 ms after the
// message (or kick 1, whichever is later).
inline void Engine::knockTraceTick(NodeWatch& w,std::uint64_t age,bool ragRead,std::uint32_t rag) noexcept {
    namespace hp=::rdrvr::vr::handphys;auto& x=w.push;
    const std::uint64_t base=std::max<std::uint64_t>(x.knockAge>=0?std::uint64_t(x.knockAge):0u,x.kick1Age);
    if(age>base+hp::servoWindowNs+50'000'000ull)return;
    const float u=x.upm>1e-3f?x.upm:1.f;const Vec3 c{x.rowC.x,0.f,x.rowC.z},a{x.rowA.x,0.f,x.rowA.z};const float cl=c.length(),al=a.length();
    const auto cm=[&](const Vec3& p,std::int16_t& f,std::int16_t& sd){if(!x.haveStart||cl<=1e-3f||al<=1e-3f)return;const Vec3 d{p.x-x.start.x,0.f,p.z-x.start.z};
        f=std::int16_t(std::clamp(M::dot(d,c*(1.f/cl))/u*100.f,-9000.f,9000.f));sd=std::int16_t(std::clamp(M::dot(d,a*(1.f/al))/u*100.f,-9000.f,9000.f));};
    Vec3 com{},pel{};const bool haveCom=readComNow(w,com,&pel);
    if(haveCom){if(x.haveComPrev&&age>x.comPrevAge){const float dt=float(double(age-x.comPrevAge)*1e-9);
            if(dt>.005f){x.comVel=(com-x.comPrev)*(1.f/(dt*u));x.comVelFrom=x.comPrevAge;x.haveComVel=M::finite(x.comVel);}}
        x.comPrev=com;x.comPrevAge=age;x.haveComPrev=true;}
    std::int32_t word{};const bool haveWord=readKickSlot(w,word);
    if(haveWord&&x.consumedAge<0){if(word<0)x.slotSawNeg=true;
        const auto oc=hp::kickSlotOutcome(word,x.slotSawNeg,x.kickQueuedActive);
        if(oc==hp::SlotApplied){x.consumedAge=std::int64_t(age);if(x.servoN==0&&x.kick1Consumed<0)x.kick1Consumed=std::int64_t(age);}
        else if(oc==hp::SlotFreed){x.kickSlot=-1;
            // an early kick 1 counted out while the object stayed inactive: nothing applied; queued again at the first ragdoll tick within
            // the kick window, the servo disarmed until then (its budget given back)
            if(x.servoN==0&&x.early==1){bump(stats.earlyFreed);x.early=2;x.kicked=false;x.kickState=KickArmed;x.kickMs=-1;x.kick1Age=0;x.haveKickPelvis=false;
                x.servoWhy=0xff;x.servoSpent=0.f;}}}
    const hp::TraceRowPlan rp=hp::traceRowPlan(x.traceN,unsigned(x.trace.size()),x.knockAge>=0&&age>=std::uint64_t(x.knockAge));
    if(rp.index>=0){if(rp.shift)for(unsigned k=0;k<unsigned(rp.index);++k)x.trace[k]=x.trace[k+1u];
        x.traceN=std::uint8_t(rp.n);auto& r=x.trace[unsigned(rp.index)];r=PushInfo::TraceRow{};r.ms=std::int16_t(std::min<std::uint64_t>(age/1'000'000ull,32000u));
        r.rag=ragRead?std::int8_t(rag!=0):std::int8_t(-1);if(haveWord)r.slot=word;
        if(haveCom){cm(pel,r.pelF,r.pelS);cm(com,r.comF,r.comS);}}}
// KNOCK SERVO (after kick 1): once the last kick's slot read consumed and a whole tick of centre-of-mass velocity was measured after it,
// one step per tick (hp::servoStep: no kick while the body moves along the push, else the kick law under the caps) while the mover is
// ragdoll, until 300 ms after kick 1 (ended reached when the last step found it moving along the push, else timeout), the budget or 4 kicks.
inline void Engine::servoTick(NodeWatch& w,std::uint64_t age,bool ragRead,std::uint32_t rag) noexcept {
    namespace hp=::rdrvr::vr::handphys;auto& x=w.push;
    if(x.servoWhy!=hp::ServoRunning)return;
    const auto end=[&](hp::ServoWhy why){x.servoWhy=std::uint8_t(why);bump(stats.servoEnds[why<8?why:7]);};
    if(age>x.kick1Age+hp::servoWindowNs){end(x.servoGoal?hp::ServoReached:hp::ServoTimeout);return;}
    if(x.consumedAge<0||!x.haveComVel||x.comVelFrom<std::uint64_t(x.consumedAge)||!(ragRead&&rag))return;
    const hp::V3 vm{x.comVel.x,0.f,x.comVel.z};const hp::V3 d=hp::v::normalized(hp::V3{x.dir.x,0.f,x.dir.z});
    const hp::ServoStep st=hp::servoStep(x.massKg,x.vDes,d,vm,x.servoSpent,x.servoN);
    x.servoGoal=st.why==hp::ServoReached;if(x.servoGoal)return; // along the push now: no kick, watched to the window's end
    if(st.why!=hp::ServoRunning){end(st.why);return;}
    const unsigned k=x.servoN;const int comp=x.kickComp,bone=x.kickBone;const Vec3 at=x.kickAt;const std::int8_t fr=x.kickFree; // kick 1's record stays
    const Vec3 J{st.j[0],st.j[1],st.j[2]};const KickState ks=kickNow(w,J*x.upm);
    x.kickComp=comp;x.kickBone=bone;x.kickAt=at;x.kickFree=fr;
    x.servoJ[k]=J;x.servoMs[k]=std::int16_t(std::min<std::uint64_t>(age/1'000'000ull,32000u));x.servoState[k]=std::uint8_t(ks);x.servoN=std::uint8_t(k+1u);
    if(x.traceN>0&&hp::traceRowIsTick(x.trace[x.traceN-1u].ms,age)){auto& r=x.trace[x.traceN-1u];const Vec3 c{x.rowC.x,0.f,x.rowC.z},a{x.rowA.x,0.f,x.rowA.z};const float cl=c.length(),al=a.length();
        if(cl>1e-3f&&al>1e-3f){r.jF=std::int16_t(M::dot(J,c*(1.f/cl)));r.jS=std::int16_t(M::dot(J,a*(1.f/al)));}}
    if(ks==KickSent){x.servoSpent+=J.length();bump(stats.servoKicks);}else{bump(stats.servoLost);end(hp::ServoSendFailed);}}
// The landed staggerFall's track, once: task +0x28 -> +0x14..+0x87 (NMStaggerFallTask::Begin reads its parameters there).
inline void Engine::readTrack(NodeWatch& w) noexcept {
    auto& x=w.push;if(x.trackOk>=0)return;
    std::uintptr_t task{};for(unsigned k=0;k<w.n&&k<w.tr.size();++k)if(w.tr[k].sfTask){task=w.tr[k].sfTask;break;}
    if(!task)return;
    std::uintptr_t track{};if(!get(task,0x28,track)||!track){x.trackOk=0;return;}
    diagnostics::ReadSegment sg{track+0x14,x.track.data(),sizeof x.track};x.trackOk=vread(&sg,1)?1:0;if(x.trackOk==1)bump(stats.trackReads);}
// The end of a watched push: the creature's horizontal move over the watch, its share along the push (fallDot), the bones' fall, its hp;
// a kick after which the creature lost hp locks the kick off for it.
inline void Engine::finishPush(NodeWatch& w) noexcept {
    auto& x=w.push;std::uintptr_t m{};float pos[3]{};
    if(x.haveStart&&get(w.actor,off::actorMover,m)&&m&&block(m+0x70,pos,sizeof pos)){
        const Vec3 d{pos[0]-x.start.x,0.f,pos[2]-x.start.z};const float l=d.length();x.movedCm=l*100.f;
        const Vec3 hd{x.dir.x,0.f,x.dir.z};const float hl=hd.length();
        if(l>.02f&&hl>1e-3f&&std::isfinite(l))x.fallDot=M::dot(d*(1.f/l),hd*(1.f/hl));}
    if(x.globals&&!x.haveBones[2]){refreshFallChain(w);readFallBones(x,2);}
    {const Vec3 hd{x.dir.x,0.f,x.dir.z};const float hl=hd.length();const Vec3 dirH=hl>1e-3f?hd*(1.f/hl):Vec3{};
     const auto fall=[&](unsigned k,float& dot,float& cm){if(!x.haveBones[0]||!x.haveBones[k]||hl<=1e-3f)return;
        const Vec3 d{x.pelvis[k].x-x.pelvis[0].x,0.f,x.pelvis[k].z-x.pelvis[0].z};const float l=d.length();cm=l*100.f;if(l>.02f&&std::isfinite(l))dot=M::dot(d*(1.f/l),dirH);};
     fall(1,x.fall300,x.fallCm300);fall(2,x.fall600,x.fallCm600);
     // the pelvis's move in the creature's frame at t0 (cm along row c / row a)
     {const Vec3 c{x.rowC.x,0.f,x.rowC.z},a{x.rowA.x,0.f,x.rowA.z};const float cl=c.length(),al=a.length(),u=x.upm>1e-3f?x.upm:1.f;
      const auto vf=[&](unsigned k,float& fwd,float& side){if(!x.haveBones[0]||!x.haveBones[k]||cl<=1e-3f||al<=1e-3f)return;
        const Vec3 d{x.pelvis[k].x-x.pelvis[0].x,0.f,x.pelvis[k].z-x.pelvis[0].z};fwd=M::dot(d,c*(1.f/cl))/u*100.f;side=M::dot(d,a*(1.f/al))/u*100.f;};
      vf(1,x.fwdCm300,x.sideCm300);vf(2,x.fwdCm600,x.sideCm600);}
     if(x.haveBones[2]){const Vec3 hp{x.head[2].x-x.pelvis[2].x,0.f,x.head[2].z-x.pelvis[2].z};const float l=hp.length();
        if(l>.05f&&std::isfinite(l)){if(hl>1e-3f)x.headFall600=M::dot(hp*(1.f/l),dirH);
            const Vec3 c{x.rowC.x,0.f,x.rowC.z};const float cl=c.length();if(cl>1e-3f)x.headFwd600=M::dot(hp*(1.f/l),c*(1.f/cl));}}}
    if(x.graded)if(const PushRec* rec=pushRecFor(w.guid,w.t0)){const float u=rec->upm>1e-3f?rec->upm:1.f;
        x.appliedCm=rec->appliedW/u*100.f;if(rec->askedW>0.f)x.askedCm=rec->askedW/u*100.f;x.jobRefused=rec->refused;
        x.refusedAtTouch=rec->refusedAtTouch;x.bumpAsked=rec->bumpAsked;x.waitMs=rec->waitMs;x.jobMerged=rec->jobMerged;x.peakA=std::max(x.peakA,rec->peakA);
        x.frozenMsg=x.frozenMsg||rec->frozenMsg;for(unsigned k=0;k<2;++k)x.aHand[k]=(rec->ph.hands>>k&1u)?rec->ph.a[k]:-1.f;}
    // a knockdown's fall by class x node x outcome (the pelvis at 600 ms along the push)
    if(x.graded&&x.decision==::rdrvr::vr::handphys::PushKnockdown){
        // the first node change at or after the knockdown message (an UP message's animation before it is no part of the fall)
        const unsigned kms=x.knockAge>=0?unsigned(std::min<std::int64_t>(x.knockAge/1'000'000,0xffff)):0u;
        const int fa=::rdrvr::vr::handphys::knockFirstAfter(w.tr,w.n,x.knockN,kms);const unsigned first=fa>=0?unsigned(fa):unsigned(w.n);
        unsigned node6=5;bool underTough=false;
        if(first<w.n){const auto& t=w.tr[first];const auto id=t.id;
            underTough=t.parents[0]==0x8b669f52u||t.parents[1]==0x8b669f52u||t.parents[2]==0x8b669f52u||id==0x8b669f52u;
            node6=id==0x02d1adb2u?0u:id==0x9974d115u?1u:id==0xbc3dfb68u?2u:id==0x731220e8u?3u:
                  (t.parents[0]==0xad400c7au||t.parents[1]==0xad400c7au||t.parents[2]==0xad400c7au||id==0xad400c7au)?4u:underTough?6u:5u;
            bump(stats.toughLanded[unsigned(std::clamp<int>(x.tough,0,2))*2u+(underTough?0u:1u)]);}
        const unsigned node=node6<3?node6:3u;
        const bool moved=x.fallCm600>=20.f;const unsigned out=!moved?3u:x.fall600>=.5f?0u:x.fall600<=-.5f?1u:2u;
        if(x.pushClass<4)bump(stats.fallBy[x.pushClass*16u+node*4u+out]);
        if(x.knockRow<4)bump(stats.fallBy2[x.knockRow*28u+node6*4u+out]);
        {const unsigned cls=x.knockClass<4?x.knockClass:x.pushClass;if(x.knockRow<4&&cls<4)bump(stats.fallBy3[x.knockRow*16u+cls*4u+out]);}
        // the route the message predicted (the mover's frozen count at it) x the first NM task after it
        if(x.knockFrozen()>=0){unsigned task=2;
            for(unsigned k=first;k<w.n&&k<w.tr.size()&&task==2;++k)for(const auto tv:w.tr[k].tasks){if(tv==0x2d43850u){task=0;break;}if(tv==0x2d42968u){task=1;break;}}
            bump(stats.routeMatrix[unsigned(x.knockFrozen())*3u+task]);}
        // KNOCK PICK: the fall against the message's push and against the kick; the landed behaviour against the pick; the ring row
        if(x.knockAge>=0){namespace hp=::rdrvr::vr::handphys;
            const auto fallAlong=[&](const Vec3& dir)->float{const Vec3 hd{dir.x,0.f,dir.z};const float hl=hd.length();
                if(!x.haveBones[0]||!x.haveBones[2]||hl<=1e-3f)return -2.f;const Vec3 d{x.pelvis[2].x-x.pelvis[0].x,0.f,x.pelvis[2].z-x.pelvis[0].z};const float l=d.length();
                return l>.02f&&std::isfinite(l)?M::dot(d*(1.f/l),hd*(1.f/hl)):-2.f;};
            x.fall600Msg=fallAlong(x.knockDir.length()>1e-3f?x.knockDir:x.dir);x.fall600Kick=fallAlong(x.kickJ.length()>1e-4f?x.kickJ:x.kickDir);
            const int lp=first<w.n&&first<w.tr.size()?hp::knockNodeParity(w.tr[first].id):-2;
            if(x.pickParity>=0)bump(stats.pickLanded[lp==-2?3u:lp<0?2u:lp==x.pickParity?0u:1u]);
            if(x.pickParity>=0&&x.tough)bump(stats.pickLandedTough[lp==-2?3u:lp<0?2u:lp==x.pickParity?0u:1u]);
            const unsigned beh=lp==1?0u:lp==0?1u:2u;const unsigned cls=x.knockClass<4?x.knockClass:x.pushClass<4?x.pushClass:4u;
            const unsigned outM=!moved?3u:x.fall600Msg>=.5f?0u:x.fall600Msg<=-.5f?1u:2u;
            if(cls<4)bump(stats.fallByPick[beh*16u+cls*4u+outM]);
            KnockRec k{};k.ns=w.t0;k.guid=w.guid;k.cls=std::uint8_t(cls);k.row=x.knockRow;k.why=x.pickWhy;k.parity=x.pickParity;k.landedParity=std::int8_t(lp);
            k.dirFwd=x.knockDirFwd;k.dirSide=x.knockDirSide;k.rollBefore=x.rollBefore;k.rollAfter=x.rollAfter;k.rollLanded=x.rollLanded;
            k.usedBefore=x.usedBefore;k.usedAfter=x.usedAfter;k.usedLanded=x.usedLanded;k.writes=x.pickWrites;k.ticks=x.pickTicks;
            if(first<w.n&&first<w.tr.size()){k.landedId=w.tr[first].id;k.landedTask=w.tr[first].tasks[0];k.landedMs=std::int32_t(w.tr[first].ms)-std::int32_t(kms);}
            {const Vec3 c{x.rowC.x,0.f,x.rowC.z},a{x.rowA.x,0.f,x.rowA.z};const float cl=c.length(),al=a.length();
             if(cl>1e-3f&&al>1e-3f){const Vec3 cn=c*(1.f/cl),an=a*(1.f/al);const float bl=x.bulletSent.length();
                if(bl>1e-4f){k.sFwd=M::dot(x.bulletSent,cn)/bl;k.sSide=M::dot(x.bulletSent,an)/bl;}
                k.jFwd=M::dot(x.kickJ,cn);k.jSide=M::dot(x.kickJ,an);}}
            k.comp=x.comp;std::memcpy(k.bone,x.bone,sizeof k.bone);k.preFwd=x.preFwd;k.preSide=x.preSide;k.kvFwd=x.kvFwd;k.kvSide=x.kvSide;
            k.fwdCm300=x.fwdCm300;k.sideCm300=x.sideCm300;k.fwdCm600=x.fwdCm600;k.sideCm600=x.sideCm600;k.fall600=x.fall600;k.fall600Msg=x.fall600Msg;
            k.fall600Kick=x.fall600Kick;k.cm600=x.fallCm600;k.face=x.knockFace;k.imp=x.knockImp;k.tough=x.tough;k.servo=std::int8_t(x.servoLever?1:0);
            {KnockTrace t{};t.ns=w.t0;t.guid=w.guid;t.cls=std::uint8_t(cls);t.servo=x.servoLever;t.early=x.earlyLever;t.earlySent=x.early;t.kickMs=x.kickMs;
             t.ragMs=x.ragdollMs;t.knockMs=x.knockAge>=0?std::int32_t(x.knockAge/1'000'000):-1;
             t.kick1LandMs=x.kick1Consumed>=0?std::int32_t(x.kick1Consumed/1'000'000):-1;t.servoN=x.servoN;t.servoWhy=x.servoWhy;t.servoSpent=x.servoSpent;
             const Vec3 c{x.rowC.x,0.f,x.rowC.z},a{x.rowA.x,0.f,x.rowA.z};const float cl=c.length(),al=a.length();
             if(cl>1e-3f&&al>1e-3f){const Vec3 cn=c*(1.f/cl),an=a*(1.f/al);t.j1F=M::dot(x.kickJ,cn);t.j1S=M::dot(x.kickJ,an);
                for(unsigned i=0;i<4;++i){t.sjF[i]=M::dot(x.servoJ[i],cn);t.sjS[i]=M::dot(x.servoJ[i],an);}}
             t.sMs=x.servoMs;t.sState=x.servoState;t.rows=x.trace;t.n=x.traceN;t.trackOk=x.trackOk;t.track=x.track;t.fall600Msg=x.fall600Msg;
             std::lock_guard tl(knockTraceMutex);knockTrace[knockTraceNext]=t;knockTraceNext=(knockTraceNext+1u)%unsigned(knockTrace.size());}
            if(x.haveBones[0]&&x.haveBones[2]){const float u=x.upm>1e-3f?x.upm:1.f;k.headDrop600=(x.head[0].y-x.head[2].y)/u*100.f;}
            std::lock_guard lock(knockRingMutex);knockRing[knockRingNext]=k;knockRingNext=(knockRingNext+1u)%unsigned(knockRing.size());}}
    std::uintptr_t h{};float hp{};if(get(w.actor,off::actorHealth,h)&&h&&get(h,off::healthValue,hp))x.hpAfter=hp;
    if(x.kicked&&x.kickState==KickSent&&x.hpBefore>0.f&&x.hpAfter>=0.f&&x.hpAfter<x.hpBefore-.5f){bump(stats.kickHpLoss);
        if(!kickLocked(w.guid)){kickLocked_[kickLockNext_]=w.guid;kickLockNext_=(kickLockNext_+1u)%unsigned(kickLocked_.size());}}
    if(x.tier!=0xff&&x.kickState<KickStateCount)bump(stats.kickBy[x.kickState]);
}
// KNOCK PICK: the roll may be written when the PICK prints and words passed (touchApi.pickOk) and the injury subtree read from a watched
// creature (#1d19797e) holds the four random children with the words the pick relies on (mode 0, roll % 2, NMShot 0 / staggerFall 1). Until
// that subtree was read (the first knockdown of a session reads it) the engine's coin stays.
inline KnockPickWhy Engine::pickReady() noexcept {
    namespace hp=::rdrvr::vr::handphys;
    if(!hp::touchApi.pickOk||!hp::touchApi.randNum||!hp::touchApi.randUsed||!hp::touchApi.randFunc)return PickNoApi;
    {std::lock_guard lock(treeMutex);
     if(pickTree_<0||injuryTreeN!=pickTreeN_){int found=-1;
        for(unsigned i=0;i<injuryTreeN&&i<injuryTree.size();++i){const auto& d=injuryTree[i];if(d.id!=0x1d19797eu)continue;
            unsigned seen=0,good=0;
            for(unsigned k=0;k<d.n&&k<d.ch.size();++k){const auto& c=d.ch[k];if(hp::knockNodeParity(c.id)<0)continue;++seen;
                for(unsigned j=0;j<c.conds&&j<c.c.size();++j){const auto& q=c.c[j];if(q.fn!=hp::conditionRandomMatch)continue;
                    if(hp::knockPickChildOk(c.id,q.fn,q.w[0],q.w[2],q.w[3]))++good;break;}}
            found=seen==4&&good==4?1:0;break;}
        pickTree_=found;pickTreeN_=injuryTreeN;stats.pickTree.store(found,std::memory_order_relaxed);}}
    return pickTree_<0?PickNoTree:pickTree_==0?PickTreeBad:PickForced;
}
// The shared roll (sm_RandomNum, sm_RandomUsed) read; parity 0 / 1: the roll's lowest bit set to it and Used cleared (the next random
// condition reuses it), only while sm_RandFunc is 0 (the engine's own draw). wrote = a word changed. Plain aligned stores after reads that
// succeeded (in-process statics of the library: the GOT words were checked at install).
inline bool Engine::pickRoll(int parity,std::int32_t& num,std::int8_t& used,bool& wrote) noexcept {
    namespace hp=::rdrvr::vr::handphys;wrote=false;num=-1;used=-1;
    const auto& t=hp::touchApi;if(!t.pickOk||!t.randNum||!t.randUsed||!t.randFunc)return false;
    std::int32_t n{};std::uint8_t u{};if(!get(t.randNum,0,n)||!get(t.randUsed,0,u))return false;
    num=n;used=std::int8_t(u?1:0);
    if(parity<0)return true;
    std::uintptr_t fn{1};if(!get(t.randFunc,0,fn)||(fn&&fn!=reinterpret_cast<std::uintptr_t>(&hp::pickFnDraw))){bump(stats.churnFuncSet);return false;}
    const std::int32_t want=hp::knockPickNum(n,parity);
    if(n!=want){*reinterpret_cast<volatile std::int32_t*>(t.randNum)=want;wrote=true;}
    if(u){*reinterpret_cast<volatile std::uint8_t*>(t.randUsed)=0;wrote=true;}
    return true;
}
// The roll's churn (diagnostics, 2 reads per tick while PICK is armed): changes of the roll, Used 0 -> 1 flips, the most changes in a second.
inline void Engine::churnTick(std::uint64_t now) noexcept {
    namespace hp=::rdrvr::vr::handphys;if(!hp::touchApi.pickOk)return;
    std::int32_t n{};std::int8_t u{};bool w{};if(!pickRoll(-1,n,u,w))return;
    bump(stats.churnTicks);
    if(churnNum_>=0&&n!=churnNum_){bump(stats.churnNum);++churnSecCount_;}
    if(churnUsed_==0&&u==1)bump(stats.churnUsed);
    churnNum_=n;churnUsed_=u;
    if(!churnSecNs_||now<churnSecNs_||now-churnSecNs_>=1'000'000'000ull){
        if(churnSecCount_>stats.churnMaxPerSec.load(std::memory_order_relaxed))stats.churnMaxPerSec.store(churnSecCount_,std::memory_order_relaxed);
        {std::uintptr_t fn{};stats.randFuncState.store(get(hp::touchApi.randFunc,0,fn)?(fn?1:0):-1,std::memory_order_relaxed);}
        churnSecNs_=now;churnSecCount_=0;}
}
// KNOCK TOUGH: the tough bit (health +0x150 bit 0x8000) of a knockdown's victim cleared with one 32-bit read-modify-write, as the engine's
// setters write that word (main thread); the slot keeps what to put back. False = no free slot, the health block unreadable or the bit
// already clear.
inline bool Engine::toughClear(const Candidate& v,std::uint64_t now,int& slot) noexcept {
    namespace hp=::rdrvr::vr::handphys;slot=-1;
    int freeSlot=-1;for(unsigned i=0;i<tough_.size();++i)if(!tough_[i].on){freeSlot=int(i);break;}
    if(freeSlot<0||!v.health||!v.actor)return false;
    std::uintptr_t h{},vt{};std::uint32_t word{};
    if(!get(v.actor,off::actorHealth,h)||h!=v.health||!get(h,0,vt)||vt!=api_.healthVtable||!get(h,off::healthFlags,word)||!(word&hp::toughBit))return false;
    *reinterpret_cast<volatile std::uint32_t*>(h+off::healthFlags)=word&~hp::toughBit;
    tough_[unsigned(freeSlot)]=ToughSlot{true,v.actor,h,v.guid,now?now:1};slot=freeSlot;return true;
}
// The bit back: only on the same actor in its slot with the same health block, and only while the bit is still clear (a script that set
// it again in between keeps its own write).
inline void Engine::toughRestore(unsigned i,bool late) noexcept {
    namespace hp=::rdrvr::vr::handphys;if(i>=tough_.size())return;auto& t=tough_[i];if(!t.on)return;t.on=false;
    std::uintptr_t a{},h{},vt{};std::uint32_t word{};
    const bool actorSame=slotActor(t.guid,a)&&a==t.actor;
    const bool healthSame=actorSame&&get(t.actor,off::actorHealth,h)&&h==t.health&&get(h,0,vt)&&vt==api_.healthVtable&&get(h,off::healthFlags,word);
    switch(hp::toughRestoreVerdict(actorSame,healthSame,word)){
        case hp::ToughRestored:*reinterpret_cast<volatile std::uint32_t*>(h+off::healthFlags)=word|hp::toughBit;bump(late?stats.toughRestoreLate:stats.toughRestored);break;
        case hp::ToughScriptChanged:bump(stats.toughScriptChanged);break;
        default:bump(stats.toughLost);break;}
}
// KNOCK PICK draw function: set only while sm_RandFunc is null (a net session sets its own), victim and parity published first.
inline void Engine::pickFnStart(std::uintptr_t actor,std::uint32_t guid,int parity,std::uint64_t now) noexcept {
    namespace hp=::rdrvr::vr::handphys;const auto& t=hp::touchApi;
    if(!t.pickFnOk||!t.randFunc||parity<0||!hp::pickFn.replay.load(std::memory_order_relaxed)){bump(stats.pickFnRefused);return;}
    const auto ours=reinterpret_cast<std::uintptr_t>(&hp::pickFnDraw);std::uintptr_t fn{1};
    if(!get(t.randFunc,0,fn)){bump(stats.pickFnRefused);return;}
    if(fn==ours){bump(stats.pickFnBusy);return;} // another knockdown's pick holds it (its tick write still covers this one)
    if(fn){bump(stats.pickFnRefused);return;}
    hp::pickFn.parity.store(parity,std::memory_order_relaxed);hp::pickFn.victim.store(actor,std::memory_order_release);
    *reinterpret_cast<volatile std::uintptr_t*>(t.randFunc)=ours;pickFnGuid_=guid;pickFnNs_=now?now:1;bump(stats.pickFnInstalled);
}
inline void Engine::pickFnEnd() noexcept {
    namespace hp=::rdrvr::vr::handphys;const auto& t=hp::touchApi;if(!pickFnGuid_)return;pickFnGuid_=0;pickFnNs_=0;
    const auto ours=reinterpret_cast<std::uintptr_t>(&hp::pickFnDraw);std::uintptr_t fn{};
    if(t.randFunc&&get(t.randFunc,0,fn)&&fn==ours){*reinterpret_cast<volatile std::uintptr_t*>(t.randFunc)=0;bump(stats.pickFnUninstalled);}
    else bump(stats.pickFnReplaced);
    hp::pickFn.victim.store(0,std::memory_order_release);hp::pickFn.parity.store(-1,std::memory_order_relaxed);
}
// KNOCK OFF: one handphys slot armed for a knockdown (the frag instance of the kick, the actor, the push direction); ms <= 0 keeps the pairs
// and only measures them (for 1500 ms). The slot's counters are reset here only. -1 = no free slot.
inline int Engine::knockOffArm(std::uintptr_t actor,std::uint32_t guid,std::uintptr_t inst,const Vec3& dir,float ms,std::uint64_t now) noexcept {
    namespace hp=::rdrvr::vr::handphys;if(!actor)return -1;
    int i=-1;for(unsigned k=0;k<koOwn_.size();++k)if(koOwn_[k].on&&koOwn_[k].actor==actor){i=int(k);break;} // the same creature again: its window renewed
    if(i<0)for(unsigned k=0;k<koOwn_.size();++k)if(!koOwn_[k].on){i=int(k);break;}
    if(i<0){bump(stats.knockOffFull);return -1;}
    const bool drop=std::isfinite(ms)&&ms>0.f;const std::uint64_t len=std::uint64_t(double(drop?std::min(ms,5000.f):1500.f)*1e6);
    auto& o=koOwn_[unsigned(i)];const bool renew=o.on;o=KnockOffOwn{true,actor,guid,now?now:1,now+len};
    auto& k=hp::knockOff[unsigned(i)];
    if(!renew){hp::knockOffSetUntil(unsigned(i),0);k.pairsEarly.store(0,std::memory_order_relaxed);k.pairsLate.store(0,std::memory_order_relaxed);
        k.towardMmsMax.store(0,std::memory_order_relaxed);k.armedNs.store(now?now:1,std::memory_order_relaxed);}
    k.inst.store(inst,std::memory_order_relaxed);k.actor.store(actor,std::memory_order_relaxed);k.drop.store(drop,std::memory_order_relaxed);
    k.dir[0].store(dir.x,std::memory_order_relaxed);k.dir[1].store(0.f,std::memory_order_relaxed);k.dir[2].store(dir.z,std::memory_order_relaxed);
    hp::knockOffSetUntil(unsigned(i),o.until);
    bump(drop?stats.knockOffArmed:stats.knockOffMeasure);return i;
}
// LATE FALL: the follow-up of a knockdown (its watch's pelvis and head at t0, the creature's frame at t0).
inline void Engine::followArm(const NodeWatch& w,int offSlot,std::uint64_t now) noexcept {
    namespace hp=::rdrvr::vr::handphys;(void)now;const auto& x=w.push;
    if(!x.haveBones[0]||x.pelvisBone<0||x.headBone<0){bump(stats.lateNoBones);return;}
    Follow* f=nullptr;for(auto& e:follow_)if(e.on&&e.guid==w.guid&&e.t0==w.t0){f=&e;break;}
    if(!f)for(auto& e:follow_)if(!e.on){f=&e;break;}
    if(!f){f=&follow_[0];for(auto& e:follow_)if(e.t0<f->t0)f=&e;bump(stats.lateLost);}
    *f=Follow{};f->on=true;f->actor=w.actor;f->guid=w.guid;f->t0=w.t0;f->pelvis0=x.pelvis[0];f->head0=x.head[0];f->rowA=x.rowA;f->rowC=x.rowC;
    f->start=x.start;f->haveStart=x.haveStart;f->upm=x.upm>1e-3f?x.upm:1.f;f->pelvisBone=x.pelvisBone;f->headBone=x.headBone;f->boneCount=x.boneCount;
    f->tough=x.tough;f->offSlot=offSlot;f->offArmed=offSlot>=0?hp::knockOff[unsigned(offSlot)&3u].armedNs.load(std::memory_order_relaxed):0;
}
// Every tick, whatever the gate: the tough bits past 250 ms put back, the draw function past 250 ms put back, the hands-off windows ended
// (the creature up again after 300 ms, or the time), the follow-ups sampled at 900 / 1200 / 1500 ms and written into their ring record.
inline void Engine::knockUpkeep(std::uint64_t now) noexcept {
    namespace hp=::rdrvr::vr::handphys;
    for(unsigned i=0;i<tough_.size();++i)if(tough_[i].on&&(now<tough_[i].ns||now-tough_[i].ns>=hp::toughLateNs))toughRestore(i,true);
    if(pickFnGuid_&&(now<pickFnNs_||now-pickFnNs_>=hp::toughLateNs))pickFnEnd();
    for(unsigned i=0;i<koOwn_.size();++i){auto& o=koOwn_[i];if(!o.on)continue;
        bool end=now>=o.until||now<o.ns;bool up=false;
        if(!end&&now-o.ns>=hp::knockOffUpNs){std::uintptr_t a{},m{};std::uint32_t rag{1};
            if(!slotActor(o.guid,a)||a!=o.actor)end=true;
            else if(get(o.actor,off::actorMover,m)&&m&&get(m,0xb8c,rag)&&rag==0){end=true;up=true;}}
        if(!end)continue;
        hp::knockOffSetUntil(i,0);o.on=false;bump(up?stats.knockOffEndedUp:stats.knockOffExpired);}
    for(auto& f:follow_){if(!f.on)continue;
        static constexpr std::uint64_t marks[3]{900'000'000ull,1'200'000'000ull,1'500'000'000ull};
        const std::uint64_t age=now>=f.t0?now-f.t0:0;
        for(unsigned k=0;k<3;++k){if(f.done&(1u<<k)||age<marks[k])continue;f.done=std::uint8_t(f.done|(1u<<k));
            std::uintptr_t a{};Chain c{};
            if(!slotActor(f.guid,a)||a!=f.actor||!deriveChain(f.actor,c)||!c.globals||c.bones!=f.boneCount||unsigned(f.pelvisBone)>=c.bones||unsigned(f.headBone)>=c.bones)continue;
            float raw[6]{};diagnostics::ReadSegment sg[2]{{c.globals+std::uintptr_t(f.pelvisBone)*off::matrixStride+off::matrixPos,&raw[0],12},
                {c.globals+std::uintptr_t(f.headBone)*off::matrixStride+off::matrixPos,&raw[3],12}};
            if(!vread(sg,2))continue;
            const Vec3 pv{raw[0],raw[1],raw[2]},hv{raw[3],raw[4],raw[5]};
            if(!M::finite(pv)||!M::finite(hv)||(f.haveStart&&((pv-f.start).length()>8.f*f.upm||(hv-f.start).length()>8.f*f.upm)))continue;
            const Vec3 cc{f.rowC.x,0.f,f.rowC.z},aa{f.rowA.x,0.f,f.rowA.z};const float cl=cc.length(),al=aa.length();
            const Vec3 d{pv.x-f.pelvis0.x,0.f,pv.z-f.pelvis0.z};
            if(cl>1e-3f&&al>1e-3f){f.fwd[k]=M::dot(d,cc*(1.f/cl))/f.upm*100.f;f.side[k]=M::dot(d,aa*(1.f/al))/f.upm*100.f;}
            if(k==2)f.headDrop1500=(f.head0.y-hv.y)/f.upm*100.f;}
        if(age<marks[2])continue;
        f.on=false;
        const std::int8_t fell=f.headDrop1500>-998.f?std::int8_t(f.headDrop1500>=60.f?1:0):std::int8_t(-1);
        if(fell<0)bump(stats.lateLost);else{bump(fell?stats.lateFell:stats.lateNotFell);bump(stats.lateFellByTough[unsigned(std::clamp<int>(f.tough,0,2))*2u+(fell?0u:1u)]);}
        bump(stats.lateFollowed);
        std::lock_guard lock(knockRingMutex);
        for(auto& k:knockRing)if(k.ns==f.t0&&k.guid==f.guid){k.fwdCmLate=f.fwd;k.sideCmLate=f.side;k.headDrop1500=f.headDrop1500;k.fell=fell;
            // KNOCK SERVO: the pelvis at 1500 ms against the push (both in the creature's frame at t0) by lever and push class
            if(k.servo>=0&&k.cls<4&&f.fwd[2]>-998.f&&std::isfinite(k.dirFwd)&&std::isfinite(k.dirSide)){
                const float l=std::sqrt(f.fwd[2]*f.fwd[2]+f.side[2]*f.side[2]),pl=std::sqrt(k.dirFwd*k.dirFwd+k.dirSide*k.dirSide);
                const float dot=l>=20.f&&pl>1e-3f?(f.fwd[2]*k.dirFwd+f.side[2]*k.dirSide)/(l*pl):0.f;
                const unsigned out=l<20.f?3u:dot>=.5f?0u:dot<=-.5f?1u:2u;bump(stats.lateByServo[unsigned(k.servo)*16u+unsigned(k.cls)*4u+out]);}
            if(f.offSlot>=0){const auto& ko=hp::knockOff[unsigned(f.offSlot)&3u];
                if(ko.armedNs.load(std::memory_order_relaxed)==f.offArmed){k.offEarly=ko.pairsEarly.load(std::memory_order_relaxed);k.offLate=ko.pairsLate.load(std::memory_order_relaxed);
                    k.offTowardMps=float(ko.towardMmsMax.load(std::memory_order_relaxed))/1000.f;k.offDrop=ko.drop.load(std::memory_order_relaxed)?1:0;}}
            break;}}
}
inline void Engine::tickWatches(std::uint64_t now) noexcept {
    churnTick(now);
    for(auto& w:watches_){if(!w.set)continue;
        std::uintptr_t a{};if(!slotActor(w.guid,a)||a!=w.actor){bump(stats.watchDropped);w=NodeWatch{};continue;}
        const auto age=now>=w.t0?now-w.t0:0;
        std::uintptr_t cn{};std::uint32_t cc{};
        if(nodeChange(w.actor,cn,cc)&&(cn!=w.lastNode||cc!=w.lastCounter)){ // a change: the full sample (names, parents, tasks)
            NodeSample x{};std::uintptr_t node{};
            if(w.n<w.tr.size()&&readNode(w.actor,x,node)){x.ms=std::uint16_t(std::min<std::uint64_t>(age/1'000'000ull,0xffffu));w.tr[w.n++]=x;
                if(node&&w.push.knockAge>=0&&age>=std::uint64_t(w.push.knockAge)&&w.kind==WatchPush)readInjuryTree(node);}
            w.lastNode=cn;w.lastCounter=cc;}
        // KNOCK PICK: the roll held at the chosen parity every tick until the first node change after the message, or 120 ms
        if(w.push.pickParity>=0&&!w.push.pickDone&&w.push.knockAge>=0&&age>=std::uint64_t(w.push.knockAge)){auto& x=w.push;
            const unsigned kms=unsigned(std::min<std::int64_t>(x.knockAge/1'000'000,0xffff));
            const bool landed=::rdrvr::vr::handphys::knockFirstAfter(w.tr,w.n,x.knockN,kms)>=0;
            std::int32_t n{};std::int8_t u{};bool wrote{};
            if(landed||age>=std::uint64_t(x.knockAge)+120'000'000ull){x.pickDone=true;if(pickRoll(-1,n,u,wrote)){x.rollLanded=n;x.usedLanded=u;}
                if(pickFnGuid_&&pickFnGuid_==w.guid)pickFnEnd();}
            else if(pickRoll(x.pickParity,n,u,wrote)){if(x.pickTicks<0xffff)++x.pickTicks;if(wrote){if(x.pickWrites<0xffff)++x.pickWrites;bump(stats.pickRewrites);}}
            else{x.pickDone=true;if(pickFnGuid_&&pickFnGuid_==w.guid)pickFnEnd();}}
        // KNOCK TOUGH: the bit back once the knockdown's node landed (or 120 ms after the message)
        if(w.push.toughSlot>=0&&!w.push.toughRestored&&w.push.knockAge>=0&&age>=std::uint64_t(w.push.knockAge)){auto& x=w.push;
            const unsigned kms=unsigned(std::min<std::int64_t>(x.knockAge/1'000'000,0xffff));
            const bool landed=::rdrvr::vr::handphys::knockFirstAfter(w.tr,w.n,x.knockN,kms)>=0;
            if(landed||age>=std::uint64_t(x.knockAge)+::rdrvr::vr::handphys::toughHoldNs){x.toughRestored=true;toughRestore(unsigned(x.toughSlot)&3u,false);}}
        if(w.push.tier!=0xff)watchPush(w,age);
        if(age<watchNs)continue;
        w.reacted=w.n>0;
        if(w.push.tier!=0xff)finishPush(w);
        bump((w.reacted?stats.watchReacted:stats.watchNotReacted)[w.kind][w.variant]);
        {std::lock_guard lock(watchMutex);watchRing[watchNext]=w;watchNext=(watchNext+1u)%unsigned(watchRing.size());}
        if((w.kind==WatchPush||w.kind==WatchSlap)&&w.push.tier!=0xff){std::lock_guard lock(pushRingMutex);pushRing[pushRingNext]=w;pushRingNext=(pushRingNext+1u)%unsigned(pushRing.size());}
        w=NodeWatch{};}
}
// A creature the player's own mover bumps (the walk-in bump the engine reacts to without hostility): its animator holds the player as
// the last bumper (+0xb04, kept) and the bump is current (+0xb08 = seconds since the last one, +0xb0c = the bump time; AfterActionTreeUpdate
// 0x1e1698c), while the hands sent no bump for 100 ms. Watched once per creature, at most one every 2 s.
inline void Engine::walkBumps(const Player& p,std::uint64_t now) noexcept {
    namespace hp=::rdrvr::vr::handphys;
    if(!hp::touchApi.nodeOk||!p.guid||(tickIndex_%6u)!=0)return;
    if(walkNs_&&now>=walkNs_&&now-walkNs_<2'000'000'000ull)return;
    const auto sent=hp::bumpSentNs.load(std::memory_order_relaxed);if(sent&&now>=sent&&now-sent<100'000'000ull)return;
    for(unsigned i=0;i<candCount_;++i){const auto& v=cand_[i];
        if(!v.anim||v.handOnly||!(v.distance<=1.5f))continue;
        alignas(4) unsigned char bb[12]{};if(!block(v.anim+0xb04,bb,sizeof bb))continue; // +0xb04 bumper, +0xb08 since, +0xb0c during
        const auto bumper=at<std::uint32_t>(bb,0);const float since=at<float>(bb,4),during=at<float>(bb,8);
        if(bumper!=p.guid||!(since<.05f)||!(during>0.f))continue;
        bool watched=false;for(const auto& w:watches_)watched=watched||(w.set&&w.guid==v.guid);
        if(watched)continue;
        if(startWatch(WatchWalk,WatchVariantNone,v.actor,v.guid,now)){walkNs_=now;bump(stats.walkBumps);}
        return;}
}
// The engine's own reaction node played on the creature as the scripts play one (SetNodeHelper: Find(path, null), SetActionNode): no
// injure message, no aggression. Refused (counted, one reason per guard) unless every guard holds: the creature component's class, no
// lock node and no Play in progress on its controller, no script tree / custom react node / tree handles (+0x5b0 / +0x5d8 / +0x5a0 /
// +0x5a8), not flagged (+0x915 bit 1, actor +0x118 bit 1), a human (+0xa30 0), alive, its mover neither ragdoll nor busy, 0.3 s in its
// current node, 1.5 s since our last play on it; the node found and in the creature's own tree (node +0x20 == the tree handle +0x598
// the actor holds, so the node outlives the play). Actor +0x112 bit 6 is the full-update state (sagActor::PreProcessPhase 0x1d1cd18..
// 0x1d1cd3c enters it while set; aiMindComponent::HandleBumpedInto 0x1e49f78 acts only while set): a creature near the player has it,
// so it is no guard.
inline NodeResult Engine::playNode(const Candidate& v,const char* path,std::uint64_t now) noexcept {
    namespace hp=::rdrvr::vr::handphys;
    if(!path||!path[0])return NodeNoPath;
    if(!hp::touchApi.findNode||!hp::touchApi.setNode||!hp::touchApi.creatureVptr)return NodeOff;
    std::uintptr_t comp{},vt{};if(!get(v.actor,off::actorComponent,comp)||!comp||!get(comp,0,vt)||vt!=hp::touchApi.creatureVptr)return NodeComp;
    const std::uintptr_t ac=comp+0xa8;std::uintptr_t lock{},h5a0{},h5a8{},h5b0{},h5d8{},tree{};std::uint8_t playing{1},f915{0xff};std::uint32_t f118{~0u};
    std::int32_t race{-1};float timeIn{-1.f};
    if(!get(ac,0x10,lock)||!get(ac,0xe0,playing)||!get(ac,0x18,timeIn)||!get(comp,0x5a0,h5a0)||!get(comp,0x5a8,h5a8)||!get(comp,0x5b0,h5b0)||
       !get(comp,0x5d8,h5d8)||!get(comp,0x598,tree)||!get(comp,0x915,f915)||!get(comp,off::compRace,race)||!get(v.actor,off::actorFlags,f118))return NodeState;
    if(lock||playing)return NodeLocked;
    if(h5a0||h5a8||h5b0||h5d8||!tree)return NodeScript;
    if(f915&0x2u)return NodeF915;
    if(f118&0x2u)return NodeF118;
    if(race!=0)return NodeRace;
    {std::uintptr_t m{};std::uint32_t rag{1};std::int16_t a{1},b{1};
     if(!get(v.actor,off::actorMover,m)||!m||!get(m,0xb8c,rag)||!get(m,0x846,a)||!get(m,0x848,b))return NodeMover;
     if(rag)return NodeRagdoll;if(a>0||b>0)return NodeBusy;}
    if(!std::isfinite(timeIn)||timeIn<.3f)return NodeBusy;
    for(const auto& c:nodeCool_)if(c.guid==v.guid&&now>=c.ns&&now-c.ns<1'500'000'000ull)return NodeRecent;
    using FindFn=void*(*)(const char*,void*);using SetFn=void(*)(void*,void*,bool);
    void* node=nullptr;
    try{node=reinterpret_cast<FindFn>(hp::touchApi.findNode)(path,nullptr);}catch(...){return NodeException;}
    if(!node)return NodeNotFound;
    std::uintptr_t nodeTree{};if(!get(reinterpret_cast<std::uintptr_t>(node),0x20,nodeTree)||nodeTree!=tree)return NodeOtherTree;
    std::uint32_t c0{};get(ac,0x1c,c0);
    try{reinterpret_cast<SetFn>(hp::touchApi.setNode)(reinterpret_cast<void*>(comp),node,true);}catch(...){return NodeException;}
    auto* slot=&nodeCool_[0];for(auto& c:nodeCool_){if(c.guid==v.guid){slot=&c;break;}if(c.ns<slot->ns)slot=&c;}
    *slot={v.guid,now};
    std::uint32_t c1{};get(ac,0x1c,c1);
    return c1!=c0?NodePlayed:NodeNoChange;
}
inline void Engine::consumeTouch(const Player& p,std::uint64_t now) noexcept {
    namespace hp=::rdrvr::vr::handphys;
    checkPushed();tickWatches(now);walkBumps(p,now);
    // both hands' new requests first, a thrown light prop's tap (its own slot: a slap of the throwing hand), then an UP message held for
    // the other hand that is due now (a knockdown among the new requests drops it before it is sent)
    for(unsigned slot=0;slot<3u+frozenHold_.size();++slot){
        const bool thrownSlot=slot==2,held=slot>=3;const unsigned hs=held?slot-3u:0u;unsigned h=held?hs&1u:slot&1u;FrozenHold fh{};
        hp::TouchRequest r{};
        if(thrownSlot){if(!hp::thrownTouchRequest.load(r)||!r.serial||r.serial==touchSerial_[2])continue;touchSerial_[2]=r.serial;bump(stats.touchThrown);
            if(r.kind!=hp::TouchSlap||!r.thrown)continue; // the thrown slot carries a slap only
            h=unsigned(r.hand)&1u;}
        else if(!held){if(!hp::touchRequest[h].load(r)||!r.serial||r.serial==touchSerial_[h])continue;touchSerial_[h]=r.serial;bump(stats.touchConsumed);}
        else{if(!hp::frozenHoldDueNow(frozenHold_[hs].s,now))continue;fh=frozenHold_[hs];frozenHold_[hs]=FrozenHold{};r=fh.r;}
        if(!hp::touchReady.load(std::memory_order_acquire)){bump(stats.touchNotReady);continue;}
        if(!held&&!hp::fresh(now,r.ns)){bump(stats.touchStale);continue;}
        int idx=-1;for(unsigned i=0;i<candCount_;++i)if(cand_[i].guid==r.guid&&cand_[i].actor==r.actor){idx=int(i);break;}
        if(idx<0){bump(stats.touchNotFound);continue;}
        Candidate& v=cand_[unsigned(idx)];
        if(v.handOnly){bump(stats.touchHandOnly);continue;}
        std::uintptr_t actor{};if(!slotActor(v.guid,actor)||actor!=v.actor){bump(stats.touchStale);continue;}
        alignas(8) unsigned char hb[off::healthBlock]{};
        if(!block(v.health,hb,sizeof hb)||at<std::uintptr_t>(hb,0)!=api_.healthVtable){bump(stats.touchStale);continue;}
        const float hpBefore=at<float>(hb,off::healthValue),koBefore=at<float>(hb,off::healthKo);
        const std::uint32_t hflags=at<std::uint32_t>(hb,off::healthFlags);
        if(!(hpBefore>0)||(hflags&off::healthDead)){bump(stats.touchStale);continue;}
        const bool invulnerable=(hflags&off::healthInvulnerable)!=0;if(invulnerable)bump(stats.touchInvulnerable);
        const float u=std::max(r.upm,1e-3f);
        const hp::V3 up{0,1,0};const hp::V3 into=hp::horizontal(r.n,up);const Vec3 dir{into[0],into[1],into[2]};
        const Vec3 point{r.point[0],r.point[1],r.point[2]};
        std::uintptr_t mover{},anim{};const bool canPush=r.push&&dir.length()>.5f&&pushTarget(v.actor,v.race,mover,anim);
        LastTouch lt{};lt.ns=now;lt.kind=r.kind;lt.hand=int(h);lt.victimGuid=v.guid;lt.hpBefore=hpBefore;lt.koBefore=koBefore;lt.a=r.a;
        if(r.kind==hp::TouchPress){
            bump(stats.touchPresses);
            PressDone* pd=nullptr;for(auto& x:pressDone_)if(x.guid==v.guid){pd=&x;break;}
            if(!pd){pd=&pressDone_[0];for(auto& x:pressDone_)if(x.ns<pd->ns)pd=&x;*pd={v.guid,0.f,now};}
            if(now<pd->ns||now-pd->ns>300'000'000ull)pd->doneM=0.f; // a new press
            pd->ns=now;
            // the depth law's step, at most the arm's own drive into the body (r.armStepM, lever npcpassive): a palm held still against
            // a body (the player walking into it, the creature walking into it) moves nothing
            const float depthStep=hp::pressStepM(r.depthM,r.pushGain,r.pushMaxM,pd->doneM,r.pushCapM);
            const float step=hp::pressArmStepM(depthStep,r.armStepM);
            if(r.armStepM>=0.f&&step<depthStep){bump(stats.pressArmLimited);if(!(step>0.f))bump(stats.pressArmZero);}
            if(canPush&&step>0.f){translate(mover,dir*(step*u));
                pd->doneM+=step;bump(stats.pushApplied);bump(stats.offsetUmSum,std::uint64_t(step*1e6f));
                {auto m=stats.maxPressUm.load(std::memory_order_relaxed);const auto x=std::uint64_t(pd->doneM*1e6f);while(x>m&&!stats.maxPressUm.compare_exchange_weak(m,x)){}}
                const float yaw=hp::pressYaw(r.point,r.pivot,into,up,u)*(depthStep>0.f?std::min(step/depthStep,1.f):0.f);
                if(anim&&yaw!=0.f){using RotateFn=void(*)(void*,float);try{reinterpret_cast<RotateFn>(hp::touchApi.rotate)(reinterpret_cast<void*>(anim),yaw);}catch(...){}bump(stats.yawTicks);}}
            lt.distCm=pd->doneM*100.f;{std::lock_guard lock(touchMutex);lastTouch=lt;}
            // the control group of the node watch: a press with no message, at most one every 2 s (a free slot only)
            if(!controlNs_||now<controlNs_||now-controlNs_>=2'000'000'000ull){controlNs_=now;startWatch(WatchPress,WatchVariantNone,v.actor,v.guid,now);}
            continue;}
        // SLAP / SHOVE (a push): the reaction field first (ComputeReactType's order), then the engine's reaction node of this kind (levers
        // slapnode / pushnode), else the injure message openTouchPlan allows: the creature itself (never a zombie: facFactionMgr::IsZombie),
        // a guid no consumer resolves (actor slot 0, its generation flipped) or none. Never the player, never damage (KO included): the
        // player's strike path and broadcast report aggression (StrikeDamageRemote, aiMindComponent::HandleInjure), so an open hand has
        // no path to them.
        const bool slap=r.kind==hp::TouchSlap,thrown=slap&&r.thrown!=0;bump(thrown?stats.touchThrownSent:slap?stats.touchSlaps:stats.touchShoves);
        const bool strong=r.strong!=0;
        // GRADED (lever pushgrade, an approach shove): one push per creature for pushRecNs; the other hand within it merges into the same
        // push (hp::pushMerge: aEff, the approach-weighted direction) and never sends a message of its own. Under pushdownmps (or no firm
        // edge) a stumble: the mover offset along the push (hp::stumbleDistanceM), no message, no reaction, no sound. At pushdownmps or more
        // a knockdown: SHOVED, no offset, no sound, the kick that sets the body's velocity along the push (hp::kickImpulse) against the
        // creature's own (NM staggerFall steps along the centre-of-mass velocity). A stumble the second hand lifts over pushdownmps becomes
        // a knockdown (its queued offset stops).
        // GRADED ESCALATION (lever pushescgrade): an escalated press is a stumble graded on min(a, armIn) (never a knockdown), so it never
        // moves the creature farther than a shove of the same force; on a creature with a live push it raises that push (rest = the larger
        // distance less what was applied) instead of adding one. A stroke peak (r.peak, lever pushpeak) raises the same hand's push.
        const bool escal=r.escalated!=0,peak=r.peak!=0;
        const bool graded=!slap&&r.pushGrade!=0&&(!escal||r.pushEscGrade!=0);
        if(peak&&!graded){bump(stats.peakNoRec);continue;}
        hp::PushSum gsum{};hp::PushDecision gdec=hp::PushStumble;PushRec* grec=nullptr;NodeWatch* gwatch=nullptr;bool upgraded=false;
        bool frozenMsg=false; // a graded stumble on a frozen mover: the UP reaction message in place of the offset
        bool frozenHeldNow=false; // that message held for the other hand (lever pushfrozenhold): nothing sent, no offset
        // hold the frozen mover's UP message for the pair window: the push's record, the deadline, in a free slot of this hand (both taken:
        // not held, the message goes out now; a held message is never replaced)
        const auto holdFrozen=[&](PushRec* rec)->bool{
            if(!rec||held||!(r.pushFrozenHoldMs>0.f))return false;
            const int fs=hp::frozenHoldFree(frozenHold_[h].s,frozenHold_[h+2u].s);if(fs<0){bump(stats.frozenHoldFull);return false;}
            auto& q=frozenHold_[h+2u*unsigned(fs)];
            if(!hp::frozenHoldArm(q.s,v.guid,rec->ns,now,r.pushFrozenHoldMs))return false;
            q.r=r;rec->frozenHeld=true;rec->frozenSeen=true;bump(stats.frozenHeld);return true;};
        const hp::V3 v0m{r.bodyVel[0]/u,0.f,r.bodyVel[2]/u};
        // a graded stumble's route on this creature's mover now (hp::stumbleRoute; the touch's pushTarget above counted the refusal once)
        const auto stumbleRouteNow=[&](int* whyOut)->hp::StumbleRoute{
            std::uintptr_t mv{},an{};int why=-1;const bool ok=pushTarget(v.actor,v.race,mv,an,&why,false);if(whyOut)*whyOut=why;
            std::int16_t b6{},b8{};std::uintptr_t m{};
            if(!ok&&why==PushWhyBusyCode&&!(get(v.actor,off::actorMover,m)&&m&&get(m,0x846,b6)&&get(m,0x848,b8))){b6=1;b8=0;}
            return hp::stumbleRoute(ok,why==PushWhyBusyCode,b6,b8,r.pushFrozenMsg!=0,r.pushBusyWaitMs);};
        // a graded stumble's offset (metres along d): queued for the push's record; a creature whose mover is suspended waits for it (lever
        // pushbusywait: the job keeps its place until the deadline) and gets one walk-in bump asked for it (the handphys tick sends the stock
        // mvrMsgBumpedInto: no damage, no aggression); a frozen mover takes the UP reaction message instead (the caller); the refusal at the
        // touch is kept on the record
        const auto queueStumble=[&](const Vec3& d,float distM,PushRec* rec){
            const hp::PushPlan pl=hp::pushPlan(distM);if(!(distM>0.f)||!pl.ticks)return;
            int why=-1;const hp::StumbleRoute rt=stumbleRouteNow(&why);
            if(rt==hp::StumbleQueue){queuePush(v.guid,v.actor,d,pl.perTickM*u,pl.ticks,0.f,0,rec?rec->ns:0);return;}
            const unsigned k=why==PushWhyBusyCode?0u:why==PushWhyRagdollCode?1u:why==PushWhyFlagsCode?2u:3u;
            if(rec&&rec->refusedAtTouch[k]<0xffffu)++rec->refusedAtTouch[k];
            if(rt==hp::StumbleMessage){bump(stats.frozenDrop);return;}
            if(rt==hp::StumbleWait){
                queuePush(v.guid,v.actor,d,pl.perTickM*u,pl.ticks,0.f,0,rec?rec->ns:0,now+std::uint64_t(double(r.pushBusyWaitMs)*1e6));bump(stats.stumbleWaits);
                if(rec&&!rec->bumpAsked){rec->bumpAsked=true;hp::BumpAsk q{};q.serial=++bumpAskSerial_;q.ns=now;q.actor=v.actor;q.guid=v.guid;hp::bumpAsk.store(q);bump(stats.bumpAsks);}}};
        if(graded){
            const hp::PushKick hk=hp::pushKickPlan({dir.x,dir.y,dir.z},r.vel,r.a,1.f,1.f);
            const hp::V3 hd=hp::v::length(hk.dir)>.5f?hk.dir:hp::V3{dir.x,0.f,dir.z};
            const bool firm=r.thrust!=0&&!escal; // a deliberate thrust (hp::pushThrust), whatever the edge's depth; an escalated press never
            const hp::PushInput pin=escal?hp::PushInEsc:peak?hp::PushInPeak:hp::PushInEdge;
            // an escalated press: the push of its own touch (this hand's shove of the same touch, pushEscRecNs) first, else a live push
            if(held){ // the UP message held for the other hand, due now: sent when its push is still a stumble on a frozen mover
                for(auto& x:pushRecs_)if(x.guid==v.guid&&x.ns==fh.s.recNs){grec=&x;break;}
                if(!grec||!grec->frozenHeld||grec->decision==hp::PushKnockdown){bump(stats.frozenHoldLost);continue;}
                grec->frozenHeld=false;gsum.aEff=grec->aEff;gsum.dir=grec->dir;gdec=hp::PushStumble;
                for(auto& w:watches_)if(w.set&&w.guid==v.guid&&w.t0==grec->watchT0&&w.kind==WatchPush){gwatch=&w;break;}
                if(stumbleRouteNow(nullptr)==hp::StumbleMessage){frozenMsg=true;grec->frozenMsg=true;grec->frozenSeen=true;bump(stats.frozenDeferredSent);bump(stats.frozenMsg);}
                else bump(stats.frozenHoldFreed);} // the mover came free: the stumble's offset (below)
            else{
            if(escal&&r.touchNs)for(auto& x:pushRecs_)if(x.guid==v.guid&&(x.hands>>h&1u)&&x.touchNs[h]==r.touchNs&&now>=x.ns&&now-x.ns<pushEscRecNs){
                grec=&x;bump(stats.escKeyed);break;}
            if(!grec)for(auto& x:pushRecs_)if(x.guid==v.guid&&now>=x.ns&&now-x.ns<pushRecNs){grec=&x;break;}
            if(peak&&!grec){bump(stats.peakNoRec);continue;}
            if(grec){
                const bool same=(grec->hands&(1u<<h))!=0;
                if(same&&pin==hp::PushInEdge){bump(stats.gradeRepeat);continue;}
                if(pin==hp::PushInEsc&&grec->decision==hp::PushKnockdown){bump(stats.escOnKnock);continue;} // down already: a press adds nothing
                grec->firm=grec->firm||firm;
                // the push regraded over its hands (hp::pushRegrade: a new hand or a peak with the pair rule; an escalation raises aEff to its
                // own a at most, its direction kept)
                gsum=hp::pushRegrade(grec->ph,grec->aEff,grec->dir,h,pin,r.a,hd);
                if(pin==hp::PushInEdge){grec->hands=std::uint8_t(grec->hands|(1u<<h));grec->touchNs[h]=r.touchNs;bump(stats.gradePairMerged);}
                else if(pin==hp::PushInPeak){grec->peakA=std::max(grec->peakA,r.a);bump(stats.peakRegrades);}
                else bump(stats.escMerged);
                grec->aEff=gsum.aEff;grec->dir=gsum.dir;
                gdec=hp::pushRedecide(pin,hp::PushDecision(grec->decision),gsum.aEff,grec->firm,r.pushDownMps);
                for(auto& w:watches_)if(w.set&&w.guid==v.guid&&w.t0==grec->watchT0&&w.kind==WatchPush){gwatch=&w;break;}
                if(gwatch){auto& x=gwatch->push;if(!same&&pin==hp::PushInEdge){x.pair=true;x.pairGapMs=r.pairGapMs>=0.f?r.pairGapMs:float(double(now-grec->ns)*1e-6);}
                    if(escal)x.escalated=true;if(peak)x.peakA=std::max(x.peakA,r.a);x.aEff=gsum.aEff;
                    x.dir={gsum.dir[0],0.f,gsum.dir[2]};x.dirFwd=M::dot(x.dir,v.rowC);x.dirSide=M::dot(x.dir,v.rowA);
                    x.pushClass=std::uint8_t(hp::pushClass(x.dirFwd,x.dirSide));x.v0Dir=hp::v::dot(grec->v0,gsum.dir);}
                if(grec->decision==hp::PushKnockdown){ // down already: a kick not sent yet takes the pair's push
                    if(gwatch&&!gwatch->push.kicked&&gwatch->push.massKg>0.f){auto& x=gwatch->push;x.vDes=hp::knockdownMps(gsum.aEff,r.pushDownMps);
                        const hp::V3 J=hp::kickImpulse(x.massKg,x.vDes,gsum.dir,{x.v0.x,0.f,x.v0.z},r.pushKickMaxNs);x.kickJ={J[0],J[1],J[2]};
                        x.kickMag=x.kickJ.length();if(x.kickMag>1e-4f)x.kickDir=x.kickJ*(1.f/x.kickMag);}
                    {std::lock_guard lock(touchMutex);lastTouch.ns=now;}
                    continue;}
                if(gdec==hp::PushStumble){
                    // a frozen mover: one UP reaction message per push (the message below), never an offset
                    if(stumbleRouteNow(nullptr)==hp::StumbleMessage){
                        if(grec->frozenMsg||grec->frozenHeld){bump(stats.frozenRepeat);{std::lock_guard lock(touchMutex);lastTouch.ns=now;}continue;}
                        if(holdFrozen(grec)){if(gwatch)gwatch->push.frozenHeld=true;{std::lock_guard lock(touchMutex);lastTouch.ns=now;}continue;}
                        frozenMsg=true;grec->frozenMsg=true;}
                    else{ // the rest of the stumble re-planned to the merged distance along the merged direction
                        const float rest=hp::stumbleRestM(gsum.aEff,grec->appliedW/u)*u;
                        queueStumble(Vec3{gsum.dir[0],0.f,gsum.dir[2]},rest/u,grec);
                        grec->askedW=std::max(grec->askedW,grec->appliedW+rest);if(gwatch)gwatch->push.askedCm=grec->askedW/u*100.f;
                        bump(stats.gradeReplanned);bump(stats.gradeStumbles);haptics::pulse(h,.35f,30'000,now,haptics::SourceMeleeHit);
                        continue;}}
                else{ // a knockdown now: the stumble's queued offset stops
                    for(auto& j:pushes_)if(j.guid==v.guid&&j.ticks){j.ticks=0;j.yawTicks=0;j.measure=true;}
                    // an UP message held for this push is dropped: the knockdown's message alone reaches the creature
                    if(grec->frozenHeld){grec->frozenHeld=false;for(auto& q:frozenHold_)if(hp::frozenHoldDrop(q.s,v.guid,grec->ns))q.r={};
                        bump(stats.frozenSuperseded);if(gwatch)gwatch->push.frozenSuperseded=true;}
                    grec->decision=hp::PushKnockdown;upgraded=true;bump(stats.gradeUpgrades);if(peak)bump(stats.peakUpgrades);}
            }else{
                if(escal)bump(stats.escGraded);
                grec=&pushRecs_[0];for(auto& x:pushRecs_)if(x.ns<grec->ns)grec=&x;
                *grec=PushRec{};grec->guid=v.guid;grec->ns=now;grec->hands=std::uint8_t(1u<<h);grec->a0=r.a;grec->d0=hd;grec->v0=v0m;grec->firm=firm;grec->upm=u;
                grec->touchNs[h]=r.touchNs;
                gsum=hp::pushRegrade(grec->ph,0.f,hd,h,hp::PushInEdge,r.a,hd);grec->aEff=gsum.aEff;grec->dir=gsum.dir;
                gdec=hp::pushRedecide(pin,hp::PushStumble,gsum.aEff,firm,r.pushDownMps);grec->decision=std::uint8_t(gdec);
                if(gdec==hp::PushStumble&&stumbleRouteNow(nullptr)==hp::StumbleMessage){if(holdFrozen(grec))frozenHeldNow=true;else{frozenMsg=true;grec->frozenMsg=true;}}}
            bump(gdec==hp::PushKnockdown?stats.gradeKnockdowns:stats.gradeStumbles);
            if(frozenMsg)bump(stats.frozenMsg);
            }
        }
        const bool knock=graded&&gdec==hp::PushKnockdown;
        // the creature's own velocity for the kick: the first hand's (a second hand's bodyVel holds the stumble offset already applied)
        const hp::V3 v0g=graded&&grec?grec->v0:v0m;
        // the push's direction: the graded push's (the palm's horizontal motion into the body, merged over a pair), else the contact normal
        const Vec3 pdir=graded&&hp::v::length(gsum.dir)>.5f?Vec3{gsum.dir[0],0.f,gsum.dir[2]}:dir;
        // the push's tier (hp::pushTier): light (an escalated press) and medium shoves move the creature only (medium: an UP reaction message
        // with lever pushmedium 1); a firm shove sends SHOVED (KNCKBK when strong). A slap keeps its reaction message.
        const unsigned tier=slap?unsigned(hp::TierFirm):r.escalated?unsigned(hp::TierLight):std::min<unsigned>(r.tier,hp::TierFirm);
        const bool wantMsg=graded?knock||frozenMsg:slap||tier==hp::TierFirm||(tier==hp::TierMedium&&r.pushMedium==1);
        if(!slap&&!held){bump(stats.shoveTier[tier]);if(wantMsg)bump(stats.shoveTierMsg[tier]);}
        // a knockdown along row a (lever pushside): the KNCKBK side reaction (hp::pushSidePlan)
        const hp::SidePlan sideP=knock?hp::pushSidePlan(r.pushSide,M::dot(pdir,v.rowC),M::dot(pdir,v.rowA),0,0.f):hp::SidePlan{};
        const int reaction=graded?(frozenMsg?hp::slapReaction(r.side):sideP.reaction>=0?sideP.reaction:hp::shoveReaction(0.f,false)):
            slap||tier!=hp::TierFirm?hp::slapReaction(r.side):hp::shoveReaction(r.side,strong);lt.reaction=wantMsg?reaction:-1;
        if(sideP.reaction>=0)bump(stats.sideReaction);
        lt.escalated=r.escalated!=0;lt.windowed=r.windowed!=0;
        if(wantMsg&&(r.slapReact||!slap)){std::uintptr_t vb{},vm{},vvt{};
            if(v.anim&&get(v.anim,off::animBlock,vb)&&vb&&get(vb,off::blockMelee,vm)&&vm&&get(vm,0,vvt)&&vvt==api_.aniMeleeVtable){
                *reinterpret_cast<std::int32_t*>(vm+off::meleeReaction)=reaction;bump(stats.touchReaction[unsigned(std::clamp(reaction,0,8))]);}}
        NodeWatch* watch=config.dry||thrown?nullptr:((upgraded||frozenMsg||held)&&gwatch?gwatch:startWatch(slap?WatchSlap:WatchPush,WatchVariantNone,v.actor,v.guid,now));
        if(watch&&watch->guid==v.guid){auto& w=watch->push;w.tier=std::uint8_t(tier);w.reaction=std::int8_t(wantMsg?reaction:-1);w.a=r.a;w.depthCm=r.depthM*100.f;w.arm=r.arm;
            w.dir=pdir;w.dirFwd=M::dot(pdir,v.rowC);w.dirSide=M::dot(pdir,v.rowA);w.rowA=v.rowA;w.rowC=v.rowC;w.slap=slap;w.strong=strong;
            {const Vec3 hf{p.forward.x,0.f,p.forward.z};const float l=hf.length();w.headDir=l>1e-3f?M::dot(hf*(1.f/l),pdir):-2.f;}
            if(graded){w.graded=true;w.decision=std::uint8_t(gdec);w.aEff=gsum.aEff;w.pair=w.pair||r.pair!=0||upgraded;if(r.pairGapMs>=0.f)w.pairGapMs=r.pairGapMs;
                w.upm=u;w.v0={v0g[0],0.f,v0g[2]};w.v0Dir=hp::v::dot(v0g,gsum.dir);w.v0Fwd=M::dot(w.v0,v.rowC);w.kick2Lever=r.pushKick2!=0;
                w.servoLever=w.servoLever||r.pushKickServo!=0;w.earlyLever=w.earlyLever||r.pushKickEarly!=0;
                if(upgraded&&now>=watch->t0)w.kickBase=now-watch->t0; // the knockdown's message is sent now: its kick window starts here
                if(grec)grec->watchT0=watch->t0;}
            w.hpBefore=hpBefore;w.hit=point;
            std::uintptr_t m{};float pos[3]{};if(get(v.actor,off::actorMover,m)&&m&&block(m+0x70,pos,sizeof pos)){w.start={pos[0],pos[1],pos[2]};w.haveStart=true;}
            {std::uint32_t rag{};if(m&&get(m,0xb8c,rag))w.ragAtPush=std::int8_t(rag!=0);}
            {std::int16_t ba{},bb{};if(m&&get(m,0x846,ba)&&get(m,0x848,bb)){w.busyA=ba;w.busyB=bb;}}
            w.pushClass=std::uint8_t(hp::pushClass(w.dirFwd,w.dirSide));w.escalated=w.escalated||escal;w.edgeA=r.edgeA>=0.f?r.edgeA:r.a;w.frozenMsg=w.frozenMsg||frozenMsg;
            w.frozenHeld=w.frozenHeld||frozenHeldNow;
            // the fall's bones: the pelvis and head of a humanoid layout (the chain nearBodies keeps for this creature)
            if(v.chain.ok&&v.layout>=0&&unsigned(v.layout)<layoutCount_&&layouts_[unsigned(v.layout)].humanoid){const auto& l=layouts_[unsigned(v.layout)].layout;
                w.globals=v.chain.globals;w.boneCount=v.chain.bones;w.pelvisBone=l.index[M::BonePelvis];w.headBone=l.index[M::BoneHead];readFallBones(w,0);
                w.comBone={l.index[M::BonePelvis],l.index[M::BoneSpine3],l.index[M::BoneHead],l.index[M::BoneKneeL],l.index[M::BoneKneeR]};
                w.torsoBones=r.pushKickHigh?std::array<std::int32_t,3>{l.index[M::BoneSpine3],l.index[M::BoneSpine0],l.index[M::BonePelvis]}:
                                            std::array<std::int32_t,3>{l.index[M::BoneSpine0],l.index[M::BoneSpine3],l.index[M::BonePelvis]};}
            // the kick's plan (sent by watchPush once ragdoll): the palm's direction, its size (graded: hp::kickImpulse in the message path)
            {const hp::PushKick pk=hp::pushKickPlan({dir.x,dir.y,dir.z},r.vel,r.a,r.pushKickK,r.pushKickMaxNs);
             w.palmMps=pk.palmMps;w.palmDir={pk.palmDir[0],pk.palmDir[1],pk.palmDir[2]};w.kickDir={pk.dir[0],pk.dir[1],pk.dir[2]};w.kickMag=pk.mag;w.kickSrc=pk.src;}}
        alignas(16) float pos16[4]{point.x,point.y,point.z,0};
        void* victim=reinterpret_cast<void*>(v.actor);void* att=reinterpret_cast<void*>(p.actor);
        // the node of this kind
        hp::TouchNodes tn{};const char* path=hp::touchNodes.load(tn)?(slap?tn.slap:tn.push):nullptr;
        NodeResult nr=NodeNoPath;
        if(!config.dry&&wantMsg&&path&&path[0]){nr=playNode(v,path,now);}
        bump(stats.nodeBy[nr<NodeCount?nr:NodeOff]);lt.node=int(nr);
        unsigned variant=WatchVariantNone;int pathCode=4;
        if(nr==NodePlayed){variant=WatchVariantNode;pathCode=5;if(!graded){try{api_.audio(att,victim,pos16);}catch(...){bump(stats.exceptions);}}}
        else{
            // the message: the zombie check only for a self message, the fake guid only where it may be needed
            const unsigned want=wantMsg?std::min<unsigned>(r.attacker,hp::OpenMsgNone):unsigned(hp::OpenMsgNone);
            bool zombie=true;
            if(want==hp::OpenMsgSelf&&hp::touchApi.zombie&&hp::touchApi.faction){using ZombieFn=bool(*)(const void*,const void*);
                try{zombie=reinterpret_cast<ZombieFn>(hp::touchApi.zombie)(reinterpret_cast<const void*>(hp::touchApi.faction),reinterpret_cast<const void*>(v.actor));}catch(...){zombie=true;}}
            if(want==hp::OpenMsgSelf&&zombie)bump(stats.touchSelfRefused);
            std::uint32_t fake=0;
            if(want==hp::OpenMsgFake||(want==hp::OpenMsgSelf&&zombie)){std::uintptr_t manager{},slots{};std::uint16_t count{},gen0{};std::uintptr_t probe{};
                if(get(api_.actorManager,0,manager)&&manager&&get(manager,0,count)&&count>=1&&get(api_.actorSlots,0,slots)&&slots&&get(slots,8,gen0)){fake=hp::fakeGuid(gen0);lt.gen0=gen0;}
                if(fake&&slotActor(fake,probe)){fake=0;lt.fakeResolved=true;} // never a guid that resolves
                lt.fake=fake;if(!fake)bump(stats.touchFakeGuidFail);}
            const hp::OpenTouchPlan plan=hp::openTouchPlan(want,v.guid,fake,zombie);
            variant=plan.msg;pathCode=plan.msg==hp::OpenMsgSelf?2:plan.msg==hp::OpenMsgFake?3:4;
            if(plan.msg!=hp::OpenMsgNone&&plan.attackerGuid&&plan.attackerGuid!=p.guid&&!invulnerable&&!config.dry){
                const M::Capsule cap=r.part>=0&&unsigned(r.part)<v.body.count?v.body.capsules[unsigned(r.part)]:M::actorCapsule(v.position);
                // the component: the engine's probe on this victim (as apply), else the frag child of the capsule's bone; both bounds-checked
                Chain chain{};const bool chainOk=deriveChain(v.actor,chain);int comp=-1;Vec3 hitPos=point,normal=dir*-1.f;
                if(chainOk&&config.probe&&api_.probeOne){
                    const Vec3 axis=M::capsuleAxisPoint(cap,point);const Vec3 nrm=M::normalized(point-axis,dir*-1.f);
                    alignas(16) float seg[8]{};const Vec3 a=axis+nrm*(cap.radius+.15f);seg[0]=a.x;seg[1]=a.y;seg[2]=a.z;seg[4]=axis.x;seg[5]=axis.y;seg[6]=axis.z;
                    alignas(16) unsigned char isect[off::isectSize]{};std::uint32_t mark=0xffff;std::memcpy(isect+off::isectLevel,&mark,4);std::memcpy(isect+off::isectMark,&mark,4);
                    int n=0;try{n=api_.probeOne(seg,.03f,1,isect,reinterpret_cast<const void*>(chain.inst));}catch(...){n=0;}
                    if(n>0){const int c=at<std::uint16_t>(isect,off::isectComponent);
                        if(validComponent(chain,c)){comp=c;bump(stats.touchProbeComp);const Vec3 hp2=vec(isect,off::isectPos),hn=vec(isect,off::isectNormal);
                            if(M::finite(hp2)&&(hp2-point).length()<1.f)hitPos=hp2;if(M::finite(hn)&&hn.length()>.5f)normal=M::normalized(hn);}}}
                if(comp<0&&chainOk){const int c=componentForBone(chain,cap.bone);if(c>=0){comp=c;bump(stats.touchBoneComp);}}
                if(comp<0)bump(stats.touchNoComp);
                lt.comp=comp;
                std::uintptr_t playerMelee{},block2{},mvt{};float impulseScale=0;
                if(p.anim&&get(p.anim,off::animBlock,block2)&&block2&&get(block2,off::blockMelee,playerMelee)&&playerMelee&&get(playerMelee,0,mvt)&&mvt==api_.aniMeleeVtable){
                    std::uintptr_t data{};float k{};if(get(playerMelee,off::meleeActionData,data)&&data&&get(data,off::actionImpulse,k)&&std::isfinite(k)&&k>=0&&k<1e4f)impulseScale=k;}
                stats.punchImpulseK.store(impulseScale,std::memory_order_relaxed);
                // the fall's direction: the impulse (NM's bulletVel: the creature falls in NMShotTask) along the push at the lever's size, else
                // the punch's own k, else the fist kick's size (k reads 0 outside an engine melee); the face from the push's direction (the
                // engine punch's convention with the push for the attacker's row: from behind = 1)
                const hp::PushImpulse pim=hp::pushImpulse({pdir.x,pdir.y,pdir.z},r.pushImpulse,impulseScale,config.push*std::max(r.a,1.f));
                const hp::V3 iv=pim.v;bump(stats.pushImpulseBy[pim.src<3?pim.src:2u]);
                const float impScale=(slap||tier==hp::TierFirm||knock)&&!frozenMsg?1.f:.3f;
                M::msg::Fields f{};f.hitPos=hitPos;f.normal=normal;f.impulse=Vec3{iv[0],iv[1],iv[2]}*impScale;f.attackerPosPtr=p.matrix+0x30;
                f.attackerGuid=plan.attackerGuid;f.flags=M::msg::flagKo;f.zone=cap.zone;f.face=hp::pushFace({v.rowC.x,v.rowC.y,v.rowC.z},{pdir.x,pdir.y,pdir.z});
                // a knockdown from the front (lever pushfront): staggerFall carries the body forward along the victim's facing, toward the
                // pusher; the low-impulse SHOVED (face 0) or the face-1 branch instead (hp::pushFrontPlan)
                // the knockdown by the route table (lever pushknock, hp::knockPlan): the push's side in the victim's frame and the mover's
                // frozen count at the message pick the face, the impulse size and the bullet's sign (the kick along the push is unchanged)
                // KNOCK PICK (lever pushknockpick): the behaviour by push side through the shared roll (written right before the broadcast,
                // held by tickWatches until the node lands); the message stays the route table's (lever pushknockpickmsg 1: the pick's,
                // hp::knockPickPlan, only when forced)
                int pickPar=-1;KnockPickWhy pickWhy=PickOff;
                if(knock&&sideP.reaction<0){
                    const float dFwd=M::dot(pdir,v.rowC),dSide=M::dot(pdir,v.rowA);
                    std::int16_t b6{-1},b8{-1};{std::uintptr_t mv{};if(!(get(v.actor,off::actorMover,mv)&&mv&&get(mv,0x846,b6)&&get(mv,0x848,b8))){b6=-1;b8=-1;}}
                    // frozen: +0x848 now, or this push sent / held the frozen mover's UP message (which clears +0x848; the route stays NMShot)
                    const bool pushFz=grec&&(grec->frozenMsg||grec->frozenSeen);const bool fz=hp::knockFrozen(b8,pushFz);
                    const hp::KnockPlan kp0=hp::knockPlan(b8<0&&!pushFz&&r.pushKnock==1?2u:r.pushKnock,fz,dFwd,r.pushFront,r.pushFrontImp,r.pushBehindImp);
                    const int wantPar=hp::knockPickParity(r.pushKnockPick,dFwd,r.pushKnockPickFwd);
                    if(wantPar>=0){pickWhy=!(watch&&watch->guid==v.guid)?PickNoWatch:pickReady();if(pickWhy==PickForced)pickPar=wantPar;}
                    bump(stats.pickBy[pickWhy<PickWhyCount?pickWhy:0u]);
                    // the message: the route table's (the device table's), the pick's (lever pushknockpickmsg 1) only with the behaviour
                    // forced; a coin (tree unread, no watch, sm_RandFunc set, ...) always keeps the route table's (hp::knockMessagePlan)
                    const hp::KnockPlan kp=hp::knockMessagePlan(kp0,r.pushKnockPickMsg?pickPar:-1,dFwd,r.pushBehindImp);
                    // an NMShot knockdown's message to the torso component (lever pushknockspine): the clavicle / arm part as the twist's cause
                    if(pickPar==0&&r.pushKnockSpine&&chainOk&&watch&&watch->guid==v.guid)
                        for(const int bn:watch->push.torsoBones)if(bn>=0&&unsigned(bn)<chain.bones){const int c=componentForBone(chain,bn);if(c>=0){comp=c;lt.comp=c;break;}}
                    if(kp.row!=hp::KnockNone){
                        if(kp.face>=0)f.face=kp.face;
                        if(kp.impulse>0.f){const float l=f.impulse.length();f.impulse=l>1e-4f?f.impulse*(kp.impulse/l):pdir*kp.impulse;}
                        else{const hp::SidePlan bp=hp::pushSidePlan(0,dFwd,0.f,pim.src,r.pushBehindImp);
                            if(bp.impulse>0.f){const float l=f.impulse.length();f.impulse=l>1e-4f?f.impulse*(bp.impulse/l):pdir*bp.impulse;bump(stats.behindImp);}}
                        // the reversed bullet: its forward part in the victim's frame mirrored, the sideways part kept (lever pushknockflip)
                        if(kp.bullet<0){const hp::V3 fb=hp::knockBulletFlip({f.impulse.x,f.impulse.y,f.impulse.z},{v.rowC.x,v.rowC.y,v.rowC.z},{0.f,1.f,0.f},r.pushKnockFlip);
                            f.impulse=Vec3{fb[0],fb[1],fb[2]};}
                        bump(stats.knockRows[kp.row<4?kp.row:0u]);
                        if(watch&&watch->guid==v.guid&&kp.row==hp::KnockFrontFree)watch->push.frontLever=std::uint8_t(r.pushFront);}
                    else{ // lever pushknock 0: the front plan and the behind size alone
                        const hp::FrontPlan fp=hp::pushFrontPlan(r.pushFront,dFwd,r.pushFrontImp);
                        if(fp.face>=0)f.face=fp.face;
                        if(fp.impulse>0.f){const float l=f.impulse.length();f.impulse=l>1e-4f?f.impulse*(fp.impulse/l):pdir*fp.impulse;}
                        if(watch&&watch->guid==v.guid)watch->push.frontLever=fp.face>=0?r.pushFront:0u;
                        // from behind with the fallback size (k unread): the size k reads in the device logs (lever pushbehindimp)
                        const hp::SidePlan bp=hp::pushSidePlan(0,dFwd,0.f,pim.src,r.pushBehindImp);
                        if(bp.impulse>0.f){const float l=f.impulse.length();f.impulse=l>1e-4f?f.impulse*(bp.impulse/l):pdir*bp.impulse;bump(stats.behindImp);}}
                    if(watch&&watch->guid==v.guid){auto& w=watch->push;w.knockRow=kp.row;w.knockB6=b6;w.knockB8=b8;w.knockDirFwd=dFwd;w.knockDirSide=dSide;
                        w.pickParity=std::int8_t(pickPar);w.pickWhy=std::uint8_t(pickWhy);w.pickDone=pickPar<0;w.knockDir=pdir;
                        w.knockFz=std::int8_t(kp.row==hp::KnockNone||(b8<0&&!pushFz)?-1:fz?1:0);
                        w.knockClass=std::uint8_t(hp::pushClass(dFwd,dSide));w.knockBullet=std::int8_t(kp.row!=hp::KnockNone?kp.bullet:1);}}
                f.component=comp;f.weapon=-1;f.damage=0.f;
                if(watch&&watch->guid==v.guid){auto& w=watch->push;w.face=std::int8_t(f.face);w.impulse=f.impulse.length();w.impulseK=impulseScale;w.impulseSrc=std::int8_t(pim.src);w.comp=comp;w.hit=hitPos;
                    w.normal=normal;w.bulletSent=f.impulse;w.zone=cap.zone;
                    // the knockdown's message: its face and size, when (its own landed read one tick later), the other messages before it
                    if(knock){w.knockFace=std::int8_t(f.face);w.knockImp=f.impulse.length();w.knockAt=now;w.knockAge=std::int64_t(now>=watch->t0?now-watch->t0:0);w.knockRead=false;w.knockN=std::int8_t(watch->n);
                        unsigned near=0;for(const auto& e:msgLog_)if(e.guid==v.guid&&e.ns&&now>=e.ns&&now-e.ns<=200'000'000ull)++near;w.msgNear=std::uint8_t(std::min(near,255u));}
                    // the kick: a firm shove of a human (no slap) with the creature itself as the message's attacker (never a zombie), the
                    // KICK prints matched, not ragdoll at the push, no kick of the other hand's shove on it within 300 ms, a torso bone and
                    // the frag instance (kickNow); no attacker guid travels with it (projMsgApplyImpulse), so it reports no aggression
                    const bool want=r.pushKick!=0&&!slap&&(graded?knock:tier==hp::TierFirm)&&plan.msg==hp::OpenMsgSelf;
                    w.inst=chainOk?chain.inst:0;
                    // graded: the kick that sets the body's horizontal velocity to knockdownMps along the push from its own (the frag
                    // archetype's mass +0x38 when 40..150 kg, else 75 kg; a plain read)
                    if(graded&&knock){float kg=-1.f;std::uintptr_t arch{};
                        if(w.inst&&get(w.inst,0x8,arch)&&arch&&!get(arch,0x38,kg))kg=-1.f;
                        w.massKg=hp::kickMassKg(kg);w.vDes=hp::knockdownMps(gsum.aEff,r.pushDownMps);
                        const hp::V3 J=hp::kickImpulse(w.massKg,w.vDes,gsum.dir,v0g,r.pushKickMaxNs);w.kickJ={J[0],J[1],J[2]};
                        w.kickMag=w.kickJ.length();w.kickDir=w.kickMag>1e-4f?w.kickJ*(1.f/w.kickMag):pdir;}
                    bool pairKick=false;
                    for(const auto& o:watches_)if(&o!=watch&&o.set&&o.guid==v.guid&&(o.push.kick||o.push.kicked)&&now>=o.t0&&now-o.t0<hp::pushKickPairNs)pairKick=true;
                    const bool torso=w.inst&&(w.torsoBones[0]>=0||w.torsoBones[1]>=0||w.torsoBones[2]>=0);
                    const auto gate=hp::pushKickGate(want,v.race==0,api_.impulse&&hp::touchApi.kickOk,w.ragAtPush,pairKick,torso,w.kickMag,kickLocked(v.guid));
                    w.kickState=kickFromGate[gate<kickFromGate.size()?gate:1u];
                    w.kick=w.kickState==KickArmed;if(w.kick)bump(stats.kickSrc[w.kickSrc<3?w.kickSrc:2u]);}
                alignas(16) unsigned char m[M::msg::size];
                if(M::msg::build(m,f)){
                    if(comp>=0&&api_.hitInfo){ // the bone name, zone and local hit of that component (SetFromTestProbe, as apply)
                        alignas(16) unsigned char isect[off::isectSize]{};M::msg::putVec(isect,off::isectPos,hitPos);M::msg::putVec(isect,off::isectNormal,normal);
                        const std::uint16_t c16=std::uint16_t(comp);std::memcpy(isect+off::isectComponent,&c16,2);std::memcpy(isect+off::isectInst,&chain.inst,sizeof chain.inst);
                        try{api_.hitInfo(m+M::msg::localPos,isect,reinterpret_cast<const void*>(v.actor));}catch(...){}}
                    if(watch&&watch->guid==v.guid){auto& w=watch->push;std::memcpy(&w.hitLocal.x,m+M::msg::localPos,4);std::memcpy(&w.hitLocal.y,m+M::msg::localPos+4,4);
                        std::memcpy(&w.hitLocal.z,m+M::msg::localPos+8,4);
                        std::memcpy(w.bone,m+M::msg::boneName,16);w.bone[16]=0;for(char& ch:w.bone)if(ch&&(ch<0x20||ch>0x7e||ch=='"'||ch=='\\'))ch='?';}
                    m[M::msg::painSurvived]=1;m[M::msg::force+1]=1; // msg +0xd9 / +0xdb: no Disarm (0x1e187bc..0x1e187d0)
                    M::msg::put<float>(m,M::msg::damage,plan.damage);lt.koDamage=plan.damage; // never damage (KO included)
                    {int z=M::msg::get<std::int32_t>(m,M::msg::zone);lt.zone=z;std::memcpy(lt.bone,m+M::msg::boneName,16);lt.bone[16]=0;
                     for(char& ch:lt.bone)if(ch&&(ch<0x20||ch>0x7e||ch=='"'||ch=='\\'))ch='?';}
                    if(pickPar>=0&&watch&&watch->guid==v.guid){auto& w=watch->push;std::int32_t n{};std::int8_t uu{};bool wrote{};
                        if(pickRoll(pickPar,n,uu,wrote)){w.rollBefore=n;w.usedBefore=uu;if(wrote)bump(stats.pickRewrites);}else{w.pickParity=-1;w.pickDone=true;w.pickWhy=PickReadFail;}}
                    // KNOCK TOUGH: a tough actor's bit cleared for this message (put back at its landed node, 120 ms at most)
                    if(knock&&watch&&watch->guid==v.guid&&(hflags&hp::toughBit)){auto& w=watch->push;bump(stats.toughSeen);w.tough=1;
                        if(!r.pushKnockTough)bump(stats.toughKeptLever);
                        else if(!hp::touchApi.toughOk)bump(stats.toughKeptPrints);
                        else{int sl=-1;if(toughClear(v,now,sl)){w.tough=2;w.toughSlot=std::int8_t(sl);w.toughRestored=false;bump(stats.toughCleared);}else bump(stats.toughKeptOther);}}
                    // KNOCK PICK: the draw function holds the parity for the victim's own conditions until the node lands
                    if(pickPar>=0&&r.pushKnockPickFunc&&watch&&watch->guid==v.guid&&watch->push.pickParity>=0)pickFnStart(v.actor,v.guid,pickPar,now);
                    try{api_.broadcast(victim,m);if(!graded)api_.audio(att,victim,pos16);}catch(...){bump(stats.exceptions);}
                    // KNOCK EARLY (lever pushKickEarly): kick 1 queued on the message's own tick (entPendingImpulse::Update counts it down
                    // while the object is inactive and applies it on the second active update), not on the first ragdoll tick
                    if(knock&&graded&&watch&&watch->guid==v.guid){auto& w=watch->push;
                        if(w.earlyLever&&w.kick&&!w.kicked){w.kicked=true;w.kickState=kickNow(*watch,w.kickJ*w.upm);
                            const std::uint64_t age=now>=watch->t0?now-watch->t0:0;
                            if(w.kickState==KickSent||w.kickState==KickLost)w.kickMs=std::int32_t(age/1'000'000ull);
                            w.early=w.kickState==KickSent?1:0;bump(w.early?stats.earlyKicks:stats.earlyFailed);
                            if(w.kickState==KickSent)w.kickQueuedActive=w.ragdollMs>=0;
                            // nothing reached the engine (no chain, no torso component, no free slot): kick 1 stays armed for the first
                            // ragdoll tick (a KickLost call did reach it and is not repeated)
                            if(w.kickState==KickInst||w.kickState==KickNoComp||w.kickState==KickFull){w.kicked=false;bump(stats.earlyRetried);}
                            if(w.kickState==KickSent){w.kick1Age=age;Vec3 pv{};if(readPelvisNow(*watch,pv)){w.kickPelvis=pv;w.kickPelvisAge=age;w.haveKickPelvis=true;}
                                if(w.servoLever){w.servoWhy=0;w.servoSpent=w.kickJ.length();bump(stats.servoArmed);}}}}
                    // KNOCK OFF: the hands leave the knocked-down creature's ragdoll alone for knockOffMs; LATE FALL: its fall followed to 1500 ms
                    if(knock&&watch&&watch->guid==v.guid){const int ko=knockOffArm(v.actor,v.guid,watch->push.inst,pdir,r.knockOffMs,now);followArm(*watch,ko,now);}
                    if(knock&&watch&&watch->guid==v.guid&&watch->push.pickParity>=0){auto& w=watch->push;std::int32_t n{};std::int8_t uu{};bool wrote{};
                        if(pickRoll(-1,n,uu,wrote)){w.rollAfter=n;w.usedAfter=uu;}}
                    // the open hands' messages: a knockdown's watch counts any other within 200 ms after it
                    msgLog_[msgLogNext_]={v.guid,now};msgLogNext_=(msgLogNext_+1u)%unsigned(msgLog_.size());
                    for(auto& o:watches_)if(&o!=watch&&o.set&&o.guid==v.guid&&o.push.knockAt&&now>=o.push.knockAt&&now-o.push.knockAt<=200'000'000ull&&o.push.msgNear<255)++o.push.msgNear;
                    // NotifyInjured 0x1e18344..0x1e183d0 copies msg +0x10..+0x6f to animator +0x840..+0x89f: equal bytes = the animator took it
                    {alignas(8) unsigned char seen[0x60]{};
                     if(v.anim&&block(v.anim+0x840,seen,sizeof seen)){lt.landed=std::memcmp(seen,m+0x10,sizeof seen)==0;bump(lt.landed?stats.msgLanded:stats.msgNotLanded);
                         get(v.anim,0xaf0,lt.animDamage);get(v.anim,0xaec,lt.animInjury);}}
                    lt.msg=true;bump(stats.touchMsgSent);}}
            else if(plan.msg!=hp::OpenMsgNone){variant=WatchVariantNone;pathCode=4;}}
        lt.attacker=int(variant);lt.path=pathCode;bump(stats.touchByVariant[std::min<unsigned>(variant,WatchVariantCount-1u)]);bump(stats.touchPath[unsigned(pathCode)]);
        if(watch&&watch->guid==v.guid)watch->variant=std::uint8_t(variant);
        {alignas(8) unsigned char after[off::healthBlock]{};if(block(v.health,after,sizeof after)){lt.hpAfter=at<float>(after,off::healthValue);lt.koAfter=at<float>(after,off::healthKo);}}
        if(slap){const float yaw=hp::slapYaw(r.tangent,r.side,r.slapYawRad);
            if(canPush)queuePush(v.guid,v.actor,dir,.015f*u,2,yaw/3.f,3);
            if(!thrown)haptics::pulse(h,.5f,40'000,now,haptics::SourceMeleeHit);} // a thrown prop's tap lands away from the hand: no buzz
        else if(graded){ // a stumble: its distance along the push; a knockdown: no offset (the kick moves the body)
            if(!knock){const float d=hp::stumbleDistanceM(gsum.aEff);lt.distCm=d*100.f;
                if(r.push&&!frozenMsg&&!frozenHeldNow)queueStumble(pdir,d,grec);
                if(grec)grec->askedW=d*u;if(watch&&watch->guid==v.guid)watch->push.askedCm=d*100.f;}
            haptics::pulse(h,knock?.5f:.35f,30'000,now,haptics::SourceMeleeHit);}
        else{ // a push escalated from a press: 30 cm over 10 ticks; an approach shove: its own distance
            const float d=r.escalated?.30f:hp::shoveDistanceM(r.a,strong);const hp::PushPlan plan=r.escalated?hp::PushPlan{.03f,10u}:hp::pushPlan(d);lt.distCm=d*100.f;
            if(canPush&&plan.ticks)queuePush(v.guid,v.actor,dir,plan.perTickM*u,plan.ticks,0.f,0);
            haptics::pulse(h,.35f,30'000,now,haptics::SourceMeleeHit);}
        {std::lock_guard lock(touchMutex);lastTouch=lt;}
    }
}

// GRAB: a held prop swung into a creature (handphys PropHitRequest; its gate passed there): the player's fist strike on that creature
// (Engine::apply: KO damage times propko, the reaction along the swing, hostility; the impulse along the swing when strong), the face
// from the swing; the other holding hand buzzes too. Horses and creatures this tick's scan does not hold are refused.
inline void Engine::consumePropHit(const Player& p,std::uint64_t now) noexcept {
    namespace hp=::rdrvr::vr::handphys;
    const auto w=hp::propHitWalk(propSerial_,hp::propHitLatest.load(std::memory_order_acquire));
    if(!w.last)return;
    propSerial_=w.last;if(w.skipped)stats.propSkipped.fetch_add(w.skipped,std::memory_order_relaxed);
    for(std::uint64_t q=w.first;q<=w.last;++q){
        hp::PropHitRequest r{};if(!hp::propHitRing[q%hp::propHitSlots].load(r)||r.serial!=q){bump(stats.propSkipped);continue;}
        propHit(p,r,now);}
}
inline void Engine::propHit(const Player& p,const ::rdrvr::vr::handphys::PropHitRequest& r,std::uint64_t now) noexcept {
    namespace hp=::rdrvr::vr::handphys;
    bump(stats.propConsumed);if(r.thrown)bump(stats.propThrownConsumed);
    const auto refuse=[&](hp::PropHitWhy w){bump(stats.propRefused[unsigned(w)<stats.propRefused.size()?unsigned(w):0u]);};
    if(!hp::fresh(now,r.ns)){refuse(hp::PropHitStale);return;}
    int idx=-1;for(unsigned i=0;i<candCount_;++i)if(cand_[i].guid==r.guid&&cand_[i].actor==r.actor){idx=int(i);break;}
    if(idx<0){refuse(hp::PropHitNotCand);return;}
    const Candidate& v=cand_[unsigned(idx)];
    if(v.handOnly||v.race!=0){refuse(hp::PropHitNotHuman);return;}
    const Vec3 pt{r.point[0],r.point[1],r.point[2]};
    int part=-1; // the body capsule nearest the contact (its zone and bone)
    {float best=1e30f;for(unsigned c=0;c<v.body.count;++c){const auto& cp=v.body.capsules[c];const float d=(M::capsuleAxisPoint(cp,pt)-pt).length()-cp.radius;
        if(std::isfinite(d)&&d<best){best=d;part=int(c);}}}
    PendingStrike s{};s.hand=r.hand>=0&&r.hand<2?r.hand:0;s.mode=Mode::Fist;s.victim=idx;s.part=part;s.point=pt;
    s.direction={r.dir[0],r.dir[1],r.dir[2]};s.speed=r.speed;s.prop=true;s.strong=r.strong!=0;s.koScale=r.ko;s.thrown=r.thrown!=0;
    try{apply(s,p,now);}catch(...){bump(stats.exceptions);}
    for(unsigned h=0;h<2;++h)if((r.hands>>h&1u)&&int(h)!=s.hand){haptics::pulse(h,.8f,60'000,now,haptics::SourceMeleeHit);bump(stats.propSecondPulse);}
}

inline void Engine::tick(const mt::Frame& frame) noexcept {
    const auto t0=nowNs();++tickIndex_;bump(stats.ticks);const std::uint64_t now=frame.now;
    knockUpkeep(now);
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
        setMasks(false,false);blockRelease(now);dropTouch();};
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
    // hand modes (holsters state; fist = empty VR hands + closed grip through the fist gate, Gothic vrgameplay.cpp:693)
    const int hold=H::holdHand.load(std::memory_order_acquire);
    const bool knife=H::heldKind.load(std::memory_order_relaxed)==kindMelee||H::currentSlot.load(std::memory_order_relaxed)==H::SlotMelee;
    const bool fistsOn=vs.meleeFists();
    const float speedSetting=vs.meleeSpeed();
    // DUAL: the phantom knife (second item, holstered for the engine) cuts with the same physical melee; fists only with both
    // hands empty.
    const int phantomHand=H::phantomHand.load(std::memory_order_acquire);
    const bool phantomKnife=phantomHand>=0&&H::phantomSlot.load(std::memory_order_relaxed)==H::SlotMelee&&H::dualActive();
    phantomKnife_=phantomKnife?phantomHand:-1;
    // the fist gate of each sample, in order (M::FistGate: a grip over .65 closed while slow, or held 180 ms)
    std::array<std::array<bool,2>,64> fistOk{};
    for(unsigned i=0;i<ns;++i)for(unsigned h=0;h<2;++h){auto& hd=hands_[h];const auto& x=samples[i];
        fistOk[i][h]=hd.gate.step(x.grips[h],{x.grip[h][0],x.grip[h][1],x.grip[h][2]},(x.valid>>h&1u)!=0,x.ns);
        if(hd.gate.waiting())bump(stats.fistWaiting);
        if(fistOk[i][h]&&!hd.gateOpen)bump(hd.gate.closeMps<M::fistCloseMps?stats.fistOpenSlow:stats.fistOpenHeld);
        hd.gateOpen=fistOk[i][h];}
    const auto modeFor=[&](unsigned h,bool fist){
        if(::rdrvr::vr::grablink::holding(h))return Mode::None; // GRAB: a hand holding a prop never punches
        if(hold==int(h)&&knife)return Mode::Blade;
        if(phantomKnife&&phantomHand==int(h))return Mode::Blade;
        if(fistsOn&&hold<0&&phantomHand<0&&fist)return Mode::Fist;
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
        for(unsigned h=0;h<2;++h)if(modeFor(h,fistOk[ns-1][h])!=Mode::None&&(last.valid>>h&1u)){armed|=1u<<h;
            if(mapOk_)handsWorld[h]=toWorld(map_,{last.grip[h][0],last.grip[h][1],last.grip[h][2]});}}
    else for(unsigned h=0;h<2;++h)if(hands_[h].mode!=Mode::None)armed|=1u<<h;
    // Knife in a VR hand or an armed fist: native strike off; FIRE off only for the engine knife / fists (DUAL: a phantom knife
    // next to an engine firearm keeps its trigger)
    setMasks(armed!=0||(hold>=0&&knife),(hold>=0&&knife)||(hold<0&&armed!=0));
    if(!armed){for(unsigned h=0;h<2;++h)if(hands_[h].mode!=Mode::None){resetHand(h,ResetMode);hands_[h].mode=Mode::None;stats.hands[h].mode.store(0);}
        // keep vr.melee.scan fresh for the report; every nearScanTicks while a hand-physics body is live (its NPC bump)
        const unsigned nearEvery=std::max(1u,::rdrvr::vr::handphys::nearScanTicks.load(std::memory_order_relaxed));
        // a held heavy prop or a thrown prop in flight (Palms::heavy, fresh): the candidates within reach of its centre too (a throw lands
        // beyond the player's own reach)
        if(mapOk_&&(tickIndex_%30u==0||(::rdrvr::vr::handphys::nearWanted.load(std::memory_order_relaxed)&&tickIndex_%nearEvery==0))){
            Vec3 second=p.position;::rdrvr::vr::handphys::Palms pm{};
            if(::rdrvr::vr::handphys::palms.load(pm)&&pm.heavy&&::rdrvr::vr::handphys::fresh(now,pm.ns)){const Vec3 c{pm.prop[0],pm.prop[1],pm.prop[2]};
                if(M::finite(c)){second=c;bump(stats.propScanPoint);}}
            scan(p,{p.position,second},3u);}
        nearBodies(now);consumeTouch(p,now);consumePropHit(p,now);runPushes(now);
        return;}
    bump(stats.activeTicks);
    if(mapOk_&&(tickIndex_-lastScan_>=3||!candCount_)){lastScan_=tickIndex_;
        // a fist / knife hand while a prop flies (or a heavy prop is held): the candidates near the prop too, as in the unarmed branch
        Vec3 propC{};bool propOk=false;{::rdrvr::vr::handphys::Palms pm{};
            if(::rdrvr::vr::handphys::palms.load(pm)&&pm.heavy&&::rdrvr::vr::handphys::fresh(now,pm.ns)){propC={pm.prop[0],pm.prop[1],pm.prop[2]};propOk=M::finite(propC);}}
        if(propOk)bump(stats.propScanPointArmed);
        scan(p,handsWorld,armed,propOk?&propC:nullptr);}
    if(mapOk_&&candCount_)buildBodies(handsWorld);
    std::array<M::BodyRef,maxCandidates> refs{};for(unsigned i=0;i<candCount_;++i)refs[i]={&cand_[i].body,!cand_[i].bodyValid||cand_[i].skip||cand_[i].handOnly};
    const auto query=[&](Vec3 a,Vec3 b,float r){return M::firstEntry(a,b,r,refs.data(),candCount_);};
    std::array<PendingStrike,2> strikes{};
    M::BodyFrame body{};if(mapOk_)body.update(map_.headLocal,map_.headForwardLocal);
    for(unsigned i=0;i<ns;++i){
        const auto& s=samples[i];
        if(!haveSpace_||s.epoch!=epoch_||s.space!=space_){if(haveSpace_)for(unsigned h=0;h<2;++h)resetHand(h,ResetSpace);epoch_=s.epoch;space_=s.space;haveSpace_=true;}
        if(mapOk_&&(s.epoch!=map_.epoch||s.space!=map_.space))continue; // a sample of another LOCAL space than the plan's
        for(unsigned h=0;h<2;++h){
            auto& hand=hands_[h];auto& hs=stats.hands[h];
            const Mode mode=modeFor(h,fistOk[i][h]);
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
                if(r.struck&&strikes[h].hand<0)strikes[h]={int(h),Mode::Fist,r.hit.target,r.hit.part,r.hit.point,tipW-previousTip,sw.speed,s.grips[h],
                    hand.gate.minGrip(s.ns),hand.gate.closedMs(s.ns),hand.gate.closeMps};
                hs.speed.store(sw.speed,std::memory_order_relaxed);maxOf(hs.maxSpeed,sw.speed);hs.contactTravel.store(sw.contactTravel,std::memory_order_relaxed);
                hs.continuous.store(sw.continuous,std::memory_order_relaxed);hs.ready.store(r.windowOpen,std::memory_order_relaxed);hs.inside.store(false);
            }
        }
    }
    for(const auto& s:strikes)if(s.hand>=0){try{apply(s,p,now);}catch(...){bump(stats.exceptions);}}
    consumeTouch(p,now);consumePropHit(p,now);runPushes(now);
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
// The push ring (touchMessages.pushRing) and the kick's counters (touchMessages.pushKick): per finished push / slap watch the push, what the
// message carried and what the animator held after it, the first node change and whether NMShotTask ran (task vtable 0x2d42968), the
// ragdoll, the kick and the fall of the pelvis / head bones.
inline void appendPushRing(std::string& out,std::uint64_t now) {
    namespace hp=::rdrvr::vr::handphys;char b[1024];const auto u=[](const Counter& c){return (unsigned long long)c.load(std::memory_order_relaxed);};
    std::snprintf(b,sizeof b,",\"pushKick\":{\"api\":%s,\"dropped\":%llu,\"hpLoss\":%llu,\"src\":{\"palm\":%llu,\"into\":%llu,\"none\":%llu},\"by\":{",
        hp::touchApi.kickOk?"true":"false",u(stats.pushDropped),u(stats.kickHpLoss),u(stats.kickSrc[0]),u(stats.kickSrc[1]),u(stats.kickSrc[2]));out+=b;
    for(unsigned i=0;i<KickStateCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",kickStateNames[i],u(stats.kickBy[i]));out+=b;}
    std::snprintf(b,sizeof b,"},\"graded\":{\"stumbles\":%llu,\"knockdowns\":%llu,\"upgrades\":%llu,\"pairMerged\":%llu,\"repeat\":%llu,\"replanned\":%llu,"
        "\"kick2Sent\":%llu,\"kick2NotWanted\":%llu,\"kick2Failed\":%llu",u(stats.gradeStumbles),u(stats.gradeKnockdowns),u(stats.gradeUpgrades),
        u(stats.gradePairMerged),u(stats.gradeRepeat),u(stats.gradeReplanned),u(stats.kick2Sent),u(stats.kick2NotWanted),u(stats.kick2Failed));out+=b;
    std::snprintf(b,sizeof b,"},\"fix\":{\"stumbleWaits\":%llu,\"waitOk\":%llu,\"busyTimeout\":%llu,\"bumpAsks\":%llu,\"jobMerged\":%llu,\"jobNoSlot\":%llu,"
        "\"escGraded\":%llu,\"escMerged\":%llu,\"peakRegrades\":%llu,\"peakUpgrades\":%llu,\"peakNoRec\":%llu,",u(stats.stumbleWaits),u(stats.stumbleWaitOk),
        u(stats.stumbleBusyTimeout),u(stats.bumpAsks),u(stats.jobMerged),u(stats.jobNoSlot),u(stats.escGraded),u(stats.escMerged),u(stats.peakRegrades),
        u(stats.peakUpgrades),u(stats.peakNoRec));out+=b;
    std::snprintf(b,sizeof b,"\"frozenMsg\":%llu,\"frozenRepeat\":%llu,\"frozenDrop\":%llu,\"escKeyed\":%llu,\"escOnKnock\":%llu,\"sideReaction\":%llu,\"behindImp\":%llu,"
        "\"fallBy\":{",u(stats.frozenMsg),u(stats.frozenRepeat),u(stats.frozenDrop),u(stats.escKeyed),u(stats.escOnKnock),u(stats.sideReaction),u(stats.behindImp));out+=b;
    // knockdowns: class -> node -> [along, against, side, none]
    {static constexpr const char* nodeText[4]{"staggerfall_back","staggerfall_front","bc3dfb68","other"};bool fc=true;
     for(unsigned c=0;c<4;++c){bool any=false;for(unsigned k=0;k<16;++k)any=any||u(stats.fallBy[c*16u+k]);if(!any)continue;
        std::snprintf(b,sizeof b,"%s\"%s\":{",fc?"":",",::rdrvr::vr::handphys::pushClassNames[c]);out+=b;fc=false;bool fn=true;
        for(unsigned n=0;n<4;++n){const auto* x=&stats.fallBy[c*16u+n*4u];if(!u(x[0])&&!u(x[1])&&!u(x[2])&&!u(x[3]))continue;
            std::snprintf(b,sizeof b,"%s\"%s\":[%llu,%llu,%llu,%llu]",fn?"":",",nodeText[n],u(x[0]),u(x[1]),u(x[2]),u(x[3]));out+=b;fn=false;}
        out+="}";}}
    out+="}";
    // KNOCKDOWN ROUTE: knockdowns by row, the held UP messages, the fall by row -> first node after the message -> [along, against, side,
    // none], the predicted route (free / frozen) -> [staggerFall, NMShot, neither]
    std::snprintf(b,sizeof b,",\"knock\":{\"rows\":{\"%s\":%llu,\"%s\":%llu,\"%s\":%llu,\"%s\":%llu},\"frozenHold\":{\"held\":%llu,\"superseded\":%llu,\"sent\":%llu,"
        "\"lost\":%llu,\"freed\":%llu,\"full\":%llu},\"routeMatrix\":{\"free\":[%llu,%llu,%llu],\"frozen\":[%llu,%llu,%llu]},\"fallBy2\":{",
        hp::knockRowNames[0],u(stats.knockRows[0]),hp::knockRowNames[1],u(stats.knockRows[1]),hp::knockRowNames[2],u(stats.knockRows[2]),hp::knockRowNames[3],u(stats.knockRows[3]),
        u(stats.frozenHeld),u(stats.frozenSuperseded),u(stats.frozenDeferredSent),u(stats.frozenHoldLost),u(stats.frozenHoldFreed),u(stats.frozenHoldFull),
        u(stats.routeMatrix[0]),u(stats.routeMatrix[1]),u(stats.routeMatrix[2]),u(stats.routeMatrix[3]),u(stats.routeMatrix[4]),u(stats.routeMatrix[5]));out+=b;
    {static constexpr const char* node6[7]{"staggerfall_back","staggerfall_front","bc3dfb68","731220e8","ad400c7a","other","tough"};bool fr=true;
     for(unsigned r=0;r<4;++r){bool any=false;for(unsigned k=0;k<28;++k)any=any||u(stats.fallBy2[r*28u+k]);if(!any)continue;
        std::snprintf(b,sizeof b,"%s\"%s\":{",fr?"":",",hp::knockRowNames[r]);out+=b;fr=false;bool fn=true;
        for(unsigned n=0;n<7;++n){const auto* x=&stats.fallBy2[r*28u+n*4u];if(!u(x[0])&&!u(x[1])&&!u(x[2])&&!u(x[3]))continue;
            std::snprintf(b,sizeof b,"%s\"%s\":[%llu,%llu,%llu,%llu]",fn?"":",",node6[n],u(x[0]),u(x[1]),u(x[2]),u(x[3]));out+=b;fn=false;}
        out+="}";}}
    // the fall by row -> push class -> [along, against, side, none]
    out+="},\"fallBy3\":{";
    {bool fr=true;
     for(unsigned r=0;r<4;++r){bool any=false;for(unsigned k=0;k<16;++k)any=any||u(stats.fallBy3[r*16u+k]);if(!any)continue;
        std::snprintf(b,sizeof b,"%s\"%s\":{",fr?"":",",hp::knockRowNames[r]);out+=b;fr=false;bool fn=true;
        for(unsigned c=0;c<4;++c){const auto* x=&stats.fallBy3[r*16u+c*4u];if(!u(x[0])&&!u(x[1])&&!u(x[2])&&!u(x[3]))continue;
            std::snprintf(b,sizeof b,"%s\"%s\":[%llu,%llu,%llu,%llu]",fn?"":",",hp::pushClassNames[c],u(x[0]),u(x[1]),u(x[2]),u(x[3]));out+=b;fn=false;}
        out+="}";}}
    out+="}";
    // KNOCK TOUGH / KNOCK OFF / LATE FALL
    {namespace hpn=::rdrvr::vr::handphys;
     std::snprintf(b,sizeof b,",\"tough\":{\"armed\":%s,\"seen\":%llu,\"cleared\":%llu,\"kept\":{\"lever\":%llu,\"prints\":%llu,\"other\":%llu},\"restored\":%llu,"
        "\"restoreLate\":%llu,\"scriptChanged\":%llu,\"lost\":%llu,\"landed\":{\"none\":[%llu,%llu],\"kept\":[%llu,%llu],\"window\":[%llu,%llu]}}",
        hpn::touchApi.toughOk?"true":"false",u(stats.toughSeen),u(stats.toughCleared),u(stats.toughKeptLever),u(stats.toughKeptPrints),u(stats.toughKeptOther),
        u(stats.toughRestored),u(stats.toughRestoreLate),u(stats.toughScriptChanged),u(stats.toughLost),u(stats.toughLanded[0]),u(stats.toughLanded[1]),
        u(stats.toughLanded[2]),u(stats.toughLanded[3]),u(stats.toughLanded[4]),u(stats.toughLanded[5]));out+=b;
     std::snprintf(b,sizeof b,",\"knockOff\":{\"armed\":%llu,\"measureOnly\":%llu,\"endedUp\":%llu,\"expired\":%llu,\"full\":%llu},"
        "\"late\":{\"followed\":%llu,\"fell\":%llu,\"notFell\":%llu,\"lost\":%llu,\"noBones\":%llu,\"byTough\":{\"none\":[%llu,%llu],\"kept\":[%llu,%llu],\"window\":[%llu,%llu]}}",
        u(stats.knockOffArmed),u(stats.knockOffMeasure),u(stats.knockOffEndedUp),u(stats.knockOffExpired),u(stats.knockOffFull),u(stats.lateFollowed),u(stats.lateFell),
        u(stats.lateNotFell),u(stats.lateLost),u(stats.lateNoBones),u(stats.lateFellByTough[0]),u(stats.lateFellByTough[1]),u(stats.lateFellByTough[2]),
        u(stats.lateFellByTough[3]),u(stats.lateFellByTough[4]),u(stats.lateFellByTough[5]));out+=b;}
    // KNOCK PICK: knockdowns by why, the forced ones by landed node [as chosen, other behaviour, other node, none], the roll's re-writes and
    // churn, the fall by landed behaviour -> class -> [along, against, side, none] against the message's push, the knockdown ring
    {const auto& t=::rdrvr::vr::handphys::touchApi;const int rf=stats.randFuncState.load(std::memory_order_relaxed);
     std::snprintf(b,sizeof b,",\"pick\":{\"armed\":%s,\"tree\":%d,\"randFunc\":\"%s\",\"by\":{",t.pickOk?"true":"false",stats.pickTree.load(std::memory_order_relaxed),
        rf<0?"unread":rf?"set":"0");out+=b;
     for(unsigned i=0;i<PickWhyCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",pickWhyNames[i],u(stats.pickBy[i]));out+=b;}
     {namespace hpn=::rdrvr::vr::handphys;
      std::snprintf(b,sizeof b,"},\"func\":{\"armed\":%s,\"installed\":%llu,\"refused\":%llu,\"busy\":%llu,\"uninstalled\":%llu,\"replaced\":%llu,\"victimDraws\":%llu,"
        "\"otherDraws\":%llu},\"landedTough\":{\"asChosen\":%llu,\"otherWay\":%llu,\"otherNode\":%llu,\"none\":%llu",t.pickFnOk?"true":"false",u(stats.pickFnInstalled),
        u(stats.pickFnRefused),u(stats.pickFnBusy),u(stats.pickFnUninstalled),u(stats.pickFnReplaced),(unsigned long long)hpn::pickFn.victimDraws.load(std::memory_order_relaxed),
        (unsigned long long)hpn::pickFn.otherDraws.load(std::memory_order_relaxed),u(stats.pickLandedTough[0]),u(stats.pickLandedTough[1]),u(stats.pickLandedTough[2]),
        u(stats.pickLandedTough[3]));out+=b;}
     std::snprintf(b,sizeof b,"},\"landed\":{\"asChosen\":%llu,\"otherWay\":%llu,\"otherNode\":%llu,\"none\":%llu},\"rewrites\":%llu,"
        "\"churn\":{\"ticks\":%llu,\"rollChanges\":%llu,\"usedFlips\":%llu,\"maxPerSec\":%llu,\"funcSetRefusals\":%llu},\"fallByPick\":{",
        u(stats.pickLanded[0]),u(stats.pickLanded[1]),u(stats.pickLanded[2]),u(stats.pickLanded[3]),u(stats.pickRewrites),u(stats.churnTicks),u(stats.churnNum),
        u(stats.churnUsed),u(stats.churnMaxPerSec),u(stats.churnFuncSet));out+=b;
     static constexpr const char* behText[3]{"staggerFall","NMShot","other"};bool fb=true;
     for(unsigned be=0;be<3;++be){bool any=false;for(unsigned k=0;k<16;++k)any=any||u(stats.fallByPick[be*16u+k]);if(!any)continue;
        std::snprintf(b,sizeof b,"%s\"%s\":{",fb?"":",",behText[be]);out+=b;fb=false;bool fc=true;
        for(unsigned c=0;c<4;++c){const auto* x=&stats.fallByPick[be*16u+c*4u];if(!u(x[0])&&!u(x[1])&&!u(x[2])&&!u(x[3]))continue;
            std::snprintf(b,sizeof b,"%s\"%s\":[%llu,%llu,%llu,%llu]",fc?"":",",hp::pushClassNames[c],u(x[0]),u(x[1]),u(x[2]),u(x[3]));out+=b;fc=false;}
        out+="}";}
     out+="}},\"knockRing\":[";
     std::array<KnockRec,24> kr{};unsigned kn{};{std::lock_guard lock(knockRingMutex);kr=knockRing;kn=knockRingNext;}bool fk=true;
     for(unsigned i=0;i<kr.size();++i){const auto& k=kr[(kn+i)%kr.size()];if(!k.ns)continue;char bone[17]{};std::memcpy(bone,k.bone,16);
        for(char& ch:bone)if(ch&&(ch<0x20||ch>0x7e||ch=='"'||ch=='\\'))ch='?';
        std::snprintf(b,sizeof b,"%s{\"ageMs\":%.0f,\"guid\":%u,\"class\":\"%s\",\"row\":\"%s\",\"dirFwd\":%.2f,\"dirSide\":%.2f,\"why\":\"%s\",\"parity\":%d,"
            "\"roll\":{\"before\":[%d,%d],\"after\":[%d,%d],\"landed\":[%d,%d],\"writes\":%u,\"ticks\":%u},\"landed\":{\"id\":\"0x%x\",\"task\":\"0x%x\",\"ms\":%d,\"parity\":%d}",
            fk?"":",",now>=k.ns?double(now-k.ns)/1e6:-1.0,k.guid,k.cls<hp::PushClassCount?hp::pushClassNames[k.cls]:"none",k.row<hp::KnockRowCount?hp::knockRowNames[k.row]:"none",
            double(k.dirFwd),double(k.dirSide),pickWhyNames[k.why<PickWhyCount?k.why:0],int(k.parity),int(k.rollBefore),int(k.usedBefore),int(k.rollAfter),int(k.usedAfter),
            int(k.rollLanded),int(k.usedLanded),unsigned(k.writes),unsigned(k.ticks),k.landedId,k.landedTask,int(k.landedMs),int(k.landedParity));out+=b;fk=false;
        std::snprintf(b,sizeof b,",\"face\":%d,\"imp\":%.1f,\"S\":[%.2f,%.2f],\"comp\":%d,\"bone\":\"%s\",\"J\":[%.1f,%.1f],\"pre\":{\"fwd\":[%.2f,%.2f],\"side\":[%.2f,%.2f]},"
            "\"vFwd\":[%.2f,%.2f,%.2f],\"vSide\":[%.2f,%.2f,%.2f],\"cm300\":[%.1f,%.1f],\"cm600\":[%.1f,%.1f],\"fall600\":{\"dir\":%.2f,\"msg\":%.2f,\"kick\":%.2f,\"cm\":%.1f}}",
            int(k.face),double(k.imp),
            double(k.sFwd),double(k.sSide),k.comp,bone,double(k.jFwd),double(k.jSide),double(k.preFwd[0]),double(k.preFwd[1]),double(k.preSide[0]),double(k.preSide[1]),
            double(k.kvFwd[0]),double(k.kvFwd[1]),double(k.kvFwd[2]),double(k.kvSide[0]),double(k.kvSide[1]),double(k.kvSide[2]),double(k.fwdCm300),double(k.sideCm300),
            double(k.fwdCm600),double(k.sideCm600),double(k.fall600),double(k.fall600Msg),double(k.fall600Kick),double(k.cm600));out+=b;
        out.pop_back(); // the record reopened for the tough state, the late fall and the hands-off window
        std::snprintf(b,sizeof b,",\"tough\":%d,\"late\":{\"fwdCm\":[%.1f,%.1f,%.1f],\"sideCm\":[%.1f,%.1f,%.1f],\"headDropCm\":[%.1f,%.1f],\"fell\":%d},"
            "\"off\":{\"pairs\":[%u,%u],\"towardMps\":%.2f,\"drop\":%d},\"servo\":%d}",int(k.tough),double(k.fwdCmLate[0]),double(k.fwdCmLate[1]),double(k.fwdCmLate[2]),
            double(k.sideCmLate[0]),double(k.sideCmLate[1]),double(k.sideCmLate[2]),double(k.headDrop600),double(k.headDrop1500),int(k.fell),k.offEarly,k.offLate,
            double(k.offTowardMps),int(k.offDrop),int(k.servo));out+=b;}
     out+="]";
     // KNOCK SERVO: the counters, the fall at 1500 ms by lever x class x (along, toward the pusher, sideways, not moved); KNOCK TRACE ring
     std::snprintf(b,sizeof b,",\"knockServo\":{\"armed\":%llu,\"kicks\":%llu,\"lost\":%llu,\"earlyKicks\":%llu,\"earlyFailed\":%llu,\"earlyRetried\":%llu,"
        "\"earlyFreed\":%llu,\"trackReads\":%llu,\"ends\":{",
        u(stats.servoArmed),u(stats.servoKicks),u(stats.servoLost),u(stats.earlyKicks),u(stats.earlyFailed),u(stats.earlyRetried),u(stats.earlyFreed),u(stats.trackReads));out+=b;
     for(unsigned i=0;i<hp::ServoWhyCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",hp::servoWhyNames[i],u(stats.servoEnds[i]));out+=b;}
     out+="},\"lateBy\":{";
     {bool fl=true;for(unsigned sv=0;sv<2;++sv)for(unsigned c=0;c<4;++c){const auto* x=&stats.lateByServo[sv*16u+c*4u];if(!u(x[0])&&!u(x[1])&&!u(x[2])&&!u(x[3]))continue;
        std::snprintf(b,sizeof b,"%s\"servo%u.%s\":[%llu,%llu,%llu,%llu]",fl?"":",",sv,hp::pushClassNames[c],u(x[0]),u(x[1]),u(x[2]),u(x[3]));out+=b;fl=false;}}
     out+="}},\"knockTrace\":[";
     {std::array<KnockTrace,8> kt{};unsigned tn{};{std::lock_guard lock(knockTraceMutex);kt=knockTrace;tn=knockTraceNext;}bool ft=true;
      for(unsigned i=0;i<kt.size();++i){const auto& t=kt[(tn+i)%kt.size()];if(!t.ns)continue;
        std::snprintf(b,sizeof b,"%s{\"ageMs\":%.0f,\"guid\":%u,\"class\":\"%s\",\"servo\":%d,\"early\":%d,\"earlySent\":%d,\"knockMs\":%d,\"ragMs\":%d,\"kickMs\":%d,"
            "\"kick1LandMs\":%d,\"J1\":[%.1f,%.1f],\"servoN\":%u,\"servoEnd\":\"%s\",\"spentNs\":%.1f,\"kicks\":[",ft?"":",",now>=t.ns?double(now-t.ns)/1e6:-1.0,t.guid,
            t.cls<hp::PushClassCount?hp::pushClassNames[t.cls]:"none",t.servo?1:0,t.early?1:0,int(t.earlySent),int(t.knockMs),int(t.ragMs),int(t.kickMs),int(t.kick1LandMs),
            double(t.j1F),double(t.j1S),unsigned(t.servoN),t.servoWhy<hp::ServoWhyCount?hp::servoWhyNames[t.servoWhy]:"none",double(t.servoSpent));out+=b;ft=false;
        for(unsigned k=0;k<t.servoN&&k<4;++k){std::snprintf(b,sizeof b,"%s[%d,%.1f,%.1f,\"%s\"]",k?",":"",int(t.sMs[k]),double(t.sjF[k]),double(t.sjS[k]),
            kickStateNames[t.sState[k]<KickStateCount?t.sState[k]:0]);out+=b;}
        out+="],\"rows\":[";
        for(unsigned k=0;k<t.n&&k<t.rows.size();++k){const auto& r=t.rows[k];
            std::snprintf(b,sizeof b,"%s[%d,%d,%d,%d,%d,%d,%lld,%d,%d]",k?",":"",int(r.ms),int(r.rag),int(r.pelF),int(r.pelS),int(r.comF),int(r.comS),
                r.slot==INT32_MIN?-999999ll:(long long)r.slot,int(r.jF),int(r.jS));out+=b;}
        std::snprintf(b,sizeof b,"],\"fall600Msg\":%.2f,\"trackOk\":%d,\"track\":[",double(t.fall600Msg),int(t.trackOk));out+=b;
        if(t.trackOk==1)for(unsigned k=0;k<t.track.size();++k){std::snprintf(b,sizeof b,"%s\"0x%x\"",k?",":"",t.track[k]);out+=b;}
        out+="]}";}}
     out+="]";}
    out+="}}},\"pushRing\":[";
    std::array<NodeWatch,8> ring{};unsigned nx{};{std::lock_guard lock(pushRingMutex);ring=pushRing;nx=pushRingNext;}bool first=true;
    static constexpr const char* tierText[4]{"light","medium","firm","prop"};
    const auto v3=[&](const char* name,const Vec3& v){std::snprintf(b,sizeof b,",\"%s\":[%.3f,%.3f,%.3f]",name,double(v.x),double(v.y),double(v.z));out+=b;};
    for(unsigned i=0;i<ring.size();++i){const auto& w=ring[(nx+i)%ring.size()];if(!w.set)continue;const auto& x=w.push;
        std::snprintf(b,sizeof b,"%s{\"ageMs\":%.0f,\"guid\":%u,\"slap\":%s,\"strong\":%s,\"tier\":\"%s\",\"a\":%.2f,\"depthCm\":%.1f,\"arm\":%.2f,\"variant\":\"%s\","
            "\"reaction\":%d,\"face\":%d,\"comp\":%d,\"zone\":%d,\"bone\":\"%s\",\"dirFwd\":%.2f,\"dirSide\":%.2f,\"headDir\":%.2f,\"palmMps\":%.2f",
            first?"":",",now>=w.t0?double(now-w.t0)/1e6:-1.0,w.guid,x.slap?"true":"false",x.strong?"true":"false",tierText[x.tier<4?x.tier:0],double(x.a),double(x.depthCm),
            double(x.arm),watchVariantNames[w.variant<WatchVariantCount?w.variant:0],int(x.reaction),int(x.face),x.comp,x.zone,x.bone,double(x.dirFwd),double(x.dirSide),
            double(x.headDir),double(x.palmMps));out+=b;first=false;
        v3("dir",x.dir);v3("palmDir",x.palmDir);v3("kickDir",x.kickDir);v3("rowC",x.rowC);v3("bulletSent",x.bulletSent);v3("hitWorld",x.hit);v3("hitLocal",x.hitLocal);
        v3("normalWorld",x.normal);v3("nmBullet",x.bullet);
        // the first node change and NMShotTask among the sampled tasks
        bool nmShot=false;for(unsigned k=0;k<w.n&&k<w.tr.size();++k)for(const auto t:w.tr[k].tasks)nmShot=nmShot||t==0x2d42968u;
        const NodeSample* c0=w.n?&w.tr[0]:nullptr;char path[100]{};if(c0){std::snprintf(path,sizeof path,"%s",c0->path);}
        std::snprintf(b,sizeof b,",\"landed\":{\"read\":%s,\"face\":%d,\"reaction\":%d,\"injury\":\"0x%x\",\"react\":%d},\"node\":{\"id\":\"0x%x\",\"parents\":[\"0x%x\",\"0x%x\",\"0x%x\"],"
            "\"ms\":%d,\"path\":\"%s\",\"nmShot\":%s},\"ragdollAtPush\":%d,\"ragdollMs\":%d,\"ragdoll150\":%d,\"kick\":{\"state\":\"%s\",\"src\":%u,\"ns\":%.1f,\"ms\":%d,"
            "\"comp\":%d,\"bone\":%d,\"freeSlots\":%d",
            x.landedRead?"true":"false",int(x.faceLanded),int(x.reactionLanded),x.injuryLanded,int(x.reactLevel),c0?c0->id:0u,c0?c0->parents[0]:0u,c0?c0->parents[1]:0u,
            c0?c0->parents[2]:0u,c0?int(c0->ms):-1,path,nmShot?"true":"false",int(x.ragAtPush),int(x.ragdollMs),int(x.ragdoll150),
            kickStateNames[x.kickState<KickStateCount?x.kickState:0],unsigned(x.kickSrc),double(x.kickMag),int(x.kickMs),int(x.kickComp),int(x.kickBone),int(x.kickFree));out+=b;
        v3("at",x.kickAt);out+="}";
        std::snprintf(b,sizeof b,",\"fall\":{\"bones\":[%d,%d,%d],\"globalsMoved\":%s,\"fall300\":%.2f,\"cm300\":%.1f,\"fall600\":%.2f,\"cm600\":%.1f,\"headFall600\":%.2f,\"headFwd600\":%.2f,"
            "\"moverDot\":%.2f,\"moverCm\":%.1f},\"hpBefore\":%.1f,\"hp600\":%.1f",x.haveBones[0]?1:0,x.haveBones[1]?1:0,x.haveBones[2]?1:0,
            x.globalsMoved?"true":"false",double(x.fall300),double(x.fallCm300),
            double(x.fall600),double(x.fallCm600),double(x.headFall600),double(x.headFwd600),double(x.fallDot),double(x.movedCm),double(x.hpBefore),double(x.hpAfter));out+=b;
        // the node at t0 (a get-up or AI move already running), the graded push
        {char bp[100]{};std::snprintf(bp,sizeof bp,"%s",w.before.path);for(char& ch:bp)if(ch&&(ch<0x20||ch>0x7e||ch=='"'||ch=='\\'))ch='?';
         std::snprintf(b,sizeof b,",\"nodeBefore\":{\"id\":\"0x%x\",\"path\":\"%s\"}",w.before.id,bp);out+=b;}
        std::snprintf(b,sizeof b,",\"fix\":{\"class\":\"%s\",\"frontLever\":%d,\"busy846\":%d,\"busy848\":%d,\"refusedAtTouch\":{\"busy\":%u,\"ragdoll\":%u,\"flags\":%u,\"other\":%u},"
            "\"waitMs\":%d,\"bumpAsked\":%s,\"escalated\":%s,\"jobMerged\":%u,\"edgeA\":%.2f,\"peakA\":%.2f,\"frozenMsg\":%s,\"aHand\":[%.2f,%.2f]}",
            x.pushClass<::rdrvr::vr::handphys::PushClassCount?::rdrvr::vr::handphys::pushClassNames[x.pushClass]:"none",x.frontLever==0xff?-1:int(x.frontLever),
            int(x.busyA),int(x.busyB),unsigned(x.refusedAtTouch[0]),unsigned(x.refusedAtTouch[1]),unsigned(x.refusedAtTouch[2]),unsigned(x.refusedAtTouch[3]),
            int(x.waitMs),x.bumpAsked?"true":"false",x.escalated?"true":"false",unsigned(x.jobMerged),double(x.edgeA),double(x.peakA),
            x.frozenMsg?"true":"false",double(x.aHand[0]),double(x.aHand[1]));out+=b;
        if(x.knockAge>=0){
            const bool along=x.fall600>=.5f&&x.fallCm600>=20.f;
            std::snprintf(b,sizeof b,",\"knock\":{\"row\":\"%s\",\"class\":\"%s\",\"dirFwd\":%.2f,\"dirSide\":%.2f,\"route\":%d,\"busy846\":%d,\"busy848\":%d,\"face\":%d,"
                "\"impulse\":%.1f,\"bullet\":%d,\"ms\":%.0f,\"msgNear\":%u,\"read\":%s,\"faceLanded\":%d,\"reactionLanded\":%d,\"frozenHeld\":%s,\"frozenSuperseded\":%s,"
                "\"pelvisFwd\":[%.2f,%.2f,%.2f],\"pelvisSide\":[%.2f,%.2f,%.2f],\"alongOk\":%s",
                x.knockRow<hp::KnockRowCount?hp::knockRowNames[x.knockRow]:"none",x.knockClass<hp::PushClassCount?hp::pushClassNames[x.knockClass]:"none",
                double(x.knockDirFwd),double(x.knockDirSide),x.knockFrozen(),int(x.knockB6),int(x.knockB8),int(x.knockFace),double(x.knockImp),int(x.knockBullet),
                double(x.knockAge)/1e6,unsigned(x.msgNear),x.knockRead?"true":"false",int(x.kFace),int(x.kReaction),x.frozenHeld?"true":"false",
                x.frozenSuperseded?"true":"false",double(x.kvFwd[0]),double(x.kvFwd[1]),double(x.kvFwd[2]),double(x.kvSide[0]),double(x.kvSide[1]),double(x.kvSide[2]),
                along?"true":"false");out+=b;
            v3("nmBullet",x.kBullet);
            std::snprintf(b,sizeof b,",\"tough\":%d,\"nodesBefore\":%d",int(x.tough),int(x.knockN));out+=b;
            // the node changes after the message (ms, id, the first two task vtables)
            out+=",\"nodes\":[";bool fn=true;const unsigned kms=unsigned(std::min<std::int64_t>(x.knockAge/1'000'000,0xffff));
            const int fa=::rdrvr::vr::handphys::knockFirstAfter(w.tr,w.n,x.knockN,kms);
            for(unsigned k=fa>=0?unsigned(fa):unsigned(w.n);k<w.n&&k<w.tr.size();++k){const auto& t=w.tr[k];
                std::snprintf(b,sizeof b,"%s[%u,\"0x%x\",\"0x%x\",\"0x%x\"]",fn?"":",",unsigned(t.ms),t.id,t.tasks[0],t.tasks[1]);out+=b;fn=false;}
            out+="]}";}
        else if(x.frozenHeld){std::snprintf(b,sizeof b,",\"knock\":{\"row\":\"none\",\"frozenHeld\":true,\"frozenSuperseded\":%s}",x.frozenSuperseded?"true":"false");out+=b;}
        if(x.graded){
            const float extra=x.movedCm>=0.f&&x.appliedCm>=0.f?x.movedCm-x.appliedCm:-999.f;
            std::snprintf(b,sizeof b,",\"grade\":{\"decision\":\"%s\",\"aEff\":%.2f,\"pair\":%s,\"pairGapMs\":%.0f,\"v0\":{\"mps\":%.2f,\"dir\":%.2f,\"rowC\":%.2f},"
                "\"massKg\":%.1f,\"vDes\":%.2f,\"askedCm\":%.1f,\"appliedCm\":%.1f,\"extraCm\":%.1f,\"refused\":{\"busy\":%u,\"ragdoll\":%u,\"flags\":%u,\"other\":%u},"
                "\"kick1\":{\"ns\":%.1f,\"alongNs\":%.1f,\"baseMs\":%.0f},\"kick2\":{\"state\":\"%s\",\"ms\":%d,\"ns\":%.1f},\"vm100\":{\"along\":%.2f,\"side\":%.2f},\"effMassKg\":%.1f,"
                "\"victim\":{\"fwdCm300\":%.1f,\"sideCm300\":%.1f,\"fwdCm600\":%.1f,\"sideCm600\":%.1f}}",
                x.decision<::rdrvr::vr::handphys::PushDecisionCount?::rdrvr::vr::handphys::pushDecisionNames[x.decision]:"none",double(x.aEff),x.pair?"true":"false",double(x.pairGapMs),
                double(x.v0.length()),double(x.v0Dir),double(x.v0Fwd),double(x.massKg),double(x.vDes),double(x.askedCm),double(x.appliedCm),double(extra),
                unsigned(x.jobRefused[0]),unsigned(x.jobRefused[1]),unsigned(x.jobRefused[2]),unsigned(x.jobRefused[3]),
                double(x.kickJ.length()),double(M::dot(x.kickJ,x.dir)),double(x.kickBase)/1e6,kickStateNames[x.kick2State<KickStateCount?x.kick2State:0],int(x.kick2Ms),double(x.kick2J.length()),
                double(x.vmAlong),double(x.vmSide),double(x.effMass),double(x.fwdCm300),double(x.sideCm300),double(x.fwdCm600),double(x.sideCm600));out+=b;}
        out+="}";}
    out+="]";
}
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
        "\"probe\":{\"hit\":%s,\"instMatch\":%s},\"fx\":%s,\"dry\":%s,\"phantom\":%s,\"grip\":%.2f,\"gripMin300\":%.2f,\"closedMs\":%.0f,\"closeMps\":%.2f},",
        l.ns&&now>=l.ns?double(now-l.ns)/1e6:-1.0,l.hand,modeName(l.mode),l.victimGuid,l.race,l.zone,l.zone>=0&&l.zone<=8?M::zoneNames[unsigned(l.zone)]:"NONE",
        l.bone,l.component,l.source,double(l.speed),double(l.damage),l.flags,double(l.hpBefore),double(l.hpAfter),double(l.msgHpAfter),
        double(l.koBefore),double(l.koAfter),l.kill?"true":"false",l.ko?"true":"false",M::reactionNames[unsigned(std::clamp(l.reaction,0,8))],l.face,
        double(l.hitPos.x),double(l.hitPos.y),double(l.hitPos.z),l.probeHit?"true":"false",l.instMatch?"true":"false",l.fx?"true":"false",l.dry?"true":"false",
        l.phantom?"true":"false",double(l.grip),double(l.gripMin),double(l.closedMs),double(l.closeMps));out+=b;
    std::snprintf(b,sizeof b,"\"fistGate\":{\"waitingSamples\":%llu,\"openSlow\":%llu,\"openHeld\":%llu,\"strikeClosedMsHist\":[%llu,%llu,%llu,%llu],"
        "\"strikeGripMin300Hist\":[%llu,%llu,%llu,%llu]},",(unsigned long long)stats.fistWaiting.load(),(unsigned long long)stats.fistOpenSlow.load(),
        (unsigned long long)stats.fistOpenHeld.load(),(unsigned long long)stats.fistClosedMsHist[0].load(),(unsigned long long)stats.fistClosedMsHist[1].load(),
        (unsigned long long)stats.fistClosedMsHist[2].load(),(unsigned long long)stats.fistClosedMsHist[3].load(),(unsigned long long)stats.fistMinGripHist[0].load(),
        (unsigned long long)stats.fistMinGripHist[1].load(),(unsigned long long)stats.fistMinGripHist[2].load(),(unsigned long long)stats.fistMinGripHist[3].load());out+=b;
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
        const auto rn=stats.hpReadN.load();
        std::snprintf(b,sizeof b,"\"hp\":{\"bodyTicks\":%llu,\"propOnly\":%llu,\"built\":%llu,\"readUsAvg\":%.1f,\"readUsMax\":%.1f,\"horses\":%llu,\"published\":%llu},"
            "\"touch\":{\"ready\":%s,\"thrown\":[%llu,%llu],\"consumed\":%llu,\"stale\":%llu,\"notFound\":%llu,\"handOnly\":%llu,\"gateClosed\":%llu,\"notReady\":%llu,"
            "\"invulnerable\":%llu,\"msgSent\":%llu,\"fakeGuidFail\":%llu,\"slaps\":%llu,\"shoves\":%llu,\"presses\":%llu,\"pushApplied\":%llu,\"pushTicks\":%llu,"
            "\"yawTicks\":%llu,\"offsetCmSum\":%.1f,\"maxPressCm\":%.1f,\"pressArmLimited\":%llu,\"pressArmZero\":%llu,\"refused\":{",
            u(stats.hpBodyTicks),u(stats.hpPropOnly),u(stats.hpBuilt),rn?double(stats.hpReadUsSum.load())/double(rn):0.0,stats.hpReadUsMax.load(),u(stats.hpHorses),u(stats.hpPublished),
            ::rdrvr::vr::handphys::touchReady.load()?"true":"false",u(stats.touchThrown),u(stats.touchThrownSent),u(stats.touchConsumed),u(stats.touchStale),u(stats.touchNotFound),u(stats.touchHandOnly),
            u(stats.touchGateClosed),u(stats.touchNotReady),u(stats.touchInvulnerable),u(stats.touchMsgSent),u(stats.touchFakeGuidFail),u(stats.touchSlaps),
            u(stats.touchShoves),u(stats.touchPresses),u(stats.pushApplied),u(stats.pushTicks),u(stats.yawTicks),double(stats.offsetUmSum.load())/1e4,
            double(stats.maxPressUm.load())/1e4,u(stats.pressArmLimited),u(stats.pressArmZero));out+=b;
        for(unsigned i=0;i<PushRefuseCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",pushRefuseNames[i],u(stats.pushRefused[i]));out+=b;}
        {const auto jm=u(stats.jobsMeasured);
         std::snprintf(b,sizeof b,"},\"mover\":{\"base\":%llu,\"sim\":%llu,\"other\":%llu,\"otherVtRel\":\"0x%llx\",\"headingTurned\":%llu,\"headingLast\":%.3f},"
            "\"offset\":{\"stored\":%llu,\"notStored\":%llu,\"consumed\":%llu,\"notConsumed\":%llu},\"jobs\":{\"measured\":%llu,\"askedCmAvg\":%.1f,"
            "\"movedCmAvg\":%.1f,\"askedLastCm\":%.1f,\"movedLastCm\":%.1f},\"msg\":{\"landed\":%llu,\"notLanded\":%llu",
            u(stats.moverBase),u(stats.moverSim),u(stats.moverOther),(unsigned long long)stats.moverOtherRel.load(),u(stats.headingTurned),double(stats.headingLast.load()),
            u(stats.pushStored),u(stats.pushNotStored),u(stats.pushConsumed),u(stats.pushNotConsumed),jm,jm?double(u(stats.jobAskedUmSum))/double(jm)/1e4:0.0,
            jm?double(u(stats.jobMovedUmSum))/double(jm)/1e4:0.0,double(stats.jobAskedLastCm.load()),double(stats.jobMovedLastCm.load()),u(stats.msgLanded),u(stats.msgNotLanded));out+=b;}
        out+="},\"byReaction\":[";
        for(unsigned i=0;i<9;++i){std::snprintf(b,sizeof b,"%s%llu",i?",":"",u(stats.touchReaction[i]));out+=b;}
        LastTouch lt{};{std::lock_guard lock(touchMutex);lt=lastTouch;}
        std::snprintf(b,sizeof b,"],\"last\":{\"ageMs\":%.0f,\"kind\":\"%s\",\"hand\":%d,\"victimGuid\":%u,\"fakeGuid\":%u,\"reaction\":\"%s\",\"msg\":%s,\"a\":%.2f,"
            "\"distCm\":%.1f,\"hpBefore\":%.1f,\"hpAfter\":%.1f,\"koBefore\":%.1f,\"koAfter\":%.1f,\"gen0\":%d,\"fakeResolved\":%s,\"landed\":%s,"
            "\"animDamage\":%.3f,\"animInjury\":\"0x%x\",\"koDamage\":%.3f}},",lt.ns&&now>=lt.ns?double(now-lt.ns)/1e6:-1.0,
            ::rdrvr::vr::handphys::touchNames[unsigned(lt.kind)&3u],lt.hand,lt.victimGuid,lt.fake,M::reactionNames[unsigned(std::clamp(lt.reaction,0,8))],lt.msg?"true":"false",
            double(lt.a),double(lt.distCm),double(lt.hpBefore),double(lt.hpAfter),double(lt.koBefore),double(lt.koAfter),lt.gen0,lt.fakeResolved?"true":"false",
            lt.landed?"true":"false",double(lt.animDamage),lt.animInjury,double(lt.koDamage));out+=b;
        out.pop_back();out.pop_back();out.pop_back(); // reopen "last" (it ended "}},": last, touch)
        std::snprintf(b,sizeof b,",\"variant\":\"%s\",\"path\":%d,\"node\":\"%s\",\"comp\":%d,\"zone\":%d,\"bone\":\"%s\",\"escalated\":%s,\"windowed\":%s},\"touchMessages\":{\"byVariant\":{",
            lt.attacker>=0&&lt.attacker<int(WatchVariantCount)?watchVariantNames[unsigned(lt.attacker)]:"?",lt.path,lt.node>=0&&lt.node<int(NodeCount)?nodeResultNames[unsigned(lt.node)]:"?",
            lt.comp,lt.zone,lt.bone,lt.escalated?"true":"false",lt.windowed?"true":"false");out+=b;
        static constexpr const char* pathNames[6]{"strike","playerBroadcast","self","fake","none","node"};
        for(unsigned i=0;i<WatchVariantCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",watchVariantNames[i],u(stats.touchByVariant[i]));out+=b;}
        out+="},\"path\":{";
        for(unsigned i=0;i<6;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",pathNames[i],u(stats.touchPath[i]));out+=b;}
        out+="},\"nodeBy\":{";
        for(unsigned i=0;i<NodeCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",nodeResultNames[i],u(stats.nodeBy[i]));out+=b;}
        {::rdrvr::vr::handphys::TouchNodes tn{};::rdrvr::vr::handphys::touchNodes.load(tn);
         std::snprintf(b,sizeof b,"},\"nodeApi\":%s,\"slapNodeSet\":%s,\"pushNodeSet\":%s,",::rdrvr::vr::handphys::touchApi.setNode?"true":"false",tn.slap[0]?"true":"false",
             tn.push[0]?"true":"false");out+=b;}
        std::snprintf(b,sizeof b,"\"selfRefused\":%llu,\"comp\":{\"probe\":%llu,\"bone\":%llu,\"none\":%llu},\"watch\":{\"nodeOk\":%s,\"started\":%llu,\"dropped\":%llu,\"walkBumps\":%llu,",
            u(stats.touchSelfRefused),u(stats.touchProbeComp),u(stats.touchBoneComp),u(stats.touchNoComp),
            ::rdrvr::vr::handphys::touchApi.nodeOk?"true":"false",u(stats.watchStarted),u(stats.watchDropped),u(stats.walkBumps));out+=b;
        for(unsigned k=0;k<WatchKindCount;++k){std::snprintf(b,sizeof b,"%s\"%s\":{",k?",":"",watchKindNames[k]);out+=b;
            for(unsigned a2=0;a2<WatchVariantCount;++a2){std::snprintf(b,sizeof b,"%s\"%s\":[%llu,%llu]",a2?",":"",watchVariantNames[a2],u(stats.watchReacted[k][a2]),u(stats.watchNotReacted[k][a2]));out+=b;}
            out+="}";}
        out+=",\"ring\":[";
        {std::array<NodeWatch,8> ring{};unsigned nx{};{std::lock_guard lock(watchMutex);ring=watchRing;nx=watchNext;}bool first=true;
         const auto sample=[&](const NodeSample& x){
            std::snprintf(b,sizeof b,"{\"ms\":%u,\"ok\":%s,\"id\":\"0x%x\",\"name\":\"%s\",\"path\":\"%s\",\"rooted\":%d,\"parents\":[\"0x%x\",\"0x%x\",\"0x%x\"],\"counter\":%u,\"timeIn\":%.2f,"
                "\"react\":%d,\"ragdoll\":%u,\"busy\":[%d,%d],\"tasks\":[\"0x%x\",\"0x%x\"],\"f112\":\"0x%x\",\"f915\":\"0x%x\"}",unsigned(x.ms),x.ok?"true":"false",x.id,x.name,x.path,
                int(x.rooted),x.parents[0],x.parents[1],x.parents[2],x.counter,double(x.timeIn),x.react,x.ragdoll,int(x.busyA),int(x.busyB),x.tasks[0],x.tasks[1],
                unsigned(x.f112),unsigned(x.f915));out+=b;};
         for(unsigned i=0;i<ring.size();++i){const auto& w=ring[(nx+i)%ring.size()];if(!w.set)continue;
            std::snprintf(b,sizeof b,"%s{\"kind\":\"%s\",\"variant\":\"%s\",\"reacted\":%s,\"before\":",first?"":",",watchKindNames[w.kind<WatchKindCount?w.kind:0],
                watchVariantNames[w.variant<WatchVariantCount?w.variant:0],w.reacted?"true":"false");out+=b;
            sample(w.before);out+=",\"changes\":[";
            for(unsigned k=0;k<w.n&&k<w.tr.size();++k){if(k)out+=",";sample(w.tr[k]);}
            out+="]";
            if(w.push.tier!=0xff){const auto& x=w.push;static constexpr const char* tierText[4]{"light","medium","firm","prop"};
                std::snprintf(b,sizeof b,",\"push\":{\"tier\":\"%s\",\"reaction\":%d,\"face\":%d,\"a\":%.2f,\"depthCm\":%.1f,\"arm\":%.2f,\"impulse\":%.2f,"
                    "\"impulseK\":%.2f,\"impulseSrc\":%d,\"dirFwd\":%.2f,\"headDir\":%.2f,\"bullet\":[%.2f,%.2f,%.2f],\"bulletRead\":%s,\"ragdoll150\":%d,\"fallDot\":%.2f,"
                    "\"movedCm\":%.1f,\"kick\":%s,\"kicked\":%s,\"hpBefore\":%.1f,\"hpAfter\":%.1f}",tierText[x.tier<4?x.tier:0],int(x.reaction),int(x.face),
                    double(x.a),double(x.depthCm),double(x.arm),double(x.impulse),double(x.impulseK),int(x.impulseSrc),double(x.dirFwd),double(x.headDir),double(x.bullet.x),
                    double(x.bullet.y),double(x.bullet.z),x.bulletRead?"true":"false",int(x.ragdoll150),double(x.fallDot),double(x.movedCm),x.kick?"true":"false",
                    x.kicked?"true":"false",double(x.hpBefore),double(x.hpAfter));out+=b;}
            out+="}";first=false;}}
        out+="]}";
        appendPushRing(out,now);
        // the push tiers (light, medium, firm: shoves and the messages they sent), the punch's impulse value, the push impulse's source
        // (lever, the punch's k, the fallback when k reads 0), pushkick kicks, the held prop
        // hits, the meleehit subtree
        std::snprintf(b,sizeof b,",\"tiers\":{\"light\":[%llu,%llu],\"medium\":[%llu,%llu],\"firm\":[%llu,%llu]},\"punchImpulseK\":%.2f,\"pushImpulseBy\":{\"lever\":%llu,\"k\":%llu,\"fallback\":%llu},\"pushKicks\":%llu,"
            "\"propHit\":{\"consumed\":%llu,\"struck\":%llu,\"secondPulse\":%llu,\"skipped\":%llu,\"thrown\":[%llu,%llu],\"scanPoint\":[%llu,%llu],\"refused\":{",u(stats.shoveTier[0]),u(stats.shoveTierMsg[0]),u(stats.shoveTier[1]),
            u(stats.shoveTierMsg[1]),u(stats.shoveTier[2]),u(stats.shoveTierMsg[2]),double(stats.punchImpulseK.load(std::memory_order_relaxed)),
            u(stats.pushImpulseBy[0]),u(stats.pushImpulseBy[1]),u(stats.pushImpulseBy[2]),u(stats.pushKicks),
            u(stats.propConsumed),u(stats.propStruck),u(stats.propSecondPulse),u(stats.propSkipped),u(stats.propThrownConsumed),u(stats.propThrownStruck),
            u(stats.propScanPoint),u(stats.propScanPointArmed));out+=b;
        for(unsigned i=0;i<::rdrvr::vr::handphys::PropHitWhyCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",::rdrvr::vr::handphys::propHitWhyNames[i],
            u(stats.propRefused[i]));out+=b;}
        std::snprintf(b,sizeof b,"}},\"meleeTree\":{\"reads\":%llu,\"nodes\":[",u(stats.meleeTreeReads));out+=b;
        {std::array<TreeNode,32> t{};unsigned tn{};{std::lock_guard lock(treeMutex);t=meleeTree;tn=meleeTreeCount;}
         for(unsigned i=0;i<tn&&i<t.size();++i){char nm[24]{};hashName(t[i].id,nm,sizeof nm);
            std::snprintf(b,sizeof b,"%s[%u,\"%s\",%d]",i?",":"",unsigned(t[i].depth),nm[0]?nm:"?",t[i].children==0xffff?-1:int(t[i].children));out+=b;}}
        // the injury subtree's selection on a knockdown's landed path: per parent its children [hash, type, children, the target's hash
        // of a reference, condition count, [[Match fn offset, +0x8, +0xc, +0x10, +0x14] ...]]
        std::snprintf(b,sizeof b,"]},\"injuryTree\":{\"tree\":%s,\"reads\":%llu,\"refused\":%llu,\"parents\":[",::rdrvr::vr::handphys::touchApi.treeOk?"true":"false",
            u(stats.injuryTreeReads),u(stats.injuryTreeRefused));out+=b;
        {std::array<TreeDump,4> d{};unsigned dn{};{std::lock_guard lock(treeMutex);d=injuryTree;dn=injuryTreeN;}
         for(unsigned i=0;i<dn&&i<d.size();++i){char pn[24]{};hashName(d[i].id,pn,sizeof pn);
            std::snprintf(b,sizeof b,"%s{\"node\":\"%s\",\"count\":%u,\"children\":[",i?",":"",pn[0]?pn:"?",unsigned(d[i].count));out+=b;
            for(unsigned c=0;c<d[i].n&&c<d[i].ch.size();++c){const auto& x=d[i].ch[c];char cn[24]{},rn[24]{};hashName(x.id,cn,sizeof cn);hashName(x.refId,rn,sizeof rn);
                std::snprintf(b,sizeof b,"%s[\"%s\",%u,%d,\"%s\",%u,[",c?",":"",cn[0]?cn:"?",unsigned(x.type),int(x.children),rn[0]?rn:"",unsigned(x.conds));out+=b;
                for(unsigned j=0;j<x.conds&&j<x.c.size();++j){const auto& q=x.c[j];
                    std::snprintf(b,sizeof b,"%s[\"0x%x\",\"0x%x\",\"0x%x\",\"0x%x\",\"0x%x\"]",j?",":"",q.fn,q.w[0],q.w[1],q.w[2],q.w[3]);out+=b;}
                out+="]]";}
            out+="]}";}}
        out+="]}}},";
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
