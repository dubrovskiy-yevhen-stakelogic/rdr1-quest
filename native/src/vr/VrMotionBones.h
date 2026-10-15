#pragma once
// Main-thread skeleton reads for the AppSW PER-BONE motion vectors (runtime/SpaceWarpBones.h). Called by
// vr/VrMotionActors.h inside its ModuleMotionActors tick (engine main thread, sagShellDerived::UpdateInputs wrapper: after
// the frame's Update and the DMZ, i.e. the skeleton globals the end-of-frame MatrixSetUpdate task skinned from).
//   * chain (per guid, 32 entries): actor+0x50 entity (vtable == entEntity 0x2dbf008+0x10, MELEE's GOT-checked word) -> +0x88 inst
//     (vtable slot +0x1f0 == entEntityInst::GetSkeleton 0x2913174: vtable word 0x2dbf308 + whole-function print, checked in
//     TraceEntry) -> frag byte +0x190 -> +0x88 cache -> +0x1a0 crSkeleton: +0x8 data, +0x20 bone count (== data+0x30 u16), +0x28
//     globals (stride 0x40, WORLD-space Matrix34 rows a, b, c, d).
//     Same chain as VrBody::readSkeleton / VrMelee::deriveChain. Re-derived after 2 s or when the per-tick header check fails.
//   * layout (per skeleton data, 16 cached): bone names (data+0x0 -> bones, stride 0x110, name pointer +0x0) -> sw::BoneKeyMap (exact
//     names: head neck spine01 spine03 pelvis arm/elbow/wrist/hip/knee/ankle/toe _l/_r); humanoid only (horses, animals: root only,
//     counted); the needed bones are merged into <= 6 contiguous runs (gap <= 3 bones, <= 48 bones: the human skeleton is 4 runs /
//     ~32 bones = 2 KB per actor).
//   * per tick (inside the actors' ONE guarded vector read): for <= 8 planned actors the crSkeleton header (skel+0x0..0x30) and the
//     runs; then sw::makeParts() (sanity: finite, <= 3 m from the root, segments <= 1.2 m) -> ActorSet::bones + ActorPose::bones.
// Selection: race 0 actors of the published set within sw::boneRangeM (25 m) of the eye with a humanoid layout, MOVING first (root > 2 mm
// or a part endpoint > 1 cm per tick within the last 0.5 s), then the nearest; mode 2 (PIN): the nearest only.
// debug.rdrvr.mvbones (re-read at 1 Hz by VrMotionActors::tick): 0 = off (no chain, no read: root only)
// [default: limb capsules can take coat / torso pixels of dressed actors], 1 = on, 2 = PIN (standing actors
// get parts and blocks too, so an AppSW dump shows every near NPC's capsules), 3 = on with TIGHT capsules
// (SpaceWarpBones.h prepareBoneParts; selection as 1). Report vr.mvactors.bones.
#include "runtime/SpaceWarpActors.h"
#include "diagnostics/GuardedReadVector.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace rdrvr::vr::mvbones {
namespace sw=::rdrvr::runtime::spacewarp;
using ReadFn=bool(*)(void*,std::uintptr_t,void*,std::size_t);
struct Env {ReadFn read{};std::uintptr_t entityVtable{},instSkeletonFn{};explicit operator bool() const noexcept {return read&&entityVtable&&instSkeletonFn;}};
inline Env env{};
namespace off {
inline constexpr std::size_t actorEntity=0x50,entityInst=0x88,instCache=0x88,instIsFrag=0x190,instSkeletonSlot=0x1f0,cacheSkeleton=0x1a0;
inline constexpr std::size_t instTypeA=0x78,instTypeB=0x98,typeBits=0xc;inline constexpr std::uint32_t typeResident=0x30000000; // fragInst::GetSkeleton
inline constexpr std::size_t skelData=0x8,skelCount=0x20,skelGlobals=0x28,skelHeader=0x30,dataBones=0x0,dataCount=0x30,boneStride=0x110,boneName=0x0,
    globalStride=0x40;
}
inline constexpr unsigned maxBones=256,maxLayouts=16,maxChains=32,maxRuns=6,maxRunBones=48,maxDerivePerTick=2;
inline constexpr unsigned maxSegments=sw::maxBoneActors*(1+maxRuns); // what plan() may append to the tick's vector read
inline constexpr std::uint64_t chainMaxAgeNs=2'000'000'000ull,movingHoldNs=500'000'000ull,retryNs=1'000'000'000ull;
inline constexpr float movingM=0.002f,movingPartM=0.01f; // root / part endpoint step per tick that counts as moving (idle breathing < 1 cm)

