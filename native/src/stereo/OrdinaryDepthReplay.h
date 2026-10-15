#pragma once
#include "OrdinaryNativeSnapshot.h"
#include "OrdinaryPacketCapture.h"
#include "OwnedDepthRequalification.h"
#include "OwnedIndexedReplay.h"
#include <memory>

namespace rdr95 {
// Single ordinary rigid geometry, LEFT depth capture -> RIGHT depth replay.
// The original Common prefix/tail, visibility list and unsupported objects
// remain native. All graph comparisons happen before GPU plan preparation.
class OrdinaryDepthReplay final {
public:
    using Observer=ordinary_vtable::Observer;
    using Body=ordinary_vtable::Body;using Call=ordinary_vtable::Call;
    using Mirror=OrdinaryPacketCapture::Mirror;
    using ContextQuery=bool(*)(OrdinaryReplayContext&)noexcept;
    struct Stats {
        std::uint64_t left{},captured{},right{},replayed{},duplicate{},capacity{},
            nativeDeclined{},packetDeclined{},targetDeclined{},emitDeclined{},faults{},reads{},bytes{};
    };
    OrdinaryDepthReplay(NativeIndexedObservation&,OrdinaryPacketCapture&,NativeCameraRuntime&,ContextQuery)noexcept;
    ~OrdinaryDepthReplay();
    OrdinaryDepthReplay(const OrdinaryDepthReplay&)=delete;
    // Called once on the ACTUAL native recording thread at selected LEFT
    // frame begin, before original MAIN wait. No native reader runs here.
    bool begin()noexcept;
    void end()noexcept;
    const Stats& statistics()const noexcept;
    static Observer::Api observer()noexcept;
    static OrdinaryPacketCapture::Api consumer()noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
    inline static thread_local OrdinaryDepthReplay* active_{};
    inline static thread_local OrdinaryDepthReplay* callOwner_{};
    static std::uint64_t before(void*,const Body&,const Call&)noexcept;
    static void after(void*,std::uint64_t,const Body&,const Call&,bool)noexcept;
    static Observer::ReplayResult replay(void*,const Body&,const Call&,Observer::ReplayGuard)noexcept;
    static void fault(void*,const Body&,const Call&)noexcept;
    static void completed(void*,const Body&,const Call&,Mirror&,VkCommandBuffer,std::span<const Mirror::OwnedPacket>)noexcept;
};
}
