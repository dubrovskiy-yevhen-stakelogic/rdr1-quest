#pragma once
#include "HudAlphaFlush.h"
#include <cstddef>
#include <cstring>
namespace rdrvr::engine {
// Relative layout of the pinned grcEffect requested blend state. Read as one
// bounded snapshot; never alias or write engine storage through this adapter.
struct HudBlendReader {
    using Read=bool(*)(void*,std::uintptr_t,void*,std::size_t);
    Read memory{};void* context{};std::uintptr_t address{};bool (*ownsThread)(){};
    static constexpr std::size_t bytes=0xad;
    static unsigned factor(unsigned native) noexcept {
        constexpr unsigned table[]{0,1,2,3,4,5,9,10,7,8,6,19,19,11,12};
        if(native==19)return 0;
        for(unsigned i=0;i<std::size(table);++i)if(table[i]==native)return i+1;
        return 0;
    }
    bool read(HudBlendState& out) const noexcept {
        struct Errno {int saved{errno};~Errno(){errno=saved;}} preserve;
        try{
            if(!memory||!ownsThread||!address||address%4||address>UINTPTR_MAX-bytes||!ownsThread())return false;
            std::array<unsigned char,bytes> raw{};
            if(!memory(context,address,raw.data(),raw.size()))return false;
            const auto word=[&](unsigned offset){std::uint32_t value{};std::memcpy(&value,raw.data()+offset,4);return value;};
            HudBlendState state{factor(word(4)),factor(word(8)),word(20),factor(word(12)),factor(word(16)),word(24),word(44),raw[172]!=0};
            if(!state.source||!state.destination||!state.alphaSource||!state.alphaDestination||
               state.operation<1||state.operation>5||state.alphaOperation<1||state.alphaOperation>5||
               state.colorMask>15||raw[172]>1)return false;
            out=state;return true;
        }catch(...){return false;}
    }
    static bool read(void* self,HudBlendState& out) noexcept {return self&&static_cast<HudBlendReader*>(self)->read(out);}
};
}
