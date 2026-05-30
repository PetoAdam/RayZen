#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <glm/glm.hpp>
#include <GLFW/glfw3.h>

#include "Scene.h"
#include "Mesh.h"
#include "Camera.h"

enum class BlockType : uint8_t {
    Air = 0,
    Grass,
    Dirt,
    Stone,
    Log,
    Leaves
};

struct MinecraftMaterialPalette {
    int grass = 0;
    int dirt = 0;
    int stone = 0;
    int log = 0;
    int leaves = 0;
};

class VoxelChunk {
public:
    VoxelChunk(int width, int height, int depth);

    int width() const { return mWidth; }
    int height() const { return mHeight; }
    int depth() const { return mDepth; }

    bool inBounds(int x, int y, int z) const;
    BlockType get(int x, int y, int z) const;
    void set(int x, int y, int z, BlockType type);
    void fill(BlockType type);

private:
    size_t index(int x, int y, int z) const;

    int mWidth;
    int mHeight;
    int mDepth;
    std::vector<BlockType> mBlocks;
};

class MinecraftWorld {
public:
    MinecraftWorld(int width, int height, int depth);

    void generate();
    std::shared_ptr<Mesh> buildMesh(const MinecraftMaterialPalette& palette) const;

    float sampleGroundHeight(float worldX, float worldZ) const;
    glm::vec3 spawnPosition() const { return mSpawnPosition; }
    glm::vec3 chunkOffset() const;

    int width() const { return mChunk.width(); }
    int depth() const { return mChunk.depth(); }

private:
    void carveTree(int baseX, int baseY, int baseZ, int trunkHeight);
    int surfaceHeightAt(int x, int z) const;
    BlockType blockAtOrAir(int x, int y, int z) const;

    VoxelChunk mChunk;
    std::vector<int> mHeightMap;
    glm::vec3 mSpawnPosition{0.0f};
};

class MinecraftPlayerController {
public:
    MinecraftPlayerController(MinecraftWorld& world, Camera& camera);

    void process(GLFWwindow* window, float deltaTime);

private:
    MinecraftWorld& mWorld;
    Camera& mCamera;
    float mWalkSpeed = 4.5f;
    float mEyeHeight = 1.7f;
    bool mIsDragging = false;
    double mLastX = 0.0;
    double mLastY = 0.0;
};

class MinecraftDemo {
public:
    explicit MinecraftDemo(Scene& scene);

    void initialize();
    MinecraftWorld& world() { return mWorld; }

private:
    void buildMaterials();
    void buildLighting();
    void buildGeometry();

    Scene& mScene;
    MinecraftWorld mWorld;
    MinecraftMaterialPalette mPalette;
    std::shared_ptr<Mesh> mChunkMesh;
};
