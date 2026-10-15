#pragma once
#include "NativeAdmissionDecoder.h"
#include <atomic>
#include <thread>
namespace rdr95 {
// PRIVATE list lifetime only. Scope must bracket the existing original native
// RenderScene route; joined() is called AFTER original MAIN Wait returns. Revoke
// before the color-boundary consumer returns (after synchronous RIGHT), before
// native Signal(+70). Does not grant immutable actors, nodes, palettes, assets,
// effect graphs or GPU resource ownership. Separate resource borrow required.
class NativeJoinedListOwner {
public:
 static constexpr Address SceneReturn=0x2a585cc;
 static constexpr unsigned MaxRanges=96;
 struct Range {Address first{},end{};};
 class Ticket {friend class NativeJoinedListOwner;const NativeJoinedListOwner*owner_{};std::uint64_t scope_{},coverage_{},frame_{};public:explicit operator bool()const noexcept{return owner_&&scope_&&frame_;}};
private:
 struct State {NativeJoinedListOwner*owner{};State*previous{};Address renderer{},viewport{};Ready header{};std::array<Range,MaxRanges> ranges{};unsigned rangeCount{};std::uint64_t scope{},coverage{},frame{};bool occupied{},valid{},joined{};};
 inline static thread_local State*active_{};
 std::atomic<std::uint64_t> coverage_{},nextScope_{};std::atomic_flag occupied_=ATOMIC_FLAG_INIT;
 bool changeCoverage(bool armed)noexcept{auto old=coverage_.load(std::memory_order_acquire);for(;;){if(old>=UINT64_MAX-3){coverage_.store(UINT64_MAX,std::memory_order_release);return false;}const auto next=((old&~std::uint64_t(1))+2)|(armed?1:0);if(coverage_.compare_exchange_weak(old,next,std::memory_order_acq_rel))return true;}}
 void revoke()noexcept{auto old=coverage_.load(std::memory_order_acquire);for(;;){if(old>=UINT64_MAX-3){coverage_.store(UINT64_MAX,std::memory_order_release);return;}if(coverage_.compare_exchange_weak(old,old+2,std::memory_order_acq_rel))return;}}
 State*state()const noexcept{auto*s=active_;return s&&s->owner==this&&s->valid&&s->coverage!=UINT64_MAX&&(s->coverage&1)&&coverage_.load(std::memory_order_acquire)==s->coverage?s:nullptr;}
 static bool range(State&s,Address p,std::size_t n){if(!n)return true;if(!p||n>UINT64_MAX-p||s.rangeCount==MaxRanges)return false;s.ranges[s.rangeCount++]={p,p+n};return true;}
 static bool plus(Address p,std::size_t n,Address&out){if(!p||n>UINT64_MAX-p)return false;out=p+n;return true;}
 static bool equal(const Ready&a,const Ready&b){return a.manager==b.manager&&a.request==b.request&&a.container==b.container&&a.records==b.records&&a.buckets==b.buckets&&a.instanceList==b.instanceList&&a.recordCount==b.recordCount&&a.bucketCount==b.bucketCount&&a.nativeFrame==b.nativeFrame;}
 static bool header(const Api&a,Address manager,Ready&out){
  auto read=[&](Address p,auto&v){return p&&sizeof(v)<=UINT64_MAX-p&&a.read(p,&v,sizeof v,a.user);};Address p{},base{};out={};out.manager=manager;
  if(!plus(manager,8,p)||!read(p,base)||!plus(base,13*0x730,out.request))return false;
  std::uint8_t ready{};if(!plus(out.request,0xc2,p)||!read(p,ready)||!(ready&1)||!plus(out.request,0xb8,p)||!read(p,out.nativeFrame)||!plus(manager,0x1f0+13*8,p)||!read(p,out.container))return false;
  if(!read(out.container,out.buckets)||!plus(out.container,8,p)||!read(p,out.records)||!plus(out.container,0x94,p)||!read(p,out.recordCount)||!plus(out.container,0x98,p)||!read(p,out.bucketCount)||!plus(out.container,0xa0,p)||!read(p,out.instanceList))return false;
  return out.bucketCount<=32&&(!out.recordCount||out.records)&&(!out.bucketCount||out.buckets);
 }
public:
 // Call only as part of installation after the exact route pins and original
 // scene/MAIN-wait coverage are verified. Disarm revokes existing tickets.
 void coverage(bool armed)noexcept{changeCoverage(armed);}
 class SceneScope {
  friend class NativeJoinedListOwner;State s_;
 public:
  SceneScope(NativeJoinedListOwner&owner,Address renderer,Address viewport,Address returnOffset)noexcept{
   s_.owner=&owner;s_.previous=active_;s_.renderer=renderer;s_.viewport=viewport;s_.coverage=owner.coverage_.load(std::memory_order_acquire);
   if(s_.previous)s_.previous->valid=false;
   if(!s_.previous&&renderer&&viewport&&returnOffset==SceneReturn&&(s_.coverage&1)&&s_.coverage!=UINT64_MAX){
    s_.occupied=!owner.occupied_.test_and_set(std::memory_order_acquire);
    if(s_.occupied){s_.scope=owner.nextScope_.fetch_add(1,std::memory_order_relaxed)+1;s_.valid=s_.scope!=0;if(!s_.valid)owner.coverage(false);}
    else owner.revoke(); // overlapping routes cannot share a lease
   }
   active_=&s_;
  }
  ~SceneScope(){s_.valid=false;s_.joined=false;active_=s_.previous;if(s_.occupied)s_.owner->occupied_.clear(std::memory_order_release);}
  SceneScope(const SceneScope&)=delete;SceneScope&operator=(const SceneScope&)=delete;
 };
 // Initial reads are guarded or made under the separate short resource borrow.
 // No pointer ranges are published on partial validation, overflow or mismatch.
 bool joined(const Api&a,Address manager,unsigned request,Address returnOffset,std::uint64_t frame){
  auto*s=state();if(!s||s->joined||request!=MainRequest||returnOffset!=MainDepthWaitReturn||!frame||!a.read)return false;
  State next=*s;next.rangeCount=0;Ready h{};if(!header(a,manager,h))return false;
  if(!range(next,manager+8,8)||!range(next,manager+0x1f0+13*8,8)||!range(next,h.request+0xb8,4)||!range(next,h.request+0xc2,1)||!range(next,h.container,0xb0)||!range(next,h.records,std::size_t(h.recordCount)*RecordStride)||!range(next,h.buckets,std::size_t(h.bucketCount)*0x70)||!range(next,h.instanceList,h.instanceList?0xdc48:0))return false;
  std::uint64_t rows=0;for(unsigned b=0;b<h.bucketCount;++b){Address at=h.buckets+b*0x70;std::uint16_t cap{},n{};Address holder{},items{};
   if(!a.read(at,&cap,2,a.user)||!a.read(at+4,&n,2,a.user)||n>cap||(rows+=n)>262144)return false;if(!n)continue;
   if(!a.read(at+0x60,&holder,8,a.user)||!holder||holder>UINT64_MAX-8||!a.read(holder,&items,8,a.user)||!range(next,holder,8)||!range(next,items,std::size_t(n)*16))return false;
  }
  Ready confirm{};if(!header(a,manager,confirm)||!equal(h,confirm)||!state())return false;
  next.header=h;next.frame=frame;next.joined=true;*s=next;return true;
 }
 bool sceneActive(Address renderer,Address viewport)const noexcept{auto*s=state();return s&&s->renderer==renderer&&s->viewport==viewport;}
 bool current()const noexcept{auto*s=state();return s&&s->joined;}
 bool current(std::uint64_t frame)const noexcept{auto*s=state();return s&&s->joined&&s->frame==frame;}
 Ticket ticket()const noexcept{Ticket t;auto*s=state();if(s&&s->joined){t.owner_=this;t.scope_=s->scope;t.coverage_=s->coverage;t.frame_=s->frame;}return t;}
 bool current(const Ticket&t)const noexcept{auto*s=state();return s&&s->joined&&t.owner_==this&&t.scope_==s->scope&&t.coverage_==s->coverage&&t.frame_==s->frame;}
 const Ready* ready()const noexcept{auto*s=state();return s&&s->joined?&s->header:nullptr;}
 bool permitsListRead(Address p,std::size_t n)const noexcept{auto*s=state();if(!s||!s->joined||!p||!n||n>UINT64_MAX-p)return false;for(unsigned i=0;i<s->rangeCount;++i){const auto&r=s->ranges[i];if(r.first<=p&&p+n<=r.end)return true;}return false;}
 void closeMain()noexcept{if(auto*s=active_;s&&s->owner==this){s->valid=false;s->joined=false;s->rangeCount=0;}}
};
}
