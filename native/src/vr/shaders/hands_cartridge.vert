#version 450
// RELOAD: one procedural cartridge = a surface of revolution (9 profile points ->
// 8 bands x 12 segments x 6 vertices = 576, no vertex buffer). LOCAL metres; viewProj = LOCAL -> clip.
// base.xyz = the rim's rear point, base.w = the case radius r; axis.xyz = rim -> bullet (unit), axis.w = the case length L (from the rim's
// front face); cyl = the open cylinder occluder (fragment shader); params.x = the style (8 live round, 9 spent case).
// Profile (z along the axis, radius): rear face / primer, rim (1.10 r, 1.2 mm), case; LIVE: a lead bullet of 1.45 r; SPENT: the open mouth
// (a 0.84 r lip, 3 mm deep sooty inside).
layout(push_constant) uniform Push {mat4 viewProj;vec4 base;vec4 axis;vec4 cyl;vec4 params;} push;
layout(location=0) out vec3 worldPos;
layout(location=1) out vec3 normal;
layout(location=2) out float radial;
layout(location=3) flat out int band;
layout(location=4) flat out vec3 eye;
const int SEG=12;
vec2 profile(int k,float r,float L,bool live){
    const float rimR=1.10*r,rimT=0.0012,z1=rimT+L,lb=1.45*r;
    if(k<=0)return vec2(0.0,0.0);
    if(k==1)return vec2(0.0,rimR);
    if(k==2)return vec2(rimT,rimR);
    if(k==3)return vec2(rimT,r);
    if(k==4)return vec2(z1,r);
    if(live){
        if(k==5)return vec2(z1,0.96*r);
        if(k==6)return vec2(z1+0.45*lb,0.86*r);
        if(k==7)return vec2(z1+0.80*lb,0.52*r);
        return vec2(z1+lb,0.0);
    }
    if(k==5)return vec2(z1,0.84*r);
    if(k==6)return vec2(z1-0.003,0.84*r);
    if(k==7)return vec2(z1-0.003,0.40*r);
    return vec2(z1-0.003,0.0);
}
void main(){
    const int dk[6]=int[6](0,1,1,0,1,0);
    const int ds[6]=int[6](0,0,1,0,1,1);
    const int idx=gl_VertexIndex;
    const int b=idx/(SEG*6);
    const int rem=idx-b*SEG*6;
    const int s=rem/6;
    const int c=rem-s*6;
    const float r=push.base.w,L=push.axis.w;
    const bool live=push.params.x<8.5;
    const vec2 p=profile(b+dk[c],r,L,live);
    const vec2 t=profile(b+1,r,L,live)-profile(b,r,L,live);
    vec2 n2=vec2(-t.y,t.x);
    n2=dot(n2,n2)>1e-14?normalize(n2):vec2(-1.0,0.0);
    const vec3 a=normalize(push.axis.xyz);
    const vec3 ref=abs(a.y)<0.9?vec3(0.0,1.0,0.0):vec3(1.0,0.0,0.0);
    const vec3 u=normalize(cross(a,ref));
    const vec3 v=cross(a,u);
    const float ang=6.2831853*float(s+ds[c])/float(SEG);
    const vec3 dir=u*cos(ang)+v*sin(ang);
    const vec3 P=push.base.xyz+a*p.x+dir*p.y;
    worldPos=P;
    normal=a*n2.x+dir*n2.y;
    radial=r>0.0?p.y/r:0.0;
    band=b;
    // the eye = the camera centre: viewProj maps it to (0, 0, B, 0)
    const vec4 e=inverse(push.viewProj)*vec4(0.0,0.0,1.0,0.0);
    eye=e.xyz/(abs(e.w)>1e-20?e.w:1e-20);
    gl_Position=push.viewProj*vec4(P,1.0);
}
