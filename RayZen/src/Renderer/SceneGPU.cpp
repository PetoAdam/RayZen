#include "Renderer/SceneGPU.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include "Logger.h"

namespace rayzen {

namespace {

namespace fs = std::filesystem;

template <typename T>
bool saveVec(const fs::path& path, const std::vector<T>& vec) {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return false;
    size_t n = vec.size();
    ofs.write(reinterpret_cast<const char*>(&n), sizeof(n));
    ofs.write(reinterpret_cast<const char*>(vec.data()), sizeof(T) * n);
    return ofs.good();
}

template <typename T>
bool loadVec(const fs::path& path, std::vector<T>& vec) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    std::error_code ec;
    const auto bytes = fs::file_size(path, ec);
    if (ec || bytes < sizeof(size_t) || bytes > 512ull * 1024 * 1024) return false;
    size_t n = 0;
    ifs.read(reinterpret_cast<char*>(&n), sizeof(n));
    if (!ifs || n > (bytes - sizeof(size_t)) / sizeof(T) ||
        sizeof(size_t) + n * sizeof(T) != bytes) return false;
    std::vector<T> loaded(n);
    ifs.read(reinterpret_cast<char*>(loaded.data()), sizeof(T) * n);
    if (!ifs) return false;
    vec = std::move(loaded);
    return true;
}

bool saveBVHFiles(const fs::path& base, const BVH& bvh) {
    return saveVec(base.string() + ".nodes.bin", bvh.nodes) &&
           saveVec(base.string() + ".tris.bin", bvh.triIndices);
}

bool loadBVHFiles(const fs::path& base, BVH& bvh) {
    return loadVec(base.string() + ".nodes.bin", bvh.nodes) &&
           loadVec(base.string() + ".tris.bin", bvh.triIndices);
}

uint64_t mixFingerprintBytes(uint64_t hash, const void* data, size_t size) {
    const uint64_t prime = 1099511628211ull;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= prime;
    }
    return hash;
}

template <typename T>
uint64_t vectorFingerprint(const std::vector<T>& vec) {
    uint64_t hash = 1469598103934665603ull;
    return mixFingerprintBytes(hash, vec.data(), vec.size() * sizeof(T));
}

struct SceneCacheManifest {
    uint64_t scene = 0;
    uint64_t triangles = 0;
    uint64_t blasNodes = 0;
    uint64_t blasIndices = 0;
    uint64_t instances = 0;
    uint64_t tlasNodes = 0;
    uint64_t tlasIndices = 0;
};

struct MeshCacheManifest {
    uint64_t mesh = 0;
    uint64_t nodes = 0;
    uint64_t indices = 0;
};

uint64_t computeMeshFingerprint(const Mesh& mesh) {
    uint64_t hash = 1469598103934665603ull;
    const size_t triangleCount = mesh.triangles.size();
    hash = mixFingerprintBytes(hash, &triangleCount, sizeof(triangleCount));
    for (const auto& triangle : mesh.triangles) {
        for (const glm::vec3* v : {&triangle.v0, &triangle.v1, &triangle.v2}) {
            for (int c = 0; c < 3; ++c) hash = mixFingerprintBytes(hash, &(*v)[c], sizeof(float));
        }
        hash = mixFingerprintBytes(hash, &triangle.materialIndex, sizeof(triangle.materialIndex));
        hash = mixFingerprintBytes(hash, &triangle.hasUV, sizeof(triangle.hasUV));
        for (const glm::vec2* uv : {&triangle.uv0, &triangle.uv1, &triangle.uv2})
            for (int c = 0; c < 2; ++c) hash = mixFingerprintBytes(hash, &(*uv)[c], sizeof(float));
    }
    return hash;
}

uint64_t computeSceneFingerprint(const Scene& scene) {
    uint64_t hash = 1469598103934665603ull;
    for (const auto& object : scene.gameObjects) {
        const uint64_t meshHash = object.mesh ? computeMeshFingerprint(*object.mesh) : 0;
        hash = mixFingerprintBytes(hash, &meshHash, sizeof(meshHash));
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                hash = mixFingerprintBytes(hash, &object.transform[c][r], sizeof(float));
    }
    return hash;
}

size_t expectedTriangleCount(const Scene& scene) {
    size_t n = 0;
    for (const auto& obj : scene.gameObjects) {
        if (obj.mesh) n += obj.mesh->triangles.size();
    }
    return n;
}

