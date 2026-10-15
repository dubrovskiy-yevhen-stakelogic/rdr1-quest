#pragma once
// Lightweight GPU frame span of the engine frame command buffer, measured in every
// profiler mode. Outside DETAIL a single TOP_OF_PIPE (CmdBegin) / BOTTOM_OF_PIPE (CmdEnd) timestamp pair per frame in
// its own 4-slot query ring, read back without waiting; in DETAIL the stage readBack feeds the same ring.
// Group B1 defines the functions (StereoGpuStages.inc / StereoReplay.cpp) and includes this header from StereoReplay.h.
// Readers: FV (FOVEATION AUTO), PC (FRAME RATE ADAPTIVE via GameXr's framePacer.pace), GameXr (gpuLiteArmed).
// All functions are lock-free and callable from any thread. A host test that calls them must link B1's definitions
// or provide its own fakes.
#include <atomic>
#include <cstdint>

namespace rdrvr::engine::stereo {
// Lite timestamps are written while true (GameXr stores !DETAIL each frame; DETAIL's stage timestamps win anyway).
inline std::atomic<bool> gpuLiteArmed{true};
inline constexpr unsigned gpuLiteWindowMax=72; // the newest-spans window of gpuLitePercentileUs
std::uint32_t gpuLiteFrameUs() noexcept;  // newest engine-frame CmdBegin->CmdEnd span in microseconds, 0 = none yet
std::uint64_t gpuLiteSerial() noexcept;   // +1 per new span (lite or DETAIL readBack); 0 = none yet
std::uint32_t gpuLitePercentileUs(unsigned pct,unsigned window) noexcept; // pct 0..100 over the newest window<=72 spans, 0 if <8 samples
inline std::uint32_t gpuLiteP90Us(unsigned w=gpuLiteWindowMax) noexcept {return gpuLitePercentileUs(90,w);}
}
