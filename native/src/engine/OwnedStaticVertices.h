#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <span>

namespace rdrvr::engine {
// CPU copies made ONLY while an audited native optimizer owns a live writable
// mapping, immediately before its original Unlock. Never retains that mapping.
// Every backing mutation must invalidate BEFORE native Lock/destruction. The
// caller still proves native ownership; the cache cannot infer it from bytes.
class OwnedStaticVertices {
    struct Impl;
public:
    static constexpr std::size_t MaxBytes=16*1024*1024;
    static constexpr unsigned MaxEntries=4096,TableCapacity=8192,MaxProbe=32;
    struct XYZ { float x{},y{},z{}; };
    struct Identity {
        std::uintptr_t vertexBuffer{},nativeBuffer{},allocation{},fvf{};
        std::uint64_t gpuBuffer{};
        std::array<std::byte,16> fvfBytes{};
        std::uint32_t byteSize{},vertexCount{},stride{};
        bool operator==(const Identity&)const=default;
    };
    class Lease {
    public:
        Lease()=default;
        ~Lease()=default;
        Lease(const Lease&)=delete;
        Lease& operator=(const Lease&)=delete;
        Lease(Lease&&) noexcept;
        Lease& operator=(Lease&&) noexcept;
        void reset() noexcept;
        explicit operator bool()const noexcept { return lock_.owns_lock()&&!positions.empty(); }
        Identity identity{};
        std::span<const XYZ> positions{};
        std::uint64_t epoch{},generation{};
    private:
        friend class OwnedStaticVertices;
        const Impl* owner_{};
        std::uint64_t incarnation_{};
        std::shared_lock<std::shared_mutex> lock_{};
    };
    struct Statistics {
        std::uint64_t epoch{},entries{},bytes{},captures{},invalidations{},evictions{},declined{};
    };
    OwnedStaticVertices();
    ~OwnedStaticVertices(); // All callers and outstanding leases must be gone.
    OwnedStaticVertices(const OwnedStaticVertices&)=delete;
    OwnedStaticVertices& operator=(const OwnedStaticVertices&)=delete;
    bool bind(std::uint64_t epoch) noexcept;
    void clear() noexcept;
    void clear(std::uint64_t expectedEpoch) noexcept; // Retain this binding; empty all copies.
    // qualified means the exact optimizer Unlock with exclusive native mapping
    // ownership. availableBytes is the verified extent starting at mapped.
    // FVF6/7 positions begin at byte0. Only XYZ is retained; consumers MUST have
    // the CertifiedXYZ1 shader proof, since packed attribute.w is not copied.
    bool capture(std::uint64_t epoch,const Identity&,const void* mapped,
                 std::size_t availableBytes,bool qualified) noexcept;
    // Underlying VkBuffer_c pointer, not grcVB: aliases share invalidation.
    // Untracked dynamic buffers return after bounded atomic reads, no lock.
    void invalidate(std::uintptr_t nativeBuffer) noexcept;
    // try_lock_shared only: a cold writer makes the source visible this frame.
    // Keep the lease through actual-header recopy and triangle publication.
    bool query(std::uint64_t epoch,const Identity&,Lease&)const noexcept;
    bool current(const Lease&)const noexcept;
    std::uint64_t invalidationSerial()const noexcept;
    Statistics statistics()const noexcept;
private:
    std::unique_ptr<Impl> impl_;
};
extern OwnedStaticVertices ownedStaticVertices;
}
