#pragma once
#include "FragmentRateMap.h"
#include <vulkan/vulkan.h>
#include <cstring>
#include <thread>

namespace rdrvr::runtime {
struct FragmentRateImageApi {
    PFN_vkGetPhysicalDeviceFormatProperties formatProperties{};
    PFN_vkGetPhysicalDeviceImageFormatProperties imageProperties{};
    PFN_vkGetPhysicalDeviceMemoryProperties memoryProperties{};
    PFN_vkCreateImage createImage{};PFN_vkDestroyImage destroyImage{};
    PFN_vkGetImageMemoryRequirements memoryRequirements{};
    PFN_vkAllocateMemory allocateMemory{};PFN_vkFreeMemory freeMemory{};
    PFN_vkBindImageMemory bindMemory{};
    PFN_vkMapMemory mapMemory{};PFN_vkUnmapMemory unmapMemory{};
    PFN_vkGetImageSubresourceLayout subresourceLayout{};
    PFN_vkCreateImageView createView{};PFN_vkDestroyImageView destroyView{};
    explicit operator bool() const {return formatProperties&&imageProperties&&memoryProperties&&createImage&&destroyImage&&
        memoryRequirements&&allocateMemory&&freeMemory&&bindMemory&&mapMemory&&unmapMemory&&subresourceLayout&&createView&&destroyView;}
};
// Immutable host-filled R8_UINT map. No queue operations or implicit waits.
// The caller records initialization, proves its completion and only then may
// expose the view to framebuffers. Retirement requires all referencing native
// framebuffers and commands to be gone, with their GPU work completed.
class FragmentRateImage {
    FragmentRateImageApi api_{};VkDevice device_{};
    VkImage image_{};VkDeviceMemory memory_{};VkImageView view_{};
    std::thread::id owner_{};unsigned layers_{};
    std::uint64_t initialization_{},lastInitialization_{};
    bool ready_{},published_{};
    VkResult result_{VK_SUCCESS};const char* status_{"EMPTY"};
    bool owner()const{return owner_==std::this_thread::get_id();}
    void release() noexcept {
        if(view_)api_.destroyView(device_,view_,nullptr);
        if(image_)api_.destroyImage(device_,image_,nullptr);
        if(memory_)api_.freeMemory(device_,memory_,nullptr);
        view_={};image_={};memory_={};device_={};layers_=0;ready_=false;
    }
public:
    FragmentRateImage()=default;
    FragmentRateImage(const FragmentRateImage&)=delete;
    FragmentRateImage& operator=(const FragmentRateImage&)=delete;
    ~FragmentRateImage(){reset();} // Unknown GPU use is quarantined, not freed.
    bool create(FragmentRateImageApi api,VkPhysicalDevice physical,VkDevice device,const FragmentRateMap& map) noexcept {
        if(device_){status_="ALREADY_CREATED";return false;}
        const std::uint64_t bytes=std::uint64_t(map.width)*map.height*map.layers;
        if(!api||!physical||!device||!map.width||!map.height||map.width>2048||map.height>2048||
           !map.layers||map.layers>2||bytes!=map.pixels.size()||bytes>8388608){status_="INVALID_INPUT";return false;}
        for(auto rate:map.pixels)if(rate!=0&&rate!=5&&rate!=10){status_="INVALID_RATE";return false;}
        VkFormatProperties format{};api.formatProperties(physical,VK_FORMAT_R8_UINT,&format);
        if(!(format.linearTilingFeatures&VK_FORMAT_FEATURE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR)){status_="LINEAR_RATE_UNSUPPORTED";return false;}
        VkImageFormatProperties properties{};
        result_=api.imageProperties(physical,VK_FORMAT_R8_UINT,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_LINEAR,VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,0,&properties);
        if(result_!=VK_SUCCESS||properties.maxExtent.width<map.width||properties.maxExtent.height<map.height||properties.maxExtent.depth<1||
           properties.maxArrayLayers<map.layers||!properties.maxMipLevels||!(properties.sampleCounts&VK_SAMPLE_COUNT_1_BIT)){status_="IMAGE_PROPERTIES";return false;}
        api_=api;device_=device;owner_=std::this_thread::get_id();layers_=map.layers;
        auto fail=[&](const char* reason){status_=reason;release();return false;};
        VkImageCreateInfo image{};image.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;image.imageType=VK_IMAGE_TYPE_2D;image.format=VK_FORMAT_R8_UINT;
        image.extent={map.width,map.height,1};image.mipLevels=1;image.arrayLayers=map.layers;image.samples=VK_SAMPLE_COUNT_1_BIT;
        image.tiling=VK_IMAGE_TILING_LINEAR;image.usage=VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
        image.sharingMode=VK_SHARING_MODE_EXCLUSIVE;image.initialLayout=VK_IMAGE_LAYOUT_PREINITIALIZED;
        VkImage created{};result_=api.createImage(device,&image,nullptr,&created);
        if(result_!=VK_SUCCESS||!created)return fail("CREATE_IMAGE");image_=created;
        VkMemoryRequirements requirements{};api.memoryRequirements(device,image_,&requirements);
        VkPhysicalDeviceMemoryProperties memory{};api.memoryProperties(physical,&memory);
        if(!requirements.size||requirements.size>67108864||memory.memoryTypeCount>VK_MAX_MEMORY_TYPES)return fail("MEMORY_PROPERTIES");
        unsigned type=memory.memoryTypeCount;
        constexpr auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for(unsigned i=0;i<memory.memoryTypeCount;++i)if((requirements.memoryTypeBits&(1u<<i))&&(memory.memoryTypes[i].propertyFlags&flags)==flags){type=i;break;}
        if(type==memory.memoryTypeCount)return fail("COHERENT_MEMORY_UNAVAILABLE");
        VkMemoryAllocateInfo allocate{};allocate.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;allocate.allocationSize=requirements.size;allocate.memoryTypeIndex=type;
        VkDeviceMemory allocated{};result_=api.allocateMemory(device,&allocate,nullptr,&allocated);
        if(result_!=VK_SUCCESS||!allocated)return fail("ALLOCATE_MEMORY");memory_=allocated;
        result_=api.bindMemory(device,image_,memory_,0);if(result_!=VK_SUCCESS)return fail("BIND_MEMORY");
        std::array<VkSubresourceLayout,2> layouts{};
        for(unsigned eye=0;eye<map.layers;++eye){VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT,0,eye};auto& l=layouts[eye];api.subresourceLayout(device,image_,&sub,&l);
            if(l.rowPitch<map.width||l.rowPitch>requirements.size||l.offset>requirements.size||
               std::uint64_t(map.height-1)>(requirements.size-l.offset)/l.rowPitch)return fail("INVALID_PITCH");
            const auto end=l.offset+std::uint64_t(map.height-1)*l.rowPitch;
            if(map.width>requirements.size-end)return fail("INVALID_PITCH");
            if(eye){const auto& previous=layouts[0];const auto previousEnd=previous.offset+std::uint64_t(map.height-1)*previous.rowPitch+map.width;
                if(l.offset<previousEnd)return fail("OVERLAPPING_LAYERS");}
        }
        void* raw{};result_=api.mapMemory(device,memory_,0,requirements.size,0,&raw);
        if(result_!=VK_SUCCESS||!raw)return fail("MAP_MEMORY");
        for(unsigned eye=0;eye<map.layers;++eye)for(unsigned row=0;row<map.height;++row)
            std::memcpy(static_cast<unsigned char*>(raw)+layouts[eye].offset+row*layouts[eye].rowPitch,
                map.pixels.data()+(std::size_t(eye)*map.height+row)*map.width,map.width);
        api.unmapMemory(device,memory_);
        VkImageViewCreateInfo view{};view.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;view.image=image_;view.viewType=map.layers==1?VK_IMAGE_VIEW_TYPE_2D:VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        view.format=VK_FORMAT_R8_UINT;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,map.layers};
        VkImageView made{};result_=api.createView(device,&view,nullptr,&made);
        if(result_!=VK_SUCCESS||!made)return fail("CREATE_VIEW");view_=made;status_="HOST_FILLED";return true;
    }
    bool beginInitialization(std::uint64_t serial,VkImageMemoryBarrier& barrier) noexcept {
        if(!device_||!owner()||ready_||initialization_||!serial||serial<=lastInitialization_)return false;
        VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;
        b.dstAccessMask=VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;b.oldLayout=VK_IMAGE_LAYOUT_PREINITIALIZED;
        b.newLayout=VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.image=image_;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,layers_};barrier=b;
        initialization_=lastInitialization_=serial;status_="INITIALIZATION_PENDING";return true;
    }
    // False means the caller discarded the command before any successful submit.
    // True requires fence/queue completion of the recorded transition.
    bool finishInitialization(std::uint64_t serial,bool completed) noexcept {
        if(!device_||!owner()||!initialization_||serial!=initialization_)return false;
        initialization_=0;ready_=completed;status_=completed?"READY":"HOST_FILLED";return true;
    }
    VkImageView publishView() noexcept {if(!ready_||!owner())return VK_NULL_HANDLE;published_=true;return view_;}
    bool retire() noexcept {if(!device_||!owner()||initialization_)return false;published_=false;return true;}
    bool reset() noexcept {if(!device_)return true;if(!owner()||initialization_||published_)return false;release();status_="EMPTY";return true;}
    // Lifecycle owner only: all referencing framebuffers/commands are retired
    // and vkDeviceWaitIdle has completed (or device loss ended GPU execution).
    // This explicit shutdown route handles maps created by different threads.
    bool destroyAfterDeviceIdle(VkDevice device) noexcept {
        if(!device_||device!=device_)return false;
        initialization_=0;published_=false;release();status_="DEVICE_RETIRED";return true;
    }
    bool ready()const noexcept{return ready_;}
    VkResult lastVkResult()const noexcept{return result_;}
    const char* status()const noexcept{return status_;}
};
}
