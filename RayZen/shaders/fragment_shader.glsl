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
    int texturePattern;
    float padMaterial;
};

const int MATERIAL_TEX_NONE   = -1;
const int MATERIAL_TEX_GRASS  = 0;
const int MATERIAL_TEX_DIRT   = 1;
const int MATERIAL_TEX_STONE  = 2;
const int MATERIAL_TEX_LOG    = 3;
const int MATERIAL_TEX_LEAVES = 4;

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

uniform int numTriangles;
uniform bool debugShowLights;
uniform int numLights; // actual number of lights in SSBO
uniform bool debugShowBVH;
uniform int debugBVHMode; // 0 = TLAS, 1 = BLAS
uniform int debugSelectedBLAS; // mesh index
uniform int debugSelectedTri; // triangle index within mesh
uniform int uniformBounceBudget; // <=0 uses default bounce count

out vec4 FragColor;

// Constants
// Ray structure
struct Ray {
    vec3 origin;
    vec3 direction;
};

#ifndef RZ_STRIP_DEBUG_FEATURES
// FPS overlay font: 8x8 bitmap for digits 0-9 and '.' (11 chars)
// Each row is a byte, LSB is leftmost pixel
const int fontWidth = 8;
const int fontHeight = 8;
const int fontChars = 11; // 0-9 and '.'
const int fontData[11][8] = int[11][8](
    // '0'
    int[8](0x3C,0x66,0x6E,0x7E,0x76,0x66,0x3C,0x00),
    // '1'
    int[8](0x18,0x38,0x18,0x18,0x18,0x18,0x3C,0x00),
    // '2'
    int[8](0x3C,0x66,0x06,0x1C,0x30,0x66,0x7E,0x00),
    // '3'
    int[8](0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00),
    // '4'
    int[8](0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00),
    // '5'
    int[8](0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00),
    // '6'
    int[8](0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00),
    // '7'
    int[8](0x7E,0x66,0x0C,0x18,0x18,0x18,0x18,0x00),
    // '8'
    int[8](0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00),
    // '9'
    int[8](0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00),
    // '.'
    int[8](0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00)
);

// FPS uniform
uniform float uniformFps;

// Draw a single font character at pixel position (top-left), returns 1.0 if inside glyph, else 0.0
float drawFontChar(int charIdx, vec2 fragCoord, vec2 pos, float scale) {
    // Flip y for top-left origin
    vec2 rel = (fragCoord - pos) / scale;
    int x = int(rel.x);
    int y = fontHeight - 1 - int(rel.y); // flip y
    if (x < 0 || x >= fontWidth || y < 0 || y >= fontHeight) return 0.0;
    int row = fontData[charIdx][y];
    int mask = 1 << (fontWidth - 1 - x); // flip x for correct left-to-right
    return float((row & mask) != 0);
}

// Draw a string of up to 8 chars (digits and '.') at pos, returns color overlay
vec3 drawFpsString(vec2 fragCoord, vec2 pos, float scale, float fps, vec3 fg, vec3 bg) {
    // Always draw as 3 digits, decimal, 1 digit: e.g. 042.7
    int fpsInt = int(fps);
    int tenths = int(fract(fps) * 10.0);
    int hundreds = (fpsInt / 100) % 10;
    int tens = (fpsInt / 10) % 10;
    int ones = fpsInt % 10;
    int chars[5];
    chars[0] = hundreds;
    chars[1] = tens;
    chars[2] = ones;
    chars[3] = 10; // '.'
    chars[4] = tenths;
    vec3 col = bg;
    for (int i = 0; i < 5; ++i) {
        float glyph = drawFontChar(chars[i], fragCoord, pos + vec2(i * (fontWidth+1) * scale, 0.0), scale);
        col = mix(col, fg, glyph);
    }
    return col;
}
#else
const int fontWidth = 8;
const int fontHeight = 8;
uniform float uniformFps;
float drawFontChar(int, vec2, vec2, float) { return 0.0; }
vec3 drawFpsString(vec2 fragCoord, vec2 pos, float scale, float fps, vec3 fg, vec3 bg) {
    return bg;
}
#endif

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

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}

