#include "Renderer/Renderer.h"
#include "Renderer/MaterialSettings.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "Logger.h"

namespace rayzen {

namespace {

bool matricesNearlyEqual(const glm::mat4& lhs, const glm::mat4& rhs, float eps = 1e-5f) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::abs(lhs[c][r] - rhs[c][r]) > eps) return false;
    return true;
}

bool vectorsNearlyEqual(const glm::vec3& a, const glm::vec3& b, float eps = 1e-5f) {
    return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(eps)));
}

std::string formatMs(double ms) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << ms;
    return oss.str();
}

const char* shadingModeName(ShadingMode mode) {
    switch (mode) {
    case ShadingMode::PathTracing: return "Path tracing";
    case ShadingMode::RayTracing: return "PBR ray tracing";
    case ShadingMode::Lambertian: return "Lambertian";
    }
    return "Unknown shading mode";
}

// Map workspace-relative path to absolute via RAYZEN_SOURCE_DIR.
#ifndef RAYZEN_SOURCE_DIR
#define RAYZEN_SOURCE_DIR "."
#endif

std::string resolveProjectPath(const std::string& rel) {
    return (std::filesystem::path(RAYZEN_SOURCE_DIR) / rel).string();
}

// Tracks the framebuffer-resize callback state; only one Renderer is expected per process.
Renderer* g_activeRenderer = nullptr;

void framebufferSizeCallback(GLFWwindow* /*window*/, int width, int height) {
    if (g_activeRenderer) g_activeRenderer->onFramebufferResized(width, height);
}

} // namespace

Renderer::Renderer(GLFWwindow* window,
                   Scene& scene,
                   const std::filesystem::path& projectRoot,
                   RendererOptions options)
    : window_(window),
      scene_(scene),
      projectRoot_(projectRoot),
      options_(options) {}

Renderer::~Renderer() {
    joinAsyncCompile();
    joinAsyncComputeJit();
    if (g_activeRenderer == this) g_activeRenderer = nullptr;
}

