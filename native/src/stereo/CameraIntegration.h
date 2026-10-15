#pragma once
#include "NativeCameraRuntime.h"
#include "NativeVertexIntegration58.h"
#include "NativeIndexedObservation.h"
#include "OrdinaryPacketCapture.h"
#include "CameraDeviceLimits.h"
#include "CameraViewportSource.h"
#include "SceneRouteTape.h"
#include "OrdinaryMainStartupRequest.h"
#include "StereoReplay.h"
#include "TargetRouteGuard.h" // Drop the draws of a failed mod-routed stage (no render pass open)
#include "EyeCullTape.h"      // EYE CULL B: the left eye skips taped draws of objects outside the left eye
#include <atomic>

namespace rdrvr::engine {
// Cold exact game-owner query, under the GameXr adapter's mutex.
// Zero means unknown/flat/auxiliary; never infer API version from GPU version.
std::uint32_t gameCameraApplicationVersion(VkPhysicalDevice,VkDevice) noexcept;
// PRIVATE shared composition. Only the cold dispatch transaction may publish
// routesInstalled after all original function pointers have been bound.
inline rdr95::NativeCameraRuntime cameraRuntime;
inline rdr95::NativeIndexedObservation indexedObservation(cameraRuntime);
inline std::atomic<bool> cameraRoutesInstalled{};
// Camera children exist only behind the GameXr device teardown, which stops
// its users and obtains an actual idle result before releasing the mod's resources.
inline std::atomic<bool> cameraGameDeviceRoute{};
inline std::atomic<VkDevice> cameraOwnerDevice{};
inline std::atomic<rdr95::NativeCameraRuntime::RecordingOwnerThread> cameraRecordingOwner{};
inline bool cameraOwnsRecordingThread(){
    const auto predicate=cameraRecordingOwner.load(std::memory_order_acquire);
    return predicate&&predicate();
}
// Terminal callbacks for the shader capture and dynamic-UBO conversion helpers.
// Those helpers must call these AFTER their own argument conversion.
inline VKAPI_ATTR VkResult VKAPI_CALL cameraCreateShader(VkDevice d,const VkShaderModuleCreateInfo* c,const VkAllocationCallbacks* a,VkShaderModule* out){return cameraRuntime.createShader(d,c,a,out);}
inline VKAPI_ATTR VkResult VKAPI_CALL cameraCreateSetLayout(VkDevice d,const VkDescriptorSetLayoutCreateInfo* c,const VkAllocationCallbacks* a,VkDescriptorSetLayout* out){return cameraRuntime.createSetLayout(d,c,a,out);}
inline VKAPI_ATTR void VKAPI_CALL cameraDestroySetLayout(VkDevice d,VkDescriptorSetLayout h,const VkAllocationCallbacks* a){cameraRuntime.destroySetLayout(d,h,a);}
inline VKAPI_ATTR void VKAPI_CALL cameraBindDescriptors(VkCommandBuffer c,VkPipelineBindPoint p,VkPipelineLayout l,std::uint32_t first,std::uint32_t count,const VkDescriptorSet* sets,std::uint32_t n,const std::uint32_t* offsets){
    stereo::hookBindDescriptorSets(c,p,l,first,count,sets,n,offsets);
    if constexpr(!ordinaryMainStartupRequested()){cameraRuntime.bindDescriptors(c,p,l,first,count,sets,n,offsets);return;}
    const auto stamp=indexedObservation.beginCall(c);
    std::array<VkDescriptorSet,7> savedSets;std::array<std::uint32_t,224> savedOffsets;
    const bool captured=stamp&&count<=savedSets.size()&&n<=savedOffsets.size()&&(!count||sets)&&(!n||offsets);
    if(captured){if(count)std::copy_n(sets,count,savedSets.data());if(n)std::copy_n(offsets,n,savedOffsets.data());}
    const auto generation=stamp?indexedObservation.layoutBefore(c,l):0;
    cameraRuntime.bindDescriptors(c,p,l,first,count,sets,n,offsets);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){
        if(captured)indexedObservation.descriptors(c,p,l,generation,first,count,savedSets.data(),n,savedOffsets.data());
        else indexedObservation.unknownState(c);
    }
    errno=saved;
}
inline VKAPI_ATTR VkResult VKAPI_CALL cameraCreatePipelineLayout(VkDevice d,const VkPipelineLayoutCreateInfo* c,const VkAllocationCallbacks* a,VkPipelineLayout* out){
    const VkPipelineLayoutCreateInfo* patched{};
    const auto r=cameraRuntime.createPipelineLayout(d,stereo::patchPipelineLayoutCreate(c,&patched)?patched:c,a,out);const int saved=errno;
    stereo::pipelineLayoutCreated(c,r==VK_SUCCESS&&out?*out:VK_NULL_HANDLE,r);errno=saved;return r;
}
inline VKAPI_ATTR void VKAPI_CALL cameraDestroyPipelineLayout(VkDevice d,VkPipelineLayout l,const VkAllocationCallbacks* a){cameraRuntime.destroyPipelineLayout(d,l,a);}
inline VKAPI_ATTR void VKAPI_CALL cameraBindPipeline(VkCommandBuffer c,VkPipelineBindPoint p,VkPipeline pipeline){stereo::hookBindPipeline(c,p,pipeline);if constexpr(!ordinaryMainStartupRequested()){cameraRuntime.bindPipeline(c,p,pipeline);return;}const auto stamp=indexedObservation.beginCall(c);cameraRuntime.bindPipeline(c,p,pipeline);const int saved=errno;if(indexedObservation.finishCall(stamp))indexedObservation.pipeline(c,p,pipeline);errno=saved;}
// targetroute::discard is the first statement of every draw/clear hook. A failed mod-routed stage may have closed the eye pass
// (no render pass in the engine primary): its draws are dropped (the stage is cancelled), before any probe/tape/observation.
// EYE CULL B (EyeCullTape.h): a draw of an object wholly outside the LEFT eye is still recorded into the stereo tape (with its
// pad) but not drawn live; leftAfterDraw still counts it (the STEREO TEST 12 split point follows the tape).
inline VKAPI_ATTR void VKAPI_CALL cameraDraw(VkCommandBuffer c,std::uint32_t n,std::uint32_t instances,std::uint32_t first,std::uint32_t base){if(targetroute::discard(targetroute::Discard::Draw))return;stereo::probeDraw(c);
    auto* t=stereo::recordingFor(c);const std::uint16_t eyePad=t?eyecull::tape::padFor(instances):0;const bool taped=t&&stereo::recordDraw(*t,c,n,instances,first,base,eyePad);
    rdr95::scene_routes::draw(false,reinterpret_cast<std::uintptr_t>(c),n,instances,first,0,base,rdr95::CameraMainRoute::selectedMain());indexedObservation.sideEffect(c);
    if(!eyecull::tape::dropLeft(eyePad,taped))cameraRuntime.draw(c,n,instances,first,base);
    stereo::leftAfterDraw(c);} // STEREO TEST 12: split the left pass after this draw (one relaxed load when off)
