#include "SceneRenderer.h"
#include "shaders.h"
#include <cstddef>
namespace rdrvr::harness {
using namespace runtime;
void SceneRenderer::initialize(VulkanBridge& gpu,VkFormat format) {
    gpu_=&gpu;
    VkAttachmentDescription attachment{};
    attachment.format=format; attachment.samples=VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    // OpenXR hands out images in COLOR_ATTACHMENT_OPTIMAL; UNDEFINED discards old pixels.
    attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{}; sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount=1; sub.pColorAttachments=&ref;
    VkSubpassDependency dep{}; dep.srcSubpass=VK_SUBPASS_EXTERNAL; dep.dstSubpass=0;
    dep.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount=1; rp.pAttachments=&attachment; rp.subpassCount=1; rp.pSubpasses=&sub; rp.dependencyCount=1; rp.pDependencies=&dep;
    checkVk(vkCreateRenderPass(gpu.device,&rp,nullptr,&pass_),"render pass");
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT,0,112};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; li.pushConstantRangeCount=1; li.pPushConstantRanges=&push;
    checkVk(vkCreatePipelineLayout(gpu.device,&li,nullptr,&layout_),"pipeline layout");
    VkShaderModule vs{},fs{};
    try {
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        si.codeSize=sizeof(shaders::vertex); si.pCode=shaders::vertex;
        checkVk(vkCreateShaderModule(gpu.device,&si,nullptr,&vs),"vertex module");
        si.codeSize=sizeof(shaders::fragment); si.pCode=shaders::fragment;
        checkVk(vkCreateShaderModule(gpu.device,&si,nullptr,&fs),"fragment module");
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType=stages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT; stages[0].module=vs; stages[0].pName="main";
        stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module=fs; stages[1].pName="main";
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology=VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount=1; vp.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.polygonMode=VK_POLYGON_MODE_FILL; rs.cullMode=VK_CULL_MODE_NONE; rs.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState color{}; color.colorWriteMask=15;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount=1; blend.pAttachments=&color;
        VkDynamicState dynamics[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; ds.dynamicStateCount=2; ds.pDynamicStates=dynamics;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pi.stageCount=2; pi.pStages=stages; pi.pVertexInputState=&vi; pi.pInputAssemblyState=&ia;
        pi.pViewportState=&vp; pi.pRasterizationState=&rs; pi.pMultisampleState=&ms; pi.pColorBlendState=&blend;
        pi.pDynamicState=&ds; pi.layout=layout_; pi.renderPass=pass_;
        checkVk(vkCreateGraphicsPipelines(gpu.device,VK_NULL_HANDLE,1,&pi,nullptr,&pipeline_),"line pipeline");
    } catch (...) { if(vs) vkDestroyShaderModule(gpu.device,vs,nullptr); if(fs) vkDestroyShaderModule(gpu.device,fs,nullptr); throw; }
    vkDestroyShaderModule(gpu.device,vs,nullptr); vkDestroyShaderModule(gpu.device,fs,nullptr);
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pc.queueFamilyIndex=gpu.queueFamily;
    checkVk(vkCreateCommandPool(gpu.device,&pc,nullptr,&pool_),"command pool");
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool=pool_; ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount=1;
    checkVk(vkAllocateCommandBuffers(gpu.device,&ca,&command_),"command buffer");
    VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fc.flags=VK_FENCE_CREATE_SIGNALED_BIT;
    checkVk(vkCreateFence(gpu.device,&fc,nullptr,&fence_),"fence");
}
void SceneRenderer::createTarget(VkImage image,VkFormat format,Extent extent,Target& t) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=image; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=format;
    vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    checkVk(vkCreateImageView(gpu_->device,&vi,nullptr,&t.view),"eye image view");
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fi.renderPass=pass_; fi.attachmentCount=1; fi.pAttachments=&t.view;
    fi.width=extent.width; fi.height=extent.height; fi.layers=1;
    checkVk(vkCreateFramebuffer(gpu_->device,&fi,nullptr,&t.framebuffer),"eye framebuffer");
}
void SceneRenderer::destroyTarget(Target& t) noexcept {
    if(t.framebuffer) vkDestroyFramebuffer(gpu_->device,t.framebuffer,nullptr);
    if(t.view) vkDestroyImageView(gpu_->device,t.view,nullptr);
    t={};
}
void SceneRenderer::draw(const Target& target,Extent extent,unsigned eye,const Pose& pose,Fov fov,const std::array<Pose,2>& hands) {
    auto device=gpu_->device;
    checkVk(vkWaitForFences(device,1,&fence_,VK_TRUE,UINT64_MAX),"previous eye complete");
    checkVk(vkResetCommandBuffer(command_,0),"reset commands");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(command_,&bi),"begin commands");
    VkClearValue clear{}; clear.color.float32[0]=eye?0.008f:0.03f; clear.color.float32[1]=0.012f; clear.color.float32[2]=eye?0.03f:0.008f; clear.color.float32[3]=1;
    VkRenderPassBeginInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; ri.renderPass=pass_; ri.framebuffer=target.framebuffer;
    ri.renderArea={{0,0},{extent.width,extent.height}}; ri.clearValueCount=1; ri.pClearValues=&clear;
    vkCmdBeginRenderPass(command_,&ri,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command_,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_);
    VkViewport viewport{0,0,float(extent.width),float(extent.height),0,1}; VkRect2D scissor{{0,0},{extent.width,extent.height}};
    vkCmdSetViewport(command_,0,1,&viewport); vkCmdSetScissor(command_,0,1,&scissor);
    struct Draw { Mat4 vp; std::array<float,4> a,b,color; } draw;
    static_assert(sizeof(Draw)==112);
    draw.vp=multiply(projection(fov),viewMatrix(pose));
    auto line=[&](Vec3 a,Vec3 b,std::array<float,4> color) {
        draw.a={a[0],a[1],a[2],1}; draw.b={b[0],b[1],b[2],1}; draw.color=color;
        vkCmdPushConstants(command_,layout_,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(draw),&draw); vkCmdDraw(command_,2,1,0,0);
    };
    for(int i=-10;i<=10;++i) {
        float x=i*0.5f;
        line({x,-1.5f,-5},{x,-1.5f,5},{0.22f,0.3f,0.22f,1});
        line({-5,-1.5f,x},{5,-1.5f,x},{0.22f,0.3f,0.22f,1});
    }
    // 0.5 metre wire cube, centre two metres in front of initial LOCAL origin.
    for(int axis=0;axis<3;++axis) for(int i=0;i<4;++i) {
        Vec3 a{-0.25f,-0.25f,-0.25f},b=a;
        a[(axis+1)%3]=b[(axis+1)%3]=(i&1)?0.25f:-0.25f;
        a[(axis+2)%3]=b[(axis+2)%3]=(i&2)?0.25f:-0.25f;
        b[axis]=0.25f; a[2]-=2; b[2]-=2;
        line(a,b,{0.9f,0.8f,0.2f,1});
    }
    for(unsigned i=0;i<2;++i) if(hands[i].positionValid && hands[i].orientationValid) {
        const auto& h=hands[i]; auto direction=rotate(h.orientation,{0,0,-1.5f});
        line(h.position,{h.position[0]+direction[0],h.position[1]+direction[1],h.position[2]+direction[2]},i?std::array<float,4>{0.2f,0.8f,1,1}:std::array<float,4>{1,0.3f,0.2f,1});
    }
    vkCmdEndRenderPass(command_); checkVk(vkEndCommandBuffer(command_),"end commands");
    checkVk(vkResetFences(device,1,&fence_),"reset fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&command_;
    checkVk(vkQueueSubmit(gpu_->queue,1,&submit,fence_),"submit eye");
    // Deliberately serialized diagnostic baseline; never release on a CPU-ready flag.
    checkVk(vkWaitForFences(device,1,&fence_,VK_TRUE,UINT64_MAX),"eye GPU complete");
}
void SceneRenderer::destroy() noexcept {
    if(!gpu_ || !gpu_->device) return;
    auto d=gpu_->device;
    if(fence_) vkDestroyFence(d,fence_,nullptr);
    if(pool_) vkDestroyCommandPool(d,pool_,nullptr);
    if(pipeline_) vkDestroyPipeline(d,pipeline_,nullptr);
    if(layout_) vkDestroyPipelineLayout(d,layout_,nullptr);
    if(pass_) vkDestroyRenderPass(d,pass_,nullptr);
    fence_={}; pool_={}; pipeline_={}; layout_={}; pass_={}; gpu_=nullptr;
}
} // namespace rdrvr::harness
