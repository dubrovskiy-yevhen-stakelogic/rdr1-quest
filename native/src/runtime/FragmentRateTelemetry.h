#pragma once
#include <atomic>
namespace rdrvr::runtime {
// 0 disabled, 1 device/full-rate fallback ready, 2 owned eye framebuffer built.
// This is resource readiness, never a measurement of GPU load or frame rate.
inline std::atomic<unsigned> fragmentRateReadiness{};
}
