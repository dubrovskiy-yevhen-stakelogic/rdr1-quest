#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace rdrvr::experimental {
// One bounded publication attempt per prepared frame. Every payload cell is
// release/acquire: observing any newer cell forces the final sequence read to
// observe that writer's odd sequence or a later sequence, hence reject mixing.
template<std::size_t N> class EarlyWork45Publication {
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::atomic<std::uint64_t> sequence_{},scene_{},pose_{},sources_{},permitted_{};
    std::array<std::atomic<std::uint64_t>,4> skipped_{};
    std::array<std::atomic<std::uint64_t>,N> values_{};
public:
    struct Packet {
        std::array<std::uint64_t,N> values{};
        std::array<std::uint64_t,4> skipped{};
        std::uint64_t scene{},pose{},sources{},permitted{},sequenceBefore{},sequenceAfter{};
        bool stable{};
    };
    bool publishPrepared(bool prepared,std::uint64_t scene,std::uint64_t pose,
                         const std::uint64_t (&values)[N],
                         const std::array<unsigned,4>& skipped,
                         unsigned sources,unsigned permitted)noexcept {
        if(!prepared||!scene||!pose)return false;
        auto sequence=sequence_.load(std::memory_order_acquire);
        if((sequence&1)||sequence>std::numeric_limits<std::uint64_t>::max()-2)return false;
        if(!sequence_.compare_exchange_strong(sequence,sequence+1,
                std::memory_order_acq_rel,std::memory_order_acquire))return false;
        scene_.store(scene,std::memory_order_release);pose_.store(pose,std::memory_order_release);
        sources_.store(sources,std::memory_order_release);permitted_.store(permitted,std::memory_order_release);
        for(std::size_t i=0;i<4;++i)skipped_[i].store(skipped[i],std::memory_order_release);
        for(std::size_t i=0;i<N;++i)values_[i].store(values[i],std::memory_order_release);
        sequence_.store(sequence+2,std::memory_order_release);
        return true;
    }
    Packet read()const noexcept {
        Packet packet;
        packet.sequenceBefore=sequence_.load(std::memory_order_acquire);
        if(!packet.sequenceBefore||(packet.sequenceBefore&1))return packet;
        packet.scene=scene_.load(std::memory_order_acquire);packet.pose=pose_.load(std::memory_order_acquire);
        packet.sources=sources_.load(std::memory_order_acquire);packet.permitted=permitted_.load(std::memory_order_acquire);
        for(std::size_t i=0;i<4;++i)packet.skipped[i]=skipped_[i].load(std::memory_order_acquire);
        for(std::size_t i=0;i<N;++i)packet.values[i]=values_[i].load(std::memory_order_acquire);
        packet.sequenceAfter=sequence_.load(std::memory_order_acquire);
        packet.stable=packet.sequenceBefore==packet.sequenceAfter&&packet.scene&&packet.pose;
        return packet;
    }
};
}
