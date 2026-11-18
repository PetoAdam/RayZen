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
    float subsurfaceMeanFreePath;
    int baseColorTexture;
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

// --- TLAS/BLAS two-level BVH support ---

// TLAS (top-level BVH over mesh instances)
layout(std430, binding = 5) buffer TLASNodeBuffer {
    BVHNode tlasNodes[];
};
layout(std430, binding = 6) buffer TLASTriIdxBuffer {
    int tlasTriIndices[];
};

// BLAS (all mesh BVHs concatenated)
layout(std430, binding = 7) buffer BLASNodeBuffer {
    BVHNode blasNodes[];
};
layout(std430, binding = 8) buffer BLASTriIdxBuffer {
    int blasTriIndices[];
};

// BVHInstance buffer (maps TLAS leaves to BLAS offsets and mesh index)
struct BVHInstance {
    int blasNodeOffset;
    int blasTriOffset;
    int meshIndex;
    int globalTriOffset; // Offset into global triangle buffer (NEW)
    mat4 transform; // (optional, not used if identity)
    mat4 inverseTransform;
};
layout(std430, binding = 9) buffer BVHInstanceBuffer {
    BVHInstance bvhInstances[];
};

layout(rgba32f, binding = 0) uniform coherent image2D accumulationImage;

uniform int numTriangles;
uniform bool debugShowLights;
uniform int numLights; // actual number of lights in SSBO
uniform bool debugShowBVH;
uniform int debugBVHMode; // 0 = TLAS, 1 = BLAS
uniform int debugSelectedBLAS; // mesh index
uniform int debugSelectedTri; // triangle index within mesh
uniform int uniformBounceBudget; // <=0 uses default bounce count
uniform int uniformShadingMode;
uniform bool uniformSubsurfaceEnabled;
uniform int uniformAccumulatedFrames;
uniform bool uniformResetAccumulation;
uniform int uniformFrameIndex;
uniform bool uniformUseImageAccumulation;
uniform float uniformDisplayExposure;
uniform vec3 uniformAmbientLightColor;
layout(binding = 1) uniform sampler2DArray materialTextures;

out vec4 FragColor;

// Constants
const float PI = 3.14159265359;
const float RAY_EPSILON = 0.002;
const float MIN_ROUGHNESS = 0.05;

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

