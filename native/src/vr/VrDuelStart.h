#pragma once
// DUEL ON LOOK (CHEATS row, default OFF, never saved).
// Look at a person, hold LT + press Y (VrDuelLink.h): the game's own duel starts with that person as the opponent.
//
// ROUTE = what the game's duel challengers do. Every duel of the game goes through the mini game script
// content/Mini_Games/Dueling/Dueling (the only callers of aniDuel::RequestDuel 0x2166bc8 are the natives BeginDuel 0x242337c / AddDueler,
// called from that script; content_android.rpf). The challengers (town scripts, poker) start it with RDRStartNewScript(path, args, 0x20, 0)
// and a 0x80 byte argument block. That native needs a calling script thread only for its parent context (rage::scrThread current +0x38),
// so the same three engine calls are made here without a parent, as mbMailBox::PopulateChallengeParams 0x27ef2e8 and
// NetScriptEvent_StartNewScript::Apply 0x27e2dec do for their root scripts:
//   sagCoreScript::FindAbsoluteScriptPath 0x1beca88 ("content..." never reads the context) -> "$/content/Mini_Games/Dueling/Dueling"
//   sagFullScriptContext::CreateNewScriptContext 0x1c063ec (self and parent are never read; root list, pool, stack 0x600, args copied)
//   sagScriptContext::RequestScript 0x1c04520 (path, -12): sagScriptContext::Update 0x1c04dd0 mounts it with the args once it is resident.
// The script then runs exactly as in the game: streams the anim sets, tasks the crowd, BeginDuel (RequestDuel on both actors = their
// aniDuel state, actor +0x38 +0xab8 +0x20 +0x30, the context holsters::duelActive reads), draw, result, cleanup, end.
// The argument block, read back from the callers' bytecode (the mission form of the poker script, no town tutorial):
//   +0x00 player, +0x08 opponent (gohGUID = actor +0x188, what the script natives resolve), +0x10 / +0x1c their positions,
//   +0x28 stay-out volume, +0x30 tutorial 0, +0x34 hostage 0, +0x44 100, +0x50 level (0 EASY, 1 MID, 2 HARD anim set), +0x54 -2.
// Like the callers: a stay-out volume (type 2, at the opponent, 10 x 10 x 15, heading to the player) from rdr2GOH::CreateVolumeInLayout
// 0x247f4c4 in a layout of the mod (cinterface::CreateNamedLayout 0x24d2c5c), destroyed here once the script context is gone
// (DestroyObject<gohVolume> 0x2497a34: guid-checked); the opponent gets weapon id 4 like theirs (GiveEnumeratedWeapon(a, 4, 0, true, true)), the player too if he owns no sidearm.
//
// Threads: main thread only (VrMainThread, the UpdateInputs wrapper in sagShellDerived::Execute: the thread that runs
// sagShellDerived::Update = UpdateClass / UpdateAllScripts). Scripts are not running there (scrThread current = null between updates).
// Verified: every called function and the layout witnesses by a whole-function FNV-1a of the running code (install(); the hash, not the
// bytes: nothing to regenerate in gamecode), GOT words of the guid tables and PARAM_noscriptstreaming. Anything else: row UNAVAILABLE.
// Refused without an engine call: menu open, not on foot, dead, Dead Eye, a duel (aniDuel state) or this script still running, no person
// in the cone. A target that dies after the press is the script's own check (it ends: "Dueling - Terminating Exit Flag Set").
// Levers: debug.rdrvr.vr.cheats.duel.cone (deg, 10), .range (m, 25), .min (m, 7: closer = the opponent is placed that far behind his
// spot, 0 = keep it), .level (0..2, 0). Report vr.cheats.duelStart.
#include "VrDuelLink.h"
#include "VrMelee.h"
#include "VrMenuPages.h"
#include "VrHaptics.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace rdrvr::vr::cheats::duel {
using ReadFn=bool(*)(void*,std::uintptr_t,void*,std::size_t);
// rage::scrVector by value: three floats (an HFA: s0..s2, s3..s5, the third one on the stack), as CreateVolumeInLayout reads them.
struct ScrVector {float x,y,z;};
using FindPathFn=bool(*)(const char* path,char* out,void* context);
using NewContextFn=void*(*)(void* self,const char* path,void* parent,const void* args,int bytes);
using RequestScriptFn=void(*)(const char* path,float priority);
using NamedLayoutFn=std::uint32_t(*)(const char* name);
using CreateVolumeFn=std::uint32_t(*)(std::uint32_t layout,const char* name,int type,ScrVector position,ScrVector rotation,ScrVector size);
using DestroyVolumeFn=void(*)(std::uint32_t guid);
using GiveFn=void(*)(void* actor,int weaponEnum,float bias,bool,bool equip);
using HasFn=bool(*)(const void* actor,int weaponEnum);

