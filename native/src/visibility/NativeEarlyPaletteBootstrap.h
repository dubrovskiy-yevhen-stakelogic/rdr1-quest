#pragma once
#include "NativeEarlyImageProof.h"
#include "NativeCachedPaletteOwner.h"
#include "NativeCachedPaletteProof.h"
#include <atomic>
namespace rdrvr::experimental {
// Cold process-image proof only. The actual current-bank membership is copied
// anew after MAIN joins, using NativeCachedPaletteOwner under its native Crit.
class NativeEarlyPaletteBootstrap {
public:
    bool publish(std::uintptr_t base,NativeEarlyImageProof::Api checks)noexcept {
        if(ready_.load(std::memory_order_acquire)||!checks.read||!checks.range)return false;
        using namespace rdr95::cached_palette_generated;
        if(!cachedPaletteCodeMatches(base,[&](std::uintptr_t address,void* bytes,std::size_t n){
            const auto offset=address-base;
            const bool got=offset==IndexGotOffset||offset==BanksGotOffset||offset==UsedGotOffset||offset==CountGotOffset||offset==PoolLockGotOffset;
            return checks.range(checks.context,address,n,got?4:5)&&checks.read(checks.context,address,bytes,n);
        }))return false;
        if(!cachedPaletteGlobalsMatch(base,[&](std::uintptr_t address,void* bytes,std::size_t n){
            return checks.range(checks.context,address,n,4)&&checks.read(checks.context,address,bytes,n);
        }))return false;
        for(const auto& entry:std::array<std::array<std::uintptr_t,2>,5>{{
            {{IndexOffset,4}},{{BanksOffset,24}},{{UsedOffset,4}},{{CountOffset,4}},{{PoolLockOffset,64}}}})
            if(!checks.range(checks.context,base+entry[0],entry[1],6))return false;
        api_.lock=reinterpret_cast<decltype(api_.lock)>(base+LockOffset);
        api_.unlock=reinterpret_cast<decltype(api_.unlock)>(base+UnlockOffset);
        api_.indexSlot=base+IndexOffset;api_.banksSlot=base+BanksOffset;api_.usedSlot=base+UsedOffset;
        api_.countSlot=base+CountOffset;api_.poolLock=base+PoolLockOffset;api_.codeProof=true;
        ready_.store(true,std::memory_order_release);return true;
    }
    bool ready()const noexcept{return ready_.load(std::memory_order_acquire);}
    rdr95::NativeCachedPaletteOwner::Api configuration()const noexcept{return ready()?api_:rdr95::NativeCachedPaletteOwner::Api{};}
private:rdr95::NativeCachedPaletteOwner::Api api_{};std::atomic<bool> ready_{};
};
inline NativeEarlyPaletteBootstrap nativeEarlyPaletteBootstrap;
}
