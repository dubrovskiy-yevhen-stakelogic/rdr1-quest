#pragma once
// LOW-FREQUENCY PASSES (debug.rdrvr.lowfreq): a per-PIPELINE coarse fragment shading rate for engine passes whose output
// is low-frequency (default: sky / clouds, AO spheres, volumetric light geometry; opt-in: the deferred light volumes, lamp
// shadow lights, glows, soft particles, the sun shadow collector). No shader edit, no render-pass change.
// How (VK_KHR_fragment_shading_rate, already enabled by the foveation device plan, GameFragmentRate.cpp):
//  * TWIN pipelines. The engine's own pipeline is created unchanged (the bridge's {1x1, KEEP,
//    REPLACE} = the foveation attachment decides). For every classified pipeline a coarse twin is created right after, in
//    the same hook (TraceEntry rateCreatePipelines -> lowFreqTwins), through the SAME bridge / camera / occlusion / stereo /
//    ablate registration as an engine batch (copies of the engine's create infos, derivative bits cleared), so a bound twin
//    is an ordinary known pipeline everywhere (stereo tape + right-eye replay, frozen-camera variants, census). The twin
//    chains {2x2 (sky: 2x2|4x4), KEEP, MAX}: final = combine1(combine0(pipeline, primitive), attachment) = MAX(pipeline,
//    attachment) per axis (spec "Fragment Shading Rates"; MAX = per-component max). The foveation map stays in charge
//    wherever it is coarser (HIGH: 2x2 ring, 4x4 edge); only the map's 1x1 fovea is shaded 2x2 by a twin.
//  * WHEN a twin is bound (bind time, TraceEntry ablateBindPipeline, before the camera / stereo hooks see the handle): only
//    inside a FOVEATED EYE pass whose foveation map is level HIGH or ULTRA (IndexedDispatch begin hook: FoveationRegistry
//    begin reports the level of the map it used; minLevel). Everywhere else - FOVEATION OFF / LOW / MEDIUM (AUTO below
//    HIGH, SWEEP), non-eye targets (water / environment reflections, full-rate map), passes the registry did not foveate,
//    secondary command buffers - the engine's own 1x1 pipeline is bound, so FOVEATION OFF still means full rate. The level
//    follows live (per pass begin); the draw hook rebinds the right member when a pass changed since the bind (a pipeline
//    bound before vkCmdBeginRenderPass or kept across passes), through the same camera / stereo bind route, so the tape
//    records exactly what the left eye drew and the right-eye replay binds the same handle with the same level's map.
//  * Depth / stencil semantics are untouched: rate only changes how many pixels one fragment SHADER invocation covers.
//    "Multi-pixel fragments defined by setting the fragment shading rate have one set of samples per pixel"; the stencil and
//    depth tests compare "at each sample's framebuffer coordinates" and a failing sample loses only its own coverage (spec
//    fragops). What a coarse fragment shares: its shaded value and a discard decision (then the depth / stencil writes that
//    follow it). Guard (debug.rdrvr.lowfreq.guard, default 2 strict): a fragment shader that can discard
//    (OpKill / OpTerminateInvocation / OpDemoteToHelperInvocation) in a pipeline that writes depth or stencil (static state
//    or dynamic depth-write / stencil op / write mask / test enable) stays 1x1. guard 1 exempts the light groups (every
//    discard of the corpus' light modules is a no-contribution cull: length(rgb) < 1e-4, outside range / cone / shadow
//    frustum; unproven for the stencil marks it leaves), guard 0 = off. The first 8 discard + write pipelines are reported
//    with their depth / stencil state (discardWrite[]). Never coarse: FragDepth / SampleMask / SampleId / SamplePosition /
//    FragStencilRefEXT, SampleRateShading, depth-replacing modes, FragCoord readers (screen-space dither patterns would grow
//    to the block size), no colour write, MSAA / sample shading / alpha-to-coverage, dynamic fragment shading rate.
//  * Visual cost: inside the fovea a twin's output is 2x2-shaded like the HIGH ring already is. The
//    coarse fragment's varyings are interpolated at the BLOCK CENTRE - the shared corner of 4 pixels -
//    and every grouped shader reads the G-buffer / depth with OpImageSampleImplicitLod: with a LINEAR
//    sampler the depth is averaged across a silhouette (a reconstructed point between both surfaces),
//    with POINT one of the 4 texels is taken. All 4 pixels of a block that pass the volume's own depth /
//    stencil test get that one result, so G-buffer silhouettes inside a lit volume get a 1-px halo /
//    notch, bump and pow(20) specular detail is halved and lamp-shadow PCF edges get blocky - hence
//    lights / shadowLights are OPT-IN. AO / volumetric fog share the same 1-px depth-edge fringe
//    (low-frequency, blended); the sky's implicit-LOD taps pick one mip coarser (stars, moon, cloud
//    noise). The 2x2 grid is screen-locked: under head motion such edges crawl, which a frozen-camera
//    eye dump does not show.
// Classification: the fragment shader's SPIR-V OpEntryPoint name (RAGE technique) through
// CategoryAblation.h's parser / rule table (ablate::parseSpirv, family), then groupOf() below. Shader
// modules are classified at vkCreateShaderModule (TraceEntry createShader, the ORIGINAL code),
// pipelines at vkCreateGraphicsPipelines (TraceEntry rateCreatePipelines, before the bridge).
// Properties, LATCHED at the first engine pipeline creation (twins are created with their pipelines;
// a change needs a restart):
//   debug.rdrvr.lowfreq       group mask: number (0x.. hex or decimal) or names joined by | , + or space:
//                             lights 0x1 (PS_PointLight / SpotLight / FillLight / DirectionalLight /
//                             DirectionalCookieLight, *Geometry: the deferred light stage), shadowLights
//                             0x2 (PS_PointLight_Shadow* / _VSDCT* / PS_SpotLightShadow*: lamp
//                             shadow-mapped lights), sky 0x4 (ps_main*, *Cloud*, *Sky*; not the
//                             PSCloudsEnvMapParab* environment-map passes), ao 0x8 (PS_AOSphere),
//                             volumetric 0x10 (PS_VolumetricPointLightGeometry, PS_lightcone*,
//                             PS_extrudeRays), glow 0x20 (window / door / glass glows, PSGlow,
//                             lightglow), fx 0x40 (PS_Draw* particles, FogCard), sun 0x80
//                             (PS_DirectionalShadow*: the sun / moon shadow collector), all, default.
//                             Unset / "default" = 0x1c (sky|ao|volumetric). 0 / off = kill switch
//                             (no twin is created).
//   debug.rdrvr.lowfreq.size  2 | 4: the sky group's size (default 2). Every other group is 2x2.
//   debug.rdrvr.lowfreq.pin   1 = 4x4 for every group in the mask (to SEE the set on an eye dump),
//                             default 0.
//   debug.rdrvr.lowfreq.guard 2 (default) strict, 1 light groups exempt, 0 off (see above).
// Capability (gameFragmentRateCapabilities): the bridge converts this device's pipelines,
// pipelineFragmentShadingRate and attachmentFragmentShadingRate enabled,
// fragmentShadingRateNonTrivialCombinerOps (MAX), 2x2 listed (4x4 for size 4 / pin, else clamped to
// 2x2). Otherwise UNAVAILABLE: nothing changes. Batches of more than 64 pipelines stay native (bridge
// rule).
// Cost: one extra pipeline compile per classified pipeline (twins.ms / maxMs in the report); per pass
// begin / bind / draw one relaxed load while no twin exists, else a pass flag per command buffer, a
// 4096-slot lookup per graphics bind and an 8-entry compare per draw.
// Report graphics.lowFreq {available, why, mask, families, sizes, caps, minLevel, liveLevel,
// pipelinesCoarse, twins{..}, passes{..}, drawsCoarsePerFrame, drawsFullPerFrame, rebinds{..},
// groups{..}, declined{..}, discardWrite[..], property{..}}.
// Draws are counted through the engine hooks (left eye + native right-eye stages, e.g. the light stage
// of both eyes); the replayed right colour tape binds the same handles and is not counted.
#include "CategoryAblation.h"           // ablate::parseSpirv / Family (technique rules)
#include "runtime/FragmentRateBridge.h" // runtime::FragmentRateCapabilities, Vulkan rate structs
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace rdrvr::engine::lowfreq {
// ---------------------------------------------------------------- groups
enum Group : unsigned {Lights,ShadowLights,Sky,Ao,Volumetric,Glow,Fx,Sun,GroupCount};
inline constexpr const char* groupNames[GroupCount]{"lights","shadowLights","sky","ao","volumetric","glow","fx","sun"};
inline constexpr unsigned defaultMask=0x1cu,allMask=(1u<<GroupCount)-1u,unsetMask=0u; // OFF unless set
// defaultMask = the 'default' token: sky|ao|volumetric (lights opt-in)
inline constexpr std::uint8_t none=0xff;
inline constexpr int minLevel=3; // Twins bind only in eye passes whose foveation map is HIGH (3) or ULTRA (4)
// Fragment technique -> group (first match). fs = OpEntryPoint name, f = its CategoryAblation family.
inline std::uint8_t groupOf(const char* fs,ablate::Family f) noexcept {
    if(!fs||!*fs||f==ablate::ShadowCaster||f==ablate::Post)return none;
    if(f==ablate::SunReceive)return Sun;
    auto has=[fs](const char* t){return std::strstr(fs,t)!=nullptr;};
    if(has("AOSphere"))return Ao;
    if(has("VolumetricPointLight")||has("lightcone")||has("extrudeRays"))return Volumetric;
    if(has("lightglow")||has("Glow")||has("Gels"))return Glow;
    if(f==ablate::Light){
        if(has("Shadow")||has("VSDCT"))return has("PointLight")||has("SpotLight")?std::uint8_t(ShadowLights):none;
        return has("PointLight")||has("SpotLight")||has("DirectionalLight")||has("DirectionalCookie")||has("FillLight")||has("lightprepass")?std::uint8_t(Lights):none;}
    // not PSTexturedBillboardNoCloudShadows; not PSCloudsEnvMapParab* (environment map, never an eye pass: no twin needed)
    if(f==ablate::Sky)return (has("ps_main")||has("Cloud")||has("Sky"))&&!has("Billboard")&&!has("EnvMap")?std::uint8_t(Sky):none;
    if(f==ablate::Fx)return has("PS_Draw")||has("Particle")||has("FogCard")?std::uint8_t(Fx):none;
    return none;
}

// ---------------------------------------------------------------- shader modules (cold, any thread)
struct Module {std::uint8_t group{none};bool discard{},fragCoord{},unsafe{};char name[48]{};};
// Fragment module scan: group from the entry name; discard instructions; builtins / modes that must stay 1x1.
inline bool scan(const std::uint32_t* code,std::size_t bytes,Module& m) noexcept {
    m=Module{};ablate::ShaderInfo s{};if(!ablate::parseSpirv(code,bytes,s)||s.model!=4u)return false; // 4 = Fragment
    std::snprintf(m.name,sizeof m.name,"%s",s.name);m.group=groupOf(s.name,s.family);
    const std::size_t n=bytes/4;
    auto builtin=[&m](std::uint32_t b){
        if(b==15u)m.fragCoord=true;                                                          // FragCoord
        else if(b==22u||b==20u||b==18u||b==19u||b==5014u)m.unsafe=true;};                    // FragDepth SampleMask SampleId SamplePosition FragStencilRefEXT
    for(std::size_t i=5;i<n;){const std::uint32_t op=code[i]&0xffffu,len=code[i]>>16;if(!len||i+len>n)break;
        if(op==17u&&len>=2&&code[i+1]==35u)m.unsafe=true;                                    // OpCapability SampleRateShading
        else if(op==16u&&len>=3&&(code[i+2]==12u||code[i+2]==5027u))m.unsafe=true;           // OpExecutionMode DepthReplacing / StencilRefReplacingEXT
        else if(op==71u&&len>=4&&code[i+2]==11u)builtin(code[i+3]);                          // OpDecorate BuiltIn
        else if(op==72u&&len>=5&&code[i+3]==11u)builtin(code[i+4]);                          // OpMemberDecorate BuiltIn
        else if(op==252u||op==4416u||op==5380u)m.discard=true;                               // OpKill / OpTerminateInvocation / OpDemoteToHelperInvocation
        i+=len;}
    return true;
}

// ---------------------------------------------------------------- configuration (latched once)
#ifndef PROP_VALUE_MAX
#define PROP_VALUE_MAX 92
#endif
struct Config {unsigned mask{unsetMask},skySize{2},guard{2};bool pin{};}; // Guard 2 (strict) by default
struct PropertyText {char mask[PROP_VALUE_MAX]{},size[PROP_VALUE_MAX]{},pin[PROP_VALUE_MAX]{},guard[PROP_VALUE_MAX]{};};
// Group mask: "" / "default" -> default; numbers (strtoul base 0) and names joined by | , + or space; false = unparsable.
inline bool parseMask(const char* text,unsigned& out) noexcept {
    out=unsetMask;if(!text||!*text)return true;
    unsigned mask=0;bool any=false;const char* p=text;
    while(*p){
        while(*p=='|'||*p==','||*p=='+'||*p==' ')++p;if(!*p)break;
        const char* e=p;while(*e&&*e!='|'&&*e!=','&&*e!='+'&&*e!=' ')++e;
        char token[24]{};const std::size_t len=std::size_t(e-p);if(len>=sizeof token)return false;std::memcpy(token,p,len);p=e;any=true;
        if(token[0]>='0'&&token[0]<='9'){char* end{};const unsigned long v=std::strtoul(token,&end,0);if(!end||*end||v>0xfffful)return false;mask|=unsigned(v);continue;}
        if(!std::strcmp(token,"default")){mask|=defaultMask;continue;}
        if(!std::strcmp(token,"all")){mask|=allMask;continue;}
        if(!std::strcmp(token,"off")||!std::strcmp(token,"none")){continue;}
        bool hit=false;for(unsigned g=0;g<GroupCount;++g)if(!std::strcmp(token,groupNames[g])){mask|=1u<<g;hit=true;}
        if(!hit&&!std::strcmp(token,"shadowlights")){mask|=1u<<ShadowLights;hit=true;}
        if(!hit)return false;}
    out=any?mask&allMask:unsetMask;return true;
}
struct Resolved {Config config{};std::array<bool,4> bad{};};
inline Resolved resolve(const PropertyText& t) noexcept {
    Resolved r{};unsigned m=unsetMask;if(parseMask(t.mask,m))r.config.mask=m;else{r.bad[0]=true;r.config.mask=unsetMask;}
    if(!t.size[0]||!std::strcmp(t.size,"2"))r.config.skySize=2;else if(!std::strcmp(t.size,"4"))r.config.skySize=4;else{r.bad[1]=true;r.config.skySize=2;}
    if(!t.pin[0]||!std::strcmp(t.pin,"0"))r.config.pin=false;else if(!std::strcmp(t.pin,"1"))r.config.pin=true;else r.bad[2]=true;
    if(!t.guard[0]||!std::strcmp(t.guard,"2"))r.config.guard=2;else if(!std::strcmp(t.guard,"0"))r.config.guard=0;else if(!std::strcmp(t.guard,"1"))r.config.guard=1;else r.bad[3]=true;
    return r;
}
inline PropertyText readProperties() noexcept {
    PropertyText t{};
#ifdef __ANDROID__
    const int saved=errno;
    __system_property_get("debug.rdrvr.lowfreq",t.mask);__system_property_get("debug.rdrvr.lowfreq.size",t.size);
    __system_property_get("debug.rdrvr.lowfreq.pin",t.pin);__system_property_get("debug.rdrvr.lowfreq.guard",t.guard);
    errno=saved;
#endif
    return t;
}

// ---------------------------------------------------------------- per-pipeline decision (pure)
enum Decline : unsigned {DBuiltin,DFragCoord,DNoColor,DMultisample,DDiscardWrite,DDynamicRate,DSize,DBridge,DeclineCount};
inline constexpr const char* declineNames[DeclineCount]{"builtin","fragCoord","noColor","multisample","discardWrite","dynamicRate","size","bridge"};
struct Decision {std::uint8_t group{none},size{};int decline{-1};bool masked{},unknownFs{},fragment{},exempt{},discardWrite{};const Module* module{};};
inline bool dynamicHas(const VkGraphicsPipelineCreateInfo& c,VkDynamicState s) noexcept {
    if(!c.pDynamicState||!c.pDynamicState->pDynamicStates)return false;
    for(std::uint32_t i=0;i<c.pDynamicState->dynamicStateCount&&i<256;++i)if(c.pDynamicState->pDynamicStates[i]==s)return true;
    return false;
}
// Depth or stencil attachment writes possible (static state or dynamic state that could enable them).
inline bool writesDepthStencil(const VkGraphicsPipelineCreateInfo& c) noexcept {
    const auto* ds=c.pDepthStencilState;if(!ds)return false;
    const bool dynDepthTest=dynamicHas(c,VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE),dynDepthWrite=dynamicHas(c,VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE);
    if((ds->depthTestEnable||dynDepthTest)&&(ds->depthWriteEnable||dynDepthWrite))return true;
    if(!ds->stencilTestEnable&&!dynamicHas(c,VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE))return false;
    const bool dynOp=dynamicHas(c,VK_DYNAMIC_STATE_STENCIL_OP),dynMask=dynamicHas(c,VK_DYNAMIC_STATE_STENCIL_WRITE_MASK);
    auto face=[&](const VkStencilOpState& s){
        return (dynOp||s.failOp!=VK_STENCIL_OP_KEEP||s.passOp!=VK_STENCIL_OP_KEEP||s.depthFailOp!=VK_STENCIL_OP_KEEP)&&(dynMask||s.writeMask!=0);};
    return face(ds->front)||face(ds->back);
}
// Groups whose every discard is a no-contribution cull (corpus decompile; guard 1 only).
constexpr bool zeroCull(unsigned group) noexcept {return group==Lights||group==ShadowLights;}
// sizeMask: bit 1 = 2x2 usable, bit 2 = 4x4 usable (caps). find(module) -> const Module* (nullptr = not a known fragment module).
template<class Find> Decision decide(const VkGraphicsPipelineCreateInfo& c,const Config& cfg,unsigned sizeMask,Find find) noexcept {
    Decision d{};VkShaderModule fs{};
    for(std::uint32_t s=0;c.pStages&&s<c.stageCount;++s)if(c.pStages[s].stage==VK_SHADER_STAGE_FRAGMENT_BIT){fs=c.pStages[s].module;d.fragment=true;}
    if(!d.fragment)return d;
    const Module* m=find(fs);if(!m){d.unknownFs=true;return d;}
    d.module=m;d.group=m->group;if(d.group>=GroupCount)return d;
    if(!(cfg.mask&(1u<<d.group))){d.masked=true;return d;}
    auto no=[&d](Decline why){d.decline=int(why);d.size=0;return d;};
    if(m->unsafe)return no(DBuiltin);
    if(m->fragCoord)return no(DFragCoord);
    bool color=false;
    if(const auto* b=c.pColorBlendState;b&&b->attachmentCount){
        if(!b->pAttachments||dynamicHas(c,VK_DYNAMIC_STATE_COLOR_WRITE_ENABLE_EXT))color=true;
        else for(std::uint32_t i=0;i<b->attachmentCount;++i)color|=b->pAttachments[i].colorWriteMask!=0;}
    if(!color)return no(DNoColor);
    const auto* ms=c.pMultisampleState;
    if(!ms||ms->rasterizationSamples!=VK_SAMPLE_COUNT_1_BIT||ms->sampleShadingEnable||ms->alphaToCoverageEnable)return no(DMultisample);
    d.discardWrite=m->discard&&writesDepthStencil(c); // Sampled into discardWrite[] whatever the guard
    if(cfg.guard&&d.discardWrite){if(cfg.guard==1&&zeroCull(d.group))d.exempt=true;else return no(DDiscardWrite);}
    if(dynamicHas(c,VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR))return no(DDynamicRate);
    unsigned size=cfg.pin?4u:d.group==Sky?cfg.skySize:2u;
    if(size==4&&!(sizeMask&4u))size=2;
    if(!(sizeMask&size))return no(DSize);
    d.size=std::uint8_t(size);return d;
}
// Depth / stencil state of a discarding fragment shader in a depth / stencil writing pipeline (first 8).
struct WriteSample {char name[48]{};std::uint8_t group{none};bool exempt{},declined{},depthTest{},depthWrite{},stencilTest{},dynamic{};
    std::array<std::uint8_t,4> front{},back{};}; // failOp, passOp, depthFailOp, writeMask (low byte)
inline WriteSample writeSample(const VkGraphicsPipelineCreateInfo& c,const Decision& d) noexcept {
    WriteSample w{};if(d.module)std::snprintf(w.name,sizeof w.name,"%s",d.module->name);w.group=d.group;w.exempt=d.exempt;w.declined=d.decline==int(DDiscardWrite);
    if(const auto* ds=c.pDepthStencilState){w.depthTest=ds->depthTestEnable;w.depthWrite=ds->depthWriteEnable;w.stencilTest=ds->stencilTestEnable;
        w.front={std::uint8_t(ds->front.failOp),std::uint8_t(ds->front.passOp),std::uint8_t(ds->front.depthFailOp),std::uint8_t(ds->front.writeMask)};
        w.back={std::uint8_t(ds->back.failOp),std::uint8_t(ds->back.passOp),std::uint8_t(ds->back.depthFailOp),std::uint8_t(ds->back.writeMask)};}
    w.dynamic=dynamicHas(c,VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE)||dynamicHas(c,VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE)||dynamicHas(c,VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE)||
              dynamicHas(c,VK_DYNAMIC_STATE_STENCIL_OP)||dynamicHas(c,VK_DYNAMIC_STATE_STENCIL_WRITE_MASK);
    return w;
}

// ---------------------------------------------------------------- runtime state
inline constexpr unsigned tableSlots=4096,tableProbe=32,sampleSlots=8;
// Engine pipeline -> its coarse twin (value = group | 0x10 for 4x4); key stays as a tombstone after forget (probe chains)
struct Slot {std::atomic<std::uintptr_t> key{},twin{};std::atomic<std::uint8_t> value{none};};
struct Counters {
    std::array<std::atomic<std::uint64_t>,GroupCount> seen{},masked{},coarse{},coarse4{},declined{},binds{},draws{},fullBinds{},fullDraws{},shaders{};
    std::array<std::atomic<std::uint64_t>,DeclineCount> why{};
    std::array<std::atomic<std::uint64_t>,5> passLevel{};
    std::atomic<std::uint64_t> batches{},unavailableBatches{},largeBatches{},unknownFs{},tableOverflow{},fragmentShaders{},unsafeShaders{},
        discardShaders{},fragCoordShaders{},pipelinesCoarse{},zeroCullExempt{},discardWrite{},
        twinBatches{},twinNs{},twinMaxNs{},twinFailed{},twinsDestroyed{},twinsOrphaned{},twinsReleased{},
        passesEligible{},passesOther{},passesNonEye{},rebindTwin{},rebindEngine{},cmdEvictions{};
};
struct State {
    std::mutex m;std::unordered_map<std::uintptr_t,Module> modules; // fragment modules only (a reused handle is overwritten / erased)
    std::array<Slot,tableSlots> table{};                             // engine -> twin; lock-free reads
    bool latched{};PropertyText text{};Resolved resolved{};std::array<WriteSample,sampleSlots> samples{};unsigned sampleCount{};
    std::atomic<bool> available{};std::atomic<const char*> why{"NOT_YET"};runtime::FragmentRateCapabilities caps{};
};
inline State state;inline Counters counters;
inline std::atomic<bool> live{}; // true once a twin exists: pass / bind / draw routing armed
inline void bump(std::atomic<std::uint64_t>& a,std::uint64_t n=1) noexcept {a.fetch_add(n,std::memory_order_relaxed);}
inline std::size_t slotOf(std::uintptr_t key) noexcept {std::uint64_t x=key;x^=x>>31;x*=0x9E3779B97F4A7C15ull;x^=x>>29;return std::size_t(x&(tableSlots-1));}
struct Pair {std::uintptr_t twin{};std::uint8_t value{none};};
inline Pair lookup(std::uintptr_t key) noexcept {
    if(!key)return {};std::size_t s=slotOf(key);
    for(unsigned i=0;i<tableProbe;++i,s=(s+1)&(tableSlots-1)){const auto k=state.table[s].key.load(std::memory_order_acquire);
        if(k==key)return {state.table[s].twin.load(std::memory_order_acquire),state.table[s].value.load(std::memory_order_relaxed)};if(!k)return {};}
    return {};
}
// under state.m. twin 0 = forget (only if present); returns the previous twin of key; ok = stored (false: table full).
inline std::uintptr_t mark(std::uintptr_t key,std::uintptr_t twin,std::uint8_t value,bool* ok=nullptr) noexcept {
    if(ok)*ok=false;if(!key)return 0;std::size_t s=slotOf(key);
    for(unsigned i=0;i<tableProbe;++i,s=(s+1)&(tableSlots-1)){auto& t=state.table[s];const auto k=t.key.load(std::memory_order_relaxed);
        if(k==key){const auto old=t.twin.load(std::memory_order_relaxed);t.value.store(twin?value:none,std::memory_order_relaxed);
            t.twin.store(twin,std::memory_order_release);if(ok)*ok=true;return old;}
        if(!k){if(!twin)return 0;t.twin.store(twin,std::memory_order_relaxed);t.value.store(value,std::memory_order_relaxed);
            t.key.store(key,std::memory_order_release);if(ok)*ok=true;return 0;}}
    if(twin)bump(counters.tableOverflow);
    return 0;
}

// [cold] TraceEntry createShader (ORIGINAL code, after the driver): classify fragment modules.
inline void shaderCreated(const std::uint32_t* code,std::size_t bytes,std::uintptr_t module) noexcept {
    if(!module)return;const int saved=errno;Module m{};const bool fragment=scan(code,bytes,m);
    try{std::lock_guard lock(state.m);if(fragment)state.modules[module]=m;else state.modules.erase(module);}catch(...){}
    if(fragment){bump(counters.fragmentShaders);if(m.group<GroupCount)bump(counters.shaders[m.group]);if(m.unsafe)bump(counters.unsafeShaders);
        if(m.discard)bump(counters.discardShaders);if(m.fragCoord)bump(counters.fragCoordShaders);}
    errno=saved;
}
inline void latch(const PropertyText& t) noexcept {if(state.latched)return;state.text=t;state.resolved=resolve(t);state.latched=true;} // under state.m
inline const char* unavailableWhy(const runtime::FragmentRateCapabilities& c,VkDevice d) noexcept {
    if(!c.ready)return "BRIDGE_NOT_READY";if(c.device!=d)return "OTHER_DEVICE";if(!c.pipeline)return "NO_PIPELINE_RATE";
    if(!c.attachment)return "NO_ATTACHMENT_RATE";if(!c.nonTrivialCombiner)return "NO_MAX_COMBINER";
    if(!(c.squareMask&2u)||c.maxFragmentSize.width<2||c.maxFragmentSize.height<2)return "NO_2X2";return nullptr;
}
struct Batch {std::array<std::uint8_t,64> want{},group{};unsigned wanted{};}; // want: coarse size of the twin (0 = no twin)
// [cold] TraceEntry rateCreatePipelines, BEFORE the engine batch: true = b.want holds the twin sizes (lowFreqTwins after the
// engine batch). capabilities() -> runtime::FragmentRateCapabilities (gameFragmentRateCapabilities; called only while the mask is on).
template<class Capabilities> bool plan(VkDevice d,std::uint32_t n,const VkGraphicsPipelineCreateInfo* c,Batch& b,Capabilities capabilities) noexcept {
    const int saved=errno;b=Batch{};b.group.fill(none);bool any=false;Config cfg{};
    try{std::lock_guard lock(state.m);if(!state.latched)latch(readProperties());cfg=state.resolved.config;}catch(...){errno=saved;return false;}
    if(!cfg.mask){state.available.store(false,std::memory_order_relaxed);state.why.store("OFF",std::memory_order_relaxed);errno=saved;return false;}
    if(!n||!c){errno=saved;return false;}
    const runtime::FragmentRateCapabilities caps=capabilities(); // outside state.m (it takes the fragment-rate module mutex)
    try{std::lock_guard lock(state.m);state.caps=caps;
        if(const char* why=unavailableWhy(caps,d)){state.available.store(false,std::memory_order_relaxed);state.why.store(why,std::memory_order_relaxed);
            bump(counters.unavailableBatches);errno=saved;return false;}
        state.available.store(true,std::memory_order_relaxed);state.why.store("OK",std::memory_order_relaxed);
        if(n>64){bump(counters.largeBatches);errno=saved;return false;} // the bridge forwards > 64 natively
        bump(counters.batches);
        const unsigned sizeMask=((caps.squareMask&2u)?2u:0u)|((caps.squareMask&4u)&&caps.maxFragmentSize.width>=4&&caps.maxFragmentSize.height>=4?4u:0u);
        auto find=[](VkShaderModule h)->const Module*{const auto it=state.modules.find(reinterpret_cast<std::uintptr_t>(h));return it==state.modules.end()?nullptr:&it->second;};
        for(std::uint32_t i=0;i<n;++i){const auto x=decide(c[i],cfg,sizeMask,find);b.group[i]=x.group;
            if(x.unknownFs)bump(counters.unknownFs);if(x.group>=GroupCount)continue;
            bump(counters.seen[x.group]);if(x.masked){bump(counters.masked[x.group]);continue;}
            if(x.discardWrite){bump(counters.discardWrite);if(state.sampleCount<sampleSlots)state.samples[state.sampleCount++]=writeSample(c[i],x);}
            if(x.decline>=0){bump(counters.declined[x.group]);bump(counters.why[unsigned(x.decline)]);continue;}
            if(x.exempt)bump(counters.zeroCullExempt);b.want[i]=x.size;++b.wanted;any=true;}
    }catch(...){any=false;b=Batch{};b.group.fill(none);}
    errno=saved;return any;
}
// [cold] after every ENGINE batch: a created engine handle that still names a twin is a reused handle whose destroy was
// not seen (all destroys route through TraceEntry coldDestroyPipeline -> release): forget it, leak + count the old twin.
inline void created(std::uint32_t n,const VkPipeline* out) noexcept {
    if(!out||!n||!live.load(std::memory_order_relaxed))return;const int saved=errno;
    try{std::lock_guard lock(state.m);
        for(std::uint32_t i=0;i<n;++i){const auto key=reinterpret_cast<std::uintptr_t>(out[i]);if(key&&mark(key,0,none))bump(counters.twinsOrphaned);}
    }catch(...){}
    errno=saved;
}
// Twin batch: copies of the planned engine create infos (borrowed nested pointers, valid during the hook), derivative bits
// cleared (basePipelineIndex refers to the engine batch). size: in = wanted, out = what the bridge planned (0/1 = not coarse).
struct TwinBatch {std::array<VkGraphicsPipelineCreateInfo,64> info{};std::array<VkPipeline,64> out{};std::array<std::uint8_t,64> size{},index{};unsigned count{};};
inline bool twinBatch(std::uint32_t n,const VkGraphicsPipelineCreateInfo* c,const VkPipeline* engine,VkResult r,const Batch& b,TwinBatch& t) noexcept {
    t.count=0;if(!c||!engine||!n||n>64||r<0)return false;
    for(std::uint32_t i=0;i<n;++i){if(b.want[i]<2||!engine[i])continue;auto& x=t.info[t.count];x=c[i];
        x.flags&=~VkPipelineCreateFlags(VK_PIPELINE_CREATE_DERIVATIVE_BIT);x.basePipelineHandle=VK_NULL_HANDLE;x.basePipelineIndex=-1;
        t.out[t.count]=VK_NULL_HANDLE;t.size[t.count]=b.want[i];t.index[t.count]=std::uint8_t(i);++t.count;}
    return t.count>0;
}
// [cold] after the twin batch went through the bridge + registries: pair engine -> twin; a twin that failed, came back 1x1
// (unrouted pass / declined plan) or found no table slot is destroyed again (never bound). destroy(VkPipeline) = the engine
// destroy route (TraceEntry coldDestroyPipeline: occlusion / camera registries, then the driver).
template<class Destroy> void twinsCreated(const VkPipeline* engine,const TwinBatch& t,VkResult tr,const Batch& b,std::uint64_t ns,Destroy destroy) noexcept {
    const int saved=errno;bump(counters.twinBatches);bump(counters.twinNs,ns);
    for(auto seen=counters.twinMaxNs.load(std::memory_order_relaxed);ns>seen&&!counters.twinMaxNs.compare_exchange_weak(seen,ns,std::memory_order_relaxed);){}
    for(unsigned k=0;k<t.count&&k<64;++k){const VkPipeline twin=t.out[k];const unsigned i=t.index[k];const std::uint8_t g=b.group[i],s=t.size[k];
        const bool usable=twin&&tr>=0&&(s==2||s==4)&&g<GroupCount&&engine&&engine[i];bool ok=false;
        if(usable)try{std::lock_guard lock(state.m);
            if(mark(reinterpret_cast<std::uintptr_t>(engine[i]),reinterpret_cast<std::uintptr_t>(twin),std::uint8_t(g|(s==4?0x10u:0u)),&ok))bump(counters.twinsOrphaned);
        }catch(...){ok=false;}
        if(ok){bump(counters.coarse[g]);if(s==4)bump(counters.coarse4[g]);bump(counters.pipelinesCoarse);live.store(true,std::memory_order_release);continue;}
        if(g<GroupCount)bump(counters.declined[g]);
        if(!twin||tr<0)bump(counters.twinFailed);else if(s!=2&&s!=4)bump(counters.why[DBridge]);
        if(twin){destroy(twin);bump(counters.twinsDestroyed);}}
    errno=saved;
}
// [cold] TraceEntry coldDestroyPipeline, BEFORE the engine pipeline is destroyed: its twin (0 = none), forgotten here; the
// caller destroys it with the engine's (the engine destroys a pipeline only after its command buffers retired).
inline std::uintptr_t release(std::uintptr_t engine) noexcept {
    if(!engine||!live.load(std::memory_order_relaxed))return 0;const int saved=errno;std::uintptr_t twin=0;
    if(lookup(engine).twin)try{std::lock_guard lock(state.m);twin=mark(engine,0,none);}catch(...){twin=0;}
    if(twin)bump(counters.twinsReleased);errno=saved;return twin;
}

// ---------------------------------------------------------------- recording threads: pass begin / end, bind, draw
// Per command buffer (thread-local, 8 recorded at once per thread): eligible = inside a foveated eye pass at minLevel+;
// engine = the engine's bound graphics pipeline, twin = its twin, sent = what really went to the camera / stereo / driver.
struct Cmd {std::uintptr_t c{},engine{},twin{},sent{};std::uint8_t value{none};bool eligible{},inPass{};};
inline thread_local std::array<Cmd,8> cmds{};inline thread_local unsigned cmdNext{};
inline Cmd* cmdOf(std::uintptr_t c,bool create) noexcept {
    for(auto& x:cmds)if(x.c==c)return &x;
    if(!create)return nullptr;
    Cmd* v=nullptr;for(auto& x:cmds)if(!x.c){v=&x;break;}
    if(!v)for(auto& x:cmds)if(!x.twin&&!x.inPass){v=&x;break;}
    if(!v){v=&cmds[cmdNext];cmdNext=(cmdNext+1)&7u;bump(counters.cmdEvictions);} // a slot with a twin: its next draw is not corrected
    *v=Cmd{};v->c=c;return v;
}
// IndexedDispatch vkCmdBeginRenderPass, after the foveation substitution: level = the level of the foveation map the eye pass
// begins with (FoveationRegistry::begin), -1 = not a foveated eye pass (non-eye target, unknown framebuffer, registry off).
inline void passBegun(std::uintptr_t c,int level) noexcept {
    if(!live.load(std::memory_order_relaxed)||!c)return;
    const bool eligible=level>=minLevel;
    if(level>=0&&level<5)bump(counters.passLevel[unsigned(level)]);else bump(counters.passesNonEye);
    bump(eligible?counters.passesEligible:counters.passesOther);
    if(Cmd* x=cmdOf(c,eligible)){x->inPass=true;x->eligible=eligible;}
}
inline void passEnded(std::uintptr_t c) noexcept {
    if(!live.load(std::memory_order_relaxed)||!c)return;
    if(Cmd* x=cmdOf(c,false)){x->inPass=false;x->eligible=false;}
}
// TraceEntry ablateBindPipeline: the handle to bind instead of the engine's (its twin inside an eligible pass, else itself).
inline std::uintptr_t bind(std::uintptr_t c,bool graphics,std::uintptr_t pipeline) noexcept {
    if(!live.load(std::memory_order_relaxed)||!graphics||!c)return pipeline;
    const Pair p=lookup(pipeline);Cmd* x=cmdOf(c,p.twin!=0);if(!x)return pipeline;
    x->engine=pipeline;x->twin=p.twin;x->value=p.value;x->sent=p.twin&&x->eligible?p.twin:pipeline;
    if(p.twin)bump(x->sent==p.twin?counters.binds[p.value&0xfu]:counters.fullBinds[p.value&0xfu]);
    return x->sent;
}
// TraceEntry ablateDraw / ablateDrawIndexed (after the ablation drop): != 0 = bind this first (the pass changed since the
// bind: twin <-> engine), through the same bind route so the stereo tape records it. Counts coarse / full draws.
inline std::uintptr_t draw(std::uintptr_t c) noexcept {
    if(!live.load(std::memory_order_relaxed))return 0;
    Cmd* x=cmdOf(c,false);if(!x||!x->twin)return 0;
    const std::uintptr_t want=x->eligible?x->twin:x->engine;std::uintptr_t rebind=0;
    if(want!=x->sent){x->sent=want;rebind=want;bump(want==x->twin?counters.rebindTwin:counters.rebindEngine);}
    bump(want==x->twin?counters.draws[x->value&0xfu]:counters.fullDraws[x->value&0xfu]);
    return rebind;
}

// ---------------------------------------------------------------- 1 s window (render thread sceneBegin) + report
struct Window {std::uint64_t scenes{};std::array<double,GroupCount> draws{},binds{},fullDraws{};};
struct Rolling {std::mutex m;std::uint64_t startNs{},scenes{},totalScenes{};std::array<std::uint64_t,GroupCount> lastDraws{},lastBinds{},lastFull{};Window window{};PropertyText now{};std::uint64_t reads{};};
inline Rolling rolling;
inline void sceneBegin() noexcept {
    const auto now=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    try{std::lock_guard lock(rolling.m);auto& r=rolling;++r.scenes;++r.totalScenes;
        if(!r.startNs){r.startNs=now;return;}
        if(now-r.startNs<1'000'000'000ull)return;
        Window w{};w.scenes=r.scenes;
        for(unsigned g=0;g<GroupCount;++g){const auto d=counters.draws[g].load(std::memory_order_relaxed),b=counters.binds[g].load(std::memory_order_relaxed),
            f=counters.fullDraws[g].load(std::memory_order_relaxed);
            w.draws[g]=double(d-r.lastDraws[g])/double(r.scenes);w.binds[g]=double(b-r.lastBinds[g])/double(r.scenes);w.fullDraws[g]=double(f-r.lastFull[g])/double(r.scenes);
            r.lastDraws[g]=d;r.lastBinds[g]=b;r.lastFull[g]=f;}
        r.window=w;r.scenes=0;r.startNs=now;r.now=readProperties();++r.reads; // property now (restartPending), once per second
    }catch(...){}
}
inline void jsonText(std::ostringstream& o,const char* s) {o<<'"';for(;s&&*s;++s){if(*s=='"'||*s=='\\')o<<'\\';if(*s>=' ')o<<*s;}o<<'"';}
// bridgeCoarse: FragmentRateBridge::coarsePipelines(); liveLevel: runtime::foveationLiveLevel (-1 unknown).
inline std::string report(std::uint64_t bridgeCoarse=0,int liveLevel=-1) {
    std::ostringstream o;o.setf(std::ios::fixed);o.precision(2);
    bool latched{};PropertyText t{};Resolved res{};runtime::FragmentRateCapabilities caps{};std::array<WriteSample,sampleSlots> samples{};unsigned sampleCount{};
    {std::lock_guard lock(state.m);latched=state.latched;t=state.text;res=state.resolved;caps=state.caps;samples=state.samples;sampleCount=state.sampleCount;}
    Window w{};PropertyText now{};std::uint64_t scenes{},reads{};
    {std::lock_guard lock(rolling.m);w=rolling.window;now=rolling.now;scenes=rolling.totalScenes;reads=rolling.reads;}
    const auto& cfg=res.config;char mask[16];std::snprintf(mask,sizeof mask,"0x%x",cfg.mask);
    o<<"\"lowFreq\":{\"available\":"<<(state.available.load()?"true":"false")<<",\"why\":";jsonText(o,state.why.load());
    o<<",\"latched\":"<<(latched?1:0)<<",\"mask\":\""<<mask<<"\",\"families\":[";
    {bool first=true;for(unsigned g=0;g<GroupCount;++g)if(cfg.mask&(1u<<g)){o<<(first?"":",")<<'"'<<groupNames[g]<<'"';first=false;}}
    o<<"],\"sizes\":{\"default\":"<<(cfg.pin?4:2)<<",\"sky\":"<<(cfg.pin?4u:cfg.skySize)<<",\"pin\":"<<(cfg.pin?1:0)<<"},\"guard\":"<<cfg.guard
     <<",\"combiner\":\"MAX(KEEP(pipeline,primitive),attachment)\",\"route\":\"twin\",\"minLevel\":"<<minLevel<<",\"liveLevel\":"<<liveLevel;
    o<<",\"caps\":{\"ready\":"<<(caps.ready?1:0)<<",\"pipelineRate\":"<<(caps.pipeline?1:0)<<",\"attachmentRate\":"<<(caps.attachment?1:0)
     <<",\"nonTrivialCombiner\":"<<(caps.nonTrivialCombiner?1:0)<<",\"rates\":[";
    {bool first=true;for(unsigned k=0;k<3;++k)if(caps.squareMask&(1u<<k)){o<<(first?"":",")<<'"'<<(1u<<k)<<'x'<<(1u<<k)<<'"';first=false;}}
    o<<"],\"maxFragmentSize\":["<<caps.maxFragmentSize.width<<','<<caps.maxFragmentSize.height<<"],\"status\":";jsonText(o,caps.status);o<<'}';
    double draws=0,binds=0,full=0;for(unsigned g=0;g<GroupCount;++g){draws+=w.draws[g];binds+=w.binds[g];full+=w.fullDraws[g];}
    o<<",\"pipelinesCoarse\":"<<counters.pipelinesCoarse.load()<<",\"bridgeCoarse\":"<<bridgeCoarse
     <<",\"twins\":{\"batches\":"<<counters.twinBatches.load()<<",\"failed\":"<<counters.twinFailed.load()<<",\"destroyed\":"<<counters.twinsDestroyed.load()
     <<",\"released\":"<<counters.twinsReleased.load()<<",\"orphaned\":"<<counters.twinsOrphaned.load()
     <<",\"ms\":"<<double(counters.twinNs.load())/1e6<<",\"maxMs\":"<<double(counters.twinMaxNs.load())/1e6<<'}';
    o<<",\"passes\":{\"eligible\":"<<counters.passesEligible.load()<<",\"other\":"<<counters.passesOther.load()<<",\"nonEye\":"<<counters.passesNonEye.load()<<",\"byLevel\":[";
    for(unsigned l=0;l<5;++l)o<<(l?",":"")<<counters.passLevel[l].load();
    o<<"]},\"drawsCoarsePerFrame\":"<<draws<<",\"drawsFullPerFrame\":"<<full<<",\"bindsCoarsePerFrame\":"<<binds
     <<",\"rebinds\":{\"toTwin\":"<<counters.rebindTwin.load()<<",\"toEngine\":"<<counters.rebindEngine.load()<<",\"cmdEvictions\":"<<counters.cmdEvictions.load()<<'}'
     <<",\"windowScenes\":"<<w.scenes<<",\"scenes\":"<<scenes;
    o<<",\"groups\":{";
    for(unsigned g=0;g<GroupCount;++g){o<<(g?",":"")<<'"'<<groupNames[g]<<"\":{\"on\":"<<((cfg.mask>>g)&1u)<<",\"shaders\":"<<counters.shaders[g].load()
        <<",\"seen\":"<<counters.seen[g].load()<<",\"masked\":"<<counters.masked[g].load()<<",\"coarse\":"<<counters.coarse[g].load()
        <<",\"coarse4\":"<<counters.coarse4[g].load()<<",\"declined\":"<<counters.declined[g].load()<<",\"drawsPerFrame\":"<<w.draws[g]
        <<",\"fullDrawsPerFrame\":"<<w.fullDraws[g]<<",\"bindsPerFrame\":"<<w.binds[g]<<",\"draws\":"<<counters.draws[g].load()<<",\"fullDraws\":"<<counters.fullDraws[g].load()<<'}';}
    o<<"},\"declined\":{";for(unsigned k=0;k<DeclineCount;++k)o<<(k?",":"")<<'"'<<declineNames[k]<<"\":"<<counters.why[k].load();
    o<<"},\"zeroCullExempt\":"<<counters.zeroCullExempt.load()<<",\"discardWriteSeen\":"<<counters.discardWrite.load()<<",\"discardWrite\":[";
    for(unsigned i=0;i<sampleCount&&i<sampleSlots;++i){const auto& s=samples[i];o<<(i?",":"")<<"{\"fs\":";jsonText(o,s.name);
        o<<",\"group\":\""<<(s.group<GroupCount?groupNames[s.group]:"none")<<"\",\"exempt\":"<<(s.exempt?1:0)<<",\"declined\":"<<(s.declined?1:0)
         <<",\"depth\":["<<(s.depthTest?1:0)<<','<<(s.depthWrite?1:0)<<"],\"stencilTest\":"<<(s.stencilTest?1:0)<<",\"dynamic\":"<<(s.dynamic?1:0)
         <<",\"front\":["<<unsigned(s.front[0])<<','<<unsigned(s.front[1])<<','<<unsigned(s.front[2])<<','<<unsigned(s.front[3])
         <<"],\"back\":["<<unsigned(s.back[0])<<','<<unsigned(s.back[1])<<','<<unsigned(s.back[2])<<','<<unsigned(s.back[3])<<"]}";}
    o<<"],\"batches\":{\"planned\":"<<counters.batches.load()<<",\"unavailable\":"<<counters.unavailableBatches.load()<<",\"large\":"<<counters.largeBatches.load()
     <<"},\"shaders\":{\"fragment\":"<<counters.fragmentShaders.load()<<",\"unsafe\":"<<counters.unsafeShaders.load()<<",\"discard\":"<<counters.discardShaders.load()
     <<",\"fragCoord\":"<<counters.fragCoordShaders.load()<<",\"unknownFsPipelines\":"<<counters.unknownFs.load()<<",\"tableOverflow\":"<<counters.tableOverflow.load()<<'}';
    o<<",\"property\":{\"mask\":";jsonText(o,t.mask);o<<",\"size\":";jsonText(o,t.size);o<<",\"pin\":";jsonText(o,t.pin);o<<",\"guard\":";jsonText(o,t.guard);
    o<<",\"bad\":["<<res.bad[0]<<','<<res.bad[1]<<','<<res.bad[2]<<','<<res.bad[3]<<"],\"now\":{\"mask\":";jsonText(o,now.mask);o<<",\"size\":";jsonText(o,now.size);
    o<<",\"pin\":";jsonText(o,now.pin);o<<",\"guard\":";jsonText(o,now.guard);o<<"},\"reads\":"<<reads;
    const bool pending=reads&&latched&&(std::strcmp(now.mask,t.mask)||std::strcmp(now.size,t.size)||std::strcmp(now.pin,t.pin)||std::strcmp(now.guard,t.guard));
    o<<",\"restartPending\":"<<(pending?1:0)<<"}}";
    return o.str();
}
} // namespace rdrvr::engine::lowfreq
