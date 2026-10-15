#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <memory>
#include <thread>
#include <type_traits>
#include <cstring>
#include <new>
#if defined(_MSC_VER)
#define RDR2_WORKER28_INLINE __forceinline
#else
#define RDR2_WORKER28_INLINE inline __attribute__((always_inline))
#endif
namespace rdr2::worker28 {
// Supplied by the layer's actual registry while a Begin HostEntry is retained.
// This carries identities, NOT a caller asserted coverage/ownership boolean.
struct PoolCommandToken {VkDevice device{};VkCommandPool pool{};VkCommandBuffer command{};
 std::uint64_t deviceGeneration{},poolGeneration{},commandGeneration{},recordingGeneration{};};
struct Dispatch15 {
 PFN_vkCmdBindPipeline pipeline{};PFN_vkCmdBindDescriptorSets descriptors{};
 PFN_vkCmdBindVertexBuffers vertices{};PFN_vkCmdBindIndexBuffer index{};
 PFN_vkCmdDraw draw{};PFN_vkCmdDrawIndexed indexed{};
 PFN_vkCmdPipelineBarrier barrier{};PFN_vkCmdBeginRenderPass beginPass{};
 PFN_vkCmdEndRenderPass endPass{};PFN_vkCmdSetViewport viewport{};
 PFN_vkCmdSetScissor scissor{};PFN_vkCmdSetStencilReference stencil{};
 PFN_vkCmdCopyImage copyImage{};PFN_vkCmdResetQueryPool resetQueryPool{};PFN_vkCmdWriteTimestamp writeTimestamp{};
 explicit operator bool()const noexcept;
};
using Dispatch13=Dispatch15; // compatibility name for the original 13-shape consumer API
class WorkerPool28 final {
public:
 static constexpr unsigned Capacity=4096,Chunk=1024;
 enum class AccessKind {Begin,Command,Resource,Lifecycle,Submit,Lookup,Shutdown};
 enum class Phase:std::uint64_t {Off=0,Open=1,Closing=2,Synchronous=3,Stopped=4};
 struct Stats {std::uint64_t enqueued{},executed{},publications{},closeRequests{},synchronousEntries{},
 admissions{},declinedAdmissions{},producerSleeps{},consumerSleeps{},rejectedPackets{},generation{},hostInflight{};
 Phase phase{};bool permanentlyRevoked{},stopped{};};
 // Diagnostic only: first failing predicate, sampled at the original decision.
 // Zero denotes admission success or no attempted admission, distinguished by caller.
 struct AdmissionReceipt74 {unsigned firstFailure{};std::uint64_t rawAdmission{};};
 class RecordingToken {
  friend class WorkerPool28;
  WorkerPool28* owner_{};VkCommandBuffer command_{};std::uint64_t generation_{},producer_{};
 public:
  RecordingToken()=default;VkCommandBuffer command()const noexcept{return command_;}
  std::uint64_t generation()const noexcept{return generation_;}
  explicit operator bool()const noexcept{return owner_&&command_&&generation_;}
 };
 class HostEntry {
  friend class WorkerPool28;
  WorkerPool28* owner_{};AccessKind kind_{};bool counted_{},preparation_{};
  HostEntry(WorkerPool28&,AccessKind)noexcept;
  HostEntry(WorkerPool28&,const RecordingToken&)noexcept;
 public:
  HostEntry()=default;~HostEntry();HostEntry(HostEntry&&)noexcept;HostEntry&operator=(HostEntry&&)noexcept;
  // True only when the core itself counted a fixed-producer OPEN receipt.
  // A later foreign entry can still close it; this is no resumption authority.
  bool preservesRecording()const noexcept{return counted_&&preparation_;}
  // No permission for the original API. Requires a counted producer guard.
  // All predecessor packets finish; OPEN is retained only if still valid.
  bool drainProducerPreparation(const RecordingToken&)noexcept;
  HostEntry(const HostEntry&)=delete;HostEntry&operator=(const HostEntry&)=delete;
  RecordingToken tryBegin(const PoolCommandToken&,const Dispatch13&,VkResult originalBeginResult,AdmissionReceipt74* receipt=nullptr)noexcept;
 };
 // Layer creates one per actual device. Failed allocation/thread creation => null;
 // its wrappers continue originals synchronously. No arbitrary completeCoverage bit.
 static std::unique_ptr<WorkerPool28> create(VkDevice,std::uint64_t deviceGeneration)noexcept;
 ~WorkerPool28();WorkerPool28(const WorkerPool28&)=delete;WorkerPool28&operator=(const WorkerPool28&)=delete;
 HostEntry enter(AccessKind kind)noexcept{return HostEntry(*this,kind);}
 // Layer must independently certify resource non-interference. The core checks
 // token/thread/generation and counts the synchronous original call; all packet
 // reservations fail while held. Failure performs one ordinary counted drain.
 HostEntry enterProducerPreparation(const RecordingToken&t)noexcept{return HostEntry(*this,t);}
 void revoke()noexcept;void stop()noexcept;
 // PRECONDITION: all layer application entrypoints returned / owning threads joined.
 // Foreign close acknowledgement alone is NOT this permission. Never call on worker.
 void shutdownAfterApplicationQuiescence()noexcept;
 // Atomic fields are independently sampled; live aggregate is approximate.
 // Exact counter equalities require drained, externally quiescent recording.
 Stats stats()const noexcept;unsigned packetBytes()const noexcept{return sizeof(Packet);}
 std::thread::native_handle_type nativeHandle(){return thread_.native_handle();}
 RDR2_WORKER28_INLINE bool pipeline(const RecordingToken&,VkCommandBuffer,VkPipelineBindPoint,VkPipeline);
 RDR2_WORKER28_INLINE bool descriptors(const RecordingToken&,VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,unsigned,unsigned,const VkDescriptorSet*,unsigned,const unsigned*);
 RDR2_WORKER28_INLINE bool vertices(const RecordingToken&,VkCommandBuffer,unsigned,unsigned,const VkBuffer*,const VkDeviceSize*);
 RDR2_WORKER28_INLINE bool index(const RecordingToken&,VkCommandBuffer,VkBuffer,VkDeviceSize,VkIndexType);
 RDR2_WORKER28_INLINE bool draw(const RecordingToken&,VkCommandBuffer,unsigned,unsigned,unsigned,unsigned);
 RDR2_WORKER28_INLINE bool indexed(const RecordingToken&,VkCommandBuffer,unsigned,unsigned,unsigned,int,unsigned);
 RDR2_WORKER28_INLINE bool barrier(const RecordingToken&,VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,unsigned,const VkMemoryBarrier*,unsigned,const VkBufferMemoryBarrier*,unsigned,const VkImageMemoryBarrier*);
 RDR2_WORKER28_INLINE bool beginPass(const RecordingToken&,VkCommandBuffer,const VkRenderPassBeginInfo*,VkSubpassContents);
 RDR2_WORKER28_INLINE bool endPass(const RecordingToken&,VkCommandBuffer);
 RDR2_WORKER28_INLINE bool viewport(const RecordingToken&,VkCommandBuffer,unsigned,unsigned,const VkViewport*);
 RDR2_WORKER28_INLINE bool scissor(const RecordingToken&,VkCommandBuffer,unsigned,unsigned,const VkRect2D*);
 RDR2_WORKER28_INLINE bool stencil(const RecordingToken&,VkCommandBuffer,VkStencilFaceFlags,unsigned);
 RDR2_WORKER28_INLINE bool copyImage(const RecordingToken&,VkCommandBuffer,VkImage,VkImageLayout,VkImage,VkImageLayout,unsigned,const VkImageCopy*);
 RDR2_WORKER28_INLINE bool resetQueryPool(const RecordingToken&,VkCommandBuffer,VkQueryPool,unsigned,unsigned);
 RDR2_WORKER28_INLINE bool writeTimestamp(const RecordingToken&,VkCommandBuffer,VkPipelineStageFlagBits,VkQueryPool,unsigned);
private:
 friend struct WorkerPool28TestAccess;
 enum class Op:unsigned {Pipeline,Descriptors,Vertices,Index,Draw,Indexed,Barrier,BeginPass,EndPass,Viewport,Scissor,Stencil,CopyImage,ResetQueryPool,WriteTimestamp};
 struct Pipeline {VkPipelineBindPoint point;VkPipeline pipeline;};
 struct Descriptors {VkPipelineBindPoint point;VkPipelineLayout layout;unsigned first,count,n;VkDescriptorSet sets[7];unsigned offsets[16];};
 struct Vertices {unsigned first,count;VkBuffer buffers[8];VkDeviceSize offsets[8];};
 struct Index {VkBuffer buffer;VkDeviceSize offset;VkIndexType type;};
 struct Draw {unsigned count,instances,first;int vertex;unsigned base;};
 struct Barrier {VkPipelineStageFlags src,dst;VkDependencyFlags flags;unsigned nm,nb,ni;VkMemoryBarrier memory[1];VkBufferMemoryBarrier buffers[1];VkImageMemoryBarrier images[1];};
 struct BeginPass {VkRenderPassBeginInfo info;VkClearValue clear[4];VkSubpassContents contents;};
 struct Viewport {unsigned first,count;VkViewport values[4];};
 struct Scissor {unsigned first,count;VkRect2D values[4];};
 struct Stencil {VkStencilFaceFlags face;unsigned reference;};
 struct CopyImage {VkImage src,dst;VkImageLayout srcLayout,dstLayout;unsigned count;VkImageCopy regions[2];};
 struct Query {VkQueryPool pool;unsigned first,count;VkPipelineStageFlagBits stage;};
 union Payload {Pipeline pipeline;Descriptors descriptors;Vertices vertices;Index index;Draw draw;Barrier barrier;BeginPass beginPass;Viewport viewport;Scissor scissor;Stencil stencil;CopyImage copy;Query query;Payload(){}};
 struct alignas(64) Packet {Op op;PFN_vkVoidFunction raw;VkCommandBuffer command;Payload data;};
 static_assert(sizeof(Packet)<=256,"bounded queue packet exceeded 256 bytes");
 static_assert(std::is_trivially_copyable<Payload>::value&&std::is_trivially_copyable<Packet>::value,"packet copy must preserve active trivial alternative");

