#pragma once
#include <array>
#include <cstdint>
namespace rdrvr::experimental::instanceProof67 {
inline constexpr std::uintptr_t function0Offset=0x2abcd14;
inline constexpr std::array<unsigned char,816> function0Bytes={
#include "gamecode/InstanceFrequencyCode67_2abcd14_816_8d.inc"
};
inline constexpr std::uintptr_t function1Offset=0x2a29810;
inline constexpr std::array<unsigned char,224> function1Bytes={
#include "gamecode/InstanceFrequencyCode67_2a29810_224_8d.inc"
};
inline constexpr std::uintptr_t function2Offset=0x2a298f0;
inline constexpr std::array<unsigned char,1004> function2Bytes={
#include "gamecode/InstanceFrequencyCode67_2a298f0_1004_8d.inc"
};
inline constexpr std::uintptr_t function3Offset=0x2abd9a4;
inline constexpr std::array<unsigned char,1300> function3Bytes={
#include "gamecode/InstanceFrequencyCode67_2abd9a4_1300_8d.inc"
};
inline constexpr std::uintptr_t function4Offset=0x2a260e8;
inline constexpr std::array<unsigned char,644> function4Bytes={
#include "gamecode/InstanceFrequencyCode67_2a260e8_644_8d.inc"
};
inline constexpr std::uintptr_t function5Offset=0x2a2a3e4;
inline constexpr std::array<unsigned char,112> function5Bytes={
#include "gamecode/InstanceFrequencyCode67_2a2a3e4_112_8d.inc"
};
inline constexpr std::uintptr_t function6Offset=0x2a27348;
inline constexpr std::array<unsigned char,332> function6Bytes={
#include "gamecode/InstanceFrequencyCode67_2a27348_332_8d.inc"
};
inline constexpr std::uintptr_t function7Offset=0x2a27e9c;
inline constexpr std::array<unsigned char,448> function7Bytes={
#include "gamecode/InstanceFrequencyCode67_2a27e9c_448_8d.inc"
};
inline constexpr std::uintptr_t function8Offset=0x2a2a454;
inline constexpr std::array<unsigned char,76> function8Bytes={
#include "gamecode/InstanceFrequencyCode67_2a2a454_76_8d.inc"
};
inline constexpr std::uintptr_t function9Offset=0x2a2a4a0;
inline constexpr std::array<unsigned char,64> function9Bytes={
#include "gamecode/InstanceFrequencyCode67_2a2a4a0_64_8d.inc"
};
inline constexpr std::uintptr_t caller0Offset=0x2abcec4;
inline constexpr std::array<unsigned char,32> caller0Bytes={
#include "gamecode/InstanceFrequencyCode67_2abcec4_32_8d.inc"
};
inline constexpr std::uintptr_t caller1Offset=0x2a298c0;
inline constexpr std::array<unsigned char,32> caller1Bytes={
#include "gamecode/InstanceFrequencyCode67_2a298c0_32_8d.inc"
};
inline constexpr std::uintptr_t caller2Offset=0x2a57234;
inline constexpr std::array<unsigned char,32> caller2Bytes={
#include "gamecode/InstanceFrequencyCode67_2a57234_32_8d.inc"
};
inline constexpr std::uintptr_t caller3Offset=0x2abd03c;
inline constexpr std::array<unsigned char,32> caller3Bytes={
#include "gamecode/InstanceFrequencyCode67_2abd03c_32_8d.inc"
};
inline constexpr std::uintptr_t caller4Offset=0x2a55650;
inline constexpr std::array<unsigned char,32> caller4Bytes={
#include "gamecode/InstanceFrequencyCode67_2a55650_32_8d.inc"
};
template<class Check> bool proof(Check&& check){return check(function0Offset,function0Bytes)&&check(function1Offset,function1Bytes)&&check(function2Offset,function2Bytes)&&check(function3Offset,function3Bytes)&&check(function4Offset,function4Bytes)&&check(function5Offset,function5Bytes)&&check(function6Offset,function6Bytes)&&check(function7Offset,function7Bytes)&&check(function8Offset,function8Bytes)&&check(function9Offset,function9Bytes)&&check(caller0Offset,caller0Bytes)&&check(caller1Offset,caller1Bytes)&&check(caller2Offset,caller2Bytes)&&check(caller3Offset,caller3Bytes)&&check(caller4Offset,caller4Bytes);}
}
