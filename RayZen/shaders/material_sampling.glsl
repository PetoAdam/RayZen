#include "pbr.glsl"

void applyBaseColorTexture(inout Material material, vec2 uv) {
    if (material.baseColorTexture >= 0 && uv.x > -1e20)
        material.albedo *= textureLod(materialTextures, vec3(uv, float(material.baseColorTexture)), 0.0).rgb;
}

float dielectricFresnel(float cosIncident, float etaIncident, float etaTransmitted) {
    if (abs(etaIncident - etaTransmitted) < 1e-6) return 0.0;
    float sinTransmitted2 = pow(etaIncident / etaTransmitted, 2.0) *
                            max(0.0, 1.0 - cosIncident * cosIncident);
    if (sinTransmitted2 >= 1.0) return 1.0;
    float cosTransmitted = sqrt(1.0 - sinTransmitted2);
    float rs = (etaIncident * cosIncident - etaTransmitted * cosTransmitted) /
               (etaIncident * cosIncident + etaTransmitted * cosTransmitted);
    float rp = (etaTransmitted * cosIncident - etaIncident * cosTransmitted) /
               (etaTransmitted * cosIncident + etaIncident * cosTransmitted);
    return 0.5 * (rs * rs + rp * rp);
}

bool sampleDielectric(vec3 incident, vec3 normal, float etaIncident, float etaTransmitted,
                      float randomValue, out vec3 direction) {
    float cosine = clamp(dot(-incident, normal), 0.0, 1.0);
    float fresnel = dielectricFresnel(cosine, etaIncident, etaTransmitted);
    vec3 transmitted = refract(incident, normal, etaIncident / etaTransmitted);
    if (fresnel >= 1.0 || randomValue < fresnel || dot(transmitted, transmitted) < 1e-8) {
        direction = reflect(incident, normal);
        return false;
    }
    direction = normalize(transmitted);
    return true;
}

bool shadowVisibility(vec3 origin, vec3 direction, float maxDistance, bool blockGlass, out vec3 visibility) {
    visibility = vec3(1.0);
    if (maxDistance <= 0.0) return true;
    vec3 start = origin;
    float previousDistance = 0.0;
    int mediumInstances[8];
    vec3 mediumTransmittance[8];
    int mediumCount = 0;
    for (int iter = 0; iter <= 32; ++iter) {
        float tHit;
        vec3 hitPoint, normal;
        int materialIndex, instance;
        vec2 uv;
        bool hit = traverseTLAS(Ray(origin, direction), tHit, hitPoint, normal, materialIndex, instance, uv);
        float distance = hit ? dot(hitPoint - start, direction) : maxDistance;
        float endDistance = min(max(distance, previousDistance), maxDistance);
        for (int i = 0; i < mediumCount; ++i)
            visibility *= pow(mediumTransmittance[i], vec3(endDistance - previousDistance));
        if (!hit || distance >= maxDistance) {
            if (hit && dot(direction, normal) > 0.0) {
                bool tracked = false;
                for (int i = 0; i < mediumCount; ++i)
                    tracked = tracked || mediumInstances[i] == instance;
                Material enclosing = materials[materialIndex];
                if (!tracked && enclosing.transparency > 0.0) {
                    applyBaseColorTexture(enclosing, uv);
                    vec3 transmittance = clamp(enclosing.albedo * enclosing.transparency,
                                               vec3(0.0001), vec3(1.0));
                    visibility *= pow(transmittance, vec3(maxDistance));
                }
            }
            return true;
        }
        if (iter == 32) {
            visibility = vec3(0.0);
            return false;
        }

        Material material = materials[materialIndex];
        if (material.transparency <= 0.0 || blockGlass) {
            visibility = vec3(0.0);
            return false;
        }
        applyBaseColorTexture(material, uv);
        vec3 transmittance = clamp(material.albedo * material.transparency, vec3(0.0001), vec3(1.0));
        bool entering = dot(direction, normal) < 0.0;
        int mediumIndex = -1;
        if (!entering) {
            for (int i = 0; i < mediumCount; ++i)
                if (mediumInstances[i] == instance) mediumIndex = i;
            if (mediumIndex < 0)
                visibility *= pow(transmittance, vec3(endDistance));
        }

        // Straight shadow rays approximate each boundary from the exterior:
        // treating their unbent direction as an interior ray creates false TIR.
        float cosine = clamp(abs(dot(direction, normal)), 0.0, 1.0);
        visibility *= 1.0 - dielectricFresnel(cosine, 1.0, max(material.ior, 0.001));
        if (entering) {
            if (mediumCount == 8) {
                visibility = vec3(0.0);
                return false;
            }
            mediumInstances[mediumCount] = instance;
            mediumTransmittance[mediumCount++] = transmittance;
        } else if (mediumIndex >= 0) {
            --mediumCount;
            mediumInstances[mediumIndex] = mediumInstances[mediumCount];
            mediumTransmittance[mediumIndex] = mediumTransmittance[mediumCount];
        }
        previousDistance = endDistance;
        origin = offsetRayOrigin(hitPoint, normal, direction);
    }
    visibility = vec3(0.0);
    return false;
}