float hash31(vec3 p) {
    p = fract(p * vec3(443.8975, 441.423, 437.195));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

float valueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float fbm(vec2 p) {
    float amplitude = 0.5;
    float value = 0.0;
    for (int i = 0; i < 4; ++i) {
        value += valueNoise(p) * amplitude;
        p *= 2.37;
        amplitude *= 0.5;
    }
    return value;
}

float valueNoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    float n000 = hash31(i);
    float n100 = hash31(i + vec3(1.0, 0.0, 0.0));
    float n010 = hash31(i + vec3(0.0, 1.0, 0.0));
    float n110 = hash31(i + vec3(1.0, 1.0, 0.0));
    float n001 = hash31(i + vec3(0.0, 0.0, 1.0));
    float n101 = hash31(i + vec3(1.0, 0.0, 1.0));
    float n011 = hash31(i + vec3(0.0, 1.0, 1.0));
    float n111 = hash31(i + vec3(1.0, 1.0, 1.0));
    vec3 u = f * f * (3.0 - 2.0 * f);
    float nx00 = mix(n000, n100, u.x);
    float nx10 = mix(n010, n110, u.x);
    float nx01 = mix(n001, n101, u.x);
    float nx11 = mix(n011, n111, u.x);
    float nxy0 = mix(nx00, nx10, u.y);
    float nxy1 = mix(nx01, nx11, u.y);
    return mix(nxy0, nxy1, u.z);
}

float fbm(vec3 p) {
    float amplitude = 0.5;
    float value = 0.0;
    for (int i = 0; i < 4; ++i) {
        value += valueNoise(p) * amplitude;
        p *= 2.437;
        amplitude *= 0.5;
    }
    return value;
}

vec2 repeatUV(vec2 uv, float scale) {
    vec2 tiled = uv * scale;
    return tiled;
}

vec3 sampleBlockPattern(int pattern, vec2 uv, int axis, vec3 worldPos, vec3 normal, vec3 baseColor) {
    float localY = fract(worldPos.y + 0.001);
    if (pattern == MATERIAL_TEX_GRASS) {
        if (axis == 1 && normal.y > -0.2) {
            vec2 topUV = repeatUV(uv, 3.2);
            float broad = fbm(topUV * 3.0);
            float fine = valueNoise(topUV * 12.0 + float(axis) * 4.0);
            vec3 shadow = vec3(0.28, 0.50, 0.22);
            vec3 light = vec3(0.48, 0.78, 0.35);
            vec3 color = mix(shadow, light, broad);
            color *= 0.9 + 0.2 * fine;
            if (normal.y < 0.0) {
                color *= 0.6;
            }
            return color;
        } else {
            vec3 dirt = vec3(0.36, 0.25, 0.13);
            vec3 moss = vec3(0.31, 0.52, 0.24);
            float blend = smoothstep(0.35, 0.92, localY);
            float mossNoise = fbm(worldPos * vec3(0.7, 0.9, 0.7) + float(axis) * 3.3);
            vec3 color = mix(dirt, moss, blend);
            color *= 0.85 + 0.25 * mossNoise;
            return color;
        }
    }
    if (pattern == MATERIAL_TEX_DIRT) {
        vec2 tiled = repeatUV(uv, 2.5);
        float clumps = fbm(tiled * 2.7 + worldPos.y);
        float stones = smoothstep(0.65, 0.95, valueNoise(tiled * 4.5));
        vec3 base = vec3(0.34, 0.24, 0.15);
        vec3 pebble = vec3(0.52, 0.42, 0.26);
        vec3 color = mix(base, pebble, stones * 0.6);
        color *= 0.85 + 0.25 * clumps;
        return color;
    }
    if (pattern == MATERIAL_TEX_STONE) {
        float strata = fbm(worldPos * 2.2 + float(axis) * 7.7);
        float veins = fbm(worldPos * 6.0 + vec3(11.0, 3.0, 5.0));
        vec3 base = mix(vec3(0.40, 0.42, 0.46), vec3(0.64, 0.66, 0.69), strata);
        base -= veins * 0.08;
        base *= 0.9 + 0.12 * valueNoise(worldPos.xz * 5.5);
        return base;
    }
    if (pattern == MATERIAL_TEX_LOG) {
        if (axis == 1) {
            vec2 local = fract(uv) - 0.5;
            float radius = length(local);
            float rings = fract(radius * 18.0 + fbm(local * 24.0));
            float cracks = fbm(local * 42.0 + worldPos.y);
            vec3 inner = vec3(0.52, 0.33, 0.18);
            vec3 outer = vec3(0.30, 0.19, 0.10);
            vec3 color = mix(inner, outer, smoothstep(0.15, 0.85, rings));
            color += vec3(0.03, 0.02, 0.015) * cracks;
            if (normal.y < 0.0) color *= 0.75;
            return color;
        } else {
            vec2 barkUV = vec2(worldPos.y, uv.y);
            barkUV *= vec2(3.5, 1.8);
            float grooves = sin(barkUV.x * 6.28318) * 0.5 + 0.5;
            float cracks = fbm(vec2(barkUV.x * 1.3, barkUV.y * 4.0));
            vec3 light = vec3(0.42, 0.27, 0.13);
            vec3 dark = vec3(0.25, 0.16, 0.08);
            vec3 color = mix(light, dark, grooves * 0.7 + cracks * 0.3);
            color *= 0.9 + 0.12 * fbm(worldPos.xz * vec2(1.5, 2.4));
            return color;
        }
    }
    if (pattern == MATERIAL_TEX_LEAVES) {
        float broad = fbm(worldPos * 3.2 + float(axis));
        float veins = fbm(worldPos * 6.5 + vec3(7.7, 2.1, 5.5));
        vec3 deep = vec3(0.18, 0.30, 0.15);
        vec3 bright = vec3(0.35, 0.58, 0.29);
        vec3 color = mix(deep, bright, broad);
        color += vec3(0.05, 0.07, 0.03) * veins;
        return color;
    }
    return baseColor;
}

