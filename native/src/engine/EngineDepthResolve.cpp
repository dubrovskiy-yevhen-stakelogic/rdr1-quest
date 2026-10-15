// Diagnostics: copy() reports which check rejected and the tracked layouts it saw
// (TargetRouteGuard.h, gameBootstrap.targetBoundary.depthResolve). The diagnostics do not change
// the checks, their order or the result.
#include "diagnostics/FrameTimings.h"
#include "EngineDepthResolve.h"
#include "TargetRouteGuard.h"
#include <cerrno>
#include <limits>
namespace rdrvr::engine {
namespace {
struct Errno {int value{errno};~Errno(){errno=value;}};
template<class T>bool read(const PassSourceAbi& a,const void* object,std::size_t offset,T& value){
    const auto p=reinterpret_cast<std::uintptr_t>(object);
    return p && !(p%8) && offset<=UINTPTR_MAX-p && sizeof(T)-1<=UINTPTR_MAX-(p+offset) && a.read(a.context,p+offset,&value,sizeof(value));
}
bool sourceLayout(VkImageLayout layout){return layout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
    layout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL || layout==VK_IMAGE_LAYOUT_GENERAL;}
}
bool EngineDepthResolve::command(void* context,VkCommandBuffer expected,CommandRecord& record) const {
    std::uintptr_t table{};void *pass{},*actual{},*desired{},*wrapper{};VkCommandBuffer cmd{};
    return expected && read(api_.memory,context,0,table) && table==api_.memory.contextVtable &&
        read(api_.memory,context,0x30a8,pass) && !pass && read(api_.memory,context,0x30b0,actual) && !actual &&
        read(api_.memory,context,0x30b8,desired) && !desired && read(api_.memory,context,0x30a0,wrapper) &&
        read(api_.memory,wrapper,0x38,cmd) && cmd==expected && commands_.recording(device_,cmd,family_,record);
}
bool EngineDepthResolve::image(unsigned eye,EngineTargetRole role,TargetImage& value) const {
    const auto* known=targets_.image(eye,role);
    return known && readTargetImage(images_,device_,api_.memory,reinterpret_cast<std::uintptr_t>(targets_.target(eye,role)),value) &&
        known->image.generation==value.image.generation && known->texture==value.texture && known->image.image==value.image.image;
}
bool EngineDepthResolve::copy(unsigned eye,void* context,VkCommandBuffer expected) noexcept {
    diagnostics::WallScope timing(diagnostics::frameTimings.depthResolve);
    Errno preserve;bool entered=false;
    // The rejecting check and the engine-tracked layouts (texture +0xa0) seen so far (-1: not read yet).
    int srcLayout=-1,dstLayout=-1;
    const auto reject=[&](const char* why) noexcept {targetroute::noteDepthReject(why,srcLayout,dstLayout);return false;};
    try {
        if(!api_)return reject("API");
        if(poisoned_)return reject("POISONED");
        if(owner_!=std::this_thread::get_id() || !api_.ownsThread())return reject("THREAD");
        if(eye>=2)return reject("EYE");
        if(!targets_.sceneReady() || !targets_.pending())return reject("TARGETS_NOT_PENDING");
        CommandRecord before{};TargetImage src{},dst{};
        if(!command(context,expected,before))return reject("COMMAND_STATE");
        if(!image(eye,EngineTargetRole::Depth,src))return reject("SRC_IMAGE");
        srcLayout=int(src.layout);
        if(!image(eye,EngineTargetRole::SampledDepth,dst))return reject("DST_IMAGE");
        dstLayout=int(dst.layout);
        if(src.image.image==dst.image.image || src.texture==dst.texture)return reject("SAME_IMAGE");
        if(src.image.format!=dst.image.format ||
           src.image.extent.width!=dst.image.extent.width || src.image.extent.height!=dst.image.extent.height)return reject("FORMAT_OR_EXTENT");
        if(!(src.image.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || !(dst.image.usage&VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
           !(dst.image.usage&VK_IMAGE_USAGE_SAMPLED_BIT))return reject("USAGE");
        if(!sourceLayout(src.layout))return reject("SRC_LAYOUT");
        // Any other destination layout (e.g. 3 = DEPTH_STENCIL_ATTACHMENT_OPTIMAL) is rejected
        if(dst.layout!=VK_IMAGE_LAYOUT_UNDEFINED && dst.layout!=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)return reject("DST_LAYOUT");
        VkImageCopy region{};region.srcSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};region.dstSubresource=region.srcSubresource;
        region.extent=src.image.extent;
        entered=true;api_.copy(reinterpret_cast<void*>(dst.texture),expected,reinterpret_cast<void*>(src.texture),&region);
        CommandRecord after{};TargetImage finalSrc{},finalDst{};
        if(!command(context,expected,after) || before.generation!=after.generation || before.recording!=after.recording ||
           !image(eye,EngineTargetRole::Depth,finalSrc) || !image(eye,EngineTargetRole::SampledDepth,finalDst) ||
           finalSrc.layout!=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL || finalDst.layout!=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL){
            srcLayout=int(finalSrc.layout);dstLayout=int(finalDst.layout); // 0 when not read
            poisoned_=true;return reject("POST_CHECK");
        }
        return true;
    }catch(...){if(entered)poisoned_=true;return reject("EXCEPTION");}
}
}