struct Print {std::uintptr_t offset;std::uint32_t size;std::uint64_t fnv;};
inline constexpr Print findPathPrint{0x1beca88,0x148,0xb146d7b92c561442ull},newContextPrint{0x1c063ec,0x114,0x6f5009f96e704135ull},
    requestScriptPrint{0x1c04520,0x4d0,0x3e7e82efa3086f5cull},namedLayoutPrint{0x24d2c5c,0x1c,0x997d1589d0d17d38ull},
    createLayoutPrint{0x24bb6e0,0x1a8,0x22b19806f0288cbbull},createVolumePrint{0x247f4c4,0x164,0xf41c388522f8bd9cull},
    destroyVolumePrint{0x2497a34,0x138,0x49d7fb838a9d7ba4ull};
// Layout witnesses, never called: RDRStartNewScript (the sequence and context +0x374), Update (+0x370 / +0x378 / +0x170), CheckIsDueling (the
// duel state path), BeginDuel / RequestDuel (what the script calls), the GetPlayerActor native (actor +0x188 = the script's handle).
inline constexpr std::array<Print,6> witnessPrints{{{0x1becc20,0x118,0x4ea6adcfc7625759ull},{0x1c04dd0,0x178,0xa757cde07632da24ull},
    {0x26ab7a0,0x40,0xef72e9a0b1aaa1b4ull},{0x242337c,0x3d0,0xb4c36e8c228631b2ull},{0x2166bc8,0x2bc,0xb5687d3dfb65a81full},{0x1cb21f0,0xa0,0x21bcf9176b360412ull}}};
inline constexpr std::uintptr_t contextManagerGotOffset=0x2e01298,contextManagerOffset=0x4e6ecfe0,contextSlotsGotOffset=0x2e012a0,contextSlotsOffset=0x4e6ecfc8,
    noStreamingGotOffset=0x2e012f8,noStreamingOffset=0x4e6ed398;
namespace off {
inline constexpr std::size_t actorGuid=0x8,actorAnim=0x38,actorHealth=0x60,actorComponent=0xb0,actorFlags=0x118,actorGoh=0x188,
    compMatrix=0x18,compIsCreatureSlot=0xc8,compRiderLink=0x894,compMountLink=0x8a0,compRace=0xa30,matrixPos=0x30,healthValue=0x20,healthFlags=0x150,
    animBlock=0xab8,blockDuel=0x20,duelState=0x30,playerMode=0x5d4,contextGuid=0x374,paramSet=0x18;
inline constexpr std::uint32_t healthDead=0x40;
}
// race table sagCreatureType 0x2e84e38: 0 Human, 1 Horse. duelWeapon: the id the challengers give (script 0x6aa0eaf2(opponent, 4, 0, 1, 1)).
inline constexpr int raceHuman=0,duelWeapon=4;
inline constexpr unsigned maxSlots=1024;
inline constexpr const char* scriptPath="content/Mini_Games/Dueling/Dueling";
inline constexpr const char* layoutName="rdrvr_duel_layout";

struct ScriptArgs {
    std::uint32_t player{},pad0{},opponent{},pad1{};
    float playerPos[3]{},opponentPos[3]{};
    std::uint32_t volume{},pad2{},tutorial{},hostage{},hostageActor{},pad3{};
    std::int32_t field40{},field44{100};
    std::uint32_t field48{},pad4{};
    std::int32_t level{},actorEnum{-2};
    unsigned char scriptOwned[0x20]{};
    std::int32_t field78{},field7c{};
};
static_assert(sizeof(ScriptArgs)==0x80&&offsetof(ScriptArgs,volume)==0x28&&offsetof(ScriptArgs,field44)==0x44&&offsetof(ScriptArgs,level)==0x50&&
    offsetof(ScriptArgs,scriptOwned)==0x58&&offsetof(ScriptArgs,field78)==0x78,"the Dueling script's argument block");

