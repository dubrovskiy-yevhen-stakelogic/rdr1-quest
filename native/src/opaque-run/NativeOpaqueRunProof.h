#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
// Generated from exact retail full-function/callsite bytes; private only.
namespace rdrvr::experimental::opaque_run_generated {
struct CodePin{std::uintptr_t offset;std::size_t count;const std::uint8_t* bytes;const char* sha256;};
// _ZN22renDrawBucketContainer4DrawEP21VisibilityListRequestPKN4rage13atFixedBitSetILi32EjEEjjjbbj
inline constexpr std::uint8_t Code0[]={
#include "gamecode/NativeOpaqueRunProof_2a28fb4_2128_8h.inc"
};
// _ZNK4rage11datCallback4CallEPv
inline constexpr std::uint8_t Code1[]={
#include "gamecode/NativeOpaqueRunProof_1a8a4dc_276_8h.inc"
};
// _ZN11sagRenderer11DrawEnvMapsEv
inline constexpr std::uint8_t Code2[]={
#include "gamecode/NativeOpaqueRunProof_2a5ac98_1432_8h.inc"
};
// _ZN12ltLightProxy18DrawWaterOccludersEv
inline constexpr std::uint8_t Code3[]={
#include "gamecode/NativeOpaqueRunProof_2a65fbc_972_8h.inc"
};
// _ZNK12ltLightProxy20DrawTerrainDepthOnlyEPKN4rage11grcViewportEbbb
inline constexpr std::uint8_t Code4[]={
#include "gamecode/NativeOpaqueRunProof_2a66388_864_8h.inc"
};
// _ZN12ltLightProxy27DrawVisibleItemsForViewportEPKN4rage11grcViewportEjj
inline constexpr std::uint8_t Code5[]={
#include "gamecode/NativeOpaqueRunProof_2a666e8_5708_8h.inc"
};
// _ZN8sagWorld9DrawStartERN4rage11grcViewportEbN21VisibilityListRequest13DrawListIndexEPKNS0_13atFixedBitSetILi32EjEE
inline constexpr std::uint8_t Code6[]={
#include "gamecode/NativeOpaqueRunProof_2aabe04_1352_8h.inc"
};
// PLT _ZN22renDrawBucketContainer4DrawEP21VisibilityListRequestPKN4rage13atFixedBitSetILi32EjEEjjjbbj
inline constexpr std::uint8_t Code7[]={
#include "gamecode/NativeOpaqueRunProof_2cad6b0_16_8h.inc"
};
// PLT _ZNK4rage11datCallback4CallEPv
inline constexpr std::uint8_t Code8[]={
#include "gamecode/NativeOpaqueRunProof_2c27880_16_8h.inc"
};
// DrawCommon PLT
inline constexpr std::uint8_t Code9[]={
#include "gamecode/NativeOpaqueRunProof_2cad680_16_8h.inc"
};
// bucket-pre
inline constexpr std::uint8_t Code10[]={
0x03,0xf9,0x07,0x94,
};
// bucket-post
inline constexpr std::uint8_t Code11[]={
0xb2,0xf8,0x07,0x94,
};
// opaque-loop-0
inline constexpr std::uint8_t Code12[]={
0xef,0x0f,0x0a,0x94,
};
// opaque-loop-1
inline constexpr std::uint8_t Code13[]={
0xc7,0x0f,0x0a,0x94,
};
inline constexpr std::array<CodePin,14> Pins{{
{0x2a28fb4,sizeof(Code0),Code0,"61b1b72cebee7db395e893b332c2e7aa58b1b7da4cce147df156cbe019e4355a"},
{0x1a8a4dc,sizeof(Code1),Code1,"40f9a9617976cbf496943efd6725a083d52995ad7f9e50cb688a766edb28b155"},
{0x2a5ac98,sizeof(Code2),Code2,"532e2c347f299140bee0ad86206aa76e9b04a75d7890ddb9ee03b906ecc72e87"},
{0x2a65fbc,sizeof(Code3),Code3,"961889767d759f059f296465976e6d0bdcf8a002189b78f0ffd750cc44dda375"},
{0x2a66388,sizeof(Code4),Code4,"fbd103a53db691c1309a19daf205f229723453795276b4e9fd4400549aa5185b"},
{0x2a666e8,sizeof(Code5),Code5,"065cb0f03d7945989ea98d61cbecf84d61da18109220c4ce94540ca6d87a3a56"},
{0x2aabe04,sizeof(Code6),Code6,"c6dd0aa5ad2acabdc7e410f3ec14633aa053fd0d4b2266e4e9209e98ffc66bbc"},
{0x2cad6b0,sizeof(Code7),Code7,"d20eaa75123691b55465e1a5c55e6bd48757782bf62907510dafbe90e2589c8e"},
{0x2c27880,sizeof(Code8),Code8,"401af53f3557e37702ec9c6de0c811cf79e7a47c2e62f2ad47397b8ffe2acd1b"},
{0x2cad680,sizeof(Code9),Code9,"5b51ec27a10a3022c15b7e64e203c11797ad346e8daead71f7dfc4f8ed0f6cbd"},
{0x2a29474,sizeof(Code10),Code10,"2b2aa67b7e845570c09c3e647ed7f620fed588e003d4fd657f20a0b028d4bf0f"},
{0x2a295b8,sizeof(Code11),Code11,"5950d47d082216acfb550c0891de3bb10335744ce1a8c4bb257bdbe3f21db230"},
{0x2a296c4,sizeof(Code12),Code12,"ad5184ffe6835acf0450c0ae248c5cd6ac3bbcbffa4bbc384c0122d5ec2a4c07"},
{0x2a29764,sizeof(Code13),Code13,"fcbc13b8545070b37034b2d65a3688dc6f9b17a9201733a3d736b55fc50dd4f7"},
}};
inline constexpr std::array<std::uintptr_t,2> Slots{0x2e5f1b8,0x2e1c2a0};
inline constexpr std::array<std::uintptr_t,2> Offsets{0x2a28fb4,0x1a8a4dc};
inline constexpr std::uintptr_t LargestEnd=0x2e5f1c0;
inline constexpr std::array<std::uintptr_t,20> InvocationReturns{0x2a5af70,0x2a66238,0x2a6655c,0x2a66954,0x2a6699c,0x2a669e8,0x2a66a38,0x2a66e38,0x2a66e90,0x2a67008,0x2a67078,0x2a671c4,0x2a67204,0x2a674dc,0x2a675e8,0x2a67630,0x2a67700,0x2aac1fc,0x2aac2b0,0x2aac2e4};
}
