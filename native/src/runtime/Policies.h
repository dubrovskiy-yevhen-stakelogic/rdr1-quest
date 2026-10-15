#pragma once
#include <cstdint>
namespace rdrvr::runtime {
class PressGate {
    bool armed_{},held_{};
public:
    void invalidate() { armed_=false; held_=false; }
    bool sample(bool active,bool down,bool enabled) {
        if(!active || !enabled) {invalidate();return false;}
        if(!armed_) {if(!down) armed_=true; return false;}
        const bool edge=down && !held_; held_=down; return edge;
    }
};
// Host-testable gates. XR/Vulkan results must be interpreted by the caller.
class TrackingGate {
    bool lost_{true};
    unsigned consecutive_{};
public:
    std::uint64_t epoch{1};
    bool ready()const{return !lost_;}
    void invalidate() { ++epoch; lost_ = true; consecutive_ = 0; }
    // The physical reference space and tracking validity have not changed;
    // retire old images/poses without manufacturing a tracking-loss interval.
    void retireContent() { ++epoch; }
    bool sample(bool valid) {
        if (!valid) {
            if (!lost_) invalidate();
            consecutive_ = 0;
            return false;
        }
        if (lost_ && ++consecutive_ >= 2) lost_ = false;
        return !lost_;
    }
};
enum class ImagePhase { Idle, Acquired, Waited, Submitted, Complete };
class ImageCycle {
    ImagePhase phase_{ImagePhase::Idle};
public:
    ImagePhase phase() const { return phase_; }
    bool acquire() { if (phase_ != ImagePhase::Idle) return false; phase_ = ImagePhase::Acquired; return true; }
    bool wait(bool succeeded) {
        if (phase_ != ImagePhase::Acquired || !succeeded) return false;
        phase_ = ImagePhase::Waited; return true;
    }
    bool submit() { if (phase_ != ImagePhase::Waited) return false; phase_ = ImagePhase::Submitted; return true; }
    bool complete(bool fenceSignaled) {
        if (phase_ != ImagePhase::Submitted || !fenceSignaled) return false;
        phase_ = ImagePhase::Complete; return true;
    }
    bool release() { if (phase_ != ImagePhase::Complete) return false; phase_ = ImagePhase::Idle; return true; }
};
} // namespace rdrvr::runtime
