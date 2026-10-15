#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <sstream>
#include <string>

namespace rdrvr::diagnostics {
// Creation observations only: these counters do not track destruction,
// residency, image-view mip ranges, sampler selection, or shader material use.
class TextureQualityTelemetry {
public:
    struct Snapshot {
        std::uint64_t successfulCreates{},failedCreates{},invalid{},pNextDeclined{},unsupported{},other{};
        std::uint64_t sampledImages{},sampledSingleMip{},sampledMultiMip{},sampledArrays{},attachmentImages{};
        // Sampled nonattachment max(width,height): <=256,512,1024,2048,>2048.
        std::array<std::uint64_t,5> sampledSizeBuckets{};
        // Attachment sample count: 1,2,4,8,16,32,64.
        std::array<std::uint64_t,7> attachmentSamples{};
        std::uint32_t maxSampledMipLevels{},maxSampledWidth{},maxSampledHeight{};
    };
private:
    std::atomic<std::uint64_t> successful_{},failed_{},invalid_{},pNext_{},unsupported_{},other_{};
    std::atomic<std::uint64_t> sampled_{},single_{},multi_{},arrays_{},attachments_{};
    std::array<std::atomic<std::uint64_t>,5> sizes_{};
    std::array<std::atomic<std::uint64_t>,7> samples_{};
    std::atomic<std::uint32_t> maxMips_{},maxWidth_{},maxHeight_{};
    template<class T> static void maximum(std::atomic<T>& current,T proposed) noexcept {
        T observed=current.load(std::memory_order_relaxed);
        while(observed<proposed&&!current.compare_exchange_weak(observed,proposed,std::memory_order_relaxed)){}
    }
    static void increment(std::atomic<std::uint64_t>& c) noexcept {c.fetch_add(1,std::memory_order_relaxed);}
public:
    void created(VkDevice device,const VkImageCreateInfo* info,VkImage image,VkResult result) noexcept {
        const int incoming=errno;
        struct Restore {int value;~Restore(){errno=value;}} restore{incoming};
        if(result!=VK_SUCCESS){increment(failed_);return;}
        increment(successful_);
        if(!device||!image||!info||info->sType!=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO){increment(invalid_);return;}
        if(info->pNext){increment(pNext_);return;}
        if(info->imageType!=VK_IMAGE_TYPE_2D){increment(unsupported_);return;}
        const auto sample=std::uint32_t(info->samples);
        if(info->format==VK_FORMAT_UNDEFINED||!info->extent.width||!info->extent.height||info->extent.depth!=1||
           !info->mipLevels||!info->arrayLayers||!sample||sample>64||(sample&(sample-1))){increment(invalid_);return;}
        constexpr VkImageUsageFlags attachment=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if(info->usage&attachment){
            increment(attachments_);unsigned bucket{};for(auto n=sample;n>1;n>>=1)++bucket;
            increment(samples_[bucket]);return;
        }
        if(!(info->usage&VK_IMAGE_USAGE_SAMPLED_BIT)){increment(other_);return;}
        increment(sampled_);increment(info->mipLevels==1?single_:multi_);
        if(info->arrayLayers>1)increment(arrays_);
        const auto size=info->extent.width>info->extent.height?info->extent.width:info->extent.height;
        const unsigned bucket=size<=256?0:size<=512?1:size<=1024?2:size<=2048?3:4;
        increment(sizes_[bucket]);maximum(maxMips_,info->mipLevels);maximum(maxWidth_,info->extent.width);maximum(maxHeight_,info->extent.height);
    }
    Snapshot snapshot() const noexcept {
        const auto get=[](const auto& c){return c.load(std::memory_order_relaxed);};
        Snapshot s{get(successful_),get(failed_),get(invalid_),get(pNext_),get(unsupported_),get(other_),
            get(sampled_),get(single_),get(multi_),get(arrays_),get(attachments_)};
        for(unsigned i=0;i<s.sampledSizeBuckets.size();++i)s.sampledSizeBuckets[i]=get(sizes_[i]);
        for(unsigned i=0;i<s.attachmentSamples.size();++i)s.attachmentSamples[i]=get(samples_[i]);
        s.maxSampledMipLevels=get(maxMips_);s.maxSampledWidth=get(maxWidth_);s.maxSampledHeight=get(maxHeight_);return s;
    }
    std::string report() const {
        const auto s=snapshot();std::ostringstream o;
        o<<"{\"scope\":\"cumulative observed VkImage creations; not live residency or material bindings\""
         <<",\"successfulCreates\":"<<s.successfulCreates<<",\"failedCreates\":"<<s.failedCreates
         <<",\"invalid\":"<<s.invalid<<",\"pNextDeclined\":"<<s.pNextDeclined<<",\"unsupported\":"<<s.unsupported<<",\"other\":"<<s.other
         <<",\"sampledNonAttachment2D\":"<<s.sampledImages<<",\"sampledSingleMip\":"<<s.sampledSingleMip
         <<",\"sampledMultiMip\":"<<s.sampledMultiMip<<",\"sampledArrays\":"<<s.sampledArrays
         <<",\"maxSampledMipLevels\":"<<s.maxSampledMipLevels<<",\"maxSampledWidth\":"<<s.maxSampledWidth<<",\"maxSampledHeight\":"<<s.maxSampledHeight
         <<",\"sampledSizeMaxAxisBounds\":[256,512,1024,2048,null],\"sampledSizeBuckets\":[";
        for(unsigned i=0;i<s.sampledSizeBuckets.size();++i){if(i)o<<',';o<<s.sampledSizeBuckets[i];}
        o<<"],\"attachmentImages\":"<<s.attachmentImages<<",\"attachmentSampleCounts\":[1,2,4,8,16,32,64],\"attachmentSamples\":[";
        for(unsigned i=0;i<s.attachmentSamples.size();++i){if(i)o<<',';o<<s.attachmentSamples[i];}
        o<<"]}";return o.str();
    }
};
inline TextureQualityTelemetry textureQualityTelemetry;
}
