#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace rdr95 {
// PRIVATE buffer owner only. No descriptors, native admission, queue submission,
// or source-pointer lifetime proof. Inputs must be synchronously readable under
// the caller's mapped-range witness until append returns. Raw PFNs must not
// reenter this owner. All command lifecycle notifications follow actual native
// results and the Vulkan external synchronization/non-pending requirements.
class CommandUniformArena final {
public:
    static constexpr unsigned MaxCommands=128, MaxBatchSlices=112;
    static constexpr unsigned MaxMetadataSlices=16384;
    static constexpr VkDeviceSize MaxMetadataBytes=32*1024*1024;
    static constexpr VkDeviceSize MaxArenaBytes=4*1024*1024;
    static constexpr VkDeviceSize MaxMemoryBytes=64*1024*1024;
    struct Configuration { VkDeviceSize bytesPerCommand=1024*1024; };
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
        explicit operator bool()const noexcept;
    };
    struct Source { const void* data{}; VkDeviceSize size{}; };
    struct CopySource { VkDeviceSize size{}; unsigned index{}; };
    using CopyFunction=bool(*)(void* context,unsigned index,void* destination,VkDeviceSize exactSize)noexcept;
    struct Slice {
        VkBuffer buffer{}; VkDeviceSize offset{},range{};
        bool operator==(const Slice&)const=default;
    };
    struct Batch {
        VkDevice device{}; VkCommandBuffer command{};
        std::uint64_t deviceGeneration{},allocationGeneration{},recordingGeneration{};
        unsigned count{};
        std::array<Slice,MaxBatchSlices> slices{};
    };
    // Compact receipt only. Owner must outlive every Ticket access. Immutable
    // slice metadata is owned per command until legal discard/free, not borrowed.
    class Ticket {
        friend class CommandUniformArena;
        const CommandUniformArena* owner_{};
        std::uint64_t device_{},allocation_{},recording_{},batchSerial_{};
        unsigned command_{},batch_{};
    public:
        Ticket()=default;
        bool current()const noexcept;
        // resolve is also allowed after End/Submit, for retained resource identity.
        // It does not authorize recording or prove GPU completion.
        bool resolve(Batch&)const noexcept;
        bool resolveForRecording(Batch&)const noexcept;
    };
    struct Stats {
        std::uint64_t arenas{},batches{},slices{},declines{},retirements{},flushFailures{};
        VkDeviceSize memoryBytes{},reservedBytes{},copiedBytes{},metadataBytes{};
        unsigned commands{};
        bool ready{},deviceLost{},terminal{};
    };
    CommandUniformArena()noexcept;
    ~CommandUniformArena(); // forget only; never assumes a borrowed device idle
    CommandUniformArena(const CommandUniformArena&)=delete;
    CommandUniformArena& operator=(const CommandUniformArena&)=delete;
    bool initialize(VkPhysicalDevice,VkDevice,Api,Configuration)noexcept;
    bool commandAllocated(VkDevice,VkCommandPool,VkCommandBuffer,VkCommandBufferLevel,VkResult)noexcept;
    void commandBegun(VkCommandBuffer,const VkCommandBufferBeginInfo*,VkResult)noexcept;
    void commandEnded(VkCommandBuffer,VkResult)noexcept;
    void commandSubmitted(VkCommandBuffer,VkResult)noexcept;
    void commandReset(VkCommandBuffer,VkResult)noexcept;
    void commandFreed(VkDevice,VkCommandPool,VkCommandBuffer)noexcept;
    void poolReset(VkDevice,VkCommandPool,VkResult)noexcept;
    void poolDestroyed(VkDevice,VkCommandPool)noexcept;
    // Unique append. Failed copies/flushes publish no ticket, but reserved bytes
    // remain consumed until successful native discard; there is no retry overwrite.
    bool append(VkCommandBuffer,std::span<const Source>,Ticket&)noexcept;
    // Copier executes unlocked; reentrant lifecycle invalidates this append and
    // resource destruction is deferred until it returns. It must not destroy this
    // owner or the actual device, nor retain destination. Failure clears output.
    bool appendCopied(VkCommandBuffer,std::span<const CopySource>,void*,CopyFunction,Ticket&)noexcept;
    Stats stats()const noexcept;
    bool shutdownAfterIdle(VkDevice,VkResult)noexcept;
    void abandonDevice(VkDevice)noexcept; // terminal, no driver calls
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool resolve(const Ticket&,Batch*,bool recording)const noexcept;
};
} // namespace rdr95
