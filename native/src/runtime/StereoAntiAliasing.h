#pragma once
// Post anti-aliasing of the published stereo pair (StereoMailbox producer).
// FXAA 3.11 quality (shaders/fxaa.frag) as a
// full-screen triangle: sampled single-sample source (the captured eye) -> mailbox
// slot image through a mod-owned render pass. Owns only private objects; never
// touches engine pipelines, targets or state caches. Caller (the mailbox) owns
// synchronization: objects are only destroyed after every GPU use completed.
// the format-pair objects (sampler, set layout, descriptor pool/sets, pipeline
// layout, render pass, pipeline) are built by build() on a background thread, never
// inside the engine's submit hook, and are kept across ring generations; only the
// per-generation target views/framebuffers and per-capture source views come and go.
// Params::debug selects an in-headset proof mode of the same pipeline (push constant,
// uniform branch in fxaa.frag): EDGES paints every edge-test pixel magenta, SPLIT keeps FXAA
// on the left half of each eye, the raw copy on the right, with a 4 px magenta divider.
#include "StereoTargets.h"
#include "StereoAntiAliasingShaders.h"
#include "AntiAliasingStatus.h"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace rdrvr::runtime {
class StereoAntiAliasing {
public:
    enum class Fallback : unsigned {SourceNotSampled,FormatUnsupported,EyeFormatsDiffer,CreateFailed,ViewFailed,Building,Count};
    static_assert(unsigned(Fallback::Count)==antiAliasingFallbackCount);
    static const char* fallbackName(Fallback f) noexcept {
        switch(f){case Fallback::SourceNotSampled:return "sourceNotSampled";case Fallback::FormatUnsupported:return "formatUnsupported";
        case Fallback::EyeFormatsDiffer:return "eyeFormatsDiffer";case Fallback::CreateFailed:return "createFailed";case Fallback::ViewFailed:return "viewFailed";
        case Fallback::Building:return "building";default:return "unknown";}
    }
    // FXAA 3.11 quality knobs (defaults = the reference "default" preset values).
    // debug = fxaa.frag debugMode (debugNone/debugEdges/debugSplit; other values run plain FXAA).
    static constexpr std::int32_t debugNone=0,debugEdges=1,debugSplit=2;
    struct Params {float subpix{.75f},edgeThreshold{.166f},edgeThresholdMin{.0833f};std::int32_t debug{};};
    static const char* debugName(std::int32_t d) noexcept {return d==debugEdges?"edges":d==debugSplit?"split":"none";}
    static constexpr unsigned maxSlots=4;
    // Luma/output encoding for a (sampled view, attachment) pair; false = unsupported pair.
    //  UNORM->UNORM: values are display-encoded (engine output after tone map), FXAA as designed.
    //  UNORM->SRGB : UNORM bytes hold sRGB (SourceEncoding::SrgbEncoded): decode at output, attachment re-encodes.
    //  SRGB ->SRGB : sampler decodes to linear; luma = sqrt(green); attachment encodes (the engine's eye format).
    //  RGBA16F     : linear HDR; luma = sqrt(g/(1+g)); output sanitized (as the HDR display pass does).
    static bool modes(VkFormat source,VkFormat target,int& luma,int& decode) noexcept {
        const auto unorm=[](VkFormat f){return f==VK_FORMAT_R8G8B8A8_UNORM||f==VK_FORMAT_B8G8R8A8_UNORM;};
        const auto srgb=[](VkFormat f){return f==VK_FORMAT_R8G8B8A8_SRGB||f==VK_FORMAT_B8G8R8A8_SRGB;};
        const auto hdr=[](VkFormat f){return f==VK_FORMAT_R16G16B16A16_SFLOAT;};
        if(hdr(source)&&hdr(target)){luma=2;decode=0;return true;}
        if(unorm(source)&&unorm(target)){luma=0;decode=0;return true;}
        if(unorm(source)&&srgb(target)){luma=0;decode=1;return true;}
        if(srgb(source)&&srgb(target)){luma=1;decode=0;return true;}
        return false;
    }
    static bool supported(VkPhysicalDevice physical,VkFormat source,VkFormat target) noexcept {
        int luma{},decode{};if(!physical||!modes(source,target,luma,decode))return false;
        VkFormatProperties s{},t{};vkGetPhysicalDeviceFormatProperties(physical,source,&s);vkGetPhysicalDeviceFormatProperties(physical,target,&t);
        const VkFormatFeatureFlags sampled=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        return (s.optimalTilingFeatures&sampled)==sampled && (t.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
    }
    // everything that depends only on the (source, target) format pair.
    struct Shared {
        VkDevice device{};VkFormat source{},target{};int luma{},decode{};unsigned slots{};
        VkSampler sampler{};VkDescriptorSetLayout setLayout{};VkDescriptorPool pool{};std::array<VkDescriptorSet,2*maxSlots> sets{};
        VkPipelineLayout layout{};VkRenderPass pass{};VkPipeline pipeline{};std::uint64_t buildNs{};
        bool valid() const noexcept {return pipeline!=VK_NULL_HANDLE;}
    };
    static void destroy(Shared& s) noexcept {
        if(!s.device){s={};return;}
        if(s.pipeline)vkDestroyPipeline(s.device,s.pipeline,nullptr);if(s.pass)vkDestroyRenderPass(s.device,s.pass,nullptr);
        if(s.layout)vkDestroyPipelineLayout(s.device,s.layout,nullptr);if(s.pool)vkDestroyDescriptorPool(s.device,s.pool,nullptr);
        if(s.setLayout)vkDestroyDescriptorSetLayout(s.device,s.setLayout,nullptr);if(s.sampler)vkDestroySampler(s.device,s.sampler,nullptr);
        s={};
    }
    // Thread-safe (touches no mailbox state; Vulkan object creation needs no external
    // device synchronization). Runs on a background thread. Throws after destroying
    // every partially created object.
    static Shared build(VkDevice device,VkFormat source,VkFormat target,unsigned slots) {
        const auto start=std::chrono::steady_clock::now();
        Shared s{};
        if(!device||!slots||slots>maxSlots||!modes(source,target,s.luma,s.decode))throw std::runtime_error("invalid stereo AA configuration");
        s.device=device;s.source=source;s.target=target;s.slots=slots;
        VkShaderModule vs{},fs{};
        try {
            VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler.magFilter=sampler.minFilter=VK_FILTER_LINEAR;
            sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sampler.maxLod=0;checkVk(vkCreateSampler(device,&sampler,nullptr,&s.sampler),"AA sampler");
            VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
            VkDescriptorSetLayoutCreateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};ds.bindingCount=1;ds.pBindings=&binding;
            checkVk(vkCreateDescriptorSetLayout(device,&ds,nullptr,&s.setLayout),"AA descriptor layout");
            VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2*slots};
            VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=2*slots;pool.poolSizeCount=1;pool.pPoolSizes=&size;
            checkVk(vkCreateDescriptorPool(device,&pool,nullptr,&s.pool),"AA descriptor pool");
            std::array<VkDescriptorSetLayout,2*maxSlots> layouts{};layouts.fill(s.setLayout);
            VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};alloc.descriptorPool=s.pool;alloc.descriptorSetCount=2*slots;alloc.pSetLayouts=layouts.data();
            checkVk(vkAllocateDescriptorSets(device,&alloc,s.sets.data()),"AA descriptor sets");
            VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(Push)};
            VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&s.setLayout;
            pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;checkVk(vkCreatePipelineLayout(device,&pl,nullptr,&s.layout),"AA pipeline layout");
            // Full overwrite: no load; the slot image ends in GENERAL like the copy path left it
            // (consumer blit / HDR display pass read it in GENERAL after the ready semaphore).
            VkAttachmentDescription attachment{};attachment.format=target;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;attachment.finalLayout=VK_IMAGE_LAYOUT_GENERAL;
            VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&color;
            std::array<VkSubpassDependency,2> deps{};
            deps[0].srcSubpass=VK_SUBPASS_EXTERNAL;deps[0].dstSubpass=0;
            deps[0].srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;deps[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            deps[0].srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;deps[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_SHADER_READ_BIT;
            deps[1].srcSubpass=0;deps[1].dstSubpass=VK_SUBPASS_EXTERNAL;
            deps[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;deps[1].dstStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            deps[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;deps[1].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
            VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&attachment;
            rp.subpassCount=1;rp.pSubpasses=&sub;rp.dependencyCount=std::uint32_t(deps.size());rp.pDependencies=deps.data();
            checkVk(vkCreateRenderPass(device,&rp,nullptr,&s.pass),"AA render pass");
            VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};module.codeSize=sizeof(aa::vertex);module.pCode=aa::vertex;
            checkVk(vkCreateShaderModule(device,&module,nullptr,&vs),"AA vertex shader");
            module.codeSize=sizeof(aa::fxaaFragment);module.pCode=aa::fxaaFragment;
            checkVk(vkCreateShaderModule(device,&module,nullptr,&fs),"AA fragment shader");
            const std::array<std::int32_t,2> constants{s.luma,s.decode};
            const std::array<VkSpecializationMapEntry,2> entries{{{0,0,4},{1,4,4}}};
            VkSpecializationInfo specialization{2,entries.data(),sizeof(constants),constants.data()};
            VkPipelineShaderStageCreateInfo stages[2]{};stages[0].sType=stages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vs;stages[0].pName="main";
            stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fs;stages[1].pName="main";stages[1].pSpecializationInfo=&specialization;
            VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;
            VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.lineWidth=1;
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState blend{};blend.colorWriteMask=0xf;
            VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=1;cb.pAttachments=&blend;
            const VkDynamicState dynamic[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dy.dynamicStateCount=2;dy.pDynamicStates=dynamic;
            VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pi.stageCount=2;pi.pStages=stages;pi.pVertexInputState=&vi;pi.pInputAssemblyState=&ia;
            pi.pViewportState=&vp;pi.pRasterizationState=&rs;pi.pMultisampleState=&ms;pi.pColorBlendState=&cb;pi.pDynamicState=&dy;pi.layout=s.layout;pi.renderPass=s.pass;
            checkVk(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pi,nullptr,&s.pipeline),"AA pipeline");
        }catch(...){if(vs)vkDestroyShaderModule(device,vs,nullptr);if(fs)vkDestroyShaderModule(device,fs,nullptr);destroy(s);throw;}
        vkDestroyShaderModule(device,vs,nullptr);vkDestroyShaderModule(device,fs,nullptr);
        s.buildNs=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count());
        return s;
    }
