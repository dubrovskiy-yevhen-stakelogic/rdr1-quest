#pragma once
#include <array>
#include <cstdint>

namespace rdrvr::engine {
// Value copy produced by the exact successful native draw binding witness.
// This is not a retained native lease. Consumers require the same synchronous
// draw stack and matching context/command before reusing the copied matrices.
struct NativeMatrixSnapshot {
    using Matrix=std::array<float,16>;
    std::uintptr_t command{},descriptor{},frame{};
    std::uint64_t buffer{},offset{};
    std::uint32_t range{};
    std::array<Matrix,4> matrices{};
    bool valid{};
    std::uintptr_t context{};
};
}
