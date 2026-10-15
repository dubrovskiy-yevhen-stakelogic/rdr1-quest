#pragma once
// BODY: pure math of the player's own body in VR - two-bone arm IK with clavicle assist, reach
// clamp + soft stretch, elbow pole hints, forearm roll share, procedural finger curl, the controller grip -> wrist mapping, the
// arm subtree re-propagation (crSkeleton::PartialUpdate rule G = G[parent] * L), body yaw and the head collapse. Header-only,
// no engine access, host-tested.
//
// Ported from gta5-quest native/vrhands/{VrMath.h, ArmIk.h, HandRig.h} with these
// adaptations for RDR1:
//  - matrices: RDR1 Matrix34 = 4 rows a,b,c,d of 16 bytes (x,y,z,w); p' = a*p.x + b*p.y + c*p.z + d (RAGE row-vector
//    "L x Gparent" == the column form "Gparent * L" used here) - the same memory meaning as GTA's 4 columns, little endian.
//  - world space, Y up: the elbow pole basis (right / forward / up of the body) is a parameter (GTA: ped object space X/Y/Z).
//  - the IK subtree is the CLAVICLE subtree (clavicle, arm, armroll, elbow, wristroll, wrist, fingers, *_Attachment): the
//    clavicle assist, the arm, the elbow and the wrist are pinned, everything else re-propagates with its own local.
//  - RDR1 bone names (crIKBodyBoneFinderByName::sm_DefaultArmBoneNames): clavicle, arm, armroll, elbow, wristroll, wrist (+_l/_r),
//    thumb_01..03, finger_11..43, neck, head (resolveRig below; indices per crSkeletonData at runtime).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace rdrvr::vr::armik {

// ============================== vector / quaternion / matrix (gta5-quest VrMath.h) ==============================
struct Vec3 {float x=0,y=0,z=0;};
inline Vec3 operator+(Vec3 a,Vec3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
inline Vec3 operator-(Vec3 a,Vec3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
inline Vec3 operator-(Vec3 a){return {-a.x,-a.y,-a.z};}
inline Vec3 operator*(Vec3 a,float s){return {a.x*s,a.y*s,a.z*s};}
inline Vec3 operator*(float s,Vec3 a){return {a.x*s,a.y*s,a.z*s};}
inline Vec3& operator+=(Vec3& a,Vec3 b){a=a+b;return a;}
inline Vec3& operator-=(Vec3& a,Vec3 b){a=a-b;return a;}
inline float Dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline Vec3 Cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
inline float Length(Vec3 a){return std::sqrt(Dot(a,a));}
inline Vec3 Lerp(Vec3 a,Vec3 b,float t){return a+(b-a)*t;}
inline Vec3 Normalize(Vec3 a,Vec3 fallback={0,0,1}){const float l=Length(a);return l>1e-8f?a*(1.0f/l):fallback;}
inline bool Finite(Vec3 a){return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z);}

struct Quat {float x=0,y=0,z=0,w=1;};
inline Quat operator*(Quat a,Quat b){
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};}
inline Quat Conjugate(Quat q){return {-q.x,-q.y,-q.z,q.w};}
inline float Dot(Quat a,Quat b){return a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w;}
inline Quat Normalize(Quat q){const float l=std::sqrt(Dot(q,q));return l>1e-12f?Quat{q.x/l,q.y/l,q.z/l,q.w/l}:Quat{};}
inline Vec3 Rotate(Quat q,Vec3 v){const Vec3 u{q.x,q.y,q.z};const Vec3 t=2.0f*Cross(u,v);return v+q.w*t+Cross(u,t);}
inline Quat AxisAngle(Vec3 axis,float radians){const Vec3 n=Normalize(axis);const float s=std::sin(.5f*radians);return {n.x*s,n.y*s,n.z*s,std::cos(.5f*radians)};}
inline Quat FromTo(Vec3 from,Vec3 to){
    const float d=Dot(from,to);
    if(d<-0.999999f){Vec3 axis=Cross(Vec3{1,0,0},from);if(Length(axis)<1e-4f)axis=Cross(Vec3{0,1,0},from);return AxisAngle(axis,3.14159265f);}
    const Vec3 c=Cross(from,to);return Normalize(Quat{c.x,c.y,c.z,1.0f+d});
}
inline Quat Slerp(Quat a,Quat b,float t){
    float d=Dot(a,b);
    if(d<0){b={-b.x,-b.y,-b.z,-b.w};d=-d;}
    if(d>0.9995f)return Normalize(Quat{a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t,a.w+(b.w-a.w)*t});
    const float th=std::acos(std::clamp(d,-1.0f,1.0f)),s=std::sin(th);
    const float wa=std::sin((1-t)*th)/s,wb=std::sin(t*th)/s;
    return {a.x*wa+b.x*wb,a.y*wa+b.y*wb,a.z*wa+b.z*wb,a.w*wa+b.w*wb};
}
struct Mat33 {Vec3 a{1,0,0},b{0,1,0},c{0,0,1};}; // columns
inline Vec3 operator*(const Mat33& m,Vec3 v){return m.a*v.x+m.b*v.y+m.c*v.z;}
inline Mat33 operator*(const Mat33& m,const Mat33& n){return {m*n.a,m*n.b,m*n.c};}
inline Mat33 Transpose(const Mat33& m){return {{m.a.x,m.b.x,m.c.x},{m.a.y,m.b.y,m.c.y},{m.a.z,m.b.z,m.c.z}};}
inline Mat33 FrameXY(Vec3 x,Vec3 yRef){
    const Vec3 ax=Normalize(x,{1,0,0});Vec3 ay=yRef-ax*Dot(yRef,ax);
    if(Length(ay)<1e-5f)ay=Cross(Vec3{0,0,1},ax);
    if(Length(ay)<1e-5f)ay=Cross(Vec3{0,1,0},ax);
    ay=Normalize(ay);return {ax,ay,Cross(ax,ay)};
}
inline Mat33 FromQuat(Quat q){q=Normalize(q);return {Rotate(q,{1,0,0}),Rotate(q,{0,1,0}),Rotate(q,{0,0,1})};}
inline Quat ToQuat(const Mat33& m){
    const float m00=m.a.x,m11=m.b.y,m22=m.c.z;const float tr=m00+m11+m22;Quat q;
    if(tr>0){const float s=std::sqrt(tr+1.0f)*2;q={(m.b.z-m.c.y)/s,(m.c.x-m.a.z)/s,(m.a.y-m.b.x)/s,.25f*s};}
    else if(m00>m11&&m00>m22){const float s=std::sqrt(1.0f+m00-m11-m22)*2;q={.25f*s,(m.b.x+m.a.y)/s,(m.c.x+m.a.z)/s,(m.b.z-m.c.y)/s};}
    else if(m11>m22){const float s=std::sqrt(1.0f+m11-m00-m22)*2;q={(m.b.x+m.a.y)/s,.25f*s,(m.c.y+m.b.z)/s,(m.c.x-m.a.z)/s};}
    else{const float s=std::sqrt(1.0f+m22-m00-m11)*2;q={(m.c.x+m.a.z)/s,(m.c.y+m.b.z)/s,.25f*s,(m.a.y-m.b.x)/s};}
    return Normalize(q);
}
inline Mat33 Orthonormalize(const Mat33& m){return FrameXY(m.a,m.b);}
struct Mat34 {Vec3 a{1,0,0},b{0,1,0},c{0,0,1},d{0,0,0};
    Mat33 Rot() const {return {a,b,c};}
};
inline Mat34 MakeMat34(const Mat33& r,Vec3 t){return {r.a,r.b,r.c,t};}
inline Vec3 TransformPoint(const Mat34& m,Vec3 p){return m.a*p.x+m.b*p.y+m.c*p.z+m.d;}
inline Vec3 TransformDir(const Mat34& m,Vec3 v){return m.a*v.x+m.b*v.y+m.c*v.z;}
inline Mat34 operator*(const Mat34& m,const Mat34& n){return {TransformDir(m,n.a),TransformDir(m,n.b),TransformDir(m,n.c),TransformPoint(m,n.d)};}
inline Mat34 InverseOrtho(const Mat34& m){const Mat33 rt=Transpose(m.Rot());return MakeMat34(rt,-(rt*m.d));}
inline Mat34 InverseAffine(const Mat34& m){
    const Vec3 r0=Cross(m.b,m.c),r1=Cross(m.c,m.a),r2=Cross(m.a,m.b);const float det=Dot(m.a,r0);
    if(std::fabs(det)<1e-12f)return InverseOrtho(m);
    const float inv=1.0f/det;
    const Mat33 ri{{r0.x*inv,r1.x*inv,r2.x*inv},{r0.y*inv,r1.y*inv,r2.y*inv},{r0.z*inv,r1.z*inv,r2.z*inv}};
    return MakeMat34(ri,-(ri*m.d));
}
inline bool Finite(const Mat34& m){return Finite(m.a)&&Finite(m.b)&&Finite(m.c)&&Finite(m.d);}
constexpr float kPi=3.14159265358979f;
constexpr float kDegToRad=kPi/180.0f;
inline float WrapAngle(float a){
    if(!std::isfinite(a))return a;
    if(a>4*kPi||a<-4*kPi)a=std::remainder(a,2*kPi);
    while(a>kPi)a-=2*kPi;
    while(a<-kPi)a+=2*kPi;
    return a;
}
inline float SmoothFactor(float dt,float tau){return tau<=0?1.0f:1.0f-std::exp(-std::max(dt,0.0f)/tau);}
inline float Clamp01(float v){return std::isfinite(v)?std::clamp(v,0.0f,1.0f):0.0f;}

