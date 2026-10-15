#pragma once
#include <cstdint>
#include <limits>
#include <string_view>
namespace rdrvr::engine {
struct Segment { std::uintptr_t begin{},end{}; unsigned flags{}; };
inline bool contains(const Segment& s,std::uintptr_t address,std::uint64_t size,unsigned required) {
    return size && s.begin<=address && address<s.end && size<=s.end-address && (s.flags&required)==required;
}
inline bool relocate(std::uintptr_t base,std::uint64_t offset,std::uintptr_t& result) {
    if(offset>std::numeric_limits<std::uintptr_t>::max()-base) return false;
    result=base+static_cast<std::uintptr_t>(offset); return true;
}
inline bool acceptedHash(std::string_view actual,std::string_view expected) {
    return expected.size()==64 && actual==expected;
}
enum class SymbolState { Missing,WrongAddress,WrongRange,Unreadable,FingerprintMismatch,Matched };
inline SymbolState classify(std::uintptr_t actual,std::uintptr_t expected,bool range,bool readable,bool fingerprint) {
    if(!actual) return SymbolState::Missing;
    if(actual!=expected) return SymbolState::WrongAddress;
    if(!range) return SymbolState::WrongRange;
    if(!readable) return SymbolState::Unreadable;
    if(!fingerprint) return SymbolState::FingerprintMismatch;
    return SymbolState::Matched;
}
} // namespace rdrvr::engine
