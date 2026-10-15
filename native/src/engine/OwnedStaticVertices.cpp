#include "OwnedStaticVertices.h"
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <mutex>
#include <utility>

namespace rdrvr::engine {
OwnedStaticVertices ownedStaticVertices;
namespace {
struct Errno { int value{errno};~Errno(){errno=value;} };
unsigned hash(std::uintptr_t value) noexcept {
    value^=value>>33;value*=0xff51afd7ed558ccdull;value^=value>>33;
    return unsigned(value)&(OwnedStaticVertices::TableCapacity-1);
}
template<class T>T field(const std::array<std::byte,16>& data,unsigned at) noexcept {
    T out{};std::memcpy(&out,data.data()+at,sizeof(out));return out;
}
bool valid(const OwnedStaticVertices::Identity& i) noexcept {
    const auto format=field<std::uint64_t>(i.fvfBytes,8)&15;
    return i.vertexBuffer&&i.nativeBuffer&&i.allocation&&i.fvf&&i.gpuBuffer&&
        i.vertexCount&&i.vertexCount<=65535&&i.stride<=255&&i.stride%4==0&&
        (field<std::uint32_t>(i.fvfBytes,0)&1)&&
        (format==6||format==7)&&i.stride>=(format==6?12u:16u)&&
        field<std::uint8_t>(i.fvfBytes,4)==i.stride&&
        std::uint64_t(i.vertexCount)*i.stride<=i.byteSize;
}
}
struct OwnedStaticVertices::Impl {
    struct Record {Identity identity{};std::unique_ptr<XYZ[]> positions;std::uint64_t generation{};};
    struct Slot {std::atomic<std::uintptr_t> key{};std::atomic<unsigned> record{};};
    static_assert(std::atomic<std::uintptr_t>::is_always_lock_free);
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    mutable std::shared_mutex mutex;
    std::array<Record,MaxEntries> records{};
    std::array<Slot,TableCapacity> table{};
    std::atomic<std::uint64_t> epoch{};
    std::atomic<std::uint64_t> invalidation{};
    std::uint64_t incarnation{},generation{},entries{},bytes{},captures{},invalidations{},evictions{},declined{};
    static constexpr unsigned Reserved=~0u;
    Slot* find(std::uintptr_t key) noexcept {
        auto at=hash(key);
        // Scan through vacant/reused slots: cold eviction may reuse tombstones.
        for(unsigned n=0;n<MaxProbe;++n,at=(at+1)&(TableCapacity-1))
            if(table[at].key.load(std::memory_order_acquire)==key)return &table[at];
        return nullptr;
    }
    Slot* reserve(std::uintptr_t key) noexcept {
        if(auto* s=find(key)){s->record.store(Reserved,std::memory_order_release);return s;}
        auto at=hash(key);
        for(unsigned n=0;n<MaxProbe;++n,at=(at+1)&(TableCapacity-1)){
            auto& s=table[at];if(s.record.load(std::memory_order_relaxed))continue;
            // Reservation is visible before its new key, so a concurrent
            // invalidation which sees that key waits for this capture.
            s.record.store(Reserved,std::memory_order_release);
            s.key.store(key,std::memory_order_release);return &s;
        }return nullptr;
    }
    void drop(unsigned index) noexcept {
        auto& r=records[index];if(!r.positions)return;
        if(auto* s=find(r.identity.nativeBuffer))
            if(s->record.load(std::memory_order_relaxed)==index+1)s->record.store(0,std::memory_order_release);
        bytes-=std::uint64_t(r.identity.vertexCount)*sizeof(XYZ);--entries;r={};
    }
    void erase(std::uintptr_t key) noexcept {
        if(auto* s=find(key)){const auto n=s->record.exchange(0,std::memory_order_acq_rel);
            if(n&&n!=Reserved)drop(n-1);}
    }
    unsigned oldest()const noexcept {
        unsigned out=MaxEntries;std::uint64_t sequence=UINT64_MAX;
        for(unsigned i=0;i<MaxEntries;++i)if(records[i].positions&&records[i].generation<sequence){out=i;sequence=records[i].generation;}
        return out;
    }
    void reset() noexcept {
        for(auto& s:table){s.record.store(0,std::memory_order_release);s.key.store(0,std::memory_order_release);}
        for(auto& r:records)r={};entries=bytes=0;
    }
};
OwnedStaticVertices::Lease::Lease(Lease&& other) noexcept { *this=std::move(other); }
OwnedStaticVertices::Lease& OwnedStaticVertices::Lease::operator=(Lease&& other) noexcept {
    if(this!=&other){reset();identity=other.identity;positions=other.positions;epoch=other.epoch;
        generation=other.generation;owner_=other.owner_;incarnation_=other.incarnation_;
        lock_=std::move(other.lock_);other.reset();}return *this;
}
void OwnedStaticVertices::Lease::reset() noexcept {
    positions={};identity={};epoch=generation=incarnation_=0;owner_=nullptr;
    lock_=std::shared_lock<std::shared_mutex>{};
}
OwnedStaticVertices::OwnedStaticVertices():impl_(std::make_unique<Impl>()){}
OwnedStaticVertices::~OwnedStaticVertices()=default;
bool OwnedStaticVertices::bind(std::uint64_t epoch) noexcept {
    Errno preserve;if(!epoch)return false;
    try {auto& s=*impl_;std::unique_lock lock(s.mutex);const auto current=s.epoch.load(std::memory_order_relaxed);
        if(current)return current==epoch;s.invalidation.fetch_add(1,std::memory_order_acq_rel);
        s.reset();++s.incarnation;s.epoch.store(epoch,std::memory_order_release);return true;
    }catch(...){return false;}
}
void OwnedStaticVertices::clear() noexcept {
    Errno preserve;auto& s=*impl_;s.invalidation.fetch_add(1,std::memory_order_acq_rel);
    try{std::unique_lock lock(s.mutex);s.epoch.store(0,std::memory_order_release);++s.incarnation;s.reset();}catch(...){}
}
void OwnedStaticVertices::clear(std::uint64_t expectedEpoch) noexcept {
    Errno preserve;try{auto& s=*impl_;std::unique_lock lock(s.mutex);
        if(!expectedEpoch||s.epoch.load(std::memory_order_relaxed)!=expectedEpoch)return;
        s.invalidation.fetch_add(1,std::memory_order_acq_rel);++s.incarnation;s.reset();}catch(...){}
}
bool OwnedStaticVertices::capture(std::uint64_t epoch,const Identity& id,const void* mapped,
        std::size_t availableBytes,bool qualified) noexcept {
    Errno preserve;Impl::Slot* reserved{};
    try {auto& s=*impl_;std::unique_lock lock(s.mutex);
        if(!epoch||s.epoch.load(std::memory_order_relaxed)!=epoch)return false;
        s.invalidation.fetch_add(1,std::memory_order_acq_rel);
        if(id.nativeBuffer)s.erase(id.nativeBuffer);
        const auto rawBytes=std::uint64_t(id.vertexCount)*id.stride;
        if(!qualified||!mapped||!valid(id)||rawBytes>availableBytes||
           reinterpret_cast<std::uintptr_t>(mapped)>UINTPTR_MAX-rawBytes){++s.declined;return false;}
        reserved=s.reserve(id.nativeBuffer);if(!reserved){++s.declined;return false;}
        const auto bytes=std::uint64_t(id.vertexCount)*sizeof(XYZ);
        while(s.entries==MaxEntries||s.bytes+bytes>MaxBytes){const auto victim=s.oldest();
            if(victim==MaxEntries){reserved->record.store(0,std::memory_order_release);++s.declined;return false;}
            s.drop(victim);++s.evictions;}
        std::unique_ptr<XYZ[]> positions;
        try{positions=std::make_unique<XYZ[]>(id.vertexCount);}
        catch(...){reserved->record.store(0,std::memory_order_release);++s.declined;return false;}
        const auto* source=static_cast<const std::byte*>(mapped);
        for(unsigned i=0;i<id.vertexCount;++i){std::memcpy(&positions[i],source+std::size_t(i)*id.stride,sizeof(XYZ));
            const auto& v=positions[i];if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z)){
                reserved->record.store(0,std::memory_order_release);++s.declined;return false;}}
        unsigned at{};while(at<MaxEntries&&s.records[at].positions)++at;
        s.records[at]={id,std::move(positions),++s.generation};++s.entries;s.bytes+=bytes;++s.captures;
        reserved->record.store(at+1,std::memory_order_release);return true;
    }catch(...){return false;}
}
void OwnedStaticVertices::invalidate(std::uintptr_t nativeBuffer) noexcept {
    Errno preserve;if(!nativeBuffer)return;auto& s=*impl_;
    if(!s.epoch.load(std::memory_order_acquire))return;
    auto* publication=s.find(nativeBuffer);
    if(!publication||!publication->record.load(std::memory_order_acquire))return;
    // Pending mutation invalidates a frame's already-built grid even while its
    // short source lease is still preventing the writer from entering native.
    s.invalidation.fetch_add(1,std::memory_order_acq_rel);
    try{std::unique_lock lock(s.mutex);s.erase(nativeBuffer);++s.invalidations;}catch(...){}
}
bool OwnedStaticVertices::query(std::uint64_t epoch,const Identity& identity,Lease& out)const noexcept {
    Errno preserve;out.reset();if(!epoch||!identity.nativeBuffer)return false;
    try{auto& s=*impl_;if(s.epoch.load(std::memory_order_acquire)!=epoch)return false;
        std::shared_lock lock(s.mutex,std::try_to_lock);if(!lock.owns_lock()||s.epoch.load(std::memory_order_relaxed)!=epoch)return false;
        auto* slot=s.find(identity.nativeBuffer);if(!slot)return false;
        const auto index=slot->record.load(std::memory_order_relaxed);if(!index||index==Impl::Reserved||index>MaxEntries)return false;
        const auto& r=s.records[index-1];if(!r.positions||r.identity!=identity)return false;
        out.identity=r.identity;out.positions={r.positions.get(),r.identity.vertexCount};out.epoch=epoch;
        out.generation=r.generation;out.incarnation_=s.incarnation;out.owner_=&s;out.lock_=std::move(lock);return true;
    }catch(...){out.reset();return false;}
}
bool OwnedStaticVertices::current(const Lease& lease)const noexcept {
    return lease.lock_.owns_lock()&&lease.owner_==impl_.get()&&
        lease.epoch==impl_->epoch.load(std::memory_order_relaxed)&&lease.incarnation_==impl_->incarnation;
}
std::uint64_t OwnedStaticVertices::invalidationSerial()const noexcept {
    return impl_->invalidation.load(std::memory_order_acquire);
}
OwnedStaticVertices::Statistics OwnedStaticVertices::statistics()const noexcept {
    Errno preserve;try{const auto& s=*impl_;std::shared_lock lock(s.mutex);
        return {s.epoch.load(std::memory_order_relaxed),s.entries,s.bytes,s.captures,s.invalidations,s.evictions,s.declined};
    }catch(...){return {};}
}
}
