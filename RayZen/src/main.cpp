#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <cstdint>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <cstddef>
#include <cmath>
#include <algorithm>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <filesystem>
#include <system_error>
#include <chrono>
#include <memory>

#include "Ray.h"
#include "Scene.h"
#include "Camera.h"
#include "Material.h"
#include "Mesh.h"
#include "BVH.h"
#include "Logger.h"
#include "GameObject.h"

#ifndef RAYZEN_SOURCE_DIR
#define RAYZEN_SOURCE_DIR "."
#endif

namespace fs = std::filesystem;

// Constants
unsigned int SCR_WIDTH = 800;
unsigned int SCR_HEIGHT = 600;

// Function prototypes
void framebuffer_size_callback(GLFWwindow* window, int width, int height);
void processInput(GLFWwindow* window, Camera& camera, float deltaTime);
GLuint loadShaders(const char* vertexPath, const char* fragmentPath);
void sendSceneDataToShader(GLuint shaderProgram, const Scene& scene, int bounceBudget, int accumulatedFrames, bool resetAccumulation, int frameIndex);
void setupQuad(GLuint& quadVAO, GLuint& quadVBO);
void initializeSSBOs(const Scene& scene, bool forceRebuildBVH = false);
void updateDynamicBVHAndSSBOs(Scene& scene);
void buildRasterMeshes(const Scene& scene);
void renderRasterized(const Scene& scene);
void cleanupRasterMeshes();
void sendRasterSceneData(GLuint shaderProgram, const Scene& scene);
void runPathTracerWarmup(GLFWwindow* window, Scene& scene, int warmupFrames);

struct RenderConfig {
    int pathTracerBounceBudget = 3;
    int swapInterval = 0;
    float displayExposure = 1.0f;
    float pathTracerAmbient = 0.02f;
    float rasterAmbient = 0.03f;
    float pointLightPower = 300.0f;
    float directionalLightPower = 2.0f;
    bool accumulationEnabled = true;
};

struct PathTracerUniformLocations {
    GLint resolution = -1;
    GLint numTriangles = -1;
    GLint numLights = -1;
    GLint bounceBudget = -1;
    GLint accumulatedFrames = -1;
    GLint resetAccumulation = -1;
    GLint frameIndex = -1;
    GLint debugShowLights = -1;
    GLint debugShowBVH = -1;
    GLint debugBVHMode = -1;
    GLint debugSelectedBLAS = -1;
    GLint debugSelectedTri = -1;
    GLint uniformFps = -1;
    GLint displayExposure = -1;
    GLint ambientLightColor = -1;
    GLint cameraViewMatrix = -1;
    GLint cameraProjectionMatrix = -1;
    GLint cameraPosition = -1;
    GLint cameraInvViewMatrix = -1;
    GLint cameraInvProjectionMatrix = -1;
};

struct RasterUniformLocations {
    GLint view = -1;
    GLint projection = -1;
    GLint cameraPosition = -1;
    GLint numLights = -1;
    GLint ambientColor = -1;
    GLint model = -1;
    GLint normalMatrix = -1;
};

// Global variables
GLuint quadVAO, quadVBO;
GLuint shaderProgram;
GLuint rasterShaderProgram;
GLuint triangleSSBO, materialSSBO, lightSSBO;
GLuint tlasNodeSSBO, tlasTriIdxSSBO, blasNodeSSBO, blasTriIdxSSBO, bvhInstanceSSBO;
GLuint accumulationTexture = 0;
float lastFrame = 0.0f;
float deltaTime = 0.0f;
bool debugShowLights = false;
bool debugShowBVH = false;
int debugBVHMode = 0; // 0 = TLAS, 1 = BLAS
int debugSelectedBLAS = 0;
int debugSelectedTri = 0;
bool editorMode = false;
int gRequestedSwapInterval = 0;
bool gAccumulationDirty = true;
int gAccumulatedFrames = 0;
int gAccumulationWidth = 0;
int gAccumulationHeight = 0;
glm::mat4 gAccumulationViewMatrix(1.0f);
glm::mat4 gAccumulationProjectionMatrix(1.0f);
glm::vec3 gAccumulationCameraPosition(0.0f);
int gTotalTriangleCount = 0;
RenderConfig gRenderConfig;
fs::path gRenderConfigPath;
fs::file_time_type gRenderConfigTimestamp{};
bool gRenderConfigTimestampValid = false;
PathTracerUniformLocations gPathTracerUniforms;
RasterUniformLocations gRasterUniforms;

std::atomic<bool> gPathTracerReady{false};
std::atomic<GLuint> gPathTracerProgramHandle{0};
std::atomic<double> gPathTracerCompileMs{0.0};
std::thread gPathTracerThread;
GLFWwindow* gPathTracerCompileWindow = nullptr;

struct RasterMeshGPU {
    GLuint vao = 0;
    GLuint vbo = 0;
    GLsizei vertexCount = 0;
};

struct RasterVertex {
    glm::vec3 position;
    glm::vec3 normal;
    int materialIndex;
};

static std::unordered_map<const Mesh*, RasterMeshGPU> gRasterMeshCache;

namespace {

void resetPathTracerAccumulation();

std::string trim(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

bool parseBool(const std::string& value, bool& out) {
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "1" || lower == "true" || lower == "yes" || lower == "on") {
        out = true;
        return true;
    }
    if (lower == "0" || lower == "false" || lower == "no" || lower == "off") {
        out = false;
        return true;
    }
    return false;
}

std::string defaultRenderConfigText() {
    return
        "# RayZen render configuration\n"
        "# Edit while the app is running; the renderer hot-reloads this file.\n"
        "path_tracer_bounce_budget=3\n"
        "swap_interval=0\n"
        "display_exposure=1.0\n"
        "path_tracer_ambient=0.02\n"
        "raster_ambient=0.03\n"
        "point_light_power=300.0\n"
        "directional_light_power=2.0\n"
        "accumulation_enabled=true\n";
}

void ensureRenderConfigFileExists(const fs::path& configPath) {
    if (fs::exists(configPath)) {
        return;
    }

    std::error_code ec;
    fs::create_directories(configPath.parent_path(), ec);
    std::ofstream configFile(configPath);
    configFile << defaultRenderConfigText();
}

bool loadRenderConfigFromFile(const fs::path& configPath, RenderConfig& config, std::string& error) {
    std::ifstream input(configPath);
    if (!input.is_open()) {
        error = "Failed to open render config: " + configPath.string();
        return false;
    }

    RenderConfig nextConfig;
    std::string line;
    int lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        const std::size_t commentPos = line.find('#');
        if (commentPos != std::string::npos) {
            line = line.substr(0, commentPos);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }

        const std::size_t separatorPos = line.find('=');
        if (separatorPos == std::string::npos) {
            error = "Invalid config line " + std::to_string(lineNumber) + ": missing '='";
            return false;
        }

        const std::string key = trim(line.substr(0, separatorPos));
        const std::string value = trim(line.substr(separatorPos + 1));

        try {
            if (key == "path_tracer_bounce_budget") {
                nextConfig.pathTracerBounceBudget = std::clamp(std::stoi(value), 1, 16);
            } else if (key == "swap_interval") {
                nextConfig.swapInterval = std::clamp(std::stoi(value), 0, 2);
            } else if (key == "display_exposure") {
                nextConfig.displayExposure = std::clamp(std::stof(value), 0.05f, 16.0f);
            } else if (key == "path_tracer_ambient") {
                nextConfig.pathTracerAmbient = std::clamp(std::stof(value), 0.0f, 1.0f);
            } else if (key == "raster_ambient") {
                nextConfig.rasterAmbient = std::clamp(std::stof(value), 0.0f, 1.0f);
            } else if (key == "point_light_power") {
                nextConfig.pointLightPower = std::max(0.0f, std::stof(value));
            } else if (key == "directional_light_power") {
                nextConfig.directionalLightPower = std::max(0.0f, std::stof(value));
            } else if (key == "accumulation_enabled") {
                bool parsed = false;
                if (!parseBool(value, parsed)) {
                    error = "Invalid boolean for accumulation_enabled at line " + std::to_string(lineNumber);
                    return false;
                }
                nextConfig.accumulationEnabled = parsed;
            } else {
                error = "Unknown config key at line " + std::to_string(lineNumber) + ": " + key;
                return false;
            }
        } catch (const std::exception&) {
            error = "Invalid value for key '" + key + "' at line " + std::to_string(lineNumber);
            return false;
        }
    }

    config = nextConfig;
    return true;
}

void cachePathTracerUniformLocations(GLuint program) {
    gPathTracerUniforms = {};
    if (program == 0) {
        return;
    }

    gPathTracerUniforms.resolution = glGetUniformLocation(program, "resolution");
    gPathTracerUniforms.numTriangles = glGetUniformLocation(program, "numTriangles");
    gPathTracerUniforms.numLights = glGetUniformLocation(program, "numLights");
    gPathTracerUniforms.bounceBudget = glGetUniformLocation(program, "uniformBounceBudget");
    gPathTracerUniforms.accumulatedFrames = glGetUniformLocation(program, "uniformAccumulatedFrames");
    gPathTracerUniforms.resetAccumulation = glGetUniformLocation(program, "uniformResetAccumulation");
    gPathTracerUniforms.frameIndex = glGetUniformLocation(program, "uniformFrameIndex");
    gPathTracerUniforms.debugShowLights = glGetUniformLocation(program, "debugShowLights");
    gPathTracerUniforms.debugShowBVH = glGetUniformLocation(program, "debugShowBVH");
    gPathTracerUniforms.debugBVHMode = glGetUniformLocation(program, "debugBVHMode");
    gPathTracerUniforms.debugSelectedBLAS = glGetUniformLocation(program, "debugSelectedBLAS");
    gPathTracerUniforms.debugSelectedTri = glGetUniformLocation(program, "debugSelectedTri");
    gPathTracerUniforms.uniformFps = glGetUniformLocation(program, "uniformFps");
    gPathTracerUniforms.displayExposure = glGetUniformLocation(program, "uniformDisplayExposure");
    gPathTracerUniforms.ambientLightColor = glGetUniformLocation(program, "uniformAmbientLightColor");
    gPathTracerUniforms.cameraViewMatrix = glGetUniformLocation(program, "camera.viewMatrix");
    gPathTracerUniforms.cameraProjectionMatrix = glGetUniformLocation(program, "camera.projectionMatrix");
    gPathTracerUniforms.cameraPosition = glGetUniformLocation(program, "camera.position");
    gPathTracerUniforms.cameraInvViewMatrix = glGetUniformLocation(program, "camera.invViewMatrix");
    gPathTracerUniforms.cameraInvProjectionMatrix = glGetUniformLocation(program, "camera.invProjectionMatrix");
}

