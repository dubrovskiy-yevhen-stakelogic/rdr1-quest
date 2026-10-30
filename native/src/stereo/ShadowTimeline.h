#pragma once
// Per-second shadow event counts of the last ringSize seconds (report graphics.shadowUpdate.timeline): what the session totals
// cannot show, namely WHEN lamps lost their shadow, the indoor flag or the night gate flipped, and whether that lines up with the
// stock camera cuts of a cutscene. Relaxed atomics only; tick() runs once per RenderScene (SunNight.h sceneBegin), bump() on any thread.
//   f      RenderScenes
//   lost   lamps that had a shadow slot last frame and have none now (their shadow is gone from the picture)
//   gain   lamps that got a slot this frame
//   move   slot now holds another lamp (includes lost / gain; a pure reorder only moves)
//   key    slot keeps its lamp but its key bytes changed (the lamp moved or changed: forced redraw)
//   empty  slots the engine cleared after the render because the caster list was empty (no map for that frame)
//   act    active slots summed over the frames (act / f = mean)
//   in     indoor flag flips (IndoorState.h)
//   sun    NIGHT OFF gate flips (SunNight.h)
//   cut    stock camera cuts of a stereo cutscene (VrCutscene.h)
//   cov    full atlas renders forced because a cascade sphere moved > 0.5 r (SunStagger.h coverage)
//   jump   full atlas renders forced because grcViewport::sm_Current moved > 5 m (SunStagger.h cut)
//   eye    cascade camera calls that got the eye camera instead of the stock one (CutsceneShadowCam.h)
//   dist   largest eye to stock camera distance, decimetres
#include <array>
#include <atomic>
#include <cstdint>
#include <ostream>

namespace rdrvr::engine::shadowtimeline {
enum Field : unsigned {Frames,LampLost,LampGain,LampMove,LampKey,LampEmpty,LampActive,IndoorFlip,SunFlip,StockCut,FullCoverage,FullJump,EyeCamera,EyeDistDm,FieldCount};
inline constexpr const char* fieldNames[FieldCount]{"f","lost","gain","move","key","empty","act","in","sun","cut","cov","jump","eye","dist"};
inline constexpr unsigned ringSize=256;
struct Row {std::atomic<std::uint32_t> second{~0u};std::array<std::atomic<std::uint32_t>,FieldCount> v{};};
inline std::array<Row,ringSize> rows{};
inline std::atomic<std::uint32_t> current{};
inline std::uint64_t firstNs{}; // render thread only (tick)
inline void tick(std::uint64_t nowNs) noexcept {
    if(!firstNs)firstNs=nowNs?nowNs:1;
    const auto second=std::uint32_t((nowNs-firstNs)/1'000'000'000ull);
    auto& r=rows[second%ringSize];
    if(r.second.load(std::memory_order_relaxed)!=second){
        for(auto& a:r.v)a.store(0,std::memory_order_relaxed);
        r.second.store(second,std::memory_order_relaxed);current.store(second,std::memory_order_relaxed);}
}
inline void bump(Field f,std::uint32_t n=1) noexcept {rows[current.load(std::memory_order_relaxed)%ringSize].v[f].fetch_add(n,std::memory_order_relaxed);}
inline void max(Field f,std::uint32_t n) noexcept {
    auto& a=rows[current.load(std::memory_order_relaxed)%ringSize].v[f];
    if(n>a.load(std::memory_order_relaxed))a.store(n,std::memory_order_relaxed);}
// "timeline":{"fields":["s","f",...],"rows":[[second,f,...],...]} oldest first; seconds without a RenderScene have no row.
inline void report(std::ostream& o) {
    const auto now=current.load(std::memory_order_relaxed);
    o<<"\"timeline\":{\"fields\":[\"s\"";
    for(const char* n:fieldNames)o<<",\""<<n<<'"';
    o<<"],\"rows\":[";bool first=true;
    for(std::uint32_t k=now>=ringSize?now-ringSize+1:0;k<=now;++k){
        const auto& r=rows[k%ringSize];if(r.second.load(std::memory_order_relaxed)!=k)continue;
        o<<(first?"[":",[")<<k;first=false;
        for(const auto& a:r.v)o<<','<<a.load(std::memory_order_relaxed);
        o<<']';}
    o<<"]}";
}
} // namespace rdrvr::engine::shadowtimeline
