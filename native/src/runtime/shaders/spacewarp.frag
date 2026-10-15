#version 450
// AppSW: camera-only motion vectors for XR_FB_space_warp.
// depthTex = this eye's depth (engine forward Z in [0,1]) at motion-vector resolution.
// reproject = relClipPrev * T(cCur - cPrev) * inverse(relClipCur) (engine NDC -> previous clip).
// params: x = engine NDC y sign relative to image-down (-1: engine y up), y = output y sign
// (+1 Vulkan NDC, -1 GL NDC), z = motion sign (+1 current - previous), w = z scale (0/1/2).
// Output (r,g,b) = NDC delta, a = 0; depth = the sampled depth (forward Z, minDepth 0, maxDepth 1).
// Per-actor rigid motion (runtime/SpaceWarpActors.h). A pixel whose position (engine NDC -> toLocal = offset from
// the actor's root, world axes, y up) lies inside the actor's upright cylinder uses the actor's reprojection
// (toPrev = relClipPrev * T(-cPrev) * Mprev * inverse(Mcur) * T(cCur) * inverse(relClipCur)) instead of the camera one.
// Bone parts (runtime/SpaceWarpBones.h): an actor may carry parts (bones.x = first, bones.y = count): inside its bounding cylinder
// (shape) the part whose capsule (segment a..b in the toLocal frame, radius^2 = a.w, 1/|b-a|^2 = b.w) contains the point with the
// NEAREST axis gives the reprojection (Gprev * inverse(Gcur) of that bone); no part -> the root cylinder (bones.z = r^2, bones.w = top).
// 22 actor blocks per eye: the block stays within 16384 B (Vulkan minimum maxUniformBufferRange).
// A part of ANY actor beats every root cylinder (blocks with parts come first, buildActorBlock); among
// root cylinders the lowest rank (shape.w = index in the actor set, nearest first) wins; the first
// root-only cylinder containing the pixel ends the scan.
layout(set = 0, binding = 0) uniform sampler2D depthTex;
struct Actor { mat4 toLocal; mat4 toPrev; vec4 rect; vec4 shape; vec4 bones; };
struct Bone { vec4 a; vec4 b; mat4 toPrev; };
layout(std140, set = 0, binding = 1) uniform Actors { vec4 header; Actor actor[22]; Bone bone[128]; } act;
layout(push_constant) uniform Push { mat4 reproject; vec4 params; } pc;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 motion;
void main() {
    float d = textureLod(depthTex, uv, 0.0).r;
    vec2 img = uv * 2.0 - 1.0;
    vec4 cur = vec4(img.x, img.y * pc.params.x, d, 1.0);
    vec4 prev = pc.reproject * cur;
    int n = d < 1.0 ? min(int(act.header.x), 22) : 0;
    int root = -1;
    float rank = 1e30;
    for (int i = 0; i < n; ++i) {
        vec4 r = act.actor[i].rect;
        if (cur.x < r.x || cur.y < r.y || cur.x > r.z || cur.y > r.w) continue;
        vec4 l = act.actor[i].toLocal * cur;
        if (l.w <= 1e-9) continue;
        vec3 p = l.xyz / l.w;
        vec4 s = act.actor[i].shape;
        if (p.y < s.y || p.y > s.z || dot(p.xz, p.xz) > s.x) continue;
        vec4 bi = act.actor[i].bones;
        int first = clamp(int(bi.x), 0, 128);
        int cnt = clamp(int(bi.y), 0, 128 - first);
        int pick = -1;
        float best = 1e30;
        for (int k = 0; k < cnt; ++k) {
            vec4 ba = act.bone[first + k].a;
            vec4 bb = act.bone[first + k].b;
            vec3 ab = bb.xyz - ba.xyz;
            vec3 ap = p - ba.xyz;
            float t = clamp(dot(ap, ab) * bb.w, 0.0, 1.0);
            vec3 q = ap - ab * t;
            float d2 = dot(q, q);
            if (d2 <= ba.w && d2 < best) { best = d2; pick = k; }
        }
        if (pick >= 0) { prev = act.bone[first + pick].toPrev * cur; root = -1; break; }
        if (p.y > bi.w || dot(p.xz, p.xz) > bi.z) continue;
        if (s.w < rank) { rank = s.w; root = i; }
        if (cnt == 0) break;
    }
    if (root >= 0) prev = act.actor[root].toPrev * cur;
    vec3 delta = vec3(0.0);
    if (prev.w > 1e-6) {
        delta = cur.xyz - prev.xyz / prev.w;
        delta.y *= pc.params.x;
        delta *= pc.params.z;
        delta.y *= pc.params.y;
        delta.z *= pc.params.w;
    }
    motion = vec4(delta, 0.0);
    gl_FragDepth = d;
}
