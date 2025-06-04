#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "Ray.h"
#include "Scene.h"
#include "Camera.h"
#include "Material.h"
#include "Mesh.h"
#include "BVH.h"

// Constants
unsigned int SCR_WIDTH = 800;
unsigned int SCR_HEIGHT = 600;

// Function prototypes
void framebuffer_size_callback(GLFWwindow* window, int width, int height);
void processInput(GLFWwindow* window, Camera& camera, float deltaTime);
GLuint loadShaders(const char* vertexPath, const char* fragmentPath);
void sendSceneDataToShader(GLuint shaderProgram, const Scene& scene);
void setupQuad(GLuint& quadVAO, GLuint& quadVBO);
void initializeSSBOs(const Scene& scene);
void updateSSBOs(const Scene& scene);

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

int main() {
    // Print control scheme at startup
    std::cout << "==== RayZen Controls ====" << std::endl;
    std::cout << "WASD: Move camera" << std::endl;
    std::cout << "Mouse Drag (LMB): Rotate camera" << std::endl;
    std::cout << "L: Toggle light debug markers" << std::endl;
    std::cout << "B: Toggle BVH wireframe debug" << std::endl;
    std::cout << "N: Toggle BVH debug mode (TLAS/BLAS)" << std::endl;
    std::cout << "ESC: Quit" << std::endl;
    std::cout << "========================" << std::endl;

    // Initialize GLFW
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(SCR_WIDTH, SCR_HEIGHT, "RayZen", nullptr, nullptr);
    if (window == nullptr) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);

    // Initialize GLEW
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        std::cerr << "Failed to initialize GLEW" << std::endl;
        return -1;
    }

    glViewport(0, 0, SCR_WIDTH, SCR_HEIGHT);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glEnable(GL_DEPTH_TEST);

    //glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);  // Hide the cursor when it's in the window

    // Load shaders
    shaderProgram = loadShaders("../shaders/vertex_shader.glsl", "../shaders/fragment_shader.glsl");

    // Setup quad for rendering
    setupQuad(quadVAO, quadVBO);

    // Define scene
    Scene scene = Scene();

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

    // Define meshes

    // Add a large cube under the monkey as a mirror floor
    Mesh floorCube;
    if (floorCube.loadFromOBJ("../meshes/cube.obj", 2)) { // 2 = mirror-like material
        // Scale the cube to be large and flatten it to act as a floor
        floorCube.transform = glm::scale(glm::mat4(1.0f), glm::vec3(8.0f, 0.5f, 8.0f));
        // Move it down so it's under the monkey
        floorCube.transform = glm::translate(floorCube.transform, glm::vec3(0.0f, -3.0f, 0.0f));
        scene.meshes.push_back(floorCube);
    }

    // Load monkey
    Mesh monkey;
    if (monkey.loadFromOBJ("../meshes/monkey.obj", 1)) {
        scene.meshes.push_back(monkey);
    }

    // Define lights
    scene.lights.push_back(Light(glm::vec4(5.0f, 5.0f, 5.0f, 1.0f), glm::vec3(1.0f, 1.0f, 1.0f), 300.0f)); // Point light at (5, 5, 5)
    scene.lights.push_back(Light(glm::vec4(0.8f, 1.4f, 0.3f, 0.0f), glm::vec3(1.0f, 1.0f, 1.0f), 2.0f)); // Directional light with direction (0.8, 1.4, 0.3)

    // Initialize SSBOs
    initializeSSBOs(scene);

    // Main render loop
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
                // Print debug message on its own line
                std::cout << std::endl << "Light debugging: " << (debugShowLights ? "On" : "Off") << std::endl;
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
                std::cout << std::endl << "BVH wireframe debugging: " << (debugShowBVH ? "On" : "Off") << std::endl;
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
                std::cout << std::endl << "BVH debug mode: " << (debugBVHMode == 0 ? "TLAS" : "BLAS") << std::endl;
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
            for (size_t meshIdx = 0; meshIdx < scene.meshes.size(); ++meshIdx) {
                const auto& mesh = scene.meshes[meshIdx];
                glm::mat4 invTransform = glm::inverse(mesh.transform);
                glm::vec3 localOrigin = glm::vec3(invTransform * glm::vec4(rayOrigin, 1.0f));
                glm::vec3 localDir = glm::normalize(glm::vec3(invTransform * glm::vec4(rayDir, 0.0f)));
                for (size_t triIdx = 0; triIdx < mesh.triangles.size(); ++triIdx) {
                    const auto& tri = mesh.triangles[triIdx];
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
                        pickedBLAS = (int)meshIdx;
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

        // Update SSBOs with the transformed vertex data
        updateSSBOs(scene);

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
        glUseProgram(shaderProgram);
        glBindVertexArray(quadVAO);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

        glfwSwapBuffers(window);
        glfwPollEvents();

        // Print fps on a single line at the bottom of the terminal
        std::cout << "\033[2K\r" << "FPS: " << 1.0/deltaTime << std::flush;
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
        std::cerr << "Vertex Shader Compilation Error: " << infoLog << std::endl;
    }

    // Compile fragment shader
    GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment, 1, &fShaderCode, nullptr);
    glCompileShader(fragment);
    glGetShaderiv(fragment, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(fragment, 512, nullptr, infoLog);
        std::cerr << "Fragment Shader Compilation Error: " << infoLog << std::endl;
    }

    // Link shaders
    GLuint shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vertex);
    glAttachShader(shaderProgram, fragment);
    glLinkProgram(shaderProgram);
    glGetProgramiv(shaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(shaderProgram, 512, nullptr, infoLog);
        std::cerr << "Shader Program Linking Error: " << infoLog << std::endl;
    }

    glDeleteShader(vertex);
    glDeleteShader(fragment);

    return shaderProgram;
}

