#pragma once
// MELEE: pure math of the physical VR melee (no engine, no threads).
//
// 1. Gothic II VR port (Gothic II VR source kit, "SK"), kept line-for-line where possible:
//    - BodyFrame + Swing        = SK/vr/vrinteractionmath.h:174-182, 206-253 (0.0.55: contactTravel/contactReady, stroke machine)
//    - meleeContact + Guard     = SK/vr/vrcombatmath.h:89-122 (yaw ellipsoid; parry guard, not wired)
//    - bladeStep                = SK/vr/vrgameplay.cpp:729-766 (occupancy +5 cm exit margin = the ONLY blade debounce, swept blade
//                                 points "previous -> current", fall back to the overlap target, contact() closes the stroke)
//    - fistStep                 = SK/vr/vrgameplay.cpp:691-704 (1.6 m/s stroke machine, 280 ms cool-down, 180 ms window, 4 rays)
//    Deviations: time is in MICROseconds (Gothic: ms; 72 Hz tape samples are 13.9 ms apart, ms rounding
//    would be a 7% speed error) - every Gothic constant is scaled 1:1000; world distances use `units` per metre everywhere Gothic
//    hard-coded centimetres (10 cm sample step, 5 cm exit margin, 4 cm fist rays) so Gothic's own scenarios run with units=100.
//    The two-handed weapon branch (support grip) is not ported: RDR1 melee weapons are one-handed (knife / torch).
// 2. RDR1 geometry: segment/capsule closest approach, first entry along a swept point path, body capsules from bone positions.
// 3. Damage / reaction policy and the hlthMsgInjure byte layout (MeleeMachine 0x1e41f1c..0x1e4207c).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rdrvr::vr::melee::math {

