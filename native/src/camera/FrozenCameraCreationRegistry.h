#pragma once
#include "FrozenCameraPipeline.h"
#include <array>
#include <memory>

namespace rdrvr::engine {
// PRIVATE cold observer/wrapper component. No dispatch-slot installation.
// Api must be ORIGINAL driver PFNs, never these wrappers. Exact readable native
// parameters and Vulkan external synchronization are caller prerequisites.
class FrozenCameraCreationRegistry final {
public:
 using Api=FrozenCameraPipelineFactory::Api;
 using Limits=FrozenCameraPipelineFactory::Limits;
 using Variant=FrozenCameraPipelineFactory::Variant;
 // Copied cold metadata, coherent with one observed native pipeline incarnation.
 // No borrowed native pointers and no callback into a render-thread adapter.
 struct DrawMetadata {
  Variant variant{};
  std::uint64_t nativeLayoutGeneration{};
  std::uint32_t setCount{},requiredNativeSets{};
  std::array<std::uint32_t,7> dynamicCounts{};
 };
 static constexpr unsigned ReplayMaxSets=7,ReplayMaxBindings=32,ReplayMaxPushes=2,ReplayMaxDynamics=10;
 struct ReplayBinding {
  std::uint32_t binding{},count{};VkDescriptorType type{};VkShaderStageFlags stages{};VkDescriptorBindingFlags flags{};
 };
 struct ReplaySet {
  VkDescriptorSetLayout native{};std::uint64_t generation{};
  VkDescriptorSetLayoutCreateFlags flags{};bool hasBindingFlags{};std::uint32_t count{};
  std::array<ReplayBinding,ReplayMaxBindings> bindings{};
 };
 struct ReplayMetadata {
  DrawMetadata draw{};
  VkPipelineCreateFlags flags{};VkShaderStageFlags stages{};
  VkRenderPass renderPass{};std::uint32_t subpass{},vertexBindingMask{},dynamicCount{};
  std::array<VkDynamicState,ReplayMaxDynamics> dynamics{};
  std::uint32_t setCount{},pushCount{};
  std::array<ReplaySet,ReplayMaxSets> sets{};
  std::array<VkPushConstantRange,ReplayMaxPushes> pushes{};
 };
 struct PlainDepthDescription {
  DrawMetadata draw{};VkRenderPass renderPass{};std::uint32_t subpass{};
  VkPrimitiveTopology topology{};VkCompareOp depthCompare{};VkSampleCountFlagBits samples{};
  VkCullModeFlags cull{};VkFrontFace frontFace{};
  std::uint32_t colorAttachmentCount{},vertexBindingMask{},dynamicCount{},requiredOwnedSets{};
  std::array<VkDynamicState,ReplayMaxDynamics> dynamics{};
  std::array<std::uint32_t,ReplayMaxSets> requiredOwnedBindings{};
  std::array<std::uint64_t,2> shaderGenerations{}; // Canonical [VS, FS], FS0 when absent.
  bool fragmentPresent{};VkDevice device{}; // Actual registry device, not a generation-only cross-registry identity.
 };
 // Immutable PSO shape evidence, not memory ownership or permission to skip.
 // This registry/factory must outlive every certificate use.
 class PlainDepthCertificate final {
  friend class FrozenCameraCreationRegistry;
  const FrozenCameraCreationRegistry* owner_{};VkPipeline native_{};
  std::uint64_t variant_{},device_{},layout_{};
 public:
  PlainDepthCertificate()=default;
  bool current()const noexcept;
  bool resolve(PlainDepthDescription&)const noexcept;
 };
 bool lookupPlainDepthCertificate(VkPipeline,PlainDepthCertificate&)const noexcept;
 static constexpr unsigned MaxShaders=4096,MaxSets=1024,MaxLayouts=1024,MaxPipelines=4096,MaxBatch=64;
 static constexpr std::size_t MaxShaderBytes=64u*1024u*1024u;
 struct Stats {std::uint64_t shaderRecords{},setRecords{},layoutRecords{},variants{},declines{},invalidations{};std::size_t shaderBytes{};bool ready{};};
 FrozenCameraCreationRegistry() noexcept;
 ~FrozenCameraCreationRegistry(); // no implicit device-idle assumption
 FrozenCameraCreationRegistry(const FrozenCameraCreationRegistry&)=delete;
 FrozenCameraCreationRegistry& operator=(const FrozenCameraCreationRegistry&)=delete;
 // Publish this immutable raw table BEFORE installing any wrapper. Without
 // originals no component can preserve a native call. Binding is one-shot;
 // an already bound, identical table is accepted, a different table declines.
 bool bindOriginals(Api) noexcept;
 bool initialize(VkDevice,std::uint64_t deviceGeneration,Api,Limits,
                 VkDescriptorSetLayout cameraLayout,std::uint64_t cameraGeneration) noexcept;

