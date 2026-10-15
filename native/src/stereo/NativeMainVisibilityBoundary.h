#pragma once
#include "NativeCurrentRecordProvider.h"
#include "MainJoin35Diagnostic.h"
#include <cerrno>
#include <thread>
#include <utility>

namespace rdrvr::experimental {
// PRIVATE hook boundary for the pinned MAIN WaitForVisibilityList call. The
// wrapper never substitutes a ready-bit poll for the ORIGINAL join/sort. It
// publishes the fixed-memory provider only AFTER the original has returned.
// A renderer owner must keep the real scene/list lifetime until endFrame().
// This boundary supplies no drawable/material/side-effect skip authority.
class NativeMainVisibilityBoundary {
public:
    using Provider=rdr95::NativeCurrentRecordProvider;
    using Address=rdr95::Address;
    struct Api {
        void* context{};
        bool(*frameCurrent)(void*,std::uint64_t)noexcept{};
        bool(*joined)(void*,Provider&)noexcept{};
        void(*revoked)(void*,std::uint64_t)noexcept{};
        // Optional for standalone provider tests; the production controller
        // supplies a short resource read lease AFTER original join/sort.
        bool(*readBegin)(void*,std::uint64_t)noexcept{};
        void(*readEnd)(void*)noexcept{};
        // Native cache synchronization, if needed, must run BEFORE the short
        // read. Never acquire a native critical section while holding Read.
        void(*prepareRead)(void*,Address,unsigned,Address,std::uint64_t)noexcept{};
    };
    struct Stats {unsigned joined{},wrongBoundary{},repeated{},declined{},revoked{};};
    explicit NativeMainVisibilityBoundary(rdr95::Api reads):provider_(reads){}
    // Must execute on the selected primary render owner, never the XR worker.
    bool beginFrame(std::uint64_t frame,Api api)noexcept {
        if(frame_||!frame||!api.frameCurrent||!api.joined||!api.revoked)return false;
        if(!api.frameCurrent(api.context,frame))return false;
        owner_=std::this_thread::get_id();api_=api;frame_=frame;stats_={};joined_=fault_=false;return true;
    }
    class PrimaryDepth {
        friend class NativeMainVisibilityBoundary;
        NativeMainVisibilityBoundary* owner_{};PrimaryDepth* previous_{};
    public:
        explicit PrimaryDepth(NativeMainVisibilityBoundary& b)noexcept {
            if(!b.owned()||!b.frame_)return;
            owner_=&b;previous_=active_;active_=this;
            if(previous_){previous_->owner_->revoke();b.revoke();}
        }
        ~PrimaryDepth(){if(owner_)active_=previous_;}
        PrimaryDepth(const PrimaryDepth&)=delete;
    };
    template<class Original>
    void wait(Address manager,unsigned request,Address returnOffset,Original&& original) {
        main_join35::event(main_join35::BoundaryCalls);
        const bool ours=owned()&&active_&&active_->owner_==this&&!active_->previous_&&frame_;
        const bool exact=ours&&request==rdr95::MainRequest&&returnOffset==rdr95::MainDepthWaitReturn;
        const int incoming=errno;
        if(exact&&joined_){main_join35::event(main_join35::Repeated);++stats_.repeated;revoke();}
        // All MAIN/foreign/shadow/reflection calls still execute exactly once.
        errno=incoming;
        try{std::forward<Original>(original)();}catch(...){const int error=errno;if(exact){main_join35::event(main_join35::NativeException);revoke();}errno=error;throw;}
        const int nativeError=errno;
        struct Preserve {int error;~Preserve(){errno=error;}} preserve{nativeError};
        if(!ours){main_join35::event(main_join35::NoPrimaryScope);return;}
        if(!exact){main_join35::event(main_join35::WrongBoundary);++stats_.wrongBoundary;return;}
        if(fault_){main_join35::event(main_join35::Faulted);return;}
        if(api_.prepareRead)api_.prepareRead(api_.context,manager,request,returnOffset,frame_);
        if(bool(api_.readBegin)!=bool(api_.readEnd)){main_join35::event(main_join35::ReadPairMismatch);++stats_.declined;revoke();return;}
        if(api_.readBegin&&!api_.readBegin(api_.context,frame_)){main_join35::event(main_join35::ReadDeclined);++stats_.declined;revoke();return;}
        struct ReadEnd{Api& api;~ReadEnd(){if(api.readEnd)api.readEnd(api.context);}} readEnd{api_};
        if(!api_.frameCurrent(api_.context,frame_)){
            main_join35::event(main_join35::FrameBeforeProviderDeclined);++stats_.declined;revoke();return;
        }
        if(!provider_.begin(manager,request,returnOffset,true,frame_)){
            main_join35::event(main_join35::ProviderBeginDeclined);main_join35::local.providerError=static_cast<unsigned>(provider_.error());++stats_.declined;revoke();return;
        }
        joined_=true;
        if(!api_.joined(api_.context,provider_)){main_join35::event(main_join35::JoinedAdapterDeclined);++stats_.declined;revoke();return;}
        if(!provider_.current()){main_join35::event(main_join35::ProviderAfterDeclined);++stats_.declined;revoke();return;}
        if(!api_.frameCurrent(api_.context,frame_)){main_join35::event(main_join35::FrameAfterJoinDeclined);++stats_.declined;revoke();return;}
        main_join35::event(main_join35::Joined);++stats_.joined;
    }
    bool current() {
        if(!owned()||!frame_||!joined_||fault_)return false;
        if(!api_.frameCurrent(api_.context,frame_)||!provider_.current()){revoke();return false;}
        return true;
    }
    void endFrame()noexcept {
        if(!owned())return;
        provider_.end();frame_=0;joined_=false;fault_=false;api_={};
    }
    const Stats& stats()const noexcept{return stats_;}
private:
    inline static thread_local PrimaryDepth* active_{};
    Provider provider_;Api api_{};std::thread::id owner_{};std::uint64_t frame_{};
    bool joined_{},fault_{};Stats stats_{};
    bool owned()const noexcept{return owner_==std::this_thread::get_id();}
    void revoke()noexcept {
        provider_.end();if(fault_)return;fault_=true;++stats_.revoked;
        if(api_.revoked)api_.revoked(api_.context,frame_);
    }
};
}
