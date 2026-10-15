#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace rdrvr::engine {
// PRIVATE isolated creation-time transform, not a Vulkan or draw permit.
// Requires a semantically valid native SPIR-V 1.3 VS. Unsupported input leaves
// output/report unchanged, including when source aliases output.
enum class CameraSplitStatus {
    Converted, InvalidStructure, UnsupportedInterface, UnsupportedAccess,
    LimitExceeded, UnsupportedPosition, AllocationFailed
};
struct FrozenCameraUniforms {
    alignas(16) std::array<float,16> relativeClip{};
    std::array<float,16> inverseView{};
};
static_assert(sizeof(FrozenCameraUniforms)==128 && alignof(FrozenCameraUniforms)==16);
static_assert(offsetof(FrozenCameraUniforms,relativeClip)==0 && offsetof(FrozenCameraUniforms,inverseView)==64);
struct CameraSplitReport {
    std::uint32_t nativeVariable{},cameraVariable{},nativeBlock{},cameraBlock{};
    std::uint32_t relativeAccesses{},inverseAccesses{},worldAccesses{};
    std::uint32_t descriptorSet{3},binding{0},uniformBytes{128};
    // Position equation is proved in the original native ABI. Transformed camera
    // descriptors need independent owned allocation/bind/pipeline attestation.
    bool originalRelativePositionProved{};
    bool operator==(const CameraSplitReport&)const=default;
};
CameraSplitStatus splitFrozenCamera(std::span<const std::uint32_t> source,
    std::vector<std::uint32_t>& output,CameraSplitReport& report) noexcept;
// Fragment-only structural rewrite. Requires valid SPIR-V 1.3 and the exact
// native set0/binding1 block; only direct member1/member3 reads are supported.
// Plain fragments without that block are declined for the caller's plain path.
// Converted always publishes a set3/binding0 declaration and redirects every
// accepted access. It never proves a vertex position equation: the report's
// originalRelativePositionProved remains false. Any failure preserves outputs.
CameraSplitStatus splitFrozenFragmentCamera(std::span<const std::uint32_t> source,
    std::vector<std::uint32_t>& output,CameraSplitReport& report) noexcept;
}
