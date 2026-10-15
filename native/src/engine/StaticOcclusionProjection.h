#pragma once
#include "NativeStaticOcclusionSource.h"
#include <span>

namespace rdrvr::engine {
// Error envelope for the certified full-precision relative-affine shader.
// No fitted tolerances: bounds follow the actual float matrices, object extent,
// source WorldSingle arithmetic error, Vulkan float/raster precision and D24.
class StaticOcclusionProjection {
public:
    using Raster=ConservativeStereoRaster;
    using Camera=NativeStaticOcclusionSource::Camera;
    bool configure(const std::array<Camera,2>&,unsigned width,unsigned height) noexcept;
    bool errors(const Raster::Bounds&,const Raster::Vec3& worldError,
                std::array<Raster::Errors,2>&) const noexcept;
    // Original convex source vertices only. Componentwise shader arithmetic
    // errors retain the full vertex extent, while nominal projective extrema
    // use real vertices rather than unrelated corners of a world-axis box.
    // Never supply a clipped/generated polygon with the original error lost.
    bool errorsSource(std::span<const Raster::Vec3>,const Raster::Vec3& worldError,
                      std::array<Raster::Errors,2>&) const noexcept;
    const std::array<Raster::Matrix,2>& matrices()const noexcept{return matrices_;}
private:
    bool errorsImpl(const Raster::Bounds&,const Raster::Vec3&,
                    std::span<const Raster::Vec3>,std::array<Raster::Errors,2>&) const noexcept;
    std::array<Camera,2> camera_{};
    std::array<Raster::Matrix,2> matrices_{};
    unsigned width_{},height_{};
};
}
