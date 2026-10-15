#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sstream>

namespace rdrvr::engine {
// Identity of one native command recording, supplied by the qualified hook.
// nativeFrame is VkContext_c::m_CurFrame, NOT an XR frame/eye counter. A new
// recordingGeneration is required after command-buffer reset, even if its
// handle and the native frame number happen to be reused.
struct NativeDescriptorEpoch {
    std::uint64_t recordingGeneration{};
    std::uintptr_t commandOwner{},device{},descriptorPool{},uploadBuffer{};
    std::uint32_t nativeFrame{};
    bool valid{};
    bool operator==(const NativeDescriptorEpoch& b)const noexcept {
        return recordingGeneration==b.recordingGeneration&&commandOwner==b.commandOwner&&
            device==b.device&&descriptorPool==b.descriptorPool&&uploadBuffer==b.uploadBuffer&&
            nativeFrame==b.nativeFrame&&valid==b.valid;
    }
};

// Reuses an immutable native UNIFORM_BUFFER descriptor set within a recording.
// No descriptor pools/sets are created, freed, updated, or rebound by this class.
// Misses call the pinned original with the exact original arguments. Hits return
// an earlier set whose complete binding contents and pool slot still match.
// The decoder matches librdr.so 3f80845e... GetOrCreateDescriptorSet + helper;
// the hook must verify BOTH code ranges, and cover every call to that function.
class NativeDescriptorReuse {
public:
    using Original=std::uintptr_t(*)(void*,void*,const void*,const void*,void*,void*,int);
    static constexpr unsigned bindingLimit=16,ways=4,buckets=64,capacity=ways*buckets;
private:
    struct Binding {std::uint64_t offset{},range{};bool operator==(const Binding& b)const noexcept{return offset==b.offset&&range==b.range;}};
    struct Key {
        std::uintptr_t context{},pool{},layout{};
        std::uint16_t mask{};
        std::array<Binding,bindingLimit> bindings{};
        bool operator==(const Key& b)const noexcept {
            return context==b.context&&pool==b.pool&&layout==b.layout&&mask==b.mask&&
                !std::memcmp(bindings.data(),b.bindings.data(),sizeof bindings);
        }
    };
    struct Pool {std::uintptr_t begin{},end{},capacity{};std::int32_t used{};std::uint32_t frame{};};
    struct Entry {Key key{};std::uintptr_t set{};std::uint32_t index{};bool valid{};};
    struct PoolObservation {std::uintptr_t address{};std::int32_t used{};std::uint32_t frame{};};
    struct Counts {std::uint64_t hits{},misses{},declined{},invalidations{},savedWrites{};};
    struct Local {
        const NativeDescriptorReuse* owner{};Original original{};NativeDescriptorEpoch epoch{};
        std::array<Entry,capacity> entries{};
        std::array<unsigned,buckets> next{};
        std::array<PoolObservation,64> pools{};
        Counts counts{};
    };
    static thread_local Local local_;
    std::atomic<std::uint64_t> hits_{},misses_{},declined_{},invalidations_{},savedWrites_{};
    std::atomic<bool> installed_{};

