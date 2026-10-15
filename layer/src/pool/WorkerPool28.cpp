#include "WorkerPool28.h"
#include <exception>
#include <utility>
namespace rdr2::worker28 {
thread_local WorkerPool28* WorkerPool28::consuming_{};
namespace {std::atomic<std::uint64_t> nextThreadIdentity{1};}
std::uint64_t WorkerPool28::threadIdentity()noexcept{
 static thread_local const auto identity=[]()noexcept{auto old=nextThreadIdentity.load(std::memory_order_relaxed);for(;;){if(!old||old==UINT64_MAX)return std::uint64_t{};if(nextThreadIdentity.compare_exchange_weak(old,old+1,std::memory_order_relaxed))return old;}}();return identity;
}
Dispatch15::operator bool()const noexcept{return pipeline&&descriptors&&vertices&&index&&draw&&indexed&&barrier&&beginPass&&endPass&&viewport&&scissor&&stencil&&copyImage&&resetQueryPool&&writeTimestamp;}
WorkerPool28::WorkerPool28(VkDevice d,std::uint64_t g):device_(d),deviceGeneration_(g),packets_(std::make_unique<std::array<Packet,Capacity>>()),thread_([this]{run();}){}
std::unique_ptr<WorkerPool28> WorkerPool28::create(VkDevice d,std::uint64_t g)noexcept{
 if(!d||!g||!std::atomic<std::uint64_t>::is_always_lock_free)return {};
 try{return std::unique_ptr<WorkerPool28>(new WorkerPool28(d,g));}catch(...){return {};}
}
WorkerPool28::~WorkerPool28(){shutdownAfterApplicationQuiescence();}
WorkerPool28::HostEntry::HostEntry(WorkerPool28&o,AccessKind k)noexcept:owner_(&o),kind_(k),counted_(o.enterHost()){}
WorkerPool28::HostEntry::HostEntry(WorkerPool28&o,const RecordingToken&t)noexcept:owner_(&o),kind_(AccessKind::Resource){counted_=o.countProducerPreparation(t,preparation_);}
WorkerPool28::HostEntry::~HostEntry(){if(owner_&&counted_)owner_->leaveHost();}
WorkerPool28::HostEntry::HostEntry(HostEntry&&o)noexcept:owner_(std::exchange(o.owner_,nullptr)),kind_(o.kind_),counted_(std::exchange(o.counted_,false)),preparation_(std::exchange(o.preparation_,false)){}
WorkerPool28::HostEntry& WorkerPool28::HostEntry::operator=(HostEntry&&o)noexcept{if(this!=&o){if(owner_&&counted_)owner_->leaveHost();owner_=std::exchange(o.owner_,nullptr);kind_=o.kind_;counted_=std::exchange(o.counted_,false);preparation_=std::exchange(o.preparation_,false);}return *this;}
WorkerPool28::RecordingToken WorkerPool28::HostEntry::tryBegin(const PoolCommandToken&t,const Dispatch13&a,VkResult r,AdmissionReceipt74* receipt)noexcept{if(receipt)*receipt={};return owner_?owner_->admit(*this,t,a,r,receipt):RecordingToken{};}
bool WorkerPool28::HostEntry::drainProducerPreparation(const RecordingToken&t)noexcept{
 return owner_&&counted_&&preparation_&&owner_->drainProducerPreparation(*this,t);
}

bool WorkerPool28::enterHost(bool countEntry)noexcept{
 if(consuming_==this)std::terminate(); // violated callback-free downstream contract; never self-wait/drop/forward
 if(countEntry)entries_.fetch_add(1,std::memory_order_relaxed);
 auto w=admission_.load(std::memory_order_acquire);bool counted{};
 for(;;){auto next=w;if(countOf(w)==(CountMask>>4)){revoked_.store(true,std::memory_order_release);}else{next+=CountUnit;counted=true;}
  if(phaseOf(w)==Phase::Open)next=(next&~PhaseMask)|std::uint64_t(Phase::Closing);
  if(admission_.compare_exchange_weak(w,next,std::memory_order_acq_rel,std::memory_order_acquire))break;counted=false;
 }
 if(phaseOf(w)==Phase::Open||phaseOf(w)==Phase::Closing)closeAndWait();return counted;
}
bool WorkerPool28::countProducerPreparation(const RecordingToken&t,bool&preserved)noexcept{
 if(consuming_==this)std::terminate(); // downstream callback-free contract remains mandatory
 preserved=false;entries_.fetch_add(1,std::memory_order_relaxed);
 const auto id=threadIdentity();
 if(t.owner_==this&&t.command_&&t.generation_&&id&&t.producer_==id&&
    producerId_.load(std::memory_order_relaxed)==id&&
    !revoked_.load(std::memory_order_acquire)&&!stopRequested_.load(std::memory_order_acquire)){
  const auto expected=(t.generation_<<GenerationShift)|std::uint64_t(Phase::Open);
  auto compare=expected;
  // Exactly OPEN/count0/no producer reservation. Existing consumer packets may
  // execute: their command resources are certified separately by the layer.
  if(admission_.compare_exchange_strong(compare,expected+CountUnit,std::memory_order_acq_rel,std::memory_order_acquire)){
   preserved=true;return true;
  }
 }
 // The failed attempt already contributed its monotonic host-entry count.
 // Do not double-count or reopen a closed generation.
 return enterHost(false);
}

bool WorkerPool28::drainProducerPreparation(HostEntry&e,const RecordingToken&t)noexcept{
 if(consuming_==this)std::terminate();
 const auto id=threadIdentity();
 if(e.owner_!=this||!e.counted_||!e.preparation_||t.owner_!=this||!t.command_||!t.generation_||
    !id||t.producer_!=id||producerId_.load(std::memory_order_relaxed)!=id)return false;
 const auto expected=(t.generation_<<GenerationShift)|std::uint64_t(Phase::Open)|CountUnit;
 std::unique_lock<std::mutex> lock(mutex_);
 if(admission_.load(std::memory_order_acquire)!=expected||revoked_.load(std::memory_order_acquire)||
    stopRequested_.load(std::memory_order_acquire))return false;
 // The counted OPEN guard prevents every reservation. The fixed producer's
 // committed tail is stable. Wait for completion AND worker idle, so no original
 // downstream call remains active when the producer's synchronous call starts.
 const auto tail=committed_.load(std::memory_order_acquire);
 raiseGoalLocked(tail);testPoint(6);
 progress_.wait(lock,[&]{return consumed_.load(std::memory_order_acquire)>=tail&&consumerIdle_;});
 // A foreign/nested entry can drain and close meanwhile. It is never reopened.
 return admission_.load(std::memory_order_acquire)==expected&&
        !revoked_.load(std::memory_order_acquire)&&!stopRequested_.load(std::memory_order_acquire);
}

void WorkerPool28::leaveHost()noexcept{admission_.fetch_sub(CountUnit,std::memory_order_release);}
bool WorkerPool28::raiseGoal89(std::uint64_t n)noexcept{
 auto old=goal_.load(std::memory_order_seq_cst);
 while(old<n){if(goal_.compare_exchange_weak(old,n,std::memory_order_seq_cst,std::memory_order_seq_cst)){
  publications_.fetch_add(1,std::memory_order_relaxed);return true;
 }}
 return false;
}
void WorkerPool28::raiseGoalLocked(std::uint64_t n)noexcept{
 raiseGoal89(n);ready_.notify_all();
}
void WorkerPool28::publish(std::uint64_t n)noexcept{
 if(!raiseGoal89(n))return;
 testPoint(9);
 // Active consumption needs only the monotonic mailbox update. The consumer
 // advertises parking before its SC mailbox recheck. A wake that observes the
 // advertisement takes the CV mutex, covering the final check-to-wait gap.
 // Only the consumer changes its parking flag. Clearing it from a delayed
 // publisher can erase a newer park after that publisher's work was consumed.
 if(consumerParking89_.load(std::memory_order_seq_cst)){
  std::lock_guard<std::mutex>lock(mutex_);ready_.notify_one();
 }
}
void WorkerPool28::closeAndWait()noexcept{
 std::unique_lock<std::mutex> lock(mutex_);progress_.notify_all();testPoint(4);
 progress_.wait(lock,[&]{return !(admission_.load(std::memory_order_acquire)&Active);});
 auto w=admission_.load(std::memory_order_acquire);if(phaseOf(w)!=Phase::Closing)return;
 const auto generation=generationOf(w);
 if(requestGeneration_!=generation){if(requestSequence_==UINT64_MAX)std::terminate();requestGeneration_=generation;++requestSequence_;requestTail_=committed_.load(std::memory_order_acquire);closes_.fetch_add(1,std::memory_order_relaxed);raiseGoalLocked(requestTail_);}
 const auto sequence=requestSequence_;progress_.wait(lock,[&]{return ackSequence_>=sequence;});
 w=admission_.load(std::memory_order_acquire);
 while(phaseOf(w)==Phase::Closing&&generationOf(w)==generation){const auto next=(w&~PhaseMask)|std::uint64_t(Phase::Synchronous);if(admission_.compare_exchange_weak(w,next,std::memory_order_acq_rel,std::memory_order_acquire))break;}
 progress_.notify_all();
}
WorkerPool28::RecordingToken WorkerPool28::admit(HostEntry&e,const PoolCommandToken&t,const Dispatch13&a,VkResult result,AdmissionReceipt74* receipt)noexcept{
 auto failed=[&](bool value,unsigned code)noexcept{if(value&&receipt)receipt->firstFailure=code;return value;};
 auto decline=[&](){declines_.fetch_add(1,std::memory_order_relaxed);return RecordingToken{};};
 if(failed(e.owner_!=this,1)||failed(!e.counted_,2)||failed(e.kind_!=AccessKind::Begin,3)||failed(result!=VK_SUCCESS,4)||failed(!a,5)||failed(revoked_.load(std::memory_order_acquire),6)||failed(stopRequested_.load(std::memory_order_acquire),7)||failed(t.device!=device_,8)||failed(t.deviceGeneration!=deviceGeneration_,9)||failed(!t.pool,10)||failed(!t.command,11)||failed(!t.poolGeneration,12)||failed(!t.commandGeneration,13)||failed(!t.recordingGeneration,14))return decline();
 const auto id=threadIdentity();if(failed(!id,15))return decline();std::lock_guard<std::mutex> lock(mutex_);
 auto expected=admission_.load(std::memory_order_acquire);if(receipt)receipt->rawAdmission=expected;const auto phase=phaseOf(expected);const auto generation=generationOf(expected);
 if(failed(revoked_.load(std::memory_order_acquire),16)||failed(stopRequested_.load(std::memory_order_acquire),17)||failed((phase!=Phase::Off&&phase!=Phase::Synchronous),18)||failed((expected&Active),19)||failed(countOf(expected)!=1,20)||failed(generation==MaxGeneration,21)||failed(!consumerIdle_,22)||failed(consumed_.load(std::memory_order_acquire)!=committed_.load(std::memory_order_acquire),23)||failed(ackSequence_!=requestSequence_,24))return decline();
 const auto owner=producerId_.load(std::memory_order_relaxed);if(failed(owner&&owner!=id,25))return decline();
 if(failed(selected_.command==t.command&&selected_.commandGeneration==t.commandGeneration&&selected_.recordingGeneration>=t.recordingGeneration,26))return decline();
 // All state below is visible only to fixed producer after successful release-CAS.
 const auto oldSelected=selected_;const auto oldDispatch=dispatch_;
 selected_=t;dispatch_=a;producerId_.store(id,std::memory_order_relaxed);
 const auto next=((generation+1)<<GenerationShift)|std::uint64_t(Phase::Open);
 if(failed(!admission_.compare_exchange_strong(expected,next,std::memory_order_release,std::memory_order_relaxed),27)){selected_=oldSelected;dispatch_=oldDispatch;producerId_.store(owner,std::memory_order_relaxed);return decline();}
 e.counted_=false;admissions_.fetch_add(1,std::memory_order_relaxed);RecordingToken r;r.owner_=this;r.command_=t.command;r.generation_=generation+1;r.producer_=id;return r;
}
WorkerPool28::Packet* WorkerPool28::reserveSlow(const RecordingToken&t,VkCommandBuffer c,std::uint64_t expected)noexcept{
 cachedTail_=consumed_.load(std::memory_order_seq_cst);
 if(head_-cachedTail_==Capacity){publish(head_);std::unique_lock<std::mutex> lock(mutex_);producerWaiting_.store(true,std::memory_order_seq_cst);producerSleeps_.fetch_add(1,std::memory_order_relaxed);testPoint(5);
  progress_.wait(lock,[&]{return consumed_.load(std::memory_order_seq_cst)>cachedTail_||admission_.load(std::memory_order_acquire)!=expected;});producerWaiting_.store(false,std::memory_order_seq_cst);cachedTail_=consumed_.load(std::memory_order_seq_cst);}
 if(admission_.load(std::memory_order_acquire)!=expected)return nullptr;return reserve(t,c);
}
void WorkerPool28::revoke()noexcept{revoked_.store(true,std::memory_order_release);auto entry=enter(AccessKind::Lookup);}
void WorkerPool28::stop()noexcept{
 revoked_.store(true,std::memory_order_release);auto entry=enter(AccessKind::Shutdown);
 auto w=admission_.load(std::memory_order_acquire);for(;;){const auto next=(w&~PhaseMask)|std::uint64_t(Phase::Stopped);if(admission_.compare_exchange_weak(w,next,std::memory_order_acq_rel,std::memory_order_acquire))break;}
}
void WorkerPool28::shutdownAfterApplicationQuiescence()noexcept{
 if(!thread_.joinable())return;if(consuming_==this)std::terminate();stop();
 // External all-entrypoint quiescence also covers producer's post-copy publication.
 if(countOf(admission_.load(std::memory_order_acquire)))std::terminate();
 {std::lock_guard<std::mutex> lock(mutex_);stopRequested_.store(true,std::memory_order_release);ready_.notify_all();}thread_.join();
}
WorkerPool28::Stats WorkerPool28::stats()const noexcept{
 Stats s;auto w=admission_.load(std::memory_order_acquire);s.enqueued=committed_.load(std::memory_order_acquire);s.executed=consumed_.load(std::memory_order_acquire);s.publications=publications_.load();s.closeRequests=closes_.load();s.synchronousEntries=entries_.load();s.admissions=admissions_.load();s.declinedAdmissions=declines_.load();s.producerSleeps=producerSleeps_.load();s.consumerSleeps=consumerSleeps_.load();s.rejectedPackets=rejected_.load();s.generation=generationOf(w);s.hostInflight=countOf(w);s.phase=phaseOf(w);s.permanentlyRevoked=revoked_.load();s.stopped=stopRequested_.load();return s;
}
void WorkerPool28::run()noexcept{
 consuming_=this;std::uint64_t tail{};
 for(;;){const auto limit=goal_.load(std::memory_order_acquire);
  while(tail<limit){execute((*packets_)[tail%Capacity]);++tail;consumed_.store(tail,std::memory_order_seq_cst);if(producerWaiting_.load(std::memory_order_seq_cst)){std::lock_guard<std::mutex> lock(mutex_);progress_.notify_all();}}
  std::unique_lock<std::mutex> lock(mutex_);consumerIdle_=true;
  progress_.notify_all(); // wake a counted producer waiting for an idle drain
  if(requestSequence_>ackSequence_&&tail>=requestTail_){ackSequence_=requestSequence_;progress_.notify_all();}
  if(stopRequested_.load(std::memory_order_acquire)&&tail==committed_.load(std::memory_order_acquire))break;
  testPoint(7);consumerParking89_.store(true,std::memory_order_seq_cst);testPoint(8);
  if(goal_.load(std::memory_order_seq_cst)==tail){
   consumerSleeps_.fetch_add(1,std::memory_order_relaxed);testPoint(10);
   ready_.wait(lock,[&]{return stopRequested_.load(std::memory_order_acquire)||goal_.load(std::memory_order_seq_cst)>tail||requestSequence_>ackSequence_;});
  }
  consumerParking89_.store(false,std::memory_order_seq_cst);
  consumerIdle_=false;
 }
 consuming_=nullptr;
}
void WorkerPool28::execute(Packet&q)noexcept{auto c=q.command;switch(q.op){
 case Op::ResetQueryPool:{auto&v=q.data.query;reinterpret_cast<PFN_vkCmdResetQueryPool>(q.raw)(c,v.pool,v.first,v.count);break;}
 case Op::WriteTimestamp:{auto&v=q.data.query;reinterpret_cast<PFN_vkCmdWriteTimestamp>(q.raw)(c,v.stage,v.pool,v.first);break;}
 case Op::Pipeline:{auto&v=q.data.pipeline;reinterpret_cast<PFN_vkCmdBindPipeline>(q.raw)(c,v.point,v.pipeline);break;}
 case Op::Descriptors:{auto&v=q.data.descriptors;reinterpret_cast<PFN_vkCmdBindDescriptorSets>(q.raw)(c,v.point,v.layout,v.first,v.count,v.count?v.sets:nullptr,v.n,v.n?v.offsets:nullptr);break;}
 case Op::Vertices:{auto&v=q.data.vertices;reinterpret_cast<PFN_vkCmdBindVertexBuffers>(q.raw)(c,v.first,v.count,v.count?v.buffers:nullptr,v.count?v.offsets:nullptr);break;}
 case Op::Index:{auto&v=q.data.index;reinterpret_cast<PFN_vkCmdBindIndexBuffer>(q.raw)(c,v.buffer,v.offset,v.type);break;}
 case Op::Draw:{auto&v=q.data.draw;reinterpret_cast<PFN_vkCmdDraw>(q.raw)(c,v.count,v.instances,v.first,v.base);break;}
 case Op::Indexed:{auto&v=q.data.draw;reinterpret_cast<PFN_vkCmdDrawIndexed>(q.raw)(c,v.count,v.instances,v.first,v.vertex,v.base);break;}
 case Op::Barrier:{auto&v=q.data.barrier;reinterpret_cast<PFN_vkCmdPipelineBarrier>(q.raw)(c,v.src,v.dst,v.flags,v.nm,v.nm?v.memory:nullptr,v.nb,v.nb?v.buffers:nullptr,v.ni,v.ni?v.images:nullptr);break;}
 case Op::BeginPass:{auto&v=q.data.beginPass;v.info.pClearValues=v.info.clearValueCount?v.clear:nullptr;reinterpret_cast<PFN_vkCmdBeginRenderPass>(q.raw)(c,&v.info,v.contents);break;}
 case Op::EndPass:reinterpret_cast<PFN_vkCmdEndRenderPass>(q.raw)(c);break;
 case Op::Viewport:{auto&v=q.data.viewport;reinterpret_cast<PFN_vkCmdSetViewport>(q.raw)(c,v.first,v.count,v.count?v.values:nullptr);break;}
 case Op::Scissor:{auto&v=q.data.scissor;reinterpret_cast<PFN_vkCmdSetScissor>(q.raw)(c,v.first,v.count,v.count?v.values:nullptr);break;}
 case Op::Stencil:{auto&v=q.data.stencil;reinterpret_cast<PFN_vkCmdSetStencilReference>(q.raw)(c,v.face,v.reference);break;}
 case Op::CopyImage:{auto&v=q.data.copy;reinterpret_cast<PFN_vkCmdCopyImage>(q.raw)(c,v.src,v.srcLayout,v.dst,v.dstLayout,v.count,v.count?v.regions:nullptr);break;}
 }}
}
