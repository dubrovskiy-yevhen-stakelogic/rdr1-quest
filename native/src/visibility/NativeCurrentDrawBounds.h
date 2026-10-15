#pragma once
#include "NativeSelectedLodGeometry.h"
#include "engine/ColdRigidDrawBounds.h"
namespace rdr95 {
// PRIVATE connected native provider -> exact owned complete prefixes -> cold
// local model bounds -> current native palette -> whole current draw bounds.
// Does not install hooks and deliberately exposes NO safe-to-skip boolean.
// Pass side effects and material/pose native mutation leases are still required.
class NativeCurrentDrawBounds {
public:
 using Bounds=rdrvr::engine::ColdRigidDrawBounds;
 enum class DiagnosticStage:unsigned {None,Arguments,EntryHandoff,WarmRefresh,ColdLease,ColdCapture,BoundsBuild,ReuseBefore,ReuseAfter,NumericUpdate,Complete};
 DiagnosticStage diagnosticStage()const noexcept{return diagnosticStage_;}
 bool prepare(NativeCurrentRecordProvider& owner,const NativeSelectedLodGeometry::Api& api,
              unsigned record,Bounds::Pose pose,Bounds::Result& result,bool geometryOnly=false,const NativeReadRecord* handoff=nullptr){
  diagnosticStage_=DiagnosticStage::Arguments;result={};owner_=&owner;api_=&api;pose_=pose;
  // Pure copied numeric data only. Failure also expires the prepared graph so
  // the next successful call must rebuild instead of reviving a partial result.
  struct OnFailure {Bounds::Result& saved;bool& graphValid;Bounds& bound;bool done{};
   ~OnFailure(){if(!done){saved.valid=false;graphValid=false;bound.reset();}}
  } completion{numericResult_,graph_.valid,bound_};
  if(!pose.scene||pose.frame!=owner.frame()||!pose.owner||!pose.revision||generation_==UINT64_MAX)return false;
  diagnosticStage_=DiagnosticStage::EntryHandoff;
  if(handoff&&!handoff->available(owner,record,api.context))return false;
  diagnosticStage_=DiagnosticStage::WarmRefresh;
  bool transformsUnchanged=false;
  const bool warm=graph_.shaderCertified()!=geometryOnly&&NativeSelectedLodGeometry::refresh(owner,api,record,graph_,handoff,&transformsUnchanged);
  if(!warm){numericResult_.valid=false;bound_.reset();++coldBuilds_;
  // A failed warm journal may follow the mandatory final record re-read, which consumes the input
  // receipt. Rebuild with the strict fresh-read path only after validating the original transfer at
  // entry and its SAME live Read now.
  diagnosticStage_=DiagnosticStage::ColdLease;
  const NativeReadRecord* coldHandoff=handoff;
  if(handoff){if(!handoff->leaseCurrent(owner,record,api.context))return false;
   if(!handoff->available(owner,record,api.context))coldHandoff=nullptr;}
  diagnosticStage_=DiagnosticStage::ColdCapture;
  if(!(geometryOnly?NativeSelectedLodGeometry::captureGeometry(owner,api,record,graph_,coldHandoff):NativeSelectedLodGeometry::capture(owner,api,record,graph_,coldHandoff)))return false;
  asset_={{graph_.drawable,++generation_},{reinterpret_cast<std::uintptr_t>(this),generation_},graph_.lod};
  models_.clear();parts_.clear();geometryParts_.clear();models_.reserve(graph_.transforms.count);parts_.reserve(graph_.parts.size());
  for(unsigned m=0;m<graph_.transforms.count;++m){const auto& t=graph_.transforms.models[m];models_.push_back({m,t.paletteIndex,graph_.partCounts[m],Bounds::Position::RigidXYZ1});}
  for(const auto& p:graph_.parts){const auto& root=p.mesh.nodes[0];Bounds::PreboundedGeometry b{};
   b.model=p.modelOrdinal;b.part=p.partOrdinal;b.geometry={p.geometry,p.mesh.generation};if(!geometryOnly)b.positionCertificate={p.material,graph_.shaderSerial};b.indexCount=p.mesh.identity.indexCount;
   b.bounds.minimum={root.minimum.x,root.minimum.y,root.minimum.z};b.bounds.maximum={root.maximum.x,root.maximum.y,root.maximum.z};if(geometryOnly)geometryParts_.push_back({b.model,b.part,b.geometry,{p.mesh.identity.vertices.fvf,p.mesh.generation},b.bounds,b.indexCount});else parts_.push_back(b);
  }
  diagnosticStage_=DiagnosticStage::BoundsBuild;
  if(geometryOnly){if(!bound_.buildGeometryPrebounded(asset_,models_,geometryParts_,{this,authority,completeGeometry}))return false;}else if(!bound_.buildPrebounded(asset_,models_,parts_,{this,authority,nullptr,complete}))return false;
  }else ++warmRefreshes_;
  // Refresh has validated the current native graph and compared every active
  // model matrix bit-for-bit. Reuse only the math for this same compiled asset;
  // the old pose is never authority for the new frame.
  if(warm&&transformsUnchanged&&numericResult_.valid&&numericResult_.asset==asset_&&
     numericResult_.models==graph_.transforms.count){
   diagnosticStage_=DiagnosticStage::ReuseBefore;
   if(!current())return false;
   result=numericResult_;result.pose=pose_;
   diagnosticStage_=DiagnosticStage::ReuseAfter;
   if(!current()){result={};return false;}
   numericResult_.pose=pose_;++numericReuses_;completion.done=true;diagnosticStage_=DiagnosticStage::Complete;return true;
  }
  // Common one/two-model records initialize only their active transform span.
  // The rare larger LOD uses a fixed, bounded stack capacity.
  diagnosticStage_=DiagnosticStage::NumericUpdate;
  if(graph_.transforms.count==1)completion.done=update<1>(result);
  else if(graph_.transforms.count==2)completion.done=update<2>(result);
  else completion.done=update<128>(result);
  if(completion.done)diagnosticStage_=DiagnosticStage::Complete;
  return completion.done;
 }
 bool validateCurrentRecord(unsigned index,const NativeReadRecord* handoff=nullptr,const NativeGeometryStorageTicket*storage=nullptr,NativeReadRecord*verified=nullptr){
  if(verified)*verified={};
  auto failed=[&](){numericResult_.valid=false;return false;};
  if(!current())return failed();bool same=false;
  if(storage&&bool(*storage))return (NativeSelectedLodGeometry::fingerprint(*owner_,*api_,index,graph_,*storage,handoff,verified)&&current())?true:failed();
  if(!NativeSelectedLodGeometry::refresh(*owner_,*api_,index,graph_,handoff,&same))return failed();
  if(!same){graph_.valid=false;bound_.reset();return failed();}return current()?true:failed();
 }
 bool current()const noexcept{return owner_&&api_&&bound_.current(pose_);}
 const NativeSelectedLodGeometry::Output& graph()const noexcept{return graph_;}
 unsigned coldBuilds()const noexcept{return coldBuilds_;}
 unsigned warmRefreshes()const noexcept{return warmRefreshes_;}
 unsigned numericUpdates()const noexcept{return numericUpdates_;}
 unsigned numericReuses()const noexcept{return numericReuses_;}
 const Bounds::Asset& asset()const noexcept{return asset_;}
private:
 DiagnosticStage diagnosticStage_{DiagnosticStage::None};
 NativeCurrentRecordProvider* owner_{};const NativeSelectedLodGeometry::Api* api_{};
 NativeSelectedLodGeometry::Output graph_{};Bounds bound_{};Bounds::Asset asset_{};Bounds::Pose pose_{};
 std::vector<Bounds::GeometryPrebound> geometryParts_;std::vector<Bounds::Model> models_;std::vector<Bounds::PreboundedGeometry> parts_;std::uint64_t generation_{};unsigned coldBuilds_{},warmRefreshes_{},numericUpdates_{},numericReuses_{};Bounds::Result numericResult_{};
 template<std::size_t N> bool update(Bounds::Result& result){
  std::array<Bounds::ModelTransform,N> transforms{};
  if(!graph_.transforms.count||graph_.transforms.count>N)return false;
  for(unsigned m=0;m<graph_.transforms.count;++m){const auto& in=graph_.transforms.models[m];auto& out=transforms[m];out.model=m;out.paletteIndex=in.paletteIndex;out.world[15]=1;for(unsigned c=0;c<4;++c)for(unsigned r=0;r<3;++r)out.world[c*4+r]=in.modelToWorld[c*3+r];}
  ++numericUpdates_;
  if(!bound_.update(pose_,std::span(transforms.data(),graph_.transforms.count),result))return false;
  numericResult_=result;return true;
 }
 static bool authority(void* user,const Bounds::Asset& asset,const Bounds::Pose* pose)noexcept{
  const auto& s=*static_cast<const NativeCurrentDrawBounds*>(user);return s.owner_&&s.api_&&asset==s.asset_&&(!pose||*pose==s.pose_)&&NativeSelectedLodGeometry::current(*s.owner_,*s.api_,s.graph_);
 }
 static bool completeGeometry(void* user,const Bounds::Asset& asset,std::span<const Bounds::Model> models,std::span<const Bounds::GeometryPrebound> parts)noexcept{
  const auto&s=*static_cast<const NativeCurrentDrawBounds*>(user);if(!authority(user,asset,nullptr)||models.size()!=s.models_.size()||parts.size()!=s.geometryParts_.size())return false;
  for(unsigned i=0;i<models.size();++i){const auto&a=models[i],&b=s.models_[i];if(a.ordinal!=b.ordinal||a.paletteIndex!=b.paletteIndex||a.partCount!=b.partCount||a.position!=b.position)return false;}
  for(unsigned i=0;i<parts.size();++i){const auto&a=parts[i],&b=s.geometryParts_[i];if(a.model!=b.model||a.part!=b.part||a.geometry!=b.geometry||a.xyzLayout!=b.xyzLayout||a.bounds.minimum!=b.bounds.minimum||a.bounds.maximum!=b.bounds.maximum||a.indexCount!=b.indexCount||!s.api_->cache->current(s.graph_.parts[i].mesh))return false;}
  return true;
 }
 static bool complete(void* user,const Bounds::Asset& asset,std::span<const Bounds::Model> models,std::span<const Bounds::PreboundedGeometry> parts)noexcept{
  const auto& s=*static_cast<const NativeCurrentDrawBounds*>(user);if(!authority(user,asset,nullptr)||models.size()!=s.models_.size()||parts.size()!=s.parts_.size())return false;
  for(unsigned i=0;i<models.size();++i){const auto&a=models[i],&b=s.models_[i];if(a.ordinal!=b.ordinal||a.paletteIndex!=b.paletteIndex||a.partCount!=b.partCount||a.position!=b.position)return false;}
  for(unsigned i=0;i<parts.size();++i){const auto&a=parts[i],&b=s.parts_[i];const auto&p=s.graph_.parts[i];if(a.model!=b.model||a.part!=b.part||a.geometry!=b.geometry||a.positionCertificate!=b.positionCertificate||a.bounds.minimum!=b.bounds.minimum||a.bounds.maximum!=b.bounds.maximum||a.indexCount!=b.indexCount||!s.api_->cache->current(p.mesh)||!ColdMaterialVariants::current(*s.api_->materials,*p.position))return false;}
  return true;
 }
};
}
