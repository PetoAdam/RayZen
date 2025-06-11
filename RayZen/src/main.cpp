#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <filesystem>
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
void sendSceneDataToShader(GLuint shaderProgram, const Scene& scene);
void setupQuad(GLuint& quadVAO, GLuint& quadVBO);
void initializeSSBOs(const Scene& scene, bool forceRebuildBVH = false);
void updateDynamicBVHAndSSBOs(Scene& scene);

// Global variables
GLuint quadVAO, quadVBO;
GLuint shaderProgram;
GLuint triangleSSBO, materialSSBO, lightSSBO;
GLuint tlasNodeSSBO, tlasTriIdxSSBO, blasNodeSSBO, blasTriIdxSSBO, bvhInstanceSSBO;
float lastFrame = 0.0f;
float deltaTime = 0.0f;
bool debugShowLights = false;
bool debugShowBVH = false;
int debugBVHMode = 0; // 0 = TLAS, 1 = BLAS
int debugSelectedBLAS = 0;
int debugSelectedTri = 0;

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
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--log=debug") logLevel = LogLevel::DEBUG;
        else if (arg == "--log=info") logLevel = LogLevel::INFO;
        else if (arg == "--log=error") logLevel = LogLevel::ERROR;
        else if (arg == "--rebuild-bvh") forceRebuildBVH = true;
    }
    Logger::setLevel(logLevel);

    // Print control scheme at startup
    Logger::info("==== RayZen Controls ====");
    Logger::info("WASD: Move camera");
    Logger::info("Mouse Drag (LMB): Rotate camera");
    Logger::info("L: Toggle light debug markers");
    Logger::info("B: Toggle BVH wireframe debug");
    Logger::info("N: Toggle BVH debug mode (TLAS/BLAS)");
    Logger::info("ESC: Quit");
    Logger::info("========================");

    // Initialize GLFW
    if (!glfwInit()) {
        Logger::error("Failed to initialize GLFW");
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(SCR_WIDTH, SCR_HEIGHT, "RayZen", nullptr, nullptr);
    if (window == nullptr) {
        Logger::error("Failed to create GLFW window");
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);

    // Initialize GLEW
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        Logger::error("Failed to initialize GLEW");
        return -1;
    }

    glViewport(0, 0, SCR_WIDTH, SCR_HEIGHT);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glEnable(GL_DEPTH_TEST);

    //glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);  // Hide the cursor when it's in the window

    // Load shaders
    auto shaderStart = std::chrono::high_resolution_clock::now();
    shaderProgram = loadShaders("../shaders/vertex_shader.glsl", "../shaders/fragment_shader.glsl");
    auto shaderEnd = std::chrono::high_resolution_clock::now();
    Logger::info(std::string("Shader compile/link time: ") + std::to_string(std::chrono::duration<double, std::milli>(shaderEnd - shaderStart).count()) + " ms");

    // Setup quad for rendering
    setupQuad(quadVAO, quadVBO);

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
        // Red matte material (non-metallic, rough)
        Material(glm::vec3(0.8f, 0.3f, 0.3f), 0.0f, 1.0f, 0.0f, 0.0f, 1.5f),
        // Green metallic material (metallic, smooth)
        Material(glm::vec3(0.1f, 0.7f, 0.1f), 1.0f, 0.2f, 0.5f, 0.0f, 1.5f),
        // Mirror-like material (fully reflective, smooth)
        Material(glm::vec3(1.0f, 1.0f, 1.0f), 1.0f, 0.1f, 1.0f, 0.0f, 1.5f),
        // Glass-like material (transparent, smooth)
        Material(glm::vec3(1.0f, 1.0f, 1.0f), 1.0f, 0.0f, 0.0f, 0.99f, 1.5f),
        // Rough surface material (non-metallic, rough)
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
    floorMesh->loadFromOBJ("../meshes/cube.obj", 0);
    monkeyMesh->loadFromOBJ("../meshes/monkey.obj", 1);
    monkey2Mesh->loadFromOBJ("../meshes/monkey.obj", 2);
    movingCubeMesh->loadFromOBJ("../meshes/cube.obj", 1);

    // Add GameObjects to the scene
    scene.gameObjects.push_back(GameObject{floorMesh, glm::translate(glm::scale(glm::mat4(1.0f), glm::vec3(8.0f, 0.5f, 8.0f)), glm::vec3(0.0f, -3.0f, 0.0f))});
    scene.gameObjects.push_back(GameObject{monkeyMesh, glm::translate(glm::mat4(1.0f), glm::vec3(-2.0f, 0.0f, 0.0f))});
    scene.gameObjects.push_back(GameObject{monkey2Mesh, glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 0.0f))});
    scene.gameObjects.push_back(GameObject{movingCubeMesh, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f))});

    // Initialize SSBOs (initial build)
    initializeSSBOs(scene, forceRebuildBVH);

    // Main render loop
    bool firstFrame = true;
    double firstUseProgramMs = 0.0, firstDrawMs = 0.0;
    float animTime = 0.0f;
    while (!glfwWindowShouldClose(window)) {

        // Calculate delta time
        float currentFrame = glfwGetTime();
        deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;
        processInput(window, scene.camera, deltaTime);
        scene.camera.updateViewMatrix();

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
        if (!scene.gameObjects.empty()) {
            // Animate the last object (moving cube)
            GameObject& movingCube = scene.gameObjects.back();
            float x = -2.0f + 2.0f * sin(animTime);
            float y = 0.5f + 0.5f * cos(animTime * 0.5f);
            movingCube.transform = glm::translate(glm::mat4(1.0f), glm::vec3(x, y, 0.0f));
        }

        // Update dynamic BVH/SSBOs for game objects
        updateDynamicBVHAndSSBOs(scene);

        // Send scene data to the shader
        sendSceneDataToShader(shaderProgram, scene);

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

        // Print fps on a single line at the bottom of the terminal only in DEBUG mode
        if (Logger::getLevel() <= LogLevel::DEBUG) {
            std::cout << "\rFPS: " << (1.0/deltaTime) << "    "  << std::endl;
        }
    }

    // Cleanup
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
    // Read vertex and fragment shader files
    std::ifstream vShaderFile(vertexPath);
    std::ifstream fShaderFile(fragmentPath);
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
    std::string cacheDir = "bvh_cache/";
    if (!fs::exists(cacheDir)) {
        fs::create_directory(cacheDir);
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
    std::string ssboCachePrefix = cacheDir + "ssbo_";
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
            Logger::info("Loaded SSBO data from cache");
        }
    }

    if (!loadedSSBOCache) {
        std::vector<BVH> meshBLAS(scene.gameObjects.size());
        std::vector<BVHNode> meshRootNodes;
        int nodeOffset = 0, triOffset = 0;
        bool loadedAllBLAS = true;
        meshInstances.clear();
        allTriangles.clear();
        // Try to load BLAS for each mesh
        for (size_t i = 0; i < scene.gameObjects.size(); ++i) {
            const auto& obj = scene.gameObjects[i];
            const auto& mesh = obj.mesh;
            std::string meshName = "mesh" + std::to_string(i);
            std::string blasBase = cacheDir + meshName;
            std::vector<Triangle> transformedTris;
            for (const auto& tri : mesh->triangles) {
                Triangle transformed;
                transformed.v0 = glm::vec3(obj.transform * glm::vec4(tri.v0, 1.0f));
                transformed.v1 = glm::vec3(obj.transform * glm::vec4(tri.v1, 1.0f));
                transformed.v2 = glm::vec3(obj.transform * glm::vec4(tri.v2, 1.0f));
                transformed.materialIndex = tri.materialIndex;
                transformedTris.push_back(transformed);
            }
            bool loaded = false;
            if (!forceRebuildBVH && fs::exists(blasBase + ".nodes.bin") && fs::exists(blasBase + ".tris.bin")) {
                loaded = loadBVHFromFile(blasBase, meshBLAS[i]);
                if (loaded) {
                    Logger::info("Loaded BLAS from cache for " + meshName);
                }
            }
            if (!loaded) {
                Logger::info("Building BLAS from scratch for " + meshName);
                meshBLAS[i].buildBLAS(transformedTris);
                saveBVHToFile(blasBase, meshBLAS[i]);
                Logger::info("Saved BLAS to cache for " + meshName);
            }
            allTriangles.insert(allTriangles.end(), transformedTris.begin(), transformedTris.end());
            meshRootNodes.push_back(meshBLAS[i].nodes[0]);
            BVHInstance inst;
            inst.blasNodeOffset = nodeOffset;
            inst.blasTriOffset = triOffset;
            inst.globalTriOffset = allTriangles.size() - transformedTris.size();
            inst.transform = obj.transform;
            meshInstances.push_back(inst);
            nodeOffset += meshBLAS[i].nodes.size();
            triOffset += meshBLAS[i].triIndices.size();
            loadedAllBLAS &= loaded;
        }
        // Try to load TLAS and BVHInstances
        BVH tlas;
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
            tlas.buildTLAS(meshInstances, meshRootNodes);
            saveBVHToFile(tlasBase, tlas);
            saveBVHInstancesToFile(cacheDir + "instances.bin", meshInstances);
            Logger::info("Saved TLAS and BVHInstances to cache");
        }
        // Flatten all BLAS nodes/indices for GPU
        allBLASNodes.clear();
        allBLASTriIndices.clear();
        for (const auto& blas : meshBLAS) {
            allBLASNodes.insert(allBLASNodes.end(), blas.nodes.begin(), blas.nodes.end());
            allBLASTriIndices.insert(allBLASTriIndices.end(), blas.triIndices.begin(), blas.triIndices.end());
        }
        tlasNodes = tlas.nodes;
        tlasTriIndices = tlas.triIndices;
        // Save SSBO-ready data to cache
        saveVectorToFile(ssboCachePrefix + "triangles.bin", allTriangles);
        saveVectorToFile(ssboCachePrefix + "blasnodes.bin", allBLASNodes);
        saveVectorToFile(ssboCachePrefix + "blastris.bin", allBLASTriIndices);
        saveVectorToFile(ssboCachePrefix + "instances.bin", meshInstances);
        saveVectorToFile(ssboCachePrefix + "tlasnodes.bin", tlasNodes);
        saveVectorToFile(ssboCachePrefix + "tlastris.bin", tlasTriIndices);
        Logger::info("Saved SSBO data to cache");
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
        glBufferData(GL_SHADER_STORAGE_BUFFER, tlasNodes.size() * sizeof(BVHNode), tlasNodes.data(), GL_STATIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, tlasNodeSSBO);
    });
    logBufferUpload("TLAS Tri Indices", tlasTriIndices.size() * sizeof(int), [&]() {
        glGenBuffers(1, &tlasTriIdxSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, tlasTriIdxSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER, tlasTriIndices.size() * sizeof(int), tlasTriIndices.data(), GL_STATIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, tlasTriIdxSSBO);
    });
    logBufferUpload("BLAS Nodes", allBLASNodes.size() * sizeof(BVHNode), [&]() {
        glGenBuffers(1, &blasNodeSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, blasNodeSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER, allBLASNodes.size() * sizeof(BVHNode), allBLASNodes.data(), GL_STATIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, blasNodeSSBO);
    });
    logBufferUpload("BLAS Tri Indices", allBLASTriIndices.size() * sizeof(int), [&]() {
        glGenBuffers(1, &blasTriIdxSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, blasTriIdxSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER, allBLASTriIndices.size() * sizeof(int), allBLASTriIndices.data(), GL_STATIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, blasTriIdxSSBO);
    });
    logBufferUpload("BVH Instances", meshInstances.size() * sizeof(BVHInstance), [&]() {
        glGenBuffers(1, &bvhInstanceSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, bvhInstanceSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER, meshInstances.size() * sizeof(BVHInstance), meshInstances.data(), GL_STATIC_DRAW);
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
            inst.blasNodeOffset += meshBLAS[j].nodes.size();
            inst.blasTriOffset += meshBLAS[j].triIndices.size();
            inst.globalTriOffset += scene.gameObjects[j].mesh->triangles.size();
        }
        inst.transform = scene.gameObjects[i].transform;
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

void sendSceneDataToShader(GLuint shaderProgram, const Scene& scene) {
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
