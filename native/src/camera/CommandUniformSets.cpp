#include "CommandUniformSets.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <limits>
#include <mutex>
#include <new>

namespace rdr95 {
namespace {
struct Errno {int value=errno;~Errno(){errno=value;}};
std::atomic<std::uint64_t> serial{};
std::uint64_t next(){auto n=serial.load();while(n!=UINT64_MAX){if(serial.compare_exchange_weak(n,n+1))return n+1;}return 0;}
constexpr unsigned HashSlots=CommandUniformSets::MaxSets*2;
}
CommandUniformSets::Api::operator bool()const noexcept{return createLayout&&destroyLayout&&createPool&&destroyPool&&allocate&&update;}
struct CommandUniformSets::Impl {
    struct Definition {
        VkDescriptorSetLayout handle{};unsigned count{};
        std::array<VkDescriptorSetLayoutBinding,MaxBindings> bindings{};
        std::array<VkDescriptorBindingFlags,MaxBindings> flags{};
    };
    struct Key {
        VkBuffer buffer{};VkCommandBuffer command{};
        std::uint64_t arenaDevice{},allocation{};unsigned layout{};
        std::array<std::uint32_t,MaxBindings> ranges{};
        bool operator==(const Key&)const=default;
    };
    struct Set {Key key{};VkDescriptorSet handle{};};
    struct Pool {VkDescriptorPool handle{};unsigned reserved{};};
    mutable std::mutex mutex;VkDevice device{};std::uint64_t generation{};Api api{};
    bool terminal{},lost{};Stats counters{};
    std::array<Definition,MaxLayouts> layouts{};
    std::array<Set,MaxSets> sets{};
    std::array<Pool,MaxSets/SetsPerPool> pools{};
    std::array<unsigned,HashSlots> index{}; // zero empty; otherwise record+1
    bool live()const{return device&&generation&&!terminal&&!lost;}
    bool result(VkResult r){if(r==VK_ERROR_DEVICE_LOST)lost=true;return r==VK_SUCCESS;}
    static std::uint64_t hash(const Key& k){
        std::uint64_t h=1469598103934665603ull;
        const auto mix=[&](std::uint64_t x){h^=x;h*=1099511628211ull;h^=h>>32;};
        mix(reinterpret_cast<std::uintptr_t>(k.buffer));mix(reinterpret_cast<std::uintptr_t>(k.command));
        mix(k.arenaDevice);mix(k.allocation);mix(k.layout);for(auto r:k.ranges)mix(r);return h;
    }
    unsigned bucket(const Key& k)const{
        unsigned i=static_cast<unsigned>(hash(k))&(HashSlots-1);
        for(unsigned n=0;n<HashSlots;++n,i=(i+1)&(HashSlots-1))
            if(!index[i]||sets[index[i]-1].key==k)return i;
        return HashSlots;
    }
    bool key(const Definition& d,unsigned layout,const CommandUniformArena::Batch& batch,
             std::span<const std::uint8_t> mapping,Key& out,Binding* binding=nullptr)const{
        if(batch.device!=device||!batch.command||!batch.deviceGeneration||!batch.allocationGeneration||!batch.recordingGeneration||
           !batch.count||batch.count>CommandUniformArena::MaxBatchSlices||mapping.size()!=d.count)return false;
        Key value{};value.command=batch.command;value.arenaDevice=batch.deviceGeneration;value.allocation=batch.allocationGeneration;value.layout=layout;
        for(unsigned i=0;i<d.count;++i){
            if(mapping[i]==Absent){if(!(d.flags[i]&VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT))return false;if(binding)binding->offsets[i]=0;continue;}
            if(mapping[i]>=batch.count)return false;
            const auto& slice=batch.slices[mapping[i]];
            if(!slice.buffer||!slice.range||slice.range>UINT32_MAX||slice.offset>UINT32_MAX||
               slice.offset>UINT64_MAX-slice.range||(value.buffer&&value.buffer!=slice.buffer))return false;
            value.buffer=slice.buffer;value.ranges[i]=static_cast<std::uint32_t>(slice.range);
            if(binding)binding->offsets[i]=static_cast<std::uint32_t>(slice.offset);
        }
        // A completely absent set is not useful for this first owned subset.
        if(!value.buffer)return false;out=value;return true;
    }
    VkDescriptorSet allocate(const Definition& d){
        Pool* pool=nullptr;
        for(unsigned i=0;i<counters.pools;++i)if(pools[i].reserved<SetsPerPool){pool=&pools[i];break;}
        if(!pool){
            if(counters.pools==pools.size())return {};
            const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,SetsPerPool*MaxBindings};
            VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            ci.maxSets=SetsPerPool;ci.poolSizeCount=1;ci.pPoolSizes=&size;
            VkDescriptorPool handle{};const auto r=api.createPool(device,&ci,nullptr,&handle);
            if(!result(r)||!handle)return {}; // failed single-create output is undefined
            pool=&pools[counters.pools++];pool->handle=handle;
        }
        ++pool->reserved; // failed allocations may consume capacity; never recycle a speculative slot
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool=pool->handle;ai.descriptorSetCount=1;ai.pSetLayouts=&d.handle;
        VkDescriptorSet handle{};const auto r=api.allocate(device,&ai,&handle);
        return result(r)?handle:VK_NULL_HANDLE;
    }
    void forget(){device={};generation=0;api={};terminal=true;counters.ready=false;}
};
CommandUniformSets::CommandUniformSets()noexcept:impl_(new(std::nothrow) Impl){}
CommandUniformSets::~CommandUniformSets()=default;
bool CommandUniformSets::initialize(VkDevice d,Api api)noexcept{
    Errno keep;try{if(!impl_||!d||!api)return false;auto& s=*impl_;std::lock_guard lock(s.mutex);
        if(s.device||s.terminal)return false;const auto generation=next();if(!generation)return false;
        s.device=d;s.api=api;s.generation=generation;s.counters.ready=true;return true;
    }catch(...){return false;}
}
bool CommandUniformSets::createLayout(const VkDescriptorSetLayoutCreateInfo& ci,Layout& out)noexcept{
    Errno keep;try{if(!impl_)return false;auto& s=*impl_;std::lock_guard lock(s.mutex);
        if(!s.live()||ci.sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO||ci.flags||!ci.bindingCount||
           ci.bindingCount>MaxBindings||!ci.pBindings||s.counters.layouts==MaxLayouts)return false;
        const VkDescriptorSetLayoutBindingFlagsCreateInfo* flags=nullptr;
        if(ci.pNext){flags=static_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo*>(ci.pNext);
            if(flags->sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO||flags->pNext||
               (flags->bindingCount&&flags->bindingCount!=ci.bindingCount)||(flags->bindingCount&&!flags->pBindingFlags))return false;}
        struct Pair{VkDescriptorSetLayoutBinding binding{};VkDescriptorBindingFlags flags{};};
        std::array<Pair,MaxBindings> pairs{};
        for(unsigned i=0;i<ci.bindingCount;++i){const auto& b=ci.pBindings[i];const auto f=flags&&flags->bindingCount?flags->pBindingFlags[i]:0;
            if(b.binding>=MaxBindings||b.descriptorType!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC||b.descriptorCount!=1||b.pImmutableSamplers||
               !b.stageFlags||(b.stageFlags&~(VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT))||
               (f&~VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT))return false;
            pairs[i]={b,f};
        }
        std::sort(pairs.begin(),pairs.begin()+ci.bindingCount,[](const Pair& a,const Pair& b){return a.binding.binding<b.binding.binding;});
        Impl::Definition d{};d.count=ci.bindingCount;
        for(unsigned i=0;i<d.count;++i){if(i&&pairs[i-1].binding.binding==pairs[i].binding.binding)return false;d.bindings[i]=pairs[i].binding;d.flags[i]=pairs[i].flags;}
        VkDescriptorSetLayoutBindingFlagsCreateInfo ownFlags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        ownFlags.bindingCount=d.count;ownFlags.pBindingFlags=d.flags.data();
        auto own=ci;own.pBindings=d.bindings.data();own.pNext=flags?&ownFlags:nullptr;
        VkDescriptorSetLayout handle{};const auto r=s.api.createLayout(s.device,&own,nullptr,&handle);
        if(!s.result(r)||!handle)return false;d.handle=handle;
        const auto index=s.counters.layouts++;s.layouts[index]=d;
        Layout value;value.owner_=this;value.generation_=s.generation;value.index_=index;out=value;return true;
    }catch(...){return false;}
}
bool CommandUniformSets::prepare(const Layout& layout,const CommandUniformArena::Ticket& bytes,
                                std::span<const std::uint8_t> mapping,Ticket& out)noexcept{
    Errno keep;try{if(!impl_)return false;
        CommandUniformArena::Batch batch{};if(!bytes.resolveForRecording(batch))return false;
        auto& s=*impl_;std::lock_guard lock(s.mutex);
        const auto fail=[&]{++s.counters.declines;return false;};
        if(!s.live()||layout.owner_!=this||layout.generation_!=s.generation||layout.index_>=s.counters.layouts)return fail();
        const auto& d=s.layouts[layout.index_];Impl::Key key{};
        if(!s.key(d,layout.index_,batch,mapping,key))return fail();
        const auto bucket=s.bucket(key);if(bucket==HashSlots)return fail();unsigned index{};
        if(s.index[bucket]){index=s.index[bucket]-1;++s.counters.hits;}
        else {
            if(s.counters.sets==MaxSets)return fail();
            const auto handle=s.allocate(d);if(!handle)return fail();
            std::array<VkDescriptorBufferInfo,MaxBindings> buffers{};std::array<VkWriteDescriptorSet,MaxBindings> writes{};unsigned count{};
            for(unsigned i=0;i<d.count;++i)if(key.ranges[i]){
                buffers[count]={key.buffer,0,key.ranges[i]};auto& w=writes[count];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w.dstSet=handle;w.dstBinding=d.bindings[i].binding;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;w.pBufferInfo=&buffers[count];++count;}
            s.api.update(s.device,count,writes.data(),0,nullptr);
            index=s.counters.sets++;s.sets[index]={key,handle};s.index[bucket]=index+1;
        }
        // Raw PFNs are terminal/nonreentrant; command lifecycle is externally
        // serialized, nevertheless never publish after the arena was retired.
        CommandUniformArena::Batch current{};if(!bytes.resolveForRecording(current)||current.deviceGeneration!=batch.deviceGeneration||
           current.allocationGeneration!=batch.allocationGeneration||current.recordingGeneration!=batch.recordingGeneration)return fail();
        Ticket value;value.owner_=this;value.generation_=s.generation;value.set_=index;value.bytes_=bytes;
        std::copy(mapping.begin(),mapping.end(),value.mapping_.begin());out=value;return true;
    }catch(...){return false;}
}
bool CommandUniformSets::resolve(const Ticket& t,Binding& out)const noexcept{
    Errno keep;try{if(!impl_||t.owner_!=this)return false;
        CommandUniformArena::Batch batch{};if(!t.bytes_.resolveForRecording(batch))return false;
        auto& s=*impl_;std::lock_guard lock(s.mutex);
        if(!s.live()||t.generation_!=s.generation||t.set_>=s.counters.sets)return false;
        const auto& set=s.sets[t.set_];const auto& d=s.layouts[set.key.layout];
        Binding value{};Impl::Key key{};
        if(!s.key(d,set.key.layout,batch,{t.mapping_.data(),d.count},key,&value)||!(key==set.key))return false;
        value.device=s.device;value.command=batch.command;value.descriptor=set.handle;value.compatibleLayout=d.handle;value.count=d.count;
        for(unsigned i=0;i<d.count;++i){const auto& b=d.bindings[i];
            value.definitions[i]={b.binding,b.descriptorCount,b.descriptorType,b.stageFlags,d.flags[i]};
            if(set.key.ranges[i])value.presentBindings|=1u<<b.binding;}
        out=value;return true;
    }catch(...){return false;}
}
bool CommandUniformSets::Ticket::resolveForRecording(Binding& out)const noexcept{return owner_&&owner_->resolve(*this,out);}
CommandUniformSets::Stats CommandUniformSets::stats()const noexcept{
    Errno keep;try{if(!impl_)return {};std::lock_guard lock(impl_->mutex);auto out=impl_->counters;out.ready=impl_->live();return out;}catch(...){return {};}
}
bool CommandUniformSets::shutdownAfterIdle(VkDevice d,VkResult r)noexcept{
    Errno keep;try{if(!impl_)return false;auto& s=*impl_;std::lock_guard lock(s.mutex);
        if(!s.device||s.device!=d||(r!=VK_SUCCESS&&r!=VK_ERROR_DEVICE_LOST))return false;
        for(unsigned i=0;i<s.counters.pools;++i)s.api.destroyPool(d,s.pools[i].handle,nullptr);
        for(unsigned i=0;i<s.counters.layouts;++i)s.api.destroyLayout(d,s.layouts[i].handle,nullptr);
        s.forget();return true;
    }catch(...){return false;}
}
void CommandUniformSets::abandonDevice(VkDevice d)noexcept{
    Errno keep;try{if(!impl_)return;auto& s=*impl_;std::lock_guard lock(s.mutex);if(d&&s.device==d)s.forget();}catch(...){}
}
}