void cacheRasterUniformLocations(GLuint program) {
    gRasterUniforms = {};
    if (program == 0) {
        return;
    }

    gRasterUniforms.view = glGetUniformLocation(program, "uView");
    gRasterUniforms.projection = glGetUniformLocation(program, "uProj");
    gRasterUniforms.cameraPosition = glGetUniformLocation(program, "uCameraPos");
    gRasterUniforms.numLights = glGetUniformLocation(program, "numLights");
    gRasterUniforms.ambientColor = glGetUniformLocation(program, "uAmbientColor");
    gRasterUniforms.model = glGetUniformLocation(program, "uModel");
    gRasterUniforms.normalMatrix = glGetUniformLocation(program, "uNormalMatrix");
}

void uploadLightSSBO(const Scene& scene) {
    if (lightSSBO == 0) {
        return;
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, lightSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, scene.lights.size() * sizeof(Light), scene.lights.data());
}

bool applyRenderConfig(GLFWwindow* window, Scene& scene, const RenderConfig& nextConfig, bool sceneReady) {
    const RenderConfig previous = gRenderConfig;
    bool needsAccumulationReset = false;

    if (scene.lights.size() >= 1 && std::abs(scene.lights[0].power - nextConfig.pointLightPower) > 1e-5f) {
        scene.lights[0].power = nextConfig.pointLightPower;
        needsAccumulationReset = true;
    }
    if (scene.lights.size() >= 2 && std::abs(scene.lights[1].power - nextConfig.directionalLightPower) > 1e-5f) {
        scene.lights[1].power = nextConfig.directionalLightPower;
        needsAccumulationReset = true;
    }

    if (previous.pathTracerBounceBudget != nextConfig.pathTracerBounceBudget ||
        std::abs(previous.pathTracerAmbient - nextConfig.pathTracerAmbient) > 1e-5f ||
        previous.accumulationEnabled != nextConfig.accumulationEnabled) {
        needsAccumulationReset = true;
    }

    if (window != nullptr && previous.swapInterval != nextConfig.swapInterval) {
        gRequestedSwapInterval = nextConfig.swapInterval;
        glfwSwapInterval(gRequestedSwapInterval);
        Logger::info("Render config: swap interval updated to " + std::to_string(gRequestedSwapInterval));
    }

    gRenderConfig = nextConfig;

    if (sceneReady) {
        uploadLightSSBO(scene);
    }

    if (needsAccumulationReset) {
        resetPathTracerAccumulation();
    }

    return true;
}

void loadInitialRenderConfig(const fs::path& configPath) {
    ensureRenderConfigFileExists(configPath);
    std::string error;
    RenderConfig loadedConfig;
    if (!loadRenderConfigFromFile(configPath, loadedConfig, error)) {
        Logger::error(error + "; using defaults");
        loadedConfig = RenderConfig{};
    }
    gRenderConfig = loadedConfig;
    std::error_code ec;
    gRenderConfigTimestamp = fs::last_write_time(configPath, ec);
    gRenderConfigTimestampValid = !ec;
}

void reloadRenderConfigIfModified(GLFWwindow* window, Scene& scene, double currentTime, bool sceneReady) {
    static double nextPollTime = 0.0;
    if (currentTime < nextPollTime || gRenderConfigPath.empty()) {
        return;
    }
    nextPollTime = currentTime + 0.25;

    std::error_code ec;
    const auto timestamp = fs::last_write_time(gRenderConfigPath, ec);
    if (ec) {
        return;
    }
    if (gRenderConfigTimestampValid && timestamp == gRenderConfigTimestamp) {
        return;
    }

    RenderConfig loadedConfig;
    std::string error;
    if (!loadRenderConfigFromFile(gRenderConfigPath, loadedConfig, error)) {
        Logger::error(error);
        return;
    }

    gRenderConfigTimestamp = timestamp;
    gRenderConfigTimestampValid = true;
    applyRenderConfig(window, scene, loadedConfig, sceneReady);
    Logger::info("Reloaded render config from " + gRenderConfigPath.string());
}

glm::mat4 makeTransform(const glm::vec3& translation, const glm::vec3& scale = glm::vec3(1.0f)) {
    return glm::translate(glm::mat4(1.0f), translation) * glm::scale(glm::mat4(1.0f), scale);
}

std::string resolveProjectPath(const fs::path& relativePath) {
    return (fs::path(RAYZEN_SOURCE_DIR) / relativePath).string();
}

bool transformsMatch(const std::vector<GameObject>& gameObjects, const std::vector<glm::mat4>& transforms, float epsilon = 1e-5f) {
    if (gameObjects.size() != transforms.size()) {
        return false;
    }

    for (size_t objectIndex = 0; objectIndex < gameObjects.size(); ++objectIndex) {
        const glm::mat4& lhs = gameObjects[objectIndex].transform;
        const glm::mat4& rhs = transforms[objectIndex];
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                if (std::abs(lhs[column][row] - rhs[column][row]) > epsilon) {
                    return false;
                }
            }
        }
    }

    return true;
}

std::vector<glm::mat4> captureTransforms(const std::vector<GameObject>& gameObjects) {
    std::vector<glm::mat4> transforms;
    transforms.reserve(gameObjects.size());
    for (const auto& object : gameObjects) {
        transforms.push_back(object.transform);
    }
    return transforms;
}

bool matricesNearlyEqual(const glm::mat4& lhs, const glm::mat4& rhs, float epsilon = 1e-5f) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (std::abs(lhs[column][row] - rhs[column][row]) > epsilon) {
                return false;
            }
        }
    }
    return true;
}

bool vectorsNearlyEqual(const glm::vec3& lhs, const glm::vec3& rhs, float epsilon = 1e-5f) {
    return glm::all(glm::lessThanEqual(glm::abs(lhs - rhs), glm::vec3(epsilon)));
}

void resetPathTracerAccumulation() {
    gAccumulationDirty = true;
    gAccumulatedFrames = 0;
}

void ensureAccumulationTexture() {
    if (SCR_WIDTH == 0 || SCR_HEIGHT == 0) {
        return;
    }

    if (accumulationTexture == 0) {
        glGenTextures(1, &accumulationTexture);
        glBindTexture(GL_TEXTURE_2D, accumulationTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, accumulationTexture);
    }

    if (gAccumulationWidth != static_cast<int>(SCR_WIDTH) || gAccumulationHeight != static_cast<int>(SCR_HEIGHT)) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, static_cast<GLsizei>(SCR_WIDTH), static_cast<GLsizei>(SCR_HEIGHT), 0, GL_RGBA, GL_FLOAT, nullptr);
        const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        glClearTexImage(accumulationTexture, 0, GL_RGBA, GL_FLOAT, zero);
        gAccumulationWidth = static_cast<int>(SCR_WIDTH);
        gAccumulationHeight = static_cast<int>(SCR_HEIGHT);
        resetPathTracerAccumulation();
    }

    glBindImageTexture(0, accumulationTexture, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
    glBindTexture(GL_TEXTURE_2D, 0);
}

bool cameraStateMatchesAccumulation(const Camera& camera) {
    return vectorsNearlyEqual(camera.position, gAccumulationCameraPosition) &&
           matricesNearlyEqual(camera.viewMatrix, gAccumulationViewMatrix) &&
           matricesNearlyEqual(camera.projectionMatrix, gAccumulationProjectionMatrix);
}

void storeAccumulationCameraState(const Camera& camera) {
    gAccumulationCameraPosition = camera.position;
    gAccumulationViewMatrix = camera.viewMatrix;
    gAccumulationProjectionMatrix = camera.projectionMatrix;
}

} // namespace

struct ShaderBinaryMetadata {
    uint64_t vertexTimestamp = 0;
    uint64_t fragmentTimestamp = 0;
    uint32_t binaryFormat = 0;
    uint32_t binaryLength = 0;
};

// Serialize a vector of POD types to a binary file
template <typename T>
bool saveVectorToFile(const std::string& filename, const std::vector<T>& vec) {
    std::ofstream ofs(filename, std::ios::binary);
    if (!ofs) return false;
    size_t size = vec.size();
    ofs.write(reinterpret_cast<const char*>(&size), sizeof(size));
    ofs.write(reinterpret_cast<const char*>(vec.data()), sizeof(T) * size);
    return ofs.good();
}

template <typename T>
bool loadVectorFromFile(const std::string& filename, std::vector<T>& vec) {
    std::ifstream ifs(filename, std::ios::binary);
    if (!ifs) return false;
    size_t size = 0;
    ifs.read(reinterpret_cast<char*>(&size), sizeof(size));
    if (!ifs) return false;
    vec.resize(size);
    ifs.read(reinterpret_cast<char*>(vec.data()), sizeof(T) * size);
    return ifs.good();
}

// Save/load a BVH (nodes + triIndices)
bool saveBVHToFile(const std::string& base, const BVH& bvh) {
    return saveVectorToFile(base + ".nodes.bin", bvh.nodes) &&
           saveVectorToFile(base + ".tris.bin", bvh.triIndices);
}
bool loadBVHFromFile(const std::string& base, BVH& bvh) {
    return loadVectorFromFile(base + ".nodes.bin", bvh.nodes) &&
           loadVectorFromFile(base + ".tris.bin", bvh.triIndices);
}

// Save/load BVHInstance vector
bool saveBVHInstancesToFile(const std::string& filename, const std::vector<BVHInstance>& insts) {
    return saveVectorToFile(filename, insts);
}
bool loadBVHInstancesFromFile(const std::string& filename, std::vector<BVHInstance>& insts) {
    return loadVectorFromFile(filename, insts);
}

size_t expectedSceneTriangleCount(const Scene& scene) {
    size_t triangleCount = 0;
    for (const auto& object : scene.gameObjects) {
        if (object.mesh) {
            triangleCount += object.mesh->triangles.size();
        }
    }
    return triangleCount;
}

uint64_t computeSceneGeometryFingerprint(const Scene& scene) {
    uint64_t hash = 1469598103934665603ull;
    const uint64_t prime = 1099511628211ull;

    auto mixBytes = [&](const void* data, size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t index = 0; index < size; ++index) {
            hash ^= static_cast<uint64_t>(bytes[index]);
            hash *= prime;
        }
    };

    for (const auto& object : scene.gameObjects) {
        const size_t triangleCount = object.mesh ? object.mesh->triangles.size() : 0;
        mixBytes(&triangleCount, sizeof(triangleCount));
        if (!object.mesh) {
            continue;
        }
        for (const auto& triangle : object.mesh->triangles) {
            mixBytes(&triangle, sizeof(Triangle));
        }
    }

    return hash;
}

uint64_t computeMeshGeometryFingerprint(const Mesh& mesh) {
    uint64_t hash = 1469598103934665603ull;
    const uint64_t prime = 1099511628211ull;

    auto mixBytes = [&](const void* data, size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t index = 0; index < size; ++index) {
            hash ^= static_cast<uint64_t>(bytes[index]);
            hash *= prime;
        }
    };

    const size_t triangleCount = mesh.triangles.size();
    mixBytes(&triangleCount, sizeof(triangleCount));
    for (const auto& triangle : mesh.triangles) {
        mixBytes(&triangle, sizeof(Triangle));
    }

    return hash;
}

