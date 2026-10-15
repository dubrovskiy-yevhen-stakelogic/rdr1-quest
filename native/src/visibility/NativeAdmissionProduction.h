#pragma once
#include "NativeCurrentAdmission.h"
#include "ColdSelectedTechniqueCache.h"
#include "NativeProductionOffsets.h"
#include "NativeProductionProof.h"
namespace rdr95 {
// PRIVATE binding factory. Code/range verification does NOT create ownership:
// the reader and callbacks must enforce the actual current native owner lease.
// configure is cold and must run outside any borrowed draw scope. No hooks,
// TLS lookup, command/query claims or engine calls are performed here.
class NativeAdmissionProduction {
public:
 struct ImageProof {
  void* context{};
  bool(*code)(void*,Address,unsigned,const char* sha256)noexcept{};
  bool(*range)(void*,Address,unsigned,unsigned requiredElfFlags)noexcept{};
 };
 struct Dependencies {
  rdr95::Api memory{};ColdMaterialVariants::Registry* registry{};VkDevice device{};
  const NativeSelectedLodGeometry::Cache* meshes{};std::uint64_t meshEpoch{};
  const EffectMutationEpoch* effects{};Address matrixBuffer{};
  ColdMaterialVariants::Owner owner{};void* context{};
  bool(*frameCurrent)(void*,std::uint64_t)noexcept{};
  bool(*materialCurrent)(void*,ColdMaterialVariants::Owner,Address,Address)noexcept{};
  bool(*matrixSetCurrent)(void*,Address)noexcept{};
 };
 bool configure(Address image,const ImageProof& proof,const Dependencies& d){
  clear();if(!image||!proof.code||!proof.range||!d.memory.read||!d.registry||!d.device||!d.meshes||!d.meshEpoch||!d.effects||!d.matrixBuffer||!d.owner.identity||!d.owner.generation||!d.frameCurrent||!d.materialCurrent)return false;
  for(const auto&p:retail::CodePins)if(p.offset>UINT64_MAX-image||p.size>UINT64_MAX-(image+p.offset)||!proof.code(proof.context,image+p.offset,p.size,p.sha256))return false;
  for(const auto&p:retail::BindingRanges)if(p.offset>UINT64_MAX-image||p.size>UINT64_MAX-(image+p.offset)||!proof.range(proof.context,image+p.offset,p.size,p.flags))return false;
  // The native bodies dereference relocated data slots. A redirected GOT may
  // make an otherwise exact body use a different selector/state table.
  for(const auto&p:retail::GlobalGotPins){Address actual{};if(image>UINT64_MAX-sizeof(Address)||p.slot>UINT64_MAX-image-sizeof(Address)||p.target>UINT64_MAX-image||!proof.range(proof.context,image+p.slot,sizeof(Address),4)||!d.memory.read(image+p.slot,&actual,sizeof actual,d.memory.user)||actual!=image+p.target)return false;}
  auto at=[&](Address p){return image+p;};deps_=d;memory_=d.memory;memory_.instanceVtable=at(retail::instanceVtable);memory_.drawableDraw=at(retail::drawableDraw);memory_.drawableDrawMulti=at(retail::drawableDrawMulti);
  materials_={memory_,d.registry,d.device,at(retail::vertexShaderVtable),at(retail::fragmentShaderVtable),at(retail::programVtable),d.matrixBuffer,d.context,d.materialCurrent,shaderSerial,shaderCurrent,d.effects};
  if(!techniques_.configure(materials_,d.owner))return false;
  selection_={memory_,d.effects,at(retail::forcedGroup),at(retail::litGroup),at(retail::unlitGroup),at(retail::shadow),at(retail::fallbackGroup),at(retail::fallbackEnabled),at(retail::shadowBufferSlot),at(retail::minimumFade)};
  geometry_={at(retail::geometryVtable),at(retail::vertexVtable),at(retail::indexVtable),at(retail::bufferVtable),at(retail::shadowBufferSlot),at(retail::forceShaderSlot),at(retail::shaderSorterSlot),at(retail::commandBufferSlot),d.meshes,d.meshEpoch,&materials_,this,allMaterial,frameCurrent,technique,matrixSetCurrent,at(retail::modelCullbackSlot),at(retail::pureModelCullback)};
  state_={memory_,at(retail::gfxVtable),at(retail::vkVtable),at(retail::rasterVtable),at(retail::depthVtable),at(retail::pendingRaster),at(retail::pendingDepth),at(retail::readyFlags),at(retail::defaults),at(retail::shadow),at(retail::globalBias),at(retail::markDynamic),at(retail::factoryDirty),&selection_};
  ready_=true;return true;
 }
 void clear()noexcept{ready_=false;techniques_.clear();deps_={};memory_={};materials_={};selection_={};geometry_={};state_={};}
 const rdr95::Api* memory()const noexcept{return ready_?&memory_:nullptr;}
 const NativeSelectedLodGeometry::Api* geometry()const noexcept{return ready_?&geometry_:nullptr;}
 const NativeCurrentFixedState::Api* fixedState()const noexcept{return ready_?&state_:nullptr;}
 const NativeTechniqueSelection::Api* selection()const noexcept{return ready_?&selection_:nullptr;}
 const ColdMaterialVariants::Api* materials()const noexcept{return ready_?&materials_:nullptr;}
 const ColdSelectedTechniqueCache::Statistics& techniqueStatistics()const noexcept{return techniques_.statistics();}
private:
 static std::uint64_t shaderSerial(const ColdMaterialVariants::Registry&r)noexcept{return r.shaderMutationSerial();}
 static bool shaderCurrent(const ColdMaterialVariants::Registry&r,std::uint64_t n)noexcept{return r.currentShaderSet(n);}
 static bool frameCurrent(void*u,std::uint64_t frame)noexcept{const auto&s=*static_cast<const NativeAdmissionProduction*>(u);return s.ready_&&s.deps_.frameCurrent&&s.deps_.frameCurrent(s.deps_.context,frame);}
 static const ColdMaterialVariants::Receipt* technique(void*u,Address m,unsigned t)noexcept{return static_cast<NativeAdmissionProduction*>(u)->techniques_.lookup(m,t);}
 static bool matrixSetCurrent(void*u,Address p)noexcept{const auto&s=*static_cast<const NativeAdmissionProduction*>(u);return s.ready_&&s.deps_.matrixSetCurrent&&s.deps_.matrixSetCurrent(s.deps_.context,p);}
 // Production uses geometry-only prepare + exact late technique, never an
 // invented all-material certificate. The legacy strict capture stays closed.
 static const ColdMaterialVariants::Receipt* allMaterial(void*,Address)noexcept{return nullptr;}
 Dependencies deps_{};rdr95::Api memory_{};ColdMaterialVariants::Api materials_{};NativeTechniqueSelection::Api selection_{};NativeSelectedLodGeometry::Api geometry_{};NativeCurrentFixedState::Api state_{};ColdSelectedTechniqueCache techniques_;bool ready_{};
};
}
