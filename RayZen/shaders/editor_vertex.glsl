#version 430 core

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in int inMaterialIndex;
layout(location = 3) in vec2 inUV;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;
uniform mat3 uNormalMatrix;

out VS_OUT {
    vec3 worldPos;
    vec3 normal;
    flat int materialIndex;
    vec2 uv;
} vs_out;

void main() {
    vec4 worldPosition = uModel * vec4(inPosition, 1.0);
    vs_out.worldPos = worldPosition.xyz;
    vs_out.normal = normalize(uNormalMatrix * inNormal);
    vs_out.materialIndex = inMaterialIndex;
    vs_out.uv = inUV;
    gl_Position = uProj * uView * worldPosition;
}