int main(int argc, char** argv) {
    // Parse CLI log level and BVH rebuild flag
    LogLevel logLevel = LogLevel::INFO;
    bool forceRebuildBVH = false;
    bool requestPathTracerOnly = false;
    int warmupFrames = 0;
    int swapInterval = 0;
    bool swapIntervalOverridden = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--log=debug") logLevel = LogLevel::DEBUG;
        else if (arg == "--log=info") logLevel = LogLevel::INFO;
        else if (arg == "--log=error") logLevel = LogLevel::ERROR;
        else if (arg == "--rebuild-bvh") forceRebuildBVH = true;
        else if (arg == "--path-tracer-only") requestPathTracerOnly = true;
        else if (arg.rfind("--swap-interval=", 0) == 0) {
            std::string value = arg.substr(std::string("--swap-interval=").size());
            try {
                swapInterval = std::clamp(std::stoi(value), 0, 2);
                swapIntervalOverridden = true;
            } catch (const std::exception&) {
                std::cerr << "Invalid value for --swap-interval: " << value << std::endl;
                swapInterval = 0;
            }
        }
        else if (arg.rfind("--warmup-frames=", 0) == 0) {
            std::string value = arg.substr(std::string("--warmup-frames=").size());
            try {
                warmupFrames = std::max(0, std::stoi(value));
            } catch (const std::exception&) {
                std::cerr << "Invalid value for --warmup-frames: " << value << std::endl;
                warmupFrames = 0;
            }
        }
    }
    if (warmupFrames > 0) {
        requestPathTracerOnly = true;
    }
    Logger::setLevel(logLevel);
    gRenderConfigPath = fs::path(RAYZEN_SOURCE_DIR) / "config" / "render_config.ini";
    loadInitialRenderConfig(gRenderConfigPath);
    if (swapIntervalOverridden) {
        gRenderConfig.swapInterval = swapInterval;
    }
    gRequestedSwapInterval = gRenderConfig.swapInterval;

    auto startupStart = std::chrono::high_resolution_clock::now();
    auto startupCheckpoint = startupStart;
    auto formatMs = [](double ms) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(3) << ms;
        return oss.str();
    };
    auto logStartupStep = [&](const std::string& label) {
        auto now = std::chrono::high_resolution_clock::now();
        double sinceLast = std::chrono::duration<double, std::milli>(now - startupCheckpoint).count();
        double total = std::chrono::duration<double, std::milli>(now - startupStart).count();
        Logger::info("Startup step [" + label + "]: " + formatMs(sinceLast) + " ms (" + formatMs(total) + " ms total)");
        startupCheckpoint = now;
    };

    auto loadMeshWithTiming = [&](const std::shared_ptr<Mesh>& mesh, const std::string& path, int materialIndex, const std::string& label) {
        auto begin = std::chrono::high_resolution_clock::now();
        bool ok = mesh->loadFromOBJ(path, materialIndex);
        auto end = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double, std::milli>(end - begin).count();
        if (!ok) {
            Logger::error("Mesh load failed [" + label + "] from " + path);
        } else {
            Logger::info("Mesh load [" + label + "] " + std::to_string(mesh->triangles.size()) + " tris in " + formatMs(elapsed) + " ms");
        }
        return ok;
    };

    // Print control scheme at startup
    Logger::info("==== RayZen Controls ====");
    Logger::info("WASD: Move camera");
    Logger::info("Mouse Drag (LMB): Rotate camera");
    Logger::info("L: Toggle light debug markers");
    Logger::info("B: Toggle BVH wireframe debug");
    Logger::info("N: Toggle BVH debug mode (TLAS/BLAS)");
    Logger::info("F2: Toggle swap interval (0/1)");
    Logger::info("F1: Toggle editor raster mode");
    Logger::info("ESC: Quit");
    Logger::info("========================");

    // Initialize GLFW
    if (!glfwInit()) {
        Logger::error("Failed to initialize GLFW");
        return -1;
    }

    struct ContextRequest {
        int major;
        int minor;
    };
    const ContextRequest candidates[] = {
        {4, 6},
        {4, 5},
        {4, 3},
        {3, 3}
    };

    GLFWwindow* window = nullptr;
    for (const auto& candidate : candidates) {
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, candidate.major);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, candidate.minor);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        if (candidate.major >= 4) {
            glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        }
        window = glfwCreateWindow(SCR_WIDTH, SCR_HEIGHT, "RayZen", nullptr, nullptr);
        if (window != nullptr) {
            Logger::info("Created OpenGL context " + std::to_string(candidate.major) + "." + std::to_string(candidate.minor));
            break;
        }
        Logger::info("Failed to create OpenGL context " + std::to_string(candidate.major) + "." + std::to_string(candidate.minor) + ", trying lower version");
    }

    if (window == nullptr) {
        Logger::error("Failed to create GLFW window with compatible OpenGL context");
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(gRequestedSwapInterval);
    logStartupStep("GLFW init + window");

    // Initialize GLEW
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        Logger::error("Failed to initialize GLEW");
        return -1;
    }
    const GLubyte* glVersion = glGetString(GL_VERSION);
    if (glVersion) {
        Logger::info(std::string("GL version: ") + reinterpret_cast<const char*>(glVersion));
    }
    GLFWmonitor* primaryMonitor = glfwGetPrimaryMonitor();
    if (primaryMonitor) {
        const GLFWvidmode* videoMode = glfwGetVideoMode(primaryMonitor);
        if (videoMode) {
            Logger::info("Primary monitor refresh rate: " + std::to_string(videoMode->refreshRate) + " Hz");
        }
    }
    Logger::info("Swap interval: " + std::to_string(gRequestedSwapInterval));
    logStartupStep("GLEW init");

    glViewport(0, 0, SCR_WIDTH, SCR_HEIGHT);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.05f, 0.05f, 0.07f, 1.0f);

    //glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);  // Hide the cursor when it's in the window

    // Load shaders
    bool awaitingPathTracerReady = false;
    auto rasterCompileStart = std::chrono::high_resolution_clock::now();
    const std::string rasterVertexShaderPath = resolveProjectPath("shaders/editor_vertex.glsl");
    const std::string rasterFragmentShaderPath = resolveProjectPath("shaders/editor_fragment.glsl");
    const std::string pathTracerVertexShaderPath = resolveProjectPath("shaders/vertex_shader.glsl");
    const std::string pathTracerFragmentShaderPath = resolveProjectPath("shaders/fragment_shader.glsl");
    const std::string cubeMeshPath = resolveProjectPath("meshes/cube.obj");
    const std::string monkeyMeshPath = resolveProjectPath("meshes/monkey.obj");
    rasterShaderProgram = loadShaders(rasterVertexShaderPath.c_str(), rasterFragmentShaderPath.c_str());
    auto rasterCompileEnd = std::chrono::high_resolution_clock::now();
    Logger::info(std::string("Raster shader compile/link time: ") + std::to_string(std::chrono::duration<double, std::milli>(rasterCompileEnd - rasterCompileStart).count()) + " ms");
    if (rasterShaderProgram == 0) {
        Logger::error("Failed to initialize raster shader program");
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }
    cacheRasterUniformLocations(rasterShaderProgram);

    int contextMajor = glfwGetWindowAttrib(window, GLFW_CONTEXT_VERSION_MAJOR);
    int contextMinor = glfwGetWindowAttrib(window, GLFW_CONTEXT_VERSION_MINOR);
    auto launchAsyncPathTracerCompile = [&](GLFWwindow* shareWindow) -> bool {
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, contextMajor);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, contextMinor);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        if (contextMajor >= 4) {
            glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        }
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        GLFWwindow* hidden = glfwCreateWindow(16, 16, "RayZenPathTracerCompile", nullptr, shareWindow);
        glfwDefaultWindowHints();
        if (!hidden) {
            return false;
        }
        gPathTracerCompileWindow = hidden;
        gPathTracerThread = std::thread([hidden, pathTracerVertexShaderPath, pathTracerFragmentShaderPath]() {
            glfwMakeContextCurrent(hidden);
            auto start = std::chrono::high_resolution_clock::now();
            GLuint program = loadShaders(pathTracerVertexShaderPath.c_str(), pathTracerFragmentShaderPath.c_str());
            auto end = std::chrono::high_resolution_clock::now();
            glFinish();
            gPathTracerProgramHandle.store(program, std::memory_order_release);
            gPathTracerCompileMs.store(std::chrono::duration<double, std::milli>(end - start).count(), std::memory_order_release);
            gPathTracerReady.store(true, std::memory_order_release);
            glfwMakeContextCurrent(nullptr);
        });
        return true;
    };
    bool userLockedEditorMode = false;
    bool forceImmediatePathTracer = requestPathTracerOnly || warmupFrames > 0;
    if (!forceImmediatePathTracer) {
        awaitingPathTracerReady = launchAsyncPathTracerCompile(window);
    }
    if (forceImmediatePathTracer || !awaitingPathTracerReady) {
        if (!forceImmediatePathTracer && !awaitingPathTracerReady) {
            Logger::info("Async path tracer compile unavailable; compiling on primary context");
        }
        auto pathTracerStart = std::chrono::high_resolution_clock::now();
        shaderProgram = loadShaders(pathTracerVertexShaderPath.c_str(), pathTracerFragmentShaderPath.c_str());
        auto pathTracerEnd = std::chrono::high_resolution_clock::now();
        gPathTracerCompileMs.store(std::chrono::duration<double, std::milli>(pathTracerEnd - pathTracerStart).count(), std::memory_order_release);
        gPathTracerReady.store(true, std::memory_order_release);
        Logger::info(std::string("Path tracer shader compile/link time: ") + std::to_string(gPathTracerCompileMs.load()) + " ms");
        if (shaderProgram == 0) {
            Logger::error("Failed to initialize path tracer shader program");
            glfwDestroyWindow(window);
            glfwTerminate();
            return -1;
        }
        cachePathTracerUniformLocations(shaderProgram);
    } else {
        shaderProgram = 0;
        editorMode = true;
        Logger::info("Path tracer shader compiling asynchronously; using editor raster mode until ready");
    }
    logStartupStep("Shader compilation");

    // Setup quad for rendering
    setupQuad(quadVAO, quadVBO);
    logStartupStep("Fullscreen quad setup");

    // Define scene
    Scene scene;

    // Define camera
    scene.camera = Camera(
        glm::vec3(0.0f, 0.75f, 5.5f), // Position
        glm::normalize(glm::vec3(0.0f, -0.08f, -1.0f)), // Direction
        glm::vec3(0.0f, 1.0f, 0.0f), // Up vector
        55.0f, // FOV
        float(SCR_WIDTH) / float(SCR_HEIGHT), // Aspect ratio
        0.1f, // Near plane
        100.0f // Far plane
    );

    // Define materials
    scene.materials = {
        // 0: Red matte
        Material(glm::vec3(0.95f, 0.05f, 0.05f), 0.0f, 0.95f, 0.0f, 0.0f, 1.5f),
        // 1: Green metallic (moderate roughness to avoid mirror-like look)
        Material(glm::vec3(0.08f, 0.72f, 0.08f), 1.0f, 0.35f, 0.3f, 0.0f, 1.5f),
        // 2: Mirror
        Material(glm::vec3(1.0f), 1.0f, 0.05f, 1.0f, 0.0f, 1.5f),
        // 3: Glass (nearly neutral tint)
        Material(glm::vec3(0.98f, 0.985f, 0.99f), 0.0f, 0.02f, 0.05f, 0.94f, 1.5f),
        // 4: Rough surface
        Material(glm::vec3(0.6f, 0.4f, 0.2f), 0.0f, 0.9f, 0.2f, 0.0f, 1.5f)
    };

    // Define lights
    scene.lights.push_back(Light(glm::vec4(5.0f, 5.0f, 5.0f, 1.0f), glm::vec3(1.0f, 1.0f, 1.0f), 300.0f)); // Point light at (5, 5, 5)
    scene.lights.push_back(Light(glm::vec4(0.8f, 1.4f, 0.3f, 0.0f), glm::vec3(1.0f, 1.0f, 1.0f), 2.0f)); // Directional light with direction (0.8, 1.4, 0.3)
    scene.lights[0].power = gRenderConfig.pointLightPower;
    scene.lights[1].power = gRenderConfig.directionalLightPower;

    // Define game objects
    // Create shared meshes
    auto floorMesh = std::make_shared<Mesh>();
    auto monkeyMesh = std::make_shared<Mesh>();
    auto glassMesh = std::make_shared<Mesh>();
    bool loadedFloor = loadMeshWithTiming(floorMesh, cubeMeshPath, 0, "floor");
    bool loadedMonkey = loadMeshWithTiming(monkeyMesh, monkeyMeshPath, 1, "monkey");
    bool loadedGlassMonkey = loadMeshWithTiming(glassMesh, monkeyMeshPath, 3, "glass monkey");
    if (!loadedFloor || !loadedMonkey || !loadedGlassMonkey) {
        Logger::error("Scene asset loading failed; aborting before cache initialization");
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }
    logStartupStep("Mesh loading");

    // Add GameObjects to the scene
    scene.gameObjects.push_back(GameObject{floorMesh, makeTransform(glm::vec3(0.0f, -2.2f, 0.0f), glm::vec3(12.0f, 0.25f, 12.0f))});
    scene.gameObjects.push_back(GameObject{monkeyMesh, makeTransform(glm::vec3(0.0f, -0.15f, 0.0f), glm::vec3(1.45f))});
    scene.gameObjects.push_back(GameObject{glassMesh, makeTransform(glm::vec3(2.75f, -0.65f, -1.65f), glm::vec3(0.9f))});
    gTotalTriangleCount = static_cast<int>(expectedSceneTriangleCount(scene));
    logStartupStep("Scene graph build");

    if (Logger::getLevel() <= LogLevel::DEBUG) {
        glm::vec3 worldMin(1e30f), worldMax(-1e30f);
        float closestHit = 1e30f;
        int closestMaterial = -1;

        auto intersectTriangle = [](const glm::vec3& origin, const glm::vec3& direction, const Triangle& triangle, float& tHit) {
            const glm::vec3 edge1 = triangle.v1 - triangle.v0;
            const glm::vec3 edge2 = triangle.v2 - triangle.v0;
            const glm::vec3 h = glm::cross(direction, edge2);
            const float a = glm::dot(edge1, h);
            if (std::abs(a) < 1e-6f) {
                return false;
            }
            const float f = 1.0f / a;
            const glm::vec3 s = origin - triangle.v0;
            const float u = f * glm::dot(s, h);
            if (u < 0.0f || u > 1.0f) {
                return false;
            }
            const glm::vec3 q = glm::cross(s, edge1);
            const float v = f * glm::dot(direction, q);
            if (v < 0.0f || u + v > 1.0f) {
                return false;
            }
            const float t = f * glm::dot(edge2, q);
            if (t <= 1e-4f) {
                return false;
            }
            tHit = t;
            return true;
        };

        for (const auto& object : scene.gameObjects) {
            for (const auto& triangle : object.mesh->triangles) {
                Triangle worldTriangle = triangle;
                worldTriangle.v0 = glm::vec3(object.transform * glm::vec4(triangle.v0, 1.0f));
                worldTriangle.v1 = glm::vec3(object.transform * glm::vec4(triangle.v1, 1.0f));
                worldTriangle.v2 = glm::vec3(object.transform * glm::vec4(triangle.v2, 1.0f));
                worldMin = glm::min(worldMin, glm::min(worldTriangle.v0, glm::min(worldTriangle.v1, worldTriangle.v2)));
                worldMax = glm::max(worldMax, glm::max(worldTriangle.v0, glm::max(worldTriangle.v1, worldTriangle.v2)));
                float tHit = 0.0f;
                if (intersectTriangle(scene.camera.position, glm::normalize(scene.camera.target), worldTriangle, tHit) && tHit < closestHit) {
                    closestHit = tHit;
                    closestMaterial = worldTriangle.materialIndex;
                }
            }
        }

        Logger::debug("Scene bounds min=" + std::to_string(worldMin.x) + "," + std::to_string(worldMin.y) + "," + std::to_string(worldMin.z) +
                      " max=" + std::to_string(worldMax.x) + "," + std::to_string(worldMax.y) + "," + std::to_string(worldMax.z));
        Logger::debug("Camera position=" + std::to_string(scene.camera.position.x) + "," + std::to_string(scene.camera.position.y) + "," + std::to_string(scene.camera.position.z) +
                      " direction=" + std::to_string(scene.camera.target.x) + "," + std::to_string(scene.camera.target.y) + "," + std::to_string(scene.camera.target.z));
        if (closestMaterial >= 0) {
            Logger::debug("Center camera ray hits scene at t=" + std::to_string(closestHit) + " material=" + std::to_string(closestMaterial));
        } else {
            Logger::debug("Center camera ray misses the scene");
        }
    }

    // Initialize SSBOs (initial build)
    initializeSSBOs(scene, forceRebuildBVH);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, triangleSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, materialSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, lightSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, tlasNodeSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, tlasTriIdxSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, blasNodeSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, blasTriIdxSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, bvhInstanceSSBO);
    logStartupStep("SSBO/BVH init");
    buildRasterMeshes(scene);
    logStartupStep("Raster mesh build");
    Logger::info("Glass monkey material index 3 at gameObject index " + std::to_string(scene.gameObjects.size() - 1));
    logStartupStep("Startup ready");

    ensureAccumulationTexture();
    storeAccumulationCameraState(scene.camera);
    resetPathTracerAccumulation();

    if (warmupFrames > 0 && shaderProgram != 0) {
        runPathTracerWarmup(window, scene, warmupFrames);
        resetPathTracerAccumulation();
        lastFrame = glfwGetTime();
    }

    // Main render loop
    bool firstFrame = true;
    double firstUseProgramMs = 0.0, firstDrawMs = 0.0;
    float animTime = 0.0f;
    int frameCounter = 0;
    const int frameLogLimit = 100;
    while (!glfwWindowShouldClose(window)) {
        auto frameStart = std::chrono::high_resolution_clock::now();

        if (awaitingPathTracerReady && gPathTracerReady.load(std::memory_order_acquire)) {
            GLuint program = gPathTracerProgramHandle.exchange(0, std::memory_order_acq_rel);
            if (program != 0) {
                shaderProgram = program;
                cachePathTracerUniformLocations(shaderProgram);
                awaitingPathTracerReady = false;
                if (!userLockedEditorMode) {
                    editorMode = false;
                }
                firstFrame = true;
                frameCounter = 0;
                glfwSwapInterval(gRequestedSwapInterval);
                resetPathTracerAccumulation();
                double compileMs = gPathTracerCompileMs.load(std::memory_order_acquire);
                Logger::info(std::string("Path tracer shader ready (compile/link time: ") + formatMs(compileMs) + " ms)" + (userLockedEditorMode ? std::string(" Press F1 to enable path tracer when ready.") : std::string("")));
            } else {
                Logger::error("Path tracer shader compilation failed; remaining in editor mode");
                awaitingPathTracerReady = false;
                userLockedEditorMode = true;
            }
        }

        // Calculate delta time
        float currentFrame = glfwGetTime();
        deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;
        reloadRenderConfigIfModified(window, scene, currentFrame, true);
        processInput(window, scene.camera, deltaTime);
        scene.camera.updateViewMatrix();
        auto afterInput = std::chrono::high_resolution_clock::now();

        static bool editorKeyPressed = false;
        if (glfwGetKey(window, GLFW_KEY_F1) == GLFW_PRESS) {
            if (!editorKeyPressed) {
                bool newEditorMode = !editorMode;
                if (!newEditorMode && shaderProgram == 0) {
                    Logger::info("Path tracer shader not ready yet; staying in editor mode");
                    userLockedEditorMode = true;
                } else {
                    editorMode = newEditorMode;
                    userLockedEditorMode = editorMode;
                    Logger::info(std::string("Editor raster mode: ") + (editorMode ? "On" : "Off"));
                    if (!editorMode) {
                        // Reset path tracer first-frame diagnostics when returning from editor mode
                        firstFrame = true;
                        frameCounter = 0;
                        glfwSwapInterval(gRequestedSwapInterval);
                        resetPathTracerAccumulation();
                    }
                }
                editorKeyPressed = true;
            }
        } else {
            editorKeyPressed = false;
        }

        static bool swapKeyPressed = false;
        if (glfwGetKey(window, GLFW_KEY_F2) == GLFW_PRESS) {
            if (!swapKeyPressed) {
                gRequestedSwapInterval = (gRequestedSwapInterval == 0) ? 1 : 0;
                glfwSwapInterval(gRequestedSwapInterval);
                Logger::info("Swap interval set to " + std::to_string(gRequestedSwapInterval));
                swapKeyPressed = true;
            }
        } else {
            swapKeyPressed = false;
        }

        // Toggle debugShowLights with 'L' key
        static bool lKeyPressed = false;
        if (glfwGetKey(window, GLFW_KEY_L) == GLFW_PRESS) {
            if (!lKeyPressed) {
                debugShowLights = !debugShowLights;
                lKeyPressed = true;
                Logger::info(std::string("Light debugging: ") + (debugShowLights ? "On" : "Off"));
            }
        } else {
            lKeyPressed = false;
        }

        // Toggle debugShowBVH with 'B' key
        static bool bKeyPressed = false;
        if (glfwGetKey(window, GLFW_KEY_B) == GLFW_PRESS) {
            if (!bKeyPressed) {
                debugShowBVH = !debugShowBVH;
                bKeyPressed = true;
                Logger::info(std::string("BVH wireframe debugging: ") + (debugShowBVH ? "On" : "Off"));
            }
        } else {
            bKeyPressed = false;
        }

        // Toggle debugBVHMode with 'N' key
        static bool nKeyPressed = false;
        if (glfwGetKey(window, GLFW_KEY_N) == GLFW_PRESS) {
            if (!nKeyPressed) {
                debugBVHMode = (debugBVHMode + 1) % 2;
                nKeyPressed = true;
                Logger::info(std::string("BVH debug mode: ") + (debugBVHMode == 0 ? "TLAS" : "BLAS"));
            }
        } else {
            nKeyPressed = false;
        }

        // Mouse picking for BLAS debug mode
        if (debugShowBVH && debugBVHMode == 1) {
            double mouseX, mouseY;
            glfwGetCursorPos(window, &mouseX, &mouseY);
            // Convert mouse to NDC
            float ndcX = 2.0f * float(mouseX) / float(SCR_WIDTH) - 1.0f;
            float ndcY = 1.0f - 2.0f * float(mouseY) / float(SCR_HEIGHT);
            // Unproject to world ray
            glm::vec4 rayClip(ndcX, ndcY, -1.0f, 1.0f);
            glm::mat4 invProj = glm::inverse(scene.camera.projectionMatrix);
            glm::mat4 invView = glm::inverse(scene.camera.viewMatrix);
            glm::vec4 rayEye = invProj * rayClip;
            rayEye = glm::vec4(rayEye.x, rayEye.y, -1.0f, 0.0f);
            glm::vec3 rayDir = glm::normalize(glm::vec3(invView * rayEye));
            glm::vec3 rayOrigin = scene.camera.position;
            float closestT = 1e30f;
            int pickedBLAS = -1, pickedTri = -1;
            for (size_t objIdx = 0; objIdx < scene.gameObjects.size(); ++objIdx) {
                const auto& obj = scene.gameObjects[objIdx];
                const auto& mesh = obj.mesh;
                glm::mat4 invTransform = glm::inverse(obj.transform);
                glm::vec3 localOrigin = glm::vec3(invTransform * glm::vec4(rayOrigin, 1.0f));
                glm::vec3 localDir = glm::normalize(glm::vec3(invTransform * glm::vec4(rayDir, 0.0f)));
                for (size_t triIdx = 0; triIdx < mesh->triangles.size(); ++triIdx) {
                    const auto& tri = mesh->triangles[triIdx];
                    // Möller–Trumbore intersection
                    glm::vec3 v0 = tri.v0, v1 = tri.v1, v2 = tri.v2;
                    glm::vec3 edge1 = v1 - v0;
                    glm::vec3 edge2 = v2 - v0;
                    glm::vec3 h = glm::cross(localDir, edge2);
                    float a = glm::dot(edge1, h);
                    if (fabs(a) < 1e-6f) continue;
                    float f = 1.0f / a;
                    glm::vec3 s = localOrigin - v0;
                    float u = f * glm::dot(s, h);
                    if (u < 0.0f || u > 1.0f) continue;
                    glm::vec3 q = glm::cross(s, edge1);
                    float v = f * glm::dot(localDir, q);
                    if (v < 0.0f || u + v > 1.0f) continue;
                    float t = f * glm::dot(edge2, q);
                    if (t > 0.0001f && t < closestT) {
                        closestT = t;
                        pickedBLAS = (int)objIdx;
                        pickedTri = (int)triIdx;
                    }
                }
            }
            if (pickedBLAS >= 0 && pickedTri >= 0) {
                debugSelectedBLAS = pickedBLAS;
                debugSelectedTri = pickedTri;
            }
        }

        // Update camera aspect ratio and projection matrix each frame
        scene.camera.aspectRatio = float(SCR_WIDTH) / float(SCR_HEIGHT);
        scene.camera.updateProjectionMatrix();

        if (!cameraStateMatchesAccumulation(scene.camera)) {
            resetPathTracerAccumulation();
        }

        // Rotate the first cube
        //scene.meshes[0].transform = glm::rotate(scene.meshes[0].transform, deltaTime, glm::vec3(0.0f, 1.0f, 0.0f));

        // Animate the moving cube (last in gameObjects)
        animTime += deltaTime;
        //if (!scene.gameObjects.empty()) {
        //    // Animate the last object (moving cube)
        //    GameObject& movingCube = scene.gameObjects.back();
        //    float x = -2.0f + 2.0f * sin(animTime);
        //    float y = 0.5f + 0.5f * cos(animTime * 0.5f);
        //    movingCube.transform = glm::translate(glm::mat4(1.0f), glm::vec3(x, y, 0.0f));
        //}

        // Update dynamic BVH/SSBOs for game objects
        updateDynamicBVHAndSSBOs(scene);
        auto afterBVH = std::chrono::high_resolution_clock::now();

        if (editorMode || shaderProgram == 0) {
            auto beforeRender = std::chrono::high_resolution_clock::now();
            renderRasterized(scene);
            auto afterRender = std::chrono::high_resolution_clock::now();
            glfwSwapBuffers(window);
            glfwPollEvents();
            auto frameEnd = std::chrono::high_resolution_clock::now();
            if (frameCounter < frameLogLimit) {
                double totalMs = std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();
                double inputMs = std::chrono::duration<double, std::milli>(afterInput - frameStart).count();
                double bvhMs = std::chrono::duration<double, std::milli>(afterBVH - afterInput).count();
                double renderMs = std::chrono::duration<double, std::milli>(afterRender - beforeRender).count();
                double swapMs = std::chrono::duration<double, std::milli>(frameEnd - afterRender).count();
                Logger::info("Frame " + std::to_string(frameCounter) + " [editor] timings: total=" + formatMs(totalMs) + " ms (input=" + formatMs(inputMs) + ", bvh=" + formatMs(bvhMs) + ", render=" + formatMs(renderMs) + ", swap=" + formatMs(swapMs) + ")");
            }
            ++frameCounter;
            continue;
        }

        ensureAccumulationTexture();
        const bool resetAccumulationThisFrame = gAccumulationDirty || !gRenderConfig.accumulationEnabled;

        auto useProgStart = std::chrono::high_resolution_clock::now();
        glUseProgram(shaderProgram);
        auto useProgEnd = std::chrono::high_resolution_clock::now();
        if (firstFrame) {
            firstUseProgramMs = std::chrono::duration<double, std::milli>(useProgEnd - useProgStart).count();
        }

        // Send scene data to the shader
        int bounceBudget = gRenderConfig.pathTracerBounceBudget;
        const int frameIndex = gRenderConfig.accumulationEnabled ? gAccumulatedFrames : frameCounter;
        sendSceneDataToShader(shaderProgram, scene, bounceBudget, gAccumulatedFrames, resetAccumulationThisFrame, frameIndex);
        auto afterSend = std::chrono::high_resolution_clock::now();

        if (gPathTracerUniforms.debugShowLights >= 0) {
            glUniform1i(gPathTracerUniforms.debugShowLights, debugShowLights ? 1 : 0);
        }
        if (gPathTracerUniforms.debugShowBVH >= 0) {
            glUniform1i(gPathTracerUniforms.debugShowBVH, debugShowBVH ? 1 : 0);
        }
        if (gPathTracerUniforms.debugBVHMode >= 0) {
            glUniform1i(gPathTracerUniforms.debugBVHMode, debugBVHMode);
        }
        if (gPathTracerUniforms.debugSelectedBLAS >= 0) {
            glUniform1i(gPathTracerUniforms.debugSelectedBLAS, debugSelectedBLAS);
        }
        if (gPathTracerUniforms.debugSelectedTri >= 0) {
            glUniform1i(gPathTracerUniforms.debugSelectedTri, debugSelectedTri);
        }

        // Render the quad
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        // Set FPS uniform for overlay
        static float smoothedFps = 0.0f;
        float fps = (deltaTime > 0.0f) ? (1.0f / deltaTime) : 0.0f;
        float alpha = 0.1f; // Smoothing factor (0.0 = no smoothing, 1.0 = instant)
        if (smoothedFps == 0.0f) smoothedFps = fps;
        else smoothedFps = alpha * fps + (1.0f - alpha) * smoothedFps;
        if (gPathTracerUniforms.uniformFps >= 0) {
            glUniform1f(gPathTracerUniforms.uniformFps, smoothedFps);
        }
        glBindVertexArray(quadVAO);
        auto drawStart = std::chrono::high_resolution_clock::now();
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        auto drawEnd = std::chrono::high_resolution_clock::now();
        if (firstFrame) {
            firstDrawMs = std::chrono::duration<double, std::milli>(drawEnd - drawStart).count();
            Logger::info(std::string("First glUseProgram time: ") + std::to_string(firstUseProgramMs) + " ms");
            Logger::info(std::string("First glDrawArrays time: ") + std::to_string(firstDrawMs) + " ms");
            firstFrame = false;
        }

        glfwSwapBuffers(window);
        glfwPollEvents();
        auto frameEnd = std::chrono::high_resolution_clock::now();

        if (frameCounter < frameLogLimit) {
            double totalMs = std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();
            double inputMs = std::chrono::duration<double, std::milli>(afterInput - frameStart).count();
            double bvhMs = std::chrono::duration<double, std::milli>(afterBVH - afterInput).count();
            double sendMs = std::chrono::duration<double, std::milli>(afterSend - afterBVH).count();
            double renderMs = std::chrono::duration<double, std::milli>(drawEnd - afterSend).count();
            double swapMs = std::chrono::duration<double, std::milli>(frameEnd - drawEnd).count();
            Logger::info("Frame " + std::to_string(frameCounter) + " timings: total=" + formatMs(totalMs) + " ms (input=" + formatMs(inputMs) + ", bvh=" + formatMs(bvhMs) + ", send=" + formatMs(sendMs) + ", render=" + formatMs(renderMs) + ", swap=" + formatMs(swapMs) + ")");
        }
        ++frameCounter;

        // Print fps on a single line at the bottom of the terminal only in DEBUG mode
        if (Logger::getLevel() <= LogLevel::DEBUG) {
            static double lastFpsPrintAt = 0.0;
            if (currentFrame - lastFpsPrintAt >= 0.25) {
                std::cout << "\rFPS: " << (1.0 / deltaTime) << "    " << std::flush;
                lastFpsPrintAt = currentFrame;
            }
        }

        gAccumulationDirty = false;
        if (gRenderConfig.accumulationEnabled) {
            ++gAccumulatedFrames;
        } else {
            gAccumulatedFrames = 0;
        }
        storeAccumulationCameraState(scene.camera);
    }

    // Cleanup
    if (gPathTracerThread.joinable()) {
        gPathTracerThread.join();
    }
    if (gPathTracerCompileWindow) {
        glfwDestroyWindow(gPathTracerCompileWindow);
        gPathTracerCompileWindow = nullptr;
    }
    cleanupRasterMeshes();
    if (accumulationTexture != 0) {
        glDeleteTextures(1, &accumulationTexture);
    }
    glDeleteVertexArrays(1, &quadVAO);
    glDeleteBuffers(1, &quadVBO);

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    SCR_WIDTH = width;
    SCR_HEIGHT = height;
    glViewport(0, 0, width, height);
    resetPathTracerAccumulation();
}

