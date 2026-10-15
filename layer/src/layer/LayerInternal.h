#pragma once
#if defined(_WIN32)
#include <windows.h>
#endif
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include "LayerControl.h"
#include "WorkerModeControl84.h"
#include "../pool/WorkerPool28.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <list>
#include <cstdint>
#include <cstdlib>
#include <type_traits>
#if defined(_MSC_VER)
#include <intrin.h>
#define RDR_QUERY_CALLER85 reinterpret_cast<std::uintptr_t>(_ReturnAddress())
#else
#define RDR_QUERY_CALLER85 reinterpret_cast<std::uintptr_t>(__builtin_extract_return_addr(__builtin_return_address(0)))
#endif
// the per-thread context must stay constant-initialised and trivially
// destructible so that no TLS guard/init routine runs on every access.
#if defined(__clang__)
#define RDR2_LAYER_CONSTINIT [[clang::require_constant_initialization]]
#else
#define RDR2_LAYER_CONSTINIT
#endif
#if defined(__clang__) || defined(__GNUC__)
#define RDR2_LAYER_HIDDEN __attribute__((visibility("hidden")))
#else
#define RDR2_LAYER_HIDDEN
#endif

namespace rdr2::layer28 {
using Worker=rdr2::worker28::WorkerPool28;
using Token=rdr2::worker28::PoolCommandToken;
using Dispatch13=rdr2::worker28::Dispatch13;
enum class Decline:std::uint32_t {None,Disabled,Allocator,Callback,Extension,Chain,MetadataAllocation,UnknownProc,PoolShape,BeginShape,Overflow,LoaderChain};
struct DeviceDispatch {
#include "GeneratedDeviceDispatch.inc"
};
struct Instance;
// Each pool keeps its own command-buffer list so a pool
// reset/destroy walks only its own command buffers, not every tracked one of the device.
struct Pool {std::uint64_t generation{};bool eligible{};std::vector<VkCommandBuffer> commands;};
struct Command {VkCommandPool pool{};std::uint64_t generation{},poolGeneration{},recordingGeneration{},draws{},thread{};bool primary{},recording{};};
struct DescriptorPool81 {
 std::uint64_t generation{};unsigned count{};
 std::array<VkDescriptorSet,256> allocated{};
};
struct DescriptorPoolCertificate83 {std::uint64_t creation{},incarnation{};};
struct DescriptorPoolReset83 {std::uint64_t creation{},incarnation{},epoch{};};
struct DescriptorLayout81 {std::uint64_t generation{};std::uint16_t bindings{},required{};};
struct FreshDescriptor81 {
 VkDescriptorSet set{};VkDescriptorPool pool{};VkDescriptorSetLayout layout{};
 std::uint64_t poolGeneration{},layoutGeneration{},allocation{},epoch{},workerGeneration{};
 std::uint16_t bindings{},required{};bool updated{};
};

struct DescriptorPoolObservation82 {
 std::uint64_t incarnation{};
 unsigned flags{},maxSets{},type{},descriptors{},sizeCount{},allocator{},next{},shape{};
};
struct DescriptorLayoutObservation82 {
 std::uint64_t incarnation{};
 unsigned flags{},bindings{},dynamic{},other{},bindingFlags{},stages{},next{},shape{};
};
struct AllocationObservation82 {
 unsigned seen{},shape{},entry{},completion{},sets{},available{},poolKnown{},layoutKnown{};
 unsigned resultKnown{},result{},setNonNull{},receipt{};
 DescriptorPoolObservation82 pool{};DescriptorLayoutObservation82 layout{};
};
struct Device;
DescriptorPoolObservation82 poolObservation82(const VkDescriptorPoolCreateInfo*,const VkAllocationCallbacks*,bool)noexcept;
DescriptorLayoutObservation82 layoutObservation82(const VkDescriptorSetLayoutCreateInfo*,bool)noexcept;
void createdPoolObservation82(Device*,VkDescriptorPool,const DescriptorPoolObservation82&,std::uint64_t)noexcept;
void createdLayoutObservation82(Device*,VkDescriptorSetLayout,const DescriptorLayoutObservation82&,std::uint64_t)noexcept;
void retirePoolObservation82(Device*,VkDescriptorPool,bool)noexcept;
void retireLayoutObservation82(Device*,VkDescriptorSetLayout)noexcept;

struct ModeReceipt84 {unsigned mode{1},leased{},applied{},pid{};std::uint64_t nonce{},leaseGeneration{},deadlineMs{},sampleMs{},appliedCounter{};};
struct PoolBirthState87 {
 // Only descriptor resource calls participate. No draw or command packet
 // performs this bookkeeping. All slots are fixed and require no allocation.
 std::array<VkDescriptorPool,64> operations{};
 std::array<VkDescriptorPool,128> changed{};
 unsigned births{},changedCount{};bool available{true};
};

struct Device {
 // key == dispatchKey(handle); published under registryMutex together with
 // handle, before vkCreateDevice returns the handle to the application.
 // instance is a weak back reference (the Instance may be destroyed
 // first); after publication it is read and cleared only under registryMutex.
 VkDevice handle{};void*key{};std::uint64_t generation{};std::weak_ptr<Instance> instance;PFN_vkGetDeviceProcAddr next{};DeviceDispatch dispatch{};
 std::unique_ptr<Worker> worker;std::mutex metadata;std::unordered_map<VkCommandPool,Pool> pools;std::unordered_map<VkCommandBuffer,Command> commands;
 std::atomic<bool> alive{true},qualified{false},ready{false};std::atomic<std::uint32_t> mode{1},decline{0};
 rdr2::diagnostic::WorkerModeControl modeControl84;bool modeControlActivated84{},modeControlDisabled84{};std::uint64_t modeNonce84{},modeLastNonce84{},modeLeaseGeneration84{};
 std::atomic<std::uint64_t> selectedProducer{},recordings{},draws{},unknownEscapes{};std::uint64_t serial{1};
 std::atomic<std::uint64_t> descriptorEpoch81{1};std::atomic<bool> descriptorEligible81{true};
 std::unordered_map<VkDescriptorPool,DescriptorPoolObservation82> poolObservations82;
 std::unordered_map<VkDescriptorSetLayout,DescriptorLayoutObservation82> layoutObservations82;
 std::uint64_t observationSerial82{1};bool observationAvailable82{true};
 PoolBirthState87 poolBirth87;
#ifdef __ANDROID__
 std::atomic<unsigned> birthLogs87{};unsigned missingPoolLogs87{};
#endif
 std::mutex descriptorMetadata81;std::uint64_t descriptorSerial81{1};
 std::unordered_map<VkDescriptorPool,DescriptorPool81> descriptorPools81;
 std::unordered_map<VkDescriptorPool,DescriptorPoolCertificate83> descriptorPoolCertificates83;
 std::unordered_map<VkDescriptorSetLayout,DescriptorLayout81> descriptorLayouts81;
 VkCommandPool selectedPool{};std::uint64_t selectedPoolGeneration{};
 std::atomic<bool> recordingActive{};std::atomic<const char*> firstCloseApi{};
 std::atomic<std::uint64_t> recordingBaseQueued{},recordingBaseExecuted{},lastRecordQueued{},lastRecordExecuted{},lastRecordDraws{},firstClosePrefix{};
#ifdef __ANDROID__
 std::atomic<bool> queuedRecordLogged{};
#endif
};
struct Instance : std::enable_shared_from_this<Instance> {VkInstance handle{};PFN_vkGetInstanceProcAddr next{};PFN_GetPhysicalDeviceProcAddr nextPhysical{};
 std::atomic<std::uint64_t> hostEntries{};
 std::atomic<bool> qualified{false},alive{true};std::atomic<std::uint32_t> decline{};bool customAllocator{};std::mutex childrenMutex;std::vector<std::weak_ptr<Device>> children;};
// Observational only: original host calls and worker admission stay unchanged.
struct QueryObservation85 {
 Token native{};std::uint64_t serial{},nonce{},workerGeneration{},prefix{},queryCaller{},resetCaller{};
 VkQueryPool queryPool{},resetPool{};std::uint64_t dataSize{},stride{};
 unsigned seen{},first{},count{},flags{},dataNonNull{},resultKnown{},scopeValid{};
 VkResult result{VK_ERROR_INITIALIZATION_FAILED};
 unsigned resetSeen{},resetFirst{},resetCount{},resetApi{},resetReturned{},resetScopeValid{};
};
struct QueryTicket85 {Token native{};std::uint64_t serial{},nonce{},workerGeneration{};bool valid{};};
QueryTicket85 observeQueryBegin85(Device*,std::uint64_t,VkQueryPool,unsigned,unsigned,size_t,void*,VkDeviceSize,VkQueryResultFlags)noexcept;
void observeQueryEnd85(Device*,const QueryTicket85&,VkResult)noexcept;
QueryTicket85 observeResetBegin85(Device*,std::uint64_t,VkQueryPool,unsigned,unsigned,unsigned)noexcept;
void observeResetEnd85(Device*,const QueryTicket85&)noexcept;

struct OriginalQuerySequence86 {
 Token native{};std::uint64_t workerGeneration{},caller{},hostEntries{};VkQueryPool pool{};bool readCompleted{};
};

// This belongs to an original call, outside the resettable recording TLS
// (clearRecording never touches it). It observes only epoch increments
// performed inside that counted call.
struct OriginalBoundary89 {Device* device{};std::uint64_t expectedEpoch{};bool valid{};};
// No std::shared_ptr member: Device objects are retained for
// the process lifetime (retired at vkDestroyDevice, never freed), so a raw
// Device* plus the alive flag is sufficient and the context stays trivially
// destructible and constant-initialised (no __tls_init guard per access).
struct ThreadContext {Device*device{};void*dispatchKey{};VkCommandBuffer command{};Worker::RecordingToken recording;
 std::uint64_t identity{},commandDraws{},commandGeneration{};
 OriginalBoundary89 boundary89{};
 QueryObservation85 query85{};
 OriginalQuerySequence86 originalQuery86{};
 unsigned beginDepth84{};ModeReceipt84 modeReceipt84{};std::uint64_t modeLogNonce84{},modeLogRecords84{};Token modeNative84{};
 // Diagnostic receipts belong to this recording thread, unlike the
 // lastRecord fields that can be overwritten by another host thread.
 // Keep the successful native Begin unadmitted during bounded synchronous
 // preparation. FIRST admission is graphics BeginRenderPass; never resume a token.
 Token pending76{};VkResult pendingBeginResult76{VK_ERROR_INITIALIZATION_FAILED};
 std::uint64_t pendingHostEntries76{};std::uint32_t deferredDecision76{};
 bool preparingInflight80{};unsigned preparationDepth80{},preparationEntries80{},preparationCommands80{};
 bool pendingReset77{};unsigned pendingPrelude77{},pendingQueries77{},pendingHostQueryResets78{},pendingTimestamps79{};
 std::uint32_t pendingQueryResult79{};VkQueryPool pendingHostQueryPool79{};unsigned pendingHostResetFirst79{},pendingHostResetCount79{};const char* pendingCancel77{};
 std::uint64_t observationBeginSerial82{};AllocationObservation82 allocation82{};
 Token activeNative81{};std::array<FreshDescriptor81,16> fresh81{};unsigned freshCount81{};
 unsigned descriptorAllocations81{},descriptorUpdates81{},descriptorPublications81{};
 Worker::AdmissionReceipt74 admission74{};
 const char* ownedClose74{};std::uint64_t ownedClosePhase74{},ownedClosePrefix74{},beginNonce74{},quotaNonce74{};
 std::uint32_t beginMask{};bool beginAttempted{},beginAdmitted{};
 std::uint64_t beginProducer{},beginSelectedProducer{},localQueuedBefore{},localExecutedBefore{},largeRecords{};
 VkCommandPool beginPool{},beginSelectedPool{};
};
static_assert(std::is_trivially_destructible<ThreadContext>::value,"ThreadContext must stay trivially destructible (no TLS destructor registration)");
RDR2_LAYER_CONSTINIT extern thread_local ThreadContext tls;
Device*deviceForKey(void*);
Instance*instanceForKey(void*);
template<class H>void*dispatchKey(H h)noexcept{return h?*reinterpret_cast<void**>(h):nullptr;}
// Generated wrappers resolve the thread context once and pass it down.
template<class H>Device*deviceFor(ThreadContext&t,H h){auto*k=dispatchKey(h);if(t.device&&t.dispatchKey==k&&t.device->alive.load(std::memory_order_acquire))return t.device;return deviceForKey(k);}
template<class H>Device*deviceFor(H h){return deviceFor(tls,h);}
template<class H>Instance*instanceFor(H h){return instanceForKey(dispatchKey(h));}
// Permanent pass-through. Set once, before the first VkInstance
// exists, when the layer is configured with recording disabled
// (rdrWorker28ConfigureOnce(1,0|2) or no RDR2_WORKER28_ENABLE=1). While set,
// every wrapper forwards to the next layer with no enter()/finish()/HostEntry/
// epoch/descriptor bookkeeping, no worker is ever created, so no admission
// can ever succeed and no OPEN phase can exist on any thread. The per-call
// cost is one relaxed load of this flag plus the device cache below.
extern RDR2_LAYER_HIDDEN std::atomic<bool> passthrough90;
// One process-wide device cache (the game has one VkDevice; both the render
// thread and the stereo thread hit it read-only). Cleared under registryMutex
// by vkDestroyDevice before the handle may be reused. Device objects are never
// freed, so a stale pointer is always dereferenceable and fails the key test.
extern RDR2_LAYER_HIDDEN std::atomic<Device*> passthroughCache90;
Device*passthroughDevice90(void*key);
inline bool passthroughActive90()noexcept{return passthrough90.load(std::memory_order_relaxed);}
// Relaxed is sufficient: the application obtained the handle after
// vkCreateDevice returned (key/dispatch written before that), and Vulkan
// forbids using a device concurrently with its destruction.
template<class H>Device*passthroughDeviceFor90(H h)noexcept{auto*k=dispatchKey(h);auto*d=passthroughCache90.load(std::memory_order_relaxed);if(d&&d->key==k)return d;return passthroughDevice90(k);}
template<class T>void require(T value){if(!value)std::terminate();}
void revoke(Device*,Decline)noexcept;
bool allocatorSafe(const VkAllocationCallbacks*)noexcept;
bool standDebugUtils(const VkDebugUtilsMessengerCreateInfoEXT*,bool chainNode=false)noexcept;
bool standDebugReport(const VkDebugReportCallbackCreateInfoEXT*,bool chainNode=false)noexcept;
// A synchronous original host call before FIRST graphics admission. This
// receipt owns the real worker HostEntry; it cannot resume a RecordingToken.
class PreparationEntry80 {
 Worker::HostEntry guard_;Device* device_{};Token token_{};
 std::uint64_t baseline_{};const char* api_{};bool preparing_{};
 bool continuing89_{};std::uint64_t coreGeneration89_{};
 ThreadContext* tls_{}; // the constructing thread's context; the guard never changes threads
 void finish()noexcept;
public:
 PreparationEntry80()=default;
 PreparationEntry80(ThreadContext&,Worker::HostEntry&&,Device*,const Token&,std::uint64_t,const char*,bool,bool continuing89=false,std::uint64_t coreGeneration89=0)noexcept;
 ~PreparationEntry80(){finish();}
 PreparationEntry80(PreparationEntry80&&)noexcept;
 PreparationEntry80&operator=(PreparationEntry80&&)noexcept;
 PreparationEntry80(const PreparationEntry80&)=delete;
 PreparationEntry80&operator=(const PreparationEntry80&)=delete;
};

PreparationEntry80 enter(ThreadContext&,Device*,Worker::AccessKind,const char*api="instance/physical host access",VkCommandBuffer originalCommand89=VK_NULL_HANDLE)noexcept;
inline PreparationEntry80 enter(Device*d,Worker::AccessKind k,const char*api="instance/physical host access",VkCommandBuffer originalCommand89=VK_NULL_HANDLE)noexcept{return enter(tls,d,k,api,originalCommand89);}
void invalidateFreshDescriptors81(ThreadContext&,Device*)noexcept;
inline void invalidateFreshDescriptors81(Device*d)noexcept{invalidateFreshDescriptors81(tls,d);}
// A counted original host call on certified new descriptor objects. This does
// not confer resource ownership: actual Create/Allocate registry receipts do.
// This guard orders the application's original host call against earlier CPU
// recording calls. It grants no query-pool ownership or GPU completion fact.
class OriginalHostQuery86 {
 Worker::HostEntry guard_;PreparationEntry80 fallback_;Device*device_{};
 Token native_{};std::uint64_t baseline_{},epoch_{},workerGeneration_{};
 bool heldDepth_{},preserved_{},finished_{};
public:
 OriginalHostQuery86(Device*,const char*,bool)noexcept;
 ~OriginalHostQuery86(){complete();}
 OriginalHostQuery86(const OriginalHostQuery86&)=delete;
 OriginalHostQuery86&operator=(const OriginalHostQuery86&)=delete;
 bool complete(bool originalAccepted=true)noexcept;
};
bool originalObservedQueryShape86(VkQueryPool,unsigned,unsigned,size_t,void*,VkDeviceSize,VkQueryResultFlags)noexcept;
void originalQueryCompleted86(Device*,std::uint64_t,VkQueryPool,bool,VkResult)noexcept;
bool originalResetSequence86(Device*,std::uint64_t,VkQueryPool,unsigned,unsigned)noexcept;
#ifdef RDR2_LAYER28_TESTING
extern thread_local std::uint64_t testOriginalCaller86;
inline std::uint64_t originalCaller86(std::uint64_t actual)noexcept{return testOriginalCaller86?testOriginalCaller86:actual;}
#else
inline std::uint64_t originalCaller86(std::uint64_t actual)noexcept{return actual;}
#endif

class PoolOperation87 {
 Device*device_{};unsigned slot_{64};
public:
 PoolOperation87(Device*,VkDescriptorPool)noexcept;
 ~PoolOperation87();
 PoolOperation87(const PoolOperation87&)=delete;
 PoolOperation87&operator=(const PoolOperation87&)=delete;
};
class PoolBirth87 {
 Device*device_{};bool tracked_{},finished_{};
public:
 explicit PoolBirth87(Device*)noexcept;
 ~PoolBirth87();
 void complete(VkResult,VkDescriptorPool,bool,const DescriptorPoolObservation82&,std::uint64_t)noexcept;
 PoolBirth87(const PoolBirth87&)=delete;
 PoolBirth87&operator=(const PoolBirth87&)=delete;
};

class ActivePreparation81 {
 Worker::HostEntry guard_;PreparationEntry80 fallback_;Device*device_{};
 Token native_{};std::uint64_t baseline_{},epoch_{},workerGeneration_{};
 unsigned entryObservation82_{1},completionObservation82_{};
 bool heldDepth_{},preserved_{},finished_{};
public:
 ActivePreparation81(Device*,const char*)noexcept;
 ~ActivePreparation81(){complete();}
 ActivePreparation81(const ActivePreparation81&)=delete;
 ActivePreparation81&operator=(const ActivePreparation81&)=delete;
 unsigned entryObservation82()const noexcept{return entryObservation82_;}
 unsigned completionObservation82()const noexcept{return completionObservation82_;}
 bool complete()noexcept;
};
bool descriptorPoolShape81(const VkDescriptorPoolCreateInfo*,const VkAllocationCallbacks*)noexcept;
bool descriptorLayoutShape81(const VkDescriptorSetLayoutCreateInfo*,DescriptorLayout81&)noexcept;
bool descriptorAllocationShape81(Device*,const VkDescriptorSetAllocateInfo*,FreshDescriptor81&,bool observe82=false)noexcept;
void createdDescriptorPool81(Device*,VkDescriptorPool,bool,std::uint64_t originalEpoch)noexcept;
void createdDescriptorLayout81(Device*,VkDescriptorSetLayout,const DescriptorLayout81&,bool,std::uint64_t originalEpoch)noexcept;
void allocatedDescriptor81(Device*,VkDescriptorSet,const FreshDescriptor81&,bool)noexcept;
bool initialDescriptorUpdate81(Device*,unsigned,const VkWriteDescriptorSet*,unsigned,const VkCopyDescriptorSet*,unsigned&)noexcept;
void completedDescriptorUpdate81(Device*,const FreshDescriptor81&,bool)noexcept;
bool coveredFreshDescriptorBind89(Device*,unsigned,const VkDescriptorSet*)noexcept;
bool consumeFreshDescriptorBind81(Device*,unsigned,const VkDescriptorSet*,unsigned,const unsigned*)noexcept;
DescriptorPoolReset83 snapshotDescriptorPoolReset83(Device*,VkDescriptorPool,VkDescriptorPoolResetFlags)noexcept;
void completedDescriptorPoolReset83(Device*,VkDescriptorPool,const DescriptorPoolReset83&)noexcept;
void destroyedDescriptorPoolCertificate83(Device*,VkDescriptorPool)noexcept;
void retireDescriptorPool81(Device*,VkDescriptorPool,bool reset=false)noexcept;
void retireDescriptorLayout81(Device*,VkDescriptorSetLayout)noexcept;

inline void noteDraw(ThreadContext&t,Device*d,VkCommandBuffer c)noexcept{if(t.device==d&&t.command==c&&t.commandDraws!=UINT64_MAX)++t.commandDraws;}
inline void noteDraw(Device*d,VkCommandBuffer c)noexcept{noteDraw(tls,d,c);}
bool startPendingPrelude77(Device*,unsigned)noexcept;
void finishPendingPrelude77(Device*,unsigned,VkResult)noexcept;
void finishPendingHostQueryReset78(Device*,VkQueryPool,unsigned,unsigned)noexcept;
bool startPendingTimestamp79(Device*,VkCommandBuffer,VkPipelineStageFlagBits,VkQueryPool,unsigned)noexcept;
void finishPendingTimestamp79(Device*)noexcept;
void firstAdmissionBeforeCommand77(ThreadContext&,Device*,VkCommandBuffer,const char*)noexcept;
inline void firstAdmissionBeforeCommand77(Device*d,VkCommandBuffer c,const char*api)noexcept{firstAdmissionBeforeCommand77(tls,d,c,api);}
struct InstanceEntries {std::shared_ptr<Instance> instance;std::vector<std::shared_ptr<Device>> owners;std::vector<PreparationEntry80> receipts;
 explicit InstanceEntries(Instance*);~InstanceEntries();void revoke(Decline)noexcept;};
#include "GeneratedDeclarations.inc"
}
