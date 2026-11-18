#pragma once
// gohGlowManager: the engine's table of glows attached to game objects. Read only, nothing is hooked.
//
// Instance: gohGlowManager::smp_Instance (+4e7dd6e8, GLOB_DAT GOT word +2e02bc0).
// Manager: entry[64], stride 0x30, then +0xc00 int32 count.
// Entry:
//   +0x00 Vector4 offset in the object's matrix
//   +0x10 gohGUID of the object
//   +0x18 OpticalEffect* (optic type 5). gohGlowManager::UpdateFX (+266f19c) sorts the table with GlowDist::GlowSort (+266f930: rank high
//         first, then near first) and keeps an effect only on the first 16; every other entry holds null. Rank = 3 with priority, 2 without,
//         minus 2 outside the view cone.
//   +0x20 u8 priority
//   +0x21 u8 eGlowType
// World position: UpdateFX stores object matrix x offset at effect+0 (+266f7a8..+266f7f8). It is skipped when the object has no matrix, so an
// effect that never got one still holds its first value; positions around the origin are dropped here.
//
// Writers of an entry, all checked by the prints below:
//   gohGlowManager::CreateObjectGlow (+266ef40): +0x20 = argument bool, +0x21 = type, offset = argument. Its only callers are script
//     commands: the native 0x1065d334 thunk +293c470 and sagFxScript::CreateGlowOnObject.
//   gohPickupManager::AddPickup (+266ee48) and gohPickupManager::Update (+266fc18): strh wzr at +0x20 and stp xzr,xzr at +0x00, so a pickup
//     always has priority 0, type 0 and offset 0.
// Priority is therefore never set on a pickup. In the decoded game scripts the native has 190 call sites and passes a constant bool at each:
// 189 pass 0 (type 0: 126, type 3: 63), one passes 1 (script 1571 at 0xcb7a, the Merchant02 skull, type 0).
//
// The engine writes the table with no lock a reader could take (script commands, the pickup manager, UpdateFX). Everything here goes through
// the guarded reader, so a stale effect pointer cannot fault; a torn read yields at worst one wrong position until the next read, which can
// change one object's cull decision or one beam and nothing else.
#include "diagnostics/GuardedReadVector.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rdrvr::engine::objectglow {
using ReadFn=bool(*)(void*,std::uintptr_t,void*,std::size_t);
inline constexpr std::uintptr_t instanceGotOffset=0x2e02bc0,instanceOffset=0x4e7dd6e8;
inline constexpr unsigned capacity=64,maxPositions=16;
inline constexpr std::size_t entryBytes=0x30,countOffset=0xc00,entryEffect=0x18,entryPriority=0x20,entryType=0x21;

struct Print {std::uintptr_t offset;std::uint32_t size;std::uint64_t fnv;};
// FNV-1a 64 of the engine code the layout above comes from (file offset = vaddr - 0x4000 in the R E segment).
inline constexpr std::array<Print,5> prints{{
    {0x266ef40,0x94,0xde12459068f562bbull},  // gohGlowManager::CreateObjectGlow
    {0x266ee48,0xf8,0xe322b2a22cb5d97full},  // gohPickupManager::AddPickup
    {0x266fc18,0x24,0xe39ef17ec1ccc10full},  // gohPickupManager::Update, the entry it adds
    {0x266f6a8,0x164,0xc48f6b5d086810e0ull}, // gohGlowManager::UpdateFX, world position -> effect+0
    {0x293c470,0x7c,0xa7bebd3adc5a92c9ull},  // script native 0x1065d334 -> CreateObjectGlow
}};

// Address of smp_Instance; 0 until install() has checked the prints and the GOT word. 0 = every reader gets an empty table.
inline std::atomic<std::uintptr_t> instanceSlot{};
inline std::atomic<const char*> installNote{"NOT RUN"};

struct Glow {float pos[3]{};std::uint8_t priority{},type{};};

inline std::uint64_t codeHash(ReadFn read,std::uintptr_t address,std::uint32_t size) noexcept {
    std::uint64_t h=0xcbf29ce484222325ull;unsigned char chunk[256];
    for(std::uint32_t done=0;done<size;){const std::uint32_t n=std::min<std::uint32_t>(size-done,sizeof chunk);
        if(!read(nullptr,address+done,chunk,n))return 0;
        for(std::uint32_t i=0;i<n;++i)h=(h^chunk[i])*0x100000001b3ull;
        done+=n;}
    return h;
}
inline void install(ReadFn read,std::uintptr_t base) noexcept {
    for(const auto& p:prints)if(codeHash(read,base+p.offset,p.size)!=p.fnv){installNote.store("PRINTS",std::memory_order_relaxed);return;}
    std::uintptr_t got{};
    if(!read(nullptr,base+instanceGotOffset,&got,sizeof got)||got!=base+instanceOffset){installNote.store("GOT",std::memory_order_relaxed);return;}
    instanceSlot.store(got,std::memory_order_relaxed);installNote.store("OK",std::memory_order_relaxed);
}
inline bool usable(const float* p) noexcept {
    return std::isfinite(p[0])&&std::isfinite(p[1])&&std::isfinite(p[2])&&std::fabs(p[0])<1e5f&&std::fabs(p[1])<1e5f&&std::fabs(p[2])<1e5f&&
        std::fabs(p[0])+std::fabs(p[1])+std::fabs(p[2])>1.f;
}
// Glows that hold an effect, with their world positions. entries = table size, with or without an effect. At most maxPositions.
inline unsigned readPositions(ReadFn read,Glow* out,unsigned max,unsigned& entries) noexcept {
    entries=0;
    try{
        const auto slot=instanceSlot.load(std::memory_order_relaxed);
        if(!read||!slot||!out||!max)return 0;
        std::uintptr_t manager{};std::int32_t count{};
        if(!read(nullptr,slot,&manager,sizeof manager)||!manager||!read(nullptr,manager+countOffset,&count,sizeof count))return 0;
        if(count<=0||count>int(capacity))return 0;
        entries=unsigned(count);
        unsigned char table[capacity*entryBytes];
        if(!read(nullptr,manager,table,std::size_t(entries)*entryBytes))return 0;
        std::array<diagnostics::ReadSegment,maxPositions> segments{};std::array<std::array<float,3>,maxPositions> positions{};std::array<unsigned,maxPositions> from{};
        unsigned n=0;const unsigned limit=std::min(max,maxPositions);
        for(unsigned i=0;i<entries&&n<limit;++i){
            std::uintptr_t effect{};std::memcpy(&effect,table+std::size_t(i)*entryBytes+entryEffect,sizeof effect);
            if(!effect)continue;
            segments[n]={effect,positions[n].data(),sizeof(float)*3};from[n]=i;++n;
        }
        if(!diagnostics::guardedReadVector.read(read,nullptr,segments.data(),n))
            for(unsigned i=0;i<n;++i)if(!read(nullptr,segments[i].source,segments[i].target,segments[i].size))positions[i]={};
        unsigned found=0;
        for(unsigned i=0;i<n;++i){
            if(!usable(positions[i].data()))continue;
            const auto* e=table+std::size_t(from[i])*entryBytes;auto& g=out[found++];
            g.pos[0]=positions[i][0];g.pos[1]=positions[i][1];g.pos[2]=positions[i][2];g.priority=e[entryPriority];g.type=e[entryType];
        }
        return found;
    }catch(...){return 0;}
}
} // namespace rdrvr::engine::objectglow
