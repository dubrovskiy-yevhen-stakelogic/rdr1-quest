#include "LayerInternal.h"
#include "WorkerDiagnosticLog82.h"
#include "ChainQualification.h"
#include "../allocator/RetailAllocatorQualification.h"
#if defined(_WIN32) && defined(RDR2_WORKER28_STAND_CALLBACKS) && !defined(RDR2_LAYER28_TESTING)
#include "../callback/StandCallbackQualification.h"
#endif
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#include <cstdio>
#include <cerrno>
#include <climits>
#ifdef __ANDROID__
#include <android/log.h>
#include <unistd.h>
#include <time.h>
#include <sys/system_properties.h>
#endif
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL rdrWorker28GetPhysicalDeviceProcAddr(VkInstance,const char*);

namespace rdr2::layer28 {
// tls is constant-initialised and trivially destructible (see LayerInternal.h),
// so no thread-local initialisation routine is emitted or called per access.
RDR2_LAYER_CONSTINIT thread_local ThreadContext tls;
RDR2_LAYER_HIDDEN std::atomic<bool> passthrough90{false};
RDR2_LAYER_HIDDEN std::atomic<Device*> passthroughCache90{nullptr};
namespace {
bool activeMetadata81(ThreadContext&,Device*,const Token&,std::uint64_t)noexcept;
bool activeMetadata81(Device*,const Token&,std::uint64_t)noexcept;
bool originalBoundaryApi89(Worker::AccessKind kind,const char* api)noexcept {
 if(!api)return false;
 if(kind==Worker::AccessKind::Resource)
  return std::strcmp(api,"vkAllocateDescriptorSets")==0||std::strcmp(api,"vkUpdateDescriptorSets")==0;
 if(kind!=Worker::AccessKind::Command)return false;
 // Only existing command wrappers with known recording-state behavior.
 static constexpr const char* allowed[]={"vkCmdBindPipeline","vkCmdBindDescriptorSets","vkCmdBindVertexBuffers",
  "vkCmdBindIndexBuffer","vkCmdPipelineBarrier","vkCmdSetViewport","vkCmdSetScissor","vkCmdSetStencilReference",
  "vkCmdCopyImage","vkCmdResetQueryPool","vkCmdWriteTimestamp"};
 for(auto* name:allowed)if(std::strcmp(api,name)==0)return true;
 return false;
}
}

namespace {
constexpr const char* Name="VK_LAYER_RDR2_raw_worker28";
std::mutex registryMutex,configMutex;
// Mandatory dispatch nodes are allocated and linked BEFORE native creation.
// Publishing a successful handle requires no allocation and cannot change the
// original success result because optional tracking ran out of memory.
std::list<std::shared_ptr<Instance>> instances;
std::list<std::shared_ptr<Device>> devices;
// Destroyed devices are retired here for the process lifetime (bounded by
// the number of vkCreateDevice calls) so raw Device pointers held by thread
// contexts and by the pass-through cache always stay dereferenceable.
std::list<std::shared_ptr<Device>> retiredDevices;
std::atomic<std::uint64_t> nextDevice{1},nextThread{1};
bool configured{},enabled{},instanceStarted{};
void creationDiagnostic(const char*kind,std::uint32_t reason,const void*chain,unsigned layerCount,const char*const*layers,unsigned extensionCount,const char*const*extensions,bool allocator)noexcept{
 static std::atomic<unsigned>emitted{};if(emitted.fetch_add(1)>=32)return;
 std::fprintf(stderr,"WORKER28_CREATE kind=%s qualified=0 reason=%u customAllocator=%u layers=",kind,reason,unsigned(allocator));
 for(unsigned n=0;n<layerCount&&n<128;++n){std::fprintf(stderr,"%s%s",n?",":"",layers[n]);}std::fprintf(stderr," extensions=");
 for(unsigned n=0;n<extensionCount&&n<128;++n){std::fprintf(stderr,"%s%s",n?",":"",extensions[n]);}std::fprintf(stderr," chain=");
 auto*b=static_cast<const VkBaseInStructure*>(chain);for(unsigned n=0;b&&n<128;++n,b=b->pNext)std::fprintf(stderr,"%s%u",n?",":"",unsigned(b->sType));std::fprintf(stderr,"\n");
#ifdef __ANDROID__
 __android_log_print(ANDROID_LOG_INFO,"RdrWorker28","create %s declined reason=%u customAllocator=%u layerCount=%u extCount=%u",kind,reason,unsigned(allocator),layerCount,extensionCount);
#endif
}
#ifdef RDR2_LAYER28_TESTING
std::atomic<bool> failOptionalMetadata{};
#endif
void optionalAllocationPoint(){
#ifdef RDR2_LAYER28_TESTING
 if(failOptionalMetadata.exchange(false))throw std::bad_alloc{};
#endif
}
struct NamedEntry {const char*name;PFN_vkVoidFunction function;bool device,physical;};
const NamedEntry entries[]={
#include "GeneratedLookup.inc"
};
const NamedEntry*lookup(const char*n)noexcept{
 if(!n)return nullptr;auto first=std::begin(entries),last=std::end(entries);
 auto it=std::lower_bound(first,last,n,[](const auto&e,const char*s){return std::strcmp(e.name,s)<0;});
 return it!=last&&std::strcmp(it->name,n)==0?it:nullptr;
}
std::uint64_t identity(ThreadContext&t)noexcept{
 if(!t.identity){auto n=nextThread.fetch_add(1,std::memory_order_relaxed);if(!n||n==UINT64_MAX)std::terminate();t.identity=n;}return t.identity;
}
inline std::uint64_t identity()noexcept{return identity(tls);}
// The existing counted Begin owns the drain. No new recording capability is
// created here; a mode decision precedes the original first-admission path.
struct BeginDepth84 final {
 BeginDepth84()noexcept{if(tls.beginDepth84==UINT_MAX)std::terminate();++tls.beginDepth84;}
 ~BeginDepth84(){--tls.beginDepth84;}
};
struct ModeInput84 {unsigned pid{};std::uint64_t ms{};std::array<char,92> property{};};
#ifdef RDR2_LAYER28_TESTING
ModeInput84 testModeInput84;
#endif
ModeInput84 modeInput84()noexcept{
 const int saved=errno;ModeInput84 input{};
#ifdef RDR2_LAYER28_TESTING
 input=testModeInput84;
#elif defined(__ANDROID__)
 input.pid=unsigned(getpid());timespec now{};
 if(clock_gettime(CLOCK_BOOTTIME,&now)==0&&now.tv_sec>0&&now.tv_nsec>=0&&now.tv_nsec<1000000000&&
    std::uint64_t(now.tv_sec)<UINT64_MAX/1000)input.ms=std::uint64_t(now.tv_sec)*1000+std::uint64_t(now.tv_nsec)/1000000;
 static_assert(PROP_VALUE_MAX<=92);__system_property_get("debug.rdrvr.worker_mode",input.property.data());
#endif
 errno=saved;return input;
}
struct ModeTicket84 {Token token{};std::uint64_t entries{},previousRecording{};bool valid{};};
bool modeIdle84(const Worker::Stats&s)noexcept{
 return (s.phase==Worker::Phase::Off||s.phase==Worker::Phase::Synchronous)&&
  s.hostInflight==1&&s.synchronousEntries&&s.synchronousEntries!=UINT64_MAX&&
  s.enqueued==s.executed&&!s.permanentlyRevoked&&!s.stopped;
}
ModeTicket84 snapshotMode84(Device*d,VkCommandBuffer c,const VkCommandBufferBeginInfo*b)noexcept{
 ModeTicket84 t{};if(!d->worker||tls.device!=d||tls.beginDepth84!=1||tls.preparationDepth80||
  !d->alive.load(std::memory_order_acquire)||!d->qualified.load(std::memory_order_acquire)||
  !b||b->sType!=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO||b->pNext||b->pInheritanceInfo||
  (b->flags&~(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT|VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT))||
  !d->selectedProducer.load()||d->selectedProducer.load()!=identity())return t;
 const auto s=d->worker->stats();if(!modeIdle84(s))return t;
 std::lock_guard<std::mutex>lock(d->metadata);const auto command=d->commands.find(c);
 if(command==d->commands.end()||!command->second.primary||command->second.recording)return t;
 const auto&v=command->second;const auto pool=d->pools.find(v.pool);
 if(pool==d->pools.end()||!pool->second.eligible||!v.generation||!v.poolGeneration||
  pool->second.generation!=v.poolGeneration)return t;
 t.token={d->handle,v.pool,c,d->generation,v.poolGeneration,v.generation,v.recordingGeneration};
 t.entries=s.synchronousEntries;t.previousRecording=v.recordingGeneration;t.valid=true;return t;
}
ModeReceipt84 completedMode84(Device*d,const ModeTicket84&t,const ModeInput84&input)noexcept{
 ModeReceipt84 receipt{};if(!t.valid||tls.device!=d||!d->worker||tls.beginDepth84!=1||tls.preparationDepth80||
  !d->alive.load(std::memory_order_acquire)||!d->qualified.load(std::memory_order_acquire)||
  d->selectedProducer.load()!=identity())return receipt;
 std::lock_guard<std::mutex>lock(d->metadata);const auto s=d->worker->stats();
 const auto command=d->commands.find(t.token.command);const auto pool=d->pools.find(t.token.pool);
 if(!modeIdle84(s)||s.synchronousEntries!=t.entries||d->handle!=t.token.device||d->generation!=t.token.deviceGeneration||
  command==d->commands.end()||pool==d->pools.end()||!pool->second.eligible||pool->second.generation!=t.token.poolGeneration)return receipt;
 const auto&v=command->second;
 if(!v.primary||v.recording||v.pool!=t.token.pool||v.poolGeneration!=t.token.poolGeneration||
  v.generation!=t.token.commandGeneration||v.recordingGeneration!=t.previousRecording)return receipt;
 const auto decision=d->modeControl84.sample(input.pid,input.ms,input.property.data());
 if(decision.leased&&!d->modeControlDisabled84){
  if(decision.nonce!=d->modeLastNonce84){
   if(d->modeLeaseGeneration84==UINT64_MAX){d->modeControlDisabled84=true;d->mode.store(1);d->modeNonce84=0;return receipt;}
   ++d->modeLeaseGeneration84;d->modeLastNonce84=decision.nonce;
  }
  d->modeControlActivated84=true;d->modeNonce84=decision.nonce;d->mode.store(decision.mode);
 }else if(d->modeControlActivated84){d->mode.store(1);d->modeNonce84=0;}
 receipt={d->mode.load(),unsigned(decision.leased&&!d->modeControlDisabled84),1,input.pid,
  d->modeNonce84,d->modeLeaseGeneration84,decision.leased?decision.deadlineMs:0,input.ms,t.entries};
 return receipt;
}

std::uint64_t serial(Device*d)noexcept{if(d->serial==UINT64_MAX){revoke(d,Decline::Overflow);return 0;}return ++d->serial;}
template<class T>T*link(const void*p,VkStructureType s){
 auto*b=static_cast<const VkBaseInStructure*>(p);for(unsigned n=0;b&&n<128;++n,b=b->pNext){
  if(b->sType==s){auto*l=const_cast<T*>(reinterpret_cast<const T*>(b));if(l->function==VK_LAYER_LINK_INFO)return l;}}
 return nullptr;
}
bool instanceExtensions(const VkInstanceCreateInfo*c)noexcept{
 static const char* allowed[]={"VK_KHR_surface","VK_KHR_win32_surface","VK_KHR_android_surface","VK_KHR_get_physical_device_properties2",
 "VK_KHR_external_memory_capabilities","VK_KHR_external_semaphore_capabilities","VK_KHR_external_fence_capabilities",
 "VK_KHR_surface_protected_capabilities","VK_EXT_swapchain_colorspace","VK_KHR_get_surface_capabilities2","VK_KHR_portability_enumeration"};
 for(unsigned n=0;n<c->enabledExtensionCount;++n){bool ok=false;for(auto*s:allowed)ok|=std::strcmp(s,c->ppEnabledExtensionNames[n])==0;
#if defined(_WIN32) && defined(RDR2_WORKER28_STAND_CALLBACKS) && !defined(RDR2_LAYER28_TESTING)
  if(rdr2::worker28::stand_callbacks::expectedExecutableSha256()[0])ok|=std::strcmp(c->ppEnabledExtensionNames[n],"VK_EXT_debug_utils")==0||std::strcmp(c->ppEnabledExtensionNames[n],"VK_EXT_debug_report")==0;
#endif
  if(!ok)return false;}return true;
}
bool deviceExtensions(const VkDeviceCreateInfo*c)noexcept{
 static const char* allowed[]={"VK_KHR_swapchain","VK_KHR_external_memory","VK_KHR_get_memory_requirements2","VK_ANDROID_external_memory_android_hardware_buffer",
 "VK_EXT_queue_family_foreign","VK_KHR_external_memory_fd","VK_KHR_external_memory_win32","VK_KHR_sampler_ycbcr_conversion","VK_EXT_debug_marker",
 "VK_KHR_maintenance1","VK_KHR_maintenance2","VK_KHR_maintenance3","VK_KHR_maintenance4","VK_KHR_maintenance5","VK_KHR_maintenance6","VK_KHR_maintenance7","VK_KHR_maintenance8","VK_KHR_maintenance9","VK_KHR_maintenance10",
 "VK_EXT_descriptor_indexing","VK_KHR_depth_stencil_resolve","VK_KHR_create_renderpass2","VK_KHR_image_format_list","VK_KHR_imageless_framebuffer",
 "VK_EXT_host_query_reset","VK_KHR_shader_float16_int8","VK_EXT_extended_dynamic_state","VK_EXT_extended_dynamic_state2","VK_EXT_extended_dynamic_state3",
 "VK_EXT_depth_clip_enable","VK_KHR_fragment_shading_rate","VK_KHR_multiview","VK_KHR_shader_draw_parameters","VK_KHR_16bit_storage",
 "VK_KHR_8bit_storage","VK_KHR_storage_buffer_storage_class","VK_KHR_buffer_device_address","VK_EXT_buffer_device_address","VK_KHR_dedicated_allocation",
 "VK_KHR_bind_memory2","VK_KHR_timeline_semaphore","VK_KHR_synchronization2","VK_KHR_dynamic_rendering","VK_KHR_separate_depth_stencil_layouts",
 "VK_EXT_scalar_block_layout","VK_KHR_uniform_buffer_standard_layout","VK_KHR_spirv_1_4","VK_KHR_shader_float_controls","VK_KHR_relaxed_block_layout"};
 for(unsigned n=0;n<c->enabledExtensionCount;++n){bool ok=false;for(auto*s:allowed)ok|=std::strcmp(s,c->ppEnabledExtensionNames[n])==0;if(!ok)return false;}return true;
}
bool instanceChain(const VkInstanceCreateInfo*c)noexcept{
 unsigned n=0;for(auto*b=static_cast<const VkBaseInStructure*>(c->pNext);b;b=b->pNext){if(++n>128)return false;
  if(b->sType==VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO)continue;
  if(b->sType==VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT&&standDebugUtils(reinterpret_cast<const VkDebugUtilsMessengerCreateInfoEXT*>(b),true))continue;
  if(b->sType==VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT&&standDebugReport(reinterpret_cast<const VkDebugReportCallbackCreateInfoEXT*>(b),true))continue;
  return false;}
 if(c->pApplicationInfo&&c->pApplicationInfo->pNext)return false;
 // This explicit layer must be last: any validation layer must remain on the
 // producer side. Unknown additional layers do not qualify this initial path.
 for(unsigned j=0;j<c->enabledLayerCount;++j){auto*s=c->ppEnabledLayerNames[j];if(std::strcmp(s,Name)==0){if(j+1!=c->enabledLayerCount)return false;}
  else if(std::strcmp(s,"VK_LAYER_KHRONOS_validation")!=0)return false;}return true;
}
bool deviceChain(const VkDeviceCreateInfo*c)noexcept{
 unsigned n=0;for(auto*b=static_cast<const VkBaseInStructure*>(c->pNext);b;b=b->pNext){if(++n>128)return false;switch(b->sType){
 case VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO:
 case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
 case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
 case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
 case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
 case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES:
 case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR:
 case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
 case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:
 case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT:
 case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES:
 case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES:
 case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:
 case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES:
 case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES:
 case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_10_FEATURES_KHR:break;
 default:return false;}}
 for(unsigned j=0;j<c->queueCreateInfoCount;++j)if(c->pQueueCreateInfos[j].pNext||c->pQueueCreateInfos[j].flags)return false;
 return c->enabledLayerCount==0;
}
Dispatch13 dispatch13(Device*d)noexcept{auto&a=d->dispatch;return {a.vkCmdBindPipeline,a.vkCmdBindDescriptorSets,a.vkCmdBindVertexBuffers,a.vkCmdBindIndexBuffer,
 a.vkCmdDraw,a.vkCmdDrawIndexed,a.vkCmdPipelineBarrier,a.vkCmdBeginRenderPass,a.vkCmdEndRenderPass,a.vkCmdSetViewport,a.vkCmdSetScissor,a.vkCmdSetStencilReference,a.vkCmdCopyImage,a.vkCmdResetQueryPool,a.vkCmdWriteTimestamp};}
void clearRecording()noexcept{tls.query85={};tls.originalQuery86={};tls.pending76={};tls.pendingBeginResult76=VK_ERROR_INITIALIZATION_FAILED;tls.command={};tls.recording={};tls.activeNative81={};tls.freshCount81=0;tls.commandDraws=0;tls.commandGeneration=0;}
}
Device*deviceForKey(void*k){std::lock_guard<std::mutex> lock(registryMutex);auto i=std::find_if(devices.begin(),devices.end(),[&](auto&d){return d->handle&&dispatchKey(d->handle)==k;});require(i!=devices.end());
 if(tls.device!=i->get())clearRecording();tls.device=i->get();tls.dispatchKey=k;return tls.device;}
Device*passthroughDevice90(void*k){std::lock_guard<std::mutex> lock(registryMutex);auto i=std::find_if(devices.begin(),devices.end(),[&](auto&d){return d->key&&d->key==k;});require(i!=devices.end());
 passthroughCache90.store(i->get(),std::memory_order_relaxed);return i->get();}
namespace {
// Retire (never free) a destroyed device: clear the pass-through cache, drop it from
// the live registry and from its instance children, keep the object alive.
// The instance is reached only by locking the weak back reference under
// registryMutex (wrap_vkDestroyInstance clears it under the same mutex), so a
// device destroyed after its instance never touches the freed Instance. The
// retired device drops the reference so it does not pin the instance storage.
void retireDevice90(Device*d)noexcept{
 std::shared_ptr<Instance> owner;
 {std::lock_guard<std::mutex> lock(registryMutex);d->alive.store(false,std::memory_order_release);passthroughCache90.store(nullptr,std::memory_order_relaxed);
  auto i=std::find_if(devices.begin(),devices.end(),[&](auto&v){return v.get()==d;});if(i!=devices.end())retiredDevices.splice(retiredDevices.end(),devices,i);
  owner=d->instance.lock();d->instance.reset();}
 if(owner){std::lock_guard<std::mutex> lock(owner->childrenMutex);owner->children.erase(std::remove_if(owner->children.begin(),owner->children.end(),[&](auto&w){auto p=w.lock();return !p||p.get()==d;}),owner->children.end());}
}
}
Instance*instanceForKey(void*k){std::lock_guard<std::mutex> lock(registryMutex);auto i=std::find_if(instances.begin(),instances.end(),[&](auto&d){return d->handle&&dispatchKey(d->handle)==k;});require(i!=instances.end());return i->get();}
void revoke(Device*d,Decline reason)noexcept{d->qualified.store(false,std::memory_order_release);auto zero=std::uint32_t{};d->decline.compare_exchange_strong(zero,std::uint32_t(reason));if(d->ready.load(std::memory_order_acquire)&&d->worker)d->worker->revoke();}
bool allocatorSafe(const VkAllocationCallbacks*a)noexcept{return a==nullptr||rdr2::worker28::retail_allocator::qualify(*a);}
bool standDebugUtils(const VkDebugUtilsMessengerCreateInfoEXT*c,bool chainNode)noexcept{
#if defined(_WIN32) && defined(RDR2_WORKER28_STAND_CALLBACKS) && !defined(RDR2_LAYER28_TESTING)
 return c&&c->sType==VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT&&(chainNode||!c->pNext)&&rdr2::worker28::stand_callbacks::qualifyUtils(c->pfnUserCallback,c->pUserData);
#else
 (void)c;(void)chainNode;return false;
#endif
}
bool standDebugReport(const VkDebugReportCallbackCreateInfoEXT*c,bool chainNode)noexcept{
#if defined(_WIN32) && defined(RDR2_WORKER28_STAND_CALLBACKS) && !defined(RDR2_LAYER28_TESTING)
 return c&&c->sType==VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT&&(chainNode||!c->pNext)&&rdr2::worker28::stand_callbacks::qualifyReport(c->pfnCallback,c->pUserData);
#else
 (void)c;(void)chainNode;return false;
#endif
}
std::atomic<std::uint64_t> diagnosticNonce74{};
void cancelPending77(ThreadContext&t,unsigned reason,const char*api)noexcept{
 t.pending76={};t.pendingBeginResult76=VK_ERROR_INITIALIZATION_FAILED;t.pendingPrelude77=0;t.preparingInflight80=false;
 t.deferredDecision76=reason;t.pendingCancel77=api;
}
inline void cancelPending77(unsigned reason,const char*api)noexcept{cancelPending77(tls,reason,api);}
bool sameToken80(const Token&a,const Token&b)noexcept{
 return a.device==b.device&&a.pool==b.pool&&a.command==b.command&&
  a.deviceGeneration==b.deviceGeneration&&a.poolGeneration==b.poolGeneration&&
  a.commandGeneration==b.commandGeneration&&a.recordingGeneration==b.recordingGeneration;
}
bool pendingMetadata80(ThreadContext&t,Device*d,const Token&token)noexcept{
#ifdef RDR2_LAYER28_TESTING
 extern void(*preparationHook80)();if(preparationHook80)preparationHook80();
#endif
 std::lock_guard<std::mutex> lock(d->metadata);
 auto command=d->commands.find(token.command);auto pool=d->pools.find(token.pool);
 return t.device==d&&t.pendingBeginResult76==VK_SUCCESS&&
  d->alive.load(std::memory_order_acquire)&&d->qualified.load(std::memory_order_acquire)&&d->mode.load()==1&&
  d->selectedProducer.load()==identity(t)&&t.command==token.command&&t.commandGeneration==token.commandGeneration&&
  token.device==d->handle&&token.deviceGeneration==d->generation&&command!=d->commands.end()&&pool!=d->pools.end()&&
  pool->second.eligible&&pool->second.generation==token.poolGeneration&&command->second.primary&&command->second.recording&&
  command->second.thread==identity(t)&&command->second.pool==token.pool&&command->second.poolGeneration==token.poolGeneration&&
  command->second.generation==token.commandGeneration&&command->second.recordingGeneration==token.recordingGeneration&&
  d->selectedPool==token.pool&&d->selectedPoolGeneration==token.poolGeneration;
}
inline bool pendingMetadata80(Device*d,const Token&token)noexcept{return pendingMetadata80(tls,d,token);}
bool preparingApi80(Worker::AccessKind kind,const char*api)noexcept{
 if(!api||std::strcmp(api,"instance/physical host access")==0)return false;
 if(kind!=Worker::AccessKind::Command&&kind!=Worker::AccessKind::Resource&&kind!=Worker::AccessKind::Lookup)return false;
 if(std::strncmp(api,"vkCmd",5)!=0)return true;
 // Only commands whose real command-buffer identity is checked by the
 // supported wrapper's pre-admission helper. No unobserved extension command.
 static constexpr const char* supported[]={"vkCmdBindPipeline","vkCmdBindDescriptorSets","vkCmdBindVertexBuffers",
  "vkCmdBindIndexBuffer","vkCmdPipelineBarrier","vkCmdSetViewport","vkCmdSetScissor","vkCmdSetStencilReference",
  "vkCmdCopyImage","vkCmdResetQueryPool","vkCmdWriteTimestamp"};
 for(auto*name:supported)if(std::strcmp(name,api)==0)return true;
 return false;
}
PreparationEntry80::PreparationEntry80(ThreadContext&c,Worker::HostEntry&&g,Device*d,const Token&t,std::uint64_t n,const char*a,bool p,bool continuation,std::uint64_t generation)noexcept:
 guard_(std::move(g)),device_(d),token_(t),baseline_(n),api_(a),preparing_(p),continuing89_(continuation),coreGeneration89_(generation),tls_(&c){}
PreparationEntry80::PreparationEntry80(PreparationEntry80&&other)noexcept:
 guard_(std::move(other.guard_)),device_(other.device_),token_(other.token_),baseline_(other.baseline_),api_(other.api_),preparing_(other.preparing_),continuing89_(other.continuing89_),coreGeneration89_(other.coreGeneration89_),tls_(other.tls_){other.preparing_=false;other.continuing89_=false;}
PreparationEntry80&PreparationEntry80::operator=(PreparationEntry80&&other)noexcept{
 if(this!=&other){finish();guard_=std::move(other.guard_);device_=other.device_;token_=other.token_;baseline_=other.baseline_;api_=other.api_;preparing_=other.preparing_;continuing89_=other.continuing89_;coreGeneration89_=other.coreGeneration89_;tls_=other.tls_;other.preparing_=false;other.continuing89_=false;}return *this;
}
void PreparationEntry80::finish()noexcept{
 const int savedErrno=errno;
 // Release the original host receipt BEFORE sampling completion. A foreign
 // entry that finishes inside this call remains visible in the monotonic sum.
 guard_=Worker::HostEntry{};
 if(continuing89_){
  continuing89_=false;auto&t=*tls_;
  if(!t.preparationDepth80)std::terminate();--t.preparationDepth80;
  const auto expected=t.boundary89;t.boundary89={};
  const auto stats=device_->worker->stats();
  const bool valid=expected.device==device_&&expected.valid&&
   device_->descriptorEpoch81.load(std::memory_order_acquire)==expected.expectedEpoch&&
   stats.phase==Worker::Phase::Open&&!stats.hostInflight&&stats.synchronousEntries==baseline_+1&&
   stats.generation==coreGeneration89_&&activeMetadata81(t,device_,token_,coreGeneration89_);
  if(!valid){
   // No resurrection of CLOSED or another recording, including callback Begin.
   auto close=device_->worker->enter(Worker::AccessKind::Lifecycle);
   if(t.device==device_){t.recording={};t.activeNative81={};t.freshCount81=0;}
  }
  errno=savedErrno;return;
 }
 if(preparing_){
  preparing_=false;auto&t=*tls_;
  // This lexical depth belongs to the original call, even when a callback
  // changed TLS.device or began a different native recording. Begin cannot
  // reset it or publish a replacement capability while the call is active.
  if(!t.preparationDepth80)std::terminate();--t.preparationDepth80;
  if(t.device==device_&&sameToken80(t.pending76,token_)){
   t.preparingInflight80=false;
   const auto stats=device_->worker->stats();
   if(stats.synchronousEntries!=baseline_+1||stats.hostInflight||
      (stats.phase!=Worker::Phase::Off&&stats.phase!=Worker::Phase::Synchronous))cancelPending77(t,4,"preparationCompletionCounter");
   else if(!pendingMetadata80(t,device_,token_))cancelPending77(t,5,"preparationCompletionIdentity");
   else{
    t.pendingHostEntries76=baseline_+1;++t.preparationEntries80;
    if(std::strncmp(api_,"vkCmd",5)==0)++t.preparationCommands80;
    if(std::strcmp(api_,"vkGetQueryPoolResults")==0)++t.pendingQueries77;
    if(std::strcmp(api_,"vkResetQueryPoolEXT")==0)++t.pendingHostQueryResets78;
    if(std::strcmp(api_,"vkCmdWriteTimestamp")==0)++t.pendingTimestamps79;
   }
  }
 }
 errno=savedErrno;
}
PreparationEntry80 enter(ThreadContext&t,Device*d,Worker::AccessKind k,const char*api,VkCommandBuffer originalCommand89)noexcept{
 if(passthroughActive90())return {}; // No receipt, epoch or TLS work while permanently passing through
 const int savedErrno=errno;
 if(t.boundary89.device)t.boundary89.valid=false;
 if(d&&d->worker&&d->ready.load(std::memory_order_acquire)&&t.device==d&&t.recording&&
    !t.preparationDepth80&&!t.boundary89.device&&originalBoundaryApi89(k,api)&&
    (k==Worker::AccessKind::Resource||originalCommand89==t.command)){
  const auto native=t.activeNative81;const auto generation=t.recording.generation();
  const auto before=d->worker->stats();const auto epoch=d->descriptorEpoch81.load(std::memory_order_acquire);
  if(before.phase==Worker::Phase::Open&&!before.hostInflight&&before.synchronousEntries<UINT64_MAX-1&&
     activeMetadata81(t,d,native,generation)){
   ++t.preparationDepth80;
   auto guard=d->worker->enterProducerPreparation(t.recording);
   const bool drained=guard.preservesRecording()&&guard.drainProducerPreparation(t.recording);
   const auto current=d->worker->stats();
   if(drained&&current.phase==Worker::Phase::Open&&current.hostInflight==1&&
      current.synchronousEntries==before.synchronousEntries+1&&current.generation==generation&&
      d->descriptorEpoch81.load(std::memory_order_acquire)==epoch&&activeMetadata81(t,d,native,generation)){
    t.boundary89={d,epoch,true};
    // Clear descriptor certificates. This preserves CPU call ordering only;
    // it never grants authority to enqueue a bind of an unknown descriptor.
    invalidateFreshDescriptors81(t,d);
    errno=savedErrno;return PreparationEntry80(t,std::move(guard),d,native,before.synchronousEntries,api,false,true,generation);
   }
   guard={};--t.preparationDepth80;
  }
 }
 invalidateFreshDescriptors81(t,d);
#ifdef RDR2_LAYER28_TESTING
 extern void(*preparationHook80)();if(preparationHook80)preparationHook80();
#endif
 bool preparing=false;Token token{};std::uint64_t baseline{};
 if(t.device==d&&t.pending76.command){
  token=t.pending76;baseline=t.pendingHostEntries76;
  if(t.preparationDepth80)cancelPending77(t,2,"nestedPreparation");
  else if(!preparingApi80(k,api))cancelPending77(t,2,api);
  else if(t.preparationEntries80>=512)cancelPending77(t,10,"preparationBudget");
  else if(!d->worker||baseline==UINT64_MAX)cancelPending77(t,4,"preparationEntryCounter");
  else{
   const auto stats=d->worker->stats();
   if(stats.synchronousEntries!=baseline||stats.hostInflight||
      (stats.phase!=Worker::Phase::Off&&stats.phase!=Worker::Phase::Synchronous))cancelPending77(t,4,"preparationEntryCounter");
   else if(!pendingMetadata80(t,d,token))cancelPending77(t,5,"preparationEntryIdentity");
   else{preparing=t.preparingInflight80=true;++t.preparationDepth80;}
  }
 }
 if(!d->ready.load(std::memory_order_acquire)||!d->worker){errno=savedErrno;return {};}
 const char*none=nullptr;const bool first=d->recordingActive.load(std::memory_order_acquire)&&d->firstCloseApi.compare_exchange_strong(none,api,std::memory_order_acq_rel);
 if(t.device==d&&t.recording&&!t.ownedClose74){const auto before=d->worker->stats();
  t.ownedClose74=api;t.ownedClosePhase74=std::uint64_t(before.phase);t.ownedClosePrefix74=before.enqueued-t.localQueuedBefore;}
 auto guard=d->worker->enter(k);if(t.device==d)t.recording={};
 if(first){const auto s=d->worker->stats();d->firstClosePrefix.store(s.enqueued-d->recordingBaseQueued.load(),std::memory_order_release);}
 errno=savedErrno;return PreparationEntry80(t,std::move(guard),d,token,baseline,api,preparing);
}

// Observations never register descriptor authority or change worker state.
namespace {struct ObservationErrno82{int incoming=errno;~ObservationErrno82(){errno=incoming;}};}
DescriptorPoolObservation82 poolObservation82(const VkDescriptorPoolCreateInfo*c,const VkAllocationCallbacks*a,bool shape)noexcept{
 DescriptorPoolObservation82 v{};if(!c)return v;
 v.flags=c->flags;v.maxSets=c->maxSets;v.sizeCount=c->poolSizeCount;v.allocator=a?1u:0u;v.next=c->pNext?1u:0u;v.shape=shape?1u:0u;
 if(c->sType==VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO&&c->poolSizeCount&&c->pPoolSizes){v.type=unsigned(c->pPoolSizes[0].type);v.descriptors=c->pPoolSizes[0].descriptorCount;}
 return v;
}
DescriptorLayoutObservation82 layoutObservation82(const VkDescriptorSetLayoutCreateInfo*c,bool shape)noexcept{
 DescriptorLayoutObservation82 v{};if(!c)return v;
 v.flags=c->flags;v.bindings=c->bindingCount;v.shape=shape?1u:0u;v.next=c->pNext?2u:0u;
 if(c->sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO||!c->bindingCount||c->bindingCount>16||!c->pBindings)return v;
 const VkDescriptorSetLayoutBindingFlagsCreateInfo*f=nullptr;
 if(c->pNext){const auto*p=static_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo*>(c->pNext);
  if(p->sType==VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO){v.next=1;
   if(!p->pNext&&(p->bindingCount==0||p->bindingCount==c->bindingCount)&&(!p->bindingCount||p->pBindingFlags))f=p;
  }
 }
 for(unsigned n=0;n<c->bindingCount;++n){const auto&b=c->pBindings[n];v.stages|=b.stageFlags;
  if(b.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)++v.dynamic;else ++v.other;
  if(f&&f->bindingCount)v.bindingFlags|=f->pBindingFlags[n];
 }
 return v;
}
namespace {
bool observationIncarnation82(Device*d,std::uint64_t&value)noexcept{
 if(!d->observationAvailable82||d->observationSerial82==UINT64_MAX){d->observationAvailable82=false;return false;}
 value=++d->observationSerial82;return true;
}
}
void createdPoolObservation82(Device*d,VkDescriptorPool h,const DescriptorPoolObservation82&fact,std::uint64_t epoch)noexcept{
 if(!h)return;ObservationErrno82 preserve;
 try{std::lock_guard<std::mutex>lock(d->descriptorMetadata81);
  if(!d->observationAvailable82||!d->alive.load(std::memory_order_acquire)||d->descriptorEpoch81.load(std::memory_order_acquire)!=epoch)return;
  auto v=fact;if(d->poolObservations82.size()>=8192||!observationIncarnation82(d,v.incarnation)){d->observationAvailable82=false;return;}
  if(!d->poolObservations82.emplace(h,v).second)d->observationAvailable82=false;
 }catch(...){std::lock_guard<std::mutex>lock(d->descriptorMetadata81);d->observationAvailable82=false;}
}
void createdLayoutObservation82(Device*d,VkDescriptorSetLayout h,const DescriptorLayoutObservation82&fact,std::uint64_t epoch)noexcept{
 if(!h)return;ObservationErrno82 preserve;
 try{std::lock_guard<std::mutex>lock(d->descriptorMetadata81);
  if(!d->observationAvailable82||!d->alive.load(std::memory_order_acquire)||d->descriptorEpoch81.load(std::memory_order_acquire)!=epoch)return;
  auto v=fact;if(d->layoutObservations82.size()>=8192||!observationIncarnation82(d,v.incarnation)){d->observationAvailable82=false;return;}
  if(!d->layoutObservations82.emplace(h,v).second)d->observationAvailable82=false;
 }catch(...){std::lock_guard<std::mutex>lock(d->descriptorMetadata81);d->observationAvailable82=false;}
}
void retirePoolObservation82(Device*d,VkDescriptorPool h,bool reset)noexcept{
 std::lock_guard<std::mutex>lock(d->descriptorMetadata81);const auto p=d->poolObservations82.find(h);
 if(p==d->poolObservations82.end())return;
 if(reset){if(!observationIncarnation82(d,p->second.incarnation))d->poolObservations82.erase(p);}
 else d->poolObservations82.erase(p);
}
void retireLayoutObservation82(Device*d,VkDescriptorSetLayout h)noexcept{
 std::lock_guard<std::mutex>lock(d->descriptorMetadata81);d->layoutObservations82.erase(h);
}

namespace {
// Protected by descriptorMetadata81, independent of command admission's lock.
std::uint64_t descriptorSerial81(Device*d)noexcept{
 if(d->descriptorSerial81==UINT64_MAX){d->descriptorEligible81=false;revoke(d,Decline::Overflow);return 0;}
 return ++d->descriptorSerial81;
}
bool activeMetadata81(ThreadContext&t,Device*d,const Token&native,std::uint64_t generation)noexcept{
 std::lock_guard<std::mutex>lock(d->metadata);
 const auto c=d->commands.find(native.command);const auto p=d->pools.find(native.pool);
 return t.device==d&&t.recording&&t.recording.generation()==generation&&
  sameToken80(t.activeNative81,native)&&native.device==d->handle&&native.deviceGeneration==d->generation&&
  d->alive.load(std::memory_order_acquire)&&d->qualified.load(std::memory_order_acquire)&&d->mode.load()==1&&
  d->selectedProducer.load()==identity(t)&&t.command==native.command&&t.commandGeneration==native.commandGeneration&&
  c!=d->commands.end()&&p!=d->pools.end()&&p->second.eligible&&p->second.generation==native.poolGeneration&&
  c->second.primary&&c->second.recording&&c->second.thread==identity(t)&&c->second.pool==native.pool&&
  c->second.poolGeneration==native.poolGeneration&&c->second.generation==native.commandGeneration&&
  c->second.recordingGeneration==native.recordingGeneration&&d->selectedPool==native.pool&&
  d->selectedPoolGeneration==native.poolGeneration;
}
bool activeMetadata81(Device*d,const Token&native,std::uint64_t generation)noexcept{return activeMetadata81(tls,d,native,generation);}
void recordCoreFallback81(Device*d,const char*api,const Worker::Stats&before)noexcept{
 invalidateFreshDescriptors81(d);
 const char*none=nullptr;
 if(d->recordingActive.load(std::memory_order_acquire)&&d->firstCloseApi.compare_exchange_strong(none,api))
  d->firstClosePrefix.store(before.enqueued-d->recordingBaseQueued.load(),std::memory_order_release);
 if(tls.device==d){
  if(tls.recording&&!tls.ownedClose74){tls.ownedClose74=api;tls.ownedClosePhase74=std::uint64_t(before.phase);tls.ownedClosePrefix74=before.enqueued-tls.localQueuedBefore;}
  tls.recording={};tls.activeNative81={};
 }
}
bool sameDescriptorReceipt81(const FreshDescriptor81&a,const FreshDescriptor81&b)noexcept{
 return a.set==b.set&&a.pool==b.pool&&a.layout==b.layout&&a.poolGeneration==b.poolGeneration&&
  a.layoutGeneration==b.layoutGeneration&&a.allocation==b.allocation&&a.epoch==b.epoch&&a.workerGeneration==b.workerGeneration;
}
}
void invalidateFreshDescriptors81(ThreadContext&t,Device*d)noexcept{
 const auto old=d->descriptorEpoch81.fetch_add(1,std::memory_order_acq_rel);
 if(t.boundary89.device==d){
  if(old!=t.boundary89.expectedEpoch||old==UINT64_MAX)t.boundary89.valid=false;
  t.boundary89.expectedEpoch=old+1;
 }
 if(old==UINT64_MAX){d->descriptorEligible81=false;revoke(d,Decline::Overflow);}
 if(t.device==d)t.freshCount81=0;
}
// A diagnostic join to the actual native recording, never query authority.
bool queryScope85(Device*d,const QueryTicket85&t)noexcept{
 if(!t.valid||tls.device!=d||!tls.query85.seen||tls.query85.serial!=t.serial||
    tls.observationBeginSerial82!=t.serial||tls.beginNonce74!=t.nonce||
    !sameToken80(tls.query85.native,t.native)||!sameToken80(tls.modeNative84,t.native)||
    tls.command!=t.native.command||tls.commandGeneration!=t.native.commandGeneration)return false;
 std::lock_guard<std::mutex>lock(d->metadata);
 const auto c=d->commands.find(t.native.command);const auto p=d->pools.find(t.native.pool);
 return d->alive.load(std::memory_order_acquire)&&t.native.device==d->handle&&t.native.deviceGeneration==d->generation&&
  c!=d->commands.end()&&p!=d->pools.end()&&p->second.generation==t.native.poolGeneration&&
  c->second.recording&&c->second.thread==identity()&&c->second.pool==t.native.pool&&
  c->second.poolGeneration==t.native.poolGeneration&&c->second.generation==t.native.commandGeneration&&
  c->second.recordingGeneration==t.native.recordingGeneration;
}
QueryTicket85 observeQueryBegin85(Device*d,std::uint64_t caller,VkQueryPool pool,unsigned first,unsigned count,
 size_t bytes,void*data,VkDeviceSize stride,VkQueryResultFlags flags)noexcept{
 const int incoming=errno;QueryTicket85 ticket{};
 if(tls.device==d&&tls.beginNonce74&&tls.recording&&!tls.ownedClose74&&!tls.query85.seen&&d->worker){
  const auto native=tls.activeNative81;const auto generation=tls.recording.generation();const auto s=d->worker->stats();
  if(s.phase==Worker::Phase::Open&&!s.hostInflight&&!s.permanentlyRevoked&&s.enqueued>=tls.localQueuedBefore&&
     activeMetadata81(d,native,generation)){
   auto&o=tls.query85;o={};o.seen=1;o.native=native;o.serial=tls.observationBeginSerial82;
   o.nonce=tls.beginNonce74;o.workerGeneration=generation;o.prefix=s.enqueued-tls.localQueuedBefore;
   o.queryCaller=caller;o.queryPool=pool;o.first=first;o.count=count;o.dataSize=bytes;o.stride=stride;
   o.flags=flags;o.dataNonNull=data!=nullptr;
   ticket={native,o.serial,o.nonce,generation,true};
  }
 }
 errno=incoming;return ticket;
}
void observeQueryEnd85(Device*d,const QueryTicket85&t,VkResult result)noexcept{
 const int incoming=errno;
 if(t.valid&&tls.query85.serial==t.serial&&sameToken80(tls.query85.native,t.native)){
  tls.query85.scopeValid=queryScope85(d,t);tls.query85.result=result;tls.query85.resultKnown=1;
 }
 errno=incoming;
}
QueryTicket85 observeResetBegin85(Device*d,std::uint64_t caller,VkQueryPool pool,unsigned first,unsigned count,unsigned api)noexcept{
 const int incoming=errno;QueryTicket85 ticket{};const auto&o=tls.query85;
 if(o.seen&&o.resultKnown&&o.scopeValid&&!o.resetSeen){
  ticket={o.native,o.serial,o.nonce,o.workerGeneration,true};
  if(queryScope85(d,ticket)){
   auto&r=tls.query85;r.resetSeen=1;r.resetCaller=caller;r.resetPool=pool;r.resetFirst=first;r.resetCount=count;r.resetApi=api;
  }else ticket.valid=false;
 }
 errno=incoming;return ticket;
}
void observeResetEnd85(Device*d,const QueryTicket85&t)noexcept{
 const int incoming=errno;
 if(t.valid&&tls.query85.serial==t.serial&&sameToken80(tls.query85.native,t.native)){
  tls.query85.resetReturned=1;tls.query85.resetScopeValid=queryScope85(d,t);
 }
 errno=incoming;
}

OriginalHostQuery86::OriginalHostQuery86(Device*d,const char*api,bool qualified)noexcept:device_(d){
 const int incoming=errno;
 if(!qualified||!d->worker||!d->ready.load(std::memory_order_acquire)||
    tls.device!=d||!tls.recording||tls.preparationDepth80){fallback_=enter(d,Worker::AccessKind::Command,api);errno=incoming;return;}
 native_=tls.activeNative81;workerGeneration_=tls.recording.generation();
 const auto before=d->worker->stats();baseline_=before.synchronousEntries;epoch_=d->descriptorEpoch81.load(std::memory_order_acquire);
 if(baseline_==UINT64_MAX||before.phase!=Worker::Phase::Open||before.hostInflight||!activeMetadata81(d,native_,workerGeneration_)){
  fallback_=enter(d,Worker::AccessKind::Command,api);errno=incoming;return;
 }
 ++tls.preparationDepth80;heldDepth_=true;
 guard_=d->worker->enterProducerPreparation(tls.recording);
 preserved_=guard_.preservesRecording()&&guard_.drainProducerPreparation(tls.recording);
 if(preserved_){
  const auto current=d->worker->stats();
  preserved_=current.synchronousEntries==baseline_+1&&current.hostInflight==1&&current.phase==Worker::Phase::Open&&
   d->descriptorEpoch81.load(std::memory_order_acquire)==epoch_&&activeMetadata81(d,native_,workerGeneration_);
 }
 if(!preserved_){guard_={};fallback_=enter(d,Worker::AccessKind::Command,api);}
 errno=incoming;
}
bool OriginalHostQuery86::complete(bool originalAccepted)noexcept{
 if(finished_)return false;finished_=true;const int incoming=errno;
 guard_={};fallback_={};
 if(heldDepth_){if(!tls.preparationDepth80)std::terminate();--tls.preparationDepth80;heldDepth_=false;}
 bool valid=preserved_&&originalAccepted;const bool held=preserved_;preserved_=false;
 if(valid){const auto current=device_->worker->stats();
  valid=current.synchronousEntries==baseline_+1&&!current.hostInflight&&current.phase==Worker::Phase::Open&&
   device_->descriptorEpoch81.load(std::memory_order_acquire)==epoch_&&activeMetadata81(device_,native_,workerGeneration_);
 }
 if(held&&!valid){auto drain=enter(device_,Worker::AccessKind::Command,"originalQueryCompletion86");}
 errno=incoming;return valid;
}
bool originalObservedQueryShape86(VkQueryPool pool,unsigned first,unsigned count,size_t bytes,void*data,VkDeviceSize stride,VkQueryResultFlags flags)noexcept{
 // Exact actual85 arguments narrow the route, not object or caller authority.
 // No query data is inspected, copied, deferred or replaced.
 return pool&&first==0&&count==2&&bytes==16&&data&&stride==8&&flags==VK_QUERY_RESULT_64_BIT;
}
void originalQueryCompleted86(Device*d,std::uint64_t caller,VkQueryPool pool,bool continued,VkResult result)noexcept{
 const int incoming=errno;tls.originalQuery86={};
 if(continued&&result==VK_SUCCESS&&tls.device==d&&tls.recording&&caller&&pool){
  tls.originalQuery86={tls.activeNative81,tls.recording.generation(),caller,d->worker->stats().synchronousEntries,pool,true};
 }
 errno=incoming;
}
bool originalResetSequence86(Device*d,std::uint64_t caller,VkQueryPool pool,unsigned first,unsigned count)noexcept{
 const int incoming=errno;const auto previous=tls.originalQuery86;tls.originalQuery86={};
 // The observed native EXT reset immediately follows its own successful read.
 // This only narrows the original-call route; SUCCESS is not a GPU lifetime lease.
 const bool matched=previous.readCompleted&&previous.caller<=UINT64_MAX-64&&caller==previous.caller+64&&
  pool==previous.pool&&first==0&&count==2&&tls.device==d&&tls.recording&&
  tls.recording.generation()==previous.workerGeneration&&sameToken80(tls.activeNative81,previous.native)&&d->worker&&
  d->worker->stats().synchronousEntries==previous.hostEntries;
 errno=incoming;return matched;
}

ActivePreparation81::ActivePreparation81(Device*d,const char*api)noexcept:device_(d){
 const int incoming=errno;
 if(!d->worker||!d->ready.load(std::memory_order_acquire)||!d->descriptorEligible81.load(std::memory_order_acquire)||
    tls.device!=d||!tls.recording||tls.preparationDepth80){fallback_=enter(d,Worker::AccessKind::Resource,api);errno=incoming;return;}
 entryObservation82_=2;
 native_=tls.activeNative81;workerGeneration_=tls.recording.generation();
 const auto before=d->worker->stats();baseline_=before.synchronousEntries;epoch_=d->descriptorEpoch81.load(std::memory_order_acquire);
 if(baseline_==UINT64_MAX||before.phase!=Worker::Phase::Open||before.hostInflight||!activeMetadata81(d,native_,workerGeneration_)){
  fallback_=enter(d,Worker::AccessKind::Resource,api);errno=incoming;return;
 }
 entryObservation82_=3;
 ++tls.preparationDepth80;heldDepth_=true;
 guard_=d->worker->enterProducerPreparation(tls.recording);preserved_=guard_.preservesRecording();
 if(preserved_)entryObservation82_=4;
 if(!preserved_)recordCoreFallback81(d,api,before);
 errno=incoming;
}
bool ActivePreparation81::complete()noexcept{
 if(finished_)return false;finished_=true;const int saved=errno;
 guard_={};fallback_={};
 if(heldDepth_){if(!tls.preparationDepth80)std::terminate();--tls.preparationDepth80;heldDepth_=false;}
 bool valid=preserved_;completionObservation82_=valid?2u:1u;preserved_=false;
 if(valid){const auto stats=device_->worker->stats();
  valid=stats.synchronousEntries==baseline_+1&&!stats.hostInflight&&stats.phase==Worker::Phase::Open&&
   device_->descriptorEpoch81.load(std::memory_order_acquire)==epoch_&&
   activeMetadata81(device_,native_,workerGeneration_);
  if(!valid){completionObservation82_=3;auto originalDrain=enter(device_,Worker::AccessKind::Resource,"descriptorPreparationCompletion");}
 }
 errno=saved;return valid;
}
bool descriptorPoolShape81(const VkDescriptorPoolCreateInfo*c,const VkAllocationCallbacks*a)noexcept{
 return !a&&c&&c->sType==VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO&&!c->pNext&&!c->flags&&
  c->maxSets==256&&c->poolSizeCount==1&&c->pPoolSizes&&
  c->pPoolSizes[0].type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC&&c->pPoolSizes[0].descriptorCount==4096;
}
bool descriptorLayoutShape81(const VkDescriptorSetLayoutCreateInfo*c,DescriptorLayout81&out)noexcept{
 out={};if(!c||c->sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO||c->flags||
  !c->bindingCount||c->bindingCount>16||!c->pBindings)return false;
 const VkDescriptorSetLayoutBindingFlagsCreateInfo*f=nullptr;
 if(c->pNext){f=static_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo*>(c->pNext);
  if(f->sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO||f->pNext||
   (f->bindingCount&&f->bindingCount!=c->bindingCount)||(f->bindingCount&&!f->pBindingFlags))return false;}
 for(unsigned n=0;n<c->bindingCount;++n){const auto&b=c->pBindings[n];
  const auto flags=f&&f->bindingCount?f->pBindingFlags[n]:0;
  if(b.binding>=16||b.descriptorCount!=1||b.descriptorType!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC||
   b.pImmutableSamplers||!b.stageFlags||(b.stageFlags&~(VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT))||
   (flags&~VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT)||(out.bindings&(1u<<b.binding)))return false;
  out.bindings|=std::uint16_t(1u<<b.binding);if(!(flags&VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT))out.required|=std::uint16_t(1u<<b.binding);
 }
 return true;
}
bool descriptorAllocationShape81(Device*d,const VkDescriptorSetAllocateInfo*c,FreshDescriptor81&out,bool observe82)noexcept{
 out={};const unsigned reason82=!d->descriptorEligible81.load(std::memory_order_acquire)?1u:!c?2u:c->sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO?3u:
  c->pNext?4u:c->descriptorSetCount!=1?5u:!c->pSetLayouts?6u:!c->descriptorPool?7u:!c->pSetLayouts[0]?8u:0u;
 if(observe82){tls.allocation82.sets=c?c->descriptorSetCount:0;tls.allocation82.shape=reason82;}
 if(reason82)return false;
 std::lock_guard<std::mutex>lock(d->descriptorMetadata81);
 const auto p=d->descriptorPools81.find(c->descriptorPool);const auto l=d->descriptorLayouts81.find(c->pSetLayouts[0]);
 if(observe82){auto&v=tls.allocation82;v.available=d->observationAvailable82?1u:0u;
  if(d->observationAvailable82){const auto op=d->poolObservations82.find(c->descriptorPool);const auto ol=d->layoutObservations82.find(c->pSetLayouts[0]);
   if(op!=d->poolObservations82.end()){v.poolKnown=1;v.pool=op->second;}
   if(ol!=d->layoutObservations82.end()){v.layoutKnown=1;v.layout=ol->second;}
  }
  v.shape=p==d->descriptorPools81.end()?9u:l==d->descriptorLayouts81.end()?10u:p->second.count==256?11u:12u;
#ifdef __ANDROID__
  if(v.shape==9&&tls.beginNonce74&&d->missingPoolLogs87<16){++d->missingPoolLogs87;
   __android_log_print(ANDROID_LOG_INFO,"RdrWorker87","route=allocation device=%p pool=%p command=%p nonce=%llu beginSerial=%llu poolKnown=%u layoutKnown=%u factsAvailable=%u",
    reinterpret_cast<void*>(d->handle),reinterpret_cast<void*>(c->descriptorPool),reinterpret_cast<void*>(tls.command),
    static_cast<unsigned long long>(tls.beginNonce74),static_cast<unsigned long long>(tls.observationBeginSerial82),v.poolKnown,v.layoutKnown,v.available);
  }
#endif
 }
 if(p==d->descriptorPools81.end()||l==d->descriptorLayouts81.end()||p->second.count==256)return false;
 out.pool=c->descriptorPool;out.layout=c->pSetLayouts[0];out.poolGeneration=p->second.generation;
 out.layoutGeneration=l->second.generation;out.bindings=l->second.bindings;out.required=l->second.required;
 return true;
}
// A successful reset can restore a quarantined, cold-certified pool. The
// diagnostic82 observation maps are deliberately absent from this protocol.
DescriptorPoolReset83 snapshotDescriptorPoolReset83(Device*d,VkDescriptorPool pool,VkDescriptorPoolResetFlags flags)noexcept{
 ObservationErrno82 preserve;
 std::lock_guard<std::mutex>lock(d->descriptorMetadata81);
 if(flags||!d->alive.load(std::memory_order_acquire)||!d->qualified.load(std::memory_order_acquire)||
    !d->descriptorEligible81.load(std::memory_order_acquire))return {};
 const auto c=d->descriptorPoolCertificates83.find(pool);
 if(c==d->descriptorPoolCertificates83.end())return {};
 return {c->second.creation,c->second.incarnation,d->descriptorEpoch81.load(std::memory_order_acquire)};
}
void completedDescriptorPoolReset83(Device*d,VkDescriptorPool pool,const DescriptorPoolReset83&ticket)noexcept{
 try{
  std::lock_guard<std::mutex>lock(d->descriptorMetadata81);
  const auto c=d->descriptorPoolCertificates83.find(pool);
  if(c==d->descriptorPoolCertificates83.end()){d->descriptorPools81.erase(pool);return;}
  const bool valid=ticket.creation&&c->second.creation==ticket.creation&&c->second.incarnation==ticket.incarnation&&
   d->alive.load(std::memory_order_acquire)&&d->qualified.load(std::memory_order_acquire)&&
   d->descriptorEligible81.load(std::memory_order_acquire)&&d->descriptorEpoch81.load(std::memory_order_acquire)==ticket.epoch;
  // Every actual successful reset invalidates older reset tickets, including
  // nested callbacks. A callback may have destroyed and recreated the handle;
  // that new creation is never authorized by the old outer reset receipt.
  const auto incarnation=descriptorSerial81(d);c->second.incarnation=incarnation;
  if(!valid||!incarnation){d->descriptorPools81.erase(pool);return;}
  const auto p=d->descriptorPools81.find(pool);
  if(p!=d->descriptorPools81.end())p->second=DescriptorPool81{incarnation};
  else{
   if(d->descriptorPools81.size()>=8192)throw 0;
   if(!d->descriptorPools81.emplace(pool,DescriptorPool81{incarnation}).second)throw 0;
  }
 }catch(...){d->descriptorEligible81=false;revoke(d,Decline::MetadataAllocation);}
}
void destroyedDescriptorPoolCertificate83(Device*d,VkDescriptorPool pool)noexcept{
 std::lock_guard<std::mutex>lock(d->descriptorMetadata81);
 d->descriptorPoolCertificates83.erase(pool);
}

namespace {
void changedPool87(Device*d,VkDescriptorPool pool)noexcept{
 auto&s=d->poolBirth87;if(!pool||!s.births||!s.available)return;
 for(unsigned n=0;n<s.changedCount;++n)if(s.changed[n]==pool)return;
 if(s.changedCount==s.changed.size()){s.available=false;return;}
 s.changed[s.changedCount++]=pool;
}
bool changedPool87(const PoolBirthState87&s,VkDescriptorPool pool)noexcept{
 for(unsigned n=0;n<s.changedCount;++n)if(s.changed[n]==pool)return true;
 return false;
}
void finishBirth87(Device*d)noexcept{
 auto&s=d->poolBirth87;if(s.births)--s.births;if(!s.births)s.changedCount=0;
}
}
PoolOperation87::PoolOperation87(Device*d,VkDescriptorPool pool)noexcept{
 const int saved=errno;if(!d||!pool){errno=saved;return;}
 std::lock_guard<std::mutex>lock(d->descriptorMetadata81);auto&s=d->poolBirth87;
 if(s.available){for(unsigned n=0;n<s.operations.size();++n)if(!s.operations[n]){s.operations[n]=pool;device_=d;slot_=n;break;}
  if(!device_)s.available=false;changedPool87(d,pool);}
 errno=saved;
}
PoolOperation87::~PoolOperation87(){
 const int saved=errno;if(device_){std::lock_guard<std::mutex>lock(device_->descriptorMetadata81);
  auto&s=device_->poolBirth87;changedPool87(device_,s.operations[slot_]);s.operations[slot_]={};}
 errno=saved;
}
PoolBirth87::PoolBirth87(Device*d)noexcept:device_(d){
 const int saved=errno;std::lock_guard<std::mutex>lock(d->descriptorMetadata81);auto&s=d->poolBirth87;
 if(s.available&&s.births<64){if(!s.births)s.changedCount=0;++s.births;tracked_=true;
  // A same-pool operation may have entered before this Create. Its receipt
  // must not be hidden merely because it predates the birth-window counter.
  for(auto pool:s.operations)changedPool87(d,pool);
 }else if(s.births>=64)s.available=false;
 errno=saved;
}
PoolBirth87::~PoolBirth87(){
 const int saved=errno;if(tracked_&&!finished_){std::lock_guard<std::mutex>lock(device_->descriptorMetadata81);finishBirth87(device_);}
 errno=saved;
}
void PoolBirth87::complete(VkResult result,VkDescriptorPool pool,bool shape,const DescriptorPoolObservation82&fact,std::uint64_t epoch)noexcept{
 const int saved=errno;if(finished_){errno=saved;return;}finished_=true;
 if(!tracked_){errno=saved;return;}
 auto*d=device_;unsigned reason{};
 try{std::lock_guard<std::mutex>lock(d->descriptorMetadata81);auto&s=d->poolBirth87;
  const bool originalSuccess=result==VK_SUCCESS&&pool;
  const bool valid=originalSuccess&&s.available&&d->alive.load(std::memory_order_acquire)&&
   d->qualified.load(std::memory_order_acquire)&&!changedPool87(s,pool);
  reason=!originalSuccess?1:!s.available?2:!d->alive.load()?3:!d->qualified.load()?4:changedPool87(s,pool)?5:0;
  if(valid&&shape&&d->descriptorEligible81.load(std::memory_order_acquire)){
   if(d->descriptorPools81.size()>=8192||d->descriptorPoolCertificates83.size()>=8192||
      d->descriptorPools81.count(pool)||d->descriptorPoolCertificates83.count(pool))throw 0;
   const auto generation=descriptorSerial81(d);
   if(generation){if(!d->descriptorPoolCertificates83.emplace(pool,DescriptorPoolCertificate83{generation,generation}).second)throw 0;
    if(!d->descriptorPools81.emplace(pool,DescriptorPool81{generation}).second)throw 0;}
  }
  if(valid&&d->observationAvailable82){auto v=fact;
   if(d->poolObservations82.size()>=8192||!observationIncarnation82(d,v.incarnation))d->observationAvailable82=false;
   else if(!d->poolObservations82.emplace(pool,v).second)d->observationAvailable82=false;
  }
  // Even an unsupported successful Create prevents an older overlapping
  // Create from publishing authority for the same returned handle.
  if(originalSuccess)changedPool87(d,pool);
  finishBirth87(d);
 }catch(...){
  {std::lock_guard<std::mutex>lock(d->descriptorMetadata81);d->poolBirth87.available=false;finishBirth87(d);}
  d->descriptorEligible81=false;revoke(d,Decline::MetadataAllocation);reason=6;
 }
#ifdef __ANDROID__
 if(d->birthLogs87.fetch_add(1,std::memory_order_relaxed)<64)
  __android_log_print(ANDROID_LOG_INFO,"RdrWorker87","device=%p pool=%p shape=%u result=%d birthReason=%u epochBefore=%llu epochAfter=%llu",
   reinterpret_cast<void*>(d->handle),reinterpret_cast<void*>(pool),unsigned(shape),int(result),reason,
   static_cast<unsigned long long>(epoch),static_cast<unsigned long long>(d->descriptorEpoch81.load()));
#else
 (void)reason;
#endif
 errno=saved;
}

void createdDescriptorPool81(Device*d,VkDescriptorPool p,bool qualified,std::uint64_t originalEpoch)noexcept{
 if(!qualified||!p||!d->descriptorEligible81.load(std::memory_order_acquire))return;
 try{std::lock_guard<std::mutex>lock(d->descriptorMetadata81);
  // Generic entries bump the epoch before retiring records under this mutex.
  // A callback retiring the newly-created handle before publication cannot be
  // overwritten by the outer original Create's eventual SUCCESS receipt.
  if(!d->alive.load(std::memory_order_acquire)||!d->qualified.load(std::memory_order_acquire)||
   d->descriptorEpoch81.load(std::memory_order_acquire)!=originalEpoch)return;
  if(d->descriptorPools81.size()>=8192||d->descriptorPoolCertificates83.size()>=8192||
     d->descriptorPools81.count(p)||d->descriptorPoolCertificates83.count(p))throw 0;
  const auto generation=descriptorSerial81(d);if(!generation)return;
  if(!d->descriptorPoolCertificates83.emplace(p,DescriptorPoolCertificate83{generation,generation}).second)throw 0;
  if(!d->descriptorPools81.emplace(p,DescriptorPool81{generation}).second)throw 0;
 }catch(...){d->descriptorEligible81=false;revoke(d,Decline::MetadataAllocation);}
}
void createdDescriptorLayout81(Device*d,VkDescriptorSetLayout h,const DescriptorLayout81&shape,bool qualified,std::uint64_t originalEpoch)noexcept{
 if(!qualified||!h||!d->descriptorEligible81.load(std::memory_order_acquire))return;
 try{std::lock_guard<std::mutex>lock(d->descriptorMetadata81);
  if(!d->alive.load(std::memory_order_acquire)||!d->qualified.load(std::memory_order_acquire)||
   d->descriptorEpoch81.load(std::memory_order_acquire)!=originalEpoch)return;
  auto copy=shape;copy.generation=descriptorSerial81(d);
  if(d->descriptorLayouts81.size()>=8192||!d->descriptorLayouts81.emplace(h,copy).second)throw 0;
 }catch(...){d->descriptorEligible81=false;revoke(d,Decline::MetadataAllocation);}
}
void allocatedDescriptor81(Device*d,VkDescriptorSet set,const FreshDescriptor81&receipt,bool preserved)noexcept{
 const int saved=errno;bool grant=false;FreshDescriptor81 fresh=receipt;
 {std::lock_guard<std::mutex>lock(d->descriptorMetadata81);
  const auto p=d->descriptorPools81.find(receipt.pool);const auto l=d->descriptorLayouts81.find(receipt.layout);
  if(set&&p!=d->descriptorPools81.end()&&l!=d->descriptorLayouts81.end()&&
     p->second.generation==receipt.poolGeneration&&l->second.generation==receipt.layoutGeneration){
   auto&pool=p->second;bool duplicate=false;for(unsigned n=0;n<pool.count;++n)duplicate|=pool.allocated[n]==set;
   if(duplicate||pool.count==256){d->descriptorEligible81=false;}
   else{pool.allocated[pool.count++]=set;fresh.set=set;fresh.allocation=descriptorSerial81(d);
    fresh.epoch=d->descriptorEpoch81.load(std::memory_order_acquire);fresh.workerGeneration=tls.recording.generation();
    grant=preserved&&tls.device==d&&tls.recording&&tls.freshCount81<tls.fresh81.size();
   }
  }
 }
 if(grant){const auto stats=d->worker->stats();
  grant=stats.phase==Worker::Phase::Open&&!stats.hostInflight&&
   d->descriptorEpoch81.load(std::memory_order_acquire)==fresh.epoch&&
   activeMetadata81(d,tls.activeNative81,fresh.workerGeneration);
 }
 if(grant){tls.fresh81[tls.freshCount81++]=fresh;++tls.descriptorAllocations81;}
 else if(preserved){auto drain=enter(d,Worker::AccessKind::Resource,"descriptorAllocationReceipt");}
 errno=saved;
}
bool initialDescriptorUpdate81(Device*d,unsigned count,const VkWriteDescriptorSet*w,unsigned copies,const VkCopyDescriptorSet*c,unsigned&slot)noexcept{
 slot=16;if(tls.device!=d||!tls.recording||!d->descriptorEligible81.load(std::memory_order_acquire)||!count||count>16||!w||copies||c)return false;
 const auto epoch=d->descriptorEpoch81.load(std::memory_order_acquire);
 for(unsigned n=0;n<tls.freshCount81;++n)if(tls.fresh81[n].set==w[0].dstSet){slot=n;break;}
 if(slot>=tls.freshCount81)return false;const auto&fresh=tls.fresh81[slot];
 if(fresh.updated||fresh.epoch!=epoch||fresh.workerGeneration!=tls.recording.generation())return false;
 std::uint16_t written{};
 for(unsigned n=0;n<count;++n){const auto&v=w[n];
  if(v.sType!=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET||v.pNext||v.dstSet!=fresh.set||v.dstBinding>=16||v.dstArrayElement||
   v.descriptorCount!=1||v.descriptorType!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC||!v.pBufferInfo||v.pImageInfo||v.pTexelBufferView||
   !v.pBufferInfo->buffer||v.pBufferInfo->offset||!v.pBufferInfo->range||v.pBufferInfo->range==VK_WHOLE_SIZE||
   !(fresh.bindings&(1u<<v.dstBinding))||(written&(1u<<v.dstBinding)))return false;
  written|=std::uint16_t(1u<<v.dstBinding);
 }
 if((written&fresh.required)!=fresh.required)return false;
 std::lock_guard<std::mutex>lock(d->descriptorMetadata81);
 const auto p=d->descriptorPools81.find(fresh.pool);const auto l=d->descriptorLayouts81.find(fresh.layout);
 return p!=d->descriptorPools81.end()&&l!=d->descriptorLayouts81.end()&&
  p->second.generation==fresh.poolGeneration&&l->second.generation==fresh.layoutGeneration;
}
void completedDescriptorUpdate81(Device*d,const FreshDescriptor81&receipt,bool preserved)noexcept{
 const int saved=errno;
 unsigned slot=16;for(unsigned n=0;n<tls.freshCount81;++n)if(sameDescriptorReceipt81(tls.fresh81[n],receipt)){slot=n;break;}
 if(preserved&&tls.device==d&&tls.recording&&slot<tls.freshCount81&&
  receipt.epoch==d->descriptorEpoch81.load(std::memory_order_acquire)&&
  receipt.workerGeneration==tls.recording.generation()&&!tls.fresh81[slot].updated){
  tls.fresh81[slot].updated=true;++tls.descriptorUpdates81;
 }else if(preserved){auto drain=enter(d,Worker::AccessKind::Resource,"descriptorUpdateReceipt");}
 errno=saved;
}
bool coveredFreshDescriptorBind89(Device*d,unsigned count,const VkDescriptorSet*sets)noexcept{
 if(tls.device!=d||!count||count>7||!sets||!tls.recording)return false;
 const auto epoch=d->descriptorEpoch81.load(std::memory_order_acquire),generation=tls.recording.generation();
 for(unsigned n=0;n<count;++n){bool found=false;
  for(unsigned k=0;k<tls.freshCount81;++k){const auto&fresh=tls.fresh81[k];
   found|=fresh.set==sets[n]&&fresh.updated&&fresh.epoch==epoch&&fresh.workerGeneration==generation;
  }
  if(!found)return false;
 }
 return true;
}
bool consumeFreshDescriptorBind81(Device*d,unsigned count,const VkDescriptorSet*sets,unsigned offsetsCount,const unsigned*offsets)noexcept{
 if(tls.device!=d||!tls.freshCount81)return true;
 const int saved=errno;
 // Match the existing serializer's bounds before reading any caller array.
 if(count>7||offsetsCount>16||(count&&!sets)||(offsetsCount&&!offsets)||!tls.recording){errno=saved;return false;}
 const auto epoch=d->descriptorEpoch81.load(std::memory_order_acquire),generation=tls.recording.generation();
 if(!activeMetadata81(d,tls.activeNative81,generation)){errno=saved;return false;}
 for(unsigned n=0;n<tls.freshCount81;++n){const auto&fresh=tls.fresh81[n];
  if(fresh.epoch!=epoch||fresh.workerGeneration!=generation){errno=saved;return false;}
 }
 // Remove before any packet publication, including rejected serializer shapes
 // and the synchronous fallback. A bound set can never regain fresh authority.
 for(unsigned n=0;n<count;++n)for(unsigned k=0;k<tls.freshCount81;){
  if(tls.fresh81[k].set==sets[n]){tls.fresh81[k]=tls.fresh81[--tls.freshCount81];++tls.descriptorPublications81;}
  else ++k;
 }
 errno=saved;return true;
}
void retireDescriptorPool81(Device*d,VkDescriptorPool pool,bool reset)noexcept{
 std::lock_guard<std::mutex>lock(d->descriptorMetadata81);auto p=d->descriptorPools81.find(pool);
 if(p==d->descriptorPools81.end())return;
 if(reset){p->second=DescriptorPool81{descriptorSerial81(d)};}else d->descriptorPools81.erase(p);
}
void retireDescriptorLayout81(Device*d,VkDescriptorSetLayout layout)noexcept{
 retireLayoutObservation82(d,layout);
 std::lock_guard<std::mutex>lock(d->descriptorMetadata81);d->descriptorLayouts81.erase(layout);
}

#ifdef RDR2_LAYER28_TESTING
void(*deferredHook76)(unsigned){};void(*preparationHook80)(){};
#endif
bool startPendingPrelude77(Device*d,unsigned)noexcept{return tls.device==d&&bool(tls.pending76.command);}
void finishPendingPrelude77(Device*d,unsigned kind,VkResult result)noexcept{
 if(tls.device==d&&kind==2)tls.pendingQueryResult79=std::uint32_t(result);
 if(tls.device==d&&tls.pending76.command&&result!=VK_SUCCESS)cancelPending77(3,kind==1?"vkResetDescriptorPool":"vkGetQueryPoolResults");
}
void finishPendingHostQueryReset78(Device*,VkQueryPool,unsigned,unsigned)noexcept{}
bool startPendingTimestamp79(Device*d,VkCommandBuffer c,VkPipelineStageFlagBits,VkQueryPool,unsigned)noexcept{
 firstAdmissionBeforeCommand77(d,c,"vkCmdWriteTimestamp");return tls.device==d&&bool(tls.pending76.command);
}
void finishPendingTimestamp79(Device*)noexcept{}
void firstAdmissionBeforeCommand77(ThreadContext&t,Device*d,VkCommandBuffer c,const char*api)noexcept{
 if(t.device!=d||!t.pending76.command)return;
 if(t.preparationDepth80){cancelPending77(t,2,"nestedCommandPreparation");return;}
 if(c!=t.pending76.command){cancelPending77(t,5,api);return;}
 if(std::strcmp(api,"vkCmdBeginRenderPass")!=0){
  if(!preparingApi80(Worker::AccessKind::Command,api))cancelPending77(t,2,api);
  return;
 }
 if(c!=t.pending76.command){cancelPending77(t,5,api);return;}
 const auto token=t.pending76;const auto beginResult=t.pendingBeginResult76;
 const auto baseline=t.pendingHostEntries76;
 t.pending76={};t.pendingBeginResult76=VK_ERROR_INITIALIZATION_FAILED;
 if(!d->worker||baseline==UINT64_MAX||d->worker->stats().synchronousEntries!=baseline){cancelPending77(t,4,"hostEntryCounterBeforeCommand");return;}
 // The entire approved native prefix has finished synchronously. This is the
 // FIRST admission for the original successful Begin, with a fresh counted
 // Begin receipt. All native calls ran once; no token resumption occurs.
 auto guard=d->worker->enter(Worker::AccessKind::Begin);
 std::lock_guard<std::mutex> lock(d->metadata);
 auto command=d->commands.find(token.command);auto pool=d->pools.find(token.pool);
 if(beginResult!=VK_SUCCESS||!d->alive.load(std::memory_order_acquire)||!d->qualified.load(std::memory_order_acquire)||d->mode.load()!=1||
    d->selectedProducer.load()!=identity(t)||t.command!=token.command||t.commandGeneration!=token.commandGeneration||
    token.device!=d->handle||token.deviceGeneration!=d->generation||command==d->commands.end()||pool==d->pools.end()||
    !pool->second.eligible||pool->second.generation!=token.poolGeneration||!command->second.primary||!command->second.recording||
    command->second.thread!=identity(t)||command->second.pool!=token.pool||command->second.poolGeneration!=token.poolGeneration||
    command->second.generation!=token.commandGeneration||command->second.recordingGeneration!=token.recordingGeneration||
    d->selectedPool!=token.pool||d->selectedPoolGeneration!=token.poolGeneration){t.deferredDecision76=5;return;}
#ifdef RDR2_LAYER28_TESTING
 if(deferredHook76)deferredHook76(1);
#endif
 if(d->worker->stats().synchronousEntries!=baseline+1){t.deferredDecision76=6;return;}
 t.beginAttempted=true;t.recording=guard.tryBegin(token,dispatch13(d),beginResult,&t.admission74);
 t.activeNative81=t.recording?token:Token{};
 t.beginAdmitted=bool(t.recording);if(!t.beginAdmitted)t.beginMask|=16384u;
 t.deferredDecision76=t.beginAdmitted?7u:9u;d->recordingActive=bool(t.recording);
#ifdef RDR2_LAYER28_TESTING
 if(deferredHook76)deferredHook76(2);
#endif
 // A foreign call can finish between the check and release-CAS. The existing
 // monotonic entry counter detects even that completed call. Close before the
 // producer can enqueue ANY packet; ordinary foreign guards remain unchanged.
 if(t.recording&&d->worker->stats().synchronousEntries!=baseline+1){
  auto cancel=d->worker->enter(Worker::AccessKind::Resource);
  t.recording={};d->recordingActive=false;t.deferredDecision76=8;
 }
}
InstanceEntries::InstanceEntries(Instance*i):instance(i->shared_from_this()){
 if(passthroughActive90())return; // Permanent pass-through takes no device receipts
 std::unique_lock<std::mutex> lock(i->childrenMutex);
 if(i->hostEntries.fetch_add(1,std::memory_order_relaxed)==UINT64_MAX)std::terminate();
 try{owners.reserve(i->children.size());receipts.reserve(i->children.size());
  for(auto&w:i->children)if(auto d=w.lock())if(d->alive.load(std::memory_order_acquire))owners.push_back(std::move(d));}
 catch(...){for(auto&w:i->children)if(auto d=w.lock())layer28::revoke(d.get(),Decline::MetadataAllocation);owners.clear();receipts.clear();return;}
 lock.unlock();
 for(auto&d:owners)receipts.push_back(enter(d.get(),Worker::AccessKind::Resource));
}
InstanceEntries::~InstanceEntries(){if(passthroughActive90())return;receipts.clear();instance->hostEntries.fetch_sub(1,std::memory_order_release);}
void InstanceEntries::revoke(Decline why)noexcept{for(auto&d:owners)layer28::revoke(d.get(),why);}

VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateInstance(const VkInstanceCreateInfo*c,const VkAllocationCallbacks*a,VkInstance*out){
 auto*l=link<VkLayerInstanceCreateInfo>(c->pNext,VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);if(!l||!l->u.pLayerInfo)return VK_ERROR_INITIALIZATION_FAILED;
 auto*chain=l->u.pLayerInfo;auto next=chain->pfnNextGetInstanceProcAddr;auto fn=reinterpret_cast<PFN_vkCreateInstance>(next(VK_NULL_HANDLE,"vkCreateInstance"));if(!fn)return VK_ERROR_INITIALIZATION_FAILED;
 std::shared_ptr<Instance> state;try{state=std::make_shared<Instance>();std::lock_guard<std::mutex>lock(registryMutex);instances.push_back(state);}catch(...){return VK_ERROR_OUT_OF_HOST_MEMORY;}
 {std::lock_guard<std::mutex>lock(configMutex);if(!configured){auto*e=std::getenv("RDR2_WORKER28_ENABLE");enabled=e&&std::strcmp(e,"1")==0;configured=true;passthrough90.store(!enabled,std::memory_order_relaxed);}instanceStarted=true;
  state->qualified=enabled;state->decline=enabled?0:std::uint32_t(Decline::Disabled);}
 state->customAllocator=!allocatorSafe(a);
 if(!instanceExtensions(c)){state->qualified=false;state->decline=std::uint32_t(Decline::Extension);}
 if(!instanceChain(c)){state->qualified=false;state->decline=std::uint32_t(Decline::Chain);}
 if(!qualifiedTerminator(reinterpret_cast<PFN_vkVoidFunction>(next))){state->qualified=false;state->decline=std::uint32_t(Decline::LoaderChain);}
 if(!state->qualified.load())creationDiagnostic("instance",state->decline,c->pNext,c->enabledLayerCount,c->ppEnabledLayerNames,c->enabledExtensionCount,c->ppEnabledExtensionNames,a!=nullptr);
 state->next=next;state->nextPhysical=chain->pfnNextGetPhysicalDeviceProcAddr;l->u.pLayerInfo=chain->pNext;
 const auto result=fn(c,a,out);{std::lock_guard<std::mutex>lock(registryMutex);if(result!=VK_SUCCESS)instances.remove(state);else state->handle=*out;}
 return result;
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyInstance(VkInstance h,const VkAllocationCallbacks*a){if(!h)return;auto*i=instanceFor(h);InstanceEntries entries_(i);entries_.revoke(Decline::LoaderChain);
 auto fn=reinterpret_cast<PFN_vkDestroyInstance>(i->next(h,"vkDestroyInstance"));i->alive=false;fn(h,a);
 // entries_ drops the last owner at return and frees the Instance. A child
 // device still alive here (vkDestroyInstance before vkDestroyDevice) must not keep
 // a back reference into it: clear it on every live child under childrenMutex, with
 // registryMutex nested (no path takes them in the reverse order) so the read in
 // retireDevice90 is ordered against this write.
 {std::lock_guard<std::mutex>children(i->childrenMutex);std::lock_guard<std::mutex>lock(registryMutex);
  for(auto&w:i->children)if(auto child=w.lock())child->instance.reset();i->children.clear();
  instances.remove_if([&](auto&v){return v.get()==i;});}}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateDevice(VkPhysicalDevice p,const VkDeviceCreateInfo*c,const VkAllocationCallbacks*a,VkDevice*out){
 auto*i=instanceFor(p);InstanceEntries guarded(i);auto*l=link<VkLayerDeviceCreateInfo>(c->pNext,VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);if(!l||!l->u.pLayerInfo)return VK_ERROR_INITIALIZATION_FAILED;
 auto*chain=l->u.pLayerInfo;auto next=chain->pfnNextGetDeviceProcAddr;auto fn=reinterpret_cast<PFN_vkCreateDevice>(chain->pfnNextGetInstanceProcAddr(i->handle,"vkCreateDevice"));require(fn);
 std::shared_ptr<Device>d;try{d=std::make_shared<Device>();}catch(...){return VK_ERROR_OUT_OF_HOST_MEMORY;}
 // d->instance is a weak back reference; `guarded` owns the Instance for this whole call.
 d->instance=i->weak_from_this();d->next=next;d->generation=nextDevice.fetch_add(1);d->qualified=i->qualified.load();d->decline=i->decline.load();
 if(!d->generation||d->generation==UINT64_MAX)revoke(d.get(),Decline::Overflow);
 if(!allocatorSafe(a))revoke(d.get(),Decline::Allocator);if(!deviceExtensions(c))revoke(d.get(),Decline::Extension);if(!deviceChain(c))revoke(d.get(),Decline::Chain);
 if(!qualifiedTerminator(reinterpret_cast<PFN_vkVoidFunction>(next))||!qualifiedTerminator(reinterpret_cast<PFN_vkVoidFunction>(chain->pfnNextGetInstanceProcAddr)))revoke(d.get(),Decline::LoaderChain);
 // Null device allocator inherits the instance policy; exact retail callbacks
 // qualify only through the real Android module/code/GOT certificate.
 if(!a&&i->customAllocator)revoke(d.get(),Decline::Allocator);
 if(!d->qualified.load())creationDiagnostic("device",d->decline,c->pNext,c->enabledLayerCount,c->ppEnabledLayerNames,c->enabledExtensionCount,c->ppEnabledExtensionNames,a!=nullptr);
 try{{std::lock_guard<std::mutex>child(i->childrenMutex);i->children.erase(std::remove_if(i->children.begin(),i->children.end(),[](auto&w){return w.expired();}),i->children.end());i->children.emplace_back(d);}
  std::lock_guard<std::mutex>lock(registryMutex);devices.push_back(d);}catch(...){return VK_ERROR_OUT_OF_HOST_MEMORY;}
 l->u.pLayerInfo=chain->pNext;const auto result=fn(p,c,a,out);if(result!=VK_SUCCESS){std::lock_guard<std::mutex>lock(registryMutex);devices.remove(d);return result;}
 {std::lock_guard<std::mutex>lock(registryMutex);d->handle=*out;d->key=dispatchKey(*out);}
 auto device=*out;auto&dispatch=d->dispatch;(void)dispatch;
 // Generated cold dispatch table; no string lookup occurs on queued draws.
 {auto&d=dispatch;
#include "GeneratedLoadDevice.inc"
 }
 // a permanently passing-through layer never owns a worker, so no admission can ever succeed.
 if(d->qualified.load()&&!passthroughActive90())d->worker=Worker::create(*out,d->generation);
 if(d->qualified.load()&&!d->worker)revoke(d.get(),Decline::MetadataAllocation);
 {std::lock_guard<std::mutex> publication(i->childrenMutex);
 d->ready.store(true,std::memory_order_release);
 // A foreign instance entry may have observed this child before its worker
 // existed. Its count spans original native work, so do not admit this device
 // if such an entry is still in flight at publication. Later entries observe
 // ready and retain the actual worker receipt before their native call.
 // The same mutex orders each foreign entry's count + child snapshot against
 // publication. Release/acquire on two separate atomics alone is insufficient.
 if(i->hostEntries.load(std::memory_order_acquire)>1||!i->qualified.load(std::memory_order_acquire))revoke(d.get(),Decline::LoaderChain);}
 if(!d->qualified.load(std::memory_order_acquire)&&d->worker)d->worker->revoke();
 return result;
}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyDevice(VkDevice h,const VkAllocationCallbacks*a){if(!h)return;
 if(passthroughActive90()){auto*p=passthroughDeviceFor90(h);auto pfn=p->dispatch.vkDestroyDevice;retireDevice90(p);pfn(h,a);return;}
 auto*d=deviceFor(h);
 {auto guard=enter(d,Worker::AccessKind::Shutdown);revoke(d,Decline::LoaderChain);d->alive=false;}
 // Vulkan requires all host accesses to this device's children to have ended.
 // This is the outer application quiescence point, not a worker close receipt.
 if(d->worker)d->worker->shutdownAfterApplicationQuiescence();auto fn=d->dispatch.vkDestroyDevice;
 retireDevice90(d);clearRecording();tls.device=nullptr;tls.dispatchKey=nullptr;fn(h,a);
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkCreateCommandPool(VkDevice h,const VkCommandPoolCreateInfo*c,const VkAllocationCallbacks*a,VkCommandPool*out){
 if(passthroughActive90()){auto*p=passthroughDeviceFor90(h);return p->dispatch.vkCreateCommandPool(h,c,a,out);}
 auto*d=deviceFor(h);auto guard=enter(d,Worker::AccessKind::Lifecycle,"vkCreateCommandPool");if(!allocatorSafe(a))revoke(d,Decline::Allocator);
 auto result=d->dispatch.vkCreateCommandPool(h,c,a,out);if(result==VK_SUCCESS)try{std::lock_guard<std::mutex>lock(d->metadata);d->pools[*out]={serial(d),c&&!c->pNext&&(c->flags&~(VK_COMMAND_POOL_CREATE_TRANSIENT_BIT|VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT))==0,{}};}catch(...){revoke(d,Decline::MetadataAllocation);}return result;}
VKAPI_ATTR void VKAPI_CALL wrap_vkDestroyCommandPool(VkDevice h,VkCommandPool pool,const VkAllocationCallbacks*a){
 if(passthroughActive90()){auto*p=passthroughDeviceFor90(h);p->dispatch.vkDestroyCommandPool(h,pool,a);return;}
 auto*d=deviceFor(h);auto guard=enter(d,Worker::AccessKind::Lifecycle,"vkDestroyCommandPool");if(!allocatorSafe(a))revoke(d,Decline::Allocator);
 {std::lock_guard<std::mutex>lock(d->metadata);for(auto it=d->commands.begin();it!=d->commands.end();)if(it->second.pool==pool)it=d->commands.erase(it);else++it;d->pools.erase(pool);if(d->selectedPool==pool){d->selectedPool={};d->selectedPoolGeneration=0;}}d->dispatch.vkDestroyCommandPool(h,pool,a);}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkResetCommandPool(VkDevice h,VkCommandPool pool,VkCommandPoolResetFlags flags){
 if(passthroughActive90()){auto*p=passthroughDeviceFor90(h);return p->dispatch.vkResetCommandPool(h,pool,flags);}
 auto*d=deviceFor(h);auto guard=enter(d,Worker::AccessKind::Lifecycle);auto r=d->dispatch.vkResetCommandPool(h,pool,flags);
 // Only this pool's own command buffers are touched, not every tracked one.
 if(r==VK_SUCCESS){std::lock_guard<std::mutex>lock(d->metadata);auto p=d->pools.find(pool);if(p!=d->pools.end()){for(auto c:p->second.commands){auto it=d->commands.find(c);if(it!=d->commands.end()&&it->second.pool==pool){it->second.recording=false;it->second.recordingGeneration=serial(d);}}}}return r;}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkAllocateCommandBuffers(VkDevice h,const VkCommandBufferAllocateInfo*c,VkCommandBuffer*out){
 if(passthroughActive90()){auto*p=passthroughDeviceFor90(h);return p->dispatch.vkAllocateCommandBuffers(h,c,out);}
 auto*d=deviceFor(h);auto guard=enter(d,Worker::AccessKind::Lifecycle);auto r=d->dispatch.vkAllocateCommandBuffers(h,c,out);
 if(r==VK_SUCCESS)try{optionalAllocationPoint();std::lock_guard<std::mutex>lock(d->metadata);auto p=d->pools.find(c->commandPool);for(unsigned n=0;n<c->commandBufferCount;++n){Command b{};b.pool=c->commandPool;b.generation=serial(d);b.poolGeneration=p==d->pools.end()?0:p->second.generation;b.primary=!c->pNext&&c->level==VK_COMMAND_BUFFER_LEVEL_PRIMARY;d->commands[out[n]]=b;if(p!=d->pools.end())p->second.commands.push_back(out[n]);}}catch(...){revoke(d,Decline::MetadataAllocation);}return r;}
VKAPI_ATTR void VKAPI_CALL wrap_vkFreeCommandBuffers(VkDevice h,VkCommandPool pool,uint32_t n,const VkCommandBuffer*c){
 if(passthroughActive90()){auto*p=passthroughDeviceFor90(h);p->dispatch.vkFreeCommandBuffers(h,pool,n,c);return;}
 auto*d=deviceFor(h);auto guard=enter(d,Worker::AccessKind::Lifecycle);
 {std::lock_guard<std::mutex>lock(d->metadata);for(unsigned j=0;j<n;++j){auto it=d->commands.find(c[j]);if(it==d->commands.end())continue;auto p=d->pools.find(it->second.pool);
  if(p!=d->pools.end()){auto&list=p->second.commands;list.erase(std::remove(list.begin(),list.end(),c[j]),list.end());}d->commands.erase(it);}}d->dispatch.vkFreeCommandBuffers(h,pool,n,c);}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkBeginCommandBuffer(VkCommandBuffer c,const VkCommandBufferBeginInfo*b){
 if(passthroughActive90()){auto*p=passthroughDeviceFor90(c);return p->dispatch.vkBeginCommandBuffer(c,b);}
 auto*d=deviceFor(c);auto guard=enter(d,Worker::AccessKind::Begin,"vkBeginCommandBuffer");clearRecording();BeginDepth84 depth84;const auto modeTicket84=snapshotMode84(d,c,b);const auto input84=modeTicket84.valid?modeInput84():ModeInput84{};
 const auto r=d->dispatch.vkBeginCommandBuffer(c,b);if(r!=VK_SUCCESS)return r;const int beginErrno84=errno;tls.modeReceipt84=completedMode84(d,modeTicket84,input84);errno=beginErrno84;d->recordings.fetch_add(1);Token token{};bool eligible=false;
 tls.pending76={};tls.pendingBeginResult76=VK_ERROR_INITIALIZATION_FAILED;tls.pendingHostEntries76=0;tls.preparingInflight80=false;tls.preparationEntries80=0;tls.preparationCommands80=0;tls.deferredDecision76=0;tls.pendingReset77=false;tls.pendingPrelude77=0;tls.pendingQueries77=0;tls.pendingHostQueryResets78=0;tls.pendingTimestamps79=0;tls.pendingQueryResult79=0;tls.pendingHostQueryPool79={};tls.pendingHostResetFirst79=0;tls.pendingHostResetCount79=0;tls.pendingCancel77=nullptr;
 tls.allocation82={};if(tls.observationBeginSerial82!=UINT64_MAX)++tls.observationBeginSerial82;
 tls.descriptorAllocations81=tls.descriptorUpdates81=tls.descriptorPublications81=0;
 tls.admission74={};tls.ownedClose74=nullptr;tls.ownedClosePhase74=0;tls.ownedClosePrefix74=0;tls.beginNonce74=diagnosticNonce74.load(std::memory_order_acquire);
 tls.beginMask=1;tls.beginAttempted=false;tls.beginAdmitted=false;tls.beginProducer=identity();tls.beginSelectedProducer=d->selectedProducer.load();tls.beginPool={};tls.beginSelectedPool={};
 {std::lock_guard<std::mutex>lock(d->metadata);auto it=d->commands.find(c);if(it!=d->commands.end()){auto&v=it->second;v.recording=true;v.draws=0;v.thread=identity();v.recordingGeneration=serial(d);auto pool=d->pools.find(v.pool);
  // Original Begin host receipt has drained the previous recording. Retain the
  // fixed producer, but select its CURRENT qualified primary pool incarnation.
  if(v.primary&&pool!=d->pools.end()&&pool->second.eligible&&pool->second.generation==v.poolGeneration&&d->selectedProducer.load()==identity()){
   d->selectedPool=v.pool;d->selectedPoolGeneration=v.poolGeneration;}
  eligible=v.primary&&pool!=d->pools.end()&&pool->second.eligible&&pool->second.generation==v.poolGeneration&&d->selectedPool==v.pool&&d->selectedPoolGeneration==v.poolGeneration&&b&&!b->pNext&&!b->pInheritanceInfo&&!(b->flags&VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT);
  tls.beginPool=v.pool;tls.beginSelectedPool=d->selectedPool;
  tls.beginMask=(!v.primary?2u:0u)|(pool==d->pools.end()?4u:0u)|(pool!=d->pools.end()&&!pool->second.eligible?8u:0u)|(pool!=d->pools.end()&&pool->second.generation!=v.poolGeneration?16u:0u)|(d->selectedPool!=v.pool?32u:0u)|(d->selectedPoolGeneration!=v.poolGeneration?64u:0u)|(!b?128u:0u)|(b&&b->pNext?256u:0u)|(b&&b->pInheritanceInfo?512u:0u)|(b&&(b->flags&VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT)?1024u:0u);
  token={d->handle,v.pool,c,d->generation,v.poolGeneration,v.generation,v.recordingGeneration};tls.commandGeneration=v.generation;}}
 tls.modeNative84=token;tls.command=c;tls.commandDraws=0;d->recordingActive=false;d->firstCloseApi=nullptr;d->firstClosePrefix=0;
 if(d->worker){auto s=d->worker->stats();d->recordingBaseQueued=s.enqueued;d->recordingBaseExecuted=s.executed;tls.localQueuedBefore=s.enqueued;tls.localExecutedBefore=s.executed;tls.pendingHostEntries76=s.synchronousEntries;}
 if(!d->qualified.load(std::memory_order_acquire))tls.beginMask|=2048u;
 if(d->mode.load()!=1)tls.beginMask|=4096u;
 if(d->selectedProducer.load()!=identity())tls.beginMask|=8192u;
 if(eligible&&d->worker&&d->qualified.load(std::memory_order_acquire)&&d->mode.load()==1&&d->selectedProducer.load()==identity()){
  if(!tls.preparationDepth80){tls.pending76=token;tls.pendingBeginResult76=r;tls.deferredDecision76=1;}
  else{tls.deferredDecision76=11;tls.pendingCancel77="nestedNativeBeginPreparation";}
 }
 d->recordingActive=bool(tls.recording);
 return r;
}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkEndCommandBuffer(VkCommandBuffer c){
 if(passthroughActive90()){auto*p=passthroughDeviceFor90(c);return p->dispatch.vkEndCommandBuffer(c);}
 auto*d=deviceFor(c);auto guard=enter(d,Worker::AccessKind::Lifecycle,"vkEndCommandBuffer");const auto r=d->dispatch.vkEndCommandBuffer(c);
 if(tls.command==c)d->draws.fetch_add(tls.commandDraws,std::memory_order_relaxed);
 if(r==VK_SUCCESS){std::lock_guard<std::mutex>lock(d->metadata);auto it=d->commands.find(c);if(it!=d->commands.end()){auto&v=it->second;
  if(v.primary&&v.recording&&v.thread==identity()&&tls.command==c&&tls.commandGeneration==v.generation&&tls.commandDraws>=128){auto zero=std::uint64_t{};d->selectedProducer.compare_exchange_strong(zero,identity());if(d->selectedProducer==identity()&&!d->selectedPool){d->selectedPool=v.pool;d->selectedPoolGeneration=v.poolGeneration;}}v.recording=false;}}
 if(d->worker){auto s=d->worker->stats();d->lastRecordQueued=s.enqueued-d->recordingBaseQueued.load();d->lastRecordExecuted=s.executed-d->recordingBaseExecuted.load();}
 d->lastRecordDraws=tls.command==c?tls.commandDraws:0;d->recordingActive=false;
#ifdef __ANDROID__
 if(tls.command==c&&tls.commandDraws>=128&&tls.query85.seen&&tls.query85.nonce&&
    (tls.largeRecords<8||(tls.largeRecords+1)%300==0)){
  const auto&o=tls.query85;const auto&t=o.native;
  writeDiagnostic82(__android_log_write,ANDROID_LOG_INFO,"RdrWorker85","device=%p command=%p pool=%p pid=%u tid=%d producer=%llu nonce=%llu deviceGeneration=%llu poolGeneration=%llu commandGeneration=%llu recordingGeneration=%llu workerGeneration=%llu prefix=%llu queryCaller=%llu queryPool=%p first=%u count=%u dataSize=%llu stride=%llu flags=%u dataNonNull=%u resultKnown=%u result=%d scopeValid=%u resetSeen=%u resetCaller=%llu resetPool=%p resetFirst=%u resetCount=%u resetApi=%u resetReturned=%u resetScopeValid=%u draws=%llu",
   reinterpret_cast<void*>(d->handle),reinterpret_cast<void*>(c),reinterpret_cast<void*>(t.pool),unsigned(getpid()),gettid(),
   static_cast<unsigned long long>(tls.beginProducer),static_cast<unsigned long long>(o.nonce),
   static_cast<unsigned long long>(t.deviceGeneration),static_cast<unsigned long long>(t.poolGeneration),
   static_cast<unsigned long long>(t.commandGeneration),static_cast<unsigned long long>(t.recordingGeneration),
   static_cast<unsigned long long>(o.workerGeneration),static_cast<unsigned long long>(o.prefix),
   static_cast<unsigned long long>(o.queryCaller),reinterpret_cast<void*>(o.queryPool),o.first,o.count,
   static_cast<unsigned long long>(o.dataSize),static_cast<unsigned long long>(o.stride),o.flags,o.dataNonNull,
   o.resultKnown,int(o.result),o.scopeValid,o.resetSeen,static_cast<unsigned long long>(o.resetCaller),
   reinterpret_cast<void*>(o.resetPool),o.resetFirst,o.resetCount,o.resetApi,o.resetReturned,o.resetScopeValid,
   static_cast<unsigned long long>(tls.commandDraws));
 }

 if(tls.command==c&&tls.commandDraws>=128&&d->worker&&tls.modeReceipt84.applied&&tls.modeReceipt84.leaseGeneration){
  const auto&m=tls.modeReceipt84;
  if(tls.modeLogNonce84!=m.nonce){tls.modeLogNonce84=m.nonce;tls.modeLogRecords84=0;}
  if(tls.modeLogRecords84!=UINT64_MAX)++tls.modeLogRecords84;
  if(tls.modeLogRecords84<=8||tls.modeLogRecords84%10==0){
   unsigned snapshotMode{};std::uint64_t snapshotGeneration{},snapshotNonce{};
   {std::lock_guard<std::mutex>lock(d->metadata);snapshotMode=d->mode.load();snapshotGeneration=d->modeLeaseGeneration84;snapshotNonce=d->modeNonce84;}
   const auto local=d->worker->stats();const auto&t=tls.modeNative84;
   writeDiagnostic82(__android_log_write,ANDROID_LOG_INFO,"RdrWorker84","device=%p command=%p pid=%u tid=%d producer=%llu selectedProducer=%llu mode=%u leased=%u nonce=%llu leaseGeneration=%llu deadlineMs=%llu sampleMs=%llu applied=%u appliedCounter=%llu deviceGeneration=%llu poolGeneration=%llu commandGeneration=%llu recordingGeneration=%llu draws=%llu queuedDelta=%llu snapshotMode=%u snapshotLeaseGeneration=%llu snapshotNonce=%llu",
    reinterpret_cast<void*>(d->handle),reinterpret_cast<void*>(c),m.pid,gettid(),
    static_cast<unsigned long long>(tls.beginProducer),static_cast<unsigned long long>(d->selectedProducer.load()),m.mode,m.leased,
    static_cast<unsigned long long>(m.nonce),static_cast<unsigned long long>(m.leaseGeneration),static_cast<unsigned long long>(m.deadlineMs),
    static_cast<unsigned long long>(m.sampleMs),m.applied,static_cast<unsigned long long>(m.appliedCounter),
    static_cast<unsigned long long>(t.deviceGeneration),static_cast<unsigned long long>(t.poolGeneration),
    static_cast<unsigned long long>(t.commandGeneration),static_cast<unsigned long long>(t.recordingGeneration),
    static_cast<unsigned long long>(tls.commandDraws),static_cast<unsigned long long>(local.enqueued-tls.localQueuedBefore),
    snapshotMode,static_cast<unsigned long long>(snapshotGeneration),static_cast<unsigned long long>(snapshotNonce));
  }
 }

 if(tls.command==c&&tls.commandDraws>=128&&d->worker){
  const auto quota=diagnosticNonce74.load(std::memory_order_acquire);
  if(tls.quotaNonce74!=quota){tls.quotaNonce74=quota;tls.largeRecords=0;}
  ++tls.largeRecords;
  if(tls.largeRecords<=8||tls.largeRecords%300==0){
   const auto local=d->worker->stats();
   writeDiagnostic82(__android_log_write,ANDROID_LOG_INFO,"RdrWorker82","device=%p command=%p tid=%d producer=%llu selectedProducerAtBegin=%llu pool=%p selectedPoolAtBegin=%p mask=%u attempted=%u admitted=%u draws=%llu cumulativeQueued=%llu cumulativeExecuted=%llu localQueuedDelta=%llu admissions=%llu declinedAdmissions=%llu phase=%u hostInflight=%llu permanentlyRevoked=%u nonce=%llu admissionReason=%u admissionWord=%llu deferredDecision=%u pendingClose=%s preludeQueries=%u preludeHostQueryResets=%u preludeTimestamps=%u preludeQueryResult=%u preparationEntries=%u preparationCommands=%u descriptorAllocations=%u descriptorUpdates=%u descriptorPublications=%u allocationObserved=%u allocationShape=%u allocationEntry=%u allocationCompletion=%u allocationSetCount=%u allocationFactsAvailable=%u allocationPoolKnown=%u allocationPoolIncarnation=%llu allocationPoolFlags=%u allocationPoolMaxSets=%u allocationPoolType=%u allocationPoolDescriptors=%u allocationPoolSizeCount=%u allocationPoolAllocator=%u allocationPoolNext=%u allocationPoolShape=%u allocationLayoutKnown=%u allocationLayoutIncarnation=%llu allocationLayoutFlags=%u allocationLayoutBindings=%u allocationLayoutDynamic=%u allocationLayoutOther=%u allocationLayoutBindingFlags=%u allocationLayoutStages=%u allocationLayoutNext=%u allocationLayoutShape=%u allocationResultKnown=%u allocationResult=%u allocationSetNonNull=%u allocationReceipt=%u ownedClose=%s ownedClosePhase=%llu ownedClosePrefix=%llu",
    reinterpret_cast<void*>(d->handle),reinterpret_cast<void*>(c),gettid(),static_cast<unsigned long long>(tls.beginProducer),static_cast<unsigned long long>(tls.beginSelectedProducer),reinterpret_cast<void*>(tls.beginPool),reinterpret_cast<void*>(tls.beginSelectedPool),tls.beginMask,unsigned(tls.beginAttempted),unsigned(tls.beginAdmitted),static_cast<unsigned long long>(tls.commandDraws),static_cast<unsigned long long>(local.enqueued),static_cast<unsigned long long>(local.executed),static_cast<unsigned long long>(local.enqueued-tls.localQueuedBefore),static_cast<unsigned long long>(local.admissions),static_cast<unsigned long long>(local.declinedAdmissions),unsigned(local.phase),static_cast<unsigned long long>(local.hostInflight),unsigned(local.permanentlyRevoked),static_cast<unsigned long long>(tls.beginNonce74),tls.admission74.firstFailure,static_cast<unsigned long long>(tls.admission74.rawAdmission),tls.deferredDecision76,tls.pendingCancel77?tls.pendingCancel77:"none",tls.pendingQueries77,tls.pendingHostQueryResets78,tls.pendingTimestamps79,tls.pendingQueryResult79,tls.preparationEntries80,tls.preparationCommands80,tls.descriptorAllocations81,tls.descriptorUpdates81,tls.descriptorPublications81,tls.allocation82.seen,tls.allocation82.shape,tls.allocation82.entry,tls.allocation82.completion,tls.allocation82.sets,tls.allocation82.available,tls.allocation82.poolKnown,static_cast<unsigned long long>(tls.allocation82.pool.incarnation),tls.allocation82.pool.flags,tls.allocation82.pool.maxSets,tls.allocation82.pool.type,tls.allocation82.pool.descriptors,tls.allocation82.pool.sizeCount,tls.allocation82.pool.allocator,tls.allocation82.pool.next,tls.allocation82.pool.shape,tls.allocation82.layoutKnown,static_cast<unsigned long long>(tls.allocation82.layout.incarnation),tls.allocation82.layout.flags,tls.allocation82.layout.bindings,tls.allocation82.layout.dynamic,tls.allocation82.layout.other,tls.allocation82.layout.bindingFlags,tls.allocation82.layout.stages,tls.allocation82.layout.next,tls.allocation82.layout.shape,tls.allocation82.resultKnown,tls.allocation82.result,tls.allocation82.setNonNull,tls.allocation82.receipt,tls.ownedClose74?tls.ownedClose74:"none",static_cast<unsigned long long>(tls.ownedClosePhase74),static_cast<unsigned long long>(tls.ownedClosePrefix74));
  }
 }
 const bool firstQueuedRecord=d->lastRecordQueued.load()&&!d->queuedRecordLogged.exchange(true,std::memory_order_relaxed);
 if(firstQueuedRecord||d->recordings.load()%300==0){
  auto close=d->firstCloseApi.load();__android_log_print(ANDROID_LOG_INFO,"RdrWorker28","record=%llu queued=%llu executed=%llu draws=%llu firstClose=%s prefix=%llu qualified=%u decline=%u",
   static_cast<unsigned long long>(d->recordings.load()),static_cast<unsigned long long>(d->lastRecordQueued.load()),static_cast<unsigned long long>(d->lastRecordExecuted.load()),static_cast<unsigned long long>(d->lastRecordDraws.load()),close?close:"none",static_cast<unsigned long long>(d->firstClosePrefix.load()),unsigned(d->qualified.load()),d->decline.load());}
#endif
 if(tls.command==c)clearRecording();return r;}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkResetCommandBuffer(VkCommandBuffer c,VkCommandBufferResetFlags flags){
 if(passthroughActive90()){auto*p=passthroughDeviceFor90(c);return p->dispatch.vkResetCommandBuffer(c,flags);}
 auto*d=deviceFor(c);auto guard=enter(d,Worker::AccessKind::Lifecycle);const auto r=d->dispatch.vkResetCommandBuffer(c,flags);
 if(r==VK_SUCCESS){std::lock_guard<std::mutex>lock(d->metadata);auto it=d->commands.find(c);if(it!=d->commands.end()){it->second.recording=false;it->second.recordingGeneration=serial(d);}}if(tls.command==c)clearRecording();return r;}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL wrap_vkGetDeviceProcAddr(VkDevice h,const char*n){if(!h||!n)return nullptr;auto*d=deviceFor(h);auto guard=enter(d,Worker::AccessKind::Lookup,"vkGetDeviceProcAddr");auto fn=d->next(h,n);if(!fn)return nullptr;
 auto*e=lookup(n);if(e&&e->device)return e->function;d->unknownEscapes.fetch_add(1);revoke(d,Decline::UnknownProc);return fn;}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL wrap_vkGetInstanceProcAddr(VkInstance h,const char*n){if(!n)return nullptr;
 if(std::strcmp(n,"vk_layerGetPhysicalDeviceProcAddr")==0)return reinterpret_cast<PFN_vkVoidFunction>(rdrWorker28GetPhysicalDeviceProcAddr);
 if(std::strcmp(n,"vkGetInstanceProcAddr")==0)return reinterpret_cast<PFN_vkVoidFunction>(wrap_vkGetInstanceProcAddr);
 if(std::strcmp(n,"vkCreateInstance")==0)return reinterpret_cast<PFN_vkVoidFunction>(wrap_vkCreateInstance);
 if(std::strcmp(n,"vkEnumerateInstanceLayerProperties")==0)return reinterpret_cast<PFN_vkVoidFunction>(wrap_vkEnumerateInstanceLayerProperties);
 if(std::strcmp(n,"vkEnumerateInstanceExtensionProperties")==0)return reinterpret_cast<PFN_vkVoidFunction>(wrap_vkEnumerateInstanceExtensionProperties);
 if(!h)return nullptr;auto*i=instanceFor(h);InstanceEntries guarded(i);auto fn=i->next(h,n);if(!fn)return nullptr;auto*e=lookup(n);if(e)return e->function;
 i->qualified=false;i->decline=std::uint32_t(Decline::UnknownProc);guarded.revoke(Decline::UnknownProc);return fn;}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkEnumerateInstanceLayerProperties(uint32_t*n,VkLayerProperties*out){if(!n)return VK_ERROR_INITIALIZATION_FAILED;if(!out){*n=1;return VK_SUCCESS;}
 if(!*n)return VK_INCOMPLETE;VkLayerProperties p{};std::strncpy(p.layerName,Name,sizeof(p.layerName)-1);std::strncpy(p.description,"RDR2 guarded raw Vulkan recording worker",sizeof(p.description)-1);p.specVersion=VK_API_VERSION_1_3;p.implementationVersion=1;*out=p;*n=1;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkEnumerateDeviceLayerProperties(VkPhysicalDevice,uint32_t*n,VkLayerProperties*out){return wrap_vkEnumerateInstanceLayerProperties(n,out);}
VKAPI_ATTR VkResult VKAPI_CALL wrap_vkEnumerateInstanceExtensionProperties(const char*name,uint32_t*n,VkExtensionProperties*){if(name&&std::strcmp(name,Name)!=0)return VK_ERROR_LAYER_NOT_PRESENT;if(!n)return VK_ERROR_INITIALIZATION_FAILED;*n=0;return VK_SUCCESS;}
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance i,const char*n){return rdr2::layer28::wrap_vkGetInstanceProcAddr(i,n);}
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice d,const char*n){return rdr2::layer28::wrap_vkGetDeviceProcAddr(d,n);}
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL rdrWorker28GetPhysicalDeviceProcAddr(VkInstance h,const char*n){using namespace rdr2::layer28;if(!h||!n)return nullptr;auto*e=lookup(n);if((e&&!e->physical)||std::strcmp(n,"vkEnumerateInstanceVersion")==0)return nullptr;auto*i=instanceFor(h);InstanceEntries guard(i);auto physical=i->nextPhysical;if(!physical)physical=reinterpret_cast<PFN_GetPhysicalDeviceProcAddr>(i->next(h,"vk_layerGetPhysicalDeviceProcAddr"));auto fn=physical?physical(h,n):(e?i->next(h,n):nullptr);if(!fn)return nullptr;if(e)return e->function;i->qualified=false;i->decline=std::uint32_t(Decline::UnknownProc);guard.revoke(Decline::UnknownProc);return fn;}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t*n,VkLayerProperties*p){return rdr2::layer28::wrap_vkEnumerateInstanceLayerProperties(n,p);}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char*l,uint32_t*n,VkExtensionProperties*p){return rdr2::layer28::wrap_vkEnumerateInstanceExtensionProperties(l,n,p);}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice d,uint32_t*n,VkLayerProperties*p){return rdr2::layer28::wrap_vkEnumerateDeviceLayerProperties(d,n,p);}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface*v){
 if(!v||v->sType!=LAYER_NEGOTIATE_INTERFACE_STRUCT||v->loaderLayerInterfaceVersion<1)return VK_ERROR_INITIALIZATION_FAILED;
 if(v->loaderLayerInterfaceVersion>2)v->loaderLayerInterfaceVersion=2;v->pfnGetInstanceProcAddr=vkGetInstanceProcAddr;v->pfnGetDeviceProcAddr=vkGetDeviceProcAddr;v->pfnGetPhysicalDeviceProcAddr=rdrWorker28GetPhysicalDeviceProcAddr;return VK_SUCCESS;}
