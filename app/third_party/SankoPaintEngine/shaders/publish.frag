#version 440

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragmentColor;

layout(binding = 1) uniform sampler2D baseLayer;
layout(binding = 2) uniform sampler2D strokeBuffer;
layout(std140, binding = 0) uniform Globals {
    mat4 clipMatrix;
    vec4 brushColor;
    int colorStrokeBuffer;
};

float byteRound(float value)
{
    return floor(clamp(value, 0.0, 1.0) * 255.0 + 0.5) / 255.0;
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
        sourceAlpha = byteRound(normalizedCoverage * brushColor.a);
        // Shader sampling is logical RGBA on every QRhi backend. Native BGRA
        // ordering only applies to CPU staging readback, not this GPU pass.
        vec3 storedColor = stroke.rgb;
        sourceColor = normalizedCoverage > 0.0
            ? clamp(storedColor / normalizedCoverage, 0.0, 1.0) : vec3(0.0);
    } else {
        // Match the CPU path's single UNORM16 -> byte publication boundary.
        float maskByte = byteRound(stroke.r);
        sourceAlpha = maskByte * brushColor.a;
        sourceColor = brushColor.rgb;
    }
    if (sourceAlpha <= 0.0) {
        fragmentColor = destination;
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