void processInput(GLFWwindow* window, Camera& camera, float deltaTime) {
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        glfwSetWindowShouldClose(window, true);
    
    // Camera movement
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
        camera.moveForward(deltaTime);
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
        camera.moveBackward(deltaTime);
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
        camera.moveLeft(deltaTime);
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
        camera.moveRight(deltaTime);

    // Apply mouse movement if the left mouse button is pressed
    // Handle mouse rotation only when the left mouse button is pressed
    static bool isDragging = false;
    static double lastX = SCR_WIDTH / 2.0f;
    static double lastY = SCR_HEIGHT / 2.0f;

    if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
        if (!isDragging) {
            // Initialize lastX, lastY when the drag starts
            glfwGetCursorPos(window, &lastX, &lastY);
            isDragging = true;
        }

        // Only apply mouse movement while dragging
        double mouseX, mouseY;
        glfwGetCursorPos(window, &mouseX, &mouseY);

        double offsetX = mouseX - lastX;
        double offsetY = lastY - mouseY; // Y-coordinates are inverted in OpenGL

        // Update rotation
        camera.rotate(offsetX, offsetY);

        // Update lastX and lastY for the next frame
        lastX = mouseX;
        lastY = mouseY;
    } else {
        // Reset dragging flag when mouse button is released
        isDragging = false;
    }
}

