#pragma once
#include "XrPlatform.h"
#include "contracts/Contracts.h"
#include <array>

namespace rdrvr::runtime {
struct OwnedImage {VkImage image{};VkDeviceMemory memory{};VkImageView view{};};
// Separate color AND depth storage for each eye. Caller owns submission and
// must complete GPU work before destroy/recreate; no retail object is owned here.
class StereoTargets {
    VkDevice device_{};
public:
    std::array<OwnedImage,2> color{},depth{};
    Extent extent{};
    VkFormat colorFormat{},depthFormat{};
    StereoTargets()=default;
    StereoTargets(const StereoTargets&)=delete;
    StereoTargets& operator=(const StereoTargets&)=delete;
    void create(VkPhysicalDevice,VkDevice,Extent,VkFormat,VkFormat);
    void destroy() noexcept;
};
OwnedImage createOwnedImage(VkPhysicalDevice,VkDevice,Extent,VkFormat,VkImageUsageFlags,VkImageAspectFlags,bool view);
void destroyOwnedImage(VkDevice,OwnedImage&) noexcept;
} // namespace rdrvr::runtime
