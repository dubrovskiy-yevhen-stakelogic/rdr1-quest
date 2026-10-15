#pragma once
#include "runtime/FragmentRateBridge.h"
#include "runtime/FragmentRateMap.h"
#include "EngineTargets.h"
#include "CameraAdapter.h"
#include <array>
#include <string>
namespace rdrvr::engine {
// [eye][level] views of every FOVEATION level (runtime::fragmentRateLevels: OFF, LOW, MEDIUM, HIGH, ULTRA).
using FragmentRateLevelViews=std::array<std::array<VkImageView,runtime::fragmentRateLevelCount>,2>;
runtime::FragmentRateBridge& gameFragmentRateBridge() noexcept;
// Only called after complete pinned hooks are armed, before native device
// creation and before any session/worker can use the newly created device.
bool gameFragmentRateDevicePlan(VkInstance,VkPhysicalDevice,const VkDeviceCreateInfo&,runtime::FragmentRateDevicePlan&) noexcept;
bool gameFragmentRateInitialize(VkPhysicalDevice,VkDevice,std::uint32_t family,std::uint32_t privateQueueIndex) noexcept;
// Acquires the private XR queue mutex ONLY for a new map upload, never for
// steady-state lookups. Runs before framebuffers acquire inspected eye images.
void gameFragmentRateEyes(const EngineEyeTargets&,const StereoCameraPlan&,std::mutex& privateQueueMutex) noexcept;
// [eye][level] rate-map views for eye images of width x height (level 0 = the full-rate map, 1..4 = the
// LOW/MEDIUM/HIGH/ULTRA radial maps prepared by gameFragmentRateEyes). False until levels 0..3 exist for both eyes of
// that extent (a missing ULTRA map is replaced by the HIGH one and counted).
bool gameFragmentRateLevelViews(unsigned width,unsigned height,FragmentRateLevelViews& out) noexcept;
// The four-level form (levels 0..3 only), kept for older callers.
bool gameFragmentRateLevelViews(unsigned width,unsigned height,std::array<std::array<VkImageView,4>,2>& out) noexcept;
// Caller stopped the XR worker, retires native references and is destroying d.
void gameFragmentRateDestroy(VkDevice) noexcept;
std::string gameFragmentRateReport();
// lowfreq: capability snapshot for per-pipeline coarse rates (cold: pipeline creation; takes the module mutex).
runtime::FragmentRateCapabilities gameFragmentRateCapabilities() noexcept;
}
