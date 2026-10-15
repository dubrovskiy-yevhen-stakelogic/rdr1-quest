#version 450
layout(set=0,binding=0) uniform sampler2D sourceImage;
layout(location=0) out vec4 color;
void main() {
    // Native capture already accumulated premultiplied color. Preserve alpha,
    // pixel coordinates and source encoding; no tone map or second multiply.
    color = texelFetch(sourceImage, ivec2(gl_FragCoord.xy), 0);
}
