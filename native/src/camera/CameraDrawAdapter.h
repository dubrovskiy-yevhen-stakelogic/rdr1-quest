#pragma once
#include "CameraUniformOwner.h"
#include "FrozenCameraPipeline.h"
#include "FrozenCameraCreationRegistry.h"
#include <memory>
#include <span>

namespace rdr95 {
// PRIVATE integration adapter, no hook installer and no MAIN/eye issuer.
// All notifications are actual successful Vulkan creation/lifecycle events.
// Raw PFNs bypass both this adapter and DynamicUniformDescriptors. External
// Vulkan synchronization and complete routing remain caller obligations.
class CameraDrawAdapter final {
public:
    static constexpr unsigned MaxSets=7,MaxDynamic=112,MaxCommands=128;
    using DrawMetadata=rdrvr::engine::FrozenCameraCreationRegistry::DrawMetadata;
    struct Api {
        PFN_vkCmdBindPipeline bindPipeline{};
        PFN_vkCmdBindDescriptorSets bindDescriptors{};
        PFN_vkCmdDraw draw{};
        PFN_vkCmdDrawIndexed drawIndexed{};
        void* lookupContext{};
        bool(*lookup)(void*,VkPipeline,rdrvr::engine::FrozenCameraPipelineFactory::Variant&) noexcept{};
        // Optional coherent cold pull. When present, actual native bind
        // registers copied creation metadata on this recording thread. No
        // worker-side adapter creation callbacks are required or accepted.
        bool(*lookupDrawMetadata)(void*,VkPipeline,DrawMetadata&) noexcept{};
        // Actual descriptor binds need the CURRENT live layout incarnation,
        // not the old layout retained by an otherwise live pipeline record.
        bool(*lookupLayoutGeneration)(void*,VkPipelineLayout,std::uint64_t&) noexcept{};
        std::uint64_t(*metadataRevision)(void*) noexcept{}; // Required only by lexical packet plans.
    };
    struct Request {
        CameraUniformOwner::Ticket camera;
        CameraUniformOwner::PassKey expected;
        // Caller must issue ONLY inside exact MAIN tile/draw scope. LeftDepth
        // alone also contains shadows. Absence always forwards native draw.
    };
    CameraDrawAdapter();
    ~CameraDrawAdapter();
    CameraDrawAdapter(const CameraDrawAdapter&)=delete;
    CameraDrawAdapter& operator=(const CameraDrawAdapter&)=delete;
    // Must succeed before any wrapper is published. Immutable raw backing
    // forwards even before initialize/after disarm. No teardown clears PFNs.
    bool bindOriginals(Api) noexcept;
    bool initialize(VkDevice,std::uint64_t factoryDeviceGeneration,Api,CameraUniformOwner&) noexcept;
    bool layoutCreated(VkPipelineLayout,const VkPipelineLayoutCreateInfo&,
                       std::span<const VkDescriptorSetLayoutCreateInfo>) noexcept;
    void layoutDestroyed(VkPipelineLayout) noexcept;
    // After successful factory.create, while the corresponding native layout
    // incarnation is still live; captured serial prevents raw layout-handle ABA.
    bool pipelineCreated(VkPipeline) noexcept;
    void pipelineDestroyed(VkPipeline) noexcept;
    void commandAllocated(VkDevice,VkCommandPool,VkCommandBuffer,VkCommandBufferLevel,VkResult) noexcept;
    void commandBegun(VkCommandBuffer,VkResult) noexcept;
    void commandEnded(VkCommandBuffer,VkResult) noexcept;
    void commandReset(VkCommandBuffer,VkResult) noexcept;
    void commandFreed(VkDevice,VkCommandPool,VkCommandBuffer) noexcept;
    void poolReset(VkDevice,VkCommandPool,VkResult) noexcept;
    void poolDestroyed(VkDevice,VkCommandPool) noexcept;
    // BEFORE ExecuteCommands/unknown graphics-state or descriptor extension.
    // Qualification remains lost until successful Begin/Reset, never guessed.
    void commandUnknown(VkCommandBuffer) noexcept;
    void disarm() noexcept;

