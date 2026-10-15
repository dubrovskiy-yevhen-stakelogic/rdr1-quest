#include "StereoTargets.h"
namespace rdrvr::runtime {
void destroyOwnedImage(VkDevice d,OwnedImage& i) noexcept {
    if(i.view)vkDestroyImageView(d,i.view,nullptr);
    if(i.image)vkDestroyImage(d,i.image,nullptr);
    if(i.memory)vkFreeMemory(d,i.memory,nullptr);
    i={};
}
OwnedImage createOwnedImage(VkPhysicalDevice physical,VkDevice device,Extent extent,VkFormat format,
                            VkImageUsageFlags usage,VkImageAspectFlags aspect,bool view) {
    OwnedImage out{};
    try {
        VkImageCreateInfo c{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};c.imageType=VK_IMAGE_TYPE_2D;c.format=format;
        c.extent={extent.width,extent.height,1};c.mipLevels=c.arrayLayers=1;c.samples=VK_SAMPLE_COUNT_1_BIT;
        c.tiling=VK_IMAGE_TILING_OPTIMAL;c.usage=usage;c.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        checkVk(vkCreateImage(device,&c,nullptr,&out.image),"stereo image");
        VkMemoryRequirements r{};vkGetImageMemoryRequirements(device,out.image,&r);
        VkPhysicalDeviceMemoryProperties m{};vkGetPhysicalDeviceMemoryProperties(physical,&m);
        std::uint32_t type=m.memoryTypeCount;
        for(std::uint32_t i=0;i<m.memoryTypeCount;++i)if((r.memoryTypeBits&(1u<<i)) && (m.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){type=i;break;}
        if(type==m.memoryTypeCount)throw std::runtime_error("stereo image memory type");
        VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=r.size;a.memoryTypeIndex=type;
        checkVk(vkAllocateMemory(device,&a,nullptr,&out.memory),"stereo image memory");
        checkVk(vkBindImageMemory(device,out.image,out.memory,0),"stereo image bind");
        if(view){VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};v.image=out.image;v.format=format;
            v.viewType=VK_IMAGE_VIEW_TYPE_2D;v.subresourceRange={aspect,0,1,0,1};
            checkVk(vkCreateImageView(device,&v,nullptr,&out.view),"stereo image view");}
        return out;
    }catch(...){destroyOwnedImage(device,out);throw;}
}
void StereoTargets::create(VkPhysicalDevice physical,VkDevice device,Extent size,VkFormat colorFmt,VkFormat depthFmt) {
    if(device_ || !device || !physical || !size.width || !size.height || size.width>8192 || size.height>8192)
        throw std::runtime_error("invalid stereo target configuration");
    VkFormatProperties cp{},dp{};vkGetPhysicalDeviceFormatProperties(physical,colorFmt,&cp);vkGetPhysicalDeviceFormatProperties(physical,depthFmt,&dp);
    if(!(cp.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) || !(dp.optimalTilingFeatures&VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
        throw std::runtime_error("stereo attachment format unsupported");
    device_=device;extent=size;colorFormat=colorFmt;depthFormat=depthFmt;
    try {
        for(unsigned i=0;i<2;++i) {
            color[i]=createOwnedImage(physical,device,size,colorFmt,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_COLOR_BIT,true);
            const auto aspect=VK_IMAGE_ASPECT_DEPTH_BIT|((depthFmt==VK_FORMAT_D24_UNORM_S8_UINT || depthFmt==VK_FORMAT_D32_SFLOAT_S8_UINT || depthFmt==VK_FORMAT_D16_UNORM_S8_UINT)?VK_IMAGE_ASPECT_STENCIL_BIT:0);
            depth[i]=createOwnedImage(physical,device,size,depthFmt,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,aspect,true);
        }
    }catch(...){destroy();throw;}
}
void StereoTargets::destroy() noexcept {
    for(auto& i:color)destroyOwnedImage(device_,i);
    for(auto& i:depth)destroyOwnedImage(device_,i);
    device_={};extent={};colorFormat=depthFormat=VK_FORMAT_UNDEFINED;
}
} // namespace rdrvr::runtime
