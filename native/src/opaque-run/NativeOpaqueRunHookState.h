#pragma once
#include "NativeOpaqueRunCoverage.h"
#include "NativeOpaqueRunProof.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>

namespace rdrvr::experimental {
// PRIVATE: lifecycle is cold and serialized. No lock is held across a native
// original. Api/context and mapped image lifetime must extend past all fetched
// wrappers. setup is one-shot; original addresses never change after publish.
class NativeOpaqueRunHookState {
public:
    using Address=std::uintptr_t;
    struct Api {
        void* context{};
        bool(*read)(void*,Address,void*,std::size_t)noexcept{};
        bool(*range)(void*,Address,std::size_t,unsigned elfFlags)noexcept{};
        // O(1) atomic load from the two once-validated, stable GOT words; no
        // guarded syscall, /proc/maps, lock, allocation, or native call here.
        bool(*loadSlot)(void*,Address,Address&)noexcept{};
        bool(*replaceSlot)(void*,Address,Address,Address)noexcept{};
        // Mandatory third dependency: exact live DrawCommon/bodyDraw slot AND
        // all original-before-call invalidation routes are owned and ready.
        bool(*dependencyCurrent)(void*)noexcept{};
    };
    explicit NativeOpaqueRunHookState(NativeOpaqueRunCoverage& coverage)noexcept:coverage_(coverage){}
    NativeOpaqueRunHookState(const NativeOpaqueRunHookState&)=delete;
    bool setup(Address image,Api api,std::array<Address,2> wrappers)noexcept {
        PreserveErrno saved;std::lock_guard lock(mutex_);
        if(configured_||!image||!api.read||!api.range||!api.loadSlot||!api.replaceSlot||
           !api.dependencyCurrent||!wrappers[0]||!wrappers[1]||wrappers[0]==wrappers[1]||
           image>UINTPTR_MAX-opaque_run_generated::LargestEnd)return false;
        if(!coverage_.attach(this,currentOwned))return false;
        image_=image;api_=api;wrappers_=wrappers;configured_=true;return true;
    }
    bool install()noexcept {
        PreserveErrno saved;std::lock_guard lock(mutex_);coverage_.disarm();
        const auto revision=coverage_.state_.load(std::memory_order_acquire);
        if(!configured_||ownedMask_||fault_.load(std::memory_order_acquire)||!verifyCode()||
           !api_.dependencyCurrent(api_.context))return false;
        for(unsigned i=0;i<2;++i){Address v{};
            if(!api_.loadSlot(api_.context,image_+opaque_run_generated::Slots[i],v)||
               v!=image_+opaque_run_generated::Offsets[i]||v==wrappers_[i])return false;
        }
        // Both originals are visible before even the FIRST slot can expose a
        // wrapper. Retain them after every partial failure/removal.
        for(unsigned i=0;i<2;++i)originals_[i].store(image_+opaque_run_generated::Offsets[i],std::memory_order_release);
        for(unsigned i=0;i<2;++i){
            if(!api_.replaceSlot(api_.context,image_+opaque_run_generated::Slots[i],original(i),wrappers_[i])){
                rollback();return false;
            }
            ownedMask_|=1u<<i;
        }
        if(!currentOwned(this)||!coverage_.arm(revision)){rollback();return false;}
        return true;
    }
    bool remove()noexcept {
        PreserveErrno saved;std::lock_guard lock(mutex_);coverage_.disarm();return rollback();
    }
    bool rearm()noexcept {
        PreserveErrno saved;std::lock_guard lock(mutex_);coverage_.disarm();
        const auto revision=coverage_.state_.load(std::memory_order_acquire);
        return configured_&&ownedMask_==3&&!fault_.load(std::memory_order_acquire)&&verifyCode()&&
            currentOwned(this)&&coverage_.arm(revision);
    }
    void disarm()noexcept {PreserveErrno saved;coverage_.disarm();}
    bool ready()noexcept {PreserveErrno saved;return coverage_.epoch()!=0;}
    std::uint64_t generation()noexcept {PreserveErrno saved;return coverage_.epoch();}
    Address original(unsigned i)const noexcept{return i<2?originals_[i].load(std::memory_order_acquire):0;}
    Address image()const noexcept{return image_;}
    bool fault()const noexcept{return fault_.load(std::memory_order_acquire);}
    bool caller(Address relative)const noexcept {
        for(const auto lr:opaque_run_generated::InvocationReturns)if(relative==lr)return true;
        return false;
    }
private:
    struct PreserveErrno{int value{errno};~PreserveErrno(){errno=value;}};
    NativeOpaqueRunCoverage& coverage_;std::mutex mutex_;Api api_{};
    Address image_{};std::array<Address,2> wrappers_{};std::array<std::atomic<Address>,2> originals_{};
    std::atomic<bool> fault_{};unsigned ownedMask_{};bool configured_{};
    static bool currentOwned(void* p)noexcept {
        auto& s=*static_cast<NativeOpaqueRunHookState*>(p);
        if(s.fault_.load(std::memory_order_acquire))return false;
        for(unsigned i=0;i<2;++i){Address actual{};
            if(!s.api_.loadSlot(s.api_.context,s.image_+opaque_run_generated::Slots[i],actual)||actual!=s.wrappers_[i])return false;
        }
        return s.api_.dependencyCurrent(s.api_.context);
    }
    bool verifyCode()const noexcept {
        std::array<std::uint8_t,64> bytes{};
        for(const auto& pin:opaque_run_generated::Pins){
            if(!api_.range(api_.context,image_+pin.offset,pin.count,5))return false;
            for(std::size_t at=0;at<pin.count;at+=bytes.size()){
                const auto n=std::min(bytes.size(),pin.count-at);
                if(!api_.read(api_.context,image_+pin.offset+at,bytes.data(),n)||std::memcmp(bytes.data(),pin.bytes+at,n))return false;
            }
        }
        for(const auto offset:opaque_run_generated::Slots)
            if(!api_.range(api_.context,image_+offset,sizeof(Address),4))return false;
        return true;
    }
    bool rollback()noexcept {
        bool ok=true;
        for(unsigned i=2;i--;)if(ownedMask_&(1u<<i)){
            if(api_.replaceSlot(api_.context,image_+opaque_run_generated::Slots[i],wrappers_[i],original(i)))ownedMask_&=~(1u<<i);
            else ok=false;
        }
        if(!ok)fault_.store(true,std::memory_order_release);
        return ok;
    }
};
}