vec3 blockTextureColor(int pattern, vec3 worldPos, vec3 normal, vec3 baseColor) {
    if (pattern == MATERIAL_TEX_NONE) {
        return baseColor;
    }
    vec3 n = normalize(normal);
    vec3 blend = pow(abs(n), vec3(8.0));
    blend = max(blend, vec3(1e-4));
    float sum = blend.x + blend.y + blend.z;
    blend /= sum;

    vec3 axisX = sampleBlockPattern(pattern, worldPos.yz, 0, worldPos, normal, baseColor);
    vec3 axisY = sampleBlockPattern(pattern, worldPos.xz, 1, worldPos, normal, baseColor);
    vec3 axisZ = sampleBlockPattern(pattern, worldPos.xy, 2, worldPos, normal, baseColor);
    return axisX * blend.x + axisY * blend.y + axisZ * blend.z;
}

vec3 evaluateMaterialAlbedo(Material material, vec3 worldPos, vec3 normal) {
    return blockTextureColor(material.texturePattern, worldPos, normal, material.albedo);
}

vec3 primarySunDirection() {
    for (int i = 0; i < numLights; ++i) {
        if (i >= lights.length()) break;
        Light light = lights[i];
        if (light.positionOrDirection.w == 0.0) {
            vec3 dir = light.positionOrDirection.xyz;
            float len2 = dot(dir, dir);
            if (len2 > 1e-6) {
                return dir / sqrt(len2);
            }
        }
    }
    return normalize(vec3(0.4, 0.85, 0.3));
}

vec3 sampleSkybox(vec3 dir) {
    dir = normalize(dir);
    float up = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);

    vec3 horizon = vec3(0.88, 0.82, 0.74);
    vec3 zenith = vec3(0.30, 0.48, 0.85);
    vec3 ground = vec3(0.10, 0.11, 0.12);
    vec3 baseSky = mix(horizon, zenith, pow(up, 1.2));
    vec3 base = mix(ground, baseSky, smoothstep(-0.1, 0.95, dir.y));

    vec3 sunDirection = primarySunDirection();
    float sunDot = max(dot(dir, sunDirection), 0.0);
    float sunCore = pow(sunDot, 1800.0);
    float sunHalo = pow(sunDot, 5.0);
    vec3 sunColor = vec3(1.0, 0.98, 0.9) * (sunCore * 45.0 + sunHalo * 0.45);

    vec3 cloudPos = dir * vec3(4.0, 2.0, 4.0) + vec3(10.0, 25.0, 5.0);
    float cloudLow = fbm(cloudPos);
    float cloudHigh = fbm(cloudPos * 2.7 + vec3(0.3, 0.6, 0.2));
    float cloudField = mix(cloudLow, cloudHigh, 0.55);
    float clouds = smoothstep(0.48, 0.65, cloudField);
    clouds *= smoothstep(0.0, 1.0, up);
    float feather = pow(clouds, 1.3);
    vec3 cloudColor = mix(vec3(1.0, 0.99, 0.96), vec3(0.95, 0.97, 1.0), up * 0.6);
    base = mix(base, cloudColor, feather * 0.5);

    base += sunColor;
    base = mix(base, vec3(1.0), 0.05);
    return clamp(base, 0.0, 4.0);
}

