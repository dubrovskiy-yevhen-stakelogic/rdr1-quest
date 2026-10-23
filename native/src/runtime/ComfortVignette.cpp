#include "ComfortVignette.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
namespace rdrvr::runtime {
void ComfortVignette::initialize(XrSession session,const XrGraphicsBindingVulkan2KHR& b){
    if(device_)throw std::runtime_error("vignette already initialized");device_=b.device;
    vkGetDeviceQueue(device_,b.queueFamilyIndex,b.queueIndex,&queue_);
    std::uint32_t n{};checkXr(xrEnumerateSwapchainFormats(session,0,&n,nullptr),"vignette formats");
    if(!n||n>256)throw std::runtime_error("vignette format count");std::vector<std::int64_t> formats(n);
    checkXr(xrEnumerateSwapchainFormats(session,n,&n,formats.data()),"vignette formats");
    if(std::find(formats.begin(),formats.end(),VK_FORMAT_R8G8B8A8_SRGB)==formats.end())throw std::runtime_error("vignette RGBA unavailable");
    XrSwapchainCreateInfo c{XR_TYPE_SWAPCHAIN_CREATE_INFO};c.format=VK_FORMAT_R8G8B8A8_SRGB; // Alpha is linear in an sRGB format; the colour is black
    c.usageFlags=XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT|XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;c.width=c.height=size;
    c.sampleCount=c.faceCount=c.arraySize=c.mipCount=1;checkXr(xrCreateSwapchain(session,&c,&swap_),"vignette swapchain");
    checkXr(xrEnumerateSwapchainImages(swap_,0,&n,nullptr),"vignette image count");if(!n||n>16)throw std::runtime_error("vignette image count invalid");
    images_.resize(n,{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});checkXr(xrEnumerateSwapchainImages(swap_,n,&n,reinterpret_cast<XrSwapchainImageBaseHeader*>(images_.data())),"vignette images");
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};buffer.size=VkDeviceSize(size)*size*4u;buffer.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    checkVk(vkCreateBuffer(device_,&buffer,nullptr,&buffer_),"vignette staging");
    VkMemoryRequirements r{};vkGetBufferMemoryRequirements(device_,buffer_,&r);VkPhysicalDeviceMemoryProperties m{};vkGetPhysicalDeviceMemoryProperties(b.physicalDevice,&m);
    unsigned type=m.memoryTypeCount;const auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for(unsigned i=0;i<m.memoryTypeCount;++i)if((r.memoryTypeBits&(1u<<i))&&(m.memoryTypes[i].propertyFlags&flags)==flags){type=i;break;}
    if(type==m.memoryTypeCount)throw std::runtime_error("vignette coherent staging unavailable");
    VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=r.size;a.memoryTypeIndex=type;
    checkVk(vkAllocateMemory(device_,&a,nullptr,&memory_),"vignette allocate");checkVk(vkBindBufferMemory(device_,buffer_,memory_,0),"vignette bind");
    void* mapped{};checkVk(vkMapMemory(device_,memory_,0,buffer.size,0,&mapped),"vignette map");
    // The ring: premultiplied black (0, 0, 0, a), a = smoothstep(innerU, outerU, radius) - clear centre, opaque out to the quad's edge.
    auto* texels=static_cast<std::uint32_t*>(mapped);
    for(std::uint32_t y=0;y<size;++y)for(std::uint32_t x=0;x<size;++x){
        const float u=(float(x)+.5f)/float(size)*2.f-1.f,v=(float(y)+.5f)/float(size)*2.f-1.f;
        const float t=std::clamp((std::sqrt(u*u+v*v)-innerU)/(outerU-innerU),0.f,1.f);
        texels[y*size+x]=std::uint32_t(std::lround(t*t*(3.f-2.f*t)*255.f))<<24;
    }
    vkUnmapMemory(device_,memory_);
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool.queueFamilyIndex=b.queueFamilyIndex;pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    checkVk(vkCreateCommandPool(device_,&pool,nullptr,&pool_),"vignette pool");
    VkCommandBufferAllocateInfo cmd{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cmd.commandPool=pool_;cmd.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cmd.commandBufferCount=1;
    checkVk(vkAllocateCommandBuffers(device_,&cmd,&command_),"vignette command");
    VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};checkVk(vkCreateFence(device_,&f,nullptr,&fence_),"vignette fence");
}
void ComfortVignette::releaseStaging() noexcept{
    if(buffer_)vkDestroyBuffer(device_,buffer_,nullptr);if(memory_)vkFreeMemory(device_,memory_,nullptr);buffer_={};memory_={};
}
bool ComfortVignette::layer(XrSpace space,float clearTan,XrCompositionLayerQuad& out){
    if(!swap_||!space)return false;
    if(submitted_){const auto result=vkGetFenceStatus(device_,fence_);if(result!=VK_NOT_READY){checkVk(result,"vignette completion");
        XrSwapchainImageReleaseInfo r{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};checkXr(xrReleaseSwapchainImage(swap_,&r),"vignette release");
        submitted_=acquired_=false;valid_=true;releaseStaging();}} // The released image stays the layer's content: never acquired again
    if(!valid_&&!acquired_){
        XrSwapchainImageAcquireInfo a{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};checkXr(xrAcquireSwapchainImage(swap_,&a,&index_),"vignette acquire");
        acquired_=true;if(index_>=images_.size())throw std::runtime_error("vignette index");
    }
    if(acquired_&&!submitted_){
        XrSwapchainImageWaitInfo w{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};w.timeout=0;const auto waited=xrWaitSwapchainImage(swap_,&w);
        if(waited!=XR_TIMEOUT_EXPIRED){checkXr(waited,"vignette wait");
            checkVk(vkResetCommandBuffer(command_,0),"vignette reset");VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            checkVk(vkBeginCommandBuffer(command_,&begin),"vignette begin");
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=images_[index_].image;b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdPipelineBarrier(command_,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
            VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={size,size,1};
            vkCmdCopyBufferToImage(command_,buffer_,b.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
            b.oldLayout=b.newLayout;b.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            vkCmdPipelineBarrier(command_,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,nullptr,0,nullptr,1,&b);
            checkVk(vkEndCommandBuffer(command_),"vignette end");checkVk(vkResetFences(device_,1,&fence_),"vignette fence reset");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command_;
            checkVk(vkQueueSubmit(queue_,1,&submit,fence_),"vignette submit");submitted_=true;
        }
    }
    if(!valid_||!(clearTan>0.f)||!std::isfinite(clearTan))return false;
    // Quad half-size (tan space) = clearTan / innerU, at `distance`; at the smallest aperture (HIGH, 25 deg) the edge is still ~67 deg out.
    const float half=clearTan/innerU*distance;
    out={XR_TYPE_COMPOSITION_LAYER_QUAD};out.space=space;out.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
    out.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; // Premultiplied: no UNPREMULTIPLIED bit
    out.pose.orientation.w=1;out.pose.position={0,0,-distance};out.size={2.f*half,2.f*half};out.subImage.swapchain=swap_;
    out.subImage.imageRect.extent={int(size),int(size)};return true;
}
void ComfortVignette::destroy() noexcept{
    if(!device_)return;
    releaseStaging();
    if(pool_)vkDestroyCommandPool(device_,pool_,nullptr);if(fence_)vkDestroyFence(device_,fence_,nullptr);if(swap_)xrDestroySwapchain(swap_);
    device_={};swap_={};pool_={};command_={};fence_={};images_.clear();valid_=acquired_=submitted_=false;
}
}
