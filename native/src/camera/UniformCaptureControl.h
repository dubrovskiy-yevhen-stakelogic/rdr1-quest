#pragma once
#include "UniformCaptureSink.h"
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <limits>

namespace rdr95 {
// Explicit lexical producer only. Resource readiness never opens capture by
// itself. No CPU skip, object admission, or native mapped-memory lifetime grant.
class UniformCaptureControl final {
public:
    using DUD=rdrvr::engine::DynamicUniformDescriptors;
    struct Conditions {bool(*recordingThread)()noexcept{};bool(*selectedMain)()noexcept{};};
    class Producer;
private:
    struct Errno {int value=errno;~Errno(){errno=value;}};
    UniformCaptureSink& sink_;DUD::CaptureObserver sinkObserver_;Conditions conditions_;
    std::atomic<std::uint64_t> epoch_{1};std::atomic<bool> ready_{},stopped_{};
    inline static thread_local Producer* active_{};
    const DUD::CaptureBatch* pending_{};std::uint64_t pendingEpoch_{};
    bool thread()const noexcept{return conditions_.recordingThread&&conditions_.recordingThread();}
    static bool enabled(void* context)noexcept{return static_cast<UniformCaptureControl*>(context)->enabled();}
    static bool prepare(void* context,const DUD::CaptureBatch& batch)noexcept {
        Errno keep;auto& self=*static_cast<UniformCaptureControl*>(context);
        if(!self.enabled())return false;
        const auto epoch=self.epoch_.load(std::memory_order_acquire);
        self.pending_=&batch;self.pendingEpoch_=epoch;
        const bool prepared=self.sinkObserver_.prepare(self.sinkObserver_.context,batch);
        return prepared&&self.enabled()&&self.epoch_.load(std::memory_order_acquire)==epoch;
    }
    static void finish(void* context,const DUD::CaptureBatch& batch,bool committed)noexcept {
        Errno keep;auto& self=*static_cast<UniformCaptureControl*>(context);
        if(!self.thread()){self.revoke();return;}
        const bool valid=self.pending_==&batch&&self.pendingEpoch_&&
            self.pendingEpoch_==self.epoch_.load(std::memory_order_acquire)&&self.enabled();
        self.pending_=nullptr;self.pendingEpoch_=0;
        self.sinkObserver_.finish(self.sinkObserver_.context,batch,committed&&valid);
    }
    static void invalidate(void* context,const DUD::CaptureCommand&)noexcept {
        static_cast<UniformCaptureControl*>(context)->revoke();
    }
public:
    UniformCaptureControl(UniformCaptureSink& sink,Conditions conditions)noexcept:
        sink_(sink),sinkObserver_(sink.observer()),conditions_(conditions){}
    UniformCaptureControl(const UniformCaptureControl&)=delete;
    // Cold, after actual owner initialization. Does not create a Producer.
    void resourcesReady()noexcept {Errno keep;if(!stopped_.load(std::memory_order_acquire)&&epoch_.load(std::memory_order_acquire)&&sinkObserver_)ready_.store(true,std::memory_order_release);}
    void disarm()noexcept {Errno keep;stopped_.store(true,std::memory_order_release);ready_.store(false,std::memory_order_release);revoke();}
    void revoke()noexcept {
        Errno keep;auto value=epoch_.load(std::memory_order_relaxed);
        while(value){const auto next=value==std::numeric_limits<std::uint64_t>::max()?0:value+1;
            if(epoch_.compare_exchange_weak(value,next,std::memory_order_acq_rel)){if(!next)ready_.store(false,std::memory_order_release);break;}}
        if(sinkObserver_)sinkObserver_.invalidate(sinkObserver_.context,{});
    }
    DUD::CaptureObserver observer()noexcept {return {this,prepare,finish,invalidate,enabled};}
    class Producer final {
        friend class UniformCaptureControl;
        UniformCaptureControl& owner_;Producer* previous_{};std::uint64_t epoch_{};bool armed_{},poisoned_{};
    public:
        // Forthcoming exact ordinary-call recorder constructs this explicitly.
        // Unknown/nested/foreign calls MUST still construct with exact=false.
        Producer(UniformCaptureControl& owner,bool exact)noexcept:owner_(owner),previous_(active_) {
            Errno keep;
            if(previous_){previous_->poisoned_=true;previous_->owner_.revoke();}
            owner_.revoke();active_=this;epoch_=owner_.epoch_.load(std::memory_order_acquire);
            armed_=!previous_&&exact&&epoch_&&owner_.ready_.load(std::memory_order_acquire)&&owner_.thread()&&
                owner_.conditions_.selectedMain&&owner_.conditions_.selectedMain();
        }
        ~Producer(){Errno keep;poisoned_=true;owner_.revoke();active_=previous_;}
        Producer(const Producer&)=delete;Producer& operator=(const Producer&)=delete;
    };
    bool enabled()noexcept {
        Errno keep;auto* source=active_;
        if(!source||&source->owner_!=this||!source->armed_||source->poisoned_)return false;
        const auto epoch=epoch_.load(std::memory_order_acquire);
        const bool valid=!stopped_.load(std::memory_order_acquire)&&ready_.load(std::memory_order_acquire)&&source->epoch_&&
            source->epoch_==epoch&&thread()&&conditions_.selectedMain&&conditions_.selectedMain()&&
            active_==source&&!source->poisoned_&&source->epoch_==epoch&&
            epoch_.load(std::memory_order_acquire)==epoch&&!stopped_.load(std::memory_order_acquire);
        if(!valid){source->poisoned_=true;revoke();}
        return valid;
    }
    bool current(VkCommandBuffer command,unsigned mask,UniformCaptureSink::Ticket& out)noexcept {
        Errno keep;if(!enabled())return false;const auto epoch=epoch_.load(std::memory_order_acquire);
        UniformCaptureSink::Ticket value;
        if(!sink_.current(command,mask,value)||!enabled()||epoch_.load(std::memory_order_acquire)!=epoch)return false;
        out=value;return true;
    }
};
}
