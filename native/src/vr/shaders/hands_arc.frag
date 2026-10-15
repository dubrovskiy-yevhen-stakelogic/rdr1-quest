#version 450
// ARC: ribbon colour. World-depth test like the laser (SAMPLED) with a per-point tolerance (pulled depth from the vertex
// shader); where the world hides the ribbon: discard, or x-ray alpha. 1-px edge anti-aliasing in pixels.
layout(set=0,binding=1) uniform sampler2D worldDepth;
layout(push_constant) uniform Push {mat4 viewProj;vec4 eye;vec4 params;vec4 style;vec4 shape;} push;
layout(location=0) in vec2 coord;
layout(location=1) in vec4 color;
layout(location=2) noperspective in float toleratedDepth;
layout(location=3) in float widthAlpha;
layout(location=0) out vec4 outColor;
vec3 encodeOutput(vec3 c,float mode){
    if(mode>1.5)return c/max(vec3(1.0)-min(c,vec3(0.95)),vec3(0.05));
    if(mode>0.5)return mix(12.92*c,1.055*pow(c,vec3(1.0/2.4))-0.055,greaterThan(c,vec3(0.0031308)));
    return c;
}
void main(){
    // Every derivative BEFORE any discard: Vulkan 1.0 / SPIR-V OpKill leaves derivatives undefined in a quad with killed lanes.
    const float across=abs(coord.y);
    const float edge=max(fwidth(coord.y),1e-4);                               // one pixel, in `across` units
    float dash=1.0;
    if(push.style.x>0.0){                                                     // dynamically uniform (push constant)
        const float s=push.shape.w>0.0?push.shape.w-coord.x:coord.x;          // DashFromEnd: phase anchored at the target
        const float u=(s-push.style.z)/push.style.x;const float e=max(fwidth(u),1e-4);const float f=fract(u);
        dash=smoothstep(0.0,e,f)*(1.0-smoothstep(push.style.y-e,push.style.y,f));
    }
    float alpha=color.a*widthAlpha*dash*(1.0-smoothstep(1.0-edge,1.0,across));
    if(push.shape.x>0.0)alpha*=smoothstep(0.0,push.shape.x,coord.x);          // fade in from the hand
    if(push.params.y>0.5){
        const float world=texelFetch(worldDepth,ivec2(gl_FragCoord.xy),0).r;
        if(min(gl_FragCoord.z,toleratedDepth)>world+push.params.w){if(push.style.w<=0.0)discard;alpha*=push.style.w;}
    }
    if(alpha<0.004)discard;
    const float profile=sqrt(max(1.0-across*across,0.0));
    const vec3 c=(color.rgb*mix(1.0,mix(0.55,1.0,profile),push.shape.y)+vec3(push.shape.z*pow(profile,8.0)))*push.eye.w;
    outColor=vec4(encodeOutput(clamp(c,vec3(0.0),vec3(1.0)),push.params.z),alpha);
}
