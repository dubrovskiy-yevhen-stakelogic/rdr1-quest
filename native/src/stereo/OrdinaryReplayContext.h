#pragma once
#include <cstdint>

namespace rdrvr::experimental {class NativeEarlyRuntimeBinding;}
namespace rdrvr::engine {class EyeViewports;}
namespace rdr95 {
// Actual selected Game pass only. This exposes identities, never a native
// read lease, admission or a lifetime extension. The short Read is separate.
struct OrdinaryReplayContext {
    rdrvr::experimental::NativeEarlyRuntimeBinding* binding{};
    rdrvr::engine::EyeViewports* views{};
    std::uintptr_t image{},gfx{},tlsSlot{};
    std::uint64_t scene{},pose{};
};
}
namespace rdrvr::engine {
bool gameOrdinaryReplayContext(rdr95::OrdinaryReplayContext&)noexcept;
}
