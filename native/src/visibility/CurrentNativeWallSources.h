#pragma once
#include "CurrentWallPatchBuilder.h"
#include "CurrentWallSeeds.h"
#include "engine/ColdOcclusionRegistry.h"
#include "engine/NativeOcclusionDrawWitness.h"
#include <optional>

namespace rdrvr::experimental {
// Native source half of the early renderer. It observes ONLY the
// selected current LEFT-depth objects and publishes at most 32 original mesh
// patches after their real DrawCommon returns. No candidate graph, mesh walk,
// late planner, previous-frame depth or substitute draw occurs here.
// The enclosing native scene must independently retain list/material/geometry
// ownership throughout all four passes; this class does not grant that lease.
class CurrentNativeWallSources {
public:
    using Native=engine::NativeStaticOcclusionSource;
    using Witness=engine::NativeOcclusionDrawWitness;
    using Registry=engine::ColdOcclusionRegistry;
    using Cones=engine::ConservativeWallCones;
    using Frame=Cones::Frame;
    struct Stats {
        unsigned selectedObjects{},inspected{},objectDeclined{},geometryDeclined{},
            primitives{},primitiveDeclined{},pipelineDeclined{},stateDeclined{},
            matrixDeclined{},bindingDeclined{},packetDeclined{},copied{},completed{},changed{};
    };
    class Object;
    class Geometry;
    class Primitive;
    void configure(Native::Api,Witness::Api,Registry*)noexcept;
    bool configured()const noexcept;
    bool begin(Frame,VkDevice,const std::array<Native::Camera,2>&,unsigned,unsigned,
               Cones&,std::span<const CurrentWallSeeds::Selection>,std::span<const WallHintExchange::Hint>)noexcept;
    void target(const Witness::Target&)noexcept;
    void end()noexcept;
    bool needsGeometryProof()const noexcept;
    void prepared(void*,const Witness::Binding&,bool exactSuccessfulLeaf)noexcept;
    // Used by Cones::Authority.sourceCurrent only while append is executing
    // inside the just-completed, still-live native DrawCommon borrow.
    bool admitsCompletedSource(Frame,const Cones::SourceKey&)const noexcept;
    const Stats& stats()const noexcept{return stats_;}
    const CurrentWallSeeds::Stats& seedStats()const noexcept{return seeds_.stats();}
    const CurrentWallPatchBuilder::Stats& patchStats()const noexcept{return patches_;}
    class Object {
        friend class CurrentNativeWallSources;
        CurrentNativeWallSources* owner_{};Object* previous_{};
        const void* info_{};Native::Object value_{};
        std::optional<Native::BorrowScope> borrow_;
        std::optional<CurrentWallSeeds::Draw> draw_;
        bool valid_{},completed_{},publishing_{};
    public:
        Object(CurrentNativeWallSources&,const void* info,bool exactMainLeftDepth)noexcept;
        ~Object();Object(const Object&)=delete;
        void complete()noexcept;
    };
    class Geometry {
        CurrentNativeWallSources* owner_{};Witness::Geometry previous_{};
    public:
        Geometry(CurrentNativeWallSources&,const void* ptr,bool exact)noexcept;
        ~Geometry();Geometry(const Geometry&)=delete;
    };
    class Primitive {
        friend class CurrentNativeWallSources;
        CurrentNativeWallSources* owner_{};Primitive* previous_{};
        void* context_{};int mode_{},first_{},count_{},base_{};
        bool indexed_{},observed_{};
    public:
        Primitive(CurrentNativeWallSources&,void*,bool indexed,int mode,int first,int count,int base)noexcept;
        ~Primitive();Primitive(const Primitive&)=delete;
    };
private:
    Native source_;Witness::Api witness_{};Registry* registry_{};
    const engine::OwnedStaticOccluders* meshes_{};
    const engine::OwnedStaticVertices* vertices_{};
    std::uint64_t meshSerial_{},vertexSerial_{};
    Frame frame_{};VkDevice device_{};unsigned width_{},height_{};
    std::array<Native::Camera,2> cameras_{};Witness::Target target_{};
    engine::StaticOcclusionProjection projection_;Cones* cones_{};
    CurrentWallSeeds seeds_;CurrentWallPatchBuilder::Stats patches_{};Stats stats_{};
    std::array<WallHintExchange::Hint,32> hints_{};unsigned hintCount_{};
    std::array<CurrentWallSeeds::Selection,32> selected_{};unsigned selectedCount_{};
    // Only the single qualified native Object owns these frame-scoped slots.
    struct Proof {Cones::SourceKey key{};Registry::Snapshot pipeline{};};
    std::array<Proof,32> proofs_{};unsigned proofCount_{};
    std::array<Cones::Source,32> copied_{};
    Object* object_{};Primitive* primitive_{};Witness::Geometry geometry_{};
    std::thread::id thread_{std::this_thread::get_id()};
    bool current()const noexcept;
};
}
