#version 450
// Based on NVIDIA FXAA 3.11 by Timothy Lottes.
// Copyright (c) 2011 NVIDIA Corporation. All rights reserved.
// Provided AS IS without warranty; see assets/licenses/NVIDIA-FXAA-3.11.txt.
// Post anti-aliasing of one published eye (StereoMailbox producer pass).
// FXAA 3.11 "quality" algorithm (T. Lottes), preset 12 (5 edge-search steps:
// 1.0 1.5 2.0 4.0 12.0), green channel as luma, neighbourhood via 2 gathers.
// Single-sample source, same extent as the target; full-screen triangle.
// Non-edge pixels (early exit) are written bit-exact from the source texel.
// In-headset proof modes (push constant, uniform branch, same pipeline):
//  debugMode 1 EDGES: every pixel that passes the edge test is solid magenta, others unchanged;
//  debugMode 2 SPLIT: left half of the eye FXAA, right half the raw copy, 4 px magenta divider.
layout(constant_id = 0) const int LUMA_MODE = 0;     // 0: display-encoded values (UNORM view)
                                                     // 1: linear LDR (sRGB view decodes) -> sqrt
                                                     // 2: linear HDR (RGBA16F) -> sqrt(x/(1+x)), output sanitized
layout(constant_id = 1) const int OUTPUT_DECODE = 0; // 1: source holds sRGB bytes in a UNORM view, target is an
                                                     //    sRGB attachment -> write linear so it re-encodes once
layout(set = 0, binding = 0) uniform sampler2D sourceImage; // bilinear, clamp to edge
layout(push_constant) uniform Params {
    vec2 rcpFrame;          // 1 / source extent
    float subpix;           // FXAA quality subpix (0.75 default)
    float edgeThreshold;    // 0.166
    float edgeThresholdMin; // 0.0833
    int debugMode;          // 0 FXAA, 1 EDGES, 2 SPLIT (offset 20)
} params;
layout(location = 0) out vec4 outColor;

float lumaOf(float g) {
    if (LUMA_MODE == 1) return sqrt(max(g, 0.0));
    if (LUMA_MODE == 2) { g = isnan(g) ? 0.0 : clamp(g, 0.0, 65504.0); return sqrt(g / (1.0 + g)); }
    return g;
}
vec4 lumaOf(vec4 g) {
    if (LUMA_MODE == 1) return sqrt(max(g, vec4(0.0)));
    if (LUMA_MODE == 2) { g = mix(clamp(g, vec4(0.0), vec4(65504.0)), vec4(0.0), isnan(g)); return sqrt(g / (vec4(1.0) + g)); }
    return g;
}
float lumaAt(vec2 p) { return lumaOf(textureLod(sourceImage, p, 0.0).g); }
vec4 finish(vec4 c) {
    if (LUMA_MODE == 2) c.rgb = mix(clamp(c.rgb, vec3(0.0), vec3(65504.0)), vec3(0.0), isnan(c.rgb));
    if (OUTPUT_DECODE == 1) {
        vec3 s = clamp(c.rgb, vec3(0.0), vec3(1.0));
        c.rgb = mix(pow((s + 0.055) / 1.055, vec3(2.4)), s / 12.92, lessThanEqual(s, vec3(0.04045)));
    }
    return c;
}

