#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace rdrvr::engine {

// A CPU-only, same-frame visibility proof. This is deliberately NOT a native
// mesh reader or a runtime toggle. The integration must certify every submitted
// triangle: actual current static render geometry, no deformation/discard/fade,
// opaque depth writes, exact current transform/LOD/cull mode, and a source lease
// held while copying vertices. Collision meshes and object bounds are NOT valid
// occluders. Candidates require complete bounds including every rendered part.
// Depth is normal Z (smaller means nearer), using LESS/LEQUAL; reverse-Z needs
// a separately proved conversion. Submitted occluders need actual full viewport/
// scissor and sample coverage: no alpha-to-coverage, sample mask, stencil reject,
// rasterizer discard, or unmodelled clipping/depth rewrite. A draw that was only
// prepared, then declined before the actual GPU command, is NOT an occluder.
//
// Matrices are row-major, multiply a column [world.xyz,1], and produce Vulkan
// clip coordinates (0 <= z <= w). Grid Y increases with projected clip Y.
// Error bounds must cover the REAL GPU projection/raster/depth differences,
// including geometry conversion, shader math and depth bias. They are not tuning
// knobs. Without such a bound, do not begin a frame. Unknown input stays visible.
// No prior frame, asset pointer, heap allocation, lock, GPU work, or clock is used.
class PlannerReplaySnapshot;
class ConservativeStereoRaster {
    friend class PlannerReplaySnapshot;
    struct Projected { double x, y, z, w; };
public:
    static constexpr unsigned Width = 64, Height = 64, Eyes = 2;
    struct Vec3 { double x{}, y{}, z{}; };
    using Matrix = std::array<double, 16>;
    struct Errors {
        // Euclidean projected vertex displacement, in this 64x64 grid.
        double coverageCells{};
        // Absolute error in the depth function throughout a covered cell and
        // the projected candidate. Includes any viewport/depth-range mapping.
        double depth{};
    };
    enum class Faces : std::uint8_t { Unknown, Both, PositiveArea, NegativeArea };
    struct Triangle { std::array<Vec3, 3> vertex{}; Faces faces{Faces::Unknown}; };
    // A cold-certified exact union of coplanar native triangles. No hull over
    // holes is permitted. Erode only its convex external boundary, retaining
    // coverage across shared internal native triangle edges.
    static constexpr unsigned MaxPatchVertices=16;
    struct Patch { std::array<Vec3,MaxPatchVertices> vertex{}; std::uint8_t count{}; Faces faces{Faces::Unknown}; };
    struct Bounds { Vec3 minimum{}, maximum{}; };
    class PreparedPatch {
        friend class ConservativeStereoRaster;
        struct Eye {std::array<Projected,MaxPatchVertices> p;double minZ{},maxZ{};int x0{},x1{},y0{},y1{};};
        std::array<Eye,Eyes> eye_;
        Bounds bounds_{};const ConservativeStereoRaster* owner_{};
        std::uint64_t frame_{},generation_{};unsigned count_{},mask_{};
    public:
        const Bounds& bounds()const noexcept{return bounds_;}
    };
    struct Budget {
        std::uint32_t triangles{4096};
        std::uint32_t rasterCells{131072};
        std::uint32_t candidates{8192};
        std::uint32_t queryCells{131072};
    };
    struct Stats {
        std::uint64_t triangles{}, invalidTriangles{}, culledFaces{}, clippedTriangles{};
        std::uint64_t rasterCells{}, coveredCells{}, depthUpdates{};
        std::array<std::uint64_t,Eyes> uniqueCoveredCells{};
        std::uint64_t sourceTileRebuilds{};
        std::uint64_t candidates{}, rejected{}, oneEyeOnly{}, invalidCandidates{}, queryCells{};
        std::uint64_t rasterBudgetStops{}, queryBudgetStops{}, staleCalls{};
        std::uint64_t preparations{},preparedEyes{},earlyClipped{},earlyCulled{},earlyEmpty{},preparationCells{},preparationBudgetBypass{};
    };

    bool begin(std::uint64_t frame, const std::array<Matrix, Eyes>& views,
               const std::array<Errors, Eyes>& errors, Budget budget) noexcept;
    bool submit(std::uint64_t frame, const Triangle&) noexcept;
    bool submit(std::uint64_t frame, const Triangle&, const std::array<Errors,Eyes>&) noexcept;
    bool submit(std::uint64_t frame, const Patch&) noexcept;
    bool submit(std::uint64_t frame, const Patch&, const std::array<Errors,Eyes>&) noexcept;
    // Project and reject useless source geometry BEFORE computing its precise
    // shader/raster error envelope. Geometry-only cell coverage is a superset
    // of every valid positively-eroded result, never an occlusion proof itself.
    bool prepare(std::uint64_t frame,const Patch&,PreparedPatch&) noexcept;
    bool prepare(std::uint64_t frame,const Triangle&,PreparedPatch&) noexcept;
    bool submit(std::uint64_t frame,const PreparedPatch&,const std::array<Errors,Eyes>&) noexcept;
    bool seal(std::uint64_t frame) noexcept;
    bool hidden(std::uint64_t frame, const Bounds&) noexcept;
    bool hidden(std::uint64_t frame, const Bounds&, const std::array<Errors,Eyes>&) noexcept;
    // Source-selection heuristic only. A tile stores the MAX of its 8x8
    // current cells, queried before seal. This does not authorize native culls.
    // Dirty tiles are rebuilt lazily, at most 64 reads per changed queried tile.
    float sourceTileDepth(std::uint64_t frame,unsigned eye,unsigned x,unsigned y) noexcept;
    float sourceCellDepth(std::uint64_t frame,unsigned eye,unsigned x,unsigned y) const noexcept;
    void reset() noexcept;
    const Stats& stats() const noexcept { return stats_; }
    bool ready(std::uint64_t frame) const noexcept { return frame && frame == frame_ && sealed_; }
    // Read-only diagnostics/oracle support. Unknown cells return 1 (far plane).
    float depth(std::uint64_t frame, unsigned eye, unsigned x, unsigned y) const noexcept;

private:
    bool prepare(std::uint64_t,const Vec3*,unsigned,Faces,PreparedPatch&) noexcept;
    void sourceCellDecrease(unsigned eye,unsigned x,unsigned y,float old)noexcept;
    bool projectRaw(unsigned eye,const Vec3&,Projected&)const noexcept;
    bool projectSourceXY(unsigned eye,const Vec3&,Projected&)const noexcept;
    bool prepareEye(unsigned,const Vec3*,unsigned,Faces,PreparedPatch::Eye&) noexcept;
    bool rasterizePrepared(unsigned,const PreparedPatch::Eye&,unsigned,const Errors&)noexcept;
    bool project(unsigned eye, const Vec3&, Projected&) const noexcept;
    bool rasterize(unsigned eye, const Triangle&) noexcept;
    bool rasterize(unsigned eye,const Vec3*,unsigned count,Faces) noexcept;
    bool hiddenEye(unsigned eye, const Bounds&, bool& valid) noexcept;
    std::array<std::array<float, Width * Height>, Eyes> depth_{};
    std::array<std::array<float,64>,Eyes> sourceTiles_{};
    std::array<std::uint64_t,Eyes> sourceTileDirty_{};
    std::array<std::array<std::uint8_t,64>,Eyes> sourceTileMaxCount_{};
    std::array<Matrix, Eyes> views_{};
    std::array<Errors, Eyes> errors_{};
    Budget budget_{};
    Stats stats_{};
    std::uint64_t frame_{};
    std::uint64_t generation_{};
    bool sealed_{};
};
} // namespace rdrvr::engine
