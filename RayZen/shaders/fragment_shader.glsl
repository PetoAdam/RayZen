#version 430 core

// Uniforms
uniform vec2 resolution;

struct Camera {
    mat4 viewMatrix;
    mat4 projectionMatrix;
    mat4 invViewMatrix;
    mat4 invProjectionMatrix;
    vec3 position;
};
uniform Camera camera;

struct Material {
    vec3 albedo;
    float metallic;
    float roughness;
    float reflectivity;
    float transparency;
    float ior;
};

struct Light {
    vec4 positionOrDirection; // w==1.0 for point lights, w==0.0 for directional lights
    vec3 color;
    float power;
};

struct Triangle {
    vec3 v0;
    float pad0; // Padding to align to 16 bytes
    vec3 v1;
    float pad1; // Padding to align to 16 bytes
    vec3 v2;
    float pad2; // Padding to align to 16 bytes
    int materialIndex;
};

struct BVHNode {
    vec3 boundsMin;
    int leftFirst;
    vec3 boundsMax;
    int count;
};

layout(std430, binding = 0) buffer TriangleBuffer {
    Triangle triangles[];
};

layout(std430, binding = 1) buffer MaterialBuffer {
    Material materials[];
};

layout(std430, binding = 2) buffer LightBuffer {
    Light lights[];
};

layout(std430, binding = 3) buffer BVHNodeBuffer {
    BVHNode bvhNodes[];
};

layout(std430, binding = 4) buffer BVHTriIdxBuffer {
    int bvhTriIndices[];
};

uniform int numTriangles;
uniform bool debugShowLights;

out vec4 FragColor;

// Constants
const vec3 ambientLightColor = vec3(0.05, 0.05, 0.05);

// Ray structure
struct Ray {
    vec3 origin;
    vec3 direction;
};

//------------------------------------------------------------------------------
// Utility functions
//------------------------------------------------------------------------------
float rand(vec2 uv) {
    return fract(sin(dot(uv, vec2(12.9898, 78.233))) * 43758.5453);
}

vec3 randomHemisphereDirection(vec3 normal, vec2 seed) {
    float u = rand(seed);
    float v = rand(seed + vec2(1.0));
    float theta = acos(sqrt(1.0 - u));
    float phi = 2.0 * 3.14159 * v;
    vec3 dir = vec3(sin(theta)*cos(phi), sin(theta)*sin(phi), cos(theta));
    vec3 up = abs(normal.y) < 0.99 ? vec3(0,1,0) : vec3(1,0,0);
    vec3 tangent = normalize(cross(up, normal));
    vec3 bitangent = cross(normal, tangent);
    return normalize(tangent * dir.x + bitangent * dir.y + normal * dir.z);
}

Ray calculateRay(vec2 uv, vec2 seed) {
    vec2 jitter = vec2(rand(seed), rand(seed + vec2(1.0))) * 0.00002;
    uv += jitter;
    vec4 ray_clip = vec4(uv * 2.0 - 1.0, -1.0, 1.0);
    vec4 ray_eye = camera.invProjectionMatrix * ray_clip;
    ray_eye = vec4(ray_eye.xy, -1.0, 0.0);
    vec3 ray_world = (camera.invViewMatrix * ray_eye).xyz;
    return Ray(camera.position, normalize(ray_world));
}

//------------------------------------------------------------------------------
// Intersection functions
//------------------------------------------------------------------------------
// Möller–Trumbore algorithm for triangle intersection.
bool hitTriangle(Triangle tri, Ray ray, out float tHit, out vec3 hitPoint, out vec3 normal, out int materialIndex) {
    vec3 edge1 = tri.v1 - tri.v0;
    vec3 edge2 = tri.v2 - tri.v0;
    vec3 h = cross(ray.direction, edge2);
    float a = dot(edge1, h);
    if (abs(a) < 0.0001)
        return false;
    float f = 1.0 / a;
    vec3 s = ray.origin - tri.v0;
    float u = f * dot(s, h);
    if (u < 0.0 || u > 1.0)
        return false;
    vec3 q = cross(s, edge1);
    float v = f * dot(ray.direction, q);
    if (v < 0.0 || u + v > 1.0)
        return false;
    float t = f * dot(edge2, q);
    if (t > 0.0001) {
        tHit = t;
        hitPoint = ray.origin + ray.direction * t;
        normal = normalize(cross(edge1, edge2));
        materialIndex = tri.materialIndex;
        return true;
    }
    return false;
}

// Ray-AABB intersection
bool intersectAABB(vec3 rayOrig, vec3 rayDirInv, vec3 bmin, vec3 bmax, out float tmin, out float tmax) {
    vec3 t0 = (bmin - rayOrig) * rayDirInv;
    vec3 t1 = (bmax - rayOrig) * rayDirInv;
    vec3 tsmaller = min(t0, t1);
    vec3 tbigger = max(t0, t1);
    tmin = max(max(tsmaller.x, tsmaller.y), tsmaller.z);
    tmax = min(min(tbigger.x, tbigger.y), tbigger.z);
    return tmax >= max(tmin, 0.0);
}

