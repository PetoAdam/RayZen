#version 430 core

struct Material {
    vec3 albedo;
    float metallic;
    float roughness;
    float reflectivity;
    float transparency;
    float ior;
    float subsurfaceMeanFreePath;
    int baseColorTexture;
};

struct Light {
    vec4 positionOrDirection; // w == 1.0 point light, w == 0.0 directional
    vec3 color;
    float power;
};

layout(std430, binding = 1) buffer MaterialBuffer {
    Material materials[];
};

layout(std430, binding = 2) buffer LightBuffer {
    Light lights[];
};

uniform vec3 uCameraPos;
uniform vec3 uAmbientColor;
uniform int numLights;
layout(binding = 1) uniform sampler2DArray materialTextures;

in VS_OUT {
    vec3 worldPos;
    vec3 normal;
    flat int materialIndex;
    vec2 uv;
} fs_in;

out vec4 FragColor;

const float PI = 3.14159265359;
const float MIN_ROUGHNESS = 0.05;
#include "pbr.glsl"

void main() {
    int matIndex = clamp(fs_in.materialIndex, 0, materials.length() - 1);
    Material material = materials[matIndex];
    if (material.baseColorTexture >= 0 && fs_in.uv.x > -1e20)
        material.albedo *= texture(materialTextures, vec3(fs_in.uv, float(material.baseColorTexture))).rgb;

    vec3 N = normalize(fs_in.normal);
    vec3 V = normalize(uCameraPos - fs_in.worldPos);
    vec3 color = uAmbientColor * material.albedo;

    for (int i = 0; i < numLights; ++i) {
        if (i >= lights.length()) break;
        Light light = lights[i];

        vec3 L;
        float attenuation;
        if (light.positionOrDirection.w == 1.0) {
            vec3 lightVec = light.positionOrDirection.xyz - fs_in.worldPos;
            float distance = max(length(lightVec), 0.001);
            L = lightVec / distance;
            attenuation = light.power / (distance * distance);
        } else {
            L = normalize(light.positionOrDirection.xyz);
            attenuation = light.power;
        }

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0) continue;

        color += opaqueBRDF(material, N, V, L) * light.color * attenuation * NdotL;
    }

    if (material.transparency > 0.0) {
        color = mix(color, material.albedo, clamp(material.transparency, 0.0, 1.0) * 0.5);
    }

    FragColor = vec4(color, 1.0);
}
