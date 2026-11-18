#version 430 core

// RayZen display resolve pass.
// Samples the HDR accumulation texture written by the compute path tracer,
// applies exposure + ACES tone-map + gamma, and overlays debug primitives.

uniform vec2 resolution;
uniform float uniformDisplayExposure;
uniform sampler2D accumulationTex;

struct Camera {
    mat4 viewMatrix;
    mat4 projectionMatrix;
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

struct BVHNode {
    vec3 boundsMin;
    int leftFirst;
    vec3 boundsMax;
    int count;
};

struct BVHInstance {
    int blasNodeOffset;
    int blasTriOffset;
    int meshIndex;
    int globalTriOffset;
    mat4 transform;
    mat4 inverseTransform;
};

layout(std430, binding = 1) buffer MaterialBuffer  { Material materials[]; };
layout(std430, binding = 2) buffer LightBuffer     { Light    lights[]; };
layout(std430, binding = 5) buffer TLASNodeBuffer  { BVHNode  tlasNodes[]; };
layout(std430, binding = 6) buffer TLASTriIdxBuffer{ int      tlasTriIndices[]; };
layout(std430, binding = 7) buffer BLASNodeBuffer  { BVHNode  blasNodes[]; };
layout(std430, binding = 8) buffer BLASTriIdxBuffer{ int      blasTriIndices[]; };
layout(std430, binding = 9) buffer BVHInstanceBuffer { BVHInstance bvhInstances[]; };

uniform int  numLights;
uniform bool debugShowLights;
uniform bool debugShowBVH;
uniform int  debugBVHMode;
uniform int  debugSelectedBLAS;
uniform int  debugSelectedTri;
out vec4 FragColor;

vec3 acesToneMap(vec3 color) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

// Hue-preserving ACES: tone-maps via the max channel so all channels are
// scaled uniformly, avoiding the pinkish/desaturated look on saturated hues.
vec3 acesToneMapHue(vec3 color) {
    float maxC = max(color.r, max(color.g, color.b));
    if (maxC < 1e-6) return vec3(0.0);
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    float tonedMax = clamp((maxC * (a * maxC + b)) / (maxC * (c * maxC + d) + e), 0.0, 1.0);
    return clamp(color * (tonedMax / maxC), 0.0, 1.0);
}

vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

float distanceToSegment(vec2 p, vec2 a, vec2 b) {
    vec2 ab = b - a;
    float t = clamp(dot(p - a, ab) / dot(ab, ab), 0.0, 1.0);
    return length(p - (a + t * ab));
}

float aabbWireframe(vec3 bmin, vec3 bmax, mat4 viewProj, vec2 fragCoord, float thickness) {
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
        s0 *= resolution; s1 *= resolution;
        float d = distanceToSegment(fragCoord, s0, s1);
        minDist = min(minDist, d);
    }
    return minDist < thickness ? 1.0 : 0.0;
}

void findBVHBranchIterative(int nodeOffset, int triOffset, int nodeCount, int selectedTri,
                            out int path[32], out int pathLen) {
    int cur = 0;
    pathLen = 0;
    for (int depth = 0; depth < 32; ++depth) {
        path[pathLen++] = cur;
        BVHNode node = blasNodes[nodeOffset + cur];
        if (node.count > 0) {
            bool found = false;
            for (int i = 0; i < node.count; ++i) {
                int triIdx = blasTriIndices[triOffset + node.leftFirst + i];
                if (triIdx == selectedTri) { found = true; break; }
            }
            if (!found) pathLen = 0;
            break;
        } else {
            int leftIdx = node.leftFirst;
            int rightIdx = node.leftFirst + 1;
            bool foundInLeft = false;
            int stack[32]; int sp = 0;
            stack[sp++] = leftIdx;
            while (sp > 0) {
                int nidx = stack[--sp];
                if (nidx < 0 || nidx >= nodeCount) continue;
                BVHNode n = blasNodes[nodeOffset + nidx];
                if (n.count > 0) {
                    for (int j = 0; j < n.count; ++j) {
                        int triIdx = blasTriIndices[triOffset + n.leftFirst + j];
                        if (triIdx == selectedTri) { foundInLeft = true; break; }
                    }
                } else {
                    stack[sp++] = n.leftFirst;
                    stack[sp++] = n.leftFirst + 1;
                }
                if (foundInLeft) break;
            }
            cur = foundInLeft ? leftIdx : rightIdx;
        }
    }
}

