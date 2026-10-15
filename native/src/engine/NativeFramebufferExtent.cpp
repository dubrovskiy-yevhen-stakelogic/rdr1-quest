#include "NativeFramebufferExtent.h"
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <limits>
#include <sstream>

namespace rdrvr::engine {
namespace {
template<class T>T field(const void* p,std::size_t offset=0){T v{};std::memcpy(&v,static_cast<const std::byte*>(p)+offset,sizeof v);return v;}
bool dimensions(std::array<std::uint32_t,2> size){return size[0]&&size[1]&&size[0]<=8192&&size[1]<=8192;}
}
bool NativeFramebufferExtent::configure(Api api) noexcept {
    if(ready()||!api.depthCreate||!api.framebufferCtor||!api.requestedExtent||!api.depthCreated||
       !api.depthVtable||!api.colorVtable||!api.textureVtable||!api.framebufferVtable||!api.displayDepthName||!api.displayVtable)return false;
    api_=api;ready_.store(true,std::memory_order_release);return true;
}
void NativeFramebufferExtent::installed(bool value) noexcept {
    installed_.store(value&&ready(),std::memory_order_release);
}
bool NativeFramebufferExtent::createDepth(void* surface,const void* descriptor,bool qualified) {
    ++depthCalls_;
    if(qualified)++depthCallerMatched_;
    // The caller is the exact native VkDevice creation boundary, and the name
    // is the exact immutable pointer emitted by CreateBackbuffers. Other depth
    // allocations, including right-eye private targets, remain native.
    if(!installed_.load(std::memory_order_acquire)||faulted()||!qualified||!surface||!descriptor||
       field<std::uintptr_t>(surface)!=api_.depthVtable||field<const void*>(descriptor,0x20)!=api_.displayDepthName)
        return api_.depthCreate(surface,descriptor);
    ++depthNameMatched_;
    std::array<std::uint32_t,2> requested{};
    const int incomingErrno=errno;
    if(!api_.requestedExtent(api_.user,requested)||!dimensions(requested)){
        errno=incomingErrno;return api_.depthCreate(surface,descriptor);
    }
    ++depthRequestReady_;
    const auto stock=field<std::array<std::uint32_t,2>>(descriptor,4);
    if(!dimensions(stock)||field<void*>(surface,0x30)){
        errno=incomingErrno;return api_.depthCreate(surface,descriptor);
    }
    ++depthQualified_;
    const std::array size{std::max(stock[0],requested[0]),std::max(stock[1],requested[1])};
    alignas(8) std::array<std::byte,0x30> copied{};std::memcpy(copied.data(),descriptor,copied.size());
    std::memcpy(copied.data()+4,size.data(),sizeof size);errno=incomingErrno;
    const bool success=api_.depthCreate(surface,copied.data());const int nativeErrno=errno;
    auto fail=[&]{++depthFailures_;fault_.store(true,std::memory_order_release);
        api_.depthCreated(api_.user,surface,size[0],size[1],false);errno=nativeErrno;return false;};
    // A false return uses the native VkDevice failure cleanup before the surface
    // can be placed in a display framebuffer. Never publish mixed dimensions.
    if(!success)return fail();
    void* texture=field<void*>(surface,0x30);
    if(field<std::uintptr_t>(surface)!=api_.depthVtable||!texture||field<std::uintptr_t>(texture)!=api_.textureVtable||
       field<std::array<std::uint32_t,2>>(surface,0x18)!=size||
       field<std::array<std::uint32_t,2>>(texture,0x48)!=size)return fail();
    const auto capacity=field<std::array<std::uint32_t,2>>(texture,0xc4);
    if(capacity[0]<size[0]||capacity[1]<size[1])return fail();
    try {
        std::lock_guard lock(mutex_);
        auto it=std::find_if(depths_.begin(),depths_.end(),[&](const auto& d){return d.surface==surface;});
        if(it==depths_.end())depths_.push_back({surface,texture});else *it={surface,texture};
    }catch(...){return fail();}
    if(size!=stock)++depthEnlarged_;
    api_.depthCreated(api_.user,surface,size[0],size[1],true);errno=nativeErrno;return true;
}
bool NativeFramebufferExtent::confirmNoDisplayDepth(void* display,bool exactInitialCaller) noexcept {
    const int saved=errno;struct Restore {int value;~Restore(){errno=value;}} restore{saved};
    ++displayChecks_;
    const auto reject=[&]{++displayRejected_;return false;};
    // These are live objects just retained/populated by the pinned native
    // constructor. No cached pointer, inferred counter or relaxed name match
    // may establish that a required depth allocation can be omitted.
    if(!ready()||!installed_.load(std::memory_order_acquire)||faulted()||!exactInitialCaller||!display||
       field<std::uintptr_t>(display)!=api_.displayVtable)return reject();
    if(field<std::uint32_t>(display,0x298)){++displayPresent_;return false;}
    const auto count=field<std::int32_t>(display,0xc8);
    auto* buffers=field<void**>(display,0xc0);
    if(count<=0||count>16||!buffers||field<std::uint32_t>(display,0x278)!=std::uint32_t(count)||
       field<void*>(display,0x60)!=buffers[0])return reject();
    for(int i=0;i<count;++i){
        auto* fb=buffers[i];
        if(!fb||field<std::uintptr_t>(fb)!=api_.framebufferVtable||!field<std::uint32_t>(fb,0xc)||field<void*>(fb,0x18))return reject();
        auto* color=field<void*>(fb,0x20);
        if(!color||field<std::uintptr_t>(color)!=api_.colorVtable)return reject();
        auto* texture=field<void*>(color,0x30);
        if(!texture||field<std::uintptr_t>(texture)!=api_.textureVtable)return reject();
        const auto size=field<std::array<std::uint32_t,2>>(texture,0x48);
        if(!dimensions(size)||size!=field<std::array<std::uint32_t,2>>(fb,0xf8))return reject();
        for(unsigned attachment=1;attachment<8;++attachment)if(field<void*>(fb,0x20+attachment*8))return reject();
    }
    ++displayAbsent_;return true;
}
void NativeFramebufferExtent::constructFramebuffer(void* framebuffer,const void* descriptor,std::uint32_t hash) {
    ++framebufferCalls_;api_.framebufferCtor(framebuffer,descriptor,hash);
    const int nativeErrno=errno;
    // Even after a later fault/uninstall, existing enlarged depths still need
    // valid framebuffers. Their correction is required until idle teardown.
    if(!framebuffer||!ready()){errno=nativeErrno;return;}
    void* depth=field<void*>(framebuffer,0x18);
    if(!depth){errno=nativeErrno;return;}
    void* expectedTexture{};
    {
        std::lock_guard lock(mutex_);
        auto it=std::find_if(depths_.begin(),depths_.end(),[&](const auto& d){return d.surface==depth;});
        if(it==depths_.end()){errno=nativeErrno;return;}
        expectedTexture=it->texture;
    }
    // These are live references retained by the original constructor. Do not
    // dereference cached depth records: a destroyed object's address may recur.
    if(field<std::uintptr_t>(framebuffer)!=api_.framebufferVtable||
       field<std::uintptr_t>(depth)!=api_.depthVtable||field<void*>(depth,0x30)!=expectedTexture||
       !expectedTexture||field<std::uintptr_t>(expectedTexture)!=api_.textureVtable){
        fault_.store(true,std::memory_order_release);errno=nativeErrno;return;
    }
    auto size=field<std::array<std::uint32_t,2>>(expectedTexture,0x48);
    for(unsigned i=0;i<8;++i)if(auto* color=field<void*>(framebuffer,0x20+i*8)){
        // Native constructor has already read precisely these texture pointers
        // and extents. Repeating those live reads cannot fail a guarded write
        // and leave its oversized MAX result visible to GetFramebuffer.
        auto* texture=field<void*>(color,0x30);
        const auto colorSize=field<std::array<std::uint32_t,2>>(texture,0x48);
        size[0]=std::min(size[0],colorSize[0]);size[1]=std::min(size[1],colorSize[1]);
        if(field<std::uintptr_t>(color)!=api_.colorVtable||field<std::uintptr_t>(texture)!=api_.textureVtable)
            fault_.store(true,std::memory_order_release);
    }
    if(!size[0]||!size[1])fault_.store(true,std::memory_order_release);
    const auto before=field<std::array<std::uint32_t,2>>(framebuffer,0xf8);
    std::memcpy(static_cast<std::byte*>(framebuffer)+0xf8,size.data(),sizeof size);
    ++trackedFramebuffers_;if(before!=size)++clampedFramebuffers_;
    lastWidth_=size[0];lastHeight_=size[1];errno=nativeErrno;
}
void NativeFramebufferExtent::resetAfterIdle() noexcept {
    installed(false);std::lock_guard lock(mutex_);depths_.clear();fault_.store(false,std::memory_order_release);
}
NativeFramebufferExtent::Stats NativeFramebufferExtent::snapshot()const noexcept {
    return {ready(),installed_.load(),faulted(),depthCalls_.load(),depthQualified_.load(),depthEnlarged_.load(),depthFailures_.load(),
        framebufferCalls_.load(),trackedFramebuffers_.load(),clampedFramebuffers_.load(),lastWidth_.load(),lastHeight_.load(),
        depthCallerMatched_.load(),depthNameMatched_.load(),depthRequestReady_.load(),
        displayChecks_.load(),displayAbsent_.load(),displayPresent_.load(),displayRejected_.load()};
}
std::string NativeFramebufferExtent::report()const {
    const auto s=snapshot();std::ostringstream o;o<<"{\"ready\":"<<s.ready<<",\"installed\":"<<s.installed<<",\"fault\":"<<s.fault
      <<",\"depthCalls\":"<<s.depthCalls<<",\"depthQualified\":"<<s.depthQualified<<",\"depthEnlarged\":"<<s.depthEnlarged<<",\"depthFailures\":"<<s.depthFailures
      <<",\"framebufferCalls\":"<<s.framebufferCalls<<",\"trackedFramebuffers\":"<<s.trackedFramebuffers<<",\"clampedFramebuffers\":"<<s.clampedFramebuffers
      <<",\"lastWidth\":"<<s.lastWidth<<",\"lastHeight\":"<<s.lastHeight
      <<",\"depthCallerMatched\":"<<s.depthCallerMatched<<",\"depthNameMatched\":"<<s.depthNameMatched<<",\"depthRequestReady\":"<<s.depthRequestReady
      <<",\"displayChecks\":"<<s.displayChecks<<",\"displayAbsent\":"<<s.displayAbsent<<",\"displayPresent\":"<<s.displayPresent<<",\"displayRejected\":"<<s.displayRejected<<'}';return o.str();
}
}
