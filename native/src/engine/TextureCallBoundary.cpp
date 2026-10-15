#include "TextureCallBoundary.h"
#include <cerrno>
namespace rdrvr::engine {
thread_local TextureCallBoundary::Active TextureCallBoundary::active_{};
bool TextureCallBoundary::invoke(EngineTextureScope::Map map,void* mapUser,Body body,void* bodyUser) {
    if(active_.boundary || !body)return false;
    status_="TEXTURE_PREFLIGHT";
    EngineTextureScope scope(api_,map,mapUser);
    if(!scope.begin())return false;
    struct Reset {Active& slot;~Reset(){slot={};}} reset{active_};
    active_={this,&scope};
    bool result{};
    try{result=body(bodyUser);}catch(...){
        // Disable routing before replay; original APIs may call other texture
        // entry points. Never let restoration itself become a new eye binding.
        active_={};scope.finish();throw;
    }
    const int saved=errno;active_={};
    const bool restored=scope.finish();status_=restored?(result?"READY":"BODY_DECLINED"):scope.status();errno=saved;
    return result && restored;
}
void TextureCallBoundary::global(int id,const void* texture) {
    if(active_.boundary==this)active_.scope->global(id,texture);
    else api_.global(id,texture);
}
void TextureCallBoundary::instance(const void* effect,void* data,int id,const void* texture,bool force,bool unbind) {
    if(active_.boundary==this)active_.scope->instance(effect,data,id,texture,force,unbind);
    else api_.instance(effect,data,id,texture,force,unbind);
}
}
