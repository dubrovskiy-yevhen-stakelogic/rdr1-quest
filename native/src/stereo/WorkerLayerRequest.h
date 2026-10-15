#pragma once
#include <vulkan/vulkan.h>
#include <cstring>
#include <vector>

namespace rdr2_game_worker {
inline constexpr const char* LayerName = "VK_LAYER_RDR2_raw_worker28";

// Only the layer-name array is owned here. All original application, extension,
// allocator and pNext data stays on the original synchronous creation call.
class InstanceRequest final {
    VkInstanceCreateInfo copy_{};
    std::vector<const char*> layers_;
    const VkInstanceCreateInfo* request_{};
public:
    InstanceRequest(const VkInstanceCreateInfo* original, bool available)
        : request_(original) {
        if (!available || !original || original->enabledLayerCount > 256 ||
            (original->enabledLayerCount && !original->ppEnabledLayerNames)) return;
        for (uint32_t i=0; i<original->enabledLayerCount; ++i) {
            const auto* name=original->ppEnabledLayerNames[i];
            if (!name || std::strcmp(name, LayerName)==0) return;
        }
        layers_.reserve(static_cast<size_t>(original->enabledLayerCount)+1);
        for (uint32_t i=0; i<original->enabledLayerCount; ++i)
            layers_.push_back(original->ppEnabledLayerNames[i]);
        layers_.push_back(LayerName);
        copy_=*original;
        copy_.enabledLayerCount=static_cast<uint32_t>(layers_.size());
        copy_.ppEnabledLayerNames=layers_.data();
        request_=&copy_;
    }
    InstanceRequest(const InstanceRequest&)=delete;
    InstanceRequest& operator=(const InstanceRequest&)=delete;
    const VkInstanceCreateInfo* get() const noexcept { return request_; }
};
}
