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
#include "VrMainThread.h"
#include "VrLassoLink.h"
#include "runtime/SpaceWarpActors.h"
#include "VrMotionBones.h"
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
#include <string>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace rdrvr::vr::mvactors {
namespace sw=::rdrvr::runtime::spacewarp;
using ReadFn=bool(*)(void*,std::uintptr_t,void*,std::size_t);
struct Env {ReadFn read{};std::uintptr_t actorManager{},actorSlots{};
    std::uintptr_t entityVtable{},instSkeletonFn{}; // The skeleton chain words (0: no per-bone vectors, roots only)
    explicit operator bool() const noexcept {return read&&actorManager&&actorSlots;}};
inline Env env{};
namespace off {
inline constexpr std::size_t actorGuid=0x8,actorComponent=0xb0,actorFlags=0x118,actorHead=0x120,compMatrix=0x18,compRace=0xa30,matrixPos=0x30,matrixSize=0x40;
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

struct Cached {std::uint32_t guid{},slot{};std::int32_t race{-1};std::uintptr_t matrix{};std::uintptr_t actor{};float pos[3]{};bool havePos{};}; // pos = last root position (bone selection)
struct Scratch {
    std::array<unsigned char,maxSlots*16> slots{};
    std::array<unsigned char,maxScan*off::actorHead> heads{};
    std::array<diagnostics::ReadSegment,maxScan*2> segs{};
    std::array<std::uintptr_t,maxScan> actor{},comp{},mtx{};std::array<std::uint32_t,maxScan> guid{},slot{};std::array<std::int32_t,maxScan> race{};
    std::array<std::array<float,3>,maxScan> pos{};
    // per tick
    std::array<std::array<unsigned char,16>,sw::maxActors> entry{};std::array<std::array<float,16>,sw::maxActors> m{};
    std::array<diagnostics::ReadSegment,sw::maxActors*2+mvbones::maxSegments> tickSegs{}; // 2 per actor + the bone reads
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
        B.actor[q]=B.actor[k];B.slot[q]=i;B.guid[q]=guid;B.comp[q]=comp;++q;
    }
    // stage 2: matrix pointer + race
    for(unsigned k=0;k<q;++k){B.mtx[k]=0;B.race[k]=-1;B.segs[2*k]={B.comp[k]+off::compMatrix,&B.mtx[k],sizeof(std::uintptr_t)};
        B.segs[2*k+1]={B.comp[k]+off::compRace,&B.race[k],sizeof(std::int32_t)};}
    detail::vread(B.segs.data(),2*q,0);
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
    const unsigned keep=std::min<unsigned>(nn,sw::maxActors);unsigned keptMoving=0;
    for(unsigned t=0;t<keep;++t){const unsigned k=nearList[t].k;cache[t]={B.guid[k],B.slot[k],B.race[k],B.mtx[k],B.actor[k],{B.pos[k][0],B.pos[k][1],B.pos[k][2]},true};
        keptMoving+=nearList[t].moving?1u:0u;}
    cacheCount=keep;stats.lastScanned.store(q,std::memory_order_relaxed);
    stats.scanInRange.store(nn,std::memory_order_relaxed);stats.scanMoving.store(moving,std::memory_order_relaxed);
    stats.movingKept.fetch_add(keptMoving,std::memory_order_relaxed);stats.movingDropped.fetch_add(moving-keptMoving,std::memory_order_relaxed);
    lastScan.ns=track?now:0;lastScan.count=0; // Positions only kept while tracking
    if(track)for(unsigned t=0;t<nn&&lastScan.count<maxScan;++t){const unsigned k=nearList[t].k;lastScan.guid[lastScan.count]=B.guid[k];lastScan.pos[lastScan.count++]=B.pos[k];}
    const auto us=(nowNs()-t0)/1000u;stats.scanUs.fetch_add(us,std::memory_order_relaxed);detail::maxOf(stats.scanUsMax,us);
}
inline void tick(const mainthread::Frame& f) noexcept {
    stats.ticks.fetch_add(1,std::memory_order_relaxed);
    if(!lastPropNs||f.now>=lastPropNs+1'000'000'000ull){lastPropNs=f.now;
        enabledProp.store(detail::intProperty("debug.rdrvr.mvactors",1),std::memory_order_relaxed);
        playerProp.store(detail::intProperty("debug.rdrvr.mvactors.player",0),std::memory_order_relaxed);
        movingProp.store(detail::intProperty("debug.rdrvr.mvactors.moving",0),std::memory_order_relaxed);
        mvbones::modeProp.store(std::clamp(detail::intProperty("debug.rdrvr.mvbones",0),0,3),std::memory_order_relaxed);}
    float cam[3]{};
    if(!env||!enabledProp.load(std::memory_order_relaxed)||!sw::actorChannel.camera(cam,f.now)){
        stats.idle.fetch_add(1,std::memory_order_relaxed);
        if(cacheCount||serial){cacheCount=0;sw::actorChannel.clear();serial=0;history={};lastScan.ns=0;lastScan.count=0;} // Also clears the change history / scan positions
        mvbones::reset();
        return;}
    if(!cacheCount||f.now<lastScanNs||f.now-lastScanNs>=rescanNs)rescan(cam,f.now);
    const auto t0=nowNs();auto& B=scratch;
    std::uintptr_t slots{};
    if(!cacheCount||!env.read(nullptr,env.actorSlots,&slots,sizeof slots)||!slots){sw::ActorSet empty{};empty.serial=++serial;sw::actorChannel.publish(empty);return;}
    unsigned n=0;
    for(unsigned i=0;i<cacheCount;++i){B.tickSegs[2*i]={slots+std::uintptr_t(cache[i].slot)*16,B.entry[i].data(),16};
        B.tickSegs[2*i+1]={cache[i].matrix,B.m[i].data(),off::matrixSize};n+=2;}
    // The bone reads of <= 8 planned actors join the same vector read
    const unsigned boneSeg=n;
    {std::array<mvbones::Candidate,sw::maxActors> cand{};
     for(unsigned i=0;i<cacheCount;++i)cand[i]={cache[i].guid,cache[i].race,cache[i].actor,{cache[i].pos[0],cache[i].pos[1],cache[i].pos[2]},cache[i].havePos};
     n+=mvbones::plan(cand.data(),cacheCount,cam,f.now,B.tickSegs.data()+n,unsigned(B.tickSegs.size())-n);}
    if(!detail::vread(B.tickSegs.data(),n,0))stats.frameReadFails.fetch_add(1,std::memory_order_relaxed);
    stats.frameReads.fetch_add(1,std::memory_order_relaxed);
    sw::ActorSet set{};set.serial=++serial;const bool history=measuring(); // Change history = measurement-pack data
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
        if(history)noteHistory(a,set.serial);
    }
    mvbones::finish(set,boneScratch,B.tickSegs.data()+boneSeg,setIndex.data(),f.now); // Parts of the planned actors
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
    out+=",\"bones\":"+mvbones::report()+"}";
    return out;
}
} // namespace rdrvr::vr::mvactors
