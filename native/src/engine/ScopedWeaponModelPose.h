#pragma once
#include "WeaponPoseCallBoundary.h"
#include "WeaponModelCallBoundary.h"
#include "WeaponModelBindingReader.h"
namespace rdrvr::engine {
// Thread-owned transaction for one EXISTING UpdateSkeleton -> SetTransform.
// Native depth/phase/serial, exact borrowed argument and live GUID generation
// must all agree. It cannot invoke native physics or retain a usable permit
// after Update unwinds, even if its completion callback is never reached.
class ScopedWeaponModelPose {
    WeaponPoseScope scope_{};LocalWeapon owner_{};WeaponModelBinding binding_{};
    WeaponNativeMatrix source_{},replacement_{},attachment_{};bool nativeFlag_{},armed_{},attempted_{},completed_{},nativeResult_{};
    bool inPhase(WeaponPosePhase phase)const{
        const auto current=WeaponPoseCallBoundary::scope();
        return scope_.serial&&current.serial==scope_.serial&&current.weapon==scope_.weapon&&
            current.phase==phase&&current.prepared;
    }
public:
    void reset(){scope_={};armed_=attempted_=completed_=nativeResult_=false;}
    bool arm(const LocalWeapon& owner,const WeaponModelBinding& binding,bool nativeFlag,
             const runtime::Mat4& source,const runtime::Mat4& replacement,const runtime::Mat4& attachment){
        reset();const auto scope=WeaponPoseCallBoundary::scope();runtime::Mat4 local{};float scale{};
        if(!scope.serial||scope.phase!=WeaponPosePhase::Prepare||scope.weapon!=reinterpret_cast<void*>(owner.weapon)||
           binding.weapon!=owner.weapon||!binding.model||!binding.modelGuid||!(binding.flags&0x20)||
           !deriveModelMuzzle(source,source,local,scale)||!deriveModelMuzzle(replacement,replacement,local,scale)||
           !deriveModelMuzzle(attachment,attachment,local,scale))return false;
        scope_=scope;owner_=owner;binding_=binding;nativeFlag_=nativeFlag;
        source_.values=source;replacement_.values=replacement;attachment_.values=attachment;armed_=true;return true;
    }
    bool prepare(WeaponModelBindingApi modelApi,WeaponAttachmentApi geometryApi,
                 const WeaponModelCall& call,WeaponNativeMatrix& out)noexcept{
        const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};
        if(!armed_||attempted_||!inPhase(WeaponPosePhase::Native))return false;
        // Consume even a failed attempt. Never redirect a second physics call.
        attempted_=true;
        if(call.model!=reinterpret_cast<void*>(binding_.model)||owner_.weapon>UINTPTR_MAX-0x80||
           reinterpret_cast<std::uintptr_t>(call.matrix)!=owner_.weapon+0x80)return false;
        WeaponModelBinding current{};runtime::Mat4 source{};
        if(!WeaponModelBindingReader(modelApi).inspect(owner_,nativeFlag_,current)||current!=binding_||
           !WeaponAttachmentReader(geometryApi).argument(call.matrix,source)||source!=source_.values)return false;
        out=replacement_;return true;
    }
    void after(const WeaponModelCall& call,bool prepared,bool nativeResult)noexcept{
        completed_=false;nativeResult_=false;
        if(!armed_||!attempted_||!prepared||!inPhase(WeaponPosePhase::Native)||
           call.model!=reinterpret_cast<void*>(binding_.model)||!call.matrix||call.matrix->values!=replacement_.values)return;
        // The effective stack argument is valid only during the ABI callback.
        // A true native return is recorded, never treated as proof of bone pose.
        if(nativeResult&&!WeaponPoseCallBoundary::commitPreparedAttachment(scope_.serial,attachment_))return;
        completed_=true;nativeResult_=nativeResult;
    }
    bool completed(bool& nativeResult)const{
        if(!completed_||!inPhase(WeaponPosePhase::Complete))return false;
        nativeResult=nativeResult_;return true;
    }
};
}
