#version 450
// Holster marker ring, alpha-blended over the hands (no hand-depth test), world-depth tested like the
// hands when the eye depth is sampled. Idle = thin pale ring; highlighted (hand inside the grab radius) = thicker gold
// ring with a faint fill.
// Palette by params.x (0 = the holster ring above). Reload overlay (channel 2b): 1 EMPTY chamber
// (white ring), 2 SPENT (amber ring; the rod tip pulses through highlight), 3 LIVE (brass disc, debug), 4 the held round's tip (warm
// disc), 5 ammo zone (faint blue ring), 6 debug point (magenta); 7 the hammer ring (HAMMER H3, highlight blinks).
layout(set=0,binding=1) uniform sampler2D worldDepth;
layout(push_constant) uniform Push {mat4 viewProj;vec4 centre;vec4 right;vec4 up;vec4 params;} push;
layout(location=0) in vec2 corner;
layout(location=1) in float highlight;
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
    const float d=length(corner);
    if(d>1.0)discard;
    const float h=clamp(highlight,0.0,1.0);
    const int style=int(push.params.x+0.5);
    const float edge=fwidth(d)+0.02;
    float alpha;vec3 tint;
    if(style==0){
        const float half_width=mix(0.06,0.12,h);
        const float ring=1.0-smoothstep(half_width,half_width+edge,abs(d-0.86));
        const float fill=0.22*h*(1.0-smoothstep(0.80,0.86,d));
        alpha=clamp(max(ring*mix(0.45,0.9,h),fill),0.0,1.0);
        tint=mix(vec3(0.85,0.82,0.74),vec3(1.0,0.72,0.18),h);
    }else if(style==3||style==4||style==6){ // discs: LIVE round, the held tip, debug points
        const float disc=1.0-smoothstep(0.70,0.70+edge,d);
        const float rim=1.0-smoothstep(0.10,0.10+edge,abs(d-0.82));
        alpha=clamp(max(disc*0.85,rim*0.6),0.0,1.0);
        tint=style==3?vec3(0.85,0.68,0.30):style==4?vec3(1.0,0.86,0.55):vec3(1.0,0.30,0.90);
        if(style==6)tint=mix(tint,vec3(1.0),0.5*h);
    }else{ // rings: EMPTY, SPENT, zone, hammer
        const float half_width=style==5?mix(0.035,0.07,h):mix(0.10,0.16,h);
        const float ring=1.0-smoothstep(half_width,half_width+edge,abs(d-0.80));
        const float fill=(style==5?0.10*h:0.18*h)*(1.0-smoothstep(0.70,0.78,d));
        const float strength=style==5?mix(0.30,0.65,h):style==2?mix(0.55,1.0,h):mix(0.70,1.0,h);
        alpha=clamp(max(ring*strength,fill),0.0,1.0);
        tint=style==1?vec3(0.95,0.95,0.92):style==2?vec3(1.0,0.52,0.10):style==5?vec3(0.50,0.80,1.0):vec3(1.0,0.84,0.30);
    }
    if(alpha<0.004)discard;
    tint*=push.up.w;
    color=vec4(encodeOutput(clamp(tint,vec3(0.0),vec3(1.0)),push.params.z),alpha);
}
