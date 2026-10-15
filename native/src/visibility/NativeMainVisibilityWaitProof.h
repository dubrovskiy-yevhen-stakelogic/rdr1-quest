#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace rdrvr::engine::main_wait_generated {
inline constexpr unsigned MainRequest=13;
inline constexpr std::uintptr_t MainWaitOffset=0x2abd050,MainGotOffset=0x2e5f6e8,MainReturnOffset=0x2a66ac4;
inline constexpr std::array<unsigned char,900> waitBytes{
#include "gamecode/NativeMainVisibilityWaitProof_2abd050_900_8h.inc"
};
inline constexpr std::array<unsigned char,5708> callerBytes{
#include "gamecode/NativeMainVisibilityWaitProof_2a666e8_5708_8h.inc"
};
inline constexpr std::array<unsigned char,16> pltBytes{
#include "gamecode/NativeMainVisibilityWaitProof_2cae110_16_8h.inc"
};
inline constexpr std::array<unsigned char,4> callBytes{0x94,0x1d,0x09,0x94};
template<class Read>bool mainVisibilityWaitCodeMatches(std::uintptr_t base,Read&&read) {
if(!base||base>UINTPTR_MAX-0x2cae120)return false;
{std::array<unsigned char,900>actual{};if(!read(base+0x2abd050,actual.data(),actual.size())||actual!=waitBytes)return false;}
{std::array<unsigned char,5708>actual{};if(!read(base+0x2a666e8,actual.data(),actual.size())||actual!=callerBytes)return false;}
{std::array<unsigned char,16>actual{};if(!read(base+0x2cae110,actual.data(),actual.size())||actual!=pltBytes)return false;}
{std::array<unsigned char,4>actual{};if(!read(base+0x2a66ac0,actual.data(),actual.size())||actual!=callBytes)return false;}
return true;
}
}
