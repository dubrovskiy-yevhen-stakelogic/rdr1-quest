#version 450
precision highp float;precision highp int;
// TAA pass R "resolve": port of the GTA5 Quest TAA
// to one eye of the RDR stereo pair. No projection jitter, no motion vectors: the history is reprojected with this
// eye's depth and the eye's own current / previous view-projection (the stereo plan's matrices the eye was drawn with).
// - current sample: the eye's own pixel (GTA5 kCurScene "FXAA current" analogue: the mailbox FXAA runs AFTER TAA on the
//   result, so a still view converges to exactly today's image and a reset/rejected pixel IS today's image);
// - working domain: perceptual, sqrt of the linear colour the SRGB view returns (UNORM eyes: the stored values);
// - neighbourhood: the 5-tap cross (default, RDR cost lever) or the GTA5 3x3 (flag kTaps9) for the YCoCg moments;
//   nearest depth (forward Z: min) from two depth gathers = 7 of the 3x3 incl. the cross (GTA5 lever 1: cross);
// - history: 5-tap Catmull-Rom at the reprojected position (weight n = the centre tap's alpha, as GTA5), clipped to
//   the YCoCg box mean +- gamma sigma (at least 1/255) grown to contain the current sample (integ2), clipping towards
//   the grown box's centre; a clipped history loses weight;
// - one-sided disocclusion test: the point's inverse depth in the previous view against the four nearest history
//   inverse depths (one gather): all nearer or all farther (+-15 %) drops the history, only within 1 / w_test (far
//   thin features flipping against the sky are never tested);
// - weight n' = min(n + 1, n_max(motion)), stored as n' (1 - near): 0 in the near field (depth closer than 1 / w_near:
//   weapon / arms move with the player, not the world), so those pixels restart cleanly. The apply pass derives the
//   blend b = clamp((stored - 1) / 2) (0 at n' = 1, 1 from n' = 3, 0 in the near field); b = 0 leaves the eye pixel
//   untouched, so "reset every frame" is bit-identical to TAA off.
// - optional centred area (cost lever): only [margin, size - margin] is resolved; history outside it is never read.
// Outputs: colour = (TAA in the perceptual domain, stored weight) = next frame's history AND the apply pass's source
// (the optional sharpen happens there and is never fed back); aux = nearest inverse view depth of this frame (1/units).
// TAA FULL (flag kFull): the eye was drawn with a sub-pixel projection jitter j (framebuffer px, pc.jitter, the same
// for both eyes; vr/TaaJitter.h): its pixel p shows the un-jittered image at p - j. The history stays un-jittered
// (reprojection with the plain plan matrices) and accumulates the RAW jittered samples (over the Halton sequence this
// integrates the pixel footprint = supersampling). What leaves the pass is un-jittered: rgb = mix(current at p + j, TAA, b)
// where "current at p + j" is the planar interpolation of the cross taps (exact at j = 0), so reset / near-field /
// disoccluded pixels (b = 0) do not shake with the jitter. The apply pass then writes every pixel (no discard).
layout(location=0) out vec4 outColor;
layout(location=1) out float outAux;
layout(set=0,binding=0) uniform sampler2D eyeColor;  // the eye's colour (SRGB view: linear values), texelFetch only
layout(set=0,binding=1) uniform sampler2D eyeDepth;  // DEPTH aspect of the eye depth (forward Z, near 0, far 1), gathers
layout(set=1,binding=0) uniform sampler2D histColor; // previous outColor, linear clamp
layout(set=1,binding=1) uniform sampler2D histAux;   // previous outAux, gathers
layout(push_constant) uniform Push {
    mat4 reproject; // this eye's Vulkan NDC (x, y, depth, 1) -> the previous frame's clip of the same eye (y down)
    vec4 winv;      // inverse view depth (1/units) = dot(winv, (ndc, 1))
    vec4 tune;      // clip gamma, n max still, n max moving, 1 / motion px
    vec3 range;     // 1 / near-field distance (1/units), 1 / depth-test distance (0 = off), sharpen amount
    uint jitter;    // This eye's projection jitter in framebuffer px, int16 x | int16 y << 16, 1/8192 px units
    uvec4 info;     // width, height, flags, area margin x | margin y << 16
} pc;
const uint kReset=1u;   // no valid history
const uint kUnorm=4u;   // the eye values are already perceptual (UNORM eye format)
const uint kTaps9=16u;  // 3x3 moments (GTA5) instead of the 5-tap cross
const uint kFull=32u;   // TAA FULL: jittered input, un-jittered output, every pixel written

