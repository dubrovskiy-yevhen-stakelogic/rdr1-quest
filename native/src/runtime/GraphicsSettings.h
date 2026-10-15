#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace rdrvr::runtime {
inline std::atomic<bool> graphicsOcclusionAvailable{};
// Foveation backend actually enabled on the game device (-1 unknown, 0 VRS, 1 FDM, 2 none), for the menu.
inline std::atomic<int> foveationActiveApi{-1};
// XR_FB_space_warp for the menu: support (-1 unknown, 0 no, 1 yes) and the compositor's
// /perfmetrics_meta/compositor/spacewarp_mode (-1 not polled), published by the XR thread.
inline std::atomic<int> spaceWarpMenuSupport{-1},spaceWarpMenuRuntime{-1};
// SPACEWARP really synthesizing (and forcing LOCK 36), and why not while it is ON (nullptr = no issue).
inline std::atomic<int> spaceWarpMenuForced{0};inline std::atomic<const char*> spaceWarpMenuIssue{nullptr};
// SHADOW MAP level the engine's shadow target was created with (-1 unknown) and its square extent
// (0 unknown), published by the engine hooks for the menu's RESTART hint.
inline std::atomic<int> shadowMapStartupLevel{-1};
inline std::atomic<unsigned> shadowMapCascadeExtent{0};
// SHADOW UPDATE hooks (CShadowMap::RenderIntoShadowMaps + sphere restore) installed: -1 unknown, 0 no, 1 yes.
inline std::atomic<int> shadowUpdateReady{-1};
// SHADOW UPDATE 5 STAGGER / 6 STAGGER FAR usable (SunStagger.h: fingerprints, both hooks, no fault-disable,
// raw clear bound): -1 unknown, 0 the levels act as EVERY, 1 yes. sunStaggerGate: 1 while the indoor gate holds the
// atlas at a stagger level, 0 not, -1 not at a stagger level. [TraceEntry]
inline std::atomic<int> sunStaggerReady{-1};
inline std::atomic<int> sunStaggerGate{-1};
// Status published by the subsystems for the GRAPHICS menu, the report and the byState
// signature. -1 = not published yet (subsystem not built or not initialised). Writers in brackets.
inline std::atomic<int> postFxMenuState{-1};      // [TraceEntry/PostFxSkip] 0 missing, 1 ready, 2 calibrating, 3 OFF FAST active, 4 fallback
inline std::atomic<int> postFxMenuProperty{-1};   // [TraceEntry/PostFxSkip] debug.rdrvr.postfx override in effect (-1 none)
inline std::atomic<int> textureFilterReady{-1};   // [EN] 0 unavailable (fingerprint), 1 live, 2 boot-only (applies at restart)
inline std::atomic<int> cascadeCullReady{-1};     // [SH via TraceEntry] CS2/CS3 fingerprint 0/1
inline std::atomic<int> lampShadowsReady{-1};     // [TraceEntry] LAMP SHADOWS hooks (LampShadows.h) 0/1
inline std::atomic<int> lampShadowsProperty{-1};  // [TraceEntry] 1 while a debug.rdrvr.lamp*/spotcascade property overrides the row
// [VisibleTrims.h] SUN SHADOWS 7 STAGGER / NIGHT OFF gate (SunNight.h). Low 3 bits: 0 not engaged,
// 1 DAY (shadows on), 2 NIGHT (shadows off now), 3 NO CLOCK (acts as STAGGER), 4 debug.rdrvr.sunnight 0 (kill switch, acts as
// STAGGER), 5 debug.rdrvr.sunnight 2 (forced off now); +8 while debug.rdrvr.sunnight is set. -1 = not published.
inline std::atomic<int> sunNightMenu{-1};
// [VisibleTrims.h] LAMP SHADOWS point-light cap lever (LampShadows.h applyCap): -1 not published, 0 unavailable
// (code/GOT/SelectionParams fingerprint), 1 usable, 2 suspended (a foreign value holds the cap).
inline std::atomic<int> lampCapMenu{-1};
// w02 WATER row: graphics.cfg water (0 OFF / 1 ON), published by GraphicsSettings on load / set / defaults and read every stereo frame by
// GameXr (eyeWaterMode); waterMenuProperty = debug.rdrvr.vr.water as GameXr last read it (-1 unset, 0 OFF, 1 ON, 2 ON NO FILTER) for PROP.
inline std::atomic<int> waterSetting{1},waterMenuProperty{-1};
inline std::atomic<int> cameraIndoorMenu{-1};     // [GameXr from indoor::flag()] -1 unknown, 0 outside, 1 in a room
inline std::atomic<int> frameRateGovernorHz{0};   // [PC] FRAME RATE ADAPTIVE target 36/24; 0 = governor idle / not built
inline std::atomic<int> foveationLiveLevel{-1};   // [FV] foveation level applied this frame (AUTO/SWEEP/property/clamp)
inline std::atomic<int> xrTimespecEnabled{-1};       // [GameXr] XR_KHR_convert_timespec_time enabled on the instance (0/1)
inline std::atomic<int> xrLayerSettingsEnabled{-1};  // [GameXr] XR_FB_composition_layer_settings enabled (0/1)
inline std::atomic<int> xrAutoLayerFilterEnabled{-1};// [GameXr] XR_META_automatic_layer_filter enabled (0/1)
inline std::atomic<int> xrDynresEnabled{-1};         // [GameXr] dynres01: XR_META_recommended_layer_resolution enabled (0/1; debug.rdrvr.dynres 0 = not requested)
// dynres01 [XrHarness]: xrGetRecommendedLayerResolutionMETA on the world projection layer, once per XR frame that submits it. Experiment
// (GPU levels 5/6 on battery need "dynamic resolution" on Quest 3/3S): query + report only (gameBootstrap.xrOptionalExtensions.dynres),
// the render resolution never follows it. listed/prop [GameXr]: runtime offers it / debug.rdrvr.dynres not "0"; fn: 1 resolved, 0 missing.
struct XrDynresTelemetry {std::atomic<int> listed{-1},prop{-1},fn{-1},lastResult{0},valid{-1},w{0},h{0},rectW{0},rectH{0};
                          std::atomic<std::uint64_t> calls{0},ok{0},fail{0},validFrames{0};};
