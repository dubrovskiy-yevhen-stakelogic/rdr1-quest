#pragma once
#include "RetailAllocatorPins.h"
#include <cstring>
namespace rdr2::worker28::retail_allocator::detail {
inline bool callbackAddresses(std::uintptr_t base,std::uintptr_t a,std::uintptr_t r,std::uintptr_t f) noexcept {
 return base&&base<=UINTPTR_MAX-0x18aac58U&&a==base+0x18aac30U&&r==base+0x18aac4cU&&f==base+0x18aac58U;
}
// Read is private to the qualifier; production always supplies the real mapped
// module reader. Host tests exercise the identical predicates with owned bytes.
template<class Read> bool imageBytes(Read&&read) noexcept {
 auto same=[&](std::uintptr_t rva,const void*b,std::size_t n,bool code){const auto*p=read(rva,n,code);return p&&std::memcmp(p,b,n)==0;};
 if(!same(0,ElfHeader,sizeof(ElfHeader),false)||!same(PhdrOffset,PhdrBytes,sizeof(PhdrBytes),false)||!same(NoteRva,NoteBytes,sizeof(NoteBytes),false))return false;
 for(const auto&p:CodePins)if(!same(p.rva,p.bytes,p.size,true))return false;
 return true;
}
template<class Read> bool gotTargets(Read&&read,std::uintptr_t base,std::uintptr_t mallocAddress,std::uintptr_t reallocAddress,std::uintptr_t freeAddress) noexcept {
 if(!base||base>UINTPTR_MAX-0x185fe98U||!mallocAddress||!reallocAddress||!freeAddress)return false;
 const std::uintptr_t rvas[]{0x2e153b0U,0x2e153a8U,0x2e15178U,0x2e12878U,0x2e15398U,0x2e12800U};
 const std::uintptr_t targets[]{base+0x185fe90U,base+0x185fe8cU,base+0x185fe98U,mallocAddress,reallocAddress,freeAddress};
 for(unsigned i=0;i<6;++i){const auto*p=read(rvas[i],sizeof(std::uintptr_t),false);std::uintptr_t actual{};if(!p)return false;std::memcpy(&actual,p,sizeof(actual));if(actual!=targets[i])return false;}
 return true;
}
}
