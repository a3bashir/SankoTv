#version 440

layout(location = 0) in vec2 vertexPosition;

layout(std140, binding = 0) uniform Globals {
    mat4 clipMatrix;
    vec4 dualParameters;
};

void main()
{
    gl_Position = clipMatrix * vec4(vertexPosition, 0.0, 1.0);
}