vec3 RgbToYCoCg(vec3 c){return vec3(dot(c,vec3(0.25,0.5,0.25)),dot(c,vec3(0.5,0.0,-0.5)),dot(c,vec3(-0.25,0.5,-0.25)));}
vec3 YCoCgToRgb(vec3 c){return vec3(c.x+c.y-c.z,c.x+c.z,c.x-c.y-c.z);}
mediump vec3 perceptual(mediump vec3 c){return (pc.info.z&kUnorm)!=0u?c:sqrt(max(c,vec3(0.0)));}

// 5-tap Catmull-Rom (bilinear taps); alpha (history weight) from the centre tap.
mediump vec4 HistoryCatmullRom(vec2 pos,vec2 rcp){
    const vec2 t1=floor(pos-0.5)+0.5,f=pos-t1;
    const vec2 w0=f*(-0.5+f*(1.0-0.5*f)),w1=1.0+f*f*(-2.5+1.5*f);
    const vec2 w2=f*(0.5+f*(2.0-1.5*f)),w3=f*f*(-0.5+0.5*f),w12=w1+w2;
    const vec2 t0=(t1-1.0)*rcp,t3=(t1+2.0)*rcp,t12=(t1+w2/w12)*rcp;
    const mediump vec4 centre=textureLod(histColor,t12,0.0);
    mediump vec3 c=textureLod(histColor,vec2(t12.x,t0.y),0.0).rgb*(w12.x*w0.y)+
                   textureLod(histColor,vec2(t0.x,t12.y),0.0).rgb*(w0.x*w12.y)+
                   centre.rgb*(w12.x*w12.y)+
                   textureLod(histColor,vec2(t3.x,t12.y),0.0).rgb*(w3.x*w12.y)+
                   textureLod(histColor,vec2(t12.x,t3.y),0.0).rgb*(w12.x*w3.y);
    const float wsum=w12.x*w0.y+w0.x*w12.y+w12.x*w12.y+w3.x*w12.y+w12.x*w3.y;
    return vec4(max(c/wsum,vec3(0.0)),centre.a);
}

