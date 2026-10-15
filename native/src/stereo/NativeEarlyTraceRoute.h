#pragma once
#include "NativeEarlyFrameController.h"
#include "opaque-run/NativeOpaqueRunCoverage.h"
#include <atomic>

namespace rdrvr::experimental {
// Thread-local route shared by the actual GameXr DrawCommon entry and the
// TraceEntry geometry/primitive witnesses. Only an enclosing selected eye pass
// publishes a controller. Shadows/reflections cannot inherit depth authority.
class NativeEarlyTraceRoute {
    inline static thread_local NativeEarlyFrameController* active_{};
    inline static thread_local bool mainTiles_{};
    inline static thread_local unsigned depth_{};
    inline static thread_local NativeOpaqueRun opaque_;
    inline static thread_local bool opaqueConfigured_{};
    inline static thread_local NativeOpaqueRun::Draw* opaqueDraw_{};
    // Pass scopes that published a controller, on all threads. 0 => no thread has a
    // controller, so controller()/source()/mainTiles() answer without a TLS access.
    inline static std::atomic<unsigned> live_{};
public:
    // No production consumer until the shared camera/context writer closure
    // is proved. Keep the observer cold as well: no native hooks or per-object
    // TLS/run bookkeeping merely to produce an unused camera memo ticket.
    static constexpr bool ObserveOpaqueRuns=false;
    static NativeOpaqueRun::Frame opaqueFrame()noexcept {
        NativeOpaqueRun::Frame frame;
        if(active_&&active_->routeFrame(frame.scene,frame.pose,frame.stage)&&
           (!active_->depth()||mainTiles_))frame.owner=reinterpret_cast<std::uintptr_t>(active_);
        return frame;
    }
    static NativeOpaqueRun& opaqueRun()noexcept {
        if(!opaqueConfigured_){
            opaqueConfigured_=opaque_.configure({nullptr,
                [](void*,const NativeOpaqueRun::Frame& frame)noexcept{return frame.valid()&&frame==opaqueFrame();},
                [](void*)noexcept{return nativeOpaqueCoverageEpoch();}});
        }
        return opaque_;
    }
    // A real TraceEntry DrawCommon wrapper constructs this before GameXr.
    // It includes even unknown LRs, so re-entrant/native routes revoke caches.
    class OpaqueDraw {
        std::optional<NativeOpaqueRun::Draw> draw_;NativeOpaqueRun::Draw* previous_{};
    public:
        OpaqueDraw(std::uintptr_t container,std::uintptr_t returnOffset)noexcept {
            if constexpr(ObserveOpaqueRuns){previous_=opaqueDraw_;draw_.emplace(opaqueRun(),container,returnOffset);opaqueDraw_=&*draw_;}
        }
        ~OpaqueDraw(){if constexpr(ObserveOpaqueRuns)opaqueDraw_=previous_;}
        OpaqueDraw(const OpaqueDraw&)=delete;
    };
    static NativeOpaqueRun::Ticket opaqueTicket()noexcept {
        if constexpr(ObserveOpaqueRuns)return opaqueDraw_?opaqueDraw_->ticket():NativeOpaqueRun::Ticket{};
        return {};
    }
    static void opaqueCommitSkipped()noexcept{if constexpr(ObserveOpaqueRuns){if(opaqueDraw_&&!opaqueDraw_->commitSkipped())opaque_.invalidate();}}
    static void opaqueBeforeOriginal()noexcept{if constexpr(ObserveOpaqueRuns){if(opaqueDraw_)opaqueDraw_->beforeOriginal();else opaque_.invalidate();}}
    class Pass {
        NativeEarlyFrameController* previous_{};bool previousTiles_{},counted_{};
    public:
        explicit Pass(NativeEarlyFrameController* controller)noexcept:
            previous_(active_),previousTiles_(mainTiles_){if constexpr(ObserveOpaqueRuns)opaque_.invalidate();
            if(!depth_&&controller){live_.fetch_add(1,std::memory_order_relaxed);counted_=true;}
            active_=depth_?nullptr:controller;++depth_;mainTiles_=false;}
        ~Pass(){if constexpr(ObserveOpaqueRuns)opaque_.invalidate();--depth_;active_=previous_;mainTiles_=previousTiles_;
            if(counted_)live_.fetch_sub(1,std::memory_order_relaxed);}
        Pass(const Pass&)=delete;
    };
    class Tiles {
        bool previous_{};
    public:
        explicit Tiles(bool exact)noexcept:previous_(mainTiles_){if constexpr(ObserveOpaqueRuns)opaque_.invalidate();mainTiles_=exact;}
        ~Tiles(){if constexpr(ObserveOpaqueRuns)opaque_.invalidate();mainTiles_=previous_;}
        Tiles(const Tiles&)=delete;
    };
    static NativeEarlyFrameController* controller()noexcept{return live_.load(std::memory_order_relaxed)?active_:nullptr;}
    static bool mainTiles()noexcept{return controller()&&mainTiles_;}
    static CurrentNativeWallSources* source()noexcept{
        auto* a=controller();return a&&a->needsGeometryProof()?&a->sources():nullptr;
    }
    class Geometry {
        std::optional<CurrentNativeWallSources::Geometry> observed_;
    public:
        Geometry(const void* geometry,bool exact)noexcept {
            if(auto* a=controller())a->sourceRead([&](auto& sources){observed_.emplace(sources,geometry,exact);});
        }
        Geometry(const Geometry&)=delete;
    };
    static void prepared(void* context,const CurrentNativeWallSources::Witness::Binding& binding,bool exact)noexcept {
        if(auto* a=controller())a->sourceRead([&](auto& sources){sources.prepared(context,binding,exact);});
    }
};
}
