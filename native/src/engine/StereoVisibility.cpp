#include "StereoVisibility.h"
#include <algorithm>
#include <limits>

namespace rdrvr::engine {
namespace {
using D3=std::array<double,3>;
double dot(D3 a,D3 b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
D3 cross(D3 a,D3 b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
bool normalize(D3& v){double n=std::sqrt(dot(v,v));if(!std::isfinite(n)||n<0.01)return false;for(auto& x:v)x/=n;return true;}
D3 column(const runtime::Mat4& m,unsigned c){return {m[c*4],m[c*4+1],m[c*4+2]};}
bool rigid(const runtime::Mat4& m){
    for(auto x:m)if(!std::isfinite(x))return false;
    if(m[3]!=0||m[7]!=0||m[11]!=0||m[15]!=1)return false;
    auto x=column(m,0),y=column(m,1),z=column(m,2);
    return std::abs(dot(x,x)-1)<0.0001&&std::abs(dot(y,y)-1)<0.0001&&std::abs(dot(z,z)-1)<0.0001&&
        std::abs(dot(x,y))<0.0001&&std::abs(dot(x,z))<0.0001&&std::abs(dot(y,z))<0.0001&&dot(cross(x,y),z)>0.9999;
}
bool perspective(const runtime::Mat4& m,float n,float f){
    for(auto x:m)if(!std::isfinite(x))return false;
    for(unsigned i:{1,2,3,4,6,7,12,13,15})if(m[i]!=0)return false;
    const double z=-double(f)/(double(f)-n),w=z*n;
    return m[0]>0&&std::abs(m[5])>0.0001&&m[11]==-1&&
        std::abs(m[10]-z)<1e-5&&std::abs(m[14]-w)<1e-5*std::max(1.0,std::abs(w));
}
float lower(double v){return std::nextafter(static_cast<float>(v),-std::numeric_limits<float>::infinity());}
float upper(double v){return std::nextafter(static_cast<float>(v),std::numeric_limits<float>::infinity());}
}
bool prepareStereoCull(const StereoCameraPlan& eyes,float nearClip,float farClip,StereoCullPlan& out) noexcept {
    if(!eyes.sceneId||!eyes.tracking.frameId||eyes.tracking.displayTime<=0||
       !eyes.tracking.epochs.tracking||!eyes.tracking.epochs.referenceSpace||!eyes.tracking.epochs.resources||
       !std::isfinite(nearClip)||!std::isfinite(farClip)||nearClip<=0||farClip<=nearClip)return false;
    for(unsigned i=0;i<2;++i)if(!rigid(eyes.world[i])||!perspective(eyes.projection[i],nearClip,farClip)||
        !validPose(eyes.tracking.eyes[i])||!runtime::usableFov(eyes.tracking.fov[i]))return false;
    D3 x{},z{};
    for(unsigned j=0;j<3;++j){x[j]=double(eyes.world[0][j])+eyes.world[1][j];z[j]=double(eyes.world[0][8+j])+eyes.world[1][8+j];}
    if(!normalize(z))return false;
    double parallel=dot(x,z);for(unsigned j=0;j<3;++j)x[j]-=parallel*z[j];
    if(!normalize(x))return false;
    D3 y=cross(z,x);
    StereoCullPlan plan{};plan.sceneId=eyes.sceneId;plan.tracking=eyes.tracking;
    auto& w=plan.camera.world;w[15]=1;
    for(unsigned j=0;j<3;++j){w[j]=float(x[j]);w[4+j]=float(y[j]);w[8+j]=float(z[j]);
        w[12+j]=float((double(eyes.world[0][12+j])+eyes.world[1][12+j])*0.5);}
    // Use the rounded output frame when bounding the corners. Subtract world
    // origins in double before rotation to avoid cancellation at distant towns.
    x=column(w,0);y=column(w,1);z=column(w,2);
    // A midpoint-origin union expands its far-plane width by IPD / nearClip.
    // At a 5 cm near plane that includes a large wedge unseen by either eye.
    // Keep the angular union of the eye rays and move ONLY the culling apex
    // backwards enough to enclose both translated near planes. Recompute all
    // finite corners below using the rounded output origin; this is essential
    // in distant towns where a centimetre translation loses float precision.
    double left=INFINITY,right=-INFINITY,bottom=INFINITY,top=-INFINITY;
    std::array<D3,16> corners{};unsigned cornerCount=0;
    for(unsigned eye=0;eye<2;++eye)for(double sx:{-1.,1.})for(double sy:{-1.,1.}){
        const auto& m=eyes.world[eye];const auto& p=eyes.projection[eye];
        D3 ray{},origin{};const D3 local{(sx+p[8])/p[0],(sy+p[9])/p[5],-1};
        for(unsigned j=0;j<3;++j){origin[j]=double(m[12+j])-w[12+j];ray[j]=m[j]*local[0]+m[4+j]*local[1]+m[8+j]*local[2];}
        const double depth=-dot(z,ray);
        if(!std::isfinite(depth)||depth<=0)return false;
        const double h=dot(x,ray)/depth,v=dot(y,ray)/depth;
        left=std::min(left,h);right=std::max(right,h);bottom=std::min(bottom,v);top=std::max(top,v);
        for(double d:{double(nearClip),double(farClip)}){
            D3 delta{};for(unsigned j=0;j<3;++j)delta[j]=origin[j]+d*ray[j];
            corners[cornerCount++]={dot(x,delta),dot(y,delta),-dot(z,delta)};
        }
    }
    if(left<-.001&&right>.001&&bottom<-.001&&top>.001){
        double back=0;
        for(const auto& c:corners){
            // Preserve the previous refusal of a finite volume crossing the
            // camera plane. Do not repair an unsupported tracking geometry.
            if(c[2]<=0)return false;
            back=std::max({back,c[0]/left-c[2],c[0]/right-c[2],c[1]/bottom-c[2],c[1]/top-c[2]});
        }
        D3 separation{};for(unsigned j=0;j<3;++j)separation[j]=double(eyes.world[1][12+j])-eyes.world[0][12+j];
        if(std::isfinite(back)&&back<=std::max(double(nearClip),4*std::sqrt(dot(separation,separation)))){
            const D3 old{w[12],w[13],w[14]};
            for(unsigned j=0;j<3;++j)w[12+j]=float(old[j]+z[j]*back);
            D3 moved{};for(unsigned j=0;j<3;++j)moved[j]=double(w[12+j])-old[j];
            plan.originBackshift=float(dot(z,moved));
        }
    }
    double l=INFINITY,r=-INFINITY,b=INFINITY,t=-INFINITY,n=INFINITY,f=0;
    for(unsigned eye=0;eye<2;++eye)for(double d:{double(nearClip),double(farClip)})for(double sx:{-1.,1.})for(double sy:{-1.,1.}){
        const auto& m=eyes.world[eye];const auto& p=eyes.projection[eye];
        D3 local{(sx+p[8])*d/p[0],(sy+p[9])*d/p[5],-d},delta{};
        for(unsigned j=0;j<3;++j)delta[j]=double(m[12+j])-w[12+j]+m[j]*local[0]+m[4+j]*local[1]+m[8+j]*local[2];
        double depth=-dot(z,delta);
        // A single forward perspective cannot enclose a volume crossing its
        // camera plane. Refuse it, never clip the unsupported part silently.
        if(!std::isfinite(depth)||depth<=0)return false;
        double h=dot(x,delta)/depth,v=dot(y,delta)/depth;
        l=std::min(l,h);r=std::max(r,h);b=std::min(b,v);t=std::max(t,v);n=std::min(n,depth);f=std::max(f,depth);
    }
    // Outward guard covers float matrix construction/engine plane normalization.
    constexpr double margin=0.0001;
    l-=margin*std::max(1.,std::abs(l));r+=margin*std::max(1.,std::abs(r));
    b-=margin*std::max(1.,std::abs(b));t+=margin*std::max(1.,std::abs(t));
    plan.camera.nearClip=lower(n*(1-margin));plan.camera.farClip=upper(f*(1+margin));
    if(!std::isfinite(l)||!std::isfinite(r)||!std::isfinite(b)||!std::isfinite(t)||
       plan.camera.nearClip<=0||!std::isfinite(plan.camera.farClip)||r<=l||t<=b)return false;
    // Native spdShaft::SetX1/SetX2/SetY1/SetY2 reconstruct symmetric planes
    // from cached half-angle tangents, not the off-axis projection matrix.
    // Enclose the asymmetric eye union in a symmetric CPU-only frustum so
    // those planes and the portal projection describe the same volume.
    const double horizontal=std::max(-l,r),vertical=std::max(-b,t);
    l=-horizontal;r=horizontal;b=-vertical;t=vertical;
    auto& p=plan.camera.projection;
    p[0]=float(2/(r-l));p[5]=float(2/(t-b));p[8]=float((r+l)/(r-l));p[9]=float((t+b)/(t-b));
    if(eyes.projection[0][5]<0){p[5]*=-1;p[9]*=-1;}
    const double cn=plan.camera.nearClip,cf=plan.camera.farClip;
    p[10]=float(-cf/(cf-cn));p[11]=-1;p[14]=float(-cf*cn/(cf-cn));
    if(!perspective(p,plan.camera.nearClip,plan.camera.farClip))return false;
    out=plan;return true;
}
} // namespace rdrvr::engine
