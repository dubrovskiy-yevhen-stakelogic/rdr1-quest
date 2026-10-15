#pragma once
// Vulkan-free part of the per-eye TAA (host-testable). Port of the CPU side of the GTA5 Quest TAA:
// per-eye history views, reset rules and the push constants of the two passes. The GTA5 view
// transform (tangents + rotation + translation, reversed Z) is replaced by one 4x4:
// R = (F P_prev) * inverse(world_prev) * world_cur * inverse(F P_cur) maps this eye's Vulkan NDC
// (x, y, depth) homogeneously onto the previous frame's clip of the same eye, with the exact plan
// matrices both frames were drawn with (world_e = plan.world[e], P_e = plan.projection[e], F = Y flip
// of the GL-style engine projection).
// The relative camera inverse(world_prev) * world_cur is formed in double so the engine's large world
// coordinates cancel.
#include "HandsMath.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace rdrvr::vr::taa {
using hands::DMat4;

// General 4x4 inverse (column-major, double). false = singular / not finite.
inline bool inverse(const DMat4& a,DMat4& out) noexcept {
    const auto& m=a.m;std::array<double,16> inv{};
    inv[0]=m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
    inv[4]=-m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
    inv[8]=m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
    inv[12]=-m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
    inv[1]=-m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
    inv[5]=m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
    inv[9]=-m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
    inv[13]=m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
    inv[2]=m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
    inv[6]=-m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
    inv[10]=m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
    inv[14]=-m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
    inv[3]=-m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
    inv[7]=m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
    inv[11]=-m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
    inv[15]=m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
    const double det=m[0]*inv[0]+m[1]*inv[4]+m[2]*inv[8]+m[3]*inv[12];
    if(!std::isfinite(det)||std::abs(det)<1e-300)return false;
    for(unsigned i=0;i<16;++i){out.m[i]=inv[i]/det;if(!std::isfinite(out.m[i]))return false;}
    return true;
}

// Tuning (GTA5 cvars, RDR defaults). Properties debug.rdrvr.vr.taa.* override (TaaPass.h).
struct Tuning {
    float clipGamma{1.25f};      // YCoCg clip half-width (sigma)                         gta5_taa_clip
    float nMaxStill{10.f};       // max history weight                                     gta5_taa_nmax_still
    float nMaxMoving{5.f};       //                                                        gta5_taa_nmax_moving
    float motionPx{6.f};         // reprojection distance at which nMaxMoving applies     gta5_taa_motion_px
    float nearMetres{1.0f};      // near field (b = 0): hands / weapon / arms             gta5_taa_near_m (GTA5 on foot 0.8)
    float depthTestMetres{60.f}; // one-sided depth test range (0 = off)                  gta5_taa_depth_test_m
    float sharpen{0.5f};         // sharpen amount (TAA FULL)                             gta5_taa_sharpen
    float cutMetres{0.5f};       // camera cut per frame (SpaceWarpMath cutMetresDefault) gta5_taa_max_speed 2.5 m/swap
    float cutDegrees{5.f};       // camera cut per frame (SpaceWarpMath cutDegreesDefault) gta5_taa_max_turn_deg 25
    std::uint32_t maxGap{2};     // render frames between history and now (1 = consecutive) gta5_taa_max_age 4 swaps
    std::uint32_t taps{5};       // RDR cost lever: 5 = cross moments (default), 9 = the GTA5 3x3
    float area{1.f};             // RDR cost lever: centred fraction of width/height resolved (1 = whole eye)
    std::uint32_t debug{};       // bit 0 reset every frame, 1 pure TAA (b = 1), 2 apply pass also on reset frames,
                                 // 3 sharpen also in plain TAA
    float jitterAmplitude{1.f};  // TAA FULL: Halton offset scale (1 = +-0.5 px; 0 = no jitter)             debug.rdrvr.vr.taa.jitter %
    std::uint32_t jitterLength{8};// Halton(2,3) positions before wrapping                                  debug.rdrvr.vr.taa.jitterlen
    // TAA LITE (taa_lite.frag): centred area resolved (the periphery stays raw), history filter, depth reprojection.
    float liteArea{.7f};         // fraction of width / height (0.3..1)                                     debug.rdrvr.vr.taa.lite.area %
    bool liteCubic{true};        // history: false = 1 bilinear tap, true = 5-tap Catmull-Rom (as TAA)      debug.rdrvr.vr.taa.lite.cubic
    float liteSubpx{1.f};        // sub-pixel-aware weight cap k (0 = off)                                  debug.rdrvr.vr.taa.lite.subpx %
    bool liteDepth{true};        // false = rotation-only reprojection (depth = far plane, no near field)   debug.rdrvr.vr.taa.lite.depth
    bool liteFp16{false};        // true = the fp16 resolve pipeline (taa_lite.frag kHalf)                  debug.rdrvr.vr.taa.lite.fp16
};
inline constexpr std::uint32_t debugReset=1,debugPure=2,debugApplyOnReset=4,debugSharpenTaa=8;
// Shader flags (taa_resolve.frag / taa_apply.frag). flagFull = TAA FULL (jittered input, un-jittered
// output, no discard), flagLite = TAA LITE (taa_lite.frag resolve; the apply pass is a plain copy of
// its output), flagLiteCubic = Catmull-Rom history, flagLiteNoDepth = rotation-only reprojection.
inline constexpr std::uint32_t flagReset=1,flagPure=2,flagUnorm=4,flagSharpen=8,flagTaps9=16,flagFull=32,flagLite=64,flagLiteCubic=128,flagLiteNoDepth=256;
// TAA LITE history alpha = weight / liteWeightScale (8-bit UNORM alpha of the eye format; weight <= liteMaxWeight).
inline constexpr float liteWeightScale=16.f,liteMaxWeight=15.f;
// taa_lite.frag `layout(constant_id=0) const bool kHalf` (VkBool32): the fp16 resolve variant of the LITE pipeline.
inline constexpr std::uint32_t liteHalfSpecId=0;

