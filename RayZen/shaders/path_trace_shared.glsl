// RayZen path tracer — compute shader backend.
// Workgroup is one tile of the output image; each invocation traces one pixel.
//
// SSBO bindings match the legacy fragment-shader path tracer for one-to-one
// data compatibility with the rest of the engine.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

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
    float subsurfaceMeanFreePath;
    int baseColorTexture;
};

struct Light {
    vec4 positionOrDirection;
    vec3 color;
    float power;
};

struct Triangle {
    vec3 v0;  float pad0;
    vec3 v1;  float pad1;
    vec3 v2;  float pad2;
    int materialIndex;
    int hasUV;
    vec2 uv0;
    vec2 uv1;
    vec2 uv2;
};

struct BVHNode {
    vec3 boundsMin;
    int leftFirst;
    vec3 boundsMax;
    int count;
};

layout(std430, binding = 0) buffer TriangleBuffer  { Triangle triangles[]; };
layout(std430, binding = 1) buffer MaterialBuffer  { Material materials[]; };
layout(std430, binding = 2) buffer LightBuffer     { Light    lights[];    };
layout(std430, binding = 5) buffer TLASNodeBuffer  { BVHNode  tlasNodes[]; };
layout(std430, binding = 6) buffer TLASTriIdxBuffer{ int      tlasTriIndices[]; };
layout(std430, binding = 7) buffer BLASNodeBuffer  { BVHNode  blasNodes[]; };
layout(std430, binding = 8) buffer BLASTriIdxBuffer{ int      blasTriIndices[]; };
struct BVHInstance {
    int blasNodeOffset;
    int blasTriOffset;
    int meshIndex;
    int globalTriOffset;
    mat4 transform;
    mat4 inverseTransform;
};
layout(std430, binding = 9) buffer BVHInstanceBuffer { BVHInstance bvhInstances[]; };

layout(rgba32f, binding = 0) uniform coherent image2D accumulationImage;

uniform int   numTriangles;
uniform int   numLights;
uniform int   uniformBounceBudget;
#ifdef RAYZEN_SHADING_MODE
#define uniformShadingMode RAYZEN_SHADING_MODE
#else
uniform int   uniformShadingMode;
#endif
uniform bool  uniformSubsurfaceEnabled;
uniform bool  uniformCausticsEnabled;
uniform int   uniformAccumulatedFrames;
uniform bool  uniformResetAccumulation;
uniform int   uniformFrameIndex;
uniform vec3  uniformAmbientLightColor;
layout(binding = 1) uniform sampler2DArray materialTextures;
layout(r32ui, binding = 2) uniform coherent uimageBuffer causticPixels;

const float PI = 3.14159265359;
const float RAY_EPSILON = 0.002;
const float MIN_ROUGHNESS = 0.05;

struct Ray { vec3 origin; vec3 direction; };

float rand(vec2 uv) {
    return fract(sin(dot(uv, vec2(12.9898, 78.233))) * 43758.5453);
}

vec3 offsetRayOrigin(vec3 point, vec3 normal, vec3 direction) {
    float push = dot(direction, normal) >= 0.0 ? 1.0 : -1.0;
    return point + normal * push * RAY_EPSILON;
}

vec3 randomHemisphereDirection(vec3 normal, vec2 seed) {
    float u = rand(seed);
    float v = rand(seed + vec2(1.0));
    float theta = acos(sqrt(1.0 - u));
    float phi = 2.0 * PI * v;
    vec3 dir = vec3(sin(theta)*cos(phi), sin(theta)*sin(phi), cos(theta));
    vec3 up = abs(normal.y) < 0.99 ? vec3(0,1,0) : vec3(1,0,0);
    vec3 tangent = normalize(cross(up, normal));
    vec3 bitangent = cross(normal, tangent);
    return normalize(tangent * dir.x + bitangent * dir.y + normal * dir.z);
}

vec3 clampRadiancePreserveHue(vec3 r, float maxComponent) {
    float peak = max(r.r, max(r.g, r.b));
    if (peak <= maxComponent || peak <= 0.0) return r;
    return r * (maxComponent / peak);
}

Ray calculateRay(vec2 uv, vec2 seed) {
    vec2 jitter = (vec2(rand(seed + vec2(13.1, 91.7)), rand(seed + vec2(47.2, 19.3))) - 0.5)
                  / max(resolution, vec2(1.0));
    uv += jitter;
    vec4 ray_clip = vec4(uv * 2.0 - 1.0, -1.0, 1.0);
    vec4 ray_eye = camera.invProjectionMatrix * ray_clip;
    ray_eye = vec4(ray_eye.xy, -1.0, 0.0);
    vec3 ray_world = (camera.invViewMatrix * ray_eye).xyz;
    return Ray(camera.position, normalize(ray_world));
}

