#version 450
// Sniper scope lens: the magnified view along the barrel, sampled from the private copy of the other eye's colour around the
// barrel's vanishing point (ScopePass.cpp). axis / right / up = (u*w, v*w, w) of the scope direction and of its right / up
// offsets (tan of the content half-angle) in that copy: uv(disc) = (axis.xy + x*right.xy + y*up.xy) / (axis.z + x*right.z + y*up.z).
// axis.w / right.w = the copied area's uv extent (outside = black). params: x flags (1 reticle duplex, 2 picture available
// (0 = dark glass), 4 near mask), y brightness, z source depth of the near distance (fully dark glass), w rim start (fraction of
// the radius); up.w = source depth of the near distance's soft end (picture fully visible).
// NEAR MASK: lensDepth = the private copy of the source eye's depth of the same square (DEPTH aspect, raw engine depth);
// a source pixel nearer than the near distance (the rifle's own bolt / tube / barrel) fades to the dark glass. The depth
// thresholds come from the CPU in the engine's own depth convention, so the ramp t = (d - z) / (up.w - z) works for forward
// (z < up.w) and reversed Z alike. One gather per pixel: the four depths' masks bilinear like the colour (soft silhouette).
// Without the mask flag binding 1 is the colour copy (bound, never read).
// The copy is in the eye's own format and written back into an eye of that format: no encode step.
// FULL ZOOM (flag 8): the quad reaches past the disc (scope.vert); outside it the eye is darkened by the surround alpha (flag
// bits 4..11 / 255), the rim starts at a fixed 0.9 (params.w = the quad scale), the reticle's lines are thinned by bits 12..19 / 255
// (the picture is far wider than the eyepiece disc).
layout(set=0,binding=0) uniform sampler2D lens;
layout(set=0,binding=1) uniform sampler2D lensDepth;
layout(push_constant) uniform Push {vec4 corner[4];vec4 axis;vec4 right;vec4 up;vec4 params;} push;
layout(location=0) in vec2 disc;
layout(location=0) out vec4 color;
float lineCover(float d,float halfWidth){const float fw=max(fwidth(d),1e-5);return 1.0-smoothstep(halfWidth-fw,halfWidth+fw,d);}
void main(){
    const float r=length(disc);
    const float fr=max(fwidth(r),1e-5);
    const int flags=int(push.params.x+0.5);
    const bool full=(flags&8)!=0;
    const float surround=full?float((flags>>4)&255)/255.0:0.0;
    if(r>1.0+fr){
        if(surround<=0.0)discard;
        color=vec4(0.0,0.0,0.0,surround);return;
    }
    const float lineScale=full?float(max((flags>>12)&255,1))/255.0:1.0;
    const float rimStart=full?0.9:push.params.w;
    const vec3 glass=vec3(0.015,0.018,0.022); // dark glass
    vec3 c=glass;
    if((flags&2)!=0){
        const vec3 h=push.axis.xyz+disc.x*push.right.xyz+disc.y*push.up.xyz;
        c=vec3(0.0);
        if(h.z>1e-6){
            const vec2 uv=h.xy/h.z;
            const vec2 hi=vec2(push.axis.w,push.right.w);
            if(all(greaterThanEqual(uv,vec2(0.0)))&&all(lessThanEqual(uv,hi))){
                c=texture(lens,uv).rgb*push.params.y;
                if((flags&4)!=0){
                    // the gather's four texels stay inside the copied w x h area (outside it the depth copy is undefined).
                    // the gather is issued at the CORNER of texels i0 / i0 + 1 (half a texel from any texel boundary), so
                    // the hardware's sub-texel rounding cannot pick texel w (undefined) while the shader weights it by ~1.
                    const vec2 size=vec2(textureSize(lensDepth,0));
                    const vec2 last=hi*size-2.0; // i0 at most w - 2 (i0 + 1 = the copy's last texel w - 1)
                    const vec2 q=clamp(uv*size-0.5,vec2(0.0),last+1.0); // texel-centre space, inside [0, w - 1]
                    const vec2 i0=min(floor(q),last);
                    const vec4 g=textureGather(lensDepth,(i0+1.0)/size,0); // x (0,1) y (1,1) z (1,0) w (0,0)
                    const vec4 t=clamp((g-push.params.z)/(push.up.w-push.params.z),0.0,1.0);
                    const vec4 k=t*t*(3.0-2.0*t);
                    const vec2 f=q-i0;
                    c=mix(glass,c,mix(mix(k.w,k.z,f.x),mix(k.x,k.y,f.x),f.y));
                }
            }
        }
    }
    c*=1.0-smoothstep(rimStart,1.0,r); // the eyepiece's dark rim
    if((flags&1)!=0){ // duplex: thin cross in the middle, thick posts from 0.55 of the radius out
        const vec2 a=abs(disc);
        float ink=max(lineCover(a.x,0.006*lineScale),lineCover(a.y,0.006*lineScale));
        ink=max(ink,max(lineCover(a.x,0.03*lineScale)*step(0.55,a.y),lineCover(a.y,0.03*lineScale)*step(0.55,a.x)));
        c*=1.0-ink;
    }
    const float edge=1.0-smoothstep(1.0-fr,1.0+fr,r);
    color=vec4(c,max(edge,surround)); // the rim is black at the edge: the surround continues it
}
