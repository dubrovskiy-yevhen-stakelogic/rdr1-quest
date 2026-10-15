#pragma once
#include "EffectMutationEpoch.h"
#include "NativeResourceGate.h"
#include <atomic>
#include <cerrno>
#include <cstdint>
namespace rdrvr::engine {
using EffectLoad=bool(*)(void*,void*,int);
using EffectShutdown=void(*)(void*);
inline rdr95::EffectMutationEpoch effectMutationEpoch;
inline std::atomic<bool> effectMutationHookReady{};
// Revocation only. Native memory ownership and a current material walk remain
// independent requirements. Call BEFORE any native renderer reinitialization.
inline void effectMutationCoverageDisarm()noexcept {
 const int error=errno;rdr95::nativeResourceGate.disarm();effectMutationHookReady.store(false,std::memory_order_release);
 effectMutationEpoch.setCoverage(false);errno=error;
}
inline std::uint64_t effectMutationTicket()noexcept {
 if(!effectMutationHookReady.load(std::memory_order_acquire))return 0;
 const auto ticket=effectMutationEpoch.ticket();
 return effectMutationHookReady.load(std::memory_order_acquire)?ticket:0;
}
inline bool effectMutationTicketCurrent(std::uint64_t ticket)noexcept {
 return effectMutationHookReady.load(std::memory_order_acquire)&&effectMutationEpoch.current(ticket)&&effectMutationHookReady.load(std::memory_order_acquire);
}
}