enum class Reset : unsigned {None,First,Resize,Gap,Cut,Toggle,Debug,Matrix,Count};
inline constexpr const char* resetNames[unsigned(Reset::Count)]{"none","first","resize","gap","cut","toggle","debug","matrix"};

// The history of one eye: what the frame that wrote it was drawn with.
struct EyeView {
    bool valid{};
    std::uint64_t frame{};      // render frame (FrameContext.renderFrameId) that wrote the history
    DMat4 world{};              // plan.world[e]: eye camera -> engine world
    DMat4 clip{};               // F * plan.projection[e]
    std::uint32_t width{},height{};
    std::int32_t format{};      // eye colour VkFormat
    std::uint32_t parity{};     // history slot written by that frame
    std::uint32_t margins{};    // resolved area: margin x | margin y << 16 (0 = whole eye)
};
// Centred resolved area of a width x height eye for the area fraction (packed margins, 0 = whole eye).
inline std::uint32_t areaMargins(std::uint32_t width,std::uint32_t height,float area) noexcept {
    if(!(area<1.f))return 0;
    const double a=std::clamp(double(area),0.3,1.0);
    const auto mx=std::uint32_t(std::lround(width*(1-a)*0.5)),my=std::uint32_t(std::lround(height*(1-a)*0.5));
    return std::min<std::uint32_t>(mx,0xFFFF)|(std::min<std::uint32_t>(my,0xFFFF)<<16);
}

// Camera motion between two eye cameras: translation (metres) and rotation angle (degrees). Columns are normalised so a
// uniform world scale cannot fake a rotation.
struct Motion {double metres{},degrees{};};
inline Motion cameraMotion(const DMat4& previous,const DMat4& current,double unitsPerMetre) noexcept {
    Motion r{};
    const double dx=current.m[12]-previous.m[12],dy=current.m[13]-previous.m[13],dz=current.m[14]-previous.m[14];
    r.metres=std::sqrt(dx*dx+dy*dy+dz*dz)/(unitsPerMetre>0?unitsPerMetre:1.0);
    double trace=0;
    for(int c=0;c<3;++c){
        double na=0,nb=0,d=0;
        for(int i=0;i<3;++i){na+=previous.m[c*4+i]*previous.m[c*4+i];nb+=current.m[c*4+i]*current.m[c*4+i];d+=previous.m[c*4+i]*current.m[c*4+i];}
        trace+=d/std::sqrt(std::max(na*nb,1e-300));
    }
    r.degrees=std::acos(std::clamp((trace-1.0)*0.5,-1.0,1.0))*57.29577951308232;
    if(!std::isfinite(r.metres))r.metres=1e30;
    if(!std::isfinite(r.degrees))r.degrees=180;
    return r;
}

