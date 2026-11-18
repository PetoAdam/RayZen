#ifndef RAYZEN_RENDERER_RENDER_CONFIG_H
#define RAYZEN_RENDERER_RENDER_CONFIG_H

#include <filesystem>
#include <string>

namespace rayzen {

enum class RendererBackend {
    Compute,
    Fragment,
};

enum class ShadingMode {
    PathTracing,
    RayTracing,
    Lambertian,
};

struct RenderConfig {
    RendererBackend backend = RendererBackend::Compute;
    ShadingMode shadingMode       = ShadingMode::PathTracing;
    int  pathTracerBounceBudget   = 5;
    int  swapInterval             = 0;
    float displayExposure         = 1.0f;
    float pathTracerAmbient       = 0.02f;
    float rasterAmbient           = 0.03f;
    float pointLightPower         = 300.0f;
    float directionalLightPower   = 2.0f;
    bool  accumulationEnabled     = true;
    bool  subsurfaceEnabled       = false;
    bool  causticsEnabled         = false;
    int   causticPhotonCount      = 65536;
};

// Polls a key=value config file on disk and reloads it when its mtime changes.
// Owns the canonical RenderConfig snapshot. Threading: not thread-safe; call from main thread.
class RenderConfigWatcher {
public:
    explicit RenderConfigWatcher(std::filesystem::path path);

    // Reads the file (creating a default if missing). Always sets current() to a valid config.
    void loadInitial();

    // Polls the config file at most once every ~250 ms. If the file was modified
    // and parses successfully, copies the new config into out and returns true.
    // The caller commits the accepted config using setCurrent().
    bool poll(double currentSeconds, RenderConfig& out);

    const RenderConfig& current() const noexcept { return current_; }
    void setCurrent(const RenderConfig& c) noexcept { current_ = c; }
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    bool readFromFile(RenderConfig& out, std::string& errorOut) const;
    static void writeDefaultIfMissing(const std::filesystem::path& path);

    std::filesystem::path path_;
    RenderConfig current_;
    std::filesystem::file_time_type lastWrite_{};
    bool lastWriteValid_ = false;
    double nextPollSeconds_ = 0.0;
};

} // namespace rayzen

#endif