// RDR1 memory <-> Mat34 (rows a,b,c,d at +0,+0x10,+0x20,+0x30; the w lanes are never written).
inline Mat34 LoadMat34(const float* m){return {{m[0],m[1],m[2]},{m[4],m[5],m[6]},{m[8],m[9],m[10]},{m[12],m[13],m[14]}};}
inline void StoreMat34(float* m,const Mat34& v){
    m[0]=v.a.x;m[1]=v.a.y;m[2]=v.a.z;m[4]=v.b.x;m[5]=v.b.y;m[6]=v.b.z;m[8]=v.c.x;m[9]=v.c.y;m[10]=v.c.z;m[12]=v.d.x;m[13]=v.d.y;m[14]=v.d.z;}
// Rotation about the unit axis `up` through `pivot` applied to a whole transform (world).
inline Mat34 RotateAbout(const Mat34& g,Vec3 pivot,const Mat33& r){return MakeMat34(r*g.Rot(),pivot+r*(g.d-pivot));}
// Yaw (radians, right-hand about up) of a horizontal direction relative to another, both projected on the plane normal to up.
inline float SignedAngleAbout(Vec3 from,Vec3 to,Vec3 up){
    const Vec3 u=Normalize(up,{0,1,0});
    Vec3 f=from-u*Dot(from,u),t=to-u*Dot(to,u);
    if(Length(f)<1e-5f||Length(t)<1e-5f)return 0.f;
    f=Normalize(f);t=Normalize(t);
    return std::atan2(Dot(Cross(f,t),u),Dot(f,t));
}

