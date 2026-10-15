#pragma once
#include <cstdint>
namespace rdr95::retail {
inline constexpr char ElfSha256[]="3f80845edec32a663c42a8570e3131446813e695076c4bc96e2142c190bc22f3";
inline constexpr std::uintptr_t instanceVtable = 0x2dd3a28;
inline constexpr std::uintptr_t geometryVtable = 0x2cdf480;
inline constexpr std::uintptr_t vertexVtable = 0x2cdf660;
inline constexpr std::uintptr_t indexVtable = 0x2cdf790;
inline constexpr std::uintptr_t bufferVtable = 0x2cbedd0;
inline constexpr std::uintptr_t gfxVtable = 0x2cbd400;
inline constexpr std::uintptr_t vkVtable = 0x2cbe5b8;
inline constexpr std::uintptr_t rasterVtable = 0x2cbf2d0;
inline constexpr std::uintptr_t depthVtable = 0x2cbed40;
inline constexpr std::uintptr_t vertexShaderVtable = 0x2cbf490;
inline constexpr std::uintptr_t fragmentShaderVtable = 0x2cbf510;
inline constexpr std::uintptr_t programVtable = 0x2cbf6a0;
inline constexpr std::uintptr_t shadowBufferSlot = 0x2e7a408;
inline constexpr std::uintptr_t forceShaderSlot = 0x4e6e1668;
inline constexpr std::uintptr_t shaderSorterSlot = 0x4e6e1750;
inline constexpr std::uintptr_t commandBufferSlot = 0x4e6d8be0;
inline constexpr std::uintptr_t modelCullbackSlot = 0x2e7a3f0;
inline constexpr std::uintptr_t pureModelCullback = 0x1b12904;
inline constexpr std::uintptr_t globalValues = 0x4e6dba80;
inline constexpr std::uintptr_t globalBuffers = 0x4e6d9178;
inline constexpr std::uintptr_t currentViewport = 0x4e6e14d8;
inline constexpr std::uintptr_t drawableDraw = 0x1d9ac1c;
inline constexpr std::uintptr_t drawableDrawMulti = 0x1d9ac40;
inline constexpr std::uintptr_t wait = 0x2abd050;
inline constexpr std::uintptr_t drawCommon = 0x2a28238;
inline constexpr std::uintptr_t effectLoad = 0x1ae3490;
inline constexpr std::uintptr_t effectShutdown = 0x1ae2930;
inline constexpr std::uintptr_t forcedGroup = 0x2e7a43c;
inline constexpr std::uintptr_t fallbackGroup = 0x2e7a440;
inline constexpr std::uintptr_t fallbackEnabled = 0x4e6e2590;
inline constexpr std::uintptr_t minimumFade = 0x4e852d04;
inline constexpr std::uintptr_t litGroup = 0x2e7a430;
inline constexpr std::uintptr_t unlitGroup = 0x2e7a434;
inline constexpr std::uintptr_t gfxTlsDescriptor = 0x2ddce90;
inline constexpr std::uintptr_t matrixIds = 0x4e6e14f0;
inline constexpr std::uintptr_t pendingRaster = 0x4e6da2a0;
inline constexpr std::uintptr_t pendingDepth = 0x4e6da380;
inline constexpr std::uintptr_t readyFlags = 0x4e6da3ac;
inline constexpr std::uintptr_t defaults = 0x2e73730;
inline constexpr std::uintptr_t shadow = 0x4e6e1104;
inline constexpr std::uintptr_t globalBias = 0x4e852d00;
inline constexpr std::uintptr_t markDynamic = 0x4e85c568;
inline constexpr std::uintptr_t factoryDirty = 0x4e6d9148;
}
