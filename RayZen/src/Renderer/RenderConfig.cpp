#include "Renderer/RenderConfig.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include "Logger.h"

namespace rayzen {

namespace {

std::string trim(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

bool parseBool(const std::string& value, bool& out) {
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "1" || lower == "true" || lower == "yes" || lower == "on") { out = true; return true; }
    if (lower == "0" || lower == "false" || lower == "no" || lower == "off") { out = false; return true; }
    return false;
}

bool parseBackend(const std::string& value, RendererBackend& out) {
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "compute") { out = RendererBackend::Compute; return true; }
    if (lower == "fragment" || lower == "legacy") { out = RendererBackend::Fragment; return true; }
    return false;
}

bool parseShadingMode(const std::string& value, ShadingMode& out) {
    if (value == "path_tracing") { out = ShadingMode::PathTracing; return true; }
    if (value == "ray_tracing") { out = ShadingMode::RayTracing; return true; }
    if (value == "lambertian") { out = ShadingMode::Lambertian; return true; }
    return false;
}

int parseInt(const std::string& value) {
    size_t parsed = 0;
    const int result = std::stoi(value, &parsed);
    if (parsed != value.size()) throw std::invalid_argument("trailing characters");
    return result;
}

float parseFloat(const std::string& value) {
    size_t parsed = 0;
    const float result = std::stof(value, &parsed);
    if (parsed != value.size() || !std::isfinite(result))
        throw std::invalid_argument("invalid finite number");
    return result;
}

const char* defaultText() {
    return
        "# RayZen render configuration\n"
        "# Edit while the app is running; the renderer hot-reloads this file.\n"
        "renderer_backend=compute\n"
        "shading_mode=path_tracing\n"
        "path_tracer_bounce_budget=5\n"
        "swap_interval=0\n"
        "display_exposure=1.0\n"
        "path_tracer_ambient=0.02\n"
        "raster_ambient=0.03\n"
        "point_light_power=300.0\n"
        "directional_light_power=2.0\n"
        "accumulation_enabled=true\n"
        "subsurface_enabled=false\n"
        "caustics_enabled=false\n"
        "caustic_photon_count=65536\n";
}

} // namespace

RenderConfigWatcher::RenderConfigWatcher(std::filesystem::path path)
    : path_(std::move(path)) {}

void RenderConfigWatcher::writeDefaultIfMissing(const std::filesystem::path& path) {
    if (std::filesystem::exists(path)) return;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (out) out << defaultText();
}

bool RenderConfigWatcher::readFromFile(RenderConfig& out, std::string& errorOut) const {
    std::ifstream input(path_);
    if (!input.is_open()) {
        errorOut = "Failed to open render config: " + path_.string();
        return false;
    }
    RenderConfig next;
    std::string line;
    int lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        const auto hashPos = line.find('#');
        if (hashPos != std::string::npos) line = line.substr(0, hashPos);
        line = trim(line);
        if (line.empty()) continue;
        const auto eqPos = line.find('=');
        if (eqPos == std::string::npos) {
            errorOut = "Invalid config line " + std::to_string(lineNumber) + ": missing '='";
            return false;
        }
        const std::string key = trim(line.substr(0, eqPos));
        const std::string value = trim(line.substr(eqPos + 1));
        try {
            if (key == "renderer_backend") {
                if (!parseBackend(value, next.backend)) {
                    errorOut = "Unknown renderer_backend value '" + value + "' (expected compute|fragment)";
                    return false;
                }
            } else if (key == "shading_mode") {
                if (!parseShadingMode(value, next.shadingMode)) {
                    errorOut = "Unknown shading_mode value '" + value +
                               "' (expected path_tracing|ray_tracing|lambertian)";
                    return false;
                }
            } else if (key == "path_tracer_bounce_budget") {
                const int budget = parseInt(value);
                if (budget < 1 || budget > 100) {
                    errorOut = "path_tracer_bounce_budget must be between 1 and 100";
                    return false;
                }
                next.pathTracerBounceBudget = budget;
            } else if (key == "swap_interval") {
                next.swapInterval = std::clamp(parseInt(value), 0, 2);
            } else if (key == "display_exposure") {
                next.displayExposure = std::clamp(parseFloat(value), 0.05f, 16.0f);
            } else if (key == "path_tracer_ambient") {
                next.pathTracerAmbient = std::clamp(parseFloat(value), 0.0f, 1.0f);
            } else if (key == "raster_ambient") {
                next.rasterAmbient = std::clamp(parseFloat(value), 0.0f, 1.0f);
            } else if (key == "point_light_power") {
                next.pointLightPower = std::max(0.0f, parseFloat(value));
            } else if (key == "directional_light_power") {
                next.directionalLightPower = std::max(0.0f, parseFloat(value));
            } else if (key == "accumulation_enabled") {
                bool parsed = false;
                if (!parseBool(value, parsed)) {
                    errorOut = "Invalid boolean for accumulation_enabled at line " + std::to_string(lineNumber);
                    return false;
                }
                next.accumulationEnabled = parsed;
            } else if (key == "subsurface_enabled") {
                if (!parseBool(value, next.subsurfaceEnabled)) {
                    errorOut = "Invalid boolean for subsurface_enabled at line " + std::to_string(lineNumber);
                    return false;
                }
            } else if (key == "caustics_enabled") {
                if (!parseBool(value, next.causticsEnabled)) {
                    errorOut = "Invalid boolean for caustics_enabled at line " + std::to_string(lineNumber);
                    return false;
                }
            } else if (key == "caustic_photon_count") {
                const int count = parseInt(value);
                if (count < 1024 || count > 262144) {
                    errorOut = "caustic_photon_count must be between 1024 and 262144";
                    return false;
                }
                next.causticPhotonCount = count;
            } else {
                errorOut = "Unknown config key at line " + std::to_string(lineNumber) + ": " + key;
                return false;
            }
        } catch (const std::invalid_argument&) {
            errorOut = "Invalid value for key '" + key + "' at line " + std::to_string(lineNumber);
            return false;
        } catch (const std::out_of_range&) {
            errorOut = "Invalid value for key '" + key + "' at line " + std::to_string(lineNumber);
            return false;
        }
    }
    if (!input.eof()) {
        errorOut = "Failed while reading render config: " + path_.string();
        return false;
    }
    out = next;
    return true;
}

void RenderConfigWatcher::loadInitial() {
    writeDefaultIfMissing(path_);
    std::string error;
    RenderConfig loaded;
    if (!readFromFile(loaded, error)) {
        Logger::error(error + "; using defaults");
        loaded = RenderConfig{};
    }
    current_ = loaded;
    std::error_code ec;
    lastWrite_ = std::filesystem::last_write_time(path_, ec);
    lastWriteValid_ = !ec;
}

bool RenderConfigWatcher::poll(double currentSeconds, RenderConfig& out) {
    if (currentSeconds < nextPollSeconds_ || path_.empty()) return false;
    nextPollSeconds_ = currentSeconds + 0.25;

    std::error_code ec;
    const auto timestamp = std::filesystem::last_write_time(path_, ec);
    if (ec) return false;
    if (lastWriteValid_ && timestamp == lastWrite_) return false;

    RenderConfig loaded;
    std::string error;
    if (!readFromFile(loaded, error)) {
        Logger::error(error);
        return false;
    }
    lastWrite_ = timestamp;
    lastWriteValid_ = true;
    out = loaded;
    return true;
}

} // namespace rayzen
