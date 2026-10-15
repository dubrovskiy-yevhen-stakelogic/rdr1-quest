#pragma once
// PLAYER SHADOW WINDOW. Pure part (no engine state, host-tested): spheres of draw
// records, the tile matrix projection into atlas pixels, the window rectangle. Included by SunStagger.h between its
// namespace blocks (uses stagger::Rect).
//
// At SUN SHADOWS STAGGER (level 5) the near cascade tile (0-19.5 m) is redrawn every 3rd frame, so the player's own
// shadow would step at 12 Hz and lag 1-3 frames. On a partial frame whose near tile is MASKED, the tile is therefore
// not skipped but redrawn inside a small window around the
// player (and his mount / creatures touching him): the snapshot light viewport (grcViewport, 0x470 bytes) is put back for
// the tile, so the casters render with the same matrix the tile's stale content (and the receiver) uses; the layer clears
// only the window rect (vkCmdClearAttachments) and clamps every scissor into it; DrawCommon skips every record whose bound
// does not touch the window. The window covers where the player was drawn last in this tile (content spheres) and where he
// is now (last frame's spheres moved by the camera's displacement), so the old footprint is erased and the new one drawn.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace rdrvr::engine::shadowupdate::stagger::player {
inline constexpr std::size_t vpBytes=0x470;            // sizeof(rage::grcViewport): operator new(0x470) in CShadowMap::CShadowMap @0x2945e78
inline constexpr std::uintptr_t vpArrayField=0x10;     // CShadowMap+0x10 -> grcViewport*[tiles] (BeginTileAroundSphere: SetCurrent([[+0x10]+8t]))
inline constexpr std::uintptr_t infoCreature=0x98,infoDrawable=0x80,infoFlags=0xac; // renDrawBucketInfo (SmallObjectCull.h)

struct Sphere {float c[3]{};float r{};};
inline constexpr unsigned maxSpheres=12;
struct SphereSet {
    std::array<Sphere,maxSpheres> s{};unsigned n{};bool overflow{};
    void clear() noexcept {n=0;overflow=false;}
    // Same record seen in several passes (tiles, eye depth, eye colour) collapses into one entry.
    void add(const Sphere& x) noexcept {
        for(unsigned i=0;i<n;++i){auto& e=s[i];const float dx=e.c[0]-x.c[0],dy=e.c[1]-x.c[1],dz=e.c[2]-x.c[2];
            if(dx*dx+dy*dy+dz*dz<=4e-4f&&std::fabs(e.r-x.r)<=.02f){e.r=std::max(e.r,x.r);return;}}
        if(n<maxSpheres)s[n++]=x;else overflow=true;
    }
    int primary() const noexcept {int b=-1;for(unsigned i=0;i<n;++i)if(b<0||s[i].r>s[unsigned(b)].r)b=int(i);return b;}
};
inline bool sane(const Sphere& s) noexcept {
    return std::isfinite(s.c[0])&&std::isfinite(s.c[1])&&std::isfinite(s.c[2])&&std::isfinite(s.r)&&s.r>0.f&&s.r<50.f&&
           std::fabs(s.c[0])<1e6f&&std::fabs(s.c[1])<1e6f&&std::fabs(s.c[2])<1e6f;
}
// World sphere of a kind-0 renDrawBucketInfo: root matrix info+0x00 (row-vector Matrix34 as 16 floats) x rmcDrawable cull
// sphere (local centre +0x20, radius +0x90) - the fields DrawCommon itself reads (SmallObjectCull::sphere). False for the
// other kinds (+0xac bits 13..15), no drawable or an insane value.
inline bool recordSphere(const unsigned char* info,Sphere& out) noexcept {
    std::uint32_t flags{};std::memcpy(&flags,info+infoFlags,4);if((flags>>13)&7u)return false;
    const unsigned char* d{};std::memcpy(&d,info+infoDrawable,sizeof d);if(!d)return false;
    float m[16],c[3],r{};std::memcpy(m,info,sizeof m);std::memcpy(c,d+0x20,sizeof c);std::memcpy(&r,d+0x90,4);
    for(unsigned i=0;i<3;++i)out.c[i]=c[0]*m[i]+c[1]*m[4+i]+c[2]*m[8+i]+m[12+i];
    out.r=r;return sane(out);
}
inline bool nearPlayer(const Sphere& s,const Sphere& player,float extra) noexcept {
    const float dx=s.c[0]-player.c[0],dy=s.c[1]-player.c[1],dz=s.c[2]-player.c[2],lim=player.r+extra;
    return dx*dx+dy*dy+dz*dz<=lim*lim;
}

