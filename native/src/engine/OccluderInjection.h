#pragma once
// Native occluder quad injection: STEREO TEST modes OCCLUDER A/B TEST (3, 30 frames on / 30 off)
// and OCCLUDERS ON (4). Inactive in every other mode. Quads come from OccluderTable.h (generated offline by
// a script from the Armadillo .rdrocc capture).
//
// Retail librdr.so facts this relies on (from the disassembly):
// - PrepareVisibilityList (+2abc8e0..8f4) sets MAIN (kind 13) request+0x10 to
//   renVisibilityListMgr::sm_OccShaftList (+4e8c1480, 24 x 0x330 spdShaft); the next
//   symbol (sm_SharedGlobalRoomList, +4e8c6100) starts exactly 24 slots later.
// - request+0x2c (occluder shaft count) is zeroed by the constructor and by
//   ResetVisibilityRequests (+2abce8c); Android never publishes a nonzero count.
// - Consumers iterate EXACTLY count shafts with no cap of their own
//   (CreateDrawListsOutDoorSS +2a7129c static nodes / skip subtree, +2a719d0 dynamic
//   objects, +2a7203c; CreateVisibleShaftList +2a73ffc/+2a74450). The only engine
//   bound is the 24-slot array (the native producer also admits at most count 23+1).
//   A bound is rejected only when wholly inside one occluder shaft (planes at +0x1c0,
//   inside n.p-w<=0); every extra shaft costs one 6-plane AABB test per
//   frustum-visible node and dynamic object, so the runtime cap is kInject (8).
// - spdShaft::IntersectOccluder(+1b978e8) needs a 4-point spdPolygon: +0x50 plane,
//   +0x60 centre, +0x70 Vector3* (16-byte stride), +0x78 u16 count. It returns true
//   only when all four points project (spdPolygon::Project +1b9b808).
// Hook: VisibilityCullBoundary::cached AFTER native CreateVisibleShaftList returned
// (audited MAIN union scope), i.e. before CreateVisibilityListPre calls AddToDrawList
// (+2abbbe4) -> CreateDrawListsOutDoorSS. The sector/portal walk is not affected.
#include "StereoTestMode.h"
#include "OccluderTable.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace rdrvr::engine {
class OccluderInjection {
public:
    using Read=bool(*)(void*,std::uintptr_t,void*,std::size_t);
    static constexpr std::uintptr_t computePlaneOffset=0x1b9b6f0,computeCenterOffset=0x1b9b668,
        shaftAssignOffset=0x21eb8ac,intersectOccluderOffset=0x1b978e8,occShaftListOffset=0x4e8c1480;
    // First 32 bytes of each retail function (position independent, verified on the APK library).
    static constexpr std::array<unsigned char,32> computePlaneFingerprint{
#include "gamecode/OccluderInjection_1b9b6f0_32_8d.inc"
};
    static constexpr std::array<unsigned char,32> computeCenterFingerprint{
#include "gamecode/OccluderInjection_1b9b668_32_8d.inc"
};
    static constexpr std::array<unsigned char,32> shaftAssignFingerprint{
#include "gamecode/OccluderInjection_21eb8ac_32_8d.inc"
};
    static constexpr std::array<unsigned char,32> intersectOccluderFingerprint{
#include "gamecode/OccluderInjection_1b978e8_32_8d.inc"
};
    static constexpr unsigned mainKind=13,windowFrames=30,ringFrames=32;
    static constexpr unsigned hardCap=24;      // sm_OccShaftList slots
    static constexpr unsigned kInject=8;       // runtime cap (consumer cost per extra shaft)
    static constexpr unsigned builderBudget=12;// candidates tried with the engine builder per frame
    static constexpr std::uint64_t builderTimeBudgetNs=250000;
    static_assert(kInject<=hardCap&&builderBudget>=kInject);
    static constexpr std::size_t shaftBytes=0x330,polygonBytes=0x130;
    static constexpr float maxCenterDistance=80.f,minPlaneDistance=2.f,selfTestBehind=3.f;
    static constexpr std::size_t tableSize=occluder_table::quads.size();
    static_assert(occluder_table::facadeBegin+occluder_table::facadeCount==tableSize&&occluder_table::wallBegin+occluder_table::wallCount<=tableSize);
    // Table selection: property debug.rdrvr.occluders.table = walls | facades (default walls), re-read every 64
    // mode frames. Facades are EXPERIMENTAL (windows/doors treated as opaque).
    enum class Table : unsigned {Walls=0,Facades=1};
    // Rooms: the native producer generated occluders only when rooms==false
    // (public CreateVisibleShaftList calls the private builder with w3=1 only on the no-room
    // branch, +2a73360/+2a7338c; gate +2a73d40..54). With rooms, AddToDrawList runs
    // CreateDrawListsOutDoor (no occluder consumers) when +0xa0!=0 && +0x30!=0
    // (+2a70040..64), otherwise CreateDrawListsOutDoorSS (consumers). Only the
    // former is declined ("roomsOutdoorPath"); rooms without room records still get the injection.
    enum Decline : unsigned {Rooms,Special,Viewport,Request,Kind,Array,CountBusy,Camera,Shaft,NoCandidate,NoneAccepted,DeclineCount};
    enum QuadReject : unsigned {Range,PlaneDistance,AboveTop,OutOfView,Duplicate,PlaneMismatch,IntersectFalse,SelfTest,QuadRejectCount};
    using ComputePlane=bool(*)(void*);
    using ComputeCenter=void(*)(void*);
    using Assign=void*(*)(void*,const void*);
    using Intersect=bool(*)(void*,const void*);
private:
    struct alignas(16) Vertex {float x,y,z,w;};
    struct alignas(16) Polygon {unsigned char bytes[polygonBytes];};
    struct Ring {
        std::array<std::array<std::atomic<std::uint32_t>,6>,ringFrames> values{};
        std::atomic<std::uint64_t> count{};
    };
    struct Candidate {float score;unsigned index;};
    std::atomic<bool> configured_{};
    std::uintptr_t base_{};Read read_{};
    ComputePlane computePlane_{};ComputeCenter computeCenter_{};Assign assign_{};Intersect intersect_{};
    // Visibility-task side (one MAIN request at a time).
    std::atomic<std::uint64_t> calls_{},modeFrames_{},attempted_{},accepted_{},offWindow_{},staleCleared_{},builderCalls_{};
    std::array<std::atomic<std::uint64_t>,DeclineCount> declined_{};
    std::array<std::atomic<std::uint64_t>,QuadRejectCount> quadRejects_{};
    std::array<std::atomic<std::uint64_t>,2> intersectResult_{};
    std::array<std::atomic<std::uint32_t>,tableSize> quadEligible_{},quadAccepted_{};
    std::atomic<unsigned> table_{unsigned(Table::Walls)};std::atomic<std::uint64_t> roomsInjectedFrames_{},roomsSeen_{};
    std::array<std::atomic<std::uint64_t>,2> tableFrames_{};
    std::atomic<std::uintptr_t> published_{},request_{};
    std::atomic<std::uint64_t> seq_{};std::atomic<unsigned> state_{}; // 0 none, 1 injected, 2 off-window eligible
    std::array<std::atomic<std::uint32_t>,3> camera_{};
    std::atomic<std::uint32_t> lastEligible_{},lastBest_{UINT32_MAX},bestPlaneDistance_{},bestCenterDistance_{},behindMax_{},frontMax_{};
    std::atomic<std::uint32_t> injectedLast_{},injectedMax_{};std::atomic<std::uint64_t> injectedSum_{},injectedFrames_{};
    std::atomic<std::uint64_t> injectNsLast_{},injectNsMax_{},injectNsSum_{},injectNsFrames_{};
    std::atomic<bool> captured_{},capturedPass_{};std::atomic<unsigned> capturedQuad_{};
    // Self test outcomes of successful builder calls: [0] pass, [1] fail.
    std::array<std::atomic<std::uint64_t>,2> selfTest_{};
    std::array<std::atomic<std::uint32_t>,24> slotPlanes_{};
    std::array<std::atomic<std::uint32_t>,4> polygonPlane_{};
    // Per-frame facts published by afterCached() (visibility task) for sample() (render thread);
    // seq_ (release in finish) orders them.
    std::atomic<unsigned> frameMode_{},frameInjected_{};std::atomic<std::uint64_t> frameIndex_{};
    std::array<std::atomic<std::uint32_t>,3> frameCamera_{},frameForward_{};
    // Render-thread side (left color exit) and body draw counter.
    std::atomic<std::uint64_t> bodies_{},sampleReadFailures_{};
    std::uint64_t lastBodies_{},lastSeq_{};
    std::atomic<std::uint32_t> lastFrameBodies_{};
    // Rings: on/off only in the A/B mode (inject windows / compare windows), always only in the always-on mode.
    Ring on_{},off_{},always_{};
    // Paired measurement: one summary per 30-frame A/B window (last 16) and the last 8 raw samples.
    enum Metric : unsigned {MInjected,MStaticChild,MDynamic,MStaticNode,MTotal,MBodies,MDepthDraws,MColorDraws,MDepthObjects,MColorObjects,
        MGrassFields,MetricCount}; // MGrassFields = visible grass fields of the exact left colour grass draw (grassManager+0xa0b8)
    struct FrameSample {std::uint64_t frame{};unsigned mode{},state{};std::array<float,MetricCount> values{};std::array<bool,MetricCount> valid{};
        std::array<float,3> camera{},forward{};};
    struct Window {std::uint64_t index{};bool on{};unsigned frames{},injectedFrames{},eligibleFrames{};
        std::array<double,MetricCount> sum{};std::array<unsigned,MetricCount> n{};
        std::array<float,3> firstCamera{},camera{},firstForward{},forward{};};
    static constexpr unsigned windowHistory=16,recentFrames=8;
    mutable std::mutex measure_;
    std::array<Window,windowHistory> windows_{};std::uint64_t windowCount_{};Window current_{};bool currentOpen_{};
    std::array<FrameSample,recentFrames> recent_{};std::uint64_t recentCount_{};

