#include "ConservativeStereoRaster.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace rdrvr::engine {
namespace {
constexpr double Epsilon = std::numeric_limits<double>::epsilon();
bool finite(double x) noexcept { return std::isfinite(x); }
bool finite(const ConservativeStereoRaster::Vec3& v) noexcept {
    return finite(v.x) && finite(v.y) && finite(v.z);
}
double edge(double ax, double ay, double bx, double by, double x, double y) noexcept {
    return (bx - ax) * (y - ay) - (by - ay) * (x - ax);
}
bool range(double low, double high, unsigned limit, int& first, int& last) noexcept {
    if (high < 0 || low > limit || low > high) return false;
    first = static_cast<int>(std::floor(std::clamp(low, 0.0, double(limit - 1))));
    last = static_cast<int>(std::floor(std::clamp(high, 0.0, double(limit - 1))));
    return true;
}
}

void ConservativeStereoRaster::reset() noexcept {
    frame_ = 0; sealed_ = false; stats_ = {}; ++generation_;
    for (auto& d : depth_) d.fill(1.f);
    for (auto& tiles:sourceTiles_) tiles.fill(1.f);
    sourceTileDirty_={};
    for(auto& count:sourceTileMaxCount_)count.fill(64);
}
bool ConservativeStereoRaster::begin(std::uint64_t frame, const std::array<Matrix, Eyes>& views,
                                     const std::array<Errors, Eyes>& errors, Budget budget) noexcept {
    reset();
    if (!frame || !budget.triangles || !budget.rasterCells || !budget.candidates || !budget.queryCells)
        return false;
    for (unsigned eye = 0; eye < Eyes; ++eye) {
        for (double v : views[eye]) if (!finite(v)) return false;
        // These minima are numerical guardrails, not a claimed GPU error bound.
        if (!finite(errors[eye].coverageCells) || errors[eye].coverageCells < 1e-4 ||
            errors[eye].coverageCells >= Width || !finite(errors[eye].depth) ||
            errors[eye].depth < 1e-7 || errors[eye].depth >= 0.25) return false;
    }
    frame_ = frame; views_ = views; errors_ = errors; budget_ = budget;
    return true;
}
bool ConservativeStereoRaster::project(unsigned eye, const Vec3& p, Projected& out) const noexcept {
    return projectRaw(eye,p,out)&&out.z>errors_[eye].depth&&out.z<1-errors_[eye].depth;
}
bool ConservativeStereoRaster::projectRaw(unsigned eye,const Vec3& p,Projected& out)const noexcept {
    if (!finite(p)) return false;
    const auto& m = views_[eye];
    const double x = m[0]*p.x + m[1]*p.y + m[2]*p.z + m[3];
    const double y = m[4]*p.x + m[5]*p.y + m[6]*p.z + m[7];
    const double z = m[8]*p.x + m[9]*p.y + m[10]*p.z + m[11];
    const double w = m[12]*p.x + m[13]*p.y + m[14]*p.z + m[15];
    if (!finite(x) || !finite(y) || !finite(z) || !finite(w) || w <= 1e-8) return false;
    out = {(x/w + 1)*Width/2, (y/w + 1)*Height/2, z/w,w};
    // Near/far-crossing inputs are intentionally retained. Coverage is not extrapolated
    // through clip intersections, and a cancelling far plane is not normalized.
    return finite(out.x) && finite(out.y) && finite(out.z) &&
        std::abs(out.x) <= 1e9 && std::abs(out.y) <= 1e9 && out.z > 0 && out.z < 1;
}
bool ConservativeStereoRaster::submit(std::uint64_t frame, const Triangle& t) noexcept {
    if (!frame || frame != frame_ || sealed_) { ++stats_.staleCalls; return false; }
    if (stats_.triangles >= budget_.triangles || stats_.rasterCells >= budget_.rasterCells) {
        ++stats_.rasterBudgetStops; return false;
    }
    ++stats_.triangles;
    if (t.faces != Faces::Both && t.faces != Faces::PositiveArea && t.faces != Faces::NegativeArea) {
        ++stats_.invalidTriangles; return false;
    }
    for (const auto& v : t.vertex) if (!finite(v)) { ++stats_.invalidTriangles; return false; }
    bool added = false;
    for (unsigned eye = 0; eye < Eyes; ++eye) added = rasterize(eye, t) || added;
    return added;
}
bool ConservativeStereoRaster::submit(std::uint64_t frame,const Triangle& t,const std::array<Errors,Eyes>& errors) noexcept {
    for(const auto& e:errors)if(!finite(e.coverageCells)||e.coverageCells<1e-4||e.coverageCells>=Width||
        !finite(e.depth)||e.depth<1e-7||e.depth>=.25)return false;
    const auto old=errors_;errors_=errors;const bool result=submit(frame,t);errors_=old;return result;
}
bool ConservativeStereoRaster::submit(std::uint64_t frame,const Patch& t)noexcept {
    if(!frame||frame!=frame_||sealed_){++stats_.staleCalls;return false;}
    if(stats_.triangles>=budget_.triangles||stats_.rasterCells>=budget_.rasterCells){++stats_.rasterBudgetStops;return false;}
    ++stats_.triangles;
    if(t.count<3||t.count>MaxPatchVertices||(t.faces!=Faces::Both&&t.faces!=Faces::PositiveArea&&t.faces!=Faces::NegativeArea)){++stats_.invalidTriangles;return false;}
    for(unsigned i=0;i<t.count;++i)if(!finite(t.vertex[i])){++stats_.invalidTriangles;return false;}
    bool added=false;for(unsigned eye=0;eye<Eyes;++eye)added=rasterize(eye,t.vertex.data(),t.count,t.faces)||added;return added;
}
bool ConservativeStereoRaster::submit(std::uint64_t frame,const Patch&t,const std::array<Errors,Eyes>&errors)noexcept {
    for(const auto&e:errors)if(!finite(e.coverageCells)||e.coverageCells<1e-4||e.coverageCells>=Width||!finite(e.depth)||e.depth<1e-7||e.depth>=.25)return false;
    const auto old=errors_;errors_=errors;const bool result=submit(frame,t);errors_=old;return result;
}
bool ConservativeStereoRaster::rasterize(unsigned eye, const Triangle& t) noexcept {
    return rasterize(eye,t.vertex.data(),3,t.faces);
}
bool ConservativeStereoRaster::rasterize(unsigned eye,const Vec3* vertices,unsigned count,Faces faces)noexcept {
    Projected p[MaxPatchVertices];
    for (unsigned i = 0; i < count; ++i) if (!project(eye, vertices[i], p[i])) {
        ++stats_.clippedTriangles; return false;
    }
    double area = edge(p[0].x,p[0].y,p[1].x,p[1].y,p[2].x,p[2].y);
    const double areaRoundoff = 128*Epsilon*(1 +
        std::abs((p[1].x-p[0].x)*(p[2].y-p[0].y)) +
        std::abs((p[1].y-p[0].y)*(p[2].x-p[0].x)));
    if (!finite(area) || std::abs(area) <= areaRoundoff) { ++stats_.invalidTriangles; return false; }
    if ((area < 0 && faces == Faces::PositiveArea) || (area > 0 && faces == Faces::NegativeArea)) {
        ++stats_.culledFaces; return false;
    }
    if (area < 0) { std::reverse(p,p+count); area = -area; }
    for(unsigned i=0;i<count;++i){const auto&a=p[i];const auto&b=p[(i+1)%count];const auto&c=p[(i+2)%count];
        const double v=edge(a.x,a.y,b.x,b.y,c.x,c.y);
        const double roundoff=128*Epsilon*(1+std::abs((b.x-a.x)*(c.y-a.y))+std::abs((b.y-a.y)*(c.x-a.x)));
        if(!finite(v)||v<=roundoff){++stats_.invalidTriangles;return false;}}
    double lowX=p[0].x,highX=p[0].x,lowY=p[0].y,highY=p[0].y,maxZ=p[0].z;
    for(unsigned i=1;i<count;++i){lowX=std::min(lowX,p[i].x);highX=std::max(highX,p[i].x);lowY=std::min(lowY,p[i].y);highY=std::max(highY,p[i].y);maxZ=std::max(maxZ,p[i].z);}
    int x0, x1, y0, y1;
    if (!range(lowX, highX, Width,x0,x1) ||
        !range(lowY, highY, Height,y0,y1)) return false;
    // The maximum vertex depth bounds the whole projected triangle when w>0.
    // Using this bound instead of dividing a nearly degenerate plane by its
    // area avoids precision amplification on thin or almost edge-on geometry.
    const double z = maxZ + errors_[eye].depth + 128*Epsilon;
    if (!finite(z) || z >= 1 || z <= 0) return false;
    float farther = static_cast<float>(z);
    if (farther < z) farther = std::nextafter(farther, 1.f);
    double margin[MaxPatchVertices];
    for (unsigned k = 0; k < count; ++k) {
        const auto& a = p[k]; const auto& b = p[(k+1)%count];
        // Erosion by the certified vertex displacement is conservative for
        // convex hulls. Extra roundoff allowance covers the edge evaluation.
        const double scale = (std::abs(b.x-a.x)+std::abs(b.y-a.y)) *
            (std::abs(a.x)+std::abs(a.y)+Width+Height+1);
        margin[k] = errors_[eye].coverageCells * std::hypot(b.x-a.x,b.y-a.y) +
            128*Epsilon*std::max(1.0,scale);
    }
    bool added = false;
    for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
        if (stats_.rasterCells >= budget_.rasterCells) { ++stats_.rasterBudgetStops; return added; }
        ++stats_.rasterCells;
        bool covered = true;
        for (unsigned k = 0; k < count && covered; ++k) {
            const auto& a = p[k]; const auto& b = p[(k+1)%count];
            // Linear edge function: its minimum over a square is at a corner.
            const double cx = (b.y-a.y) >= 0 ? x+1.0 : x;
            const double cy = (b.x-a.x) >= 0 ? y : y+1.0;
            covered = edge(a.x,a.y,b.x,b.y,cx,cy) > margin[k];
        }
        if (!covered) continue;
        ++stats_.coveredCells;
        // Round toward farther depth; never invent a closer blocker by float
        // conversion. Each stored depth belongs to ONE fully covering triangle.
        auto& old = depth_[eye][unsigned(y)*Width+unsigned(x)];
        if (farther < old) {
            if(old==1.f)++stats_.uniqueCoveredCells[eye];
            sourceCellDecrease(eye,unsigned(x),unsigned(y),old);
            old = farther; ++stats_.depthUpdates; added = true;
        }
    }
    return added;
}
bool ConservativeStereoRaster::prepare(std::uint64_t frame,const Patch&t,PreparedPatch&out)noexcept {
    return prepare(frame,t.vertex.data(),t.count,t.faces,out);
}
bool ConservativeStereoRaster::prepare(std::uint64_t frame,const Triangle&t,PreparedPatch&out)noexcept {
    return prepare(frame,t.vertex.data(),3,t.faces,out);
}
bool ConservativeStereoRaster::prepare(std::uint64_t frame,const Vec3* vertices,unsigned count,Faces faces,PreparedPatch&out)noexcept {
    out.owner_=nullptr;out.mask_=0;
    if(!frame||frame!=frame_||sealed_){++stats_.staleCalls;return false;}
    if(stats_.preparations>=budget_.triangles||stats_.triangles>=budget_.triangles||stats_.rasterCells>=budget_.rasterCells){++stats_.rasterBudgetStops;return false;}
    ++stats_.preparations;
    if(count<3||count>MaxPatchVertices||(faces!=Faces::Both&&faces!=Faces::PositiveArea&&faces!=Faces::NegativeArea)){++stats_.invalidTriangles;return false;}
    out.bounds_={vertices[0],vertices[0]};
    for(unsigned i=0;i<count;++i){const auto&v=vertices[i];if(!finite(v)){++stats_.invalidTriangles;return false;}
        out.bounds_.minimum.x=std::min(out.bounds_.minimum.x,v.x);out.bounds_.minimum.y=std::min(out.bounds_.minimum.y,v.y);out.bounds_.minimum.z=std::min(out.bounds_.minimum.z,v.z);
        out.bounds_.maximum.x=std::max(out.bounds_.maximum.x,v.x);out.bounds_.maximum.y=std::max(out.bounds_.maximum.y,v.y);out.bounds_.maximum.z=std::max(out.bounds_.maximum.z,v.z);}
    for(unsigned eye=0;eye<Eyes;++eye)if(prepareEye(eye,vertices,count,faces,out.eye_[eye])){out.mask_|=1u<<eye;++stats_.preparedEyes;}
    if(!out.mask_)return false;
    out.count_=count;out.owner_=this;out.frame_=frame;out.generation_=generation_;return true;
}
bool ConservativeStereoRaster::projectSourceXY(unsigned eye,const Vec3&v,Projected&out)const noexcept {
    const auto&m=views_[eye];const double x=m[0]*v.x+m[1]*v.y+m[2]*v.z+m[3],y=m[4]*v.x+m[5]*v.y+m[6]*v.z+m[7],w=m[12]*v.x+m[13]*v.y+m[14]*v.z+m[15];
    if(!finite(x)||!finite(y)||!finite(w)||w<=1e-8)return false;
    out={(x/w+1)*Width/2,(y/w+1)*Height/2,0,w};
    return finite(out.x)&&finite(out.y)&&std::abs(out.x)<=1e9&&std::abs(out.y)<=1e9;
}bool ConservativeStereoRaster::prepareEye(unsigned eye,const Vec3* vertices,unsigned count,Faces faces,PreparedPatch::Eye&out)noexcept {
    auto*p=out.p.data();
    for(unsigned i=0;i<3;++i)if(!projectSourceXY(eye,vertices[i],p[i])){++stats_.earlyClipped;++stats_.clippedTriangles;return false;}
    double area=edge(p[0].x,p[0].y,p[1].x,p[1].y,p[2].x,p[2].y);
    const double roundoff=128*Epsilon*(1+std::abs((p[1].x-p[0].x)*(p[2].y-p[0].y))+std::abs((p[1].y-p[0].y)*(p[2].x-p[0].x)));
    if(!finite(area)||std::abs(area)<=roundoff){++stats_.invalidTriangles;return false;}
    if((area<0&&faces==Faces::PositiveArea)||(area>0&&faces==Faces::NegativeArea)){++stats_.earlyCulled;++stats_.culledFaces;return false;}
    for(unsigned i=3;i<count;++i)if(!projectSourceXY(eye,vertices[i],p[i])){++stats_.earlyClipped;++stats_.clippedTriangles;return false;}
    // Only geometry capable of covering a complete cell needs depth projection.
    // XY/w match the original calculations exactly; z/w is merely deferred.
    const auto completeDepth=[&]{
        const auto&m=views_[eye];out.minZ=1;out.maxZ=0;
        for(unsigned i=0;i<count;++i){const auto&v=vertices[i];
            const double z=m[8]*v.x+m[9]*v.y+m[10]*v.z+m[11];
            const double depth=z/p[area<0?count-1-i:i].w;
            if(!finite(depth)||depth<=0||depth>=1){++stats_.earlyClipped;++stats_.clippedTriangles;return false;}
            p[area<0?count-1-i:i].z=depth;out.minZ=std::min(out.minZ,depth);out.maxZ=std::max(out.maxZ,depth);
        }return true;
    };
    if(area<0)std::reverse(p,p+count);
    for(unsigned i=0;i<count;++i){const auto&a=p[i];const auto&b=p[(i+1)%count];const auto&c=p[(i+2)%count];
        const double v=edge(a.x,a.y,b.x,b.y,c.x,c.y),error=128*Epsilon*(1+std::abs((b.x-a.x)*(c.y-a.y))+std::abs((b.y-a.y)*(c.x-a.x)));
        if(!finite(v)||v<=error){++stats_.invalidTriangles;return false;}}
    double lowX=p[0].x,highX=p[0].x,lowY=p[0].y,highY=p[0].y;
    for(unsigned i=1;i<count;++i){lowX=std::min(lowX,p[i].x);highX=std::max(highX,p[i].x);lowY=std::min(lowY,p[i].y);highY=std::max(highY,p[i].y);}
    if(!range(lowX,highX,Width,out.x0,out.x1)||!range(lowY,highY,Height,out.y0,out.y1)){++stats_.earlyEmpty;return false;}
    // A complete unit square must fit in this rectangle. Widen rather than
    // narrow floating endpoints; surviving polygons retain the original range.
    const double padding=256*Epsilon*(1+std::max({std::abs(lowX),std::abs(highX),std::abs(lowY),std::abs(highY)}));
    if(std::ceil(std::max(0.,lowX-padding))>std::floor(std::min(double(Width),highX+padding)-1)||
       std::ceil(std::max(0.,lowY-padding))>std::floor(std::min(double(Height),highY+padding)-1)){++stats_.earlyEmpty;return false;}
    // Every original accepted cell had these exact edge expressions > its
    // strictly positive error margin. Zero-margin coverage is therefore only
    // a cheap necessary condition. This loop never stores depth or rejects a
    // candidate. If its separate work budget expires, defer to the full proof.
    for(int y=out.y0;y<=out.y1;++y)for(int x=out.x0;x<=out.x1;++x){
        if(stats_.preparationCells>=budget_.rasterCells){++stats_.preparationBudgetBypass;return completeDepth();}
        ++stats_.preparationCells;bool possible=true;
        for(unsigned k=0;k<count&&possible;++k){const auto&a=p[k];const auto&b=p[(k+1)%count];
            const double cx=b.y-a.y>=0?x+1.:x,cy=b.x-a.x>=0?y:y+1.;possible=edge(a.x,a.y,b.x,b.y,cx,cy)>0;}
        if(possible)return completeDepth();
    }
    ++stats_.earlyEmpty;return false;
}
bool ConservativeStereoRaster::submit(std::uint64_t frame,const PreparedPatch&prepared,const std::array<Errors,Eyes>&errors)noexcept {
    if(!frame||frame!=frame_||sealed_||prepared.owner_!=this||prepared.frame_!=frame||prepared.generation_!=generation_||!prepared.mask_){++stats_.staleCalls;return false;}
    for(const auto&e:errors)if(!finite(e.coverageCells)||e.coverageCells<1e-4||e.coverageCells>=Width||!finite(e.depth)||e.depth<1e-7||e.depth>=.25)return false;
    if(stats_.triangles>=budget_.triangles||stats_.rasterCells>=budget_.rasterCells){++stats_.rasterBudgetStops;return false;}
    ++stats_.triangles;bool added=false;
    for(unsigned eye=0;eye<Eyes;++eye)if(prepared.mask_&(1u<<eye))added=rasterizePrepared(eye,prepared.eye_[eye],prepared.count_,errors[eye])||added;
    return added;
}
bool ConservativeStereoRaster::rasterizePrepared(unsigned eye,const PreparedPatch::Eye&prepared,unsigned count,const Errors&error)noexcept {
    if(!(prepared.minZ>error.depth&&prepared.maxZ<1-error.depth)){++stats_.clippedTriangles;return false;}
    const auto*p=prepared.p.data();const double z=prepared.maxZ+error.depth+128*Epsilon;
    if(!finite(z)||z>=1||z<=0)return false;float farther=static_cast<float>(z);if(farther<z)farther=std::nextafter(farther,1.f);
    double margin[MaxPatchVertices];
    for(unsigned k=0;k<count;++k){const auto&a=p[k];const auto&b=p[(k+1)%count];const double scale=(std::abs(b.x-a.x)+std::abs(b.y-a.y))*(std::abs(a.x)+std::abs(a.y)+Width+Height+1);
        margin[k]=error.coverageCells*std::hypot(b.x-a.x,b.y-a.y)+128*Epsilon*std::max(1.,scale);}
    bool added=false;
    for(int y=prepared.y0;y<=prepared.y1;++y)for(int x=prepared.x0;x<=prepared.x1;++x){
        if(stats_.rasterCells>=budget_.rasterCells){++stats_.rasterBudgetStops;return added;}++stats_.rasterCells;
        // Existing nearer depth already proves this cell at least as well.
        // Skipping its edge work cannot lose or introduce an occlusion result.
        auto&old=depth_[eye][unsigned(y)*Width+unsigned(x)];if(!(farther<old))continue;
        bool covered=true;for(unsigned k=0;k<count&&covered;++k){const auto&a=p[k];const auto&b=p[(k+1)%count];
            const double cx=b.y-a.y>=0?x+1.:x,cy=b.x-a.x>=0?y:y+1.;covered=edge(a.x,a.y,b.x,b.y,cx,cy)>margin[k];}
        if(!covered)continue;++stats_.coveredCells;
        if(old==1.f)++stats_.uniqueCoveredCells[eye];
        sourceCellDecrease(eye,unsigned(x),unsigned(y),old);
        old=farther;++stats_.depthUpdates;added=true;
    }
    return added;
}bool ConservativeStereoRaster::seal(std::uint64_t frame) noexcept {
    if (!frame || frame != frame_) { ++stats_.staleCalls; return false; }
    sealed_ = true; return true;
}
bool ConservativeStereoRaster::hidden(std::uint64_t frame, const Bounds& b) noexcept {
    if (!ready(frame)) { ++stats_.staleCalls; return false; }
    if (stats_.candidates >= budget_.candidates || stats_.queryCells >= budget_.queryCells) {
        ++stats_.queryBudgetStops; return false;
    }
    ++stats_.candidates;
    if (!finite(b.minimum) || !finite(b.maximum) || b.minimum.x > b.maximum.x ||
        b.minimum.y > b.maximum.y || b.minimum.z > b.maximum.z) { ++stats_.invalidCandidates; return false; }
    bool valid0 = true, valid1 = true;
    const bool a = hiddenEye(0, b, valid0), c = hiddenEye(1, b, valid1);
    if (!valid0 || !valid1) { ++stats_.invalidCandidates; return false; }
    if (a != c) ++stats_.oneEyeOnly;
    if (a && c) { ++stats_.rejected; return true; }
    return false;
}
bool ConservativeStereoRaster::hiddenEye(unsigned eye, const Bounds& b, bool& valid) noexcept {
    double lowX = 1e100, lowY = 1e100, highX = -1e100, highY = -1e100, nearest = 1;
    for (unsigned i = 0; i < 8; ++i) {
        const Vec3 v{(i&1)?b.maximum.x:b.minimum.x, (i&2)?b.maximum.y:b.minimum.y,
                     (i&4)?b.maximum.z:b.minimum.z};
        Projected p;
        if (!project(eye,v,p)) { valid = false; return false; }
        lowX = std::min(lowX,p.x); lowY = std::min(lowY,p.y);
        highX = std::max(highX,p.x); highY = std::max(highY,p.y); nearest = std::min(nearest,p.z);
    }
    // Positive w on all corners makes extrema of projected convex bounds occur
    // at vertices. Expand coverage and move candidate depth towards the camera.
    const double e = errors_[eye].coverageCells;
    nearest -= errors_[eye].depth + 128*Epsilon*(1+std::abs(nearest));
    int x0,x1,y0,y1;
    if (!range(lowX-e,highX+e,Width,x0,x1) || !range(lowY-e,highY+e,Height,y0,y1)) return false;
    for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
        if (stats_.queryCells >= budget_.queryCells) { ++stats_.queryBudgetStops; return false; }
        ++stats_.queryCells;
        if (!(depth_[eye][unsigned(y)*Width+unsigned(x)] < nearest)) return false;
    }
    return true;
}
bool ConservativeStereoRaster::hidden(std::uint64_t frame,const Bounds& b,const std::array<Errors,Eyes>& errors) noexcept {
    for(const auto& e:errors)if(!finite(e.coverageCells)||e.coverageCells<1e-4||e.coverageCells>=Width||
        !finite(e.depth)||e.depth<1e-7||e.depth>=.25)return false;
    const auto old=errors_;errors_=errors;const bool result=hidden(frame,b);errors_=old;return result;
}
float ConservativeStereoRaster::sourceCellDepth(std::uint64_t frame,unsigned eye,unsigned x,unsigned y)const noexcept {
    if(!frame||frame!=frame_||eye>=Eyes||x>=Width||y>=Height)return 1.0f;
    return depth_[eye][y*Width+x];
}
void ConservativeStereoRaster::sourceCellDecrease(unsigned eye,unsigned x,unsigned y,float old)noexcept {
    const unsigned index=(y/8)*8+x/8;const std::uint64_t bit=std::uint64_t(1)<<index;
    // Depth only decreases. A tile maximum stays exact while any of its
    // previous maxima survives. Partial coverage therefore does not rescan
    // the same still-open tile after every unrelated patch.
    if(!(sourceTileDirty_[eye]&bit)&&old==sourceTiles_[eye][index]&&!--sourceTileMaxCount_[eye][index])
        sourceTileDirty_[eye]|=bit;
}
float ConservativeStereoRaster::sourceTileDepth(std::uint64_t frame,unsigned eye,unsigned x,unsigned y)noexcept {
    if(!frame||frame!=frame_||eye>=Eyes||x>=8||y>=8)return 1.f;
    const unsigned index=y*8+x;const std::uint64_t bit=std::uint64_t(1)<<index;
    if(sourceTileDirty_[eye]&bit){
        float farthest=0.f;std::uint8_t count=0;
        for(unsigned iy=y*8;iy<(y+1)*8;++iy)for(unsigned ix=x*8;ix<(x+1)*8;++ix){
            const float value=depth_[eye][iy*Width+ix];
            if(value>farthest){farthest=value;count=1;}else if(value==farthest)++count;
        }
        ++stats_.sourceTileRebuilds;sourceTileMaxCount_[eye][index]=count;
        sourceTiles_[eye][index]=farthest;sourceTileDirty_[eye]&=~bit;
    }
    return sourceTiles_[eye][index];
}
float ConservativeStereoRaster::depth(std::uint64_t frame, unsigned eye, unsigned x, unsigned y) const noexcept {
    if (!ready(frame) || eye >= Eyes || x >= Width || y >= Height) return 1.f;
    return depth_[eye][y*Width+x];
}
} // namespace rdrvr::engine




