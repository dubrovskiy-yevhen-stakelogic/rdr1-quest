#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace rdrvr::engine::effect_mutation_generated {
inline constexpr std::uintptr_t LoadOffset=0x1ae3490,LoadGotOffset=0x2e22098,ShutdownOffset=0x1ae2930,ShutdownGotOffset=0x2e22038;
struct Pin {std::uintptr_t offset;const unsigned char*bytes;std::size_t size;};
inline constexpr std::array<unsigned char,240> code0{
#include "gamecode/NativeEffectMutationProof_1ae5098_240_8h.inc"
};
inline constexpr std::array<unsigned char,16> code1{
#include "gamecode/NativeEffectMutationProof_2c33550_16_8h.inc"
};
inline constexpr std::array<unsigned char,372> code2{
#include "gamecode/NativeEffectMutationProof_1ae6454_372_8h.inc"
};
inline constexpr std::array<unsigned char,16> code3{
#include "gamecode/NativeEffectMutationProof_2c33580_16_8h.inc"
};
inline constexpr std::array<unsigned char,560> code4{
#include "gamecode/NativeEffectMutationProof_1ae0a58_560_8h.inc"
};
inline constexpr std::array<unsigned char,16> code5{
#include "gamecode/NativeEffectMutationProof_2c33300_16_8h.inc"
};
inline constexpr std::array<unsigned char,260> code6{
#include "gamecode/NativeEffectMutationProof_1ae526c_260_8h.inc"
};
inline constexpr std::array<unsigned char,16> code7{
#include "gamecode/NativeEffectMutationProof_2c33510_16_8h.inc"
};
inline constexpr std::array<unsigned char,848> code8{
#include "gamecode/NativeEffectMutationProof_1ae2930_848_8h.inc"
};
inline constexpr std::array<unsigned char,16> code9{
#include "gamecode/NativeEffectMutationProof_2c333b0_16_8h.inc"
};
inline constexpr std::array<unsigned char,348> code10{
#include "gamecode/NativeEffectMutationProof_1ae0f2c_348_8h.inc"
};
inline constexpr std::array<unsigned char,16> code11{
#include "gamecode/NativeEffectMutationProof_2c33330_16_8h.inc"
};
inline constexpr std::array<unsigned char,7176> code12{
#include "gamecode/NativeEffectMutationProof_1ae3490_7176_8h.inc"
};
inline constexpr std::array<unsigned char,16> code13{
#include "gamecode/NativeEffectMutationProof_2c33470_16_8h.inc"
};
inline constexpr std::array<unsigned char,404> code14{
#include "gamecode/NativeEffectMutationProof_1ae0cdc_404_8h.inc"
};
inline constexpr std::array<unsigned char,16> code15{
#include "gamecode/NativeEffectMutationProof_2c33310_16_8h.inc"
};
inline constexpr std::array<unsigned char,252> code16{
#include "gamecode/NativeEffectMutationProof_1ae5954_252_8h.inc"
};
inline constexpr std::array<unsigned char,16> code17{
#include "gamecode/NativeEffectMutationProof_2c33520_16_8h.inc"
};
inline constexpr std::array<unsigned char,468> code18{
#include "gamecode/NativeEffectMutationProof_1ae2fd8_468_8h.inc"
};
inline constexpr std::array<unsigned char,764> code19{
#include "gamecode/NativeEffectMutationProof_1ae2634_764_8h.inc"
};
inline constexpr std::array<Pin,20> Pins{{{28201112,code0.data(),code0.size()},{46347600,code1.data(),code1.size()},{28206164,code2.data(),code2.size()},{46347648,code3.data(),code3.size()},{28183128,code4.data(),code4.size()},{46347008,code5.data(),code5.size()},{28201580,code6.data(),code6.size()},{46347536,code7.data(),code7.size()},{28191024,code8.data(),code8.size()},{46347184,code9.data(),code9.size()},{28184364,code10.data(),code10.size()},{46347056,code11.data(),code11.size()},{28193936,code12.data(),code12.size()},{46347376,code13.data(),code13.size()},{28183772,code14.data(),code14.size()},{46347024,code15.data(),code15.size()},{28203348,code16.data(),code16.size()},{46347552,code17.data(),code17.size()},{28192728,code18.data(),code18.size()},{28190260,code19.data(),code19.size()}}};
template<class Read>bool effectMutationCodeMatches(std::uintptr_t base,Read&&read){
if(!base||base>UINTPTR_MAX-0x2e220a0)return false;
std::array<unsigned char,7176> actual{};
for(const auto&p:Pins){if(p.size>actual.size()||!read(base+p.offset,actual.data(),p.size))return false;for(std::size_t i=0;i<p.size;++i)if(actual[i]!=p.bytes[i])return false;}
return true;
}
}
