#version 450
// Laser sight of the VR-held firearm. No vertex buffer, 6 vertices per draw.
// mode 0 (b.w < 0.5): beam = a quad from a to b (LOCAL metres) that always faces the eye, half width max(a.w, params.x * distance)
//                     so it stays at least ~1-2 pixels wide far away; mode 1: dot = eye-facing disc of radius
//                     max(4 * a.w, 3 * params.x * distance) at b (the laser end / hit point).
// viewProj = F * P_eye * inverse(E_eye) * S (LOCAL metres -> clip), eye.xyz = the eye position in LOCAL metres, eye.w = brightness.
layout(push_constant) uniform Push {mat4 viewProj;vec4 a;vec4 b;vec4 eye;vec4 params;} push;
layout(location=0) out vec2 coord; // beam: (along 0..1, across -1..1); dot: disc coordinates
const vec2 corners[6]=vec2[6](vec2(0,-1),vec2(1,-1),vec2(1,1),vec2(0,-1),vec2(1,1),vec2(0,1));
vec3 anyPerpendicular(vec3 v){return abs(v.y)<0.99?normalize(cross(v,vec3(0,1,0))):normalize(cross(v,vec3(1,0,0)));}
void main(){
    const vec2 q=corners[gl_VertexIndex];
    vec3 p;
    if(push.b.w<0.5){
        const vec3 axis=normalize(push.b.xyz-push.a.xyz);
        const vec3 centre=mix(push.a.xyz,push.b.xyz,q.x);
        const vec3 toEye=push.eye.xyz-centre;const float dist=length(toEye);
        vec3 side=cross(axis,toEye);
        side=length(side)>1e-6?normalize(side):anyPerpendicular(axis);
        const float halfWidth=max(push.a.w,push.params.x*dist);
        p=centre+side*halfWidth*q.y;
        coord=q;
    }else{
        const vec2 c=vec2(q.x*2.0-1.0,q.y);
        const vec3 toEye=push.eye.xyz-push.b.xyz;const float dist=max(length(toEye),1e-4);const vec3 n=toEye/dist;
        const vec3 right=anyPerpendicular(n);const vec3 up=cross(n,right);
        const float radius=max(4.0*push.a.w,3.0*push.params.x*dist);
        p=push.b.xyz+(right*c.x+up*c.y)*radius;
        coord=c;
    }
    gl_Position=push.viewProj*vec4(p,1.0);
}
