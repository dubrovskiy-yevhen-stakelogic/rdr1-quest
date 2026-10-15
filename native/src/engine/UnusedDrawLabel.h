#pragma once
#include <cstdint>

namespace rdrvr::engine {
// The pinned grmShader::BeginDraw formats a 128-byte PIX label. Its pinned
// PIXBegin consumer never reads the label on any path. Keep PIXBegin/PIXEnd
// and all rendering state; omit only this exact caller's dead formatting.
// The AArch64 entry tail-forwards every other caller, including unknown
// variadic argument layouts, without unpacking or rebuilding a va_list.
class UnusedDrawLabel {
public:
    // Configure once before publishing the GOT hook. Conflicting addresses
    // cannot replace the original while native calls may be in flight.
    bool configure(std::uintptr_t original,std::uintptr_t exactReturn) noexcept;
    bool ready()const noexcept;
    std::uintptr_t entry()const noexcept;
    std::uint64_t skipped()const noexcept;
};
inline UnusedDrawLabel unusedDrawLabel;
}
