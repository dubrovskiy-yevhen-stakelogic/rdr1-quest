#version 450
// Sniper scope lens (ScopePass.cpp): the eyepiece disc's bounding square, its 4 corners already in the eye's clip space
// (CPU, double precision), so the disc coordinate is interpolated perspective-correct. No vertex buffer, 6 vertices.
layout(push_constant) uniform Push {vec4 corner[4];vec4 axis;vec4 right;vec4 up;vec4 params;} push;
layout(location=0) out vec2 disc; // -1..1 along the scope's right / up
const int order[6]=int[6](0,1,2,0,2,3);
const vec2 coords[4]=vec2[4](vec2(-1.0,-1.0),vec2(1.0,-1.0),vec2(1.0,1.0),vec2(-1.0,1.0));
// FULL ZOOM (flag 8): the quad is the disc's square scaled by params.w, so the disc coordinate reaches +-params.w at its corners.
void main(){
    const int i=order[gl_VertexIndex];
    const int flags=int(push.params.x+0.5);
    disc=(flags&8)!=0?coords[i]*push.params.w:coords[i];
    gl_Position=push.corner[i];
}
