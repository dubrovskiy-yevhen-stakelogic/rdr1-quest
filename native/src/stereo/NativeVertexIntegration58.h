#pragma once
#include "native_vertex_stream_merge58.h"
#include <vulkan/vulkan.h>
#include <atomic>
namespace rdrvr::engine {
inline experimental::NativeVertexStreamMerge58<VkCommandBuffer,VkBuffer,VkDeviceSize,PFN_vkCmdBindVertexBuffers> nativeVertexStreamMerge58;
inline std::atomic<bool> nativeVertexInputReady58{};
}
