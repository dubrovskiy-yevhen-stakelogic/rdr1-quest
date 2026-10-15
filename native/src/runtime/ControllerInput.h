#pragma once
#include "contracts/Contracts.h"
#include <algorithm>
#include <cmath>
#include <mutex>

namespace rdrvr::runtime {
enum PadButton : std::uint32_t {
    PadA=1u<<0,PadB=1u<<1,PadX=1u<<2,PadY=1u<<3,PadMenu=1u<<4,
    PadLeftStick=1u<<5,PadRightStick=1u<<6,PadLeftGrip=1u<<7,PadRightGrip=1u<<8,
    PadLeftTrigger=1u<<9,PadRightTrigger=1u<<10,
    PadDpadUp=1u<<11 // PadState output only (Android key 19 -> d11 pad 10 = WHISTLE); never in a ControllerSample
};
struct ControllerSample {
    std::uint64_t sequence{},publishedNs{},epoch{},continuity{},referenceSpace{};
    std::uint32_t buttons{};
    std::array<float,4> sticks{}; // left X/Y, right X/Y; OpenXR positive Y = up
    std::array<float,2> triggers{},grips{};
    std::array<Pose,2> aim{},grip{};
    bool focused{},active{};
    // Separate visual epoch retirement (World <-> Optic) from focus/recenter
    // invalidation. Zero keeps the original epoch behavior for legacy producers.
    std::uint64_t inputEpoch{};
};
struct PadState {
    std::uint32_t buttons{};
    std::array<float,4> sticks{};
    std::array<float,2> triggers{};
    bool ready{};
};
inline bool validController(const ControllerSample& sample) {
    bool valid=sample.sequence&&sample.publishedNs&&sample.epoch&&sample.focused&&sample.active&&!(sample.buttons&~0x7fu);
    for(float x:sample.sticks)valid=valid&&std::isfinite(x)&&std::abs(x)<=1.01f;
    for(const auto& axes:{sample.triggers,sample.grips})for(float x:axes)valid=valid&&std::isfinite(x)&&x>=0&&x<=1.01f;
    return valid;
}
class ControllerMailbox {
    mutable std::mutex mutex_;
    ControllerSample latest_{};std::uint64_t sequence_{},continuity_{1};bool available_{};
public:
    static constexpr std::uint64_t maxAgeNs=150'000'000;
    bool publish(const ControllerSample& sample) {
        std::lock_guard guard(mutex_);
        const bool valid=sample.sequence>sequence_&&validController(sample);
        if(!valid){if(available_)++continuity_;available_=false;return false;}
        latest_=sample;sequence_=sample.sequence;available_=true;return true;
    }
    void invalidate(){std::lock_guard guard(mutex_);if(available_)++continuity_;available_=false;}
    bool snapshot(std::uint64_t now,ControllerSample& out)const {
        std::lock_guard guard(mutex_);
        if(!available_||now<latest_.publishedNs||now-latest_.publishedNs>maxAgeNs)return false;
        out=latest_;out.continuity=continuity_;return true;
    }
};
// Game-thread policy. Regaining focus cannot turn a held trigger into a shot.
// The game owns button edges; this class emits held state only, once per update.
class ControllerPolicy {
    std::uint64_t epoch_{},continuity_{};bool armed_{};
    std::array<bool,4> analogButtons_{};
public:
    void invalidate(){armed_=false;analogButtons_={};epoch_=continuity_=0;}
    PadState sample(const ControllerSample* input){
        PadState out{};
        if(!input||!validController(*input)){invalidate();return out;}
        const auto inputEpoch=input->inputEpoch?input->inputEpoch:input->epoch;
        if(epoch_!=inputEpoch||continuity_!=input->continuity){invalidate();epoch_=inputEpoch;continuity_=input->continuity;}
        if(!armed_){
            bool neutral=input->buttons==0;
            for(float x:input->sticks)neutral=neutral&&std::abs(x)<=0.15f;
            for(const auto& axes:{input->triggers,input->grips})for(float x:axes)neutral=neutral&&x<=0.15f;
            if(neutral)armed_=true;
            return out;
        }
        out.ready=true;out.buttons=input->buttons;
        for(unsigned i=0;i<4;++i)out.sticks[i]=std::clamp(input->sticks[i],-1.0f,1.0f);
        for(unsigned i=0;i<2;++i)out.triggers[i]=std::clamp(input->triggers[i],0.0f,1.0f);
        const float values[]{input->grips[0],input->grips[1],input->triggers[0],input->triggers[1]};
        const std::uint32_t bits[]{PadLeftGrip,PadRightGrip,PadLeftTrigger,PadRightTrigger};
        for(unsigned i=0;i<4;++i){analogButtons_[i]=values[i]>=(analogButtons_[i]?0.45f:0.55f);if(analogButtons_[i])out.buttons|=bits[i];}
        return out;
    }
};
}
