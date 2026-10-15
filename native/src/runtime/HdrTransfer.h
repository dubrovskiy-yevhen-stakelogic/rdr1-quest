#pragma once
#include "StereoTargets.h"
#include "StereoReticle.h"
#include <vector>

namespace rdrvr::runtime {
// Owns only the private XR transfer pass, never game pipelines or targets.
// Caller must finish recorded GPU work before reconfiguration/destruction.
class HdrTransfer {
    struct Target {VkImageView view{};VkFramebuffer framebuffer{};};
    VkDevice device_{};Extent extent_{};bool encode_{},overlay_{};
    VkImageView source_{};VkSampler sampler_{};
    VkDescriptorSetLayout descriptorLayout_{};VkDescriptorPool pool_{};VkDescriptorSet descriptor_{};
    VkRenderPass pass_{};VkPipelineLayout layout_{};VkPipeline pipeline_{};
    std::vector<Target> targets_;
    void createPass(VkDevice,VkImage,Extent,VkFormat source,VkFormat destination,const std::vector<XrSwapchainImageVulkan2KHR>&,bool overlay);
public:
    HdrTransfer()=default;
    HdrTransfer(const HdrTransfer&)=delete;
    HdrTransfer& operator=(const HdrTransfer&)=delete;
    // Deliberately no implicit GPU destruction: a failed wait must quarantine
    // handles, including during owner unwinding. Explicit destroy follows a fence.
    void create(VkDevice,VkImage source,Extent,VkFormat destination,const std::vector<XrSwapchainImageVulkan2KHR>&);
    // Private command buffer only, outside native engine recording/state caches.
    // Source is premultiplied RGBA/BGRA in GENERAL; destination is loaded in
    // COLOR_ATTACHMENT_OPTIMAL. Both formats must use the same color encoding.
    // Keeps native/theater pixels beneath the transparent UI unchanged.
    void createOverlay(VkDevice,VkImage source,Extent,VkFormat sourceFormat,VkFormat destination,const std::vector<XrSwapchainImageVulkan2KHR>&);
    void draw(VkCommandBuffer,unsigned target) const;
    void draw(VkCommandBuffer,unsigned target,const ScreenReticle&) const;
    void destroy() noexcept;
};
}