bool intersectAABB(vec3 rayOrig, vec3 rayDirInv, vec3 bmin, vec3 bmax, out float tmin, out float tmax) {
    vec3 t0 = (bmin - rayOrig) * rayDirInv;
    vec3 t1 = (bmax - rayOrig) * rayDirInv;
    vec3 tsmaller = min(t0, t1);
    vec3 tbigger  = max(t0, t1);
    tmin = max(max(tsmaller.x, tsmaller.y), tsmaller.z);
    tmax = min(min(tbigger.x, tbigger.y), tbigger.z);
    return tmax >= max(tmin, 0.0);
}

bool hitTriangle(Triangle tri, Ray ray, out float tHit, out vec3 hitPoint, out vec3 normal, out int materialIndex, out vec2 uv) {
    vec3 edge1 = tri.v1 - tri.v0;
    vec3 edge2 = tri.v2 - tri.v0;
    vec3 h = cross(ray.direction, edge2);
    float a = dot(edge1, h);
    if (abs(a) < 0.0001) return false;
    float f = 1.0 / a;
    vec3 s = ray.origin - tri.v0;
    float u = f * dot(s, h);
    if (u < 0.0 || u > 1.0) return false;
    vec3 q = cross(s, edge1);
    float v = f * dot(ray.direction, q);
    if (v < 0.0 || u + v > 1.0) return false;
    float t = f * dot(edge2, q);
    if (t > RAY_EPSILON) {
        tHit = t;
        hitPoint = ray.origin + ray.direction * t;
        normal = normalize(cross(edge1, edge2));
        materialIndex = tri.materialIndex;
        uv = tri.hasUV != 0 ? tri.uv0 * (1.0 - u - v) + tri.uv1 * u + tri.uv2 * v : vec2(-1e30);
        return true;
    }
    return false;
}

bool traverseBLAS(Ray ray, int blasNodeOffset, int blasTriOffset, int globalTriOffset,
                  out float tHit, out vec3 hitPoint, out vec3 normal, out int materialIndex, out vec2 uv) {
    tHit = 1e30;
    bool hit = false;
    int stack[64];
    int sp = 0;
    stack[sp++] = 0;
    vec3 invDir = 1.0 / ray.direction;
    while (sp > 0) {
        int nidx = stack[--sp];
        BVHNode node = blasNodes[blasNodeOffset + nidx];
        float tmin, tmax;
        if (!intersectAABB(ray.origin, invDir, node.boundsMin, node.boundsMax, tmin, tmax) || tmin > tHit)
            continue;
        if (node.count > 0) {
            for (int i = 0; i < node.count; ++i) {
                int triIdx = globalTriOffset + blasTriIndices[blasTriOffset + node.leftFirst + i];
                float t;
                vec3 hp, n;
                int mi;
                vec2 candidateUV;
                if (hitTriangle(triangles[triIdx], ray, t, hp, n, mi, candidateUV)) {
                    if (t < tHit) {
                        tHit = t; hitPoint = hp; normal = n; materialIndex = mi; uv = candidateUV; hit = true;
                    }
                }
            }
        } else {
            int leftIdx = node.leftFirst;
            int rightIdx = node.leftFirst + 1;
            BVHNode L = blasNodes[blasNodeOffset + leftIdx];
            BVHNode R = blasNodes[blasNodeOffset + rightIdx];
            float Ltmin, Ltmax, Rtmin, Rtmax;
            bool hitL = intersectAABB(ray.origin, invDir, L.boundsMin, L.boundsMax, Ltmin, Ltmax) && Ltmin <= tHit;
            bool hitR = intersectAABB(ray.origin, invDir, R.boundsMin, R.boundsMax, Rtmin, Rtmax) && Rtmin <= tHit;
            if (hitL && hitR) {
                if (Ltmin < Rtmin) { stack[sp++] = rightIdx; stack[sp++] = leftIdx; }
                else               { stack[sp++] = leftIdx;  stack[sp++] = rightIdx; }
            } else if (hitL) {
                stack[sp++] = leftIdx;
            } else if (hitR) {
                stack[sp++] = rightIdx;
            }
        }
    }
    return hit;
}

