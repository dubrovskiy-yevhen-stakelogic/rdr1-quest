#version 450
// Laser sight beam / end dot, alpha-blended, world-depth tested like the hands when the eye depth is sampled
// (params.y > 0.5), occluded by the hands through the private depth (test, no write). Red with a hot core.
layout(set=0,binding=1) uniform sampler2D worldDepth;
layout(push_constant) uniform Push {mat4 viewProj;vec4 a;vec4 b;vec4 eye;vec4 params;} push;
layout(location=0) in vec2 coord;
layout(location=0) out vec4 color;
vec3 encodeOutput(vec3 c,float mode){
    if(mode>1.5)return c/max(vec3(1.0)-min(c,vec3(0.95)),vec3(0.05));
    if(mode>0.5)return mix(12.92*c,1.055*pow(c,vec3(1.0/2.4))-0.055,greaterThan(c,vec3(0.0031308)));
    return c;
}
void main(){
    if(push.params.y>0.5){
        const float world=texelFetch(worldDepth,ivec2(gl_FragCoord.xy),0).r;
        if(gl_FragCoord.z>world+push.params.w)discard;
    }
    float alpha;float core;
    if(push.b.w<0.5){
        const float across=1.0-abs(coord.y);
        alpha=smoothstep(0.0,0.55,across)*0.9;
        core=pow(across,6.0);
    }else{
        const float d=length(coord);
        if(d>1.0)discard;
        alpha=1.0-smoothstep(0.55,1.0,d);
        core=1.0-smoothstep(0.0,0.45,d);
    }
    if(alpha<0.004)discard;
    const vec3 tint=mix(vec3(1.0,0.06,0.04),vec3(1.0,0.75,0.7),core)*push.eye.w;
    color=vec4(encodeOutput(clamp(tint,vec3(0.0),vec3(1.0)),push.params.z),alpha);
}
