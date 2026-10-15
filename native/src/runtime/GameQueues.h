#pragma once
// The game's own queue handles, resolved by the Vulkan bridge once after device creation:
// Appended layout = vkGetDeviceQueue(family,0..n-1) (the stock handles), Split layouts =
// vkGetDeviceQueue2(PROTECTED, family, i). Producers that build a "game queue" list
// (StereoMailbox.cpp:77, GameTheater.cpp:67, GameStereoCapture.cpp:24, GameHudCapture.cpp:19) use gameQueueList()
// when it returns > 0 and fall back to their own vkGetDeviceQueue loop while it returns 0 (nothing published).
#include "GraphicsSettings.h"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#if defined(__ANDROID__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace rdrvr::runtime {
class GameQueues {
public:
    static constexpr unsigned capacity=4;
    // Publisher (device creation): 1..capacity non-null handles, else nothing is published and false is returned.
    // A later publish (device re-creation) replaces the list.
    bool publish(const VkQueue* queues,unsigned n) noexcept {
        if(!queues||!n||n>capacity)return false;
        for(unsigned i=0;i<n;++i)if(!queues[i])return false;
        count_.store(0,std::memory_order_release);
        for(unsigned i=0;i<n;++i)queues_[i].store(queues[i],std::memory_order_relaxed);
        count_.store(n,std::memory_order_release);publishes_.fetch_add(1,std::memory_order_relaxed);
        return true;
    }
    // Copies up to max handles into out; returns how many (0 while unpublished or when out is null).
    unsigned list(VkQueue* out,unsigned max) const noexcept {
        const unsigned n=count_.load(std::memory_order_acquire);
        if(!out)return 0;
        const unsigned m=n<max?n:max;
        for(unsigned i=0;i<m;++i)out[i]=queues_[i].load(std::memory_order_relaxed);
        return m;
    }
    unsigned count() const noexcept {return count_.load(std::memory_order_acquire);}
    std::uint64_t publishes() const noexcept {return publishes_.load(std::memory_order_relaxed);}
    void reset() noexcept {count_.store(0,std::memory_order_release);} // device destroyed
private:
    std::array<std::atomic<VkQueue>,capacity> queues_{};
    std::atomic<unsigned> count_{};
    std::atomic<std::uint64_t> publishes_{};
};
inline GameQueues gameQueues;
inline bool publishGameQueues(const VkQueue* queues,unsigned n) noexcept {return gameQueues.publish(queues,n);}
inline unsigned gameQueueList(VkQueue* out,unsigned max) noexcept {return gameQueues.list(out,max);}

