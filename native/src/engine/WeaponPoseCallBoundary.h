#pragma once
#include "WeaponAttachmentReader.h"
#include "NativeAimProbe.h"
#include <atomic>
namespace rdrvr::engine {
struct WeaponPoseCall {
    void* weapon{};const WeaponNativeMatrix* attachment{};
    const AimNativeVector* velocity{};bool nativeFlag{};float nativeScalar{};
};
enum class WeaponPosePhase {None,Prepare,Native,Complete};
struct WeaponPoseScope {
    std::uint64_t serial{};void* weapon{};WeaponPosePhase phase{};bool prepared{};
};
class WeaponPoseCallBoundary {
public:
    using Original=void(*)(void*,const WeaponNativeMatrix*,const AimNativeVector*,bool,float);
    using After=void(*)(void*,const WeaponPoseCall&,bool) noexcept;
    using Prepare=bool(*)(void*,const WeaponPoseCall&,WeaponNativeMatrix&);
private:
    Original original_{};Prepare prepare_{};After after_{};void* context_{};
    inline static thread_local unsigned depth_{};
    inline static thread_local std::uint64_t nextSerial_{};
    inline static thread_local WeaponPoseScope scope_{};
    inline static thread_local WeaponNativeMatrix* candidate_{};
    std::atomic<std::uint64_t> passed_{},prepared_{},failed_{},nested_{},completed_{};
public:
    // A prepared native attachment still requires fresh owner/context, source
    // identity, current geometry and view-to-hand obstruction qualification.
    // The retail callback qualifies its local tracked-hand replacement here.
    void configure(Original original,Prepare prepare,void* context=nullptr,After after=nullptr){original_=original;prepare_=prepare;context_=context;after_=after;}
    // A model replacement belongs to this exact outer native call, not a TLS
    // boolean surviving exceptions or a subsequent call to a different weapon.
    static WeaponPoseScope scope(){return depth_==1?scope_:WeaponPoseScope{};}
    // Only the wrapper-owned argument can be committed, while the qualified
    // native call is running. Never writes the actor's borrowed source matrix.
    static bool commitPreparedAttachment(std::uint64_t serial,const WeaponNativeMatrix& matrix){
        const auto current=scope();runtime::Mat4 local{};float scale{};
        if(!serial||current.serial!=serial||current.phase!=WeaponPosePhase::Native||!current.prepared||!candidate_||
           !deriveModelMuzzle(matrix.values,matrix.values,local,scale))return false;
        *candidate_=matrix;return true;
    }
    void update(const WeaponPoseCall& call,bool pinnedCaller){
        const int incoming=errno;const bool previous=depth_!=0;const auto parent=scope_;++depth_;
        struct Restore{WeaponPoseScope parent;WeaponNativeMatrix* candidate;~Restore(){scope_=parent;candidate_=candidate;--depth_;}} restore{parent,candidate_};
        if(!previous){
            scope_={};
            // Exhaustion fails closed rather than reusing a historic ticket.
            if(pinnedCaller&&call.weapon&&nextSerial_!=UINT64_MAX)
                scope_={++nextSerial_,call.weapon,WeaponPosePhase::Prepare,false};
        }
        WeaponNativeMatrix proposed{};const auto* attachment=call.attachment;
        if(previous)++nested_;
        if(!previous&&pinnedCaller&&call.weapon&&call.attachment&&prepare_){
            try{
                if(prepare_(context_,call,proposed)){
                    runtime::Mat4 identity{};float scale{};
                    if(deriveModelMuzzle(proposed.values,proposed.values,identity,scale)){attachment=&proposed;++prepared_;}
                    else ++failed_;
                }
            }catch(...){++failed_;}
        }
        if(attachment==call.attachment)++passed_;
        errno=incoming;
        // Native Update retains animation, inherited velocity, flags, scalar,
        // model/skeleton ownership and exceptions. Never replay or retry it.
        if(!previous){scope_.phase=WeaponPosePhase::Native;scope_.prepared=attachment!=call.attachment;candidate_=scope_.prepared?&proposed:nullptr;}
        original_(call.weapon,attachment,call.velocity,call.nativeFlag,call.nativeScalar);
        // Only successful outer native calls publish completion. The effective
        // attachment remains alive during this callback, and is never retained.
        const int nativeErrno=errno;if(!previous)scope_.phase=WeaponPosePhase::Complete;
        if(!previous&&pinnedCaller&&call.weapon&&call.attachment&&after_){
            auto effective=call;effective.attachment=attachment;
            after_(context_,effective,attachment!=call.attachment);++completed_;
        }
        errno=nativeErrno;
    }
    std::uint64_t completed()const{return completed_.load();}
    std::uint64_t passed()const{return passed_.load();}
    std::uint64_t prepared()const{return prepared_.load();}
    std::uint64_t failed()const{return failed_.load();}
    std::uint64_t nested()const{return nested_.load();}
};
}
