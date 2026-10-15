#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace rdrvr::experimental {
// Ownership of exactly the twelve verified retail geometry
// hook words. This is NOT a native object, geometry, shader or resource lease.
// The caller must also hold the real ResourceRead and image/code proof.
// Cold bind/publish/rearm are serialized externally. The image/GOT storage and
// proof context outlive this object and all readers; disarm before unmapping.
// Installation/removal must disarm before changing words. Observed ownership
// loss is latched; restoring a word alone never re-enables this helper.
class NativeEarlyColdOwners {
public:
    using Address=std::uintptr_t;
    static constexpr std::size_t Count=12;
    // Exact order from TraceEntry's complete twelve-slot transaction. Some are
    // vtable words, not GOT words; all are fixed mapped retail image storage.
    inline static constexpr std::array<Address,Count> SlotOffsets{
        0x2cdf678,0x2cdf7b8,0x2cdf7c0,0x2e16870,0x2cbede0,
        0x2cdf688,0x2e22900,0x2e22938,0x2cdf490,0x2e24b00,0x2cdf680,0x2cdf7a8};
    inline static constexpr std::array<Address,Count> OriginalOffsets{
        0x1b1d070,0x1b2303c,0x1b2310c,0x18b1ab8,0x18b1ab8,
        0x1b1d178,0x1b15be4,0x1b19110,0x1b19110,0x2947c7c,0x1b1d118,0x1b230b8};
    struct ImageProof {
        void* context{};
        bool(*read)(void*,Address,void*,std::size_t)noexcept{};
        bool(*range)(void*,Address,std::size_t,unsigned elfFlags)noexcept{};
    };
    struct Slot {Address address{},wrapper{};};
    class Observation {
        friend class NativeEarlyColdOwners;
        const NativeEarlyColdOwners* owner_{};std::uint64_t epoch_{};
    public:
        bool current()const noexcept {return owner_&&epoch_&&(epoch_&1)&&owner_->state_.load(std::memory_order_acquire)==epoch_;}
    };
    // Scalar observation only. The mod's own installation/removal writes already
    // disarm before touching a slot. It cannot discover an unobserved third-
    // party overwrite. Reissue only after the full exact words check below.
    Observation observe()noexcept {Observation o;const auto e=generation();if(e){o.owner_=this;o.epoch_=e;}return o;}
    NativeEarlyColdOwners()noexcept=default;
    NativeEarlyColdOwners(const NativeEarlyColdOwners&)=delete;
    NativeEarlyColdOwners& operator=(const NativeEarlyColdOwners&)=delete;

    bool bind(Address image,ImageProof proof)noexcept {
        if(bound_||!image||!proof.read||!proof.range||
           image>UINTPTR_MAX-0x2e24b08||image%alignof(Address))return false;
        for(const auto offset:SlotOffsets)
            if(!proof.range(proof.context,image+offset,sizeof(Address),4))return false;
        image_=image;proof_=proof;bound_=true;return true;
    }
    // Called only AFTER the full transaction's exact code/data pins and twelve
    // successful CAS installs. Values are immutable after this one publish.
    bool publish(const std::array<Slot,Count>& slots)noexcept {
        if(!bound_||published_)return false;
        disarm();const auto before=state_.load(std::memory_order_acquire);
        for(std::size_t i=0;i<Count;++i){
            if(slots[i].address!=image_+SlotOffsets[i]||!slots[i].wrapper||
               slots[i].wrapper==image_+OriginalOffsets[i])return false;
            Address value{};
            if(!proof_.range(proof_.context,slots[i].address,sizeof(Address),4)||
               !proof_.read(proof_.context,slots[i].address,&value,sizeof value)||
               value!=slots[i].wrapper)return false;
        }
        slots_=slots;published_=true;
        return arm(before);
    }
    // Revalidates the same once-published words; never changes pointer storage.
    bool rearm()noexcept {
        disarm();const auto before=state_.load(std::memory_order_acquire);
        if(!bound_||!published_)return false;
        for(const auto& slot:slots_){Address value{};
            if(!proof_.range(proof_.context,slot.address,sizeof(Address),4)||
               !proof_.read(proof_.context,slot.address,&value,sizeof value)||value!=slot.wrapper)return false;
        }
        return arm(before);
    }
    void disarm()noexcept {
        auto old=state_.load(std::memory_order_acquire);
        for(;;){const auto next=old>=End-2?End:((old+2)&~std::uint64_t{1});
            if(state_.compare_exchange_weak(old,next,std::memory_order_acq_rel,std::memory_order_acquire))return;
        }
    }
    bool current()noexcept{return generation()!=0;}
    bool current(std::uint64_t saved)noexcept{return saved&&(saved&1)&&generation()==saved;}
    std::uint64_t generation()noexcept {
        const auto before=state_.load(std::memory_order_acquire);
        if(!(before&1))return 0;
        if(!wordsCurrent()){disarm();return 0;}
        return state_.load(std::memory_order_acquire)==before?before:0;
    }
private:
    static constexpr auto End=std::numeric_limits<std::uint64_t>::max()-1;
    static_assert(sizeof(Address)==8&&std::atomic<Address>::is_always_lock_free);
    std::atomic<std::uint64_t> state_{};
    ImageProof proof_{};Address image_{};std::array<Slot,Count> slots_{};
    bool bound_{},published_{};
    static Address loadWord(Address address)noexcept {
#if defined(__clang__) || defined(__GNUC__)
        return __atomic_load_n(reinterpret_cast<const Address*>(address),__ATOMIC_ACQUIRE);
#else
        return std::atomic_ref<Address>(*reinterpret_cast<Address*>(address)).load(std::memory_order_acquire);
#endif
    }
    bool wordsCurrent()const noexcept {
        for(const auto& slot:slots_)
            if(loadWord(slot.address)!=slot.wrapper)return false;
        return true;
    }
    bool arm(std::uint64_t expected)noexcept {
        if((expected&1)||expected>=End-2||!wordsCurrent())return false;
        return state_.compare_exchange_strong(expected,(expected+2)|1,
            std::memory_order_acq_rel,std::memory_order_acquire);
    }
    friend struct NativeEarlyColdOwnersTestAccess;
};
inline NativeEarlyColdOwners nativeEarlyColdOwners;
}
