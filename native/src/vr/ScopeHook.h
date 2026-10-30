#pragma once
// The only part of the sniper scope module EngineStereoPass.cpp sees. Same call contract as the hands hook (HandsHook.h EyeCall):
// once per eye on the render thread, in the engine's frame command buffer, outside any engine render pass, after the TAA hook
// and BEFORE the hands hook and capture_.recordEye(eye). Leaves every image in its received layout and restores the engine's
// bound command state; its result never affects the pair. A null hook costs one relaxed atomic load per eye.
#include "HandsHook.h"
#include <atomic>

namespace rdrvr::vr::scope {
using EyeHook=bool(*)(const hands::EyeCall&) noexcept;
inline std::atomic<EyeHook> eyeHook{nullptr};
} // namespace rdrvr::vr::scope
