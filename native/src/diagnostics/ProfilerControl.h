#pragma once
#include <atomic>
#include <cstdint>

namespace rdrvr::diagnostics {
// Toggle diagnostic work only. Frame ownership, validation and GPU retirement
// remain active independently of profiling. Low-cost new-frame history remains
// available to the FPS overlay/menu while detailed scopes are off. Startup is
// deliberately OFF; only the explicit DETAIL setting enables these clocks.
inline std::atomic<std::uint64_t> profilerState{0};
inline bool profilingEnabled() noexcept {return (profilerState.load(std::memory_order_relaxed)&1)!=0;}
inline std::uint64_t profilerTicket() noexcept {return profilerState.load(std::memory_order_relaxed);}
inline std::atomic<bool> captureMeasurementExclusion{false};
inline std::atomic<std::uint64_t> captureExcludedFrame{0},captureBeginNs{0},captureEndNs{0},captureRateBarrierNs{0};
inline bool captureDurationExcluded(std::uint64_t begin,std::uint64_t end) noexcept {
    const auto first=captureBeginNs.load(std::memory_order_acquire);
    if(!first||end<first)return false;
    const auto last=captureEndNs.load(std::memory_order_acquire);
    return !last||begin<=last;
}
inline bool profilerTicketValid(std::uint64_t ticket) noexcept {
    return (ticket&1)&&!captureMeasurementExclusion.load(std::memory_order_acquire)&&ticket==profilerState.load(std::memory_order_relaxed);
}
inline void setProfiling(bool enabled) noexcept {
    auto state=profilerState.load(std::memory_order_relaxed);
    while(bool(state&1)!=enabled&&!profilerState.compare_exchange_weak(state,((state+2)&~std::uint64_t(1))|unsigned(enabled),std::memory_order_relaxed)){}
}
// Private diagnostic capture integration: no setting change, no automatic arm.
inline std::uint64_t beginCaptureMeasurements(std::uint64_t frame,std::uint64_t ticket,std::uint64_t now) noexcept {
    if(!frame||!now||ticket>UINT64_MAX-4)return 0;
    bool idle=false;
    if(!captureMeasurementExclusion.compare_exchange_strong(idle,true,std::memory_order_acq_rel))return 0;
    auto expected=ticket;
    if(!profilerState.compare_exchange_strong(expected,ticket+2,std::memory_order_acq_rel)){
        captureMeasurementExclusion.store(false,std::memory_order_release);return 0;
    }
    captureEndNs.store(0,std::memory_order_relaxed);
    captureBeginNs.store(now,std::memory_order_release);
    captureExcludedFrame.store(frame,std::memory_order_release);
    captureRateBarrierNs.store(UINT64_MAX,std::memory_order_release);
    return ticket+2;
}
inline void endCaptureMeasurements(std::uint64_t now) noexcept {
    captureEndNs.store(now,std::memory_order_release);
    // A sample window must not bridge the capture pause. Event rows remain for
    // provenance; live FPS starts a fresh interval after this barrier.
    captureRateBarrierNs.store(now,std::memory_order_release);
    auto state=profilerState.load(std::memory_order_relaxed);
    while(!profilerState.compare_exchange_weak(state,state+2,std::memory_order_acq_rel)){}
    captureMeasurementExclusion.store(false,std::memory_order_release);
}

}