GLuint loadShaders(const char* vertexPath, const char* fragmentPath) {
    fs::path vertexAbs = fs::absolute(fs::path(vertexPath));
    fs::path fragmentAbs = fs::absolute(fs::path(fragmentPath));

    auto toTimestamp = [](const fs::path& path) -> uint64_t {
        std::error_code ec;
        auto ft = fs::last_write_time(path, ec);
        if (ec) return 0;
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(ft.time_since_epoch()).count());
    };

    uint64_t vertexTimestamp = toTimestamp(vertexAbs);
    uint64_t fragmentTimestamp = toTimestamp(fragmentAbs);

    fs::path cacheDir = vertexAbs.parent_path() / "cache";
    std::error_code cacheEc;
    fs::create_directories(cacheDir, cacheEc);

    std::string cacheKeyBase = vertexAbs.filename().string() + "_" + fragmentAbs.filename().string();
    std::string cacheKey = cacheKeyBase + "_" + std::to_string(std::hash<std::string>{}(vertexAbs.string() + "|" + fragmentAbs.string()));
    fs::path binaryPath = cacheDir / (cacheKey + ".bin");
    fs::path metaPath = cacheDir / (cacheKey + ".meta");

    static bool binarySupportChecked = false;
    static GLint binaryFormatCount = 0;
    if (!binarySupportChecked) {
        glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &binaryFormatCount);
        binarySupportChecked = true;
        if (binaryFormatCount <= 0) {
            Logger::info("GL program binary retrieval unavailable; shader caching disabled");
        }
    }
    bool canUseBinary = binaryFormatCount > 0;

    if (canUseBinary && fs::exists(binaryPath) && fs::exists(metaPath)) {
        ShaderBinaryMetadata meta{};
        std::ifstream metaFile(metaPath, std::ios::binary);
        if (metaFile.read(reinterpret_cast<char*>(&meta), sizeof(meta)) &&
            meta.vertexTimestamp == vertexTimestamp &&
            meta.fragmentTimestamp == fragmentTimestamp &&
            meta.binaryLength > 0) {
            std::vector<char> binary(meta.binaryLength);
            std::ifstream binFile(binaryPath, std::ios::binary);
            if (binFile.read(binary.data(), binary.size())) {
                GLuint program = glCreateProgram();
                glProgramBinary(program, meta.binaryFormat, binary.data(), static_cast<GLsizei>(binary.size()));
                GLint linked = GL_FALSE;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (linked == GL_TRUE) {
                    Logger::info("Loaded shader binary cache for " + cacheKeyBase);
                    return program;
                }
                Logger::info("Shader binary cache invalid for " + cacheKeyBase + ", recompiling");
                glDeleteProgram(program);
            }
        }
    }

    // Read vertex and fragment shader files
    std::ifstream vShaderFile(vertexAbs);
    std::ifstream fShaderFile(fragmentAbs);
    if (!vShaderFile.is_open() || !fShaderFile.is_open()) {
        Logger::error("Failed to open shader sources: " + vertexAbs.string() + " and/or " + fragmentAbs.string());
        return 0;
    }
    std::stringstream vShaderStream, fShaderStream;

    vShaderStream << vShaderFile.rdbuf();
    fShaderStream << fShaderFile.rdbuf();

    std::string vertexCode = vShaderStream.str();
    std::string fragmentCode = fShaderStream.str();

    const char* vShaderCode = vertexCode.c_str();
    const char* fShaderCode = fragmentCode.c_str();

    // Compile vertex shader
    GLuint vertex = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex, 1, &vShaderCode, nullptr);
    glCompileShader(vertex);
    int success;
    char infoLog[512];
    glGetShaderiv(vertex, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(vertex, 512, nullptr, infoLog);
        Logger::error(std::string("Vertex Shader Compilation Error: ") + infoLog);
        glDeleteShader(vertex);
        return 0;
    }

    // Compile fragment shader
    GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment, 1, &fShaderCode, nullptr);
    glCompileShader(fragment);
    glGetShaderiv(fragment, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(fragment, 512, nullptr, infoLog);
        Logger::error(std::string("Fragment Shader Compilation Error: ") + infoLog);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        return 0;
    }

    // Link shaders
    GLuint shaderProgram = glCreateProgram();
    if (canUseBinary) {
        glProgramParameteri(shaderProgram, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    }
    glAttachShader(shaderProgram, vertex);
    glAttachShader(shaderProgram, fragment);
    glLinkProgram(shaderProgram);
    glGetProgramiv(shaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(shaderProgram, 512, nullptr, infoLog);
        Logger::error(std::string("Shader Program Linking Error: ") + infoLog);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        glDeleteProgram(shaderProgram);
        return 0;
    }

    glDeleteShader(vertex);
    glDeleteShader(fragment);

    if (canUseBinary) {
        GLint binaryLength = 0;
        glGetProgramiv(shaderProgram, GL_PROGRAM_BINARY_LENGTH, &binaryLength);
        if (binaryLength > 0) {
            std::vector<char> binary(binaryLength);
            GLenum binaryFormat = 0;
            GLsizei lengthWritten = 0;
            glGetProgramBinary(shaderProgram, binaryLength, &lengthWritten, &binaryFormat, binary.data());
            if (lengthWritten > 0) {
                binary.resize(lengthWritten);
                ShaderBinaryMetadata meta{};
                meta.vertexTimestamp = vertexTimestamp;
                meta.fragmentTimestamp = fragmentTimestamp;
                meta.binaryFormat = binaryFormat;
                meta.binaryLength = static_cast<uint32_t>(binary.size());
                std::ofstream binOut(binaryPath, std::ios::binary);
                std::ofstream metaOut(metaPath, std::ios::binary);
                if (binOut.write(binary.data(), binary.size()) &&
                    metaOut.write(reinterpret_cast<const char*>(&meta), sizeof(meta))) {
                    Logger::info("Wrote shader binary cache for " + cacheKeyBase);
                }
            }
        }
    }

    return shaderProgram;
}

