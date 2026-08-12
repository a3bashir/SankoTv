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
};

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
    if (useCustomTip != 0) {
        coverage = texture(customTip, rotated * 0.5 + 0.5).r;
    } else if (hardness >= 0.999 || distanceFromCenter <= hardness) {
        coverage = 1.0;
    } else {
        float t = (distanceFromCenter - hardness) / max(1.0 - hardness, 0.001);
        coverage = exp(-3.0 * t * t) * (1.0 - t);
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
        float grain = texture(grainTexture, grainUv).r;
        grain = clamp((grain - 0.5) * grainParameters.w + 0.5, 0.0, 1.0);
        coverage *= mix(1.0, grain, grainDepth);
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
