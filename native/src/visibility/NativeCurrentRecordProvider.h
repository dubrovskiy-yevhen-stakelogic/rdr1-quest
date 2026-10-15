#pragma once
#include "NativeAdmissionDecoder.h"
#include <thread>
namespace rdr95 {
// Live provider, no installed hook. A read callback is mandatory.
// Production use may borrow only the synchronous current native render-owner
// lease after the complete original wait. This class never guesses memory readability.
class NativeCurrentRecordProvider {
public:
 class HeaderScope;
 // Opaque bytes from one successful parse, NOT a native memory/Read permit.
 // Capturing bytes grants nothing; only SelectedLodGeometry may compare them, after
 // freshly checking all selected-model links/skin bytes in its own journal.
 class RecordSnapshot {
  friend class NativeCurrentRecordProvider;
  const NativeCurrentRecordProvider* provider_{};
  std::uint64_t revision_{},recordRevision_{},frame_{};unsigned index_{};
  Address address_{},node_{},drawable_{};bool multi_{},valid_{};
  std::array<std::byte,RecordStride> raw_{};
  std::array<std::byte,0xf8> nodeBytes_{};
 public:RecordSnapshot()noexcept=default;
 };
private:
 friend class NativeCurrentAdmission;
 friend struct NativeRecordSnapshotTestAccess;
 friend class NativeCurrentRigidMatrices;
 friend class NativeSelectedLodGeometry;
 AdmissionDecoder decoder_; Inventory accounting_{}; Ready ready_{};
 std::array<std::uint64_t,1024> common_{},instance_{};
 std::thread::id owner_{};std::uint64_t frame_{},revision_{},recordRevision_{};bool active_{};HeaderScope* header_{};
 static void advance(std::uint64_t& n)noexcept{if(n!=UINT64_MAX)++n;}
 static bool bit(const auto& a,unsigned n){return (a[n/64]>>(n%64))&1;}
 static void set(auto& a,unsigned n){a[n/64]|=std::uint64_t(1)<<(n%64);}
 bool leased()const{return active_&&owner_==std::this_thread::get_id();}
 bool decline(Error e){accounting_.error=e;active_=false;return false;}
 // Final compare only inside the same externally certified Read. Unlike a
 // second record(), it does not repeat model-family parsing. The caller must
 // already have compared the complete selected model journal this attempt.
 bool compareRecordSnapshot(unsigned index,const RecordSnapshot& observed){
  if(!leased()||!observed.valid_||observed.provider_!=this||observed.revision_!=revision()||
     observed.recordRevision_!=recordRevision()||observed.frame_!=frame_||observed.index_!=index||
     !ordinary(index)||!current())return false;
  Address address{};
  if(!AdmissionDecoder::plus(ready_.records,index*RecordStride,address)||address!=observed.address_)return false;
  advance(recordRevision_);const auto consumed=recordRevision();if(!consumed)return false;
  std::array<std::byte,RecordStride> raw{};
  if(!decoder_.read(address,raw.data(),raw.size())||raw!=observed.raw_)return false;
  // These two drawable checks are NOT in the geometry journal. Preserve them
  // rather than treating a stable drawable pointer as draw-method authority.
  Address vt{},draw{};const auto expected=observed.multi_?decoder_.api_.drawableDrawMulti:decoder_.api_.drawableDraw;
  if(!expected||!decoder_.at(observed.drawable_,0,vt)||!decoder_.at(vt,observed.multi_?0x38:0x30,draw)||!decoder_.methodKnown(observed.multi_,vt,draw))return false;
  // Exact-type check must precede the derived extent read, including failures.
  if(!decoder_.at(observed.node_,0,vt)||vt!=decoder_.api_.instanceVtable)return false;
  std::array<std::byte,0xf8> node{};
  if(!decoder_.read(observed.node_,node.data(),node.size())||node!=observed.nodeBytes_)return false;
  return current()&&leased()&&observed.revision_==revision()&&observed.frame_==frame_&&
     recordRevision()==consumed;
 }
 bool sameHeader() {Ready now{};return decoder_.header(ready_.manager,now)&&now.request==ready_.request&&now.container==ready_.container&&now.records==ready_.records&&now.recordCount==ready_.recordCount&&now.buckets==ready_.buckets&&now.bucketCount==ready_.bucketCount&&now.instanceList==ready_.instanceList&&now.nativeFrame==ready_.nativeFrame;}
public:
 // A lexical optimization of repeated ready-header checks, issued ONLY by
 // Admission from a concrete current binding Read. It never grants a borrow.
 // No original/native callbacks may run before it ends. Provider and issuer
 // outlive this nonmovable scope; nested scopes invalidate outer reuse.
 class HeaderScope {
  friend class NativeCurrentRecordProvider;
  friend class NativeCurrentAdmission;
  NativeCurrentRecordProvider* provider_{};HeaderScope* previous_{};
  const void* issuer_{};std::uint64_t serial_{},revision_{};
  bool(*current_)(const void*,std::uint64_t)noexcept{};bool reusable_{};
  bool activate(NativeCurrentRecordProvider& p,const void* issuer,std::uint64_t serial,
                bool(*current)(const void*,std::uint64_t)noexcept)noexcept{
   if(provider_||!p.leased()||!p.revision()||!issuer||!serial||!current)return false;
   if(p.header_)p.header_->reusable_=false;
   const auto revision=p.revision();
   // Force one REAL header read even when an earlier scope existed.
   if(!current(issuer,serial)||!p.sameHeader()||p.revision()!=revision||!current(issuer,serial))return false;
   provider_=&p;previous_=p.header_;issuer_=issuer;serial_=serial;revision_=revision;current_=current;
   reusable_=previous_==nullptr;p.header_=this;return true;
  }
  bool valid(const NativeCurrentRecordProvider& p)const noexcept{
   return reusable_&&provider_==&p&&revision_&&revision_==p.revision()&&issuer_&&current_&&current_(issuer_,serial_);
  }
 public:
  HeaderScope()noexcept=default;
  ~HeaderScope(){if(provider_&&provider_->header_==this)provider_->header_=previous_;}
  HeaderScope(const HeaderScope&)=delete;HeaderScope&operator=(const HeaderScope&)=delete;
  void invalidate()noexcept{reusable_=false;}
 };
 struct GroupView {unsigned group{},lod{},count{};Address drawable{};std::array<std::uint16_t,128> indices{};};
 explicit NativeCurrentRecordProvider(Api api):decoder_(api){decoder_.out_=&accounting_;}
 NativeCurrentRecordProvider(const NativeCurrentRecordProvider&)=delete;
 void end(){active_=false;frame_=0;advance(revision_);}
 bool begin(Address mgr,unsigned index,Address returnOffset,bool primaryDepth,std::uint64_t frame){
  end();accounting_.reads=accounting_.bytes=0;accounting_.error=Error::None;
  if(index!=MainRequest||returnOffset!=MainDepthWaitReturn||!primaryDepth||!frame)return decline(Error::WrongBoundary);
  if(!decoder_.header(mgr,ready_))return decline(Error::NotReady);
  if(ready_.bucketCount>32)return decline(Error::Budget);
  common_.fill(0);instance_.fill(0);owner_=std::this_thread::get_id();frame_=frame;active_=true;
  if(!groups([&](const GroupView& g){for(unsigned i=0;i<g.count;++i){unsigned ix=g.indices[i];if(ix>=ready_.recordCount||bit(instance_,ix))return false;set(instance_,ix);}
  return true;}))return decline(Error::BadIndex);
  std::uint64_t rows=0;
  for(unsigned b=0;b<ready_.bucketCount;++b){Address h{},holder{},items{};std::uint16_t cap{},n{};if(!AdmissionDecoder::plus(ready_.buckets,b*0x70,h)||!decoder_.at(h,0,cap)||!decoder_.at(h,4,n))return decline(Error::ReadFailed);
   if(n>cap||(rows+=n)>262144)return decline(Error::Budget);if(!n)continue;
   if(!decoder_.at(h,0x60,holder)||!decoder_.at(holder,0,items))return decline(Error::ReadFailed);
   // Small bounded batches reduce callback/kernel overhead for guarded diagnostic use.
   std::array<std::byte,64*16> batch{};
   for(unsigned base=0;base<n;base+=64){unsigned count=(n-base)<64?(n-base):64;Address p{};if(!AdmissionDecoder::plus(items,base*16,p)||!decoder_.read(p,batch.data(),count*16))return decline(Error::ReadFailed);
    for(unsigned i=0;i<count;++i){auto ix=AdmissionDecoder::get<std::uint16_t>(batch,i*16);if(ix>=ready_.recordCount||bit(instance_,ix))return decline(Error::AmbiguousRecord);set(common_,ix);}
   }
  }
  if(!sameHeader())return decline(Error::HeaderChanged);return true;
 }
 const Ready* ready()const{return leased()?&ready_:nullptr;}
 std::uint64_t frame()const{return leased()?frame_:0;}
 // Monotonic lexical identities, never a substitute for the native Read lease.
 std::uint64_t revision()const noexcept{return leased()&&revision_!=UINT64_MAX?revision_:0;}
 std::uint64_t recordRevision()const noexcept{return leased()&&recordRevision_!=UINT64_MAX?recordRevision_:0;}
 Address diagnosticInstanceVtable()const noexcept{return decoder_.api_.instanceVtable;}
 std::uint64_t reads()const{return accounting_.reads;}
 std::uint64_t bytes()const{return accounting_.bytes;}
 Error error()const{return accounting_.error;}
 bool current(){if(!leased())return false;if(header_&&header_->valid(*this))return true;if(!sameHeader())return decline(Error::HeaderChanged);return true;}
 bool ordinary(unsigned index)const{return leased()&&index<ready_.recordCount&&bit(common_,index)&&!bit(instance_,index);}
 template<class Visitor> bool groups(Visitor&& visitor){
  if(!leased())return false;if(!ready_.instanceList)return true;
  std::uint32_t count{};if(!decoder_.at(ready_.instanceList,0xdc40,count)||count>48)return false;
  std::array<std::uint8_t,192> sizes{};if(count&&!decoder_.read(ready_.instanceList+0x1580,sizes.data(),count*4))return false;
  for(unsigned g=0;g<count;++g)for(unsigned lod=0;lod<4;++lod){GroupView v{};v.group=g;v.lod=lod;v.count=sizes[g*4+lod];if(v.count>128)return false;if(!v.count)continue;
   Address p{};if(!decoder_.at(ready_.instanceList,0xd640+g*32+lod*8,v.drawable)||!v.drawable||!AdmissionDecoder::plus(ready_.instanceList,0x1640+g*0x400+lod*0x100,p)||!decoder_.read(p,v.indices.data(),v.count*2))return false;
   for(unsigned j=0;j<v.count;++j)if(v.indices[j]>=ready_.recordCount)return false;
   if(!visitor(v))return false;
  }
  return true;
 }
 bool record(unsigned index,Record& r,RecordSnapshot* observed=nullptr){
  if(observed)observed->valid_=false;
  r={};if(!leased())return false;advance(recordRevision_);
  const auto observedRevision=observed?revision():0,observedRecordRevision=observed?recordRevision():0,observedFrame=observed?frame_:0;
  if(index>=ready_.recordCount)return false;r.index=static_cast<std::uint16_t>(index);
  if(!AdmissionDecoder::plus(ready_.records,index*RecordStride,r.address))return false;
  if(bit(instance_,index)){r.family=Family::NativeInstanceList;r.instanceReferences=1;return true;}
  if(!bit(common_,index))return true;
  std::array<std::byte,RecordStride> b{};if(!decoder_.read(r.address,b.data(),b.size()))return false;
  r.ordinaryFields=true;r.bucketReferences=1;r.world=AdmissionDecoder::matrix(b,0);r.previousWorld=AdmissionDecoder::matrix(b,0x40);r.worldFinite=AdmissionDecoder::finite(r.world);
  r.drawable=AdmissionDecoder::get<Address>(b,0x80);r.node=AdmissionDecoder::get<Address>(b,0x88);r.matrixSet=AdmissionDecoder::get<Address>(b,0x90);r.creature=AdmissionDecoder::get<Address>(b,0x98);r.flags=AdmissionDecoder::get<std::uint32_t>(b,0xac);r.lod=static_cast<std::uint16_t>((r.flags>>10)&7);r.ambientOcclusion=AdmissionDecoder::get<std::uint32_t>(b,0xa8);
  if(r.creature||(r.flags&0xe000)){r.family=Family::SpecialOrCreature;return true;}
  decoder_.models(r);decoder_.bounds(r,observed?&observed->nodeBytes_:nullptr);
  if(!r.drawMethodKnown||!r.worldFinite)return true;
  if(!r.matrixSet){r.family=Family::StaticCommon;r.ordinaryStateOnlyCandidate=r.nodeBoundsCurrent&&!r.nativeNodeExclusion&&!(r.flags&0x10000)&&r.world==r.previousWorld&&r.modelFamily==ModelFamily::Rigid;}
  else if(r.modelFamily==ModelFamily::Rigid)r.family=Family::RigidMatrixSet;
  else if(r.modelFamily==ModelFamily::Skinned)r.family=Family::SkinnedMatrixSet;
  else if(r.modelFamily==ModelFamily::Mixed)r.family=Family::MixedMatrixSet;
  if(observed&&r.targetNodeCurrent()&&r.drawMethodKnown&&r.worldFinite&&
     (r.family==Family::StaticCommon||r.family==Family::RigidMatrixSet)&&
     observedRevision&&observedRecordRevision&&observedRevision==revision()&&
     observedRecordRevision==recordRevision()&&observedFrame==frame_){
   observed->provider_=this;observed->revision_=revision();observed->recordRevision_=recordRevision();observed->frame_=frame_;observed->index_=index;
   observed->address_=r.address;observed->node_=r.node;observed->drawable_=r.drawable;observed->multi_=r.matrixSet!=0;observed->raw_=b;observed->valid_=true;
  }
  return true;
 }
 template<class Visitor> bool visit(Visitor&& visitor){if(!leased())return false;for(unsigned i=0;i<ready_.recordCount;++i){Record r{};if(!record(i,r)||!visitor(r))return false;}return current();}
};
} // namespace rdr95
