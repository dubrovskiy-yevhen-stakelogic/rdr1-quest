#pragma once
#include "diagnostics/DrawStageTimings.h"
#include "NativeDrawSuppression.h"
#include "BodyDrawRecordReader.h"
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace rdrvr::engine {
// Match the local actor's per-instance crCreature, never a drawable/asset that
// can be shared by NPCs. This does not write native visibility or animation.
class BodyDrawFilter {
public:
    using Original=void(*)(void*,const void*,int,int,float,unsigned,unsigned);
    using Reader=bool(*)(void*,std::uintptr_t,void*,std::size_t);
private:
    Original original_{};Reader read_{};bool plain_{};
    struct State{BodyDrawFilter* owner{};std::uintptr_t creature{},hat{};bool failed{},preserveUpdates{};std::uint64_t inspected{},hidden{},hats{},errors{};};
    // The active eye lives in diagnostics::drawThreadContext (one TLS resolution per
    // draw() for this filter, the draw suppression scope and the stage counters).
    static State* active()noexcept{return static_cast<State*>(diagnostics::drawThreadContext.bodyFilter);}
    static void activate(State* s)noexcept{diagnostics::drawThreadContext.bodyFilter=s;}
    std::atomic<std::uint64_t> inspected_{},hidden_{},hats_{},errors_{};
    std::atomic<std::uintptr_t> globalHat_{};std::atomic<std::uint64_t> globalHats_{};
    // Probe: the distinct renDrawBucketInfo nodes (+0x88) drawn for the player's crCreature (+0x98) outside
    // the eye scopes (shadow maps). 1 = the head is part of the body's single draw record (no per-pass head filtering possible); >1 = the
    // player is drawn as several nodes (head / hair could be filtered per pass). Read-only, errno preserved.
    std::atomic<std::uintptr_t> probeCreature_{};std::array<std::atomic<std::uintptr_t>,8> probeNodes_{};std::atomic<std::uint64_t> probeRecords_{},probeOverflow_{};
public:
    // HEAD SHADOW: the player's skin matrices are left
    // UNCOLLAPSED by the skinning task (shadow maps / reflections get the head); inside the eye scopes the player's draw record binds a
    // copy whose head rows are collapsed. grmMatrixSet (info+0x90): current d11::ConstantBuffer_c at +0x68 (CPU data +0, upload stamp
    // +0x24), previous at +0xa0; VkContext_c::BindVertexConstants copies *(cb+0) into the frame's uniform ring only when the stamp is not
    // this frame's, so for the eye draw the data pointers point at the scratch and the stamps are forced stale; both 0x38-byte buffer
    // objects are restored bit-exact after the original (also on exceptions). The right eye replays the left draw's bone ring offset.
    // fill(set, scratch, capacityFloats) -> n > 0: rows (current n then previous n, 12 floats each) with the head collapsed, n = bone count;
    // -1: draw unchanged (this set was collapsed by the skinning task itself: fallback mode); 0: no data for this set (miss) -> the player
    // record is HIDDEN for that eye draw (never the head in the eye).
    using HeadFill=std::int32_t(*)(std::uintptr_t set,float* scratch,std::size_t capacityFloats) noexcept;
    static constexpr std::size_t setInfoOffset=0x90,cbCurrent=0x68,cbPrevious=0xa0,cbSize=0x38,cbStamp=0x24,setRows=0xdc,rowFloats=12;
    void setHeadFill(HeadFill fill)noexcept{headFill_.store(fill,std::memory_order_release);}
    bool headFillActive()const noexcept{return headFill_.load(std::memory_order_relaxed)!=nullptr;}
    std::uint64_t headSwaps()const noexcept{return headSwaps_.load(std::memory_order_relaxed);}
    std::uint64_t headMisses()const noexcept{return headMisses_.load(std::memory_order_relaxed);}
    std::uint64_t headAsIs()const noexcept{return headAsIs_.load(std::memory_order_relaxed);}
private:
    std::atomic<HeadFill> headFill_{};std::atomic<std::uint64_t> headSwaps_{},headMisses_{},headAsIs_{};
    // RAII: data pointers -> scratch, stamps stale, original, both buffer objects restored.
    struct BoneSwap {
        unsigned char* cur{};unsigned char* prev{};alignas(8) unsigned char savedCur[cbSize]{},savedPrev[cbSize]{};bool on{};
        BoneSwap(std::uintptr_t set,float* scratch,std::uint32_t n)noexcept{
            cur=reinterpret_cast<unsigned char*>(set+cbCurrent);prev=reinterpret_cast<unsigned char*>(set+cbPrevious);
            std::memcpy(savedCur,cur,cbSize);std::memcpy(savedPrev,prev,cbSize);
            float* c=scratch;float* p=scratch+std::size_t(n)*rowFloats;const std::uint32_t stale=0xffffffffu;
            std::memcpy(cur,&c,sizeof c);std::memcpy(prev,&p,sizeof p);std::memcpy(cur+cbStamp,&stale,4);std::memcpy(prev+cbStamp,&stale,4);on=true;}
        ~BoneSwap(){if(on){std::memcpy(cur,savedCur,cbSize);std::memcpy(prev,savedPrev,cbSize);}}
        BoneSwap(const BoneSwap&)=delete;BoneSwap& operator=(const BoneSwap&)=delete;
    };
public:
    class Eye {
        State state_;State* previous_;
    public:
        Eye(BodyDrawFilter& owner,std::uintptr_t creature,std::uintptr_t hat=0,bool preserveUpdates=false):state_{&owner,creature,hat,!creature||(preserveUpdates&&!nativeDrawSuppression.ready()),preserveUpdates},previous_(active()){
            if(previous_){previous_->failed=true;state_.failed=true;}activate(&state_);
        }
        ~Eye(){activate(previous_);auto& o=*state_.owner;
            o.inspected_.fetch_add(state_.inspected,std::memory_order_relaxed);o.hidden_.fetch_add(state_.hidden,std::memory_order_relaxed);
            o.hats_.fetch_add(state_.hats,std::memory_order_relaxed);o.errors_.fetch_add(state_.errors,std::memory_order_relaxed);}
        Eye(const Eye&)=delete;Eye& operator=(const Eye&)=delete;
        bool good()const{return !state_.failed;}
    };
    // The mod's record reader (a plain copy of the synchronous renDrawBucketInfo
    // argument) is used inline: no indirect call and no errno save/restore per object.
    void configure(Original original,Reader read){original_=original;read_=read;plain_=read==&readBodyDrawRecordField;}
    // The visible-object paths of draw() (also used while the head-collapsed matrix set is bound).
    void callVisible(State* s,void* container,const void* info,int commands,int instance,float fade,unsigned flags,unsigned extra){
        if(s&&s->owner==this&&s->preserveUpdates&&!nativeDrawSuppression.anyHidden()){
            try{original_(container,info,commands,instance,fade,flags,extra);}catch(...){s->failed=true;throw;}
        }else if(s&&s->owner==this&&s->preserveUpdates){
            NativeDrawSuppression::Scope geometry(nativeDrawSuppression,false);
            try{original_(container,info,commands,instance,fade,flags,extra);}catch(...){s->failed=true;throw;}
            if(!geometry.good()){s->failed=true;++s->errors;}
        }else original_(container,info,commands,instance,fade,flags,extra);
    }
    void draw(void* container,const void* info,int commands,int instance,float fade,unsigned flags,unsigned extra,bool pinned){
        auto* s=active();bool hide=false;
        // BODY: outside the eye scopes (shadow maps, ...) the hat node is hidden while the player's
        // head is collapsed for the eyes - the skin matrices are shared by every pass, so the shadow
        // has no head either and would otherwise show a lone hat.
        if(!(s&&s->owner==this)){
            const auto pc=probeCreature_.load(std::memory_order_relaxed);
            if(pc&&pinned&&read_&&info){const auto p=reinterpret_cast<std::uintptr_t>(info);
                if(p<=UINTPTR_MAX-0x98-sizeof(std::uintptr_t)){const int incoming=errno;std::uintptr_t creature{},node{};bool ok=false;
                    if(plain_){std::memcpy(&creature,reinterpret_cast<const void*>(p+0x98),sizeof creature);std::memcpy(&node,reinterpret_cast<const void*>(p+0x88),sizeof node);ok=true;}
                    else ok=read_(nullptr,p+0x98,&creature,sizeof creature)&&read_(nullptr,p+0x88,&node,sizeof node);
                    errno=incoming;
                    if(ok&&creature==pc){probeRecords_.fetch_add(1,std::memory_order_relaxed);bool placed=false;
                        for(auto& slot:probeNodes_){auto v=slot.load(std::memory_order_relaxed);if(v==node){placed=true;break;}
                            if(!v&&slot.compare_exchange_strong(v,node,std::memory_order_relaxed)){placed=true;break;}if(v==node){placed=true;break;}}
                        if(!placed)probeOverflow_.fetch_add(1,std::memory_order_relaxed);}}}
            const auto hat=globalHat_.load(std::memory_order_relaxed);
            if(hat&&pinned&&read_&&info){std::uintptr_t node{};const auto p=reinterpret_cast<std::uintptr_t>(info);
                const int incoming=errno;
                bool ok=false;
                if(p<=UINTPTR_MAX-0x88-sizeof(node)){
                    if(plain_){std::memcpy(&node,reinterpret_cast<const void*>(p+0x88),sizeof node);ok=true;}
                    else ok=read_(nullptr,p+0x88,&node,sizeof node);}
                errno=incoming;
                if(ok&&node==hat){hide=true;globalHats_.fetch_add(1,std::memory_order_relaxed);}
            }
        }
        std::uintptr_t swapSet{};std::uint32_t swapN{};
        alignas(16) static thread_local float swapScratch[255*2*rowFloats];
        if(s&&s->owner==this){
            const bool plain=plain_;int incoming{};if(!plain)incoming=errno;
            const auto field=[&](std::uintptr_t address,std::uintptr_t& out){
                if(plain){std::memcpy(&out,reinterpret_cast<const void*>(address),sizeof out);return true;}
                return read_(nullptr,address,&out,sizeof out);};
            try{
                std::uintptr_t creature{};const auto p=reinterpret_cast<std::uintptr_t>(info);
                if(s->failed||!pinned||!read_||!p||p>UINTPTR_MAX-0x98-sizeof(creature)||
                   !field(p+0x98,creature)){s->failed=true;++s->errors;}
                else{
                    ++s->inspected;hide=creature&&creature==s->creature;
                    if(!hide&&s->hat){std::uintptr_t node{};
                        if(!field(p+0x88,node)){s->failed=true;++s->errors;}
                        else if(node==s->hat){hide=true;++s->hats;}
                    }
                    // HEAD SHADOW: the player's own record (the probe creature) binds a head-collapsed copy of its matrix set
                    const auto fill=headFill_.load(std::memory_order_acquire);const auto pc=probeCreature_.load(std::memory_order_relaxed);
                    if(!hide&&fill&&pc&&creature==pc&&!s->failed){std::uintptr_t set{};
                        std::int32_t got=0;
                        if(p<=UINTPTR_MAX-setInfoOffset-sizeof(set)&&field(p+setInfoOffset,set)&&set)got=fill(set,swapScratch,sizeof swapScratch/sizeof(float));
                        if(got>0){swapSet=set;swapN=std::uint32_t(got);headSwaps_.fetch_add(1,std::memory_order_relaxed);}
                        else if(got<0)headAsIs_.fetch_add(1,std::memory_order_relaxed);
                        else{hide=true;headMisses_.fetch_add(1,std::memory_order_relaxed);} // fail closed: never the head in the eye
                    }
                    if(hide)++s->hidden;
                }
            }catch(...){s->failed=true;++s->errors;}
            if(!plain)errno=incoming;
        }
        diagnostics::DrawStageTimings::object(hide);
        if(swapSet&&!hide){BoneSwap swap(swapSet,swapScratch,swapN);callVisible(s,container,info,commands,instance,fade,flags,extra);return;}
        if(s&&s->owner==this&&s->preserveUpdates&&!hide&&!nativeDrawSuppression.anyHidden()){
            // Visible object and no hiding scope live on any thread: the visible scope
            // below would only clear an inherited suppression that cannot exist.
            try{original_(container,info,commands,instance,fade,flags,extra);}
            catch(...){s->failed=true;throw;}
        }else if(s&&s->owner==this&&s->preserveUpdates){
            // Primary native rendering owns required PreDraw/DrawUpdate and
            // skinning task retirement. Run the complete original even for the
            // hidden actor; only its final primitive calls are suppressed.
            // A visible nested DrawCommon suspends its hidden parent's scope.
            NativeDrawSuppression::Scope geometry(nativeDrawSuppression,hide);
            try{original_(container,info,commands,instance,fade,flags,extra);}
            catch(...){s->failed=true;throw;}
            if(!geometry.good()){s->failed=true;++s->errors;}
        }else if(!hide)original_(container,info,commands,instance,fade,flags,extra);
    }
    std::uint64_t inspected()const{return inspected_.load();}
    std::uint64_t hidden()const{return hidden_.load();}
    std::uint64_t hats()const{return hats_.load();}
    std::uint64_t errors()const{return errors_.load();}
    // BODY: the hat node hidden in every pass outside the eye scopes (0 = off). Set by GameXr per frame from the camera plan.
    void setGlobalHat(std::uintptr_t hat)noexcept{globalHat_.store(hat,std::memory_order_relaxed);}
    std::uintptr_t globalHat()const noexcept{return globalHat_.load(std::memory_order_relaxed);}
    std::uint64_t globalHats()const{return globalHats_.load();}
    // Probe: the player's crCreature (0 = off; a new creature restarts the node set)
    void setProbeCreature(std::uintptr_t c)noexcept{if(probeCreature_.exchange(c,std::memory_order_relaxed)!=c){for(auto& n:probeNodes_)n.store(0,std::memory_order_relaxed);
        probeRecords_.store(0,std::memory_order_relaxed);probeOverflow_.store(0,std::memory_order_relaxed);}}
    unsigned probeNodes()const noexcept{unsigned n=0;for(const auto& v:probeNodes_)if(v.load(std::memory_order_relaxed))++n;return n;}
    std::uint64_t probeRecords()const noexcept{return probeRecords_.load(std::memory_order_relaxed);}
    std::uint64_t probeOverflow()const noexcept{return probeOverflow_.load(std::memory_order_relaxed);}
};
}
