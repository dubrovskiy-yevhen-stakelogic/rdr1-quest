// PRIVATE include fragment: include AFTER readMemory, mapping, replacePointer,
// engineBase and patchFault definitions, inside TraceEntry's anonymous namespace.
// Include GameMainVisibilityWait.h and NativeMainVisibilityWaitProof.h at file
// scope under RDRVR_HEAD_CAMERA. Install explicitly only after other MAIN owner
// proofs are configured. No automatic installation occurs in this fragment.
#ifdef RDRVR_HEAD_CAMERA
MainVisibilityWait originalMainVisibilityWait{};
bool mainVisibilityWaitSlotOwned{};
__attribute__((noinline)) void observedMainVisibilityWait(void* manager,std::uint32_t request) {
    const int incoming=errno;
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const auto relative=caller>=engineBase?caller-engineBase:std::uintptr_t{};
    rdrvr::experimental::main_join35::event(rdrvr::experimental::main_join35::HookCalls);
    rdrvr::experimental::main_join35::caller(relative,request);
    const auto original=originalMainVisibilityWait; // published BEFORE slot CAS
    errno=incoming;
    if(mainVisibilityWaitHookReady.load(std::memory_order_acquire)&&
       request==main_wait_generated::MainRequest&&relative==main_wait_generated::MainReturnOffset) {
        rdrvr::experimental::main_join35::event(rdrvr::experimental::main_join35::HookQualified);
        gameMainVisibilityWait(original,manager,request,relative);
    } else {rdrvr::experimental::main_join35::event(rdrvr::experimental::main_join35::HookFallthrough);original(manager,request);}
}
[[maybe_unused]] bool installMainVisibilityWaitHook() {
    using namespace main_wait_generated;
    const int incoming=errno;struct RestoreErrno{int value;~RestoreErrno(){errno=value;}} restore{incoming};
    mainVisibilityWaitHookReady.store(false,std::memory_order_release);
    if(!engineBase||engineBase>UINTPTR_MAX-MainGotOffset-sizeof(std::uintptr_t))return false;
    if(!mainVisibilityWaitCodeMatches(engineBase,[](std::uintptr_t address,void* bytes,std::size_t count){
        Mapping region{};return mapping(address,count,region)&&(region.protection&PROT_EXEC)&&readMemory(nullptr,address,bytes,count);
    }))return false;
    const auto expected=engineBase+MainWaitOffset;
    std::uintptr_t current{};
    if(!readMemory(nullptr,engineBase+MainGotOffset,&current,sizeof current)||current!=expected)return false;
    // Never chain over a foreign hook or replace another owner of this shared
    // wait slot. Non-MAIN/shadow/reflection uses remain original fallthrough.
    originalMainVisibilityWait=reinterpret_cast<MainVisibilityWait>(expected);
    if(!replacePointer(engineBase+MainGotOffset,expected,reinterpret_cast<std::uintptr_t>(observedMainVisibilityWait)))return false;
    mainVisibilityWaitSlotOwned=true;mainVisibilityWaitHookReady.store(true,std::memory_order_release);return true;
}
[[maybe_unused]] bool removeMainVisibilityWaitHook() {
    using namespace main_wait_generated;
    const int incoming=errno;struct RestoreErrno{int value;~RestoreErrno(){errno=value;}} restore{incoming};
    mainVisibilityWaitHookReady.store(false,std::memory_order_release);
    if(!mainVisibilityWaitSlotOwned)return true;
    const bool restored=replacePointer(engineBase+MainGotOffset,reinterpret_cast<std::uintptr_t>(observedMainVisibilityWait),reinterpret_cast<std::uintptr_t>(originalMainVisibilityWait));
    if(!restored)patchFault.store(true,std::memory_order_release);else mainVisibilityWaitSlotOwned=false;
    // Keep original stable even after rollback: an already loaded wrapper
    // pointer must still forward safely while frame-owner teardown completes.
    return restored;
}
#endif
