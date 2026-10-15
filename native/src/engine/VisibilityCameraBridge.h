#pragma once
#include "StereoVisibility.h"
#include "HeadCameraAnchor.h"
#include <atomic>
#include <mutex>
#include <string>

namespace rdrvr::engine {
// RDR's room add marker increments once in Draw before CreateSceneVisibility.
// It is a correlation token, NOT the simulation tick. A matching camera alone
// cannot identify a frame while standing still.
struct VisibilityTicket {
    std::uint64_t serial{};
    std::uintptr_t renderer{};
    std::uint32_t marker{};
    StockCamera stock{};
    StereoCameraPlan eyes{};
    StereoCullPlan cull{};
    bool stereoCull{true}; // false: stock-camera resource probe, never valid for eye replay
};
class VisibilityCameraBridge {
public:
    using Reader=bool(*)(void*,std::uintptr_t,void*,std::size_t);
private:
    runtime::TrackedViewsMailbox& views_;
    mutable std::mutex mutex_;
    CameraAdapter adapter_;
    std::uint32_t anchorActor_{};std::uint64_t anchorRevision_{};bool headAnchored_{};
    struct Slot {VisibilityTicket ticket{};bool pending{},complete{},consumed{};};
    std::array<Slot,4> slots_{};
    std::uint64_t next_{},prepared_{},completed_{},matched_{},missing_{},badCamera_{},badTracking_{},rejected_{},overwritten_{};
    std::uint64_t headPrepared_{};
    std::atomic<std::uint64_t> busy_{};
    // LOCAL->world yaw (psi) of the last prepared head-anchored plan, for the main-thread camera rig
    // (roomscale request direction, headYawWorld). Tiny seqlock over atomics: one writer (begin, under mutex_).
    std::atomic<std::uint32_t> yawSequence_{};std::atomic<float> yaw_{};std::atomic<std::uint64_t> yawNs_{},yawOrigin_{};
    void publishYaw(const StereoCameraPlan& plan,std::uint64_t ns) noexcept;
public:
    explicit VisibilityCameraBridge(runtime::TrackedViewsMailbox& v):views_(v){}
    // Read-only until a separately audited producer supplies a culling clone.
    // A ticket becomes usable only after the original visibility call returns.
    std::uint64_t begin(std::uintptr_t renderer,std::uint32_t marker,std::uintptr_t viewport,std::uintptr_t camera,
                        std::uint64_t ns,Reader,void* context,const HeadCameraAnchor* anchor=nullptr,bool requireAnchor=false);
    void finish(std::uint64_t serial,bool success) noexcept;
    bool prepared(std::uint64_t serial,std::uintptr_t renderer,VisibilityTicket& out) const;
    bool consume(std::uintptr_t renderer,std::uint32_t marker,std::uintptr_t viewport,std::uint64_t ns,
                 Reader,void* context,VisibilityTicket& out);
    std::string report() const;
    // psi = heading (atan2(x,z), radians) of LOCAL +Z mapped to world by the last head-anchored plan, i.e.
    // world = R_y(psi) * local for horizontal vectors. originSerial = TrackingOrigin::serial of that plan (0 = own latch).
    // False when none was prepared within maxAgeNs. Any thread, lock-free.
    bool worldYaw(std::uint64_t now,float& psi,std::uint64_t& originSerial,std::uint64_t maxAgeNs=250'000'000) const noexcept;
};
} // namespace rdrvr::engine
