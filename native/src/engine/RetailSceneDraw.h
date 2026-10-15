#pragma once
#include "RetailSceneBoundary.h"
#include "EngineStereoPass.h"
#include "SceneCallBoundary.h"
#include "ReplayUpdates.h"
#include "WorldDrawProfile.h"
#include "WaterCallbackBoundary.h"
#include <string>
namespace rdrvr::engine {
struct RetailDrawApi {
    RetailSceneBoundary::Draw depth{},color{};
    RetailSceneBoundary::Lights lights{};
    void (*collector)(void*,void*){};
    void (*beginColor)(void*){};
    void (*setupDepth)(void*,bool){};
    void (*depthTargets)(void*,void*,void*){};
    void (*clear)(bool,std::uint32_t,bool,float,bool,unsigned,int){};
    explicit operator bool()const{return depth&&color&&lights&&collector&&beginColor&&setupDepth&&depthTargets&&clear;}
};
struct WaterSceneReplay {
    WaterCallbackBoundary* callbacks{};void* request{};void* query{};WaterPostFxRoute postfx{};
    explicit operator bool()const{return callbacks&&request&&query&&postfx.object&&postfx.color&&postfx.depth;}
    bool any()const{return callbacks||request||query||postfx.object||postfx.color||postfx.depth;}
};
// Synchronous body for EngineStereoPass::drawScene. No full RenderScene/Frame,
// simulation update, target-pointer replacement or private shaft fabrication.
// All borrowed objects must remain inside RetailSceneBoundary's consumer call.
class RetailSceneDraw {
    RetailDrawApi api_;const RetailSceneFrame& frame_;void* collector_{};void* factory_{};void* context_{};
    void* stockDepth_{};void* stockSampled_{};std::span<const StockSceneTarget> stock_;
    EngineTextureMap& map_;TextureCallBoundary& textures_;TargetCallBoundary& targets_;
    EngineDepthResolve& resolve_;ReplayUpdates& replay_;WorldDrawProfile& profile_;
    WaterSceneReplay water_{};
    const char* status_{"NOT_ATTEMPTED"};unsigned stage_{},eye_{},depthCopies_{};
    bool routed_{},replayGood_{},profileGood_{};
    bool stage(SceneStage,unsigned,const void*,const EngineEyeTargets&,EnginePassBinding&);
public:
    RetailSceneDraw(RetailDrawApi api,const RetailSceneFrame& frame,void* collector,void* factory,void* context,
                    void* stockDepth,void* stockSampled,std::span<const StockSceneTarget> stock,
                    EngineTextureMap& map,TextureCallBoundary& textures,TargetCallBoundary& targets,
                    EngineDepthResolve& resolve,ReplayUpdates& replay,WorldDrawProfile& profile,WaterSceneReplay water={}):
        api_(api),frame_(frame),collector_(collector),factory_(factory),context_(context),stockDepth_(stockDepth),stockSampled_(stockSampled),stock_(stock),
        map_(map),textures_(textures),targets_(targets),resolve_(resolve),replay_(replay),profile_(profile),water_(water){}
    static bool draw(void*,SceneStage,unsigned,const void*,const EngineEyeTargets&,EnginePassBinding&);
    std::string report()const;
};
}
