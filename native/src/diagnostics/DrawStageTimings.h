#pragma once
#include "ThreadCpuScope.h"
#include "DrawThreadContext.h"
#include <atomic>

namespace rdrvr::diagnostics {
// Sum outer native bucket calls within one stock/eye stage. No engine reads,
// allocation or locking at the per-object draw boundary; clocks sample 1/16 calls.
class DrawStageTimings {
    struct PartCount {std::uint64_t wall{},cpu{},calls{};bool cpuValid{true};unsigned depth{};};
    static constexpr unsigned partCount=11;
    inline static constexpr const char* partNames[]{"alpha","instances","callbacks","commonSample","creaturePre","creatureUpdate","shaderGlobals","objectAO","flushState","pipelineState","indexedGeometry"};
    struct Counters {std::uint64_t wall{},cpu{},buckets{},objects{},hidden{},ordinal{};bool cpuValid{true},sampled{};unsigned depth{},phase{},samplePhase{};std::array<PartCount,partCount> parts{};};
    struct PartRow {DurationWindow wall,cpu,calls;};
    struct Row {DurationWindow bucketWall,bucketCpu,bucketCount,objectCount,hiddenCount;std::array<PartRow,partCount> parts;std::atomic<unsigned> phase{};};
    struct State {DrawStageTimings* owner{};unsigned index{};Counters counts{};};
    // The active stage lives in drawThreadContext (shared per-thread DrawCommon struct).
    static State* active()noexcept{return static_cast<State*>(drawThreadContext.stageTimings);}
    static void activate(State* s)noexcept{drawThreadContext.stageTimings=s;}
    std::array<Row,9> rows_;
    std::array<std::atomic<std::uint32_t>,9> lastObjects_{}; // Objects of the last measured stage (UINT32_MAX: profiler off)
    std::array<std::atomic<std::uint64_t>,9> lastCpu_{},finished_{}; // Bucket CPU of the last measured stage, measured stages
public:
    std::uint32_t lastObjects(unsigned index)const noexcept{return index<9?lastObjects_[index].load(std::memory_order_relaxed):UINT32_MAX;}
    std::uint64_t lastCpu(unsigned index)const noexcept{return index<9?lastCpu_[index].load(std::memory_order_relaxed):UINT64_MAX;} // UINT64_MAX: invalid
    std::uint64_t finishedStages(unsigned index)const noexcept{return index<9?finished_[index].load(std::memory_order_relaxed):0;}
    std::atomic<unsigned> installedParts{};
    class Stage {
        State state_;State* previous_{};bool enabled_{};std::uint64_t ticket_{profilerTicket()};
    public:
        Stage(DrawStageTimings& owner,unsigned index,bool enabled=true):state_{&owner,index},previous_(active()),enabled_(enabled&&index<9&&profilingEnabled()){if(enabled_){state_.counts.phase=owner.rows_[index].phase.fetch_add(1,std::memory_order_relaxed)&15;activate(&state_);}
            else if(enabled&&index<9)owner.lastObjects_[index].store(UINT32_MAX,std::memory_order_relaxed);}
        ~Stage(){finish();}
        Stage(const Stage&)=delete;Stage& operator=(const Stage&)=delete;
        void finish()noexcept{
            if(!enabled_)return;const int saved=errno;activate(previous_);enabled_=false;
            if(!profilerTicketValid(ticket_)){errno=saved;return;}
            auto& r=state_.owner->rows_[state_.index];const auto& c=state_.counts;
            state_.owner->lastObjects_[state_.index].store(std::uint32_t(c.objects),std::memory_order_relaxed);
            state_.owner->lastCpu_[state_.index].store(c.cpuValid?c.cpu:UINT64_MAX,std::memory_order_relaxed);
            state_.owner->finished_[state_.index].fetch_add(1,std::memory_order_release);
            // Windows with zero buckets still contribute a real zero sum.
            r.bucketWall.elapsed(1,c.wall+1);if(c.cpuValid)r.bucketCpu.elapsed(1,c.cpu+1);
            r.bucketCount.elapsed(1,c.buckets+1);r.objectCount.elapsed(1,c.objects+1);r.hiddenCount.elapsed(1,c.hidden+1);
            for(unsigned i=0;i<partCount;++i){auto& row=r.parts[i];const auto& p=c.parts[i];
                row.wall.elapsed(1,p.wall+1);if(p.cpuValid)row.cpu.elapsed(1,p.cpu+1);row.calls.elapsed(1,p.calls+1);}
            errno=saved;
        }
    };
    class Bucket {
        State* state_{};std::uint64_t wall_{},cpu_{};bool outer_{};
    public:
        Bucket(){
            state_=profilingEnabled()?active():nullptr;if(!state_)return;
            outer_=state_->counts.depth++==0;if(!outer_)return;
            ++state_->counts.buckets;wall_=monotonicNs();cpu_=threadCpuNs();
        }
        ~Bucket(){
            if(!state_)return;const int saved=errno;
            if(outer_){const auto cpu=threadCpuNs(),wall=monotonicNs();
                if(wall_&&wall>=wall_)state_->counts.wall+=wall-wall_;
                if(cpu_&&cpu>=cpu_)state_->counts.cpu+=cpu-cpu_;else state_->counts.cpuValid=false;
            }
            --state_->counts.depth;errno=saved;
        }
        Bucket(const Bucket&)=delete;Bucket& operator=(const Bucket&)=delete;
    };
    // Profiler off => no TLS access (a stage is only activated with the profiler on, and a
    // stage whose profiler ticket changed is discarded in finish()).
    static void object(bool hidden)noexcept{if(!profilingEnabled())return;if(auto* s=active()){++s->counts.objects;s->counts.hidden+=hidden;}}
    // Parts 0..2 are complete native batch calls. Parts 3..10 observe one in
    // sixteen DrawCommon calls, rotating the ordinal every stage/frame. Measure
    // parents and children on different calls so child clock overhead is not
    // amplified inside the parent estimate. These are unpaired estimates.
    class CommonSample {
        State* state_{};bool previous_{};unsigned previousPhase_{};
    public:
        CommonSample(bool eligible=true):state_(active()){if(state_){auto& c=state_->counts;previous_=c.sampled;previousPhase_=c.samplePhase;c.sampled=eligible;c.samplePhase=(c.ordinal++-c.phase)&15;}}
        ~CommonSample(){if(state_){state_->counts.sampled=previous_;state_->counts.samplePhase=previousPhase_;}}
        CommonSample(const CommonSample&)=delete;
    };
    static constexpr unsigned samplePhase(unsigned part){return part==3?0:part==10?8:part==8?10:part==9?12:4;}
    class Part {
        PartCount* counts_{};std::uint64_t wall_{},cpu_{};bool outer_{};
    public:
        explicit Part(unsigned index){auto* s=profilingEnabled()?active():nullptr;if(!s||index>=partCount||(index>=3&&(!s->counts.sampled||s->counts.samplePhase!=samplePhase(index))))return;
            counts_=&s->counts.parts[index];outer_=counts_->depth++==0;if(!outer_)return;
            ++counts_->calls;wall_=monotonicNs();cpu_=threadCpuNs();}
        ~Part(){if(!counts_)return;const int saved=errno;if(outer_){const auto cpu=threadCpuNs(),wall=monotonicNs();
                if(wall_&&wall>=wall_)counts_->wall+=wall-wall_;
                if(cpu_&&cpu>=cpu_)counts_->cpu+=cpu-cpu_;else counts_->cpuValid=false;}
            --counts_->depth;errno=saved;}
        Part(const Part&)=delete;
    };
    double partCpuMs(unsigned stage,unsigned part,bool estimate=false)const{
        if(stage>=9||part>=partCount||!(installedParts.load(std::memory_order_relaxed)&(1u<<part)))return -1;const auto s=rows_[stage].parts[part].cpu.recentSnapshot();
        return s.samples?s.recentMean/1e6*(estimate&&part>=3?16:1):-1;
    }
    std::string report()const{
        constexpr const char* names[]{"stockDepth","stockLight","stockColor","leftDepth","leftLight","leftColor","rightDepth","rightLight","rightColor"};
        const auto counts=[](const DurationWindow& window){const auto s=window.snapshot();std::ostringstream o;
            o<<"{\"samples\":"<<s.samples<<",\"totalSamples\":"<<s.total<<",\"recentMean32\":"<<s.recentMean
             <<",\"last\":"<<s.last<<",\"p50\":"<<s.p50<<",\"p95\":"<<s.p95<<",\"max\":"<<s.maximum<<'}';return o.str();};
        std::ostringstream o;o<<"{\"installedParts\":"<<installedParts.load()<<",\"sampling\":\"disjoint-parent-child-phases\",\"scope\":\"outer-native-buckets-per-stage\",\"objectCounter\":\"DrawCommon-including-hidden-local-body\",\"stages\":{";
        for(unsigned i=0;i<rows_.size();++i){if(i)o<<',';const auto& r=rows_[i];
            o<<'"'<<names[i]<<"\":{\"bucketWall\":"<<r.bucketWall.snapshot().json()<<",\"bucketCpu\":"<<r.bucketCpu.snapshot().json()
             <<",\"bucketCount\":"<<counts(r.bucketCount)<<",\"objectCount\":"<<counts(r.objectCount)<<",\"hiddenCount\":"<<counts(r.hiddenCount)<<",\"parts\":{";
            for(unsigned p=0;p<partCount;++p){if(p)o<<',';const auto& v=r.parts[p];
                o<<'"'<<partNames[p]<<"\":{\"sampleStride\":"<<(p>=3?16:1)<<",\"samplePhase\":"<<(p>=3?samplePhase(p):0)<<",\"wall\":"<<v.wall.snapshot().json()
                 <<",\"cpu\":"<<v.cpu.snapshot().json()<<",\"calls\":"<<counts(v.calls)<<'}';}
            o<<"}}";
        }
        return o.str()+"}}";
    }
};
inline DrawStageTimings drawStageTimings;
}
