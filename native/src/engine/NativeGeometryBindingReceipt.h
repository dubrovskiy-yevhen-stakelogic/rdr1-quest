#pragma once
#include "OwnedStaticOccluders.h"
#include <utility>

namespace rdrvr::engine {
class NativeOcclusionDrawWitness;
class NativeStaticOcclusionSource;
class NativeStaticTriangles;
// Sealed copied evidence from matchesGeometry, usable only by the same live
// native DrawCommon borrow. It owns no native memory and grants no extra lease.
class NativeGeometryBindingReceipt {
    friend class NativeOcclusionDrawWitness;
    friend class NativeStaticOcclusionSource;
    friend class NativeStaticTriangles;
    OwnedStaticOccluders::Identity meshIdentity_{};
    const NativeStaticOcclusionSource* owner_{};
    const void* scope_{};
    std::uint64_t scopeGeneration_{},frame_{};
    std::uintptr_t context_{},command_{};
    unsigned positionBytes_{},stride_{};
    std::uint64_t inspectionGeneration_{};bool transformQualified_{};bool valid_{};
public:
    NativeGeometryBindingReceipt()=default;
    NativeGeometryBindingReceipt(const NativeGeometryBindingReceipt&)=delete;
    NativeGeometryBindingReceipt& operator=(const NativeGeometryBindingReceipt&)=delete;
    NativeGeometryBindingReceipt(NativeGeometryBindingReceipt&& other)noexcept{*this=std::move(other);}
    NativeGeometryBindingReceipt& operator=(NativeGeometryBindingReceipt&& other)noexcept {
        if(this!=&other){meshIdentity_=other.meshIdentity_;owner_=other.owner_;scope_=other.scope_;
            scopeGeneration_=other.scopeGeneration_;frame_=other.frame_;context_=other.context_;command_=other.command_;
            positionBytes_=other.positionBytes_;stride_=other.stride_;inspectionGeneration_=other.inspectionGeneration_;transformQualified_=other.transformQualified_;valid_=other.valid_;other.reset();}return *this;
    }
    void reset()noexcept{transformQualified_=false;inspectionGeneration_=0;valid_=false;owner_=nullptr;scope_=nullptr;scopeGeneration_=frame_=context_=command_=0;}
    explicit operator bool()const noexcept{return valid_;}
};
}
