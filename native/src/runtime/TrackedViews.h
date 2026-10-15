#pragma once
#include "Math.h"
#include "contracts/SubmissionPolicy.h"
#include <mutex>

namespace rdrvr::runtime {
// A single publication owns both eyes. XrTime is never compared with steady-clock time.
struct TrackedViews {
    std::uint64_t frameId{}, publishedNs{};
    std::int64_t displayTime{};
    Epochs epochs{};
    std::array<Pose,2> eyes{};
    std::array<Fov,2> fov{};
};
inline bool usableFov(Fov f) {
    constexpr float limit=1.5707f;
    return std::isfinite(f.left) && std::isfinite(f.right) && std::isfinite(f.up) && std::isfinite(f.down) &&
        f.left>-limit && f.right<limit && f.down>-limit && f.up<limit &&
        f.right-f.left>0.01f && f.up-f.down>0.01f;
}
class TrackedViewsMailbox {
    mutable std::mutex mutex_;
    TrackedViews latest_{};
    bool available_{};
    std::uint64_t sequence_{};
public:
    static constexpr std::uint64_t maxAgeNs=100'000'000;
    bool publish(const TrackedViews& v) {
        std::lock_guard guard(mutex_);
        bool valid=v.frameId>sequence_ && v.publishedNs && v.displayTime>0 &&
            v.epochs.tracking && v.epochs.referenceSpace && v.epochs.resources;
        for(unsigned i=0;i<2;++i)valid &= validPose(v.eyes[i]) && usableFov(v.fov[i]);
        if(!valid){available_=false;return false;}
        latest_=v;sequence_=v.frameId;available_=true;return true;
    }
    void invalidate() {std::lock_guard guard(mutex_);available_=false;}
    bool snapshot(std::uint64_t nowNs,TrackedViews& out) const {
        std::lock_guard guard(mutex_);
        if(!available_ || nowNs<latest_.publishedNs || nowNs-latest_.publishedNs>maxAgeNs)return false;
        out=latest_;return true;
    }
};
} // namespace rdrvr::runtime
