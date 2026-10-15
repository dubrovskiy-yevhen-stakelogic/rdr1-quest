#pragma once
#include <cmath>
#include <cstdint>
#include <numbers>
namespace rdrvr::engine {
struct SnapTurnIdentity {
    std::uint64_t inputEpoch{},continuity{},reference{},revision{};
    std::uint32_t actorGuid{};
    bool operator==(const SnapTurnIdentity&)const=default;
    explicit operator bool()const{return inputEpoch&&continuity&&reference&&revision&&actorGuid;}
};
// Vice City Quest's 0.70 engage / 0.35 release and signed 30 degree step.
// Entry, identity changes and interrupted updates additionally require neutral.
class SnapTurnLatch {
    SnapTurnIdentity identity_{};std::uint64_t stamp_{};bool armed_{};
public:
    float sample(bool eligible,const SnapTurnIdentity& id,float x,std::uint64_t now){
        if(!eligible||!id||!std::isfinite(x)||std::abs(x)>1.001f||!now){armed_=false;identity_={};stamp_=0;return 0;}
        if(id!=identity_||!stamp_||now<stamp_||now-stamp_>100'000'000)armed_=false;
        identity_=id;stamp_=now;
        if(std::abs(x)<=.35f){armed_=true;return 0;}
        if(!armed_||std::abs(x)<.70f)return 0;
        armed_=false;return -std::copysign(std::numbers::pi_v<float>/6,x);
    }
};
}
