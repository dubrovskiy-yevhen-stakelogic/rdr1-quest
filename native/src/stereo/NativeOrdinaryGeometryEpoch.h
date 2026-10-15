#pragma once
#include "NativeResourceGate.h"
#include "opaque-run/NativeEarlyColdOwners.h"
#include <array>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <type_traits>
#include <utility>
namespace rdrvr::experimental {
// Independent packet-geometry invalidation. Every observed event matters,
// including backings absent from the software occluder cache. This is NOT a
// claim that all native writers are installed, a Read or GPU lifetime lease.
// Consumer admission separately requires exact installed event coverage.
class NativeOrdinaryGeometryEpoch final {
public:
 enum class Kind:unsigned {Vertex,Index};
 enum class Entry:unsigned {Generic,ReadOnly};
 class Observation {
  friend class NativeOrdinaryGeometryEpoch;const NativeOrdinaryGeometryEpoch*owner_{};std::uint64_t word_{};NativeEarlyColdOwners::Observation cold_{};bool selectedPair_{};
 public:bool current()const noexcept{return owner_&&word_&&(!(word_&1)||selectedPair_)&&cold_.current()&&owner_->published_.load(std::memory_order_acquire)==word_&&cold_.current();}
 };
 // Metadata access is a distinct token, never a geometry replay certificate.
 // Short ResourceRead still excludes observed header/lifetime mutations.
 class MetadataObservation {
  friend class NativeOrdinaryGeometryEpoch;const NativeOrdinaryGeometryEpoch*owner_{};std::uint64_t word_{};NativeEarlyColdOwners::Observation cold_{};
 public:bool current()const noexcept{return owner_&&word_&&cold_.current()&&owner_->published_.load(std::memory_order_acquire)==word_&&cold_.current();}
 };
 static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
 NativeOrdinaryGeometryEpoch()=default;
 NativeOrdinaryGeometryEpoch(const NativeOrdinaryGeometryEpoch&)=delete;
 // Bounded state history under the existing accounting lock, no native reads
 // or clocks. Freeze on the FIRST accounting failure; later calls cannot hide it.
 std::string report()const {
  std::lock_guard lock(mutex_);std::ostringstream o;
  o<<"{\"published\":"<<published_.load()<<",\"version\":"<<version_<<",\"armed\":"<<(armed_?"true":"false")
   <<",\"failed\":"<<(failed_?"true":"false")<<",\"firstFailure\":"<<firstFailure_<<",\"active\":"<<active_
   <<",\"coldCurrent\":"<<(cold_.current()?"true":"false")<<",\"eventsSeen\":"<<eventsSeen_<<",\"history\":[";
  const auto n=std::min<std::uint64_t>(eventsSeen_,history_.size());
  for(std::uint64_t i=0;i<n;++i){if(i)o<<',';const auto&e=history_[(eventsSeen_-n+i)%history_.size()];o<<'[';
   for(unsigned j=0;j<e.size();++j){if(j)o<<',';o<<'\"'<<e[j]<<'\"';}o<<']';}
  o<<"],\"mapped\":[";bool first=true;for(const auto&m:maps_)if(m.key){if(!first)o<<',';first=false;o<<"[\""<<m.key<<"\","<<unsigned(m.kind)<<','<<(m.mapped?"true":"false")<<']';}o<<"]}";return o.str();
 }
 // Issuance is cold/once per short Read; consumption is atomic-only.
 Observation observe()const noexcept {std::lock_guard lock(mutex_);Observation o;const auto w=published_.load(std::memory_order_acquire);if(w&&!(w&1)&&cold_.current()){o.owner_=this;o.word_=w;o.cold_=cold_;}return o;}
 MetadataObservation observeMetadata()const noexcept {
  std::lock_guard lock(mutex_);MetadataObservation o;const auto w=published_.load(std::memory_order_acquire);
  if(w&&cold_.current()){o.owner_=this;o.word_=w;o.cold_=cold_;}return o;
 }
 Observation observePair(std::uintptr_t vertex,std::uintptr_t index)const noexcept {
  std::lock_guard lock(mutex_);Observation o;const auto w=published_.load(std::memory_order_acquire);
  if(!vertex||!index||!w||!cold_.current())return o;
  std::uint64_t mapped{};
  for(const auto&m:maps_)if(m.key){
   // In-flight Lock/Unlock and unknown global mutation are never certified.
   if(!m.mapped||(m.kind==Kind::Vertex&&m.key==vertex)||(m.kind==Kind::Index&&m.key==index))return o;
   ++mapped;
  }
  if(active_!=mapped)return o;
  o.owner_=this;o.word_=w;o.cold_=cold_;o.selectedPair_=true;return o;
 }
 bool arm(NativeEarlyColdOwners& owners)noexcept {
  const auto cold=owners.observe();if(!cold.current())return false;
  std::lock_guard lock(mutex_);if(failed_||active_||!cold.current())return false;
  advance();cold_=cold;armed_=true;publish();event(1);return cold.current()&&!failed_;
 }
 // Call before any observed cold hook withdrawal/republication. Mapped
 // activity is preserved; rearming cannot erase an outstanding native map.
 void suspend()noexcept {std::lock_guard lock(mutex_);armed_=false;advance();publish();event(2);}
 void disarm()noexcept {std::lock_guard lock(mutex_);failure(1);publish();}
 // These adapters preserve result/exception/errno and forward exactly once,
 // including after accounting failure. No mutex is held across originals.
 template<class Original>auto map(rdr95::NativeResourceGate&gate,const void*key,Kind kind,Original&& original,Entry entry=Entry::Generic)->std::invoke_result_t<Original> {
  Preserve preserve;
  // Pinned native RO/Generic entries tail-delegate to each other. Only one
  // cross-entry delegation for the same object, thread, owner and gate belongs
  // to the outer mapping. The outer Writer and token cover the whole original.
  auto* parent=mapCall_;const bool delegate=parent&&parent->owner==this&&parent->gate==&gate&&
      parent->key==key&&parent->kind==kind&&parent->entry!=entry&&!parent->delegated&&!parent->nested;
  MapCall call(this,&gate,key,kind,entry,delegate);
  if(delegate){parent->delegated=true;
   try{auto result=std::forward<Original>(original)();preserve.error=errno;return result;}
   catch(...){preserve.error=errno;disarm();throw;}
  }
  const auto token=beginMap(reinterpret_cast<std::uintptr_t>(key),kind);
  rdr95::NativeResourceGate::Writer writer(gate);
  errno=preserve.error;
  try{auto result=std::forward<Original>(original)();preserve.error=errno;finishMap(token,bool(result));return result;}
  catch(...){preserve.error=errno;finishMap(token,false);disarm();throw;}
 }
 template<class Original>auto unmap(rdr95::NativeResourceGate&gate,const void*key,Kind kind,Original&& original)->std::invoke_result_t<Original> {
  Preserve preserve;Mutation active(*this);rdr95::NativeResourceGate::Writer writer(gate);errno=preserve.error;
  try{
   if constexpr(std::is_void_v<std::invoke_result_t<Original>>){std::forward<Original>(original)();preserve.error=errno;finishUnmap(reinterpret_cast<std::uintptr_t>(key),kind,true);}
   else {auto result=std::forward<Original>(original)();preserve.error=errno;finishUnmap(reinterpret_cast<std::uintptr_t>(key),kind,bool(result));return result;}
  }catch(...){preserve.error=errno;disarm();throw;}
 }
 template<class Original>auto mutate(rdr95::NativeResourceGate&gate,Original&& original)->std::invoke_result_t<Original> {
  Preserve preserve;Mutation active(*this);rdr95::NativeResourceGate::Writer writer(gate);errno=preserve.error;
  try{
   if constexpr(std::is_void_v<std::invoke_result_t<Original>>){std::forward<Original>(original)();preserve.error=errno;}
   else{auto result=std::forward<Original>(original)();preserve.error=errno;return result;}
  }catch(...){preserve.error=errno;throw;}
 }
private:
 struct MapCall;
 inline static thread_local MapCall* mapCall_{};
 struct MapCall {
  NativeOrdinaryGeometryEpoch*owner; rdr95::NativeResourceGate*gate;const void*key;Kind kind;Entry entry;
  bool nested{},delegated{};MapCall*previous;
  MapCall(NativeOrdinaryGeometryEpoch*o,rdr95::NativeResourceGate*g,const void*k,Kind kindValue,Entry entryValue,bool n):
    owner(o),gate(g),key(k),kind(kindValue),entry(entryValue),nested(n),previous(mapCall_){mapCall_=this;}
  ~MapCall(){mapCall_=previous;}
 };
 struct Preserve {int error=errno;~Preserve(){errno=error;}};
 struct Map {std::uintptr_t key{};std::uint64_t serial{};Kind kind{};bool mapped{};};
 struct Token {unsigned index{128};std::uint64_t serial{};};
 mutable std::mutex mutex_;NativeEarlyColdOwners::Observation cold_{};bool armed_{};std::array<Map,128> maps_{};std::uint64_t version_{2},active_{};bool failed_{};
 std::atomic<std::uint64_t> published_{};
 std::array<std::array<std::uint64_t,7>,128> history_{};std::uint64_t eventsSeen_{};unsigned firstFailure_{};
 void event(unsigned op,std::uintptr_t key=0,unsigned kind=0,unsigned result=0)noexcept {
  if(firstFailure_)return;history_[eventsSeen_++%history_.size()]={op,key,kind,result,active_,version_,unsigned(armed_)};
 }
 void failure(unsigned reason)noexcept {if(!firstFailure_){event(9,0,0,reason);firstFailure_=reason;}failed_=true;}
 void advance()noexcept{if(version_>UINT64_MAX-3)failure(2);else version_+=2;}
 void publish()noexcept{published_.store((failed_||!armed_)?0:(version_|std::uint64_t(active_!=0)),std::memory_order_release);}
 void enter()noexcept{std::lock_guard lock(mutex_);advance();if(active_==UINT64_MAX)failure(3);else ++active_;publish();event(3);}
 void leave()noexcept{std::lock_guard lock(mutex_);if(!active_)failure(4);else --active_;publish();event(4);}
 class Mutation {NativeOrdinaryGeometryEpoch&owner_;public:explicit Mutation(NativeOrdinaryGeometryEpoch&o)noexcept:owner_(o){owner_.enter();}~Mutation(){owner_.leave();}Mutation(const Mutation&)=delete;};
 Token beginMap(std::uintptr_t key,Kind kind)noexcept {
  std::lock_guard lock(mutex_);advance();Token t;
  event(5,key,unsigned(kind));
  if(!key||failed_){failure(5);publish();return t;}
  // Native cloth reads and OptimiseVertexBuffersD11 writes can overlap on
  // different threads (guest41: callers 1f07334 / 1b15e58). Each non-delegated
  // Lock owns a separate token until its Unlock; duplicate keys are not a
  // permanent accounting failure. In-flight entries deny ALL observations;
  // mapped entries deny their selected pair until EVERY matching map ends.
  // Unlock may retire any completed token for the key: only the aggregate
  // count is observable, and every begin/unmap changes the global version.
  for(unsigned i=0;i<maps_.size();++i)if(!maps_[i].key){
   if(active_==UINT64_MAX){failure(3);break;}++active_;maps_[i]={key,version_,kind,false};t={i,version_};publish();return t;
  }
  failure(7);publish();return t;
 }
 void finishMap(Token token,bool success)noexcept {
  std::lock_guard lock(mutex_);
  if(token.index>=maps_.size()||!maps_[token.index].key||maps_[token.index].serial!=token.serial||maps_[token.index].mapped){failure(8);publish();return;}
  auto&m=maps_[token.index];event(6,m.key,unsigned(m.kind),unsigned(success));if(success)m.mapped=true;else {m={};if(active_)--active_;else failure(9);}publish();
 }
 void finishUnmap(std::uintptr_t key,Kind kind,bool success)noexcept {
  std::lock_guard lock(mutex_);
  event(7,key,unsigned(kind),unsigned(success));
  for(auto&m:maps_)if(key&&m.key==key&&m.kind==kind&&m.mapped){
   if(!success){failure(10);publish();return;}m={};if(active_)--active_;else failure(11);publish();return;
  }
  // Unknown or pre-install Lock interval cannot manufacture a quiet epoch.
  failure(12);publish();
 }
 friend struct NativeOrdinaryGeometryEpochTestAccess;
};
inline NativeOrdinaryGeometryEpoch nativeOrdinaryGeometryEpoch;
}
