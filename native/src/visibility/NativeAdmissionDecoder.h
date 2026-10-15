#pragma once
// Native admission decoder: inventory/probe only. No hook installation, writes, Vulkan calls or skip authority.
#include <array>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <limits>
namespace rdr95 {
using Address=std::uint64_t;
inline constexpr Address MainDepthWaitReturn=0x2a66ac4;
inline constexpr unsigned MainRequest=13, RecordStride=0xb0;
enum class Family:unsigned { Unknown, StaticCommon, RigidMatrixSet, SkinnedMatrixSet, MixedMatrixSet, NativeInstanceList, SpecialOrCreature, Count };
enum class ModelFamily:unsigned { Unknown, Empty, Rigid, Skinned, Mixed };
enum class NodeBoundsDecline:unsigned { None,MissingNodeOrApi,VtableRead,VtableMismatch,NodeRead,VptrChanged,DrawableMismatch,WorldMismatch,AabbNumeric,Count };
enum class Error:unsigned { None, WrongBoundary, ReadFailed, NotReady, HeaderChanged, Budget, BadIndex, AmbiguousRecord };
struct Api { bool(*read)(Address,void*,std::size_t,void*){};void* user{}; Address instanceVtable{},drawableDraw{},drawableDrawMulti{};
 void* methodContext{};bool(*ordinaryMethod)(void*,Address point,Address observed,Address expected)noexcept{}; };
struct Ready { Address manager{},request{},container{},records{},buckets{},instanceList{}; std::uint16_t recordCount{},bucketCount{};std::uint32_t nativeFrame{}; };
struct Item { std::uint16_t record{},bucket{},flags{}; float fade{},secondaryFade{}; };
struct Group { std::uint16_t group{},lod{};Address drawable{};std::vector<std::uint16_t> records; };
struct Record {
 Address address{},drawable{},node{},matrixSet{},creature{};
 // Diagnostic value only, copied from the existing mismatch read. Never type authority.
 Address refusedNodeVtable{};
 std::array<float,12> world{},previousWorld{};
 std::array<float,3> minimum{},maximum{};
 std::uint32_t flags{},ambientOcclusion{};
 std::uint16_t index{},lod{},modelCount{},rigidModels{},skinnedModels{};std::uint32_t bucketReferences{},instanceReferences{};
 Family family{Family::Unknown};ModelFamily modelFamily{ModelFamily::Unknown};
 bool ordinaryFields{},worldFinite{},nodeBoundsCurrent{},drawMethodKnown{},nativeNodeExclusion{};
 NodeBoundsDecline nodeBoundsDecline{NodeBoundsDecline::None};
 // Separate complete-record-mesh target identity. Never source AABB authority.
 bool recordDrawableTargetCurrent{};
 bool targetNodeCurrent()const noexcept {return nodeBoundsCurrent||(recordDrawableTargetCurrent&&
  family==Family::StaticCommon&&modelFamily==ModelFamily::Rigid&&!matrixSet&&!creature&&!nativeNodeExclusion&&
  !(flags&0x1e000)&&worldFinite&&world==previousWorld&&nodeBoundsDecline==NodeBoundsDecline::DrawableMismatch);}
 // This only identifies the ordinary no-creature/no-special DrawCommon branch.
 // It is NOT material/opacity/deformation/temporal or whole-object skip proof.
 bool ordinaryStateOnlyCandidate{};
};
struct Inventory {
 Ready ready{};std::vector<Record> records;std::vector<Item> items;std::vector<Group> groups;
 std::array<unsigned,static_cast<unsigned>(Family::Count)> families{};
 std::uint64_t reads{},bytes{};Error error{Error::None};bool complete{};
};
class NativeCurrentRecordProvider;
class AdmissionDecoder {
 friend class NativeCurrentRecordProvider;
 friend class NativeCurrentRigidMatrices;
 friend class NativeSelectedLodGeometry;
 Api api_;Inventory* out_{};
 static bool plus(Address a,std::size_t n,Address& b) { if(n>std::numeric_limits<Address>::max()-a)return false;b=a+n;return true; }
 bool read(Address a,void* p,std::size_t n) { Address e{};if(!a||!plus(a,n,e)||!api_.read)return false;++out_->reads;out_->bytes+=n;return api_.read(a,p,n,api_.user); }
 template<class T> bool at(Address a,std::size_t o,T& v){Address p{};return plus(a,o,p)&&read(p,&v,sizeof(v));}
 template<class T,std::size_t N> static T get(const std::array<std::byte,N>& b,std::size_t o){T t{};std::memcpy(&t,b.data()+o,sizeof(t));return t;}
 template<std::size_t N> static std::array<float,12> matrix(const std::array<std::byte,N>& b,std::size_t o){std::array<float,12> r{};for(unsigned c=0;c<4;++c)for(unsigned j=0;j<3;++j)r[c*3+j]=get<float>(b,o+c*16+j*4);return r;}
 static bool finite(const std::array<float,12>& m){for(auto f:m)if(!std::isfinite(f))return false;return true;}
 bool fail(Error e){out_->error=e;out_->complete=false;return false;}
 bool header(Address mgr,Ready& r){
  r.manager=mgr;Address base{};if(!at(mgr,8,base)||!plus(base,MainRequest*0x730,r.request))return false;
  std::uint8_t ready{};if(!at(r.request,0xc2,ready)||!(ready&1))return false;
  if(!at(r.request,0xb8,r.nativeFrame)||!at(mgr,0x1f0+MainRequest*8,r.container))return false;
  if(!at(r.container,0,r.buckets)||!at(r.container,8,r.records)||!at(r.container,0x94,r.recordCount)||!at(r.container,0x98,r.bucketCount)||!at(r.container,0xa0,r.instanceList))return false;
  return (!r.recordCount||r.records)&&(!r.bucketCount||r.buckets);
 }
 bool methodKnown(bool multi,Address point,Address observed)const noexcept {
  const auto expected=multi?api_.drawableDrawMulti:api_.drawableDraw;
  if(!expected)return false;
  if(observed==expected)return true;
  return !multi&&api_.ordinaryMethod&&api_.ordinaryMethod(api_.methodContext,point,observed,expected);
 }
 void models(Record& r){
  Address vt{},draw{},holder{},ptr{};std::uint16_t count{};
  const bool multi=r.matrixSet&&r.family!=Family::NativeInstanceList;const auto expected=multi?api_.drawableDrawMulti:api_.drawableDraw; if(!r.drawable||!expected||!at(r.drawable,0,vt)||!at(vt,multi?0x38:0x30,draw)||!methodKnown(multi,vt,draw))return;
  r.drawMethodKnown=true;if(r.lod>=4||!at(r.drawable,0x50+r.lod*8,holder)||!holder||!at(holder,0,ptr)||!at(holder,8,count))return;
  if(count>128||(!ptr&&count))return;r.modelCount=count;
  for(unsigned i=0;i<count;++i){Address model{};std::uint8_t skin{};if(!at(ptr,i*8,model)||!model||!at(model,0x2d,skin))return; if(skin)++r.skinnedModels;else ++r.rigidModels;}
  r.modelFamily=count==0?ModelFamily::Empty:r.rigidModels&&r.skinnedModels?ModelFamily::Mixed:r.skinnedModels?ModelFamily::Skinned:ModelFamily::Rigid;
 }
 void bounds(Record& r,std::array<std::byte,0xf8>* observed=nullptr){
  r.nodeBoundsDecline=NodeBoundsDecline::MissingNodeOrApi;
  if(!r.node||!api_.instanceVtable)return;Address vt{};
  r.nodeBoundsDecline=NodeBoundsDecline::VtableRead;if(!at(r.node,0,vt))return;
  r.nodeBoundsDecline=NodeBoundsDecline::VtableMismatch;if(vt!=api_.instanceVtable){r.refusedNodeVtable=vt;return;}
  std::array<std::byte,0xf8> n{};r.nodeBoundsDecline=NodeBoundsDecline::NodeRead;if(!read(r.node,n.data(),n.size()))return;
  r.nodeBoundsDecline=NodeBoundsDecline::VptrChanged;if(get<Address>(n,0)!=vt)return;
  r.nativeNodeExclusion=(get<std::uint32_t>(n,0x2c)&0x100400)!=0;
  r.nodeBoundsDecline=NodeBoundsDecline::DrawableMismatch;
  if(r.drawable!=get<Address>(n,0xe8)&&r.drawable!=get<Address>(n,0xf0)){
   // Exact DrawCommon passes record+80 and record matrices to pinned DrawSingle.
   // The owned COMPLETE selected mesh, not this mismatched node AABB, bounds targets.
   // Keep every original typed field and native read; inspect only the copied node.
   if(!r.matrixSet&&!r.creature&&!r.nativeNodeExclusion&&!(r.flags&0x1e000)&&r.worldFinite&&
      r.world==r.previousWorld&&r.drawMethodKnown&&r.modelFamily==ModelFamily::Rigid&&matrix(n,0x60)==r.world){
    for(unsigned j=0;j<3;++j){const auto lo=get<float>(n,0xa0+4*j),hi=get<float>(n,0xb0+4*j);
     if(!std::isfinite(lo)||!std::isfinite(hi)||lo>hi)return;}
    r.recordDrawableTargetCurrent=true;if(observed)*observed=n;
   }
   return;
  }
  r.nodeBoundsDecline=NodeBoundsDecline::WorldMismatch;if(matrix(n,0x60)!=r.world)return;
  r.nodeBoundsDecline=NodeBoundsDecline::AabbNumeric;
  for(unsigned j=0;j<3;++j){r.minimum[j]=get<float>(n,0xa0+4*j);r.maximum[j]=get<float>(n,0xb0+4*j);if(!std::isfinite(r.minimum[j])||!std::isfinite(r.maximum[j])||r.minimum[j]>r.maximum[j])return;}
  if(observed)*observed=n;
  r.nodeBoundsCurrent=true;r.nodeBoundsDecline=NodeBoundsDecline::None;
 }
public:
 explicit AdmissionDecoder(Api api):api_(api){}
 bool decodeAfterMainDepthWait(Address mgr,unsigned request,Address returnOffset,bool primaryDepth,Inventory& out){
  out={};out_=&out;if(request!=MainRequest||returnOffset!=MainDepthWaitReturn||!primaryDepth)return fail(Error::WrongBoundary);
  // Caller invokes synchronously AFTER original WaitForVisibilityList returns on render owner.
  // The ready bit alone never substitutes for the join/sort; no calls or waits are made here.
  if(!header(mgr,out.ready))return fail(Error::NotReady);
  if(out.ready.bucketCount>32)return fail(Error::Budget);
  out.records.resize(out.ready.recordCount);
  for(unsigned i=0;i<out.ready.recordCount;++i){auto& r=out.records[i];r.index=static_cast<std::uint16_t>(i);if(!plus(out.ready.records,i*RecordStride,r.address))return fail(Error::BadIndex);}
  if(out.ready.instanceList){
   std::uint32_t count{};if(!at(out.ready.instanceList,0xdc40,count))return fail(Error::ReadFailed);if(count>48)return fail(Error::Budget);
   for(unsigned g=0;g<count;++g)for(unsigned lod=0;lod<4;++lod){
    std::uint8_t n{};if(!at(out.ready.instanceList,0x1580+g*4+lod,n))return fail(Error::ReadFailed);if(n>128)return fail(Error::Budget);if(!n)continue;
    Group group{};group.group=static_cast<std::uint16_t>(g);group.lod=static_cast<std::uint16_t>(lod);if(!at(out.ready.instanceList,0xd640+g*32+lod*8,group.drawable)||!group.drawable)return fail(Error::ReadFailed);
    group.records.resize(n);Address p{};if(!plus(out.ready.instanceList,0x1640+g*0x400+lod*0x100,p)||!read(p,group.records.data(),n*2))return fail(Error::ReadFailed);
    for(auto ix:group.records){if(ix>=out.records.size())return fail(Error::BadIndex);auto& r=out.records[ix];if(r.instanceReferences++)return fail(Error::AmbiguousRecord);r.drawable=group.drawable;r.lod=group.lod;r.family=Family::NativeInstanceList;}
    out.groups.push_back(std::move(group));
   }
  }
  for(unsigned b=0;b<out.ready.bucketCount;++b){
   Address h{},holder{},items{};std::uint16_t cap{},n{};if(!plus(out.ready.buckets,b*0x70,h)||!at(h,0,cap)||!at(h,4,n))return fail(Error::ReadFailed);
   if(n>cap||out.items.size()+n>262144)return fail(Error::Budget);if(!n)continue;
   if(!at(h,0x60,holder)||!at(holder,0,items))return fail(Error::ReadFailed);
   for(unsigned i=0;i<n;++i){std::array<std::byte,16> bytes{};Address p{};if(!plus(items,i*16,p)||!read(p,bytes.data(),bytes.size()))return fail(Error::ReadFailed);Item item{get<std::uint16_t>(bytes,0),static_cast<std::uint16_t>(b),get<std::uint16_t>(bytes,2),get<float>(bytes,8),get<float>(bytes,12)};
    if(item.record>=out.records.size())return fail(Error::BadIndex);auto& r=out.records[item.record];if(r.instanceReferences)return fail(Error::AmbiguousRecord);++r.bucketReferences;out.items.push_back(item);
   }
  }
  for(auto& r:out.records){
   std::array<std::byte,RecordStride> bytes{};if(!read(r.address,bytes.data(),bytes.size()))return fail(Error::ReadFailed);
   r.world=matrix(bytes,0);r.worldFinite=finite(r.world);r.ambientOcclusion=get<std::uint32_t>(bytes,0xa8);
   if(r.instanceReferences){r.matrixSet=get<Address>(bytes,0x90);models(r);++out.families[static_cast<unsigned>(r.family)];continue;}
   // Unreferenced slots can be failed admissions or leftovers. Never assign them skip authority.
   if(!r.bucketReferences){++out.families[0];continue;}
   r.ordinaryFields=true;r.previousWorld=matrix(bytes,0x40);r.drawable=get<Address>(bytes,0x80);r.node=get<Address>(bytes,0x88);r.matrixSet=get<Address>(bytes,0x90);r.creature=get<Address>(bytes,0x98);r.flags=get<std::uint32_t>(bytes,0xac);r.lod=static_cast<std::uint16_t>((r.flags>>10)&7);
   if(r.creature||(r.flags&0xe000)){r.family=Family::SpecialOrCreature;}
   else {models(r);bounds(r);if(r.drawMethodKnown&&r.worldFinite){
    if(!r.matrixSet){r.family=Family::StaticCommon;r.ordinaryStateOnlyCandidate=r.nodeBoundsCurrent&&!r.nativeNodeExclusion&&!(r.flags&0x10000)&&r.world==r.previousWorld&&r.modelFamily==ModelFamily::Rigid;}
    else if(r.modelFamily==ModelFamily::Rigid)r.family=Family::RigidMatrixSet;
    else if(r.modelFamily==ModelFamily::Skinned)r.family=Family::SkinnedMatrixSet;
    else if(r.modelFamily==ModelFamily::Mixed)r.family=Family::MixedMatrixSet;
   }}++out.families[static_cast<unsigned>(r.family)];
  }
  Ready final{};if(!header(mgr,final))return fail(Error::HeaderChanged);
  if(final.request!=out.ready.request||final.container!=out.ready.container||final.records!=out.ready.records||final.recordCount!=out.ready.recordCount||final.buckets!=out.ready.buckets||final.bucketCount!=out.ready.bucketCount||final.instanceList!=out.ready.instanceList||final.nativeFrame!=out.ready.nativeFrame)return fail(Error::HeaderChanged);
  out.complete=true;return true;
 }
 // Exact DrawOpaque row admission. The current Draw-container bucket selection and
 // callbacks remain native. This function does not authorize arbitrary bucket sources.
 static bool opaqueRowAllowed(const Record& record,const Item& item,unsigned categoryMask,unsigned exclusionMode,unsigned highBitEnabled,bool midRangeEnabled,bool lowRangeEnabled){
  const unsigned classify=(item.flags&0x8000)?highBitEnabled:(exclusionMode?0:(item.flags<0x4000?static_cast<unsigned>(midRangeEnabled):static_cast<unsigned>(lowRangeEnabled)));
  return classify!=0&&((record.flags>>24)&categoryMask)!=0;
 }
};
} // namespace rdr95


