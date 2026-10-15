#pragma once
#include "ControllerAim.h"
namespace rdrvr::engine {
// A model-space muzzle pose must come from the same current native weapon /
// skeleton as the model being placed. This does not authorize engine writes.
struct WeaponPosePlan {runtime::Mat4 modelWorld{},muzzleWorld{};};
namespace weapon_pose_detail {
using namespace runtime;
inline float dot(Vec3 a,Vec3 b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
inline bool rigid(const Mat4& m){
    for(float x:m)if(!std::isfinite(x))return false;
    if(m[3]!=0||m[7]!=0||m[11]!=0||m[15]!=1)return false;
    const Vec3 x{m[0],m[1],m[2]},y{m[4],m[5],m[6]},z{m[8],m[9],m[10]};
    const Vec3 cross{x[1]*y[2]-x[2]*y[1],x[2]*y[0]-x[0]*y[2],x[0]*y[1]-x[1]*y[0]};
    return std::abs(dot(x,x)-1)<0.0001f&&std::abs(dot(y,y)-1)<0.0001f&&std::abs(dot(z,z)-1)<0.0001f&&
        std::abs(dot(x,y))<0.0001f&&std::abs(dot(x,z))<0.0001f&&std::abs(dot(y,z))<0.0001f&&dot(cross,z)>0.9999f;
}
}
// Recover an authored attachment-to-muzzle transform from two poses sampled in
// the same native update. Uniform scale is factored out; shear/reflection and
// different scales decline. This is not a guessed hand/model-axis convention.
inline bool deriveModelMuzzle(const runtime::Mat4& modelWorld,const runtime::Mat4& muzzleWorld,
                             runtime::Mat4& localMuzzle,float& modelScale){
    using namespace runtime;
    for(float x:modelWorld)if(!std::isfinite(x))return false;
    for(float x:muzzleWorld)if(!std::isfinite(x))return false;
    const auto length=[](const Mat4& m){return std::sqrt(m[0]*m[0]+m[1]*m[1]+m[2]*m[2]);};
    const float scale=length(modelWorld);
    if(!std::isfinite(scale)||scale<0.001f||scale>1000)return false;
    Mat4 parent=modelWorld,muzzle=muzzleWorld;
    for(unsigned col=0;col<3;++col)for(unsigned row=0;row<3;++row){parent[col*4+row]/=scale;muzzle[col*4+row]/=scale;}
    if(!weapon_pose_detail::rigid(parent)||!weapon_pose_detail::rigid(muzzle))return false;
    Mat4 local{};local[15]=1;
    for(unsigned col=0;col<3;++col)for(unsigned row=0;row<3;++row)
        for(unsigned k=0;k<3;++k)local[col*4+row]+=parent[row*4+k]*muzzle[col*4+k];
    for(unsigned row=0;row<3;++row)for(unsigned k=0;k<3;++k)
        local[12+row]+=parent[row*4+k]*(muzzle[12+k]-parent[12+k])/scale;
    if(!weapon_pose_detail::rigid(local))return false;
    localMuzzle=local;modelScale=scale;return true;
}
inline bool prepareWeaponPose(const ControllerRay& hand,const runtime::Mat4& modelMuzzle,
                              float modelScale,WeaponPosePlan& out){
    using namespace runtime;
    if(!hand.sceneId||!hand.controllerSequence||!hand.trackingEpoch||!hand.referenceSpaceEpoch||
       !std::isfinite(modelScale)||modelScale<0.001f||modelScale>1000||!weapon_pose_detail::rigid(modelMuzzle))return false;
    const auto& x=hand.right;const auto& y=hand.up;const auto& f=hand.direction;const auto& p=hand.origin;
    WeaponPosePlan plan{};
    // The audited simple native aimer writes negative shot direction as the
    // muzzle's Z axis. Keep the controller's roll, rather than reconstructing
    // 'up' from the horizon (which becomes singular when aiming vertically).
    plan.muzzleWorld={x[0],x[1],x[2],0,y[0],y[1],y[2],0,-f[0],-f[1],-f[2],0,p[0],p[1],p[2],1};
    if(!weapon_pose_detail::rigid(plan.muzzleWorld))return false;
    // W = targetMuzzle * inverse(modelMuzzle orientation). Preserve native
    // uniform model scale; translation places the authored muzzle on the ray.
    auto& world=plan.modelWorld;world[15]=1;
    for(unsigned col=0;col<3;++col)for(unsigned row=0;row<3;++row){
        for(unsigned k=0;k<3;++k)world[col*4+row]+=plan.muzzleWorld[k*4+row]*modelMuzzle[k*4+col];
        world[col*4+row]*=modelScale;
    }
    for(unsigned row=0;row<3;++row){world[12+row]=p[row];
        for(unsigned k=0;k<3;++k)world[12+row]-=world[k*4+row]*modelMuzzle[12+k];
    }
    for(float value:world)if(!std::isfinite(value))return false;
    out=plan;return true;
}
}
