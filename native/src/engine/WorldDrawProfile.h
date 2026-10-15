#pragma once
#include "diagnostics/DrawStageTimings.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
namespace rdrvr::engine {
// Initial world profile: draw buckets 0..3 and 5. Water (4), distortion (6)
// and the unverified final bucket (7) are excluded in eye replays because their
// callbacks bind stock postprocess targets. The normal game draw is untouched.
// A supplied main request enables bucket 4 only for a separately guarded water
// color stage. Reflection requests remain ineligible even if their index is 13.
class WorldDrawProfile {
public:
    using Original=void(*)(void*,void*,const void*,unsigned,unsigned,unsigned,bool,bool,unsigned);
    using Reader=bool(*)(void*,std::uintptr_t,void*,std::size_t);
private:
    struct State {WorldDrawProfile* owner{};bool failed{};unsigned calls{},filtered{};const void* waterRequest{};bool plain{};bool water{};};
    inline static thread_local State* active_{};
    Original original_{};Reader read_{};
    // cpu02: request+0xcc and *buckets are synchronous arguments of the wrapped Draw (the engine dereferences request itself),
    // so they are copied directly instead of one process_vm_readv each (2 per Draw). debug.rdrvr.profile.plain 1 = plain reads; default
    // guarded (0): plain reads would fault on a bad pointer where the guarded read only marks the eye failed, for ~0.07 ms.
    // Latched per Eye scope; the property is read at Eye entry at most once per second, never per draw.
    std::atomic<bool> plain_{false};std::atomic<std::uint64_t> lastReadNs_{},propertyReads_{};
    std::atomic<std::uint64_t> eyes_{},plainEyes_{},failedEyes_{},calls_{},filtered_{}; // cpu02: identity counters (summed at Eye exit)
    static bool readPlainProperty()noexcept{
#ifdef __ANDROID__
        const int saved=errno;char v[PROP_VALUE_MAX]{};const bool on=__system_property_get("debug.rdrvr.profile.plain",v)>0&&v[0]=='1'&&!v[1];errno=saved;return on;
#else
        return true;
#endif
    }
    void refresh()noexcept{
        const auto t=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        auto last=lastReadNs_.load(std::memory_order_relaxed);
        if((last&&t-last<1000000000ull)||!lastReadNs_.compare_exchange_strong(last,t,std::memory_order_relaxed))return;
        plain_.store(readPlainProperty(),std::memory_order_relaxed);propertyReads_.fetch_add(1,std::memory_order_relaxed);
    }
    bool field(const State& s,const void* p,std::uintptr_t offset,std::uint32_t* out)const{
        if(s.plain){std::memcpy(out,static_cast<const unsigned char*>(p)+offset,4);return true;}
        return read_(nullptr,reinterpret_cast<std::uintptr_t>(p)+offset,out,4);
    }
public:
    class Eye {
        State state_;State* previous_{};
    public:
        // w01 WATER: water=true keeps bucket 4 (water) for any index-13 request, as waterRequest does for its one request;
        // distortion (6) and 7 stay filtered.
        explicit Eye(WorldDrawProfile& owner,const void* waterRequest=nullptr,bool water=false):state_{&owner,false,0,0,waterRequest,false,water},previous_(active_){
            owner.refresh();state_.plain=owner.plain_.load(std::memory_order_relaxed);
            if(previous_){previous_->failed=true;state_.failed=true;}active_=&state_;
        }
        ~Eye(){active_=previous_;auto& o=*state_.owner;
            o.eyes_.fetch_add(1,std::memory_order_relaxed);if(state_.plain)o.plainEyes_.fetch_add(1,std::memory_order_relaxed);
            if(state_.failed)o.failedEyes_.fetch_add(1,std::memory_order_relaxed);
            o.calls_.fetch_add(state_.calls,std::memory_order_relaxed);o.filtered_.fetch_add(state_.filtered,std::memory_order_relaxed);}
        Eye(const Eye&)=delete;Eye& operator=(const Eye&)=delete;
        bool good()const{return !state_.failed;}
        unsigned calls()const{return state_.calls;}
    };
    void configure(Original original,Reader read){original_=original;read_=read;}
    void draw(void* container,void* request,const void* buckets,unsigned categories,unsigned pass,unsigned flags,bool a,bool b,unsigned tail){
        auto* s=active_;
        if(!s||s->owner!=this){diagnostics::DrawStageTimings::Bucket timing;original_(container,request,buckets,categories,pass,flags,a,b,tail);return;}
        if(s->failed)return;
        const int incoming=errno;std::uint32_t index{},mask=0xffffffff;
        try{
            if(!read_||!request||(s->waterRequest&&request!=s->waterRequest)||!field(*s,request,0xcc,&index)||index!=13||
               (buckets&&!field(*s,buckets,0,&mask))){s->failed=true;errno=incoming;return;}
            const auto selected=mask&((s->waterRequest||s->water)?0x3fu:0x2fu);++s->calls;if(selected!=mask)++s->filtered;
            errno=incoming;
            // A local bitset preserves the caller's request and callback table.
            // The ninth argument remains stack-passed in the ARM64 ABI.
            diagnostics::DrawStageTimings::Bucket timing;
            original_(container,request,&selected,categories,pass,flags,a,b,tail);
        }catch(...){s->failed=true;throw;}
    }
    // cpu02: plain 0/1 A/B must give the same calls and filtered per eye and failedEyes 0.
    std::string report()const{
        auto u=[](const std::atomic<std::uint64_t>& v){return std::to_string(v.load(std::memory_order_relaxed));};
        std::string o="{\"plain\":";o+=plain_.load(std::memory_order_relaxed)?"true":"false";
        o+=",\"propertyReads\":"+u(propertyReads_)+",\"eyes\":"+u(eyes_)+",\"plainEyes\":"+u(plainEyes_)+",\"failedEyes\":"+u(failedEyes_)+
            ",\"calls\":"+u(calls_)+",\"filtered\":"+u(filtered_)+'}';
        return o;
    }
};
}
