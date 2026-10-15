#pragma once
#include "engine/PassResources.h"
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <mutex>
#include <thread>

namespace rdrvr::experimental {
// PRIVATE companion for the EXACT PassResources instance. Install complete
// mutation coverage before arming. Every bind/created/destroyed/clear call,
// including unsupported replacement and exceptional exits, must be enclosed
// in Mutation. This is metadata revocation, NOT a native/GPU resource lease.
class NativeEarlyImageRegistry {
public:
    using Registry=engine::PassResources;
    using Record=engine::ImageRecord;
    class Cache2;
    class Ticket {
        friend class NativeEarlyImageRegistry;
        friend class Cache2;
        const NativeEarlyImageRegistry* owner_{};
        const Registry* registry_{};
        std::uint64_t revision_{};
    public:
        Ticket()=default;
        // Issued identity, not freshness: an expired issued ticket remains
        // truthy so optional legacy checks cannot turn expiry into "absent".
        explicit operator bool()const noexcept{return owner_&&registry_&&revision_&&!(revision_&1);}
        bool current()const noexcept;
    };
    // A smaller limit is useful for exhaustion tests. It can only shorten the
    // lifetime of this companion; there is no API to reset a poisoned epoch.
    explicit NativeEarlyImageRegistry(Registry& registry,
        std::uint64_t maximumRevision=UINT64_MAX-1)noexcept:
        registry_(&registry),maximum_(maximumRevision&~std::uint64_t(1)) {}
    NativeEarlyImageRegistry(const NativeEarlyImageRegistry&)=delete;
    NativeEarlyImageRegistry& operator=(const NativeEarlyImageRegistry&)=delete;
    // Caller owns teardown: companion/registry outlive all Mutations, caches
    // and synchronous tickets. Never rebind this object to another registry.
    bool setCoverage(bool complete)noexcept {
        try {
            std::lock_guard lock(mutex_);advance();enabled_=complete;
            publish();return enabled_&&!poisoned_&&!faulted_.load(std::memory_order_acquire);
        }catch(...){fault();return false;}
    }
    Ticket issue(const Registry& registry)const noexcept {
        Ticket result;if(&registry!=registry_)return result;
        const auto revision=published_.load(std::memory_order_acquire);
        if(!revision||(revision&1)||faulted_.load(std::memory_order_acquire))return result;
        result.owner_=this;result.registry_=registry_;result.revision_=revision;
        return result;
    }
    bool current(const Registry& registry,const Ticket& ticket)const noexcept {
        return &registry==registry_&&ticket.owner_==this&&ticket.registry_==registry_&&
            ticket.revision_&&!(ticket.revision_&1)&&
            published_.load(std::memory_order_acquire)==ticket.revision_&&!faulted_.load(std::memory_order_acquire);
    }
    class Mutation {
        NativeEarlyImageRegistry* owner_{};
    public:
        explicit Mutation(NativeEarlyImageRegistry& owner)noexcept:owner_(&owner){const int saved=errno;owner.enter();errno=saved;}
        ~Mutation(){const int saved=errno;owner_->leave();errno=saved;}
        Mutation(const Mutation&)=delete;Mutation& operator=(const Mutation&)=delete;
    };
    // Per render owner / eye / pass. Only copied creation metadata is cached;
    // target/texture/data pointer graphs and actual layout remain fresh reads.
    class Cache2 {
        struct Entry {VkDevice device{};VkImage image{};std::uint64_t revision{};Record record{};};
        NativeEarlyImageRegistry* owner_{};
        std::thread::id thread_{std::this_thread::get_id()};
        std::array<Entry,2> entries_{};unsigned replace_{};
    public:
        struct Statistics {std::uint64_t hits{},misses{},declines{};};
        explicit Cache2(NativeEarlyImageRegistry& owner)noexcept:owner_(&owner){}
        Cache2(const Cache2&)=delete;Cache2& operator=(const Cache2&)=delete;
        Ticket issue(const Registry& registry)const noexcept {
            return thread_==std::this_thread::get_id()?owner_->issue(registry):Ticket{};
        }
        bool current(const Registry& registry,const Ticket& ticket)const noexcept {
            return owner_->current(registry,ticket);
        }
        bool lookup(Registry& registry,VkDevice device,VkImage image,Record& out,const Ticket& ticket)noexcept {
            const int saved=errno;struct Restore{int value;~Restore(){errno=value;}} restore{saved};
            out={};
            // Foreign threads decline before touching mutable cache storage.
            if(thread_!=std::this_thread::get_id())return false;
            if(!device||!image||!owner_->current(registry,ticket)){++stats_.declines;return false;}
            for(const auto& entry:entries_)
                if(entry.revision==ticket.revision_&&entry.device==device&&entry.image==image){
                    out=entry.record;
                    if(!owner_->current(registry,ticket)){out={};++stats_.declines;return false;}
                    ++stats_.hits;return true;
                }
            ++stats_.misses;Record record{};
            try {
                if(!registry.lookup(device,image,record)||record.device!=device||record.image!=image||
                   !record.generation||!owner_->current(registry,ticket)){++stats_.declines;return false;}
            }catch(...){++stats_.declines;return false;}
            entries_[replace_]={device,image,ticket.revision_,record};replace_^=1;
            out=record;
            if(!owner_->current(registry,ticket)){out={};++stats_.declines;return false;}
            return true;
        }
        static bool lookupCallback(void* context,Registry& registry,VkDevice device,VkImage image,
                                   Record& out,const Ticket& ticket)noexcept {
            if(!context){out={};return false;}
            return static_cast<Cache2*>(context)->lookup(registry,device,image,out,ticket);
        }
        // Same-thread statistics only; never used to grant a metadata borrow.
        Statistics statistics()const noexcept{return thread_==std::this_thread::get_id()?stats_:Statistics{};}
    private:Statistics stats_{};
    };
private:
    Registry* const registry_;
    const std::uint64_t maximum_;
    std::mutex mutex_; // cold mutators only; no cache-hit/issue/current lock
    std::atomic<std::uint64_t> published_{};
    std::atomic<bool> faulted_{};
    std::uint64_t revision_{},writers_{};
    bool enabled_{},poisoned_{};
    void fault()noexcept{faulted_.store(true,std::memory_order_release);published_.store(0,std::memory_order_release);}
    void advance()noexcept {
        if(poisoned_||revision_>maximum_||maximum_-revision_<2){poisoned_=true;return;}
        revision_+=2;
    }
    void publish()noexcept {
        published_.store(enabled_&&!poisoned_&&!faulted_.load(std::memory_order_acquire)?
            revision_|std::uint64_t(writers_!=0):0,std::memory_order_release);
    }
    void enter()noexcept {
        try {
            std::lock_guard lock(mutex_);advance();
            if(writers_==UINT64_MAX)poisoned_=true;else ++writers_;
            publish();
        }catch(...){fault();}
    }
    void leave()noexcept {
        try {
            std::lock_guard lock(mutex_);
            if(!writers_)poisoned_=true;else --writers_;
            publish();
        }catch(...){fault();}
    }
};
inline bool NativeEarlyImageRegistry::Ticket::current()const noexcept {
    return owner_&&registry_&&owner_->current(*registry_,*this);
}
}
