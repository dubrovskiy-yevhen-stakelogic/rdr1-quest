#pragma once
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace rdrvr::engine {
struct alignas(16) AimNativeVector {std::array<float,4> v{};};
struct alignas(16) AimNativeSegment {AimNativeVector start{},end{};};
// Audited ARM64 phIntersection stride, not a declaration of the retail class.
struct alignas(16) AimNativeIntersection {std::array<std::byte,0x70> bytes{};};
static_assert(sizeof(AimNativeVector)==16&&sizeof(AimNativeSegment)==32&&sizeof(AimNativeIntersection)==0x70);
struct NativeAimProbeApi {
    int (*exclude)(const std::uint32_t*,const void**,int,unsigned){};
    int (*probe)(const AimNativeSegment*,int,AimNativeIntersection*,int,const void* const*,int,int){};
    AimNativeIntersection* (*closest)(int,const AimNativeVector*,AimNativeIntersection*,bool,AimNativeIntersection**){};
    explicit operator bool()const{return exclude&&probe&&closest;}
};
struct NativeAimHit {
    bool blocked{};AimNativeVector point{};float distance{};
    // Copied native result; instance references are borrowed, never cached or dereferenced here.
    AimNativeIntersection intersection{};
};
class NativeAimProbe {
    NativeAimProbeApi api_;
    template<class T>static void put(AimNativeIntersection& hit,std::size_t offset,T value){std::memcpy(hit.bytes.data()+offset,&value,sizeof(value));}
public:
    explicit NativeAimProbe(NativeAimProbeApi api):api_(api){}
    bool castBetween(std::uint32_t actorGuid,const AimNativeVector& start,const AimNativeVector& end,NativeAimHit& out)const noexcept{
        const int saved=errno;struct Restore{int saved;~Restore(){errno=saved;}} restore{saved};
        AimNativeVector direction{};double length2{};
        for(unsigned i=0;i<3;++i){
            if(!std::isfinite(start.v[i])||!std::isfinite(end.v[i]))return false;
            const double delta=double(end.v[i])-start.v[i];length2+=delta*delta;
        }
        const double length=std::sqrt(length2);
        if(!std::isfinite(length)||length<0.0001||length>2000)return false;
        for(unsigned i=0;i<3;++i)direction.v[i]=float((double(end.v[i])-start.v[i])/length);
        return cast(actorGuid,start,direction,float(length),out);
    }
    // Invoke only on the qualified local native owner/input thread, at a pinned
    // reticle or ordinary vector-Aim boundary, with freshly checked context.
    // Saturation is not a clear ray. All output is local until fully validated.
    bool cast(std::uint32_t actorGuid,const AimNativeVector& origin,const AimNativeVector& direction,
              float range,NativeAimHit& out)const noexcept {
        const int saved=errno;struct Restore{int saved;~Restore(){errno=saved;}} restore{saved};
        try {
            if(!api_||!std::isfinite(range)||range<=0||range>2000)return false;
            double length2{};AimNativeSegment segment{};segment.start=origin;
            for(unsigned i=0;i<3;++i){
                if(!std::isfinite(origin.v[i])||!std::isfinite(direction.v[i]))return false;
                length2+=double(direction.v[i])*direction.v[i];segment.end.v[i]=origin.v[i]+range*direction.v[i];
                if(!std::isfinite(segment.end.v[i]))return false;
            }
            if(std::abs(length2-1)>0.0001f)return false;
            double segmentLength2{};
            for(unsigned i=0;i<3;++i){const double d=double(segment.end.v[i])-segment.start.v[i];segmentLength2+=d*d;}
            if(std::abs(std::sqrt(segmentLength2)-range)>range*0.001)return false;
            segment.start.v[3]=0;
            std::array<const void*,20> excluded{};
            const auto count=api_.exclude(&actorGuid,excluded.data(),int(excluded.size()),0x176);
            // At least the owner's instance must exist. Full list may be truncated.
            if(count<=0||count>=int(excluded.size()))return false;
            for(int i=0;i<count;++i)if(!excluded[i])return false;
            std::array<AimNativeIntersection,16> hits{};
            for(auto& hit:hits){put(hit,8,std::uint32_t(0xffff));put(hit,0x64,std::uint32_t(0xffff));}
            const int found=api_.probe(&segment,int(hits.size()),hits.data(),count,excluded.data(),0x500208,0);
            if(found<0||found>=int(hits.size()))return false;
            // Native reticle asks the selector to inspect all initialized slots,
            // not the return value of TestInLevel. false retains material filtering.
            auto* selected=api_.closest(int(hits.size()),&segment.start,hits.data(),false,nullptr);
            NativeAimHit result{};result.point=segment.end;result.distance=float(range*std::sqrt(length2));
            if(selected){
                const auto address=reinterpret_cast<std::uintptr_t>(selected),begin=reinterpret_cast<std::uintptr_t>(hits.data());
                if(address<begin||address>=begin+sizeof(hits)||(address-begin)%sizeof(AimNativeIntersection))return false;
                std::uintptr_t instance{};std::memcpy(&instance,selected->bytes.data(),sizeof(instance));
                if(!instance)return false;
                std::memcpy(result.point.v.data(),selected->bytes.data()+0x10,12);result.point.v[3]=0;
                double along{},distance2{};
                for(unsigned i=0;i<3;++i){
                    if(!std::isfinite(result.point.v[i]))return false;
                    const double delta=double(result.point.v[i])-origin.v[i];along+=delta*direction.v[i];distance2+=delta*delta;
                }
                const double tolerance=0.002+range*0.00001;
                if(along/length2 < -tolerance||along/length2>range+tolerance||distance2-along*along/length2>tolerance*tolerance)return false;
                result.distance=float(std::sqrt(distance2));result.blocked=true;result.intersection=*selected;
            }
            out=result;return true;
        }catch(...){return false;}
    }
};
}
