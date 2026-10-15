#pragma once
#include "NativeStaticOcclusionSource.h"
#include <cstdint>
#include <memory>
#include <thread>

namespace rdrvr::engine {
// Same-frame proof ledger only. Does not read retained native pointers, alter
// native state, submit graphics commands, or create occluders from an AABB.
// Integration owns the exact native record and complete shader/draw certificate.
class StaticStereoOcclusion {
public:
    using Object=NativeStaticOcclusionSource::Object;
    using Bounds=ConservativeStereoRaster::Bounds;
    using Reject=bool(*)(void*,std::uint64_t,const Object&) noexcept;
    using TargetVisitor=void(*)(void*,const Object&) noexcept;
    enum class Pass : unsigned {Depth,Color};
    struct DrawKey {
        std::uintptr_t container{},info{};
        int commands{},instance{};
        std::uint32_t fadeBits{},flags{},extra{};
        bool operator==(const DrawKey&)const noexcept=default;
    };
    static constexpr unsigned MaxObjects=2048,MaxCalls=4096;
    struct Stats {
        std::uint64_t frames{},leftScopes{},completedScopes{},primitives{},unsafePrimitives{};
        std::uint64_t eligibleObjects{},eligibleDepth{},eligibleColor{},queries{},queryHits{},skippedDepth{},skippedColor{};
        std::uint64_t declined{},changedObjects{},nested{},budgetStops{},invalidStages{};
    };
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    class ObjectScope {
        friend class StaticStereoOcclusion;
        StaticStereoOcclusion* owner_{};
        ObjectScope* previous_{};
        std::uint64_t epoch_{};
        unsigned object_{MaxObjects},call_{MaxCalls},draws_{};
        Pass pass_{Pass::Depth};
        bool active_{},safe_{true},finished_{};
        inline static thread_local ObjectScope* current_{};
    public:
        ObjectScope(StaticStereoOcclusion&,const DrawKey&,const Object&,bool qualified=true) noexcept;
        ~ObjectScope();
        // Call only after the original whole DrawCommon returns normally.
        void complete() noexcept;
        ObjectScope(const ObjectScope&)=delete;
        ObjectScope& operator=(const ObjectScope&)=delete;
    };
    StaticStereoOcclusion();
    ~StaticStereoOcclusion();
    StaticStereoOcclusion(const StaticStereoOcclusion&)=delete;
    StaticStereoOcclusion& operator=(const StaticStereoOcclusion&)=delete;
    void configure(Reject,void* user=nullptr) noexcept;
    bool begin(std::uint64_t frame) noexcept;
    // Copied potential targets only; no query and no new culling authority.
    // An unsupported depth pass can still acquire an independent COLOR proof.
    bool visitSourceTargets(std::uint64_t frame,TargetVisitor,void*)const noexcept;
    bool leftColor(std::uint64_t frame) noexcept;
    bool seal(std::uint64_t frame) noexcept;
    // LEFT depth sources are already sealed. An already certified DEPTH
    // object with provably visible bounds needs no COLOR shader witness at all.
    // Unknown/absent depth evidence returns true, preserving normal collection.
    // True is never permission to skip a native draw.
    bool colorMayBeHidden(std::uint64_t frame,const Object& fresh) noexcept;
    bool beginRight(std::uint64_t frame,Pass) noexcept;
    // Every ACTUAL emitted primitive under the current LEFT object must call
    // this, including false for unsupported/nonindexed/indirect/instanced draws.
    void observe(bool candidateSafe) noexcept;
    // Cheap preflight before borrowing native object metadata. True is only a
    // reason to perform the fresh sameObject check, never permission to skip.
    bool candidate(std::uint64_t frame,Pass,const DrawKey&) const noexcept;
    // liveCheck is deliberately evaluated on EVERY hidden RIGHT call, after the
    // cached coverage result. Pending tasks/native writes cannot reuse a LEFT
    // or RIGHT-depth quiescence decision in the later color pass.
    bool reject(std::uint64_t frame,Pass,const DrawKey&,const Object& fresh,
        Reject liveCheck=nullptr,void* liveUser=nullptr) noexcept;
    void end() noexcept;
    // Published at seal/end only. Safe for a diagnostic reader on another thread.
    Stats snapshot() const noexcept;
private:
    bool ownerThread()const noexcept;
    void finish(ObjectScope&,bool complete) noexcept;
    void poison(ObjectScope&) noexcept;
};
}
