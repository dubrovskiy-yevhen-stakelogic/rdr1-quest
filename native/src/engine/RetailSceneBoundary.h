#pragma once
#include <atomic>
#include "diagnostics/FrameTimings.h"
#include "diagnostics/ThreadCpuScope.h"
#include "diagnostics/DrawStageTimings.h"
#ifdef RDRVR_HEAD_CAMERA
#endif
#include "diagnostics/ThreadLoad.h"
#include <cerrno>
#include <cstdint>

namespace rdrvr::engine {
struct RetailSceneFrame {
    void* renderer{};void* viewport{};void* lights{};const void* shaft{};
};
// The shaft is a RenderScene stack object. The consumer runs synchronously at
// the return of that scene's color pass, before matrix unlock / scene signals.
// Neither this class nor its consumer may retain its borrowed pointers.
class RetailSceneBoundary {
public:
    using Draw=void(*)(void*,void*);
    using Lights=void(*)(void*,const void*);
    using Consumer=bool(*)(const RetailSceneFrame&);
    using DrawRoute=void(*)(Draw,void*,void*,bool);
    using LightRoute=void(*)(Lights,void*,const void*,bool);
private:
    struct State {RetailSceneBoundary* owner{};RetailSceneFrame frame{};unsigned phase{};bool failed{},consuming{};};
    inline static thread_local State* active_{};
    Draw depth_{},color_{};Lights lights_{};Consumer consumer_{};
    DrawRoute depthRoute_{},colorRoute_{};LightRoute lightRoute_{};
    std::atomic<std::uint64_t> complete_{},consumed_{},declined_{},errors_{};
    State* state(){return active_&&active_->owner==this&&!active_->failed&&!active_->consuming?active_:nullptr;}
public:
    class Scene {
        State state_;State* previous_{};
        diagnostics::WallScope timing_{diagnostics::frameTimings.nativeSceneInclusive};
        diagnostics::ThreadCpuScope cpuTiming_{diagnostics::threadCpuTimings.nativeSceneInclusive};
    public:
        Scene(RetailSceneBoundary& owner,void* renderer,void* viewport):state_{&owner,{renderer,viewport,nullptr,nullptr}},previous_(active_){
            if(previous_){previous_->failed=true;state_.failed=true;}
            if(!renderer||!viewport)state_.failed=true;active_=&state_;
            if(!state_.failed)diagnostics::renderThreadLoad.sample();
        }
        ~Scene(){active_=previous_;}
        Scene(const Scene&)=delete;Scene& operator=(const Scene&)=delete;
    };
    void configure(Draw depth,Lights lights,Draw color,Consumer consumer){depth_=depth;lights_=lights;color_=color;consumer_=consumer;}
    void configureRoutes(DrawRoute depth,LightRoute lights,DrawRoute color){depthRoute_=depth;lightRoute_=lights;colorRoute_=color;}
    bool qualifiesColor(void* renderer,void* viewport,bool exactCaller){
        auto* s=exactCaller?state():nullptr;
        return s&&s->phase==2&&renderer==s->frame.renderer&&viewport==s->frame.viewport;
    }
    void depth(void* renderer,void* viewport,bool exactCaller){
        auto* s=exactCaller?state():nullptr;
        diagnostics::WallScope timing(s?&diagnostics::frameTimings.stockDepth:nullptr);
        diagnostics::ThreadCpuScope cpuTiming(s?&diagnostics::threadCpuTimings.stockDepth:nullptr);
        diagnostics::DrawStageTimings::Stage drawTiming(diagnostics::drawStageTimings,0,s!=nullptr);
#ifdef RDRVR_HEAD_CAMERA
#endif
        try{if(depthRoute_)depthRoute_(depth_,renderer,viewport,s&&s->phase==0&&renderer==s->frame.renderer&&viewport==s->frame.viewport);else depth_(renderer,viewport);}catch(...){if(s)s->failed=true;throw;}
        drawTiming.finish();cpuTiming.finish();timing.finish();
        if(!s||s->failed)return;
        if(s->phase||renderer!=s->frame.renderer||viewport!=s->frame.viewport)s->failed=true;
        else s->phase=1;
    }
    void lights(void* manager,const void* shaft,bool exactCaller){
        auto* s=exactCaller?state():nullptr;
        diagnostics::WallScope timing(s?&diagnostics::frameTimings.stockLight:nullptr);
        diagnostics::ThreadCpuScope cpuTiming(s?&diagnostics::threadCpuTimings.stockLight:nullptr);
        diagnostics::DrawStageTimings::Stage drawTiming(diagnostics::drawStageTimings,1,s!=nullptr);
#ifdef RDRVR_HEAD_CAMERA
#endif
        try{if(lightRoute_)lightRoute_(lights_,manager,shaft,s&&s->phase==1);else lights_(manager,shaft);}catch(...){if(s)s->failed=true;throw;}
        drawTiming.finish();cpuTiming.finish();timing.finish();
        if(!s||s->failed)return;
        if(s->phase!=1||!manager||!shaft)s->failed=true;
        else{s->frame.lights=manager;s->frame.shaft=shaft;s->phase=2;}
    }
    void color(void* renderer,void* viewport,bool exactCaller){
        auto* s=exactCaller?state():nullptr;
        diagnostics::WallScope timing(s?&diagnostics::frameTimings.stockColor:nullptr);
        diagnostics::ThreadCpuScope cpuTiming(s?&diagnostics::threadCpuTimings.stockColor:nullptr);
        diagnostics::DrawStageTimings::Stage drawTiming(diagnostics::drawStageTimings,2,s!=nullptr);
#ifdef RDRVR_HEAD_CAMERA
#endif
        try{if(colorRoute_)colorRoute_(color_,renderer,viewport,s&&s->phase==2&&renderer==s->frame.renderer&&viewport==s->frame.viewport);else color_(renderer,viewport);}catch(...){if(s)s->failed=true;throw;}
        drawTiming.finish();cpuTiming.finish();timing.finish();
        if(!s||s->failed)return;
        if(s->phase!=2||renderer!=s->frame.renderer||viewport!=s->frame.viewport){s->failed=true;return;}
        const int result=errno;s->phase=3;s->consuming=true;++complete_;
#ifdef RDRVR_HEAD_CAMERA
        // RIGHT is consumed below. It must not be attributed to LEFT color.
#endif
        try{if(consumer_&&consumer_(s->frame))++consumed_;else ++declined_;}catch(...){++errors_;}
        s->consuming=false;errno=result;
    }
    std::uint64_t complete()const{return complete_;}
    std::uint64_t consumed()const{return consumed_;}
    std::uint64_t declined()const{return declined_;}
    std::uint64_t errors()const{return errors_;}
};
}