void overlayBVHWireframe(vec2 fragCoord, out float tlasWire, out vec3 tlasColor,
                         out float blasWire, out vec3 blasColor) {
    tlasWire = 0.0; tlasColor = vec3(0.0);
    blasWire = 0.0; blasColor = vec3(0.0);
    mat4 viewProj = camera.projectionMatrix * camera.viewMatrix;
    if (debugBVHMode == 0) {
        int tlasNodeCount = tlasNodes.length();
        for (int i = 0; i < tlasNodeCount; ++i) {
            BVHNode node = tlasNodes[i];
            if (node.count > 0) {
                float w = aabbWireframe(node.boundsMin, node.boundsMax, viewProj, fragCoord, 1.5);
                if (w > 0.0) {
                    int meshIdx = tlasTriIndices[node.leftFirst];
                    if (meshIdx < 0 || meshIdx >= bvhInstances.length()) continue;
                    float t = float(meshIdx) / float(bvhInstances.length());
                    vec3 grad = hsv2rgb(vec3(0.0 + t * 0.5, 1.0, 1.0));
                    tlasColor = mix(tlasColor, grad, w);
                    tlasWire = max(tlasWire, w);
                }
            }
        }
        for (int i = 0; i < bvhInstances.length(); ++i) {
            int rootIdx = bvhInstances[i].blasNodeOffset;
            BVHNode node = blasNodes[rootIdx];
            float w = aabbWireframe(node.boundsMin, node.boundsMax,
                                    viewProj * bvhInstances[i].transform, fragCoord, 2.0);
            if (w > 0.0) {
                blasColor = mix(blasColor, vec3(0.0), w);
                blasWire = max(blasWire, w);
            }
        }
    } else {
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
        mat4 instanceTransform = bvhInstances[selectedBLAS].transform;
        for (int i = 0; i < pathLen; ++i) {
            BVHNode node = blasNodes[nodeOffset + path[i]];
            float wire = aabbWireframe(node.boundsMin, node.boundsMax, viewProj * instanceTransform, fragCoord, 2.0);
            blasWire = max(blasWire, wire);
            float hue = float(i) / float(pathLen);
            blasColor = mix(blasColor, hsv2rgb(vec3(hue, 1.0, 1.0)), wire);
        }
    }
}

void main() {
    vec2 uv = gl_FragCoord.xy / resolution;
    vec3 hdr = texture(accumulationTex, uv).rgb;
    vec3 color = acesToneMapHue(hdr * uniformDisplayExposure);
    color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / 2.2));

    if (debugShowBVH) {
        float tlasWire, blasWire;
        vec3 tlasColor, blasColor;
        overlayBVHWireframe(gl_FragCoord.xy, tlasWire, tlasColor, blasWire, blasColor);
        if (tlasWire > 0.0 || blasWire > 0.0) {
            color = mix(color, tlasColor, 0.5 * tlasWire);
            color = mix(color, blasColor, 0.5 * blasWire);
        }
    }

    if (debugShowLights) {
        for (int i = 0; i < numLights; ++i) {
            if (i >= lights.length()) break;
            Light light = lights[i];
            if (light.positionOrDirection.w == 1.0) {
                vec4 clip = camera.projectionMatrix * camera.viewMatrix * vec4(light.positionOrDirection.xyz, 1.0);
                if (clip.w > 0.0) {
                    vec3 ndc = clip.xyz / clip.w;
                    vec2 screen = (ndc.xy * 0.5 + 0.5) * resolution;
                    float dist = length(gl_FragCoord.xy - screen);
                    float markerRadius = 8.0;
                    if (dist < markerRadius) {
                        float alpha = smoothstep(markerRadius, markerRadius - 2.0, dist);
                        color = mix(color, light.color, alpha);
                    }
                }
            }
        }
    }

    FragColor = vec4(color, 1.0);
}
