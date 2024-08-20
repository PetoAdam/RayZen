#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <float.h>
#include <cmath>

#include "Ray.h"
#include "Sphere.h"
#include "Scene.h"
#include "Camera.h"
#include "Material.h"
#include "Light.h"

// Constants
const unsigned int SCR_WIDTH = 800;
const unsigned int SCR_HEIGHT = 600;
const int MAX_SPHERES = 10;
const int MAX_MATERIALS = 10;

// Function prototypes
void framebuffer_size_callback(GLFWwindow* window, int width, int height);
void processInput(GLFWwindow* window);
GLuint loadShaders(const char* vertexPath, const char* fragmentPath);
void setupFramebuffers(GLuint &framebufferA, GLuint &framebufferB, GLuint &textureA, GLuint &textureB, GLuint &rbo);
void renderToFramebuffer(GLuint framebuffer, GLuint shaderProgram, GLuint quadVAO);
void displayFramebuffer(GLuint texture, GLuint shaderProgram, GLuint quadVAO);

// Global variables
GLuint framebufferA, framebufferB;
GLuint textureA, textureB;
GLuint rbo;
GLuint quadVAO, quadVBO;
GLuint shaderProgram;

void sendSceneDataToShader(GLuint shaderProgram, const Camera& camera, const std::vector<Sphere>& spheres, const std::vector<Material>& materials) {
    glUseProgram(shaderProgram);

    GLint viewMatrixLoc = glGetUniformLocation(shaderProgram, "camera.viewMatrix");
    if (viewMatrixLoc == -1) std::cerr << "Uniform 'camera.viewMatrix' not found!" << std::endl;

    GLint projMatrixLoc = glGetUniformLocation(shaderProgram, "camera.projectionMatrix");
    if (projMatrixLoc == -1) std::cerr << "Uniform 'camera.projectionMatrix' not found!" << std::endl;

    GLint camPosLoc = glGetUniformLocation(shaderProgram, "camera.position");
    if (camPosLoc == -1) std::cerr << "Uniform 'camera.position' not found!" << std::endl;

    glUniformMatrix4fv(viewMatrixLoc, 1, GL_FALSE, glm::value_ptr(camera.viewMatrix));
    glUniformMatrix4fv(projMatrixLoc, 1, GL_FALSE, glm::value_ptr(camera.projectionMatrix));
    glUniform3fv(camPosLoc, 1, glm::value_ptr(camera.position));

    GLint spheresLoc = glGetUniformLocation(shaderProgram, "spheres");
    if (spheresLoc == -1) std::cerr << "Uniform 'spheres' not found!" << std::endl;
    glUniform3fv(glGetUniformLocation(shaderProgram, "spheres[0].center"), spheres.size(), glm::value_ptr(spheres[0].center));
    glUniform1fv(glGetUniformLocation(shaderProgram, "spheres[0].radius"), spheres.size(), &spheres[0].radius);
    glUniform1iv(glGetUniformLocation(shaderProgram, "spheres[0].materialIndex"), spheres.size(), &spheres[0].materialIndex);
    if(glGetUniformLocation(shaderProgram, "spheres[0].radius") == -1) std::cerr << "Materialindex not found" << std::endl;

    GLint materialsLoc = glGetUniformLocation(shaderProgram, "materials");
    glUniform3fv(glGetUniformLocation(shaderProgram, "materials[0].diffuse"), materials.size(), glm::value_ptr(materials[0].diffuse));
    glUniform3fv(glGetUniformLocation(shaderProgram, "materials[0].specular"), materials.size(), glm::value_ptr(materials[0].specular));
    glUniform1fv(glGetUniformLocation(shaderProgram, "materials[0].shininess"), materials.size(), &materials[0].shininess);
    glUniform1fv(glGetUniformLocation(shaderProgram, "materials[0].metallic"), materials.size(), &materials[0].metallic);
    glUniform1fv(glGetUniformLocation(shaderProgram, "materials[0].fuzz"), materials.size(), &materials[0].fuzz);
    glUniform3fv(glGetUniformLocation(shaderProgram, "materials[0].albedo"), materials.size(), glm::value_ptr(materials[0].albedo));

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        std::cerr << "OpenGL Error: " << err << std::endl;
    }
}