// ============================== two-bone IK (gta5-quest ArmIk.h) ==============================
struct ArmIkParams {
    float side=1.0f;        // +1 right arm, -1 left arm
    float elbowOut=0.35f;   // pole weight along the body's right * side
    float elbowDown=1.0f;   // pole weight along -up
    float elbowBack=0.3f;   // pole weight along -forward
    float elbowWrist=0.4f;  // pole weight along the back of the hand (elbow follows wrist roll)
    float rollShare=0.5f;   // share of the wrist twist moved into the forearm roll bone
    float reachMax=0.995f;  // max reach as a fraction of upper + fore length
    float stretchMax=0.0f;  // metres of soft arm stretch beyond reachMax (0 = off)
    // The body basis of the pole in the solve's space (GTA: ped object space X right, Y forward, Z up).
    Vec3 right{1,0,0},forward{0,1,0},up{0,0,1};
};
struct ArmIkInput {
    Mat34 upperArm,forearm,hand; // animated globals of the chain (arm, elbow, wrist)
    float lenUpper=0,lenFore=0;  // <= 0: measured from the matrices
    Mat34 target;                // desired wrist global (rotation orthonormal)
    float weight=1.0f;           // 0 = animated pose, 1 = target
    Vec3 handBack{0,0,1};        // back of the hand in the WRIST bone's local frame (pole hint)
    Vec3 lastPerp{0,0,0};        // persistent elbow direction
    Vec3 hingeLocal{0,0,0};      // elbow hinge in the upper arm's local frame (0 = unknown)
};
struct ArmIkOutput {
    Mat34 upperArm,forearm,hand;
    Vec3 perp;
    float clampedBy=0;           // metres the wrist target was out of reach
    Vec3 hingeLocal;
    float stretch=0;             // metres added to upper + fore length
    float stretchScale=1;
};
inline float SoftStretch(float need,float maxLen){
    if(!(need>0.0f)||!(maxLen>0.0f)||!std::isfinite(need)||!std::isfinite(maxLen))return 0.0f;
    const float knee=.25f*maxLen,lin=maxLen-knee;
    if(need<=lin)return need;
    return maxLen-knee*std::exp(-(need-lin)/knee);
}
namespace detail {
inline Vec3 AnyPerp(Vec3 v){const Vec3 ref=std::fabs(v.y)<0.9f?Vec3{0,1,0}:Vec3{1,0,0};return Normalize(Cross(v,ref),{1,0,0});}
inline bool PerpTo(Vec3 p,Vec3 dir,Vec3* out){const Vec3 q=p-dir*Dot(p,dir);const float l=Length(q);if(!(l>1e-3f)||!std::isfinite(l))return false;*out=q*(1.0f/l);return true;}
} // namespace detail
inline ArmIkOutput SolveArm(const ArmIkInput& in,const ArmIkParams& params){
    using namespace detail;
    ArmIkOutput out;out.upperArm=in.upperArm;out.forearm=in.forearm;out.hand=in.hand;out.perp=in.lastPerp;out.clampedBy=0;out.hingeLocal=in.hingeLocal;
    const float w=std::isfinite(in.weight)?std::clamp(in.weight,0.0f,1.0f):0.0f;
    if(w<=0.0f)return out;
    if(!Finite(in.upperArm)||!Finite(in.forearm)||!Finite(in.hand)||!Finite(in.target))return out;
    const Vec3 S=in.upperArm.d,E0=in.forearm.d,W0=in.hand.d;
    const float la0=Length(E0-S),lb0=Length(W0-E0);
    float la=in.lenUpper>0?in.lenUpper:la0;float lb=in.lenFore>0?in.lenFore:lb0;
    if(w<1.0f&&la0>1e-3f&&lb0>1e-3f){la=la0+(la-la0)*w;lb=lb0+(lb-lb0)*w;}
    if(!(la>1e-3f)||!(lb>1e-3f)||!std::isfinite(la+lb))return out;
    const Vec3 Wt=Lerp(W0,in.target.d,w);
    const Quat qa=ToQuat(Orthonormalize(in.hand.Rot())),qb=ToQuat(Orthonormalize(in.target.Rot()));
    const Quat qt=Normalize(Slerp(qa,qb,w));
    const Vec3 dv=Wt-S;const float d=Length(dv);
    Vec3 dir=d>1e-4f?dv*(1.0f/d):Normalize(W0-S,Normalize(E0-S,{0,1,0}));
    const float reachFrac=std::isfinite(params.reachMax)?std::clamp(params.reachMax,0.5f,1.0f):0.995f;
    float dMax=reachFrac*(la+lb);
    if(w<1.0f){const float d0=std::min(Length(W0-S),la+lb);if(d0>dMax)dMax+=(d0-dMax)*(1.0f-w);}
    if(params.stretchMax>0.0f&&d>dMax){
        const float add=w*SoftStretch((d-dMax)/reachFrac,std::min(params.stretchMax,0.5f));
        if(add>0.0f&&std::isfinite(add)){const float k=1.0f+add/(la+lb);la*=k;lb*=k;dMax+=reachFrac*add;out.stretch=add;out.stretchScale=k;}
    }
    const float dMin=std::min(std::fabs(la-lb)+0.01f,dMax);
    const float dc=std::clamp(d,dMin,dMax);
    const Vec3 Wc=S+dir*dc;
    out.clampedBy=Length(Wc-Wt);
    const Vec3 handBackW=Rotate(qt,Normalize(in.handBack,{0,0,1}));
    const Vec3 pole=Normalize(params.right,{1,0,0})*(params.side*params.elbowOut)-Normalize(params.up,{0,0,1})*params.elbowDown-
                    Normalize(params.forward,{0,1,0})*params.elbowBack+handBackW*params.elbowWrist;
    Vec3 perpPole;
    if(!PerpTo(pole,dir,&perpPole)&&!PerpTo(in.lastPerp,dir,&perpPole)&&!PerpTo(E0-S,dir,&perpPole))perpPole=AnyPerp(dir);
    Vec3 perp=perpPole,perpAnim;
    const Vec3 dir0=Normalize(W0-S,dir);
    if(w<1.0f&&Dot(dir0,dir)>-0.99f&&PerpTo(E0-S,dir0,&perpAnim)){
        perpAnim=Rotate(FromTo(dir0,dir),perpAnim);perpAnim=Normalize(perpAnim-dir*Dot(perpAnim,dir),perpPole);
        const float ang=std::atan2(Dot(Cross(perpAnim,perpPole),dir),Dot(perpAnim,perpPole));
        perp=Normalize(Rotate(AxisAngle(dir,ang*w),perpAnim),perpPole);
    }
    out.perp=perp;
    const float cosA=std::clamp((la*la+dc*dc-lb*lb)/(2.0f*la*dc),-1.0f,1.0f);
    const float sinA=std::sqrt(std::max(0.0f,1.0f-cosA*cosA));
    const Vec3 E=S+(dir*cosA+perp*sinA)*la;
    const Vec3 u0=Normalize(E0-S,dir),v0=Normalize(W0-E0,dir);
    const Vec3 u1=Normalize(E-S,dir),v1=Normalize(Wc-E,dir);
    Vec3 n1=Cross(u1,v1);
    if(Length(n1)<0.02f)n1=Cross(u1,perp);
    n1=Normalize(n1,AnyPerp(u1));
    const Mat33 UA0=Orthonormalize(in.upperArm.Rot());
    Vec3 n0=Cross(u0,v0);const float s0=Length(n0);
    if(s0>=0.1f)out.hingeLocal=Normalize(Transpose(UA0)*(n0*(1.0f/s0)),in.hingeLocal);
    if(s0<0.02f)n0=Length(in.hingeLocal)>0.5f?UA0*in.hingeLocal:n1;
    n0=Normalize(n0,n1);
    const Mat33 Rua=FrameXY(u1,n1)*Transpose(FrameXY(u0,n0));
    const Mat33 Rfa=FrameXY(v1,n1)*Transpose(FrameXY(v0,n0));
    out.upperArm=MakeMat34(Rua*in.upperArm.Rot(),S);
    out.forearm=MakeMat34(Rfa*in.forearm.Rot(),E);
    // Keep the wrist's native scale (the solved rotation is unit; RDR1 globals carry the skeleton's scale in the axes).
    const float scale=Length(in.hand.a);
    out.hand=MakeMat34(FromQuat(qt),Wc);
    if(std::isfinite(scale)&&scale>.5f&&scale<2.f&&std::fabs(scale-1.f)>1e-4f){out.hand.a=out.hand.a*scale;out.hand.b=out.hand.b*scale;out.hand.c=out.hand.c*scale;}
    if(!Finite(out.upperArm)||!Finite(out.forearm)||!Finite(out.hand)){
        out.upperArm=in.upperArm;out.forearm=in.forearm;out.hand=in.hand;out.perp=in.lastPerp;out.clampedBy=0;out.hingeLocal=in.hingeLocal;
        out.stretch=0;out.stretchScale=1;
    }
    return out;
}
inline float TwistAbout(const Mat33& from,const Mat33& to,Vec3 axis){
    const Vec3 ax=Normalize(axis,{0,0,1});
    Quat q=ToQuat(Orthonormalize(to*Transpose(from)));
    if(q.w<0)q={-q.x,-q.y,-q.z,-q.w};
    const float p=q.x*ax.x+q.y*ax.y+q.z*ax.z;
    if(std::fabs(p)<1e-9f&&std::fabs(q.w)<1e-9f)return 0.0f;
    float a=WrapAngle(2.0f*std::atan2(p,q.w));
    if(a<=-kPi)a+=2*kPi;
    return std::isfinite(a)?a:0.0f;
}
// HandRig.h ForearmRollLocal: `share` of the wrist twist the IK added (new vs animated wrist local in the elbow frame) about the
// forearm axis, pre-multiplied into the animated roll bone local (translation kept).
inline Mat34 ForearmRollLocal(const Mat34& animRollLocal,const Mat33& animHandLocal,const Mat33& newHandLocal,Vec3 axisInForearm,float share,
                              float maxTwist=2.6f,float* twistOut=nullptr){
    const Vec3 axis=Normalize(axisInForearm,{1,0,0});
    float twist=TwistAbout(Orthonormalize(animHandLocal),Orthonormalize(newHandLocal),axis);
    twist=std::clamp(twist,-maxTwist,maxTwist);
    if(twistOut)*twistOut=twist;
    const float angle=(std::isfinite(share)?share:0.0f)*twist;
    if(!std::isfinite(angle)||std::fabs(angle)<1e-6f)return animRollLocal;
    const Mat33 r=FromQuat(AxisAngle(axis,angle));
    return MakeMat34(r*animRollLocal.Rot(),animRollLocal.d);
}
inline Quat CurlJoint(Quat bindLocal,Vec3 axisLocal,float radians){
    if(!std::isfinite(radians)||Length(axisLocal)<1e-8f)return bindLocal;
    return Normalize(AxisAngle(axisLocal,radians)*bindLocal);
}
// ArmIk.h ClavicleReach: rotation about the clavicle origin that brings the shoulder towards a target beyond `start` of the reach
// (at most maxRadians, times the weight). Identity when not needed.
inline Quat ClavicleReach(Vec3 clavicle,const ArmIkInput& in,float start,float maxRadians,float* angleOut=nullptr){
    if(angleOut)*angleOut=0.0f;
    const Quat none{};
    const float w=std::isfinite(in.weight)?std::clamp(in.weight,0.0f,1.0f):0.0f;
    const float maxA=std::isfinite(maxRadians)?std::clamp(maxRadians,0.0f,.5f*kPi):0.0f;
    if(w<=0.0f||!(maxA>0.0f)||!Finite(clavicle))return none;
    if(!Finite(in.upperArm)||!Finite(in.forearm)||!Finite(in.hand)||!Finite(in.target))return none;
    const float frac=std::isfinite(start)?std::clamp(start,0.5f,1.0f):0.98f;
    const Vec3 S=in.upperArm.d,E0=in.forearm.d,W0=in.hand.d;
    const float la0=Length(E0-S),lb0=Length(W0-E0);
    float la=in.lenUpper>0?in.lenUpper:la0;float lb=in.lenFore>0?in.lenFore:lb0;
    if(w<1.0f&&la0>1e-3f&&lb0>1e-3f){la=la0+(la-la0)*w;lb=lb0+(lb-lb0)*w;}
    if(!(la>1e-3f)||!(lb>1e-3f)||!std::isfinite(la+lb))return none;
    const Vec3 Wt=Lerp(W0,in.target.d,w);
    const float reach=frac*(la+lb);
    const Vec3 rs=S-clavicle,rt=Wt-clavicle;
    const float r=Length(rs),dt=Length(rt),d0=Length(Wt-S);
    if(!(d0>reach)||!(r>1e-3f)||!(dt>1e-3f))return none;
    const Vec3 axis=Cross(rs,rt);const float sinPart=Length(axis);
    if(!(sinPart>1e-6f*r*dt))return none;
    const float phi=std::atan2(sinPart,Dot(rs,rt));
    const float c=(r*r+dt*dt-reach*reach)/(2.0f*r*dt);
    const float need=c>=1.0f?phi:phi-std::acos(std::max(c,-1.0f));
    const float angle=std::clamp(need,0.0f,std::min(phi,maxA))*w;
    if(!(angle>0.0f)||!std::isfinite(angle))return none;
    if(angleOut)*angleOut=angle;
    return AxisAngle(axis*(1.0f/sinPart),angle);
}

