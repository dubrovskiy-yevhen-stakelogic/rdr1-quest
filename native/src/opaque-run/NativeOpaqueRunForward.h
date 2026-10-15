#pragma once
#include "NativeOpaqueRun.h"
#include <cerrno>
#include <cstdint>

namespace rdrvr::experimental {
using NativeOpaqueContainer=void(*)(void*,void*,const void*,std::uint32_t,std::uint32_t,std::uint32_t,bool,bool,std::uint32_t);
using NativeOpaqueCallback=void(*)(const void*,void*);
struct NativeOpaqueOriginalErrno{int value{errno};~NativeOpaqueOriginalErrno(){errno=value;}};
inline void nativeOpaqueForwardContainer(NativeOpaqueContainer original,NativeOpaqueRun& run,NativeOpaqueRun::Frame frame,
    void* self,void* request,const void* bits,std::uint32_t a,std::uint32_t b,std::uint32_t c,bool d,bool e,std::uint32_t f){
    NativeOpaqueOriginalErrno saved;
    NativeOpaqueRun::Invocation invocation(run,frame,reinterpret_cast<std::uintptr_t>(self));
    errno=saved.value;
    try{original(self,request,bits,a,b,c,d,e,f);saved.value=errno;}
    catch(...){saved.value=errno;throw;}
}
inline void nativeOpaqueForwardCallback(NativeOpaqueCallback original,NativeOpaqueRun& run,std::uintptr_t returnOffset,
    const void* callback,void* request){
    NativeOpaqueOriginalErrno saved;
    NativeOpaqueRun::Callback scope(run,returnOffset);
    errno=saved.value;
    try{original(callback,request);saved.value=errno;}
    catch(...){saved.value=errno;throw;}
}
}
