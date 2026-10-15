#pragma once
#include "NativeStaticMatrixWitness.h"
#include "NativeGeometryBindingReceipt.h"
#include <array>
#include <cstdint>

namespace rdrvr::engine {
// A synchronous native-owned borrow, not a general memory reader. The caller
// owns the pinned DrawCommon -> Geometry -> VkContext leaf stack. No pointer in
// an output grants a lease beyond that stack. No allocation, locks, syscalls,
// native calls, GPU work or persistent command state are performed here.
class NativeOcclusionDrawWitness {
public:
    using Source=NativeStaticOcclusionSource;
    using Binding=NativeStaticMatrixWitness::ActualBinding;
    struct Api {
        std::uintptr_t contextVtable{},pipelineVtable{},framebufferVtable{},bufferVtable{};
        std::uintptr_t staticVtable{},drawableDraw{},geometryVtable{};
        bool completeCodeProof{};
        // Exact pinned grmGeometry::sm_UseShadowBuffer native byte. Its live
        // value and geometry+14 together select the prepared-buffer branch.
        std::uintptr_t shadowBufferEnabledSlot{};
        std::uintptr_t vertexVtable{},indexVtable{};
    };
    struct Target {
        std::array<std::uintptr_t,8> colorSurfaces{};
        std::uintptr_t depthSurface{};
        std::uint32_t width{},height{};
        // Established once per scene/stage from existing live PassResources:
        // exact surfaces + image generation, full extent, one layer/mip/sample,
        // supported color and normal depth formats. Size alone is NOT proof.
        bool verified{};
    };
    struct Snapshot {
        std::uintptr_t command{},frame{},nativePipeline{},pipeline{},framebuffer{},renderPass{};
        std::uint64_t vertexBuffer{},vertexOffset{},indexBuffer{};
        std::array<float,6> viewport{}; // actual x,y,width,height,minDepth,maxDepth
        std::array<std::int32_t,2> scissorOffset{};
        std::array<std::uint32_t,2> scissorExtent{};
        NativeStaticMatrixWitness::Snapshot matrices{};
        bool valid{};
        std::uintptr_t context{};
        bool stateValid{};
    };
    enum class GeometryFailure : unsigned {
        None,Unqualified,Address,Type,ShadowBuffer,Ring,ExtraStream,PendingTask,
        Vertices,Indices,IndexCount,Declaration,Count
    };
    struct Geometry {
        const void* vertices{};const void* indices{};
        std::uint32_t indexCount{};unsigned mode{}; // Converted d11::PrimitiveType, NOT raw grcDrawMode.
        bool valid{};
        GeometryFailure failure{GeometryFailure::Unqualified};
    };
    struct ObjectBorrow {
        Source::Object object{};
        // worldError is intentionally NOT computed here. Finalize the numeric
        // bound once per distinct copied object before any raster rejection.
        bool valid{};
    };
    // Only AFTER native BindInputLayout succeeded AND the dynamic-descriptor
    // scope is good, with one of the four pinned leaf return addresses. Those
    // leaves issue their draw unconditionally after this successful return.
    static bool capture(const Api&,void* context,const Binding&,const Target&,
                        bool exactSuccessfulBind,Snapshot&) noexcept;
    // Split form lets the caller decline unknown cold PSO certificates before
    // copying/validating the uploaded matrices. Both calls belong to the SAME
    // synchronous successful primitive; no native calls or lease extension in
    // between. State-only snapshots are never valid whole-draw witnesses.
    static bool captureState(const Api&,void* context,const Binding&,const Target&,
                             bool exactSuccessfulBind,Snapshot&) noexcept;
    static bool captureMatrices(const Api&,void* context,const Binding&,Snapshot&) noexcept;
    // At the exact direct/virtual grmGeometryQB::Draw entry. Ring zero, no
    // simulation stream or pending task. The native global/instance predicate
    // selects ordinary +18/+8 or completed optimized +b0/+a8 positions/layout.
    static bool geometry(const Api&,const void* liveGeometry,bool exactScope,Geometry&) noexcept;
    // During the same submitted primitive borrow, prove that the certified
    // position input is the selected native position channel, not merely a
    // rigid shader reading an unrelated attribute. The caller also requires
    // certificate binding zero / offset zero. This does not map mesh data or
    // certify its lifetime beyond this synchronous native draw.
    static bool matchesGeometry(const Api&,const Geometry&,const Snapshot&,
                                unsigned positionBytes,unsigned stride,
                                NativeGeometryBindingReceipt* sourceReceipt=nullptr) noexcept;
    // Only at a current RIGHT DrawCommon entry, after a fresh object witness
    // and positive same-frame coverage. Walk the CURRENT selected LOD owners;
    // never dereference retained LEFT geometry pointers. A newly queued native
    // task, changed class, malformed or over-budget graph leaves the draw live.
    static constexpr unsigned MaxReplayModels=64,MaxReplayGeometries=256;
    static bool replayQuiescent(const Api&,const void* liveInfo,const Source::Object& fresh,
                                bool exactScope) noexcept;
    // At the exact live DrawCommon entry; read the node vptr BEFORE any derived
    // fields. Unknown/short node classes are never bulk-read as static objects.
    static bool object(const Api&,std::uint64_t frame,const void* liveInfo,
                       bool exactScope,ObjectBorrow&) noexcept;
};
}
