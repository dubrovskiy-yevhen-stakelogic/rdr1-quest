// PRIVATE actual TraceEntry composition. Included after original PFN fields.
// These exact OBJECT8 dispatch RVAs are in saved librdr-dynsym.txt (no new scan).
constexpr std::uintptr_t cameraCreateLayoutSlot=0x4e693f80;
constexpr std::uintptr_t cameraDestroyLayoutSlot=0x4e693f88;
constexpr std::uintptr_t cameraBindPipelineSlot=0x4e694048;
constexpr std::uintptr_t cameraDrawSlot=0x4e6940b0;
constexpr std::uintptr_t cameraDrawIndexedSlot=0x4e6940b8;
PFN_vkCreatePipelineLayout originalCameraCreateLayout{};
PFN_vkDestroyPipelineLayout originalCameraDestroyLayout{};
PFN_vkCmdBindPipeline originalCameraBindPipeline{};
PFN_vkCmdDraw originalCameraDraw{};
PFN_vkCmdDrawIndexed originalCameraDrawIndexed{};

bool bindCameraRuntimeOriginals(PFN_vkQueueSubmit queue) noexcept {
    rdr95::NativeCameraRuntime::Dispatch raw;
    raw.creation={originalCreateShader,originalColdDestroyShader,originalDynamicCreateLayout,originalDynamicDestroyLayout,
                  originalCameraCreateLayout,originalCameraDestroyLayout,originalRateCreatePipelines,originalColdDestroyPipeline};
    // Loader exports bypass the native engine's dispatch variables. These cold
    // owner allocations must never re-enter descriptor conversion/observers.
    raw.resources={::vkGetPhysicalDeviceProperties,::vkGetPhysicalDeviceMemoryProperties,
        ::vkCreateBuffer,::vkGetBufferMemoryRequirements,::vkAllocateMemory,::vkBindBufferMemory,
        ::vkMapMemory,::vkFlushMappedMemoryRanges,::vkUnmapMemory,::vkDestroyBuffer,::vkFreeMemory,
        ::vkCreateDescriptorPool,::vkDestroyDescriptorPool,::vkAllocateDescriptorSets,::vkUpdateDescriptorSets,
        originalDynamicCreateLayout,originalDynamicDestroyLayout};
    raw.bindPipeline=originalCameraBindPipeline;raw.bindDescriptors=originalDynamicBind;
    raw.draw=originalCameraDraw;raw.drawIndexed=originalCameraDrawIndexed;
    raw.allocateCommands=originalAllocateCommands;raw.freeCommands=originalFreeCommands;
    raw.beginCommand=originalBeginCommand;raw.endCommand=originalEndCommand;
    raw.resetCommand=originalResetCommand;raw.resetPool=originalResetPool;raw.destroyPool=originalDestroyPool;
    raw.executeCommands=originalDynamicExecute;raw.submit=queue;
    return cameraRuntime.bindOriginals(raw);
}

void initializeCameraRuntime(VkPhysicalDevice physical,VkDevice device,std::uint64_t generation) noexcept {
    if(!cameraRoutesInstalled.load(std::memory_order_acquire)||
       !cameraGameDeviceRoute.load(std::memory_order_acquire))return;
    const auto applicationVersion=gameCameraApplicationVersion(physical,device);
    if(!applicationVersion)return;
    VkDevice empty{};
    if(!cameraOwnerDevice.compare_exchange_strong(empty,device,std::memory_order_acq_rel))return;
    VkPhysicalDeviceProperties properties{};::vkGetPhysicalDeviceProperties(physical,&properties);
    // The exact successful game bridge binding supplied the requested API.
    // Physical 1.2+ alone does not promote that application's API version.
    // This reads supported limits only; it enables no device features.
    const auto limits=rdr95::CameraDeviceLimits::capture(physical,properties,applicationVersion,
        {::vkEnumerateDeviceExtensionProperties,::vkGetPhysicalDeviceProperties2});
    if(cameraRuntime.initializeDevice(physical,device,generation,limits,cameraOwnsRecordingThread))
        initializeUniformResources(physical,device);
}

// Captured original PFNs are immutable before any wrapper slot is published.
bool bindIndexedObservationOriginals()noexcept {
    rdr95::NativeIndexedObservation::Mirror::Api api;
    api.pipeline=originalCameraBindPipeline;api.descriptors=originalDynamicBind;
    api.vertices=indexedRaw_vkCmdBindVertexBuffers;api.indices=indexedRaw_vkCmdBindIndexBuffer;
    api.viewport=indexedRaw_vkCmdSetViewport;api.scissor=indexedRaw_vkCmdSetScissor;
    api.lineWidth=indexedRaw_vkCmdSetLineWidth;api.depthBias=indexedRaw_vkCmdSetDepthBias;
    api.blendConstants=indexedRaw_vkCmdSetBlendConstants;api.depthBounds=indexedRaw_vkCmdSetDepthBounds;
    api.stencilCompare=indexedRaw_vkCmdSetStencilCompareMask;api.stencilWrite=indexedRaw_vkCmdSetStencilWriteMask;
    api.stencilReference=indexedRaw_vkCmdSetStencilReference;api.depthBoundsEnable=indexedDepthEnableRaw;
    api.push=indexedRaw_vkCmdPushConstants;api.draw=originalCameraDrawIndexed;
    return indexedObservation.bindOriginals(api);
}