// ============================== hand frame + controller grip -> wrist (gta5-quest HandRig.h) ==============================
struct HandFrame {
    Vec3 knuckle{0,1,0};  // wrist-local unit: wrist -> middle finger base
    Vec3 palmOut{0,0,-1}; // wrist-local unit: out of the palm
    Vec3 side{1,0,0};     // wrist-local unit: towards the thumb / index side
    bool valid=false;
    float thumbSide=0,thumbPalm=0;
};
// Hand frame from the finger base positions in the WRIST bone frame (RDR1: middle = finger_21 / finger, index finger_11, pinky
// finger_41, thumb thumb_01). Without index/pinky (mitten rigs) the side comes from the thumb (perpendicular to the knuckle).
inline HandFrame MeasureHandFrame(Vec3 thumbBase,bool haveIndexPinky,Vec3 indexBase,Vec3 middleBase,Vec3 pinkyBase,bool right){
    HandFrame f;
    if(!Finite(thumbBase)||!Finite(middleBase)||Length(middleBase)<0.01f)return f;
    const Vec3 k=Normalize(middleBase);
    Vec3 s=haveIndexPinky&&Finite(indexBase)&&Finite(pinkyBase)&&Length(indexBase-pinkyBase)>=0.01f?indexBase-pinkyBase:thumbBase;
    s=s-k*Dot(s,k);
    if(Length(s)<0.005f)return f;
    s=Normalize(s);
    const Vec3 c=Cross(k,s);
    f.knuckle=k;f.side=s;f.palmOut=Normalize(right?-c:c);f.valid=true;
    const Vec3 t=Normalize(thumbBase,{0,0,0});f.thumbSide=Dot(t,f.side);f.thumbPalm=Dot(t,f.palmOut);
    return f;
}
// OpenXR grip: origin in the curled fingers, -Z along the handle, +X into the right palm / out of the left palm. The wrist sits
// wristBack (7 cm) behind the grip origin along the aim forward and wristDepth (3 cm) towards the back of the hand.
constexpr float kWristBack=0.07f,kWristDepth=0.03f;
inline Vec3 DefaultAimInGrip(){return Normalize(Vec3{0.0f,-0.7071f,-0.7071f});}
// Wrist transform in the controller GRIP frame (metres). wrist world = grip world * HandInGrip.
inline Mat34 HandInGrip(const HandFrame& hf,Vec3 aimFwdInGrip,bool right,float wristBack=kWristBack,float wristDepth=kWristDepth){
    const Vec3 palm{right?-1.0f:1.0f,0.0f,0.0f};
    Vec3 fwd=aimFwdInGrip-palm*Dot(aimFwdInGrip,palm);
    if(!Finite(fwd)||Length(fwd)<0.2f)fwd=DefaultAimInGrip();
    fwd=Normalize(fwd);
    const Vec3 hk=hf.knuckle,hp=Normalize(hf.palmOut-hk*Dot(hf.palmOut,hk),{0,0,-1});
    const Mat33 H{hk,hp,Cross(hk,hp)};
    const Mat33 G{fwd,palm,Cross(fwd,palm)};
    const Mat33 R=G*Transpose(H);
    const Vec3 p=fwd*-wristBack-palm*wristDepth;
    return MakeMat34(Orthonormalize(R),p);
}

