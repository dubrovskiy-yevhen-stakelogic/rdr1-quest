#pragma once
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <utility>

namespace rdrvr::engine {
// CPU visibility shafts require the frozen conservative union. The selected
// eye viewport retains exact optical metadata for depth/lighting shaders.
// Only two pinned eye-local Set calls may enter this route; no viewport bytes,
// shader state or borrowed union state are modified or retained here.
class EyeShaftBoundary {
public:
    struct Selection {const void* eyeViewport{};const void* unionViewport{};std::uint64_t sceneId{};};
    // Must establish current frozen eye identity, render owner, renderer/scene
    // serial and a live union lease for the entire synchronous native call.
    using Resolve=bool(*)(void*,const void* viewport,const void* camera,Selection&);
private:
    Resolve resolve_{};void* user_{};
    inline static thread_local const EyeShaftBoundary* active_{};
    std::atomic<std::uint64_t> qualified_{},routed_{},declined_{},nested_{};
    static bool cameraFor(const void* viewport,const void* camera) noexcept {
        const auto p=reinterpret_cast<std::uintptr_t>(viewport);
        return p&&p<=UINTPTR_MAX-0x40&&reinterpret_cast<std::uintptr_t>(camera)==p+0x40;
    }
public:
    void configure(Resolve resolve,void* user=nullptr) noexcept {resolve_=resolve;user_=user;}
    template<class Native> void shaft(const void* viewport,const void* camera,bool exact,Native&& native){
        const int incoming=errno;Selection selected{};bool route=false;
        const auto* previous=active_;
        struct Scope {const EyeShaftBoundary*& slot;const EyeShaftBoundary* previous;~Scope(){slot=previous;}} scope{active_,previous};
        if(exact){
            ++qualified_;
            if(previous)++nested_;
            else if(resolve_&&cameraFor(viewport,camera)){
                active_=this;
                try{route=resolve_(user_,viewport,camera,selected)&&selected.eyeViewport==viewport&&
                    selected.sceneId&&selected.unionViewport&&selected.unionViewport!=viewport&&
                    reinterpret_cast<std::uintptr_t>(selected.unionViewport)<=UINTPTR_MAX-0x40;}
                catch(...){route=false;}
            }
            if(route)++routed_;else ++declined_;
        }
        const void* v=route?selected.unionViewport:viewport;
        const void* m=route?reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(v)+0x40):camera;
        errno=incoming;
        std::forward<Native>(native)(v,m);
    }
    std::uint64_t qualified()const noexcept{return qualified_;}
    std::uint64_t routed()const noexcept{return routed_;}
    std::uint64_t declined()const noexcept{return declined_;}
    std::uint64_t nested()const noexcept{return nested_;}
};
}
