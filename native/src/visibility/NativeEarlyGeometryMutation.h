#pragma once
#include "NativeResourceGate.h"
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace rdrvr::experimental {
// Exact retail grmGeometryQB::OptimiseVertexBuffersD11 entry, already covered
// by the full native witness function/PLT proof. Its asynchronous task can run
// outside ResourceQueueUpdate and can free original VB/declaration storage.
// This is a resource writer boundary, never an immutable-geometry permit.
class NativeEarlyGeometryMutation {
public:
    struct Api {
        void* context{};
        bool(*read)(void*,std::uintptr_t,void*,std::size_t)noexcept{};
        bool completeCodeProof{};
    };
    static bool optimiserNoOp(const Api& api,std::uintptr_t geometry)noexcept {
        // The native entry returns if !+14, +b0 exists, or original +18 is null.
        // Preserve its short-circuit order: never inspect an unused pointer.
        if(!api.completeCodeProof||!api.read||!geometry||geometry>UINTPTR_MAX-0xb8)return false;
        std::uint8_t enabled{};std::uintptr_t optimized{},original{};
        if(!api.read(api.context,geometry+0x14,&enabled,sizeof enabled))return false;
        if(!enabled)return true;
        if(!api.read(api.context,geometry+0xb0,&optimized,sizeof optimized))return false;
        if(optimized)return true;
        return api.read(api.context,geometry+0x18,&original,sizeof original)&&!original;
    }
    template<class Original>
    static void invoke(rdr95::NativeResourceGate& gate,const Api& api,void* geometry,Original&& original) {
        struct Errno {int value{errno};~Errno(){errno=value;}} result;
        rdr95::NativeResourceGate::Writer writer(gate,rdr95::NativeResourceGate::Writer::Mode::Quiet);
        // Writer drains short readers before examining any native fields. A
        // proven no-op blocks new reads for its call but preserves frame data.
        if(!optimiserNoOp(api,reinterpret_cast<std::uintptr_t>(geometry)))writer.markMutation();
        errno=result.value;
        try{std::forward<Original>(original)();result.value=errno;}
        catch(...){result.value=errno;throw;}
    }
};
}
