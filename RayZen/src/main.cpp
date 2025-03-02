#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "Ray.h"
#include "Sphere.h"
#include "Scene.h"
#include "Camera.h"
#include "Material.h"

// Constants
unsigned int SCR_WIDTH = 800;
unsigned int SCR_HEIGHT = 600;

// Function prototypes
void framebuffer_size_callback(GLFWwindow* window, int width, int height);
void processInput(GLFWwindow* window, Camera& camera, float deltaTime);
GLuint loadShaders(const char* vertexPath, const char* fragmentPath);
void sendSceneDataToShader(GLuint shaderProgram, const Camera& camera, const std::vector<Sphere>& spheres, const std::vector<Material>& materials, const std::vector<Light>& lights);
void setupQuad(GLuint& quadVAO, GLuint& quadVBO);

// Global variables
GLuint quadVAO, quadVBO;
GLuint shaderProgram;
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

    // Define camera
    Camera camera(
        glm::vec3(0.0f, 0.0f, 3.0f), // Position
        glm::vec3(0.0f, 0.0f, -1.0f), // Direction
        glm::vec3(0.0f, 1.0f, 0.0f), // Up vector
        70.0f, // FOV
        float(SCR_WIDTH) / float(SCR_HEIGHT), // Aspect ratio
        0.1f, // Near plane
        100.0f // Far plane
    );

    // Define materials
    std::vector<Material> materials = {
        // Red matte material (non-metallic, rough)
        Material(glm::vec3(0.8f, 0.3f, 0.3f), 0.0f, 1.0f, 0.0f, 0.0f, 1.5f),
        // Green metallic material (metallic, smooth)
        Material(glm::vec3(0.1f, 0.7f, 0.1f), 1.0f, 0.2f, 0.5f, 0.0f, 1.5f),
        // Mirror-like material (fully reflective, smooth)
        Material(glm::vec3(1.0f, 1.0f, 1.0f), 1.0f, 0.1f, 1.0f, 0.0f, 1.5f),
        // Glass-like material (transparent, smooth)
        Material(glm::vec3(1.0f, 1.0f, 1.0f), 1.0f, 0.0f, 0.0f, 0.9f, 1.5f),
        // Rough surface material (non-metallic, rough)
        Material(glm::vec3(0.6f, 0.4f, 0.2f), 0.0f, 0.9f, 0.2f, 0.0f, 1.5f)
    };

    // Define spheres
    std::vector<Sphere> spheres = {
        // First sphere (position, radius, material index)
        Sphere(glm::vec3(0.0f, 1.0f, -1.0f), 0.5f, 0), // Red matte
        Sphere(glm::vec3(1.0f, 0.0f, -1.5f), 0.5f, 1), // Green shiny
        Sphere(glm::vec3(-1.5f, -0.5f, -2.0f), 0.7f, 2), // Mirror-like
        Sphere(glm::vec3(2.0f, -1.0f, -3.0f), 0.6f, 3), // Transparent (glass)
        Sphere(glm::vec3(0.5f, -1.5f, -2.5f), 0.5f, 4),  // Rough surface
        Sphere(glm::vec3(0.0f, 0.0f, -15.0f), 10.0f, 0),  // Rough surface
    };

    // Define lights
    std::vector<Light> lights;
    lights.push_back(Light(glm::vec4(5.0f, 5.0f, 5.0f, 1.0f), glm::vec3(1.0f, 1.0f, 1.0f), 300.0f)); // Point light at (5, 5, 5)
    lights.push_back(Light(glm::vec4(1.0f, -1.0f, 0.0f, 0.0f), glm::vec3(1.0f, 1.0f, 1.0f), 2.0f)); // Directional light with direction (1, -1, 0)


    // Main render loop
    while (!glfwWindowShouldClose(window)) {

        // Calculate delta time
        float currentFrame = glfwGetTime();
        deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;
        processInput(window, camera, deltaTime);

        // Print fps
        //std::clog << 1.0/deltaTime << std::endl;

        // Update camera aspect ratio and projection matrix each frame
        camera.aspectRatio = float(SCR_WIDTH) / float(SCR_HEIGHT);
        camera.updateProjectionMatrix();

        // Send scene data to the shader
        sendSceneDataToShader(shaderProgram, camera, spheres, materials, lights);

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

void sendSceneDataToShader(GLuint shaderProgram, const Camera& camera, const std::vector<Sphere>& spheres, const std::vector<Material>& materials, const std::vector<Light>& lights) {
    glUseProgram(shaderProgram);

    // Send resolution uniform
    glUniform2f(glGetUniformLocation(shaderProgram, "resolution"), float(SCR_WIDTH), float(SCR_HEIGHT));

    // Send camera data
    glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "camera.viewMatrix"), 1, GL_FALSE, glm::value_ptr(camera.viewMatrix));
    glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "camera.projectionMatrix"), 1, GL_FALSE, glm::value_ptr(camera.projectionMatrix));
    glUniform3fv(glGetUniformLocation(shaderProgram, "camera.position"), 1, glm::value_ptr(camera.position));

    // Send spheres data
    for (size_t i = 0; i < spheres.size(); ++i) {
        std::string index = std::to_string(i);
        glUniform3fv(glGetUniformLocation(shaderProgram, ("spheres[" + index + "].center").c_str()), 1, glm::value_ptr(spheres[i].center));
        glUniform1f(glGetUniformLocation(shaderProgram, ("spheres[" + index + "].radius").c_str()), spheres[i].radius);
        glUniform1i(glGetUniformLocation(shaderProgram, ("spheres[" + index + "].materialIndex").c_str()), spheres[i].materialIndex);
    }

    // Send materials data
    for (size_t i = 0; i < materials.size(); ++i) {
        std::string index = std::to_string(i);
        glUniform3fv(glGetUniformLocation(shaderProgram, ("materials[" + index + "].albedo").c_str()), 1, glm::value_ptr(materials[i].albedo));
        glUniform1f(glGetUniformLocation(shaderProgram, ("materials[" + index + "].metallic").c_str()), materials[i].metallic);
        glUniform1f(glGetUniformLocation(shaderProgram, ("materials[" + index + "].roughness").c_str()), materials[i].roughness);
        glUniform1f(glGetUniformLocation(shaderProgram, ("materials[" + index + "].reflectivity").c_str()), materials[i].reflectivity);
        glUniform1f(glGetUniformLocation(shaderProgram, ("materials[" + index + "].transparency").c_str()), materials[i].transparency);
        glUniform1f(glGetUniformLocation(shaderProgram, ("materials[" + index + "].ior").c_str()), materials[i].ior);
    }

    // Send light data
    for (size_t i = 0; i < lights.size(); ++i) {
        std::string index = std::to_string(i);
        glUniform4fv(glGetUniformLocation(shaderProgram, ("lights[" + index + "].positionOrDirection").c_str()), 1, glm::value_ptr(lights[i].positionOrDirection));
        glUniform3fv(glGetUniformLocation(shaderProgram, ("lights[" + index + "].color").c_str()), 1, glm::value_ptr(lights[i].getColor()));
        glUniform1f(glGetUniformLocation(shaderProgram, ("lights[" + index + "].power").c_str()), lights[i].power);
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
