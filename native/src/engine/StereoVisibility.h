#pragma once
#include "CameraAdapter.h"

namespace rdrvr::engine {
// One conservative convex volume enclosing BOTH finite eye frusta. This is for
// generating a shared visibility list, never for rendering either eye. It does
// not make mono occlusion queries, LOD decisions or temporal history reusable.
struct StereoCullPlan {
    std::uint64_t sceneId{};
    runtime::TrackedViews tracking{};
    StockCamera camera{};
    float originBackshift{}; // Culling-only; never applied to either rendered eye.
};
bool prepareStereoCull(const StereoCameraPlan&,float nearClip,float farClip,StereoCullPlan&) noexcept;
} // namespace rdrvr::engine
