#pragma once
#include "ProfilerControl.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace rdrvr::diagnostics {
inline std::uint64_t monotonicNs() noexcept {
    const int saved=errno;
    const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    errno=saved;return ns>0?static_cast<std::uint64_t>(ns):0;
}

// Bounded rolling wall durations. No allocation/sort on the render path.
// Percentiles are exact nearest-rank values of the last 512 observations,
// including waits inside the measured call; these are NOT GPU timestamps.
class DurationWindow {
public:
    static constexpr std::size_t capacity=512;
    struct Snapshot {
        std::uint64_t total{},minimum{},p50{},p95{},p99{},maximum{},recentMean{},last{};
        std::size_t samples{};
        std::string json() const {
            std::ostringstream out;
            out<<"{\"samples\":"<<samples<<",\"totalSamples\":"<<total
               <<",\"minNs\":"<<minimum<<",\"p50Ns\":"<<p50<<",\"p95Ns\":"<<p95
               <<",\"p99Ns\":"<<p99<<",\"maxNs\":"<<maximum<<",\"recentMean32Ns\":"<<recentMean<<",\"lastNs\":"<<last<<'}';return out.str();
        }
    };
private:
    mutable std::mutex mutex_;
    std::array<std::uint64_t,capacity> values_{};
    std::size_t next_{},size_{};std::uint64_t total_{};
public:
    bool elapsedWall(std::uint64_t begin,std::uint64_t end) noexcept {
        return !captureDurationExcluded(begin,end)&&elapsed(begin,end);
    }
    bool elapsed(std::uint64_t begin,std::uint64_t end) noexcept {
        if(!begin||end<begin||captureMeasurementExclusion.load(std::memory_order_acquire))return false;
        std::lock_guard lock(mutex_);values_[next_]=end-begin;next_=(next_+1)%capacity;
        if(size_<capacity)++size_;if(total_!=UINT64_MAX)++total_;return true;
    }
    struct RecentSnapshot {std::uint64_t total{},recentMean{},last{};std::size_t samples{};};
    // HUD needs only the last 32 values, not sorted 512-sample percentiles.
    // Copy a coherent small window, then do arithmetic outside the writer lock.
    RecentSnapshot recentSnapshot() const {
        RecentSnapshot out{};std::array<std::uint64_t,32> values{};std::size_t count{};
        {std::lock_guard lock(mutex_);out.samples=size_;out.total=total_;
            count=std::min<std::size_t>(size_,values.size());
            for(std::size_t i=0;i<count;++i)values[i]=values_[(next_+capacity-1-i)%capacity];}
        if(!count)return out;
        long double sum{};for(std::size_t i=0;i<count;++i)sum+=values[i];
        out.last=values[0];out.recentMean=static_cast<std::uint64_t>(sum/count);return out;
    }
    Snapshot snapshot() const {
        Snapshot out{};std::array<std::uint64_t,capacity> values{};
        std::size_t next{};
        {std::lock_guard lock(mutex_);values=values_;out.samples=size_;out.total=total_;next=next_;}
        if(!out.samples)return out;
        const auto recentCount=std::min<std::size_t>(out.samples,32);long double sum{};
        for(std::size_t i=0;i<recentCount;++i)sum+=values[(next+capacity-1-i)%capacity];
        out.last=values[(next+capacity-1)%capacity];out.recentMean=static_cast<std::uint64_t>(sum/recentCount);
        std::sort(values.begin(),values.begin()+out.samples);
        const auto percentile=[&](std::size_t percent){return values[(out.samples*percent+99)/100-1];};
        out.minimum=values[0];out.maximum=values[out.samples-1];
        out.p50=percentile(50);out.p95=percentile(95);out.p99=percentile(99);return out;
    }
};
class WallScope {
    DurationWindow* window_;std::uint64_t ticket_{profilerTicket()},start_{};
public:
    explicit WallScope(DurationWindow& window):WallScope(&window){}
    explicit WallScope(DurationWindow* window):window_(window){if(ticket_&1)start_=monotonicNs();else window_=nullptr;}
    WallScope(const WallScope&)=delete;
    ~WallScope(){finish();}
    std::uint64_t start() const{return start_;}
    // History remains active with profiling disabled; never interpret a zero
    // diagnostic start as a real timestamp or mix a scope across toggle epochs.
    std::uint64_t elapsedNs() const noexcept {
        if(!start_||!profilerTicketValid(ticket_))return 0;
        const auto now=monotonicNs();return now>=start_?now-start_:0;
    }
    void finish() noexcept {
        if(!window_)return;const int saved=errno;
        if(profilerTicketValid(ticket_))window_->elapsedWall(start_,monotonicNs());window_=nullptr;errno=saved;
    }
};

