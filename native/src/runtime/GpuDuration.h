#pragma once
#include "XrPlatform.h"
#include "FramePacing.h" // laneCaptureNs / laneDisplayNs / gpuLanesAlwaysOn
#include "diagnostics/FrameTimings.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>
namespace rdrvr::runtime {
// Private serial command buffers only. Read after their EXISTING completion
// fence; no WAIT query flag and no extra submission/wait for profiling.
// One lane per in-flight command buffer (ring slot x producer/consumer);
// each lane owns a timestamp pair so results are read only after that lane's fence.
// Lanes record in every profiler mode (gpuLanesAlwaysOn, debug.rdrvr.gpulite=0 turns it off). The
// DETAIL-only outputs keep their meaning: the capture/display windows and the value completed() returns are fed only
// for a valid profiler ticket; always-on lanes feed the laneCaptureNs/laneDisplayNs EMAs (FRAME RATE ADAPTIVE) and:
//  - blitQueueDelay = display lane begin - the linked capture lane's end (same timestamp counter family; the consumer
//    waits the slot's ready semaphore, so a negative value would mean the two queues do not share the counter);
//  - blitStartLag = display lane begin - CPU submit (cntvct_el0), only when the CPU counter runs at the timestamp
//    period (+-0.5 %); reported relative to the smallest raw value seen, with that raw minimum: domain "shared"
//    (|min| <= 3 ms), "offset", "mismatch" (other frequency) or "unsupported" (no CPU counter).
class GpuDuration {
    struct Lane {
        VkCommandBuffer recording{};std::uint64_t ticket{};bool ended{};
        bool always{};                                        // Recorded because the lanes are always on
        std::uint64_t beginTicks{},endTicks{},cpuSubmit{},linkedEnd{};bool linkedValid{},measured{};
    };
    struct Signed { // last 256 signed samples, exact nearest-rank percentiles at report time
        std::array<std::int64_t,256> v{};unsigned n{},next{};std::uint64_t total{},negative{};
        void add(std::int64_t x) noexcept {v[next]=x;next=(next+1)%v.size();if(n<v.size())++n;++total;if(x<0)++negative;}
        std::int64_t percentile(unsigned pct,std::int64_t offset=0) const {
            if(!n)return 0;auto s=v;std::sort(s.begin(),s.begin()+n);return s[std::max(1u,(n*pct+99)/100)-1]-offset;}
        std::int64_t maximum() const noexcept {std::int64_t m=n?v[0]:0;for(unsigned i=0;i<n;++i)m=std::max(m,v[i]);return m;}
    };
    VkDevice device_{};VkQueryPool pool_{};double period_{};std::uint64_t mask_{};unsigned bits_{};bool attempted_{};
    std::vector<Lane> lanes_;
    std::uint64_t unavailable_{};diagnostics::DurationWindow capture_,display_;
    std::uint64_t alwaysCapture_{},alwaysDisplay_{};             // Measured lane spans (every mode)
    Signed queueDelay_,startLag_;std::int64_t minLagRaw_{INT64_MAX};std::uint64_t lagRejected_{};
public:
    static constexpr unsigned maxLanes=16;
    // a - b of two counter values that wrap at 2^bits, sign-extended (bits 1..64).
    static std::int64_t signedTicks(std::uint64_t a,std::uint64_t b,unsigned bits) noexcept {
        if(bits>=64||!bits)return std::int64_t(a-b);
        const std::uint64_t m=(1ull<<bits)-1,d=(a-b)&m;
        return (d>>(bits-1))&1u?std::int64_t(d)-std::int64_t(1ull<<bits):std::int64_t(d);
    }
    // The CPU's generic timer (19.2 MHz on Quest; the Adreno always-on counter is INFERRED to be the same).
    static std::uint64_t cpuCounter() noexcept {
#if defined(__aarch64__)
        std::uint64_t v;asm volatile("mrs %0, cntvct_el0":"=r"(v));return v;
#else
        return 0;
#endif
    }
    static std::uint64_t cpuCounterHz() noexcept {
#if defined(__aarch64__)
        std::uint64_t v;asm volatile("mrs %0, cntfrq_el0":"=r"(v));return v;
#else
        return 0;
#endif
    }
    // "shared" | "offset" | "mismatch" | "unsupported" (see the class comment).
    const char* lagDomain() const noexcept {
        const auto hz=cpuCounterHz();
        if(!hz||period_<=0)return "unsupported";
        if(std::abs(1e9/double(hz)-period_)>=0.005*period_)return "mismatch";
        if(!startLag_.n)return "unknown";
        return std::llabs(minLagRaw_)<=3'000'000?"shared":"offset";
    }
    void initialize(VkPhysicalDevice physical,VkDevice device,unsigned family,unsigned lanes=1){
        if(attempted_)return;attempted_=true;device_=device;
        if(!lanes||lanes>maxLanes)return;
        lanes_.assign(lanes,{});
        std::uint32_t count{};vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,queues.data());
        if(family>=count||!queues[family].timestampValidBits)return;
        const auto bits=queues[family].timestampValidBits;mask_=bits>=64?UINT64_MAX:(1ull<<bits)-1;bits_=bits;
        VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(physical,&props);period_=props.limits.timestampPeriod;if(period_<=0)return;
        VkQueryPoolCreateInfo create{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};create.queryType=VK_QUERY_TYPE_TIMESTAMP;create.queryCount=2*lanes;
        if(vkCreateQueryPool(device,&create,nullptr,&pool_)!=VK_SUCCESS)pool_={};
    }
    void begin(VkCommandBuffer c,unsigned lane=0){
        if(!pool_||!c||lane>=lanes_.size())return;auto& l=lanes_[lane];if(l.recording)return;
        l.ticket=diagnostics::profilerTicket();
        l.always=gpuLanesAlwaysOn.load(std::memory_order_relaxed);
        if(!(l.ticket&1)&&!l.always)return;
        l.recording=c;l.ended=false;
        vkCmdResetQueryPool(c,pool_,2*lane,2);vkCmdWriteTimestamp(c,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,pool_,2*lane);
    }
    void end(VkCommandBuffer c,unsigned lane=0){
        // Completes a query pair already recorded even if profiling was switched OFF
        // between begin/end. Completion will discard that measurement.
        if(lane>=lanes_.size())return;auto& l=lanes_[lane];
        if(l.recording&&l.recording==c&&!l.ended){vkCmdWriteTimestamp(c,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,pool_,2*lane+1);l.ended=true;}
    }
    // CPU counter right before the lane's command buffer is submitted (display lanes: blitStartLag).
    void noteSubmit(unsigned lane) noexcept {if(lane<lanes_.size())lanes_[lane].cpuSubmit=cpuCounter();}
    // The display lane `consumer` reads the pair captured by lane `producer`, whose completion was already
    // observed: remember that capture's end timestamp (only if it was measured).
    void link(unsigned consumer,unsigned producer) noexcept {
        if(consumer>=lanes_.size()||producer>=lanes_.size())return;
        auto& c=lanes_[consumer];const auto& p=lanes_[producer];c.linkedValid=p.measured&&!p.recording;c.linkedEnd=p.endTicks; // Never a stale end of the slot's previous capture
    }
    // Call once the lane's command buffer completion fence has been observed signalled.
    // Returns the measured GPU ns (0 = not measured) so callers can attribute it.
    // The return value and the capture/display windows stay DETAIL-only (valid profiler ticket).
    std::uint64_t completed(unsigned lane,bool display){
        if(lane>=lanes_.size())return 0;auto& l=lanes_[lane];
        const bool detail=diagnostics::profilerTicketValid(l.ticket);
        const bool measure=pool_&&l.recording&&l.ended&&(l.always||detail);
        const bool linked=l.linkedValid;const auto linkedEnd=l.linkedEnd,submitted=l.cpuSubmit;
        l.recording={};l.ended=false;l.ticket=0;l.always=false;l.measured=false;l.linkedValid=false;l.cpuSubmit=0;
        if(!measure)return 0;
        struct Value {std::uint64_t ticks,available;};std::array<Value,2> values{};
        const auto result=vkGetQueryPoolResults(device_,pool_,2*lane,2,sizeof(values),values.data(),sizeof(Value),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if(result!=VK_SUCCESS||!values[0].available||!values[1].available){++unavailable_;return 0;}
        const auto ticks=(values[1].ticks-values[0].ticks)&mask_;
        const double ns=ticks*period_;if(ns<0||ns>60e9){++unavailable_;return 0;}
        // Always-on outputs.
        l.beginTicks=values[0].ticks&mask_;l.endTicks=values[1].ticks&mask_;l.measured=true;
        {auto& ema=display?laneDisplayNs:laneCaptureNs;const auto x=static_cast<std::uint64_t>(ns);const auto e=ema.load(std::memory_order_relaxed);
         ema.store(e?std::uint64_t(std::int64_t(e)+(std::int64_t(x)-std::int64_t(e))/8):x,std::memory_order_relaxed);}
        ++(display?alwaysDisplay_:alwaysCapture_);
        if(display&&linked)queueDelay_.add(std::int64_t(double(signedTicks(l.beginTicks,linkedEnd,bits_))*period_));
        if(display&&submitted){
            const auto hz=cpuCounterHz();
            if(hz&&std::abs(1e9/double(hz)-period_)<0.005*period_){
                const auto raw=std::int64_t(double(signedTicks(l.beginTicks,submitted&mask_,bits_))*period_);
                startLag_.add(raw);minLagRaw_=std::min(minLagRaw_,raw);
            }else ++lagRejected_;
        }
        if(!detail)return 0;
        (display?display_:capture_).elapsed(1,1+static_cast<std::uint64_t>(ns));
        return 1+static_cast<std::uint64_t>(ns);
    }
    void completed(bool display){completed(0,display);}
    // CLOCK_MONOTONIC ns of the lane's last measured end timestamp. Call right after completed(lane) (the stereo mailbox:
    // when it observes a capture fence signalled), nowNs = CLOCK_MONOTONIC read now. The GPU counter runs at the CPU generic
    // timer's rate with a constant offset (blitStartLag domain "shared" / "offset"): offset = the smallest display-lane begin -
    // CPU submit seen, i.e. the true submit->start lag floor is folded in (ready may come out up to that floor early). 0 = unknown:
    // lane not measured, no blitStartLag sample yet, another counter frequency, no CPU counter (host), or a value after nowNs.
    std::uint64_t endMonotonicNs(unsigned lane,std::uint64_t nowNs) const noexcept {
        if(lane>=lanes_.size()||!lanes_[lane].measured||!startLag_.n||period_<=0||!nowNs)return 0;
        const auto hz=cpuCounterHz();if(!hz||std::abs(1e9/double(hz)-period_)>=0.005*period_)return 0;
        const std::int64_t d=std::int64_t(double(signedTicks(lanes_[lane].endTicks,cpuCounter()&mask_,bits_))*period_)-minLagRaw_;
        if(d>0||d<-1'000'000'000ll||std::int64_t(nowNs)+d<=0)return 0;
        return std::uint64_t(std::int64_t(nowNs)+d);
    }
    bool recording(unsigned lane)const{return lane<lanes_.size()&&lanes_[lane].recording;}
    void destroy(){if(pool_)vkDestroyQueryPool(device_,pool_,nullptr);pool_={};attempted_=false;device_={};lanes_.clear();}
    std::string report()const{
        std::ostringstream o;o<<"{\"supported\":"<<(pool_?"true":"false")<<",\"clock\":\"vulkan-timestamp\",\"scope\":\"mailbox-transfer-only\",\"extraWaits\":0,\"lanes\":"<<lanes_.size()<<",\"timestampPeriod\":"<<period_<<",\"unavailable\":"<<unavailable_
         <<",\"capture\":"<<capture_.snapshot().json()<<",\"display\":"<<display_.snapshot().json();
        o<<",\"alwaysOn\":"<<(gpuLanesAlwaysOn.load(std::memory_order_relaxed)?"true":"false")<<",\"validBits\":"<<bits_
         <<",\"alwaysOnSamples\":{\"capture\":"<<alwaysCapture_<<",\"display\":"<<alwaysDisplay_<<'}'
         <<",\"laneEmaNs\":{\"capture\":"<<laneCaptureNs.load(std::memory_order_relaxed)<<",\"display\":"<<laneDisplayNs.load(std::memory_order_relaxed)<<'}'
         <<",\"blitQueueDelay\":{\"samples\":"<<queueDelay_.n<<",\"total\":"<<queueDelay_.total<<",\"p50Ns\":"<<queueDelay_.percentile(50)<<",\"p95Ns\":"<<queueDelay_.percentile(95)
         <<",\"maxNs\":"<<queueDelay_.maximum()<<",\"negative\":"<<queueDelay_.negative<<'}'
         <<",\"blitStartLag\":{\"samples\":"<<startLag_.n<<",\"total\":"<<startLag_.total<<",\"p50Ns\":"<<startLag_.percentile(50,startLag_.n?minLagRaw_:0)
         <<",\"p95Ns\":"<<startLag_.percentile(95,startLag_.n?minLagRaw_:0)<<",\"minRawNs\":"<<(startLag_.n?minLagRaw_:0)<<",\"rejected\":"<<lagRejected_
         <<",\"cpuCounterHz\":"<<cpuCounterHz()<<",\"domain\":\""<<lagDomain()<<"\"}}";
        return o.str();
    }
    // Test accessors: measured blitQueueDelay samples / negatives.
    std::uint64_t queueDelaySamples() const noexcept {return queueDelay_.total;}
    std::uint64_t queueDelayNegative() const noexcept {return queueDelay_.negative;}
    std::uint64_t alwaysOnSamples(bool display) const noexcept {return display?alwaysDisplay_:alwaysCapture_;}
};
}
