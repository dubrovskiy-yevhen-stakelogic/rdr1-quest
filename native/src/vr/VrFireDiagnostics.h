#pragma once
// Bounded, event-only diagnostics. Never changes input or game state.
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace rdrvr::vr::firediag {
struct Event {
    std::uint64_t sequence{},ns{},attemptNs{},weapon{};
    const char* stage{"ROUTE"};const char* reason{""};const char* hint{""};
    std::array<char,48> name{};
    int hand{-1},heldHand{-1},slot{-1},kind{-1},generation{-1};
    int preset{-1},hammer{-1},reload{-1},armed{-1},menu{-1},aimBits{-1},selectState{-1},forceAim{-1};
    float trigger{-1},grip{-1},sentRt{-1},sentLt{-1},clip{-1},pool{-1};
    int ammoKnown{},nativeReloading{-1};
};
inline constexpr std::size_t capacity=64;
inline std::mutex mutex;
inline std::array<Event,capacity> events{};
inline std::uint64_t count{};
inline void record(Event e) noexcept {
    {std::lock_guard lock(mutex);e.sequence=++count;events[(count-1)%capacity]=e;}
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO,"RdrVrFire",
        "event=%llu attempt=%llu stage=%s reason=%s hint=%s weapon=%s ptr=%llx hand=%d held=%d slot=%d gen=%d "
        "trigger=%.3f grip=%.3f rt=%.3f lt=%.3f armed=%d menu=%d preset=%d hammer=%d reload=%d "
        "aimBits=%d forceAim=%d select=%d ammoKnown=%d clip=%.1f pool=%.1f nativeReload=%d",
        (unsigned long long)e.sequence,(unsigned long long)e.attemptNs,e.stage,e.reason,e.hint,e.name.data(),
        (unsigned long long)e.weapon,e.hand,e.heldHand,e.slot,e.generation,double(e.trigger),double(e.grip),
        double(e.sentRt),double(e.sentLt),e.armed,e.menu,e.preset,e.hammer,e.reload,e.aimBits,e.forceAim,
        e.selectState,e.ammoKnown,double(e.clip),double(e.pool),e.nativeReloading);
#endif
}
inline void quoted(std::string& out,const char* s) {
    out+='"';for(;*s;++s){const auto c=static_cast<unsigned char>(*s);
        if(c=='"'||c=='\\'){out+='\\';out+=char(c);}
        else if(c<32||c>=127){char b[7];std::snprintf(b,sizeof b,"\\u%04x",unsigned(c));out+=b;}
        else out+=char(c);}
    out+='"';
}
inline double number(float v) noexcept {return std::isfinite(v)?double(v):-1.0;}
inline std::string report() {
    std::array<Event,capacity> copy;std::uint64_t total{};
    {std::lock_guard lock(mutex);copy=events;total=count;}
    std::string out="{\"capacity\":64,\"total\":"+std::to_string(total)+",\"events\":[";
    const auto begin=total>capacity?total-capacity:0;
    for(auto i=begin;i<total;++i){const auto& e=copy[i%capacity];if(i!=begin)out+=',';
        char b[1600];std::snprintf(b,sizeof b,
            "{\"sequence\":%llu,\"ns\":%llu,\"attemptNs\":%llu,\"weapon\":\"%llx\",\"hand\":%d,\"heldHand\":%d,"
            "\"slot\":%d,\"kind\":%d,\"generation\":%d,\"preset\":%d,\"hammer\":%d,\"reload\":%d,\"armed\":%d,\"menu\":%d,"
            "\"trigger\":%.3f,\"grip\":%.3f,\"sentRt\":%.3f,\"sentLt\":%.3f,\"aimBits\":%d,\"selectState\":%d,\"forceAim\":%d,"
            "\"ammoKnown\":%d,\"clip\":%.1f,\"pool\":%.1f,\"nativeReloading\":%d,\"stage\":",
            (unsigned long long)e.sequence,(unsigned long long)e.ns,(unsigned long long)e.attemptNs,(unsigned long long)e.weapon,
            e.hand,e.heldHand,e.slot,e.kind,e.generation,e.preset,e.hammer,e.reload,e.armed,e.menu,
            number(e.trigger),number(e.grip),number(e.sentRt),number(e.sentLt),e.aimBits,e.selectState,e.forceAim,
            e.ammoKnown,number(e.clip),number(e.pool),e.nativeReloading);
        out+=b;quoted(out,e.stage);out+=",\"reason\":";quoted(out,e.reason);out+=",\"hint\":";quoted(out,e.hint);
        out+=",\"name\":";quoted(out,e.name.data());out+='}';}
    out+="]}";return out;
}

// Caller owns synchronization. Keep rapid presses until matched or expired; match only the same weapon.
template<class Shot,std::size_t N=16> struct Pending {
    std::array<Shot,N> slots{};
    template<class Expired> void expire(std::uint64_t now,std::uint64_t window,Expired expired) {
        for(auto& s:slots)if(s.open&&now>=s.ns&&now-s.ns>window){s.open=false;expired(s);}
    }
    template<class Overflow> void add(const Shot& s,Overflow overflow) {
        auto* target=&slots[0];
        for(auto& slot:slots){if(!slot.open){target=&slot;break;}if(slot.ns<target->ns)target=&slot;}
        if(target->open)overflow(*target);*target=s;
    }
    Shot* match(std::uintptr_t weapon,std::uint64_t now,std::uint64_t window) noexcept {
        if(!weapon)return nullptr;
        Shot* oldest=nullptr;
        for(auto& s:slots)if(s.open&&s.weapon==weapon&&now>=s.ns&&now-s.ns<=window&&(!oldest||s.ns<oldest->ns))oldest=&s;
        if(oldest)oldest->open=false;return oldest;
    }
};
} // namespace rdrvr::vr::firediag
