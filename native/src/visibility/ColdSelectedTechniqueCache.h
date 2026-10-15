#pragma once
#include "ColdMaterialVariants.h"
#include <thread>
namespace rdr95 {
// PRIVATE bounded cold certificate memo. It never calls native code. Input
// pointers are read only under the supplied current native owner borrow.
// Returned storage is stable, but its content may change on a later lookup
// after global revocation. Consume it synchronously, never retain the pointer.
// Callers must still call
// ColdMaterialVariants::current and compare material/effect/covered technique.
class ColdSelectedTechniqueCache {
public:
 using M=ColdMaterialVariants;static constexpr unsigned Capacity=1024;
 struct Statistics {unsigned lookups{},hits{},builds{},negativeHits{},failed{},budget{},ownerDeclines{};};
 ColdSelectedTechniqueCache()=default;ColdSelectedTechniqueCache(const ColdSelectedTechniqueCache&)=delete;
 bool configure(const M::Api& api,M::Owner owner,unsigned capacity=Capacity){
  clear();if(!api.memory.read||!api.registry||!api.device||!api.effectEpoch||!api.ownerCurrent||!api.shaderSerial||!api.shaderSetCurrent||!owner.identity||!owner.generation||!capacity||capacity>Capacity)return false;
  api_=api;owner_=owner;capacity_=capacity;thread_=std::this_thread::get_id();return true;
 }
 void clear()noexcept{for(auto&e:entries_)e.reset();capacity_=0;api_={};owner_={};stats_={};}
 const M::Receipt* lookup(Address material,unsigned technique)noexcept {
  if(thread_!=std::this_thread::get_id())return nullptr;
  ++stats_.lookups;if(!capacity_||!material||technique<1||technique>256){++stats_.ownerDeclines;return nullptr;}
  const auto serial=api_.shaderSerial(*api_.registry),epoch=api_.effectEpoch->ticket();Address effect{};
  if(!serial||!epoch||material>UINT64_MAX-0x18||!api_.memory.read(material+0x10,&effect,8,api_.memory.user)||!effect||!api_.ownerCurrent(api_.ownerContext,owner_,material,effect)){++stats_.ownerDeclines;return nullptr;}
  unsigned free=capacity_;const unsigned start=unsigned((material^(effect>>4)^technique)%capacity_);
  for(unsigned n=0;n<capacity_;++n){const unsigned at=(start+n)%capacity_;auto&e=entries_[at];if(!e){if(free==capacity_)free=at;continue;}
   // Retained objects stay allocated; no live typed receipt is replaced. Old
   // global tickets revoke the graph first, so stale slots can be overwritten.
   if(e->serial!=serial||e->epoch!=epoch){if(free==capacity_)free=at;continue;}
   if(e->material!=material||e->effect!=effect||e->technique!=technique)continue;
   if(!api_.effectEpoch->current(epoch)||!api_.shaderSetCurrent(*api_.registry,serial))return nullptr;
   if(e->negative){++stats_.negativeHits;return nullptr;}
   if(M::current(api_,e->receipt)){++stats_.hits;return &e->receipt;}++stats_.ownerDeclines;return nullptr;
  }
  if(free==capacity_){++stats_.budget;return nullptr;}++stats_.builds;
  try {
   if(!entries_[free])entries_[free]=std::make_unique<Entry>();auto&e=*entries_[free];e.receipt.reset();e.material=material;e.effect=effect;e.technique=technique;e.serial=serial;e.epoch=epoch;e.negative=false;
   M::Stats proof{};if(!M::buildTechniques(api_,owner_,material,std::span(&technique,1),e.receipt,proof)){
    // Structural/position refusal is stable only for this exact graph epoch.
    // Transient failed reads/budgets/owner races are retried on a later lookup.
    ++stats_.failed;e.negative=proof.failure==M::Failure::Position||proof.failure==M::Failure::Effects||proof.failure==M::Failure::Constants||proof.failure==M::Failure::Pass||proof.failure==M::Failure::Program;
    if(!e.negative){e.serial=0;e.epoch=0;}return nullptr;
   }
   if(e.receipt.effect()!=effect||e.receipt.shaderSerial()!=serial||e.receipt.effectTicket()!=epoch||!M::current(api_,e.receipt)){e.serial=0;e.epoch=0;return nullptr;}
   return &e.receipt;
  }catch(...){++stats_.failed;if(entries_[free]){entries_[free]->receipt.reset();entries_[free]->serial=0;entries_[free]->epoch=0;}return nullptr;}
 }
 const Statistics& statistics()const noexcept{return stats_;}
private:
 struct Entry {Address material{},effect{};std::uint64_t serial{},epoch{};unsigned technique{};bool negative{};M::Receipt receipt;};
 std::array<std::unique_ptr<Entry>,Capacity> entries_{};M::Api api_{};M::Owner owner_{};unsigned capacity_{};std::thread::id thread_{};Statistics stats_{};
};
}
