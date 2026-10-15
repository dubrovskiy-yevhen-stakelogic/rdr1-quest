#pragma once
#include "NativeResourceGate.h"
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace rdr95 {
// Current native function arguments own the scheduler through this call.
// Helpers NEVER issue a read lease or declare arbitrary scene/pose immutable.
using QueueUpdateOriginal=bool(*)(void*);
using StreamingCompleteOriginal=void(*)(void*,void*);
using SchedulerShutdownOriginal=void(*)();
using NativeCritical=void(*)(void*);
struct ResourceQueueApi {
 NativeCritical lock{},unlock{};
 const void* burstRequest{}; // exact pinned BSS 88B; count+50
 const void* const* singleton{}; // exact pinned sm_Instance
 bool proof{};
};
struct ResourceOriginalErrno {int value{errno};~ResourceOriginalErrno(){errno=value;}};
template<class T>T resourceScalar(const void* p,std::size_t offset)noexcept {T v{};std::memcpy(&v,static_cast<const unsigned char*>(p)+offset,sizeof v);return v;}
// MUST be invoked only while this scheduler's native recursive Crit is held.
// No pointer chasing/allocation; conservative extra deferred count check.
inline bool resourceQueueQuietLocked(const ResourceQueueApi& api,void* scheduler)noexcept {
 if(!api.proof||!api.singleton||!api.burstRequest||!scheduler)return false;
 const void* singleton{};std::memcpy(&singleton,api.singleton,sizeof singleton);
 return singleton==scheduler&&resourceScalar<std::int32_t>(scheduler,0xc2c0)==0&&
  resourceScalar<std::int32_t>(scheduler,0xc358)==0&&resourceScalar<std::uint16_t>(scheduler,0xc218)==0&&
  resourceScalar<std::uint16_t>(scheduler,0xc228)==0&&resourceScalar<std::int32_t>(api.burstRequest,0x50)==0;
}
inline bool invokeResourceQueueUpdate(NativeResourceGate& gate,const ResourceQueueApi& api,QueueUpdateOriginal original,void* scheduler) {
 ResourceOriginalErrno resultError;
 NativeResourceGate::Writer writer(gate,NativeResourceGate::Writer::Mode::Quiet);
 struct ExtraLock {NativeCritical unlock{};void* object{};~ExtraLock(){release();}void release(){if(object){auto*p=object;object=nullptr;unlock(p);}}} extra;
 if(api.proof&&api.lock&&api.unlock&&scheduler&&reinterpret_cast<std::uintptr_t>(scheduler)<=UINTPTR_MAX-0xc3c8){
  auto* critical=static_cast<unsigned char*>(scheduler)+0xc3c8;
  api.lock(critical);extra.unlock=api.unlock;extra.object=critical;
  if(!resourceQueueQuietLocked(api,scheduler)){writer.markMutation();extra.release();}
 }else writer.markMutation();
 // Quiet: extra native Crit remains held through original, so no producer can
 // turn the inspected empty queues into callbacks. Nonquiet: mark BEFORE any
 // callbacks, drop extra recursion before original, preserve native lock order.
 errno=resultError.value;
 try{const bool result=original(scheduler);resultError.value=errno;return result;}
 catch(...){resultError.value=errno;throw;}
}
inline void invokeResourceStreamingComplete(NativeResourceGate& gate,StreamingCompleteOriginal original,void* scheduler,void* streamable) {
 ResourceOriginalErrno resultError;NativeResourceGate::Writer writer(gate);
 errno=resultError.value;
 try{original(scheduler,streamable);resultError.value=errno;}catch(...){resultError.value=errno;throw;}
}
inline void invokeResourceSchedulerShutdown(NativeResourceGate& gate,SchedulerShutdownOriginal original) {
 ResourceOriginalErrno resultError;gate.disarm();NativeResourceGate::Writer writer(gate);
 errno=resultError.value;
 try{original();resultError.value=errno;}catch(...){resultError.value=errno;throw;}
 // Intentionally stays disarmed until an explicit fresh native-init transaction.
}
}
