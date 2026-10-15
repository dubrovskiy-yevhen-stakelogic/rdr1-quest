#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include "CameraMetadataActivity.h"

namespace rdr95 {
// PRIVATE: actual owned Vulkan resources, but no shader/pipeline/bind authority.
// Caller supplies the complete Vulkan command lifecycle with the usual external
// synchronization/non-pending rules. No method submits or waits on a queue.
class CameraUniformOwner final {
public:
    static constexpr unsigned DescriptorSet=3, BindingIndex=0, PayloadBytes=128;
    static constexpr VkShaderStageFlags ShaderStages=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT;
    static constexpr unsigned MaxCommands=128, SlotsPerCommand=16;
    static constexpr VkDeviceSize MaxMemoryBytes=16*1024*1024;
    using Bytes=std::array<std::byte,PayloadBytes>;
    struct PassKey {
        std::uint64_t pass{},pose{};
        std::uint32_t eye{};
        bool operator==(const PassKey&)const=default;
    };
    struct Api {
        PFN_vkGetPhysicalDeviceProperties getProperties{};
        PFN_vkGetPhysicalDeviceMemoryProperties getMemoryProperties{};
        PFN_vkCreateBuffer createBuffer{};
        PFN_vkGetBufferMemoryRequirements getBufferRequirements{};
        PFN_vkAllocateMemory allocateMemory{};
        PFN_vkBindBufferMemory bindMemory{};
        PFN_vkMapMemory mapMemory{};
        PFN_vkFlushMappedMemoryRanges flushMemory{};
        PFN_vkUnmapMemory unmapMemory{};
        PFN_vkDestroyBuffer destroyBuffer{};
        PFN_vkFreeMemory freeMemory{};
        PFN_vkCreateDescriptorPool createPool{};
        PFN_vkDestroyDescriptorPool destroyPool{};
        PFN_vkAllocateDescriptorSets allocateSets{};
        PFN_vkUpdateDescriptorSets updateSets{};
        PFN_vkCreateDescriptorSetLayout createLayout{};
        PFN_vkDestroyDescriptorSetLayout destroyLayout{};
        explicit operator bool()const noexcept;
    };
    struct Layout {
        VkDevice device{};
        VkDescriptorSetLayout handle{};
        std::uint64_t generation{};
    };
    struct Binding {
        VkDevice device{}; VkCommandBuffer command{};
        VkDescriptorSet descriptor{}; VkDescriptorSetLayout layout{};
        VkBuffer buffer{}; VkDeviceSize offset{},range{};
        std::uint64_t deviceGeneration{},allocationGeneration{},recordingGeneration{};
        PassKey key{};
    };
    // Issued only by a full locked recording resolve. Owner must outlive every
    // use. Semantic invalidation only: no GPU lease or native lifetime grant.
    // Any owner mutation permanently expires a saved observation, including
    // failed lifecycle events and prepare hits. Foreign-thread use declines.
    class RecordingObservation final {
        friend class CameraUniformOwner;
        const CameraUniformOwner* owner_{};
        std::uint64_t stamp_{};
        std::thread::id thread_{};
    public:
        RecordingObservation()=default;
        bool current()const noexcept;
    };
    class Ticket {
        friend class CameraUniformOwner;
        const CameraUniformOwner* owner_{};
        std::uint64_t device_{},allocation_{},recording_{};
        unsigned command_{},slot_{};
    public:
        Ticket()=default;
        // Resource identity may remain current after End and repeated Submit.
        // resolveForRecording additionally requires the exact recording thread.
        bool current()const noexcept;
        bool resolveForRecording(Binding&)const noexcept;
        bool observeForRecording(Binding&,RecordingObservation&)const noexcept;
    };
    struct Stats {
        std::uint64_t arenas{},payloads{},hits{},declines{},retirements{};
        VkDeviceSize memoryBytes{};
        unsigned commands{};
        bool ready{},deviceLost{};
    };

    CameraUniformOwner()noexcept;
    ~CameraUniformOwner(); // forgets only; does NOT assume a borrowed device idle
    CameraUniformOwner(const CameraUniformOwner&)=delete;
    CameraUniformOwner& operator=(const CameraUniformOwner&)=delete;
    bool initialize(VkPhysicalDevice,VkDevice,Api)noexcept;
    bool layout(Layout&)const noexcept;
    bool layoutCurrent(const Layout&)const noexcept;
    bool commandAllocated(VkDevice,VkCommandPool,VkCommandBuffer,
                          VkCommandBufferLevel,VkResult)noexcept;
    void commandBegun(VkCommandBuffer,const VkCommandBufferBeginInfo*,VkResult)noexcept;
    void commandEnded(VkCommandBuffer,VkResult)noexcept;
    void commandSubmitted(VkCommandBuffer,VkResult)noexcept; // deliberately retains
    void commandReset(VkCommandBuffer,VkResult)noexcept;
    void commandFreed(VkDevice,VkCommandPool,VkCommandBuffer)noexcept;
    void poolReset(VkDevice,VkCommandPool,VkResult)noexcept;
    void poolDestroyed(VkDevice,VkCommandPool)noexcept;
    bool prepare(VkCommandBuffer,PassKey,const Bytes&,Ticket&)noexcept;
    Stats stats()const noexcept;

    // Owner and all Vk resources must outlive tickets and recorded uses. After
    // actual idle (or device loss) and stopped submissions, destroy resources.
    // An idle failure is NOT permission; caller may explicitly abandon only
    // immediately before actual borrowed-device destruction. Abandon calls no Vk.
    bool shutdownAfterIdle(VkDevice,VkResult idleResult)noexcept;
    void abandonDevice(VkDevice)noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    CameraMetadataActivity activity_;
    bool resolve(const Ticket&,Binding*,bool recording)const noexcept;
    bool observe(const Ticket&,Binding&,RecordingObservation&)const noexcept;
};
} // namespace rdr95
