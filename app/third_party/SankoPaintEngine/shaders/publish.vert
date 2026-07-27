#version 440

layout(location = 0) in vec2 vertexPosition;
layout(location = 0) out vec2 uv;

layout(std140, binding = 0) uniform Globals {
    mat4 clipMatrix;
    vec4 brushColor;
    int colorStrokeBuffer;
};

void main()
{
    gl_Position = clipMatrix * vec4(vertexPosition, 0.0, 1.0);
    uv = vertexPosition * 0.5 + 0.5;
}
