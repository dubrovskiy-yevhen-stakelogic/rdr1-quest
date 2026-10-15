#pragma once
#include <cstdint>
#include <cstddef>
#include <array>
namespace rdr95::ordinary_call::pins {
struct Pin {std::uintptr_t offset;const std::uint32_t* words;std::size_t count;};
inline constexpr std::uint32_t DrawCommon[]={
#include "gamecode/OrdinaryDrawPins_2a28238_3168_32hu.inc"
};
inline constexpr std::uint32_t Drawable[]={
#include "gamecode/OrdinaryDrawPins_1d9ac1c_36_32hu.inc"
};
inline constexpr std::uint32_t DrawSingle[]={
#include "gamecode/OrdinaryDrawPins_1d9d594_196_32hu.inc"
};
inline constexpr std::array<Pin,3> all{{
{0x2a28238,DrawCommon,792},
{0x1d9ac1c,Drawable,9},
{0x1d9d594,DrawSingle,49},
}};
inline constexpr std::uintptr_t site=0x2a28d4c,target=0x1d9ac1c,caller=site+4;
inline constexpr std::uint32_t expected=0xd63f0100u;
static_assert(DrawCommon[(site-0x2a28238)/4]==expected);
}