bool validateBVH(const BVH& bvh, size_t primitiveCount) {
    if (primitiveCount == 0) return bvh.nodes.empty() && bvh.triIndices.empty();
    if (bvh.nodes.empty() || bvh.triIndices.size() != primitiveCount ||
        bvh.nodes.size() > 2 * primitiveCount - 1) return false;
    std::vector<bool> visited(bvh.nodes.size(), false), indices(primitiveCount, false);
    std::vector<std::pair<size_t, int>> stack{{0, 0}};
    while (!stack.empty()) {
        const auto [index, depth] = stack.back();
        stack.pop_back();
        if (index >= bvh.nodes.size() || visited[index] || depth > BVH::MaxDepth) return false;
        visited[index] = true;
        const BVHNode& node = bvh.nodes[index];
        for (int c = 0; c < 3; ++c) {
            if (!std::isfinite(node.boundsMin[c]) || !std::isfinite(node.boundsMax[c]) ||
                node.boundsMin[c] > node.boundsMax[c]) return false;
        }
        if (node.count == -1) {
            if (node.leftFirst < 0 || static_cast<size_t>(node.leftFirst) + 1 >= bvh.nodes.size()) return false;
            stack.emplace_back(static_cast<size_t>(node.leftFirst), depth + 1);
            stack.emplace_back(static_cast<size_t>(node.leftFirst) + 1, depth + 1);
        } else {
            if (node.count <= 0 || node.leftFirst < 0 ||
                static_cast<size_t>(node.leftFirst) + static_cast<size_t>(node.count) > primitiveCount) return false;
            for (int i = 0; i < node.count; ++i) {
                const int primitive = bvh.triIndices[node.leftFirst + i];
                if (primitive < 0 || static_cast<size_t>(primitive) >= primitiveCount || indices[primitive]) return false;
                indices[primitive] = true;
            }
        }
    }
    return std::all_of(visited.begin(), visited.end(), [](bool v) { return v; }) &&
           std::all_of(indices.begin(), indices.end(), [](bool v) { return v; });
}

BVHNode transformedRoot(const BVHNode& meshRoot, const glm::mat4& transform) {
    glm::vec3 corners[8];
    corners[0] = meshRoot.boundsMin;
    corners[1] = glm::vec3(meshRoot.boundsMin.x, meshRoot.boundsMin.y, meshRoot.boundsMax.z);
    corners[2] = glm::vec3(meshRoot.boundsMin.x, meshRoot.boundsMax.y, meshRoot.boundsMin.z);
    corners[3] = glm::vec3(meshRoot.boundsMin.x, meshRoot.boundsMax.y, meshRoot.boundsMax.z);
    corners[4] = glm::vec3(meshRoot.boundsMax.x, meshRoot.boundsMin.y, meshRoot.boundsMin.z);
    corners[5] = glm::vec3(meshRoot.boundsMax.x, meshRoot.boundsMin.y, meshRoot.boundsMax.z);
    corners[6] = glm::vec3(meshRoot.boundsMax.x, meshRoot.boundsMax.y, meshRoot.boundsMin.z);
    corners[7] = meshRoot.boundsMax;
    glm::vec3 bmin(1e30f), bmax(-1e30f);
    for (int c = 0; c < 8; ++c) {
        glm::vec3 tc = glm::vec3(transform * glm::vec4(corners[c], 1.0f));
        bmin = glm::min(bmin, tc);
        bmax = glm::max(bmax, tc);
    }
    BVHNode out = meshRoot;
    out.boundsMin = bmin;
    out.boundsMax = bmax;
    return out;
}

bool transformsMatch(const std::vector<GameObject>& gameObjects,
                     const std::vector<glm::mat4>& transforms) {
    if (gameObjects.size() != transforms.size()) return false;
    for (size_t i = 0; i < gameObjects.size(); ++i) {
        const glm::mat4& a = gameObjects[i].transform;
        const glm::mat4& b = transforms[i];
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (a[c][r] != b[c][r]) return false;
    }
    return true;
}

bool validTransform(const glm::mat4& transform) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!std::isfinite(transform[c][r])) return false;
    return transform[0][3] == 0.0f && transform[1][3] == 0.0f &&
           transform[2][3] == 0.0f && transform[3][3] == 1.0f &&
           std::isfinite(glm::determinant(transform)) && glm::determinant(transform) != 0.0f;
}