bool Renderer::initialize() {
    g_activeRenderer = this;
    glfwSetFramebufferSizeCallback(window_, framebufferSizeCallback);
    if (options_.keepWarm) {
        if (!warmWindow_.open()) return false;
        glfwSetWindowCloseCallback(window_, [](GLFWwindow*) {
            if (g_activeRenderer) g_activeRenderer->hideWarmWindow();
        });
        Logger::info("Warm window enabled. Close the window to hide it; use --restore-warm to reopen.");
    }

    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    viewportWidth_ = std::max(1, w);
    viewportHeight_ = std::max(1, h);
    glViewport(0, 0, viewportWidth_, viewportHeight_);

    const auto configPath = options_.configPath.empty()
        ? projectRoot_ / "config" / "render_config.ini" : options_.configPath;
    configWatcher_ = std::make_unique<RenderConfigWatcher>(configPath);
    configWatcher_->loadInitial();
    if (options_.swapIntervalOverridden) {
        RenderConfig overridden = configWatcher_->current();
        overridden.swapInterval = options_.overrideSwapInterval;
        configWatcher_->setCurrent(overridden);
    }
    requestedSwapInterval_ = configWatcher_->current().swapInterval;
    glfwSwapInterval(requestedSwapInterval_);

    // Apply config-driven light powers immediately (matches old behavior).
    if (scene_.lights.size() >= 1) scene_.lights[0].power = configWatcher_->current().pointLightPower;
    if (scene_.lights.size() >= 2) scene_.lights[1].power = configWatcher_->current().directionalLightPower;

    // Raster pass (always available; required for warmup-while-compiling fallback).
    if (!rasterPass_.initialize(resolveProjectPath("shaders/editor_vertex.glsl"),
                                resolveProjectPath("shaders/editor_fragment.glsl"))) {
        return false;
    }
    if (!settingsPanel_.initialize(resolveProjectPath("shaders/settings_panel.vert"),
                                   resolveProjectPath("shaders/settings_panel.frag"))) {
        Logger::error("Failed to initialize render settings panel");
        return false;
    }

    // Initialize SSBOs and persistently bind them; identical layout for both backends.
    if (!sceneGPU_.initialize(scene_, options_.forceRebuildBVH, options_.cacheOnly)) {
        return false;
    }
    sceneGPU_.bindAll();
    totalTriangles_ = sceneGPU_.totalTriangles();
    rasterPass_.buildSceneGeometry(scene_);

    const RenderConfig& cfg = configWatcher_->current();
    useFragmentBackend_ = (cfg.backend == RendererBackend::Fragment);
    if (useFragmentBackend_ && cfg.causticsEnabled) {
        Logger::error("Caustics require the compute backend");
        return false;
    }

    if (useFragmentBackend_) {
        // Fragment backend: keep async compile path for parity with old behavior.
        const bool forceImmediate = options_.requestPathTracerOnly || options_.warmupFrames > 0;
        if (forceImmediate) {
            if (!legacyPass_.initialize(resolveProjectPath("shaders/vertex_shader.glsl"),
                                        resolveProjectPath("shaders/fragment_shader.glsl"))) {
                return false;
            }
            legacyPass_.resize(viewportWidth_, viewportHeight_);
            legacyAdopted_ = true;
        } else {
            // Try to launch async compile so we can show editor mode immediately.
            if (options_.disableAsyncFragmentCompile || !launchAsyncFragmentCompile()) {
                Logger::info("Async fragment-backend compile unavailable; compiling on primary context");
                if (!legacyPass_.initialize(resolveProjectPath("shaders/vertex_shader.glsl"),
                                            resolveProjectPath("shaders/fragment_shader.glsl"))) {
                    return false;
                }
                legacyPass_.resize(viewportWidth_, viewportHeight_);
                legacyAdopted_ = true;
            } else {
                awaitingFragmentReady_ = true;
                debug_.editorMode = true;
                Logger::info("Fragment-backend shader compiling asynchronously; using editor raster mode until ready");
            }
        }
    } else {
        // Compute backend: simpler pipeline; compile compute + resolve passes synchronously.
        if (!pathTracer_.initialize(resolveProjectPath("shaders/path_trace.comp"), cfg.shadingMode)) {
            if (options_.requireComputeWhenConfigured) {
                Logger::error("Compute backend requested but path_trace.comp failed to initialize. Refusing fallback.");
                Logger::error("Set renderer_backend=fragment or relax strict mode in RendererOptions.");
                return false;
            }
            Logger::error("Compute backend unavailable; falling back to fragment backend");
            useFragmentBackend_ = true;
            if (!legacyPass_.initialize(resolveProjectPath("shaders/vertex_shader.glsl"),
                                        resolveProjectPath("shaders/fragment_shader.glsl"))) {
                return false;
            }
            legacyPass_.resize(viewportWidth_, viewportHeight_);
            legacyAdopted_ = true;
        } else if (!resolvePass_.initialize(resolveProjectPath("shaders/display_resolve.vert"),
                                            resolveProjectPath("shaders/display_resolve.frag"))) {
            if (options_.requireComputeWhenConfigured) {
                Logger::error("Compute backend requested but display_resolve shaders failed to initialize. Refusing fallback.");
                Logger::error("Set renderer_backend=fragment or relax strict mode in RendererOptions.");
                return false;
            }
            Logger::error("Display resolve shader unavailable; falling back to fragment backend");
            useFragmentBackend_ = true;
            if (!legacyPass_.initialize(resolveProjectPath("shaders/vertex_shader.glsl"),
                                        resolveProjectPath("shaders/fragment_shader.glsl"))) {
                return false;
            }
            legacyPass_.resize(viewportWidth_, viewportHeight_);
            legacyAdopted_ = true;
        } else {
            pathTracer_.resize(viewportWidth_, viewportHeight_);
            Logger::info("Renderer backend: compute");
            // Launch background JIT so the heavy driver compile happens while
            // the user sees the editor raster mode instead of freezing on F1.
            if (options_.asyncComputeJit && options_.startInEditorMode) {
                if (!launchAsyncComputeJit()) {
                    Logger::error("Background compute precompile unavailable; first path-traced frame may stall");
                    computeJitReady_.store(true);
                }
            } else {
                computeJitReady_.store(true);
            }
        }
    }

    Logger::info(std::string("Renderer active backend: ") + (useFragmentBackend_ ? "fragment" : "compute"));

    if (options_.startInEditorMode && !options_.requestPathTracerOnly) {
        debug_.editorMode = !useFragmentBackend_ || !legacyPass_.ready();
        debug_.userLockedEditorMode = false;
        pendingJitActivation_ = !useFragmentBackend_ && debug_.editorMode;
        if (debug_.editorMode) {
            Logger::info("Showing editor raster mode while the path tracer prepares; it will activate automatically. Press F1 to stay in editor mode.");
        }
    }

    accumulationView_ = scene_.camera.viewMatrix;
    accumulationProj_ = scene_.camera.projectionMatrix;
    accumulationCameraPos_ = scene_.camera.position;
    accumulationDirty_ = true;
    accumulatedFrames_ = 0;
    return true;
}

