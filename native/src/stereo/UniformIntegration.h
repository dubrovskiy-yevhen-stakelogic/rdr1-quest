#pragma once
#include "CameraIntegration.h"
#include "UniformCaptureControl.h"
#include "OrdinaryMainStartupRequest.h"

namespace rdrvr::engine {
// PRIVATE lifetime plumbing. These own only snapshots/resources; no Producer
// is constructed here, at device creation, or in the generic draw wrappers.
inline bool uniformRecordingThread()noexcept{return cameraOwnsRecordingThread();}
inline bool uniformSelectedMain()noexcept{return rdr95::CameraMainRoute::selectedMain();}
inline rdr95::CommandUniformArena uniformArena;
inline rdr95::CommandUniformSets uniformSets;
inline rdr95::UniformCaptureSink uniformSink(uniformArena,uniformSets,uniformRecordingThread);
inline rdr95::UniformCaptureControl uniformCapture(uniformSink,{uniformRecordingThread,uniformSelectedMain});
inline rdr95::OrdinaryPacketCapture ordinaryCapture(indexedObservation,uniformCapture,uniformSink,uniformRecordingThread);
// No completed-consumer configuration here: resource initialization and
// trace-hook readiness alone are not object replay/admission authority.
inline std::atomic<VkDevice> uniformOwnerDevice{};
inline std::atomic<bool> uniformInitializationAttempted{};

inline void initializeUniformResources(VkPhysicalDevice physical,VkDevice device)noexcept {
    if constexpr(!ordinaryMainStartupRequested())return;
    const int saved=errno;struct Restore{int value;~Restore(){errno=value;}} keep{saved};
    if(!physical||!device||!cameraRuntime.ready()||cameraOwnerDevice.load(std::memory_order_acquire)!=device||
       uniformInitializationAttempted.exchange(true,std::memory_order_acq_rel))return;
    uniformOwnerDevice.store(device,std::memory_order_release);
    // Loader exports bypass engine dispatch slots and all capture observers.
    const rdr95::CommandUniformArena::Api buffers{::vkGetPhysicalDeviceProperties,::vkGetPhysicalDeviceMemoryProperties,
        ::vkCreateBuffer,::vkGetBufferMemoryRequirements,::vkAllocateMemory,::vkBindBufferMemory,
        ::vkMapMemory,::vkFlushMappedMemoryRanges,::vkUnmapMemory,::vkDestroyBuffer,::vkFreeMemory};
    const rdr95::CommandUniformSets::Api descriptors{::vkCreateDescriptorSetLayout,::vkDestroyDescriptorSetLayout,
        ::vkCreateDescriptorPool,::vkDestroyDescriptorPool,::vkAllocateDescriptorSets,::vkUpdateDescriptorSets};
    if(!uniformArena.initialize(physical,device,buffers,{})||!uniformSets.initialize(device,descriptors)||
       !dynamicUniformDescriptors.observeCaptures(uniformCapture.observer())){
        // Initialization creates no GPU arena/descriptor allocations. Preserve
        // the tracked owner for actual device cleanup, but never arm capture.
        uniformCapture.disarm();return;
    }
    uniformCapture.resourcesReady(); // Resource readiness only; Producer absent.
}
inline void uniformDeviceDisarm(VkDevice device)noexcept {
    if(device&&uniformOwnerDevice.load(std::memory_order_acquire)==device){ordinaryCapture.disarm();uniformCapture.disarm();}
}
inline void uniformShutdownAfterIdle(VkDevice device,VkResult idle)noexcept {
    const int saved=errno;struct Restore{int value;~Restore(){errno=value;}} keep{saved};
    if(device!=uniformOwnerDevice.load(std::memory_order_acquire))return;
    ordinaryCapture.disarm();uniformCapture.disarm();
    if(uniformSets.shutdownAfterIdle(device,idle))uniformArena.shutdownAfterIdle(device,idle);
}
inline void uniformAbandonDevice(VkDevice device)noexcept {
    const int saved=errno;struct Restore{int value;~Restore(){errno=value;}} keep{saved};
    if(!device||device!=uniformOwnerDevice.load(std::memory_order_acquire))return;
    ordinaryCapture.disarm();uniformCapture.disarm();uniformSets.abandonDevice(device);uniformArena.abandonDevice(device);
    auto expected=device;uniformOwnerDevice.compare_exchange_strong(expected,VK_NULL_HANDLE,std::memory_order_acq_rel);
}
inline void uniformSubmitted(std::uint32_t count,const VkSubmitInfo* submissions,VkResult result)noexcept {
    const int saved=errno;struct Restore{int value;~Restore(){errno=value;}} keep{saved};
    if(!submissions||!uniformOwnerDevice.load(std::memory_order_acquire))return;
    // End/successful Submit retain immutable bytes. Only actual device loss
    // changes Arena authority; queue completion is never inferred here.
    for(std::uint32_t i=0;i<count;++i)if(submissions[i].pCommandBuffers)
        for(std::uint32_t j=0;j<submissions[i].commandBufferCount;++j)
            uniformArena.commandSubmitted(submissions[i].pCommandBuffers[j],result);
}
// Future exact ordinary-call recorder uses this lexical type plus current(...).
// It is intentionally NOT instantiated by the generic trace or Game hooks.
using UniformProducer=rdr95::UniformCaptureControl::Producer;
}
