#include "CommandUniformArena.h"
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
struct Errno { int* cell=&errno; int saved=*cell; ~Errno(){*cell=saved;} };
bool powerOfTwo(VkDeviceSize n)noexcept{return n&&!(n&(n-1));}
std::atomic<std::uint64_t> deviceSerial{};
std::uint64_t nextDevice()noexcept {
    auto n=deviceSerial.load(std::memory_order_relaxed);
    while(n!=std::numeric_limits<std::uint64_t>::max())
        if(deviceSerial.compare_exchange_weak(n,n+1,std::memory_order_relaxed))return n+1;
    return 0;
}
bool byteCopy(void* context,unsigned i,void* destination,VkDeviceSize bytes)noexcept {
    const auto* sources=static_cast<const CommandUniformArena::Source*>(context);
    const auto source=reinterpret_cast<std::uintptr_t>(sources[i].data);
    const auto target=reinterpret_cast<std::uintptr_t>(destination);
    if(!source||bytes>std::numeric_limits<std::uintptr_t>::max()-source||
       bytes>std::numeric_limits<std::uintptr_t>::max()-target||
       (source<target+bytes&&target<source+bytes))return false;
    std::memcpy(destination,sources[i].data,static_cast<std::size_t>(bytes));return true;
}
}
CommandUniformArena::Api::operator bool()const noexcept {
    return getProperties&&getMemoryProperties&&createBuffer&&getBufferRequirements&&
        allocateMemory&&bindMemory&&mapMemory&&flushMemory&&unmapMemory&&destroyBuffer&&freeMemory;
}
struct CommandUniformArena::Impl {
    struct Arena {VkBuffer buffer{};VkDeviceMemory memory{};void* data{};VkDeviceSize bytes{};bool mapped{},coherent{};};
    struct StoredSlice {VkDeviceSize offset{},range{};};
    struct BatchRecord {std::uint64_t serial{};unsigned first{},count{};};
    static_assert(sizeof(StoredSlice)==16&&sizeof(BatchRecord)==16);
    struct Command {
        VkCommandBuffer handle{};VkCommandPool pool{};
        VkCommandBufferLevel level=VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        std::uint64_t allocation{},recording{};std::thread::id thread{};
        bool valid{},canRecord{},poisoned{},copying{},retiring{};
        VkDeviceSize used{};Arena arena{};
        std::unique_ptr<BatchRecord[]> batches;std::unique_ptr<StoredSlice[]> slices;
        unsigned metadataCapacity{},batchCount{},sliceCount{};VkDeviceSize metadataBytes{};
    };
    mutable std::mutex mutex;
    Api api{};VkDevice device{};VkPhysicalDevice physical{};
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkDeviceSize alignment{},arenaBytes{},maximumRange{};
    std::uint64_t deviceGeneration{},serial{};
    bool ready{},lost{},exhausted{},terminal{};
    unsigned copying{};
    Stats counters{};
    std::array<Command,MaxCommands> commands{};
    bool live()const noexcept{return ready&&!lost&&!exhausted&&!terminal&&device;}
    bool result(VkResult r)noexcept{if(r==VK_ERROR_DEVICE_LOST)lost=true;return r==VK_SUCCESS;}
    std::uint64_t next()noexcept {
        if(exhausted||serial==std::numeric_limits<std::uint64_t>::max()) {exhausted=true;ready=false;return 0;}
        return ++serial;
    }
    Command* find(VkCommandBuffer h)noexcept {
        if(h)for(auto& c:commands)if(c.handle==h)return &c;
        return nullptr;
    }
    void destroy(Arena& a)noexcept {
        if(a.mapped)api.unmapMemory(device,a.memory);
        if(a.buffer)api.destroyBuffer(device,a.buffer,nullptr);
        if(a.memory)api.freeMemory(device,a.memory,nullptr);
        a={};
    }
    void free(Command& c)noexcept {
        if(c.copying) {c.retiring=true;c.valid=false;c.canRecord=false;return;}
        counters.memoryBytes-=c.arena.bytes;counters.metadataBytes-=c.metadataBytes;destroy(c.arena);
        if(c.handle&&counters.commands)--counters.commands;
        c={};++counters.retirements;
    }
    void discard(Command& c)noexcept {
        c.valid=false;c.canRecord=false;c.thread={};c.used=0;c.recording=next();
        c.batchCount=0;c.sliceCount=0;
        ++counters.retirements;
    }
    bool makeArena(Command& c)noexcept {
        if(c.arena.buffer)return true;
        const auto capacity=static_cast<unsigned>(std::min<VkDeviceSize>(arenaBytes/alignment,MaxMetadataSlices));
        const VkDeviceSize metadataBytes=VkDeviceSize(capacity)*(sizeof(BatchRecord)+sizeof(StoredSlice));
        if(!capacity||metadataBytes>MaxMetadataBytes-counters.metadataBytes)return false;
        // Both bounded arrays are cold/lazy and reserved before any copier.
        // Failure leaves no Vulkan allocation or partially published arena.
        std::unique_ptr<BatchRecord[]> batches(new(std::nothrow) BatchRecord[capacity]);
        std::unique_ptr<StoredSlice[]> slices(new(std::nothrow) StoredSlice[capacity]);
        if(!batches||!slices)return false;
        Arena a{};const auto fail=[&]()noexcept{destroy(a);return false;};
        try {
            VkBufferCreateInfo b{};b.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            b.size=arenaBytes;b.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;b.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
            if(!result(api.createBuffer(device,&b,nullptr,&a.buffer))){a.buffer=VK_NULL_HANDLE;return false;}
            if(!a.buffer)return fail();
            VkMemoryRequirements requirements{};api.getBufferRequirements(device,a.buffer,&requirements);
            if(requirements.size<arenaBytes||!powerOfTwo(requirements.alignment)||!requirements.memoryTypeBits||
               requirements.size>MaxMemoryBytes||requirements.size>MaxMemoryBytes-counters.memoryBytes)return fail();
            unsigned selected=VK_MAX_MEMORY_TYPES;
            for(unsigned pass=0;pass<2&&selected==VK_MAX_MEMORY_TYPES;++pass)
                for(unsigned i=0;i<memoryProperties.memoryTypeCount;++i) {
                    const auto flags=memoryProperties.memoryTypes[i].propertyFlags;
                    if((requirements.memoryTypeBits&(1u<<i))&&(flags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)&&
                       (pass||(flags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) {selected=i;break;}
                }
            if(selected==VK_MAX_MEMORY_TYPES)return fail();
            a.coherent=(memoryProperties.memoryTypes[selected].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
            VkMemoryAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=selected;
            if(!result(api.allocateMemory(device,&allocation,nullptr,&a.memory))){a.memory=VK_NULL_HANDLE;return fail();}
            if(!a.memory)return fail();
            a.bytes=requirements.size;
            if(!result(api.bindMemory(device,a.buffer,a.memory,0)))return fail();
            if(!result(api.mapMemory(device,a.memory,0,VK_WHOLE_SIZE,0,&a.data))){a.data=nullptr;return fail();}
            a.mapped=true;if(!a.data)return fail();
            c.batches=std::move(batches);c.slices=std::move(slices);c.metadataCapacity=capacity;c.metadataBytes=metadataBytes;
            c.arena=a;counters.memoryBytes+=a.bytes;counters.metadataBytes+=metadataBytes;++counters.arenas;return true;
        }catch(...){return fail();}
    }
    void forget()noexcept {
        ready=false;deviceGeneration=0;device=VK_NULL_HANDLE;physical=VK_NULL_HANDLE;
        api={};alignment=arenaBytes=maximumRange=0;lost=false;
        for(auto& c:commands)c={};
        counters.memoryBytes=0;counters.metadataBytes=0;counters.commands=0;
        // Serial, saturation and terminal-abandon state survive numeric handle ABA.
    }
};
CommandUniformArena::CommandUniformArena()noexcept:impl_(new(std::nothrow) Impl){}
CommandUniformArena::~CommandUniformArena()=default;
bool CommandUniformArena::initialize(VkPhysicalDevice p,VkDevice d,Api api,Configuration config)noexcept {
    Errno preserve;
    try {
        if(!impl_||!p||!d||!api||!config.bytesPerCommand||config.bytesPerCommand>MaxArenaBytes)return false;
        auto& s=*impl_;std::lock_guard lock(s.mutex);
        if(s.device||s.exhausted||s.terminal)return false;
        VkPhysicalDeviceProperties properties{};VkPhysicalDeviceMemoryProperties memory{};
        api.getProperties(p,&properties);api.getMemoryProperties(p,&memory);
        const auto& limits=properties.limits;
        if(!limits.maxUniformBufferRange||!powerOfTwo(limits.minUniformBufferOffsetAlignment)||
           !powerOfTwo(limits.nonCoherentAtomSize)||!memory.memoryTypeCount||
           memory.memoryTypeCount>VK_MAX_MEMORY_TYPES||!memory.memoryHeapCount||
           memory.memoryHeapCount>VK_MAX_MEMORY_HEAPS)return false;
        for(unsigned i=0;i<memory.memoryTypeCount;++i)
            if(memory.memoryTypes[i].heapIndex>=memory.memoryHeapCount)return false;
        const auto alignment=std::max(limits.minUniformBufferOffsetAlignment,limits.nonCoherentAtomSize);
        if(alignment>MaxArenaBytes)return false;
        const auto size=(config.bytesPerCommand+alignment-1)&~(alignment-1);
        if(!size||size>MaxArenaBytes)return false;
        const auto generation=nextDevice();if(!generation){s.exhausted=true;return false;}
        s.api=api;s.device=d;s.physical=p;s.memoryProperties=memory;s.alignment=alignment;
        s.arenaBytes=size;s.maximumRange=limits.maxUniformBufferRange;s.deviceGeneration=generation;
        s.lost=false;s.ready=true;return true;
    }catch(...){return false;}
}
bool CommandUniformArena::commandAllocated(VkDevice d,VkCommandPool pool,VkCommandBuffer h,VkCommandBufferLevel level,VkResult r)noexcept {
    Errno preserve;
    try {if(!impl_)return false;auto& s=*impl_;std::lock_guard lock(s.mutex);
        if(!s.live()||d!=s.device||!pool||!h||r!=VK_SUCCESS)return false;
        if(auto* c=s.find(h)){c->valid=false;c->canRecord=false;c->poisoned=true;return false;}
        for(auto& c:s.commands)if(!c.handle){const auto generation=s.next();if(!generation)return false;
            c.handle=h;c.pool=pool;c.level=level;c.allocation=generation;++s.counters.commands;return true;}
        ++s.counters.declines;return false;
    }catch(...){return false;}
}
void CommandUniformArena::commandBegun(VkCommandBuffer h,const VkCommandBufferBeginInfo* info,VkResult r)noexcept {
    Errno preserve;
    try {if(!impl_)return;auto& s=*impl_;std::lock_guard lock(s.mutex);auto* c=s.find(h);if(!c)return;
        if(!s.result(r)){c->canRecord=false;return;}
        s.discard(*c);
        if(!s.live()||c->poisoned||c->retiring||!c->recording||c->level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY||
           !info||info->sType!=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO||info->pNext||info->pInheritanceInfo||
           (info->flags&~VkCommandBufferUsageFlags(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT)))return;
        c->thread=std::this_thread::get_id();c->valid=true;c->canRecord=true;
    }catch(...){}
}
void CommandUniformArena::commandEnded(VkCommandBuffer h,VkResult r)noexcept {
    Errno preserve;
    try {if(!impl_)return;auto& s=*impl_;std::lock_guard lock(s.mutex);
        if(auto* c=s.find(h)){c->canRecord=false;s.result(r);}
    }catch(...){}
}
void CommandUniformArena::commandSubmitted(VkCommandBuffer h,VkResult r)noexcept {
    Errno preserve;if(r!=VK_ERROR_DEVICE_LOST)return;
    try {if(!impl_)return;auto& s=*impl_;std::lock_guard lock(s.mutex);if(s.find(h))s.lost=true;}catch(...){}
}
void CommandUniformArena::commandReset(VkCommandBuffer h,VkResult r)noexcept {
    Errno preserve;
    try {if(!impl_)return;auto& s=*impl_;std::lock_guard lock(s.mutex);if(auto* c=s.find(h)){
        if(s.result(r))s.discard(*c);else c->canRecord=false;}
    }catch(...){}
}
void CommandUniformArena::commandFreed(VkDevice d,VkCommandPool pool,VkCommandBuffer h)noexcept {
    Errno preserve;
    try {if(!impl_)return;auto& s=*impl_;std::lock_guard lock(s.mutex);auto* c=s.find(h);
        if(d==s.device&&c&&c->pool==pool)s.free(*c);
    }catch(...){}
}
void CommandUniformArena::poolReset(VkDevice d,VkCommandPool pool,VkResult r)noexcept {
    Errno preserve;
    try {if(!impl_)return;auto& s=*impl_;std::lock_guard lock(s.mutex);if(d!=s.device||!pool)return;
        const bool success=s.result(r);for(auto& c:s.commands)if(c.handle&&c.pool==pool){
            if(success)s.discard(c);else c.canRecord=false;}
    }catch(...){}
}
void CommandUniformArena::poolDestroyed(VkDevice d,VkCommandPool pool)noexcept {
    Errno preserve;
    try {if(!impl_)return;auto& s=*impl_;std::lock_guard lock(s.mutex);if(d!=s.device||!pool)return;
        for(auto& c:s.commands)if(c.handle&&c.pool==pool)s.free(c);
    }catch(...){}
}
bool CommandUniformArena::append(VkCommandBuffer h,std::span<const Source> sources,Ticket& out)noexcept {
    Errno preserve;out={};
    if(sources.empty()||sources.size()>MaxBatchSlices)return false;
    std::array<Source,MaxBatchSlices> captured{};std::array<CopySource,MaxBatchSlices> requests{};
    for(unsigned i=0;i<sources.size();++i){captured[i]=sources[i];requests[i]={sources[i].size,i};
        const auto address=reinterpret_cast<std::uintptr_t>(captured[i].data);
        if(!address||captured[i].size>std::numeric_limits<std::uintptr_t>::max()-address)return false;}
    return appendCopied(h,{requests.data(),sources.size()},captured.data(),byteCopy,out);
}
bool CommandUniformArena::appendCopied(VkCommandBuffer h,std::span<const CopySource> sources,void* context,CopyFunction copy,Ticket& out)noexcept {
    Errno preserve;out={};
    if(!impl_||!copy||sources.empty()||sources.size()>MaxBatchSlices)return false;
    std::array<CopySource,MaxBatchSlices> requests{};
    std::array<Slice,MaxBatchSlices> slices{};
    std::copy(sources.begin(),sources.end(),requests.begin());
    const auto count=static_cast<unsigned>(sources.size());
    auto& s=*impl_;
    try {
        std::unique_lock lock(s.mutex);auto* c=s.find(h);
        if(!s.live()||!c||!c->valid||!c->canRecord||c->copying||c->retiring||
           c->thread!=std::this_thread::get_id()){++s.counters.declines;return false;}
        const auto start=c->used;auto end=start;
        for(unsigned i=0;i<count;++i){const auto size=requests[i].size;
            if(!size||size>s.maximumRange||size>s.arenaBytes||
               size>std::numeric_limits<std::size_t>::max()){++s.counters.declines;return false;}
            const auto extent=(size+s.alignment-1)&~(s.alignment-1);
            if(extent>s.arenaBytes-end){++s.counters.declines;return false;}
            slices[i].offset=end;slices[i].range=size;end+=extent;
        }
        if(!s.makeArena(*c)){++s.counters.declines;return false;}
        if(c->batchCount==c->metadataCapacity||count>c->metadataCapacity-c->sliceCount){++s.counters.declines;return false;}
        const auto batchSerial=s.next();if(!batchSerial){++s.counters.declines;return false;}
        const auto batchIndex=c->batchCount++,firstSlice=c->sliceCount;c->sliceCount+=count;
        auto& metadata=c->batches[batchIndex];metadata={};metadata.first=firstSlice;metadata.count=count;
        for(unsigned i=0;i<count;++i)c->slices[firstSlice+i]={slices[i].offset,slices[i].range};
        const auto device=s.deviceGeneration,allocation=c->allocation,recording=c->recording;
        const auto index=static_cast<unsigned>(c-s.commands.data());
        const auto arena=c->arena;
        for(unsigned i=0;i<count;++i)slices[i].buffer=arena.buffer;
        c->used=end;c->copying=true;++s.copying;s.counters.reservedBytes+=end-start;
        lock.unlock();
        bool copied=true;VkDeviceSize copiedBytes=0;
        for(unsigned i=0;i<count;++i){
            auto* destination=static_cast<std::byte*>(arena.data)+slices[i].offset;
            if(!copy(context,requests[i].index,destination,slices[i].range)){copied=false;break;}
            copiedBytes+=slices[i].range;
        }
        lock.lock();
        // Slot remains reserved while copying: free is deferred and same-slot
        // reallocation/reentrant append cannot expose or overwrite these bytes.
        c=&s.commands[index];s.counters.copiedBytes+=copiedBytes;
        bool publish=copied&&s.live()&&s.deviceGeneration==device&&c->handle==h&&
            c->allocation==allocation&&c->recording==recording&&c->valid&&c->canRecord&&
            !c->retiring&&!c->poisoned&&c->thread==std::this_thread::get_id()&&c->used==end;
        if(publish&&!arena.coherent){
            VkMappedMemoryRange range{};range.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory=arena.memory;range.offset=start;range.size=end-start;
            if(!s.result(s.api.flushMemory(s.device,1,&range))){publish=false;++s.counters.flushFailures;}
        }
        c->copying=false;--s.copying;
        if(publish){
            out.owner_=this;out.device_=device;out.allocation_=allocation;out.recording_=recording;
            c->batches[batchIndex].serial=batchSerial;
            out.command_=index;out.batch_=batchIndex;out.batchSerial_=batchSerial;
            ++s.counters.batches;s.counters.slices+=count;
        }else ++s.counters.declines;
        if(s.terminal){if(!s.copying)s.forget();}
        else if(c->retiring)s.free(*c);
        return publish;
    }catch(...){return false;}
}
bool CommandUniformArena::resolve(const Ticket& t,Batch* out,bool recording)const noexcept {
    Errno preserve;if(out)*out={};
    try {if(!impl_||t.owner_!=this)return false;auto& s=*impl_;std::lock_guard lock(s.mutex);
        if(!s.live()||t.device_!=s.deviceGeneration||t.command_>=MaxCommands||!t.batchSerial_)return false;
        const auto& c=s.commands[t.command_];
        if(!c.handle||!c.valid||c.retiring||c.poisoned||t.allocation_!=c.allocation||t.recording_!=c.recording||
           !c.arena.buffer||(recording&&(!c.canRecord||c.copying||c.thread!=std::this_thread::get_id())))return false;
        if(!c.batches||!c.slices||t.batch_>=c.batchCount)return false;
        const auto& batch=c.batches[t.batch_];
        if(batch.serial!=t.batchSerial_||!batch.count||batch.count>MaxBatchSlices||
           batch.first>c.sliceCount||batch.count>c.sliceCount-batch.first)return false;
        for(unsigned i=0;i<batch.count;++i){const auto& q=c.slices[batch.first+i];
            if(q.offset>s.arenaBytes||!q.range||q.range>s.arenaBytes-q.offset||q.offset+q.range>c.used)return false;}
        if(out){out->device=s.device;out->command=c.handle;out->deviceGeneration=s.deviceGeneration;
            out->allocationGeneration=c.allocation;out->recordingGeneration=c.recording;out->count=batch.count;
            for(unsigned i=0;i<batch.count;++i){const auto& q=c.slices[batch.first+i];out->slices[i]={c.arena.buffer,q.offset,q.range};}}
        return true;
    }catch(...){return false;}
}
bool CommandUniformArena::Ticket::current()const noexcept{return owner_&&owner_->resolve(*this,nullptr,false);}
bool CommandUniformArena::Ticket::resolve(Batch& out)const noexcept {
    if(!owner_){out={};return false;}return owner_->resolve(*this,&out,false);
}
bool CommandUniformArena::Ticket::resolveForRecording(Batch& out)const noexcept {
    if(!owner_){out={};return false;}return owner_->resolve(*this,&out,true);
}
CommandUniformArena::Stats CommandUniformArena::stats()const noexcept {
    Errno preserve;
    try {if(!impl_)return {};auto& s=*impl_;std::lock_guard lock(s.mutex);auto result=s.counters;
        result.ready=s.live();result.deviceLost=s.lost;result.terminal=s.terminal;return result;
    }catch(...){return {};}
}
bool CommandUniformArena::shutdownAfterIdle(VkDevice d,VkResult idle)noexcept {
    Errno preserve;
    try {if(!impl_)return false;auto& s=*impl_;std::lock_guard lock(s.mutex);
        if(!d||d!=s.device||s.copying||(idle!=VK_SUCCESS&&idle!=VK_ERROR_DEVICE_LOST))return false;
        s.ready=false;for(auto& c:s.commands)if(c.handle)s.free(c);s.forget();return true;
    }catch(...){return false;}
}
void CommandUniformArena::abandonDevice(VkDevice d)noexcept {
    Errno preserve;
    try {if(!impl_)return;auto& s=*impl_;std::lock_guard lock(s.mutex);
        if(d&&d==s.device){s.terminal=true;s.ready=false;if(!s.copying)s.forget();}
    }catch(...){}
}
} // namespace rdr95
