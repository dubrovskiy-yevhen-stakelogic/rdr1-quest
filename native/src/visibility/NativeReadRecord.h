#pragma once
#include "NativeCurrentRecordProvider.h"
namespace rdr95 {
// Owned POD transfer, not a native lifetime permit. Only Admission issues this immediately
// after its successful record read while an actual
// binding-issued PreparationTicket remains inside the SAME ResourceGate Read.
class NativeReadRecord {
 friend class NativeCurrentAdmission;
 friend class NativeSelectedLodGeometry;
 const NativeCurrentRecordProvider* provider_{};
 const void* issuer_{};void* geometryContext_{};
 bool(*current_)(const void*,std::uint64_t)noexcept{};
 std::uint64_t providerRevision_{},recordRevision_{},frame_{},receiptSerial_{};
 unsigned index_{};Record record_{};
 // Optional observation belongs to the issuing recordCurrent stack frame. It outlives fingerprint
 // and its final record consumption, after which the issuer revokes receiptSerial_ BEFORE
 // destroying the observation. A default preparation transfer therefore neither clears nor
 // copies a 500-byte payload.
 const NativeCurrentRecordProvider::RecordSnapshot* snapshot_{};
public:
 NativeReadRecord()noexcept=default;
 // The issuer must outlive the lexical helper call. It validates its CURRENT
 // active scope by serial; it must never dereference a saved dead scope pointer.
 bool leaseCurrent(const NativeCurrentRecordProvider& p,unsigned index,void* context)const noexcept{
  return provider_==&p&&issuer_&&current_&&receiptSerial_&&geometryContext_==context&&
   providerRevision_&&providerRevision_==p.revision()&&frame_&&frame_==p.frame()&&
   index_==index&&record_.index==index&&current_(issuer_,receiptSerial_);
 }
 bool available(const NativeCurrentRecordProvider& p,unsigned index,void* context)const noexcept{
  return recordRevision_&&recordRevision_==p.recordRevision()&&leaseCurrent(p,index,context);
 }
 bool copy(const NativeCurrentRecordProvider& p,unsigned index,void* context,Record& out)const noexcept{
  if(!available(p,index,context))return false;
  out=record_;return true;
 }
};
}
