#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cerrno>

namespace rdrvr::experimental {
// Actual native readers have two disjoint lifetime contracts: the joined MAIN
// list owns its bounded list/header storage until the color consumer returns;
// drawable/model/effect reads require the short resource exclusion. An epoch
// alone or the current thread never grants memcpy access to arbitrary memory.
// Callbacks must be supplied by those real owners, not by cache/hint identities.
class NativeEarlyRuntimeBinding;
class NativeEarlyReadRouter {
    friend class NativeEarlyRuntimeBinding;
public:
    struct Api {
        void* context{};
        bool(*guarded)(void*,std::uintptr_t,void*,std::size_t)noexcept{};
        bool(*resourceBorrow)(void*)noexcept{};
        bool(*listBorrow)(void*,std::uintptr_t,std::size_t)noexcept{};
    };
    explicit NativeEarlyReadRouter(Api api)noexcept:api_(api){}
    bool read(std::uintptr_t address,void* output,std::size_t bytes)const noexcept {
        // errno's address belongs to this synchronous thread invocation. Keep
        // callback-to-callback values unchanged, but resolve the cell only once.
        int* const errorCell=&errno;
        struct Restore{int* cell;int value;~Restore(){*cell=value;}} restore{errorCell,*errorCell};
        if(!address||!output||!bytes||bytes>UINTPTR_MAX-address)return false;
        if((api_.resourceBorrow&&api_.resourceBorrow(api_.context))||
           (api_.listBorrow&&api_.listBorrow(api_.context,address,bytes))){
            std::memcpy(output,reinterpret_cast<const void*>(address),bytes);return true;
        }
        return api_.guarded&&api_.guarded(api_.context,address,output,bytes);
    }
    static bool nativeRead(std::uint64_t address,void* output,std::size_t bytes,void* router)noexcept {
        return router&&static_cast<const NativeEarlyReadRouter*>(router)->read(address,output,bytes);
    }
    static bool sourceRead(void* router,std::uintptr_t address,void* output,std::size_t bytes)noexcept {
        return router&&static_cast<const NativeEarlyReadRouter*>(router)->read(address,output,bytes);
    }
private:
    // This entry is installed only by the real Binding. Generic read() and its
    // callbacks remain unchanged; neither a caller bool nor Api grants copies.
    enum class CopyResult {Unavailable,Failed,Copied};
    CopyResult(*copy_)(void*,std::uintptr_t,void*,std::size_t)noexcept{};
    bool scopedRead(std::uintptr_t address,void* output,std::size_t bytes)const noexcept {
        if(copy_){
            // Binding's private callback preserves errno through its exact
            // lexical Read's cached cell. Failed/Unavailable are neutral.
            const auto result=copy_(api_.context,address,output,bytes);
            if(result!=CopyResult::Unavailable)return result==CopyResult::Copied;
        }
        return read(address,output,bytes);
    }
    static bool physicalNativeRead(std::uint64_t address,void* output,std::size_t bytes,void* router)noexcept {
        return router&&static_cast<const NativeEarlyReadRouter*>(router)->scopedRead(address,output,bytes);
    }
    static bool physicalSourceRead(void* router,std::uintptr_t address,void* output,std::size_t bytes)noexcept {
        return router&&static_cast<const NativeEarlyReadRouter*>(router)->scopedRead(address,output,bytes);
    }
    Api api_;
};
}
