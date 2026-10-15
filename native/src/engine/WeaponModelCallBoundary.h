#pragma once
#include "WeaponAttachmentReader.h"
#include <atomic>
namespace rdrvr::engine {
struct WeaponModelCall {void* model{};const WeaponNativeMatrix* matrix{};};
// ABI boundary for the EXISTING bool goh::SetTransform(Matrix34 const&) call.
// Not installed by this class. The prepare callback must qualify the exact
// outer weapon Update, current model identity and pinned native caller.
class WeaponModelCallBoundary {
public:
    using Original=bool(*)(void*,const WeaponNativeMatrix*);
    using Prepare=bool(*)(void*,const WeaponModelCall&,WeaponNativeMatrix&);
    using After=void(*)(void*,const WeaponModelCall&,bool,bool)noexcept;
private:
    Original original_{};Prepare prepare_{};After after_{};void* context_{};
    inline static thread_local bool inside_{};
    std::atomic<std::uint64_t> prepared_{},passed_{},failed_{},nested_{},completed_{},nativeTrue_{};
public:
    void configure(Original original,Prepare prepare,void* context=nullptr,After after=nullptr){
        original_=original;prepare_=prepare;context_=context;after_=after;
    }
    bool transform(const WeaponModelCall& call,bool pinnedCaller){
        const int incoming=errno;const bool previous=inside_;inside_=true;
        struct Restore{bool previous;~Restore(){inside_=previous;}} restore{previous};
        WeaponNativeMatrix proposed{};const auto* matrix=call.matrix;
        if(previous)++nested_;
        if(!previous&&pinnedCaller&&call.model&&call.matrix&&prepare_){
            try{
                if(prepare_(context_,call,proposed)){
                    runtime::Mat4 local{};float scale{};
                    if(deriveModelMuzzle(proposed.values,proposed.values,local,scale)){matrix=&proposed;++prepared_;}
                    else ++failed_;
                }
            }catch(...){++failed_;}
        }
        if(matrix==call.matrix)++passed_;errno=incoming;
        // Preserve the return value even when native movement reports false.
        // No retry, duplicate physics, or synthetic success after an exception.
        const bool result=original_(call.model,matrix);const int nativeErrno=errno;
        if(!previous&&pinnedCaller&&call.model&&call.matrix){
            if(after_){auto effective=call;effective.matrix=matrix;after_(context_,effective,matrix!=call.matrix,result);}
            ++completed_;if(result)++nativeTrue_;
        }
        errno=nativeErrno;return result;
    }
    std::uint64_t prepared()const{return prepared_.load();}
    std::uint64_t passed()const{return passed_.load();}
    std::uint64_t failed()const{return failed_.load();}
    std::uint64_t nested()const{return nested_.load();}
    std::uint64_t completed()const{return completed_.load();}
    std::uint64_t nativeTrue()const{return nativeTrue_.load();}
};
}
