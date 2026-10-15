#pragma once
#include "IndexedCommandState.h"
#include "NativeCameraRuntime.h"
#include <memory>

namespace rdr95 {
// Synchronous composition of already-captured owned indexed packets and the
// selected MAIN camera plan. This does not admit a retail object for CPU bypass.
// Caller retains packet storage, resources and both owners for the entire call.
class OwnedIndexedReplay final {
public:
    using Mirror=rdrvr::engine::IndexedCommandState;
    using Result=Mirror::Emission;
    OwnedIndexedReplay(Mirror&,NativeCameraRuntime&)noexcept;
    ~OwnedIndexedReplay();
    OwnedIndexedReplay(const OwnedIndexedReplay&)=delete;
    OwnedIndexedReplay& operator=(const OwnedIndexedReplay&)=delete;
    struct Guard {
        void* context{};
        // Synchronous non-owning external admission, checked inside the
        // Runtime plan before/after every raw PFN. Must remain lightweight.
        bool(*current)(void*)noexcept{};
    };
    Result emit(std::span<const Mirror::OwnedPacket>)noexcept;
    Result emit(std::span<const Mirror::OwnedPacket>,Guard)noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