// ---- state: main thread writes, menu / report read ----
inline std::atomic<bool> available{};
inline std::atomic<std::uint64_t> presses{},started{},refused{},scans{},volumesDestroyed{},weaponsGiven{},opponentMoved{},scriptEnds{};
inline std::atomic<const char*> lastError{""},phase{"IDLE"};
inline std::atomic<std::uint32_t> lastGuid{},lastGoh{};
inline std::atomic<int> lastRace{-1},duelState{-1},hint{0};
inline std::atomic<float> lastDistance{-1.f},lastAngle{-1.f},hintDistance{-1.f},startSeconds{-1.f};
inline std::atomic<bool> duelActiveAfter{},scriptRunning{};

namespace detail {
struct Api {
    ReadFn read{};std::uintptr_t contextManager{},contextSlots{},noStreaming{};
    FindPathFn findPath{};NewContextFn newContext{};RequestScriptFn requestScript{};NamedLayoutFn namedLayout{};CreateVolumeFn createVolume{};DestroyVolumeFn destroyVolume{};
    std::uintptr_t actorManager{},actorSlots{},isCreatureFn{};
};
inline Api api{};
struct Running {std::uint32_t context{},volume{};std::uintptr_t player{};std::uint64_t startNs{};bool seen{};};
inline Running running{};
template<class T> inline bool rd(std::uintptr_t p,std::size_t o,T& out) noexcept {return p&&api.read&&api.read(nullptr,p+o,&out,sizeof out);}
inline float prop(const char* name,float fallback,float lo,float hi) noexcept {
#if defined(__ANDROID__)
    char v[PROP_VALUE_MAX]{};if(__system_property_get(name,v)>0){char* end{};const float f=std::strtof(v,&end);if(end!=v&&std::isfinite(f))return std::clamp(f,lo,hi);}
#else
    (void)name;(void)lo;(void)hi;
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
inline bool scriptAlive(std::uint32_t guid) noexcept {
    std::uintptr_t manager{},slots{};std::uint16_t count{},generation{};
    return guid&&rd(api.contextManager,0,manager)&&rd(manager,0,count)&&(guid&0xffffu)<count&&rd(api.contextSlots,0,slots)&&
        rd(slots,std::size_t(guid&0xffffu)*16+8,generation)&&generation==(guid>>16);
}
inline int readDuelState(std::uintptr_t actor) noexcept {
    std::uintptr_t anim{},block{},duel{};std::int32_t state{-1};
    return rd(actor,off::actorAnim,anim)&&anim&&rd(anim,off::animBlock,block)&&block&&rd(block,off::blockDuel,duel)&&duel&&rd(duel,off::duelState,state)?int(state):-1;
}
inline bool rootOf(std::uintptr_t actor,std::array<float,3>& out) noexcept {
    std::uintptr_t comp{},matrix{};float p[3]{};
    if(!rd(actor,off::actorComponent,comp)||!comp||!rd(comp,off::compMatrix,matrix)||!matrix||!api.read(nullptr,matrix+off::matrixPos,p,sizeof p))return false;
    for(unsigned i=0;i<3;++i){if(!std::isfinite(p[i])||std::fabs(p[i])>=1e6f)return false;out[i]=p[i];}
    return true;
}
struct Target {std::uintptr_t actor{};std::uint32_t guid{},goh{};std::int32_t race{-1};float distance{},angle{};std::array<float,3> root{};};
struct Gaze {std::array<float,3> origin{},forward{};float units{1};};
// The clock is read here, not the frame's: commands run after every module tick, so a plan the XR thread published since the frame began is
// newer than Frame::now and vrMeleeWorldMap rejects it (now < publishedNs). One retry for a plan published between the two lines.
inline bool gaze(Gaze& g) noexcept {
    namespace ml=::rdrvr::vr::melee;
    const auto fn=ml::worldMapFn.load(std::memory_order_acquire);if(!fn)return false;
    ml::WorldMap m{};if((!fn(m,::rdrvr::vr::nowNs())&&!fn(m,::rdrvr::vr::nowNs()))||!m.valid||!(m.units>0.f))return false;
    const auto f=ml::rotate(m,m.headForwardLocal);const float len=std::sqrt(f.x*f.x+f.y*f.y+f.z*f.z);if(!(len>.5f)||!std::isfinite(len))return false;
    g.origin={m.headWorld.x,m.headWorld.y,m.headWorld.z};g.forward={f.x/len,f.y/len,f.z/len};g.units=m.units;
    return std::isfinite(g.origin[0]+g.origin[1]+g.origin[2]);
}
// The living human closest to the gaze ray inside the cone: feet / chest / head of each actor against the ray. No LOS test (no verified
// ray call on this path yet): a person seen through a wall can be picked. A rider (component +0x8a0 = a mount whose +0x894 names him
// back, the links VrMelee reads) is not a target.
inline bool pick(const Gaze& g,std::uintptr_t player,std::uint32_t playerGuid,float coneDeg,float rangeM,Target& out) noexcept {
    scans.fetch_add(1,std::memory_order_relaxed);
    static std::array<unsigned char,std::size_t(maxSlots)*16> table;
    std::uintptr_t manager{},slots{};std::uint16_t count{};
    if(!rd(api.actorManager,0,manager)||!rd(manager,0,count)||!rd(api.actorSlots,0,slots)||!slots)return false;
    const unsigned n=std::min<unsigned>(count,maxSlots);if(!n||!api.read(nullptr,slots,table.data(),std::size_t(n)*16))return false;
    const float cosCone=std::cos(coneDeg*0.0174532925f);bool found=false;
    for(unsigned i=0;i<n;++i){
        std::uintptr_t actor{};std::uint16_t generation{};std::memcpy(&actor,table.data()+std::size_t(i)*16,sizeof actor);
        std::memcpy(&generation,table.data()+std::size_t(i)*16+8,sizeof generation);
        if(!actor||actor==player)continue;
        std::array<float,3> root{};if(!rootOf(actor,root))continue;
        float best=-2.f,distance=0.f;
        for(const float up:{.3f,1.1f,1.6f}){const float v[3]{root[0]-g.origin[0],root[1]+up*g.units-g.origin[1],root[2]-g.origin[2]};
            const float len=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);if(!(len>.01f))continue;
            const float c=(v[0]*g.forward[0]+v[1]*g.forward[1]+v[2]*g.forward[2])/len;if(c>best){best=c;distance=len;}}
        const float metres=distance/g.units;
        if(best<cosCone||metres>rangeM||metres<.5f)continue;
        std::uint32_t guid{},flags{},goh{},hpFlags{},mountGuid{};std::uintptr_t comp{},health{};std::int32_t race{-1};float hp{};
        if(!rd(actor,off::actorGuid,guid)||(guid&0xffffu)!=i||(guid>>16)!=generation||guid==playerGuid||!rd(actor,off::actorFlags,flags)||(flags&1u))continue;
        if(!rd(actor,off::actorGoh,goh)||!goh||goh==0xffffffffu)continue;
        // A creature component first (the VrMelee test): +0xa30 of anything else (a wagon, a train) is not a race and often reads 0 = human
        std::uintptr_t compVt{},isCreature{};
        if(!rd(actor,off::actorComponent,comp)||!rd(comp,0,compVt)||!rd(compVt,off::compIsCreatureSlot,isCreature)||isCreature!=api.isCreatureFn)continue;
        if(!rd(comp,off::compRace,race)||race!=raceHuman)continue;
        if(!rd(comp,off::compMountLink,mountGuid))continue;
        if(mountGuid&&(mountGuid&0xffffu)<n){std::uintptr_t mount{},mountComp{};std::uint32_t rider{};
            std::memcpy(&mount,table.data()+std::size_t(mountGuid&0xffffu)*16,sizeof mount);
            if(mount&&rd(mount,off::actorComponent,mountComp)&&mountComp&&rd(mountComp,off::compRiderLink,rider)&&rider==guid)continue;}
        if(!rd(actor,off::actorHealth,health)||!rd(health,off::healthValue,hp)||!rd(health,off::healthFlags,hpFlags)||!(hp>0.f)||(hpFlags&off::healthDead))continue;
        const float angle=std::acos(std::clamp(best,-1.f,1.f))*57.29578f;
        if(!found||angle<out.angle){out=Target{actor,guid,goh,race,metres,angle,root};found=true;}
    }
    return found;
}
inline void finishRunning() noexcept {
    if(running.volume&&api.destroyVolume){api.destroyVolume(running.volume);volumesDestroyed.fetch_add(1,std::memory_order_relaxed);}
    running=Running{};scriptRunning.store(false,std::memory_order_relaxed);scriptEnds.fetch_add(1,std::memory_order_relaxed);
    phase.store("ENDED",std::memory_order_relaxed);
}
} // namespace detail

