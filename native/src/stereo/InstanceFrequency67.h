#pragma once
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

namespace rdrvr::experimental {
// Read-only, lexical Reset -> MAIN Clear -> StartFrame diagnostic. No native
// pointer or key is dereferenced after the original Reset lifetime ends.
class InstanceFrequency67 {
public:
    using Reader=bool(*)(void*,std::uintptr_t,void*,std::size_t);
    using Reset=void(*)(void*,bool);
    using Clear=int(*)(void*,bool);
    using Start=int(*)(void*,int);
    static constexpr unsigned beforeBytes=0x1640, keyCount=48;
private:
    struct State {InstanceFrequency67* owner{};void* container{};void* manager{};bool inClear{},inStart{},qualified{};};
    static thread_local State* active_;
    struct ActiveScope {State* previous;explicit ActiveScope(State& s):previous(active_){active_=&s;}~ActiveScope(){active_=previous;}};
    struct OriginalError {int value=errno;~OriginalError(){errno=value;}};
    template<class Fn> static decltype(auto) original(OriginalError& error,Fn fn){
        errno=error.value;
        try {
            if constexpr(std::is_void_v<decltype(fn())>){fn();error.value=errno;return;}
            else {auto result=fn();error.value=errno;return result;}
        }catch(...){error.value=errno;throw;}
    }
    Reader reader_{};void* context_{};Reset reset_{};Clear clear_{};Start start_{};
    std::array<std::atomic<std::uint64_t>,7> counters_{};
    std::atomic<std::uint64_t> sequence_{};
    std::array<std::atomic<std::uint64_t>,beforeBytes/8> before_{};
    std::array<std::atomic<std::uint64_t>,keyCount> selected_{};
    std::atomic<std::uint32_t> oldCount_{},newCount_{},threshold_{},returned_{};
    bool read(std::uintptr_t p,void* out,std::size_t size)noexcept{
        try{return reader_&&p&&size<=UINTPTR_MAX-p&&reader_(context_,p,out,size);}catch(...){return false;}
    }
    void publish(const std::array<std::byte,beforeBytes>& bytes,std::uint32_t oldCount,
                 const std::array<std::uint64_t,keyCount>& keys,std::uint32_t count,int threshold,int returned)noexcept{
        auto seq=sequence_.load(std::memory_order_relaxed);
        if(seq&1||seq>=UINT64_MAX-2||!sequence_.compare_exchange_strong(seq,seq+1,std::memory_order_acq_rel))return;
        for(unsigned i=0;i<before_.size();++i){std::uint64_t word{};std::memcpy(&word,bytes.data()+i*8,8);before_[i].store(word,std::memory_order_relaxed);}
        for(unsigned i=0;i<keys.size();++i)selected_[i].store(keys[i],std::memory_order_relaxed);
        oldCount_.store(oldCount,std::memory_order_relaxed);newCount_.store(count,std::memory_order_relaxed);
        threshold_.store(std::uint32_t(threshold),std::memory_order_relaxed);returned_.store(std::uint32_t(returned),std::memory_order_relaxed);
        sequence_.store(seq+2,std::memory_order_release);
        counters_[6].fetch_add(1,std::memory_order_relaxed);
    }
public:
    void configure(Reset reset,Clear clear,Start start,Reader reader,void* context=nullptr)noexcept{
        reset_=reset;clear_=clear;start_=start;reader_=reader;context_=context;
    }
    void reset(void* manager,bool flag,bool lease){
        OriginalError error;State state{this};
        const bool nested=active_!=nullptr;
        ActiveScope scope(state); // every nested/off reset blocks outer admission
        if(lease){
            counters_[0].fetch_add(1,std::memory_order_relaxed);
            if(nested)counters_[1].fetch_add(1,std::memory_order_relaxed);
            else {
                const auto p=reinterpret_cast<std::uintptr_t>(manager);
                if(p&&p<=UINTPTR_MAX-0x260&&read(p+0x258,&state.container,8)){
                    const auto c=reinterpret_cast<std::uintptr_t>(state.container);
                    state.qualified=c&&c<=UINTPTR_MAX-0xa8&&read(c+0xa0,&state.manager,8)&&state.manager;
                }
                if(state.qualified)counters_[2].fetch_add(1,std::memory_order_relaxed);
                else counters_[3].fetch_add(1,std::memory_order_relaxed);
            }
        }
        original(error,[&]{reset_(manager,flag);});
    }
    int clear(void* container,bool flag,bool caller){
        OriginalError error;auto* s=active_;
        if(!caller||!s||s->owner!=this||!s->qualified||s->inClear||!flag||container!=s->container){
            State barrier{};ActiveScope scope(barrier);
            return original(error,[&]{return clear_(container,flag);});
        }
        struct ClearScope {State& state;explicit ClearScope(State& s):state(s){state.inClear=true;}~ClearScope(){state.inClear=false;}} scope{*s};
        return original(error,[&]{return clear_(container,flag);});
    }
    int start(void* manager,int threshold,bool caller){
        OriginalError error;auto* s=active_;
        if(!caller||!s||s->owner!=this||!s->qualified||!s->inClear||s->inStart||manager!=s->manager)
            return original(error,[&]{return start_(manager,threshold);});
        struct StartScope {State& state;explicit StartScope(State& s):state(s){state.inStart=true;}~StartScope(){state.inStart=false;}} scope{*s};
        counters_[4].fetch_add(1,std::memory_order_relaxed);
        // Only diagnostic reads. Even a refused/corrupt snapshot forwards the
        // complete stock StartFrame exactly once and preserves its int result.
        const auto p=reinterpret_cast<std::uintptr_t>(manager);
        std::array<std::byte,beforeBytes> before{};
        std::uint32_t oldCount{};
        const bool good=p&&p<=UINTPTR_MAX-0xdc44&&read(p,before.data(),before.size())&&
                        read(p+0xdc40,&oldCount,4)&&oldCount<=48;
        const int result=original(error,[&]{return start_(manager,threshold);});
        std::array<std::uint64_t,keyCount> keys{};std::uint32_t count{};
        if(good&&read(p+0x1400,keys.data(),sizeof(keys))&&read(p+0xdc40,&count,4)&&count<=48)
            publish(before,oldCount,keys,count,threshold,result);
        else counters_[5].fetch_add(1,std::memory_order_relaxed);
        return result;
    }
    std::string report()const{
        static constexpr const char* names[]{"leaseResets","nestedResets","qualifiedResets","readDeclinedResets","mainStartFrames","snapshotFailures","publishedReceipts"};
        std::string out="{";
        for(unsigned i=0;i<counters_.size();++i){if(i)out+=",";out+='"';out+=names[i];out+="\":"+std::to_string(counters_[i].load(std::memory_order_relaxed));}
        std::array<std::uint64_t,beforeBytes/8> words{};std::array<std::uint64_t,keyCount> keys{};
        std::uint32_t old{},count{},threshold{},returned{};std::uint64_t seq{};bool stable=false;
        for(unsigned attempt=0;attempt<3&&!stable;++attempt){
            seq=sequence_.load(std::memory_order_acquire);if(!seq||seq&1)continue;
            for(unsigned i=0;i<words.size();++i)words[i]=before_[i].load(std::memory_order_relaxed);
            for(unsigned i=0;i<keys.size();++i)keys[i]=selected_[i].load(std::memory_order_relaxed);
            old=oldCount_.load(std::memory_order_relaxed);count=newCount_.load(std::memory_order_relaxed);
            threshold=threshold_.load(std::memory_order_relaxed);returned=returned_.load(std::memory_order_relaxed);
            // Order the payload reads before validating the sequence on ARM.
            std::atomic_thread_fence(std::memory_order_acquire);
            stable=sequence_.load(std::memory_order_acquire)==seq&&count<=48&&old<=48;
        }
        out+=",\"stable\":";out+=stable?"true":"false";
        if(stable){
            static constexpr char hex[]="0123456789abcdef";
            out+=",\"serial\":"+std::to_string(seq/2)+",\"beforeHex\":\"";
            for(auto word:words)for(unsigned j=0;j<8;++j){const auto b=unsigned((word>>(j*8))&255);out+=hex[b>>4];out+=hex[b&15];}
            out+="\",\"oldGroupCount\":"+std::to_string(old)+",\"afterGroupCount\":"+std::to_string(count);
            out+=",\"threshold\":"+std::to_string(std::int32_t(threshold))+",\"nativeReturn\":"+std::to_string(std::int32_t(returned));
            out+=",\"afterSelectedKeys\":[";
            for(unsigned i=0;i<count;++i){if(i)out+=",";out+=std::to_string(keys[i]);}out+="]";
        }
        return out+",\"fpsClaim\":null}";
    }
};
inline thread_local InstanceFrequency67::State* InstanceFrequency67::active_{};
inline InstanceFrequency67 instanceFrequency67;
inline std::atomic<bool> instanceFrequencyReady67{};
} // namespace rdrvr::experimental
