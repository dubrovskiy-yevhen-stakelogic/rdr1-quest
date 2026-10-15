// PRIVATE include inside TraceEntry anonymous namespace AFTER readMemory,
// mapping, replacePointer, engineBase and patchFault. At file scope include
// GameEffectMutation.h and NativeEffectMutationProof.h under HEAD_CAMERA.
#ifdef RDRVR_HEAD_CAMERA
EffectLoad originalEffectLoad{};EffectShutdown originalEffectShutdown{};
unsigned effectMutationSlotsOwned{};
struct EffectOriginalErrno {int value{errno};~EffectOriginalErrno(){errno=value;}};
__attribute__((noinline)) bool observedEffectLoad(void* effect,void* stream,int mode) {
 EffectOriginalErrno resultError;
 rdr95::NativeResourceGate::Writer resourceMutation(rdr95::nativeResourceGate);
 rdr95::EffectMutationEpoch::Scope mutation(effectMutationEpoch);
 errno=resultError.value;
 try {const bool result=originalEffectLoad(effect,stream,mode);resultError.value=errno;return result;}
 catch(...){resultError.value=errno;throw;}
}
__attribute__((noinline)) void observedEffectShutdown(void* effect) {
 EffectOriginalErrno resultError;
 rdr95::NativeResourceGate::Writer resourceMutation(rdr95::nativeResourceGate);
 rdr95::EffectMutationEpoch::Scope mutation(effectMutationEpoch);
 errno=resultError.value;
 try {originalEffectShutdown(effect);resultError.value=errno;}
 catch(...){resultError.value=errno;throw;}
}
[[maybe_unused]] bool removeEffectMutationHooks() {
 using namespace effect_mutation_generated;
 EffectOriginalErrno preserve;
 effectMutationCoverageDisarm();
 // Keep the resource reader exclusion until every owned effect slot is
 // restored; already-fetched wrappers retain their original/API lifetime.
 rdr95::NativeResourceGate::Writer drain(rdr95::nativeResourceGate,rdr95::NativeResourceGate::Writer::Mode::Quiet);
 bool ok=true;
 if(effectMutationSlotsOwned&2){
  if(replacePointer(engineBase+ShutdownGotOffset,reinterpret_cast<std::uintptr_t>(observedEffectShutdown),reinterpret_cast<std::uintptr_t>(originalEffectShutdown)))effectMutationSlotsOwned&=~2u;
  else ok=false;
 }
 if(effectMutationSlotsOwned&1){
  if(replacePointer(engineBase+LoadGotOffset,reinterpret_cast<std::uintptr_t>(observedEffectLoad),reinterpret_cast<std::uintptr_t>(originalEffectLoad)))effectMutationSlotsOwned&=~1u;
  else ok=false;
 }
 if(!ok)patchFault.store(true,std::memory_order_release);
 // Original pointers deliberately remain alive for already-fetched wrappers.
 return ok;
}
[[maybe_unused]] bool rearmEffectMutationHooks() {
 using namespace effect_mutation_generated;
 EffectOriginalErrno preserve;
 effectMutationCoverageDisarm();
 if(effectMutationSlotsOwned!=3||!engineBase||engineBase>UINTPTR_MAX-LoadGotOffset-sizeof(std::uintptr_t)||
    reinterpret_cast<std::uintptr_t>(originalEffectLoad)!=engineBase+LoadOffset||
    reinterpret_cast<std::uintptr_t>(originalEffectShutdown)!=engineBase+ShutdownOffset)return false;
 std::uintptr_t load{},shutdown{};
 if(!readMemory(nullptr,engineBase+LoadGotOffset,&load,sizeof load)||load!=reinterpret_cast<std::uintptr_t>(observedEffectLoad)||
    !readMemory(nullptr,engineBase+ShutdownGotOffset,&shutdown,sizeof shutdown)||shutdown!=reinterpret_cast<std::uintptr_t>(observedEffectShutdown))return false;
 // Preserve counted live writers: do not remove/reinstall either hook. The
 // epoch remains odd until the last current mutation leaves its Scope.
 effectMutationEpoch.setCoverage(true);effectMutationHookReady.store(true,std::memory_order_release);return true;
}
[[maybe_unused]] bool installEffectMutationHooks() {
 using namespace effect_mutation_generated;
 EffectOriginalErrno preserve;
 effectMutationCoverageDisarm();
 if(effectMutationSlotsOwned||!engineBase||engineBase>UINTPTR_MAX-LoadGotOffset-sizeof(std::uintptr_t))return false;
 if(!effectMutationCodeMatches(engineBase,[](std::uintptr_t address,void* bytes,std::size_t count){
  Mapping region{};return mapping(address,count,region)&&(region.protection&PROT_EXEC)&&readMemory(nullptr,address,bytes,count);
 }))return false;
 std::uintptr_t load{},shutdown{};
 if(!readMemory(nullptr,engineBase+LoadGotOffset,&load,sizeof load)||load!=engineBase+LoadOffset||
    !readMemory(nullptr,engineBase+ShutdownGotOffset,&shutdown,sizeof shutdown)||shutdown!=engineBase+ShutdownOffset)return false;
 // Exact ownership: never chain or replace someone else's hooks.
 originalEffectLoad=reinterpret_cast<EffectLoad>(load);originalEffectShutdown=reinterpret_cast<EffectShutdown>(shutdown);
 if(!replacePointer(engineBase+LoadGotOffset,load,reinterpret_cast<std::uintptr_t>(observedEffectLoad)))return false;
 effectMutationSlotsOwned=1;
 if(!replacePointer(engineBase+ShutdownGotOffset,shutdown,reinterpret_cast<std::uintptr_t>(observedEffectShutdown))){removeEffectMutationHooks();return false;}
 effectMutationSlotsOwned=3;
 effectMutationEpoch.setCoverage(true);effectMutationHookReady.store(true,std::memory_order_release);return true;
}
#endif