// ---- tile matrix -> atlas uv ----
// The tile matrix tile+0x10..0x4f (16 floats) maps a world point to atlas texture coordinates (the receiver's +0x58 array).
// BeginTileAroundSphere builds it as rows (stp q1,q2 / q3,q0 = vp rows x bias x window), i.e. row-vector convention
// out = x*T[0..3] + y*T[4..7] + z*T[8..11] + T[12..15] (convention 0). The convention is NOT assumed: conventionFor()
// accepts the first of {row, row with v flipped, column, column with v flipped} that maps the tile's own sphere centre
// onto the centre of the tile's learned quadrant (reported as player.convention; -1 = none -> no window).
inline constexpr int conventionCount=4;
inline bool project(const float* T,int conv,const float* p,float& u,float& v) noexcept {
    float o[4];
    if(conv<2)for(unsigned i=0;i<4;++i)o[i]=p[0]*T[i]+p[1]*T[4+i]+p[2]*T[8+i]+T[12+i];
    else for(unsigned i=0;i<4;++i)o[i]=p[0]*T[4*i]+p[1]*T[4*i+1]+p[2]*T[4*i+2]+T[4*i+3];
    if(!std::isfinite(o[3])||!(std::fabs(o[3])>1e-6f))return false;
    u=o[0]/o[3];v=o[1]/o[3];if(conv&1)v=1.f-v;
    return std::isfinite(u)&&std::isfinite(v)&&std::fabs(u)<1e4f&&std::fabs(v)<1e4f;
}
// |grad u|, |grad v| per metre at p (numerical; ortho -> constant).
inline bool scale(const float* T,int conv,const float* p,float& su,float& sv) noexcept {
    float u0{},v0{};if(!project(T,conv,p,u0,v0))return false;
    float du2=0.f,dv2=0.f;
    for(unsigned a=0;a<3;++a){float q[3]{p[0],p[1],p[2]};q[a]+=1.f;float u{},v{};if(!project(T,conv,q,u,v))return false;
        du2+=(u-u0)*(u-u0);dv2+=(v-v0)*(v-v0);}
    su=std::sqrt(du2);sv=std::sqrt(dv2);return std::isfinite(su)&&std::isfinite(sv)&&su>0.f&&sv>0.f;
}
struct FRect {float x0{},y0{},x1{},y1{};bool valid{};};
inline FRect unite(const FRect& a,const FRect& b) noexcept {
    if(!a.valid)return b;if(!b.valid)return a;
    return {std::min(a.x0,b.x0),std::min(a.y0,b.y0),std::max(a.x1,b.x1),std::max(a.y1,b.y1),true};
}
inline bool overlaps(const FRect& a,const FRect& b) noexcept {return a.valid&&b.valid&&a.x0<b.x1&&b.x0<a.x1&&a.y0<b.y1&&b.y0<a.y1;}
// Atlas pixel square of a sphere (radius + margin) through the tile matrix.
inline bool sphereRect(const float* T,int conv,unsigned width,unsigned height,const Sphere& s,float margin,FRect& out) noexcept {
    float u{},v{},su{},sv{};if(!project(T,conv,s.c,u,v)||!scale(T,conv,s.c,su,sv))return false;
    const float R=s.r+margin,W=float(width),H=float(height);
    out={(u-R*su)*W,(v-R*sv)*H,(u+R*su)*W,(v+R*sv)*H,true};
    return std::isfinite(out.x0)&&std::isfinite(out.y0)&&std::isfinite(out.x1)&&std::isfinite(out.y1);
}
// entry = the tile's sphere entry {x,y,z,r^2} (CShadowMap+0x130+16t as committed with the matrix).
inline int conventionFor(const float* T,const float* entry,const Rect& quad,unsigned width,unsigned height) noexcept {
    if(quad.w<=0||quad.h<=0||!width||!height||!(entry[3]>0.f)||!std::isfinite(entry[3]))return -1;
    const Sphere s{{entry[0],entry[1],entry[2]},std::sqrt(entry[3])};if(!sane(s))return -1;
    const float qx=float(quad.x)+float(quad.w)*.5f,qy=float(quad.y)+float(quad.h)*.5f;
    for(int conv=0;conv<conventionCount;++conv){
        float u{},v{},su{},sv{};if(!project(T,conv,s.c,u,v)||!scale(T,conv,s.c,su,sv))continue;
        const float px=u*float(width),py=v*float(height);
        if(std::fabs(px-qx)>.2f*float(quad.w)||std::fabs(py-qy)>.2f*float(quad.h))continue;
        const float rx=s.r*su*float(width)/float(quad.w),ry=s.r*sv*float(height)/float(quad.h); // ~0.5 when the ortho box fits the sphere
        if(rx<.1f||rx>2.f||ry<.1f||ry>2.f)continue;
        return conv;
    }
    return -1;
}
inline void predict(const SphereSet& last,const float* d,SphereSet& out) noexcept {
    out=last;for(unsigned i=0;i<out.n;++i)for(unsigned a=0;a<3;++a)out.s[i].c[a]+=d[a];
}
inline bool sameMatrix(const unsigned char* a,const unsigned char* b) noexcept {
    float x[16],y[16];std::memcpy(x,a,64);std::memcpy(y,b,64);
    for(unsigned i=0;i<16;++i){if(!std::isfinite(x[i])||!std::isfinite(y[i]))return false;
        if(std::fabs(x[i]-y[i])>1e-4f*std::max(1.f,std::max(std::fabs(x[i]),std::fabs(y[i]))))return false;}
    return true;
}

