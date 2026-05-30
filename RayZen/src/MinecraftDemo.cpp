#include "MinecraftDemo.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "Logger.h"

extern unsigned int SCR_WIDTH;
extern unsigned int SCR_HEIGHT;

namespace {
constexpr float kBlockSize = 1.0f;

int materialIndexFor(BlockType type, const MinecraftMaterialPalette& palette) {
    switch (type) {
        case BlockType::Grass:  return palette.grass;
        case BlockType::Dirt:   return palette.dirt;
        case BlockType::Stone:  return palette.stone;
        case BlockType::Log:    return palette.log;
        case BlockType::Leaves: return palette.leaves;
        default:                return palette.dirt;
    }
}

} // namespace

VoxelChunk::VoxelChunk(int width, int height, int depth)
    : mWidth(width), mHeight(height), mDepth(depth), mBlocks(static_cast<size_t>(width * height * depth), BlockType::Air) {}

size_t VoxelChunk::index(int x, int y, int z) const {
    return static_cast<size_t>((y * mDepth + z) * mWidth + x);
}

bool VoxelChunk::inBounds(int x, int y, int z) const {
    return x >= 0 && x < mWidth && y >= 0 && y < mHeight && z >= 0 && z < mDepth;
}

BlockType VoxelChunk::get(int x, int y, int z) const {
    if (!inBounds(x, y, z)) {
        return BlockType::Air;
    }
    return mBlocks[index(x, y, z)];
}

void VoxelChunk::set(int x, int y, int z, BlockType type) {
    if (!inBounds(x, y, z)) {
        return;
    }
    mBlocks[index(x, y, z)] = type;
}

void VoxelChunk::fill(BlockType type) {
    std::fill(mBlocks.begin(), mBlocks.end(), type);
}

MinecraftWorld::MinecraftWorld(int width, int height, int depth)
    : mChunk(width, height, depth), mHeightMap(static_cast<size_t>(width * depth), -1) {}

void MinecraftWorld::generate() {
    mChunk.fill(BlockType::Air);

    const float maxTerrain = static_cast<float>(mChunk.height() - 2);
    const float baseHeight = maxTerrain * 0.5f;
    std::mt19937 rng(1337);

    for (int x = 0; x < mChunk.width(); ++x) {
        for (int z = 0; z < mChunk.depth(); ++z) {
            float fx = static_cast<float>(x) / static_cast<float>(mChunk.width());
            float fz = static_cast<float>(z) / static_cast<float>(mChunk.depth());
            float undulation = std::sin(fx * glm::two_pi<float>()) * 2.5f + std::cos(fz * glm::two_pi<float>() * 0.75f) * 2.0f;
            float ridge = std::sin((fx + fz) * glm::pi<float>()) * 1.5f;
            int columnHeight = static_cast<int>(std::round(baseHeight + undulation + ridge));
            columnHeight = std::clamp(columnHeight, 1, mChunk.height() - 2);

            for (int y = 0; y <= columnHeight; ++y) {
                BlockType type = BlockType::Stone;
                if (y == columnHeight) {
                    type = BlockType::Grass;
                } else if (y >= columnHeight - 2) {
                    type = BlockType::Dirt;
                }
                if (y == 0) {
                    type = BlockType::Stone;
                }
                mChunk.set(x, y, z, type);
            }
        }
    }

    std::uniform_int_distribution<int> treeX(2, mChunk.width() - 3);
    std::uniform_int_distribution<int> treeZ(2, mChunk.depth() - 3);
    std::uniform_int_distribution<int> trunkHeightDist(4, 5);
    const int desiredTrees = std::max(4, (mChunk.width() * mChunk.depth()) / 96);
    int planted = 0;
    for (int attempt = 0; attempt < desiredTrees * 4 && planted < desiredTrees; ++attempt) {
        int x = treeX(rng);
        int z = treeZ(rng);
        int centerX = mChunk.width() / 2;
        int centerZ = mChunk.depth() / 2;
        if (std::abs(x - centerX) < 3 && std::abs(z - centerZ) < 3) {
            continue;
        }

        int surfaceY = surfaceHeightAt(x, z);
        if (surfaceY <= 0) {
            continue;
        }
        if (mChunk.get(x, surfaceY, z) != BlockType::Grass) {
            continue;
        }

        int trunkHeight = trunkHeightDist(rng);
        carveTree(x, surfaceY + 1, z, trunkHeight);
        ++planted;
    }

    for (int x = 0; x < mChunk.width(); ++x) {
        for (int z = 0; z < mChunk.depth(); ++z) {
            int surface = surfaceHeightAt(x, z);
            size_t idx = static_cast<size_t>(z * mChunk.width() + x);
            mHeightMap[idx] = surface;
        }
    }

    int spawnX = mChunk.width() / 2;
    int spawnZ = mChunk.depth() / 2;
    int spawnSurface = surfaceHeightAt(spawnX, spawnZ);
    if (spawnSurface < 0) {
        spawnSurface = 1;
    }
    glm::vec3 offset = chunkOffset();
    mSpawnPosition = offset + glm::vec3(static_cast<float>(spawnX) + 0.5f,
                                        static_cast<float>(spawnSurface) + 1.0f,
                                        static_cast<float>(spawnZ) + 0.5f);

    Logger::info("Minecraft world generated: " + std::to_string(mChunk.width()) + "x" + std::to_string(mChunk.depth()) + " columns");
}