bool validVolumeMaterials(const Scene& scene) {
    for (const Material& material : scene.materials) {
        if (!std::isfinite(material.subsurfaceMeanFreePath) || material.subsurfaceMeanFreePath < 0.0f ||
            ((material.transparency > 0.0f || material.subsurfaceMeanFreePath > 0.0f) &&
             (!std::isfinite(material.ior) || material.ior <= 0.0f))) {
            Logger::error("Volume materials require a non-negative finite mean free path and a positive finite IOR");
            return false;
        }
        if (material.baseColorTexture < -1 ||
            (material.baseColorTexture >= 0 &&
             static_cast<size_t>(material.baseColorTexture) >= scene.textures.size())) {
            Logger::error("Material references an invalid base-color texture");
            return false;
        }
    }
    return true;
}

uint64_t textureFingerprint(const Scene& scene) {
    uint64_t hash = 1469598103934665603ull;
    const size_t count = scene.textures.size();
    hash = mixFingerprintBytes(hash, &count, sizeof(count));
    for (const TextureAsset& texture : scene.textures) {
        const uint64_t revision = texture.revision();
        const int width = texture.width(), height = texture.height();
        const size_t bytes = texture.rgba().size();
        hash = mixFingerprintBytes(hash, &revision, sizeof(revision));
        hash = mixFingerprintBytes(hash, &width, sizeof(width));
        hash = mixFingerprintBytes(hash, &height, sizeof(height));
        hash = mixFingerprintBytes(hash, &bytes, sizeof(bytes));
    }
    return hash;
}

template <typename T>
void uploadVector(const std::vector<T>& values, GLenum usage) {
    const size_t bytes = values.size() * sizeof(T);
    glBufferData(GL_SHADER_STORAGE_BUFFER, std::max<size_t>(bytes, 1),
                 bytes ? values.data() : nullptr, usage);
}

void timedUpload(const char* name, size_t bytes, const std::function<void()>& fn) {
    auto start = std::chrono::high_resolution_clock::now();
    fn();
    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    Logger::info(std::string("SSBO upload: ") + name + ", size: " + std::to_string(bytes / 1024) +
                 " KB, time: " + std::to_string(ms) + " ms");
}

} // namespace

SceneGPU::SceneGPU() = default;

SceneGPU::~SceneGPU() {
    GLuint buffers[] = {triangleSSBO_, materialSSBO_, lightSSBO_,
                        tlasNodeSSBO_, tlasTriIdxSSBO_, blasNodeSSBO_,
                        blasTriIdxSSBO_, bvhInstanceSSBO_};
    for (GLuint id : buffers) {
        if (id != 0) glDeleteBuffers(1, &id);
    }
    if (textureArray_ != 0) glDeleteTextures(1, &textureArray_);
}

bool SceneGPU::uploadTextures(const Scene& scene) {
    GLint maxSize = 0, maxLayers = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &maxLayers);
    int width = 1, height = 1;
    for (const TextureAsset& texture : scene.textures) {
        if (texture.width() <= 0 || texture.height() <= 0 || texture.width() > maxSize || texture.height() > maxSize ||
            texture.rgba().size() != static_cast<size_t>(texture.width()) * static_cast<size_t>(texture.height()) * 4) {
            Logger::error("Invalid texture dimensions or pixel data: " + texture.name());
            return false;
        }
        width = std::max(width, texture.width());
        height = std::max(height, texture.height());
    }
    const size_t layers = std::max<size_t>(scene.textures.size(), 1);
    const size_t layerBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    if (layers > static_cast<size_t>(maxLayers) || layerBytes > (256ull * 1024 * 1024) / layers) {
        Logger::error("Material texture array exceeds GPU limits or the 256 MiB budget");
        return false;
    }
    GLuint textureArray = 0;
    glGenTextures(1, &textureArray);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, textureArray);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_SRGB8_ALPHA8, width, height,
                 static_cast<GLsizei>(layers), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    std::vector<std::uint8_t> pixels(layerBytes, 255);
    for (size_t layer = 0; layer < layers; ++layer) {
        if (!scene.textures.empty()) {
            const TextureAsset& source = scene.textures[layer];
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    const size_t src = (static_cast<size_t>(y) * source.height() / height * source.width() +
                                        static_cast<size_t>(x) * source.width() / width) * 4;
                    const size_t dst = (static_cast<size_t>(y) * width + x) * 4;
                    std::copy_n(source.rgba().data() + src, 4, pixels.data() + dst);
                }
        }
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, static_cast<GLint>(layer),
                        width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    }
    glActiveTexture(GL_TEXTURE0);
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        glDeleteTextures(1, &textureArray);
        Logger::error("Failed to upload material textures (OpenGL error " + std::to_string(error) + ")");
        return false;
    }
    if (textureArray_ != 0) glDeleteTextures(1, &textureArray_);
    textureArray_ = textureArray;
    texturesFingerprint_ = textureFingerprint(scene);
    return true;
}

