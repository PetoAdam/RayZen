// RayZen — entry point.
//
// This translation unit owns process bootstrap only:
//   1. CLI parsing
//   2. Logger configuration
//   3. GLFW + GLEW initialization with version fallback
//   4. Scene construction (cube floor + material showcase)
//   5. Hand-off to rayzen::Renderer
//
// All rendering, hot-reload, input handling, and BVH/SSBO management lives in
// the rayzen:: subsystem under include/Renderer/ and src/Renderer/.

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "Camera.h"
#include "GameObject.h"
#include "Light.h"
#include "Logger.h"
#include "Material.h"
#include "Mesh.h"
#include "Scene.h"
#include "Renderer/Renderer.h"
#include "Renderer/RenderConfig.h"
#include "Renderer/MaterialSettings.h"
#include "Renderer/FluidDemo.h"

#ifndef RAYZEN_SOURCE_DIR
#define RAYZEN_SOURCE_DIR "."
#endif

namespace fs = std::filesystem;

namespace {

constexpr unsigned int kInitialWidth = 800;
constexpr unsigned int kInitialHeight = 600;

std::string resolveProjectPath(const fs::path& rel) {
    return (fs::path(RAYZEN_SOURCE_DIR) / rel).string();
}

glm::mat4 makeTransform(const glm::vec3& translation, const glm::vec3& scale = glm::vec3(1.0f)) {
    return glm::scale(glm::translate(glm::mat4(1.0f), translation), scale);
}

GLFWwindow* createWindowWithFallback(unsigned int width, unsigned int height, bool hidden) {
    struct Candidate { int major, minor; };
    const Candidate candidates[] = { {4,6}, {4,5}, {4,4}, {4,3} };
    for (const auto& c : candidates) {
        glfwDefaultWindowHints();
        if (hidden) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, c.major);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, c.minor);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        if (c.major >= 4) glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        GLFWwindow* w = glfwCreateWindow(width, height, "RayZen", nullptr, nullptr);
        if (w) {
            Logger::info("Created OpenGL context " + std::to_string(c.major) + "." + std::to_string(c.minor));
            return w;
        }
        Logger::info("Failed to create OpenGL context " + std::to_string(c.major) + "." +
                     std::to_string(c.minor) + ", trying lower version");
    }
    return nullptr;
}

bool buildScene(Scene& scene, const rayzen::RenderConfig& config) {
    scene.camera = Camera(
        glm::vec3(0.0f, 0.75f, 5.5f),
        glm::normalize(glm::vec3(0.0f, -0.08f, -1.0f)),
        glm::vec3(0.0f, 1.0f, 0.0f),
        55.0f,
        float(kInitialWidth) / float(kInitialHeight),
        0.1f,
        100.0f);

    scene.materials = {
        Material(glm::vec3(0.95f, 0.05f, 0.05f), 0.0f, 0.95f, 0.0f, 0.0f, 1.5f),
        Material(glm::vec3(0.08f, 0.72f, 0.08f), 1.0f, 0.35f, 0.3f, 0.0f, 1.5f),
        Material(glm::vec3(1.0f),                1.0f, 0.05f, 1.0f, 0.0f, 1.5f),
        Material(glm::vec3(1.0f),                0.0f, 0.02f, 0.0f, 1.0f, 1.5f),
        Material(glm::vec3(0.08f, 0.28f, 0.8f),  0.0f, 0.25f, 0.0f, 0.0f, 1.5f),
        Material(glm::vec3(0.9f, 0.5f, 0.3f),   0.0f, 0.4f,  0.0f, 0.0f, 1.3f, 0.35f)
    };
    scene.textures.push_back(TextureAsset::checker());
    scene.materials[0].baseColorTexture = 0;

    scene.lights.push_back(Light(glm::vec4(5.0f, 5.0f, 5.0f, 1.0f), glm::vec3(1.0f), 300.0f));
    scene.lights.push_back(Light(glm::vec4(0.8f, 1.4f, 0.3f, 0.0f), glm::vec3(1.0f), 2.0f));
    scene.lights[0].power = config.pointLightPower;
    scene.lights[1].power = config.directionalLightPower;

    auto floor = std::make_shared<Mesh>();
    auto monkey = std::make_shared<Mesh>();
    auto plasticMonkey = std::make_shared<Mesh>();
    auto waxCube = std::make_shared<Mesh>();
    auto glassCube = std::make_shared<Mesh>();
    const std::string cubeMeshPath = resolveProjectPath("meshes/cube.obj");
    const std::string monkeyMeshPath = resolveProjectPath("meshes/monkey.obj");
    if (!floor->loadFromOBJ(cubeMeshPath, 0) ||
        !monkey->loadFromOBJ(monkeyMeshPath, 1) ||
        !plasticMonkey->loadFromOBJ(monkeyMeshPath, 4) ||
        !waxCube->loadFromOBJ(cubeMeshPath, 5) ||
        !glassCube->loadFromOBJ(cubeMeshPath, 3)) {
        Logger::error("Scene asset loading failed");
        return false;
    }
    scene.gameObjects.push_back(GameObject{floor,
        makeTransform(glm::vec3(0.0f, -2.2f, 0.0f), glm::vec3(12.0f, 0.25f, 12.0f))});
    scene.gameObjects.push_back(GameObject{monkey,
        makeTransform(glm::vec3(0.0f, -0.15f, 0.0f), glm::vec3(1.45f))});
    scene.gameObjects.push_back(GameObject{glassCube,
        makeTransform(glm::vec3(2.75f, -1.05f, -1.65f), glm::vec3(0.9f))});
    scene.gameObjects.push_back(GameObject{plasticMonkey,
        makeTransform(glm::vec3(-2.75f, -0.65f, -1.65f), glm::vec3(0.9f))});
    scene.gameObjects.push_back(GameObject{waxCube,
        makeTransform(glm::vec3(-2.0f, -1.5f, 0.1f), glm::vec3(0.4f))});
    return true;
}

