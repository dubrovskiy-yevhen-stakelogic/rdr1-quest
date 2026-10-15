#pragma once
// ARC: the main-thread side of the shared arc renderer.
// Header-only, Vulkan-free (the render side is HandsPass.cpp; its producer API is HandsArcApi.h).
//
//  * EngineProbeApi + castWith: the engine segment probe (ConfigureActorExcludeInsts 0x217bc70,
//    ProbeMultipleExcludeInsts 0x2179ff0, FindClosestBulletStopper 0x239705c - the laser's
//    NativeAimProbe validation, but with the include mask / selection of the query) and the capsule
//    probe slot (sTestCapsule 0x268b300, bound by the teleport module: VrTeleportLoco.h
//    bindArcCapsule; nullptr = ProbeResult::Failed). Failed / NoBudget are NEVER "clear".
//  * Ledger: every main-thread cast of one Execute iteration (window = mainthread::runningFrame inside run(), frames + 1 before it) is
//    charged; soft users (LASER, TELEPORT, LASSO, DEBUG) share `debug.rdrvr.vr.arc.casts` (12) with fair shares, COMMIT has its own reserve
//    (`debug.rdrvr.vr.arc.commit`, 4), hard users (THROW_FLIGHT, VALIDATE, RELOAD) are never refused and reduce the next window's soft grants.
//  * Context (lazy, one per mainthread::run call): the head anchor of this Execute iteration = the key
//    of the eye pair Draw N renders, the player GUID (the anchor's - never the held weapon's owner),
//    up, the RAW controller sample (physical hands 0 left / 1 right, no holsters::weaponInput), the
//    LOCAL -> world map of the shifted plan + the eye midpoint, and the arc start on the BODY IK hand
//    (bodyik::handBox).
//  * Tracer: the ONE ballistic / path tracer (chords cast in order through the ledger, temporal reuse, Pending while anything before the
//    landing is unresolved, eye -> hand line-of-sight chord, validateShown = commit on what was SHOWN, never a re-launch).
//  * Looks + toFrame: the ArcFrame a feature publishes from its tick (hands::publishArc).
//  * ModuleArc tick (mainthread id 7): the DEBUG producer (`debug.rdrvr.vr.arc.test 1|2`) and the live levers (every 64 ticks).
//  * report() -> vr.arc, probesReport() -> vr.probes.
// Callbacks (anchorFn / mapFn / inputFn) are noexcept and catch internally; the tick catches
// everything itself (mainthread Tick is noexcept, run()'s try/catch is dead code).
#include "HandsArcApi.h"
#include "VrHandBox.h"
#include "VrMainThread.h"
#include "VrSettings.h"
#include "runtime/ControllerInput.h"
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
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace rdrvr::vr::arc {
using V3=std::array<float,3>;using M4=std::array<float,16>; // column-major, runtime::Mat4 layout (as VrThrow)
namespace hs=::rdrvr::vr::hands;

namespace math {
inline V3 add(V3 a,V3 b) noexcept {return {a[0]+b[0],a[1]+b[1],a[2]+b[2]};}
inline V3 sub(V3 a,V3 b) noexcept {return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
inline V3 mul(V3 a,float s) noexcept {return {a[0]*s,a[1]*s,a[2]*s};}
inline float dot(V3 a,V3 b) noexcept {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
inline V3 cross(V3 a,V3 b) noexcept {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
inline float length(V3 a) noexcept {return std::sqrt(dot(a,a));}
inline float distance(V3 a,V3 b) noexcept {return length(sub(a,b));}
inline bool finite(V3 a) noexcept {return std::isfinite(a[0])&&std::isfinite(a[1])&&std::isfinite(a[2]);}
inline V3 normalized(V3 a) noexcept {const float l=length(a);return l>1e-9f&&std::isfinite(l)?mul(a,1.f/l):V3{0,0,0};}
inline V3 lerp(V3 a,V3 b,float t) noexcept {return add(a,mul(sub(b,a),t));}
inline V3 xform(const M4& m,V3 p) noexcept {return {m[0]*p[0]+m[4]*p[1]+m[8]*p[2]+m[12],m[1]*p[0]+m[5]*p[1]+m[9]*p[2]+m[13],m[2]*p[0]+m[6]*p[1]+m[10]*p[2]+m[14]};}
inline V3 xformDir(const M4& m,V3 v) noexcept {return {m[0]*v[0]+m[4]*v[1]+m[8]*v[2],m[1]*v[0]+m[5]*v[1]+m[9]*v[2],m[2]*v[0]+m[6]*v[1]+m[10]*v[2]};}
inline V3 rotate(const std::array<float,4>& q,V3 v) noexcept { // (x, y, z, w)
    const V3 t{2*(q[1]*v[2]-q[2]*v[1]),2*(q[2]*v[0]-q[0]*v[2]),2*(q[0]*v[1]-q[1]*v[0])};
    return {v[0]+q[3]*t[0]+q[1]*t[2]-q[2]*t[1],v[1]+q[3]*t[1]+q[2]*t[0]-q[0]*t[2],v[2]+q[3]*t[2]+q[0]*t[1]-q[1]*t[0]};
}
} // namespace math

using Counter=std::atomic<std::uint64_t>;
inline void bump(Counter& c,std::uint64_t n=1) noexcept {c.fetch_add(n,std::memory_order_relaxed);}
inline void maxOf(Counter& c,std::uint64_t v) noexcept {auto m=c.load(std::memory_order_relaxed);while(v>m&&!c.compare_exchange_weak(m,v,std::memory_order_relaxed)){}}
inline std::uint64_t u64(const Counter& c) noexcept {return c.load(std::memory_order_relaxed);}

// ============================== engine probe ==============================
// Function pointers = the NativeAimProbeApi signatures (bound by GameXr from reticleProbeApi.probe after the TraceEntry fingerprints).
struct alignas(16) EngineVec {float v[4];};
struct alignas(16) EngineSegment {EngineVec start,end;};
struct alignas(16) EngineIntersection {std::byte bytes[0x70];};                                         // 0x70-byte stride
static_assert(sizeof(EngineVec)==16&&sizeof(EngineSegment)==32&&sizeof(EngineIntersection)==0x70);
struct EngineProbeApi {
    int (*exclude)(const std::uint32_t*,const void**,int,unsigned){};                                    // 0x217bc70
    int (*probe)(const EngineSegment*,int,EngineIntersection*,int,const void* const*,int,int){};         // 0x2179ff0
    EngineIntersection* (*closest)(int,const EngineVec*,EngineIntersection*,bool,EngineIntersection**){};// 0x239705c
    // Capsule query: int sTestCapsule(a, b, r, hits (0x70 stride, +8/+0x64 = 0xffff), max, include, type, state, swept,
    // excl, n) 0x268b300 - verified and bound by the teleport module (VrTeleportLoco.h bindArcCapsule);
    // nullptr = capsule queries fail.
    int (*capsule)(const EngineVec*,const EngineVec*,float,EngineIntersection*,int,unsigned,unsigned,unsigned char,bool,const void* const*,int){};
    explicit operator bool() const noexcept {return exclude&&probe&&closest;}
};
inline std::atomic<const EngineProbeApi*> engineApi{};

enum class Select : std::uint8_t {BulletStopper=0,AnySurface=1};   // FindClosestBulletStopper bool
// Include masks (read from the engine's call sites, not named rage enums): 0x500208 =
// reticle/laser/throw, CONTAINS 0x400000 = WATER (arcs land on the water surface); 0x4500208
// projectiles; 0x1 mover bounds; 0x2000000 FX ground plane; 0x2400000 ground | water; 0x100028
// lasso capsule.
// radiusM > 0 -> capsule path (engine world units, as the engine's own radius; the name keeps the contract's spelling).
struct ProbeQuery {V3 from{},to{};std::uint32_t include{0x500208};Select select{Select::BulletStopper};float radiusM{0};std::uint8_t state{7};
                   std::uint32_t type{0xffffffffu};std::uint8_t maxHits{8};};
struct Hit {V3 point{},normal{};float distance{},fraction{};bool normalValid{};std::uint32_t material{};
            std::uintptr_t instance{};    // phIntersection +0x00: borrowed, COMPARE-ONLY, never dereferenced, never kept past the tick
            std::uint16_t levelIndex{};}; // +0x08 (pre-filled 0xffff; meaning [H])
enum class ProbeResult : std::uint8_t {Clear,Hit,NoBudget,Failed};  // Failed = engine refused / saturated / invalid: NEVER "clear"
enum class ProbeUser : std::uint8_t {Laser,ThrowFlight,Teleport,Lasso,Debug,Commit,Validate,Reload,Count};
inline constexpr unsigned probeUsers=unsigned(ProbeUser::Count);
inline constexpr std::array<const char*,probeUsers> probeUserNames{"LASER","THROW_FLIGHT","TELEPORT","LASSO","DEBUG","COMMIT","VALIDATE","RELOAD"};
inline constexpr bool softUser(ProbeUser u) noexcept {return u==ProbeUser::Laser||u==ProbeUser::Teleport||u==ProbeUser::Lasso||u==ProbeUser::Debug;}
inline constexpr bool hardUser(ProbeUser u) noexcept {return u==ProbeUser::ThrowFlight||u==ProbeUser::Validate||u==ProbeUser::Reload;}

namespace detail {
template<class T> inline T get(const EngineIntersection& h,std::size_t offset) noexcept {T v{};std::memcpy(&v,h.bytes+offset,sizeof v);return v;}
template<class T> inline void put(EngineIntersection& h,std::size_t offset,T v) noexcept {std::memcpy(h.bytes+offset,&v,sizeof v);}
inline EngineVec vec(V3 p) noexcept {return {{p[0],p[1],p[2],0.f}};}
// Fill out from an engine hit; false = invalid (instance 0, not finite, off the swept volume).
inline bool readHit(const EngineIntersection& h,V3 from,V3 dir,float range,float lateral,Hit& out) noexcept {
    const auto instance=get<std::uintptr_t>(h,0);if(!instance)return false;
    float p[3]{},n[3]{};std::memcpy(p,h.bytes+0x10,sizeof p);std::memcpy(n,h.bytes+0x20,sizeof n);
    const V3 point{p[0],p[1],p[2]};if(!math::finite(point))return false;
    // double precision (as NativeAimProbe::cast): world coordinates of thousands of units would cancel in float
    const double d[3]{double(point[0])-from[0],double(point[1])-from[1],double(point[2])-from[2]};
    const double along=d[0]*dir[0]+d[1]*dir[1]+d[2]*dir[2],dd=d[0]*d[0]+d[1]*d[1]+d[2]*d[2],off2=std::max(0.0,dd-along*along);
    const double tol=.002+double(range)*1e-5;
    if(along<-(tol+lateral)||along>range+tol+lateral||off2>(tol+lateral)*(tol+lateral))return false;
    Hit r{};r.point=point;r.normal={n[0],n[1],n[2]};const float nl=math::length(r.normal);r.normalValid=std::isfinite(nl)&&nl>.9f&&nl<1.1f;
    if(!r.normalValid)r.normal={0,0,0};
    r.distance=float(std::sqrt(dd));r.fraction=range>0.f?float(std::clamp(along/range,0.0,1.0)):0.f;
    r.material=get<std::uint32_t>(h,0x60);r.instance=instance;r.levelIndex=get<std::uint16_t>(h,0x08);
    out=r;return true;
}
} // namespace detail

// Pure cast logic (host-testable with stub pointers): NativeAimProbe::cast's validation copied - finite inputs, 1e-4 < length <= 2000;
// the excluded list from exclude(&guid, list, 20, 0x176) unless a cached one is passed (count <= 0 / >= 20 / null entry -> Failed); hits
// pre-filled 0xffff at +0x08/+0x64; segment: probe(seg, 16, hits, n, list, q.include, 0) (found < 0 or >= 16 -> Failed: saturation is
// not clear) + closest(16, &start, hits, q.select == AnySurface, nullptr) (outside the array / misaligned / instance 0 / off the segment
// -> Failed; none -> Clear); capsule (q.radiusM > 0): capsule(...) (no pointer / n < 0 / n >= maxHits -> Failed), the valid hit closest
// to `from`. Hit = +0x10 point, +0x20 normal (|n| 0.9..1.1 -> normalValid), +0x60 material, +0x00 instance, +0x08 levelIndex.
inline ProbeResult castWith(const EngineProbeApi& api,std::uint32_t actorGuid,const ProbeQuery& q,const void* const* cachedExclude,int cachedCount,
                            Hit& out) noexcept {
    try{
        out=Hit{};
        if(!api||!actorGuid||!math::finite(q.from)||!math::finite(q.to))return ProbeResult::Failed;
        const V3 delta=math::sub(q.to,q.from);const double length=std::sqrt(double(delta[0])*delta[0]+double(delta[1])*delta[1]+double(delta[2])*delta[2]);
        if(!std::isfinite(length)||length<1e-4||length>2000)return ProbeResult::Failed;
        const V3 dir{float(delta[0]/length),float(delta[1]/length),float(delta[2]/length)};const float range=float(length);
        std::array<const void*,20> own{};const void* const* list=cachedExclude;int count=cachedCount;
        if(!list||count<=0){
            count=api.exclude(&actorGuid,own.data(),int(own.size()),0x176);
            if(count<=0||count>=int(own.size()))return ProbeResult::Failed;
            list=own.data();
        }
        if(count<=0||count>=20)return ProbeResult::Failed;
        for(int i=0;i<count;++i)if(!list[i])return ProbeResult::Failed;
        std::array<EngineIntersection,16> hits{};
        for(auto& h:hits){detail::put(h,8,std::uint32_t(0xffff));detail::put(h,0x64,std::uint32_t(0xffff));}
        const EngineVec a=detail::vec(q.from),b=detail::vec(q.to);
        if(q.radiusM>0.f){
            if(!api.capsule||!std::isfinite(q.radiusM)||q.radiusM>50.f)return ProbeResult::Failed;
            const int maxHits=std::clamp(int(q.maxHits),1,16);
            const int n=api.capsule(&a,&b,q.radiusM,hits.data(),maxHits,q.include,q.type,q.state,false,list,count);
            if(n<0||n>=maxHits)return ProbeResult::Failed;
            if(!n)return ProbeResult::Clear;
            bool any=false;Hit best{};
            for(int i=0;i<n;++i){Hit h{};if(!detail::readHit(hits[std::size_t(i)],q.from,dir,range,q.radiusM,h))continue;
                if(!any||h.distance<best.distance){best=h;any=true;}}
            if(!any)return ProbeResult::Failed;
            out=best;return ProbeResult::Hit;
        }
        EngineSegment segment{a,b};
        const int found=api.probe(&segment,int(hits.size()),hits.data(),count,list,int(q.include),0);
        if(found<0||found>=int(hits.size()))return ProbeResult::Failed;
        EngineIntersection* selected=api.closest(int(hits.size()),&segment.start,hits.data(),q.select==Select::AnySurface,nullptr);
        if(!selected)return ProbeResult::Clear;
        const auto address=reinterpret_cast<std::uintptr_t>(selected),begin=reinterpret_cast<std::uintptr_t>(hits.data());
        if(address<begin||address>=begin+sizeof(hits)||(address-begin)%sizeof(EngineIntersection))return ProbeResult::Failed;
        if(!detail::readHit(*selected,q.from,dir,range,0.f,out)){out=Hit{};return ProbeResult::Failed;}
        return ProbeResult::Hit;
    }catch(...){out=Hit{};return ProbeResult::Failed;}
}

// ============================== main-thread probe ledger ==============================
// One window = one Execute iteration: mainthread::runningFrame inside run(), mainthread::frames + 1 before it (the laser at PostCameraUpdate
// of the same iteration). Soft users share `cap`: LASER 1 per laser that had demand (2 with a DUAL phantom laser), then
// every soft user with demand last window gets min(2, demand) (rotating order), the rest round-robin
// by unmet demand (rotating start); a newcomer may take up to 2 from the unallocated rest. COMMIT
// uses its own reserve. Hard users are never refused, are charged, and reduce the next window's soft
// budget.
// The LASER has a floor of min(demand, 2) (newcomer: 2) that does not depend on `cap` or on last window's hard casts - it
// still comes out of the budget when there is one, so `cap` only bounds the arc users (.arc.casts 0 = arcs Pending, the laser probes).
struct UserStats {Counter casts{},refused{},failed{},nsSum{},nsMax{},mmSum{},nsPerMSumCasts{},nsPerMMax{},budgetStarved{},demandWindows{};};
struct Ledger {
    std::atomic<unsigned> cap{12},commitReserve{4};
    // current window (main thread only)
    std::uint64_t window{};unsigned rotate{};
    std::array<unsigned,probeUsers> grant{},used{},asked{},demandNow{},demandLast{},newcomer{};
    unsigned unallocated{},commitUsed{},hardNow{},hardLast{};std::uint64_t windowNs{},windowCasts{};
    // exclude cache (one exclude() per (tick run, actor); valid only inside one mainthread::run call)
    std::uint64_t excludeWindow{};std::uint32_t excludeGuid{};std::array<const void*,20> excludeList{};int excludeCount{};
    // stats (any thread reads)
    std::array<UserStats,probeUsers> users{};
    Counter windows{},castsMax{},windowNsSum{},windowNsMax{},overCap{},excludeHits{},excludeMisses{},excludeFailed{};
    static std::uint64_t currentWindow() noexcept {
        const auto r=mainthread::runningFrame.load(std::memory_order_relaxed);
        return r?r:mainthread::frames.load(std::memory_order_relaxed)+1;
    }
    void roll() noexcept {
        const auto id=currentWindow();if(id==window)return;
        if(window){ // close the old window
            bump(windows);maxOf(castsMax,windowCasts);windowNsSum.fetch_add(windowNs,std::memory_order_relaxed);maxOf(windowNsMax,windowNs);
            if(windowCasts>std::uint64_t(cap.load(std::memory_order_relaxed))+commitReserve.load(std::memory_order_relaxed))bump(overCap);
        }
        window=id;
        for(unsigned u=0;u<probeUsers;++u){demandLast[u]=std::max(demandNow[u],asked[u]);demandNow[u]=asked[u]=used[u]=grant[u]=newcomer[u]=0;}
        hardLast=hardNow;hardNow=0;commitUsed=0;windowNs=0;windowCasts=0;
        // grants of the new window from last window's demand
        const unsigned c=cap.load(std::memory_order_relaxed);unsigned budget=c>hardLast?c-hardLast:0u;
        constexpr std::array<ProbeUser,3> rest{ProbeUser::Teleport,ProbeUser::Lasso,ProbeUser::Debug};
        const auto L=unsigned(ProbeUser::Laser);
        if(demandLast[L]){grant[L]=std::min(demandLast[L],2u);budget-=std::min(budget,grant[L]);} // 1 per laser; 2 with a DUAL phantom laser; floor
        for(unsigned k=0;k<rest.size();++k){const auto u=unsigned(rest[(k+rotate)%rest.size()]);
            if(!demandLast[u])continue;const unsigned g=std::min({2u,demandLast[u],budget});grant[u]+=g;budget-=g;}
        for(bool progress=true;budget&&progress;){progress=false;
            for(unsigned k=0;k<rest.size()&&budget;++k){const auto u=unsigned(rest[(k+rotate)%rest.size()]);
                if(grant[u]<demandLast[u]){++grant[u];--budget;progress=true;}}}
        unallocated=budget;
        for(unsigned u=0;u<probeUsers;++u){if(!softUser(ProbeUser(u))||!demandLast[u])continue;bump(users[u].demandWindows);
            const unsigned minShare=u==L?1u:std::min(2u,demandLast[u]);if(grant[u]<minShare)bump(users[u].budgetStarved);}
        ++rotate;
    }
    // false -> NoBudget (counted `refused`). Every call counts as demand of this window.
    bool take(ProbeUser user) noexcept {
        roll();const auto u=unsigned(user);if(u>=probeUsers)return false;
        ++asked[u];
        if(hardUser(user)){++used[u];++hardNow;return true;}
        if(user==ProbeUser::Commit){if(commitUsed<commitReserve.load(std::memory_order_relaxed)){++commitUsed;++used[u];return true;}bump(users[u].refused);return false;}
        if(used[u]<grant[u]){++used[u];return true;}
        if(!demandLast[u]&&newcomer[u]<2&&(unallocated||user==ProbeUser::Laser)){if(unallocated)--unallocated;++newcomer[u];++used[u];return true;} // laser floor
        bump(users[u].refused);return false;
    }
    // Every cast (the laser, VrThrow and arc::probe included): time, length (metres), engine failure.
    void charge(ProbeUser user,std::uint64_t ns,float metres,bool failed) noexcept {
        roll();const auto u=unsigned(user);if(u>=probeUsers)return;
        auto& s=users[u];bump(s.casts);if(failed)bump(s.failed);s.nsSum.fetch_add(ns,std::memory_order_relaxed);maxOf(s.nsMax,ns);
        const float m=std::isfinite(metres)&&metres>0.f?std::min(metres,1e5f):0.f;s.mmSum.fetch_add(std::uint64_t(m*1000.f),std::memory_order_relaxed);
        if(m>=.1f){const auto perM=std::uint64_t(double(ns)/double(m));bump(s.nsPerMSumCasts);maxOf(s.nsPerMMax,perM);}
        ++windowCasts;windowNs+=ns;
    }
    void demand(ProbeUser user,unsigned casts) noexcept {roll();const auto u=unsigned(user);if(u<probeUsers)demandNow[u]=std::max(demandNow[u],casts);}
    // One exclude() per (tick run, actor); nullptr outside a mainthread::run call or when the engine refuses (castWith builds its own).
    const void* const* excluded(std::uint32_t actorGuid,int& count) noexcept {
        count=0;const auto run=mainthread::runningFrame.load(std::memory_order_relaxed);const auto* api=engineApi.load(std::memory_order_acquire);
        if(!run||!actorGuid||!api||!*api)return nullptr;
        if(excludeWindow==run&&excludeGuid==actorGuid&&excludeCount>0){bump(excludeHits);count=excludeCount;return excludeList.data();}
        bump(excludeMisses);excludeWindow=0;excludeCount=0;excludeList={};
        try{
            std::uint32_t guid=actorGuid;const int n=api->exclude(&guid,excludeList.data(),int(excludeList.size()),0x176);
            if(n<=0||n>=int(excludeList.size())){bump(excludeFailed);return nullptr;}
            for(int i=0;i<n;++i)if(!excludeList[std::size_t(i)]){bump(excludeFailed);return nullptr;}
            excludeWindow=run;excludeGuid=actorGuid;excludeCount=n;count=n;return excludeList.data();
        }catch(...){bump(excludeFailed);return nullptr;}
    }
    unsigned granted(ProbeUser u) const noexcept {return grant[unsigned(u)];}
    unsigned usedNow(ProbeUser u) const noexcept {return used[unsigned(u)];}
    void resetForTests() noexcept {
        window=0;rotate=0;grant={};used={};asked={};demandNow={};demandLast={};newcomer={};unallocated=commitUsed=hardNow=hardLast=0;windowNs=windowCasts=0;
        excludeWindow=0;excludeGuid=0;excludeList={};excludeCount=0;
        for(auto& s:users)for(auto* c:{&s.casts,&s.refused,&s.failed,&s.nsSum,&s.nsMax,&s.mmSum,&s.nsPerMSumCasts,&s.nsPerMMax,&s.budgetStarved,&s.demandWindows})c->store(0);
        for(auto* c:{&windows,&castsMax,&windowNsSum,&windowNsMax,&overCap,&excludeHits,&excludeMisses,&excludeFailed})c->store(0);
    }
};
inline Ledger ledger; // main thread only (report reads its atomics)

// ============================== per-tick context ==============================
struct AnchorInputs {                   // GameXr vrArcAnchor(now): presentation.snapshot + headCamera.snapshot(now, revision)
    std::uint64_t key{};                // anchor.sampledNs: the call gameBeginVisibility makes at Draw+0x10c of this iteration
    std::uint32_t actorGuid{};          // anchor.actorGuid = the player - NOT vw::ownerSnapshot()
    V3 up{0,1,0};V3 anchorPosition{};   // anchor.up (Y-up)
    bool world{};                       // presentation world && anchor available && the VR menu not eating input
};
struct MapInputs {M4 localToWorld{};std::uint64_t referenceSpace{};float upm{1};V3 eye{};bool bridged{},valid{};}; // vrThrowMap + eye midpoint
using AnchorFn=bool(*)(std::uint64_t now,AnchorInputs&) noexcept;       // all callbacks: noexcept, catch internally
using MapFn=bool(*)(std::uint64_t now,MapInputs&) noexcept;
using InputFn=bool(*)(std::uint64_t now,runtime::ControllerSample& raw) noexcept;  // controllers.snapshot(now): RAW, no weaponInput
inline std::atomic<AnchorFn> anchorFn{};inline std::atomic<MapFn> mapFn{};inline std::atomic<InputFn> inputFn{};
enum class StartSource : std::uint8_t {None,Body,Controller};

struct ContextStats {
    Counter built{},noAnchor{},notWorld{},noActor{},noMap{},noInput{},spaceMismatch{};std::array<Counter,2> untracked{};
    Counter startBody{},startController{},startNone{},toHandSamples{},toHandUmSum{},toHandUmMaxSteady{},toHandUmMaxBridged{},handBoxStale{};
};
inline ContextStats contextStats;

class Context {                         // main thread, inside a mainthread tick; getters fetch once per tick (lazy)
    std::uint64_t now_{},window_{},prevTickNs_{};
    mutable bool anchorFetched_{},anchorOk_{},mapFetched_{},mapOk_{},inputFetched_{},inputOk_{};
    mutable AnchorInputs anchor_{};mutable MapInputs map_{};mutable runtime::ControllerSample raw_{};
    mutable std::array<bool,2> untrackedCounted_{};
    void fetchAnchor() const noexcept {
        if(anchorFetched_)return;anchorFetched_=true;
        const auto fn=anchorFn.load(std::memory_order_acquire);AnchorInputs a{};bool ok=false;
        if(fn){try{ok=fn(now_,a);}catch(...){ok=false;}}
        if(ok&&!math::finite(a.up))ok=false;
        anchorOk_=ok&&a.key;anchor_=anchorOk_?a:AnchorInputs{};
        bump(contextStats.built);
        if(!anchorOk_)bump(contextStats.noAnchor);
        else if(!anchor_.world)bump(contextStats.notWorld);
        else if(!anchor_.actorGuid)bump(contextStats.noActor);
    }
    void fetchMap() const noexcept {
        if(mapFetched_)return;mapFetched_=true;
        const auto fn=mapFn.load(std::memory_order_acquire);MapInputs m{};bool ok=false;
        if(fn){try{ok=fn(now_,m);}catch(...){ok=false;}}
        if(ok){for(float x:m.localToWorld)ok=ok&&std::isfinite(x);ok=ok&&std::isfinite(m.upm)&&m.upm>.01f&&m.upm<=1000.f&&m.referenceSpace;}
        mapOk_=ok;map_=ok?m:MapInputs{};if(ok)map_.valid=true;
        if(!ok)bump(contextStats.noMap);
    }
    void fetchInput() const noexcept {
        if(inputFetched_)return;inputFetched_=true;
        const auto fn=inputFn.load(std::memory_order_acquire);runtime::ControllerSample s{};bool ok=false;
        if(fn){try{ok=fn(now_,s);}catch(...){ok=false;}}
        ok=ok&&runtime::validController(s)&&s.referenceSpace;
        inputOk_=ok;raw_=ok?s:runtime::ControllerSample{};
        if(!ok)bump(contextStats.noInput);
    }
    bool spaceOk() const noexcept { // the raw poses and the map in the same XR reference space (a recenter in between: no mapping)
        fetchMap();fetchInput();if(!mapOk_||!inputOk_)return false;
        if(raw_.referenceSpace!=map_.referenceSpace){bump(contextStats.spaceMismatch);return false;}
        return true;
    }
    void untracked(unsigned hand) const noexcept {if(hand<2&&!untrackedCounted_[hand]){untrackedCounted_[hand]=true;bump(contextStats.untracked[hand]);}}
public:
    void reset(std::uint64_t now,std::uint64_t window,std::uint64_t prevTickNs) noexcept {
        now_=now;window_=window;prevTickNs_=prevTickNs;anchorFetched_=anchorOk_=mapFetched_=mapOk_=inputFetched_=inputOk_=false;
        anchor_={};map_={};raw_={};untrackedCounted_={};
    }
    std::uint64_t now() const noexcept {return now_;}
    std::uint64_t window() const noexcept {return window_;}
    bool world() const noexcept {fetchAnchor();return anchorOk_&&anchor_.world;} // false -> producers clearArc and cancel pending commits
    std::uint64_t key() const noexcept {return world()?anchor_.key:0;}            // 0 when !world
    std::uint32_t actorGuid() const noexcept {fetchAnchor();return anchorOk_?anchor_.actorGuid:0;}
    V3 up() const noexcept {fetchAnchor();const V3 u=anchorOk_?math::normalized(anchor_.up):V3{0,1,0};return math::length(u)>.5f?u:V3{0,1,0};}
    V3 anchorPosition() const noexcept {fetchAnchor();return anchor_.anchorPosition;}
    bool map(MapInputs& out) const noexcept {fetchMap();if(!mapOk_)return false;out=map_;return true;}
    float upm() const noexcept {fetchMap();return mapOk_?map_.upm:1.f;}
    bool eye(V3& world) const noexcept {fetchMap();if(!mapOk_||!math::finite(map_.eye))return false;world=map_.eye;return true;}
    bool inputValid() const noexcept {fetchInput();return inputOk_;}
    const runtime::ControllerSample* raw() const noexcept {fetchInput();return inputOk_?&raw_:nullptr;}
    // `hand` is the PHYSICAL index: 0 = left, 1 = right. False when the hand pose is not tracked / not finite / no map.
    bool aimRay(unsigned hand,V3& origin,V3& direction) const noexcept {
        if(hand>1||!spaceOk())return false;
        const auto& p=raw_.aim[hand];if(!validPose(p)){untracked(hand);return false;}
        const V3 o=math::xform(map_.localToWorld,{p.position[0],p.position[1],p.position[2]});
        const V3 d=math::normalized(math::xformDir(map_.localToWorld,math::rotate(p.orientation,{0,0,-1})));
        if(!math::finite(o)||math::length(d)<.5f){untracked(hand);return false;}
        origin=o;direction=d;return true;
    }
    bool gripPoint(unsigned hand,V3& world) const noexcept {
        if(hand>1||!spaceOk())return false;
        const auto& p=raw_.grip[hand];if(!validPose(p)){untracked(hand);return false;}
        const V3 o=math::xform(map_.localToWorld,{p.position[0],p.position[1],p.position[2]});
        if(!math::finite(o)){untracked(hand);return false;}
        world=o;return true;
    }
    // Arc start: the BODY IK grip point of this engine frame (bodyik::handBox of hook A in Update N, ikDriven[hand], published since the
    // previous tick) when BODY shows the arm; else the controller grip. Reports start.* and the residual to the controller grip.
    StartSource startPoint(unsigned hand,V3& world) const noexcept {
        V3 ctrl{};const bool haveCtrl=gripPoint(hand,ctrl);
        bodyik::HandBox b{};
        if(hand<2&&bodyik::handBox.load(b)&&b.ikDriven[hand]&&b.ns){
            const bool recent=now_>=b.ns?now_-b.ns<=100'000'000ull:b.ns-now_<5'000'000ull;
            const bool fresh=recent&&(!prevTickNs_||b.ns>prevTickNs_);
            const V3 g{b.grip[hand][0],b.grip[hand][1],b.grip[hand][2]};
            fetchMap();
            const bool space=!mapOk_||!b.referenceSpace||b.referenceSpace==map_.referenceSpace;
            if(fresh&&space&&math::finite(g)){
                world=g;bump(contextStats.startBody);
                if(haveCtrl){const float mm=math::distance(g,ctrl)*1000.f/std::max(upm(),.01f);
                    if(std::isfinite(mm)){bump(contextStats.toHandSamples);contextStats.toHandUmSum.fetch_add(std::uint64_t(mm*1000.f),std::memory_order_relaxed);
                        maxOf(map_.bridged?contextStats.toHandUmMaxBridged:contextStats.toHandUmMaxSteady,std::uint64_t(mm*1000.f));}}
                return StartSource::Body;
            }
            bump(contextStats.handBoxStale);
        }
        if(haveCtrl){world=ctrl;bump(contextStats.startController);return StartSource::Controller;}
        bump(contextStats.startNone);return StartSource::None;
    }
};
namespace detail {
struct ContextSlot {Context ctx;std::uint64_t id{},lastNow{};};
inline ContextSlot& contextSlot() noexcept {static ContextSlot s;return s;}
}
// The same object for every tick of one mainthread::run call (outside run(): one per `now`).
inline Context& frameContext(std::uint64_t now) noexcept {
    auto& s=detail::contextSlot();const auto run=mainthread::runningFrame.load(std::memory_order_relaxed);
    const std::uint64_t id=run?run:(now|(std::uint64_t(1)<<63));
    if(id!=s.id){const auto prev=s.lastNow;s.id=id;s.lastNow=now;s.ctx.reset(now,Ledger::currentWindow(),prev);}
    return s.ctx;
}
inline void resetContextForTests() noexcept {auto& s=detail::contextSlot();s.id=0;s.lastNow=0;s.ctx.reset(0,0,0);}

// Through the ledger + castWith. NoBudget = not cast; the cached exclude list is valid only inside one mainthread::run call.
inline ProbeResult probe(const Context& ctx,ProbeUser user,const ProbeQuery& q,Hit& out) noexcept {
    out=Hit{};
    if(!ledger.take(user))return ProbeResult::NoBudget;
    const auto* api=engineApi.load(std::memory_order_acquire);const auto guid=ctx.actorGuid();
    if(!api||!*api||!guid){ledger.charge(user,0,0.f,true);return ProbeResult::Failed;}
    int count=0;const void* const* list=ledger.excluded(guid,count);
    const auto t0=nowNs();const auto r=castWith(*api,guid,q,list,count,out);const auto dt=nowNs()-t0;
    ledger.charge(user,dt,math::distance(q.from,q.to)/std::max(ctx.upm(),.01f),r==ProbeResult::Failed);
    return r;
}

// ============================== launch direction filter (1-euro, comfort: hand tremor at 36 Hz) ==============================
struct DirectionFilter {
    float minCutoffHz{1.5f},beta{.5f},dCutoffHz{1.f};
    V3 last{},dLast{};std::uint64_t lastNs{};bool have{};
    static float alpha(float cutoffHz,float dt) noexcept {const float tau=1.f/(6.2831853f*std::max(cutoffHz,1e-3f));return 1.f/(1.f+tau/dt);}
    V3 filter(const V3& raw,std::uint64_t now) noexcept {
        const V3 x=math::normalized(raw);if(math::length(x)<.5f)return raw;
        if(!have||now<=lastNs||now-lastNs>500'000'000ull){have=true;last=x;dLast={0,0,0};lastNs=now;return x;}
        const float dt=std::clamp(float(double(now-lastNs)/1e9),1e-4f,.1f);lastNs=now;
        const V3 dx=math::mul(math::sub(x,last),1.f/dt);
        dLast=math::lerp(dLast,dx,alpha(dCutoffHz,dt));
        const float cutoff=minCutoffHz+beta*math::length(dLast);
        const V3 out=math::normalized(math::lerp(last,x,alpha(cutoff,dt)));
        if(math::length(out)<.5f){last=x;return x;}
        last=out;return out;
    }
    void reset() noexcept {have=false;last=dLast={0,0,0};lastNs=0;}
};

// ============================== the ONE shared ballistic / path tracer ==============================
struct Launch {                             // world units / s; gravity in m/s^2 along -ctx.up (scaled by upm)
    V3 origin{},velocity{};float gravityMps2{9.81f};float dragPerS{0};
    float maxTimeS{2.5f},maxRangeM{25.f},maxDropM{15.f};
};
struct TraceConfig {
    float chordDtS{0};                      // > 0: chords of this duration (teleport 0.12 s); 0: `chords` equal chords over the flight
    unsigned chords{8},maxChords{16},pointsPerChord{4};   // drawn points = chords * pointsPerChord + 1, capped at 64
    float reuseM{.05f};unsigned reuseMaxTicks{6},staleAfterTicks{2};
    bool losFromEye{};                      // chord 0 = eye -> origin (teleport HAND_BLOCKED; lasso through a fence)
    Select losSelect{Select::AnySurface};   // the LOS chord's selection (the other chords use query.select)
    ProbeQuery query{};                     // include mask, selection, capsule radius (from/to ignored)
    ProbeUser user{ProbeUser::Debug};
};
enum class ChordState : std::uint8_t {Fresh,Reused,Unknown,Failed};   // Unknown = needed a cast, got no budget, no usable cache
inline constexpr unsigned maxTraceChords=32;
struct Trace {
    unsigned count{};std::array<V3,hs::arcMaxPoints> p{};   // world units, p[0] = launch origin, last = landing (when hit)
    unsigned chordCount{};std::array<ChordState,maxTraceChords> chord{};std::array<std::uint64_t,maxTraceChords> chordTick{};
    bool hit{},pending{},truncated{},handBlocked{};Hit landing{};unsigned landingChord{};
    float lengthM{},timeS{};unsigned casts{},reused{},unknown{},failed{};std::uint64_t key{};
    // For validateShown: chord k = chordPoint[k] -> chordPoint[k+1]; the LOS chord; upm of the trace
    std::array<V3,maxTraceChords+1> chordPoint{};
    bool losUsed{};ChordState losState{ChordState::Unknown};std::uint64_t losTick{};V3 losFrom{},losTo{};
    float upm{1};std::uint64_t window{};
};
// pending == any chord up to the landing (or to the end without a hit), or the LOS chord, is Unknown or Failed (a hand-blocked trace is
// resolved: invalid, never pending).
enum class CommitState : std::uint8_t {Pending,Accepted,Rejected};
enum class CommitReject : std::uint8_t {None,Changed,Failed,Timeout,NoContext};
struct CommitResult {CommitState state{};CommitReject reason{};Hit landing{};float shownToCommittedCm{};unsigned casts{};};

struct TracerStats {
    Counter runs{},chords{},cast{},reused{},unknown{},failed{},pendingFrames{},hits{},handBlocked{},truncated{},nonFinite{};
    Counter checks{},accepted{},changed{},rejectedFailed{},timeout{},noContext{},commitCasts{},shownToCommittedUmSum{},shownToCommittedUmMax{},
        rejectedAfterShownValid{},cancelled{},expired{};
};
inline TracerStats tracerStats;
// Consumers' commit-edge bookkeeping: an edge cancelled (no world / no aim at its tick) or expired (> 100 ms old).
inline void noteCommitCancelled() noexcept {bump(tracerStats.cancelled);}
inline void noteCommitExpired() noexcept {bump(tracerStats.expired);}

class Tracer {                              // one per channel; main thread only
    struct Cache {V3 a{},b{};std::uint64_t tick{};ProbeResult result{ProbeResult::Failed};Hit hit{};bool valid{};};
    std::array<Cache,maxTraceChords> cache_{};Cache los_{};
    bool haveShown_{},shownValid_{};Trace shown_{};unsigned commitTicks_{},commitCasts_{};
    static bool reusable(const Cache& c,V3 a,V3 b,float tolUnits,std::uint64_t tick,unsigned maxTicks) noexcept {
        return c.valid&&tick>=c.tick&&tick-c.tick<=maxTicks&&math::distance(a,c.a)<tolUnits&&math::distance(b,c.b)<tolUnits&&
            (c.result==ProbeResult::Clear||c.result==ProbeResult::Hit);
    }
    // Shared core: dense polyline pts[0..n) (world units), chord boundaries every `ppc` points, optional per-point times.
    Trace trace(const Context& ctx,const V3* pts,const float* times,unsigned n,unsigned ppc,const TraceConfig& cfg) noexcept {
        Trace t{};t.key=ctx.key();t.upm=ctx.upm();const auto tick=Ledger::currentWindow();t.window=tick;t.chord.fill(ChordState::Unknown);
        bump(tracerStats.runs);
        if(n<2||ppc<1){bump(tracerStats.nonFinite);return t;}
        for(unsigned i=0;i<n;++i)if(!math::finite(pts[i])){bump(tracerStats.nonFinite);return t;}
        const unsigned chords=std::min((n-1+ppc-1)/ppc,maxTraceChords);
        t.chordCount=chords;
        for(unsigned k=0;k<=chords;++k)t.chordPoint[k]=pts[std::min(k*ppc,n-1)];
        const float tol=std::max(cfg.reuseM,0.f)*t.upm;
        unsigned demand=0;bool stopped=false;
        ProbeQuery q=cfg.query;
        // the eye -> hand line of sight
        if(cfg.losFromEye){
            t.losUsed=true;V3 e{};
            if(ctx.eye(e)){
                t.losFrom=e;t.losTo=pts[0];
                if(reusable(los_,e,pts[0],tol,tick,cfg.reuseMaxTicks)){t.losState=ChordState::Reused;t.losTick=los_.tick;++t.reused;
                    if(los_.result==ProbeResult::Hit)t.handBlocked=true;}
                else{
                    ++demand;q.from=e;q.to=pts[0];q.select=cfg.losSelect;Hit h{};
                    const auto r=probe(ctx,cfg.user,q,h);
                    if(r==ProbeResult::Clear||r==ProbeResult::Hit){t.losState=ChordState::Fresh;t.losTick=tick;++t.casts;los_={e,pts[0],tick,r,h,true};
                        if(r==ProbeResult::Hit)t.handBlocked=true;}
                    else if(r==ProbeResult::NoBudget){t.losState=ChordState::Unknown;++t.unknown;stopped=true;}
                    else{t.losState=ChordState::Failed;++t.failed;stopped=true;los_.valid=false;}
                }
            }else{t.losState=ChordState::Unknown;++t.unknown;stopped=true;}
        }
        q.select=cfg.query.select;
        int landingChord=-1;Hit landing{};
        for(unsigned k=0;k<chords&&!t.handBlocked;++k){
            const V3 a=t.chordPoint[k],b=t.chordPoint[k+1];
            auto& c=cache_[k];
            if(reusable(c,a,b,tol,tick,cfg.reuseMaxTicks)){t.chord[k]=ChordState::Reused;t.chordTick[k]=c.tick;++t.reused;
                if(c.result==ProbeResult::Hit){landingChord=int(k);landing=c.hit;break;}
                continue;}
            ++demand;
            if(stopped){t.chord[k]=ChordState::Unknown;++t.unknown;continue;}
            q.from=a;q.to=b;Hit h{};
            if(math::distance(a,b)<1e-4f){t.chord[k]=ChordState::Fresh;t.chordTick[k]=tick;c={a,b,tick,ProbeResult::Clear,{},true};continue;}
            const auto r=probe(ctx,cfg.user,q,h);
            if(r==ProbeResult::NoBudget){t.chord[k]=ChordState::Unknown;++t.unknown;stopped=true;continue;}
            if(r==ProbeResult::Failed){t.chord[k]=ChordState::Failed;++t.failed;stopped=true;c.valid=false;continue;}
            t.chord[k]=ChordState::Fresh;t.chordTick[k]=tick;++t.casts;c={a,b,tick,r,h,true};
            if(r==ProbeResult::Hit){landingChord=int(k);landing=h;break;}
        }
        // pending: the LOS chord or any chord up to the landing / the end unresolved
        const unsigned last=landingChord>=0?unsigned(landingChord):(chords?chords-1:0);
        bool pending=t.losUsed&&(t.losState==ChordState::Unknown||t.losState==ChordState::Failed);
        if(!t.handBlocked)for(unsigned k=0;k<=last&&k<chords;++k)if(t.chord[k]==ChordState::Unknown||t.chord[k]==ChordState::Failed)pending=true;
        if(t.handBlocked)pending=false;
        t.pending=pending;
        // the drawn polyline: up to the landing (cut at the hit point), else the whole path
        float endTime=times?times[n-1]:0.f;
        if(landingChord>=0&&!t.handBlocked){
            const unsigned i0=unsigned(landingChord)*ppc,i1=std::min(i0+ppc,n-1);
            const V3 a=pts[i0],b=pts[i1];const V3 ab=math::sub(b,a);const float L2=math::dot(ab,ab);
            const float f=L2>1e-12f?std::clamp(math::dot(math::sub(landing.point,a),ab)/L2,0.f,1.f):0.f;
            const float idx=float(i0)+f*float(i1-i0);
            t.count=0;
            for(unsigned i=0;i<=i0&&t.count<hs::arcMaxPoints;++i)t.p[t.count++]=pts[i];
            for(unsigned i=i0+1;i<i1&&float(i)<idx&&t.count<hs::arcMaxPoints-1;++i)t.p[t.count++]=pts[i];
            if(t.count<hs::arcMaxPoints)t.p[t.count++]=landing.point;
            t.hit=true;t.landing=landing;t.landingChord=unsigned(landingChord);
            if(times)endTime=times[i0]+f*(times[i1]-times[i0]);
        }else{t.count=std::min(n,hs::arcMaxPoints);for(unsigned i=0;i<t.count;++i)t.p[i]=pts[i];}
        t.truncated=!t.hit&&!t.handBlocked;
        float len=0.f;for(unsigned i=1;i<t.count;++i)len+=math::distance(t.p[i-1],t.p[i]);
        t.lengthM=len/std::max(t.upm,.01f);t.timeS=endTime;
        ledger.demand(cfg.user,demand);
        bump(tracerStats.chords,chords);bump(tracerStats.cast,t.casts);bump(tracerStats.reused,t.reused);bump(tracerStats.unknown,t.unknown);
        bump(tracerStats.failed,t.failed);if(t.pending)bump(tracerStats.pendingFrames);if(t.hit)bump(tracerStats.hits);
        if(t.handBlocked)bump(tracerStats.handBlocked);if(t.truncated)bump(tracerStats.truncated);
        return t;
    }
public:
    // The parabola p(t) = o + v t + 1/2 g t^2 (g along -up, scaled by upm), drag as an exponential velocity decay; ends at maxRange
    // (horizontal), maxDrop below the origin or maxTime. Chords: chordDtS > 0 -> chords of that duration (<= maxChords); else `chords`
    // equal chords. Points: pointsPerChord per chord (<= 64 in all).
    Trace run(const Context& ctx,const Launch& L,const TraceConfig& cfg) noexcept {
        const float upm=ctx.upm();V3 up=ctx.up();
        if(!math::finite(L.origin)||!math::finite(L.velocity)||!std::isfinite(L.gravityMps2)||!std::isfinite(L.dragPerS)||!(L.maxTimeS>0.f)||
           !std::isfinite(L.maxTimeS)||!(L.maxRangeM>0.f)||!(L.maxDropM>0.f)){bump(tracerStats.runs);bump(tracerStats.nonFinite);return Trace{};}
        const float g=L.gravityMps2*upm,k=std::max(L.dragPerS,0.f);
        const auto at=[&](float t){const float s=k>1e-6f?(1.f-std::exp(-k*t))/k:t;
            return math::add(math::add(L.origin,math::mul(L.velocity,s)),math::mul(up,-.5f*g*t*t));};
        // end time by stepping (1/240 s), refined by bisection on the limit that stopped it
        const float range=L.maxRangeM*upm,drop=L.maxDropM*upm;
        const auto beyond=[&](float t){const V3 d=math::sub(at(t),L.origin);const float v=math::dot(d,up);const V3 h=math::sub(d,math::mul(up,v));
            return math::length(h)>range||-v>drop;};
        float T=std::min(L.maxTimeS,30.f);
        {const float dt=1.f/240.f;for(float t=dt;t<T;t+=dt)if(beyond(t)){float lo=t-dt,hi=t;for(int i=0;i<24;++i){const float m=.5f*(lo+hi);(beyond(m)?hi:lo)=m;}T=hi;break;}}
        unsigned chords=0;std::array<float,maxTraceChords+1> ct{};
        if(cfg.chordDtS>0.f){
            const unsigned cap=std::clamp(cfg.maxChords,1u,maxTraceChords);
            chords=std::clamp(unsigned(std::ceil(T/cfg.chordDtS-1e-4f)),1u,cap);
            for(unsigned i=0;i<=chords;++i)ct[i]=std::min(float(i)*cfg.chordDtS,T);
            if(float(chords)*cfg.chordDtS<T)T=ct[chords]=float(chords)*cfg.chordDtS; // maxChords reached: the trace ends earlier
            else ct[chords]=T;
        }else{
            chords=std::clamp(std::min(cfg.chords,cfg.maxChords?cfg.maxChords:cfg.chords),1u,maxTraceChords);
            for(unsigned i=0;i<=chords;++i)ct[i]=T*float(i)/float(chords);
        }
        const unsigned ppc=std::clamp(std::min(cfg.pointsPerChord,(hs::arcMaxPoints-1)/chords),1u,hs::arcMaxPoints-1);
        std::array<V3,hs::arcMaxPoints> pts{};std::array<float,hs::arcMaxPoints> times{};unsigned n=0;
        pts[n]=at(0.f);times[n++]=0.f;
        for(unsigned c=0;c<chords;++c)for(unsigned j=1;j<=ppc&&n<hs::arcMaxPoints;++j){const float t=ct[c]+(ct[c+1]-ct[c])*float(j)/float(ppc);pts[n]=at(t);times[n++]=t;}
        return trace(ctx,pts.data(),times.data(),n,ppc,cfg);
    }
    // The same chord / reuse / Pending / markShown / validateShown machinery over a caller-supplied path (LASSO:
    // engine-step Euler + homing); chords = groups of pointsPerChord points (more points than 32 chords hold -> larger groups).
    Trace runPath(const Context& ctx,const V3* points,unsigned n,const TraceConfig& cfg) noexcept {
        if(!points||n<2){bump(tracerStats.runs);bump(tracerStats.nonFinite);return Trace{};}
        n=std::min(n,hs::arcMaxPoints);
        unsigned ppc=std::max(cfg.pointsPerChord,1u);
        while((n-1+ppc-1)/ppc>std::min(std::max(cfg.maxChords,1u),maxTraceChords))++ppc;
        return trace(ctx,points,nullptr,n,ppc,cfg);
    }
    // toFrame calls it: the trace the player is looking at (valid = it was drawn Valid).
    void markShown(const Trace& t,bool valid=false) noexcept {shown_=t;haveShown_=true;shownValid_=valid;commitTicks_=0;commitCasts_=0;}
    bool hasShown() const noexcept {return haveShown_;}
    const Trace& shown() const noexcept {return shown_;}
    // Validate what was SHOWN: re-cast (commit reserve) only the shown chords up to the shown landing that are Unknown / Failed / older than
    // staleAfterTicks, along the SHOWN polyline (never a new launch). Accepted when every chord up to the landing is clear and the landing is
    // within 10 cm of the shown one; Changed when an earlier chord now hits, the landing chord is now clear or the landing moved > 10 cm (or
    // nothing with a landing was shown); Failed on an engine failure; Timeout after 3 ticks without resolution; NoContext when !world or the
    // input is gone. Call once per tick until resolved.
    CommitResult validateShown(const Context& ctx,const TraceConfig& cfg) noexcept {
        CommitResult r{};
        const auto finish=[&](CommitState s,CommitReject why){
            r.state=s;r.reason=why;r.casts=commitCasts_;bump(tracerStats.commitCasts,commitCasts_);
            if(s==CommitState::Accepted)bump(tracerStats.accepted);
            else{switch(why){case CommitReject::Changed:bump(tracerStats.changed);break;case CommitReject::Failed:bump(tracerStats.rejectedFailed);break;
                case CommitReject::Timeout:bump(tracerStats.timeout);break;default:bump(tracerStats.noContext);break;}
                if(shownValid_)bump(tracerStats.rejectedAfterShownValid);}
            commitTicks_=0;commitCasts_=0;return r;};
        if(!commitTicks_)bump(tracerStats.checks);
        ++commitTicks_;
        if(!haveShown_||!ctx.world()||!ctx.inputValid())return finish(CommitState::Rejected,CommitReject::NoContext);
        auto& s=shown_;
        if(!s.hit||s.handBlocked)return finish(CommitState::Rejected,CommitReject::Changed);
        const auto tick=Ledger::currentWindow();bool unresolved=false;
        const auto stale=[&](ChordState st,std::uint64_t at){return st==ChordState::Unknown||st==ChordState::Failed||tick<at||tick-at>cfg.staleAfterTicks;};
        ProbeQuery q=cfg.query;
        if(s.losUsed&&stale(s.losState,s.losTick)){
            q.from=s.losFrom;q.to=s.losTo;q.select=cfg.losSelect;Hit h{};const auto pr=probe(ctx,ProbeUser::Commit,q,h);
            if(pr==ProbeResult::NoBudget)unresolved=true;
            else{++commitCasts_;
                if(pr==ProbeResult::Failed)return finish(CommitState::Rejected,CommitReject::Failed);
                if(pr==ProbeResult::Hit)return finish(CommitState::Rejected,CommitReject::Changed);
                s.losState=ChordState::Fresh;s.losTick=tick;}
        }
        float movedUnits=0.f;
        for(unsigned k=0;!unresolved&&k<=s.landingChord&&k<s.chordCount;++k){
            if(!stale(s.chord[k],s.chordTick[k]))continue;
            q.from=s.chordPoint[k];q.to=s.chordPoint[k+1];q.select=cfg.query.select;Hit h{};
            if(math::distance(q.from,q.to)<1e-4f){s.chord[k]=ChordState::Fresh;s.chordTick[k]=tick;continue;}
            const auto pr=probe(ctx,ProbeUser::Commit,q,h);
            if(pr==ProbeResult::NoBudget){unresolved=true;break;}
            ++commitCasts_;
            if(pr==ProbeResult::Failed)return finish(CommitState::Rejected,CommitReject::Failed);
            if(k<s.landingChord){if(pr==ProbeResult::Hit)return finish(CommitState::Rejected,CommitReject::Changed);}
            else{
                if(pr!=ProbeResult::Hit)return finish(CommitState::Rejected,CommitReject::Changed);
                movedUnits=math::distance(h.point,s.landing.point);
                if(movedUnits>.10f*s.upm)return finish(CommitState::Rejected,CommitReject::Changed);
                s.landing=h;
            }
            s.chord[k]=ChordState::Fresh;s.chordTick[k]=tick;
        }
        if(unresolved){
            if(commitTicks_>=3)return finish(CommitState::Rejected,CommitReject::Timeout);
            r.state=CommitState::Pending;r.casts=commitCasts_;return r;
        }
        r.landing=s.landing;r.shownToCommittedCm=movedUnits*100.f/std::max(s.upm,.01f);
        const auto um=std::uint64_t(r.shownToCommittedCm*1e4f);tracerStats.shownToCommittedUmSum.fetch_add(um,std::memory_order_relaxed);
        maxOf(tracerStats.shownToCommittedUmMax,um);
        const auto out=finish(CommitState::Accepted,CommitReject::None);
        return CommitResult{out.state,out.reason,s.landing,r.shownToCommittedCm,out.casts};
    }
    void reset() noexcept {cache_={};los_={};haveShown_=false;shownValid_=false;shown_=Trace{};commitTicks_=0;commitCasts_=0;} // a new aim session
};

// ============================== looks ==============================
struct Look {std::uint32_t rgbaStart{},rgbaEnd{};float halfWidthStartM{.007f},halfWidthEndM{.010f};float groundTolM{.30f};
             hs::ArcStyle style{};hs::ArcMarker marker{};};
// VALID cyan-white (0.55, 0.85, 1.0) a .85 + Disc r 0.30 + pillar 0.6 m; INVALID orange-red (1.0, 0.30, 0.15) + Cross; solid (no dashes);
// ribbon x-ray .30, marker x-ray .45.
inline Look teleportLook(bool valid) noexcept {
    Look l{};const float r=valid?.55f:1.f,g=valid?.85f:.30f,b=valid?1.f:.15f;
    l.rgbaStart=l.rgbaEnd=hs::packRgba(r,g,b,.85f);l.halfWidthStartM=.007f;l.halfWidthEndM=.010f;l.groundTolM=.30f;
    l.style.dashPeriodM=0;l.style.xrayAlpha=.30f;l.style.fadeInM=.10f;l.style.tube=1;l.style.core=.30f;l.style.flags=0;
    l.marker.shape=valid?hs::ArcMarkerShape::Disc:hs::ArcMarkerShape::Cross;l.marker.rgba=hs::packRgba(r,g,b,.90f);l.marker.radiusM=.30f;
    l.marker.liftM=.02f;l.marker.depthTolM=.30f;l.marker.xrayAlpha=.45f;
    if(valid){l.marker.pillarHeightM=.6f;l.marker.pillarRgba=hs::packRgba(r,g,b,.55f);}
    return l;
}
// A fresh trace whose full checks did not run yet = teleportLook(true) with marker alpha x 0.5 and no pillar.
inline Look teleportValidatingLook() noexcept {
    Look l=teleportLook(true);const auto a=float((l.marker.rgba>>24)&0xffu)/255.f*.5f;
    l.marker.rgba=(l.marker.rgba&0x00ffffffu)|(std::uint32_t(a*255.f+.5f)<<24);l.marker.pillarHeightM=0;l.marker.pillarRgba=0;return l;
}
// Lasso: dark rope colour + strong core boost (= light core, dark rim - readable on desert ground), dashes anchored at
// the target; committed = brighter core and +2 mm; gold Ring r 0.5 m on a target, else a Disc on the surface hit (the producer may replace
// the shape: red Cross out of range, None).
inline Look lassoLook(bool target,bool committed=false) noexcept {
    Look l{};l.rgbaStart=l.rgbaEnd=hs::packRgba(.22f,.16f,.10f,.85f);
    l.halfWidthStartM=.006f+(committed?.002f:0.f);l.halfWidthEndM=.008f+(committed?.002f:0.f);l.groundTolM=.30f;
    l.style.dashPeriodM=.25f;l.style.dashDuty=.55f;l.style.xrayAlpha=.25f;l.style.fadeInM=.10f;l.style.tube=1;l.style.core=committed?.95f:.75f;
    l.style.flags=hs::ArcStyle::DashFromEnd;
    l.marker.shape=target?hs::ArcMarkerShape::Ring:hs::ArcMarkerShape::Disc;
    l.marker.rgba=target?hs::packRgba(1.f,.78f,.20f,.95f):hs::packRgba(.92f,.88f,.80f,.70f);l.marker.radiusM=target?.50f:.25f;
    l.marker.liftM=.02f;l.marker.depthTolM=.30f;l.marker.xrayAlpha=.45f;
    return l;
}
// = teleport FADED: neutral (0.75, 0.80, 0.85) a .35, no marker, no pillar, never Valid.
inline Look pendingLook() noexcept {
    Look l{};l.rgbaStart=l.rgbaEnd=hs::packRgba(.75f,.80f,.85f,.35f);l.halfWidthStartM=.006f;l.halfWidthEndM=.008f;l.groundTolM=.30f;
    l.style.dashPeriodM=0;l.style.xrayAlpha=.20f;l.style.fadeInM=.10f;l.style.tube=1;l.style.core=.15f;l.marker.shape=hs::ArcMarkerShape::None;
    return l;
}
// DEBUG (arc.test): green Ring on walkable ground, red Cross otherwise.
inline Look debugLook(bool valid) noexcept {
    Look l{};const float r=valid?.25f:1.f,g=valid?1.f:.20f,b=valid?.35f:.15f;
    l.rgbaStart=l.rgbaEnd=hs::packRgba(r,g,b,.85f);l.style.dashPeriodM=0;l.style.xrayAlpha=.30f;l.style.fadeInM=.10f;l.style.tube=1;l.style.core=.30f;
    l.marker.shape=valid?hs::ArcMarkerShape::Ring:hs::ArcMarkerShape::Cross;l.marker.rgba=hs::packRgba(r,g,b,.90f);l.marker.radiusM=.30f;
    l.marker.liftM=.02f;l.marker.depthTolM=.30f;l.marker.xrayAlpha=.45f;
    return l;
}

struct JitterStats {Counter stillFrames{},jitterUmSum{},jitterUmMax{};};
inline JitterStats jitterStats;
namespace detail {struct JitterState {std::uint64_t window{};V3 origin{},dir{},landing{};bool have{};};inline JitterState jitter;}

// World frame: key = ctx.key(); widths / colours interpolated along the length; depthTolM 0 along the arc, ramping to look.groundTolM over
// the last 1.0 m before a hit; marker at the landing (normal from the hit, else up; lift) when hit; trace.pending -> pendingLook (flag
// Pending, never Valid); flags Valid / Probed / Truncated / HandBlocked; tracer.markShown(trace) on the trace it encoded.
inline void toFrame(const Context& ctx,Tracer& tracer,const Trace& t,const Look& look0,bool valid,hs::ArcFrame& out) noexcept {
    out=hs::ArcFrame{};out.space=std::uint32_t(hs::ArcSpace::World);out.key=ctx.key();out.unitsPerMetre=t.upm>0.f?t.upm:1.f;
    if(!out.key||t.count<2){out.count=0;tracer.markShown(t,false);return;}
    const Look pl=pendingLook();const Look& L=t.pending?pl:look0;
    valid=valid&&!t.pending;
    const unsigned n=std::min(t.count,hs::arcMaxPoints);
    std::array<float,hs::arcMaxPoints> s{};for(unsigned i=1;i<n;++i)s[i]=s[i-1]+math::distance(t.p[i-1],t.p[i]);
    const float total=s[n-1],upm=std::max(out.unitsPerMetre,.01f);
    for(unsigned i=0;i<n;++i){
        auto& p=out.points[i];p.position[0]=t.p[i][0];p.position[1]=t.p[i][1];p.position[2]=t.p[i][2];
        const float f=total>0.f?s[i]/total:0.f;
        p.halfWidthM=L.halfWidthStartM+(L.halfWidthEndM-L.halfWidthStartM)*f;
        float c[4];for(unsigned k=0;k<4;++k)c[k]=hs::rgbaChannel(L.rgbaStart,k)+(hs::rgbaChannel(L.rgbaEnd,k)-hs::rgbaChannel(L.rgbaStart,k))*f;
        p.rgba=hs::packRgba(c[0],c[1],c[2],c[3]);
        const float fromEndM=(total-s[i])/upm;
        p.depthTolM=t.hit&&!t.pending&&fromEndM<1.f?std::clamp(L.groundTolM*(1.f-fromEndM),0.f,2.f):0.f;
    }
    out.count=n;out.style=L.style;
    if(t.hit&&!t.pending&&!t.handBlocked&&L.marker.shape!=hs::ArcMarkerShape::None){
        out.marker=L.marker;
        out.marker.centre[0]=t.landing.point[0];out.marker.centre[1]=t.landing.point[1];out.marker.centre[2]=t.landing.point[2];
        const V3 nrm=t.landing.normalValid?math::normalized(t.landing.normal):ctx.up();
        out.marker.normal[0]=nrm[0];out.marker.normal[1]=nrm[1];out.marker.normal[2]=nrm[2];
    }else{out.marker=hs::ArcMarker{};out.marker.shape=hs::ArcMarkerShape::None;}
    out.flags=(valid?hs::ArcFrame::Valid:0u)|(t.casts+t.reused?hs::ArcFrame::Probed:0u)|(t.pending?hs::ArcFrame::Pending:0u)|
        (t.truncated?hs::ArcFrame::Truncated:0u)|(t.handBlocked?hs::ArcFrame::HandBlocked:0u);
    if(!hs::validArc(out)){out.count=0;out.marker=hs::ArcMarker{};bump(tracerStats.nonFinite);}
    tracer.markShown(t,valid&&out.count);
    // landing jitter while the hand is still (comfort metric): origin moved < 1 cm and direction < 0.5 deg since the last frame
    auto& j=detail::jitter;
    if(t.hit&&!t.pending&&n>=2){
        const V3 o=t.p[0],d=math::normalized(math::sub(t.p[1],t.p[0]));
        if(j.have&&t.window==j.window+1&&math::distance(o,j.origin)<.01f*upm&&math::dot(d,j.dir)>.99996f){
            bump(jitterStats.stillFrames);const auto um=std::uint64_t(math::distance(t.landing.point,j.landing)/upm*1e6f);
            jitterStats.jitterUmSum.fetch_add(um,std::memory_order_relaxed);maxOf(jitterStats.jitterUmMax,um);}
        j={t.window,o,d,t.landing.point,true};
    }else j.have=false;
}

// ============================== ModuleArc tick: DEBUG producer + live levers ==============================
using PropertyReader=bool(*)(const char* name,char* value,std::size_t size) noexcept;
#ifdef __ANDROID__
inline bool androidPropertyReader(const char* name,char* value,std::size_t size) noexcept {
    char buffer[PROP_VALUE_MAX]{};const int n=__system_property_get(name,buffer);if(n<=0||!size)return false;std::snprintf(value,size,"%s",buffer);return true;}
inline std::atomic<PropertyReader> propertyReader{&androidPropertyReader};
#else
inline std::atomic<PropertyReader> propertyReader{nullptr};
#endif
inline int propertyInt(const char* name,int fallback) noexcept {
    const auto fn=propertyReader.load(std::memory_order_acquire);char text[96]{};
    if(!fn||!fn(name,text,sizeof text)||!text[0])return fallback;
    char* end{};const long v=std::strtol(text,&end,10);return end!=text&&*end==0?int(v):fallback;
}
inline std::atomic<int> testMode{0},filterEnabled{1};
struct ModuleStats {Counter ticks{},leverReads{},exceptions{},debugPublished{},debugCleared{},debugLocal{};};
inline ModuleStats moduleStats;
namespace detail {
struct DebugState {Tracer tracer;DirectionFilter filter;bool shown{},armed{};std::uint64_t lastLeverIndex{};bool leversRead{};};
inline DebugState& debugState() noexcept {static DebugState s;return s;}
inline void readLevers() noexcept {
    ledger.cap.store(unsigned(std::clamp(propertyInt("debug.rdrvr.vr.arc.casts",12),0,64)),std::memory_order_relaxed);
    ledger.commitReserve.store(unsigned(std::clamp(propertyInt("debug.rdrvr.vr.arc.commit",4),0,16)),std::memory_order_relaxed);
    filterEnabled.store(propertyInt("debug.rdrvr.vr.arc.filter",1)?1:0,std::memory_order_relaxed);
    testMode.store(std::clamp(propertyInt("debug.rdrvr.vr.arc.test",0),0,2),std::memory_order_relaxed);
    bump(moduleStats.leverReads);
}
inline void hideDebug(DebugState& d) noexcept {if(d.shown){hs::clearArc(hs::ArcChannel::Debug);d.shown=false;bump(moduleStats.debugCleared);}d.tracer.reset();d.filter.reset();}
// arc.test 2: a LOCAL-space parabola from the right controller (8 m/s, gravity -Y, no probes) - renderer-only smoke.
inline void debugLocal(const Context& c,DebugState& d) noexcept {
    const auto* raw=c.raw();if(!raw||!validPose(raw->aim[1])){hideDebug(d);return;}
    const auto& p=raw->aim[1];const V3 o{p.position[0],p.position[1],p.position[2]};const V3 dir=math::normalized(math::rotate(p.orientation,{0,0,-1}));
    hs::ArcFrame f{};f.space=std::uint32_t(hs::ArcSpace::Local);f.referenceSpace=raw->referenceSpace;
    const Look l=debugLook(true);f.style=l.style;
    unsigned n=0;const float v=8.f;
    for(unsigned i=0;i<hs::arcMaxPoints;++i){const float t=float(i)*.025f;
        const V3 q{o[0]+dir[0]*v*t,o[1]+dir[1]*v*t-4.905f*t*t,o[2]+dir[2]*v*t};
        auto& a=f.points[n++];a.position[0]=q[0];a.position[1]=q[1];a.position[2]=q[2];a.halfWidthM=.007f;a.rgba=l.rgbaStart;a.depthTolM=0;
        if(q[1]<o[1]-2.5f)break;}
    f.count=n;f.marker=l.marker;f.marker.shape=hs::ArcMarkerShape::Dot;
    const auto& e=f.points[n-1].position;f.marker.centre[0]=e[0];f.marker.centre[1]=e[1];f.marker.centre[2]=e[2];
    f.flags=hs::ArcFrame::Valid;
    hs::publishArc(hs::ArcChannel::Debug,f);d.shown=true;bump(moduleStats.debugPublished);bump(moduleStats.debugLocal);
}
} // namespace detail
inline void tick(const mainthread::Frame& fr) noexcept {
    try{
        bump(moduleStats.ticks);auto& d=detail::debugState();
        if(!d.leversRead||fr.index>=d.lastLeverIndex+64||fr.index<d.lastLeverIndex){detail::readLevers();d.leversRead=true;d.lastLeverIndex=fr.index;}
        const int test=testMode.load(std::memory_order_relaxed);
        if(!test||killed(Feature::Arc)||killed(Feature::Hands)){
            detail::hideDebug(d);if(d.armed){hs::armArc(hs::ArcChannel::Debug,false);d.armed=false;}return;}
        if(!d.armed){hs::armArc(hs::ArcChannel::Debug,true);d.armed=true;}
        const auto st=hs::arcStatus();
        if(st==hs::ArcStatus::Failed||st==hs::ArcStatus::Killed||st==hs::ArcStatus::Suppressed){detail::hideDebug(d);return;}
        auto& c=frameContext(fr.now);
        if(!c.inputValid()){detail::hideDebug(d);return;}
        if(test==2){detail::debugLocal(c,d);return;}
        if(!c.world()){detail::hideDebug(d);return;}
        V3 o{},dir{};MapInputs m{};
        if(!c.aimRay(1,o,dir)||!c.map(m)){detail::hideDebug(d);return;} // RIGHT physical controller
        V3 s{};if(c.startPoint(1,s)==StartSource::None)s=o;
        if(filterEnabled.load(std::memory_order_relaxed))dir=d.filter.filter(dir,fr.now);
        Launch L{};L.origin=math::add(s,math::mul(dir,.05f*m.upm));L.velocity=math::mul(dir,8.f*m.upm);L.maxTimeS=2.5f;L.maxRangeM=25.f;L.maxDropM=15.f;
        TraceConfig cfg{};cfg.chordDtS=.12f;cfg.maxChords=16;cfg.pointsPerChord=4;cfg.losFromEye=true;cfg.losSelect=Select::AnySurface;
        cfg.query.include=0x500208;cfg.query.select=Select::BulletStopper;cfg.user=ProbeUser::Debug;
        const Trace t=d.tracer.run(c,L,cfg);
        const bool valid=!t.pending&&!t.handBlocked&&t.hit&&t.landing.normalValid&&math::dot(math::normalized(t.landing.normal),c.up())>.7f;
        hs::ArcFrame f{};toFrame(c,d.tracer,t,debugLook(valid),valid,f);
        hs::publishArc(hs::ArcChannel::Debug,f);d.shown=true;bump(moduleStats.debugPublished);
    }catch(...){bump(moduleStats.exceptions);}
}
// Startup (GameXr gameConfigureInput): registers ModuleArc. Idempotent.
inline bool install() noexcept {return mainthread::registerModule(mainthread::ModuleArc,"arc",&tick,nullptr);}
inline void resetForTests() noexcept {
    ledger.resetForTests();resetContextForTests();detail::debugState().tracer.reset();detail::debugState().filter.reset();
    detail::debugState().shown=detail::debugState().armed=false;detail::debugState().leversRead=false;detail::debugState().lastLeverIndex=0;
    detail::jitter=detail::JitterState{};
}

// ============================== reports ==============================
inline std::string report() {
    std::string o;o.reserve(2048);char b[1024];
    const auto& cs=contextStats;const auto& ts=tracerStats;
    std::snprintf(b,sizeof b,"{\"ticks\":%llu,\"contexts\":{\"built\":%llu,\"noAnchor\":%llu,\"notWorld\":%llu,\"noActor\":%llu,\"noMap\":%llu,\"noInput\":%llu,"
        "\"spaceMismatch\":%llu,\"untracked\":[%llu,%llu]}",
        (unsigned long long)u64(moduleStats.ticks),(unsigned long long)u64(cs.built),(unsigned long long)u64(cs.noAnchor),(unsigned long long)u64(cs.notWorld),
        (unsigned long long)u64(cs.noActor),(unsigned long long)u64(cs.noMap),(unsigned long long)u64(cs.noInput),(unsigned long long)u64(cs.spaceMismatch),
        (unsigned long long)u64(cs.untracked[0]),(unsigned long long)u64(cs.untracked[1]));o+=b;
    const auto th=u64(cs.toHandSamples);
    std::snprintf(b,sizeof b,",\"start\":{\"body\":%llu,\"controller\":%llu,\"none\":%llu,\"handBoxStale\":%llu,\"handBoxes\":%llu,\"toHandMmAvg\":%.2f,"
        "\"toHandMmMaxSteady\":%.2f,\"toHandMmMaxBridged\":%.2f}",
        (unsigned long long)u64(cs.startBody),(unsigned long long)u64(cs.startController),(unsigned long long)u64(cs.startNone),
        (unsigned long long)u64(cs.handBoxStale),(unsigned long long)u64(bodyik::handBoxPublished),
        th?double(u64(cs.toHandUmSum))/double(th)/1000.0:0.0,double(u64(cs.toHandUmMaxSteady))/1000.0,double(u64(cs.toHandUmMaxBridged))/1000.0);o+=b;
    std::snprintf(b,sizeof b,",\"tracer\":{\"runs\":%llu,\"chords\":%llu,\"cast\":%llu,\"reused\":%llu,\"unknown\":%llu,\"failed\":%llu,\"pendingFrames\":%llu,"
        "\"hits\":%llu,\"handBlocked\":%llu,\"truncated\":%llu,\"nonFinite\":%llu}",
        (unsigned long long)u64(ts.runs),(unsigned long long)u64(ts.chords),(unsigned long long)u64(ts.cast),(unsigned long long)u64(ts.reused),
        (unsigned long long)u64(ts.unknown),(unsigned long long)u64(ts.failed),(unsigned long long)u64(ts.pendingFrames),(unsigned long long)u64(ts.hits),
        (unsigned long long)u64(ts.handBlocked),(unsigned long long)u64(ts.truncated),(unsigned long long)u64(ts.nonFinite));o+=b;
    const auto acc=u64(ts.accepted);
    std::snprintf(b,sizeof b,",\"commit\":{\"checks\":%llu,\"accepted\":%llu,\"rejected\":{\"changed\":%llu,\"failed\":%llu,\"timeout\":%llu,\"noContext\":%llu},"
        "\"casts\":%llu,\"shownToCommittedCmAvg\":%.2f,\"shownToCommittedCmMax\":%.2f,\"rejectedAfterShownValid\":%llu,\"cancelled\":%llu,\"expired\":%llu}",
        (unsigned long long)u64(ts.checks),(unsigned long long)acc,(unsigned long long)u64(ts.changed),(unsigned long long)u64(ts.rejectedFailed),
        (unsigned long long)u64(ts.timeout),(unsigned long long)u64(ts.noContext),(unsigned long long)u64(ts.commitCasts),
        acc?double(u64(ts.shownToCommittedUmSum))/double(acc)/1e4:0.0,double(u64(ts.shownToCommittedUmMax))/1e4,
        (unsigned long long)u64(ts.rejectedAfterShownValid),(unsigned long long)u64(ts.cancelled),(unsigned long long)u64(ts.expired));o+=b;
    o+=",\"channels\":[";
    constexpr std::array<ProbeUser,hs::arcChannels> channelUser{ProbeUser::Lasso,ProbeUser::Teleport,ProbeUser::Count,ProbeUser::Debug,ProbeUser::Count}; // THROW / MARKER: no probes
    for(unsigned c=0;c<hs::arcChannels;++c){
        const auto st=hs::arcChannelStats(hs::ArcChannel(c));const auto u=channelUser[c];
        std::snprintf(b,sizeof b,"%s{\"name\":\"%s\",\"published\":%llu,\"cleared\":%llu,\"hidden\":%llu,\"wrongThread\":%llu,\"rejected\":%llu,\"budgetStarved\":%llu}",
            c?",":"",hs::arcChannelNames[c],(unsigned long long)st.published,(unsigned long long)st.cleared,(unsigned long long)st.hides,
            (unsigned long long)st.wrongThread,(unsigned long long)st.rejected,u<ProbeUser::Count?(unsigned long long)u64(ledger.users[unsigned(u)].budgetStarved):0ull);o+=b;}
    const auto sf=u64(jitterStats.stillFrames);
    std::snprintf(b,sizeof b,"],\"jitter\":{\"stillFrames\":%llu,\"landingJitterCmAvg\":%.2f,\"landingJitterCmMax\":%.2f},\"filter\":%d,\"test\":%d,"
        "\"status\":\"%s\",\"statusNote\":\"%s\",\"debug\":{\"published\":%llu,\"cleared\":%llu,\"local\":%llu},\"leverReads\":%llu,\"exceptions\":%llu,"
        "\"engineApi\":%s,\"capsuleApi\":%s}",
        (unsigned long long)sf,sf?double(u64(jitterStats.jitterUmSum))/double(sf)/1e4:0.0,double(u64(jitterStats.jitterUmMax))/1e4,
        filterEnabled.load(),testMode.load(),hs::arcStatusName(hs::arcStatus()),hs::arcStatusNote(),
        (unsigned long long)u64(moduleStats.debugPublished),(unsigned long long)u64(moduleStats.debugCleared),(unsigned long long)u64(moduleStats.debugLocal),
        (unsigned long long)u64(moduleStats.leverReads),(unsigned long long)u64(moduleStats.exceptions),
        engineApi.load()&&*engineApi.load()?"true":"false",engineApi.load()&&engineApi.load()->capsule?"true":"false");o+=b;
    return o;
}
inline std::string probesReport() {
    std::string o;o.reserve(2048);char b[640];const auto w=u64(ledger.windows);
    std::snprintf(b,sizeof b,"{\"cap\":%u,\"commitReserve\":%u,\"windows\":%llu,\"perWindow\":{\"castsMax\":%llu,\"usAvg\":%.1f,\"usMax\":%.1f},\"overCap\":%llu,\"users\":[",
        ledger.cap.load(),ledger.commitReserve.load(),(unsigned long long)w,(unsigned long long)u64(ledger.castsMax),
        w?double(u64(ledger.windowNsSum))/double(w)/1e3:0.0,double(u64(ledger.windowNsMax))/1e3,(unsigned long long)u64(ledger.overCap));o+=b;
    for(unsigned u=0;u<probeUsers;++u){const auto& s=ledger.users[u];const auto n=u64(s.casts);const auto mm=u64(s.mmSum);
        std::snprintf(b,sizeof b,"%s{\"name\":\"%s\",\"casts\":%llu,\"refused\":%llu,\"failed\":%llu,\"usAvg\":%.1f,\"usMax\":%.1f,\"mAvg\":%.2f,"
            "\"usPerMAvg\":%.1f,\"usPerMMax\":%.1f,\"budgetStarved\":%llu,\"demandWindows\":%llu}",
            u?",":"",probeUserNames[u],(unsigned long long)n,(unsigned long long)u64(s.refused),(unsigned long long)u64(s.failed),
            n?double(u64(s.nsSum))/double(n)/1e3:0.0,double(u64(s.nsMax))/1e3,n?double(mm)/double(n)/1e3:0.0,
            mm?double(u64(s.nsSum))/(double(mm)/1e3)/1e3:0.0,double(u64(s.nsPerMMax))/1e3,(unsigned long long)u64(s.budgetStarved),
            (unsigned long long)u64(s.demandWindows));o+=b;}
    std::snprintf(b,sizeof b,"],\"excludeCache\":{\"hits\":%llu,\"misses\":%llu,\"failed\":%llu}}",
        (unsigned long long)u64(ledger.excludeHits),(unsigned long long)u64(ledger.excludeMisses),(unsigned long long)u64(ledger.excludeFailed));o+=b;
    return o;
}
} // namespace rdrvr::vr::arc
