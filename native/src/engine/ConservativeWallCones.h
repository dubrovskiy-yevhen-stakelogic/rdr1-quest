#pragma once
#include "ConservativeStereoRaster.h"
#include <array>
#include <cstdint>
#include <span>

namespace rdrvr::engine {
// PRIVATE bounded same-frame analytic coverage classifier. No native calls,
// mesh traversal, grid rasterization, previous-frame verdict, or allocation.
// It DOES NOT certify source admission/materials: a current native authority
// must supply both callbacks. Recorded-scene adapters are offline evidence only.
class ConservativeWallCones {
public:
    using Raster=ConservativeStereoRaster;
    static constexpr unsigned MaxWalls=32,MaxCandidates=8192,MaxQueries=MaxCandidates*4;
    struct Frame {
        std::uint64_t scene{},pose{},admission{},mutation{};
        bool operator==(const Frame&)const=default;
        bool valid()const noexcept{return scene&&pose&&admission;}
    };
    struct SourceKey {
        // lod is the selected numeric LOD; zero is valid.
        std::uint64_t object{},geometry{},material{},lod{},transform{};
        std::uint64_t geometryGeneration{},materialGeneration{},transformGeneration{},record{};
        // Exact immutable OwnedMesh patch index under geometryGeneration; 0 is valid.
        std::uint32_t patch{};
        bool operator==(const SourceKey&)const=default;
    };
    struct Source {
        SourceKey key{};
        Raster::Patch polygon{};
        std::array<Raster::Errors,2> errors{};
    };
    struct Authority {
        void* context{};
        // Exact current scene+frozen pose+admission+resource mutation ticket.
        bool(*frameCurrent)(void*,const Frame&)noexcept{};
        // Current admitted record, chosen LOD/material/fade/opaque depth-state,
        // actual certified shader/geometry/transform and immutable polygon.
        // patch must belong to that exact geometry generation.
        // No old visible/hidden bit is a legal implementation of this callback.
        bool(*sourceCurrent)(void*,const Frame&,const SourceKey&)noexcept{};
    };
    struct Stats {
        std::uint64_t sources{},admissionDeclined{},invalidSources{},sourceEyes{},
            sourceClipped{},sourceBackfaces{},sourceDegenerate{},candidates{},
            invalidCandidates{},eyeTests{},planeTests{},oneEye{},bothEyes{},
            stale{},budget{};
    };
    bool build(Frame,const std::array<Raster::Matrix,2>&,std::span<const Source>,Authority)noexcept;
    // Monotonic CURRENT-frame seed publication after its native opaque draw.
    // O(polygon vertices squared), no reprojection of existing walls or reset
    // of candidate decisions. At most MaxWalls source attempts per frame.
    bool append(Frame,const Source&)noexcept;
    // Return: bit1 = left fully hidden, bit2 = right. Native whole-object reject only
    // when result==3 AND caller separately proves candidate bounds/side effects.
    // The candidate error is the unchanged existing full-AABB error envelope.
    std::uint8_t hidden(Frame,const Raster::Bounds&,const std::array<Raster::Errors,2>&)noexcept;
    bool ready(Frame)const noexcept;
    std::uint64_t buildSerial()const noexcept{return buildSerial_;}
    std::uint64_t validSourceRevision()const noexcept{return revision_;}
    const Stats& stats()const noexcept{return stats_;}
    void reset()noexcept;
private:
    struct Edge {double ax{},ay{},dx{},dy{},margin{};};
    struct EyeWall {std::array<Edge,Raster::MaxPatchVertices> edges{};unsigned count{};double farther{};bool valid{};};
    struct Wall {std::array<EyeWall,2> eye{};};
    struct P {double x{},y{},z{},w{};};
    bool project(unsigned,const Raster::Vec3&,P&)const noexcept;
    bool source(unsigned,const Source&,EyeWall&)noexcept;
    bool candidate(unsigned,const Raster::Bounds&,const Raster::Errors&,double&,double&,double&,double&,double&)const noexcept;
    Frame frame_{};Authority authority_{};std::array<Raster::Matrix,2> matrices_{};
    std::array<Wall,MaxWalls> walls_{};unsigned count_{};bool built_{};Stats stats_{};
    std::uint64_t buildSerial_{},revision_{};bool serialExhausted_{};
};
}


