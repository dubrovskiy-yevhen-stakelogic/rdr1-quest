#pragma once
// The only part of the TAA module EngineStereoPass.cpp sees. Same call contract as the hands hook
// (HandsHook.h EyeCall): once per eye on the render thread, in the engine's frame command buffer, outside any engine
// render pass, after the eye's colour was closed and read and BEFORE the hands hook and capture_.recordEye(eye) (the
// hands never enter the TAA history; the mailbox FXAA/copy reads the TAA result). The hook leaves every image in the
// layout it received and restores the engine's bound command state; its result never affects the pair.
// A null hook (module killed / not configured) costs one relaxed atomic load per eye.
#include "HandsHook.h"
#include <atomic>

namespace rdrvr::vr::taa {
using EyeHook=bool(*)(const hands::EyeCall&) noexcept;
inline std::atomic<EyeHook> eyeHook{nullptr};
// ARC: (renderFrame << 1 | eye) of the last eye the TAA hook recorded (render thread); the hands pass buckets its GPU time by
// "TAA ran on this eye" (vr.hands.arc.gpu.split). 0 = never.
inline std::atomic<std::uint64_t> recordedEyeKey{0};
} // namespace rdrvr::vr::taa
