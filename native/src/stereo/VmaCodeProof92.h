#pragma once
#include <array>
#include <cstdint>
namespace rdr92 {
template<class Check>bool vmaCodeProof(Check check){return
check(0x18e2780,std::array<std::uint8_t,816>{
#include "gamecode/VmaCodeProof92_18e2780_816_8h.inc"
})&&
check(0x18e2ab0,std::array<std::uint8_t,836>{
#include "gamecode/VmaCodeProof92_18e2ab0_836_8h.inc"
})&&
check(0x18e2f64,std::array<std::uint8_t,172>{
#include "gamecode/VmaCodeProof92_18e2f64_172_8h.inc"
})&&
check(0x18b1904,std::array<std::uint8_t,380>{
#include "gamecode/VmaCodeProof92_18b1904_380_8h.inc"
})&&
check(0x18b1ab8,std::array<std::uint8_t,76>{
#include "gamecode/VmaCodeProof92_18b1ab8_76_8h.inc"
})&&
check(0x18dfc10,std::array<std::uint8_t,20>{
#include "gamecode/VmaCodeProof92_18dfc10_20_8h.inc"
})&&
check(0x2c1d340,std::array<std::uint8_t,16>{
#include "gamecode/VmaCodeProof92_2c1d340_16_8h.inc"
})&&
check(0x2c1cd10,std::array<std::uint8_t,16>{
#include "gamecode/VmaCodeProof92_2c1cd10_16_8h.inc"
})&&
check(0x2c1cd40,std::array<std::uint8_t,16>{
#include "gamecode/VmaCodeProof92_2c1cd40_16_8h.inc"
})&&
true;}
}
