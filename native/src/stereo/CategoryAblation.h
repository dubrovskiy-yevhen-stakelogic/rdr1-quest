#pragma once
// GEOMETRY COST PROFILER ("ablate"). Measures WHERE the GPU time of a frame goes, per draw
// category, on the device, with the picture checkable per category.
// debug.rdrvr.ablate=<scheme> (default unset = OFF): every RenderScene (TraceEntry observeScene ->
// sceneBegin) advances a window schedule B C0 B C1 B C2 ... (B = baseline, Ck = category k
// suppressed; window = debug.rdrvr.ablate.window scenes, default 90, of which the first
// debug.rdrvr.ablate.settle, default 30, are not measured). Measured per window: mean GPU span of
// the engine frame command buffer (GpuLite.h ring: the lite CmdBegin->CmdEnd span outside DETAIL,
// the DETAIL readBack's "frame" span inside it; the right eye is spliced into the same command
// buffer, so BOTH eyes are in it) and the mean frame interval. Per category the paired estimator d
// = mean(Ck) - (mean(B before) + mean(B after))/2 removes slow drift (heat, clock, scene); report
// mean, SE, median. Suppression: the engine's vkCmdDraw / vkCmdDrawIndexed is dropped in
// TraceEntry ablateDraw* BEFORE the camera hook (cameraDraw*: target-route discard, probe, stereo
// tape record, scene routes) - the place where TargetRouteGuard already drops draws. State ops
// (pipeline / descriptor / vertex / index binds, push constants, barriers, render passes, clears)
// are untouched; the right eye replays the left tape which never got the draw (= skips the same
// draws); right-eye NATIVE stages (stereo pass fallbacks, light stage) decide the same way
// (decision = f(draw bits), never f(eye)). Never dropped: draws of pipelines this header did not
// register (mod passes / raw entry points), draws inside an engine query
// (vkCmdBeginQuery..EndQuery: AI ambient + optics visibility would change), indirect draws
// (IndexedDispatch.fragment.inc, not hooked here). CPU-side engine work (DrawCommon,
// PreDraw/DrawUpdate of creatures, uniform writes) still runs: this measures GPU cost (the
// interval shows CPU effects, if any). Draw bits = thread_local scope bits | bits of the bound
// pipeline:
//  - pipeline: technique family of its VS and FS = SPIR-V OpEntryPoint name (RAGE technique: VSTerrainBlend, PS_Grass,
//    VSShadowDepthSkinned, PS_PointLight_Shadow ...; rules[] below, checked against a 2048-module shader corpus)
//    + skinned (gBoneMtx / *Skinned) + instanced (gInstancedMtx) + depth-only (no colour attachment) + registered;
//  - scopes: stock depth / light / colour stage (retailDepth / retailLights / retailColor exact; the stereo pass's own depth and
//    light calls get the same exclusive stage bit through TraceEntry ablateStereoDepth / ablateStereoLights, so a stage bit never
//    depends on the eye), stereo pass (eyecull::stereoPassDepth), grass manager draw,
//    sun shadow atlas render, sun shadow collector call, lamp DrawShadow, and the DrawCommon object (bodyDraw): class
//    (creature / matrix set / rigid, kind != 0, instance != 0) and buckets of eye-pixel size (sphere diameter, SmallObjectCull metric),
//    distance and bounding radius (metres) of the root-matrix cull sphere.
//  - owner family: the depth stage draws every model-like effect (model, cloth, weapon...) with the SAME generic
//    ShadowBlend techniques (pipeline bit AShared). Such a draw inside a DrawCommon object takes the colour-stage family of the same
//    geometry (key = drawable + draw arguments, learned from the colour stages of the previous scenes, value frozen for the whole
//    scene so both eyes decide alike): the family rows are then "the objects of that family, depth + colour", not "the technique".
// Category = {any, all, none} masks over those bits. Schemes: pass (shadow passes, grass, lights), family (techniques), object
// (DrawCommon classes and buckets), all; cycle:<category> = that one only (B C B C ...); count = census only (nothing suppressed);
// pin:<category> = that category suppressed every scene (look at the picture / eye dumps to verify what a category is). Changing the property resets the counters (generation+1);
// switching it off keeps them (and the scheme they belong to) for the last snapshot.
// Off: one relaxed load per draw / bind / scope, one property read per 64 RenderScenes; shader/pipeline creation always classifies
// (cold, needed when the profiler is switched on later).
// Report graphics.ablation (cumulative sums: diff two snapshots for a time window). For DETAIL per-stage
// deltas GameXr stereoGpuState may call appendSignature(sig,size) (before the _x item): byState keys get _abb / _ab<k>.
#include "GpuLite.h"        // stereo::gpuLiteSerial (+1 per span, lite or DETAIL)
#include "SmallObjectCull.h" // SmallObjectCull::sphere / eyeHeightPx (renDrawBucketInfo cull sphere)
#include "EyeCull.h"        // eyecull::stereoPassDepth (the stereo pass consumer: right-eye native stages)
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace rdrvr::engine::stereo {unsigned gpuLiteRing(std::uint32_t* out,unsigned max) noexcept;} // StereoReplay.h (StereoGpuStages.inc)