// ---- vector (Tempest::Vec3 subset) ----
struct Vec3 {
    float x{},y{},z{};
    constexpr Vec3()=default;
    constexpr Vec3(float a,float b,float c):x(a),y(b),z(c){}
    float length() const noexcept {return std::sqrt(x*x+y*y+z*z);}
    friend constexpr Vec3 operator+(Vec3 a,Vec3 b) noexcept {return {a.x+b.x,a.y+b.y,a.z+b.z};}
    friend constexpr Vec3 operator-(Vec3 a,Vec3 b) noexcept {return {a.x-b.x,a.y-b.y,a.z-b.z};}
    friend constexpr Vec3 operator-(Vec3 a) noexcept {return {-a.x,-a.y,-a.z};}
    friend constexpr Vec3 operator*(Vec3 a,float s) noexcept {return {a.x*s,a.y*s,a.z*s};}
    friend constexpr Vec3 operator*(float s,Vec3 a) noexcept {return {a.x*s,a.y*s,a.z*s};}
    friend constexpr Vec3 operator/(Vec3 a,float s) noexcept {return {a.x/s,a.y/s,a.z/s};}
    Vec3& operator+=(Vec3 b) noexcept {x+=b.x;y+=b.y;z+=b.z;return *this;}
};
inline float dot(Vec3 a,Vec3 b) noexcept {return a.x*b.x+a.y*b.y+a.z*b.z;}
inline Vec3 cross(Vec3 a,Vec3 b) noexcept {return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
inline bool finite(Vec3 p) noexcept {return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
inline Vec3 normalized(Vec3 v,Vec3 fallback={0,0,1}) noexcept {const float l=v.length();return std::isfinite(l)&&l>0.0001f?v/l:fallback;}

// ===================================================== Gothic port =====================================================
// SK/vr/vrinteractionmath.h:174-182. update() takes the head position and its forward axis (Gothic: axis(headWorld,2)).
struct BodyFrame {
    Vec3 head{},forward{0,0,1},right{1,0,0};
    void update(Vec3 headPosition,Vec3 headForwardAxis) noexcept {
        head=headPosition;auto f=headForwardAxis;f.y=0;
        if(f.length()>.15f)forward=normalized(f);
        right=normalized(cross(Vec3(0,1,0),forward),{1,0,0});
    }
    Vec3 local(Vec3 world,float units) const noexcept {const auto d=(world-head)/units;return {dot(d,right),d.y,dot(d,forward)};}
};

// SK/vr/vrinteractionmath.h:206-253 (Gothic 0.0.55). nowUs = microseconds (Gothic: milliseconds), coolUntil +280 ms.
// Body-local and raw tracking samples reject locomotion and camera discontinuities.
// Each stroke can contact once; a return stroke rearms without a stationary hold.
struct Swing {
    Vec3 previousTracking{},previousGrip{},strokeDirection{};float strokeTravel=0,reverseTravel=0,contactTravel=0;
    Vec3 previous{},previousBodyForward{0,0,1},previousHead{};
    std::uint64_t stamp=0,coolUntil=0;bool valid=false,armed=false,active=false,contactReady=false;
    float speed=0;bool continuous=false;
    float travel=0; // Travel of the last update (telemetry only)
    void reset() noexcept {valid=armed=active=continuous=contactReady=false;speed=strokeTravel=reverseTravel=contactTravel=0;strokeDirection={};coolUntil=0;}
    void contact() noexcept {active=false;}
    bool update(Vec3 tip,const BodyFrame& body,float units,std::uint64_t nowUs,bool allowed,float threshold,const Vec3* tracking=nullptr,
                const Vec3* gripTracking=nullptr) noexcept {
        const Vec3 local=body.local(tip,units);
        if(!allowed||!finite(local)||(tracking&&!finite(*tracking))||(gripTracking&&!finite(*gripTracking))){reset();return false;}
        const float dt=valid&&nowUs>stamp?float(nowUs-stamp)*1e-6f:0;
        const float jump=(body.head-previousHead).length()/units;
        const bool continuity=valid&&dt>=.001f&&dt<=.1f&&jump<.4f&&dot(body.forward,previousBodyForward)>.94f;
        const auto movement=tracking?*tracking-previousTracking:local-previous;
        travel=std::min((local-previous).length(),movement.length());
        speed=continuity?travel/dt:0;
        const float gripTravel=gripTracking?(*gripTracking-previousGrip).length():0.f;
        const bool gripContinuous=!gripTracking||(continuity&&gripTravel<.30f&&gripTravel/dt<15.f);
        continuous=continuity&&gripContinuous&&travel<(gripTracking?2.f:.5f)&&speed<(gripTracking?80.f:24.f);
        // Entry/exit collision state owns melee debouncing. A fresh entry must not depend
        // on the direction, cooldown or consumed animation phase of an earlier stroke.
        contactTravel=continuous&&speed>=.7f?std::min(contactTravel+travel,.2f):0.f;
        contactReady=continuous&&speed>=threshold&&(!tracking||contactTravel>=.035f);
        if(!continuous){armed=active=false;strokeTravel=reverseTravel=0;strokeDirection={};}
        else if(speed<.7f){
            active=false;armed=nowUs>=coolUntil;strokeTravel=reverseTravel=0;strokeDirection={};
        }else{
            const auto direction=normalized(movement);
            if(strokeDirection.length()<.01f){
                armed=true;strokeDirection=direction;
            }else if(dot(direction,strokeDirection)<-.25f){
                reverseTravel+=travel;
                if(reverseTravel>=.035f){
                    active=false;armed=true;strokeTravel=reverseTravel-travel;reverseTravel=0;strokeDirection=direction;
                }
            }else reverseTravel=0;
            if(armed)strokeTravel+=travel;
        }
        const bool strike=continuous&&armed&&speed>=threshold&&nowUs>=coolUntil&&(!tracking||strokeTravel>=.035f);
        if(strike){armed=false;active=true;coolUntil=nowUs+280'000;}
        if(tracking)previousTracking=*tracking;
        if(gripTracking)previousGrip=*gripTracking;
        previous=local;previousHead=body.head;previousBodyForward=body.forward;stamp=nowUs;valid=true;
        return strike;
    }
};

// SK/vr/vrcombatmath.h:89-101: finite segment against the oriented NPC ellipsoid, including an origin already inside.
// Gothic NPC model; RDR targets are bone capsules (below).
inline bool meleeContact(Vec3 from,Vec3 to,Vec3 center,Vec3 radius,float yaw,float& fraction) noexcept {
    if(!finite(from)||!finite(to)||!finite(center)||!finite(radius)||!std::isfinite(yaw)||radius.x<=0||radius.y<=0||radius.z<=0)return false;
    const float c=std::cos(yaw),s=std::sin(yaw);
    auto local=[&](Vec3 v){return Vec3((v.x*c+v.z*s)/radius.x,v.y/radius.y,(-v.x*s+v.z*c)/radius.z);};
    const auto o=local(from-center),d=local(to-from);
    const float cc=dot(o,o)-1.f;if(cc<=0){fraction=0;return true;}
    const float a=dot(d,d),b=dot(o,d),disc=b*b-a*cc;
    if(a<1e-12f||disc<0)return false;
    const float t=(-b-std::sqrt(disc))/a;
    if(t<0||t>1)return false;
    fraction=t;return true;
}
// SK/vr/vrcombatmath.h:102-122 (parry guard; nowUs); not wired.
struct Guard {
    Vec3 base{},tip{},head{},forward{0,0,1};float units=100;
    std::uint64_t until=0;int hand=-1;
    bool blocks(Vec3 attacker,std::uint64_t now) const noexcept {
        if(hand<0||hand>1||now>until||!finite(base)||!finite(tip)||!finite(head)||!finite(attacker)||!std::isfinite(units)||units<=0)return false;
        auto incoming=attacker-head;incoming.y=0;const float distance=incoming.length()/units;
        auto blade=(tip-base)/units;const float length=blade.length();
        if(distance<.05f||distance>3.f||length<.20f)return false;
        incoming=normalized(incoming);blade=blade/length;
        if(dot(incoming,normalized(Vec3(forward.x,0,forward.z)))<.25f||std::abs(dot(blade,incoming))>.45f||std::abs(blade.y)>.65f)return false;
        // Nearest point of the blade to the line of attack must cover the torso.
        const auto a=(base-head)/units,span=(tip-base)/units;
        const auto side=normalized(cross(Vec3(0,1,0),incoming));
        const float lateral=dot(a,side),slope=dot(span,side);
        const float t=std::abs(slope)>.001f?std::clamp(-lateral/slope,0.f,1.f):.5f;
        const auto p=a+span*t;
        return dot(p,incoming)>.05f&&dot(p,incoming)<1.f&&p.y>-.75f&&p.y<.25f&&std::abs(dot(p,side))<.3f;
    }
};

// ---- production loops (SK/vr/vrgameplay.cpp) ----
// A target query result. target = caller's victim index (-1 none), part = body part (capsule) index, t = entry fraction along
// the queried segment, point = entry point.
struct Contact {int target{-1},part{-1};float t{};Vec3 point{};bool hit() const noexcept {return target>=0;}};
// World geometry of one sample (Gothic: the same world space as the swing; RDR1: the swing is measured in OpenXR LOCAL - raw tracking
// and head-relative body frame - while collisions run in the game world through the newest camera plan).
// head = Gothic body.head (clearPath origin), position = calibrated grip (clearPath head->grip, grip->blade point).
struct WorldPose {Vec3 base{},tip{},position{},head{};};
struct BladeHold {Swing swing;std::uint64_t strikeUntil{};Vec3 previousBase{},previousTip{};bool insideTarget{};};
struct BladeResult {
    bool previous{},entry{},rejectedEntry{},wallBlocked{},struck{};Contact overlap{},hit{};
};
// SK/vr/vrgameplay.cpp:729-766. swingTip/body/swingUnits/tracking/gripTracking feed Swing::update exactly as Gothic's
// (tip, body, units, &tracking, &gripTracking); w = world geometry; bladeRadius/sampleStep in metres, scaled by worldUnits
// (Gothic: 3 cm, 10 cm, 5 cm exit margin with units=100).
// overlapQuery(base,tip,radius) -> Contact (rayNpcMelee of the CURRENT blade, start inside counts);
// sweep(old,current,radius) -> Contact; clearPath(from,to) -> bool (world ray, Gothic clearPath).
template<class Overlap,class Sweep,class Clear>
inline BladeResult bladeStep(BladeHold& h,const WorldPose& w,Vec3 swingTip,const BodyFrame& body,float swingUnits,float worldUnits,
                             std::uint64_t nowUs,bool physicalCombat,float swingSpeed,const Vec3& tracking,const Vec3& gripTracking,
                             float bladeRadius,float sampleStep,Overlap&& overlapQuery,Sweep&& sweep,Clear&& clearPath) {
    BladeResult r{};
    const bool previous=h.swing.valid;r.previous=previous;
    h.swing.update(swingTip,body,swingUnits,nowUs,physicalCombat,swingSpeed,&tracking,&gripTracking);
    h.strikeUntil=h.swing.contactReady?nowUs+1:0;
    if(!physicalCombat||!previous||!h.swing.continuous||h.swing.speed<.7f)h.strikeUntil=0;
    // Track occupancy even below the swing threshold. Reversal inside a body is extraction, not entry.
    const Contact overlap=overlapQuery(w.base,w.tip,bladeRadius*worldUnits+(h.insideTarget?.05f*worldUnits:0.f));
    const bool entry=!h.insideTarget;r.entry=entry;r.overlap=overlap;
    if(entry&&overlap.hit()&&h.strikeUntil<nowUs&&previous)r.rejectedEntry=true;
    if(entry&&h.strikeUntil>=nowUs){
        if(!clearPath(w.head,w.position))r.wallBlocked=true;
        else{
            Contact hit{};
            const int samples=std::clamp(int(std::ceil((w.tip-w.base).length()/(sampleStep*worldUnits))),4,20);
            for(int k=0;k<=samples&&!hit.hit();++k){
                const float t=float(k)/float(samples);const auto old=h.previousBase+(h.previousTip-h.previousBase)*t;
                const auto current=w.base+(w.tip-w.base)*t;
                if(clearPath(w.position,current))hit=sweep(old,current,bladeRadius*worldUnits);
            }
            if(!hit.hit())hit=overlap;
            if(hit.hit()){r.hit=hit;r.struck=true;h.strikeUntil=0;h.swing.contact();}
        }
    }
    h.insideTarget=overlap.hit();
    h.previousBase=w.base;h.previousTip=w.tip;
    return r;
}
struct FistHold {Swing swing;std::uint64_t strikeUntil{};Vec3 previousTip{};};
struct FistResult {bool strokeOpened{},windowOpen{},wallBlocked{},struck{};Contact hit{};};
// SK/vr/vrgameplay.cpp:691-704: 1.6 m/s stroke machine; a strike opens a 180 ms window; 4 parallel rays (offset/radius 4 cm,
// world axes). w.tip = fist tip in the world, w.head = clearPath origin.
template<class Sweep,class Clear>
inline FistResult fistStep(FistHold& h,const WorldPose& w,Vec3 swingTip,const BodyFrame& body,float swingUnits,float worldUnits,
                           std::uint64_t nowUs,bool fists,const Vec3& tracking,float rayOffset,float rayRadius,Sweep&& sweep,Clear&& clearPath) {
    FistResult r{};
    if(h.swing.update(swingTip,body,swingUnits,nowUs,fists,1.6f,&tracking)){h.strikeUntil=nowUs+180'000;r.strokeOpened=true;}
    if(!h.swing.continuous||!fists||h.swing.speed<.7f)h.strikeUntil=0;
    r.windowOpen=h.strikeUntil>=nowUs;
    if(h.strikeUntil>=nowUs){
        if(!clearPath(w.head,w.tip))r.wallBlocked=true;
        else{
            const float o=rayOffset*worldUnits;Contact target{};
            for(Vec3 offset:{Vec3(),Vec3(o,0,0),Vec3(-o,0,0),Vec3(0,o,0)}){
                target=sweep(h.previousTip+offset,w.tip+offset,rayRadius*worldUnits);if(target.hit())break;
            }
            if(target.hit()){r.hit=target;r.struck=true;h.strikeUntil=0;}
        }
    }
    h.previousTip=w.tip;
    return r;
}

// ===================================================== RDR1 geometry =====================================================
// Squared distance between segments p1q1 and p2q2 (Ericson, Real-Time Collision Detection 5.1.9); s,t = closest parameters.
inline float segmentSegmentDistance2(Vec3 p1,Vec3 q1,Vec3 p2,Vec3 q2,float& s,float& t) noexcept {
    const Vec3 d1=q1-p1,d2=q2-p2,r=p1-p2;
    const float a=dot(d1,d1),e=dot(d2,d2),f=dot(d2,r);
    constexpr float eps=1e-12f;
    if(a<=eps&&e<=eps){s=t=0;return dot(r,r);}
    if(a<=eps){s=0;t=std::clamp(f/e,0.f,1.f);}
    else{
        const float c=dot(d1,r);
        if(e<=eps){t=0;s=std::clamp(-c/a,0.f,1.f);}
        else{
            const float b=dot(d1,d2),denom=a*e-b*b;
            s=denom>eps?std::clamp((b*f-c*e)/denom,0.f,1.f):0.f;
            t=(b*s+f)/e;
            if(t<0){t=0;s=std::clamp(-c/a,0.f,1.f);}
            else if(t>1){t=1;s=std::clamp((b-c)/a,0.f,1.f);}
        }
    }
    const Vec3 c1=p1+d1*s,c2=p2+d2*t,d=c1-c2;
    return dot(d,d);
}
inline float pointSegmentDistance2(Vec3 p,Vec3 a,Vec3 b,float* tOut=nullptr) noexcept {
    const Vec3 ab=b-a;const float l=dot(ab,ab);
    const float t=l>1e-12f?std::clamp(dot(p-a,ab)/l,0.f,1.f):0.f;
    if(tOut)*tOut=t;
    const Vec3 d=p-(a+ab*t);return dot(d,d);
}
struct Capsule {Vec3 a{},b{};float radius{};int zone{1};int bone{-1};std::uint8_t kind{};};
// First fraction t in [0,1] along A->B at which the point is within capsule radius + extra (distance to a segment along a line
// is convex: the inside set is one interval; bisection between 0 and the closest approach). Start inside -> t=0.
inline bool segmentCapsuleEntry(Vec3 A,Vec3 B,const Capsule& c,float extra,float& t) noexcept {
    if(!finite(A)||!finite(B)||!finite(c.a)||!finite(c.b)||!(c.radius>0)||!std::isfinite(extra))return false;
    const float R=c.radius+extra;if(!(R>0))return false;
    float s{},u{};
    if(segmentSegmentDistance2(A,B,c.a,c.b,s,u)>R*R)return false;
    if(pointSegmentDistance2(A,c.a,c.b)<=R*R){t=0;return true;}
    float lo=0,hi=s;
    for(int i=0;i<24;++i){const float mid=.5f*(lo+hi);if(pointSegmentDistance2(A+(B-A)*mid,c.a,c.b)<=R*R)hi=mid;else lo=mid;}
    t=hi;return true;
}
// Closest point of a capsule's axis to p and the outward unit normal (fallback when p sits on the axis).
inline Vec3 capsuleAxisPoint(const Capsule& c,Vec3 p) noexcept {float t{};pointSegmentDistance2(p,c.a,c.b,&t);return c.a+(c.b-c.a)*t;}

// ---- body capsules from bones (radii are the mod's own estimates) ----
enum BoneKey : unsigned {BoneHead,BoneNeck,BoneSpine3,BoneSpine0,BonePelvis,BoneArmL,BoneElbowL,BoneWristL,BoneArmR,BoneElbowR,BoneWristR,
    BoneHipL,BoneKneeL,BoneAnkleL,BoneHipR,BoneKneeR,BoneAnkleR,BoneKeyCount};
inline constexpr std::array<const char*,BoneKeyCount> boneKeyNames{"head","neck","spine3","spine0","pelvis","armL","elbowL","wristL",
    "armR","elbowR","wristR","hipL","kneeL","ankleL","hipR","kneeR","ankleR"};
// Aliases in priority order (RDR1 strings: head neck01/02 spine00..03 pelvis pelvis01 root clavicle_l/r elbow_l/r wrist_l/r hip_l/r
// knee_l/r ankle_l/r).
inline constexpr std::array<std::array<const char*,4>,BoneKeyCount> boneAliases{{
    {"head","head01","skull",nullptr},{"neck01","neck","neck00","neck02"},{"spine03","spine3","spine02","chest"},
    {"spine00","spine0","spine01","spine"},{"pelvis","pelvis01","hips","root"},
    {"arm_l","upperarm_l","shoulder_l","clavicle_l"},{"elbow_l","forearm_l","lowerarm_l",nullptr},{"wrist_l","hand_l",nullptr,nullptr},
    {"arm_r","upperarm_r","shoulder_r","clavicle_r"},{"elbow_r","forearm_r","lowerarm_r",nullptr},{"wrist_r","hand_r",nullptr,nullptr},
    {"hip_l","thigh_l","upperleg_l",nullptr},{"knee_l","calf_l","lowerleg_l",nullptr},{"ankle_l","foot_l",nullptr,nullptr},
    {"hip_r","thigh_r","upperleg_r",nullptr},{"knee_r","calf_r","lowerleg_r",nullptr},{"ankle_r","foot_r",nullptr,nullptr}}};
inline char lower(char c) noexcept {return c>='A'&&c<='Z'?char(c-'A'+'a'):c;}
// Match rank of `name` for key k: 0..3 = exact alias i, 4..7 = name starts with alias i followed by '_' (e.g. "hip_l_JointRotTrans"),
// -1 = no match. Lower is better.
inline int boneRank(const char* name,unsigned k) noexcept {
    if(!name||k>=BoneKeyCount)return -1;
    for(int pass=0;pass<2;++pass)for(int i=0;i<4;++i){
        const char* a=boneAliases[k][std::size_t(i)];if(!a)continue;
        std::size_t n=0;while(a[n]&&name[n]&&lower(name[n])==a[n])++n;
        if(a[n])continue;
        if(pass==0&&!name[n])return i;
        if(pass==1&&name[n]=='_')return 4+i;
    }
    return -1;
}
struct BoneLayout {std::array<int,BoneKeyCount> index{};std::uint32_t found{};
    BoneLayout(){index.fill(-1);}
    bool has(unsigned k) const noexcept {return k<BoneKeyCount&&index[k]>=0;}
    bool humanoid() const noexcept {return has(BoneHead)&&has(BonePelvis)&&(has(BoneSpine3)||has(BoneSpine0)||has(BoneNeck));}
};
// names[i] = bone i's name (nullptr allowed). Each key takes the best-ranked bone (ties: the lowest index).
inline BoneLayout resolveBones(const char* const* names,unsigned count) noexcept {
    BoneLayout l{};std::array<int,BoneKeyCount> best{};best.fill(99);
    for(unsigned i=0;i<count;++i)for(unsigned k=0;k<BoneKeyCount;++k){
        const int r=boneRank(names[i],k);if(r>=0&&r<best[k]){best[k]=r;l.index[k]=int(i);}
    }
    for(unsigned k=0;k<BoneKeyCount;++k)if(l.index[k]>=0)l.found|=1u<<k;
    return l;
}
struct CapsuleSpec {BoneKey a,b;float radius,lift;int zone;std::uint8_t kind;};
// kind: 0 head 1 neck 2 chest 3 belly 4 arm 5 leg 6 fallback (bone cloud) 7 fallback (actor)
inline constexpr std::array<CapsuleSpec,13> capsuleSpecs{{
    {BoneHead,BoneHead,.11f,.08f,0,0},{BoneNeck,BoneSpine3,.12f,0,0,1},{BoneSpine3,BoneSpine0,.17f,0,1,2},{BoneSpine0,BonePelvis,.16f,0,3,3},
    {BoneArmL,BoneElbowL,.06f,0,5,4},{BoneElbowL,BoneWristL,.05f,0,5,4},{BoneArmR,BoneElbowR,.06f,0,6,4},{BoneElbowR,BoneWristR,.05f,0,6,4},
    {BoneHipL,BoneKneeL,.08f,0,7,5},{BoneKneeL,BoneAnkleL,.06f,0,7,5},{BoneHipR,BoneKneeR,.08f,0,8,5},{BoneKneeR,BoneAnkleR,.06f,0,8,5},
    {BoneSpine3,BonePelvis,.17f,0,1,2}}}; // last: chest+belly in one when spine00 is missing
inline constexpr unsigned maxCapsules=16;
struct Body {std::array<Capsule,maxCapsules> capsules{};unsigned count{};};
// position(k, out) -> bool : world position of key k (Y up). up = world up (RDR1: +Y). The bone index stored is the layout's.
template<class Position>
inline unsigned buildBody(const BoneLayout& layout,Position&& position,Body& out,float scale=1.f) noexcept {
    out.count=0;
    std::array<Vec3,BoneKeyCount> p{};std::array<bool,BoneKeyCount> ok{};
    for(unsigned k=0;k<BoneKeyCount;++k)ok[k]=layout.has(k)&&position(k,p[k])&&finite(p[k]);
    for(std::size_t i=0;i<capsuleSpecs.size()&&out.count<maxCapsules;++i){
        const auto& s=capsuleSpecs[i];
        if(i==capsuleSpecs.size()-1&&(ok[BoneSpine0]||!ok[BoneSpine3]||!ok[BonePelvis]))continue; // only as the substitute
        if(!ok[s.a]||!ok[s.b])continue;
        if((p[s.a]-p[s.b]).length()>1.5f*scale)continue; // a bone far from its partner (bad layout) never forms a part
        Capsule c{};c.a=p[s.a]+Vec3(0,s.lift*scale,0);c.b=p[s.b]+Vec3(0,s.lift*scale,0);c.radius=s.radius*scale;c.zone=s.zone;
        c.bone=layout.index[s.a];c.kind=s.kind;out.capsules[out.count++]=c;
    }
    return out.count;
}
// Generic body (animals / unknown skeletons): one capsule through the bone cloud's long horizontal axis at mid height.
inline bool cloudCapsule(const Vec3* points,unsigned n,Capsule& out) noexcept {
    Vec3 lo{1e9f,1e9f,1e9f},hi{-1e9f,-1e9f,-1e9f};unsigned used=0;
    for(unsigned i=0;i<n;++i){const auto& q=points[i];if(!finite(q))continue;++used;
        lo={std::min(lo.x,q.x),std::min(lo.y,q.y),std::min(lo.z,q.z)};hi={std::max(hi.x,q.x),std::max(hi.y,q.y),std::max(hi.z,q.z)};}
    if(used<2)return false;
    const Vec3 size=hi-lo,mid=(lo+hi)*.5f;
    if(size.x>4||size.y>4||size.z>4)return false; // not one creature
    Capsule c{};c.kind=6;c.zone=1;
    if(size.y>=std::max(size.x,size.z)){ // upright
        c.radius=std::clamp(.5f*std::max(size.x,size.z),.08f,.5f);c.a={mid.x,lo.y+c.radius,mid.z};c.b={mid.x,std::max(hi.y-c.radius,lo.y+c.radius),mid.z};
    }else{
        const bool alongX=size.x>=size.z;const float half=.5f*(alongX?size.x:size.z);
        c.radius=std::clamp(.5f*std::max(alongX?size.z:size.x,size.y),.08f,.6f);
        const Vec3 axis=alongX?Vec3(1,0,0):Vec3(0,0,1);const float l=std::max(half-c.radius,0.f);
        c.a=mid-axis*l;c.b=mid+axis*l;
    }
    out=c;return true;
}
// Last resort: an upright capsule above the actor's feet (the mover capsule is coarse; used only without any skeleton).
inline Capsule actorCapsule(Vec3 feet) noexcept {Capsule c{};c.a=feet+Vec3(0,.35f,0);c.b=feet+Vec3(0,1.45f,0);c.radius=.25f;c.zone=1;c.kind=7;return c;}

// Earliest entry along A->B over a set of bodies (target index = body index). Bodies flagged `skip` are not considered.
struct BodyRef {const Body* body{};bool skip{};};
inline Contact firstEntry(Vec3 A,Vec3 B,float extra,const BodyRef* bodies,unsigned n) noexcept {
    Contact best{};best.t=2;
    for(unsigned v=0;v<n;++v){
        if(!bodies[v].body||bodies[v].skip)continue;
        const auto& b=*bodies[v].body;
        for(unsigned i=0;i<b.count;++i){float t{};
            if(segmentCapsuleEntry(A,B,b.capsules[i],extra,t)&&t<best.t){best.target=int(v);best.part=int(i);best.t=t;best.point=A+(B-A)*t;}}
    }
    if(!best.hit())best.t=0;
    return best;
}

// ===================================================== damage / reaction policy =====================================================
// Zones (hlthHitInfo, sm_HlthZoneNames 0x2e88cd0): 0 HEAD 1 TORSO_UPPER 2 TORSO_HEART 3 TORSO_GUT 4 TORSO_LOWER 5 LEFT_ARM 6 RIGHT_ARM
// 7 LEFT_LEG 8 RIGHT_LEG.
inline constexpr std::array<const char*,9> zoneNames{"HEAD","TORSO_UPPER","TORSO_HEART","TORSO_GUT","TORSO_LOWER","LEFT_ARM","RIGHT_ARM",
    "LEFT_LEG","RIGHT_LEG"};
inline bool lethalZone(int zone) noexcept {return zone==0||zone==2;}
inline bool limbZone(int zone) noexcept {return zone>=5&&zone<=8;}
struct DamageConfig {
    float torso{.55f},limb{.35f},lethal{1000.f},fastSpeed{4.f},fastMultiplier{1.3f};
    float fistKo{.34f},fistKoStrong{.5f},fistStrongSpeed{3.5f};
    bool backstab{false}; // knife from behind (face=1) is lethal
};
// Knife: flag 4 (lethal melee), damage = maxHP * k; head/neck/heart (and backstab) = 1000 (TargetKillTask template, the engine
// still clamps unkillable 0x2000 / incapacitate 0x40000). The engine then multiplies by health+0x4c (degree 0) and DDA x1..1.7.
inline float bladeDamage(const DamageConfig& c,int zone,float maxHp,float speed,bool fromBehind) noexcept {
    if(lethalZone(zone)||(c.backstab&&fromBehind))return c.lethal;
    const float hp=std::isfinite(maxHp)&&maxHp>0?maxHp:100.f;
    float d=hp*(limbZone(zone)?c.limb:c.torso);
    if(std::isfinite(speed)&&speed>=c.fastSpeed)d*=c.fastMultiplier;
    return d;
}
// Fist: flag 2 (KO points, no DDA/faction scaling), damage = maxKO (health+0x24) * k.
inline float fistDamage(const DamageConfig& c,float maxKo,float speed) noexcept {
    const float ko=std::isfinite(maxKo)&&maxKo>0?maxKo:100.f;
    return ko*(std::isfinite(speed)&&speed>=c.fistStrongSpeed?c.fistKoStrong:c.fistKo);
}
// MeleeReactionTypes (table 0x2e86208): 0 NONE 1 UP_FOR 2 UP_RGT 3 UP_LFT 4 KNCKBK 5 KNCKBK_RGT 6 KNCKBK_LFT 7 SHOVED 8 KICKED.
inline constexpr std::array<const char*,9> reactionNames{"NONE","UP_FOR","UP_RGT","UP_LFT","KNCKBK","KNCKBK_RGT","KNCKBK_LFT","SHOVED","KICKED"};
// v = strike direction (world), right = victim right axis. Knife: UP_FOR, sideways (|v.r| > .5) UP_RGT/UP_LFT; fist: the same when
// weak, KNCKBK / KNCKBK_RGT / KNCKBK_LFT at >= strongSpeed.
inline int reactionType(bool fist,Vec3 v,Vec3 right,float speed,float strongSpeed=3.5f) noexcept {
    v=normalized(v,{0,0,0});right=normalized(right,{0,0,0});
    const float side=dot(v,right);
    const bool strong=fist&&std::isfinite(speed)&&speed>=strongSpeed;
    if(std::abs(side)>.5f)return strong?(side>0?5:6):(side>0?2:3);
    return strong?4:1;
}
// Native "face" (MeleeMachine 0x1e42014..0x1e42068): 1 when victim and attacker forward rows point the same way (= from behind).
inline int faceFromForwards(Vec3 victimForward,Vec3 attackerForward) noexcept {return dot(victimForward,attackerForward)>=0?1:0;}

// ===================================================== hlthMsgInjure =====================================================
// 0x100-byte, 16-aligned, zeroed, WRITABLE (HandleInjure writes +0xe4/+0xe8). Field offsets verified in MeleeMachine 0x1e41f1c..
// 0x1e4207c (stack msg at sp+0x160) and SetFromTestProbe 0x1e82374 (hit info at msg+0x10).
namespace msg {
inline constexpr std::size_t size=0x100;
inline constexpr std::size_t id=0x00,localPos=0x10,localNormal=0x20,worldPos=0x30,worldNormal=0x40,boneName=0x70,zone=0x80,degree=0x84,
    face=0x88,component=0x8c,hitTime=0x90,melee=0x96,infoValid=0x98,impulse=0xa0,attackerPos=0xb0,explosionPos=0xb8,weapon=0xc4,
    flags=0xc8,damage=0xcc,attacker=0xd4,painDied=0xd8,painSurvived=0xd9,force=0xda,timer=0xe0,hpAfter=0xe4,hpFraction=0xe8;
inline constexpr std::uint32_t messageId=0x24;
inline constexpr std::uint32_t flagKo=0x2,flagLethal=0x4,flagInstantKill=0x80000; // instant kill: NEVER set (bypasses unkillable)
static_assert(worldPos==localPos+0x20&&worldNormal==localPos+0x30&&boneName==localPos+0x60&&zone==localPos+0x70&&component==localPos+0x7c&&
              hitTime==localPos+0x80&&infoValid==localPos+0x88,"hlthHitInfo (SetFromTestProbe) embedded at msg+0x10");
static_assert(hpFraction+4<=size&&attacker+4<=flags+0x10);
struct Fields {
    Vec3 hitPos{},normal{},impulse{};std::uintptr_t attackerPosPtr{};std::uint32_t attackerGuid{},flags{};
    int zone{1},face{0},component{-1},weapon{-1};float damage{};
};
template<class T> inline void put(unsigned char* m,std::size_t off,T v) noexcept {std::memcpy(m+off,&v,sizeof v);}
template<class T> inline T get(const unsigned char* m,std::size_t off) noexcept {T v{};std::memcpy(&v,m+off,sizeof v);return v;}
inline void putVec(unsigned char* m,std::size_t off,Vec3 v) noexcept {const float f[4]{v.x,v.y,v.z,0.f};std::memcpy(m+off,f,sizeof f);}
// False (buffer untouched beyond zeroing) for a non-finite input or a forbidden flag.
inline bool build(unsigned char* m,const Fields& f) noexcept {
    std::memset(m,0,size);
    if(!finite(f.hitPos)||!finite(f.normal)||!finite(f.impulse)||!std::isfinite(f.damage)||f.damage<0)return false;
    if(f.flags&flagInstantKill)return false;
    if(f.flags!=flagKo&&f.flags!=flagLethal)return false;
    put<std::uint32_t>(m,id,messageId);
    putVec(m,localPos,f.hitPos);putVec(m,localNormal,f.normal);putVec(m,worldPos,f.hitPos);putVec(m,worldNormal,f.normal);
    put<std::int32_t>(m,zone,f.zone>=0&&f.zone<=8?f.zone:1);put<std::int32_t>(m,degree,0);put<std::int32_t>(m,face,f.face?1:0);
    put<std::int32_t>(m,component,f.component);put<float>(m,hitTime,0.f);
    m[melee]=1;m[infoValid]=1;
    putVec(m,impulse,f.impulse);
    put<std::uintptr_t>(m,attackerPos,f.attackerPosPtr);
    put<std::int32_t>(m,weapon,f.weapon);put<std::uint32_t>(m,flags,f.flags);put<float>(m,damage,f.damage);
    put<std::uint32_t>(m,attacker,f.attackerGuid);
    m[painDied]=1;m[painSurvived]=1;m[force]=0;
    put<float>(m,timer,0.f);put<float>(m,hpAfter,-1.f);put<float>(m,hpFraction,-1.f);
    return true;
}
} // namespace msg
// projMsgApplyImpulse (0x40): +0 id 0x33, +0x10 impulse (world), +0x20 point (world), +0x30 int component, +0x34 float, +0x38 bool bullet.
namespace impulse {
inline constexpr std::size_t size=0x40,id=0x00,vec=0x10,point=0x20,component=0x30,scale=0x34,bullet=0x38;
inline constexpr std::uint32_t messageId=0x33;
inline bool build(unsigned char* m,Vec3 v,Vec3 p,int comp) noexcept {
    std::memset(m,0,size);if(!finite(v)||!finite(p))return false;
    msg::put<std::uint32_t>(m,id,messageId);msg::putVec(m,vec,v);msg::putVec(m,point,p);msg::put<std::int32_t>(m,component,comp);
    msg::put<float>(m,scale,1.f);m[bullet]=0;return true;
}
} // namespace impulse

} // namespace rdrvr::vr::melee::math
