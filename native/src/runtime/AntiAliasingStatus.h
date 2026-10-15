#pragma once
#include <atomic>

namespace rdrvr::runtime {
// What the stereo mailbox really did with the last published pair (written by StereoMailbox::capture),
// for the GRAPHICS menu row and the gpuStages byState signature (_aa<effective>).
inline std::atomic<int> antiAliasingEffective{-1};  // -1 no pair yet, 0 copy (OFF requested), 1 FXAA ran, 2 FXAA requested but copied
inline std::atomic<int> antiAliasingFallback{-1};   // StereoAntiAliasing::Fallback of the last copied FXAA request (-1 none)
inline std::atomic<int> antiAliasingProperty{-1};   // debug.rdrvr.aa override in effect (-1 none)
// fxaa.frag debugMode of the last FXAA pair (0 plain FXAA, 1 EDGES, 2 SPLIT; 0 when no FXAA ran),
// stored by StereoMailbox::capture next to antiAliasingEffective (byState signature _aa1d<mode>).
inline std::atomic<int> antiAliasingDebug{0};
// TAA (vr/TaaPass.h): the ANTI-ALIASING row value in effect (menu or debug.rdrvr.aa), stored by
// StereoMailbox::antiAliasingRequest; the render-thread TAA pass runs for it (one capture of lag).
// 0 OFF, 1 TAA (no jitter), 2 TAA FULL (projection jitter, vr/TaaJitter.h),
// 3 TAA LITE (cheap TAA: 8-bit history in the eye format, no aux, centred area).
inline std::atomic<int> antiAliasingRequested{-1};
inline constexpr int antiAliasingRowTaa=1,antiAliasingRowTaaFull=2,antiAliasingRowTaaLite=3; // = GraphicsSettings::antiAliasingTaa/TaaFull/TaaLite (static_assert in StereoMailbox.cpp)
// TAA module state for the menu row suffix: -1 not configured, 0 unavailable (debug.rdrvr.vr.taa=0), 1 building,
// 2 ready, 3 failed, 4 no usable eye depth, 5 eye format unsupported.
inline std::atomic<int> taaMenuState{-1};
// TAA modes: 1 = the mailbox FXAA runs after TAA, 0 = plain copy after TAA (debug.rdrvr.vr.taa.fxaa, read by the TAA module).
inline std::atomic<int> taaPostFxaa{0};
// TAA LITE sharpness from the ANTI-ALIASING row (GraphicsSettings::taaSharpness, published on load / set): 0 NORMAL, 1 SHARP,
// 2 SHARPER -> TaaPass sub-pixel weight cap k 1 / 2 / 4 (debug.rdrvr.vr.taa.lite.subpx, when set, still wins). Band ratios
// (L after TAA / R raw, 0.16-0.25 / 0.25-0.35 cycles/px): still k1 0.90/0.81, k4 0.92/0.90; moving k1 0.68-0.84/0.67-0.72, k4 0.81-0.90/0.81-0.84.
inline std::atomic<int> taaSharpness{0};
// Same order as StereoAntiAliasing::Fallback (static_assert there).
inline constexpr unsigned antiAliasingFallbackCount=6;
inline const char* antiAliasingFallbackText(int reason) noexcept {
    switch(reason){case 0:return "SOURCE NOT SAMPLED";case 1:return "FORMAT";case 2:return "EYE FORMATS";
    case 3:return "CREATE FAILED";case 4:return "VIEW FAILED";case 5:return "BUILDING";default:return "?";}
}
}
