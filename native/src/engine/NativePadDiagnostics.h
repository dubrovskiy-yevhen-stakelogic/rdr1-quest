#pragma once
#include "runtime/ControllerInput.h"
#include <cstdio>
#include <mutex>
#include <string>
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace rdrvr::engine {
// Observe delivery AFTER the native base Update; ROUTED in the holster filter is
// earlier and cannot prove that the game received a trigger. No writes/calls to
// gameplay APIs. Offsets are from the pinned AndroidGamepad/Gamepad/ioPad ABI:
// raw buttons +350, held +3c, pressed +ed (LT=16, RT=17), connected +300.
class NativePadDiagnostics {
public:
    struct Event {
        std::uint64_t ns{};
        const char* status{"UNKNOWN"};
        float physicalLt{},physicalRt{},requestedRt{},submittedRt{};
        bool ready{},requestedBit{},submittedBit{},selected{},readable{};
        int rawLt{-1},rawRt{-1},heldLt{-1},heldRt{-1},pressedLt{-1},pressedRt{-1},connected{-1};
        std::uint64_t filterRejected{},controlsRejected{};
    };
private:
    mutable std::mutex mutex_;
    std::array<Event,64> ring_{};
    std::uint64_t count_{},pressNs_{};
    bool down_{};unsigned sample_{};
    const char* lastStatus_{"UNKNOWN"};
public:
    // Press, release, two held samples, and failures while held; bounded even if
    // a user leaves the trigger squeezed. Called only by the native input thread.
    bool wanted(const Event& e) noexcept {
        const bool down=e.physicalLt>.45f||e.physicalRt>.45f;
        bool take=down!=down_;
        if(down&&!down_){pressNs_=e.ns;sample_=0;}
        if(down&&e.ns>=pressNs_){
            const auto elapsed=e.ns-pressNs_;
            if((sample_==0&&elapsed>=100'000'000)||(sample_==1&&elapsed>=700'000'000)){++sample_;take=true;}
        }
        if(down&&e.status!=lastStatus_)take=true;
        down_=down;lastStatus_=e.status;return take;
    }
    void record(const Event& e) {
        {std::lock_guard lock(mutex_);ring_[count_++%ring_.size()]=e;}
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO,"RdrVrFire",
            "stage=NATIVE_PAD ns=%llu status=%s physical=%.3f/%.3f requestedRt=%.3f/%d submittedRt=%.3f/%d ready=%d selected=%d readable=%d raw=%d/%d held=%d/%d pressed=%d/%d connected=%d filterRejected=%llu controlsRejected=%llu",
            (unsigned long long)e.ns,e.status,double(e.physicalLt),double(e.physicalRt),double(e.requestedRt),int(e.requestedBit),
            double(e.submittedRt),int(e.submittedBit),int(e.ready),int(e.selected),int(e.readable),e.rawLt,e.rawRt,e.heldLt,e.heldRt,
            e.pressedLt,e.pressedRt,e.connected,(unsigned long long)e.filterRejected,(unsigned long long)e.controlsRejected);
#endif
    }
    std::string report() const {
        std::array<Event,64> copy;std::uint64_t total{};
        {std::lock_guard lock(mutex_);copy=ring_;total=count_;}
        std::string out="{\"total\":"+std::to_string(total)+",\"events\":[";
        const auto start=total>copy.size()?total-copy.size():0;
        for(auto i=start;i<total;++i){const auto& e=copy[i%copy.size()];if(i!=start)out+=',';
            char b[1000];std::snprintf(b,sizeof b,
                "{\"ns\":%llu,\"status\":\"%s\",\"physicalLt\":%.3f,\"physicalRt\":%.3f,\"requestedRt\":%.3f,\"submittedRt\":%.3f,"
                "\"ready\":%d,\"requestedBit\":%d,\"submittedBit\":%d,\"selected\":%d,\"readable\":%d,\"rawLt\":%d,\"rawRt\":%d,"
                "\"heldLt\":%d,\"heldRt\":%d,\"pressedLt\":%d,\"pressedRt\":%d,\"connected\":%d,\"filterRejected\":%llu,\"controlsRejected\":%llu}",
                (unsigned long long)e.ns,e.status,double(e.physicalLt),double(e.physicalRt),double(e.requestedRt),double(e.submittedRt),
                int(e.ready),int(e.requestedBit),int(e.submittedBit),int(e.selected),int(e.readable),e.rawLt,e.rawRt,e.heldLt,e.heldRt,
                e.pressedLt,e.pressedRt,e.connected,(unsigned long long)e.filterRejected,(unsigned long long)e.controlsRejected);out+=b;
        }
        return out+"]}";
    }
};
}
