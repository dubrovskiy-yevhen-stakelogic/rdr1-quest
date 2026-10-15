#version 450
// Cartridge shading - brass case, nickel primer + lead bullet (LIVE), dull brass + struck dark primer + sooty open mouth
// (SPENT); a top light, a head light and a metal highlight. Opaque, depth-tested and written against the pass's private depth (the VR
// hands occlude it); never world-tested (the overlay is drawn over the engine gun), so a chambered round is hidden ANALYTICALLY by the
// open cylinder: cyl.xyz = the rear-face centre - base, cyl.w = the outer radius, params.w = the cylinder length (0 = no occluder). A
// fragment is discarded when the eye ray reaches the solid cylinder before it, unless the ray enters through this round's own chamber
// mouth (the rear or front face within 1.06 r of the chamber axis): the rim / primer show at the rear face, the bullet tip at the front.
// params: x style (8 live, 9 spent), y brightness, z output encode (as hands.frag), w cylinder length.
layout(push_constant) uniform Push {mat4 viewProj;vec4 base;vec4 axis;vec4 cyl;vec4 params;} push;
layout(location=0) in vec3 worldPos;
layout(location=1) in vec3 normal;
layout(location=2) in float radial;
layout(location=3) flat in int band;
layout(location=4) flat in vec3 eye;
layout(location=0) out vec4 color;
vec3 encodeOutput(vec3 c,float mode){
    if(mode>1.5)return c/max(vec3(1.0)-min(c,vec3(0.95)),vec3(0.05));
    if(mode>0.5)return mix(12.92*c,1.055*pow(c,vec3(1.0/2.4))-0.055,greaterThan(c,vec3(0.0031308)));
    return c;
}
void main(){
    const bool live=push.params.x<8.5;
    const vec3 a=normalize(push.axis.xyz);
    const float r=push.base.w;
    if(push.cyl.w>0.0&&push.params.w>0.0){
        const vec3 eRel=eye-push.base.xyz,d=worldPos-eye;
        const float z0=dot(push.cyl.xyz,a),len=push.params.w,Rc=push.cyl.w;
        const vec3 cp=push.cyl.xyz-a*z0;
        const float ez=dot(eRel,a),dz=dot(d,a);
        float t0=-1e9,t1=1e9;bool slab=true;
        if(abs(dz)>1e-9){const float ta=(z0-ez)/dz,tb=(z0+len-ez)/dz;t0=min(ta,tb);t1=max(ta,tb);}
        else slab=ez>=z0&&ez<=z0+len;
        const vec3 eq=(eRel-a*ez)-cp,dq=d-a*dz;
        const float A=dot(dq,dq),B=2.0*dot(eq,dq),C=dot(eq,eq)-Rc*Rc;
        float s0=-1e9,s1=1e9;bool tube=true;
        if(A>1e-14){const float disc=B*B-4.0*A*C;if(disc<0.0)tube=false;else{const float q=sqrt(disc);s0=(-B-q)/(2.0*A);s1=(-B+q)/(2.0*A);}}
        else tube=C<0.0;
        if(slab&&tube){
            const float tin=max(t0,s0),tout=min(t1,s1);
            if(tin<tout&&tin<0.9999&&tout>0.0){
                bool visible=false;
                if(t0>=s0){const vec3 X=eRel+d*tin;const float zx=dot(X,a);visible=length(X-a*zx)<1.06*r;} // through a face: own mouth?
                if(!visible)discard;
            }
        }
    }
    const vec3 brass=live?vec3(0.62,0.42,0.13):vec3(0.40,0.28,0.10);
    vec3 base=brass;float metal=live?0.75:0.45;
    if(band==0){ // the case head: primer + headstamp
        if(radial<0.36)base=live?vec3(0.58,0.56,0.52):vec3(0.09,0.08,0.07);
        else if(radial<0.42)base=live?vec3(0.50,0.27,0.15):vec3(0.06,0.05,0.04);
        if(!live&&radial<0.12)base=vec3(0.025);
    }else if(band>=5){ // LIVE: the lead bullet; SPENT: the sooty inside of the open mouth
        base=live?vec3(0.36,0.35,0.37):vec3(0.035,0.03,0.025);metal=live?0.25:0.0;
    }
    const vec3 V=normalize(eye-worldPos);
    vec3 N=normalize(normal);if(dot(N,V)<0.0)N=-N;
    const vec3 Ltop=normalize(vec3(0.3,1.0,0.2));
    const float diffuse=0.30+0.45*max(dot(N,Ltop),0.0)+0.35*max(dot(N,V),0.0);
    const vec3 H=normalize(Ltop+V);
    const float spec=metal*(0.9*pow(max(dot(N,H),0.0),40.0)+0.35*pow(max(dot(N,V),0.0),24.0));
    vec3 c=base*diffuse+spec*mix(vec3(1.0),base*1.6,0.5);
    c*=push.params.y;
    color=vec4(encodeOutput(clamp(c,vec3(0.0),vec3(1.0)),push.params.z),1.0);
}
