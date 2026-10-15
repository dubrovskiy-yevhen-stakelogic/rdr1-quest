// PRIVATE actual Trace include, inside its anonymous namespace. At FILE scope
// include NativeOpaqueRunHookState.h, NativeOpaqueRunForward.h and
// NativeEarlyTraceRoute.h. ROOT owns the only TLS run/frame route. No automatic
// hook setup/install is performed. ROOT supplies setup Api after all three
// route dependencies are proven. Keep Api/context/image alive after removal.
#ifdef RDRVR_HEAD_CAMERA
rdrvr::experimental::NativeOpaqueRunHookState opaqueRunHookState(rdrvr::experimental::nativeOpaqueRunCoverage);
__attribute__((noinline)) void observedOpaqueContainer(void* self,void* request,const void* bits,
    std::uint32_t a,std::uint32_t b,std::uint32_t c,bool d,bool e,std::uint32_t f){
    const int incoming=errno;
    const auto pc=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const auto base=opaqueRunHookState.image();const auto relative=pc>=base?pc-base:0;
    auto& run=rdrvr::experimental::NativeEarlyTraceRoute::opaqueRun();
    auto frame=rdrvr::experimental::NativeEarlyTraceRoute::opaqueFrame();
    if(!opaqueRunHookState.ready()||!opaqueRunHookState.caller(relative))frame={};
    const auto original=reinterpret_cast<rdrvr::experimental::NativeOpaqueContainer>(opaqueRunHookState.original(0));
    errno=incoming;
    rdrvr::experimental::nativeOpaqueForwardContainer(original,run,frame,self,request,bits,a,b,c,d,e,f);
}
__attribute__((noinline)) void observedOpaqueBucketCallback(const void* callback,void* request){
    const int incoming=errno;
    const auto pc=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const auto base=opaqueRunHookState.image();const auto relative=pc>=base?pc-base:0;
    auto& run=rdrvr::experimental::NativeEarlyTraceRoute::opaqueRun();
    const auto original=reinterpret_cast<rdrvr::experimental::NativeOpaqueCallback>(opaqueRunHookState.original(1));
    errno=incoming;
    rdrvr::experimental::nativeOpaqueForwardCallback(original,run,relative,callback,request);
}
[[maybe_unused]] bool setupNativeOpaqueRunHooks(std::uintptr_t image,rdrvr::experimental::NativeOpaqueRunHookState::Api api)noexcept{
    return opaqueRunHookState.setup(image,api,{reinterpret_cast<std::uintptr_t>(observedOpaqueContainer),reinterpret_cast<std::uintptr_t>(observedOpaqueBucketCallback)});
}
[[maybe_unused]] bool installNativeOpaqueRunHooks()noexcept{return opaqueRunHookState.install();}
[[maybe_unused]] bool removeNativeOpaqueRunHooks()noexcept{return opaqueRunHookState.remove();}
[[maybe_unused]] bool rearmNativeOpaqueRunHooks()noexcept{return opaqueRunHookState.rearm();}
[[maybe_unused]] void disarmNativeOpaqueRunHooks()noexcept{opaqueRunHookState.disarm();}
[[maybe_unused]] bool nativeOpaqueRunHooksReady()noexcept{return opaqueRunHookState.ready();}
[[maybe_unused]] std::uint64_t nativeOpaqueRunHooksGeneration()noexcept{return opaqueRunHookState.generation();}
#endif
