#pragma once
#include <array>
#include <cstdint>
#include <type_traits>
namespace rdrvr::engine {
// Both native callers inline EndTreeRenderTimer. Their GetNumBuffers PLT
// call immediately precedes the unsafe vector.back() / EndTimeQuery block.
// Only verified return PCs may resume after that block during eye replay.
// Keep this a C-compatible trivial aggregate: AAPCS64 returns it in x0/x1.
struct TreeTimerReturn {std::uintptr_t value,resume;};
static_assert(std::is_trivial_v<TreeTimerReturn> && std::is_standard_layout_v<TreeTimerReturn>);
static_assert(sizeof(TreeTimerReturn)==2*sizeof(std::uintptr_t));
class TreeTimerRouting {
public:
    using Count=unsigned(*)(const void*);
    struct Site {std::uintptr_t caller{},resume{};};
private:
    Count count_{};bool(*skip_)(){};std::array<Site,2> sites_{};
public:
    void configure(Count count,bool(*skip)(),std::array<Site,2> sites){count_=count;skip_=skip;sites_=sites;}
    TreeTimerReturn dispatch(const void* display,std::uintptr_t caller)const{
        for(const auto& site:sites_)if(site.caller&&site.caller==caller&&skip_&&skip_())return {0,site.resume};
        return {count_(display),0};
    }
};
extern TreeTimerRouting treeTimerRouting;
extern "C" unsigned rdrvrTreeTimerBuffers(const void*);
}