void printControls() {
    Logger::info("==== RayZen Controls ====");
    Logger::info("WASD: Move camera");
    Logger::info("Mouse Drag (LMB): Rotate camera");
    Logger::info("L: Toggle light debug markers");
    Logger::info("B: Toggle BVH wireframe debug");
    Logger::info("N: Toggle BVH debug mode (TLAS/BLAS)");
    Logger::info("F2: Toggle swap interval (0/1)");
    Logger::info("F1: Toggle editor raster mode");
    Logger::info("F3: Settings; Tab changes page, arrows adjust, F4 saves materials");
    Logger::info("ESC: Quit");
    Logger::info("========================");
}

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool isLikelySoftwareRenderer(const std::string& vendor, const std::string& renderer) {
    const std::string v = toLower(vendor);
    const std::string r = toLower(renderer);
    return r.find("llvmpipe") != std::string::npos ||
           r.find("softpipe") != std::string::npos ||
           r.find("swrast") != std::string::npos ||
           r.find("software") != std::string::npos ||
           v.find("mesa/x.org") != std::string::npos;
}

bool isWslD3D12Renderer(const std::string& vendor, const std::string& renderer) {
    const std::string v = toLower(vendor);
    const std::string r = toLower(renderer);
    return v.find("microsoft") != std::string::npos &&
           r.find("d3d12") != std::string::npos;
}

bool isLikelyWslEnvironment() {
    if (const char* distro = std::getenv("WSL_DISTRO_NAME")) {
        if (distro[0] != '\0') return true;
    }

    std::ifstream osrelease("/proc/sys/kernel/osrelease");
    if (!osrelease) return false;

    std::string value;
    std::getline(osrelease, value);
    const std::string lower = toLower(value);
    return lower.find("microsoft") != std::string::npos ||
           lower.find("wsl") != std::string::npos;
}

