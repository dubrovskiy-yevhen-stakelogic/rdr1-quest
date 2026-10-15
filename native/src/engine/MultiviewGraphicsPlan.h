#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <cstring>
#include <cstdint>

namespace rdrvr::engine {
struct MultiviewGraphicsSupport {
    // These are enabled logical-device features, not merely physical support.
    bool depthClipEnable{},extendedDepthBounds{};
};
// Owns a bounded copy of a supported native graphics pipeline description.
// Inputs must already be readable/validated by the caller. This is not an
// interposer or a shader-coverage decision. Replacement shaders, layout and
// render pass must outlive vkCreateGraphicsPipelines; no Vulkan handle is owned.
class MultiviewGraphicsPlan {
    static constexpr unsigned maxStages=2,maxEntries=64,maxData=4096,maxName=128;
    std::array<VkPipelineShaderStageCreateInfo,maxStages> stages_{};
    std::array<std::array<char,maxName>,maxStages> names_{};
    std::array<VkSpecializationInfo,maxStages> specializations_{};
    std::array<std::array<VkSpecializationMapEntry,maxEntries>,maxStages> entries_{};
    std::array<std::array<unsigned char,maxData>,maxStages> data_{};
    std::array<VkVertexInputBindingDescription,32> bindings_{};
    std::array<VkVertexInputAttributeDescription,32> attributes_{};
    VkPipelineVertexInputStateCreateInfo vertex_{};
    VkPipelineInputAssemblyStateCreateInfo assembly_{};
    VkViewport viewport_{};VkRect2D scissor_{};
    VkPipelineViewportStateCreateInfo viewportState_{};
    VkPipelineRasterizationStateCreateInfo raster_{};
    VkPipelineRasterizationDepthClipStateCreateInfoEXT depthClip_{};
    VkPipelineMultisampleStateCreateInfo multisample_{};VkSampleMask sampleMask_{};
    VkPipelineDepthStencilStateCreateInfo depth_{};
    std::array<VkPipelineColorBlendAttachmentState,8> attachments_{};
    VkPipelineColorBlendStateCreateInfo blend_{};
    std::array<VkDynamicState,10> dynamics_{};
    VkPipelineDynamicStateCreateInfo dynamic_{};
    VkGraphicsPipelineCreateInfo info_{};bool valid_{};
    template<class T> static bool plain(const T* p,VkStructureType type){return p&&p->sType==type&&!p->pNext&&!p->flags;}
public:
    MultiviewGraphicsPlan()=default;
    MultiviewGraphicsPlan(const MultiviewGraphicsPlan&)=delete;
    MultiviewGraphicsPlan& operator=(const MultiviewGraphicsPlan&)=delete;
    bool prepare(const VkGraphicsPipelineCreateInfo& native,VkShaderModule vertexShader,VkShaderModule fragmentShader,
                 VkPipelineLayout layout,VkRenderPass pass,MultiviewGraphicsSupport support={}) noexcept {
        valid_=false;
        if(native.sType!=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO || native.pNext ||
           (native.flags&~(VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT|VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT)) ||
           !layout || !pass || !native.layout || !native.renderPass || native.subpass ||
           !native.stageCount || native.stageCount>maxStages || !native.pStages || native.pTessellationState)return false;
        unsigned seen=0;
        for(unsigned i=0;i<native.stageCount;++i){
            const auto& s=native.pStages[i];
            if(!plain(&s,VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO) || !s.module || !s.pName)return false;
            const unsigned bit=s.stage==VK_SHADER_STAGE_VERTEX_BIT?1:s.stage==VK_SHADER_STAGE_FRAGMENT_BIT?2:0;
            const auto shader=bit==1?vertexShader:fragmentShader;
            if(!bit || (seen&bit) || !shader)return false;
            seen|=bit;unsigned n=0;
            while(n<maxName && s.pName[n]){names_[i][n]=s.pName[n];++n;}
            if(!n || n==maxName)return false;
            names_[i][n]=0;stages_[i]=s;stages_[i].module=shader;stages_[i].pName=names_[i].data();
            if(s.pSpecializationInfo){
                const auto& sp=*s.pSpecializationInfo;
                if(sp.mapEntryCount>maxEntries || sp.dataSize>maxData || (sp.mapEntryCount&&!sp.pMapEntries) || (sp.dataSize&&!sp.pData))return false;
                for(unsigned j=0;j<sp.mapEntryCount;++j){
                    const auto& e=sp.pMapEntries[j];
                    if(!e.size || e.offset>sp.dataSize || e.size>sp.dataSize-e.offset)return false;
                    for(unsigned k=0;k<j;++k)if(sp.pMapEntries[k].constantID==e.constantID)return false;
                    entries_[i][j]=e;
                }
                if(sp.dataSize)std::memcpy(data_[i].data(),sp.pData,sp.dataSize);
                specializations_[i]=sp;specializations_[i].pMapEntries=sp.mapEntryCount?entries_[i].data():nullptr;
                specializations_[i].pData=sp.dataSize?data_[i].data():nullptr;stages_[i].pSpecializationInfo=&specializations_[i];
            }
        }
        if(!(seen&1))return false;
        const auto* v=native.pVertexInputState;
        if(!plain(v,VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO) || v->vertexBindingDescriptionCount>bindings_.size() ||
           v->vertexAttributeDescriptionCount>attributes_.size() || (v->vertexBindingDescriptionCount&&!v->pVertexBindingDescriptions) ||
           (v->vertexAttributeDescriptionCount&&!v->pVertexAttributeDescriptions))return false;
        for(unsigned i=0;i<v->vertexBindingDescriptionCount;++i)bindings_[i]=v->pVertexBindingDescriptions[i];
        for(unsigned i=0;i<v->vertexAttributeDescriptionCount;++i)attributes_[i]=v->pVertexAttributeDescriptions[i];
        vertex_=*v;vertex_.pVertexBindingDescriptions=v->vertexBindingDescriptionCount?bindings_.data():nullptr;
        vertex_.pVertexAttributeDescriptions=v->vertexAttributeDescriptionCount?attributes_.data():nullptr;
        if(!plain(native.pInputAssemblyState,VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO) ||
           native.pInputAssemblyState->topology==VK_PRIMITIVE_TOPOLOGY_PATCH_LIST ||
           !native.pRasterizationState || native.pRasterizationState->sType!=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO ||
           native.pRasterizationState->flags ||
           native.pRasterizationState->rasterizerDiscardEnable ||
           !plain(native.pMultisampleState,VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO) ||
           native.pMultisampleState->rasterizationSamples!=VK_SAMPLE_COUNT_1_BIT)return false;
        assembly_=*native.pInputAssemblyState;raster_=*native.pRasterizationState;multisample_=*native.pMultisampleState;
        if(raster_.pNext){
            const auto* clip=static_cast<const VkPipelineRasterizationDepthClipStateCreateInfoEXT*>(raster_.pNext);
            if(!support.depthClipEnable||clip->sType!=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT||
               clip->pNext||clip->flags||clip->depthClipEnable>VK_TRUE)return false;
            depthClip_=*clip;raster_.pNext=&depthClip_;
        }
        if(multisample_.pSampleMask){sampleMask_=*multisample_.pSampleMask;multisample_.pSampleMask=&sampleMask_;}
        bool dynamicViewport=false,dynamicScissor=false;
        if(native.pDynamicState){
            const auto& d=*native.pDynamicState;
            if(!plain(&d,VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO) || d.dynamicStateCount>dynamics_.size() || (d.dynamicStateCount&&!d.pDynamicStates))return false;
            for(unsigned i=0;i<d.dynamicStateCount;++i){
                // The pinned native builder adds precisely DEPTH_BOUNDS and
                // DEPTH_BOUNDS_TEST_ENABLE when its extension branch is active.
                // Do not generalize that evidence to viewport-with-count or
                // other extension states/command contracts.
                const auto state=d.pDynamicStates[i];
                const bool core=state>=VK_DYNAMIC_STATE_VIEWPORT&&state<=VK_DYNAMIC_STATE_STENCIL_REFERENCE;
                if(!core&&!(support.extendedDepthBounds&&state==VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE))return false;
                for(unsigned j=0;j<i;++j)if(d.pDynamicStates[j]==d.pDynamicStates[i])return false;
                dynamics_[i]=d.pDynamicStates[i];dynamicViewport|=dynamics_[i]==VK_DYNAMIC_STATE_VIEWPORT;dynamicScissor|=dynamics_[i]==VK_DYNAMIC_STATE_SCISSOR;
            }
            dynamic_=d;dynamic_.pDynamicStates=d.dynamicStateCount?dynamics_.data():nullptr;
        }
        if(!plain(native.pViewportState,VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO) ||
           native.pViewportState->viewportCount!=1 || native.pViewportState->scissorCount!=1 ||
           (!dynamicViewport&&!native.pViewportState->pViewports) || (!dynamicScissor&&!native.pViewportState->pScissors))return false;
        viewportState_=*native.pViewportState;
        if(dynamicViewport)viewportState_.pViewports=nullptr;
        else {viewport_=*viewportState_.pViewports;viewportState_.pViewports=&viewport_;}
        if(dynamicScissor)viewportState_.pScissors=nullptr;
        else {scissor_=*viewportState_.pScissors;viewportState_.pScissors=&scissor_;}
        if(native.pDepthStencilState){
            if(!plain(native.pDepthStencilState,VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO))return false;
            depth_=*native.pDepthStencilState;
        }
        if(native.pColorBlendState){
            const auto& b=*native.pColorBlendState;
            if(!plain(&b,VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO) || b.attachmentCount>attachments_.size() || (b.attachmentCount&&!b.pAttachments))return false;
            for(unsigned i=0;i<b.attachmentCount;++i)attachments_[i]=b.pAttachments[i];
            blend_=b;blend_.pAttachments=b.attachmentCount?attachments_.data():nullptr;
        }
        info_=native;info_.pStages=stages_.data();info_.pVertexInputState=&vertex_;info_.pInputAssemblyState=&assembly_;
        info_.pViewportState=&viewportState_;info_.pRasterizationState=&raster_;info_.pMultisampleState=&multisample_;
        info_.pDepthStencilState=native.pDepthStencilState?&depth_:nullptr;info_.pColorBlendState=native.pColorBlendState?&blend_:nullptr;
        info_.pDynamicState=native.pDynamicState?&dynamic_:nullptr;info_.layout=layout;info_.renderPass=pass;
        info_.basePipelineHandle=VK_NULL_HANDLE;info_.basePipelineIndex=-1;valid_=true;return true;
    }
    const VkGraphicsPipelineCreateInfo* info() const noexcept{return valid_?&info_:nullptr;}
};
} // namespace rdrvr::engine
