#include "RetailSceneDraw.h"
#include <sstream>
namespace rdrvr::engine {
bool RetailSceneDraw::draw(void* user,SceneStage stage,unsigned eye,const void* viewport,const EngineEyeTargets& images,EnginePassBinding& binding){
    return user&&static_cast<RetailSceneDraw*>(user)->stage(stage,eye,viewport,images,binding);
}
bool RetailSceneDraw::stage(SceneStage stage,unsigned eye,const void* viewport,const EngineEyeTargets& images,EnginePassBinding& binding){
    stage_=unsigned(stage);eye_=eye;routed_=replayGood_=profileGood_=false;status_="RETAIL_PREFLIGHT";
    targets_.resetReport();textures_.resetReport();
    if(!api_||!frame_.renderer||!frame_.lights||!frame_.shaft||!collector_||!viewport||!stockDepth_||!stockSampled_)return false;
    EngineTargetRole role{};
    switch(stage){
        case SceneStage::Depth:role=EngineTargetRole::ShadowCollector;break;
        case SceneStage::Lighting:role=EngineTargetRole::LightAccumulation;break;
        case SceneStage::Color:role=EngineTargetRole::Color;break;
        default:return false;
    }
    diagnostics::DrawStageTimings::Stage drawTiming(diagnostics::drawStageTimings,3+eye*3+unsigned(stage));
#ifdef RDRVR_HEAD_CAMERA
#endif
    status_="REPLAY_SCOPE";ReplayUpdates::Eye replay(replay_,frame_.renderer);if(!replay)return false;
    std::optional<WaterCallbackBoundary::Eye> waterScope;
    if(stage==SceneStage::Color&&water_.any()){
        if(!water_||!images.waterReady()||water_.postfx.depth!=stockDepth_)return false;
        bool mainColor=false;
        for(const auto& source:stock_)if(source.role==EngineTargetRole::Color&&source.target==water_.postfx.color)mainColor=true;
        if(!mainColor)return false;
        const auto* color=images.image(eye);if(!color)return false;
        waterScope.emplace(*water_.callbacks,frame_.renderer,water_.request,water_.query,color->image.extent.width,color->image.extent.height);
        if(!waterScope->good())return false;
    }
    WorldDrawProfile::Eye profile(profile_,waterScope?water_.request:nullptr);
    struct Body {
        RetailSceneDraw& self;SceneStage stage;const void* viewport;
        static bool invoke(void* ptr){auto& b=*static_cast<Body*>(ptr);auto& s=b.self;
            switch(b.stage){
                case SceneStage::Depth:
                    // Directional shadows run inside depth(), before Lighting.
                    // Native ResolveDepthBuffer also binds this instance sampler;
                    // the mod's private copy route replaces that function, so bind it
                    // here while the texture journal maps/restores stock pointers.
                    s.api_.depthTargets(s.frame_.lights,s.stockDepth_,s.stockSampled_);
                    s.api_.setupDepth(s.frame_.renderer,false);
                    s.api_.depth(s.frame_.renderer,const_cast<void*>(b.viewport));break;
                case SceneStage::Lighting:
                    s.api_.collector(s.collector_,nullptr);
                    // Re-submit the same borrowed stock pointers through the
                    // shader argument route, not eye pointers into the manager.
                    s.api_.depthTargets(s.frame_.lights,s.stockDepth_,s.stockSampled_);
                    s.api_.lights(s.frame_.lights,s.frame_.shaft);break;
                case SceneStage::Color:
                    s.api_.collector(s.collector_,nullptr);
                    s.api_.beginColor(s.frame_.lights);
                    s.api_.clear(true,0xff000000u,false,1.0f,false,0,255);
                    s.api_.color(s.frame_.renderer,const_cast<void*>(b.viewport));break;
            }
            return true;
        }
    } body{*this,stage,viewport};
    status_="SCENE_ROUTES";
    const bool routed=invokeSceneRoutes(textures_,targets_,map_,stock_,binding,images,factory_,eye,role,
        {&resolve_,context_,frame_.lights},Body::invoke,&body,waterScope?TargetPassPolicy::Water:TargetPassPolicy::StageOnly,
        waterScope?water_.postfx:WaterPostFxRoute{});
    depthCopies_+=targets_.depthCopies();routed_=routed;replayGood_=replay.good();profileGood_=profile.good();
    status_=!routed?"SCENE_ROUTES":!replayGood_?"REPLAY_UPDATES":!profileGood_?"WORLD_PROFILE":
        (waterScope&&!waterScope->good())?"WATER_SCOPE":"READY";
    return routed&&replayGood_&&profileGood_&&(!waterScope||waterScope->good());
}
std::string RetailSceneDraw::report()const {
    std::ostringstream o;o<<"{\"state\":\""<<status_<<"\",\"stage\":"<<stage_<<",\"eye\":"<<eye_
        <<",\"target\":\""<<targets_.status()<<"\",\"texture\":\""<<textures_.status()
        <<"\",\"depthCopies\":"<<depthCopies_<<",\"routed\":"<<routed_
        <<",\"replayGood\":"<<replayGood_<<",\"profileGood\":"<<profileGood_<<'}';return o.str();
}
}