private:
    struct Eye {VkImageView target{},source{};VkImage sourceImage{};VkFramebuffer framebuffer{};Extent extent{};};
    // fxaa.frag push block (std430): vec2 rcpFrame @0, subpix @8, edgeThreshold @12, edgeThresholdMin @16, int debugMode @20.
    struct Push {float rcpX,rcpY,subpix,edgeThreshold,edgeThresholdMin;std::int32_t debug;};
    static_assert(sizeof(Push)==24&&offsetof(Push,debug)==20);
    Shared shared_{};bool targets_{};
    std::array<std::array<Eye,2>,maxSlots> eyes_{};
    static void barrier(VkCommandBuffer c,VkImage image,VkImageLayout old,VkImageLayout next,VkAccessFlags src,VkAccessFlags dst,std::uint32_t layer){
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;b.oldLayout=old;b.newLayout=next;
        b.srcAccessMask=src;b.dstAccessMask=dst;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,layer,1};
        vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
    }
public:
    StereoAntiAliasing()=default;
    StereoAntiAliasing(const StereoAntiAliasing&)=delete;
    StereoAntiAliasing& operator=(const StereoAntiAliasing&)=delete;
    bool ready() const noexcept {return shared_.valid();}
    bool targetsReady() const noexcept {return targets_;}
    bool matches(VkFormat source,VkFormat target) const noexcept {return ready()&&source==shared_.source&&target==shared_.target;}
    int lumaMode() const noexcept {return shared_.luma;}
    int outputDecode() const noexcept {return shared_.decode;}
    std::uint64_t buildNs() const noexcept {return shared_.buildNs;}
    // Takes a finished build(); the previous format pair must have been destroyed.
    void adopt(Shared&& s) noexcept {if(ready())destroy(s);else{shared_=s;s={};}}
    // Per ring generation (cheap: one view + framebuffer per slot image). targets[slot][eye]
    // stay alive until destroyTargets(). Throws after destroying the partial targets.
    void createTargets(const std::array<std::array<VkImage,2>,maxSlots>& targets,const std::array<Extent,2>& extents) {
        if(!ready()||targets_)throw std::runtime_error("AA targets without pipeline");
        try{
            for(unsigned s=0;s<shared_.slots;++s)for(unsigned e=0;e<2;++e){
                auto& eye=eyes_[s][e];eye.extent=extents[e];
                if(!targets[s][e]||!extents[e].width||!extents[e].height)throw std::runtime_error("AA target missing");
                VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=targets[s][e];view.viewType=VK_IMAGE_VIEW_TYPE_2D;
                view.format=shared_.target;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                checkVk(vkCreateImageView(shared_.device,&view,nullptr,&eye.target),"AA target view");
                VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=shared_.pass;fb.attachmentCount=1;fb.pAttachments=&eye.target;
                fb.width=extents[e].width;fb.height=extents[e].height;fb.layers=1;checkVk(vkCreateFramebuffer(shared_.device,&fb,nullptr,&eye.framebuffer),"AA framebuffer");
            }
        }catch(...){destroyTargets();throw;}
        targets_=true;
    }
    // Before recording a slot: per-capture source views (the captured images may be
    // recreated between captures; a view never outlives the copy that used it) and
    // descriptor writes. The slot's previous GPU work must be complete. Throws after
    // releasing the slot's views; nothing was recorded yet, the caller may copy instead.
    void prepare(unsigned slot,const std::array<VkImage,2>& sources,const std::array<std::uint32_t,2>& layers) {
        if(!targets_||slot>=shared_.slots)throw std::runtime_error("AA prepare without targets");
        release(slot);
        try{
            for(unsigned e=0;e<2;++e){
                auto& eye=eyes_[slot][e];
                VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=sources[e];view.viewType=VK_IMAGE_VIEW_TYPE_2D;
                view.format=shared_.source;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,layers[e],1};
                checkVk(vkCreateImageView(shared_.device,&view,nullptr,&eye.source),"AA source view");eye.sourceImage=sources[e];
                VkDescriptorImageInfo info{shared_.sampler,eye.source,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=shared_.sets[2*slot+e];write.descriptorCount=1;
                write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&info;vkUpdateDescriptorSets(shared_.device,1,&write,0,nullptr);
            }
        }catch(...){release(slot);throw;}
    }
    // Records one eye: source -> SHADER_READ_ONLY, FXAA pass into the slot image
    // (ends GENERAL), source back to its layout. Valid only after prepare(slot).
    void record(VkCommandBuffer c,unsigned slot,unsigned eye,VkImage source,VkImageLayout layout,std::uint32_t layer,const Params& p) const {
        const auto& e=eyes_[slot][eye];
        barrier(c,source,layout,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,VK_ACCESS_SHADER_READ_BIT,layer);
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=shared_.pass;begin.framebuffer=e.framebuffer;
        begin.renderArea={{0,0},{e.extent.width,e.extent.height}};vkCmdBeginRenderPass(c,&begin,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,shared_.pipeline);
        vkCmdBindDescriptorSets(c,VK_PIPELINE_BIND_POINT_GRAPHICS,shared_.layout,0,1,&shared_.sets[2*slot+eye],0,nullptr);
        const Push push{1.f/float(e.extent.width),1.f/float(e.extent.height),p.subpix,p.edgeThreshold,p.edgeThresholdMin,p.debug};
        vkCmdPushConstants(c,shared_.layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);
        const VkViewport viewport{0,0,float(e.extent.width),float(e.extent.height),0,1};const VkRect2D scissor{{0,0},{e.extent.width,e.extent.height}};
        vkCmdSetViewport(c,0,1,&viewport);vkCmdSetScissor(c,0,1,&scissor);vkCmdDraw(c,3,1,0,0);vkCmdEndRenderPass(c);
        barrier(c,source,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,layout,VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,layer);
    }
    // does a live source view of this slot read one of these images?
    bool references(unsigned slot,const std::array<VkImage,2>& images) const noexcept {
        if(slot>=maxSlots)return false;
        for(const auto& e:eyes_[slot])if(e.source&&e.sourceImage&&(e.sourceImage==images[0]||e.sourceImage==images[1]))return true;
        return false;
    }
    // The slot's GPU work completed (fence observed), was rebuilt or never submitted.
    void release(unsigned slot) noexcept {
        if(!shared_.device||slot>=maxSlots)return;
        for(auto& e:eyes_[slot])if(e.source){vkDestroyImageView(shared_.device,e.source,nullptr);e.source={};e.sourceImage={};}
    }
    // Ring rebuild / detach (all slot fences observed): views and framebuffers only.
    void destroyTargets() noexcept {
        if(shared_.device)for(auto& slot:eyes_)for(auto& e:slot){
            if(e.source)vkDestroyImageView(shared_.device,e.source,nullptr);
            if(e.framebuffer)vkDestroyFramebuffer(shared_.device,e.framebuffer,nullptr);
            if(e.target)vkDestroyImageView(shared_.device,e.target,nullptr);
        }
        eyes_={};targets_=false;
    }
    // Shutdown / format-pair change with nothing in flight: everything.
    void destroy() noexcept {destroyTargets();destroy(shared_);}
};
} // namespace rdrvr::runtime