// on: 0 = recording disabled, 1 = enabled (full admission protocol), 2 = explicit
// permanent pass-through (identical to 0; older layers reject 2, so a caller that
// asks for it cannot silently end up paying the protocol). With 0/2 the layer
// never creates a worker and every wrapper forwards directly.
extern "C" RDR_W28_EXPORT uint32_t rdrWorker28ConfigureOnce(uint32_t abi,uint32_t on){using namespace rdr2::layer28;if(abi!=1||on>2)return 0;const bool want=on==1;std::lock_guard<std::mutex>lock(configMutex);if(instanceStarted)return 0;if(configured)return enabled==want;configured=true;enabled=want;passthrough90.store(!enabled,std::memory_order_relaxed);return 1;}
#ifdef RDR2_LAYER28_TESTING
extern "C" RDR_W28_EXPORT void rdrWorker28TestFailOptionalMetadata(){rdr2::layer28::failOptionalMetadata=true;}
namespace rdr2::layer28 {thread_local std::uint64_t testOriginalCaller86{};}
extern "C" void rdrWorker86TestCaller(std::uint64_t value){rdr2::layer28::testOriginalCaller86=value;}
extern "C" std::uint64_t rdrWorker85TestFact(VkDevice h,unsigned which){
 using namespace rdr2::layer28;deviceFor(h);const auto&o=tls.query85;
 switch(which){case 0:return o.seen;case 1:return o.queryCaller;case 2:return o.first;case 3:return o.count;
 case 4:return o.dataSize;case 5:return o.stride;case 6:return o.flags;case 7:return o.dataNonNull;
 case 8:return o.resultKnown;case 9:return std::uint64_t(std::int64_t(o.result));case 10:return o.scopeValid;
 case 11:return o.prefix;case 12:return o.resetSeen;case 13:return o.resetCaller;case 14:return o.resetFirst;
 case 15:return o.resetCount;case 16:return o.resetApi;case 17:return o.resetReturned;case 18:return o.resetScopeValid;
 case 19:return o.nonce;case 20:return o.native.recordingGeneration;default:return UINT64_MAX;}
}