bool SceneGPU::initialize(const Scene& scene, bool forceRebuildBVH, bool cacheOnly, bool requireCacheWrite,
                          bool persistCache) {
    if (!validVolumeMaterials(scene)) return false;
    for (const auto& obj : scene.gameObjects) {
        if (!obj.mesh || obj.mesh->triangles.empty()) {
            Logger::error("Cannot build a BVH for a missing or empty mesh");
            return false;
        }
        if (!validTransform(obj.transform)) {
            Logger::error("Cannot build a BVH for a non-affine, singular or non-finite transform");
            return false;
        }
        for (const auto& tri : obj.mesh->triangles) {
            if (tri.materialIndex < 0 || static_cast<size_t>(tri.materialIndex) >= scene.materials.size()) {
                Logger::error("Mesh triangle references an invalid material");
                return false;
            }
        }
    }
    cacheDir_ = "bvh_cache/v4/";
    std::error_code ec;
    fs::create_directories(cacheDir_, ec);
    if (ec) {
        Logger::error("Cannot create BVH cache directory: " + ec.message());
        if (requireCacheWrite) return false;
    }

    std::vector<Triangle> allTriangles;
    std::vector<BVHNode> allBLASNodes;
    std::vector<int> allBLASTriIndices;
    std::vector<BVHInstance> meshInstances;
    std::vector<BVHNode> tlasNodes;
    std::vector<int> tlasTriIndices;

    const std::string ssboCachePrefix = (cacheDir_ / "ssbo_v4_").string();
    const size_t expectedTris = expectedTriangleCount(scene);
    const uint64_t expectedFingerprint = computeSceneFingerprint(scene);
    const fs::path sceneFingerprintPath = ssboCachePrefix + "fingerprint.bin";

    bool loadedSSBOCache = false;
    if (!forceRebuildBVH &&
        fs::exists(ssboCachePrefix + "triangles.bin") &&
        fs::exists(ssboCachePrefix + "blasnodes.bin") &&
        fs::exists(ssboCachePrefix + "blastris.bin") &&
        fs::exists(ssboCachePrefix + "instances.bin") &&
        fs::exists(ssboCachePrefix + "tlasnodes.bin") &&
        fs::exists(ssboCachePrefix + "tlastris.bin") &&
        fs::exists(sceneFingerprintPath)) {
        SceneCacheManifest cached{};
        std::ifstream in(sceneFingerprintPath, std::ios::binary);
        loadedSSBOCache =
            in.read(reinterpret_cast<char*>(&cached), sizeof(cached)).good() &&
            loadVec(ssboCachePrefix + "triangles.bin", allTriangles) &&
            loadVec(ssboCachePrefix + "blasnodes.bin", allBLASNodes) &&
            loadVec(ssboCachePrefix + "blastris.bin", allBLASTriIndices) &&
            loadVec(ssboCachePrefix + "instances.bin", meshInstances) &&
            loadVec(ssboCachePrefix + "tlasnodes.bin", tlasNodes) &&
            loadVec(ssboCachePrefix + "tlastris.bin", tlasTriIndices);
        if (loadedSSBOCache) {
            if (meshInstances.size() != scene.gameObjects.size() ||
                cached.scene != expectedFingerprint ||
                cached.triangles != vectorFingerprint(allTriangles) ||
                cached.blasNodes != vectorFingerprint(allBLASNodes) ||
                cached.blasIndices != vectorFingerprint(allBLASTriIndices) ||
                cached.instances != vectorFingerprint(meshInstances) ||
                cached.tlasNodes != vectorFingerprint(tlasNodes) ||
                cached.tlasIndices != vectorFingerprint(tlasTriIndices) ||
                allTriangles.size() != expectedTris ||
                allBLASTriIndices.size() != expectedTris) {
                Logger::info("SSBO cache invalidated");
                loadedSSBOCache = false;
                allTriangles.clear(); allBLASNodes.clear(); allBLASTriIndices.clear();
                meshInstances.clear(); tlasNodes.clear(); tlasTriIndices.clear();
            } else {
                meshBLAS_.assign(scene.gameObjects.size(), {});
                size_t nodeOffset = 0, triOffset = 0, globalOffset = 0;
                for (size_t i = 0; loadedSSBOCache && i < meshInstances.size(); ++i) {
                    const BVHInstance& inst = meshInstances[i];
                    const size_t nextNode = i + 1 < meshInstances.size() && meshInstances[i + 1].blasNodeOffset >= 0
                        ? static_cast<size_t>(meshInstances[i + 1].blasNodeOffset) : allBLASNodes.size();
                    const size_t nextTri = i + 1 < meshInstances.size() && meshInstances[i + 1].blasTriOffset >= 0
                        ? static_cast<size_t>(meshInstances[i + 1].blasTriOffset) : allBLASTriIndices.size();
                    const size_t count = scene.gameObjects[i].mesh->triangles.size();
                    if (inst.blasNodeOffset < 0 || inst.blasTriOffset < 0 || inst.globalTriOffset < 0 ||
                        static_cast<size_t>(inst.blasNodeOffset) != nodeOffset ||
                        static_cast<size_t>(inst.blasTriOffset) != triOffset ||
                        static_cast<size_t>(inst.globalTriOffset) != globalOffset ||
                        inst.meshIndex != static_cast<int>(i) || nextNode <= nodeOffset ||
                        nextNode > allBLASNodes.size() || nextTri > allBLASTriIndices.size() ||
                        nextTri < triOffset || nextTri - triOffset != count) {
                        loadedSSBOCache = false;
                        break;
                    }
                    auto& blas = meshBLAS_[i];
                    blas.nodes.assign(allBLASNodes.begin() + nodeOffset, allBLASNodes.begin() + nextNode);
                    blas.triIndices.assign(allBLASTriIndices.begin() + triOffset, allBLASTriIndices.begin() + nextTri);
                    loadedSSBOCache = validateBVH(blas, count);
                    nodeOffset = nextNode;
                    triOffset = nextTri;
                    globalOffset += count;
                }
                BVH tlas;
                tlas.nodes = tlasNodes;
                tlas.triIndices = tlasTriIndices;
                loadedSSBOCache = loadedSSBOCache && validateBVH(tlas, scene.gameObjects.size());
                if (loadedSSBOCache) Logger::info("Loaded SSBO, BLAS and TLAS data from cache");
            }
        }
    }

    if (!loadedSSBOCache) {
        if (cacheOnly) {
            Logger::error("No compatible, valid BVH scene cache; run --prepare-cache");
            return false;
        }
        allBLASNodes.clear(); allBLASTriIndices.clear(); tlasNodes.clear(); tlasTriIndices.clear();
        meshBLAS_.assign(scene.gameObjects.size(), {});
        std::vector<BVHNode> worldRootNodes;
        worldRootNodes.reserve(scene.gameObjects.size());
        int nodeOffset = 0;
        int triOffset = 0;
        meshInstances.clear();
        allTriangles.clear();

        for (size_t i = 0; i < scene.gameObjects.size(); ++i) {
            const auto& obj = scene.gameObjects[i];
            const auto& mesh = obj.mesh;
            const auto& meshTris = mesh->triangles;
            std::string meshName = "mesh" + std::to_string(i);
            fs::path blasBase = cacheDir_ / meshName;
            fs::path blasFingerprintPath = (blasBase.string() + ".fingerprint.bin");
            bool loaded = false;
            if (!forceRebuildBVH &&
                fs::exists(blasBase.string() + ".nodes.bin") &&
                fs::exists(blasBase.string() + ".tris.bin") &&
                fs::exists(blasFingerprintPath)) {
                MeshCacheManifest cachedFp{};
                uint64_t expectFp = computeMeshFingerprint(*mesh);
                std::ifstream fpFile(blasFingerprintPath, std::ios::binary);
                if (fpFile.read(reinterpret_cast<char*>(&cachedFp), sizeof(cachedFp)).good() &&
                    cachedFp.mesh == expectFp) {
                    loaded = loadBVHFiles(blasBase, meshBLAS_[i]) &&
                             cachedFp.nodes == vectorFingerprint(meshBLAS_[i].nodes) &&
                             cachedFp.indices == vectorFingerprint(meshBLAS_[i].triIndices) &&
                             validateBVH(meshBLAS_[i], meshTris.size());
                }
                if (loaded) {
                    Logger::info("Loaded BLAS from cache for " + meshName);
                }
            }
            if (!loaded) {
                Logger::info("Building BLAS from scratch for " + meshName);
                meshBLAS_[i].buildBLAS(meshTris);
                if (persistCache) {
                    const bool saved = saveBVHFiles(blasBase, meshBLAS_[i]);
                    MeshCacheManifest fp{computeMeshFingerprint(*mesh), vectorFingerprint(meshBLAS_[i].nodes),
                                         vectorFingerprint(meshBLAS_[i].triIndices)};
                    std::ofstream fpFile(blasFingerprintPath, std::ios::binary);
                    fpFile.write(reinterpret_cast<const char*>(&fp), sizeof(fp));
                    fpFile.close();
                    if (!saved || !fpFile) Logger::error("Failed to save BLAS cache for " + meshName);
                    if (requireCacheWrite && (!saved || !fpFile)) return false;
                }
            }

            size_t triBase = allTriangles.size();
            allTriangles.insert(allTriangles.end(), meshTris.begin(), meshTris.end());

            const BVHNode& meshRoot = meshBLAS_[i].nodes[0];
            worldRootNodes.push_back(transformedRoot(meshRoot, obj.transform));

            BVHInstance inst{};
            inst.blasNodeOffset = nodeOffset;
            inst.blasTriOffset = triOffset;
            inst.globalTriOffset = static_cast<int>(triBase);
            inst.meshIndex = static_cast<int>(i);
            inst.transform = obj.transform;
            inst.inverseTransform = glm::inverse(obj.transform);
            meshInstances.push_back(inst);

            nodeOffset += static_cast<int>(meshBLAS_[i].nodes.size());
            triOffset += static_cast<int>(meshBLAS_[i].triIndices.size());
        }

        BVH tlas;
        Logger::info("Building TLAS from current scene transforms");
        tlas.buildTLAS(meshInstances, worldRootNodes);

        allBLASNodes.clear();
        allBLASTriIndices.clear();
        for (const auto& blas : meshBLAS_) {
            allBLASNodes.insert(allBLASNodes.end(), blas.nodes.begin(), blas.nodes.end());
            allBLASTriIndices.insert(allBLASTriIndices.end(), blas.triIndices.begin(), blas.triIndices.end());
        }
        tlasNodes = tlas.nodes;
        tlasTriIndices = tlas.triIndices;

        if (persistCache) {
            // Invalidate the manifest before writing any of the dependent files.
            fs::remove(sceneFingerprintPath, ec);
            if (ec) {
                Logger::error("Cannot invalidate BVH cache manifest: " + ec.message());
                if (requireCacheWrite) return false;
            }
            bool saved = !ec &&
                saveVec(fs::path(ssboCachePrefix + "triangles.bin"), allTriangles) &&
                saveVec(fs::path(ssboCachePrefix + "blasnodes.bin"), allBLASNodes) &&
                saveVec(fs::path(ssboCachePrefix + "blastris.bin"), allBLASTriIndices) &&
                saveVec(fs::path(ssboCachePrefix + "instances.bin"), meshInstances) &&
                saveVec(fs::path(ssboCachePrefix + "tlasnodes.bin"), tlasNodes) &&
                saveVec(fs::path(ssboCachePrefix + "tlastris.bin"), tlasTriIndices);
            if (saved) {
                SceneCacheManifest manifest{expectedFingerprint, vectorFingerprint(allTriangles),
                    vectorFingerprint(allBLASNodes), vectorFingerprint(allBLASTriIndices),
                    vectorFingerprint(meshInstances), vectorFingerprint(tlasNodes), vectorFingerprint(tlasTriIndices)};
                std::ofstream fpFile(sceneFingerprintPath, std::ios::binary);
                fpFile.write(reinterpret_cast<const char*>(&manifest), sizeof(manifest));
                fpFile.close();
                saved = static_cast<bool>(fpFile);
            }
            if (saved) Logger::info("Saved SSBO, BLAS and TLAS data to cache");
            else Logger::error("Failed to save complete BVH scene cache");
            if (requireCacheWrite && !saved) return false;
        }
    }

    // Reconcile transforms with the current scene state (cache may be stale on transforms only).
    if (loadedSSBOCache) {
        for (size_t i = 0; i < meshInstances.size() && i < scene.gameObjects.size(); ++i) {
            meshInstances[i].transform = scene.gameObjects[i].transform;
            meshInstances[i].meshIndex = static_cast<int>(i);
            meshInstances[i].inverseTransform = glm::inverse(scene.gameObjects[i].transform);
        }
    }

    // Cache mesh root nodes (used by updateDynamic).
    meshRootNodes_.clear();
    meshRootNodes_.reserve(meshBLAS_.size());
    for (const auto& blas : meshBLAS_) {
        meshRootNodes_.push_back(blas.nodes[0]);
    }

    totalTriangles_ = static_cast<int>(allTriangles.size());

    if (!uploadTextures(scene)) return false;
    Logger::info("Initializing SSBOs for triangles, materials, lights, BVHs, and instances");
    timedUpload("Triangles", allTriangles.size() * sizeof(Triangle), [&]() {
        glGenBuffers(1, &triangleSSBO_);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, triangleSSBO_);
        uploadVector(allTriangles, GL_DYNAMIC_DRAW);
    });
    timedUpload("Materials", scene.materials.size() * sizeof(Material), [&]() {
        glGenBuffers(1, &materialSSBO_);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, materialSSBO_);
        uploadVector(scene.materials, GL_DYNAMIC_DRAW);
    });
    timedUpload("Lights", scene.lights.size() * sizeof(Light), [&]() {
        glGenBuffers(1, &lightSSBO_);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, lightSSBO_);
        uploadVector(scene.lights, GL_DYNAMIC_DRAW);
    });
    timedUpload("TLAS Nodes", tlasNodes.size() * sizeof(BVHNode), [&]() {
        glGenBuffers(1, &tlasNodeSSBO_);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, tlasNodeSSBO_);
        uploadVector(tlasNodes, GL_DYNAMIC_DRAW);
    });
    timedUpload("TLAS Tri Indices", tlasTriIndices.size() * sizeof(int), [&]() {
        glGenBuffers(1, &tlasTriIdxSSBO_);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, tlasTriIdxSSBO_);
        uploadVector(tlasTriIndices, GL_DYNAMIC_DRAW);
    });
    timedUpload("BLAS Nodes", allBLASNodes.size() * sizeof(BVHNode), [&]() {
        glGenBuffers(1, &blasNodeSSBO_);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, blasNodeSSBO_);
        uploadVector(allBLASNodes, GL_DYNAMIC_DRAW);
    });
    timedUpload("BLAS Tri Indices", allBLASTriIndices.size() * sizeof(int), [&]() {
        glGenBuffers(1, &blasTriIdxSSBO_);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, blasTriIdxSSBO_);
        uploadVector(allBLASTriIndices, GL_DYNAMIC_DRAW);
    });
    timedUpload("BVH Instances", meshInstances.size() * sizeof(BVHInstance), [&]() {
        glGenBuffers(1, &bvhInstanceSSBO_);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, bvhInstanceSSBO_);
        uploadVector(meshInstances, GL_DYNAMIC_DRAW);
    });
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    previousTransforms_.clear();
    previousMeshes_.clear();
    meshFingerprints_.clear();
    for (const auto& obj : scene.gameObjects) {
        previousTransforms_.push_back(obj.transform);
        previousMeshes_.push_back(obj.mesh);
        meshFingerprints_.push_back(computeMeshFingerprint(*obj.mesh));
    }
    materialsFingerprint_ = vectorFingerprint(scene.materials);
    lightsFingerprint_ = vectorFingerprint(scene.lights);
    return true;
}