vec3 hemisphereAmbient(vec3 normal) {
    vec3 groundColor = vec3(0.32, 0.27, 0.22);
    float up = clamp(normal.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 envDir = normal.y >= 0.0 ? normal : normalize(vec3(normal.x, 0.05, normal.z));
    vec3 sky = sampleSkybox(envDir);
    return mix(groundColor, sky, up);
}

vec3 applyHeightFog(vec3 incomingColor, vec3 rayDir, float distanceToSurface, vec3 hitPoint) {
    float travel = max(distanceToSurface, 0.0);
    float fogStart = 35.0;
    float fogFull = 220.0;
    float fogTravel = max(travel - fogStart, 0.0);
    float distFactor = fogFull > fogStart ? clamp(fogTravel / (fogFull - fogStart), 0.0, 1.0) : 1.0;
    float heightFactor = clamp((hitPoint.y + 8.0) / 120.0, 0.0, 1.0);
    float density = mix(0.009, 0.0025, heightFactor);
    float fog = 1.0 - exp(-fogTravel * density);
    fog *= distFactor;
    float horizon = smoothstep(-0.35, 0.45, rayDir.y);
    vec3 fogHorizon = vec3(0.42, 0.52, 0.66);
    vec3 fogSky = sampleSkybox(rayDir);
    vec3 fogColor = mix(fogHorizon, fogSky, horizon);
    fogColor = mix(fogColor, vec3(0.36, 0.40, 0.48), clamp(0.4 - rayDir.y, 0.0, 1.0) * 0.35);
    return mix(incomingColor, fogColor, clamp(fog, 0.0, 1.0));
}

vec3 toneMapACES(vec3 color) {
    vec3 numerator = color * (2.51 * color + 0.03);
    vec3 denominator = color * (2.43 * color + 0.59) + 0.14;
    return clamp(numerator / denominator, 0.0, 1.0);
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

#ifndef RZ_STRIP_DEBUG_FEATURES
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
            float w = aabbWireframe(node.boundsMin, node.boundsMax, camera.projectionMatrix * camera.viewMatrix, fragCoord, 2.0);
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
            // Transform AABB corners to world space using instanceTransform
            vec3 bmin = (instanceTransform * vec4(node.boundsMin, 1.0)).xyz;
            vec3 bmax = (instanceTransform * vec4(node.boundsMax, 1.0)).xyz;
            float wire = aabbWireframe(bmin, bmax, viewProj, fragCoord, 2.0);
            blasWire = max(blasWire, wire);
            float hue = float(i) / float(pathLen);
            blasColor = mix(blasColor, hsv2rgb(vec3(hue, 1.0, 1.0)), wire);
        }
    }
}
#else
vec3 hsv2rgb(vec3 c) { return vec3(0.0); }
void overlayBVHWireframe(vec2, out float tlasWire, out vec3 tlasColor, out float blasWire, out vec3 blasColor) {
    tlasWire = 0.0;
    tlasColor = vec3(0.0);
    blasWire = 0.0;
    blasColor = vec3(0.0);
}
#endif

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

