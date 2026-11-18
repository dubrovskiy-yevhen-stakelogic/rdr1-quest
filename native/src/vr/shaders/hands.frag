#version 450
// Hands: albedo * Lambert, tested against the eye's WORLD depth (forward Z: near 0, far 1, same projection
// rows and full-eye viewport as the world pass, so gl_FragCoord addresses the same texel). The hand's own depth
// (self-occlusion) is the pass's depth attachment; the world depth is never written.
layout(set=0,binding=0) uniform sampler2D albedo;
layout(set=0,binding=1) uniform sampler2D worldDepth;
layout(push_constant) uniform Push {mat4 clip;vec4 weights;vec4 light;vec4 params;vec4 blood;} push;
layout(location=0) in vec2 uv;
layout(location=1) in float shade;
layout(location=2) in vec4 blood;
layout(location=0) out vec4 color;
vec3 encodeOutput(vec3 c,float mode){
    if(mode>1.5)return c/max(vec3(1.0)-min(c,vec3(0.95)),vec3(0.05));            // HDR target: inverse of c/(1+c)
    if(mode>0.5)return mix(12.92*c,1.055*pow(c,vec3(1.0/2.4))-0.055,greaterThan(c,vec3(0.0031308))); // UNORM target
    return c;                                                                    // SRGB target: the attachment encodes
}
void main(){
    if(push.params.y>0.5){
        const float world=texelFetch(worldDepth,ivec2(gl_FragCoord.xy),0).r;
        if(gl_FragCoord.z>world+push.params.w)discard;
    }
    vec3 a=texture(albedo,uv).rgb;
    if(blood.w>0.0){ // BLOOD ON THE KNUCKLES: dark red within ~3 cm of the knuckle face, blotched on 7 mm cells
        const float core=1.0-smoothstep(0.016,0.034,length(blood.xyz));
        const float n=fract(sin(dot(floor(blood.xyz*140.0),vec3(12.9898,78.233,37.719)))*43758.5453);
        a=mix(a,vec3(0.30,0.018,0.012),clamp(core*mix(0.45,1.0,n)*blood.w,0.0,0.9));
    }
    const vec3 c=clamp(a*shade*push.params.x,vec3(0.0),vec3(1.0));
    color=vec4(encodeOutput(c,push.params.z),1.0);
}
