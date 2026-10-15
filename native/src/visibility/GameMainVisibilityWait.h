#pragma once
#include <atomic>
#include <cstdint>
namespace rdrvr::engine {
using MainVisibilityWait = void(*)(void*,std::uint32_t);
inline std::atomic<bool> mainVisibilityWaitHookReady{};
// PRIVATE integration callback implemented by the GameXr controller. It must
// ALWAYS invoke original(manager,request) exactly once, on every path, and
// preserve that invocation's errno. Use NativeMainVisibilityBoundary::wait:
// provider publication follows the completed ORIGINAL join/sort, never a poll.
// Only the frozen PRIMARY LEFT-depth scope can grant ready-list authority;
// private RIGHT uses the same native caller address and must just forward.
void gameMainVisibilityWait(MainVisibilityWait original,void* manager,
    std::uint32_t request,std::uintptr_t returnOffset);
}
