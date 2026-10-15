#pragma once
#include "EngineFramebuffers.h"
#include "EngineTextureMap.h"
#include "EngineDepthResolve.h"
namespace rdrvr::engine {
struct EngineTargetBindingApi {
    void (*lock)(void*,int,const void*,const void*,unsigned,bool,int,unsigned,bool){};
    void (*unlock)(void*,int,const void*,int,bool){};
    void (*resolveDepth)(void*,bool){};
    explicit operator bool() const {return lock && unlock && resolveDepth;}
};
struct EngineDepthRoute {EngineDepthResolve* resolver{};void* context{};void* manager{};};
enum class TargetPassPolicy {StageOnly,Water};
struct EnginePostFxApi {
    void (*bind)(void*,bool,const void*,bool){};
    void (*release)(void*){};
};
struct WaterPostFxRoute {void* object{};const void* color{};const void* depth{};};
// Replace the factory's single main-world attachment binding, never its global
// owning descriptor. Only the explicitly mapped color/depth pair is supported.
// Unsupported nested offscreen work invalidates this eye, and must not fall
// through into a factory mutation while the adapter owns the framebuffer.
class TargetCallBoundary {
    struct Active {
        TargetCallBoundary* boundary{};EnginePassBinding* binding{};EngineTextureMap* map{};
        const EngineEyeTargets* targets{};void* factory{};unsigned eye{};EngineTargetRole role{};
        bool failed{},locked{},closed{};
        EngineDepthRoute depth{};
        TargetPassPolicy policy{};EngineTargetRole boundRole{};
        WaterPostFxRoute postfx{};unsigned postfxPhase{};
    };
    static thread_local Active* active_;
    EngineTargetBindingApi api_{};
    EnginePostFxApi postfxApi_{};
    const char* status_{"NOT_INVOKED"};unsigned depthCopies_{};
    static void fail(Active* s,const char* reason){if(!s->failed)s->boundary->status_=reason;s->failed=true;}
public:
    using Body=bool(*)(void*);
    static bool busy(){return active_!=nullptr;}
    void configure(EngineTargetBindingApi api){api_=api;}
    void configurePostFx(EnginePostFxApi api){postfxApi_=api;}
    const char* status()const{return status_;}
    unsigned depthCopies()const{return depthCopies_;}
    void resetReport(){if(!busy()){status_="NOT_INVOKED";depthCopies_=0;}}
    bool invoke(EnginePassBinding&,EngineTextureMap&,const EngineEyeTargets&,void* factory,unsigned,EngineTargetRole,Body,void*,EngineDepthRoute={},TargetPassPolicy=TargetPassPolicy::StageOnly,WaterPostFxRoute={});
    void lock(void*,int,const void*,const void*,unsigned,bool,int,unsigned,bool);
    void unlock(void*,int,const void*,int,bool);
    void resolveDepth(void*,bool);
    void bindPostFx(void*,bool,const void*,bool,bool pinnedWaterCaller);
    void releasePostFx(void*,bool pinnedWaterCaller);
};
}
