#pragma once
#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
namespace rdr95 {
// PRIVATE native effect graph revocation. A ticket is NOT a memory-lifetime
// lease: native reads still require the render/resource-owner borrow. Writers
// are counted across threads and nesting; only the last exit republishes quiet.
class EffectMutationEpoch {
 std::mutex mutex_;
 std::atomic<std::uint64_t> published_{0};
 std::uint64_t version_{2},writers_{};
 bool coverage_{},poisoned_{};
 void advance()noexcept{if(version_>std::numeric_limits<std::uint64_t>::max()-3)poisoned_=true;else version_+=2;}
 void publish()noexcept{published_.store(coverage_&&!poisoned_?(version_|std::uint64_t(writers_!=0)):0,std::memory_order_release);}
 void enter()noexcept{std::lock_guard lock(mutex_);if(!writers_)advance();if(writers_==std::numeric_limits<std::uint64_t>::max())poisoned_=true;else ++writers_;publish();}
 void leave()noexcept{std::lock_guard lock(mutex_);if(!writers_){poisoned_=true;publish();return;}--writers_;publish();}
public:
 EffectMutationEpoch()=default;EffectMutationEpoch(const EffectMutationEpoch&)=delete;EffectMutationEpoch&operator=(const EffectMutationEpoch&)=delete;
 // Call only after BOTH complete Load/Shutdown routes are installed/proven.
 // Disabling, re-enabling and rebinding always revoke previous tickets.
 void setCoverage(bool complete)noexcept{std::lock_guard lock(mutex_);advance();coverage_=complete;publish();}
 std::uint64_t ticket()const noexcept{const auto x=published_.load(std::memory_order_acquire);return (x&1)?0:x;}
 bool current(std::uint64_t ticket)const noexcept{return ticket&&!(ticket&1)&&published_.load(std::memory_order_acquire)==ticket;}
 class Scope {
  EffectMutationEpoch* owner_;
 public:
  explicit Scope(EffectMutationEpoch& owner)noexcept:owner_(&owner){owner_->enter();}
  ~Scope(){owner_->leave();}
  Scope(const Scope&)=delete;Scope&operator=(const Scope&)=delete;Scope(Scope&&)=delete;Scope&operator=(Scope&&)=delete;
 };
};
}
