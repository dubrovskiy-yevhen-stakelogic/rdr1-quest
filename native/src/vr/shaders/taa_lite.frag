#version 450
precision highp float;precision highp int;
// TAA LITE, pass R "resolve": the cheap variant of taa_resolve.frag for the Quest 3S (tiled,
// bandwidth / texture bound GPU). Same reprojection (this eye's plan matrices, no jitter, no motion
// vectors), same reset rules and near field; what is cheaper:
// - history = ONE image in the eye's own format (B8G8R8A8_SRGB on the Quest: 4 B/px instead of RGBA16F 8 B + R16F 2 B),
//   rgb = the colour this frame DISPLAYS, a = history weight / 16; no aux image (no depth history);
// - output = that history image; the apply pass is a plain copy into the eye (no blending, eye loadOp DONT_CARE);
// - current neighbourhood: the 5-tap cross YCoCg moments as TAA (the eye taps are cache hits: ALU only, no extra bandwidth);
// - depth: ONE gather (nearest of the 2x2 at p..p+1) instead of two; flag kNoDepth: rotation-only (far plane);
// - history: one bilinear tap (default) or the 5-tap Catmull-Rom (flag kCubic);
// - no one-sided depth test (the clip against the cross box rejects disocclusions / movers);
// - only the centred area [margin, size - margin] is processed (pc.info.w; the periphery stays the raw eye).
// b = clamp((weight - 1) / 2) as in TAA; b = 0 pixels (reset, near field, first frame) store the eye texel unchanged
// (bit-exact through the SRGB round trip), so "reset every frame" leaves the eye bit-identical to TAA off.
// FP16 (debug.rdrvr.vr.taa.lite.fp16): TaaPass builds a 2nd LITE pipeline with the specialization
// constant kHalf = true -> resolveHalf (the colour / neighbourhood / clip / Catmull-Rom sum in
// mediump = fp16 on Adreno; pixel positions, reprojection, depth, texture coordinates, Catmull-Rom
// weights and the history weight n stay highp). The default pipeline (kHalf = false, no
// specialization) runs resolveFull; the driver folds the branch.
layout(location=0) out vec4 outColor;
layout(set=0,binding=0) uniform sampler2D eyeColor;  // the eye's colour (SRGB view: linear values), texelFetch only
layout(set=0,binding=1) uniform sampler2D eyeDepth;  // DEPTH aspect of the eye depth (forward Z, near 0, far 1), one gather
layout(set=1,binding=0) uniform sampler2D histColor; // previous outColor (eye format), linear clamp
layout(push_constant) uniform Push {
    mat4 reproject; // this eye's Vulkan NDC (x, y, depth, 1) -> the previous frame's clip of the same eye (y down)
    vec4 winv;      // inverse view depth (1/units) = dot(winv, (ndc, 1))
    vec4 tune;      // clip gamma, n max still, n max moving, 1 / motion px
    vec3 range;     // 1 / near-field distance (1/units), sub-pixel weight k (LITE; TaaMath liteSubpx), sharpen (unused)
    uint jitter;    // unused (LITE has no jitter)
    uvec4 info;     // width, height, flags, area margin x | margin y << 16
} pc;
const uint kReset=1u;
const uint kUnorm=4u;
const uint kCubic=128u;
const uint kNoDepth=256u;
const float kWeightScale=16.0,kMaxWeight=15.0; // = TaaMath.h liteWeightScale / liteMaxWeight
layout(constant_id=0) const bool kHalf=false; // TaaMath.h liteHalfSpecId; true = resolveHalf

vec3 RgbToYCoCg(vec3 c){return vec3(dot(c,vec3(0.25,0.5,0.25)),dot(c,vec3(0.5,0.0,-0.5)),dot(c,vec3(-0.25,0.5,-0.25)));}
vec3 YCoCgToRgb(vec3 c){return vec3(c.x+c.y-c.z,c.x+c.z,c.x-c.y-c.z);}
mediump vec3 perceptual(mediump vec3 c){return (pc.info.z&kUnorm)!=0u?c:sqrt(max(c,vec3(0.0)));}
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

