#pragma once
#include "ControllerInput.h"
#include <atomic>

namespace rdrvr::runtime {
struct HapticSample {
    std::array<float,2> amplitude{};
    std::uint64_t stamp{},inputEpoch{},continuity{};
};
// Native game and XR threads exchange values, never native pad pointers.
class HapticMailbox {
    mutable std::mutex mutex_;HapticSample latest_{};bool available_{};
    std::atomic<std::uint64_t> published_{},rejected_{};
public:
    static constexpr std::uint64_t maxAgeNs=75'000'000;
    bool publish(const HapticSample& sample){
        std::lock_guard guard(mutex_);
        bool valid=sample.stamp&&sample.inputEpoch&&sample.continuity;
        for(float a:sample.amplitude)valid=valid&&std::isfinite(a)&&a>=0&&a<=1;
        if(!valid|| (available_&&sample.stamp<latest_.stamp)){available_=false;++rejected_;return false;}
        latest_=sample;available_=true;++published_;return true;
    }
    void invalidate(){std::lock_guard guard(mutex_);available_=false;}
    bool snapshot(std::uint64_t now,const ControllerSample& input,HapticSample& out)const{
        std::lock_guard guard(mutex_);
        const auto epoch=input.inputEpoch?input.inputEpoch:input.epoch;
        if(!available_||!validController(input)||now<input.publishedNs||now-input.publishedNs>ControllerMailbox::maxAgeNs||
           now<latest_.stamp||now-latest_.stamp>maxAgeNs||latest_.inputEpoch!=epoch||latest_.continuity!=input.continuity)return false;
        out=latest_;return true;
    }
    std::uint64_t published()const{return published_.load();}
    std::uint64_t rejected()const{return rejected_.load();}
};
struct HapticApi {
    void* context{};
    bool (*apply)(void*,unsigned,float,std::uint64_t){};
    bool (*stop)(void*,unsigned){};
};
// XR-thread only. Short finite pulses bound output even if the game stalls.
class HapticOutput {
    HapticApi api_{};std::array<bool,2> playing_{};
    std::uint64_t applied_{},stopped_{},failed_{};
public:
    static constexpr std::uint64_t pulseNs=30'000'000;
    void configure(HapticApi api){api_=api;playing_={};}
    void update(const HapticSample* sample)noexcept{
        for(unsigned hand=0;hand<2;++hand){
            const float amplitude=sample?sample->amplitude[hand]:0;
            try{
                if(std::isfinite(amplitude)&&amplitude>0&&amplitude<=1&&api_.apply){
                    if(api_.apply(api_.context,hand,amplitude,pulseNs)){playing_[hand]=true;++applied_;}
                    else ++failed_;
                }else if(playing_[hand]&&api_.stop){
                    if(api_.stop(api_.context,hand)){playing_[hand]=false;++stopped_;}else ++failed_;
                }
            }catch(...){++failed_;}
        }
    }
    std::uint64_t applied()const{return applied_;}
    std::uint64_t stopped()const{return stopped_;}
    std::uint64_t failed()const{return failed_;}
};
}
