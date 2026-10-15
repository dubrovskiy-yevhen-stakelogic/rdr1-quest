#pragma once
#include "IndexedCommandState.h"
#include "NativeCameraRuntime.h"
#include <atomic>
#include <memory>
#include <thread>

namespace rdr95 {
// Actual native state observations only. No drawable admission or CPU skip.
// OFF is a single atomic branch, including before metadata-generation reads.
// bindOriginals/arm run once before the first native recording. All driver
// pointers bypass these observers and the private camera temporary binds.
class NativeIndexedObservation final {
public:
 using Mirror=rdrvr::engine::IndexedCommandState;
 explicit NativeIndexedObservation(NativeCameraRuntime&)noexcept;
 ~NativeIndexedObservation();
 NativeIndexedObservation(const NativeIndexedObservation&)=delete;
 NativeIndexedObservation&operator=(const NativeIndexedObservation&)=delete;
 bool bindOriginals(Mirror::Api)noexcept;
 bool arm()noexcept; // Called only by a complete, cold dispatch installation.
 void disarm(const char* reason="unspecified")noexcept;
 const char* disarmReason()const noexcept {auto p=disarmReason_.load(std::memory_order_acquire);return p?p:"NONE";}
 bool enabled()const noexcept{return armed_.load(std::memory_order_acquire);}
 unsigned lifecycleBits()const noexcept {
  return unsigned(bound_.load(std::memory_order_acquire))|unsigned(armed_.load(std::memory_order_acquire))<<1|
   unsigned(terminal_.load(std::memory_order_acquire))<<2|unsigned(ownerChosen_.load(std::memory_order_acquire))<<3;
 }
 void begun(VkCommandBuffer,VkResult)noexcept;
 // Call BEFORE end/reset/free/pool changes, even if the native call fails.
 // This invalidates CPU observation only; it releases no GPU storage.
 void invalidate(VkCommandBuffer)noexcept;
 void invalidateAll()noexcept;
 // Commands not represented by an indexed packet advance this observation
 // counter. A future ordinary recorder must decline if it changes in scope.
 void sideEffect(VkCommandBuffer)noexcept;
 std::uint64_t effects()const noexcept{return effects_.load(std::memory_order_acquire);}
 class CallStamp final {
  friend class NativeIndexedObservation;
  VkCommandBuffer command_{};std::uint64_t serial_{},revocation_{};
 public:
  explicit operator bool()const noexcept{return command_&&serial_;}
 };
 // Exact before/after guard for raw native calls. Nested observations or a
 // same-handle Begin cannot publish outer arguments into the newer state.
 CallStamp beginCall(VkCommandBuffer)noexcept;
 bool finishCall(const CallStamp&)noexcept;
 std::uint64_t layoutBefore(VkCommandBuffer,VkPipelineLayout)noexcept;
 void pipeline(VkCommandBuffer,VkPipelineBindPoint,VkPipeline)noexcept;
 void descriptors(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,std::uint64_t before,
                  std::uint32_t first,std::uint32_t count,const VkDescriptorSet*,
                  std::uint32_t offsetCount,const std::uint32_t*)noexcept;
 void vertices(VkCommandBuffer,std::uint32_t,std::uint32_t,const VkBuffer*,const VkDeviceSize*)noexcept;
 void index(VkCommandBuffer,VkBuffer,VkDeviceSize,VkIndexType)noexcept;
 void viewport(VkCommandBuffer,std::uint32_t,std::uint32_t,const VkViewport*)noexcept;
 void scissor(VkCommandBuffer,std::uint32_t,std::uint32_t,const VkRect2D*)noexcept;
 void lineWidth(VkCommandBuffer,float)noexcept;
 void depthBias(VkCommandBuffer,float,float,float)noexcept;
 void blendConstants(VkCommandBuffer,const float*)noexcept;
 void depthBounds(VkCommandBuffer,float,float)noexcept;
 void stencilCompare(VkCommandBuffer,VkStencilFaceFlags,std::uint32_t)noexcept;
 void stencilWrite(VkCommandBuffer,VkStencilFaceFlags,std::uint32_t)noexcept;
 void stencilReference(VkCommandBuffer,VkStencilFaceFlags,std::uint32_t)noexcept;
 void depthBoundsEnable(VkCommandBuffer,VkBool32)noexcept;
 void push(VkCommandBuffer,VkPipelineLayout,std::uint64_t before,VkShaderStageFlags,std::uint32_t,std::uint32_t,const void*)noexcept;
 void beginPass(VkCommandBuffer,const VkRenderPassBeginInfo*,VkSubpassContents)noexcept;
 void endPass(VkCommandBuffer)noexcept;
 void unknownState(VkCommandBuffer)noexcept;
 // Recording-thread access for the separately qualified ordinary producer.
 // This pointer is never permission to bypass a native drawable.
 Mirror* current(VkCommandBuffer)noexcept;
private:
 struct Cache{VkPipeline pipeline{};std::uint64_t revision{};bool imported{};};
 NativeCameraRuntime& runtime_;Mirror::Api api_{};
 std::unique_ptr<Mirror> mirror_;
 std::array<Cache,1024> cache_{};
 std::atomic<const char*> disarmReason_{};
 std::atomic<bool> bound_{},armed_{},terminal_{},ownerChosen_{};
 std::atomic<VkCommandBuffer> active_{};
 std::atomic<std::uint64_t> revocation_{1},effects_{1};
 std::thread::id owner_{};std::uint64_t observed_{},deviceGeneration_{},callSerial_{};VkDevice device_{};
 bool owner()const noexcept;
 static void advance(std::atomic<std::uint64_t>&)noexcept;
 bool import(VkPipeline)noexcept;
};
}
