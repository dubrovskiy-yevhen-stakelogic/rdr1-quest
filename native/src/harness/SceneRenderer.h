#pragma once
#include "runtime/VulkanBridge.h"
#include "runtime/Math.h"
#include <vector>
namespace rdrvr::harness {
struct Target { VkImageView view{}; VkFramebuffer framebuffer{}; };
class SceneRenderer {
    runtime::VulkanBridge* gpu_{};
    VkRenderPass pass_{};
    VkPipelineLayout layout_{};
    VkPipeline pipeline_{};
    VkCommandPool pool_{};
    VkCommandBuffer command_{};
    VkFence fence_{};
public:
    void initialize(runtime::VulkanBridge& gpu, VkFormat format);
    void createTarget(VkImage image, VkFormat format, Extent extent, Target& target);
    void destroyTarget(Target& target) noexcept;
    void draw(const Target& target, Extent extent, unsigned eye, const Pose& pose, Fov fov,
              const std::array<Pose,2>& hands);
    void destroy() noexcept;
};
} // namespace rdrvr::harness
