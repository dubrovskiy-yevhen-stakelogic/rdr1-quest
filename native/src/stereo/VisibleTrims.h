#pragma once
// User-selectable VISIBLE trims for heavy night scenes. Glue between
// the GRAPHICS rows (runtime GraphicsSettings) and the engine levers, called once per RenderScene from TraceEntry observeScene:
//  * SUN SHADOWS 7 STAGGER / NIGHT OFF (SunNight.h, via ShadowUpdate.h): returns the governed SUN SHADOWS flag that
//    GraphicsDrawPolicy::Scene gets instead of settings.shadows, publishes runtime::sunNightMenu. Kill switch
//    debug.rdrvr.sunnight 0; report graphics.shadowUpdate.sunNight; signature _su7[z].
//  * LAMP SHADOWS 4 1/3 CAP 2 / 5 OFF (LampShadows.h applyCap / clearSlots, driven by lampshadow::sceneBegin(row)):
//    publishes runtime::lampCapMenu. Kill switch debug.rdrvr.lampcap 0; report graphics.localShadow.cap/off/capByLevel; _ls<cap>...
//  * SMALL OBJECTS 24 / 32 px need no glue: SmallObjectCull reads graphicsSettings.smallObjectsPx() every MAIN frame
//    (kill switch debug.rdrvr.smallobjects 0; report smallObjects.thresholdPx/thresholdSource; signature _so24/_so32).
#include "runtime/GraphicsSettings.h"
#include "ShadowUpdate.h"
#include "LampShadows.h"
#include <chrono>
#include <cstdint>

namespace rdrvr::engine::trims {
static_assert(shadowupdate::sunnight::rowValue==runtime::GraphicsSettings::shadowUpdateNightOff&&
              shadowupdate::sunnight::dayLevel==runtime::GraphicsSettings::shadowUpdateStagger);
static_assert(shadowupdate::menuLevels==runtime::GraphicsSettings::shadowUpdateLevels);
static_assert(lampshadow::presetCount==runtime::GraphicsSettings::lampShadowsPresets&&
              lampshadow::presetLevers(runtime::GraphicsSettings::lampShadowsCap2).cap==1&&
              lampshadow::presetLevers(runtime::GraphicsSettings::lampShadowsOff).cap==lampshadow::capOff);
// observeScene, every RenderScene, after lampshadow::sceneBegin. Returns the SUN SHADOWS flag for this scene's draw policy.
inline bool sceneBegin(const runtime::GraphicsSettings::Settings& s) noexcept {
    namespace sn=shadowupdate::sunnight;
    const auto now=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    const bool shadows=sn::sceneBegin(s.shadows,s.shadowUpdate,now);
    runtime::sunNightMenu.store(sn::menuState(),std::memory_order_relaxed);
    runtime::lampCapMenu.store(lampshadow::capMenuState(),std::memory_order_relaxed);
    return shadows;
}
} // namespace rdrvr::engine::trims
