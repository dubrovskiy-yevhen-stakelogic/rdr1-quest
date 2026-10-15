#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <string>
namespace rdrvr::engine {
struct CommandRecord {
    VkCommandBuffer command{};VkCommandPool pool{};VkDevice device{};
    std::uint32_t family{};std::uint64_t generation{},recording{};
};
// Observes successful Vulkan operations without modifying them. The callback is
// noexcept, must not reenter this registry, and runs before a tracked recording
// becomes unusable. Vulkan's pool/buffer external synchronization remains with
// the engine; this mutex protects metadata, not the underlying Vulkan objects.
class CommandResources {
    struct Pool {std::uint32_t family;};
    enum class State {Initial,Recording,Executable,Invalid};
    struct Buffer {CommandRecord record;VkCommandBufferLevel level;State state;std::thread::id owner;};
    mutable std::mutex mutex_;
    VkDevice device_{};
    std::unordered_map<VkCommandPool,Pool> pools_;
    std::unordered_map<VkCommandBuffer,Buffer> buffers_;
    std::uint64_t generation_{},recording_{},misses_{},rejected_{},invalidated_{};
    void (*invalidated)(VkCommandBuffer) noexcept{};
    void invalidate(Buffer&) noexcept;
    void erasePool(VkCommandPool) noexcept;
public:
    static constexpr std::size_t poolCapacity=256,bufferCapacity=4096;
    bool bind(VkDevice,void(*)(VkCommandBuffer) noexcept);
    void poolCreated(VkDevice,const VkCommandPoolCreateInfo*,VkCommandPool,VkResult) noexcept;
    void poolDestroyed(VkDevice,VkCommandPool) noexcept;
    void allocated(VkDevice,const VkCommandBufferAllocateInfo*,const VkCommandBuffer*,VkResult) noexcept;
    void freed(VkDevice,VkCommandPool,std::uint32_t,const VkCommandBuffer*) noexcept;
    void begun(VkCommandBuffer,const VkCommandBufferBeginInfo*,VkResult) noexcept;
    void ended(VkCommandBuffer,VkResult) noexcept;
    void reset(VkCommandBuffer,VkResult) noexcept;
    void poolReset(VkDevice,VkCommandPool,VkResult) noexcept;
    bool recording(VkDevice,VkCommandBuffer,std::uint32_t expectedFamily,CommandRecord&) noexcept;
    void clear(VkDevice) noexcept;
    std::string report() const;
};
}
