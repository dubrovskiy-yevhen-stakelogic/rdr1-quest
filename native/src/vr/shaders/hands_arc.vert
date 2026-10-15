#version 450
// ARC: polyline ribbon. One instance per segment k (point k -> k+1), 6 vertices, no index buffer. Four instance-rate bindings on
// the same vertex ring at entry offsets 0/1/2/3 of the padded run [p0, p0, p1, ..., pn-1, pn-1]: prev, a, b, next.
// The joint between instances k and k+1 is computed from the SAME operands through ONE code path -> bitwise identical (watertight);
// the extra outputs (tolerated depth, width alpha) also depend only on that joint's point.
layout(location=0) in vec4 prevP;            // binding 0: xyz (w unused)
layout(location=1) in vec4 aP;               // binding 1: xyz LOCAL metres, w half width (m)
layout(location=2) in vec4 aColor;           // binding 1: R8G8B8A8_UNORM, linear colour + alpha
layout(location=3) in vec2 aS;               // binding 1: x arc length from point 0 (m), y depth tolerance (m, >= 0)
layout(location=4) in vec4 bP;               // binding 2
layout(location=5) in vec4 bColor;
layout(location=6) in vec2 bS;
layout(location=7) in vec4 nextP;            // binding 3
// params: x minimum half width per metre of distance (minHalfPx * pixelAngle), y worldTest, z encode, w depthEpsilon (window z)
layout(push_constant) uniform Push {mat4 viewProj;vec4 eye;vec4 params;vec4 style;vec4 shape;} push;
layout(location=0) out vec2 coord;                         // x arc length (m), y across -1..1 of the DRAWN width
layout(location=1) out vec4 color;
layout(location=2) noperspective out float toleratedDepth; // window depth of this corner pulled toward the eye by the tolerance
layout(location=3) out float widthAlpha;                   // true / drawn width (< 1 where the minimum pixel width widened it)
const vec2 corners[6]=vec2[6](vec2(0,-1),vec2(1,-1),vec2(1,1),vec2(0,-1),vec2(1,1),vec2(0,1));
vec3 anyPerpendicular(vec3 v){return abs(v.y)<0.99?normalize(cross(v,vec3(0,1,0))):normalize(cross(v,vec3(1,0,0)));}
void main(){
    const vec2 q=corners[gl_VertexIndex];
    const bool atB=q.x>0.5;
    const vec3 lo=atB?aP.xyz:prevP.xyz;      // end of k: (p_k, p_k+2); start of k+1: (p_k, p_k+2) - same operands
    const vec3 hi=atB?nextP.xyz:bP.xyz;
    const vec4 P=atB?bP:aP;
    vec3 t=hi-lo;t=dot(t,t)>1e-12?normalize(t):vec3(0,0,-1);
    const vec3 toEye=push.eye.xyz-P.xyz;const float dist=max(length(toEye),1e-4);
    vec3 side=cross(t,toEye);side=dot(side,side)>1e-12?normalize(side):anyPerpendicular(t);
    const float drawn=max(P.w,push.params.x*dist);
    const vec3 corner=P.xyz+side*drawn*q.y;
    gl_Position=push.viewProj*vec4(corner,1.0);
    // Looser world-depth test near the ground: the same screen position (a point on the corner's own eye ray), `tol` metres closer.
    const float tol=max(atB?bS.y:aS.y,0.0);
    const vec3 towardEye=push.eye.xyz-corner;const float ce=length(towardEye);
    const vec3 pulled=corner+(ce>1e-4?towardEye/ce:vec3(0.0))*min(tol,0.9*ce);
    const vec4 c2=push.viewProj*vec4(pulled,1.0);
    toleratedDepth=c2.z/max(c2.w,1e-6);
    coord=vec2(atB?bS.x:aS.x,q.y);
    color=atB?bColor:aColor;
    widthAlpha=clamp(P.w/drawn,0.25,1.0);
}
