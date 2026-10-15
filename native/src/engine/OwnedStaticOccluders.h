#pragma once
#include "OwnedStaticVertices.h"
#include <array>
#include <bit>
#include <memory>
#include <shared_mutex>
#include <span>

namespace rdrvr::engine {
// Cold, exact planar-union preprocessing of the actual submitted index range.
// No hull, epsilon welding, collision geometry, GPU mapping or per-frame rebuild.
class OwnedStaticOccluders {
    struct Impl;
    struct SharedData;
public:
    using XYZ=OwnedStaticVertices::XYZ;
    static constexpr unsigned MaxPatchVertices=16,MaxBoundaryVertices=128;
    static constexpr unsigned MaxInputTriangles=32768,MaxEntries=4096,TableCapacity=16384,MaxProbe=32;
    static constexpr std::size_t MaxBytes=32*1024*1024;
    struct Identity {
        OwnedStaticVertices::Identity vertices{};
        std::uintptr_t indexBuffer{},nativeIndexBuffer{},indexAllocation{};
        std::uint64_t indexGpuBuffer{};
        std::uint32_t indexByteSize{},indexCount{};
        bool operator==(const Identity&)const=default;
    };
    struct LocalPatch {std::uint32_t firstIndex{};std::uint16_t sourceTriangleCount{};std::uint8_t count{},reserved{};};
    static_assert(sizeof(LocalPatch)==8);
    static constexpr unsigned MaxBvhLeafPatches=16;
    // Median split reaches <=16 patches by height ceil(log2(ceil(P/16))).
    // Reserving a complete tree at that height bounds the actual topology,
    // without a two-nodes-per-patch transient allocation.
    static constexpr std::size_t bvhNodeCapacity(std::size_t patches)noexcept {
        if(!patches||patches>MaxInputTriangles)return 0;
        return 2*std::bit_ceil((patches+MaxBvhLeafPatches-1)/MaxBvhLeafPatches)-1;
    }
    // Internal children are adjacent; leaves refer to patchOrder, preserving
    // the original descending-area patches array used by the Lease API.
    struct BvhNode {
        XYZ minimum{},maximum{};std::uint32_t first{},count{};
        // Componentwise maximum LOCAL span of any individual descendant
        // patch, rounded outward; never the (usually larger) whole-node span.
        // Infinity means no finite size bound, not an empty/tiny subtree.
        XYZ maxPatchExtent{};
        // Componentwise max over descendant patches of their vertex minima,
        // rounded toward +infinity, and min over their vertex maxima rounded
        // toward -infinity. For a clip row and one chosen coordinate these
        // bound an ACTUAL vertex in EVERY patch; other coordinates still use
        // this node's outer bounds. They are source-planning hints only.
        // Nonfinite values mean unknown, never a proof of empty coverage.
        XYZ patchMinMaximum{},patchMaxMinimum{};
    };
    static_assert(sizeof(BvhNode)==68);
    class Snapshot {
        friend class OwnedStaticOccluders;
        std::shared_ptr<const SharedData> data_{};
        std::uint64_t incarnation_{},serial_{};
    public:
        Snapshot()=default;~Snapshot()=default;
        Snapshot(const Snapshot&)=delete;Snapshot& operator=(const Snapshot&)=delete;
        Snapshot(Snapshot&&)noexcept;Snapshot& operator=(Snapshot&&)noexcept;
        void reset()noexcept;
        explicit operator bool()const noexcept{return bool(data_)&&!patches.empty();}
        Identity identity{};std::uint64_t epoch{},generation{},byteSize{};
        std::span<const XYZ> positions{};std::span<const LocalPatch> patches{};
        std::span<const std::uint16_t> patchIndices{},patchOrder{};
        std::span<const BvhNode> nodes{};
    };
    class Lease {
        friend class OwnedStaticOccluders;
        const Impl* owner_{};std::uint64_t incarnation_{},serial_{};
        std::shared_lock<std::shared_mutex> lock_{};
    public:
        Lease()=default;~Lease()=default;Lease(const Lease&)=delete;Lease& operator=(const Lease&)=delete;
        Lease(Lease&&)noexcept;Lease& operator=(Lease&&)noexcept;
        void reset()noexcept;
        explicit operator bool()const noexcept{return lock_.owns_lock()&&!patches.empty();}
        Identity identity{};std::uint64_t epoch{},generation{};
        std::span<const XYZ> positions{};std::span<const LocalPatch> patches{};
        std::span<const std::uint16_t> patchIndices{};
    };
    struct Statistics {std::uint64_t epoch{},entries{},bytes{},captures{},invalidations{},evictions{},declined{},inputTriangles{},patches{},mergedTriangles{},residentBytes{};};
    OwnedStaticOccluders();~OwnedStaticOccluders();
    OwnedStaticOccluders(const OwnedStaticOccluders&)=delete;OwnedStaticOccluders& operator=(const OwnedStaticOccluders&)=delete;
    bool bind(std::uint64_t epoch)noexcept;void clear()noexcept;
    // Both sources are owner-held immutable/live mappings for this exact native
    // optimizer transaction. No pointer survives this synchronous copy.
    bool capture(std::uint64_t epoch,const Identity&,const void* vertexMapped,std::size_t vertexBytes,
                 const std::uint16_t* indices,std::size_t indexBytes,bool qualified,
                 std::uint64_t expectedMutationSerial)noexcept;
    void invalidate(std::uintptr_t nativeBacking)noexcept;
    // Before native index Lock and after its matching Unlock (or failed Lock).
    // Active writes are tracked even before the first cache capture; overflow
    // quarantines this cache until device clear, never guesses immutability.
    void beginIndexMutation(std::uintptr_t nativeBacking)noexcept;
    void endIndexMutation(std::uintptr_t nativeBacking)noexcept;
    bool query(std::uint64_t epoch,const Identity&,Lease&)const noexcept;
    bool current(const Lease&)const noexcept;
    // Owns copied immutable values, not native objects or a retained read lock.
    // Invalidation/clear revoke current(), but cannot invalidate the memory.
    bool querySnapshot(std::uint64_t epoch,const Identity&,Snapshot&)const noexcept;
    bool current(const Snapshot&)const noexcept;
    std::uint64_t invalidationSerial()const noexcept;
    std::uint64_t mutationSerial()const noexcept;
    Statistics statistics()const noexcept;
private:std::unique_ptr<Impl> impl_;
};
extern OwnedStaticOccluders ownedStaticOccluders;
}
