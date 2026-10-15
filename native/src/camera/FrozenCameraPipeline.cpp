#include "FrozenCameraPipeline.h"
#include "MultiviewGraphicsPlan.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace rdrvr::engine {
namespace {
struct Errno {int* cell=&errno;int value=*cell;~Errno(){*cell=value;}};
struct Descriptor {std::uint32_t set{},binding{};bool hasSet{},hasBinding{},variable{};};
bool add(std::uint64_t& count,std::uint32_t n,std::uint32_t limit) noexcept {
    count+=n;return count<=limit;
}
bool stageInterface(std::span<const std::uint32_t> w,VkShaderStageFlagBits stage,
                    unsigned setCount,std::string_view expectedEntry,bool fragmentCamera) noexcept {
    if(w.size()<5||w.size()>131072||w[0]!=0x07230203||w[1]!=0x00010300||
       !w[3]||w[3]>1048576||w[4]||expectedEntry.empty()||expectedEntry.size()>=128||
       (stage!=VK_SHADER_STAGE_VERTEX_BIT&&stage!=VK_SHADER_STAGE_FRAGMENT_BIT))return false;
    try {
        std::unordered_map<std::uint32_t,Descriptor> descriptors;
        unsigned entries=0,memoryModels=0;
        for(std::size_t i=5;i<w.size();){
            const auto count=w[i]>>16,op=w[i]&65535u;
            if(!count||count>w.size()-i)return false;
            auto valid=[&](std::uint32_t id){return id&&id<w[3];};
            if(op==14){if(count!=3||w[i+1]!=0||w[i+2]!=1||++memoryModels!=1)return false;}
            if(op==15){
                if(count<4||++entries!=1||w[i+1]!=(stage==VK_SHADER_STAGE_VERTEX_BIT?0u:4u)||!valid(w[i+2]))return false;
                const auto* name=reinterpret_cast<const char*>(w.data()+i+3);
                const auto capacity=std::size_t(count-3)*4;
                const auto* end=static_cast<const char*>(std::memchr(name,0,capacity));
                if(!end||std::string_view(name,static_cast<std::size_t>(end-name))!=expectedEntry)return false;
            }
            if(op==73||op==74||op==75||op==332)return false; // no indirect/id decorations
            if(op==71){
                if(count<3||!valid(w[i+1]))return false;
                if(w[i+2]==33||w[i+2]==34){
                    if(count!=4)return false;
                    auto& d=descriptors[w[i+1]];
                    if(w[i+2]==33){if(d.hasBinding)return false;d.binding=w[i+3];d.hasBinding=true;}
                    else {if(d.hasSet)return false;d.set=w[i+3];d.hasSet=true;}
                }
            }
            if(op==59){
                if(count<4||count>5||!valid(w[i+1])||!valid(w[i+2]))return false;
                const auto storage=w[i+3];
                if(storage==0||storage==2||storage==12){
                    auto& d=descriptors[w[i+2]];if(d.variable)return false;d.variable=true;
                }
            }
            i+=count;
        }
        if(entries!=1||memoryModels!=1)return false;
        std::vector<std::uint64_t> used;
        for(const auto& pair:descriptors){const auto& d=pair.second;
            if(!d.variable||!d.hasSet||!d.hasBinding||d.set>=setCount||d.set==3||
               (!fragmentCamera&&stage==VK_SHADER_STAGE_FRAGMENT_BIT&&d.set==0&&d.binding==1))return false;
            const auto key=(std::uint64_t(d.set)<<32)|d.binding;
            if(std::find(used.begin(),used.end(),key)!=used.end())return false;
            used.push_back(key);
        }
        return true;
    }catch(...){return false;}
}
}
bool frozenCameraStageInterface(std::span<const std::uint32_t> w,VkShaderStageFlagBits stage,
                                unsigned setCount,std::string_view expectedEntry) noexcept {
    // Public/generic validation never accepts an unconverted native FS camera.
    return stageInterface(w,stage,setCount,expectedEntry,false);
}

