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

namespace fs = std::filesystem;

// Constants
unsigned int SCR_WIDTH = 800;
unsigned int SCR_HEIGHT = 600;

// Function prototypes
void framebuffer_size_callback(GLFWwindow* window, int width, int height);
void processInput(GLFWwindow* window, Camera& camera, float deltaTime);
GLuint loadShaders(const char* vertexPath, const char* fragmentPath);
void sendSceneDataToShader(GLuint shaderProgram, const Scene& scene, int bounceBudget);
void setupQuad(GLuint& quadVAO, GLuint& quadVBO);
void initializeSSBOs(const Scene& scene, bool forceRebuildBVH = false);
void updateDynamicBVHAndSSBOs(Scene& scene);
void buildRasterMeshes(const Scene& scene);
void renderRasterized(const Scene& scene);
void cleanupRasterMeshes();
void sendRasterSceneData(GLuint shaderProgram, const Scene& scene);
void runPathTracerWarmup(GLFWwindow* window, Scene& scene, int warmupFrames);

// Global variables
GLuint quadVAO, quadVBO;
GLuint shaderProgram;
GLuint rasterShaderProgram;
GLuint triangleSSBO, materialSSBO, lightSSBO;
GLuint tlasNodeSSBO, tlasTriIdxSSBO, blasNodeSSBO, blasTriIdxSSBO, bvhInstanceSSBO;
float lastFrame = 0.0f;
float deltaTime = 0.0f;
bool debugShowLights = false;
bool debugShowBVH = false;
int debugBVHMode = 0; // 0 = TLAS, 1 = BLAS
int debugSelectedBLAS = 0;
int debugSelectedTri = 0;
bool editorMode = false;

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

