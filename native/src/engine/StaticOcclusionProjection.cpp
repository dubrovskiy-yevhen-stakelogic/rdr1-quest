#include "StaticOcclusionProjection.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace rdrvr::engine {
namespace {
// Shader multiplication/addition precision: use 1 ULP, rather than assuming
// round-to-nearest. Covers reassociation and contraction of the four-term dot.
constexpr double U=0x1p-23, Tiny=0x1p-120;
constexpr double gamma(unsigned n){return (n*U)/(1-n*U);}
bool finite(double x){return std::isfinite(x)&&std::abs(x)<0x1p60;}
}
bool StaticOcclusionProjection::configure(const std::array<Camera,2>& c,unsigned w,unsigned h) noexcept {
    width_=height_=0;
    if(w<64||h<64||w>16384||h>16384)return false;
    for(unsigned eye=0;eye<2;++eye){
        for(float x:c[eye].relativeClip)if(!finite(x))return false;
        for(float x:c[eye].inverseView)if(!finite(x))return false;
        if(c[eye].inverseView[3]!=0||c[eye].inverseView[7]!=0||c[eye].inverseView[11]!=0||c[eye].inverseView[15]!=1)return false;
        auto& m=matrices_[eye];
        for(unsigned row=0;row<4;++row){
            for(unsigned col=0;col<3;++col)m[row*4+col]=c[eye].relativeClip[col*4+row];
            m[row*4+3]=c[eye].relativeClip[12+row];
            for(unsigned col=0;col<3;++col)m[row*4+3]-=m[row*4+col]*c[eye].inverseView[12+col];
        }
    }
    camera_=c;width_=w;height_=h;return true;
}
bool StaticOcclusionProjection::errors(const Raster::Bounds& bounds,const Raster::Vec3& inputError,
                                      std::array<Raster::Errors,2>& out)const noexcept {
    return errorsImpl(bounds,inputError,{},out);
}
bool StaticOcclusionProjection::errorsSource(std::span<const Raster::Vec3> vertices,
                                            const Raster::Vec3& inputError,
                                            std::array<Raster::Errors,2>& out)const noexcept {
    out={};
    if(vertices.size()<3||vertices.size()>Raster::MaxPatchVertices)return false;
    Raster::Bounds bounds{vertices.front(),vertices.front()};
    for(const auto& v:vertices){
        if(!finite(v.x)||!finite(v.y)||!finite(v.z))return false;
        bounds.minimum={std::min(bounds.minimum.x,v.x),std::min(bounds.minimum.y,v.y),std::min(bounds.minimum.z,v.z)};
        bounds.maximum={std::max(bounds.maximum.x,v.x),std::max(bounds.maximum.y,v.y),std::max(bounds.maximum.z,v.z)};
    }
    return errorsImpl(bounds,inputError,vertices,out);
}
bool StaticOcclusionProjection::errorsImpl(const Raster::Bounds& bounds,const Raster::Vec3& inputError,
                                          std::span<const Raster::Vec3> vertices,
                                          std::array<Raster::Errors,2>& out)const noexcept {
    out={};if(!width_||!height_)return false;
    const double lo[]{bounds.minimum.x,bounds.minimum.y,bounds.minimum.z};
    const double hi[]{bounds.maximum.x,bounds.maximum.y,bounds.maximum.z};
    const double we[]{inputError.x,inputError.y,inputError.z};
    for(unsigned i=0;i<3;++i)if(!finite(lo[i])||!finite(hi[i])||lo[i]>hi[i]||!finite(we[i])||we[i]<0)return false;
    for(unsigned eye=0;eye<2;++eye){
        const auto& c=camera_[eye];double maxClip[4]{},clipError[4]{},maxRatio[3]{},minW=1e100;
        double relAbs[3]{},relError[3]{};
        for(unsigned col=0;col<3;++col){
            const double translation=c.inverseView[12+col];
            relAbs[col]=std::max(std::abs(lo[col]-translation),std::abs(hi[col]-translation));
            relError[col]=we[col]+gamma(2)*(relAbs[col]+we[col])+Tiny;
        }
        for(unsigned row=0;row<4;++row){
            double magnitude=std::abs(c.relativeClip[12+row]);
            for(unsigned col=0;col<3;++col){const double a=std::abs(c.relativeClip[col*4+row]);
                magnitude+=a*(relAbs[col]+relError[col]);clipError[row]+=a*relError[col];}
            clipError[row]+=gamma(8)*magnitude+Tiny;
        }
        if(vertices.empty()){
            // Each rounded subtraction, multiplication and addition is monotone.
            // With the SAME dot-product order, choosing the endpoints by coefficient
            // sign produces exactly the extrema over the eight AABB corners.
            for(unsigned row=0;row<4;++row){
                double minimum=c.relativeClip[12+row],maximum=minimum;
                for(unsigned col=0;col<3;++col){const double a=c.relativeClip[col*4+row];
                    const double lower=a<0?hi[col]:lo[col],upper=a<0?lo[col]:hi[col];
                    minimum+=a*(lower-c.inverseView[12+col]);
                    maximum+=a*(upper-c.inverseView[12+col]);}
                maxClip[row]=std::max(std::abs(minimum),std::abs(maximum));
                if(row==3)minW=minimum;
            }
        }else{
            // A linear functional reaches its extrema on a convex polygon's
            // vertices. The AABB above still bounds every shader operation;
            // only the nominal quotient interval is narrowed to real geometry.
            // The directed double margin covers these CPU dot computations.
            constexpr double du=std::numeric_limits<double>::epsilon();
            constexpr double dg=(16*du)/(1-16*du);
            for(const auto& v:vertices){
                const double xyz[]{v.x,v.y,v.z};double intervalLow[4]{},intervalHigh[4]{};
                for(unsigned row=0;row<4;++row){
                    double q=c.relativeClip[12+row],magnitude=std::abs(q);
                    for(unsigned col=0;col<3;++col){
                        const double coefficient=c.relativeClip[col*4+row];
                        q+=coefficient*(xyz[col]-c.inverseView[12+col]);
                        magnitude+=std::abs(coefficient)*(std::abs(xyz[col])+std::abs(c.inverseView[12+col]));
                    }
                    const double margin=dg*magnitude+std::numeric_limits<double>::min();
                    const double low=std::nextafter(q-margin,-std::numeric_limits<double>::infinity());
                    const double high=std::nextafter(q+margin,std::numeric_limits<double>::infinity());
                    intervalLow[row]=low;intervalHigh[row]=high;
                    maxClip[row]=std::max(maxClip[row],std::max(std::abs(low),std::abs(high)));
                    if(row==3)minW=std::min(minW,low);
                }
                if(!(intervalLow[3]>0))return false;
                for(unsigned row=0;row<3;++row){
                    const double ratio=std::nextafter(std::max(std::abs(intervalLow[row]),std::abs(intervalHigh[row]))/intervalLow[3],std::numeric_limits<double>::infinity());
                    if(!finite(ratio))return false;maxRatio[row]=std::max(maxRatio[row],ratio);
                }
            }
        }
        const double safeW=minW-clipError[3];
        if(!finite(safeW)||safeW<=0x1p-16)return false;
        double e[3]{};
        for(unsigned row=0;row<3;++row){
            // Difference of two quotients plus generous 8-ULP reciprocal /
            // viewport precision allowance (division requires at most 2.5 ULP).
            if(vertices.empty()){
                // No patch vertices: the bound from the AABB extrema alone.
                e[row]=clipError[row]/safeW+maxClip[row]*clipError[3]/(minW*safeW)+
                    gamma(16)*(1+(maxClip[row]+clipError[row])/safeW);
            }else{
                // Every point of the exact convex patch has positive W and
                // |q/W| <= the maximum vertex ratio. Keep the full-AABB shader
                // error E, including every removed native interior vertex.
                // For |dq|<=Eq, |dW|<=Ew:
                // |(q+dq)/(W+dW)-q/W| <= (Eq+Rmax*Ew)/(Wmin-Ew).
                // This couples each nominal q with its OWN W, not an unrelated
                // minimum-W corner. The global error remains uniform.
                const double product=std::nextafter(maxRatio[row]*clipError[3],std::numeric_limits<double>::infinity());
                const double numerator=std::nextafter(clipError[row]+product,std::numeric_limits<double>::infinity());
                const double delta=std::nextafter(numerator/safeW,std::numeric_limits<double>::infinity());
                e[row]=std::nextafter(delta+gamma(16)*(1+maxRatio[row]+delta),std::numeric_limits<double>::infinity());
            }
        }
        // One FULL pixel exceeds Vulkan's worst allowed subpixel snap (1/16).
        // Depth extrema avoid unstable triangle-plane division. 1e-5 is the
        // fixed-function relative precision minimum, four operations cover
        // interpolation/range conversion; two D24 quanta cover store/compare.
        out[eye]={std::max(1e-4,std::hypot(e[0]*32+64./width_,e[1]*32+64./height_)),
            std::max(1e-7,e[2]+4e-5+2./16777215.)};
        if(!finite(out[eye].coverageCells)||out[eye].coverageCells>=64||!finite(out[eye].depth)||out[eye].depth>=.25)return false;
    }
    return true;
}
}