void main(){
    const ivec2 size=ivec2(pc.info.xy),p=ivec2(gl_FragCoord.xy),last=size-1;
    const uint flags=pc.info.z;
    const vec2 rcp=1.0/vec2(size);
    // Moments over the cross (or 3x3); the centre is the current sample.
    const mediump vec3 cur=perceptual(texelFetch(eyeColor,p,0).rgb);
    const vec3 cy=RgbToYCoCg(cur);
    vec3 mean=cy,mean2=cy*cy;
    const ivec2 cross[4]=ivec2[4](ivec2(-1,0),ivec2(1,0),ivec2(0,-1),ivec2(0,1));
    vec3 ny[4];
    for(int k=0;k<4;++k){const vec3 y=RgbToYCoCg(perceptual(texelFetch(eyeColor,clamp(p+cross[k],ivec2(0),last),0).rgb));ny[k]=y;mean+=y;mean2+=y*y;}
    float taps=5.0;
    if((flags&kTaps9)!=0u){
        const ivec2 diagonal[4]=ivec2[4](ivec2(-1,-1),ivec2(1,-1),ivec2(-1,1),ivec2(1,1));
        for(int k=0;k<4;++k){const vec3 y=RgbToYCoCg(perceptual(texelFetch(eyeColor,clamp(p+diagonal[k],ivec2(0),last),0).rgb));mean+=y;mean2+=y*y;}
        taps=9.0;
    }
    // Nearest depth: gathers of the 2x2 quads at the pixel's top-left and bottom-right corners (7 of the 3x3).
    const vec4 g0=textureGather(eyeDepth,vec2(p)*rcp,0),g1=textureGather(eyeDepth,vec2(p+1)*rcp,0);
    const float zmin=min(min(min(g0.x,g0.y),min(g0.z,g0.w)),min(min(g1.x,g1.y),min(g1.z,g1.w)));
    const vec2 ndc=(vec2(p)+0.5)*rcp*2.0-1.0;
    const float w_c=max(dot(pc.winv,vec4(ndc,zmin,1.0)),0.0);
    mean*=1.0/taps;mean2*=1.0/taps;
    const vec3 box=max(pc.tune.x*sqrt(max(mean2-mean*mean,vec3(0.0))),vec3(1.0/255.0));
    vec3 hist_y=vec3(0.0);
    float n_hist=0.0,speed=0.0;
    if((flags&kReset)==0u){
        const vec4 prev=pc.reproject*vec4(ndc,zmin,1.0);
        if(prev.w>1e-6){
            const vec2 hp=(prev.xy/prev.w*0.5+0.5)*vec2(size);
            const vec2 margin=vec2(float(pc.info.w&0xFFFFu),float(pc.info.w>>16u));
            const vec2 lo=margin+(pc.info.w!=0u?2.0:0.0),hi=vec2(size)-lo; // area: stay 2 px inside the resolved rect
            if(all(greaterThanEqual(hp,lo))&&all(lessThanEqual(hp,hi))){
                const vec2 uv=hp*rcp;
                const mediump vec4 h=HistoryCatmullRom(hp,rcp);
                hist_y=RgbToYCoCg(h.rgb);
                n_hist=h.a;
                // Disocclusion / movers: one-sided test against the four history depths around hp.
                const float w_exp=w_c/prev.w;
                if(pc.range.y>0.0){
                    const vec4 r=textureGather(histAux,uv,0);
                    const float rmin=min(min(r.x,r.y),min(r.z,r.w)),rmax=max(max(r.x,r.y),max(r.z,r.w));
                    const float tolerance=0.15*w_exp+1e-4;
                    if(max(w_exp,rmax)>pc.range.y&&(rmin>w_exp+tolerance||rmax<w_exp-tolerance))n_hist=0.0;
                }
                // YCoCg clip: box grown to contain the current sample, clip towards its centre.
                const vec3 blo=min(mean-box,cy),bhi=max(mean+box,cy);
                const vec3 centre=0.5*(blo+bhi),extent=max(0.5*(bhi-blo),vec3(1e-6));
                const vec3 offset=hist_y-centre,ratio=abs(offset/extent);
                const float ma=max(ratio.x,max(ratio.y,ratio.z));
                if(ma>1.0)hist_y=centre+offset/ma;
                n_hist*=clamp(1.5-0.5*ma,0.0,1.0);
                speed=distance(hp,vec2(p)+0.5);
            }
        }
    }
    const float n_max=mix(pc.tune.y,pc.tune.z,clamp(speed*pc.tune.w,0.0,1.0));
    const float n_new=min(n_hist+1.0,n_max);
    const vec3 taa=clamp(YCoCgToRgb(mix(hist_y,cy,1.0/n_new)),0.0,1.0);
    const float near=smoothstep(0.8*pc.range.x,pc.range.x,w_c);
    const float stored=n_new*(1.0-near);
    if((flags&kFull)!=0u){
        // un-jittered current: the jittered image at p + j (planar interpolation over the cross; j within +-1 px)
        const vec2 j=vec2(float(int(pc.jitter<<16u)>>16),float(int(pc.jitter)>>16))*(1.0/8192.0);
        const vec2 a=min(abs(j),vec2(1.0));
        const vec3 cu=cy+a.x*((j.x>=0.0?ny[1]:ny[0])-cy)+a.y*((j.y>=0.0?ny[3]:ny[2])-cy);
        const float b=clamp((stored-1.0)*0.5,0.0,1.0);
        outColor=vec4(mix(clamp(YCoCgToRgb(cu),0.0,1.0),taa,b),stored);
    }else outColor=vec4(taa,stored);
    outAux=w_c;
}