void main() {
    vec2 rcp = params.rcpFrame;
    vec2 posM = gl_FragCoord.xy * rcp;                     // pixel centre
    vec4 rgbyM = texelFetch(sourceImage, ivec2(gl_FragCoord.xy), 0);
    // SPLIT: the divider (|x - W/2| < 2, drawn whatever the pass finds) proves the image
    // is the FXAA target; the right half is the raw copy to compare against.
    // finish() keeps magenta for every target (sRGB/UNORM encode, OUTPUT_DECODE, HDR).
    if (params.debugMode == 2) {
        float mid = 0.5 / rcp.x;
        if (abs(gl_FragCoord.x - mid) < 2.0) { outColor = finish(vec4(1.0, 0.0, 1.0, rgbyM.a)); return; }
        if (gl_FragCoord.x > mid) { outColor = finish(rgbyM); return; }
    }
    // Gathers centred on texel CORNERS select their 2x2 footprint robustly
    // (a gather at a texel centre depends on coordinate rounding).
    // Gather components: x=(u0,v1) y=(u1,v1) z=(u1,v0) w=(u0,v0).
    vec4 luma4A = lumaOf(textureGather(sourceImage, posM + 0.5 * rcp, 1)); // S SE E M
    vec4 luma4B = lumaOf(textureGather(sourceImage, posM - 0.5 * rcp, 1)); // W M N NW
    float lumaM = luma4A.w;
    float lumaE = luma4A.z;
    float lumaS = luma4A.x;
    float lumaSE = luma4A.y;
    float lumaNW = luma4B.w;
    float lumaN = luma4B.z;
    float lumaW = luma4B.x;

    float maxSM = max(lumaS, lumaM);
    float minSM = min(lumaS, lumaM);
    float maxESM = max(lumaE, maxSM);
    float minESM = min(lumaE, minSM);
    float maxWN = max(lumaN, lumaW);
    float minWN = min(lumaN, lumaW);
    float rangeMax = max(maxWN, maxESM);
    float rangeMin = min(minWN, minESM);
    float rangeMaxScaled = rangeMax * params.edgeThreshold;
    float range = rangeMax - rangeMin;
    float rangeMaxClamped = max(params.edgeThresholdMin, rangeMaxScaled);
    if (range < rangeMaxClamped) { outColor = finish(rgbyM); return; }
    // EDGES: every pixel the edge search would process.
    if (params.debugMode == 1) { outColor = finish(vec4(1.0, 0.0, 1.0, rgbyM.a)); return; }

    float lumaNE = lumaOf(textureLodOffset(sourceImage, posM, 0.0, ivec2( 1, -1)).g);
    float lumaSW = lumaOf(textureLodOffset(sourceImage, posM, 0.0, ivec2(-1,  1)).g);
    float lumaNS = lumaN + lumaS;
    float lumaWE = lumaW + lumaE;
    float subpixRcpRange = 1.0 / range;
    float subpixNSWE = lumaNS + lumaWE;
    float edgeHorz1 = (-2.0 * lumaM) + lumaNS;
    float edgeVert1 = (-2.0 * lumaM) + lumaWE;
    float lumaNESE = lumaNE + lumaSE;
    float lumaNWNE = lumaNW + lumaNE;
    float edgeHorz2 = (-2.0 * lumaE) + lumaNESE;
    float edgeVert2 = (-2.0 * lumaN) + lumaNWNE;
    float lumaNWSW = lumaNW + lumaSW;
    float lumaSWSE = lumaSW + lumaSE;
    float edgeHorz4 = (abs(edgeHorz1) * 2.0) + abs(edgeHorz2);
    float edgeVert4 = (abs(edgeVert1) * 2.0) + abs(edgeVert2);
    float edgeHorz3 = (-2.0 * lumaW) + lumaNWSW;
    float edgeVert3 = (-2.0 * lumaS) + lumaSWSE;
    float edgeHorz = abs(edgeHorz3) + edgeHorz4;
    float edgeVert = abs(edgeVert3) + edgeVert4;
    float subpixNWSWNESE = lumaNWSW + lumaNESE;
    float lengthSign = rcp.x;
    bool horzSpan = edgeHorz >= edgeVert;
    float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;
    if (!horzSpan) lumaN = lumaW;
    if (!horzSpan) lumaS = lumaE;
    if (horzSpan) lengthSign = rcp.y;
    float subpixB = (subpixA * (1.0 / 12.0)) - lumaM;
    float gradientN = lumaN - lumaM;
    float gradientS = lumaS - lumaM;
    float lumaNN = lumaN + lumaM;
    float lumaSS = lumaS + lumaM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN) lengthSign = -lengthSign;
    float subpixC = clamp(abs(subpixB) * subpixRcpRange, 0.0, 1.0);

    vec2 posB = posM;
    vec2 offNP = vec2(horzSpan ? rcp.x : 0.0, horzSpan ? 0.0 : rcp.y);
    if (!horzSpan) posB.x += lengthSign * 0.5;
    if (horzSpan) posB.y += lengthSign * 0.5;

    vec2 posN = posB - offNP;          // step 0: 1.0
    vec2 posP = posB + offNP;
    float subpixD = (-2.0 * subpixC) + 3.0;
    float lumaEndN = lumaAt(posN);
    float subpixE = subpixC * subpixC;
    float lumaEndP = lumaAt(posP);
    if (!pairN) lumaNN = lumaSS;
    float gradientScaled = gradient * (1.0 / 4.0);
    float lumaMM = lumaM - lumaNN * 0.5;
    float subpixF = subpixD * subpixE;
    bool lumaMLTZero = lumaMM < 0.0;
    lumaEndN -= lumaNN * 0.5;
    lumaEndP -= lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled;
    bool doneP = abs(lumaEndP) >= gradientScaled;
    if (!doneN) posN -= offNP * 1.5;   // step 1: 1.5
    bool doneNP = (!doneN) || (!doneP);
    if (!doneP) posP += offNP * 1.5;
    const float steps[3] = float[3](2.0, 4.0, 12.0); // steps 2..4
    for (int i = 0; i < 3 && doneNP; ++i) {
        if (!doneN) lumaEndN = lumaAt(posN) - lumaNN * 0.5;
        if (!doneP) lumaEndP = lumaAt(posP) - lumaNN * 0.5;
        doneN = abs(lumaEndN) >= gradientScaled;
        doneP = abs(lumaEndP) >= gradientScaled;
        if (!doneN) posN -= offNP * steps[i];
        doneNP = (!doneN) || (!doneP);
        if (!doneP) posP += offNP * steps[i];
    }

    float dstN = horzSpan ? posM.x - posN.x : posM.y - posN.y;
    float dstP = horzSpan ? posP.x - posM.x : posP.y - posM.y;
    bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;
    float spanLength = dstP + dstN;
    bool goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
    float spanLengthRcp = 1.0 / spanLength;
    bool directionN = dstN < dstP;
    float dst = min(dstN, dstP);
    bool goodSpan = directionN ? goodSpanN : goodSpanP;
    float subpixG = subpixF * subpixF;
    float pixelOffset = (dst * (-spanLengthRcp)) + 0.5;
    float subpixH = subpixG * params.subpix;
    float pixelOffsetGood = goodSpan ? pixelOffset : 0.0;
    float pixelOffsetSubpix = max(pixelOffsetGood, subpixH);
    if (!horzSpan) posM.x += pixelOffsetSubpix * lengthSign;
    if (horzSpan) posM.y += pixelOffsetSubpix * lengthSign;
    outColor = finish(vec4(textureLod(sourceImage, posM, 0.0).rgb, rgbyM.a));
}