// BVH traversal for closest hit
bool traverseBVH(Ray ray, out float tHit, out vec3 hitPoint, out vec3 normal, out int materialIndex) {
    tHit = 1e30;
    bool hit = false;
    int stack[64];
    int stackPtr = 0;
    stack[stackPtr++] = 0; // root node
    vec3 invDir = 1.0 / ray.direction;
    while (stackPtr > 0) {
        int nidx = stack[--stackPtr];
        BVHNode node = bvhNodes[nidx];
        float tmin, tmax;
        if (!intersectAABB(ray.origin, invDir, node.boundsMin, node.boundsMax, tmin, tmax) || tmin > tHit)
            continue;
        if (node.count > 0) { // leaf
            for (int i = 0; i < node.count; ++i) {
                int triIdx = bvhTriIndices[node.leftFirst + i];
                float t;
                vec3 tempHit, tempNormal;
                int tempMat;
                if (hitTriangle(triangles[triIdx], ray, t, tempHit, tempNormal, tempMat)) {
                    if (t < tHit) {
                        tHit = t;
                        hitPoint = tempHit;
                        normal = tempNormal;
                        materialIndex = tempMat;
                        hit = true;
                    }
                }
            }
        } else { // internal
            stack[stackPtr++] = node.leftFirst;
            stack[stackPtr++] = node.leftFirst + 1;
        }
    }
    return hit;
}

// Shadow test: cast a ray from hitPoint along lightDir and check for occlusion
bool isInShadow(vec3 hitPoint, vec3 lightDir, float maxDistance, out float shadowAttenuation) {
    shadowAttenuation = 1.0;
    float tHit;
    vec3 tempHit, tempNormal;
    int tempMat;
    Ray shadowRay = Ray(hitPoint, lightDir);
    if (traverseBVH(shadowRay, tHit, tempHit, tempNormal, tempMat)) {
        if (tHit > 0.001 && tHit < maxDistance) {
            float transparency = materials[tempMat].transparency;
            shadowAttenuation *= transparency;
            if (shadowAttenuation < 0.05)
                return true;
        }
    }
    return false;
}

//------------------------------------------------------------------------------
// Lighting and material functions
//------------------------------------------------------------------------------
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

vec3 reflectRay(vec3 incident, vec3 normal) {
    return incident - 2.0 * dot(incident, normal) * normal;
}

vec3 refractRay(vec3 incident, vec3 normal, float ior) {
    float cosi = clamp(dot(incident, normal), -1.0, 1.0);
    // TODO: check if this is needed
    //if (dot(incident, normal) > 0.0) normal = -normal;
    float etai = 1.0, etat = ior;
    vec3 n = normal;
    if (cosi < 0.0) {
        cosi = -cosi;
    } else {
        float temp = etai;
        etai = etat;
        etat = temp;
        n = -normal;
    }
    float eta = etai / etat;
    float k = 1.0 - eta * eta * (1.0 - cosi * cosi);
    return (k < 0.0) ? reflectRay(incident, normal) : eta * incident + (eta * cosi - sqrt(k)) * n;
}

vec3 calculateLighting(vec3 hitPoint, vec3 normal, Material material, vec3 viewDir) {
    vec3 F0 = mix(vec3(0.04), material.albedo, material.metallic);
    vec3 finalColor = ambientLightColor * material.albedo;

    // Loop over lights (assumed 10 lights maximum)
    for (int i = 0; i < 10; ++i) {
        Light light = lights[i];
        vec3 lightDir;
        float attenuation = 1.0;
        float shadowAttenuation;

        if (light.positionOrDirection.w == 1.0) { // Point light
            vec3 lightVec = light.positionOrDirection.xyz - hitPoint;
            float distance = max(length(lightVec), 0.001);
            lightDir = normalize(lightVec);
            attenuation = light.power / (distance * distance);

            // Check shadows for point lights
            if (isInShadow(hitPoint + lightDir * 0.001, lightDir, distance, shadowAttenuation))
                continue;
        } else { // Directional light
            lightDir = normalize(light.positionOrDirection.xyz);
            attenuation = light.power;

            // Check shadows for directional lights
            if (isInShadow(hitPoint + lightDir * 0.001, lightDir, 1e30, shadowAttenuation))
                continue;
        }
        attenuation *= shadowAttenuation;

        // Calculate shading terms
        vec3 halfwayDir = normalize(lightDir + viewDir);
        float NdotL = max(dot(normal, lightDir), 0.0);
        float NdotV = max(dot(normal, viewDir), 0.0);

        vec3 F = fresnelSchlick(max(dot(halfwayDir, viewDir), 0.0), F0);

        float alpha = material.roughness * material.roughness;
        float alpha2 = alpha * alpha;
        float denom = (dot(normal, halfwayDir) * dot(normal, halfwayDir) * (alpha2 - 1.0) + 1.0);
        float D = alpha2 / (3.14159 * denom * denom);

        float k = (material.roughness + 1.0) * (material.roughness + 1.0) / 8.0;
        float G = NdotV / (NdotV * (1.0 - k) + k);
        G *= NdotL / (NdotL * (1.0 - k) + k);

        float denomSpec = max(4.0 * NdotV * NdotL, 0.0001);
        vec3 specular = (F * D * G) / denomSpec;

        vec3 diffuse = (1.0 - F) * material.albedo * NdotL / 3.14159;

        finalColor += max(vec3(0.0), (diffuse + specular) * light.color * attenuation);
    }

    return finalColor;
}