extern "C" void rdrWorker84TestInput(unsigned pid,std::uint64_t ms,const char*property){
 using namespace rdr2::layer28;testModeInput84={};testModeInput84.pid=pid;testModeInput84.ms=ms;
 if(property)std::strncpy(testModeInput84.property.data(),property,testModeInput84.property.size()-1);
}
extern "C" std::uint64_t rdrWorker84TestFact(VkDevice h,unsigned which){
 using namespace rdr2::layer28;auto*d=deviceFor(h);std::lock_guard<std::mutex>lock(d->metadata);
 switch(which){case 0:return d->mode.load();case 1:return d->modeLeaseGeneration84;case 2:return d->modeNonce84;
 case 3:return tls.modeReceipt84.applied;case 4:return tls.modeReceipt84.deadlineMs;case 5:return tls.modeReceipt84.sampleMs;
 case 6:return tls.modeReceipt84.appliedCounter;case 7:return tls.beginDepth84;default:return UINT64_MAX;}
}

extern "C" RDR_W28_EXPORT uint32_t rdrWorker31TestLastBeginMask(){return rdr2::layer28::tls.beginMask;}
// 0 = the live device holds no instance reference (cleared), 1 = it holds
// an expired one, 2 = its instance is alive.
extern "C" RDR_W28_EXPORT uint32_t rdrWorker90TestDeviceInstance(VkDevice h){using namespace rdr2::layer28;auto*d=deviceFor(h);std::lock_guard<std::mutex>lock(registryMutex);
 const std::weak_ptr<Instance> none;if(!d->instance.owner_before(none)&&!none.owner_before(d->instance))return 0;return d->instance.expired()?1:2;}