// ============================== fingers (HandRig.h BuildFingerPoses, simplified to curl angles) ==============================
constexpr int kFingers=5,kJoints=3;
enum FingerIndex : int {kThumb=0,kIndexFinger,kMiddle,kRing,kPinky};
struct FingerJointBind {bool valid=false;Quat bindLocal;Mat33 parentInHand;Vec3 posInHand;};
struct FingerCurl {
    bool valid[kFingers]{};
    Quat bind[kFingers][kJoints];   // bind local rotation (open hand)
    Vec3 axis[kFingers][kJoints];   // curl axis in the joint's parent frame (positive closes)
    float fist[kFingers][kJoints]{};// fist angle (radians)
};
struct FingerCurlTable {float finger[3]={60.0f,80.0f,60.0f};float thumb[3]={35.0f,40.0f,35.0f};};
constexpr float kThumbOpposition=0.65f,kThumbOppositionMax=55.0f*kDegToRad;
inline FingerCurl BuildFingerCurl(const FingerJointBind bind[kFingers][kJoints],Vec3 palmOutHand,const FingerCurlTable& table={}){
    FingerCurl p;const Vec3 palm=Normalize(palmOutHand,{0,0,-1});
    for(int f=0;f<kFingers;++f){
        bool ok=true;for(int j=0;j<kJoints;++j)ok=ok&&bind[f][j].valid;
        p.valid[f]=ok;
        for(int j=0;j<kJoints;++j){
            const FingerJointBind& b=bind[f][j];
            p.bind[f][j]=b.valid?Normalize(b.bindLocal):Quat{};p.axis[f][j]={0,0,0};
            if(!ok)continue;
            const Vec3 dir=j<kJoints-1?bind[f][j+1].posInHand-b.posInHand:b.posInHand-bind[f][j-1].posInHand;
            Vec3 axisHand=Cross(Normalize(dir,{0,1,0}),palm);
            if(Length(axisHand)<0.05f)axisHand=Cross(Normalize(dir,{0,1,0}),Vec3{palm.y,palm.z,palm.x});
            axisHand=Normalize(axisHand,{1,0,0});
            p.fist[f][j]=(f==kThumb?table.thumb[j]:table.finger[j])*kDegToRad;
            if(f==kThumb&&j==0&&bind[kIndexFinger][1].valid){
                const Vec3 d=Normalize(dir,{0,1,0});const Vec3 toIndex=bind[kIndexFinger][1].posInHand-b.posInHand;
                const Vec3 cr=Cross(d,Normalize(toIndex,d));
                if(Length(cr)>0.05f){axisHand=Normalize(cr,{1,0,0});
                    const float between=std::acos(std::clamp(Dot(d,Normalize(toIndex,d)),-1.0f,1.0f));
                    p.fist[f][j]=std::min(kThumbOpposition*between,kThumbOppositionMax);}
            }
            p.axis[f][j]=Normalize(Transpose(b.parentInHand)*axisHand,{1,0,0});
        }
    }
    return p;
}
enum class FingerMode : std::uint8_t {Native,Free,Support,Fist};
// Curl fraction of the fist angle per finger (VC 4-pose blend open/grip/trigger/fist from analog grip g and trigger t).
inline float FingerFraction(FingerMode mode,int finger,float grip,float trigger){
    const float g=Clamp01(grip),t=Clamp01(trigger);
    switch(mode){
    case FingerMode::Free:{
        const float wg=g*(1-t),wt=(1-g)*t,wf=g*t;
        const float pg=finger==kIndexFinger?0.f:1.f,pt=finger==kIndexFinger?1.f:finger==kThumb?.5f:0.f;
        return wg*pg+wt*pt+wf;}
    case FingerMode::Support:return .8f;
    case FingerMode::Fist:return 1.f;
    default:return 0.f;
    }
}

