#pragma once
#include "EngineTextureScope.h"
namespace rdrvr::engine {
// Only a synchronous audited eye body may enable argument substitution. The
// thread-local pointer never escapes invoke, including exception unwinding.
// All other engine callers use the original functions with their exact args.
class TextureCallBoundary {
    struct Active {TextureCallBoundary* boundary;EngineTextureScope* scope;};
    static thread_local Active active_;
    EngineTextureApi api_{};
    const char* status_{"NOT_INVOKED"};
public:
    using Body=bool(*)(void*);
    static bool busy(){return active_.boundary!=nullptr;}
    void configure(EngineTextureApi api) {api_=api;} // before publishing hooks
    const char* status()const{return status_;}
    void resetReport(){if(!busy())status_="NOT_INVOKED";}
    bool invoke(EngineTextureScope::Map,void* mapUser,Body,void* bodyUser);
    void global(int,const void*);
    void instance(const void*,void*,int,const void*,bool,bool);
};
}
