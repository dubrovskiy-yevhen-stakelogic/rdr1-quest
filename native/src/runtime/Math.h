#pragma once
#include "contracts/Contracts.h"
#include <cmath>
namespace rdrvr::runtime {
using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>;
using Mat4 = std::array<float, 16>;
inline Vec3 rotate(Quat q, Vec3 v) {
    const Vec3 t{2*(q[1]*v[2]-q[2]*v[1]), 2*(q[2]*v[0]-q[0]*v[2]), 2*(q[0]*v[1]-q[1]*v[0])};
    return {v[0]+q[3]*t[0]+q[1]*t[2]-q[2]*t[1],
            v[1]+q[3]*t[1]+q[2]*t[0]-q[0]*t[2],
            v[2]+q[3]*t[2]+q[0]*t[1]-q[1]*t[0]};
}
inline Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 m{};
    for (int c=0;c<4;++c) for(int r=0;r<4;++r) for(int k=0;k<4;++k)
        m[c*4+r] += a[k*4+r]*b[c*4+k];
    return m;
}
inline Mat4 viewMatrix(const Pose& p) {
    const Quat inverse{-p.orientation[0],-p.orientation[1],-p.orientation[2],p.orientation[3]};
    auto x=rotate(inverse,{1,0,0}), y=rotate(inverse,{0,1,0}), z=rotate(inverse,{0,0,1});
    auto t=rotate(inverse,{-p.position[0],-p.position[1],-p.position[2]});
    return {x[0],x[1],x[2],0, y[0],y[1],y[2],0, z[0],z[1],z[2],0, t[0],t[1],t[2],1};
}
// OpenXR right-handed metres/-Z forward -> Vulkan clip Y down, Z in [0,1].
inline Mat4 projection(Fov f, float nearZ=0.05f, float farZ=100.f) {
    const float l=std::tan(f.left),r=std::tan(f.right),u=std::tan(f.up),d=std::tan(f.down);
    Mat4 m{};
    m[0]=2/(r-l); m[5]=-2/(u-d); m[8]=(r+l)/(r-l); m[9]=-(u+d)/(u-d);
    m[10]=-farZ/(farZ-nearZ); m[11]=-1; m[14]=-farZ*nearZ/(farZ-nearZ);
    return m;
}
} // namespace rdrvr::runtime