void SceneGPU::bindAll() const {
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, triangleSSBO_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, materialSSBO_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, lightSSBO_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, tlasNodeSSBO_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, tlasTriIdxSSBO_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, blasNodeSSBO_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, blasTriIdxSSBO_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, bvhInstanceSSBO_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, textureArray_);
    glActiveTexture(GL_TEXTURE0);
}

void SceneGPU::swap(SceneGPU& other) noexcept {
    using std::swap;
    swap(triangleSSBO_, other.triangleSSBO_);
    swap(materialSSBO_, other.materialSSBO_);
    swap(lightSSBO_, other.lightSSBO_);
    swap(tlasNodeSSBO_, other.tlasNodeSSBO_);
    swap(tlasTriIdxSSBO_, other.tlasTriIdxSSBO_);
    swap(blasNodeSSBO_, other.blasNodeSSBO_);
    swap(blasTriIdxSSBO_, other.blasTriIdxSSBO_);
    swap(bvhInstanceSSBO_, other.bvhInstanceSSBO_);
    swap(textureArray_, other.textureArray_);
    swap(totalTriangles_, other.totalTriangles_);
    swap(meshBLAS_, other.meshBLAS_);
    swap(meshRootNodes_, other.meshRootNodes_);
    swap(previousTransforms_, other.previousTransforms_);
    swap(previousMeshes_, other.previousMeshes_);
    swap(meshFingerprints_, other.meshFingerprints_);
    swap(materialsFingerprint_, other.materialsFingerprint_);
    swap(lightsFingerprint_, other.lightsFingerprint_);
    swap(texturesFingerprint_, other.texturesFingerprint_);
    swap(cacheDir_, other.cacheDir_);
}

