#pragma once
#include "NativeStaticTriangles.h"
#include "NativeMatrixSnapshot.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace rdrvr::engine {
class NativeOcclusionDrawWitness;
class NativeGeometryBindingReceipt;
// Synchronous native borrow adapter. It owns copied values only; node/drawable
// integers are same-frame identity tokens, never retained dereferenceable leases.
// This reader does NOT authorize whole DrawCommon skipping: its owner must prove
// every emitted depth/color primitive of the object rigid, with no unknown or
// indirect/deforming/instanced path, before using an authored AABB as a candidate.
class NativeStaticOcclusionSource {
    friend class NativeOcclusionDrawWitness;
public:
    using Raster=ConservativeStereoRaster;
    using Read=NativeStaticTriangles::Read;
    using Matrix=Raster::Matrix;
    using FloatMatrix=std::array<float,16>;
    enum class Position { Unknown,DirectMvpXYZ1,RelativeAffineXYZ1 };
    struct Api {
        Read read{};void* context{};
        std::uintptr_t staticVtable{},contextVtable{};
        NativeStaticTriangles::Api triangles{};
        bool completeCodeProof{};
        std::uintptr_t drawableDraw{}; // Pinned ordinary rmcDrawable::Draw.
        // Optional only for an explicit live native DrawCommon borrow. Unknown
        // addresses and general source/ring/buffer reads keep the guarded reader.
        Read borrowedRead{};
    };
    struct Budget {
        std::uint32_t objectReads{4096},matrixReads{64};
        // Guarded I/O only. Native-owned object borrows are independently
        // bounded by objectReads and the fixed 448-byte validation record.
        std::uint64_t bytes{1024*1024};
        NativeStaticTriangles::Budget triangles{};
        // Deferred packets consume no transformed-patch prefix budget.
        std::uint32_t packetMatrixReads{512};
    };
    struct Object {
        std::uint64_t frame{};
        std::uintptr_t node{},drawable{};
        Matrix world{};
        Raster::Bounds bounds{};
        Raster::Vec3 worldError{};
        // Comparable copied metadata; deliberately excludes bucket/LOD fade
        // flags and DrawCommon record address which can differ across passes.
        bool valid{};
    };
    struct Camera {
        FloatMatrix relativeClip{},inverseView{};
        // For DirectMvp ONLY: exact current native viewport+100 for this model,
        // not a numerically guessed P*V*M. RelativeAffine does not consume it.
        FloatMatrix fullClip{};
        bool fullClipReady{};
    };
    struct Submitted {
        const void* info{};
        void* context{};
        std::uintptr_t command{};
        const void* vertexBuffer{};const void* indexBuffer{};
        std::uint32_t indexCount{},positionBytes{},vertexStride{};
        Position position{Position::Unknown};
        Raster::Faces faces{Raster::Faces::Unknown};
        Camera camera{};
        // Exact certificate/lifetime gate from the integration: emitted MAIN
        // LEFT depth draw, matching frame+recording generation, opaque normal-Z
        // full raster/sample coverage, one instance, no concurrent native write.
        bool qualified{};
    };
    struct Snapshot {
        Object object{};
        NativeStaticTriangles::Snapshot geometry{};
        bool completeDraw{}; // false if the bounded reader copied only a prefix.
    };
    using Packet=NativeStaticTriangles::Packet;
    enum class ObjectRefusal : std::uint8_t {
        Creature, MatrixSet, RecordFlags, NodeType, ChangedNodeType, NodeFlags,
        DrawableIdentity, DrawMethod, NonFiniteWorld, RecordWorld, InstanceWorld,
        Bounds, NumericalError, Count
    };
    static constexpr const char* objectRefusalName(ObjectRefusal reason) noexcept {
        constexpr const char* names[]={"creature","matrixSet","recordFlags","nodeType","changedNodeType","nodeFlags",
            "drawableIdentity","drawMethod","nonFiniteWorld","recordWorld","instanceWorld","bounds","numericalError"};
        const auto at=static_cast<std::size_t>(reason);
        return at<static_cast<std::size_t>(ObjectRefusal::Count)?names[at]:"unknown";
    }
    struct Stats {
        std::uint64_t objects{},objectReady{},draws{},captured{},bytes{},reads{};
        std::uint64_t stale{},unqualified{},badObject{},changedObject{},badUniforms{};
        std::uint64_t matrixMismatch{},readFailures{},budgetStops{};
        std::uint64_t borrowedReads{},errorCacheHits{},errorComputations{};
        std::uint64_t guardedBytes{},borrowedBytes{};
        std::uint64_t reusedMatrixSnapshots{},invalidMatrixSnapshots{};
        std::uint64_t packetDraws{},capturedPackets{};
        std::uint64_t verifiedPacketCaptures{},invalidPacketReceipts{};
        std::array<std::uint64_t,static_cast<std::size_t>(ObjectRefusal::Count)> objectRefusals{};
    };
    class BorrowScope {
        friend class NativeStaticOcclusionSource;
        friend class NativeOcclusionDrawWitness;
        NativeStaticOcclusionSource* owner_{};BorrowScope* previous_{};
        const void* info_{};std::uint64_t frame_{},epoch_{};bool qualified_{};
        std::uint64_t generation_{},inspectionGeneration_{};
        Object inspectedObject_{};bool objectInspected_{};
    public:
        // Owner proves the pinned ordinary DrawCommon record and transitive
        // node/drawable remain live and quiescent until this scope exits.
        // Type is checked BEFORE borrowing the static node's larger body.
        BorrowScope(NativeStaticOcclusionSource&,std::uint64_t,const void* info,bool qualified) noexcept;
        ~BorrowScope();BorrowScope(const BorrowScope&)=delete;
    };
    void configure(Api) noexcept;
    bool begin(std::uint64_t,Budget) noexcept;
    void end() noexcept;
    bool ready()const noexcept{return ready_;}
    bool canCapture(std::uint64_t)const noexcept;
    bool canCapturePacket(std::uint64_t)const noexcept;
    bool inspect(std::uint64_t,const void* info,Object&) noexcept;
    // Object identity was observed at the live DrawCommon entry. Revalidated
    // after the source snapshot; any replacement clears the entire result.
    const Snapshot& capture(std::uint64_t,const Object&,const Submitted&) noexcept;
    // Exact current successful draw witness, consumed synchronously inside
    // this source's matching native BorrowScope. No second read of the ring.
    const Snapshot& capture(std::uint64_t,const Object&,const Submitted&,const NativeMatrixSnapshot&) noexcept;
    // Matrix witness and native object lease must still be live. The result
    // holds no engine pointer lease or cache lock after this call returns.
    bool capturePacket(std::uint64_t,const Object&,const Submitted&,const NativeMatrixSnapshot&,Packet&) noexcept;
    bool capturePacketVerified(std::uint64_t,const Object&,const Submitted&,const NativeMatrixSnapshot&,
                               const NativeGeometryBindingReceipt&,Packet&) noexcept;
    bool qualifyPacketTransform(std::uint64_t,const Object&,const Camera&,const NativeMatrixSnapshot&,NativeGeometryBindingReceipt&)noexcept;
    bool capturePacketQualified(std::uint64_t,const NativeGeometryBindingReceipt&,Raster::Faces,Packet&)noexcept;
    static bool sameObject(const Object&,const Object&) noexcept;
    const Stats& stats()const noexcept{return stats_;}
    const NativeStaticTriangles::Stats& triangleStats()const noexcept{return triangles_.stats();}
private:
    bool refuseObject(ObjectRefusal reason) noexcept {
        ++stats_.badObject;++stats_.objectRefusals[static_cast<std::size_t>(reason)];return false;
    }
    bool read(std::uintptr_t,void*,std::size_t) noexcept;
    bool readObject(bool borrowed,std::uintptr_t,void*,std::size_t) noexcept;
    bool readWith(Read,std::uintptr_t,void*,std::size_t,bool borrowed=false) noexcept;
    bool matrices(const Object&,const Submitted&) noexcept;
    bool matrices(const Object&,const Submitted&,const NativeMatrixSnapshot&) noexcept;
    const Snapshot& captureImpl(std::uint64_t,const Object&,const Submitted&,const NativeMatrixSnapshot*) noexcept;
    Api api_{};Budget budget_{};Stats stats_{};NativeStaticTriangles triangles_{};
    Snapshot result_{};std::uint64_t frame_{};bool ready_{};
    // Pure numerical memo: no frame identity or borrowed pointer lease. Fresh
    // native validation always precedes comparison of these copied inputs.
    struct ErrorEntry {
        std::uintptr_t node{},drawable{};Matrix world{};Raster::Bounds bounds{};
        Raster::Vec3 error{};bool valid{};
    };
    std::array<ErrorEntry,1024> errors_{};
    std::array<std::uint8_t,256> errorVictims_{};
    std::uint64_t epoch_{};
    static thread_local BorrowScope* borrowScope_;
    static thread_local std::uint64_t borrowGeneration_;
};
}