void Renderer::hideWarmWindow() {
    glfwSetWindowShouldClose(window_, GLFW_FALSE);
    glfwHideWindow(window_);
    warmWindowHidden_ = true;
    Logger::info("Window hidden; renderer remains warm. Use --restore-warm or --quit-warm.");
}

bool Renderer::launchAsyncFragmentCompile() {
    int contextMajor = glfwGetWindowAttrib(window_, GLFW_CONTEXT_VERSION_MAJOR);
    int contextMinor = glfwGetWindowAttrib(window_, GLFW_CONTEXT_VERSION_MINOR);
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, contextMajor);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, contextMinor);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    if (contextMajor >= 4) glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* hidden = glfwCreateWindow(16, 16, "RayZenFragmentCompile", nullptr, window_);
    glfwDefaultWindowHints();
    if (!hidden) return false;
    fragmentCompileWindow_ = hidden;
    const std::string vp = resolveProjectPath("shaders/vertex_shader.glsl");
    const std::string fp = resolveProjectPath("shaders/fragment_shader.glsl");
    fragmentCompileThread_ = std::thread([this, hidden, vp, fp]() {
        glfwMakeContextCurrent(hidden);
        glewExperimental = GL_TRUE;
        glewInit(); // GLEW init per-context for shared resource use.
        auto start = std::chrono::high_resolution_clock::now();
        GLProgram prog;
        bool ok = prog.loadGraphics(vp, fp);
        auto end = std::chrono::high_resolution_clock::now();
        glFinish();
        if (ok) {
            // Steal the program ID and hand it to the main thread.
            GLuint id = prog.release();
            fragmentProgramHandle_.store(id, std::memory_order_release);
        }
        fragmentCompileMs_.store(std::chrono::duration<double, std::milli>(end - start).count(),
                                 std::memory_order_release);
        fragmentReady_.store(true, std::memory_order_release);
        glfwMakeContextCurrent(nullptr);
    });
    return true;
}

void Renderer::joinAsyncCompile() {
    if (fragmentCompileThread_.joinable()) fragmentCompileThread_.join();
    if (fragmentCompileWindow_) {
        glfwDestroyWindow(fragmentCompileWindow_);
        fragmentCompileWindow_ = nullptr;
    }
}

bool Renderer::launchAsyncComputeJit() {
    int contextMajor = glfwGetWindowAttrib(window_, GLFW_CONTEXT_VERSION_MAJOR);
    int contextMinor = glfwGetWindowAttrib(window_, GLFW_CONTEXT_VERSION_MINOR);
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, contextMajor);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, contextMinor);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    if (contextMajor >= 4) glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* hidden = glfwCreateWindow(16, 16, "RayZenComputeJit", nullptr, window_);
    glfwDefaultWindowHints();
    if (!hidden) return false;
    computeJitWindow_ = hidden;
    const GLuint progId = pathTracer_.programId();

    computeJitThread_ = std::thread([this, hidden, progId]() {
        glfwMakeContextCurrent(hidden);
        glewExperimental = GL_TRUE;
        glewInit();

        // Create a tiny 1x1 RGBA32F image to satisfy the image binding.
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0, GL_RGBA, GL_FLOAT, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);

        GLuint emptyBuffer = 0;
        glGenBuffers(1, &emptyBuffer);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, emptyBuffer);
        glBufferData(GL_SHADER_STORAGE_BUFFER, 1, nullptr, GL_STATIC_DRAW);
        for (GLuint binding : {0u, 1u, 2u, 5u, 6u, 7u, 8u, 9u})
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, emptyBuffer);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

        // One 1x1 dispatch forces the driver to JIT-compile the compute shader.
        glUseProgram(progId);
        glBindImageTexture(0, tex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
        glUniform2f(glGetUniformLocation(progId, "resolution"), 1.0f, 1.0f);
        glUniform1i(glGetUniformLocation(progId, "numTriangles"), 0);
        glUniform1i(glGetUniformLocation(progId, "numLights"), 0);
        glUniform1i(glGetUniformLocation(progId, "uniformBounceBudget"), 1);
        glUniform1i(glGetUniformLocation(progId, "uniformAccumulatedFrames"), 0);
        glUniform1i(glGetUniformLocation(progId, "uniformResetAccumulation"), 1);
        glUniform1i(glGetUniformLocation(progId, "uniformFrameIndex"), 0);
        const glm::mat4 identity(1.0f);
        glUniformMatrix4fv(glGetUniformLocation(progId, "camera.invViewMatrix"), 1, GL_FALSE, glm::value_ptr(identity));
        glUniformMatrix4fv(glGetUniformLocation(progId, "camera.invProjectionMatrix"), 1, GL_FALSE, glm::value_ptr(identity));
        const auto jitStart = std::chrono::steady_clock::now();
        glDispatchCompute(1, 1, 1);
        glFinish(); // blocks until JIT is done
        Logger::info("Compute prewarm dispatch: " + formatMs(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - jitStart).count()) + " ms");

        glDeleteTextures(1, &tex);
        glDeleteBuffers(1, &emptyBuffer);
        glfwMakeContextCurrent(nullptr);

        computeJitReady_.store(true, std::memory_order_release);
        Logger::info("Compute shader JIT precompile complete.");
    });
    return true;
}