// ============================== subtree re-propagation (crSkeleton::PartialUpdate rule) ==============================
// Relative indices [0, count), depth first, parentRel[i] < i for i > 0; bone 0 final in obj[0]. Pinned bones keep obj and get
// local = inverse(obj[parent]) * obj; the others get obj = obj[parent] * local.
inline bool Repropagate(int count,const std::int16_t* parentRel,const std::uint8_t* pinned,Mat34* obj,Mat34* local){
    for(int i=1;i<count;++i){
        const int p=parentRel[i];
        if(p<0||p>=i)return false;
        if(pinned[i])local[i]=InverseAffine(obj[p])*obj[i];
        else obj[i]=obj[p]*local[i];
    }
    return true;
}

// ============================== one arm over the CLAVICLE subtree (HandRig.h PoseArm, RDR1 variant) ==============================
constexpr int kMaxArm=64;
struct ArmRig {
    int count=0;                       // bones in [clavicle, subtree end)
    std::int16_t parentRel[kMaxArm]{};  // parent - clavicle (-1 for the clavicle)
    std::uint8_t pinned[kMaxArm]{};     // arm, elbow, wrist
    int armRel=-1,elbowRel=-1,wristRel=-1,rollRel=-1; // rollRel: wristroll when a child of the elbow (-1 none)
    bool clavicleIsParent=false;        // parent(arm) == clavicle (clavicle assist allowed)
    int fingerRel[kFingers][kJoints];
    Vec3 rollAxis{1,0,0};               // elbow-frame axis towards the wrist
    HandFrame hand;
    FingerCurl curl;bool fingersValid=false;
    ArmRig(){for(auto& f:fingerRel)for(int& j:f)j=-1;}
};
struct ArmPoseInput {
    Mat34 clavicleParent;               // global of the clavicle's parent (spine)
    Mat34 target;                       // wrist global target
    float weight=1;
    ArmIkParams params;
    Vec3 lastPerp{0,0,0},hingeLocal{0,0,0};
    bool clavicleReach=true;float clavicleStart=0.98f,clavicleMaxRadians=15.0f*kDegToRad;
    FingerMode fingers=FingerMode::Native;float grip=0,trigger=0;
};
struct ArmPoseOutput {
    ArmIkOutput ik;float twist=0;bool clavicleMoved=false;float clavicleAngle=0;bool fingersPosed=false;
};
// obj / local: globals and locals of the clavicle subtree (index 0 = clavicle) in, the re-posed subtree out; modified[i] = 1 where
// a local changed. False = malformed rig or a non-finite result (the caller writes nothing).
inline bool PoseArm(const ArmRig& rig,const ArmPoseInput& in,Mat34* obj,Mat34* local,std::uint8_t* modified,ArmPoseOutput& out){
    const int n=rig.count;
    if(n<4||n>kMaxArm||rig.armRel<0||rig.elbowRel<=rig.armRel||rig.wristRel<=rig.elbowRel||rig.wristRel>=n)return false;
    for(int k=0;k<n;++k)modified[k]=0;
    out=ArmPoseOutput{};
    if(in.clavicleReach&&rig.clavicleIsParent&&rig.armRel>0){
        ArmIkInput probe;probe.upperArm=obj[rig.armRel];probe.forearm=obj[rig.elbowRel];probe.hand=obj[rig.wristRel];
        probe.target=in.target;probe.weight=in.weight;
        float angle=0;const Quat q=ClavicleReach(obj[0].d,probe,in.clavicleStart,in.clavicleMaxRadians,&angle);
        if(angle>0.0f&&Finite(in.clavicleParent)){
            const Mat33 r=FromQuat(q);const Vec3 c=obj[0].d;
            const Mat34 delta=MakeMat34(r,c-r*c);
            obj[0]=MakeMat34(r*obj[0].Rot(),c);
            local[0]=InverseAffine(in.clavicleParent)*obj[0];modified[0]=1;
            obj[rig.armRel]=delta*obj[rig.armRel];obj[rig.elbowRel]=delta*obj[rig.elbowRel];obj[rig.wristRel]=delta*obj[rig.wristRel];
            out.clavicleMoved=true;out.clavicleAngle=angle;
        }
    }
    const Mat34 fa0=obj[rig.elbowRel],hand0=obj[rig.wristRel];
    ArmIkInput ik;ik.upperArm=obj[rig.armRel];ik.forearm=fa0;ik.hand=hand0;ik.target=in.target;ik.weight=in.weight;
    ik.handBack=-rig.hand.palmOut;ik.lastPerp=in.lastPerp;ik.hingeLocal=in.hingeLocal;
    out.ik=SolveArm(ik,in.params);
    obj[rig.armRel]=out.ik.upperArm;obj[rig.elbowRel]=out.ik.forearm;obj[rig.wristRel]=out.ik.hand;
    modified[rig.armRel]=modified[rig.elbowRel]=modified[rig.wristRel]=1;
    if(rig.rollRel>0&&rig.rollRel<n&&!rig.pinned[rig.rollRel]){
        local[rig.rollRel]=ForearmRollLocal(local[rig.rollRel],Transpose(Orthonormalize(fa0.Rot()))*Orthonormalize(hand0.Rot()),
            Transpose(Orthonormalize(out.ik.forearm.Rot()))*Orthonormalize(out.ik.hand.Rot()),rig.rollAxis,in.params.rollShare,2.6f,&out.twist);
        modified[rig.rollRel]=1;
    }
    if(out.ik.stretch>0.0f&&out.ik.stretchScale>1.0f){ // other direct children of arm / elbow slide along the stretched bone
        const float k=out.ik.stretchScale;
        for(int i=1;i<n;++i){
            const int p=rig.parentRel[i];
            if(rig.pinned[i]||(p!=rig.armRel&&p!=rig.elbowRel))continue;
            const Mat34& bone=obj[p];
            const Vec3 seg=p==rig.armRel?obj[rig.elbowRel].d-obj[rig.armRel].d:obj[rig.wristRel].d-obj[rig.elbowRel].d;
            const float len=Length(seg);if(!(len>1e-3f))continue;
            const Vec3 axisL=Normalize(Transpose(Orthonormalize(bone.Rot()))*seg,{});
            const float along=std::clamp(Dot(local[i].d,axisL),0.0f,len/k);
            local[i].d+=axisL*(along*(k-1.0f));modified[i]=1;
        }
    }
    if(in.fingers!=FingerMode::Native&&rig.fingersValid){
        const float w=Clamp01(in.weight);
        for(int f=0;f<kFingers;++f){
            if(!rig.curl.valid[f])continue;
            const float frac=FingerFraction(in.fingers,f,in.grip,in.trigger);
            for(int j=0;j<kJoints;++j){
                const int r=rig.fingerRel[f][j];if(r<=0||r>=n||rig.pinned[r])continue;
                const Quat target=CurlJoint(rig.curl.bind[f][j],rig.curl.axis[f][j],frac*rig.curl.fist[f][j]);
                const Quat anim=ToQuat(Orthonormalize(local[r].Rot()));
                const Quat q=w>=1.0f?target:Normalize(Slerp(anim,target,w));
                const float s=Length(local[r].a);
                Mat34 m=MakeMat34(FromQuat(q),local[r].d);
                if(std::isfinite(s)&&s>.5f&&s<2.f&&std::fabs(s-1.f)>1e-4f){m.a=m.a*s;m.b=m.b*s;m.c=m.c*s;}
                local[r]=m;modified[r]=1;
            }
        }
        out.fingersPosed=true;
    }
    if(!Repropagate(n,rig.parentRel,rig.pinned,obj,local))return false;
    // pinned bones whose parent did not change keep their local only up to float noise; mark them modified (written)
    for(int k=0;k<n;++k){if(!Finite(obj[k])||!Finite(local[k]))return false;if(rig.pinned[k])modified[k]=1;}
    return true;
}

