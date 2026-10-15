#pragma once
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>

namespace rdrvr::engine {
struct WaterWetObjects {
    std::uintptr_t puddles{},wind{},vertPush{},vertPushGuard{},useVertPush{};
    explicit operator bool()const{return puddles&&wind&&vertPush&&vertPushGuard&&useVertPush;}
    bool any()const{return puddles||wind||vertPush||vertPushGuard||useVertPush;}
};
struct WaterReplayStateApi {
    using Read=bool(*)(void*,std::uintptr_t,void*,std::size_t);
    using Write=bool(*)(void*,std::uintptr_t,const void*,std::size_t);
    Read read{};Write write{};void* user{};bool (*ownsThread)(){};
    // Slots contain renderer, request-array base and rmPtfxManager pointers.
    // The last is NOT the CPtFxManager facade whose SetDepthBuffer ignores this.
    std::uintptr_t rendererSlot{},requestsSlot{},particlesSlot{},fastGlobals{},commandBuffers{};
    WaterWetObjects wet{};
    explicit operator bool()const{return read&&write&&ownsThread&&rendererSlot&&requestsSlot&&particlesSlot&&fastGlobals&&commandBuffers;}
};
// Bounded CPU state touched by the pinned Pre/PostWater and terrain-depth
// callbacks. This complements (does not replace) texture/numeric shader journals,
// query suppression and private target routing. No production scope is wired yet.
class WaterReplayState {
    struct Region {
        std::uintptr_t address{};std::size_t size{};
        std::array<unsigned char,120> before{};
        std::array<bool,120> mutableByte{};
    };
    struct Identity {
        std::uintptr_t renderer{},requests{},particles{};
        bool operator==(const Identity&)const=default;
    } identity_{};
    WaterReplayStateApi api_;std::array<Region,17> regions_{};unsigned count_{};
    const std::thread::id owner_{std::this_thread::get_id()};
    bool active_{},failed_{};
    inline static thread_local WaterReplayState* current_{};
    struct Errno {int saved{errno};~Errno(){errno=saved;}};
    static bool range(std::uintptr_t base,std::size_t offset,std::size_t size){
        return base&&offset<=std::numeric_limits<std::uintptr_t>::max()-base&&
            size<=std::numeric_limits<std::uintptr_t>::max()-(base+offset);
    }
    bool owner()const{return api_&&owner_==std::this_thread::get_id()&&api_.ownsThread();}
    bool identity(Identity& out)const{
        return api_.read(api_.user,api_.rendererSlot,&out.renderer,8)&&out.renderer&&
            api_.read(api_.user,api_.requestsSlot,&out.requests,8)&&out.requests&&
            api_.read(api_.user,api_.particlesSlot,&out.particles,8)&&out.particles;
    }
    bool add(std::uintptr_t base,std::size_t offset,std::size_t size,std::size_t mutableOffset=0,std::size_t mutableSize=0){
        if(count_==regions_.size()||size>120||!range(base,offset,size)||mutableOffset>size||mutableSize>size-mutableOffset)return false;
        auto& r=regions_[count_++];r={};r.address=base+offset;r.size=size;
        for(std::size_t i=mutableOffset;i<mutableOffset+mutableSize;++i)r.mutableByte[i]=true;
        return api_.read(api_.user,r.address,r.before.data(),size);
    }
    bool validate(bool unchanged)const{
        Identity now{};if(!identity(now)||now!=identity_)return false;
        std::array<unsigned char,120> bytes{};
        for(unsigned i=0;i<count_;++i){const auto& r=regions_[i];
            if(!api_.read(api_.user,r.address,bytes.data(),r.size))return false;
            for(std::size_t j=0;j<r.size;++j)if((unchanged||!r.mutableByte[j])&&bytes[j]!=r.before[j])return false;
        }
        // All identities are rechecked after the complete borrowed snapshot.
        Identity after{};return identity(after)&&after==identity_;
    }
public:
    explicit WaterReplayState(WaterReplayStateApi api):api_(api){}
    ~WaterReplayState(){restore();}
    WaterReplayState(const WaterReplayState&)=delete;WaterReplayState& operator=(const WaterReplayState&)=delete;
    bool capture(void* renderer,void* mainRequest)noexcept{
        Errno preserve;
        try{
            if(!owner()||active_||failed_)return false;
            if(current_){current_->failed_=true;failed_=true;return false;}
            if(!identity(identity_)||identity_.renderer!=reinterpret_cast<std::uintptr_t>(renderer)||
               !range(identity_.requests,13*0x730,0x730)||
               identity_.requests+13*0x730!=reinterpret_cast<std::uintptr_t>(mainRequest))return false;
            count_=0;
            // Immutable target/shader identity, renderer scratch, native query
            // normalization + optional transparency update, command-buffer flag.
            if(!add(identity_.renderer,0x550,16)||!add(identity_.renderer,0x628,8,0,8)||
               !add(identity_.renderer,0x660,0x38)||
               !add(api_.fastGlobals,0,120)||!add(api_.commandBuffers,0,1,0,1)||
               !add(identity_.particles,0x930,8,0,8))return false;
            auto& fast=regions_[3];
            auto& scratch=regions_[2];
            for(auto span: {std::array<unsigned,2>{0x18,6},{0x20,4},{0x28,16}})
                for(unsigned j=0;j<span[1];++j)scratch.mutableByte[span[0]+j]=true;
            if(fast.before[0x3c]!=1||regions_[4].before[0]>1)return false; // never lazy Setup in an eye
            for(unsigned i: {0x54u,0x64u})for(unsigned j=0;j<4;++j)fast.mutableByte[i+j]=true;
            const auto request=reinterpret_cast<std::uintptr_t>(mainRequest);std::uint32_t index{};
            if(!api_.read(api_.user,request+0xcc,&index,4)||index!=13||!add(request,0xcc,4))return false;
            // datCallback is 36 bytes; four-byte gaps are deliberately excluded.
            for(unsigned offset:{0x4b0u,0x4d8u,0x5f0u,0x618u})if(!add(request,offset,36,0,36))return false;
            if(api_.wet.any()){
                if(!api_.wet)return false;
                const auto first=count_;
                // Static ELF objects; mutable lanes are caches uploaded by
                // VertPushSettings::SetGlobals and cleared by TreeResetWind.
                if(!add(api_.wet.puddles,0,96)||!add(api_.wet.wind,0,80,0,48)||
                   !add(api_.wet.vertPushGuard,0,8)||!add(api_.wet.vertPush,0,112)||
                   !add(api_.wet.vertPush,112,112,0,64)||!add(api_.wet.useVertPush,0,1,0,1))return false;
                auto& tail=regions_[first+4];
                for(unsigned i=96;i<112;++i)tail.mutableByte[i]=true;
                // Already constructed AND shader IDs initialized. Capturing
                // defaults and allowing lazy lookup would leave the shader
                // global table changed despite restoring these CPU objects.
                if(regions_[first].before[0]!=1||regions_[first+1].before[0x40]!=0||
                   !(regions_[first+2].before[0]&1)||tail.before[0xb0-112]!=1||
                   regions_[first+5].before[0]>1)return false;
            }
            // Reject aliases before any native body is allowed to run.
            for(unsigned i=0;i<count_;++i)for(unsigned j=0;j<i;++j)
                if(regions_[i].address<regions_[j].address+regions_[j].size&&
                   regions_[j].address<regions_[i].address+regions_[i].size)return false;
            if(!validate(true))return false;
            current_=this;active_=true;return true;
        }catch(...){return false;}
    }
    bool restore()noexcept{
        Errno preserve;if(!active_)return !failed_;
        try{
            // Do not write through changed singleton identities or a reloaded
            // shader table. Validate the entire set before restoring any byte.
            if(!owner())return false;
            const bool valid=current_==this&&validate(false);
            active_=false;if(current_==this)current_=nullptr;
            if(!valid){failed_=true;return false;}
            for(unsigned n=count_;n>0;--n){const auto& r=regions_[n-1];
                for(std::size_t begin=0;begin<r.size;){
                    if(!r.mutableByte[begin]){++begin;continue;}
                    auto end=begin+1;while(end<r.size&&r.mutableByte[end])++end;
                    try{if(!api_.write(api_.user,r.address+begin,r.before.data()+begin,end-begin))failed_=true;}
                    catch(...){failed_=true;}
                    begin=end;
                }
            }
            if(!validate(true))failed_=true;
            return !failed_;
        }catch(...){active_=false;if(current_==this)current_=nullptr;failed_=true;return false;}
    }
    bool good()const{return !failed_;}
    bool active()const{return active_;}
};
}
