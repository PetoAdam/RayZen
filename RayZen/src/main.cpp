#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <float.h>
#include <cmath>

#include "Ray.h"
#include "Sphere.h"
#include "Scene.h"
#include "Camera.h"
#include "Material.h"
#include "Light.h"

const int WIDTH = 800;
const int HEIGHT = 600;
const int NUM_SAMPLES = 1;

GLuint quadVAO, quadVBO;

void initQuad() {
    GLfloat vertices[] = {
        // positions        // texCoords
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f,
        -1.0f,  1.0f,  0.0f, 1.0f
    };
    
    glGenVertexArrays(1, &quadVAO);
    glGenBuffers(1, &quadVBO);

    glBindVertexArray(quadVAO);

    glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (GLvoid*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (GLvoid*)(2 * sizeof(GLfloat)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
}

void renderQuad() {
    glBindVertexArray(quadVAO);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glBindVertexArray(0);
}

glm::vec3 rayColor(const Ray& ray, const Scene& scene, int depth);

void renderScene(const Camera& camera, Scene& scene, int width, int height, GLuint texture, int num_samples, GLuint shaderProgram) {
    std::vector<glm::vec3> framebuffer(width * height);

    // Perform ray tracing
    for (int j = 0; j < height; ++j) {
        for (int i = 0; i < width; ++i) {
            glm::vec3 color_sum(0.0f);

            for (int s = 0; s < num_samples; ++s) {
                float u = (i + drand48()) / float(width);
                float v = (j + drand48()) / float(height);
                Ray ray = camera.getRay(u, v);
                color_sum += rayColor(ray, scene, 2); // 50 is the max recursion depth
            }

            framebuffer[j * width + i] = color_sum / float(num_samples);
        }
    }

    // Upload framebuffer to texture
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGB, GL_FLOAT, framebuffer.data());

    // Render using the shader program
    glUseProgram(shaderProgram);
    glBindTexture(GL_TEXTURE_2D, texture);

    // Render a full-screen quad
    renderQuad();
}

// Function to calculate ray color based on intersections and lighting
glm::vec3 rayColor(const Ray& ray, const Scene& scene, int depth) {
    HitRecord rec;
    if (depth <= 0) {
        return glm::vec3(0.0f);
    }

    if (scene.hit(ray, 0.001f, FLT_MAX, rec)) {
        glm::vec3 attenuation;
        glm::vec3 scattered = rec.material->scatter(ray, rec, attenuation);
        
        // Lighting calculations
        glm::vec3 color = glm::vec3(0.0f);
        for (const auto& light : scene.lights) {
            glm::vec3 light_dir = glm::normalize(light.position - rec.p);
            float light_distance = glm::length(light.position - rec.p);
            
            // Check for shadows
            Ray shadow_ray(rec.p, light_dir);
            HitRecord shadow_rec;
            if (!scene.hit(shadow_ray, 0.001f, light_distance - 0.001f, shadow_rec)) {
                float diffuse = glm::max(glm::dot(rec.normal, light_dir), 0.0f);
                color += attenuation * diffuse * light.intensity;
            }
        }

        // Recursive call for reflected rays
        glm::vec3 reflected_color = rayColor(Ray(rec.p, scattered), scene, depth - 1);
        return color * attenuation + reflected_color * attenuation;
    }

    // Background color
    glm::vec3 unit_direction = glm::normalize(ray.direction);
    float t = 0.5f * (unit_direction.y + 1.0f);
    return (1.0f - t) * glm::vec3(1.0f, 1.0f, 1.0f) + t * glm::vec3(0.5f, 0.7f, 1.0f);
}

GLuint loadShader(const char* vertexPath, const char* fragmentPath) {
    // Read shader source code
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
    glShaderSource(vertex, 1, &vShaderCode, NULL);
    glCompileShader(vertex);
    int success;
    char infoLog[512];
    glGetShaderiv(vertex, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(vertex, 512, NULL, infoLog);
        std::cerr << "Vertex Shader Compilation Error: " << infoLog << std::endl;
    }

    // Compile fragment shader
    GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment, 1, &fShaderCode, NULL);
    glCompileShader(fragment);
    glGetShaderiv(fragment, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(fragment, 512, NULL, infoLog);
        std::cerr << "Fragment Shader Compilation Error: " << infoLog << std::endl;
    }

    // Link shaders into a program
    GLuint shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vertex);
    glAttachShader(shaderProgram, fragment);
    glLinkProgram(shaderProgram);
    glGetProgramiv(shaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(shaderProgram, 512, NULL, infoLog);
        std::cerr << "Shader Program Linking Error: " << infoLog << std::endl;
    }

    glDeleteShader(vertex);
    glDeleteShader(fragment);

    return shaderProgram;
}

int main() {
    // Initialize GLFW and create a window
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return -1;
    }

    // Set GLFW to use OpenGL 3.3 core profile
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(WIDTH, HEIGHT, "RayZen", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);

    // Initialize GLEW
    if (glewInit() != GLEW_OK) {
        std::cerr << "Failed to initialize GLEW" << std::endl;
        return -1;
    }

    // Initialize quad for rendering
    initQuad();

    GLuint shaderProgram = loadShader("../shaders/shader.vs", "../shaders/shader.fs");
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, WIDTH, HEIGHT, 0, GL_RGB, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Setup Scene
    Scene scene;
    // Lambertian material
    Material lambertian(MaterialType::Lambertian);
    lambertian.setAlbedo(glm::vec3(0.8f, 0.3f, 0.3f));
    scene.addObject(Sphere(glm::vec3(0.0f, 0.0f, -1.0f), 0.5f, &lambertian));
    // Phong material
    Material phong(MaterialType::Phong);
    phong.setDiffuse(glm::vec3(0.8f, 0.6f, 0.2f));
    phong.setSpecular(glm::vec3(1.0f));
    phong.setShininess(32.0f);
    scene.addObject(Sphere(glm::vec3(-1.0f, 0.0f, -1.0f), 0.5f, &phong));
    // Metallic material
    Material metallic(MaterialType::Metallic);
    metallic.setAlbedo(glm::vec3(0.8f, 0.8f, 0.8f));
    metallic.setFuzz(0.3f);
    scene.addObject(Sphere(glm::vec3(1.0f, 0.0f, -1.0f), 0.5f, &metallic));
    // Subsurface scattering material
    Material sss(MaterialType::Subsurface);
    sss.setAlbedo(glm::vec3(0.7f, 0.5f, 0.3f));
    sss.setSSSScale(0.1f);
    scene.addObject(Sphere(glm::vec3(0.0f, -100.5f, -1.0f), 100.0f, &sss));
    scene.addObject(Sphere(glm::vec3(0.0f, -100.5f, -1.0f), 100.0f, &lambertian)); // Ground
    // Add a light source
    scene.addLight(Light(glm::vec3(5.0f, 5.0f, 0.0f), glm::vec3(1.0f, 1.0f, 1.0f)));
    // Set up the camera
    Camera camera;

    // Render loop
    while (!glfwWindowShouldClose(window)) {
        // Process input (placeholder function)
        float deltaTime = 0.01f; // Assume a fixed time step for simplicity
        //processInput(window, camera);

        // Render the scene
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderScene(camera, scene, WIDTH, HEIGHT, texture, NUM_SAMPLES, shaderProgram);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glDeleteTextures(1, &texture);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
