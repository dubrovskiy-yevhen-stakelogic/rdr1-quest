#pragma once
#include "native_vertex_binding_batch58.h"
#include <cstddef>
#include <cstring>

namespace rdrvr::experimental {
// Exact ARM64 VkContext_c::BindInputLayout legacy stream loop. Its stock loop
// updates current buffer/offset before each driver call, then advances to slot8.
// Merge only consecutive dirty non-null streams from that live input context.
template<class Command,class Buffer,class Size,class Function>
class NativeVertexStreamMerge58 {
    using Counts=NativeVertexBindingBatch58<Command,Buffer,Size,Function>;
    Counts counts_;
    // The pinned BindInputLayout return site seen by a qualified scope (one fixed
    // address per process). bind() needs the thread's scope only for calls from that site.
    std::atomic<std::uintptr_t> site_{};
    static_assert(sizeof(Command)==8&&sizeof(Buffer)==8&&sizeof(Size)==8);
    template<class T> static T read(const void* p) noexcept {T value;std::memcpy(&value,p,sizeof value);return value;}
    template<class T> static void write(void* p,T value) noexcept {std::memcpy(p,&value,sizeof value);}
public:
    static constexpr unsigned streams=8;
    static constexpr std::size_t currentBuffers=0x32e8,currentOffsets=0x3328,desiredBuffers=0x33a8,desiredOffsets=0x33e8,currentFrame=0x30a0,frameCommand=0x38;
    class Scope;
private:
    inline static thread_local Scope* active_{};
public:
    class Scope {
        friend class NativeVertexStreamMerge58;
        NativeVertexStreamMerge58& owner_;
        Scope* previous_{};
        std::byte* context_{};
        std::uintptr_t callReturn_{};
        bool qualified_{},optimize_{},busy_{};
    public:
        Scope(NativeVertexStreamMerge58& owner,void* context,std::uintptr_t callReturn,bool ready,bool optimize) noexcept
            :owner_(owner),previous_(active_),context_(static_cast<std::byte*>(context)),callReturn_(callReturn),
             qualified_(ready&&context&&callReturn&&!previous_&&owner.counts_.enabled()),optimize_(optimize) {
            if(previous_)previous_->qualified_=false;
            if(qualified_)owner.site_.store(callReturn,std::memory_order_relaxed);
            active_=this;
        }
        ~Scope(){if(active_==this)active_=previous_;}
        Scope(const Scope&)=delete;
    };
    void enable() noexcept {counts_.enable();}
    void disable() noexcept {if(auto* s=active_)s->qualified_=false;counts_.disable();}
    void publish() noexcept {counts_.publish();}
    auto snapshot() const noexcept {return counts_.snapshot();}
    std::string report() const {return counts_.report();}
    void bind(Function next,std::uintptr_t callReturn,Command command,unsigned first,unsigned count,const Buffer* buffers,const Size* offsets) noexcept {
        // Binds outside the pinned BindInputLayout site never qualify: no TLS access.
        if(callReturn!=site_.load(std::memory_order_relaxed)){next(command,first,count,buffers,offsets);return;}
        auto* s=active_;
        if(!s||&s->owner_!=this||!s->qualified_||s->busy_||!counts_.enabled()||callReturn!=s->callReturn_||!command||count!=1||first>=streams||
           buffers!=reinterpret_cast<const Buffer*>(s->context_+currentBuffers+first*8)||
           offsets!=reinterpret_cast<const Size*>(s->context_+currentOffsets+first*8)) {
            next(command,first,count,buffers,offsets);return;
        }
        // All pointers below are within the already-live exact native argument.
        // No cached native pointer or desired binding survives this call.
        auto* frame=read<std::byte*>(s->context_+currentFrame);
        if(!frame||read<Command>(frame+frameCommand)!=command) {next(command,first,count,buffers,offsets);return;}
        unsigned merged=1;
        if(s->optimize_) {
            const auto firstBuffer=read<Buffer>(buffers);
            if(!firstBuffer||firstBuffer!=read<Buffer>(s->context_+desiredBuffers+first*8)||
               read<Size>(offsets)!=read<Size>(s->context_+desiredOffsets+first*8)) {
                next(command,first,count,buffers,offsets);return;
            }
            for(unsigned slot=first+1;slot<streams;++slot) {
                const auto b=read<Buffer>(s->context_+desiredBuffers+slot*8);
                const auto o=read<Size>(s->context_+desiredOffsets+slot*8);
                auto* current=s->context_+currentBuffers+slot*8;
                auto* currentOffset=s->context_+currentOffsets+slot*8;
                if(!b||(read<Buffer>(current)==b&&read<Size>(currentOffset)==o))break;
                write(current,b);write(currentOffset,o);++merged;
            }
        }
        // Immediate raw call: no queue, new recording lifetime or consumer
        // interception. Following stock loop slots now observe equal fields.
        s->busy_=true;next(command,first,merged,buffers,offsets);s->busy_=false;
        // Only real merges are counted (a merged==1 group adds 1 source and 1 emitted
        // call, i.e. nothing to sourceCalls-emittedCalls); inputScopes counts merged groups.
        if(merged>1)counts_.recordGroup(merged,1,merged,merged);
    }
};
}