std::vector<Triangle> combineTriangles(const Scene& scene) {
    std::vector<Triangle> allTriangles;
    for (const auto& obj : scene.gameObjects) {
        for (const auto& tri : obj.mesh->triangles) {
            Triangle transformed;
            // Transform each vertex (assumes the obj.transform is rotation/translation/uniform scale)
            transformed.v0 = glm::vec3(obj.transform * glm::vec4(tri.v0, 1.0f));
            transformed.v1 = glm::vec3(obj.transform * glm::vec4(tri.v1, 1.0f));
            transformed.v2 = glm::vec3(obj.transform * glm::vec4(tri.v2, 1.0f));
            transformed.materialIndex = tri.materialIndex;
            allTriangles.push_back(transformed);
        }
    }
    return allTriangles;
}

void initializeSSBOs(const Scene& scene, bool forceRebuildBVH) {
    // Cache directory
    std::string cacheDir = "bvh_cache/v2/";
    if (!fs::exists(cacheDir)) {
        fs::create_directories(cacheDir);
        Logger::info("Created BVH cache directory: " + cacheDir);
    }

    // Try to load SSBO-ready data from cache
    std::vector<Triangle> allTriangles;
    std::vector<BVHNode> allBLASNodes;
    std::vector<int> allBLASTriIndices;
    std::vector<BVHInstance> meshInstances;
    std::vector<BVHNode> tlasNodes;
    std::vector<int> tlasTriIndices;
    bool loadedSSBOCache = false;
    std::string ssboCachePrefix = cacheDir + "ssbo_v2_";
    const size_t expectedTriangles = expectedSceneTriangleCount(scene);
    const uint64_t expectedFingerprint = computeSceneGeometryFingerprint(scene);
    const std::string sceneFingerprintPath = ssboCachePrefix + "fingerprint.bin";
    if (!forceRebuildBVH &&
        fs::exists(ssboCachePrefix + "triangles.bin") &&
        fs::exists(ssboCachePrefix + "blasnodes.bin") &&
        fs::exists(ssboCachePrefix + "blastris.bin") &&
        fs::exists(ssboCachePrefix + "instances.bin") &&
        fs::exists(ssboCachePrefix + "tlasnodes.bin") &&
        fs::exists(ssboCachePrefix + "tlastris.bin") &&
        fs::exists(sceneFingerprintPath)) {
        uint64_t cachedFingerprint = 0;
        std::ifstream fingerprintFile(sceneFingerprintPath, std::ios::binary);
        loadedSSBOCache =
            fingerprintFile.read(reinterpret_cast<char*>(&cachedFingerprint), sizeof(cachedFingerprint)).good() &&
            loadVectorFromFile(ssboCachePrefix + "triangles.bin", allTriangles) &&
            loadVectorFromFile(ssboCachePrefix + "blasnodes.bin", allBLASNodes) &&
            loadVectorFromFile(ssboCachePrefix + "blastris.bin", allBLASTriIndices) &&
            loadVectorFromFile(ssboCachePrefix + "instances.bin", meshInstances) &&
            loadVectorFromFile(ssboCachePrefix + "tlasnodes.bin", tlasNodes) &&
            loadVectorFromFile(ssboCachePrefix + "tlastris.bin", tlasTriIndices);
        if (loadedSSBOCache) {
            // Invalidate if scene object count changed since cache write
            if (meshInstances.size() != scene.gameObjects.size()) {
                Logger::info("Cache invalidated: game object count changed (was " + std::to_string(meshInstances.size()) + ", now " + std::to_string(scene.gameObjects.size()) + ")");
                loadedSSBOCache = false;
                allTriangles.clear(); allBLASNodes.clear(); allBLASTriIndices.clear();
                meshInstances.clear(); tlasNodes.clear(); tlasTriIndices.clear();
            } else if (cachedFingerprint != expectedFingerprint) {
                Logger::info("Cache invalidated: scene geometry fingerprint changed");
                loadedSSBOCache = false;
                allTriangles.clear(); allBLASNodes.clear(); allBLASTriIndices.clear();
                meshInstances.clear(); tlasNodes.clear(); tlasTriIndices.clear();
            } else if (allTriangles.size() != expectedTriangles || allBLASTriIndices.size() != expectedTriangles) {
                Logger::info("Cache invalidated: geometry triangle count changed (cache has " + std::to_string(allTriangles.size()) + ", scene has " + std::to_string(expectedTriangles) + ")");
                loadedSSBOCache = false;
                allTriangles.clear(); allBLASNodes.clear(); allBLASTriIndices.clear();
                meshInstances.clear(); tlasNodes.clear(); tlasTriIndices.clear();
            } else {
                Logger::info("Loaded SSBO data from cache");
            }
        }
    }

    if (!loadedSSBOCache) {
        std::vector<BVH> meshBLAS(scene.gameObjects.size());
        std::vector<BVHNode> worldRootNodes;
        worldRootNodes.reserve(scene.gameObjects.size());
        int nodeOffset = 0;
        int triOffset = 0;
        bool loadedAllBLAS = true;
        meshInstances.clear();
        allTriangles.clear();

        for (size_t i = 0; i < scene.gameObjects.size(); ++i) {
            const auto& obj = scene.gameObjects[i];
            const auto& mesh = obj.mesh;
            const auto& meshTris = mesh->triangles;
            std::string meshName = "mesh" + std::to_string(i);
            std::string blasBase = cacheDir + meshName;
            std::string blasFingerprintPath = blasBase + ".fingerprint.bin";
            bool loaded = false;
            if (!forceRebuildBVH &&
                fs::exists(blasBase + ".nodes.bin") &&
                fs::exists(blasBase + ".tris.bin") &&
                fs::exists(blasFingerprintPath)) {
                uint64_t cachedMeshFingerprint = 0;
                uint64_t expectedMeshFingerprint = computeMeshGeometryFingerprint(*mesh);
                std::ifstream fingerprintFile(blasFingerprintPath, std::ios::binary);
                if (fingerprintFile.read(reinterpret_cast<char*>(&cachedMeshFingerprint), sizeof(cachedMeshFingerprint)).good() &&
                    cachedMeshFingerprint == expectedMeshFingerprint) {
                    loaded = loadBVHFromFile(blasBase, meshBLAS[i]);
                }
                if (loaded) {
                    Logger::info("Loaded BLAS from cache for " + meshName + " (" + std::to_string(meshBLAS[i].nodes.size()) + " nodes)");
                } else if (cachedMeshFingerprint != expectedMeshFingerprint) {
                    Logger::info("BLAS cache invalidated for " + meshName + ": mesh geometry fingerprint changed");
                }
            }
            if (!loaded) {
                Logger::info("Building BLAS from scratch for " + meshName);
                meshBLAS[i].buildBLAS(meshTris);
                saveBVHToFile(blasBase, meshBLAS[i]);
                uint64_t meshFingerprint = computeMeshGeometryFingerprint(*mesh);
                std::ofstream fingerprintFile(blasFingerprintPath, std::ios::binary);
                fingerprintFile.write(reinterpret_cast<const char*>(&meshFingerprint), sizeof(meshFingerprint));
                Logger::info("Saved BLAS to cache for " + meshName);
            }

            size_t triBase = allTriangles.size();
            allTriangles.insert(allTriangles.end(), meshTris.begin(), meshTris.end());

            const BVHNode& meshRoot = meshBLAS[i].nodes[0];
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
                glm::vec3 tc = glm::vec3(obj.transform * glm::vec4(corners[c], 1.0f));
                bmin = glm::min(bmin, tc);
                bmax = glm::max(bmax, tc);
            }
            BVHNode instRoot = meshRoot;
            instRoot.boundsMin = bmin;
            instRoot.boundsMax = bmax;
            worldRootNodes.push_back(instRoot);

            BVHInstance inst{};
            inst.blasNodeOffset = nodeOffset;
            inst.blasTriOffset = triOffset;
            inst.globalTriOffset = static_cast<int>(triBase);
            inst.meshIndex = static_cast<int>(i);
            inst.transform = obj.transform;
            inst.inverseTransform = glm::inverse(obj.transform);
            meshInstances.push_back(inst);

            nodeOffset += static_cast<int>(meshBLAS[i].nodes.size());
            triOffset += static_cast<int>(meshBLAS[i].triIndices.size());
            loadedAllBLAS &= loaded;
        }

        BVH tlas;
        tlas.nodes.clear();
        tlas.triIndices.clear();
        Logger::info("Building TLAS from current scene transforms");
        tlas.buildTLAS(meshInstances, worldRootNodes);
        saveBVHInstancesToFile(cacheDir + "instances.bin", meshInstances);

        allBLASNodes.clear();
        allBLASTriIndices.clear();
        for (const auto& blas : meshBLAS) {
            allBLASNodes.insert(allBLASNodes.end(), blas.nodes.begin(), blas.nodes.end());
            allBLASTriIndices.insert(allBLASTriIndices.end(), blas.triIndices.begin(), blas.triIndices.end());
        }
        tlasNodes = tlas.nodes;
        tlasTriIndices = tlas.triIndices;

        saveVectorToFile(ssboCachePrefix + "triangles.bin", allTriangles);
        saveVectorToFile(ssboCachePrefix + "blasnodes.bin", allBLASNodes);
        saveVectorToFile(ssboCachePrefix + "blastris.bin", allBLASTriIndices);
        saveVectorToFile(ssboCachePrefix + "instances.bin", meshInstances);
        saveVectorToFile(ssboCachePrefix + "tlasnodes.bin", tlasNodes);
        saveVectorToFile(ssboCachePrefix + "tlastris.bin", tlasTriIndices);
        std::ofstream fingerprintFile(sceneFingerprintPath, std::ios::binary);
        fingerprintFile.write(reinterpret_cast<const char*>(&expectedFingerprint), sizeof(expectedFingerprint));
        Logger::info("Saved SSBO data to cache");

        for (size_t i = 0; i < tlasTriIndices.size(); ++i) {
            if (tlasTriIndices[i] < 0 || tlasTriIndices[i] >= static_cast<int>(meshInstances.size())) {
                Logger::error("Invalid TLAS tri index at " + std::to_string(i) + ": " + std::to_string(tlasTriIndices[i]));
            }
        }
    }

    if (loadedSSBOCache) {
        for (size_t i = 0; i < meshInstances.size() && i < scene.gameObjects.size(); ++i) {
            meshInstances[i].transform = scene.gameObjects[i].transform;
            meshInstances[i].meshIndex = static_cast<int>(i);
            meshInstances[i].inverseTransform = glm::inverse(scene.gameObjects[i].transform);
        }
    }

    Logger::info("Initializing SSBOs for triangles, materials, lights, BVHs, and instances");

    auto logBufferUpload = [](const char* name, size_t bytes, auto uploadFunc) {
        auto start = std::chrono::high_resolution_clock::now();
        uploadFunc();
        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        Logger::info(std::string("SSBO upload: ") + name + ", size: " + std::to_string(bytes/1024) + " KB, time: " + std::to_string(ms) + " ms");
    };

    logBufferUpload("Triangles", allTriangles.size() * sizeof(Triangle), [&]() {
        glGenBuffers(1, &triangleSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, triangleSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER, allTriangles.size() * sizeof(Triangle), allTriangles.data(), GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, triangleSSBO);
    });
    logBufferUpload("Materials", scene.materials.size() * sizeof(Material), [&]() {
        glGenBuffers(1, &materialSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, materialSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER, scene.materials.size() * sizeof(Material), scene.materials.data(), GL_STATIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, materialSSBO);
    });
    logBufferUpload("Lights", scene.lights.size() * sizeof(Light), [&]() {
        glGenBuffers(1, &lightSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, lightSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER, scene.lights.size() * sizeof(Light), scene.lights.data(), GL_STATIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, lightSSBO);
    });
    logBufferUpload("TLAS Nodes", tlasNodes.size() * sizeof(BVHNode), [&]() {
        glGenBuffers(1, &tlasNodeSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, tlasNodeSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER, tlasNodes.size() * sizeof(BVHNode), tlasNodes.data(), GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, tlasNodeSSBO);
    });
    logBufferUpload("TLAS Tri Indices", tlasTriIndices.size() * sizeof(int), [&]() {
        glGenBuffers(1, &tlasTriIdxSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, tlasTriIdxSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER, tlasTriIndices.size() * sizeof(int), tlasTriIndices.data(), GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, tlasTriIdxSSBO);
    });
    logBufferUpload("BLAS Nodes", allBLASNodes.size() * sizeof(BVHNode), [&]() {
        glGenBuffers(1, &blasNodeSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, blasNodeSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER, allBLASNodes.size() * sizeof(BVHNode), allBLASNodes.data(), GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, blasNodeSSBO);
    });
    logBufferUpload("BLAS Tri Indices", allBLASTriIndices.size() * sizeof(int), [&]() {
        glGenBuffers(1, &blasTriIdxSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, blasTriIdxSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER, allBLASTriIndices.size() * sizeof(int), allBLASTriIndices.data(), GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, blasTriIdxSSBO);
    });
    logBufferUpload("BVH Instances", meshInstances.size() * sizeof(BVHInstance), [&]() {
        glGenBuffers(1, &bvhInstanceSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, bvhInstanceSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER, meshInstances.size() * sizeof(BVHInstance), meshInstances.data(), GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, bvhInstanceSSBO);
    });
}

