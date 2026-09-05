#version 440

layout(location = 0) in vec2 localPosition;
layout(location = 1) in vec4 stampParameters;
layout(location = 2) in float stampFlow;
layout(location = 3) in vec3 varyingColor;
layout(location = 4) in vec2 canvasPosition;
layout(location = 5) in vec4 grainParameters;
layout(location = 6) in vec2 grainFlags;
layout(location = 7) in vec4 noiseData; // xy=seed halves, z=amount, w=half size
layout(location = 0) out vec4 fragmentColor;
layout(binding = 1) uniform sampler2D customTip;
layout(binding = 2) uniform sampler2D grainTexture;

layout(std140, binding = 0) uniform Globals {
    mat4 clipMatrix;
    int useCustomTip;
    int textureBlendMode;
    // Custom-tip extent (Brush::customTipExtent), std140 offset 72 here
    // (two ints precede it) - see stamp.frag for the contract.
    vec2 tipExtent;
};

// Exact-sampling helpers — see stamp.frag; mirrored verbatim.
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

// Mirrors TextureBlend.h line for line (and stamp.frag, verbatim) — see
// there for the enum order and per-mode confidence grading.
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

// Mirrors NoiseField.h line for line — the ONE implementation discipline
// (and stamp.frag, verbatim).
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
    // Rectangle clip + extent divide, mirrored from stamp.frag.
    if (useCustomTip != 0
        && (abs(rotated.x) > tipExtent.x || abs(rotated.y) > tipExtent.y))
        discard;
    float coverage = useCustomTip != 0
        ? texture(customTip, rotated / tipExtent * 0.5 + 0.5).r : 1.0;
    if (useCustomTip == 0 && hardness < 0.999 && distanceFromCenter > hardness) {
        float t = (distanceFromCenter - hardness) / max(1.0 - hardness, 0.001);
        coverage = exp(-3.0 * t * t) * (1.0 - t);
    }
    float rawTip = coverage; // pre-noise, for the exact-sampling branch
    // Noise perturbs the TIP's coverage BEFORE grain — see stamp.frag.
    if (noiseData.z > 0.0)
        coverage = noisyCoverage(coverage);
    float grainValue = 1.0;
    float grainDepth = grainParameters.x;
    if (grainDepth > 0.0) {
        float grainScale = max(grainParameters.y, 1.0);
        float gc = cos(grainParameters.z), gs = sin(grainParameters.z);
        vec2 grainUv = vec2(gc * canvasPosition.x + gs * canvasPosition.y,
                           -gs * canvasPosition.x + gc * canvasPosition.y) / grainScale;
        grainValue = texture(grainTexture, grainUv).r;
        grainValue = clamp((grainValue - 0.5) * grainParameters.w + 0.5, 0.0, 1.0);
        if (textureBlendMode == 0) {
            coverage *= mix(1.0, grainValue, grainDepth); // Multiply, verbatim
        } else {
            // Exact-sampling chain — see stamp.frag.
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
        // grainAffectsColor below keeps the multiply-shaped factor under
        // every mode — a tint, not coverage arithmetic (TextureBlend.h).
    }
    float alpha = coverage * stampFlow * opacity;
    vec3 resolvedColor = grainFlags.y > 0.5
        ? varyingColor * mix(1.0, grainValue, grainDepth) : varyingColor;
    fragmentColor = vec4(resolvedColor * alpha, alpha);
}
