#pragma once
// THROW: toss the revolver and catch it with either hand. "Visual throw": for the engine the weapon
// simply goes back to its holster (PutAwayWeapon, synchronous, exactly the HOLD release path) and stays
// in the inventory the whole time. The holstered item's own every-frame weapWeapon::Update (the pinned
// call the DUAL phantom also re-poses, ret 0x26770a4) gets the mod's ballistic pose instead of the
// holster bone (GameXr vrThrowPose, holster-locator correction K = the PhantomFix measurement). World
// collision = the laser's NativeAimProbe::castBetween (player excluded), bounce / settle / rest on the
// side. Catch = an empty hand closes on the gun (air) or above it (ground) -> SelectItem as OpDraw.
// Ending the flight for ANY reason (timeouts, distance, context, the game draws it, slot change, menu,
// failures) = stop overriding the pose -> the gun is in its holster. Nothing can be lost: the inventory
// is never touched.
//
// Threads:
//  - input thread (holsters InputBrain): ReleaseEstimator per hand (grip pose history), the throw decision at a HOLD release
//    (Release box + holsters op OpThrow), the catch test against `targetBox` (holsters op OpCatch).
//  - engine main thread: holsters EngineSide executes OpThrow / OpCatch (prepare -> PutAwayWeapon -> launch, SelectItem -> catching) and
//    calls observe() from its poll; tick() (mainthread ModuleThrow) runs the flight: probe, bounce, settle, timeouts, catch target.
//  - weapon pose hooks (any thread): evalModel() of the published arc (Box<Arc>, lock-free).
// Kill switch: debug.rdrvr.vr.throw 0 (startup; also forces the menu row THROW WEAPON OFF - PROP). Menu: HANDS / WEAPONS -> THROW WEAPON.
#include "VrMainThread.h"
#include "VrHaptics.h"
#include "VrTouch.h" // Reload / cylinder close: the runtime grip velocities of the same XR sample (XrSide)
#include "runtime/ControllerInput.h"
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <type_traits>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace rdrvr::vr::throwing {
using runtime::ControllerSample;
using V3=std::array<float,3>;using M3=std::array<float,9>;using M4=std::array<float,16>; // M3/M4 column-major (RT::Mat4 layout)

// ---- configuration (startup properties debug.rdrvr.vr.throw.*; written once before any thread uses them) ----
struct Config {
    // Tosses qualify from 1.0 m/s peak / 0.6 m/s at release. Gravity x0.6 with launch gain 1.0 gives about
    // the arc of gain 1.25 at full gravity (apex x1.07), flown 1.33x slower. Air / ground catch radius
    // 0.35 m; a magnet bends the flight into an empty hand within pullRadius (stronger while the hand
    // closes); the throwing hand may catch after 150 ms.
    float minSpeed{1.f},releaseSpeed{.6f},gain{1.f},spin{.8f},maxSpeed{12.f},maxSpin{25.f};  // m/s, rad/s (tracking space)
    float gravityScale{.6f},pullRadius{.45f},pullRate{10.f},pullAway{.30f},pullMinGravity{.3f},pullTotalS{1.2f};std::uint64_t pullGraceNs{300'000'000},pullMaxNs{700'000'000};
    // The estimate window (the toss stroke can end > 100 ms before the grip reads open), the "still moving"
    // window before the release, the XR-rate tape's freshness, a downward-dominant release (|v.y| > downDot |v|) is never a throw.
    std::uint64_t windowNs{250'000'000},releaseWindowNs{120'000'000},tapeMaxAgeNs{60'000'000};float downDot{.7f};
    float catchRadius{.35f},groundRadius{.35f},groundHeight{.60f},magnetRadius{.55f};
    float restS{30.f},flyMaxS{6.f},distM{25.f},bounce{.30f},friction{.35f},drag{.05f},angularDamping{.30f};
    // Absolute safety nets, whatever the state:
    // totalS since the launch (flying / bouncing / lying: 12 s, then it flies back), no movement over stillM for stillS while FLYING
    // (a flight that hangs on geometry), more than belowM under the player's feet (fell through planks / into a hole). A flight that
    // barely moves for stuckS after an impact (sliding / jittering on a steep or thin surface) and a run of tinyBounces bounces slower
    // than tinyBounceSpeed settle where they are (RESTING: the ground pick-up still works until totalS).
    float totalS{12.f},stillS{3.f},stillM{.05f},belowM{2.f},stuckS{.6f},stuckM{.10f},tinyBounceSpeed{.8f};unsigned tinyBounces{3u};
    unsigned slots{2u};               // bit (1 << slot): 2 = SIDEARM; 3 adds MELEE (knife); long guns are never thrown
    bool closedCatch{true},physics{},pull{true};unsigned returnMs{300};
    std::uint64_t graceNs{150'000'000},catchingNs{400'000'000},placementMaxAgeNs{150'000'000},ackTimeoutNs{500'000'000};
    float gravity() const noexcept {return -9.81f*gravityScale;}
};
inline Config config{};
inline std::atomic<bool> killed{},menuOn{true},started{};
inline bool active() noexcept {return !killed.load(std::memory_order_relaxed)&&menuOn.load(std::memory_order_relaxed);}
inline void setMenu(bool on) noexcept {menuOn.store(on,std::memory_order_relaxed);}
inline bool slotThrowable(int slot) noexcept {return slot>=0&&slot<2&&(config.slots>>unsigned(slot)&1u);} // MELEE / SIDEARM only
// engine::WeaponClass values (WeaponTypeReader): 1 Pistol, 2 DualPistol, 10 Melee.
inline bool kindThrowable(int kind) noexcept {return ((kind==1||kind==2)&&slotThrowable(1))||(kind==10&&slotThrowable(0));}

// ---- small seqlock (single writer, lock-free readers; same contract as holsters::Seqlock) ----
template<class T> class Box {
    static_assert(std::is_trivially_copyable_v<T>&&sizeof(T)%8==0);
    static constexpr std::size_t words=sizeof(T)/8;
    std::atomic<std::uint64_t> sequence_{0};std::array<std::atomic<std::uint64_t>,words> data_{};
public:
    void store(const T& value) noexcept {
        std::array<std::uint64_t,words> raw{};std::memcpy(raw.data(),&value,sizeof(T));
        const auto s=sequence_.load(std::memory_order_relaxed);sequence_.store(s+1,std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for(std::size_t i=0;i<words;++i)data_[i].store(raw[i],std::memory_order_relaxed);
        sequence_.store(s+2,std::memory_order_release);
    }
    bool load(T& out) const noexcept {
        for(int attempt=0;attempt<8;++attempt){
            const auto s0=sequence_.load(std::memory_order_acquire);if(s0&1u)continue;
            std::array<std::uint64_t,words> raw{};for(std::size_t i=0;i<words;++i)raw[i]=data_[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if(sequence_.load(std::memory_order_relaxed)==s0){if(!s0)return false;std::memcpy(&out,raw.data(),sizeof(T));return true;}
        }
        return false;
    }
};

// =====================================================================================================================
// Pure math (host-tested).
namespace math {
inline V3 add(V3 a,V3 b) noexcept {return {a[0]+b[0],a[1]+b[1],a[2]+b[2]};}
inline V3 sub(V3 a,V3 b) noexcept {return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
inline V3 mul(V3 a,float s) noexcept {return {a[0]*s,a[1]*s,a[2]*s};}
inline float dot(V3 a,V3 b) noexcept {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
inline V3 cross(V3 a,V3 b) noexcept {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
inline float length(V3 a) noexcept {return std::sqrt(dot(a,a));}
inline V3 normalized(V3 a) noexcept {const float l=length(a);return l>1e-9f&&std::isfinite(l)?mul(a,1.f/l):V3{0,0,0};}
inline bool finite(V3 a) noexcept {return std::isfinite(a[0])&&std::isfinite(a[1])&&std::isfinite(a[2]);}
inline bool finite(const M4& m) noexcept {for(float x:m)if(!std::isfinite(x))return false;return true;}
inline M3 identity3() noexcept {return {1,0,0,0,1,0,0,0,1};}
inline V3 column(const M3& m,unsigned c) noexcept {return {m[c*3],m[c*3+1],m[c*3+2]};}
inline V3 mulv(const M3& m,V3 v) noexcept {V3 r{};for(unsigned i=0;i<3;++i)r[i]=m[i]*v[0]+m[3+i]*v[1]+m[6+i]*v[2];return r;}
inline V3 applyT(const M3& m,V3 v) noexcept {return {dot(column(m,0),v),dot(column(m,1),v),dot(column(m,2),v)};}
inline M3 multiply(const M3& a,const M3& b) noexcept {M3 r{};for(unsigned c=0;c<3;++c){const V3 x=mulv(a,column(b,c));for(unsigned i=0;i<3;++i)r[c*3+i]=x[i];}return r;}
// Rotation by the rotation vector rv (axis * angle), Rodrigues.
inline M3 rotation(V3 rv) noexcept {
    const float angle=length(rv);if(!(angle>1e-9f)||!std::isfinite(angle))return identity3();
    const V3 k=mul(rv,1.f/angle);const float c=std::cos(angle),s=std::sin(angle),t=1.f-c;
    M3 r{};
    // column-major: r[col*3+row] = c*delta + s*[k]x(row,col) + t*k_row*k_col
    const float K[3][3]{{0,-k[2],k[1]},{k[2],0,-k[0]},{-k[1],k[0],0}};
    for(unsigned col=0;col<3;++col)for(unsigned row=0;row<3;++row)r[col*3+row]=(row==col?c:0.f)+s*K[row][col]+t*k[row]*k[col];
    return r;
}
// Shortest-arc rotation turning unit a onto unit b.
inline M3 shortestArc(V3 a,V3 b) noexcept {
    a=normalized(a);b=normalized(b);const V3 x=cross(a,b);const float s=length(x),c=dot(a,b);
    if(s<1e-7f)return identity3(); // parallel (anti-parallel never asked: callers pick the nearer sign)
    return rotation(mul(x,std::atan2(s,c)/s));
}
inline bool orthonormalize(M3& m) noexcept {
    V3 x=column(m,0),y=column(m,1);x=normalized(x);if(length(x)<.5f)return false;
    y=sub(y,mul(x,dot(x,y)));y=normalized(y);if(length(y)<.5f)return false;
    V3 z=cross(x,y);if(dot(z,column(m,2))<0)return false; // reflection
    for(unsigned i=0;i<3;++i){m[i]=x[i];m[3+i]=y[i];m[6+i]=z[i];}return true;
}
using Q=std::array<float,4>; // x y z w
inline Q quat(const M3& m) noexcept { // rotation matrix -> unit quaternion
    const float m00=m[0],m11=m[4],m22=m[8],m01=m[3],m10=m[1],m02=m[6],m20=m[2],m12=m[7],m21=m[5];
    const float tr=m00+m11+m22;Q q{};
    if(tr>0){const float s=std::sqrt(tr+1.f)*2.f;q={(m21-m12)/s,(m02-m20)/s,(m10-m01)/s,.25f*s};}
    else if(m00>m11&&m00>m22){const float s=std::sqrt(1.f+m00-m11-m22)*2.f;q={.25f*s,(m01+m10)/s,(m02+m20)/s,(m21-m12)/s};}
    else if(m11>m22){const float s=std::sqrt(1.f+m11-m00-m22)*2.f;q={(m01+m10)/s,.25f*s,(m12+m21)/s,(m02-m20)/s};}
    else{const float s=std::sqrt(1.f+m22-m00-m11)*2.f;q={(m02+m20)/s,(m12+m21)/s,.25f*s,(m10-m01)/s};}
    const float n=std::sqrt(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);if(n>0)for(float& x:q)x/=n;return q;
}
inline M3 matrix(Q q) noexcept {
    const float x=q[0],y=q[1],z=q[2],w=q[3];
    return {1-2*(y*y+z*z),2*(x*y+z*w),2*(x*z-y*w), 2*(x*y-z*w),1-2*(x*x+z*z),2*(y*z+x*w), 2*(x*z+y*w),2*(y*z-x*w),1-2*(x*x+y*y)};
}
inline Q slerp(Q a,Q b,float t) noexcept {
    float d=a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+a[3]*b[3];if(d<0){for(float& x:b)x=-x;d=-d;}
    Q r{};
    if(d>.9995f){for(unsigned i=0;i<4;++i)r[i]=a[i]+(b[i]-a[i])*t;}
    else{const float th=std::acos(std::fmin(d,1.f)),s=std::sin(th),wa=std::sin((1-t)*th)/s,wb=std::sin(t*th)/s;for(unsigned i=0;i<4;++i)r[i]=a[i]*wa+b[i]*wb;}
    const float n=std::sqrt(r[0]*r[0]+r[1]*r[1]+r[2]*r[2]+r[3]*r[3]);if(n>0)for(float& x:r)x/=n;return r;
}
inline float smoothstep(float t) noexcept {t=std::fmin(1.f,std::fmax(0.f,t));return t*t*(3-2*t);}
// Affine 4x4 (column-major) pieces: rotation part / native scale (|col0|) / translation.
inline float scaleOf(const M4& m) noexcept {return std::sqrt(m[0]*m[0]+m[1]*m[1]+m[2]*m[2]);}
inline M3 rotationOf(const M4& m) noexcept {const float s=scaleOf(m);M3 r{};if(!(s>1e-9f))return identity3();
    for(unsigned c=0;c<3;++c)for(unsigned i=0;i<3;++i)r[c*3+i]=m[c*4+i]/s;orthonormalize(r);return r;}
inline V3 xform(const M4& m,V3 p) noexcept {return {m[0]*p[0]+m[4]*p[1]+m[8]*p[2]+m[12],m[1]*p[0]+m[5]*p[1]+m[9]*p[2]+m[13],m[2]*p[0]+m[6]*p[1]+m[10]*p[2]+m[14]};}
inline V3 xformDir(const M4& m,V3 v) noexcept {return {m[0]*v[0]+m[4]*v[1]+m[8]*v[2],m[1]*v[0]+m[5]*v[1]+m[9]*v[2],m[2]*v[0]+m[6]*v[1]+m[10]*v[2]};}
// model = [R*scale | p - R*scale*centre]: the model frame whose point `centre` sits at p.
inline M4 compose(const M3& r,V3 p,float scale,V3 centre) noexcept {
    M4 m{};for(unsigned c=0;c<3;++c)for(unsigned i=0;i<3;++i)m[c*4+i]=r[c*3+i]*scale;
    const V3 off=mulv(r,mul(centre,scale));for(unsigned i=0;i<3;++i)m[12+i]=p[i]-off[i];m[15]=1;return m;
}
// Rigid blend of two affine frames with uniform scale (translation lerp, rotation slerp, scale lerp).
inline M4 blend(const M4& a,const M4& b,float t) noexcept {
    const float sa=scaleOf(a),sb=scaleOf(b),s=sa+(sb-sa)*t;
    const M3 r=matrix(slerp(quat(rotationOf(a)),quat(rotationOf(b)),t));
    M4 m{};for(unsigned c=0;c<3;++c)for(unsigned i=0;i<3;++i)m[c*4+i]=r[c*3+i]*s;
    for(unsigned i=0;i<3;++i)m[12+i]=a[12+i]+(b[12+i]-a[12+i])*t;m[15]=1;return m;
}
// Inverse of an affine map with a uniform scale (LOCAL -> world plan maps): x = R^T (y - t) / s.
inline bool inverseUniform(const M4& m,M4& out) noexcept {
    const float s=scaleOf(m);if(!(s>1e-9f)||!std::isfinite(s))return false;
    out={};for(unsigned c=0;c<3;++c)for(unsigned i=0;i<3;++i)out[c*4+i]=m[i*4+c]/(s*s);
    const V3 t{m[12],m[13],m[14]};const V3 r=xformDir(out,t);for(unsigned i=0;i<3;++i)out[12+i]=-r[i];out[15]=1;return finite(out);
}
} // namespace math

// =====================================================================================================================
// Release velocity estimator (input thread; one per hand). Grip poses of new controller samples in tracking space (LOCAL metres).
class ReleaseEstimator {
public:
    static constexpr unsigned capacity=32; // 250 ms of XR-rate samples (72..90 Hz)
    static constexpr float glitchSpeed=20.f; // m/s: faster than any hand = a tracking jump
    struct Sample {std::uint64_t ns{};V3 p{};math::Q q{0,0,0,1};};
    // redirected = the fastest interval pointed down (the dip before / the recoil after a toss) and an upward-or-level stroke at
    // least half as fast was chosen instead; downPeak = that rejected downward speed.
    struct Estimate {bool valid{},glitch{},redirected{};V3 v{},w{};float speedPeak{},speedRelease{},downPeak{};unsigned samples{};std::uint64_t peakNs{},releaseNs{};};
    void reset() noexcept {count_=0;next_=0;lastSequence_=0;}
    // Appends the grip pose of hand h for a NEW sample (sequence changed); a new epoch / reference space / continuity restarts history.
    void note(const ControllerSample& in,unsigned h) noexcept {
        if(h>1)return;
        if(in.epoch!=epoch_||in.referenceSpace!=space_||in.continuity!=continuity_){reset();epoch_=in.epoch;space_=in.referenceSpace;continuity_=in.continuity;}
        if(!in.sequence||in.sequence==lastSequence_)return;
        lastSequence_=in.sequence;
        const auto& g=in.grip[h];
        if(!g.positionValid||!g.orientationValid)return;
        add(in.publishedNs,{g.position[0],g.position[1],g.position[2]},{g.orientation[0],g.orientation[1],g.orientation[2],g.orientation[3]});
    }
    // one pose (tracking space) at ns; older-or-equal than the newest is ignored (the XR tape replays in time order).
    void add(std::uint64_t ns,V3 p,math::Q q) noexcept {
        Sample s{};s.ns=ns;s.p=p;s.q=q;
        if(!ns||!math::finite(s.p)||!std::isfinite(s.q[3]))return;
        if(count_){const auto& newest=ring_[(next_+capacity-1)%capacity];if(s.ns<=newest.ns)return;}
        ring_[next_]=s;next_=(next_+1)%capacity;if(count_<capacity)++count_;
    }
    // Peak-centred finite differences over the last windowNs (intervals >= 4 ms; the hand decelerates while it opens).
    // window 250 ms, "speed at release" = the fastest interval ending in the last releaseWindowNs, and a downward-dominant
    // peak gives way to the fastest upward / level local peak >= max(half of it, minUseful) - nobody tosses a revolver into the ground,
    // a fast downward interval next to a toss is the dip before it or the recoil after it.
    Estimate estimate(std::uint64_t windowNs=250'000'000,float minUseful=.8f,std::uint64_t releaseWindowNs=120'000'000) const noexcept {
        Estimate e{};if(count_<2)return e;
        std::array<Sample,capacity> s{};unsigned n=0;const auto& newest=ring_[(next_+capacity-1)%capacity];
        for(unsigned i=0;i<count_;++i){const auto& x=ring_[(next_+capacity-count_+i)%capacity];if(x.ns+windowNs>=newest.ns)s[n++]=x;}
        if(n<2)return e;
        std::array<V3,capacity> v{},w{};std::array<std::uint64_t,capacity> at{};unsigned m=0;unsigned prev=0;
        for(unsigned i=1;i<n;++i){
            const auto dtNs=s[i].ns-s[prev].ns;if(dtNs<4'000'000ull)continue;
            const float dt=float(double(dtNs)*1e-9);
            v[m]=math::mul(math::sub(s[i].p,s[prev].p),1.f/dt);
            const auto& a=s[prev].q;const auto& b=s[i].q; // dq = b * conj(a): rotation from a to b in tracking space
            math::Q d{b[3]*-a[0]+b[0]*a[3]+b[1]*-a[2]-b[2]*-a[1],b[3]*-a[1]-b[0]*-a[2]+b[1]*a[3]+b[2]*-a[0],
                      b[3]*-a[2]+b[0]*-a[1]-b[1]*-a[0]+b[2]*a[3],b[3]*a[3]-b[0]*-a[0]-b[1]*-a[1]-b[2]*-a[2]};
            if(d[3]<0)for(float& x:d)x=-x;
            const float sh=std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
            w[m]=sh>1e-9f?math::mul(V3{d[0],d[1],d[2]},2.f*std::atan2(sh,d[3])/sh/dt):V3{0,0,0};
            at[m]=s[i].ns;++m;prev=i;
            // a tracking jump restarts the history (the longer window must not let an old re-acquisition veto a toss); a jump in
            // the newest interval is still reported as a glitch below
            if(math::length(v[m-1])>glitchSpeed&&i+1<n){m=0;}
        }
        if(!m)return e;
        unsigned k=0;for(unsigned i=1;i<m;++i)if(math::length(v[i])>math::length(v[k]))k=i;
        if(math::length(v[k])>glitchSpeed){e.glitch=true;e.speedPeak=math::length(v[k]);e.samples=n;return e;} // a tracking jump, never a throw
        { // A downward-dominant peak -> the toss stroke next to it (tracking space: +Y up)
            const float pk=math::length(v[k]);
            if(v[k][1]<-.5f*pk){
                int best=-1;float bs=0.f;
                for(unsigned j=0;j<m;++j){if(j==k)continue;const float sp=math::length(v[j]);
                    if(sp<std::fmax(.5f*pk,minUseful)||v[j][1]<-.2f*sp)continue;
                    if((j>0&&math::length(v[j-1])>sp)||(j+1<m&&math::length(v[j+1])>sp))continue; // local maximum only
                    if(sp>bs){bs=sp;best=int(j);}}
                if(best>=0){e.downPeak=pk;e.redirected=true;k=unsigned(best);}
            }
        }
        // peak-centred mean: the peak interval and its neighbours still at >= 85 % of the peak (a neighbour already decelerating while
        // the hand opens, or still accelerating, would pull the throw short)
        const float peak=math::length(v[k]);const unsigned lo=k?k-1:0,hi=k+1<m?k+1:k;V3 sv{},sw{};float c=0;
        for(unsigned i=lo;i<=hi;++i)if(i==k||math::length(v[i])>=.85f*peak){sv=math::add(sv,v[i]);sw=math::add(sw,w[i]);c+=1.f;}
        e.v=math::mul(sv,1.f/c);e.w=math::mul(sw,1.f/c);
        e.speedPeak=peak;e.peakNs=at[k];e.releaseNs=newest.ns;e.samples=n;
        e.speedRelease=math::length(v[m-1]);if(m>=2)e.speedRelease=std::fmax(e.speedRelease,math::length(v[m-2]));
        for(unsigned i=0;i<m;++i)if(at[i]+releaseWindowNs>=newest.ns)e.speedRelease=std::fmax(e.speedRelease,math::length(v[i]));
        e.valid=math::finite(e.v)&&math::finite(e.w);return e;
    }
    unsigned count() const noexcept {return count_;}
private:
    std::array<Sample,capacity> ring_{};unsigned count_{},next_{};
    std::uint64_t epoch_{},space_{},continuity_{},lastSequence_{};
};

// =====================================================================================================================
// XR-rate grip tape (XR thread -> input thread). The input brain runs once per GAME frame (26-36 fps
// on Quest 3S): only 3 samples per 100 ms. XrHarness pushes every locateHands sample here (next to the
// HANDS / MELEE tapes); the release estimate prefers it.
// Entry (152 bytes): ns = publishedNs (freshness against the input thread's now); poseNs = the time the
// poses / velocities are predicted for (pdtMono, CLOCK_MONOTONIC = steady_clock on Android; 0 = unknown):
// the gesture detectors difference on it, never on ns. w / v = the runtime's XrSpaceVelocity of both
// grip spaces (LOCAL). valid: bit h pose (VALID & TRACKED), bit 2+h angular velocity, bit 4+h linear
// velocity. touch: bit h index finger on the trigger (SPIN), bit 2 the touch action active, bit 31
// tapeMasked = the VR menu ate the input (grips / triggers zeroed). fromTape tests only `valid >> h & 1`.
struct TapeEntry {std::uint64_t ns{},space{};std::array<V3,2> p{};std::array<math::Q,2> q{};
    std::uint32_t valid{},touch{};std::array<float,2> grips{},triggers{};std::uint64_t poseNs{};std::array<V3,2> w{},v{};};
static_assert(sizeof(TapeEntry)==152&&sizeof(TapeEntry)%8==0,"the 152-byte tape entry");
inline constexpr std::uint32_t tapeMaskedBit=0x80000000u; // touch bit 31
inline constexpr std::uint32_t tapeAngular(unsigned h) noexcept {return 4u<<h;}
inline constexpr std::uint32_t tapeLinear(unsigned h) noexcept {return 16u<<h;}
inline constexpr unsigned tapeSize=48;
inline std::array<Box<TapeEntry>,tapeSize> tapeRing{};
inline std::atomic<std::uint64_t> tapeHead{},tapePushed{},tapeUsed{},tapeFallback{},tapeMasked{},tapeVelocity{};
// XR thread (single writer), every controller sample. poseNs = pdtMono of the frame the poses were located for (0 = unknown), masked = the
// VR menu consumed the input this sample (its grips / triggers are already zero).
inline void pushSample(const ControllerSample& s,std::uint64_t poseNs=0,bool masked=false) noexcept {
    if(!s.focused||!s.active||!s.publishedNs)return;
    TapeEntry t{};t.ns=s.publishedNs;t.space=s.referenceSpace;t.poseNs=poseNs;t.grips=s.grips;t.triggers=s.triggers;
    if(masked){t.touch|=tapeMaskedBit;tapeMasked.fetch_add(1,std::memory_order_relaxed);}
    for(unsigned h=0;h<2;++h){const auto& g=s.grip[h];if(!g.positionValid||!g.orientationValid)continue;
        t.p[h]={g.position[0],g.position[1],g.position[2]};t.q[h]={g.orientation[0],g.orientation[1],g.orientation[2],g.orientation[3]};
        if(math::finite(t.p[h])&&std::isfinite(t.q[h][3]))t.valid|=1u<<h;}
    if(!t.valid)return;
    const auto& side=::rdrvr::vr::touch::xrSide; // the same XR sample's runtime velocities (ControllerActions::read)
    if(side.publishedNs&&side.publishedNs==s.publishedNs){
        for(unsigned h=0;h<2;++h){if(!(t.valid>>h&1u))continue;
            if(side.validBits>>h&1u){t.w[h]={side.w[h][0],side.w[h][1],side.w[h][2]};if(math::finite(t.w[h]))t.valid|=tapeAngular(h);}
            if(side.validBits>>(2+h)&1u){t.v[h]={side.v[h][0],side.v[h][1],side.v[h][2]};if(math::finite(t.v[h]))t.valid|=tapeLinear(h);}}
        if(t.valid&0x3cu)tapeVelocity.fetch_add(1,std::memory_order_relaxed);
    }
    const auto head=tapeHead.load(std::memory_order_relaxed);
    tapeRing[head%tapeSize].store(t);tapeHead.store(head+1,std::memory_order_release);tapePushed.fetch_add(1,std::memory_order_relaxed);
}
// Input thread: hand h's XR-rate history of reference space `space` (the last 400 ms up to now) into `out`. False = the tape is not
// fresh (no XR samples within tapeMaxAgeNs of now) or too short: the caller uses its own game-rate history.
inline bool fromTape(unsigned h,std::uint64_t space,std::uint64_t now,ReleaseEstimator& out,std::uint64_t maxAgeNs) noexcept {
    out.reset();if(h>1||!space)return false;
    const auto head=tapeHead.load(std::memory_order_acquire);
    std::array<TapeEntry,tapeSize> got{};unsigned n=0;
    for(std::uint64_t i=head>tapeSize?head-tapeSize:0;i<head;++i){TapeEntry t{};
        if(!tapeRing[i%tapeSize].load(t)||t.space!=space||!(t.valid>>h&1u)||t.ns>now+5'000'000ull||t.ns+400'000'000ull<now)continue;got[n++]=t;}
    for(unsigned i=1;i<n;++i){TapeEntry x=got[i];unsigned j=i;while(j>0&&got[j-1].ns>x.ns){got[j]=got[j-1];--j;}got[j]=x;} // by time
    if(n<3||got[n-1].ns+maxAgeNs<now)return false;
    for(unsigned i=0;i<n;++i)out.add(got[i].ns,got[i].p[h],got[i].q[h]);
    return out.count()>=3;
}

// =====================================================================================================================
// Published state.
enum State : int {StateIdle=0,StateFlying=1,StateResting=2,StateCatching=3,StateReturning=4};
inline constexpr std::array<const char*,5> stateNames{"IDLE","FLYING","RESTING","CATCHING","RETURNING"};
enum End : unsigned {EndCaught,EndRecall,EndReturnRest,EndReturnFly,EndReturnDistance,EndReturnContext,EndLostFall,EndGameDraw,EndSlotChanged,
    EndNoWeapon,EndKill,EndMenuOff,EndReturnTimeout,EndReturnStill,EndReturnBelow,EndCount};
inline constexpr std::array<const char*,EndCount> endNames{"CAUGHT","RECALL","RETURN_REST","RETURN_FLY","RETURN_DISTANCE","RETURN_CONTEXT",
    "LOST_FALL","GAME_DRAW","SLOT_CHANGED","NO_WEAPON","KILL","MENU_OFF","RETURN_TIMEOUT","RETURN_STILL","RETURN_BELOW"};
enum Refuse : unsigned {RefuseGate,RefuseNotSidearm,RefuseNoPlacement,RefuseStale,RefuseSwitching,RefuseCylinder,RefuseOff,RefuseBusy,RefuseNotInHand,
    RefuseNoSlot,RefuseCount};
inline constexpr std::array<const char*,RefuseCount> refuseNames{"GATE","NOT_SIDEARM","NO_PLACEMENT","STALE","SWITCHING","CYLINDER","OFF","BUSY",
    "NOT_IN_HAND","NO_SLOT"};
enum Guard : unsigned {GuardSlow,GuardTrigger,GuardRecentShot,GuardCylinder,GuardNotHold,GuardNotAllowed,GuardMenu,GuardNotThrowable,GuardBusy,
    GuardGlitch,GuardDown,GuardSocket,GuardRecentClose,GuardCount}; // down = a downward-dominant release (holster), socket = released inside a holster zone
    // reload: recentClose = released < 400 ms after any cylinder close (or while
    // the PUSH visual follows the hand): a wave-close + let-go is a holster, never a toss
inline constexpr std::array<const char*,GuardCount> guardNames{"slow","trigger","recentShot","cylinder","notHold","notAllowed","menu","notThrowable","busy",
    "glitch","down","socket","recentClose"};
enum CatchRefuse : unsigned {CatchGate,CatchState,CatchSlot,CatchGameRefused,CatchSelectFailed,CatchBusy,CatchRefuseCount};
inline constexpr std::array<const char*,CatchRefuseCount> catchRefuseNames{"GATE","STATE","SLOT","GAME_REFUSED","SELECT_FAILED","BUSY"};
enum PoseDecline : unsigned {DeclineNoArc,DeclineWeapon,DeclineEval,DeclineArgument,DeclineRigid,DeclineCatch,DeclineCount};
inline constexpr std::array<const char*,DeclineCount> declineNames{"NO_ARC","WEAPON","EVAL","ARGUMENT","RIGID","CATCH"};

inline std::atomic<int> state{StateIdle},slot{-1},throwHand{-1},catchHand{-1};
inline std::atomic<std::uintptr_t> item{},weapon{};
inline std::atomic<std::uint32_t> generation{};
inline std::atomic<std::uint64_t> throwNs{},endNs{},lastShotNs{};
inline std::atomic<bool> catchInHand{},noProbe{};
// OpThrow acknowledgement for the input brain: (command sequence << 1) | started.
inline std::atomic<std::uint64_t> ack{};
inline int currentState() noexcept {return state.load(std::memory_order_acquire);}
inline bool busy() noexcept {return currentState()!=StateIdle;}
// The thrown item's weapon (pose hooks, any thread): the state while `w` is the thrown weapon, else Idle.
inline int poseState(std::uintptr_t w) noexcept {
    if(!w)return StateIdle;const int s=state.load(std::memory_order_acquire);
    return s!=StateIdle&&weapon.load(std::memory_order_acquire)==w?s:StateIdle;
}

// The flight (world units, the engine's Y-up world; one or two segments: seg[1] takes over at switchNs = a bounce found ahead).
struct Segment {std::uint64_t t0{};V3 p{},v{},g{};M3 r{};V3 w{};float pad{};};
struct Arc {
    std::uint64_t generation{},switchNs{},blendStartNs{},returnStartNs{};
    std::uintptr_t weapon{},item{};
    std::array<Segment,2> seg{};
    V3 centre{};float scale{1};           // the model point that flies on the arc (model units), the weapon's native scale
    M3 fromR{};V3 fromP{};                // settle blend: the impact pose (centre position)
    std::uint32_t state{},blendMs{},returnMs{},animated{};
    M4 lastModel{};                       // RETURNING: the model at the start of the fly-back blend
};
inline Box<Arc> arcBox{};
// Pure: the gun model (world, column-major) at `now` (the hook's clock). False = no usable pose.
inline bool evalModel(const Arc& a,std::uint64_t now,M4& out) noexcept {
    using namespace math;
    const Segment& s=(a.switchNs&&now>=a.switchNs)?a.seg[1]:a.seg[0];
    float dt=now>s.t0?float(double(now-s.t0)*1e-9):0.f;dt=std::fmin(dt,.5f);
    V3 p=add(add(s.p,mul(s.v,dt)),mul(s.g,.5f*dt*dt));
    M3 r=multiply(rotation(mul(s.w,dt)),s.r);
    if(a.blendMs&&a.blendStartNs&&now>=a.blendStartNs&&now<a.blendStartNs+std::uint64_t(a.blendMs)*1'000'000ull){
        const float t=smoothstep(float(double(now-a.blendStartNs)/(double(a.blendMs)*1e6)));
        r=matrix(slerp(quat(a.fromR),quat(r),t));p=add(a.fromP,mul(sub(p,a.fromP),t));
    }
    out=compose(r,p,a.scale,a.centre);return finite(out);
}
inline float returnFraction(const Arc& a,std::uint64_t now) noexcept {
    if(!a.returnMs||now<=a.returnStartNs)return 0.f;
    return math::smoothstep(float(double(now-a.returnStartNs)/(double(a.returnMs)*1e6)));
}

// Catch target for the input brain (tracking space LOCAL metres of `referenceSpace`).
struct Target {V3 pos{},vel{};float radius{},groundRadius{},groundHeight{};std::uint32_t state{};std::uint64_t ns{},referenceSpace{};
    std::int32_t slot{-1},throwHand{-1};std::uint64_t throwNs{};std::uint32_t generation{},pad{};};
inline Box<Target> targetBox{};
// The empty hands the magnet may pull the flying gun towards (input brain, every usable frame while a target is
// published; LOCAL metres of referenceSpace): bit h of mask = hand h is free to catch (not holding / phantom / foregrip), grip = analog grip.
struct CatchHands {std::array<V3,2> pos{};std::array<float,2> grip{};std::uint32_t mask{},pad{};std::uint64_t ns{},referenceSpace{};};
inline Box<CatchHands> handsBox{};
struct Release {std::uint64_t seq{},releaseNs{},peakNs{},referenceSpace{};V3 v{},w{};float speedPeak{},speedRelease{};std::uint32_t samples{};std::int32_t hand{-1};
    std::uint32_t flags{};float downPeak{};
    V3 own{};std::uint32_t pad2{};}; // Flags: 1 = XR-rate tape, 2 = redirected from a downward peak; SPIN: own = the gun's spin (LOCAL rad/s)
inline constexpr std::uint32_t releaseFromTape=1u,releaseRedirected=2u;
inline constexpr std::uint32_t releaseFromSpin=4u; // SPIN: a release from the trigger finger (VrSpin.h): `own` is added to the flight spin unscaled
inline Box<Release> releaseBox{};

// ---- counters (report) ----
inline std::atomic<std::uint64_t> throwsRequested{},throwsStarted{},flightTicks{},probes{},probeFails{},probeNsSum{},probeNsMax{},hits{},bounces{},
    normalInvalid{},noProbeFrames{},catchesPosted{},catchesAir{},catchesClosed{},catchesGround{},catchesSameHand{},catchesOtherHand{},catchTimeouts{},
    magnetTicks{},poseCalls{},posePlaced{},poseCatching{},poseReturning{},fixMeasured{},fixApplied{},renderForcedCalls{},renderRestored{},
    promotes{},ackTimeouts{},holsterFallbacks{},targetsPublished{},mapFailures{},redirects{},
    magnetPulls{},magnetPullTicks{}, // Approaches the magnet bent towards a hand / ticks it steered
    stuckRests{},tinyRests{},slopeRests{}; // Settled where it hung / after a run of tiny bounces / a slow bounce on a slope
inline std::array<std::atomic<std::uint64_t>,RefuseCount> refusals{};
inline std::array<std::atomic<std::uint64_t>,GuardCount> guards{};
inline std::array<std::atomic<std::uint64_t>,EndCount> ends{};
inline std::array<std::atomic<std::uint64_t>,CatchRefuseCount> catchRefusals{};
inline std::array<std::atomic<std::uint64_t>,DeclineCount> poseDeclines{};
inline std::atomic<const char*> lastEnd{"NONE"},lastRefusal{"NONE"};
inline std::atomic<float> speedMax{},maxDistM{},maxHeightM{};
inline Box<Release> lastReleaseBox{}; // the last QUALIFIED release (report)
struct Landing {V3 point{},normal{};std::uint64_t ns{};};inline Box<Landing> lastLandingBox{};
inline void atomicMax(std::atomic<float>& a,float v) noexcept {float m=a.load(std::memory_order_relaxed);while(v>m&&!a.compare_exchange_weak(m,v)){}}
inline void atomicMax(std::atomic<std::uint64_t>& a,std::uint64_t v) noexcept {auto m=a.load(std::memory_order_relaxed);while(v>m&&!a.compare_exchange_weak(m,v)){}}
inline void bump(std::atomic<std::uint64_t>& a) noexcept {a.fetch_add(1,std::memory_order_relaxed);}
inline void guard(Guard g) noexcept {bump(guards[g]);}

// =====================================================================================================================
// Engine-side interface (GameXr provides it at startup; null = that piece is unavailable).
struct StartRequest {std::uintptr_t weapon{},item{};int hand{-1};bool phantom{};std::uint64_t now{};};
struct StartPose {
    M4 model{},map{};float upm{1};      // the gun model at release (world), LOCAL metres -> world of that placement, units per metre
    V3 centre{},barrel{0,0,-1},gunUp{0,1,0}; // model space: flying point, barrel direction, the gun's up (rest orientation)
    V3 up{0,1,0},moverVelocity{};        // world up, the player's mover velocity (world units / s)
    std::uint64_t ns{};std::uint32_t actorGuid{};std::uintptr_t actor{};bool animated{};
};
struct Hit {V3 point{},normal{};bool normalValid{};};
struct Map {M4 localToWorld{};std::uint64_t referenceSpace{};};
using StartFn=bool(*)(const StartRequest&,StartPose&) noexcept;
using ProbeFn=bool(*)(std::uint32_t actorGuid,const V3& from,const V3& to,bool& blocked,Hit& out) noexcept; // false = the cast failed
using MapFn=bool(*)(std::uint64_t now,Map& out) noexcept;
using PlayerFn=bool(*)(std::uintptr_t actor,V3& capsule) noexcept;
using OwnerWeaponFn=std::uintptr_t(*)() noexcept;
inline std::atomic<StartFn> startFn{};inline std::atomic<ProbeFn> probeFn{};inline std::atomic<MapFn> mapFn{};
inline std::atomic<PlayerFn> playerFn{};inline std::atomic<OwnerWeaponFn> ownerWeaponFn{};

// What holsters' poll saw this frame (main thread).
struct Facts {bool valid{};std::uintptr_t handItem{},slotItem{},slotWeapon{};bool context{true};};
// Prepared launch (computed BEFORE the engine put-away: a failure leaves the release a plain holster).
struct Prepared {bool ok{};StartPose pose{};Release release{};int hand{-1};bool phantom{};std::uint64_t now{};};

// =====================================================================================================================
// Main-thread state machine.
class Machine {
public:
    // OpThrow step 1: the start pose from the release placement + the release box. RefuseCount = ok.
    Refuse prepare(const StartRequest& r,std::uint32_t boxSeq,Prepared& out) noexcept {
        out=Prepared{};
        if(!active())return RefuseOff;
        if(busy())return RefuseBusy;
        Release rel{};if(!releaseBox.load(rel)||std::uint32_t(rel.seq&0x7fffffffu)!=boxSeq||rel.hand!=r.hand)return RefuseStale;
        const auto fn=startFn.load(std::memory_order_acquire);if(!fn)return RefuseNoPlacement;
        StartPose p{};bool ok=false;try{ok=fn(r,p);}catch(...){ok=false;}
        if(!ok||!math::finite(p.model)||!math::finite(p.map)||!(p.upm>0.f))return RefuseNoPlacement;
        if(!p.ns||(r.now>p.ns&&r.now-p.ns>config.placementMaxAgeNs))return RefuseStale;
        out.ok=true;out.pose=p;out.release=rel;out.hand=r.hand;out.phantom=r.phantom;out.now=r.now;return RefuseCount;
    }
    // OpThrow step 2 (after the put-away / phantom clear): the item is in slot k of the inventory -> FLYING.
    void launch(const Prepared& pr,int k,std::uintptr_t it,std::uintptr_t w,bool renderForced,std::uint64_t now) noexcept {
        using namespace math;
        const auto& p=pr.pose;const float upm=p.upm;
        const M3 ms{p.map[0],p.map[1],p.map[2],p.map[4],p.map[5],p.map[6],p.map[8],p.map[9],p.map[10]}; // LOCAL metres -> world (scaled)
        M3 rmap=ms;for(float& x:rmap)x/=upm;orthonormalize(rmap);
        V3 vl=mul(pr.release.v,config.gain);if(length(vl)>config.maxSpeed)vl=mul(normalized(vl),config.maxSpeed);
        V3 wl=mul(pr.release.w,config.spin);if(length(wl)>config.maxSpin)wl=mul(normalized(wl),config.maxSpin);
        if((pr.release.flags&releaseFromSpin)&&finite(pr.release.own)){
            wl=add(wl,pr.release.own);const float cap=std::fmax(config.maxSpin,30.f);if(length(wl)>cap)wl=mul(normalized(wl),cap);}
        V3 mover=p.moverVelocity;if(!finite(mover)||length(mover)>20.f*upm)mover={};
        f_={};f_.upm=upm;f_.up=normalized(p.up);if(length(f_.up)<.5f)f_.up={0,1,0};
        f_.barrel=normalized(p.barrel);f_.gunUp=normalized(p.gunUp);f_.actorGuid=p.actorGuid;f_.actor=p.actor;
        Arc a{};a.generation=generation.load(std::memory_order_relaxed)+1;a.weapon=w;a.item=it;a.centre=p.centre;a.scale=scaleOf(p.model);
        a.animated=p.animated?1u:0u;a.state=StateFlying;a.returnMs=config.returnMs;
        Segment& s=a.seg[0];s.t0=p.ns;s.r=rotationOf(p.model);s.p=xform(p.model,p.centre);
        s.v=add(mulv(ms,vl),mover);s.w=mulv(rmap,wl);s.g=mul(f_.up,config.gravity()*upm);
        arc_=a;f_.launchNs=now;f_.lastTickNs=now;f_.lastProbeNs=s.t0;f_.lastP=s.p;f_.releaseHeight=dot(s.p,f_.up);f_.lastP0=s.p;
        f_.renderForced=renderForced;f_.hand=pr.hand;f_.slot=k;
        publish(StateFlying);
        slot.store(k,std::memory_order_relaxed);item.store(it,std::memory_order_relaxed);weapon.store(w,std::memory_order_relaxed);
        throwHand.store(pr.hand,std::memory_order_relaxed);catchHand.store(-1,std::memory_order_relaxed);throwNs.store(now,std::memory_order_relaxed);
        catchInHand.store(false,std::memory_order_relaxed);generation.store(std::uint32_t(a.generation),std::memory_order_relaxed);
        state.store(StateFlying,std::memory_order_release);
        bump(throwsStarted);haptics::pulse(unsigned(pr.hand)&1u,.25f,15'000,now,haptics::SourceReturn);
        atomicMax(speedMax,length(vl));lastReleaseBox.store(pr.release);
    }
    // Immediate end: stop overriding (the gun is natively in its holster from the next Update).
    void end(End why,std::uint64_t now) noexcept {
        if(currentState()==StateIdle)return;
        if(f_.renderForced)pendingRefresh_=arc_.item;
        state.store(StateIdle,std::memory_order_release);
        arc_.state=StateIdle;arcBox.store(arc_);Target t{};t.state=StateIdle;t.ns=now;targetBox.store(t);
        slot.store(-1,std::memory_order_relaxed);item.store(0,std::memory_order_relaxed);weapon.store(0,std::memory_order_relaxed);
        throwHand.store(-1,std::memory_order_relaxed);catchHand.store(-1,std::memory_order_relaxed);catchInHand.store(false,std::memory_order_relaxed);
        endNs.store(now?now:1,std::memory_order_relaxed);bump(ends[why]);lastEnd.store(endNames[why],std::memory_order_relaxed);
        f_=Flight{};
    }
    // Fly-back: a returnMs blend from the current pose to the native holster pose (the hook blends), then end(why).
    void beginReturn(End why,std::uint64_t now) noexcept {
        const int st=currentState();if(st==StateIdle||st==StateReturning)return;
        if(!config.returnMs){end(why,now);return;}
        M4 m{};if(!evalModel(arc_,now,m)){end(why,now);return;}
        arc_.lastModel=m;arc_.returnStartNs=now;arc_.returnMs=config.returnMs;f_.returnWhy=why;
        if(f_.hand>=0)haptics::pulse(unsigned(f_.hand)&1u,.25f,30'000,now,haptics::SourceReturn);
        publish(StateReturning);state.store(StateReturning,std::memory_order_release);
    }
    // OpCatch executed (SelectItem issued): the frozen pose until the item is in the hand / the observer published it.
    void catching(int hand,bool inHand,std::uint64_t now) noexcept {
        const int st=currentState();if(st!=StateFlying&&st!=StateResting)return;
        f_.preCatch=st;f_.catchStartNs=now;f_.catchHand=hand;
        M4 m{};freeze(now,&m);
        catchHand.store(hand,std::memory_order_relaxed);catchInHand.store(inHand,std::memory_order_relaxed);
        publish(StateCatching);state.store(StateCatching,std::memory_order_release);
        if(hand==f_.hand)bump(catchesSameHand);else bump(catchesOtherHand);
    }
    void noteInHand(bool inHand) noexcept {catchInHand.store(inHand,std::memory_order_relaxed);}
    // The catch did not land (refused before / timed out after SelectItem): the flight resumes from the frozen pose.
    void catchRefused(std::uint64_t now) noexcept {
        if(currentState()!=StateCatching)return;
        const int back=f_.preCatch==StateResting?StateResting:StateFlying;
        Segment& s=arc_.seg[0];s.t0=now;
        if(back==StateFlying){s.v=f_.frozenV;s.w=f_.frozenW;s.g=math::mul(f_.up,config.gravity()*f_.upm);f_.lastProbeNs=now;f_.lastP=s.p;}
        arc_.switchNs=0;catchHand.store(-1,std::memory_order_relaxed);catchInHand.store(false,std::memory_order_relaxed);
        publish(back);state.store(back,std::memory_order_release);
    }
    // Holsters poll (main thread, every successful sample while busy).
    void observe(const Facts& x,std::uint64_t now) noexcept {
        const int st=currentState();if(st==StateIdle)return;
        if(!x.valid){end(EndNoWeapon,now);return;}
        if(!active()){if(killed.load(std::memory_order_relaxed))end(EndKill,now);else beginReturn(EndMenuOff,now);return;}
        if(!x.slotItem||x.slotItem!=arc_.item){
            if(x.handItem==arc_.item&&arc_.item){if(st==StateCatching){f_.inHandSeen=true;noteInHand(true);return;}end(EndGameDraw,now);return;}
            end(EndSlotChanged,now);return;}
        if(x.slotWeapon!=arc_.weapon){end(EndNoWeapon,now);return;}
        if(x.handItem==arc_.item){if(st==StateCatching){f_.inHandSeen=true;noteInHand(true);return;}end(EndGameDraw,now);return;}
        if(!x.context&&st!=StateCatching){beginReturn(EndReturnContext,now);return;}
    }
    void observeInvalid(std::uint64_t now) noexcept {if(busy())end(EndNoWeapon,now);}
    // ModuleThrow tick: the flight, timeouts, the catch target.
    void tick(std::uint64_t now) noexcept {
        const int st=currentState();if(st==StateIdle)return;
        bump(flightTicks);
        const float dtTick=f_.lastTickNs&&now>f_.lastTickNs?float(double(now-f_.lastTickNs)*1e-9):.016f;f_.lastTickNs=now;
        if(st==StateReturning){if(now>=arc_.returnStartNs+std::uint64_t(arc_.returnMs)*1'000'000ull)end(f_.returnWhy,now);return;}
        if(st==StateCatching){
            const auto ow=ownerWeaponFn.load(std::memory_order_acquire);std::uintptr_t o=0;if(ow){try{o=ow();}catch(...){o=0;}}
            if(f_.inHandSeen&&(o==arc_.weapon||!ow)){end(EndCaught,now);return;}
            if(now-f_.catchStartNs>config.catchingNs){
                if(f_.inHandSeen){end(EndCaught,now);return;}
                bump(catchTimeouts);catchRefused(now);}
            return;
        }
        if(arc_.switchNs&&now>=arc_.switchNs){arc_.seg[0]=arc_.seg[1];arc_.switchNs=0;
            if(f_.pendingRest){f_.pendingRest=false;arc_.state=StateResting;f_.restNs=now;publish(StateResting);state.store(StateResting,std::memory_order_release);}
            else publish(StateFlying);}
        const int st2=currentState();
        // player distance (and the no-probe ground)
        V3 capsule{};bool haveCapsule=false;
        if(const auto pf=playerFn.load(std::memory_order_acquire)){try{haveCapsule=pf(f_.actor,capsule)&&math::finite(capsule);}catch(...){haveCapsule=false;}}
        V3 centre=centreAt(now);
        if(haveCapsule){const float d=math::length(math::sub(centre,capsule))/f_.upm;atomicMax(maxDistM,d);
            if(d>config.distM){beginReturn(EndReturnDistance,now);return;}}
        // absolute safety nets: total age, deep under the player's feet (the capsule origin = the feet)
        if(config.totalS>0.f&&now>f_.launchNs&&now-f_.launchNs>secondsNs(config.totalS)){beginReturn(EndReturnTimeout,now);return;}
        if(haveCapsule&&config.belowM>0.f&&math::dot(math::sub(centre,capsule),f_.up)/f_.upm<-config.belowM){beginReturn(EndReturnBelow,now);return;}
        if(st2==StateResting){
            if(now-f_.restNs>std::uint64_t(double(config.restS)*1e9)){beginReturn(EndReturnRest,now);return;}
            publishTarget(now,centre,{});return;
        }
        // FLYING
        if(now-f_.launchNs>std::uint64_t(double(config.flyMaxS)*1e9)){beginReturn(EndReturnFly,now);return;}
        const float height=(math::dot(centre,f_.up)-f_.releaseHeight)/f_.upm;atomicMax(maxHeightM,height);
        if(height<-60.f){beginReturn(EndLostFall,now);return;}
        { // Movement watchdogs. stillS without moving stillM = fly back; after an impact, stuckS within stuckM = lie here
            using math::length;using math::sub;
            if(!f_.stillNs||length(sub(centre,f_.stillP))/f_.upm>config.stillM){f_.stillP=centre;f_.stillNs=now;}
            else if(config.stillS>0.f&&now>f_.stillNs&&now-f_.stillNs>secondsNs(config.stillS)){beginReturn(EndReturnStill,now);return;}
            if(!f_.stuckNs||length(sub(centre,f_.stuckP))/f_.upm>config.stuckM){f_.stuckP=centre;f_.stuckNs=now;}
            else if(f_.impacted&&!arc_.switchNs&&config.stuckS>0.f&&now>f_.stuckNs&&now-f_.stuckNs>secondsNs(config.stuckS)){
                settleHere(now,centre);bump(stuckRests);publishTarget(now,centre,{});return;}
        }
        if(!arc_.switchNs){
            rebase(now,dtTick);
            pull(now,dtTick); // The magnet (before the probe: the look-ahead sees the steered path)
            const std::uint64_t look=std::uint64_t(std::fmin(std::fmax(double(dtTick)*1.5,.010),.060)*1e9);
            collide(now,now+look,haveCapsule,capsule);
        }
        if(currentState()==StateFlying||currentState()==StateResting)publishTarget(now,centreAt(now),velocityAt(now));
    }
    std::uintptr_t takeRefresh() noexcept {const auto r=pendingRefresh_;pendingRefresh_=0;return r;}
    const Arc& arc() const noexcept {return arc_;}
    int hand() const noexcept {return f_.hand;}
private:
    struct Flight {
        float upm{1};V3 up{0,1,0},barrel{0,0,-1},gunUp{0,1,0};std::uint32_t actorGuid{};std::uintptr_t actor{};
        std::uint64_t launchNs{},lastTickNs{},lastProbeNs{},restNs{},catchStartNs{};V3 lastP{},lastP0{};float releaseHeight{};
        unsigned bounces{};bool impacted{},pendingRest{},renderForced{},inHandSeen{};int hand{-1},slot{-1},preCatch{StateIdle},catchHand{-1};
        V3 frozenV{},frozenW{};End returnWhy{EndReturnFly};
        std::array<bool,2> away{},pulling{},spent{}; // Magnet: the gun has been > pullAway from hand h / an approach to h is being steered /
        std::array<std::uint64_t,2> pullStartNs{};    // that approach used its pullMaxNs (released until the gun leaves pullRadius)
        float pullTotalS{};                           // steering time of this flight (pullTotalS cap: never a hover)
        V3 stillP{},stuckP{};std::uint64_t stillNs{},stuckNs{};unsigned tiny{}; // Movement watchdogs, tiny-bounce run
    };
    Arc arc_{};Flight f_{};std::uintptr_t pendingRefresh_{};
    void publish(int st) noexcept {arc_.state=std::uint32_t(st);arcBox.store(arc_);}
    static std::uint64_t secondsNs(float s) noexcept {return std::uint64_t(double(s)*1e9);}
    // Lying on its side on a surface with normal n: the gun's side axis (barrel x grip-down) along +-n, the barrel's heading kept.
    M3 lyingOn(const M3& r,V3 n) const noexcept {
        using namespace math;
        const V3 side=normalized(cross(f_.barrel,mul(f_.gunUp,-1.f)));
        V3 sideW=mulv(r,side);if(length(sideW)<.5f)sideW=n;
        const V3 target=dot(sideW,n)>=0?n:mul(n,-1.f);
        M3 rest=multiply(shortestArc(sideW,target),r);orthonormalize(rest);return rest;
    }
    // Hung / sliding on geometry after an impact -> it lies where it is (the world up as the surface normal).
    void settleHere(std::uint64_t now,V3 centre) noexcept {
        const M3 r=rotationAt(now);Segment s{};s.t0=now;s.p=centre;s.r=lyingOn(r,f_.up);
        arc_.fromR=r;arc_.fromP=centre;arc_.blendStartNs=now;arc_.blendMs=150;arc_.seg[0]=s;arc_.switchNs=0;
        f_.pendingRest=false;f_.restNs=now;f_.lastP=centre;f_.lastProbeNs=now;
        publish(StateResting);state.store(StateResting,std::memory_order_release);
    }
    const Segment& seg(std::uint64_t now) const noexcept {return (arc_.switchNs&&now>=arc_.switchNs)?arc_.seg[1]:arc_.seg[0];}
    V3 centreAt(std::uint64_t now) const noexcept {
        const auto& s=seg(now);const float dt=now>s.t0?std::fmin(float(double(now-s.t0)*1e-9),.5f):0.f;
        return math::add(math::add(s.p,math::mul(s.v,dt)),math::mul(s.g,.5f*dt*dt));}
    V3 velocityAt(std::uint64_t now) const noexcept {
        const auto& s=seg(now);const float dt=now>s.t0?std::fmin(float(double(now-s.t0)*1e-9),.5f):0.f;return math::add(s.v,math::mul(s.g,dt));}
    M3 rotationAt(std::uint64_t now) const noexcept {
        const auto& s=seg(now);const float dt=now>s.t0?std::fmin(float(double(now-s.t0)*1e-9),.5f):0.f;
        M3 r=math::multiply(math::rotation(math::mul(s.w,dt)),s.r);math::orthonormalize(r);return r;}
    // Re-base segment 0 at now (keeps the hook's dt small; linear drag and angular damping per tick).
    void rebase(std::uint64_t now,float dtTick) noexcept {
        Segment s=arc_.seg[0];const V3 p=centreAt(now);const V3 v=velocityAt(now);const M3 r=rotationAt(now);
        s.t0=now;s.p=p;s.v=math::mul(v,std::exp(-config.drag*dtTick));s.r=r;s.w=math::mul(s.w,std::exp(-config.angularDamping*dtTick));
        arc_.seg[0]=s;arc_.blendMs=0;publish(StateFlying);
    }
    // The magnet: the nearest free hand within pullRadius (the throwing hand only after pullGraceNs AND once the gun
    // was > pullAway from it, so a toss is never sucked back into the releasing palm) bends the velocity towards itself: approach speed
    // 6/s x distance (>= 0.3 m/s, never faster than the flight), rate pullRate x (0.35 + 0.65 closeness) (x1.0 .. x0.75 closing .. open hand),
    // and the gravity fades with the closeness down to pullMinGravity (the gun floats into the palm). One approach steers at most
    // pullMaxNs (an open hand that does not close lets it drop; re-armed once the gun left pullRadius). Not pulled = full gravity again.
    void pull(std::uint64_t now,float dt) noexcept {
        using namespace math;
        Segment& s=arc_.seg[0];const V3 fullG=mul(f_.up,config.gravity()*f_.upm);
        const auto noPull=[&]{if(s.g[0]!=fullG[0]||s.g[1]!=fullG[1]||s.g[2]!=fullG[2]){s.g=fullG;publish(StateFlying);}};
        if(!config.pull||!(config.pullRate>0.f)||!(dt>0.f)||!(config.pullRadius>0.f)||f_.pullTotalS>=config.pullTotalS){noPull();return;}
        CatchHands hb{};if(!handsBox.load(hb)||!hb.mask||!hb.referenceSpace||(hb.ns>=now?hb.ns-now:now-hb.ns)>100'000'000ull){noPull();return;}
        const auto mf=mapFn.load(std::memory_order_acquire);Map m{};bool ok=false;if(mf){try{ok=mf(now,m);}catch(...){ok=false;}}
        if(!ok||m.referenceSpace!=hb.referenceSpace){noPull();return;}
        const V3 c=centreAt(now);int best=-1;float bestD=config.pullRadius;V3 bestW{};
        for(int h=0;h<2;++h){
            if(!(hb.mask>>unsigned(h)&1u))continue;
            const V3 w=xform(m.localToWorld,hb.pos[std::size_t(h)]);if(!finite(w))continue;
            const float d=length(sub(w,c))/f_.upm;if(!std::isfinite(d))continue;
            if(d>config.pullAway)f_.away[std::size_t(h)]=true;
            if(d>config.pullRadius){f_.spent[std::size_t(h)]=false;f_.pulling[std::size_t(h)]=false;}
            if(f_.spent[std::size_t(h)])continue;
            if(f_.pulling[std::size_t(h)]&&now>f_.pullStartNs[std::size_t(h)]+config.pullMaxNs){f_.spent[std::size_t(h)]=true;f_.pulling[std::size_t(h)]=false;continue;}
            if(h==f_.hand&&(!f_.away[std::size_t(h)]||now<f_.launchNs+config.pullGraceNs))continue;
            if(d<bestD){bestD=d;best=h;bestW=w;}
        }
        if(best<0){noPull();return;}
        const float closeness=1.f-bestD/config.pullRadius,grip=std::fmin(1.f,std::fmax(0.f,hb.grip[std::size_t(best)]));
        const float k=1.f-std::exp(-config.pullRate*(.35f+.65f*closeness)*(.75f+.25f*grip)*dt);
        const V3 dir=bestD>1e-3f?normalized(sub(bestW,c)):V3{0,0,0};
        const float speed=length(s.v)/f_.upm,want=std::fmin(std::fmax(bestD*6.f,.3f),std::fmax(speed,1.f))*f_.upm;
        s.v=add(s.v,mul(sub(mul(dir,want),s.v),k));s.g=mul(fullG,std::fmax(config.pullMinGravity,1.f-closeness));
        if(!finite(s.v))s.v={};
        publish(StateFlying);bump(magnetPullTicks);
        if(!f_.pulling[std::size_t(best)]){f_.pulling[std::size_t(best)]=true;f_.pullStartNs[std::size_t(best)]=now;bump(magnetPulls);}
        f_.pullTotalS+=dt;
    }
    void freeze(std::uint64_t now,M4* model) noexcept {
        f_.frozenV=velocityAt(now);f_.frozenW=seg(now).w;
        Segment s{};s.t0=now;s.p=centreAt(now);s.r=rotationAt(now);
        if(model)*model=math::compose(s.r,s.p,arc_.scale,arc_.centre);
        arc_.seg[0]=s;arc_.switchNs=0;arc_.blendMs=0;f_.pendingRest=false;
    }
    void publishTarget(std::uint64_t now,V3 centre,V3 velocity) noexcept {
        const auto mf=mapFn.load(std::memory_order_acquire);Map m{};bool ok=false;
        if(mf){try{ok=mf(now,m);}catch(...){ok=false;}}
        M4 inv{};Target t{};t.state=std::uint32_t(currentState());t.ns=now;t.slot=f_.slot;t.throwHand=f_.hand;t.throwNs=throwNs.load(std::memory_order_relaxed);
        t.generation=std::uint32_t(arc_.generation);t.radius=config.catchRadius;t.groundRadius=config.groundRadius;t.groundHeight=config.groundHeight;
        if(!ok||!m.referenceSpace||!math::inverseUniform(m.localToWorld,inv)){bump(mapFailures);t.referenceSpace=0;targetBox.store(t);return;}
        t.pos=math::xform(inv,centre);t.vel=math::xformDir(inv,velocity);t.referenceSpace=m.referenceSpace;
        if(!math::finite(t.pos)||!math::finite(t.vel)){bump(mapFailures);t.referenceSpace=0;}
        else bump(targetsPublished);
        targetBox.store(t);
    }
    // Probe the path [lastProbe .. tB] (tB = now + one frame look-ahead: the hook never shows the gun inside a surface).
    void collide(std::uint64_t now,std::uint64_t tB,bool haveCapsule,V3 capsule) noexcept {
        using namespace math;
        const V3 a=f_.lastP,b=centreAt(tB);const std::uint64_t tA=f_.lastProbeNs<now?f_.lastProbeNs:now;
        const V3 d=sub(b,a);const float len=length(d);
        f_.lastP=centreAt(now);f_.lastProbeNs=now;
        if(!(len>1e-4f*f_.upm))return;
        bool blocked=false;Hit h{};bool cast=false;
        if(const auto pf=probeFn.load(std::memory_order_acquire)){
            const auto t0=nowNs();try{cast=pf(f_.actorGuid,a,b,blocked,h);}catch(...){cast=false;}
            const auto dt=nowNs()-t0;probeNsSum.fetch_add(dt,std::memory_order_relaxed);atomicMax(probeNsMax,dt);
            if(cast)bump(probes);else bump(probeFails);
            noProbe.store(false,std::memory_order_relaxed);
        }
        if(!cast){ // no probe API / failed cast: the player's capsule height is the ground (NO PROBE)
            bump(noProbeFrames);noProbe.store(true,std::memory_order_relaxed);
            if(!haveCapsule)return;
            const float ga=dot(a,f_.up)-dot(capsule,f_.up),gb=dot(b,f_.up)-dot(capsule,f_.up);
            if(!(ga>=0.f&&gb<0.f))return;
            const float f=ga/(ga-gb);blocked=true;h.point=add(a,mul(d,f));h.normal=f_.up;h.normalValid=true;
        }
        if(!blocked)return;
        bump(hits);
        const float frac=std::fmin(1.f,std::fmax(0.f,length(sub(h.point,a))/len));
        std::uint64_t tHit=tA+std::uint64_t(double(tB-tA)*double(frac));if(tHit<now)tHit=now;
        const V3 dir=mul(d,1.f/len);V3 n=h.normal;
        if(!h.normalValid||!finite(n)||std::fabs(length(n)-1.f)>.1f){ // normal unusable: a floor below the point? else straight back
            bump(normalInvalid);n=mul(dir,-1.f);
            if(cast){const auto pf=probeFn.load(std::memory_order_acquire);bool down=false;Hit g{};
                const V3 from=add(h.point,mul(f_.up,.3f*f_.upm)),to=sub(h.point,mul(f_.up,.1f*f_.upm));
                bool ok=false;try{ok=pf&&pf(f_.actorGuid,from,to,down,g);}catch(...){ok=false;}
                if(ok){bump(probes);if(down)n=f_.up;}else bump(probeFails);}
        }
        n=normalized(n);if(dot(n,dir)>0)n=mul(n,-1.f);
        bounce(tHit,h.point,n,now);
    }
    void bounce(std::uint64_t tHit,V3 point,V3 n,std::uint64_t now) noexcept {
        using namespace math;
        const V3 v=velocityAt(tHit);const M3 r=rotationAt(tHit);const V3 w=seg(tHit).w;
        const float vn=dot(v,n);const V3 vN=mul(n,vn),vT=sub(v,vN);
        V3 v2=sub(mul(vT,1.f-config.friction),mul(vN,config.bounce));
        const V3 w2=add(mul(w,.6f),mul(cross(n,vT),1.f/(.06f*f_.upm)));
        V3 p2=add(point,mul(n,.025f*f_.upm));
        ++f_.bounces;bump(bounces);
        Landing L{};L.point=point;L.normal=n;L.ns=tHit;lastLandingBox.store(L);
        if(!f_.impacted){f_.impacted=true;for(unsigned h=0;h<2;++h)haptics::pulse(h,.15f,12'000,now,haptics::SourceOther);}
        const float upDot=dot(n,f_.up);const bool floor=upDot>.55f;
        Segment s{};s.t0=tHit;s.p=p2;s.r=r;
        bool settle=floor&&(length(v2)<.8f*f_.upm||f_.bounces>=4);
        // a slow bounce on a slope (planks at an angle, a ramp) lies there; a run of tiny bounces (jitter on thin /
        // steep geometry, an invalid normal) lies there too - on the surface when it faces up at all, else on the world up at the contact
        if(length(v)<config.tinyBounceSpeed*f_.upm)++f_.tiny;else f_.tiny=0;
        V3 nRest=n;
        if(!settle&&upDot>.2f&&length(v2)<.35f*f_.upm){settle=true;bump(slopeRests);}
        if(!settle&&config.tinyBounces&&f_.tiny>=config.tinyBounces){settle=true;bump(tinyRests);if(upDot<=.2f){nRest=f_.up;p2=add(point,mul(nRest,.025f*f_.upm));}}
        if(!settle&&f_.bounces>=4){beginReturn(EndReturnFly,now);return;} // a steep surface keeps bouncing: fly back
        if(settle){
            // lying on its side: the gun's side axis (barrel x grip-down) along +-n, the barrel's heading kept
            s.r=lyingOn(r,nRest);s.p=p2;s.v={};s.w={};s.g={};
            arc_.fromR=r;arc_.fromP=p2;arc_.blendStartNs=tHit;arc_.blendMs=150;
        }else{s.v=v2;s.w=w2;s.g=mul(f_.up,config.gravity()*f_.upm);}
        f_.lastP=p2;f_.lastProbeNs=tHit;
        if(tHit<=now){arc_.seg[0]=s;arc_.switchNs=0;
            if(settle){f_.restNs=now;publish(StateResting);state.store(StateResting,std::memory_order_release);}else publish(StateFlying);}
        else{arc_.seg[1]=s;arc_.switchNs=tHit;f_.pendingRest=settle;publish(StateFlying);}
    }
};
inline Machine machine{};
// Main thread (mainthread ModuleThrow).
inline void tickThunk(const mainthread::Frame& f) noexcept {try{machine.tick(f.now);}catch(...){}}

// ---- startup: debug.rdrvr.vr.throw 0 (kill) + tuning properties (read once; any thread before the hooks) ----
inline std::string property(const char* name) noexcept {
#if defined(__ANDROID__)
    char value[PROP_VALUE_MAX]{};const int saved=errno;const int n=__system_property_get(name,value);errno=saved;
    return n>0?std::string(value,std::size_t(n)):std::string();
#else
    (void)name;return {};
#endif
}
inline void startup() noexcept {
    if(started.exchange(true))return;
    const auto v=property("debug.rdrvr.vr.throw");killed.store(!v.empty()&&v[0]=='0',std::memory_order_relaxed);
    const auto real=[](const char* key,float& out,float lo,float hi){const auto s=property(key);if(s.empty())return;char* e{};const float x=std::strtof(s.c_str(),&e);
        if(e!=s.c_str()&&std::isfinite(x)&&x>=lo&&x<=hi)out=x;};
    real("debug.rdrvr.vr.throw.minspeed",config.minSpeed,.5f,10.f);real("debug.rdrvr.vr.throw.release",config.releaseSpeed,.2f,10.f);
    real("debug.rdrvr.vr.throw.gain",config.gain,.2f,4.f);
    // .gravity <0.2..1.5> (0.6), .groundcatch <m> (0.35), .pull 0 (no magnet), .pullradius <m> (0.45), .pullrate <1/s> (10), .grace <ms> (150)
    real("debug.rdrvr.vr.throw.gravity",config.gravityScale,.2f,1.5f);real("debug.rdrvr.vr.throw.groundcatch",config.groundRadius,.05f,.8f);
    real("debug.rdrvr.vr.throw.pullradius",config.pullRadius,.05f,1.f);real("debug.rdrvr.vr.throw.pullrate",config.pullRate,0.f,40.f);
    {const auto c=property("debug.rdrvr.vr.throw.pull");if(!c.empty())config.pull=c[0]!='0';}
    {float g=float(double(config.graceNs)/1e6);real("debug.rdrvr.vr.throw.grace",g,0.f,1000.f);config.graceNs=std::uint64_t(double(g)*1e6);}
    real("debug.rdrvr.vr.throw.spin",config.spin,0.f,3.f);real("debug.rdrvr.vr.throw.catch",config.catchRadius,.05f,.6f);
    real("debug.rdrvr.vr.throw.rest",config.restS,1.f,600.f);real("debug.rdrvr.vr.throw.flymax",config.flyMaxS,.5f,30.f);
    real("debug.rdrvr.vr.throw.dist",config.distM,2.f,200.f);real("debug.rdrvr.vr.throw.bounce",config.bounce,0.f,1.f);
    real("debug.rdrvr.vr.throw.friction",config.friction,0.f,1.f);
    // .total <s> (12, 0 = off), .still <s> (3, 0 = off), .below <m> (2, 0 = off), .stuck <s> (0.6, 0 = off), .tiny <n> (3, 0 = off)
    real("debug.rdrvr.vr.throw.total",config.totalS,0.f,120.f);real("debug.rdrvr.vr.throw.still",config.stillS,0.f,60.f);
    real("debug.rdrvr.vr.throw.below",config.belowM,0.f,50.f);real("debug.rdrvr.vr.throw.stuck",config.stuckS,0.f,10.f);
    {float t=float(config.tinyBounces);real("debug.rdrvr.vr.throw.tiny",t,0.f,10.f);config.tinyBounces=unsigned(t);}
    {float s=float(config.slots);real("debug.rdrvr.vr.throw.slots",s,0.f,3.f);config.slots=unsigned(s)&3u;}
    {float r=float(config.returnMs);real("debug.rdrvr.vr.throw.returnms",r,0.f,2000.f);config.returnMs=unsigned(r);}
    {const auto c=property("debug.rdrvr.vr.throw.closedcatch");if(!c.empty())config.closedCatch=c[0]!='0';}
    {const auto c=property("debug.rdrvr.vr.throw.physics");config.physics=!c.empty()&&c[0]=='1';} // no physics mode is built: reported only
}

// ---- menu row THROW WEAPON note (panel glyphs) ----
inline const char* statusNote() noexcept {
    switch(currentState()){
    case StateFlying:case StateCatching:case StateReturning:return noProbe.load(std::memory_order_relaxed)?"NO PROBE":"FLYING";
    case StateResting:return "ON GROUND";default:return "READY";}
}

// "vr":{"throw":<this>} (any thread)
inline std::string report() {
    std::string out;out.reserve(4096);char b[1536];const auto now=nowNs();const auto t=throwNs.load();
    const auto u=[](const std::atomic<std::uint64_t>& a){return (unsigned long long)a.load(std::memory_order_relaxed);};
    std::snprintf(b,sizeof b,"{\"enabled\":%s,\"killed\":%s,\"menu\":%s,\"mode\":\"VISUAL\",\"status\":\"%s\",\"state\":\"%s\",\"slot\":%d,\"item\":%s,"
        "\"weapon\":%s,\"generation\":%u,\"throwHand\":%d,\"catchHand\":%d,\"ageMs\":%.0f,\"config\":{\"minSpeed\":%.2f,\"gain\":%.2f,\"spin\":%.2f,"
        "\"catch\":%.2f,\"rest\":%.0f,\"flyMax\":%.1f,\"dist\":%.0f,\"bounce\":%.2f,\"friction\":%.2f,\"slots\":%u,\"closedCatch\":%s,\"returnMs\":%u,"
        "\"releaseSpeed\":%.2f,\"windowMs\":%.0f,\"gravityScale\":%.2f,\"groundCatch\":%.2f,\"groundHeight\":%.2f,\"pull\":%s,\"pullRadius\":%.2f,"
        "\"pullRate\":%.1f,\"graceMs\":%.0f,\"totalS\":%.1f,\"stillS\":%.1f,\"stillM\":%.2f,\"belowM\":%.1f,\"stuckS\":%.2f,\"tinyBounces\":%u},",
        active()?"true":"false",killed.load()?"true":"false",menuOn.load()?"true":"false",killed.load()?"PROP":statusNote(),
        stateNames[unsigned(currentState())%stateNames.size()],slot.load(),item.load()?"true":"false",weapon.load()?"true":"false",generation.load(),
        throwHand.load(),catchHand.load(),busy()&&t&&now>=t?double(now-t)/1e6:-1.0,double(config.minSpeed),double(config.gain),double(config.spin),
        double(config.catchRadius),double(config.restS),double(config.flyMaxS),double(config.distM),double(config.bounce),double(config.friction),
        config.slots,config.closedCatch?"true":"false",config.returnMs,double(config.releaseSpeed),double(config.windowNs)/1e6,
        double(config.gravityScale),double(config.groundRadius),double(config.groundHeight),config.pull?"true":"false",double(config.pullRadius),
        double(config.pullRate),double(config.graceNs)/1e6,double(config.totalS),double(config.stillS),double(config.stillM),double(config.belowM),
        double(config.stuckS),config.tinyBounces);out+=b;
    std::snprintf(b,sizeof b,"\"throws\":{\"requested\":%llu,\"started\":%llu,\"promotes\":%llu,\"ackTimeouts\":%llu,\"holsterFallbacks\":%llu,\"lastRefusal\":\"%s\",\"refused\":{",
        u(throwsRequested),u(throwsStarted),u(promotes),u(ackTimeouts),u(holsterFallbacks),lastRefusal.load());out+=b;
    for(unsigned i=0;i<RefuseCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",refuseNames[i],u(refusals[i]));out+=b;}
    out+="}},\"guards\":{";
    for(unsigned i=0;i<GuardCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",guardNames[i],u(guards[i]));out+=b;}
    {Release r{};const bool have=lastReleaseBox.load(r);
     std::snprintf(b,sizeof b,"},\"release\":{\"last\":{\"valid\":%s,\"hand\":%d,\"speedPeak\":%.2f,\"speedRelease\":%.2f,\"spin\":%.2f,\"gain\":%.2f,"
        "\"samples\":%u,\"peakAgeMs\":%.1f,\"v\":[%.2f,%.2f,%.2f],\"w\":[%.2f,%.2f,%.2f],\"tape\":%s,\"redirected\":%s,\"downPeak\":%.2f},\"speedMax\":%.2f,"
        "\"redirects\":%llu,\"tape\":{\"pushed\":%llu,\"used\":%llu,\"fallback\":%llu}},",
        have?"true":"false",r.hand,double(r.speedPeak),double(r.speedRelease),double(math::length(r.w)),double(config.gain),r.samples,
        have&&r.releaseNs>=r.peakNs?double(r.releaseNs-r.peakNs)/1e6:-1.0,double(r.v[0]),double(r.v[1]),double(r.v[2]),double(r.w[0]),double(r.w[1]),double(r.w[2]),
        (r.flags&releaseFromTape)?"true":"false",(r.flags&releaseRedirected)?"true":"false",double(r.downPeak),
        double(speedMax.load()),u(redirects),u(tapePushed),u(tapeUsed),u(tapeFallback));out+=b;}
    {Landing L{};const bool have=lastLandingBox.load(L);const auto pr=probes.load();
     std::snprintf(b,sizeof b,"\"flight\":{\"ticks\":%llu,\"probes\":%llu,\"probeFails\":%llu,\"probeUsAvg\":%.1f,\"probeUsMax\":%.1f,\"hits\":%llu,"
        "\"bounces\":%llu,\"normalInvalid\":%llu,\"noProbeFrames\":%llu,\"maxDistM\":%.2f,\"maxHeightM\":%.2f,\"targets\":%llu,\"mapFailures\":%llu,"
        "\"landed\":%s,\"lastLanding\":[%.2f,%.2f,%.2f],\"lastNormal\":[%.2f,%.2f,%.2f],\"stuckRests\":%llu,\"tinyRests\":%llu,\"slopeRests\":%llu},",
        u(flightTicks),(unsigned long long)pr,u(probeFails),pr?double(probeNsSum.load())/double(pr)/1e3:0.0,double(probeNsMax.load())/1e3,u(hits),u(bounces),
        u(normalInvalid),u(noProbeFrames),double(maxDistM.load()),double(maxHeightM.load()),u(targetsPublished),u(mapFailures),have?"true":"false",
        double(L.point[0]),double(L.point[1]),double(L.point[2]),double(L.normal[0]),double(L.normal[1]),double(L.normal[2]),
        u(stuckRests),u(tinyRests),u(slopeRests));out+=b;}
    std::snprintf(b,sizeof b,"\"pose\":{\"calls\":%llu,\"placed\":%llu,\"catching\":%llu,\"returning\":%llu,\"fixMeasured\":%llu,\"fixApplied\":%llu,"
        "\"renderForced\":%llu,\"renderRestored\":%llu,\"declines\":{",u(poseCalls),u(posePlaced),u(poseCatching),u(poseReturning),u(fixMeasured),u(fixApplied),
        u(renderForcedCalls),u(renderRestored));out+=b;
    for(unsigned i=0;i<DeclineCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",declineNames[i],u(poseDeclines[i]));out+=b;}
    std::snprintf(b,sizeof b,"}},\"catches\":{\"posted\":%llu,\"air\":%llu,\"closedHand\":%llu,\"ground\":%llu,\"sameHand\":%llu,\"otherHand\":%llu,"
        "\"timeouts\":%llu,\"magnetTicks\":%llu,\"magnetPulls\":%llu,\"magnetPullTicks\":%llu,\"refused\":{",u(catchesPosted),u(catchesAir),u(catchesClosed),u(catchesGround),u(catchesSameHand),
        u(catchesOtherHand),u(catchTimeouts),u(magnetTicks),u(magnetPulls),u(magnetPullTicks));out+=b;
    for(unsigned i=0;i<CatchRefuseCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",catchRefuseNames[i],u(catchRefusals[i]));out+=b;}
    out+="}},\"ends\":{";
    for(unsigned i=0;i<EndCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",endNames[i],u(ends[i]));out+=b;}
    std::snprintf(b,sizeof b,"},\"lastEnd\":\"%s\",\"physics\":{\"requested\":%s,\"implemented\":false}}",lastEnd.load(),config.physics?"true":"false");out+=b;
    return out;
}
} // namespace rdrvr::vr::throwing
