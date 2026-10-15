#pragma once
#include "NativeStaticOcclusionSource.h"
#include "NativeMatrixSnapshot.h"

namespace rdrvr::engine {
// Reads ONLY a synchronous, transitive native-owned borrow at the successful
// emitted native draw boundary. Unlike the general source reader
// this must never receive arbitrary asset addresses. No retained native pointer,
// guarded syscall, allocation, lock, native call or GPU synchronization.
class NativeStaticMatrixWitness {
public:
    using Matrix=NativeStaticOcclusionSource::FloatMatrix;
    struct Api {std::uintptr_t contextVtable{},bufferVtable{};bool completeCodeProof{};};
    struct ActualBinding {
        std::uintptr_t command{},descriptor{};
        std::uint64_t buffer{};
        std::uint64_t offset{};
        std::uint32_t range{};
        // Actual current GRAPHICS set0/binding1, resolved by the converted
        // descriptor ledger with exact slot/offset ordering and current command
        // generation. Never infer offset index from native constant slot 1.
        bool valid{};
    };
    using Snapshot=NativeMatrixSnapshot;
    // Context and its native frame/CB/ring children are a transitive synchronous
    // owner lease supplied only by the pinned emitted primitive call.
    // On failure valid=false and identity fields are zero; matrix tails are
    // unspecified. Success writes the owned matrix bytes exactly once.
    static bool capture(const Api&,void* context,const ActualBinding&,Snapshot&) noexcept;
    static bool relativeMatches(const Snapshot&,const NativeStaticOcclusionSource::Object&,
        const NativeStaticOcclusionSource::Camera&) noexcept;
};
}
