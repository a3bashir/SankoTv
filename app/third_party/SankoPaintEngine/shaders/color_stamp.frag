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
};

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
    float coverage = useCustomTip != 0
        ? texture(customTip, rotated * 0.5 + 0.5).r : 1.0;
    if (useCustomTip == 0 && hardness < 0.999 && distanceFromCenter > hardness) {
        float t = (distanceFromCenter - hardness) / max(1.0 - hardness, 0.001);
        coverage = exp(-3.0 * t * t) * (1.0 - t);
    }
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
        coverage *= mix(1.0, grainValue, grainDepth);
    }
    float alpha = coverage * stampFlow * opacity;
    vec3 resolvedColor = grainFlags.y > 0.5
        ? varyingColor * mix(1.0, grainValue, grainDepth) : varyingColor;
    fragmentColor = vec4(resolvedColor * alpha, alpha);
}
