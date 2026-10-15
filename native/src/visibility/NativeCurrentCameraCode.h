#pragma once
#include <array>
#include <cstdint>
#include <span>
namespace rdrvr::experimental::current_camera_code {
struct Pin {std::uintptr_t offset;std::span<const unsigned char> bytes;};
inline constexpr std::array<unsigned char,972> code0={
#include "gamecode/NativeCurrentCameraCode_1afe47c_972_8d.inc"
};
inline constexpr std::array<unsigned char,176> code1={
#include "gamecode/NativeCurrentCameraCode_1afefe4_176_8d.inc"
};
inline constexpr std::array<unsigned char,384> code2={
#include "gamecode/NativeCurrentCameraCode_1aff9d8_384_8d.inc"
};
inline constexpr std::array<unsigned char,20> code3={
#include "gamecode/NativeCurrentCameraCode_188b9ac_20_8d.inc"
};
inline constexpr std::array<unsigned char,392> code4={
#include "gamecode/NativeCurrentCameraCode_1affb58_392_8d.inc"
};
inline constexpr std::array<unsigned char,792> code5={
#include "gamecode/NativeCurrentCameraCode_1ac60c0_792_8d.inc"
};
inline constexpr std::array<unsigned char,184> code6={
#include "gamecode/NativeCurrentCameraCode_1ac4728_184_8d.inc"
};
inline constexpr std::array<unsigned char,464> code7={
#include "gamecode/NativeCurrentCameraCode_1afdda8_464_8d.inc"
};
inline constexpr std::array<unsigned char,312> code8={
#include "gamecode/NativeCurrentCameraCode_1b011c0_312_8d.inc"
};
inline constexpr std::array<unsigned char,44> code9={
#include "gamecode/NativeCurrentCameraCode_188b980_44_8d.inc"
};
inline constexpr std::array<unsigned char,232> code10={
#include "gamecode/NativeCurrentCameraCode_18aed10_232_8d.inc"
};
inline constexpr std::array pins={Pin{0x1afe47c,code0},Pin{0x1afefe4,code1},Pin{0x1aff9d8,code2},Pin{0x188b9ac,code3},Pin{0x1affb58,code4},Pin{0x1ac60c0,code5},Pin{0x1ac4728,code6},Pin{0x1afdda8,code7},Pin{0x1b011c0,code8},Pin{0x188b980,code9},Pin{0x18aed10,code10}};
}
