#pragma once
#include "CameraMainRoute.h"
#include "FrozenCameraCreationRegistry.h"
#include <memory>

namespace rdr95 {
// PRIVATE concrete forwarding composition. No hook installer. Complete native
// routing and Vulkan external synchronization remain caller prerequisites.
class NativeCameraRuntime final {
public:
 using Registry=rdrvr::engine::FrozenCameraCreationRegistry;
 using Limits=Registry::Limits;
 static constexpr unsigned MaxCommands=CameraUniformOwner::MaxCommands;
 struct Dispatch {
  Registry::Api creation{};                 // Original driver PFNs only.
  CameraUniformOwner::Api resources{};      // Original driver PFNs only.
  PFN_vkCmdBindPipeline bindPipeline{};
  PFN_vkCmdBindDescriptorSets bindDescriptors{};
  PFN_vkCmdDraw draw{};
  PFN_vkCmdDrawIndexed drawIndexed{};
  PFN_vkAllocateCommandBuffers allocateCommands{};
  PFN_vkFreeCommandBuffers freeCommands{};
  PFN_vkBeginCommandBuffer beginCommand{};
  PFN_vkEndCommandBuffer endCommand{};
  PFN_vkResetCommandBuffer resetCommand{};
  PFN_vkResetCommandPool resetPool{};
  PFN_vkDestroyCommandPool destroyPool{};
  PFN_vkCmdExecuteCommands executeCommands{};
  PFN_vkQueueSubmit submit{};
  explicit operator bool()const noexcept;
 };
 struct Stats {
  unsigned allocations{},importedCommands{};
  std::uint64_t allocationSerial{},declines{},foreignCalls{};
  bool deviceReady{},recordingOwnerChosen{};
 };
 NativeCameraRuntime()noexcept;
 ~NativeCameraRuntime(); // No implicit Vulkan idle or resource destruction.
 NativeCameraRuntime(const NativeCameraRuntime&)=delete;
 NativeCameraRuntime&operator=(const NativeCameraRuntime&)=delete;
 // Immutable backing must be bound before any wrapper is published. Failures
 // after this point preserve raw native forwarding; no owner-thread requirement.
 bool bindOriginals(Dispatch)noexcept;
 // Call after actual successful device creation, with that device's enabled
 // features/limits. Does NOT choose the recording thread from UI/device-create.
 // Pinned native render-thread predicate; called outside table locks after a
 // successful Begin. Missing/false/throwing refuses adoption, not native work.
 using RecordingOwnerThread=bool(*)();
 bool initializeDevice(VkPhysicalDevice,VkDevice,std::uint64_t,Limits,RecordingOwnerThread)noexcept;

 VkResult createShader(VkDevice,const VkShaderModuleCreateInfo*,const VkAllocationCallbacks*,VkShaderModule*)noexcept;
 VkResult createSetLayout(VkDevice,const VkDescriptorSetLayoutCreateInfo*,const VkAllocationCallbacks*,VkDescriptorSetLayout*)noexcept;
 VkResult createPipelineLayout(VkDevice,const VkPipelineLayoutCreateInfo*,const VkAllocationCallbacks*,VkPipelineLayout*)noexcept;
 VkResult createPipelines(VkDevice,VkPipelineCache,std::uint32_t,const VkGraphicsPipelineCreateInfo*,const VkAllocationCallbacks*,VkPipeline*)noexcept;
 void destroyShader(VkDevice,VkShaderModule,const VkAllocationCallbacks*)noexcept;
 void destroySetLayout(VkDevice,VkDescriptorSetLayout,const VkAllocationCallbacks*)noexcept;
 void destroyPipelineLayout(VkDevice,VkPipelineLayout,const VkAllocationCallbacks*)noexcept;
 void destroyPipeline(VkDevice,VkPipeline,const VkAllocationCallbacks*)noexcept;

