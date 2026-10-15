#include "ColdOcclusionRegistry.h"
#include <array>
#include <atomic>
#include <cerrno>
#include <mutex>
#include <limits>
#include <type_traits>
#include <unordered_map>

namespace rdrvr::engine {
namespace {
struct Errno {int value{errno};~Errno(){errno=value;}};
template<class T> std::uint64_t key(T handle) noexcept {
    if constexpr(std::is_pointer_v<T>)return reinterpret_cast<std::uintptr_t>(handle);
    else return std::uint64_t(handle);
}
unsigned hash(std::uint64_t value) noexcept {
    value^=value>>33;value*=0xff51afd7ed558ccdull;value^=value>>33;
    return unsigned(value)&(ColdOcclusionRegistry::tableCapacity-1);
}
}
struct ColdOcclusionRegistry::Impl {
    using C=StaticOcclusionCertificate;
    struct Shader {C::Shader proof{};std::uint64_t generation{};};
    struct Record {Snapshot snapshot{};std::uint64_t device{},incarnation{};};
    struct ShaderRecord {ShaderSnapshot snapshot{};};
    struct ShaderSlot {std::atomic<std::uint64_t> handle{};std::atomic<const ShaderRecord*> record{};};
    struct Slot {std::atomic<std::uint64_t> handle{};std::atomic<const Record*> record{};};
    static_assert(std::atomic<const Record*>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    mutable std::mutex mutex;
    std::atomic<std::uint64_t> device{},epoch{},incarnation{};
    std::atomic<std::uint64_t> shaderRevision{};
    // Cold writers already hold mutex. Odd means publication in progress.
    // Saturation permanently revokes rather than permitting serial ABA.
    struct ShaderMutation {
        std::atomic<std::uint64_t>& revision;std::uint64_t before;
        explicit ShaderMutation(Impl& s) noexcept :revision(s.shaderRevision),before(revision.load(std::memory_order_seq_cst)) {
            const auto maximum=std::numeric_limits<std::uint64_t>::max();
            if(before>=maximum-2){before=maximum;revision.store(maximum,std::memory_order_seq_cst);}
            else revision.store(before+1,std::memory_order_seq_cst);
        }
        ~ShaderMutation(){if(before!=std::numeric_limits<std::uint64_t>::max())revision.store(before+2,std::memory_order_seq_cst);}
    };
    std::array<Slot,tableCapacity> table{};
    std::array<ShaderSlot,tableCapacity> shaderTable{};
    std::array<std::unique_ptr<ShaderRecord>,recordCapacity> shaderRecords{};
    std::uint64_t shaderRetained{},shaderLive{},shaderDeclined{};
    // Published records never mutate or move, including after clear/destruction.
    // Bounded retention avoids a hot refcount and cross-thread read UAF. The
    // registry itself must outlive its callers, as every renderer owner does.
    std::array<std::unique_ptr<Record>,recordCapacity> records{};
    std::unordered_map<std::uint64_t,Shader> shaders;
    std::uint64_t generation{},retained{},live{},declined{},inspected{},unknown{};
    std::array<std::uint64_t,unsigned(C::PipelineReason::Count)> reasons{};
    Slot* slot(std::uint64_t handle,bool insert) noexcept {
        auto at=hash(handle);
        for(unsigned i=0;i<maxProbe;++i,at=(at+1)&(tableCapacity-1)){
            auto& s=table[at];const auto current=s.handle.load(std::memory_order_relaxed);
            if(current==handle)return &s;
            if(!current){if(!insert)return nullptr;s.handle.store(handle,std::memory_order_release);return &s;}
        }return nullptr;
    }
    void erase(std::uint64_t handle) noexcept {
        if(auto* s=slot(handle,false))if(s->record.exchange(nullptr,std::memory_order_acq_rel))--live;
        // Keep tombstone keys: removing a key would break collision probing.
    }
    ShaderSlot* shaderSlot(std::uint64_t handle,bool insert) noexcept {
        auto at=hash(handle);
        for(unsigned i=0;i<maxProbe;++i,at=(at+1)&(tableCapacity-1)){
            auto& s=shaderTable[at];const auto current=s.handle.load(std::memory_order_relaxed);
            if(current==handle)return &s;
            if(!current){if(!insert)return nullptr;s.handle.store(handle,std::memory_order_release);return &s;}
        }return nullptr;
    }
    void eraseShader(std::uint64_t handle) noexcept {
        if(auto* s=shaderSlot(handle,false))if(s->record.exchange(nullptr,std::memory_order_acq_rel))--shaderLive;
    }
    void reset() noexcept {
        for(auto& s:table){s.record.store(nullptr,std::memory_order_release);s.handle.store(0,std::memory_order_release);}
        for(auto& s:shaderTable){s.record.store(nullptr,std::memory_order_release);s.handle.store(0,std::memory_order_release);}
        shaders.clear();live=0;shaderLive=0;
    }
};
ColdOcclusionRegistry::ColdOcclusionRegistry():impl_(std::make_unique<Impl>()){}
ColdOcclusionRegistry::~ColdOcclusionRegistry()=default;
bool ColdOcclusionRegistry::bind(VkDevice device,std::uint64_t epoch) noexcept {
    Errno preserve;if(!device||!epoch)return false;
    try {auto& s=*impl_;std::lock_guard lock(s.mutex);const auto current=s.epoch.load(std::memory_order_relaxed);
        if(current)return current==epoch&&s.device.load(std::memory_order_relaxed)==key(device);
        Impl::ShaderMutation changing(s);
        s.reset();s.device.store(key(device),std::memory_order_relaxed);
        s.incarnation.fetch_add(1,std::memory_order_release);s.epoch.store(epoch,std::memory_order_release);return true;
    }catch(...){return false;}
}
void ColdOcclusionRegistry::clear(VkDevice device) noexcept {
    Errno preserve;try{auto& s=*impl_;std::lock_guard lock(s.mutex);
        if(!device||s.device.load(std::memory_order_relaxed)!=key(device))return;
        Impl::ShaderMutation changing(s);
        s.epoch.store(0,std::memory_order_release);s.incarnation.fetch_add(1,std::memory_order_release);
        s.reset();s.device.store(0,std::memory_order_release);
    }catch(...){}
}
void ColdOcclusionRegistry::createdShader(VkDevice device,const VkShaderModuleCreateInfo* ci,VkShaderModule module,VkResult result) noexcept {
    Errno preserve;if(result!=VK_SUCCESS||!module||!impl_->epoch.load(std::memory_order_acquire))return;
    try{auto& s=*impl_;std::lock_guard lock(s.mutex);
        if(!s.epoch.load(std::memory_order_relaxed)||s.device.load(std::memory_order_relaxed)!=key(device))return;
        Impl::ShaderMutation changing(s);
        s.eraseShader(key(module));
        s.shaders.erase(key(module));
        if(s.shaders.size()>=shaderCapacity){++s.declined;++s.shaderDeclined;return;}
        ++s.inspected;auto proof=StaticOcclusionCertificate::shader(ci);
        const auto generation=++s.generation;
        s.shaders.emplace(key(module),Impl::Shader{proof,generation});
        auto* slot=s.shaderSlot(key(module),true);
        if(!slot||s.shaderRetained==recordCapacity){++s.shaderDeclined;return;}
        ShaderSnapshot snapshot{s.epoch.load(std::memory_order_relaxed),s.incarnation.load(std::memory_order_relaxed),generation,
            reinterpret_cast<std::uintptr_t>(&s),device,module,proof};
        auto record=std::make_unique<Impl::ShaderRecord>(Impl::ShaderRecord{snapshot});
        const auto* publication=record.get();s.shaderRecords[s.shaderRetained++]=std::move(record);
        slot->record.store(publication,std::memory_order_release);++s.shaderLive;
    }catch(...){try{auto& s=*impl_;std::lock_guard lock(s.mutex);++s.declined;}catch(...){} }
}
void ColdOcclusionRegistry::destroyedShader(VkDevice device,VkShaderModule module) noexcept {
    Errno preserve;try{auto& s=*impl_;std::lock_guard lock(s.mutex);
        if(device&&module&&s.device.load(std::memory_order_relaxed)==key(device)){
            Impl::ShaderMutation changing(s);s.eraseShader(key(module));s.shaders.erase(key(module));}
    }catch(...){}
}
void ColdOcclusionRegistry::createdPipelines(VkDevice device,std::uint32_t count,const VkGraphicsPipelineCreateInfo* ci,const VkPipeline* handles,VkResult) noexcept {
    Errno preserve;if(!count||!handles||!impl_->epoch.load(std::memory_order_acquire))return;
    try{auto& s=*impl_;std::lock_guard lock(s.mutex);const auto epoch=s.epoch.load(std::memory_order_relaxed);
        if(!epoch||s.device.load(std::memory_order_relaxed)!=key(device))return;
        // Invalid/unbounded borrowed arrays cannot leave old certificates live.
        if(!ci||count>1024){for(auto& p:s.table)p.record.store(nullptr,std::memory_order_release);s.live=0;++s.declined;return;}
        // Invalidate the complete returned batch before any throwing allocation.
        // A partial host failure must not preserve a replaced later handle.
        for(unsigned i=0;i<count;++i)if(handles[i])s.erase(key(handles[i]));
        for(unsigned i=0;i<count;++i){if(!handles[i])continue;const auto handle=key(handles[i]);
            // Even malformed duplicate batch outputs must not double-count a
            // live publication or retain the first proof after a later failure.
            s.erase(handle);
            auto* slot=s.slot(handle,true);if(!slot||s.retained==recordCapacity){++s.declined;continue;}
            Snapshot snapshot{};snapshot.deviceEpoch=epoch;snapshot.pipelineGeneration=++s.generation;snapshot.pipeline=handles[i];
            StaticOcclusionCertificate::Shader vertex{},fragment{};
            const auto& create=ci[i];
            if(create.sType==VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO&&create.stageCount==2&&create.pStages){
                for(unsigned j=0;j<2;++j){const auto& stage=create.pStages[j];
                    const auto found=s.shaders.find(key(stage.module));if(found==s.shaders.end())continue;
                    if(stage.stage==VK_SHADER_STAGE_VERTEX_BIT){vertex=found->second.proof;snapshot.vertexShaderGeneration=found->second.generation;}
                    if(stage.stage==VK_SHADER_STAGE_FRAGMENT_BIT){fragment=found->second.proof;snapshot.fragmentShaderGeneration=found->second.generation;}
                }
            }
            snapshot.certificate=StaticOcclusionCertificate::pipeline(create,vertex,fragment);
            if(!snapshot.certificate.candidateSafe)++s.unknown;
            ++s.reasons[unsigned(snapshot.certificate.reason)];
            // Allocation failure leaves this handle absent, never an old proof.
            auto record=std::make_unique<Impl::Record>(Impl::Record{snapshot,key(device),s.incarnation.load(std::memory_order_relaxed)});
            const auto* publication=record.get();s.records[s.retained++]=std::move(record);
            slot->record.store(publication,std::memory_order_release);++s.live;
        }
    }catch(...){try{auto& s=*impl_;std::lock_guard lock(s.mutex);++s.declined;}catch(...){} }
}
void ColdOcclusionRegistry::destroyedPipeline(VkDevice device,VkPipeline pipeline) noexcept {
    Errno preserve;try{auto& s=*impl_;std::lock_guard lock(s.mutex);
        if(s.device.load(std::memory_order_relaxed)==key(device))s.erase(key(pipeline));
    }catch(...){}
}
bool ColdOcclusionRegistry::query(VkDevice device,VkPipeline pipeline,Snapshot& out) const noexcept {
    out={};if(!device||!pipeline)return false;const auto& s=*impl_;
    const auto epoch=s.epoch.load(std::memory_order_acquire);if(!epoch)return false;
    const auto incarnation=s.incarnation.load(std::memory_order_acquire),handle=key(pipeline),owner=key(device);
    if(s.device.load(std::memory_order_acquire)!=owner)return false;
    auto at=hash(handle);
    for(unsigned i=0;i<maxProbe;++i,at=(at+1)&(tableCapacity-1)){
        const auto& slot=s.table[at];const auto observed=slot.handle.load(std::memory_order_acquire);
        if(!observed)return false;if(observed!=handle)continue;
        const auto* record=slot.record.load(std::memory_order_acquire);
        if(!record||record->device!=owner||record->incarnation!=incarnation||record->snapshot.deviceEpoch!=epoch||key(record->snapshot.pipeline)!=handle)return false;
        const auto copy=record->snapshot;
        if(slot.record.load(std::memory_order_acquire)!=record||s.epoch.load(std::memory_order_acquire)!=epoch||
           s.incarnation.load(std::memory_order_acquire)!=incarnation||s.device.load(std::memory_order_acquire)!=owner)return false;
        out=copy;return true;
    }return false;
}
ColdOcclusionRegistry::Statistics ColdOcclusionRegistry::statistics() const noexcept {
    Errno preserve;try{const auto& s=*impl_;std::lock_guard lock(s.mutex);
        return {s.epoch.load(std::memory_order_relaxed),s.shaders.size(),s.live,s.retained,s.declined,s.inspected,s.unknown,
            sizeof(Impl)+s.retained*sizeof(Impl::Record)+s.shaders.size()*sizeof(Impl::Shader)+s.shaderRetained*sizeof(Impl::ShaderRecord),
            sizeof(Impl)+recordCapacity*sizeof(Impl::Record)+shaderCapacity*sizeof(Impl::Shader)+recordCapacity*sizeof(Impl::ShaderRecord),s.reasons};
    }catch(...){return {};}
}
bool ColdOcclusionRegistry::queryShader(VkDevice device,VkShaderModule module,ShaderSnapshot& out) const noexcept {
    out={};if(!device||!module)return false;const auto& s=*impl_;
    const auto epoch=s.epoch.load(std::memory_order_acquire);if(!epoch)return false;
    const auto incarnation=s.incarnation.load(std::memory_order_acquire),handle=key(module),owner=key(device);
    if(s.device.load(std::memory_order_acquire)!=owner)return false;
    auto at=hash(handle);
    for(unsigned i=0;i<maxProbe;++i,at=(at+1)&(tableCapacity-1)){
        const auto& slot=s.shaderTable[at];const auto observed=slot.handle.load(std::memory_order_acquire);
        if(!observed)return false;if(observed!=handle)continue;
        const auto* record=slot.record.load(std::memory_order_acquire);if(!record)return false;
        const auto& value=record->snapshot;
        if(key(value.device)!=owner||value.incarnation!=incarnation||value.deviceEpoch!=epoch||key(value.module)!=handle)return false;
        const auto copy=value;
        if(slot.record.load(std::memory_order_acquire)!=record||s.epoch.load(std::memory_order_acquire)!=epoch||
            s.incarnation.load(std::memory_order_acquire)!=incarnation||s.device.load(std::memory_order_acquire)!=owner)return false;
        out=copy;return true;
    }return false;
}
bool ColdOcclusionRegistry::currentShader(const ShaderSnapshot& value) const noexcept {
    const auto& s=*impl_;
    if(!value.device||!value.module||!value.shaderGeneration||value.registryIdentity!=reinterpret_cast<std::uintptr_t>(&s))return false;
    const auto owner=key(value.device),handle=key(value.module),epoch=value.deviceEpoch,incarnation=value.incarnation;
    if(!epoch||s.epoch.load(std::memory_order_acquire)!=epoch||s.incarnation.load(std::memory_order_acquire)!=incarnation||s.device.load(std::memory_order_acquire)!=owner)return false;
    auto at=hash(handle);
    for(unsigned i=0;i<maxProbe;++i,at=(at+1)&(tableCapacity-1)){
        const auto& slot=s.shaderTable[at];const auto observed=slot.handle.load(std::memory_order_acquire);
        if(!observed)return false;if(observed!=handle)continue;
        const auto* record=slot.record.load(std::memory_order_acquire);if(!record)return false;
        const auto& v=record->snapshot;
        if(v.shaderGeneration!=value.shaderGeneration||v.incarnation!=incarnation||v.deviceEpoch!=epoch||key(v.device)!=owner||key(v.module)!=handle)return false;
        return slot.record.load(std::memory_order_acquire)==record&&s.epoch.load(std::memory_order_acquire)==epoch&&
            s.incarnation.load(std::memory_order_acquire)==incarnation&&s.device.load(std::memory_order_acquire)==owner;
    }return false;
}
ColdOcclusionRegistry::ShaderStatistics ColdOcclusionRegistry::shaderStatistics() const noexcept {
    Errno preserve;try{const auto& s=*impl_;std::lock_guard lock(s.mutex);
        return {s.shaderLive,s.shaderRetained,s.shaderDeclined,sizeof(s.shaderTable)+sizeof(s.shaderRecords)+s.shaderRetained*sizeof(Impl::ShaderRecord),
            sizeof(s.shaderTable)+sizeof(s.shaderRecords)+recordCapacity*sizeof(Impl::ShaderRecord)};
    }catch(...){return {};}
}

std::uint64_t ColdOcclusionRegistry::shaderMutationSerial() const noexcept {
    const auto& s=*impl_;const auto serial=s.shaderRevision.load(std::memory_order_seq_cst);
    if(!serial||(serial&1)||!s.epoch.load(std::memory_order_acquire))return 0;
    return s.shaderRevision.load(std::memory_order_seq_cst)==serial?serial:0;
}
bool ColdOcclusionRegistry::currentShaderSet(std::uint64_t serial) const noexcept {
    return serial&&!(serial&1)&&shaderMutationSerial()==serial;
}

}