int main(int argc, char** argv) {
    // Parse CLI log level and BVH rebuild flag
    LogLevel logLevel = LogLevel::INFO;
    bool forceRebuildBVH = false;
    bool requestPathTracerOnly = false;
    int warmupFrames = 0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--log=debug") logLevel = LogLevel::DEBUG;
        else if (arg == "--log=info") logLevel = LogLevel::INFO;
        else if (arg == "--log=error") logLevel = LogLevel::ERROR;
        else if (arg == "--rebuild-bvh") forceRebuildBVH = true;
        else if (arg == "--path-tracer-only") requestPathTracerOnly = true;
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
    glfwSwapInterval(0); // disable vsync until warm-up completes
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
    logStartupStep("GLEW init");

    glViewport(0, 0, SCR_WIDTH, SCR_HEIGHT);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.05f, 0.05f, 0.07f, 1.0f);

    //glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);  // Hide the cursor when it's in the window

    // Load shaders
    bool awaitingPathTracerReady = false;
    auto rasterCompileStart = std::chrono::high_resolution_clock::now();
    rasterShaderProgram = loadShaders("../shaders/editor_vertex.glsl", "../shaders/editor_fragment.glsl");
    auto rasterCompileEnd = std::chrono::high_resolution_clock::now();
    Logger::info(std::string("Raster shader compile/link time: ") + std::to_string(std::chrono::duration<double, std::milli>(rasterCompileEnd - rasterCompileStart).count()) + " ms");

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
        gPathTracerThread = std::thread([hidden]() {
            glfwMakeContextCurrent(hidden);
            auto start = std::chrono::high_resolution_clock::now();
            GLuint program = loadShaders("../shaders/vertex_shader.glsl", "../shaders/fragment_shader.glsl");
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
        shaderProgram = loadShaders("../shaders/vertex_shader.glsl", "../shaders/fragment_shader.glsl");
        auto pathTracerEnd = std::chrono::high_resolution_clock::now();
        gPathTracerCompileMs.store(std::chrono::duration<double, std::milli>(pathTracerEnd - pathTracerStart).count(), std::memory_order_release);
        gPathTracerReady.store(true, std::memory_order_release);
        Logger::info(std::string("Path tracer shader compile/link time: ") + std::to_string(gPathTracerCompileMs.load()) + " ms");
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
        glm::vec3(0.0f, 0.0f, 3.0f), // Position
        glm::vec3(0.0f, 0.0f, -1.0f), // Direction
        glm::vec3(0.0f, 1.0f, 0.0f), // Up vector
        70.0f, // FOV
        float(SCR_WIDTH) / float(SCR_HEIGHT), // Aspect ratio
        0.1f, // Near plane
        100.0f // Far plane
    );

    // Define materials
    scene.materials = {
        // 0: Red matte
        Material(glm::vec3(0.8f, 0.3f, 0.3f), 0.0f, 1.0f, 0.0f, 0.0f, 1.5f),
        // 1: Green metallic (moderate roughness to avoid mirror-like look)
        Material(glm::vec3(0.1f, 0.7f, 0.1f), 1.0f, 0.35f, 0.3f, 0.0f, 1.5f),
        // 2: Mirror
        Material(glm::vec3(1.0f), 1.0f, 0.05f, 1.0f, 0.0f, 1.5f),
        // 3: Glass (slight blue tint for visibility)
        Material(glm::vec3(0.85f, 0.95f, 1.0f), 0.0f, 0.02f, 0.05f, 0.94f, 1.5f),
        // 4: Rough surface
        Material(glm::vec3(0.6f, 0.4f, 0.2f), 0.0f, 0.9f, 0.2f, 0.0f, 1.5f)
    };

    // Define lights
    scene.lights.push_back(Light(glm::vec4(5.0f, 5.0f, 5.0f, 1.0f), glm::vec3(1.0f, 1.0f, 1.0f), 300.0f)); // Point light at (5, 5, 5)
    scene.lights.push_back(Light(glm::vec4(0.8f, 1.4f, 0.3f, 0.0f), glm::vec3(1.0f, 1.0f, 1.0f), 2.0f)); // Directional light with direction (0.8, 1.4, 0.3)

    // Define game objects
    // Create shared meshes
    auto floorMesh = std::make_shared<Mesh>();
    auto monkeyMesh = std::make_shared<Mesh>();
    auto monkey2Mesh = std::make_shared<Mesh>();
    auto movingCubeMesh = std::make_shared<Mesh>();
    auto movingCubeMesh2 = std::make_shared<Mesh>();
    auto movingCubeMesh3 = std::make_shared<Mesh>();
    auto glassMesh = std::make_shared<Mesh>();
    loadMeshWithTiming(floorMesh, "../meshes/cube.obj", 0, "floor");
    loadMeshWithTiming(monkeyMesh, "../meshes/monkey.obj", 1, "monkey A");
    loadMeshWithTiming(monkey2Mesh, "../meshes/monkey.obj", 2, "monkey B");
    loadMeshWithTiming(movingCubeMesh, "../meshes/car.obj", 0, "car");
    loadMeshWithTiming(movingCubeMesh2, "../meshes/monkey.obj", 0, "monkey C");
    loadMeshWithTiming(movingCubeMesh3, "../meshes/monkey.obj", 0, "monkey D");
    loadMeshWithTiming(glassMesh, "../meshes/monkey.obj", 3, "glass monkey");
    logStartupStep("Mesh loading");

    // Add GameObjects to the scene
    scene.gameObjects.push_back(GameObject{floorMesh, glm::translate(glm::scale(glm::mat4(1.0f), glm::vec3(8.0f, 0.5f, 8.0f)), glm::vec3(0.0f, -3.0f, 0.0f))});
    scene.gameObjects.push_back(GameObject{monkeyMesh, glm::translate(glm::mat4(1.0f), glm::vec3(-4.0f, 0.0f, 0.0f))});
    scene.gameObjects.push_back(GameObject{monkey2Mesh, glm::translate(glm::mat4(1.0f), glm::vec3(4.0f, 0.0f, 0.0f))});
    scene.gameObjects.push_back(GameObject{movingCubeMesh, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.0f))});
    scene.gameObjects.push_back(GameObject{movingCubeMesh2, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -4.f))});
    scene.gameObjects.push_back(GameObject{movingCubeMesh3, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 4.f))});
    scene.gameObjects.push_back(GameObject{glassMesh, glm::translate(glm::scale(glm::mat4(1.0f), glm::vec3(1.2f)), glm::vec3(2.5f, 0.8f, 2.5f))});
    logStartupStep("Scene graph build");

    // Initialize SSBOs (initial build)
    initializeSSBOs(scene, forceRebuildBVH);
    logStartupStep("SSBO/BVH init");
    buildRasterMeshes(scene);
    logStartupStep("Raster mesh build");
    Logger::info("Glass monkey material index 3 at gameObject index " + std::to_string(scene.gameObjects.size()-1));
    logStartupStep("Startup ready");

    if (warmupFrames > 0 && shaderProgram != 0) {
        runPathTracerWarmup(window, scene, warmupFrames);
        lastFrame = glfwGetTime();
    }

    // Main render loop
    bool firstFrame = true;
    double firstUseProgramMs = 0.0, firstDrawMs = 0.0;
    float animTime = 0.0f;
    int frameCounter = 0;
    const int frameLogLimit = 100;
    const int vsyncRestoreFrame = 5;
    bool vsyncRestored = false;
    while (!glfwWindowShouldClose(window)) {
        auto frameStart = std::chrono::high_resolution_clock::now();

        if (awaitingPathTracerReady && gPathTracerReady.load(std::memory_order_acquire)) {
            GLuint program = gPathTracerProgramHandle.exchange(0, std::memory_order_acq_rel);
            if (program != 0) {
                shaderProgram = program;
                awaitingPathTracerReady = false;
                if (!userLockedEditorMode) {
                    editorMode = false;
                }
                firstFrame = true;
                frameCounter = 0;
                vsyncRestored = false;
                glfwSwapInterval(0);
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
                        vsyncRestored = false;
                        glfwSwapInterval(0);
                    }
                }
                editorKeyPressed = true;
            }
        } else {
            editorKeyPressed = false;
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
            if (!vsyncRestored && frameCounter >= vsyncRestoreFrame) {
                glfwSwapInterval(1);
                vsyncRestored = true;
                Logger::info("Re-enabled vsync after warmup frames");
            }
            ++frameCounter;
            continue;
        }

        // Send scene data to the shader
    int bounceBudget = (frameCounter == 0) ? 1 : 5;
    sendSceneDataToShader(shaderProgram, scene, bounceBudget);
        auto afterSend = std::chrono::high_resolution_clock::now();

        // Set debugShowLights uniform
        GLint debugLoc = glGetUniformLocation(shaderProgram, "debugShowLights");
        glUniform1i(debugLoc, debugShowLights ? 1 : 0);
        // Set debugShowBVH uniform
        GLint bvhLoc = glGetUniformLocation(shaderProgram, "debugShowBVH");
        glUniform1i(bvhLoc, debugShowBVH ? 1 : 0);
        // Set debugBVHMode uniform
        GLint modeLoc = glGetUniformLocation(shaderProgram, "debugBVHMode");
        glUniform1i(modeLoc, debugBVHMode);
        // Set debugSelectedBLAS and debugSelectedTri uniforms
        GLint selBLASLoc = glGetUniformLocation(shaderProgram, "debugSelectedBLAS");
        glUniform1i(selBLASLoc, debugSelectedBLAS);
        GLint selTriLoc = glGetUniformLocation(shaderProgram, "debugSelectedTri");
        glUniform1i(selTriLoc, debugSelectedTri);

        // Render the quad
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        auto useProgStart = std::chrono::high_resolution_clock::now();
        glUseProgram(shaderProgram);
        // Set FPS uniform for overlay
        static float smoothedFps = 0.0f;
        float fps = (deltaTime > 0.0f) ? (1.0f / deltaTime) : 0.0f;
        float alpha = 0.1f; // Smoothing factor (0.0 = no smoothing, 1.0 = instant)
        if (smoothedFps == 0.0f) smoothedFps = fps;
        else smoothedFps = alpha * fps + (1.0f - alpha) * smoothedFps;
        GLint fpsLoc = glGetUniformLocation(shaderProgram, "uniformFps");
        glUniform1f(fpsLoc, smoothedFps);
        auto useProgEnd = std::chrono::high_resolution_clock::now();
        if (firstFrame) {
            firstUseProgramMs = std::chrono::duration<double, std::milli>(useProgEnd - useProgStart).count();
        }
        glBindVertexArray(quadVAO);
        auto drawStart = std::chrono::high_resolution_clock::now();
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
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

        if (!vsyncRestored && frameCounter >= vsyncRestoreFrame) {
            glfwSwapInterval(1);
            vsyncRestored = true;
            Logger::info("Re-enabled vsync after warmup frames");
        }

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
            std::cout << "\rFPS: " << (1.0/deltaTime) << "    "  << std::endl;
        }
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
    }

    // Compile fragment shader
    GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment, 1, &fShaderCode, nullptr);
    glCompileShader(fragment);
    glGetShaderiv(fragment, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(fragment, 512, nullptr, infoLog);
        Logger::error(std::string("Fragment Shader Compilation Error: ") + infoLog);
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
    if (!forceRebuildBVH &&
        fs::exists(ssboCachePrefix + "triangles.bin") &&
        fs::exists(ssboCachePrefix + "blasnodes.bin") &&
        fs::exists(ssboCachePrefix + "blastris.bin") &&
        fs::exists(ssboCachePrefix + "instances.bin") &&
        fs::exists(ssboCachePrefix + "tlasnodes.bin") &&
        fs::exists(ssboCachePrefix + "tlastris.bin")) {
        loadedSSBOCache =
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
            bool loaded = false;
            if (!forceRebuildBVH && fs::exists(blasBase + ".nodes.bin") && fs::exists(blasBase + ".tris.bin")) {
                loaded = loadBVHFromFile(blasBase, meshBLAS[i]);
                if (loaded) {
                    Logger::info("Loaded BLAS from cache for " + meshName);
                }
            }
            if (!loaded) {
                Logger::info("Building BLAS from scratch for " + meshName);
                meshBLAS[i].buildBLAS(meshTris);
                saveBVHToFile(blasBase, meshBLAS[i]);
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
        std::string tlasBase = cacheDir + "scene_tlas";
        bool loadedTLAS = false;
        if (!forceRebuildBVH && loadedAllBLAS && fs::exists(tlasBase + ".nodes.bin") && fs::exists(tlasBase + ".tris.bin") && fs::exists(cacheDir + "instances.bin")) {
            loadedTLAS = loadBVHFromFile(tlasBase, tlas) && loadBVHInstancesFromFile(cacheDir + "instances.bin", meshInstances);
            if (loadedTLAS) {
                Logger::info("Loaded TLAS and BVHInstances from cache");
            }
        }
        if (!loadedTLAS) {
            Logger::info("Building TLAS from scratch");
            tlas.nodes.clear();
            tlas.triIndices.clear();
            tlas.buildTLAS(meshInstances, worldRootNodes);
            saveBVHToFile(tlasBase, tlas);
            saveBVHInstancesToFile(cacheDir + "instances.bin", meshInstances);
            Logger::info("Saved TLAS and BVHInstances to cache");
        }

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
    // 1. Build BLAS for each unique mesh only once (unless geometry changes)
    static std::vector<BVH> meshBLAS;
    static std::vector<BVHNode> meshRootNodes;
    static bool first = true;
    if (first || meshBLAS.size() != scene.gameObjects.size()) {
        meshBLAS.resize(scene.gameObjects.size());
        meshRootNodes.resize(scene.gameObjects.size());
        for (size_t i = 0; i < scene.gameObjects.size(); ++i) {
            meshBLAS[i].buildBLAS(scene.gameObjects[i].mesh->triangles);
            meshRootNodes[i] = meshBLAS[i].nodes[0];
        }
        first = false;
    }
    // 2. Build BVHInstances for all objects (with current transform)
    std::vector<BVHInstance> meshInstances;
    for (size_t i = 0; i < scene.gameObjects.size(); ++i) {
        BVHInstance inst;
        // Compute node/tri/globalTri offsets for this mesh in the concatenated BLAS buffers
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
    // 3. Flatten all BLAS nodes/indices for GPU
    std::vector<BVHNode> allBLASNodes;
    std::vector<int> allBLASTriIndices;
    for (size_t i = 0; i < meshBLAS.size(); ++i) {
        allBLASNodes.insert(allBLASNodes.end(), meshBLAS[i].nodes.begin(), meshBLAS[i].nodes.end());
        allBLASTriIndices.insert(allBLASTriIndices.end(), meshBLAS[i].triIndices.begin(), meshBLAS[i].triIndices.end());
    }
    // 4. Combine all triangles (object space, not transformed)
    std::vector<Triangle> allTriangles;
    for (const auto& obj : scene.gameObjects) {
        allTriangles.insert(allTriangles.end(), obj.mesh->triangles.begin(), obj.mesh->triangles.end());
    }
    // 5. Rebuild TLAS every frame (since transforms may change)
    std::vector<BVHNode> instanceRootNodes;
    for (size_t i = 0; i < scene.gameObjects.size(); ++i) {
        // Transform mesh AABB by instance transform
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
    //Logger::info("[updateDynamicBVHAndSSBOs] meshInstances size: " + std::to_string(meshInstances.size()) + ", instanceRootNodes size: " + std::to_string(instanceRootNodes.size()));
    tlas.buildTLAS(meshInstances, instanceRootNodes);
    // 6. Update SSBOs
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, triangleSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, allTriangles.size() * sizeof(Triangle), allTriangles.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, blasNodeSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, allBLASNodes.size() * sizeof(BVHNode), allBLASNodes.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, blasTriIdxSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, allBLASTriIndices.size() * sizeof(int), allBLASTriIndices.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, bvhInstanceSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, meshInstances.size() * sizeof(BVHInstance), meshInstances.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, tlasNodeSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, tlas.nodes.size() * sizeof(BVHNode), tlas.nodes.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, tlasTriIdxSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, tlas.triIndices.size() * sizeof(int), tlas.triIndices.data());
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
    glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "uView"), 1, GL_FALSE, glm::value_ptr(scene.camera.viewMatrix));
    glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "uProj"), 1, GL_FALSE, glm::value_ptr(scene.camera.projectionMatrix));
    glUniform3fv(glGetUniformLocation(shaderProgram, "uCameraPos"), 1, glm::value_ptr(scene.camera.position));
    glUniform1i(glGetUniformLocation(shaderProgram, "numLights"), static_cast<int>(scene.lights.size()));
    GLint ambientLoc = glGetUniformLocation(shaderProgram, "uAmbientColor");
    if (ambientLoc >= 0) {
        glUniform3f(ambientLoc, 0.03f, 0.03f, 0.03f);
    }
}

