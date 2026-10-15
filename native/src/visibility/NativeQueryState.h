#pragma once
#include "NativeQueryGuard.h"
#include <type_traits>
namespace rdrvr::engine {
inline experimental::NativeQueryGuard nativeQueryGuard;
inline std::atomic<bool> nativeQueryHooksReady{};
inline std::atomic<std::uint64_t> nativeQueryCoverageEpoch{};
struct NativeQueryTicket {experimental::NativeQueryGuard::Ticket command{};std::uint64_t coverage{};};
template<class T>std::uint64_t queryHandle(T h)noexcept {
    if constexpr(std::is_pointer_v<T>)return reinterpret_cast<std::uintptr_t>(h);
    else return std::uint64_t(h);
}
inline void nativeQueryCoverageDisarm()noexcept {
    nativeQueryHooksReady.store(false,std::memory_order_release);
    nativeQueryCoverageEpoch.fetch_add(1,std::memory_order_acq_rel);
    nativeQueryGuard.invalidateAllRecordings();
}
// Initialization/teardown are serialized by the existing native init owner.
inline void nativeQueryCoverageArm()noexcept {
    nativeQueryCoverageDisarm();
    if(nativeQueryCoverageEpoch.load(std::memory_order_acquire)!=0)
        nativeQueryHooksReady.store(true,std::memory_order_release);
}
inline void nativeQueryRecordingBegun(std::uint64_t command,bool success,bool plainPrimary)noexcept {
    const auto epoch=nativeQueryCoverageEpoch.load(std::memory_order_acquire);
    const bool covered=epoch&&nativeQueryHooksReady.load(std::memory_order_acquire);
    nativeQueryGuard.begun(command,success,plainPrimary&&covered);
    if(!covered||nativeQueryCoverageEpoch.load(std::memory_order_acquire)!=epoch||!nativeQueryHooksReady.load(std::memory_order_acquire))
        nativeQueryGuard.commandUnknown(command);
}
inline bool nativeQueriesCurrent(const NativeQueryTicket& ticket)noexcept {
    return ticket.coverage&&nativeQueryHooksReady.load(std::memory_order_acquire)&&
        nativeQueryCoverageEpoch.load(std::memory_order_acquire)==ticket.coverage&&nativeQueryGuard.current(ticket.command)&&
        nativeQueryCoverageEpoch.load(std::memory_order_acquire)==ticket.coverage&&nativeQueryHooksReady.load(std::memory_order_acquire);
}
inline bool nativeQueriesQuiet(std::uint64_t device,std::uint64_t command,NativeQueryTicket& out)noexcept {
    out={};NativeQueryTicket value;value.coverage=nativeQueryCoverageEpoch.load(std::memory_order_acquire);
    if(!value.coverage||!nativeQueryHooksReady.load(std::memory_order_acquire)||!nativeQueryGuard.quiet(device,command,value.command)||!nativeQueriesCurrent(value))return false;
    out=value;return true;
}
}
