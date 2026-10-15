#pragma once
#include <array>
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
            consumed_=true;
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
inline bool theaterGripChord(HoldChord& chord,std::array<bool,2>& held,std::array<float,2>& grips,std::uint64_t now,bool busy,
                             std::uint64_t duration=2'000'000'000){
    if(busy){chord.invalidate();held={};return false;}
    for(unsigned i=0;i<2;++i)held[i]=grips[i]>=(held[i]?0.25f:0.85f);
    const auto result=chord.sample(held[0],held[1],now,duration);
    if(result.consume)grips={};
    return result.fired;
}
}