namespace rdrvr::engine::ablate {
// ---------------------------------------------------------------- technique families (pipeline bits 0..20)
enum Family : unsigned {Other,Post,ShadowCaster,Light,SunReceive,Grass,Terrain,Water,Sky,Imposter,Character,Fur,Cloth,Weapon,Foliage,
    Treerock,LowLod,Glass,Decal,Fx,Model,FamilyCount};
inline constexpr const char* familyNames[FamilyCount]{"other","post","shadowCaster","light","sunReceive","grass","terrain","water","sky",
    "imposter","character","fur","cloth","weapon","foliage","treerock","lowLod","glass","decal","fx","model"};
struct Rule {Family family;const char* text;};
// OpEntryPoint name -> family, first matching rule wins (substring, case-sensitive). The depth stage draws models with the
// ShadowBlend* techniques (eye depth + shadow collector), the colour stage with the material ones (Bump*, Diffuse* ...): both are
// "model"; the stage itself is a scope bit. Cloth / weapon / fur effects have no depth technique of their own (corpus): their depth draws
// use the generic ShadowBlend ones too (AShared -> owner family per draw, see drop). Scripts can parse the {Family,"text"}
// pairs between RULES BEGIN / RULES END.
// RULES BEGIN
inline constexpr Rule rules[]{
    {Post,"ToneMap"},{Post,"Gauss"},{Post,"Blur"},{Post,"Bloom"},{Post,"Kawase"},{Post,"Copy"},{Post,"Blit"},{Post,"FXAA"},{Post,"TAA"},
    {Post,"DOF"},{Post,"Luminance"},{Post,"BrightPass"},{Post,"PS_Apply"},{Post,"Upscale"},{Post,"SGSR"},{Post,"Lanczos"},{Post,"Bink"},
    {Post,"FullscreenQuad"},{Post,"PassThrough"},{Post,"Mirager"},{Post,"Zombie"},{Post,"Death"},{Post,"Zoom"},{Post,"MotionBlur"},
    {Post,"GenerateVelocity"},{Post,"EdgeMax"},{Post,"ScaleBuffer"},{Post,"SeedRT"},{Post,"ColorFilters"},{Post,"SharpenPass"},
    {Post,"ShadowComposite"},{Post,"Postfx"},{Post,"postfx"},{Post,"PS_Clear"},{Post,"ConvertRGB"},{Post,"MiniAdapt"},{Post,"UI"},
    {Post,"Radar"},{Post,"VSMap"},{Post,"PSMap"},{Post,"WantedMeter"},{Post,"PSScope"},{Post,"PS_Dummy"},{Post,"PSIMB"},{Post,"PSShock"},
    {Post,"PSBlack"},{Post,"PS_Black"},{Post,"PSRDR2"},{Post,"PSLine"},{Post,"PSMissilePath"},{Post,"PS_DebugDraw"},{Post,"InitialScale"},
    {Post,"Injury"},
    {ShadowCaster,"ShadowDepth"},{ShadowCaster,"ShadowOnly"},{ShadowCaster,"ShadowBillboard"},{ShadowCaster,"UnlitDepthOnly"},
    {ShadowCaster,"PSDrawDepth"},{ShadowCaster,"VSPointLightShadow"},{ShadowCaster,"PSPointLightShadow"},
    {ShadowCaster,"CurvedPointLightShadow"},{ShadowCaster,"PointShadowCurvedModel"},{ShadowCaster,"PS_NULL"},{ShadowCaster,"ps_empty"},
    {Light,"PointLight"},{Light,"SpotLight"},{Light,"DirectionalLight"},{Light,"DirectionalCookie"},{Light,"FillLight"},
    {Light,"lightprepass"},{Light,"VolumetricPointLight"},{Light,"PS_Reflection"},{Light,"AOSphere"},{Light,"lightcone"},
    {Light,"lightglow"},{Light,"extrudeRays"},{Light,"PolyFiller"},{Light,"lightEnvironmentMap"},
    {SunReceive,"DirectionalShadow"},{SunReceive,"blendshadows"},
    {Grass,"Grass"},
    {Terrain,"Terrain"},{Terrain,"CliffWall"},
    {Water,"Water"},{Water,"Puddle"},{Water,"RiverSplash"},
    {Sky,"Cloud"},{Sky,"Sky"},{Sky,"ps_main"},{Sky,"vs_main"},{Sky,"TransformSun"},{Sky,"SunOptics"},{Sky,"ProjectorFlare"},
    {Sky,"Spherical"},{Sky,"DpTransform"},{Sky,"vs_stencil"},
    {Imposter,"Imposter"},
    {Character,"Character"},{Character,"SharedUVChar"},{Character,"Hair"},{Character,"VSSkin"},{Character,"PSSkin"},
    {Character,"CharTransparent"},
    {Fur,"Fur"},
    {Cloth,"Cloth"},
    {Weapon,"Weapon"},{Weapon,"Scope"},
    {Foliage,"Foliage"},{Foliage,"Cacti"},{Foliage,"VSCati"},{Foliage,"PSCati"},
    {Treerock,"Treerock"},
    {LowLod,"LowLod"},
    {Glass,"Glass"},{Glass,"WindowGlow"},{Glass,"Gels"}, // Gels: emissive window / door glow cards (VSGels + PSWindowGlow / PSDoorGlows)
    {Decal,"Decal"},{Decal,"Graffiti"},{Decal,"Footprint"},{Decal,"wheeltrack"},{Decal,"Poster"},{Decal,"PSBlood"},
    {Decal,"VSBlood"},
    {Fx,"Particle"},{Fx,"PerVert"},{Fx,"PS_Draw"},{Fx,"VS_Transform"},{Fx,"RainDrop"},{Fx,"Shimmer"},{Fx,"Debris"},{Fx,"Beacon"},
    {Fx,"Glow"},{Fx,"Marker"},{Fx,"Missile"},{Fx,"ProjectorBeam"},{Fx,"makeCutout"},{Fx,"Shard"},{Fx,"RingDepth"},{Fx,"FogCard"},{Post,"PSFog"}, // PSFog: rdr postfx block (fullscreen)
    {Model,"ShadowBlend"},{Model,"Collector"},{Model,"Bump"},{Model,"Diffuse"},{Model,"Alpha"},{Model,"Layer"},{Model,"Reflection"},
    {Model,"Flatten"},{Model,"CurvedModel"},{Model,"Mirror"},{Model,"Wire"},{Model,"Billboard"},{Model,"Unlit"},{Model,"Textured"},
    {Model,"PPP"},{Model,"Normal"},{Model,"Spec"},{Model,"Snow"},{Model,"FlatTool"},{Model,"TransformFlat"},{Model,"SmoothClip"},
    {Model,"PassColor"},
};
// RULES END
inline Family classify(const char* name) noexcept {
    if(!name||!*name)return Other;
    for(const auto& r:rules)if(std::strstr(name,r.text))return r.family;
    return Other;
}
// Fullscreen / generic vertex techniques (VS_DirectionalLight_fs, VS_FullscreenQuad ...) take the fragment technique's family.
inline Family primaryOf(Family vs,Family fs) noexcept {return (vs==Other||vs==Post||vs==Light)&&fs!=Other?fs:vs;}

// ---------------------------------------------------------------- draw bits
enum Bit : unsigned {
    ASkinned=FamilyCount,AInstanced,ADepthOnly,ARegistered,AShared,       // pipeline attributes (21..25); AShared: generic ShadowBlend
    PGrass,PSunAtlas,PSunCollector,PLamp,SDepth,SColor,SStereo,SLight,     // pass / stage scopes (26..33)
    OObject,OCreature,OMatrixSet,ORigid,OKindOther,OInstance,ONoCamera,    // DrawCommon object (34..40)
    OPx,ODist=OPx+5,ORad=ODist+5,BitCount=ORad+5};                         // 5 buckets each: px 41..45, dist 46..50, radius 51..55
static_assert(BitCount<=64,"the draw bits fit one word");
constexpr std::uint64_t bit(unsigned i) noexcept {return 1ull<<i;}
constexpr std::uint64_t fam(Family f) noexcept {return bit(unsigned(f));}
inline constexpr std::uint64_t objectMask=((bit(BitCount-1)<<1)-1)&~(bit(OObject)-1),familyMask=bit(FamilyCount)-1;
inline constexpr std::uint64_t mainStages=bit(SDepth)|bit(SColor),shadowPasses=bit(PSunAtlas)|bit(PLamp);
inline constexpr std::uint64_t stageBits=bit(SDepth)|bit(SColor)|bit(SLight); // exclusive: a stage scope replaces the enclosing stage
inline constexpr float pxEdges[4]{8,32,128,512},distEdges[4]{10,30,80,200},radEdges[4]{0.5f,2,8,32}; // px (1760-px eye), m, m
inline unsigned bucketOf(float v,const float (&edges)[4]) noexcept {unsigned b=0;while(b<4&&!(v<edges[b]))++b;return b;} // NaN -> 4

// ---------------------------------------------------------------- categories and schemes
struct Category {const char* name;std::uint64_t any,all,none;};
constexpr bool matches(std::uint64_t bits,const Category& c) noexcept {return (!c.any||(bits&c.any))&&(bits&c.all)==c.all&&!(bits&c.none);}
inline constexpr Category categories[]{
    // ---- pass (0..8): shadow passes, grass, deferred lights
    {"grassPass",bit(PGrass),0,0},                                          // grassManager draw (colour 0 / depth 2), every caller
    {"sunAtlas",bit(PSunAtlas),0,0},                                        // CShadowMap::RenderIntoShadowMaps (cascade casters)
    {"sunAtlasCreature",0,bit(PSunAtlas)|bit(OCreature),0},                 //   ... casters that are creatures (peds, horses)
    {"sunAtlasSmall",bit(OPx)|bit(OPx+1),bit(PSunAtlas),bit(OCreature)},    //   ... casters < 32 px in the EYE
    {"sunCollector",bit(PSunCollector),0,0},                                // sun shadow collector call (screen space, in the eye pass)
    {"lampShadows",bit(PLamp),0,0},                                         // CCubeShadowMap::DrawShadow (cube faces + spot maps)
    {"lampCreature",0,bit(PLamp)|bit(OCreature),0},                         //   ... casters that are creatures
    {"lights",fam(Light),0,fam(SunReceive)|shadowPasses},                   // deferred light volumes (light stage, both eyes native)
    {"sunReceive",fam(SunReceive),0,shadowPasses},                          // PS_DirectionalShadow* receivers
    // ---- family (9..28): techniques outside the shadow passes; a generic ShadowBlend depth draw of a DrawCommon object counts
    //      for its owner's colour family (cloth / weapon / fur ... have no depth technique of their own), see AShared
    {"terrain",fam(Terrain),0,shadowPasses},{"grass",fam(Grass),0,shadowPasses},{"treerock",fam(Treerock),0,shadowPasses},
    {"foliage",fam(Foliage),0,shadowPasses},{"imposter",fam(Imposter),0,shadowPasses},{"lowLod",fam(LowLod),0,shadowPasses},
    {"character",fam(Character),0,shadowPasses},{"fur",fam(Fur),0,shadowPasses},{"cloth",fam(Cloth),0,shadowPasses},
    {"weapon",fam(Weapon),0,shadowPasses},{"model",fam(Model),0,shadowPasses|bit(ASkinned)},
    {"modelSkinned",0,fam(Model)|bit(ASkinned),shadowPasses},{"water",fam(Water),0,shadowPasses},{"sky",fam(Sky),0,shadowPasses},
    {"glass",fam(Glass),0,shadowPasses},{"decal",fam(Decal),0,shadowPasses},{"fx",fam(Fx),0,shadowPasses},
    {"shadowTechniqueElsewhere",fam(ShadowCaster),0,shadowPasses},           // shadow-depth techniques outside the known shadow scopes
    {"instancedTechnique",0,bit(AInstanced),shadowPasses},                  // pipelines with rage_instancemtx (any family)
    {"otherTechnique",fam(Other),0,shadowPasses|fam(Post)},                 // registered pipelines no rule matched
    // ---- object (29..50): DrawCommon objects in the stock depth/colour stages (+ stereo pass; never the light stage); buckets exclude creatures
    {"creature",mainStages,bit(OCreature),0},{"matrixSet",mainStages,bit(OMatrixSet),0},{"rigid",mainStages,bit(ORigid),0},
    {"kindOther",mainStages,bit(OKindOther),0},{"instanced",mainStages,bit(OInstance),0},
    {"px0_8",mainStages,bit(OPx+0),bit(OCreature)},{"px8_32",mainStages,bit(OPx+1),bit(OCreature)},
    {"px32_128",mainStages,bit(OPx+2),bit(OCreature)},{"px128_512",mainStages,bit(OPx+3),bit(OCreature)},
    {"px512p",mainStages,bit(OPx+4),bit(OCreature)},
    {"dist0_10",mainStages,bit(ODist+0),bit(OCreature)},{"dist10_30",mainStages,bit(ODist+1),bit(OCreature)},
    {"dist30_80",mainStages,bit(ODist+2),bit(OCreature)},{"dist80_200",mainStages,bit(ODist+3),bit(OCreature)},
    {"dist200p",mainStages,bit(ODist+4),bit(OCreature)},
    {"rad0_05",mainStages,bit(ORad+0),bit(OCreature)},{"rad05_2",mainStages,bit(ORad+1),bit(OCreature)},
    {"rad2_8",mainStages,bit(ORad+2),bit(OCreature)},{"rad8_32",mainStages,bit(ORad+3),bit(OCreature)},
    {"rad32p",mainStages,bit(ORad+4),bit(OCreature)},
    {"nonObject",mainStages,0,bit(OObject)|bit(SLight)},                    // depth/colour-stage draws outside DrawCommon (grass, sky, terrain...)
    {"noCamera",mainStages,bit(ONoCamera),0},                               // objects without an eye plan (no px/dist buckets)
};
inline constexpr unsigned categoryCount=unsigned(sizeof(categories)/sizeof(categories[0])),maxCategories=64;
static_assert(categoryCount<=maxCategories,"per-category arrays");
struct Scheme {const char* name;unsigned first,count;};
inline constexpr unsigned passFirst=0,familyFirst=9,objectFirst=29;
inline constexpr Scheme schemes[]{{"off",0,0},{"pass",passFirst,familyFirst-passFirst},{"family",familyFirst,objectFirst-familyFirst},
    {"object",objectFirst,categoryCount-objectFirst},{"all",0,categoryCount}};
inline constexpr unsigned schemeCount=unsigned(sizeof(schemes)/sizeof(schemes[0]));
constexpr bool sameName(const char* a,const char* b) noexcept {while(*a&&*a==*b){++a;++b;}return *a==*b;}
static_assert(sameName(categories[familyFirst].name,"terrain")&&sameName(categories[objectFirst].name,"creature")&&
    sameName(categories[familyFirst-1].name,"sunReceive"),"scheme ranges follow categories[]");

// ---------------------------------------------------------------- shader / pipeline registry (cold, any thread)
struct ShaderInfo {Family family{Other};unsigned model{~0u};bool skinned{},instanced{},terrainNames{},treeNames{},shared{};char name[64]{};std::uint64_t hash{};std::uint32_t words{};};
// Literal string of words [first,end) (nul-terminated, packed little-endian), truncated to cap-1 characters.
inline void literal(const std::uint32_t* w,std::size_t first,std::size_t end,char* out,std::size_t cap) noexcept {
    std::size_t k=0;
    for(std::size_t i=first;i<end;++i)for(unsigned j=0;j<4;++j){const char c=char((w[i]>>(8*j))&0xffu);if(!c){out[k]=0;return;}if(k+1<cap)out[k++]=c;}
    out[k]=0;
}
// SPIR-V: FNV-1a 64 over the words (= StereoReplay patchShaderCreate's hash = corpus-fnv.json keys), the first OpEntryPoint name
// (RAGE technique) and model, OpName/OpMemberName gBoneMtx / gInstancedMtx and the effect's own names: one technique name is
// shared by several effects (PSShadowBlendRDR2 = the depth-stage technique of the model, terrain and tree effects; VSTransformFlat,
// PSReflection ...), so a model/other technique with terrain names (MegaTex*, TerrainBlendMap*, TerrainDiffuseSampler*) is terrain and
// one with tree names (WorldViewProjInverseTrees, TreeColourSampler*, TreeTintSampler*) treerock (foliage when "Alpha").
// Stops at the first OpFunction.
inline bool parseSpirv(const std::uint32_t* code,std::size_t bytes,ShaderInfo& out) noexcept {
    out=ShaderInfo{};if(!code||bytes<20||bytes%4)return false;
    const std::size_t n=bytes/4;out.words=std::uint32_t(n);
    std::uint64_t h=1469598103934665603ull;for(std::size_t i=0;i<n;++i){h^=code[i];h*=1099511628211ull;}out.hash=h;
    if(code[0]!=0x07230203u)return false;
    for(std::size_t i=5;i<n;){
        const std::uint32_t op=code[i]&0xffffu,len=code[i]>>16;
        if(!len||i+len>n)break;
        if(op==15&&len>=4&&!out.name[0]){out.model=code[i+1];literal(code,i+3,i+len,out.name,sizeof out.name);}
        else if((op==5&&len>=3)||(op==6&&len>=4)){char m[40];literal(code,i+(op==5?2:3),i+len,m,sizeof m);
            if(!std::strcmp(m,"gBoneMtx"))out.skinned=true;else if(!std::strcmp(m,"gInstancedMtx"))out.instanced=true;
            else if(!std::strncmp(m,"MegaTex",7)||!std::strncmp(m,"TerrainBlendMap",15)||!std::strncmp(m,"TerrainDiffuseSampler",21))out.terrainNames=true;
            else if(!std::strcmp(m,"WorldViewProjInverseTrees")||!std::strncmp(m,"TreeColourSampler",17)||!std::strncmp(m,"TreeTintSampler",15))out.treeNames=true;}
        else if(op==54)break; // OpFunction: debug names and annotations are over
        i+=len;
    }
    for(char* p=out.name;*p;++p)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'))*p='_'; // JSON-safe report names
    out.family=classify(out.name);if(std::strstr(out.name,"Skinned"))out.skinned=true;
    if(out.name[0]&&(out.family==Model||out.family==Other)){
        if(out.terrainNames)out.family=Terrain;else if(out.treeNames)out.family=std::strstr(out.name,"Alpha")?Foliage:Treerock;}
    out.shared=out.family==Model&&std::strstr(out.name,"ShadowBlend"); // Generic depth technique of model-like effects
    return out.name[0]!=0;
}
inline constexpr unsigned nameCapacity=4096,pipeSlots=16384,probeLimit=64;
struct PipeSlot {std::atomic<std::uintptr_t> key{};std::atomic<std::uint64_t> bits{},meta{};};
struct ModuleRec {std::uint64_t bits{};std::uint16_t name{};Family family{Other};bool shared{};};
struct Registry {
    std::mutex m;std::unordered_map<std::uintptr_t,ModuleRec> modules;std::vector<std::string> names;std::unordered_map<std::string,std::uint16_t> ids;
    std::array<std::uint64_t,nameCapacity> nameHash{};std::array<std::uint8_t,nameCapacity> nameFamily{};
    std::array<PipeSlot,pipeSlots> pipes{}; // lock-free reads (render thread), inserts under m
    std::atomic<std::uint64_t> shaders{},named{},pipelines{},overflow{},updates{},nameOverflow{};
    std::array<std::atomic<std::uint64_t>,FamilyCount> shaderFamilies{},pipelineFamilies{};
};
inline Registry registry;
// meta: VS name id 0..11 | FS name id 12..23 | VkPrimitiveTopology+1 24..27 (0 unknown) | primary family 28..32
constexpr std::uint64_t packMeta(unsigned vs,unsigned fs,unsigned topology,unsigned primary) noexcept {
    return std::uint64_t(vs&0xfffu)|std::uint64_t(fs&0xfffu)<<12|std::uint64_t(topology&0xfu)<<24|std::uint64_t(primary&0x1fu)<<28;}
constexpr unsigned metaVs(std::uint64_t m) noexcept {return unsigned(m&0xfffu);}
constexpr unsigned metaFs(std::uint64_t m) noexcept {return unsigned((m>>12)&0xfffu);}
constexpr unsigned metaFamily(std::uint64_t m) noexcept {return unsigned((m>>28)&0x1fu);}
// Triangles of one draw: list n/3, strip/fan n-2, points/lines 0; times the instance count.
constexpr std::uint64_t triangles(std::uint32_t n,std::uint32_t instances,std::uint64_t meta) noexcept {
    const unsigned t=unsigned(meta>>24)&0xfu;const std::uint64_t per=t==4?n/3u:(t==5||t==6)?(n>=2?n-2u:0u):0u;return per*(instances?instances:1u);}
template<class H> std::uintptr_t handleKey(H h) noexcept {if constexpr(std::is_pointer_v<H>)return reinterpret_cast<std::uintptr_t>(h);else return std::uintptr_t(h);}
inline std::size_t slotOf(std::uintptr_t key) noexcept {std::uint64_t x=key;x^=x>>31;x*=0x9E3779B97F4A7C15ull;x^=x>>29;return std::size_t(x&(pipeSlots-1));}
inline bool insertPipeline(std::uintptr_t key,std::uint64_t bits,std::uint64_t meta) noexcept { // under registry.m
    auto& r=registry;std::size_t s=slotOf(key);
    for(unsigned i=0;i<probeLimit;++i,s=(s+1)&(pipeSlots-1)){auto& p=r.pipes[s];const auto k=p.key.load(std::memory_order_relaxed);
        if(k==key){p.bits.store(bits,std::memory_order_relaxed);p.meta.store(meta,std::memory_order_relaxed);r.updates.fetch_add(1,std::memory_order_relaxed);return true;}
        if(!k){p.bits.store(bits,std::memory_order_relaxed);p.meta.store(meta,std::memory_order_relaxed);p.key.store(key,std::memory_order_release);return true;}}
    r.overflow.fetch_add(1,std::memory_order_relaxed);return false;
}
inline bool lookupPipeline(std::uintptr_t key,std::uint64_t& bits,std::uint64_t& meta) noexcept {
    if(!key)return false;std::size_t s=slotOf(key);
    for(unsigned i=0;i<probeLimit;++i,s=(s+1)&(pipeSlots-1)){const auto& p=registry.pipes[s];const auto k=p.key.load(std::memory_order_acquire);
        if(k==key){bits=p.bits.load(std::memory_order_relaxed);meta=p.meta.load(std::memory_order_relaxed);return true;}if(!k)return false;}
    return false;
}
// One id per (technique name, module family): a name shared by several effects (PSShadowBlendRDR2 of the model / terrain / tree
// effects) gets one row per family in the technique lists, not the family of whichever module came first.
inline std::uint16_t intern(const ShaderInfo& s) { // under registry.m (callers catch)
    auto& r=registry;if(!s.name[0])return 0;
    if(r.names.empty())r.names.emplace_back(""); // id 0 = unknown
    std::string key=s.name;key+='#';key+=familyNames[s.family];
    if(const auto it=r.ids.find(key);it!=r.ids.end())return it->second;
    if(r.names.size()>=nameCapacity){r.nameOverflow.fetch_add(1,std::memory_order_relaxed);return 0;}
    const auto id=std::uint16_t(r.names.size());r.names.emplace_back(s.name);r.ids.emplace(std::move(key),id);
    r.nameHash[id]=s.hash;r.nameFamily[id]=std::uint8_t(s.family);return id;
}
// TraceEntry createShader, with the ORIGINAL (pre eye-position patch) code: the hash then matches corpus-fnv.json.
inline void shaderCreated(const std::uint32_t* code,std::size_t bytes,std::uintptr_t module) noexcept {
    if(!module)return;ShaderInfo s{};const bool named=parseSpirv(code,bytes,s);
    try{std::lock_guard lock(registry.m);ModuleRec rec{};rec.family=s.family;rec.shared=s.shared;
        rec.bits=fam(s.family)|(s.skinned?bit(ASkinned):0)|(s.instanced?bit(AInstanced):0);rec.name=named?intern(s):0;registry.modules[module]=rec;}
    catch(...){return;}
    registry.shaders.fetch_add(1,std::memory_order_relaxed);if(named)registry.named.fetch_add(1,std::memory_order_relaxed);
    registry.shaderFamilies[s.family].fetch_add(1,std::memory_order_relaxed);
}
// TraceEntry rateCreatePipelines (after the driver): Info = VkGraphicsPipelineCreateInfo (templated for the host test).
template<class Info,class Handle> void pipelinesCreated(std::uint32_t n,const Info* infos,const Handle* out,bool ok) noexcept {
    if(!ok||!infos||!out)return;
    try{std::lock_guard lock(registry.m);
        for(std::uint32_t i=0;i<n;++i){const auto key=handleKey(out[i]);if(!key)continue;const auto& info=infos[i];
            std::uint64_t bits=bit(ARegistered);unsigned vsName=0,fsName=0;Family vs=Other,fs=Other;bool any=false,shared=false;
            for(std::uint32_t s=0;info.pStages&&s<info.stageCount;++s){const auto& st=info.pStages[s];
                const auto it=registry.modules.find(handleKey(st.module));if(it==registry.modules.end())continue;
                bits|=it->second.bits;any=true;shared|=it->second.shared;
                if(unsigned(st.stage)==1u){vsName=it->second.name;vs=it->second.family;}else if(unsigned(st.stage)==0x10u){fsName=it->second.name;fs=it->second.family;}}
            if(!any)bits|=fam(Other);
            const unsigned topology=info.pInputAssemblyState?unsigned(info.pInputAssemblyState->topology)+1u:0u;
            if(!info.pColorBlendState||!info.pColorBlendState->attachmentCount)bits|=bit(ADepthOnly);
            const auto primary=primaryOf(vs,fs);
            if(shared&&(bits&familyMask)==fam(Model))bits|=bit(AShared); // Owner family decided per draw (drop)
            if(insertPipeline(key,bits,packMeta(vsName,fsName,topology,primary))){registry.pipelines.fetch_add(1,std::memory_order_relaxed);
                registry.pipelineFamilies[primary].fetch_add(1,std::memory_order_relaxed);}
        }
    }catch(...){}
}

// ---------------------------------------------------------------- control (any thread reads)
enum Mode : unsigned {Off,Count,Cycle,Pin};
inline constexpr const char* modeNames[4]{"off","count","cycle","pin"};
struct Control {
    std::atomic<bool> armed{};std::atomic<int> suppress{-1},upcoming{-1};std::atomic<unsigned> first{},count{},mode{};
    std::atomic<std::uint64_t> epoch{1},otherDraws{},otherDropped{};
};
inline Control control;
inline std::atomic<std::uint64_t> sceneSerial{};
inline bool armed() noexcept {return control.armed.load(std::memory_order_relaxed);}

// ---------------------------------------------------------------- per thread (the render thread is the "owner": sceneBegin)
inline constexpr unsigned boundCache=4;
struct Local {
    std::uint64_t scope{};std::uintptr_t drawable{};int queryDepth{};bool owner{},techniques{};
    struct Bound {std::uintptr_t c{};std::uint64_t bits{},meta{},epoch{};};std::array<Bound,boundCache> bound{};unsigned boundNext{};
    std::array<std::uint32_t,maxCategories> draws{},objects{},right{},dropped{};std::array<std::uint64_t,maxCategories> tris{};
    std::uint64_t frameDraws{},frameTris{},frameUnknown{},frameProtected{},frameObjects{},frameShared{},frameOwned{},frameOwnerUnknown{};
    std::uint64_t camScene{~0ull};bool camValid{};float cam[3]{},focal{},upm{1.f};
};
inline thread_local Local local;
struct Techniques {std::array<std::atomic<std::uint64_t>,nameCapacity> vsDraws{},vsTris{},fsDraws{},fsTris{};};
inline Techniques techniques; // baseline measured frames, render thread

// Owner family of a geometry (render thread only, Local::owner). Key = drawable (renDrawBucketInfo +0x80) and the
// draw arguments (index count, first index, vertex offset: the same grmGeometry draw in the depth and the colour stage). Learned
// from colour-stage draws (non-shared technique); read by shared-technique draws. A value written in scene S is read from scene
// S+1 on (`old` within S): every draw of one scene - left depth before left colour, right depth after it - sees the same owner.
inline constexpr unsigned ownerSlots=8192,ownerProbe=8;inline constexpr std::uint8_t ownerNone=0xff;
struct OwnerSlot {std::uint64_t key{};std::uint32_t scene{};std::uint8_t cur{ownerNone},old{ownerNone};};
struct Owners {std::array<OwnerSlot,ownerSlots> slot{};std::uint64_t inserts{},evictions{};
    void clear() noexcept {slot.fill(OwnerSlot{});inserts=evictions=0;}};
inline Owners owners;
inline std::uint64_t ownerKey(std::uintptr_t drawable,std::uint32_t n,std::uint32_t first,std::int32_t vertex,bool indexed) noexcept {
    std::uint64_t x=std::uint64_t(drawable)*0x9E3779B97F4A7C15ull^(std::uint64_t(n)<<32|first)*0xC2B2AE3D27D4EB4Full^
        (std::uint64_t(std::uint32_t(vertex))<<1|(indexed?1u:0u))*0x165667B19E3779F9ull;
    x^=x>>29;x*=0xBF58476D1CE4E5B9ull;x^=x>>32;return x?x:1u;}
inline unsigned ownerOf(std::uint64_t key,std::uint32_t scene) noexcept {
    std::size_t s=std::size_t(key)&(ownerSlots-1);
    for(unsigned i=0;i<ownerProbe;++i,s=(s+1)&(ownerSlots-1)){const auto& o=owners.slot[s];
        if(o.key==key)return o.scene==scene?o.old:o.cur;if(!o.key)break;}
    return ownerNone;
}
inline void learnOwner(std::uint64_t key,unsigned family,std::uint32_t scene) noexcept {
    std::size_t s=std::size_t(key)&(ownerSlots-1),victim=s;std::uint32_t age=0;
    for(unsigned i=0;i<ownerProbe;++i,s=(s+1)&(ownerSlots-1)){auto& o=owners.slot[s];
        if(o.key==key){if(o.scene!=scene){o.old=o.cur;o.scene=scene;}o.cur=std::uint8_t(family);return;}
        if(!o.key){victim=s;age=~0u;break;}
        if(scene-o.scene>=age){age=scene-o.scene;victim=s;}}
    auto& o=owners.slot[victim];if(o.key)++owners.evictions;++owners.inserts;
    o.key=key;o.scene=scene;o.cur=std::uint8_t(family);o.old=ownerNone; // new this scene: unknown for this scene's reads
}
// Owner family of a colour pipeline: its most specific material family (cloth over model for a VSBumpSpecAO + PSBumpSpecAOCloth
// pair); none for post / light / shadow / unknown techniques.
inline unsigned materialOf(std::uint64_t bits) noexcept {
    const std::uint64_t m=bits&familyMask&~(fam(Grass)-1);return m?unsigned(std::countr_zero(m)):ownerNone;}

// [armed] vkCmdBindPipeline (TraceEntry ablateBindPipeline): the pipeline's bits for this command buffer (4-entry cache per thread).
inline void bindPipeline(std::uintptr_t c,bool graphics,std::uintptr_t pipeline) noexcept {
    if(!graphics||!c)return;auto& t=local;Local::Bound* slot=nullptr;
    for(auto& b:t.bound)if(b.c==c){slot=&b;break;}
    if(!slot){slot=&t.bound[t.boundNext];t.boundNext=(t.boundNext+1)%boundCache;}
    slot->c=c;slot->epoch=control.epoch.load(std::memory_order_relaxed);slot->bits=0;slot->meta=0;lookupPipeline(pipeline,slot->bits,slot->meta);
}
// [armed] vkCmdDraw / vkCmdDrawIndexed (TraceEntry ablateDraw*): count, and true = drop this draw. firstElement / vertex / indexed: the
// draw arguments (firstIndex + vertexOffset, or firstVertex) for the owner key.
inline bool drop(std::uintptr_t c,std::uint32_t n,std::uint32_t instances,std::uint32_t firstElement=0,std::int32_t vertex=0,bool indexed=true) noexcept {
    auto& t=local;std::uint64_t bits=t.scope,meta=0;const auto e=control.epoch.load(std::memory_order_relaxed);
    for(const auto& b:t.bound)if(b.c==c&&b.epoch==e){bits|=b.bits;meta=b.meta;break;}
    if(t.owner&&eyecull::stereoPassDepth)bits|=bit(SStereo);
    if(t.owner&&t.drawable&&(bits&bit(ARegistered))){ // Owner family (shared depth technique <- colour stage)
        const auto key=ownerKey(t.drawable,n,firstElement,vertex,indexed);const auto scene=std::uint32_t(sceneSerial.load(std::memory_order_relaxed));
        if(bits&bit(AShared)){++t.frameShared;const unsigned f=ownerOf(key,scene);
            if(f<FamilyCount){bits=(bits&~familyMask)|fam(Family(f));if(f!=Model)++t.frameOwned;}else ++t.frameOwnerUnknown;}
        else if((bits&bit(SColor))&&!(bits&(bit(ADepthOnly)|shadowPasses))){const unsigned f=materialOf(bits);if(f<FamilyCount)learnOwner(key,f,scene);}
    }
    const unsigned first=control.first.load(std::memory_order_relaxed),count=std::min(control.count.load(std::memory_order_relaxed),maxCategories);
    const int sup=control.suppress.load(std::memory_order_relaxed);
    const bool want=sup>=0&&unsigned(sup)<count&&first+unsigned(sup)<categoryCount&&matches(bits,categories[first+unsigned(sup)]);
    const bool registered=(bits&bit(ARegistered))!=0,dropIt=want&&registered&&t.queryDepth<=0;
    if(!t.owner){control.otherDraws.fetch_add(1,std::memory_order_relaxed);if(dropIt)control.otherDropped.fetch_add(1,std::memory_order_relaxed);return dropIt;}
    const auto tri=triangles(n,instances,meta);
    ++t.frameDraws;t.frameTris+=tri;if(!registered)++t.frameUnknown;if(want&&!dropIt)++t.frameProtected;
    for(unsigned k=0;k<count&&first+k<categoryCount;++k)if(matches(bits,categories[first+k])){++t.draws[k];t.tris[k]+=tri;if(bits&bit(SStereo))++t.right[k];}
    if(dropIt)++t.dropped[unsigned(sup)];
    if(t.techniques&&registered){
        if(const auto vs=metaVs(meta)){techniques.vsDraws[vs].fetch_add(1,std::memory_order_relaxed);techniques.vsTris[vs].fetch_add(tri,std::memory_order_relaxed);}
        if(const auto fs=metaFs(meta)){techniques.fsDraws[fs].fetch_add(1,std::memory_order_relaxed);techniques.fsTris[fs].fetch_add(tri,std::memory_order_relaxed);}}
    return dropIt;
}
// [armed] vkCmdBeginQuery +1 / vkCmdEndQuery -1 (TraceEntry observed*Query): nothing is dropped inside a query.
inline void queryEdge(int delta) noexcept {if(!armed())return;auto& t=local;t.queryDepth=std::max(0,t.queryDepth+delta);}
// Pass / stage scope: one relaxed load when off; LIFO restore. stage: b replaces the enclosing stage bit (stageBits), e.g. the
// stereo pass's light stage inside the left colour stage's scope.
class Scope {
    std::uint64_t saved_{};bool on_{};
public:
    explicit Scope(unsigned b,bool when=true,bool stage=false) noexcept {
        if(!when||!armed())return;auto& t=local;saved_=t.scope;t.scope=(stage?t.scope&~stageBits:t.scope)|bit(b);on_=true;}
    ~Scope(){if(on_)local.scope=saved_;}
    Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
};
// Eye midpoint / focal / units per metre of this RenderScene's eye plan (EyeViewports::plan(), as EyeCull uses it).
template<class Provider> void sceneCamera(Local& t,Provider provider) noexcept {
    const auto scene=sceneSerial.load(std::memory_order_relaxed);if(t.camScene==scene&&t.camValid)return;
    t.camScene=scene;t.camValid=false;
    const auto* v=provider();if(!v||!v->ready())return;const auto& p=v->plan();
    for(unsigned i=0;i<3;++i)t.cam[i]=0.5f*(p.world[0][12+i]+p.world[1][12+i]);
    t.focal=std::fabs(p.projection[0][5])*SmallObjectCull::eyeHeightPx*0.5f;
    t.upm=std::isfinite(p.unitsPerMetre)&&p.unitsPerMetre>0.f?p.unitsPerMetre:1.f;
    t.camValid=std::isfinite(t.cam[0])&&std::isfinite(t.cam[1])&&std::isfinite(t.cam[2])&&std::isfinite(t.focal)&&t.focal>0.f;
}
// renDrawBucketInfo (SmallObjectCull.h layout): +0x80 drawable, +0x90 grmMatrixSet, +0x98 crCreature, +0xac bits 13..15 kind.
template<class Provider> std::uint64_t objectBits(Local& t,const void* infoPointer,int instance,Provider provider) noexcept {
    std::uint64_t bits=bit(OObject)|(instance?bit(OInstance):0);
    const auto* info=static_cast<const unsigned char*>(infoPointer);if(!info)return bits;
    std::uintptr_t creature,matrixSet;std::uint32_t flags;const unsigned char* drawable;
    std::memcpy(&creature,info+0x98,sizeof creature);std::memcpy(&matrixSet,info+0x90,sizeof matrixSet);
    std::memcpy(&flags,info+0xac,4);std::memcpy(&drawable,info+0x80,sizeof drawable);
    bits|=creature?bit(OCreature):matrixSet?bit(OMatrixSet):bit(ORigid);
    if((flags>>13)&7u)return bits|bit(OKindOther);
    float c[3],r;if(!drawable||!SmallObjectCull::sphere(info,c,r))return bits;
    sceneCamera(t,provider);bits|=bit(ORad+bucketOf(r/t.upm,radEdges));
    if(!t.camValid)return bits|bit(ONoCamera);
    const float d[3]{c[0]-t.cam[0],c[1]-t.cam[1],c[2]-t.cam[2]};const float dist=std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
    bits|=bit(ODist+bucketOf(dist/t.upm,distEdges));
    return bits|bit(OPx+bucketOf(dist>r?2.f*r/dist*t.focal:INFINITY,pxEdges));
}
// bodyDraw (renDrawBucketContainer::DrawCommon), after the SMALL OBJECTS / EYE CULL skips: the object's bits for every draw it issues.
template<class Provider> class ObjectScope {
    std::uint64_t saved_{};std::uintptr_t savedDrawable_{};bool on_{};
public:
    ObjectScope(const void* info,int instance,Provider provider) noexcept {
        if(!armed())return;auto& t=local;saved_=t.scope;savedDrawable_=t.drawable;on_=true;
        t.scope=(t.scope&~objectMask)|objectBits(t,info,instance,provider);
        t.drawable=0;if(info)std::memcpy(&t.drawable,static_cast<const unsigned char*>(info)+0x80,sizeof t.drawable);
        if(!t.owner)return;++t.frameObjects;
        const unsigned first=control.first.load(std::memory_order_relaxed),count=std::min(control.count.load(std::memory_order_relaxed),maxCategories);
        for(unsigned k=0;k<count&&first+k<categoryCount;++k)if(matches(t.scope,categories[first+k]))++t.objects[k];
    }
    ~ObjectScope(){if(on_){local.scope=saved_;local.drawable=savedDrawable_;}}
    ObjectScope(const ObjectScope&)=delete;ObjectScope& operator=(const ObjectScope&)=delete;
};
template<class Provider> ObjectScope(const void*,int,Provider)->ObjectScope<Provider>;

// ---------------------------------------------------------------- window schedule (pure, host-tested)
class Scheduler {
public:
    struct Config {unsigned categories{},window{90},settle{30},minSamples{20};int pin{-1};bool cycle{};};
    struct Result {bool closed{},valid{},pair{};int phase{-1};double gpu{},interval{};unsigned samples{};
        int pairCategory{-1};double sup{},base{},supInterval{},baseInterval{};};
    void configure(const Config& c) noexcept {
        cfg_=c;if(cfg_.window<2)cfg_.window=2;if(cfg_.settle>=cfg_.window)cfg_.settle=cfg_.window-1;
        window_=0;frame_=0;acc_={};prevBase_={};pending_={};
    }
    const Config& config() const noexcept {return cfg_;}
    // B C0 B C1 ... B C(K-1) B C0 ...; pin: always the pin; count / no categories: always the baseline.
    int phaseOf(std::uint64_t w) const noexcept {
        if(cfg_.pin>=0)return cfg_.pin;if(!cfg_.cycle||!cfg_.categories)return -1;return (w&1u)?int((w/2)%cfg_.categories):-1;}
    int phase() const noexcept {return phaseOf(window_);}
    int upcoming() const noexcept {return frame_+1>=cfg_.window?phaseOf(window_+1):phase();} // the next scene's phase
    bool measuring() const noexcept {return frame_>=cfg_.settle;}
    std::uint64_t window() const noexcept {return window_;}
    unsigned frame() const noexcept {return frame_;}
    void addGpu(double ms) noexcept {if(measuring()){acc_.gpu+=ms;++acc_.gpuN;}}
    void addInterval(double ms) noexcept {if(measuring()){acc_.iv+=ms;++acc_.ivN;}}
    void hitch() noexcept {acc_.hitch=true;}
    // The current scene is over: close the window after `window` scenes; a valid baseline closes the pending category window
    // into a pair d = sup - (previous baseline + this baseline)/2.
    Result advance() noexcept {
        Result r{};if(++frame_<cfg_.window)return r;
        r.closed=true;r.phase=phase();r.samples=acc_.gpuN;r.valid=!acc_.hitch&&acc_.gpuN>=cfg_.minSamples;
        r.gpu=acc_.gpuN?acc_.gpu/acc_.gpuN:0.0;r.interval=acc_.ivN?acc_.iv/acc_.ivN:0.0;
        if(cfg_.cycle&&cfg_.pin<0){
            if(r.phase<0){
                if(r.valid&&prevBase_.valid&&pending_.valid){r.pair=true;r.pairCategory=pending_.phase;r.sup=pending_.gpu;r.supInterval=pending_.interval;
                    r.base=0.5*(prevBase_.gpu+r.gpu);r.baseInterval=0.5*(prevBase_.interval+r.interval);}
                prevBase_={r.valid,r.phase,r.gpu,r.interval};pending_={};
            }else pending_={r.valid,r.phase,r.gpu,r.interval};
        }
        ++window_;frame_=0;acc_={};return r;
    }
private:
    struct Acc {double gpu{},iv{};unsigned gpuN{},ivN{};bool hitch{};};
    struct Done {bool valid{};int phase{-1};double gpu{},interval{};};
    Config cfg_{};std::uint64_t window_{};unsigned frame_{};Acc acc_{};Done prevBase_{},pending_{};
};

// ---------------------------------------------------------------- cumulative statistics (render thread writes, report reads)
struct Sum {std::atomic<double> v{0.0};void add(double x) noexcept {v.store(v.load(std::memory_order_relaxed)+x,std::memory_order_relaxed);}
    double get() const noexcept {return v.load(std::memory_order_relaxed);}void clear() noexcept {v.store(0.0,std::memory_order_relaxed);}};
using Counter=std::atomic<std::uint64_t>;
inline void add(Counter& c,std::uint64_t v) noexcept {if(v)c.fetch_add(v,std::memory_order_relaxed);}
inline constexpr unsigned recentPairs=64,recentWindows=48;
struct CatStats {
    Counter pairs{},supWindows{},supValid{},supFrames{},supDropped{},supDraws{},baseDraws{},baseTris{},baseObjects{},baseRight{};
    Sum d,d2,sup,base,di,di2,supWindowGpu; // pairs: d = sup - base (GPU ms), di = interval delta; supWindowGpu = valid sup window means
    std::array<std::atomic<float>,recentPairs> recent{};Counter recentCount{};
    void clear() noexcept {
        for(auto* c:{&pairs,&supWindows,&supValid,&supFrames,&supDropped,&supDraws,&baseDraws,&baseTris,&baseObjects,&baseRight,&recentCount})c->store(0);
        for(auto* s:{&d,&d2,&sup,&base,&di,&di2,&supWindowGpu})s->clear();for(auto& x:recent)x.store(0.f);}
};
struct WindowLog {std::atomic<int> phase{};std::atomic<bool> valid{};std::atomic<float> gpu{},interval{};std::atomic<std::uint32_t> samples{};};
struct Stats {
    std::array<CatStats,maxCategories> cat{};
    Counter generation{},frames{},windows{},invalidWindows{},hitches{},spans{},spanOutliers{},detailFrames{},cycles{},
        baseFrames{},baseWindows{},baseValid{},baseDraws{},baseTris{},baseUnknown{},baseObjects{},protectedDraws{},propertyReads{},badProperty{},
        baseShared{},baseOwned{},baseOwnerUnknown{}; // Shared-technique object draws / with a non-model owner / no owner yet
    Sum baseGpu,baseInterval;
    std::array<WindowLog,recentWindows> log{};Counter logCount{};
    void clear() noexcept {
        for(auto& c:cat)c.clear();
        for(auto* c:{&frames,&windows,&invalidWindows,&hitches,&spans,&spanOutliers,&detailFrames,&cycles,&baseFrames,&baseWindows,&baseValid,
            &baseDraws,&baseTris,&baseUnknown,&baseObjects,&protectedDraws,&logCount,&baseShared,&baseOwned,&baseOwnerUnknown})c->store(0);
        baseGpu.clear();baseInterval.clear();
        for(auto& a:techniques.vsDraws)a.store(0);for(auto& a:techniques.vsTris)a.store(0);for(auto& a:techniques.fsDraws)a.store(0);for(auto& a:techniques.fsTris)a.store(0);}
};
inline Stats stats;

// ---------------------------------------------------------------- render thread runtime (sceneBegin)
struct Request {unsigned mode{Off},first{},count{};int pin{-1};bool valid{true};};
// "" / 0 / off; 1 pass, 2 family, 3 object, 4 all (or the names); count; pin:<category> (suppressed every scene: the picture);
// cycle:<category> (B C B C ... for that one category: many pairs fast). Anything else: off + badProperty.
inline Request parseRequest(const char* v) noexcept {
    Request r{};if(!v||!*v||!std::strcmp(v,"0")||!std::strcmp(v,"off"))return r;
    for(unsigned i=1;i<schemeCount;++i){char digit[4];std::snprintf(digit,sizeof digit,"%u",i);
        if(!std::strcmp(v,schemes[i].name)||!std::strcmp(v,digit)){r.mode=Cycle;r.first=schemes[i].first;r.count=schemes[i].count;return r;}}
    if(!std::strcmp(v,"count")){r.mode=Count;r.count=categoryCount;return r;}
    if(!std::strncmp(v,"pin:",4))for(unsigned k=0;k<categoryCount;++k)if(!std::strcmp(v+4,categories[k].name)){r.mode=Pin;r.count=categoryCount;r.pin=int(k);return r;}
    if(!std::strncmp(v,"cycle:",6))for(unsigned k=0;k<categoryCount;++k)if(!std::strcmp(v+6,categories[k].name)){r.mode=Cycle;r.first=k;r.count=1;return r;}
    r.valid=false;return r;
}
inline unsigned parseUnsigned(const char* v,unsigned fallback,unsigned lo,unsigned hi) noexcept {
    if(!v||!*v)return fallback;char* end{};const long n=std::strtol(v,&end,10);if(end==v||*end||n<long(lo)||n>long(hi))return fallback;return unsigned(n);}
struct Runtime {
    Scheduler sched;Request request{},kept{};char property[92]{"\x01"};unsigned window{90},settle{30},keptWindow{90},keptSettle{30};
    bool configured{},started{}; // kept*: the last run (report after switching off)
    std::uint64_t lastSerial{},lastNs{};int framePhase{-1};bool frameMeasuring{},frameDetail{};
};
inline Runtime rt;
inline std::array<const char*,3> testProperties{};inline bool testPropertiesSet{}; // host tests: ablate, window, settle
inline void readProperty(unsigned i,const char* name,char* out,std::size_t size) noexcept {
    out[0]=0;
    if(testPropertiesSet){if(testProperties[i])std::snprintf(out,size,"%s",testProperties[i]);return;}
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX]{};if(__system_property_get(name,v)>0)std::snprintf(out,size,"%s",v);
#else
    (void)name;
#endif
}
inline std::uint64_t nowNs() noexcept {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
inline void reconfigure(Runtime& g,const Request& q) noexcept {
    g.request=q;g.started=false;g.lastNs=0;g.framePhase=-1;g.frameMeasuring=false;g.lastSerial=stereo::gpuLiteSerial();
    control.armed.store(false,std::memory_order_relaxed);control.suppress.store(-1,std::memory_order_relaxed);control.upcoming.store(-1,std::memory_order_relaxed);
    // Off keeps the counters AND the run they belong to (kept: report's byCategory / scheme / pin) for the last snapshot.
    if(q.mode!=Off){stats.clear();stats.generation.fetch_add(1,std::memory_order_relaxed);g.kept=q;g.keptWindow=g.window;g.keptSettle=g.settle;owners.clear();}
    const unsigned measured=g.window>g.settle?g.window-g.settle:1u;
    g.sched.configure({q.count,g.window,g.settle,std::max(1u,measured/2),q.pin,q.mode==Cycle});
    control.first.store(q.first,std::memory_order_relaxed);control.count.store(q.count,std::memory_order_relaxed);
    control.mode.store(q.mode,std::memory_order_relaxed);control.epoch.fetch_add(1,std::memory_order_relaxed);
    control.armed.store(q.mode!=Off,std::memory_order_relaxed);
}
inline void refresh(Runtime& g) noexcept {
    char v[92],w[16]{},s[16]{};readProperty(0,"debug.rdrvr.ablate",v,sizeof v);stats.propertyReads.fetch_add(1,std::memory_order_relaxed);
    if(*v&&std::strcmp(v,"0")&&std::strcmp(v,"off")){readProperty(1,"debug.rdrvr.ablate.window",w,sizeof w);readProperty(2,"debug.rdrvr.ablate.settle",s,sizeof s);}
    const unsigned window=parseUnsigned(w,90,20,3600),settle=std::min(parseUnsigned(s,30,0,3600),window-10);
    if(g.configured&&!std::strcmp(v,g.property)&&window==g.window&&settle==g.settle)return;
    g.configured=true;std::snprintf(g.property,sizeof g.property,"%s",v);g.window=window;g.settle=settle;
    const auto q=parseRequest(v);if(!q.valid)stats.badProperty.fetch_add(1,std::memory_order_relaxed);
    reconfigure(g,q);
}
inline void closeWindow(const Scheduler::Result& r) noexcept {
    add(stats.windows,1);if(!r.valid)add(stats.invalidWindows,1);
    const auto n=stats.logCount.fetch_add(1,std::memory_order_relaxed);auto& l=stats.log[n%recentWindows];
    l.phase.store(r.phase);l.valid.store(r.valid);l.gpu.store(float(r.gpu));l.interval.store(float(r.interval));l.samples.store(r.samples);
    if(r.phase<0){add(stats.baseWindows,1);if(r.valid){add(stats.baseValid,1);stats.baseGpu.add(r.gpu);stats.baseInterval.add(r.interval);}}
    else if(unsigned(r.phase)<maxCategories){auto& c=stats.cat[unsigned(r.phase)];add(c.supWindows,1);if(r.valid){add(c.supValid,1);c.supWindowGpu.add(r.gpu);}
        if(rt.request.mode==Cycle&&unsigned(r.phase)+1==rt.request.count)add(stats.cycles,1);}
    if(r.pair&&r.pairCategory>=0&&unsigned(r.pairCategory)<maxCategories){auto& c=stats.cat[unsigned(r.pairCategory)];
        const double d=r.sup-r.base,di=r.supInterval-r.baseInterval;add(c.pairs,1);c.d.add(d);c.d2.add(d*d);c.sup.add(r.sup);c.base.add(r.base);
        c.di.add(di);c.di2.add(di*di);c.recent[c.recentCount.fetch_add(1,std::memory_order_relaxed)%recentPairs].store(float(d));}
}
// The scene that just ended: its counters (by its phase), the GPU spans read back since the last scene (frames ~4 behind: in the
// same window once past the settle), the scene-to-scene interval (> 250 ms = hitch: the window is not used).
inline void finishFrame(Runtime& g,Local& t) noexcept {
    const auto serial=stereo::gpuLiteSerial();
    if(serial>g.lastSerial){
        std::uint32_t us[stereo::gpuLiteWindowMax]{};
        const unsigned n=stereo::gpuLiteRing(us,unsigned(std::min<std::uint64_t>(serial-g.lastSerial,stereo::gpuLiteWindowMax)));
        for(unsigned i=0;i<n;++i){const double ms=us[i]/1000.0;add(stats.spans,1);if(!(ms>0.5&&ms<200.0)){add(stats.spanOutliers,1);continue;}if(g.started)g.sched.addGpu(ms);}
        g.lastSerial=serial;
    }
    const auto now=nowNs();
    if(g.started&&g.lastNs){const double iv=double(now-g.lastNs)/1e6;if(iv>250.0){g.sched.hitch();add(stats.hitches,1);}else g.sched.addInterval(iv);}
    g.lastNs=now;
    const unsigned count=std::min(g.request.count,maxCategories);
    if(g.started&&g.frameMeasuring){
        if(g.frameDetail)add(stats.detailFrames,1);
        if(g.framePhase<0){add(stats.baseFrames,1);add(stats.baseDraws,t.frameDraws);add(stats.baseTris,t.frameTris);add(stats.baseUnknown,t.frameUnknown);
            add(stats.baseObjects,t.frameObjects);add(stats.baseShared,t.frameShared);add(stats.baseOwned,t.frameOwned);add(stats.baseOwnerUnknown,t.frameOwnerUnknown);
            for(unsigned k=0;k<count;++k){auto& c=stats.cat[k];add(c.baseDraws,t.draws[k]);add(c.baseTris,t.tris[k]);add(c.baseObjects,t.objects[k]);add(c.baseRight,t.right[k]);}}
        else if(unsigned(g.framePhase)<count){auto& c=stats.cat[unsigned(g.framePhase)];add(c.supFrames,1);add(c.supDropped,t.dropped[unsigned(g.framePhase)]);
            add(c.supDraws,t.draws[unsigned(g.framePhase)]);}
        add(stats.protectedDraws,t.frameProtected);
    }
    for(unsigned k=0;k<count;++k){t.draws[k]=t.objects[k]=t.right[k]=t.dropped[k]=0;t.tris[k]=0;}
    t.frameDraws=t.frameTris=t.frameUnknown=t.frameProtected=t.frameObjects=t.frameShared=t.frameOwned=t.frameOwnerUnknown=0;
}
// TraceEntry observeScene (render thread, every RenderScene). detail = the DETAIL profiler's stage markers are armed.
inline void sceneBegin(bool detail) noexcept {
    const auto scene=sceneSerial.fetch_add(1,std::memory_order_relaxed)+1;auto& g=rt;
    if(!g.configured||(scene&63u)==0)refresh(g);
    if(g.request.mode==Off)return;
    auto& t=local;t.owner=true;t.queryDepth=0;
    finishFrame(g,t);
    if(g.started){const auto r=g.sched.advance();if(r.closed)closeWindow(r);}
    g.started=true;g.framePhase=g.sched.phase();g.frameMeasuring=g.sched.measuring();g.frameDetail=detail;add(stats.frames,1);
    control.suppress.store(g.request.mode==Count?-1:g.framePhase,std::memory_order_relaxed);
    control.upcoming.store(g.request.mode==Count?-1:g.sched.upcoming(),std::memory_order_relaxed);
    t.techniques=g.frameMeasuring&&g.framePhase<0;
}
// GameXr stereoGpuState (optional, before the _x item): byState keys _abb (baseline) / _ab<k> / _abp<k> (pin) / _abc (count) of
// the NEXT scene, so the DETAIL stage spans separate per category; nothing while off.
inline void appendSignature(char* sig,std::size_t size) noexcept {
    const unsigned mode=control.mode.load(std::memory_order_relaxed);if(mode==Off||!sig)return;
    const std::size_t used=std::strlen(sig);if(used+8>=size)return;const int k=control.upcoming.load(std::memory_order_relaxed);
    if(mode==Count)std::snprintf(sig+used,size-used,"_abc");else if(mode==Pin)std::snprintf(sig+used,size-used,"_abp%d",k);
    else if(k<0)std::snprintf(sig+used,size-used,"_abb");else std::snprintf(sig+used,size-used,"_ab%d",k);
}

// ---------------------------------------------------------------- report: graphics.ablation
inline std::string report() {
    std::ostringstream o;o.setf(std::ios::fixed);
    const auto u=[](const Counter& c){return c.load(std::memory_order_relaxed);};
    const auto num=[&o](double v,int p=3){if(!std::isfinite(v)){o<<"null";return;}o.precision(p);o<<v;};
    // q = the run the counters belong to: the active one, or after switching off the last one; mode = now.
    const auto& q=rt.request.mode!=Off?rt.request:rt.kept;const unsigned count=std::min(q.count,maxCategories);
    std::string scheme=q.mode==Cycle?(q.count==1&&q.first<categoryCount?std::string("cycle:")+categories[q.first].name:"?"):
        q.mode==Off?"off":q.mode==Pin?"pin":"count";
    for(unsigned i=1;i<schemeCount;++i)if(q.mode==Cycle&&schemes[i].first==q.first&&schemes[i].count==q.count)scheme=schemes[i].name;
    const double baseFrames=double(u(stats.baseFrames)),bv=double(u(stats.baseValid));
    o<<"\"ablation\":{\"mode\":\""<<modeNames[rt.request.mode&3u]<<"\",\"scheme\":\""<<scheme<<"\",\"property\":\""<<(rt.property[0]=='\x01'?"":rt.property)
     <<"\",\"generation\":"<<u(stats.generation)<<",\"armed\":"<<(armed()?1:0)<<",\"window\":"<<rt.keptWindow<<",\"settle\":"<<rt.keptSettle
     <<",\"pin\":"<<(q.pin>=0?std::string("\"")+categories[q.pin].name+"\"":std::string("null"))<<",\"phase\":"<<control.suppress.load()
     <<",\"scenes\":"<<sceneSerial.load()<<",\"frames\":"<<u(stats.frames)<<",\"windows\":"<<u(stats.windows)<<",\"invalidWindows\":"<<u(stats.invalidWindows)
     <<",\"cycles\":"<<u(stats.cycles)<<",\"hitches\":"<<u(stats.hitches)<<",\"spans\":"<<u(stats.spans)<<",\"spanOutliers\":"<<u(stats.spanOutliers)
     <<",\"detailFrames\":"<<u(stats.detailFrames)<<",\"propertyReads\":"<<u(stats.propertyReads)<<",\"badProperty\":"<<u(stats.badProperty)
     <<",\"baseFrames\":"<<u(stats.baseFrames)<<",\"baseWindows\":"<<u(stats.baseWindows)<<",\"baseValid\":"<<u(stats.baseValid)
     <<",\"baseGpuMs\":";num(bv?stats.baseGpu.get()/bv:NAN);o<<",\"baseIntervalMs\":";num(bv?stats.baseInterval.get()/bv:NAN);
    o<<",\"baseGpuSum\":";num(stats.baseGpu.get(),4);o<<",\"baseIntervalSum\":";num(stats.baseInterval.get(),4);
    o<<",\"baseDraws\":"<<u(stats.baseDraws)<<",\"baseTris\":"<<u(stats.baseTris)<<",\"baseUnknownDraws\":"<<u(stats.baseUnknown)<<",\"baseObjects\":"<<u(stats.baseObjects)
     <<",\"drawsPerFrame\":";num(baseFrames?double(u(stats.baseDraws))/baseFrames:NAN,1);o<<",\"trisPerFrame\":";num(baseFrames?double(u(stats.baseTris))/baseFrames:NAN,0);
    o<<",\"protectedDraws\":"<<u(stats.protectedDraws)<<",\"otherThreadDraws\":"<<control.otherDraws.load()<<",\"otherThreadDropped\":"<<control.otherDropped.load();
    // Generic ShadowBlend draws of DrawCommon objects per baseline frame, of which re-labelled to a non-model owner /
    // without a learned owner yet (stay "model"); owner table inserts / evictions (render thread counters, read racily)
    o<<",\"baseSharedDraws\":"<<u(stats.baseShared)<<",\"baseOwnedDraws\":"<<u(stats.baseOwned)<<",\"baseOwnerUnknownDraws\":"<<u(stats.baseOwnerUnknown)
     <<",\"sharedPerFrame\":";num(baseFrames?double(u(stats.baseShared))/baseFrames:NAN,1);o<<",\"ownedPerFrame\":";num(baseFrames?double(u(stats.baseOwned))/baseFrames:NAN,1);
    o<<",\"ownerUnknownPerFrame\":";num(baseFrames?double(u(stats.baseOwnerUnknown))/baseFrames:NAN,1);
    o<<",\"ownerInserts\":"<<owners.inserts<<",\"ownerEvictions\":"<<owners.evictions;
    { // registry
        o<<",\"registry\":{\"shaders\":"<<u(registry.shaders)<<",\"named\":"<<u(registry.named)<<",\"pipelines\":"<<u(registry.pipelines)
         <<",\"overflow\":"<<u(registry.overflow)<<",\"updates\":"<<u(registry.updates)<<",\"nameOverflow\":"<<u(registry.nameOverflow);
        for(const auto* which:{"shaderFamilies","pipelineFamilies"}){const auto& a=which[0]=='s'?registry.shaderFamilies:registry.pipelineFamilies;
            o<<",\""<<which<<"\":{";for(unsigned f=0;f<FamilyCount;++f)o<<(f?",":"")<<'"'<<familyNames[f]<<"\":"<<u(a[f]);o<<'}';}
        o<<'}';
    }
    o<<",\"byCategory\":{";
    for(unsigned k=0;k<count&&q.first+k<categoryCount;++k){
        const auto& c=stats.cat[k];const double p=double(u(c.pairs)),sf=double(u(c.supFrames)),sv=double(u(c.supValid));
        const double mean=p?c.d.get()/p:NAN,var=p>1?(c.d2.get()-p*mean*mean)/(p-1):NAN,se=p>1&&var>0?std::sqrt(var/p):NAN;
        const double imean=p?c.di.get()/p:NAN;
        std::array<float,recentPairs> rec{};const unsigned rn=unsigned(std::min<std::uint64_t>(u(c.recentCount),recentPairs));
        for(unsigned i=0;i<rn;++i)rec[i]=c.recent[i].load();std::sort(rec.begin(),rec.begin()+rn);
        const double median=rn?(rn&1u?rec[rn/2]:0.5*(rec[rn/2-1]+rec[rn/2])):NAN;
        o<<(k?",":"")<<'"'<<categories[q.first+k].name<<"\":{\"pairs\":"<<u(c.pairs)<<",\"gpuDeltaMs\":";num(mean);o<<",\"gpuDeltaSe\":";num(se);
        o<<",\"gpuDeltaMedianMs\":";num(median);o<<",\"gpuBaseMs\":";num(p?c.base.get()/p:NAN);o<<",\"gpuSupMs\":";num(p?c.sup.get()/p:NAN);
        o<<",\"intervalDeltaMs\":";num(imean);o<<",\"supWindowGpuMs\":";num(sv?c.supWindowGpu.get()/sv:NAN);
        o<<",\"drawsPerFrame\":";num(baseFrames?double(u(c.baseDraws))/baseFrames:NAN,1);o<<",\"trisPerFrame\":";num(baseFrames?double(u(c.baseTris))/baseFrames:NAN,0);
        o<<",\"objectsPerFrame\":";num(baseFrames?double(u(c.baseObjects))/baseFrames:NAN,1);o<<",\"rightNativeDrawsPerFrame\":";num(baseFrames?double(u(c.baseRight))/baseFrames:NAN,1);
        o<<",\"droppedPerFrame\":";num(sf?double(u(c.supDropped))/sf:NAN,1);o<<",\"supDrawsPerFrame\":";num(sf?double(u(c.supDraws))/sf:NAN,1);
        o<<",\"supWindows\":"<<u(c.supWindows)<<",\"supValid\":"<<u(c.supValid)<<",\"supFrames\":"<<u(c.supFrames)<<",\"sum\":{\"d\":";num(c.d.get(),4);
        o<<",\"d2\":";num(c.d2.get(),4);o<<",\"sup\":";num(c.sup.get(),4);o<<",\"base\":";num(c.base.get(),4);o<<",\"di\":";num(c.di.get(),4);
        o<<",\"di2\":";num(c.di2.get(),4);o<<",\"supWindowGpu\":";num(c.supWindowGpu.get(),4);
        o<<",\"baseDraws\":"<<u(c.baseDraws)<<",\"baseTris\":"<<u(c.baseTris)<<",\"baseObjects\":"<<u(c.baseObjects)<<",\"baseRight\":"<<u(c.baseRight)
         <<",\"supDropped\":"<<u(c.supDropped)<<",\"supDraws\":"<<u(c.supDraws)<<"}}";
    }
    o<<'}';
    { // techniques of the baseline measured frames (top VS by triangles, top FS by draws), cumulative
        std::vector<std::string> names;std::vector<std::uint64_t> hashes;std::vector<unsigned> families;
        try{std::lock_guard lock(registry.m);names=registry.names;hashes.assign(registry.nameHash.begin(),registry.nameHash.begin()+std::min<std::size_t>(names.size(),nameCapacity));
            for(std::size_t i=0;i<names.size()&&i<nameCapacity;++i)families.push_back(registry.nameFamily[i]);}catch(...){}
        const auto list=[&](const char* key,const std::array<Counter,nameCapacity>& draws,const std::array<Counter,nameCapacity>& tris,bool byTris,unsigned top){
            std::vector<unsigned> idx;for(unsigned i=1;i<names.size()&&i<nameCapacity;++i)if(u(draws[i]))idx.push_back(i);
            std::sort(idx.begin(),idx.end(),[&](unsigned a,unsigned b){return byTris?u(tris[a])>u(tris[b]):u(draws[a])>u(draws[b]);});
            if(idx.size()>top)idx.resize(top);
            o<<",\""<<key<<"\":[";
            for(std::size_t j=0;j<idx.size();++j){const unsigned i=idx[j];char h[20];std::snprintf(h,sizeof h,"%llx",static_cast<unsigned long long>(hashes[i]));
                o<<(j?",":"")<<"{\"name\":\""<<names[i]<<"\",\"family\":\""<<familyNames[families[i]<FamilyCount?families[i]:0]<<"\",\"fnv\":\""<<h
                 <<"\",\"draws\":"<<u(draws[i])<<",\"tris\":"<<u(tris[i])<<",\"drawsPerFrame\":";num(baseFrames?double(u(draws[i]))/baseFrames:NAN,1);
                o<<",\"trisPerFrame\":";num(baseFrames?double(u(tris[i]))/baseFrames:NAN,0);o<<'}';}
            o<<']';};
        list("techniquesVs",techniques.vsDraws,techniques.vsTris,true,40);list("techniquesFs",techniques.fsDraws,techniques.fsTris,false,24);
    }
    o<<",\"recentWindows\":[";
    {const auto n=u(stats.logCount);const auto m=std::min<std::uint64_t>(n,recentWindows);
        for(std::uint64_t j=0;j<m;++j){const auto& l=stats.log[(n-m+j)%recentWindows];const int ph=l.phase.load();
            o<<(j?",":"")<<"{\"phase\":\""<<(ph<0?"base":ph<int(count)&&q.first+unsigned(ph)<categoryCount?categories[q.first+unsigned(ph)].name:"?")
             <<"\",\"valid\":"<<(l.valid.load()?1:0)<<",\"gpuMs\":";num(l.gpu.load(),2);o<<",\"intervalMs\":";num(l.interval.load(),2);o<<",\"n\":"<<l.samples.load()<<'}';}}
    o<<"],\"notes\":\"GPU = engine frame command buffer span (both eyes); d = sup - mean(baseline before, after); draws/tris/objects = "
       "left native + right-eye native stages per baseline frame (replayed right-eye draws mirror the left tape); indirect draws not covered; "
       "family rows: a generic ShadowBlend depth draw of an object counts for the family of the same geometry's colour draw (owned/ownerUnknown), "
       "techniques are rows per (name, module family); categories overlap, deltas do not add up\"}";
    return o.str();
}
// Host tests.
inline void resetForTests() noexcept {
    stats.clear();rt=Runtime{};control.armed.store(false);control.suppress.store(-1);control.upcoming.store(-1);control.first.store(0);
    control.count.store(0);control.mode.store(Off);control.otherDraws.store(0);control.otherDropped.store(0);sceneSerial.store(0);local=Local{};
    owners.clear();
}
} // namespace rdrvr::engine::ablate