void renderRasterized(const Scene& scene) {
    glEnable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(rasterShaderProgram);
    sendRasterSceneData(rasterShaderProgram, scene);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, materialSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, lightSSBO);

    GLint modelLoc = glGetUniformLocation(rasterShaderProgram, "uModel");
    GLint normalLoc = glGetUniformLocation(rasterShaderProgram, "uNormalMatrix");

    for (const auto& obj : scene.gameObjects) {
        const Mesh* meshPtr = obj.mesh.get();
        auto it = gRasterMeshCache.find(meshPtr);
        if (it == gRasterMeshCache.end()) continue;

        const RasterMeshGPU& gpu = it->second;
        if (gpu.vertexCount == 0) continue;

        glm::mat4 model = obj.transform;
        glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(model)));

        if (modelLoc >= 0) {
            glUniformMatrix4fv(modelLoc, 1, GL_FALSE, glm::value_ptr(model));
        }
        if (normalLoc >= 0) {
            glUniformMatrix3fv(normalLoc, 1, GL_FALSE, glm::value_ptr(normalMatrix));
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
        int bounceBudget = (i == 0) ? 1 : 5;
        sendSceneDataToShader(shaderProgram, scene, bounceBudget);
        GLint debugLoc = glGetUniformLocation(shaderProgram, "debugShowLights");
        glUniform1i(debugLoc, debugShowLights ? 1 : 0);
        GLint bvhLoc = glGetUniformLocation(shaderProgram, "debugShowBVH");
        glUniform1i(bvhLoc, debugShowBVH ? 1 : 0);
        GLint modeLoc = glGetUniformLocation(shaderProgram, "debugBVHMode");
        glUniform1i(modeLoc, debugBVHMode);
        GLint selBLASLoc = glGetUniformLocation(shaderProgram, "debugSelectedBLAS");
        glUniform1i(selBLASLoc, debugSelectedBLAS);
        GLint selTriLoc = glGetUniformLocation(shaderProgram, "debugSelectedTri");
        glUniform1i(selTriLoc, debugSelectedTri);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBindVertexArray(quadVAO);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glFinish();
        glfwSwapBuffers(window);
        glfwPollEvents();
    }
    glBindVertexArray(0);
    glfwShowWindow(window);
    Logger::info("Warm-up complete; first on-screen frame should reuse the cached pipeline");
}