// ---- the window ----
enum Reason : unsigned {Ok,Off,NoVp,NotLearned,Projection,NoPlayer,Overflow,PlayerOutside,Empty,TooLarge,MatrixMismatch,Proxy,EarlyPass,ReasonCount};
inline constexpr const char* reasonNames[ReasonCount]{"ok","off","noVp","notLearned","projection","noPlayer","overflow","playerOutside",
                                                      "empty","tooLarge","matrixMismatch","proxy","earlyPass"};
struct WindowInput {
    const float* T{};int conv{-1};unsigned width{},height{};Rect quad{};
    const SphereSet* predicted{};const SphereSet* content{};
    float margin{.25f},contentMargin{.05f},maxFraction{.3f};
};
// out = the integer pixel rect (inside the quadrant) covering every predicted sphere (+margin) and every content sphere
// (+contentMargin); the predicted primary sphere centre must lie inside the quadrant (else the player is not in this tile).
inline unsigned window(const WindowInput& in,Rect& out,FRect& outF) noexcept {
    if(!in.predicted||!in.predicted->n)return NoPlayer;
    if(in.predicted->overflow||(in.content&&in.content->overflow))return Overflow;
    FRect u{};
    for(unsigned i=0;i<in.predicted->n;++i){FRect r{};if(!sphereRect(in.T,in.conv,in.width,in.height,in.predicted->s[i],in.margin,r))return Projection;u=unite(u,r);}
    {const auto& p=in.predicted->s[unsigned(in.predicted->primary())];float pu{},pv{};
     if(!project(in.T,in.conv,p.c,pu,pv))return Projection;
     const float px=pu*float(in.width),py=pv*float(in.height);
     if(px<float(in.quad.x)||py<float(in.quad.y)||px>=float(in.quad.x+in.quad.w)||py>=float(in.quad.y+in.quad.h))return PlayerOutside;}
    if(in.content)for(unsigned i=0;i<in.content->n;++i){FRect r{};if(!sphereRect(in.T,in.conv,in.width,in.height,in.content->s[i],in.contentMargin,r))return Projection;u=unite(u,r);}
    const float qx0=float(in.quad.x),qy0=float(in.quad.y),qx1=float(in.quad.x+in.quad.w),qy1=float(in.quad.y+in.quad.h);
    const int x0=int(std::floor(std::clamp(u.x0,qx0,qx1))),y0=int(std::floor(std::clamp(u.y0,qy0,qy1)));
    const int x1=int(std::ceil(std::clamp(u.x1,qx0,qx1))),y1=int(std::ceil(std::clamp(u.y1,qy0,qy1)));
    if(x1<=x0||y1<=y0)return Empty;
    out={x0,y0,x1-x0,y1-y0};outF={float(x0),float(y0),float(x1),float(y1),true};
    if(float(out.w)*float(out.h)>in.maxFraction*float(in.quad.w)*float(in.quad.h))return TooLarge;
    return Ok;
}
// Window tile filter: does this record's bound touch the window? (projection failure -> true: drawn, scissor-clipped)
inline bool touches(const float* T,int conv,unsigned width,unsigned height,const Sphere& s,const FRect& window) noexcept {
    FRect r{};if(!sphereRect(T,conv,width,height,s,0.f,r))return true;
    return overlaps(r,window);
}
// Integer rect intersection (empty -> w = h = 0 at a's origin clamped into b).
inline Rect intersect(const Rect& a,const Rect& b) noexcept {
    const int x0=std::max(a.x,b.x),y0=std::max(a.y,b.y),x1=std::min(a.x+a.w,b.x+b.w),y1=std::min(a.y+a.h,b.y+b.h);
    if(x1<=x0||y1<=y0)return {std::clamp(a.x,b.x,b.x+b.w),std::clamp(a.y,b.y,b.y+b.h),0,0};
    return {x0,y0,x1-x0,y1-y0};
}
} // namespace rdrvr::engine::shadowupdate::stagger::player
