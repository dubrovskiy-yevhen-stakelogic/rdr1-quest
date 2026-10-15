#pragma once
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sstream>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace rdrvr::engine {
// Memoize ONLY the pure native hash of the complete 148-byte RenderPassDesc.
// Content comparison includes every byte and the seed. No borrowed pointers,
// pipeline objects, camera state, or framebuffer lifetime escape this call.
class PipelinePassHashMemo {
public:
    using Original = std::uint32_t(*)(const void*,std::size_t,std::uint32_t);
    static constexpr std::size_t bytes=148;
    static_assert(bytes==18*8+4);
    // cpu02: key equality as 18 u64 + 1 u32 XOR/OR (same result as !memcmp over the 148 bytes, no libc call).
    static bool sameKey(const std::byte* a,const void* other)noexcept{
        const auto* b=static_cast<const std::byte*>(other);std::uint64_t d{};
        for(std::size_t i=0;i<144;i+=8){std::uint64_t x,y;std::memcpy(&x,a+i,8);std::memcpy(&y,b+i,8);d|=x^y;}
        std::uint32_t x,y;std::memcpy(&x,a+144,4);std::memcpy(&y,b+144,4);return !(d|(x^y));
    }
private:
    struct Entry {std::array<std::byte,bytes> data{};std::uint32_t seed{},hash{};bool valid{};};
    struct Local {const PipelinePassHashMemo* owner{};Original original{};std::array<Entry,4> entries{};unsigned next{},recent{};std::uint64_t hits{},misses{};};
    static thread_local Local local_;
    std::atomic<std::uint64_t> hits_{},misses_{};
    std::atomic<bool> installed_{};
    // cpu02: the first thread with a qualified call (the render thread) keeps its cache in owner_, found by comparing its thread
    // pointer (one mrs) instead of the dynamic TLS resolver; every other thread keeps local_. Entries are a pure function of
    // (bytes, seed) and each cache stays private to one live thread. debug.rdrvr.passmemo.fast 0 = thread_local + memcmp.
    Local owner_{};std::atomic<std::uintptr_t> ownerTp_{};
    std::atomic<bool> fast_{true};std::atomic<std::uint64_t> lastReadNs_{},propertyReads_{},ownerHits_{},ownerMisses_{};
    static std::uintptr_t threadPointer()noexcept{
#if defined(__aarch64__)
        return reinterpret_cast<std::uintptr_t>(__builtin_thread_pointer());
#else
        return 0; // host builds: no owner cache
#endif
    }
    Local& local(bool fast)noexcept{
        if(fast){const auto tp=threadPointer();auto o=ownerTp_.load(std::memory_order_relaxed);
            if(tp&&(o==tp||(!o&&ownerTp_.compare_exchange_strong(o,tp,std::memory_order_relaxed))))return owner_;}
        return local_;
    }
    static bool readFastProperty()noexcept{
#ifdef __ANDROID__
        const int saved=errno;char v[PROP_VALUE_MAX]{};const bool off=__system_property_get("debug.rdrvr.passmemo.fast",v)>0&&v[0]=='0'&&!v[1];errno=saved;return !off;
#else
        return true;
#endif
    }
    // cpu02: at install and at most once per second from a publish (every 1024 calls of one thread), never per call.
    void refresh(bool force)noexcept{
        const auto t=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        auto last=lastReadNs_.load(std::memory_order_relaxed);
        if((!force&&last&&t-last<1000000000ull)||!lastReadNs_.compare_exchange_strong(last,t,std::memory_order_relaxed))return;
        fast_.store(readFastProperty(),std::memory_order_relaxed);propertyReads_.fetch_add(1,std::memory_order_relaxed);
    }
    void publish(Local& s,bool force)noexcept{
        if(s.owner!=this||(!force&&s.hits+s.misses<1024))return;
        hits_.fetch_add(s.hits,std::memory_order_relaxed);misses_.fetch_add(s.misses,std::memory_order_relaxed);
        if(&s==&owner_){ownerHits_.fetch_add(s.hits,std::memory_order_relaxed);ownerMisses_.fetch_add(s.misses,std::memory_order_relaxed);}
        s.hits=s.misses=0;refresh(false);
    }
public:
    void installed(bool value)noexcept{refresh(true);installed_.store(value,std::memory_order_relaxed);}
    bool installed()const noexcept{return installed_.load(std::memory_order_relaxed);}
    std::uint32_t invoke(Original original,const void* data,std::size_t size,std::uint32_t seed,bool qualified){
        if(!qualified||!data||size!=bytes)return original(data,size,seed);
        const bool fast=fast_.load(std::memory_order_relaxed);
        auto& s=local(fast);
        if(s.owner!=this||s.original!=original){s={};s.owner=this;s.original=original;}
        auto matches=[&](const Entry& e){return e.valid&&e.seed==seed&&(fast?sameKey(e.data.data(),data):!std::memcmp(e.data.data(),data,bytes));};
        unsigned hit=s.recent;
        if(!matches(s.entries[hit])){
            hit=4;
            for(unsigned i=0;i<4;++i)if(i!=s.recent&&matches(s.entries[i])){hit=i;break;}
        }
        if(hit<4){s.recent=hit;++s.hits;publish(s,false);return s.entries[hit].hash;}
        // The pinned original is a pure byte hash. Invoke it on the exact
        // original arguments on misses; commit the entry only after success.
        const auto value=original(data,size,seed);
        const auto slot=s.next++%4;auto& e=s.entries[slot];
        std::memcpy(e.data.data(),data,bytes);e.seed=seed;e.hash=value;e.valid=true;
        s.recent=slot;++s.misses;publish(s,false);return value;
    }
    void publish(bool force=true)noexcept{
        const auto tp=threadPointer();publish(tp&&ownerTp_.load(std::memory_order_relaxed)==tp?owner_:local_,force);
    }
    std::uint64_t hits()const{return hits_.load(std::memory_order_relaxed);}
    std::uint64_t misses()const{return misses_.load(std::memory_order_relaxed);}
    std::string report()const{
        std::ostringstream o;o<<"{\"installed\":"<<(installed_.load()?"true":"false")
          <<",\"scope\":\"native-render-pass-descriptor-hash\",\"keyBytes\":148,\"capacityPerThread\":4"
          <<",\"hits\":"<<hits()<<",\"misses\":"<<misses()<<",\"unpublishedPerThreadMax\":1023"
          // cpu02: fast 0/1 A/B must keep the hit ratio; ownerHits/ownerMisses = calls served by the owner-thread cache
          <<",\"fast\":"<<(fast_.load(std::memory_order_relaxed)?"true":"false")<<",\"propertyReads\":"<<propertyReads_.load(std::memory_order_relaxed)
          <<",\"ownerClaimed\":"<<(ownerTp_.load(std::memory_order_relaxed)?"true":"false")
          <<",\"ownerHits\":"<<ownerHits_.load(std::memory_order_relaxed)<<",\"ownerMisses\":"<<ownerMisses_.load(std::memory_order_relaxed)<<'}';
        return o.str();
    }
};
inline thread_local PipelinePassHashMemo::Local PipelinePassHashMemo::local_{};
inline PipelinePassHashMemo pipelinePassHashMemo;
}
