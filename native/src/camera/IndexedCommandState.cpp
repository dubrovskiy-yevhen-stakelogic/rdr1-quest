#include "IndexedCommandState.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <new>

namespace rdrvr::engine {
namespace {
struct Errno {int value{errno};~Errno(){errno=value;}};
constexpr unsigned vp=1u,sc=2u,lw=4u,db=8u,bc=16u,bounds=32u,cmp=64u,wr=256u,ref=1024u,be=4096u;
constexpr VkShaderStageFlags stages=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT;
constexpr VkShaderStageFlagBits stage(unsigned i){return i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;}
std::uint32_t words(unsigned offset,unsigned n){
    if(!n)return 0;const unsigned count=n/4;
    return (count==32?~0u:((1u<<count)-1u))<<(offset/4);
}
unsigned dynamic(VkDynamicState s){switch(s){
case VK_DYNAMIC_STATE_VIEWPORT:return vp;case VK_DYNAMIC_STATE_SCISSOR:return sc;
case VK_DYNAMIC_STATE_LINE_WIDTH:return lw;case VK_DYNAMIC_STATE_DEPTH_BIAS:return db;
case VK_DYNAMIC_STATE_BLEND_CONSTANTS:return bc;case VK_DYNAMIC_STATE_DEPTH_BOUNDS:return bounds;
case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:return cmp|(cmp<<1);
case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:return wr|(wr<<1);
case VK_DYNAMIC_STATE_STENCIL_REFERENCE:return ref|(ref<<1);
case VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE:return be;default:return 0;}}
std::atomic<std::uint64_t> issuers{1};
std::uint64_t nextIssuer(){auto value=issuers.load(std::memory_order_relaxed);while(value&&value!=UINT64_MAX){if(issuers.compare_exchange_weak(value,value+1,std::memory_order_relaxed))return value;}return 0;}
}
struct IndexedCommandState::Layout {
    struct Binding {std::uint32_t binding{},count{};VkDescriptorType type{};VkShaderStageFlags stages{};VkDescriptorBindingFlags flags{};
        std::array<VkSampler,32> samplers{};bool immutable{};bool operator==(const Binding&) const=default;};
    struct SetDef {std::array<Binding,32> bindings{};unsigned count{},dynamic{};VkDescriptorSetLayoutCreateFlags flags{};
        bool operator==(const SetDef& b)const{return count==b.count&&dynamic==b.dynamic&&flags==b.flags&&std::equal(bindings.begin(),bindings.begin()+count,b.bindings.begin());}};
    enum class Origin {NativeCreation,HistoricalNative,OwnedRestore};
    Origin origin{};std::uint64_t externalGeneration{};
    VkPipelineLayout handle{};std::uint64_t serial{};bool alive{};unsigned count{},pushCount{};
    std::array<SetDef,maxSets> sets{};std::array<VkPushConstantRange,2> pushes{};
    std::array<VkDescriptorSetLayout,maxSets> nativeSets{};
    std::array<std::uint64_t,maxSets> nativeSetGenerations{};
    bool samePush(const Layout& b)const{
        if(pushCount!=b.pushCount)return false;
        for(unsigned i=0;i<pushCount;++i)if(pushes[i].stageFlags!=b.pushes[i].stageFlags||pushes[i].offset!=b.pushes[i].offset||pushes[i].size!=b.pushes[i].size)return false;
        return true;
    }
    bool compatible(const Layout& b,unsigned set)const{
        if(set>=count||set>=b.count||!samePush(b))return false;
        for(unsigned i=0;i<=set;++i)if(!(sets[i]==b.sets[i]))return false;
        return true;
    }
    bool same(const Layout& b)const{return count==b.count&&samePush(b)&&(!count||compatible(b,count-1));}
    std::uint32_t required()const{unsigned mask=0;for(unsigned i=0;i<count;++i)if(sets[i].count)mask|=1u<<i;return mask;}
    std::uint32_t pushMask(unsigned s)const{unsigned mask=0;for(unsigned i=0;i<pushCount;++i)if(pushes[i].stageFlags&stage(s))mask|=words(pushes[i].offset,pushes[i].size);return mask;}
    VkShaderStageFlags wordStages(unsigned word)const{VkShaderStageFlags result=0;for(unsigned i=0;i<pushCount;++i)if((words(pushes[i].offset,pushes[i].size)&(1u<<word)))result|=pushes[i].stageFlags;return result;}
};
struct IndexedCommandState::Pipeline {
    VkPipeline handle{};std::uint64_t serial{};bool alive{};const Layout* native{};const Layout* restore{};
    VkRenderPass pass{};unsigned subpass{},dynamic{},vertices{};
    FrozenCameraPipelineFactory::Variant variant{};
};
struct IndexedCommandState::Cold {
    std::array<std::unique_ptr<Layout>,1024> layouts{};std::array<Pipeline,512> pipelines{};unsigned layoutCount{},pipelineCount{};std::uint64_t serial{};
};
IndexedCommandState::IndexedCommandState(VkDevice device,std::uint64_t generation):cold_(new(std::nothrow) Cold),device_(device),deviceGeneration_(generation){
    issuer_=nextIssuer();if(!issuer_)failed_.store(true);
}
IndexedCommandState::~IndexedCommandState()=default;
bool IndexedCommandState::coldAccess()noexcept{
    if(std::this_thread::get_id()!=owner_){failed_.store(true,std::memory_order_release);return false;}
    if(emitting_){failed_.store(true,std::memory_order_release);return false;}
    return cold_&&issuer_&&device_&&deviceGeneration_&&!retired_.load(std::memory_order_acquire);
}
bool IndexedCommandState::access()noexcept{return coldAccess()&&recording_&&!failed_.load(std::memory_order_acquire);}
bool IndexedCommandState::access(VkCommandBuffer c)noexcept{
    if(!access()||c!=command_.buffer)return false;
    if(observationRevision_==UINT64_MAX){failed_.store(true,std::memory_order_release);return false;}
    ++observationRevision_;return true;
}
bool IndexedCommandState::good()const noexcept{return std::this_thread::get_id()==owner_&&cold_&&recording_&&!emitting_&&!failed_.load(std::memory_order_acquire)&&!retired_.load(std::memory_order_acquire);}
const IndexedCommandState::Layout* IndexedCommandState::layout(VkPipelineLayout h)const noexcept{
    if(!cold_||!h)return nullptr;for(unsigned i=cold_->layoutCount;i;--i){const auto& x=*cold_->layouts[i-1];if(x.origin==Layout::Origin::NativeCreation&&x.alive&&x.handle==h)return &x;}return nullptr;
}
const IndexedCommandState::Layout* IndexedCommandState::historicalLayout(VkPipelineLayout h,std::uint64_t generation)const noexcept{
    if(!cold_||!h||!generation)return nullptr;
    if(state_.pipeline){const auto* x=state_.pipeline->native;if(x->origin==Layout::Origin::HistoricalNative&&x->handle==h&&x->externalGeneration==generation)return x;}
    for(unsigned i=cold_->layoutCount;i;--i){const auto& x=*cold_->layouts[i-1];
        if(x.origin==Layout::Origin::HistoricalNative&&x.handle==h&&x.externalGeneration==generation)return &x;}return nullptr;
}
const IndexedCommandState::Pipeline* IndexedCommandState::pipeline(VkPipeline h)const noexcept{
    if(!cold_||!h)return nullptr;for(unsigned i=cold_->pipelineCount;i;--i){const auto& x=cold_->pipelines[i-1];if(x.alive&&x.handle==h)return &x;}return nullptr;
}
bool IndexedCommandState::layoutCreated(VkPipelineLayout handle,const VkPipelineLayoutCreateInfo& info,std::span<const VkDescriptorSetLayoutCreateInfo> sets)noexcept{
    Errno keep;if(!coldAccess()||!handle||info.sType!=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO||info.flags||info.pNext||info.setLayoutCount>maxSets||
       info.setLayoutCount!=sets.size()||(info.setLayoutCount&&!info.pSetLayouts)||info.pushConstantRangeCount>2||
       (info.pushConstantRangeCount&&!info.pPushConstantRanges)||cold_->layoutCount==cold_->layouts.size()||cold_->serial==UINT64_MAX)return false;
    Layout out{};out.handle=handle;out.count=info.setLayoutCount;out.pushCount=info.pushConstantRangeCount;
    VkShaderStageFlags used=0;
    for(unsigned i=0;i<out.pushCount;++i){const auto& p=info.pPushConstantRanges[i];
        if(!p.stageFlags||(p.stageFlags&~stages)||(p.stageFlags&used)||!p.size||(p.offset%4)||(p.size%4)||p.offset>=maxPushBytes||p.size>maxPushBytes-p.offset)return false;
        used|=p.stageFlags;out.pushes[i]=p;
    }
    std::sort(out.pushes.begin(),out.pushes.begin()+out.pushCount,[](auto& a,auto& b){return a.stageFlags<b.stageFlags;});
    for(unsigned i=0;i<out.count;++i){const auto& s=sets[i];auto& d=out.sets[i];
        if(!info.pSetLayouts[i]||s.sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO||s.flags||s.bindingCount>d.bindings.size()||(s.bindingCount&&!s.pBindings))return false;
        const VkDescriptorSetLayoutBindingFlagsCreateInfo* flags=nullptr;
        if(s.pNext){flags=static_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo*>(s.pNext);
            if(flags->sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO||flags->pNext||
               (flags->bindingCount&&flags->bindingCount!=s.bindingCount)||(flags->bindingCount&&!flags->pBindingFlags))return false;}
        d.count=s.bindingCount;
        for(unsigned j=0;j<s.bindingCount;++j){const auto& a=s.pBindings[j];auto& b=d.bindings[j];
            if(!a.descriptorCount)return false;b.binding=a.binding;b.count=a.descriptorCount;b.type=a.descriptorType;b.stages=a.stageFlags;b.immutable=a.pImmutableSamplers!=nullptr;
            b.flags=flags&&flags->bindingCount?flags->pBindingFlags[j]:0;
            if(b.flags&~VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT)return false;
            if(b.immutable){if(a.descriptorCount>b.samplers.size()||(a.descriptorType!=VK_DESCRIPTOR_TYPE_SAMPLER&&a.descriptorType!=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))return false;
                std::copy_n(a.pImmutableSamplers,a.descriptorCount,b.samplers.begin());}
            if(a.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC||a.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC){
                if(a.descriptorCount>maxOffsets-d.dynamic)return false;d.dynamic+=a.descriptorCount;}
        }
        std::sort(d.bindings.begin(),d.bindings.begin()+d.count,[](auto& a,auto& b){return a.binding<b.binding;});
        for(unsigned j=1;j<d.count;++j)if(d.bindings[j-1].binding==d.bindings[j].binding)return false;
    }
    auto owned=std::unique_ptr<Layout>(new(std::nothrow) Layout(out));if(!owned)return false;
    for(unsigned i=0;i<cold_->layoutCount;++i)if(cold_->layouts[i]->origin==Layout::Origin::NativeCreation&&cold_->layouts[i]->handle==handle)cold_->layouts[i]->alive=false;
    owned->serial=++cold_->serial;owned->alive=true;cold_->layouts[cold_->layoutCount++]=std::move(owned);return true;
}
void IndexedCommandState::layoutDestroyed(VkPipelineLayout h)noexcept{Errno keep;if(!coldAccess())return;for(unsigned i=0;i<cold_->layoutCount;++i)if(cold_->layouts[i]->handle==h)cold_->layouts[i]->alive=false;}
bool IndexedCommandState::pipelineCreated(VkPipeline h,const VkGraphicsPipelineCreateInfo& info,VkPipelineLayout restore)noexcept{
    Errno keep;if(!coldAccess()||!h||info.sType!=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO||info.pNext||!info.renderPass||
       cold_->pipelineCount==cold_->pipelines.size()||cold_->serial==UINT64_MAX)return false;
    auto* l=layout(info.layout);auto* r=layout(restore);if(!l||!r||!l->same(*r))return false;
    if(!info.stageCount||info.stageCount>2||!info.pStages)return false;unsigned seen=0;
    for(unsigned i=0;i<info.stageCount;++i){unsigned s=info.pStages[i].stage;if((s!=VK_SHADER_STAGE_VERTEX_BIT&&s!=VK_SHADER_STAGE_FRAGMENT_BIT)||(seen&s))return false;seen|=s;}
    if(!(seen&VK_SHADER_STAGE_VERTEX_BIT))return false;
    const auto* v=info.pVertexInputState;
    if(!v||v->sType!=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO||v->pNext||v->flags||v->vertexBindingDescriptionCount>maxBindings||v->vertexAttributeDescriptionCount>32||
       (v->vertexBindingDescriptionCount&&!v->pVertexBindingDescriptions)||(v->vertexAttributeDescriptionCount&&!v->pVertexAttributeDescriptions))return false;
    Pipeline p{};p.handle=h;p.native=l;p.restore=r;p.pass=info.renderPass;p.subpass=info.subpass;
    unsigned declared=0;
    for(unsigned i=0;i<v->vertexBindingDescriptionCount;++i){const auto b=v->pVertexBindingDescriptions[i].binding;if(b>=maxBindings||(declared&(1u<<b)))return false;declared|=1u<<b;}
    for(unsigned i=0;i<v->vertexAttributeDescriptionCount;++i){const auto b=v->pVertexAttributeDescriptions[i].binding;if(b>=maxBindings||!(declared&(1u<<b)))return false;p.vertices|=1u<<b;}
    if(info.pDynamicState){const auto& d=*info.pDynamicState;
        if(d.sType!=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO||d.pNext||d.flags||d.dynamicStateCount>10||(d.dynamicStateCount&&!d.pDynamicStates))return false;
        for(unsigned i=0;i<d.dynamicStateCount;++i){const auto m=dynamic(d.pDynamicStates[i]);if(!m||(m&p.dynamic))return false;p.dynamic|=m;}}
    if(p.dynamic&(vp|sc)){const auto* vps=info.pViewportState;
        if(!vps||vps->sType!=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO||vps->pNext||vps->flags||vps->viewportCount!=1||vps->scissorCount!=1)return false;}
    for(unsigned i=0;i<cold_->pipelineCount;++i)if(cold_->pipelines[i].handle==h)cold_->pipelines[i].alive=false;
    p.serial=++cold_->serial;p.alive=true;cold_->pipelines[cold_->pipelineCount++]=p;return true;
}
void IndexedCommandState::pipelineDestroyed(VkPipeline h)noexcept{Errno keep;if(!coldAccess())return;for(unsigned i=0;i<cold_->pipelineCount;++i)if(cold_->pipelines[i].handle==h)cold_->pipelines[i].alive=false;}
bool IndexedCommandState::begin(Command c,Api api)noexcept{
    Errno keep;if(!coldAccess())return false;
    if(c.device!=device_||c.deviceGeneration!=deviceGeneration_||!c.pool||!c.buffer||!c.allocation||!c.recording||session_==UINT64_MAX){recording_=false;failed_.store(true);return false;}
    ++session_;command_=c;api_=api;state_={};pass_={};subpass_=0;recording_=true;failed_.store(false,std::memory_order_release);return true;
}
void IndexedCommandState::end(VkCommandBuffer observed)noexcept{Errno keep;if(!access(observed))return;recording_=false;pass_={};}
void IndexedCommandState::retireDevice()noexcept{Errno keep;retired_.store(true,std::memory_order_release);failed_.store(true,std::memory_order_release);}
void IndexedCommandState::beginPass(VkCommandBuffer observed, VkRenderPass p,std::uint32_t s)noexcept{Errno keep;if(!access(observed))return;if(!p||pass_){failed_.store(true);return;}pass_=p;subpass_=s;}
void IndexedCommandState::endPass(VkCommandBuffer observed)noexcept{Errno keep;if(!access(observed))return;if(!pass_)failed_.store(true);pass_={};}
void IndexedCommandState::unsupported(VkCommandBuffer observed)noexcept{Errno keep;if(!access(observed))return;failed_.store(true,std::memory_order_release);}
void IndexedCommandState::bindPipeline(VkCommandBuffer observed, VkPipelineBindPoint point,VkPipeline h)noexcept{Errno keep;if(!access(observed))return;if(point==VK_PIPELINE_BIND_POINT_GRAPHICS){state_.pipeline=pipeline(h);
    // Static pipeline state invalidates its previous dynamic value. An unknown
    // pipeline conservatively invalidates every tracked dynamic value.
    state_.dynamicKnown&=state_.pipeline?state_.pipeline->dynamic:0u;}}
void IndexedCommandState::bindDescriptors(VkCommandBuffer observed, VkPipelineBindPoint point,VkPipelineLayout h,std::uint32_t first,std::span<const VkDescriptorSet> sets,std::span<const std::uint32_t> offsets)noexcept{
    Errno keep;if(!access(observed)||point!=VK_PIPELINE_BIND_POINT_GRAPHICS)return;recordDescriptors(layout(h),first,sets,offsets);
}
void IndexedCommandState::bindDescriptorsCurrent(VkCommandBuffer observed,VkPipelineBindPoint point,VkPipelineLayout h,
        std::uint64_t before,std::uint64_t after,std::uint32_t first,std::span<const VkDescriptorSet> sets,std::span<const std::uint32_t> offsets)noexcept{
    Errno keep;if(!access(observed)||point!=VK_PIPELINE_BIND_POINT_GRAPHICS)return;
    recordDescriptors(before&&before==after?historicalLayout(h,before):nullptr,first,sets,offsets);
}
void IndexedCommandState::recordDescriptors(const Layout* l,std::uint32_t first,std::span<const VkDescriptorSet> sets,std::span<const std::uint32_t> offsets)noexcept{
    if(!l||sets.empty()||first>=maxSets||sets.size()>maxSets-first||first+sets.size()>l->count){state_.sets={};return;}
    unsigned total=0;for(unsigned i=0;i<sets.size();++i){if(!sets[i]){state_.sets={};return;}total+=l->sets[first+i].dynamic;}
    if(total!=offsets.size()){state_.sets={};return;}
    // Conservative disturbance: keep an older set only when all prefixes up
    // to BOTH its index and the last newly bound set are compatible.
    const unsigned last=first+static_cast<unsigned>(sets.size())-1;
    for(unsigned i=0;i<maxSets;++i)if(state_.sets[i].valid&&!state_.sets[i].layout->compatible(*l,std::max(i,last)))state_.sets[i].valid=false;
    unsigned at=0;for(unsigned i=0;i<sets.size();++i){auto& s=state_.sets[first+i];s={};s.handle=sets[i];s.layout=l;s.count=l->sets[first+i].dynamic;s.valid=true;
        std::copy_n(offsets.begin()+at,s.count,s.offsets.begin());at+=s.count;}
}
void IndexedCommandState::bindVertices(VkCommandBuffer observed, std::uint32_t first,std::span<const VkBuffer> buffers,std::span<const VkDeviceSize> offsets)noexcept{
    Errno keep;if(!access(observed))return;if(buffers.empty()||buffers.size()!=offsets.size()||first>=maxBindings||buffers.size()>maxBindings-first){state_.vertexMask=0;return;}
    for(unsigned i=0;i<buffers.size();++i){const unsigned b=first+i;state_.vertices[b]=buffers[i];state_.offsets[b]=offsets[i];if(buffers[i])state_.vertexMask|=1u<<b;else state_.vertexMask&=~(1u<<b);}
}
void IndexedCommandState::bindIndex(VkCommandBuffer observed, VkBuffer b,VkDeviceSize o,VkIndexType t)noexcept{Errno keep;if(!access(observed))return;state_.index=b;state_.indexOffset=o;state_.indexType=t;state_.indexKnown=b&&(t==VK_INDEX_TYPE_UINT16||t==VK_INDEX_TYPE_UINT32);}
void IndexedCommandState::setViewport(VkCommandBuffer observed, std::uint32_t first,std::span<const VkViewport> v)noexcept{Errno keep;if(!access(observed))return;if(first||v.size()!=1){state_.dynamicKnown&=~vp;return;}state_.viewport=v[0];state_.dynamicKnown|=vp;}
void IndexedCommandState::setScissor(VkCommandBuffer observed, std::uint32_t first,std::span<const VkRect2D> v)noexcept{Errno keep;if(!access(observed))return;if(first||v.size()!=1){state_.dynamicKnown&=~sc;return;}state_.scissor=v[0];state_.dynamicKnown|=sc;}
void IndexedCommandState::setLineWidth(VkCommandBuffer observed, float x)noexcept{Errno keep;if(access(observed)){state_.lineWidth=x;state_.dynamicKnown|=lw;}}
void IndexedCommandState::setDepthBias(VkCommandBuffer observed, float x,float y,float z)noexcept{Errno keep;if(access(observed)){state_.depthBias[0]=x;state_.depthBias[1]=y;state_.depthBias[2]=z;state_.dynamicKnown|=db;}}
void IndexedCommandState::setBlendConstants(VkCommandBuffer observed, const float* p)noexcept{Errno keep;if(!access(observed))return;if(!p){state_.dynamicKnown&=~bc;return;}std::copy_n(p,4,state_.blend);state_.dynamicKnown|=bc;}
void IndexedCommandState::setDepthBounds(VkCommandBuffer observed, float x,float y)noexcept{Errno keep;if(access(observed)){state_.depthBounds[0]=x;state_.depthBounds[1]=y;state_.dynamicKnown|=bounds;}}
void IndexedCommandState::stencil(VkStencilFaceFlags f,std::uint32_t v,std::array<std::uint32_t,2>& values,unsigned bit)noexcept{
    if(!f||(f&~VK_STENCIL_FACE_FRONT_AND_BACK)){state_.dynamicKnown&=~(bit|(bit<<1));return;}
    if(f&VK_STENCIL_FACE_FRONT_BIT){values[0]=v;state_.dynamicKnown|=bit;}if(f&VK_STENCIL_FACE_BACK_BIT){values[1]=v;state_.dynamicKnown|=bit<<1;}
}
void IndexedCommandState::setStencilCompare(VkCommandBuffer observed, VkStencilFaceFlags f,std::uint32_t v)noexcept{Errno keep;if(access(observed))stencil(f,v,state_.compare,cmp);}
void IndexedCommandState::setStencilWrite(VkCommandBuffer observed, VkStencilFaceFlags f,std::uint32_t v)noexcept{Errno keep;if(access(observed))stencil(f,v,state_.write,wr);}
void IndexedCommandState::setStencilReference(VkCommandBuffer observed, VkStencilFaceFlags f,std::uint32_t v)noexcept{Errno keep;if(access(observed))stencil(f,v,state_.reference,ref);}
void IndexedCommandState::setDepthBoundsEnable(VkCommandBuffer observed, VkBool32 x)noexcept{Errno keep;if(access(observed)){state_.boundsEnable=x;if(x<=VK_TRUE)state_.dynamicKnown|=be;else state_.dynamicKnown&=~be;}}
void IndexedCommandState::pushConstants(VkCommandBuffer observed, VkPipelineLayout h,VkShaderStageFlags flags,std::uint32_t offset,std::span<const std::uint8_t> bytes)noexcept{
    Errno keep;if(!access(observed))return;recordPush(layout(h),flags,offset,bytes);
}
void IndexedCommandState::pushConstantsCurrent(VkCommandBuffer observed,VkPipelineLayout h,std::uint64_t before,std::uint64_t after,
        VkShaderStageFlags flags,std::uint32_t offset,std::span<const std::uint8_t> bytes)noexcept{
    Errno keep;if(!access(observed))return;recordPush(before&&before==after?historicalLayout(h,before):nullptr,flags,offset,bytes);
}
void IndexedCommandState::recordPush(const Layout* l,VkShaderStageFlags flags,std::uint32_t offset,std::span<const std::uint8_t> bytes)noexcept{
    if(!l||!flags||(flags&~stages)||bytes.empty()||(offset%4)||(bytes.size()%4)||offset>=maxPushBytes||bytes.size()>maxPushBytes-offset){state_.pushKnown={};return;}
    const auto mask=words(offset,static_cast<unsigned>(bytes.size()));
    for(unsigned i=0;i<2;++i)if(flags&stage(i))if((l->pushMask(i)&mask)!=mask){state_.pushKnown={};return;}
    // VUID01796: every overlapping range contributes ALL its stages to this
    // update. Updating a shared range one stage at a time is not valid Vulkan.
    for(unsigned w=offset/4;w<(offset+bytes.size())/4;++w)if(l->wordStages(w)!=flags){state_.pushKnown={};return;}
    for(unsigned i=0;i<2;++i)if(flags&stage(i)){
        if(state_.pushLayout[i]&&!state_.pushLayout[i]->samePush(*l))state_.pushKnown[i]=0;
        state_.pushLayout[i]=l;std::copy(bytes.begin(),bytes.end(),state_.push[i].begin()+offset);state_.pushKnown[i]|=mask;}
}
bool IndexedCommandState::complete(const State& s)const noexcept{
    const auto* p=s.pipeline;if(!p||!p->alive||!p->restore->alive||!s.indexKnown||(s.vertexMask&p->vertices)!=p->vertices||
       (s.dynamicKnown&p->dynamic)!=p->dynamic||!pass_||p->pass!=pass_||p->subpass!=subpass_)return false;
    const auto* l=p->native;const unsigned required=l->required();
    for(unsigned i=0;i<maxSets;++i)if(required&(1u<<i)){
        const auto& d=s.sets[i];if(!d.valid||!d.handle||!d.layout||!d.layout->compatible(*l,i)||d.count!=l->sets[i].dynamic)return false;}
    for(unsigned i=0;i<2;++i){const auto mask=l->pushMask(i);if(mask&&(!s.pushLayout[i]||!s.pushLayout[i]->samePush(*l)||(s.pushKnown[i]&mask)!=mask))return false;}
    for(unsigned w=0;w<maxPushBytes/4;++w)if(l->wordStages(w)==stages&&std::memcmp(s.push[0].data()+4*w,s.push[1].data()+4*w,4))return false;
    return true;
}
bool IndexedCommandState::snapshotIndexed(VkCommandBuffer observed, Indexed args,Packet& output)noexcept{
    Errno keep;if(!access(observed)||!args.count||!args.instances||!complete(state_))return false;
    Packet p{};p.state_=state_;p.command_=command_;p.indexed_=args;p.pass_=pass_;p.subpass_=subpass_;p.issuer_=issuer_;p.session_=session_;output=p;return true;
}
bool IndexedCommandState::canEmit(const State& target,const State& original)const noexcept{
    if(!complete(target)||!complete(original)||!api_.pipeline||!api_.draw||!api_.indices)return false;
    const auto* a=target.pipeline;const auto* b=original.pipeline;
    // Initial supported subset: identical dynamic masks. This avoids emitting
    // a setter after restoration to a static PSO (VUID08608), and ensures every
    // state invalidated by the temporary bind is explicitly restored.
    if(a->dynamic!=b->dynamic)return false;
    if((original.vertexMask&a->vertices)!=a->vertices||(original.dynamicKnown&a->dynamic)!=a->dynamic)return false;
    if(a->vertices&&!api_.vertices)return false;
    const unsigned bits=a->dynamic;
    if(((bits&vp)&&!api_.viewport)||((bits&sc)&&!api_.scissor)||((bits&lw)&&!api_.lineWidth)||((bits&db)&&!api_.depthBias)||
       ((bits&bc)&&!api_.blendConstants)||((bits&bounds)&&!api_.depthBounds)||((bits&(cmp|(cmp<<1)))&&!api_.stencilCompare)||
       ((bits&(wr|(wr<<1)))&&!api_.stencilWrite)||((bits&(ref|(ref<<1)))&&!api_.stencilReference)||((bits&be)&&!api_.depthBoundsEnable))return false;
    const unsigned required=a->native->required();
    if(required&&!api_.descriptors)return false;
    // Do not create descriptor state that cannot be put back. Also restore all
    // known native sets that a different layout prefix could disturb.
    for(unsigned i=0;i<maxSets;++i){const auto& d=original.sets[i];
        if((required&(1u<<i))&&!d.valid)return false;
        if(d.valid&&(!d.layout||!d.layout->compatible(*b->restore,i)||d.count!=b->restore->sets[i].dynamic))return false;}
    if(required){for(unsigned i=0;i<maxSets;++i)if(original.sets[i].valid&&!api_.descriptors)return false;}
    if(a->native->pushCount){if(!api_.push||!a->native->samePush(*b->native))return false;
        for(unsigned i=0;i<2;++i){const auto mask=a->native->pushMask(i);if((original.pushKnown[i]&mask)!=mask)return false;}}
    return true;
}
void IndexedCommandState::emitState(const State& s,std::uint32_t vertexMask,std::uint32_t dynamicMask,std::uint32_t setMask,bool pushes)noexcept{
    const auto c=command_.buffer;const auto& p=*s.pipeline;
    api_.pipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,p.handle);
    for(unsigned i=0;i<maxSets;++i)if(setMask&(1u<<i)){const auto& d=s.sets[i];api_.descriptors(c,VK_PIPELINE_BIND_POINT_GRAPHICS,p.restore->handle,i,1,&d.handle,d.count,d.count?d.offsets.data():nullptr);}
    for(unsigned i=0;i<maxBindings;++i)if(vertexMask&(1u<<i))api_.vertices(c,i,1,&s.vertices[i],&s.offsets[i]);
    api_.indices(c,s.index,s.indexOffset,s.indexType);
    if(dynamicMask&vp)api_.viewport(c,0,1,&s.viewport);if(dynamicMask&sc)api_.scissor(c,0,1,&s.scissor);
    if(dynamicMask&lw)api_.lineWidth(c,s.lineWidth);if(dynamicMask&db)api_.depthBias(c,s.depthBias[0],s.depthBias[1],s.depthBias[2]);
    if(dynamicMask&bc)api_.blendConstants(c,s.blend);if(dynamicMask&bounds)api_.depthBounds(c,s.depthBounds[0],s.depthBounds[1]);
    for(unsigned i=0;i<2;++i){const auto face=i?VK_STENCIL_FACE_BACK_BIT:VK_STENCIL_FACE_FRONT_BIT;
        if(dynamicMask&(cmp<<i))api_.stencilCompare(c,face,s.compare[i]);if(dynamicMask&(wr<<i))api_.stencilWrite(c,face,s.write[i]);
        if(dynamicMask&(ref<<i))api_.stencilReference(c,face,s.reference[i]);}
    if(dynamicMask&be)api_.depthBoundsEnable(c,s.boundsEnable);
    if(pushes)for(unsigned w=0;w<maxPushBytes/4;){const auto flags=p.native->wordStages(w);if(!flags){++w;continue;}
        const unsigned first=w++;while(w<maxPushBytes/4&&p.native->wordStages(w)==flags)++w;
        api_.push(c,p.restore->handle,flags,4*first,4*(w-first),s.push[(flags&VK_SHADER_STAGE_VERTEX_BIT)?0:1].data()+4*first);}
}
IndexedCommandState::Emission IndexedCommandState::emitAndRestore(const Packet& packet)noexcept{
    Errno keep;
    if(!access()||packet.issuer_!=issuer_||packet.session_!=session_||!(packet.command_==command_)||packet.pass_!=pass_||packet.subpass_!=subpass_||
       !canEmit(packet.state_,state_))return Emission::Declined;
    const State original=state_;const auto& target=packet.state_;unsigned restoreSets=0;
    if(target.pipeline->native->required())for(unsigned i=0;i<maxSets;++i)if(original.sets[i].valid)restoreSets|=1u<<i;
    emitting_=true;
    emitState(target,target.pipeline->vertices,target.pipeline->dynamic,target.pipeline->native->required(),target.pipeline->native->pushCount!=0);
    const auto& a=packet.indexed_;api_.draw(command_.buffer,a.count,a.instances,a.first,a.vertex,a.base);
    emitState(original,target.pipeline->vertices,target.pipeline->dynamic,restoreSets,target.pipeline->native->pushCount!=0);
    emitting_=false;
    return (failed_.load(std::memory_order_acquire)||retired_.load(std::memory_order_acquire))?Emission::EmittedFaulted:Emission::Emitted;
}
bool IndexedCommandState::emissionCurrent()noexcept {
    const auto local=[&]()noexcept{return !failed_.load(std::memory_order_acquire)&&!retired_.load(std::memory_order_acquire);};
    if(!local())return false;
    if(execution_.current&&!execution_.current(execution_.context)){failed_.store(true,std::memory_order_release);return false;}
    return local(); // The external predicate may itself reenter an observer.
}
bool IndexedCommandState::emitStateChecked(const State& s,std::uint32_t vertexMask,
        std::uint32_t dynamicMask,std::uint32_t setMask,bool pushes,bool bindPipeline)noexcept {
    const auto c=command_.buffer;const auto& p=*s.pipeline;
    // The terminal raw PFNs may report observer reentry by revoking this mirror.
    // Stop ALL later calls at that point; recording/resource lifetime is unknown.
    const auto call=[&](auto fn,auto... args)noexcept {
        if(!emissionCurrent())return false;
        fn(args...);return emissionCurrent();
    };
    if(bindPipeline&&!call(api_.pipeline,c,VK_PIPELINE_BIND_POINT_GRAPHICS,p.handle))return false;
    for(unsigned i=0;i<maxSets;++i)if(setMask&(1u<<i)){const auto& d=s.sets[i];
        if(!call(api_.descriptors,c,VK_PIPELINE_BIND_POINT_GRAPHICS,p.restore->handle,i,1u,&d.handle,d.count,d.count?d.offsets.data():nullptr))return false;}
    for(unsigned i=0;i<maxBindings;++i)if(vertexMask&(1u<<i))
        if(!call(api_.vertices,c,i,1u,&s.vertices[i],&s.offsets[i]))return false;
    if(!call(api_.indices,c,s.index,s.indexOffset,s.indexType))return false;
    const auto mask=dynamicMask;
    if((mask&vp)&&!call(api_.viewport,c,0u,1u,&s.viewport))return false;
    if((mask&sc)&&!call(api_.scissor,c,0u,1u,&s.scissor))return false;
    if((mask&lw)&&!call(api_.lineWidth,c,s.lineWidth))return false;
    if((mask&db)&&!call(api_.depthBias,c,s.depthBias[0],s.depthBias[1],s.depthBias[2]))return false;
    if((mask&bc)&&!call(api_.blendConstants,c,s.blend))return false;
    if((mask&bounds)&&!call(api_.depthBounds,c,s.depthBounds[0],s.depthBounds[1]))return false;
    for(unsigned i=0;i<2;++i){const auto face=i?VK_STENCIL_FACE_BACK_BIT:VK_STENCIL_FACE_FRONT_BIT;
        if((mask&(cmp<<i))&&!call(api_.stencilCompare,c,face,s.compare[i]))return false;
        if((mask&(wr<<i))&&!call(api_.stencilWrite,c,face,s.write[i]))return false;
        if((mask&(ref<<i))&&!call(api_.stencilReference,c,face,s.reference[i]))return false;}
    if((mask&be)&&!call(api_.depthBoundsEnable,c,s.boundsEnable))return false;
    if(pushes)for(unsigned w=0;w<maxPushBytes/4;){const auto flags=p.native->wordStages(w);
        if(!flags){++w;continue;}const unsigned first=w++;
        while(w<maxPushBytes/4&&p.native->wordStages(w)==flags)++w;
        if(!call(api_.push,c,p.restore->handle,flags,4*first,4*(w-first),s.push[(flags&VK_SHADER_STAGE_VERTEX_BIT)?0:1].data()+4*first))return false;}
    return emissionCurrent();
}
IndexedCommandState::Emission IndexedCommandState::emitSequence(const void* packets,unsigned count,const Packet&(*at)(const void*,unsigned)noexcept,Execution execution,bool sequence)noexcept {
    Errno keep;
    if(!access()||!packets||!at||!count||count>maxPackets)return Emission::Declined;
    const State original=state_;
    unsigned vertexMask=0,dynamicMask=0,requiredSets=0,restoreSets=0;bool pushes=false;
    for(unsigned part=0;part<count;++part){const auto& packet=at(packets,part);
        if(packet.issuer_!=issuer_||packet.session_!=session_||!(packet.command_==command_)||
           packet.pass_!=pass_||packet.subpass_!=subpass_||!packet.indexed_.count||!packet.indexed_.instances||
           !canEmit(packet.state_,original))return Emission::Declined;
        const auto& p=*packet.state_.pipeline;
        vertexMask|=p.vertices;dynamicMask|=p.dynamic;requiredSets|=p.native->required();pushes|=p.native->pushCount!=0;
    }
    if(requiredSets)for(unsigned i=0;i<maxSets;++i)if(original.sets[i].valid)restoreSets|=1u<<i;
    // No callbacks in preflight; still observe concurrent terminal invalidation
    // before exposing the first target call. There is no partial Declined path.
    if(!access())return Emission::Declined;
    execution_=execution;
    struct ClearExecution {Execution& value;~ClearExecution(){value={};}} clear{execution_};
    emitting_=true;
    if(!emissionCurrent()){emitting_=false;return Emission::Declined;}
    for(unsigned part=0;part<count;++part){const auto& packet=at(packets,part);
        const State target=packet.state_;const Indexed args=packet.indexed_;
        const auto& p=*target.pipeline;
        if(!emitStateChecked(target,p.vertices,p.dynamic,p.native->required(),p.native->pushCount!=0,!sequence))break;
        if(!emissionCurrent())break;
        if(execution_.drawIndexed){
            if(!execution_.drawIndexed(execution_.context,part,args)){failed_.store(true,std::memory_order_release);break;}
        }else api_.draw(command_.buffer,args.count,args.instances,args.first,args.vertex,args.base);
        if(!emissionCurrent())break;
    }
    if(emissionCurrent())
        (void)emitStateChecked(original,vertexMask,dynamicMask,restoreSets,pushes);
    // Keep observer reentry forbidden through the LAST external predicate.
    const bool completed=emissionCurrent();
    emitting_=false;
    return completed?Emission::Emitted:Emission::EmittedFaulted;
}
IndexedCommandState::Emission IndexedCommandState::emitAndRestore(std::span<const Packet> packets)noexcept {
    if(packets.size()>maxPackets)return Emission::Declined;
    return emitSequence(packets.data(),static_cast<unsigned>(packets.size()),[](const void* p,unsigned i)noexcept->const Packet&{return static_cast<const Packet*>(p)[i];},{});
}
void IndexedCommandState::describeState(const State& state,unsigned setMask,Description& out)const noexcept {
    Description value{};value.command=command_;value.pass=pass_;value.subpass=subpass_;
    value.pipeline=state.pipeline->handle;value.restoreLayout=state.pipeline->restore->handle;
    value.variantSerial=state.pipeline->variant.serial;value.nativeLayoutGeneration=state.pipeline->native->externalGeneration;
    value.issuer=issuer_;value.session=session_;value.setMask=setMask;
    for(unsigned i=0;i<maxSets;++i)if(setMask&(1u<<i)){const auto& s=state.sets[i];auto& d=value.sets[i];
        d.handle=s.handle;d.sourceLayout=s.layout->handle;d.sourceLayoutGeneration=s.layout->externalGeneration?s.layout->externalGeneration:s.layout->serial;d.count=s.count;
        std::copy_n(s.offsets.begin(),s.count,d.offsets.begin());}
    out=value;
}
bool IndexedCommandState::describe(const Packet& packet,Description& out)noexcept {
    Errno keep;
    if(!access()||packet.issuer_!=issuer_||packet.session_!=session_||!(packet.command_==command_)||
       packet.pass_!=pass_||packet.subpass_!=subpass_||!complete(packet.state_))return false;
    describeState(packet.state_,packet.state_.pipeline->native->required(),out);return true;
}
bool IndexedCommandState::describeIncoming(Description& out)noexcept {
    Errno keep;if(!access()||!complete(state_))return false;
    unsigned mask=0;const auto& restore=*state_.pipeline->restore;
    for(unsigned i=0;i<maxSets;++i){const auto& s=state_.sets[i];if(!s.valid)continue;
        if(!s.layout||!s.layout->compatible(restore,i)||s.count!=restore.sets[i].dynamic)return false;
        mask|=1u<<i;}
    describeState(state_,mask,out);return true;
}
bool IndexedCommandState::uniformInterface(const Layout& layout,unsigned set,const rdr95::CommandUniformSets::Binding& own)const noexcept {
    if(set>=layout.count)return false;
    const auto& def=layout.sets[set];
    if(!def.count||def.count!=own.count||def.dynamic!=own.count||own.count>own.definitions.size())return false;
    for(unsigned i=0;i<own.count;++i){const auto& a=def.bindings[i];const auto& b=own.definitions[i];
        if(a.immutable||a.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC||a.count!=1||
           b.binding!=a.binding||b.count!=a.count||b.type!=a.type||b.stages!=a.stages||b.flags!=a.flags)return false;
    }
    return true;
}
bool IndexedCommandState::snapshotOwnedIndexed(VkCommandBuffer command,Indexed args,
    const rdr95::UniformCaptureSink& sink,OwnedPacket& output)noexcept {
    Errno keep;Packet ordinary;if(!snapshotIndexed(command,args,ordinary))return false;
    // Sink.current invokes the integration's recording-thread predicate. Do
    // not publish an older source state if that callback changed the mirror.
    const auto observed=observationRevision_;
    const auto* layout=ordinary.state_.pipeline->native;unsigned mask{};
    for(unsigned set=0;set<layout->count;++set){const auto& def=layout->sets[set];
        bool uniform=false;
        for(unsigned i=0;i<def.count;++i){const auto type=def.bindings[i].type;
            if(type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)uniform=true;
            // Mutable buffers not represented by DUD cannot silently borrow a
            // source-eye binding. Immutable textures need separate admission.
            if(type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER||type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER||
               type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC||type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER||
               type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)return false;
        }
        if(uniform)mask|=1u<<set;
    }
    if(!mask)return false;
    rdr95::UniformCaptureSink::Ticket ticket;if(!sink.current(command,mask,ticket))return false;
    const auto sourceCommand=ticket.command();
    if(sourceCommand.device!=command_.device||sourceCommand.pool!=command_.pool||sourceCommand.command!=command_.buffer)return false;
    for(unsigned set=0;set<layout->count;++set)if(mask&(1u<<set)){
        rdr95::UniformCaptureSink::SourceBinding source;rdr95::CommandUniformSets::Binding own;
        if(!ticket.resolve(set,source,own))return false;
        auto& current=ordinary.state_.sets[set];
        if(!current.valid||!current.layout||ticket.nativePipelineLayout()!=current.layout->handle||
           source.native!=current.handle||source.count!=current.count||source.count>source.offsets.size()||
           !std::equal(source.offsets.begin(),source.offsets.begin()+source.count,current.offsets.begin())||
           own.device!=command_.device||own.command!=command_.buffer||!own.descriptor||
           !uniformInterface(*layout,set,own))return false;
        current.handle=own.descriptor;current.count=own.count;current.offsets={};
        std::copy_n(own.offsets.begin(),own.count,current.offsets.begin());
    }
    if(observationRevision_!=observed||!access(command)||!complete(ordinary.state_))return false;
    OwnedPacket value;value.packet_=ordinary;value.uniforms_=ticket;value.mask_=mask;
    if(!ownedCurrent(value))return false;
    output=value;return true;
}
bool IndexedCommandState::ownedCurrent(const OwnedPacket& packet)noexcept {
    const auto& p=packet.packet_;
    if(!access()||!packet.mask_||p.issuer_!=issuer_||p.session_!=session_||!(p.command_==command_)||
       p.pass_!=pass_||p.subpass_!=subpass_||!complete(p.state_))return false;
    for(unsigned set=0;set<maxSets;++set)if(packet.mask_&(1u<<set)){
        rdr95::UniformCaptureSink::SourceBinding source;rdr95::CommandUniformSets::Binding own;
        if(!packet.uniforms_.resolve(set,source,own))return false;
        const auto& saved=p.state_.sets[set];
        if(own.device!=command_.device||own.command!=command_.buffer||own.descriptor!=saved.handle||
           own.count!=saved.count||!uniformInterface(*p.state_.pipeline->native,set,own)||
           !std::equal(own.offsets.begin(),own.offsets.begin()+own.count,saved.offsets.begin()))return false;
    }
    return true;
}
IndexedCommandState::Emission IndexedCommandState::emitOwnedAndRestore(const OwnedPacket& packet)noexcept {
    Errno keep;if(!ownedCurrent(packet))return Emission::Declined;
    return emitOwnedAndRestore(std::span<const OwnedPacket>(&packet,1));
}
bool IndexedCommandState::describeOwned(const OwnedPacket& packet,Description& output)noexcept {
    Errno keep;return ownedCurrent(packet)&&describe(packet.packet_,output);
}
bool IndexedCommandState::describeOwnedGeometry(const OwnedPacket& packet,OwnedGeometry& output)noexcept {
    Errno keep;if(!ownedCurrent(packet))return false;
    const auto& p=packet.packet_;const auto& s=p.state_;
    OwnedGeometry value;value.vertices=s.vertices;value.offsets=s.offsets;value.vertexMask=s.vertexMask;
    value.index=s.index;value.indexOffset=s.indexOffset;value.indexType=s.indexType;value.draw=p.indexed_;
    if(!good())return false;output=value;return true;
}
IndexedCommandState::Emission IndexedCommandState::emitOwnedAndRestore(std::span<const OwnedPacket> packets)noexcept {
    Errno keep;if(packets.empty()||packets.size()>maxPackets)return Emission::Declined;
    // No copies of 64 large packets, heap allocation, or exported draw token.
    // Every arena ticket is resolved before the first state write.
    for(const auto& p:packets)if(!ownedCurrent(p))return Emission::Declined;
    return emitSequence(packets.data(),static_cast<unsigned>(packets.size()),[](const void* p,unsigned i)noexcept->const Packet&{return static_cast<const OwnedPacket*>(p)[i].packet_;},{});
}

