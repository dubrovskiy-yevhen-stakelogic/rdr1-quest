#pragma once
#include "XrPlatform.h"
#include "diagnostics/PerfPanel.h"
#include <vector>
namespace rdrvr::runtime {
// XR worker owns every operation. Persistent staging updates at 2 Hz, with
// zero-timeout image waits and fence polling; never stalls rendering for stats.
class PerfOverlay {
    VkDevice device_{};VkQueue queue_{};XrSwapchain swap_{};VkBuffer buffer_{};VkDeviceMemory memory_{};void* mapped_{};
    VkCommandPool pool_{};VkCommandBuffer command_{};VkFence fence_{};
    std::vector<XrSwapchainImageVulkan2KHR> images_;std::uint32_t index_{};
    bool acquired_{},submitted_{},valid_{};std::uint64_t nextUpdate_{};
public:
    void initialize(XrSession,const XrGraphicsBindingVulkan2KHR&);
    bool due(std::uint64_t now)const{return swap_&&!acquired_&&!submitted_&&now>=nextUpdate_;}
    void requestUpdate() noexcept {nextUpdate_=0;}
    bool pump(std::uint64_t now,const diagnostics::PerfPanel* update,XrSpace,XrCompositionLayerQuad&);
    // Only after the owning queue/device is idle, including teardown failures.
    void destroy() noexcept;
};
}
