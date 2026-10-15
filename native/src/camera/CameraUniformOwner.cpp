#include "CameraUniformOwner.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <thread>

namespace rdr95 {
namespace {
struct Errno { int* cell=&errno; int value=*cell; ~Errno(){*cell=value;} };
bool powerOfTwo(VkDeviceSize n)noexcept{return n&&!(n&(n-1));}
std::atomic<std::uint64_t> deviceSerial{};
std::uint64_t nextDeviceGeneration()noexcept {
    auto value=deviceSerial.load(std::memory_order_relaxed);
    for(;;) {
        if(value==std::numeric_limits<std::uint64_t>::max())return 0;
        if(deviceSerial.compare_exchange_weak(value,value+1,std::memory_order_relaxed))return value+1;
    }
}
}
CameraUniformOwner::Api::operator bool()const noexcept {
    return getProperties&&getMemoryProperties&&createBuffer&&getBufferRequirements&&
        allocateMemory&&bindMemory&&mapMemory&&flushMemory&&unmapMemory&&destroyBuffer&&
        freeMemory&&createPool&&destroyPool&&allocateSets&&updateSets&&createLayout&&destroyLayout;
}

struct CameraUniformOwner::Impl {
    struct Arena {
        VkBuffer buffer{}; VkDeviceMemory memory{}; VkDescriptorPool pool{};
        std::array<VkDescriptorSet,SlotsPerCommand> sets{};
        void* data{}; VkDeviceSize bytes{}; bool mapped{},coherent{};
    };
    struct Payload { PassKey key{}; Bytes bytes{}; };
    struct Command {
        VkCommandBuffer handle{}; VkCommandPool pool{};
        VkCommandBufferLevel level=VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        std::uint64_t allocation{},recording{};
        std::thread::id thread{};
        bool canRecord{},valid{},poisoned{};
        unsigned used{};
        Arena arena;
        std::array<Payload,SlotsPerCommand> payloads{};
    };
    mutable std::mutex mutex;
    Api api{}; VkDevice device{}; VkPhysicalDevice physical{};
    VkDescriptorSetLayout layout{};
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkDeviceSize stride{},arenaBytes{};
    std::uint64_t serial{},deviceGeneration{};
    bool ready{},lost{},exhausted{};
    Stats counters{};
    std::array<Command,MaxCommands> commands{};

