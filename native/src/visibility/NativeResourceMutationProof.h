#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
namespace rdr95::resource_generated {
inline constexpr std::uintptr_t UpdateOffset=0x1a2be04,UpdateGotOffset=0x2e1cb20;
inline constexpr std::uintptr_t CompleteOffset=0x1a2d340,CompleteGotOffset=0x2e1cb48;
inline constexpr std::uintptr_t ShutdownOffset=0x1a2b258,ShutdownGotOffset=0x2e12e30;
inline constexpr std::uintptr_t LockOffset=0x187f844,UnlockOffset=0x187f8b4,SingletonOffset=0x4e6afca0,BurstOffset=0x4e6afca8,SingletonGotOffset=0x2ddeef8,BurstGotOffset=0x2ddeee8;
inline constexpr std::array<unsigned char,3768> bytes0{
#include "gamecode/NativeResourceMutationProof_1a2be04_3768_8h.inc"
};
inline constexpr std::array<unsigned char,476> bytes1{
#include "gamecode/NativeResourceMutationProof_1a2d340_476_8h.inc"
};
inline constexpr std::array<unsigned char,60> bytes2{
#include "gamecode/NativeResourceMutationProof_1a2b258_60_8h.inc"
};
inline constexpr std::array<unsigned char,112> bytes3{
#include "gamecode/NativeResourceMutationProof_187f844_112_8h.inc"
};
inline constexpr std::array<unsigned char,4> bytes4{0xa7,0x52,0x4e,0x14};
inline constexpr std::array<unsigned char,108> bytes5{
#include "gamecode/NativeResourceMutationProof_187f8bc_108_8h.inc"
};
inline constexpr std::array<unsigned char,300> bytes6{
#include "gamecode/NativeResourceMutationProof_1a2b0a8_300_8h.inc"
};
inline constexpr std::array<unsigned char,52> bytes7{
#include "gamecode/NativeResourceMutationProof_1a2b1d4_52_8h.inc"
};
inline constexpr std::array<unsigned char,128> bytes8{
#include "gamecode/NativeResourceMutationProof_1a2b294_128_8h.inc"
};
inline constexpr std::array<unsigned char,264> bytes9{
#include "gamecode/NativeResourceMutationProof_1a2dc40_264_8h.inc"
};
inline constexpr std::array<unsigned char,416> bytes10{
#include "gamecode/NativeResourceMutationProof_1a2dd48_416_8h.inc"
};
inline constexpr std::array<unsigned char,1156> bytes11{
#include "gamecode/NativeResourceMutationProof_1a2ce30_1156_8h.inc"
};
inline constexpr std::array<unsigned char,192> bytes12{
#include "gamecode/NativeResourceMutationProof_1a2afe4_192_8h.inc"
};
inline constexpr std::array<unsigned char,620> bytes13{
#include "gamecode/NativeResourceMutationProof_1a2baa0_620_8h.inc"
};
inline constexpr std::array<unsigned char,172> bytes14{
#include "gamecode/NativeResourceMutationProof_1a2cd84_172_8h.inc"
};
inline constexpr std::array<unsigned char,36> bytes15{
#include "gamecode/NativeResourceMutationProof_1a2fbc4_36_8h.inc"
};
inline constexpr std::array<unsigned char,468> bytes16{
#include "gamecode/NativeResourceMutationProof_1a2fcbc_468_8h.inc"
};
inline constexpr std::array<unsigned char,244> bytes17{
#include "gamecode/NativeResourceMutationProof_1a2d5f0_244_8h.inc"
};
inline constexpr std::array<unsigned char,524> bytes18{
#include "gamecode/NativeResourceMutationProof_1a2d6e4_524_8h.inc"
};
inline constexpr std::array<unsigned char,16> bytes19{
#include "gamecode/NativeResourceMutationProof_2c28980_16_8h.inc"
};
inline constexpr std::array<unsigned char,4> bytes20{0x69,0x51,0x29,0x94};
inline constexpr std::array<unsigned char,4> bytes21{0x95,0x1e,0x29,0x94};
inline constexpr std::array<unsigned char,4> bytes22{0x5b,0xb4,0x1c,0x94};
inline constexpr std::array<unsigned char,4> bytes23{0x4c,0xd4,0x0d,0x94};
inline constexpr std::array<unsigned char,4> bytes24{0x12,0xd4,0x0d,0x94};
inline constexpr std::array<unsigned char,4> bytes25{0x11,0xd0,0x0c,0x94};
inline constexpr std::array<unsigned char,4> bytes26{0x3a,0x64,0x0c,0x94};
inline constexpr std::array<unsigned char,4> bytes27{0x93,0x4c,0x07,0x94};
inline constexpr std::array<unsigned char,4> bytes28{0x33,0x15,0x06,0x14};
inline constexpr std::array<unsigned char,4> bytes29{0xe6,0xe6,0x05,0x94};
inline constexpr std::array<unsigned char,16> bytes30{
#include "gamecode/NativeResourceMutationProof_2c289d0_16_8h.inc"
};
inline constexpr std::array<unsigned char,4> bytes31{0x7c,0xe3,0x47,0x14};
inline constexpr std::array<unsigned char,4> bytes32{0xe5,0xe2,0x47,0x94};
inline constexpr std::array<unsigned char,16> bytes33{
#include "gamecode/NativeResourceMutationProof_2c14fa0_16_8h.inc"
};
inline constexpr std::array<unsigned char,4> bytes34{0x8d,0x52,0x4f,0x94};
template<class Read>bool resourceMutationCodeMatches(std::uintptr_t base,Read&&read){if(!base||base>UINTPTR_MAX-BurstOffset-88)return false;
{std::array<unsigned char,3768>copy{};if(!read(base+0x1a2be04,copy.data(),copy.size())||copy!=bytes0)return false;}
{std::array<unsigned char,476>copy{};if(!read(base+0x1a2d340,copy.data(),copy.size())||copy!=bytes1)return false;}
{std::array<unsigned char,60>copy{};if(!read(base+0x1a2b258,copy.data(),copy.size())||copy!=bytes2)return false;}
{std::array<unsigned char,112>copy{};if(!read(base+0x187f844,copy.data(),copy.size())||copy!=bytes3)return false;}
{std::array<unsigned char,4>copy{};if(!read(base+0x187f8b4,copy.data(),copy.size())||copy!=bytes4)return false;}
{std::array<unsigned char,108>copy{};if(!read(base+0x187f8bc,copy.data(),copy.size())||copy!=bytes5)return false;}
{std::array<unsigned char,300>copy{};if(!read(base+0x1a2b0a8,copy.data(),copy.size())||copy!=bytes6)return false;}
{std::array<unsigned char,52>copy{};if(!read(base+0x1a2b1d4,copy.data(),copy.size())||copy!=bytes7)return false;}
{std::array<unsigned char,128>copy{};if(!read(base+0x1a2b294,copy.data(),copy.size())||copy!=bytes8)return false;}
{std::array<unsigned char,264>copy{};if(!read(base+0x1a2dc40,copy.data(),copy.size())||copy!=bytes9)return false;}
{std::array<unsigned char,416>copy{};if(!read(base+0x1a2dd48,copy.data(),copy.size())||copy!=bytes10)return false;}
{std::array<unsigned char,1156>copy{};if(!read(base+0x1a2ce30,copy.data(),copy.size())||copy!=bytes11)return false;}
{std::array<unsigned char,192>copy{};if(!read(base+0x1a2afe4,copy.data(),copy.size())||copy!=bytes12)return false;}
{std::array<unsigned char,620>copy{};if(!read(base+0x1a2baa0,copy.data(),copy.size())||copy!=bytes13)return false;}
{std::array<unsigned char,172>copy{};if(!read(base+0x1a2cd84,copy.data(),copy.size())||copy!=bytes14)return false;}
{std::array<unsigned char,36>copy{};if(!read(base+0x1a2fbc4,copy.data(),copy.size())||copy!=bytes15)return false;}
{std::array<unsigned char,468>copy{};if(!read(base+0x1a2fcbc,copy.data(),copy.size())||copy!=bytes16)return false;}
{std::array<unsigned char,244>copy{};if(!read(base+0x1a2d5f0,copy.data(),copy.size())||copy!=bytes17)return false;}
{std::array<unsigned char,524>copy{};if(!read(base+0x1a2d6e4,copy.data(),copy.size())||copy!=bytes18)return false;}
{std::array<unsigned char,16>copy{};if(!read(base+0x2c28980,copy.data(),copy.size())||copy!=bytes19)return false;}
{std::array<unsigned char,4>copy{};if(!read(base+0x21d43dc,copy.data(),copy.size())||copy!=bytes20)return false;}
{std::array<unsigned char,4>copy{};if(!read(base+0x21e0f2c,copy.data(),copy.size())||copy!=bytes21)return false;}
{std::array<unsigned char,4>copy{};if(!read(base+0x24fb814,copy.data(),copy.size())||copy!=bytes22)return false;}
{std::array<unsigned char,4>copy{};if(!read(base+0x28b3850,copy.data(),copy.size())||copy!=bytes23)return false;}
{std::array<unsigned char,4>copy{};if(!read(base+0x28b3938,copy.data(),copy.size())||copy!=bytes24)return false;}
{std::array<unsigned char,4>copy{};if(!read(base+0x28f493c,copy.data(),copy.size())||copy!=bytes25)return false;}
{std::array<unsigned char,4>copy{};if(!read(base+0x290f898,copy.data(),copy.size())||copy!=bytes26)return false;}
{std::array<unsigned char,4>copy{};if(!read(base+0x2a55734,copy.data(),copy.size())||copy!=bytes27)return false;}
{std::array<unsigned char,4>copy{};if(!read(base+0x2aa34b4,copy.data(),copy.size())||copy!=bytes28)return false;}
{std::array<unsigned char,4>copy{};if(!read(base+0x2aaede8,copy.data(),copy.size())||copy!=bytes29)return false;}
{std::array<unsigned char,16>copy{};if(!read(base+0x2c289d0,copy.data(),copy.size())||copy!=bytes30)return false;}
{std::array<unsigned char,4>copy{};if(!read(base+0x1a2fbe0,copy.data(),copy.size())||copy!=bytes31)return false;}
{std::array<unsigned char,4>copy{};if(!read(base+0x1a2fe3c,copy.data(),copy.size())||copy!=bytes32)return false;}
{std::array<unsigned char,16>copy{};if(!read(base+0x2c14fa0,copy.data(),copy.size())||copy!=bytes33)return false;}
{std::array<unsigned char,4>copy{};if(!read(base+0x184056c,copy.data(),copy.size())||copy!=bytes34)return false;}
return true;}
}
