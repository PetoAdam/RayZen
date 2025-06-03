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
float lastFrame = 0.0f;
float deltaTime = 0.0f;

int main() {
    // Initialize GLFW
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(SCR_WIDTH, SCR_HEIGHT, "Path Tracer", nullptr, nullptr);
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
    // Load monkey
    Mesh monkey;
    if (monkey.loadFromOBJ("../meshes/monkey.obj", 0)) {
        scene.meshes.push_back(monkey);
    }

    // Add a large cube under the monkey as a mirror floor
    Mesh floorCube;
    if (floorCube.loadFromOBJ("../meshes/cube.obj", 2)) { // 2 = mirror-like material
        // Scale the cube to be large and flatten it to act as a floor
        floorCube.transform = glm::scale(glm::mat4(1.0f), glm::vec3(8.0f, 0.5f, 8.0f));
        // Move it down so it's under the monkey
        floorCube.transform = glm::translate(floorCube.transform, glm::vec3(0.0f, -3.0f, 0.0f));
        scene.meshes.push_back(floorCube);
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

        // Print fps
        std::clog << 1.0/deltaTime << std::endl;

        // Update camera aspect ratio and projection matrix each frame
        scene.camera.aspectRatio = float(SCR_WIDTH) / float(SCR_HEIGHT);
        scene.camera.updateProjectionMatrix();

        // Rotate the first cube
        //scene.meshes[0].transform = glm::rotate(scene.meshes[0].transform, deltaTime, glm::vec3(0.0f, 1.0f, 0.0f));

        // Update SSBOs with the transformed vertex data
        updateSSBOs(scene);

        // Send scene data to the shader
        sendSceneDataToShader(shaderProgram, scene);

        // Render the quad
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(shaderProgram);
        glBindVertexArray(quadVAO);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

        glfwSwapBuffers(window);
        glfwPollEvents();
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
    // Combine triangles from all meshes
    std::vector<Triangle> allTriangles = combineTriangles(scene);

    // Log BVH generation
    std::cout << "[RayZen] Generating BVH for scene with " << allTriangles.size() << " triangles..." << std::endl;

    // Build BVH
    BVH bvh;
    bvh.build(allTriangles);

    // Debug output
    //std::cout << "Number of triangles: " << allTriangles.size() << std::endl;
    //for (const auto& tri : allTriangles) {
    //    std::cout << "Triangle: (" 
    //              << tri.v0.x << ", " << tri.v0.y << ", " << tri.v0.z << "), ("
    //              << tri.v1.x << ", " << tri.v1.y << ", " << tri.v1.z << "), ("
    //              << tri.v2.x << ", " << tri.v2.y << ", " << tri.v2.z << ")"
    //              << std::endl;
    //}

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

    // Create and bind the BVH SSBO
    GLuint bvhSSBO;
    glGenBuffers(1, &bvhSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, bvhSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, bvh.nodes.size() * sizeof(BVHNode), bvh.nodes.data(), GL_STATIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, bvhSSBO);

    // Create and bind the BVH triangle index SSBO
    GLuint bvhTriIdxSSBO;
    glGenBuffers(1, &bvhTriIdxSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, bvhTriIdxSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, bvh.triIndices.size() * sizeof(int), bvh.triIndices.data(), GL_STATIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, bvhTriIdxSSBO);
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

    glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "camera.viewMatrix"), 1, GL_FALSE, glm::value_ptr(scene.camera.viewMatrix));
    glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "camera.projectionMatrix"), 1, GL_FALSE, glm::value_ptr(scene.camera.projectionMatrix));
    glUniform3fv(glGetUniformLocation(shaderProgram, "camera.position"), 1, glm::value_ptr(scene.camera.position));
    glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "camera.invViewMatrix"), 1, GL_FALSE, glm::value_ptr(invViewMatrix));
    glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "camera.invProjectionMatrix"), 1, GL_FALSE, glm::value_ptr(invProjMatrix));
    
    // Bind the SSBOs
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, triangleSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, materialSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, lightSSBO);

    // Send the number of triangles
    glUniform1i(glGetUniformLocation(shaderProgram, "numTriangles"), scene.meshes.size() * scene.meshes[0].triangles.size());
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
