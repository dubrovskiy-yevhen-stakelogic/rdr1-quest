#pragma once
#include <cerrno>
#include <cstddef>
#include <cstdint>
namespace rdrvr::engine {
enum class NativeScopeMode { Unknown, Inactive, Active, Transition };
struct NativeScopeApi {
    bool (*read)(void*,std::uintptr_t,void*,std::size_t){};void* context{};
    std::uintptr_t singleton{},table{},disableParam{},singletonTable{};
    explicit operator bool()const{return read&&singleton&&table&&disableParam;}
};
// SetSniperScopeState writes requested +0x29. PreDraw copies it to active
// +0x28. Keep the whole stock scope frame through both sides of that boundary.
// PARAM_nonewsniperscope+0x18 suppresses this feature even before manager Init.
class NativeScopeContext {
    NativeScopeApi api_;
    template<class T>bool get(std::uintptr_t object,std::size_t offset,T& out)const{
        return object&&object<=UINTPTR_MAX-offset&&object+offset<=UINTPTR_MAX-sizeof(T)&&
            api_.read(api_.context,object+offset,&out,sizeof(T));
    }
    struct Identity {
        std::uintptr_t disabled{},manager{},table{};std::uint16_t states{};
        bool operator==(const Identity&)const=default;
    };
    bool identity(Identity& result)const{
        Identity s{};
        if(!get(api_.disableParam,0x18,s.disabled))return false;
        if(!s.disabled&&(!get(api_.singleton,0,s.manager)||!get(s.manager,0,s.table)||(s.table!=api_.table&&(!api_.singletonTable||s.table!=api_.singletonTable))||
            !get(s.manager,0x28,s.states)||(s.states&0xff)>1||(s.states>>8)>1))return false;
        result=s;return true;
    }
public:
    explicit NativeScopeContext(NativeScopeApi api):api_(api){}
    NativeScopeMode sample()const noexcept{
        const int saved=errno;struct Restore{int saved;~Restore(){errno=saved;}} restore{saved};
        try{
            Identity before{},after{};
            if(!api_||!identity(before)||!identity(after)||before!=after)return NativeScopeMode::Unknown;
            if(before.disabled)return NativeScopeMode::Inactive;
            const auto active=before.states&0xff,requested=before.states>>8;
            if(active!=requested)return NativeScopeMode::Transition;
            return active?NativeScopeMode::Active:NativeScopeMode::Inactive;
        }catch(...){return NativeScopeMode::Unknown;}
    }
};
}
