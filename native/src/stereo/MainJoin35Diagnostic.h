#pragma once
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <ostream>
namespace rdrvr::experimental::main_join35 {
// Diagnostic only. Thread-local scalar receipts neither read native memory nor
// issue permissions. Publish on the existing scene boundary, never per draw.
enum Event : unsigned {
    HookCalls, HookQualified, HookFallthrough, RouteSelected, RouteAbsent,
    BoundaryCalls, NoPrimaryScope, WrongBoundary, Repeated, NativeException,
    Faulted, ReadPairMismatch, ReadDeclined, FrameBeforeProviderDeclined,
    ProviderBeginDeclined, JoinedAdapterDeclined, ProviderAfterDeclined,
    FrameAfterJoinDeclined, Joined, Count
};
inline constexpr const char* names[] = {
    "hookCalls", "hookQualified", "hookFallthrough", "routeSelected", "routeAbsent",
    "boundaryCalls", "noPrimaryScope", "wrongBoundary", "repeated", "nativeException",
    "faulted", "readPairMismatch", "readDeclined", "frameBeforeProviderDeclined",
    "providerBeginDeclined", "joinedAdapterDeclined", "providerAfterDeclined",
    "frameAfterJoinDeclined", "joined"
};
struct Caller { std::uintptr_t offset{}; unsigned request{}; std::uint64_t calls{}; };
struct Receipt {
    std::array<std::uint64_t, Count> events{};
    std::array<Caller, 8> callers{};
    std::uint64_t callerOverflow{};
    unsigned providerError{};
};
inline thread_local Receipt local{};
inline void increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}
inline void event(Event value) noexcept {
    int* cell = &errno; const int saved = *cell;
    increment(local.events[value]); *cell = saved;
}
inline void caller(std::uintptr_t offset, unsigned request) noexcept {
    int* cell = &errno; const int saved = *cell;
    struct Restore { int* cell; int saved; ~Restore() { *cell = saved; } } restore{cell, saved};
    for (auto& row : local.callers) {
        if (!row.calls) { row.offset = offset; row.request = request; row.calls = 1; return; }
        if (row.offset == offset && row.request == request) { increment(row.calls); return; }
    }
    increment(local.callerOverflow);
}
struct Published {
    std::atomic_flag publishing=ATOMIC_FLAG_INIT;
    std::atomic<std::uint64_t> sequence{}, frame{}, callerOverflow{}, providerError{};
    std::array<std::atomic<std::uint64_t>, Count> events{};
    std::array<std::array<std::atomic<std::uint64_t>, 3>, 8> callers{};
};
inline Published published{};
// The existing scene owner is the sole publisher. Readers use only atomics;
// unstable snapshots are marked, never reported as an accepted scene receipt.
inline void publish(std::uint64_t frame) noexcept {
    int* cell = &errno; const int saved = *cell;
    if (published.publishing.test_and_set(std::memory_order_acquire)) { *cell=saved; return; }
    struct Release { ~Release(){published.publishing.clear(std::memory_order_release);} } release;
    const auto seq = published.sequence.load(std::memory_order_seq_cst);
    if (seq > std::numeric_limits<std::uint64_t>::max() - 2) { *cell = saved; return; }
    published.sequence.store(seq + 1, std::memory_order_seq_cst);
    published.frame.store(frame, std::memory_order_seq_cst);
    for (unsigned i=0; i<Count; ++i) published.events[i].store(local.events[i], std::memory_order_seq_cst);
    for (unsigned i=0; i<8; ++i) {
        published.callers[i][0].store(local.callers[i].offset, std::memory_order_seq_cst);
        published.callers[i][1].store(local.callers[i].request, std::memory_order_seq_cst);
        published.callers[i][2].store(local.callers[i].calls, std::memory_order_seq_cst);
    }
    published.callerOverflow.store(local.callerOverflow, std::memory_order_seq_cst);
    published.providerError.store(local.providerError, std::memory_order_seq_cst);
    published.sequence.store(seq + 2, std::memory_order_seq_cst); *cell = saved;
}
inline void write(std::ostream& out) {
    Receipt snapshot;
    const auto before=published.sequence.load(std::memory_order_seq_cst);
    const auto frame=published.frame.load(std::memory_order_seq_cst);
    for (unsigned i=0; i<Count; ++i) snapshot.events[i]=published.events[i].load(std::memory_order_seq_cst);
    for (unsigned i=0; i<8; ++i) {
        snapshot.callers[i].offset=published.callers[i][0].load(std::memory_order_seq_cst);
        snapshot.callers[i].request=static_cast<unsigned>(published.callers[i][1].load(std::memory_order_seq_cst));
        snapshot.callers[i].calls=published.callers[i][2].load(std::memory_order_seq_cst);
    }
    snapshot.callerOverflow=published.callerOverflow.load(std::memory_order_seq_cst);
    snapshot.providerError=static_cast<unsigned>(published.providerError.load(std::memory_order_seq_cst));
    const auto after=published.sequence.load(std::memory_order_seq_cst);
    out<<",\"mainJoin35\":{\"stable\":"<<((before && !(before&1) && before==after)?"true":"false")
       <<",\"scene\":"<<frame<<",\"sequence\":"<<after<<",\"cumulativeOnPublishedThread\":true,\"events\":{";
    for (unsigned i=0; i<Count; ++i) {if(i)out<<',';out<<'"'<<names[i]<<"\":"<<snapshot.events[i];}
    out<<"},\"providerError\":"<<snapshot.providerError<<",\"callerOverflow\":"<<snapshot.callerOverflow<<",\"callers\":[";
    for (unsigned i=0; i<8; ++i) {if(i)out<<',';const auto& row=snapshot.callers[i];out<<"{\"offset\":"<<row.offset<<",\"request\":"<<row.request<<",\"calls\":"<<row.calls<<'}';}
    out<<"]}";
}
}
