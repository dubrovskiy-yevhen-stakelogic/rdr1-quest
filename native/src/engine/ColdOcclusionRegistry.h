#pragma once
#include "StaticOcclusionCertificate.h"
#include <cstdint>
#include <memory>

namespace rdrvr::engine {
// Cold creation/destruction only. Query reads immutable publication slots and
// takes no lock, reference count, allocation, native-memory read or clock.
// Native draw witnesses still prove current target/raster/buffers/matrices.
class ColdOcclusionRegistry {
public:
    static constexpr unsigned tableCapacity=8192,shaderCapacity=8192,recordCapacity=32768,maxProbe=32;
    struct Snapshot {
        std::uint64_t deviceEpoch{},pipelineGeneration{},vertexShaderGeneration{},fragmentShaderGeneration{};
        VkPipeline pipeline{};
        StaticOcclusionCertificate::Pipeline certificate{};
    };
    // Standalone live-module proof, independent of any historical PSO. Owned
    // value, not a lifetime lease: caller must obey native module external sync.
    struct ShaderSnapshot {
        std::uint64_t deviceEpoch{},incarnation{},shaderGeneration{};
        std::uintptr_t registryIdentity{};
        VkDevice device{};VkShaderModule module{};
        StaticOcclusionCertificate::Shader certificate{};
    };
    // Subset of Statistics memory payload; do not add the two totals.
    struct ShaderStatistics {std::uint64_t live{},retained{},declined{},reservedBytes{},maximumBytes{};};
    struct Statistics {
        // Payload accounting excludes implementation-specific map nodes and
        // allocator headers. Their count is also bounded by shaderCapacity.
        std::uint64_t deviceEpoch{},shaders{},livePipelines{},retainedRecords{},declined{},
            shaderInspections{},unknownPipelines{},reservedBytes{},maximumBytes{};
        std::array<std::uint64_t,unsigned(StaticOcclusionCertificate::PipelineReason::Count)> reasons{};
    };
    ColdOcclusionRegistry();
    ~ColdOcclusionRegistry();
    ColdOcclusionRegistry(const ColdOcclusionRegistry&)=delete;
    ColdOcclusionRegistry& operator=(const ColdOcclusionRegistry&)=delete;
    bool bind(VkDevice,std::uint64_t deviceEpoch) noexcept;
    void clear(VkDevice) noexcept;
    void createdShader(VkDevice,const VkShaderModuleCreateInfo*,VkShaderModule,VkResult) noexcept;
    void destroyedShader(VkDevice,VkShaderModule) noexcept;
    // Vulkan may return some valid handles on an aggregate creation failure.
    // Every non-null returned handle is inventoried, even when result!=SUCCESS.
    void createdPipelines(VkDevice,std::uint32_t,const VkGraphicsPipelineCreateInfo*,const VkPipeline*,VkResult) noexcept;
    void destroyedPipeline(VkDevice,VkPipeline) noexcept;
    bool query(VkDevice,VkPipeline,Snapshot&) const noexcept;
    Statistics statistics() const noexcept;
    bool queryShader(VkDevice,VkShaderModule,ShaderSnapshot&) const noexcept;
    // Identity freshness only. Use an unmodified query result; registry must
    // outlive all queries and freshness checks on retained snapshot values.
    bool currentShader(const ShaderSnapshot&) const noexcept;
    // O(1) revocation ticket for a complete cold-certified module set. Zero
    // means unbound, mutation in progress or exhausted serial space. Bracket
    // cold graph inspection with this ticket and currentShaderSet(); do not
    // acquire a new ticket for an old graph after invalidation. Pipeline-only
    // changes do not alter shader identity and do not revoke this ticket.
    // This is not a lifetime lease: native owner/external synchronization is
    // still required across use, including material and geometry mutations.
    std::uint64_t shaderMutationSerial() const noexcept;
    bool currentShaderSet(std::uint64_t serial) const noexcept;
    ShaderStatistics shaderStatistics() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
inline ColdOcclusionRegistry coldOcclusionRegistry;
}
