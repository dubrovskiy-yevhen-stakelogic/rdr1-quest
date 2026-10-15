#pragma once
#include "CameraMainRoute.h"
#include "engine/EyeViewports.h"
#include <cstring>

namespace rdr95 {
// Capture owned viewport values after the existing selected-eye boundary.
// This does NOT prove that the mutable native CPU constant buffer still equals
// these values; only the rewritten GPU path consumes this independent copy.
class CameraViewportSource final {
    inline static std::atomic<std::uint64_t> captures_{};
public:
    static bool capture(const rdrvr::engine::EyeViewports& views,unsigned eye,
                        std::uint64_t expectedScene,std::uint64_t expectedPose,
                        CameraMainRoute::Stage stage,CameraMainRoute::Description& out) noexcept {
        if(!expectedScene||!expectedPose||eye>=2||
           (stage!=CameraMainRoute::Stage::Depth&&stage!=CameraMainRoute::Stage::Color)||
           !views.ready()||!views.matches(eye))return false;
        const auto& plan=views.plan();
        if(plan.sceneId!=expectedScene||plan.tracking.frameId!=expectedPose)return false;
        const auto* bytes=static_cast<const std::byte*>(views.viewport(eye));
        if(!bytes)return false;
        CameraMainRoute::Description value;
        value.scene=expectedScene;value.pose=expectedPose;value.eye=eye;value.stage=stage;
        // Pinned owned clone offsets, already populated by native camera and
        // projection setters. Exact ColMajor payload; no recomputation.
        std::memcpy(value.camera.data(),bytes+0xc0,64);
        std::memcpy(value.camera.data()+64,bytes+0x140,64);
        auto serial=captures_.load(std::memory_order_relaxed);
        while(serial!=std::numeric_limits<std::uint64_t>::max()){
            if(captures_.compare_exchange_weak(serial,serial+1,std::memory_order_relaxed)){
                // This tags this owned source snapshot, not the lifetime of a
                // native pointer. Every capture is new even if an address or
                // scene/pose identifier was reused. No borrow escapes.
                value.viewGeneration=serial+1;out=value;return true;
            }
        }
        return false;
    }
};
} // namespace rdr95