void MinecraftWorld::carveTree(int baseX, int baseY, int baseZ, int trunkHeight) {
    for (int i = 0; i < trunkHeight && baseY + i < mChunk.height() - 1; ++i) {
        mChunk.set(baseX, baseY + i, baseZ, BlockType::Log);
    }

    int canopyBase = baseY + trunkHeight - 1;
    for (int y = 0; y < 3; ++y) {
        int layerY = canopyBase + y;
        if (layerY >= mChunk.height()) {
            break;
        }
        int radius = (y == 0) ? 2 : 1;
        for (int dx = -radius; dx <= radius; ++dx) {
            for (int dz = -radius; dz <= radius; ++dz) {
                int px = baseX + dx;
                int pz = baseZ + dz;
                if (!mChunk.inBounds(px, layerY, pz)) {
                    continue;
                }
                if (dx == 0 && dz == 0 && y == 2) {
                    continue;
                }
                if (mChunk.get(px, layerY, pz) == BlockType::Air) {
                    mChunk.set(px, layerY, pz, BlockType::Leaves);
                }
            }
        }
    }
}

int MinecraftWorld::surfaceHeightAt(int x, int z) const {
    for (int y = mChunk.height() - 1; y >= 0; --y) {
        BlockType type = mChunk.get(x, y, z);
        if (type != BlockType::Air && type != BlockType::Leaves) {
            return y;
        }
    }
    return -1;
}

BlockType MinecraftWorld::blockAtOrAir(int x, int y, int z) const {
    if (!mChunk.inBounds(x, y, z)) {
        return BlockType::Air;
    }
    return mChunk.get(x, y, z);
}

glm::vec3 MinecraftWorld::chunkOffset() const {
    return glm::vec3(-0.5f * static_cast<float>(mChunk.width()) * kBlockSize,
                     0.0f,
                     -0.5f * static_cast<float>(mChunk.depth()) * kBlockSize);
}

