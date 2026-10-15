#pragma once
#include "CertificateChecks.h"
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <cstring>

namespace rdr2::worker28::retail_allocator::detail {
// Published only after the full module/file/permission certificate succeeds.
// Its engine and libc loader references are retained for the process lifetime.
// Recheck the original callback code and GOT bytes without resolving modules or
// reopening /proc/self/maps for every resource. Never retain the caller's struct.
struct CachedCertificate {
 struct Block {const void* address{};const void* expected{};std::size_t bytes{};};
 std::uintptr_t base{};
 std::array<Block,3+sizeof(CodePins)/sizeof(CodePins[0])> blocks{};
 std::array<const void*,6> slots{};
 std::array<std::uintptr_t,6> targets{};
 template<class Read> bool capture(Read&& read,std::uintptr_t imageBase,
   std::uintptr_t allocation,std::uintptr_t reallocation,std::uintptr_t release) noexcept {
  base=imageBase;
  targets={base+0x185fe90U,base+0x185fe8cU,base+0x185fe98U,allocation,reallocation,release};
  unsigned n{};
  auto add=[&](std::uintptr_t rva,const void* expected,std::size_t bytes,bool code){
   const void* address=read(rva,bytes,code);if(!address)return false;
   blocks[n++]={address,expected,bytes};return true;
  };
  if(!add(0,ElfHeader,sizeof(ElfHeader),false)||!add(PhdrOffset,PhdrBytes,sizeof(PhdrBytes),false)||
     !add(NoteRva,NoteBytes,sizeof(NoteBytes),false))return false;
  for(const auto& pin:CodePins)if(!add(pin.rva,pin.bytes,pin.size,true))return false;
  constexpr std::uintptr_t rvas[]{0x2e153b0U,0x2e153a8U,0x2e15178U,0x2e12878U,0x2e15398U,0x2e12800U};
  for(unsigned i=0;i<slots.size();++i)if(!(slots[i]=read(rvas[i],sizeof(std::uintptr_t),false)))return false;
  return true;
 }
 bool matches(const VkAllocationCallbacks& c) const noexcept {
  if(c.pfnInternalAllocation||c.pfnInternalFree||!callbackAddresses(base,
      reinterpret_cast<std::uintptr_t>(c.pfnAllocation),reinterpret_cast<std::uintptr_t>(c.pfnReallocation),
      reinterpret_cast<std::uintptr_t>(c.pfnFree)))return false;
  for(const auto& b:blocks)if(!b.address||std::memcmp(b.address,b.expected,b.bytes))return false;
  for(unsigned i=0;i<slots.size();++i){std::uintptr_t actual{};if(!slots[i])return false;
   std::memcpy(&actual,slots[i],sizeof(actual));if(actual!=targets[i])return false;}
  // The certified leaf callbacks ignore pUserData; forwarding preserves it.
  return true;
 }
};
}