SceneUpdate SceneGPU::updateDynamic(const Scene& scene) {
    bool geometryChanged = scene.gameObjects.size() != previousMeshes_.size();
    for (size_t i = 0; !geometryChanged && i < scene.gameObjects.size(); ++i) {
        const auto& mesh = scene.gameObjects[i].mesh;
        geometryChanged = !mesh || mesh != previousMeshes_[i] ||
                          computeMeshFingerprint(*mesh) != meshFingerprints_[i];
    }
    if (geometryChanged) {
        SceneGPU replacement;
        if (!replacement.initialize(scene, false, false, false, false)) return SceneUpdate::Error;
        swap(replacement);
        bindAll();
        return SceneUpdate::Scene;
    }
    bool updated = false;
    const uint64_t materialHash = vectorFingerprint(scene.materials);
    const uint64_t lightHash = vectorFingerprint(scene.lights);
    const uint64_t textureHash = textureFingerprint(scene);
    if ((materialHash != materialsFingerprint_ || textureHash != texturesFingerprint_) &&
        !validVolumeMaterials(scene)) return SceneUpdate::Error;
    if (textureHash != texturesFingerprint_) {
        if (!uploadTextures(scene)) return SceneUpdate::Error;
        updated = true;
    }
    if (materialHash != materialsFingerprint_) {
        for (const auto& obj : scene.gameObjects) {
            for (const auto& tri : obj.mesh->triangles) {
                if (tri.materialIndex < 0 || static_cast<size_t>(tri.materialIndex) >= scene.materials.size()) {
                    Logger::error("Mesh triangle references an invalid material");
                    return SceneUpdate::Error;
                }
            }
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, materialSSBO_);
        uploadVector(scene.materials, GL_DYNAMIC_DRAW);
        materialsFingerprint_ = materialHash;
        updated = true;
    }
    if (lightHash != lightsFingerprint_) {
        uploadLights(scene);
        updated = true;
    }
    if (transformsMatch(scene.gameObjects, previousTransforms_)) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        return updated ? SceneUpdate::Transforms : SceneUpdate::Unchanged;
    }
    for (const auto& obj : scene.gameObjects) {
        if (!validTransform(obj.transform)) {
            Logger::error("Cannot update BVH for a non-affine, singular or non-finite transform");
            return SceneUpdate::Error;
        }
    }

    std::vector<BVHInstance> meshInstances;
    meshInstances.reserve(scene.gameObjects.size());
    int nodeOffset = 0, triOffset = 0, globalTriOffset = 0;
    for (size_t i = 0; i < scene.gameObjects.size(); ++i) {
        BVHInstance inst{};
        inst.blasNodeOffset = nodeOffset;
        inst.blasTriOffset = triOffset;
        inst.globalTriOffset = globalTriOffset;
        inst.meshIndex = static_cast<int>(i);
        inst.transform = scene.gameObjects[i].transform;
        inst.inverseTransform = glm::inverse(scene.gameObjects[i].transform);
        meshInstances.push_back(inst);
        nodeOffset += static_cast<int>(meshBLAS_[i].nodes.size());
        triOffset += static_cast<int>(meshBLAS_[i].triIndices.size());
        globalTriOffset += static_cast<int>(scene.gameObjects[i].mesh->triangles.size());
    }

    std::vector<BVHNode> instanceRootNodes;
    instanceRootNodes.reserve(scene.gameObjects.size());
    for (size_t i = 0; i < scene.gameObjects.size(); ++i) {
        instanceRootNodes.push_back(transformedRoot(meshRootNodes_[i], scene.gameObjects[i].transform));
    }

    BVH tlas;
    tlas.buildTLAS(meshInstances, instanceRootNodes);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, bvhInstanceSSBO_);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, meshInstances.size() * sizeof(BVHInstance), meshInstances.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, tlasNodeSSBO_);
    uploadVector(tlas.nodes, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, tlasTriIdxSSBO_);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, tlas.triIndices.size() * sizeof(int), tlas.triIndices.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    if (const GLenum error = glGetError(); error != GL_NO_ERROR) {
        Logger::error("Failed to upload moving scene BVH (OpenGL error " + std::to_string(error) + ")");
        return SceneUpdate::Error;
    }

    previousTransforms_.clear();
    previousTransforms_.reserve(scene.gameObjects.size());
    for (const auto& obj : scene.gameObjects) {
        previousTransforms_.push_back(obj.transform);
    }
    return SceneUpdate::Transforms;
}

void SceneGPU::uploadLights(const Scene& scene) {
    if (lightSSBO_ == 0) return;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, lightSSBO_);
    uploadVector(scene.lights, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    lightsFingerprint_ = vectorFingerprint(scene.lights);
}

} // namespace rayzen
