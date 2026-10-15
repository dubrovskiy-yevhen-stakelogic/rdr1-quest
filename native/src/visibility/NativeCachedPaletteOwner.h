#pragma once
#include "NativeResourceGate.h"
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace rdr95 {
// Exact CURRENT BANK membership, not a generic matrix-set memory/pose lease.
// prepare is called once AFTER the original qualified MAIN wait and BEFORE any
// outer ResourceGate::Read. It locks native cache Crit, takes a short Read, copies
// only immutable entry metadata, releases Read, then unlocks native Crit.
// A true matrixSetCurrent is usable ONLY for +90 from a completed joined record.
// It does not certify an arbitrary address found in the pool or a pending writer.
class NativeCachedPaletteOwner {
public:
    static constexpr std::uint32_t PoolBytes=0xaa800, SlotBytes=64, Slots=PoolBytes/SlotBytes;
    using Address=std::uintptr_t;
    struct Api {
        void* context{};
        bool (*read)(void*,Address,void*,std::size_t)noexcept{};
        // Must bind the exact post-MAIN-wait scene interval ending before signal70
        // and revoke on scene/disarm/reinit. No engine calls inside this callback.
        bool (*frameCurrent)(void*,std::uint64_t)noexcept{};
        void (*lock)(void*){};
        void (*unlock)(void*){};
        NativeResourceGate* resources{};
        Address indexSlot{},banksSlot{},usedSlot{},countSlot{},poolLock{};
        bool codeProof{};
    };
    enum class Failure {None,Arguments,OuterRead,Owner,Read,Shape,Entry,Changed,Lock};
    NativeCachedPaletteOwner()=default;
    NativeCachedPaletteOwner(const NativeCachedPaletteOwner&)=delete;
    NativeCachedPaletteOwner&operator=(const NativeCachedPaletteOwner&)=delete;
    void reset()noexcept {valid_=false;frame_=resource_=0;base_=0;used_=entries_=0;failure_=Failure::None;}
    bool prepare(const Api& api,std::uint64_t frame)noexcept {
        reset();api_=api;thread_=std::this_thread::get_id();frame_=frame;
        if(!frame||!api.codeProof||!api.read||!api.frameCurrent||!api.lock||!api.unlock||!api.resources
            ||!api.indexSlot||!api.banksSlot||!api.usedSlot||!api.countSlot||!api.poolLock)return fail(Failure::Arguments);
        const auto before=api.resources->ticket();
        if(!before||!api.frameCurrent(api.context,frame))return fail(Failure::Owner);
        if(api.resources->borrowedOnThisThread(before))return fail(Failure::OuterRead);
        const int saved=errno;bool locked=false,good=false;
        try {
            api.lock(reinterpret_cast<void*>(api.poolLock));locked=true;
            {
                NativeResourceGate::Read borrow(*api.resources);
                if(!borrow||borrow.generation()!=before||!api.frameCurrent(api.context,frame))fail(Failure::Owner);
                else {resource_=before;good=scan();if(good&&(!borrow.current()||!api.frameCurrent(api.context,frame)))good=fail(Failure::Changed);}
            }
            locked=false;api.unlock(reinterpret_cast<void*>(api.poolLock));
        }catch(...){good=fail(Failure::Lock);if(locked){try{api.unlock(reinterpret_cast<void*>(api.poolLock));}catch(...){}}}
        errno=saved;
        valid_=good&&api.resources->frameRevisionCurrent(before)&&api.frameCurrent(api.context,frame);
        if(!valid_&&failure_==Failure::None)failure_=Failure::Changed;
        return valid_;
    }
    // No native reads, lock, allocation or pool scan. Copied-frame authority only.
    bool current()const noexcept {
        return valid_&&thread_==std::this_thread::get_id()&&api_.resources
            &&api_.resources->frameRevisionCurrent(resource_)&&api_.frameCurrent(api_.context,frame_);
    }
    bool matrixSetCurrent(Address body)const noexcept {
        if(!current()||!api_.resources->borrowedOnThisThread(resource_)||body<base_+SlotBytes)return false;
        const auto offset=body-base_-SlotBytes;
        if(offset>=used_||(offset&(SlotBytes-1)))return false;
        const auto slot=static_cast<std::uint32_t>(offset/SlotBytes);
        return (starts_[slot/64]&(std::uint64_t{1}<<(slot%64)))!=0;
    }
    Failure failure()const noexcept{return failure_;}
    std::uint32_t entries()const noexcept{return entries_;}
    std::uint64_t resourceGeneration()const noexcept{return current()?resource_:0;}
private:
    Api api_{};std::thread::id thread_{};std::uint64_t frame_{},resource_{};
    Address base_{};std::uint32_t used_{},entries_{};bool valid_{};Failure failure_{};
    std::array<std::uint64_t,(Slots+63)/64> starts_{};
    bool fail(Failure f)noexcept{valid_=false;failure_=f;return false;}
    template<class T>bool read(Address p,T& out)const noexcept {
        return p&&p<=UINTPTR_MAX-sizeof(T)&&api_.resources->borrowedOnThisThread(resource_)
            &&api_.read(api_.context,p,&out,sizeof(out));
    }
    bool scan()noexcept {
        std::uint32_t bank{},used{},count{};std::array<Address,3>bases{};
        if(!read(api_.indexSlot,bank)||!read(api_.banksSlot,bases)||!read(api_.usedSlot,used)||!read(api_.countSlot,count))return fail(Failure::Read);
        if(bank>=3||!bases[0]||(bases[0]&63)||bases[0]>UINTPTR_MAX-3*PoolBytes
            ||bases[1]!=bases[0]+PoolBytes||bases[2]!=bases[1]+PoolBytes||used>PoolBytes||(used&63)
            ||count>Slots/5||(count==0)!=(used==0))return fail(Failure::Shape);
        starts_.fill(0);const auto base=bases[bank];std::uint32_t offset=0;
        for(std::uint32_t i=0;i<count;++i){
            if(offset>=used||used-offset<SlotBytes+1)return fail(Failure::Entry);
            std::uint32_t flags{};std::uint8_t bones{};
            if(!read(base+offset+0x10,flags)||!read(base+offset+0x40,bones))return fail(Failure::Read);
            const auto stride=(flags&0x1ffu)*SlotBytes;
            // ComputeSize=0xe0+96*n; native entry stride=(ComputeSize+0x7f)&~63.
            const auto expected=(0xe0u+96u*bones+0x7fu)&~63u;
            if(stride!=expected||stride>used-offset)return fail(Failure::Entry);
            // Zero-bone entries are legal storage, not a drawable palette.
            if(bones)starts_[(offset/SlotBytes)/64]|=std::uint64_t{1}<<((offset/SlotBytes)%64);
            offset+=stride;
        }
        if(offset!=used)return fail(Failure::Entry);
        std::uint32_t bank2{},used2{},count2{};std::array<Address,3>bases2{};
        if(!read(api_.indexSlot,bank2)||!read(api_.banksSlot,bases2)||!read(api_.usedSlot,used2)||!read(api_.countSlot,count2))return fail(Failure::Read);
        if(bank!=bank2||bases!=bases2||used!=used2||count!=count2)return fail(Failure::Changed);
        base_=base;used_=used;entries_=count;return true;
    }
};
}