void Renderer::joinAsyncComputeJit() {
    if (computeJitThread_.joinable()) computeJitThread_.join();
    if (computeJitWindow_) {
        glfwDestroyWindow(computeJitWindow_);
        computeJitWindow_ = nullptr;
    }
}

void Renderer::pickupAsyncPathTracer() {
    if (!awaitingFragmentReady_ || !fragmentReady_.load(std::memory_order_acquire)) return;

    GLuint program = fragmentProgramHandle_.exchange(0, std::memory_order_acq_rel);
    if (program == 0) {
        Logger::error("Path tracer shader compilation failed; remaining in editor mode");
        awaitingFragmentReady_ = false;
        debug_.userLockedEditorMode = true;
        return;
    }
    legacyPass_.adoptProgram(program);
    legacyPass_.resize(viewportWidth_, viewportHeight_);
    legacyAdopted_ = true;
    awaitingFragmentReady_ = false;
    if (!debug_.userLockedEditorMode) debug_.editorMode = false;
    glfwSwapInterval(requestedSwapInterval_);
    resetAccumulation();
    double compileMs = fragmentCompileMs_.load(std::memory_order_acquire);
    Logger::info("Path tracer shader ready (compile/link time: " + formatMs(compileMs) + " ms)" +
                 (debug_.userLockedEditorMode ? std::string(" Press F1 to enable path tracer when ready.")
                                              : std::string("")));
}

void Renderer::resetAccumulation() {
    accumulationDirty_ = true;
    accumulatedFrames_ = 0;
}

void Renderer::applyConfig(const RenderConfig& next) {
    const RenderConfig prev = configWatcher_->current();
    if (next.causticsEnabled && useFragmentBackend_) {
        Logger::error("Caustics require the compute backend; settings were not applied");
        return;
    }
    bool needsReset = false;

    if (scene_.lights.size() >= 1 && std::abs(scene_.lights[0].power - next.pointLightPower) > 1e-5f) {
        scene_.lights[0].power = next.pointLightPower;
        needsReset = true;
    }
    if (scene_.lights.size() >= 2 && std::abs(scene_.lights[1].power - next.directionalLightPower) > 1e-5f) {
        scene_.lights[1].power = next.directionalLightPower;
        needsReset = true;
    }
    if (prev.shadingMode != next.shadingMode ||
        prev.pathTracerBounceBudget != next.pathTracerBounceBudget ||
        std::abs(prev.pathTracerAmbient - next.pathTracerAmbient) > 1e-5f ||
        prev.accumulationEnabled != next.accumulationEnabled ||
        prev.subsurfaceEnabled != next.subsurfaceEnabled ||
        prev.causticsEnabled != next.causticsEnabled ||
        prev.causticPhotonCount != next.causticPhotonCount) {
        needsReset = true;
    }
    if (prev.swapInterval != next.swapInterval) {
        requestedSwapInterval_ = next.swapInterval;
        glfwSwapInterval(requestedSwapInterval_);
        Logger::info("Render config: swap interval updated to " + std::to_string(requestedSwapInterval_));
    }
    if (prev.backend != next.backend) {
        Logger::info("Render config: backend change detected; restart required to take effect");
    }

    configWatcher_->setCurrent(next);
    sceneGPU_.uploadLights(scene_);
    if (needsReset) resetAccumulation();
}