vec3 offsetRayOrigin(vec3 point, vec3 normal, vec3 direction) {
    float push = dot(direction, normal) >= 0.0 ? 1.0 : -1.0;
    return point + normal * push * RAY_EPSILON;
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

vec3 clampRadiancePreserveHue(vec3 radiance, float maxComponent) {
    float peak = max(radiance.r, max(radiance.g, radiance.b));
    if (peak <= maxComponent || peak <= 0.0) {
        return radiance;
    }
    return radiance * (maxComponent / peak);
}

vec3 acesToneMap(vec3 color) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

vec3 acesToneMapHue(vec3 color) {
    float maxC = max(color.r, max(color.g, color.b));
    if (maxC < 1e-6) return vec3(0.0);
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    float tonedMax = clamp((maxC * (a * maxC + b)) / (maxC * (c * maxC + d) + e), 0.0, 1.0);
    return clamp(color * (tonedMax / maxC), 0.0, 1.0);
}

Ray calculateRay(vec2 uv, vec2 seed) {
    vec2 jitter = (vec2(rand(seed + vec2(13.1, 91.7)), rand(seed + vec2(47.2, 19.3))) - 0.5) / max(resolution, vec2(1.0));
    uv += jitter;
    vec4 ray_clip = vec4(uv * 2.0 - 1.0, -1.0, 1.0);
    vec4 ray_eye = camera.invProjectionMatrix * ray_clip;
    ray_eye = vec4(ray_eye.xy, -1.0, 0.0);
    vec3 ray_world = (camera.invViewMatrix * ray_eye).xyz;
    return Ray(camera.position, normalize(ray_world));
}

// Helper: HSV to RGB for gradient coloring
vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

// Distance from point p to segment ab
float distanceToSegment(vec2 p, vec2 a, vec2 b) {
    vec2 ab = b - a;
    float t = clamp(dot(p - a, ab) / dot(ab, ab), 0.0, 1.0);
    return length(p - (a + t * ab));
}

// Draw wireframe for a single AABB in screen space
float aabbWireframe(vec3 bmin, vec3 bmax, mat4 viewProj, vec2 fragCoord, float thickness) {
    // 12 edges of the box
    vec3 corners[8];
    corners[0] = vec3(bmin.x, bmin.y, bmin.z);
    corners[1] = vec3(bmax.x, bmin.y, bmin.z);
    corners[2] = vec3(bmax.x, bmax.y, bmin.z);
    corners[3] = vec3(bmin.x, bmax.y, bmin.z);
    corners[4] = vec3(bmin.x, bmin.y, bmax.z);
    corners[5] = vec3(bmax.x, bmin.y, bmax.z);
    corners[6] = vec3(bmax.x, bmax.y, bmax.z);
    corners[7] = vec3(bmin.x, bmax.y, bmax.z);
    int edges[24] = int[24](0,1, 1,2, 2,3, 3,0, 4,5, 5,6, 6,7, 7,4, 0,4, 1,5, 2,6, 3,7);
    float minDist = 1e6;
    for (int i = 0; i < 12; ++i) {
        vec4 p0 = viewProj * vec4(corners[edges[i*2]], 1.0);
        vec4 p1 = viewProj * vec4(corners[edges[i*2+1]], 1.0);
        if (p0.w <= 0.0 || p1.w <= 0.0) continue;
        vec2 s0 = p0.xy / p0.w * 0.5 + 0.5;
        vec2 s1 = p1.xy / p1.w * 0.5 + 0.5;
        s0 *= resolution;
        s1 *= resolution;
        float d = distanceToSegment(fragCoord, s0, s1);
        minDist = min(minDist, d);
    }
    return minDist < thickness ? 1.0 : 0.0;
}

// Helper: robust iterative search for path from root to leaf containing selectedTri in BLAS
void findBVHBranchIterative(int nodeOffset, int triOffset, int nodeCount, int selectedTri, out int path[32], out int pathLen) {
    int cur = 0;
    pathLen = 0;
    for (int depth = 0; depth < 32; ++depth) {
        path[pathLen++] = cur;
        BVHNode node = blasNodes[nodeOffset + cur];
        if (node.count > 0) { // leaf
            bool found = false;
            for (int i = 0; i < node.count; ++i) {
                int triIdx = blasTriIndices[triOffset + node.leftFirst + i];
                if (triIdx == selectedTri) {
                    found = true;
                    break;
                }
            }
            if (!found) pathLen = 0; // not found in this leaf
            break;
        } else {
            // Internal: check both children for selectedTri
            int leftIdx = node.leftFirst;
            int rightIdx = node.leftFirst + 1;
            bool foundInLeft = false;
            // Search left subtree for selectedTri
            int stack[32]; int sp = 0;
            stack[sp++] = leftIdx;
            while (sp > 0) {
                int nidx = stack[--sp];
                if (nidx < 0 || nidx >= nodeCount) continue;
                BVHNode n = blasNodes[nodeOffset + nidx];
                if (n.count > 0) {
                    for (int j = 0; j < n.count; ++j) {
                        int triIdx = blasTriIndices[triOffset + n.leftFirst + j];
                        if (triIdx == selectedTri) {
                            foundInLeft = true;
                            break;
                        }
                    }
                } else {
                    stack[sp++] = n.leftFirst;
                    stack[sp++] = n.leftFirst + 1;
                }
                if (foundInLeft) break;
            }
            if (foundInLeft) {
                cur = leftIdx;
            } else {
                cur = rightIdx;
            }
        }
    }
}

// BVH wireframe overlay for TLAS and BLAS
void overlayBVHWireframe(vec2 fragCoord, out float tlasWire, out vec3 tlasColor, out float blasWire, out vec3 blasColor) {
    tlasWire = 0.0;
    tlasColor = vec3(0.0);
    blasWire = 0.0;
    blasColor = vec3(0.0);
    if (debugBVHMode == 0) {
        // TLAS overlay (iterate all nodes; leaves only)
        int tlasNodeCount = tlasNodes.length();
        for (int i = 0; i < tlasNodeCount; ++i) {
            BVHNode node = tlasNodes[i];
            if (node.count > 0) { // Only draw leaves
                float w = aabbWireframe(node.boundsMin, node.boundsMax, camera.projectionMatrix * camera.viewMatrix, fragCoord, 1.5);
                if (w > 0.0) {
                    //float t = float(i) / float(tlasNodes.length());
                    //vec3 grad = hsv2rgb(vec3(0.0 + t * 0.5, 1.0, 1.0));
                    int meshIdx = tlasTriIndices[node.leftFirst];
                    if (meshIdx < 0 || meshIdx >= bvhInstances.length()) continue;
                    float t = float(meshIdx) / float(bvhInstances.length());
                    vec3 grad = hsv2rgb(vec3(0.0 + t * 0.5, 1.0, 1.0));
                    tlasColor = mix(tlasColor, grad, w);
                    tlasWire = max(tlasWire, w);
                }
            }
        }
        // BLAS overlay: draw the root node of each mesh's BLAS for clear debugging
        for (int i = 0; i < bvhInstances.length(); ++i) {
            int rootIdx = bvhInstances[i].blasNodeOffset;
            BVHNode node = blasNodes[rootIdx];
            float w = aabbWireframe(node.boundsMin, node.boundsMax,
                                    camera.projectionMatrix * camera.viewMatrix * bvhInstances[i].transform,
                                    fragCoord, 2.0);
            if (w > 0.0) {
                vec3 black = vec3(0.0);
                blasColor = mix(blasColor, black, w);
                blasWire = max(blasWire, w);
            }
        }
    } else if (debugBVHMode == 1) {
        // BLAS mode: draw only the BVH branch for the selected triangle in the selected BLAS
        int selectedBLAS = debugSelectedBLAS;
        int selectedTri = debugSelectedTri;
        if (selectedBLAS < 0 || selectedBLAS >= bvhInstances.length()) return;
        int nodeOffset = bvhInstances[selectedBLAS].blasNodeOffset;
        int triOffset = bvhInstances[selectedBLAS].blasTriOffset;
        int nodeCount = 0;
        if (selectedBLAS + 1 < bvhInstances.length()) {
            nodeCount = bvhInstances[selectedBLAS + 1].blasNodeOffset - nodeOffset;
        } else {
            nodeCount = blasNodes.length() - nodeOffset;
        }
        int path[32]; int pathLen = 0;
        findBVHBranchIterative(nodeOffset, triOffset, nodeCount, selectedTri, path, pathLen);
        // Fetch the instance transform for this BLAS
        mat4 instanceTransform = bvhInstances[selectedBLAS].transform;
        mat4 viewProj = camera.projectionMatrix * camera.viewMatrix;
        for (int i = 0; i < pathLen; ++i) {
            BVHNode node = blasNodes[nodeOffset + path[i]];
            float wire = aabbWireframe(node.boundsMin, node.boundsMax, viewProj * instanceTransform, fragCoord, 2.0);
            blasWire = max(blasWire, wire);
            float hue = float(i) / float(pathLen);
            blasColor = mix(blasColor, hsv2rgb(vec3(hue, 1.0, 1.0)), wire);
        }
    }
}

//------------------------------------------------------------------------------
// Intersection functions for TLAS/BLAS
//------------------------------------------------------------------------------

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

// Möller–Trumbore algorithm for triangle intersection
bool hitTriangle(Triangle tri, Ray ray, out float tHit, out vec3 hitPoint, out vec3 normal, out int materialIndex, out vec2 uv) {
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

// Traverse BLAS for a mesh instance
bool traverseBLAS(Ray ray, int blasNodeOffset, int blasTriOffset, int globalTriOffset, out float tHit, out vec3 hitPoint, out vec3 normal, out int materialIndex, out vec2 uv) {
    tHit = 1e30;
    bool hit = false;
    int stack[64];
    int stackPtr = 0;
    stack[stackPtr++] = 0; // root node of this BLAS
    vec3 invDir = 1.0 / ray.direction;
    while (stackPtr > 0) {
        int nidx = stack[--stackPtr];
        BVHNode node = blasNodes[blasNodeOffset + nidx];
        float tmin, tmax;
        if (!intersectAABB(ray.origin, invDir, node.boundsMin, node.boundsMax, tmin, tmax) || tmin > tHit)
            continue;
        if (node.count > 0) { // leaf
            for (int i = 0; i < node.count; ++i) {
                int triIdx = globalTriOffset + blasTriIndices[blasTriOffset + node.leftFirst + i];
                float t;
                vec3 tempHit, tempNormal;
                int tempMat;
                vec2 tempUV;
                if (hitTriangle(triangles[triIdx], ray, t, tempHit, tempNormal, tempMat, tempUV)) {
                    if (t < tHit) {
                        tHit = t;
                        hitPoint = tempHit;
                        normal = tempNormal;
                        materialIndex = tempMat;
                        uv = tempUV;
                        hit = true;
                    }
                }
            }
        } else { // internal
            int leftIdx = node.leftFirst;
            int rightIdx = node.leftFirst + 1;
            BVHNode leftNode = blasNodes[blasNodeOffset + leftIdx];
            BVHNode rightNode = blasNodes[blasNodeOffset + rightIdx];

            float leftTMin, leftTMax;
            float rightTMin, rightTMax;
            bool hitLeft = intersectAABB(ray.origin, invDir, leftNode.boundsMin, leftNode.boundsMax, leftTMin, leftTMax) && leftTMin <= tHit;
            bool hitRight = intersectAABB(ray.origin, invDir, rightNode.boundsMin, rightNode.boundsMax, rightTMin, rightTMax) && rightTMin <= tHit;

            if (hitLeft && hitRight) {
                if (leftTMin < rightTMin) {
                    stack[stackPtr++] = rightIdx;
                    stack[stackPtr++] = leftIdx;
                } else {
                    stack[stackPtr++] = leftIdx;
                    stack[stackPtr++] = rightIdx;
                }
            } else if (hitLeft) {
                stack[stackPtr++] = leftIdx;
            } else if (hitRight) {
                stack[stackPtr++] = rightIdx;
            }
        }
    }
    return hit;
}

// Traverse TLAS, return closest hit and mesh instance index
bool traverseTLAS(Ray ray, out float tHit, out vec3 hitPoint, out vec3 normal, out int materialIndex, out int instanceIdx, out vec2 uv) {
    tHit = 1e30;
    if (tlasNodes.length() == 0) return false;
    bool hit = false;
    int stack[64];
    int stackPtr = 0;
    stack[stackPtr++] = 0; // root node
    vec3 invDir = 1.0 / ray.direction;
    while (stackPtr > 0) {
        int nidx = stack[--stackPtr];
        BVHNode node = tlasNodes[nidx];
        float tmin, tmax;
        if (!intersectAABB(ray.origin, invDir, node.boundsMin, node.boundsMax, tmin, tmax) || tmin > tHit)
            continue;
        if (node.count > 0) { // leaf: contains mesh instance indices
            for (int i = 0; i < node.count; ++i) {
                int instIdx = tlasTriIndices[node.leftFirst + i];
                BVHInstance inst = bvhInstances[instIdx];
                // Transform ray to mesh local space using inverse(instance.transform)
                mat4 invTransform = inst.inverseTransform;
                vec3 localOrigin = vec3(invTransform * vec4(ray.origin, 1.0));
                vec3 localDir = normalize(vec3(invTransform * vec4(ray.direction, 0.0)));
                Ray localRay = Ray(localOrigin, localDir);
                float tLocal;
                vec3 localHit, localNormal;
                int tempMat;
                vec2 tempUV;
                if (traverseBLAS(localRay, inst.blasNodeOffset, inst.blasTriOffset, inst.globalTriOffset, tLocal, localHit, localNormal, tempMat, tempUV)) {
                    // Transform hit point and normal back to world space
                    vec3 worldHit = vec3(inst.transform * vec4(localHit, 1.0));
                    float tWorld = length(worldHit - ray.origin); // world-space t
                    if (tWorld < tHit) {
                        tHit = tWorld;
                        hitPoint = worldHit;
                        mat3 normalMatrix = mat3(transpose(inst.inverseTransform));
                        normal = normalize(normalMatrix * localNormal);
                        materialIndex = tempMat;
                        instanceIdx = instIdx;
                        uv = tempUV;
                        hit = true;
                    }
                }
            }
        } else { // internal
            int leftIdx = node.leftFirst;
            int rightIdx = node.leftFirst + 1;
            BVHNode leftNode = tlasNodes[leftIdx];
            BVHNode rightNode = tlasNodes[rightIdx];

            float leftTMin, leftTMax;
            float rightTMin, rightTMax;
            bool hitLeft = intersectAABB(ray.origin, invDir, leftNode.boundsMin, leftNode.boundsMax, leftTMin, leftTMax) && leftTMin <= tHit;
            bool hitRight = intersectAABB(ray.origin, invDir, rightNode.boundsMin, rightNode.boundsMax, rightTMin, rightTMax) && rightTMin <= tHit;

            if (hitLeft && hitRight) {
                if (leftTMin < rightTMin) {
                    stack[stackPtr++] = rightIdx;
                    stack[stackPtr++] = leftIdx;
                } else {
                    stack[stackPtr++] = leftIdx;
                    stack[stackPtr++] = rightIdx;
                }
            } else if (hitLeft) {
                stack[stackPtr++] = leftIdx;
            } else if (hitRight) {
                stack[stackPtr++] = rightIdx;
            }
        }
    }
    return hit;
}

bool traverseTLAS(Ray ray, out float tHit, out vec3 hitPoint, out vec3 normal, out int materialIndex, out int instanceIdx) {
    vec2 uv;
    return traverseTLAS(ray, tHit, hitPoint, normal, materialIndex, instanceIdx, uv);
}

//------------------------------------------------------------------------------
// Lighting and material functions
//------------------------------------------------------------------------------
#include "material_sampling.glsl"

#define RAYZEN_FRAGMENT_BACKEND
#include "direct_lighting.glsl"

#include "ray_tracing.glsl"

//------------------------------------------------------------------------------
// Main path tracing loop
//------------------------------------------------------------------------------
void main() {
    vec2 uv = gl_FragCoord.xy / resolution;
    vec2 frameSeed = vec2(float(uniformFrameIndex) + 0.5, float(uniformFrameIndex) * 1.61803398875 + 0.5);
    vec2 seed = uv + frameSeed;

    vec3 radiance = vec3(0.0);
    int maxBounces = uniformShadingMode == 0 ? max(uniformBounceBudget, 1) : 1;
    int numSamples = 1; // increase for better quality

    // BVH debug overlay variables
    float bvhWire = 0.0;
    vec3 bvhWireColor = vec3(0.0);
    float tlasWire = 0.0, blasWire = 0.0;
    vec3 tlasColor = vec3(0.0), blasColor = vec3(0.0);

    if (debugShowBVH) {
        overlayBVHWireframe(gl_FragCoord.xy, tlasWire, tlasColor, blasWire, blasColor);
    }

    if (uniformShadingMode == 1) {
        seed = (uv + frameSeed) * float(gl_FragCoord.x + gl_FragCoord.y + 1.0);
        radiance = traceRealtime(calculateRay(uv, seed), seed);
    } else for (int samp = 0; samp < numSamples; ++samp) {
        seed = (uv + frameSeed) * float(gl_FragCoord.x + gl_FragCoord.y + samp + 1.0);
        Ray ray = calculateRay(uv, seed);
        vec3 currentOrigin = ray.origin;
        vec3 currentDirection = ray.direction;
        vec3 throughput = vec3(1.0);
        int mediumInstance = -1;
        int mediumMaterial = -1;
        vec3 mediumAlbedo = vec3(1.0);
        bool hitSomething = false;

        for (int bounce = 0; bounce < maxBounces; ++bounce) {
            vec2 tempseed = seed * float((bounce + 1) * (bounce + 1)) * 12793.46 + float(bounce) * 1423.34;
            vec3 hitPoint, hitNormal;
            int materialIndex = -1;
            int instanceIdx = -1;
            float closestT = 1e30;
            Material hitMaterial;
            vec2 hitUV;

            // Use TLAS/BLAS traversal for intersection
            bool found = traverseTLAS(Ray(currentOrigin, currentDirection), closestT, hitPoint, hitNormal, materialIndex, instanceIdx, hitUV);
            if (mediumInstance >= 0) {
                Material medium = materials[mediumMaterial];
                medium.albedo = mediumAlbedo;
                if (uniformSubsurfaceEnabled && medium.subsurfaceMeanFreePath > 0.0) {
                    float distance = -log(max(1.0 - rand(tempseed + vec2(63.1, 8.7)), 1e-6)) *
                                     medium.subsurfaceMeanFreePath;
                    if (distance < closestT) {
                        vec3 scatterPoint = currentOrigin + currentDirection * distance;
                        throughput *= clamp(medium.albedo, vec3(0.0), vec3(1.0));
                        radiance += throughput * subsurfaceDirectLight(scatterPoint, mediumInstance, medium);
                        if (bounce + 1 >= maxBounces) break;
                        currentOrigin = scatterPoint;
                        currentDirection = randomSphereDirection(tempseed);
                        continue;
                    }
                } else if (found) {
                    vec3 transmittance = clamp(medium.albedo * medium.transparency, vec3(0.0001), vec3(1.0));
                    throughput *= pow(transmittance, vec3(closestT));
                }
                if (!found || instanceIdx != mediumInstance) break;
            }
            if (!found) {
                // Skybox: blueish gradient, less bright
                float t = 0.5 * (normalize(currentDirection).y + 1.0);
                vec3 skyColor = mix(vec3(0.15, 0.25, 0.45), vec3(0.5, 0.7, 1.0), t); // deep blue to light blue
                radiance += throughput * skyColor;
                break;
            }
            hitSomething = true;
            hitMaterial = materials[materialIndex];
            applyBaseColorTexture(hitMaterial, hitUV);
            vec3 viewDir = normalize(-currentDirection);
            vec3 shadingNormal = dot(hitNormal, viewDir) >= 0.0 ? hitNormal : -hitNormal;
            if (mediumInstance < 0)
                radiance += throughput * calculateLighting(hitPoint, shadingNormal, hitMaterial, viewDir, bounce == 0);
            if (bounce + 1 >= maxBounces) break;

            float randVal = rand(tempseed + vec2(float(samp), float(bounce)));

            // Transparent / refractive handling
            if (hitMaterial.transparency > 0.0 ||
                (uniformSubsurfaceEnabled && hitMaterial.subsurfaceMeanFreePath > 0.0)) {
                bool entering = mediumInstance < 0;
                float ior = max(hitMaterial.ior, 0.001);
                if (sampleDielectric(currentDirection, shadingNormal, entering ? 1.0 : ior,
                                     entering ? ior : 1.0, randVal, currentDirection)) {
                    mediumInstance = entering ? instanceIdx : -1;
                    mediumMaterial = entering ? materialIndex : -1;
                    if (entering) mediumAlbedo = hitMaterial.albedo;
                }
            } else {
                vec3 weight;
                if (!sampleOpaque(hitMaterial, shadingNormal, viewDir, tempseed, currentDirection, weight)) break;
                throughput *= weight;
            }
            currentOrigin = offsetRayOrigin(hitPoint, hitNormal, currentDirection);

            // Russian roulette termination after a few bounces
            if (bounce > 2) {
                float p = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05, 0.95);
                if (rand(tempseed + vec2(31.73 + float(samp), 47.29 + float(bounce))) > p)
                    break;
                throughput /= p;
            }
        }
    }
    vec3 sampleRadiance = max(radiance / float(numSamples), vec3(0.0));
    sampleRadiance = clampRadiancePreserveHue(sampleRadiance, 8.0);

    vec3 accumulatedRadiance = sampleRadiance;
    if (uniformUseImageAccumulation) {
        ivec2 pixelCoord = clamp(ivec2(gl_FragCoord.xy), ivec2(0), imageSize(accumulationImage) - ivec2(1));
        if (!uniformResetAccumulation && uniformAccumulatedFrames > 0) {
            vec3 previousRadiance = imageLoad(accumulationImage, pixelCoord).rgb;
            float blend = 1.0 / (float(uniformAccumulatedFrames) + 1.0);
            accumulatedRadiance = mix(previousRadiance, sampleRadiance, blend);
        }
        imageStore(accumulationImage, pixelCoord, vec4(accumulatedRadiance, 1.0));
    }

    vec3 color = acesToneMapHue(accumulatedRadiance * uniformDisplayExposure);
    color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / 2.2));

    // Overlay BVH wireframe if enabled
    if (debugShowBVH && (tlasWire > 0.0 || blasWire > 0.0)) {
        color = mix(color, tlasColor, 0.5 * tlasWire);
        color = mix(color, blasColor, 0.5 * blasWire);
    }

    // Debug: Render light positions as screen-space markers
    if (debugShowLights) {
        for (int i = 0; i < numLights; ++i) {
            if (i >= lights.length()) break;
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
