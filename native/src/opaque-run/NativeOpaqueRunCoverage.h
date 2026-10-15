#pragma once
#include <atomic>
#include <cstdint>
#include <limits>

namespace rdrvr::experimental {
class NativeOpaqueRunHookState;
// Owner/context and the exact mapped retail GOT storage outlive this object and
// every in-flight wrapper. Only the verified hook transaction can arm it.
class NativeOpaqueRunCoverage {
    friend class NativeOpaqueRunHookState;
    std::atomic<std::uint64_t> state_{};
    void* context_{};
    bool(*owned_)(void*)noexcept{};
    bool attach(void* p,bool(*fn)(void*)noexcept)noexcept {
        if(context_||owned_||!p||!fn)return false;
        context_=p;owned_=fn;return true;
    }
    bool advance(bool ready)noexcept {
        auto old=state_.load(std::memory_order_acquire);
        for(;;){
            constexpr auto end=std::numeric_limits<std::uint64_t>::max()-1;
            if(old>=end-2){state_.store(end,std::memory_order_release);return false;}
            const auto next=((old+2)&~std::uint64_t{1})|(ready?1u:0u);
            if(state_.compare_exchange_weak(old,next,std::memory_order_acq_rel,std::memory_order_acquire))return ready;
        }
    }
    bool arm(std::uint64_t expected)noexcept {
        constexpr auto end=std::numeric_limits<std::uint64_t>::max()-1;
        if((expected&1)||expected>=end-2||!owned_||!owned_(context_))return false;
        const auto next=(expected+2)|1u;
        return state_.compare_exchange_strong(expected,next,std::memory_order_acq_rel,std::memory_order_acquire);
    }
    friend struct NativeOpaqueRunCoverageTestAccess;
public:
    NativeOpaqueRunCoverage()noexcept=default;
    NativeOpaqueRunCoverage(const NativeOpaqueRunCoverage&)=delete;
    void disarm()noexcept{advance(false);}
    std::uint64_t epoch()noexcept {
        const auto before=state_.load(std::memory_order_acquire);
        if(!(before&1))return 0;
        if(!owned_||!owned_(context_)){disarm();return 0;}
        return state_.load(std::memory_order_acquire)==before?before:0;
    }
};
inline NativeOpaqueRunCoverage nativeOpaqueRunCoverage;
inline std::uint64_t nativeOpaqueCoverageEpoch()noexcept{return nativeOpaqueRunCoverage.epoch();}
inline void nativeOpaqueCoverageDisarm()noexcept{nativeOpaqueRunCoverage.disarm();}
}
