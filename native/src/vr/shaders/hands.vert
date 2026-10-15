#version 450
// UXRH hand, GPU four-pose blend (open, grip, trigger, both). The 104-byte UXRH vertex is the vertex
// buffer as stored in the asset: 4 positions, 4 normals, uv. No per-frame CPU vertex data.
layout(location=0) in vec3 p0;
layout(location=1) in vec3 p1;
layout(location=2) in vec3 p2;
layout(location=3) in vec3 p3;
layout(location=4) in vec3 n0;
layout(location=5) in vec3 n1;
layout(location=6) in vec3 n2;
layout(location=7) in vec3 n3;
layout(location=8) in vec2 texcoord;
// clip = F * P_eye * inverse(E_eye) * M_hand (double on the CPU). light.xyz = light direction in MODEL space
// (M is orthonormal up to scale, so the model-space direction gives the LOCAL-space Lambert term), light.w = ambient.
// params: x brightness, y world-depth test, z output encode (0 linear, 1 sRGB, 2 inverse Reinhard), w depth epsilon.
layout(push_constant) uniform Push {mat4 clip;vec4 weights;vec4 light;vec4 params;} push;
layout(location=0) out vec2 uv;
layout(location=1) out float shade;
void main(){
    const vec4 w=push.weights;
    const vec3 p=w.x*p0+w.y*p1+w.z*p2+w.w*p3;
    const vec3 n=w.x*n0+w.y*n1+w.z*n2+w.w*n3;
    gl_Position=push.clip*vec4(p,1.0);
    uv=texcoord;
    const float lambert=dot(n,n)>1e-12?max(dot(normalize(n),push.light.xyz),0.0):0.0;
    shade=push.light.w+(1.0-push.light.w)*lambert;
}
