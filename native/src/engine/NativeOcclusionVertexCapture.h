#pragma once
#include "OwnedStaticVertices.h"
#include "OwnedStaticOccluders.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace rdrvr::engine {
// Cold resource preparation only. Copy optimized static position data while
// its original native Lock is still held; no GPU map, readback or wait.
class NativeOcclusionVertexCapture {
public:
    using Read=bool(*)(void*,std::uintptr_t,void*,std::size_t);
    struct Api {
        Read read{};void* context{};
        std::uintptr_t geometryVtable{},vertexVtable{},bufferVtable{},unlockReturn{};
        std::uint64_t epoch{};bool completeProof{};
        std::uintptr_t indexVtable{};
    };
    class Scope {
        friend class NativeOcclusionVertexCapture;
        NativeOcclusionVertexCapture* owner_{};Scope* previous_{};
        const void* geometry_{};bool valid_{};
    public:
        Scope(NativeOcclusionVertexCapture&,const void* geometry)noexcept;
        ~Scope();Scope(const Scope&)=delete;
    };
    void configure(Api,OwnedStaticVertices*,OwnedStaticOccluders* mesh=nullptr)noexcept;
    void beforeUnlock(const void* vertex,std::uintptr_t caller)noexcept;
    void beforeLock(const void* vertex)noexcept;
    void beforeBackingDestroy(const void* backing)noexcept;
    void beforeIndexLock(const void* index)noexcept;
    void afterIndexUnlock(const void* index)noexcept;
    struct Stats {std::uint64_t scopes{},attempts{},captured{},refused{},meshAttempts{},meshCaptured{},meshRefused{};};
    Stats stats()const noexcept;
private:
    static thread_local Scope* current_;
    Api api_{};OwnedStaticVertices* cache_{};bool ready_{};
    OwnedStaticOccluders* meshes_{};
    std::atomic<std::uint64_t> scopes_{},attempts_{},captured_{},refused_{};
    std::atomic<std::uint64_t> meshAttempts_{},meshCaptured_{},meshRefused_{};
};
inline NativeOcclusionVertexCapture nativeOcclusionVertexCapture;
} // namespace rdrvr::engine
