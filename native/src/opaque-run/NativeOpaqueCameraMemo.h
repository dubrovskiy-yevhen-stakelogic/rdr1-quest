#pragma once
#include "NativeOpaqueRun.h"
#include <type_traits>
#include <thread>

namespace rdrvr::experimental {
// Generic COPIED camera/current-pass result memo, not a skip permit.
// Key equality MUST include exact Description(scene/pose/stage/controller),
// viewport, matrix buffer, resource/device and configuration identities used
// by the full capture. Value owns only copied POD; native pointers are numeric
// identities and may not be dereferenced under this memo's authority.
// Value must expose optional imageTicket with bool/current(), as CurrentPass.
// populate ONLY after successful full native camera/pass proof under its real
// short ResourceRead. The required validation callback verifies that exact
// physical Read, current configuration/resources and any independent revision.
// It must be read-only/non-calling; this class never acquires a native lease.
// Strong final skip/source publication checks are STILL required by the caller.
template<class Key,class Value>
class NativeOpaqueCameraMemo {
    static_assert(std::is_trivially_copyable_v<Key>);
    static_assert(std::is_trivially_copyable_v<Value>);
public:
    using RunTicket=NativeOpaqueRun::Ticket;
    struct Authority {
        void* context{};
        bool(*current)(void*,const Key&,const Value&)noexcept{};
    };
    NativeOpaqueCameraMemo()noexcept=default;
    NativeOpaqueCameraMemo(const NativeOpaqueCameraMemo&)=delete;
    NativeOpaqueCameraMemo&operator=(const NativeOpaqueCameraMemo&)=delete;
    void reset()noexcept{if(owned())valid_=false;}
    bool populate(const RunTicket& run,const Key& key,const Value& value,Authority authority)noexcept {
        if(!owned())return false;
        valid_=false;
        if(!run.current()||!authority.current||!imageCurrent(value)||
            !authority.current(authority.context,key,value)||!imageCurrent(value)||!run.current())return false;
        key_=key;value_=value;authority_=authority;run_=run;valid_=true;return true;
    }
    // False never overwrites output. Cache mismatches discard the old value;
    // callers perform the full capture and may populate a new qualified run.
    bool get(const RunTicket& current,const Key& key,Value& out)noexcept {
        if(!owned())return false;
        if(!valid_||!(key_==key)||!run_.sameRun(current)||!imageCurrent(value_)||
           !authority_.current||!authority_.current(authority_.context,key_,value_)||
           !imageCurrent(value_)||!run_.sameRun(current)){
            valid_=false;return false;
        }
        out=value_;return true;
    }
private:
    const std::thread::id thread_=std::this_thread::get_id();
    Key key_{};Value value_{};Authority authority_{};RunTicket run_{};bool valid_{};
    bool owned()const noexcept{return thread_==std::this_thread::get_id();}
    static bool imageCurrent(const Value& value)noexcept {
        return !value.imageTicket||value.imageTicket.current();
    }
};
}
