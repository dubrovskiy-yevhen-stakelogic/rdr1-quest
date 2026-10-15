#include "EngineFramebuffers.h"
#include <cerrno>
#include <cstring>
#include <limits>
#include <cmath>

namespace rdrvr::engine {
namespace {
struct Errno {int value{errno};~Errno(){errno=value;}};
template<class T>bool read(const EngineFramebufferApi& api,const void* p,std::size_t offset,T& out) {
    const auto a=reinterpret_cast<std::uintptr_t>(p);
    return a && !(a%8) && offset<=std::numeric_limits<std::uintptr_t>::max()-a &&
        sizeof(T)-1<=std::numeric_limits<std::uintptr_t>::max()-(a+offset) && api.memory.read(api.memory.context,a+offset,&out,sizeof(T));
}
bool table(const EngineFramebufferApi& api,const void* p,std::uintptr_t expected) {std::uintptr_t v{};return expected && read(api,p,0,v) && v==expected;}
template<class T,std::size_t N>T field(const std::array<std::byte,N>& bytes,std::size_t offset){
    T value{};std::memcpy(&value,bytes.data()+offset,sizeof(value));return value;
}
bool surface(const EngineFramebufferApi& api,void* p,std::uintptr_t vtable,std::uintptr_t texture){
    std::array<std::byte,0x38> bytes{};
    return vtable && read(api,p,0,bytes) && field<std::uintptr_t>(bytes,0)==vtable &&
        field<std::uintptr_t>(bytes,0x30)==texture;
}
template<class T>void put(FramebufferDesc& desc,std::size_t offset,const T& value){std::memcpy(desc.bytes.data()+offset,&value,sizeof(value));}
bool dirty(const EngineFramebufferApi& api){std::uint8_t value{};return !api.memory.read(api.memory.context,api.factoryDirty,&value,1) || value!=0;}
unsigned bufferEye(unsigned i){return i<2?i:(i<6?(i-2)/2:(i-6)/2);}
EngineTargetRole bufferRole(unsigned i){return i<2?EngineTargetRole::Color:(i>=6?
    (i%2?EngineTargetRole::Refraction:EngineTargetRole::WaterDepth):
    (i%2?EngineTargetRole::LightAccumulation:EngineTargetRole::ShadowCollector));}
bool withDepth(EngineTargetRole role){return role!=EngineTargetRole::Refraction;}
}
NativeBindingFlush flushPendingNativeBinding(const EngineFramebufferApi& api,CommandResources& commands,VkDevice device,
                                            std::uint32_t family,bool (*currentGfx)(void**),void (*factoryFlush)()) noexcept {
    Errno preserve;
    try{
        if(!api||!api.ownsThread()||!currentGfx||!factoryFlush)return NativeBindingFlush::Unavailable;
        std::uint8_t pending{};
        if(!api.memory.read(api.memory.context,api.factoryDirty,&pending,1)||pending>1)return NativeBindingFlush::Unavailable;
        if(!pending)return NativeBindingFlush::NotPending;
        void *gfx{},*context{},*buffer{},*wrapper{};VkCommandBuffer command{};CommandRecord before{},after{};std::uint32_t refs{};
        if(!currentGfx(&gfx)||!table(api,gfx,api.gfxVtable)||!read(api,gfx,0x128,context)||!table(api,context,api.memory.contextVtable)||
           !read(api,gfx,0x158,buffer)||!table(api,buffer,api.framebufferVtable)||!read(api,buffer,0xc,refs)||!refs||refs>=0x7fffffff||
           !read(api,context,0x30a0,wrapper)||!read(api,wrapper,0x38,command)||!commands.recording(device,command,family,before))return NativeBindingFlush::Unavailable;
        factoryFlush();
        // Revalidate before the second call; native allocation/flush must not
        // switch to a different command buffer or context.
        void *gfxNow{},*contextNow{},*wrapperNow{};VkCommandBuffer commandNow{};
        if(!currentGfx(&gfxNow)||gfxNow!=gfx||!table(api,gfxNow,api.gfxVtable)||
           !read(api,gfxNow,0x128,contextNow)||contextNow!=context||!table(api,contextNow,api.memory.contextVtable)||
           !read(api,contextNow,0x30a0,wrapperNow)||!read(api,wrapperNow,0x38,commandNow)||commandNow!=command||
           !commands.recording(device,commandNow,family,after)||after.generation!=before.generation||after.recording!=before.recording)return NativeBindingFlush::Unavailable;
        api.flush(context);
        if(dirty(api))return NativeBindingFlush::Unavailable;
        return NativeBindingFlush::Flushed;
    }catch(...){return NativeBindingFlush::Unavailable;}
}
unsigned EngineFramebuffers::index(unsigned eye,EngineTargetRole role){
    if(eye>=2)return 10;
    switch(role){
    case EngineTargetRole::Color:return eye;
    case EngineTargetRole::ShadowCollector:return 2+eye*2;
    case EngineTargetRole::LightAccumulation:return 3+eye*2;
    case EngineTargetRole::WaterDepth:return 6+eye*2;
    case EngineTargetRole::Refraction:return 7+eye*2;
    default:return 10;
    }
}
bool EngineFramebuffers::owner() const {return owner_==std::this_thread::get_id() && api_.ownsThread();}
bool EngineFramebuffers::release(void*& p) noexcept {
    if(!p)return true;
    // Forget even an unreadable reference: its address could later be reused by
    // another engine object. Retrying a quarantined pointer is not a safe release.
    void* value=p;p=nullptr;
    try {
        std::uint32_t count{};
        if(!table(api_,value,api_.framebufferVtable) || !read(api_,value,0xc,count) || !count){poisoned_=true;return false;}
        // Null before calling: a failed/throwing release must never be repeated.
        FramebufferRef ref{value};api_.release(&ref);return true;
    }catch(...){poisoned_=true;return false;}
}
bool EngineFramebuffers::inspect(void* fb,unsigned index) const {
    if(index>=count())return false;
    const unsigned eye=bufferEye(index);const auto role=bufferRole(index);
    const auto* color=targets_.image(eye,role);const auto* depth=targets_.image(eye,true);const bool hasDepth=withDepth(role);
    // One fresh guarded copy per object, valid only inside this inspection.
    // No engine calls or retained snapshots cross the validation boundary.
    std::array<std::byte,0x100> bytes{};
    if(!color || !depth || !api_.framebufferVtable || !read(api_,fb,0,bytes) ||
       field<std::uintptr_t>(bytes,0)!=api_.framebufferVtable)return false;
    const auto width=field<std::uint32_t>(bytes,0xf8),height=field<std::uint32_t>(bytes,0xfc);
    auto* cs=field<void*>(bytes,0x20);auto* ds=field<void*>(bytes,0x18);void *tcs{},*tds{};
    if(!field<std::uint32_t>(bytes,0xc) ||
       width!=color->image.extent.width || height!=color->image.extent.height ||
       !read(api_,targets_.target(eye,role),0x70,tcs) || cs!=tcs ||
       !surface(api_,cs,api_.colorVtable,color->texture))return false;
    if(hasDepth){
        if(!read(api_,targets_.target(eye,true),0xa0,tds) || ds!=tds ||
           !surface(api_,ds,api_.depthVtable,depth->texture))return false;
    }else if(ds)return false;
    for(unsigned i=1;i<8;++i)if(field<void*>(bytes,0x20+i*8))return false;
    return true;
}
EngineFramebuffers::~EngineFramebuffers(){Errno preserve;if(api_ && owner())reset();}
bool EngineFramebuffers::prepare() noexcept {
    Errno preserve;
    if(!api_ || poisoned_ || scoped_ || targets_.pending() || !targets_.ready() ||
       (owner_!=std::thread::id{} && !owner()) || !api_.ownsThread() || !table(api_,device_,api_.deviceVtable))return false;
    owner_=std::this_thread::get_id();
    std::array<void*,10> next{};FramebufferDesc desc{};bool constructed=false;
    try {
        if(ready()){
            for(unsigned i=0;i<count();++i)if(!inspect(buffers_[i],i))return false;
            return true;
        }
        for(unsigned i=0;i<count();++i){
            const unsigned eye=bufferEye(i);const auto role=bufferRole(i);
            void *cs{},*ds{};std::array<float,4> clear{};float depth{1};std::uint8_t stencil{};
            if(!read(api_,targets_.target(eye,role),0x70,cs) || !table(api_,cs,api_.colorVtable) ||
               !read(api_,targets_.target(eye,role),0xb0,clear))throw 0;
            if(withDepth(role) && (!read(api_,targets_.target(eye,true),0xa0,ds) || !table(api_,ds,api_.depthVtable) ||
               !read(api_,targets_.target(eye,true),0xc0,depth) || !read(api_,targets_.target(eye,true),0xc4,stencil) ||
               !std::isfinite(depth) || depth<0 || depth>1))throw 0;
            for(float x:clear)if(!std::isfinite(x))throw 0;
            api_.descriptor(&desc,&ds,&cs);constructed=true;
            put(desc,0x48,clear);put(desc,0xc8,depth);put(desc,0xcc,std::uint32_t(stencil));
            auto ref=api_.create(device_,&desc);next[i]=ref.value;
            constructed=false;api_.destroyDescriptor(&desc);
            if(!inspect(next[i],i))throw 0;
        }
        for(unsigned i=0;i<count();++i){std::uint32_t a{};if(!read(api_,next[i],0x10,a))throw 0;
            for(unsigned j=0;j<i;++j){std::uint32_t b{};
                if(next[i]==next[j] || !read(api_,next[j],0x10,b) || a==b)throw 0;
            }
        }
        // These releases cannot touch outstanding GPU use: caller retired targets.
        bool ok=true;for(auto& p:buffers_)ok=release(p) && ok;
        if(!ok)throw 0;
        buffers_=next;generation_=targets_.generation();return true;
    }catch(...){
        if(constructed){try{api_.destroyDescriptor(&desc);}catch(...){poisoned_=true;}}
        for(auto& p:next)release(p);
        return false;
    }
}
bool EngineFramebuffers::reset() noexcept {
    Errno preserve;
    if(!api_ || !owner() || scoped_ || targets_.pending())return false;
    bool ok=true;for(auto& p:buffers_)ok=release(p) && ok;
    if(ok)generation_=0;
    return ok;
}
bool EnginePassBinding::currentCommand() const {
    auto& api=pair_.api_;void* wrapper{};VkCommandBuffer cmd{};CommandRecord record{};
    return pair_.owner() && table(api,context_,api.memory.contextVtable) && read(api,context_,0x30a0,wrapper) &&
        read(api,wrapper,0x38,cmd) && commands_.recording(device_,cmd,family_,record) &&
        record.command==command_.command && record.generation==command_.generation && record.recording==command_.recording;
}
bool EnginePassBinding::state(void* desired,void* actual,bool open) const {
    auto& api=pair_.api_;void* vk{};std::array<void*,3> binding{};
    return !dirty(api) && table(api,gfx_,api.gfxVtable) && read(api,gfx_,0x128,vk) && vk==context_ &&
        read(api,context_,0x30a8,binding) && binding[2]==desired && binding[1]==actual && bool(binding[0])==open;
}
bool EnginePassBinding::rasterMatches(const EngineRasterViewport& viewport,const EngineRasterScissor& scissor) const {
    auto& api=pair_.api_;std::array<std::byte,40> cached{},raster{};
    static_assert(sizeof(VkViewport)==24 && sizeof(VkRect2D)==16);
    if(!read(api,gfx_,0x130,cached) || std::memcmp(cached.data(),&viewport,sizeof(viewport)) ||
       std::memcmp(cached.data()+24,&scissor,sizeof(scissor)) || !read(api,context_,0x3440,raster))return false;
    const auto actual=field<VkViewport>(raster,16);const auto actualClip=field<VkRect2D>(raster,0);
    // Pinned VkContext uses a negative viewport height. Its input is integer
    // left/right/top/bottom, not Vulkan x/y/width/height.
    return actual.x==float(viewport.left) && actual.y==float(viewport.bottom) &&
        actual.width==float(std::int64_t(viewport.right)-viewport.left) &&
        actual.height==float(std::int64_t(viewport.top)-viewport.bottom) &&
        actual.minDepth==viewport.minDepth && actual.maxDepth==viewport.maxDepth &&
        actualClip.offset.x==scissor.left && actualClip.offset.y==scissor.top &&
        actualClip.extent.width==std::uint32_t(std::int64_t(scissor.right)-scissor.left) &&
        actualClip.extent.height==std::uint32_t(std::int64_t(scissor.bottom)-scissor.top);
}
bool EnginePassBinding::prepareNative(void* gfx,void* context,void (*factoryFlush)()) noexcept {
    Errno preserve;
    if(probe(gfx,context))return true;
    if(!factoryFlush||active_||poisoned_||!pair_.ready()||pair_.targets_.pending()||!pair_.owner())return false;
    auto& api=pair_.api_;
    try{
        // Match HUD preparation: consume a pending native factory binding and
        // deferred clears before demanding an active Vulkan pass. Never flush
        // into an unregistered command buffer or an unvalidated context.
        void *vk{},*old{},*wrapper{};VkCommandBuffer cmd{};CommandRecord recording{};
        if(!table(api,gfx,api.gfxVtable)||!table(api,context,api.memory.contextVtable)||
           !read(api,gfx,0x128,vk)||vk!=context||!read(api,gfx,0x158,old)||!table(api,old,api.framebufferVtable)||
           !read(api,context,0x30a0,wrapper)||!read(api,wrapper,0x38,cmd)||!commands_.recording(device_,cmd,family_,recording))return false;
        if(std::strcmp(status_,"VTABLE_OR_FACTORY_DIRTY")&&std::strcmp(status_,"COMMAND_OR_PENDING_CLEARS")&&std::strcmp(status_,"NATIVE_PASS_PENDING"))return false;
        factoryFlush();api.flush(context);
        return probe(gfx,context);
    }catch(...){status_="NATIVE_FLUSH_EXCEPTION";return false;}
}
bool EnginePassBinding::probe(void* gfx,void* context) noexcept {return beginImpl(gfx,context,true);}
bool EnginePassBinding::begin(void* gfx,void* context) noexcept {return beginImpl(gfx,context,false);}
bool EnginePassBinding::beginImpl(void* gfx,void* context,bool probe) noexcept {
    Errno preserve;auto& api=pair_.api_;
    status_="OWNER_OR_TARGETS";
    if(active_ || poisoned_ || pair_.scoped_ || !pair_.ready() || (probe?pair_.targets_.pending():!pair_.targets_.pending()) || !pair_.owner())return false;
    try {
        status_="VTABLE_OR_FACTORY_DIRTY";
        if(!table(api,gfx,api.gfxVtable) || !table(api,context,api.memory.contextVtable) || dirty(api))return false;
        void *vk{},*old{},*wrapper{};std::uint32_t width{},height{},clears{},count{},id{};VkCommandBuffer cmd{};
        status_="COMMAND_OR_PENDING_CLEARS";
        if(!read(api,gfx,0x128,vk) || vk!=context || !read(api,gfx,0x158,old) || !table(api,old,api.framebufferVtable) ||
           !read(api,old,0xc,count) || !count || count>=0x7fffffff || !read(api,old,0xf8,width) || !read(api,old,0xfc,height) ||
           !read(api,gfx,0x160,id) || !read(api,context,0x32a8,clears) || clears ||
           !read(api,context,0x30a0,wrapper) || !read(api,wrapper,0x38,cmd) || !commands_.recording(device_,cmd,family_,command_))return false;
        status_="FRAMEBUFFER_ID";
        std::uint32_t oldId{};if(!read(api,old,0x10,oldId) || oldId!=id)return false;
        status_="FRAMEBUFFER_EXTENT";
        const auto* image=pair_.targets_.image(0);
        if(!image || !pair_.targets_.sourceExtentMatches(width,height))return false;
        status_="EYE_FRAMEBUFFER";
        for(unsigned i=0;i<pair_.count();++i){std::uint32_t eyeId{};
            if(!pair_.inspect(pair_.buffers_[i],i) || !read(api,pair_.buffers_[i],0x10,eyeId) || eyeId==id)return false;}
        gfx_=gfx;context_=context;
        // Only a flushed, active original pass is eligible; pending bindings and
        // clear requests must be consumed by the engine before this boundary.
        status_="NATIVE_PASS_PENDING";
        if(!state(old,old,true) || !read(api,gfx,0x130,previousViewport_) || !read(api,gfx,0x148,previousScissor_))return false;
        status_="RASTER_STATE";
        const auto& v=previousViewport_;const auto& s=previousScissor_;
        if(v.left<0 || v.top<0 || v.right<=v.left || v.bottom<=v.top ||
           s.left<0 || s.top<0 || s.right<s.left || s.bottom<s.top ||
           !std::isfinite(v.minDepth) || !std::isfinite(v.maxDepth) || v.minDepth<0 || v.minDepth>1 || v.maxDepth<0 || v.maxDepth>1 ||
           width>std::uint32_t(INT32_MAX) || height>std::uint32_t(INT32_MAX) || !rasterMatches(v,s))return false;
        eyeViewport_={0,std::int32_t(image->image.extent.width),0,std::int32_t(image->image.extent.height),v.minDepth,v.maxDepth};
        eyeScissor_={0,std::int32_t(image->image.extent.width),0,std::int32_t(image->image.extent.height)};
        status_="READY";if(probe)return true;
        api.increment(reinterpret_cast<volatile std::uint32_t*>(static_cast<std::byte*>(old)+0xc));
        previous_=old;active_=true;pair_.scoped_=true;buffer_=-1;touched_=false;return true;
    }catch(...){return false;}
}
bool EnginePassBinding::eye(unsigned eye,EngineTargetRole role) noexcept {
    Errno preserve;
    const auto index=EngineFramebuffers::index(eye,role);
    if(!active_ || poisoned_ || index>=pair_.count() || !pair_.ready() || !pair_.targets_.pending())return false;
    try {
        bool reuse=false;
        if(!currentCommand() || dirty(pair_.api_) || !pair_.inspect(pair_.buffers_[index],index))return false;
        if(buffer_>=0){
            if(!state(pair_.buffers_[buffer_],pair_.buffers_[buffer_],true))return false;
            if(buffer_==int(index)){
                // A native Lock may request the attachment already selected by
                // this stage. Do not force null -> same framebuffer: on a tile
                // GPU that needlessly ends/stores and reopens/loads the pass.
                // A pending clear takes the full close path (closeImpl), so its
                // original raster state and native clear ordering are retained.
                // The Flush below still owns all native barrier restarts.
                std::uint32_t clears{};
                if(!read(pair_.api_,context_,0x32a8,clears))return false;
                reuse=!clears;
            }
            if(!reuse&&!closeImpl())return false;
        }
        touched_=true;
        if(!pair_.api_.bind(gfx_,pair_.buffers_[index]) || !pair_.api_.bindVk(context_,pair_.buffers_[index])){poisoned_=true;return false;}
        pair_.api_.viewport(gfx_,eyeViewport_);pair_.api_.scissor(gfx_,eyeScissor_);
        pair_.api_.flush(context_);
        if(!state(pair_.buffers_[index],pair_.buffers_[index],true) || !rasterMatches(eyeViewport_,eyeScissor_)){poisoned_=true;return false;}
        buffer_=int(index);if(reuse)sameEyeFramebufferReuses.fetch_add(1,std::memory_order_relaxed);return true;
    }catch(...){poisoned_=true;return false;}
}
bool EnginePassBinding::closeImpl() {
    if(!currentCommand() || dirty(pair_.api_))return false;
    std::uint32_t clears{};
    if(!read(pair_.api_,context_,0x32a8,clears))return false;
    if(clears){
        // grcDevice::Clear may only queue VkContext clear bits. Consume them
        // against their current native target before changing desired binding.
        pair_.api_.flush(context_);
        if(!read(pair_.api_,context_,0x32a8,clears)||clears)return false;
    }
    touched_=true;
    if(!pair_.api_.bindVk(context_,nullptr))return false;
    // Flush closes through EndRenderPass + AddImageBarriers and clears the
    // active framebuffer reference. Calling EndRenderPass alone leaves stale
    // 0x30b0 and lets the next flush end the same pass a second time.
    pair_.api_.flush(context_);
    if(!state(nullptr,nullptr,false))return false;
    buffer_=-1;return true;
}
bool EnginePassBinding::close() noexcept {
    Errno preserve;if(!active_ || poisoned_)return false;
    // A draw body may have switched targets for water/reflections. Do not copy
    // stale eye pixels merely because the command buffer itself still matches.
    try {if(buffer_>=0 && state(pair_.buffers_[buffer_],pair_.buffers_[buffer_],true) && closeImpl())return true;}catch(...){}
    poisoned_=true;return false;
}
bool EnginePassBinding::closeIfBound() noexcept {
    Errno preserve;if(!active_||poisoned_)return false;
    if(buffer_<0){try{return currentCommand()&&state(nullptr,nullptr,false);}catch(...){return false;}}
    return close();
}
bool EnginePassBinding::closeNativeColor(const void* target) noexcept {
    Errno preserve;
    if(!active_ || poisoned_ || touched_ || buffer_>=0 || !target)return false;
    try {
        auto& api=pair_.api_;void *surface{},*bound{};std::uint32_t width{},height{};
        if(!currentCommand() || !state(previous_,previous_,true) ||
           !table(api,target,api.memory.targetVtable) || !read(api,target,0x70,surface) || !surface ||
           !read(api,previous_,0x20,bound) || surface!=bound ||
           !read(api,previous_,0xf8,width) || !read(api,previous_,0xfc,height))return false;
        const auto& v=previousViewport_;const auto& s=previousScissor_;
        if(v.left || v.top || s.left || s.top || v.right!=std::int64_t(width) ||
           v.bottom!=std::int64_t(height) || s.right!=std::int64_t(width) ||
           s.bottom!=std::int64_t(height) || !rasterMatches(v,s))return false;
        if(closeImpl())return true;
    }catch(...){}
    poisoned_=true;return false;
}
bool EnginePassBinding::restore() noexcept {
    Errno preserve;if(!active_)return !poisoned_;
    if(!pair_.owner())return false;
    try {
        if(!touched_){
            active_=false;pair_.scoped_=false;
            if(!pair_.release(previous_))throw 0;
            return true;
        }
        if(!currentCommand() || !closeImpl())throw 0;
        // GfxContext compares an id cache before delegating. If no eye was bound,
        // its cache still names previous_, so explicitly restore VkContext too.
        if(!pair_.api_.bind(gfx_,previous_) || !pair_.api_.bindVk(context_,previous_))throw 0;
        pair_.api_.viewport(gfx_,previousViewport_);pair_.api_.scissor(gfx_,previousScissor_);
        pair_.api_.flush(context_);
        if(!state(previous_,previous_,true) || !rasterMatches(previousViewport_,previousScissor_))throw 0;
        active_=false;pair_.scoped_=false;buffer_=-1;
        if(!pair_.release(previous_))throw 0;
        return true;
    }catch(...){poisoned_=true;pair_.poisoned_=true;return false;}
}
EnginePassBinding::~EnginePassBinding(){restore();}
bool EnginePassBinding::matches(unsigned eye,EngineTargetRole role,bool dynamicScissor) const noexcept {
    Errno preserve;const auto index=EngineFramebuffers::index(eye,role);
    if(!active_ || poisoned_ || index>=pair_.count() || buffer_!=int(index) || !pair_.ready())return false;
    try{
        auto scissor=eyeScissor_;
        if(dynamicScissor&&(!read(pair_.api_,gfx_,0x148,scissor)||scissor.left<0||scissor.top<0||
           scissor.right<scissor.left||scissor.bottom<scissor.top||scissor.right>eyeScissor_.right||scissor.bottom>eyeScissor_.bottom))return false;
        return currentCommand() && state(pair_.buffers_[index],pair_.buffers_[index],true) && rasterMatches(eyeViewport_,scissor);
    }catch(...){return false;}
}
bool EnginePassBinding::finishDraw(unsigned eye,EngineTargetRole role) noexcept {
    Errno preserve;
    if(!matches(eye,role,true))return false;
    try{pair_.api_.scissor(gfx_,eyeScissor_);return matches(eye,role);}catch(...){return false;}
}
void EnginePassBinding::captureRasterObservation(bool beforeRestore) noexcept {
    Errno preserve;EngineRasterObservation value{};value.attempted=true;value.beforeRestore=beforeRestore;
    try {
        std::array<std::byte,40> cached{},native{};
        if(active_&&pair_.owner()&&read(pair_.api_,gfx_,0x130,cached)&&read(pair_.api_,context_,0x3440,native)){
            value.available=true;value.viewport=field<EngineRasterViewport>(cached,0);
            value.scissor=field<EngineRasterScissor>(cached,24);
            value.nativeScissor=field<VkRect2D>(native,0);value.nativeViewport=field<VkViewport>(native,16);
            const auto& v=value.viewport;const auto& s=value.scissor;const auto& n=value.nativeViewport;const auto& c=value.nativeScissor;
            value.fullViewport=!std::memcmp(&v,&eyeViewport_,sizeof v);
            value.fullScissor=!std::memcmp(&s,&eyeScissor_,sizeof s);
            value.consistent=n.x==float(v.left)&&n.y==float(v.bottom)&&n.width==float(std::int64_t(v.right)-v.left)&&
                n.height==float(std::int64_t(v.top)-v.bottom)&&n.minDepth==v.minDepth&&n.maxDepth==v.maxDepth&&
                c.offset.x==s.left&&c.offset.y==s.top&&std::int64_t(c.extent.width)==std::int64_t(s.right)-s.left&&
                std::int64_t(c.extent.height)==std::int64_t(s.bottom)-s.top;
        }
    }catch(...){}
    rasterObservation_=value;
}
bool EnginePassBinding::flushDraw(unsigned eye,EngineTargetRole role) noexcept {
    Errno preserve;if(!matches(eye,role,true))return false;
    try{
        pair_.api_.flush(context_);std::uint32_t clears{};
        return read(pair_.api_,context_,0x32a8,clears)&&!clears&&matches(eye,role,true);
    }catch(...){return false;}
}
}