#endif
extern "C" RDR_W28_EXPORT uint32_t rdrWorker28OwnsEntry(PFN_vkVoidFunction p){using namespace rdr2::layer28;if(!p)return 0;
 if(p==reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr)||p==reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr)||p==reinterpret_cast<PFN_vkVoidFunction>(vkNegotiateLoaderLayerInterfaceVersion)||p==reinterpret_cast<PFN_vkVoidFunction>(rdrWorker28GetPhysicalDeviceProcAddr)||p==reinterpret_cast<PFN_vkVoidFunction>(vkEnumerateInstanceLayerProperties)||p==reinterpret_cast<PFN_vkVoidFunction>(vkEnumerateInstanceExtensionProperties)||p==reinterpret_cast<PFN_vkVoidFunction>(vkEnumerateDeviceLayerProperties))return 1;for(const auto&e:entries)if(e.function==p)return 1;return 0;}
extern "C" RDR_W28_EXPORT uint32_t rdrWorker28Prepare(VkDevice h){using namespace rdr2::layer28;auto*d=deviceFor(h);return d->worker?1:0;}
extern "C" RDR_W28_EXPORT uint32_t rdrWorker28SetRecordingMode(VkDevice h,uint32_t m){using namespace rdr2::layer28;if(m>1)return 0;auto*d=deviceFor(h);auto guard=enter(d,Worker::AccessKind::Resource);d->mode=m;return 1;}
extern "C" RDR_W28_EXPORT void* rdrWorker28GetWorkerNativeHandle(VkDevice h){using namespace rdr2::layer28;auto*d=deviceFor(h);if(!d->worker)return nullptr;return reinterpret_cast<void*>(d->worker->nativeHandle());}
extern "C" RDR_W28_EXPORT uint32_t rdrWorker28Drain(VkDevice h){using namespace rdr2::layer28;auto*d=deviceFor(h);auto guard=enter(d,Worker::AccessKind::Resource);return 1;}
extern "C" RDR_W28_EXPORT uint32_t rdrWorker28Snapshot(VkDevice h,RdrWorker28Stats*out){using namespace rdr2::layer28;if(!out||out->size!=sizeof(*out)||out->abi!=1)return 0;auto*d=deviceFor(h);
 auto s=d->worker?d->worker->stats():Worker::Stats{};RdrWorker28Stats r{};r.size=sizeof(r);r.abi=1;r.enabled=enabled;r.qualified=d->qualified;r.mode=d->mode;r.declineReason=d->decline;r.producerSelected=d->selectedProducer!=0;r.drained=s.enqueued==s.executed&&s.phase!=Worker::Phase::Open&&s.phase!=Worker::Phase::Closing;
 r.deviceGeneration=d->generation;r.recordings=d->recordings;r.queued=s.enqueued;r.executed=s.executed;r.admissions=s.admissions;r.closeRequests=s.closeRequests;r.unknownEscapes=d->unknownEscapes;r.originalDraws=d->draws;r.selectedProducer=d->selectedProducer;r.workerGeneration=s.generation;r.publications=s.publications;
 r.lastRecordQueued=d->lastRecordQueued;r.lastRecordExecuted=d->lastRecordExecuted;r.lastRecordDraws=d->lastRecordDraws;r.queuedPrefixAtFirstClose=d->firstClosePrefix;
 if(auto close=d->firstCloseApi.load())std::strncpy(r.firstCloseApi,close,sizeof(r.firstCloseApi)-1);*out=r;return 1;}

