#pragma once
#include "ConservativeStereoRaster.h"
#include "OwnedStaticOccluders.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace rdrvr::engine {
class OwnedStaticVertices;
class OwnedStaticOccluders;
class NativeStaticOcclusionSource;
class NativeGeometryBindingReceipt;
// Bounded synchronous snapshot of the ALREADY mapped buffers borrowed by the
// pinned full-buffer grcDevice::DrawIndexedPrimitive call. Never calls native
// getters, Map, Lock, InitCPUCache or a GPU API. No asset pointer survives capture.
// Native ownership/quiescence and opaque affine shader proof MUST be supplied by
// integration. Guarded reads and repeated headers cannot establish those facts.
class NativeStaticTriangles {
public:
    static constexpr std::size_t MaxTriangles=256, MaxVertexBytes=65536;
    using Read=bool(*)(void*,std::uintptr_t,void*,std::size_t);
    struct Api {
        Read read{}; void* context{};
        std::uintptr_t vertexVtable{},indexVtable{},bufferVtable{};
        bool completeCodeProof{};
        // Optional exact owned XYZ copy from the cold optimizer Unlock. This
        // never authorizes reading a former native map after it was unmapped.
        const OwnedStaticVertices* ownedVertices{};
        std::uint64_t ownedEpoch{};
        const OwnedStaticOccluders* ownedOccluders{};
    };
    struct Budget { std::uint32_t draws{32},triangles{2048}; std::uint64_t bytes{512*1024}; std::uint32_t packetDraws{512}; };
    // CertifiedXYZ1 is permitted only after exact shader dataflow proves that
    // Position uses attribute.xyz and supplies its own homogeneous 1. Packed
    // normal bits in attribute.w are otherwise deliberately rejected.
    enum class Position { Homogeneous,CertifiedXYZ1 };
    struct Draw {
        const void* vertexBuffer{}; const void* indexBuffer{};
        std::uint32_t indexCount{}; // Exact native w4: firstIndex=baseVertex=0.
        ConservativeStereoRaster::Matrix world{}; // Row-major affine transform.
        ConservativeStereoRaster::Faces faces{ConservativeStereoRaster::Faces::Unknown};
        // Actual submitted MAIN static LEFT depth-writing draw, one instance, triangle list,
        // no missing vertex streams, opaque/no-discard affine shader/pipeline,
        // complete current world transform, owner-held and no concurrent writes.
        // Also normal LESS/LEQUAL Z, full viewport/scissor/sample coverage, no
        // stencil/A2C/sample-mask/rasterizer discard, actual GPU draw emitted.
        bool qualified{};
        Position position{Position::Homogeneous};
        // Nonzero values require agreement with the actual bound PSO's vertex
        // input. Zero keeps the standalone source-reader contract.
        std::uint32_t expectedPositionBytes{},expectedStride{};
    };
    struct Snapshot {
        std::array<ConservativeStereoRaster::Triangle,MaxTriangles> triangles{};
        std::array<ConservativeStereoRaster::Patch,MaxTriangles> patches{};
        std::uint32_t count{};
        std::uint32_t patchCount{},sourceTriangleCount{};
        bool completeDraw{};
        std::uint64_t frame{};
        ConservativeStereoRaster::Vec3 worldError{};
        // Exact Vulkan handles from the native backing wrappers (+0x10).
        // No pointer or ownership escapes; integration must match actual binds.
        std::uint64_t vertexHandle{},indexHandle{};
    };
    // A packet owns copied scalar evidence and optionally immutable mesh data.
    // Pending identities are sealed by the live native witness, then compared
    // against the mod's own cache only. No native pointer may be dereferenced later.
    struct Packet {
    private:
        friend class NativeStaticTriangles;
        friend class PlannerReplaySnapshot;
        OwnedStaticOccluders::Identity identity_{};
        const OwnedStaticOccluders* cache_{};
        std::uint64_t epoch_{},serial_{};
        bool pending_{};
    public:
        OwnedStaticOccluders::Snapshot mesh{};
        ConservativeStereoRaster::Matrix world{};
        ConservativeStereoRaster::Vec3 worldError{};
        ConservativeStereoRaster::Faces faces{ConservativeStereoRaster::Faces::Unknown};
        std::uint64_t frame{},vertexHandle{},indexHandle{};
        Packet()=default;Packet(const Packet&)=delete;Packet& operator=(const Packet&)=delete;
        Packet(Packet&& other)noexcept{*this=std::move(other);}
        Packet& operator=(Packet&& other)noexcept {
            if(this!=&other){mesh=std::move(other.mesh);world=other.world;worldError=other.worldError;faces=other.faces;
                frame=other.frame;vertexHandle=other.vertexHandle;indexHandle=other.indexHandle;
                identity_=other.identity_;cache_=other.cache_;epoch_=other.epoch_;serial_=other.serial_;pending_=other.pending_;other.reset();}
            return *this;
        }
        explicit operator bool()const noexcept{return frame&&(pending_||bool(mesh));}
        bool pending()const noexcept{return pending_;}
        const OwnedStaticOccluders::Identity& identity()const noexcept{return pending_?identity_:mesh.identity;}
        void reset()noexcept{mesh.reset();frame=vertexHandle=indexHandle=0;worldError={};faces=ConservativeStereoRaster::Faces::Unknown;
            pending_=false;cache_=nullptr;epoch_=serial_=0;}
    };
    struct Stats {
        std::uint64_t calls{},qualified{},copiedTriangles{},bytes{},reads{},unqualified{};
        std::uint64_t badOwner{},unmapped{},unsupportedFormat{},badRange{},badData{};
        std::uint64_t readFailures{},changedOwner{},budgetStops{},stale{};
        std::uint64_t ownedQueries{},ownedHits{},ownedMisses{},ownedBytes{};
        std::uint64_t meshQueries{},meshHits{},meshMisses{},copiedPatches{},sourceTriangles{};
        std::uint64_t transformedVertices{},reusedVertices{};
        std::uint64_t packetCalls{},packets{},packetPatches{},deferredPackets{};
    };
    void configure(Api) noexcept;
    bool begin(std::uint64_t frame,Budget) noexcept;
    bool canCapture(std::uint64_t frame)const noexcept;
    bool canCapturePacket(std::uint64_t frame)const noexcept;
    bool capturePacket(std::uint64_t frame,const Draw&,Packet&) noexcept;
    // Explicit frame and cache identity prevent cross-frame/foreign-cache use.
    // Resolution performs no native reads; failure clears the whole packet.
    static bool packetCurrent(std::uint64_t frame,const Packet&,const OwnedStaticOccluders&) noexcept;
    static bool resolvePacket(std::uint64_t frame,Packet&,const OwnedStaticOccluders&) noexcept;
    const Snapshot& capture(std::uint64_t frame,const Draw&) noexcept;
    // Synchronous output into caller-owned storage; published counts delimit
    // all valid elements. Failed captures clear metadata, never array tails.
    const Snapshot& captureInto(std::uint64_t frame,const Draw&,Snapshot&) noexcept;
    static void clear(Snapshot&) noexcept;
    void end() noexcept;
    bool ready() const noexcept { return ready_; }
    const Stats& stats() const noexcept { return stats_; }
    // Outward absolute error of float32 WorldSingle*XYZ1, permitting arbitrary
    // contraction/order and input/result flush-to-zero. localAbs bounds xyz.
    // False means a possible float overflow or non-float native coefficient.
    static bool worldErrorBound(const ConservativeStereoRaster::Matrix& world,
        const ConservativeStereoRaster::Vec3& localAbs,ConservativeStereoRaster::Vec3&) noexcept;
private:
    friend class NativeStaticOcclusionSource;
    bool capturePacketVerified(std::uint64_t,const Draw&,const NativeGeometryBindingReceipt&,Packet&) noexcept;
    bool captureQualifiedPacket(std::uint64_t,const ConservativeStereoRaster::Matrix&,ConservativeStereoRaster::Faces,const NativeGeometryBindingReceipt&,Packet&)noexcept;
    bool read(std::uintptr_t,void*,std::size_t) noexcept;
    Api api_{}; Budget budget_{}; Stats stats_{}; Snapshot snapshot_{};
    std::array<std::uint16_t,MaxTriangles*3> indices_{};
    std::array<std::byte,MaxVertexBytes> vertices_{};
    struct Transformed {ConservativeStereoRaster::Vec3 value{};std::uint32_t epoch{};std::uint16_t index{};};
    std::array<Transformed,512> transformed_{};
    std::uint32_t transformEpoch_{};
    std::uint64_t frame_{},meshSerial_{}; bool ready_{};
};
} // namespace rdrvr::engine