// ============================== skeleton map (names + parents -> rig), pure ==============================
// Bones are depth-first: the subtree of b is [b, SubtreeEnd(b)).
template<class ParentAt>
inline int SubtreeEnd(int b,int count,ParentAt&& parentAt){
    if(b<0||b>=count)return b;
    int j=b+1;
    for(;j<count;++j){int p=parentAt(j);int guard=0;bool inside=false;
        while(p>=0&&guard++<256){if(p==b){inside=true;break;}if(p<b)break;p=parentAt(p);}
        if(!inside)break;}
    return j;
}
template<class ParentAt>
inline bool IsAncestor(int ancestor,int bone,ParentAt&& parentAt){
    int p=bone;for(int guard=0;p>=0&&guard<256;++guard){if(p==ancestor)return true;p=parentAt(p);}return false;}
inline bool NameEquals(const char* a,const char* b){
    if(!a||!b)return false;
    for(;*a&&*b;++a,++b){char x=*a,y=*b;if(x>='A'&&x<='Z')x=char(x-'A'+'a');if(y>='A'&&y<='Z')y=char(y-'A'+'a');if(x!=y)return false;}
    return !*a&&!*b;
}
struct ArmBones {
    int clavicle=-1,arm=-1,armroll=-1,elbow=-1,wristroll=-1,wrist=-1,attach=-1,end=-1;
    int finger[kFingers][kJoints];
    bool valid=false;const char* why="NOT_RUN";
    ArmBones(){for(auto& f:finger)for(int& j:f)j=-1;}
};
struct BodyBones {
    ArmBones arm[2];          // [0] right, [1] left (engine sides)
    int head=-1,neck=-1,root=-1,spine=-1;int headEnd=-1,neckEnd=-1;bool depthFirst=false,indexOrder=true;
    int firstBad=-1,firstBadParent=-2; // First bone breaking the global depth-first order (report)
    int pelvis=-1,chest=-1,hip[2]{-1,-1}; // Holster sockets on the skeleton (pelvis, spine03, hip_r / hip_l)
};
// nameAt(i) -> const char* (nullptr when unreadable), parentAt(i) -> int.
template<class NameAt,class ParentAt>
inline BodyBones ResolveBones(int count,NameAt&& nameAt,ParentAt&& parentAt){
    BodyBones b;
    b.depthFirst=count>0&&parentAt(0)<0;
    for(int i=1;i<count&&b.depthFirst;++i){const int p=parentAt(i);if(p<0||p>=i){b.depthFirst=false;b.firstBad=i;b.firstBadParent=p;}}
    // The player rig (119 bones) is not globally depth-first; only the touched subtrees must be contiguous:
    // [start,end) holds exactly the descendants of start, each with its parent inside the range before it.
    const auto contiguous=[&](int start,int end){
        if(start<0||end<=start||end>count)return false;
        for(int j=start+1;j<end;++j){const int p=parentAt(j);if(p<start||p>=j)return false;}
        for(int j=0;j<count;++j){if(j>=start&&j<end)continue;if(IsAncestor(start,j,parentAt))return false;}
        return true;};
    const char* suf[2]={"_r","_l"};
    char want[40];
    const auto is=[&](int i,const char* base,const char* s){std::snprintf(want,sizeof want,"%s%s",base,s);return NameEquals(nameAt(i),want);};
    for(int i=0;i<count;++i){
        const char* n=nameAt(i);if(!n)continue;
        if(NameEquals(n,"head"))b.head=i;else if(NameEquals(n,"neck"))b.neck=i;else if(NameEquals(n,"spine00"))b.spine=i;
        else if(NameEquals(n,"pelvis"))b.pelvis=i;else if(NameEquals(n,"spine03"))b.chest=i;else if(NameEquals(n,"hip_r"))b.hip[0]=i;
        else if(NameEquals(n,"hip_l"))b.hip[1]=i;
        for(int s=0;s<2;++s){
            auto& a=b.arm[s];
            if(is(i,"clavicle",suf[s]))a.clavicle=i;else if(is(i,"arm",suf[s]))a.arm=i;else if(is(i,"armroll",suf[s]))a.armroll=i;
            else if(is(i,"elbow",suf[s]))a.elbow=i;else if(is(i,"wristroll",suf[s]))a.wristroll=i;else if(is(i,"wrist",suf[s]))a.wrist=i;
            else if(is(i,"wrist",s?"_l_attachment":"_r_attachment"))a.attach=i;
            for(int j=0;j<kJoints;++j){char nm[24];
                std::snprintf(nm,sizeof nm,"thumb_0%d",j+1);if(is(i,nm,suf[s]))a.finger[kThumb][j]=i;
                for(int f=1;f<kFingers;++f){std::snprintf(nm,sizeof nm,"finger_%d%d",f,j+1);if(is(i,nm,suf[s]))a.finger[f][j]=i;}}
        }
    }
    b.root=count>0?0:-1;
    if(b.head>=0){const int e=SubtreeEnd(b.head,count,parentAt);if(contiguous(b.head,e))b.headEnd=e;}
    if(b.neck>=0){const int e=SubtreeEnd(b.neck,count,parentAt);if(contiguous(b.neck,e))b.neckEnd=e;}
    for(auto& a:b.arm){
        if(a.clavicle<0){a.why="NO_CLAVICLE";continue;}
        if(a.arm<0){a.why="NO_ARM";continue;}
        if(a.elbow<0){a.why="NO_ELBOW";continue;}
        if(a.wrist<0){a.why="NO_WRIST";continue;}
        if(!IsAncestor(a.clavicle,a.arm,parentAt)||!IsAncestor(a.arm,a.elbow,parentAt)||!IsAncestor(a.elbow,a.wrist,parentAt)){a.why="CHAIN";continue;}
        a.end=SubtreeEnd(a.clavicle,count,parentAt);
        if(!contiguous(a.clavicle,a.end)){a.why="NOT_CONTIGUOUS";continue;}
        if(a.end-a.clavicle>kMaxArm){a.why="SUBTREE_TOO_BIG";continue;}
        if(a.attach>=0&&!(a.attach>a.clavicle&&a.attach<a.end))a.attach=-1; // the attachment bone must ride in the subtree
        a.valid=true;a.why="OK"; // armroll / wristroll / fingers / attachment are optional
    }
    return b;
}

