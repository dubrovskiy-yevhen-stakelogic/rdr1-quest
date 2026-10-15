#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "Sha256Arm90.h"

namespace rdr2::worker28::retail_allocator::detail {
// Small streaming SHA-256 used only by cold file qualification. The host tests
// compare empty/abc/long/multi-block vectors and the complete pinned engine.
class Sha256 {
 std::uint32_t state_[8]{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
  0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
 std::uint8_t block_[64]{}; std::size_t used_{}; std::uint64_t bytes_{};
 static std::uint32_t rotr(std::uint32_t x,unsigned n) noexcept {return(x>>n)|(x<<(32-n));}
#if defined(__ANDROID__) && defined(__aarch64__)
 const bool hardware90_;
#endif
 void compress() noexcept {
#if defined(__ANDROID__) && defined(__aarch64__)
  if(hardware90_){sha256ArmCompress90(state_,block_);return;}
#endif
  static constexpr std::uint32_t k[64]{
   0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
   0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
   0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
   0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
   0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
   0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
   0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
   0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
  std::uint32_t w[64]{};
  for(unsigned i=0;i<16;++i)w[i]=(std::uint32_t(block_[i*4])<<24)|(std::uint32_t(block_[i*4+1])<<16)|(std::uint32_t(block_[i*4+2])<<8)|block_[i*4+3];
  for(unsigned i=16;i<64;++i){const auto a=w[i-15],b=w[i-2];w[i]=w[i-16]+(rotr(a,7)^rotr(a,18)^(a>>3))+w[i-7]+(rotr(b,17)^rotr(b,19)^(b>>10));}
  auto a=state_[0],b=state_[1],c=state_[2],d=state_[3],e=state_[4],f=state_[5],g=state_[6],h=state_[7];
  for(unsigned i=0;i<64;++i){const auto x=h+(rotr(e,6)^rotr(e,11)^rotr(e,25))+((e&f)^(~e&g))+k[i]+w[i];const auto y=(rotr(a,2)^rotr(a,13)^rotr(a,22))+((a&b)^(a&c)^(b&c));h=g;g=f;f=e;e=d+x;d=c;c=b;b=a;a=x+y;}
  state_[0]+=a;state_[1]+=b;state_[2]+=c;state_[3]+=d;state_[4]+=e;state_[5]+=f;state_[6]+=g;state_[7]+=h;
 }
 public:
 explicit Sha256(bool portableOnly90=false) noexcept
#if defined(__ANDROID__) && defined(__aarch64__)
  :hardware90_(!portableOnly90&&sha256ArmAvailable90())
#endif
 { (void)portableOnly90; }
 bool hardwareSelected90() const noexcept {
#if defined(__ANDROID__) && defined(__aarch64__)
  return hardware90_;
#else
  return false;
#endif
 }
 void update(const void* data,std::size_t n) noexcept {
  const auto*p=static_cast<const std::uint8_t*>(data);bytes_+=n;
  while(n){const auto m=n<64-used_?n:64-used_;std::memcpy(block_+used_,p,m);p+=m;n-=m;used_+=m;if(used_==64){compress();used_=0;}}
 }
 std::array<std::uint8_t,32> finish() noexcept {
  const auto bits=bytes_*8;block_[used_++]=0x80;
  if(used_>56){std::memset(block_+used_,0,64-used_);compress();used_=0;}
  std::memset(block_+used_,0,56-used_);for(unsigned i=0;i<8;++i)block_[63-i]=std::uint8_t(bits>>(i*8));compress();
  std::array<std::uint8_t,32> out{};for(unsigned i=0;i<8;++i)for(unsigned j=0;j<4;++j)out[i*4+j]=std::uint8_t(state_[i]>>(24-8*j));return out;
 }
};
}
