#pragma once
#include "FrozenCameraPipeline.h"
#include <cerrno>
#include <cstring>
#include <vector>

namespace rdr95 {
// Cold physical-device query only. This does not enable any device feature.
// The factory clones only exact successfully observed native layouts; the new
// camera layout itself uses a static UBO with no update-after-bind flags.
struct CameraDeviceLimits {
    using Limits=rdrvr::engine::FrozenCameraPipelineFactory::Limits;
    struct Api {
        PFN_vkEnumerateDeviceExtensionProperties extensions{};
        PFN_vkGetPhysicalDeviceProperties2 properties2{};
    };
    static Limits capture(VkPhysicalDevice device,const VkPhysicalDeviceProperties& properties,
                          std::uint32_t applicationApiVersion,Api api) noexcept {
        struct Restore {int value=errno;~Restore(){errno=value;}} preserve;
        const auto& p=properties.limits;
        Limits out{p.maxBoundDescriptorSets,p.maxPerStageDescriptorUniformBuffers,
                   p.maxPerStageResources,p.maxDescriptorSetUniformBuffers};
        out.maxFragmentUniforms=p.maxPerStageDescriptorUniformBuffers;
        out.maxFragmentResources=p.maxPerStageResources;
        if(!device||!api.properties2||VK_API_VERSION_VARIANT(properties.apiVersion)!=0||
           VK_API_VERSION_VARIANT(applicationApiVersion)!=0||
           properties.apiVersion<VK_API_VERSION_1_1||applicationApiVersion<VK_API_VERSION_1_1)return out;
        try {
            // A newer physical device does not upgrade the application's API.
            // A 1.1 instance must use advertised EXT support even on 1.2+ GPUs.
            bool supported=properties.apiVersion>=VK_API_VERSION_1_2&&applicationApiVersion>=VK_API_VERSION_1_2;
            if(!supported&&api.extensions){
                std::uint32_t count{};
                if(api.extensions(device,nullptr,&count,nullptr)==VK_SUCCESS&&count&&count<=1024){
                    std::vector<VkExtensionProperties> entries(count);
                    const auto capacity=count;
                    if(api.extensions(device,nullptr,&count,entries.data())==VK_SUCCESS&&count<=capacity)
                        for(std::uint32_t i=0;i<count;++i)
                            supported|=std::strncmp(entries[i].extensionName,VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
                                                    VK_MAX_EXTENSION_NAME_SIZE)==0;
                }
            }
            if(!supported)return out;
            VkPhysicalDeviceDescriptorIndexingProperties indexing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
            VkPhysicalDeviceProperties2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};query.pNext=&indexing;
            api.properties2(device,&query);
            // A mismatched provider/device cannot certify limits for this owner.
            if(query.properties.apiVersion!=properties.apiVersion||query.properties.vendorID!=properties.vendorID||
               query.properties.deviceID!=properties.deviceID)return out;
            out.maxVertexUpdateAfterBindUniforms=indexing.maxPerStageDescriptorUpdateAfterBindUniformBuffers;
            out.maxVertexUpdateAfterBindResources=indexing.maxPerStageUpdateAfterBindResources;
            out.maxSetUpdateAfterBindUniforms=indexing.maxDescriptorSetUpdateAfterBindUniformBuffers;
            out.maxFragmentUpdateAfterBindUniforms=indexing.maxPerStageDescriptorUpdateAfterBindUniformBuffers;
            out.maxFragmentUpdateAfterBindResources=indexing.maxPerStageUpdateAfterBindResources;
        }catch(...){} // Ordinary limits remain useful; unknown UAB stays refused.
        return out;
    }
};
}
