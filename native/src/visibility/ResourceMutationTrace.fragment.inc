// PRIVATE TraceEntry anonymous namespace fragment, under HEAD_CAMERA.
// File-scope includes GameResourceMutation.h + NativeResourceMutationProof.h.
// Requires readMemory/mapping/Mapping/replacePointer/engineBase/patchFault.
#ifdef RDRVR_HEAD_CAMERA
rdr95::QueueUpdateOriginal originalResourceQueueUpdate{};
rdr95::StreamingCompleteOriginal originalResourceStreamingComplete{};
rdr95::SchedulerShutdownOriginal originalResourceSchedulerShutdown{};
rdr95::ResourceQueueApi resourceQueueApi{};
unsigned resourceMutationSlotsOwned{};
std::atomic<bool> resourceMutationHooksReady{};
__attribute__((noinline)) bool observedResourceQueueUpdate(void* scheduler){return rdr95::invokeResourceQueueUpdate(rdr95::nativeResourceGate,resourceQueueApi,originalResourceQueueUpdate,scheduler);}
__attribute__((noinline)) void observedResourceStreamingComplete(void* scheduler,void* streamable){rdr95::invokeResourceStreamingComplete(rdr95::nativeResourceGate,originalResourceStreamingComplete,scheduler,streamable);}
__attribute__((noinline)) void observedResourceSchedulerShutdown(){resourceMutationHooksReady.store(false,std::memory_order_release);rdr95::invokeResourceSchedulerShutdown(rdr95::nativeResourceGate,originalResourceSchedulerShutdown);}
[[maybe_unused]] void disarmResourceMutationHooks(){resourceMutationHooksReady.store(false,std::memory_order_release);rdr95::nativeResourceGate.disarm();}
[[maybe_unused]] bool removeResourceMutationHooks(){
 using namespace rdr95::resource_generated;rdr95::ResourceOriginalErrno preserve;disarmResourceMutationHooks();
 // Existing readers must leave BEFORE the first writer wrapper is removed.
 // Disarm alone revokes results but does not relinquish a physical borrow.
 rdr95::NativeResourceGate::Writer drain(rdr95::nativeResourceGate,rdr95::NativeResourceGate::Writer::Mode::Quiet);
 bool ok=true;
 if(resourceMutationSlotsOwned&4){if(replacePointer(engineBase+ShutdownGotOffset,reinterpret_cast<std::uintptr_t>(observedResourceSchedulerShutdown),reinterpret_cast<std::uintptr_t>(originalResourceSchedulerShutdown)))resourceMutationSlotsOwned&=~4u;else ok=false;}
 if(resourceMutationSlotsOwned&2){if(replacePointer(engineBase+CompleteGotOffset,reinterpret_cast<std::uintptr_t>(observedResourceStreamingComplete),reinterpret_cast<std::uintptr_t>(originalResourceStreamingComplete)))resourceMutationSlotsOwned&=~2u;else ok=false;}
 if(resourceMutationSlotsOwned&1){if(replacePointer(engineBase+UpdateGotOffset,reinterpret_cast<std::uintptr_t>(observedResourceQueueUpdate),reinterpret_cast<std::uintptr_t>(originalResourceQueueUpdate)))resourceMutationSlotsOwned&=~1u;else ok=false;}
 if(!ok)patchFault.store(true,std::memory_order_release);return ok;
 // Originals/API stay alive for already-fetched wrappers. Never clear here.
}
[[maybe_unused]] bool rearmResourceMutationHooks(){
 using namespace rdr95::resource_generated;rdr95::ResourceOriginalErrno preserve;disarmResourceMutationHooks();
 if(resourceMutationSlotsOwned!=7||!engineBase||engineBase>UINTPTR_MAX-BurstOffset-88||
 reinterpret_cast<std::uintptr_t>(originalResourceQueueUpdate)!=engineBase+UpdateOffset||
 reinterpret_cast<std::uintptr_t>(originalResourceStreamingComplete)!=engineBase+CompleteOffset||
 reinterpret_cast<std::uintptr_t>(originalResourceSchedulerShutdown)!=engineBase+ShutdownOffset)return false;
 for(const auto& entry:std::array<std::array<std::uintptr_t,2>,5>{{
  {{UpdateGotOffset,reinterpret_cast<std::uintptr_t>(observedResourceQueueUpdate)}},
  {{CompleteGotOffset,reinterpret_cast<std::uintptr_t>(observedResourceStreamingComplete)}},
  {{ShutdownGotOffset,reinterpret_cast<std::uintptr_t>(observedResourceSchedulerShutdown)}},
  {{SingletonGotOffset,engineBase+SingletonOffset}},{{BurstGotOffset,engineBase+BurstOffset}}}}){
  std::uintptr_t actual{};if(!readMemory(nullptr,engineBase+entry[0],&actual,sizeof actual)||actual!=entry[1])return false;
 }
 resourceMutationHooksReady.store(true,std::memory_order_release);return true;
 // Caller must combine this readiness with BOTH effect hooks and explicitly
 // setCoverage(Required); this fragment cannot invent complete owner coverage.
}
[[maybe_unused]] bool installResourceMutationHooks(){
 using namespace rdr95::resource_generated;rdr95::ResourceOriginalErrno preserve;disarmResourceMutationHooks();
 if(resourceMutationSlotsOwned||!engineBase||engineBase>UINTPTR_MAX-BurstOffset-88)return false;
 if(!resourceMutationCodeMatches(engineBase,[](std::uintptr_t a,void*p,std::size_t n){Mapping m{};return mapping(a,n,m)&&(m.protection&PROT_EXEC)&&readMemory(nullptr,a,p,n);} ))return false;
 Mapping instance{},burst{};std::uintptr_t si{},bu{};
 if(!mapping(engineBase+SingletonOffset,8,instance)||(instance.protection&(PROT_READ|PROT_WRITE))!=(PROT_READ|PROT_WRITE)||
 !mapping(engineBase+BurstOffset,88,burst)||(burst.protection&(PROT_READ|PROT_WRITE))!=(PROT_READ|PROT_WRITE)||
 !readMemory(nullptr,engineBase+SingletonGotOffset,&si,sizeof si)||si!=engineBase+SingletonOffset||
 !readMemory(nullptr,engineBase+BurstGotOffset,&bu,sizeof bu)||bu!=engineBase+BurstOffset)return false;
 std::uintptr_t update{},complete{},shutdown{};
 if(!readMemory(nullptr,engineBase+UpdateGotOffset,&update,8)||update!=engineBase+UpdateOffset||
 !readMemory(nullptr,engineBase+CompleteGotOffset,&complete,8)||complete!=engineBase+CompleteOffset||
 !readMemory(nullptr,engineBase+ShutdownGotOffset,&shutdown,8)||shutdown!=engineBase+ShutdownOffset)return false;
 originalResourceQueueUpdate=reinterpret_cast<rdr95::QueueUpdateOriginal>(update);
 originalResourceStreamingComplete=reinterpret_cast<rdr95::StreamingCompleteOriginal>(complete);
 originalResourceSchedulerShutdown=reinterpret_cast<rdr95::SchedulerShutdownOriginal>(shutdown);
 resourceQueueApi={reinterpret_cast<rdr95::NativeCritical>(engineBase+LockOffset),reinterpret_cast<rdr95::NativeCritical>(engineBase+UnlockOffset),reinterpret_cast<const void*>(bu),reinterpret_cast<const void*const*>(si),true};
 if(!replacePointer(engineBase+UpdateGotOffset,update,reinterpret_cast<std::uintptr_t>(observedResourceQueueUpdate)))return false;resourceMutationSlotsOwned=1;
 if(!replacePointer(engineBase+CompleteGotOffset,complete,reinterpret_cast<std::uintptr_t>(observedResourceStreamingComplete))){removeResourceMutationHooks();return false;}resourceMutationSlotsOwned=3;
 if(!replacePointer(engineBase+ShutdownGotOffset,shutdown,reinterpret_cast<std::uintptr_t>(observedResourceSchedulerShutdown))){removeResourceMutationHooks();return false;}resourceMutationSlotsOwned=7;
 resourceMutationHooksReady.store(true,std::memory_order_release);return true;
}
#endif
