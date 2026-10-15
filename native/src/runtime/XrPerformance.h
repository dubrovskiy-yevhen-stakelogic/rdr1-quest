#pragma once
#include "PerformancePolicy.h"
#include "DisplayRefresh.h"
#include "AndroidThreadPolicy.h"
#include "ResolutionTelemetry.h"
#include "diagnostics/FrameTimings.h"
#include "diagnostics/ProcessLoad.h"
#include "KgslClock.h" // kgsl GPU clock reader + the measurement pack switch (measurementPack)
#include "HeatWarning.h" // heat01: runtime THERMAL notifications feed the OVERHEAT WARNING
#include <openxr/openxr.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#endif
namespace rdrvr::runtime {
struct PerformanceSupport {
    bool settings{},metrics{},refresh{},threads{};
    void enable(const std::vector<XrExtensionProperties>& available,std::vector<const char*>& names){
        for(const auto& p:available){
            if(!std::strcmp(p.extensionName,androidThreadExtension)){threads=true;names.push_back(androidThreadExtension);}
            if(!std::strcmp(p.extensionName,XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)){refresh=true;names.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);}
            if(!std::strcmp(p.extensionName,XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME)){settings=true;names.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);}
            if(!std::strcmp(p.extensionName,XR_META_PERFORMANCE_METRICS_EXTENSION_NAME)){metrics=true;names.push_back(XR_META_PERFORMANCE_METRICS_EXTENSION_NAME);}
        }
    }
};
// XR-thread-owned state; diagnostics readers receive a published JSON copy.
class PerformanceTelemetry {
    mutable std::mutex mutex_;std::string json_{"{\"state\":\"NOT_INITIALIZED\"}"};
public:
    void publish(std::string s){std::lock_guard lock(mutex_);json_=std::move(s);}
    std::string report()const{std::lock_guard lock(mutex_);return json_;}
};
inline PerformanceTelemetry performanceTelemetry;
// Measurement pack (debug.rdrvr.perfmetrics, default 1, re-read at most once per second; 0 = the
// DETAIL-only behaviour: outside DETAIL the block is then a stale initialize() snapshot, with
// gpuResult -7 and requestedGpuLevel 50 as member defaults). Pack on, in every profiler mode at
// 1 Hz (XR thread, pump() runs before xrWaitFrame):
//  - publish() of levels / results / thread hints / refresh / process (+ "levels", "measurementPack", "pollCost", "kgsl");
//  - the 7 coreMetrics below; the other META counters stay DETAIL-only;
//  - KgslClock::sample() half a second after each META poll (spreads the XR-thread cost; cost reported in pollCost);
//  - the first accepted CPU/GPU level result, the first failed one and thermal events are logged once (RdrVrXR).
// Metric windows are the last metricWindow polls (p50/p95/mean/last/max) + "sum" of every valid value since start, so a
// reader gets exact per-window means as delta(sum)/delta(total).
inline constexpr std::array<const char*,7> coreMetrics{"/perfmetrics_meta/compositor/gpu_frametime","/perfmetrics_meta/compositor/dropped_frame_count",
    "/perfmetrics_meta/compositor/spacewarp_mode","/perfmetrics_meta/app/gpu_frametime","/perfmetrics_meta/device/gpu_utilization",
    "/perfmetrics_meta/app/motion_to_photon_latency","/perfmetrics_meta/device/cpu_utilization_average"};
