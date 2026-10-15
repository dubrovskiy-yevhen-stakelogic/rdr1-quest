#include "FrozenCameraCreationRegistry.h"
#include "PlainDepthShader.h"
#include <array>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <unordered_map>
#include <type_traits>
#include <vector>

namespace rdrvr::engine {
namespace {
struct Errno {int* cell=&errno;int value=*cell;~Errno(){*cell=value;}};
// Internal calls use original driver dispatch. If a callback enters an observer
// while that call is active, forward native work but do not recursively capture.
thread_local unsigned internalDepth{};
struct Internal {Internal(){++internalDepth;}~Internal(){--internalDepth;}};
bool sameApi(const FrozenCameraCreationRegistry::Api&a,const FrozenCameraCreationRegistry::Api&b)noexcept{return a.createShader==b.createShader&&a.destroyShader==b.destroyShader&&a.createSetLayout==b.createSetLayout&&a.destroySetLayout==b.destroySetLayout&&a.createLayout==b.createLayout&&a.destroyLayout==b.destroyLayout&&a.createPipelines==b.createPipelines&&a.destroyPipeline==b.destroyPipeline;}
}
struct FrozenCameraCreationRegistry::Impl {
 struct Shader {VkShaderModule native{};std::uint64_t generation{};bool alive{};std::vector<std::uint32_t> words;};
 struct Set {
  VkDescriptorSetLayout native{},owned{};std::uint64_t generation{};bool alive{};
  VkDescriptorSetLayoutCreateFlags flags{};bool hasBindingFlags{};
  std::vector<VkDescriptorSetLayoutBinding> bindings;std::vector<VkDescriptorBindingFlags> bindingFlags;
  VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
  VkDescriptorSetLayoutCreateInfo description() noexcept {
   flagsInfo.bindingCount=static_cast<unsigned>(bindingFlags.size());flagsInfo.pBindingFlags=bindingFlags.empty()?nullptr:bindingFlags.data();
   VkDescriptorSetLayoutCreateInfo r{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};r.flags=flags;r.bindingCount=static_cast<unsigned>(bindings.size());r.pBindings=bindings.empty()?nullptr:bindings.data();r.pNext=hasBindingFlags?&flagsInfo:nullptr;return r;
  }
 };
 struct Layout {VkPipelineLayout native{};std::uint64_t generation{};bool alive{};std::vector<const Set*> sets;std::vector<VkDescriptorSetLayout> nativeSets;std::vector<VkPushConstantRange> pushes;};
 struct ReplayGraphics {
  VkPipelineCreateFlags flags{};VkShaderStageFlags stages{};VkRenderPass renderPass{};
  std::uint32_t subpass{},vertexBindingMask{},dynamicCount{};
  std::array<VkDynamicState,ReplayMaxDynamics> dynamics{};
  bool operator==(const ReplayGraphics&)const=default;
 };
 struct DepthGraphics {
  VkPrimitiveTopology topology{};VkCompareOp compare{};VkSampleCountFlagBits samples{};
  VkCullModeFlags cull{};VkFrontFace front{};unsigned colors{},count{};
  std::array<VkShaderStageFlagBits,2> stages{};std::array<std::array<char,128>,2> entries{};
  bool operator==(const DepthGraphics&)const=default;
 };
 struct DepthEvidence {DepthGraphics graphics{};unsigned ownedSets{};std::array<unsigned,7> ownedBindings{};std::array<std::uint64_t,2> shaderGenerations{};bool valid{};};
 struct Pipeline {const Layout* layout{};DrawMetadata metadata{};ReplayGraphics replay{};bool replayValid{};DepthEvidence depth{};};
 struct Before {const Layout* layout{};std::array<const Shader*,2> shaders{};unsigned count{};ReplayGraphics replay{};bool replayValid{};DepthEvidence depth{};};
 mutable std::mutex mutex;
 VkDevice device{};std::uint64_t generation{},serial{};Api api{};bool initialized{},ready{};
 std::atomic<bool> reentryLost{};
 std::atomic<bool> originalsBound{};
 std::unique_ptr<FrozenCameraPipelineFactory> factory;
 Stats counters{};
 std::vector<std::unique_ptr<Shader>> shaders;
 std::vector<std::unique_ptr<Set>> sets;
 std::vector<std::unique_ptr<Layout>> layouts;
 std::unordered_map<VkShaderModule,const Shader*> activeShaders;
 std::unordered_map<VkDescriptorSetLayout,const Set*> activeSets;
 std::unordered_map<VkPipelineLayout,const Layout*> activeLayouts;
 std::unordered_map<VkPipeline,Pipeline> activePipelines;
 std::uint64_t next() noexcept {if(serial==UINT64_MAX)return 0;return ++serial;}
 bool captures(VkDevice d)const noexcept{return ready&&d==device&&!internalDepth&&!reentryLost.load(std::memory_order_relaxed);}
 template<class Map,class Handle>void remove(Map&map,Handle h) noexcept {const auto it=map.find(h);if(it!=map.end()){const_cast<std::remove_const_t<std::remove_pointer_t<typename Map::mapped_type>>*>(it->second)->alive=false;map.erase(it);++counters.invalidations;}}
 template<class Map,class Handle,class Record>static bool same(const Map& map,Handle h,const Record* r)noexcept{const auto it=map.find(h);return r&&r->alive&&it!=map.end()&&it->second==r;}
 std::unique_ptr<Shader> shaderInput(const VkShaderModuleCreateInfo* ci) {
  if(!ci||ci->sType!=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO||ci->pNext||ci->flags||!ci->pCode||ci->codeSize<20||ci->codeSize%4||ci->codeSize>131072u*4)return {};
  auto r=std::make_unique<Shader>();r->words.assign(ci->pCode,ci->pCode+ci->codeSize/4);return r;
 }
 std::unique_ptr<Set> setInput(const VkDescriptorSetLayoutCreateInfo* ci) {
  if(!ci||ci->sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO||(ci->flags&~VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT)||ci->bindingCount>1024||(ci->bindingCount&&!ci->pBindings))return {};
  auto r=std::make_unique<Set>();r->flags=ci->flags;
  for(unsigned i=0;i<ci->bindingCount;++i){const auto&b=ci->pBindings[i];if(b.pImmutableSamplers||!b.descriptorCount)return {};for(const auto&old:r->bindings)if(old.binding==b.binding)return {};r->bindings.push_back(b);}
  if(ci->pNext){const auto*p=static_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo*>(ci->pNext);if(p->sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO||p->pNext||(p->bindingCount&&p->bindingCount!=ci->bindingCount)||(p->bindingCount&&!p->pBindingFlags))return {};r->hasBindingFlags=true;if(p->bindingCount)r->bindingFlags.assign(p->pBindingFlags,p->pBindingFlags+p->bindingCount);}
  return r;
 }
 std::unique_ptr<Layout> layoutInput(const VkPipelineLayoutCreateInfo* ci) {
  if(!ci||ci->sType!=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO||ci->pNext||ci->flags||ci->setLayoutCount>7||(ci->setLayoutCount&&!ci->pSetLayouts)||ci->pushConstantRangeCount>32||(ci->pushConstantRangeCount&&!ci->pPushConstantRanges))return {};
  auto r=std::make_unique<Layout>();
  for(unsigned i=0;i<ci->setLayoutCount;++i){const auto it=activeSets.find(ci->pSetLayouts[i]);if(it==activeSets.end()||!it->second->alive)return {};r->sets.push_back(it->second);r->nativeSets.push_back(it->first);}
  if(ci->pushConstantRangeCount)r->pushes.assign(ci->pPushConstantRanges,ci->pPushConstantRanges+ci->pushConstantRangeCount);return r;
 }
 bool layoutUnchanged(const Layout&r)const noexcept {for(const auto*s:r.sets)if(!same(activeSets,s->native,s))return false;return true;}
 static bool replayInput(const VkGraphicsPipelineCreateInfo& ci,ReplayGraphics& out)noexcept {
  if(ci.sType!=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO||ci.pNext||ci.flags||!ci.renderPass||
     !ci.stageCount||ci.stageCount>2||!ci.pStages)return false;
  ReplayGraphics r{};r.flags=ci.flags;r.renderPass=ci.renderPass;r.subpass=ci.subpass;
  for(unsigned i=0;i<ci.stageCount;++i){const auto& s=ci.pStages[i];
   if(s.sType!=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO||s.pNext||s.flags||
      (s.stage!=VK_SHADER_STAGE_VERTEX_BIT&&s.stage!=VK_SHADER_STAGE_FRAGMENT_BIT)||(r.stages&s.stage))return false;
   r.stages|=s.stage;
  }
  if(!(r.stages&VK_SHADER_STAGE_VERTEX_BIT))return false;
  const auto* v=ci.pVertexInputState;
  if(!v||v->sType!=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO||v->pNext||v->flags||
     v->vertexBindingDescriptionCount>8||v->vertexAttributeDescriptionCount>32||
     (v->vertexBindingDescriptionCount&&!v->pVertexBindingDescriptions)||(v->vertexAttributeDescriptionCount&&!v->pVertexAttributeDescriptions))return false;
  unsigned declared=0;
  for(unsigned i=0;i<v->vertexBindingDescriptionCount;++i){const auto b=v->pVertexBindingDescriptions[i].binding;
   if(b>=8||(declared&(1u<<b)))return false;declared|=1u<<b;
  }
  for(unsigned i=0;i<v->vertexAttributeDescriptionCount;++i){const auto b=v->pVertexAttributeDescriptions[i].binding;
   if(b>=8||!(declared&(1u<<b)))return false;r.vertexBindingMask|=1u<<b;
  }
  bool viewport=false;
  if(ci.pDynamicState){const auto& d=*ci.pDynamicState;
   if(d.sType!=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO||d.pNext||d.flags||d.dynamicStateCount>ReplayMaxDynamics||
      (d.dynamicStateCount&&!d.pDynamicStates))return false;
   for(unsigned i=0;i<d.dynamicStateCount;++i){const auto value=d.pDynamicStates[i];
    switch(value){
    case VK_DYNAMIC_STATE_VIEWPORT:case VK_DYNAMIC_STATE_SCISSOR:viewport=true;break;
    case VK_DYNAMIC_STATE_LINE_WIDTH:case VK_DYNAMIC_STATE_DEPTH_BIAS:case VK_DYNAMIC_STATE_BLEND_CONSTANTS:
    case VK_DYNAMIC_STATE_DEPTH_BOUNDS:case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:
    case VK_DYNAMIC_STATE_STENCIL_REFERENCE:case VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE:break;
    default:return false;
    }
    for(unsigned j=0;j<i;++j)if(r.dynamics[j]==value)return false;r.dynamics[i]=value;
   }
   r.dynamicCount=d.dynamicStateCount;
  }
  if(viewport){const auto* vps=ci.pViewportState;
   if(!vps||vps->sType!=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO||vps->pNext||vps->flags||vps->viewportCount!=1||vps->scissorCount!=1)return false;
  }
  out=r;return true;
 }
 static bool replayLayout(const Layout& layout,ReplayMetadata& out)noexcept {
  if(layout.sets.size()>ReplayMaxSets||layout.pushes.size()>ReplayMaxPushes)return false;
  VkShaderStageFlags used=0;
  for(unsigned i=0;i<layout.pushes.size();++i){const auto& p=layout.pushes[i];
   constexpr auto stages=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT;
   if(!p.stageFlags||(p.stageFlags&~stages)||(p.stageFlags&used)||!p.size||(p.offset%4)||(p.size%4)||p.offset>=128||p.size>128-p.offset)return false;
   used|=p.stageFlags;out.pushes[i]=p;
  }
  out.pushCount=static_cast<unsigned>(layout.pushes.size());out.setCount=static_cast<unsigned>(layout.sets.size());
  for(unsigned i=0;i<layout.sets.size();++i){const auto& s=*layout.sets[i];auto& d=out.sets[i];
   if(s.flags||s.bindings.size()>ReplayMaxBindings||(!s.bindingFlags.empty()&&s.bindingFlags.size()!=s.bindings.size()))return false;
   d.native=s.native;d.generation=s.generation;d.flags=s.flags;d.hasBindingFlags=s.hasBindingFlags;d.count=static_cast<unsigned>(s.bindings.size());
   for(unsigned j=0;j<s.bindings.size();++j){const auto& b=s.bindings[j];const auto flags=s.bindingFlags.empty()?0:s.bindingFlags[j];
    if(b.pImmutableSamplers||!b.descriptorCount||(flags&~VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT))return false;
    d.bindings[j]={b.binding,b.descriptorCount,b.descriptorType,b.stageFlags,flags};
   }
   std::sort(d.bindings.begin(),d.bindings.begin()+d.count,[](const auto& a,const auto& b){return a.binding<b.binding;});
  }
  return true;
 }
 static bool depthGraphics(const VkGraphicsPipelineCreateInfo& ci,DepthGraphics& out)noexcept {
  if(!ci.pStages||!ci.stageCount||ci.stageCount>2||ci.pTessellationState)return false;
  DepthGraphics d{};d.count=ci.stageCount;
  for(unsigned i=0;i<ci.stageCount;++i){const auto& s=ci.pStages[i];
   if(!s.pName||s.pSpecializationInfo)return false;unsigned n=0;while(n<128&&s.pName[n])++n;
   if(!n||n==128)return false;std::copy_n(s.pName,n,d.entries[i].begin());d.stages[i]=s.stage;
  }
  const auto* a=ci.pInputAssemblyState;
  if(!a||a->sType!=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO||a->pNext||a->flags||a->topology!=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST||a->primitiveRestartEnable)return false;
  d.topology=a->topology;
  const auto* r=ci.pRasterizationState;
  if(!r||r->sType!=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO||r->pNext||r->flags||r->depthClampEnable||r->rasterizerDiscardEnable||r->depthBiasEnable||r->polygonMode!=VK_POLYGON_MODE_FILL||
     (r->cullMode&~VK_CULL_MODE_FRONT_AND_BACK)||(r->frontFace!=VK_FRONT_FACE_CLOCKWISE&&r->frontFace!=VK_FRONT_FACE_COUNTER_CLOCKWISE))return false;
  d.cull=r->cullMode;d.front=r->frontFace;
  const auto* z=ci.pDepthStencilState;
  if(!z||z->sType!=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO||z->pNext||z->flags||z->depthTestEnable!=VK_TRUE||z->depthWriteEnable!=VK_TRUE||z->depthBoundsTestEnable||z->stencilTestEnable)return false;
  if(z->depthCompareOp!=VK_COMPARE_OP_LESS&&z->depthCompareOp!=VK_COMPARE_OP_LESS_OR_EQUAL&&z->depthCompareOp!=VK_COMPARE_OP_GREATER&&z->depthCompareOp!=VK_COMPARE_OP_GREATER_OR_EQUAL)return false;
  d.compare=z->depthCompareOp;
  const auto* m=ci.pMultisampleState;
  if(!m||m->sType!=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO||m->pNext||m->flags||m->rasterizationSamples!=VK_SAMPLE_COUNT_1_BIT||m->sampleShadingEnable||m->alphaToCoverageEnable||m->alphaToOneEnable||(m->pSampleMask&&!(m->pSampleMask[0]&1u)))return false;
  d.samples=m->rasterizationSamples;
  if(const auto* c=ci.pColorBlendState){
   if(c->sType!=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO||c->pNext||c->flags||c->logicOpEnable||c->attachmentCount>8||(c->attachmentCount&&!c->pAttachments))return false;
   d.colors=c->attachmentCount;for(unsigned i=0;i<c->attachmentCount;++i)if(c->pAttachments[i].colorWriteMask)return false;
  }
  const auto* v=ci.pViewportState;const auto* dynamic=ci.pDynamicState;
  if(!v||v->sType!=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO||v->pNext||v->flags||v->viewportCount!=1||v->scissorCount!=1||
     !dynamic||dynamic->dynamicStateCount!=2||!dynamic->pDynamicStates)return false;
  const auto x=dynamic->pDynamicStates[0],y=dynamic->pDynamicStates[1];
  if(!((x==VK_DYNAMIC_STATE_VIEWPORT&&y==VK_DYNAMIC_STATE_SCISSOR)||(y==VK_DYNAMIC_STATE_VIEWPORT&&x==VK_DYNAMIC_STATE_SCISSOR)))return false;
  out=d;return true;
 }
 static DepthEvidence depthBefore(const Before& before,const VkGraphicsPipelineCreateInfo& ci)noexcept {
  DepthEvidence result{};
  try {
   if(!before.replayValid||!before.layout||!before.layout->pushes.empty()||!depthGraphics(ci,result.graphics))return {};
   ReplayMetadata definitions;if(!replayLayout(*before.layout,definitions))return {};
   for(unsigned set=0;set<definitions.setCount;++set){const auto& d=definitions.sets[set];if(set==3&&d.count)return {};
    for(unsigned j=0;j<d.count;++j){const auto& binding=d.bindings[j];
     if(binding.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC||binding.count!=1||binding.binding>=16||!binding.stages||
        (binding.stages&~(VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT)))return {};
     result.ownedSets|=1u<<set;
    }
   }
   for(unsigned i=0;i<before.count;++i){detail::PlainDepthShader::Result shader;
    if(!detail::PlainDepthShader::run(before.shaders[i]->words,ci.pStages[i].stage,definitions.setCount,result.graphics.entries[i].data(),shader))return {};
    for(const auto& uniform:shader.uniforms){if(uniform.set>=definitions.setCount)return {};
     const auto& d=definitions.sets[uniform.set];bool found=false;
     for(unsigned j=0;j<d.count;++j)if(d.bindings[j].binding==uniform.binding&&(d.bindings[j].stages&ci.pStages[i].stage))found=true;
     if(!found)return {};result.ownedBindings[uniform.set]|=1u<<uniform.binding;
    }
    result.shaderGenerations[ci.pStages[i].stage==VK_SHADER_STAGE_VERTEX_BIT?0u:1u]=before.shaders[i]->generation;
   }
   result.valid=true;return result;
  }catch(...){return {};}
 }
 Before pipelineBefore(const VkGraphicsPipelineCreateInfo&ci)const noexcept {
  Before r;if(ci.sType!=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO||!ci.stageCount||ci.stageCount>2||!ci.pStages)return r;
  const auto l=activeLayouts.find(ci.layout);if(l==activeLayouts.end()||!l->second->alive)return r;r.layout=l->second;
  for(unsigned i=0;i<ci.stageCount;++i){const auto s=activeShaders.find(ci.pStages[i].module);if(s==activeShaders.end()||!s->second->alive)return {};r.shaders[i]=s->second;}
  r.count=ci.stageCount;r.replayValid=replayInput(ci,r.replay);r.depth=depthBefore(r,ci);return r;
 }
 bool unchanged(const Before&r,const VkGraphicsPipelineCreateInfo&ci)const noexcept {
  if(!r.layout||r.count!=ci.stageCount||!ci.pStages||!same(activeLayouts,ci.layout,r.layout))return false;
  for(unsigned i=0;i<r.count;++i)if(!same(activeShaders,ci.pStages[i].module,r.shaders[i]))return false;return true;
 }
 void pipelineAfter(const Before&before,const VkGraphicsPipelineCreateInfo&ci,VkPipeline pipeline) {
  FrozenCameraPipelineFactory::Variant previous;
  if(pipeline&&(activePipelines.count(pipeline)||factory->lookup(pipeline,previous))){activePipelines.erase(pipeline);factory->nativeDestroyed(pipeline);++counters.declines;return;}
  if(!pipeline||!unchanged(before,ci)||activePipelines.size()>=MaxPipelines){++counters.declines;return;}
  ReplayGraphics returned{};const bool replayReturned=before.replayValid&&replayInput(ci,returned)&&returned==before.replay;
  DepthGraphics returnedDepth{};const bool depthReturned=before.depth.valid&&depthGraphics(ci,returnedDepth)&&returnedDepth==before.depth.graphics;
  const auto& snapshot=*before.layout;
  DrawMetadata metadata{};metadata.nativeLayoutGeneration=snapshot.generation;metadata.setCount=static_cast<unsigned>(snapshot.sets.size());
  for(unsigned i=0;i<snapshot.sets.size();++i){
   const auto& bindings=snapshot.sets[i]->bindings;if(!bindings.empty())metadata.requiredNativeSets|=1u<<i;
   for(const auto& binding:bindings)if(binding.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC||binding.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC){
    if(binding.descriptorCount>UINT32_MAX-metadata.dynamicCounts[i]){++counters.declines;return;}
    metadata.dynamicCounts[i]+=binding.descriptorCount;
   }
  }
  std::array<VkDescriptorSetLayoutCreateInfo,7> definitions{};
  std::array<VkDescriptorSetLayout,7> clones{};
  for(unsigned i=0;i<snapshot.sets.size();++i){definitions[i]=const_cast<Set*>(snapshot.sets[i])->description();clones[i]=snapshot.sets[i]->owned;}
  VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount=static_cast<unsigned>(snapshot.sets.size());layout.pSetLayouts=snapshot.nativeSets.empty()?nullptr:snapshot.nativeSets.data();
  layout.pushConstantRangeCount=static_cast<unsigned>(snapshot.pushes.size());layout.pPushConstantRanges=snapshot.pushes.empty()?nullptr:snapshot.pushes.data();
  std::array<FrozenCameraPipelineFactory::StageWords,2> words{};
  for(unsigned i=0;i<before.count;++i)words[i]={before.shaders[i]->native,before.shaders[i]->words};
  FrozenCameraPipelineFactory::Input input{pipeline,&ci,&layout,{definitions.data(),snapshot.sets.size()},{words.data(),before.count},{clones.data(),snapshot.sets.size()}};
  FrozenCameraPipelineFactory::Variant value;Internal internal;
  if(factory->create(input,value)&&!reentryLost.load(std::memory_order_relaxed)){
   if(!value.restoreLayout||value.nativeLayout!=snapshot.native||value.nativeSetCount!=metadata.setCount||value.requiredNativeSets!=metadata.requiredNativeSets){factory->nativeDestroyed(pipeline);++counters.declines;return;}
   metadata.variant=value;
   // Only this owned, immutable record reaches a render-thread pull. A failed
   // allocation retires the selectable variant; its GPU resources stay owned.
   ReplayGraphics after{};const bool replayValid=replayReturned&&replayInput(ci,after)&&after==before.replay;
   DepthEvidence depth=before.depth;DepthGraphics afterDepth{};depth.valid=depthReturned&&replayValid&&depthGraphics(ci,afterDepth)&&afterDepth==before.depth.graphics;
   try{activePipelines.emplace(pipeline,Pipeline{before.layout,metadata,before.replay,replayValid,depth});++counters.variants;}
   catch(...){factory->nativeDestroyed(pipeline);++counters.declines;}
  }else {factory->nativeDestroyed(pipeline);++counters.declines;}
 }
 void clear(bool destroy) noexcept {
  ready=false;
  if(factory){if(destroy)factory->shutdownAfterIdle(device,VK_SUCCESS);else factory->abandon(device);}
  if(destroy)for(auto&s:sets)if(s->owned)api.destroySetLayout(device,s->owned,nullptr);
  activePipelines.clear();activeLayouts.clear();activeSets.clear();activeShaders.clear();layouts.clear();sets.clear();shaders.clear();device={};counters.shaderBytes=0;
 }
};
FrozenCameraCreationRegistry::FrozenCameraCreationRegistry()noexcept:impl_(new(std::nothrow) Impl){}
FrozenCameraCreationRegistry::~FrozenCameraCreationRegistry()=default;
bool FrozenCameraCreationRegistry::bindOriginals(Api api)noexcept {Errno preserve;try{if(!impl_||!api)return false;auto&s=*impl_;std::lock_guard lock(s.mutex);if(s.originalsBound.load(std::memory_order_relaxed))return sameApi(s.api,api);s.api=api;s.originalsBound.store(true,std::memory_order_release);return true;}catch(...){return false;}}
bool FrozenCameraCreationRegistry::initialize(VkDevice d,std::uint64_t gen,Api api,Limits limits,VkDescriptorSetLayout camera,std::uint64_t cameraGen)noexcept {
 Errno preserve;try{if(!bindOriginals(api)||!d||!gen)return false;auto&s=*impl_;std::lock_guard lock(s.mutex);if(s.initialized)return false;
  s.shaders.reserve(MaxShaders);s.sets.reserve(MaxSets);s.layouts.reserve(MaxLayouts);s.activeShaders.reserve(MaxShaders);s.activeSets.reserve(MaxSets);s.activeLayouts.reserve(MaxLayouts);s.activePipelines.reserve(MaxPipelines);
  auto factory=std::make_unique<FrozenCameraPipelineFactory>();Internal internal;if(!factory->initialize(d,gen,api,limits,camera,cameraGen))return false;
  s.device=d;s.generation=gen;s.factory=std::move(factory);s.initialized=s.ready=true;return true;
 }catch(...){return false;}
}
VkResult FrozenCameraCreationRegistry::createShader(VkDevice d,const VkShaderModuleCreateInfo*ci,const VkAllocationCallbacks*a,VkShaderModule*out)noexcept {
 if(!impl_||!impl_->originalsBound.load(std::memory_order_acquire)||!impl_->api.createShader)return VK_ERROR_INITIALIZATION_FAILED;auto&s=*impl_;const int incoming=errno;std::unique_ptr<Impl::Shader> captured;
 if(internalDepth){s.reentryLost.store(true,std::memory_order_relaxed);return s.api.createShader(d,ci,a,out);}
 try{std::lock_guard lock(s.mutex);if(s.captures(d)&&s.shaders.size()<MaxShaders&&ci&&ci->codeSize<=MaxShaderBytes-s.counters.shaderBytes)captured=s.shaderInput(ci);}catch(...){}
 errno=incoming;const auto result=s.api.createShader(d,ci,a,out);const int resulting=errno;
 try{std::lock_guard lock(s.mutex);if(s.captures(d)&&result==VK_SUCCESS&&out&&*out){if(s.activeShaders.count(*out)){s.remove(s.activeShaders,*out);captured.reset();}if(captured&&s.shaders.size()<MaxShaders&&captured->words.size()*4<=MaxShaderBytes-s.counters.shaderBytes){captured->native=*out;captured->generation=s.next();captured->alive=true;if(captured->generation){const auto bytes=captured->words.size()*4;s.activeShaders.emplace(*out,captured.get());s.shaders.push_back(std::move(captured));s.counters.shaderBytes+=bytes;++s.counters.shaderRecords;}}else ++s.counters.declines;}}catch(...){}
 errno=resulting;return result;
}
VkResult FrozenCameraCreationRegistry::createSetLayout(VkDevice d,const VkDescriptorSetLayoutCreateInfo*ci,const VkAllocationCallbacks*a,VkDescriptorSetLayout*out)noexcept {
 if(!impl_||!impl_->originalsBound.load(std::memory_order_acquire)||!impl_->api.createSetLayout)return VK_ERROR_INITIALIZATION_FAILED;auto&s=*impl_;const int incoming=errno;std::unique_ptr<Impl::Set> captured;
 if(internalDepth){s.reentryLost.store(true,std::memory_order_relaxed);return s.api.createSetLayout(d,ci,a,out);}
 try{std::lock_guard lock(s.mutex);if(s.captures(d)&&s.sets.size()<MaxSets)captured=s.setInput(ci);}catch(...){}
 errno=incoming;const auto result=s.api.createSetLayout(d,ci,a,out);const int resulting=errno;VkDescriptorSetLayout clone{};
 try{std::lock_guard lock(s.mutex);if(s.captures(d)&&result==VK_SUCCESS&&out&&*out){if(s.activeSets.count(*out)){s.remove(s.activeSets,*out);captured.reset();}if(captured&&s.sets.size()<MaxSets){captured->native=*out;captured->generation=s.next();captured->alive=true;auto desc=captured->description();Internal internal;VkDescriptorSetLayout returned{};if(captured->generation&&s.api.createSetLayout(s.device,&desc,nullptr,&returned)==VK_SUCCESS&&returned){clone=returned;if(!s.reentryLost.load(std::memory_order_relaxed)){captured->owned=clone;s.activeSets.emplace(*out,captured.get());s.sets.push_back(std::move(captured));clone={};++s.counters.setRecords;}}else ++s.counters.declines;}else ++s.counters.declines;}}catch(...){}
 if(clone){Internal internal;s.api.destroySetLayout(d,clone,nullptr);}errno=resulting;return result;
}
VkResult FrozenCameraCreationRegistry::createPipelineLayout(VkDevice d,const VkPipelineLayoutCreateInfo*ci,const VkAllocationCallbacks*a,VkPipelineLayout*out)noexcept {
 if(!impl_||!impl_->originalsBound.load(std::memory_order_acquire)||!impl_->api.createLayout)return VK_ERROR_INITIALIZATION_FAILED;auto&s=*impl_;const int incoming=errno;std::unique_ptr<Impl::Layout> captured;
 if(internalDepth){s.reentryLost.store(true,std::memory_order_relaxed);return s.api.createLayout(d,ci,a,out);}
 try{std::lock_guard lock(s.mutex);if(s.captures(d)&&s.layouts.size()<MaxLayouts)captured=s.layoutInput(ci);}catch(...){}
 errno=incoming;const auto result=s.api.createLayout(d,ci,a,out);const int resulting=errno;
 try{std::lock_guard lock(s.mutex);if(s.captures(d)&&result==VK_SUCCESS&&out&&*out){if(s.activeLayouts.count(*out)){s.remove(s.activeLayouts,*out);captured.reset();}if(captured&&s.layouts.size()<MaxLayouts&&s.layoutUnchanged(*captured)){captured->native=*out;captured->generation=s.next();captured->alive=true;if(captured->generation){s.activeLayouts.emplace(*out,captured.get());s.layouts.push_back(std::move(captured));++s.counters.layoutRecords;}}else ++s.counters.declines;}}catch(...){}
 errno=resulting;return result;
}
VkResult FrozenCameraCreationRegistry::createPipelines(VkDevice d,VkPipelineCache cache,std::uint32_t count,const VkGraphicsPipelineCreateInfo*ci,const VkAllocationCallbacks*a,VkPipeline*out)noexcept {
 if(!impl_||!impl_->originalsBound.load(std::memory_order_acquire)||!impl_->api.createPipelines)return VK_ERROR_INITIALIZATION_FAILED;auto&s=*impl_;const int incoming=errno;std::array<Impl::Before,MaxBatch> before{};bool captured=false;
 if(internalDepth){s.reentryLost.store(true,std::memory_order_relaxed);return s.api.createPipelines(d,cache,count,ci,a,out);}
 try{std::lock_guard lock(s.mutex);if(s.captures(d)&&ci&&count&&count<=MaxBatch){captured=true;for(unsigned i=0;i<count;++i)before[i]=s.pipelineBefore(ci[i]);}}catch(...){}
 errno=incoming;const auto result=s.api.createPipelines(d,cache,count,ci,a,out);const int resulting=errno;
 try{std::lock_guard lock(s.mutex);if(s.captures(d)&&captured&&result==VK_SUCCESS&&out)for(unsigned i=0;i<count;++i)s.pipelineAfter(before[i],ci[i],out[i]);}catch(...){}
 errno=resulting;return result;
}
#define RDR95_DESTROY(Method,Handle,Field,Map) \
void FrozenCameraCreationRegistry::Method(VkDevice d,Handle h,const VkAllocationCallbacks*a)noexcept { \
 if(!impl_||!impl_->originalsBound.load(std::memory_order_acquire)||!impl_->api.Field)return;auto&s=*impl_;const int incoming=errno; \
 try{if(!internalDepth){std::lock_guard lock(s.mutex);if(d==s.device)s.remove(s.Map,h);}else s.reentryLost.store(true,std::memory_order_relaxed);}catch(...){} \
 errno=incoming;s.api.Field(d,h,a); \
}
RDR95_DESTROY(destroyShader,VkShaderModule,destroyShader,activeShaders)
RDR95_DESTROY(destroySetLayout,VkDescriptorSetLayout,destroySetLayout,activeSets)
RDR95_DESTROY(destroyPipelineLayout,VkPipelineLayout,destroyLayout,activeLayouts)
#undef RDR95_DESTROY
void FrozenCameraCreationRegistry::destroyPipeline(VkDevice d,VkPipeline h,const VkAllocationCallbacks*a)noexcept {
 if(!impl_||!impl_->originalsBound.load(std::memory_order_acquire)||!impl_->api.destroyPipeline)return;auto&s=*impl_;const int incoming=errno;try{if(!internalDepth){std::lock_guard lock(s.mutex);if(d==s.device&&s.factory){s.activePipelines.erase(h);s.factory->nativeDestroyed(h);}}else s.reentryLost.store(true,std::memory_order_relaxed);}catch(...){}errno=incoming;s.api.destroyPipeline(d,h,a);
}
bool FrozenCameraCreationRegistry::lookup(VkPipeline h,Variant&out)const noexcept {Errno preserve;try{if(!impl_||internalDepth)return false;auto&s=*impl_;std::lock_guard lock(s.mutex);return s.ready&&!s.reentryLost.load(std::memory_order_relaxed)&&s.factory&&s.factory->lookup(h,out);}catch(...){return false;}}
bool FrozenCameraCreationRegistry::lookupDrawMetadata(VkPipeline h,DrawMetadata&out)const noexcept {
 Errno preserve;try{
  if(!impl_||internalDepth)return false;auto&s=*impl_;std::lock_guard lock(s.mutex);
  if(!s.ready||s.reentryLost.load(std::memory_order_relaxed)||!s.factory)return false;
  const auto it=s.activePipelines.find(h);if(it==s.activePipelines.end())return false;
  const auto&record=it->second;const auto*layout=record.layout;
  // Layout is registry-owned historical metadata, not a borrowed live handle.
  // Its immutable definition and generation remain tied to this pipeline even
  // after native destruction/recreation. Actual restoration uses the owned
  // original-compatible layout in this exact factory variant.
  if(!layout||layout->generation!=record.metadata.nativeLayoutGeneration)return false;
  Variant variant{};if(!s.factory->lookup(h,variant)||!variant.restoreLayout||variant.restoreLayout!=record.metadata.variant.restoreLayout||variant.serial!=record.metadata.variant.serial||variant.nativeLayout!=layout->native||variant.deviceGeneration!=s.generation)return false;
  out=record.metadata;return true;
 }catch(...){return false;}
}
FrozenCameraCreationRegistry::Stats FrozenCameraCreationRegistry::stats()const noexcept {Errno preserve;try{if(!impl_||internalDepth)return {};auto&s=*impl_;std::lock_guard lock(s.mutex);auto r=s.counters;r.ready=s.ready&&!s.reentryLost.load(std::memory_order_relaxed);return r;}catch(...){return {};}}
bool FrozenCameraCreationRegistry::lookupReplayMetadata(VkPipeline h,ReplayMetadata& out)const noexcept {
 Errno preserve;try{
  if(!impl_||internalDepth)return false;auto& s=*impl_;std::lock_guard lock(s.mutex);
  if(!s.ready||s.reentryLost.load(std::memory_order_relaxed)||!s.factory)return false;
  const auto it=s.activePipelines.find(h);if(it==s.activePipelines.end())return false;
  const auto& r=it->second;const auto* l=r.layout;
  if(!r.replayValid||!l||l->generation!=r.metadata.nativeLayoutGeneration)return false;
  Variant v{};
  if(!s.factory->lookup(h,v)||!v.restoreLayout||v.restoreLayout!=r.metadata.variant.restoreLayout||
     v.serial!=r.metadata.variant.serial||v.nativeLayout!=l->native||v.deviceGeneration!=s.generation)return false;
  ReplayMetadata value{};value.draw=r.metadata;
  if(!Impl::replayLayout(*l,value))return false;
  value.flags=r.replay.flags;value.stages=r.replay.stages;value.renderPass=r.replay.renderPass;
  value.subpass=r.replay.subpass;value.vertexBindingMask=r.replay.vertexBindingMask;
  value.dynamicCount=r.replay.dynamicCount;value.dynamics=r.replay.dynamics;
  out=value;return true;
 }catch(...){return false;}
}
bool FrozenCameraCreationRegistry::lookupPlainDepthCertificate(VkPipeline h,PlainDepthCertificate& out)const noexcept {
 Errno preserve;try{if(!impl_||internalDepth)return false;auto& s=*impl_;std::lock_guard lock(s.mutex);
  if(!s.ready||s.reentryLost.load(std::memory_order_relaxed)||!s.factory)return false;
  const auto it=s.activePipelines.find(h);if(it==s.activePipelines.end()||!it->second.depth.valid)return false;
  const auto& record=it->second;Variant current{};
  if(!record.layout||record.layout->generation!=record.metadata.nativeLayoutGeneration||!s.factory->lookup(h,current)||!current.restoreLayout||current.restoreLayout!=record.metadata.variant.restoreLayout||current.serial!=record.metadata.variant.serial||current.deviceGeneration!=s.generation||current.nativeLayout!=record.layout->native)return false;
  PlainDepthCertificate c;c.owner_=this;c.native_=h;c.variant_=current.serial;c.device_=s.generation;c.layout_=record.metadata.nativeLayoutGeneration;out=c;return true;
 }catch(...){return false;}
}
bool FrozenCameraCreationRegistry::resolvePlainDepth(VkPipeline h,std::uint64_t variant,std::uint64_t device,std::uint64_t layout,PlainDepthDescription* out)const noexcept {
 Errno preserve;try{if(!impl_||internalDepth)return false;auto& s=*impl_;std::lock_guard lock(s.mutex);
  if(!s.ready||s.reentryLost.load(std::memory_order_relaxed)||!s.factory||s.generation!=device)return false;
  const auto it=s.activePipelines.find(h);if(it==s.activePipelines.end())return false;const auto& r=it->second;
  if(!r.depth.valid||!r.layout||r.metadata.nativeLayoutGeneration!=layout||r.layout->generation!=layout||r.metadata.variant.serial!=variant)return false;
  Variant current{};if(!s.factory->lookup(h,current)||!current.restoreLayout||current.serial!=variant||current.deviceGeneration!=device||current.nativeLayout!=r.layout->native||current.restoreLayout!=r.metadata.variant.restoreLayout)return false;
  if(out){PlainDepthDescription d;d.device=s.device;d.draw=r.metadata;d.renderPass=r.replay.renderPass;d.subpass=r.replay.subpass;
   d.topology=r.depth.graphics.topology;d.depthCompare=r.depth.graphics.compare;d.samples=r.depth.graphics.samples;
   d.cull=r.depth.graphics.cull;d.frontFace=r.depth.graphics.front;d.colorAttachmentCount=r.depth.graphics.colors;
   d.vertexBindingMask=r.replay.vertexBindingMask;d.dynamicCount=r.replay.dynamicCount;d.dynamics=r.replay.dynamics;
   d.requiredOwnedSets=r.depth.ownedSets;d.requiredOwnedBindings=r.depth.ownedBindings;d.shaderGenerations=r.depth.shaderGenerations;
   d.fragmentPresent=bool(r.replay.stages&VK_SHADER_STAGE_FRAGMENT_BIT);*out=d;}
  return true;
 }catch(...){return false;}
}
bool FrozenCameraCreationRegistry::PlainDepthCertificate::current()const noexcept {return owner_&&owner_->resolvePlainDepth(native_,variant_,device_,layout_,nullptr);}
bool FrozenCameraCreationRegistry::PlainDepthCertificate::resolve(PlainDepthDescription& out)const noexcept {return owner_&&owner_->resolvePlainDepth(native_,variant_,device_,layout_,&out);}
bool FrozenCameraCreationRegistry::lookupLayoutGeneration(VkPipelineLayout h,std::uint64_t&out)const noexcept {
 Errno preserve;try{
  if(!impl_||internalDepth)return false;auto&s=*impl_;std::lock_guard lock(s.mutex);
  if(!s.ready||s.reentryLost.load(std::memory_order_relaxed))return false;
  const auto it=s.activeLayouts.find(h);if(it==s.activeLayouts.end()||!it->second->alive||!it->second->generation)return false;
  out=it->second->generation;return true;
 }catch(...){return false;}
}
void FrozenCameraCreationRegistry::shutdownAfterIdle(VkDevice d,VkResult r)noexcept {Errno preserve;try{if(!impl_)return;auto&s=*impl_;std::lock_guard lock(s.mutex);if(d&&d==s.device&&(r==VK_SUCCESS||r==VK_ERROR_DEVICE_LOST)){Internal internal;s.clear(true);}}catch(...){} }
void FrozenCameraCreationRegistry::abandon(VkDevice d)noexcept {Errno preserve;try{if(!impl_)return;auto&s=*impl_;std::lock_guard lock(s.mutex);if(d&&d==s.device)s.clear(false);}catch(...){} }
}
