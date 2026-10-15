#pragma once
#include "ControllerInput.h"
#include <atomic>
#include <sstream>

namespace rdrvr::runtime {
// Opt-in local bench command: a single A pulse in a freshly observed native
// menu. Never synthesize tracking, focus, gameplay movement or held buttons.
class BenchMenuInput {
    std::atomic<std::uint64_t> pending_{},requested_{},pulses_{},cancelled_{};
    std::atomic<bool> busy_{};
    std::uint64_t start_{};bool pressed_{};
public:
    bool request(std::uint64_t now){
        if(!now||busy_.load())return false;
        std::uint64_t empty{};
        if(!pending_.compare_exchange_strong(empty,now))return false;
        ++requested_;return true;
    }
    bool apply(ControllerSample& sample,bool nativeMenu,std::uint64_t now){
        const auto pending=pending_.exchange(0);
        if(pending){
            if(start_||now<pending||now-pending>5'000'000'000ull||!nativeMenu||!sample.focused){++cancelled_;}
            else{start_=now;pressed_=false;busy_=true;}
        }
        if(!start_)return false;
        bool physical=sample.buttons!=0;
        for(float x:sample.sticks)physical=physical||std::abs(x)>.15f;
        for(const auto& axes:{sample.triggers,sample.grips})for(float x:axes)physical=physical||x>.15f;
        if(!nativeMenu||!sample.focused||physical||now<start_||now-start_>=800'000'000ull){
            if(!pressed_)++cancelled_;start_=0;busy_=false;return false;
        }
        const auto elapsed=now-start_;
        sample.active=true;sample.buttons=0;sample.sticks={};sample.triggers={};sample.grips={};
        if(elapsed>=300'000'000ull&&elapsed<500'000'000ull){sample.buttons=PadA;if(!pressed_){pressed_=true;++pulses_;}}
        return true;
    }
    std::string report()const{
        std::ostringstream o;o<<"{\"requested\":"<<requested_<<",\"pulses\":"<<pulses_<<",\"cancelled\":"<<cancelled_<<",\"busy\":"<<busy_<<'}';return o.str();
    }
};
inline BenchMenuInput benchMenuInput;
}