void resolveFull(){
    const ivec2 size=ivec2(pc.info.xy),p=ivec2(gl_FragCoord.xy),last=size-1;
    const uint flags=pc.info.z;
    const vec2 rcp=1.0/vec2(size);
    const vec4 raw=texelFetch(eyeColor,p,0);
    const vec3 cur=RgbToYCoCg(perceptual(raw.rgb));
    // YCoCg moments of the cross (as TAA): mean +- gamma sigma, at least 1/255, grown to contain the
    // current sample. A plain RGB min/max box is wide in every RGB channel on checkered ground and lets
    // a red history survive (LITE has no depth test to drop it); the YCoCg chroma box is tight on grey
    // content.
    vec3 mean=cur,mean2=cur*cur;
    const ivec2 cross[4]=ivec2[4](ivec2(-1,0),ivec2(1,0),ivec2(0,-1),ivec2(0,1));
    for(int k=0;k<4;++k){const vec3 c=RgbToYCoCg(perceptual(texelFetch(eyeColor,clamp(p+cross[k],ivec2(0),last),0).rgb));mean+=c;mean2+=c*c;}
    mean*=0.2;mean2*=0.2;
    const vec3 box=max(pc.tune.x*sqrt(max(mean2-mean*mean,vec3(0.0))),vec3(1.0/255.0));
    const vec3 lo=min(mean-box,cur),hi=max(mean+box,cur);
    float z=1.0;
    if((flags&kNoDepth)==0u){const vec4 g=textureGather(eyeDepth,vec2(p+1)*rcp,0);z=min(min(g.x,g.y),min(g.z,g.w));}
    const vec2 ndc=(vec2(p)+0.5)*rcp*2.0-1.0;
    const float w_c=max(dot(pc.winv,vec4(ndc,z,1.0)),0.0);
    vec3 hist=cur;
    float n_hist=0.0,speed=0.0,s=0.0; // s = bilinear resampling blur of this history tap (0 on a texel centre .. 2 at a corner)
    if((flags&kReset)==0u){
        const vec4 prev=pc.reproject*vec4(ndc,z,1.0);
        if(prev.w>1e-6){
            const vec2 hp=(prev.xy/prev.w*0.5+0.5)*vec2(size);
            const vec2 margin=vec2(float(pc.info.w&0xFFFFu),float(pc.info.w>>16u));
            const vec2 lim=margin+(pc.info.w!=0u?2.0:0.0),him=vec2(size)-lim; // area: stay 2 px inside the resolved rect
            if(all(greaterThanEqual(hp,lim))&&all(lessThanEqual(hp,him))){
                const mediump vec4 h=(flags&kCubic)!=0u?HistoryCatmullRom(hp,rcp):textureLod(histColor,hp*rcp,0.0);
                const vec2 fr=fract(hp-0.5);s=4.0*(fr.x*(1.0-fr.x)+fr.y*(1.0-fr.y));
                hist=RgbToYCoCg(perceptual(h.rgb));
                n_hist=h.a*kWeightScale;
                // clip towards the centre of the box: rejects disocclusions and movers (no depth test in LITE)
                const vec3 centre=0.5*(lo+hi),extent=max(0.5*(hi-lo),vec3(1e-6));
                const vec3 offset=hist-centre,ratio=abs(offset/extent);
                const float ma=max(ratio.x,max(ratio.y,ratio.z));
                if(ma>1.0)hist=centre+offset/ma;
                n_hist*=clamp(1.5-0.5*ma,0.0,1.0);
                speed=distance(hp,vec2(p)+0.5);
            }
        }
    }
    // Fewer accumulated frames exactly when the history tap lies between texels - every frame of
    // bilinear resampling adds f(1-f) px^2 of blur, the EMA keeps ~n of them (soap under real head motion); a still head (s = 0) keeps n.
    const float n_max=max(min(mix(pc.tune.y,pc.tune.z,clamp(speed*pc.tune.w,0.0,1.0)),kMaxWeight)/(1.0+pc.range.y*s),min(2.0,pc.tune.y));
    const float n_new=min(n_hist+1.0,n_max);
    const vec3 taa=mix(hist,cur,1.0/n_new);
    const float near=smoothstep(0.8*pc.range.x,pc.range.x,w_c);
    const float stored=n_new*(1.0-near);
    const float b=clamp((stored-1.0)*0.5,0.0,1.0);
    const vec3 shown=clamp(YCoCgToRgb(mix(cur,taa,b)),0.0,1.0);
    const vec3 encoded=(flags&kUnorm)!=0u?shown:shown*shown;
    outColor=vec4(b>0.0?encoded:raw.rgb,stored*(1.0/kWeightScale)); // b = 0: the eye texel itself (bit-exact)
}