void Renderer::onFramebufferResized(int width, int height) {
    viewportWidth_ = std::max(1, width);
    viewportHeight_ = std::max(1, height);
    glViewport(0, 0, viewportWidth_, viewportHeight_);
    if (useFragmentBackend_) {
        legacyPass_.resize(viewportWidth_, viewportHeight_);
    } else {
        pathTracer_.resize(viewportWidth_, viewportHeight_);
    }
    resetAccumulation();
}

bool Renderer::runWarmup() {
    if (options_.warmupFrames <= 0) return true;
    if (useFragmentBackend_ && !legacyPass_.ready()) return false;

    Logger::info("Running path tracer warm-up for " + std::to_string(options_.warmupFrames) +
                 " frame(s) before enabling interactive rendering");
    glfwSwapInterval(0);
    for (int i = 0; i < options_.warmupFrames; ++i) {
        const RenderConfig cfg = configWatcher_->current();
        sceneGPU_.bindAll();
        if (useFragmentBackend_) {
            legacyPass_.resize(viewportWidth_, viewportHeight_);
            int budget = (i == 0) ? 1 : cfg.pathTracerBounceBudget;
            RenderConfig warmCfg = cfg;
            warmCfg.pathTracerBounceBudget = budget;
            warmCfg.causticsEnabled = false;
            legacyPass_.render(scene_, warmCfg, debug_, totalTriangles_, true, 0, i);
        } else {
            pathTracer_.resize(viewportWidth_, viewportHeight_);
            int budget = (i == 0) ? 1 : cfg.pathTracerBounceBudget;
            RenderConfig warmCfg = cfg;
            warmCfg.pathTracerBounceBudget = budget;
            warmCfg.causticsEnabled = false;
            if (!pathTracer_.dispatch(scene_.camera, warmCfg, debug_, totalTriangles_,
                                      static_cast<int>(scene_.lights.size()), true, 0, i)) {
                Logger::error("Compute warm-up dispatch failed");
                return false;
            }
            resolvePass_.render(pathTracer_.accumulationTexture(),
                                viewportWidth_, viewportHeight_, scene_, warmCfg, debug_);
        }
        // Avoid unbounded stalls on some WSL D3D12 stacks during first-frame JIT.
        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (fence) {
            constexpr GLuint64 kWarmupWaitNs = 250000000ULL; // 250ms max per warmup frame
            const GLenum waitResult = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, kWarmupWaitNs);
            if (waitResult == GL_TIMEOUT_EXPIRED) {
                Logger::info("Warm-up frame exceeded 250ms wait budget; continuing without blocking");
            }
            glDeleteSync(fence);
        } else {
            glFlush();
        }
        glfwSwapBuffers(window_);
        glfwPollEvents();
    }
    glfwSwapInterval(requestedSwapInterval_);
    Logger::info("Warm-up complete; first on-screen frame should reuse the cached pipeline");
    resetAccumulation();
    lastFrameTime_ = static_cast<float>(glfwGetTime());
    return true;
}

