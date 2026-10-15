#include "LayerInternal.h"
#include <cerrno>
namespace rdr2::layer28 {
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkAcquireDrmDisplayEXT(VkPhysicalDevice physicalDevice, int32_t drmFd, VkDisplayKHR display) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkAcquireDrmDisplayEXT>(i->next(i->handle,"vkAcquireDrmDisplayEXT"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, drmFd, display);
}
#if defined(VK_USE_PLATFORM_WIN32_KHR)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkAcquireFullScreenExclusiveModeEXT(VkDevice device, VkSwapchainKHR swapchain) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkAcquireFullScreenExclusiveModeEXT;require(pfn);return pfn(device, swapchain);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkAcquireFullScreenExclusiveModeEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkAcquireFullScreenExclusiveModeEXT");
 require(fn);
 return fn(device, swapchain);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* pAcquireInfo, uint32_t* pImageIndex) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkAcquireNextImage2KHR;require(pfn);return pfn(device, pAcquireInfo, pImageIndex);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkAcquireNextImage2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkAcquireNextImage2KHR");
 require(fn);
 return fn(device, pAcquireInfo, pImageIndex);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkAcquireNextImageKHR;require(pfn);return pfn(device, swapchain, timeout, semaphore, fence, pImageIndex);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkAcquireNextImageKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkAcquireNextImageKHR");
 require(fn);
 return fn(device, swapchain, timeout, semaphore, fence, pImageIndex);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkAcquirePerformanceConfigurationINTEL(VkDevice device, const VkPerformanceConfigurationAcquireInfoINTEL* pAcquireInfo, VkPerformanceConfigurationINTEL* pConfiguration) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkAcquirePerformanceConfigurationINTEL;require(pfn);return pfn(device, pAcquireInfo, pConfiguration);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkAcquirePerformanceConfigurationINTEL;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkAcquirePerformanceConfigurationINTEL");
 require(fn);
 return fn(device, pAcquireInfo, pConfiguration);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkAcquireProfilingLockKHR(VkDevice device, const VkAcquireProfilingLockInfoKHR* pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkAcquireProfilingLockKHR;require(pfn);return pfn(device, pInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkAcquireProfilingLockKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkAcquireProfilingLockKHR");
 require(fn);
 return fn(device, pInfo);
}
#if defined(VK_USE_PLATFORM_WIN32_KHR)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkAcquireWinrtDisplayNV(VkPhysicalDevice physicalDevice, VkDisplayKHR display) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkAcquireWinrtDisplayNV>(i->next(i->handle,"vkAcquireWinrtDisplayNV"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, display);
}
#endif
#if defined(VK_USE_PLATFORM_XLIB_XRANDR_EXT)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkAcquireXlibDisplayEXT(VkPhysicalDevice physicalDevice, Display* dpy, VkDisplayKHR display) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkAcquireXlibDisplayEXT>(i->next(i->handle,"vkAcquireXlibDisplayEXT"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, dpy, display);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkAllocateDescriptorSets(VkDevice device,const VkDescriptorSetAllocateInfo*info,VkDescriptorSet*out){
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkAllocateDescriptorSets;require(pfn);return pfn(device, info, out);}
 auto*d=deviceFor(device);auto fn=d->dispatch.vkAllocateDescriptorSets;require(fn);
 PoolOperation87 poolOperation87(d,info?info->descriptorPool:VkDescriptorPool{});
 const int incoming=errno;const bool observe82=tls.device==d&&tls.recording&&!tls.allocation82.seen&&tls.observationBeginSerial82!=UINT64_MAX;
 const auto observationSerial82=tls.observationBeginSerial82;
 if(observe82){tls.allocation82={};tls.allocation82.seen=1;if(!out)tls.allocation82.shape=13;}
 FreshDescriptor81 receipt;const bool shape=out&&descriptorAllocationShape81(d,info,receipt,observe82);
 errno=incoming;
 if(!shape){auto entry=enter(d,Worker::AccessKind::Resource,"vkAllocateDescriptorSets");
  if(info&&info->descriptorPool)retireDescriptorPool81(d,info->descriptorPool);
  errno=incoming;const auto result=fn(device,info,out);const int saved=errno;
  if(observe82&&tls.device==d&&tls.observationBeginSerial82==observationSerial82){auto&v=tls.allocation82;v.resultKnown=1;v.result=unsigned(result);
   v.setNonNull=result==VK_SUCCESS&&out&&*out?1u:0u;v.receipt=result!=VK_SUCCESS?3u:v.setNonNull?2u:4u;}
  errno=saved;return result;
 }
 ActivePreparation81 entry(d,"vkAllocateDescriptorSets");
 if(observe82&&tls.device==d&&tls.observationBeginSerial82==observationSerial82)tls.allocation82.entry=entry.entryObservation82();errno=incoming;
 const auto result=fn(device,info,out);const int saved=errno;const bool preserved=entry.complete();
 if(observe82&&tls.device==d&&tls.observationBeginSerial82==observationSerial82)tls.allocation82.completion=entry.completionObservation82();
 const auto typedBefore82=tls.descriptorAllocations81;
 if(result==VK_SUCCESS&&*out)allocatedDescriptor81(d,*out,receipt,preserved);
 else if(preserved){auto drain=enter(d,Worker::AccessKind::Resource,"descriptorAllocationFailure");}
 if(observe82&&tls.device==d&&tls.observationBeginSerial82==observationSerial82){auto&v=tls.allocation82;v.resultKnown=1;v.result=unsigned(result);
  v.setNonNull=result==VK_SUCCESS&&*out?1u:0u;v.receipt=result!=VK_SUCCESS?3u:!v.setNonNull?4u:tls.descriptorAllocations81>typedBefore82?1u:5u;}
 errno=saved;return result;
}

VKAPI_ATTR VkResult VKAPI_CALL wrap_vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* pAllocateInfo, const VkAllocationCallbacks* pAllocator, VkDeviceMemory* pMemory) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkAllocateMemory;require(pfn);return pfn(device, pAllocateInfo, pAllocator, pMemory);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkAllocateMemory;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkAllocateMemory");
 require(fn);
 return fn(device, pAllocateInfo, pAllocator, pMemory);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkAntiLagUpdateAMD(VkDevice device, const VkAntiLagDataAMD* pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkAntiLagUpdateAMD;require(pfn);pfn(device, pData);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkAntiLagUpdateAMD;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkAntiLagUpdateAMD");
 require(fn);
 fn(device, pData);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkBindAccelerationStructureMemoryNV(VkDevice device, uint32_t bindInfoCount, const VkBindAccelerationStructureMemoryInfoNV* pBindInfos) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkBindAccelerationStructureMemoryNV;require(pfn);return pfn(device, bindInfoCount, pBindInfos);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkBindAccelerationStructureMemoryNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkBindAccelerationStructureMemoryNV");
 require(fn);
 return fn(device, bindInfoCount, pBindInfos);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkBindBufferMemory;require(pfn);return pfn(device, buffer, memory, memoryOffset);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkBindBufferMemory;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkBindBufferMemory");
 require(fn);
 return fn(device, buffer, memory, memoryOffset);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkBindBufferMemory2(VkDevice device, uint32_t bindInfoCount, const VkBindBufferMemoryInfo* pBindInfos) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkBindBufferMemory2;require(pfn);return pfn(device, bindInfoCount, pBindInfos);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkBindBufferMemory2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkBindBufferMemory2");
 require(fn);
 return fn(device, bindInfoCount, pBindInfos);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkBindBufferMemory2KHR(VkDevice device, uint32_t bindInfoCount, const VkBindBufferMemoryInfo* pBindInfos) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkBindBufferMemory2KHR;require(pfn);return pfn(device, bindInfoCount, pBindInfos);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkBindBufferMemory2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkBindBufferMemory2KHR");
 require(fn);
 return fn(device, bindInfoCount, pBindInfos);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkBindDataGraphPipelineSessionMemoryARM(VkDevice device, uint32_t bindInfoCount, const VkBindDataGraphPipelineSessionMemoryInfoARM* pBindInfos) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkBindDataGraphPipelineSessionMemoryARM;require(pfn);return pfn(device, bindInfoCount, pBindInfos);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkBindDataGraphPipelineSessionMemoryARM;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkBindDataGraphPipelineSessionMemoryARM");
 require(fn);
 return fn(device, bindInfoCount, pBindInfos);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkBindImageMemory;require(pfn);return pfn(device, image, memory, memoryOffset);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkBindImageMemory;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkBindImageMemory");
 require(fn);
 return fn(device, image, memory, memoryOffset);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkBindImageMemory2(VkDevice device, uint32_t bindInfoCount, const VkBindImageMemoryInfo* pBindInfos) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkBindImageMemory2;require(pfn);return pfn(device, bindInfoCount, pBindInfos);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkBindImageMemory2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkBindImageMemory2");
 require(fn);
 return fn(device, bindInfoCount, pBindInfos);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkBindImageMemory2KHR(VkDevice device, uint32_t bindInfoCount, const VkBindImageMemoryInfo* pBindInfos) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkBindImageMemory2KHR;require(pfn);return pfn(device, bindInfoCount, pBindInfos);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkBindImageMemory2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkBindImageMemory2KHR");
 require(fn);
 return fn(device, bindInfoCount, pBindInfos);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkBindOpticalFlowSessionImageNV(VkDevice device, VkOpticalFlowSessionNV session, VkOpticalFlowSessionBindingPointNV bindingPoint, VkImageView view, VkImageLayout layout) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkBindOpticalFlowSessionImageNV;require(pfn);return pfn(device, session, bindingPoint, view, layout);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkBindOpticalFlowSessionImageNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkBindOpticalFlowSessionImageNV");
 require(fn);
 return fn(device, session, bindingPoint, view, layout);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkBindTensorMemoryARM(VkDevice device, uint32_t bindInfoCount, const VkBindTensorMemoryInfoARM* pBindInfos) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkBindTensorMemoryARM;require(pfn);return pfn(device, bindInfoCount, pBindInfos);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkBindTensorMemoryARM;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkBindTensorMemoryARM");
 require(fn);
 return fn(device, bindInfoCount, pBindInfos);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkBindVideoSessionMemoryKHR(VkDevice device, VkVideoSessionKHR videoSession, uint32_t bindSessionMemoryInfoCount, const VkBindVideoSessionMemoryInfoKHR* pBindSessionMemoryInfos) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkBindVideoSessionMemoryKHR;require(pfn);return pfn(device, videoSession, bindSessionMemoryInfoCount, pBindSessionMemoryInfos);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkBindVideoSessionMemoryKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkBindVideoSessionMemoryKHR");
 require(fn);
 return fn(device, videoSession, bindSessionMemoryInfoCount, pBindSessionMemoryInfos);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkBuildAccelerationStructuresKHR(VkDevice                                           device, VkDeferredOperationKHR deferredOperation, uint32_t infoCount, const VkAccelerationStructureBuildGeometryInfoKHR* pInfos, const VkAccelerationStructureBuildRangeInfoKHR* const* ppBuildRangeInfos) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkBuildAccelerationStructuresKHR;require(pfn);return pfn(device, deferredOperation, infoCount, pInfos, ppBuildRangeInfos);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkBuildAccelerationStructuresKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkBuildAccelerationStructuresKHR");
 require(fn);
 return fn(device, deferredOperation, infoCount, pInfos, ppBuildRangeInfos);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkBuildMicromapsEXT(VkDevice                                      device, VkDeferredOperationKHR        deferredOperation, uint32_t                                      infoCount, const VkMicromapBuildInfoEXT* pInfos) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkBuildMicromapsEXT;require(pfn);return pfn(device, deferredOperation, infoCount, pInfos);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkBuildMicromapsEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkBuildMicromapsEXT");
 require(fn);
 return fn(device, deferredOperation, infoCount, pInfos);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkClearShaderInstrumentationMetricsARM(VkDevice device, VkShaderInstrumentationARM instrumentation) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkClearShaderInstrumentationMetricsARM;require(pfn);pfn(device, instrumentation);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkClearShaderInstrumentationMetricsARM;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkClearShaderInstrumentationMetricsARM");
 require(fn);
 fn(device, instrumentation);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBeginConditionalRendering2EXT(VkCommandBuffer   commandBuffer, const VkConditionalRenderingBeginInfo2EXT* pConditionalRenderingBegin) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBeginConditionalRendering2EXT;require(pfn);pfn(commandBuffer, pConditionalRenderingBegin);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBeginConditionalRendering2EXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBeginConditionalRendering2EXT");
 require(fn);
 fn(commandBuffer, pConditionalRenderingBegin);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBeginConditionalRenderingEXT(VkCommandBuffer commandBuffer, const VkConditionalRenderingBeginInfoEXT* pConditionalRenderingBegin) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBeginConditionalRenderingEXT;require(pfn);pfn(commandBuffer, pConditionalRenderingBegin);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBeginConditionalRenderingEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBeginConditionalRenderingEXT");
 require(fn);
 fn(commandBuffer, pConditionalRenderingBegin);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBeginCustomResolveEXT(VkCommandBuffer commandBuffer, const VkBeginCustomResolveInfoEXT* pBeginCustomResolveInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBeginCustomResolveEXT;require(pfn);pfn(commandBuffer, pBeginCustomResolveInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBeginCustomResolveEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBeginCustomResolveEXT");
 require(fn);
 fn(commandBuffer, pBeginCustomResolveInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBeginDebugUtilsLabelEXT(VkCommandBuffer commandBuffer, const VkDebugUtilsLabelEXT* pLabelInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBeginDebugUtilsLabelEXT;require(pfn);pfn(commandBuffer, pLabelInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBeginDebugUtilsLabelEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBeginDebugUtilsLabelEXT");
 require(fn);
 fn(commandBuffer, pLabelInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCmdBeginGpaSampleAMD(VkCommandBuffer commandBuffer, VkGpaSessionAMD                   gpaSession, const VkGpaSampleBeginInfoAMD*    pGpaSampleBeginInfo, uint32_t*                         pSampleID) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBeginGpaSampleAMD;require(pfn);return pfn(commandBuffer, gpaSession, pGpaSampleBeginInfo, pSampleID);}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBeginGpaSampleAMD;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBeginGpaSampleAMD");
 require(fn);
 return fn(commandBuffer, gpaSession, pGpaSampleBeginInfo, pSampleID);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCmdBeginGpaSessionAMD(VkCommandBuffer commandBuffer, VkGpaSessionAMD                   gpaSession) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBeginGpaSessionAMD;require(pfn);return pfn(commandBuffer, gpaSession);}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBeginGpaSessionAMD;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBeginGpaSessionAMD");
 require(fn);
 return fn(commandBuffer, gpaSession);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBeginPerTileExecutionQCOM(VkCommandBuffer commandBuffer, const VkPerTileBeginInfoQCOM* pPerTileBeginInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBeginPerTileExecutionQCOM;require(pfn);pfn(commandBuffer, pPerTileBeginInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBeginPerTileExecutionQCOM;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBeginPerTileExecutionQCOM");
 require(fn);
 fn(commandBuffer, pPerTileBeginInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBeginQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query, VkQueryControlFlags flags) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBeginQuery;require(pfn);pfn(commandBuffer, queryPool, query, flags);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBeginQuery;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBeginQuery");
 require(fn);
 fn(commandBuffer, queryPool, query, flags);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBeginQueryIndexedEXT(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query, VkQueryControlFlags flags, uint32_t index) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBeginQueryIndexedEXT;require(pfn);pfn(commandBuffer, queryPool, query, flags, index);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBeginQueryIndexedEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBeginQueryIndexedEXT");
 require(fn);
 fn(commandBuffer, queryPool, query, flags, index);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBeginRenderPass(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* pRenderPassBegin, VkSubpassContents contents) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBeginRenderPass;require(pfn);pfn(commandBuffer, pRenderPassBegin, contents);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBeginRenderPass;
 if(t.pending76.command){const int savedErrno=errno;firstAdmissionBeforeCommand77(t,d,commandBuffer,"vkCmdBeginRenderPass");errno=savedErrno;}
 if (t.command==commandBuffer && t.recording && d->worker) { if(d->worker->beginPass(t.recording, commandBuffer, pRenderPassBegin, contents)) return; }
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBeginRenderPass");
 require(fn);
 fn(commandBuffer, pRenderPassBegin, contents);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBeginRenderPass2(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo*      pRenderPassBegin, const VkSubpassBeginInfo*      pSubpassBeginInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBeginRenderPass2;require(pfn);pfn(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBeginRenderPass2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBeginRenderPass2");
 require(fn);
 fn(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBeginRenderPass2KHR(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo*      pRenderPassBegin, const VkSubpassBeginInfo*      pSubpassBeginInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBeginRenderPass2KHR;require(pfn);pfn(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBeginRenderPass2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBeginRenderPass2KHR");
 require(fn);
 fn(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBeginRendering(VkCommandBuffer                   commandBuffer, const VkRenderingInfo*                              pRenderingInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBeginRendering;require(pfn);pfn(commandBuffer, pRenderingInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBeginRendering;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBeginRendering");
 require(fn);
 fn(commandBuffer, pRenderingInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBeginRenderingKHR(VkCommandBuffer                   commandBuffer, const VkRenderingInfo*                              pRenderingInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBeginRenderingKHR;require(pfn);pfn(commandBuffer, pRenderingInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBeginRenderingKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBeginRenderingKHR");
 require(fn);
 fn(commandBuffer, pRenderingInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBeginShaderInstrumentationARM(VkCommandBuffer commandBuffer, VkShaderInstrumentationARM instrumentation) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBeginShaderInstrumentationARM;require(pfn);pfn(commandBuffer, instrumentation);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBeginShaderInstrumentationARM;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBeginShaderInstrumentationARM");
 require(fn);
 fn(commandBuffer, instrumentation);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBeginTransformFeedback2EXT(VkCommandBuffer   commandBuffer, uint32_t                            firstCounterRange, uint32_t            counterRangeCount, const VkBindTransformFeedbackBuffer2InfoEXT* pCounterInfos) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBeginTransformFeedback2EXT;require(pfn);pfn(commandBuffer, firstCounterRange, counterRangeCount, pCounterInfos);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBeginTransformFeedback2EXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBeginTransformFeedback2EXT");
 require(fn);
 fn(commandBuffer, firstCounterRange, counterRangeCount, pCounterInfos);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBeginTransformFeedbackEXT(VkCommandBuffer commandBuffer, uint32_t firstCounterBuffer, uint32_t counterBufferCount, const VkBuffer* pCounterBuffers, const VkDeviceSize* pCounterBufferOffsets) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBeginTransformFeedbackEXT;require(pfn);pfn(commandBuffer, firstCounterBuffer, counterBufferCount, pCounterBuffers, pCounterBufferOffsets);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBeginTransformFeedbackEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBeginTransformFeedbackEXT");
 require(fn);
 fn(commandBuffer, firstCounterBuffer, counterBufferCount, pCounterBuffers, pCounterBufferOffsets);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBeginVideoCodingKHR(VkCommandBuffer commandBuffer, const VkVideoBeginCodingInfoKHR* pBeginInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBeginVideoCodingKHR;require(pfn);pfn(commandBuffer, pBeginInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBeginVideoCodingKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBeginVideoCodingKHR");
 require(fn);
 fn(commandBuffer, pBeginInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBindDescriptorBufferEmbeddedSamplers2EXT(VkCommandBuffer commandBuffer, const VkBindDescriptorBufferEmbeddedSamplersInfoEXT* pBindDescriptorBufferEmbeddedSamplersInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBindDescriptorBufferEmbeddedSamplers2EXT;require(pfn);pfn(commandBuffer, pBindDescriptorBufferEmbeddedSamplersInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBindDescriptorBufferEmbeddedSamplers2EXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBindDescriptorBufferEmbeddedSamplers2EXT");
 require(fn);
 fn(commandBuffer, pBindDescriptorBufferEmbeddedSamplersInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBindDescriptorBufferEmbeddedSamplersEXT(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout, uint32_t set) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBindDescriptorBufferEmbeddedSamplersEXT;require(pfn);pfn(commandBuffer, pipelineBindPoint, layout, set);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBindDescriptorBufferEmbeddedSamplersEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBindDescriptorBufferEmbeddedSamplersEXT");
 require(fn);
 fn(commandBuffer, pipelineBindPoint, layout, set);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBindDescriptorBuffersEXT(VkCommandBuffer commandBuffer, uint32_t bufferCount, const VkDescriptorBufferBindingInfoEXT* pBindingInfos) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBindDescriptorBuffersEXT;require(pfn);pfn(commandBuffer, bufferCount, pBindingInfos);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBindDescriptorBuffersEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBindDescriptorBuffersEXT");
 require(fn);
 fn(commandBuffer, bufferCount, pBindingInfos);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBindDescriptorSets(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout, uint32_t firstSet, uint32_t descriptorSetCount, const VkDescriptorSet* pDescriptorSets, uint32_t dynamicOffsetCount, const uint32_t* pDynamicOffsets) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBindDescriptorSets;require(pfn);pfn(commandBuffer, pipelineBindPoint, layout, firstSet, descriptorSetCount, pDescriptorSets, dynamicOffsetCount, pDynamicOffsets);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBindDescriptorSets;
 if(t.pending76.command){const int savedErrno=errno;firstAdmissionBeforeCommand77(t,d,commandBuffer,"vkCmdBindDescriptorSets");errno=savedErrno;}
 if (t.command==commandBuffer && t.recording && d->worker && coveredFreshDescriptorBind89(d,descriptorSetCount,pDescriptorSets) && consumeFreshDescriptorBind81(d,descriptorSetCount,pDescriptorSets,dynamicOffsetCount,pDynamicOffsets)) { if(d->worker->descriptors(t.recording, commandBuffer, pipelineBindPoint, layout, firstSet, descriptorSetCount, pDescriptorSets, dynamicOffsetCount, pDynamicOffsets)) return; }
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBindDescriptorSets",commandBuffer);
 require(fn);
 fn(commandBuffer, pipelineBindPoint, layout, firstSet, descriptorSetCount, pDescriptorSets, dynamicOffsetCount, pDynamicOffsets);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBindDescriptorSets2(VkCommandBuffer commandBuffer, const VkBindDescriptorSetsInfo*   pBindDescriptorSetsInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBindDescriptorSets2;require(pfn);pfn(commandBuffer, pBindDescriptorSetsInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBindDescriptorSets2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBindDescriptorSets2");
 require(fn);
 fn(commandBuffer, pBindDescriptorSetsInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBindDescriptorSets2KHR(VkCommandBuffer commandBuffer, const VkBindDescriptorSetsInfo*   pBindDescriptorSetsInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBindDescriptorSets2KHR;require(pfn);pfn(commandBuffer, pBindDescriptorSetsInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBindDescriptorSets2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBindDescriptorSets2KHR");
 require(fn);
 fn(commandBuffer, pBindDescriptorSetsInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBindIndexBuffer(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkIndexType indexType) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBindIndexBuffer;require(pfn);pfn(commandBuffer, buffer, offset, indexType);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBindIndexBuffer;
 if(t.pending76.command){const int savedErrno=errno;firstAdmissionBeforeCommand77(t,d,commandBuffer,"vkCmdBindIndexBuffer");errno=savedErrno;}
 if (t.command==commandBuffer && t.recording && d->worker) { if(d->worker->index(t.recording, commandBuffer, buffer, offset, indexType)) return; }
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBindIndexBuffer",commandBuffer);
 require(fn);
 fn(commandBuffer, buffer, offset, indexType);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBindIndexBuffer2(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, VkIndexType indexType) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBindIndexBuffer2;require(pfn);pfn(commandBuffer, buffer, offset, size, indexType);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBindIndexBuffer2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBindIndexBuffer2");
 require(fn);
 fn(commandBuffer, buffer, offset, size, indexType);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBindIndexBuffer2KHR(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, VkIndexType indexType) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBindIndexBuffer2KHR;require(pfn);pfn(commandBuffer, buffer, offset, size, indexType);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBindIndexBuffer2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBindIndexBuffer2KHR");
 require(fn);
 fn(commandBuffer, buffer, offset, size, indexType);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBindIndexBuffer3KHR(VkCommandBuffer   commandBuffer, const VkBindIndexBuffer3InfoKHR*    pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBindIndexBuffer3KHR;require(pfn);pfn(commandBuffer, pInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBindIndexBuffer3KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBindIndexBuffer3KHR");
 require(fn);
 fn(commandBuffer, pInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBindInvocationMaskHUAWEI(VkCommandBuffer commandBuffer, VkImageView imageView, VkImageLayout imageLayout) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBindInvocationMaskHUAWEI;require(pfn);pfn(commandBuffer, imageView, imageLayout);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBindInvocationMaskHUAWEI;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBindInvocationMaskHUAWEI");
 require(fn);
 fn(commandBuffer, imageView, imageLayout);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBindPipeline(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipeline pipeline) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBindPipeline;require(pfn);pfn(commandBuffer, pipelineBindPoint, pipeline);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBindPipeline;
 if(t.pending76.command){const int savedErrno=errno;firstAdmissionBeforeCommand77(t,d,commandBuffer,"vkCmdBindPipeline");errno=savedErrno;}
 if (t.command==commandBuffer && t.recording && d->worker) { if(d->worker->pipeline(t.recording, commandBuffer, pipelineBindPoint, pipeline)) return; }
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBindPipeline",commandBuffer);
 require(fn);
 fn(commandBuffer, pipelineBindPoint, pipeline);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBindPipelineShaderGroupNV(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipeline pipeline, uint32_t groupIndex) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBindPipelineShaderGroupNV;require(pfn);pfn(commandBuffer, pipelineBindPoint, pipeline, groupIndex);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBindPipelineShaderGroupNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBindPipelineShaderGroupNV");
 require(fn);
 fn(commandBuffer, pipelineBindPoint, pipeline, groupIndex);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBindResourceHeapEXT(VkCommandBuffer                   commandBuffer, const VkBindHeapInfoEXT*                            pBindInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBindResourceHeapEXT;require(pfn);pfn(commandBuffer, pBindInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBindResourceHeapEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBindResourceHeapEXT");
 require(fn);
 fn(commandBuffer, pBindInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBindSamplerHeapEXT(VkCommandBuffer                   commandBuffer, const VkBindHeapInfoEXT*                            pBindInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBindSamplerHeapEXT;require(pfn);pfn(commandBuffer, pBindInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBindSamplerHeapEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBindSamplerHeapEXT");
 require(fn);
 fn(commandBuffer, pBindInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBindShadersEXT(VkCommandBuffer commandBuffer, uint32_t stageCount, const VkShaderStageFlagBits* pStages, const VkShaderEXT* pShaders) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBindShadersEXT;require(pfn);pfn(commandBuffer, stageCount, pStages, pShaders);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBindShadersEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBindShadersEXT");
 require(fn);
 fn(commandBuffer, stageCount, pStages, pShaders);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBindShadingRateImageNV(VkCommandBuffer commandBuffer, VkImageView imageView, VkImageLayout imageLayout) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBindShadingRateImageNV;require(pfn);pfn(commandBuffer, imageView, imageLayout);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBindShadingRateImageNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBindShadingRateImageNV");
 require(fn);
 fn(commandBuffer, imageView, imageLayout);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBindTileMemoryQCOM(VkCommandBuffer commandBuffer, const VkTileMemoryBindInfoQCOM* pTileMemoryBindInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBindTileMemoryQCOM;require(pfn);pfn(commandBuffer, pTileMemoryBindInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBindTileMemoryQCOM;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBindTileMemoryQCOM");
 require(fn);
 fn(commandBuffer, pTileMemoryBindInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBindTransformFeedbackBuffers2EXT(VkCommandBuffer   commandBuffer, uint32_t                            firstBinding, uint32_t                            bindingCount, const VkBindTransformFeedbackBuffer2InfoEXT* pBindingInfos) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBindTransformFeedbackBuffers2EXT;require(pfn);pfn(commandBuffer, firstBinding, bindingCount, pBindingInfos);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBindTransformFeedbackBuffers2EXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBindTransformFeedbackBuffers2EXT");
 require(fn);
 fn(commandBuffer, firstBinding, bindingCount, pBindingInfos);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBindTransformFeedbackBuffersEXT(VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount, const VkBuffer* pBuffers, const VkDeviceSize* pOffsets, const VkDeviceSize* pSizes) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBindTransformFeedbackBuffersEXT;require(pfn);pfn(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets, pSizes);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBindTransformFeedbackBuffersEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBindTransformFeedbackBuffersEXT");
 require(fn);
 fn(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets, pSizes);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBindVertexBuffers(VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount, const VkBuffer* pBuffers, const VkDeviceSize* pOffsets) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBindVertexBuffers;require(pfn);pfn(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBindVertexBuffers;
 if(t.pending76.command){const int savedErrno=errno;firstAdmissionBeforeCommand77(t,d,commandBuffer,"vkCmdBindVertexBuffers");errno=savedErrno;}
 if (t.command==commandBuffer && t.recording && d->worker) { if(d->worker->vertices(t.recording, commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets)) return; }
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBindVertexBuffers",commandBuffer);
 require(fn);
 fn(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBindVertexBuffers2(VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount, const VkBuffer* pBuffers, const VkDeviceSize* pOffsets, const VkDeviceSize* pSizes, const VkDeviceSize* pStrides) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBindVertexBuffers2;require(pfn);pfn(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets, pSizes, pStrides);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBindVertexBuffers2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBindVertexBuffers2");
 require(fn);
 fn(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets, pSizes, pStrides);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBindVertexBuffers2EXT(VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount, const VkBuffer* pBuffers, const VkDeviceSize* pOffsets, const VkDeviceSize* pSizes, const VkDeviceSize* pStrides) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBindVertexBuffers2EXT;require(pfn);pfn(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets, pSizes, pStrides);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBindVertexBuffers2EXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBindVertexBuffers2EXT");
 require(fn);
 fn(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets, pSizes, pStrides);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBindVertexBuffers3KHR(VkCommandBuffer commandBuffer, uint32_t                            firstBinding, uint32_t                            bindingCount, const VkBindVertexBuffer3InfoKHR* pBindingInfos) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBindVertexBuffers3KHR;require(pfn);pfn(commandBuffer, firstBinding, bindingCount, pBindingInfos);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBindVertexBuffers3KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBindVertexBuffers3KHR");
 require(fn);
 fn(commandBuffer, firstBinding, bindingCount, pBindingInfos);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBlitImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout, VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageBlit* pRegions, VkFilter filter) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBlitImage;require(pfn);pfn(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions, filter);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBlitImage;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBlitImage");
 require(fn);
 fn(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions, filter);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBlitImage2(VkCommandBuffer commandBuffer, const VkBlitImageInfo2* pBlitImageInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBlitImage2;require(pfn);pfn(commandBuffer, pBlitImageInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBlitImage2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBlitImage2");
 require(fn);
 fn(commandBuffer, pBlitImageInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBlitImage2KHR(VkCommandBuffer commandBuffer, const VkBlitImageInfo2* pBlitImageInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBlitImage2KHR;require(pfn);pfn(commandBuffer, pBlitImageInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBlitImage2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBlitImage2KHR");
 require(fn);
 fn(commandBuffer, pBlitImageInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBuildAccelerationStructureNV(VkCommandBuffer commandBuffer, const VkAccelerationStructureInfoNV* pInfo, VkBuffer instanceData, VkDeviceSize instanceOffset, VkBool32 update, VkAccelerationStructureNV dst, VkAccelerationStructureNV src, VkBuffer scratch, VkDeviceSize scratchOffset) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBuildAccelerationStructureNV;require(pfn);pfn(commandBuffer, pInfo, instanceData, instanceOffset, update, dst, src, scratch, scratchOffset);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBuildAccelerationStructureNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBuildAccelerationStructureNV");
 require(fn);
 fn(commandBuffer, pInfo, instanceData, instanceOffset, update, dst, src, scratch, scratchOffset);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBuildAccelerationStructuresIndirectKHR(VkCommandBuffer                  commandBuffer, uint32_t                                           infoCount, const VkAccelerationStructureBuildGeometryInfoKHR* pInfos, const VkDeviceAddress*             pIndirectDeviceAddresses, const uint32_t*                    pIndirectStrides, const uint32_t* const*             ppMaxPrimitiveCounts) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBuildAccelerationStructuresIndirectKHR;require(pfn);pfn(commandBuffer, infoCount, pInfos, pIndirectDeviceAddresses, pIndirectStrides, ppMaxPrimitiveCounts);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBuildAccelerationStructuresIndirectKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBuildAccelerationStructuresIndirectKHR");
 require(fn);
 fn(commandBuffer, infoCount, pInfos, pIndirectDeviceAddresses, pIndirectStrides, ppMaxPrimitiveCounts);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBuildAccelerationStructuresKHR(VkCommandBuffer                                    commandBuffer, uint32_t infoCount, const VkAccelerationStructureBuildGeometryInfoKHR* pInfos, const VkAccelerationStructureBuildRangeInfoKHR* const* ppBuildRangeInfos) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBuildAccelerationStructuresKHR;require(pfn);pfn(commandBuffer, infoCount, pInfos, ppBuildRangeInfos);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBuildAccelerationStructuresKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBuildAccelerationStructuresKHR");
 require(fn);
 fn(commandBuffer, infoCount, pInfos, ppBuildRangeInfos);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBuildClusterAccelerationStructureIndirectNV(VkCommandBuffer                     commandBuffer, const VkClusterAccelerationStructureCommandsInfoNV*  pCommandInfos) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBuildClusterAccelerationStructureIndirectNV;require(pfn);pfn(commandBuffer, pCommandInfos);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBuildClusterAccelerationStructureIndirectNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBuildClusterAccelerationStructureIndirectNV");
 require(fn);
 fn(commandBuffer, pCommandInfos);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBuildMicromapsEXT(VkCommandBuffer             commandBuffer, uint32_t                                      infoCount, const VkMicromapBuildInfoEXT* pInfos) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBuildMicromapsEXT;require(pfn);pfn(commandBuffer, infoCount, pInfos);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBuildMicromapsEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBuildMicromapsEXT");
 require(fn);
 fn(commandBuffer, infoCount, pInfos);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdBuildPartitionedAccelerationStructuresNV(VkCommandBuffer                     commandBuffer, const VkBuildPartitionedAccelerationStructureInfoNV*  pBuildInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdBuildPartitionedAccelerationStructuresNV;require(pfn);pfn(commandBuffer, pBuildInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdBuildPartitionedAccelerationStructuresNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdBuildPartitionedAccelerationStructuresNV");
 require(fn);
 fn(commandBuffer, pBuildInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdClearAttachments(VkCommandBuffer commandBuffer, uint32_t attachmentCount, const VkClearAttachment* pAttachments, uint32_t rectCount, const VkClearRect* pRects) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdClearAttachments;require(pfn);pfn(commandBuffer, attachmentCount, pAttachments, rectCount, pRects);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdClearAttachments;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdClearAttachments");
 require(fn);
 fn(commandBuffer, attachmentCount, pAttachments, rectCount, pRects);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdClearColorImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout, const VkClearColorValue* pColor, uint32_t rangeCount, const VkImageSubresourceRange* pRanges) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdClearColorImage;require(pfn);pfn(commandBuffer, image, imageLayout, pColor, rangeCount, pRanges);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdClearColorImage;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdClearColorImage");
 require(fn);
 fn(commandBuffer, image, imageLayout, pColor, rangeCount, pRanges);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdClearDepthStencilImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout, const VkClearDepthStencilValue* pDepthStencil, uint32_t rangeCount, const VkImageSubresourceRange* pRanges) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdClearDepthStencilImage;require(pfn);pfn(commandBuffer, image, imageLayout, pDepthStencil, rangeCount, pRanges);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdClearDepthStencilImage;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdClearDepthStencilImage");
 require(fn);
 fn(commandBuffer, image, imageLayout, pDepthStencil, rangeCount, pRanges);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdControlVideoCodingKHR(VkCommandBuffer commandBuffer, const VkVideoCodingControlInfoKHR* pCodingControlInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdControlVideoCodingKHR;require(pfn);pfn(commandBuffer, pCodingControlInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdControlVideoCodingKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdControlVideoCodingKHR");
 require(fn);
 fn(commandBuffer, pCodingControlInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdConvertCooperativeVectorMatrixNV(VkCommandBuffer commandBuffer, uint32_t infoCount, const VkConvertCooperativeVectorMatrixInfoNV* pInfos) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdConvertCooperativeVectorMatrixNV;require(pfn);pfn(commandBuffer, infoCount, pInfos);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdConvertCooperativeVectorMatrixNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdConvertCooperativeVectorMatrixNV");
 require(fn);
 fn(commandBuffer, infoCount, pInfos);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyAccelerationStructureKHR(VkCommandBuffer commandBuffer, const VkCopyAccelerationStructureInfoKHR* pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyAccelerationStructureKHR;require(pfn);pfn(commandBuffer, pInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyAccelerationStructureKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyAccelerationStructureKHR");
 require(fn);
 fn(commandBuffer, pInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyAccelerationStructureNV(VkCommandBuffer commandBuffer, VkAccelerationStructureNV dst, VkAccelerationStructureNV src, VkCopyAccelerationStructureModeKHR mode) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyAccelerationStructureNV;require(pfn);pfn(commandBuffer, dst, src, mode);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyAccelerationStructureNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyAccelerationStructureNV");
 require(fn);
 fn(commandBuffer, dst, src, mode);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyAccelerationStructureToMemoryKHR(VkCommandBuffer commandBuffer, const VkCopyAccelerationStructureToMemoryInfoKHR* pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyAccelerationStructureToMemoryKHR;require(pfn);pfn(commandBuffer, pInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyAccelerationStructureToMemoryKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyAccelerationStructureToMemoryKHR");
 require(fn);
 fn(commandBuffer, pInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyBuffer(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer, uint32_t regionCount, const VkBufferCopy* pRegions) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyBuffer;require(pfn);pfn(commandBuffer, srcBuffer, dstBuffer, regionCount, pRegions);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyBuffer;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyBuffer");
 require(fn);
 fn(commandBuffer, srcBuffer, dstBuffer, regionCount, pRegions);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyBuffer2(VkCommandBuffer commandBuffer, const VkCopyBufferInfo2* pCopyBufferInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyBuffer2;require(pfn);pfn(commandBuffer, pCopyBufferInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyBuffer2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyBuffer2");
 require(fn);
 fn(commandBuffer, pCopyBufferInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyBuffer2KHR(VkCommandBuffer commandBuffer, const VkCopyBufferInfo2* pCopyBufferInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyBuffer2KHR;require(pfn);pfn(commandBuffer, pCopyBufferInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyBuffer2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyBuffer2KHR");
 require(fn);
 fn(commandBuffer, pCopyBufferInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyBufferToImage(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkBufferImageCopy* pRegions) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyBufferToImage;require(pfn);pfn(commandBuffer, srcBuffer, dstImage, dstImageLayout, regionCount, pRegions);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyBufferToImage;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyBufferToImage");
 require(fn);
 fn(commandBuffer, srcBuffer, dstImage, dstImageLayout, regionCount, pRegions);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyBufferToImage2(VkCommandBuffer commandBuffer, const VkCopyBufferToImageInfo2* pCopyBufferToImageInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyBufferToImage2;require(pfn);pfn(commandBuffer, pCopyBufferToImageInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyBufferToImage2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyBufferToImage2");
 require(fn);
 fn(commandBuffer, pCopyBufferToImageInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyBufferToImage2KHR(VkCommandBuffer commandBuffer, const VkCopyBufferToImageInfo2* pCopyBufferToImageInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyBufferToImage2KHR;require(pfn);pfn(commandBuffer, pCopyBufferToImageInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyBufferToImage2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyBufferToImage2KHR");
 require(fn);
 fn(commandBuffer, pCopyBufferToImageInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyGpaSessionResultsAMD(VkCommandBuffer commandBuffer, VkGpaSessionAMD                   gpaSession) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyGpaSessionResultsAMD;require(pfn);pfn(commandBuffer, gpaSession);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyGpaSessionResultsAMD;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyGpaSessionResultsAMD");
 require(fn);
 fn(commandBuffer, gpaSession);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout, VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageCopy* pRegions) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyImage;require(pfn);pfn(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyImage;
 if(t.pending76.command){const int savedErrno=errno;firstAdmissionBeforeCommand77(t,d,commandBuffer,"vkCmdCopyImage");errno=savedErrno;}
 if (t.command==commandBuffer && t.recording && d->worker) { if(d->worker->copyImage(t.recording, commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions)) return; }
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyImage",commandBuffer);
 require(fn);
 fn(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyImage2(VkCommandBuffer commandBuffer, const VkCopyImageInfo2* pCopyImageInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyImage2;require(pfn);pfn(commandBuffer, pCopyImageInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyImage2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyImage2");
 require(fn);
 fn(commandBuffer, pCopyImageInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyImage2KHR(VkCommandBuffer commandBuffer, const VkCopyImageInfo2* pCopyImageInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyImage2KHR;require(pfn);pfn(commandBuffer, pCopyImageInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyImage2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyImage2KHR");
 require(fn);
 fn(commandBuffer, pCopyImageInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyImageToBuffer(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout, VkBuffer dstBuffer, uint32_t regionCount, const VkBufferImageCopy* pRegions) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyImageToBuffer;require(pfn);pfn(commandBuffer, srcImage, srcImageLayout, dstBuffer, regionCount, pRegions);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyImageToBuffer;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyImageToBuffer");
 require(fn);
 fn(commandBuffer, srcImage, srcImageLayout, dstBuffer, regionCount, pRegions);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyImageToBuffer2(VkCommandBuffer commandBuffer, const VkCopyImageToBufferInfo2* pCopyImageToBufferInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyImageToBuffer2;require(pfn);pfn(commandBuffer, pCopyImageToBufferInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyImageToBuffer2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyImageToBuffer2");
 require(fn);
 fn(commandBuffer, pCopyImageToBufferInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyImageToBuffer2KHR(VkCommandBuffer commandBuffer, const VkCopyImageToBufferInfo2* pCopyImageToBufferInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyImageToBuffer2KHR;require(pfn);pfn(commandBuffer, pCopyImageToBufferInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyImageToBuffer2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyImageToBuffer2KHR");
 require(fn);
 fn(commandBuffer, pCopyImageToBufferInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyImageToMemoryKHR(VkCommandBuffer commandBuffer, const VkCopyDeviceMemoryImageInfoKHR* pCopyMemoryInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyImageToMemoryKHR;require(pfn);pfn(commandBuffer, pCopyMemoryInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyImageToMemoryKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyImageToMemoryKHR");
 require(fn);
 fn(commandBuffer, pCopyMemoryInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyMemoryIndirectKHR(VkCommandBuffer commandBuffer, const VkCopyMemoryIndirectInfoKHR* pCopyMemoryIndirectInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyMemoryIndirectKHR;require(pfn);pfn(commandBuffer, pCopyMemoryIndirectInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyMemoryIndirectKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyMemoryIndirectKHR");
 require(fn);
 fn(commandBuffer, pCopyMemoryIndirectInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyMemoryIndirectNV(VkCommandBuffer commandBuffer, VkDeviceAddress copyBufferAddress, uint32_t copyCount, uint32_t stride) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyMemoryIndirectNV;require(pfn);pfn(commandBuffer, copyBufferAddress, copyCount, stride);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyMemoryIndirectNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyMemoryIndirectNV");
 require(fn);
 fn(commandBuffer, copyBufferAddress, copyCount, stride);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyMemoryKHR(VkCommandBuffer commandBuffer, const VkCopyDeviceMemoryInfoKHR* pCopyMemoryInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyMemoryKHR;require(pfn);pfn(commandBuffer, pCopyMemoryInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyMemoryKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyMemoryKHR");
 require(fn);
 fn(commandBuffer, pCopyMemoryInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyMemoryToAccelerationStructureKHR(VkCommandBuffer commandBuffer, const VkCopyMemoryToAccelerationStructureInfoKHR* pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyMemoryToAccelerationStructureKHR;require(pfn);pfn(commandBuffer, pInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyMemoryToAccelerationStructureKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyMemoryToAccelerationStructureKHR");
 require(fn);
 fn(commandBuffer, pInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyMemoryToImageIndirectKHR(VkCommandBuffer commandBuffer, const VkCopyMemoryToImageIndirectInfoKHR* pCopyMemoryToImageIndirectInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyMemoryToImageIndirectKHR;require(pfn);pfn(commandBuffer, pCopyMemoryToImageIndirectInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyMemoryToImageIndirectKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyMemoryToImageIndirectKHR");
 require(fn);
 fn(commandBuffer, pCopyMemoryToImageIndirectInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyMemoryToImageIndirectNV(VkCommandBuffer commandBuffer, VkDeviceAddress copyBufferAddress, uint32_t copyCount, uint32_t stride, VkImage dstImage, VkImageLayout dstImageLayout, const VkImageSubresourceLayers* pImageSubresources) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyMemoryToImageIndirectNV;require(pfn);pfn(commandBuffer, copyBufferAddress, copyCount, stride, dstImage, dstImageLayout, pImageSubresources);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyMemoryToImageIndirectNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyMemoryToImageIndirectNV");
 require(fn);
 fn(commandBuffer, copyBufferAddress, copyCount, stride, dstImage, dstImageLayout, pImageSubresources);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyMemoryToImageKHR(VkCommandBuffer commandBuffer, const VkCopyDeviceMemoryImageInfoKHR* pCopyMemoryInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyMemoryToImageKHR;require(pfn);pfn(commandBuffer, pCopyMemoryInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyMemoryToImageKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyMemoryToImageKHR");
 require(fn);
 fn(commandBuffer, pCopyMemoryInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyMemoryToMicromapEXT(VkCommandBuffer    commandBuffer, const VkCopyMemoryToMicromapInfoEXT* pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyMemoryToMicromapEXT;require(pfn);pfn(commandBuffer, pInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyMemoryToMicromapEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyMemoryToMicromapEXT");
 require(fn);
 fn(commandBuffer, pInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyMicromapEXT(VkCommandBuffer commandBuffer, const VkCopyMicromapInfoEXT*      pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyMicromapEXT;require(pfn);pfn(commandBuffer, pInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyMicromapEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyMicromapEXT");
 require(fn);
 fn(commandBuffer, pInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyMicromapToMemoryEXT(VkCommandBuffer    commandBuffer, const VkCopyMicromapToMemoryInfoEXT* pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyMicromapToMemoryEXT;require(pfn);pfn(commandBuffer, pInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyMicromapToMemoryEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyMicromapToMemoryEXT");
 require(fn);
 fn(commandBuffer, pInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyQueryPoolResults(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize stride, VkQueryResultFlags flags) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyQueryPoolResults;require(pfn);pfn(commandBuffer, queryPool, firstQuery, queryCount, dstBuffer, dstOffset, stride, flags);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyQueryPoolResults;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyQueryPoolResults");
 require(fn);
 fn(commandBuffer, queryPool, firstQuery, queryCount, dstBuffer, dstOffset, stride, flags);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyQueryPoolResultsToMemoryKHR(VkCommandBuffer   commandBuffer, VkQueryPool                         queryPool, uint32_t                            firstQuery, uint32_t                            queryCount, const VkStridedDeviceAddressRangeKHR* pDstRange, VkAddressCommandFlagsKHR    dstFlags, VkQueryResultFlags  queryResultFlags) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyQueryPoolResultsToMemoryKHR;require(pfn);pfn(commandBuffer, queryPool, firstQuery, queryCount, pDstRange, dstFlags, queryResultFlags);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyQueryPoolResultsToMemoryKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyQueryPoolResultsToMemoryKHR");
 require(fn);
 fn(commandBuffer, queryPool, firstQuery, queryCount, pDstRange, dstFlags, queryResultFlags);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCopyTensorARM(VkCommandBuffer commandBuffer, const VkCopyTensorInfoARM* pCopyTensorInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCopyTensorARM;require(pfn);pfn(commandBuffer, pCopyTensorInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCopyTensorARM;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCopyTensorARM");
 require(fn);
 fn(commandBuffer, pCopyTensorInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCuLaunchKernelNVX(VkCommandBuffer commandBuffer, const VkCuLaunchInfoNVX* pLaunchInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCuLaunchKernelNVX;require(pfn);pfn(commandBuffer, pLaunchInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCuLaunchKernelNVX;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCuLaunchKernelNVX");
 require(fn);
 fn(commandBuffer, pLaunchInfo);
}
#if defined(VK_ENABLE_BETA_EXTENSIONS)
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdCudaLaunchKernelNV(VkCommandBuffer commandBuffer, const VkCudaLaunchInfoNV* pLaunchInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdCudaLaunchKernelNV;require(pfn);pfn(commandBuffer, pLaunchInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdCudaLaunchKernelNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdCudaLaunchKernelNV");
 require(fn);
 fn(commandBuffer, pLaunchInfo);
}
#endif
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDebugMarkerBeginEXT(VkCommandBuffer commandBuffer, const VkDebugMarkerMarkerInfoEXT* pMarkerInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDebugMarkerBeginEXT;require(pfn);pfn(commandBuffer, pMarkerInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDebugMarkerBeginEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDebugMarkerBeginEXT");
 require(fn);
 fn(commandBuffer, pMarkerInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDebugMarkerEndEXT(VkCommandBuffer commandBuffer) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDebugMarkerEndEXT;require(pfn);pfn(commandBuffer);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDebugMarkerEndEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDebugMarkerEndEXT");
 require(fn);
 fn(commandBuffer);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDebugMarkerInsertEXT(VkCommandBuffer commandBuffer, const VkDebugMarkerMarkerInfoEXT* pMarkerInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDebugMarkerInsertEXT;require(pfn);pfn(commandBuffer, pMarkerInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDebugMarkerInsertEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDebugMarkerInsertEXT");
 require(fn);
 fn(commandBuffer, pMarkerInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDecodeVideoKHR(VkCommandBuffer commandBuffer, const VkVideoDecodeInfoKHR* pDecodeInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDecodeVideoKHR;require(pfn);pfn(commandBuffer, pDecodeInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDecodeVideoKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDecodeVideoKHR");
 require(fn);
 fn(commandBuffer, pDecodeInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDecompressMemoryEXT(VkCommandBuffer commandBuffer, const VkDecompressMemoryInfoEXT* pDecompressMemoryInfoEXT) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDecompressMemoryEXT;require(pfn);pfn(commandBuffer, pDecompressMemoryInfoEXT);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDecompressMemoryEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDecompressMemoryEXT");
 require(fn);
 fn(commandBuffer, pDecompressMemoryInfoEXT);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDecompressMemoryIndirectCountEXT(VkCommandBuffer commandBuffer, VkMemoryDecompressionMethodFlagsEXT decompressionMethod, VkDeviceAddress indirectCommandsAddress, VkDeviceAddress indirectCommandsCountAddress, uint32_t maxDecompressionCount, uint32_t stride) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDecompressMemoryIndirectCountEXT;require(pfn);pfn(commandBuffer, decompressionMethod, indirectCommandsAddress, indirectCommandsCountAddress, maxDecompressionCount, stride);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDecompressMemoryIndirectCountEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDecompressMemoryIndirectCountEXT");
 require(fn);
 fn(commandBuffer, decompressionMethod, indirectCommandsAddress, indirectCommandsCountAddress, maxDecompressionCount, stride);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDecompressMemoryIndirectCountNV(VkCommandBuffer commandBuffer, VkDeviceAddress indirectCommandsAddress, VkDeviceAddress indirectCommandsCountAddress, uint32_t stride) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDecompressMemoryIndirectCountNV;require(pfn);pfn(commandBuffer, indirectCommandsAddress, indirectCommandsCountAddress, stride);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDecompressMemoryIndirectCountNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDecompressMemoryIndirectCountNV");
 require(fn);
 fn(commandBuffer, indirectCommandsAddress, indirectCommandsCountAddress, stride);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDecompressMemoryNV(VkCommandBuffer commandBuffer, uint32_t decompressRegionCount, const VkDecompressMemoryRegionNV* pDecompressMemoryRegions) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDecompressMemoryNV;require(pfn);pfn(commandBuffer, decompressRegionCount, pDecompressMemoryRegions);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDecompressMemoryNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDecompressMemoryNV");
 require(fn);
 fn(commandBuffer, decompressRegionCount, pDecompressMemoryRegions);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDispatch(VkCommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDispatch;require(pfn);pfn(commandBuffer, groupCountX, groupCountY, groupCountZ);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDispatch;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDispatch");
 require(fn);
 fn(commandBuffer, groupCountX, groupCountY, groupCountZ);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDispatchBase(VkCommandBuffer commandBuffer, uint32_t baseGroupX, uint32_t baseGroupY, uint32_t baseGroupZ, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDispatchBase;require(pfn);pfn(commandBuffer, baseGroupX, baseGroupY, baseGroupZ, groupCountX, groupCountY, groupCountZ);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDispatchBase;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDispatchBase");
 require(fn);
 fn(commandBuffer, baseGroupX, baseGroupY, baseGroupZ, groupCountX, groupCountY, groupCountZ);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDispatchBaseKHR(VkCommandBuffer commandBuffer, uint32_t baseGroupX, uint32_t baseGroupY, uint32_t baseGroupZ, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDispatchBaseKHR;require(pfn);pfn(commandBuffer, baseGroupX, baseGroupY, baseGroupZ, groupCountX, groupCountY, groupCountZ);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDispatchBaseKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDispatchBaseKHR");
 require(fn);
 fn(commandBuffer, baseGroupX, baseGroupY, baseGroupZ, groupCountX, groupCountY, groupCountZ);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDispatchDataGraphARM(VkCommandBuffer commandBuffer, VkDataGraphPipelineSessionARM session, const VkDataGraphPipelineDispatchInfoARM* pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDispatchDataGraphARM;require(pfn);pfn(commandBuffer, session, pInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDispatchDataGraphARM;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDispatchDataGraphARM");
 require(fn);
 fn(commandBuffer, session, pInfo);
}
#if defined(VK_ENABLE_BETA_EXTENSIONS)
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDispatchGraphAMDX(VkCommandBuffer                                 commandBuffer, VkDeviceAddress                                 scratch, VkDeviceSize                                    scratchSize, const VkDispatchGraphCountInfoAMDX*             pCountInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDispatchGraphAMDX;require(pfn);pfn(commandBuffer, scratch, scratchSize, pCountInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDispatchGraphAMDX;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDispatchGraphAMDX");
 require(fn);
 fn(commandBuffer, scratch, scratchSize, pCountInfo);
}
#endif
#if defined(VK_ENABLE_BETA_EXTENSIONS)
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDispatchGraphIndirectAMDX(VkCommandBuffer                                 commandBuffer, VkDeviceAddress                                 scratch, VkDeviceSize                                    scratchSize, const VkDispatchGraphCountInfoAMDX*             pCountInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDispatchGraphIndirectAMDX;require(pfn);pfn(commandBuffer, scratch, scratchSize, pCountInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDispatchGraphIndirectAMDX;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDispatchGraphIndirectAMDX");
 require(fn);
 fn(commandBuffer, scratch, scratchSize, pCountInfo);
}
#endif
#if defined(VK_ENABLE_BETA_EXTENSIONS)
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDispatchGraphIndirectCountAMDX(VkCommandBuffer                                 commandBuffer, VkDeviceAddress                                 scratch, VkDeviceSize                                    scratchSize, VkDeviceAddress                                 countInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDispatchGraphIndirectCountAMDX;require(pfn);pfn(commandBuffer, scratch, scratchSize, countInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDispatchGraphIndirectCountAMDX;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDispatchGraphIndirectCountAMDX");
 require(fn);
 fn(commandBuffer, scratch, scratchSize, countInfo);
}
#endif
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDispatchIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDispatchIndirect;require(pfn);pfn(commandBuffer, buffer, offset);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDispatchIndirect;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDispatchIndirect");
 require(fn);
 fn(commandBuffer, buffer, offset);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDispatchIndirect2KHR(VkCommandBuffer   commandBuffer, const VkDispatchIndirect2InfoKHR*   pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDispatchIndirect2KHR;require(pfn);pfn(commandBuffer, pInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDispatchIndirect2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDispatchIndirect2KHR");
 require(fn);
 fn(commandBuffer, pInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDispatchTileQCOM(VkCommandBuffer commandBuffer, const VkDispatchTileInfoQCOM* pDispatchTileInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDispatchTileQCOM;require(pfn);pfn(commandBuffer, pDispatchTileInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDispatchTileQCOM;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDispatchTileQCOM");
 require(fn);
 fn(commandBuffer, pDispatchTileInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDraw;require(pfn);pfn(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDraw;
 noteDraw(t,d,commandBuffer);
 if(t.pending76.command){const int savedErrno=errno;firstAdmissionBeforeCommand77(t,d,commandBuffer,"vkCmdDraw");errno=savedErrno;}
 if (t.command==commandBuffer && t.recording && d->worker) { if(d->worker->draw(t.recording, commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance)) return; }
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDraw");
 require(fn);
 fn(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawClusterHUAWEI(VkCommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawClusterHUAWEI;require(pfn);pfn(commandBuffer, groupCountX, groupCountY, groupCountZ);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawClusterHUAWEI;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawClusterHUAWEI");
 require(fn);
 fn(commandBuffer, groupCountX, groupCountY, groupCountZ);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawClusterIndirectHUAWEI(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawClusterIndirectHUAWEI;require(pfn);pfn(commandBuffer, buffer, offset);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawClusterIndirectHUAWEI;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawClusterIndirectHUAWEI");
 require(fn);
 fn(commandBuffer, buffer, offset);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawIndexed;require(pfn);pfn(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawIndexed;
 noteDraw(t,d,commandBuffer);
 if(t.pending76.command){const int savedErrno=errno;firstAdmissionBeforeCommand77(t,d,commandBuffer,"vkCmdDrawIndexed");errno=savedErrno;}
 if (t.command==commandBuffer && t.recording && d->worker) { if(d->worker->indexed(t.recording, commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance)) return; }
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawIndexed");
 require(fn);
 fn(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawIndexedIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawIndexedIndirect;require(pfn);pfn(commandBuffer, buffer, offset, drawCount, stride);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawIndexedIndirect;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawIndexedIndirect");
 require(fn);
 fn(commandBuffer, buffer, offset, drawCount, stride);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawIndexedIndirect2KHR(VkCommandBuffer   commandBuffer, const VkDrawIndirect2InfoKHR*       pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawIndexedIndirect2KHR;require(pfn);pfn(commandBuffer, pInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawIndexedIndirect2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawIndexedIndirect2KHR");
 require(fn);
 fn(commandBuffer, pInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawIndexedIndirectCount(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawIndexedIndirectCount;require(pfn);pfn(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawIndexedIndirectCount;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawIndexedIndirectCount");
 require(fn);
 fn(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawIndexedIndirectCount2KHR(VkCommandBuffer   commandBuffer, const VkDrawIndirectCount2InfoKHR*  pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawIndexedIndirectCount2KHR;require(pfn);pfn(commandBuffer, pInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawIndexedIndirectCount2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawIndexedIndirectCount2KHR");
 require(fn);
 fn(commandBuffer, pInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawIndexedIndirectCountAMD(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawIndexedIndirectCountAMD;require(pfn);pfn(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawIndexedIndirectCountAMD;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawIndexedIndirectCountAMD");
 require(fn);
 fn(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawIndexedIndirectCountKHR(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawIndexedIndirectCountKHR;require(pfn);pfn(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawIndexedIndirectCountKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawIndexedIndirectCountKHR");
 require(fn);
 fn(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawIndirect;require(pfn);pfn(commandBuffer, buffer, offset, drawCount, stride);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawIndirect;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawIndirect");
 require(fn);
 fn(commandBuffer, buffer, offset, drawCount, stride);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawIndirect2KHR(VkCommandBuffer   commandBuffer, const VkDrawIndirect2InfoKHR*       pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawIndirect2KHR;require(pfn);pfn(commandBuffer, pInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawIndirect2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawIndirect2KHR");
 require(fn);
 fn(commandBuffer, pInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawIndirectByteCount2EXT(VkCommandBuffer commandBuffer, uint32_t instanceCount, uint32_t firstInstance, const VkBindTransformFeedbackBuffer2InfoEXT* pCounterInfo, uint32_t counterOffset, uint32_t vertexStride) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawIndirectByteCount2EXT;require(pfn);pfn(commandBuffer, instanceCount, firstInstance, pCounterInfo, counterOffset, vertexStride);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawIndirectByteCount2EXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawIndirectByteCount2EXT");
 require(fn);
 fn(commandBuffer, instanceCount, firstInstance, pCounterInfo, counterOffset, vertexStride);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawIndirectByteCountEXT(VkCommandBuffer commandBuffer, uint32_t instanceCount, uint32_t firstInstance, VkBuffer counterBuffer, VkDeviceSize counterBufferOffset, uint32_t counterOffset, uint32_t vertexStride) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawIndirectByteCountEXT;require(pfn);pfn(commandBuffer, instanceCount, firstInstance, counterBuffer, counterBufferOffset, counterOffset, vertexStride);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawIndirectByteCountEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawIndirectByteCountEXT");
 require(fn);
 fn(commandBuffer, instanceCount, firstInstance, counterBuffer, counterBufferOffset, counterOffset, vertexStride);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawIndirectCount(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawIndirectCount;require(pfn);pfn(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawIndirectCount;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawIndirectCount");
 require(fn);
 fn(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawIndirectCount2KHR(VkCommandBuffer   commandBuffer, const VkDrawIndirectCount2InfoKHR*  pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawIndirectCount2KHR;require(pfn);pfn(commandBuffer, pInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawIndirectCount2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawIndirectCount2KHR");
 require(fn);
 fn(commandBuffer, pInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawIndirectCountAMD(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawIndirectCountAMD;require(pfn);pfn(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawIndirectCountAMD;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawIndirectCountAMD");
 require(fn);
 fn(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawIndirectCountKHR(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawIndirectCountKHR;require(pfn);pfn(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawIndirectCountKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawIndirectCountKHR");
 require(fn);
 fn(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawMeshTasksEXT(VkCommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawMeshTasksEXT;require(pfn);pfn(commandBuffer, groupCountX, groupCountY, groupCountZ);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawMeshTasksEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawMeshTasksEXT");
 require(fn);
 fn(commandBuffer, groupCountX, groupCountY, groupCountZ);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawMeshTasksIndirect2EXT(VkCommandBuffer   commandBuffer, const VkDrawIndirect2InfoKHR*       pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawMeshTasksIndirect2EXT;require(pfn);pfn(commandBuffer, pInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawMeshTasksIndirect2EXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawMeshTasksIndirect2EXT");
 require(fn);
 fn(commandBuffer, pInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawMeshTasksIndirectCount2EXT(VkCommandBuffer   commandBuffer, const VkDrawIndirectCount2InfoKHR*  pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawMeshTasksIndirectCount2EXT;require(pfn);pfn(commandBuffer, pInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawMeshTasksIndirectCount2EXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawMeshTasksIndirectCount2EXT");
 require(fn);
 fn(commandBuffer, pInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawMeshTasksIndirectCountEXT(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawMeshTasksIndirectCountEXT;require(pfn);pfn(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawMeshTasksIndirectCountEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawMeshTasksIndirectCountEXT");
 require(fn);
 fn(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawMeshTasksIndirectCountNV(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawMeshTasksIndirectCountNV;require(pfn);pfn(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawMeshTasksIndirectCountNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawMeshTasksIndirectCountNV");
 require(fn);
 fn(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawMeshTasksIndirectEXT(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawMeshTasksIndirectEXT;require(pfn);pfn(commandBuffer, buffer, offset, drawCount, stride);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawMeshTasksIndirectEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawMeshTasksIndirectEXT");
 require(fn);
 fn(commandBuffer, buffer, offset, drawCount, stride);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawMeshTasksIndirectNV(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawMeshTasksIndirectNV;require(pfn);pfn(commandBuffer, buffer, offset, drawCount, stride);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawMeshTasksIndirectNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawMeshTasksIndirectNV");
 require(fn);
 fn(commandBuffer, buffer, offset, drawCount, stride);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawMeshTasksNV(VkCommandBuffer commandBuffer, uint32_t taskCount, uint32_t firstTask) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawMeshTasksNV;require(pfn);pfn(commandBuffer, taskCount, firstTask);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawMeshTasksNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawMeshTasksNV");
 require(fn);
 fn(commandBuffer, taskCount, firstTask);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawMultiEXT(VkCommandBuffer commandBuffer, uint32_t drawCount, const VkMultiDrawInfoEXT* pVertexInfo, uint32_t instanceCount, uint32_t firstInstance, uint32_t stride) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawMultiEXT;require(pfn);pfn(commandBuffer, drawCount, pVertexInfo, instanceCount, firstInstance, stride);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawMultiEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawMultiEXT");
 require(fn);
 fn(commandBuffer, drawCount, pVertexInfo, instanceCount, firstInstance, stride);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdDrawMultiIndexedEXT(VkCommandBuffer commandBuffer, uint32_t drawCount, const VkMultiDrawIndexedInfoEXT* pIndexInfo, uint32_t instanceCount, uint32_t firstInstance, uint32_t stride, const int32_t* pVertexOffset) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdDrawMultiIndexedEXT;require(pfn);pfn(commandBuffer, drawCount, pIndexInfo, instanceCount, firstInstance, stride, pVertexOffset);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdDrawMultiIndexedEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdDrawMultiIndexedEXT");
 require(fn);
 fn(commandBuffer, drawCount, pIndexInfo, instanceCount, firstInstance, stride, pVertexOffset);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdEncodeVideoKHR(VkCommandBuffer commandBuffer, const VkVideoEncodeInfoKHR* pEncodeInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdEncodeVideoKHR;require(pfn);pfn(commandBuffer, pEncodeInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdEncodeVideoKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdEncodeVideoKHR");
 require(fn);
 fn(commandBuffer, pEncodeInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdEndConditionalRenderingEXT(VkCommandBuffer commandBuffer) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdEndConditionalRenderingEXT;require(pfn);pfn(commandBuffer);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdEndConditionalRenderingEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdEndConditionalRenderingEXT");
 require(fn);
 fn(commandBuffer);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdEndDebugUtilsLabelEXT(VkCommandBuffer commandBuffer) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdEndDebugUtilsLabelEXT;require(pfn);pfn(commandBuffer);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdEndDebugUtilsLabelEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdEndDebugUtilsLabelEXT");
 require(fn);
 fn(commandBuffer);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdEndGpaSampleAMD(VkCommandBuffer commandBuffer, VkGpaSessionAMD                   gpaSession, uint32_t                          sampleID) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdEndGpaSampleAMD;require(pfn);pfn(commandBuffer, gpaSession, sampleID);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdEndGpaSampleAMD;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdEndGpaSampleAMD");
 require(fn);
 fn(commandBuffer, gpaSession, sampleID);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCmdEndGpaSessionAMD(VkCommandBuffer commandBuffer, VkGpaSessionAMD                   gpaSession) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdEndGpaSessionAMD;require(pfn);return pfn(commandBuffer, gpaSession);}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdEndGpaSessionAMD;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdEndGpaSessionAMD");
 require(fn);
 return fn(commandBuffer, gpaSession);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdEndPerTileExecutionQCOM(VkCommandBuffer commandBuffer, const VkPerTileEndInfoQCOM* pPerTileEndInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdEndPerTileExecutionQCOM;require(pfn);pfn(commandBuffer, pPerTileEndInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdEndPerTileExecutionQCOM;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdEndPerTileExecutionQCOM");
 require(fn);
 fn(commandBuffer, pPerTileEndInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdEndQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdEndQuery;require(pfn);pfn(commandBuffer, queryPool, query);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdEndQuery;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdEndQuery");
 require(fn);
 fn(commandBuffer, queryPool, query);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdEndQueryIndexedEXT(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query, uint32_t index) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdEndQueryIndexedEXT;require(pfn);pfn(commandBuffer, queryPool, query, index);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdEndQueryIndexedEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdEndQueryIndexedEXT");
 require(fn);
 fn(commandBuffer, queryPool, query, index);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdEndRenderPass(VkCommandBuffer commandBuffer) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdEndRenderPass;require(pfn);pfn(commandBuffer);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdEndRenderPass;
 if(t.pending76.command){const int savedErrno=errno;firstAdmissionBeforeCommand77(t,d,commandBuffer,"vkCmdEndRenderPass");errno=savedErrno;}
 if (t.command==commandBuffer && t.recording && d->worker) { if(d->worker->endPass(t.recording, commandBuffer)) return; }
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdEndRenderPass");
 require(fn);
 fn(commandBuffer);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdEndRenderPass2(VkCommandBuffer commandBuffer, const VkSubpassEndInfo*        pSubpassEndInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdEndRenderPass2;require(pfn);pfn(commandBuffer, pSubpassEndInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdEndRenderPass2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdEndRenderPass2");
 require(fn);
 fn(commandBuffer, pSubpassEndInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdEndRenderPass2KHR(VkCommandBuffer commandBuffer, const VkSubpassEndInfo*        pSubpassEndInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdEndRenderPass2KHR;require(pfn);pfn(commandBuffer, pSubpassEndInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdEndRenderPass2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdEndRenderPass2KHR");
 require(fn);
 fn(commandBuffer, pSubpassEndInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdEndRendering(VkCommandBuffer                   commandBuffer) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdEndRendering;require(pfn);pfn(commandBuffer);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdEndRendering;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdEndRendering");
 require(fn);
 fn(commandBuffer);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdEndRendering2EXT(VkCommandBuffer                   commandBuffer, const VkRenderingEndInfoKHR*        pRenderingEndInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdEndRendering2EXT;require(pfn);pfn(commandBuffer, pRenderingEndInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdEndRendering2EXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdEndRendering2EXT");
 require(fn);
 fn(commandBuffer, pRenderingEndInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdEndRendering2KHR(VkCommandBuffer                   commandBuffer, const VkRenderingEndInfoKHR*        pRenderingEndInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdEndRendering2KHR;require(pfn);pfn(commandBuffer, pRenderingEndInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdEndRendering2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdEndRendering2KHR");
 require(fn);
 fn(commandBuffer, pRenderingEndInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdEndRenderingKHR(VkCommandBuffer                   commandBuffer) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdEndRenderingKHR;require(pfn);pfn(commandBuffer);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdEndRenderingKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdEndRenderingKHR");
 require(fn);
 fn(commandBuffer);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdEndShaderInstrumentationARM(VkCommandBuffer commandBuffer) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdEndShaderInstrumentationARM;require(pfn);pfn(commandBuffer);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdEndShaderInstrumentationARM;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdEndShaderInstrumentationARM");
 require(fn);
 fn(commandBuffer);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdEndTransformFeedback2EXT(VkCommandBuffer   commandBuffer, uint32_t                            firstCounterRange, uint32_t            counterRangeCount, const VkBindTransformFeedbackBuffer2InfoEXT* pCounterInfos) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdEndTransformFeedback2EXT;require(pfn);pfn(commandBuffer, firstCounterRange, counterRangeCount, pCounterInfos);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdEndTransformFeedback2EXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdEndTransformFeedback2EXT");
 require(fn);
 fn(commandBuffer, firstCounterRange, counterRangeCount, pCounterInfos);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdEndTransformFeedbackEXT(VkCommandBuffer commandBuffer, uint32_t firstCounterBuffer, uint32_t counterBufferCount, const VkBuffer* pCounterBuffers, const VkDeviceSize* pCounterBufferOffsets) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdEndTransformFeedbackEXT;require(pfn);pfn(commandBuffer, firstCounterBuffer, counterBufferCount, pCounterBuffers, pCounterBufferOffsets);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdEndTransformFeedbackEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdEndTransformFeedbackEXT");
 require(fn);
 fn(commandBuffer, firstCounterBuffer, counterBufferCount, pCounterBuffers, pCounterBufferOffsets);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdEndVideoCodingKHR(VkCommandBuffer commandBuffer, const VkVideoEndCodingInfoKHR* pEndCodingInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdEndVideoCodingKHR;require(pfn);pfn(commandBuffer, pEndCodingInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdEndVideoCodingKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdEndVideoCodingKHR");
 require(fn);
 fn(commandBuffer, pEndCodingInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdExecuteCommands(VkCommandBuffer commandBuffer, uint32_t commandBufferCount, const VkCommandBuffer* pCommandBuffers) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdExecuteCommands;require(pfn);pfn(commandBuffer, commandBufferCount, pCommandBuffers);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdExecuteCommands;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdExecuteCommands");
 require(fn);
 fn(commandBuffer, commandBufferCount, pCommandBuffers);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdExecuteGeneratedCommandsEXT(VkCommandBuffer commandBuffer, VkBool32 isPreprocessed, const VkGeneratedCommandsInfoEXT* pGeneratedCommandsInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdExecuteGeneratedCommandsEXT;require(pfn);pfn(commandBuffer, isPreprocessed, pGeneratedCommandsInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdExecuteGeneratedCommandsEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdExecuteGeneratedCommandsEXT");
 require(fn);
 fn(commandBuffer, isPreprocessed, pGeneratedCommandsInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdExecuteGeneratedCommandsNV(VkCommandBuffer commandBuffer, VkBool32 isPreprocessed, const VkGeneratedCommandsInfoNV* pGeneratedCommandsInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdExecuteGeneratedCommandsNV;require(pfn);pfn(commandBuffer, isPreprocessed, pGeneratedCommandsInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdExecuteGeneratedCommandsNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdExecuteGeneratedCommandsNV");
 require(fn);
 fn(commandBuffer, isPreprocessed, pGeneratedCommandsInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdFillBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize size, uint32_t data) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdFillBuffer;require(pfn);pfn(commandBuffer, dstBuffer, dstOffset, size, data);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdFillBuffer;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdFillBuffer");
 require(fn);
 fn(commandBuffer, dstBuffer, dstOffset, size, data);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdFillMemoryKHR(VkCommandBuffer   commandBuffer, const VkDeviceAddressRangeKHR*      pDstRange, VkAddressCommandFlagsKHR dstFlags, uint32_t                            data) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdFillMemoryKHR;require(pfn);pfn(commandBuffer, pDstRange, dstFlags, data);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdFillMemoryKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdFillMemoryKHR");
 require(fn);
 fn(commandBuffer, pDstRange, dstFlags, data);
}
#if defined(VK_ENABLE_BETA_EXTENSIONS)
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdInitializeGraphScratchMemoryAMDX(VkCommandBuffer                                 commandBuffer, VkPipeline                                      executionGraph, VkDeviceAddress                                 scratch, VkDeviceSize                                    scratchSize) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdInitializeGraphScratchMemoryAMDX;require(pfn);pfn(commandBuffer, executionGraph, scratch, scratchSize);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdInitializeGraphScratchMemoryAMDX;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdInitializeGraphScratchMemoryAMDX");
 require(fn);
 fn(commandBuffer, executionGraph, scratch, scratchSize);
}
#endif
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdInsertDebugUtilsLabelEXT(VkCommandBuffer commandBuffer, const VkDebugUtilsLabelEXT* pLabelInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdInsertDebugUtilsLabelEXT;require(pfn);pfn(commandBuffer, pLabelInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdInsertDebugUtilsLabelEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdInsertDebugUtilsLabelEXT");
 require(fn);
 fn(commandBuffer, pLabelInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdNextSubpass(VkCommandBuffer commandBuffer, VkSubpassContents contents) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdNextSubpass;require(pfn);pfn(commandBuffer, contents);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdNextSubpass;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdNextSubpass");
 require(fn);
 fn(commandBuffer, contents);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdNextSubpass2(VkCommandBuffer commandBuffer, const VkSubpassBeginInfo*      pSubpassBeginInfo, const VkSubpassEndInfo*        pSubpassEndInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdNextSubpass2;require(pfn);pfn(commandBuffer, pSubpassBeginInfo, pSubpassEndInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdNextSubpass2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdNextSubpass2");
 require(fn);
 fn(commandBuffer, pSubpassBeginInfo, pSubpassEndInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdNextSubpass2KHR(VkCommandBuffer commandBuffer, const VkSubpassBeginInfo*      pSubpassBeginInfo, const VkSubpassEndInfo*        pSubpassEndInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdNextSubpass2KHR;require(pfn);pfn(commandBuffer, pSubpassBeginInfo, pSubpassEndInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdNextSubpass2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdNextSubpass2KHR");
 require(fn);
 fn(commandBuffer, pSubpassBeginInfo, pSubpassEndInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdOpticalFlowExecuteNV(VkCommandBuffer commandBuffer, VkOpticalFlowSessionNV session, const VkOpticalFlowExecuteInfoNV* pExecuteInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdOpticalFlowExecuteNV;require(pfn);pfn(commandBuffer, session, pExecuteInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdOpticalFlowExecuteNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdOpticalFlowExecuteNV");
 require(fn);
 fn(commandBuffer, session, pExecuteInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdPipelineBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags, uint32_t memoryBarrierCount, const VkMemoryBarrier* pMemoryBarriers, uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier* pBufferMemoryBarriers, uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier* pImageMemoryBarriers) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdPipelineBarrier;require(pfn);pfn(commandBuffer, srcStageMask, dstStageMask, dependencyFlags, memoryBarrierCount, pMemoryBarriers, bufferMemoryBarrierCount, pBufferMemoryBarriers, imageMemoryBarrierCount, pImageMemoryBarriers);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdPipelineBarrier;
 if(t.pending76.command){const int savedErrno=errno;firstAdmissionBeforeCommand77(t,d,commandBuffer,"vkCmdPipelineBarrier");errno=savedErrno;}
 if (t.command==commandBuffer && t.recording && d->worker) { if(d->worker->barrier(t.recording, commandBuffer, srcStageMask, dstStageMask, dependencyFlags, memoryBarrierCount, pMemoryBarriers, bufferMemoryBarrierCount, pBufferMemoryBarriers, imageMemoryBarrierCount, pImageMemoryBarriers)) return; }
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdPipelineBarrier",commandBuffer);
 require(fn);
 fn(commandBuffer, srcStageMask, dstStageMask, dependencyFlags, memoryBarrierCount, pMemoryBarriers, bufferMemoryBarrierCount, pBufferMemoryBarriers, imageMemoryBarrierCount, pImageMemoryBarriers);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdPipelineBarrier2(VkCommandBuffer                   commandBuffer, const VkDependencyInfo*                             pDependencyInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdPipelineBarrier2;require(pfn);pfn(commandBuffer, pDependencyInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdPipelineBarrier2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdPipelineBarrier2");
 require(fn);
 fn(commandBuffer, pDependencyInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdPipelineBarrier2KHR(VkCommandBuffer                   commandBuffer, const VkDependencyInfo*                             pDependencyInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdPipelineBarrier2KHR;require(pfn);pfn(commandBuffer, pDependencyInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdPipelineBarrier2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdPipelineBarrier2KHR");
 require(fn);
 fn(commandBuffer, pDependencyInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdPreprocessGeneratedCommandsEXT(VkCommandBuffer commandBuffer, const VkGeneratedCommandsInfoEXT* pGeneratedCommandsInfo, VkCommandBuffer stateCommandBuffer) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdPreprocessGeneratedCommandsEXT;require(pfn);pfn(commandBuffer, pGeneratedCommandsInfo, stateCommandBuffer);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdPreprocessGeneratedCommandsEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdPreprocessGeneratedCommandsEXT");
 require(fn);
 fn(commandBuffer, pGeneratedCommandsInfo, stateCommandBuffer);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdPreprocessGeneratedCommandsNV(VkCommandBuffer commandBuffer, const VkGeneratedCommandsInfoNV* pGeneratedCommandsInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdPreprocessGeneratedCommandsNV;require(pfn);pfn(commandBuffer, pGeneratedCommandsInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdPreprocessGeneratedCommandsNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdPreprocessGeneratedCommandsNV");
 require(fn);
 fn(commandBuffer, pGeneratedCommandsInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdPushConstants(VkCommandBuffer commandBuffer, VkPipelineLayout layout, VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size, const void* pValues) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdPushConstants;require(pfn);pfn(commandBuffer, layout, stageFlags, offset, size, pValues);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdPushConstants;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdPushConstants");
 require(fn);
 fn(commandBuffer, layout, stageFlags, offset, size, pValues);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdPushConstants2(VkCommandBuffer commandBuffer, const VkPushConstantsInfo*        pPushConstantsInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdPushConstants2;require(pfn);pfn(commandBuffer, pPushConstantsInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdPushConstants2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdPushConstants2");
 require(fn);
 fn(commandBuffer, pPushConstantsInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdPushConstants2KHR(VkCommandBuffer commandBuffer, const VkPushConstantsInfo*        pPushConstantsInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdPushConstants2KHR;require(pfn);pfn(commandBuffer, pPushConstantsInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdPushConstants2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdPushConstants2KHR");
 require(fn);
 fn(commandBuffer, pPushConstantsInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdPushDataEXT(VkCommandBuffer                   commandBuffer, const VkPushDataInfoEXT*                            pPushDataInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdPushDataEXT;require(pfn);pfn(commandBuffer, pPushDataInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdPushDataEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdPushDataEXT");
 require(fn);
 fn(commandBuffer, pPushDataInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdPushDescriptorSet(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout, uint32_t set, uint32_t descriptorWriteCount, const VkWriteDescriptorSet* pDescriptorWrites) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdPushDescriptorSet;require(pfn);pfn(commandBuffer, pipelineBindPoint, layout, set, descriptorWriteCount, pDescriptorWrites);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdPushDescriptorSet;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdPushDescriptorSet");
 require(fn);
 fn(commandBuffer, pipelineBindPoint, layout, set, descriptorWriteCount, pDescriptorWrites);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdPushDescriptorSet2(VkCommandBuffer commandBuffer, const VkPushDescriptorSetInfo*    pPushDescriptorSetInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdPushDescriptorSet2;require(pfn);pfn(commandBuffer, pPushDescriptorSetInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdPushDescriptorSet2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdPushDescriptorSet2");
 require(fn);
 fn(commandBuffer, pPushDescriptorSetInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdPushDescriptorSet2KHR(VkCommandBuffer commandBuffer, const VkPushDescriptorSetInfo*    pPushDescriptorSetInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdPushDescriptorSet2KHR;require(pfn);pfn(commandBuffer, pPushDescriptorSetInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdPushDescriptorSet2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdPushDescriptorSet2KHR");
 require(fn);
 fn(commandBuffer, pPushDescriptorSetInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdPushDescriptorSetKHR(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout, uint32_t set, uint32_t descriptorWriteCount, const VkWriteDescriptorSet* pDescriptorWrites) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdPushDescriptorSetKHR;require(pfn);pfn(commandBuffer, pipelineBindPoint, layout, set, descriptorWriteCount, pDescriptorWrites);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdPushDescriptorSetKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdPushDescriptorSetKHR");
 require(fn);
 fn(commandBuffer, pipelineBindPoint, layout, set, descriptorWriteCount, pDescriptorWrites);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdPushDescriptorSetWithTemplate(VkCommandBuffer commandBuffer, VkDescriptorUpdateTemplate descriptorUpdateTemplate, VkPipelineLayout layout, uint32_t set, const void* pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdPushDescriptorSetWithTemplate;require(pfn);pfn(commandBuffer, descriptorUpdateTemplate, layout, set, pData);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdPushDescriptorSetWithTemplate;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdPushDescriptorSetWithTemplate");
 require(fn);
 fn(commandBuffer, descriptorUpdateTemplate, layout, set, pData);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdPushDescriptorSetWithTemplate2(VkCommandBuffer commandBuffer, const VkPushDescriptorSetWithTemplateInfo* pPushDescriptorSetWithTemplateInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdPushDescriptorSetWithTemplate2;require(pfn);pfn(commandBuffer, pPushDescriptorSetWithTemplateInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdPushDescriptorSetWithTemplate2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdPushDescriptorSetWithTemplate2");
 require(fn);
 fn(commandBuffer, pPushDescriptorSetWithTemplateInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdPushDescriptorSetWithTemplate2KHR(VkCommandBuffer commandBuffer, const VkPushDescriptorSetWithTemplateInfo* pPushDescriptorSetWithTemplateInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdPushDescriptorSetWithTemplate2KHR;require(pfn);pfn(commandBuffer, pPushDescriptorSetWithTemplateInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdPushDescriptorSetWithTemplate2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdPushDescriptorSetWithTemplate2KHR");
 require(fn);
 fn(commandBuffer, pPushDescriptorSetWithTemplateInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdPushDescriptorSetWithTemplateKHR(VkCommandBuffer commandBuffer, VkDescriptorUpdateTemplate descriptorUpdateTemplate, VkPipelineLayout layout, uint32_t set, const void* pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdPushDescriptorSetWithTemplateKHR;require(pfn);pfn(commandBuffer, descriptorUpdateTemplate, layout, set, pData);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdPushDescriptorSetWithTemplateKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdPushDescriptorSetWithTemplateKHR");
 require(fn);
 fn(commandBuffer, descriptorUpdateTemplate, layout, set, pData);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdResetEvent(VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags stageMask) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdResetEvent;require(pfn);pfn(commandBuffer, event, stageMask);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdResetEvent;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdResetEvent");
 require(fn);
 fn(commandBuffer, event, stageMask);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdResetEvent2(VkCommandBuffer                   commandBuffer, VkEvent                                             event, VkPipelineStageFlags2               stageMask) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdResetEvent2;require(pfn);pfn(commandBuffer, event, stageMask);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdResetEvent2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdResetEvent2");
 require(fn);
 fn(commandBuffer, event, stageMask);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdResetEvent2KHR(VkCommandBuffer                   commandBuffer, VkEvent                                             event, VkPipelineStageFlags2               stageMask) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdResetEvent2KHR;require(pfn);pfn(commandBuffer, event, stageMask);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdResetEvent2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdResetEvent2KHR");
 require(fn);
 fn(commandBuffer, event, stageMask);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdResetQueryPool(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdResetQueryPool;require(pfn);pfn(commandBuffer, queryPool, firstQuery, queryCount);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdResetQueryPool;
 if(t.pending76.command){const int savedErrno=errno;firstAdmissionBeforeCommand77(t,d,commandBuffer,"vkCmdResetQueryPool");errno=savedErrno;}
 if (t.command==commandBuffer && t.recording && d->worker) { if(d->worker->resetQueryPool(t.recording, commandBuffer, queryPool, firstQuery, queryCount)) return; }
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdResetQueryPool",commandBuffer);
 require(fn);
 fn(commandBuffer, queryPool, firstQuery, queryCount);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdResolveImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout, VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageResolve* pRegions) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdResolveImage;require(pfn);pfn(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdResolveImage;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdResolveImage");
 require(fn);
 fn(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdResolveImage2(VkCommandBuffer commandBuffer, const VkResolveImageInfo2* pResolveImageInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdResolveImage2;require(pfn);pfn(commandBuffer, pResolveImageInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdResolveImage2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdResolveImage2");
 require(fn);
 fn(commandBuffer, pResolveImageInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdResolveImage2KHR(VkCommandBuffer commandBuffer, const VkResolveImageInfo2* pResolveImageInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdResolveImage2KHR;require(pfn);pfn(commandBuffer, pResolveImageInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdResolveImage2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdResolveImage2KHR");
 require(fn);
 fn(commandBuffer, pResolveImageInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetAlphaToCoverageEnableEXT(VkCommandBuffer commandBuffer, VkBool32 alphaToCoverageEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetAlphaToCoverageEnableEXT;require(pfn);pfn(commandBuffer, alphaToCoverageEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetAlphaToCoverageEnableEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetAlphaToCoverageEnableEXT");
 require(fn);
 fn(commandBuffer, alphaToCoverageEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetAlphaToOneEnableEXT(VkCommandBuffer commandBuffer, VkBool32 alphaToOneEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetAlphaToOneEnableEXT;require(pfn);pfn(commandBuffer, alphaToOneEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetAlphaToOneEnableEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetAlphaToOneEnableEXT");
 require(fn);
 fn(commandBuffer, alphaToOneEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetAttachmentFeedbackLoopEnableEXT(VkCommandBuffer commandBuffer, VkImageAspectFlags aspectMask) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetAttachmentFeedbackLoopEnableEXT;require(pfn);pfn(commandBuffer, aspectMask);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetAttachmentFeedbackLoopEnableEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetAttachmentFeedbackLoopEnableEXT");
 require(fn);
 fn(commandBuffer, aspectMask);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetBlendConstants(VkCommandBuffer commandBuffer, const float blendConstants[4]) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetBlendConstants;require(pfn);pfn(commandBuffer, blendConstants);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetBlendConstants;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetBlendConstants");
 require(fn);
 fn(commandBuffer, blendConstants);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetCheckpointNV(VkCommandBuffer commandBuffer, const void* pCheckpointMarker) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetCheckpointNV;require(pfn);pfn(commandBuffer, pCheckpointMarker);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetCheckpointNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetCheckpointNV");
 require(fn);
 fn(commandBuffer, pCheckpointMarker);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetCoarseSampleOrderNV(VkCommandBuffer commandBuffer, VkCoarseSampleOrderTypeNV sampleOrderType, uint32_t customSampleOrderCount, const VkCoarseSampleOrderCustomNV* pCustomSampleOrders) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetCoarseSampleOrderNV;require(pfn);pfn(commandBuffer, sampleOrderType, customSampleOrderCount, pCustomSampleOrders);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetCoarseSampleOrderNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetCoarseSampleOrderNV");
 require(fn);
 fn(commandBuffer, sampleOrderType, customSampleOrderCount, pCustomSampleOrders);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetColorBlendAdvancedEXT(VkCommandBuffer commandBuffer, uint32_t firstAttachment, uint32_t attachmentCount, const VkColorBlendAdvancedEXT* pColorBlendAdvanced) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetColorBlendAdvancedEXT;require(pfn);pfn(commandBuffer, firstAttachment, attachmentCount, pColorBlendAdvanced);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetColorBlendAdvancedEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetColorBlendAdvancedEXT");
 require(fn);
 fn(commandBuffer, firstAttachment, attachmentCount, pColorBlendAdvanced);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetColorBlendEnableEXT(VkCommandBuffer commandBuffer, uint32_t firstAttachment, uint32_t attachmentCount, const VkBool32* pColorBlendEnables) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetColorBlendEnableEXT;require(pfn);pfn(commandBuffer, firstAttachment, attachmentCount, pColorBlendEnables);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetColorBlendEnableEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetColorBlendEnableEXT");
 require(fn);
 fn(commandBuffer, firstAttachment, attachmentCount, pColorBlendEnables);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetColorBlendEquationEXT(VkCommandBuffer commandBuffer, uint32_t firstAttachment, uint32_t attachmentCount, const VkColorBlendEquationEXT* pColorBlendEquations) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetColorBlendEquationEXT;require(pfn);pfn(commandBuffer, firstAttachment, attachmentCount, pColorBlendEquations);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetColorBlendEquationEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetColorBlendEquationEXT");
 require(fn);
 fn(commandBuffer, firstAttachment, attachmentCount, pColorBlendEquations);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetColorWriteEnableEXT(VkCommandBuffer       commandBuffer, uint32_t                                attachmentCount, const VkBool32*   pColorWriteEnables) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetColorWriteEnableEXT;require(pfn);pfn(commandBuffer, attachmentCount, pColorWriteEnables);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetColorWriteEnableEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetColorWriteEnableEXT");
 require(fn);
 fn(commandBuffer, attachmentCount, pColorWriteEnables);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetColorWriteMaskEXT(VkCommandBuffer commandBuffer, uint32_t firstAttachment, uint32_t attachmentCount, const VkColorComponentFlags* pColorWriteMasks) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetColorWriteMaskEXT;require(pfn);pfn(commandBuffer, firstAttachment, attachmentCount, pColorWriteMasks);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetColorWriteMaskEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetColorWriteMaskEXT");
 require(fn);
 fn(commandBuffer, firstAttachment, attachmentCount, pColorWriteMasks);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetComputeOccupancyPriorityNV(VkCommandBuffer commandBuffer, const VkComputeOccupancyPriorityParametersNV* pParameters) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetComputeOccupancyPriorityNV;require(pfn);pfn(commandBuffer, pParameters);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetComputeOccupancyPriorityNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetComputeOccupancyPriorityNV");
 require(fn);
 fn(commandBuffer, pParameters);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetConservativeRasterizationModeEXT(VkCommandBuffer commandBuffer, VkConservativeRasterizationModeEXT conservativeRasterizationMode) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetConservativeRasterizationModeEXT;require(pfn);pfn(commandBuffer, conservativeRasterizationMode);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetConservativeRasterizationModeEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetConservativeRasterizationModeEXT");
 require(fn);
 fn(commandBuffer, conservativeRasterizationMode);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetCoverageModulationModeNV(VkCommandBuffer commandBuffer, VkCoverageModulationModeNV coverageModulationMode) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetCoverageModulationModeNV;require(pfn);pfn(commandBuffer, coverageModulationMode);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetCoverageModulationModeNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetCoverageModulationModeNV");
 require(fn);
 fn(commandBuffer, coverageModulationMode);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetCoverageModulationTableEnableNV(VkCommandBuffer commandBuffer, VkBool32 coverageModulationTableEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetCoverageModulationTableEnableNV;require(pfn);pfn(commandBuffer, coverageModulationTableEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetCoverageModulationTableEnableNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetCoverageModulationTableEnableNV");
 require(fn);
 fn(commandBuffer, coverageModulationTableEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetCoverageModulationTableNV(VkCommandBuffer commandBuffer, uint32_t coverageModulationTableCount, const float* pCoverageModulationTable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetCoverageModulationTableNV;require(pfn);pfn(commandBuffer, coverageModulationTableCount, pCoverageModulationTable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetCoverageModulationTableNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetCoverageModulationTableNV");
 require(fn);
 fn(commandBuffer, coverageModulationTableCount, pCoverageModulationTable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetCoverageReductionModeNV(VkCommandBuffer commandBuffer, VkCoverageReductionModeNV coverageReductionMode) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetCoverageReductionModeNV;require(pfn);pfn(commandBuffer, coverageReductionMode);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetCoverageReductionModeNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetCoverageReductionModeNV");
 require(fn);
 fn(commandBuffer, coverageReductionMode);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetCoverageToColorEnableNV(VkCommandBuffer commandBuffer, VkBool32 coverageToColorEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetCoverageToColorEnableNV;require(pfn);pfn(commandBuffer, coverageToColorEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetCoverageToColorEnableNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetCoverageToColorEnableNV");
 require(fn);
 fn(commandBuffer, coverageToColorEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetCoverageToColorLocationNV(VkCommandBuffer commandBuffer, uint32_t coverageToColorLocation) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetCoverageToColorLocationNV;require(pfn);pfn(commandBuffer, coverageToColorLocation);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetCoverageToColorLocationNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetCoverageToColorLocationNV");
 require(fn);
 fn(commandBuffer, coverageToColorLocation);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetCullMode(VkCommandBuffer commandBuffer, VkCullModeFlags cullMode) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetCullMode;require(pfn);pfn(commandBuffer, cullMode);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetCullMode;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetCullMode");
 require(fn);
 fn(commandBuffer, cullMode);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetCullModeEXT(VkCommandBuffer commandBuffer, VkCullModeFlags cullMode) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetCullModeEXT;require(pfn);pfn(commandBuffer, cullMode);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetCullModeEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetCullModeEXT");
 require(fn);
 fn(commandBuffer, cullMode);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetDepthBias(VkCommandBuffer commandBuffer, float depthBiasConstantFactor, float depthBiasClamp, float depthBiasSlopeFactor) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetDepthBias;require(pfn);pfn(commandBuffer, depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetDepthBias;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetDepthBias");
 require(fn);
 fn(commandBuffer, depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetDepthBias2EXT(VkCommandBuffer commandBuffer, const VkDepthBiasInfoEXT*         pDepthBiasInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetDepthBias2EXT;require(pfn);pfn(commandBuffer, pDepthBiasInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetDepthBias2EXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetDepthBias2EXT");
 require(fn);
 fn(commandBuffer, pDepthBiasInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetDepthBiasEnable(VkCommandBuffer commandBuffer, VkBool32 depthBiasEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetDepthBiasEnable;require(pfn);pfn(commandBuffer, depthBiasEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetDepthBiasEnable;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetDepthBiasEnable");
 require(fn);
 fn(commandBuffer, depthBiasEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetDepthBiasEnableEXT(VkCommandBuffer commandBuffer, VkBool32 depthBiasEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetDepthBiasEnableEXT;require(pfn);pfn(commandBuffer, depthBiasEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetDepthBiasEnableEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetDepthBiasEnableEXT");
 require(fn);
 fn(commandBuffer, depthBiasEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetDepthBounds(VkCommandBuffer commandBuffer, float minDepthBounds, float maxDepthBounds) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetDepthBounds;require(pfn);pfn(commandBuffer, minDepthBounds, maxDepthBounds);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetDepthBounds;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetDepthBounds");
 require(fn);
 fn(commandBuffer, minDepthBounds, maxDepthBounds);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetDepthBoundsTestEnable(VkCommandBuffer commandBuffer, VkBool32 depthBoundsTestEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetDepthBoundsTestEnable;require(pfn);pfn(commandBuffer, depthBoundsTestEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetDepthBoundsTestEnable;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetDepthBoundsTestEnable");
 require(fn);
 fn(commandBuffer, depthBoundsTestEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetDepthBoundsTestEnableEXT(VkCommandBuffer commandBuffer, VkBool32 depthBoundsTestEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetDepthBoundsTestEnableEXT;require(pfn);pfn(commandBuffer, depthBoundsTestEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetDepthBoundsTestEnableEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetDepthBoundsTestEnableEXT");
 require(fn);
 fn(commandBuffer, depthBoundsTestEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetDepthClampEnableEXT(VkCommandBuffer commandBuffer, VkBool32 depthClampEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetDepthClampEnableEXT;require(pfn);pfn(commandBuffer, depthClampEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetDepthClampEnableEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetDepthClampEnableEXT");
 require(fn);
 fn(commandBuffer, depthClampEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetDepthClampRangeEXT(VkCommandBuffer commandBuffer, VkDepthClampModeEXT depthClampMode, const VkDepthClampRangeEXT* pDepthClampRange) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetDepthClampRangeEXT;require(pfn);pfn(commandBuffer, depthClampMode, pDepthClampRange);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetDepthClampRangeEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetDepthClampRangeEXT");
 require(fn);
 fn(commandBuffer, depthClampMode, pDepthClampRange);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetDepthClipEnableEXT(VkCommandBuffer commandBuffer, VkBool32 depthClipEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetDepthClipEnableEXT;require(pfn);pfn(commandBuffer, depthClipEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetDepthClipEnableEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetDepthClipEnableEXT");
 require(fn);
 fn(commandBuffer, depthClipEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetDepthClipNegativeOneToOneEXT(VkCommandBuffer commandBuffer, VkBool32 negativeOneToOne) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetDepthClipNegativeOneToOneEXT;require(pfn);pfn(commandBuffer, negativeOneToOne);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetDepthClipNegativeOneToOneEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetDepthClipNegativeOneToOneEXT");
 require(fn);
 fn(commandBuffer, negativeOneToOne);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetDepthCompareOp(VkCommandBuffer commandBuffer, VkCompareOp depthCompareOp) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetDepthCompareOp;require(pfn);pfn(commandBuffer, depthCompareOp);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetDepthCompareOp;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetDepthCompareOp");
 require(fn);
 fn(commandBuffer, depthCompareOp);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetDepthCompareOpEXT(VkCommandBuffer commandBuffer, VkCompareOp depthCompareOp) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetDepthCompareOpEXT;require(pfn);pfn(commandBuffer, depthCompareOp);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetDepthCompareOpEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetDepthCompareOpEXT");
 require(fn);
 fn(commandBuffer, depthCompareOp);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetDepthTestEnable(VkCommandBuffer commandBuffer, VkBool32 depthTestEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetDepthTestEnable;require(pfn);pfn(commandBuffer, depthTestEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetDepthTestEnable;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetDepthTestEnable");
 require(fn);
 fn(commandBuffer, depthTestEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetDepthTestEnableEXT(VkCommandBuffer commandBuffer, VkBool32 depthTestEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetDepthTestEnableEXT;require(pfn);pfn(commandBuffer, depthTestEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetDepthTestEnableEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetDepthTestEnableEXT");
 require(fn);
 fn(commandBuffer, depthTestEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetDepthWriteEnable(VkCommandBuffer commandBuffer, VkBool32 depthWriteEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetDepthWriteEnable;require(pfn);pfn(commandBuffer, depthWriteEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetDepthWriteEnable;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetDepthWriteEnable");
 require(fn);
 fn(commandBuffer, depthWriteEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetDepthWriteEnableEXT(VkCommandBuffer commandBuffer, VkBool32 depthWriteEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetDepthWriteEnableEXT;require(pfn);pfn(commandBuffer, depthWriteEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetDepthWriteEnableEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetDepthWriteEnableEXT");
 require(fn);
 fn(commandBuffer, depthWriteEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetDescriptorBufferOffsets2EXT(VkCommandBuffer commandBuffer, const VkSetDescriptorBufferOffsetsInfoEXT* pSetDescriptorBufferOffsetsInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetDescriptorBufferOffsets2EXT;require(pfn);pfn(commandBuffer, pSetDescriptorBufferOffsetsInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetDescriptorBufferOffsets2EXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetDescriptorBufferOffsets2EXT");
 require(fn);
 fn(commandBuffer, pSetDescriptorBufferOffsetsInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetDescriptorBufferOffsetsEXT(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout, uint32_t firstSet, uint32_t setCount, const uint32_t* pBufferIndices, const VkDeviceSize* pOffsets) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetDescriptorBufferOffsetsEXT;require(pfn);pfn(commandBuffer, pipelineBindPoint, layout, firstSet, setCount, pBufferIndices, pOffsets);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetDescriptorBufferOffsetsEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetDescriptorBufferOffsetsEXT");
 require(fn);
 fn(commandBuffer, pipelineBindPoint, layout, firstSet, setCount, pBufferIndices, pOffsets);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetDeviceMask(VkCommandBuffer commandBuffer, uint32_t deviceMask) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetDeviceMask;require(pfn);pfn(commandBuffer, deviceMask);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetDeviceMask;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetDeviceMask");
 require(fn);
 fn(commandBuffer, deviceMask);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetDeviceMaskKHR(VkCommandBuffer commandBuffer, uint32_t deviceMask) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetDeviceMaskKHR;require(pfn);pfn(commandBuffer, deviceMask);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetDeviceMaskKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetDeviceMaskKHR");
 require(fn);
 fn(commandBuffer, deviceMask);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetDiscardRectangleEXT(VkCommandBuffer commandBuffer, uint32_t firstDiscardRectangle, uint32_t discardRectangleCount, const VkRect2D* pDiscardRectangles) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetDiscardRectangleEXT;require(pfn);pfn(commandBuffer, firstDiscardRectangle, discardRectangleCount, pDiscardRectangles);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetDiscardRectangleEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetDiscardRectangleEXT");
 require(fn);
 fn(commandBuffer, firstDiscardRectangle, discardRectangleCount, pDiscardRectangles);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetDiscardRectangleEnableEXT(VkCommandBuffer commandBuffer, VkBool32 discardRectangleEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetDiscardRectangleEnableEXT;require(pfn);pfn(commandBuffer, discardRectangleEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetDiscardRectangleEnableEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetDiscardRectangleEnableEXT");
 require(fn);
 fn(commandBuffer, discardRectangleEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetDiscardRectangleModeEXT(VkCommandBuffer commandBuffer, VkDiscardRectangleModeEXT discardRectangleMode) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetDiscardRectangleModeEXT;require(pfn);pfn(commandBuffer, discardRectangleMode);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetDiscardRectangleModeEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetDiscardRectangleModeEXT");
 require(fn);
 fn(commandBuffer, discardRectangleMode);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetDispatchParametersARM(VkCommandBuffer commandBuffer, const VkDispatchParametersARM*    pDispatchParameters) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetDispatchParametersARM;require(pfn);pfn(commandBuffer, pDispatchParameters);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetDispatchParametersARM;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetDispatchParametersARM");
 require(fn);
 fn(commandBuffer, pDispatchParameters);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetEvent(VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags stageMask) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetEvent;require(pfn);pfn(commandBuffer, event, stageMask);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetEvent;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetEvent");
 require(fn);
 fn(commandBuffer, event, stageMask);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetEvent2(VkCommandBuffer                   commandBuffer, VkEvent                                             event, const VkDependencyInfo*                             pDependencyInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetEvent2;require(pfn);pfn(commandBuffer, event, pDependencyInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetEvent2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetEvent2");
 require(fn);
 fn(commandBuffer, event, pDependencyInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetEvent2KHR(VkCommandBuffer                   commandBuffer, VkEvent                                             event, const VkDependencyInfo*                             pDependencyInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetEvent2KHR;require(pfn);pfn(commandBuffer, event, pDependencyInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetEvent2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetEvent2KHR");
 require(fn);
 fn(commandBuffer, event, pDependencyInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetExclusiveScissorEnableNV(VkCommandBuffer commandBuffer, uint32_t firstExclusiveScissor, uint32_t exclusiveScissorCount, const VkBool32* pExclusiveScissorEnables) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetExclusiveScissorEnableNV;require(pfn);pfn(commandBuffer, firstExclusiveScissor, exclusiveScissorCount, pExclusiveScissorEnables);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetExclusiveScissorEnableNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetExclusiveScissorEnableNV");
 require(fn);
 fn(commandBuffer, firstExclusiveScissor, exclusiveScissorCount, pExclusiveScissorEnables);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetExclusiveScissorNV(VkCommandBuffer commandBuffer, uint32_t firstExclusiveScissor, uint32_t exclusiveScissorCount, const VkRect2D* pExclusiveScissors) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetExclusiveScissorNV;require(pfn);pfn(commandBuffer, firstExclusiveScissor, exclusiveScissorCount, pExclusiveScissors);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetExclusiveScissorNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetExclusiveScissorNV");
 require(fn);
 fn(commandBuffer, firstExclusiveScissor, exclusiveScissorCount, pExclusiveScissors);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetExtraPrimitiveOverestimationSizeEXT(VkCommandBuffer commandBuffer, float extraPrimitiveOverestimationSize) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetExtraPrimitiveOverestimationSizeEXT;require(pfn);pfn(commandBuffer, extraPrimitiveOverestimationSize);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetExtraPrimitiveOverestimationSizeEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetExtraPrimitiveOverestimationSizeEXT");
 require(fn);
 fn(commandBuffer, extraPrimitiveOverestimationSize);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetFragmentShadingRateEnumNV(VkCommandBuffer           commandBuffer, VkFragmentShadingRateNV                     shadingRate, const VkFragmentShadingRateCombinerOpKHR    combinerOps[2]) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetFragmentShadingRateEnumNV;require(pfn);pfn(commandBuffer, shadingRate, combinerOps);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetFragmentShadingRateEnumNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetFragmentShadingRateEnumNV");
 require(fn);
 fn(commandBuffer, shadingRate, combinerOps);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetFragmentShadingRateKHR(VkCommandBuffer           commandBuffer, const VkExtent2D*                           pFragmentSize, const VkFragmentShadingRateCombinerOpKHR    combinerOps[2]) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetFragmentShadingRateKHR;require(pfn);pfn(commandBuffer, pFragmentSize, combinerOps);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetFragmentShadingRateKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetFragmentShadingRateKHR");
 require(fn);
 fn(commandBuffer, pFragmentSize, combinerOps);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetFrontFace(VkCommandBuffer commandBuffer, VkFrontFace frontFace) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetFrontFace;require(pfn);pfn(commandBuffer, frontFace);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetFrontFace;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetFrontFace");
 require(fn);
 fn(commandBuffer, frontFace);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetFrontFaceEXT(VkCommandBuffer commandBuffer, VkFrontFace frontFace) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetFrontFaceEXT;require(pfn);pfn(commandBuffer, frontFace);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetFrontFaceEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetFrontFaceEXT");
 require(fn);
 fn(commandBuffer, frontFace);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetLineRasterizationModeEXT(VkCommandBuffer commandBuffer, VkLineRasterizationModeEXT lineRasterizationMode) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetLineRasterizationModeEXT;require(pfn);pfn(commandBuffer, lineRasterizationMode);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetLineRasterizationModeEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetLineRasterizationModeEXT");
 require(fn);
 fn(commandBuffer, lineRasterizationMode);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetLineStipple(VkCommandBuffer commandBuffer, uint32_t lineStippleFactor, uint16_t lineStipplePattern) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetLineStipple;require(pfn);pfn(commandBuffer, lineStippleFactor, lineStipplePattern);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetLineStipple;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetLineStipple");
 require(fn);
 fn(commandBuffer, lineStippleFactor, lineStipplePattern);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetLineStippleEXT(VkCommandBuffer commandBuffer, uint32_t lineStippleFactor, uint16_t lineStipplePattern) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetLineStippleEXT;require(pfn);pfn(commandBuffer, lineStippleFactor, lineStipplePattern);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetLineStippleEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetLineStippleEXT");
 require(fn);
 fn(commandBuffer, lineStippleFactor, lineStipplePattern);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetLineStippleEnableEXT(VkCommandBuffer commandBuffer, VkBool32 stippledLineEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetLineStippleEnableEXT;require(pfn);pfn(commandBuffer, stippledLineEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetLineStippleEnableEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetLineStippleEnableEXT");
 require(fn);
 fn(commandBuffer, stippledLineEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetLineStippleKHR(VkCommandBuffer commandBuffer, uint32_t lineStippleFactor, uint16_t lineStipplePattern) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetLineStippleKHR;require(pfn);pfn(commandBuffer, lineStippleFactor, lineStipplePattern);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetLineStippleKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetLineStippleKHR");
 require(fn);
 fn(commandBuffer, lineStippleFactor, lineStipplePattern);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetLineWidth(VkCommandBuffer commandBuffer, float lineWidth) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetLineWidth;require(pfn);pfn(commandBuffer, lineWidth);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetLineWidth;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetLineWidth");
 require(fn);
 fn(commandBuffer, lineWidth);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetLogicOpEXT(VkCommandBuffer commandBuffer, VkLogicOp logicOp) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetLogicOpEXT;require(pfn);pfn(commandBuffer, logicOp);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetLogicOpEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetLogicOpEXT");
 require(fn);
 fn(commandBuffer, logicOp);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetLogicOpEnableEXT(VkCommandBuffer commandBuffer, VkBool32 logicOpEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetLogicOpEnableEXT;require(pfn);pfn(commandBuffer, logicOpEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetLogicOpEnableEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetLogicOpEnableEXT");
 require(fn);
 fn(commandBuffer, logicOpEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetPatchControlPointsEXT(VkCommandBuffer commandBuffer, uint32_t patchControlPoints) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetPatchControlPointsEXT;require(pfn);pfn(commandBuffer, patchControlPoints);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetPatchControlPointsEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetPatchControlPointsEXT");
 require(fn);
 fn(commandBuffer, patchControlPoints);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCmdSetPerformanceMarkerINTEL(VkCommandBuffer commandBuffer, const VkPerformanceMarkerInfoINTEL* pMarkerInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetPerformanceMarkerINTEL;require(pfn);return pfn(commandBuffer, pMarkerInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetPerformanceMarkerINTEL;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetPerformanceMarkerINTEL");
 require(fn);
 return fn(commandBuffer, pMarkerInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCmdSetPerformanceOverrideINTEL(VkCommandBuffer commandBuffer, const VkPerformanceOverrideInfoINTEL* pOverrideInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetPerformanceOverrideINTEL;require(pfn);return pfn(commandBuffer, pOverrideInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetPerformanceOverrideINTEL;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetPerformanceOverrideINTEL");
 require(fn);
 return fn(commandBuffer, pOverrideInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCmdSetPerformanceStreamMarkerINTEL(VkCommandBuffer commandBuffer, const VkPerformanceStreamMarkerInfoINTEL* pMarkerInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetPerformanceStreamMarkerINTEL;require(pfn);return pfn(commandBuffer, pMarkerInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetPerformanceStreamMarkerINTEL;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetPerformanceStreamMarkerINTEL");
 require(fn);
 return fn(commandBuffer, pMarkerInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetPolygonModeEXT(VkCommandBuffer commandBuffer, VkPolygonMode polygonMode) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetPolygonModeEXT;require(pfn);pfn(commandBuffer, polygonMode);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetPolygonModeEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetPolygonModeEXT");
 require(fn);
 fn(commandBuffer, polygonMode);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetPrimitiveRestartEnable(VkCommandBuffer commandBuffer, VkBool32 primitiveRestartEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetPrimitiveRestartEnable;require(pfn);pfn(commandBuffer, primitiveRestartEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetPrimitiveRestartEnable;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetPrimitiveRestartEnable");
 require(fn);
 fn(commandBuffer, primitiveRestartEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetPrimitiveRestartEnableEXT(VkCommandBuffer commandBuffer, VkBool32 primitiveRestartEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetPrimitiveRestartEnableEXT;require(pfn);pfn(commandBuffer, primitiveRestartEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetPrimitiveRestartEnableEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetPrimitiveRestartEnableEXT");
 require(fn);
 fn(commandBuffer, primitiveRestartEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetPrimitiveRestartIndexEXT(VkCommandBuffer commandBuffer, uint32_t primitiveRestartIndex) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetPrimitiveRestartIndexEXT;require(pfn);pfn(commandBuffer, primitiveRestartIndex);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetPrimitiveRestartIndexEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetPrimitiveRestartIndexEXT");
 require(fn);
 fn(commandBuffer, primitiveRestartIndex);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetPrimitiveTopology(VkCommandBuffer commandBuffer, VkPrimitiveTopology primitiveTopology) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetPrimitiveTopology;require(pfn);pfn(commandBuffer, primitiveTopology);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetPrimitiveTopology;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetPrimitiveTopology");
 require(fn);
 fn(commandBuffer, primitiveTopology);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetPrimitiveTopologyEXT(VkCommandBuffer commandBuffer, VkPrimitiveTopology primitiveTopology) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetPrimitiveTopologyEXT;require(pfn);pfn(commandBuffer, primitiveTopology);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetPrimitiveTopologyEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetPrimitiveTopologyEXT");
 require(fn);
 fn(commandBuffer, primitiveTopology);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetProvokingVertexModeEXT(VkCommandBuffer commandBuffer, VkProvokingVertexModeEXT provokingVertexMode) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetProvokingVertexModeEXT;require(pfn);pfn(commandBuffer, provokingVertexMode);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetProvokingVertexModeEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetProvokingVertexModeEXT");
 require(fn);
 fn(commandBuffer, provokingVertexMode);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetRasterizationSamplesEXT(VkCommandBuffer commandBuffer, VkSampleCountFlagBits  rasterizationSamples) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetRasterizationSamplesEXT;require(pfn);pfn(commandBuffer, rasterizationSamples);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetRasterizationSamplesEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetRasterizationSamplesEXT");
 require(fn);
 fn(commandBuffer, rasterizationSamples);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetRasterizationStreamEXT(VkCommandBuffer commandBuffer, uint32_t rasterizationStream) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetRasterizationStreamEXT;require(pfn);pfn(commandBuffer, rasterizationStream);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetRasterizationStreamEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetRasterizationStreamEXT");
 require(fn);
 fn(commandBuffer, rasterizationStream);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetRasterizerDiscardEnable(VkCommandBuffer commandBuffer, VkBool32 rasterizerDiscardEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetRasterizerDiscardEnable;require(pfn);pfn(commandBuffer, rasterizerDiscardEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetRasterizerDiscardEnable;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetRasterizerDiscardEnable");
 require(fn);
 fn(commandBuffer, rasterizerDiscardEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetRasterizerDiscardEnableEXT(VkCommandBuffer commandBuffer, VkBool32 rasterizerDiscardEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetRasterizerDiscardEnableEXT;require(pfn);pfn(commandBuffer, rasterizerDiscardEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetRasterizerDiscardEnableEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetRasterizerDiscardEnableEXT");
 require(fn);
 fn(commandBuffer, rasterizerDiscardEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetRayTracingPipelineStackSizeKHR(VkCommandBuffer commandBuffer, uint32_t pipelineStackSize) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetRayTracingPipelineStackSizeKHR;require(pfn);pfn(commandBuffer, pipelineStackSize);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetRayTracingPipelineStackSizeKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetRayTracingPipelineStackSizeKHR");
 require(fn);
 fn(commandBuffer, pipelineStackSize);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetRenderingAttachmentLocations(VkCommandBuffer commandBuffer, const VkRenderingAttachmentLocationInfo* pLocationInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetRenderingAttachmentLocations;require(pfn);pfn(commandBuffer, pLocationInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetRenderingAttachmentLocations;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetRenderingAttachmentLocations");
 require(fn);
 fn(commandBuffer, pLocationInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetRenderingAttachmentLocationsKHR(VkCommandBuffer commandBuffer, const VkRenderingAttachmentLocationInfo* pLocationInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetRenderingAttachmentLocationsKHR;require(pfn);pfn(commandBuffer, pLocationInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetRenderingAttachmentLocationsKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetRenderingAttachmentLocationsKHR");
 require(fn);
 fn(commandBuffer, pLocationInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetRenderingInputAttachmentIndices(VkCommandBuffer commandBuffer, const VkRenderingInputAttachmentIndexInfo* pInputAttachmentIndexInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetRenderingInputAttachmentIndices;require(pfn);pfn(commandBuffer, pInputAttachmentIndexInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetRenderingInputAttachmentIndices;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetRenderingInputAttachmentIndices");
 require(fn);
 fn(commandBuffer, pInputAttachmentIndexInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetRenderingInputAttachmentIndicesKHR(VkCommandBuffer commandBuffer, const VkRenderingInputAttachmentIndexInfo* pInputAttachmentIndexInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetRenderingInputAttachmentIndicesKHR;require(pfn);pfn(commandBuffer, pInputAttachmentIndexInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetRenderingInputAttachmentIndicesKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetRenderingInputAttachmentIndicesKHR");
 require(fn);
 fn(commandBuffer, pInputAttachmentIndexInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetRepresentativeFragmentTestEnableNV(VkCommandBuffer commandBuffer, VkBool32 representativeFragmentTestEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetRepresentativeFragmentTestEnableNV;require(pfn);pfn(commandBuffer, representativeFragmentTestEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetRepresentativeFragmentTestEnableNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetRepresentativeFragmentTestEnableNV");
 require(fn);
 fn(commandBuffer, representativeFragmentTestEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetSampleLocationsEXT(VkCommandBuffer commandBuffer, const VkSampleLocationsInfoEXT* pSampleLocationsInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetSampleLocationsEXT;require(pfn);pfn(commandBuffer, pSampleLocationsInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetSampleLocationsEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetSampleLocationsEXT");
 require(fn);
 fn(commandBuffer, pSampleLocationsInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetSampleLocationsEnableEXT(VkCommandBuffer commandBuffer, VkBool32 sampleLocationsEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetSampleLocationsEnableEXT;require(pfn);pfn(commandBuffer, sampleLocationsEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetSampleLocationsEnableEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetSampleLocationsEnableEXT");
 require(fn);
 fn(commandBuffer, sampleLocationsEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetSampleMaskEXT(VkCommandBuffer commandBuffer, VkSampleCountFlagBits  samples, const VkSampleMask*    pSampleMask) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetSampleMaskEXT;require(pfn);pfn(commandBuffer, samples, pSampleMask);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetSampleMaskEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetSampleMaskEXT");
 require(fn);
 fn(commandBuffer, samples, pSampleMask);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetScissor(VkCommandBuffer commandBuffer, uint32_t firstScissor, uint32_t scissorCount, const VkRect2D* pScissors) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetScissor;require(pfn);pfn(commandBuffer, firstScissor, scissorCount, pScissors);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetScissor;
 if(t.pending76.command){const int savedErrno=errno;firstAdmissionBeforeCommand77(t,d,commandBuffer,"vkCmdSetScissor");errno=savedErrno;}
 if (t.command==commandBuffer && t.recording && d->worker) { if(d->worker->scissor(t.recording, commandBuffer, firstScissor, scissorCount, pScissors)) return; }
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetScissor",commandBuffer);
 require(fn);
 fn(commandBuffer, firstScissor, scissorCount, pScissors);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetScissorWithCount(VkCommandBuffer commandBuffer, uint32_t scissorCount, const VkRect2D* pScissors) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetScissorWithCount;require(pfn);pfn(commandBuffer, scissorCount, pScissors);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetScissorWithCount;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetScissorWithCount");
 require(fn);
 fn(commandBuffer, scissorCount, pScissors);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetScissorWithCountEXT(VkCommandBuffer commandBuffer, uint32_t scissorCount, const VkRect2D* pScissors) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetScissorWithCountEXT;require(pfn);pfn(commandBuffer, scissorCount, pScissors);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetScissorWithCountEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetScissorWithCountEXT");
 require(fn);
 fn(commandBuffer, scissorCount, pScissors);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetShadingRateImageEnableNV(VkCommandBuffer commandBuffer, VkBool32 shadingRateImageEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetShadingRateImageEnableNV;require(pfn);pfn(commandBuffer, shadingRateImageEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetShadingRateImageEnableNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetShadingRateImageEnableNV");
 require(fn);
 fn(commandBuffer, shadingRateImageEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetStencilCompareMask(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, uint32_t compareMask) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetStencilCompareMask;require(pfn);pfn(commandBuffer, faceMask, compareMask);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetStencilCompareMask;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetStencilCompareMask");
 require(fn);
 fn(commandBuffer, faceMask, compareMask);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetStencilOp(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, VkStencilOp failOp, VkStencilOp passOp, VkStencilOp depthFailOp, VkCompareOp compareOp) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetStencilOp;require(pfn);pfn(commandBuffer, faceMask, failOp, passOp, depthFailOp, compareOp);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetStencilOp;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetStencilOp");
 require(fn);
 fn(commandBuffer, faceMask, failOp, passOp, depthFailOp, compareOp);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetStencilOpEXT(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, VkStencilOp failOp, VkStencilOp passOp, VkStencilOp depthFailOp, VkCompareOp compareOp) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetStencilOpEXT;require(pfn);pfn(commandBuffer, faceMask, failOp, passOp, depthFailOp, compareOp);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetStencilOpEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetStencilOpEXT");
 require(fn);
 fn(commandBuffer, faceMask, failOp, passOp, depthFailOp, compareOp);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetStencilReference(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, uint32_t reference) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetStencilReference;require(pfn);pfn(commandBuffer, faceMask, reference);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetStencilReference;
 if(t.pending76.command){const int savedErrno=errno;firstAdmissionBeforeCommand77(t,d,commandBuffer,"vkCmdSetStencilReference");errno=savedErrno;}
 if (t.command==commandBuffer && t.recording && d->worker) { if(d->worker->stencil(t.recording, commandBuffer, faceMask, reference)) return; }
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetStencilReference",commandBuffer);
 require(fn);
 fn(commandBuffer, faceMask, reference);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetStencilTestEnable(VkCommandBuffer commandBuffer, VkBool32 stencilTestEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetStencilTestEnable;require(pfn);pfn(commandBuffer, stencilTestEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetStencilTestEnable;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetStencilTestEnable");
 require(fn);
 fn(commandBuffer, stencilTestEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetStencilTestEnableEXT(VkCommandBuffer commandBuffer, VkBool32 stencilTestEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetStencilTestEnableEXT;require(pfn);pfn(commandBuffer, stencilTestEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetStencilTestEnableEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetStencilTestEnableEXT");
 require(fn);
 fn(commandBuffer, stencilTestEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetStencilWriteMask(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, uint32_t writeMask) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetStencilWriteMask;require(pfn);pfn(commandBuffer, faceMask, writeMask);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetStencilWriteMask;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetStencilWriteMask");
 require(fn);
 fn(commandBuffer, faceMask, writeMask);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetTessellationDomainOriginEXT(VkCommandBuffer commandBuffer, VkTessellationDomainOrigin domainOrigin) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetTessellationDomainOriginEXT;require(pfn);pfn(commandBuffer, domainOrigin);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetTessellationDomainOriginEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetTessellationDomainOriginEXT");
 require(fn);
 fn(commandBuffer, domainOrigin);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetVertexInputEXT(VkCommandBuffer commandBuffer, uint32_t vertexBindingDescriptionCount, const VkVertexInputBindingDescription2EXT* pVertexBindingDescriptions, uint32_t vertexAttributeDescriptionCount, const VkVertexInputAttributeDescription2EXT* pVertexAttributeDescriptions) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetVertexInputEXT;require(pfn);pfn(commandBuffer, vertexBindingDescriptionCount, pVertexBindingDescriptions, vertexAttributeDescriptionCount, pVertexAttributeDescriptions);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetVertexInputEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetVertexInputEXT");
 require(fn);
 fn(commandBuffer, vertexBindingDescriptionCount, pVertexBindingDescriptions, vertexAttributeDescriptionCount, pVertexAttributeDescriptions);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetViewport(VkCommandBuffer commandBuffer, uint32_t firstViewport, uint32_t viewportCount, const VkViewport* pViewports) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetViewport;require(pfn);pfn(commandBuffer, firstViewport, viewportCount, pViewports);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetViewport;
 if(t.pending76.command){const int savedErrno=errno;firstAdmissionBeforeCommand77(t,d,commandBuffer,"vkCmdSetViewport");errno=savedErrno;}
 if (t.command==commandBuffer && t.recording && d->worker) { if(d->worker->viewport(t.recording, commandBuffer, firstViewport, viewportCount, pViewports)) return; }
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetViewport",commandBuffer);
 require(fn);
 fn(commandBuffer, firstViewport, viewportCount, pViewports);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetViewportShadingRatePaletteNV(VkCommandBuffer commandBuffer, uint32_t firstViewport, uint32_t viewportCount, const VkShadingRatePaletteNV* pShadingRatePalettes) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetViewportShadingRatePaletteNV;require(pfn);pfn(commandBuffer, firstViewport, viewportCount, pShadingRatePalettes);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetViewportShadingRatePaletteNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetViewportShadingRatePaletteNV");
 require(fn);
 fn(commandBuffer, firstViewport, viewportCount, pShadingRatePalettes);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetViewportSwizzleNV(VkCommandBuffer commandBuffer, uint32_t firstViewport, uint32_t viewportCount, const VkViewportSwizzleNV* pViewportSwizzles) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetViewportSwizzleNV;require(pfn);pfn(commandBuffer, firstViewport, viewportCount, pViewportSwizzles);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetViewportSwizzleNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetViewportSwizzleNV");
 require(fn);
 fn(commandBuffer, firstViewport, viewportCount, pViewportSwizzles);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetViewportWScalingEnableNV(VkCommandBuffer commandBuffer, VkBool32 viewportWScalingEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetViewportWScalingEnableNV;require(pfn);pfn(commandBuffer, viewportWScalingEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetViewportWScalingEnableNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetViewportWScalingEnableNV");
 require(fn);
 fn(commandBuffer, viewportWScalingEnable);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetViewportWScalingNV(VkCommandBuffer commandBuffer, uint32_t firstViewport, uint32_t viewportCount, const VkViewportWScalingNV* pViewportWScalings) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetViewportWScalingNV;require(pfn);pfn(commandBuffer, firstViewport, viewportCount, pViewportWScalings);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetViewportWScalingNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetViewportWScalingNV");
 require(fn);
 fn(commandBuffer, firstViewport, viewportCount, pViewportWScalings);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetViewportWithCount(VkCommandBuffer commandBuffer, uint32_t viewportCount, const VkViewport* pViewports) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetViewportWithCount;require(pfn);pfn(commandBuffer, viewportCount, pViewports);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetViewportWithCount;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetViewportWithCount");
 require(fn);
 fn(commandBuffer, viewportCount, pViewports);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSetViewportWithCountEXT(VkCommandBuffer commandBuffer, uint32_t viewportCount, const VkViewport* pViewports) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSetViewportWithCountEXT;require(pfn);pfn(commandBuffer, viewportCount, pViewports);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSetViewportWithCountEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSetViewportWithCountEXT");
 require(fn);
 fn(commandBuffer, viewportCount, pViewports);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdSubpassShadingHUAWEI(VkCommandBuffer commandBuffer) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdSubpassShadingHUAWEI;require(pfn);pfn(commandBuffer);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdSubpassShadingHUAWEI;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdSubpassShadingHUAWEI");
 require(fn);
 fn(commandBuffer);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdTraceRaysIndirect2KHR(VkCommandBuffer commandBuffer, VkDeviceAddress indirectDeviceAddress) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdTraceRaysIndirect2KHR;require(pfn);pfn(commandBuffer, indirectDeviceAddress);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdTraceRaysIndirect2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdTraceRaysIndirect2KHR");
 require(fn);
 fn(commandBuffer, indirectDeviceAddress);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdTraceRaysIndirectKHR(VkCommandBuffer commandBuffer, const VkStridedDeviceAddressRegionKHR* pRaygenShaderBindingTable, const VkStridedDeviceAddressRegionKHR* pMissShaderBindingTable, const VkStridedDeviceAddressRegionKHR* pHitShaderBindingTable, const VkStridedDeviceAddressRegionKHR* pCallableShaderBindingTable, VkDeviceAddress indirectDeviceAddress) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdTraceRaysIndirectKHR;require(pfn);pfn(commandBuffer, pRaygenShaderBindingTable, pMissShaderBindingTable, pHitShaderBindingTable, pCallableShaderBindingTable, indirectDeviceAddress);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdTraceRaysIndirectKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdTraceRaysIndirectKHR");
 require(fn);
 fn(commandBuffer, pRaygenShaderBindingTable, pMissShaderBindingTable, pHitShaderBindingTable, pCallableShaderBindingTable, indirectDeviceAddress);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdTraceRaysKHR(VkCommandBuffer commandBuffer, const VkStridedDeviceAddressRegionKHR* pRaygenShaderBindingTable, const VkStridedDeviceAddressRegionKHR* pMissShaderBindingTable, const VkStridedDeviceAddressRegionKHR* pHitShaderBindingTable, const VkStridedDeviceAddressRegionKHR* pCallableShaderBindingTable, uint32_t width, uint32_t height, uint32_t depth) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdTraceRaysKHR;require(pfn);pfn(commandBuffer, pRaygenShaderBindingTable, pMissShaderBindingTable, pHitShaderBindingTable, pCallableShaderBindingTable, width, height, depth);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdTraceRaysKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdTraceRaysKHR");
 require(fn);
 fn(commandBuffer, pRaygenShaderBindingTable, pMissShaderBindingTable, pHitShaderBindingTable, pCallableShaderBindingTable, width, height, depth);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdTraceRaysNV(VkCommandBuffer commandBuffer, VkBuffer raygenShaderBindingTableBuffer, VkDeviceSize raygenShaderBindingOffset, VkBuffer missShaderBindingTableBuffer, VkDeviceSize missShaderBindingOffset, VkDeviceSize missShaderBindingStride, VkBuffer hitShaderBindingTableBuffer, VkDeviceSize hitShaderBindingOffset, VkDeviceSize hitShaderBindingStride, VkBuffer callableShaderBindingTableBuffer, VkDeviceSize callableShaderBindingOffset, VkDeviceSize callableShaderBindingStride, uint32_t width, uint32_t height, uint32_t depth) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdTraceRaysNV;require(pfn);pfn(commandBuffer, raygenShaderBindingTableBuffer, raygenShaderBindingOffset, missShaderBindingTableBuffer, missShaderBindingOffset, missShaderBindingStride, hitShaderBindingTableBuffer, hitShaderBindingOffset, hitShaderBindingStride, callableShaderBindingTableBuffer, callableShaderBindingOffset, callableShaderBindingStride, width, height, depth);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdTraceRaysNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdTraceRaysNV");
 require(fn);
 fn(commandBuffer, raygenShaderBindingTableBuffer, raygenShaderBindingOffset, missShaderBindingTableBuffer, missShaderBindingOffset, missShaderBindingStride, hitShaderBindingTableBuffer, hitShaderBindingOffset, hitShaderBindingStride, callableShaderBindingTableBuffer, callableShaderBindingOffset, callableShaderBindingStride, width, height, depth);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdUpdateBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize dataSize, const void* pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdUpdateBuffer;require(pfn);pfn(commandBuffer, dstBuffer, dstOffset, dataSize, pData);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdUpdateBuffer;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdUpdateBuffer");
 require(fn);
 fn(commandBuffer, dstBuffer, dstOffset, dataSize, pData);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdUpdateMemoryKHR(VkCommandBuffer   commandBuffer, const VkDeviceAddressRangeKHR*      pDstRange, VkAddressCommandFlagsKHR dstFlags, VkDeviceSize                        dataSize, const void*          pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdUpdateMemoryKHR;require(pfn);pfn(commandBuffer, pDstRange, dstFlags, dataSize, pData);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdUpdateMemoryKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdUpdateMemoryKHR");
 require(fn);
 fn(commandBuffer, pDstRange, dstFlags, dataSize, pData);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdUpdatePipelineIndirectBufferNV(VkCommandBuffer commandBuffer, VkPipelineBindPoint           pipelineBindPoint, VkPipeline                    pipeline) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdUpdatePipelineIndirectBufferNV;require(pfn);pfn(commandBuffer, pipelineBindPoint, pipeline);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdUpdatePipelineIndirectBufferNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdUpdatePipelineIndirectBufferNV");
 require(fn);
 fn(commandBuffer, pipelineBindPoint, pipeline);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdWaitEvents(VkCommandBuffer commandBuffer, uint32_t eventCount, const VkEvent* pEvents, VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask, uint32_t memoryBarrierCount, const VkMemoryBarrier* pMemoryBarriers, uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier* pBufferMemoryBarriers, uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier* pImageMemoryBarriers) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdWaitEvents;require(pfn);pfn(commandBuffer, eventCount, pEvents, srcStageMask, dstStageMask, memoryBarrierCount, pMemoryBarriers, bufferMemoryBarrierCount, pBufferMemoryBarriers, imageMemoryBarrierCount, pImageMemoryBarriers);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdWaitEvents;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdWaitEvents");
 require(fn);
 fn(commandBuffer, eventCount, pEvents, srcStageMask, dstStageMask, memoryBarrierCount, pMemoryBarriers, bufferMemoryBarrierCount, pBufferMemoryBarriers, imageMemoryBarrierCount, pImageMemoryBarriers);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdWaitEvents2(VkCommandBuffer                   commandBuffer, uint32_t                                            eventCount, const VkEvent*                     pEvents, const VkDependencyInfo*            pDependencyInfos) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdWaitEvents2;require(pfn);pfn(commandBuffer, eventCount, pEvents, pDependencyInfos);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdWaitEvents2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdWaitEvents2");
 require(fn);
 fn(commandBuffer, eventCount, pEvents, pDependencyInfos);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdWaitEvents2KHR(VkCommandBuffer                   commandBuffer, uint32_t                                            eventCount, const VkEvent*                     pEvents, const VkDependencyInfo*            pDependencyInfos) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdWaitEvents2KHR;require(pfn);pfn(commandBuffer, eventCount, pEvents, pDependencyInfos);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdWaitEvents2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdWaitEvents2KHR");
 require(fn);
 fn(commandBuffer, eventCount, pEvents, pDependencyInfos);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdWriteAccelerationStructuresPropertiesKHR(VkCommandBuffer commandBuffer, uint32_t accelerationStructureCount, const VkAccelerationStructureKHR* pAccelerationStructures, VkQueryType queryType, VkQueryPool queryPool, uint32_t firstQuery) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdWriteAccelerationStructuresPropertiesKHR;require(pfn);pfn(commandBuffer, accelerationStructureCount, pAccelerationStructures, queryType, queryPool, firstQuery);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdWriteAccelerationStructuresPropertiesKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdWriteAccelerationStructuresPropertiesKHR");
 require(fn);
 fn(commandBuffer, accelerationStructureCount, pAccelerationStructures, queryType, queryPool, firstQuery);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdWriteAccelerationStructuresPropertiesNV(VkCommandBuffer commandBuffer, uint32_t accelerationStructureCount, const VkAccelerationStructureNV* pAccelerationStructures, VkQueryType queryType, VkQueryPool queryPool, uint32_t firstQuery) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdWriteAccelerationStructuresPropertiesNV;require(pfn);pfn(commandBuffer, accelerationStructureCount, pAccelerationStructures, queryType, queryPool, firstQuery);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdWriteAccelerationStructuresPropertiesNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdWriteAccelerationStructuresPropertiesNV");
 require(fn);
 fn(commandBuffer, accelerationStructureCount, pAccelerationStructures, queryType, queryPool, firstQuery);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdWriteBufferMarker2AMD(VkCommandBuffer                   commandBuffer, VkPipelineStageFlags2               stage, VkBuffer                                            dstBuffer, VkDeviceSize                                        dstOffset, uint32_t                                            marker) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdWriteBufferMarker2AMD;require(pfn);pfn(commandBuffer, stage, dstBuffer, dstOffset, marker);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdWriteBufferMarker2AMD;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdWriteBufferMarker2AMD");
 require(fn);
 fn(commandBuffer, stage, dstBuffer, dstOffset, marker);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdWriteBufferMarkerAMD(VkCommandBuffer commandBuffer, VkPipelineStageFlagBits pipelineStage, VkBuffer dstBuffer, VkDeviceSize dstOffset, uint32_t marker) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdWriteBufferMarkerAMD;require(pfn);pfn(commandBuffer, pipelineStage, dstBuffer, dstOffset, marker);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdWriteBufferMarkerAMD;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdWriteBufferMarkerAMD");
 require(fn);
 fn(commandBuffer, pipelineStage, dstBuffer, dstOffset, marker);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdWriteMarkerToMemoryAMD(VkCommandBuffer   commandBuffer, const VkMemoryMarkerInfoAMD*         pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdWriteMarkerToMemoryAMD;require(pfn);pfn(commandBuffer, pInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdWriteMarkerToMemoryAMD;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdWriteMarkerToMemoryAMD");
 require(fn);
 fn(commandBuffer, pInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdWriteMicromapsPropertiesEXT(VkCommandBuffer commandBuffer, uint32_t                                 micromapCount, const VkMicromapEXT* pMicromaps, VkQueryType        queryType, VkQueryPool                              queryPool, uint32_t                                 firstQuery) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdWriteMicromapsPropertiesEXT;require(pfn);pfn(commandBuffer, micromapCount, pMicromaps, queryType, queryPool, firstQuery);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdWriteMicromapsPropertiesEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdWriteMicromapsPropertiesEXT");
 require(fn);
 fn(commandBuffer, micromapCount, pMicromaps, queryType, queryPool, firstQuery);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdWriteTimestamp(VkCommandBuffer commandBuffer, VkPipelineStageFlagBits pipelineStage, VkQueryPool queryPool, uint32_t query) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdWriteTimestamp;require(pfn);pfn(commandBuffer, pipelineStage, queryPool, query);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdWriteTimestamp;
 if(t.pending76.command){
  const int incomingErrno=errno;const bool pending=startPendingTimestamp79(d,commandBuffer,pipelineStage,queryPool,query);errno=incomingErrno;
  if(pending){
   {auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdWriteTimestamp",commandBuffer);require(fn);fn(commandBuffer,pipelineStage,queryPool,query);}
   const int savedErrno=errno;finishPendingTimestamp79(d);errno=savedErrno;return;
  }
 }
 if (t.command==commandBuffer && t.recording && d->worker) { if(d->worker->writeTimestamp(t.recording, commandBuffer, pipelineStage, queryPool, query)) return; }
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdWriteTimestamp",commandBuffer);
 require(fn);
 fn(commandBuffer, pipelineStage, queryPool, query);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdWriteTimestamp2(VkCommandBuffer                   commandBuffer, VkPipelineStageFlags2               stage, VkQueryPool                                         queryPool, uint32_t                                            query) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdWriteTimestamp2;require(pfn);pfn(commandBuffer, stage, queryPool, query);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdWriteTimestamp2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdWriteTimestamp2");
 require(fn);
 fn(commandBuffer, stage, queryPool, query);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkCmdWriteTimestamp2KHR(VkCommandBuffer                   commandBuffer, VkPipelineStageFlags2               stage, VkQueryPool                                         queryPool, uint32_t                                            query) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(commandBuffer);auto pfn=p->dispatch.vkCmdWriteTimestamp2KHR;require(pfn);pfn(commandBuffer, stage, queryPool, query);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,commandBuffer); auto fn=d->dispatch.vkCmdWriteTimestamp2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCmdWriteTimestamp2KHR");
 require(fn);
 fn(commandBuffer, stage, queryPool, query);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCompileDeferredNV(VkDevice device, VkPipeline pipeline, uint32_t shader) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCompileDeferredNV;require(pfn);return pfn(device, pipeline, shader);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCompileDeferredNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCompileDeferredNV");
 require(fn);
 return fn(device, pipeline, shader);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkConvertCooperativeVectorMatrixNV(VkDevice device, const VkConvertCooperativeVectorMatrixInfoNV* pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkConvertCooperativeVectorMatrixNV;require(pfn);return pfn(device, pInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkConvertCooperativeVectorMatrixNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkConvertCooperativeVectorMatrixNV");
 require(fn);
 return fn(device, pInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCopyAccelerationStructureKHR(VkDevice device, VkDeferredOperationKHR deferredOperation, const VkCopyAccelerationStructureInfoKHR* pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCopyAccelerationStructureKHR;require(pfn);return pfn(device, deferredOperation, pInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCopyAccelerationStructureKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCopyAccelerationStructureKHR");
 require(fn);
 return fn(device, deferredOperation, pInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCopyAccelerationStructureToMemoryKHR(VkDevice device, VkDeferredOperationKHR deferredOperation, const VkCopyAccelerationStructureToMemoryInfoKHR* pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCopyAccelerationStructureToMemoryKHR;require(pfn);return pfn(device, deferredOperation, pInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCopyAccelerationStructureToMemoryKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCopyAccelerationStructureToMemoryKHR");
 require(fn);
 return fn(device, deferredOperation, pInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCopyImageToImage(VkDevice device, const VkCopyImageToImageInfo*    pCopyImageToImageInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCopyImageToImage;require(pfn);return pfn(device, pCopyImageToImageInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCopyImageToImage;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCopyImageToImage");
 require(fn);
 return fn(device, pCopyImageToImageInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCopyImageToImageEXT(VkDevice device, const VkCopyImageToImageInfo*    pCopyImageToImageInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCopyImageToImageEXT;require(pfn);return pfn(device, pCopyImageToImageInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCopyImageToImageEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCopyImageToImageEXT");
 require(fn);
 return fn(device, pCopyImageToImageInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCopyImageToMemory(VkDevice device, const VkCopyImageToMemoryInfo*    pCopyImageToMemoryInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCopyImageToMemory;require(pfn);return pfn(device, pCopyImageToMemoryInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCopyImageToMemory;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCopyImageToMemory");
 require(fn);
 return fn(device, pCopyImageToMemoryInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCopyImageToMemoryEXT(VkDevice device, const VkCopyImageToMemoryInfo*    pCopyImageToMemoryInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCopyImageToMemoryEXT;require(pfn);return pfn(device, pCopyImageToMemoryInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCopyImageToMemoryEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCopyImageToMemoryEXT");
 require(fn);
 return fn(device, pCopyImageToMemoryInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCopyMemoryToAccelerationStructureKHR(VkDevice device, VkDeferredOperationKHR deferredOperation, const VkCopyMemoryToAccelerationStructureInfoKHR* pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCopyMemoryToAccelerationStructureKHR;require(pfn);return pfn(device, deferredOperation, pInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCopyMemoryToAccelerationStructureKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCopyMemoryToAccelerationStructureKHR");
 require(fn);
 return fn(device, deferredOperation, pInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCopyMemoryToImage(VkDevice device, const VkCopyMemoryToImageInfo*    pCopyMemoryToImageInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCopyMemoryToImage;require(pfn);return pfn(device, pCopyMemoryToImageInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCopyMemoryToImage;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCopyMemoryToImage");
 require(fn);
 return fn(device, pCopyMemoryToImageInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCopyMemoryToImageEXT(VkDevice device, const VkCopyMemoryToImageInfo*    pCopyMemoryToImageInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCopyMemoryToImageEXT;require(pfn);return pfn(device, pCopyMemoryToImageInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCopyMemoryToImageEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCopyMemoryToImageEXT");
 require(fn);
 return fn(device, pCopyMemoryToImageInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCopyMemoryToMicromapEXT(VkDevice                               device, VkDeferredOperationKHR deferredOperation, const VkCopyMemoryToMicromapInfoEXT*   pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCopyMemoryToMicromapEXT;require(pfn);return pfn(device, deferredOperation, pInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCopyMemoryToMicromapEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCopyMemoryToMicromapEXT");
 require(fn);
 return fn(device, deferredOperation, pInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCopyMicromapEXT(VkDevice                               device, VkDeferredOperationKHR deferredOperation, const VkCopyMicromapInfoEXT*           pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCopyMicromapEXT;require(pfn);return pfn(device, deferredOperation, pInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCopyMicromapEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCopyMicromapEXT");
 require(fn);
 return fn(device, deferredOperation, pInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCopyMicromapToMemoryEXT(VkDevice                               device, VkDeferredOperationKHR deferredOperation, const VkCopyMicromapToMemoryInfoEXT*   pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCopyMicromapToMemoryEXT;require(pfn);return pfn(device, deferredOperation, pInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCopyMicromapToMemoryEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCopyMicromapToMemoryEXT");
 require(fn);
 return fn(device, deferredOperation, pInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateAccelerationStructure2KHR(VkDevice                                     device, const VkAccelerationStructureCreateInfo2KHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkAccelerationStructureKHR*                  pAccelerationStructure) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateAccelerationStructure2KHR;require(pfn);return pfn(device, pCreateInfo, pAllocator, pAccelerationStructure);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateAccelerationStructure2KHR;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateAccelerationStructure2KHR");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pAccelerationStructure);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateAccelerationStructureKHR(VkDevice                                           device, const VkAccelerationStructureCreateInfoKHR*        pCreateInfo, const VkAllocationCallbacks*       pAllocator, VkAccelerationStructureKHR*                        pAccelerationStructure) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateAccelerationStructureKHR;require(pfn);return pfn(device, pCreateInfo, pAllocator, pAccelerationStructure);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateAccelerationStructureKHR;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateAccelerationStructureKHR");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pAccelerationStructure);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateAccelerationStructureNV(VkDevice device, const VkAccelerationStructureCreateInfoNV* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkAccelerationStructureNV* pAccelerationStructure) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateAccelerationStructureNV;require(pfn);return pfn(device, pCreateInfo, pAllocator, pAccelerationStructure);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateAccelerationStructureNV;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateAccelerationStructureNV");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pAccelerationStructure);
}
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateAndroidSurfaceKHR(VkInstance instance, const VkAndroidSurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(i->next(i->handle,"vkCreateAndroidSurfaceKHR"));
 InstanceEntries entries(i);
 if(!allocatorSafe(pAllocator)) { i->qualified=false; i->decline=std::uint32_t(Decline::Allocator); entries.revoke(Decline::Allocator); }
 require(fn);
 return fn(instance, pCreateInfo, pAllocator, pSurface);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateBuffer(VkDevice device, const VkBufferCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkBuffer* pBuffer) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateBuffer;require(pfn);return pfn(device, pCreateInfo, pAllocator, pBuffer);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateBuffer;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateBuffer");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pBuffer);
}
#if defined(VK_USE_PLATFORM_FUCHSIA)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateBufferCollectionFUCHSIA(VkDevice device, const VkBufferCollectionCreateInfoFUCHSIA* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkBufferCollectionFUCHSIA* pCollection) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateBufferCollectionFUCHSIA;require(pfn);return pfn(device, pCreateInfo, pAllocator, pCollection);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateBufferCollectionFUCHSIA;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateBufferCollectionFUCHSIA");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pCollection);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateBufferView(VkDevice device, const VkBufferViewCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkBufferView* pView) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateBufferView;require(pfn);return pfn(device, pCreateInfo, pAllocator, pView);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateBufferView;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateBufferView");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pView);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkComputePipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateComputePipelines;require(pfn);return pfn(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateComputePipelines;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateComputePipelines");
 require(fn);
 return fn(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateCuFunctionNVX(VkDevice device, const VkCuFunctionCreateInfoNVX* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkCuFunctionNVX* pFunction) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateCuFunctionNVX;require(pfn);return pfn(device, pCreateInfo, pAllocator, pFunction);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateCuFunctionNVX;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateCuFunctionNVX");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pFunction);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateCuModuleNVX(VkDevice device, const VkCuModuleCreateInfoNVX* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkCuModuleNVX* pModule) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateCuModuleNVX;require(pfn);return pfn(device, pCreateInfo, pAllocator, pModule);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateCuModuleNVX;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateCuModuleNVX");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pModule);
}
#if defined(VK_ENABLE_BETA_EXTENSIONS)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateCudaFunctionNV(VkDevice device, const VkCudaFunctionCreateInfoNV* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkCudaFunctionNV* pFunction) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateCudaFunctionNV;require(pfn);return pfn(device, pCreateInfo, pAllocator, pFunction);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateCudaFunctionNV;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateCudaFunctionNV");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pFunction);
}
#endif
#if defined(VK_ENABLE_BETA_EXTENSIONS)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateCudaModuleNV(VkDevice device, const VkCudaModuleCreateInfoNV* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkCudaModuleNV* pModule) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateCudaModuleNV;require(pfn);return pfn(device, pCreateInfo, pAllocator, pModule);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateCudaModuleNV;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateCudaModuleNV");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pModule);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateDataGraphPipelineSessionARM(VkDevice                                     device, const VkDataGraphPipelineSessionCreateInfoARM*   pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDataGraphPipelineSessionARM*                   pSession) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateDataGraphPipelineSessionARM;require(pfn);return pfn(device, pCreateInfo, pAllocator, pSession);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateDataGraphPipelineSessionARM;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateDataGraphPipelineSessionARM");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pSession);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateDataGraphPipelinesARM(VkDevice               device, VkDeferredOperationKHR deferredOperation, VkPipelineCache        pipelineCache, uint32_t               createInfoCount, const VkDataGraphPipelineCreateInfoARM* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline*     pPipelines) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateDataGraphPipelinesARM;require(pfn);return pfn(device, deferredOperation, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateDataGraphPipelinesARM;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateDataGraphPipelinesARM");
 require(fn);
 return fn(device, deferredOperation, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateDebugReportCallbackEXT(VkInstance instance, const VkDebugReportCallbackCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDebugReportCallbackEXT* pCallback) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkCreateDebugReportCallbackEXT>(i->next(i->handle,"vkCreateDebugReportCallbackEXT"));
 InstanceEntries entries(i);
 if(!allocatorSafe(pAllocator)) { i->qualified=false; i->decline=std::uint32_t(Decline::Allocator); entries.revoke(Decline::Allocator); }
 if(!standDebugReport(pCreateInfo)) { i->qualified=false; i->decline=std::uint32_t(Decline::Callback); entries.revoke(Decline::Callback); }
 require(fn);
 return fn(instance, pCreateInfo, pAllocator, pCallback);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pMessenger) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(i->next(i->handle,"vkCreateDebugUtilsMessengerEXT"));
 InstanceEntries entries(i);
 if(!allocatorSafe(pAllocator)) { i->qualified=false; i->decline=std::uint32_t(Decline::Allocator); entries.revoke(Decline::Allocator); }
 if(!standDebugUtils(pCreateInfo)) { i->qualified=false; i->decline=std::uint32_t(Decline::Callback); entries.revoke(Decline::Callback); }
 require(fn);
 return fn(instance, pCreateInfo, pAllocator, pMessenger);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateDeferredOperationKHR(VkDevice device, const VkAllocationCallbacks* pAllocator, VkDeferredOperationKHR* pDeferredOperation) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateDeferredOperationKHR;require(pfn);return pfn(device, pAllocator, pDeferredOperation);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateDeferredOperationKHR;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateDeferredOperationKHR");
 require(fn);
 return fn(device, pAllocator, pDeferredOperation);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateDescriptorPool(VkDevice device,const VkDescriptorPoolCreateInfo*info,const VkAllocationCallbacks*allocator,VkDescriptorPool*out){
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateDescriptorPool;require(pfn);return pfn(device, info, allocator, out);}
 auto*d=deviceFor(device);auto fn=d->dispatch.vkCreateDescriptorPool;require(fn);const int incoming=errno;
 if(!allocatorSafe(allocator))revoke(d,Decline::Allocator);
 const bool shape=out&&descriptorPoolShape81(info,allocator);const auto observation82=poolObservation82(info,allocator,shape);errno=incoming;
 if(!shape){auto entry=enter(d,Worker::AccessKind::Resource,"vkCreateDescriptorPool");const auto originalEpoch82=d->descriptorEpoch81.load(std::memory_order_acquire);errno=incoming;
  PoolBirth87 birth87(d);const auto result=fn(device,info,allocator,out);const int saved=errno;entry={};
  birth87.complete(result,result==VK_SUCCESS&&out?*out:VkDescriptorPool{},false,observation82,originalEpoch82);errno=saved;return result;
 }
 ActivePreparation81 entry(d,"vkCreateDescriptorPool");const auto originalEpoch=d->descriptorEpoch81.load(std::memory_order_acquire);
 PoolBirth87 birth87(d);errno=incoming;const auto result=fn(device,info,allocator,out);
 const int saved=errno;const bool preserved=entry.complete();
 if(result==VK_SUCCESS&&*out)birth87.complete(result,*out,true,observation82,originalEpoch);
 else birth87.complete(result,VkDescriptorPool{},true,observation82,originalEpoch);
 if((result!=VK_SUCCESS||!*out)&&preserved){auto drain=enter(d,Worker::AccessKind::Resource,"descriptorPoolCreationFailure");}
 errno=saved;return result;
}

VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateDescriptorSetLayout(VkDevice device,const VkDescriptorSetLayoutCreateInfo*info,const VkAllocationCallbacks*allocator,VkDescriptorSetLayout*out){
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateDescriptorSetLayout;require(pfn);return pfn(device, info, allocator, out);}
 auto*d=deviceFor(device);auto fn=d->dispatch.vkCreateDescriptorSetLayout;require(fn);const int incoming=errno;
 const bool safe=allocatorSafe(allocator);if(!safe)revoke(d,Decline::Allocator);
 DescriptorLayout81 shape;const bool qualified=safe&&out&&descriptorLayoutShape81(info,shape);const auto observation82=layoutObservation82(info,qualified);
 auto entry=enter(d,Worker::AccessKind::Resource,"vkCreateDescriptorSetLayout");const auto originalEpoch=d->descriptorEpoch81.load(std::memory_order_acquire);errno=incoming;
 const auto result=fn(device,info,allocator,out);const int saved=errno;
 entry={};
 if(result==VK_SUCCESS&&out&&*out){createdDescriptorLayout81(d,*out,shape,qualified,originalEpoch);createdLayoutObservation82(d,*out,observation82,originalEpoch);}
 errno=saved;return result;
}

VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateDescriptorUpdateTemplate(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateDescriptorUpdateTemplate;require(pfn);return pfn(device, pCreateInfo, pAllocator, pDescriptorUpdateTemplate);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateDescriptorUpdateTemplate;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateDescriptorUpdateTemplate");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pDescriptorUpdateTemplate);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateDescriptorUpdateTemplateKHR(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateDescriptorUpdateTemplateKHR;require(pfn);return pfn(device, pCreateInfo, pAllocator, pDescriptorUpdateTemplate);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateDescriptorUpdateTemplateKHR;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateDescriptorUpdateTemplateKHR");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pDescriptorUpdateTemplate);
}
#if defined(VK_USE_PLATFORM_DIRECTFB_EXT)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateDirectFBSurfaceEXT(VkInstance instance, const VkDirectFBSurfaceCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkCreateDirectFBSurfaceEXT>(i->next(i->handle,"vkCreateDirectFBSurfaceEXT"));
 InstanceEntries entries(i);
 if(!allocatorSafe(pAllocator)) { i->qualified=false; i->decline=std::uint32_t(Decline::Allocator); entries.revoke(Decline::Allocator); }
 require(fn);
 return fn(instance, pCreateInfo, pAllocator, pSurface);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateDisplayModeKHR(VkPhysicalDevice physicalDevice, VkDisplayKHR display, const VkDisplayModeCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDisplayModeKHR* pMode) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkCreateDisplayModeKHR>(i->next(i->handle,"vkCreateDisplayModeKHR"));
 InstanceEntries entries(i);
 if(!allocatorSafe(pAllocator)) { i->qualified=false; i->decline=std::uint32_t(Decline::Allocator); entries.revoke(Decline::Allocator); }
 require(fn);
 return fn(physicalDevice, display, pCreateInfo, pAllocator, pMode);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateDisplayPlaneSurfaceKHR(VkInstance instance, const VkDisplaySurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkCreateDisplayPlaneSurfaceKHR>(i->next(i->handle,"vkCreateDisplayPlaneSurfaceKHR"));
 InstanceEntries entries(i);
 if(!allocatorSafe(pAllocator)) { i->qualified=false; i->decline=std::uint32_t(Decline::Allocator); entries.revoke(Decline::Allocator); }
 require(fn);
 return fn(instance, pCreateInfo, pAllocator, pSurface);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateEvent(VkDevice device, const VkEventCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkEvent* pEvent) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateEvent;require(pfn);return pfn(device, pCreateInfo, pAllocator, pEvent);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateEvent;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateEvent");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pEvent);
}
#if defined(VK_ENABLE_BETA_EXTENSIONS)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateExecutionGraphPipelinesAMDX(VkDevice                                        device, VkPipelineCache pipelineCache, uint32_t                                        createInfoCount, const VkExecutionGraphPipelineCreateInfoAMDX* pCreateInfos, const VkAllocationCallbacks*    pAllocator, VkPipeline*               pPipelines) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateExecutionGraphPipelinesAMDX;require(pfn);return pfn(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateExecutionGraphPipelinesAMDX;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateExecutionGraphPipelinesAMDX");
 require(fn);
 return fn(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateExternalComputeQueueNV(VkDevice device, const VkExternalComputeQueueCreateInfoNV* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkExternalComputeQueueNV* pExternalQueue) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateExternalComputeQueueNV;require(pfn);return pfn(device, pCreateInfo, pAllocator, pExternalQueue);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateExternalComputeQueueNV;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateExternalComputeQueueNV");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pExternalQueue);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateFence(VkDevice device, const VkFenceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkFence* pFence) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateFence;require(pfn);return pfn(device, pCreateInfo, pAllocator, pFence);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateFence;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateFence");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pFence);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkFramebuffer* pFramebuffer) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateFramebuffer;require(pfn);return pfn(device, pCreateInfo, pAllocator, pFramebuffer);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateFramebuffer;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateFramebuffer");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pFramebuffer);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateGpaSessionAMD(VkDevice                                     device, const VkGpaSessionCreateInfoAMD*             pCreateInfo, const VkAllocationCallbacks* pAllocator, VkGpaSessionAMD*                             pGpaSession) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateGpaSessionAMD;require(pfn);return pfn(device, pCreateInfo, pAllocator, pGpaSession);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateGpaSessionAMD;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateGpaSessionAMD");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pGpaSession);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkGraphicsPipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateGraphicsPipelines;require(pfn);return pfn(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateGraphicsPipelines;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateGraphicsPipelines");
 require(fn);
 return fn(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateHeadlessSurfaceEXT(VkInstance instance, const VkHeadlessSurfaceCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(i->next(i->handle,"vkCreateHeadlessSurfaceEXT"));
 InstanceEntries entries(i);
 if(!allocatorSafe(pAllocator)) { i->qualified=false; i->decline=std::uint32_t(Decline::Allocator); entries.revoke(Decline::Allocator); }
 require(fn);
 return fn(instance, pCreateInfo, pAllocator, pSurface);
}
#if defined(VK_USE_PLATFORM_IOS_MVK)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateIOSSurfaceMVK(VkInstance instance, const VkIOSSurfaceCreateInfoMVK* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkCreateIOSSurfaceMVK>(i->next(i->handle,"vkCreateIOSSurfaceMVK"));
 InstanceEntries entries(i);
 if(!allocatorSafe(pAllocator)) { i->qualified=false; i->decline=std::uint32_t(Decline::Allocator); entries.revoke(Decline::Allocator); }
 require(fn);
 return fn(instance, pCreateInfo, pAllocator, pSurface);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateImage(VkDevice device, const VkImageCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkImage* pImage) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateImage;require(pfn);return pfn(device, pCreateInfo, pAllocator, pImage);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateImage;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateImage");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pImage);
}
#if defined(VK_USE_PLATFORM_FUCHSIA)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateImagePipeSurfaceFUCHSIA(VkInstance instance, const VkImagePipeSurfaceCreateInfoFUCHSIA* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkCreateImagePipeSurfaceFUCHSIA>(i->next(i->handle,"vkCreateImagePipeSurfaceFUCHSIA"));
 InstanceEntries entries(i);
 if(!allocatorSafe(pAllocator)) { i->qualified=false; i->decline=std::uint32_t(Decline::Allocator); entries.revoke(Decline::Allocator); }
 require(fn);
 return fn(instance, pCreateInfo, pAllocator, pSurface);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateImageView(VkDevice device, const VkImageViewCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkImageView* pView) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateImageView;require(pfn);return pfn(device, pCreateInfo, pAllocator, pView);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateImageView;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateImageView");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pView);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateIndirectCommandsLayoutEXT(VkDevice device, const VkIndirectCommandsLayoutCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkIndirectCommandsLayoutEXT* pIndirectCommandsLayout) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateIndirectCommandsLayoutEXT;require(pfn);return pfn(device, pCreateInfo, pAllocator, pIndirectCommandsLayout);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateIndirectCommandsLayoutEXT;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateIndirectCommandsLayoutEXT");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pIndirectCommandsLayout);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateIndirectCommandsLayoutNV(VkDevice device, const VkIndirectCommandsLayoutCreateInfoNV* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkIndirectCommandsLayoutNV* pIndirectCommandsLayout) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateIndirectCommandsLayoutNV;require(pfn);return pfn(device, pCreateInfo, pAllocator, pIndirectCommandsLayout);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateIndirectCommandsLayoutNV;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateIndirectCommandsLayoutNV");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pIndirectCommandsLayout);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateIndirectExecutionSetEXT(VkDevice device, const VkIndirectExecutionSetCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkIndirectExecutionSetEXT* pIndirectExecutionSet) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateIndirectExecutionSetEXT;require(pfn);return pfn(device, pCreateInfo, pAllocator, pIndirectExecutionSet);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateIndirectExecutionSetEXT;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateIndirectExecutionSetEXT");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pIndirectExecutionSet);
}
#if defined(VK_USE_PLATFORM_MACOS_MVK)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateMacOSSurfaceMVK(VkInstance instance, const VkMacOSSurfaceCreateInfoMVK* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkCreateMacOSSurfaceMVK>(i->next(i->handle,"vkCreateMacOSSurfaceMVK"));
 InstanceEntries entries(i);
 if(!allocatorSafe(pAllocator)) { i->qualified=false; i->decline=std::uint32_t(Decline::Allocator); entries.revoke(Decline::Allocator); }
 require(fn);
 return fn(instance, pCreateInfo, pAllocator, pSurface);
}
#endif
#if defined(VK_USE_PLATFORM_METAL_EXT)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateMetalSurfaceEXT(VkInstance instance, const VkMetalSurfaceCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(i->next(i->handle,"vkCreateMetalSurfaceEXT"));
 InstanceEntries entries(i);
 if(!allocatorSafe(pAllocator)) { i->qualified=false; i->decline=std::uint32_t(Decline::Allocator); entries.revoke(Decline::Allocator); }
 require(fn);
 return fn(instance, pCreateInfo, pAllocator, pSurface);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateMicromapEXT(VkDevice                                           device, const VkMicromapCreateInfoEXT*                     pCreateInfo, const VkAllocationCallbacks*       pAllocator, VkMicromapEXT*                                     pMicromap) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateMicromapEXT;require(pfn);return pfn(device, pCreateInfo, pAllocator, pMicromap);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateMicromapEXT;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateMicromapEXT");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pMicromap);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateOpticalFlowSessionNV(VkDevice device, const VkOpticalFlowSessionCreateInfoNV* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkOpticalFlowSessionNV* pSession) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateOpticalFlowSessionNV;require(pfn);return pfn(device, pCreateInfo, pAllocator, pSession);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateOpticalFlowSessionNV;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateOpticalFlowSessionNV");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pSession);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreatePipelineBinariesKHR(VkDevice device, const VkPipelineBinaryCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkPipelineBinaryHandlesInfoKHR* pBinaries) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreatePipelineBinariesKHR;require(pfn);return pfn(device, pCreateInfo, pAllocator, pBinaries);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreatePipelineBinariesKHR;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreatePipelineBinariesKHR");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pBinaries);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreatePipelineCache(VkDevice device, const VkPipelineCacheCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkPipelineCache* pPipelineCache) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreatePipelineCache;require(pfn);return pfn(device, pCreateInfo, pAllocator, pPipelineCache);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreatePipelineCache;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreatePipelineCache");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pPipelineCache);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkPipelineLayout* pPipelineLayout) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreatePipelineLayout;require(pfn);return pfn(device, pCreateInfo, pAllocator, pPipelineLayout);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreatePipelineLayout;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreatePipelineLayout");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pPipelineLayout);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreatePrivateDataSlot(VkDevice device, const VkPrivateDataSlotCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkPrivateDataSlot* pPrivateDataSlot) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreatePrivateDataSlot;require(pfn);return pfn(device, pCreateInfo, pAllocator, pPrivateDataSlot);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreatePrivateDataSlot;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreatePrivateDataSlot");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pPrivateDataSlot);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreatePrivateDataSlotEXT(VkDevice device, const VkPrivateDataSlotCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkPrivateDataSlot* pPrivateDataSlot) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreatePrivateDataSlotEXT;require(pfn);return pfn(device, pCreateInfo, pAllocator, pPrivateDataSlot);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreatePrivateDataSlotEXT;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreatePrivateDataSlotEXT");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pPrivateDataSlot);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateQueryPool(VkDevice device, const VkQueryPoolCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkQueryPool* pQueryPool) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateQueryPool;require(pfn);return pfn(device, pCreateInfo, pAllocator, pQueryPool);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateQueryPool;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateQueryPool");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pQueryPool);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateRayTracingPipelinesKHR(VkDevice device, VkDeferredOperationKHR deferredOperation, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkRayTracingPipelineCreateInfoKHR* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateRayTracingPipelinesKHR;require(pfn);return pfn(device, deferredOperation, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateRayTracingPipelinesKHR;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateRayTracingPipelinesKHR");
 require(fn);
 return fn(device, deferredOperation, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateRayTracingPipelinesNV(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkRayTracingPipelineCreateInfoNV* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateRayTracingPipelinesNV;require(pfn);return pfn(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateRayTracingPipelinesNV;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateRayTracingPipelinesNV");
 require(fn);
 return fn(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateRenderPass;require(pfn);return pfn(device, pCreateInfo, pAllocator, pRenderPass);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateRenderPass;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateRenderPass");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pRenderPass);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateRenderPass2;require(pfn);return pfn(device, pCreateInfo, pAllocator, pRenderPass);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateRenderPass2;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateRenderPass2");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pRenderPass);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateRenderPass2KHR(VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateRenderPass2KHR;require(pfn);return pfn(device, pCreateInfo, pAllocator, pRenderPass);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateRenderPass2KHR;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateRenderPass2KHR");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pRenderPass);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateSampler(VkDevice device, const VkSamplerCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSampler* pSampler) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateSampler;require(pfn);return pfn(device, pCreateInfo, pAllocator, pSampler);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateSampler;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateSampler");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pSampler);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateSamplerYcbcrConversion(VkDevice device, const VkSamplerYcbcrConversionCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSamplerYcbcrConversion* pYcbcrConversion) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateSamplerYcbcrConversion;require(pfn);return pfn(device, pCreateInfo, pAllocator, pYcbcrConversion);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateSamplerYcbcrConversion;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateSamplerYcbcrConversion");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pYcbcrConversion);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateSamplerYcbcrConversionKHR(VkDevice device, const VkSamplerYcbcrConversionCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSamplerYcbcrConversion* pYcbcrConversion) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateSamplerYcbcrConversionKHR;require(pfn);return pfn(device, pCreateInfo, pAllocator, pYcbcrConversion);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateSamplerYcbcrConversionKHR;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateSamplerYcbcrConversionKHR");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pYcbcrConversion);
}
#if defined(VK_USE_PLATFORM_SCREEN_QNX)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateScreenSurfaceQNX(VkInstance instance, const VkScreenSurfaceCreateInfoQNX* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkCreateScreenSurfaceQNX>(i->next(i->handle,"vkCreateScreenSurfaceQNX"));
 InstanceEntries entries(i);
 if(!allocatorSafe(pAllocator)) { i->qualified=false; i->decline=std::uint32_t(Decline::Allocator); entries.revoke(Decline::Allocator); }
 require(fn);
 return fn(instance, pCreateInfo, pAllocator, pSurface);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSemaphore* pSemaphore) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateSemaphore;require(pfn);return pfn(device, pCreateInfo, pAllocator, pSemaphore);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateSemaphore;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateSemaphore");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pSemaphore);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateShaderInstrumentationARM(VkDevice device, const VkShaderInstrumentationCreateInfoARM* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkShaderInstrumentationARM* pInstrumentation) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateShaderInstrumentationARM;require(pfn);return pfn(device, pCreateInfo, pAllocator, pInstrumentation);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateShaderInstrumentationARM;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateShaderInstrumentationARM");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pInstrumentation);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkShaderModule* pShaderModule) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateShaderModule;require(pfn);return pfn(device, pCreateInfo, pAllocator, pShaderModule);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateShaderModule;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateShaderModule");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pShaderModule);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateShadersEXT(VkDevice device, uint32_t createInfoCount, const VkShaderCreateInfoEXT* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkShaderEXT* pShaders) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateShadersEXT;require(pfn);return pfn(device, createInfoCount, pCreateInfos, pAllocator, pShaders);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateShadersEXT;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateShadersEXT");
 require(fn);
 return fn(device, createInfoCount, pCreateInfos, pAllocator, pShaders);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateSharedSwapchainsKHR(VkDevice device, uint32_t swapchainCount, const VkSwapchainCreateInfoKHR* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchains) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateSharedSwapchainsKHR;require(pfn);return pfn(device, swapchainCount, pCreateInfos, pAllocator, pSwapchains);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateSharedSwapchainsKHR;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateSharedSwapchainsKHR");
 require(fn);
 return fn(device, swapchainCount, pCreateInfos, pAllocator, pSwapchains);
}
#if defined(VK_USE_PLATFORM_GGP)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateStreamDescriptorSurfaceGGP(VkInstance instance, const VkStreamDescriptorSurfaceCreateInfoGGP* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkCreateStreamDescriptorSurfaceGGP>(i->next(i->handle,"vkCreateStreamDescriptorSurfaceGGP"));
 InstanceEntries entries(i);
 if(!allocatorSafe(pAllocator)) { i->qualified=false; i->decline=std::uint32_t(Decline::Allocator); entries.revoke(Decline::Allocator); }
 require(fn);
 return fn(instance, pCreateInfo, pAllocator, pSurface);
}
#endif
#if defined(VK_USE_PLATFORM_OHOS)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateSurfaceOHOS(VkInstance instance, const VkSurfaceCreateInfoOHOS* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkCreateSurfaceOHOS>(i->next(i->handle,"vkCreateSurfaceOHOS"));
 InstanceEntries entries(i);
 if(!allocatorSafe(pAllocator)) { i->qualified=false; i->decline=std::uint32_t(Decline::Allocator); entries.revoke(Decline::Allocator); }
 require(fn);
 return fn(instance, pCreateInfo, pAllocator, pSurface);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateSwapchainKHR;require(pfn);return pfn(device, pCreateInfo, pAllocator, pSwapchain);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateSwapchainKHR;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateSwapchainKHR");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pSwapchain);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateTensorARM(VkDevice device, const VkTensorCreateInfoARM* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkTensorARM* pTensor) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateTensorARM;require(pfn);return pfn(device, pCreateInfo, pAllocator, pTensor);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateTensorARM;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateTensorARM");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pTensor);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateTensorViewARM(VkDevice device, const VkTensorViewCreateInfoARM* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkTensorViewARM* pView) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateTensorViewARM;require(pfn);return pfn(device, pCreateInfo, pAllocator, pView);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateTensorViewARM;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateTensorViewARM");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pView);
}
#if defined(VK_USE_PLATFORM_UBM_SEC)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateUbmSurfaceSEC(VkInstance instance, const VkUbmSurfaceCreateInfoSEC* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkCreateUbmSurfaceSEC>(i->next(i->handle,"vkCreateUbmSurfaceSEC"));
 InstanceEntries entries(i);
 if(!allocatorSafe(pAllocator)) { i->qualified=false; i->decline=std::uint32_t(Decline::Allocator); entries.revoke(Decline::Allocator); }
 require(fn);
 return fn(instance, pCreateInfo, pAllocator, pSurface);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateValidationCacheEXT(VkDevice device, const VkValidationCacheCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkValidationCacheEXT* pValidationCache) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateValidationCacheEXT;require(pfn);return pfn(device, pCreateInfo, pAllocator, pValidationCache);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateValidationCacheEXT;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateValidationCacheEXT");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pValidationCache);
}
#if defined(VK_USE_PLATFORM_VI_NN)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateViSurfaceNN(VkInstance instance, const VkViSurfaceCreateInfoNN* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkCreateViSurfaceNN>(i->next(i->handle,"vkCreateViSurfaceNN"));
 InstanceEntries entries(i);
 if(!allocatorSafe(pAllocator)) { i->qualified=false; i->decline=std::uint32_t(Decline::Allocator); entries.revoke(Decline::Allocator); }
 require(fn);
 return fn(instance, pCreateInfo, pAllocator, pSurface);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateVideoSessionKHR(VkDevice device, const VkVideoSessionCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkVideoSessionKHR* pVideoSession) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateVideoSessionKHR;require(pfn);return pfn(device, pCreateInfo, pAllocator, pVideoSession);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateVideoSessionKHR;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateVideoSessionKHR");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pVideoSession);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateVideoSessionParametersKHR(VkDevice device, const VkVideoSessionParametersCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkVideoSessionParametersKHR* pVideoSessionParameters) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkCreateVideoSessionParametersKHR;require(pfn);return pfn(device, pCreateInfo, pAllocator, pVideoSessionParameters);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkCreateVideoSessionParametersKHR;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkCreateVideoSessionParametersKHR");
 require(fn);
 return fn(device, pCreateInfo, pAllocator, pVideoSessionParameters);
}
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateWaylandSurfaceKHR(VkInstance instance, const VkWaylandSurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(i->next(i->handle,"vkCreateWaylandSurfaceKHR"));
 InstanceEntries entries(i);
 if(!allocatorSafe(pAllocator)) { i->qualified=false; i->decline=std::uint32_t(Decline::Allocator); entries.revoke(Decline::Allocator); }
 require(fn);
 return fn(instance, pCreateInfo, pAllocator, pSurface);
}
#endif
#if defined(VK_USE_PLATFORM_WIN32_KHR)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(i->next(i->handle,"vkCreateWin32SurfaceKHR"));
 InstanceEntries entries(i);
 if(!allocatorSafe(pAllocator)) { i->qualified=false; i->decline=std::uint32_t(Decline::Allocator); entries.revoke(Decline::Allocator); }
 require(fn);
 return fn(instance, pCreateInfo, pAllocator, pSurface);
}
#endif
#if defined(VK_USE_PLATFORM_XCB_KHR)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateXcbSurfaceKHR(VkInstance instance, const VkXcbSurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkCreateXcbSurfaceKHR>(i->next(i->handle,"vkCreateXcbSurfaceKHR"));
 InstanceEntries entries(i);
 if(!allocatorSafe(pAllocator)) { i->qualified=false; i->decline=std::uint32_t(Decline::Allocator); entries.revoke(Decline::Allocator); }
 require(fn);
 return fn(instance, pCreateInfo, pAllocator, pSurface);
}
#endif
#if defined(VK_USE_PLATFORM_XLIB_KHR)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateXlibSurfaceKHR(VkInstance instance, const VkXlibSurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkCreateXlibSurfaceKHR>(i->next(i->handle,"vkCreateXlibSurfaceKHR"));
 InstanceEntries entries(i);
 if(!allocatorSafe(pAllocator)) { i->qualified=false; i->decline=std::uint32_t(Decline::Allocator); entries.revoke(Decline::Allocator); }
 require(fn);
 return fn(instance, pCreateInfo, pAllocator, pSurface);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkDebugMarkerSetObjectNameEXT(VkDevice device, const VkDebugMarkerObjectNameInfoEXT* pNameInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDebugMarkerSetObjectNameEXT;require(pfn);return pfn(device, pNameInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDebugMarkerSetObjectNameEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDebugMarkerSetObjectNameEXT");
 require(fn);
 return fn(device, pNameInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkDebugMarkerSetObjectTagEXT(VkDevice device, const VkDebugMarkerObjectTagInfoEXT* pTagInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDebugMarkerSetObjectTagEXT;require(pfn);return pfn(device, pTagInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDebugMarkerSetObjectTagEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDebugMarkerSetObjectTagEXT");
 require(fn);
 return fn(device, pTagInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDebugReportMessageEXT(VkInstance instance, VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objectType, uint64_t object, size_t location, int32_t messageCode, const char* pLayerPrefix, const char* pMessage) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkDebugReportMessageEXT>(i->next(i->handle,"vkDebugReportMessageEXT"));
 InstanceEntries entries(i);
 require(fn);
 fn(instance, flags, objectType, object, location, messageCode, pLayerPrefix, pMessage);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkDeferredOperationJoinKHR(VkDevice device, VkDeferredOperationKHR operation) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDeferredOperationJoinKHR;require(pfn);return pfn(device, operation);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDeferredOperationJoinKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDeferredOperationJoinKHR");
 require(fn);
 return fn(device, operation);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyAccelerationStructureKHR(VkDevice device, VkAccelerationStructureKHR accelerationStructure, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyAccelerationStructureKHR;require(pfn);pfn(device, accelerationStructure, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyAccelerationStructureKHR;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyAccelerationStructureKHR");
 require(fn);
 fn(device, accelerationStructure, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyAccelerationStructureNV(VkDevice device, VkAccelerationStructureNV accelerationStructure, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyAccelerationStructureNV;require(pfn);pfn(device, accelerationStructure, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyAccelerationStructureNV;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyAccelerationStructureNV");
 require(fn);
 fn(device, accelerationStructure, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyBuffer;require(pfn);pfn(device, buffer, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyBuffer;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyBuffer");
 require(fn);
 fn(device, buffer, pAllocator);
}
#if defined(VK_USE_PLATFORM_FUCHSIA)
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyBufferCollectionFUCHSIA(VkDevice device, VkBufferCollectionFUCHSIA collection, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyBufferCollectionFUCHSIA;require(pfn);pfn(device, collection, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyBufferCollectionFUCHSIA;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyBufferCollectionFUCHSIA");
 require(fn);
 fn(device, collection, pAllocator);
}
#endif
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyBufferView(VkDevice device, VkBufferView bufferView, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyBufferView;require(pfn);pfn(device, bufferView, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyBufferView;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyBufferView");
 require(fn);
 fn(device, bufferView, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyCuFunctionNVX(VkDevice device, VkCuFunctionNVX function, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyCuFunctionNVX;require(pfn);pfn(device, function, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyCuFunctionNVX;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyCuFunctionNVX");
 require(fn);
 fn(device, function, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyCuModuleNVX(VkDevice device, VkCuModuleNVX module, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyCuModuleNVX;require(pfn);pfn(device, module, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyCuModuleNVX;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyCuModuleNVX");
 require(fn);
 fn(device, module, pAllocator);
}
#if defined(VK_ENABLE_BETA_EXTENSIONS)
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyCudaFunctionNV(VkDevice device, VkCudaFunctionNV function, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyCudaFunctionNV;require(pfn);pfn(device, function, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyCudaFunctionNV;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyCudaFunctionNV");
 require(fn);
 fn(device, function, pAllocator);
}
#endif
#if defined(VK_ENABLE_BETA_EXTENSIONS)
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyCudaModuleNV(VkDevice device, VkCudaModuleNV module, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyCudaModuleNV;require(pfn);pfn(device, module, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyCudaModuleNV;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyCudaModuleNV");
 require(fn);
 fn(device, module, pAllocator);
}
#endif
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyDataGraphPipelineSessionARM(VkDevice device, VkDataGraphPipelineSessionARM session, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyDataGraphPipelineSessionARM;require(pfn);pfn(device, session, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyDataGraphPipelineSessionARM;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyDataGraphPipelineSessionARM");
 require(fn);
 fn(device, session, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyDebugReportCallbackEXT(VkInstance instance, VkDebugReportCallbackEXT callback, const VkAllocationCallbacks* pAllocator) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkDestroyDebugReportCallbackEXT>(i->next(i->handle,"vkDestroyDebugReportCallbackEXT"));
 InstanceEntries entries(i);
 if(!allocatorSafe(pAllocator)) { i->qualified=false; i->decline=std::uint32_t(Decline::Allocator); entries.revoke(Decline::Allocator); }
 require(fn);
 fn(instance, callback, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT messenger, const VkAllocationCallbacks* pAllocator) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(i->next(i->handle,"vkDestroyDebugUtilsMessengerEXT"));
 InstanceEntries entries(i);
 if(!allocatorSafe(pAllocator)) { i->qualified=false; i->decline=std::uint32_t(Decline::Allocator); entries.revoke(Decline::Allocator); }
 require(fn);
 fn(instance, messenger, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyDeferredOperationKHR(VkDevice device, VkDeferredOperationKHR operation, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyDeferredOperationKHR;require(pfn);pfn(device, operation, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyDeferredOperationKHR;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyDeferredOperationKHR");
 require(fn);
 fn(device, operation, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyDescriptorPool(VkDevice device,VkDescriptorPool pool,const VkAllocationCallbacks*allocator){
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyDescriptorPool;require(pfn);pfn(device, pool, allocator);return;}
 auto*d=deviceFor(device);auto fn=d->dispatch.vkDestroyDescriptorPool;require(fn);const int incoming=errno;
 if(!allocatorSafe(allocator))revoke(d,Decline::Allocator);
 PoolOperation87 poolOperation87(d,pool);auto entry=enter(d,Worker::AccessKind::Resource,"vkDestroyDescriptorPool");destroyedDescriptorPoolCertificate83(d,pool);retirePoolObservation82(d,pool,false);retireDescriptorPool81(d,pool);errno=incoming;fn(device,pool,allocator);
}

VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyDescriptorSetLayout(VkDevice device,VkDescriptorSetLayout layout,const VkAllocationCallbacks*allocator){
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyDescriptorSetLayout;require(pfn);pfn(device, layout, allocator);return;}
 auto*d=deviceFor(device);auto fn=d->dispatch.vkDestroyDescriptorSetLayout;require(fn);const int incoming=errno;
 if(!allocatorSafe(allocator))revoke(d,Decline::Allocator);
 auto entry=enter(d,Worker::AccessKind::Resource,"vkDestroyDescriptorSetLayout");retireDescriptorLayout81(d,layout);errno=incoming;fn(device,layout,allocator);
}

VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyDescriptorUpdateTemplate(VkDevice device, VkDescriptorUpdateTemplate descriptorUpdateTemplate, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyDescriptorUpdateTemplate;require(pfn);pfn(device, descriptorUpdateTemplate, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyDescriptorUpdateTemplate;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyDescriptorUpdateTemplate");
 require(fn);
 fn(device, descriptorUpdateTemplate, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyDescriptorUpdateTemplateKHR(VkDevice device, VkDescriptorUpdateTemplate descriptorUpdateTemplate, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyDescriptorUpdateTemplateKHR;require(pfn);pfn(device, descriptorUpdateTemplate, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyDescriptorUpdateTemplateKHR;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyDescriptorUpdateTemplateKHR");
 require(fn);
 fn(device, descriptorUpdateTemplate, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyEvent(VkDevice device, VkEvent event, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyEvent;require(pfn);pfn(device, event, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyEvent;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyEvent");
 require(fn);
 fn(device, event, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyExternalComputeQueueNV(VkDevice device, VkExternalComputeQueueNV externalQueue, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyExternalComputeQueueNV;require(pfn);pfn(device, externalQueue, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyExternalComputeQueueNV;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyExternalComputeQueueNV");
 require(fn);
 fn(device, externalQueue, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyFence;require(pfn);pfn(device, fence, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyFence;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyFence");
 require(fn);
 fn(device, fence, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyFramebuffer;require(pfn);pfn(device, framebuffer, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyFramebuffer;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyFramebuffer");
 require(fn);
 fn(device, framebuffer, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyGpaSessionAMD(VkDevice                                          device, VkGpaSessionAMD gpaSession, const VkAllocationCallbacks*      pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyGpaSessionAMD;require(pfn);pfn(device, gpaSession, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyGpaSessionAMD;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyGpaSessionAMD");
 require(fn);
 fn(device, gpaSession, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyImage;require(pfn);pfn(device, image, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyImage;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyImage");
 require(fn);
 fn(device, image, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyImageView(VkDevice device, VkImageView imageView, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyImageView;require(pfn);pfn(device, imageView, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyImageView;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyImageView");
 require(fn);
 fn(device, imageView, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyIndirectCommandsLayoutEXT(VkDevice device, VkIndirectCommandsLayoutEXT indirectCommandsLayout, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyIndirectCommandsLayoutEXT;require(pfn);pfn(device, indirectCommandsLayout, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyIndirectCommandsLayoutEXT;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyIndirectCommandsLayoutEXT");
 require(fn);
 fn(device, indirectCommandsLayout, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyIndirectCommandsLayoutNV(VkDevice device, VkIndirectCommandsLayoutNV indirectCommandsLayout, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyIndirectCommandsLayoutNV;require(pfn);pfn(device, indirectCommandsLayout, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyIndirectCommandsLayoutNV;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyIndirectCommandsLayoutNV");
 require(fn);
 fn(device, indirectCommandsLayout, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyIndirectExecutionSetEXT(VkDevice device, VkIndirectExecutionSetEXT indirectExecutionSet, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyIndirectExecutionSetEXT;require(pfn);pfn(device, indirectExecutionSet, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyIndirectExecutionSetEXT;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyIndirectExecutionSetEXT");
 require(fn);
 fn(device, indirectExecutionSet, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyMicromapEXT(VkDevice                                        device, VkMicromapEXT micromap, const VkAllocationCallbacks*    pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyMicromapEXT;require(pfn);pfn(device, micromap, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyMicromapEXT;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyMicromapEXT");
 require(fn);
 fn(device, micromap, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyOpticalFlowSessionNV(VkDevice device, VkOpticalFlowSessionNV session, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyOpticalFlowSessionNV;require(pfn);pfn(device, session, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyOpticalFlowSessionNV;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyOpticalFlowSessionNV");
 require(fn);
 fn(device, session, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyPipeline;require(pfn);pfn(device, pipeline, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyPipeline;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyPipeline");
 require(fn);
 fn(device, pipeline, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyPipelineBinaryKHR(VkDevice device, VkPipelineBinaryKHR pipelineBinary, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyPipelineBinaryKHR;require(pfn);pfn(device, pipelineBinary, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyPipelineBinaryKHR;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyPipelineBinaryKHR");
 require(fn);
 fn(device, pipelineBinary, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyPipelineCache(VkDevice device, VkPipelineCache pipelineCache, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyPipelineCache;require(pfn);pfn(device, pipelineCache, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyPipelineCache;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyPipelineCache");
 require(fn);
 fn(device, pipelineCache, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyPipelineLayout(VkDevice device, VkPipelineLayout pipelineLayout, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyPipelineLayout;require(pfn);pfn(device, pipelineLayout, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyPipelineLayout;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyPipelineLayout");
 require(fn);
 fn(device, pipelineLayout, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyPrivateDataSlot(VkDevice device, VkPrivateDataSlot privateDataSlot, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyPrivateDataSlot;require(pfn);pfn(device, privateDataSlot, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyPrivateDataSlot;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyPrivateDataSlot");
 require(fn);
 fn(device, privateDataSlot, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyPrivateDataSlotEXT(VkDevice device, VkPrivateDataSlot privateDataSlot, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyPrivateDataSlotEXT;require(pfn);pfn(device, privateDataSlot, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyPrivateDataSlotEXT;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyPrivateDataSlotEXT");
 require(fn);
 fn(device, privateDataSlot, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyQueryPool(VkDevice device, VkQueryPool queryPool, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyQueryPool;require(pfn);pfn(device, queryPool, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyQueryPool;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyQueryPool");
 require(fn);
 fn(device, queryPool, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyRenderPass(VkDevice device, VkRenderPass renderPass, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyRenderPass;require(pfn);pfn(device, renderPass, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyRenderPass;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyRenderPass");
 require(fn);
 fn(device, renderPass, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroySampler(VkDevice device, VkSampler sampler, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroySampler;require(pfn);pfn(device, sampler, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroySampler;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroySampler");
 require(fn);
 fn(device, sampler, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroySamplerYcbcrConversion(VkDevice device, VkSamplerYcbcrConversion ycbcrConversion, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroySamplerYcbcrConversion;require(pfn);pfn(device, ycbcrConversion, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroySamplerYcbcrConversion;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroySamplerYcbcrConversion");
 require(fn);
 fn(device, ycbcrConversion, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroySamplerYcbcrConversionKHR(VkDevice device, VkSamplerYcbcrConversion ycbcrConversion, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroySamplerYcbcrConversionKHR;require(pfn);pfn(device, ycbcrConversion, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroySamplerYcbcrConversionKHR;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroySamplerYcbcrConversionKHR");
 require(fn);
 fn(device, ycbcrConversion, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroySemaphore(VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroySemaphore;require(pfn);pfn(device, semaphore, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroySemaphore;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroySemaphore");
 require(fn);
 fn(device, semaphore, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyShaderEXT(VkDevice device, VkShaderEXT shader, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyShaderEXT;require(pfn);pfn(device, shader, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyShaderEXT;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyShaderEXT");
 require(fn);
 fn(device, shader, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyShaderInstrumentationARM(VkDevice device, VkShaderInstrumentationARM instrumentation, const VkAllocationCallbacks*    pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyShaderInstrumentationARM;require(pfn);pfn(device, instrumentation, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyShaderInstrumentationARM;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyShaderInstrumentationARM");
 require(fn);
 fn(device, instrumentation, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyShaderModule(VkDevice device, VkShaderModule shaderModule, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyShaderModule;require(pfn);pfn(device, shaderModule, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyShaderModule;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyShaderModule");
 require(fn);
 fn(device, shaderModule, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* pAllocator) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkDestroySurfaceKHR>(i->next(i->handle,"vkDestroySurfaceKHR"));
 InstanceEntries entries(i);
 if(!allocatorSafe(pAllocator)) { i->qualified=false; i->decline=std::uint32_t(Decline::Allocator); entries.revoke(Decline::Allocator); }
 require(fn);
 fn(instance, surface, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroySwapchainKHR;require(pfn);pfn(device, swapchain, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroySwapchainKHR;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroySwapchainKHR");
 require(fn);
 fn(device, swapchain, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyTensorARM(VkDevice device, VkTensorARM tensor, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyTensorARM;require(pfn);pfn(device, tensor, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyTensorARM;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyTensorARM");
 require(fn);
 fn(device, tensor, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyTensorViewARM(VkDevice device, VkTensorViewARM tensorView, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyTensorViewARM;require(pfn);pfn(device, tensorView, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyTensorViewARM;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyTensorViewARM");
 require(fn);
 fn(device, tensorView, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyValidationCacheEXT(VkDevice device, VkValidationCacheEXT validationCache, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyValidationCacheEXT;require(pfn);pfn(device, validationCache, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyValidationCacheEXT;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyValidationCacheEXT");
 require(fn);
 fn(device, validationCache, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyVideoSessionKHR(VkDevice device, VkVideoSessionKHR videoSession, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyVideoSessionKHR;require(pfn);pfn(device, videoSession, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyVideoSessionKHR;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyVideoSessionKHR");
 require(fn);
 fn(device, videoSession, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyVideoSessionParametersKHR(VkDevice device, VkVideoSessionParametersKHR videoSessionParameters, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDestroyVideoSessionParametersKHR;require(pfn);pfn(device, videoSessionParameters, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDestroyVideoSessionParametersKHR;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDestroyVideoSessionParametersKHR");
 require(fn);
 fn(device, videoSessionParameters, pAllocator);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkDeviceWaitIdle(VkDevice device) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDeviceWaitIdle;require(pfn);return pfn(device);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDeviceWaitIdle;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDeviceWaitIdle");
 require(fn);
 return fn(device);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkDisplayPowerControlEXT(VkDevice device, VkDisplayKHR display, const VkDisplayPowerInfoEXT* pDisplayPowerInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkDisplayPowerControlEXT;require(pfn);return pfn(device, display, pDisplayPowerInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkDisplayPowerControlEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkDisplayPowerControlEXT");
 require(fn);
 return fn(device, display, pDisplayPowerInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(i->next(i->handle,"vkEnumerateDeviceExtensionProperties"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pLayerName, pPropertyCount, pProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkEnumeratePhysicalDeviceGroups(VkInstance instance, uint32_t* pPhysicalDeviceGroupCount, VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkEnumeratePhysicalDeviceGroups>(i->next(i->handle,"vkEnumeratePhysicalDeviceGroups"));
 InstanceEntries entries(i);
 require(fn);
 return fn(instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkEnumeratePhysicalDeviceGroupsKHR(VkInstance instance, uint32_t* pPhysicalDeviceGroupCount, VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkEnumeratePhysicalDeviceGroupsKHR>(i->next(i->handle,"vkEnumeratePhysicalDeviceGroupsKHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkEnumeratePhysicalDeviceQueueFamilyPerformanceCountersByRegionARM(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, uint32_t* pCounterCount, VkPerformanceCounterARM* pCounters, VkPerformanceCounterDescriptionARM* pCounterDescriptions) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkEnumeratePhysicalDeviceQueueFamilyPerformanceCountersByRegionARM>(i->next(i->handle,"vkEnumeratePhysicalDeviceQueueFamilyPerformanceCountersByRegionARM"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, queueFamilyIndex, pCounterCount, pCounters, pCounterDescriptions);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, uint32_t* pCounterCount, VkPerformanceCounterKHR* pCounters, VkPerformanceCounterDescriptionKHR* pCounterDescriptions) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR>(i->next(i->handle,"vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, queueFamilyIndex, pCounterCount, pCounters, pCounterDescriptions);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkEnumeratePhysicalDeviceShaderInstrumentationMetricsARM(VkPhysicalDevice physicalDevice, uint32_t* pDescriptionCount, VkShaderInstrumentationMetricDescriptionARM* pDescriptions) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkEnumeratePhysicalDeviceShaderInstrumentationMetricsARM>(i->next(i->handle,"vkEnumeratePhysicalDeviceShaderInstrumentationMetricsARM"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pDescriptionCount, pDescriptions);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkEnumeratePhysicalDevices(VkInstance instance, uint32_t* pPhysicalDeviceCount, VkPhysicalDevice* pPhysicalDevices) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(i->next(i->handle,"vkEnumeratePhysicalDevices"));
 InstanceEntries entries(i);
 require(fn);
 return fn(instance, pPhysicalDeviceCount, pPhysicalDevices);
}
#if defined(VK_USE_PLATFORM_METAL_EXT)
VKAPI_ATTR void VKAPI_CALL wrap_vkExportMetalObjectsEXT(VkDevice device, VkExportMetalObjectsInfoEXT* pMetalObjectsInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkExportMetalObjectsEXT;require(pfn);pfn(device, pMetalObjectsInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkExportMetalObjectsEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkExportMetalObjectsEXT");
 require(fn);
 fn(device, pMetalObjectsInfo);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkFlushMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange* pMemoryRanges) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkFlushMappedMemoryRanges;require(pfn);return pfn(device, memoryRangeCount, pMemoryRanges);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkFlushMappedMemoryRanges;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkFlushMappedMemoryRanges");
 require(fn);
 return fn(device, memoryRangeCount, pMemoryRanges);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkFreeDescriptorSets(VkDevice device,VkDescriptorPool pool,uint32_t count,const VkDescriptorSet*sets){
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkFreeDescriptorSets;require(pfn);return pfn(device, pool, count, sets);}
 auto*d=deviceFor(device);auto fn=d->dispatch.vkFreeDescriptorSets;require(fn);const int incoming=errno;
 PoolOperation87 poolOperation87(d,pool);auto entry=enter(d,Worker::AccessKind::Resource,"vkFreeDescriptorSets");
 // This qualified DUD pool has no FREE_DESCRIPTOR_SET flag. Any separate free
 // path retires its qualification, even if the original reports an error.
 retireDescriptorPool81(d,pool);errno=incoming;return fn(device,pool,count,sets);
}

VKAPI_ATTR void VKAPI_CALL wrap_vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkFreeMemory;require(pfn);pfn(device, memory, pAllocator);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkFreeMemory;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkFreeMemory");
 require(fn);
 fn(device, memory, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetAccelerationStructureBuildSizesKHR(VkDevice                                            device, VkAccelerationStructureBuildTypeKHR                 buildType, const VkAccelerationStructureBuildGeometryInfoKHR*  pBuildInfo, const uint32_t*  pMaxPrimitiveCounts, VkAccelerationStructureBuildSizesInfoKHR*           pSizeInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetAccelerationStructureBuildSizesKHR;require(pfn);pfn(device, buildType, pBuildInfo, pMaxPrimitiveCounts, pSizeInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetAccelerationStructureBuildSizesKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetAccelerationStructureBuildSizesKHR");
 require(fn);
 fn(device, buildType, pBuildInfo, pMaxPrimitiveCounts, pSizeInfo);
}
VKAPI_ATTR VkDeviceAddress VKAPI_CALL wrap_vkGetAccelerationStructureDeviceAddressKHR(VkDevice device, const VkAccelerationStructureDeviceAddressInfoKHR* pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetAccelerationStructureDeviceAddressKHR;require(pfn);return pfn(device, pInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetAccelerationStructureDeviceAddressKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetAccelerationStructureDeviceAddressKHR");
 require(fn);
 return fn(device, pInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetAccelerationStructureHandleNV(VkDevice device, VkAccelerationStructureNV accelerationStructure, size_t dataSize, void* pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetAccelerationStructureHandleNV;require(pfn);return pfn(device, accelerationStructure, dataSize, pData);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetAccelerationStructureHandleNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetAccelerationStructureHandleNV");
 require(fn);
 return fn(device, accelerationStructure, dataSize, pData);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetAccelerationStructureMemoryRequirementsNV(VkDevice device, const VkAccelerationStructureMemoryRequirementsInfoNV* pInfo, VkMemoryRequirements2* pMemoryRequirements) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetAccelerationStructureMemoryRequirementsNV;require(pfn);pfn(device, pInfo, pMemoryRequirements);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetAccelerationStructureMemoryRequirementsNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetAccelerationStructureMemoryRequirementsNV");
 require(fn);
 fn(device, pInfo, pMemoryRequirements);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetAccelerationStructureOpaqueCaptureDescriptorDataEXT(VkDevice device, const VkAccelerationStructureCaptureDescriptorDataInfoEXT* pInfo, void* pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetAccelerationStructureOpaqueCaptureDescriptorDataEXT;require(pfn);return pfn(device, pInfo, pData);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetAccelerationStructureOpaqueCaptureDescriptorDataEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetAccelerationStructureOpaqueCaptureDescriptorDataEXT");
 require(fn);
 return fn(device, pInfo, pData);
}
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetAndroidHardwareBufferPropertiesANDROID(VkDevice device, const struct AHardwareBuffer* buffer, VkAndroidHardwareBufferPropertiesANDROID* pProperties) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetAndroidHardwareBufferPropertiesANDROID;require(pfn);return pfn(device, buffer, pProperties);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetAndroidHardwareBufferPropertiesANDROID;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetAndroidHardwareBufferPropertiesANDROID");
 require(fn);
 return fn(device, buffer, pProperties);
}
#endif
#if defined(VK_USE_PLATFORM_FUCHSIA)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetBufferCollectionPropertiesFUCHSIA(VkDevice device, VkBufferCollectionFUCHSIA collection, VkBufferCollectionPropertiesFUCHSIA* pProperties) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetBufferCollectionPropertiesFUCHSIA;require(pfn);return pfn(device, collection, pProperties);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetBufferCollectionPropertiesFUCHSIA;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetBufferCollectionPropertiesFUCHSIA");
 require(fn);
 return fn(device, collection, pProperties);
}
#endif
VKAPI_ATTR VkDeviceAddress VKAPI_CALL wrap_vkGetBufferDeviceAddress(VkDevice device, const VkBufferDeviceAddressInfo* pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetBufferDeviceAddress;require(pfn);return pfn(device, pInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetBufferDeviceAddress;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetBufferDeviceAddress");
 require(fn);
 return fn(device, pInfo);
}
VKAPI_ATTR VkDeviceAddress VKAPI_CALL wrap_vkGetBufferDeviceAddressEXT(VkDevice device, const VkBufferDeviceAddressInfo* pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetBufferDeviceAddressEXT;require(pfn);return pfn(device, pInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetBufferDeviceAddressEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetBufferDeviceAddressEXT");
 require(fn);
 return fn(device, pInfo);
}
VKAPI_ATTR VkDeviceAddress VKAPI_CALL wrap_vkGetBufferDeviceAddressKHR(VkDevice device, const VkBufferDeviceAddressInfo* pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetBufferDeviceAddressKHR;require(pfn);return pfn(device, pInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetBufferDeviceAddressKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetBufferDeviceAddressKHR");
 require(fn);
 return fn(device, pInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer, VkMemoryRequirements* pMemoryRequirements) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetBufferMemoryRequirements;require(pfn);pfn(device, buffer, pMemoryRequirements);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetBufferMemoryRequirements;
 require(fn);
 fn(device, buffer, pMemoryRequirements);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetBufferMemoryRequirements2(VkDevice device, const VkBufferMemoryRequirementsInfo2* pInfo, VkMemoryRequirements2* pMemoryRequirements) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetBufferMemoryRequirements2;require(pfn);pfn(device, pInfo, pMemoryRequirements);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetBufferMemoryRequirements2;
 require(fn);
 fn(device, pInfo, pMemoryRequirements);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetBufferMemoryRequirements2KHR(VkDevice device, const VkBufferMemoryRequirementsInfo2* pInfo, VkMemoryRequirements2* pMemoryRequirements) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetBufferMemoryRequirements2KHR;require(pfn);pfn(device, pInfo, pMemoryRequirements);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetBufferMemoryRequirements2KHR;
 require(fn);
 fn(device, pInfo, pMemoryRequirements);
}
VKAPI_ATTR uint64_t VKAPI_CALL wrap_vkGetBufferOpaqueCaptureAddress(VkDevice device, const VkBufferDeviceAddressInfo* pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetBufferOpaqueCaptureAddress;require(pfn);return pfn(device, pInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetBufferOpaqueCaptureAddress;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetBufferOpaqueCaptureAddress");
 require(fn);
 return fn(device, pInfo);
}
VKAPI_ATTR uint64_t VKAPI_CALL wrap_vkGetBufferOpaqueCaptureAddressKHR(VkDevice device, const VkBufferDeviceAddressInfo* pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetBufferOpaqueCaptureAddressKHR;require(pfn);return pfn(device, pInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetBufferOpaqueCaptureAddressKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetBufferOpaqueCaptureAddressKHR");
 require(fn);
 return fn(device, pInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetBufferOpaqueCaptureDescriptorDataEXT(VkDevice device, const VkBufferCaptureDescriptorDataInfoEXT* pInfo, void* pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetBufferOpaqueCaptureDescriptorDataEXT;require(pfn);return pfn(device, pInfo, pData);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetBufferOpaqueCaptureDescriptorDataEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetBufferOpaqueCaptureDescriptorDataEXT");
 require(fn);
 return fn(device, pInfo, pData);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetCalibratedTimestampsEXT(VkDevice device, uint32_t timestampCount, const VkCalibratedTimestampInfoKHR* pTimestampInfos, uint64_t* pTimestamps, uint64_t* pMaxDeviation) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetCalibratedTimestampsEXT;require(pfn);return pfn(device, timestampCount, pTimestampInfos, pTimestamps, pMaxDeviation);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetCalibratedTimestampsEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetCalibratedTimestampsEXT");
 require(fn);
 return fn(device, timestampCount, pTimestampInfos, pTimestamps, pMaxDeviation);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetCalibratedTimestampsKHR(VkDevice device, uint32_t timestampCount, const VkCalibratedTimestampInfoKHR* pTimestampInfos, uint64_t* pTimestamps, uint64_t* pMaxDeviation) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetCalibratedTimestampsKHR;require(pfn);return pfn(device, timestampCount, pTimestampInfos, pTimestamps, pMaxDeviation);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetCalibratedTimestampsKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetCalibratedTimestampsKHR");
 require(fn);
 return fn(device, timestampCount, pTimestampInfos, pTimestamps, pMaxDeviation);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetClusterAccelerationStructureBuildSizesNV(VkDevice device, const VkClusterAccelerationStructureInputInfoNV* pInfo, VkAccelerationStructureBuildSizesInfoKHR* pSizeInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetClusterAccelerationStructureBuildSizesNV;require(pfn);pfn(device, pInfo, pSizeInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetClusterAccelerationStructureBuildSizesNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetClusterAccelerationStructureBuildSizesNV");
 require(fn);
 fn(device, pInfo, pSizeInfo);
}
#if defined(VK_ENABLE_BETA_EXTENSIONS)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetCudaModuleCacheNV(VkDevice device, VkCudaModuleNV module, size_t* pCacheSize, void* pCacheData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetCudaModuleCacheNV;require(pfn);return pfn(device, module, pCacheSize, pCacheData);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetCudaModuleCacheNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetCudaModuleCacheNV");
 require(fn);
 return fn(device, module, pCacheSize, pCacheData);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetDataGraphPipelineAvailablePropertiesARM(VkDevice device, const VkDataGraphPipelineInfoARM* pPipelineInfo, uint32_t* pPropertiesCount, VkDataGraphPipelinePropertyARM* pProperties) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDataGraphPipelineAvailablePropertiesARM;require(pfn);return pfn(device, pPipelineInfo, pPropertiesCount, pProperties);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDataGraphPipelineAvailablePropertiesARM;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDataGraphPipelineAvailablePropertiesARM");
 require(fn);
 return fn(device, pPipelineInfo, pPropertiesCount, pProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetDataGraphPipelinePropertiesARM(VkDevice                          device, const VkDataGraphPipelineInfoARM* pPipelineInfo, uint32_t                          propertiesCount, VkDataGraphPipelinePropertyQueryResultARM* pProperties) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDataGraphPipelinePropertiesARM;require(pfn);return pfn(device, pPipelineInfo, propertiesCount, pProperties);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDataGraphPipelinePropertiesARM;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDataGraphPipelinePropertiesARM");
 require(fn);
 return fn(device, pPipelineInfo, propertiesCount, pProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetDataGraphPipelineSessionBindPointRequirementsARM(VkDevice device, const VkDataGraphPipelineSessionBindPointRequirementsInfoARM* pInfo, uint32_t* pBindPointRequirementCount, VkDataGraphPipelineSessionBindPointRequirementARM* pBindPointRequirements) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDataGraphPipelineSessionBindPointRequirementsARM;require(pfn);return pfn(device, pInfo, pBindPointRequirementCount, pBindPointRequirements);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDataGraphPipelineSessionBindPointRequirementsARM;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDataGraphPipelineSessionBindPointRequirementsARM");
 require(fn);
 return fn(device, pInfo, pBindPointRequirementCount, pBindPointRequirements);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetDataGraphPipelineSessionMemoryRequirementsARM(VkDevice device, const VkDataGraphPipelineSessionMemoryRequirementsInfoARM* pInfo, VkMemoryRequirements2* pMemoryRequirements) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDataGraphPipelineSessionMemoryRequirementsARM;require(pfn);pfn(device, pInfo, pMemoryRequirements);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDataGraphPipelineSessionMemoryRequirementsARM;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDataGraphPipelineSessionMemoryRequirementsARM");
 require(fn);
 fn(device, pInfo, pMemoryRequirements);
}
VKAPI_ATTR uint32_t VKAPI_CALL wrap_vkGetDeferredOperationMaxConcurrencyKHR(VkDevice device, VkDeferredOperationKHR operation) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeferredOperationMaxConcurrencyKHR;require(pfn);return pfn(device, operation);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeferredOperationMaxConcurrencyKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeferredOperationMaxConcurrencyKHR");
 require(fn);
 return fn(device, operation);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetDeferredOperationResultKHR(VkDevice device, VkDeferredOperationKHR operation) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeferredOperationResultKHR;require(pfn);return pfn(device, operation);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeferredOperationResultKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeferredOperationResultKHR");
 require(fn);
 return fn(device, operation);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetDescriptorEXT(VkDevice device, const VkDescriptorGetInfoEXT* pDescriptorInfo, size_t dataSize, void* pDescriptor) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDescriptorEXT;require(pfn);pfn(device, pDescriptorInfo, dataSize, pDescriptor);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDescriptorEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDescriptorEXT");
 require(fn);
 fn(device, pDescriptorInfo, dataSize, pDescriptor);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetDescriptorSetHostMappingVALVE(VkDevice device, VkDescriptorSet descriptorSet, void** ppData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDescriptorSetHostMappingVALVE;require(pfn);pfn(device, descriptorSet, ppData);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDescriptorSetHostMappingVALVE;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDescriptorSetHostMappingVALVE");
 require(fn);
 fn(device, descriptorSet, ppData);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetDescriptorSetLayoutBindingOffsetEXT(VkDevice device, VkDescriptorSetLayout layout, uint32_t binding, VkDeviceSize* pOffset) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDescriptorSetLayoutBindingOffsetEXT;require(pfn);pfn(device, layout, binding, pOffset);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDescriptorSetLayoutBindingOffsetEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDescriptorSetLayoutBindingOffsetEXT");
 require(fn);
 fn(device, layout, binding, pOffset);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetDescriptorSetLayoutHostMappingInfoVALVE(VkDevice device, const VkDescriptorSetBindingReferenceVALVE* pBindingReference, VkDescriptorSetLayoutHostMappingInfoVALVE* pHostMapping) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDescriptorSetLayoutHostMappingInfoVALVE;require(pfn);pfn(device, pBindingReference, pHostMapping);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDescriptorSetLayoutHostMappingInfoVALVE;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDescriptorSetLayoutHostMappingInfoVALVE");
 require(fn);
 fn(device, pBindingReference, pHostMapping);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetDescriptorSetLayoutSizeEXT(VkDevice device, VkDescriptorSetLayout layout, VkDeviceSize* pLayoutSizeInBytes) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDescriptorSetLayoutSizeEXT;require(pfn);pfn(device, layout, pLayoutSizeInBytes);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDescriptorSetLayoutSizeEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDescriptorSetLayoutSizeEXT");
 require(fn);
 fn(device, layout, pLayoutSizeInBytes);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetDescriptorSetLayoutSupport(VkDevice device, const VkDescriptorSetLayoutCreateInfo* pCreateInfo, VkDescriptorSetLayoutSupport* pSupport) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDescriptorSetLayoutSupport;require(pfn);pfn(device, pCreateInfo, pSupport);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDescriptorSetLayoutSupport;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDescriptorSetLayoutSupport");
 require(fn);
 fn(device, pCreateInfo, pSupport);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetDescriptorSetLayoutSupportKHR(VkDevice device, const VkDescriptorSetLayoutCreateInfo* pCreateInfo, VkDescriptorSetLayoutSupport* pSupport) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDescriptorSetLayoutSupportKHR;require(pfn);pfn(device, pCreateInfo, pSupport);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDescriptorSetLayoutSupportKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDescriptorSetLayoutSupportKHR");
 require(fn);
 fn(device, pCreateInfo, pSupport);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetDeviceAccelerationStructureCompatibilityKHR(VkDevice device, const VkAccelerationStructureVersionInfoKHR* pVersionInfo, VkAccelerationStructureCompatibilityKHR* pCompatibility) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceAccelerationStructureCompatibilityKHR;require(pfn);pfn(device, pVersionInfo, pCompatibility);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceAccelerationStructureCompatibilityKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceAccelerationStructureCompatibilityKHR");
 require(fn);
 fn(device, pVersionInfo, pCompatibility);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetDeviceBufferMemoryRequirements(VkDevice device, const VkDeviceBufferMemoryRequirements* pInfo, VkMemoryRequirements2* pMemoryRequirements) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceBufferMemoryRequirements;require(pfn);pfn(device, pInfo, pMemoryRequirements);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceBufferMemoryRequirements;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceBufferMemoryRequirements");
 require(fn);
 fn(device, pInfo, pMemoryRequirements);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetDeviceBufferMemoryRequirementsKHR(VkDevice device, const VkDeviceBufferMemoryRequirements* pInfo, VkMemoryRequirements2* pMemoryRequirements) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceBufferMemoryRequirementsKHR;require(pfn);pfn(device, pInfo, pMemoryRequirements);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceBufferMemoryRequirementsKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceBufferMemoryRequirementsKHR");
 require(fn);
 fn(device, pInfo, pMemoryRequirements);
}
VKAPI_ATTR uint64_t VKAPI_CALL wrap_vkGetDeviceCombinedImageSamplerIndexNVX(VkDevice device, uint64_t imageViewIndex, uint64_t samplerIndex) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceCombinedImageSamplerIndexNVX;require(pfn);return pfn(device, imageViewIndex, samplerIndex);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceCombinedImageSamplerIndexNVX;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceCombinedImageSamplerIndexNVX");
 require(fn);
 return fn(device, imageViewIndex, samplerIndex);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetDeviceFaultDebugInfoKHR(VkDevice device, VkDeviceFaultDebugInfoKHR* pDebugInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceFaultDebugInfoKHR;require(pfn);return pfn(device, pDebugInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceFaultDebugInfoKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceFaultDebugInfoKHR");
 require(fn);
 return fn(device, pDebugInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetDeviceFaultInfoEXT(VkDevice device, VkDeviceFaultCountsEXT* pFaultCounts, VkDeviceFaultInfoEXT* pFaultInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceFaultInfoEXT;require(pfn);return pfn(device, pFaultCounts, pFaultInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceFaultInfoEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceFaultInfoEXT");
 require(fn);
 return fn(device, pFaultCounts, pFaultInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetDeviceFaultReportsKHR(VkDevice device, uint64_t timeout, uint32_t* pFaultCounts, VkDeviceFaultInfoKHR* pFaultInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceFaultReportsKHR;require(pfn);return pfn(device, timeout, pFaultCounts, pFaultInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceFaultReportsKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceFaultReportsKHR");
 require(fn);
 return fn(device, timeout, pFaultCounts, pFaultInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetDeviceGroupPeerMemoryFeatures(VkDevice device, uint32_t heapIndex, uint32_t localDeviceIndex, uint32_t remoteDeviceIndex, VkPeerMemoryFeatureFlags* pPeerMemoryFeatures) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceGroupPeerMemoryFeatures;require(pfn);pfn(device, heapIndex, localDeviceIndex, remoteDeviceIndex, pPeerMemoryFeatures);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceGroupPeerMemoryFeatures;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceGroupPeerMemoryFeatures");
 require(fn);
 fn(device, heapIndex, localDeviceIndex, remoteDeviceIndex, pPeerMemoryFeatures);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetDeviceGroupPeerMemoryFeaturesKHR(VkDevice device, uint32_t heapIndex, uint32_t localDeviceIndex, uint32_t remoteDeviceIndex, VkPeerMemoryFeatureFlags* pPeerMemoryFeatures) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceGroupPeerMemoryFeaturesKHR;require(pfn);pfn(device, heapIndex, localDeviceIndex, remoteDeviceIndex, pPeerMemoryFeatures);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceGroupPeerMemoryFeaturesKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceGroupPeerMemoryFeaturesKHR");
 require(fn);
 fn(device, heapIndex, localDeviceIndex, remoteDeviceIndex, pPeerMemoryFeatures);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetDeviceGroupPresentCapabilitiesKHR(VkDevice device, VkDeviceGroupPresentCapabilitiesKHR* pDeviceGroupPresentCapabilities) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceGroupPresentCapabilitiesKHR;require(pfn);return pfn(device, pDeviceGroupPresentCapabilities);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceGroupPresentCapabilitiesKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceGroupPresentCapabilitiesKHR");
 require(fn);
 return fn(device, pDeviceGroupPresentCapabilities);
}
#if defined(VK_USE_PLATFORM_WIN32_KHR)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetDeviceGroupSurfacePresentModes2EXT(VkDevice device, const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo, VkDeviceGroupPresentModeFlagsKHR* pModes) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceGroupSurfacePresentModes2EXT;require(pfn);return pfn(device, pSurfaceInfo, pModes);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceGroupSurfacePresentModes2EXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceGroupSurfacePresentModes2EXT");
 require(fn);
 return fn(device, pSurfaceInfo, pModes);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetDeviceGroupSurfacePresentModesKHR(VkDevice device, VkSurfaceKHR surface, VkDeviceGroupPresentModeFlagsKHR* pModes) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceGroupSurfacePresentModesKHR;require(pfn);return pfn(device, surface, pModes);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceGroupSurfacePresentModesKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceGroupSurfacePresentModesKHR");
 require(fn);
 return fn(device, surface, pModes);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetDeviceImageMemoryRequirements(VkDevice device, const VkDeviceImageMemoryRequirements* pInfo, VkMemoryRequirements2* pMemoryRequirements) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceImageMemoryRequirements;require(pfn);pfn(device, pInfo, pMemoryRequirements);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceImageMemoryRequirements;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceImageMemoryRequirements");
 require(fn);
 fn(device, pInfo, pMemoryRequirements);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetDeviceImageMemoryRequirementsKHR(VkDevice device, const VkDeviceImageMemoryRequirements* pInfo, VkMemoryRequirements2* pMemoryRequirements) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceImageMemoryRequirementsKHR;require(pfn);pfn(device, pInfo, pMemoryRequirements);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceImageMemoryRequirementsKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceImageMemoryRequirementsKHR");
 require(fn);
 fn(device, pInfo, pMemoryRequirements);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetDeviceImageSparseMemoryRequirements(VkDevice device, const VkDeviceImageMemoryRequirements* pInfo, uint32_t* pSparseMemoryRequirementCount, VkSparseImageMemoryRequirements2* pSparseMemoryRequirements) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceImageSparseMemoryRequirements;require(pfn);pfn(device, pInfo, pSparseMemoryRequirementCount, pSparseMemoryRequirements);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceImageSparseMemoryRequirements;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceImageSparseMemoryRequirements");
 require(fn);
 fn(device, pInfo, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetDeviceImageSparseMemoryRequirementsKHR(VkDevice device, const VkDeviceImageMemoryRequirements* pInfo, uint32_t* pSparseMemoryRequirementCount, VkSparseImageMemoryRequirements2* pSparseMemoryRequirements) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceImageSparseMemoryRequirementsKHR;require(pfn);pfn(device, pInfo, pSparseMemoryRequirementCount, pSparseMemoryRequirements);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceImageSparseMemoryRequirementsKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceImageSparseMemoryRequirementsKHR");
 require(fn);
 fn(device, pInfo, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetDeviceImageSubresourceLayout(VkDevice device, const VkDeviceImageSubresourceInfo* pInfo, VkSubresourceLayout2* pLayout) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceImageSubresourceLayout;require(pfn);pfn(device, pInfo, pLayout);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceImageSubresourceLayout;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceImageSubresourceLayout");
 require(fn);
 fn(device, pInfo, pLayout);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetDeviceImageSubresourceLayoutKHR(VkDevice device, const VkDeviceImageSubresourceInfo* pInfo, VkSubresourceLayout2* pLayout) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceImageSubresourceLayoutKHR;require(pfn);pfn(device, pInfo, pLayout);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceImageSubresourceLayoutKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceImageSubresourceLayoutKHR");
 require(fn);
 fn(device, pInfo, pLayout);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetDeviceMemoryCommitment(VkDevice device, VkDeviceMemory memory, VkDeviceSize* pCommittedMemoryInBytes) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceMemoryCommitment;require(pfn);pfn(device, memory, pCommittedMemoryInBytes);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceMemoryCommitment;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceMemoryCommitment");
 require(fn);
 fn(device, memory, pCommittedMemoryInBytes);
}
VKAPI_ATTR uint64_t VKAPI_CALL wrap_vkGetDeviceMemoryOpaqueCaptureAddress(VkDevice device, const VkDeviceMemoryOpaqueCaptureAddressInfo* pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceMemoryOpaqueCaptureAddress;require(pfn);return pfn(device, pInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceMemoryOpaqueCaptureAddress;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceMemoryOpaqueCaptureAddress");
 require(fn);
 return fn(device, pInfo);
}
VKAPI_ATTR uint64_t VKAPI_CALL wrap_vkGetDeviceMemoryOpaqueCaptureAddressKHR(VkDevice device, const VkDeviceMemoryOpaqueCaptureAddressInfo* pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceMemoryOpaqueCaptureAddressKHR;require(pfn);return pfn(device, pInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceMemoryOpaqueCaptureAddressKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceMemoryOpaqueCaptureAddressKHR");
 require(fn);
 return fn(device, pInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetDeviceMicromapCompatibilityEXT(VkDevice                                 device, const VkMicromapVersionInfoEXT*          pVersionInfo, VkAccelerationStructureCompatibilityKHR* pCompatibility) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceMicromapCompatibilityEXT;require(pfn);pfn(device, pVersionInfo, pCompatibility);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceMicromapCompatibilityEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceMicromapCompatibilityEXT");
 require(fn);
 fn(device, pVersionInfo, pCompatibility);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceQueue;require(pfn);pfn(device, queueFamilyIndex, queueIndex, pQueue);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceQueue;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceQueue");
 require(fn);
 fn(device, queueFamilyIndex, queueIndex, pQueue);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* pQueueInfo, VkQueue* pQueue) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceQueue2;require(pfn);pfn(device, pQueueInfo, pQueue);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceQueue2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceQueue2");
 require(fn);
 fn(device, pQueueInfo, pQueue);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetDeviceSubpassShadingMaxWorkgroupSizeHUAWEI(VkDevice device, VkRenderPass renderpass, VkExtent2D* pMaxWorkgroupSize) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceSubpassShadingMaxWorkgroupSizeHUAWEI;require(pfn);return pfn(device, renderpass, pMaxWorkgroupSize);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceSubpassShadingMaxWorkgroupSizeHUAWEI;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceSubpassShadingMaxWorkgroupSizeHUAWEI");
 require(fn);
 return fn(device, renderpass, pMaxWorkgroupSize);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetDeviceTensorMemoryRequirementsARM(VkDevice device, const VkDeviceTensorMemoryRequirementsARM* pInfo, VkMemoryRequirements2* pMemoryRequirements) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDeviceTensorMemoryRequirementsARM;require(pfn);pfn(device, pInfo, pMemoryRequirements);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDeviceTensorMemoryRequirementsARM;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDeviceTensorMemoryRequirementsARM");
 require(fn);
 fn(device, pInfo, pMemoryRequirements);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetDisplayModeProperties2KHR(VkPhysicalDevice physicalDevice, VkDisplayKHR display, uint32_t* pPropertyCount, VkDisplayModeProperties2KHR* pProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetDisplayModeProperties2KHR>(i->next(i->handle,"vkGetDisplayModeProperties2KHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, display, pPropertyCount, pProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetDisplayModePropertiesKHR(VkPhysicalDevice physicalDevice, VkDisplayKHR display, uint32_t* pPropertyCount, VkDisplayModePropertiesKHR* pProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetDisplayModePropertiesKHR>(i->next(i->handle,"vkGetDisplayModePropertiesKHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, display, pPropertyCount, pProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetDisplayPlaneCapabilities2KHR(VkPhysicalDevice physicalDevice, const VkDisplayPlaneInfo2KHR* pDisplayPlaneInfo, VkDisplayPlaneCapabilities2KHR* pCapabilities) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetDisplayPlaneCapabilities2KHR>(i->next(i->handle,"vkGetDisplayPlaneCapabilities2KHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pDisplayPlaneInfo, pCapabilities);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetDisplayPlaneCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkDisplayModeKHR mode, uint32_t planeIndex, VkDisplayPlaneCapabilitiesKHR* pCapabilities) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetDisplayPlaneCapabilitiesKHR>(i->next(i->handle,"vkGetDisplayPlaneCapabilitiesKHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, mode, planeIndex, pCapabilities);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetDisplayPlaneSupportedDisplaysKHR(VkPhysicalDevice physicalDevice, uint32_t planeIndex, uint32_t* pDisplayCount, VkDisplayKHR* pDisplays) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetDisplayPlaneSupportedDisplaysKHR>(i->next(i->handle,"vkGetDisplayPlaneSupportedDisplaysKHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, planeIndex, pDisplayCount, pDisplays);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetDrmDisplayEXT(VkPhysicalDevice physicalDevice, int32_t drmFd, uint32_t connectorId, VkDisplayKHR* display) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetDrmDisplayEXT>(i->next(i->handle,"vkGetDrmDisplayEXT"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, drmFd, connectorId, display);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetDynamicRenderingTilePropertiesQCOM(VkDevice device, const VkRenderingInfo* pRenderingInfo, VkTilePropertiesQCOM* pProperties) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetDynamicRenderingTilePropertiesQCOM;require(pfn);return pfn(device, pRenderingInfo, pProperties);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetDynamicRenderingTilePropertiesQCOM;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetDynamicRenderingTilePropertiesQCOM");
 require(fn);
 return fn(device, pRenderingInfo, pProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetEncodedVideoSessionParametersKHR(VkDevice device, const VkVideoEncodeSessionParametersGetInfoKHR* pVideoSessionParametersInfo, VkVideoEncodeSessionParametersFeedbackInfoKHR* pFeedbackInfo, size_t* pDataSize, void* pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetEncodedVideoSessionParametersKHR;require(pfn);return pfn(device, pVideoSessionParametersInfo, pFeedbackInfo, pDataSize, pData);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetEncodedVideoSessionParametersKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetEncodedVideoSessionParametersKHR");
 require(fn);
 return fn(device, pVideoSessionParametersInfo, pFeedbackInfo, pDataSize, pData);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetEventStatus(VkDevice device, VkEvent event) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetEventStatus;require(pfn);return pfn(device, event);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetEventStatus;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetEventStatus");
 require(fn);
 return fn(device, event);
}
#if defined(VK_ENABLE_BETA_EXTENSIONS)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetExecutionGraphPipelineNodeIndexAMDX(VkDevice                                        device, VkPipeline                                      executionGraph, const VkPipelineShaderStageNodeCreateInfoAMDX*  pNodeInfo, uint32_t*                                       pNodeIndex) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetExecutionGraphPipelineNodeIndexAMDX;require(pfn);return pfn(device, executionGraph, pNodeInfo, pNodeIndex);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetExecutionGraphPipelineNodeIndexAMDX;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetExecutionGraphPipelineNodeIndexAMDX");
 require(fn);
 return fn(device, executionGraph, pNodeInfo, pNodeIndex);
}
#endif
#if defined(VK_ENABLE_BETA_EXTENSIONS)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetExecutionGraphPipelineScratchSizeAMDX(VkDevice                                        device, VkPipeline                                      executionGraph, VkExecutionGraphPipelineScratchSizeAMDX*        pSizeInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetExecutionGraphPipelineScratchSizeAMDX;require(pfn);return pfn(device, executionGraph, pSizeInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetExecutionGraphPipelineScratchSizeAMDX;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetExecutionGraphPipelineScratchSizeAMDX");
 require(fn);
 return fn(device, executionGraph, pSizeInfo);
}
#endif
VKAPI_ATTR void VKAPI_CALL wrap_vkGetExternalComputeQueueDataNV(VkExternalComputeQueueNV externalQueue, VkExternalComputeQueueDataParamsNV* params, void* pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(externalQueue);auto pfn=p->dispatch.vkGetExternalComputeQueueDataNV;require(pfn);pfn(externalQueue, params, pData);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,externalQueue); auto fn=d->dispatch.vkGetExternalComputeQueueDataNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetExternalComputeQueueDataNV");
 require(fn);
 fn(externalQueue, params, pData);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetFenceFdKHR(VkDevice device, const VkFenceGetFdInfoKHR* pGetFdInfo, int* pFd) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetFenceFdKHR;require(pfn);return pfn(device, pGetFdInfo, pFd);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetFenceFdKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetFenceFdKHR");
 require(fn);
 return fn(device, pGetFdInfo, pFd);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetFenceStatus(VkDevice device, VkFence fence) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetFenceStatus;require(pfn);return pfn(device, fence);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetFenceStatus;
 require(fn);
 return fn(device, fence);
}
#if defined(VK_USE_PLATFORM_WIN32_KHR)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetFenceWin32HandleKHR(VkDevice device, const VkFenceGetWin32HandleInfoKHR* pGetWin32HandleInfo, HANDLE* pHandle) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetFenceWin32HandleKHR;require(pfn);return pfn(device, pGetWin32HandleInfo, pHandle);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetFenceWin32HandleKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetFenceWin32HandleKHR");
 require(fn);
 return fn(device, pGetWin32HandleInfo, pHandle);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetFramebufferTilePropertiesQCOM(VkDevice device, VkFramebuffer framebuffer, uint32_t* pPropertiesCount, VkTilePropertiesQCOM* pProperties) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetFramebufferTilePropertiesQCOM;require(pfn);return pfn(device, framebuffer, pPropertiesCount, pProperties);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetFramebufferTilePropertiesQCOM;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetFramebufferTilePropertiesQCOM");
 require(fn);
 return fn(device, framebuffer, pPropertiesCount, pProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetGeneratedCommandsMemoryRequirementsEXT(VkDevice device, const VkGeneratedCommandsMemoryRequirementsInfoEXT* pInfo, VkMemoryRequirements2* pMemoryRequirements) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetGeneratedCommandsMemoryRequirementsEXT;require(pfn);pfn(device, pInfo, pMemoryRequirements);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetGeneratedCommandsMemoryRequirementsEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetGeneratedCommandsMemoryRequirementsEXT");
 require(fn);
 fn(device, pInfo, pMemoryRequirements);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetGeneratedCommandsMemoryRequirementsNV(VkDevice device, const VkGeneratedCommandsMemoryRequirementsInfoNV* pInfo, VkMemoryRequirements2* pMemoryRequirements) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetGeneratedCommandsMemoryRequirementsNV;require(pfn);pfn(device, pInfo, pMemoryRequirements);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetGeneratedCommandsMemoryRequirementsNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetGeneratedCommandsMemoryRequirementsNV");
 require(fn);
 fn(device, pInfo, pMemoryRequirements);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetGpaDeviceClockInfoAMD(VkDevice                    device, VkGpaDeviceGetClockInfoAMD* pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetGpaDeviceClockInfoAMD;require(pfn);return pfn(device, pInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetGpaDeviceClockInfoAMD;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetGpaDeviceClockInfoAMD");
 require(fn);
 return fn(device, pInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetGpaSessionResultsAMD(VkDevice                                 device, VkGpaSessionAMD                          gpaSession, uint32_t                                 sampleID, size_t*            pSizeInBytes, void* pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetGpaSessionResultsAMD;require(pfn);return pfn(device, gpaSession, sampleID, pSizeInBytes, pData);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetGpaSessionResultsAMD;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetGpaSessionResultsAMD");
 require(fn);
 return fn(device, gpaSession, sampleID, pSizeInBytes, pData);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetGpaSessionStatusAMD(VkDevice        device, VkGpaSessionAMD gpaSession) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetGpaSessionStatusAMD;require(pfn);return pfn(device, gpaSession);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetGpaSessionStatusAMD;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetGpaSessionStatusAMD");
 require(fn);
 return fn(device, gpaSession);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetImageDrmFormatModifierPropertiesEXT(VkDevice device, VkImage image, VkImageDrmFormatModifierPropertiesEXT* pProperties) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetImageDrmFormatModifierPropertiesEXT;require(pfn);return pfn(device, image, pProperties);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetImageDrmFormatModifierPropertiesEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetImageDrmFormatModifierPropertiesEXT");
 require(fn);
 return fn(device, image, pProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetImageMemoryRequirements(VkDevice device, VkImage image, VkMemoryRequirements* pMemoryRequirements) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetImageMemoryRequirements;require(pfn);pfn(device, image, pMemoryRequirements);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetImageMemoryRequirements;
 require(fn);
 fn(device, image, pMemoryRequirements);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetImageMemoryRequirements2(VkDevice device, const VkImageMemoryRequirementsInfo2* pInfo, VkMemoryRequirements2* pMemoryRequirements) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetImageMemoryRequirements2;require(pfn);pfn(device, pInfo, pMemoryRequirements);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetImageMemoryRequirements2;
 require(fn);
 fn(device, pInfo, pMemoryRequirements);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetImageMemoryRequirements2KHR(VkDevice device, const VkImageMemoryRequirementsInfo2* pInfo, VkMemoryRequirements2* pMemoryRequirements) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetImageMemoryRequirements2KHR;require(pfn);pfn(device, pInfo, pMemoryRequirements);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetImageMemoryRequirements2KHR;
 require(fn);
 fn(device, pInfo, pMemoryRequirements);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetImageOpaqueCaptureDataEXT(VkDevice                                            device, uint32_t                                            imageCount, const VkImage*                     pImages, VkHostAddressRangeEXT*             pDatas) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetImageOpaqueCaptureDataEXT;require(pfn);return pfn(device, imageCount, pImages, pDatas);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetImageOpaqueCaptureDataEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetImageOpaqueCaptureDataEXT");
 require(fn);
 return fn(device, imageCount, pImages, pDatas);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetImageOpaqueCaptureDescriptorDataEXT(VkDevice device, const VkImageCaptureDescriptorDataInfoEXT* pInfo, void* pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetImageOpaqueCaptureDescriptorDataEXT;require(pfn);return pfn(device, pInfo, pData);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetImageOpaqueCaptureDescriptorDataEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetImageOpaqueCaptureDescriptorDataEXT");
 require(fn);
 return fn(device, pInfo, pData);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetImageSparseMemoryRequirements(VkDevice device, VkImage image, uint32_t* pSparseMemoryRequirementCount, VkSparseImageMemoryRequirements* pSparseMemoryRequirements) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetImageSparseMemoryRequirements;require(pfn);pfn(device, image, pSparseMemoryRequirementCount, pSparseMemoryRequirements);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetImageSparseMemoryRequirements;
 require(fn);
 fn(device, image, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetImageSparseMemoryRequirements2(VkDevice device, const VkImageSparseMemoryRequirementsInfo2* pInfo, uint32_t* pSparseMemoryRequirementCount, VkSparseImageMemoryRequirements2* pSparseMemoryRequirements) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetImageSparseMemoryRequirements2;require(pfn);pfn(device, pInfo, pSparseMemoryRequirementCount, pSparseMemoryRequirements);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetImageSparseMemoryRequirements2;
 require(fn);
 fn(device, pInfo, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetImageSparseMemoryRequirements2KHR(VkDevice device, const VkImageSparseMemoryRequirementsInfo2* pInfo, uint32_t* pSparseMemoryRequirementCount, VkSparseImageMemoryRequirements2* pSparseMemoryRequirements) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetImageSparseMemoryRequirements2KHR;require(pfn);pfn(device, pInfo, pSparseMemoryRequirementCount, pSparseMemoryRequirements);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetImageSparseMemoryRequirements2KHR;
 require(fn);
 fn(device, pInfo, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetImageSubresourceLayout(VkDevice device, VkImage image, const VkImageSubresource* pSubresource, VkSubresourceLayout* pLayout) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetImageSubresourceLayout;require(pfn);pfn(device, image, pSubresource, pLayout);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetImageSubresourceLayout;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetImageSubresourceLayout");
 require(fn);
 fn(device, image, pSubresource, pLayout);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetImageSubresourceLayout2(VkDevice device, VkImage image, const VkImageSubresource2* pSubresource, VkSubresourceLayout2* pLayout) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetImageSubresourceLayout2;require(pfn);pfn(device, image, pSubresource, pLayout);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetImageSubresourceLayout2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetImageSubresourceLayout2");
 require(fn);
 fn(device, image, pSubresource, pLayout);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetImageSubresourceLayout2EXT(VkDevice device, VkImage image, const VkImageSubresource2* pSubresource, VkSubresourceLayout2* pLayout) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetImageSubresourceLayout2EXT;require(pfn);pfn(device, image, pSubresource, pLayout);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetImageSubresourceLayout2EXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetImageSubresourceLayout2EXT");
 require(fn);
 fn(device, image, pSubresource, pLayout);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetImageSubresourceLayout2KHR(VkDevice device, VkImage image, const VkImageSubresource2* pSubresource, VkSubresourceLayout2* pLayout) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetImageSubresourceLayout2KHR;require(pfn);pfn(device, image, pSubresource, pLayout);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetImageSubresourceLayout2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetImageSubresourceLayout2KHR");
 require(fn);
 fn(device, image, pSubresource, pLayout);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetImageViewAddressNVX(VkDevice device, VkImageView imageView, VkImageViewAddressPropertiesNVX* pProperties) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetImageViewAddressNVX;require(pfn);return pfn(device, imageView, pProperties);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetImageViewAddressNVX;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetImageViewAddressNVX");
 require(fn);
 return fn(device, imageView, pProperties);
}
VKAPI_ATTR uint64_t VKAPI_CALL wrap_vkGetImageViewHandle64NVX(VkDevice device, const VkImageViewHandleInfoNVX* pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetImageViewHandle64NVX;require(pfn);return pfn(device, pInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetImageViewHandle64NVX;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetImageViewHandle64NVX");
 require(fn);
 return fn(device, pInfo);
}
VKAPI_ATTR uint32_t VKAPI_CALL wrap_vkGetImageViewHandleNVX(VkDevice device, const VkImageViewHandleInfoNVX* pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetImageViewHandleNVX;require(pfn);return pfn(device, pInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetImageViewHandleNVX;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetImageViewHandleNVX");
 require(fn);
 return fn(device, pInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetImageViewOpaqueCaptureDescriptorDataEXT(VkDevice device, const VkImageViewCaptureDescriptorDataInfoEXT* pInfo, void* pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetImageViewOpaqueCaptureDescriptorDataEXT;require(pfn);return pfn(device, pInfo, pData);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetImageViewOpaqueCaptureDescriptorDataEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetImageViewOpaqueCaptureDescriptorDataEXT");
 require(fn);
 return fn(device, pInfo, pData);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetLatencyTimingsLegacyNV(VkDevice device, void* pTimings) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetLatencyTimingsLegacyNV;require(pfn);pfn(device, pTimings);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetLatencyTimingsLegacyNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetLatencyTimingsLegacyNV");
 require(fn);
 fn(device, pTimings);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetLatencyTimingsNV(VkDevice device, VkSwapchainKHR swapchain, VkGetLatencyMarkerInfoNV* pLatencyMarkerInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetLatencyTimingsNV;require(pfn);pfn(device, swapchain, pLatencyMarkerInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetLatencyTimingsNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetLatencyTimingsNV");
 require(fn);
 fn(device, swapchain, pLatencyMarkerInfo);
}
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetMemoryAndroidHardwareBufferANDROID(VkDevice device, const VkMemoryGetAndroidHardwareBufferInfoANDROID* pInfo, struct AHardwareBuffer** pBuffer) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetMemoryAndroidHardwareBufferANDROID;require(pfn);return pfn(device, pInfo, pBuffer);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetMemoryAndroidHardwareBufferANDROID;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetMemoryAndroidHardwareBufferANDROID");
 require(fn);
 return fn(device, pInfo, pBuffer);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetMemoryFdKHR(VkDevice device, const VkMemoryGetFdInfoKHR* pGetFdInfo, int* pFd) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetMemoryFdKHR;require(pfn);return pfn(device, pGetFdInfo, pFd);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetMemoryFdKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetMemoryFdKHR");
 require(fn);
 return fn(device, pGetFdInfo, pFd);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetMemoryFdPropertiesKHR(VkDevice device, VkExternalMemoryHandleTypeFlagBits handleType, int fd, VkMemoryFdPropertiesKHR* pMemoryFdProperties) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetMemoryFdPropertiesKHR;require(pfn);return pfn(device, handleType, fd, pMemoryFdProperties);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetMemoryFdPropertiesKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetMemoryFdPropertiesKHR");
 require(fn);
 return fn(device, handleType, fd, pMemoryFdProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetMemoryHostPointerPropertiesEXT(VkDevice device, VkExternalMemoryHandleTypeFlagBits handleType, const void* pHostPointer, VkMemoryHostPointerPropertiesEXT* pMemoryHostPointerProperties) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetMemoryHostPointerPropertiesEXT;require(pfn);return pfn(device, handleType, pHostPointer, pMemoryHostPointerProperties);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetMemoryHostPointerPropertiesEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetMemoryHostPointerPropertiesEXT");
 require(fn);
 return fn(device, handleType, pHostPointer, pMemoryHostPointerProperties);
}
#if defined(VK_USE_PLATFORM_METAL_EXT)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetMemoryMetalHandleEXT(VkDevice device, const VkMemoryGetMetalHandleInfoEXT* pGetMetalHandleInfo, void** pHandle) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetMemoryMetalHandleEXT;require(pfn);return pfn(device, pGetMetalHandleInfo, pHandle);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetMemoryMetalHandleEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetMemoryMetalHandleEXT");
 require(fn);
 return fn(device, pGetMetalHandleInfo, pHandle);
}
#endif
#if defined(VK_USE_PLATFORM_METAL_EXT)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetMemoryMetalHandlePropertiesEXT(VkDevice device, VkExternalMemoryHandleTypeFlagBits handleType, const void* pHandle, VkMemoryMetalHandlePropertiesEXT* pMemoryMetalHandleProperties) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetMemoryMetalHandlePropertiesEXT;require(pfn);return pfn(device, handleType, pHandle, pMemoryMetalHandleProperties);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetMemoryMetalHandlePropertiesEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetMemoryMetalHandlePropertiesEXT");
 require(fn);
 return fn(device, handleType, pHandle, pMemoryMetalHandleProperties);
}
#endif
#if defined(VK_USE_PLATFORM_OHOS)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetMemoryNativeBufferOHOS(VkDevice device, const VkMemoryGetNativeBufferInfoOHOS* pInfo, struct OH_NativeBuffer** pBuffer) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetMemoryNativeBufferOHOS;require(pfn);return pfn(device, pInfo, pBuffer);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetMemoryNativeBufferOHOS;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetMemoryNativeBufferOHOS");
 require(fn);
 return fn(device, pInfo, pBuffer);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetMemoryRemoteAddressNV(VkDevice device, const VkMemoryGetRemoteAddressInfoNV* pMemoryGetRemoteAddressInfo, VkRemoteAddressNV* pAddress) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetMemoryRemoteAddressNV;require(pfn);return pfn(device, pMemoryGetRemoteAddressInfo, pAddress);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetMemoryRemoteAddressNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetMemoryRemoteAddressNV");
 require(fn);
 return fn(device, pMemoryGetRemoteAddressInfo, pAddress);
}
#if defined(VK_USE_PLATFORM_WIN32_KHR)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetMemoryWin32HandleKHR(VkDevice device, const VkMemoryGetWin32HandleInfoKHR* pGetWin32HandleInfo, HANDLE* pHandle) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetMemoryWin32HandleKHR;require(pfn);return pfn(device, pGetWin32HandleInfo, pHandle);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetMemoryWin32HandleKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetMemoryWin32HandleKHR");
 require(fn);
 return fn(device, pGetWin32HandleInfo, pHandle);
}
#endif
#if defined(VK_USE_PLATFORM_WIN32_KHR)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetMemoryWin32HandleNV(VkDevice device, VkDeviceMemory memory, VkExternalMemoryHandleTypeFlagsNV handleType, HANDLE* pHandle) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetMemoryWin32HandleNV;require(pfn);return pfn(device, memory, handleType, pHandle);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetMemoryWin32HandleNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetMemoryWin32HandleNV");
 require(fn);
 return fn(device, memory, handleType, pHandle);
}
#endif
#if defined(VK_USE_PLATFORM_WIN32_KHR)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetMemoryWin32HandlePropertiesKHR(VkDevice device, VkExternalMemoryHandleTypeFlagBits handleType, HANDLE handle, VkMemoryWin32HandlePropertiesKHR* pMemoryWin32HandleProperties) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetMemoryWin32HandlePropertiesKHR;require(pfn);return pfn(device, handleType, handle, pMemoryWin32HandleProperties);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetMemoryWin32HandlePropertiesKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetMemoryWin32HandlePropertiesKHR");
 require(fn);
 return fn(device, handleType, handle, pMemoryWin32HandleProperties);
}
#endif
#if defined(VK_USE_PLATFORM_FUCHSIA)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetMemoryZirconHandleFUCHSIA(VkDevice device, const VkMemoryGetZirconHandleInfoFUCHSIA* pGetZirconHandleInfo, zx_handle_t* pZirconHandle) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetMemoryZirconHandleFUCHSIA;require(pfn);return pfn(device, pGetZirconHandleInfo, pZirconHandle);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetMemoryZirconHandleFUCHSIA;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetMemoryZirconHandleFUCHSIA");
 require(fn);
 return fn(device, pGetZirconHandleInfo, pZirconHandle);
}
#endif
#if defined(VK_USE_PLATFORM_FUCHSIA)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetMemoryZirconHandlePropertiesFUCHSIA(VkDevice device, VkExternalMemoryHandleTypeFlagBits handleType, zx_handle_t zirconHandle, VkMemoryZirconHandlePropertiesFUCHSIA* pMemoryZirconHandleProperties) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetMemoryZirconHandlePropertiesFUCHSIA;require(pfn);return pfn(device, handleType, zirconHandle, pMemoryZirconHandleProperties);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetMemoryZirconHandlePropertiesFUCHSIA;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetMemoryZirconHandlePropertiesFUCHSIA");
 require(fn);
 return fn(device, handleType, zirconHandle, pMemoryZirconHandleProperties);
}
#endif
VKAPI_ATTR void VKAPI_CALL wrap_vkGetMicromapBuildSizesEXT(VkDevice                            device, VkAccelerationStructureBuildTypeKHR buildType, const VkMicromapBuildInfoEXT*       pBuildInfo, VkMicromapBuildSizesInfoEXT*        pSizeInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetMicromapBuildSizesEXT;require(pfn);pfn(device, buildType, pBuildInfo, pSizeInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetMicromapBuildSizesEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetMicromapBuildSizesEXT");
 require(fn);
 fn(device, buildType, pBuildInfo, pSizeInfo);
}
#if defined(VK_USE_PLATFORM_OHOS)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetNativeBufferPropertiesOHOS(VkDevice device, const struct OH_NativeBuffer* buffer, VkNativeBufferPropertiesOHOS* pProperties) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetNativeBufferPropertiesOHOS;require(pfn);return pfn(device, buffer, pProperties);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetNativeBufferPropertiesOHOS;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetNativeBufferPropertiesOHOS");
 require(fn);
 return fn(device, buffer, pProperties);
}
#endif
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPartitionedAccelerationStructuresBuildSizesNV(VkDevice device, const VkPartitionedAccelerationStructureInstancesInputNV* pInfo, VkAccelerationStructureBuildSizesInfoKHR*                  pSizeInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetPartitionedAccelerationStructuresBuildSizesNV;require(pfn);pfn(device, pInfo, pSizeInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetPartitionedAccelerationStructuresBuildSizesNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetPartitionedAccelerationStructuresBuildSizesNV");
 require(fn);
 fn(device, pInfo, pSizeInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPastPresentationTimingEXT(VkDevice device, const VkPastPresentationTimingInfoEXT* pPastPresentationTimingInfo, VkPastPresentationTimingPropertiesEXT* pPastPresentationTimingProperties) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetPastPresentationTimingEXT;require(pfn);return pfn(device, pPastPresentationTimingInfo, pPastPresentationTimingProperties);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetPastPresentationTimingEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetPastPresentationTimingEXT");
 require(fn);
 return fn(device, pPastPresentationTimingInfo, pPastPresentationTimingProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPastPresentationTimingGOOGLE(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pPresentationTimingCount, VkPastPresentationTimingGOOGLE* pPresentationTimings) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetPastPresentationTimingGOOGLE;require(pfn);return pfn(device, swapchain, pPresentationTimingCount, pPresentationTimings);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetPastPresentationTimingGOOGLE;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetPastPresentationTimingGOOGLE");
 require(fn);
 return fn(device, swapchain, pPresentationTimingCount, pPresentationTimings);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPerformanceParameterINTEL(VkDevice device, VkPerformanceParameterTypeINTEL parameter, VkPerformanceValueINTEL* pValue) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetPerformanceParameterINTEL;require(pfn);return pfn(device, parameter, pValue);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetPerformanceParameterINTEL;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetPerformanceParameterINTEL");
 require(fn);
 return fn(device, parameter, pValue);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(VkPhysicalDevice physicalDevice, uint32_t* pTimeDomainCount, VkTimeDomainKHR* pTimeDomains) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT>(i->next(i->handle,"vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pTimeDomainCount, pTimeDomains);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR(VkPhysicalDevice physicalDevice, uint32_t* pTimeDomainCount, VkTimeDomainKHR* pTimeDomains) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR>(i->next(i->handle,"vkGetPhysicalDeviceCalibrateableTimeDomainsKHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pTimeDomainCount, pTimeDomains);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceCooperativeMatrixFlexibleDimensionsPropertiesNV(VkPhysicalDevice physicalDevice, uint32_t* pPropertyCount, VkCooperativeMatrixFlexibleDimensionsPropertiesNV* pProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceCooperativeMatrixFlexibleDimensionsPropertiesNV>(i->next(i->handle,"vkGetPhysicalDeviceCooperativeMatrixFlexibleDimensionsPropertiesNV"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pPropertyCount, pProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(VkPhysicalDevice physicalDevice, uint32_t* pPropertyCount, VkCooperativeMatrixPropertiesKHR* pProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR>(i->next(i->handle,"vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pPropertyCount, pProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceCooperativeMatrixPropertiesNV(VkPhysicalDevice physicalDevice, uint32_t* pPropertyCount, VkCooperativeMatrixPropertiesNV* pProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesNV>(i->next(i->handle,"vkGetPhysicalDeviceCooperativeMatrixPropertiesNV"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pPropertyCount, pProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceCooperativeVectorPropertiesNV(VkPhysicalDevice physicalDevice, uint32_t* pPropertyCount, VkCooperativeVectorPropertiesNV* pProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceCooperativeVectorPropertiesNV>(i->next(i->handle,"vkGetPhysicalDeviceCooperativeVectorPropertiesNV"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pPropertyCount, pProperties);
}
VKAPI_ATTR VkDeviceSize VKAPI_CALL wrap_vkGetPhysicalDeviceDescriptorSizeEXT(VkPhysicalDevice                                    physicalDevice, VkDescriptorType                                    descriptorType) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceDescriptorSizeEXT>(i->next(i->handle,"vkGetPhysicalDeviceDescriptorSizeEXT"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, descriptorType);
}
#if defined(VK_USE_PLATFORM_DIRECTFB_EXT)
VKAPI_ATTR VkBool32 VKAPI_CALL wrap_vkGetPhysicalDeviceDirectFBPresentationSupportEXT(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, IDirectFB* dfb) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceDirectFBPresentationSupportEXT>(i->next(i->handle,"vkGetPhysicalDeviceDirectFBPresentationSupportEXT"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, queueFamilyIndex, dfb);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceDisplayPlaneProperties2KHR(VkPhysicalDevice physicalDevice, uint32_t* pPropertyCount, VkDisplayPlaneProperties2KHR* pProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceDisplayPlaneProperties2KHR>(i->next(i->handle,"vkGetPhysicalDeviceDisplayPlaneProperties2KHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pPropertyCount, pProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceDisplayPlanePropertiesKHR(VkPhysicalDevice physicalDevice, uint32_t* pPropertyCount, VkDisplayPlanePropertiesKHR* pProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR>(i->next(i->handle,"vkGetPhysicalDeviceDisplayPlanePropertiesKHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pPropertyCount, pProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceDisplayProperties2KHR(VkPhysicalDevice physicalDevice, uint32_t* pPropertyCount, VkDisplayProperties2KHR* pProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceDisplayProperties2KHR>(i->next(i->handle,"vkGetPhysicalDeviceDisplayProperties2KHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pPropertyCount, pProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceDisplayPropertiesKHR(VkPhysicalDevice physicalDevice, uint32_t* pPropertyCount, VkDisplayPropertiesKHR* pProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceDisplayPropertiesKHR>(i->next(i->handle,"vkGetPhysicalDeviceDisplayPropertiesKHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pPropertyCount, pProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceExternalBufferInfo* pExternalBufferInfo, VkExternalBufferProperties* pExternalBufferProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceExternalBufferProperties>(i->next(i->handle,"vkGetPhysicalDeviceExternalBufferProperties"));
 InstanceEntries entries(i);
 require(fn);
 fn(physicalDevice, pExternalBufferInfo, pExternalBufferProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceExternalBufferPropertiesKHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceExternalBufferInfo* pExternalBufferInfo, VkExternalBufferProperties* pExternalBufferProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR>(i->next(i->handle,"vkGetPhysicalDeviceExternalBufferPropertiesKHR"));
 InstanceEntries entries(i);
 require(fn);
 fn(physicalDevice, pExternalBufferInfo, pExternalBufferProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceExternalFenceProperties(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceExternalFenceInfo* pExternalFenceInfo, VkExternalFenceProperties* pExternalFenceProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceExternalFenceProperties>(i->next(i->handle,"vkGetPhysicalDeviceExternalFenceProperties"));
 InstanceEntries entries(i);
 require(fn);
 fn(physicalDevice, pExternalFenceInfo, pExternalFenceProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceExternalFencePropertiesKHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceExternalFenceInfo* pExternalFenceInfo, VkExternalFenceProperties* pExternalFenceProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceExternalFencePropertiesKHR>(i->next(i->handle,"vkGetPhysicalDeviceExternalFencePropertiesKHR"));
 InstanceEntries entries(i);
 require(fn);
 fn(physicalDevice, pExternalFenceInfo, pExternalFenceProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceExternalImageFormatPropertiesNV(VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags, VkExternalMemoryHandleTypeFlagsNV externalHandleType, VkExternalImageFormatPropertiesNV* pExternalImageFormatProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceExternalImageFormatPropertiesNV>(i->next(i->handle,"vkGetPhysicalDeviceExternalImageFormatPropertiesNV"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, format, type, tiling, usage, flags, externalHandleType, pExternalImageFormatProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceExternalSemaphoreProperties(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo* pExternalSemaphoreInfo, VkExternalSemaphoreProperties* pExternalSemaphoreProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(i->next(i->handle,"vkGetPhysicalDeviceExternalSemaphoreProperties"));
 InstanceEntries entries(i);
 require(fn);
 fn(physicalDevice, pExternalSemaphoreInfo, pExternalSemaphoreProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo* pExternalSemaphoreInfo, VkExternalSemaphoreProperties* pExternalSemaphoreProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR>(i->next(i->handle,"vkGetPhysicalDeviceExternalSemaphorePropertiesKHR"));
 InstanceEntries entries(i);
 require(fn);
 fn(physicalDevice, pExternalSemaphoreInfo, pExternalSemaphoreProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceExternalTensorPropertiesARM(VkPhysicalDevice                             physicalDevice, const VkPhysicalDeviceExternalTensorInfoARM* pExternalTensorInfo, VkExternalTensorPropertiesARM*               pExternalTensorProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceExternalTensorPropertiesARM>(i->next(i->handle,"vkGetPhysicalDeviceExternalTensorPropertiesARM"));
 InstanceEntries entries(i);
 require(fn);
 fn(physicalDevice, pExternalTensorInfo, pExternalTensorProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures* pFeatures) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(i->next(i->handle,"vkGetPhysicalDeviceFeatures"));
 InstanceEntries entries(i);
 require(fn);
 fn(physicalDevice, pFeatures);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* pFeatures) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(i->next(i->handle,"vkGetPhysicalDeviceFeatures2"));
 InstanceEntries entries(i);
 require(fn);
 fn(physicalDevice, pFeatures);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceFeatures2KHR(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* pFeatures) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2KHR>(i->next(i->handle,"vkGetPhysicalDeviceFeatures2KHR"));
 InstanceEntries entries(i);
 require(fn);
 fn(physicalDevice, pFeatures);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties* pFormatProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(i->next(i->handle,"vkGetPhysicalDeviceFormatProperties"));
 InstanceEntries entries(i);
 require(fn);
 fn(physicalDevice, format, pFormatProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties2* pFormatProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties2>(i->next(i->handle,"vkGetPhysicalDeviceFormatProperties2"));
 InstanceEntries entries(i);
 require(fn);
 fn(physicalDevice, format, pFormatProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceFormatProperties2KHR(VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties2* pFormatProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties2KHR>(i->next(i->handle,"vkGetPhysicalDeviceFormatProperties2KHR"));
 InstanceEntries entries(i);
 require(fn);
 fn(physicalDevice, format, pFormatProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceFragmentShadingRatesKHR(VkPhysicalDevice physicalDevice, uint32_t* pFragmentShadingRateCount, VkPhysicalDeviceFragmentShadingRateKHR* pFragmentShadingRates) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR>(i->next(i->handle,"vkGetPhysicalDeviceFragmentShadingRatesKHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pFragmentShadingRateCount, pFragmentShadingRates);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties* pImageFormatProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties>(i->next(i->handle,"vkGetPhysicalDeviceImageFormatProperties"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, format, type, tiling, usage, flags, pImageFormatProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo, VkImageFormatProperties2* pImageFormatProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(i->next(i->handle,"vkGetPhysicalDeviceImageFormatProperties2"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pImageFormatInfo, pImageFormatProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceImageFormatProperties2KHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo, VkImageFormatProperties2* pImageFormatProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2KHR>(i->next(i->handle,"vkGetPhysicalDeviceImageFormatProperties2KHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pImageFormatInfo, pImageFormatProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties* pMemoryProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(i->next(i->handle,"vkGetPhysicalDeviceMemoryProperties"));
 InstanceEntries entries(i);
 require(fn);
 fn(physicalDevice, pMemoryProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties2* pMemoryProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(i->next(i->handle,"vkGetPhysicalDeviceMemoryProperties2"));
 InstanceEntries entries(i);
 require(fn);
 fn(physicalDevice, pMemoryProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceMemoryProperties2KHR(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties2* pMemoryProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2KHR>(i->next(i->handle,"vkGetPhysicalDeviceMemoryProperties2KHR"));
 InstanceEntries entries(i);
 require(fn);
 fn(physicalDevice, pMemoryProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceMultisamplePropertiesEXT(VkPhysicalDevice physicalDevice, VkSampleCountFlagBits samples, VkMultisamplePropertiesEXT* pMultisampleProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceMultisamplePropertiesEXT>(i->next(i->handle,"vkGetPhysicalDeviceMultisamplePropertiesEXT"));
 InstanceEntries entries(i);
 require(fn);
 fn(physicalDevice, samples, pMultisampleProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceOpticalFlowImageFormatsNV(VkPhysicalDevice physicalDevice, const VkOpticalFlowImageFormatInfoNV* pOpticalFlowImageFormatInfo, uint32_t* pFormatCount, VkOpticalFlowImageFormatPropertiesNV* pImageFormatProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceOpticalFlowImageFormatsNV>(i->next(i->handle,"vkGetPhysicalDeviceOpticalFlowImageFormatsNV"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pOpticalFlowImageFormatInfo, pFormatCount, pImageFormatProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pRectCount, VkRect2D* pRects) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDevicePresentRectanglesKHR>(i->next(i->handle,"vkGetPhysicalDevicePresentRectanglesKHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, surface, pRectCount, pRects);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties* pProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(i->next(i->handle,"vkGetPhysicalDeviceProperties"));
 require(fn);
 fn(physicalDevice, pProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2* pProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(i->next(i->handle,"vkGetPhysicalDeviceProperties2"));
 require(fn);
 fn(physicalDevice, pProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2* pProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2KHR>(i->next(i->handle,"vkGetPhysicalDeviceProperties2KHR"));
 require(fn);
 fn(physicalDevice, pProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceQueueFamilyDataGraphEngineOperationPropertiesARM(VkPhysicalDevice                                 physicalDevice, uint32_t                                         queueFamilyIndex, const VkQueueFamilyDataGraphPropertiesARM*       pQueueFamilyDataGraphProperties, VkBaseOutStructure*        pProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyDataGraphEngineOperationPropertiesARM>(i->next(i->handle,"vkGetPhysicalDeviceQueueFamilyDataGraphEngineOperationPropertiesARM"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, queueFamilyIndex, pQueueFamilyDataGraphProperties, pProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceQueueFamilyDataGraphOpticalFlowImageFormatsARM(VkPhysicalDevice                                physicalDevice, uint32_t                                        queueFamilyIndex, const VkQueueFamilyDataGraphPropertiesARM*      pQueueFamilyDataGraphProperties, const VkDataGraphOpticalFlowImageFormatInfoARM* pOpticalFlowImageFormatInfo, uint32_t*                 pFormatCount, VkDataGraphOpticalFlowImageFormatPropertiesARM* pImageFormatProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyDataGraphOpticalFlowImageFormatsARM>(i->next(i->handle,"vkGetPhysicalDeviceQueueFamilyDataGraphOpticalFlowImageFormatsARM"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, queueFamilyIndex, pQueueFamilyDataGraphProperties, pOpticalFlowImageFormatInfo, pFormatCount, pImageFormatProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceQueueFamilyDataGraphProcessingEnginePropertiesARM(VkPhysicalDevice                                 physicalDevice, const VkPhysicalDeviceQueueFamilyDataGraphProcessingEngineInfoARM* pQueueFamilyDataGraphProcessingEngineInfo, VkQueueFamilyDataGraphProcessingEnginePropertiesARM*               pQueueFamilyDataGraphProcessingEngineProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyDataGraphProcessingEnginePropertiesARM>(i->next(i->handle,"vkGetPhysicalDeviceQueueFamilyDataGraphProcessingEnginePropertiesARM"));
 InstanceEntries entries(i);
 require(fn);
 fn(physicalDevice, pQueueFamilyDataGraphProcessingEngineInfo, pQueueFamilyDataGraphProcessingEngineProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceQueueFamilyDataGraphPropertiesARM(VkPhysicalDevice                     physicalDevice, uint32_t                             queueFamilyIndex, uint32_t*                            pQueueFamilyDataGraphPropertyCount, VkQueueFamilyDataGraphPropertiesARM* pQueueFamilyDataGraphProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyDataGraphPropertiesARM>(i->next(i->handle,"vkGetPhysicalDeviceQueueFamilyDataGraphPropertiesARM"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, queueFamilyIndex, pQueueFamilyDataGraphPropertyCount, pQueueFamilyDataGraphProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR(VkPhysicalDevice physicalDevice, const VkQueryPoolPerformanceCreateInfoKHR* pPerformanceQueryCreateInfo, uint32_t* pNumPasses) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR>(i->next(i->handle,"vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR"));
 InstanceEntries entries(i);
 require(fn);
 fn(physicalDevice, pPerformanceQueryCreateInfo, pNumPasses);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice, uint32_t* pQueueFamilyPropertyCount, VkQueueFamilyProperties* pQueueFamilyProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(i->next(i->handle,"vkGetPhysicalDeviceQueueFamilyProperties"));
 require(fn);
 fn(physicalDevice, pQueueFamilyPropertyCount, pQueueFamilyProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice, uint32_t* pQueueFamilyPropertyCount, VkQueueFamilyProperties2* pQueueFamilyProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties2>(i->next(i->handle,"vkGetPhysicalDeviceQueueFamilyProperties2"));
 require(fn);
 fn(physicalDevice, pQueueFamilyPropertyCount, pQueueFamilyProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceQueueFamilyProperties2KHR(VkPhysicalDevice physicalDevice, uint32_t* pQueueFamilyPropertyCount, VkQueueFamilyProperties2* pQueueFamilyProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties2KHR>(i->next(i->handle,"vkGetPhysicalDeviceQueueFamilyProperties2KHR"));
 require(fn);
 fn(physicalDevice, pQueueFamilyPropertyCount, pQueueFamilyProperties);
}
#if defined(VK_USE_PLATFORM_SCREEN_QNX)
VKAPI_ATTR VkBool32 VKAPI_CALL wrap_vkGetPhysicalDeviceScreenPresentationSupportQNX(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, struct _screen_window* window) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceScreenPresentationSupportQNX>(i->next(i->handle,"vkGetPhysicalDeviceScreenPresentationSupportQNX"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, queueFamilyIndex, window);
}
#endif
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageTiling tiling, uint32_t* pPropertyCount, VkSparseImageFormatProperties* pProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceSparseImageFormatProperties>(i->next(i->handle,"vkGetPhysicalDeviceSparseImageFormatProperties"));
 InstanceEntries entries(i);
 require(fn);
 fn(physicalDevice, format, type, samples, usage, tiling, pPropertyCount, pProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceSparseImageFormatProperties2(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2* pFormatInfo, uint32_t* pPropertyCount, VkSparseImageFormatProperties2* pProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceSparseImageFormatProperties2>(i->next(i->handle,"vkGetPhysicalDeviceSparseImageFormatProperties2"));
 InstanceEntries entries(i);
 require(fn);
 fn(physicalDevice, pFormatInfo, pPropertyCount, pProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPhysicalDeviceSparseImageFormatProperties2KHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2* pFormatInfo, uint32_t* pPropertyCount, VkSparseImageFormatProperties2* pProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceSparseImageFormatProperties2KHR>(i->next(i->handle,"vkGetPhysicalDeviceSparseImageFormatProperties2KHR"));
 InstanceEntries entries(i);
 require(fn);
 fn(physicalDevice, pFormatInfo, pPropertyCount, pProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV(VkPhysicalDevice physicalDevice, uint32_t* pCombinationCount, VkFramebufferMixedSamplesCombinationNV* pCombinations) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV>(i->next(i->handle,"vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pCombinationCount, pCombinations);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceSurfaceCapabilities2EXT(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilities2EXT* pSurfaceCapabilities) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2EXT>(i->next(i->handle,"vkGetPhysicalDeviceSurfaceCapabilities2EXT"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, surface, pSurfaceCapabilities);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo, VkSurfaceCapabilities2KHR* pSurfaceCapabilities) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(i->next(i->handle,"vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pSurfaceInfo, pSurfaceCapabilities);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* pSurfaceCapabilities) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(i->next(i->handle,"vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, surface, pSurfaceCapabilities);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo, uint32_t* pSurfaceFormatCount, VkSurfaceFormat2KHR* pSurfaceFormats) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormats2KHR>(i->next(i->handle,"vkGetPhysicalDeviceSurfaceFormats2KHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pSurfaceInfo, pSurfaceFormatCount, pSurfaceFormats);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pSurfaceFormatCount, VkSurfaceFormatKHR* pSurfaceFormats) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(i->next(i->handle,"vkGetPhysicalDeviceSurfaceFormatsKHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, surface, pSurfaceFormatCount, pSurfaceFormats);
}
#if defined(VK_USE_PLATFORM_WIN32_KHR)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceSurfacePresentModes2EXT(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo, uint32_t* pPresentModeCount, VkPresentModeKHR* pPresentModes) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModes2EXT>(i->next(i->handle,"vkGetPhysicalDeviceSurfacePresentModes2EXT"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pSurfaceInfo, pPresentModeCount, pPresentModes);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pPresentModeCount, VkPresentModeKHR* pPresentModes) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(i->next(i->handle,"vkGetPhysicalDeviceSurfacePresentModesKHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, surface, pPresentModeCount, pPresentModes);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, VkSurfaceKHR surface, VkBool32* pSupported) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(i->next(i->handle,"vkGetPhysicalDeviceSurfaceSupportKHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, queueFamilyIndex, surface, pSupported);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceToolProperties(VkPhysicalDevice physicalDevice, uint32_t* pToolCount, VkPhysicalDeviceToolProperties* pToolProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceToolProperties>(i->next(i->handle,"vkGetPhysicalDeviceToolProperties"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pToolCount, pToolProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceToolPropertiesEXT(VkPhysicalDevice physicalDevice, uint32_t* pToolCount, VkPhysicalDeviceToolProperties* pToolProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceToolPropertiesEXT>(i->next(i->handle,"vkGetPhysicalDeviceToolPropertiesEXT"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pToolCount, pToolProperties);
}
#if defined(VK_USE_PLATFORM_UBM_SEC)
VKAPI_ATTR VkBool32 VKAPI_CALL wrap_vkGetPhysicalDeviceUbmPresentationSupportSEC(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, struct ubm_device* device) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceUbmPresentationSupportSEC>(i->next(i->handle,"vkGetPhysicalDeviceUbmPresentationSupportSEC"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, queueFamilyIndex, device);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceVideoCapabilitiesKHR(VkPhysicalDevice physicalDevice, const VkVideoProfileInfoKHR* pVideoProfile, VkVideoCapabilitiesKHR* pCapabilities) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR>(i->next(i->handle,"vkGetPhysicalDeviceVideoCapabilitiesKHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pVideoProfile, pCapabilities);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceVideoEncodeQualityLevelInfoKHR* pQualityLevelInfo, VkVideoEncodeQualityLevelPropertiesKHR* pQualityLevelProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR>(i->next(i->handle,"vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pQualityLevelInfo, pQualityLevelProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPhysicalDeviceVideoFormatPropertiesKHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceVideoFormatInfoKHR* pVideoFormatInfo, uint32_t* pVideoFormatPropertyCount, VkVideoFormatPropertiesKHR* pVideoFormatProperties) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR>(i->next(i->handle,"vkGetPhysicalDeviceVideoFormatPropertiesKHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, pVideoFormatInfo, pVideoFormatPropertyCount, pVideoFormatProperties);
}
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
VKAPI_ATTR VkBool32 VKAPI_CALL wrap_vkGetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, struct wl_display* display) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR>(i->next(i->handle,"vkGetPhysicalDeviceWaylandPresentationSupportKHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, queueFamilyIndex, display);
}
#endif
#if defined(VK_USE_PLATFORM_WIN32_KHR)
VKAPI_ATTR VkBool32 VKAPI_CALL wrap_vkGetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR>(i->next(i->handle,"vkGetPhysicalDeviceWin32PresentationSupportKHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, queueFamilyIndex);
}
#endif
#if defined(VK_USE_PLATFORM_XCB_KHR)
VKAPI_ATTR VkBool32 VKAPI_CALL wrap_vkGetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, xcb_connection_t* connection, xcb_visualid_t visual_id) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceXcbPresentationSupportKHR>(i->next(i->handle,"vkGetPhysicalDeviceXcbPresentationSupportKHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, queueFamilyIndex, connection, visual_id);
}
#endif
#if defined(VK_USE_PLATFORM_XLIB_KHR)
VKAPI_ATTR VkBool32 VKAPI_CALL wrap_vkGetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, Display* dpy, VisualID visualID) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceXlibPresentationSupportKHR>(i->next(i->handle,"vkGetPhysicalDeviceXlibPresentationSupportKHR"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, queueFamilyIndex, dpy, visualID);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPipelineBinaryDataKHR(VkDevice device, const VkPipelineBinaryDataInfoKHR* pInfo, VkPipelineBinaryKeyKHR* pPipelineBinaryKey, size_t* pPipelineBinaryDataSize, void* pPipelineBinaryData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetPipelineBinaryDataKHR;require(pfn);return pfn(device, pInfo, pPipelineBinaryKey, pPipelineBinaryDataSize, pPipelineBinaryData);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetPipelineBinaryDataKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetPipelineBinaryDataKHR");
 require(fn);
 return fn(device, pInfo, pPipelineBinaryKey, pPipelineBinaryDataSize, pPipelineBinaryData);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPipelineCacheData(VkDevice device, VkPipelineCache pipelineCache, size_t* pDataSize, void* pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetPipelineCacheData;require(pfn);return pfn(device, pipelineCache, pDataSize, pData);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetPipelineCacheData;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetPipelineCacheData");
 require(fn);
 return fn(device, pipelineCache, pDataSize, pData);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPipelineExecutableInternalRepresentationsKHR(VkDevice                        device, const VkPipelineExecutableInfoKHR*  pExecutableInfo, uint32_t* pInternalRepresentationCount, VkPipelineExecutableInternalRepresentationKHR* pInternalRepresentations) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetPipelineExecutableInternalRepresentationsKHR;require(pfn);return pfn(device, pExecutableInfo, pInternalRepresentationCount, pInternalRepresentations);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetPipelineExecutableInternalRepresentationsKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetPipelineExecutableInternalRepresentationsKHR");
 require(fn);
 return fn(device, pExecutableInfo, pInternalRepresentationCount, pInternalRepresentations);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPipelineExecutablePropertiesKHR(VkDevice                        device, const VkPipelineInfoKHR*        pPipelineInfo, uint32_t* pExecutableCount, VkPipelineExecutablePropertiesKHR* pProperties) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetPipelineExecutablePropertiesKHR;require(pfn);return pfn(device, pPipelineInfo, pExecutableCount, pProperties);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetPipelineExecutablePropertiesKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetPipelineExecutablePropertiesKHR");
 require(fn);
 return fn(device, pPipelineInfo, pExecutableCount, pProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPipelineExecutableStatisticsKHR(VkDevice                        device, const VkPipelineExecutableInfoKHR*  pExecutableInfo, uint32_t* pStatisticCount, VkPipelineExecutableStatisticKHR* pStatistics) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetPipelineExecutableStatisticsKHR;require(pfn);return pfn(device, pExecutableInfo, pStatisticCount, pStatistics);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetPipelineExecutableStatisticsKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetPipelineExecutableStatisticsKHR");
 require(fn);
 return fn(device, pExecutableInfo, pStatisticCount, pStatistics);
}
VKAPI_ATTR VkDeviceAddress VKAPI_CALL wrap_vkGetPipelineIndirectDeviceAddressNV(VkDevice device, const VkPipelineIndirectDeviceAddressInfoNV* pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetPipelineIndirectDeviceAddressNV;require(pfn);return pfn(device, pInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetPipelineIndirectDeviceAddressNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetPipelineIndirectDeviceAddressNV");
 require(fn);
 return fn(device, pInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPipelineIndirectMemoryRequirementsNV(VkDevice device, const VkComputePipelineCreateInfo* pCreateInfo, VkMemoryRequirements2* pMemoryRequirements) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetPipelineIndirectMemoryRequirementsNV;require(pfn);pfn(device, pCreateInfo, pMemoryRequirements);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetPipelineIndirectMemoryRequirementsNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetPipelineIndirectMemoryRequirementsNV");
 require(fn);
 fn(device, pCreateInfo, pMemoryRequirements);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPipelineKeyKHR(VkDevice device, const VkPipelineCreateInfoKHR* pPipelineCreateInfo, VkPipelineBinaryKeyKHR* pPipelineKey) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetPipelineKeyKHR;require(pfn);return pfn(device, pPipelineCreateInfo, pPipelineKey);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetPipelineKeyKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetPipelineKeyKHR");
 require(fn);
 return fn(device, pPipelineCreateInfo, pPipelineKey);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetPipelinePropertiesEXT(VkDevice device, const VkPipelineInfoKHR* pPipelineInfo, VkBaseOutStructure* pPipelineProperties) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetPipelinePropertiesEXT;require(pfn);return pfn(device, pPipelineInfo, pPipelineProperties);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetPipelinePropertiesEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetPipelinePropertiesEXT");
 require(fn);
 return fn(device, pPipelineInfo, pPipelineProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPrivateData(VkDevice device, VkObjectType objectType, uint64_t objectHandle, VkPrivateDataSlot privateDataSlot, uint64_t* pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetPrivateData;require(pfn);pfn(device, objectType, objectHandle, privateDataSlot, pData);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetPrivateData;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetPrivateData");
 require(fn);
 fn(device, objectType, objectHandle, privateDataSlot, pData);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetPrivateDataEXT(VkDevice device, VkObjectType objectType, uint64_t objectHandle, VkPrivateDataSlot privateDataSlot, uint64_t* pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetPrivateDataEXT;require(pfn);pfn(device, objectType, objectHandle, privateDataSlot, pData);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetPrivateDataEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetPrivateDataEXT");
 require(fn);
 fn(device, objectType, objectHandle, privateDataSlot, pData);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetQueryPoolResults(VkDevice device, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount, size_t dataSize, void* pData, VkDeviceSize stride, VkQueryResultFlags flags) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetQueryPoolResults;require(pfn);return pfn(device, queryPool, firstQuery, queryCount, dataSize, pData, stride, flags);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetQueryPoolResults;
 const auto caller86=originalCaller86(RDR_QUERY_CALLER85);
 const auto observation85=observeQueryBegin85(d,RDR_QUERY_CALLER85,queryPool,firstQuery,queryCount,dataSize,pData,stride,flags);
 const bool pending=startPendingPrelude77(d,2);VkResult result;bool continued86{};
 {OriginalHostQuery86 entry(d,"vkGetQueryPoolResults",originalObservedQueryShape86(queryPool,firstQuery,queryCount,dataSize,pData,stride,flags));
  require(fn);result=fn(device, queryPool, firstQuery, queryCount, dataSize, pData, stride, flags);continued86=entry.complete(result>=0);}
 const int savedErrno=errno;if(pending)finishPendingPrelude77(d,2,result);observeQueryEnd85(d,observation85,result);
 originalQueryCompleted86(d,caller86,queryPool,continued86,result);errno=savedErrno;
 return result;
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetQueueCheckpointData2NV(VkQueue queue, uint32_t* pCheckpointDataCount, VkCheckpointData2NV* pCheckpointData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(queue);auto pfn=p->dispatch.vkGetQueueCheckpointData2NV;require(pfn);pfn(queue, pCheckpointDataCount, pCheckpointData);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,queue); auto fn=d->dispatch.vkGetQueueCheckpointData2NV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetQueueCheckpointData2NV");
 require(fn);
 fn(queue, pCheckpointDataCount, pCheckpointData);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetQueueCheckpointDataNV(VkQueue queue, uint32_t* pCheckpointDataCount, VkCheckpointDataNV* pCheckpointData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(queue);auto pfn=p->dispatch.vkGetQueueCheckpointDataNV;require(pfn);pfn(queue, pCheckpointDataCount, pCheckpointData);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,queue); auto fn=d->dispatch.vkGetQueueCheckpointDataNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetQueueCheckpointDataNV");
 require(fn);
 fn(queue, pCheckpointDataCount, pCheckpointData);
}
#if defined(VK_USE_PLATFORM_XLIB_XRANDR_EXT)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetRandROutputDisplayEXT(VkPhysicalDevice physicalDevice, Display* dpy, RROutput rrOutput, VkDisplayKHR* pDisplay) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetRandROutputDisplayEXT>(i->next(i->handle,"vkGetRandROutputDisplayEXT"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, dpy, rrOutput, pDisplay);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR(VkDevice device, VkPipeline pipeline, uint32_t firstGroup, uint32_t groupCount, size_t dataSize, void* pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetRayTracingCaptureReplayShaderGroupHandlesKHR;require(pfn);return pfn(device, pipeline, firstGroup, groupCount, dataSize, pData);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetRayTracingCaptureReplayShaderGroupHandlesKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetRayTracingCaptureReplayShaderGroupHandlesKHR");
 require(fn);
 return fn(device, pipeline, firstGroup, groupCount, dataSize, pData);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetRayTracingShaderGroupHandlesKHR(VkDevice device, VkPipeline pipeline, uint32_t firstGroup, uint32_t groupCount, size_t dataSize, void* pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetRayTracingShaderGroupHandlesKHR;require(pfn);return pfn(device, pipeline, firstGroup, groupCount, dataSize, pData);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetRayTracingShaderGroupHandlesKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetRayTracingShaderGroupHandlesKHR");
 require(fn);
 return fn(device, pipeline, firstGroup, groupCount, dataSize, pData);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetRayTracingShaderGroupHandlesNV(VkDevice device, VkPipeline pipeline, uint32_t firstGroup, uint32_t groupCount, size_t dataSize, void* pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetRayTracingShaderGroupHandlesNV;require(pfn);return pfn(device, pipeline, firstGroup, groupCount, dataSize, pData);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetRayTracingShaderGroupHandlesNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetRayTracingShaderGroupHandlesNV");
 require(fn);
 return fn(device, pipeline, firstGroup, groupCount, dataSize, pData);
}
VKAPI_ATTR VkDeviceSize VKAPI_CALL wrap_vkGetRayTracingShaderGroupStackSizeKHR(VkDevice device, VkPipeline pipeline, uint32_t group, VkShaderGroupShaderKHR groupShader) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetRayTracingShaderGroupStackSizeKHR;require(pfn);return pfn(device, pipeline, group, groupShader);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetRayTracingShaderGroupStackSizeKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetRayTracingShaderGroupStackSizeKHR");
 require(fn);
 return fn(device, pipeline, group, groupShader);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetRefreshCycleDurationGOOGLE(VkDevice device, VkSwapchainKHR swapchain, VkRefreshCycleDurationGOOGLE* pDisplayTimingProperties) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetRefreshCycleDurationGOOGLE;require(pfn);return pfn(device, swapchain, pDisplayTimingProperties);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetRefreshCycleDurationGOOGLE;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetRefreshCycleDurationGOOGLE");
 require(fn);
 return fn(device, swapchain, pDisplayTimingProperties);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetRenderAreaGranularity(VkDevice device, VkRenderPass renderPass, VkExtent2D* pGranularity) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetRenderAreaGranularity;require(pfn);pfn(device, renderPass, pGranularity);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetRenderAreaGranularity;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetRenderAreaGranularity");
 require(fn);
 fn(device, renderPass, pGranularity);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetRenderingAreaGranularity(VkDevice device, const VkRenderingAreaInfo* pRenderingAreaInfo, VkExtent2D* pGranularity) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetRenderingAreaGranularity;require(pfn);pfn(device, pRenderingAreaInfo, pGranularity);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetRenderingAreaGranularity;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetRenderingAreaGranularity");
 require(fn);
 fn(device, pRenderingAreaInfo, pGranularity);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetRenderingAreaGranularityKHR(VkDevice device, const VkRenderingAreaInfo* pRenderingAreaInfo, VkExtent2D* pGranularity) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetRenderingAreaGranularityKHR;require(pfn);pfn(device, pRenderingAreaInfo, pGranularity);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetRenderingAreaGranularityKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetRenderingAreaGranularityKHR");
 require(fn);
 fn(device, pRenderingAreaInfo, pGranularity);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetSamplerOpaqueCaptureDescriptorDataEXT(VkDevice device, const VkSamplerCaptureDescriptorDataInfoEXT* pInfo, void* pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetSamplerOpaqueCaptureDescriptorDataEXT;require(pfn);return pfn(device, pInfo, pData);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetSamplerOpaqueCaptureDescriptorDataEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetSamplerOpaqueCaptureDescriptorDataEXT");
 require(fn);
 return fn(device, pInfo, pData);
}
#if defined(VK_USE_PLATFORM_SCREEN_QNX)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetScreenBufferPropertiesQNX(VkDevice device, const struct _screen_buffer* buffer, VkScreenBufferPropertiesQNX* pProperties) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetScreenBufferPropertiesQNX;require(pfn);return pfn(device, buffer, pProperties);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetScreenBufferPropertiesQNX;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetScreenBufferPropertiesQNX");
 require(fn);
 return fn(device, buffer, pProperties);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetSemaphoreCounterValue(VkDevice device, VkSemaphore semaphore, uint64_t* pValue) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetSemaphoreCounterValue;require(pfn);return pfn(device, semaphore, pValue);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetSemaphoreCounterValue;
 require(fn);
 return fn(device, semaphore, pValue);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetSemaphoreCounterValueKHR(VkDevice device, VkSemaphore semaphore, uint64_t* pValue) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetSemaphoreCounterValueKHR;require(pfn);return pfn(device, semaphore, pValue);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetSemaphoreCounterValueKHR;
 require(fn);
 return fn(device, semaphore, pValue);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetSemaphoreFdKHR(VkDevice device, const VkSemaphoreGetFdInfoKHR* pGetFdInfo, int* pFd) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetSemaphoreFdKHR;require(pfn);return pfn(device, pGetFdInfo, pFd);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetSemaphoreFdKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetSemaphoreFdKHR");
 require(fn);
 return fn(device, pGetFdInfo, pFd);
}
#if defined(VK_USE_PLATFORM_WIN32_KHR)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetSemaphoreWin32HandleKHR(VkDevice device, const VkSemaphoreGetWin32HandleInfoKHR* pGetWin32HandleInfo, HANDLE* pHandle) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetSemaphoreWin32HandleKHR;require(pfn);return pfn(device, pGetWin32HandleInfo, pHandle);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetSemaphoreWin32HandleKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetSemaphoreWin32HandleKHR");
 require(fn);
 return fn(device, pGetWin32HandleInfo, pHandle);
}
#endif
#if defined(VK_USE_PLATFORM_FUCHSIA)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetSemaphoreZirconHandleFUCHSIA(VkDevice device, const VkSemaphoreGetZirconHandleInfoFUCHSIA* pGetZirconHandleInfo, zx_handle_t* pZirconHandle) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetSemaphoreZirconHandleFUCHSIA;require(pfn);return pfn(device, pGetZirconHandleInfo, pZirconHandle);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetSemaphoreZirconHandleFUCHSIA;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetSemaphoreZirconHandleFUCHSIA");
 require(fn);
 return fn(device, pGetZirconHandleInfo, pZirconHandle);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetShaderBinaryDataEXT(VkDevice device, VkShaderEXT shader, size_t* pDataSize, void* pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetShaderBinaryDataEXT;require(pfn);return pfn(device, shader, pDataSize, pData);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetShaderBinaryDataEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetShaderBinaryDataEXT");
 require(fn);
 return fn(device, shader, pDataSize, pData);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetShaderInfoAMD(VkDevice device, VkPipeline pipeline, VkShaderStageFlagBits shaderStage, VkShaderInfoTypeAMD infoType, size_t* pInfoSize, void* pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetShaderInfoAMD;require(pfn);return pfn(device, pipeline, shaderStage, infoType, pInfoSize, pInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetShaderInfoAMD;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetShaderInfoAMD");
 require(fn);
 return fn(device, pipeline, shaderStage, infoType, pInfoSize, pInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetShaderInstrumentationValuesARM(VkDevice device, VkShaderInstrumentationARM instrumentation, uint32_t* pMetricBlockCount, void* pMetricValues, VkShaderInstrumentationValuesFlagsARM flags) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetShaderInstrumentationValuesARM;require(pfn);return pfn(device, instrumentation, pMetricBlockCount, pMetricValues, flags);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetShaderInstrumentationValuesARM;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetShaderInstrumentationValuesARM");
 require(fn);
 return fn(device, instrumentation, pMetricBlockCount, pMetricValues, flags);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetShaderModuleCreateInfoIdentifierEXT(VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo, VkShaderModuleIdentifierEXT* pIdentifier) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetShaderModuleCreateInfoIdentifierEXT;require(pfn);pfn(device, pCreateInfo, pIdentifier);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetShaderModuleCreateInfoIdentifierEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetShaderModuleCreateInfoIdentifierEXT");
 require(fn);
 fn(device, pCreateInfo, pIdentifier);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetShaderModuleIdentifierEXT(VkDevice device, VkShaderModule shaderModule, VkShaderModuleIdentifierEXT* pIdentifier) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetShaderModuleIdentifierEXT;require(pfn);pfn(device, shaderModule, pIdentifier);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetShaderModuleIdentifierEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetShaderModuleIdentifierEXT");
 require(fn);
 fn(device, shaderModule, pIdentifier);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetSleepStatusLegacyNV(VkDevice device, VkBool32* pLowLatencyMode) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetSleepStatusLegacyNV;require(pfn);pfn(device, pLowLatencyMode);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetSleepStatusLegacyNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetSleepStatusLegacyNV");
 require(fn);
 fn(device, pLowLatencyMode);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetSwapchainCounterEXT(VkDevice device, VkSwapchainKHR swapchain, VkSurfaceCounterFlagBitsEXT counter, uint64_t* pCounterValue) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetSwapchainCounterEXT;require(pfn);return pfn(device, swapchain, counter, pCounterValue);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetSwapchainCounterEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetSwapchainCounterEXT");
 require(fn);
 return fn(device, swapchain, counter, pCounterValue);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount, VkImage* pSwapchainImages) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetSwapchainImagesKHR;require(pfn);return pfn(device, swapchain, pSwapchainImageCount, pSwapchainImages);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetSwapchainImagesKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetSwapchainImagesKHR");
 require(fn);
 return fn(device, swapchain, pSwapchainImageCount, pSwapchainImages);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetSwapchainStatusKHR(VkDevice device, VkSwapchainKHR swapchain) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetSwapchainStatusKHR;require(pfn);return pfn(device, swapchain);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetSwapchainStatusKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetSwapchainStatusKHR");
 require(fn);
 return fn(device, swapchain);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetSwapchainTimeDomainPropertiesEXT(VkDevice device, VkSwapchainKHR swapchain, VkSwapchainTimeDomainPropertiesEXT* pSwapchainTimeDomainProperties, uint64_t* pTimeDomainsCounter) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetSwapchainTimeDomainPropertiesEXT;require(pfn);return pfn(device, swapchain, pSwapchainTimeDomainProperties, pTimeDomainsCounter);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetSwapchainTimeDomainPropertiesEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetSwapchainTimeDomainPropertiesEXT");
 require(fn);
 return fn(device, swapchain, pSwapchainTimeDomainProperties, pTimeDomainsCounter);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetSwapchainTimingPropertiesEXT(VkDevice device, VkSwapchainKHR swapchain, VkSwapchainTimingPropertiesEXT* pSwapchainTimingProperties, uint64_t* pSwapchainTimingPropertiesCounter) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetSwapchainTimingPropertiesEXT;require(pfn);return pfn(device, swapchain, pSwapchainTimingProperties, pSwapchainTimingPropertiesCounter);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetSwapchainTimingPropertiesEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetSwapchainTimingPropertiesEXT");
 require(fn);
 return fn(device, swapchain, pSwapchainTimingProperties, pSwapchainTimingPropertiesCounter);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkGetTensorMemoryRequirementsARM(VkDevice device, const VkTensorMemoryRequirementsInfoARM* pInfo, VkMemoryRequirements2* pMemoryRequirements) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetTensorMemoryRequirementsARM;require(pfn);pfn(device, pInfo, pMemoryRequirements);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetTensorMemoryRequirementsARM;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetTensorMemoryRequirementsARM");
 require(fn);
 fn(device, pInfo, pMemoryRequirements);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetTensorOpaqueCaptureDataARM(VkDevice                                            device, uint32_t                                            tensorCount, const VkTensorARM*                pTensors, VkHostAddressRangeEXT*            pDatas) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetTensorOpaqueCaptureDataARM;require(pfn);return pfn(device, tensorCount, pTensors, pDatas);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetTensorOpaqueCaptureDataARM;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetTensorOpaqueCaptureDataARM");
 require(fn);
 return fn(device, tensorCount, pTensors, pDatas);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetTensorOpaqueCaptureDescriptorDataARM(VkDevice                                    device, const VkTensorCaptureDescriptorDataInfoARM* pInfo, void*                                       pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetTensorOpaqueCaptureDescriptorDataARM;require(pfn);return pfn(device, pInfo, pData);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetTensorOpaqueCaptureDescriptorDataARM;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetTensorOpaqueCaptureDescriptorDataARM");
 require(fn);
 return fn(device, pInfo, pData);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetTensorViewOpaqueCaptureDescriptorDataARM(VkDevice                                        device, const VkTensorViewCaptureDescriptorDataInfoARM* pInfo, void*                                           pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetTensorViewOpaqueCaptureDescriptorDataARM;require(pfn);return pfn(device, pInfo, pData);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetTensorViewOpaqueCaptureDescriptorDataARM;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetTensorViewOpaqueCaptureDescriptorDataARM");
 require(fn);
 return fn(device, pInfo, pData);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetValidationCacheDataEXT(VkDevice device, VkValidationCacheEXT validationCache, size_t* pDataSize, void* pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetValidationCacheDataEXT;require(pfn);return pfn(device, validationCache, pDataSize, pData);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetValidationCacheDataEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetValidationCacheDataEXT");
 require(fn);
 return fn(device, validationCache, pDataSize, pData);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetVideoSessionMemoryRequirementsKHR(VkDevice device, VkVideoSessionKHR videoSession, uint32_t* pMemoryRequirementsCount, VkVideoSessionMemoryRequirementsKHR* pMemoryRequirements) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkGetVideoSessionMemoryRequirementsKHR;require(pfn);return pfn(device, videoSession, pMemoryRequirementsCount, pMemoryRequirements);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkGetVideoSessionMemoryRequirementsKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkGetVideoSessionMemoryRequirementsKHR");
 require(fn);
 return fn(device, videoSession, pMemoryRequirementsCount, pMemoryRequirements);
}
#if defined(VK_USE_PLATFORM_WIN32_KHR)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkGetWinrtDisplayNV(VkPhysicalDevice physicalDevice, uint32_t deviceRelativeId, VkDisplayKHR* pDisplay) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkGetWinrtDisplayNV>(i->next(i->handle,"vkGetWinrtDisplayNV"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, deviceRelativeId, pDisplay);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkImportFenceFdKHR(VkDevice device, const VkImportFenceFdInfoKHR* pImportFenceFdInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkImportFenceFdKHR;require(pfn);return pfn(device, pImportFenceFdInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkImportFenceFdKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkImportFenceFdKHR");
 require(fn);
 return fn(device, pImportFenceFdInfo);
}
#if defined(VK_USE_PLATFORM_WIN32_KHR)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkImportFenceWin32HandleKHR(VkDevice device, const VkImportFenceWin32HandleInfoKHR* pImportFenceWin32HandleInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkImportFenceWin32HandleKHR;require(pfn);return pfn(device, pImportFenceWin32HandleInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkImportFenceWin32HandleKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkImportFenceWin32HandleKHR");
 require(fn);
 return fn(device, pImportFenceWin32HandleInfo);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkImportSemaphoreFdKHR(VkDevice device, const VkImportSemaphoreFdInfoKHR* pImportSemaphoreFdInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkImportSemaphoreFdKHR;require(pfn);return pfn(device, pImportSemaphoreFdInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkImportSemaphoreFdKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkImportSemaphoreFdKHR");
 require(fn);
 return fn(device, pImportSemaphoreFdInfo);
}
#if defined(VK_USE_PLATFORM_WIN32_KHR)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkImportSemaphoreWin32HandleKHR(VkDevice device, const VkImportSemaphoreWin32HandleInfoKHR* pImportSemaphoreWin32HandleInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkImportSemaphoreWin32HandleKHR;require(pfn);return pfn(device, pImportSemaphoreWin32HandleInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkImportSemaphoreWin32HandleKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkImportSemaphoreWin32HandleKHR");
 require(fn);
 return fn(device, pImportSemaphoreWin32HandleInfo);
}
#endif
#if defined(VK_USE_PLATFORM_FUCHSIA)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkImportSemaphoreZirconHandleFUCHSIA(VkDevice device, const VkImportSemaphoreZirconHandleInfoFUCHSIA* pImportSemaphoreZirconHandleInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkImportSemaphoreZirconHandleFUCHSIA;require(pfn);return pfn(device, pImportSemaphoreZirconHandleInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkImportSemaphoreZirconHandleFUCHSIA;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkImportSemaphoreZirconHandleFUCHSIA");
 require(fn);
 return fn(device, pImportSemaphoreZirconHandleInfo);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkInitializePerformanceApiINTEL(VkDevice device, const VkInitializePerformanceApiInfoINTEL* pInitializeInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkInitializePerformanceApiINTEL;require(pfn);return pfn(device, pInitializeInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkInitializePerformanceApiINTEL;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkInitializePerformanceApiINTEL");
 require(fn);
 return fn(device, pInitializeInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkInvalidateMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange* pMemoryRanges) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkInvalidateMappedMemoryRanges;require(pfn);return pfn(device, memoryRangeCount, pMemoryRanges);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkInvalidateMappedMemoryRanges;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkInvalidateMappedMemoryRanges");
 require(fn);
 return fn(device, memoryRangeCount, pMemoryRanges);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkLatencySleepLegacyNV(VkDevice device, VkSemaphore signalSemaphore, uint64_t value) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkLatencySleepLegacyNV;require(pfn);pfn(device, signalSemaphore, value);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkLatencySleepLegacyNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkLatencySleepLegacyNV");
 require(fn);
 fn(device, signalSemaphore, value);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkLatencySleepNV(VkDevice device, VkSwapchainKHR swapchain, const VkLatencySleepInfoNV* pSleepInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkLatencySleepNV;require(pfn);return pfn(device, swapchain, pSleepInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkLatencySleepNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkLatencySleepNV");
 require(fn);
 return fn(device, swapchain, pSleepInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void** ppData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkMapMemory;require(pfn);return pfn(device, memory, offset, size, flags, ppData);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkMapMemory;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkMapMemory");
 require(fn);
 return fn(device, memory, offset, size, flags, ppData);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkMapMemory2(VkDevice device, const VkMemoryMapInfo* pMemoryMapInfo, void** ppData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkMapMemory2;require(pfn);return pfn(device, pMemoryMapInfo, ppData);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkMapMemory2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkMapMemory2");
 require(fn);
 return fn(device, pMemoryMapInfo, ppData);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkMapMemory2KHR(VkDevice device, const VkMemoryMapInfo* pMemoryMapInfo, void** ppData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkMapMemory2KHR;require(pfn);return pfn(device, pMemoryMapInfo, ppData);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkMapMemory2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkMapMemory2KHR");
 require(fn);
 return fn(device, pMemoryMapInfo, ppData);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkMergePipelineCaches(VkDevice device, VkPipelineCache dstCache, uint32_t srcCacheCount, const VkPipelineCache* pSrcCaches) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkMergePipelineCaches;require(pfn);return pfn(device, dstCache, srcCacheCount, pSrcCaches);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkMergePipelineCaches;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkMergePipelineCaches");
 require(fn);
 return fn(device, dstCache, srcCacheCount, pSrcCaches);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkMergeValidationCachesEXT(VkDevice device, VkValidationCacheEXT dstCache, uint32_t srcCacheCount, const VkValidationCacheEXT* pSrcCaches) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkMergeValidationCachesEXT;require(pfn);return pfn(device, dstCache, srcCacheCount, pSrcCaches);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkMergeValidationCachesEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkMergeValidationCachesEXT");
 require(fn);
 return fn(device, dstCache, srcCacheCount, pSrcCaches);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkQueueBeginDebugUtilsLabelEXT(VkQueue queue, const VkDebugUtilsLabelEXT* pLabelInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(queue);auto pfn=p->dispatch.vkQueueBeginDebugUtilsLabelEXT;require(pfn);pfn(queue, pLabelInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,queue); auto fn=d->dispatch.vkQueueBeginDebugUtilsLabelEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkQueueBeginDebugUtilsLabelEXT");
 require(fn);
 fn(queue, pLabelInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkQueueBindSparse(VkQueue queue, uint32_t bindInfoCount, const VkBindSparseInfo* pBindInfo, VkFence fence) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(queue);auto pfn=p->dispatch.vkQueueBindSparse;require(pfn);return pfn(queue, bindInfoCount, pBindInfo, fence);}
 ThreadContext& t=tls;auto* d=deviceFor(t,queue); auto fn=d->dispatch.vkQueueBindSparse;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkQueueBindSparse");
 require(fn);
 return fn(queue, bindInfoCount, pBindInfo, fence);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkQueueEndDebugUtilsLabelEXT(VkQueue queue) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(queue);auto pfn=p->dispatch.vkQueueEndDebugUtilsLabelEXT;require(pfn);pfn(queue);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,queue); auto fn=d->dispatch.vkQueueEndDebugUtilsLabelEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkQueueEndDebugUtilsLabelEXT");
 require(fn);
 fn(queue);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkQueueInsertDebugUtilsLabelEXT(VkQueue queue, const VkDebugUtilsLabelEXT* pLabelInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(queue);auto pfn=p->dispatch.vkQueueInsertDebugUtilsLabelEXT;require(pfn);pfn(queue, pLabelInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,queue); auto fn=d->dispatch.vkQueueInsertDebugUtilsLabelEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkQueueInsertDebugUtilsLabelEXT");
 require(fn);
 fn(queue, pLabelInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkQueueNotifyOutOfBandLegacyNV(VkQueue queue, uint32_t queueType) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(queue);auto pfn=p->dispatch.vkQueueNotifyOutOfBandLegacyNV;require(pfn);pfn(queue, queueType);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,queue); auto fn=d->dispatch.vkQueueNotifyOutOfBandLegacyNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkQueueNotifyOutOfBandLegacyNV");
 require(fn);
 fn(queue, queueType);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkQueueNotifyOutOfBandNV(VkQueue queue, const VkOutOfBandQueueTypeInfoNV* pQueueTypeInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(queue);auto pfn=p->dispatch.vkQueueNotifyOutOfBandNV;require(pfn);pfn(queue, pQueueTypeInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,queue); auto fn=d->dispatch.vkQueueNotifyOutOfBandNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkQueueNotifyOutOfBandNV");
 require(fn);
 fn(queue, pQueueTypeInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(queue);auto pfn=p->dispatch.vkQueuePresentKHR;require(pfn);return pfn(queue, pPresentInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,queue); auto fn=d->dispatch.vkQueuePresentKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkQueuePresentKHR");
 require(fn);
 return fn(queue, pPresentInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkQueueSetPerfHintQCOM(VkQueue queue, const VkPerfHintInfoQCOM*  pPerfHintInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(queue);auto pfn=p->dispatch.vkQueueSetPerfHintQCOM;require(pfn);return pfn(queue, pPerfHintInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,queue); auto fn=d->dispatch.vkQueueSetPerfHintQCOM;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkQueueSetPerfHintQCOM");
 require(fn);
 return fn(queue, pPerfHintInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkQueueSetPerformanceConfigurationINTEL(VkQueue queue, VkPerformanceConfigurationINTEL configuration) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(queue);auto pfn=p->dispatch.vkQueueSetPerformanceConfigurationINTEL;require(pfn);return pfn(queue, configuration);}
 ThreadContext& t=tls;auto* d=deviceFor(t,queue); auto fn=d->dispatch.vkQueueSetPerformanceConfigurationINTEL;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkQueueSetPerformanceConfigurationINTEL");
 require(fn);
 return fn(queue, configuration);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(queue);auto pfn=p->dispatch.vkQueueSubmit;require(pfn);return pfn(queue, submitCount, pSubmits, fence);}
 ThreadContext& t=tls;auto* d=deviceFor(t,queue); auto fn=d->dispatch.vkQueueSubmit;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkQueueSubmit");
 require(fn);
 return fn(queue, submitCount, pSubmits, fence);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkQueueSubmit2(VkQueue                          queue, uint32_t                            submitCount, const VkSubmitInfo2*              pSubmits, VkFence           fence) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(queue);auto pfn=p->dispatch.vkQueueSubmit2;require(pfn);return pfn(queue, submitCount, pSubmits, fence);}
 ThreadContext& t=tls;auto* d=deviceFor(t,queue); auto fn=d->dispatch.vkQueueSubmit2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkQueueSubmit2");
 require(fn);
 return fn(queue, submitCount, pSubmits, fence);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkQueueSubmit2KHR(VkQueue                          queue, uint32_t                            submitCount, const VkSubmitInfo2*              pSubmits, VkFence           fence) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(queue);auto pfn=p->dispatch.vkQueueSubmit2KHR;require(pfn);return pfn(queue, submitCount, pSubmits, fence);}
 ThreadContext& t=tls;auto* d=deviceFor(t,queue); auto fn=d->dispatch.vkQueueSubmit2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkQueueSubmit2KHR");
 require(fn);
 return fn(queue, submitCount, pSubmits, fence);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkQueueWaitIdle(VkQueue queue) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(queue);auto pfn=p->dispatch.vkQueueWaitIdle;require(pfn);return pfn(queue);}
 ThreadContext& t=tls;auto* d=deviceFor(t,queue); auto fn=d->dispatch.vkQueueWaitIdle;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkQueueWaitIdle");
 require(fn);
 return fn(queue);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkRegisterCustomBorderColorEXT(VkDevice                                            device, const VkSamplerCustomBorderColorCreateInfoEXT*      pBorderColor, VkBool32                                            requestIndex, uint32_t*                                           pIndex) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkRegisterCustomBorderColorEXT;require(pfn);return pfn(device, pBorderColor, requestIndex, pIndex);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkRegisterCustomBorderColorEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkRegisterCustomBorderColorEXT");
 require(fn);
 return fn(device, pBorderColor, requestIndex, pIndex);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkRegisterDeviceEventEXT(VkDevice device, const VkDeviceEventInfoEXT* pDeviceEventInfo, const VkAllocationCallbacks* pAllocator, VkFence* pFence) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkRegisterDeviceEventEXT;require(pfn);return pfn(device, pDeviceEventInfo, pAllocator, pFence);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkRegisterDeviceEventEXT;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkRegisterDeviceEventEXT");
 require(fn);
 return fn(device, pDeviceEventInfo, pAllocator, pFence);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkRegisterDisplayEventEXT(VkDevice device, VkDisplayKHR display, const VkDisplayEventInfoEXT* pDisplayEventInfo, const VkAllocationCallbacks* pAllocator, VkFence* pFence) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkRegisterDisplayEventEXT;require(pfn);return pfn(device, display, pDisplayEventInfo, pAllocator, pFence);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkRegisterDisplayEventEXT;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkRegisterDisplayEventEXT");
 require(fn);
 return fn(device, display, pDisplayEventInfo, pAllocator, pFence);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkReleaseCapturedPipelineDataKHR(VkDevice device, const VkReleaseCapturedPipelineDataInfoKHR* pInfo, const VkAllocationCallbacks* pAllocator) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkReleaseCapturedPipelineDataKHR;require(pfn);return pfn(device, pInfo, pAllocator);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkReleaseCapturedPipelineDataKHR;
 if (!allocatorSafe(pAllocator)) revoke(d,Decline::Allocator);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkReleaseCapturedPipelineDataKHR");
 require(fn);
 return fn(device, pInfo, pAllocator);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkReleaseDisplayEXT(VkPhysicalDevice physicalDevice, VkDisplayKHR display) {
 auto* i=instanceFor(physicalDevice); auto fn=reinterpret_cast<PFN_vkReleaseDisplayEXT>(i->next(i->handle,"vkReleaseDisplayEXT"));
 InstanceEntries entries(i);
 require(fn);
 return fn(physicalDevice, display);
}
#if defined(VK_USE_PLATFORM_WIN32_KHR)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkReleaseFullScreenExclusiveModeEXT(VkDevice device, VkSwapchainKHR swapchain) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkReleaseFullScreenExclusiveModeEXT;require(pfn);return pfn(device, swapchain);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkReleaseFullScreenExclusiveModeEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkReleaseFullScreenExclusiveModeEXT");
 require(fn);
 return fn(device, swapchain);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkReleasePerformanceConfigurationINTEL(VkDevice device, VkPerformanceConfigurationINTEL configuration) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkReleasePerformanceConfigurationINTEL;require(pfn);return pfn(device, configuration);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkReleasePerformanceConfigurationINTEL;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkReleasePerformanceConfigurationINTEL");
 require(fn);
 return fn(device, configuration);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkReleaseProfilingLockKHR(VkDevice device) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkReleaseProfilingLockKHR;require(pfn);pfn(device);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkReleaseProfilingLockKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkReleaseProfilingLockKHR");
 require(fn);
 fn(device);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkReleaseSwapchainImagesEXT(VkDevice device, const VkReleaseSwapchainImagesInfoKHR* pReleaseInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkReleaseSwapchainImagesEXT;require(pfn);return pfn(device, pReleaseInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkReleaseSwapchainImagesEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkReleaseSwapchainImagesEXT");
 require(fn);
 return fn(device, pReleaseInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkReleaseSwapchainImagesKHR(VkDevice device, const VkReleaseSwapchainImagesInfoKHR* pReleaseInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkReleaseSwapchainImagesKHR;require(pfn);return pfn(device, pReleaseInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkReleaseSwapchainImagesKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkReleaseSwapchainImagesKHR");
 require(fn);
 return fn(device, pReleaseInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkResetDescriptorPool(VkDevice device, VkDescriptorPool descriptorPool, VkDescriptorPoolResetFlags flags) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkResetDescriptorPool;require(pfn);return pfn(device, descriptorPool, flags);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkResetDescriptorPool;
 PoolOperation87 poolOperation87(d,descriptorPool);const bool pending=startPendingPrelude77(d,1);VkResult result;
 {auto entry=enter(t,d,Worker::AccessKind::Command,"vkResetDescriptorPool");require(fn);const auto ticket83=snapshotDescriptorPoolReset83(d,descriptorPool,flags);result=fn(device, descriptorPool, flags);const int saved=errno;if(result==VK_SUCCESS){retirePoolObservation82(d,descriptorPool,true);completedDescriptorPoolReset83(d,descriptorPool,ticket83);}errno=saved;}
 const int savedErrno=errno;if(pending)finishPendingPrelude77(d,1,result);errno=savedErrno;
 return result;
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkResetEvent(VkDevice device, VkEvent event) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkResetEvent;require(pfn);return pfn(device, event);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkResetEvent;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkResetEvent");
 require(fn);
 return fn(device, event);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkResetFences(VkDevice device, uint32_t fenceCount, const VkFence* pFences) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkResetFences;require(pfn);return pfn(device, fenceCount, pFences);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkResetFences;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkResetFences");
 require(fn);
 return fn(device, fenceCount, pFences);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkResetGpaSessionAMD(VkDevice        device, VkGpaSessionAMD gpaSession) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkResetGpaSessionAMD;require(pfn);return pfn(device, gpaSession);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkResetGpaSessionAMD;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkResetGpaSessionAMD");
 require(fn);
 return fn(device, gpaSession);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkResetQueryPool(VkDevice device, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkResetQueryPool;require(pfn);pfn(device, queryPool, firstQuery, queryCount);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkResetQueryPool;
 const auto observation85=observeResetBegin85(d,RDR_QUERY_CALLER85,queryPool,firstQuery,queryCount,1);
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkResetQueryPool");
 require(fn);
 fn(device, queryPool, firstQuery, queryCount);
 observeResetEnd85(d,observation85);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkResetQueryPoolEXT(VkDevice device, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkResetQueryPoolEXT;require(pfn);pfn(device, queryPool, firstQuery, queryCount);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkResetQueryPoolEXT;
 const auto caller86=originalCaller86(RDR_QUERY_CALLER85);
 const auto observation85=observeResetBegin85(d,RDR_QUERY_CALLER85,queryPool,firstQuery,queryCount,2);
 const bool pending=startPendingPrelude77(d,3);
 {OriginalHostQuery86 entry(d,"vkResetQueryPoolEXT",originalResetSequence86(d,caller86,queryPool,firstQuery,queryCount));require(fn);fn(device, queryPool, firstQuery, queryCount);}
 const int savedErrno=errno;if(pending)finishPendingHostQueryReset78(d,queryPool,firstQuery,queryCount);observeResetEnd85(d,observation85);errno=savedErrno;
}
#if defined(VK_USE_PLATFORM_FUCHSIA)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkSetBufferCollectionBufferConstraintsFUCHSIA(VkDevice device, VkBufferCollectionFUCHSIA collection, const VkBufferConstraintsInfoFUCHSIA* pBufferConstraintsInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkSetBufferCollectionBufferConstraintsFUCHSIA;require(pfn);return pfn(device, collection, pBufferConstraintsInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkSetBufferCollectionBufferConstraintsFUCHSIA;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkSetBufferCollectionBufferConstraintsFUCHSIA");
 require(fn);
 return fn(device, collection, pBufferConstraintsInfo);
}
#endif
#if defined(VK_USE_PLATFORM_FUCHSIA)
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkSetBufferCollectionImageConstraintsFUCHSIA(VkDevice device, VkBufferCollectionFUCHSIA collection, const VkImageConstraintsInfoFUCHSIA* pImageConstraintsInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkSetBufferCollectionImageConstraintsFUCHSIA;require(pfn);return pfn(device, collection, pImageConstraintsInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkSetBufferCollectionImageConstraintsFUCHSIA;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkSetBufferCollectionImageConstraintsFUCHSIA");
 require(fn);
 return fn(device, collection, pImageConstraintsInfo);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkSetDebugUtilsObjectNameEXT(VkDevice device, const VkDebugUtilsObjectNameInfoEXT* pNameInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkSetDebugUtilsObjectNameEXT;require(pfn);return pfn(device, pNameInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkSetDebugUtilsObjectNameEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkSetDebugUtilsObjectNameEXT");
 require(fn);
 return fn(device, pNameInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkSetDebugUtilsObjectTagEXT(VkDevice device, const VkDebugUtilsObjectTagInfoEXT* pTagInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkSetDebugUtilsObjectTagEXT;require(pfn);return pfn(device, pTagInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkSetDebugUtilsObjectTagEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkSetDebugUtilsObjectTagEXT");
 require(fn);
 return fn(device, pTagInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkSetDeviceMemoryPriorityEXT(VkDevice       device, VkDeviceMemory memory, float          priority) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkSetDeviceMemoryPriorityEXT;require(pfn);pfn(device, memory, priority);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkSetDeviceMemoryPriorityEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkSetDeviceMemoryPriorityEXT");
 require(fn);
 fn(device, memory, priority);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkSetEvent(VkDevice device, VkEvent event) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkSetEvent;require(pfn);return pfn(device, event);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkSetEvent;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkSetEvent");
 require(fn);
 return fn(device, event);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkSetGpaDeviceClockModeAMD(VkDevice                     device, VkGpaDeviceClockModeInfoAMD* pInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkSetGpaDeviceClockModeAMD;require(pfn);return pfn(device, pInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkSetGpaDeviceClockModeAMD;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkSetGpaDeviceClockModeAMD");
 require(fn);
 return fn(device, pInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkSetHdrMetadataEXT(VkDevice device, uint32_t swapchainCount, const VkSwapchainKHR* pSwapchains, const VkHdrMetadataEXT* pMetadata) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkSetHdrMetadataEXT;require(pfn);pfn(device, swapchainCount, pSwapchains, pMetadata);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkSetHdrMetadataEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkSetHdrMetadataEXT");
 require(fn);
 fn(device, swapchainCount, pSwapchains, pMetadata);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkSetLatencyMarkerLegacyNV(VkDevice device, uint64_t frameID, uint32_t marker) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkSetLatencyMarkerLegacyNV;require(pfn);pfn(device, frameID, marker);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkSetLatencyMarkerLegacyNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkSetLatencyMarkerLegacyNV");
 require(fn);
 fn(device, frameID, marker);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkSetLatencyMarkerNV(VkDevice device, VkSwapchainKHR swapchain, const VkSetLatencyMarkerInfoNV* pLatencyMarkerInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkSetLatencyMarkerNV;require(pfn);pfn(device, swapchain, pLatencyMarkerInfo);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkSetLatencyMarkerNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkSetLatencyMarkerNV");
 require(fn);
 fn(device, swapchain, pLatencyMarkerInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkSetLatencySleepModeLegacyNV(VkDevice device, VkBool32 lowLatencyMode, VkBool32 lowLatencyBoost, uint32_t minimumIntervalUs) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkSetLatencySleepModeLegacyNV;require(pfn);pfn(device, lowLatencyMode, lowLatencyBoost, minimumIntervalUs);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkSetLatencySleepModeLegacyNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkSetLatencySleepModeLegacyNV");
 require(fn);
 fn(device, lowLatencyMode, lowLatencyBoost, minimumIntervalUs);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkSetLatencySleepModeNV(VkDevice device, VkSwapchainKHR swapchain, const VkLatencySleepModeInfoNV* pSleepModeInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkSetLatencySleepModeNV;require(pfn);return pfn(device, swapchain, pSleepModeInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkSetLatencySleepModeNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkSetLatencySleepModeNV");
 require(fn);
 return fn(device, swapchain, pSleepModeInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkSetLocalDimmingAMD(VkDevice device, VkSwapchainKHR swapChain, VkBool32 localDimmingEnable) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkSetLocalDimmingAMD;require(pfn);pfn(device, swapChain, localDimmingEnable);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkSetLocalDimmingAMD;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkSetLocalDimmingAMD");
 require(fn);
 fn(device, swapChain, localDimmingEnable);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkSetPrivateData(VkDevice device, VkObjectType objectType, uint64_t objectHandle, VkPrivateDataSlot privateDataSlot, uint64_t data) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkSetPrivateData;require(pfn);return pfn(device, objectType, objectHandle, privateDataSlot, data);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkSetPrivateData;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkSetPrivateData");
 require(fn);
 return fn(device, objectType, objectHandle, privateDataSlot, data);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkSetPrivateDataEXT(VkDevice device, VkObjectType objectType, uint64_t objectHandle, VkPrivateDataSlot privateDataSlot, uint64_t data) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkSetPrivateDataEXT;require(pfn);return pfn(device, objectType, objectHandle, privateDataSlot, data);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkSetPrivateDataEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkSetPrivateDataEXT");
 require(fn);
 return fn(device, objectType, objectHandle, privateDataSlot, data);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkSetSwapchainPresentTimingQueueSizeEXT(VkDevice device, VkSwapchainKHR swapchain, uint32_t size) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkSetSwapchainPresentTimingQueueSizeEXT;require(pfn);return pfn(device, swapchain, size);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkSetSwapchainPresentTimingQueueSizeEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkSetSwapchainPresentTimingQueueSizeEXT");
 require(fn);
 return fn(device, swapchain, size);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkShutdownLatencyDeviceLegacyNV(VkDevice device) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkShutdownLatencyDeviceLegacyNV;require(pfn);pfn(device);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkShutdownLatencyDeviceLegacyNV;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkShutdownLatencyDeviceLegacyNV");
 require(fn);
 fn(device);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkSignalSemaphore(VkDevice device, const VkSemaphoreSignalInfo* pSignalInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkSignalSemaphore;require(pfn);return pfn(device, pSignalInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkSignalSemaphore;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkSignalSemaphore");
 require(fn);
 return fn(device, pSignalInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkSignalSemaphoreKHR(VkDevice device, const VkSemaphoreSignalInfo* pSignalInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkSignalSemaphoreKHR;require(pfn);return pfn(device, pSignalInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkSignalSemaphoreKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkSignalSemaphoreKHR");
 require(fn);
 return fn(device, pSignalInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkSubmitDebugUtilsMessageEXT(VkInstance instance, VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageTypes, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData) {
 auto* i=instanceFor(instance); auto fn=reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(i->next(i->handle,"vkSubmitDebugUtilsMessageEXT"));
 InstanceEntries entries(i);
 require(fn);
 fn(instance, messageSeverity, messageTypes, pCallbackData);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkTransitionImageLayout(VkDevice device, uint32_t transitionCount, const VkHostImageLayoutTransitionInfo*    pTransitions) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkTransitionImageLayout;require(pfn);return pfn(device, transitionCount, pTransitions);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkTransitionImageLayout;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkTransitionImageLayout");
 require(fn);
 return fn(device, transitionCount, pTransitions);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkTransitionImageLayoutEXT(VkDevice device, uint32_t transitionCount, const VkHostImageLayoutTransitionInfo*    pTransitions) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkTransitionImageLayoutEXT;require(pfn);return pfn(device, transitionCount, pTransitions);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkTransitionImageLayoutEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkTransitionImageLayoutEXT");
 require(fn);
 return fn(device, transitionCount, pTransitions);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkTrimCommandPool(VkDevice device, VkCommandPool commandPool, VkCommandPoolTrimFlags flags) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkTrimCommandPool;require(pfn);pfn(device, commandPool, flags);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkTrimCommandPool;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkTrimCommandPool");
 require(fn);
 fn(device, commandPool, flags);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkTrimCommandPoolKHR(VkDevice device, VkCommandPool commandPool, VkCommandPoolTrimFlags flags) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkTrimCommandPoolKHR;require(pfn);pfn(device, commandPool, flags);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkTrimCommandPoolKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkTrimCommandPoolKHR");
 require(fn);
 fn(device, commandPool, flags);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkUninitializePerformanceApiINTEL(VkDevice device) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkUninitializePerformanceApiINTEL;require(pfn);pfn(device);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkUninitializePerformanceApiINTEL;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkUninitializePerformanceApiINTEL");
 require(fn);
 fn(device);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkUnmapMemory;require(pfn);pfn(device, memory);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkUnmapMemory;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkUnmapMemory");
 require(fn);
 fn(device, memory);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkUnmapMemory2(VkDevice device, const VkMemoryUnmapInfo* pMemoryUnmapInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkUnmapMemory2;require(pfn);return pfn(device, pMemoryUnmapInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkUnmapMemory2;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkUnmapMemory2");
 require(fn);
 return fn(device, pMemoryUnmapInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkUnmapMemory2KHR(VkDevice device, const VkMemoryUnmapInfo* pMemoryUnmapInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkUnmapMemory2KHR;require(pfn);return pfn(device, pMemoryUnmapInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkUnmapMemory2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkUnmapMemory2KHR");
 require(fn);
 return fn(device, pMemoryUnmapInfo);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkUnregisterCustomBorderColorEXT(VkDevice                                            device, uint32_t                                            index) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkUnregisterCustomBorderColorEXT;require(pfn);pfn(device, index);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkUnregisterCustomBorderColorEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkUnregisterCustomBorderColorEXT");
 require(fn);
 fn(device, index);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkUpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet descriptorSet, VkDescriptorUpdateTemplate descriptorUpdateTemplate, const void* pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkUpdateDescriptorSetWithTemplate;require(pfn);pfn(device, descriptorSet, descriptorUpdateTemplate, pData);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkUpdateDescriptorSetWithTemplate;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkUpdateDescriptorSetWithTemplate");
 require(fn);
 fn(device, descriptorSet, descriptorUpdateTemplate, pData);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkUpdateDescriptorSetWithTemplateKHR(VkDevice device, VkDescriptorSet descriptorSet, VkDescriptorUpdateTemplate descriptorUpdateTemplate, const void* pData) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkUpdateDescriptorSetWithTemplateKHR;require(pfn);pfn(device, descriptorSet, descriptorUpdateTemplate, pData);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkUpdateDescriptorSetWithTemplateKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkUpdateDescriptorSetWithTemplateKHR");
 require(fn);
 fn(device, descriptorSet, descriptorUpdateTemplate, pData);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkUpdateDescriptorSets(VkDevice device,uint32_t count,const VkWriteDescriptorSet*w,uint32_t copies,const VkCopyDescriptorSet*c){
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkUpdateDescriptorSets;require(pfn);pfn(device, count, w, copies, c);return;}
 auto*d=deviceFor(device);auto fn=d->dispatch.vkUpdateDescriptorSets;require(fn);const int incoming=errno;unsigned slot;
 const bool shape=initialDescriptorUpdate81(d,count,w,copies,c,slot);errno=incoming;
 if(!shape){auto entry=enter(d,Worker::AccessKind::Resource,"vkUpdateDescriptorSets");errno=incoming;fn(device,count,w,copies,c);return;}
 const auto receipt=tls.fresh81[slot];ActivePreparation81 entry(d,"vkUpdateDescriptorSets");errno=incoming;
 fn(device,count,w,copies,c);const int saved=errno;completedDescriptorUpdate81(d,receipt,entry.complete());errno=saved;
}

VKAPI_ATTR void VKAPI_CALL wrap_vkUpdateIndirectExecutionSetPipelineEXT(VkDevice device, VkIndirectExecutionSetEXT indirectExecutionSet, uint32_t executionSetWriteCount, const VkWriteIndirectExecutionSetPipelineEXT* pExecutionSetWrites) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkUpdateIndirectExecutionSetPipelineEXT;require(pfn);pfn(device, indirectExecutionSet, executionSetWriteCount, pExecutionSetWrites);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkUpdateIndirectExecutionSetPipelineEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkUpdateIndirectExecutionSetPipelineEXT");
 require(fn);
 fn(device, indirectExecutionSet, executionSetWriteCount, pExecutionSetWrites);
}
VKAPI_ATTR void VKAPI_CALL wrap_vkUpdateIndirectExecutionSetShaderEXT(VkDevice device, VkIndirectExecutionSetEXT indirectExecutionSet, uint32_t executionSetWriteCount, const VkWriteIndirectExecutionSetShaderEXT* pExecutionSetWrites) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkUpdateIndirectExecutionSetShaderEXT;require(pfn);pfn(device, indirectExecutionSet, executionSetWriteCount, pExecutionSetWrites);return;}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkUpdateIndirectExecutionSetShaderEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkUpdateIndirectExecutionSetShaderEXT");
 require(fn);
 fn(device, indirectExecutionSet, executionSetWriteCount, pExecutionSetWrites);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkUpdateVideoSessionParametersKHR(VkDevice device, VkVideoSessionParametersKHR videoSessionParameters, const VkVideoSessionParametersUpdateInfoKHR* pUpdateInfo) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkUpdateVideoSessionParametersKHR;require(pfn);return pfn(device, videoSessionParameters, pUpdateInfo);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkUpdateVideoSessionParametersKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkUpdateVideoSessionParametersKHR");
 require(fn);
 return fn(device, videoSessionParameters, pUpdateInfo);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkWaitForFences(VkDevice device, uint32_t fenceCount, const VkFence* pFences, VkBool32 waitAll, uint64_t timeout) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkWaitForFences;require(pfn);return pfn(device, fenceCount, pFences, waitAll, timeout);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkWaitForFences;
 require(fn);
 return fn(device, fenceCount, pFences, waitAll, timeout);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkWaitForPresent2KHR(VkDevice device, VkSwapchainKHR swapchain, const VkPresentWait2InfoKHR* pPresentWait2Info) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkWaitForPresent2KHR;require(pfn);return pfn(device, swapchain, pPresentWait2Info);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkWaitForPresent2KHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkWaitForPresent2KHR");
 require(fn);
 return fn(device, swapchain, pPresentWait2Info);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkWaitForPresentKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t presentId, uint64_t timeout) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkWaitForPresentKHR;require(pfn);return pfn(device, swapchain, presentId, timeout);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkWaitForPresentKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkWaitForPresentKHR");
 require(fn);
 return fn(device, swapchain, presentId, timeout);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkWaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo* pWaitInfo, uint64_t timeout) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkWaitSemaphores;require(pfn);return pfn(device, pWaitInfo, timeout);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkWaitSemaphores;
 require(fn);
 return fn(device, pWaitInfo, timeout);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkWaitSemaphoresKHR(VkDevice device, const VkSemaphoreWaitInfo* pWaitInfo, uint64_t timeout) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkWaitSemaphoresKHR;require(pfn);return pfn(device, pWaitInfo, timeout);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkWaitSemaphoresKHR;
 require(fn);
 return fn(device, pWaitInfo, timeout);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkWriteAccelerationStructuresPropertiesKHR(VkDevice device, uint32_t accelerationStructureCount, const VkAccelerationStructureKHR* pAccelerationStructures, VkQueryType  queryType, size_t       dataSize, void* pData, size_t stride) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkWriteAccelerationStructuresPropertiesKHR;require(pfn);return pfn(device, accelerationStructureCount, pAccelerationStructures, queryType, dataSize, pData, stride);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkWriteAccelerationStructuresPropertiesKHR;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkWriteAccelerationStructuresPropertiesKHR");
 require(fn);
 return fn(device, accelerationStructureCount, pAccelerationStructures, queryType, dataSize, pData, stride);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkWriteMicromapsPropertiesEXT(VkDevice                                 device, uint32_t                                 micromapCount, const VkMicromapEXT* pMicromaps, VkQueryType                              queryType, size_t                                   dataSize, void*                     pData, size_t                                   stride) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkWriteMicromapsPropertiesEXT;require(pfn);return pfn(device, micromapCount, pMicromaps, queryType, dataSize, pData, stride);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkWriteMicromapsPropertiesEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkWriteMicromapsPropertiesEXT");
 require(fn);
 return fn(device, micromapCount, pMicromaps, queryType, dataSize, pData, stride);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkWriteResourceDescriptorsEXT(VkDevice                                                device, uint32_t                                                resourceCount, const VkResourceDescriptorInfoEXT*  pResources, const VkHostAddressRangeEXT*        pDescriptors) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkWriteResourceDescriptorsEXT;require(pfn);return pfn(device, resourceCount, pResources, pDescriptors);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkWriteResourceDescriptorsEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkWriteResourceDescriptorsEXT");
 require(fn);
 return fn(device, resourceCount, pResources, pDescriptors);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkWriteSamplerDescriptorsEXT(VkDevice                                            device, uint32_t                                            samplerCount, const VkSamplerCreateInfo*       pSamplers, const VkHostAddressRangeEXT*     pDescriptors) {
 if(passthroughActive90()){auto* p=passthroughDeviceFor90(device);auto pfn=p->dispatch.vkWriteSamplerDescriptorsEXT;require(pfn);return pfn(device, samplerCount, pSamplers, pDescriptors);}
 ThreadContext& t=tls;auto* d=deviceFor(t,device); auto fn=d->dispatch.vkWriteSamplerDescriptorsEXT;
 auto entry=enter(t,d,Worker::AccessKind::Command,"vkWriteSamplerDescriptorsEXT");
 require(fn);
 return fn(device, samplerCount, pSamplers, pDescriptors);
}
}
