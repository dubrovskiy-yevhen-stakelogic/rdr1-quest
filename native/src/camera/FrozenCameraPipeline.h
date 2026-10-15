#pragma once
#include "FrozenCameraSplit.h"
#include <vulkan/vulkan.h>
#include <array>
#include <memory>
#include <span>
#include <string_view>

namespace rdrvr::engine {
// Synchronous creation from the actual, successfully created native pipeline.
// Every borrowed description/module-word pair must be its exact creation input.
// This class does not infer those inputs from handles or install draw hooks.
class FrozenCameraPipelineFactory final {
public:
    struct Api {
        PFN_vkCreateShaderModule createShader{}; PFN_vkDestroyShaderModule destroyShader{};
        PFN_vkCreateDescriptorSetLayout createSetLayout{}; PFN_vkDestroyDescriptorSetLayout destroySetLayout{};
        PFN_vkCreatePipelineLayout createLayout{}; PFN_vkDestroyPipelineLayout destroyLayout{};
        PFN_vkCreateGraphicsPipelines createPipelines{}; PFN_vkDestroyPipeline destroyPipeline{};
        explicit operator bool() const noexcept;
    };
    struct Limits {
        std::uint32_t maxBoundSets{},maxVertexUniforms{},maxVertexResources{},maxSetUniforms{};
        bool depthClipEnable{},extendedDepthBounds{};
        // Required in addition to ordinary limits if ANY observed layout uses
        // UPDATE_AFTER_BIND_POOL. Zero conservatively declines such a pipeline.
        std::uint32_t maxVertexUpdateAfterBindUniforms{},maxVertexUpdateAfterBindResources{},maxSetUpdateAfterBindUniforms{};
        // This isolated factory requires an owner DSL visible to BOTH VS and FS.
        // Its one camera UBO counts against fragment budgets even for a plain
        // native FS or a fragment-free depth pipeline. Zero declines admission.
        std::uint32_t maxFragmentUniforms{},maxFragmentResources{};
        std::uint32_t maxFragmentUpdateAfterBindUniforms{},maxFragmentUpdateAfterBindResources{};
    };
    struct StageWords {VkShaderModule module{};std::span<const std::uint32_t> words;};
    struct Input {
        VkPipeline nativePipeline{};
        const VkGraphicsPipelineCreateInfo* pipeline{};
        const VkPipelineLayoutCreateInfo* layout{};
        // Exactly one description for each pSetLayouts element, even empty sets.
        std::span<const VkDescriptorSetLayoutCreateInfo> setLayouts;
        std::span<const StageWords> stages;
        // Optional live owned clones with EXACT same definitions as setLayouts.
        // The creation observer owns/proves these copies; original native DSL
        // handles may legally have been destroyed after pipeline-layout creation.
        // Empty span keeps the original live-handle contract unchanged.
        std::span<const VkDescriptorSetLayout> compatibleSetLayouts;
    };
    struct Variant {
        VkPipeline nativePipeline{},pipeline{}; VkPipelineLayout layout{};
        VkDescriptorSetLayout cameraLayout{};
        std::uint64_t deviceGeneration{},cameraLayoutGeneration{},serial{};
        VkPipelineLayout nativeLayout{};
        std::uint32_t nativeSetCount{},requiredNativeSets{};
        // Owned clone of the ORIGINAL pipeline layout: native set definitions
        // and push ranges, without camera set3 insertion/replacement. Retained
        // until idle, so native restoration never dereferences nativeLayout.
        VkPipelineLayout restoreLayout{};
        // Binding camera set3 can disturb native sets4..6 through prefix-layout
        // incompatibility. A draw adapter MUST rebind the exact required suffix
        // with this layout; preserving its create description alone is not enough.
        explicit operator bool() const noexcept {return pipeline&&layout&&restoreLayout&&serial;}
    };
    FrozenCameraPipelineFactory();
    ~FrozenCameraPipelineFactory();
    FrozenCameraPipelineFactory(const FrozenCameraPipelineFactory&)=delete;
    FrozenCameraPipelineFactory& operator=(const FrozenCameraPipelineFactory&)=delete;
    // cameraLayout must have set3/binding0 UNIFORM_BUFFER[1], VS|FS visibility,
    // and the exact 128-byte split ABI. Its description is caller-attested.
    // cameraLayout is borrowed from the immutable camera owner; it and device
    // must outlive this factory's explicit shutdown. No implicit GPU destroys.
    bool initialize(VkDevice,std::uint64_t deviceGeneration,Api,Limits,
                    VkDescriptorSetLayout cameraLayout,std::uint64_t cameraGeneration) noexcept;
    bool create(const Input&,Variant&) noexcept;
    bool lookup(VkPipeline native,Variant&) const noexcept;
    // Invalidate future selection. Retain GPU objects for executable resubmits.
    void nativeDestroyed(VkPipeline) noexcept;
    // Caller stopped all users/submissions and proved device idle or lost.
    void shutdownAfterIdle(VkDevice,VkResult) noexcept;
    // Immediately before device destruction when idle could not be proved.
    void abandon(VkDevice) noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};

// Bounded declaration check on an already semantically valid native SPIR-V1.3
// module. Not a replacement for a SPIR-V validator or a GPU lifetime witness.
bool frozenCameraStageInterface(std::span<const std::uint32_t>,VkShaderStageFlagBits,
                                unsigned nativeSetCount,std::string_view expectedEntry) noexcept;
}
