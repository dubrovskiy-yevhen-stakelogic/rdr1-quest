#pragma once
#include "NativeStaticTriangles.h"
#include "StaticOcclusionProjection.h"
#include <array>
#include <cstdint>

namespace rdrvr::engine {
// Actual LEFT-depth packets are admitted first; only at the LEFT-depth boundary
// is their owned immutable geometry traversed globally, best coverage first.
// Bounds affect scheduling ONLY. Every occluding cell still needs the original
// exact patch, facing, projection/error envelope and ConservativeStereoRaster.
class PlannerReplaySnapshot;
class PriorityOcclusionSources {
    friend class PlannerReplaySnapshot;
public:
    using Raster=ConservativeStereoRaster;
    using Packet=NativeStaticTriangles::Packet;
    static constexpr unsigned MaxSources=512,MaxQueue=4096,MaxTargets=2048;
    struct Budget {unsigned sources{MaxSources},queue{MaxQueue},expansions{8192},patches{2048},targetQueries{262144},reachabilityCells{131072};bool earlyCoverage{false};};
    struct Stats {
        std::uint64_t offered{},sources{},resolvedPackets{},meshQueries{},meshHits{},meshMisses{},availablePatches{},invalid{},sourceOverflow{},queueDropped{},queueHighWater{};
        std::uint64_t earlyTrials{},earlyTrialPatches{},earlyFullCoverage{};
        std::uint64_t objectBoundQueries{},earlyObjectDrops{},envelopeQueries{},envelopeHits{};
        std::uint64_t targets{},retiredTargets{},invalidTargets{},targetOverflow{},targetQueries{},targetWork{},targetBudgetStops{},targetRejectedBranches{};
        std::uint64_t unreachableTargets{},reachabilityCells{},reachabilityBudgetStops{},reachabilityPreflightStops{};
        std::uint64_t coveredNodes{},coveredPatches{},tileQueries{},smallBoundsDropped{},backfaceBoundsDropped{},clipDropped{},clipNodeDropped{};
        std::uint64_t nodeExpansions{},leafExpansions{},patchCandidates{},selectedPatches{},transformedVertices{};
        std::uint64_t prepared{},errorRejected{},rasterAccepted{},sourceTriangles{},expansionStops{},patchStops{},stale{},mutations{};
    };
    bool begin(std::uint64_t frame,const StaticOcclusionProjection&,Raster&,
               const OwnedStaticOccluders*,Budget)noexcept;
    bool enqueue(Packet&&)noexcept;
    bool enqueue(Packet&&,const Raster::Bounds&)noexcept;
    bool mayContribute(const Raster::Bounds&)const noexcept;
    bool coverageComplete()const noexcept;
    bool addTarget(const Raster::Bounds&,const Raster::Vec3& worldError)noexcept;
    // Call once after ALL source submissions, before raster/ledger seal. False
    // means no cells added; it never suppresses native rendering by itself.
    bool build(std::uint64_t frame)noexcept;
    void reset()noexcept;
    const Stats& stats()const noexcept{return stats_;}
private:
    struct Entry {
        struct Eye {double nearDepth{};unsigned char x0{},x1{},y0{},y1{},cx0{},cx1{},cy0{},cy1{};bool known{},empty{};};
        std::array<Eye,2> eye{};double area{},nearW{};unsigned source{},item{},maxPos{},minPos{};bool patch{},potential{},backfaceRejected{},clipRejected{};
    };
    struct Envelope {Raster::Bounds bounds{};Entry entry{};std::uint64_t epoch{};};
    static constexpr unsigned EnvelopeSlots=2048;
    mutable std::array<Envelope,EnvelopeSlots> envelopes_{};
    std::uint64_t envelopeEpoch_{};
    std::array<bool,MaxSources> clipReady_{};
    struct Target {std::array<Entry::Eye,2> eye{};bool active{};};
    std::array<Target,MaxTargets> targets_{};unsigned targetCount_{},activeTargets_{};
    std::array<double,2> maxTargetDepth_{};
    std::array<Entry,MaxSources> roots_{};
    std::array<std::array<double,64*64>,2> possibleDepth_{};
    std::array<Packet,MaxSources> sources_{};
    std::array<std::array<Raster::Matrix,2>,MaxSources> localClip_{};
    std::array<Entry,MaxQueue> entries_{};
    std::array<unsigned,MaxQueue> maxHeap_{},minHeap_{},free_{};
    unsigned sourceCount_{},queued_{},freeCount_{};
    const StaticOcclusionProjection* projection_{};Raster* raster_{};
    const OwnedStaticOccluders* cache_{};std::uint64_t frame_{},serial_{};
    Budget budget_{};mutable Stats stats_{};bool built_{};bool earlyTrialUsed_{};unsigned earlySource_{MaxSources},earlyPatchMask_{};
    bool better(const Entry&,const Entry&)const noexcept;
    bool heapBetter(unsigned,unsigned,bool)const noexcept;
    void swapHeap(unsigned,unsigned,bool)noexcept;
    void repair(unsigned,bool)noexcept;
    void remove(unsigned)noexcept;
    bool push(const Entry&)noexcept;
    Entry pop()noexcept;
    bool rank(unsigned source,unsigned item,bool patch,Entry&)noexcept;
    bool planBounds(const Raster::Bounds&,Entry&)const noexcept;
    bool enqueueImpl(Packet&&,const Raster::Bounds*)noexcept;
    void prepareClip(unsigned)noexcept;
    bool chargeTarget()noexcept;
    bool submit(const Entry&)noexcept;
    bool covered(const Entry&)noexcept;
    bool coveredEye(const Entry::Eye&,unsigned eye,bool targetBudget)noexcept;
    bool needed(const Entry&)const noexcept;
    void retireTargets(const Entry&)noexcept;
    void reachableTargets()noexcept;
    void updateTargetDepths(bool budgeted=false)noexcept;
};
}
