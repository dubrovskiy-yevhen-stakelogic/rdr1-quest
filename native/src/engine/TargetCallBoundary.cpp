//  - B: invoke() publishes the stage's failed flag to the draw hooks (TargetRouteGuard.h): after a failure the stage's
//       remaining native draws/clears are dropped instead of being recorded outside the closed eye pass;
//  - C: every first failure of a stage is counted per reason (report gameBootstrap.targetBoundary) and logged
//       (RdrVr WARN, at most once per second); routed invokes are counted per eye/role (eye 1 = native right-eye stage);
//  - resolveDepth: a failed in-stage depth copy reopens the eye pass at once (the success path's own rebind), so the
//       engine keeps an open pass for the rest of the (cancelled) stage.
// TargetCallBoundary.h stays the mirror header (no class change: it is compiled from mirror/ by other TUs).
#include "TargetCallBoundary.h"
#include "TargetRouteGuard.h"
#include <cerrno>
// First failure of the routed stage `s`: TargetCallBoundary::fail + count/log (members only: Active is private).
#define RDRVR_ROUTE_FAIL(s,reason) do{auto* const routeState_=(s);const bool routeAlready_=routeState_->failed;fail(routeState_,(reason));     if(!routeAlready_)::rdrvr::engine::targetroute::noteFailure((reason),routeState_->eye,unsigned(routeState_->role));}while(0)
namespace rdrvr::engine {
thread_local TargetCallBoundary::Active* TargetCallBoundary::active_{};
bool TargetCallBoundary::invoke(EnginePassBinding& binding,EngineTextureMap& map,const EngineEyeTargets& targets,
                                void* factory,unsigned eye,EngineTargetRole role,Body body,void* user,EngineDepthRoute depth,TargetPassPolicy policy,WaterPostFxRoute postfx){
    if(!active_){status_="TARGET_PREFLIGHT";depthCopies_=0;}
    if(active_ || !api_ || !body || !factory || !binding.matches(eye,role) ||
       (role!=EngineTargetRole::Color && role!=EngineTargetRole::ShadowCollector && role!=EngineTargetRole::LightAccumulation) ||
       (policy!=TargetPassPolicy::StageOnly && policy!=TargetPassPolicy::Water) ||
       (policy==TargetPassPolicy::Water && (role!=EngineTargetRole::Color || !targets.waterReady()))){
        targetroute::preflightRejects.fetch_add(1,std::memory_order_relaxed);return false;}
    Active state{this,&binding,&map,&targets,factory,eye,role};
    status_="READY";
    state.depth=depth;state.policy=policy;state.boundRole=role;state.postfx=postfx;
    // RenderColour starts with main already bound by the caller. PreWater's
    // first operation may release that inherited binding without a local lock.
    state.locked=policy==TargetPassPolicy::Water;
    struct Reset {~Reset(){active_=nullptr;targetroute::leaveRoute();}} reset;active_=&state;
    targetroute::enterRoute(&state.failed,eye,unsigned(role)); // The draw hooks drop draws once state.failed
    const auto finish=[&]{
        binding.captureRasterObservation(true);
        active_=nullptr;targetroute::leaveRoute();
        // Unlock closes the eye pass. Reopen the expected stage attachment so
        // the caller can verify camera/binding before advancing or copying.
        const bool restored=(state.closed || state.boundRole!=role)?binding.eye(eye,role):binding.finishDraw(eye,role);
        // PostWater deliberately leaves main bound for the remaining color
        // buckets. Only this exact water-policy endpoint may remain open;
        // incomplete refraction/depth passes still fail. Callback/query scopes
        // separately require a complete Pre/Post interval before publication.
        const bool openMain=policy==TargetPassPolicy::Water && state.locked && !state.closed && state.boundRole==EngineTargetRole::Color;
        if(!restored)RDRVR_ROUTE_FAIL(&state,"TARGET_FINISH_BINDING");
        if(state.locked&&!openMain)RDRVR_ROUTE_FAIL(&state,"TARGET_UNBALANCED_LOCK");
        if(state.postfxPhase==1)RDRVR_ROUTE_FAIL(&state,"POSTFX_INCOMPLETE");
        return !state.failed;
    };
    bool result{};
    try{result=body(user);}catch(...){finish();throw;}
    const int saved=errno;const bool restored=finish();errno=saved;
    return result && restored;
}
void TargetCallBoundary::lock(void* factory,int index,const void* color,const void* depth,unsigned face,bool changeDepth,int resolve,unsigned mip,bool window){
    auto* s=active_;
    if(!s || s->boundary!=this){api_.lock(factory,index,color,depth,face,changeDepth,resolve,mip,window);return;}
    const int saved=errno;
    if(s->failed)return;
    const void *mappedColor{},*mappedDepth{};
    auto role=s->role;
    bool colorValid=s->map->attachment(color,false,&mappedColor);
    if(colorValid && s->policy==TargetPassPolicy::Water){
        if(mappedColor==s->targets->target(s->eye,EngineTargetRole::Refraction))role=EngineTargetRole::Refraction;
        else if(mappedColor==s->targets->target(s->eye,EngineTargetRole::WaterDepth))role=EngineTargetRole::WaterDepth;
    }
    const bool refraction=role==EngineTargetRole::Refraction && s->policy==TargetPassPolicy::Water;
    const bool depthValid=refraction?!depth:(s->map->attachment(depth,true,&mappedDepth) && mappedDepth==s->targets->target(s->eye,true));
    // BindPCRenderTarget also reopens main color with window=false. Its
    // attachment dimensions still match; this variant is water-scope only.
    const bool windowValid=refraction?!window:(window || (s->policy==TargetPassPolicy::Water && role==EngineTargetRole::Color));
    // These are the exact arguments used by LockShadowCollector and
    // BeginRenderingLights. The pinned factory ignores resolve, but restricting
    // it avoids accidentally accepting an unaudited variant of the call.
    if(s->failed || factory!=s->factory || index || face || !changeDepth || resolve!=-1 || mip || !windowValid ||
       !colorValid || !depthValid || mappedColor!=s->targets->target(s->eye,role) ||
       !s->binding->eye(s->eye,role))RDRVR_ROUTE_FAIL(s,"TARGET_LOCK_ARGUMENT_OR_BINDING");
    else{s->locked=true;s->closed=false;s->boundRole=role;}
    // All mapped images have the original full extent. eye() sets that exact
    // viewport/scissor through GfxContext without changing global dimensions.
    errno=saved;
}
void TargetCallBoundary::unlock(void* factory,int index,const void* flags,int resolve,bool window){
    auto* s=active_;
    if(!s || s->boundary!=this){api_.unlock(factory,index,flags,resolve,window);return;}
    const int saved=errno;
    // This pinned D11 implementation never reads flags or resolve. The light
    // pass supplies a stack flags object whereas the collector often uses null.
    if(s->failed || factory!=s->factory || index || resolve!=-1 || !s->locked ||
       !s->binding->matches(s->eye,s->boundRole,true) || !s->binding->close())RDRVR_ROUTE_FAIL(s,"TARGET_UNLOCK_ARGUMENT_OR_BINDING");
    else{s->locked=false;s->closed=true;}
    errno=saved;
}
void TargetCallBoundary::resolveDepth(void* manager,bool force){
    auto* s=active_;
    if(!s || s->boundary!=this){api_.resolveDepth(manager,force);return;}
    const int saved=errno;
    // Collector(false) resolves before shadow sampling. The main color pass
    // also calls ResolveDepthBuffer(true) at 0x2a5025c after opaque geometry.
    // That pass inherits its binding from EngineStereoPass, without a local
    // factory lock. Copy the current eye again before alpha/soft-depth effects;
    // the mono manager cache cannot represent this eye's newly written depth.
    const bool collector=s->role==EngineTargetRole::ShadowCollector && !force && s->locked;
    const bool color=s->role==EngineTargetRole::Color && force;
    if(s->failed || !s->depth.resolver || !s->depth.context || manager!=s->depth.manager ||
       (!collector && !color) || s->boundRole!=s->role || s->closed ||
       !s->binding->matches(s->eye,s->role,true) || !s->binding->close())RDRVR_ROUTE_FAIL(s,"DEPTH_RESOLVE_CONTEXT_OR_BINDING");
    else {
        s->closed=true;
        if(!s->depth.resolver->copy(s->eye,s->depth.context,s->binding->command())){
            RDRVR_ROUTE_FAIL(s,"DEPTH_RESOLVE_COPY");
            // close() above left the engine with no render pass. Reopen
            // the eye pass now, exactly as the success path does below; the stage stays failed (cancelled) and the draw
            // hooks drop its remaining draws either way. On a reopen failure the pass stays closed (finish() rebinds).
            if(s->binding->eye(s->eye,s->role)){s->closed=false;targetroute::reopened.fetch_add(1,std::memory_order_relaxed);}
            else targetroute::reopenFailed.fetch_add(1,std::memory_order_relaxed);
        }
        else if(!s->binding->eye(s->eye,s->role))RDRVR_ROUTE_FAIL(s,"DEPTH_RESOLVE_REBIND");
        else {s->closed=false;++depthCopies_;}
    }
    errno=saved;
}
void TargetCallBoundary::releasePostFx(void* object,bool pinnedWaterCaller){
    auto* s=active_;
    if(!s||s->boundary!=this){postfxApi_.release(object);return;}
    // Native Release also binds the display backbuffer and decrements factory
    // references. A private water pass only closes its own active main buffer;
    // the shared postFX +0x18 flag and factory descriptor remain untouched.
    if(s->failed||s->policy!=TargetPassPolicy::Water||!pinnedWaterCaller||!object||object!=s->postfx.object||
       s->postfxPhase||s->boundRole!=EngineTargetRole::Color||!s->locked){RDRVR_ROUTE_FAIL(s,"POSTFX_RELEASE");return;}
    unlock(s->factory,0,nullptr,-1,false);
    if(!s->failed)s->postfxPhase=1;
}
void TargetCallBoundary::bindPostFx(void* object,bool flag,const void* depth,bool finalFlag,bool pinnedWaterCaller){
    auto* s=active_;
    if(!s||s->boundary!=this){postfxApi_.bind(object,flag,depth,finalFlag);return;}
    if(s->failed||s->policy!=TargetPassPolicy::Water||!pinnedWaterCaller||!object||object!=s->postfx.object||
       s->postfxPhase!=1||flag||!finalFlag||!depth||depth!=s->postfx.depth||!s->postfx.color||s->locked){RDRVR_ROUTE_FAIL(s,"POSTFX_BIND");return;}
    const void* color{};
    if(!s->map->attachment(s->postfx.color,false,&color)||color!=s->targets->target(s->eye,EngineTargetRole::Color)){RDRVR_ROUTE_FAIL(s,"POSTFX_COLOR_MAPPING");return;}
    // Exact native BindPCRenderTarget factory arguments, including window=false.
    // Avoid its sole additional mutation: postFX +0x18 = 1.
    lock(s->factory,0,s->postfx.color,depth,0,true,-1,0,false);
    if(!s->failed)s->postfxPhase=2;
}
}
#undef RDRVR_ROUTE_FAIL
