#pragma once
#include <cstdint>
namespace rdr95::joined_list {
struct CodePin{std::uintptr_t offset;unsigned size;const char* sha256;};
inline constexpr CodePin Pins[]={
 { 0x2a29810,224,"e50358430e2ddb2bcdf686d5129b4b51c0ba5ae02d7a09a7062c23df563864f3"},
 { 0x2a4dde4,8,"aea762cb6ccf9955e1358e9280eeb3e1d3c7db5eb402224b644c15de2aa5e37c"},
 { 0x23a0128,920,"aa7e23144c9b38a6b210c89a094894bce6e479333c05b14ab5b0a153d348fba8"},
 { 0x2a5810c,3252,"af0771b4227af6c2c225e3cbb864e7e5096920e335fcd2a751597a0d1c4e154c"},
 { 0x2a5548c,3948,"d8bb8df5bc01d4ff61b21d55476f7fa7c14cc12b2cddacdeafd56efc3a25fc9f"},
 { 0x2a530f0,2708,"55d4ab4e03b135cec85ef42fcbf1528771deb012701ec008ad702b09173dda0b"},
 { 0x23a24e0,4728,"7e89043f183f2b26038e7454e30c760d307dca396e54b85726cfe38e43c5abb1"},
 { 0x2abcd14,816,"105b1b8a3c3479497e27f1c85d0d01969898ba579bbc771eb6fe7c17d49924ba"},
 { 0x2abd048,8,"a7bc3397ed027c17d1a1c9a3a9650b00ce8b6c618cf2fc2e3d2963dc247a5cdd"},
 { 0x2abd050,900,"d2f7135e11ce0d75944b64f62c8d62dcfbbc3737110799c4a175686a5cbaa6f9"},
};
template<class Check>bool codeMatches(std::uintptr_t base,Check&&check){if(!base)return false;for(const auto&p:Pins)if(p.offset>UINTPTR_MAX-base||p.size>UINTPTR_MAX-(base+p.offset)||!check(base+p.offset,p.size,p.sha256))return false;return true;}
inline constexpr std::uintptr_t sceneGot=0x2e5f870,sceneReturn=0x2a585cc;
inline constexpr std::uintptr_t waitGot=0x2e5f6e8,waitReturn=0x2a66ac4;
}
