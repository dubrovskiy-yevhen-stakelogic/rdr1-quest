#pragma once
#include "diagnostics/ThreadCpuScope.h"
#include "runtime/ResolutionTelemetry.h"
#include <atomic>
#include <cstring>
#include <cmath>

namespace rdrvr::engine {
// The exact RenderFrame -> UpdateDynamicResolutionScaling call owns this
// renderer. Disable its automatic scale controller, then let the original
// perform all reset/resize/AA bookkeeping at its configured fixed cap (+5c8).
// This is process-local: no saved graphics options or display size are written.
class NativeResolutionPolicy {
public:
    using Original=void(*)(void*);
    using Reader=bool(*)(void*,std::uintptr_t,void*,std::size_t);
    using Writer=bool(*)(std::uintptr_t,std::uint8_t);
private:
    Original original_{};Reader read_{};Writer write_{};
    std::atomic<std::uint64_t> calls_{},disabled_{},declined_{};
    diagnostics::DurationWindow wall_,cpu_;
    static inline thread_local bool active_{};
public:
    void configure(Original original,Reader read,Writer write){original_=original;read_=read;write_=write;}
    void update(void* renderer,bool exactCaller){
        const int incoming=errno;
        if(!exactCaller||active_){original_(renderer);return;}
        struct Active {Active(){active_=true;}~Active(){active_=false;}} active;
        ++calls_;
        diagnostics::WallScope wall(wall_);diagnostics::ThreadCpuScope cpu(cpu_);
        const auto p=reinterpret_cast<std::uintptr_t>(renderer);
        std::array<std::byte,0x6c> values{};
        bool valid=false;
        try{
        valid=p&&!(p%8)&&p<=UINTPTR_MAX-0x5cc&&read_&&write_&&read_(nullptr,p+0x560,values.data(),values.size());
        if(valid){
            float scale{},cap{};std::memcpy(&scale,values.data()+4,4);std::memcpy(&cap,values.data()+0x68,4);
            const auto enabled=std::to_integer<unsigned>(values[0x24]);
            // Accept the retail normalized range; never increase a configured
            // cap, invent an extent or bypass the native resizing function.
            valid=enabled<=1&&std::isfinite(scale)&&scale>0&&scale<=1&&std::isfinite(cap)&&cap>0&&cap<=1;
            if(valid&&enabled){valid=write_(p+0x584,0);if(valid)++disabled_;}
        }
        }catch(...){valid=false;}
        if(!valid)++declined_;
        errno=incoming;original_(renderer);
        const int result=errno;float scale{};
        try{if(valid&&read_(nullptr,p+0x564,&scale,sizeof(scale))&&std::isfinite(scale)&&scale>0&&scale<=1)
            runtime::resolutionTelemetry.nativeScale.store(scale,std::memory_order_relaxed);
        }catch(...){}
        errno=result;
    }
    std::string report()const{
        std::ostringstream o;o<<"{\"policy\":\"native-fixed-cap\",\"calls\":"<<calls_.load()
            <<",\"disabled\":"<<disabled_.load()<<",\"declined\":"<<declined_.load()
            <<",\"wall\":"<<wall_.snapshot().json()<<",\"threadCpu\":"<<cpu_.snapshot().json()<<'}';return o.str();
    }
};
}
