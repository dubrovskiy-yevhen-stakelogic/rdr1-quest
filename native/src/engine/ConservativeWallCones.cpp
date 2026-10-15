#include "ConservativeWallCones.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace rdrvr::engine {
namespace {
constexpr double E=std::numeric_limits<double>::epsilon();
constexpr double Infinity=std::numeric_limits<double>::infinity();
bool error(const ConservativeStereoRaster::Errors& e)noexcept {
    return std::isfinite(e.coverageCells)&&e.coverageCells>=1e-4&&e.coverageCells<64&&
           std::isfinite(e.depth)&&e.depth>=1e-7&&e.depth<.25;
}
bool key(const ConservativeWallCones::SourceKey& k)noexcept {
    return k.object&&k.geometry&&k.material&&k.transform&&k.geometryGeneration&&
           k.materialGeneration&&k.transformGeneration&&k.record;
}
double edge(double ax,double ay,double bx,double by,double x,double y)noexcept {
    return (bx-ax)*(y-ay)-(by-ay)*(x-ax);
}
double roundoff(double ax,double ay,double bx,double by,double x,double y)noexcept {
    return 128*E*(1+std::abs((bx-ax)*(y-ay))+std::abs((by-ay)*(x-ax)));
}
}
void ConservativeWallCones::reset()noexcept {
    frame_={};authority_={};count_=0;built_=false;stats_={};revision_=0;
    if(buildSerial_==UINT64_MAX)serialExhausted_=true;else ++buildSerial_;
}
bool ConservativeWallCones::ready(Frame f)const noexcept {
    return built_&&f.valid()&&f==frame_&&authority_.frameCurrent&&
           authority_.frameCurrent(authority_.context,frame_);
}
bool ConservativeWallCones::project(unsigned eye,const Raster::Vec3& v,P& out)const noexcept {
    if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z))return false;
    const auto& m=matrices_[eye];
    const double x=m[0]*v.x+m[1]*v.y+m[2]*v.z+m[3];
    const double y=m[4]*v.x+m[5]*v.y+m[6]*v.z+m[7];
    const double z=m[8]*v.x+m[9]*v.y+m[10]*v.z+m[11];
    const double w=m[12]*v.x+m[13]*v.y+m[14]*v.z+m[15];
    if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||!std::isfinite(w)||w<=1e-8)return false;
    out={(x/w+1)*32,(y/w+1)*32,z/w,w};
    return std::isfinite(out.x)&&std::isfinite(out.y)&&std::isfinite(out.z)&&
           std::abs(out.x)<=1e9&&std::abs(out.y)<=1e9&&out.z>0&&out.z<1;
}
bool ConservativeWallCones::source(unsigned eye,const Source& source,EyeWall& out)noexcept {
    out.valid=false;out.count=0;
    const auto& polygon=source.polygon;const auto& e=source.errors[eye];
    P p[Raster::MaxPatchVertices];
    for(unsigned i=0;i<polygon.count;++i)if(!project(eye,polygon.vertex[i],p[i])||
       p[i].z<=e.depth||p[i].z>=1-e.depth){++stats_.sourceClipped;return false;}
    const auto a=edge(p[0].x,p[0].y,p[1].x,p[1].y,p[2].x,p[2].y);
    if(!std::isfinite(a)||std::abs(a)<=roundoff(p[0].x,p[0].y,p[1].x,p[1].y,p[2].x,p[2].y)){
        ++stats_.sourceDegenerate;return false;
    }
    if((a<0&&polygon.faces==Raster::Faces::PositiveArea)||(a>0&&polygon.faces==Raster::Faces::NegativeArea)){
        ++stats_.sourceBackfaces;return false;
    }
    if(a<0)std::reverse(p,p+polygon.count);
    double maxZ=0;
    for(unsigned i=0;i<polygon.count;++i){
        const auto& v=p[i];const auto& next=p[(i+1)%polygon.count];maxZ=std::max(maxZ,v.z);
        // Every non-edge vertex must lie strictly inside this edge. This also
        // rejects self-intersecting windings; holes cannot be invented by hulls.
        for(unsigned j=0;j<polygon.count;++j){
            if(j==i||j==(i+1)%polygon.count)continue;
            const double signedArea=edge(v.x,v.y,next.x,next.y,p[j].x,p[j].y);
            if(!std::isfinite(signedArea)||signedArea<=roundoff(v.x,v.y,next.x,next.y,p[j].x,p[j].y)){
                ++stats_.sourceDegenerate;return false;
            }
        }
        auto& line=out.edges[i];line.ax=v.x;line.ay=v.y;line.dx=next.x-v.x;line.dy=next.y-v.y;
        const double scale=(std::abs(line.dx)+std::abs(line.dy))*(std::abs(v.x)+std::abs(v.y)+129);
        line.margin=std::nextafter(e.coverageCells*std::hypot(line.dx,line.dy)+128*E*std::max(1.,scale),Infinity);
        if(!std::isfinite(line.margin)){++stats_.sourceDegenerate;return false;}
    }
    out.farther=std::nextafter(maxZ+e.depth+128*E,Infinity);
    if(!std::isfinite(out.farther)||out.farther<=0||out.farther>=1){++stats_.sourceClipped;return false;}
    out.count=polygon.count;out.valid=true;++stats_.sourceEyes;return true;
}
bool ConservativeWallCones::build(Frame f,const std::array<Raster::Matrix,2>& matrices,
                                  std::span<const Source> sources,Authority authority)noexcept {
    reset();
    if(serialExhausted_||!f.valid()||sources.size()>MaxWalls||!authority.frameCurrent||!authority.sourceCurrent||
       !authority.frameCurrent(authority.context,f))return false;
    for(const auto& m:matrices)for(double x:m)if(!std::isfinite(x))return false;
    frame_=f;matrices_=matrices;authority_=authority;
    built_=true;
    for(const auto& s:sources)append(f,s);
    if(!authority.frameCurrent(authority.context,f)){reset();return false;}
    return true;
}
bool ConservativeWallCones::append(Frame f,const Source& s)noexcept {
    if(!ready(f)){++stats_.stale;return false;}
    if(stats_.sources>=MaxWalls){++stats_.budget;return false;}
    ++stats_.sources;
    if(!key(s.key)||!authority_.sourceCurrent(authority_.context,f,s.key)){++stats_.admissionDeclined;return false;}
    if(s.polygon.count<3||s.polygon.count>Raster::MaxPatchVertices||
       (s.polygon.faces!=Raster::Faces::Both&&s.polygon.faces!=Raster::Faces::PositiveArea&&s.polygon.faces!=Raster::Faces::NegativeArea)||
       !error(s.errors[0])||!error(s.errors[1])){++stats_.invalidSources;return false;}
    auto& wall=walls_[count_];wall.eye[0].valid=wall.eye[1].valid=false;
    bool any=false;for(unsigned eye=0;eye<2;++eye)any=source(eye,s,wall.eye[eye])||any;
    if(!ready(f)){++stats_.stale;return false;}
    if(any){++count_;++revision_;}
    return any;
}
bool ConservativeWallCones::candidate(unsigned eye,const Raster::Bounds& b,const Raster::Errors& e,
                                       double& lx,double& hx,double& ly,double& hy,double& near)const noexcept {
    lx=ly=Infinity;hx=hy=-Infinity;near=1;
    for(unsigned i=0;i<8;++i){
        const Raster::Vec3 v{(i&1)?b.maximum.x:b.minimum.x,(i&2)?b.maximum.y:b.minimum.y,(i&4)?b.maximum.z:b.minimum.z};P p;
        if(!project(eye,v,p)||p.z<=e.depth||p.z>=1-e.depth)return false;
        lx=std::min(lx,p.x);hx=std::max(hx,p.x);ly=std::min(ly,p.y);hy=std::max(hy,p.y);near=std::min(near,p.z);
    }
    lx=std::nextafter(lx-e.coverageCells,-Infinity);hx=std::nextafter(hx+e.coverageCells,Infinity);
    ly=std::nextafter(ly-e.coverageCells,-Infinity);hy=std::nextafter(hy+e.coverageCells,Infinity);
    if(hx<0||hy<0||lx>64||ly>64)return false;
    lx=std::max(0.,lx);hx=std::min(64.,hx);ly=std::max(0.,ly);hy=std::min(64.,hy);
    near=std::nextafter(near-e.depth-128*E*(1+std::abs(near)),-Infinity);
    return std::isfinite(near)&&near>0&&lx<=hx&&ly<=hy;
}
std::uint8_t ConservativeWallCones::hidden(Frame f,const Raster::Bounds& b,
                                           const std::array<Raster::Errors,2>& errors)noexcept {
    if(!ready(f)){++stats_.stale;return 0;}
    if(stats_.candidates>=MaxQueries){++stats_.budget;return 0;}
    ++stats_.candidates;
    if(!error(errors[0])||!error(errors[1])||!std::isfinite(b.minimum.x)||!std::isfinite(b.minimum.y)||!std::isfinite(b.minimum.z)||
       !std::isfinite(b.maximum.x)||!std::isfinite(b.maximum.y)||!std::isfinite(b.maximum.z)||
       b.minimum.x>b.maximum.x||b.minimum.y>b.maximum.y||b.minimum.z>b.maximum.z){++stats_.invalidCandidates;return 0;}
    std::uint8_t mask=0;
    for(unsigned eye=0;eye<2;++eye){
        double lx,hx,ly,hy,near;
        if(!candidate(eye,b,errors[eye],lx,hx,ly,hy,near)){++stats_.invalidCandidates;continue;}
        for(unsigned i=0;i<count_;++i){
            const auto& wall=walls_[i].eye[eye];++stats_.eyeTests;
            if(!wall.valid||!(wall.farther<near))continue;
            bool inside=true;
            for(unsigned j=0;j<wall.count;++j){
                ++stats_.planeTests;const auto& line=wall.edges[j];
                const double x=line.dy>=0?hx:lx,y=line.dx>=0?ly:hy;
                const double distance=line.dx*(y-line.ay)-line.dy*(x-line.ax);
                if(!std::isfinite(distance)||distance<=line.margin){inside=false;break;}
            }
            if(inside){mask|=std::uint8_t(1u<<eye);break;}
        }
    }
    // Revocation during this bounded query cannot publish a stale decision.
    if(!ready(f)){++stats_.stale;return 0;}
    if(mask==3)++stats_.bothEyes;else if(mask)++stats_.oneEye;return mask;
}
}


