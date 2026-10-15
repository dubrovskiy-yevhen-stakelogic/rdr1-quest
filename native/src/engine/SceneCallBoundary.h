#pragma once
#include "TextureCallBoundary.h"
#include "TargetCallBoundary.h"
namespace rdrvr::engine {
// One lexical body controls both texture arguments and target/resolve calls.
// Keep the texture scope outermost: original shader values are restored only
// after the expected framebuffer has been recovered on the inner scope's exit.
inline bool invokeSceneRoutes(TextureCallBoundary& textures,TargetCallBoundary& targets,
                               EngineTextureMap& map,std::span<const StockSceneTarget> stock,
                               EnginePassBinding& binding,const EngineEyeTargets& images,void* factory,
                               unsigned eye,EngineTargetRole role,EngineDepthRoute depth,
                               TargetCallBoundary::Body draw,void* user,TargetPassPolicy policy=TargetPassPolicy::StageOnly,WaterPostFxRoute postfx={}){
    if(!draw || TextureCallBoundary::busy() || TargetCallBoundary::busy() || !map.prepare(eye,stock))return false;
    struct Call {
        TargetCallBoundary& targets;EngineTextureMap& map;EnginePassBinding& binding;const EngineEyeTargets& images;
        void* factory;unsigned eye;EngineTargetRole role;EngineDepthRoute depth;TargetCallBoundary::Body draw;void* user;TargetPassPolicy policy;WaterPostFxRoute postfx;
        static bool invoke(void* p){auto& c=*static_cast<Call*>(p);
            return c.targets.invoke(c.binding,c.map,c.images,c.factory,c.eye,c.role,c.draw,c.user,c.depth,c.policy,c.postfx);}
    } call{targets,map,binding,images,factory,eye,role,depth,draw,user,policy,postfx};
    return textures.invoke(EngineTextureMap::route,&map,Call::invoke,&call);
}
}
