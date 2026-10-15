#version 450
layout(set=0,binding=0) uniform sampler2D sourceImage;
layout(push_constant) uniform Output { uint encodeSrgb; float markerX; float markerY; float markerRadius; float markerOpacity; } outputInfo;
layout(location=0) out vec4 color;
void main() {
    // Pixel-preserving transfer: no UV inversion, filtering or eye reprojection.
    vec3 c = texelFetch(sourceImage, ivec2(gl_FragCoord.xy), 0).rgb;
    c = mix(clamp(c, vec3(0.0), vec3(65504.0)), vec3(0.0), isnan(c));
    // Explicit temporary preview curve; no shared exposure/history or second tick.
    c = c / (vec3(1.0) + c);
    if (outputInfo.markerRadius > 0.0) {
        float d = length(gl_FragCoord.xy - vec2(outputInfo.markerX, outputInfo.markerY));
        float ring = abs(d - outputInfo.markerRadius);
        float border = 1.0 - smoothstep(1.1, 1.9, ring);
        float ink = 1.0 - smoothstep(0.25, 0.85, ring);
        vec3 marked = mix(mix(c, vec3(0.0), border), vec3(1.0), ink);
        // Fade the complete outlined marker in linear light, before the one
        // output sRGB conversion. Scene alpha remains opaque.
        c = mix(c, marked, outputInfo.markerOpacity);
    }
    if (outputInfo.encodeSrgb != 0u)
        c = mix(1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, 12.92 * c,
                lessThanEqual(c, vec3(0.0031308)));
    color = vec4(c, 1.0);
}