 // Actual original-first entry functions, callable from exact wrappers.
 // Before-tickets are internal and opaque. No post-only guessed-incarnation API.
 VkResult createShader(VkDevice,const VkShaderModuleCreateInfo*,const VkAllocationCallbacks*,VkShaderModule*) noexcept;
 VkResult createSetLayout(VkDevice,const VkDescriptorSetLayoutCreateInfo*,const VkAllocationCallbacks*,VkDescriptorSetLayout*) noexcept;
 VkResult createPipelineLayout(VkDevice,const VkPipelineLayoutCreateInfo*,const VkAllocationCallbacks*,VkPipelineLayout*) noexcept;
 VkResult createPipelines(VkDevice,VkPipelineCache,std::uint32_t,const VkGraphicsPipelineCreateInfo*,const VkAllocationCallbacks*,VkPipeline*) noexcept;
 // Invalidation occurs BEFORE original destruction. Original is called once,
 // outside the registry lock; a clone already in creation finishes first.
 void destroyShader(VkDevice,VkShaderModule,const VkAllocationCallbacks*) noexcept;
 void destroySetLayout(VkDevice,VkDescriptorSetLayout,const VkAllocationCallbacks*) noexcept;
 void destroyPipelineLayout(VkDevice,VkPipelineLayout,const VkAllocationCallbacks*) noexcept;
 void destroyPipeline(VkDevice,VkPipeline,const VkAllocationCallbacks*) noexcept;
 bool lookup(VkPipeline,Variant&) const noexcept;
 // Survives native layout and DSL destruction: variant.restoreLayout owns the
 // exact original-compatible restoration layout until idle. The generation
 // remains the pipeline's captured incarnation, never the newest numeric-handle
 // incarnation. Failure leaves output unchanged. This copy is not a GPU lease;
 // the registry/factory must outlive all recording and executable command uses.
 bool lookupDrawMetadata(VkPipeline,DrawMetadata&) const noexcept;
 // One coherent copied tuple from ACTUAL successful creation wrappers. The
 // graphics subset is sampled before/after original creation; DSL/push records
 // are retained immutable copies. No native pointers, worker callback or fake
 // layout event. Unsupported subsets decline only this export. Binding flags
 // PARTIALLY_BOUND are preserved exactly, never converted to complete bindings.
 // Out/errno unchanged on failure. This is metadata, not admission or a lease.
 bool lookupReplayMetadata(VkPipeline,ReplayMetadata&) const noexcept;
 // Identity of a NEW native descriptor-bind call, distinct from historical
 // pipeline metadata. Check before/after the native call; dead/unknown refuses.
 bool lookupLayoutGeneration(VkPipelineLayout,std::uint64_t&) const noexcept;
 Stats stats() const noexcept;
 // Stop all wrappers/users first. Factory clones die before retained compatible
 // set-layout clones. Camera layout/device outlive this explicit shutdown.
 void shutdownAfterIdle(VkDevice,VkResult) noexcept;
 void abandon(VkDevice) noexcept;
private:
 bool resolvePlainDepth(VkPipeline,std::uint64_t,std::uint64_t,std::uint64_t,PlainDepthDescription*)const noexcept;
 struct Impl;std::unique_ptr<Impl> impl_;
};
}
