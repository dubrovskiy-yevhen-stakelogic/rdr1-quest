#version 450
precision highp float;precision highp int;
// TAA pass A "apply": writes the resolve result into the eye colour with fixed-function blending
// (SRC_ALPHA / ONE_MINUS_SRC_ALPHA, RGB only): eye = mix(eye, TAA, b), b = clamp((weight - 1) / 2) - the GTA5 confidence
// (0 at weight 1, 1 from weight 3; the near field stores weight 0). Pixels with b = 0 (reset, rejected history, near
// field) are discarded, so they keep the eye's own bits exactly.
// Optional sharpen (kSharpen: TAA FULL, or debug bit 3 in TAA): the GTA5 clamped 5-tap sharpen
// (taa_sharpen.comp) of the new history, never fed back into it.
// TAA FULL (kFull): the resolve output is already the un-jittered display colour of EVERY pixel (b = 0 pixels hold the
// un-jittered current), so nothing is discarded and the blend alpha is 1 (eye = resolve output).
layout(location=0) out vec4 outColor;
layout(set=0,binding=0) uniform sampler2D taaColor; // resolve output (perceptual rgb, a = weight), texelFetch only
layout(push_constant) uniform Push {mat4 reproject;vec4 winv;vec4 tune;vec3 range;uint jitter;uvec4 info;} pc;
const uint kPure=2u;    // debug: display = pure TAA (b = 1)
const uint kUnorm=4u;
const uint kSharpen=8u;
const uint kFull=32u;
const uint kLite=64u;   // TAA LITE: taaColor is the LITE history (eye format, rgb = the displayed colour): plain copy
// TAA FULL and TAA LITE run this shader through the "copy" pipeline (no blending, eye loadOp DONT_CARE: every pixel of
// the render area is written, alpha = 1); TAA keeps the blended LOAD pipeline with the discard.
void main(){
    const ivec2 p=ivec2(gl_FragCoord.xy),last=ivec2(pc.info.xy)-1;
    if((pc.info.z&kLite)!=0u){outColor=vec4(texelFetch(taaColor,p,0).rgb,1.0);return;}
    const mediump vec4 t=texelFetch(taaColor,p,0);
    const bool full=(pc.info.z&kFull)!=0u;
    const float b=(full||(pc.info.z&kPure)!=0u)?1.0:clamp((t.a-1.0)*0.5,0.0,1.0);
    if(!(b>0.0))discard;
    mediump vec3 c=t.rgb;
    if((pc.info.z&kSharpen)!=0u){
        const mediump vec3 n=texelFetch(taaColor,ivec2(p.x,max(p.y-1,0)),0).rgb;
        const mediump vec3 s=texelFetch(taaColor,ivec2(p.x,min(p.y+1,last.y)),0).rgb;
        const mediump vec3 w=texelFetch(taaColor,ivec2(max(p.x-1,0),p.y),0).rgb;
        const mediump vec3 e=texelFetch(taaColor,ivec2(min(p.x+1,last.x),p.y),0).rgb;
        const mediump vec3 lo=min(min(min(n,s),min(w,e)),c),hi=max(max(max(n,s),max(w,e)),c);
        c=clamp(c+pc.range.z*0.25*(4.0*c-n-s-w-e),lo,hi);
    }
    outColor=vec4((pc.info.z&kUnorm)!=0u?c:c*c,b);
}
