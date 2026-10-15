#pragma once
#include "IndexedCommandState.h"
#include "CameraMainRoute.h"
#include "engine/EyeViewports.h"

namespace rdr95 {
// Private data requalification, NOT permission to omit a native draw. The
// caller is the synchronous OrdinaryPacketCapture.completed integration and
// supplies that just-completed group's actual sealed packet. Ordinary object,
// intended technique/fixed state, GPU buffer lifetime and attachment ownership
// remain separate admission obligations. Never retags a recording or pass.
class OwnedDepthRequalification final {
    using Mirror=rdrvr::engine::IndexedCommandState;
    using Registry=rdrvr::engine::FrozenCameraCreationRegistry;
    using Views=rdrvr::engine::EyeViewports;
    struct Extent {unsigned width{},height{};};
public:
    // Cheap lexical pre-gate only. No descriptor, geometry or resource reads.
    static bool selectedEye(unsigned eye)noexcept {
        CameraMainRoute::PacketScopeStamp stamp;return eye<2&&route(eye,stamp);
    }
    class Source final {
        friend class OwnedDepthRequalification;
        Mirror::OwnedPacket packet_{};
        Registry::PlainDepthCertificate certificate_{};
        std::uint64_t scene_{},pose_{};
        Extent extent_{};
    public: Source()=default;
    };
    // This result does not outlive mirror, registry, uniform owners or the
    // lexical selected MAIN Draw. `current` is suitable for a terminal replay
    // guard; resolving the copied packet alone does not extend that lifetime.
    // current() is ONLY a pure route/mirror liveness predicate. The controlled
    // NativeRuntime plan owns PSO/resource epoch checks between raw PFNs.
    // resolve() performs full certificate/owned-byte checks once before plan
    // preparation. current() alone neither revalidates nor leases resources.
    class PreparedRight final {
        friend class OwnedDepthRequalification;
        Mirror* mirror_{};
        Mirror::OwnedPacket packet_{};
        Registry::PlainDepthCertificate certificate_{};
        CameraMainRoute::PacketScopeStamp route_{};
        std::uint64_t observed_{};
    public:
        PreparedRight()=default;
        bool current()const noexcept;
        bool resolve(Mirror::OwnedPacket&)const noexcept;
    };
    // Failures leave output unchanged. No Vulkan PFNs, allocation, native
    // camera mutation or callback which exposes a borrowed native pointer.
    static bool captureLeft(Mirror&,const Mirror::OwnedPacket&,
        const Registry::PlainDepthCertificate&,const Views&,Source&)noexcept;
    static bool requalifyRight(Mirror&,const Source&,const Views&,PreparedRight&)noexcept;
private:
    static bool route(unsigned,CameraMainRoute::PacketScopeStamp&)noexcept;
    static bool extent(const Views&,unsigned,const CameraMainRoute::PacketScopeStamp&,Extent&)noexcept;
    static bool full(const VkViewport&,const VkRect2D&,Extent)noexcept;
    static bool proof(Mirror&,const Mirror::OwnedPacket&,const Registry::PlainDepthCertificate&)noexcept;
    static bool local(const Mirror&,const Mirror::OwnedPacket&,std::uint64_t)noexcept;
};
}