bool IndexedCommandState::preflightOwned(std::span<const OwnedPacket> packets)noexcept {
    Errno keep;if(packets.empty()||packets.size()>maxPackets||!access())return false;
    for(const auto& p:packets)if(!ownedCurrent(p)||!canEmit(p.packet_.state_,state_))return false;
    return access();
}
IndexedCommandState::Emission IndexedCommandState::emitOwnedAndRestore(std::span<const OwnedPacket> packets,Execution execution)noexcept {
    Errno keep;if(!execution.current||!execution.drawIndexed||!preflightOwned(packets))return Emission::Declined;
    return emitSequence(packets.data(),static_cast<unsigned>(packets.size()),[](const void* p,unsigned i)noexcept->const Packet&{return static_cast<const OwnedPacket*>(p)[i].packet_;},execution);
}
bool IndexedCommandState::ownedSequenceCurrent(const Description& saved,std::uint64_t observed)const noexcept {
    return std::this_thread::get_id()==owner_&&cold_&&recording_&&
        !failed_.load(std::memory_order_acquire)&&!retired_.load(std::memory_order_acquire)&&
        observationRevision_==observed&&issuer_==saved.issuer&&session_==saved.session&&
        command_==saved.command&&pass_==saved.pass&&subpass_==saved.subpass;
}
// Only OwnedIndexedReplay can pair omitted target binds with the private
// Runtime sequence plan. canEmit still requires identical dynamic masks.
IndexedCommandState::Emission IndexedCommandState::emitOwnedSequence(std::span<const OwnedPacket> packets,Execution execution)noexcept {
    Errno keep;if(!execution.current||!execution.drawIndexed||!preflightOwned(packets))return Emission::Declined;
    return emitSequence(packets.data(),static_cast<unsigned>(packets.size()),[](const void* p,unsigned i)noexcept->const Packet&{return static_cast<const OwnedPacket*>(p)[i].packet_;},execution,true);
}
#include "IndexedReplayImport.inc"
}
