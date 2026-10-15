#pragma once
// Hand assets embedded by tools/hands_assets.py from assets/vrhands (UltimateXR BigHands, MIT,
// see assets/vrhands/SOURCE.md and ULTIMATEXR_LICENSE.txt).
#include <array>
#include <cstdint>
#include <span>
namespace rdrvr::vr::hands::assets {
// BigHandLeft.uxrh sha256=6bb32e6f79ed6dba401bd36dd39d3178f8259096f889ef01c870d057134eb535 vertices=2437 indices=13092
// BigHandRight.uxrh sha256=48b14a77efea68027f5ae6723994cadc06dac58708ecb7339cc16ecb1ade2478 vertices=2437 indices=13092
// BigHandsAlbedo.rgba sha256=46ba8b2f08ce1ca420537e060f64be139dd39a22357d4ea5b9b7b69da2ba2ad4 -> 512x512 R8G8B8A8_SRGB, 10 mips, 1398100 bytes, chain sha256=653e369285913f6fa3d535658c9c27284238dc58af604718ae146cca71454d7a
inline constexpr std::uint32_t albedoSize=512,albedoLevels=10;
struct AlbedoLevel {std::uint32_t offset,size;};
inline constexpr std::array<AlbedoLevel,albedoLevels> albedoLayout{{{0,512},{1048576,256},{1310720,128},{1376256,64},{1392640,32},{1396736,16},{1397760,8},{1398016,4},{1398080,2},{1398096,1}}};
std::span<const unsigned char> leftMesh() noexcept;   // UXRH v1 as stored
std::span<const unsigned char> rightMesh() noexcept;
std::span<const unsigned char> albedo() noexcept;     // mip chain, level 0 first, tightly packed RGBA8 sRGB
}
