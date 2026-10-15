#pragma once
#include <vulkan/vulkan.h>
#include "UniformCaptureSink.h"
#include "FrozenCameraCreationRegistry.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>

namespace rdr95 {class OwnedIndexedReplay;class OwnedDepthRequalification;}
namespace rdrvr::engine {
// A single recording-thread mirror of ACTUAL native Vulkan state writes.
// No installer, object replay admission, buffer/descriptor content ownership,
// synchronization or resource lifetime grant. Every referenced GPU object must
// independently remain valid through command retirement/resubmission.
class IndexedCommandState final {
public:
    static constexpr unsigned maxSets=7,maxBindings=8,maxOffsets=32,maxPushBytes=128;
    static constexpr unsigned maxPackets=64;
    struct Command {
        VkDevice device{};VkCommandPool pool{};VkCommandBuffer buffer{};
        std::uint64_t allocation{},recording{},deviceGeneration{};
        bool operator==(const Command&) const =default;
    };
    struct Indexed {std::uint32_t count{},instances{},first{};std::int32_t vertex{};std::uint32_t base{};};
    struct Api {
        PFN_vkCmdBindPipeline pipeline{};PFN_vkCmdBindDescriptorSets descriptors{};
        PFN_vkCmdBindVertexBuffers vertices{};PFN_vkCmdBindIndexBuffer indices{};
        PFN_vkCmdSetViewport viewport{};PFN_vkCmdSetScissor scissor{};
        PFN_vkCmdSetLineWidth lineWidth{};PFN_vkCmdSetDepthBias depthBias{};
        PFN_vkCmdSetBlendConstants blendConstants{};PFN_vkCmdSetDepthBounds depthBounds{};
        PFN_vkCmdSetStencilCompareMask stencilCompare{};PFN_vkCmdSetStencilWriteMask stencilWrite{};
        PFN_vkCmdSetStencilReference stencilReference{};
        PFN_vkCmdSetDepthBoundsTestEnable depthBoundsEnable{};
        PFN_vkCmdPushConstants push{};PFN_vkCmdDrawIndexed draw{};
    };
private:
    struct Layout;struct Pipeline;struct Cold;
    struct Set {
        VkDescriptorSet handle{};const Layout* layout{};
        std::array<std::uint32_t,maxOffsets> offsets{};std::uint32_t count{};bool valid{};
    };
    struct State {
        const Pipeline* pipeline{};std::array<Set,maxSets> sets{};
        std::array<VkBuffer,maxBindings> vertices{};std::array<VkDeviceSize,maxBindings> offsets{};std::uint32_t vertexMask{};
        VkBuffer index{};VkDeviceSize indexOffset{};VkIndexType indexType{};bool indexKnown{};
        VkViewport viewport{};VkRect2D scissor{};float lineWidth{},depthBias[3]{},blend[4]{},depthBounds[2]{};
        std::array<std::uint32_t,2> compare{},write{},reference{};
        VkBool32 boundsEnable{};std::uint32_t dynamicKnown{};
        std::array<std::array<std::uint8_t,maxPushBytes>,2> push{};
        std::array<std::uint32_t,2> pushKnown{};std::array<const Layout*,2> pushLayout{};
    };
public:
    class Packet {
        friend class IndexedCommandState;
        friend class ::rdr95::OwnedDepthRequalification;
        State state_{};Command command_{};Indexed indexed_{};
        VkRenderPass pass_{};std::uint32_t subpass_{};
        std::uint64_t issuer_{},session_{};
    public:
        Packet()=default;
    };
    class OwnedPacket {
        friend class IndexedCommandState;
        friend class ::rdr95::OwnedDepthRequalification;
        Packet packet_{};rdr95::UniformCaptureSink::Ticket uniforms_{};
        unsigned mask_{};
    public:
        OwnedPacket()=default;
    };
    enum class Emission {Declined,Emitted,EmittedFaulted};
    // Read-only copied tuples for a separately qualified Runtime plan. These
    // values are descriptions, never a packet issuer or a resource lease.
    struct Execution {
        void* context{};
        bool(*current)(void*)noexcept{};
        bool(*drawIndexed)(void*,unsigned part,Indexed)noexcept{};
    };
    struct DescriptorDescription {
        VkDescriptorSet handle{};VkPipelineLayout sourceLayout{};
        std::uint64_t sourceLayoutGeneration{};std::uint32_t count{};
        std::array<std::uint32_t,maxOffsets> offsets{};
    };
    struct Description {
        Command command{};VkRenderPass pass{};std::uint32_t subpass{};
        VkPipeline pipeline{};VkPipelineLayout restoreLayout{};
        std::uint64_t issuer{},session{};std::uint32_t setMask{};
        std::array<DescriptorDescription,maxSets> sets{};
        std::uint64_t variantSerial{},nativeLayoutGeneration{};
    };
    struct OwnedGeometry {
        std::array<VkBuffer,maxBindings> vertices{};
        std::array<VkDeviceSize,maxBindings> offsets{};
        std::uint32_t vertexMask{};
        VkBuffer index{};VkDeviceSize indexOffset{};VkIndexType indexType{};
        Indexed draw{};
    };
    IndexedCommandState(VkDevice,std::uint64_t deviceGeneration);~IndexedCommandState();
    IndexedCommandState(const IndexedCommandState&)=delete;IndexedCommandState& operator=(const IndexedCommandState&)=delete;
    // Successful creation inputs only. Cold fixed-capacity immutable copies;
    // original DSL handles may subsequently die. Only exact binding-flags
    // pNext with PARTIALLY_BOUND (including legal zero count) is supported.
    bool layoutCreated(VkPipelineLayout,const VkPipelineLayoutCreateInfo&,
                       std::span<const VkDescriptorSetLayoutCreateInfo>) noexcept;
    void layoutDestroyed(VkPipelineLayout) noexcept;
    // restore must be an independently retained, live layout whose copied
    // definitions exactly match the native layout. No borrowed creation ptrs.
    bool pipelineCreated(VkPipeline,const VkGraphicsPipelineCreateInfo&,VkPipelineLayout restore) noexcept;
    void pipelineDestroyed(VkPipeline) noexcept;
    // Cold recording-thread import of an actual Registry export, not a new
    // native creation event. Dead historical native definitions stay separate
    // from the owned restore layout. Duplicate variants reuse their records.
    bool importReplayMetadata(const FrozenCameraCreationRegistry::ReplayMetadata&) noexcept;
    // Called after actual successful Begin. Reusing even an identical key
    // creates a fresh mirror session; old packets never revive.
    bool begin(Command,Api) noexcept;
    void end(VkCommandBuffer) noexcept;
    // Permanent for this device incarnation; retaining original forwarders is
    // the caller's responsibility. A numeric device-handle ABA needs a new instance.
    void retireDevice() noexcept;
    void beginPass(VkCommandBuffer, VkRenderPass,std::uint32_t subpass=0) noexcept;
    void endPass(VkCommandBuffer) noexcept;
    void unsupported(VkCommandBuffer) noexcept;
    // Post-original observations. Arguments are copied, including every
    // dynamic offset; these must see the FINAL translated native descriptor
    // bind, never temporary binds from the mod's own camera sandwich.
    void bindPipeline(VkCommandBuffer, VkPipelineBindPoint,VkPipeline) noexcept;
    void bindDescriptors(VkCommandBuffer, VkPipelineBindPoint,VkPipelineLayout,std::uint32_t first,
                         std::span<const VkDescriptorSet>,std::span<const std::uint32_t>) noexcept;
    // CURRENT live Registry generations from before/after the terminal native
    // bind, never historical pipeline generations. Unknown/mismatch clears
    // descriptor knowledge. No native input pointer is dereferenced here.
    void bindDescriptorsCurrent(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,
                                std::uint64_t before,std::uint64_t after,std::uint32_t first,
                                std::span<const VkDescriptorSet>,std::span<const std::uint32_t>) noexcept;
    void bindVertices(VkCommandBuffer, std::uint32_t first,std::span<const VkBuffer>,std::span<const VkDeviceSize>) noexcept;
    void bindIndex(VkCommandBuffer, VkBuffer,VkDeviceSize,VkIndexType) noexcept;
    void setViewport(VkCommandBuffer, std::uint32_t first,std::span<const VkViewport>) noexcept;
    void setScissor(VkCommandBuffer, std::uint32_t first,std::span<const VkRect2D>) noexcept;
    void setLineWidth(VkCommandBuffer, float) noexcept;void setDepthBias(VkCommandBuffer, float constant,float clamp,float slope) noexcept;
    void setBlendConstants(VkCommandBuffer, const float*) noexcept;void setDepthBounds(VkCommandBuffer, float,float) noexcept;
    void setStencilCompare(VkCommandBuffer, VkStencilFaceFlags,std::uint32_t) noexcept;
    void setStencilWrite(VkCommandBuffer, VkStencilFaceFlags,std::uint32_t) noexcept;
    void setStencilReference(VkCommandBuffer, VkStencilFaceFlags,std::uint32_t) noexcept;
    void setDepthBoundsEnable(VkCommandBuffer, VkBool32) noexcept;
    void pushConstants(VkCommandBuffer, VkPipelineLayout,VkShaderStageFlags,std::uint32_t offset,std::span<const std::uint8_t>) noexcept;
    void pushConstantsCurrent(VkCommandBuffer,VkPipelineLayout,std::uint64_t before,std::uint64_t after,
                              VkShaderStageFlags,std::uint32_t offset,std::span<const std::uint8_t>) noexcept;
    bool snapshotIndexed(VkCommandBuffer, Indexed,Packet&) noexcept;
    // Preflights the entire write+restore sequence before its first PFN. This
    // emits ACTUAL raw indexed commands for caller-owned authored resources;
    // it is not retail drawable admission. Declined guarantees no Vulkan call.
    // EmittedFaulted must NEVER trigger a fallback draw after partial emission.
    Emission emitAndRestore(const Packet&) noexcept;
    // ALL preflights finish before the first PFN, then incoming state is restored
    // once on success. Any failed/retired observer stops ALL further PFNs,
    // including restoration: command/resource lifetime is then unknown.
    // Packet span/storage must stay immutable throughout this synchronous
    // call (including raw PFNs). No heap allocation and no cross-call cache.
    Emission emitAndRestore(std::span<const Packet>) noexcept;
    bool describe(const Packet&,Description&) noexcept;
    bool describeIncoming(Description&) noexcept;
    // Capture only after the actual terminal translated bind was observed.
    // Every dynamic UBO set must match the current DUD snapshot exactly. Other
    // resource contents/lifetimes and per-eye textures remain caller obligations.
    bool snapshotOwnedIndexed(VkCommandBuffer,Indexed,const rdr95::UniformCaptureSink&,OwnedPacket&) noexcept;
    // Synchronous resolution retains the nonforgeable arena tickets. No public
    // conversion exposes an ordinary packet that could outlive this validation.
    Emission emitOwnedAndRestore(const OwnedPacket&) noexcept;
    Emission emitOwnedAndRestore(std::span<const OwnedPacket>) noexcept;
    bool describeOwned(const OwnedPacket&,Description&) noexcept;
    // A validated copied description only; no packet issuer or buffer lease.
    // Failure leaves output unchanged. Intended for source completion checks.
    bool describeOwnedGeometry(const OwnedPacket&,OwnedGeometry&) noexcept;
    bool preflightOwned(std::span<const OwnedPacket>) noexcept;
    Emission emitOwnedAndRestore(std::span<const OwnedPacket>,Execution) noexcept;
    bool good() const noexcept;
private:
    friend class ::rdr95::OwnedIndexedReplay;
    friend class ::rdr95::OwnedDepthRequalification;
    Emission emitOwnedSequence(std::span<const OwnedPacket>,Execution) noexcept;
    bool ownedSequenceCurrent(const Description&,std::uint64_t observed)const noexcept;
    std::unique_ptr<Cold> cold_;const std::thread::id owner_{std::this_thread::get_id()};
    const VkDevice device_{};const std::uint64_t deviceGeneration_{};std::atomic<bool> retired_{};
    std::atomic<bool> failed_{};std::uint64_t issuer_{},session_{},observationRevision_{};Command command_{};Api api_{};Execution execution_{};
    State state_{};VkRenderPass pass_{};std::uint32_t subpass_{};bool recording_{},emitting_{};
    bool access() noexcept;bool access(VkCommandBuffer) noexcept;bool coldAccess() noexcept;
    const Layout* layout(VkPipelineLayout) const noexcept;const Pipeline* pipeline(VkPipeline) const noexcept;
    const Layout* historicalLayout(VkPipelineLayout,std::uint64_t) const noexcept;
    void recordDescriptors(const Layout*,std::uint32_t,std::span<const VkDescriptorSet>,std::span<const std::uint32_t>) noexcept;
    void recordPush(const Layout*,VkShaderStageFlags,std::uint32_t,std::span<const std::uint8_t>) noexcept;
    bool complete(const State&) const noexcept;
    bool uniformInterface(const Layout&,unsigned,const rdr95::CommandUniformSets::Binding&)const noexcept;
    bool ownedCurrent(const OwnedPacket&)noexcept;
    Emission emitSequence(const void*,unsigned,const Packet&(*)(const void*,unsigned)noexcept,Execution,bool sequence=false)noexcept;
    bool canEmit(const State&,const State&) const noexcept;
    void emitState(const State&,std::uint32_t vertexMask,std::uint32_t dynamicMask,
                   std::uint32_t setMask,bool pushes) noexcept;
    void stencil(VkStencilFaceFlags,std::uint32_t,std::array<std::uint32_t,2>&,unsigned) noexcept;
    bool emitStateChecked(const State&,std::uint32_t vertexMask,std::uint32_t dynamicMask,
                          std::uint32_t setMask,bool pushes,bool bindPipeline=true) noexcept;
    bool emissionCurrent()noexcept;
    void describeState(const State&,std::uint32_t setMask,Description&)const noexcept;
};
}