std::shared_ptr<Mesh> MinecraftWorld::buildMesh(const MinecraftMaterialPalette& palette) const {
    auto mesh = std::make_shared<Mesh>();
    size_t approxSurfaceTris = static_cast<size_t>(mChunk.width()) * static_cast<size_t>(mChunk.depth()) * 12ull;
    mesh->triangles.reserve(approxSurfaceTris);

    const glm::vec3 offset = chunkOffset();

    auto pushQuad = [&](const std::array<glm::vec3, 4>& verts, BlockType type) {
        Triangle t0;
        t0.v0 = verts[0];
        t0.v1 = verts[1];
        t0.v2 = verts[2];
        t0.materialIndex = materialIndexFor(type, palette);
        mesh->triangles.push_back(t0);

        Triangle t1;
        t1.v0 = verts[0];
        t1.v1 = verts[2];
        t1.v2 = verts[3];
        t1.materialIndex = t0.materialIndex;
        mesh->triangles.push_back(t1);
    };

    auto leafMaskNoise = [](int bx, int by, int bz, int faceId, int uCell, int vCell) {
        uint32_t h = 2166136261u;
        auto mix = [&](uint32_t value) {
            h ^= value;
            h *= 16777619u;
        };
        mix(static_cast<uint32_t>(bx * 73856093));
        mix(static_cast<uint32_t>(by * 19349663));
        mix(static_cast<uint32_t>(bz * 83492791));
        mix(static_cast<uint32_t>(faceId * 2654435761u));
        mix(static_cast<uint32_t>(uCell * 374761393));
        mix(static_cast<uint32_t>(vCell * 668265263));
        return static_cast<float>(h & 0xffffu) / 65535.0f;
    };

    enum FaceId { POS_X = 0, NEG_X, POS_Y, NEG_Y, POS_Z, NEG_Z };

    auto addFace = [&](const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, const glm::vec3& v3,
                       BlockType type, const glm::vec3& expectedNormal, int faceId,
                       int blockX, int blockY, int blockZ) {
        std::array<glm::vec3, 4> verts{v0, v1, v2, v3};
        glm::vec3 computedNormal = glm::vec3(0.0f);
        glm::vec3 edge1 = verts[1] - verts[0];
        glm::vec3 edge2 = verts[2] - verts[0];
        if (glm::dot(edge1, edge1) > 0.0f && glm::dot(edge2, edge2) > 0.0f) {
            computedNormal = glm::normalize(glm::cross(edge1, edge2));
        }
        if (glm::dot(computedNormal, expectedNormal) < 0.0f) {
            std::swap(verts[1], verts[3]);
        }

        if (type == BlockType::Leaves) {
            const int subdivisions = 3;
            const float holeChance = 0.4f;
            glm::vec3 origin = verts[0];
            glm::vec3 edgeU = verts[1] - verts[0];
            glm::vec3 edgeV = verts[3] - verts[0];
            bool emitted = false;
            std::array<glm::vec3, 4> fallback{};
            bool fallbackSet = false;
            for (int u = 0; u < subdivisions; ++u) {
                for (int v = 0; v < subdivisions; ++v) {
                    float fu0 = static_cast<float>(u) / static_cast<float>(subdivisions);
                    float fv0 = static_cast<float>(v) / static_cast<float>(subdivisions);
                    float fu1 = static_cast<float>(u + 1) / static_cast<float>(subdivisions);
                    float fv1 = static_cast<float>(v + 1) / static_cast<float>(subdivisions);
                    std::array<glm::vec3, 4> subVerts{
                        origin + edgeU * fu0 + edgeV * fv0,
                        origin + edgeU * fu1 + edgeV * fv0,
                        origin + edgeU * fu1 + edgeV * fv1,
                        origin + edgeU * fu0 + edgeV * fv1};
                    if (!fallbackSet) {
                        fallback = subVerts;
                        fallbackSet = true;
                    }
                    float noise = leafMaskNoise(blockX, blockY, blockZ, faceId, u, v);
                    if (noise < holeChance && emitted) {
                        continue;
                    }
                    pushQuad(subVerts, type);
                    emitted = true;
                }
            }
            if (!emitted && fallbackSet) {
                pushQuad(fallback, type);
            }
        } else {
            pushQuad(verts, type);
        }
    };

    for (int x = 0; x < mChunk.width(); ++x) {
        for (int y = 0; y < mChunk.height(); ++y) {
            for (int z = 0; z < mChunk.depth(); ++z) {
                BlockType type = mChunk.get(x, y, z);
                if (type == BlockType::Air) {
                    continue;
                }

                glm::vec3 minCorner = offset + glm::vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)) * kBlockSize;
                glm::vec3 maxCorner = minCorner + glm::vec3(kBlockSize);

                if (blockAtOrAir(x + 1, y, z) == BlockType::Air) {
                    addFace({maxCorner.x, minCorner.y, minCorner.z},
                            {maxCorner.x, maxCorner.y, minCorner.z},
                            {maxCorner.x, maxCorner.y, maxCorner.z},
                            {maxCorner.x, minCorner.y, maxCorner.z}, type, glm::vec3(1.0f, 0.0f, 0.0f), POS_X, x, y, z);
                }
                if (blockAtOrAir(x - 1, y, z) == BlockType::Air) {
                    addFace({minCorner.x, minCorner.y, maxCorner.z},
                            {minCorner.x, maxCorner.y, maxCorner.z},
                            {minCorner.x, maxCorner.y, minCorner.z},
                            {minCorner.x, minCorner.y, minCorner.z}, type, glm::vec3(-1.0f, 0.0f, 0.0f), NEG_X, x, y, z);
                }
                if (blockAtOrAir(x, y + 1, z) == BlockType::Air) {
                    addFace({minCorner.x, maxCorner.y, minCorner.z},
                            {minCorner.x, maxCorner.y, maxCorner.z},
                            {maxCorner.x, maxCorner.y, maxCorner.z},
                            {maxCorner.x, maxCorner.y, minCorner.z}, type, glm::vec3(0.0f, 1.0f, 0.0f), POS_Y, x, y, z);
                }
                if (blockAtOrAir(x, y - 1, z) == BlockType::Air) {
                    addFace({minCorner.x, minCorner.y, maxCorner.z},
                            {minCorner.x, minCorner.y, minCorner.z},
                            {maxCorner.x, minCorner.y, minCorner.z},
                            {maxCorner.x, minCorner.y, maxCorner.z}, type, glm::vec3(0.0f, -1.0f, 0.0f), NEG_Y, x, y, z);
                }
                if (blockAtOrAir(x, y, z + 1) == BlockType::Air) {
                    addFace({maxCorner.x, minCorner.y, maxCorner.z},
                            {maxCorner.x, maxCorner.y, maxCorner.z},
                            {minCorner.x, maxCorner.y, maxCorner.z},
                            {minCorner.x, minCorner.y, maxCorner.z}, type, glm::vec3(0.0f, 0.0f, 1.0f), POS_Z, x, y, z);
                }
                if (blockAtOrAir(x, y, z - 1) == BlockType::Air) {
                    addFace({minCorner.x, minCorner.y, minCorner.z},
                            {minCorner.x, maxCorner.y, minCorner.z},
                            {maxCorner.x, maxCorner.y, minCorner.z},
                            {maxCorner.x, minCorner.y, minCorner.z}, type, glm::vec3(0.0f, 0.0f, -1.0f), NEG_Z, x, y, z);
                }
            }
        }
    }

    Logger::info("Minecraft chunk baked: " + std::to_string(mesh->triangles.size()) + " triangles");
    return mesh;
}