extern "C" RDR_W28_EXPORT void rdrWorker74DiagnosticNonce(uint64_t nonce){rdr2::layer28::diagnosticNonce74.store(nonce,std::memory_order_release);}
#ifdef RDR2_LAYER28_TESTING
extern "C" RDR_W28_EXPORT uint32_t rdrWorker74TestLastAdmissionReason(){return rdr2::layer28::tls.admission74.firstFailure;}
extern "C" RDR_W28_EXPORT const char* rdrWorker74TestOwnedClose(){return rdr2::layer28::tls.ownedClose74;}
#endif

#ifdef RDR2_LAYER28_TESTING
extern "C" RDR_W28_EXPORT uint32_t rdrWorker76TestDecision(){return rdr2::layer28::tls.deferredDecision76;}
extern "C" RDR_W28_EXPORT void rdrWorker76TestHook(void(*hook)(unsigned)){rdr2::layer28::deferredHook76=hook;}
extern "C" RDR_W28_EXPORT void rdrWorker79TestTimestampHook(void(*hook)()){(void)hook;}
extern "C" RDR_W28_EXPORT void rdrWorker80TestPreparationHook(void(*hook)()){rdr2::layer28::preparationHook80=hook;}
extern "C" RDR_W28_EXPORT std::uint64_t rdrWorker83TestPoolFacts(VkDevice h,VkDescriptorPool pool,unsigned which){
 using namespace rdr2::layer28;auto*d=deviceFor(h);std::lock_guard<std::mutex>lock(d->descriptorMetadata81);
 const auto c=d->descriptorPoolCertificates83.find(pool);const auto p=d->descriptorPools81.find(pool);
 switch(which){case 0:return c==d->descriptorPoolCertificates83.end()?0:c->second.creation;
 case 1:return c==d->descriptorPoolCertificates83.end()?0:c->second.incarnation;
 case 2:return p==d->descriptorPools81.end()?0:p->second.generation;
 case 3:return p==d->descriptorPools81.end()?UINT64_MAX:p->second.count;
 default:return UINT64_MAX;}
}
extern "C" RDR_W28_EXPORT void rdrWorker83TestLoseCertificate(VkDevice h,VkDescriptorPool pool){
 using namespace rdr2::layer28;auto*d=deviceFor(h);std::lock_guard<std::mutex>lock(d->descriptorMetadata81);
 d->descriptorPoolCertificates83.erase(pool);d->descriptorPools81.erase(pool);
}
extern "C" RDR_W28_EXPORT void rdrWorker83TestSerialOverflow(VkDevice h){
 using namespace rdr2::layer28;auto*d=deviceFor(h);std::lock_guard<std::mutex>lock(d->descriptorMetadata81);d->descriptorSerial81=UINT64_MAX;
}