// Main tick (cheats module, 8 Hz-ish, ON or running only): the running script's state; the row's hint while the menu shows it. No reads otherwise.
inline void tick(std::uint64_t now,std::uint64_t frame,bool inGame,std::uintptr_t player,std::uint32_t playerGuid) noexcept {
    using namespace detail;
    if(!available.load(std::memory_order_relaxed))return;
    if(running.context){
        const int state=readDuelState(running.player);duelState.store(state,std::memory_order_relaxed);
        if(state>0&&!running.seen){running.seen=true;duelActiveAfter.store(true,std::memory_order_relaxed);phase.store("DUEL",std::memory_order_relaxed);
            startSeconds.store(float(double(now-running.startNs)/1e9),std::memory_order_relaxed);}
        if(inGame&&!scriptAlive(running.context))finishRunning();
    }
    if(!duellink::enabled.load(std::memory_order_relaxed)||!::rdrvr::vr::menuOpen.load(std::memory_order_relaxed)||(frame&15u)||!inGame||!player)return;
    Gaze g{};Target t{};
    if(gaze(g)&&pick(g,player,playerGuid,prop("debug.rdrvr.vr.cheats.duel.cone",10.f,1.f,45.f),prop("debug.rdrvr.vr.cheats.duel.range",25.f,2.f,100.f),t)){
        hintDistance.store(t.distance,std::memory_order_relaxed);hint.store(2,std::memory_order_relaxed);}
    else hint.store(1,std::memory_order_relaxed);
}