float MinecraftWorld::sampleGroundHeight(float worldX, float worldZ) const {
    glm::vec3 offset = chunkOffset();
    float localX = (worldX - offset.x) / kBlockSize;
    float localZ = (worldZ - offset.z) / kBlockSize;
    int ix = static_cast<int>(std::floor(localX));
    int iz = static_cast<int>(std::floor(localZ));
    ix = std::clamp(ix, 0, mChunk.width() - 1);
    iz = std::clamp(iz, 0, mChunk.depth() - 1);
    size_t idx = static_cast<size_t>(iz * mChunk.width() + ix);
    int surface = mHeightMap[idx];
    if (surface < 0) {
        surface = 0;
    }
    return offset.y + (static_cast<float>(surface) + 1.0f) * kBlockSize;
}

MinecraftPlayerController::MinecraftPlayerController(MinecraftWorld& world, Camera& camera)
    : mWorld(world), mCamera(camera) {
    mCamera.speed = mWalkSpeed;
    mCamera.sensitivity = 0.12f;
    float ground = mWorld.sampleGroundHeight(mCamera.position.x, mCamera.position.z);
    mCamera.position.y = ground + mEyeHeight;
}

void MinecraftPlayerController::process(GLFWwindow* window, float deltaTime) {
    float speed = (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) ? mWalkSpeed * 1.8f : mWalkSpeed;

    glm::vec3 forward = glm::vec3(mCamera.target.x, 0.0f, mCamera.target.z);
    float forwardLen = glm::length(forward);
    if (!std::isfinite(forwardLen) || forwardLen < 1e-4f) {
        forward = glm::vec3(0.0f, 0.0f, -1.0f);
    } else {
        forward /= forwardLen;
    }
    glm::vec3 right = glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f));
    float rightLen = glm::length(right);
    if (rightLen < 1e-4f) {
        right = glm::vec3(1.0f, 0.0f, 0.0f);
    } else {
        right /= rightLen;
    }

    glm::vec3 movement(0.0f);
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) movement += forward;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) movement -= forward;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) movement -= right;
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) movement += right;

    float moveLen = glm::length(movement);
    if (moveLen > 0.0f) {
        movement = (movement / moveLen) * speed * deltaTime;
        mCamera.position += movement;
    }

    if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
        if (!mIsDragging) {
            glfwGetCursorPos(window, &mLastX, &mLastY);
            mIsDragging = true;
        }
        double mouseX, mouseY;
        glfwGetCursorPos(window, &mouseX, &mouseY);
        double offsetX = mouseX - mLastX;
        double offsetY = mLastY - mouseY;
        mCamera.rotate(offsetX, offsetY);
        mLastX = mouseX;
        mLastY = mouseY;
    } else {
        mIsDragging = false;
    }

    float ground = mWorld.sampleGroundHeight(mCamera.position.x, mCamera.position.z);
    mCamera.position.y = ground + mEyeHeight;
}