void maybeEnableWslD3D12Overrides(bool enabled) {
#if defined(__linux__)
    if (!enabled || !isLikelyWslEnvironment()) return;

    // In WSLg, forcing the Mesa D3D12 Gallium path avoids llvmpipe fallback
    // in shells where auto-detection picks software rendering.
    if (std::getenv("MESA_LOADER_DRIVER_OVERRIDE") == nullptr) {
        setenv("MESA_LOADER_DRIVER_OVERRIDE", "d3d12", 1);
    }
    if (std::getenv("GALLIUM_DRIVER") == nullptr) {
        setenv("GALLIUM_DRIVER", "d3d12", 1);
    }
#else
    (void)enabled;
#endif
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && (std::string(argv[1]) == "--restore-warm" || std::string(argv[1]) == "--quit-warm")) {
        const auto command = std::string(argv[1]) == "--restore-warm"
            ? rayzen::WarmCommand::Restore : rayzen::WarmCommand::Quit;
        if (!rayzen::WarmWindow::send(command)) return EXIT_FAILURE;
        std::cout << "Warm-window request sent.\n";
        return EXIT_SUCCESS;
    }
    LogLevel logLevel = LogLevel::INFO;
    rayzen::RendererOptions options;
    bool requireHardwareRenderer = false;
    bool wslD3D12Override = true;
    bool autoWarmupEnabled = false;
    bool prepareShaders = false;
    bool prepareBVH = false;
    bool shaderCacheOnly = false;
    bool explicitPathTracerOnly = false;
    std::vector<std::string> texturePaths;
    std::string fluidMode;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--log=debug") logLevel = LogLevel::DEBUG;
        else if (arg == "--log=info") logLevel = LogLevel::INFO;
        else if (arg == "--log=error") logLevel = LogLevel::ERROR;
        else if (arg == "--rebuild-bvh") options.forceRebuildBVH = true;
        else if (arg == "--prepare-cache") { prepareShaders = true; prepareBVH = true; }
        else if (arg == "--prepare-shaders") prepareShaders = true;
        else if (arg == "--prepare-bvh") prepareBVH = true;
        else if (arg == "--cache-only") { shaderCacheOnly = true; options.cacheOnly = true; }
        else if (arg == "--shader-cache-only") shaderCacheOnly = true;
        else if (arg == "--bvh-cache-only") options.cacheOnly = true;
        else if (arg == "--path-tracer-only") { options.requestPathTracerOnly = true; explicitPathTracerOnly = true; }
        else if (arg == "--keep-warm") options.keepWarm = true;
        else if (arg == "--restore-warm" || arg == "--quit-warm") {
            std::cerr << arg << " must be used without other arguments\n";
            return EXIT_FAILURE;
        }
        else if (arg == "--allow-software-renderer") options.allowSoftwareRenderer = true;
        else if (arg == "--require-hardware-renderer") requireHardwareRenderer = true;
        else if (arg == "--no-wsl-d3d12-override") wslD3D12Override = false;
        else if (arg == "--auto-warmup") autoWarmupEnabled = true;
        else if (arg == "--no-auto-warmup") autoWarmupEnabled = false;
        else if (arg.rfind("--fluid=", 0) == 0) {
            fluidMode = arg.substr(8);
            if (fluidMode != "2d" && fluidMode != "3d") {
                std::cerr << "--fluid requires 2d or 3d\n";
                return EXIT_FAILURE;
            }
        }
        else if (arg == "--allow-backend-fallback") options.requireComputeWhenConfigured = false;
        else if (arg.rfind("--config=", 0) == 0) {
            options.configPath = arg.substr(std::string("--config=").size());
            if (options.configPath.empty() || !fs::is_regular_file(options.configPath)) {
                std::cerr << "--config requires an existing configuration file\n";
                return EXIT_FAILURE;
            }
            options.configPath = fs::absolute(options.configPath);
        }
        else if (arg.rfind("--texture=", 0) == 0) {
            const std::string path = arg.substr(std::string("--texture=").size());
            if (path.empty()) {
                std::cerr << "--texture requires a PNG file path\n";
                return EXIT_FAILURE;
            }
            texturePaths.push_back(path);
        }
        else if (arg.rfind("--benchmark-frames=", 0) == 0) {
            try {
                const std::string value = arg.substr(std::string("--benchmark-frames=").size());
                size_t parsed = 0;
                options.benchmarkFrames = std::stoi(value, &parsed);
                if (parsed != value.size()) {
                    std::cerr << "Invalid value for --benchmark-frames\n";
                    return EXIT_FAILURE;
                }
                if (options.benchmarkFrames < 1 || options.benchmarkFrames > 100000) {
                    std::cerr << "--benchmark-frames must be between 1 and 100000\n";
                    return EXIT_FAILURE;
                }
            } catch (const std::exception&) {
                std::cerr << "Invalid value for --benchmark-frames\n";
                return EXIT_FAILURE;
            }
        }
        else if (arg.rfind("--swap-interval=", 0) == 0) {
            try {
                options.overrideSwapInterval = std::clamp(
                    std::stoi(arg.substr(std::string("--swap-interval=").size())), 0, 2);
                options.swapIntervalOverridden = true;
            } catch (const std::exception&) {
                std::cerr << "Invalid value for --swap-interval\n";
            }
        }
        else if (arg.rfind("--warmup-frames=", 0) == 0) {
            try {
                options.warmupFrames = std::max(0,
                    std::stoi(arg.substr(std::string("--warmup-frames=").size())));
            } catch (const std::exception&) {
                std::cerr << "Invalid value for --warmup-frames\n";
            }
        }
    }
    const bool prepareCache = prepareShaders || prepareBVH;
    if (!fluidMode.empty() && (prepareCache || shaderCacheOnly || options.cacheOnly || options.keepWarm ||
                               options.forceRebuildBVH || explicitPathTracerOnly || options.warmupFrames > 0 || autoWarmupEnabled ||
                               !options.configPath.empty() || !texturePaths.empty())) {
        std::cerr << "--fluid cannot be combined with scene, cache, warm window or shader warmup options\n";
        return EXIT_FAILURE;
    }
    if ((prepareCache && (shaderCacheOnly || options.cacheOnly)) ||
        (options.cacheOnly && options.forceRebuildBVH)) {
        std::cerr << "Preparation cannot be combined with load-only modes; "
                     "--bvh-cache-only cannot be combined with --rebuild-bvh\n";
        return EXIT_FAILURE;
    }
    if (shaderCacheOnly) {
        rayzen::GLProgram::setCacheMode(rayzen::ShaderCacheMode::LoadOnly);
        options.requestPathTracerOnly = true;
        options.disableAsyncFragmentCompile = true;
        options.requireComputeWhenConfigured = true;
    } else if (prepareCache) {
        rayzen::GLProgram::setCacheMode(rayzen::ShaderCacheMode::Prepare);
    }
    if (options.warmupFrames > 0) options.requestPathTracerOnly = true;
    if (options.benchmarkFrames > 0) options.requestPathTracerOnly = true;
    Logger::setLevel(logLevel);

    maybeEnableWslD3D12Overrides(wslD3D12Override);

    if (fluidMode.empty()) printControls();

    if (!glfwInit()) {
        Logger::error("Failed to initialize GLFW");
        return EXIT_FAILURE;
    }

    GLFWwindow* window = createWindowWithFallback(kInitialWidth, kInitialHeight, prepareCache);
    if (!window) {
        Logger::error("Failed to create GLFW window with compatible OpenGL context");
        glfwTerminate();
        return EXIT_FAILURE;
    }
    if (options.keepWarm && (prepareCache || options.benchmarkFrames > 0)) {
        std::cerr << "--keep-warm cannot be combined with cache preparation or benchmarking\n";
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }
    glfwMakeContextCurrent(window);

    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        Logger::error("Failed to initialize GLEW");
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }
    if (const GLubyte* v = glGetString(GL_VERSION)) {
        Logger::info(std::string("GL version: ") + reinterpret_cast<const char*>(v));
    }
    std::string glVendor;
    std::string glRenderer;
    std::string glslVersion;
    if (const GLubyte* v = glGetString(GL_VENDOR)) {
        glVendor = reinterpret_cast<const char*>(v);
        Logger::info("GL vendor: " + glVendor);
    }
    if (const GLubyte* v = glGetString(GL_RENDERER)) {
        glRenderer = reinterpret_cast<const char*>(v);
        Logger::info("GL renderer: " + glRenderer);
    }
    if (const GLubyte* v = glGetString(GL_SHADING_LANGUAGE_VERSION)) {
        glslVersion = reinterpret_cast<const char*>(v);
        Logger::info("GLSL version: " + glslVersion);
    }
    const bool softwareRenderer = isLikelySoftwareRenderer(glVendor, glRenderer);
    if (softwareRenderer && !options.allowSoftwareRenderer) {
        const bool inWsl = isLikelyWslEnvironment();
        if (inWsl && !requireHardwareRenderer) {
            Logger::error("Detected software OpenGL renderer: " + glRenderer);
            Logger::error("WSL2 commonly exposes llvmpipe instead of hardware OpenGL.");
            Logger::error("Continuing in compatibility mode (software renderer enabled).");
            Logger::error("Use --require-hardware-renderer to enforce strict GPU-only startup.");
            options.allowSoftwareRenderer = true;
        }
    }
    if (softwareRenderer && !options.allowSoftwareRenderer) {
        Logger::error("Detected software OpenGL renderer: " + glRenderer);
        Logger::error("RayZen requires a hardware GPU renderer in strict mode.");
        Logger::error("Use --allow-software-renderer to bypass this check.");
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }
    if (softwareRenderer) {
        Logger::info("Software renderer allowed by CLI override.");
    }

    if (!fluidMode.empty()) {
        bool success = false;
        glfwSetWindowTitle(window, fluidMode == "2d" ? "RayZen Fluid 2D" : "RayZen Fluid 3D");
        if (options.swapIntervalOverridden) glfwSwapInterval(options.overrideSwapInterval);
        {
            rayzen::FluidDemo fluid;
            success = fluid.initialize(fs::path(RAYZEN_SOURCE_DIR) / "shaders",
                fluidMode == "2d" ? rayzen::FluidDimension::Two : rayzen::FluidDimension::Three);
            if (success) success = fluid.run(window, options.benchmarkFrames);
        }
        glfwDestroyWindow(window);
        glfwTerminate();
        return success ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (!prepareCache && !shaderCacheOnly && autoWarmupEnabled && options.warmupFrames == 0 && isWslD3D12Renderer(glVendor, glRenderer)) {
        options.warmupFrames = 1;
        Logger::info("WSL D3D12 detected: enabling 1 hidden warm-up frame to avoid long first-frame black screen.");
    }
    if (!prepareCache && !shaderCacheOnly && !autoWarmupEnabled &&
        options.warmupFrames == 0 &&
        !options.requestPathTracerOnly &&
        isWslD3D12Renderer(glVendor, glRenderer)) {
        options.startInEditorMode = true;
        options.preferFragmentToggleFromEditor = false;
        options.disableAsyncFragmentCompile = false;
        options.asyncComputeJit = true;
        Logger::info("WSL D3D12 detected: starting in editor mode for immediate visual output.");
        Logger::info("WSL D3D12 detected: the path tracer will activate automatically when ready.");
        Logger::info("WSL D3D12 detected: precompiling compute shader in background (first-run may take a minute).");
    }

    glEnable(GL_DEPTH_TEST);
    glClearColor(0.05f, 0.05f, 0.07f, 1.0f);

    const fs::path configPath = options.configPath.empty()
        ? fs::path(RAYZEN_SOURCE_DIR) / "config" / "render_config.ini" : options.configPath;
    rayzen::RenderConfigWatcher bootWatcher(configPath);
    bootWatcher.loadInitial();

    Scene scene;
    if ((!prepareCache || prepareBVH) && !buildScene(scene, bootWatcher.current())) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }
    for (const std::string& path : texturePaths) {
        TextureAsset texture;
        if (!texture.loadPNG(path)) {
            glfwDestroyWindow(window);
            glfwTerminate();
            return EXIT_FAILURE;
        }
        scene.textures.push_back(std::move(texture));
        Logger::info("Material texture " + std::to_string(scene.textures.size() - 1) + ": " + path);
    }
    if ((!prepareCache || prepareBVH) &&
        !rayzen::loadMaterialSettings(fs::path(RAYZEN_SOURCE_DIR) / "config" / "materials.ini", scene)) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    bool success = false;
    if (prepareCache) {
        success = true;
        if (prepareBVH) {
            rayzen::SceneGPU sceneGPU;
            success = sceneGPU.initialize(scene, true, false, true);
        }
        if (success && prepareShaders) {
            rayzen::GLProgram editor, compute, realtime, lambertian, caustics, resolve, fragment, settings;
            success = editor.loadGraphics(resolveProjectPath("shaders/editor_vertex.glsl"),
                                          resolveProjectPath("shaders/editor_fragment.glsl")) &&
                settings.loadGraphics(resolveProjectPath("shaders/settings_panel.vert"),
                                      resolveProjectPath("shaders/settings_panel.frag")) &&
                compute.loadCompute(resolveProjectPath("shaders/path_trace.comp")) &&
                realtime.loadCompute(resolveProjectPath("shaders/realtime_ray.comp")) &&
                lambertian.loadCompute(resolveProjectPath("shaders/lambertian.comp")) &&
                caustics.loadCompute(resolveProjectPath("shaders/caustics.comp")) &&
                resolve.loadGraphics(resolveProjectPath("shaders/display_resolve.vert"),
                                     resolveProjectPath("shaders/display_resolve.frag")) &&
                fragment.loadGraphics(resolveProjectPath("shaders/vertex_shader.glsl"),
                                      resolveProjectPath("shaders/fragment_shader.glsl"));
        }
        if (success) Logger::info("Requested caches prepared successfully");
        else Logger::error("Cache preparation failed");
    } else {
        rayzen::Renderer renderer(window, scene, fs::path(RAYZEN_SOURCE_DIR), options);
        success = renderer.initialize();
        if (success) success = renderer.run();
        else Logger::error("Renderer initialization failed");
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
