#pragma once
#include <array>
#include <cstdint>
#include <cstring>
namespace rdrvr::engine {
// IntendedTargetData::Reset, bhWeaponsPacket::Zero and SetupTargetPacket in
// the pinned retail image establish the untracked vector-target defaults.
// Do not carry an old character, velocity or body-part tag to a new ray.
struct alignas(16) WeaponFreeAimTarget {
    std::array<std::uint8_t,48> intended{};
    static constexpr bool tracked=false;
    static constexpr std::uint32_t guid=0;
    static constexpr int mode=-1;
    static constexpr std::uint16_t instance=0,part=0xffff;
    static constexpr int index=0;
    WeaponFreeAimTarget(){
        const std::uint32_t noMode=0xffffffffu;
        std::memcpy(intended.data()+0x24,&noMode,sizeof(noMode));
    }
};
static_assert(sizeof(WeaponFreeAimTarget)==48&&alignof(WeaponFreeAimTarget)==16);
}