// Dynamic BVH/SSBO update for game objects
void updateDynamicBVHAndSSBOs(Scene& scene) {
    static std::vector<BVH> meshBLAS;
    static std::vector<BVHNode> meshRootNodes;
    static std::vector<glm::mat4> previousTransforms;
    if (meshBLAS.size() != scene.gameObjects.size()) {
        meshBLAS.resize(scene.gameObjects.size());
        meshRootNodes.resize(scene.gameObjects.size());
        for (size_t i = 0; i < scene.gameObjects.size(); ++i) {
            meshBLAS[i].buildBLAS(scene.gameObjects[i].mesh->triangles);
            meshRootNodes[i] = meshBLAS[i].nodes[0];
        }
        previousTransforms.clear();
    }

    if (transformsMatch(scene.gameObjects, previousTransforms)) {
        return;
    }

    std::vector<BVHInstance> meshInstances;
    meshInstances.reserve(scene.gameObjects.size());
    for (size_t i = 0; i < scene.gameObjects.size(); ++i) {
        BVHInstance inst;
        inst.blasNodeOffset = 0;
        inst.blasTriOffset = 0;
        inst.globalTriOffset = 0;
        for (int j = 0; j < i; ++j) {
            inst.blasNodeOffset += static_cast<int>(meshBLAS[j].nodes.size());
            inst.blasTriOffset += static_cast<int>(meshBLAS[j].triIndices.size());
            inst.globalTriOffset += static_cast<int>(scene.gameObjects[j].mesh->triangles.size());
        }
        inst.transform = scene.gameObjects[i].transform;
        inst.inverseTransform = glm::inverse(scene.gameObjects[i].transform);
        inst.meshIndex = static_cast<int>(i);
        meshInstances.push_back(inst);
    }

    std::vector<BVHNode> instanceRootNodes;
    instanceRootNodes.reserve(scene.gameObjects.size());
    for (size_t i = 0; i < scene.gameObjects.size(); ++i) {
        const BVHNode& meshRoot = meshRootNodes[i];
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
            glm::vec3 tc = glm::vec3(scene.gameObjects[i].transform * glm::vec4(corners[c], 1.0f));
            bmin = glm::min(bmin, tc);
            bmax = glm::max(bmax, tc);
        }
        BVHNode instRoot = meshRoot;
        instRoot.boundsMin = bmin;
        instRoot.boundsMax = bmax;
        instanceRootNodes.push_back(instRoot);
    }
    static BVH tlas;
    tlas.buildTLAS(meshInstances, instanceRootNodes);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, bvhInstanceSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, meshInstances.size() * sizeof(BVHInstance), meshInstances.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, tlasNodeSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, tlas.nodes.size() * sizeof(BVHNode), tlas.nodes.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, tlasTriIdxSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, tlas.triIndices.size() * sizeof(int), tlas.triIndices.data());

    previousTransforms = captureTransforms(scene.gameObjects);
}

