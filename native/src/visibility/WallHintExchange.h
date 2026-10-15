#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>

namespace rdrvr::experimental {
// PRIVATE producer/consumer transfer for spatial-selection HINTS ONLY.
// No matrices, depth, visible/hidden bits, native pointer leases or draw permits
// cross this boundary. The render owner never waits for the selector worker.
class WallHintExchange {
public:
    static constexpr unsigned Capacity=32;
    struct Hint {
        std::uint64_t object{},objectGeneration{},geometry{},geometryGeneration{};
        std::uint32_t patch{};
        bool operator==(const Hint&)const=default;
        bool valid()const noexcept{return object&&objectGeneration&&geometry&&geometryGeneration;}
    };
    struct Packet {
        std::uint64_t scene{},request{};
        std::array<Hint,Capacity> hints{};
        unsigned count{};
        bool operator==(const Packet&)const=default;
    };
    // Stable WORLD/level incarnation, not the per-frame StereoCameraPlan sceneId.
    // Camera movement increments request; it does not discard useful old hints.
    // Every hint is still matched against current object/geometry generations.
    void scene(std::uint64_t value)noexcept {scene_.store(value,std::memory_order_release);}
    bool publish(std::uint64_t scene,std::uint64_t request,std::span<const Hint> hints)noexcept {
        if(!scene||!request||hints.size()>Capacity||scene_.load(std::memory_order_acquire)!=scene)return false;
        for(unsigned i=0;i<hints.size();++i){
            if(!hints[i].valid())return false;
            for(unsigned j=0;j<i;++j)if(hints[j]==hints[i])return false;
        }
        // Worker may wait for the bounded POD copy by the consumer. The
        // consumer only uses try_lock and never invokes this publication path.
        std::lock_guard lock(mutex_);
        if(scene_.load(std::memory_order_acquire)!=scene||
           (packet_.scene==scene&&request<=packet_.request))return false;
        packet_.scene=scene;packet_.request=request;packet_.count=unsigned(hints.size());
        for(unsigned i=0;i<hints.size();++i)packet_.hints[i]=hints[i];
        for(unsigned i=unsigned(hints.size());i<Capacity;++i)packet_.hints[i]={};
        return scene_.load(std::memory_order_acquire)==scene;
    }
    bool tryRead(std::uint64_t scene,std::uint64_t lastRequest,Packet& output)noexcept {
        if(!scene||scene_.load(std::memory_order_acquire)!=scene)return false;
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock()||packet_.scene!=scene||packet_.request<=lastRequest||
           scene_.load(std::memory_order_acquire)!=scene)return false;
        output=packet_;return true;
    }
private:
    std::atomic<std::uint64_t> scene_{};std::mutex mutex_;Packet packet_{};
};
}