bool shadowVisibility(vec3 origin, vec3 direction, float maxDistance, out vec3 visibility) {
    return shadowVisibility(origin, direction, maxDistance, false, visibility);
}

vec3 randomSphereDirection(vec2 seed) {
    float z = 1.0 - 2.0 * rand(seed + vec2(5.3, 9.7));
    float phi = 2.0 * PI * rand(seed + vec2(17.2, 3.4));
    float r = sqrt(max(0.0, 1.0 - z * z));
    return vec3(r * cos(phi), r * sin(phi), z);
}

vec3 subsurfaceDirectLight(vec3 point, int mediumInstance, Material medium) {
    vec3 result = vec3(0.0);
    for (int i = 0; i < numLights && i < lights.length(); ++i) {
        Light light = lights[i];
        vec3 direction;
        float distanceToLight = 1e30;
        float attenuation = light.power;
        if (light.positionOrDirection.w == 1.0) {
            vec3 delta = light.positionOrDirection.xyz - point;
            distanceToLight = length(delta);
            if (distanceToLight <= RAY_EPSILON) continue;
            direction = delta / distanceToLight;
            attenuation /= distanceToLight * distanceToLight;
        } else {
            direction = normalize(light.positionOrDirection.xyz);
        }
        float distanceToExit;
        vec3 exitPoint, exitNormal;
        int exitMaterial, exitInstance;
        if (!traverseTLAS(Ray(point, direction), distanceToExit, exitPoint, exitNormal,
                          exitMaterial, exitInstance) ||
            exitInstance != mediumInstance || distanceToExit >= distanceToLight) continue;
        float cosine = abs(dot(direction, exitNormal));
        float transmission = 1.0 - dielectricFresnel(cosine, max(medium.ior, 0.001), 1.0);
        vec3 visibility;
        vec3 shadowOrigin = offsetRayOrigin(exitPoint, exitNormal, direction);
        if (transmission <= 0.0 ||
            !shadowVisibility(shadowOrigin, direction, distanceToLight - distanceToExit -
                              dot(shadowOrigin - exitPoint, direction), visibility)) continue;
        result += light.color * attenuation * visibility * transmission *
                  exp(-distanceToExit / medium.subsurfaceMeanFreePath) / (4.0 * PI);
    }
    return result;
}

bool sampleOpaque(Material m, vec3 n, vec3 v, vec2 seed, out vec3 l, out vec3 weight) {
    vec3 f0 = materialF0(m);
    float pSpec = clamp(max(f0.r, max(f0.g, f0.b)), 0.05, 0.95);
    if (rand(seed + vec2(7.31, 2.17)) < pSpec) {
        float u = rand(seed + vec2(11.13, 5.19));
        float phi = 2.0 * PI * rand(seed + vec2(3.71, 17.23));
        float alpha = pow(clamp(m.roughness, MIN_ROUGHNESS, 1.0), 2.0);
        float cosTheta = sqrt((1.0 - u) / (1.0 + (alpha * alpha - 1.0) * u));
        float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
        vec3 up = abs(n.y) < 0.99 ? vec3(0, 1, 0) : vec3(1, 0, 0);
        vec3 tangent = normalize(cross(up, n));
        vec3 h = normalize(tangent * (sinTheta * cos(phi)) +
                           cross(n, tangent) * (sinTheta * sin(phi)) + n * cosTheta);
        l = reflect(-v, h);
    } else {
        l = randomHemisphereDirection(n, seed + vec2(19.43, 23.11));
    }
    float nl = max(dot(n, l), 0.0);
    if (nl <= 0.0 || length(v + l) < 1e-6) return false;
    vec3 h = normalize(v + l);
    float nh = max(dot(n, h), 0.0), vh = max(dot(v, h), 0.0);
    float alpha = pow(clamp(m.roughness, MIN_ROUGHNESS, 1.0), 2.0);
    float a2 = alpha * alpha;
    float d = nh * nh * (a2 - 1.0) + 1.0;
    float D = a2 / max(PI * d * d, 1e-20);
    float pdf = (1.0 - pSpec) * nl / PI + pSpec * D * nh / max(4.0 * vh, 1e-6);
    if (pdf <= 0.0) return false;
    weight = opaqueBRDF(m, n, v, l) * nl / pdf;
    return true;
}
