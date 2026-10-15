// GPU timestamps at the stage boundaries of the engine's frame command buffer.
// Included by StereoReplay.cpp inside namespace rdrvr::engine::stereo, after the registries (deviceRegistry,
// framebufferRegistry, viewRegistry, coldMutex). Armed only while the DETAIL profiler is on (GameXr sets gpuStagesArmed
// every frame); otherwise every entry point is one relaxed load in an inline header wrapper and nothing here runs.
//
// Stage markers: one timestamp per boundary (StereoReplay.h GpuMark); a stage is the span between two markers.
// Markers are written outside render passes: at the boundary when no engine render pass is open, otherwise right after
// the open pass ends (EndRenderPass hook), before the next BeginRenderPass, or at vkEndCommandBuffer.
// Every ENGINE render pass of the frame command buffer also gets a timestamp right before its
// vkCmdBeginRenderPass and right after its vkCmdEndRenderPass (both outside the pass), classified by its framebuffer
// (extent, colour/depth attachments, eye image or not). Depth-only non-eye passes are shadow maps: their per-frame sum
// is the "sunShadow" stage (the sun call itself draws the screen-space collector inside an open eye pass, which a tiler
// cannot time on its own), and "preDepthRest" is preDepth minus the shadow-map passes recorded before the left depth
// stage. The replay's own passes (raw entry points) are covered by the right-eye stage markers only.
// Query pool: one slot of (markers + 2 x passes) queries per frame, a ring of 4; a frame resets its slot at
// vkBeginCommandBuffer (outside any pass); the host reads the slot back (no WAIT, availability checked) when the slot
// comes round again 4 frames later.
// byState: per settings signature (GameXr: resolution, shadows, grass, foveation, small objects + STEREO TEST mode)
// sums/sums of squares/min/max of every stage, of the frame interval, the HUD FPS and the render-thread stage CPU,
// excluding the first 8 frames after a change; at most 16 signatures (least recently used evicted).
// Per state also the boundaries inserted per frame (STEREO TEST 10/11/12, exact per-frame count of that slot) and
// the signed paired estimator rightMinusLeft = (rightDepth+rightColor) - (leftDepth+leftColor) of every frame.
//
//  - Lite span (GpuLite.h): outside DETAIL, commandBegun of a learned frame command buffer (the left depth tape's)
//    resets its slot of an own 8-query pool (4 slots x 2) and writes TOP_OF_PIPE; gpuCommandEnd writes BOTTOM_OF_PIPE.
//    Every lite begin polls the pending slots oldest-first (no WAIT, availability checked); a slot reused while still
//    pending counts as late (or stale when only an older frame's values were there). Consumed spans (us) go to a 72-entry
//    atomic ring + serial (lock-free readers: FOVEATION AUTO, FRAME RATE ADAPTIVE), a 256-entry window (report) and the
//    byState stage "frame" of the "_ptL" state. The DETAIL readBack pushes its "frame" span into the same ring.
//  - Stages-only sub-mode: debug.rdrvr.gpustages.passes 0 = stage markers only, 1 = per-pass timestamps every DETAIL
//    frame, 2 (default) = alternate 120 DETAIL frames with / 120 without (latched by gpuSetState for the next frames,
//    0 while testModeAlternating); a frame without them still classifies its passes (census, deferral) but writes no
//    pass timestamp and pushes no sunShadow/preDepthRest/class time ("passes" msPerFrame divide by perPassFrames).
//  - depthByDraws: 21 bins of 100 draws of the left depth tape's pass 0 (DETAIL frames): mean leftDepth / rightDepth.
//
// Settings signature (gpuStages.byState keys; GameXr stereoGpuState writes the graphics part, gpuSetState the suffix):
//   res<s>[p<now>]_sh<0|1>_gr<0|1>_fov<k>[a]_so<px>_aa<e>[d<n>]_su<n>_sm<extent>_gd<n>_sw<n>[s]
//   _oc<0|1>          OCCLUSION menu value (requested)
//   _pf<0|1|2>        POST FX OFF / ON / OFF FAST
//   _in<0|1|x>        camera indoor (SUN SHADOWS INDOOR), x = unknown
//   _pl<0..4>         frame pacer signature code (FRAME RATE AUTO/LOCK 36/LOCK 24/ADAPTIVE 36/24)
//   _em<pct>          EDGE MARGIN
//   _af<cap|x>_mb<centi|x>   applied anisotropy cap / mip bias (EngineTextureFilter.h)
//   _cc<0..2>         CASCADE CULL effective;  _sp<0..3> SHARPEN effective;  _qp<0|2|3> XR queue layout
//   _m<mode>[a|b]     STEREO TEST mode and A/B window (gpuSetState)
//   _pt<1|0|L>        DETAIL with per-pass timestamps / DETAIL stage markers only / lite span (gpuSetState)
//   _x<hex4>[t]       effective lever mask (S02Levers.h), 't' = one of the 2 frames after a mask change (LAST
//                     item of the graphics part)
// Parse it with ^(.+?)_m(\d+)([ab]?)_pt([01L])$ ; the graphics part is cut (signatureTruncated) so the suffix
// always fits signatureSize.
// Spans sceneTail (StereoEnd->TailEnd), queries (TailEnd->QueriesEnd), framePost (QueriesEnd->CmdEnd); TailEnd /
// QueriesEnd are requested by the EndMainColorPass / sagFxManager::RunQueries hooks and, like every marker, deferred to the
// next pass edge while an engine pass is open (queries ~0 when the query draws share the tail's eye pass). eyePassByIndex:
// per left region (leftDepth incl. the sun marks, leftLight, leftColor) and pass ordinal k < 32 the passes, GPU ms and engine
// draws (gpuDrawSeen) of _pt1 frames. passDescs: distinct (render pass, class, region, renderArea) begun in DETAIL frames.
namespace gpu {
constexpr unsigned markerCount=unsigned(GpuMark::Count);
constexpr unsigned maxPasses=160; // Heavy frames exceed 128 passes
constexpr unsigned slotQueries=markerCount+2*maxPasses;
constexpr unsigned slotCount=4;
constexpr unsigned windowSize=256;
constexpr unsigned settleFrames=8;
constexpr unsigned stateCapacity=96; // The _pt1/_pt0 alternation doubles the DETAIL states
constexpr unsigned classCapacity=48;
constexpr unsigned fbCacheSize=64;
struct Span {const char* name;GpuMark from,to;};
constexpr Span spans[]{
    {"frame",GpuMark::CmdBegin,GpuMark::CmdEnd},                 // whole frame command buffer
    {"preDepth",GpuMark::CmdBegin,GpuMark::LeftDepthBegin},       // before the left depth stage (shadow maps etc.)
    {"leftDepth",GpuMark::LeftDepthBegin,GpuMark::LeftDepthEnd},  // stockDepth (eye depth + shadow collector)
    {"depthToLight",GpuMark::LeftDepthEnd,GpuMark::LeftLightBegin},
    {"leftLight",GpuMark::LeftLightBegin,GpuMark::LeftLightEnd},
    {"lightToColor",GpuMark::LeftLightEnd,GpuMark::LeftColorBegin},
    {"leftColor",GpuMark::LeftColorBegin,GpuMark::LeftColorEnd},
    {"colorToStereo",GpuMark::LeftColorEnd,GpuMark::StereoBegin},
    {"rightDepth",GpuMark::RightDepthBegin,GpuMark::RightDepthEnd}, // replay splice (or native right depth)
    {"rightLight",GpuMark::RightLightBegin,GpuMark::RightLightEnd},
    {"rightColor",GpuMark::RightColorBegin,GpuMark::RightColorEnd},
    {"stereoHead",GpuMark::StereoBegin,GpuMark::RightDepthBegin}, // left-eye capture copy (primary reuse) + right eye bind
    {"stereoTail",GpuMark::RightColorEnd,GpuMark::StereoEnd},     // right-eye capture copy + binding restore
    {"stereoPass",GpuMark::StereoBegin,GpuMark::StereoEnd},       // the whole right-eye pass (EngineStereoPass::drawScene)
    {"post",GpuMark::StereoEnd,GpuMark::CmdEnd},                  // engine after the stereo pass until vkEndCommandBuffer
    {"sceneTail",GpuMark::StereoEnd,GpuMark::TailEnd},            // RenderScene tail after the capture (glows, cones, particles...)
    {"queries",GpuMark::TailEnd,GpuMark::QueriesEnd},             // AI ambient + optics occlusion queries
    {"framePost",GpuMark::QueriesEnd,GpuMark::CmdEnd},            // RenderFrame post / 2D / HUD until vkEndCommandBuffer
};
constexpr unsigned spanCount=sizeof(spans)/sizeof(spans[0]);
constexpr const char* derivedNames[]{"sunShadow","preDepthRest"}; // from the per-pass timestamps
constexpr unsigned derivedSun=spanCount,derivedPreRest=spanCount+1;
constexpr unsigned stageCount=spanCount+2;
constexpr const char* stageName(unsigned k) noexcept {return k<spanCount?spans[k].name:derivedNames[k-spanCount];}
constexpr std::uint32_t bit(GpuMark m) noexcept {return 1u<<unsigned(m);}
// region of the frame that starts at a marker (pass table)
constexpr const char* regionName(unsigned m) noexcept {
    switch(GpuMark(m)){
    case GpuMark::CmdBegin:return "preDepth";case GpuMark::LeftDepthBegin:case GpuMark::SunBegin:case GpuMark::SunEnd:return "leftDepth";
    case GpuMark::LeftDepthEnd:return "depthToLight";case GpuMark::LeftLightBegin:return "leftLight";case GpuMark::LeftLightEnd:return "lightToColor";
    case GpuMark::LeftColorBegin:return "leftColor";case GpuMark::LeftColorEnd:return "colorToStereo";case GpuMark::StereoBegin:return "stereoHead";
    case GpuMark::RightDepthBegin:return "rightDepth";case GpuMark::RightLightBegin:return "rightLight";case GpuMark::RightColorBegin:return "rightColor";
    case GpuMark::RightDepthEnd:case GpuMark::RightLightEnd:return "rightGap";case GpuMark::RightColorEnd:return "stereoTail";
    case GpuMark::StereoEnd:return "post";case GpuMark::TailEnd:return "queries";case GpuMark::QueriesEnd:return "framePost";default:return "?";}
}

GpuQueryFunctions testFunctions{};std::atomic<bool> testFunctionsSet{};
GpuQueryFunctions functions() noexcept {
    if(testFunctionsSet.load(std::memory_order_acquire))return testFunctions;
    GpuQueryFunctions f{}; // loader exports (bypass the engine dispatch slots and every observer)
    f.createQueryPool=::vkCreateQueryPool;f.destroyQueryPool=::vkDestroyQueryPool;f.getQueryPoolResults=::vkGetQueryPoolResults;
    f.cmdResetQueryPool=::vkCmdResetQueryPool;f.cmdWriteTimestamp=::vkCmdWriteTimestamp;
    f.getPhysicalDeviceProperties=::vkGetPhysicalDeviceProperties;f.getQueueFamilyProperties=::vkGetPhysicalDeviceQueueFamilyProperties;
    return f;
}
std::uint64_t wallNs() noexcept {return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
struct Window {
    std::array<std::uint64_t,windowSize> values{};unsigned count{},next{};std::uint64_t last{},total{};
    void push(std::uint64_t ns) noexcept {values[next]=ns;next=(next+1)%windowSize;if(count<windowSize)++count;last=ns;++total;}
};
struct Stat {double sum{},sumSq{},min{},max{};std::uint64_t count{};
    void add(double v) noexcept {if(!count||v<min)min=v;if(!count||v>max)max=v;sum+=v;sumSq+=v*v;++count;}};
constexpr bool sameName(const char* a,const char* b) noexcept {while(*a&&*a==*b){++a;++b;}return *a==*b;}
constexpr unsigned spanIndex(const char* name) noexcept {for(unsigned k=0;k<spanCount;++k)if(sameName(spans[k].name,name))return k;return spanCount;}
constexpr unsigned spanLeftDepth=spanIndex("leftDepth"),spanLeftColor=spanIndex("leftColor"),spanRightDepth=spanIndex("rightDepth"),spanRightColor=spanIndex("rightColor");
constexpr unsigned spanFrame=spanIndex("frame");
static_assert(spanLeftDepth<spanCount&&spanLeftColor<spanCount&&spanRightDepth<spanCount&&spanRightColor<spanCount&&spanFrame<spanCount);
constexpr unsigned signatureSize=128; // graphics signature (worst case ~110) + "_m<mode>[ab]_pt<x>"
constexpr unsigned ptWindow=120;       // passMode 2: DETAIL frames with, then without, per-pass timestamps
constexpr unsigned depthBins=21;       // depthByDraws: 100 draws per bin, the last one >= 2000
struct StateStats {
    char sig[signatureSize]{};std::uint64_t id{},lastUse{},frames{},settling{},cpuFrames{},cpuSettling{};
    std::array<Stat,stageCount> stages{};Stat interval{},hudHz{},depthCpuMs{},colorCpuMs{},depthObjects{},colorObjects{},stereoDrawCpuMs{};
    Stat inserted{},rightMinusLeft{};
    bool lite{}; // A "_ptL" state (lite spans only; DETAIL frames are never attributed to it, nor lite spans to the others)
};
struct PassClass {char key[48]{};std::uint32_t width{},height{};unsigned colors{},depthFormat{};bool eye{},shadow{},known{};
    std::uint64_t passes{},frames{},sumNs{},maxNs{},lastFrame{UINT64_MAX};unsigned region{};};
struct PassRec {std::uint16_t cls{};std::uint8_t region{};bool ended{};std::uint8_t group{},ordinal{};std::uint16_t draws{};}; // Group 1..3 = leftDepth/leftLight/leftColor
constexpr unsigned eyeGroups=3,eyeOrdinals=32,descCapacity=48;
constexpr const char* eyeGroupNames[eyeGroups]{"leftDepth","leftLight","leftColor"};
constexpr std::uint8_t eyeGroupOf(unsigned marker) noexcept {
    switch(GpuMark(marker)){case GpuMark::LeftDepthBegin:case GpuMark::SunBegin:case GpuMark::SunEnd:return 1;case GpuMark::LeftLightBegin:return 2;
    case GpuMark::LeftColorBegin:return 3;default:return 0;}
}
struct EyePassStat {std::uint64_t passes{},sumNs{},sumDraws{};};
struct DescSeen {VkRenderPass rp{};std::uint16_t cls{};std::uint8_t region{};VkRect2D area{};std::uint64_t count{};};
struct SlotInfo {std::uint32_t written{};bool recorded{},ended{},invalid{},settled{};std::uint64_t lastBegin{UINT64_MAX},stateId{};unsigned stateIndex{};
    unsigned passCount{};std::array<PassRec,maxPasses> passes{};unsigned inserted{}; // inserted: boundaries emitted into this frame
    std::array<std::uint8_t,eyeGroups> groupPasses{}; // Per left region ordinal of the next pass
    bool perPass{true};std::uint32_t depthPass0Draws{UINT32_MAX};}; // Per-pass timestamps written / left depth pass 0 draws
// Lite span: one slot = {CmdBegin, CmdEnd} of one frame command buffer
constexpr unsigned liteSlots=4;
struct LiteSlot {bool begun{},ended{},pending{},invalid{},staleSeen{},settled{};std::uint64_t lastBegin{UINT64_MAX},stateId{};unsigned stateIndex{};};
struct Lite {
    GpuQueryFunctions fn{};VkDevice device{};VkQueryPool pool{};double period{1.0};std::uint64_t mask{~0ull};bool failed{};const char* status{"IDLE"};
    std::array<LiteSlot,liteSlots> slots{};unsigned next{};VkCommandBuffer command{};unsigned slot{};
    std::uint64_t begins{},ends{},frames{},detailFrames{},late{},stale{},abandoned{},mismatch{},outliers{},readErrors{},sinceEnd{};
    Window window{};const char* source{"none"};
};
struct DepthBin {std::uint64_t n{};double leftMs{},rightMs{};};
struct State {
    std::mutex m;
    GpuQueryFunctions fn{};VkDevice device{};VkQueryPool pool{};double period{1.0};std::uint64_t mask{~0ull};unsigned validBits{64};
    bool failed{};const char* status{"IDLE"};
    std::array<VkCommandBuffer,4> known{};unsigned knownNext{};               // learned frame command buffers (left depth tape)
    VkCommandBuffer command{};unsigned slot{};std::uint32_t written{},pending{};bool inPass{},invalid{};std::uint64_t serial{};unsigned lastMarker{};
    int openPass{-1};
    std::array<SlotInfo,slotCount> slots{};
    std::array<Window,stageCount> windows{};
    std::uint64_t frames{},unavailable{},stale{},incomplete{},abandoned{},deferred{},mismatch{},readErrors{},outliers{},begun{},passOverflow{},unclassified{};
    VkResult lastError{VK_SUCCESS};
    // pass classes
    std::array<PassClass,classCapacity> classes{};unsigned classCount{},classOverflow{};
    std::array<std::pair<VkFramebuffer,std::uint16_t>,fbCacheSize> fbCache{};unsigned fbCacheNext{};unsigned eyeGeneration{},cacheGeneration{};
    std::array<VkImage,16> eyeImages{};unsigned eyeCount{};
    // byState
    std::array<StateStats,stateCapacity> states{};unsigned stateCount{},current{UINT32_MAX};std::uint64_t nextStateId{1},useClock{},evictions{};
    unsigned gpuSince{},cpuSince{};std::uint64_t lastBeginWall{},lastBeginStateId{};bool lastBeginSettled{};
    std::uint64_t cpuSerial[5]{};
    // Census: every render pass the engine begins on an eye framebuffer (first 32), with the region it was first seen in
    std::array<VkRenderPass,32> eyePasses{};std::array<std::uint8_t,32> eyePassRegion{};unsigned eyePassCount{};
    std::uint64_t ptCounter{},signatureTruncated{},perPassFrames{};bool nextPerPass{true};
    std::array<DepthBin,depthBins> depthBins{};
    Lite lite{};
    std::array<std::array<EyePassStat,eyeOrdinals>,eyeGroups> eyePass{};
    std::array<DescSeen,descCapacity> descs{};unsigned descCount{};std::uint64_t descOverflow{};
} state;
// Lock-free: the frame command buffers learned from the left depth tape (lite begin filter, render thread) and
// the lite span ring read by the governors (any thread).
std::array<std::atomic<VkCommandBuffer>,4> liteKnown{};std::atomic<unsigned> liteKnownNext{};
std::array<std::atomic<std::uint32_t>,gpuLiteWindowMax> liteRing{};std::atomic<std::uint64_t> liteSerial{};
// Properties (latchBoundaryPhase re-reads them every 64th frame) and test overrides
std::atomic<bool> liteProperty{true};std::atomic<int> passModeProperty{2};std::atomic<int> liteOverride{-1},passModeOverride{-1};
bool liteEnabled() noexcept {const int o=liteOverride.load(std::memory_order_relaxed);return o>=0?o!=0:liteProperty.load(std::memory_order_relaxed);}
int passMode() noexcept {const int o=passModeOverride.load(std::memory_order_relaxed);return o>=0?o:passModeProperty.load(std::memory_order_relaxed);}
void refreshProperties() noexcept { // render thread (latchBoundaryPhase), every 64th call
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX]{};
    liteProperty.store(!(__system_property_get("debug.rdrvr.gpulite",v)>0&&v[0]=='0'&&!v[1]),std::memory_order_relaxed);
    int mode=2;if(__system_property_get("debug.rdrvr.gpustages.passes",v)>0&&v[0]>='0'&&v[0]<='2'&&!v[1])mode=v[0]-'0';
    passModeProperty.store(mode,std::memory_order_relaxed);
#endif
}
std::uint32_t toUs(std::uint64_t ns) noexcept {return std::uint32_t(std::min<std::uint64_t>((ns+500)/1000,UINT32_MAX));}
void pushLiteSpan(std::uint64_t ns,const char* source) noexcept { // caller holds state.m (render thread): ring + window
    const std::uint32_t us=toUs(ns);const std::uint64_t n=liteSerial.load(std::memory_order_relaxed);
    liteRing[unsigned(n%gpuLiteWindowMax)].store(us,std::memory_order_relaxed);liteSerial.store(n+1,std::memory_order_release);
    state.lite.window.push(us);state.lite.source=source;
}
void learnFrameCommand(VkCommandBuffer c) noexcept { // render thread: lite begin filter (4 entries, round robin)
    if(!c)return;for(const auto& k:liteKnown)if(k.load(std::memory_order_relaxed)==c)return;
    const unsigned i=liteKnownNext.fetch_add(1,std::memory_order_relaxed)%unsigned(liteKnown.size());liteKnown[i].store(c,std::memory_order_relaxed);
}
bool knownFrameCommand(VkCommandBuffer c) noexcept {for(const auto& k:liteKnown)if(k.load(std::memory_order_relaxed)==c)return true;return false;}
// Timestamp bits of the engine's graphics family and the period. 0 = ok, 1 = no physical device (retry), 2 = unsupported.
int timestampsOf(const GpuQueryFunctions& fn,VkDevice d,unsigned& bits,double& period) noexcept {
    VkPhysicalDevice phys{};{std::lock_guard lock(coldMutex);auto it=deviceRegistry.find(d);if(it!=deviceRegistry.end())phys=it->second.physical;}
    if(!phys)return 1;
    VkPhysicalDeviceProperties props{};fn.getPhysicalDeviceProperties(phys,&props);
    std::uint32_t families=0;fn.getQueueFamilyProperties(phys,&families,nullptr);
    std::array<VkQueueFamilyProperties,16> fam{};families=std::min<std::uint32_t>(families,std::uint32_t(fam.size()));fn.getQueueFamilyProperties(phys,&families,fam.data());
    bits=0;for(std::uint32_t i=0;i<families;++i)if(fam[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){bits=fam[i].timestampValidBits;break;} // the engine's graphics family
    if(!bits||props.limits.timestampPeriod<=0.f)return 2;
    period=props.limits.timestampPeriod;return 0;
}
bool completeFunctions(const GpuQueryFunctions& f) noexcept {return f.createQueryPool&&f.getQueryPoolResults&&f.cmdResetQueryPool&&f.cmdWriteTimestamp&&f.getPhysicalDeviceProperties&&f.getQueueFamilyProperties;}
// caller holds state.m
bool ensurePool(VkDevice d) noexcept {
    auto& s=state;
    if(s.failed)return false;
    if(s.pool&&s.device==d)return true;
    s.pool={};s.device={};for(auto& x:s.slots)x=SlotInfo{}; // another device: the previous pool is gone (deviceDestroyed/shutdown)
    s.fn=functions();
    if(!completeFunctions(s.fn)){s.failed=true;s.status="NO_FUNCTIONS";return false;}
    unsigned bits=0;double period=1.0;
    switch(timestampsOf(s.fn,d,bits,period)){case 1:s.status="NO_PHYSICAL_DEVICE";return false;case 2:s.failed=true;s.status="TIMESTAMPS_UNSUPPORTED";return false;default:break;}
    s.validBits=bits;s.mask=bits>=64?~0ull:((1ull<<bits)-1);s.period=period;
    VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};ci.queryType=VK_QUERY_TYPE_TIMESTAMP;ci.queryCount=slotQueries*slotCount;
    VkQueryPool pool{};if(s.fn.createQueryPool(d,&ci,nullptr,&pool)!=VK_SUCCESS||!pool){s.failed=true;s.status="POOL_CREATE";return false;}
    s.pool=pool;s.device=d;s.status="READY";return true;
}
// caller holds state.m: the lite span's own pool (8 queries), same checks
bool ensureLitePool(VkDevice d) noexcept {
    auto& L=state.lite;
    if(L.failed)return false;
    if(L.pool&&L.device==d)return true;
    L.pool={};L.device={};for(auto& x:L.slots)x=LiteSlot{};L.command={};L.next=0;
    L.fn=functions();
    if(!completeFunctions(L.fn)){L.failed=true;L.status="NO_FUNCTIONS";return false;}
    unsigned bits=0;double period=1.0;
    switch(timestampsOf(L.fn,d,bits,period)){case 1:L.status="NO_PHYSICAL_DEVICE";return false;case 2:L.failed=true;L.status="TIMESTAMPS_UNSUPPORTED";return false;default:break;}
    L.mask=bits>=64?~0ull:((1ull<<bits)-1);L.period=period;
    VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};ci.queryType=VK_QUERY_TYPE_TIMESTAMP;ci.queryCount=2*liteSlots;
    VkQueryPool pool{};if(L.fn.createQueryPool(d,&ci,nullptr,&pool)!=VK_SUCCESS||!pool){L.failed=true;L.status="POOL_CREATE";return false;}
    L.pool=pool;L.device=d;L.status="READY";return true;
}
void write(GpuMark m,VkPipelineStageFlagBits stage) noexcept { // caller holds state.m, a frame is open
    auto& s=state;if(s.written&bit(m))return;
    s.fn.cmdWriteTimestamp(s.command,stage,s.pool,s.slot*slotQueries+unsigned(m));
    s.written|=bit(m);s.slots[s.slot].written=s.written;s.lastMarker=unsigned(m);
}
void flush() noexcept { // caller holds state.m: pending markers at an out-of-pass point, in marker order
    auto& s=state;const std::uint32_t p=s.pending;s.pending=0;
    for(unsigned i=0;i<markerCount;++i)if(p&(1u<<i)){write(GpuMark(i),VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);++s.deferred;}
}
void closeFrame(bool ended) noexcept { // caller holds state.m
    auto& s=state;if(!s.command)return;
    if(ended){flush();write(GpuMark::CmdEnd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);}else ++s.abandoned;
    auto& slot=s.slots[s.slot];slot.ended=ended;slot.invalid=s.invalid||!ended;
    slot.inserted=stats.boundary.insertedThisFrame.exchange(0,std::memory_order_relaxed); // every insertion of the frame is recorded into its command buffer
    s.command={};gpuFrameCommand.store(VK_NULL_HANDLE,std::memory_order_relaxed);s.pending=0;s.inPass=false;s.openPass=-1;
}
// ---- pass classification (caller holds state.m) ----
bool isDepthFormat(VkFormat f) noexcept {return f>=VK_FORMAT_D16_UNORM&&f<=VK_FORMAT_D32_SFLOAT_S8_UINT;}
std::uint16_t classify(VkFramebuffer fb) noexcept {
    auto& s=state;
    if(s.cacheGeneration!=s.eyeGeneration){s.fbCache={};s.cacheGeneration=s.eyeGeneration;}
    for(const auto& [f,c]:s.fbCache)if(f==fb&&fb)return c;
    PassClass k{};
    if(fb){std::lock_guard lock(coldMutex);
        auto it=framebufferRegistry.find(fb);
        if(it!=framebufferRegistry.end()){const auto& r=it->second;k.known=true;k.width=r.width;k.height=r.height;
            for(std::uint32_t i=0;i<r.count&&i<r.views.size();++i){auto v=viewRegistry.find(r.views[i]);if(v==viewRegistry.end())continue;
                if(isDepthFormat(v->second.format))k.depthFormat=unsigned(v->second.format);else ++k.colors;
                for(unsigned e=0;e<s.eyeCount;++e)if(s.eyeImages[e]==v->second.image)k.eye=true;}}}
    if(!k.known)++s.unclassified;
    k.shadow=k.known&&!k.eye&&!k.colors&&k.depthFormat;
    if(k.known)std::snprintf(k.key,sizeof k.key,"%ux%u c%u d%u%s",k.width,k.height,k.colors,k.depthFormat,k.eye?" eye":"");
    else std::snprintf(k.key,sizeof k.key,"unknown");
    unsigned idx=UINT32_MAX;
    for(unsigned i=0;i<s.classCount;++i)if(!std::strcmp(s.classes[i].key,k.key)){idx=i;break;}
    if(idx==UINT32_MAX){ // the last slot is reserved for "other" (overflow)
        if(s.classCount<classCapacity-1){idx=s.classCount++;s.classes[idx]=k;}
        else{++s.classOverflow;idx=classCapacity-1;if(s.classCount<classCapacity){s.classCount=classCapacity;s.classes[idx]=PassClass{};std::snprintf(s.classes[idx].key,sizeof s.classes[idx].key,"other");}}
    }
    if(fb){s.fbCache[s.fbCacheNext]={fb,std::uint16_t(idx)};s.fbCacheNext=(s.fbCacheNext+1)%fbCacheSize;}
    return std::uint16_t(idx);
}
// ---- byState (caller holds state.m) ----
StateStats* stateById(std::uint64_t id,unsigned index) noexcept {auto& s=state;return index<s.stateCount&&s.states[index].id==id?&s.states[index]:nullptr;}
// A frame (DETAIL or lite) begins in the current state: settled after 8 frames in it; frame interval between begins.
struct FrameState {bool settled{};unsigned stateIndex{UINT32_MAX};std::uint64_t stateId{};};
FrameState noteFrameState() noexcept {
    auto& s=state;FrameState f{};
    f.settled=s.current<s.stateCount&&s.gpuSince>=settleFrames;f.stateIndex=s.current;f.stateId=s.current<s.stateCount?s.states[s.current].id:0;
    if(s.current<s.stateCount&&!f.settled)++s.states[s.current].settling;
    ++s.gpuSince;
    const std::uint64_t now=wallNs();
    if(f.settled&&s.lastBeginSettled&&s.lastBeginStateId==f.stateId&&now>s.lastBeginWall)
        if(auto* st=stateById(f.stateId,f.stateIndex))st->interval.add(double(now-s.lastBeginWall)/1e6);
    s.lastBeginWall=now;s.lastBeginStateId=f.stateId;s.lastBeginSettled=f.settled;
    return f;
}
void readBack(unsigned index) noexcept { // the previous frame recorded in this slot
    auto& s=state;auto& slot=s.slots[index];if(!slot.recorded)return;slot.recorded=false;
    if(!slot.ended||slot.invalid){++s.incomplete;return;}
    std::array<std::uint64_t,slotQueries*2> r{}; // {value, availability} per query
    const bool perPass=slot.perPass; // Stage markers only when false (no pass timestamp was written)
    const unsigned queries=markerCount+(perPass?2*slot.passCount:0u);
    const VkResult res=s.fn.getQueryPoolResults(s.device,s.pool,index*slotQueries,queries,queries*2*sizeof(std::uint64_t),r.data(),2*sizeof(std::uint64_t),
                                                VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if(res!=VK_SUCCESS&&res!=VK_NOT_READY){++s.readErrors;s.lastError=res;if(res==VK_ERROR_DEVICE_LOST){s.failed=true;s.status="DEVICE_LOST";}return;}
    for(unsigned i=0;i<markerCount;++i)if((slot.written&(1u<<i))&&!r[2*i+1]){++s.unavailable;return;}
    if(perPass)for(unsigned p=0;p<slot.passCount;++p){const unsigned q=markerCount+2*p;if(!r[2*q+1]||(slot.passes[p].ended&&!r[2*q+3])){++s.unavailable;return;}}
    const std::uint64_t begin=r[2*unsigned(GpuMark::CmdBegin)];
    if(begin==slot.lastBegin){++s.stale;return;} // the command buffer of that frame never executed: values of an older frame
    slot.lastBegin=begin;++s.frames;
    const double maxTicks=2e9/s.period; // > 2 s: not a frame span
    auto* st=slot.settled?stateById(slot.stateId,slot.stateIndex):nullptr;if(st)++st->frames;
    auto push=[&](unsigned k,std::uint64_t ns){s.windows[k].push(ns);if(st)st->stages[k].add(double(ns)/1e6);};
    std::array<std::uint64_t,spanCount> spanNs{};std::array<bool,spanCount> spanOk{};
    for(unsigned k=0;k<spanCount;++k){const auto& sp=spans[k];
        if(!(slot.written&bit(sp.from))||!(slot.written&bit(sp.to)))continue;
        const std::uint64_t ticks=(r[2*unsigned(sp.to)]-r[2*unsigned(sp.from)])&s.mask;
        if(double(ticks)>maxTicks){++s.outliers;continue;}
        spanNs[k]=std::uint64_t(double(ticks)*s.period+0.5);spanOk[k]=true;push(k,spanNs[k]);}
    if(st){st->inserted.add(double(slot.inserted)); // Paired per-frame estimator (both eyes draw the same content)
        if(spanOk[spanLeftDepth]&&spanOk[spanLeftColor]&&spanOk[spanRightDepth]&&spanOk[spanRightColor])
            st->rightMinusLeft.add((double(spanNs[spanRightDepth])+double(spanNs[spanRightColor])-double(spanNs[spanLeftDepth])-double(spanNs[spanLeftColor]))/1e6);}
    if(spanOk[spanFrame]){pushLiteSpan(spanNs[spanFrame],"detail");++s.lite.detailFrames;} // GpuLite.h readers in DETAIL too
    if(slot.depthPass0Draws!=UINT32_MAX&&spanOk[spanLeftDepth]&&spanOk[spanRightDepth]){ // depthByDraws (render-mode flip diagnostic)
        auto& bin=s.depthBins[std::min<unsigned>(slot.depthPass0Draws/100u,depthBins-1)];
        ++bin.n;bin.leftMs+=double(spanNs[spanLeftDepth])/1e6;bin.rightMs+=double(spanNs[spanRightDepth])/1e6;}
    if(!perPass)return;
    ++s.perPassFrames;
    // engine render passes
    std::uint64_t shadowNs=0,preShadowNs=0;
    for(unsigned p=0;p<slot.passCount;++p){const auto& rec=slot.passes[p];if(!rec.ended)continue;const unsigned q=markerCount+2*p;
        const std::uint64_t ticks=(r[2*q+2]-r[2*q])&s.mask;if(double(ticks)>maxTicks){++s.outliers;continue;}
        const std::uint64_t ns=std::uint64_t(double(ticks)*s.period+0.5);auto& c=s.classes[rec.cls];
        ++c.passes;c.sumNs+=ns;c.maxNs=std::max(c.maxNs,ns);c.region=rec.region;if(c.lastFrame!=s.frames){c.lastFrame=s.frames;++c.frames;}
        if(rec.group&&rec.group<=eyeGroups&&rec.ordinal<eyeOrdinals){auto& e=s.eyePass[rec.group-1u][rec.ordinal];++e.passes;e.sumNs+=ns;e.sumDraws+=rec.draws;}
        if(c.shadow){shadowNs+=ns;if(rec.region==unsigned(GpuMark::CmdBegin))preShadowNs+=ns;}}
    push(derivedSun,shadowNs);
    if(spanOk[1])push(derivedPreRest,spanNs[1]>preShadowNs?spanNs[1]-preShadowNs:0);
}
// Lite: poll every pending slot, oldest first (never waits). A slot is consumed when both timestamps are available
// and CmdBegin differs from the value consumed last time in that slot (equal: the command buffer of that frame never
// executed, the slot still holds an older frame's values -> stays pending, counted stale on reuse). Caller holds state.m.
void litePoll() noexcept {
    auto& s=state;auto& L=s.lite;if(!L.pool)return;
    for(unsigned k=0;k<liteSlots;++k){const unsigned i=(L.next+k)%liteSlots;auto& slot=L.slots[i];if(!slot.pending)continue;
        std::array<std::uint64_t,4> r{};
        const VkResult res=L.fn.getQueryPoolResults(L.device,L.pool,i*2,2,sizeof r,r.data(),2*sizeof(std::uint64_t),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if(res!=VK_SUCCESS&&res!=VK_NOT_READY){++L.readErrors;if(res==VK_ERROR_DEVICE_LOST){L.failed=true;L.status="DEVICE_LOST";return;}continue;}
        if(!r[1]||!r[3])continue;                                  // GPU not there yet
        if(r[0]==slot.lastBegin){slot.staleSeen=true;continue;}  // never executed (so far)
        slot.lastBegin=r[0];slot.pending=false;
        const std::uint64_t ticks=(r[2]-r[0])&L.mask;
        if(double(ticks)>2e9/L.period){++L.outliers;continue;}
        const std::uint64_t ns=std::uint64_t(double(ticks)*L.period+0.5);
        pushLiteSpan(ns,"lite");++L.frames; // the DETAIL stage windows ("stages") stay DETAIL-only
        if(auto* st=slot.settled?stateById(slot.stateId,slot.stateIndex):nullptr){++st->frames;st->stages[spanFrame].add(double(ns)/1e6);}
    }
}
}
void gpuFrameBegin(VkDevice d,VkCommandBuffer c) noexcept { // commandBegun, after the driver's vkBeginCommandBuffer
    using namespace gpu;auto& s=state;
    try{std::lock_guard lock(s.m);
        if(!gpuStagesArmed.load(std::memory_order_relaxed)||!c)return;
        if(std::find(s.known.begin(),s.known.end(),c)==s.known.end())return; // not a frame command buffer (yet)
        if(s.lite.command){s.lite.command={};gpuLiteCommand.store(VK_NULL_HANDLE,std::memory_order_relaxed);} // DETAIL wins over an open lite span
        if(s.command)closeFrame(false); // the previous frame command buffer was never ended
        if(!ensurePool(d))return;
        s.slot=unsigned(s.serial++%slotCount);readBack(s.slot);if(s.failed)return;
        s.fn.cmdResetQueryPool(c,s.pool,s.slot*slotQueries,slotQueries);
        stats.boundary.insertedThisFrame.store(0,std::memory_order_relaxed); // Count this frame's boundaries only
        s.command=c;s.written=0;s.pending=0;s.inPass=false;s.invalid=false;s.openPass=-1;++s.begun;
        auto& slot=s.slots[s.slot];slot.written=0;slot.recorded=true;slot.ended=false;slot.invalid=false;slot.passCount=0;slot.groupPasses={};
        slot.perPass=s.nextPerPass;slot.depthPass0Draws=UINT32_MAX; // Latched by gpuSetState for this frame
        // byState: the state this frame belongs to (settled after 8 frames in it); frame interval between begins
        if(s.current<s.stateCount&&s.states[s.current].lite){slot.settled=false;slot.stateIndex=UINT32_MAX;slot.stateId=0;} // a lite state's signature (switch frame)
        else{const auto f=noteFrameState();slot.settled=f.settled;slot.stateIndex=f.stateIndex;slot.stateId=f.stateId;}
        gpuFrameCommand.store(c,std::memory_order_relaxed);
        write(GpuMark::CmdBegin,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    }catch(...){}
}
void gpuMarkSlow(GpuMark m) noexcept {
    using namespace gpu;auto& s=state;
    try{std::lock_guard lock(s.m);
        if(!s.command||unsigned(m)>=markerCount||((s.written|s.pending)&bit(m)))return; // first request wins
        if(s.inPass){s.pending|=bit(m);return;}
        write(m,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }catch(...){}
}
void gpuPassEdge(VkCommandBuffer c,const VkRenderPassBeginInfo* b) noexcept { // b: begin (before the pass); null: end (after it)
    using namespace gpu;auto& s=state;
    try{std::lock_guard lock(s.m);
        if(!s.command||c!=s.command)return;
        auto& slot=s.slots[s.slot];
        if(b){
            flush();
            const std::uint16_t cls=classify(b->framebuffer);const std::uint8_t region=std::uint8_t(s.lastMarker);
            if(s.classes[cls].eye&&b->renderPass&&s.eyePassCount<s.eyePasses.size()){bool seen=false; // Census (DETAIL only)
                for(unsigned i=0;i<s.eyePassCount&&!seen;++i)seen=s.eyePasses[i]==b->renderPass;
                if(!seen){s.eyePasses[s.eyePassCount]=b->renderPass;s.eyePassRegion[s.eyePassCount++]=region;}}
            s.openPass=-1;
            { // passDescs: distinct (render pass, class, region, renderArea)
                bool seen=false;
                for(unsigned i=0;i<s.descCount&&!seen;++i){auto& d=s.descs[i];
                    if(d.rp==b->renderPass&&d.cls==cls&&d.region==region&&d.area.offset.x==b->renderArea.offset.x&&d.area.offset.y==b->renderArea.offset.y&&
                       d.area.extent.width==b->renderArea.extent.width&&d.area.extent.height==b->renderArea.extent.height){++d.count;seen=true;}}
                if(!seen){if(s.descCount<descCapacity)s.descs[s.descCount++]={b->renderPass,cls,region,b->renderArea,1};else ++s.descOverflow;}
            }
            const std::uint8_t group=eyeGroupOf(region);std::uint8_t ordinal=0xff;
            if(group){auto& n=slot.groupPasses[group-1u];ordinal=n;if(n<0xff)++n;}
            gpuOpenPassDraws.store(0,std::memory_order_relaxed); // Engine draws of this pass (gpuDrawSeen)
            if(slot.perPass){ // Stage markers only on the other frames (classification, census and deferral unchanged)
                if(slot.passCount<maxPasses){const unsigned p=slot.passCount++;auto& rec=slot.passes[p];rec.cls=cls;rec.region=region;rec.ended=false;
                    rec.group=group;rec.ordinal=ordinal;rec.draws=0;
                    s.fn.cmdWriteTimestamp(c,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,s.pool,s.slot*slotQueries+markerCount+2*p);s.openPass=int(p);}
                else ++s.passOverflow;}
            s.inPass=true;
        }else{
            s.inPass=false;
            if(s.openPass>=0){const unsigned p=unsigned(s.openPass);
                s.fn.cmdWriteTimestamp(c,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,s.pool,s.slot*slotQueries+markerCount+2*p+1);slot.passes[p].ended=true;s.openPass=-1;
                slot.passes[p].draws=std::uint16_t(std::min<std::uint32_t>(gpuOpenPassDraws.load(std::memory_order_relaxed),65535u));}
            flush();
        }
    }catch(...){}
}
void gpuCommandEnding(VkCommandBuffer c) noexcept { // before the driver's vkEndCommandBuffer
    using namespace gpu;auto& s=state;
    try{std::lock_guard lock(s.m);if(s.command&&c==s.command)closeFrame(true);}catch(...){}
}
// The left stages recorded into this command buffer: learn it as a frame command buffer; a stage recorded into a
// different one than the open frame's makes that frame's spans meaningless.
void gpuStageCommand(VkCommandBuffer c) noexcept {
    using namespace gpu;auto& s=state;if(!c)return;
    learnFrameCommand(c); // A later FPS-profiler frame measures it with the lite span
    try{std::lock_guard lock(s.m);
        if(std::find(s.known.begin(),s.known.end(),c)==s.known.end()){s.known[s.knownNext]=c;s.knownNext=(s.knownNext+1)%unsigned(s.known.size());}
        if(s.command&&c!=s.command&&!s.invalid){s.invalid=true;++s.mismatch;}
    }catch(...){}
}
// DETAIL: draws of the left depth tape's pass 0 for this frame's slot (depthByDraws).
void gpuNoteDepthPass0(VkCommandBuffer c,std::uint32_t draws) noexcept {
    using namespace gpu;auto& s=state;
    try{std::lock_guard lock(s.m);if(s.command&&c==s.command)s.slots[s.slot].depthPass0Draws=draws;}catch(...){}
}
// ---- lite span ----
// endRecording (left depth stage, DETAIL off): learn the frame command buffer; a stage recorded into another command
// buffer than the open lite span's makes that span meaningless.
void gpuLiteStageCommand(VkCommandBuffer c) noexcept {
    using namespace gpu;auto& s=state;if(!c)return;
    learnFrameCommand(c);
    if(!gpuLiteCommand.load(std::memory_order_relaxed))return;
    try{std::lock_guard lock(s.m);auto& L=s.lite;if(L.command&&c!=L.command&&!L.slots[L.slot].invalid){L.slots[L.slot].invalid=true;++L.mismatch;}}catch(...){}
}
// commandBegun of a learned frame command buffer while DETAIL is off (after the driver's vkBeginCommandBuffer, outside any
// render pass): poll, reuse the next slot, reset its 2 queries, CmdBegin timestamp.
void gpuLiteBegin(VkDevice d,VkCommandBuffer c) noexcept {
    using namespace gpu;auto& s=state;
    if(!c||!knownFrameCommand(c))return; // lock-free filter: not a frame command buffer
    try{std::lock_guard lock(s.m);auto& L=s.lite;
        if(gpuStagesArmed.load(std::memory_order_relaxed))return; // DETAIL wins
        if(!ensureLitePool(d))return;
        litePoll();if(L.failed)return;
        ++L.begins;++L.sinceEnd; // begins since the last end seen (NO_END_HOOK at 16)
        const unsigned i=L.next;L.next=(L.next+1)%liteSlots;auto& slot=L.slots[i];
        if(slot.pending){if(slot.staleSeen)++L.stale;else ++L.late;}else if(slot.begun&&!slot.ended)++L.abandoned;
        slot.begun=true;slot.ended=false;slot.pending=false;slot.invalid=false;slot.staleSeen=false;
        L.fn.cmdResetQueryPool(c,L.pool,i*2,2);L.fn.cmdWriteTimestamp(c,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,L.pool,i*2);
        if(s.current<s.stateCount&&s.states[s.current].lite){const auto f=noteFrameState();slot.settled=f.settled;slot.stateIndex=f.stateIndex;slot.stateId=f.stateId;}
        else{slot.settled=false;slot.stateIndex=UINT32_MAX;slot.stateId=0;} // the current state is a DETAIL one (no _ptL signature set yet): not attributed
        L.command=c;L.slot=i;gpuLiteCommand.store(c,std::memory_order_relaxed);
    }catch(...){}
}
void gpuLiteEnding(VkCommandBuffer c) noexcept { // before the driver's vkEndCommandBuffer
    using namespace gpu;auto& s=state;
    try{std::lock_guard lock(s.m);auto& L=s.lite;
        if(!L.command||c!=L.command||!L.pool)return;
        L.fn.cmdWriteTimestamp(c,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,L.pool,L.slot*2+1);
        auto& slot=L.slots[L.slot];slot.ended=true;slot.pending=!slot.invalid;
        L.command={};gpuLiteCommand.store(VK_NULL_HANDLE,std::memory_order_relaxed);++L.ends;L.sinceEnd=0;
    }catch(...){}
}
std::uint32_t gpuLiteFrameUs() noexcept {const auto n=gpu::liteSerial.load(std::memory_order_acquire);return n?gpu::liteRing[unsigned((n-1)%gpuLiteWindowMax)].load(std::memory_order_relaxed):0u;}
std::uint64_t gpuLiteSerial() noexcept {return gpu::liteSerial.load(std::memory_order_acquire);}
unsigned gpuLiteRing(std::uint32_t* out,unsigned max) noexcept {
    if(!out)return 0;const auto n=gpu::liteSerial.load(std::memory_order_acquire);
    const unsigned count=unsigned(std::min<std::uint64_t>({n,std::uint64_t(gpuLiteWindowMax),std::uint64_t(max)}));
    for(unsigned i=0;i<count;++i)out[i]=gpu::liteRing[unsigned((n-count+i)%gpuLiteWindowMax)].load(std::memory_order_relaxed);
    return count;
}
// Nearest rank over the newest min(window, 72) spans; 0 below 8 samples.
std::uint32_t gpuLitePercentileUs(unsigned pct,unsigned window) noexcept {
    std::array<std::uint32_t,gpuLiteWindowMax> v{};const unsigned n=gpuLiteRing(v.data(),std::min(window,gpuLiteWindowMax));
    if(n<8)return 0;std::sort(v.begin(),v.begin()+n);
    const unsigned p=std::min(pct,100u);const unsigned rank=std::max(1u,(n*p+99)/100);return v[std::min(n,rank)-1];
}
void setGpuLiteForTests(int enabled) noexcept {gpu::liteOverride.store(enabled<0?-1:enabled?1:0,std::memory_order_relaxed);}
void setGpuPassModeForTests(int mode) noexcept {gpu::passModeOverride.store(mode<0?-1:std::min(mode,2),std::memory_order_relaxed);}
void gpuNoteEyeImages(const FrameMaps& maps) noexcept { // replay(): the eye images (pass classification)
    using namespace gpu;auto& s=state;
    try{std::lock_guard lock(s.m);
        std::array<VkImage,16> eyes{};unsigned n=0;
        for(unsigned k=0;k<maps.imageCount&&k<8;++k){if(maps.images[k].left)eyes[n++]=maps.images[k].left;if(maps.images[k].right)eyes[n++]=maps.images[k].right;}
        if(n!=s.eyeCount||!std::equal(eyes.begin(),eyes.begin()+n,s.eyeImages.begin())){s.eyeImages=eyes;s.eyeCount=n;++s.eyeGeneration;}
    }catch(...){}
}
void gpuDeviceGone(VkDevice d,bool idle) noexcept { // shutdownAfterIdle (destroy) / deviceDestroyed (forget)
    using namespace gpu;auto& s=state;
    try{std::lock_guard lock(s.m);auto& L=s.lite; // The lite pool goes with its device as well
        if(L.device==d){if(idle&&L.pool&&L.fn.destroyQueryPool)L.fn.destroyQueryPool(d,L.pool,nullptr);
            L.pool={};L.device={};L.command={};gpuLiteCommand.store(VK_NULL_HANDLE,std::memory_order_relaxed);for(auto& x:L.slots)x=LiteSlot{};L.next=0;L.status="IDLE";
            for(auto& k:liteKnown)k.store(VK_NULL_HANDLE,std::memory_order_relaxed);}
        if(s.device!=d)return;
        if(idle&&s.pool&&s.fn.destroyQueryPool)s.fn.destroyQueryPool(d,s.pool,nullptr);
        s.pool={};s.device={};s.command={};gpuFrameCommand.store(VK_NULL_HANDLE,std::memory_order_relaxed);s.known={};s.knownNext=0;
        for(auto& x:s.slots)x=SlotInfo{};s.status="IDLE";s.fbCache={};s.eyeCount=0;++s.eyeGeneration;s.eyePassCount=0;
    }catch(...){}
}
void gpuSetState(const char* graphics) noexcept {
    using namespace gpu;auto& s=state;
    try{const unsigned phase=boundaryPhase.load(std::memory_order_relaxed); // STEREO TEST 10/11/12 on (a) / off (b), latched before this call
        const unsigned mode=testMode.load(std::memory_order_relaxed);const bool detail=gpuStagesArmed.load(std::memory_order_relaxed);
        std::lock_guard lock(s.m);
        char pt='L'; // The next frames' timestamps (lite span outside DETAIL)
        if(detail){const int pm=testModeAlternating(mode)||mode==testModeFoveationSweep?0:passMode(); // Sweep levels not split by _pt
            const bool perPass=pm==1||(pm!=0&&((s.ptCounter/ptWindow)&1u)==0);++s.ptCounter;
            s.nextPerPass=perPass;pt=perPass?'1':'0';}
        char suffix[32];std::snprintf(suffix,sizeof suffix,"_m%u%s_pt%c",mode,phase==1?"a":phase==2?"b":"",pt);
        const char* g=graphics?graphics:"?";const std::size_t room=signatureSize-1-std::strlen(suffix),length=std::strlen(g);
        if(length>room)++s.signatureTruncated; // the graphics part is cut, the parseable suffix stays
        char sig[signatureSize];std::snprintf(sig,sizeof sig,"%.*s%s",int(std::min(length,room)),g,suffix);
        ++s.useClock;
        if(s.current<s.stateCount&&!std::strcmp(s.states[s.current].sig,sig)){s.states[s.current].lastUse=s.useClock;return;}
        unsigned idx=UINT32_MAX;for(unsigned i=0;i<s.stateCount;++i)if(!std::strcmp(s.states[i].sig,sig)){idx=i;break;}
        if(idx==UINT32_MAX){
            if(s.stateCount<stateCapacity)idx=s.stateCount++;
            else{idx=0;for(unsigned i=1;i<s.stateCount;++i)if(s.states[i].lastUse<s.states[idx].lastUse)idx=i;++s.evictions;}
            s.states[idx]=StateStats{};std::snprintf(s.states[idx].sig,sizeof s.states[idx].sig,"%s",sig);s.states[idx].id=s.nextStateId++;s.states[idx].lite=pt=='L';}
        s.states[idx].lastUse=s.useClock;s.current=idx;s.gpuSince=0;s.cpuSince=0;
    }catch(...){}
}
void gpuNoteFrameCpu(const FrameStageSample& f,double hudHz) noexcept { // GameXr frame end, before gpuSetState
    using namespace gpu;auto& s=state;
    try{std::lock_guard lock(s.m);
        const StageSample* samples[5]{&f.depthBucket,&f.colorBucket,&f.depthStage,&f.colorStage,&f.stereoDraw};
        bool fresh[5]{};for(unsigned i=0;i<5;++i){fresh[i]=samples[i]->ns!=UINT64_MAX&&samples[i]->serial!=s.cpuSerial[i];s.cpuSerial[i]=samples[i]->serial;}
        if(s.current>=s.stateCount)return;auto& st=s.states[s.current];
        if(s.cpuSince++<settleFrames){++st.cpuSettling;return;}
        ++st.cpuFrames;
        if(fresh[0]&&f.depthObjects!=UINT32_MAX){st.depthCpuMs.add(double(f.depthBucket.ns)/1e6);st.depthObjects.add(f.depthObjects);}
        if(fresh[1]&&f.colorObjects!=UINT32_MAX){st.colorCpuMs.add(double(f.colorBucket.ns)/1e6);st.colorObjects.add(f.colorObjects);}
        if(fresh[4])st.stereoDrawCpuMs.add(double(f.stereoDraw.ns)/1e6);
        if(hudHz>0)st.hudHz.add(hudHz);
    }catch(...){}
}
void setGpuQueryFunctionsForTests(const GpuQueryFunctions* f) noexcept {
    if(f){gpu::testFunctions=*f;gpu::testFunctionsSet.store(true,std::memory_order_release);}else gpu::testFunctionsSet.store(false,std::memory_order_release);
}
std::string gpuStagesReport(){
    using namespace gpu;auto& s=state;std::ostringstream o;
    std::lock_guard lock(s.m);
    o<<"{\"armed\":"<<(gpuStagesArmed.load()?1:0)<<",\"status\":\""<<s.status<<"\",\"clock\":\"vulkan-timestamp\",\"timestampPeriod\":"<<s.period<<",\"validBits\":"<<s.validBits
     <<",\"slots\":"<<slotCount<<",\"framesBegun\":"<<s.begun<<",\"frames\":"<<s.frames<<",\"unavailable\":"<<s.unavailable<<",\"stale\":"<<s.stale<<",\"incomplete\":"<<s.incomplete
     <<",\"abandoned\":"<<s.abandoned<<",\"deferredMarkers\":"<<s.deferred<<",\"commandMismatch\":"<<s.mismatch<<",\"outliers\":"<<s.outliers<<",\"readErrors\":"<<s.readErrors
     <<",\"lastError\":"<<int(s.lastError)<<",\"passOverflow\":"<<s.passOverflow<<",\"unclassifiedPasses\":"<<s.unclassified<<",\"classOverflow\":"<<s.classOverflow
     <<",\"passMode\":"<<passMode()<<",\"nextPerPass\":"<<(s.nextPerPass?1:0)<<",\"perPassFrames\":"<<s.perPassFrames<<",\"signatureTruncated\":"<<s.signatureTruncated<<",\"stages\":{";
    o.setf(std::ios::fixed);o.precision(3);
    for(unsigned k=0;k<stageCount;++k){const auto& w=s.windows[k];
        o<<(k?",":"")<<"\""<<stageName(k)<<"\":{\"samples\":"<<w.count<<",\"total\":"<<w.total;
        if(w.count){std::array<std::uint64_t,windowSize> v{};std::copy_n(w.values.begin(),w.count,v.begin());std::sort(v.begin(),v.begin()+w.count);
            long double sum=0;for(unsigned i=0;i<w.count;++i)sum+=v[i];
            const auto pct=[&](unsigned p){return double(v[std::min<unsigned>(w.count-1,(w.count*p+99)/100-1)])/1e6;};
            o<<",\"p50Ms\":"<<pct(50)<<",\"p95Ms\":"<<pct(95)<<",\"lastMs\":"<<double(w.last)/1e6<<",\"meanMs\":"<<double(sum/w.count)/1e6;}
        o<<"}";}
    // engine render passes by framebuffer class, largest GPU time first
    std::array<unsigned,classCapacity> order{};for(unsigned i=0;i<s.classCount;++i)order[i]=i;
    std::sort(order.begin(),order.begin()+s.classCount,[&](unsigned a,unsigned b){return s.classes[a].sumNs>s.classes[b].sumNs;});
    o<<"},\"passes\":[";
    for(unsigned n=0;n<s.classCount;++n){const auto& c=s.classes[order[n]];
        o<<(n?",":"")<<"{\"key\":\""<<c.key<<"\",\"eye\":"<<(c.eye?1:0)<<",\"shadow\":"<<(c.shadow?1:0)<<",\"region\":\""<<regionName(c.region)<<"\",\"passes\":"<<c.passes<<",\"frames\":"<<c.frames
         <<",\"msPerFrame\":"<<(s.perPassFrames?double(c.sumNs)/1e6/double(s.perPassFrames):0.0)<<",\"msPerPass\":"<<(c.passes?double(c.sumNs)/1e6/double(c.passes):0.0)<<",\"maxMs\":"<<double(c.maxNs)/1e6<<"}";}
    // per settings state: sums for mean/stddev (ms), frames after the 8-frame settle
    auto stat=[&](const Stat& x){o<<"{\"count\":"<<x.count<<",\"sum\":"<<x.sum<<",\"sumSq\":"<<x.sumSq<<",\"min\":"<<x.min<<",\"max\":"<<x.max<<"}";};
    o<<"],\"currentState\":\""<<(s.current<s.stateCount?s.states[s.current].sig:"")<<"\",\"stateEvictions\":"<<s.evictions<<",\"byState\":{";
    for(unsigned i=0;i<s.stateCount;++i){const auto& st=s.states[i];
        o<<(i?",":"")<<"\""<<st.sig<<"\":{\"frames\":"<<st.frames<<",\"settling\":"<<st.settling<<",\"cpuFrames\":"<<st.cpuFrames<<",\"cpuSettling\":"<<st.cpuSettling<<",\"stagesMs\":{";
        for(unsigned k=0;k<stageCount;++k){o<<(k?",":"")<<"\""<<stageName(k)<<"\":";stat(st.stages[k]);}
        o<<"},\"intervalMs\":";stat(st.interval);o<<",\"hudHz\":";stat(st.hudHz);
        o<<",\"cpu\":{\"depthCpuMs\":";stat(st.depthCpuMs);o<<",\"depthObjects\":";stat(st.depthObjects);o<<",\"colorCpuMs\":";stat(st.colorCpuMs);
        o<<",\"colorObjects\":";stat(st.colorObjects);o<<",\"stereoDrawCpuMs\":";stat(st.stereoDrawCpuMs);o<<"}";
        o<<",\"inserted\":";stat(st.inserted);o<<",\"rightMinusLeftMs\":";stat(st.rightMinusLeft);o<<"}";}
    o<<"}";
    // eyePassByIndex (_pt1 frames): per left region and pass ordinal
    o<<",\"eyePassByIndex\":{";
    for(unsigned g=0;g<eyeGroups;++g){o<<(g?",":"")<<"\""<<eyeGroupNames[g]<<"\":[";bool first=true;
        for(unsigned k=0;k<eyeOrdinals;++k){const auto& e=s.eyePass[g][k];if(!e.passes)continue;
            o<<(first?"":",")<<"{\"k\":"<<k<<",\"passes\":"<<e.passes<<",\"msSum\":"<<double(e.sumNs)/1e6<<",\"msMean\":"<<double(e.sumNs)/1e6/double(e.passes)
             <<",\"drawsSum\":"<<e.sumDraws<<",\"drawsMean\":"<<double(e.sumDraws)/double(e.passes)<<"}";first=false;}
        o<<"]";}
    o<<"}";
    // depthByDraws (DETAIL frames by the left depth tape's pass-0 draws) and the lite span
    o<<",\"depthByDraws\":[";
    for(unsigned k=0;k<depthBins;++k){const auto& b=s.depthBins[k];
        o<<(k?",":"")<<"{\"from\":"<<k*100<<",\"n\":"<<b.n<<",\"leftDepthMs\":"<<(b.n?b.leftMs/double(b.n):0.0)<<",\"rightDepthMs\":"<<(b.n?b.rightMs/double(b.n):0.0)<<"}";}
    const auto& L=s.lite;unsigned pending=0;for(const auto& x:L.slots)pending+=x.pending?1u:0u;
    o<<"],\"lite\":{\"armed\":"<<(gpuLiteArmed.load()?1:0)<<",\"property\":"<<(liteEnabled()?1:0)
     <<",\"status\":\""<<(L.failed?L.status:L.sinceEnd>=16?"NO_END_HOOK":"OK")<<"\",\"poolStatus\":\""<<L.status<<"\",\"source\":\""<<L.source<<"\""
     <<",\"frames\":"<<L.frames<<",\"detailFrames\":"<<L.detailFrames<<",\"begins\":"<<L.begins<<",\"ends\":"<<L.ends<<",\"pending\":"<<pending
     <<",\"late\":"<<L.late<<",\"stale\":"<<L.stale<<",\"abandoned\":"<<L.abandoned<<",\"mismatch\":"<<L.mismatch<<",\"outliers\":"<<L.outliers
     <<",\"readErrors\":"<<L.readErrors<<",\"serial\":"<<liteSerial.load()<<",\"lastUs\":"<<gpuLiteFrameUs();
    {const auto& w=L.window;std::array<std::uint64_t,windowSize> v{};std::copy_n(w.values.begin(),w.count,v.begin());std::sort(v.begin(),v.begin()+w.count);
     const auto pct=[&](unsigned p)->std::uint64_t{return w.count?v[std::min<unsigned>(w.count-1,(w.count*p+99)/100-1)]:0;};
     o<<",\"p50Us\":"<<pct(50)<<",\"p90Us\":"<<pct(90)<<",\"p95Us\":"<<pct(95)<<",\"maxUs\":"<<(w.count?v[w.count-1]:0)<<",\"windowSamples\":"<<w.count;}
    {std::array<std::uint32_t,gpuLiteWindowMax> ring{};const unsigned n=gpuLiteRing(ring.data(),gpuLiteWindowMax);o<<",\"ring\":[";for(unsigned i=0;i<n;++i)o<<(i?",":"")<<ring[i];o<<"]";}
    o<<"}}";return o.str();
}
