#version 440

layout(location = 0) in vec2 vertexPosition;
layout(location = 1) in vec4 centerSizeOpacity;
layout(location = 2) in vec4 shapeParameters;
layout(location = 3) in vec4 canvasGrain;
layout(location = 4) in vec4 grainStyle;
layout(location = 5) in vec4 noiseAttributes; // x=seed lo16, y=seed hi16, z=amount

layout(location = 0) out vec2 localPosition;
layout(location = 1) out vec4 stampParameters;
layout(location = 2) out float stampFlow;
layout(location = 3) out vec2 canvasPosition;
layout(location = 4) out vec4 grainParameters;
layout(location = 5) out vec2 grainFlags;
layout(location = 6) out vec4 noiseData; // xy=seed halves, z=amount, w=half size

layout(std140, binding = 0) uniform Globals {
    mat4 clipMatrix;
    int useCustomTip;
    int accumulateFlow;
    int textureBlendMode;
};

void main()
{
    vec2 pixelPosition = centerSizeOpacity.xy + vertexPosition * centerSizeOpacity.z * 0.5;
    vec2 normalized = pixelPosition / 256.0 * 2.0 - 1.0;
    gl_Position = clipMatrix * vec4(normalized, 0.0, 1.0);
    localPosition = vertexPosition;
    stampParameters = vec4(centerSizeOpacity.w, shapeParameters.xyz);
    stampFlow = shapeParameters.w;
    canvasPosition = grainStyle.z > 0.5
        ? canvasGrain.xy + vertexPosition * centerSizeOpacity.z * 0.5
        : vertexPosition * centerSizeOpacity.z * 0.5;
    grainParameters = vec4(canvasGrain.zw, grainStyle.xy);
    grainFlags = grainStyle.zw;
    // Noise (Phase 6e): the fragment reconstructs its offset from the
    // stamp's raster centre as localPosition * halfSize — exact under
    // linear interpolation — which is the CPU's (x + .5 - rasterCentre)
    // per pixel, the Rolling-grain anchor.
    noiseData = vec4(noiseAttributes.xyz, centerSizeOpacity.z * 0.5);
}
