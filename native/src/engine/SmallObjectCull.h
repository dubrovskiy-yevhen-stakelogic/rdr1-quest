#pragma once
// Early small-object culling at the engine's per-object draw entry.
//
// Hook: renDrawBucketContainer::DrawCommon (librdr +2a28238, GOT +2e5f1a0; the mod's
// bodyDraw wrapper, pinned callers DrawOpaque +2a28f90 / DrawAlphaSorted +2a281e0 /
// Draw +2a296c8,+2a29768). One object per call; the right eye replays the LEFT tape, so a
// skipped object is skipped for both eyes.
//
// renDrawBucketInfo, filled by renDrawBucketInfo::Set (+2a2a510) from renDrawBucketParams:
//   +0x00 Matrix34 world  <- *params+0x10 (copied for EVERY object)
//   +0x40 Matrix34        <- *params+0x18
//   +0x80 drawable: params+0x58 rmcDrawable (kind 0; kind 4 when params+0x50==4),
//         params+0x60 -> kind 1, +0x68 -> kind 2, +0x70 -> kind 3 (+0xac bits 13..15)
//   +0x88 sagWorldNode* <- params+0x08, +0x90 grmMatrixSet* <- params+0x20,
//   +0x98 crCreature* <- params+0x78, +0xac bit 16 = (params+0x30 != 0).
// DrawCommon kind-0 path (+2a28be8..+2a28d4c): matrix set (+0x90) non-null ->
// grmMatrixSet::PrepareBuffer + drawable vtable+0x38 (rmcDrawable::DrawSkinned: per model
// grmModel::DrawSkinned only for truly skinned models, otherwise grmModel::Draw with the
// bone matrix - fragment/articulated props); null -> vtable+0x30 (Draw with info+0x00).
// rmcDrawable cull sphere: local centre +0x20/+0x24/+0x28, radius +0x90; the engine's own
// rmcDrawable::IsVisible (+1d9a678) transforms it with the ROOT matrix, as done here for
// every class.
// Classes (kind 0 only): Rigid (no matrix set), MatrixSet (matrix set, no creature),
// Creature (crCreature: PreDraw/DrawUpdate run in DrawCommon -> never skipped). Rigid and
// MatrixSet objects below the threshold are skipped.
// Gate: begin() is one relaxed load + branch unless a threshold is set (menu SMALL
// OBJECTS or debug.rdrvr.smallobjects=<px>) or the histogram is explicitly requested with
// debug.rdrvr.smallobjects.histogram=1 (bucket counts only, no clock calls) or =2 (counts
// plus the CPU bracket on every 4th object). The DETAIL profiler alone does not enable it.
// cpu02: the CPU bracket runs only with histogram=2 (max of the caller's request and the property); a threshold alone no longer times.
// Projected size = bounding-sphere DIAMETER in eye pixels = 2r/d * |P11| * 1760/2.
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace rdrvr::engine {
class SmallObjectCull {
public:
    enum StageId : unsigned {None=0,Depth=1,Color=2};
    enum Class : unsigned {Rigid=0,MatrixSet=1,Creature=2,ClassCount=3};
    static constexpr unsigned bucketCount=7;                 // <1,<2,<4,<8,<16,<32,>=32 px
    static constexpr std::array<float,6> bucketEdges{1,2,4,8,16,32};
    static constexpr unsigned ringFrames=32,timingStride=4;  // CPU bracket every 4th measured object
    static constexpr float eyeHeightPx=1760.f;
    enum Exclusion : unsigned {NotMain,Unpinned,Nested,NoCamera,Kind,NoDrawable,BadSphere,ExclusionCount};
    using Read=bool(*)(void*,std::uintptr_t,void*,std::size_t);
    struct Token {unsigned stage{},cls{},bucket{UINT32_MAX};bool entered{},skip{},timed{},counted{};std::uint64_t t0{};unsigned* nest{};}; // cpu02: nest = this thread's counter (end() without TLS)
    class Stage {
        unsigned previous_{};
    public:
        Stage(SmallObjectCull&,StageId stage,bool exact)noexcept:previous_(tls_.stage){tls_.stage=exact?unsigned(stage):0u;}
        ~Stage(){tls_.stage=previous_;}
        Stage(const Stage&)=delete;Stage& operator=(const Stage&)=delete;
    };
private:
    using Grid=std::array<std::array<std::array<std::uint32_t,bucketCount>,ClassCount>,2>;
    using AtomicGrid=std::array<std::array<std::array<std::atomic<std::uint32_t>,bucketCount>,ClassCount>,2>;
    using AtomicGrid64=std::array<std::array<std::array<std::atomic<std::uint64_t>,bucketCount>,ClassCount>,2>;
    struct Tls {unsigned stage,nest;};constinit inline static thread_local Tls tls_{}; // cpu02: one TLS lookup for stage and nest
    Read read_{};
    std::atomic<bool> active_{};       // threshold>0 || histogram requested; false => begin() is one branch
    std::atomic<bool> timing_{};       // cpu02: histogram==2 only: CPU bracket on every 4th measured object
    std::atomic<unsigned> histogram_{}; int histogramProperty_{0};
    // Published once per MAIN frame by frame() (visibility task).
    std::atomic<std::uint64_t> cameraSeq_{};
    std::array<std::atomic<std::uint32_t>,3> camera_{};std::atomic<std::uint32_t> focal_{};
    std::atomic<std::uintptr_t> container_{};std::atomic<unsigned> threshold_{},source_{}; // source 0 menu, 1 property
    std::atomic<std::uint64_t> frames_{},propertyReads_{};int property_{-1};
    // Render thread: current frame accumulators.
    std::uint64_t consumedSeq_{},frameSeq_{},objectCounter_{};bool frameSeqSet_{};float frameCamera_[3]{},frameFocal_{};
    struct Accum {
        Grid count{},skipped{},timed{};
        std::array<std::array<std::array<std::uint64_t,bucketCount>,ClassCount>,2> ns{};
        std::array<std::array<std::uint32_t,ExclusionCount>,2> excluded{};
    } frame_{};
    struct Row {
        AtomicGrid count{},skipped{},timed{};AtomicGrid64 ns{};
        std::array<std::array<std::atomic<std::uint32_t>,ExclusionCount>,2> excluded{};
    };
    std::array<Row,ringFrames> ring_{};std::atomic<std::uint64_t> ringCount_{};
    AtomicGrid64 lifeNs_{},lifeTimed_{};
    std::atomic<std::uint64_t> skippedTotal_{},midFrameCameraChanges_{},endFramesWithoutCamera_{};
    // hat01 (user: NPC hats and held newspapers vanished a few metres away): worn/held props are separate draws without a
    // creature pointer, so the size test dropped them (0.3 m hat < 16 px beyond ~10.7 m, 0.1 m prop beyond ~3.6 m). Small objects whose
    // sphere touches a creature sphere of the PREVIOUS frame (depth stage list; same decision in depth and colour) are never skipped.
    // debug.rdrvr.smallobjects.attach 0 = old behaviour (A/B). Render thread only (measure/endFrame), except attach_ (frame()).
    static constexpr unsigned maxCreatures=64;static constexpr float attachSlackM=0.6f; // ped motion per frame + hand reach
    struct Ball{float c[3]{},r{};};std::array<Ball,maxCreatures> creatureNext_{},creatureNow_{};unsigned creatureNextN_{},creatureNowN_{};
    std::atomic<bool> attach_{true};std::atomic<std::uint64_t> attachedKept_{};std::atomic<unsigned> creaturesLast_{}; // report copy
    bool nearCreature(const float* c,float r)const noexcept{
        for(unsigned i=0;i<creatureNowN_;++i){const auto& k=creatureNow_[i];const float d[3]{c[0]-k.c[0],c[1]-k.c[1],c[2]-k.c[2]},l=k.r+r+attachSlackM;
            if(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]<l*l)return true;}
        return false;}

    static std::uint32_t bits(float v)noexcept{std::uint32_t u;std::memcpy(&u,&v,4);return u;}
    static float value(std::uint32_t u)noexcept{float v;std::memcpy(&v,&u,4);return v;}
    static std::uint64_t now()noexcept{
        return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    bool read(std::uintptr_t a,void* out,std::size_t n)const noexcept{try{return read_&&a&&read_(nullptr,a,out,n);}catch(...){return false;}}
    static int readProperty()noexcept{
#ifdef __ANDROID__
        char v[PROP_VALUE_MAX]{};if(__system_property_get("debug.rdrvr.smallobjects",v)<=0)return -1;
        char* end{};const long n=std::strtol(v,&end,10);if(end==v||*end||n<0||n>1000)return -1;return int(n);
#else
        return -1;
#endif
    }
    static bool readAttachProperty()noexcept{ // hat01: unset/anything but "0" = keep props near creatures
#ifdef __ANDROID__
        char v[PROP_VALUE_MAX]{};return !(__system_property_get("debug.rdrvr.smallobjects.attach",v)>0&&v[0]=='0'&&!v[1]);
#else
        return true;
#endif
    }
    static int readHistogramProperty()noexcept{
#ifdef __ANDROID__
        char v[PROP_VALUE_MAX]{};if(__system_property_get("debug.rdrvr.smallobjects.histogram",v)<=0)return 0;
        return (v[0]=='1'||v[0]=='2')&&!v[1]?v[0]-'0':0;
#else
        return 0;
#endif
    }
public:
    void configure(Read read)noexcept{read_=read;}
    // The threshold frame() applied last (px, 0 = off) and whether debug.rdrvr.smallobjects set it (byState _so<px>[p]).
    unsigned thresholdPx()const noexcept{return threshold_.load(std::memory_order_relaxed);}
    bool thresholdFromProperty()const noexcept{return source_.load(std::memory_order_relaxed)!=0;}
    static float projectedPx(const float* centre,float radius,const float* camera,float focal)noexcept{
        const float d[3]{centre[0]-camera[0],centre[1]-camera[1],centre[2]-camera[2]};
        const float dist=std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
        if(!(dist>radius))return INFINITY;
        return 2.f*radius/dist*focal;
    }
    static unsigned bucketOf(float px)noexcept{
        for(unsigned b=0;b<bucketEdges.size();++b)if(px<bucketEdges[b])return b;
        return bucketCount-1;
    }
    // World-space sphere of a kind-0 renDrawBucketInfo (direct loads: DrawCommon reads the same fields).
    static bool sphere(const unsigned char* info,float* centre,float& radius)noexcept{
        const unsigned char* drawable;std::memcpy(&drawable,info+0x80,sizeof drawable);
        if(!drawable)return false;
        float m[16],c[3];std::memcpy(m,info,sizeof m);std::memcpy(c,drawable+0x20,sizeof c);std::memcpy(&radius,drawable+0x90,4);
        for(unsigned i=0;i<3;++i)centre[i]=c[0]*m[i]+c[1]*m[4+i]+c[2]*m[8+i]+m[12+i];
        return std::isfinite(centre[0])&&std::isfinite(centre[1])&&std::isfinite(centre[2])&&std::isfinite(radius)&&radius>0.f&&radius<1.0e5f;
    }
    // Visibility task, once per MAIN frame (audited union scope of the cached hook).
    // histogram: explicit request (0 off, 1 counts, 2 counts + timing); -1 = read the property.
    void frame(std::uintptr_t request,const void* viewport,unsigned menuPx,int histogram=-1)noexcept{
        const auto f=frames_.fetch_add(1,std::memory_order_relaxed);
        if((f&63)==0){property_=readProperty();histogramProperty_=readHistogramProperty();attach_.store(readAttachProperty(),std::memory_order_relaxed);propertyReads_.fetch_add(1,std::memory_order_relaxed);}
        const bool property=property_>=0;const unsigned threshold=property?unsigned(property_):menuPx;
        const unsigned wanted=unsigned(histogram>histogramProperty_?histogram:histogramProperty_); // cpu02: max(request, property), so the property can add timing
        threshold_.store(threshold,std::memory_order_relaxed);source_.store(property?1u:0u,std::memory_order_relaxed);
        histogram_.store(wanted,std::memory_order_relaxed);timing_.store(wanted>=2,std::memory_order_relaxed); // cpu02: no clock pair per 4th object for the menu threshold
        active_.store(threshold>0||wanted>0,std::memory_order_relaxed);
        if(!threshold&&!wanted)return; // nothing measured: no per-frame reads either
        float rows[16]{},p11{};std::uintptr_t container{};
        const auto v=reinterpret_cast<std::uintptr_t>(viewport);
        if(!viewport||!read(v+0x40,rows,sizeof rows)||!read(v+0x1c0+0x14,&p11,4)||!read(request+0x88,&container,sizeof container))return;
        const float focal=std::fabs(p11)*eyeHeightPx*0.5f;
        if(!std::isfinite(rows[12])||!std::isfinite(rows[13])||!std::isfinite(rows[14])||!(focal>0.f)||!std::isfinite(focal)||!container)return;
        for(unsigned i=0;i<3;++i)camera_[i].store(bits(rows[12+i]),std::memory_order_relaxed);
        focal_.store(bits(focal),std::memory_order_relaxed);container_.store(container,std::memory_order_relaxed);
        cameraSeq_.fetch_add(1,std::memory_order_release);
    }
    bool active()const noexcept{return active_.load(std::memory_order_relaxed);}
    // Render thread, DrawCommon entry (bodyDraw). skip => caller returns without drawing.
    // Inline gate (inactive: one relaxed load + branch at the call site, no call).
    Token begin(const void* container,const void* infoPointer,bool pinned)noexcept{
        if(!active_.load(std::memory_order_relaxed))return Token{};
        return measure(container,infoPointer,pinned);
    }
    Token measure(const void* container,const void* infoPointer,bool pinned)noexcept{
        Token t{};auto& tl=tls_;
        const unsigned stage=tl.stage;
        if(stage!=Depth&&stage!=Color)return t;
        const unsigned s=stage-1;t.stage=stage;t.entered=true;t.nest=&tl.nest;
        auto excl=[&](Exclusion e){++frame_.excluded[s][e];return t;};
        if(tl.nest++)return excl(Nested); // only the outermost DrawCommon is measured or skipped
        if(!pinned)return excl(Unpinned);
        if(reinterpret_cast<std::uintptr_t>(container)!=container_.load(std::memory_order_relaxed))return excl(NotMain);
        const auto seq=cameraSeq_.load(std::memory_order_acquire);
        if(seq==consumedSeq_)return excl(NoCamera);
        if(!frameSeqSet_){frameSeq_=seq;frameSeqSet_=true;
            for(unsigned i=0;i<3;++i)frameCamera_[i]=value(camera_[i].load(std::memory_order_relaxed));
            frameFocal_=value(focal_.load(std::memory_order_relaxed));}
        else if(seq!=frameSeq_)midFrameCameraChanges_.fetch_add(1,std::memory_order_relaxed); // keep this frame's snapshot
        const auto* info=static_cast<const unsigned char*>(infoPointer);
        std::uintptr_t creature,matrixSet;std::uint32_t flags;const unsigned char* drawable;
        std::memcpy(&creature,info+0x98,sizeof creature);std::memcpy(&matrixSet,info+0x90,sizeof matrixSet);
        std::memcpy(&flags,info+0xac,4);std::memcpy(&drawable,info+0x80,sizeof drawable);
        if((flags>>13)&7u)return excl(Kind);
        if(!drawable)return excl(NoDrawable);
        float centre[3],radius;
        if(!sphere(info,centre,radius))return excl(BadSphere);
        t.cls=creature?Creature:(matrixSet?MatrixSet:Rigid);
        const float px=projectedPx(centre,radius,frameCamera_,frameFocal_);
        t.bucket=bucketOf(px);t.counted=true;++frame_.count[s][t.cls][t.bucket];
        const unsigned threshold=threshold_.load(std::memory_order_relaxed);
        if(t.cls==Creature&&s==0&&creatureNextN_<maxCreatures)creatureNext_[creatureNextN_++]=Ball{{centre[0],centre[1],centre[2]},radius}; // hat01
        if(threshold&&t.cls!=Creature&&px<float(threshold)){
            if(attach_.load(std::memory_order_relaxed)&&nearCreature(centre,radius))attachedKept_.fetch_add(1,std::memory_order_relaxed); // hat01: worn/held props stay
            else{t.skip=true;++frame_.skipped[s][t.cls][t.bucket];skippedTotal_.fetch_add(1,std::memory_order_relaxed);return t;}}
        if(timing_.load(std::memory_order_relaxed)&&(objectCounter_++%timingStride)==0){t.timed=true;t.t0=now();}
        return t;
    }
    // Must be called for every begin() (also for skipped objects).
    void end(const Token& t)noexcept{
        if(!t.entered)return;
        if(t.nest&&*t.nest)--*t.nest; // cpu02: entered => nest points at this thread's counter (set in measure)
        if(!t.timed)return;
        const auto ns=now()-t.t0;const unsigned s=t.stage-1;
        frame_.ns[s][t.cls][t.bucket]+=ns;++frame_.timed[s][t.cls][t.bucket];
        lifeNs_[s][t.cls][t.bucket].fetch_add(ns,std::memory_order_relaxed);lifeTimed_[s][t.cls][t.bucket].fetch_add(1,std::memory_order_relaxed);
    }
    // Render thread, after the exact stock colour stage of the frame.
    void endFrame()noexcept{
        if(!active_.load(std::memory_order_relaxed)&&!frameSeqSet_)return; // gated off: nothing to publish
        if(!frameSeqSet_)endFramesWithoutCamera_.fetch_add(1,std::memory_order_relaxed);
        consumedSeq_=frameSeqSet_?frameSeq_:cameraSeq_.load(std::memory_order_acquire);frameSeqSet_=false;
        if(creatureNextN_){creatureNow_=creatureNext_;creatureNowN_=creatureNextN_;creaturesLast_.store(creatureNowN_,std::memory_order_relaxed);}creatureNextN_=0; // hat01: a frame without a depth stage keeps the last list
        const auto n=ringCount_.load(std::memory_order_relaxed);auto& row=ring_[n%ringFrames];
        for(unsigned s=0;s<2;++s){
            for(unsigned c=0;c<ClassCount;++c)for(unsigned b=0;b<bucketCount;++b){
                row.count[s][c][b].store(frame_.count[s][c][b],std::memory_order_relaxed);row.skipped[s][c][b].store(frame_.skipped[s][c][b],std::memory_order_relaxed);
                row.timed[s][c][b].store(frame_.timed[s][c][b],std::memory_order_relaxed);row.ns[s][c][b].store(frame_.ns[s][c][b],std::memory_order_relaxed);
            }
            for(unsigned e=0;e<ExclusionCount;++e)row.excluded[s][e].store(frame_.excluded[s][e],std::memory_order_relaxed);
        }
        ringCount_.store(n+1,std::memory_order_release);
        frame_={};
    }
    std::uint64_t skipped()const noexcept{return skippedTotal_.load(std::memory_order_relaxed);}
    std::string report()const{
        static constexpr const char* stageNames[2]{"stockDepth","stockColor"};
        static constexpr const char* classNames[ClassCount]{"rigid","matrixSet","creature"};
        static constexpr const char* bucketNames[bucketCount]{"lt1","lt2","lt4","lt8","lt16","lt32","ge32"};
        static constexpr const char* exclusionNames[ExclusionCount]{"notMain","unpinned","nested","noCamera","kind","noDrawable","badSphere"};
        auto num=[](std::string& o,double v){if(!std::isfinite(v)){o+="null";return;}char b[40];std::snprintf(b,sizeof b,"%.3f",v);o+=b;};
        auto u=[](std::uint64_t v){return std::to_string(v);};
        const auto total=ringCount_.load(std::memory_order_acquire);const auto n=total<ringFrames?total:ringFrames;
        std::string o="{\"active\":";o+=active_.load(std::memory_order_relaxed)?"true":"false";
        static constexpr const char* histogramNames[3]{"\"off\"","\"counts\"","\"timed\""};
        o+=",\"histogram\":";o+=histogramNames[histogram_.load(std::memory_order_relaxed)%3];
        o+=",\"timing\":";o+=timing_.load(std::memory_order_relaxed)?"true":"false";
        o+=",\"thresholdPx\":"+u(threshold_.load(std::memory_order_relaxed));
        o+=",\"thresholdSource\":";o+=source_.load(std::memory_order_relaxed)?"\"property\"":"\"menu\"";
        o+=",\"sizeMetric\":\"sphere diameter px = 2r/d*|P11|*1760/2 (root matrix x rmcDrawable cull sphere)\",\"focalPx\":";num(o,value(focal_.load(std::memory_order_relaxed)));
        o+=",\"mainFrames\":"+u(frames_.load(std::memory_order_relaxed))+",\"framesMeasured\":"+u(total)+",\"window\":"+u(n);
        o+=",\"skippedTotal\":"+u(skippedTotal_.load(std::memory_order_relaxed))+",\"midFrameCameraChanges\":"+u(midFrameCameraChanges_.load(std::memory_order_relaxed))+
            ",\"framesWithoutCamera\":"+u(endFramesWithoutCamera_.load(std::memory_order_relaxed))+",\"timingStride\":"+u(timingStride);
        o+=",\"attach\":";o+=attach_.load(std::memory_order_relaxed)?"true":"false";o+=",\"attachedKept\":"+u(attachedKept_.load(std::memory_order_relaxed))+",\"creaturesLast\":"+u(creaturesLast_.load(std::memory_order_relaxed)); // hat01
        double savedNsPerFrame=0;
        for(unsigned s=0;s<2;++s){
            o+=",\"";o+=stageNames[s];o+="\":{";
            double stageObjects=0,stageSkipped=0;
            for(unsigned c=0;c<ClassCount;++c){
                if(c)o+=',';o+='"';o+=classNames[c];o+="\":{\"buckets\":{";
                double objects=0,skippedSum=0,drawnNs=0;
                for(unsigned b=0;b<bucketCount;++b){
                    double cnt=0,sk=0,ns=0,timed=0;
                    for(std::uint64_t i=0;i<n;++i){const auto& r=ring_[i];
                        cnt+=r.count[s][c][b].load(std::memory_order_relaxed);sk+=r.skipped[s][c][b].load(std::memory_order_relaxed);
                        ns+=double(r.ns[s][c][b].load(std::memory_order_relaxed));timed+=r.timed[s][c][b].load(std::memory_order_relaxed);}
                    const double lifeT=double(lifeTimed_[s][c][b].load(std::memory_order_relaxed));
                    const double perObject=timed>0?ns/timed:(lifeT>0?double(lifeNs_[s][c][b].load(std::memory_order_relaxed))/lifeT:NAN);
                    if(b)o+=',';o+='"';o+=bucketNames[b];o+="\":{\"objectsPerFrame\":";num(o,n?cnt/double(n):NAN);
                    o+=",\"skippedPerFrame\":";num(o,n?sk/double(n):NAN);o+=",\"nsPerObject\":";num(o,perObject);o+='}';
                    if(n){objects+=cnt/double(n);skippedSum+=sk/double(n);
                        if(std::isfinite(perObject)){drawnNs+=(cnt-sk)/double(n)*perObject;savedNsPerFrame+=sk/double(n)*perObject;}}
                }
                o+="},\"objectsPerFrame\":";num(o,objects);o+=",\"skippedPerFrame\":";num(o,skippedSum);o+=",\"drawnNsPerFrame\":";num(o,drawnNs);o+='}';
                stageObjects+=objects;stageSkipped+=skippedSum;
            }
            o+=",\"objectsPerFrame\":";num(o,stageObjects);o+=",\"skippedPerFrame\":";num(o,stageSkipped);
            o+=",\"excludedPerFrame\":{";
            for(unsigned e=0;e<ExclusionCount;++e){double x=0;for(std::uint64_t i=0;i<n;++i)x+=ring_[i].excluded[s][e].load(std::memory_order_relaxed);
                if(e)o+=',';o+='"';o+=exclusionNames[e];o+="\":";num(o,n?x/double(n):NAN);}
            o+="}}";
        }
        o+=",\"savedNsPerFrameEstimate\":";num(o,savedNsPerFrame);
        o+='}';return o;
    }
};
} // namespace rdrvr::engine