inline XrDynresTelemetry xrDynres;
inline std::atomic<int> xrOptionalExtensionsRetryResult{0}; // [GameXr] XrResult of xrCreateInstance WITH the optional extensions when it failed and was retried without them (0 = no retry)
inline std::atomic<int> sharpenEffective{0};      // [PC] SHARPEN value really chained on the projection layer (0 none)
inline std::atomic<int> xrQueueLayoutActive{-1};  // [QU] layout the device was created with: 0 Appended, 2 SplitHigh, 3 SplitNormal
inline std::atomic<const char*> xrQueueLayoutReason{nullptr}; // [QU] why that layout (static string; nullptr = not decided)
inline std::atomic<std::uint64_t> xrLayerFramesSubmitted{0};  // [PC] XR frames submitted with a world STEREO layer
// VR-only configuration. Never points at the retail game's options or saves.
class GraphicsSettings {
public:
    enum class ProfilerMode : unsigned { Off=0,Fps=1,Detail=2 };
    // SUN SHADOWS keeps STAGGER / NIGHT OFF (user's moon-shadows-off request).
    struct Settings {
        unsigned resolutionPercent{100}; // Default 100: the scene is geometry bound, so resolution is the wrong lever
        bool shadows{true},grass{true};
        unsigned edgeMarginPercent{10};
        bool occlusion{};          // cfg key occlusionV2 (EXPERIMENTAL, default OFF); legacy key "occlusion": read and ignored
        ProfilerMode profilerMode{ProfilerMode::Off};
        unsigned smallObjectsPx{16};
        unsigned foveation{3};
        unsigned foveationApi{};   // 0 VRS, 1 FDM, 2 NONE (device creation: applies after restart)
        unsigned antiAliasing{3};   // (cfg v4): 0 OFF, 1 TAA, 2 TAA FULL (projection jitter + TAA + sharpen)
                                   // 3 TAA LITE (cheap TAA: 8-bit history, centred area; menu order OFF/LITE/TAA/FULL).
                                   // Render-thread TAA per eye (vr/TaaPass.h); the mailbox pass is a copy (antiAliasingPost).
                                   // cfg v1..v3 values (0 OFF, 1 FXAA, 2 FXAA EDGES, 3 FXAA SPLIT, 4 TAA, 5 TAA SHARP) migrate: FXAA* -> OFF, TAA* -> TAA.
        unsigned taaSharpness{};   // TAA LITE sharpness 0 NORMAL, 1 SHARP, 2 SHARPER (k 1 / 2 / 4; live; on the ANTI-ALIASING row)
        unsigned frameRate{0};
        unsigned spaceWarp{1};      // 0 OFF, 1 ON, 2 ON ZERO MV, 3 ON FAR DEPTH, 4 ON INVERT, 5 ON FLIP Y (XR_FB_space_warp, live)
        unsigned shadowUpdate{7};
                                   // 5 STAGGER (one atlas tile per frame), 6 STAGGER FAR (near tile every frame + one far tile)
                                   // 7 STAGGER / NIGHT OFF = STAGGER by day, SUN SHADOWS OFF from 20:00 to 05:00 game time
                                   // (stereo/SunNight.h; ShadowUpdate.h runs it as 5)
        unsigned shadowMap{2};      // 0 NATIVE, 1 640, 2 512 (cascade map extent at creation: after restart)
        unsigned grassDistance{1};  // 0 FULL, 1 75%, 2 50%, 3 35% (grass cull/thinning distances, live)
        // (live unless noted; cfg key = field name):
        unsigned postFx{};         // 0 OFF (bind-only post, default), 1 ON, 2 OFF FAST (session-only: saved as 0)
        unsigned textureFilter{};  // 0 NATIVE, 1 8X, 2 4X, 3 2X, 4 OFF (anisotropy cap, textureFilterAnisotropy)
        unsigned mipBias{};        // 0 "0", 1 "0.25", 2 "0.5" (global mip bias, mipBiasValue)
        unsigned sharpen{};        // 0 OFF, 1 NORMAL, 2 QUALITY, 3 AUTO (XR_FB_composition_layer_settings, sharpenLayerFlags)
        unsigned cascadeCull{1};
        unsigned xrQueuePriority{};// 0 NORMAL (Appended), 1 PRIORITY (SplitHigh); device creation: applies after restart
        bool xrQueueVerified{true};// no row: false while a PRIORITY start has not yet shown 300 layer frames (QU gate)
        unsigned lampShadows{3};   // LAMP SHADOWS 0 NATIVE, 1 1/2, 2 1/2 NO CASCADE, 3 1/3 NO CASC CULL X2 (live
                                   // 4 1/3 CAP 2 (preset 3 + at most 2 shadowed point lamps), 5 OFF (no lamp shadows)
        //  lever mask (stereo/S02Levers.h bits; no menu row). Cfg key s02Mask, always hex (optional 0x, like the property), clamped
        // to s02MaskBits; written as 0x%04x. Applied every RenderScene unless debug.rdrvr.s02 is set (the property wins, 0 = native).
        unsigned s02Mask{0x1Fu};   // default 0x1F = COLOUR RESTARTS OFF | R EYE MERGE | PX TAIL | PX NO-FLUSH | INDOOR RT
        bool heatWarning{true};    // heat01: red HEADSET OVERHEATING lines (HeatWarning.h; live, published as heatWarningSetting; cfg version stays 4)
        unsigned gpuBoost{2};      // boost01: GPU BOOST 0 OFF, 1 640, 2 690 (GpuBoost.h; live, published as gpuBoostSetting; written only when not 2)
        bool water{true};          // w02: WATER in the eyes (GameXr eyeWaterMode; live, published as waterSetting; written only when OFF)
        bool operator==(const Settings&) const = default;
    };
    struct Extent {std::uint32_t width{},height{};bool operator==(const Extent&) const = default;};
    struct Snapshot {
        Settings requested{};
        unsigned startupResolutionPercent{100};
        Extent applied{};
        bool loaded{},pendingRestart{},persistenceOk{true};
        int legacyOcclusion{-1};           // Value of the ignored pre-V2 key "occlusion" in the loaded file, -1 absent
        unsigned startupXrQueuePriority{}; // xrQueuePriority as loaded at startup (the device was created with it)
        // cfg format. cfgVersionLoaded = version= of the file accepted at startup (0: no file / rejected file).
        // cfgMigrated: that file had version < cfgVersion: its antiAliasing was mapped and, for version < 3, the ship keys
        // (migrateShipDefaults) were set to the new defaults;
        // cfgMigrationSaved: the migrated file (version=4) was written. s02MaskClamped: the loaded s02Mask had bits outside s02MaskBits.
        unsigned cfgVersionLoaded{};
        bool cfgMigrated{},cfgMigrationSaved{},s02MaskClamped{};
    };
    inline static constexpr std::array<unsigned,9> resolutionValues{50,67,75,83,90,100,117,133,150};
    inline static constexpr std::array<unsigned,3> edgeMarginValues{0,10,20};
    // 24 and 32 px are visible trims; cfg stays version 4 (an older build rejects a file with 24/32)
    inline static constexpr std::array<unsigned,7> smallObjectsValues{0,2,4,8,16,24,32};
    static bool validSmallObjects(unsigned) noexcept;
    static constexpr unsigned foveationLevels=6,foveationApis=3;
    static const char* foveationName(unsigned) noexcept;
    static const char* foveationApiName(unsigned) noexcept;
    // The ANTI-ALIASING row is OFF / TAA / TAA FULL (no FXAA rows; the mailbox
    // FXAA pass stays as the optional post of the TAA modes, debug.rdrvr.vr.taa.fxaa=1). cfg v1..v3 values: legacyAntiAliasing().
    // 3 = TAA LITE (cheap TAA, vr/TaaPass.h). cfg stays version 4: 0..2 keep their meaning (an older
    // build rejects a file with antiAliasing=3 = the usual downgrade cost). The menu shows the row in cost order
    // OFF -> TAA LITE -> TAA -> TAA FULL (antiAliasingMenuOrder / antiAliasingAtMenuPosition), the stored values are unchanged.
    static constexpr unsigned antiAliasingModes=4;
    static constexpr unsigned antiAliasingTaa=1,antiAliasingTaaFull=2,antiAliasingTaaLite=3;
    // Every TAA row value runs TAA LITE (same look at a fraction of the cost); menu OFF <-> TAA LITE.
    static constexpr unsigned antiAliasingLiteOnly(unsigned v) noexcept {return v==0u?0u:antiAliasingTaaLite;}
    static constexpr unsigned antiAliasingMenuModes=2;
    static constexpr unsigned antiAliasingMenuOrder(unsigned v) noexcept {return v==antiAliasingTaaLite?1u:v==antiAliasingTaa?2u:v==antiAliasingTaaFull?3u:0u;}
    static constexpr unsigned antiAliasingAtMenuPosition(unsigned p) noexcept {return p==1?antiAliasingTaaLite:p==2?antiAliasingTaa:p==3?antiAliasingTaaFull:0u;}
    static constexpr unsigned legacyAntiAliasingModes=6; // cfg v1..v3: OFF/FXAA/FXAA EDGES/FXAA SPLIT/TAA/TAA SHARP
    static constexpr unsigned legacyAntiAliasing(unsigned v) noexcept {return v>=4&&v<legacyAntiAliasingModes?antiAliasingTaa:0u;}
    // The mailbox pass of a row value: OFF -> copy; TAA / TAA FULL -> copy (default) or FXAA (postFxaa, debug.rdrvr.vr.taa.fxaa=1).
    // Result: 0 copy, 1 FXAA (StereoMailbox's pass request).
    static constexpr unsigned antiAliasingPost(unsigned mode,bool postFxaa=false) noexcept {return mode>=antiAliasingTaa&&mode<antiAliasingModes&&postFxaa?1u:0u;}
    static const char* antiAliasingName(unsigned) noexcept;
    static constexpr unsigned frameRateModes=4,spaceWarpModes=7;
    static const char* frameRateName(unsigned) noexcept;
    static const char* spaceWarpName(unsigned) noexcept;
    static constexpr unsigned shadowUpdateLevels=8,shadowMapLevels=3,grassDistanceLevels=4;
    static constexpr unsigned legacyShadowQualityLevels=4; // Legacy key shadowQuality: accepted and ignored
    static const char* shadowUpdateName(unsigned) noexcept;
    static const char* shadowMapName(unsigned) noexcept;
    static const char* grassDistanceName(unsigned) noexcept;
    // Further rows. Each count is the cfg bound (a value >= count is rejected).
    static constexpr unsigned postFxModes=3,textureFilterLevels=5,mipBiasLevels=3,sharpenModes=4,cascadeCullLevels=3,xrQueuePriorities=2;
    static constexpr unsigned foveationUltra=4,foveationAuto=5,frameRateAdaptive=3,shadowUpdateIndoor=4,postFxOffFast=2;
    static constexpr unsigned shadowUpdateStagger=5,shadowUpdateStaggerFar=6;
    // SUN SHADOWS "STAGGER / NIGHT OFF" (cfg shadowUpdate=7, version stays 4; an older build rejects such a file).
    static constexpr unsigned shadowUpdateNightOff=7;
    static const char* postFxName(unsigned) noexcept;          // OFF, ON, OFF FAST
    static const char* textureFilterName(unsigned) noexcept;   // NATIVE, 8X, 4X, 2X, OFF
    static const char* mipBiasName(unsigned) noexcept;         // 0, 0.25, 0.5
    static const char* sharpenName(unsigned) noexcept;         // OFF, NORMAL, QUALITY, AUTO
    static const char* cascadeCullName(unsigned) noexcept;     // NATIVE, X2, X4
    static const char* xrQueuePriorityName(unsigned) noexcept; // NORMAL, PRIORITY
    static constexpr unsigned lampShadowsPresets=6;
    static constexpr unsigned lampShadowsCap2=4,lampShadowsOff=5;
    // cfg format 3 = ship defaults. A file with version 1 or 2 loads, then gets the six ship keys (lampShadows,
    // shadowUpdate, foveation, cascadeCull, frameRate, s02Mask) set to Settings{} values and is saved once as version=3.
    // cfg format 4 = the OFF/TAA/TAA FULL antiAliasing values. A file with version 1..3 has its antiAliasing mapped by
    // legacyAntiAliasing (the ship keys only for version < 3) and is saved once as version=4.
    static constexpr unsigned cfgVersion=4,cfgShipVersion=3;
    static constexpr unsigned s02MaskBits=0x0FFFu,s02MaskDefault=0x1Fu;
    // s02Mask value: always hex like the debug.rdrvr.s02 property: optional "0x"/"0X", then 1..8 hex digits
    // ("1f" = "0x1F" = 0x1F, "10" = 0x10). false = syntax error (the file is rejected like any other bad value).
    // out = value & s02MaskBits; clamped = bits were dropped.
    static bool parseS02Mask(std::string_view text,unsigned& out,bool& clamped) noexcept;
    static void migrateShipDefaults(Settings&) noexcept; // the six ship keys = Settings{} values, every other key kept
    static const char* lampShadowsName(unsigned) noexcept;      // NATIVE, 1/2, 1/2 NO CASCADE, 1/3 NO CASC CULL X2, 1/3 CAP 2, OFF
    // Requested anisotropy cap (0 = engine native, 1 = filtering off), global mip bias, CS2/CS3 threshold scale.
    static constexpr unsigned textureFilterAnisotropy(unsigned l) noexcept {return l==1?8u:l==2?4u:l==3?2u:l==4?1u:0u;}
    static constexpr float mipBiasValue(unsigned l) noexcept {return l==1?.25f:l==2?.5f:0.f;}
    static constexpr float cascadeCullScale(unsigned l) noexcept {return l==1?2.f:l==2?4.f:1.f;}
    // XrCompositionLayerSettingsFB::layerFlags: NORMAL 0x4 (NORMAL_SHARPENING), QUALITY 0x8 (QUALITY_SHARPENING), AUTO
    // 0x20|0x8 (AUTO_LAYER_FILTER_BIT_META | QUALITY_SHARPENING), or 0x4 without XR_META_automatic_layer_filter.
    // Never a super-sampling bit (0x1/0x2).
    static constexpr std::uint64_t sharpenLayerFlags(unsigned m,bool autoLayerFilter=true) noexcept {
        return m==1?0x4u:m==2?0x8u:m==3?(autoLayerFilter?0x28u:0x4u):0u;}
    static bool validResolution(unsigned) noexcept;
    static bool validEdgeMargin(unsigned) noexcept;
    static bool validProfilerMode(ProfilerMode) noexcept;
    static const char* profilerModeName(ProfilerMode) noexcept;
    // Full graphics.cfg filename in the app's own files directory. Call before
    // loadOnce; changing the path after startup is deliberately rejected.
    bool configurePath(std::string path) noexcept;
    // Missing file is a valid first run. Malformed/unsupported content loads
    // defaults and reports false. Repeated calls never change startup settings.
    bool loadOnce() noexcept;
    Snapshot snapshot() const noexcept;
    // Accepted edits apply in memory even if persistence fails; false reports
    // invalid input, use before loadOnce, or failure to save. See persistenceOk.
    bool setResolutionPercent(unsigned) noexcept;
    bool setShadows(bool) noexcept;
    bool setGrass(bool) noexcept;
    // Compatibility setter: true requests FPS only, never detailed timers.
    bool setProfiler(bool) noexcept;
    bool setProfilerMode(ProfilerMode) noexcept;
    bool setEdgeMarginPercent(unsigned) noexcept;
    bool setOcclusion(bool) noexcept;
    bool setSmallObjectsPx(unsigned) noexcept;
    bool setFoveation(unsigned) noexcept;
    bool setFoveationApi(unsigned) noexcept;
    bool setAntiAliasing(unsigned) noexcept;
    bool setTaaSharpness(unsigned) noexcept;
    static constexpr unsigned taaSharpnessModes=3;
    static const char* taaSharpnessSuffix(unsigned s) noexcept {return s==1?" SHARP":s==2?" SHARPER":"";}
    bool setFrameRate(unsigned) noexcept;
    bool setSpaceWarp(unsigned) noexcept;
    bool setShadowUpdate(unsigned) noexcept;
    bool setShadowMap(unsigned) noexcept;
    bool setGrassDistance(unsigned) noexcept;
    // The setters below each validate, take the lock once and save once.
    bool setPostFx(unsigned) noexcept;
    bool setTextureFilter(unsigned) noexcept;
    bool setMipBias(unsigned) noexcept;
    bool setSharpen(unsigned) noexcept;
    bool setCascadeCull(unsigned) noexcept;
    bool setXrQueuePriority(unsigned) noexcept;
    bool setXrQueueVerified(bool) noexcept;
    bool setLampShadows(unsigned) noexcept;
    bool setHeatWarning(bool) noexcept;       // heat01
    bool setGpuBoost(unsigned) noexcept;      // boost01
    bool setWater(bool) noexcept;             // w02
    static constexpr unsigned gpuBoostModes=3; // boost01
    static const char* gpuBoostName(unsigned v) noexcept {return v==1?"640":v==2?"690":"OFF";} // boost01
    bool setSunShadows(bool on,unsigned update) noexcept; // SUN SHADOWS row: shadows + shadowUpdate (pass the stored update when off)
    bool setGrassRow(bool on,unsigned distance) noexcept; // GRASS row: grass + grassDistance (pass the stored distance when off)
    bool restoreDefaults() noexcept;                      // settings = Settings{}, one save
    bool save() noexcept;
    void recordAppliedExtent(std::uint32_t width,std::uint32_t height) noexcept;
    bool shadows() const noexcept;
    bool grass() const noexcept;
    // Compatibility getter means detailed work; use fpsVisible() for the HUD.
    bool profiler() const noexcept;
    ProfilerMode profilerMode() const noexcept;
    bool fpsVisible() const noexcept;
    bool detailedProfiling() const noexcept;
    unsigned edgeMarginPercent() const noexcept;
    bool occlusion() const noexcept;
    unsigned smallObjectsPx() const noexcept;
    unsigned foveation() const noexcept;
    unsigned foveationApi() const noexcept;
    unsigned antiAliasing() const noexcept;
    unsigned frameRate() const noexcept;
    unsigned spaceWarp() const noexcept;
    unsigned shadowUpdate() const noexcept;
    unsigned shadowMap() const noexcept;
    unsigned grassDistance() const noexcept;
    unsigned postFx() const noexcept;        // This and the getters below return requested values
    unsigned textureFilter() const noexcept;
    unsigned mipBias() const noexcept;
    unsigned sharpen() const noexcept;
    unsigned cascadeCull() const noexcept;
    unsigned xrQueuePriority() const noexcept;
    bool xrQueueVerified() const noexcept;
    unsigned lampShadows() const noexcept;
    unsigned s02Mask() const noexcept;
    bool heatWarning() const noexcept;       // heat01 (requested; debug.rdrvr.heatwarn may override it: HeatWarning::enabled)
    unsigned gpuBoost() const noexcept;      // boost01 (requested; debug.rdrvr.boost may override it: GpuBoost.h)
    bool water() const noexcept;             // w02 (requested; debug.rdrvr.vr.water may override it)
    int legacyOcclusion() const noexcept;    // -1: the loaded file had no pre-V2 "occlusion" key
private:
    mutable std::mutex mutex_;
    std::string path_;
    Settings settings_{};
    Extent applied_{};
    unsigned startupResolutionPercent_{100},startupXrQueuePriority_{};
    int legacyOcclusion_{-1};
    unsigned cfgVersionLoaded_{};
    bool cfgMigrated_{},cfgMigrationSaved_{},s02MaskClamped_{};
    bool loaded_{},loadResult_{},persistenceOk_{true};
    bool setLevel(unsigned Settings::*member,unsigned value,unsigned count) noexcept;
    bool saveLocked() noexcept;
    bool setFlag(bool Settings::*member,bool value) noexcept;
};
extern GraphicsSettings graphicsSettings;
}
