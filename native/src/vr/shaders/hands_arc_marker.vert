#version 450
// ARC: end marker = a disc of radius centre.w lying in the plane with normal normal.xyz (camera-facing when the CPU passes the
// direction to the eye). normal.w = shape + heading/2pi (heading in the (t1, t2) basis below; the CPU replicates anyPerpendicular).
// 6 vertices, no vertex buffer.
layout(push_constant) uniform Push {mat4 viewProj;vec4 centre;vec4 normal;vec4 color;vec4 params;} push;
layout(location=0) out vec2 corner;
const vec2 corners[6]=vec2[6](vec2(-1,-1),vec2(1,-1),vec2(1,1),vec2(-1,-1),vec2(1,1),vec2(-1,1));
vec3 anyPerpendicular(vec3 v){return abs(v.y)<0.99?normalize(cross(v,vec3(0,1,0))):normalize(cross(v,vec3(1,0,0)));}
void main(){
    const vec2 q=corners[gl_VertexIndex];
    const vec3 n=normalize(push.normal.xyz);const vec3 t1=anyPerpendicular(n);const vec3 t2=cross(n,t1);
    gl_Position=push.viewProj*vec4(push.centre.xyz+(t1*q.x+t2*q.y)*push.centre.w,1.0);
    corner=q;
}
