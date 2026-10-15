#pragma once
#include <cstdint>
namespace rdrvr::experimental {class NativeEarlyRuntimeBinding;}
namespace rdr95 {
// PRIVATE factory-issued current-Read storage authority. It permits omission
// ONLY of the exact VkBuffer ctor/dtor-only header journal. Native selectors,
// VB/IB wrapper bytes, FVF and material pointers remain fresh. Never a skip.
class NativeGeometryStorageTicket {
 friend class rdrvr::experimental::NativeEarlyRuntimeBinding;
 friend class NativeCurrentAdmission;
 friend class NativeSelectedLodGeometry;
 const void* owner_{};const void* read_{};void* context_{};
 std::uint64_t serial_{},frame_{};
 bool(*current_)(const void*,const void*,std::uint64_t)noexcept{};
 bool matches(std::uint64_t frame,void* context)const noexcept{return frame_==frame&&context_==context&&bool(*this);}
public:
 explicit operator bool()const noexcept{return owner_&&read_&&serial_&&current_&&current_(owner_,read_,serial_);}
};
}
