#pragma once
#include <atomic>
#include <cstdint>
#include <limits>
namespace rdr95 {
// Semantic invalidation only. This is not a lock, GPU lease, or memory pin.
// Epoch and active writers share one atomic word: a reader cannot observe a
// new epoch with an old independently sampled zero writer count.
class CameraMetadataActivity final {
    static constexpr std::uint64_t CountMask=65535,MaxEpoch=UINT64_MAX>>16;
    std::atomic<std::uint64_t> state_{std::uint64_t(1)<<16};
    const std::uint64_t limit_;
    void change(bool enter)noexcept {
        auto old=state_.load(std::memory_order_acquire);
        while(old!=UINT64_MAX){
            const auto epoch=old>>16,count=old&CountMask;
            const bool valid=epoch<limit_&&(enter?count<CountMask:count!=0);
            const auto next=valid?((epoch+1)<<16)|(enter?count+1:count-1):UINT64_MAX;
            if(state_.compare_exchange_weak(old,next,std::memory_order_acq_rel,std::memory_order_acquire))return;
        }
    }
public:
    // Smaller limits only shorten eligibility; useful for finite exhaustion tests.
    explicit CameraMetadataActivity(std::uint64_t epochLimit=MaxEpoch)noexcept:limit_(epochLimit<MaxEpoch?epochLimit:MaxEpoch){}
    class Mutation final {
        CameraMetadataActivity& owner_;
    public:
        explicit Mutation(CameraMetadataActivity& o)noexcept:owner_(o){owner_.change(true);}
        ~Mutation(){owner_.change(false);}
        Mutation(const Mutation&)=delete;Mutation&operator=(const Mutation&)=delete;
    };
    std::uint64_t current()const noexcept {
        const auto value=state_.load(std::memory_order_acquire);
        return value!=UINT64_MAX&&(value&CountMask)==0&&(value>>16)<=limit_?value:0;
    }
};
}
