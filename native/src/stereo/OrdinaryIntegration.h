#pragma once
#include "OrdinaryVtableObserver.h"
#include "OrdinaryGateProbe.h"
#include "OrdinaryBodyGate.h"
#include <atomic>
#include <cstring>
#include <optional>

namespace rdr95::ordinary_integration {
// Published once during verified cold startup. The three native slots may retain
// a fetched wrapper even after a failed transaction: this object is never freed.
inline std::atomic<ordinary_vtable::Transaction*> transaction{};
inline bool publish(ordinary_vtable::Transaction& value) noexcept {
    auto* expected=static_cast<ordinary_vtable::Transaction*>(nullptr);
    return transaction.compare_exchange_strong(expected,&value,std::memory_order_release,std::memory_order_acquire)||expected==&value;
}
inline bool method(void*,std::uint64_t point,std::uint64_t observed,std::uint64_t expected) noexcept {
    auto* t=transaction.load(std::memory_order_acquire);
    return t&&expected==t->base()+ordinary_vtable::nativeOffset&&t->methodIsNative(point,observed);
}
// The native DrawCommon record remains borrowed only over this synchronous
// call. These reads neither admit the object nor extend the drawable lifetime.
// An empty nested scope masks an outer scope; OFF performs no native reads.
class Body final {
    std::optional<ordinary_vtable::BodyScope> scope_;
public:
    Body(void* container,const void* record,bool pinned) noexcept {
        gateprobe::hit(gateprobe::BodyCalls);
        auto* t=transaction.load(std::memory_order_acquire);
        if(!t||!t->current())return;
        gateprobe::hit(gateprobe::BodyCurrent);
        ordinary_vtable::Body value{};
        const auto address=reinterpret_cast<std::uintptr_t>(record);
        if(pinned&&container&&address&&address<=UINTPTR_MAX-0xb0){
            gateprobe::hit(gateprobe::BodyPinned);
            std::uintptr_t creature{},matrices{},drawable{},point{};std::uint32_t flags{};
            const auto* bytes=static_cast<const unsigned char*>(record);
            std::memcpy(&creature,bytes+0x98,sizeof creature);
            std::memcpy(&matrices,bytes+0x90,sizeof matrices);
            std::memcpy(&flags,bytes+0xac,sizeof flags);
            if(gateprobe::enabled)++gateprobe::bodyClasses[ordinaryBodyClass(creature!=0,matrices!=0,flags)];
            if(ordinaryBodyScalars(creature!=0,matrices!=0,flags)){
                gateprobe::hit(gateprobe::BodyStatic);
                std::memcpy(&drawable,bytes+0x80,sizeof drawable);
                if(drawable&&drawable<=UINTPTR_MAX-sizeof point){
                    std::memcpy(&point,reinterpret_cast<const void*>(drawable),sizeof point);
                    gateprobe::sample(0,point,t->base());
                    if(t->coveredAddressPoint(point)){gateprobe::hit(gateprobe::BodyCovered);value={reinterpret_cast<std::uintptr_t>(container),address,drawable,point};}
                }
            }
        }
        scope_.emplace(value);
    }
    Body(const Body&)=delete;
    Body& operator=(const Body&)=delete;
};
}
