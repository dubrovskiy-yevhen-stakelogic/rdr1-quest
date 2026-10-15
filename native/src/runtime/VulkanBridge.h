#pragma once
#include "XrPlatform.h"
namespace rdrvr::runtime {
class VulkanBootstrap;
// One owner. Retail adapter must negotiate BEFORE creating its instance/device.
// Standalone owns its objects. Borrowing requires an already negotiated bootstrap;
// the game retains ownership of instance/device and only the private queue is used.
class VulkanBridge {
    bool owned_{};
public:
    VulkanBridge()=default;
    VulkanBridge(const VulkanBridge&)=delete;
    VulkanBridge& operator=(const VulkanBridge&)=delete;
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    std::uint32_t queueFamily{}, queueIndex{};
    void create(XrInstance xr, XrSystemId system);
    void borrow(const VulkanBootstrap& negotiated);
    void waitIdle() noexcept;
    void destroy() noexcept;
    XrGraphicsBindingVulkan2KHR binding() const;
};
} // namespace rdrvr::runtime
