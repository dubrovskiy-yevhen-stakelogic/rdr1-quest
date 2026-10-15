#pragma once
#include <cstdint>
#include <cstddef>
namespace rdr2::worker28::retail_allocator::detail {
inline constexpr std::uint8_t FileDigest[]{0x3f,0x80,0x84,0x5e,0xde,0xc3,0x2a,0x66,0x3c,0x42,0xa8,0x57,0x0e,0x31,0x31,0x44,0x68,0x13,0xe6,0x95,0x07,0x6c,0x4b,0xc9,0x6e,0x21,0x42,0xc1,0x90,0xbc,0x22,0xf3};
inline constexpr std::uint64_t FileSize=1323108640ULL;
inline constexpr std::uintptr_t PhdrOffset=64U;
inline constexpr std::uint8_t ElfHeader[]{
#include "gamecode/RetailAllocatorPins_0_64_8h.inc"
};
inline constexpr std::uint8_t PhdrBytes[]{
#include "gamecode/RetailAllocatorPins_40_840_8h.inc"
};
inline constexpr std::uint8_t NoteBytes[]{
#include "gamecode/RetailAllocatorPins_4e908000_184_8h.inc"
};
inline constexpr std::uintptr_t NoteRva=1318092800U;
struct CodePin {std::uintptr_t rva;std::size_t size;const std::uint8_t* bytes;};
inline constexpr std::uint8_t Code0[]{
#include "gamecode/RetailAllocatorPins_18aac30_28_8h.inc"
};
inline constexpr std::uint8_t Code1[]{0xe0,0x03,0x01,0xaa,0xe1,0x03,0x02,0xaa,0x8f,0xbb,0x4d,0x14};
inline constexpr std::uint8_t Code2[]{
#include "gamecode/RetailAllocatorPins_18aac58_16_8h.inc"
};
inline constexpr std::uint8_t Code3[]{0xe0,0x03,0x01,0xaa,0x67,0xd1,0x4e,0x14};
inline constexpr std::uint8_t Code4[]{0xf9,0xe6,0x4e,0x14};
inline constexpr std::uint8_t Code5[]{0x2a,0xd1,0x4e,0x14};
inline constexpr std::uint8_t Code6[]{
#include "gamecode/RetailAllocatorPins_2c19aa0_16_8h.inc"
};
inline constexpr std::uint8_t Code7[]{
#include "gamecode/RetailAllocatorPins_2c19a90_16_8h.inc"
};
inline constexpr std::uint8_t Code8[]{
#include "gamecode/RetailAllocatorPins_2c19630_16_8h.inc"
};
inline constexpr std::uint8_t Code9[]{
#include "gamecode/RetailAllocatorPins_2c14430_16_8h.inc"
};
inline constexpr std::uint8_t Code10[]{
#include "gamecode/RetailAllocatorPins_2c19a70_16_8h.inc"
};
inline constexpr std::uint8_t Code11[]{
#include "gamecode/RetailAllocatorPins_2c14340_16_8h.inc"
};
inline constexpr CodePin CodePins[]{
{0x18aac30U,sizeof(Code0),Code0},
{0x18aac4cU,sizeof(Code1),Code1},
{0x18aac58U,sizeof(Code2),Code2},
{0x185fe90U,sizeof(Code3),Code3},
{0x185fe8cU,sizeof(Code4),Code4},
{0x185fe98U,sizeof(Code5),Code5},
{0x2c19aa0U,sizeof(Code6),Code6},
{0x2c19a90U,sizeof(Code7),Code7},
{0x2c19630U,sizeof(Code8),Code8},
{0x2c14430U,sizeof(Code9),Code9},
{0x2c19a70U,sizeof(Code10),Code10},
{0x2c14340U,sizeof(Code11),Code11},
};
}