void sendSceneDataToShader(GLuint shaderProgram, const Scene& scene, int bounceBudget) {
    glUseProgram(shaderProgram);
    
    // Send resolution uniform
    glUniform2f(glGetUniformLocation(shaderProgram, "resolution"), float(SCR_WIDTH), float(SCR_HEIGHT));
    
    // Send camera data
    glm::mat4 invViewMatrix = glm::inverse(scene.camera.viewMatrix);
    glm::mat4 invProjMatrix = glm::inverse(scene.camera.projectionMatrix);

    // Send the number of triangles
    int totalTriangles = 0;
    for (const auto& obj : scene.gameObjects) {
        totalTriangles += obj.mesh->triangles.size();
    }
    glUniform1i(glGetUniformLocation(shaderProgram, "numTriangles"), totalTriangles);
    glUniform1i(glGetUniformLocation(shaderProgram, "numLights"), (int)scene.lights.size());
    glUniform1i(glGetUniformLocation(shaderProgram, "uniformBounceBudget"), bounceBudget);

    glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "camera.viewMatrix"), 1, GL_FALSE, glm::value_ptr(scene.camera.viewMatrix));
    glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "camera.projectionMatrix"), 1, GL_FALSE, glm::value_ptr(scene.camera.projectionMatrix));
    glUniform3fv(glGetUniformLocation(shaderProgram, "camera.position"), 1, glm::value_ptr(scene.camera.position));
    glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "camera.invViewMatrix"), 1, GL_FALSE, glm::value_ptr(invViewMatrix));
    glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "camera.invProjectionMatrix"), 1, GL_FALSE, glm::value_ptr(invProjMatrix));
    
    // Bind the SSBOs
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, triangleSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, materialSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, lightSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, tlasNodeSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, tlasTriIdxSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, blasNodeSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, blasTriIdxSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, bvhInstanceSSBO);

    
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