struct Layout {std::uintptr_t data{};std::uint32_t count{},id{};bool humanoid{};sw::BoneKeyMap keys{};unsigned runs{},bones{};std::uint64_t usedNs{};
    std::array<std::uint16_t,maxRuns> runStart{},runLen{};std::array<std::int16_t,sw::KeyCount> slot{};};
struct Chain {std::uint32_t guid{};std::uintptr_t actor{},skel{},data{},globals{};std::uint32_t count{};int layout{-1};
    std::uint64_t ns{},seenNs{},movingNs{},failNs{};float root[3]{};std::array<std::array<float,3>,sw::maxActorBones> tip{};unsigned tips{};bool ok{},haveRoot{};};
struct Stats {
    std::atomic<std::uint64_t> ticks{},chains{},chainFails{},layouts{},humanoid{},nonHumanoid{},layoutFails{},layoutEvictions{},planned{},published{},parts{},
        headerMismatch{},sanityDrops{},readFails{},farSkipped{},capSkipped{},raceSkipped{},us{},usMax{};
    std::atomic<unsigned> lastActors{},maxActors{},lastParts{},lastCandidates{},lastMoving{};
};
inline Stats stats{};
inline std::atomic<int> modeProp{0}; // default 0 = off
inline std::atomic<const char*> state{"NOT ATTEMPTED"};
inline std::atomic<std::uint32_t> firstLayoutKeys[sw::KeyCount]{}; // report: bone index + 1 of each key of the first humanoid layout (0 missing)

// ------------------------------------------------------------------------------------------ main thread state
struct Planned {unsigned cache{};int chain{};unsigned seg{},segs{};};
struct State {
    std::array<Layout,maxLayouts> layout{};unsigned layouts{};
    std::array<Chain,maxChains> chain{};
    std::array<Planned,sw::maxBoneActors> plan{};unsigned planned{};
    std::array<std::array<unsigned char,off::skelHeader>,sw::maxBoneActors> header{};
    std::array<std::array<unsigned char,maxRunBones*off::globalStride>,sw::maxBoneActors> buf{};
    bool cleared{true};
};
inline State st{}; // main thread only

