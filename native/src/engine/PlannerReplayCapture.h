#pragma once
#include "PlannerReplaySnapshot.h"
#include <atomic>
#include <cstdint>
#include <string>
namespace rdrvr::engine {
// PRIVATE diagnostic controller. One explicit request per process; never arms
// from a settings change, never changes source eligibility or native rendering.
class PlannerReplayCapture {
public:
    enum class State:unsigned {Idle,Arming,Armed,Claimed,Ready,Taken,Failed};
    enum class Failure:unsigned {None,ProfilerChanged,NoMatchingScene,Before,After,Incomplete,Cancelled};
    class Scene {
        friend class PlannerReplayCapture;
        PlannerReplayCapture* owner_{};Scene* previous_{};bool claimed_{};
    public:
        explicit Scene(PlannerReplayCapture&)noexcept;
        ~Scene();Scene(const Scene&)=delete;Scene&operator=(const Scene&)=delete;
    };
    explicit PlannerReplayCapture(unsigned qualifiedWarmupFrames=0)noexcept:warmupRequired_(qualifiedWarmupFrames){}
    unsigned warmupCompleted()const noexcept{return warmupCompleted_.load(std::memory_order_relaxed);}
    bool request()noexcept;bool cancelPending()noexcept;
    bool claim(std::uint64_t frame,std::uint64_t detailedEpoch)noexcept;
    void before(std::uint64_t frame,PriorityOcclusionSources&,const std::array<NativeStaticOcclusionSource::Camera,2>&,
                unsigned width,unsigned height,std::string_view buildId,std::span<const PlannerReplaySnapshot::Counter>)noexcept;
    void abandonFrame(std::uint64_t frame)noexcept;
    void after(std::uint64_t frame,const PriorityOcclusionSources&)noexcept;
    bool claimed(std::uint64_t frame)const noexcept;
    std::vector<std::byte> take()noexcept;
    void closeWriter()noexcept; // Terminal background worker exit, including during Claimed.
    void completeWrite()noexcept; // Reporter finally, including allocation/IO failure.
    State state()const noexcept{return state_.load(std::memory_order_acquire);}
    std::string report()const;
private:
    static thread_local Scene* current_;
    std::atomic<State> state_{State::Idle};std::atomic<Failure> failure_{Failure::None};
    // Request epoch is diagnostic provenance, not a gate on future startup modes.
    std::atomic<std::uint64_t> requestedEpoch_{},boundEpoch_{},captureEpoch_{},preCaptureDeferrals_{},frame_{},byteSize_{};
    const unsigned warmupRequired_{};std::atomic<unsigned> warmupCompleted_{};
    PlannerReplaySnapshot snapshot_;bool before_{};std::atomic<bool> ownsMeasurementExclusion_{false},writerClosed_{false};
    void finish(Scene&)noexcept;
};
extern PlannerReplayCapture plannerReplayCapture;
}
