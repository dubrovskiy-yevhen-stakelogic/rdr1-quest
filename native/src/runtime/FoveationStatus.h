#pragma once
#include <atomic>

namespace rdrvr::runtime {
// What the GRAPHICS menu shows for FOVEATION / FOVEATION API (written by engine/GameFoveation).
// foveationActiveApi (GraphicsSettings.h) is the backend really enabled on the game device.
inline std::atomic<int> foveationStartupApi{-1};          // graphics.cfg foveationApi when the device was created
inline std::atomic<bool> foveationApiProperty{};          // debug.rdrvr.foveation.api decided the device request
inline std::atomic<int> foveationLevelProperty{-1};       // debug.rdrvr.foveation override in effect (-1: none)
inline std::atomic<const char*> foveationApiNote{nullptr}; // short reason when the active API is not the requested one
}
