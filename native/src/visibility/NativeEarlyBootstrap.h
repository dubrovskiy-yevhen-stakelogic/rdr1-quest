#pragma once
#include "NativeEarlyImageProof.h"
#include "CurrentNativeWallSources.h"
#include <atomic>

namespace rdrvr::experimental {
// Immutable process-image configuration, published by TraceEntry::arm BEFORE
// the original Android entry starts native workers. Device/frame ownership is
// deliberately absent: GameXr must acquire its real current scene and resource
// leases before constructing a controller on the render thread.
class NativeEarlyBootstrap {
public:
    using Sources=CurrentNativeWallSources;
    using TlsSlot=std::uintptr_t(*)()noexcept;
    bool publish(std::uintptr_t image,NativeEarlyImageProof::Api checks,
                 Sources::Native::Api source,Sources::Witness::Api witness,TlsSlot tlsSlot=nullptr)noexcept {
        if(ready_.load(std::memory_order_acquire)||!source.completeCodeProof||!witness.completeCodeProof||
           !source.read||!source.drawableDraw||!source.triangles.ownedOccluders||!source.triangles.ownedEpoch||
           source.staticVtable!=witness.staticVtable||source.contextVtable!=witness.contextVtable||
           source.drawableDraw!=witness.drawableDraw||!witness.shadowBufferEnabledSlot||!proof_.bind(image,checks))return false;
        image_=image;source_=source;witness_=witness;tlsSlot_=tlsSlot;ready_.store(true,std::memory_order_release);return true;
    }
    bool ready()const noexcept{return ready_.load(std::memory_order_acquire);}
    std::uintptr_t image()const noexcept{return ready()?image_:0;}
    const Sources::Native::Api* source()const noexcept{return ready()?&source_:nullptr;}
    const Sources::Witness::Api* witness()const noexcept{return ready()?&witness_:nullptr;}
    // Resolve OUTSIDE the short resource borrow. GameXr then reads this thread's
    // exact TLS pointer slot inside currentPass, without a native TLS resolver
    // call or assuming the stage-entry Gfx pointer can never change.
    std::uintptr_t currentGfxSlot()const noexcept{return ready()&&tlsSlot_?tlsSlot_():0;}
    rdr95::NativeAdmissionProduction::ImageProof imageProof()noexcept{return ready()?proof_.callbacks():rdr95::NativeAdmissionProduction::ImageProof{};}
private:
    NativeEarlyImageProof proof_;Sources::Native::Api source_{};Sources::Witness::Api witness_{};
    std::uintptr_t image_{};TlsSlot tlsSlot_{};std::atomic<bool> ready_{};
};
inline NativeEarlyBootstrap nativeEarlyBootstrap;
}