namespace detail {
inline std::uint64_t nowNs() noexcept {return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
template<class T> inline bool get(std::uintptr_t base,std::size_t o,T& out) noexcept {
    return env.read&&base&&base<=UINTPTR_MAX-o-sizeof(T)&&env.read(nullptr,base+o,&out,sizeof(T));}
template<class T> inline T at(const unsigned char* b,std::size_t o) noexcept {T v{};std::memcpy(&v,b+o,sizeof v);return v;}
inline bool vread(diagnostics::ReadSegment* s,unsigned n) noexcept { // all-or-nothing, then per segment (size 0 = failed)
    if(!n)return true;
    try{if(diagnostics::guardedReadVector.read(env.read,nullptr,s,n))return true;}catch(...){}
    bool all=true;
    for(unsigned k=0;k<n;++k)if(!env.read(nullptr,s[k].source,s[k].target,s[k].size)){std::memset(s[k].target,0,s[k].size);s[k].size=0;all=false;}
    return all;
}
inline void maxOf(std::atomic<std::uint64_t>& a,std::uint64_t v) noexcept {auto c=a.load(std::memory_order_relaxed);while(v>c&&!a.compare_exchange_weak(c,v,std::memory_order_relaxed)){}}
inline void bump(std::atomic<std::uint64_t>& a,std::uint64_t n=1) noexcept {a.fetch_add(n,std::memory_order_relaxed);}
} // namespace detail

// Merge the needed bone indices into runs (gap <= 3 bones, <= maxRunBones in all, <= maxRuns). false = not representable.
inline bool planRuns(const sw::BoneKeyMap& keys,std::uint32_t count,Layout& l) noexcept {
    std::array<std::int16_t,sw::KeyCount> idx{};unsigned n=0;
    for(unsigned k=0;k<sw::KeyCount;++k)if(keys.has(k)&&std::uint32_t(keys.index[k])<count)idx[n++]=keys.index[k];
    std::sort(idx.begin(),idx.begin()+n);n=unsigned(std::unique(idx.begin(),idx.begin()+n)-idx.begin());
    l.runs=0;l.bones=0;l.slot.fill(-1);
    for(unsigned i=0;i<n;){
        unsigned j=i;while(j+1<n&&idx[j+1]-idx[j]<=4)++j; // gap <= 3 bones in between
        const unsigned len=unsigned(idx[j]-idx[i]+1);
        if(l.runs>=maxRuns||l.bones+len>maxRunBones)return false;
        l.runStart[l.runs]=std::uint16_t(idx[i]);l.runLen[l.runs]=std::uint16_t(len);++l.runs;l.bones+=len;i=j+1;
    }
    // key -> slot in the actor's buffer (runs back to back)
    for(unsigned k=0;k<sw::KeyCount;++k){if(!keys.has(k))continue;unsigned base=0;
        for(unsigned r=0;r<l.runs;++r){const int b=keys.index[k];if(b>=l.runStart[r]&&b<l.runStart[r]+l.runLen[r]){l.slot[k]=std::int16_t(base+unsigned(b-l.runStart[r]));break;}base+=l.runLen[r];}}
    return l.runs>0;
}
// Bone names of a skeleton data block -> layout (once per data pointer; bounded: one new layout per tick).
// A full table evicts the least recently used layout (its chains re-derive). Skeleton data blocks are per
// TYPE in RDR1 (MELEE's layout cache holds 2 for all NPCs); eviction only matters for per-actor blocks.
inline int layoutFor(std::uintptr_t data,std::uint32_t count,std::uint64_t now) noexcept {
    for(unsigned i=0;i<st.layouts;++i)if(st.layout[i].data==data&&st.layout[i].count==count){st.layout[i].usedNs=now;return int(i);}
    if(count>maxBones){detail::bump(stats.layoutFails);return -1;}
    unsigned slot=st.layouts;
    if(slot>=maxLayouts){slot=0;for(unsigned i=1;i<maxLayouts;++i)if(st.layout[i].usedNs<st.layout[slot].usedNs)slot=i;
        for(auto& c:st.chain)if(c.layout==int(slot)){c.ok=false;c.layout=-1;}
        detail::bump(stats.layoutEvictions);}
    std::uintptr_t bones{};if(!detail::get(data,off::dataBones,bones)||!bones){detail::bump(stats.layoutFails);return -1;}
    static std::array<std::uintptr_t,maxBones> names{};static std::array<diagnostics::ReadSegment,maxBones> s{};
    static std::array<std::array<char,32>,maxBones> text{};std::array<const char*,maxBones> ptrs{};
    for(unsigned i=0;i<count;++i){names[i]=0;s[i]={bones+std::uintptr_t(i)*off::boneStride+off::boneName,&names[i],8};}
    if(!detail::vread(s.data(),count)){detail::bump(stats.layoutFails);return -1;}
    unsigned m=0;for(unsigned i=0;i<count;++i){text[i].fill(0);if(names[i])s[m++]={names[i],text[i].data(),24};}
    if(!detail::vread(s.data(),m)) // a name near the end of a mapping: 24 bytes may fail, retry those with 8
        for(unsigned k=0;k<m;++k)if(!s[k].size){std::uint64_t w{};if(detail::get(s[k].source,0,w))std::memcpy(s[k].target,&w,8);}
    for(unsigned i=0;i<count;++i){text[i][24]=0;ptrs[i]=text[i][0]?text[i].data():nullptr;}
    Layout l{};l.data=data;l.count=count;l.id=std::uint32_t((data>>4)^(std::uint64_t(count)*2654435761u))|1u;
    l.keys=sw::resolveBoneKeys(ptrs.data(),count);l.humanoid=l.keys.humanoid()&&planRuns(l.keys,count,l);l.usedNs=now;
    detail::bump(stats.layouts);detail::bump(l.humanoid?stats.humanoid:stats.nonHumanoid);
    if(l.humanoid&&!firstLayoutKeys[sw::KeyPelvis].load(std::memory_order_relaxed))
        for(unsigned k=0;k<sw::KeyCount;++k)firstLayoutKeys[k].store(l.keys.has(k)?std::uint32_t(l.keys.index[k]+1):0u,std::memory_order_relaxed);
    st.layout[slot]=l;if(slot==st.layouts)++st.layouts;return int(slot);
}
// actor -> crSkeleton: entEntityInst::GetSkeleton 0x2913174 (ldrb [x0,#0x190]; cbz -> null; b fragInst::GetSkeleton) and
// fragInst::GetSkeleton 0x1ea3ddc (cache = [x0,#0x88]; type: [x0,#0x78] with bits 0x30000000 at +0xc and a non-null first word, else
// [x0,#0x98]; null type -> null; skeleton = [cache,#0x1a0]) replicated with guarded reads (VrBody::readSkeleton / VrMelee::deriveChain).
inline bool deriveChain(std::uintptr_t actor,Chain& c) noexcept {
    std::uintptr_t ent{},vt{},inst{},slot{},cache{},typeA{},type{};std::uint8_t frag{};std::uint16_t dataCount{};std::uint32_t bits{};
    c.ok=false;c.skel=c.data=c.globals=0;c.count=0;
    if(!detail::get(actor,off::actorEntity,ent)||!ent||!detail::get(ent,0,vt)||vt!=env.entityVtable)return false;
    if(!detail::get(ent,off::entityInst,inst)||!inst||!detail::get(inst,0,vt)||!vt||!detail::get(vt,off::instSkeletonSlot,slot)||slot!=env.instSkeletonFn)return false;
    if(!detail::get(inst,off::instIsFrag,frag)||!frag||!detail::get(inst,off::instCache,cache)||!cache||!detail::get(inst,off::instTypeA,typeA))return false;
    if(typeA){if(!detail::get(typeA,off::typeBits,bits)||(bits&off::typeResident)!=off::typeResident||!detail::get(typeA,0,type)||!type)return false;}
    else if(!detail::get(inst,off::instTypeB,type)||!type)return false;
    if(!detail::get(cache,off::cacheSkeleton,c.skel)||!c.skel)return false;
    if(!detail::get(c.skel,off::skelData,c.data)||!c.data||!detail::get(c.skel,off::skelGlobals,c.globals)||!c.globals||
       !detail::get(c.skel,off::skelCount,c.count)||!detail::get(c.data,off::dataCount,dataCount))return false;
    return c.count>0&&c.count<=maxBones&&dataCount==c.count;
}
inline Chain* chainOf(std::uint32_t guid,bool create,std::uint64_t now) noexcept {
    Chain* oldest=&st.chain[0];
    for(auto& c:st.chain){if(c.guid==guid&&c.seenNs)return &c;if(c.seenNs<oldest->seenNs)oldest=&c;}
    if(!create)return nullptr;
    *oldest=Chain{};oldest->guid=guid;oldest->seenNs=now;return oldest;
}
inline void reset() noexcept {if(st.cleared)return;st.chain={};st.planned=0;st.cleared=true;} // layouts stay (data pointers of skeleton types)

// One published actor of the tick as plan() needs it (VrMotionActors' cache entry).
struct Candidate {std::uint32_t guid{};std::int32_t race{-1};std::uintptr_t actor{};float pos[3]{};bool havePos{};};
// Plans this tick's bone reads: appends <= maxSegments segments (crSkeleton header + runs per planned actor) to segs. Returns how many.
inline unsigned plan(const Candidate* cand,unsigned count,const float cam[3],std::uint64_t now,diagnostics::ReadSegment* segs,unsigned room) noexcept {
    const auto t0=detail::nowNs();st.planned=0;const int mode=modeProp.load(std::memory_order_relaxed);
    if(!env||!mode){reset();return 0;}
    st.cleared=false;detail::bump(stats.ticks);
    struct Near {unsigned i{};int chain{};float score{};bool moving{};};std::array<Near,sw::maxActors> list{};unsigned nl=0,derived=0,moving=0;
    const float range2=sw::boneRangeM*sw::boneRangeM;
    for(unsigned i=0;i<count&&i<sw::maxActors;++i){
        const auto& a=cand[i];if(!a.havePos||!a.actor)continue;
        if(a.race!=0){detail::bump(stats.raceSkipped);continue;}
        const float dx=a.pos[0]-cam[0],dy=a.pos[1]-cam[1],dz=a.pos[2]-cam[2],d2=dx*dx+dy*dy+dz*dz;
        if(!(d2<=range2)){detail::bump(stats.farSkipped);continue;}
        Chain* c=chainOf(a.guid,true,now);c->seenNs=now;
        const bool usable=c->ok&&c->actor==a.actor;                                  // an aged chain stays usable until re-derived
        if(!usable||now-c->ns>chainMaxAgeNs){
            const bool backoff=c->failNs&&now-c->failNs<retryNs&&c->actor==a.actor;
            if(derived<maxDerivePerTick&&!backoff){
                ++derived;c->actor=a.actor;c->ns=now;c->tips=0;
                if(!deriveChain(a.actor,*c)){c->failNs=now;c->layout=-1;detail::bump(stats.chainFails);continue;}
                c->layout=layoutFor(c->data,c->count,now);c->failNs=0;detail::bump(stats.chains);
                if(c->layout<0){c->ok=false;c->failNs=now;continue;}
                c->ok=true;
            }else if(!usable)continue;
        }
        if(c->layout<0||!st.layout[unsigned(c->layout)].humanoid)continue;
        if(c->haveRoot){const float ex=a.pos[0]-c->root[0],ey=a.pos[1]-c->root[1],ez=a.pos[2]-c->root[2];if(ex*ex+ey*ey+ez*ez>movingM*movingM)c->movingNs=now;}
        std::memcpy(c->root,a.pos,sizeof c->root);c->haveRoot=true;
        const bool mv=c->movingNs&&now-c->movingNs<movingHoldNs;moving+=mv?1u:0u;
        list[nl++]={i,int(c-st.chain.data()),mode==2||mv?d2:d2+1e6f,mv};
    }
    std::sort(list.begin(),list.begin()+nl,[](const Near& x,const Near& y){return x.score<y.score;});
    stats.lastCandidates.store(nl,std::memory_order_relaxed);stats.lastMoving.store(moving,std::memory_order_relaxed);
    unsigned used=0;
    for(unsigned t=0;t<nl;++t){
        if(st.planned>=sw::maxBoneActors){detail::bump(stats.capSkipped,nl-t);break;}
        const auto& c=st.chain[unsigned(list[t].chain)];if(!c.ok||c.layout<0)continue; // evicted by a later candidate's new layout
        auto& l=st.layout[unsigned(c.layout)];l.usedNs=now; // LRU: only the planned actors' layouts stay young
        if(used+1+l.runs>room){detail::bump(stats.capSkipped,nl-t);break;}
        auto& p=st.plan[st.planned];p.cache=list[t].i;p.chain=list[t].chain;p.seg=used;p.segs=1+l.runs;
        segs[used++]={c.skel,st.header[st.planned].data(),off::skelHeader};
        unsigned base=0;
        for(unsigned r=0;r<l.runs;++r){segs[used++]={c.globals+std::uintptr_t(l.runStart[r])*off::globalStride,st.buf[st.planned].data()+std::size_t(base)*off::globalStride,
            std::size_t(l.runLen[r])*off::globalStride};base+=l.runLen[r];}
        ++st.planned;
    }
    detail::bump(stats.planned,st.planned);
    const auto us=(detail::nowNs()-t0)/1000u;detail::bump(stats.us,us);detail::maxOf(stats.usMax,us);
    return used;
}
// After the tick's vector read: segs = the same array plan() appended to (sizes 0 = failed); setIndex[cache index] = the actor's index in
// `set` (-1: not published this tick). The parts go to `out` (serial = set.serial; published with the set: ActorChannel::publish(set, &out)),
// ActorPose::bones = the index in out, set.boneCount = out.count.
inline void finish(sw::ActorSet& set,sw::BoneSet& out,const diagnostics::ReadSegment* segs,const int* setIndex,std::uint64_t now) noexcept {
    const auto t0=detail::nowNs();const int mode=modeProp.load(std::memory_order_relaxed);
    set.boneCount=0;set.boneMode=unsigned(std::clamp(mode,0,3));out.serial=set.serial;out.count=0;
    if(!env||!mode)return;
    unsigned parts=0;
    for(unsigned q=0;q<st.planned;++q){
        const auto& p=st.plan[q];const int si=setIndex[p.cache];if(si<0||unsigned(si)>=set.count)continue;
        auto& c=st.chain[unsigned(p.chain)];if(!c.ok||c.layout<0)continue;const auto& l=st.layout[unsigned(c.layout)];
        bool read=true;for(unsigned k=0;k<p.segs;++k)read=read&&segs[p.seg+k].size;
        if(!read){detail::bump(stats.readFails);c.ok=false;continue;}
        const unsigned char* h=st.header[q].data();
        if(detail::at<std::uintptr_t>(h,off::skelData)!=c.data||detail::at<std::uintptr_t>(h,off::skelGlobals)!=c.globals||detail::at<std::uint32_t>(h,off::skelCount)!=c.count){
            detail::bump(stats.headerMismatch);c.ok=false;continue;} // re-derived next tick
        std::array<std::array<float,12>,sw::KeyCount> km{};std::array<const float*,sw::KeyCount> mp{};std::array<std::int16_t,sw::KeyCount> bi{};bi.fill(-1);
        for(unsigned k=0;k<sw::KeyCount;++k){const int s=l.slot[k];if(s<0)continue;
            const unsigned char* g=st.buf[q].data()+std::size_t(s)*off::globalStride;
            for(int row=0;row<4;++row)for(int col=0;col<3;++col)km[k][std::size_t(row*3+col)]=detail::at<float>(g,std::size_t(row*16+col*4)); // w lanes are padding
            mp[k]=km[k].data();bi[k]=l.keys.index[k];}
        auto& pose=set.actor[unsigned(si)];sw::ActorBones b{};
        const float root[3]{pose.m[9],pose.m[10],pose.m[11]};
        if(sw::makeParts(mp,bi,root,1.f,b)<10||out.count>=sw::maxBoneActors){detail::bump(stats.sanityDrops);continue;}
        b.guid=pose.guid;b.layout=l.id;
        // moving: any part endpoint moved > 1 cm since the last read (gestures of a standing NPC count, idle breathing does not)
        bool mv=false; // the first read (or a part count change) is a baseline, not motion
        if(c.tips==b.count)for(unsigned k=0;k<b.count&&!mv;++k){const float ex=b.part[k].a[0]-c.tip[k][0],ey=b.part[k].a[1]-c.tip[k][1],ez=b.part[k].a[2]-c.tip[k][2];
            mv=ex*ex+ey*ey+ez*ez>movingPartM*movingPartM;}
        if(mv)c.movingNs=now;
        for(unsigned k=0;k<b.count;++k)std::memcpy(c.tip[k].data(),b.part[k].a,sizeof(float)*3);c.tips=b.count;
        pose.bones=std::int32_t(out.count);out.bones[out.count++]=b;parts+=b.count;
    }
    set.boneCount=out.count;detail::bump(stats.published,set.boneCount);detail::bump(stats.parts,parts);
    stats.lastActors.store(set.boneCount,std::memory_order_relaxed);stats.lastParts.store(parts,std::memory_order_relaxed);
    {auto m=stats.maxActors.load(std::memory_order_relaxed);while(set.boneCount>m&&!stats.maxActors.compare_exchange_weak(m,set.boneCount,std::memory_order_relaxed)){}}
    const auto us=(detail::nowNs()-t0)/1000u;detail::bump(stats.us,us);detail::maxOf(stats.usMax,us);
}
inline bool install(const Env& e) noexcept {
    if(!e){state.store("NO SKELETON WORDS");return false;}
    env=e;state.store("INSTALLED");return true;
}
inline std::string report() {
    char b[1400];const auto ticks=stats.ticks.load();
    int n=std::snprintf(b,sizeof b,"{\"state\":\"%s\",\"mode\":%d,\"ticks\":%llu,\"chains\":%llu,\"chainFails\":%llu,\"layouts\":%llu,\"humanoid\":%llu,\"nonHumanoid\":%llu,"
        "\"layoutFails\":%llu,\"layoutEvictions\":%llu,\"planned\":%llu,\"published\":%llu,\"parts\":%llu,\"headerMismatch\":%llu,\"sanityDrops\":%llu,\"readFails\":%llu,"
        "\"farSkipped\":%llu,\"capSkipped\":%llu,\"raceSkipped\":%llu,\"lastActors\":%u,\"maxActors\":%u,\"lastParts\":%u,\"lastCandidates\":%u,\"lastMoving\":%u,"
        "\"usAvg\":%.1f,\"usMax\":%llu,\"keys\":{",state.load(),modeProp.load(),(unsigned long long)ticks,(unsigned long long)stats.chains.load(),
        (unsigned long long)stats.chainFails.load(),(unsigned long long)stats.layouts.load(),(unsigned long long)stats.humanoid.load(),
        (unsigned long long)stats.nonHumanoid.load(),(unsigned long long)stats.layoutFails.load(),(unsigned long long)stats.layoutEvictions.load(),(unsigned long long)stats.planned.load(),
        (unsigned long long)stats.published.load(),(unsigned long long)stats.parts.load(),(unsigned long long)stats.headerMismatch.load(),
        (unsigned long long)stats.sanityDrops.load(),(unsigned long long)stats.readFails.load(),(unsigned long long)stats.farSkipped.load(),
        (unsigned long long)stats.capSkipped.load(),(unsigned long long)stats.raceSkipped.load(),stats.lastActors.load(),stats.maxActors.load(),
        stats.lastParts.load(),stats.lastCandidates.load(),stats.lastMoving.load(),ticks?double(stats.us.load())/double(ticks):0.0,(unsigned long long)stats.usMax.load());
    std::string out(b,n>0?std::size_t(std::min(n,int(sizeof b)-1)):0);
    for(unsigned k=0;k<sw::KeyCount;++k){std::snprintf(b,sizeof b,"%s\"%s\":%d",k?",":"",sw::boneKeyName(k),int(firstLayoutKeys[k].load())-1);out+=b;} // -1 = missing / none yet
    out+="}}";
    return out;
}
} // namespace rdrvr::vr::mvbones
