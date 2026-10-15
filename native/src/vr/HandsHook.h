#pragma once
// HANDS: the only part of the hands module EngineStereoPass.cpp sees. EngineStereoPass::draw calls the hook once per
// eye on the render thread, in the engine's frame command buffer, OUTSIDE any engine render pass, after the eye's colour
// was closed and read (left: closeNativeColor + readPassSource; right: after the replayed/native colour + EYE_SOURCE) and
// BEFORE capture_.recordEye(eye) copies it into the capture slot. The hook must leave every
// image in the layout it received and restore the engine's bound command state; its result never affects the pair.
// A null hook (module killed / not configured) costs one relaxed atomic load per eye.
#include <vulkan/vulkan.h>
#include "runtime/Math.h"
#include <array>
#include <atomic>
#include <cstdint>

namespace rdrvr::vr::hands {
struct EyeTarget {
    VkImage image{};VkFormat format{};VkExtent2D extent{};VkImageLayout layout{};
    VkImageUsageFlags usage{};VkSampleCountFlagBits samples{};std::uint32_t layers{},mips{};
    std::uint64_t generation{}; // PassResources ImageRecord.generation (a recreated image with a reused handle differs)
};
// The eye plan of this frame (StereoCameraPlan fields the pass needs; LOCAL poses in metres, unitsPerMetre scale).
struct EyePlan {
    std::uint64_t sceneId{},trackingFrame{},trackingEpoch{},referenceSpace{};
    std::array<::rdrvr::Pose,2> eyes{};
    std::array<runtime::Mat4,2> projection{},world{};
    float unitsPerMetre{1},nearClip{},farClip{};
    std::uint64_t headSampledNs{}; // StereoCameraPlan::headSampledNs (the head/body anchor sample; keys the VR weapon frame)
};
struct EyeCall {
    VkDevice device{};VkCommandBuffer command{};std::uint32_t family{};unsigned eye{};
    std::uint64_t renderFrame{};   // FrameContext.renderFrameId of the pair being recorded
    std::uint64_t retiredFrame{};  // GameStereoCapture::retiredFrame(): GPU-proven (or discarded) pairs
    EyeTarget color{},depth{};     // depth.image == VK_NULL_HANDLE: the eye's depth is unknown
    const EyePlan* plan{};
};
using EyeHook=bool(*)(const EyeCall&) noexcept;
inline std::atomic<EyeHook> eyeHook{nullptr};
} // namespace rdrvr::vr::hands
