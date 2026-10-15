#pragma once
#include "NativeIndexedObservation.h"
#include "OrdinaryVtableObserver.h"
#include "UniformCaptureControl.h"
#include <array>
#include <memory>
#include <optional>
#include <span>

namespace rdr95 {
// Groups terminal indexed draws made by ONE verified ordinary virtual call.
// This is capture, not admission: publishing a packet does not authorize any
// native skip, borrowed resource lifetime, other eye, or later command buffer.
class OrdinaryPacketCapture final {
public:
    using Mirror=NativeIndexedObservation::Mirror;
    using Body=ordinary_vtable::Body;
    using Call=ordinary_vtable::Call;
    struct Api {
        void* context{};
        // Synchronous borrowed view. A consumer may copy packet values to its
        // own storage, but must perform independent destination admission.
        void(*completed)(void*,const Body&,const Call&,Mirror&,VkCommandBuffer,
                         std::span<const Mirror::OwnedPacket>)noexcept{};
    };
    using RecordingThread=bool(*)()noexcept;
    OrdinaryPacketCapture(NativeIndexedObservation&,UniformCaptureControl&,const UniformCaptureSink&,RecordingThread)noexcept;
    ~OrdinaryPacketCapture();
    OrdinaryPacketCapture(const OrdinaryPacketCapture&)=delete;
    OrdinaryPacketCapture& operator=(const OrdinaryPacketCapture&)=delete;
    bool configure(Api)noexcept; // one cold attempt; absent consumer keeps OFF
    void disarm()noexcept;
    ordinary_vtable::Observer::Api observer()noexcept;
    // Terminal wrapper calls this AFTER successful finishCall for the exact
    // raw draw. A failed raw guard must call invalidateCurrent instead.
    static void indexed(VkCommandBuffer,Mirror::Indexed)noexcept;
    static void invalidateCurrent()noexcept;
private:
    struct Storage {std::array<Mirror::OwnedPacket,Mirror::maxPackets> packets;};
    NativeIndexedObservation& observation_;UniformCaptureControl& control_;const UniformCaptureSink& sink_;
    RecordingThread recordingThread_{};Api api_{};std::unique_ptr<Storage> storage_;
    std::atomic<bool> configured_{},ready_{},stopped_{};
    std::atomic<std::uint64_t> revocation_{1};
    std::thread::id owner_{};bool ownerChosen_{};
    std::optional<UniformCaptureControl::Producer> producer_;
    inline static thread_local OrdinaryPacketCapture* active_{};
    std::uint64_t serial_{},currentSerial_{},effects_{},revocationAtBegin_{};
    Body body_{};Call call_{};VkCommandBuffer command_{};Mirror* mirror_{};
    unsigned count_{};bool failed_{},publishing_{};
    static std::uint64_t before(void*,const Body&,const Call&)noexcept;
    static void after(void*,std::uint64_t,const Body&,const Call&,bool)noexcept;
    bool current()noexcept;
    void capture(VkCommandBuffer,Mirror::Indexed)noexcept;
    void revoke()noexcept;
};
}
