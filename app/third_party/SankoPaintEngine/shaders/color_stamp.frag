#version 440

layout(location = 0) in vec2 localPosition;
layout(location = 1) in vec4 stampParameters;
layout(location = 2) in float stampFlow;
layout(location = 3) in vec3 varyingColor;
layout(location = 4) in vec2 canvasPosition;
layout(location = 5) in vec4 grainParameters;
layout(location = 6) in vec2 grainFlags;
layout(location = 0) out vec4 fragmentColor;
layout(binding = 1) uniform sampler2D customTip;
layout(binding = 2) uniform sampler2D grainTexture;

layout(std140, binding = 0) uniform Globals {
    mat4 clipMatrix;
    int useCustomTip;
};

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