MinecraftDemo::MinecraftDemo(Scene& scene)
    : mScene(scene), mWorld(160, 40, 160) {}

void MinecraftDemo::initialize() {
    buildMaterials();
    buildLighting();
    buildGeometry();

    glm::vec3 spawn = mWorld.spawnPosition();
    glm::vec3 eye = spawn + glm::vec3(0.0f, 1.7f, 0.0f);
    glm::vec3 direction = glm::normalize(glm::vec3(-0.6f, -0.25f, -0.7f));
    mScene.camera = Camera(eye, direction, glm::vec3(0.0f, 1.0f, 0.0f), 70.0f, float(SCR_WIDTH) / float(SCR_HEIGHT), 0.1f, 150.0f);
    mScene.camera.speed = 1.0f;
    mScene.camera.sensitivity = 0.12f;
    mScene.camera.updateProjectionMatrix();
    mScene.camera.updateViewMatrix();

    Logger::info("Minecraft demo initialized. Use WASD to walk and hold LMB to look around.");
}

void MinecraftDemo::buildMaterials() {
    mScene.materials.clear();
    mPalette.grass = static_cast<int>(mScene.materials.size());
    mScene.materials.emplace_back(glm::vec3(0.34f, 0.60f, 0.29f), 0.0f, 0.995f, 0.0f, 0.0f, 1.46f, MATERIAL_TEX_GRASS);
    mPalette.dirt = static_cast<int>(mScene.materials.size());
    mScene.materials.emplace_back(glm::vec3(0.41f, 0.29f, 0.18f), 0.0f, 0.99f, 0.0f, 0.0f, 1.5f, MATERIAL_TEX_DIRT);
    mPalette.stone = static_cast<int>(mScene.materials.size());
    mScene.materials.emplace_back(glm::vec3(0.54f, 0.55f, 0.57f), 0.0f, 0.78f, 0.04f, 0.0f, 1.5f, MATERIAL_TEX_STONE);
    mPalette.log = static_cast<int>(mScene.materials.size());
    mScene.materials.emplace_back(glm::vec3(0.38f, 0.24f, 0.14f), 0.0f, 0.99f, 0.0f, 0.0f, 1.48f, MATERIAL_TEX_LOG);
    mPalette.leaves = static_cast<int>(mScene.materials.size());
    mScene.materials.emplace_back(glm::vec3(0.25f, 0.42f, 0.24f), 0.0f, 0.97f, 0.0f, 0.0f, 1.45f, MATERIAL_TEX_LEAVES);
}

void MinecraftDemo::buildLighting() {
    mScene.lights.clear();
    glm::vec3 sunRays = glm::normalize(glm::vec3(-0.55f, -1.0f, -0.35f));
    glm::vec3 toSun = -sunRays; // shader expects direction from surface toward light
    mScene.lights.push_back(Light(glm::vec4(toSun, 0.0f), glm::vec3(1.0f, 0.96f, 0.90f), 6.0f));

    glm::vec3 skyFillRays = glm::normalize(glm::vec3(0.15f, -1.0f, 0.05f));
    glm::vec3 toSky = -skyFillRays;
    mScene.lights.push_back(Light(glm::vec4(toSky, 0.0f), glm::vec3(0.45f, 0.55f, 0.85f), 0.7f));
}

void MinecraftDemo::buildGeometry() {
    mScene.gameObjects.clear();
    mWorld.generate();
    mChunkMesh = mWorld.buildMesh(mPalette);
    mScene.gameObjects.push_back(GameObject{mChunkMesh, glm::mat4(1.0f)});
}
