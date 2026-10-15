#pragma once
// AppSW: host-testable math for XR_FB_space_warp motion vectors.
//
// Matrices are 4x4 column-major (m[c*4+r]), the layout of the engine's relative clip
// matrices (FrameMaps::relClip, EyeViewports::cameraMatrices) and of runtime::Mat4:
//   clip = relClip * (p - c)        p = world position, c = eye position (engine units)
// Reprojection of a current-frame NDC point into the previous frame's clip space:
//   R = relClipPrev * T(cCur - cPrev) * inverse(relClipCur)        (same form as eyeCorrection)
// Engine NDC (from the replay's matrices): x right, y UP (P[5] > 0, the image is
// rendered upright through a flipped viewport), z = stored depth in [0,1] (forward Z,
// near -> 0, far -> 1; relClip row2 = (f/(f-n)) * row3 - f*n/(f-n)).
// The camera-only motion vector of a pixel is cur - prev in NDC; every convention the
// compositor may expect is a parameter (see Conventions) so it can be flipped on device.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace rdrvr::runtime::spacewarp {
using M4=std::array<double,16>;
inline M4 identity() noexcept {M4 m{};m[0]=m[5]=m[10]=m[15]=1;return m;}
inline M4 mul(const M4& a,const M4& b) noexcept {
    M4 m{};
    for(int c=0;c<4;++c)for(int r=0;r<4;++r){double s=0;for(int k=0;k<4;++k)s+=a[k*4+r]*b[c*4+k];m[c*4+r]=s;}
    return m;
}
inline M4 fromFloat(const float* f) noexcept {M4 m{};for(int i=0;i<16;++i)m[i]=f[i];return m;}
inline bool invert(const M4& in,M4& out) noexcept {
    // Gauss-Jordan with partial pivoting on [A | I] (row-major scratch of the column-major input).
    double a[4][8]{};
    for(int r=0;r<4;++r){for(int c=0;c<4;++c)a[r][c]=in[c*4+r];a[r][4+r]=1;}
    for(int col=0;col<4;++col){
        int pivot=col;for(int r=col+1;r<4;++r)if(std::fabs(a[r][col])>std::fabs(a[pivot][col]))pivot=r;
        if(!(std::fabs(a[pivot][col])>1e-300))return false;
        if(pivot!=col)for(int k=0;k<8;++k)std::swap(a[pivot][k],a[col][k]);
        const double d=a[col][col];for(int k=0;k<8;++k)a[col][k]/=d;
        for(int r=0;r<4;++r)if(r!=col){const double f=a[r][col];if(f!=0)for(int k=0;k<8;++k)a[r][k]-=f*a[col][k];}
    }
    for(int r=0;r<4;++r)for(int c=0;c<4;++c)out[c*4+r]=a[r][4+c];
    for(double v:out)if(!std::isfinite(v))return false;
    return true;
}
inline M4 translation(double x,double y,double z) noexcept {M4 m=identity();m[12]=x;m[13]=y;m[14]=z;return m;}
// R = relPrev * T(posCur - posPrev) * inverse(relCur), single precision result.
inline bool reprojection(const float* relCur,const float* posCur,const float* relPrev,const float* posPrev,std::array<float,16>& out) noexcept {
    if(!relCur||!posCur||!relPrev||!posPrev)return false;
    for(int i=0;i<16;++i)if(!std::isfinite(relCur[i])||!std::isfinite(relPrev[i]))return false;
    for(int i=0;i<3;++i)if(!std::isfinite(posCur[i])||!std::isfinite(posPrev[i]))return false;
    M4 inv{};if(!invert(fromFloat(relCur),inv))return false;
    const M4 r=mul(mul(fromFloat(relPrev),translation(double(posCur[0])-posPrev[0],double(posCur[1])-posPrev[1],double(posCur[2])-posPrev[2])),inv);
    for(int i=0;i<16;++i){if(!std::isfinite(r[i])||std::fabs(r[i])>3e38)return false;out[i]=float(r[i]);}
    return true;
}
// Device-side conventions (push constants of the motion-vector pass, see spacewarp.frag).
struct Conventions {
    float engineY{-1};  // +1: engine NDC y points DOWN the image (Vulkan-native); -1: engine NDC y points UP
    float mvY{1};       // output y: +1 = Vulkan NDC (down = +), -1 = GL NDC (up = +)
    float mvSign{1};    // +1 = current - previous (Meta XrSpaceWarp sample), -1 = previous - current
    float mvZ{1};       // 0 = no z, 1 = delta of stored depth (Vulkan NDC z), 2 = GL NDC z (2x)
};
// CPU reference of the fragment shader. u,v in [0,1] with v = 0 at the TOP image row;
// depth = stored depth value. Returns the motion vector (x, y, z); w is always 0.
inline std::array<float,3> motionVector(const std::array<float,16>& m,float u,float v,float depth,const Conventions& c) noexcept {
    const float ix=u*2-1,iy=v*2-1;                           // image NDC: x right, y down
    const float cur[4]{ix,iy*c.engineY,depth,1};            // engine NDC
    float prev[4]{};
    for(int r=0;r<4;++r){float s=0;for(int k=0;k<4;++k)s+=m[k*4+r]*cur[k];prev[r]=s;}
    if(!(prev[3]>1e-6f))return {0,0,0};
    float d[3]{cur[0]-prev[0]/prev[3],cur[1]-prev[1]/prev[3],cur[2]-prev[2]/prev[3]};
    d[1]*=c.engineY;                                        // back to image-down NDC
    for(float& x:d)x*=c.mvSign;
    d[1]*=c.mvY;d[2]*=c.mvZ;
    return {d[0],d[1],d[2]};
}
// Engine depth range from a relative clip matrix (forward Z, [0,1]): row2 = A*row3 + (0,0,0,B)
// with A = f/(f-n), B = -f*n/(f-n). Returns false for non-perspective input.
inline bool depthRange(const float* rel,double& nearOut,double& farOut) noexcept {
    // w row = (m[3],m[7],m[11],m[15]) with m[15]==0; z row = (m[2],m[6],m[10],m[14]).
    const double w2=double(rel[3])*rel[3]+double(rel[7])*rel[7]+double(rel[11])*rel[11];
    if(!(w2>1e-12)||std::fabs(rel[15])>1e-6)return false;
    const double a=(double(rel[2])*rel[3]+double(rel[6])*rel[7]+double(rel[10])*rel[11])/w2;
    const double b=rel[14];
    if(!(a>1)||!(b<0))return false;
    const double n=-b/a,f=b/(1-a); // B/A = -n, A(f-n) = f  =>  f = B/(1-A)
    if(!(n>0)||!(f>n)||!std::isfinite(f))return false;
    nearOut=n;farOut=f;return true;
}
// World-from-LOCAL transform (engine units). eyeWorld = plan.world[i] (column-major, translation
// at [12..14]); the eye's LOCAL pose (OpenXR quaternion xyzw, metres). Inverse of
// E = [R(q) | p * unitsPerMetre] applied on the right: W = eyeWorld * inverse(E).
inline M4 rotation(const double q[4]) noexcept {
    const double x=q[0],y=q[1],z=q[2],w=q[3];
    M4 m=identity();
    m[0]=1-2*(y*y+z*z);m[1]=2*(x*y+z*w);m[2]=2*(x*z-y*w);
    m[4]=2*(x*y-z*w);m[5]=1-2*(x*x+z*z);m[6]=2*(y*z+x*w);
    m[8]=2*(x*z+y*w);m[9]=2*(y*z-x*w);m[10]=1-2*(x*x+y*y);
    return m;
}
inline M4 worldFromLocal(const float* eyeWorld,const float q[4],const float p[3],double unitsPerMetre) noexcept {
    double n=0;double qd[4];for(int i=0;i<4;++i){qd[i]=q[i];n+=qd[i]*qd[i];}
    n=std::sqrt(n);if(n>0)for(double& v:qd)v/=n;
    M4 e=rotation(qd);e[12]=p[0]*unitsPerMetre;e[13]=p[1]*unitsPerMetre;e[14]=p[2]*unitsPerMetre;
    // rigid inverse of e
    M4 inv=identity();
    for(int r=0;r<3;++r)for(int c=0;c<3;++c)inv[c*4+r]=e[r*4+c];
    for(int r=0;r<3;++r)inv[12+r]=-(inv[0*4+r]*e[12]+inv[1*4+r]*e[13]+inv[2*4+r]*e[14]);
    return mul(fromFloat(eyeWorld),inv);
}
struct PoseD {double q[4]{0,0,0,1};double p[3]{};};
inline void quaternionFrom(const M4& m,double q[4]) noexcept {
    // column-major rotation part; m[c*4+r]
    const double r00=m[0],r11=m[5],r22=m[10],r10=m[1],r01=m[4],r20=m[2],r02=m[8],r21=m[6],r12=m[9];
    const double t=r00+r11+r22;
    if(t>0){const double s=std::sqrt(t+1)*2;q[3]=0.25*s;q[0]=(r21-r12)/s;q[1]=(r02-r20)/s;q[2]=(r10-r01)/s;}
    else if(r00>r11&&r00>r22){const double s=std::sqrt(1+r00-r11-r22)*2;q[3]=(r21-r12)/s;q[0]=0.25*s;q[1]=(r01+r10)/s;q[2]=(r02+r20)/s;}
    else if(r11>r22){const double s=std::sqrt(1+r11-r00-r22)*2;q[3]=(r02-r20)/s;q[0]=(r01+r10)/s;q[1]=0.25*s;q[2]=(r12+r21)/s;}
    else{const double s=std::sqrt(1+r22-r00-r11)*2;q[3]=(r10-r01)/s;q[0]=(r02+r20)/s;q[1]=(r12+r21)/s;q[2]=0.25*s;}
    double n=std::sqrt(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);if(n>0)for(int i=0;i<4;++i)q[i]/=n;
    if(q[3]<0)for(int i=0;i<4;++i)q[i]=-q[i];
}
// appSpaceDeltaPose = inverse(prev) * cur (pose of the current app space in the previous one),
// translation in metres. Rotation is orthonormalized (the stock camera is validated to 0.5 %).
inline bool deltaPose(const M4& prev,const M4& cur,double unitsPerMetre,PoseD& out) noexcept {
    if(!(unitsPerMetre>0))return false;
    M4 inv{};if(!invert(prev,inv))return false;
    const M4 d=mul(inv,cur);
    for(double v:d)if(!std::isfinite(v))return false;
    quaternionFrom(d,out.q);
    for(int i=0;i<3;++i)out.p[i]=d[12+i]/unitsPerMetre;
    return true;
}
inline double rotationDegrees(const PoseD& p) noexcept {
    const double w=std::fabs(p.q[3]);return w>=1?0:2*std::acos(w)*57.29577951308232;
}
inline double translationMetres(const PoseD& p) noexcept {return std::sqrt(p.p[0]*p.p[0]+p.p[1]*p.p[1]+p.p[2]*p.p[2]);}