// Traverse BLAS for a mesh instance
bool traverseBLAS(Ray ray, int blasNodeOffset, int blasTriOffset, int globalTriOffset, out float tHit, out vec3 hitPoint, out vec3 normal, out int materialIndex) {    
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

// Traverse TLAS, return closest hit and mesh instance index
bool traverseTLAS(Ray ray, out float tHit, out vec3 hitPoint, out vec3 normal, out int materialIndex, out int instanceIdx) {
    tHit = 1e30;
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
                if (traverseBLAS(localRay, inst.blasNodeOffset, inst.blasTriOffset, inst.globalTriOffset, tLocal, localHit, localNormal, tempMat)) {
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

// Shadow test: cast a ray and check for occlusion using TLAS/BLAS
// Transparent-aware shadow query: accumulates transmission through multiple transparent objects
bool shadowVisibility(vec3 origin, vec3 dir, float maxDist, out float visibility) {
    visibility = 1.0;
    float traveled = 0.0;
    const float EPS = 0.001;
    for (int iter = 0; iter < 32 && visibility > 0.05; ++iter) {
        float tHit; vec3 hp, n; int matIdx; int inst;
        if (!traverseTLAS(Ray(origin, dir), tHit, hp, n, matIdx, inst)) return true; // no more hits
        if (tHit < EPS) { origin += dir * EPS; continue; }
        traveled += tHit;
        if (traveled >= maxDist) return true; // reached light
        Material m = materials[matIdx];
        if (m.transparency > 0.0) {
            visibility *= m.transparency;
            origin = hp + dir * EPS; // continue through
            continue;
        } else {
            visibility = 0.0; // opaque blocker
            return false;
        }
    }
    return visibility > 0.05;
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

bool refractDir(vec3 incident, vec3 normal, float eta, out vec3 refr) {
    // incident assumed normalized, normal points against incident when entering
    float cosi = clamp(dot(-incident, normal), -1.0, 1.0);
    float sint2 = max(0.0, 1.0 - cosi * cosi);
    float eta2 = eta * eta;
    float k = 1.0 - eta2 * sint2;
    if (k < 0.0) return false; // total internal reflection
    refr = normalize(eta * incident + (eta * cosi - sqrt(k)) * normal);
    return true;
}

vec3 calculateLighting(vec3 hitPoint, vec3 normal, Material material, vec3 viewDir) {
    // For transparent dielectrics: compute only specular reflection lobe (no diffuse)
    if (material.transparency > 0.0) {
        vec3 F0 = vec3(pow((1.0 - material.ior) / (1.0 + material.ior), 2.0));
        vec3 specAccum = vec3(0.0);
        for (int i = 0; i < numLights; ++i) {
            if (i >= lights.length()) break;
            Light light = lights[i];
            vec3 L; float attenuation; float visibility;
            if (light.positionOrDirection.w == 1.0) {
                vec3 lv = light.positionOrDirection.xyz - hitPoint;
                float dist = max(length(lv), 0.001);
                L = lv / dist;
                attenuation = light.power / (dist * dist);
                if (!shadowVisibility(hitPoint + L * 0.001, L, dist, visibility)) continue;
            } else {
                L = normalize(light.positionOrDirection.xyz);
                attenuation = light.power;
                if (!shadowVisibility(hitPoint + L * 0.001, L, 1e30, visibility)) continue;
            }
            attenuation *= visibility;
            float NdotL = max(dot(normal, L), 0.0);
            if (NdotL <= 0.0) continue;
            vec3 H = normalize(L + viewDir);
            float NdotH = max(dot(normal, H), 0.0);
            float cosTheta = max(dot(H, viewDir), 0.0);
            vec3 F = fresnelSchlick(cosTheta, F0);
            float rough = max(material.roughness, 0.02);
            float a = rough * rough;
            float a2 = a * a;
            float dDen = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
            float D = a2 / (3.14159 * dDen * dDen + 1e-6);
            float k = (rough + 1.0)*(rough + 1.0)/8.0;
            float NdotV = max(dot(normal, viewDir), 0.0);
            float Gv = NdotV / (NdotV * (1.0 - k) + k + 1e-6);
            float Gl = NdotL / (NdotL * (1.0 - k) + k + 1e-6);
            float denom = max(4.0 * NdotL * NdotV, 1e-4);
            vec3 spec = (F * D * Gv * Gl) / denom;
            specAccum += spec * light.color * attenuation * NdotL;
        }
        return specAccum;
    }
    vec3 F0 = mix(vec3(0.02), material.albedo, material.metallic);
    vec3 ambientIrradiance = hemisphereAmbient(normal) * 0.35;
    vec3 finalColor = ambientIrradiance * material.albedo;
    vec3 sunDir = primarySunDirection();
    float sunWrap = pow(max(dot(normal, sunDir), 0.0), 3.5);
    float roughResponse = pow(max(1.0 - clamp(material.roughness, 0.0, 1.0), 0.0), 1.4);
    vec3 sunBounce = vec3(1.0, 0.92, 0.75) * sunWrap * (0.18 * roughResponse);
    finalColor += sunBounce * material.albedo;

    // Loop over lights (assumed 10 lights maximum)
    for (int i = 0; i < numLights; ++i) {
        if (i >= lights.length()) break; // safety
        Light light = lights[i];
        vec3 lightDir;
        float attenuation = 1.0;
        float visibility;

        if (light.positionOrDirection.w == 1.0) { // Point light
            vec3 lightVec = light.positionOrDirection.xyz - hitPoint;
            float distance = max(length(lightVec), 0.001);
            lightDir = normalize(lightVec);
            attenuation = light.power / (distance * distance);

            // Check shadows for point lights
            if (!shadowVisibility(hitPoint + lightDir * 0.001, lightDir, distance, visibility)) continue;
        } else { // Directional light
            lightDir = normalize(light.positionOrDirection.xyz);
            attenuation = light.power;

            if (!shadowVisibility(hitPoint + lightDir * 0.001, lightDir, 1e30, visibility)) continue;
        }
        attenuation *= visibility;

        // Calculate shading terms
        vec3 halfwayDir = normalize(lightDir + viewDir);
        float NdotL = max(dot(normal, lightDir), 0.0);
        float NdotV = max(dot(normal, viewDir), 0.0);

        vec3 F = fresnelSchlick(max(dot(halfwayDir, viewDir), 0.0), F0);

        float perceptualRoughness = clamp(material.roughness, 0.045, 1.0);
        float alpha = perceptualRoughness * perceptualRoughness;
        float alpha2 = alpha * alpha;
        float denom = (dot(normal, halfwayDir) * dot(normal, halfwayDir) * (alpha2 - 1.0) + 1.0);
        float D = alpha2 / (3.14159 * denom * denom);

        float k = (material.roughness + 1.0) * (material.roughness + 1.0) / 8.0;
        float G = NdotV / (NdotV * (1.0 - k) + k);
        G *= NdotL / (NdotL * (1.0 - k) + k);

        float denomSpec = max(4.0 * NdotV * NdotL, 0.0001);
        vec3 specular = (F * D * G) / denomSpec;

        float roughEnergyLoss = pow(clamp(1.0 - perceptualRoughness, 0.0, 1.0), 4.5);
        float dielectricSpec = material.reflectivity * roughEnergyLoss;
        float specScale = mix(dielectricSpec, 1.0, material.metallic);
        specular *= specScale;

        float diffuseScale = (1.0 - material.metallic) * (1.0 - min(specScale * 1.1, 0.95));
        vec3 diffuse = diffuseScale * (1.0 - F) * material.albedo * NdotL / 3.14159;

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
    int maxBounces = uniformBounceBudget > 0 ? uniformBounceBudget : 5;
    float currentIor = 1.0; // track medium IOR (air)
    int numSamples = 1; // increase for better quality

    // BVH debug overlay variables
    float bvhWire = 0.0;
    vec3 bvhWireColor = vec3(0.0);
    float tlasWire = 0.0, blasWire = 0.0;
    vec3 tlasColor = vec3(0.0), blasColor = vec3(0.0);

    if (debugShowBVH) {
        overlayBVHWireframe(gl_FragCoord.xy, tlasWire, tlasColor, blasWire, blasColor);
    }

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
            int instanceIdx = -1;
            float closestT = 1e30;
            Material hitMaterial;

            // Use TLAS/BLAS traversal for intersection
            bool found = traverseTLAS(Ray(currentOrigin, currentDirection), closestT, hitPoint, hitNormal, materialIndex, instanceIdx);
            if (!found) {
                vec3 skyColor = sampleSkybox(currentDirection);
                color += throughput * skyColor;
                break;
            }
            hitSomething = true;
            hitMaterial = materials[materialIndex];
            hitMaterial.albedo = evaluateMaterialAlbedo(hitMaterial, hitPoint, hitNormal);
            vec3 viewDir = normalize(camera.position - hitPoint);
            // Only add explicit direct lighting on first bounce to avoid energy blow-up without MIS
            if (bounce == 0) {
                vec3 direct = calculateLighting(hitPoint, hitNormal, hitMaterial, viewDir);
                direct = applyHeightFog(direct, currentDirection, closestT, hitPoint);
                color += throughput * direct;
            }

            float randVal = rand(tempseed + vec2(float(samp), float(bounce)));

            // Transparent / refractive handling
            if (hitMaterial.transparency > 0.0) {
                // Deterministic dielectric: always refract (unless TIR) and rely on direct spec for reflection.
                bool entering = dot(-currentDirection, hitNormal) > 0.0;
                vec3 N = entering ? hitNormal : -hitNormal; // outward normal relative to current medium
                float extIor = currentIor;
                float nextIor = entering ? hitMaterial.ior : 1.0;
                float eta = extIor / nextIor;
                float cosi = clamp(dot(-currentDirection, N), 0.0, 1.0);
                float F0 = pow((extIor - nextIor) / (extIor + nextIor), 2.0);
                float fresnel = F0 + (1.0 - F0) * pow(1.0 - cosi, 5.0);
                vec3 refr;
                bool ok = refractDir(currentDirection, N, eta, refr);
                if (!ok) { // Total internal reflection: fallback reflect only
                    currentDirection = reflectRay(currentDirection, N);
                    // Slight energy trim
                    throughput *= vec3(0.98);
                } else {
                    // Apply transmission weighting; leave reflection handled by spec direct term
                    currentDirection = refr;
                    currentIor = nextIor;
                    vec3 tint = mix(vec3(1.0), hitMaterial.albedo, hitMaterial.transparency);
                    vec3 transmitWeight = tint * hitMaterial.transparency * (1.0 - fresnel);
                    throughput *= clamp(transmitWeight, vec3(0.0), vec3(1.0));
                }
                // Advanced: could add chromatic dispersion here by varying IOR per channel
            } else {
                // Opaque: choose glossy vs diffuse based on reflectivity and roughness
                float glossyProbBase = hitMaterial.reflectivity * pow(clamp(1.0 - hitMaterial.roughness, 0.0, 1.0), 4.0);
                float glossyProb = mix(glossyProbBase, hitMaterial.reflectivity, hitMaterial.metallic);
                glossyProb = clamp(glossyProb, 0.0, 0.95);
                if (randVal < glossyProb) {
                    currentDirection = reflectRay(currentDirection, hitNormal);
                    throughput *= vec3(0.9);
                } else {
                    currentDirection = randomHemisphereDirection(hitNormal, tempseed);
                    float bounceAtten = 0.3 + 0.35 * hitMaterial.roughness;
                    throughput *= hitMaterial.albedo * bounceAtten * (1.0 - glossyProb);
                }
            }
            // Offset origin based on direction vs surface normal to avoid self-intersections, especially exiting glass
            vec3 offsetNormal = hitNormal;
            float pushDir = dot(currentDirection, hitNormal) > 0.0 ? 1.0 : -1.0;
            currentOrigin = hitPoint + offsetNormal * pushDir * 0.003;

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
    color = toneMapACES(color);
    color = pow(color, vec3(1.0/2.2));
    color = clamp(color, 0.0, 1.0);

    // Overlay BVH wireframe if enabled
    if (debugShowBVH && (tlasWire > 0.0 || blasWire > 0.0)) {
        color = mix(color, tlasColor, 0.5 * tlasWire);
        color = mix(color, blasColor, 0.5 * blasWire);
    }

    // Debug: Render light positions as screen-space markers
#ifndef RZ_STRIP_DEBUG_FEATURES
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

    // FPS overlay (top-left, white text, black bg)
    float margin = 8.0;
    vec2 fpsPos = vec2(margin, resolution.y - margin - fontHeight * 2.0); // top-left
    float fpsScale = 2.0; // 2x font size
    vec3 fpsCol = drawFpsString(gl_FragCoord.xy, fpsPos, fpsScale, uniformFps, vec3(1.0), color);
    float anyFps = 0.0;
    for (int y = 0; y < fontHeight; ++y) {
        for (int x = 0; x < fontWidth * 6; ++x) {
            vec2 p = fpsPos + vec2(x, y) * fpsScale;
            if (abs(gl_FragCoord.x - p.x) < 1.0 && abs(gl_FragCoord.y - p.y) < 1.0) {
                anyFps = 1.0;
            }
        }
    }
    color = mix(color, fpsCol, anyFps);
#endif

    FragColor = vec4(color, 1.0);
}
