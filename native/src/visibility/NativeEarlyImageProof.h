#pragma once
#include "NativeAdmissionProduction.h"
#include "NativeJoinedListProof.h"
#include "engine/StaticOcclusionCertificate.h"
#include <vector>
#include <algorithm>
#include <cstring>

namespace rdrvr::experimental {
// Cold verification of the exact mapped retail functions consumed by the
// production admission factory. Performs no native calls or writes. The
// resulting callback accepts only the exact generated pin table, not arbitrary
// addresses that happen to belong to a readable/executable mapping.
class NativeEarlyImageProof {
public:
    using Address=std::uintptr_t;
    struct Api {
        void* context{};
        bool(*read)(void*,Address,void*,std::size_t)noexcept{};
        bool(*range)(void*,Address,std::size_t,unsigned elfFlags)noexcept{};
    };
    bool bind(Address image,Api api)noexcept {
        ready_=false;base_=0;api_={};
        if(!image||!api.read||!api.range)return false;
        try {
            std::size_t largest{};for(const auto& pin:rdr95::retail::CodePins)largest=std::max(largest,std::size_t(pin.size));
            for(const auto& pin:rdr95::joined_list::Pins)largest=std::max(largest,std::size_t(pin.size));
            if(!largest||largest>1024*1024)return false;
            std::vector<std::uint8_t> bytes(largest);
            auto verify=[&](const auto& pin){
                if(!fits(image,pin.offset,pin.size)||!api.range(api.context,image+pin.offset,pin.size,5)||
                   !api.read(api.context,image+pin.offset,bytes.data(),pin.size))return false;
                const auto digest=engine::StaticOcclusionCertificate::fingerprint({bytes.data(),pin.size});
                return sameDigest(digest,pin.sha256);
            };
            for(const auto& pin:rdr95::retail::CodePins)if(!verify(pin))return false;
            for(const auto& pin:rdr95::joined_list::Pins)if(!verify(pin))return false;
            for(const auto& range:rdr95::retail::BindingRanges)
                if(!fits(image,range.offset,range.size)||!api.range(api.context,image+range.offset,range.size,range.flags))return false;
            for(const auto& pin:rdr95::retail::GlobalGotPins)
                if(!fits(image,pin.slot,sizeof(std::uint64_t))||!api.range(api.context,image+pin.slot,sizeof(std::uint64_t),4))return false;
        }catch(...){return false;}
        base_=image;api_=api;ready_=true;return true;
    }
    void disarm()noexcept{ready_=false;}
    bool ready(Address image)const noexcept{return ready_&&base_==image;}
    rdr95::NativeAdmissionProduction::ImageProof callbacks()noexcept{return {this,code,range};}
private:
    Api api_{};Address base_{};bool ready_{};
    static bool fits(Address image,Address offset,std::size_t bytes)noexcept{return offset<=UINTPTR_MAX-image&&bytes<=UINTPTR_MAX-image-offset;}
    static bool sameDigest(const engine::StaticOcclusionCertificate::Digest& digest,const char* expected)noexcept {
        if(!expected||std::strlen(expected)!=64)return false;constexpr char hex[]="0123456789abcdef";
        for(unsigned i=0;i<digest.size();++i)if(expected[i*2]!=hex[digest[i]>>4]||expected[i*2+1]!=hex[digest[i]&15])return false;
        return true;
    }
    static bool code(void* context,std::uint64_t address,unsigned size,const char* sha)noexcept {
        auto& self=*static_cast<NativeEarlyImageProof*>(context);if(!self.ready_||!sha||address<self.base_)return false;
        for(const auto& pin:rdr95::retail::CodePins)
            if(address-self.base_==pin.offset&&size==pin.size&&std::strcmp(sha,pin.sha256)==0)return true;
        for(const auto& pin:rdr95::joined_list::Pins)
            if(address-self.base_==pin.offset&&size==pin.size&&std::strcmp(sha,pin.sha256)==0)return true;
        return false;
    }
    static bool range(void* context,std::uint64_t address,unsigned size,unsigned flags)noexcept {
        auto& self=*static_cast<NativeEarlyImageProof*>(context);if(!self.ready_||address<self.base_)return false;
        for(const auto& item:rdr95::retail::BindingRanges)
            if(address-self.base_==item.offset&&size==item.size&&flags==item.flags)
                return self.api_.range(self.api_.context,address,size,flags);
        for(const auto& pin:rdr95::retail::GlobalGotPins)
            if(address-self.base_==pin.slot&&size==sizeof(std::uint64_t)&&flags==4)
                return self.api_.range(self.api_.context,address,size,flags);
        return false;
    }
};
}