 static constexpr std::uint64_t PhaseMask=7,Active=8,CountUnit=16,CountMask=((1ull<<20)-1)<<4,GenerationShift=24,MaxGeneration=(1ull<<40)-1;
 static constexpr Phase phaseOf(std::uint64_t w)noexcept{return Phase(w&PhaseMask);}
 static constexpr std::uint64_t generationOf(std::uint64_t w)noexcept{return w>>GenerationShift;}
 static constexpr std::uint64_t countOf(std::uint64_t w)noexcept{return(w&CountMask)>>4;}
 VkDevice device_{};std::uint64_t deviceGeneration_{};
 std::unique_ptr<std::array<Packet,Capacity>> packets_;
 std::uint64_t head_{},cachedTail_{},lastPublished_{}; // fixed producer ONLY
 PoolCommandToken selected_{};Dispatch13 dispatch_{}; // publication before OPEN
 std::atomic<std::uint64_t> producerId_{};
 alignas(64) std::atomic<std::uint64_t> admission_{};
 alignas(64) std::atomic<std::uint64_t> committed_{},goal_{},consumed_{};
 std::atomic<bool> revoked_{},stopRequested_{},producerWaiting_{};
 std::atomic<bool> consumerParking89_{};
 std::mutex mutex_;std::condition_variable ready_,progress_;
 std::uint64_t requestGeneration_{},requestSequence_{},requestTail_{},ackSequence_{};
 bool consumerIdle_{true}; // mutex protected
 std::atomic<std::uint64_t> publications_{},closes_{},entries_{},admissions_{},declines_{},producerSleeps_{},consumerSleeps_{},rejected_{};
 std::thread thread_;
#ifdef RDR2_WORKER28_TESTING
 using TestHook=void(*)(void*,unsigned);TestHook testHook_{};void*testContext_{};
 void testPoint(unsigned n){if(testHook_)testHook_(testContext_,n);}
#else
 void testPoint(unsigned)noexcept{}
#endif
 explicit WorkerPool28(VkDevice,std::uint64_t);
 static std::uint64_t threadIdentity()noexcept;
 static thread_local WorkerPool28* consuming_;
 bool enterHost(bool countEntry=true)noexcept;
 bool countProducerPreparation(const RecordingToken&,bool&)noexcept;
 bool drainProducerPreparation(HostEntry&,const RecordingToken&)noexcept;
 void leaveHost()noexcept;void closeAndWait()noexcept;
 RecordingToken admit(HostEntry&,const PoolCommandToken&,const Dispatch13&,VkResult,AdmissionReceipt74*)noexcept;
 RDR2_WORKER28_INLINE Packet* reserve(const RecordingToken&,VkCommandBuffer)noexcept;
 RDR2_WORKER28_INLINE void commit(bool publishDraw89=false)noexcept;
 Packet* reserveSlow(const RecordingToken&,VkCommandBuffer,std::uint64_t expected)noexcept;
 void publish(std::uint64_t)noexcept;void raiseGoalLocked(std::uint64_t)noexcept;bool raiseGoal89(std::uint64_t)noexcept;
 void run()noexcept;static void execute(Packet&)noexcept;
 bool reject()noexcept{rejected_.fetch_add(1,std::memory_order_relaxed);return false;}
};
}
#include "WorkerPool28.inl"
