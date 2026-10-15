#include "HdrTransfer.h"
#include "tonemap-shaders.h"
namespace rdrvr::runtime {
void HdrTransfer::create(VkDevice device,VkImage source,Extent extent,VkFormat format,const std::vector<XrSwapchainImageVulkan2KHR>& images) {
    createPass(device,source,extent,VK_FORMAT_R16G16B16A16_SFLOAT,format,images,false);
}
void HdrTransfer::createOverlay(VkDevice device,VkImage source,Extent extent,VkFormat sourceFormat,VkFormat format,const std::vector<XrSwapchainImageVulkan2KHR>& images) {
    const auto srgb=[](VkFormat f){return f==VK_FORMAT_R8G8B8A8_SRGB||f==VK_FORMAT_B8G8R8A8_SRGB;};
    const auto unorm=[](VkFormat f){return f==VK_FORMAT_R8G8B8A8_UNORM||f==VK_FORMAT_B8G8R8A8_UNORM;};
    if(!((srgb(sourceFormat)&&srgb(format))||(unorm(sourceFormat)&&unorm(format))))throw std::runtime_error("overlay color encoding mismatch");
    createPass(device,source,extent,sourceFormat,format,images,true);
}
void HdrTransfer::createPass(VkDevice device,VkImage source,Extent extent,VkFormat sourceFormat,VkFormat format,const std::vector<XrSwapchainImageVulkan2KHR>& images,bool overlay) {
    if(device_||!device||!source||!extent.width||!extent.height||images.empty())throw std::runtime_error("invalid HDR transfer");
    for(const auto& image:images)if(!image.image||image.image==source)throw std::runtime_error("transfer image alias or null target");
    const bool unorm=format==VK_FORMAT_R8G8B8A8_UNORM||format==VK_FORMAT_B8G8R8A8_UNORM;
    if(!unorm&&format!=VK_FORMAT_R8G8B8A8_SRGB&&format!=VK_FORMAT_B8G8R8A8_SRGB)throw std::runtime_error("unsupported HDR display format");
    device_=device;extent_=extent;overlay_=overlay;encode_=unorm&&!overlay;
    VkShaderModule vs{},fs{};
    try {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=source;view.viewType=VK_IMAGE_VIEW_TYPE_2D;
        view.format=sourceFormat;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        checkVk(vkCreateImageView(device,&view,nullptr,&source_),"HDR source view");
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler.magFilter=sampler.minFilter=VK_FILTER_NEAREST;
        sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        checkVk(vkCreateSampler(device,&sampler,nullptr,&sampler_),"HDR sampler");
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};ds.bindingCount=1;ds.pBindings=&binding;
        checkVk(vkCreateDescriptorSetLayout(device,&ds,nullptr,&descriptorLayout_),"HDR descriptor layout");
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=1;pool.poolSizeCount=1;pool.pPoolSizes=&size;
        checkVk(vkCreateDescriptorPool(device,&pool,nullptr,&pool_),"HDR descriptor pool");
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};alloc.descriptorPool=pool_;alloc.descriptorSetCount=1;alloc.pSetLayouts=&descriptorLayout_;
        checkVk(vkAllocateDescriptorSets(device,&alloc,&descriptor_),"HDR descriptor set");
        VkDescriptorImageInfo sourceInfo{sampler_,source_,VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=descriptor_;write.descriptorCount=1;
        write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&sourceInfo;vkUpdateDescriptorSets(device,1,&write,0,nullptr);
        VkAttachmentDescription attachment{};attachment.format=format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp=overlay?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout=overlay?VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;attachment.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&color;
        VkSubpassDependency dep{};dep.srcSubpass=VK_SUBPASS_EXTERNAL;dep.dstSubpass=0;
        dep.srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;dep.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;dep.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&attachment;
        rp.subpassCount=1;rp.pSubpasses=&sub;rp.dependencyCount=1;rp.pDependencies=&dep;
        checkVk(vkCreateRenderPass(device,&rp,nullptr,&pass_),"HDR render pass");
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,20};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&descriptorLayout_;
        pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;checkVk(vkCreatePipelineLayout(device,&pl,nullptr,&layout_),"HDR pipeline layout");
        VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};module.codeSize=sizeof(tonemap::vertex);module.pCode=tonemap::vertex;
        checkVk(vkCreateShaderModule(device,&module,nullptr,&vs),"HDR vertex shader");
        module.codeSize=overlay?sizeof(tonemap::overlayFragment):sizeof(tonemap::fragment);module.pCode=overlay?tonemap::overlayFragment:tonemap::fragment;
        checkVk(vkCreateShaderModule(device,&module,nullptr,&fs),"HDR fragment shader");
        VkPipelineShaderStageCreateInfo stages[2]{};stages[0].sType=stages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vs;stages[0].pName="main";
        stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fs;stages[1].pName="main";
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};blend.colorWriteMask=15;
        if(overlay){blend.blendEnable=VK_TRUE;blend.srcColorBlendFactor=blend.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;
            blend.dstColorBlendFactor=blend.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend.colorBlendOp=blend.alphaBlendOp=VK_BLEND_OP_ADD;}
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=1;cb.pAttachments=&blend;
        const VkDynamicState dynamic[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dy.dynamicStateCount=2;dy.pDynamicStates=dynamic;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pi.stageCount=2;pi.pStages=stages;pi.pVertexInputState=&vi;pi.pInputAssemblyState=&ia;
        pi.pViewportState=&vp;pi.pRasterizationState=&rs;pi.pMultisampleState=&ms;pi.pColorBlendState=&cb;pi.pDynamicState=&dy;pi.layout=layout_;pi.renderPass=pass_;
        checkVk(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pi,nullptr,&pipeline_),"HDR pipeline");
        targets_.resize(images.size());
        for(unsigned i=0;i<images.size();++i){auto& t=targets_[i];view.image=images[i].image;view.format=format;
            checkVk(vkCreateImageView(device,&view,nullptr,&t.view),"HDR output view");
            VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=pass_;fb.attachmentCount=1;fb.pAttachments=&t.view;
            fb.width=extent.width;fb.height=extent.height;fb.layers=1;checkVk(vkCreateFramebuffer(device,&fb,nullptr,&t.framebuffer),"HDR output framebuffer");}
    }catch(...){if(vs)vkDestroyShaderModule(device,vs,nullptr);if(fs)vkDestroyShaderModule(device,fs,nullptr);destroy();throw;}
    vkDestroyShaderModule(device,vs,nullptr);vkDestroyShaderModule(device,fs,nullptr);
}
void HdrTransfer::draw(VkCommandBuffer command,unsigned index)const {
    draw(command,index,{});
}
void HdrTransfer::draw(VkCommandBuffer command,unsigned index,const ScreenReticle& marker)const {
    if(!pipeline_||!command||index>=targets_.size())throw std::runtime_error("invalid HDR draw");
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=pass_;begin.framebuffer=targets_[index].framebuffer;
    begin.renderArea={{0,0},{extent_.width,extent_.height}};vkCmdBeginRenderPass(command,&begin,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,0,1,&descriptor_,0,nullptr);
    if(!overlay_){
        struct Push {std::uint32_t encode;float x,y,radius,opacity;};static_assert(sizeof(Push)==20);
        Push push{std::uint32_t(encode_),0,0,0,0};
        if(validReticle(marker)){push.x=marker.u*extent_.width;push.y=marker.v*extent_.height;
            push.radius=std::clamp(marker.radius*std::min(extent_.width,extent_.height),2.f,8.f);push.opacity=marker.opacity;}
        vkCmdPushConstants(command,layout_,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);
    }
    VkViewport viewport{0,0,float(extent_.width),float(extent_.height),0,1};VkRect2D scissor{{0,0},{extent_.width,extent_.height}};
    vkCmdSetViewport(command,0,1,&viewport);vkCmdSetScissor(command,0,1,&scissor);vkCmdDraw(command,3,1,0,0);vkCmdEndRenderPass(command);
}
void HdrTransfer::destroy() noexcept {
    if(!device_)return;
    for(auto& t:targets_){if(t.framebuffer)vkDestroyFramebuffer(device_,t.framebuffer,nullptr);if(t.view)vkDestroyImageView(device_,t.view,nullptr);}targets_.clear();
    if(pipeline_)vkDestroyPipeline(device_,pipeline_,nullptr);if(layout_)vkDestroyPipelineLayout(device_,layout_,nullptr);if(pass_)vkDestroyRenderPass(device_,pass_,nullptr);
    if(pool_)vkDestroyDescriptorPool(device_,pool_,nullptr);if(descriptorLayout_)vkDestroyDescriptorSetLayout(device_,descriptorLayout_,nullptr);
    if(sampler_)vkDestroySampler(device_,sampler_,nullptr);if(source_)vkDestroyImageView(device_,source_,nullptr);
    pipeline_={};layout_={};pass_={};pool_={};descriptor_={};descriptorLayout_={};sampler_={};source_={};device_={};extent_={};encode_=overlay_=false;
}
}
