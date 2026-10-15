#pragma once
#include <array>
#include <cstdint>
namespace rdr92 {
struct VmaHookRoute {std::uintptr_t slot{},original{},replacement{};};
// At the certified pre-InitVulkan boundary. Inspect every slot before mutation;
// failed installation leaves persistence disabled even if rollback itself fails.
template<class Read,class Replace>
bool installVmaHooks(const std::array<VmaHookRoute,3>& routes,Read read,Replace replace,bool& rollbackFault){
    for(const auto& r:routes)if(!r.slot||!r.original||!r.replacement||!read(r.slot,r.original))return false;
    unsigned patched{};
    while(patched<routes.size()){
        const auto& r=routes[patched];if(!replace(r.slot,r.original,r.replacement))break;++patched;
    }
    if(patched==routes.size())return true;
    while(patched){const auto& r=routes[--patched];if(!replace(r.slot,r.replacement,r.original))rollbackFault=true;}
    return false;
}
}
