#version 440

layout(location = 0) in vec2 localPosition;
layout(location = 1) in vec4 stampParameters;
layout(location = 2) in float stampFlow;
layout(location = 3) in vec2 canvasPosition;
layout(location = 4) in vec4 grainParameters;
layout(location = 5) in vec2 grainFlags;
layout(location = 6) in vec4 noiseData; // xy=seed halves, z=amount, w=half size
layout(location = 0) out vec4 fragmentColor;

layout(binding = 1) uniform sampler2D customTip;
layout(binding = 2) uniform sampler2D grainTexture;
layout(std140, binding = 0) uniform Globals {
    mat4 clipMatrix;
    int useCustomTip;
    int accumulateFlow;
    int textureBlendMode;
    // Custom-tip extent (Brush::customTipExtent), offset 80: the image
    // occupies |rotated| <= tipExtent, longer axis = 1. (1,1) for square
    // tips, where the divide is exact and the clip can never fire.
    vec2 tipExtent;
};

// Mirrors TextureBlend.h line for line — the ONE implementation
// discipline. Enum order: 0 Multiply (never routed here — the grain block
// keeps the original expression verbatim), 1 Subtract, 2 Darken,
// 3 Overlay, 4 ColorBurn, 5 LinearBurn, 6 HardMix, 7 Height,
// 8 LinearHeight. Confidence per mode is graded in TextureBlend.h:
// Multiply EXACT, Height/LinearHeight PS-MATCHED via Krita's
// *_PHOTOSHOP ops, the rest CONSTRUCTED under mix(c, f(c,t), d).
// Exact-sampling helpers for the non-Multiply branch. The blend formulas
// amplify coverage disagreement by up to (1 + 9*depth) — Height's own
// gain — so the two renderers must SAMPLE IDENTICALLY, not merely
// closely: manual bilinear with float weights replicates GrainField's
// arithmetic (the sampler's fixed-point weights do not), and round8
// lands the tip on the same 8-bit grid StrokeBuilder bakes tip rasters
// to. The Multiply path keeps its samplers untouched.
float round8(float v)
{
    return floor(clamp(v, 0.0, 1.0) * 255.0 + 0.5) / 255.0;
}

float bilinearClampR(sampler2D tex, vec2 uv)
{
    ivec2 sz = textureSize(tex, 0);
    vec2 xy = uv * vec2(sz) - 0.5;
    ivec2 i0 = ivec2(floor(xy));
    vec2 w = xy - vec2(i0);
    ivec2 i1 = clamp(i0 + 1, ivec2(0), sz - 1);
    i0 = clamp(i0, ivec2(0), sz - 1);
    float v00 = texelFetch(tex, ivec2(i0.x, i0.y), 0).r;
    float v10 = texelFetch(tex, ivec2(i1.x, i0.y), 0).r;
    float v01 = texelFetch(tex, ivec2(i0.x, i1.y), 0).r;
    float v11 = texelFetch(tex, ivec2(i1.x, i1.y), 0).r;
    return mix(mix(v00, v10, w.x), mix(v01, v11, w.x), w.y);
}

// GrainField::wrapped, verbatim: repeat addressing, texel centres (i+.5).
float bilinearRepeatR(sampler2D tex, vec2 uv)
{
    ivec2 sz = textureSize(tex, 0);
    vec2 f = uv - floor(uv);
    vec2 xy = f * vec2(sz) - 0.5;
    ivec2 i0 = ivec2(floor(xy));
    vec2 w = xy - vec2(i0);
    ivec2 i1 = i0 + 1;
    i0 = (i0 % sz + sz) % sz;
    i1 = (i1 % sz + sz) % sz;
    float v00 = texelFetch(tex, ivec2(i0.x, i0.y), 0).r;
    float v10 = texelFetch(tex, ivec2(i1.x, i0.y), 0).r;
    float v01 = texelFetch(tex, ivec2(i0.x, i1.y), 0).r;
    float v11 = texelFetch(tex, ivec2(i1.x, i1.y), 0).r;
    return mix(mix(v00, v10, w.x), mix(v01, v11, w.x), w.y);
}

float textureBlendCoverage(int mode, float c, float t, float d)
{
    if (mode == 7)
        return clamp(c * (1.0 + 9.0 * d) - d * t, 0.0, 1.0);
    if (mode == 8) {
        float m = c * (1.0 + 9.0 * d);
        return clamp(max(m * (1.0 - d * t), m - d * t), 0.0, 1.0);
    }
    float f = c;
    if (mode == 1) f = max(0.0, c - (1.0 - t));
    else if (mode == 2) f = min(c, t);
    else if (mode == 3) f = c <= 0.5 ? 2.0 * c * t
                                     : 1.0 - 2.0 * (1.0 - c) * (1.0 - t);
    else if (mode == 4) f = t > 0.0 ? 1.0 - min(1.0, (1.0 - c) / t) : 0.0;
    else if (mode == 5) f = max(0.0, c + t - 1.0);
    else if (mode == 6) f = c + t >= 1.0 ? 1.0 : 0.0;
    return clamp(c * (1.0 - d) + f * d, 0.0, 1.0);
}

