#version 440

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragmentColor;

layout(binding = 1) uniform sampler2D baseLayer;
layout(binding = 2) uniform sampler2D strokeBuffer;
layout(std140, binding = 0) uniform Globals {
    mat4 clipMatrix;
    vec4 brushColor;
    int colorStrokeBuffer;
    float wetEdges;   // 0 = off; per-stroke rim pooling at THIS boundary
    float wetCeiling; // mask path: brush opacity; colour path: 1.0
    int eraseMode;    // 1: coverage REMOVES alpha (mirrors eraseOut on CPU)
};

float byteRound(float value)
{
    return floor(clamp(value, 0.0, 1.0) * 255.0 + 0.5) / 255.0;
}

// The CPU implementation is wetEdgesAlpha in WetEdges.h — line for line.
// Applied to the ACCUMULATED stroke coverage, once per stroke, before the
// byte quantisation both paths share. out <= alpha always, so the flow
// opacity ceiling cannot be exceeded.
float wetEdgesAlpha(float alpha)
{
    float cRel = clamp(alpha / max(wetCeiling, 0.0001), 0.0, 1.0);
    return alpha * (1.0 - wetEdges * cRel * cRel);
}

void main()
{
    ivec2 framebufferPixel = ivec2(gl_FragCoord.xy);
    vec4 destination = texelFetch(baseLayer,
        ivec2(framebufferPixel.x, 255 - framebufferPixel.y), 0);
    vec4 stroke = texelFetch(strokeBuffer, framebufferPixel, 0);
    float sourceAlpha;
    vec3 sourceColor;
    if (colorStrokeBuffer != 0) {
        float normalizedCoverage = stroke.a;
        // Colour recovery divides by the ORIGINAL coverage (the stored
        // colour is premultiplied by it); only the published ALPHA takes
        // the wet transfer — mirroring ColorStrokeBuffer::composite.
        float published = wetEdges > 0.0
            ? wetEdgesAlpha(normalizedCoverage) : normalizedCoverage;
        sourceAlpha = byteRound(published * brushColor.a);
        // Shader sampling is logical RGBA on every QRhi backend. Native BGRA
        // ordering only applies to CPU staging readback, not this GPU pass.
        vec3 storedColor = stroke.rgb;
        sourceColor = normalizedCoverage > 0.0
            ? clamp(storedColor / normalizedCoverage, 0.0, 1.0) : vec3(0.0);
    } else {
        // Match the CPU path's single UNORM16 -> byte publication boundary
        // (StrokeBuilder::publishMask8: transfer first, then the byte).
        float s = stroke.r;
        if (wetEdges > 0.0)
            s = wetEdgesAlpha(s);
        float maskByte = byteRound(s);
        sourceAlpha = maskByte * brushColor.a;
        sourceColor = brushColor.rgb;
    }
    if (sourceAlpha <= 0.0) {
        fragmentColor = destination;
        return;
    }
    if (eraseMode != 0) {
        // The erase composite - the CPU implementation is eraseOut in
        // PixelCompositor.cpp, line for line: the same published coverage
        // that would deposit colour removes alpha instead, colour channels
        // untouched, sharing the byteRound quantisation boundary.
        fragmentColor = vec4(destination.rgb,
                             byteRound(destination.a * (1.0 - sourceAlpha)));
        return;
    }
    float outputAlpha = sourceAlpha + destination.a * (1.0 - sourceAlpha);
    vec3 outputColor = outputAlpha > 0.0
        ? (sourceColor * sourceAlpha
           + destination.rgb * destination.a * (1.0 - sourceAlpha)) / outputAlpha
        : vec3(0.0);
    fragmentColor = vec4(byteRound(outputColor.r), byteRound(outputColor.g),
                         byteRound(outputColor.b), byteRound(outputAlpha));
}