// The queue layout the game device was created with (published by
// GameVulkanBridge::createDevice before the device is returned to the game), the xrQueueVerified
// safety gate (gameQueueVerifyTick) and the KGSL preemption counter, for gameXr.queueLayout
// (gameQueueReport). Gate: a split start writes xrQueueVerified=0 right before its
// xrCreateVulkanDeviceKHR; 300 world stereo layer frames later (runtime::xrLayerFramesSubmitted;
// theater/menu layers excluded), and only once the game took at least one redirected queue
// (redirects > 0), the tick writes 1. A start whose cfg asks PRIORITY while the previous split start
// stayed 0 (crash, hang or dead display) uses Appended (reason PREV_SPLIT_UNVERIFIED) and keeps the
// 0 until XR QUEUE NORMAL is selected (the tick then writes 1: the next PRIORITY start tries the
// split again).
class GameQueueLayout {
public:
    struct Info {
        int requested{-1},active{-1};         // QueueLayout values: 0 Appended, 2 SplitHigh, 3 SplitNormal; -1 not decided
        const char* source{"none"};           // property | cfg | default
        const char* reason{"NOT_DECIDED"};    // OK or why Appended was used (static string)
        bool fallback{},prevUnverified{},splitAttempted{},protectedMemory{},globalPriorityExtAdded{};
        bool gateWritten{};                   // xrQueueVerified=0 was persisted for this start (split device created)
        bool startupVerified{true};           // xrQueueVerified as loaded at startup
        int splitXrResult{},splitVkResult{};  // the split create's results (0 when not attempted)
        unsigned gameQueues{},gameQueueFlags{};   // the game's queue count in the family; its create-info flags (1 = PROTECTED)
        unsigned xrFamily{},xrIndex{},xrFlags{},globalPriority{};
        unsigned familyFlags{},familyQueues{};
        const char* globalPriorityExt{""};
        bool deviceCreated{};
    };
    enum Gate : int {GateIdle=0,GateVerifying=1,GateRearm=2,GateVerified=3,GateRearmed=4,GateWriteFailed=5,GateNotGated=6};
    static constexpr std::uint64_t verifyFrames=300;
    static constexpr unsigned preemptRing=64;
    static constexpr std::uint64_t preemptPeriodNs=1'000'000'000ull;
    static const char* layoutName(int v) noexcept {return v==0?"APPENDED":v==2?"SPLIT_HIGH":v==3?"SPLIT_NORMAL":"NONE";}
    static const char* gateName(int g) noexcept {
        switch(g){case GateIdle:return "IDLE";case GateVerifying:return "VERIFYING";case GateRearm:return "WAIT_NORMAL";
            case GateVerified:return "VERIFIED";case GateRearmed:return "REARMED";case GateWriteFailed:return "WRITE_FAILED";
            case GateNotGated:return "NOT_GATED";}
        return "?";
    }
    // Bridge, once per game device (also on a failed create, with deviceCreated false). settings receives the gate
    // writes (nullptr: never written); layerFrames = runtime::xrLayerFramesSubmitted now.
    void publish(const Info& info,GraphicsSettings* settings,std::uint64_t layerFrames) noexcept {
        {std::lock_guard lock(mutex_);info_=info;}
        settings_.store(settings,std::memory_order_relaxed);baseline_.store(layerFrames,std::memory_order_relaxed);
        const bool split=info.active==2||info.active==3;
        int gate=GateIdle;
        if(info.deviceCreated&&split)gate=info.gateWritten?GateVerifying:GateNotGated;
        else if(info.deviceCreated&&!info.startupVerified&&settings)gate=GateRearm;
        gate_.store(gate,std::memory_order_release);
    }
    Info info() const noexcept {std::lock_guard lock(mutex_);return info_;}
    int gate() const noexcept {return gate_.load(std::memory_order_acquire);}
    // Render thread, once per frame (gameQueueVerifyTick). Cheap unless the gate has work: one atomic load and a clock compare.
    void tick(std::uint64_t layerFrames,std::uint64_t nowNs) noexcept {
        const int gate=gate_.load(std::memory_order_acquire);
        if(gate==GateVerifying) {
            const auto base=baseline_.load(std::memory_order_relaxed);
            if(layerFrames>=base&&layerFrames-base>=verifyFrames&&redirects.load(std::memory_order_relaxed)>0) { // Game queues really redirected
                auto* settings=settings_.load(std::memory_order_relaxed);
                const bool ok=settings&&settings->setXrQueueVerified(true);
                verifiedAtFrames_.store(layerFrames-base,std::memory_order_relaxed);writes_.fetch_add(1,std::memory_order_relaxed);
                gate_.store(ok?GateVerified:GateWriteFailed,std::memory_order_release);
            }
        }else if(gate==GateRearm) {
            auto* settings=settings_.load(std::memory_order_relaxed);
            if(settings&&settings->snapshot().requested.xrQueuePriority==0) {
                const bool ok=settings->setXrQueueVerified(true);writes_.fetch_add(1,std::memory_order_relaxed);
                gate_.store(ok?GateRearmed:GateWriteFailed,std::memory_order_release);
            }
        }
        if(nowNs>=nextPreemptNs_) {nextPreemptNs_=nowNs+preemptPeriodNs;samplePreempt(nowNs);}
        lastLayerFrames_.store(layerFrames,std::memory_order_relaxed);
    }
    // KGSL /sys/class/kgsl/kgsl-3d0/preempt_count sample (global counter; it drops to 0 on GPU slumber).
    void notePreemptCount(std::uint64_t value,std::uint64_t nowNs) noexcept {
        preemptReadable_.store(1,std::memory_order_relaxed);
        if(!preemptHave_||value<preemptLast_||nowNs<=preemptLastNs_) {
            if(preemptHave_&&value<preemptLast_)preemptResets_.fetch_add(1,std::memory_order_relaxed);
            preemptHave_=true;preemptLast_=value;preemptLastNs_=nowNs;return;
        }
        const std::uint64_t delta=value-preemptLast_,elapsed=nowNs-preemptLastNs_;
        const std::uint64_t perSecond=(delta*1'000'000'000ull+elapsed/2)/elapsed;
        const auto n=preemptSamples_.load(std::memory_order_relaxed);
        preemptRate_[n%preemptRing].store(static_cast<std::uint32_t>(std::min<std::uint64_t>(perSecond,0xffffffffu)),std::memory_order_relaxed);
        preemptSamples_.store(n+1,std::memory_order_relaxed);preemptTotal_.fetch_add(delta,std::memory_order_relaxed);
        preemptLast_=value;preemptLastNs_=nowNs;
    }
    void markPreemptUnreadable() noexcept {preemptReadable_.store(0,std::memory_order_relaxed);}
    std::string report() const {
        const Info i=info();
        const auto b=[](bool v){return v?"true":"false";};
        const auto u=[](std::uint64_t v){return std::to_string(v);};
        std::string s="{\"requested\":"+std::to_string(i.requested)+",\"active\":"+std::to_string(i.active)
            +",\"requestedName\":\""+layoutName(i.requested)+"\",\"activeName\":\""+layoutName(i.active)
            +"\",\"source\":\""+i.source+"\",\"fallback\":"+b(i.fallback)+",\"reason\":\""+i.reason
            +"\",\"deviceCreated\":"+b(i.deviceCreated)+",\"splitAttempted\":"+b(i.splitAttempted)
            +",\"splitXrResult\":"+std::to_string(i.splitXrResult)+",\"splitVkResult\":"+std::to_string(i.splitVkResult)
            +",\"gameQueues\":"+u(i.gameQueues)+",\"gameQueueFlags\":"+u(i.gameQueueFlags)+",\"publishedQueues\":"+u(gameQueues.count())
            +",\"publishes\":"+u(gameQueues.publishes())
            +",\"xrQueue\":{\"family\":"+u(i.xrFamily)+",\"index\":"+u(i.xrIndex)+",\"flags\":"+u(i.xrFlags)+",\"globalPriority\":"+u(i.globalPriority)+'}'
            +",\"familyFlags\":"+u(i.familyFlags)+",\"familyQueues\":"+u(i.familyQueues)
            +",\"protectedMemory\":"+b(i.protectedMemory)+",\"globalPriorityExt\":\""+i.globalPriorityExt+"\",\"globalPriorityExtAdded\":"+b(i.globalPriorityExtAdded)
            +",\"redirects\":"+u(redirects.load(std::memory_order_relaxed))+",\"forwarded\":"+u(forwarded.load(std::memory_order_relaxed))
            +",\"prevUnverified\":"+b(i.prevUnverified)+",\"startupVerified\":"+b(i.startupVerified)
            +",\"verify\":{\"gate\":\""+gateName(gate())+"\",\"gateWritten\":"+b(i.gateWritten)+",\"needed\":"+u(verifyFrames)
            +",\"layerFrames\":"+u(lastLayerFrames_.load(std::memory_order_relaxed)-std::min(lastLayerFrames_.load(std::memory_order_relaxed),baseline_.load(std::memory_order_relaxed)))
            +",\"verifiedAtFrames\":"+u(verifiedAtFrames_.load(std::memory_order_relaxed))+",\"writes\":"+u(writes_.load(std::memory_order_relaxed))+'}';
        const int readable=preemptReadable_.load(std::memory_order_relaxed);
        const auto samples=preemptSamples_.load(std::memory_order_relaxed);
        std::array<std::uint32_t,preemptRing> rates{};const unsigned n=static_cast<unsigned>(std::min<std::uint64_t>(samples,preemptRing));
        for(unsigned k=0;k<n;++k)rates[k]=preemptRate_[k].load(std::memory_order_relaxed);
        std::sort(rates.begin(),rates.begin()+n);
        s+=",\"gpuPreempt\":{\"readable\":"+std::string(b(readable==1))+",\"attempted\":"+b(readable>=0)
            +",\"total\":"+(readable==1?u(preemptTotal_.load(std::memory_order_relaxed)):std::string("-1"))
            +",\"perSecondP50\":"+(n?u(rates[(n-1)/2]):std::string("-1"))+",\"perSecondMax\":"+(n?u(rates[n-1]):std::string("-1"))
            +",\"samples\":"+u(samples)+",\"resets\":"+u(preemptResets_.load(std::memory_order_relaxed))+"}}";
        return s;
    }
    void reset() noexcept { // host tests
        {std::lock_guard lock(mutex_);info_={};}
        settings_.store(nullptr);baseline_.store(0);gate_.store(GateIdle);verifiedAtFrames_.store(0);writes_.store(0);lastLayerFrames_.store(0);
        redirects.store(0);forwarded.store(0);preemptReadable_.store(-1);preemptSamples_.store(0);preemptTotal_.store(0);preemptResets_.store(0);
        preemptHave_=false;preemptLast_=preemptLastNs_=0;nextPreemptNs_=0;
    }
    std::atomic<std::uint64_t> redirects{},forwarded{}; // GameVulkanBridge::getDeviceQueue: split redirects / pass-through
private:
    void samplePreempt([[maybe_unused]] std::uint64_t nowNs) noexcept {
#if defined(__ANDROID__)
        if(preemptReadable_.load(std::memory_order_relaxed)==0)return; // SELinux or a missing node: tried once, never again
        const int fd=::open("/sys/class/kgsl/kgsl-3d0/preempt_count",O_RDONLY|O_CLOEXEC);
        char text[32]{};const auto got=fd>=0?::read(fd,text,sizeof text-1):-1;
        if(fd>=0)::close(fd);
        char* end{};const unsigned long long value=got>0?std::strtoull(text,&end,10):0;
        if(got<=0||end==text){markPreemptUnreadable();return;}
        notePreemptCount(value,nowNs);
#endif
    }
    mutable std::mutex mutex_;
    Info info_{};
    std::atomic<GraphicsSettings*> settings_{};
    std::atomic<std::uint64_t> baseline_{},verifiedAtFrames_{},writes_{},lastLayerFrames_{};
    std::atomic<int> gate_{GateIdle};
    // Preemption sampling: render thread only (tick); the report reads the atomics.
    std::uint64_t nextPreemptNs_{},preemptLast_{},preemptLastNs_{};bool preemptHave_{};
    std::atomic<int> preemptReadable_{-1};
    std::atomic<std::uint64_t> preemptSamples_{},preemptTotal_{},preemptResets_{};
    std::array<std::atomic<std::uint32_t>,preemptRing> preemptRate_{};
};
inline GameQueueLayout gameQueueLayout;
// Render thread, once per frame (GameXr FinishPrimary): the xrQueueVerified gate (after 300 layer frames of a
// split start) and the 1 Hz KGSL preemption sample. Never throws; no file access unless the gate fires (once) or 1 s passed.
inline void gameQueueVerifyTick() noexcept {
    const auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    gameQueueLayout.tick(xrLayerFramesSubmitted.load(std::memory_order_relaxed),now>0?static_cast<std::uint64_t>(now):0);
}
// gameXr report "queueLayout".
inline std::string gameQueueReport() {return gameQueueLayout.report();}
}