// Main thread. A press refused before any look at the target: counted, reported and felt like the others.
inline const char* refuseEarly(std::uint64_t now,const char* why) noexcept {
    presses.fetch_add(1,std::memory_order_relaxed);refused.fetch_add(1,std::memory_order_relaxed);lastError.store(why,std::memory_order_relaxed);
    ::rdrvr::vr::haptics::pulse(0,.35f,40'000,now);return why;
}
// Main thread (cheats execute, OpDuel). nullptr = started; else the refusal in panel glyphs. armed: the player owns a sidearm (or the list is not known yet).
inline const char* press(std::uint64_t now,std::uintptr_t playerObject,std::uintptr_t actor,std::uint32_t playerGuid,float hp,bool deadEye,bool armed,GiveFn give,HasFn has) noexcept {
    using namespace detail;
    presses.fetch_add(1,std::memory_order_relaxed);
    const auto refuse=[&](const char* why)noexcept{refused.fetch_add(1,std::memory_order_relaxed);lastError.store(why,std::memory_order_relaxed);
        ::rdrvr::vr::haptics::pulse(0,.35f,40'000,now);return why;};
    if(!available.load(std::memory_order_relaxed))return refuse("UNAVAILABLE");
    if(!duellink::enabled.load(std::memory_order_relaxed))return refuse("DUEL ON LOOK IS OFF");
    if(::rdrvr::vr::menuOpen.load(std::memory_order_relaxed))return refuse("MENU OPEN");
    std::uint32_t mode{};if(!rd(playerObject,off::playerMode,mode)||mode!=2u)return refuse("NOT ON FOOT");
    if(!(hp>0.f))return refuse("NOT ALIVE");
    if(deadEye)return refuse("DEAD EYE ACTIVE");
    if(readDuelState(actor)!=0)return refuse("A DUEL IS RUNNING");
    if(running.context&&scriptAlive(running.context))return refuse("A DUEL IS STARTING");
    if(running.context)finishRunning();
    std::uintptr_t noStreaming{};if(!rd(api.noStreaming,off::paramSet,noStreaming)||noStreaming)return refuse("SCRIPT STREAMING OFF");
    Gaze g{};if(!gaze(g))return refuse("NO HEAD POSE");
    Target t{};
    if(!pick(g,actor,playerGuid,prop("debug.rdrvr.vr.cheats.duel.cone",10.f,1.f,45.f),prop("debug.rdrvr.vr.cheats.duel.range",25.f,2.f,100.f),t))return refuse("NO NPC IN SIGHT");
    lastGuid.store(t.guid,std::memory_order_relaxed);lastGoh.store(t.goh,std::memory_order_relaxed);lastRace.store(t.race,std::memory_order_relaxed);
    lastDistance.store(t.distance,std::memory_order_relaxed);lastAngle.store(t.angle,std::memory_order_relaxed);
    // BeginDuel / RequestDuel use the opponent's aniDuel (actor +0x38 +0xab8 +0x20) unchecked: it must exist and be idle
    if(readDuelState(t.actor)!=0)return refuse("HE CANNOT DUEL NOW");
    std::array<float,3> me{};std::uint32_t playerGoh{};if(!rootOf(actor,me)||!rd(actor,off::actorGoh,playerGoh)||!playerGoh||playerGoh==0xffffffffu)return refuse("NO PLAYER HANDLE");
    char path[256]{};if(!api.findPath(scriptPath,path,nullptr)||!path[0])return refuse("NO DUEL SCRIPT");
    // The two spots: where they stand; a person closer than .min is placed that far along the player -> him line (level, the mover settles him).
    float dx=t.root[0]-me[0],dz=t.root[2]-me[2];const float flat=std::sqrt(dx*dx+dz*dz);
    const float minUnits=prop("debug.rdrvr.vr.cheats.duel.min",7.f,0.f,30.f)*g.units;
    std::array<float,3> spot=t.root;
    if(flat>.01f&&flat<minUnits){spot[0]=me[0]+dx/flat*minUnits;spot[2]=me[2]+dz/flat*minUnits;opponentMoved.fetch_add(1,std::memory_order_relaxed);}
    dx=me[0]-spot[0];dz=me[2]-spot[2];
    const std::uint32_t layout=api.namedLayout(layoutName);if(!layout)return refuse("NO LAYOUT");
    // heading = the script's heading native 0x1bef1cc on (player - opponent): atan2(-x, -z) in degrees; the volume takes degrees
    const float heading=std::atan2(-dx,-dz)*57.29578f;
    static std::uint32_t nameCounter{};char volumeName[32];std::snprintf(volumeName,sizeof volumeName,"Duel_StayOut_%u",++nameCounter);
    const std::uint32_t volume=api.createVolume(layout,volumeName,2,ScrVector{spot[0],spot[1],spot[2]},ScrVector{0.f,heading,0.f},ScrVector{10.f,10.f,15.f});
    if(!volume)return refuse("NO VOLUME");
    ScriptArgs a{};a.player=playerGoh;a.opponent=t.goh;a.volume=volume;a.level=int(prop("debug.rdrvr.vr.cheats.duel.level",0.f,0.f,2.f));
    for(unsigned i=0;i<3;++i){a.playerPos[i]=me[i];a.opponentPos[i]=spot[i];}
    void* context=api.newContext(nullptr,path,nullptr,&a,int(sizeof a));
    if(!context){api.destroyVolume(volume);return refuse("NO SCRIPT CONTEXT");}
    api.requestScript(path,-12.f);
    if(give&&has){
        if(!has(reinterpret_cast<const void*>(t.actor),duelWeapon)){give(reinterpret_cast<void*>(t.actor),duelWeapon,0.f,true,true);weaponsGiven.fetch_add(1,std::memory_order_relaxed);}
        if(!armed){give(reinterpret_cast<void*>(actor),duelWeapon,0.f,true,true);weaponsGiven.fetch_add(1,std::memory_order_relaxed);}
    }
    std::uint32_t contextGuid{};rd(reinterpret_cast<std::uintptr_t>(context),off::contextGuid,contextGuid);
    running=Running{contextGuid,volume,actor,now,false};
    started.fetch_add(1,std::memory_order_relaxed);lastError.store("",std::memory_order_relaxed);duelActiveAfter.store(false,std::memory_order_relaxed);
    startSeconds.store(-1.f,std::memory_order_relaxed);scriptRunning.store(true,std::memory_order_relaxed);phase.store("SCRIPT REQUESTED",std::memory_order_relaxed);
    ::rdrvr::vr::haptics::pulse(1,.9f,150'000,now);
    return nullptr;
}

// Menu row value (XR thread).
inline void rowText(char* out,std::size_t size) noexcept {
    if(!duellink::enabled.load(std::memory_order_relaxed)){std::snprintf(out,size,"OFF");return;}
    // No plus sign here: CheatsPage sanitizes row and footer to the panel glyphs and would draw it as '?'.
    if(scriptRunning.load(std::memory_order_relaxed)){std::snprintf(out,size,"ON - LT AND Y - DUEL RUNNING");return;}
    const int h=hint.load(std::memory_order_relaxed);
    if(h==2)std::snprintf(out,size,"ON - LT AND Y - NPC %.0f M",double(hintDistance.load(std::memory_order_relaxed)));
    else if(h==1)std::snprintf(out,size,"ON - LT AND Y - NO NPC IN SIGHT");
    else std::snprintf(out,size,"ON - LT AND Y");
}
inline void toggle(bool on) noexcept {
    duellink::enabled.store(on,std::memory_order_release);hint.store(0,std::memory_order_relaxed);
    ::rdrvr::vr::cheatNote.store(on?"LOOK AT A PERSON: HOLD LT - PRESS Y":nullptr,std::memory_order_release);
}

// Startup (arm thread, from cheats::install): nothing is called before every print and GOT word checked out.
template<class Got>
inline void install(ReadFn read,std::uintptr_t base,std::uintptr_t actorManager,std::uintptr_t actorSlots,Got&& got) noexcept {
    using namespace detail;
    api=Api{};api.read=read;api.actorManager=actorManager;api.actorSlots=actorSlots;api.isCreatureFn=base+::rdrvr::vr::melee::isCreatureOffset;
    const auto ok=[&](const Print& p){return read&&codeHash(base+p.offset,p.size)==p.fnv;};
    bool all=read&&actorManager&&actorSlots&&ok(findPathPrint)&&ok(newContextPrint)&&ok(requestScriptPrint)&&ok(namedLayoutPrint)&&ok(createLayoutPrint)&&
        ok(createVolumePrint)&&ok(destroyVolumePrint)&&got(contextManagerGotOffset,contextManagerOffset)&&got(contextSlotsGotOffset,contextSlotsOffset)&&
        got(noStreamingGotOffset,noStreamingOffset);
    for(const auto& p:witnessPrints)all=all&&ok(p);
    if(!all){phase.store("ENGINE MISMATCH",std::memory_order_relaxed);return;}
    api.contextManager=base+contextManagerOffset;api.contextSlots=base+contextSlotsOffset;api.noStreaming=base+noStreamingOffset;
    api.findPath=reinterpret_cast<FindPathFn>(base+findPathPrint.offset);api.newContext=reinterpret_cast<NewContextFn>(base+newContextPrint.offset);
    api.requestScript=reinterpret_cast<RequestScriptFn>(base+requestScriptPrint.offset);api.namedLayout=reinterpret_cast<NamedLayoutFn>(base+namedLayoutPrint.offset);
    api.createVolume=reinterpret_cast<CreateVolumeFn>(base+createVolumePrint.offset);api.destroyVolume=reinterpret_cast<DestroyVolumeFn>(base+destroyVolumePrint.offset);
    available.store(true,std::memory_order_release);
}

// Report object vr.cheats.duelStart (no leading comma).
inline std::string report() {
    char b[960];
    std::snprintf(b,sizeof b,"{\"available\":%s,\"enabled\":%s,\"presses\":%llu,\"chords\":%llu,\"postFailures\":%llu,\"started\":%llu,\"refused\":%llu,"
        "\"lastTarget\":{\"guid\":%u,\"goh\":%u,\"race\":%d,\"distanceM\":%.2f,\"angleDeg\":%.2f},\"lastError\":\"%s\",\"phase\":\"%s\",\"scriptRunning\":%s,"
        "\"duelActiveAfter\":%s,\"duelState\":%d,\"secondsToDuel\":%.2f,\"scans\":%llu,\"opponentMoved\":%llu,\"weaponsGiven\":%llu,\"volumesDestroyed\":%llu,\"scriptEnds\":%llu}",
        available.load()?"true":"false",duellink::enabled.load()?"true":"false",(unsigned long long)presses.load(),(unsigned long long)duellink::chords.load(),
        (unsigned long long)duellink::postFailures.load(),(unsigned long long)started.load(),(unsigned long long)refused.load(),lastGuid.load(),lastGoh.load(),lastRace.load(),
        double(lastDistance.load()),double(lastAngle.load()),lastError.load(),phase.load(),scriptRunning.load()?"true":"false",duelActiveAfter.load()?"true":"false",
        duelState.load(),double(startSeconds.load()),(unsigned long long)scans.load(),(unsigned long long)opponentMoved.load(),(unsigned long long)weaponsGiven.load(),
        (unsigned long long)volumesDestroyed.load(),(unsigned long long)scriptEnds.load());
    return b;
}
} // namespace rdrvr::vr::cheats::duel
