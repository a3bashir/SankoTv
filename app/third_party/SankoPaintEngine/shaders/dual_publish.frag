#version 440

layout(location = 0) out vec4 fragmentColor;

layout(binding = 1) uniform sampler2D baseLayer;
layout(binding = 2) uniform sampler2D primaryStroke;
layout(binding = 3) uniform sampler2D secondaryStroke;
layout(std140, binding = 0) uniform Globals {
    mat4 clipMatrix;
    vec4 dualParameters; // x=blend mode, y=master opacity, z=dual mode, w=wet edges
    vec4 wetParameters;  // x=wet ceiling
};

float byteRound(float value)
{
    return floor(clamp(value, 0.0, 1.0) * 255.0 + 0.5) / 255.0;
}

// Mirrors WetEdges.h line for line — the ONE implementation discipline.
float wetEdgesAlpha(float alpha, float ceiling, float amount)
{
    float cRel = clamp(ceiling > 0.0001 ? alpha / ceiling : 0.0, 0.0, 1.0);
    return alpha * (1.0 - amount * cRel * cRel);
}

// Mirrors DualBrushCompositor's modulateAlpha: out = f(A.a, B.a), colour is
// A's. Every f is confined (f(0, b) == 0). NormalOver (0) and Screen (4)
// fall back to Multiply — both add coverage outside A, contradicting
// modulation.
float modulateAlpha(float a, float b, int mode)
{
    if (mode == 3) return a * (1.0 - b);
    if (mode == 6) return max(a + b - 1.0, 0.0);
    if (mode == 5) {
        return a <= 0.5 ? 2.0 * a * b
                        : 1.0 - 2.0 * (1.0 - a) * (1.0 - b);
    }
    return a * b;
}

vec3 blendColor(vec3 a, vec3 b, int mode)
{
    if (mode == 1) return a * b;
    if (mode == 3) return max(a - b, vec3(0.0));
    if (mode == 4) return 1.0 - (1.0 - a) * (1.0 - b);
    if (mode == 5) {
        return mix(2.0 * a * b,
                   1.0 - 2.0 * (1.0 - a) * (1.0 - b),
                   step(vec3(0.5), a));
    }
    if (mode == 6) return max(a + b - 1.0, vec3(0.0));
    return b;
}

vec4 combineStrokes(vec4 a, vec4 b, int mode)
{
    if (mode == 2)
        return vec4(a.rgb, a.a * b.a);
    if (mode == 0) {
        float alpha = b.a + a.a * (1.0 - b.a);
        vec3 color = alpha > 0.0
            ? (b.rgb * b.a + a.rgb * a.a * (1.0 - b.a)) / alpha
            : vec3(0.0);
        return vec4(color, alpha);
    }
    float alpha = a.a + b.a - a.a * b.a;
    vec3 overlap = blendColor(a.rgb, b.rgb, mode);
    vec3 premult = a.rgb * a.a * (1.0 - b.a)
        + b.rgb * b.a * (1.0 - a.a) + overlap * a.a * b.a;
    return vec4(alpha > 0.0 ? premult / alpha : vec3(0.0), alpha);
}

void main()
{
    ivec2 framebufferPixel = ivec2(gl_FragCoord.xy);
    ivec2 uploadedPixel = ivec2(framebufferPixel.x, 255 - framebufferPixel.y);
    vec4 destination = texelFetch(baseLayer, uploadedPixel, 0);
    vec4 a = texelFetch(primaryStroke, uploadedPixel, 0);
    vec4 b = texelFetch(secondaryStroke, uploadedPixel, 0);
    int mode = int(dualParameters.x + 0.5);
    vec4 source;
    if (int(dualParameters.z + 0.5) == 1) {
        source = vec4(a.rgb, clamp(modulateAlpha(a.a, b.a, mode), 0.0, 1.0));
    } else {
        source = combineStrokes(a, b, mode);
    }
    // Wet edges on the COMBINED coverage — the dual publication boundary,
    // mirroring the single-brush publish. Only removes paint.
    if (dualParameters.w > 0.0)
        source.a = wetEdgesAlpha(source.a, wetParameters.x, dualParameters.w);
    source.a *= clamp(dualParameters.y, 0.0, 1.0);
    float outputAlpha = source.a + destination.a * (1.0 - source.a);
    vec3 outputColor = outputAlpha > 0.0
        ? (source.rgb * source.a
           + destination.rgb * destination.a * (1.0 - source.a)) / outputAlpha
        : vec3(0.0);
    fragmentColor = vec4(byteRound(outputColor.r), byteRound(outputColor.g),
                         byteRound(outputColor.b), byteRound(outputAlpha));
}