struct FrameTimings {
    DurationWindow worldAttempt,worldRecorded,hudPrepare,hudInterval,hudFinish,presentTransfers,xrWait,xrWork,xrEnd,headSample,controllerTarget,controllerAttachment,globalCapture,globalRestore,resourceInspect,targetPrepare,nativePreflight,stereoDraw,leftDepth,leftLight,leftColor,rightDepth,rightLight,rightColor,eyeCopy,depthResolve,passRestore,theaterPresent,theaterFenceWait,theaterQueueSubmit,stereoCapture,stereoRender,stereoFenceWait,stereoQueueSubmit,stereoRebuild,stereoScreens,stereoLockWait,theaterLockWait,xrImageWait,runtimeMetricsPoll;
    DurationWindow stockDepth,stockLight,stockColor,nativeSceneInclusive,swappyPresent;
    std::string report() const {
        std::ostringstream out;
        out<<"{\"units\":\"nanoseconds\",\"clock\":\"steady-wall\",\"includesDriverWaits\":true"
           <<",\"gpuTimestamps\":false,\"windowCapacity\":"<<DurationWindow::capacity;
        for(const auto& [name,window]:std::array<std::pair<const char*,const DurationWindow*>,45>{{
            {"swappyPresent",&swappyPresent},
            {"stockDepth",&stockDepth},{"stockLight",&stockLight},{"stockColor",&stockColor},{"nativeSceneInclusive",&nativeSceneInclusive},
            {"worldAttempt",&worldAttempt},
            {"worldRecorded",&worldRecorded},
            {"hudPrepare",&hudPrepare},
            {"hudInterval",&hudInterval},
            {"hudFinish",&hudFinish},
            {"presentTransfers",&presentTransfers},
            {"xrWait",&xrWait},
            {"xrWork",&xrWork},
            {"xrEnd",&xrEnd},
            {"headSample",&headSample},
            {"controllerTarget",&controllerTarget},
            {"controllerAttachment",&controllerAttachment},
            {"globalCapture",&globalCapture},
            {"globalRestore",&globalRestore},
            {"resourceInspect",&resourceInspect},
            {"targetPrepare",&targetPrepare},
            {"nativePreflight",&nativePreflight},
            {"stereoDraw",&stereoDraw},
            {"leftDepth",&leftDepth},
            {"leftLight",&leftLight},
            {"leftColor",&leftColor},
            {"rightDepth",&rightDepth},
            {"rightLight",&rightLight},
            {"rightColor",&rightColor},
            {"eyeCopy",&eyeCopy},
            {"depthResolve",&depthResolve},
            {"passRestore",&passRestore},
            {"theaterPresent",&theaterPresent},
            {"theaterFenceWait",&theaterFenceWait},
            {"theaterQueueSubmit",&theaterQueueSubmit},
            {"stereoCapture",&stereoCapture},
            {"stereoRender",&stereoRender},
            {"stereoFenceWait",&stereoFenceWait},
            {"stereoQueueSubmit",&stereoQueueSubmit},
            {"stereoRebuild",&stereoRebuild},
            {"stereoScreens",&stereoScreens},
            {"stereoLockWait",&stereoLockWait},
            {"theaterLockWait",&theaterLockWait},
            {"xrImageWait",&xrImageWait},
            {"runtimeMetricsPoll",&runtimeMetricsPoll}}})out<<",\""<<name<<"\":"<<window->snapshot().json();
        return out.str()+'}';
    }
};
inline FrameTimings frameTimings;
}