// Mirrors NoiseField.h line for line — the ONE implementation discipline.
// 32-bit unsigned wrap arithmetic, identical in GLSL uint and C++ quint32;
// the unit keeps 24 bits so float32 computes the exact CPU value.
uint noiseHash(uint seed, int ix, int iy)
{
    uint v = seed ^ (uint(ix) * 0x85ebca6bu) ^ (uint(iy) * 0xc2b2ae35u);
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return v;
}

float noisyCoverage(float coverage)
{
    float w = 2.0 * min(coverage, 1.0 - coverage);
    if (w <= 0.0)
        return coverage;
    ivec2 p = ivec2(floor(localPosition * noiseData.w + 0.25));
    uint seed = uint(noiseData.x) | (uint(noiseData.y) << 16);
    float n = float(noiseHash(seed, p.x, p.y) >> 8)
        * (1.0 / 16777216.0) * 2.0 - 1.0;
    return clamp(coverage + noiseData.z * n * w, 0.0, 1.0);
}

void main()
{
    float opacity = stampParameters.x;
    float angle = stampParameters.y;
    float compression = max(stampParameters.z, 0.001);
    float hardness = stampParameters.w;
    float c = cos(angle), s = sin(angle);
    vec2 rotated = vec2(c * localPosition.x + s * localPosition.y,
                       -s * localPosition.x + c * localPosition.y);
    rotated.x /= compression;
    float distanceFromCenter = length(rotated);
    if (distanceFromCenter >= 1.0)
        discard;
    float coverage;
    float rawTip; // pre-noise tip coverage, for the exact-sampling branch
    if (useCustomTip != 0) {
        // Mirrors StrokeBuilder::shapedTipForStamp's rectangle clip and
        // extent divide, same expression order.
        if (abs(rotated.x) > tipExtent.x || abs(rotated.y) > tipExtent.y)
            discard;
        coverage = texture(customTip, rotated / tipExtent * 0.5 + 0.5).r;
        rawTip = coverage;
    } else if (hardness >= 0.999 || distanceFromCenter <= hardness) {
        coverage = 1.0;
        rawTip = 1.0;
    } else {
        float t = (distanceFromCenter - hardness) / max(1.0 - hardness, 0.001);
        coverage = exp(-3.0 * t * t) * (1.0 - t);
        rawTip = coverage;
    }
    // Noise perturbs the TIP's coverage BEFORE grain multiplies it —
    // matching StrokeBuilder::placeStamp's order.
    if (noiseData.z > 0.0)
        coverage = noisyCoverage(coverage);
    float grainDepth = grainParameters.x;
    if (grainDepth > 0.0) {
        float grainScale = max(grainParameters.y, 1.0);
        float gc = cos(grainParameters.z), gs = sin(grainParameters.z);
        vec2 grainUv = vec2(gc * canvasPosition.x + gs * canvasPosition.y,
                           -gs * canvasPosition.x + gc * canvasPosition.y) / grainScale;
        if (textureBlendMode == 0) {
            float grain = texture(grainTexture, grainUv).r;
            grain = clamp((grain - 0.5) * grainParameters.w + 0.5, 0.0, 1.0);
            coverage *= mix(1.0, grain, grainDepth); // Multiply, verbatim
        } else {
            // Exact-sampling chain (see the helpers above): tip re-sampled
            // with float-weight bilinear onto the CPU's 8-bit bake grid,
            // noise re-applied (exact by construction), grain sampled with
            // GrainField::wrapped's arithmetic. This is what holds the
            // gain-amplified modes inside the CPU/GPU tolerance.
            float cb = useCustomTip != 0
                ? round8(bilinearClampR(customTip,
                                        rotated / tipExtent * 0.5 + 0.5))
                : round8(rawTip);
            if (noiseData.z > 0.0)
                cb = noisyCoverage(cb);
            float t = bilinearRepeatR(grainTexture, grainUv);
            t = clamp((t - 0.5) * grainParameters.w + 0.5, 0.0, 1.0);
            coverage = textureBlendCoverage(textureBlendMode, cb, t,
                                            grainDepth);
        }
    }
    if (accumulateFlow != 0) {
        float deposit = coverage * stampFlow;
        fragmentColor = vec4(opacity * deposit, opacity * deposit,
                             opacity * deposit, deposit);
    } else {
        float alpha = coverage * opacity;
        fragmentColor = vec4(alpha, alpha, alpha, alpha);
    }
}
