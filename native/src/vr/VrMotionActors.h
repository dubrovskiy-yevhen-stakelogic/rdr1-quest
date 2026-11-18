#pragma once
// Actor matrices for the AppSW per-actor motion vectors (runtime/SpaceWarpActors.h).
//
// Main-thread module (ModuleMotionActors, after the simulation step like MELEE / LASSO): only while SPACEWARP produces pairs
// (the render thread refreshed the channel's camera within 0.5 s).
//   * rescan (4 Hz, or when the cache is empty): VrMelee's verified actor table (aGuidGeneral<aActor> slots, 16 B: ptr, u16 gen
//     at +8) -> actor heads (guid +0x8, component +0xb0, flags +0x118) -> component matrix pointer (+0x18) and race (+0xa30)
//     -> matrix translation (+0x30); the nearest maxActors within rangeM of the eye (the player excluded unless
//     debug.rdrvr.mvactors.player 1). Batched like VrLasso::scan (one guarded vector read per stage).
//   * every tick: ONE guarded vector read of the cached actors' slot entries (still the same guid) and root matrices (64 B:
//     a, b, c, d rows of 16 B) -> ActorSet -> actorChannel.
// debug.rdrvr.mvactors 0 = off (empty set: camera-only vectors), re-read once per second.
// debug.rdrvr.mvactors.moving 1 = the rescan keeps MOVING actors first (default 0 = the nearest 24).
// Report: moving / held / alternating, scanInRange / scanMoving / movingKept / movingDropped; these
// are measurement-pack data (debug.rdrvr.perfmetrics, default 1): with perfmetrics 0 and moving 0
// neither the change history nor the scan positions are kept (zero cost).
// VrMotionBones.h adds the skeleton joints of <= 8 near actors in the SAME per-tick vector read
// (crSkeleton header + bone runs), parts attached to the published set (ActorSet::bones); debug.rdrvr.mvbones 0/1/2/3 re-read here at
// 1 Hz; Env + entityVtable / instSkeletonFn (MELEE's words, checked again in TraceEntry); report vr.mvactors.bones.
// Body motion vectors (VrMotionBodyLink.h, props re-read here at 1 Hz, both default 0): every published set carries VrBody hook A's rig map of the
// frame (ActorSet::rig) and, with debug.rdrvr.mvbody 1, John's parts (guid playerBodyGuid); with debug.rdrvr.vr.riding.horseparts 1 the
// mount's chain parts. Report vr.mvactors.mvBody.
// RAGDOLL gate (debug.rdrvr.mvactors.ragdoll 1 [default] / 0, hold debug.rdrvr.mvactors.ragdollhold ms [500], fall window
// debug.rdrvr.mvactors.ragdollwin ms [3000], 0 = the whole ragdoll): an actor whose mover is the sim mover (vtable =
// vr::camera::moverSimVtable, verified by TraceEntry) with its ragdoll word +0xb8c set is published with ActorPose::skip (SpaceWarpActors.h
// ActorRagdoll: camera-only vectors) for the fall window from the word's set, then only within the hold of a root turn over 20 degrees or a
// step over 1 m in one tick, and for the hold after the word clears. HandleHasDied sets the word with no clear of its own, so a corpse or a
// hogtied body is a ragdoll for as long as it lies or is carried: past the window it keeps its own vectors (a body carried on a horse moves
// with it), the turn guard (ActorTurn) still catches a flip. While ragdoll the root matrix this module reads
// (the mover's own, mover +0x40) is rebuilt every physics update from the falling body (mvrMoverSimComponent::PerformPhysicalUpdate
// 0x2892d58 -> mvrMoverInst::Teleport 0x288c1a4: b = up, c = +-the body's flattened c by the sign of its b.y): its yaw flips by up to 180
// degrees in one tick. One 4-byte read per sim mover joins the tick's vector read; the mover's vtable is read at the rescan.
// Report vr.mvactors.ragdoll (episodes, big root moves outside them).
#include "VrMainThread.h"
#include "VrLassoLink.h"
#include "runtime/SpaceWarpActors.h"
#include "VrMotionBones.h"
#include "VrRideSteadyLink.h" // RIDE STEADY: the horse under the eye
#include "VrMotionBodyLink.h" // John's parts + the rig map of the frame (VrBody hook A)
#include "runtime/KgslClock.h" // runtime::measurementPack (debug.rdrvr.perfmetrics) gates the counters
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
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace rdrvr::vr::mvactors {
namespace sw=::rdrvr::runtime::spacewarp;
using ReadFn=bool(*)(void*,std::uintptr_t,void*,std::size_t);
struct Env {ReadFn read{};std::uintptr_t actorManager{},actorSlots{};
    std::uintptr_t entityVtable{},instSkeletonFn{}; // The skeleton chain words (0: no per-bone vectors, roots only)
    const std::atomic<std::uintptr_t>* moverSimWord{}; // vr::camera::moverSimVtable (0 inside: unverified, no ragdoll gate)
    explicit operator bool() const noexcept {return read&&actorManager&&actorSlots;}};
inline Env env{};
namespace off {
inline constexpr std::size_t actorGuid=0x8,actorComponent=0xb0,actorFlags=0x118,actorHead=0x120,compMatrix=0x18,compRace=0xa30,matrixPos=0x30,matrixSize=0x40;
inline constexpr std::size_t actorMover=0x88,moverRagdoll=0xb8c; // mvrMoverSimComponent::BecomeRagdoll 0x2892ecc / HandleHasDied set +0xb8c, ExitSimulated clears it
}
inline constexpr unsigned maxSlots=1024,maxScan=256;
inline constexpr float rangeM=80.f;
inline constexpr std::uint64_t rescanNs=250'000'000ull;

struct Stats {
    std::atomic<std::uint64_t> ticks{},idle{},rescans{},published{},frameReads{},frameReadFails{},dropped{},slotChanged{},scanUs{},frameUs{};
    std::atomic<std::uint64_t> scanUsMax{},frameUsMax{};std::atomic<unsigned> lastCount{},lastScanned{},maxCount{};
    // Is an actor's root matrix rewritten on EVERY main-thread tick? Per published actor and tick: moving = changed since the
    // previous tick; held = unchanged but changed on the previous tick (a stop, or an update every other tick); alternating = the
    // pattern changed / unchanged / changed (timesliced updates: then the rendered NPC itself moves at half rate).
    std::atomic<std::uint64_t> moving{},held{},alternating{};
    // Moving-first selection (rescan): actors in range / moving at the last scan; moving ones kept / dropped (beyond 24), cumulative
    std::atomic<unsigned> scanInRange{},scanMoving{};std::atomic<std::uint64_t> movingKept{},movingDropped{};
};
inline Stats stats{};
inline std::atomic<const char*> installState{"NOT ATTEMPTED"};
inline std::atomic<int> enabledProp{1},playerProp{0},movingProp{0}; // movingProp = debug.rdrvr.mvactors.moving
inline std::atomic<int> ragdollProp{1},ragdollHoldMs{500},ragdollWinMs{3000}; // debug.rdrvr.mvactors.ragdoll / .ragdollhold / .ragdollwin
inline bool measuring() noexcept {return ::rdrvr::runtime::measurementPack.load(std::memory_order_relaxed)!=0;}

namespace detail {
inline int intProperty([[maybe_unused]] const char* name,int fallback) noexcept {
#if defined(__ANDROID__)
    char v[PROP_VALUE_MAX]{};if(__system_property_get(name,v)<=0)return fallback;
    char* end{};const long x=std::strtol(v,&end,10);return end&&end!=v&&!*end?int(x):fallback;
#else
    return fallback;
#endif
}
template<class T> inline T at(const unsigned char* b,std::size_t o) noexcept {T v{};std::memcpy(&v,b+o,sizeof v);return v;}
inline bool vread(diagnostics::ReadSegment* s,unsigned n,int fill) noexcept { // all-or-nothing, then per segment (size 0 = failed)
    if(!n)return true;
    try{if(diagnostics::guardedReadVector.read(env.read,nullptr,s,n))return true;}catch(...){}
    bool all=true;
    for(unsigned k=0;k<n;++k)if(!env.read(nullptr,s[k].source,s[k].target,s[k].size)){std::memset(s[k].target,fill,s[k].size);s[k].size=0;all=false;}
    return all;
}
inline void maxOf(std::atomic<std::uint64_t>& a,std::uint64_t v) noexcept {auto c=a.load(std::memory_order_relaxed);while(v>c&&!a.compare_exchange_weak(c,v,std::memory_order_relaxed)){}}
} // namespace detail

struct Cached {std::uint32_t guid{},slot{};std::int32_t race{-1};std::uintptr_t matrix{};std::uintptr_t actor{};float pos[3]{};bool havePos{};
    std::uintptr_t mover{};bool simMover{};}; // pos = last root position (bone selection); mover = actor +0x88, simMover = its vtable is the sim mover's
struct Scratch {
    std::array<unsigned char,maxSlots*16> slots{};
    std::array<unsigned char,maxScan*off::actorHead> heads{};
    std::array<diagnostics::ReadSegment,maxScan*3> segs{};
    std::array<std::uintptr_t,maxScan> actor{},comp{},mtx{},mover{},moverVt{};std::array<std::uint32_t,maxScan> guid{},slot{};std::array<std::int32_t,maxScan> race{};
    std::array<std::array<float,3>,maxScan> pos{};
    // per tick
    std::array<std::array<unsigned char,16>,sw::maxActors> entry{};std::array<std::array<float,16>,sw::maxActors> m{};
    std::array<std::uint32_t,sw::maxActors> word{};std::array<int,sw::maxActors> wordSeg{}; // the ragdoll words of the sim movers
    std::array<diagnostics::ReadSegment,sw::maxActors*3+mvbones::maxSegments> tickSegs{}; // 2 per actor + 1 per sim mover + the bone reads
};
inline Scratch scratch;            // main thread only
inline sw::BoneSet boneScratch;    // This tick's parts (main thread only; ~10 KB, never on the stack)
inline std::array<Cached,sw::maxActors> cache{};inline unsigned cacheCount{};inline std::uint64_t lastScanNs{},serial{},lastPropNs{}; // main thread only
// The previous rescan's in-range positions by guid (moving-first selection), main thread only.
inline constexpr double movingMps=0.15; // root speed above which an actor counts as moving (walking ~1.4 m/s; idle sway << 0.15)
struct LastScan {std::uint64_t ns{};unsigned count{};std::array<std::uint32_t,maxScan> guid{};std::array<std::array<float,3>,maxScan> pos{};};
inline LastScan lastScan{};
// Change history by guid (survives the 4 Hz rescans), main thread only. bits: bit 0 = changed on this tick, 1 the tick before, 2...
struct History {std::uint32_t guid{};std::uint8_t bits{};std::uint64_t seen{};float m[12]{};};
inline std::array<History,sw::maxActors*2> history{};
inline void noteHistory(const sw::ActorPose& a,std::uint64_t tick) noexcept {
    History* h=nullptr;History* oldest=&history[0];
    for(auto& x:history){if(x.guid==a.guid&&x.seen){h=&x;break;}if(x.seen<oldest->seen)oldest=&x;}
    if(!h||h->seen+1!=tick){if(!h)h=oldest;*h=History{};h->guid=a.guid;h->seen=tick;std::memcpy(h->m,a.m,sizeof h->m);return;} // new / gap: baseline
    const bool changed=std::memcmp(h->m,a.m,sizeof h->m)!=0;
    h->bits=std::uint8_t(((h->bits<<1)|(changed?1u:0u))&7u);h->seen=tick;std::memcpy(h->m,a.m,sizeof h->m);
    if(changed)stats.moving.fetch_add(1,std::memory_order_relaxed);else if(h->bits&2u)stats.held.fetch_add(1,std::memory_order_relaxed);
    if(h->bits==5u)stats.alternating.fetch_add(1,std::memory_order_relaxed);
}

// RAGDOLL: per guid (kept across rescans): the last root matrix and tick, the ragdoll word, the clock it cleared, the open episode.
// An episode = one ragdoll stretch of an actor: its root's step / turn on the first tick, the largest while ragdoll, on the exit tick and
// during the hold, the turns over 90 degrees, the first clean tick after the exit (step <= 0.1, turn <= 10 deg; ms after the exit).
// Engine units are metres (the AppSW pair's unitsPerMetre 1).
// Past the fall window: skipTicks / keptTicks = ragdoll ticks with camera vectors / their own vectors, lastUnstableMs = the last tick of a
// turn over ragUnstableDeg or a step over ragUnstableStepM after the episode's start (ms, -1 none).
struct RagEpisode {std::uint32_t guid{};std::int32_t race{-1};std::uint64_t startNs{},endNs{},exitNs{};std::uint32_t ticks{},flips{},skipTicks{},keptTicks{};
    float enterStepM{-1.f},enterTurnDeg{-1.f},maxStepM{},maxTurnDeg{},exitStepM{-1.f},exitTurnDeg{-1.f},holdMaxStepM{},holdMaxTurnDeg{};std::int32_t firstCleanMs{-1},lastUnstableMs{-1};
    bool open{},exited{};};
struct RagTrack {std::uint32_t guid{};std::uint64_t seen{};float m[12]{};bool rag{};std::uint64_t exitNs{},ragNs{},unstableNs{};RagEpisode ep{};};
inline constexpr float ragUnstableDeg=20.f,ragUnstableStepM=1.f;
struct BigMove {std::uint32_t guid{};std::int32_t race{-1};float stepM{},turnDeg{};bool simMover{};std::uint32_t word{};std::int32_t msSinceExit{-1};std::uint64_t ns{};};
struct RagStats {
    std::atomic<std::uint64_t> ragdollTicks{},holdTicks{},enters{},exits{},readFails{},flips{},flipsRagdoll{},episodes{},bigMoves{};
    std::atomic<std::uint64_t> keptTicks{},unstableTicks{}; // ragdoll ticks past the window with their own vectors; unstable ragdoll ticks
    std::atomic<unsigned> simMovers{},otherMovers{};
    std::atomic<float> maxStepM{},maxTurnDeg{},maxStepRagdollM{},maxTurnRagdollDeg{},maxStepHoldM{},maxTurnHoldDeg{};
    std::atomic<const char*> gate{"NOT STARTED"};
};
inline RagStats ragStats{};
inline std::array<RagTrack,48> ragTracks{};                       // main thread only
inline std::mutex ragRingMutex;inline std::array<RagEpisode,16> ragRing{};inline unsigned ragRingNext{};
inline std::array<BigMove,16> bigRing{};inline unsigned bigRingNext{};  // under ragRingMutex
inline void maxF(std::atomic<float>& a,float v) noexcept {if(!std::isfinite(v))return;auto c=a.load(std::memory_order_relaxed);while(v>c&&!a.compare_exchange_weak(c,v,std::memory_order_relaxed)){}}
// The root's step (engine units) and turn (degrees) between two matrices (rows a, b, c, d of 3).
inline void rootMotion(const float* p,const float* c,float& stepM,float& turnDeg) noexcept {
    const double dx=double(c[9])-p[9],dy=double(c[10])-p[10],dz=double(c[11])-p[11];stepM=float(std::sqrt(dx*dx+dy*dy+dz*dz));
    double tr=0;for(int i=0;i<9;++i)tr+=double(p[i])*c[i]; // trace(Rp^T Rc) over the three rows
    turnDeg=float(std::acos(std::clamp((tr-1.0)/2.0,-1.0,1.0))*57.29577951308232);
}
inline void closeEpisode(RagEpisode& e,std::uint64_t now) noexcept {
    if(!e.open)return;e.endNs=now;e.open=false;ragStats.episodes.fetch_add(1,std::memory_order_relaxed);
    std::lock_guard lock(ragRingMutex);ragRing[ragRingNext]=e;ragRingNext=(ragRingNext+1u)%unsigned(ragRing.size());}
// One published actor of this tick: the skip it gets (0 none, 1 ragdoll, 2 the hold after it). word: the ragdoll word (known = read from a
// sim mover); gate = the skip is applied (else only measured); winNs = the fall window (0: the whole ragdoll).
inline std::uint8_t ragdollStep(std::uint32_t guid,std::int32_t race,const float* m,bool known,std::uint32_t word,bool simMover,bool gate,
                                std::uint64_t tick,std::uint64_t now,std::uint64_t holdNs,std::uint64_t winNs=3'000'000'000ull) noexcept {
    RagTrack* t=nullptr;RagTrack* oldest=&ragTracks[0];
    for(auto& x:ragTracks){if(x.guid==guid&&x.seen){t=&x;break;}if(x.seen<oldest->seen)oldest=&x;}
    bool havePrev=false;
    if(!t){t=oldest;if(t->ep.open)closeEpisode(t->ep,now);*t=RagTrack{};t->guid=guid;}
    else havePrev=t->seen+1==tick;
    float step=-1.f,turn=-1.f;if(havePrev)rootMotion(t->m,m,step,turn);
    std::memcpy(t->m,m,sizeof t->m);t->seen=tick;
    const bool rag=known&&word!=0;auto& e=t->ep;
    if(rag&&!t->rag){ragStats.enters.fetch_add(1,std::memory_order_relaxed);if(e.open)closeEpisode(e,now);
        e=RagEpisode{};e.open=true;e.guid=guid;e.race=race;e.startNs=now;e.enterStepM=step;e.enterTurnDeg=turn;t->exitNs=0;t->ragNs=now?now:1;t->unstableNs=0;}
    if(!rag&&t->rag){ragStats.exits.fetch_add(1,std::memory_order_relaxed);t->exitNs=now?now:1;
        if(e.open){e.exited=true;e.exitNs=t->exitNs;e.exitStepM=step;e.exitTurnDeg=turn;}}
    t->rag=rag;
    const bool hold=!rag&&t->exitNs&&now>=t->exitNs&&now-t->exitNs<holdNs;
    if(havePrev){
        maxF(ragStats.maxStepM,step);maxF(ragStats.maxTurnDeg,turn);if(turn>90.f)ragStats.flips.fetch_add(1,std::memory_order_relaxed);
        if(rag){maxF(ragStats.maxStepRagdollM,step);maxF(ragStats.maxTurnRagdollDeg,turn);
            if(e.open){e.maxStepM=std::max(e.maxStepM,step);e.maxTurnDeg=std::max(e.maxTurnDeg,turn);if(turn>90.f)++e.flips;}
            if(turn>90.f)ragStats.flipsRagdoll.fetch_add(1,std::memory_order_relaxed);}
        else if(hold){maxF(ragStats.maxStepHoldM,step);maxF(ragStats.maxTurnHoldDeg,turn);
            if(e.open){e.holdMaxStepM=std::max(e.holdMaxStepM,step);e.holdMaxTurnDeg=std::max(e.holdMaxTurnDeg,turn);}}
        else if(step>.5f||turn>30.f){ragStats.bigMoves.fetch_add(1,std::memory_order_relaxed);
            BigMove b{};b.guid=guid;b.race=race;b.stepM=step;b.turnDeg=turn;b.simMover=simMover;b.word=word;b.ns=now;
            b.msSinceExit=t->exitNs&&now>=t->exitNs?std::int32_t(std::min<std::uint64_t>((now-t->exitNs)/1'000'000ull,1'000'000'000ull)):-1;
            std::lock_guard lock(ragRingMutex);bigRing[bigRingNext]=b;bigRingNext=(bigRingNext+1u)%unsigned(bigRing.size());}}
    // the fall window, then only around an unstable tick
    bool fall=false;
    if(rag){ragStats.ragdollTicks.fetch_add(1,std::memory_order_relaxed);if(e.open)++e.ticks;
        if(havePrev&&(turn>ragUnstableDeg||step>ragUnstableStepM)){t->unstableNs=now?now:1;ragStats.unstableTicks.fetch_add(1,std::memory_order_relaxed);
            if(e.open&&now>=e.startNs)e.lastUnstableMs=std::int32_t(std::min<std::uint64_t>((now-e.startNs)/1'000'000ull,1'000'000'000ull));}
        const bool inWin=!winNs||(t->ragNs&&now>=t->ragNs&&now-t->ragNs<winNs);
        const bool shaky=t->unstableNs&&now>=t->unstableNs&&now-t->unstableNs<holdNs;
        fall=inWin||shaky;
        if(!inWin&&e.open)++(fall?e.skipTicks:e.keptTicks);
        if(!fall)ragStats.keptTicks.fetch_add(1,std::memory_order_relaxed);}
    if(hold)ragStats.holdTicks.fetch_add(1,std::memory_order_relaxed);
    if(e.open&&e.exited&&!rag&&!hold){ // after the hold: the first clean tick ends the episode (3 s at most)
        if(havePrev&&step<=.1f&&turn<=10.f){e.firstCleanMs=std::int32_t((now-e.exitNs)/1'000'000ull);closeEpisode(e,now);}
        else if(now>=e.exitNs&&now-e.exitNs>3'000'000'000ull)closeEpisode(e,now);}
    return !gate?std::uint8_t(0):rag?std::uint8_t(fall?1:0):hold?std::uint8_t(2):std::uint8_t(0);
}
inline std::uint32_t playerGuid(std::uint64_t now) noexcept {
    lasso::PlayerLink pl{};
    return lasso::playerBox.load(pl)&&pl.valid&&now>=pl.ns&&now-pl.ns<500'000'000ull?pl.guid:0u;
}
inline void rescan(const float cam[3],std::uint64_t now) noexcept {
    const auto t0=nowNs();auto& B=scratch;cacheCount=0;lastScanNs=now;stats.rescans.fetch_add(1,std::memory_order_relaxed);
    std::uintptr_t manager{},slots{};std::uint16_t count{};
    if(!env.read(nullptr,env.actorManager,&manager,sizeof manager)||!manager||!env.read(nullptr,manager,&count,sizeof count)||
       !env.read(nullptr,env.actorSlots,&slots,sizeof slots)||!slots)return;
    const unsigned nSlots=std::min<unsigned>(count,maxSlots);
    if(!nSlots||!env.read(nullptr,slots,B.slots.data(),std::size_t(nSlots)*16))return;
    const std::uint32_t player=playerProp.load(std::memory_order_relaxed)?0u:playerGuid(now);
    // stage 1: actor heads
    unsigned m=0;
    for(unsigned i=0;i<nSlots&&m<maxScan;++i){const auto a=detail::at<std::uintptr_t>(B.slots.data(),std::size_t(i)*16);if(!a)continue;
        B.actor[m]=a;B.slot[m]=i;B.segs[m]={a,B.heads.data()+std::size_t(m)*off::actorHead,off::actorHead};++m;}
    detail::vread(B.segs.data(),m,0);
    unsigned q=0;
    for(unsigned k=0;k<m;++k){
        if(!B.segs[k].size)continue;const unsigned char* h=B.heads.data()+std::size_t(k)*off::actorHead;const unsigned i=B.slot[k];
        const auto guid=detail::at<std::uint32_t>(h,off::actorGuid);const auto gen=detail::at<std::uint16_t>(B.slots.data(),std::size_t(i)*16+8);
        if((guid&0xffffu)!=i||(guid>>16)!=gen||(player&&guid==player)||(detail::at<std::uint32_t>(h,off::actorFlags)&1u))continue;
        const auto comp=detail::at<std::uintptr_t>(h,off::actorComponent);if(!comp)continue;
        B.actor[q]=B.actor[k];B.slot[q]=i;B.guid[q]=guid;B.comp[q]=comp;B.mover[q]=detail::at<std::uintptr_t>(h,off::actorMover);++q;
    }
    // stage 2: matrix pointer + race (+ the mover's vtable: the ragdoll gate)
    const std::uintptr_t simVt=env.moverSimWord?env.moverSimWord->load(std::memory_order_acquire):0;
    unsigned nm=0;std::array<int,maxScan> vtSeg{};
    for(unsigned k=0;k<q;++k){B.mtx[k]=0;B.race[k]=-1;B.moverVt[k]=0;B.segs[2*k]={B.comp[k]+off::compMatrix,&B.mtx[k],sizeof(std::uintptr_t)};
        B.segs[2*k+1]={B.comp[k]+off::compRace,&B.race[k],sizeof(std::int32_t)};}
    for(unsigned k=0;k<q;++k){vtSeg[k]=-1;if(simVt&&B.mover[k]){vtSeg[k]=int(2*q+nm);B.segs[2*q+nm]={B.mover[k],&B.moverVt[k],sizeof(std::uintptr_t)};++nm;}}
    detail::vread(B.segs.data(),2*q+nm,0);
    for(unsigned k=0;k<q;++k)if(vtSeg[k]>=0&&!B.segs[unsigned(vtSeg[k])].size)B.moverVt[k]=0;
    // stage 3: root positions
    unsigned w=0;std::array<unsigned,maxScan> live{};
    for(unsigned k=0;k<q;++k)if(B.segs[2*k].size&&B.mtx[k]){live[w]=k;B.segs[w]={B.mtx[k]+off::matrixPos,B.pos[k].data(),sizeof(float)*3};++w;}
    detail::vread(B.segs.data(),w,0xff);
    // MOVING actors first. In a crowd most of the 24 nearest actors can be 'still' on every pair (no motion block,
    // camera vectors), so nearest-only selection spends the slots on standing NPCs and never tracks walking ones
    // further away. Moving = the root moved > movingMps since the previous scan (same guid, <= 2 s ago);
    // debug.rdrvr.mvactors.moving 0 = nearest only.
    struct Near {unsigned k{};float d2{};bool moving{};};std::array<Near,maxScan> nearList{};unsigned nn=0,moving=0;
    const bool preferMoving=movingProp.load(std::memory_order_relaxed)!=0,track=preferMoving||measuring(); // track = moving detection on
    const double dt=track&&lastScan.ns&&now>lastScan.ns&&now-lastScan.ns<=2'000'000'000ull?double(now-lastScan.ns)*1e-9:0.0;
    for(unsigned j=0;j<w;++j){
        if(!B.segs[j].size)continue;const unsigned k=live[j];const auto& p=B.pos[k];
        if(!std::isfinite(p[0])||!std::isfinite(p[1])||!std::isfinite(p[2])||std::fabs(p[0])>=1e6f||std::fabs(p[1])>=1e6f||std::fabs(p[2])>=1e6f)continue;
        const float dx=p[0]-cam[0],dy=p[1]-cam[1],dz=p[2]-cam[2],d2=dx*dx+dy*dy+dz*dz;if(!(d2<=rangeM*rangeM))continue;
        bool mv=false;
        if(dt>0)for(unsigned i=0;i<lastScan.count;++i)if(lastScan.guid[i]==B.guid[k]){
            const float ex=p[0]-lastScan.pos[i][0],ey=p[1]-lastScan.pos[i][1],ez=p[2]-lastScan.pos[i][2];
            mv=std::sqrt(double(ex)*ex+double(ey)*ey+double(ez)*ez)>movingMps*dt;break;}
        nearList[nn++]={k,d2,mv};moving+=mv?1u:0u;
    }
    std::sort(nearList.begin(),nearList.begin()+nn,[preferMoving](const Near& x,const Near& y){
        return preferMoving&&x.moving!=y.moving?x.moving:x.d2<y.d2;});
    const unsigned keep=std::min<unsigned>(nn,sw::maxActors);unsigned keptMoving=0,sims=0,others=0;
    for(unsigned t=0;t<keep;++t){const unsigned k=nearList[t].k;cache[t]={B.guid[k],B.slot[k],B.race[k],B.mtx[k],B.actor[k],{B.pos[k][0],B.pos[k][1],B.pos[k][2]},true,
        B.mover[k],simVt&&B.moverVt[k]==simVt};
        keptMoving+=nearList[t].moving?1u:0u;sims+=cache[t].simMover?1u:0u;others+=cache[t].simMover?0u:1u;}
    ragStats.simMovers.store(sims,std::memory_order_relaxed);ragStats.otherMovers.store(others,std::memory_order_relaxed);
    cacheCount=keep;stats.lastScanned.store(q,std::memory_order_relaxed);
    stats.scanInRange.store(nn,std::memory_order_relaxed);stats.scanMoving.store(moving,std::memory_order_relaxed);
    stats.movingKept.fetch_add(keptMoving,std::memory_order_relaxed);stats.movingDropped.fetch_add(moving-keptMoving,std::memory_order_relaxed);
    lastScan.ns=track?now:0;lastScan.count=0; // Positions only kept while tracking
    if(track)for(unsigned t=0;t<nn&&lastScan.count<maxScan;++t){const unsigned k=nearList[t].k;lastScan.guid[lastScan.count]=B.guid[k];lastScan.pos[lastScan.count++]=B.pos[k];}
    const auto us=(nowNs()-t0)/1000u;stats.scanUs.fetch_add(us,std::memory_order_relaxed);detail::maxOf(stats.scanUsMax,us);
}
// Body motion vectors (VrMotionBodyLink.h): hook A's record of this frame (published after the previous tick, <= 100 ms old) -> the set's RIG map and,
// with debug.rdrvr.mvbody 1, the player body's BoneSet entry. Rig entries (player body, horse parts) never give way to NPC parts: a full
// set gives up its last NPC entry (that actor keeps its root).
inline std::uint64_t lastRigTickNs{}; // main thread only: the previous tick (a record published before it is not this frame's)
inline int rigSlot(sw::ActorSet& set,sw::BoneSet& bones) noexcept {
    if(bones.count<sw::maxBoneActors)return int(bones.count++);
    for(int k=int(sw::maxBoneActors)-1;k>=0;--k){if(sw::rigPartLayout(bones.bones[unsigned(k)].layout))continue;
        for(unsigned j=0;j<set.count&&j<sw::maxActors;++j)if(set.actor[j].bones==k)set.actor[j].bones=-1;
        ::rdrvr::vr::bodylink::dropNpc.fetch_add(1,std::memory_order_relaxed);return k;}
    return -1;
}
inline void attachBody(sw::ActorSet& set,sw::BoneSet& bones,std::uint64_t now,std::uint64_t since) noexcept {
    namespace bl=::rdrvr::vr::bodylink;
    if(!bl::wanted())return;
    static bl::Record rec{}; // main thread only (~1.4 KB off the stack)
    if(!bl::read(rec)||!rec.ns||rec.ns<=since||rec.ns>now||now-rec.ns>100'000'000ull){bl::stale.fetch_add(1,std::memory_order_relaxed);return;}
    bool finite=true;for(float x:rec.map)finite=finite&&std::isfinite(x)&&std::fabs(x)<1e7f;
    if(!finite)return;
    std::memcpy(set.rig,rec.map,sizeof set.rig);set.rigValid=true;bl::mapsUsed.fetch_add(1,std::memory_order_relaxed);
    if(!bl::bodyProp.load(std::memory_order_relaxed)||rec.parts.count<10||rec.parts.guid!=sw::playerBodyGuid||rec.parts.layout!=sw::playerBodyLayout)return;
    const int k=rigSlot(set,bones);if(k<0)return;
    bones.bones[unsigned(k)]=rec.parts;bones.serial=set.serial;set.boneCount=bones.count;++set.rigParts;bl::bodiesUsed.fetch_add(1,std::memory_order_relaxed);
}
inline void tick(const mainthread::Frame& f) noexcept {
    stats.ticks.fetch_add(1,std::memory_order_relaxed);
    if(!lastPropNs||f.now>=lastPropNs+1'000'000'000ull){lastPropNs=f.now;
        enabledProp.store(detail::intProperty("debug.rdrvr.mvactors",1),std::memory_order_relaxed);
        playerProp.store(detail::intProperty("debug.rdrvr.mvactors.player",0),std::memory_order_relaxed);
        movingProp.store(detail::intProperty("debug.rdrvr.mvactors.moving",0),std::memory_order_relaxed);
        ragdollProp.store(detail::intProperty("debug.rdrvr.mvactors.ragdoll",1)!=0?1:0,std::memory_order_relaxed);
        ragdollHoldMs.store(std::clamp(detail::intProperty("debug.rdrvr.mvactors.ragdollhold",500),0,5000),std::memory_order_relaxed);
        ragdollWinMs.store(std::clamp(detail::intProperty("debug.rdrvr.mvactors.ragdollwin",3000),0,60000),std::memory_order_relaxed);
        mvbones::modeProp.store(std::clamp(detail::intProperty("debug.rdrvr.mvbones",0),0,3),std::memory_order_relaxed);
        namespace bl=::rdrvr::vr::bodylink; // Body motion vectors (VrMotionBodyLink.h): both OFF by default
        bl::bodyProp.store(detail::intProperty("debug.rdrvr.mvbody",0)!=0?1:0,std::memory_order_relaxed);
        bl::horseProp.store(detail::intProperty("debug.rdrvr.vr.riding.horseparts",0)!=0?1:0,std::memory_order_relaxed);}
    float cam[3]{};
    if(!env||!enabledProp.load(std::memory_order_relaxed)||!sw::actorChannel.camera(cam,f.now)){
        stats.idle.fetch_add(1,std::memory_order_relaxed);::rdrvr::vr::steadylink::mountActor.store(0,std::memory_order_relaxed);
        if(cacheCount||serial){cacheCount=0;sw::actorChannel.clear();serial=0;history={};lastScan.ns=0;lastScan.count=0;} // Also clears the change history / scan positions
        mvbones::reset();
        return;}
    if(!cacheCount||f.now<lastScanNs||f.now-lastScanNs>=rescanNs)rescan(cam,f.now);
    const auto t0=nowNs();auto& B=scratch;
    std::uintptr_t slots{};
    if(!cacheCount||!env.read(nullptr,env.actorSlots,&slots,sizeof slots)||!slots){::rdrvr::vr::steadylink::mountActor.store(0,std::memory_order_relaxed);sw::ActorSet empty{};empty.serial=++serial;
        if(::rdrvr::vr::bodylink::wanted()){ // the rig map / player body also with no actor in range (alone on the trail)
            boneScratch.count=0;boneScratch.serial=empty.serial;attachBody(empty,boneScratch,f.now,lastRigTickNs);lastRigTickNs=f.now;
            sw::actorChannel.publish(empty,&boneScratch);return;}
        sw::actorChannel.publish(empty);return;}
    unsigned n=0;
    for(unsigned i=0;i<cacheCount;++i){B.tickSegs[2*i]={slots+std::uintptr_t(cache[i].slot)*16,B.entry[i].data(),16};
        B.tickSegs[2*i+1]={cache[i].matrix,B.m[i].data(),off::matrixSize};n+=2;}
    // RAGDOLL: one 4-byte word per sim mover in the same read
    const bool simWords=env.moverSimWord&&env.moverSimWord->load(std::memory_order_relaxed)!=0;
    for(unsigned i=0;i<cacheCount;++i){B.wordSeg[i]=-1;B.word[i]=0;
        if(simWords&&cache[i].simMover&&cache[i].mover){B.wordSeg[i]=int(n);B.tickSegs[n++]={cache[i].mover+off::moverRagdoll,&B.word[i],sizeof(std::uint32_t)};}}
    // The bone reads of <= 8 planned actors join the same vector read
    const unsigned boneSeg=n;
    {std::array<mvbones::Candidate,sw::maxActors> cand{};
     for(unsigned i=0;i<cacheCount;++i)cand[i]={cache[i].guid,cache[i].race,cache[i].actor,{cache[i].pos[0],cache[i].pos[1],cache[i].pos[2]},cache[i].havePos};
     n+=mvbones::plan(cand.data(),cacheCount,cam,f.now,B.tickSegs.data()+n,unsigned(B.tickSegs.size())-n);}
    if(!detail::vread(B.tickSegs.data(),n,0))stats.frameReadFails.fetch_add(1,std::memory_order_relaxed);
    stats.frameReads.fetch_add(1,std::memory_order_relaxed);
    sw::ActorSet set{};set.serial=++serial;const bool history=measuring(); // Change history = measurement-pack data
    const bool gate=simWords&&ragdollProp.load(std::memory_order_relaxed)!=0;
    ragStats.gate.store(!simWords?"NO WORDS":gate?"ON":"OFF",std::memory_order_relaxed);
    const std::uint64_t holdNs=std::uint64_t(ragdollHoldMs.load(std::memory_order_relaxed))*1'000'000ull;
    const std::uint64_t winNs=std::uint64_t(ragdollWinMs.load(std::memory_order_relaxed))*1'000'000ull;
    std::array<int,sw::maxActors> setIndex{};setIndex.fill(-1); // Cache index -> published index
    for(unsigned i=0;i<cacheCount;++i){
        if(!B.tickSegs[2*i].size||!B.tickSegs[2*i+1].size){stats.dropped.fetch_add(1,std::memory_order_relaxed);continue;}
        // the slot still holds this actor generation (a despawned actor's matrix memory is never trusted)
        if(detail::at<std::uint16_t>(B.entry[i].data(),8)!=(cache[i].guid>>16)||!detail::at<std::uintptr_t>(B.entry[i].data(),0)){
            stats.slotChanged.fetch_add(1,std::memory_order_relaxed);continue;}
        const auto& r=B.m[i];bool ok=true;
        for(int row=0;row<4;++row)for(int c=0;c<3;++c)ok=ok&&std::isfinite(r[row*4+c])&&std::fabs(r[row*4+c])<1e6f; // w lanes are padding
        if(!ok){stats.dropped.fetch_add(1,std::memory_order_relaxed);continue;}
        setIndex[i]=int(set.count);
        auto& a=set.actor[set.count++];a.guid=cache[i].guid;a.race=cache[i].race;
        for(int row=0;row<4;++row)for(int c=0;c<3;++c)a.m[row*3+c]=r[row*4+c];
        for(int c=0;c<3;++c)cache[i].pos[c]=a.m[9+c];cache[i].havePos=true; // The bone selection's distance / root motion
        {const int ws=B.wordSeg[i];const bool known=ws>=0&&B.tickSegs[unsigned(ws)].size!=0;if(ws>=0&&!known)ragStats.readFails.fetch_add(1,std::memory_order_relaxed);
         a.skip=ragdollStep(a.guid,a.race,a.m,known,known?B.word[i]:0u,cache[i].simMover,gate,set.serial,f.now,holdNs,winNs);}
        if(history)noteHistory(a,set.serial);
    }
    mvbones::finish(set,boneScratch,B.tickSegs.data()+boneSeg,setIndex.data(),f.now); // Parts of the planned actors
    { // RIDE STEADY: the horse under the eye (race 1, root within 1.3 m horizontally and 1.0..3.8 m below the eye)
        std::uintptr_t horse=0;float bd=1.69f;
        for(unsigned i=0;i<cacheCount;++i){const int si=setIndex[i];if(si<0)continue;const auto& a=set.actor[unsigned(si)];if(a.race!=1)continue;
            const float dx=a.m[9]-cam[0],dz=a.m[11]-cam[2],dy=cam[1]-a.m[10],d2=dx*dx+dz*dz;
            if(std::isfinite(d2)&&d2<bd&&dy>1.f&&dy<3.8f){bd=d2;horse=cache[i].actor;}}
        ::rdrvr::vr::steadylink::mountActor.store(horse,std::memory_order_relaxed);
        attachBody(set,boneScratch,f.now,lastRigTickNs); // rig map + player body (before the horse: the body never gives way)
        // HORSE PARTS (VrRideSteadyLink.h, debug.rdrvr.vr.riding.horseparts 1): the mount's leg / neck / head capsules from hook A of THIS frame
        // (world, carrier = the engine bone) -> the mount's ActorBones. SpaceWarp::prepareActorPair re-places them on the drawn rig (no rig
        // map in this set = dropped there) and keeps their motion relative to the root.
        namespace sl=::rdrvr::vr::steadylink;static sl::HorseParts hp{}; // main thread only (~1.2 KB off the stack)
        static sw::ActorBones hb{};
        if(::rdrvr::vr::bodylink::horseProp.load(std::memory_order_relaxed)&&horse){
            if(sl::readParts(hp)&&hp.actor==horse&&hp.count&&hp.ns>lastRigTickNs&&f.now>=hp.ns&&f.now-hp.ns<100'000'000ull){
                for(unsigned i=0;i<cacheCount;++i){if(cache[i].actor!=horse)continue;const int si=setIndex[i];if(si<0)break;auto& pose=set.actor[unsigned(si)];
                    if(pose.bones>=0)break; // already has parts (never for a horse today)
                    hb.guid=pose.guid;hb.layout=sw::horsePartLayout;hb.count=0;
                    for(unsigned k=0;k<hp.count&&k<sw::maxActorBones;++k){const auto& s=hp.part[k];if(s.bone<0)continue;auto& p=hb.part[hb.count++];
                        std::memcpy(p.a,s.a,sizeof p.a);std::memcpy(p.b,s.b,sizeof p.b);std::memcpy(p.m,s.m,sizeof p.m);p.radius=s.radius;
                        p.cls=std::uint8_t(sw::ClassHead);p.spec=std::uint8_t(k);p.bone=std::uint16_t(s.bone);}
                    const int k=hb.count?rigSlot(set,boneScratch):-1;if(k<0)break;
                    pose.bones=k;boneScratch.bones[unsigned(k)]=hb;boneScratch.serial=set.serial;set.boneCount=boneScratch.count;++set.rigParts;
                    sl::partsUsed.fetch_add(1,std::memory_order_relaxed);break;}
            }else sl::partsStale.fetch_add(1,std::memory_order_relaxed);}
        lastRigTickNs=f.now;}
    sw::actorChannel.publish(set,&boneScratch);stats.published.fetch_add(1,std::memory_order_relaxed);
    stats.lastCount.store(set.count,std::memory_order_relaxed);
    {auto c=stats.maxCount.load(std::memory_order_relaxed);while(set.count>c&&!stats.maxCount.compare_exchange_weak(c,set.count,std::memory_order_relaxed)){}}
    const auto us=(nowNs()-t0)/1000u;stats.frameUs.fetch_add(us,std::memory_order_relaxed);detail::maxOf(stats.frameUsMax,us);
}
inline void tickThunk(const mainthread::Frame& f) noexcept {tick(f);}
// TraceEntry (arm): the actor-table words were checked against the GOT by the caller.
inline bool install(const Env& e) noexcept {
    if(!e){installState.store("NO ACTOR TABLE");return false;}
    if(!mainthread::executorInstalled()){installState.store("NO EXECUTOR");return false;}
    env=e;
    mvbones::install({e.read,e.entityVtable,e.instSkeletonFn}); // Without the words -> "NO SKELETON WORDS", roots only
    if(!mainthread::registerModule(mainthread::ModuleMotionActors,"mvactors",&tickThunk,nullptr)){installState.store("DUPLICATE MODULE");return false;}
    installState.store("INSTALLED");return true;
}
inline std::string report() {
    char b[1280];const auto ticks=stats.ticks.load(),rescans=stats.rescans.load(),reads=stats.frameReads.load();
    std::snprintf(b,sizeof b,"{\"state\":\"%s\",\"enabled\":%d,\"player\":%d,\"ticks\":%llu,\"idle\":%llu,\"rescans\":%llu,\"published\":%llu,"
        "\"frameReads\":%llu,\"frameReadFails\":%llu,\"dropped\":%llu,\"slotChanged\":%llu,\"lastCount\":%u,\"maxCount\":%u,\"lastScanned\":%u,"
        "\"scanUsAvg\":%.1f,\"scanUsMax\":%llu,\"frameUsAvg\":%.1f,\"frameUsMax\":%llu,\"moving\":%llu,\"held\":%llu,\"alternating\":%llu,"
        "\"movingFirst\":%d,\"scanInRange\":%u,\"scanMoving\":%u,\"movingKept\":%llu,\"movingDropped\":%llu}",
        installState.load(),enabledProp.load(),playerProp.load(),(unsigned long long)ticks,(unsigned long long)stats.idle.load(),(unsigned long long)rescans,
        (unsigned long long)stats.published.load(),(unsigned long long)reads,(unsigned long long)stats.frameReadFails.load(),(unsigned long long)stats.dropped.load(),
        (unsigned long long)stats.slotChanged.load(),stats.lastCount.load(),stats.maxCount.load(),stats.lastScanned.load(),
        rescans?double(stats.scanUs.load())/double(rescans):0.0,(unsigned long long)stats.scanUsMax.load(),
        reads?double(stats.frameUs.load())/double(reads):0.0,(unsigned long long)stats.frameUsMax.load(),
        (unsigned long long)stats.moving.load(),(unsigned long long)stats.held.load(),(unsigned long long)stats.alternating.load(),
        movingProp.load(),stats.scanInRange.load(),stats.scanMoving.load(),(unsigned long long)stats.movingKept.load(),(unsigned long long)stats.movingDropped.load());
    std::string out(b);if(!out.empty()&&out.back()=='}')out.pop_back();
    out+=",\"bones\":"+mvbones::report();
    {const auto& R=ragStats;char m[1024];const auto L=[](const std::atomic<std::uint64_t>& x){return (unsigned long long)x.load(std::memory_order_relaxed);};
     std::snprintf(m,sizeof m,",\"ragdoll\":{\"gate\":\"%s\",\"holdMs\":%d,\"winMs\":%d,\"keptActorTicks\":%llu,\"unstableActorTicks\":%llu,\"simMovers\":%u,\"otherMovers\":%u,\"readFails\":%llu,\"ragdollActorTicks\":%llu,"
        "\"holdActorTicks\":%llu,\"enters\":%llu,\"exits\":%llu,\"episodes\":%llu,\"maxStepM\":%.3f,\"maxTurnDeg\":%.1f,\"maxStepRagdollM\":%.3f,\"maxTurnRagdollDeg\":%.1f,"
        "\"maxStepHoldM\":%.3f,\"maxTurnHoldDeg\":%.1f,\"flips\":%llu,\"flipsRagdoll\":%llu,\"bigMoves\":%llu,\"ring\":[",R.gate.load(),ragdollHoldMs.load(),ragdollWinMs.load(),L(R.keptTicks),L(R.unstableTicks),R.simMovers.load(),
        R.otherMovers.load(),L(R.readFails),L(R.ragdollTicks),L(R.holdTicks),L(R.enters),L(R.exits),L(R.episodes),double(R.maxStepM.load()),double(R.maxTurnDeg.load()),
        double(R.maxStepRagdollM.load()),double(R.maxTurnRagdollDeg.load()),double(R.maxStepHoldM.load()),double(R.maxTurnHoldDeg.load()),L(R.flips),L(R.flipsRagdoll),L(R.bigMoves));out+=m;
     std::array<RagEpisode,16> er{};std::array<BigMove,16> br{};unsigned en{},bn{};
     {std::lock_guard lock(ragRingMutex);er=ragRing;en=ragRingNext;br=bigRing;bn=bigRingNext;}
     const auto now=nowNs();bool first=true;
     for(unsigned i=0;i<er.size();++i){const auto& e=er[(en+i)%er.size()];if(!e.guid)continue;
        std::snprintf(m,sizeof m,"%s{\"guid\":%u,\"race\":%d,\"ageMs\":%.0f,\"durMs\":%.0f,\"ticks\":%u,\"enterStepM\":%.3f,\"enterTurnDeg\":%.1f,\"maxStepM\":%.3f,"
            "\"maxTurnDeg\":%.1f,\"flips\":%u,\"exitStepM\":%.3f,\"exitTurnDeg\":%.1f,\"holdMaxStepM\":%.3f,\"holdMaxTurnDeg\":%.1f,\"firstCleanMsAfterExit\":%d,\"lastUnstableMs\":%d,"
            "\"skipPastWin\":%u,\"keptPastWin\":%u}",
            first?"":",",e.guid,e.race,now>=e.endNs?double(now-e.endNs)/1e6:-1.0,e.exitNs>=e.startNs&&e.exitNs?double(e.exitNs-e.startNs)/1e6:-1.0,e.ticks,
            double(e.enterStepM),double(e.enterTurnDeg),double(e.maxStepM),double(e.maxTurnDeg),e.flips,double(e.exitStepM),double(e.exitTurnDeg),double(e.holdMaxStepM),
            double(e.holdMaxTurnDeg),e.firstCleanMs,e.lastUnstableMs,e.skipTicks,e.keptTicks);out+=m;first=false;}
     out+="],\"bigMoves\":[";first=true;
     for(unsigned i=0;i<br.size();++i){const auto& b=br[(bn+i)%br.size()];if(!b.guid)continue;
        std::snprintf(m,sizeof m,"%s{\"guid\":%u,\"race\":%d,\"stepM\":%.3f,\"turnDeg\":%.1f,\"simMover\":%s,\"word\":%u,\"msSinceExit\":%d,\"ageMs\":%.0f}",
            first?"":",",b.guid,b.race,double(b.stepM),double(b.turnDeg),b.simMover?"true":"false",b.word,b.msSinceExit,now>=b.ns?double(now-b.ns)/1e6:-1.0);out+=m;first=false;}
     out+="]}";}
    {namespace bl=::rdrvr::vr::bodylink;namespace sl=::rdrvr::vr::steadylink;char m[400]; // what the ticks attached (VrMotionBodyLink.h)
     std::snprintf(m,sizeof m,",\"mvBody\":{\"body\":%d,\"horseParts\":%d,\"mapsUsed\":%llu,\"bodiesUsed\":%llu,\"stale\":%llu,\"dropNpc\":%llu,\"horseUsed\":%llu,\"horseStale\":%llu}",
        bl::bodyProp.load(),bl::horseProp.load(),(unsigned long long)bl::mapsUsed.load(),(unsigned long long)bl::bodiesUsed.load(),(unsigned long long)bl::stale.load(),
        (unsigned long long)bl::dropNpc.load(),(unsigned long long)sl::partsUsed.load(),(unsigned long long)sl::partsStale.load());out+=m;}
    out+="}";
    return out;
}
} // namespace rdrvr::vr::mvactors
