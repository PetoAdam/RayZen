#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <streambuf>
#include <string>
#include <vector>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "Renderer/Renderer.h"

#ifdef __linux__
#include <unistd.h>
#endif

namespace fs = std::filesystem;

class ActivationOutput : public std::streambuf {
public:
    ActivationOutput(std::streambuf* output, GLFWwindow* window) : output_(output), window_(window) {}
    bool activated() const { return activated_; }

protected:
    std::streamsize xsputn(const char* data, std::streamsize size) override {
        const auto written = output_->sputn(data, size);
        line_.append(data, static_cast<size_t>(size));
        if (!activated_ && line_.find("Compute shader precompile complete. PBR ray tracing active.") != std::string::npos) {
            activated_ = true;
            glfwSetWindowShouldClose(window_, GLFW_TRUE);
        }
        if (line_.size() > 4096) line_.erase(0, line_.size() - 1024);
        return written;
    }

    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) return traits_type::not_eof(value);
        const char c = traits_type::to_char_type(value);
        return xsputn(&c, 1) == 1 ? value : traits_type::eof();
    }

    int sync() override { return output_->pubsync(); }

private:
    std::streambuf* output_;
    GLFWwindow* window_;
    std::string line_;
    bool activated_ = false;
};

int main() {
#ifdef __linux__
    const std::string scope = "test-" + std::to_string(getpid());
    {
        rayzen::WarmWindow control(scope);
        rayzen::WarmWindow duplicate(scope);
        if (!control.open() || duplicate.open() ||
            !rayzen::WarmWindow::send(rayzen::WarmCommand::Restore, scope) ||
            control.poll() != rayzen::WarmCommand::Restore ||
            !rayzen::WarmWindow::send(rayzen::WarmCommand::Quit, scope) ||
            control.poll() != rayzen::WarmCommand::Quit) {
            std::cerr << "Warm-window control failed\n";
            return 1;
        }
    }
    if (rayzen::WarmWindow::send(rayzen::WarmCommand::Restore, scope)) {
        std::cerr << "Warm-window control remained available after shutdown\n";
        return 1;
    }
#endif
    if (!glfwInit()) return 77;
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(64, 64, "renderer startup test", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 77;
    }
    glfwMakeContextCurrent(window);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return 77;
    }
    glGetError();

    const fs::path oldDir = fs::current_path();
    const fs::path dir = fs::temp_directory_path() / ("rayzen-startup-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(dir);
    fs::current_path(dir);
    Scene scene;
    scene.lights.emplace_back(glm::vec4(0, 1, 0, 0), glm::vec3(1), 1.0f);
    rayzen::RendererOptions options;
    options.configPath = dir / "chosen.ini";
    std::ofstream(options.configPath) << "point_light_power=7\nshading_mode=ray_tracing\n";
    options.startInEditorMode = true;
    options.asyncComputeJit = true;
    std::streambuf* previousOutput = std::cout.rdbuf();
    ActivationOutput output(previousOutput, window);
    std::cout.rdbuf(&output);
    bool success = false;
    {
        rayzen::Renderer renderer(window, scene, dir, options);
        success = renderer.initialize() && scene.lights[0].power == 7.0f &&
                  !fs::exists(dir / "config" / "render_config.ini") &&
                  renderer.run() && output.activated();
        GLint texture = 0;
        glGetIntegeri_v(GL_IMAGE_BINDING_NAME, 0, &texture);
        if (success && texture != 0) {
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
            GLint width = 0, height = 0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
            if (width > 0 && height > 0) {
                std::vector<float> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, pixels.data());
                const size_t center = (static_cast<size_t>(height / 2) * width + width / 2) * 4;
                success = pixels[center] > 0.15f && pixels[center + 2] > 0.55f && glGetError() == GL_NO_ERROR;
            } else {
                success = false;
            }
        } else {
            success = false;
        }
    }
    std::cout.rdbuf(previousOutput);
    fs::current_path(oldDir);
    fs::remove_all(dir);
    glfwDestroyWindow(window);
    glfwTerminate();
    if (!success) std::cerr << "Automatic path tracer startup failed\n";
    return success ? 0 : 1;
}