// Builds the arm rig (relative indices) from the resolved bones and the parents. side: 0 right, 1 left.
template<class ParentAt>
inline bool BuildArmRig(const ArmBones& a,ParentAt&& parentAt,ArmRig& rig){
    rig=ArmRig{};
    if(!a.valid)return false;
    const int base=a.clavicle;rig.count=a.end-base;
    if(rig.count<4||rig.count>kMaxArm)return false;
    for(int i=0;i<rig.count;++i){const int p=i?parentAt(base+i)-base:-1;if(i&&(p<0||p>=i))return false;rig.parentRel[i]=std::int16_t(p);}
    rig.armRel=a.arm-base;rig.elbowRel=a.elbow-base;rig.wristRel=a.wrist-base;
    rig.pinned[rig.armRel]=rig.pinned[rig.elbowRel]=rig.pinned[rig.wristRel]=1;
    rig.clavicleIsParent=rig.parentRel[rig.armRel]==0;
    if(a.wristroll>=0&&parentAt(a.wristroll)==a.elbow&&a.wristroll>base&&a.wristroll<a.end)rig.rollRel=a.wristroll-base;
    for(int f=0;f<kFingers;++f)for(int j=0;j<kJoints;++j)rig.fingerRel[f][j]=a.finger[f][j]>base&&a.finger[f][j]<a.end?a.finger[f][j]-base:-1;
    return true;
}

// ============================== head collapse (grmMatrixSet::Update wrap) ==============================
// G' = S_P(eps) * G: the axes scaled by eps, the origin pulled to P (skinned verts of the range collapse onto P).
inline Mat34 CollapseAbout(const Mat34& g,Vec3 pivot,float eps){return {g.a*eps,g.b*eps,g.c*eps,pivot+(g.d-pivot)*eps};}
} // namespace rdrvr::vr::armik
