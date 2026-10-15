#include "NativeIndexedObservation.h"
#include <cerrno>
#include <new>
#include <tuple>

namespace rdr95 {
namespace {
struct Errno{int value=errno;~Errno(){errno=value;}};
auto apiKey(const NativeIndexedObservation::Mirror::Api&a)noexcept{
 return std::tie(a.pipeline,a.descriptors,a.vertices,a.indices,a.viewport,a.scissor,a.lineWidth,a.depthBias,
  a.blendConstants,a.depthBounds,a.stencilCompare,a.stencilWrite,a.stencilReference,a.depthBoundsEnable,a.push,a.draw);
}
}
NativeIndexedObservation::NativeIndexedObservation(NativeCameraRuntime&r)noexcept:runtime_(r){}
NativeIndexedObservation::~NativeIndexedObservation()=default;
void NativeIndexedObservation::advance(std::atomic<std::uint64_t>&value)noexcept{
 auto n=value.load(std::memory_order_relaxed);while(n!=UINT64_MAX&&!value.compare_exchange_weak(n,n+1,std::memory_order_acq_rel)){}
}
bool NativeIndexedObservation::bindOriginals(Mirror::Api api)noexcept{
 Errno keep;
 if(bound_.load(std::memory_order_acquire))return apiKey(api_)==apiKey(api);
 if(terminal_.load(std::memory_order_acquire)||!api.pipeline||!api.descriptors||!api.vertices||!api.indices||
    !api.viewport||!api.scissor||!api.lineWidth||!api.depthBias||!api.blendConstants||!api.depthBounds||
    !api.stencilCompare||!api.stencilWrite||!api.stencilReference||!api.push||!api.draw)return false;
 // Optional extended-dynamic-state entry is absent on engines that never
 // request it. Such a PSO still declines emission if its function is missing.
 api_=api;bound_.store(true,std::memory_order_release);return true;
}
bool NativeIndexedObservation::arm()noexcept{
 Errno keep;if(!bound_.load(std::memory_order_acquire)||terminal_.load(std::memory_order_acquire)||ownerChosen_.load(std::memory_order_acquire))return false;
 armed_.store(true,std::memory_order_release);return true;
}
void NativeIndexedObservation::disarm(const char* reason)noexcept{
 const char* empty=nullptr;disarmReason_.compare_exchange_strong(empty,reason,std::memory_order_acq_rel);
 Errno keep;terminal_.store(true,std::memory_order_release);armed_.store(false,std::memory_order_release);advance(revocation_);
}
bool NativeIndexedObservation::owner()const noexcept{return ownerChosen_.load(std::memory_order_acquire)&&owner_==std::this_thread::get_id();}
void NativeIndexedObservation::begun(VkCommandBuffer command,VkResult result)noexcept{
 Errno keep;if(!enabled())return;
 if(result!=VK_SUCCESS){invalidate(command);return;}
 NativeCameraRuntime::RecordingIdentity id;
 if(!runtime_.lookupRecording(command,id)){invalidate(command);return;}
 if(ownerChosen_.load(std::memory_order_acquire)&&!owner()){invalidate(command);return;}
 if(callSerial_==UINT64_MAX){disarm("NativeIndexedObservation.cpp:44");return;}++callSerial_;
 active_.store(VK_NULL_HANDLE,std::memory_order_release);
 if(!mirror_){
  // Runtime chose this exact recording thread. UI/device creation never
  // constructs the thread-affine mirror or invents a command generation.
  mirror_.reset(new(std::nothrow) Mirror(id.device,id.deviceGeneration));
  if(!mirror_){disarm("NativeIndexedObservation.cpp:50");return;}
  owner_=std::this_thread::get_id();device_=id.device;deviceGeneration_=id.deviceGeneration;
  ownerChosen_.store(true,std::memory_order_release);
 }
 if(device_!=id.device||deviceGeneration_!=id.deviceGeneration){disarm("NativeIndexedObservation.cpp:54");return;}
 observed_=revocation_.load(std::memory_order_acquire);
 if(observed_==UINT64_MAX||!mirror_->begin({id.device,id.pool,id.command,id.allocation,id.recording,id.deviceGeneration},api_))return;
 active_.store(command,std::memory_order_release);
}
void NativeIndexedObservation::invalidate(VkCommandBuffer command)noexcept{
 Errno keep;if(enabled()&&command&&active_.load(std::memory_order_acquire)==command)advance(revocation_);
}
void NativeIndexedObservation::invalidateAll()noexcept{Errno keep;if(enabled())advance(revocation_);}
void NativeIndexedObservation::sideEffect(VkCommandBuffer command)noexcept{
 Errno keep;if(enabled()&&command&&active_.load(std::memory_order_acquire)==command)advance(effects_);
}
NativeIndexedObservation::Mirror* NativeIndexedObservation::current(VkCommandBuffer command)noexcept{
 Errno keep;if(!enabled()||!command||active_.load(std::memory_order_acquire)!=command)return nullptr;
 if(!owner()){invalidate(command);return nullptr;}
 if(!mirror_||!runtime_.metadataRevision()||observed_!=revocation_.load(std::memory_order_acquire)||observed_==UINT64_MAX){
  if(mirror_)mirror_->unsupported(command);return nullptr;
 }
 return mirror_->good()?mirror_.get():nullptr;
}
NativeIndexedObservation::CallStamp NativeIndexedObservation::beginCall(VkCommandBuffer c)noexcept{
 Errno keep;CallStamp out;if(!current(c))return out;
 if(callSerial_==UINT64_MAX){disarm("NativeIndexedObservation.cpp:76");return out;}
 out.command_=c;out.serial_=++callSerial_;out.revocation_=observed_;return out;
}
bool NativeIndexedObservation::finishCall(const CallStamp& stamp)noexcept{
 Errno keep;if(!stamp)return false;
 if(!current(stamp.command_)||stamp.serial_!=callSerial_||stamp.revocation_!=observed_||callSerial_==UINT64_MAX){
  invalidate(stamp.command_);return false;
 }
 ++callSerial_;return true;
}
bool NativeIndexedObservation::import(VkPipeline pipeline)noexcept{
 const auto revision=runtime_.metadataRevision();if(!pipeline||!revision)return false;
 const auto key=reinterpret_cast<std::uintptr_t>(pipeline);
 const unsigned first=static_cast<unsigned>((key>>3)^(key>>17))&(cache_.size()-1);
 Cache* entry=nullptr;
 for(unsigned i=0;i<cache_.size();++i){auto&c=cache_[(first+i)&(cache_.size()-1)];
  if(c.pipeline==pipeline){entry=&c;break;}if(!c.pipeline){entry=&c;break;}}
 if(!entry)return false;
 if(entry->pipeline==pipeline&&entry->revision==revision)return entry->imported;
 NativeCameraRuntime::Registry::ReplayMetadata value;
 const bool ok=runtime_.lookupReplayMetadata(pipeline,value)&&runtime_.metadataRevision()==revision&&mirror_->importReplayMetadata(value)&&runtime_.metadataRevision()==revision;
 *entry={pipeline,revision,ok};return ok;
}
void NativeIndexedObservation::pipeline(VkCommandBuffer command,VkPipelineBindPoint point,VkPipeline pipeline)noexcept{
 Errno keep;if(auto*m=current(command)){
  if(point!=VK_PIPELINE_BIND_POINT_GRAPHICS){sideEffect(command);return;}
  const bool ok=import(pipeline);m->bindPipeline(command,point,ok?pipeline:VK_NULL_HANDLE);
 }
}
std::uint64_t NativeIndexedObservation::layoutBefore(VkCommandBuffer command,VkPipelineLayout layout)noexcept{
 Errno keep;if(!current(command))return 0;std::uint64_t generation{};
 return runtime_.lookupLayoutGeneration(layout,generation)?generation:0;
}
void NativeIndexedObservation::descriptors(VkCommandBuffer c,VkPipelineBindPoint p,VkPipelineLayout l,std::uint64_t before,
 std::uint32_t first,std::uint32_t n,const VkDescriptorSet* sets,std::uint32_t count,const std::uint32_t* offsets)noexcept{
 Errno keep;if(auto*m=current(c)){
  if(n>Mirror::maxSets||count>Mirror::maxSets*Mirror::maxOffsets||(n&&!sets)||(count&&!offsets)){m->unsupported(c);return;}
  std::uint64_t after{};runtime_.lookupLayoutGeneration(l,after);
  m->bindDescriptorsCurrent(c,p,l,before,after,first,{sets,n},{offsets,count});
 }
}
void NativeIndexedObservation::vertices(VkCommandBuffer c,std::uint32_t first,std::uint32_t n,const VkBuffer*b,const VkDeviceSize*o)noexcept{
 Errno keep;if(auto*m=current(c)){if(n>Mirror::maxBindings||(n&&(!b||!o))){m->unsupported(c);return;}m->bindVertices(c,first,{b,n},{o,n});}
}
void NativeIndexedObservation::index(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkIndexType t)noexcept{Errno keep;if(auto*m=current(c))m->bindIndex(c,b,o,t);}
void NativeIndexedObservation::viewport(VkCommandBuffer c,std::uint32_t f,std::uint32_t n,const VkViewport*v)noexcept{
 Errno keep;if(auto*m=current(c)){if(n!=1||!v){m->unsupported(c);return;}m->setViewport(c,f,{v,n});}
}
void NativeIndexedObservation::scissor(VkCommandBuffer c,std::uint32_t f,std::uint32_t n,const VkRect2D*v)noexcept{
 Errno keep;if(auto*m=current(c)){if(n!=1||!v){m->unsupported(c);return;}m->setScissor(c,f,{v,n});}
}
void NativeIndexedObservation::lineWidth(VkCommandBuffer c,float a)noexcept{Errno keep;if(auto*m=current(c))m->setLineWidth(c,a);}
void NativeIndexedObservation::depthBias(VkCommandBuffer c,float a,float b,float d)noexcept{Errno keep;if(auto*m=current(c))m->setDepthBias(c,a,b,d);}
void NativeIndexedObservation::blendConstants(VkCommandBuffer c,const float*a)noexcept{Errno keep;if(auto*m=current(c))m->setBlendConstants(c,a);}
void NativeIndexedObservation::depthBounds(VkCommandBuffer c,float a,float b)noexcept{Errno keep;if(auto*m=current(c))m->setDepthBounds(c,a,b);}
void NativeIndexedObservation::stencilCompare(VkCommandBuffer c,VkStencilFaceFlags f,std::uint32_t a)noexcept{Errno keep;if(auto*m=current(c))m->setStencilCompare(c,f,a);}
void NativeIndexedObservation::stencilWrite(VkCommandBuffer c,VkStencilFaceFlags f,std::uint32_t a)noexcept{Errno keep;if(auto*m=current(c))m->setStencilWrite(c,f,a);}
void NativeIndexedObservation::stencilReference(VkCommandBuffer c,VkStencilFaceFlags f,std::uint32_t a)noexcept{Errno keep;if(auto*m=current(c))m->setStencilReference(c,f,a);}
void NativeIndexedObservation::depthBoundsEnable(VkCommandBuffer c,VkBool32 a)noexcept{Errno keep;if(auto*m=current(c))m->setDepthBoundsEnable(c,a);}
void NativeIndexedObservation::push(VkCommandBuffer c,VkPipelineLayout l,std::uint64_t before,VkShaderStageFlags f,std::uint32_t o,std::uint32_t n,const void*b)noexcept{
 Errno keep;if(auto*m=current(c)){if(n>Mirror::maxPushBytes||(n&&!b)){m->unsupported(c);return;}std::uint64_t after{};runtime_.lookupLayoutGeneration(l,after);
  m->pushConstantsCurrent(c,l,before,after,f,o,{static_cast<const std::uint8_t*>(b),n});}
}
void NativeIndexedObservation::beginPass(VkCommandBuffer c,const VkRenderPassBeginInfo*b,VkSubpassContents contents)noexcept{
 Errno keep;sideEffect(c);if(auto*m=current(c)){
  if(!b||b->sType!=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO||b->pNext||contents!=VK_SUBPASS_CONTENTS_INLINE){m->unsupported(c);return;}
  m->beginPass(c,b->renderPass,0);
 }
}
void NativeIndexedObservation::endPass(VkCommandBuffer c)noexcept{Errno keep;sideEffect(c);if(auto*m=current(c))m->endPass(c);}
void NativeIndexedObservation::unknownState(VkCommandBuffer c)noexcept{Errno keep;sideEffect(c);invalidate(c);}
}