// ----------------------------------------------------------------------------------------------------
// SPACEWARP menu variants (graphics.cfg spaceWarp, debug.rdrvr.spacewarp overrides). One variable each;
// every variant keeps the defaults otherwise (MV = cur - prev NDC, head motion included, per-eye matrices,
// forward Z with nearZ/farZ in metres, deltaPose = inv(prev) * cur, colour/depth/MV paired by slot):
//   1 ON           all defaults
//   2 ON ZERO MV   the motion-vector pass gets an identity reprojection (delta = 0); depth/deltaPose normal
//   3 ON FAR DEPTH layer nearZ = 1e6, farZ = +inf (legal: both positive, nearZ != farZ): positional
//                  timewarp becomes rotation-only (isolates the depth / nearZ / farZ inputs)
//   4 ON INVERT    mvSign -1 (prev - cur)
//   5 ON FLIP Y    mvY -1 (GL NDC, up = +)
//   6 ON CAM ONLY  camera-only vectors (no per-actor motion)
inline constexpr unsigned variantCount=7;
inline const char* variantName(unsigned m) noexcept {
    switch(m){case 1:return "ON";case 2:return "ON ZERO MV";case 3:return "ON FAR DEPTH";case 4:return "ON INVERT";case 5:return "ON FLIP Y";case 6:return "ON CAM ONLY";default:return "OFF";}
}
// Per-actor motion vectors in every ON variant except ZERO MV and CAM ONLY.
inline bool variantActors(unsigned m) noexcept {return m!=0&&m!=2&&m!=6;}
inline bool variantZeroMotion(unsigned m) noexcept {return m==2;}
inline bool variantFarDepth(unsigned m) noexcept {return m==3;}
inline float variantSign(unsigned m) noexcept {return m==4?-1.f:1.f;}
inline float variantY(unsigned m) noexcept {return m==5?-1.f:1.f;}
inline std::array<float,16> identityReprojection() noexcept {std::array<float,16> m{};m[0]=m[5]=m[10]=m[15]=1;return m;}
// Reprojection pushed to the motion-vector pass for a slot (ZERO MV: identity).
inline std::array<float,16> pushedReprojection(unsigned variant,const std::array<float,16>& r) noexcept {
    return variantZeroMotion(variant)?identityReprojection():r;
}
// Depth parameters of XrCompositionLayerSpaceWarpInfoFB. depthMode = debug.rdrvr.spacewarp.depth (0 forward,
// 1 farZ = +inf, 2 reversed); FAR DEPTH overrides it.
struct LayerDepth {float minDepth{0},maxDepth{1},nearZ{},farZ{};};
inline LayerDepth layerDepth(unsigned variant,int depthMode,float nearZ,float farZ) noexcept {
    LayerDepth out{};
    if(variantFarDepth(variant)){out.nearZ=1e6f;out.farZ=std::numeric_limits<float>::infinity();return out;}
    if(depthMode==2){out.nearZ=farZ;out.farZ=nearZ;}
    else{out.nearZ=nearZ;out.farZ=depthMode==1?std::numeric_limits<float>::infinity():farZ;}
    return out;
}
// History rule: vectors only when exactly one rendered frame separates the pair from the previous produced pair
// (renderFrameId + 1); any gap (rejected world frames, a suspension) takes the no-history / FRAME_SKIP path.
inline bool consecutiveFrames(std::uint64_t previous,std::uint64_t current) noexcept {return current==previous+1;}
// Camera cut / teleport / snap turn (30 deg): no extrapolation from this pair. Well above per-pair locomotion
// (p95 per pair: 46 mm translation, 0.04 deg rotation).
inline constexpr double cutMetresDefault=0.5,cutDegreesDefault=5.0;
inline bool cameraCut(const PoseD& d,double metres=cutMetresDefault,double degrees=cutDegreesDefault) noexcept {
    return translationMetres(d)>metres||rotationDegrees(d)>degrees;
}
// CPU-side motion-vector magnitude (x, y NDC) at the screen centre of one eye: the report's camera-motion probe.
inline double centreMotion(const std::array<float,16>& reproject,const Conventions& c) noexcept {
    const auto mv=motionVector(reproject,0.5f,0.5f,0.999f,c);
    return std::sqrt(double(mv[0])*mv[0]+double(mv[1])*mv[1]);
}
// AppSW budget guard (XR thread). AppSW only pays off when nearly every XR frame of the runtime's half-rate loop
// gets a new pair: a re-submitted pair costs a whole slot (27.8 ms) plus a backward content step (FRAME_SKIP).
// More than maxRepeats re-submits among the last `window` counted XR frames suspend the space-warp info for a
// back-off period: 5 s, doubled (max 40 s) while a trip follows within calmNs of the previous resume, back to 5 s
// after a calm period; reset() on a mode change. Only XR frames inside the runtime's half-rate loop are counted
// (the 72 <-> 36 Hz transitions after an enable/resume would trip it: LOCK 36 at 72 Hz repeats every pair).
struct CadenceGuard {
    static constexpr unsigned window=72,maxRepeats=7;
    static constexpr std::uint64_t baseNs=5'000'000'000ull,maxNs=40'000'000'000ull,calmNs=30'000'000'000ull;
    std::array<std::uint8_t,window> bits{};unsigned next{},repeats{},samples{};
    std::uint64_t until{},backoff{baseNs},trips{},counted{},lastSuspendNs{};
    bool suspended(std::uint64_t now) const noexcept {return now<until;}
    void reset() noexcept {*this=CadenceGuard{};}
    void clearWindow() noexcept {bits={};next=repeats=samples=0;}
    // One XR frame with space-warp info chained (resubmit: the pair was already shown). True = suspension started.
    bool note(bool resubmit,bool halfRate,std::uint64_t now) noexcept {
        if(!halfRate||suspended(now))return false;
        ++counted;
        repeats-=bits[next];bits[next]=std::uint8_t(resubmit);repeats+=bits[next];next=(next+1)%window;if(samples<window)++samples;
        if(repeats<=maxRepeats)return false;
        if(trips&&now-until>calmNs)backoff=baseNs; // calm since the last resume: start over
        lastSuspendNs=backoff;until=now+backoff;backoff=std::min(backoff*2,maxNs);++trips;clearWindow();return true;
    }
};
} // namespace rdrvr::runtime::spacewarp
