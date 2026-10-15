#pragma once
// In-headset diagnostic switch for the stereo replay (graphics menu row "STEREO TEST").
// Modes 0/1 map to the live replay mask (StereoReplay.cpp liveMask()); further diagnostic masks
// (COLOR NATIVE, NO TEX OVERRIDES, SKIP ... VS, CLEAR ..., MAGENTA, NO UBO REWRITE, KEEP MOTION BLUR, ASYNC
// SECONDARIES) are reachable through the property debug.rdrvr.stereoreplay.mask (bits 18,21-23,25-29,31).
// Adding a mode: append its menu name to testModeNames (the menu cycles through testModeCount, the per-mode report
// tables size themselves from it) and give it a named constant below.
#include <atomic>
namespace rdrvr::engine::stereo {
inline std::atomic<unsigned> testMode{0};
inline constexpr const char* testModeNames[]{
    "OFF","SWAP EYES","NO ASYNC WORKER","OCCLUDER A/B TEST","OCCLUDERS ON",                                  // 0..4
    "WORKER LITTLE CORES","WORKER NICE 0","WORKER COLOR AT END","WORKER CORE 2","WORKER STATIC AFFINITY",          // 5..9
    "R EYE EXTRA PASSES","R EYE SPLIT PASSES","L EYE SPLIT PASSES",                                            // 10..12
    "FREEZE SHADOW ATLAS","RECEIVER NO CLOUDS","RECEIVER RANGE CAP","SKIP POSTFX",                            // 13..16
    "FOVEATION SWEEP","TEXTURE FILTER A/B","MIP BIAS A/B","R EYE MERGE PASSES",                              // 17..20
    "TAIL IN EYES","NEAR CLIP STOCK","NEAR CLIP 0.2",                                                          // 21..23
    "SCISSOR AS LEFT","R EYE NO UBO REWRITE",                                                                  // 24..25
    "PLAYER SHADOW OFF",                                                                                       // 26
};
constexpr unsigned testModeCount=unsigned(sizeof(testModeNames)/sizeof(testModeNames[0]));
// Mode 2 is not a replay mask: the worker recording (async=2) is the default; this mode turns it off for
// comparison (inline replay, same as debug.rdrvr.stereoreplay.async=0), read by StereoReplayAsync.cpp asyncMode().
constexpr unsigned testModeNoAsyncWorker=2;
// Mode 3 is not a replay mask: native occluder quad injection, alternating (OccluderInjection.h, 30 frames on / 30 off).
constexpr unsigned occluderTestMode=3;
// Mode 4 is not a replay mask: occluder injection always on (OccluderInjection.h).
constexpr unsigned occluderAlwaysMode=4;
// Modes 5..9 are not replay masks: worker (async=2) variants for in-headset comparison, read by StereoReplayAsync.cpp; the
// menu value takes precedence over the debug.rdrvr.stereoreplay.* properties. Each mode accumulates its own
// left-stage CPU / object counts in the report (stereoReplay.async.byMode).
constexpr unsigned testModeWorkerLittle=5;  // worker pinned to the little cores (lowest cpu_capacity)
constexpr unsigned testModeWorkerNice0=6;   // worker at nice 0 throughout (no boost), default affinity
constexpr unsigned testModeWorkerColorEnd=7;// depth streams; the colour job is launched at the end of the left colour stage
constexpr unsigned testModeWorkerCore2=8;   // worker pinned to cpu 2 (plus the little cores when cpu 2 is a little core)
constexpr unsigned testModeWorkerAsync05=9; // Worker at nice -10 throughout, static auto affinity, no render-cpu following
// Modes 10..12 are not replay masks: render-pass boundary cost, each alternating by itself (StereoReplay.cpp
// latchBoundaryPhase): frames 0-59 of every 120 insert the extra boundaries (gpuStages.byState signature "..._m<mode>a"),
// frames 60-119 do not ("..._m<mode>b"). Full replay with the worker, as mode 0. Report stereoReplay.boundaryTest.
constexpr unsigned testModeBoundaryExtraR=10; // control: an empty LOAD/STORE twin pass after every replayed right-eye pass (expect ~0)
constexpr unsigned testModeBoundarySplitR=11; // right-eye replay passes with >= 32 draws split in two (head STORE + tail LOAD)
constexpr unsigned testModeBoundarySplitL=12; // left-eye engine passes with >= 32 draws split after half of their draws
constexpr bool testModeBoundary(unsigned m) noexcept {return m>=testModeBoundaryExtraR&&m<=testModeBoundarySplitL;}
// Modes 13..16 are not replay masks: sun-shadow / post-processing cost measurements (full replay with the
// worker, as mode 0), read by ShadowUpdate.h / PostFxSkip.h; report graphics.shadowQuality.shadowUpdate / skipPostFx.
constexpr unsigned testModeFreezeShadowAtlas=13; // cascade atlas never re-rendered after its last render (SHADOW UPDATE with N = infinity)
constexpr unsigned testModeReceiverNoClouds=14;  // receiver cloud argument forced false (WithoutClouds technique, range = cascade range)
constexpr unsigned testModeReceiverRangeCap=15;  // receiver range (depth bounds) clamped to 60 m
constexpr unsigned testModeSkipPostFx=16;        // draws of the post-processing after the recorded stereo pair become bind-only
// Modes 17..20 are not replay masks. 17 is read by GameFoveation.cpp; 18..20 alternate like 10..12: StereoReplay.cpp
// latchBoundaryPhase sets boundaryPhase 1 (frames 0-59 of 120, signature "..._m<mode>a") / 2 ("..._m<mode>b") for every
// testModeAlternating() mode, while boundaryFlags stays 0 for 18/19 (only mode 20 sets BoundaryMergeR in window a).
constexpr unsigned testModeFoveationSweep=17;  // FOVEATION OFF,LOW,MEDIUM,HIGH,ULTRA, 64 frames each (byState fov0..fov4 _m17)
constexpr unsigned testModeTextureFilterAB=18; // window a: anisotropy cap 4X (textureFilter level 2), b: NATIVE; the row is ignored
constexpr unsigned testModeMipBiasAB=19;       // window a: mip bias 0.5 (mipBias level 2), b: 0; the row is ignored
constexpr unsigned testModeMergeR=20;          // window a: right-eye colour replay with merged LOAD passes, b: native passes
// Modes 21..23 are not replay masks; full replay with the worker, as mode 0.
// 21: the RenderScene tail that natively draws AFTER the stereo pair capture (GPU rain CVfxWeather::Render, lamp volumetric
//     lights x2, hi-res particles, FX objects, light cones, lamp coronas) is drawn early, inside the recorded LEFT colour stage
//     (GameXr primaryDraw -> PostFxSkip.h postfx::tail::early), so it lands in the left eye before its capture and in the
//     right-eye colour tape; the native tail calls of that RenderScene are skipped. Also on with debug.rdrvr.tail=1.
// 22/23: VisibilityCameraBridge near clip: 22 = the engine's stock near (no 0.05 clamp), 23 = 0.2 (depth precision comparison for
//     far-terrain flicker; D24 forward Z: step ~ 2^-24 d^2/near).
constexpr unsigned testModeTailInEyes=21;
constexpr unsigned testModeNearStock=22;
constexpr unsigned testModeNear20=23;
// Modes 24/25: full replay with the worker, as mode 0.
// 24: right-eye replay re-emits the LEFT eye's scissor rectangles verbatim (StereoScissor.h
//     otherwise remaps sub-rectangle scissors such as rage::SkyDome::SetMoonExtents into the right eye).
// 25: replay mask bit 28 (no right-eye uniform rewrite: every draw keeps the LEFT set-0 blocks and gets
//     the clip correction push). Isolates right-eye-only artefacts of the UBO rewrite; lighting terms
//     that read the camera position / inverse matrices stay left-eye in this mode.
constexpr unsigned testModeScissorAsLeft=24;
constexpr unsigned testModeNoUboRewrite=25;
// Mode 26: full replay with the worker, as mode 0. Turns the PLAYER SHADOW WINDOW off (SunStagger.h:
// at SUN SHADOWS STAGGER the masked near tile is redrawn around the player every frame; off = the
// player's shadow steps at 12 Hz). Same as debug.rdrvr.sunstagger bit 32; signature suffix _su5w.
constexpr unsigned testModePlayerShadowOff=26;
// Modes with an a/b window (boundaryPhase 1/2). testModeBoundary() keeps deciding the boundary flags of 10..12 only.
constexpr bool testModeAlternating(unsigned m) noexcept {
    return testModeBoundary(m)||(m>=testModeTextureFilterAB&&m<=testModeMergeR);}
constexpr const char* testModeName(unsigned m) noexcept {return m<testModeCount?testModeNames[m]:"?";}
constexpr unsigned testModeMask(unsigned m) noexcept {
    switch(m){
    case 1:return 1u<<30; // SWAP EYES (GameStereoCapture.cpp)
    case testModeNoUboRewrite:return 1u<<28; // R EYE NO UBO REWRITE (StereoReplay.cpp snapshotRing: uboEnabled=false)
    default:return 0u;    // 2..: full replay, see the constants above
    }
}
}
