#version 450
// HANDS holster marker: an eye-facing quad (6 vertices, no vertex buffer) around a LOCAL-space
// centre;
// the fragment shader cuts the ring. viewProj = F * P_eye * inverse(E_eye) (LOCAL -> clip).
// centre.w = radius, right.w = highlight (0..1), up.w = brightness, params as hands.frag (x unused).
layout(push_constant) uniform Push {mat4 viewProj;vec4 centre;vec4 right;vec4 up;vec4 params;} push;
layout(location=0) out vec2 corner;
layout(location=1) out float highlight;
const vec2 corners[6]=vec2[6](vec2(-1,-1),vec2(1,-1),vec2(1,1),vec2(-1,-1),vec2(1,1),vec2(-1,1));
void main(){
    const vec2 q=corners[gl_VertexIndex];
    const vec3 p=push.centre.xyz+(push.right.xyz*q.x+push.up.xyz*q.y)*push.centre.w;
    gl_Position=push.viewProj*vec4(p,1.0);
    corner=q;
    highlight=push.right.w;
}
