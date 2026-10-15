#version 450
// AppSW: slot depth at the motion-vector resolution (depth-only pass, fullscreen triangle of
// spacewarp.vert) from the full-resolution eye depth copy (StereoCaptureSlot::recordDepth, sampled).
// Forward Z (near 0, far 1): the farthest (max) of the 2x2 texels around the target pixel centre, as
// Oculus' OculusCameraMotionVectors.shader (min under UNITY_REVERSED_Z). Replaces the second full-size
// D24S8 copy per eye (D24S8 cannot be blitted on Quest 3).
layout(set = 0, binding = 0) uniform sampler2D src;
layout(location = 0) in vec2 uv;
void main() {
    vec4 g = textureGather(src, uv, 0);
    gl_FragDepth = max(max(g.x, g.y), max(g.z, g.w));
}
