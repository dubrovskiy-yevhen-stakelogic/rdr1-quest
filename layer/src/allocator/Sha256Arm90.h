#pragma once
#include <cstdint>
#if defined(__ANDROID__) && defined(__aarch64__)
#include <arm_neon.h>
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif

namespace rdr2::worker28::retail_allocator::detail {
inline bool sha256ArmAvailable90() noexcept {
#if defined(__ANDROID__) && defined(__aarch64__)
    return (getauxval(AT_HWCAP) & HWCAP_SHA2) != 0;
#else
    return false;
#endif
}

#if defined(__ANDROID__) && defined(__aarch64__)
// Called only after the kernel advertised SHA2. Keep the optional instructions
// inside this function; the ordinary ARM64 fallback uses the baseline ISA.
__attribute__((target("sha2"), noinline)) inline
void sha256ArmCompress90(std::uint32_t* state, const std::uint8_t* block) noexcept {
    static constexpr std::uint32_t k[64]{
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    const auto initialABCD = vld1q_u32(state);
    const auto initialEFGH = vld1q_u32(state + 4);
    auto abcd = initialABCD;
    auto efgh = initialEFGH;
    uint32x4_t words[4];
    for (unsigned i = 0; i < 4; ++i)
        words[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + i * 16)));
    for (unsigned round = 0; round < 16; ++round) {
        const unsigned slot = round & 3;
        const auto input = vaddq_u32(words[slot], vld1q_u32(k + round * 4));
        const auto oldABCD = abcd;
        abcd = vsha256hq_u32(abcd, efgh, input);
        efgh = vsha256h2q_u32(efgh, oldABCD, input);
        if (round < 12) {
            words[slot] = vsha256su0q_u32(words[slot], words[(slot + 1) & 3]);
            words[slot] = vsha256su1q_u32(words[slot], words[(slot + 2) & 3], words[(slot + 3) & 3]);
        }
    }
    vst1q_u32(state, vaddq_u32(abcd, initialABCD));
    vst1q_u32(state + 4, vaddq_u32(efgh, initialEFGH));
}
#endif
}