bool traverseTLAS(Ray ray, out float tHit, out vec3 hitPoint, out vec3 normal,
                  out int materialIndex, out int instanceIdx, out vec2 uv) {
    tHit = 1e30;
    if (tlasNodes.length() == 0) return false;
    bool hit = false;
    int stack[64];
    int sp = 0;
    stack[sp++] = 0;
    vec3 invDir = 1.0 / ray.direction;
    while (sp > 0) {
        int nidx = stack[--sp];
        BVHNode node = tlasNodes[nidx];
        float tmin, tmax;
        if (!intersectAABB(ray.origin, invDir, node.boundsMin, node.boundsMax, tmin, tmax) || tmin > tHit)
            continue;
        if (node.count > 0) {
            for (int i = 0; i < node.count; ++i) {
                int instIdx = tlasTriIndices[node.leftFirst + i];
                BVHInstance inst = bvhInstances[instIdx];
                mat4 invT = inst.inverseTransform;
                vec3 lo = vec3(invT * vec4(ray.origin, 1.0));
                vec3 ld = normalize(vec3(invT * vec4(ray.direction, 0.0)));
                Ray lr = Ray(lo, ld);
                float tLocal;
                vec3 lh, ln;
                int mi;
                vec2 localUV;
                if (traverseBLAS(lr, inst.blasNodeOffset, inst.blasTriOffset, inst.globalTriOffset,
                                 tLocal, lh, ln, mi, localUV)) {
                    vec3 wh = vec3(inst.transform * vec4(lh, 1.0));
                    float tWorld = length(wh - ray.origin);
                    if (tWorld < tHit) {
                        tHit = tWorld;
                        hitPoint = wh;
                        mat3 nm = mat3(transpose(inst.inverseTransform));
                        normal = normalize(nm * ln);
                        materialIndex = mi;
                        instanceIdx = instIdx;
                        uv = localUV;
                        hit = true;
                    }
                }
            }
        } else {
            int leftIdx = node.leftFirst;
            int rightIdx = node.leftFirst + 1;
            BVHNode L = tlasNodes[leftIdx];
            BVHNode R = tlasNodes[rightIdx];
            float Ltmin, Ltmax, Rtmin, Rtmax;
            bool hitL = intersectAABB(ray.origin, invDir, L.boundsMin, L.boundsMax, Ltmin, Ltmax) && Ltmin <= tHit;
            bool hitR = intersectAABB(ray.origin, invDir, R.boundsMin, R.boundsMax, Rtmin, Rtmax) && Rtmin <= tHit;
            if (hitL && hitR) {
                if (Ltmin < Rtmin) { stack[sp++] = rightIdx; stack[sp++] = leftIdx; }
                else               { stack[sp++] = leftIdx;  stack[sp++] = rightIdx; }
            } else if (hitL) {
                stack[sp++] = leftIdx;
            } else if (hitR) {
                stack[sp++] = rightIdx;
            }
        }
    }
    return hit;
}

bool traverseTLAS(Ray ray, out float tHit, out vec3 hitPoint, out vec3 normal,
                  out int materialIndex, out int instanceIdx) {
    vec2 uv;
    return traverseTLAS(ray, tHit, hitPoint, normal, materialIndex, instanceIdx, uv);
}

#include "material_sampling.glsl"

#ifndef RAYZEN_CAUSTIC_PASS
#include "direct_lighting.glsl"
#if RAYZEN_SHADING_MODE == 1
#include "ray_tracing.glsl"
#endif

