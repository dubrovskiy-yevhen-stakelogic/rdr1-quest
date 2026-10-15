#pragma once
#include "HudAlphaFlush.h"
#include <atomic>
namespace rdrvr::engine {
struct HudRoutingApi {
    using Bind=bool(*)(void*,void*);
    HudAlphaApi alpha{};Bind bind{};
    bool (*current)(void*,void*,void**){};
    bool (*resolveNull)(void*,void*,void**){};
    void* context{};
};
// The native pass owns all framebuffer references. This thread-local route
// borrows them only between begin/end and redirects returns to that exact
// framebuffer; unrelated temporary targets retain their original blend state.
class HudDrawRouting {
    HudRoutingApi api_;HudAlphaFlush alpha_;
    void *gfx_{},*stock_{},*hud_{};bool failed_{},restoring_{};
    inline static thread_local HudDrawRouting* active_{};
    // Routings currently begun on any thread. 0 => active_ is null on every thread.
    inline static std::atomic<unsigned> live_{};
    struct Errno {int saved{errno};~Errno(){errno=saved;}};
    bool current(void*& result) noexcept {
        try{return api_.current(api_.context,gfx_,&result);}catch(...){return false;}
    }
public:
    struct Counts {unsigned normalized{},masks{},temporary{},mapped{},unsupported{},readFailures{};} counts;
    explicit HudDrawRouting(HudRoutingApi api):api_(api),alpha_(api.alpha){}
    ~HudDrawRouting(){end();}
    HudDrawRouting(const HudDrawRouting&)=delete;
    HudDrawRouting& operator=(const HudDrawRouting&)=delete;
    bool begin(void* gfx,void* stock,void* hud) noexcept {
        Errno preserve;
        if(active_||gfx_||!gfx||!stock||!hud||stock==hud||alpha_.poisoned()||
           !api_.alpha.read||!api_.alpha.set||!api_.alpha.flush||!api_.bind||!api_.current||!api_.resolveNull)return false;
        gfx_=gfx;void* bound{};
        if(!current(bound)||bound!=hud){gfx_=nullptr;return false;}
        stock_=stock;hud_=hud;failed_=false;counts={};live_.fetch_add(1,std::memory_order_relaxed);active_=this;return true;
    }
    bool end() noexcept {
        Errno preserve;
        if(!gfx_)return !failed_&&!alpha_.poisoned();
        if(active_!=this)return false;
        void* bound{};if(!current(bound)||bound!=hud_)failed_=true;
        // Requested state was restored after each flush, but the corrected
        // native blend object is still bound. Flush the unmodified request once
        // before returning to stock rendering, including on abort/destruction.
        active_=nullptr;live_.fetch_sub(1,std::memory_order_relaxed);restoring_=true;
        try{api_.alpha.flush();}catch(...){failed_=true;}
        restoring_=false;gfx_=stock_=hud_=nullptr;return !failed_&&!alpha_.poisoned();
    }
    bool good() const {return !failed_&&!alpha_.poisoned();}
    bool active() const {return active_==this;}
    static bool live() noexcept {return live_.load(std::memory_order_relaxed)!=0;}
    static bool bind(HudRoutingApi::Bind original,void* gfx,void* requested){
        if(!live())return original(gfx,requested);
        auto* s=active_;
        if(!s||s->api_.bind!=original||s->gfx_!=gfx||s->restoring_)return original(gfx,requested);
        const int incoming=errno;void* target=requested;
        if(!requested){
            bool known=false;try{known=s->api_.resolveNull(s->api_.context,gfx,&target);}catch(...){}
            if(!known){s->failed_=true;target=nullptr;++s->counts.readFailures;}
        }
        if(target==s->stock_){target=s->hud_;++s->counts.mapped;}
        // Preserve nullptr's native resolution unless it resolves to the mapped
        // stock framebuffer. Avoid replacing an unrelated default framebuffer.
        void* argument=target==s->hud_?target:requested;
        errno=incoming;bool result{};
        try{result=original(gfx,argument);}catch(...){s->failed_=true;throw;}
        const int outgoing=errno;void* actual{};
        if(!result||!s->current(actual)||actual!=target)s->failed_=true;
        errno=outgoing;return result;
    }
    static void flush(void(*original)(),bool colorDraw=true){
        if(!live()){original();return;}
        auto* s=active_;
        if(!colorDraw||!s||s->api_.alpha.flush!=original||s->restoring_){original();return;}
        const int incoming=errno;void* bound{};const bool readable=s->current(bound);errno=incoming;
        if(!readable){s->failed_=true;++s->counts.readFailures;original();return;}
        if(bound!=s->hud_){++s->counts.temporary;original();return;}
        try{
            const auto result=s->alpha_.invoke(true);
            if(result==HudAlphaResult::Normalized)++s->counts.normalized;
            else if(result==HudAlphaResult::MaskOnly)++s->counts.masks;
            else {s->failed_=true;++s->counts.unsupported;}
        }catch(...){s->failed_=true;throw;}
    }
};
}