    std::uint64_t next()noexcept {
        if(exhausted||serial==std::numeric_limits<std::uint64_t>::max()){
            exhausted=true;ready=false;return 0;
        }
        return ++serial;
    }
    bool live()const noexcept{return ready&&!lost&&!exhausted&&device&&layout;}
    bool result(VkResult r)noexcept {if(r==VK_ERROR_DEVICE_LOST)lost=true;return r==VK_SUCCESS;}
    Command* find(VkCommandBuffer h)noexcept {
        if(!h)return nullptr;
        for(auto& c:commands)if(c.handle==h)return &c;
        return nullptr;
    }
    void destroy(Arena& a)noexcept {
        // Descriptor objects are discarded before their immutable bindings.
        if(a.pool)api.destroyPool(device,a.pool,nullptr);
        if(a.mapped)api.unmapMemory(device,a.memory);
        if(a.buffer)api.destroyBuffer(device,a.buffer,nullptr);
        if(a.memory)api.freeMemory(device,a.memory,nullptr);
        a={};
    }
    void free(Command& c)noexcept {
        const auto bytes=c.arena.bytes;
        destroy(c.arena);
        counters.memoryBytes-=bytes;
        if(c.handle&&counters.commands)--counters.commands;
        c={}; ++counters.retirements;
    }
    void discard(Command& c)noexcept {
        // Called ONLY after successful native discard. Descriptor bindings stay
        // fixed; arena bytes become writable only in a fresh valid recording.
        c.canRecord=false;c.valid=false;c.thread={};c.used=0;c.recording=next();
        ++counters.retirements;
    }
    bool makeArena(Command& c)noexcept {
        if(c.arena.buffer)return true;
        Arena a{};
        const auto fail=[&]()noexcept {destroy(a);return false;};
        VkBufferCreateInfo buffer{};buffer.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer.size=arenaBytes;buffer.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        buffer.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        VkResult r=api.createBuffer(device,&buffer,nullptr,&a.buffer);
        if(!result(r)){a.buffer=VK_NULL_HANDLE;return false;}
        if(!a.buffer)return fail();
        VkMemoryRequirements req{};api.getBufferRequirements(device,a.buffer,&req);
        if(req.size<arenaBytes||!powerOfTwo(req.alignment)||!req.memoryTypeBits||
           req.size>MaxMemoryBytes||req.size>MaxMemoryBytes-counters.memoryBytes)return fail();
        unsigned selected=VK_MAX_MEMORY_TYPES;
        for(unsigned pass=0;pass<2&&selected==VK_MAX_MEMORY_TYPES;++pass)
            for(unsigned i=0;i<memoryProperties.memoryTypeCount;++i) {
                const auto flags=memoryProperties.memoryTypes[i].propertyFlags;
                if((req.memoryTypeBits&(1u<<i))&&(flags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)&&
                   (pass||(flags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) {selected=i;break;}
            }
        if(selected==VK_MAX_MEMORY_TYPES)return fail();
        a.coherent=(memoryProperties.memoryTypes[selected].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
        VkMemoryAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize=req.size;allocation.memoryTypeIndex=selected;
        r=api.allocateMemory(device,&allocation,nullptr,&a.memory);
        if(!result(r)){a.memory=VK_NULL_HANDLE;return fail();}
        if(!a.memory)return fail();
        a.bytes=req.size;
        if(!result(api.bindMemory(device,a.buffer,a.memory,0)))return fail();
        r=api.mapMemory(device,a.memory,0,VK_WHOLE_SIZE,0,&a.data);
        if(!result(r)){a.data=nullptr;return fail();}
        a.mapped=true;
        if(!a.data)return fail();
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,SlotsPerCommand};
        VkDescriptorPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets=SlotsPerCommand;pool.poolSizeCount=1;pool.pPoolSizes=&size;
        r=api.createPool(device,&pool,nullptr,&a.pool);
        if(!result(r)){a.pool=VK_NULL_HANDLE;return fail();}
        if(!a.pool)return fail();
        std::array<VkDescriptorSetLayout,SlotsPerCommand> layouts{};layouts.fill(layout);
        VkDescriptorSetAllocateInfo sets{};sets.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        sets.descriptorPool=a.pool;sets.descriptorSetCount=SlotsPerCommand;sets.pSetLayouts=layouts.data();
        if(!result(api.allocateSets(device,&sets,a.sets.data())))return fail();
        std::array<VkDescriptorBufferInfo,SlotsPerCommand> buffers{};
        std::array<VkWriteDescriptorSet,SlotsPerCommand> writes{};
        for(unsigned i=0;i<SlotsPerCommand;++i) {
            if(!a.sets[i])return fail();
            buffers[i]={a.buffer,VkDeviceSize(i)*stride,PayloadBytes};
            auto& w=writes[i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet=a.sets[i];w.dstBinding=BindingIndex;w.descriptorCount=1;
            w.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;w.pBufferInfo=&buffers[i];
        }
        api.updateSets(device,SlotsPerCommand,writes.data(),0,nullptr);
        c.arena=a;counters.memoryBytes+=a.bytes;++counters.arenas;return true;
    }
    void forget()noexcept {
        ready=false;deviceGeneration=0;layout=VK_NULL_HANDLE;device=VK_NULL_HANDLE;
        physical=VK_NULL_HANDLE;api={};stride=arenaBytes=0;lost=false;
        for(auto& c:commands)c={};
        counters.memoryBytes=0;counters.commands=0;
        // serial/exhausted intentionally survive same-object/device-handle ABA.
    }
};

CameraUniformOwner::CameraUniformOwner()noexcept:impl_(new(std::nothrow) Impl){}
CameraUniformOwner::~CameraUniformOwner()=default;
bool CameraUniformOwner::initialize(VkPhysicalDevice physical,VkDevice device,Api api)noexcept {
    Errno preserve;CameraMetadataActivity::Mutation mutation(activity_);
    try {
        if(!impl_||!physical||!device||!api)return false;
        auto& s=*impl_;std::lock_guard lock(s.mutex);
        if(s.device||s.exhausted)return false;
        VkPhysicalDeviceProperties properties{};VkPhysicalDeviceMemoryProperties memory{};
        api.getProperties(physical,&properties);api.getMemoryProperties(physical,&memory);
        const auto& l=properties.limits;
        if(l.maxBoundDescriptorSets<=DescriptorSet||l.maxUniformBufferRange<PayloadBytes||
           !l.maxPerStageDescriptorUniformBuffers||!l.maxDescriptorSetUniformBuffers||
           !powerOfTwo(l.minUniformBufferOffsetAlignment)||!powerOfTwo(l.nonCoherentAtomSize)||
           !memory.memoryTypeCount||memory.memoryTypeCount>VK_MAX_MEMORY_TYPES||
           !memory.memoryHeapCount||memory.memoryHeapCount>VK_MAX_MEMORY_HEAPS)return false;
        for(unsigned i=0;i<memory.memoryTypeCount;++i)
            if(memory.memoryTypes[i].heapIndex>=memory.memoryHeapCount)return false;
        const auto alignment=std::max(l.minUniformBufferOffsetAlignment,l.nonCoherentAtomSize);
        if(alignment>MaxMemoryBytes/SlotsPerCommand)return false;
        const auto stride=(VkDeviceSize(PayloadBytes)+alignment-1)&~(alignment-1);
        if(!stride||stride>MaxMemoryBytes/SlotsPerCommand)return false;
        const auto generation=nextDeviceGeneration();if(!generation){s.exhausted=true;return false;}
        const VkDescriptorSetLayoutBinding binding{BindingIndex,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,ShaderStages,nullptr};
        VkDescriptorSetLayoutCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount=1;info.pBindings=&binding;
        VkDescriptorSetLayout layout{};
        if(api.createLayout(device,&info,nullptr,&layout)!=VK_SUCCESS||!layout)return false;
        s.device=device;s.physical=physical;s.api=api;s.layout=layout;s.deviceGeneration=generation;
        s.memoryProperties=memory;s.stride=stride;s.arenaBytes=stride*SlotsPerCommand;
        s.lost=false;s.ready=true;return true;
    }catch(...){return false;}
}
bool CameraUniformOwner::layout(Layout& out)const noexcept {
    Errno preserve;out={};
    try {if(!impl_)return false;auto& s=*impl_;std::lock_guard lock(s.mutex);
        if(!s.live())return false;out={s.device,s.layout,s.deviceGeneration};return true;
    }catch(...){return false;}
}
bool CameraUniformOwner::layoutCurrent(const Layout& value)const noexcept {
    Errno preserve;
    try {if(!impl_)return false;auto& s=*impl_;std::lock_guard lock(s.mutex);
        return s.live()&&value.device==s.device&&value.handle==s.layout&&value.generation==s.deviceGeneration;
    }catch(...){return false;}
}
bool CameraUniformOwner::commandAllocated(VkDevice d,VkCommandPool pool,VkCommandBuffer h,
    VkCommandBufferLevel level,VkResult result)noexcept {
    Errno preserve;CameraMetadataActivity::Mutation mutation(activity_);
    try {if(!impl_)return false;auto& s=*impl_;std::lock_guard lock(s.mutex);
        if(!s.live()||d!=s.device||!pool||!h||result!=VK_SUCCESS)return false;
        if(auto* existing=s.find(h)) {existing->valid=false;existing->canRecord=false;existing->poisoned=true;return false;}
        for(auto& c:s.commands)if(!c.handle){
            const auto generation=s.next();if(!generation)return false;
            c.handle=h;c.pool=pool;c.level=level;c.allocation=generation;++s.counters.commands;return true;
        }
        ++s.counters.declines;return false;
    }catch(...){return false;}
}
void CameraUniformOwner::commandBegun(VkCommandBuffer h,const VkCommandBufferBeginInfo* info,VkResult result)noexcept {
    Errno preserve;CameraMetadataActivity::Mutation mutation(activity_);
    try {if(!impl_)return;auto& s=*impl_;std::lock_guard lock(s.mutex);auto* c=s.find(h);if(!c)return;
        if(!s.result(result)){c->canRecord=false;c->valid=false;return;}
        s.discard(*c);
        if(!s.live()||c->poisoned||!c->recording||c->level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY||!info||
           info->sType!=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO||info->pNext||info->pInheritanceInfo||
           (info->flags&~VkCommandBufferUsageFlags(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT)))return;
        c->thread=std::this_thread::get_id();c->canRecord=true;c->valid=true;
    }catch(...){}
}
void CameraUniformOwner::commandEnded(VkCommandBuffer h,VkResult r)noexcept {
    Errno preserve;CameraMetadataActivity::Mutation mutation(activity_);
    try {if(!impl_)return;auto& s=*impl_;std::lock_guard lock(s.mutex);auto* c=s.find(h);if(!c)return;
        c->canRecord=false;if(!s.result(r))c->valid=false;
    }catch(...){}
}
void CameraUniformOwner::commandSubmitted(VkCommandBuffer h,VkResult r)noexcept {
    // Retention follows command recording, not submit count/result or a CPU frame.
    Errno preserve;CameraMetadataActivity::Mutation mutation(activity_);
    if(r!=VK_ERROR_DEVICE_LOST)return;
    try {if(!impl_)return;auto& s=*impl_;std::lock_guard lock(s.mutex);
        if(s.find(h))s.lost=true;
    }catch(...){}
}
void CameraUniformOwner::commandReset(VkCommandBuffer h,VkResult r)noexcept {
    Errno preserve;CameraMetadataActivity::Mutation mutation(activity_);
    try {if(!impl_)return;auto& s=*impl_;std::lock_guard lock(s.mutex);auto* c=s.find(h);if(!c)return;
        if(s.result(r))s.discard(*c);else {c->canRecord=false;c->valid=false;}
    }catch(...){}
}
void CameraUniformOwner::commandFreed(VkDevice d,VkCommandPool pool,VkCommandBuffer h)noexcept {
    Errno preserve;CameraMetadataActivity::Mutation mutation(activity_);
    try {if(!impl_)return;auto& s=*impl_;std::lock_guard lock(s.mutex);auto* c=s.find(h);
        if(d==s.device&&c&&c->pool==pool)s.free(*c);
    }catch(...){}
}
void CameraUniformOwner::poolReset(VkDevice d,VkCommandPool pool,VkResult r)noexcept {
    Errno preserve;CameraMetadataActivity::Mutation mutation(activity_);
    try {if(!impl_)return;auto& s=*impl_;std::lock_guard lock(s.mutex);if(d!=s.device)return;
        const bool success=s.result(r);
        for(auto& c:s.commands)if(c.handle&&c.pool==pool){
            if(success)s.discard(c);else {c.canRecord=false;c.valid=false;}
        }
    }catch(...){}
}
void CameraUniformOwner::poolDestroyed(VkDevice d,VkCommandPool pool)noexcept {
    Errno preserve;CameraMetadataActivity::Mutation mutation(activity_);
    try {if(!impl_)return;auto& s=*impl_;std::lock_guard lock(s.mutex);if(d!=s.device)return;
        for(auto& c:s.commands)if(c.handle&&c.pool==pool)s.free(c);
    }catch(...){}
}
bool CameraUniformOwner::prepare(VkCommandBuffer h,PassKey key,const Bytes& bytes,Ticket& out)noexcept {
    Errno preserve;CameraMetadataActivity::Mutation mutation(activity_);out={};
    try {if(!impl_)return false;auto& s=*impl_;std::lock_guard lock(s.mutex);auto* c=s.find(h);
        if(!s.live()||!c||!c->valid||!c->canRecord||c->thread!=std::this_thread::get_id()||
           !key.pass||!key.pose||key.eye>1){++s.counters.declines;return false;}
        const Bytes captured=bytes;
        unsigned slot=0;
        for(;slot<c->used;++slot)if(c->payloads[slot].key==key&&c->payloads[slot].bytes==captured)break;
        if(slot==c->used) {
            if(c->used==SlotsPerCommand||!s.makeArena(*c)){++s.counters.declines;return false;}
            auto* destination=static_cast<std::byte*>(c->arena.data)+VkDeviceSize(slot)*s.stride;
            std::memcpy(destination,captured.data(),PayloadBytes);
            if(!c->arena.coherent) {
                VkMappedMemoryRange range{};range.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                range.memory=c->arena.memory;range.offset=VkDeviceSize(slot)*s.stride;range.size=s.stride;
                if(!s.result(s.api.flushMemory(s.device,1,&range))){++s.counters.declines;return false;}
            }
            c->payloads[slot]={key,captured};++c->used;++s.counters.payloads;
        }else ++s.counters.hits;
        out.owner_=this;out.device_=s.deviceGeneration;out.allocation_=c->allocation;
        out.recording_=c->recording;out.command_=unsigned(c-s.commands.data());out.slot_=slot;
        return true;
    }catch(...){return false;}
}
bool CameraUniformOwner::resolve(const Ticket& t,Binding* out,bool recording)const noexcept {
    Errno preserve;if(out)*out={};
    try {if(!impl_||t.owner_!=this)return false;auto& s=*impl_;std::lock_guard lock(s.mutex);
        if(!s.live()||t.device_!=s.deviceGeneration||t.command_>=MaxCommands)return false;
        const auto& c=s.commands[t.command_];
        if(!c.handle||!c.valid||t.allocation_!=c.allocation||t.recording_!=c.recording||
           t.slot_>=c.used||!c.arena.buffer||!c.arena.sets[t.slot_]||
           (recording&&(!c.canRecord||c.thread!=std::this_thread::get_id())))return false;
        if(out)*out={s.device,c.handle,c.arena.sets[t.slot_],s.layout,c.arena.buffer,
            VkDeviceSize(t.slot_)*s.stride,PayloadBytes,s.deviceGeneration,c.allocation,c.recording,
            c.payloads[t.slot_].key};
        return true;
    }catch(...){return false;}
}
bool CameraUniformOwner::Ticket::current()const noexcept{return owner_&&owner_->resolve(*this,nullptr,false);}
bool CameraUniformOwner::Ticket::resolveForRecording(Binding& out)const noexcept {
    if(!owner_){out={};return false;}return owner_->resolve(*this,&out,true);
}
bool CameraUniformOwner::RecordingObservation::current()const noexcept {
    // Check issuing thread before reading anything through the owner pointer.
    return owner_&&stamp_&&thread_==std::this_thread::get_id()&&owner_->activity_.current()==stamp_;
}
bool CameraUniformOwner::observe(const Ticket& t,Binding& out,RecordingObservation& observation)const noexcept {
    Errno preserve;out={};observation={};
    const auto before=activity_.current();
    if(!before)return false;
    Binding resolved{};
    if(!resolve(t,&resolved,true)||activity_.current()!=before)return false;
    RecordingObservation issued;issued.owner_=this;issued.stamp_=before;issued.thread_=std::this_thread::get_id();
    out=resolved;observation=issued;return true;
}
bool CameraUniformOwner::Ticket::observeForRecording(Binding& out,RecordingObservation& observation)const noexcept {
    if(!owner_){out={};observation={};return false;}
    return owner_->observe(*this,out,observation);
}
CameraUniformOwner::Stats CameraUniformOwner::stats()const noexcept {
    Errno preserve;
    try {if(!impl_)return {};auto& s=*impl_;std::lock_guard lock(s.mutex);
        auto r=s.counters;r.ready=s.live();r.deviceLost=s.lost;return r;
    }catch(...){return {};}
}
bool CameraUniformOwner::shutdownAfterIdle(VkDevice d,VkResult idle)noexcept {
    Errno preserve;CameraMetadataActivity::Mutation mutation(activity_);
    try {if(!impl_)return false;auto& s=*impl_;std::lock_guard lock(s.mutex);
        if(!d||d!=s.device||(idle!=VK_SUCCESS&&idle!=VK_ERROR_DEVICE_LOST))return false;
        s.ready=false;for(auto& c:s.commands)if(c.handle)s.free(c);
        s.api.destroyLayout(s.device,s.layout,nullptr);s.forget();return true;
    }catch(...){return false;}
}
void CameraUniformOwner::abandonDevice(VkDevice d)noexcept {
    Errno preserve;CameraMetadataActivity::Mutation mutation(activity_);
    try {if(!impl_)return;auto& s=*impl_;std::lock_guard lock(s.mutex);
        if(d&&d==s.device)s.forget();
    }catch(...){}
}
} // namespace rdr95