// GTA5 reset rules for one eye (toggle / matrix failures are decided by the caller).
inline Reset resetReason(const EyeView& v,std::uint64_t frame,const DMat4& world,std::uint32_t width,std::uint32_t height,std::int32_t format,
                         const Tuning& t,double unitsPerMetre,Motion* motion=nullptr,std::uint32_t margins=0) noexcept {
    if(!v.valid)return Reset::First;
    if(v.width!=width||v.height!=height||v.format!=format||v.margins!=margins)return Reset::Resize;
    if(frame<=v.frame||frame-v.frame>std::max<std::uint32_t>(t.maxGap,1))return Reset::Gap;
    const auto m=cameraMotion(v.world,world,unitsPerMetre);if(motion)*motion=m;
    if(!(m.metres<=double(t.cutMetres))||!(m.degrees<=double(t.cutDegrees)))return Reset::Cut;
    if(t.debug&debugReset)return Reset::Debug;
    return Reset::None;
}

// Push constants of both passes (taa_resolve.frag / taa_apply.frag `Push`): exactly 128 bytes.
struct Push {
    float reproject[16]{};
    float winv[4]{};
    float tune[4]{};
    float range[3]{};
    std::uint32_t jitter{}; // packJitter of the eye's projection jitter (framebuffer px)
    std::uint32_t info[4]{};
};
// Framebuffer-pixel jitter as int16 x | int16 y << 16 in 1/8192 px (taa_resolve.frag unpacks; |j| < 4 px).
inline std::uint32_t packJitter(float x,float y) noexcept {
    const auto q=[](float v){const double c=std::clamp(double(std::isfinite(v)?v:0.f)*8192.0,-32767.0,32767.0);
        return std::uint32_t(std::uint16_t(std::int16_t(std::lround(c))));};
    return q(x)|(q(y)<<16);
}
static_assert(sizeof(Push)==128);

// F * P (Vulkan y-down clip for a positive-height viewport; hands::vulkanProjection).
inline DMat4 eyeClip(const runtime::Mat4& projection) noexcept {return hands::vulkanProjection(projection);}

// Fills the push constants. previous == nullptr (reset): the reprojection is unused (identity). false = singular
// matrices (the caller then resets the eye with Reset::Matrix and records nothing for the history read).
inline bool buildPush(const EyeView* previous,const DMat4& world,const DMat4& clip,double unitsPerMetre,std::uint32_t width,std::uint32_t height,
                      std::uint32_t flags,const Tuning& t,Push& out,std::uint32_t margins=0,std::uint32_t jitter=0) noexcept {
    out=Push{};
    DMat4 clipInverse{};if(!inverse(clip,clipInverse))return false;
    DMat4 r=hands::identity();
    if(previous){
        DMat4 previousWorldInverse{};if(!inverse(previous->world,previousWorldInverse))return false;
        const DMat4 relative=hands::multiply(previousWorldInverse,world); // current eye camera -> previous eye camera
        r=hands::multiply(hands::multiply(previous->clip,relative),clipInverse);
    }
    for(unsigned i=0;i<16;++i){out.reproject[i]=float(r.m[i]);if(!std::isfinite(out.reproject[i]))return false;}
    for(int c=0;c<4;++c)out.winv[c]=float(clipInverse.m[c*4+3]); // row 3: inverse view depth of (ndc, 1)
    const double upm=unitsPerMetre>0?unitsPerMetre:1.0;
    out.tune[0]=t.clipGamma;out.tune[1]=t.nMaxStill;out.tune[2]=t.nMaxMoving;out.tune[3]=t.motionPx>0?1.f/t.motionPx:0.f;
    out.range[0]=t.nearMetres>0?float(1.0/(double(t.nearMetres)*upm)):1e30f; // 1e30: no near field
    out.range[1]=t.depthTestMetres>0?float(1.0/(double(t.depthTestMetres)*upm)):0.f;
    out.range[2]=t.sharpen;out.jitter=jitter;
    out.info[0]=width;out.info[1]=height;out.info[2]=flags;out.info[3]=margins;
    return true;
}
} // namespace rdrvr::vr::taa