    // Forward once, then snapshot actual emitted values. bindDescriptors MUST
    // be called at DynamicUniformDescriptors::bind's final original invocation,
    // after dynamic-offset translation (including the pass-through branch).
    void bindPipeline(VkCommandBuffer,VkPipelineBindPoint,VkPipeline) noexcept;
    void bindDescriptors(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,
                         std::uint32_t,std::uint32_t,const VkDescriptorSet*,
                         std::uint32_t,const std::uint32_t*) noexcept;
    // Explicit owned-layout path. This never treats an owned handle as a native
    // creation incarnation. The intended PSO must already be bound/imported.
    // Declined emits no Vulkan call. BoundUntracked means one raw bind occurred
    // but post-call identity failed; the command remains unknown until Begin.
    enum class OwnedBindResult { Declined, Bound, BoundUntracked };
    OwnedBindResult bindOwnedDescriptors(VkCommandBuffer,VkPipeline intendedNative,
                         VkPipelineLayout ownedRestore,std::uint32_t,std::uint32_t,
                         const VkDescriptorSet*,std::uint32_t,const std::uint32_t*) noexcept;
    static constexpr unsigned MaxPacketParts=64,MaxPacketOffsets=32;
    struct PacketDescriptor {VkDescriptorSet handle{};unsigned count{};std::array<std::uint32_t,MaxPacketOffsets> offsets{};};
    struct PacketTuple {VkPipeline pipeline{};VkPipelineLayout restoreLayout{};unsigned setMask{};std::array<PacketDescriptor,MaxSets> descriptors{};std::uint64_t expectedVariantSerial{},expectedNativeLayoutGeneration{};};
    enum class PacketResult {Declined,Completed,EmittedFaulted};
    class IndexedPacketPlan final {
        friend class CameraDrawAdapter;
        CameraDrawAdapter* owner_{};std::uint64_t serial_{};
        IndexedPacketPlan(CameraDrawAdapter* o,std::uint64_t s)noexcept:owner_(o),serial_(s){}
    public:
        IndexedPacketPlan(const IndexedPacketPlan&)=delete;IndexedPacketPlan&operator=(const IndexedPacketPlan&)=delete;
        bool current()const noexcept;
        void markFault()noexcept;
        bool drawIndexed(unsigned part,std::uint32_t count,std::uint32_t instances,std::uint32_t first,std::int32_t vertex,std::uint32_t base)noexcept;
    };
    // Internal composition entry: request is issued by the lexical MAIN route.
    // Raw state setters occur in emit; actual draws MUST use the supplied plan.
    PacketResult withIndexedPacketPlan(VkCommandBuffer,std::span<const PacketTuple>,const PacketTuple& incoming,
        const Request&,void* lifetime,bool(*current)(void*)noexcept,void* context,bool(*emit)(void*,IndexedPacketPlan&)noexcept)noexcept;
    // After bindOriginals succeeds, return whether an owned variant was used;
    // ALWAYS forward one original draw. All preflight occurs before override.
    // Native pipeline and every captured suffix set are restored synchronously
    // before returning.
    bool draw(VkCommandBuffer,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,
              const Request* =nullptr) noexcept;
    bool drawIndexed(VkCommandBuffer,std::uint32_t,std::uint32_t,std::uint32_t,
                     std::int32_t,std::uint32_t,const Request* =nullptr) noexcept;
private:
    friend class NativeCameraRuntime;
    PacketResult withIndexedPlanImpl(VkCommandBuffer,std::span<const PacketTuple>,const PacketTuple&,
        const Request&,void*,bool(*)(void*)noexcept,void*,bool(*)(void*,IndexedPacketPlan&)noexcept,bool sequence)noexcept;
    struct Impl;std::unique_ptr<Impl> impl_;
};
} // namespace rdr95