void setupQuad() {
    GLfloat vertices[] = {
        // Positions        // Texture Coords
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 1.0f
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

void setupFramebuffers(GLuint &framebufferA, GLuint &framebufferB, GLuint &textureA, GLuint &textureB, GLuint &rbo) {
    glGenFramebuffers(1, &framebufferA);
    glGenFramebuffers(1, &framebufferB);
    glGenTextures(1, &textureA);
    glGenTextures(1, &textureB);
    glGenRenderbuffers(1, &rbo);

    // Setup framebufferA
    glBindFramebuffer(GL_FRAMEBUFFER, framebufferA);
    glBindTexture(GL_TEXTURE_2D, textureA);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, SCR_WIDTH, SCR_HEIGHT, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textureA, 0);

    // Setup framebufferB
    glBindFramebuffer(GL_FRAMEBUFFER, framebufferB);
    glBindTexture(GL_TEXTURE_2D, textureB);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, SCR_WIDTH, SCR_HEIGHT, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textureB, 0);

    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, SCR_WIDTH, SCR_HEIGHT);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "Framebuffer not complete!" << std::endl;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0); // Unbind framebuffer
}

void renderToFramebuffer(GLuint framebuffer, GLuint shaderProgram, GLuint quadVAO) {
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glViewport(0, 0, SCR_WIDTH, SCR_HEIGHT);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(shaderProgram);
    glBindVertexArray(quadVAO);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glBindVertexArray(0);
}

void displayFramebuffer(GLuint texture, GLuint shaderProgram, GLuint quadVAO) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, SCR_WIDTH, SCR_HEIGHT);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(shaderProgram);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindVertexArray(quadVAO);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glBindVertexArray(0);
}

int main() {
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(SCR_WIDTH, SCR_HEIGHT, "Ray Tracing", nullptr, nullptr);
    if (window == nullptr) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);

    glewExperimental = true;
    if (glewInit() != GLEW_OK) {
        std::cerr << "Failed to initialize GLEW" << std::endl;
        return -1;
    }

    glViewport(0, 0, SCR_WIDTH, SCR_HEIGHT);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);

    shaderProgram = loadShaders("../shaders/vertex_shader.glsl", "../shaders/fragment_shader.glsl");

    setupQuad();
    setupFramebuffers(framebufferA, framebufferB, textureA, textureB, rbo);

    Camera camera(
        glm::vec3(0.0f, 0.0f, 3.0f),
        glm::vec3(0.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, 1.0f),
        70.0f,
        (float)SCR_WIDTH / (float)SCR_HEIGHT,
        0.001f,
        10000.0f
    );

    std::vector<Material> materials = {
        Material(glm::vec3(0.8f, 0.3f, 0.3f), glm::vec3(0.9f, 0.9f, 0.9f), 32.0f, 0.1f, 0.0f, glm::vec3(0.8f, 0.3f, 0.3f)),
        //Material(glm::vec3(0.3f, 0.8f, 0.3f), glm::vec3(0.9f, 0.9f, 0.9f), 64.0f, 0.5f, 0.1f, glm::vec3(0.3f, 0.8f, 0.3f))
    };

    std::vector<Sphere> spheres = {
        Sphere(glm::vec3(0.0f, 0.0f, -1.0f), 0.5f, 0),
        //Sphere(glm::vec3(1.0f, 0.0f, -1.5f), 0.2f, 1)
    };

    bool useFramebufferA = true;

    while (!glfwWindowShouldClose(window)) {
        processInput(window);

        // Render to the framebuffer
        GLuint currentFramebuffer = useFramebufferA ? framebufferA : framebufferB;
        GLuint currentTexture = useFramebufferA ? textureA : textureB;

        renderToFramebuffer(currentFramebuffer, shaderProgram, quadVAO);

        // Display the result
        displayFramebuffer(currentTexture, shaderProgram, quadVAO);

        // Swap framebuffers
        useFramebufferA = !useFramebufferA;

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glDeleteFramebuffers(1, &framebufferA);
    glDeleteFramebuffers(1, &framebufferB);
    glDeleteTextures(1, &textureA);
    glDeleteTextures(1, &textureB);
    glDeleteRenderbuffers(1, &rbo);
    glDeleteVertexArrays(1, &quadVAO);
    glDeleteBuffers(1, &quadVBO);

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    glViewport(0, 0, width, height);
}

void processInput(GLFWwindow* window) {
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        glfwSetWindowShouldClose(window, true);
}

GLuint loadShaders(const char* vertexPath, const char* fragmentPath) {
    std::ifstream vShaderFile(vertexPath);
    std::ifstream fShaderFile(fragmentPath);
    std::stringstream vShaderStream, fShaderStream;
    vShaderStream << vShaderFile.rdbuf();
    fShaderStream << fShaderFile.rdbuf();
    std::string vertexCode = vShaderStream.str();
    std::string fragmentCode = fShaderStream.str();
    const char* vShaderCode = vertexCode.c_str();
    const char* fShaderCode = fragmentCode.c_str();

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

    GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment, 1, &fShaderCode, NULL);
    glCompileShader(fragment);
    glGetShaderiv(fragment, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(fragment, 512, NULL, infoLog);
        std::cerr << "Fragment Shader Compilation Error: " << infoLog << std::endl;
    }

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
