#include "UniformCaptureSink.h"
#include <algorithm>
#include <cerrno>
#include <limits>
#include <new>

namespace rdr95 {
namespace {struct Errno {int value=errno;~Errno(){errno=value;}};}
struct UniformCaptureSink::Impl {
    CommandUniformArena& arena;CommandUniformSets& sets;RecordingThread ownsThread{};
    // invalidate may be delivered by lifecycle/worker callbacks. It never
    // waits for the synchronous copy or accesses the recording-thread arrays.
    std::atomic<std::uint64_t> epoch{1};bool busy{};
    struct Definition {
        std::uint64_t owner{},generation{};VkDescriptorSetLayout native{};
        CommandUniformSets::Layout owned{};
    };
    struct Current {VkCommandBuffer command{};std::uint64_t epoch{};Ticket value{};};
    std::array<Definition,MaxLayouts> layouts{};unsigned layoutCount{};
    std::array<Current,MaxCommands> commands{};
    const DUD::CaptureBatch* pendingIdentity{};std::uint64_t pendingEpoch{};Ticket pending{};
    Impl(CommandUniformArena& a,CommandUniformSets& s,RecordingThread p):arena(a),sets(s),ownsThread(p){}
    bool owner()const noexcept{return ownsThread&&ownsThread();}
    void revoke()noexcept {
        auto n=epoch.load(std::memory_order_relaxed);
        while(n&& !epoch.compare_exchange_weak(n,n==UINT64_MAX?0:n+1,std::memory_order_acq_rel)){}
    }
    Current* find(VkCommandBuffer c)noexcept {
        const auto now=epoch.load(std::memory_order_acquire);
        Current* empty=nullptr;Current* stale=nullptr;
        for(auto& slot:commands){
            if(slot.command==c)return &slot;
            if(!slot.command&&!empty)empty=&slot;
            if(slot.epoch!=now&&!stale)stale=&slot;
        }
        // Only CPU "currently bound" state is replaced. Already returned
        // tickets own their copied arena/descriptor identities independently.
        return empty?empty:stale;
    }
    bool layout(const DUD::CaptureCommand& command,const DUD::CaptureSet& set,
                std::span<const DUD::CaptureRange> ranges,CommandUniformSets::Layout& out)noexcept {
        for(unsigned i=0;i<layoutCount;++i){const auto& l=layouts[i];
            if(l.owner==command.owner&&l.generation==set.generation&&l.native==set.layout){out=l.owned;return true;}}
        if(layoutCount==MaxLayouts||ranges.empty()||ranges.size()>DUD::slots)return false;
        std::array<VkDescriptorSetLayoutBinding,DUD::slots> bindings{};
        std::array<VkDescriptorBindingFlags,DUD::slots> flags{};
        for(unsigned i=0;i<ranges.size();++i){const auto& r=ranges[i];
            if(r.arrayElement||r.binding>=DUD::slots||(i&&r.binding<=ranges[i-1].binding)||r.descriptorLayout!=set.layout||
               r.layoutGeneration!=set.generation||r.descriptor!=set.descriptor||
               (r.flags&~VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT))return false;
            bindings[i]={r.binding,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1,r.stages,nullptr};flags[i]=r.flags;}
        VkDescriptorSetLayoutBindingFlagsCreateInfo f{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        f.bindingCount=static_cast<unsigned>(ranges.size());f.pBindingFlags=flags.data();
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount=f.bindingCount;ci.pBindings=bindings.data();ci.pNext=&f;
        CommandUniformSets::Layout value;if(!sets.createLayout(ci,value))return false;
        layouts[layoutCount++]={command.owner,set.generation,set.layout,value};out=value;return true;
    }
};
UniformCaptureSink::UniformCaptureSink(CommandUniformArena& a,CommandUniformSets& s,RecordingThread p)noexcept
    :impl_(new(std::nothrow) Impl(a,s,p)){}
UniformCaptureSink::~UniformCaptureSink()=default;
UniformCaptureSink::DUD::CaptureObserver UniformCaptureSink::observer()noexcept{return {this,prepare,finish,invalidate};}
bool UniformCaptureSink::prepare(void* context,const DUD::CaptureBatch& batch)noexcept {
    Errno keep;auto& self=*static_cast<UniformCaptureSink*>(context);if(!self.impl_)return false;auto& s=*self.impl_;
    if(!s.owner()||!batch.current()){s.revoke();return false;}
    if(s.busy){s.revoke();return false;}s.busy=true;
    struct Exit {Impl& s;~Exit(){s.busy=false;}} exit{s};
    s.pendingIdentity=nullptr;s.pending={};const auto epoch=s.epoch.load(std::memory_order_acquire);if(!epoch)return false;
    const auto command=batch.command();const auto ranges=batch.ranges();const auto nativeSets=batch.boundSets();const auto first=batch.firstSet();
    if(!command.device||!command.pool||!command.command||!command.owner||!command.allocation||!command.recording||
       !batch.pipelineLayout()||nativeSets.empty()||first>=DUD::sets||nativeSets.size()>DUD::sets-first||ranges.size()>DUD::slots*DUD::sets||!s.find(command.command))return false;
    std::array<CommandUniformArena::CopySource,DUD::slots*DUD::sets> copies{};
    std::array<std::uint8_t,DUD::slots*DUD::sets> copied{};copied.fill(CommandUniformSets::Absent);unsigned count{};
    for(unsigned i=0;i<ranges.size();++i){const auto& r=ranges[i];
        if(!r.present)continue;
        if(!r.range||r.range>std::numeric_limits<std::size_t>::max()||r.baseOffset||r.effectiveOffset>UINT32_MAX)return false;
        copied[i]=static_cast<std::uint8_t>(count);copies[count++]={r.range,i};}
    if(!count)return false;
    CommandUniformArena::Ticket bytes;
    struct CopyContext {const DUD::CaptureBatch* batch;};CopyContext copyContext{&batch};
    const auto copier=[](void* p,unsigned index,void* out,VkDeviceSize size)noexcept {
        const auto& c=*static_cast<CopyContext*>(p);
        return size<=std::numeric_limits<std::size_t>::max()&&c.batch->copy(index,out,static_cast<std::size_t>(size));};
    if(!s.arena.appendCopied(command.command,{copies.data(),count},&copyContext,copier,bytes)||!batch.current())return false;
    Ticket value;value.command_=command;value.layout_=batch.pipelineLayout();
    for(unsigned i=0;i<nativeSets.size();++i){const auto& set=nativeSets[i];if(!set.converted)continue;
        if(!set.descriptor||!set.layout||!set.generation||!set.rangeCount||set.rangeCount>DUD::slots||
           set.firstRange>ranges.size()||set.rangeCount>ranges.size()-set.firstRange)return false;
        const auto selected=ranges.subspan(set.firstRange,set.rangeCount);CommandUniformSets::Layout layout;
        if(!s.layout(command,set,selected,layout))return false;
        auto& source=value.sources_[first+i];source.native=set.descriptor;source.layout=set.layout;source.layoutGeneration=set.generation;source.count=set.rangeCount;
        std::array<std::uint8_t,DUD::slots> mapping{};
        for(unsigned j=0;j<set.rangeCount;++j){const auto& r=selected[j];
            if(r.set!=first+i||r.descriptor!=set.descriptor||r.descriptorLayout!=set.layout||r.layoutGeneration!=set.generation||r.arrayElement||r.baseOffset||
               r.effectiveOffset>UINT32_MAX||(!r.present&&(!(r.flags&VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT)||r.range)))return false;
            source.offsets[j]=static_cast<std::uint32_t>(r.effectiveOffset);mapping[j]=copied[set.firstRange+j];}
        if(!s.sets.prepare(layout,bytes,{mapping.data(),set.rangeCount},value.sets_[first+i]))return false;
        value.mask_|=1u<<(first+i);
    }
    if(!value.mask_||!batch.current()||s.epoch.load(std::memory_order_acquire)!=epoch)return false;
    s.pending=std::move(value);s.pendingIdentity=&batch;s.pendingEpoch=epoch;return true;
}
void UniformCaptureSink::finish(void* context,const DUD::CaptureBatch& batch,bool committed)noexcept {
    Errno keep;auto& self=*static_cast<UniformCaptureSink*>(context);if(!self.impl_)return;auto& s=*self.impl_;
    if(!s.owner()){s.revoke();return;}
    const bool match=s.pendingIdentity==&batch&&s.pending.command_==batch.command()&&s.pending.layout_==batch.pipelineLayout();
    const auto epoch=s.epoch.load(std::memory_order_acquire);
    if(committed&&match&&epoch&&epoch==s.pendingEpoch){
        bool valid=true;
        for(unsigned i=0;i<DUD::sets;++i)if(s.pending.mask_&(1u<<i)){CommandUniformSets::Binding b;
            valid&=s.pending.sets_[i].resolveForRecording(b)&&b.device==s.pending.command_.device&&b.command==s.pending.command_.command;}
        if(auto* slot=s.find(s.pending.command_.command);valid&&slot){
            // A partial native bind may disturb an old layout prefix. Retain
            // only sets explicitly present in this newly observed bind.
            slot->command=s.pending.command_.command;slot->epoch=epoch;slot->value=s.pending;
        }else s.revoke();
    }else s.revoke();
    s.pendingIdentity=nullptr;s.pending={};s.pendingEpoch=0;
}
void UniformCaptureSink::invalidate(void* context,const DUD::CaptureCommand&)noexcept {
    Errno keep;auto& self=*static_cast<UniformCaptureSink*>(context);if(self.impl_)self.impl_->revoke();
}
bool UniformCaptureSink::current(VkCommandBuffer command,unsigned required,Ticket& out)const noexcept {
    Errno keep;if(!impl_)return false;const auto& s=*impl_;
    if(!s.owner()||s.busy||!command||!required||(required&~((1u<<DUD::sets)-1)))return false;
    const auto epoch=s.epoch.load(std::memory_order_acquire);if(!epoch)return false;
    for(const auto& c:s.commands)if(c.command==command){
        if(c.epoch!=epoch||(c.value.mask_&required)!=required)return false;
        for(unsigned i=0;i<DUD::sets;++i)if(required&(1u<<i)){SourceBinding native;CommandUniformSets::Binding own;if(!c.value.resolve(i,native,own))return false;}
        if(s.epoch.load(std::memory_order_acquire)!=epoch)return false;out=c.value;return true;
    }
    return false;
}
bool UniformCaptureSink::Ticket::resolve(unsigned set,SourceBinding& source,CommandUniformSets::Binding& binding)const noexcept {
    Errno keep;if(set>=DUD::sets||!(mask_&(1u<<set)))return false;
    CommandUniformSets::Binding b;if(!sets_[set].resolveForRecording(b)||b.device!=command_.device||b.command!=command_.command)return false;
    source=sources_[set];binding=b;return true;
}
}