//------------------------------------------------------------------------------
// Main path tracing loop
//------------------------------------------------------------------------------
void main() {
    vec2 uv = gl_FragCoord.xy / resolution;
    vec2 seed = uv;

    vec3 color = vec3(0.0);
    int maxBounces = 3;
    int numSamples = 2;

    for (int samp = 0; samp < numSamples; ++samp) {
        seed = uv * float(gl_FragCoord.x + gl_FragCoord.y + samp + 1.0);
        Ray ray = calculateRay(uv, seed);
        vec3 currentOrigin = ray.origin;
        vec3 currentDirection = ray.direction;
        vec3 throughput = vec3(1.0);
        bool hitSomething = false;

        for (int bounce = 0; bounce < maxBounces; ++bounce) {
            vec2 tempseed = seed * float(bounce * bounce) * 12793.46 + float(bounce) * 1423.34;
            vec3 hitPoint, hitNormal;
            int materialIndex = -1;
            float closestT = 1e30;
            Material hitMaterial;

            // Use BVH traversal for intersection
            bool found = traverseBVH(Ray(currentOrigin, currentDirection), closestT, hitPoint, hitNormal, materialIndex);
            if (!found) {
                // Skybox: blueish gradient, less bright
                float t = 0.5 * (normalize(currentDirection).y + 1.0);
                vec3 skyColor = mix(vec3(0.15, 0.25, 0.45), vec3(0.5, 0.7, 1.0), t); // deep blue to light blue
                color += throughput * skyColor;
                break;
            }
            hitSomething = true;
            hitMaterial = materials[materialIndex];
            vec3 viewDir = normalize(camera.position - hitPoint);
            color += throughput * calculateLighting(hitPoint, hitNormal, hitMaterial, viewDir);

            float randVal = rand(tempseed + vec2(float(samp), float(bounce)));

            // Transparent material: mix reflection and refraction
            if (hitMaterial.transparency > 0.0) {
                float cosi = clamp(dot(-currentDirection, hitNormal), 0.0, 1.0);
                float F0 = pow((1.0 - hitMaterial.ior) / (1.0 + hitMaterial.ior), 2.0);
                vec3 F0vec = vec3(F0);
                vec3 F = fresnelSchlick(cosi, F0vec);
                float fresnelProbability = clamp(F.r, 0.0, 1.0);

                //TODO: Check other option?
                if (randVal > hitMaterial.transparency /*randVal < fresnelProbability * hitMaterial.transparency*/) {
                    currentDirection = reflectRay(currentDirection, hitNormal);
                } else {
                    //currentDirection = refractRay(currentDirection, hitNormal, hitMaterial.ior);
                }
            } else {
                // For opaque materials: choose between reflection and diffuse scattering.
                if (randVal < hitMaterial.reflectivity) {
                    currentDirection = reflectRay(currentDirection, hitNormal);
                } else {
                    currentDirection = randomHemisphereDirection(hitNormal, tempseed);
                    throughput *= 0.05; // extra attenuation for diffuse bounces
                }
            }
            currentOrigin = hitPoint + currentDirection * 0.001;
            throughput *= hitMaterial.albedo;

            // Russian roulette termination after a few bounces
            if (bounce > 2) {
                float p = max(throughput.r, max(throughput.g, throughput.b));
                if (rand(tempseed + vec2(float(samp), float(bounce))) > p)
                    break;
                throughput /= p;
            }
        }
    }
    color /= float(numSamples);
    color = clamp(color, 0.0, 1.0);

    // Debug: Render light positions as screen-space markers
    if (debugShowLights) {
        for (int i = 0; i < 10; ++i) {
            Light light = lights[i];
            if (light.positionOrDirection.w == 1.0) { // Only for point lights
                // Project world position to NDC
                vec4 clip = camera.projectionMatrix * camera.viewMatrix * vec4(light.positionOrDirection.xyz, 1.0);
                if (clip.w > 0.0) {
                    vec3 ndc = clip.xyz / clip.w;
                    // Convert NDC to screen coordinates
                    vec2 screen = (ndc.xy * 0.5 + 0.5) * resolution;
                    float dist = length(gl_FragCoord.xy - screen);
                    float markerRadius = 8.0; // pixels
                    if (dist < markerRadius) {
                        // Draw a colored circle (yellow)
                        float alpha = smoothstep(markerRadius, markerRadius-2.0, dist);
                        color = mix(color, light.color, alpha);
                    }
                }
            }
        }
    }
    FragColor = vec4(color, 1.0);
}
