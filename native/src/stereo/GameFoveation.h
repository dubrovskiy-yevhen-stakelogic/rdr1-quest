#pragma once
// FOVEATION (GRAPHICS menu row, graphics.cfg foveation=0..5; debug.rdrvr.foveation=<0..4> live override,
// debug.rdrvr.foveation.autofloor=<0..4>, default 1) for the eye scene passes, with two mutually exclusive
// device backends chosen at vkCreateDevice:
//   VRS  VK_KHR_fragment_shading_rate (GameFragmentRate path; radial 8x8-texel rate maps per eye and level),
//   FDM  VK_EXT_fragment_density_map (this module: density-map attachment on colour+depth passes, R8G8 band maps),
//   NONE neither (passes exactly as the engine made them).
// graphics.cfg foveationApi=0/1/2 (menu row FOVEATION API, applies after restart), debug.rdrvr.foveation.api=
// vrs|fdm|none overrides it. The level applies live: begin-time framebuffer substitution (FoveationRegistry.h).
// foveation=4 ULTRA (VRS radii .30/.45, vertical .82; FDM = HIGH bands), 5 AUTO (FoveationPlans.h
// AutoController on the GpuLite.h span), STEREO TEST 17 FOVEATION SWEEP (OFF..ULTRA, 64 world frames each). The level
// is decided at the end of a world frame and applied at the start of the next one (gameFoveationNextLevel).
#include "FoveationRegistry.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace rdrvr::engine {
class EngineEyeTargets;
struct StereoCameraPlan;
// Begin-hook gate: false = one relaxed load per vkCmdBeginRenderPass and nothing else.
inline std::atomic<bool> foveationArmed{};
foveation::Registry& foveationRegistry() noexcept;
// [recording threads] true: begin with out (eye framebuffer replaced by the variant of the current level).
// level (optional) = the level of the foveation map the eye pass begins with, -1 = not a foveated eye pass.
bool foveationBegin(const VkRenderPassBeginInfo& in,VkRenderPassBeginInfo& out,int* level=nullptr) noexcept;
// [stereo replay, lock-free] left-eye foveation view -> right-eye view of the same level.
VkImageView foveationRightView(VkImageView left) noexcept;
// ---- device creation (GameXr::device, under its mutex)
foveation::Api gameFoveationApiRequest() noexcept;
void gameFoveationQuery(VkPhysicalDevice) noexcept; // FDM support for the report and the plan
bool gameFoveationDensityPlan(const VkDeviceCreateInfo&,foveation::DensityDevicePlan&) noexcept;
void gameFoveationDensityRejected(VkResult) noexcept; // the FDM request failed; the device was created without it
// active: the backend really enabled on this device (None: original passes). Raw framebuffer functions are the
// driver's (variants bypass the engine hooks); the stereo registration keeps variants resolvable by the replay.
void gameFoveationDeviceCreated(foveation::Api active,VkPhysicalDevice,VkDevice,std::uint32_t family,std::uint32_t queueIndex,std::mutex* queueMutex) noexcept;
// ---- creation hooks (TraceEntry)
VkResult gameFoveationCreatePass(PFN_vkCreateRenderPass original,VkDevice,const VkRenderPassCreateInfo*,const VkAllocationCallbacks*,VkRenderPass*) noexcept;
void gameFoveationPassObserved(VkDevice,const VkRenderPassCreateInfo*,VkRenderPass,VkResult) noexcept; // after the VRS bridge
void gameFoveationPassDestroyed(VkDevice,VkRenderPass) noexcept;
VkResult gameFoveationCreateFramebuffer(PFN_vkCreateFramebuffer original,VkDevice,const VkFramebufferCreateInfo*,const VkAllocationCallbacks*,VkFramebuffer*) noexcept;
void gameFoveationFramebufferCreated(VkDevice,const VkFramebufferCreateInfo* finalInfo,VkFramebuffer,VkResult) noexcept;
void gameFoveationFramebufferDestroyed(VkDevice,VkFramebuffer) noexcept;
void gameFoveationViewCreated(VkDevice,const VkImageViewCreateInfo*,VkImageView,VkResult) noexcept;
void gameFoveationViewDestroyed(VkDevice,VkImageView) noexcept;
// ---- world frame (render thread): level poll, eye registration, level maps. retiredFrame: the GPU retirement
// watermark of the stereo capture (renderFrameId units, = plan.sceneId) for evicting least recently used variants.
void gameFoveationFrame(const EngineEyeTargets&,const StereoCameraPlan&,std::mutex& queueMutex,std::uint64_t retiredFrame) noexcept;
// Caller stopped the XR worker; waits for device idle, destroys variants and maps.
void gameFoveationDestroy(VkDevice) noexcept;
std::string gameFoveationReport();
// [render thread, frame end]: the level the NEXT world frame will record with (decided in gameFoveationFrame,
// applied one frame later), for the byState signature; -1 = no foveation API active or no world frame yet.
int gameFoveationNextLevel() noexcept;
// True when that level was chosen by FOVEATION AUTO (signature suffix "a").
bool gameFoveationAutoActive() noexcept;
}