extern "C" RDR_W28_EXPORT std::uint64_t rdrWorker82TestFact(VkDevice h,unsigned which){
 using namespace rdr2::layer28;deviceFor(h);switch(which){case 0:return tls.allocation82.seen;case 1:return tls.allocation82.shape;case 2:return tls.allocation82.entry;case 3:return tls.allocation82.completion;case 4:return tls.allocation82.sets;case 5:return tls.allocation82.available;case 6:return tls.allocation82.poolKnown;case 7:return tls.allocation82.pool.incarnation;case 8:return tls.allocation82.pool.flags;case 9:return tls.allocation82.pool.maxSets;case 10:return tls.allocation82.pool.type;case 11:return tls.allocation82.pool.descriptors;case 12:return tls.allocation82.pool.sizeCount;case 13:return tls.allocation82.pool.allocator;case 14:return tls.allocation82.pool.next;case 15:return tls.allocation82.pool.shape;case 16:return tls.allocation82.layoutKnown;case 17:return tls.allocation82.layout.incarnation;case 18:return tls.allocation82.layout.flags;case 19:return tls.allocation82.layout.bindings;case 20:return tls.allocation82.layout.dynamic;case 21:return tls.allocation82.layout.other;case 22:return tls.allocation82.layout.bindingFlags;case 23:return tls.allocation82.layout.stages;case 24:return tls.allocation82.layout.next;case 25:return tls.allocation82.layout.shape;case 26:return tls.allocation82.resultKnown;case 27:return tls.allocation82.result;case 28:return tls.allocation82.setNonNull;case 29:return tls.allocation82.receipt;default:return UINT64_MAX;}
}
extern "C" RDR_W28_EXPORT std::uint64_t rdrWorker82TestPoolIncarnation(VkDevice h,VkDescriptorPool pool){
 using namespace rdr2::layer28;auto*d=deviceFor(h);std::lock_guard<std::mutex>lock(d->descriptorMetadata81);const auto p=d->poolObservations82.find(pool);return d->observationAvailable82&&p!=d->poolObservations82.end()?p->second.incarnation:0;
}
extern "C" RDR_W28_EXPORT std::uint64_t rdrWorker81TestFacts(VkDevice h,unsigned which){
 using namespace rdr2::layer28;auto*d=deviceFor(h);const auto stats=d->worker->stats();
 switch(which){case 0:return stats.hostInflight;case 1:return std::uint64_t(stats.phase);
 case 2:return tls.descriptorAllocations81;case 3:return tls.descriptorUpdates81;case 4:return tls.descriptorPublications81;
 case 5:return tls.freshCount81;case 6:return stats.synchronousEntries;
 case 7:{std::lock_guard<std::mutex>lock(d->descriptorMetadata81);return d->descriptorPools81.count(reinterpret_cast<VkDescriptorPool>(std::uintptr_t(99)));}
 case 8:{std::lock_guard<std::mutex>lock(d->descriptorMetadata81);return d->descriptorLayouts81.count(reinterpret_cast<VkDescriptorSetLayout>(std::uintptr_t(301)));}
 default:return UINT64_MAX;}
}
extern "C" RDR_W28_EXPORT void rdrWorker81TestCorruptActive(VkDevice h,unsigned which){
 using namespace rdr2::layer28;auto*d=deviceFor(h);auto&t=tls.activeNative81;
 switch(which){case 0:t.device=nullptr;break;case 1:t.pool=nullptr;break;case 2:t.command=nullptr;break;
 case 3:++t.deviceGeneration;break;case 4:++t.poolGeneration;break;case 5:++t.commandGeneration;break;
 case 6:++t.recordingGeneration;break;case 7:d->descriptorEpoch81.fetch_add(1);break;}
}
extern "C" RDR_W28_EXPORT VkResult rdrWorker80TestMovePreparation(VkDevice h,VkDescriptorPool pool){
 using namespace rdr2::layer28;auto*d=deviceFor(h);auto first=enter(d,Worker::AccessKind::Command,"vkResetDescriptorPool");
 PreparationEntry80 second(std::move(first)),third;third=std::move(second);
 return d->dispatch.vkResetDescriptorPool(h,pool,0);
}
extern "C" RDR_W28_EXPORT void rdrWorker76TestCorruptPending(unsigned which){using namespace rdr2::layer28;
 switch(which){case 0:tls.pending76.device={};break;case 1:++tls.pending76.deviceGeneration;break;
 case 2:++tls.pending76.poolGeneration;break;case 3:++tls.pending76.commandGeneration;break;
 case 4:++tls.pending76.recordingGeneration;break;case 5:tls.pending76.pool={};break;
 case 6:tls.pendingBeginResult76=VK_ERROR_OUT_OF_HOST_MEMORY;break;case 7:++tls.commandGeneration;break;}
}
#endif
