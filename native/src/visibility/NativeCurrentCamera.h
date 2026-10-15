#pragma once
#include "NativeCurrentCameraCode.h"
#include "engine/NativeStaticOcclusionSource.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>

namespace rdrvr::experimental {
// Camera-only proof at the current DrawCommon entry. This does NOT
// prove selected shaders/material aliases, object bounds, side effects, query
// state, native ownership or pose age. Those are independent admission gates.
// Never reads the preceding draw's descriptor/ring or WorldSingle/MVP bytes.
class NativeCurrentCamera {
public:
    using Address=std::uintptr_t;
    using Camera=engine::NativeStaticOcclusionSource::Camera;
    struct Lease {
        std::uint64_t scene{},pose{},owner{},mutation{};
        bool valid()const noexcept{return scene&&pose&&owner&&mutation;}
        bool operator==(const Lease&)const=default;
    };
    struct Api {
        void* context{};
        bool(*read)(void*,Address,void*,std::size_t)noexcept{};
        Address engineBase{};
        void* ownerContext{};
        // Must enforce the current mutation-stable native owner borrow on THIS
        // thread. Repeated reads do not create a lifetime/mutation exclusion.
        bool(*ownerCurrent)(void*,const Lease&)noexcept{};
    };
    struct Call {Lease lease{};Address viewport{},matrixBuffer{};Camera expected{};};
    enum class Failure {None,Unqualified,Code,Owner,Read,Selector,Handle,Descriptor,Mapping,Buffer,Camera,Changed};
    struct Result {bool valid{};Failure failure{};unsigned reads{},bytes{};Address viewport{},matrixBuffer{},cpuData{};};
    class CodeProof {
        friend class NativeCurrentCamera;
        Address base_{};void* context_{};
        bool(*read_)(void*,Address,void*,std::size_t)noexcept{};
    public: explicit operator bool()const noexcept{return base_&&read_;}
    };
    inline static constexpr Address CurrentViewportRva=0x4e6e14d8,MatrixIdsRva=0x4e6e14f0,
        GlobalValuesRva=0x4e6dba80,GlobalBuffersRva=0x4e6d9178,CommandBufferRva=0x4e6d8be0;
    // Cold exact function-byte verification. Original code/PLT interposition
    // lifetime remains owned by the caller's pinned engine instance. No hooks.
    static bool prove(const Api& api,CodeProof& out)noexcept {
        Errno preserve;out={};
        if(!api.read||!api.engineBase||api.engineBase>UINTPTR_MAX-(GlobalValuesRva+0x4384))return false;
        std::array<unsigned char,128> bytes{};
        for(const auto& pin:current_camera_code::pins){
            if(pin.offset>UINTPTR_MAX-api.engineBase||pin.bytes.size()>UINTPTR_MAX-(api.engineBase+pin.offset))return false;
            for(std::size_t at=0;at<pin.bytes.size();at+=bytes.size()){
                const auto n=std::min(bytes.size(),pin.bytes.size()-at);
                if(!api.read(api.context,api.engineBase+pin.offset+at,bytes.data(),n)||
                   std::memcmp(bytes.data(),pin.bytes.data()+at,n))return false;
            }
        }
        // These native GOT entries are the actual selectors used by the pinned
        // SetCurrent/Regenerate/SetGlobalFloat paths, not guessed storage.
        for(const auto& pair:std::array<std::array<Address,2>,3>{{{0x2de0958,CurrentViewportRva},{0x2de09a0,CommandBufferRva},{0x2de0a18,GlobalBuffersRva}}}){
            Address address{};
            if(!api.read(api.context,api.engineBase+pair[0],&address,sizeof address)||address!=api.engineBase+pair[1])return false;
        }
        out.base_=api.engineBase;out.context_=api.context;out.read_=api.read;return true;
    }
    static bool capture(const Api& api,const CodeProof& proof,const Call& call,Result& out)noexcept {
        Errno preserve;out={};
        auto fail=[&](Failure f){out.valid=false;out.failure=f;out.viewport=out.matrixBuffer=out.cpuData=0;return false;};
        if(!api.read||!api.ownerCurrent||!call.lease.valid()||!fits(call.viewport,0x180)||!fits(call.matrixBuffer,0x20)||
           !finite(call.expected.relativeClip)||!finite(call.expected.inverseView))return fail(Failure::Unqualified);
        if(!proof||proof.base_!=api.engineBase||proof.context_!=api.context||proof.read_!=api.read)return fail(Failure::Code);
        if(!api.ownerCurrent(api.ownerContext,call.lease))return fail(Failure::Owner);
        // Fixed-size journal, no allocation. It detects unexpected publication
        // changes, but the caller's native owner remains mandatory.
        struct Item {Address address{};unsigned offset{},size{};};
        std::array<Item,20> journal{};std::array<std::byte,512> saved{};unsigned items{},used{};
        auto read=[&](Address p,void* bytes,std::size_t n){
            if(!fits(p,n)||items==journal.size()||n>saved.size()-used)return false;
            ++out.reads;out.bytes+=unsigned(n);if(!api.read(api.context,p,bytes,n))return false;
            std::memcpy(saved.data()+used,bytes,n);journal[items++]={p,used,unsigned(n)};used+=unsigned(n);return true;
        };
        auto load=[&](Address p,auto& value){return read(p,&value,sizeof value);};
        Address command{},viewport{};std::array<std::int32_t,4> ids{};std::uint32_t globals{};
        if(!load(api.engineBase+CommandBufferRva,command)||!load(api.engineBase+CurrentViewportRva,viewport)||
           !load(api.engineBase+MatrixIdsRva,ids)||!load(api.engineBase+GlobalValuesRva+0x4380,globals))return fail(Failure::Read);
        if(command||viewport!=call.viewport)return fail(Failure::Selector);
        if(!globals||globals>240)return fail(Failure::Handle);
        for(unsigned i=0;i<4;++i){
            if(ids[i]<=0||unsigned(ids[i])>globals)return fail(Failure::Handle);
            for(unsigned j=0;j<i;++j)if(ids[i]==ids[j])return fail(Failure::Handle);
            const auto index=Address(ids[i]-1);std::array<std::uint8_t,2> desc{};std::array<std::byte,16> mapping{};
            if(!load(api.engineBase+GlobalValuesRva+index*72,desc)||!load(api.engineBase+GlobalBuffersRva+index*16,mapping))return fail(Failure::Read);
            if(desc!=std::array<std::uint8_t,2>{9,1})return fail(Failure::Descriptor);
            if(field<Address>(mapping,0)!=call.matrixBuffer||field<std::int32_t>(mapping,8)!=std::int32_t(i*64))return fail(Failure::Mapping);
        }
        std::array<std::byte,32> buffer{};std::int32_t size{};
        if(!load(call.matrixBuffer,buffer))return fail(Failure::Read);
        const auto data=field<Address>(buffer,0),layout=field<Address>(buffer,8),mapped=field<Address>(buffer,24);
        if(!fits(data,272)||!fits(layout,28)||(mapped&&mapped!=data))return fail(Failure::Buffer);
        if(!load(layout+24,size))return fail(Failure::Read);
        if(size!=272)return fail(Failure::Buffer);
        std::array<float,16> viewClip{},viewInverse{},cpuClip{},cpuInverse{};
        if(!load(viewport+0xc0,viewClip)||!load(viewport+0x140,viewInverse)||
           !load(data+64,cpuClip)||!load(data+192,cpuInverse))return fail(Failure::Read);
        if(!same(viewClip,call.expected.relativeClip)||!same(viewInverse,call.expected.inverseView)||
           !same(cpuClip,call.expected.relativeClip)||!same(cpuInverse,call.expected.inverseView))return fail(Failure::Camera);
        std::array<std::byte,64> after{};
        for(unsigned i=0;i<items;++i){const auto& v=journal[i];++out.reads;out.bytes+=v.size;
            if(!api.read(api.context,v.address,after.data(),v.size)||std::memcmp(after.data(),saved.data()+v.offset,v.size))return fail(Failure::Changed);
        }
        if(!api.ownerCurrent(api.ownerContext,call.lease))return fail(Failure::Owner);
        out.valid=true;out.viewport=viewport;out.matrixBuffer=call.matrixBuffer;out.cpuData=data;return true;
    }
private:
    struct Errno {int value{errno};~Errno(){errno=value;}};
    static bool fits(Address p,std::size_t n)noexcept{return p&&n&&n<=UINTPTR_MAX-p;}
    static bool finite(const std::array<float,16>& m)noexcept{for(float v:m)if(!std::isfinite(v))return false;return true;}
    static bool same(const std::array<float,16>& a,const std::array<float,16>& b)noexcept{return !std::memcmp(a.data(),b.data(),64);}
    template<class T,std::size_t N>static T field(const std::array<std::byte,N>& b,unsigned at)noexcept{T v{};std::memcpy(&v,b.data()+at,sizeof v);return v;}
};
}