    static std::uint32_t bits(float v)noexcept{return std::bit_cast<std::uint32_t>(v);}
    static float value(const std::atomic<std::uint32_t>& v)noexcept{return std::bit_cast<float>(v.load(std::memory_order_relaxed));}
    static std::uint64_t now()noexcept{
        return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    static void raise(std::atomic<std::uint64_t>& slot,std::uint64_t v)noexcept{
        auto old=slot.load(std::memory_order_relaxed);while(old<v&&!slot.compare_exchange_weak(old,v,std::memory_order_relaxed)){}
    }
    bool read(std::uintptr_t address,void* out,std::size_t size)const noexcept{
        try{return read_&&address&&read_(nullptr,address,out,size);}catch(...){return false;}
    }
    // Relaxed 32-bit store into engine memory (request+0x2c is 4-byte aligned).
    static void store32(std::uintptr_t address,std::uint32_t v)noexcept{
#if defined(_MSC_VER)&&!defined(__clang__)
        *reinterpret_cast<volatile std::uint32_t*>(address)=v; // host tests only
#else
        __atomic_store_n(reinterpret_cast<std::uint32_t*>(address),v,__ATOMIC_RELAXED);
#endif
    }
    static Table readTableProperty()noexcept{
#ifdef __ANDROID__
        char value[PROP_VALUE_MAX]{};__system_property_get("debug.rdrvr.occluders.table",value);
        if(!std::strcmp(value,"facades"))return Table::Facades;
#endif
        return Table::Walls;
    }
    // Caller holds measure_.
    void closeWindow()noexcept{
        if(currentOpen_&&current_.frames)windows_[windowCount_++%windowHistory]=current_;
        currentOpen_=false;
    }
    void decline(Decline d)noexcept{declined_[d].fetch_add(1,std::memory_order_relaxed);}
    void reject(QuadReject r)noexcept{quadRejects_[r].fetch_add(1,std::memory_order_relaxed);}
    void finish(std::uintptr_t request,unsigned state)noexcept{
        request_.store(request,std::memory_order_relaxed);state_.store(state,std::memory_order_relaxed);
        seq_.fetch_add(1,std::memory_order_release);
    }
    void clearStale(std::uintptr_t request)noexcept{
        const auto previous=published_.exchange(0,std::memory_order_relaxed);
        if(!previous||previous!=request)return;
        std::uint32_t count{};
        if(read(request+0x2c,&count,4)&&count){
            store32(request+0x2c,0u);
            staleCleared_.fetch_add(1,std::memory_order_relaxed);
        }
    }
    static float dot(const float* a,const float* b)noexcept{return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
    static float planeValue(const float* plane,const float* p)noexcept{return dot(plane,p)-plane[3];}
    static void fill(const occluder_table::Quad& q,Polygon& polygon,std::array<Vertex,4>& vertices)noexcept{
        for(unsigned i=0;i<4;++i){const auto& p=q.points[i];vertices[i]={p[0],p[1],p[2],0};}
        std::memset(polygon.bytes,0,sizeof polygon.bytes);
        const Vertex* pointer=vertices.data();const std::uint16_t count=4;
        std::memcpy(polygon.bytes+0x70,&pointer,sizeof pointer);
        std::memcpy(polygon.bytes+0x78,&count,2);std::memcpy(polygon.bytes+0x7a,&count,2);
    }
    // Engine builder for one table quad into slot: ONE builder call per candidate.
    // Winding does not matter: spdShaft::SetFrontPlane (+1b94800) negates the stored
    // plane whenever the apex is on its negative side and IntersectOccluder sorts the
    // four projected points, so both vertex orders build the same shaft. The self test
    // is the only orientation guard: a point 3 m behind the quad on the apex->centre ray
    // must be inside all six occluder planes; the apex-quad midpoint outside at least one.
    bool build(unsigned index,unsigned char* slot,const void* mainShaft,const float* camera)noexcept{
        const auto& q=occluder_table::quads[index];
        const float delta[3]{q.center[0]-camera[0],q.center[1]-camera[1],q.center[2]-camera[2]};
        const float length=std::sqrt(dot(delta,delta));
        const float behind[3]{camera[0]+delta[0]*(length+selfTestBehind)/length,camera[1]+delta[1]*(length+selfTestBehind)/length,
            camera[2]+delta[2]*(length+selfTestBehind)/length};
        const float front[3]{camera[0]+delta[0]*0.5f,camera[1]+delta[1]*0.5f,camera[2]+delta[2]*0.5f};
        Polygon polygon{};std::array<Vertex,4> vertices{};float plane[4]{};
        fill(q,polygon,vertices);
        computePlane_(polygon.bytes);
        std::memcpy(plane,polygon.bytes+0x50,sizeof plane);
        // Engine plane must match the table normal (either sign) and pass through the quad
        // (checked per point: comparing w directly is ill-conditioned at |p| ~ 3000 m).
        if(!(std::fabs(dot(plane,q.normal))>0.999f)||!(std::fabs(planeValue(plane,q.points[0]))<0.05f)||
           !(std::fabs(planeValue(plane,q.points[2]))<0.05f)){reject(PlaneMismatch);return false;}
        computeCenter_(polygon.bytes);
        assign_(slot,mainShaft);
        builderCalls_.fetch_add(1,std::memory_order_relaxed);
        const bool intersected=intersect_(slot,polygon.bytes);
        intersectResult_[intersected?0:1].fetch_add(1,std::memory_order_relaxed);
        if(!intersected){reject(IntersectFalse);return false;}
        float planes[24]{};std::memcpy(planes,slot+0x1c0,sizeof planes);
        float behindMax=-INFINITY,frontMax=-INFINITY;
        for(unsigned i=0;i<6;++i){behindMax=std::fmax(behindMax,planeValue(planes+4*i,behind));frontMax=std::fmax(frontMax,planeValue(planes+4*i,front));}
        behindMax_.store(bits(behindMax),std::memory_order_relaxed);frontMax_.store(bits(frontMax),std::memory_order_relaxed);
        const bool pass=behindMax<-0.01f&&frontMax>0.f;
        if(!captured_.load(std::memory_order_relaxed)){
            for(unsigned i=0;i<24;++i)slotPlanes_[i].store(bits(planes[i]),std::memory_order_relaxed);
            for(unsigned i=0;i<4;++i)polygonPlane_[i].store(bits(plane[i]),std::memory_order_relaxed);
            capturedPass_.store(pass,std::memory_order_relaxed);capturedQuad_.store(index,std::memory_order_relaxed);
            captured_.store(true,std::memory_order_release);
        }
        selfTest_[pass?0:1].fetch_add(1,std::memory_order_relaxed);
        if(!pass){reject(SelfTest);return false;}
        return true;
    }
    // Opposite faces / near-coplanar copies: a candidate parallel (|n.n'|>0.99) to an
    // already injected quad, within 0.3 m of its plane and with overlapping in-plane
    // bounding circles adds no culling beyond that quad.
    static bool duplicateOf(const occluder_table::Quad& a,const occluder_table::Quad& b)noexcept{
        if(!(std::fabs(dot(a.normal,b.normal))>0.99f))return false;
        const float d[3]{a.center[0]-b.center[0],a.center[1]-b.center[1],a.center[2]-b.center[2]};
        const float along=dot(b.normal,d);
        if(!(std::fabs(along)<0.3f))return false;
        const float inPlane[3]{d[0]-b.normal[0]*along,d[1]-b.normal[1]*along,d[2]-b.normal[2]*along};
        return std::sqrt(dot(inPlane,inPlane))<radius(a)+radius(b);
    }
    static float radius(const occluder_table::Quad& q)noexcept{
        const float d[3]{q.points[2][0]-q.points[0][0],q.points[2][1]-q.points[0][1],q.points[2][2]-q.points[0][2]};
        return 0.5f*std::sqrt(dot(d,d));
    }
public:
    template<class Check> static bool codeMatches(std::uintptr_t base,Check&& check){
        return check(base+computePlaneOffset,computePlaneFingerprint)&&check(base+computeCenterOffset,computeCenterFingerprint)&&
            check(base+shaftAssignOffset,shaftAssignFingerprint)&&check(base+intersectOccluderOffset,intersectOccluderFingerprint);
    }
    // Caller verifies codeMatches() and that base+occShaftListOffset is RW mapped.
    void configure(std::uintptr_t base,Read read)noexcept{
        if(configured_.load(std::memory_order_acquire)||!base||!read)return;
        base_=base;read_=read;
        computePlane_=reinterpret_cast<ComputePlane>(base+computePlaneOffset);
        computeCenter_=reinterpret_cast<ComputeCenter>(base+computeCenterOffset);
        assign_=reinterpret_cast<Assign>(base+shaftAssignOffset);
        intersect_=reinterpret_cast<Intersect>(base+intersectOccluderOffset);
        configured_.store(true,std::memory_order_release);
    }
    // Host tests only: explicit builder functions instead of base+offset.
    void configureForTest(std::uintptr_t base,Read read,ComputePlane plane,ComputeCenter center,Assign assign,Intersect intersect)noexcept{
        base_=base;read_=read;computePlane_=plane;computeCenter_=center;assign_=assign;intersect_=intersect;
        configured_.store(true,std::memory_order_release);
    }
    bool configured()const noexcept{return configured_.load(std::memory_order_acquire);}
    void body()noexcept{bodies_.store(bodies_.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);} // cpu02: render thread only (pinned DrawCommon): no atomic RMW per object
    // Visibility task, audited MAIN union scope, after native CreateVisibleShaftList.
    void afterCached(void* requestPointer,const void* viewport,bool rooms,bool special)noexcept{
        if(!configured_.load(std::memory_order_acquire)||!requestPointer)return;
        const int incoming=errno;struct Restore{int value;~Restore(){errno=value;}} restore{incoming};
        const auto request=reinterpret_cast<std::uintptr_t>(requestPointer);
        calls_.fetch_add(1,std::memory_order_relaxed);
        clearStale(request);
        const unsigned mode=stereo::testMode.load(std::memory_order_relaxed);
        frameMode_.store(mode,std::memory_order_relaxed);frameInjected_.store(0,std::memory_order_relaxed);
        if(mode!=stereo::occluderTestMode&&mode!=stereo::occluderAlwaysMode){finish(request,0);return;}
        const auto frame=modeFrames_.fetch_add(1,std::memory_order_relaxed);
        frameIndex_.store(frame,std::memory_order_relaxed);
        const bool inject=mode==stereo::occluderAlwaysMode||((frame/windowFrames)&1)==0;
        // Union camera (Matrix34 at viewport+0x40: rows a,b,c,d; d = apex, view along -c),
        // published for the paired measurement before any decline.
        float rows[16]{};bool cameraOk=false;
        if(viewport&&read(reinterpret_cast<std::uintptr_t>(viewport)+0x40,rows,sizeof rows)){
            cameraOk=std::isfinite(rows[12])&&std::isfinite(rows[13])&&std::isfinite(rows[14]);
            for(unsigned i=0;i<3;++i){frameCamera_[i].store(bits(rows[12+i]),std::memory_order_relaxed);frameForward_[i].store(bits(-rows[8+i]),std::memory_order_relaxed);}
        }
        if((frame&63)==0)table_.store(unsigned(readTableProperty()),std::memory_order_relaxed);
        const auto table=Table(table_.load(std::memory_order_relaxed));
        const std::size_t rangeBegin=table==Table::Walls?occluder_table::wallBegin:occluder_table::facadeBegin;
        const std::size_t rangeEnd=rangeBegin+(table==Table::Walls?occluder_table::wallCount:occluder_table::facadeCount);
        tableFrames_[unsigned(table)&1u].fetch_add(1,std::memory_order_relaxed);
        auto no=[&](Decline d){decline(d);finish(request,0);};
        if(rooms){
            roomsSeen_.fetch_add(1,std::memory_order_relaxed);
            std::uintptr_t room{};std::uint32_t roomRecords{};
            if(!read(request+0xa0,&room,sizeof room)||!read(request+0x30,&roomRecords,4))return no(Request);
            if(room&&roomRecords)return no(Rooms); // CreateDrawListsOutDoor path: no occluder consumers
        }
        if(special)return no(Special);
        if(!viewport)return no(Viewport);
        std::uint32_t kind{},count{};std::uintptr_t array{};
        if(!read(request+0xcc,&kind,4)||!read(request+0x10,&array,sizeof array)||!read(request+0x2c,&count,4))return no(Request);
        if(kind!=mainKind)return no(Kind);
        if(array!=base_+occShaftListOffset)return no(Array);
        if(count)return no(CountBusy);
        if(!cameraOk)return no(Camera);
        const float camera[3]{rows[12],rows[13],rows[14]};
        for(unsigned i=0;i<3;++i)camera_[i].store(bits(camera[i]),std::memory_order_relaxed);
        // Main (union) shaft side planes: a quad centre must be in view, because the
        // self test probes the apex->centre ray.
        float view[24]{};
        if(!read(request+0x150+0x1c0,view,sizeof view))return no(Shaft);
        const auto start=now();
        std::array<Candidate,tableSize> candidates{};unsigned eligible=0;
        float bestScore=-1;unsigned best=UINT32_MAX;float bestPlane=0,bestDistance=0;
        for(unsigned i=unsigned(rangeBegin);i<unsigned(rangeEnd);++i){
            const auto& q=occluder_table::quads[i];
            const float delta[3]{q.center[0]-camera[0],q.center[1]-camera[1],q.center[2]-camera[2]};
            const float distance=std::sqrt(dot(delta,delta));
            if(!(distance<=maxCenterDistance)||!(distance>0.f)){reject(Range);continue;}
            const float signedPlane=dot(q.normal,camera)-dot(q.normal,q.points[0]);
            if(!(std::fabs(signedPlane)>minPlaneDistance)){reject(PlaneDistance);continue;}
            const float top=std::max(std::max(q.points[0][1],q.points[1][1]),std::max(q.points[2][1],q.points[3][1]));
            if(camera[1]>top){reject(AboveTop);continue;}
            bool inView=true;for(unsigned p=0;p<6&&inView;++p)inView=planeValue(view+4*p,q.center)<=0.f;
            if(!inView){reject(OutOfView);continue;}
            const float score=q.area*std::fabs(dot(q.normal,delta))/(distance*distance*distance);
            candidates[eligible++]={score,i};quadEligible_[i].fetch_add(1,std::memory_order_relaxed);
            if(score>bestScore){bestScore=score;best=i;bestPlane=signedPlane;bestDistance=distance;}
        }
        lastEligible_.store(eligible,std::memory_order_relaxed);lastBest_.store(best,std::memory_order_relaxed);
        bestPlaneDistance_.store(bits(bestPlane),std::memory_order_relaxed);bestCenterDistance_.store(bits(bestDistance),std::memory_order_relaxed);
        if(!eligible)return no(NoCandidate);
        if(!inject){offWindow_.fetch_add(1,std::memory_order_relaxed);finish(request,2);return;}
        attempted_.fetch_add(1,std::memory_order_relaxed);
        std::sort(candidates.begin(),candidates.begin()+eligible,[](const Candidate& a,const Candidate& b){return a.score>b.score;});
        const auto* mainShaft=reinterpret_cast<const void*>(request+0x150);
        unsigned injected=0,built=0;std::array<unsigned,kInject> chosen{};
        for(unsigned t=0;t<eligible&&injected<kInject&&built<builderBudget;++t){
            const unsigned index=candidates[t].index;
            bool duplicate=false;
            for(unsigned k=0;k<injected&&!duplicate;++k)duplicate=duplicateOf(occluder_table::quads[index],occluder_table::quads[chosen[k]]);
            if(duplicate){reject(Duplicate);continue;}  // no builder call spent
            if(built&&now()-start>builderTimeBudgetNs)break;
            ++built;
            auto* slot=reinterpret_cast<unsigned char*>(array+std::uintptr_t(injected)*shaftBytes); // 16-byte aligned
            if(build(index,slot,mainShaft,camera)){quadAccepted_[index].fetch_add(1,std::memory_order_relaxed);chosen[injected++]=index;}
        }
        const auto elapsed=now()-start;
        injectNsLast_.store(elapsed,std::memory_order_relaxed);raise(injectNsMax_,elapsed);
        injectNsSum_.fetch_add(elapsed,std::memory_order_relaxed);injectNsFrames_.fetch_add(1,std::memory_order_relaxed);
        injectedLast_.store(injected,std::memory_order_relaxed);injectedSum_.fetch_add(injected,std::memory_order_relaxed);
        injectedFrames_.fetch_add(1,std::memory_order_relaxed);
        {auto old=injectedMax_.load(std::memory_order_relaxed);while(old<injected&&!injectedMax_.compare_exchange_weak(old,injected,std::memory_order_relaxed)){}}
        if(!injected)return no(NoneAccepted);
        store32(request+0x2c,injected);
        published_.store(request,std::memory_order_relaxed);
        accepted_.fetch_add(1,std::memory_order_relaxed);
        if(rooms)roomsInjectedFrames_.fetch_add(1,std::memory_order_relaxed);
        frameInjected_.store(injected,std::memory_order_relaxed);
        finish(request,1);
    }
    // Render thread, after the exact LEFT color stage (MAIN wait already joined, both LEFT
    // tapes of this frame completed). depthDraws/colorDraws: stereo::lastTapeDraws() of
    // the LEFT depth/colour tapes; depthObjects/colorObjects: DrawCommon objects of the
    // stock depth/colour stage (drawStageTimings, DETAIL profiler). UINT32_MAX = unavailable.
    void sample(std::uint32_t depthDraws=UINT32_MAX,std::uint32_t colorDraws=UINT32_MAX,
                std::uint32_t depthObjects=UINT32_MAX,std::uint32_t colorObjects=UINT32_MAX,std::uint32_t grassFields=UINT32_MAX)noexcept{
        if(!configured_.load(std::memory_order_acquire))return;
        const int incoming=errno;struct Restore{int value;~Restore(){errno=value;}} restore{incoming};
        const auto bodies=bodies_.load(std::memory_order_relaxed);
        const auto drawn=bodies-lastBodies_;lastBodies_=bodies;
        const auto seq=seq_.load(std::memory_order_acquire);
        if(seq==lastSeq_)return;
        const bool consecutive=seq==lastSeq_+1;lastSeq_=seq;
        lastFrameBodies_.store(consecutive?std::uint32_t(drawn):UINT32_MAX,std::memory_order_relaxed);
        const unsigned mode=frameMode_.load(std::memory_order_relaxed);
        const bool ab=mode==stereo::occluderTestMode,always=mode==stereo::occluderAlwaysMode;
        if(!ab&&!always)return;
        const unsigned state=state_.load(std::memory_order_relaxed);
        FrameSample s{};s.frame=frameIndex_.load(std::memory_order_relaxed);s.mode=mode;s.state=state;
        s.values[MInjected]=float(frameInjected_.load(std::memory_order_relaxed));s.valid[MInjected]=true;
        const auto request=request_.load(std::memory_order_relaxed);
        constexpr std::uintptr_t offsets[4]{0x38,0x3c,0x48,0x64};constexpr Metric metrics[4]{MStaticChild,MDynamic,MStaticNode,MTotal};
        bool countsOk=request!=0;
        for(unsigned i=0;i<4&&countsOk;++i){std::uint32_t v{};if(read(request+offsets[i],&v,4)){s.values[metrics[i]]=float(v);s.valid[metrics[i]]=true;}else countsOk=false;}
        if(!countsOk){sampleReadFailures_.fetch_add(1,std::memory_order_relaxed);for(auto m:metrics)s.valid[m]=false;}
        if(consecutive){s.values[MBodies]=float(drawn);s.valid[MBodies]=true;}
        if(depthDraws!=UINT32_MAX){s.values[MDepthDraws]=float(depthDraws);s.valid[MDepthDraws]=true;}
        if(colorDraws!=UINT32_MAX){s.values[MColorDraws]=float(colorDraws);s.valid[MColorDraws]=true;}
        if(depthObjects!=UINT32_MAX){s.values[MDepthObjects]=float(depthObjects);s.valid[MDepthObjects]=true;}
        if(colorObjects!=UINT32_MAX){s.values[MColorObjects]=float(colorObjects);s.valid[MColorObjects]=true;}
        if(grassFields!=UINT32_MAX){s.values[MGrassFields]=float(grassFields);s.valid[MGrassFields]=true;}
        for(unsigned i=0;i<3;++i){s.camera[i]=value(frameCamera_[i]);s.forward[i]=value(frameForward_[i]);}
        // Sample rings: on/off only in the A/B mode, always only in the always-on mode.
        if(countsOk&&((ab&&(state==1||state==2))||(always&&state==1))){
            auto& ring=always?always_:(state==1?on_:off_);
            const auto n=ring.count.load(std::memory_order_relaxed);auto& row=ring.values[n%ringFrames];
            for(unsigned i=0;i<4;++i)row[i].store(std::uint32_t(s.values[metrics[i]]),std::memory_order_relaxed);
            row[4].store(consecutive?std::uint32_t(drawn):UINT32_MAX,std::memory_order_relaxed);
            row[5].store(std::uint32_t(s.values[MInjected]),std::memory_order_relaxed);
            ring.count.store(n+1,std::memory_order_release);
        }
        std::lock_guard lock(measure_);
        recent_[recentCount_++%recentFrames]=s;
        if(!ab){closeWindow();return;}
        const auto index=s.frame/windowFrames;
        if(!currentOpen_||current_.index!=index){
            closeWindow();
            current_={};current_.index=index;current_.on=(index&1)==0;current_.firstCamera=s.camera;current_.firstForward=s.forward;currentOpen_=true;
        }
        auto& w=current_;++w.frames;if(state==1)++w.injectedFrames;else if(state==2)++w.eligibleFrames;
        for(unsigned m=0;m<MetricCount;++m)if(s.valid[m]){w.sum[m]+=s.values[m];++w.n[m];}
        w.camera=s.camera;w.forward=s.forward;
    }
    std::string report()const{
        auto num=[](std::string& o,float v){if(!std::isfinite(v)){o+="null";return;}char b[32];std::snprintf(b,sizeof b,"%.4f",double(v));o+=b;};
        auto u=[](std::uint64_t v){return std::to_string(v);};
        static constexpr const char* names[DeclineCount]{"roomsOutdoorPath","special","viewport","request","kind","array","countBusy","camera",
            "shaft","noCandidate","noneAccepted"};
        static constexpr const char* rejects[QuadRejectCount]{"range","planeDistance","aboveTop","outOfView","duplicate","planeMismatch","intersectFalse","selfTest"};
        const unsigned mode=stereo::testMode.load(std::memory_order_relaxed);
        std::string o="{\"configured\":";o+=configured_.load(std::memory_order_acquire)?"true":"false";
        o+=",\"active\":";o+=mode==stereo::occluderTestMode||mode==stereo::occluderAlwaysMode?"true":"false";
        o+=",\"alwaysOn\":";o+=mode==stereo::occluderAlwaysMode?"true":"false";
        const auto frames=modeFrames_.load(std::memory_order_relaxed);
        o+=",\"tableSize\":"+u(tableSize)+",\"wallCount\":"+u(occluder_table::wallCount)+",\"facadeBegin\":"+u(occluder_table::facadeBegin)+
            ",\"facadeCount\":"+u(occluder_table::facadeCount)+",\"table\":";o+=table_.load(std::memory_order_relaxed)==unsigned(Table::Walls)?"\"walls\"":"\"facades\"";
        o+=",\"tableFrames\":{\"walls\":"+u(tableFrames_[0].load(std::memory_order_relaxed))+",\"facades\":"+u(tableFrames_[1].load(std::memory_order_relaxed))+'}';
        o+=",\"rooms\":{\"seen\":"+u(roomsSeen_.load(std::memory_order_relaxed))+",\"injectedFrames\":"+u(roomsInjectedFrames_.load(std::memory_order_relaxed))+'}';
        o+=",\"kCap\":"+u(kInject)+",\"hardCap\":"+u(hardCap)+",\"builderBudget\":"+u(builderBudget);
        o+=",\"mainCalls\":"+u(calls_.load(std::memory_order_relaxed))+",\"modeFrames\":"+u(frames);
        o+=",\"injectWindow\":";o+=mode==stereo::occluderAlwaysMode||((frames/windowFrames)&1)==0?"true":"false";
        o+=",\"attempted\":"+u(attempted_.load(std::memory_order_relaxed))+",\"accepted\":"+u(accepted_.load(std::memory_order_relaxed))+
            ",\"offWindowEligible\":"+u(offWindow_.load(std::memory_order_relaxed))+",\"staleCleared\":"+u(staleCleared_.load(std::memory_order_relaxed))+
            ",\"sampleReadFailures\":"+u(sampleReadFailures_.load(std::memory_order_relaxed))+",\"builderCalls\":"+u(builderCalls_.load(std::memory_order_relaxed));
        const auto injFrames=injectedFrames_.load(std::memory_order_relaxed);
        o+=",\"injected\":{\"last\":"+u(injectedLast_.load(std::memory_order_relaxed))+",\"avg\":";
        if(injFrames)num(o,float(double(injectedSum_.load(std::memory_order_relaxed))/double(injFrames)));else o+="null";
        o+=",\"max\":"+u(injectedMax_.load(std::memory_order_relaxed))+",\"frames\":"+u(injFrames)+'}';
        const auto nsFrames=injectNsFrames_.load(std::memory_order_relaxed);
        o+=",\"injectNs\":{\"last\":"+u(injectNsLast_.load(std::memory_order_relaxed))+",\"max\":"+u(injectNsMax_.load(std::memory_order_relaxed))+",\"avg\":"+
            (nsFrames?u(injectNsSum_.load(std::memory_order_relaxed)/nsFrames):std::string("null"))+'}';
        o+=",\"declined\":{";
        for(unsigned i=0;i<DeclineCount;++i){if(i)o+=',';o+='"';o+=names[i];o+="\":"+u(declined_[i].load(std::memory_order_relaxed));}
        o+="},\"quadRejects\":{";
        for(unsigned i=0;i<QuadRejectCount;++i){if(i)o+=',';o+='"';o+=rejects[i];o+="\":"+u(quadRejects_[i].load(std::memory_order_relaxed));}
        o+="},\"intersect\":{\"true\":"+u(intersectResult_[0].load(std::memory_order_relaxed))+",\"false\":"+u(intersectResult_[1].load(std::memory_order_relaxed))+'}';
        o+=",\"selfTestResult\":{\"pass\":"+u(selfTest_[0].load(std::memory_order_relaxed))+",\"fail\":"+u(selfTest_[1].load(std::memory_order_relaxed))+'}';
        o+=",\"camera\":[";for(unsigned i=0;i<3;++i){if(i)o+=',';num(o,value(camera_[i]));}o+=']';
        const auto best=lastBest_.load(std::memory_order_relaxed);
        o+=",\"lastEligible\":"+u(lastEligible_.load(std::memory_order_relaxed))+",\"lastBest\":";o+=best<tableSize?u(best):std::string("null");
        o+=",\"bestSignedPlaneDistance\":";num(o,value(bestPlaneDistance_));o+=",\"bestCenterDistance\":";num(o,value(bestCenterDistance_));
        o+=",\"selfTest\":{\"behindMax\":";num(o,value(behindMax_));o+=",\"frontMax\":";num(o,value(frontMax_));o+='}';
        const bool captured=captured_.load(std::memory_order_acquire);
        o+=",\"capturedSelfTestPass\":";o+=captured?(capturedPass_.load(std::memory_order_relaxed)?"true":"false"):"null";
        o+=",\"capturedQuad\":";o+=captured?u(capturedQuad_.load(std::memory_order_relaxed)):std::string("null");
        o+=",\"polygonPlane\":[";if(captured)for(unsigned i=0;i<4;++i){if(i)o+=',';num(o,value(polygonPlane_[i]));}o+=']';
        o+=",\"slot0Planes\":[";if(captured)for(unsigned i=0;i<24;++i){if(i)o+=',';num(o,value(slotPlanes_[i]));}o+=']';
        // Per-quad counters (only quads that were ever eligible): index, centre, eligible frames, accepted frames.
        o+=",\"quads\":[";bool firstQuad=true;
        for(unsigned i=0;i<tableSize;++i){
            const auto e=quadEligible_[i].load(std::memory_order_relaxed),a=quadAccepted_[i].load(std::memory_order_relaxed);
            if(!e&&!a)continue;
            if(!firstQuad)o+=',';firstQuad=false;
            const auto& q=occluder_table::quads[i];
            o+="{\"i\":"+u(i)+",\"kind\":";o+=i>=occluder_table::facadeBegin?"\"facade\"":"\"wall\"";
            o+=",\"c\":[";num(o,q.center[0]);o+=',';num(o,q.center[1]);o+=',';num(o,q.center[2]);
            o+="],\"eligible\":"+u(e)+",\"accepted\":"+u(a)+'}';
        }
        o+=']';
        const auto last=lastFrameBodies_.load(std::memory_order_relaxed);
        o+=",\"lastFrameBodyDraws\":";o+=last==UINT32_MAX?std::string("null"):u(last);
        auto ring=[&](const char* name,const Ring& r){
            const auto total=r.count.load(std::memory_order_acquire);const auto n=total<ringFrames?total:ringFrames;
            double sum[6]{};std::uint64_t bodyFrames{};
            for(std::uint64_t i=0;i<n;++i){const auto& row=r.values[i];
                for(unsigned k=0;k<4;++k)sum[k]+=row[k].load(std::memory_order_relaxed);
                const auto b=row[4].load(std::memory_order_relaxed);if(b!=UINT32_MAX){sum[4]+=b;++bodyFrames;}
                sum[5]+=row[5].load(std::memory_order_relaxed);}
            o+=",\"";o+=name;o+="\":{\"samples\":"+u(total)+",\"window\":"+u(n);
            static constexpr const char* keys[6]{"staticChild","dynamic","staticNode","total","bodyDraws","injected"};
            for(unsigned k=0;k<6;++k){const auto d=k==4?bodyFrames:n;o+=",\"";o+=keys[k];o+="\":";
                if(!d)o+="null";else num(o,float(sum[k]/double(d)));}
            o+='}';
        };
        ring("on",on_);ring("off",off_);ring("always",always_);
        static constexpr const char* metricNames[MetricCount]{"injected","staticChild","dynamic","staticNode","total","bodyDraws","depthDraws","colorDraws","depthObjects","colorObjects",
            "grassFields"};
        auto vec=[&](const std::array<float,3>& v){o+='[';for(unsigned i=0;i<3;++i){if(i)o+=',';num(o,v[i]);}o+=']';};
        auto dist=[](const std::array<float,3>& a,const std::array<float,3>& b){float s=0;for(unsigned i=0;i<3;++i)s+=(a[i]-b[i])*(a[i]-b[i]);return std::sqrt(s);};
        auto angle=[](const std::array<float,3>& a,const std::array<float,3>& b){
            const float la=std::sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]),lb=std::sqrt(b[0]*b[0]+b[1]*b[1]+b[2]*b[2]);
            if(!(la>0)||!(lb>0))return 180.f;float c=(a[0]*b[0]+a[1]*b[1]+a[2]*b[2])/(la*lb);c=std::fmax(-1.f,std::fmin(1.f,c));
            return std::acos(c)*57.29578f;};
        std::array<Window,windowHistory> windows{};unsigned count=0;std::array<FrameSample,recentFrames> recent{};unsigned recentCount=0;
        {
            std::lock_guard lock(measure_);
            const auto first=windowCount_>windowHistory?windowCount_-windowHistory:0;
            for(auto i=first;i<windowCount_;++i)windows[count++]=windows_[i%windowHistory];
            const auto rfirst=recentCount_>recentFrames?recentCount_-recentFrames:0;
            for(auto i=rfirst;i<recentCount_;++i)recent[recentCount++]=recent_[i%recentFrames];
        }
        // A window is 'still' when the camera moved < 1 m and turned < 10 deg within it.
        auto still=[&](const Window& w){return dist(w.firstCamera,w.camera)<1.f&&angle(w.firstForward,w.forward)<10.f;};
        auto mean=[](const Window& w,unsigned m){return w.n[m]?float(w.sum[m]/double(w.n[m])):NAN;};
        o+=",\"windows\":[";
        for(unsigned i=0;i<count;++i){
            const auto& w=windows[i];if(i)o+=',';
            o+="{\"w\":"+u(w.index)+",\"on\":";o+=w.on?"true":"false";
            o+=",\"frames\":"+u(w.frames)+",\"injectedFrames\":"+u(w.injectedFrames)+",\"eligibleFrames\":"+u(w.eligibleFrames);
            for(unsigned m=0;m<MetricCount;++m){o+=",\"";o+=metricNames[m];o+="\":";num(o,mean(w,m));}
            o+=",\"camera\":";vec(w.camera);o+=",\"forward\":";vec(w.forward);
            o+=",\"travel\":";num(o,dist(w.firstCamera,w.camera));o+=",\"turnDeg\":";num(o,angle(w.firstForward,w.forward));
            o+=",\"still\":";o+=still(w)?"true":"false";o+='}';
        }
        o+=']';
        // Consecutive on/off windows taken at the same spot (both still, cameras < 1 m and
        // < 10 deg apart): mean of (on - off) per metric.
        std::array<double,MetricCount> delta{};std::array<unsigned,MetricCount> deltaN{};unsigned pairs=0;
        double grassOn=0,grassOff=0;unsigned grassN=0; // Paired means of the visible grass fields
        for(unsigned i=0;i+1<count;++i){
            const auto& a=windows[i];const auto& b=windows[i+1];
            if(b.index!=a.index+1||a.on==b.on||!still(a)||!still(b)||dist(a.camera,b.camera)>=1.f||angle(a.forward,b.forward)>=10.f)continue;
            const auto& on=a.on?a:b;const auto& off=a.on?b:a;++pairs;
            for(unsigned m=0;m<MetricCount;++m){const float x=mean(on,m),y=mean(off,m);if(std::isfinite(x)&&std::isfinite(y)){delta[m]+=x-y;++deltaN[m];}}
            {const float x=mean(on,MGrassFields),y=mean(off,MGrassFields);if(std::isfinite(x)&&std::isfinite(y)){grassOn+=x;grassOff+=y;++grassN;}}
        }
        o+=",\"paired\":{\"pairs\":"+u(pairs)+",\"onMinusOff\":{";
        for(unsigned m=0;m<MetricCount;++m){if(m)o+=',';o+='"';o+=metricNames[m];o+="\":";num(o,deltaN[m]?float(delta[m]/deltaN[m]):NAN);}
        o+="},\"grassFields\":{\"pairs\":"+u(grassN)+",\"on\":";num(o,grassN?float(grassOn/grassN):NAN);o+=",\"off\":";num(o,grassN?float(grassOff/grassN):NAN);
        o+="}}";
        o+=",\"recent\":[";
        for(unsigned i=0;i<recentCount;++i){
            const auto& s=recent[i];if(i)o+=',';
            o+="{\"frame\":"+u(s.frame)+",\"mode\":"+u(s.mode)+",\"state\":"+u(s.state);
            for(unsigned m=0;m<MetricCount;++m){o+=",\"";o+=metricNames[m];o+="\":";num(o,s.valid[m]?s.values[m]:NAN);}
            o+=",\"camera\":";vec(s.camera);o+=",\"forward\":";vec(s.forward);o+='}';
        }
        o+=']';
        o+='}';return o;
    }
};
} // namespace rdrvr::engine
