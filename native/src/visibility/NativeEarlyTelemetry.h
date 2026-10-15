#pragma once
#include <atomic>
#include <array>
#include <cstdint>
namespace rdrvr::experimental {
// One native render producer, XR/UI readers. Diagnostics only, no permissions.
// All payload fields are atomic; a sequence retry cannot race plain memory.
class NativeEarlyTelemetry {
public:
    struct Snapshot {std::uint64_t scene{};std::array<unsigned,4> skipped{};unsigned sources{},eligible{};};
    void publish(const Snapshot& s)noexcept {
        serial_.fetch_add(1,std::memory_order_acq_rel);
        scene_.store(s.scene,std::memory_order_relaxed);
        for(unsigned i=0;i<4;++i)skipped_[i].store(s.skipped[i],std::memory_order_relaxed);
        sources_.store(s.sources,std::memory_order_relaxed);eligible_.store(s.eligible,std::memory_order_relaxed);
        serial_.fetch_add(1,std::memory_order_release);
    }
    Snapshot snapshot()const noexcept {
        for(unsigned attempt=0;attempt<3;++attempt){
            const auto before=serial_.load(std::memory_order_acquire);if(before&1)continue;
            Snapshot out;out.scene=scene_.load(std::memory_order_relaxed);
            for(unsigned i=0;i<4;++i)out.skipped[i]=skipped_[i].load(std::memory_order_relaxed);
            out.sources=sources_.load(std::memory_order_relaxed);out.eligible=eligible_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if(before==serial_.load(std::memory_order_relaxed))return out;
        }return {};
    }
private:
    std::atomic<std::uint64_t> serial_{},scene_{};
    std::array<std::atomic<unsigned>,4> skipped_{};std::atomic<unsigned> sources_{},eligible_{};
};
inline NativeEarlyTelemetry nativeEarlyTelemetry;
}
