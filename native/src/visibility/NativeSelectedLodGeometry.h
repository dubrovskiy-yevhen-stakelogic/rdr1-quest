#pragma once
#include "NativeCurrentRigidMatrices.h"
#include "ColdMaterialVariants.h"
#include "NativeGeometryStorageTicket.h"
#include "engine/OwnedStaticOccluders.h"
#include <algorithm>
#include "NativeGeometryReadJournal.h"
#ifndef RDR95_PACKED_GEOMETRY_JOURNAL
#define RDR95_PACKED_GEOMETRY_JOURNAL 1
#endif
namespace rdr95 {
// PRIVATE cold/admission adapter. Typed material receipts are mandatory; there
// is no "safe" bool supplied by runtime and no fabricated position certificate.
// This resolves the COMPLETE selected LOD, including currently mask-disabled
// parts (a conservative larger bound). It is not itself a native skip token.
class NativeSelectedLodGeometry {
 using Journal=NativeGeometryReadJournal<RDR95_PACKED_GEOMETRY_JOURNAL != 0>;
 struct Reader { AdmissionDecoder& native;Journal& journal;
  bool read(Address p,void* v,std::size_t n,bool storage=false){if(n>192||!native.read(p,v,n))return false;return journal.append(p,v,n,storage);}
  template<class T>bool at(Address p,unsigned off,T& v,bool storage=false){Address q{};return AdmissionDecoder::plus(p,off,q)&&read(q,&v,sizeof v,storage);}
 };
public:
 using Cache=rdrvr::engine::OwnedStaticOccluders;
 struct Api {
  Address geometryVtable{},vertexVtable{},indexVtable{},bufferVtable{};
  Address shadowBufferSlot{},forceShaderSlot{},shaderSorterSlot{},commandBufferSlot{};
  const Cache* cache{};std::uint64_t cacheEpoch{};
  const ColdMaterialVariants::Api* materials{};
  void* context{};
  const ColdMaterialVariants::Receipt*(*material)(void*,Address)noexcept{};
  // Actual native mutation-stable borrow; thread checks/headers are insufficient.
  bool(*ownerCurrent)(void*,std::uint64_t frame)noexcept{};
  const ColdMaterialVariants::Receipt*(*technique)(void*,Address,unsigned)noexcept{};
  // Production always installs this thunk; absent cached-bank provenance
  // returns false. A resource Read alone never certifies palette immutability.
  bool(*matrixSetCurrent)(void*,Address)noexcept{};
  // Production factory verifies the global GOT/range and exact pure leaf code.
  Address modelCullbackSlot{},pureModelCullback{};
 };
 enum class Failure{None,Unqualified,Owner,Record,Model,Callback,Material,Geometry,CacheMiss,Changed,Budget};
 enum class CacheDecline:unsigned {None,QueryDeclined,EmptyNodes,SnapshotChanged,PrefixIncomplete,RootNumeric};
 enum class CallbackDecline:unsigned {None,ForceRead,SorterRead,CommandRead,ForceActive,SorterActive,CommandActive,ModelCallback};
 struct Part {
  Address geometry{},material{},model{};unsigned modelOrdinal{},partOrdinal{};
  Cache::Snapshot mesh{};const ColdMaterialVariants::Receipt* position{};
 };
 struct Output {
  NativeCurrentRigidMatrices::Output transforms{};
  std::array<unsigned,128> partCounts{};std::vector<Part> parts{};
  std::uint64_t frame{},cacheSerial{},shaderSerial{},effectTicket{};Address record{},drawable{};unsigned lod{};
  unsigned meshQueries{};bool valid{};Failure failure{Failure::None};
  CacheDecline cacheDecline{CacheDecline::None};
  CallbackDecline callbackDecline{CallbackDecline::None};unsigned callbackGlobalMask{};
  unsigned modelCallbackDataChecks{};std::array<unsigned,7> modelCallbackResults{};
 private:
  friend class NativeSelectedLodGeometry;bool passesInherited_{},shaderCertified_{};Journal journal_;
  Address callbackSlot_{},pureCallback_{},callbackCode_{};
  Record preparedRecord_{};std::vector<std::array<float,12>> paletteRows_;std::array<float,3> paletteTranslation_{};std::uint8_t paletteCount_{};
 public:
  bool passesInherited()const noexcept{return valid&&passesInherited_;}
  bool shaderCertified()const noexcept{return valid&&shaderCertified_;}
  // Node roots enclose every index in each exact current geometry prefix.
  // The caller may feed these real prebounds into ColdRigidDrawBounds.
 };
 static bool capture(NativeCurrentRecordProvider& p,const Api&a,unsigned record,Output&o,const NativeReadRecord* handoff=nullptr){return captureImpl(p,a,record,o,true,handoff);}
 static bool captureGeometry(NativeCurrentRecordProvider& p,const Api&a,unsigned record,Output&o,const NativeReadRecord* handoff=nullptr){return captureImpl(p,a,record,o,false,handoff);}
 static bool current(NativeCurrentRecordProvider&,const Api&,const Output&);
 static bool fingerprint(NativeCurrentRecordProvider&,const Api&,unsigned,const Output&,const NativeGeometryStorageTicket&,const NativeReadRecord* =nullptr,NativeReadRecord* =nullptr);
 // Warm validation reads the ordered native pointer graph in dependency order.
 // No owned mesh query, triangle scan, vector allocation or shader graph build.
 static bool refresh(NativeCurrentRecordProvider&,const Api&,unsigned record,Output&,const NativeReadRecord* handoff=nullptr,bool* transformsUnchanged=nullptr);
private:
 template<std::size_t N>static bool refreshBounded(NativeCurrentRecordProvider&,const Api&,unsigned,Output&,const NativeReadRecord*,bool*);
 static bool captureImpl(NativeCurrentRecordProvider&,const Api&,unsigned,Output&,bool,const NativeReadRecord*);
 template<class T,std::size_t N>static T get(const std::array<std::byte,N>& b,unsigned o){T x{};std::memcpy(&x,b.data()+o,sizeof x);return x;}
 static bool identity(Reader&,const Api&,Address,Cache::Identity&);
 static bool capturePalette(NativeCurrentRecordProvider&,const Api&,const Record&,Output&);
 static bool sameRecord(const Record&,const Record&);
};
inline bool NativeSelectedLodGeometry::identity(Reader& d,const Api& a,Address geometry,Cache::Identity& id){
 Address vt{};if(!d.at(geometry,0,vt)||vt!=a.geometryVtable)return false;
 std::array<std::byte,0xc0> g{};std::uint8_t shadow{};if(!d.read(geometry,g.data(),g.size())||get<Address>(g,0)!=vt||!d.at(a.shadowBufferSlot,0,shadow))return false;
 const bool optimized=get<std::uint8_t>(g,0x14)&&shadow;
 if(get<std::uint8_t>(g,0x63)||get<Address>(g,0x88)||get<Address>(g,0xa0)||get<std::uint8_t>(g,0x62)!=3)return false;
 const auto v=get<Address>(g,optimized?0xb0:0x18),i=get<Address>(g,0x38),decl=get<Address>(g,optimized?0xa8:8);const auto n=get<std::uint32_t>(g,0x58);
 if(!decl||!n||n%3||n/3>Cache::MaxInputTriangles||!d.at(v,0,vt)||vt!=a.vertexVtable||!d.at(i,0,vt)||vt!=a.indexVtable)return false;
 std::array<std::byte,0x40> vb{};std::array<std::byte,0x28> ib{};
 if(!d.read(v,vb.data(),vb.size())||!d.read(i,ib.data(),ib.size())||get<Address>(vb,0)!=a.vertexVtable||get<Address>(ib,0)!=a.indexVtable)return false;
 const auto f=get<Address>(vb,0x30),nativeV=get<Address>(vb,0x38),nativeI=get<Address>(ib,0x18);
 if(get<std::uint8_t>(vb,0xa)||get<std::uint32_t>(ib,0x20)||!d.at(nativeV,0,vt,true)||vt!=a.bufferVtable||!d.at(nativeI,0,vt,true)||vt!=a.bufferVtable)return false;
 std::array<std::byte,0x28> nv{},ni{};std::array<std::byte,16> fvf{};
 if(!d.read(nativeV,nv.data(),nv.size(),true)||!d.read(nativeI,ni.data(),ni.size(),true)||!d.read(f,fvf.data(),fvf.size())||get<Address>(nv,0)!=a.bufferVtable||get<Address>(ni,0)!=a.bufferVtable||get<std::uint32_t>(nv,0x24)!=0||get<std::uint32_t>(ni,0x24)!=1)return false;
 const auto stride=get<std::uint32_t>(vb,0x18);const auto count=get<std::uint16_t>(vb,8);const auto indexCapacity=get<std::uint32_t>(ib,8);const auto vbytes=get<std::uint32_t>(nv,0x20);const auto ibytes=get<std::uint32_t>(ni,0x20);const auto format=get<std::uint64_t>(fvf,8)&15;
 if(!count||!stride||stride>255||stride%4||!(get<std::uint32_t>(fvf,0)&1)||get<std::uint8_t>(fvf,4)!=stride||(format!=6&&format!=7)||stride<(format==6?12u:16u)||n>indexCapacity||std::uint64_t(indexCapacity)*2>ibytes||std::uint64_t(count)*stride>vbytes)return false;
 id={{v,nativeV,get<Address>(nv,0x18),f,get<std::uint64_t>(nv,0x10),fvf,vbytes,count,stride},i,nativeI,get<Address>(ni,0x18),get<std::uint64_t>(ni,0x10),ibytes,n};
 return id.vertices.allocation&&id.indexAllocation&&id.vertices.gpuBuffer&&id.indexGpuBuffer;
}
inline bool NativeSelectedLodGeometry::captureImpl(NativeCurrentRecordProvider& owner,const Api& a,unsigned index,Output& out,bool shaderRequired,const NativeReadRecord* handoff){
 out.valid=false;out.meshQueries=0;out.journal_.clear();out.passesInherited_=shaderRequired;out.shaderCertified_=shaderRequired;out.parts.clear();out.partCounts.fill(0);out.failure=Failure::None;out.cacheDecline=CacheDecline::None;out.callbackDecline=CallbackDecline::None;out.callbackGlobalMask=0;out.modelCallbackDataChecks=0;out.modelCallbackResults.fill(0);out.callbackSlot_=out.pureCallback_=out.callbackCode_=0;
 auto fail=[&](Failure f){out.failure=f;return false;};
 if(!owner.leased()||!a.cache||!a.cacheEpoch||!a.geometryVtable||!a.vertexVtable||!a.indexVtable||!a.bufferVtable||!a.shadowBufferSlot||!a.forceShaderSlot||!a.shaderSorterSlot||!a.commandBufferSlot||!a.materials||!a.material||!a.ownerCurrent)return fail(Failure::Unqualified);
 if(!a.ownerCurrent(a.context,owner.frame()))return fail(Failure::Owner);
 Reader d{owner.decoder_,out.journal_};Address force{},sorter{},command{};
 // All three reads still precede value guards, exactly as the parent OR chain.
 if(!d.at(a.forceShaderSlot,0,force)){out.callbackDecline=CallbackDecline::ForceRead;return fail(Failure::Callback);}
 if(!d.at(a.shaderSorterSlot,0,sorter)){out.callbackDecline=CallbackDecline::SorterRead;return fail(Failure::Callback);}
 if(!d.at(a.commandBufferSlot,0,command)){out.callbackDecline=CallbackDecline::CommandRead;return fail(Failure::Callback);}
 out.callbackGlobalMask=unsigned(force!=0)|(unsigned(sorter!=0)<<1)|(unsigned(command!=0)<<2);
 if(force){out.callbackDecline=CallbackDecline::ForceActive;return fail(Failure::Callback);}
 if(sorter){out.callbackDecline=CallbackDecline::SorterActive;return fail(Failure::Callback);}
 if(command){out.callbackDecline=CallbackDecline::CommandActive;return fail(Failure::Callback);}
 Record r{};if(!(handoff?handoff->copy(owner,index,a.context,r):owner.record(index,r))||r.creature||r.nativeNodeExclusion||!r.targetNodeCurrent()||(r.family!=Family::StaticCommon&&r.family!=Family::RigidMatrixSet)||!NativeCurrentRigidMatrices::capture(owner,index,out.transforms,a.context,a.matrixSetCurrent,handoff))return fail(Failure::Record);
 Address group{},materials{};std::uint16_t materialCount{};
 if(!d.at(r.drawable,0x10,group)||!d.at(group,0x40,materials)||!d.at(group,0x48,materialCount)||!materialCount||!materials)return fail(Failure::Material);
 out.effectTicket=a.materials->effectEpoch?a.materials->effectEpoch->ticket():0;
 out.cacheSerial=a.cache->invalidationSerial();out.shaderSerial=a.materials->shaderSerial(*a.materials->registry);
 if(!out.effectTicket||!out.cacheSerial||!out.shaderSerial)return fail(Failure::Owner);
 for(unsigned m=0;m<out.transforms.count;++m){const auto model=out.transforms.models[m].native;std::array<std::byte,0x30> header{};if(!d.read(model,header.data(),header.size()))return fail(Failure::Model);
  const auto array=get<Address>(header,8),materialIndices=get<Address>(header,0x20);const auto n=get<std::uint16_t>(header,0x2e);
  if(get<Address>(header,0x18)){
   ++out.modelCallbackDataChecks;
   auto decline=[&](unsigned reason,Failure failure){++out.modelCallbackResults[reason];out.callbackDecline=CallbackDecline::ModelCallback;return fail(failure);};
   if(!a.modelCullbackSlot||!a.pureModelCullback)return decline(1,Failure::Callback);
   Address callback{};if(!d.at(a.modelCullbackSlot,0,callback))return decline(2,Failure::Callback);
   if(callback&&callback!=a.pureModelCullback)return decline(3,Failure::Callback);
   if(out.callbackSlot_&&out.callbackCode_!=callback)return decline(6,Failure::Changed);
   ++out.modelCallbackResults[callback?5:4];
   out.callbackSlot_=a.modelCullbackSlot;out.pureCallback_=a.pureModelCullback;out.callbackCode_=callback;
  }
  if(get<std::uint8_t>(header,0x2d)||!n||n>256||out.parts.size()+n>8192||!array||!materialIndices)return fail(Failure::Model);
  out.partCounts[m]=n;
  for(unsigned j=0;j<n;++j){Address geometry{},material{};std::uint16_t mi{};
   if(!d.at(array,j*8,geometry)||!d.at(materialIndices,j*2,mi)||mi>=materialCount||!d.at(materials,mi*8,material)||!material)return fail(Failure::Material);
   Address effect{};if(!d.at(material,0x10,effect))return fail(Failure::Material);
   const ColdMaterialVariants::Receipt* proof=nullptr;
   if(shaderRequired){proof=a.material(a.context,material);if(!proof||!*proof||!proof->allTechniques()||proof->material()!=material||proof->effect()!=effect||proof->positionLocation()!=0||proof->effectTicket()!=out.effectTicket||proof->shaderSerial()!=out.shaderSerial||!ColdMaterialVariants::current(*a.materials,*proof))return fail(Failure::Material);out.passesInherited_ &= proof->noRenderStateOverrides();}
   Cache::Identity id{};if(!identity(d,a,geometry,id))return fail(Failure::Geometry);Part part{};part.geometry=geometry;part.material=material;part.model=model;part.modelOrdinal=m;part.partOrdinal=j;part.position=proof;
   ++out.meshQueries;
   if(!a.cache->querySnapshot(a.cacheEpoch,id,part.mesh)){out.cacheDecline=CacheDecline::QueryDeclined;return fail(Failure::CacheMiss);}
   if(part.mesh.nodes.empty()){out.cacheDecline=CacheDecline::EmptyNodes;return fail(Failure::CacheMiss);}
   if(!a.cache->current(part.mesh)){out.cacheDecline=CacheDecline::SnapshotChanged;return fail(Failure::CacheMiss);}
   // Strong complete-prefix accounting. Degenerate triangles are retained by
   // cold preprocessing; merges preserve exact convex unions, never a hull.
   std::uint64_t covered=0;for(const auto& patch:part.mesh.patches)covered+=patch.sourceTriangleCount;
   if(covered!=id.indexCount/3){out.cacheDecline=CacheDecline::PrefixIncomplete;return fail(Failure::CacheMiss);}
   for(unsigned k=0;k<3;++k){const auto& b=part.mesh.nodes[0];const float lo=k==0?b.minimum.x:k==1?b.minimum.y:b.minimum.z,hi=k==0?b.maximum.x:k==1?b.maximum.y:b.maximum.z;if(!std::isfinite(lo)||!std::isfinite(hi)||lo>hi){out.cacheDecline=CacheDecline::RootNumeric;return fail(Failure::CacheMiss);}}
   out.parts.push_back(std::move(part));
  }
 }
 if(!a.materials->effectEpoch->current(out.effectTicket)||!a.ownerCurrent(a.context,owner.frame())||!owner.current()||out.cacheSerial!=a.cache->invalidationSerial()||!a.materials->shaderSetCurrent(*a.materials->registry,out.shaderSerial))return fail(Failure::Changed);
 if(handoff&&!handoff->leaseCurrent(owner,index,a.context))return fail(Failure::Changed);
 if(!capturePalette(owner,a,r,out))return fail(Failure::Changed);
 out.preparedRecord_=r;out.frame=owner.frame();out.record=r.address;out.drawable=r.drawable;out.lod=r.lod;out.valid=true;return true;
}
inline bool NativeSelectedLodGeometry::refresh(NativeCurrentRecordProvider& owner,const Api&a,unsigned index,Output&out,const NativeReadRecord* handoff,bool* transformsUnchanged){
 // Previous count selects scratch only. Capture re-reads the actual count,
 // pointers, model fields and palette; changed count fails the warm path.
 if(out.callbackSlot_&&(out.callbackSlot_!=a.modelCullbackSlot||out.pureCallback_!=a.pureModelCullback))return false;
 if(out.transforms.count==1)return refreshBounded<1>(owner,a,index,out,handoff,transformsUnchanged);
 if(out.transforms.count==2)return refreshBounded<2>(owner,a,index,out,handoff,transformsUnchanged);
 return refreshBounded<128>(owner,a,index,out,handoff,transformsUnchanged);
}
template<std::size_t N>inline bool NativeSelectedLodGeometry::refreshBounded(NativeCurrentRecordProvider& owner,const Api&a,unsigned index,Output&out,const NativeReadRecord* handoff,bool* transformsUnchanged){
 if(transformsUnchanged)*transformsUnchanged=false;
 if(!out.valid||!owner.leased()||!a.cache||!a.materials||!a.materials->effectEpoch||!a.materials->effectEpoch->current(out.effectTicket)||!a.ownerCurrent||!a.ownerCurrent(a.context,owner.frame())||a.cache->invalidationSerial()!=out.cacheSerial||!a.materials->shaderSetCurrent(*a.materials->registry,out.shaderSerial))return false;
 Record r{};if(!(handoff?handoff->copy(owner,index,a.context,r):owner.record(index,r))||r.drawable!=out.drawable||r.lod!=out.lod||r.creature||r.nativeNodeExclusion||!r.targetNodeCurrent()||(r.family!=Family::StaticCommon&&r.family!=Family::RigidMatrixSet))return false;
 NativeCurrentRigidMatrices::BoundedOutput<N> matrices{};if(!NativeCurrentRigidMatrices::capture(owner,index,matrices,a.context,a.matrixSetCurrent,handoff)||matrices.count!=out.transforms.count)return false;
 for(unsigned m=0;m<matrices.count;++m)if(matrices.models[m].native!=out.transforms.models[m].native||matrices.models[m].paletteIndex!=out.transforms.models[m].paletteIndex)return false;
 // Parent links appear BEFORE children. On a changed pointer the walk stops
 // before dereferencing the stale cached child; current model roots were checked above.
 std::array<std::byte,192> scratch{};
 for(const auto& q:out.journal_){const auto bytes=out.journal_.bytes(q);if(bytes.size()>scratch.size()||!owner.decoder_.read(q.address,scratch.data(),bytes.size())||std::memcmp(scratch.data(),bytes.data(),bytes.size()))return false;}
 for(const auto& part:out.parts)if(!a.cache->current(part.mesh)||(out.shaderCertified_&&(!part.position||!ColdMaterialVariants::current(*a.materials,*part.position))))return false;
 if(!a.materials->effectEpoch->current(out.effectTicket)||!a.ownerCurrent(a.context,owner.frame())||a.cache->invalidationSerial()!=out.cacheSerial||!a.materials->shaderSetCurrent(*a.materials->registry,out.shaderSerial)||!owner.current())return false;
 if(handoff&&!handoff->leaseCurrent(owner,index,a.context))return false;
 if(transformsUnchanged){
  const auto& old=out.transforms;bool same=old.count==matrices.count&&old.drawable==matrices.drawable&&old.matrixSet==matrices.matrixSet;
  for(unsigned m=0;same&&m<matrices.count;++m){const auto& x=old.models[m];const auto& y=matrices.models[m];same=x.native==y.native&&x.paletteIndex==y.paletteIndex&&std::memcmp(x.modelToWorld.data(),y.modelToWorld.data(),sizeof(x.modelToWorld))==0;}
  *transformsUnchanged=same;
 }
 // Only count active models are observable. Preserve inactive storage and
 // publish the same metadata, without constructing/copying a 128-model scratch.
 std::copy_n(matrices.models.begin(),matrices.count,out.transforms.models.begin());
 out.transforms.frame=matrices.frame;out.transforms.record=matrices.record;out.transforms.drawable=matrices.drawable;
 out.transforms.matrixSet=matrices.matrixSet;out.transforms.count=matrices.count;out.transforms.valid=matrices.valid;
 if(!capturePalette(owner,a,r,out))return false;
 out.preparedRecord_=r;out.frame=owner.frame();out.record=r.address;out.failure=Failure::None;return true;
}
inline bool NativeSelectedLodGeometry::current(NativeCurrentRecordProvider& owner,const Api& a,const Output& out){
 if(!(out.valid&&owner.leased()&&owner.frame()==out.frame&&a.ownerCurrent&&a.ownerCurrent(a.context,out.frame)&&a.cache&&a.cache->invalidationSerial()==out.cacheSerial&&a.materials&&a.materials->effectEpoch&&a.materials->effectEpoch->current(out.effectTicket)&&a.materials->registry&&a.materials->shaderSetCurrent&&a.materials->shaderSetCurrent(*a.materials->registry,out.shaderSerial)&&owner.current()))return false;
 if(out.callbackSlot_){
  if(out.callbackSlot_!=a.modelCullbackSlot||out.pureCallback_!=a.pureModelCullback)return false;
  Address callback{};if(!owner.decoder_.at(out.callbackSlot_,0,callback)||callback!=out.callbackCode_||(callback&&callback!=a.pureModelCullback))return false;
 }
 return true;
}
inline bool NativeSelectedLodGeometry::sameRecord(const Record&a,const Record&b){
 return a.address==b.address&&a.index==b.index&&a.node==b.node&&a.drawable==b.drawable&&a.matrixSet==b.matrixSet&&a.creature==b.creature&&
  a.world==b.world&&a.previousWorld==b.previousWorld&&a.minimum==b.minimum&&a.maximum==b.maximum&&a.flags==b.flags&&a.ambientOcclusion==b.ambientOcclusion&&
  a.lod==b.lod&&a.modelCount==b.modelCount&&a.rigidModels==b.rigidModels&&a.skinnedModels==b.skinnedModels&&a.family==b.family&&a.modelFamily==b.modelFamily&&
  a.ordinaryFields==b.ordinaryFields&&a.worldFinite==b.worldFinite&&a.nodeBoundsCurrent==b.nodeBoundsCurrent&&a.drawMethodKnown==b.drawMethodKnown&&
  a.nativeNodeExclusion==b.nativeNodeExclusion&&a.ordinaryStateOnlyCandidate==b.ordinaryStateOnlyCandidate&&a.recordDrawableTargetCurrent==b.recordDrawableTargetCurrent;
}
inline bool NativeSelectedLodGeometry::capturePalette(NativeCurrentRecordProvider&owner,const Api&a,const Record&r,Output&out){
 out.paletteRows_.clear();out.paletteCount_=0;out.paletteTranslation_={};
 if(!r.matrixSet)return true;
 // Unqualified palettes take the full-validation path. Never
 // infer a cached-bank lease merely from a pointer or the resource Read.
 if(!a.matrixSetCurrent||!a.matrixSetCurrent(a.context,r.matrixSet))return true;
 auto&d=owner.decoder_;if(!d.at(r.matrixSet,0,out.paletteCount_)||!out.paletteCount_||
    !d.at(r.matrixSet,0x10,out.paletteTranslation_))return false;
 out.paletteRows_.resize(out.transforms.count);
 for(unsigned i=0;i<out.transforms.count;++i){const auto index=out.transforms.models[i].paletteIndex;Address address{};
  if(index>=out.paletteCount_||!AdmissionDecoder::plus(r.matrixSet,0xdc+std::size_t(index)*48,address)||
     !d.read(address,out.paletteRows_[i].data(),48))return false;}
 return a.matrixSetCurrent(a.context,r.matrixSet)&&a.ownerCurrent(a.context,owner.frame())&&
  a.cache->invalidationSerial()==out.cacheSerial&&a.materials->effectEpoch->current(out.effectTicket)&&
  a.materials->shaderSetCurrent(*a.materials->registry,out.shaderSerial);
}
inline bool NativeSelectedLodGeometry::fingerprint(NativeCurrentRecordProvider&owner,const Api&a,unsigned index,const Output&out,
 const NativeGeometryStorageTicket&storage,const NativeReadRecord*handoff,NativeReadRecord*verified){
 if(verified)*verified={};
 if(!storage.matches(owner.frame(),a.context)||!current(owner,a,out))return false;
 Record r{};if(!(handoff?handoff->copy(owner,index,a.context,r):owner.record(index,r))||!sameRecord(r,out.preparedRecord_))return false;
 auto&d=owner.decoder_;Address holder{},array{};std::uint16_t count{};
 if(r.lod>=4||!d.at(r.drawable,0x50+r.lod*8,holder)||!holder||!d.at(holder,0,array)||!array||
    !d.at(holder,8,count)||!count||count!=out.transforms.count||count>128)return false;
 std::array<Address,128> models{};if(!d.read(array,models.data(),std::size_t(count)*8))return false;
 for(unsigned i=0;i<count;++i)if(models[i]!=out.transforms.models[i].native)return false;
 // All native parent links, model bytes, selected wrapper fields and FVF stay
 // fresh and ordered. Only the audited backing object's ctor/dtor-only bytes
 // are omitted under the factory's explicit installed-owner receipt.
 std::array<std::byte,192> scratch{};
 for(const auto&q:out.journal_){if(q.backingStorage)continue;const auto bytes=out.journal_.bytes(q);
  if(bytes.size()>scratch.size()||!d.read(q.address,scratch.data(),bytes.size())||std::memcmp(scratch.data(),bytes.data(),bytes.size()))return false;}
 if(r.matrixSet){
  if(!a.matrixSetCurrent||out.paletteRows_.size()!=count||!a.matrixSetCurrent(a.context,r.matrixSet))return false;
  std::uint8_t n{};std::array<float,3> translation{};
  if(!d.at(r.matrixSet,0,n)||n!=out.paletteCount_||!d.at(r.matrixSet,0x10,translation)||translation!=out.paletteTranslation_)return false;
  for(unsigned i=0;i<count;++i){Address address{};std::array<float,12> row{};
   if(!AdmissionDecoder::plus(r.matrixSet,0xdc+std::size_t(out.transforms.models[i].paletteIndex)*48,address)||
      !d.read(address,row.data(),48)||row!=out.paletteRows_[i])return false;}
  if(!a.matrixSetCurrent(a.context,r.matrixSet))return false;
 }
 for(const auto&part:out.parts)if(!a.cache->current(part.mesh)||(out.shaderCertified_&&(!part.position||!ColdMaterialVariants::current(*a.materials,*part.position))))return false;
 // The model graph above is fresh; do not parse it a second and third time
 // merely to reproduce the same Record. Compare actual raw/typed-node bytes,
 // including the drawable vptr/Draw slot which is outside the model journal.
 Record finalRecord{};
 const bool matches=handoff?(handoff->snapshot_&&owner.compareRecordSnapshot(index,*handoff->snapshot_)):
    (owner.record(index,finalRecord)&&sameRecord(r,finalRecord));
 const auto finalProviderRevision=owner.revision(),finalRecordRevision=owner.recordRevision();
 if(!matches||!storage.matches(owner.frame(),a.context)||
    (handoff&&!handoff->leaseCurrent(owner,index,a.context))||!current(owner,a,out)||
    !finalProviderRevision||!finalRecordRevision||owner.revision()!=finalProviderRevision||owner.recordRevision()!=finalRecordRevision)return false;
 if(verified&&handoff){*verified=*handoff;verified->snapshot_=nullptr;verified->recordRevision_=finalRecordRevision;}
 return true;
}
} // namespace rdr95
