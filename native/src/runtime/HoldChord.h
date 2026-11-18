#pragma once
#include <array>
#include <atomic>
#include <cstdint>
namespace rdrvr::runtime {
struct ChordResult {bool fired{},consume{};};
// A chord is armed only after a full release. Partial releases interrupt the
// hold, but keep its buttons consumed until both are up. No repeat after focus
// loss or a reference-space reset while the physical buttons are still held.
class HoldChord {
    std::uint64_t start_{};bool ready_{},fired_{},consumed_{};
public:
    void invalidate(){start_=0;ready_=fired_=consumed_=false;}
    ChordResult sample(bool first,bool second,std::uint64_t now,std::uint64_t duration){
        bool edge=false;
        if(!now||!duration){invalidate();return {};}
        if(first&&second){
            // Only an armed chord owns the squeeze. An unarmed one (re-arms after a full release) passes the grips untouched:
            // zeroing grips it can never fire on reads as an opened hand downstream (a hold that ends with both grips closed).
            if(ready_)consumed_=true;
            if(ready_&&!start_)start_=now;
            if(start_&&now<start_){invalidate();consumed_=true;}
            if(ready_&&!fired_&&start_&&now-start_>=duration){edge=true;fired_=true;}
        }else if(!first&&!second){invalidate();ready_=true;}
        else start_=0;
        return {edge,consumed_};
    }
};
// The manual-theater "both grips held 2 s" chord. busy = the VR holsters own a grip (a weapon held, a foregrip
// support, a hand over a live socket): the chord is disarmed (re-arms only after a full release) and the grips pass
// untouched - consuming them would read as a grip release (HOLD put-away, support dropped) before the 2 s are up.
// Diagnostics of the chord (XR thread writes, any thread reads): samples taken while busy, samples whose grips it zeroed, its fires;
// consuming = the last sample zeroed the grips.
// unarmedPassed = samples with both grips held while the chord was not armed (passed untouched).
struct TheaterChordStats {std::atomic<std::uint64_t> busy{},consumed{},fired{},unarmedPassed{};std::atomic<bool> consuming{};};
inline TheaterChordStats theaterChordStats{};
// owned (optional): bit h = grip h was squeezed (> 0.30) and the chord zeroed it in this sample - the squeeze belongs to the chord,
// the holsters never read that zero as an opened hand.
inline bool theaterGripChord(HoldChord& chord,std::array<bool,2>& held,std::array<float,2>& grips,std::uint64_t now,bool busy,
                             std::uint64_t duration=2'000'000'000,std::uint8_t* owned=nullptr){
    auto& st=theaterChordStats;
    if(owned)*owned=0;
    if(busy){chord.invalidate();held={};st.busy.fetch_add(1,std::memory_order_relaxed);st.consuming.store(false,std::memory_order_relaxed);return false;}
    for(unsigned i=0;i<2;++i)held[i]=grips[i]>=(held[i]?0.25f:0.85f);
    const auto result=chord.sample(held[0],held[1],now,duration);
    if(held[0]&&held[1]&&!result.consume)st.unarmedPassed.fetch_add(1,std::memory_order_relaxed);
    if(result.consume){
        if(owned)*owned=std::uint8_t((grips[0]>0.30f?1u:0u)|(grips[1]>0.30f?2u:0u));
        grips={};st.consumed.fetch_add(1,std::memory_order_relaxed);}
    st.consuming.store(result.consume,std::memory_order_relaxed);
    if(result.fired)st.fired.fetch_add(1,std::memory_order_relaxed);
    return result.fired;
}
}