 VkResult allocateCommands(VkDevice,const VkCommandBufferAllocateInfo*,VkCommandBuffer*)noexcept;
 void freeCommands(VkDevice,VkCommandPool,std::uint32_t,const VkCommandBuffer*)noexcept;
 // First successful qualified primary Begin on recordingOwnerThread selects
 // the adapter owner and imports the exact current allocation incarnation.
 VkResult beginCommand(VkCommandBuffer,const VkCommandBufferBeginInfo*)noexcept;
 VkResult endCommand(VkCommandBuffer)noexcept;
 VkResult resetCommand(VkCommandBuffer,VkCommandBufferResetFlags)noexcept;
 VkResult resetPool(VkDevice,VkCommandPool,VkCommandPoolResetFlags)noexcept;
 void destroyPool(VkDevice,VkCommandPool,const VkAllocationCallbacks*)noexcept;
 VkResult submit(VkQueue,std::uint32_t,const VkSubmitInfo*,VkFence)noexcept;
 void executeCommands(VkCommandBuffer,std::uint32_t,const VkCommandBuffer*)noexcept;
 // Invoke before other unmodelled native graphics commands. Foreign-thread
 // activity is forwarded raw and marks capture unknown, never calls/disarms
 // the owner-only adapter. Owner receives all actual command lifecycle events.
 void commandUnknown(VkCommandBuffer)noexcept;
 void bindPipeline(VkCommandBuffer,VkPipelineBindPoint,VkPipeline)noexcept;
 // Final emitted native descriptor bind AFTER dynamic-offset translation.
 void bindDescriptors(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,
                      std::uint32_t,std::uint32_t,const VkDescriptorSet*,
                      std::uint32_t,const std::uint32_t*)noexcept;
 using OwnedBindResult=CameraDrawAdapter::OwnedBindResult;
 // Coherent copied creation metadata, not command admission or a resource lease.
 // Failure leaves output unchanged. Native layout may already be destroyed.
 bool lookupDrawMetadata(VkPipeline,Registry::DrawMetadata&)noexcept;
 // Read-only observation seams. No raw command, recording adoption or camera
 // permission is issued. The caller must already be the selected owner.
 struct RecordingIdentity {
  VkDevice device{};VkCommandPool pool{};VkCommandBuffer command{};
  std::uint64_t deviceGeneration{},allocation{},recording{};
 };
 bool lookupRecording(VkCommandBuffer,RecordingIdentity&)noexcept;
 bool lookupReplayMetadata(VkPipeline,Registry::ReplayMetadata&)noexcept;
 bool lookupPlainDepthCertificate(VkPipeline,Registry::PlainDepthCertificate&)noexcept;
 bool lookupLayoutGeneration(VkPipelineLayout,std::uint64_t&)noexcept;
 std::uint64_t metadataRevision()const noexcept;
 // Explicit packet binding. No raw call on Declined; BoundUntracked means the
 // caller must not interpret the operation as unexecuted or silently replay it.
 OwnedBindResult bindOwnedDescriptors(VkCommandBuffer,VkPipeline intendedNative,
                      VkPipelineLayout ownedRestore,std::uint32_t,std::uint32_t,
                      const VkDescriptorSet*,std::uint32_t,const std::uint32_t*)noexcept;
 using PacketTuple=CameraDrawAdapter::PacketTuple;
 using IndexedPacketPlan=CameraDrawAdapter::IndexedPacketPlan;
 using PacketResult=CameraDrawAdapter::PacketResult;
 // Synchronous noncopy plan, all parts and incoming state validated before emit.
 // Once emit is entered, failure is terminal EmittedFaulted, never Declined.
 // The exact initial command ticket is sampled once under its table lock.
 // A combined active-writer/epoch stamp revokes subsequent lexical checks.
 // This is not external Vulkan synchronization or a GPU-resource lease.
 PacketResult withIndexedPacketPlan(VkCommandBuffer,std::span<const PacketTuple>,const PacketTuple& incoming,
                      void* context,bool(*emit)(void*,IndexedPacketPlan&)noexcept)noexcept;
 bool draw(VkCommandBuffer,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t)noexcept;
 bool drawIndexed(VkCommandBuffer,std::uint32_t,std::uint32_t,std::uint32_t,std::int32_t,std::uint32_t)noexcept;
 // Stable owner for the actual lexical CameraMainRoute::Pass. draw methods
 // issue/consume MAIN qualification internally; no transferable Request API.
 CameraUniformOwner& uniforms()noexcept;
 bool ready()const noexcept;
 Stats stats()const noexcept;
 // Atomic admission-only suspension for dispatch reinitialization. Retains all
 // GPU objects/raw originals. This one-shot instance never rearms on a guessed
 // same-device dispatch state; a fresh initialized composition is required.
 void disarm()noexcept;
 // Stop wrappers/recording/submissions first. Registry destroys both owned
 // layouts/pipelines before Owner destroys the camera layout. No lock held
 // across original native calls or draws; no implicit wait/submit is performed.
 bool shutdownAfterIdle(VkDevice,VkResult)noexcept;
 void abandonDevice(VkDevice)noexcept;
private:
 friend class OwnedIndexedReplay;
 // Only the compound owned emitter can defer restoration until Mirror commit.
 PacketResult withIndexedSequencePlan(VkCommandBuffer,std::span<const PacketTuple>,const PacketTuple&,
                      void*,bool(*)(void*,IndexedPacketPlan&)noexcept,bool(*)(void*)noexcept)noexcept;
 PacketResult withIndexedPlanImpl(VkCommandBuffer,std::span<const PacketTuple>,const PacketTuple&,
                      void*,bool(*)(void*,IndexedPacketPlan&)noexcept,bool sequence,bool(*)(void*)noexcept)noexcept;
 CameraUniformOwner uniforms_;
 struct Impl;std::unique_ptr<Impl> impl_;
};
}