inline std::atomic<int> hostPerfMetricsProperty{1}; // Off Android: stands in for debug.rdrvr.perfmetrics
class XrPerformance {
    static constexpr unsigned metricWindow=32; // 32 s at 1 Hz (fits 24-60 s measurement windows)
    struct Metric {XrPath path{};std::string name;std::array<double,metricWindow> values{};unsigned size{},next{};std::uint64_t total{},invalid{};double sum{};int unit{};bool currentValid{},core{};XrResult result{XR_SUCCESS};};
    // XR-thread cost of one 1 Hz phase (META poll incl. process sample, kgsl sample, publish).
    struct Cost {std::uint64_t n{},ns{},lastNs{},maxNs{};void add(std::uint64_t d){++n;ns+=d;lastNs=d;maxNs=std::max(maxNs,d);}
        void json(std::ostringstream& o,const char* key)const{o<<'"'<<key<<"\":{\"n\":"<<n<<",\"lastUs\":"<<double(lastNs)/1e3<<",\"maxUs\":"<<double(maxNs)/1e3<<",\"meanUs\":"<<(n?double(ns)/1e3/double(n):0.)<<'}';}};
    // Per domain (0 CPU, 1 GPU) level request history for "levels" and the one-shot log lines.
    struct LevelLog {std::uint64_t requests{},failures{},firstAcceptedNs{};int firstAcceptedLevel{-1};XrResult firstAcceptedResult{XR_ERROR_FUNCTION_UNSUPPORTED},lastFailure{XR_SUCCESS};bool boostSeen{},failLogged{};};
    PerformanceSupport support_{};PerformancePolicy policy_;DisplayRefresh refresh_;AndroidThreadPolicy threads_;
    XrSession session_{};PFN_xrPerfSettingsSetPerformanceLevelEXT set_{};PFN_xrQueryPerformanceMetricsCounterMETA query_{};
    std::vector<Metric> metrics_;bool metricsEnabled_{};XrResult metricsResult_{XR_ERROR_FUNCTION_UNSUPPORTED};
    std::array<int,2> accepted_{-1,-1};std::array<XrResult,2> results_{XR_ERROR_FUNCTION_UNSUPPORTED,XR_ERROR_FUNCTION_UNSUPPORTED};
    std::uint64_t nextPoll_{},nextRetry_{},events_{};int desired_{50},desiredGpu_{50},thermalCpu_{},thermalGpu_{};
    diagnostics::ProcessLoad process_;
    bool pack_{true};std::uint64_t nextProperty_{},nextKgsl_{},firstPollNs_{},publishes_{},metaPolls_{};unsigned coreFound_{};
    std::array<LevelLog,2> levelLog_{};Cost metaCost_,kgslCost_,publishCost_;KgslClock kgsl_;
    static bool packProperty() noexcept {
#if defined(__ANDROID__)
        char v[PROP_VALUE_MAX]{};if(__system_property_get("debug.rdrvr.perfmetrics",v)<=0)return true;
        char* end{};const long x=std::strtol(v,&end,10);return !(end&&end!=v&&!*end&&x==0);
#else
        return hostPerfMetricsProperty.load(std::memory_order_relaxed)!=0;
#endif
    }
    // True when the switch changed (pump publishes once, so a pack-off report shows enabled 0 instead of a stale 1).
    bool pollProperty(std::uint64_t now) noexcept {
        if(now<nextProperty_)return false;nextProperty_=now+1'000'000'000ull;
        const bool was=pack_;pack_=packProperty();measurementPack.store(pack_?1:0,std::memory_order_relaxed);return was!=pack_;
    }
    static const char* levelName(int level) noexcept {switch(level){case 75:return "BOOST";case 50:return "HIGH";case 25:return "LOW";case 0:return "SAVE";default:return "N/A";}}
    void noteLevel(unsigned domain,int level,XrResult result,std::uint64_t now) noexcept {
        auto& l=levelLog_[domain];++l.requests;
        if(XR_SUCCEEDED(result)){
            if(level==75)l.boostSeen=true;
            if(!l.firstAcceptedNs){l.firstAcceptedNs=now;l.firstAcceptedLevel=level;l.firstAcceptedResult=result;
#if defined(__ANDROID__)
                if(pack_)__android_log_print(ANDROID_LOG_INFO,"RdrVrXR","perf level accepted domain=%s level=%d (%s) result=%d",domain?"GPU":"CPU",level,levelName(level),int(result));
#endif
            }
        }else{++l.failures;l.lastFailure=result;
#if defined(__ANDROID__)
            if(pack_&&!l.failLogged)__android_log_print(ANDROID_LOG_WARN,"RdrVrXR","perf level request failed domain=%s level=%d result=%d",domain?"GPU":"CPU",level,int(result));
#endif
            l.failLogged=true;}
    }
    template<class T>static T load(XrInstance i,const char* name){T p{};if(XR_FAILED(xrGetInstanceProcAddr(i,name,reinterpret_cast<PFN_xrVoidFunction*>(&p))))return nullptr;return p;}
    void publish(){
        std::ostringstream o;o<<"{\"settingsSupported\":"<<support_.settings<<",\"metricsSupported\":"<<support_.metrics
         <<",\"metricsEnabled\":"<<metricsEnabled_<<",\"metricsResult\":"<<metricsResult_
         <<",\"requestedLevel\":"<<desired_<<",\"requestedCpuLevel\":"<<desired_<<",\"requestedGpuLevel\":"<<desiredGpu_<<",\"cpuGpuTrade\":-1,\"levelsAreHintsNotClocks\":true"<<",\"acceptedCpuHint\":"<<accepted_[0]<<",\"acceptedGpuHint\":"<<accepted_[1]
         <<",\"cpuResult\":"<<results_[0]<<",\"gpuResult\":"<<results_[1]<<",\"boostUsed\":"<<policy_.used
         <<",\"thermalBlocked\":"<<policy_.thermal<<",\"thermalCpu\":"<<thermalCpu_<<",\"thermalGpu\":"<<thermalGpu_
         <<",\"performanceEvents\":"<<events_<<",\"process\":"<<process_.report()<<",\"resolution\":"<<resolutionTelemetry.report()<<",\"androidThreads\":"<<threads_.report()<<",\"displayRefresh\":"<<refresh_.report()<<",\"pollIntervalNs\":1000000000";
        // Proof fields (pack state, level acceptance, XR-thread cost, kgsl clock).
        const auto now=diagnostics::monotonicNs();
        o<<",\"measurementPack\":{\"enabled\":"<<(pack_?1:0)<<",\"property\":\"debug.rdrvr.perfmetrics\",\"profilerDetail\":"<<(diagnostics::profilingEnabled()?1:0)
         <<",\"publishes\":"<<++publishes_<<",\"metaPolls\":"<<metaPolls_<<",\"coreListed\":"<<coreMetrics.size()<<",\"coreFound\":"<<coreFound_<<",\"metricWindow\":"<<metricWindow<<'}';
        o<<",\"levels\":{";
        for(unsigned i=0;i<2;++i){const auto& l=levelLog_[i];const int requested=i?desiredGpu_:desired_;
            o<<(i?",":"")<<'"'<<(i?"gpu":"cpu")<<"\":{\"requested\":"<<requested<<",\"accepted\":"<<accepted_[i]<<",\"acceptedName\":\""<<levelName(accepted_[i])<<"\",\"result\":"<<results_[i]
             <<",\"inEffect\":"<<(accepted_[i]==requested?1:0)<<",\"requests\":"<<l.requests<<",\"failures\":"<<l.failures<<",\"lastFailure\":"<<l.lastFailure<<",\"boostSeen\":"<<(l.boostSeen?1:0)
             <<",\"firstAcceptedLevel\":"<<l.firstAcceptedLevel<<",\"firstAcceptedResult\":"<<l.firstAcceptedResult<<",\"firstAcceptedAgoS\":"<<(l.firstAcceptedNs&&now>=l.firstAcceptedNs?double(now-l.firstAcceptedNs)/1e9:-1.)<<'}';}
        o<<",\"gpuBoostInEffect\":"<<(desiredGpu_==75&&accepted_[1]==75?1:0)<<",\"cpuBoostInEffect\":"<<(desired_==75&&accepted_[0]==75?1:0)<<'}';
        o<<",\"pollCost\":{";metaCost_.json(o,"meta");o<<',';kgslCost_.json(o,"kgsl");o<<',';publishCost_.json(o,"publish");
        const double span=firstPollNs_&&now>firstPollNs_?double(now-firstPollNs_)/1e9:0.;
        o<<",\"totalUsPerSecond\":"<<(span>=1.?double(metaCost_.ns+kgslCost_.ns+publishCost_.ns)/1e3/span:-1.)<<'}';
        o<<",\"kgsl\":"<<(kgsl_.samples()||pack_?kgsl_.report(now):std::string("{\"status\":\"OFF\"}"));
        o<<",\"metrics\":[";
        for(unsigned i=0;i<metrics_.size();++i){const auto& m=metrics_[i];if(i)o<<',';
            o<<"{\"path\":\""<<m.name<<"\",\"core\":"<<(m.core?1:0)<<",\"unit\":"<<m.unit<<",\"samples\":"<<m.size<<",\"total\":"<<m.total<<",\"invalid\":"<<m.invalid<<",\"result\":"<<m.result<<",\"currentValid\":"<<m.currentValid;
            if(m.size){auto sorted=m.values;std::sort(sorted.begin(),sorted.begin()+m.size);double windowSum=0;for(unsigned k=0;k<m.size;++k)windowSum+=m.values[k];
                o<<",\"last\":"<<m.values[(m.next+metricWindow-1)%metricWindow]<<",\"p50\":"<<sorted[(m.size*50+99)/100-1]<<",\"p95\":"<<sorted[(m.size*95+99)/100-1]<<",\"max\":"<<sorted[m.size-1]
                 <<",\"mean\":"<<windowSum/double(m.size)<<",\"sum\":"<<std::to_string(m.sum);} // fixed notation: exact delta(sum)/delta(total) after hours
            o<<'}';}performanceTelemetry.publish(o.str()+"]}");
    }
public:
    AndroidThreadPolicy& threads(){return threads_;}
    const diagnostics::ProcessLoad& process()const{return process_;}
    const char* acceptedMode(unsigned domain)const{
        if(domain>=2)return "N/A";
        switch(accepted_[domain]){case 75:return "BOOST";case 50:return "HIGH";case 25:return "LOW";case 0:return "SAVE";default:return "N/A";}
    }
    float displayHz()const{return refresh_.actual();}
    unsigned refreshAttempts()const{return refresh_.attempts();}
    void refreshChanged(const XrEventDataDisplayRefreshRateChangedFB& event){refresh_.changed(event);}
    double latest(const char* name,int unit=-1)const{
        for(const auto& m:metrics_)if(m.name==name&&m.size&&m.currentValid&&XR_SUCCEEDED(m.result)&&(unit<0||m.unit==unit))return m.values[(m.next+metricWindow-1)%metricWindow];
        return -1;
    }
    double latestPercent(const char* name)const{
        const auto v=latest(name,XR_PERFORMANCE_METRICS_COUNTER_UNIT_PERCENTAGE_META);
        return v>=0&&v<=100?v:-1;
    }
    bool boostAccepted()const{return desired_==75&&accepted_[0]==75;}
    void initialize(XrInstance instance,XrSession session,PerformanceSupport support){
        support_=support;session_=session;
        refresh_.initialize(instance,session,support.refresh);
        if(support.settings)set_=load<PFN_xrPerfSettingsSetPerformanceLevelEXT>(instance,"xrPerfSettingsSetPerformanceLevelEXT");
        if(support.metrics){
            auto enumerate=load<PFN_xrEnumeratePerformanceMetricsCounterPathsMETA>(instance,"xrEnumeratePerformanceMetricsCounterPathsMETA");
            auto enable=load<PFN_xrSetPerformanceMetricsStateMETA>(instance,"xrSetPerformanceMetricsStateMETA");
            query_=load<PFN_xrQueryPerformanceMetricsCounterMETA>(instance,"xrQueryPerformanceMetricsCounterMETA");
            std::uint32_t count{};
            if(enumerate&&enable&&query_&&XR_SUCCEEDED(enumerate(instance,0,&count,nullptr))&&count&&count<=256){
                std::vector<XrPath> paths(count);
                if(XR_SUCCEEDED(enumerate(instance,count,&count,paths.data()))){
                    for(auto path:paths){char text[XR_MAX_PATH_LENGTH]{};std::uint32_t length{};
                        if(XR_SUCCEEDED(xrPathToString(instance,path,sizeof(text),&length,text))){Metric m;m.path=path;m.name=text;
                            for(const char* c:coreMetrics)if(m.name==c){m.core=true;++coreFound_;}
                            metrics_.push_back(std::move(m));}}
                    XrPerformanceMetricsStateMETA state{XR_TYPE_PERFORMANCE_METRICS_STATE_META};state.enabled=XR_TRUE;
                    metricsResult_=enable(session,&state);metricsEnabled_=XR_SUCCEEDED(metricsResult_);
                }
            }
        }
        pollProperty(diagnostics::monotonicNs()); // The first publish already shows the pack state
        publish();
    }
    void begin(std::uint64_t now=diagnostics::monotonicNs()){heatWarning.resetXr(); accepted_={-1,-1};nextRetry_=0;refresh_.begin();threads_.begin();pump(false,now);}
    void event(const XrEventDataPerfSettingsEXT& event){
        ++events_;
#if defined(__ANDROID__)
        if(pack_)__android_log_print(ANDROID_LOG_INFO,"RdrVrXR","perf settings event domain=%s subDomain=%d from=%d to=%d",
            event.domain==XR_PERF_SETTINGS_DOMAIN_CPU_EXT?"CPU":"GPU",int(event.subDomain),int(event.fromLevel),int(event.toLevel));
#endif
        if(event.subDomain==XR_PERF_SETTINGS_SUB_DOMAIN_THERMAL_EXT){
            (event.domain==XR_PERF_SETTINGS_DOMAIN_CPU_EXT?thermalCpu_:thermalGpu_)=event.toLevel;
            heatWarning.noteXrThermal(event.domain==XR_PERF_SETTINGS_DOMAIN_CPU_EXT?0u:1u,int(event.toLevel)); // heat01
            if(event.toLevel>=XR_PERF_SETTINGS_NOTIF_LEVEL_WARNING_EXT){policy_.warning();nextRetry_=0;}
        }
    }
    void pump(bool world,std::uint64_t now=diagnostics::monotonicNs()){
        if(pollProperty(now))publish();
        threads_.pump(now);
        refresh_.pump(now);
        desired_=policy_.level(now,world);desiredGpu_=policy_.gpuLevel();
        if(set_&&now>=nextRetry_){
            const std::array<XrPerfSettingsDomainEXT,2> domains{XR_PERF_SETTINGS_DOMAIN_CPU_EXT,XR_PERF_SETTINGS_DOMAIN_GPU_EXT};
            const std::array<int,2> desired{desired_,desiredGpu_};
            for(unsigned i=0;i<2;++i)if(accepted_[i]!=desired[i]){results_[i]=set_(session_,domains[i],static_cast<XrPerfSettingsLevelEXT>(desired[i]));if(XR_SUCCEEDED(results_[i]))accepted_[i]=desired[i];
                noteLevel(i,desired[i],results_[i],now);}
            nextRetry_=now+1'000'000'000ull;
        }
        // kgsl half a second after each META poll (pack on only).
        if(pack_&&nextKgsl_&&now>=nextKgsl_){nextKgsl_=0;const auto t0=diagnostics::monotonicNs();kgsl_.sample(now);kgslCost_.add(diagnostics::monotonicNs()-t0);}
        const bool detail=diagnostics::profilingEnabled();
        if((!detail&&!pack_)||now<nextPoll_)return;nextPoll_=now+1'000'000'000ull; // Pack off = DETAIL-only gate
        if(pack_)nextKgsl_=now+500'000'000ull;if(!firstPollNs_)firstPollNs_=now;++metaPolls_;
        diagnostics::WallScope timing(diagnostics::frameTimings.runtimeMetricsPoll);
        const auto t0=diagnostics::monotonicNs();
        process_.sample();
        if(metricsEnabled_)for(auto& m:metrics_){
            m.currentValid=false;
            if(!detail&&!m.core)continue; // Outside DETAIL only the core counters
            XrPerformanceMetricsCounterMETA value{XR_TYPE_PERFORMANCE_METRICS_COUNTER_META};m.result=query_(session_,m.path,&value);
            const auto flags=value.counterFlags;
            if(XR_FAILED(m.result)||!(flags&(XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META|XR_PERFORMANCE_METRICS_COUNTER_UINT_VALUE_VALID_BIT_META))){++m.invalid;continue;}
            const double v=flags&XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META?static_cast<double>(value.floatValue):static_cast<double>(value.uintValue);
            if(!std::isfinite(v)){++m.invalid;continue;}m.currentValid=true;m.unit=value.counterUnit;m.values[m.next]=v;m.next=(m.next+1)%metricWindow;if(m.size<metricWindow)++m.size;++m.total;m.sum+=v;
        }
        const auto t1=diagnostics::monotonicNs();metaCost_.add(t1-t0);
        publish();
        publishCost_.add(diagnostics::monotonicNs()-t1);
    }
};
}
