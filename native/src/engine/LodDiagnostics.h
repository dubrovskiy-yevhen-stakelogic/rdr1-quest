#pragma once
#include "StereoVisibility.h"
#include "diagnostics/FrameTimings.h"
#include "runtime/ResolutionTelemetry.h"
#include <array>
#include <cstring>
#include <mutex>
#include <sstream>
namespace rdrvr::engine {
// Read-only fields audited against the supported ELF. Native renderer getters
// load the four floats at +570/+574/+578/+57c. grcViewport::RegeneratePerspective
// uses +338 (degrees), +340/+344 (clip), +348/+34c (cached half-angle tangents).
class LodDiagnostics {
public:
    using Reader=bool(*)(void*,std::uintptr_t,void*,std::size_t);
    struct Values {std::array<float,4> renderer{};std::array<float,6> camera{};float treeHigh{};};
private:
    Reader read_{};void* user_{};std::uintptr_t treeHigh_{};
    std::uint64_t next_{},samples_{},failures_{},changed_{};
    mutable std::mutex mutex_;std::string report_{"{\"state\":\"WAITING\"}"};
    bool values(std::uintptr_t renderer,std::uintptr_t viewport,Values& out){
        if(!read_||!renderer||!viewport||!treeHigh_||renderer>UINTPTR_MAX-0x580||viewport>UINTPTR_MAX-0x350)return false;
        if(!read_(user_,renderer+0x570,out.renderer.data(),16)||!read_(user_,viewport+0x338,out.camera.data(),24)||
           !read_(user_,treeHigh_,&out.treeHigh,4))return false;
        for(float v:out.renderer)if(!std::isfinite(v))return false;
        for(float v:out.camera)if(!std::isfinite(v))return false;
        return std::isfinite(out.treeHigh);
    }
    void finish(std::uintptr_t renderer,std::uintptr_t viewport,const Values& before,const StereoCameraPlan& eyes){
        Values after{};const bool valid=values(renderer,viewport,after);
        ++samples_;if(!valid)++failures_;
        const bool changed=valid&&(before.renderer!=after.renderer||before.camera!=after.camera||before.treeHigh!=after.treeHigh);
        if(changed)++changed_;
        StereoCullPlan cull{};
        const float eyeNear=eyes.projection[0][14]/eyes.projection[0][10];
        const bool cullValid=prepareStereoCull(eyes,eyeNear,before.camera[3],cull);
        std::ostringstream o;o<<"{\"state\":\""<<(valid?"SAMPLED":"POST_READ_FAILED")<<"\",\"samples\":"<<samples_
            <<",\"readFailures\":"<<failures_<<",\"changedAcrossEyes\":"<<changed_<<",\"units\":\"native-engine\""
            <<",\"worldStreamingRadius\":"<<before.renderer[0]<<",\"terrainStreamingFactor\":"<<before.renderer[1]
            <<",\"treeDetail\":"<<before.renderer[2]<<",\"treeHighLodStreamingDistance\":"<<before.renderer[3]
            <<",\"treeHighDetailRangeFromCellSurface\":"<<before.treeHigh
            <<",\"stockFovDegrees\":"<<before.camera[0]<<",\"stockNear\":"<<before.camera[2]<<",\"stockFar\":"<<before.camera[3]
            <<",\"stockTanHalfVertical\":"<<before.camera[4]<<",\"stockTanHalfHorizontal\":"<<before.camera[5];
        if(cullValid)o<<",\"cullNear\":"<<cull.camera.nearClip<<",\"cullFar\":"<<cull.camera.farClip
            <<",\"cullBackshift\":"<<cull.originBackshift<<",\"cullHorizontalTangentSpan\":"<<2/cull.camera.projection[0]
            <<",\"leftHorizontalTangentSpan\":"<<2/eyes.projection[0][0]<<",\"rightHorizontalTangentSpan\":"<<2/eyes.projection[1][0];
        o<<'}';std::lock_guard lock(mutex_);report_=o.str();
    }
public:
    void configure(Reader read,void* user,std::uintptr_t treeHigh){read_=read;user_=user;treeHigh_=treeHigh;}
    std::string report()const{std::lock_guard lock(mutex_);return report_;}
    class Pair {
        LodDiagnostics* owner_{};std::uintptr_t renderer_{},viewport_{};Values before_{};const StereoCameraPlan* eyes_{};
    public:
        Pair(LodDiagnostics& owner,std::uintptr_t renderer,std::uintptr_t viewport,const StereoCameraPlan& eyes,std::uint64_t now):renderer_(renderer),viewport_(viewport),eyes_(&eyes){
            if(now<owner.next_)return;owner.next_=now+1'000'000'000ull;
            // GetResolutionScale (retail ELF +2a580b8) reads renderer+564.
            // Read independently: unavailable DRS telemetry must not gate eyes.
            float scale=-1;
            if(!owner.read_||!renderer||renderer>UINTPTR_MAX-0x568||!owner.read_(owner.user_,renderer+0x564,&scale,4)||
               !std::isfinite(scale)||scale<=0||scale>4)scale=-1;
            runtime::resolutionTelemetry.nativeScale.store(scale,std::memory_order_relaxed);
            if(owner.values(renderer,viewport,before_))owner_=&owner;
            else {++owner.failures_;std::lock_guard lock(owner.mutex_);owner.report_="{\"state\":\"PRE_READ_FAILED\"}";}
        }
        ~Pair(){if(owner_)try{owner_->finish(renderer_,viewport_,before_,*eyes_);}catch(...){}}
        Pair(const Pair&)=delete;Pair& operator=(const Pair&)=delete;
    };
};
}
