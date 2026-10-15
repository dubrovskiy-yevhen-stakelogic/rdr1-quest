#pragma once
#include <cstdint>
namespace rdrvr::experimental { class NativeEarlyRuntimeBinding; }
namespace rdr95 {
class NativeCurrentAdmission;
// PRIVATE: one physical Read only, not semantic validity or skip authority.
// Only Binding issues; Admission consumes solely inside its exact DrawScope.
// The issuing Binding and its lexical Read must outlive all uses.
class NativeReadQualification {
 friend class NativeCurrentAdmission;
 friend class rdrvr::experimental::NativeEarlyRuntimeBinding;
 const void* owner_{};const void* read_{};std::uint64_t serial_{};
 bool(*live_)(const void*,const void*,std::uint64_t)noexcept{};
 bool issued()const noexcept{return owner_!=nullptr;}
 bool live()const noexcept{return owner_&&read_&&serial_&&live_&&live_(owner_,read_,serial_);}
public:
 NativeReadQualification()noexcept=default;
};
}
