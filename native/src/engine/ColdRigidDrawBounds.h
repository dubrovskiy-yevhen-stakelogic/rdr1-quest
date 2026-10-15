#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace rdrvr::engine {
// PRIVATE CPU-only complete-LOD bound compiler. Keys are obligations for a
// real native owner, not self-certifying pointers or previous-frame evidence.
class ColdRigidDrawBounds {
public:
    static constexpr unsigned MaxModels=256,MaxParts=8192;
    static constexpr std::uint64_t MaxIndices=8u*1024u*1024u;
    struct Key {std::uint64_t identity{},generation{};bool operator==(const Key&)const=default;};
    struct Asset {
        Key drawable{},dependencies{};std::uint32_t lod{};
        bool operator==(const Asset&)const=default;
    };
    struct XYZ {float x{},y{},z{};};
    struct Bounds {std::array<double,3> minimum{},maximum{};};
    enum class Position {Unknown,RigidXYZ1};
    struct Model {
        std::uint32_t ordinal{},paletteIndex{},partCount{};
        Position position{};
    };
    struct Geometry {
        std::uint32_t model{},part{};
        Key geometry{},positionCertificate{};
        // Entire currently selected native geometry index range, with native
        // baseVertex already resolved into these OWNED XYZ positions. A patch
        // subset or incomplete mesh is not legal input. No native pointers.
        std::span<const XYZ> positions{};
        std::span<const std::uint32_t> indices{};
    };
    struct PreboundedGeometry {
        std::uint32_t model{},part{};
        Key geometry{},positionCertificate{};
        Bounds bounds{};
        std::uint32_t indexCount{};
        // Complete current index range, not a selected patch or native AABB.
        // completePrebounded must prove provenance and full containment, e.g.
        // an owned mesh root only if EVERY input triangle was retained.
    };
    // Pure native geometry preparation for a late-selected technique. This is
    // deliberately a separate type: xyzLayout is NOT a shader certificate.
    // A resulting bound applies only if drawAllowed subsequently proves the
    // actually selected VS uses this XYZ1 input and the same affine transform.
    struct GeometryPrebound {
        std::uint32_t model{},part{};
        Key geometry{},xyzLayout{};
        Bounds bounds{};
        std::uint32_t indexCount{};
    };
    struct Pose {
        std::uint64_t scene{},frame{},owner{},revision{};
        bool operator==(const Pose&)const=default;
        // revision is an adapter-owned mutation-stable receipt, never native
        // MatrixSet+0xd8 (that field is only an upload-frame stamp).
    };
    struct ModelTransform {
        std::uint32_t model{},paletteIndex{};
        // Exact FINAL native model-to-world coefficients, column-major affine.
        // For DrawMulti this includes ComputeMatrix and optional parent in the
        // pinned native FP order. This helper does not reproduce that producer.
        std::array<float,16> world{};
        // Optional conservative coefficient uncertainty. Exact copied native
        // floats use zero. Bottom affine row must remain exact [0,0,0,1].
        std::array<double,16> coefficientError{};
    };
    struct Authority {
        void* context{};
        // Requires a mutation-stable current borrow, all cold dependencies
        // (geometry, selected LOD, material/position certificate) and, for a
        // nonnull pose, the complete current copied matrix palette. Current()
        // is NOT a replacement for actual ownership or concurrency exclusion.
        bool(*current)(void*,const Asset&,const Pose*)noexcept{};
        // Verifies counts/order/full native index ranges and every part of the
        // selected LOD against the current authoritative graph; no skinning,
        // deformation, unsupported position shader or missing model permitted.
        bool(*complete)(void*,const Asset&,std::span<const Model>,std::span<const Geometry>)noexcept{};
        bool(*completePrebounded)(void*,const Asset&,std::span<const Model>,std::span<const PreboundedGeometry>)noexcept{};
    };
    struct GeometryAuthority {
        void* context{};
        bool(*current)(void*,const Asset&,const Pose*)noexcept{};
        // Complete chosen-LOD native XYZ index prefixes and rigid model/palette
        // ownership only. Does not certify any material or actual shader.
        bool(*complete)(void*,const Asset&,std::span<const Model>,std::span<const GeometryPrebound>)noexcept{};
    };
    enum class ProofKind {None,ShaderCertified,ConditionalAffineXYZ};
    struct Result {
        Bounds bounds{};
        double worldError{};
        std::array<double,3> axisError{};
        std::uint32_t models{};
        Asset asset{};Pose pose{};
        bool valid{};
        ProofKind proofKind{ProofKind::None};
    };
    enum class Failure {None,Authority,Manifest,Geometry,NonFinite,Budget,Transform,Changed};
    bool build(const Asset&,std::span<const Model>,std::span<const Geometry>,Authority)noexcept;
    bool buildPrebounded(const Asset&,std::span<const Model>,std::span<const PreboundedGeometry>,Authority)noexcept;
    bool buildGeometryPrebounded(const Asset&,std::span<const Model>,std::span<const GeometryPrebound>,GeometryAuthority)noexcept;
    bool update(const Pose&,std::span<const ModelTransform>,Result&)const noexcept;
    bool current(const Pose&)const noexcept;
    void reset()noexcept;
    Failure failure()const noexcept{return failure_;}
    std::uint32_t models()const noexcept{return count_;}
    bool local(std::uint32_t model,Bounds&)const noexcept;
private:
    struct Local {Bounds bounds{};std::uint32_t paletteIndex{};};
    Asset asset_{};Authority authority_{};std::array<Local,MaxModels> locals_{};
    std::uint32_t count_{};bool valid_{};mutable Failure failure_{Failure::None};
    ProofKind proofKind_{ProofKind::None};
};
}