mediump vec3 RgbToYCoCgH(mediump vec3 c){return vec3(dot(c,vec3(0.25,0.5,0.25)),dot(c,vec3(0.5,0.0,-0.5)),dot(c,vec3(-0.25,0.5,-0.25)));}
mediump vec3 YCoCgToRgbH(mediump vec3 c){return vec3(c.x+c.y-c.z,c.x+c.z,c.x-c.y-c.z);}
// Relaxed (fp16) texel results through a mediump sampler parameter (no 2nd descriptor variable); integer coordinates only.
// The history taps (normalised float coordinates) stay highp fetches: a relaxed sample may also truncate its operands, and a
// fp16 coordinate is up to ~0.4 px off at x ~ 1680.
mediump vec4 FetchH(mediump sampler2D t,ivec2 p){return texelFetch(t,p,0);}
mediump vec3 TapH(ivec2 p,ivec2 last){return RgbToYCoCgH(perceptual(FetchH(eyeColor,clamp(p,ivec2(0),last)).rgb));}
// The same 5-tap Catmull-Rom: weights and coordinates highp, the weights normalised in highp (sum 1), the colour sum in fp16;
// the history weight (alpha) is returned highp.
mediump vec3 HistoryCatmullRomH(vec2 pos,vec2 rcp,out float alpha){
    const vec2 t1=floor(pos-0.5)+0.5,f=pos-t1;
    const vec2 w0=f*(-0.5+f*(1.0-0.5*f)),w1=1.0+f*f*(-2.5+1.5*f);
    const vec2 w2=f*(0.5+f*(2.0-1.5*f)),w3=f*f*(-0.5+0.5*f),w12=w1+w2;
    const vec2 t0=(t1-1.0)*rcp,t3=(t1+2.0)*rcp,t12=(t1+w2/w12)*rcp;
    const float a=w12.x*w0.y,b=w0.x*w12.y,c=w12.x*w12.y,d=w3.x*w12.y,e=w12.x*w3.y,r=1.0/(a+b+c+d+e);
    const mediump float wa=a*r,wb=b*r,wc=c*r,wd=d*r,we=e*r;
    const vec4 centre=textureLod(histColor,t12,0.0);alpha=centre.a;
    const mediump vec3 ha=textureLod(histColor,vec2(t12.x,t0.y),0.0).rgb,hb=textureLod(histColor,vec2(t0.x,t12.y),0.0).rgb,hc=centre.rgb;
    const mediump vec3 hd=textureLod(histColor,vec2(t3.x,t12.y),0.0).rgb,he=textureLod(histColor,vec2(t12.x,t3.y),0.0).rgb;
    return max(ha*wa+hb*wb+hc*wc+hd*wd+he*we,vec3(0.0));
}
// resolveFull with `precision mediump float` (every unqualified float below is fp16; highp is spelled out). Differences:
// - variance as the mean of squared deviations (x16 before squaring: no fp16 cancellation of
//   mean2 - mean^2, and the squares stay fp16 normals wherever gamma sigma exceeds the 1/255 floor),
//   box = gamma sigma as in resolveFull;
// - the clip guard 1e-4 (an fp16 normal) instead of 1e-6 (never active: the box makes the extent >= 1/255);
// - b > 0 is decided on the highp b (the b = 0 passthrough stores the fp16 eye texel: exact through the 8-bit SRGB round trip,
//   the fp16 error is < 0.06 LSB).
void resolveHalf(){
    precision mediump float;
    const ivec2 size=ivec2(pc.info.xy),p=ivec2(gl_FragCoord.xy),last=size-1;
    const uint flags=pc.info.z;
    const highp vec2 rcp=1.0/vec2(size);
    const vec4 raw=FetchH(eyeColor,p);
    const vec3 cur=RgbToYCoCgH(perceptual(raw.rgb));
    const vec3 c1=TapH(p+ivec2(-1,0),last),c2=TapH(p+ivec2(1,0),last),c3=TapH(p+ivec2(0,-1),last),c4=TapH(p+ivec2(0,1),last);
    const vec3 mean=(cur+c1+c2+c3+c4)*0.2;
    const vec3 d0=(cur-mean)*16.0,d1=(c1-mean)*16.0,d2=(c2-mean)*16.0,d3=(c3-mean)*16.0,d4=(c4-mean)*16.0;
    const float gamma=pc.tune.x*0.0625; // clip gamma / 16 (the deviations are x16)
    const vec3 box=max(gamma*sqrt((d0*d0+d1*d1+d2*d2+d3*d3+d4*d4)*0.2),vec3(1.0/255.0));
    const vec3 lo=min(mean-box,cur),hi=max(mean+box,cur);
    highp float z=1.0;
    if((flags&kNoDepth)==0u){const highp vec4 g=textureGather(eyeDepth,vec2(p+1)*rcp,0);z=min(min(g.x,g.y),min(g.z,g.w));}
    const highp vec2 ndc=(vec2(p)+0.5)*rcp*2.0-1.0;
    const highp float w_c=max(dot(pc.winv,vec4(ndc,z,1.0)),0.0);
    vec3 hist=cur;
    highp float n_hist=0.0,speed=0.0,s=0.0;
    if((flags&kReset)==0u){
        const highp vec4 prev=pc.reproject*vec4(ndc,z,1.0);
        if(prev.w>1e-6){
            const highp vec2 hp=(prev.xy/prev.w*0.5+0.5)*vec2(size);
            const highp vec2 margin=vec2(float(pc.info.w&0xFFFFu),float(pc.info.w>>16u));
            const highp vec2 lim=margin+(pc.info.w!=0u?2.0:0.0),him=vec2(size)-lim;
            if(all(greaterThanEqual(hp,lim))&&all(lessThanEqual(hp,him))){
                highp float alpha;vec3 h;
                if((flags&kCubic)!=0u)h=HistoryCatmullRomH(hp,rcp,alpha);
                else{const highp vec4 t=textureLod(histColor,hp*rcp,0.0);h=t.rgb;alpha=t.a;}
                const highp vec2 fr=fract(hp-0.5);s=4.0*(fr.x*(1.0-fr.x)+fr.y*(1.0-fr.y));
                hist=RgbToYCoCgH(perceptual(h));
                n_hist=alpha*kWeightScale;
                const vec3 centre=0.5*(lo+hi),extent=max(0.5*(hi-lo),vec3(1e-4));
                const vec3 offset=hist-centre,ratio=abs(offset/extent);
                const float ma=max(ratio.x,max(ratio.y,ratio.z));
                if(ma>1.0)hist=centre+offset/ma;
                n_hist*=clamp(1.5-0.5*ma,0.0,1.0);
                speed=distance(hp,vec2(p)+0.5);
            }
        }
    }
    const highp float n_max=max(min(mix(pc.tune.y,pc.tune.z,clamp(speed*pc.tune.w,0.0,1.0)),kMaxWeight)/(1.0+pc.range.y*s),min(2.0,pc.tune.y));
    const highp float n_new=min(n_hist+1.0,n_max);
    const float wcur=1.0/n_new;
    const vec3 taa=mix(hist,cur,wcur);
    const highp float near=smoothstep(0.8*pc.range.x,pc.range.x,w_c);
    const highp float stored=n_new*(1.0-near);
    const highp float bh=clamp((stored-1.0)*0.5,0.0,1.0);
    const float b=bh;
    const vec3 shown=clamp(YCoCgToRgbH(mix(cur,taa,b)),0.0,1.0);
    const vec3 encoded=(flags&kUnorm)!=0u?shown:shown*shown;
    outColor=vec4(bh>0.0?encoded:raw.rgb,stored*(1.0/kWeightScale)); // b = 0: the eye texel itself
}
void main(){if(kHalf)resolveHalf();else resolveFull();} // kHalf is a specialization constant (one branch survives)