bool Renderer::run() {
    if (!runWarmup()) return false;
    lastFrameTime_ = static_cast<float>(glfwGetTime());
    constexpr int benchmarkWarmupFrames = 10;
    double benchmarkTotalMs = 0.0;
    int benchmarkMeasuredFrames = 0;

    while (!glfwWindowShouldClose(window_)) {
        if (options_.keepWarm) {
            const WarmCommand command = warmWindow_.poll();
            if (command == WarmCommand::Error) return false;
            if (command == WarmCommand::Quit) break;
            if (command == WarmCommand::Restore) {
                glfwShowWindow(window_);
                glfwFocusWindow(window_);
                warmWindowHidden_ = false;
                lastFrameTime_ = static_cast<float>(glfwGetTime());
                Logger::info("Warm window restored.");
            }
        }
        if (warmWindowHidden_) {
            glfwWaitEventsTimeout(0.1);
            lastFrameTime_ = static_cast<float>(glfwGetTime());
            continue;
        }
        auto frameStart = std::chrono::steady_clock::now();
        pickupAsyncPathTracer();

        const float currentFrame = static_cast<float>(glfwGetTime());
        deltaTime_ = currentFrame - lastFrameTime_;
        lastFrameTime_ = currentFrame;
        if (frameCounter_ > 0 && deltaTime_ > 0.0f) {
            smoothedFrameTime_ = smoothedFrameTime_ == 0.0f ? deltaTime_ :
                                 0.1f * deltaTime_ + 0.9f * smoothedFrameTime_;
        }

        // Hot-reload config.
        RenderConfig reloaded = configWatcher_->current();
        if (configWatcher_->poll(currentFrame, reloaded)) {
            applyConfig(reloaded);
            Logger::info("Reloaded render config from " + configWatcher_->path().string());
        }

        // Input + camera.
        FrameInputResult input = input_.update(window_, deltaTime_, viewportWidth_, viewportHeight_,
                                                scene_.camera, debug_, scene_);
        RenderConfig edited = configWatcher_->current();
        if (settingsPanel_.update(window_, edited, scene_)) applyConfig(edited);
        if (settingsPanel_.consumeSaveRequested() &&
            !saveMaterialSettings(projectRoot_ / "config" / "materials.ini", scene_)) {
            Logger::error("Material settings were not saved");
        }
        scene_.camera.updateViewMatrix();
        scene_.camera.aspectRatio = float(viewportWidth_) / float(std::max(1, viewportHeight_));
        scene_.camera.updateProjectionMatrix();

        if (input.requestEditorToggle) {
            const bool ptReady = useFragmentBackend_ ? legacyPass_.ready() : pathTracer_.ready();
            const bool newEditorMode = !debug_.editorMode;
            if (!newEditorMode && !ptReady) {
                if (awaitingFragmentReady_) {
                    debug_.userLockedEditorMode = !debug_.userLockedEditorMode;
                    Logger::info(debug_.userLockedEditorMode
                        ? "Staying in editor mode after shader compilation"
                        : "Path tracer will activate when shader compilation finishes");
                } else {
                    Logger::info("Path tracer shader not ready yet; staying in editor mode");
                    debug_.userLockedEditorMode = true;
                }
            } else {
                if (!newEditorMode && !useFragmentBackend_ && debug_.editorMode) {
                    if (options_.preferFragmentToggleFromEditor) {
                        Logger::info("Switching F1 toggle to fragment path tracer fallback for stability");
                        pendingComputeActivation_ = false;
                        pendingJitActivation_ = false;
                        debug_.userLockedEditorMode = false;

                        if (!legacyPass_.ready() && !awaitingFragmentReady_) {
                            if (!options_.disableAsyncFragmentCompile && launchAsyncFragmentCompile()) {
                                awaitingFragmentReady_ = true;
                                useFragmentBackend_ = true;
                                Logger::info("Fragment path tracer compiling asynchronously; staying in editor mode until ready");
                            } else {
                                Logger::info("Compiling fragment path tracer on primary context");
                                if (legacyPass_.initialize(resolveProjectPath("shaders/vertex_shader.glsl"),
                                                           resolveProjectPath("shaders/fragment_shader.glsl"))) {
                                    legacyPass_.resize(viewportWidth_, viewportHeight_);
                                    legacyAdopted_ = true;
                                    useFragmentBackend_ = true;
                                    debug_.editorMode = false;
                                    glfwSwapInterval(requestedSwapInterval_);
                                    resetAccumulation();
                                    Logger::info("Fragment path tracer ready; editor raster mode: Off");
                                } else {
                                    Logger::error("Failed to initialize fragment path tracer fallback; staying in editor mode");
                                    debug_.userLockedEditorMode = true;
                                }
                            }
                        } else if (legacyPass_.ready()) {
                            useFragmentBackend_ = true;
                            debug_.editorMode = false;
                            glfwSwapInterval(requestedSwapInterval_);
                            resetAccumulation();
                            Logger::info("Fragment path tracer ready; editor raster mode: Off");
                        } else {
                            useFragmentBackend_ = true;
                            awaitingFragmentReady_ = true;
                            Logger::info("Fragment path tracer still compiling; staying in editor mode until ready");
                        }
                    } else {
                        if (pendingJitActivation_) {
                            pendingJitActivation_ = false;
                            debug_.userLockedEditorMode = true;
                            Logger::info("Staying in editor mode after compute precompilation");
                        } else if (!computeJitReady_.load(std::memory_order_acquire)) {
                            pendingJitActivation_ = true;
                            debug_.userLockedEditorMode = false;
                            Logger::info("Path tracer will activate when compute precompilation finishes");
                        } else {
                            pendingComputeActivation_ = false;
                            debug_.editorMode = false;
                            debug_.userLockedEditorMode = false;
                            glfwSwapInterval(requestedSwapInterval_);
                            resetAccumulation();
                            Logger::info("Editor raster mode: Off");
                        }
                    }
                } else {
                    debug_.editorMode = newEditorMode;
                    debug_.userLockedEditorMode = debug_.editorMode;
                    Logger::info(std::string("Editor raster mode: ") + (debug_.editorMode ? "On" : "Off"));
                    if (!debug_.editorMode) {
                        glfwSwapInterval(requestedSwapInterval_);
                        resetAccumulation();
                    }
                }
            }
        }
        if (input.requestSwapToggle) {
            requestedSwapInterval_ = (requestedSwapInterval_ == 0) ? 1 : 0;
            glfwSwapInterval(requestedSwapInterval_);
            Logger::info("Swap interval set to " + std::to_string(requestedSwapInterval_));
        }

        if (input.cameraMoved ||
            !vectorsNearlyEqual(scene_.camera.position, accumulationCameraPos_) ||
            !matricesNearlyEqual(scene_.camera.viewMatrix, accumulationView_) ||
            !matricesNearlyEqual(scene_.camera.projectionMatrix, accumulationProj_)) {
            resetAccumulation();
        }

        // Update dynamic geometry SSBOs if anything changed.
        const SceneUpdate sceneUpdate = sceneGPU_.updateDynamic(scene_);
        if (sceneUpdate == SceneUpdate::Error) {
            Logger::error("Scene update failed; stopping renderer");
            return false;
        }
        if (sceneUpdate != SceneUpdate::Unchanged) resetAccumulation();
        if (sceneUpdate == SceneUpdate::Scene) {
            totalTriangles_ = sceneGPU_.totalTriangles();
            rasterPass_.buildSceneGeometry(scene_);
        }

        if (pendingJitActivation_ && computeJitReady_.load(std::memory_order_acquire)) {
            pendingJitActivation_ = false;
            debug_.editorMode = false;
            debug_.userLockedEditorMode = false;
            glfwSwapInterval(requestedSwapInterval_);
            resetAccumulation();
            Logger::info(std::string("Compute shader precompile complete. ") +
                         shadingModeName(configWatcher_->current().shadingMode) + " active.");
        }

        if (!renderFrame()) return false;
        const float fps = smoothedFrameTime_ > 0.0f ? 1.0f / smoothedFrameTime_ : 0.0f;
        settingsPanel_.render(viewportWidth_, viewportHeight_, configWatcher_->current(), scene_, fps, debug_.editorMode);
        if (options_.benchmarkFrames > 0) glFinish();
        glfwSwapBuffers(window_);
        glfwPollEvents();

        if (!maybeActivateComputePathTracer()) return false;

        accumulationDirty_ = false;
        if (configWatcher_->current().accumulationEnabled) ++accumulatedFrames_;
        else accumulatedFrames_ = 0;
        accumulationView_ = scene_.camera.viewMatrix;
        accumulationProj_ = scene_.camera.projectionMatrix;
        accumulationCameraPos_ = scene_.camera.position;

        ++frameCounter_;
        auto frameEnd = std::chrono::steady_clock::now();
        if (options_.benchmarkFrames > 0 && frameCounter_ > benchmarkWarmupFrames) {
            benchmarkTotalMs += std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();
            if (++benchmarkMeasuredFrames == options_.benchmarkFrames) {
                const double averageMs = benchmarkTotalMs / benchmarkMeasuredFrames;
                Logger::info("Synchronized benchmark: " + std::to_string(benchmarkMeasuredFrames) +
                             " frames, " + formatMs(averageMs) + " ms/frame, " +
                             formatMs(1000.0 / averageMs) + " FPS");
                return true;
            }
        }
        if (frameCounter_ <= 100) {
            double totalMs = std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();
            Logger::info("Frame " + std::to_string(frameCounter_ - 1) + " timings: total=" + formatMs(totalMs) + " ms");
        }
        if (Logger::getLevel() <= LogLevel::DEBUG) {
            static double lastFpsPrint = 0.0;
            if (currentFrame - lastFpsPrint >= 0.25 && deltaTime_ > 0.0f) {
                std::cout << "\rFPS: " << (1.0 / deltaTime_) << "    " << std::flush;
                lastFpsPrint = currentFrame;
            }
        }
    }
    return true;
}

