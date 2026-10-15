#pragma once
#include "PriorityOcclusionSources.h"
#include <atomic>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace rdrvr::engine {
// Diagnostic data only; no restore or visibility-authority API is compiled here.
// The scene owner copies one bounded snapshot. A separate reporter may take its
// completed bytes later, without keeping native pointers or cache leases alive.
class PlannerReplaySnapshot {
public:
    static constexpr std::size_t MaxBytes=32*1024*1024;
    enum class State:unsigned {Idle,Capturing,Ready,Taken,Failed};
    bool captureBefore(PriorityOcclusionSources&,const std::array<NativeStaticOcclusionSource::Camera,2>&,
                       unsigned width,unsigned height,std::uint64_t epoch,std::string_view buildId)noexcept;
    struct Counter {std::string_view name;std::uint64_t value;};
    bool metadata(std::span<const Counter>)noexcept;
    void abandon()noexcept{fail();}
    void captureAfter(const PriorityOcclusionSources&)noexcept;
    std::vector<std::byte> take()noexcept;
    State state()const noexcept{return state_.load(std::memory_order_acquire);}
    std::uint64_t frame()const noexcept{return frame_.load(std::memory_order_acquire);}
private:
    std::atomic<State> state_{State::Idle};std::atomic<std::uint64_t> frame_{};
    std::vector<std::byte> bytes_;
    std::uint64_t serial_{};const OwnedStaticOccluders* cache_{};
    void field(std::string_view,unsigned kind,const void*,std::size_t count,std::size_t size);
    void u64(std::string_view,std::uint64_t);
    void u32(std::string_view,std::span<const std::uint32_t>);
    void f32(std::string_view,std::span<const float>);
    void f64(std::string_view,std::span<const double>);
    void entry(std::string_view,const PriorityOcclusionSources::Entry&);
    void grid(std::string_view,const PriorityOcclusionSources&);
    void rasterState(std::string_view,const ConservativeStereoRaster&);
    void stats(std::string_view,const PriorityOcclusionSources::Stats&);
    void mesh(std::string_view,const OwnedStaticOccluders::Snapshot&);
    void fail()noexcept;
};
}
