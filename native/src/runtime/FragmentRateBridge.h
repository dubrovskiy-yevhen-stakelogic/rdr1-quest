#pragma once
#include "FragmentRatePlans.h"
#include "FragmentRateRouting.h"
#include "FragmentRateTelemetry.h"
#include <atomic>
#include <cerrno>

namespace rdrvr::runtime {
struct FragmentRateBridgeApi {
    PFN_vkCreateRenderPass createPass{};PFN_vkCreateRenderPass2KHR createPass2{};PFN_vkDestroyRenderPass destroyPass{};
    PFN_vkCreateFramebuffer createFramebuffer{};PFN_vkCreateGraphicsPipelines createPipelines{};
    PFN_vkCreateImageView createView{};PFN_vkDestroyImageView destroyView{};
    explicit operator bool()const{return createPass&&createPass2&&destroyPass&&createFramebuffer&&createPipelines&&createView&&destroyView;}
};
// What the device owner proved and enabled (GameFragmentRate gameFragmentRateCapabilities). ready = the bridge
// converts pipelines of `device`; pipeline/attachment = VkPhysicalDeviceFragmentShadingRateFeaturesKHR enabled on it;
// squareMask bit k = (2^k)x(2^k) listed for 1 sample by vkGetPhysicalDeviceFragmentShadingRatesKHR.
struct FragmentRateCapabilities {
    bool ready{},pipeline{},attachment{},nonTrivialCombiner{};unsigned squareMask{};VkExtent2D maxFragmentSize{};VkDevice device{};const char* status{""};
};
// Device owner proves native pass shapes and complete hook coverage, enables
// device features and prepares a permanent full-rate map before configure().
// There are deliberately no per-draw hooks, shader edits or render-pass swaps.
class FragmentRateBridge {
    FragmentRateBridgeApi api_{};FragmentRateRouting routing_;
    VkDevice device_{};VkExtent2D texel_{};
    std::atomic<std::uint64_t> convertedPasses_{},fallbackPasses_{},convertedPipelines_{},declinedPipelines_{},invalidFrames_{};
    // Telemetry only: batches of more than 64 pipelines stay unconverted (unchanged default image) and are
    // counted; routable = their pipelines whose pass is converted (what conversion would change); unrouted = pipelines of
    // a <=64 batch whose pass is not converted (created natively).
    std::atomic<std::uint64_t> largeBatches_{},largeBatchPipelines_{},largeBatchRoutable_{},unroutedPipelines_{};
    std::atomic<std::uint32_t> largeBatchMax_{};
    std::atomic<std::uint64_t> coarsePipelines_{}; // Converted with a coarse pipeline rate (MAX combiner)
public:
    bool configure(FragmentRateBridgeApi api,VkDevice device,VkExtent2D texel,VkImageView full,unsigned width,unsigned height) noexcept {
        if(device_||!api||!texel.width||!texel.height)return false;
        if(!routing_.bind(device,full,width,height))return false;
        api_=api;device_=device;texel_=texel;return true;
    }
    FragmentRateRouting& routing() noexcept{return routing_;}
    // Hooks pass the original function explicitly so disabled/foreign-device
    // calls retain their exact arguments and execute once, without a registry.
    VkResult createPass(PFN_vkCreateRenderPass original,VkDevice d,const VkRenderPassCreateInfo* c,const VkAllocationCallbacks* a,VkRenderPass* out) noexcept {
        const int incoming=errno;
        if(d!=device_||!device_||!c||!out)return original(d,c,a,out);
        FragmentRatePassPlan plan;
        if(!plan.prepare(*c,texel_,true)){++fallbackPasses_;errno=incoming;return original(d,c,a,out);}
        errno=incoming;VkRenderPass created{};auto result=api_.createPass2(d,plan.info(),a,&created);const int saved=errno;
        if(result==VK_SUCCESS&&created){
            if(routing_.addPass(d,created,plan.nativeAttachmentCount())){*out=created;++convertedPasses_;errno=saved;return result;}
            api_.destroyPass(d,created,a);result=VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        // Other passes in the same native compatibility class may already be
        // transformed. An unextended fallback here would break that class.
        *out=VK_NULL_HANDLE;errno=saved;return result==VK_SUCCESS?VK_ERROR_INITIALIZATION_FAILED:result;
    }
    void destroyPass(PFN_vkDestroyRenderPass original,VkDevice d,VkRenderPass pass,const VkAllocationCallbacks* a) noexcept {
        const int incoming=errno;if(d==device_&&device_)routing_.removePass(d,pass);errno=incoming;original(d,pass,a);
    }
    VkResult createView(PFN_vkCreateImageView original,VkDevice d,const VkImageViewCreateInfo* c,const VkAllocationCallbacks* a,VkImageView* out) noexcept {
        const auto result=original(d,c,a,out);const int saved=errno;
        if(d==device_&&device_&&result==VK_SUCCESS&&c&&out&&*out)routing_.addView(d,*c,*out);errno=saved;return result;
    }
    void destroyView(PFN_vkDestroyImageView original,VkDevice d,VkImageView view,const VkAllocationCallbacks* a) noexcept {
        const int incoming=errno;if(d==device_&&device_)routing_.removeView(d,view);errno=incoming;original(d,view,a);
    }
    VkResult createFramebuffer(PFN_vkCreateFramebuffer original,VkDevice d,const VkFramebufferCreateInfo* c,const VkAllocationCallbacks* a,VkFramebuffer* out) noexcept {
        const int incoming=errno;
        if(d!=device_||!device_||!c||!out)return original(d,c,a,out);
        const auto decision=routing_.framebuffer(d,*c);
        if(!decision.converted){errno=incoming;return original(d,c,a,out);}
        FragmentRateRouting::Pass pass{};FragmentRateFramebufferPlan plan;
        if(!decision.map||!routing_.pass(d,c->renderPass,pass)||!plan.prepare(*c,pass.attachments,decision.map)){
            // Passing the unextended attachment list to a converted pass would be invalid Vulkan.
            ++invalidFrames_;*out=VK_NULL_HANDLE;errno=incoming;return VK_ERROR_INITIALIZATION_FAILED;
        }
        errno=incoming;const auto result=original(d,plan.info(),a,out);const int saved=errno;
        if(result==VK_SUCCESS&&out&&*out&&decision.eye>=0)fragmentRateReadiness.store(2,std::memory_order_relaxed);
        errno=saved;return result;
    }
    // coarse (optional, one byte per pipeline): in = wanted coarse size (0/1 none, 2 or 4), out = the size really
    // planned (0 when the pipeline is not converted: foreign device, large batch, unrouted pass, declined plan).
    VkResult createPipelines(PFN_vkCreateGraphicsPipelines original,VkDevice d,VkPipelineCache cache,std::uint32_t count,const VkGraphicsPipelineCreateInfo* c,const VkAllocationCallbacks* a,VkPipeline* out,std::uint8_t* coarse=nullptr) noexcept {
        const int incoming=errno;
        if(coarse&&(d!=device_||!device_||!count||!c||!out||count>64))for(std::uint32_t i=0;i<count&&i<64;++i)coarse[i]=0;
        if(d!=device_||!device_||!count||!c||!out)return original(d,cache,count,c,a,out);
        if(count>64){
            // Counted only, forwarded unchanged (the original arguments, one call).
            ++largeBatches_;largeBatchPipelines_.fetch_add(count,std::memory_order_relaxed);
            for(auto seen=largeBatchMax_.load(std::memory_order_relaxed);count>seen&&!largeBatchMax_.compare_exchange_weak(seen,count,std::memory_order_relaxed);){}
            std::uint64_t routable=0;
            for(std::uint32_t i=0;i<count;++i){FragmentRateRouting::Pass pass{};if(routing_.pass(d,c[i].renderPass,pass))++routable;}
            largeBatchRoutable_.fetch_add(routable,std::memory_order_relaxed);
            errno=incoming;return original(d,cache,count,c,a,out);
        }
        std::array<FragmentRatePipelinePlan,64> plans;
        std::array<VkGraphicsPipelineCreateInfo,64> copies{};bool changed=false;
        for(unsigned i=0;i<count;++i){copies[i]=c[i];FragmentRateRouting::Pass pass{};
            const unsigned want=coarse&&(coarse[i]==2||coarse[i]==4)?coarse[i]:1u;if(coarse)coarse[i]=0;
            if(!routing_.pass(d,c[i].renderPass,pass)){++unroutedPipelines_;continue;}
            if(plans[i].prepare(c[i],true,want)){copies[i]=*plans[i].info();changed=true;++convertedPipelines_;if(want>1){coarse[i]=std::uint8_t(want);++coarsePipelines_;}}
            else ++declinedPipelines_; // Native default fragment size 1x1 is retained.
        }
        // Keep ONE call and the complete order: derivative basePipelineIndex,
        // partial success and failure outputs retain native batch semantics.
        errno=incoming;return original(d,cache,count,changed?copies.data():c,a,out);
    }
    bool clear(VkDevice d) noexcept {if(d!=device_||!device_)return false;routing_.clear(d);device_={};api_={};return true;}
    std::uint64_t convertedPasses()const{return convertedPasses_.load();}
    std::uint64_t fallbackPasses()const{return fallbackPasses_.load();}
    std::uint64_t convertedPipelines()const{return convertedPipelines_.load();}
    std::uint64_t declinedPipelines()const{return declinedPipelines_.load();}
    std::uint64_t invalidFrames()const{return invalidFrames_.load();}
    std::uint64_t largeBatches()const{return largeBatches_.load();}
    std::uint64_t largeBatchPipelines()const{return largeBatchPipelines_.load();}
    std::uint64_t largeBatchRoutable()const{return largeBatchRoutable_.load();}
    std::uint32_t largeBatchMax()const{return largeBatchMax_.load();}
    std::uint64_t unroutedPipelines()const{return unroutedPipelines_.load();}
    std::uint64_t coarsePipelines()const{return coarsePipelines_.load();}
    bool configuredFor(VkDevice d)const noexcept{return d&&d==device_;}  // Pipelines of d are converted
};
}
