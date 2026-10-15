#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
namespace rdrvr::engine {
// d11::VkContext_c::EndOcclusionQuery(unsigned long long); full code, SHA256 4b3afcc62f081de3ce73d9c34e9c65db23d112ef7120a1b96313fc7876030af4
inline constexpr std::array<unsigned char,224> queryCode0{
#include "gamecode/NativeQueryCodeProof_18afd14_224_8h.inc"
};
// d11::VkContext_c::FlushOcclusionBeginList(); full code, SHA256 456ef0602816614d13ee0e8fcd443a52e044ba7b846f0a8a2dffad46771264c2
inline constexpr std::array<unsigned char,148> queryCode1{
#include "gamecode/NativeQueryCodeProof_18af180_148_8h.inc"
};
// d11::VkContext_c::BeginConditionalQuery(unsigned long long); full code, SHA256 110f46b5b35c069160560c6ad6786f647dd44e8760a52a46fc22dbbcd7630b91
inline constexpr std::array<unsigned char,4> queryCode2{0xc0,0x03,0x5f,0xd6};
// d11::VkContext_c::BeginOcclusionQuery(unsigned long long); full code, SHA256 97d0bdc3f2e79b6f4b1105ae7e9c8d20dc3a51b832fcd07a207272ef1bea6020
inline constexpr std::array<unsigned char,144> queryCode3{
#include "gamecode/NativeQueryCodeProof_18afc84_144_8h.inc"
};
// d11::VkContext_c::BindInputLayout(); full code, SHA256 e4b27f2bbc55077284b622bac79cddc134ba0bb60917815bad08746a357fa392
inline constexpr std::array<unsigned char,1604> queryCode4{
#include "gamecode/NativeQueryCodeProof_18af214_1604_8h.inc"
};
// d11::VkContext_c::EndConditionalQuery(unsigned long long); full code, SHA256 110f46b5b35c069160560c6ad6786f647dd44e8760a52a46fc22dbbcd7630b91
inline constexpr std::array<unsigned char,4> queryCode5{0xc0,0x03,0x5f,0xd6};
// d11::VkContext_c::BeginConditionalRender; full code, SHA256 110f46b5b35c069160560c6ad6786f647dd44e8760a52a46fc22dbbcd7630b91
inline constexpr std::array<unsigned char,4> queryCode6{0xc0,0x03,0x5f,0xd6};
// d11::VkContext_c::EndConditionalRender; full code, SHA256 110f46b5b35c069160560c6ad6786f647dd44e8760a52a46fc22dbbcd7630b91
inline constexpr std::array<unsigned char,4> queryCode7{0xc0,0x03,0x5f,0xd6};
template<class Read>bool nativeQueryCodeMatches(std::uintptr_t base,Read&& read) {
    if(!base)return false;
    if(base>UINTPTR_MAX-0x18afdf4)return false;
    {std::array<unsigned char,224> observed{};if(!read(base+0x18afd14,observed.data(),observed.size())||observed!=queryCode0)return false;}
    if(base>UINTPTR_MAX-0x18af214)return false;
    {std::array<unsigned char,148> observed{};if(!read(base+0x18af180,observed.data(),observed.size())||observed!=queryCode1)return false;}
    if(base>UINTPTR_MAX-0x18ad568)return false;
    {std::array<unsigned char,4> observed{};if(!read(base+0x18ad564,observed.data(),observed.size())||observed!=queryCode2)return false;}
    if(base>UINTPTR_MAX-0x18afd14)return false;
    {std::array<unsigned char,144> observed{};if(!read(base+0x18afc84,observed.data(),observed.size())||observed!=queryCode3)return false;}
    if(base>UINTPTR_MAX-0x18af858)return false;
    {std::array<unsigned char,1604> observed{};if(!read(base+0x18af214,observed.data(),observed.size())||observed!=queryCode4)return false;}
    if(base>UINTPTR_MAX-0x18ad56c)return false;
    {std::array<unsigned char,4> observed{};if(!read(base+0x18ad568,observed.data(),observed.size())||observed!=queryCode5)return false;}
    if(base>UINTPTR_MAX-0x18ad570)return false;
    {std::array<unsigned char,4> observed{};if(!read(base+0x18ad56c,observed.data(),observed.size())||observed!=queryCode6)return false;}
    if(base>UINTPTR_MAX-0x18ad574)return false;
    {std::array<unsigned char,4> observed{};if(!read(base+0x18ad570,observed.data(),observed.size())||observed!=queryCode7)return false;}
    return true;
}
}
