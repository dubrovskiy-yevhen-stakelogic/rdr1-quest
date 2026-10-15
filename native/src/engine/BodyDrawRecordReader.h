#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rdrvr::engine {
// Only called synchronously for the pinned live DrawCommon record fields.
// This is a borrowed native record read, not the diagnostic pipe reader.
inline bool readBodyDrawRecordField(void*, std::uintptr_t address, void* out, std::size_t size) {
    if (size != sizeof(std::uintptr_t)) return false;
    std::memcpy(out, reinterpret_cast<const void*>(address), size);
    return true;
}
}
