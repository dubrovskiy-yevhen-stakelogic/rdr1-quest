#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <string>
namespace rdrvr::engine {
PFN_vkVoidFunction imageProc(PFN_vkVoidFunction,const char*,PFN_vkCreateImage,PFN_vkDestroyImage) noexcept;
struct ImageRecord {
    VkImage image{};VkDevice device{};VkExtent3D extent{};VkFormat format{};
    VkImageUsageFlags usage{};VkSampleCountFlagBits samples{};
    std::uint32_t layers{},mips{};std::uint64_t generation{};
};
// Metadata only. Never modifies VkImageCreateInfo or owns game images. A bounded
// registry records only unextended, ordinary color/depth attachments on the game device.
class PassResources {
    mutable std::mutex mutex_;
    VkDevice device_{};
    std::unordered_map<VkImage,ImageRecord> images_;
    std::uint64_t generation_{},created_{},destroyed_{},rejected_{},misses_{};
public:
    static constexpr std::size_t capacity=4096;
    bool bind(VkDevice);
    void created(VkDevice,const VkImageCreateInfo*,VkImage,VkResult) noexcept;
    void destroyed(VkDevice,VkImage) noexcept;
    bool lookup(VkDevice,VkImage,ImageRecord&);
    void clear(VkDevice) noexcept;
    std::string report() const;
};
struct PassSource {
    ImageRecord image{};
    VkImageLayout layout{};
    VkCommandBuffer command{};
    std::uintptr_t renderTarget{},texture{};
};
struct PassSourceAbi {
    std::uintptr_t targetVtable{},textureVtable{},contextVtable{};
    using Reader=bool(*)(void*,std::uintptr_t,void*,std::size_t);
    Reader read{};void* context{};
};
struct TargetImage {ImageRecord image{};VkImageLayout layout{};std::uintptr_t target{},texture{};};
// Object/image identity only: creation may still have UNDEFINED layout. Does not
// authorize a copy, acquire ownership, or require a recording command buffer.
bool readTargetImage(PassResources&,VkDevice,const PassSourceAbi&,std::uintptr_t,TargetImage&,const char** status=nullptr);
// Exact pinned ARM64 layout, only after the owner ended its active render pass.
// Reader validates every memory access. No virtual call, dereference, layout
// mutation, EndRenderPass or queue operation is performed here. The render owner
// must retain the target/context through capture; lookup is not an engine AddRef.
bool readPassSource(PassResources&,VkDevice,const PassSourceAbi&,
                    std::uintptr_t target,std::uintptr_t context,PassSource&);
}
