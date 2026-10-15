#pragma once
#include <array>
#include <cstdint>
namespace rdrvr::experimental::modelProof69 {
inline constexpr std::uintptr_t body0Offset=0x1d9d594;
inline constexpr std::array<unsigned char,196> body0Bytes={
#include "gamecode/NativeModelCode69_1d9d594_196_8d.inc"
};
inline constexpr std::uintptr_t body1Offset=0x1d9d658;
inline constexpr std::array<unsigned char,888> body1Bytes={
#include "gamecode/NativeModelCode69_1d9d658_888_8d.inc"
};
inline constexpr std::uintptr_t body2Offset=0x1b13184;
inline constexpr std::array<unsigned char,776> body2Bytes={
#include "gamecode/NativeModelCode69_1b13184_776_8d.inc"
};
inline constexpr std::uintptr_t body3Offset=0x1b12904;
inline constexpr std::array<unsigned char,196> body3Bytes={
#include "gamecode/NativeModelCode69_1b12904_196_8d.inc"
};
inline constexpr std::uintptr_t body4Offset=0x1b011c0;
inline constexpr std::array<unsigned char,312> body4Bytes={
#include "gamecode/NativeModelCode69_1b011c0_312_8d.inc"
};
inline constexpr std::uintptr_t body5Offset=0x1b1348c;
inline constexpr std::array<unsigned char,328> body5Bytes={
#include "gamecode/NativeModelCode69_1b1348c_328_8d.inc"
};
inline constexpr std::uintptr_t body6Offset=0x1b27488;
inline constexpr std::array<unsigned char,172> body6Bytes={
#include "gamecode/NativeModelCode69_1b27488_172_8d.inc"
};
inline constexpr std::uintptr_t body7Offset=0x1b9e350;
inline constexpr std::array<unsigned char,8> body7Bytes={224,3,31,42,192,3,95,214};
inline constexpr std::uintptr_t body8Offset=0x1d9ac1c;
inline constexpr std::array<unsigned char,36> body8Bytes={
#include "gamecode/NativeModelCode69_1d9ac1c_36_8d.inc"
};
inline constexpr std::uintptr_t body9Offset=0x1d9ac40;
inline constexpr std::array<unsigned char,40> body9Bytes={
#include "gamecode/NativeModelCode69_1d9ac40_40_8d.inc"
};
inline constexpr std::uintptr_t body10Offset=0x1b135d4;
inline constexpr std::array<unsigned char,456> body10Bytes={
#include "gamecode/NativeModelCode69_1b135d4_456_8d.inc"
};
inline constexpr std::uintptr_t body11Offset=0x1b129c8;
inline constexpr std::array<unsigned char,40> body11Bytes={
#include "gamecode/NativeModelCode69_1b129c8_40_8d.inc"
};
inline constexpr std::uintptr_t body12Offset=0x2a28238;
inline constexpr std::array<unsigned char,3168> body12Bytes={
#include "gamecode/NativeModelCode69_2a28238_3168_8d.inc"
};
template<class Check> bool proof(Check&& check){return check(body0Offset,body0Bytes)&&check(body1Offset,body1Bytes)&&check(body2Offset,body2Bytes)&&check(body3Offset,body3Bytes)&&check(body4Offset,body4Bytes)&&check(body5Offset,body5Bytes)&&check(body6Offset,body6Bytes)&&check(body7Offset,body7Bytes)&&check(body8Offset,body8Bytes)&&check(body9Offset,body9Bytes)&&check(body10Offset,body10Bytes)&&check(body11Offset,body11Bytes)&&check(body12Offset,body12Bytes);}
}
