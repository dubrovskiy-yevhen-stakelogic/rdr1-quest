#pragma once
#include "CameraAdapter.h"
#include <mutex>
namespace rdrvr::engine {
enum class WorldCompletion {Ready,TrackingExpired,TimerAdvanced,BodyFilterFault,VisibilityFault,ResourceFault};
// Main-thread culling may use the head only after a render-thread preflight.
// A failed replay cannot re-arm itself every other frame in the same context.
class WorldReplayGate {
    struct Key {
        std::uint64_t tracking{},space{},revision{};std::uint32_t actor{};
        bool operator==(const Key&)const=default;
    };
    static Key key(const StereoCameraPlan& p){return {p.tracking.epochs.tracking,p.tracking.epochs.referenceSpace,p.presentationRevision,p.actorGuid};}
    mutable std::mutex mutex_;Key context_{};std::uint64_t generation_{},until_{};bool faulted_{};
public:
    bool qualify(const StereoCameraPlan& p,std::uint64_t generation,std::uint64_t now){
        std::lock_guard lock(mutex_);const auto k=key(p);
        if(k!=context_||generation!=generation_){context_=k;generation_=generation;faulted_=false;}
        if(faulted_||!generation||!k.tracking||!k.space){until_=0;return false;}
        until_=now+runtime::TrackedViewsMailbox::maxAgeNs;return true;
    }
    bool ready(const StereoCameraPlan& p,std::uint64_t now)const{
        std::lock_guard lock(mutex_);return !faulted_&&until_&&now<until_&&key(p)==context_;
    }
    // Backpressure is not a failed preflight; preserve the existing bounded lease.
    void skipped(bool backpressure){if(!backpressure)invalidate();}
    void invalidate(){std::lock_guard lock(mutex_);until_=0;}
    void fault(const StereoCameraPlan& p,std::uint64_t generation){
        std::lock_guard lock(mutex_);context_=key(p);generation_=generation;until_=0;faulted_=true;
    }
    void rejectCompletion(const StereoCameraPlan& p,std::uint64_t generation,WorldCompletion result){
        // A completed but late frame is dropped, not evidence that drawing
        // corrupted the context. Re-arm only through a new normal preflight.
        if(result==WorldCompletion::TrackingExpired||result==WorldCompletion::TimerAdvanced)invalidate();
        else if(result!=WorldCompletion::Ready)fault(p,generation);
    }
};
}
