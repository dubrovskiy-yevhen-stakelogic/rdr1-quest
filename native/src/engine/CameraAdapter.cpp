#include "CameraAdapter.h"
#include <cstring>

namespace rdrvr::engine {
using namespace runtime;
namespace {
float dot(Vec3 a,Vec3 b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
Vec3 column(const Mat4& m,unsigned i){return {m[i*4],m[i*4+1],m[i*4+2]};}
Quat product(Quat a,Quat b) {
    return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
            a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
            a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
            a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
}
bool validStock(const StockCamera& c) {
    for(float f:c.world)if(!std::isfinite(f))return false;
    for(float f:c.projection)if(!std::isfinite(f))return false;
    const auto x=column(c.world,0),y=column(c.world,1),z=column(c.world,2);
    const Vec3 cross{x[1]*y[2]-x[2]*y[1],x[2]*y[0]-x[0]*y[2],x[0]*y[1]-x[1]*y[0]};
    if(std::abs(dot(x,x)-1)>0.005f || std::abs(dot(y,y)-1)>0.005f || std::abs(dot(z,z)-1)>0.005f ||
       std::abs(dot(x,y))>0.005f || std::abs(dot(x,z))>0.005f || std::abs(dot(y,z))>0.005f || dot(cross,z)<0.995f)return false;
    if(c.world[3]!=0 || c.world[7]!=0 || c.world[11]!=0 || c.world[15]!=1)return false;
    return std::isfinite(c.nearClip) && std::isfinite(c.farClip) && c.nearClip>0 && c.farClip>c.nearClip &&
        c.projection[0]>0 && std::abs(c.projection[5])>0.001f &&
        std::abs(c.projection[11]+1)<0.001f && std::abs(c.projection[15])<0.001f;
}
Mat4 inverseRigid(const Mat4& m) {
    Mat4 out{m[0],m[4],m[8],0,m[1],m[5],m[9],0,m[2],m[6],m[10],0,0,0,0,1};
    for(unsigned i=0;i<3;++i)out[12+i]=-(m[i*4]*m[12]+m[i*4+1]*m[13]+m[i*4+2]*m[14]);
    return out;
}
}
bool decodeCamera(std::span<const std::byte> fields,StockCamera& out) {
    if(fields.size()!=cameraReadBytes)return false;
    StockCamera c{};
    std::memcpy(c.world.data(),fields.data(),64);
    // Matrix34 has four 16-byte strides; padding is unspecified, not a fourth component.
    c.world[3]=c.world[7]=c.world[11]=0;c.world[15]=1;
    std::memcpy(c.projection.data(),fields.data()+0x1c0-cameraReadOffset,64);
    std::memcpy(&c.nearClip,fields.data()+0x340-cameraReadOffset,4);
    std::memcpy(&c.farClip,fields.data()+0x344-cameraReadOffset,4);
    if(!validStock(c))return false;
    out=c;return true;
}
CameraResult CameraAdapter::prepare(const StockCamera& c,const TrackedViews& v,std::uint64_t sceneId,
                                    float scale,StereoCameraPlan& out,const TrackingOrigin* shared) {
    if(!validStock(c)){invalidate();return CameraResult::InvalidStock;}
    if(!sceneId || !v.frameId || !v.epochs.tracking || !v.epochs.referenceSpace || !v.epochs.resources ||
       !validPose(v.eyes[0]) || !validPose(v.eyes[1]) || !usableFov(v.fov[0]) || !usableFov(v.fov[1])) {
        invalidate();return CameraResult::InvalidTracking;
    }
    if(!std::isfinite(scale) || scale<0.01f || scale>100)return CameraResult::InvalidScale;
    Vec3 delta{},head{};
    for(unsigned j=0;j<3;++j){delta[j]=v.eyes[1].position[j]-v.eyes[0].position[j];head[j]=(v.eyes[1].position[j]+v.eyes[0].position[j])*0.5f;}
    const float ipd=std::sqrt(dot(delta,delta));
    if(!std::isfinite(ipd) || ipd<0.03f || ipd>0.09f){invalidate();return CameraResult::InvalidIpd;}
    // A rig origin of this LOCAL space wins over (and seeds) the adapter's own latch.
    bool useShared=false;
    if(shared&&shared->valid){
        float q{};for(float f:shared->inverseYaw)q+=f*f;
        bool finite=std::isfinite(q)&&std::abs(q-1)<0.01f;for(float f:shared->position)finite=finite&&std::isfinite(f);
        if(finite&&shared->referenceSpace==v.epochs.referenceSpace)useShared=true;else ++staleShared_;
    }
    if(useShared){
        origin_=shared->position;inverseYaw_=shared->inverseYaw;epochs_=v.epochs;anchored_=true;originSerial_=shared->serial;++sharedPlans_;
    }else if(!anchored_ || epochs_!=v.epochs) {
        Quat headQ{};float norm{};float sign{};
        for(unsigned j=0;j<4;++j)sign+=v.eyes[0].orientation[j]*v.eyes[1].orientation[j];
        for(unsigned j=0;j<4;++j){headQ[j]=v.eyes[0].orientation[j]+(sign<0?-1:1)*v.eyes[1].orientation[j];norm+=headQ[j]*headQ[j];}
        norm=std::sqrt(norm);for(float& f:headQ)f/=norm;
        const auto forward=rotate(headQ,{0,0,-1});
        if(forward[0]*forward[0]+forward[2]*forward[2]<0.001f)return CameraResult::InvalidTracking;
        const float yaw=std::atan2(-forward[0],-forward[2]);
        inverseYaw_={0,-std::sin(yaw/2),0,std::cos(yaw/2)};origin_=head;epochs_=v.epochs;anchored_=true;
        originSerial_=0;++ownLatches_;
    }
    StereoCameraPlan plan{};plan.sceneId=sceneId;plan.tracking=v;plan.ipdMetres=ipd;plan.unitsPerMetre=scale;plan.baseWorld=c.world;
    plan.originSerial=originSerial_;
    plan.nearClip=c.nearClip;plan.farClip=c.farClip;
    for(unsigned i=0;i<2;++i) {
        const auto q=product(inverseYaw_,v.eyes[i].orientation);
        Vec3 offset{};for(unsigned j=0;j<3;++j)offset[j]=v.eyes[i].position[j]-origin_[j];
        offset=rotate(inverseYaw_,offset);
        const auto x=rotate(q,{1,0,0}),y=rotate(q,{0,1,0}),z=rotate(q,{0,0,1});
        const Mat4 relative{x[0],x[1],x[2],0,y[0],y[1],y[2],0,z[0],z[1],z[2],0,
                            offset[0]*scale,offset[1]*scale,offset[2]*scale,1};
        plan.world[i]=multiply(c.world,relative);plan.view[i]=inverseRigid(plan.world[i]);
        plan.projection[i]=projection(v.fov[i],c.nearClip,c.farClip);
        // Match the engine's observed vertical clip convention instead of guessing a backend flip.
        if(c.projection[5]>0){plan.projection[i][5]*=-1;plan.projection[i][9]*=-1;}
        for(float f:plan.world[i])if(!std::isfinite(f)){invalidate();return CameraResult::InvalidTracking;}
        for(float f:plan.view[i])if(!std::isfinite(f)){invalidate();return CameraResult::InvalidTracking;}
        for(float f:plan.projection[i])if(!std::isfinite(f))return CameraResult::InvalidStock;
    }
    out=plan;return CameraResult::Ready;
}
} // namespace rdrvr::engine
