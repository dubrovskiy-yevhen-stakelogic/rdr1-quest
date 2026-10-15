#pragma once
#include <openxr/openxr.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <sstream>
namespace rdrvr::runtime {
// Owned by the XR thread. A successful request is not proof of a rate change.
class DisplayRefresh {
    XrSession session_{};
    PFN_xrEnumerateDisplayRefreshRatesFB enumerate_{};
    PFN_xrGetDisplayRefreshRateFB get_{};
    PFN_xrRequestDisplayRefreshRateFB request_{};
    bool supported_{},targetAvailable_{},listValid_{};
    std::array<float,32> rates_{};
    std::uint32_t count_{};
    float actual_{};
    unsigned attempts_{};
    std::uint64_t next_{};
    XrResult requestResult_{XR_ERROR_FUNCTION_UNSUPPORTED},getResult_{XR_ERROR_FUNCTION_UNSUPPORTED},enumerateResult_{XR_ERROR_FUNCTION_UNSUPPORTED};
    template<class T>static T load(XrInstance instance,const char* name){T fn{};return XR_SUCCEEDED(xrGetInstanceProcAddr(instance,name,reinterpret_cast<PFN_xrVoidFunction*>(&fn)))?fn:nullptr;}
    void enumerateRates(){
        targetAvailable_=listValid_=false;count_=0;rates_={};
        enumerateResult_=enumerate_(session_,0,&count_,nullptr);
        if(XR_FAILED(enumerateResult_)||!count_||count_>rates_.size())return;
        enumerateResult_=enumerate_(session_,static_cast<std::uint32_t>(rates_.size()),&count_,rates_.data());
        if(XR_FAILED(enumerateResult_)||!count_||count_>rates_.size())return;
        for(unsigned i=0;i<count_;++i)if(!std::isfinite(rates_[i])||rates_[i]<=0)return;
        listValid_=true;
        for(unsigned i=0;i<count_;++i)if(rates_[i]==target)targetAvailable_=true;
    }
public:
    static constexpr float target=72.f;
    void initialize(XrInstance instance,XrSession session,bool enabled){
        *this=DisplayRefresh{};
        session_=session;
        if(!enabled)return;
        enumerate_=load<PFN_xrEnumerateDisplayRefreshRatesFB>(instance,"xrEnumerateDisplayRefreshRatesFB");
        get_=load<PFN_xrGetDisplayRefreshRateFB>(instance,"xrGetDisplayRefreshRateFB");
        request_=load<PFN_xrRequestDisplayRefreshRateFB>(instance,"xrRequestDisplayRefreshRateFB");
        supported_=enumerate_&&get_&&request_;
    }
    void begin(){attempts_=0;next_=0;actual_=0;targetAvailable_=listValid_=false;count_=0;rates_={};requestResult_=XR_ERROR_FUNCTION_UNSUPPORTED;}
    void changed(const XrEventDataDisplayRefreshRateChangedFB& event){
        if(std::isfinite(event.toDisplayRefreshRate)&&event.toDisplayRefreshRate>0)actual_=event.toDisplayRefreshRate;
        // Confirm with get on the next pump. The bounded retry budget is not
        // reset by events, avoiding a request loop against runtime policy.
        next_=0;
    }
    void pump(std::uint64_t now){
        if(!supported_||now<next_)return;
        next_=now+1'000'000'000ull;
        float rate{};getResult_=get_(session_,&rate);
        actual_=XR_SUCCEEDED(getResult_)&&std::isfinite(rate)&&rate>0?rate:0;
        // Enumerate in the running session, not once before xrBeginSession.
        // Like the working Quest VC port, let the runtime validate a direct
        // request if Horizon supplies no usable list. Explicit exclusion of
        // 72 by a valid list is respected, and no path can exceed three calls.
        if(attempts_<3){
            enumerateRates();
            if((targetAvailable_||!listValid_)&&actual_!=target){++attempts_;requestResult_=request_(session_,target);}
        }
    }
    float actual()const{return actual_;}
    unsigned attempts()const{return attempts_;}
    std::string report()const{
        std::ostringstream o;o<<"{\"supported\":"<<supported_<<",\"targetHz\":72,\"targetAvailable\":"<<targetAvailable_
            <<",\"listValid\":"<<listValid_<<",\"enumerateResult\":"<<enumerateResult_<<",\"offeredCount\":"<<count_<<",\"offeredHz\":[";
        if(listValid_)for(unsigned i=0;i<count_;++i){if(i)o<<',';o<<rates_[i];}
        o<<"],\"directFallback\":"<<(!listValid_&&attempts_>0)<<",\"actualHz\":"<<actual_<<",\"attempts\":"<<attempts_<<",\"requestResult\":"<<requestResult_<<",\"getResult\":"<<getResult_<<'}';return o.str();
    }
};
}
