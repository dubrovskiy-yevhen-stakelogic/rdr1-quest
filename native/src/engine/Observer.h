#pragma once
#include <array>
#include <cstdint>
#include <string>
namespace rdrvr::engine {
struct SymbolSpec {
    const char* name;
    std::uint64_t offset,size;
    bool slot;
    std::array<unsigned char,32> fingerprint;
    unsigned fingerprintSize;
};
// Read-only snapshot: never calls a game function or writes a slot/code page.
std::string observe(const char* enginePath,const char* verifiedSha);
} // namespace rdrvr::engine
