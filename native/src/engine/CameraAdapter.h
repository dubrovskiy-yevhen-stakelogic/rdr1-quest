#pragma once
#include "runtime/TrackedViews.h"
#include <span>

namespace rdrvr::engine {
// Only the fields verified in the pinned SetCameraMtx/SetProjection/Perspective
// implementations are decoded. This is NOT a guessed grcViewport C++ layout.
struct StockCamera {
    runtime::Mat4 world{},projection{};
    float nearClip{},farClip{};
};
inline constexpr std::size_t cameraReadOffset=0x40, cameraReadBytes=0x308;
bool decodeCamera(std::span<const std::byte> fields,StockCamera& out);
struct StereoCameraPlan {
    std::uint64_t sceneId{};
    runtime::TrackedViews tracking{};
    std::array<runtime::Mat4,2> world{},view{},projection{};
    float ipdMetres{},unitsPerMetre{};
    float nearClip{},farClip{}; // Exact inputs used to construct both projections.
    runtime::Mat4 baseWorld{}; // Native horizontal camera before tracked HMD composition.
    bool headAnchored{};
    std::uint32_t actorGuid{};
    std::uint64_t presentationRevision{},headSampledNs{};
    std::uintptr_t bodyCreature{},hatNode{};
    std::uint64_t originSerial{}; // TrackingOrigin::serial composed into this plan (0 = the adapter's own latch)
};
// Tracking origin owned by the main-thread camera rig (vr/VrCamera.h) and carried by the head anchor.
// position = (B.x, recenterY, B.z) in OpenXR LOCAL metres (B = body point, recenterY = eye height at recenter);
// inverseYaw = yaw-only rotation undoing the HMD heading latched at recenter. Every eye of a plan is
//   world_e = base * [R(inverseYaw) | -R(inverseYaw)*position*s] * E_e
// so ControllerAim / TrackedWeaponPose / AimTargetReticle / SpaceWarp worldFromLocal / teleport view (all derived from
// plan.world[e] and plan.tracking.eyes[e]) keep using one rigid LOCAL->world transform per plan. An origin is valid only
// in the LOCAL space it was latched in; a different referenceSpace epoch falls back to the adapter's own latch.
struct TrackingOrigin {
    runtime::Vec3 position{};
    runtime::Quat inverseYaw{0,0,0,1};
    std::uint64_t referenceSpace{},serial{};
    bool valid{};
};
enum class CameraResult { Ready, InvalidStock, InvalidTracking, InvalidScale, InvalidIpd };
class CameraAdapter {
    bool anchored_{};
    Epochs epochs_{};
    runtime::Vec3 origin_{};
    runtime::Quat inverseYaw_{0,0,0,1};
    std::uint64_t originSerial_{}; // Serial of the shared origin last composed (0 = own latch)
    std::uint64_t sharedPlans_{},ownLatches_{},staleShared_{};
public:
    void invalidate() {anchored_=false;}
    // `shared` (optional) replaces the latch below while its referenceSpace epoch matches the views. invalidate()
    // then does not move the tracking origin: only the rig re-latches it (recenter, actor change, long anchor loss).
    CameraResult prepare(const StockCamera&,const runtime::TrackedViews&,std::uint64_t sceneId,
                         float unitsPerMetre,StereoCameraPlan& out,const TrackingOrigin* shared=nullptr);
    std::uint64_t sharedPlans() const {return sharedPlans_;}
    std::uint64_t ownLatches() const {return ownLatches_;}
    std::uint64_t staleShared() const {return staleShared_;}
};
} // namespace rdrvr::engine