bool Renderer::maybeActivateComputePathTracer() {
    if (!pendingComputeActivation_ || useFragmentBackend_ || !pathTracer_.ready()) return true;

    // Perform one lightweight warm-up dispatch while the raster frame remains visible.
    RenderConfig warmCfg = configWatcher_->current();
    warmCfg.causticsEnabled = false;
    if (warmCfg.pathTracerBounceBudget > 1) warmCfg.pathTracerBounceBudget = 1;

    pathTracer_.resize(viewportWidth_, viewportHeight_);
    sceneGPU_.bindAll();
    if (!pathTracer_.dispatch(scene_.camera, warmCfg, debug_, totalTriangles_,
                              static_cast<int>(scene_.lights.size()), true, 0, 0)) {
        Logger::error("Compute warm-up dispatch failed");
        pendingComputeActivation_ = false;
        return false;
    }
    glFlush();

    pendingComputeActivation_ = false;
    debug_.editorMode = false;
    debug_.userLockedEditorMode = false;
    glfwSwapInterval(requestedSwapInterval_);
    resetAccumulation();
    Logger::info("Compute path tracer warm-up complete; editor raster mode: Off");
    return true;
}

bool Renderer::renderFrame() {
    const RenderConfig& cfg = configWatcher_->current();
    const bool ptReady = useFragmentBackend_ ? legacyPass_.ready() : pathTracer_.ready();
    if (debug_.editorMode || !ptReady) {
        rasterPass_.render(scene_, cfg);
        return true;
    }

    // Some GL-on-D3D12 paths can lose SSBO base bindings across state transitions.
    // Rebinding per-frame keeps compute/fragment path tracer resources deterministic.
    sceneGPU_.bindAll();

    RenderConfig dispatchCfg = cfg;
    if (!useFragmentBackend_ && cfg.shadingMode == ShadingMode::PathTracing && !computeStartupBudgetComplete_) {
        if (!cfg.accumulationEnabled || cfg.pathTracerBounceBudget <= 1) {
            computeStartupBudgetComplete_ = true;
        } else if (accumulatedFrames_ < 2) {
            dispatchCfg.pathTracerBounceBudget = 1;
            if (!loggedComputeStartupBudgetHint_) {
                Logger::info("Compute startup: using bounce budget 1 for first frames; restoring configured budget automatically");
                loggedComputeStartupBudgetHint_ = true;
            }
        } else {
            computeStartupBudgetComplete_ = true;
            resetAccumulation();
        }
    }
    const bool resetThisFrame = accumulationDirty_ || !cfg.accumulationEnabled;
    const int frameIndex = cfg.accumulationEnabled ? accumulatedFrames_ : frameCounter_;

    if (useFragmentBackend_) {
        if (cfg.causticsEnabled) {
            Logger::error("Caustics require the compute backend");
            return false;
        }
        sceneGPU_.bindAll();
        bool fragmentResetThisFrame = resetThisFrame;
        int fragmentAccumulatedFrames = accumulatedFrames_;
        if (options_.preferFragmentToggleFromEditor) {
            // WSL D3D12 fragment fallback can exhibit unstable imageLoad accumulation
            // behavior; force per-frame reset for stable, visible output.
            fragmentResetThisFrame = true;
            fragmentAccumulatedFrames = 0;
        }
        legacyPass_.resize(viewportWidth_, viewportHeight_);
        legacyPass_.render(scene_, cfg, debug_, totalTriangles_, fragmentResetThisFrame,
                           fragmentAccumulatedFrames, frameIndex);
    } else {
        sceneGPU_.bindAll();
        if (cfg.causticsEnabled) {
            if (!causticPass_.ready() && !causticPass_.initialize(resolveProjectPath("shaders/caustics.comp"))) {
                Logger::error("Failed to initialize caustics");
                return false;
            }
            if (!causticPass_.render(scene_, dispatchCfg, viewportWidth_, viewportHeight_, frameIndex)) {
                Logger::error("Failed to render caustics");
                return false;
            }
        }
        pathTracer_.resize(viewportWidth_, viewportHeight_);
        if (!pathTracer_.dispatch(scene_.camera, dispatchCfg, debug_, totalTriangles_,
                                  static_cast<int>(scene_.lights.size()),
                                  resetThisFrame, accumulatedFrames_, frameIndex)) {
            Logger::error("Compute shading dispatch failed");
            return false;
        }
        resolvePass_.render(pathTracer_.accumulationTexture(),
                            viewportWidth_, viewportHeight_, scene_, dispatchCfg, debug_);
    }
    return true;
}

} // namespace rayzen
