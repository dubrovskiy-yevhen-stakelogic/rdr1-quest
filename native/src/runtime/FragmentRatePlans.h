#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <cstring>

namespace rdrvr::runtime {
// Creation plans only. All input pointers must already be validated/readable.
// No retail hooks, handle ownership, implicit GPU waits or automatic enablement.
class FragmentRateDevicePlan {
    std::array<const char*,130> extensions_{};
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR feature_{};
    VkDeviceCreateInfo info_{};bool valid_{};
public:
    FragmentRateDevicePlan()=default;
    FragmentRateDevicePlan(const FragmentRateDevicePlan&)=delete;
    FragmentRateDevicePlan& operator=(const FragmentRateDevicePlan&)=delete;
    // Retains original nested pointers through the immediate vkCreateDevice.
    bool prepare(const VkDeviceCreateInfo& native,bool rateExtension,bool renderPass2Extension,
                 const VkPhysicalDeviceFragmentShadingRateFeaturesKHR& support) noexcept {
        valid_=false;
        if(native.sType!=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO || native.flags || !rateExtension || !renderPass2Extension ||
           !support.pipelineFragmentShadingRate || !support.attachmentFragmentShadingRate || native.enabledExtensionCount>128 ||
           (native.enabledExtensionCount&&!native.ppEnabledExtensionNames))return false;
        unsigned count=0;bool rate=false,pass2=false;
        for(unsigned i=0;i<native.enabledExtensionCount;++i){
            const char* name=native.ppEnabledExtensionNames[i];if(!name)return false;
            unsigned length=0;while(length<256&&name[length])++length;if(!length||length==256)return false;
            extensions_[count++]=name;rate|=!std::strcmp(name,VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);pass2|=!std::strcmp(name,VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
        }
        if(!rate)extensions_[count++]=VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME;
        if(!pass2)extensions_[count++]=VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME;
        std::array<const VkBaseInStructure*,32> visited{};unsigned visitedCount=0,found=0;
        for(auto* p=static_cast<const VkBaseInStructure*>(native.pNext);p;p=p->pNext){
            if(visitedCount==visited.size())return false;
            for(unsigned i=0;i<visitedCount;++i)if(visited[i]==p)return false;
            visited[visitedCount++]=p;
            if(p->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR){
                if(++found>1)return false;
                const auto* f=reinterpret_cast<const VkPhysicalDeviceFragmentShadingRateFeaturesKHR*>(p);
                if(!f->pipelineFragmentShadingRate||!f->attachmentFragmentShadingRate)return false;
            }
            // Vulkan forbids enabling these together with KHR fragment rates.
            if(p->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT && reinterpret_cast<const VkPhysicalDeviceFragmentDensityMapFeaturesEXT*>(p)->fragmentDensityMap)return false;
            if(p->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADING_RATE_IMAGE_FEATURES_NV && reinterpret_cast<const VkPhysicalDeviceShadingRateImageFeaturesNV*>(p)->shadingRateImage)return false;
        }
        info_=native;info_.enabledExtensionCount=count;info_.ppEnabledExtensionNames=extensions_.data();
        if(!found){feature_={};feature_.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR;feature_.pNext=const_cast<void*>(native.pNext);feature_.pipelineFragmentShadingRate=VK_TRUE;feature_.attachmentFragmentShadingRate=VK_TRUE;info_.pNext=&feature_;}
        valid_=true;return true;
    }
    const VkDeviceCreateInfo* info()const noexcept{return valid_?&info_:nullptr;}
};

class FragmentRatePassPlan {
    std::array<VkAttachmentDescription2,10> attachments_{};
    std::array<VkAttachmentReference2,8> colors_{};
    VkAttachmentReference2 depth_{},rateReference_{};
    std::array<VkSubpassDependency2,8> dependencies_{};
    VkFragmentShadingRateAttachmentInfoKHR rate_{};
    VkSubpassDescription2 subpass_{};
    VkRenderPassCreateInfo2 info_{};
    std::uint32_t correlation_{},nativeCount_{};bool valid_{};
    static VkImageAspectFlags depthAspect(VkFormat f){
        switch(f){case VK_FORMAT_D16_UNORM:case VK_FORMAT_X8_D24_UNORM_PACK32:case VK_FORMAT_D32_SFLOAT:return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_S8_UINT:return VK_IMAGE_ASPECT_STENCIL_BIT;
        case VK_FORMAT_D16_UNORM_S8_UINT:case VK_FORMAT_D24_UNORM_S8_UINT:case VK_FORMAT_D32_SFLOAT_S8_UINT:return VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
        default:return 0;}
    }
public:
    FragmentRatePassPlan()=default;
    FragmentRatePassPlan(const FragmentRatePassPlan&)=delete;
    FragmentRatePassPlan& operator=(const FragmentRatePassPlan&)=delete;
    // Supports an ordinary native v1 pass and the multiview v1 shape.
    // Physical texel/rate/format support is checked by the device owner first.
    bool prepare(const VkRenderPassCreateInfo& native,VkExtent2D texel,bool enabled) noexcept {
        valid_=false;nativeCount_=0;correlation_=0;
        auto power=[](unsigned n){return n&&!(n&(n-1))&&n<=256;};
        if(!enabled || !power(texel.width)||!power(texel.height) || native.sType!=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO || native.flags ||
           !native.attachmentCount || native.attachmentCount>9 || !native.pAttachments || native.subpassCount!=1 || !native.pSubpasses ||
           native.dependencyCount>8 || (native.dependencyCount&&!native.pDependencies))return false;
        const auto& s=native.pSubpasses[0];
        if(s.flags || s.pipelineBindPoint!=VK_PIPELINE_BIND_POINT_GRAPHICS || s.inputAttachmentCount || s.preserveAttachmentCount || s.pResolveAttachments ||
           !s.colorAttachmentCount || s.colorAttachmentCount>8 || !s.pColorAttachments)return false;
        if(native.pNext){
            const auto* mv=static_cast<const VkRenderPassMultiviewCreateInfo*>(native.pNext);
            if(mv->sType!=VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO||mv->pNext||mv->subpassCount!=1||!mv->pViewMasks||mv->pViewMasks[0]!=3||
               mv->correlationMaskCount!=1||!mv->pCorrelationMasks||mv->pCorrelationMasks[0]!=3||
               (mv->dependencyCount&&mv->dependencyCount!=native.dependencyCount)||(mv->dependencyCount&&!mv->pViewOffsets))return false;
            for(unsigned i=0;i<mv->dependencyCount;++i)if(mv->pViewOffsets[i])return false;
            correlation_=3;
        }
        for(unsigned i=0;i<native.attachmentCount;++i){const auto& a=native.pAttachments[i];
            if(a.flags || a.samples!=VK_SAMPLE_COUNT_1_BIT || a.format==VK_FORMAT_UNDEFINED)return false;
            attachments_[i]={};auto& d=attachments_[i];d.sType=VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;d.format=a.format;d.samples=a.samples;
            d.loadOp=a.loadOp;d.storeOp=a.storeOp;d.stencilLoadOp=a.stencilLoadOp;d.stencilStoreOp=a.stencilStoreOp;d.initialLayout=a.initialLayout;d.finalLayout=a.finalLayout;
        }
        for(unsigned i=0;i<s.colorAttachmentCount;++i){const auto& r=s.pColorAttachments[i];if(r.attachment!=VK_ATTACHMENT_UNUSED&&r.attachment>=native.attachmentCount)return false;
            colors_[i]={};colors_[i].sType=VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;colors_[i].attachment=r.attachment;colors_[i].layout=r.layout;colors_[i].aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;}
        if(s.pDepthStencilAttachment){const auto& r=*s.pDepthStencilAttachment;if(r.attachment!=VK_ATTACHMENT_UNUSED&&r.attachment>=native.attachmentCount)return false;
            const auto aspect=r.attachment==VK_ATTACHMENT_UNUSED?0:depthAspect(native.pAttachments[r.attachment].format);
            if(r.attachment!=VK_ATTACHMENT_UNUSED&&!aspect)return false;
            depth_={};depth_.sType=VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;depth_.attachment=r.attachment;depth_.layout=r.layout;depth_.aspectMask=aspect;
        }
        for(unsigned i=0;i<native.dependencyCount;++i){const auto& d=native.pDependencies[i];
            if(!((d.srcSubpass==0&&d.dstSubpass==VK_SUBPASS_EXTERNAL)||(d.srcSubpass==VK_SUBPASS_EXTERNAL&&d.dstSubpass==0)) ||
               (d.dependencyFlags&~VK_DEPENDENCY_BY_REGION_BIT))return false;
            auto& v=dependencies_[i];v={};v.sType=VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;v.srcSubpass=d.srcSubpass;v.dstSubpass=d.dstSubpass;v.srcStageMask=d.srcStageMask;v.dstStageMask=d.dstStageMask;v.srcAccessMask=d.srcAccessMask;v.dstAccessMask=d.dstAccessMask;v.dependencyFlags=d.dependencyFlags;
        }
        auto& a=attachments_[native.attachmentCount];a={};a.sType=VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;a.format=VK_FORMAT_R8_UINT;a.samples=VK_SAMPLE_COUNT_1_BIT;
        a.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;a.storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;a.initialLayout=a.finalLayout=VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
        rateReference_={};rateReference_.sType=VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;rateReference_.attachment=native.attachmentCount;rateReference_.layout=a.initialLayout;rateReference_.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
        rate_={};rate_.sType=VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR;rate_.pFragmentShadingRateAttachment=&rateReference_;rate_.shadingRateAttachmentTexelSize=texel;
        subpass_={};subpass_.sType=VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;subpass_.pNext=&rate_;subpass_.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;subpass_.viewMask=correlation_;subpass_.colorAttachmentCount=s.colorAttachmentCount;subpass_.pColorAttachments=colors_.data();subpass_.pDepthStencilAttachment=s.pDepthStencilAttachment?&depth_:nullptr;
        info_={};info_.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;info_.attachmentCount=native.attachmentCount+1;info_.pAttachments=attachments_.data();info_.subpassCount=1;info_.pSubpasses=&subpass_;info_.dependencyCount=native.dependencyCount;info_.pDependencies=native.dependencyCount?dependencies_.data():nullptr;
        info_.correlatedViewMaskCount=correlation_?1:0;info_.pCorrelatedViewMasks=correlation_?&correlation_:nullptr;
        nativeCount_=native.attachmentCount;valid_=true;return true;
    }
    const VkRenderPassCreateInfo2* info()const noexcept{return valid_?&info_:nullptr;}
    unsigned nativeAttachmentCount()const noexcept{return valid_?nativeCount_:0;}
};

class FragmentRateFramebufferPlan {
    std::array<VkImageView,10> views_{};VkFramebufferCreateInfo info_{};bool valid_{};
public:
    FragmentRateFramebufferPlan()=default;
    FragmentRateFramebufferPlan(const FragmentRateFramebufferPlan&)=delete;
    FragmentRateFramebufferPlan& operator=(const FragmentRateFramebufferPlan&)=delete;
    bool prepare(const VkFramebufferCreateInfo& native,unsigned expectedNativeCount,VkImageView rateView) noexcept {
        valid_=false;
        if(native.sType!=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO||native.pNext||native.flags||!native.renderPass||!rateView||!expectedNativeCount||expectedNativeCount>9||
           native.attachmentCount!=expectedNativeCount||!native.pAttachments||!native.width||!native.height||native.width>16384||native.height>16384||native.layers!=1)return false;
        for(unsigned i=0;i<native.attachmentCount;++i){if(!native.pAttachments[i])return false;views_[i]=native.pAttachments[i];}
        views_[native.attachmentCount]=rateView;info_=native;info_.attachmentCount=native.attachmentCount+1;info_.pAttachments=views_.data();valid_=true;return true;
    }
    const VkFramebufferCreateInfo* info()const noexcept{return valid_?&info_:nullptr;}
};

class FragmentRatePipelinePlan {
    VkGraphicsPipelineCreateInfo info_{};VkPipelineFragmentShadingRateStateCreateInfoKHR rate_{};bool valid_{};
public:
    FragmentRatePipelinePlan()=default;
    FragmentRatePipelinePlan(const FragmentRatePipelinePlan&)=delete;
    FragmentRatePipelinePlan& operator=(const FragmentRatePipelinePlan&)=delete;
    // Immediate-call plan: ALL native nested descriptions remain borrowed and
    // unchanged, including depth-clip/extended-dynamic-state feature structures.
    // Caller proves a rate-compatible pass/device and shader feature support.
    // lowfreq: coarse 2|4 = pipeline rate coarse x coarse combined MAX with the attachment (final = MAX(KEEP(pipeline,
    // primitive), attachment): the foveation map still coarsens further where it is coarser, never finer). The caller proves
    // fragmentShadingRateNonTrivialCombinerOps and the listed square size. Anything else = the native 1x1 KEEP/REPLACE plan.
    bool prepare(const VkGraphicsPipelineCreateInfo& native,bool compatible,unsigned coarse=1) noexcept {
        valid_=false;
        if(!compatible||native.sType!=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO||!native.renderPass||!native.layout||
           !native.pMultisampleState||native.pMultisampleState->rasterizationSamples!=VK_SAMPLE_COUNT_1_BIT)return false;
        std::array<const VkBaseInStructure*,32> visited{};unsigned count=0;
        for(auto* p=static_cast<const VkBaseInStructure*>(native.pNext);p;p=p->pNext){
            if(count==visited.size()||p->sType==VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR)return false;
            for(unsigned i=0;i<count;++i)if(visited[i]==p)return false;visited[count++]=p;
        }
        if(native.pDynamicState){const auto& d=*native.pDynamicState;
            if(d.dynamicStateCount>64||(d.dynamicStateCount&&!d.pDynamicStates))return false;
            for(unsigned i=0;i<d.dynamicStateCount;++i)if(d.pDynamicStates[i]==VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR)return false;
        }
        rate_={};rate_.sType=VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR;rate_.pNext=native.pNext;rate_.fragmentSize={1,1};rate_.combinerOps[0]=VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;rate_.combinerOps[1]=VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR;
        if(coarse==2||coarse==4){rate_.fragmentSize={coarse,coarse};rate_.combinerOps[1]=VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_KHR;}
        info_=native;info_.pNext=&rate_;valid_=true;return true;
    }
    const VkGraphicsPipelineCreateInfo* info()const noexcept{return valid_?&info_:nullptr;}
};
} // namespace rdrvr::runtime
