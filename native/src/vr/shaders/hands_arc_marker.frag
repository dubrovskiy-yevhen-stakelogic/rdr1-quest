#version 450
// Arc marker shapes 1 ring, 2 ring + faint fill, 3 ring + X (invalid), 4 soft dot, 5 ring + arrow (facing).
// World-depth tested with a CPU-computed window-depth tolerance (params.w = epsilon + depth step of depthTolM at this distance);
// x-ray like the ribbon. normal.w = shape (integer part) + heading / 2pi (fractional part, angle in the vertex shader's t1/t2 basis).
layout(set=0,binding=1) uniform sampler2D worldDepth;
layout(push_constant) uniform Push {mat4 viewProj;vec4 centre;vec4 normal;vec4 color;vec4 params;} push;
layout(location=0) in vec2 corner;
layout(location=0) out vec4 outColor;
vec3 encodeOutput(vec3 c,float mode){
    if(mode>1.5)return c/max(vec3(1.0)-min(c,vec3(0.95)),vec3(0.05));
    if(mode>0.5)return mix(12.92*c,1.055*pow(c,vec3(1.0/2.4))-0.055,greaterThan(c,vec3(0.0031308)));
    return c;
}
void main(){
    // Derivatives BEFORE any discard (Vulkan 1.0 / OpKill).
    const float d=length(corner);const float e=fwidth(d)+0.02;
    float alpha=push.color.a;
    if(push.params.y>0.5){
        const float world=texelFetch(worldDepth,ivec2(gl_FragCoord.xy),0).r;
        if(gl_FragCoord.z>world+push.params.w){if(push.params.x<=0.0)discard;alpha*=push.params.x;}
    }
    if(d>1.0)discard;
    const int shape=int(floor(push.normal.w));const float heading=fract(push.normal.w)*6.28318531;
    float m=1.0-smoothstep(0.07,0.07+e,abs(d-0.86));
    if(shape==2)m=max(m,0.25*(1.0-smoothstep(0.80,0.86,d)));
    if(shape==3){const vec2 r=vec2(corner.x+corner.y,corner.x-corner.y)*0.70710678;
        m=max(m,(1.0-smoothstep(0.08,0.08+e,min(abs(r.x),abs(r.y))))*(1.0-smoothstep(0.66,0.70,d)));}
    if(shape==4)m=1.0-smoothstep(0.45,1.0,d);
    if(shape==5){const vec2 h=vec2(cos(heading),sin(heading));
        const vec2 r=vec2(dot(corner,h),dot(corner,vec2(-h.y,h.x)));            // r.x along the facing direction
        const float tri=max((abs(r.y)*1.6+r.x-0.62)*0.53,-0.05-r.x);            // < 0 inside the chevron (tip at r.x = 0.62)
        m=max(m,1.0-smoothstep(0.0,e,tri));}
    alpha*=m;if(alpha<0.004)discard;
    outColor=vec4(encodeOutput(clamp(push.color.rgb,vec3(0.0),vec3(1.0)),push.params.z),alpha);
}