    template<class T> static T read(const void* p,std::size_t offset=0)noexcept {
        T value{};std::memcpy(&value,static_cast<const unsigned char*>(p)+offset,sizeof value);return value;
    }
    static bool vectorSpan(std::uintptr_t begin,std::uintptr_t end,std::size_t stride,std::size_t maximum)noexcept {
        return end>=begin&&(!end||begin)&&((end-begin)%stride)==0&&(end-begin)/stride<=maximum;
    }
    static bool poolState(const void* p,Pool& out)noexcept {
        out=read<Pool>(p);
        return vectorSpan(out.begin,out.end,8,1u<<20)&&out.capacity>=out.end&&out.used>=0&&
            static_cast<std::uint64_t>(out.used)<=(out.end-out.begin)/8;
    }
    // Read only entries which the original helper traverses. In particular,
    // unused vector slots are not dereferenced: they may retain stale pointers.
    static bool addShaderBindings(const void* shader,int type,std::uint16_t& mask,unsigned& work)noexcept {
        if(!shader)return false;
        const auto begin=read<std::uintptr_t>(shader,0xe0),end=read<std::uintptr_t>(shader,0xe8);
        if(!vectorSpan(begin,end,0x38,64))return false;
        for(auto outer=begin;outer!=end;outer+=0x38){
            const auto* record=reinterpret_cast<const void*>(outer);
            const auto first=read<std::uintptr_t>(record,8),last=read<std::uintptr_t>(record,16);
            if(!vectorSpan(first,last,0x18,128))return false;
            const auto n=(last-first)/0x18;
            if(n>256-work)return false;work+=static_cast<unsigned>(n);
            for(auto binding=first;binding!=last;binding+=0x18){
                const auto* b=reinterpret_cast<const void*>(binding);
                if(read<std::int32_t>(b,4)!=type)continue;
                const auto slot=read<std::int32_t>(b);
                if(slot<0||slot>=static_cast<int>(bindingLimit))return false;
                mask|=static_cast<std::uint16_t>(1u<<slot);
            }
        }
        return true;
    }
    static bool key(void* context,void* pool,const void* layout,const void* constants,void* vs,void* ps,int type,Key& out)noexcept {
        if(type!=6||!context||!pool||!layout||!constants)return false;
        out.context=reinterpret_cast<std::uintptr_t>(context);out.pool=reinterpret_cast<std::uintptr_t>(pool);
        out.layout=read<std::uintptr_t>(layout);if(!out.layout)return false;
        unsigned work{};
        if(!addShaderBindings(vs,type,out.mask,work)||!addShaderBindings(ps,type,out.mask,work)||!out.mask)return false;
        const auto begin=read<std::uintptr_t>(constants),end=read<std::uintptr_t>(constants,8);
        if(!vectorSpan(begin,end,8,256))return false;
        const auto count=(end-begin)/8;
        for(unsigned i=0;i<bindingLimit;++i){
            if(!(out.mask&(1u<<i)))continue;
            if(i>=count)return false;
            const auto pointer=read<std::uintptr_t>(reinterpret_cast<const void*>(begin),i*8);
            if(!pointer)continue; // Native null CB emits offset=range=0.
            const auto* cb=reinterpret_cast<const void*>(pointer);
            const auto descriptor=read<std::uintptr_t>(cb,8);if(!descriptor)return false;
            const auto range=read<std::int32_t>(reinterpret_cast<const void*>(descriptor),0x18);
            if(range<0)return false;
            out.bindings[i]={read<std::uint64_t>(cb,0x28),static_cast<std::uint64_t>(range)};
        }
        return true;
    }
    static unsigned bucket(const Key& key)noexcept {
        // This hash chooses four candidate slots; equality always checks every
        // binding value, so collisions cannot cause a false hit.
        std::uint64_t h=key.context^(key.pool<<1)^(key.layout<<3)^key.mask;
        for(unsigned i=0;i<bindingLimit;++i)h+=key.bindings[i].offset^(key.bindings[i].range<<32);
        h^=h>>33;h*=0xff51afd7ed558ccdull;h^=h>>33;
        return static_cast<unsigned>(h)&(buckets-1);
    }
    static unsigned writes(std::uint16_t mask)noexcept {unsigned n{};while(mask){mask&=static_cast<std::uint16_t>(mask-1);++n;}return n;}
    static void clearEntries(Local& s)noexcept {
        for(auto& entry:s.entries)entry.valid=false;
        for(auto& pool:s.pools)pool={};
        ++s.counts.invalidations;
    }
    static void observePool(Local& s,std::uintptr_t address,const Pool& pool)noexcept {
        // A reduced cursor means that native code recycled the pool. Retaining
        // old sets after that would silently change already recorded draws.
        auto& seen=s.pools[(address>>5)&(s.pools.size()-1)];
        if(seen.address&&seen.address!=address){
            // Observation-table eviction must evict the corresponding cache
            // entries too; otherwise a recycled pool could evade the cursor
            // check after a hash collision in this bounded table.
            for(auto& e:s.entries)if(e.valid&&e.key.pool==seen.address)e.valid=false;
            ++s.counts.invalidations;
        }
        if(seen.address==address&&(seen.frame!=pool.frame||seen.used>pool.used)){
            for(auto& e:s.entries)if(e.valid&&e.key.pool==address)e.valid=false;
            ++s.counts.invalidations;
        }
        seen={address,pool.used,pool.frame};
    }
public:
    void installed(bool v)noexcept{installed_.store(v,std::memory_order_relaxed);}
    bool installed()const noexcept{return installed_.load(std::memory_order_relaxed);}
    // Call at the beginning AND end of the qualified native scene. This also
    // publishes counters; no entry may survive a scene lifetime boundary.
    void reset()noexcept {auto& s=local_;if(s.owner==this){clearEntries(s);publish();s.epoch={};}}
    std::uintptr_t invoke(Original original,const NativeDescriptorEpoch& epoch,bool qualified,
                          void* context,void* pool,const void* layout,const void* constants,void* vs,void* ps,int type){
        auto& s=local_;
        if(s.owner!=this||s.original!=original){if(s.owner==this)publish();s={};s.owner=this;s.original=original;}
        auto decline=[&](){clearEntries(s);++s.counts.declined;publish(false);return original(context,pool,layout,constants,vs,ps,type);};
        if(!qualified||!epoch.valid||!epoch.recordingGeneration||!epoch.commandOwner||!epoch.device||!epoch.descriptorPool||!epoch.uploadBuffer)return decline();
        if(!(s.epoch==epoch)){clearEntries(s);s.epoch=epoch;}
        Key content{};Pool before{};
        if(!key(context,pool,layout,constants,vs,ps,type,content)||!poolState(pool,before))return decline();
        observePool(s,content.pool,before);
        const auto b=bucket(content),base=b*ways;
        for(unsigned i=0;i<ways;++i){
            auto& e=s.entries[base+i];
            if(!e.valid||!(e.key==content)||before.frame!=epoch.nativeFrame||e.index>=static_cast<std::uint32_t>(before.used))continue;
            if(read<std::uintptr_t>(reinterpret_cast<const void*>(before.begin),e.index*8)!=e.set){e.valid=false;continue;}
            ++s.counts.hits;s.counts.savedWrites+=writes(content.mask);publish(false);return e.set;
        }
        const auto result=original(context,pool,layout,constants,vs,ps,type);
        ++s.counts.misses;
        Pool after{};
        if(result&&poolState(pool,after)&&after.frame==epoch.nativeFrame&&after.used>0&&
            after.used==(before.frame==epoch.nativeFrame?before.used+1:1)){
            const auto index=static_cast<std::uint32_t>(after.used-1);
            if(read<std::uintptr_t>(reinterpret_cast<const void*>(after.begin),index*8)==result){
                observePool(s,content.pool,after);
                auto& entry=s.entries[base+(s.next[b]++%ways)];entry={content,result,index,true};
            }else clearEntries(s);
        }else clearEntries(s);
        publish(false);return result;
    }
    void publish(bool force=true)noexcept {
        auto& s=local_;if(s.owner!=this)return;const auto& c=s.counts;
        if(!force&&c.hits+c.misses+c.declined<1024)return;
        hits_.fetch_add(c.hits,std::memory_order_relaxed);misses_.fetch_add(c.misses,std::memory_order_relaxed);
        declined_.fetch_add(c.declined,std::memory_order_relaxed);invalidations_.fetch_add(c.invalidations,std::memory_order_relaxed);
        savedWrites_.fetch_add(c.savedWrites,std::memory_order_relaxed);s.counts={};
    }
    std::string report()const {
        std::ostringstream o;o<<"{\"installed\":"<<(installed()?"true":"false")<<",\"scope\":\"native-recording-uniform-descriptors\",\"capacityPerThread\":"<<capacity
            <<",\"hits\":"<<hits_.load()<<",\"misses\":"<<misses_.load()<<",\"declined\":"<<declined_.load()
            <<",\"invalidations\":"<<invalidations_.load()<<",\"savedDescriptorWrites\":"<<savedWrites_.load()<<",\"unpublishedPerThreadMax\":1023}";return o.str();
    }
    std::uint64_t hits()const noexcept{return hits_.load(std::memory_order_relaxed);}
    std::uint64_t misses()const noexcept{return misses_.load(std::memory_order_relaxed);}
    std::uint64_t declined()const noexcept{return declined_.load(std::memory_order_relaxed);}
    std::uint64_t savedWrites()const noexcept{return savedWrites_.load(std::memory_order_relaxed);}
};
inline thread_local NativeDescriptorReuse::Local NativeDescriptorReuse::local_{};
inline NativeDescriptorReuse nativeDescriptorReuse;
}