void main() {
    ivec2 pixelCoord = ivec2(gl_GlobalInvocationID.xy);
    if (pixelCoord.x >= int(resolution.x) || pixelCoord.y >= int(resolution.y)) return;
    vec2 uv = (vec2(pixelCoord) + vec2(0.5)) / resolution;

    vec2 frameSeed = vec2(float(uniformFrameIndex) + 0.5,
                          float(uniformFrameIndex) * 1.61803398875 + 0.5);
    vec2 seed = uv + frameSeed;

    vec3 radiance = vec3(0.0);
    int numSamples = 1;

#if RAYZEN_SHADING_MODE == 1
    {
        seed = (uv + frameSeed) * float(pixelCoord.x + pixelCoord.y + 1.0);
        radiance = traceRealtime(calculateRay(uv, seed), seed);
    }
#elif RAYZEN_SHADING_MODE == 2
    {
        seed = (uv + frameSeed) * float(pixelCoord.x + pixelCoord.y + 1.0);
        Ray ray = calculateRay(uv, seed);
        float distance;
        vec3 point, normal;
        int materialIndex, instance;
        vec2 hitUV;
        if (traverseTLAS(ray, distance, point, normal, materialIndex, instance, hitUV)) {
            Material material = materials[materialIndex];
            applyBaseColorTexture(material, hitUV);
            vec3 viewDir = -ray.direction;
            normal = dot(normal, viewDir) >= 0.0 ? normal : -normal;
            radiance = calculateLighting(point, normal, material, viewDir, true);
        } else {
            float t = 0.5 * (ray.direction.y + 1.0);
            radiance = mix(vec3(0.15, 0.25, 0.45), vec3(0.5, 0.7, 1.0), t);
        }
    }
#else
    int maxBounces = max(uniformBounceBudget, 1);
    for (int samp = 0; samp < numSamples; ++samp) {
        seed = (uv + frameSeed) * float(pixelCoord.x + pixelCoord.y + samp + 1.0);
        Ray ray = calculateRay(uv, seed);
        vec3 origin = ray.origin;
        vec3 dir = ray.direction;
        vec3 throughput = vec3(1.0);
        int mediumInstance = -1;
        int mediumMaterial = -1;
        vec3 mediumAlbedo = vec3(1.0);

        for (int bounce = 0; bounce < maxBounces; ++bounce) {
            vec2 tempseed = seed * float((bounce + 1) * (bounce + 1)) * 12793.46 + float(bounce) * 1423.34;
            vec3 hp, hn;
            int matIdx = -1;
            int instIdx = -1;
            float closestT = 1e30;
            Material mat;
            vec2 hitUV;

            bool found = traverseTLAS(Ray(origin, dir), closestT, hp, hn, matIdx, instIdx, hitUV);
            if (mediumInstance >= 0) {
                Material medium = materials[mediumMaterial];
                medium.albedo = mediumAlbedo;
                if (uniformSubsurfaceEnabled && medium.subsurfaceMeanFreePath > 0.0) {
                    float distance = -log(max(1.0 - rand(tempseed + vec2(63.1, 8.7)), 1e-6)) *
                                     medium.subsurfaceMeanFreePath;
                    if (distance < closestT) {
                        vec3 scatterPoint = origin + dir * distance;
                        throughput *= clamp(medium.albedo, vec3(0.0), vec3(1.0));
                        radiance += throughput * subsurfaceDirectLight(scatterPoint, mediumInstance, medium);
                        if (bounce + 1 >= maxBounces) break;
                        origin = scatterPoint;
                        dir = randomSphereDirection(tempseed);
                        continue;
                    }
                } else if (found) {
                    vec3 transmittance = clamp(medium.albedo * medium.transparency, vec3(0.0001), vec3(1.0));
                    throughput *= pow(transmittance, vec3(closestT));
                }
                if (!found || instIdx != mediumInstance) break;
            }
            if (!found) {
                float t = 0.5 * (normalize(dir).y + 1.0);
                vec3 sky = mix(vec3(0.15, 0.25, 0.45), vec3(0.5, 0.7, 1.0), t);
                radiance += throughput * sky;
                break;
            }
            mat = materials[matIdx];
            applyBaseColorTexture(mat, hitUV);
            vec3 viewDir = normalize(-dir);
            vec3 shadingNormal = dot(hn, viewDir) >= 0.0 ? hn : -hn;
            if (mediumInstance < 0)
                radiance += throughput * calculateLighting(hp, shadingNormal, mat, viewDir, bounce == 0);
            if (bounce + 1 >= maxBounces) break;
            float r = rand(tempseed + vec2(float(samp), float(bounce)));
            if (mat.transparency > 0.0 ||
                (uniformSubsurfaceEnabled && mat.subsurfaceMeanFreePath > 0.0)) {
                bool entering = mediumInstance < 0;
                float ior = max(mat.ior, 0.001);
                if (sampleDielectric(dir, shadingNormal, entering ? 1.0 : ior,
                                     entering ? ior : 1.0, r, dir)) {
                    mediumInstance = entering ? instIdx : -1;
                    mediumMaterial = entering ? matIdx : -1;
                    if (entering) mediumAlbedo = mat.albedo;
                }
            } else {
                vec3 weight;
                if (!sampleOpaque(mat, shadingNormal, viewDir, tempseed, dir, weight)) break;
                throughput *= weight;
            }
            origin = offsetRayOrigin(hp, hn, dir);
            if (bounce > 2) {
                float p = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05, 0.95);
                if (rand(tempseed + vec2(31.73 + float(samp), 47.29 + float(bounce))) > p) break;
                throughput /= p;
            }
        }
    }
#endif

    vec3 sampleRadiance = max(radiance / float(numSamples), vec3(0.0));
    sampleRadiance = clampRadiancePreserveHue(sampleRadiance, 8.0);
    if (uniformCausticsEnabled) {
        int index = (pixelCoord.y * int(resolution.x) + pixelCoord.x) * 4;
        sampleRadiance += vec3(imageLoad(causticPixels, index).r,
                               imageLoad(causticPixels, index + 1).r,
                               imageLoad(causticPixels, index + 2).r) / 1048576.0;
    }

    ivec2 storeCoord = clamp(pixelCoord, ivec2(0), imageSize(accumulationImage) - ivec2(1));
    vec3 accumulatedRadiance = sampleRadiance;
    if (!uniformResetAccumulation && uniformAccumulatedFrames > 0) {
        vec3 previous = imageLoad(accumulationImage, storeCoord).rgb;
        float blend = 1.0 / (float(uniformAccumulatedFrames) + 1.0);
        accumulatedRadiance = mix(previous, sampleRadiance, blend);
    }
    imageStore(accumulationImage, storeCoord, vec4(accumulatedRadiance, 1.0));
}
#endif
