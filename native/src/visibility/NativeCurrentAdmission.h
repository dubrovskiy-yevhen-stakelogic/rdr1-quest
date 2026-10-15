#pragma once
#include "NativeCurrentDrawBounds.h"
#include "NativeProductionOffsets.h"
#include "NativeCurrentFixedState.h"
#include "NativeReadQualification.h"
#include <memory>
#include <optional>
#include <bit>
namespace rdrvr::experimental {class NativeEarlyRuntimeBinding;class NativeEarlyFrameController;}
namespace rdr95 {
// PRIVATE seam. No renderer calls, no visibility decisions, no installed hook.
// A Permit identifies freshly prepared complete geometry only. Caller MUST run
// recordCurrent + fixedState + its query/camera/target checks before a skip.
class NativeCurrentAdmission {
 friend class rdrvr::experimental::NativeEarlyFrameController;
public:
 using Bounds=NativeCurrentDrawBounds::Bounds;
 static constexpr unsigned MaxObjects=1024,MaxRecords=65536,KeyBuckets=4096,MaxKeyChain=32;
 struct Frame {std::uint64_t scene{},frame{},pose{},resourceGeneration{};bool operator==(const Frame&)const=default;};
 class Permit {friend class NativeCurrentAdmission;const NativeCurrentAdmission*owner_{};std::uint64_t generation_{};unsigned slot_{},index_{};public:explicit operator bool()const noexcept{return owner_&&generation_;}};
 struct Candidate {unsigned index{};Family family{Family::Unknown};Address node{},drawable{};Bounds::Bounds bounds{};std::array<double,3> axisError{};double worldError{};Bounds::ProofKind proofKind{Bounds::ProofKind::None};bool sourceNodeCurrent{};Permit permit{};};
 struct SourceMetadata {unsigned index{};Address node{},drawable{},geometry{};const NativeSelectedLodGeometry::Cache::Snapshot* mesh{};std::array<float,12> world{};};
 struct Stats {std::array<unsigned,unsigned(Family::Count)> families{};
 std::array<unsigned,8> callbackDeclines{},callbackGlobalMasks{};unsigned modelCallbackDataChecks{};std::array<unsigned,7> modelCallbackResults{};
 std::array<unsigned,6> cacheDeclines{};
 struct VtableSample{Address rva{};unsigned count{};};
 std::array<VtableSample,4> rigidVtableSamples{};
 unsigned rigidVtableObserved{},rigidVtableUnnormalized{},rigidVtableOverflow{};
 std::array<unsigned,18> nodeBoundsDeclines{};std::array<unsigned,3> preDeclines{};std::array<unsigned,11> prepareStages{},geometryFailures{};
 unsigned ordinaryRecords{},unknownDrawMethods{},nonfiniteWorlds{},coldAttempts{},warmAttempts{};unsigned records{},candidates{},cold{},warm{},unsupported{},budget{},duplicates{},revoked{},stateDeclined{},fullValidations{},reusedValidations{},reclaimed{},indexBudget{},preparationReuses{},recordHandoffs{},fingerprintValidations{},finalRecordReuses{};std::uint64_t reads{},bytes{},keyProbes{},allocationProbes{};};
 // Only the concrete binding can issue this token for an actual current Read
 // instance. A pointer/generation or caller-provided safe boolean cannot make
 // one. The issuing binding must outlive all of its short lexical tickets.
 class PreparationTicket {
  friend class NativeCurrentAdmission;
  friend class rdrvr::experimental::NativeEarlyRuntimeBinding;
  const void* owner_{};const void* read_{};void* geometryContext_{};
  bool(*current_)(const void*,const void*,std::uint64_t)noexcept{};
  std::uint64_t serial_{},frame_{},resources_{};Ready ready_{};
  NativeSelectedLodGeometry::Api geometry_{};
  NativeGeometryStorageTicket storage_{};
   NativeReadQualification drawQualification_{};
 public:
  explicit operator bool()const noexcept{return owner_&&read_&&serial_&&current_&&current_(owner_,read_,serial_);}
 };
 // MAIN-only immediate preparation reuse. Create BEFORE begin, consume only
 // read-only mask preparation, invalidate BEFORE native code/Read end. No
 // ticket created after begin can retroactively certify that preparation.
 class PreparationScope {
  friend class NativeCurrentAdmission;
  NativeCurrentAdmission*owner_{};PreparationScope*previous_{};PreparationTicket ticket_{};
  std::uint64_t before_{},sealed_{},boundGeneration_{},providerRevision_{};bool reusable_{},began_{},bound_{};
 public:
  PreparationScope(NativeCurrentAdmission& owner,PreparationTicket ticket)noexcept:
   owner_(&owner),previous_(activePreparation_),ticket_(ticket),before_(owner.generation_){
   reusable_=!previous_&&!activeDraw_&&bool(ticket_);
   if(previous_)previous_->invalidate();if(activeDraw_)activeDraw_->invalidate();activePreparation_=this;
  }
  ~PreparationScope(){invalidate();activePreparation_=previous_;}
  PreparationScope(const PreparationScope&)=delete;PreparationScope&operator=(const PreparationScope&)=delete;
  void invalidate()noexcept{reusable_=bound_=false;sealed_=0;if(owner_)owner_->activeRecordReceipt_=0;}
 };
 // A synchronous adapter scope, NOT a native lifetime lock. All memory readers
 // and certificate callbacks in this scope must be read-only under the current
 // mutation-stable native borrow. End/invalidate it BEFORE calling original or
 // any callback that may change native state. Revoke tickets remain checked on
 // every reuse; nested scopes permanently disable outer reuse.
 class DrawScope {
  friend class NativeCurrentAdmission;
  NativeCurrentAdmission* owner_{};DrawScope* previous_{};
  NativeCurrentFixedState::Call call_{};std::uint64_t generation_{};
  PreparationTicket ticket_{};NativeCurrentRecordProvider::HeaderScope header_{};
  std::uint64_t headerSerial_{},providerRevision_{};
  unsigned index_{};bool reusable_{},checked_{},accepted_{};
 public:
  DrawScope(NativeCurrentAdmission& owner,const NativeCurrentFixedState::Call& call,PreparationTicket ticket)noexcept:owner_(&owner),previous_(activeDraw_),call_(call),generation_(owner.generation_),ticket_(ticket){
   if(activePreparation_)activePreparation_->invalidate();
   reusable_=!previous_&&owner.provider_&&owner.thread_==std::this_thread::get_id();
   if(previous_)previous_->invalidate();activeDraw_=this;
   if(reusable_&&owner.ticketArguments(ticket_)&&owner.nextHeaderSerial_!=UINT64_MAX){
    headerSerial_=++owner.nextHeaderSerial_;providerRevision_=owner.provider_->revision();
    header_.activate(*owner.provider_,&owner,headerSerial_,drawHeaderCurrent);
   }
  }
  DrawScope(NativeCurrentAdmission& owner,const NativeCurrentFixedState::Call& call)noexcept:DrawScope(owner,call,PreparationTicket{}){}
  ~DrawScope(){invalidate();activeDraw_=previous_;}
  DrawScope(const DrawScope&)=delete;DrawScope&operator=(const DrawScope&)=delete;
  void invalidate()noexcept{reusable_=false;checked_=false;accepted_=false;headerSerial_=0;header_.invalidate();if(owner_)owner_->activeRecordReceipt_=0;}
 };
 bool begin(NativeCurrentRecordProvider&,const NativeSelectedLodGeometry::Api&,Frame);
 template<class Visitor>bool candidates(Visitor&& visitor){if(!frameCurrent())return false;for(unsigned i=0;i<recordCount_;++i){Candidate c{};if(candidate(i,c)&&!visitor(c))return false;}return frameCurrent();}
 bool candidate(unsigned,Candidate&)const noexcept;
 template<class Visitor>bool sourceMetadata(Visitor&& visitor){if(!frameCurrent())return false;for(unsigned i=0;i<recordCount_;++i){Candidate c;if(!candidate(i,c)||c.family!=Family::StaticCommon||!c.sourceNodeCurrent)continue;auto*s=lookup(i,c.permit);if(!s||!s->prepared.current())continue;const auto&g=s->prepared.graph();for(const auto&part:g.parts){SourceMetadata x{i,c.node,c.drawable,part.geometry,&part.mesh,g.transforms.models[part.modelOrdinal].modelToWorld};if(!visitor(x))return false;}}return frameCurrent();}
 unsigned recordCount()const noexcept{return recordCount_;}
 bool recordCurrent(unsigned,const Permit&);
 bool fixedState(const NativeCurrentFixedState::Api&,const NativeCurrentFixedState::Call&,const Permit&,NativeCurrentFixedState::Result&);
 bool frameCurrent()const;
 void end()noexcept{if(activePreparation_&&activePreparation_->owner_==this)activePreparation_->invalidate();activeRecordReceipt_=0;provider_=nullptr;recordCount_=0;}
 const Stats& statistics()const noexcept{return stats_;}
 const Frame& frame()const noexcept{return frame_;}
private:
 // Only Controller may use this within its exact object's accepted DrawScope.
 // This is liveness of a checked same-Read path, never final semantic commit.
 bool qualifiedDrawCurrent(const DrawScope& expected,const NativeCurrentFixedState::Call& call)const noexcept;
 inline static thread_local DrawScope* activeDraw_{};
 inline static thread_local PreparationScope* activePreparation_{};
 static bool sameCall(const NativeCurrentFixedState::Call&a,const NativeCurrentFixedState::Call&b)noexcept{return a.gfx==b.gfx&&a.vk==b.vk&&a.container==b.container&&a.record==b.record&&a.command==b.command&&a.bucket==b.bucket&&std::bit_cast<std::uint32_t>(a.fade)==std::bit_cast<std::uint32_t>(b.fade)&&a.flags==b.flags&&a.extra==b.extra;}
 struct Slot {Address node{},drawable{};unsigned lod{},bucket{},next{MaxObjects},previous{MaxObjects};std::uint64_t touched{};NativeCurrentDrawBounds prepared;Candidate candidate;bool valid{};};
 struct Reference {std::uint64_t generation{};std::uint16_t slot{};};
 std::array<std::unique_ptr<Slot>,MaxObjects> slots_{};
 // Intrusive bounded hash chains: reclaim unlinks its exact old key in O(1),
 // so there are no stale/tombstone entries and no full-table miss scan.
 std::array<unsigned,KeyBuckets> keyHeads_=[](){std::array<unsigned,KeyBuckets> x{};x.fill(MaxObjects);return x;}();
 std::array<Reference,MaxRecords> records_{};
 NativeCurrentRecordProvider*provider_{};NativeSelectedLodGeometry::Api api_{};Frame frame_{};Stats stats_{};
 std::thread::id thread_{};std::uint64_t generation_{},nextRecordReceipt_{},activeRecordReceipt_{},nextHeaderSerial_{};unsigned recordCount_{},allocated_{},reclaimCursor_{};bool allocationExhausted_{};
 static std::uint64_t mixKey(std::uint64_t x){x^=x>>30;x*=0xbf58476d1ce4e5b9ull;x^=x>>27;x*=0x94d049bb133111ebull;return x^(x>>31);}
 static unsigned keyBucket(Address node,Address drawable,unsigned lod){return unsigned((mixKey(node)^std::rotl(mixKey(drawable),21)^mixKey(lod))&(KeyBuckets-1));}
 unsigned findSlot(const Record&,unsigned bucket,unsigned& chain);
 unsigned allocateSlot();
 void unlinkSlot(unsigned)noexcept;
 bool preparationCurrent(const PreparationScope&)const noexcept;
 bool preparationArguments(const PreparationScope&)const noexcept;
 bool ticketArguments(const PreparationTicket&)const noexcept;
 static bool preparationHeaderCurrent(const void*,std::uint64_t)noexcept;
 static bool drawHeaderCurrent(const void*,std::uint64_t)noexcept;
 static bool sameGeometryApi(const NativeSelectedLodGeometry::Api&a,const NativeSelectedLodGeometry::Api&b)noexcept{
  return a.geometryVtable==b.geometryVtable&&a.vertexVtable==b.vertexVtable&&a.indexVtable==b.indexVtable&&a.bufferVtable==b.bufferVtable&&
   a.shadowBufferSlot==b.shadowBufferSlot&&a.forceShaderSlot==b.forceShaderSlot&&a.shaderSorterSlot==b.shaderSorterSlot&&a.commandBufferSlot==b.commandBufferSlot&&
   a.cache==b.cache&&a.cacheEpoch==b.cacheEpoch&&a.materials==b.materials&&a.context==b.context&&a.material==b.material&&
   a.ownerCurrent==b.ownerCurrent&&a.technique==b.technique&&a.matrixSetCurrent==b.matrixSetCurrent&&a.modelCullbackSlot==b.modelCullbackSlot&&a.pureModelCullback==b.pureModelCullback;
 }
 NativeReadRecord sealRecord(unsigned,const Record&)noexcept;
 NativeReadRecord sealDrawRecord(unsigned,const Record&,const NativeCurrentRecordProvider::RecordSnapshot* =nullptr)noexcept;
 static bool recordReceiptCurrent(const void*,std::uint64_t)noexcept;
 Slot* lookup(unsigned,const Permit&)const noexcept;
 static bool sameTransforms(const NativeCurrentRigidMatrices::Output&a,const NativeCurrentRigidMatrices::Output&b){if(a.count!=b.count||a.drawable!=b.drawable||a.matrixSet!=b.matrixSet)return false;for(unsigned i=0;i<a.count;++i){const auto&x=a.models[i],&y=b.models[i];if(x.native!=y.native||x.ordinal!=y.ordinal||x.paletteIndex!=y.paletteIndex||x.modelToWorld!=y.modelToWorld)return false;}return true;}
};
inline unsigned NativeCurrentAdmission::findSlot(const Record&r,unsigned bucket,unsigned& chain){
 chain=0;for(unsigned at=keyHeads_[bucket];at!=MaxObjects;at=slots_[at]->next){
  ++stats_.keyProbes;++chain;const auto&s=*slots_[at];if(s.node==r.node&&s.drawable==r.drawable&&s.lod==r.lod)return at;
  if(chain==MaxKeyChain)break;
 }return MaxObjects;
}
inline void NativeCurrentAdmission::unlinkSlot(unsigned index)noexcept{
 auto&s=*slots_[index];
 if(s.previous==MaxObjects)keyHeads_[s.bucket]=s.next;else slots_[s.previous]->next=s.next;
 if(s.next!=MaxObjects)slots_[s.next]->previous=s.previous;
 s.next=s.previous=MaxObjects;
}
inline unsigned NativeCurrentAdmission::allocateSlot(){
 if(allocated_<MaxObjects){++stats_.allocationProbes;const auto slot=allocated_;slots_[slot]=std::make_unique<Slot>();++allocated_;return slot;}
 if(allocationExhausted_)return MaxObjects;
 for(unsigned n=0;n<MaxObjects;++n){const unsigned slot=reclaimCursor_;reclaimCursor_=(reclaimCursor_+1)%MaxObjects;++stats_.allocationProbes;
  if(slots_[slot]->touched!=generation_){unlinkSlot(slot);++stats_.reclaimed;return slot;}
 }
 allocationExhausted_=true;return MaxObjects;
}
inline bool NativeCurrentAdmission::begin(NativeCurrentRecordProvider&p,const NativeSelectedLodGeometry::Api&a,Frame f){
 // Reset the previous frame's state without destroying the newly entered preparation
 // scope. An externally called end() does invalidate that scope.
 provider_=nullptr;recordCount_=0;activeRecordReceipt_=0;stats_={};auto* preparation=activePreparation_;
 const bool prepare=preparation&&preparation->owner_==this&&preparation->reusable_&&!preparation->began_&&preparation->before_==generation_;
 if(preparation&&preparation->owner_==this){preparation->sealed_=0;preparation->began_=true;if(!prepare)preparation->invalidate();}
 if(!f.scene||!f.frame||!f.pose||!f.resourceGeneration||f.frame!=p.frame()||!p.ready()||!a.ownerCurrent||!a.ownerCurrent(a.context,f.frame)||generation_==UINT64_MAX)return false;
 if(frame_.resourceGeneration&&frame_.resourceGeneration!=f.resourceGeneration){for(auto&s:slots_)s.reset();allocated_=reclaimCursor_=0;keyHeads_.fill(MaxObjects);}
 provider_=&p;api_=a;frame_=f;thread_=std::this_thread::get_id();++generation_;recordCount_=p.ready()->recordCount;
 if(prepare&&preparationArguments(*preparation)){preparation->bound_=true;preparation->boundGeneration_=generation_;preparation->providerRevision_=p.revision();}
 NativeCurrentRecordProvider::HeaderScope header;
 if(prepare&&preparationCurrent(*preparation)&&!header.activate(p,this,generation_,preparationHeaderCurrent)){end();return false;}
 allocationExhausted_=false;const auto reads=p.reads(),bytes=p.bytes();
 for(unsigned i=0;i<recordCount_;++i){Record r{};if(!p.record(i,r)){end();return false;}++stats_.records;++stats_.families[unsigned(r.family)];
  stats_.ordinaryRecords+=r.ordinaryFields;stats_.unknownDrawMethods+=r.ordinaryFields&&!r.drawMethodKnown;
  stats_.nonfiniteWorlds+=r.ordinaryFields&&!r.worldFinite;
  if((r.family!=Family::StaticCommon&&r.family!=Family::RigidMatrixSet)||!r.targetNodeCurrent()||r.nativeNodeExclusion){
   const unsigned reason=(r.family!=Family::StaticCommon&&r.family!=Family::RigidMatrixSet)?0:!r.targetNodeCurrent()?1:2;
   if(reason==1){const unsigned family=r.family==Family::StaticCommon?0:1;const auto decline=unsigned(r.nodeBoundsDecline);if(decline<unsigned(NodeBoundsDecline::Count))++stats_.nodeBoundsDeclines[family*9+decline];
    if(family==1&&r.nodeBoundsDecline==NodeBoundsDecline::VtableMismatch){
     ++stats_.rigidVtableObserved;
     // Numeric diagnostic normalization against the pinned factory anchor.
     // It neither reads the refused vtable nor certifies its class/layout.
     static_assert(retail::instanceVtable==0x2dd3a28);
     const Address anchor=p.diagnosticInstanceVtable();
     const Address base=anchor>=retail::instanceVtable?anchor-retail::instanceVtable:0;
     const Address rva=base&&r.refusedNodeVtable>base?r.refusedNodeVtable-base:0;
     if(!rva||rva>=0x50000000ull)++stats_.rigidVtableUnnormalized;
     else{bool counted=false;for(auto& sample:stats_.rigidVtableSamples){
      if(sample.rva==rva||!sample.rva){sample.rva=rva;++sample.count;counted=true;break;}}
      if(!counted)++stats_.rigidVtableOverflow;
     }
    }}++stats_.preDeclines[reason];++stats_.unsupported;continue;}
  const auto bucket=keyBucket(r.node,r.drawable,r.lod);unsigned chain{};unsigned slot=findSlot(r,bucket,chain);
  if(slot==MaxObjects){
   if(chain==MaxKeyChain){++stats_.indexBudget;++stats_.budget;continue;}
   slot=allocateSlot();if(slot!=MaxObjects){auto&s=*slots_[slot];s.bucket=bucket;s.previous=MaxObjects;s.next=keyHeads_[bucket];
    if(s.next!=MaxObjects)slots_[s.next]->previous=slot;keyHeads_[bucket]=slot;}
  }
  if(slot==MaxObjects){++stats_.budget;continue;}
  auto&s=*slots_[slot];if(s.touched==generation_){++stats_.duplicates;continue;}s.valid=false;s.node=r.node;s.drawable=r.drawable;s.lod=r.lod;s.touched=generation_;
  const auto cold=s.prepared.coldBuilds(),warm=s.prepared.warmRefreshes();Bounds::Result b{};
  auto handoff=sealRecord(i,r);
  const auto* handoffPointer=activeRecordReceipt_?&handoff:nullptr;
  const bool prepared=s.prepared.prepare(p,api_,i,{f.scene,f.frame,r.node,f.pose},b,true,handoffPointer);
  activeRecordReceipt_=0;
  stats_.coldAttempts+=s.prepared.coldBuilds()-cold;stats_.warmAttempts+=s.prepared.warmRefreshes()-warm;
  if(s.prepared.coldBuilds()!=cold&&unsigned(s.prepared.diagnosticStage())>=unsigned(NativeCurrentDrawBounds::DiagnosticStage::ColdCapture)){const auto& graph=s.prepared.graph();stats_.modelCallbackDataChecks+=graph.modelCallbackDataChecks;for(unsigned reason=0;reason<7;++reason)stats_.modelCallbackResults[reason]+=graph.modelCallbackResults[reason];}
  if(!prepared){
   const auto stage=s.prepared.diagnosticStage();++stats_.prepareStages[unsigned(stage)];
   if(stage==NativeCurrentDrawBounds::DiagnosticStage::ColdCapture){
    const auto& graph=s.prepared.graph();++stats_.geometryFailures[unsigned(graph.failure)];
    if(graph.failure==NativeSelectedLodGeometry::Failure::CacheMiss)++stats_.cacheDeclines[unsigned(graph.cacheDecline)];
    if(graph.failure==NativeSelectedLodGeometry::Failure::Callback){
     const auto reason=graph.callbackDecline;++stats_.callbackDeclines[unsigned(reason)];
     if(reason==NativeSelectedLodGeometry::CallbackDecline::ForceActive||reason==NativeSelectedLodGeometry::CallbackDecline::SorterActive||reason==NativeSelectedLodGeometry::CallbackDecline::CommandActive)++stats_.callbackGlobalMasks[graph.callbackGlobalMask];
    }
   }
   ++stats_.unsupported;continue;}
  stats_.cold+=s.prepared.coldBuilds()-cold;stats_.warm+=s.prepared.warmRefreshes()-warm;
  Candidate c{};c.index=i;c.family=r.family;c.node=r.node;c.drawable=r.drawable;c.bounds=b.bounds;c.worldError=b.worldError;c.axisError=b.axisError;c.proofKind=b.proofKind;c.sourceNodeCurrent=r.targetNodeCurrent();
  c.permit.owner_=this;c.permit.generation_=generation_;c.permit.slot_=slot;c.permit.index_=i;s.candidate=c;s.valid=true;records_[i]={generation_,static_cast<std::uint16_t>(slot)};++stats_.candidates;
 }
 stats_.reads=p.reads()-reads;stats_.bytes=p.bytes()-bytes;const bool valid=frameCurrent();
 if(prepare&&valid&&preparationCurrent(*preparation))preparation->sealed_=generation_;
 return valid;
}
inline bool NativeCurrentAdmission::preparationArguments(const PreparationScope&s)const noexcept{
 return activePreparation_==&s&&s.owner_==this&&s.reusable_&&ticketArguments(s.ticket_);
}
inline bool NativeCurrentAdmission::qualifiedDrawCurrent(const DrawScope& expected,const NativeCurrentFixedState::Call& call)const noexcept{
 if(thread_!=std::this_thread::get_id()||activeDraw_!=&expected||expected.owner_!=this||
    !expected.reusable_||!expected.checked_||!expected.accepted_||!expected.headerSerial_||
    expected.generation_!=generation_||!sameCall(expected.call_,call)||!provider_||!provider_->leased()||
    !expected.providerRevision_||provider_->revision()!=expected.providerRevision_||
    provider_->header_!=&expected.header_||!expected.ticket_.drawQualification_.issued())return false;
 // valid() retains the exact active header serial and binding/read qualification.
 // Failed activation has no published active header; nested headers invalidate it.
 return expected.header_.valid(*provider_);
}
inline bool NativeCurrentAdmission::ticketArguments(const PreparationTicket&t)const noexcept{
 const auto*r=provider_?provider_->ready():nullptr;
 return bool(t)&&r&&
  t.geometryContext_==api_.context&&sameGeometryApi(t.geometry_,api_)&&t.frame_==frame_.frame&&t.resources_==frame_.resourceGeneration&&
  r->manager==t.ready_.manager&&r->request==t.ready_.request&&r->container==t.ready_.container&&
  r->records==t.ready_.records&&r->buckets==t.ready_.buckets&&r->instanceList==t.ready_.instanceList&&
  r->recordCount==t.ready_.recordCount&&r->bucketCount==t.ready_.bucketCount&&r->nativeFrame==t.ready_.nativeFrame;
}
inline bool NativeCurrentAdmission::preparationHeaderCurrent(const void*p,std::uint64_t serial)noexcept{
 const auto&s=*static_cast<const NativeCurrentAdmission*>(p);const auto*scope=activePreparation_;
 return serial&&s.generation_==serial&&scope&&s.preparationCurrent(*scope);
}
inline bool NativeCurrentAdmission::drawHeaderCurrent(const void*p,std::uint64_t serial)noexcept{
 const auto&s=*static_cast<const NativeCurrentAdmission*>(p);const auto*scope=activeDraw_;
 return scope&&scope->owner_==&s&&scope->reusable_&&serial&&scope->headerSerial_==serial&&
  scope->generation_==s.generation_&&s.provider_&&scope->providerRevision_&&
  s.provider_->revision()==scope->providerRevision_&&
   (scope->ticket_.drawQualification_.issued()?scope->ticket_.drawQualification_.live():bool(scope->ticket_));
}
inline bool NativeCurrentAdmission::preparationCurrent(const PreparationScope&s)const noexcept{
 // The complete API/list arguments were sealed once, before the first record.
 // The real issuer already checks resource/effect/shader/mesh/list revocation;
 // the immutable joined header must not be reread per candidate here.
 return activePreparation_==&s&&s.owner_==this&&s.reusable_&&s.bound_&&s.boundGeneration_==generation_&&
  provider_&&s.providerRevision_&&provider_->revision()==s.providerRevision_&&bool(s.ticket_);
}
inline NativeReadRecord NativeCurrentAdmission::sealRecord(unsigned index,const Record&r)noexcept{
 NativeReadRecord out;activeRecordReceipt_=0;const auto*s=activePreparation_;
 if(!s||!preparationCurrent(*s)||nextRecordReceipt_==UINT64_MAX||!provider_->recordRevision())return out;
 activeRecordReceipt_=++nextRecordReceipt_;out.provider_=provider_;out.issuer_=this;out.geometryContext_=api_.context;
 out.current_=recordReceiptCurrent;out.providerRevision_=provider_->revision();out.recordRevision_=provider_->recordRevision();
 out.frame_=frame_.frame;out.receiptSerial_=activeRecordReceipt_;out.index_=index;out.record_=r;++stats_.recordHandoffs;return out;
}
inline NativeReadRecord NativeCurrentAdmission::sealDrawRecord(unsigned index,const Record&r,const NativeCurrentRecordProvider::RecordSnapshot*snapshot)noexcept{
 NativeReadRecord out;activeRecordReceipt_=0;const auto*scope=activeDraw_;
 // The constructor captured a real Binding Read. A default/null scope takes
 // the full path; an active DrawScope alone is never ownership proof.
 if(!scope||scope->owner_!=this||!scope->headerSerial_||
    !drawHeaderCurrent(this,scope->headerSerial_)||!ticketArguments(scope->ticket_)||
    !provider_||!provider_->recordRevision()||nextRecordReceipt_==UINT64_MAX)return out;
 activeRecordReceipt_=++nextRecordReceipt_;out.provider_=provider_;out.issuer_=this;out.geometryContext_=api_.context;
 out.current_=recordReceiptCurrent;out.providerRevision_=provider_->revision();out.recordRevision_=provider_->recordRevision();
 out.frame_=frame_.frame;out.receiptSerial_=activeRecordReceipt_;out.index_=index;out.record_=r;out.snapshot_=snapshot;++stats_.recordHandoffs;return out;
}
inline bool NativeCurrentAdmission::recordReceiptCurrent(const void*p,std::uint64_t serial)noexcept{
 const auto&s=*static_cast<const NativeCurrentAdmission*>(p);const auto*scope=activePreparation_;
 if(!serial||s.activeRecordReceipt_!=serial)return false;
 if(scope&&s.preparationCurrent(*scope))return true;
 const auto*draw=activeDraw_;
 return draw&&draw->owner_==&s&&draw->headerSerial_&&drawHeaderCurrent(&s,draw->headerSerial_);
}
inline bool NativeCurrentAdmission::frameCurrent()const{return provider_&&thread_==std::this_thread::get_id()&&provider_->frame()==frame_.frame&&api_.ownerCurrent&&api_.ownerCurrent(api_.context,frame_.frame)&&provider_->current();}
inline NativeCurrentAdmission::Slot* NativeCurrentAdmission::lookup(unsigned index,const Permit&t)const noexcept{
 if(!provider_||thread_!=std::this_thread::get_id()||t.owner_!=this||t.generation_!=generation_||t.index_!=index||index>=recordCount_||t.slot_>=MaxObjects)return nullptr;const auto&r=records_[index];if(r.generation!=generation_||r.slot!=t.slot_)return nullptr;auto*s=slots_[t.slot_].get();return s&&s->valid&&s->touched==generation_?s:nullptr;
}
inline bool NativeCurrentAdmission::candidate(unsigned index,Candidate&out)const noexcept{out={};if(index>=recordCount_)return false;const auto&r=records_[index];if(r.generation!=generation_)return false;auto*s=slots_[r.slot].get();if(!s||!s->valid||s->candidate.index!=index||s->touched!=generation_)return false;out=s->candidate;return true;}
inline bool NativeCurrentAdmission::recordCurrent(unsigned index,const Permit&t){
 auto*s=lookup(index,t);auto*scope=activeDraw_;
 auto* preparation=activePreparation_;
 if(s&&!scope&&preparation&&preparation->sealed_==generation_&&preparationCurrent(*preparation)&&s->prepared.current()){
  ++stats_.preparationReuses;return true;
 }
 const bool scoped=s&&scope&&scope->owner_==this&&scope->generation_==generation_&&scope->reusable_&&scope->call_.record==s->prepared.graph().record&&provider_->ready()&&scope->call_.container==provider_->ready()->container;
 if(scoped&&scope->checked_&&scope->index_==index){
  ++stats_.reusedValidations;
  if(scope->accepted_&&frameCurrent()&&s->prepared.current())return true;
  scope->accepted_=false;++stats_.revoked;return false;
 }
 ++stats_.fullValidations;
 auto validated=[&](){
  if(!s||!frameCurrent()){++stats_.revoked;return false;}
  NativeReadRecord handoff;
  const bool canHandoff=scoped&&scope->headerSerial_&&drawHeaderCurrent(this,scope->headerSerial_)&&ticketArguments(scope->ticket_);
  const auto*storage=scoped&&scope->ticket_.storage_?&scope->ticket_.storage_:nullptr;
  // Only the storage path constructs the observation. It remains alive until
  // the forwarded Record has been consumed and activeRecordReceipt_ cleared.
  std::optional<NativeCurrentRecordProvider::RecordSnapshot> snapshot;
  struct ExpireReceipt {std::uint64_t& serial;~ExpireReceipt(){serial=0;}} expire{activeRecordReceipt_};
  if(canHandoff){Record initial{};if(storage)snapshot.emplace();
   if(!provider_->record(index,initial,snapshot?&*snapshot:nullptr)){++stats_.revoked;return false;}
   handoff=sealDrawRecord(index,initial,snapshot?&*snapshot:nullptr);}
  const auto* input=activeRecordReceipt_?&handoff:nullptr;
  if(storage){--stats_.fullValidations;++stats_.fingerprintValidations;}
  NativeReadRecord verified;const bool expectVerified=storage&&input;
  const bool valid=s->prepared.validateCurrentRecord(index,input,storage,expectVerified?&verified:nullptr);
  Record r{};const bool reused=valid&&expectVerified&&verified.copy(*provider_,index,api_.context,r);
  activeRecordReceipt_=0;
  if(!valid||(expectVerified&&!reused)){++stats_.revoked;return false;}
  if(reused)++stats_.finalRecordReuses;
  if((!reused&&!provider_->record(index,r))||r.node!=s->node||r.drawable!=s->drawable||r.lod!=s->lod||r.family!=s->candidate.family||r.creature||r.nativeNodeExclusion||!r.targetNodeCurrent()){s->valid=false;++stats_.revoked;return false;}
  return s->prepared.current();
 };
 const bool result=validated();if(scoped){scope->index_=index;scope->checked_=true;scope->accepted_=result;}return result;
}
inline bool NativeCurrentAdmission::fixedState(const NativeCurrentFixedState::Api&a,const NativeCurrentFixedState::Call&call,const Permit&t,NativeCurrentFixedState::Result&out){
 out={};auto*scope=activeDraw_;if(scope&&scope->owner_==this&&!sameCall(scope->call_,call)){out.failure=NativeCurrentFixedState::Failure::Identity;++stats_.stateDeclined;return false;}
 auto*s=lookup(t.index_,t);if(!s||!recordCurrent(t.index_,t)||!NativeCurrentFixedState::capture(a,*provider_,api_,s->prepared.graph(),call,out)){++stats_.stateDeclined;return false;}
 // Copied context bytes are reusable only inside this exact actual DrawScope
 // and physical Read. Any other (or null) ticket keeps Raster's fresh-read fallback.
 if(scope&&scope->owner_==this&&scope->headerSerial_&&sameCall(scope->call_,call)&&
    drawHeaderCurrent(this,scope->headerSerial_)&&ticketArguments(scope->ticket_))
  out.snapshot.seal(this,scope->headerSerial_,drawHeaderCurrent,call);
 return true;
}
}
