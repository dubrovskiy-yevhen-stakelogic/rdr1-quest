#pragma once
// HORSE PARTS - the chain selection (pure: no engine access; VrBody steady::horseMotionParts reads the skeleton and caches the
// result per skeleton data; host-testable). The mount's 4 legs, neck and head as capsules along its skeleton CHAINS, in the root frame
// (up = the world up, forward = -root.c), from the CURRENT pose of a STANDING horse (all 4 hooves down; the caller retries):
//   hooves   = per quadrant (front / back x left / right of the root) the deepest LEAF bone < 0.30 m over the root (then the lowest; an
//              IK / helper bone parented to the root is shallow);
//   a leg    = the hoof's ancestors up to (not incl.) the first bone shared with another hoof (the spine / pelvis);
//   head tip = the bone farthest ahead (> 0.6 m up, > 0.8 m ahead, on no leg); the neck = its ancestors up to (not incl.) the first leg
//              ancestor; the head capsule = the tip and its first ancestor >= 0.30 m behind it (the skull pivot = the carrier).
// A chain becomes <= 3 capsules (joints < 3 cm from the last kept one skipped, then the shortest segment merged with its shorter
// neighbour), each carried by its UPPER joint's bone. Legs <= 12 + neck <= 3 + head 1 <= 16 parts.
#include "VrArmIk.h"
#include <algorithm>
#include <array>
#include <cstdint>
namespace rdrvr::vr::horsechains {
namespace A=::rdrvr::vr::armik;
inline constexpr unsigned maxSegs=16;inline constexpr int maxBones=400;
struct Seg {std::int16_t a{-1},b{-1};float radius{},extend{},len{};}; // joint a (carrier) -> joint b, + extend past b (the hoof / muzzle)
struct Chains {bool ok{};const char* why{"NOT_RUN"};std::array<Seg,maxSegs> seg{};unsigned segs{},legSegs{},neckSegs{};};
// par[i] = parent index (-1 = root), globals = count world matrices (stride 16 floats, rows a, b, c, d + w padding), root = the skeleton root
inline Chains select(const std::int32_t* par,int count,const float* globals,const A::Mat34& root) noexcept {
    Chains hc{};
    const auto fail=[&](const char* why){hc=Chains{};hc.why=why;return hc;};
    if(!par||!globals||count<8||count>maxBones)return fail("COUNT");
    static std::array<A::Vec3,maxBones> P{};static std::array<std::uint8_t,maxBones> kids{},anc{}; // the caller serializes calls
    const A::Mat34 inv=A::InverseAffine(root);
    const A::Vec3 up=A::Normalize(A::TransformDir(inv,A::Vec3{0.f,1.f,0.f}),{0.f,1.f,0.f}),f0=A::TransformDir(inv,root.c*-1.f);
    const A::Vec3 fwd=A::Normalize(f0-up*A::Dot(f0,up),{0.f,0.f,-1.f}),side=A::Cross(up,fwd);
    for(int i=0;i<count;++i){kids[std::size_t(i)]=0;anc[std::size_t(i)]=0;}
    for(int i=0;i<count;++i){P[std::size_t(i)]=A::TransformPoint(inv,A::LoadMat34(globals+std::size_t(i)*16).d);if(!A::Finite(P[std::size_t(i)]))return fail("POSE");
        const int p=par[i];if(p<-1||p>=count||p==i)return fail("PARENT_RANGE");if(p>=0&&kids[std::size_t(p)]<255)++kids[std::size_t(p)];}
    const auto h=[&](int i){return A::Dot(P[std::size_t(i)],up);};const auto f=[&](int i){return A::Dot(P[std::size_t(i)],fwd);};
    const auto s=[&](int i){return A::Dot(P[std::size_t(i)],side);};const auto dist=[&](int i,int j){return A::Length(P[std::size_t(i)]-P[std::size_t(j)]);};
    // hooves: 0 front +side, 1 front -side, 2 back +side, 3 back -side
    std::array<int,4> hoof{-1,-1,-1,-1},depth{};
    for(int i=0;i<count;++i){if(kids[std::size_t(i)]||!(h(i)<.30f))continue;const int q=(f(i)>0.f?0:2)+(s(i)>0.f?0:1);
        int d=0;for(int b=par[i];b>=0&&d<64;b=par[b])++d;auto& w=hoof[std::size_t(q)];
        if(w<0||d>depth[std::size_t(q)]||(d==depth[std::size_t(q)]&&h(i)<h(w))){w=i;depth[std::size_t(q)]=d;}}
    for(int q:hoof)if(q<0)return fail("HOOVES");
    if(!(f(hoof[0])-f(hoof[2])>.6f&&f(hoof[1])-f(hoof[3])>.6f&&s(hoof[0])-s(hoof[1])>.12f&&s(hoof[2])-s(hoof[3])>.12f))return fail("HOOF_SPREAD");
    for(unsigned q=0;q<4;++q){int b=hoof[q];for(int d=0;b>=0&&d<64;++d){anc[std::size_t(b)]|=std::uint8_t(1u<<q);b=par[b];}if(b>=0)return fail("DEPTH");}
    // joints top -> leaf (c[0..n)) -> <= most capsules carried by their upper joint; r0 = the top capsule's radius, r1 the others
    const auto addJoints=[&](const int* c,int n,int most,float r0,float r1,float extend){
        std::array<int,64> j{};int m=0;
        for(int k=0;k<n&&k<64;++k){if(m&&dist(c[k],j[std::size_t(m-1)])<.03f)continue;j[std::size_t(m++)]=c[k];}
        if(m<2)return false;
        while(m-1>most){int best=0;float bl=1e9f;
            for(int k=0;k+1<m;++k){const float l=dist(j[std::size_t(k)],j[std::size_t(k+1)]);if(l<bl){bl=l;best=k;}}
            const float lp=best>0?dist(j[std::size_t(best-1)],j[std::size_t(best)]):1e9f,ln=best+2<m?dist(j[std::size_t(best+1)],j[std::size_t(best+2)]):1e9f;
            const int drop=lp<ln?best:best+1; // the joint shared with the shorter neighbour (never the top / the leaf)
            for(int k=drop;k+1<m;++k)j[std::size_t(k)]=j[std::size_t(k+1)];--m;}
        for(int k=0;k+1<m;++k){if(hc.segs>=maxSegs)return false;auto& sg=hc.seg[hc.segs++];sg.a=std::int16_t(j[std::size_t(k)]);sg.b=std::int16_t(j[std::size_t(k+1)]);
            sg.len=dist(sg.a,sg.b);sg.radius=k==0?r0:r1;sg.extend=k+2==m?extend:0.f;}
        return true;};
    std::array<int,64> c{};
    for(unsigned q=0;q<4;++q){int n=0; // leaf .. top, then reversed
        for(int b=hoof[q];b>=0&&n<64&&anc[std::size_t(b)]==(1u<<q);b=par[b])c[std::size_t(n++)]=b;
        if(n<3)return fail("LEG_CHAIN");std::reverse(c.begin(),c.begin()+n);
        if(!addJoints(c.data(),n,3,.11f,.07f,.05f))return fail("LEG_SEGS");}
    hc.legSegs=hc.segs;
    int tip=-1;for(int i=0;i<count;++i)if(!anc[std::size_t(i)]&&h(i)>.6f&&(tip<0||f(i)>f(tip)))tip=i;
    if(tip<0||!(f(tip)>.8f))return fail("HEAD_TIP");
    int n=0;for(int b=tip;b>=0&&n<64&&!anc[std::size_t(b)];b=par[b])c[std::size_t(n++)]=b; // tip .. the bone under the chest
    int poll=-1;for(int k=1;k<n;++k)if(dist(c[std::size_t(k)],tip)>=.30f){poll=k;break;}
    if(poll<0||n-poll<2||!(f(tip)-f(c[std::size_t(n-1)])>.5f))return fail("NECK_CHAIN");
    const int pollBone=c[std::size_t(poll)];std::reverse(c.begin(),c.begin()+n); // top .. tip; the poll at n-1-poll
    if(!addJoints(c.data(),n-poll,3,.18f,.16f,0.f))return fail("NECK_SEGS");
    hc.neckSegs=hc.segs-hc.legSegs;
    if(hc.segs>=maxSegs)return fail("HEAD_SEG");
    {auto& sg=hc.seg[hc.segs++];sg.a=std::int16_t(pollBone);sg.b=std::int16_t(tip);sg.len=dist(pollBone,tip);sg.radius=.14f;sg.extend=.05f;}
    hc.ok=true;hc.why="OK";return hc;
}
} // namespace rdrvr::vr::horsechains