FrozenCameraPipelineFactory::Api::operator bool() const noexcept {
    return createShader&&destroyShader&&createSetLayout&&destroySetLayout&&createLayout&&destroyLayout&&createPipelines&&destroyPipeline;
}
struct FrozenCameraPipelineFactory::Impl {
    static constexpr unsigned capacity=4096;
    VkDevice device{};std::uint64_t deviceGeneration{},cameraGeneration{},serial{};
    VkDescriptorSetLayout camera{},empty{};Api api{};Limits limits{};bool initialized{};
    mutable std::mutex mutex;
    struct Record {Variant value;bool selectable{true};};
    std::vector<Record> records;
    std::unordered_map<VkPipeline,std::size_t> active;
    void clear(bool destroy) noexcept {
        if(destroy){
            for(auto& r:records){api.destroyPipeline(device,r.value.pipeline,nullptr);api.destroyLayout(device,r.value.layout,nullptr);api.destroyLayout(device,r.value.restoreLayout,nullptr);}
            if(empty)api.destroySetLayout(device,empty,nullptr);
        }
        active.clear();records.clear();empty={};camera={};device={};
    }
};
FrozenCameraPipelineFactory::FrozenCameraPipelineFactory():impl_(std::make_unique<Impl>()){}
FrozenCameraPipelineFactory::~FrozenCameraPipelineFactory()=default;
bool FrozenCameraPipelineFactory::initialize(VkDevice device,std::uint64_t generation,Api api,Limits limits,
                                             VkDescriptorSetLayout camera,std::uint64_t cameraGeneration) noexcept {
    Errno preserve;std::lock_guard lock(impl_->mutex);
    if(impl_->initialized||!device||!generation||!api||!camera||!cameraGeneration||
       limits.maxBoundSets<4||!limits.maxVertexUniforms||!limits.maxVertexResources||!limits.maxSetUniforms||
       !limits.maxFragmentUniforms||!limits.maxFragmentResources)return false;
    try {impl_->records.reserve(Impl::capacity);impl_->active.reserve(Impl::capacity);}catch(...){return false;}
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkDescriptorSetLayout empty{};
    if(api.createSetLayout(device,&ci,nullptr,&empty)!=VK_SUCCESS||!empty)return false;
    impl_->device=device;impl_->deviceGeneration=generation;impl_->cameraGeneration=cameraGeneration;
    impl_->api=api;impl_->limits=limits;impl_->camera=camera;impl_->empty=empty;impl_->initialized=true;return true;
}
bool FrozenCameraPipelineFactory::create(const Input& input,Variant& output) noexcept {
    Errno preserve;std::lock_guard lock(impl_->mutex);auto& state=*impl_;
    if(!state.device||!input.nativePipeline||!input.pipeline||!input.layout||
       state.records.size()==Impl::capacity||state.active.count(input.nativePipeline))return false;
    const auto& native=*input.pipeline;const auto& layout=*input.layout;
    if(!native.layout||layout.sType!=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO||layout.pNext||layout.flags||
       layout.setLayoutCount>7||layout.setLayoutCount>state.limits.maxBoundSets||
       (layout.setLayoutCount&&!layout.pSetLayouts)||input.setLayouts.size()!=layout.setLayoutCount||
       (!input.compatibleSetLayouts.empty()&&input.compatibleSetLayouts.size()!=layout.setLayoutCount)||
       layout.pushConstantRangeCount>32||(layout.pushConstantRangeCount&&!layout.pPushConstantRanges)||
       native.stageCount==0||native.stageCount>2||!native.pStages||input.stages.size()!=native.stageCount)return false;
    std::uint64_t vertexUniforms=1,vertexResources=1,setUniforms=1;
    std::uint64_t fragmentUniforms=1,fragmentResources=1;
    bool updateAfterBind=false;std::uint32_t requiredSets=0;
    for(unsigned i=0;i<layout.setLayoutCount;++i){
        const auto& s=input.setLayouts[i];
        if(!layout.pSetLayouts[i]||s.sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO||
           (!input.compatibleSetLayouts.empty()&&!input.compatibleSetLayouts[i])||
           (s.flags&~VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT)||s.bindingCount>1024||
           (s.bindingCount&&!s.pBindings))return false;
        if(i==3&&(s.bindingCount||s.flags||s.pNext))return false;
        updateAfterBind|=bool(s.flags&VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT);
        if(s.bindingCount)requiredSets|=1u<<i;
        if(s.pNext){const auto* flags=static_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo*>(s.pNext);
            if(flags->sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO||flags->pNext||
               (flags->bindingCount&&flags->bindingCount!=s.bindingCount)||
               (flags->bindingCount&&!flags->pBindingFlags))return false;}
        for(unsigned b=0;b<s.bindingCount;++b){const auto& binding=s.pBindings[b];
            if(!binding.descriptorCount)return false;
            const bool uniform=binding.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER||
                               binding.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            if(uniform&&!add(setUniforms,binding.descriptorCount,state.limits.maxSetUniforms))return false;
            if(binding.stageFlags&VK_SHADER_STAGE_VERTEX_BIT){
                if(!add(vertexResources,binding.descriptorCount,state.limits.maxVertexResources))return false;
                if(uniform&&!add(vertexUniforms,binding.descriptorCount,state.limits.maxVertexUniforms))return false;
            }
            if(binding.stageFlags&VK_SHADER_STAGE_FRAGMENT_BIT){
                if(!add(fragmentResources,binding.descriptorCount,state.limits.maxFragmentResources))return false;
                if(uniform&&!add(fragmentUniforms,binding.descriptorCount,state.limits.maxFragmentUniforms))return false;
            }
        }
    }
    if(updateAfterBind&&(vertexUniforms>state.limits.maxVertexUpdateAfterBindUniforms||
       vertexResources>state.limits.maxVertexUpdateAfterBindResources||
       setUniforms>state.limits.maxSetUpdateAfterBindUniforms||
       fragmentUniforms>state.limits.maxFragmentUpdateAfterBindUniforms||
       fragmentResources>state.limits.maxFragmentUpdateAfterBindResources))return false;
    std::array<VkDescriptorSetLayout,7> sets;sets.fill(state.empty);
    for(unsigned i=0;i<layout.setLayoutCount;++i)sets[i]=input.compatibleSetLayouts.empty()?layout.pSetLayouts[i]:input.compatibleSetLayouts[i];
    const auto nativeSets=sets;
    VkPipelineLayoutCreateInfo restoreInfo=layout;
    restoreInfo.pSetLayouts=layout.setLayoutCount?nativeSets.data():nullptr;
    sets[3]=state.camera;
    VkPipelineLayoutCreateInfo ownLayout=layout;ownLayout.setLayoutCount=std::max(layout.setLayoutCount,4u);ownLayout.pSetLayouts=sets.data();
    VkShaderModule transformed{},fragment{},transformedFragment{};VkPipelineLayout pipelineLayout{},restoreLayout{};VkPipeline pipeline{};
    auto cleanup=[&]{
        if(pipeline)state.api.destroyPipeline(state.device,pipeline,nullptr);
        if(pipelineLayout)state.api.destroyLayout(state.device,pipelineLayout,nullptr);
        if(restoreLayout)state.api.destroyLayout(state.device,restoreLayout,nullptr);
        if(transformed)state.api.destroyShader(state.device,transformed,nullptr);
        if(transformedFragment)state.api.destroyShader(state.device,transformedFragment,nullptr);
    };
    try {
        std::span<const std::uint32_t> vertexWords;
        std::vector<std::uint32_t> rewrittenFragment;
        unsigned seen=0;
        for(unsigned i=0;i<native.stageCount;++i){const auto& s=native.pStages[i];const auto& words=input.stages[i];
            const unsigned bit=s.stage==VK_SHADER_STAGE_VERTEX_BIT?1:s.stage==VK_SHADER_STAGE_FRAGMENT_BIT?2:0;
            if(!bit||(seen&bit)||!s.module||words.module!=s.module||!s.pName)return false;
            std::size_t nameLength=0;while(nameLength<128&&s.pName[nameLength])++nameLength;
            if(nameLength==128)return false;
            const std::string_view entry{s.pName,nameLength};
            if(!frozenCameraStageInterface(words.words,s.stage,layout.setLayoutCount,entry)){
                // Only a successful exact FS transform may cross the strict
                // generic FS0/b1 barrier. All other interface checks remain.
                if(bit!=2||!stageInterface(words.words,s.stage,layout.setLayoutCount,entry,true))return false;
                CameraSplitReport fragmentReport;
                if(splitFrozenFragmentCamera(words.words,rewrittenFragment,fragmentReport)!=CameraSplitStatus::Converted)return false;
            }
            seen|=bit;if(bit==1)vertexWords=words.words;else fragment=s.module;
        }
        if(!(seen&1))return false;
        std::vector<std::uint32_t> rewritten;CameraSplitReport report;
        if(splitFrozenCamera(vertexWords,rewritten,report)!=CameraSplitStatus::Converted)return false;
        VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shader.codeSize=rewritten.size()*4;shader.pCode=rewritten.data();
        // Single-object create outputs are undefined on runtime failure. Never
        // destroy a non-null poison value a failed call may have written.
        if(state.api.createShader(state.device,&shader,nullptr,&transformed)!=VK_SUCCESS){transformed={};cleanup();return false;}
        if(!transformed){cleanup();return false;}
        if(!rewrittenFragment.empty()){
            shader.codeSize=rewrittenFragment.size()*4;shader.pCode=rewrittenFragment.data();
            if(state.api.createShader(state.device,&shader,nullptr,&transformedFragment)!=VK_SUCCESS){transformedFragment={};cleanup();return false;}
            if(!transformedFragment){cleanup();return false;}
            fragment=transformedFragment;
        }
        if(state.api.createLayout(state.device,&ownLayout,nullptr,&pipelineLayout)!=VK_SUCCESS){pipelineLayout={};cleanup();return false;}
        if(!pipelineLayout){cleanup();return false;}
        if(state.api.createLayout(state.device,&restoreInfo,nullptr,&restoreLayout)!=VK_SUCCESS){restoreLayout={};cleanup();return false;}
        if(!restoreLayout){cleanup();return false;}
        // This existing bounded copier does not alter view count. Keep the exact
        // native render pass/attachments/states; only transformed modules/layout change.
        auto original=native;VkPipelineFragmentShadingRateStateCreateInfoKHR rate{};
        if(original.pNext){
            const auto* p=static_cast<const VkPipelineFragmentShadingRateStateCreateInfoKHR*>(original.pNext);
            if(p->sType!=VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR||p->pNext){cleanup();return false;}
            rate=*p;original.pNext=nullptr;
        }
        MultiviewGraphicsPlan plan;
        if(!plan.prepare(original,transformed,fragment,pipelineLayout,native.renderPass,
                         {state.limits.depthClipEnable,state.limits.extendedDepthBounds})){cleanup();return false;}
        auto graphics=*plan.info();graphics.pNext=native.pNext?&rate:nullptr;
        // Preserve native push constants through layout compatibility. The
        // graphics copier never owns or changes the pipeline-layout definition.
        if(state.api.createPipelines(state.device,VK_NULL_HANDLE,1,&graphics,nullptr,&pipeline)!=VK_SUCCESS||!pipeline){cleanup();return false;}
        if(state.serial==UINT64_MAX){cleanup();return false;}
        Variant value{input.nativePipeline,pipeline,pipelineLayout,state.camera,state.deviceGeneration,state.cameraGeneration,state.serial+1,
                      native.layout,layout.setLayoutCount,requiredSets,restoreLayout};
        state.active.emplace(input.nativePipeline,state.records.size()); // only throwing publication
        state.records.push_back({value,true}); // capacity reserved; POD, cannot throw
        ++state.serial;pipeline={};pipelineLayout={};restoreLayout={};cleanup();output=value;return true;
    }catch(...){cleanup();return false;}
}
bool FrozenCameraPipelineFactory::lookup(VkPipeline native,Variant& out) const noexcept {
    Errno preserve;std::lock_guard lock(impl_->mutex);const auto it=impl_->active.find(native);
    if(it==impl_->active.end())return false;const auto& record=impl_->records[it->second];
    if(!record.selectable)return false;out=record.value;return true;
}
void FrozenCameraPipelineFactory::nativeDestroyed(VkPipeline native) noexcept {
    Errno preserve;std::lock_guard lock(impl_->mutex);auto it=impl_->active.find(native);if(it==impl_->active.end())return;
    impl_->records[it->second].selectable=false;impl_->active.erase(it);
}
void FrozenCameraPipelineFactory::shutdownAfterIdle(VkDevice device,VkResult result) noexcept {
    Errno preserve;std::lock_guard lock(impl_->mutex);
    if(device&&device==impl_->device&&(result==VK_SUCCESS||result==VK_ERROR_DEVICE_LOST))impl_->clear(true);
}
void FrozenCameraPipelineFactory::abandon(VkDevice device) noexcept {
    Errno preserve;std::lock_guard lock(impl_->mutex);if(device&&device==impl_->device)impl_->clear(false);
}
}
