#pragma once
#include <array>
#include <cstdint>
namespace rdr95::cached_palette_generated {
inline constexpr std::uintptr_t LockOffset=0x1a21bf0,UnlockOffset=0x187f8b4;
inline constexpr std::uintptr_t PoolLockOffset=0x4e85841c,PoolLockGotOffset=0x2e108a8; // _ZN22renDrawBucketContainer17sm_MatrixPoolLockE
inline constexpr std::uintptr_t CountOffset=0x4e858394,CountGotOffset=0x2e108b8; // _ZN22renDrawBucketContainer21sm_SkeletonCacheUsageE
inline constexpr std::uintptr_t UsedOffset=0x4e858398,UsedGotOffset=0x2e108b0; // _ZN22renDrawBucketContainer18sm_MatrixPoolUsageE
inline constexpr std::uintptr_t BanksOffset=0x4e852d70,BanksGotOffset=0x2e108a0; // _ZN22renDrawBucketContainer22sm_GlobalMatrixSetHeapE
inline constexpr std::uintptr_t IndexOffset=0x4e858390,IndexGotOffset=0x2e10898; // _ZN22renDrawBucketContainer18sm_CurMatSetBufferE
inline constexpr std::array<unsigned char,696> bytes0{
#include "gamecode/NativeCachedPaletteProof_2a26cf4_696_8h.inc"
};
inline constexpr std::array<unsigned char,476> bytes1{
#include "gamecode/NativeCachedPaletteProof_2a26fac_476_8h.inc"
};
inline constexpr std::array<unsigned char,132> bytes2{
#include "gamecode/NativeCachedPaletteProof_2a29cfc_132_8h.inc"
};
inline constexpr std::array<unsigned char,80> bytes3{
#include "gamecode/NativeCachedPaletteProof_2a29d80_80_8h.inc"
};
inline constexpr std::array<unsigned char,576> bytes4{
#include "gamecode/NativeCachedPaletteProof_2a29f58_576_8h.inc"
};
inline constexpr std::array<unsigned char,52> bytes5{
#include "gamecode/NativeCachedPaletteProof_2a29f24_52_8h.inc"
};
inline constexpr std::array<unsigned char,80> bytes6{
#include "gamecode/NativeCachedPaletteProof_2a2a198_80_8h.inc"
};
inline constexpr std::array<unsigned char,156> bytes7{
#include "gamecode/NativeCachedPaletteProof_2a2a1e8_156_8h.inc"
};
inline constexpr std::array<unsigned char,56> bytes8{
#include "gamecode/NativeCachedPaletteProof_2a2785c_56_8h.inc"
};
inline constexpr std::array<unsigned char,20> bytes9{
#include "gamecode/NativeCachedPaletteProof_2a27894_20_8h.inc"
};
inline constexpr std::array<unsigned char,480> bytes10{
#include "gamecode/NativeCachedPaletteProof_2a2a510_480_8h.inc"
};
inline constexpr std::array<unsigned char,16> bytes11{
#include "gamecode/NativeCachedPaletteProof_1b02988_16_8h.inc"
};
inline constexpr std::array<unsigned char,192> bytes12{
#include "gamecode/NativeCachedPaletteProof_1b02a78_192_8h.inc"
};
inline constexpr std::array<unsigned char,44> bytes13{
#include "gamecode/NativeCachedPaletteProof_1b0286c_44_8h.inc"
};
inline constexpr std::array<unsigned char,176> bytes14{
#include "gamecode/NativeCachedPaletteProof_1b028d8_176_8h.inc"
};
inline constexpr std::array<unsigned char,524> bytes15{
#include "gamecode/NativeCachedPaletteProof_1b02c08_524_8h.inc"
};
inline constexpr std::array<unsigned char,100> bytes16{
#include "gamecode/NativeCachedPaletteProof_1b02e14_100_8h.inc"
};
inline constexpr std::array<unsigned char,40> bytes17{
#include "gamecode/NativeCachedPaletteProof_1b02e78_40_8h.inc"
};
inline constexpr std::array<unsigned char,132> bytes18{
#include "gamecode/NativeCachedPaletteProof_1b02f94_132_8h.inc"
};
inline constexpr std::array<unsigned char,112> bytes19{
#include "gamecode/NativeCachedPaletteProof_1b02ea0_112_8h.inc"
};
inline constexpr std::array<unsigned char,4> bytes20{0x98,0xdb,0x47,0x14};
inline constexpr std::array<unsigned char,112> bytes21{
#include "gamecode/NativeCachedPaletteProof_187f844_112_8h.inc"
};
inline constexpr std::array<unsigned char,4> bytes22{0xa7,0x52,0x4e,0x14};
inline constexpr std::array<unsigned char,2708> bytes23{
#include "gamecode/NativeCachedPaletteProof_2a530f0_2708_8h.inc"
};
inline constexpr std::array<unsigned char,3252> bytes24{
#include "gamecode/NativeCachedPaletteProof_2a5810c_3252_8h.inc"
};
inline constexpr std::array<unsigned char,8> bytes25{0x00,0x38,0x40,0xf9,0x6a,0x64,0x07,0x14};
inline constexpr std::array<unsigned char,900> bytes26{
#include "gamecode/NativeCachedPaletteProof_2abd050_900_8h.inc"
};
inline constexpr std::array<unsigned char,3948> bytes27{
#include "gamecode/NativeCachedPaletteProof_2a5548c_3948_8h.inc"
};
inline constexpr std::array<unsigned char,4728> bytes28{
#include "gamecode/NativeCachedPaletteProof_23a24e0_4728_8h.inc"
};
template<class Read>bool cachedPaletteCodeMatches(std::uintptr_t base,Read&& read){if(!base||base>UINTPTR_MAX-0x4e85845c)return false;
{std::array<unsigned char,696> copy{};if(!read(base+0x2a26cf4,copy.data(),copy.size())||copy!=bytes0)return false;}
{std::array<unsigned char,476> copy{};if(!read(base+0x2a26fac,copy.data(),copy.size())||copy!=bytes1)return false;}
{std::array<unsigned char,132> copy{};if(!read(base+0x2a29cfc,copy.data(),copy.size())||copy!=bytes2)return false;}
{std::array<unsigned char,80> copy{};if(!read(base+0x2a29d80,copy.data(),copy.size())||copy!=bytes3)return false;}
{std::array<unsigned char,576> copy{};if(!read(base+0x2a29f58,copy.data(),copy.size())||copy!=bytes4)return false;}
{std::array<unsigned char,52> copy{};if(!read(base+0x2a29f24,copy.data(),copy.size())||copy!=bytes5)return false;}
{std::array<unsigned char,80> copy{};if(!read(base+0x2a2a198,copy.data(),copy.size())||copy!=bytes6)return false;}
{std::array<unsigned char,156> copy{};if(!read(base+0x2a2a1e8,copy.data(),copy.size())||copy!=bytes7)return false;}
{std::array<unsigned char,56> copy{};if(!read(base+0x2a2785c,copy.data(),copy.size())||copy!=bytes8)return false;}
{std::array<unsigned char,20> copy{};if(!read(base+0x2a27894,copy.data(),copy.size())||copy!=bytes9)return false;}
{std::array<unsigned char,480> copy{};if(!read(base+0x2a2a510,copy.data(),copy.size())||copy!=bytes10)return false;}
{std::array<unsigned char,16> copy{};if(!read(base+0x1b02988,copy.data(),copy.size())||copy!=bytes11)return false;}
{std::array<unsigned char,192> copy{};if(!read(base+0x1b02a78,copy.data(),copy.size())||copy!=bytes12)return false;}
{std::array<unsigned char,44> copy{};if(!read(base+0x1b0286c,copy.data(),copy.size())||copy!=bytes13)return false;}
{std::array<unsigned char,176> copy{};if(!read(base+0x1b028d8,copy.data(),copy.size())||copy!=bytes14)return false;}
{std::array<unsigned char,524> copy{};if(!read(base+0x1b02c08,copy.data(),copy.size())||copy!=bytes15)return false;}
{std::array<unsigned char,100> copy{};if(!read(base+0x1b02e14,copy.data(),copy.size())||copy!=bytes16)return false;}
{std::array<unsigned char,40> copy{};if(!read(base+0x1b02e78,copy.data(),copy.size())||copy!=bytes17)return false;}
{std::array<unsigned char,132> copy{};if(!read(base+0x1b02f94,copy.data(),copy.size())||copy!=bytes18)return false;}
{std::array<unsigned char,112> copy{};if(!read(base+0x1b02ea0,copy.data(),copy.size())||copy!=bytes19)return false;}
{std::array<unsigned char,4> copy{};if(!read(base+0x1a21bf0,copy.data(),copy.size())||copy!=bytes20)return false;}
{std::array<unsigned char,112> copy{};if(!read(base+0x187f844,copy.data(),copy.size())||copy!=bytes21)return false;}
{std::array<unsigned char,4> copy{};if(!read(base+0x187f8b4,copy.data(),copy.size())||copy!=bytes22)return false;}
{std::array<unsigned char,2708> copy{};if(!read(base+0x2a530f0,copy.data(),copy.size())||copy!=bytes23)return false;}
{std::array<unsigned char,3252> copy{};if(!read(base+0x2a5810c,copy.data(),copy.size())||copy!=bytes24)return false;}
{std::array<unsigned char,8> copy{};if(!read(base+0x2a4dde4,copy.data(),copy.size())||copy!=bytes25)return false;}
{std::array<unsigned char,900> copy{};if(!read(base+0x2abd050,copy.data(),copy.size())||copy!=bytes26)return false;}
{std::array<unsigned char,3948> copy{};if(!read(base+0x2a5548c,copy.data(),copy.size())||copy!=bytes27)return false;}
{std::array<unsigned char,4728> copy{};if(!read(base+0x23a24e0,copy.data(),copy.size())||copy!=bytes28)return false;}
return true;}
template<class Read>bool cachedPaletteGlobalsMatch(std::uintptr_t base,Read&& read){if(!base||base>UINTPTR_MAX-0x4e85845c)return false;
{std::uintptr_t pointer{};if(!read(base+0x2e108a8,&pointer,sizeof(pointer))||pointer!=base+0x4e85841c)return false;}
{std::uintptr_t pointer{};if(!read(base+0x2e108b8,&pointer,sizeof(pointer))||pointer!=base+0x4e858394)return false;}
{std::uintptr_t pointer{};if(!read(base+0x2e108b0,&pointer,sizeof(pointer))||pointer!=base+0x4e858398)return false;}
{std::uintptr_t pointer{};if(!read(base+0x2e108a0,&pointer,sizeof(pointer))||pointer!=base+0x4e852d70)return false;}
{std::uintptr_t pointer{};if(!read(base+0x2e10898,&pointer,sizeof(pointer))||pointer!=base+0x4e858390)return false;}
return true;}
}
