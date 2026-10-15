#pragma once
#include <cstdint>
namespace rdr95 {
// Same predicate used by the live Body scope and the recorded-input PC replay.
// This is only the scalar gate; it grants no pointer lifetime, draw or skip permission.
inline bool ordinaryBodyScalars(bool creature,bool matrices,std::uint32_t flags) noexcept {
    return !creature&&!matrices&&!(flags&0xe000u);
}
inline unsigned ordinaryBodyClass(bool creature,bool matrices,std::uint32_t flags) noexcept {
    return ((flags>>13)&7u)*4u+(matrices?2u:0u)+(creature?1u:0u);
}
}