std::vector<Triangle> combineTriangles(const Scene& scene) {
    std::vector<Triangle> allTriangles;
    for (const auto& mesh : scene.meshes) {
        for (const auto& tri : mesh.triangles) {
            Triangle transformed;
            // Transform each vertex (assumes the mesh.transform is rotation/translation/uniform scale)
            transformed.v0 = glm::vec3(mesh.transform * glm::vec4(tri.v0, 1.0f));
            transformed.v1 = glm::vec3(mesh.transform * glm::vec4(tri.v1, 1.0f));
            transformed.v2 = glm::vec3(mesh.transform * glm::vec4(tri.v2, 1.0f));
            transformed.materialIndex = tri.materialIndex;
            allTriangles.push_back(transformed);
        }
    }
    return allTriangles;
}

void initializeSSBOs(const Scene& scene) {
    // Combine triangles from all meshes into a single global buffer
    std::vector<Triangle> allTriangles;
    std::vector<BVH> meshBLAS;
    std::vector<BVHInstance> meshInstances;
    std::vector<BVHNode> meshRootNodes;
    int nodeOffset = 0, triOffset = 0, globalTriOffset = 0;
    for (size_t i = 0; i < scene.meshes.size(); ++i) {
        const auto& mesh = scene.meshes[i];
        int meshTriStart = allTriangles.size();
        std::vector<Triangle> transformedTris;
        for (const auto& tri : mesh.triangles) {
            Triangle transformed;
            transformed.v0 = glm::vec3(mesh.transform * glm::vec4(tri.v0, 1.0f));
            transformed.v1 = glm::vec3(mesh.transform * glm::vec4(tri.v1, 1.0f));
            transformed.v2 = glm::vec3(mesh.transform * glm::vec4(tri.v2, 1.0f));
            transformed.materialIndex = tri.materialIndex;
            transformedTris.push_back(transformed);
        }
        // Build BLAS for this mesh using transformed triangles!
        BVH blas;
        blas.buildBLAS(transformedTris);
        // Do NOT offset BLAS triangle indices here!
        // for (auto& idx : blas.triIndices) {
        //     idx += meshTriStart;
        // }
        // Append transformed triangles to the global triangle buffer
        allTriangles.insert(allTriangles.end(), transformedTris.begin(), transformedTris.end());
        meshRootNodes.push_back(blas.nodes[0]);
        BVHInstance inst;
        inst.blasNodeOffset = nodeOffset;
        inst.blasTriOffset = triOffset;
        inst.meshIndex = (int)i;
        inst.globalTriOffset = meshTriStart; // NEW: store global triangle offset
        inst.transform = mesh.transform;
        meshInstances.push_back(inst);
        nodeOffset += blas.nodes.size();
        triOffset += blas.triIndices.size();
        meshBLAS.push_back(std::move(blas));
    }
    // Build TLAS
    BVH tlas;
    tlas.buildTLAS(meshInstances, meshRootNodes);
    // Flatten all BLAS nodes/indices for GPU
    std::vector<BVHNode> allBLASNodes;
    std::vector<int> allBLASTriIndices;
    for (const auto& blas : meshBLAS) {
        allBLASNodes.insert(allBLASNodes.end(), blas.nodes.begin(), blas.nodes.end());
        allBLASTriIndices.insert(allBLASTriIndices.end(), blas.triIndices.begin(), blas.triIndices.end());
    }

    // --- DEBUG LOGGING ---
    std::cout << "[DEBUG] Mesh count: " << scene.meshes.size() << std::endl;
    for (size_t i = 0; i < scene.meshes.size(); ++i) {
        std::cout << "[DEBUG] Mesh " << i << " triangle count: " << scene.meshes[i].triangles.size() << std::endl;
    }
    std::cout << "[DEBUG] All triangles count: " << allTriangles.size() << std::endl;
    std::cout << "[DEBUG] MeshInstances: " << meshInstances.size() << std::endl;
    for (size_t i = 0; i < meshInstances.size(); ++i) {
        std::cout << "[DEBUG] MeshInstance " << i << ": nodeOffset=" << meshInstances[i].blasNodeOffset << ", triOffset=" << meshInstances[i].blasTriOffset << std::endl;
    }
    std::cout << "[DEBUG] BLAS count: " << meshBLAS.size() << std::endl;
    for (size_t i = 0; i < meshBLAS.size(); ++i) {
        std::cout << "[DEBUG] BLAS " << i << " nodes: " << meshBLAS[i].nodes.size() << ", tris: " << meshBLAS[i].triIndices.size() << std::endl;
    }
    std::cout << "[DEBUG] TLAS nodes: " << tlas.nodes.size() << ", TLAS triIndices: " << tlas.triIndices.size() << std::endl;
    std::cout << "[DEBUG] allBLASNodes: " << allBLASNodes.size() << ", allBLASTriIndices: " << allBLASTriIndices.size() << std::endl;
    // --- END DEBUG LOGGING ---

    // Create and bind the triangle SSBO
    glGenBuffers(1, &triangleSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, triangleSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, allTriangles.size() * sizeof(Triangle), allTriangles.data(), GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, triangleSSBO);
    // Create and bind the material SSBO
    glGenBuffers(1, &materialSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, materialSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, scene.materials.size() * sizeof(Material), scene.materials.data(), GL_STATIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, materialSSBO);
    // Create and bind the light SSBO
    glGenBuffers(1, &lightSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, lightSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, scene.lights.size() * sizeof(Light), scene.lights.data(), GL_STATIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, lightSSBO);
    // Create and bind the TLAS node SSBO (binding = 5)
    glGenBuffers(1, &tlasNodeSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, tlasNodeSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, tlas.nodes.size() * sizeof(BVHNode), tlas.nodes.data(), GL_STATIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, tlasNodeSSBO);
    // Create and bind the TLAS tri index SSBO (binding = 6)
    glGenBuffers(1, &tlasTriIdxSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, tlasTriIdxSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, tlas.triIndices.size() * sizeof(int), tlas.triIndices.data(), GL_STATIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, tlasTriIdxSSBO);
    // Create and bind the BLAS node SSBO (binding = 7)
    glGenBuffers(1, &blasNodeSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, blasNodeSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, allBLASNodes.size() * sizeof(BVHNode), allBLASNodes.data(), GL_STATIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, blasNodeSSBO);
    // Create and bind the BLAS tri index SSBO (binding = 8)
    glGenBuffers(1, &blasTriIdxSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, blasTriIdxSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, allBLASTriIndices.size() * sizeof(int), allBLASTriIndices.data(), GL_STATIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, blasTriIdxSSBO);
    // Create and bind the BVHInstance SSBO (binding = 9)
    glGenBuffers(1, &bvhInstanceSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, bvhInstanceSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, meshInstances.size() * sizeof(BVHInstance), meshInstances.data(), GL_STATIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, bvhInstanceSSBO);
}

void updateSSBOs(const Scene& scene) {
    // Combine triangles from all meshes
    std::vector<Triangle> allTriangles = combineTriangles(scene);

    // Update the triangle SSBO with the transformed vertex data
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, triangleSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, allTriangles.size() * sizeof(Triangle), allTriangles.data());
    // TODO: If meshes move, rebuild and update the BVH and its SSBOs here for dynamic scenes.
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
    for (const auto& mesh : scene.meshes) {
        totalTriangles += mesh.triangles.size();
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
