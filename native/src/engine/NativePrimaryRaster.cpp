#include "NativePrimaryRaster.h"
#include "diagnostics/GuardedReadVector.h"
#include <cerrno>
#include <cmath>
#include <cstring>
#include <sstream>

namespace rdrvr::engine {
namespace {
struct Errno {int saved{errno};~Errno(){errno=saved;}};
template<class T> bool read(const PassSourceAbi& m,const void* p,std::size_t offset,T& out){
    const auto a=reinterpret_cast<std::uintptr_t>(p);
    return a&&offset<=UINTPTR_MAX-a&&sizeof(T)<=UINTPTR_MAX-a-offset&&m.read(m.context,a+offset,&out,sizeof(out));
}
template<class T> bool global(const PassSourceAbi& m,std::uintptr_t p,T& out){return read(m,reinterpret_cast<const void*>(p),0,out);}
template<class T,std::size_t N> T field(const std::array<std::byte,N>& d,std::size_t off){T out{};std::memcpy(&out,d.data()+off,sizeof(out));return out;}
bool equal(const EngineRasterViewport& a,const EngineRasterViewport& b){return a.left==b.left&&a.right==b.right&&a.top==b.top&&a.bottom==b.bottom&&a.minDepth==b.minDepth&&a.maxDepth==b.maxDepth;}
bool equal(const EngineRasterScissor& a,const EngineRasterScissor& b){return a.left==b.left&&a.right==b.right&&a.top==b.top&&a.bottom==b.bottom;}
bool raster(const PassSourceAbi& m,void* gfx,void* context,EngineRasterViewport& v,EngineRasterScissor& s){
    std::array<std::byte,40> native{};
    if(!read(m,gfx,0x130,v)||!read(m,gfx,0x148,s)||!read(m,context,0x3440,native))return false;
    const auto clip=field<VkRect2D>(native,0);const auto vp=field<VkViewport>(native,16);
    return v.left>=0&&v.right>v.left&&v.top>=0&&v.bottom>v.top&&s.left>=0&&s.top>=0&&s.right>=s.left&&s.bottom>=s.top&&
        std::isfinite(v.minDepth)&&std::isfinite(v.maxDepth)&&v.minDepth>=0&&v.minDepth<=1&&v.maxDepth>=0&&v.maxDepth<=1&&
        vp.x==float(v.left)&&vp.y==float(v.bottom)&&vp.width==float(std::int64_t(v.right)-v.left)&&vp.height==float(std::int64_t(v.top)-v.bottom)&&
        vp.minDepth==v.minDepth&&vp.maxDepth==v.maxDepth&&clip.offset.x==s.left&&clip.offset.y==s.top&&
        clip.extent.width==std::uint32_t(std::int64_t(s.right)-s.left)&&clip.extent.height==std::uint32_t(std::int64_t(s.bottom)-s.top);
}
}
void NativePrimaryRaster::fail(const char* why) noexcept {if(!failed_)++faults_;failed_=true;status_=why;}
void NativePrimaryRaster::forget() noexcept {
    active_=false;renderer_=gfx_=context_=collector_=postfx_=lights_=depth_=depthSurface_=nullptr;
    targets_={};surfaces_={};owner_={};
}
void NativePrimaryRaster::configure(Api api) noexcept {
    Errno preserve;std::lock_guard lock(mutex_);
    if(active_){fail("CONFIGURE_ACTIVE");return;}api_=api;failed_=false;status_="CONFIGURED";
}
// Every identity is re-read at the address the verified chain must use (the
// expected pointers recorded by begin()) in ONE vectored guarded read; the result is
// true exactly when the step-by-step chain below would be true. Reads through a stale
// expected pointer stay kernel-checked (or inside the engine's always-mapped segments).
bool NativePrimaryRaster::identities() const {
    const auto& a=api_.resources;const auto& m=a.memory;
    if(diagnostics::guardedReadVector.vectored(m.read)){
        void* gfx{};
        if(owner_!=std::this_thread::get_id()||!a.ownsThread()||!a.currentGfx(&gfx)||gfx!=gfx_)return false;
        constexpr unsigned n=18;
        std::array<std::uintptr_t,n> got{},want{};std::array<diagnostics::ReadSegment,n> s{};unsigned k=0;
        bool ok=true;
        const auto add=[&](const void* base,std::size_t offset,std::uintptr_t expected){
            const auto p=reinterpret_cast<std::uintptr_t>(base);
            if(!p||offset>UINTPTR_MAX-p||sizeof(std::uintptr_t)>UINTPTR_MAX-p-offset)ok=false;
            s[k]={p+offset,&got[k],sizeof(std::uintptr_t)};want[k]=expected;++k;};
        const auto id=[](const void* p){return reinterpret_cast<std::uintptr_t>(p);};
        add(reinterpret_cast<const void*>(a.collector),0,id(collector_));add(reinterpret_cast<const void*>(a.postfx),0,id(postfx_));
        add(reinterpret_cast<const void*>(a.lights),0,id(lights_));
        add(gfx_,0,api_.framebuffer.gfxVtable);add(gfx_,0x128,id(context_));add(context_,0,m.contextVtable);
        add(collector_,0x48,id(depth_));add(depth_,0,m.targetVtable);add(depth_,0xa0,id(depthSurface_));
        const std::array owners{postfx_,collector_,lights_};constexpr std::array<std::size_t,3> offsets{0x10,0x40,0x878};
        for(unsigned i=0;i<3;++i){add(owners[i],offsets[i],id(targets_[i]));add(targets_[i],0,m.targetVtable);add(targets_[i],0x70,id(surfaces_[i]));}
        if(!ok||k!=n||!diagnostics::guardedReadVector.read(m.read,m.context,s.data(),n))return false;
        return got==want;
    }
    void *collector{},*postfx{},*lights{},*gfx{},*context{},*depth{},*surface{};std::uintptr_t table{};
    if(owner_!=std::this_thread::get_id()||!a.ownsThread()||!a.currentGfx(&gfx)||gfx!=gfx_||
       !global(m,a.collector,collector)||collector!=collector_||!global(m,a.postfx,postfx)||postfx!=postfx_||
       !global(m,a.lights,lights)||lights!=lights_||!read(m,gfx,0,table)||table!=api_.framebuffer.gfxVtable||
       !read(m,gfx,0x128,context)||context!=context_||!read(m,context,0,table)||table!=m.contextVtable||
       !read(m,collector,0x48,depth)||depth!=depth_||!read(m,depth,0,table)||table!=m.targetVtable||
       !read(m,depth,0xa0,surface)||surface!=depthSurface_)return false;
    const std::array owners{postfx,collector,lights};constexpr std::array<std::size_t,3> offsets{0x10,0x40,0x878};
    for(unsigned i=0;i<3;++i){void* target{};
        if(!read(m,owners[i],offsets[i],target)||target!=targets_[i]||!read(m,target,0,table)||table!=m.targetVtable||
           !read(m,target,0x70,surface)||surface!=surfaces_[i])return false;
    }
    return true;
}
bool NativePrimaryRaster::begin(void* renderer,std::uint32_t width,std::uint32_t height) noexcept {
    Errno preserve;std::lock_guard lock(mutex_);
    if(active_){fail("NESTED_BEGIN");return false;}if(failed_)return false;
    status_="BEGIN_PREFLIGHT";
    try{
        const auto& a=api_.resources;const auto& f=api_.framebuffer;const auto& m=a.memory;
        if(!f||!a||!renderer||!width||!height||width>8192||height>8192||!a.ownsThread()||!f.ownsThread()){++declined_;return false;}
        owner_=std::this_thread::get_id();renderer_=renderer;width_=width;height_=height;
        if(!a.currentGfx(&gfx_)||!read(m,gfx_,0x128,context_)||!global(m,a.collector,collector_)||!collector_||
           !global(m,a.postfx,postfx_)||!postfx_||!global(m,a.lights,lights_)||!lights_||
           !read(m,collector_,0x48,depth_)||!depth_||!read(m,depth_,0xa0,depthSurface_)||!depthSurface_)throw 0;
        const std::array owners{postfx_,collector_,lights_};constexpr std::array<std::size_t,3> offsets{0x10,0x40,0x878};
        for(unsigned i=0;i<3;++i){
            if(!read(m,owners[i],offsets[i],targets_[i])||!targets_[i]||!read(m,targets_[i],0x70,surfaces_[i])||!surfaces_[i])throw 0;
            for(unsigned j=0;j<i;++j)if(targets_[i]==targets_[j]||surfaces_[i]==surfaces_[j])throw 0;
        }
        if(!identities())throw 0;
        // Pinned SetFrameBuffer at 1ac3040 reads surface +18/+1c for dimensions.
        for(unsigned i=0;i<4;++i){std::array<std::byte,0x38> surface{};void* ptr=i<3?surfaces_[i]:depthSurface_;
            if(!read(m,ptr,0,surface)||field<std::uintptr_t>(surface,0)!=(i<3?f.colorVtable:f.depthVtable)||
               !field<std::uint32_t>(surface,0xc)||field<std::uint32_t>(surface,0x18)!=width||field<std::uint32_t>(surface,0x1c)!=height||
               !field<std::uintptr_t>(surface,0x30))throw 0;
        }
        active_=true;++begins_;status_="ACTIVE";return true;
    }catch(...){
        // No native write has happened. Unavailable targets may decline primary
        // reuse without quarantining otherwise valid replay or future frames.
        ++declined_;status_="BEGIN_IDENTITIES_UNAVAILABLE";forget();return false;
    }
}
void NativePrimaryRaster::bound(void* gfx,void* requested,bool nativeResult) noexcept {
    Errno preserve;std::lock_guard lock(mutex_);if(!active_||failed_)return;++bounds_;
    try{
        if(owner_!=std::this_thread::get_id()){fail("BOUND_OWNER");return;}
        if(!requested||gfx!=gfx_){++ignored_;return;}
        const auto& f=api_.framebuffer;const auto& m=api_.resources.memory;
        std::array<std::byte,0x100> fb{};
        if(!read(m,requested,0,fb)||field<std::uintptr_t>(fb,0)!=f.framebufferVtable){++ignored_;return;}
        auto* color=field<void*>(fb,0x20);unsigned role=3;
        for(unsigned i=0;i<3;++i)if(color==surfaces_[i])role=i;
        if(role==3){++ignored_;return;}
        void* actual{};std::uint32_t id{};
        // HUD routing can substitute another framebuffer. It owns that result;
        // an original requested native target does not grant permission to edit it.
        if(!nativeResult||!read(m,gfx,0x158,actual)||actual!=requested){++ignored_;return;}
        ++matched_;++roleMatches_[role];
        if(!identities()||!read(m,gfx,0x160,id)||id!=field<std::uint32_t>(fb,0x10)||
           !field<std::uint32_t>(fb,0xc)||field<std::uint32_t>(fb,0xf8)!=width_||field<std::uint32_t>(fb,0xfc)!=height_||
           field<void*>(fb,0x18)!=depthSurface_){fail("FRAMEBUFFER_IDENTITY");return;}
        for(unsigned i=1;i<8;++i)if(field<void*>(fb,0x20+i*8)){fail("EXTRA_COLOR_ATTACHMENT");return;}
        for(unsigned i=0;i<2;++i){
            std::array<std::byte,0x38> surface{};void* texture{};
            const auto* target=i?depth_:targets_[role];const auto* ptr=i?depthSurface_:surfaces_[role];
            if(!read(m,ptr,0,surface)||!read(m,target,0x60,texture)||!texture||
               field<std::uintptr_t>(surface,0)!=(i?f.depthVtable:f.colorVtable)||
               !field<std::uint32_t>(surface,0xc)||field<std::uint32_t>(surface,0x18)!=width_||
               field<std::uint32_t>(surface,0x1c)!=height_||field<void*>(surface,0x30)!=texture){fail("SURFACE_CHANGED");return;}
        }
        EngineRasterViewport before{},after{};EngineRasterScissor beforeClip{},afterClip{};
        if(!raster(m,gfx_,context_,before,beforeClip)){fail("RASTER_READ");return;}
        const EngineRasterViewport wanted{0,static_cast<std::int32_t>(width_),0,static_cast<std::int32_t>(height_),before.minDepth,before.maxDepth};
        const EngineRasterScissor clip{0,static_cast<std::int32_t>(width_),0,static_cast<std::int32_t>(height_)};
        const bool changeViewport=!equal(before,wanted),changeScissor=!equal(beforeClip,clip);
        if(changeViewport)f.viewport(gfx_,wanted);
        if(changeScissor)f.scissor(gfx_,clip);
        if(!raster(m,gfx_,context_,after,afterClip)||!equal(after,wanted)||!equal(afterClip,clip)){fail("RASTER_WRITE");return;}
        if(changeViewport||changeScissor){
            ++corrected_;++roleCorrections_[role];lastRole_=role;
            beforeViewport_=before;beforeScissor_=beforeClip;afterViewport_=after;afterScissor_=afterClip;
        }else ++unchanged_;
        status_="ACTIVE";
    }catch(...){fail("BOUND_EXCEPTION");}
}
bool NativePrimaryRaster::end() noexcept {
    Errno preserve;std::lock_guard lock(mutex_);
    if(active_&&owner_!=std::this_thread::get_id())fail("END_OWNER");
    forget();if(!failed_)status_="IDLE";return !failed_;
}
bool NativePrimaryRaster::good() const noexcept {std::lock_guard lock(mutex_);return !failed_;}
std::string NativePrimaryRaster::report() const {
    std::lock_guard lock(mutex_);std::ostringstream o;
    o<<"{\"state\":\""<<status_<<"\",\"active\":"<<active_<<",\"width\":"<<width_<<",\"height\":"<<height_
     <<",\"begins\":"<<begins_<<",\"declined\":"<<declined_<<",\"bounds\":"<<bounds_<<",\"matched\":"<<matched_<<",\"corrected\":"<<corrected_
     <<",\"unchanged\":"<<unchanged_<<",\"ignored\":"<<ignored_<<",\"faults\":"<<faults_<<",\"lastCorrectedRole\":"<<lastRole_;
    auto vp=[&](const char* name,const EngineRasterViewport& v){o<<",\""<<name<<"\":["<<v.left<<','<<v.right<<','<<v.top<<','<<v.bottom<<']';};
    auto sc=[&](const char* name,const EngineRasterScissor& v){o<<",\""<<name<<"\":["<<v.left<<','<<v.right<<','<<v.top<<','<<v.bottom<<']';};
    vp("beforeViewport",beforeViewport_);vp("afterViewport",afterViewport_);sc("beforeScissor",beforeScissor_);sc("afterScissor",afterScissor_);
    o<<",\"roleMatches\":["<<roleMatches_[0]<<','<<roleMatches_[1]<<','<<roleMatches_[2]<<"],\"roleCorrections\":["<<roleCorrections_[0]<<','<<roleCorrections_[1]<<','<<roleCorrections_[2]<<"]}";
    return o.str();
}
}
