#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vulkan/vulkan.h>

namespace rdrvr::engine {
// Creation-time proof only. A certificate never proves live buffer contents,
// object bounds, the current pass, or dynamic viewport/scissor state.
class StaticOcclusionCertificate {
public:
    enum class PositionModel : unsigned { Unknown,DirectMvpXYZ1,RelativeAffineXYZ1 };
    enum class PipelineReason : unsigned { None,CreateInfo,ShaderStages,UnknownPosition,VertexEffects,Extension,VertexInput,PositionAttribute,VertexBinding,FixedState,DepthClip,DynamicState,Viewport,Scissor,FragmentEffects,Count };
    static constexpr const char* reasonName(PipelineReason r) noexcept {
        constexpr const char* names[]{"none","createInfo","shaderStages","unknownPosition","vertexEffects","extension","vertexInput","positionAttribute","vertexBinding","fixedState","depthClip","dynamicState","viewport","scissor","fragmentEffects"};
        return unsigned(r)<unsigned(PipelineReason::Count)?names[unsigned(r)]:"invalid";
    }
    enum Dynamic : unsigned { Viewport=1,Scissor=2 };
    using Digest=std::array<std::uint8_t,32>;
    struct Shader {
        Digest digest{};
        PositionModel position{PositionModel::Unknown};
        unsigned stage{},positionLocation{},matrixSet{},matrixBinding{1};
        bool sideEffectFree{},fragmentSafe{},fragmentOpaque{},vertexCoverage{},structuralPosition{};
        std::array<char,128> entry{};
    };
    struct Pipeline {
        PositionModel position{PositionModel::Unknown};
        bool positionKnown{},candidateSafe{},opaqueDepth{};
        unsigned binding{},stride{},offset{},requiredDynamic{};
        VkFormat format{VK_FORMAT_UNDEFINED};
        VkCullModeFlags cull{};VkFrontFace frontFace{};
        VkViewport viewport{};VkRect2D scissor{};
        PipelineReason reason{PipelineReason::CreateInfo};
    };
    static Digest fingerprint(std::span<const std::uint8_t>) noexcept;
    static Shader shader(const VkShaderModuleCreateInfo*) noexcept;
    static Pipeline pipeline(const VkGraphicsPipelineCreateInfo&,const Shader& vertex,const Shader& fragment) noexcept;
};
}