void buildRasterMeshes(const Scene& scene) {
    for (const auto& obj : scene.gameObjects) {
        const Mesh* meshPtr = obj.mesh.get();
        if (!meshPtr) continue;
        if (gRasterMeshCache.find(meshPtr) != gRasterMeshCache.end()) continue;
        if (meshPtr->triangles.empty()) continue;

        std::vector<RasterVertex> vertices;
        vertices.reserve(meshPtr->triangles.size() * 3);

        for (const auto& tri : meshPtr->triangles) {
            glm::vec3 p0 = tri.v0;
            glm::vec3 p1 = tri.v1;
            glm::vec3 p2 = tri.v2;
            glm::vec3 normal = glm::normalize(glm::cross(p1 - p0, p2 - p0));
            if (!std::isfinite(normal.x) || !std::isfinite(normal.y) || !std::isfinite(normal.z) || glm::length(normal) < 1e-5f) {
                normal = glm::vec3(0.0f, 1.0f, 0.0f);
            }

            RasterVertex v0{p0, normal, tri.materialIndex};
            RasterVertex v1{p1, normal, tri.materialIndex};
            RasterVertex v2{p2, normal, tri.materialIndex};
            vertices.push_back(v0);
            vertices.push_back(v1);
            vertices.push_back(v2);
        }

        if (vertices.empty()) continue;

        RasterMeshGPU gpu;
        glGenVertexArrays(1, &gpu.vao);
        glGenBuffers(1, &gpu.vbo);
        glBindVertexArray(gpu.vao);
        glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(RasterVertex), vertices.data(), GL_STATIC_DRAW);

        GLsizei stride = sizeof(RasterVertex);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(RasterVertex, position)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(RasterVertex, normal)));
        glEnableVertexAttribArray(2);
        glVertexAttribIPointer(2, 1, GL_INT, stride, reinterpret_cast<void*>(offsetof(RasterVertex, materialIndex)));

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        gpu.vertexCount = static_cast<GLsizei>(vertices.size());
        gRasterMeshCache[meshPtr] = gpu;
    }
}

void sendRasterSceneData(GLuint shaderProgram, const Scene& scene) {
    if (gRasterUniforms.view >= 0) {
        glUniformMatrix4fv(gRasterUniforms.view, 1, GL_FALSE, glm::value_ptr(scene.camera.viewMatrix));
    }
    if (gRasterUniforms.projection >= 0) {
        glUniformMatrix4fv(gRasterUniforms.projection, 1, GL_FALSE, glm::value_ptr(scene.camera.projectionMatrix));
    }
    if (gRasterUniforms.cameraPosition >= 0) {
        glUniform3fv(gRasterUniforms.cameraPosition, 1, glm::value_ptr(scene.camera.position));
    }
    if (gRasterUniforms.numLights >= 0) {
        glUniform1i(gRasterUniforms.numLights, static_cast<int>(scene.lights.size()));
    }
    if (gRasterUniforms.ambientColor >= 0) {
        glUniform3f(gRasterUniforms.ambientColor, gRenderConfig.rasterAmbient, gRenderConfig.rasterAmbient, gRenderConfig.rasterAmbient);
    }
}

void renderRasterized(const Scene& scene) {
    glEnable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(rasterShaderProgram);
    sendRasterSceneData(rasterShaderProgram, scene);

    for (const auto& obj : scene.gameObjects) {
        const Mesh* meshPtr = obj.mesh.get();
        auto it = gRasterMeshCache.find(meshPtr);
        if (it == gRasterMeshCache.end()) continue;

        const RasterMeshGPU& gpu = it->second;
        if (gpu.vertexCount == 0) continue;

        glm::mat4 model = obj.transform;
        glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(model)));

        if (gRasterUniforms.model >= 0) {
            glUniformMatrix4fv(gRasterUniforms.model, 1, GL_FALSE, glm::value_ptr(model));
        }
        if (gRasterUniforms.normalMatrix >= 0) {
            glUniformMatrix3fv(gRasterUniforms.normalMatrix, 1, GL_FALSE, glm::value_ptr(normalMatrix));
        }

        glBindVertexArray(gpu.vao);
        glDrawArrays(GL_TRIANGLES, 0, gpu.vertexCount);
    }

    glBindVertexArray(0);
    glUseProgram(0);
}

void cleanupRasterMeshes() {
    for (auto& kv : gRasterMeshCache) {
        if (kv.second.vbo != 0) {
            glDeleteBuffers(1, &kv.second.vbo);
        }
        if (kv.second.vao != 0) {
            glDeleteVertexArrays(1, &kv.second.vao);
        }
    }
    gRasterMeshCache.clear();
}

void runPathTracerWarmup(GLFWwindow* window, Scene& scene, int warmupFrames) {
    if (warmupFrames <= 0 || shaderProgram == 0) {
        return;
    }
    Logger::info("Running path tracer warm-up for " + std::to_string(warmupFrames) + " frame(s) before enabling interactive rendering");
    glfwHideWindow(window);
    glfwSwapInterval(0);
    for (int i = 0; i < warmupFrames; ++i) {
        ensureAccumulationTexture();
        glUseProgram(shaderProgram);
        int bounceBudget = (i == 0) ? 1 : gRenderConfig.pathTracerBounceBudget;
        sendSceneDataToShader(shaderProgram, scene, bounceBudget, 0, true, i);
        if (gPathTracerUniforms.debugShowLights >= 0) {
            glUniform1i(gPathTracerUniforms.debugShowLights, debugShowLights ? 1 : 0);
        }
        if (gPathTracerUniforms.debugShowBVH >= 0) {
            glUniform1i(gPathTracerUniforms.debugShowBVH, debugShowBVH ? 1 : 0);
        }
        if (gPathTracerUniforms.debugBVHMode >= 0) {
            glUniform1i(gPathTracerUniforms.debugBVHMode, debugBVHMode);
        }
        if (gPathTracerUniforms.debugSelectedBLAS >= 0) {
            glUniform1i(gPathTracerUniforms.debugSelectedBLAS, debugSelectedBLAS);
        }
        if (gPathTracerUniforms.debugSelectedTri >= 0) {
            glUniform1i(gPathTracerUniforms.debugSelectedTri, debugSelectedTri);
        }
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBindVertexArray(quadVAO);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        glFinish();
        glfwSwapBuffers(window);
        glfwPollEvents();
    }
    glBindVertexArray(0);
    glfwShowWindow(window);
    glfwSwapInterval(gRequestedSwapInterval);
    Logger::info("Warm-up complete; first on-screen frame should reuse the cached pipeline");
}

void sendSceneDataToShader(GLuint shaderProgram, const Scene& scene, int bounceBudget, int accumulatedFrames, bool resetAccumulation, int frameIndex) {
    glm::mat4 invViewMatrix = glm::inverse(scene.camera.viewMatrix);
    glm::mat4 invProjMatrix = glm::inverse(scene.camera.projectionMatrix);

    if (gPathTracerUniforms.resolution >= 0) {
        glUniform2f(gPathTracerUniforms.resolution, float(SCR_WIDTH), float(SCR_HEIGHT));
    }
    if (gPathTracerUniforms.numTriangles >= 0) {
        glUniform1i(gPathTracerUniforms.numTriangles, gTotalTriangleCount);
    }
    if (gPathTracerUniforms.numLights >= 0) {
        glUniform1i(gPathTracerUniforms.numLights, static_cast<int>(scene.lights.size()));
    }
    if (gPathTracerUniforms.bounceBudget >= 0) {
        glUniform1i(gPathTracerUniforms.bounceBudget, bounceBudget);
    }
    if (gPathTracerUniforms.accumulatedFrames >= 0) {
        glUniform1i(gPathTracerUniforms.accumulatedFrames, gRenderConfig.accumulationEnabled ? accumulatedFrames : 0);
    }
    if (gPathTracerUniforms.resetAccumulation >= 0) {
        glUniform1i(gPathTracerUniforms.resetAccumulation, resetAccumulation ? 1 : 0);
    }
    if (gPathTracerUniforms.frameIndex >= 0) {
        glUniform1i(gPathTracerUniforms.frameIndex, frameIndex);
    }
    if (gPathTracerUniforms.displayExposure >= 0) {
        glUniform1f(gPathTracerUniforms.displayExposure, gRenderConfig.displayExposure);
    }
    if (gPathTracerUniforms.ambientLightColor >= 0) {
        glUniform3f(gPathTracerUniforms.ambientLightColor, gRenderConfig.pathTracerAmbient, gRenderConfig.pathTracerAmbient, gRenderConfig.pathTracerAmbient);
    }
    if (gPathTracerUniforms.cameraViewMatrix >= 0) {
        glUniformMatrix4fv(gPathTracerUniforms.cameraViewMatrix, 1, GL_FALSE, glm::value_ptr(scene.camera.viewMatrix));
    }
    if (gPathTracerUniforms.cameraProjectionMatrix >= 0) {
        glUniformMatrix4fv(gPathTracerUniforms.cameraProjectionMatrix, 1, GL_FALSE, glm::value_ptr(scene.camera.projectionMatrix));
    }
    if (gPathTracerUniforms.cameraPosition >= 0) {
        glUniform3fv(gPathTracerUniforms.cameraPosition, 1, glm::value_ptr(scene.camera.position));
    }
    if (gPathTracerUniforms.cameraInvViewMatrix >= 0) {
        glUniformMatrix4fv(gPathTracerUniforms.cameraInvViewMatrix, 1, GL_FALSE, glm::value_ptr(invViewMatrix));
    }
    if (gPathTracerUniforms.cameraInvProjectionMatrix >= 0) {
        glUniformMatrix4fv(gPathTracerUniforms.cameraInvProjectionMatrix, 1, GL_FALSE, glm::value_ptr(invProjMatrix));
    }
}

void setupQuad(GLuint& quadVAO, GLuint& quadVBO) {
    float quadVertices[] = {
        -1.0f,  1.0f, 0.0f,
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         1.0f,  1.0f, 0.0f
    };

    glGenVertexArrays(1, &quadVAO);
    glGenBuffers(1, &quadVBO);
    glBindVertexArray(quadVAO);

    glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), &quadVertices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glBindVertexArray(0);
}
