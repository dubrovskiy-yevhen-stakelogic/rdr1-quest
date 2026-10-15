#include "NativeCameraRuntime.h"
#include "CameraMetadataActivity.h"
#include <array>
#include <atomic>
#include <cerrno>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>
#include <unordered_map>

namespace rdr95 {
namespace {
struct Errno {int* cell=&errno;int value=*cell;~Errno(){*cell=value;}};
template<class A>auto creationKey(const A&a){return std::tie(a.createShader,a.destroyShader,a.createSetLayout,a.destroySetLayout,a.createLayout,a.destroyLayout,a.createPipelines,a.destroyPipeline);}
template<class A>auto resourceKey(const A&a){return std::tie(a.getProperties,a.getMemoryProperties,a.createBuffer,a.getBufferRequirements,a.allocateMemory,a.bindMemory,a.mapMemory,a.flushMemory,a.unmapMemory,a.destroyBuffer,a.freeMemory,a.createPool,a.destroyPool,a.allocateSets,a.updateSets,a.createLayout,a.destroyLayout);}
auto dispatchKey(const NativeCameraRuntime::Dispatch&a){return std::tie(a.bindPipeline,a.bindDescriptors,a.draw,a.drawIndexed,a.allocateCommands,a.freeCommands,a.beginCommand,a.endCommand,a.resetCommand,a.resetPool,a.destroyPool,a.executeCommands,a.submit);}
}
struct NativeCameraRuntime::Impl {
 struct Command {VkDevice device{};VkCommandPool pool{};VkCommandBuffer handle{};VkCommandBufferLevel level{};std::uint64_t incarnation{},activity{};bool recording{},eligible{};std::thread::id thread{};};
 struct Ticket {unsigned index=MaxCommands;Command command{};explicit operator bool()const{return index<MaxCommands&&command.handle&&command.incarnation;}};
 struct Imported {VkDevice device{};VkCommandPool pool{};VkCommandBuffer handle{};std::uint64_t incarnation{},activity{};};
 mutable std::mutex mutex;
 CameraMetadataActivity metadataActivity,lifecycleActivity;
 Dispatch api{};std::atomic<bool> bound{},ready{},suspended{};
 CameraUniformOwner& uniforms;Registry registry;CameraDrawAdapter adapter;
 std::array<Command,MaxCommands> commands{};
 std::array<Imported,MaxCommands> imported{}; // recording-owner thread only
 std::unordered_map<VkCommandBuffer,unsigned> index;
 // Recording-thread-only memo of a SUCCESSFUL full route check. The combined
 // lifecycle word includes active writers before they acquire the table lock;
 // it is never a resource lease or permission for simultaneous command use.
 VkCommandBuffer routedCommand{};std::uint64_t routedEpoch{};
 VkDevice device{};std::uint64_t generation{},serial{},declines{},foreignCalls{};
 RecordingOwnerThread predicate{};std::thread::id owner{};bool initializing{},initialized{};std::atomic<bool> ownerChosen{};
 explicit Impl(CameraUniformOwner&o):uniforms(o){}
 bool live()const noexcept{return ready.load(std::memory_order_acquire)&&!suspended.load(std::memory_order_acquire);}
 std::uint64_t next()noexcept{if(serial==UINT64_MAX)return 0;return ++serial;}
 Ticket ticket(VkCommandBuffer h)const {const auto it=index.find(h);return it==index.end()?Ticket{}:Ticket{it->second,commands[it->second]};}
 bool same(const Ticket&t)const noexcept{return t&&commands[t.index].handle==t.command.handle&&commands[t.index].incarnation==t.command.incarnation;}
 bool exact(const Ticket&t)const noexcept{return same(t)&&commands[t.index].activity==t.command.activity;}
 bool owning()const noexcept{return ownerChosen.load(std::memory_order_acquire)&&owner==std::this_thread::get_id();}
 void invalidate(Command&c)noexcept{c.eligible=false;c.activity=next();}
 static bool metadata(void*p,VkPipeline h,Registry::DrawMetadata&v)noexcept{auto&s=*static_cast<Impl*>(p);return s.live()&&s.registry.lookupDrawMetadata(h,v)&&s.live();}
 static bool layoutGeneration(void*p,VkPipelineLayout h,std::uint64_t&v)noexcept{auto&s=*static_cast<Impl*>(p);return s.live()&&s.registry.lookupLayoutGeneration(h,v)&&s.live();}
 static std::uint64_t metadataRevision(void*p)noexcept{auto&s=*static_cast<Impl*>(p);return s.live()?s.metadataActivity.current():0;}
 CameraDrawAdapter::Api adapterApi()noexcept{return {api.bindPipeline,api.bindDescriptors,api.draw,api.drawIndexed,this,nullptr,metadata,layoutGeneration,metadataRevision};}
 // Called on selected recording thread only. No table lock spans adapter/raw
 // calls. Native host synchronization excludes concurrent use/free of this cmd.
 bool route(VkCommandBuffer h,std::uint64_t* planEpoch=nullptr)noexcept {
  if(planEpoch)*planEpoch=0;
  const bool callerOwns=owning();
  if(callerOwns){
   const auto cachedEpoch=lifecycleActivity.current();
   if(!live()||!cachedEpoch)return false;
   if(h&&routedCommand==h&&routedEpoch==cachedEpoch){
    if(!live()||!owning()||lifecycleActivity.current()!=cachedEpoch)return false;
    if(planEpoch)*planEpoch=cachedEpoch;
    return true;
   }
  }
  // Foreign routes revoke the memo BEFORE waiting for table access, even if
  // there is no matching command or the owning thread currently holds it.
  std::optional<CameraMetadataActivity::Mutation> foreign;
  if(!callerOwns)foreign.emplace(lifecycleActivity);
  Ticket t;bool currentOwner{};std::uint64_t epoch{};
  try{std::lock_guard lock(mutex);t=ticket(h);currentOwner=owning();
   if(!currentOwner){++foreignCalls;if(t)invalidate(commands[t.index]);return false;}
   if(!live()||!t)return false;
   epoch=lifecycleActivity.current();if(!epoch)return false;
  }catch(...){return false;}
  auto&i=imported[t.index];
  if(i.handle!=h||i.incarnation!=t.command.incarnation)return false;
  if(i.activity!=t.command.activity){adapter.commandUnknown(h);i.activity=t.command.activity;}
  const bool good=t.command.recording&&t.command.eligible&&t.command.thread==std::this_thread::get_id();
  if(!good||!live()||!owning()||lifecycleActivity.current()!=epoch)return false;
  routedCommand=h;routedEpoch=epoch;
  if(planEpoch)*planEpoch=epoch;
  return true;
 }
 void importBegin(const Ticket&t)noexcept {
  auto&i=imported[t.index];
  if(i.handle!=t.command.handle||i.incarnation!=t.command.incarnation){
   if(i.handle)adapter.commandFreed(i.device,i.pool,i.handle);
   adapter.commandAllocated(t.command.device,t.command.pool,t.command.handle,t.command.level,VK_SUCCESS);
   i={t.command.device,t.command.pool,t.command.handle,t.command.incarnation,t.command.activity};
  }
  adapter.commandBegun(t.command.handle,VK_SUCCESS);i.activity=t.command.activity;
 }
 void removeImported(const Ticket&t)noexcept {
  if(!t||!owning())return;auto&i=imported[t.index];
  if(i.handle==t.command.handle&&i.incarnation==t.command.incarnation){adapter.commandFreed(i.device,i.pool,i.handle);i={};}
 }
};
NativeCameraRuntime::Dispatch::operator bool()const noexcept{return creation&&resources&&bindPipeline&&bindDescriptors&&draw&&drawIndexed&&allocateCommands&&freeCommands&&beginCommand&&endCommand&&resetCommand&&resetPool&&destroyPool&&executeCommands&&submit;}
NativeCameraRuntime::NativeCameraRuntime()noexcept{try{impl_=std::make_unique<Impl>(uniforms_);}catch(...){}}
NativeCameraRuntime::~NativeCameraRuntime()=default;
bool NativeCameraRuntime::bindOriginals(Dispatch api)noexcept {
 Errno preserve;try{if(!impl_||!api)return false;auto&s=*impl_;CameraMetadataActivity::Mutation lifecycle(s.lifecycleActivity);std::lock_guard lock(s.mutex);
  if(s.bound.load(std::memory_order_relaxed))return creationKey(s.api.creation)==creationKey(api.creation)&&resourceKey(s.api.resources)==resourceKey(api.resources)&&dispatchKey(s.api)==dispatchKey(api);
  if(!s.registry.bindOriginals(api.creation))return false;s.index.reserve(MaxCommands);s.api=api;s.bound.store(true,std::memory_order_release);return true;
 }catch(...){return false;}
}
bool NativeCameraRuntime::initializeDevice(VkPhysicalDevice physical,VkDevice device,std::uint64_t generation,Limits limits,RecordingOwnerThread predicate)noexcept {
 // This branch uses the existing native EyeViewports and stereo globals.
 // Optional camera variants and their recording consumers stay unavailable.
 (void)physical;(void)device;(void)generation;(void)limits;(void)predicate;return false;
}
#define CAMERA_CREATE(Name,Raw,Params,Args) VkResult NativeCameraRuntime::Name Params noexcept {if(!impl_||!impl_->bound.load(std::memory_order_acquire))return VK_ERROR_INITIALIZATION_FAILED;return impl_->api.creation.Raw Args;}
CAMERA_CREATE(createShader,createShader,(VkDevice d,const VkShaderModuleCreateInfo*c,const VkAllocationCallbacks*a,VkShaderModule*out),(d,c,a,out))
CAMERA_CREATE(createSetLayout,createSetLayout,(VkDevice d,const VkDescriptorSetLayoutCreateInfo*c,const VkAllocationCallbacks*a,VkDescriptorSetLayout*out),(d,c,a,out))
CAMERA_CREATE(createPipelineLayout,createLayout,(VkDevice d,const VkPipelineLayoutCreateInfo*c,const VkAllocationCallbacks*a,VkPipelineLayout*out),(d,c,a,out))
CAMERA_CREATE(createPipelines,createPipelines,(VkDevice d,VkPipelineCache cache,std::uint32_t n,const VkGraphicsPipelineCreateInfo*c,const VkAllocationCallbacks*a,VkPipeline*out),(d,cache,n,c,a,out))
#undef CAMERA_CREATE
#define CAMERA_DESTROY(Name,Raw,Handle) void NativeCameraRuntime::Name(VkDevice d,Handle h,const VkAllocationCallbacks*a)noexcept{if(impl_&&impl_->bound.load(std::memory_order_acquire))impl_->api.creation.Raw(d,h,a);}
CAMERA_DESTROY(destroyShader,destroyShader,VkShaderModule)
CAMERA_DESTROY(destroySetLayout,destroySetLayout,VkDescriptorSetLayout)
CAMERA_DESTROY(destroyPipelineLayout,destroyLayout,VkPipelineLayout)
CAMERA_DESTROY(destroyPipeline,destroyPipeline,VkPipeline)
#undef CAMERA_DESTROY
VkResult NativeCameraRuntime::allocateCommands(VkDevice d,const VkCommandBufferAllocateInfo*c,VkCommandBuffer*out)noexcept {
 if(!impl_||!impl_->bound.load(std::memory_order_acquire))return VK_ERROR_INITIALIZATION_FAILED;return impl_->api.allocateCommands(d,c,out);
}
void NativeCameraRuntime::freeCommands(VkDevice d,VkCommandPool p,std::uint32_t n,const VkCommandBuffer*c)noexcept {
 if(impl_&&impl_->bound.load(std::memory_order_acquire))impl_->api.freeCommands(d,p,n,c);
}
VkResult NativeCameraRuntime::beginCommand(VkCommandBuffer h,const VkCommandBufferBeginInfo*b)noexcept {
 if(!impl_||!impl_->bound.load(std::memory_order_acquire))return VK_ERROR_INITIALIZATION_FAILED;return impl_->api.beginCommand(h,b);
}
VkResult NativeCameraRuntime::endCommand(VkCommandBuffer h)noexcept {
 if(!impl_||!impl_->bound.load(std::memory_order_acquire))return VK_ERROR_INITIALIZATION_FAILED;return impl_->api.endCommand(h);
}
VkResult NativeCameraRuntime::resetCommand(VkCommandBuffer h,VkCommandBufferResetFlags flags)noexcept {
 if(!impl_||!impl_->bound.load(std::memory_order_acquire))return VK_ERROR_INITIALIZATION_FAILED;return impl_->api.resetCommand(h,flags);
}
VkResult NativeCameraRuntime::resetPool(VkDevice d,VkCommandPool p,VkCommandPoolResetFlags flags)noexcept {
 if(!impl_||!impl_->bound.load(std::memory_order_acquire))return VK_ERROR_INITIALIZATION_FAILED;return impl_->api.resetPool(d,p,flags);
}
void NativeCameraRuntime::destroyPool(VkDevice d,VkCommandPool p,const VkAllocationCallbacks*a)noexcept {
 if(impl_&&impl_->bound.load(std::memory_order_acquire))impl_->api.destroyPool(d,p,a);
}
VkResult NativeCameraRuntime::submit(VkQueue q,std::uint32_t n,const VkSubmitInfo*c,VkFence f)noexcept {
 if(!impl_||!impl_->bound.load(std::memory_order_acquire))return VK_ERROR_INITIALIZATION_FAILED;return impl_->api.submit(q,n,c,f);
}
void NativeCameraRuntime::commandUnknown(VkCommandBuffer h)noexcept {
 // No optional recording consumer exists in this branch.
 (void)h;
}
void NativeCameraRuntime::executeCommands(VkCommandBuffer h,std::uint32_t n,const VkCommandBuffer*c)noexcept {
 if(impl_&&impl_->bound.load(std::memory_order_acquire))impl_->api.executeCommands(h,n,c);
}
void NativeCameraRuntime::bindPipeline(VkCommandBuffer h,VkPipelineBindPoint p,VkPipeline v)noexcept {
 if(impl_&&impl_->bound.load(std::memory_order_acquire))impl_->api.bindPipeline(h,p,v);
}
void NativeCameraRuntime::bindDescriptors(VkCommandBuffer h,VkPipelineBindPoint p,VkPipelineLayout l,std::uint32_t first,std::uint32_t n,const VkDescriptorSet*sets,std::uint32_t nd,const std::uint32_t*offsets)noexcept {
 if(impl_&&impl_->bound.load(std::memory_order_acquire))impl_->api.bindDescriptors(h,p,l,first,n,sets,nd,offsets);
}
bool NativeCameraRuntime::lookupDrawMetadata(VkPipeline h,Registry::DrawMetadata& out)noexcept {
 Errno preserve;if(!impl_||!impl_->live())return false;Registry::DrawMetadata value{};
 if(!impl_->registry.lookupDrawMetadata(h,value)||!impl_->live())return false;out=value;return true;
}
bool NativeCameraRuntime::lookupRecording(VkCommandBuffer h,RecordingIdentity& out)noexcept {
 Errno preserve;if(!impl_||!impl_->live()||!impl_->owning())return false;auto&s=*impl_;
 try{std::lock_guard lock(s.mutex);const auto t=s.ticket(h);
  if(!s.live()||!t||!t.command.recording||!t.command.eligible||t.command.thread!=std::this_thread::get_id())return false;
  const auto&i=s.imported[t.index];
  if(i.handle!=h||i.incarnation!=t.command.incarnation||i.activity!=t.command.activity)return false;
  out={t.command.device,t.command.pool,h,s.generation,t.command.incarnation,t.command.activity};return true;
 }catch(...){return false;}
}
bool NativeCameraRuntime::lookupReplayMetadata(VkPipeline h,Registry::ReplayMetadata& out)noexcept {
 Errno preserve;if(!impl_||!impl_->live())return false;const auto before=impl_->metadataActivity.current();
 Registry::ReplayMetadata value{};
 if(!before||!impl_->registry.lookupReplayMetadata(h,value)||!impl_->live()||before!=impl_->metadataActivity.current())return false;
 out=value;return true;
}
bool NativeCameraRuntime::lookupPlainDepthCertificate(VkPipeline h,Registry::PlainDepthCertificate& out)noexcept {
 Errno preserve;if(!impl_||!impl_->live()||!impl_->owning())return false;const auto before=impl_->metadataActivity.current();
 Registry::PlainDepthCertificate value{};
 if(!before||!impl_->registry.lookupPlainDepthCertificate(h,value)||!impl_->live()||!impl_->owning()||before!=impl_->metadataActivity.current())return false;
 out=value;return true;
}
bool NativeCameraRuntime::lookupLayoutGeneration(VkPipelineLayout h,std::uint64_t& out)noexcept {
 Errno preserve;if(!impl_||!impl_->live())return false;const auto before=impl_->metadataActivity.current();std::uint64_t value{};
 if(!before||!impl_->registry.lookupLayoutGeneration(h,value)||!impl_->live()||before!=impl_->metadataActivity.current())return false;
 out=value;return true;
}
std::uint64_t NativeCameraRuntime::metadataRevision()const noexcept {
 Errno preserve;return impl_&&impl_->live()?impl_->metadataActivity.current():0;
}
NativeCameraRuntime::OwnedBindResult NativeCameraRuntime::bindOwnedDescriptors(VkCommandBuffer h,VkPipeline intended,VkPipelineLayout restore,
 std::uint32_t first,std::uint32_t count,const VkDescriptorSet* sets,std::uint32_t n,const std::uint32_t* offsets)noexcept {
 const int incoming=errno;if(!impl_||!impl_->bound.load(std::memory_order_acquire))return OwnedBindResult::Declined;
 auto&s=*impl_;const bool routed=s.route(h);errno=incoming;if(!routed)return OwnedBindResult::Declined;
 return s.adapter.bindOwnedDescriptors(h,intended,restore,first,count,sets,n,offsets);
}
NativeCameraRuntime::PacketResult NativeCameraRuntime::withIndexedPacketPlan(VkCommandBuffer h,std::span<const PacketTuple> parts,const PacketTuple& incoming,
 void* context,bool(*emit)(void*,IndexedPacketPlan&)noexcept)noexcept {
 return withIndexedPlanImpl(h,parts,incoming,context,emit,false,nullptr);
}
NativeCameraRuntime::PacketResult NativeCameraRuntime::withIndexedSequencePlan(VkCommandBuffer h,std::span<const PacketTuple> parts,const PacketTuple& incoming,
 void* context,bool(*emit)(void*,IndexedPacketPlan&)noexcept,bool(*mirrorCurrent)(void*)noexcept)noexcept {
 if(!mirrorCurrent)return PacketResult::Declined;
 return withIndexedPlanImpl(h,parts,incoming,context,emit,true,mirrorCurrent);
}
NativeCameraRuntime::PacketResult NativeCameraRuntime::withIndexedPlanImpl(VkCommandBuffer h,std::span<const PacketTuple> parts,const PacketTuple& incoming,
 void* context,bool(*emit)(void*,IndexedPacketPlan&)noexcept,bool sequence,bool(*mirrorCurrent)(void*)noexcept)noexcept {
 Errno preserve;if(!impl_||!emit)return PacketResult::Declined;auto&s=*impl_;std::uint64_t epoch{};
 if(!s.route(h,&epoch))return PacketResult::Declined;
 const auto stamp=CameraMainRoute::packetStamp();CameraDrawAdapter::Request request{};
 if(!CameraMainRoute::request(h,request)||!CameraMainRoute::packetCurrent(stamp))return PacketResult::Declined;
 // route checked the full command/imported ticket once under the table lock.
 // Every later mutation changes the combined epoch before touching that table
 // or calling the native lifecycle function. No mutable table fields are read
 // by this lexical predicate. External Vulkan lifetime synchronization remains
 // required: the epoch is revocation, not a drain or a resource lease.
 struct Lifetime {Impl* impl;std::uint64_t epoch;CameraMainRoute::PacketScopeStamp stamp;void* mirrorContext;bool(*mirrorCurrent)(void*)noexcept;
  static bool current(void* p)noexcept{auto&v=*static_cast<Lifetime*>(p);auto&s=*v.impl;
   return s.live()&&s.owning()&&CameraMainRoute::packetCurrent(v.stamp)&&
          (!v.mirrorCurrent||v.mirrorCurrent(v.mirrorContext))&&
          s.live()&&s.owning()&&CameraMainRoute::packetCurrent(v.stamp)&&
          v.epoch&&s.lifecycleActivity.current()==v.epoch;}
 } lifetime{&s,epoch,stamp,context,mirrorCurrent};
 return s.adapter.withIndexedPlanImpl(h,parts,incoming,request,&lifetime,Lifetime::current,context,emit,sequence);
}
bool NativeCameraRuntime::draw(VkCommandBuffer h,std::uint32_t n,std::uint32_t instances,std::uint32_t first,std::uint32_t base)noexcept {
 if(impl_&&impl_->bound.load(std::memory_order_acquire))impl_->api.draw(h,n,instances,first,base);return false;
}
bool NativeCameraRuntime::drawIndexed(VkCommandBuffer h,std::uint32_t n,std::uint32_t instances,std::uint32_t first,std::int32_t vertex,std::uint32_t base)noexcept {
 if(impl_&&impl_->bound.load(std::memory_order_acquire))impl_->api.drawIndexed(h,n,instances,first,vertex,base);return false;
}
CameraUniformOwner&NativeCameraRuntime::uniforms()noexcept{return uniforms_;}
bool NativeCameraRuntime::ready()const noexcept{return impl_&&impl_->live();}
void NativeCameraRuntime::disarm()noexcept{Errno preserve;if(impl_){CameraMetadataActivity::Mutation lifecycle(impl_->lifecycleActivity);impl_->suspended.store(true,std::memory_order_release);impl_->ready.store(false,std::memory_order_release);impl_->adapter.disarm();}}
NativeCameraRuntime::Stats NativeCameraRuntime::stats()const noexcept{Errno preserve;Stats out;try{if(!impl_)return out;auto&s=*impl_;std::lock_guard lock(s.mutex);out.allocations=static_cast<unsigned>(s.index.size());out.allocationSerial=s.serial;out.declines=s.declines;out.foreignCalls=s.foreignCalls;out.deviceReady=s.live();out.recordingOwnerChosen=s.ownerChosen;if(s.owning())for(const auto&i:s.imported)out.importedCommands+=bool(i.handle);}catch(...){}return out;}
bool NativeCameraRuntime::shutdownAfterIdle(VkDevice d,VkResult r)noexcept{Errno preserve;if(!impl_||!d||d!=impl_->device||(r!=VK_SUCCESS&&r!=VK_ERROR_DEVICE_LOST))return false;auto&s=*impl_;CameraMetadataActivity::Mutation lifecycle(s.lifecycleActivity);s.ready.store(false,std::memory_order_release);s.adapter.disarm();s.registry.shutdownAfterIdle(d,r);const bool done=uniforms_.shutdownAfterIdle(d,r);try{std::lock_guard lock(s.mutex);s.index.clear();s.commands={};}catch(...){}return done;}
void NativeCameraRuntime::abandonDevice(VkDevice d)noexcept{Errno preserve;if(!impl_||!d||d!=impl_->device)return;auto&s=*impl_;CameraMetadataActivity::Mutation lifecycle(s.lifecycleActivity);s.ready.store(false,std::memory_order_release);s.adapter.disarm();s.registry.abandon(d);uniforms_.abandonDevice(d);try{std::lock_guard lock(s.mutex);s.index.clear();s.commands={};}catch(...){} }
}