inline VKAPI_ATTR void VKAPI_CALL cameraDrawIndexed(VkCommandBuffer c,std::uint32_t n,std::uint32_t instances,std::uint32_t first,std::int32_t vertex,std::uint32_t base){if(targetroute::discard(targetroute::Discard::DrawIndexed))return;stereo::probeDraw(c);
    auto* t=stereo::recordingFor(c);const std::uint16_t eyePad=t?eyecull::tape::padFor(instances):0;const bool taped=t&&stereo::recordDrawIndexed(*t,c,n,instances,first,vertex,base,eyePad);
    if constexpr(!ordinaryMainStartupRequested()){rdr95::scene_routes::draw(true,reinterpret_cast<std::uintptr_t>(c),n,instances,first,vertex,base,rdr95::CameraMainRoute::selectedMain());
        if(!eyecull::tape::dropLeft(eyePad,taped))cameraRuntime.drawIndexed(c,n,instances,first,vertex,base);
        stereo::leftAfterDraw(c);return;}
    (void)taped; // The ordinary-capture path below always draws live (its packet capture observes the live draw)
    const auto stamp=indexedObservation.beginCall(c);
    rdr95::scene_routes::draw(true,reinterpret_cast<std::uintptr_t>(c),n,instances,first,vertex,base,rdr95::CameraMainRoute::selectedMain());
    cameraRuntime.drawIndexed(c,n,instances,first,vertex,base);stereo::leftAfterDraw(c);const int saved=errno;
    if(stamp){
        if(indexedObservation.finishCall(stamp))rdr95::OrdinaryPacketCapture::indexed(c,{n,instances,first,vertex,base});
        else rdr95::OrdinaryPacketCapture::invalidateCurrent();
    }else if(indexedObservation.enabled())rdr95::OrdinaryPacketCapture::invalidateCurrent();
    errno=saved;
}
inline VKAPI_ATTR VkResult VKAPI_CALL cameraSubmit(VkQueue q,std::uint32_t n,const VkSubmitInfo* s,VkFence f){return cameraRuntime.submit(q,n,s,f);}
} // namespace rdrvr::engine
