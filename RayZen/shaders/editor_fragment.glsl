#version 430 core

struct Material {
    vec3 albedo;
    float metallic;
    float roughness;
    float reflectivity;
    float transparency;
    float ior;
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

in VS_OUT {
    vec3 worldPos;
    vec3 normal;
    flat int materialIndex;
} fs_in;

out vec4 FragColor;

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

float distributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159 * denom * denom, 1e-4);
}

float geometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k + 1e-6);
}

void main() {
    int matIndex = clamp(fs_in.materialIndex, 0, materials.length() - 1);
    Material material = materials[matIndex];

    vec3 N = normalize(fs_in.normal);
    vec3 V = normalize(uCameraPos - fs_in.worldPos);
    float NdotV = max(dot(N, V), 0.0);

    vec3 F0 = mix(vec3(0.04), material.albedo, material.metallic);
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

        vec3 H = normalize(V + L);
        float NdotH = max(dot(N, H), 0.0);
        float VdotH = max(dot(V, H), 0.0);

        float rough = clamp(material.roughness, 0.05, 1.0);
        float D = distributionGGX(NdotH, rough);
        float G = geometrySchlickGGX(NdotV, rough) * geometrySchlickGGX(NdotL, rough);
        vec3 F = fresnelSchlick(VdotH, F0);

        vec3 numerator = D * G * F;
        float denominator = max(4.0 * NdotV * NdotL, 1e-4);
        vec3 specular = numerator / denominator;

        vec3 kS = F;
        vec3 kD = (1.0 - kS) * (1.0 - material.metallic);
        vec3 diffuse = kD * material.albedo / 3.14159;

        color += (diffuse + specular) * light.color * attenuation * NdotL;
    }

    if (material.transparency > 0.0) {
        color = mix(color, material.albedo, clamp(material.transparency, 0.0, 1.0) * 0.5);
    }

    FragColor = vec4(color, 1.0);
}
