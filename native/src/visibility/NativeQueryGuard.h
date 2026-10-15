#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>

namespace rdrvr::experimental {
// PRIVATE observer of actual Vulkan command/query lifecycle. No Vulkan calls.
// Native pending-query count alone is insufficient after vkCmdBeginQuery was
// emitted. Unknown/secondary/inherited or malformed streams stay ineligible.
// Mutators are infrequent command/query events. quiet/current are bounded,
// mutex-free and allocation-free, and need the caller's native command lease.
class NativeQueryGuard {
public:
    using Handle=std::uint64_t;
    static constexpr unsigned Capacity=2048,MaxProbe=32,MaxActive=16;
    struct Ticket {std::uintptr_t owner{};unsigned slot{};std::uint64_t revision{};Handle command{},device{};};
    // Allocation is a fresh native lifetime, even when a handle was reused.
    bool allocated(Handle device,Handle pool,Handle command,bool primary)noexcept {
        if(!device||!pool||!command)return false;
        std::lock_guard lock(mutex_);auto* s=find(command,true);if(!s)return false;
        Mutation change(*s);s->device.store(device);s->pool.store(pool);s->live=true;s->primary=primary;
        s->recording=false;s->fault=false;s->count=0;return true;
    }
    void begun(Handle command,bool successful,bool noInheritanceOrUnknownExtensions)noexcept {
        std::lock_guard lock(mutex_);if(auto* s=find(command,false)){
            Mutation change(*s);s->count=0;s->fault=!successful||!noInheritanceOrUnknownExtensions||!s->primary;
            s->recording=successful&&s->live;s->publish();
        }
    }
    void ended(Handle command)noexcept {invalidate(command,false);}
    void reset(Handle command)noexcept {invalidate(command,false);}
    void freed(Handle device,Handle pool,Handle command)noexcept {
        std::lock_guard lock(mutex_);if(auto* s=find(command,false))if(s->device.load()==device&&s->pool.load()==pool){
            Mutation change(*s);s->live=false;s->recording=false;s->count=0;
        }
    }
    // Invoke BEFORE native pool reset/free/device destruction regardless of
    // result. A successful command begin is the only recording recovery.
    void poolInvalidated(Handle device,Handle pool,bool destroyed)noexcept {
        std::lock_guard lock(mutex_);for(auto& s:slots_)if(s.handle.load()&&s.device.load()==device&&s.pool.load()==pool){
            Mutation change(s);s.recording=false;s.count=0;if(destroyed)s.live=false;
        }
    }
    void deviceDestroyed(Handle device)noexcept {
        std::lock_guard lock(mutex_);for(auto& s:slots_)if(s.handle.load()&&s.device.load()==device){
            Mutation change(s);s.live=false;s.recording=false;s.count=0;
        }
    }
    void invalidateAllRecordings()noexcept {
        std::lock_guard lock(mutex_);for(auto& s:slots_)if(s.handle.load()){
            Mutation change(s);s.recording=false;s.count=0;s.fault=true;
        }
    }
    void beginQuery(Handle command,Handle pool,unsigned index)noexcept {
        std::lock_guard lock(mutex_);if(auto* s=find(command,false)){
            Mutation change(*s);
            if(!s->live||!s->recording||s->fault||!pool||s->count==MaxActive){s->fault=true;return;}
            for(unsigned i=0;i<s->count;++i)if(s->active[i]==Query{pool,index}){s->fault=true;return;}
            s->active[s->count++]={pool,index};s->publish();
        }
    }
    void endQuery(Handle command,Handle pool,unsigned index)noexcept {
        std::lock_guard lock(mutex_);if(auto* s=find(command,false)){
            Mutation change(*s);
            if(!s->live||!s->recording||s->fault||!pool){s->fault=true;return;}
            for(unsigned i=0;i<s->count;++i)if(s->active[i]==Query{pool,index}){
                s->active[i]=s->active[--s->count];s->publish();return;
            }
            s->fault=true; // Wrong pool/index cannot decrement someone else's query.
        }
    }
    // An untracked secondary/extension can contain query activity. Do not
    // infer a clean state after vkCmdExecuteCommands or partial interception.
    void commandUnknown(Handle command)noexcept {invalidate(command,true);}
    bool quiet(Handle device,Handle command,Ticket& out)const noexcept {
        out={};if(!device||!command)return false;const auto* s=findRead(command);if(!s)return false;
        const auto revision=s->revision.load(std::memory_order_seq_cst);
        if(!revision||(revision&1)||s->device.load(std::memory_order_acquire)!=device||!s->quiet.load(std::memory_order_acquire))return false;
        Ticket value{reinterpret_cast<std::uintptr_t>(this),unsigned(s-slots_.data()),revision,command,device};
        if(!current(value))return false;out=value;return true;
    }
    bool current(const Ticket& ticket)const noexcept {
        if(ticket.owner!=reinterpret_cast<std::uintptr_t>(this)||ticket.slot>=Capacity||!ticket.revision||(ticket.revision&1)||!ticket.device||!ticket.command)return false;
        const auto& s=slots_[ticket.slot];
        return s.revision.load(std::memory_order_seq_cst)==ticket.revision&&
            s.handle.load(std::memory_order_acquire)==ticket.command&&s.device.load(std::memory_order_acquire)==ticket.device&&
            s.quiet.load(std::memory_order_acquire)&&s.revision.load(std::memory_order_seq_cst)==ticket.revision;
    }
private:
    struct Query {Handle pool{};unsigned index{};bool operator==(const Query&)const=default;};
    struct Slot {
        std::atomic<Handle> handle{},device{},pool{};std::atomic<std::uint64_t> revision{};std::atomic<bool> quiet{};
        // Only cold/event writers under mutex access these fields.
        std::array<Query,MaxActive> active{};unsigned count{};bool live{},primary{},recording{},fault{};
        void publish()noexcept{quiet.store(live&&primary&&recording&&!fault&&!count,std::memory_order_release);}
    };
    struct Mutation {
        Slot& slot;std::uint64_t before;
        explicit Mutation(Slot& s)noexcept:slot(s),before(s.revision.load(std::memory_order_seq_cst)){
            constexpr auto max=std::numeric_limits<std::uint64_t>::max();
            if(before>=max-2){before=max;s.revision.store(max,std::memory_order_seq_cst);}
            else s.revision.store(before+1,std::memory_order_seq_cst);
            s.quiet.store(false,std::memory_order_release);
        }
        ~Mutation(){if(before!=std::numeric_limits<std::uint64_t>::max())slot.revision.store(before+2,std::memory_order_seq_cst);}
    };
    std::array<Slot,Capacity> slots_{};std::mutex mutex_;
    static unsigned hash(Handle h)noexcept {h^=h>>33;h*=0xff51afd7ed558ccdull;h^=h>>33;return unsigned(h)&(Capacity-1);}
    Slot* find(Handle command,bool insert)noexcept {
        if(!command)return nullptr;auto at=hash(command);
        for(unsigned i=0;i<MaxProbe;++i,at=(at+1)&(Capacity-1)){
            auto& s=slots_[at];const auto h=s.handle.load(std::memory_order_relaxed);
            if(h==command)return &s;if(!h){if(insert){s.handle.store(command,std::memory_order_release);return &s;}return nullptr;}
        }return nullptr;
    }
    const Slot* findRead(Handle command)const noexcept {
        auto at=hash(command);for(unsigned i=0;i<MaxProbe;++i,at=(at+1)&(Capacity-1)){
            const auto& s=slots_[at];const auto h=s.handle.load(std::memory_order_acquire);if(h==command)return &s;if(!h)return nullptr;
        }return nullptr;
    }
    void invalidate(Handle command,bool unknown)noexcept {
        std::lock_guard lock(mutex_);if(auto* s=find(command,false)){Mutation change(*s);if(unknown)s->fault=true;else{s->recording=false;s->count=0;}}
    }
};
}
