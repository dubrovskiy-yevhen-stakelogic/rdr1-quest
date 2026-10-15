#pragma once
#include "NativeResourceGate.h"
#include <cerrno>
namespace rdrvr::experimental {class NativeEarlyRuntimeBinding;}
namespace rdr95 {
// PRIVATE. A synchronous memory-liveness capability, never a semantic permit.
// Only the actual Binding can issue it after a successful strong Read entry.
// Covered writers publish mutation BEFORE waiting, but cannot change/free the
// borrowed resource graph until this exact physical root reader is released.
// No native call may occur inside the copy phase. No value survives Read end.
class NativePhysicalCopyScope {
 friend class rdrvr::experimental::NativeEarlyRuntimeBinding;
 const NativeResourceGate::ThreadReadState* ownerState_{};
 const void* owner_{};const NativeResourceGate::Read* read_{};
 std::uint64_t serial_{},stamp_{};
 int* errnoCell_{};

 void reset()noexcept{
  auto& state=NativeResourceGate::readState_;
  if(state.copy==this)state.copy=nullptr;
  ownerState_=nullptr;owner_=nullptr;read_=nullptr;serial_=stamp_=0;errnoCell_=nullptr;
 }
 bool activate(const void* owner,const NativeResourceGate::Read& read,std::uint64_t serial)noexcept{
  auto& state=NativeResourceGate::readState_;
  if(state.copy&&state.copy!=this)state.copy->reset();
  reset();
  if(!owner||!serial||serial==UINT64_MAX||!read.current()||read.root_!=&read||
     state.top!=&read||!state.stamp||state.stamp==UINT64_MAX)return false;
  // Cache the owning thread's cell, never its value. Each copy preserves
  // the errno value at that particular invocation, including output aliases.
  errnoCell_=&errno;
  ownerState_=&state;owner_=owner;read_=&read;serial_=serial;stamp_=state.stamp;state.copy=this;return true;
 }
 const NativeResourceGate::ThreadReadState* localState()const noexcept{
  const auto& state=NativeResourceGate::readState_;return state.copy==this?&state:nullptr;
 }
 // Binding MUST check its immutable owning thread id BEFORE this access.
 // The real lexical Read and its thread must outlive this capability; reset
 // clears this pointer before Read destruction. No thread-id reuse grant.
 const NativeResourceGate::ThreadReadState* ownerState()const noexcept{return ownerState_;}
 int* errnoCell()const noexcept{return errnoCell_;}
 bool current(const NativeResourceGate::ThreadReadState& state,const void* owner,std::uint64_t serial,const NativeResourceGate::Read* read)const noexcept{
  // TLS check comes first: a foreign thread must not inspect another thread's
  // lexical Read. Stamp changes precede Read destruction, avoiding stale access.
  if(state.copy!=this||stamp_!=state.stamp||!stamp_||stamp_==UINT64_MAX)return false;
  return owner_==owner&&serial_==serial&&serial&&read_==read&&read&&
      state.top==read&&read->owner_&&read->root_==read&&read->locked_;
 }
public:
 NativePhysicalCopyScope()noexcept=default;
 ~NativePhysicalCopyScope(){reset();}
 NativePhysicalCopyScope(const NativePhysicalCopyScope&)=delete;
 NativePhysicalCopyScope&operator=(const NativePhysicalCopyScope&)=delete;
};
}
