#pragma once
#include <atomic>
#include <cstdint>
namespace rdrvr::runtime {
// Engine main-thread updates (qualified sagShellDerived::UpdateInputs, TraceEntry nativeUpdateInputs). FramePacer REGRID input:
// pace() runs only on stereo frames, so the delta since the previous pace() counts the engine frames without a pair.
inline std::atomic<std::uint64_t> engineMainFrames{};
}
