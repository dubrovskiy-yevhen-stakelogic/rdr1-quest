#include "PerfOverlay.h"
#include <algorithm>
#include <cstring>
namespace rdrvr::runtime {
void PerfOverlay::initialize(XrSession session,const XrGraphicsBindingVulkan2KHR& b){
    if(device_)throw std::runtime_error("performance overlay already initialized");device_=b.device;
    vkGetDeviceQueue(device_,b.queueFamilyIndex,b.queueIndex,&queue_);
    std::uint32_t n{};checkXr(xrEnumerateSwapchainFormats(session,0,&n,nullptr),"stats formats");
    if(!n||n>256)throw std::runtime_error("stats format count");std::vector<std::int64_t> formats(n);
    checkXr(xrEnumerateSwapchainFormats(session,n,&n,formats.data()),"stats formats");
    if(std::find(formats.begin(),formats.end(),VK_FORMAT_R8G8B8A8_SRGB)==formats.end())throw std::runtime_error("stats RGBA unavailable");
    XrSwapchainCreateInfo c{XR_TYPE_SWAPCHAIN_CREATE_INFO};c.format=VK_FORMAT_R8G8B8A8_SRGB;
    c.usageFlags=XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT|XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;c.width=diagnostics::PerfPanel::width;c.height=diagnostics::PerfPanel::height;
    c.sampleCount=c.faceCount=c.arraySize=c.mipCount=1;checkXr(xrCreateSwapchain(session,&c,&swap_),"stats swapchain");
    checkXr(xrEnumerateSwapchainImages(swap_,0,&n,nullptr),"stats image count");if(!n||n>16)throw std::runtime_error("stats image count invalid");
    images_.resize(n,{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});checkXr(xrEnumerateSwapchainImages(swap_,n,&n,reinterpret_cast<XrSwapchainImageBaseHeader*>(images_.data())),"stats images");
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};buffer.size=sizeof(diagnostics::PerfPanel::pixels);buffer.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    checkVk(vkCreateBuffer(device_,&buffer,nullptr,&buffer_),"stats staging");
    VkMemoryRequirements r{};vkGetBufferMemoryRequirements(device_,buffer_,&r);VkPhysicalDeviceMemoryProperties m{};vkGetPhysicalDeviceMemoryProperties(b.physicalDevice,&m);
    unsigned type=m.memoryTypeCount;const auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for(unsigned i=0;i<m.memoryTypeCount;++i)if((r.memoryTypeBits&(1u<<i))&&(m.memoryTypes[i].propertyFlags&flags)==flags){type=i;break;}
    if(type==m.memoryTypeCount)throw std::runtime_error("stats coherent staging unavailable");
    VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=r.size;a.memoryTypeIndex=type;
    checkVk(vkAllocateMemory(device_,&a,nullptr,&memory_),"stats allocate");checkVk(vkBindBufferMemory(device_,buffer_,memory_,0),"stats bind");
    checkVk(vkMapMemory(device_,memory_,0,buffer.size,0,&mapped_),"stats map");
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool.queueFamilyIndex=b.queueFamilyIndex;pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    checkVk(vkCreateCommandPool(device_,&pool,nullptr,&pool_),"stats pool");
    VkCommandBufferAllocateInfo cmd{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cmd.commandPool=pool_;cmd.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cmd.commandBufferCount=1;
    checkVk(vkAllocateCommandBuffers(device_,&cmd,&command_),"stats command");
    VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};checkVk(vkCreateFence(device_,&f,nullptr,&fence_),"stats fence");
}
bool PerfOverlay::pump(std::uint64_t now,const diagnostics::PerfPanel* update,XrSpace space,XrCompositionLayerQuad& out){
    if(!swap_)return false;
    if(submitted_){const auto result=vkGetFenceStatus(device_,fence_);if(result!=VK_NOT_READY){checkVk(result,"stats completion");
        XrSwapchainImageReleaseInfo r{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};checkXr(xrReleaseSwapchainImage(swap_,&r),"stats release");submitted_=acquired_=false;valid_=true;}}
    if(update&&due(now)){
        std::memcpy(mapped_,update->pixels.data(),sizeof(update->pixels));
        XrSwapchainImageAcquireInfo a{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};checkXr(xrAcquireSwapchainImage(swap_,&a,&index_),"stats acquire");
        acquired_=true;nextUpdate_=now+500'000'000ull;if(index_>=images_.size())throw std::runtime_error("stats index");
    }
    if(acquired_&&!submitted_){
        XrSwapchainImageWaitInfo w{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};w.timeout=0;const auto waited=xrWaitSwapchainImage(swap_,&w);
        if(waited!=XR_TIMEOUT_EXPIRED){checkXr(waited,"stats wait");
            checkVk(vkResetCommandBuffer(command_,0),"stats reset");VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            checkVk(vkBeginCommandBuffer(command_,&begin),"stats begin");
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=images_[index_].image;b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdPipelineBarrier(command_,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
            VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={diagnostics::PerfPanel::width,diagnostics::PerfPanel::height,1};
            vkCmdCopyBufferToImage(command_,buffer_,b.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
            b.oldLayout=b.newLayout;b.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            vkCmdPipelineBarrier(command_,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,nullptr,0,nullptr,1,&b);
            checkVk(vkEndCommandBuffer(command_),"stats end");checkVk(vkResetFences(device_,1,&fence_),"stats fence reset");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command_;
            checkVk(vkQueueSubmit(queue_,1,&submit,fence_),"stats submit");submitted_=true;
        }
    }
    if(!valid_)return false;
    out={XR_TYPE_COMPOSITION_LAYER_QUAD};out.space=space;out.eyeVisibility=XR_EYE_VISIBILITY_BOTH;out.pose.orientation.w=1;
    out.pose.position={-0.30f,0.24f,-1.3f};out.size={0.70f,0.70f*diagnostics::PerfPanel::height/diagnostics::PerfPanel::width};out.subImage.swapchain=swap_;
    out.subImage.imageRect.extent={int(diagnostics::PerfPanel::width),int(diagnostics::PerfPanel::height)};return true;
}
void PerfOverlay::destroy() noexcept{
    if(!device_)return;
    if(mapped_)vkUnmapMemory(device_,memory_);if(buffer_)vkDestroyBuffer(device_,buffer_,nullptr);if(memory_)vkFreeMemory(device_,memory_,nullptr);
    if(pool_)vkDestroyCommandPool(device_,pool_,nullptr);if(fence_)vkDestroyFence(device_,fence_,nullptr);if(swap_)xrDestroySwapchain(swap_);
    device_={};swap_={};mapped_=nullptr;buffer_={};memory_={};pool_={};command_={};fence_={};images_.clear();valid_=acquired_=submitted_=false;
}
}
