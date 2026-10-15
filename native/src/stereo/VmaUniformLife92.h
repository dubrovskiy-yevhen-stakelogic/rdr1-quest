#pragma once
#include "engine/DynamicUniformDescriptors.h"
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>

namespace rdr92 {
// Called only from pinned early VMA PLT hooks. No Vulkan dispatch-table inference.
class VmaUniformLife final {
public:
    using Create=VkResult(*)(void*,const VkBufferCreateInfo*,const void*,VkBuffer*,void**,void*);
    using Aligned=VkResult(*)(void*,const VkBufferCreateInfo*,const void*,VkDeviceSize,VkBuffer*,void**,void*);
    using Destroy=void(*)(void*,VkBuffer,void*);
    struct AllocatorInfo {VkInstance instance{};VkPhysicalDevice physical{};VkDevice device{};};
    using Info=void(*)(void*,AllocatorInfo*);
    static_assert(sizeof(AllocatorInfo)==24);
private:
    rdrvr::engine::DynamicUniformDescriptors& owner_;
    Create create_{};Aligned aligned_{};Destroy destroy_{};Info info_{};
    std::uintptr_t constructorReturn_{};
    std::atomic<bool> installed_{};
    std::atomic<VkDevice> device_{};
    std::atomic<std::uint64_t> qualifiedCreates_{},retirements_{};
    VkDevice device(void* allocator)const noexcept{
        if(!allocator||!info_)return {};
        AllocatorInfo value;info_(allocator,&value);return value.device;
    }
    bool owning(VkDevice d)const noexcept{return d&&d==device_.load(std::memory_order_acquire)&&installed_.load(std::memory_order_acquire);}
public:
    explicit VmaUniformLife(rdrvr::engine::DynamicUniformDescriptors& owner)noexcept:owner_(owner){}
    // Configure while the native dispatch slots are still zero and before InitVulkan.
    bool configure(Create create,Aligned aligned,Destroy destroy,Info info,std::uintptr_t constructorReturn)noexcept{
        if(installed_.load()||create_||!create||!aligned||!destroy||!info||!constructorReturn)return false;
        create_=create;aligned_=aligned;destroy_=destroy;info_=info;constructorReturn_=constructorReturn;return true;
    }
    void installed(bool success)noexcept{installed_.store(success,std::memory_order_release);}
    bool installed()const noexcept{return installed_.load(std::memory_order_acquire);}
    bool bindDevice(VkDevice d)noexcept{
        if(!installed()||!d||device_.load(std::memory_order_acquire))return false;
        if(!owner_.enablePersistentBuffers(d))return false;
        device_.store(d,std::memory_order_release);return true;
    }
    void suspendDevice()noexcept{
        const auto d=device_.exchange({},std::memory_order_acq_rel);if(d)owner_.disablePersistentBuffers(d);
    }
    VkResult create(void* allocator,const VkBufferCreateInfo* ci,const void* allocationCI,VkBuffer* buffer,void** allocation,void* allocationInfo)noexcept{
        if(!device_.load(std::memory_order_acquire)||!installed())return create_(allocator,ci,allocationCI,buffer,allocation,allocationInfo);
        const auto result=create_(allocator,ci,allocationCI,buffer,allocation,allocationInfo);const int saved=errno;
        // This generic entry is not the certified native uniform constructor.
        // A missed/reused known handle revokes its old incarnation, never adopts it.
        if(result==VK_SUCCESS&&buffer&&*buffer){const auto d=device(allocator);if(owning(d))owner_.bufferDestroyed(d,*buffer);}
        errno=saved;return result;
    }
    VkResult aligned(void* allocator,const VkBufferCreateInfo* ci,const void* allocationCI,VkDeviceSize alignment,
                     VkBuffer* buffer,void** allocation,void* allocationInfo,std::uintptr_t caller)noexcept{
        if(!device_.load(std::memory_order_acquire)||!installed())return aligned_(allocator,ci,allocationCI,alignment,buffer,allocation,allocationInfo);
        VkBufferCreateInfo observed{};if(ci)std::memcpy(&observed,ci,sizeof observed);
        const bool candidate=caller==constructorReturn_&&ci&&allocationCI&&buffer&&allocation&&!allocationInfo&&
            reinterpret_cast<std::uintptr_t>(allocation)==reinterpret_cast<std::uintptr_t>(buffer)+sizeof(VkBuffer)&&
            !*buffer&&!*allocation&&
            alignment==16&&observed.sType==VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO&&!observed.pNext&&!observed.flags&&
            observed.size&&observed.size<=UINT32_MAX&&observed.usage==VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT&&
            observed.sharingMode==VK_SHARING_MODE_EXCLUSIVE&&!observed.queueFamilyIndexCount&&!observed.pQueueFamilyIndices;
        const auto result=aligned_(allocator,ci,allocationCI,alignment,buffer,allocation,allocationInfo);const int saved=errno;
        if(result==VK_SUCCESS&&buffer&&*buffer){const auto d=device(allocator);if(owning(d)){
            if(candidate&&*allocation){owner_.bufferCreated(d,*buffer,observed.size,observed.usage);++qualifiedCreates_;}
            else owner_.bufferDestroyed(d,*buffer);
        }}
        errno=saved;return result;
    }
    void destroy(void* allocator,VkBuffer buffer,void* allocation)noexcept{
        const int incoming=errno;
        if(buffer){const auto d=device(allocator);if(owning(d)){owner_.bufferDestroyed(d,buffer);++retirements_;}}
        errno=incoming;destroy_(allocator,buffer,allocation);
    }
    std::uint64_t qualifiedCreates()const noexcept{return qualifiedCreates_.load(std::memory_order_relaxed);}
    std::uint64_t retirements()const noexcept{return retirements_.load(std::memory_order_relaxed);}
};
}
