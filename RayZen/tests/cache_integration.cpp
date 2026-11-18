#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <png.h>
#include <glm/gtc/matrix_transform.hpp>

#include "Renderer/GLResources.h"
#include "Renderer/CausticPass.h"
#include "Renderer/LegacyFragmentPass.h"
#include "Renderer/MaterialSettings.h"
#include "Renderer/PathTracerPass.h"
#include "Renderer/RenderConfig.h"
#include "Renderer/SceneGPU.h"
#include "Renderer/RasterPass.h"

namespace fs = std::filesystem;

namespace {

bool checkGlassShadows(const fs::path& dir) {
    Scene scene;
    auto receiver = std::make_shared<Mesh>();
    Triangle a{}, b{};
    a.v0 = {-4,-4,0}; a.v1 = {4,-4,0}; a.v2 = {4,4,0};
    b.v0 = {-4,-4,0}; b.v1 = {4,4,0}; b.v2 = {-4,4,0};
    receiver->triangles = {a, b};
    auto box = std::make_shared<Mesh>();
    const auto face = [&](glm::vec3 p, glm::vec3 q, glm::vec3 r, glm::vec3 s) {
        Triangle first{}, second{};
        first.v0 = p; first.v1 = q; first.v2 = r;
        second.v0 = p; second.v1 = r; second.v2 = s;
        first.materialIndex = second.materialIndex = 1;
        first.hasUV = second.hasUV = 1;
        first.uv0 = first.uv1 = first.uv2 = glm::vec2(0.5f);
        second.uv0 = second.uv1 = second.uv2 = glm::vec2(0.5f);
        box->triangles.push_back(first);
        box->triangles.push_back(second);
    };
    face({-1,-1,1}, {1,-1,1}, {1,1,1}, {-1,1,1});
    face({1,-1,-1}, {-1,-1,-1}, {-1,1,-1}, {1,1,-1});
    face({1,-1,1}, {1,-1,-1}, {1,1,-1}, {1,1,1});
    face({-1,-1,-1}, {-1,-1,1}, {-1,1,1}, {-1,1,-1});
    face({-1,1,1}, {1,1,1}, {1,1,-1}, {-1,1,-1});
    face({-1,-1,-1}, {1,-1,-1}, {1,-1,1}, {-1,-1,1});
    scene.gameObjects = {{receiver, glm::mat4(1)}};
    scene.materials.emplace_back(glm::vec3(1), 0.0f, 1.0f);
    scene.materials.emplace_back(glm::vec3(1), 0.0f, 0.05f, 0.0f, 1.0f, 1.5f);
    scene.camera = Camera(glm::vec3(0,0,3), glm::vec3(0,0,-1), glm::vec3(0,1,0),
                          45.0f, 1.0f, 0.1f, 100.0f);
    scene.lights.emplace_back(glm::vec4(0.6f,0,0.8f,0), glm::vec3(1), 1.0f);
    TextureAsset tint;
    if (!tint.setRGBA("shadow tint", 1, 1, {255,128,255,255})) return false;
    scene.textures.push_back(tint);
    rayzen::SceneGPU gpu;
    rayzen::PathTracerPass tracer;
    rayzen::LegacyFragmentPass legacy;
    if (!gpu.initialize(scene, true) ||
        !tracer.initialize((dir / "path_trace.comp").string()) ||
        !legacy.initialize((dir / "legacy.vert").string(), (dir / "legacy.frag").string())) return false;
    gpu.bindAll();
    tracer.resize(128, 128);
    legacy.resize(128, 128);
    glViewport(0, 0, 128, 128);
    rayzen::RenderConfig settings;
    settings.shadingMode = rayzen::ShadingMode::Lambertian;
    settings.pathTracerBounceBudget = 1;
    settings.pathTracerAmbient = 0.0f;
    const auto measure = [&](const char* label) {
        tracer.dispatch(scene.camera, settings, {}, gpu.totalTriangles(), 1, true, 0, 0);
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
        glBindTexture(GL_TEXTURE_2D, tracer.accumulationTexture());
        std::vector<float> pixels(128 * 128 * 4);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, pixels.data());
        const size_t center = (64 * 128 + 64) * 4;
        const glm::vec3 linear(pixels[center], pixels[center + 1], pixels[center + 2]);
        legacy.render(scene, settings, {}, gpu.totalTriangles(), true, 0, 0);
        glm::vec3 displayed;
        glReadPixels(64, 64, 1, 1, GL_RGB, GL_FLOAT, &displayed[0]);
        const float peak = std::max(linear.r, std::max(linear.g, linear.b));
        glm::vec3 expected(0);
        if (peak > 1e-6f) {
            const float mapped = std::clamp((peak * (2.51f * peak + 0.03f)) /
                                           (peak * (2.43f * peak + 0.59f) + 0.14f), 0.0f, 1.0f);
            expected = glm::pow(glm::clamp(linear * (mapped / peak), 0.0f, 1.0f), glm::vec3(1.0f / 2.2f));
        }
        if (glm::length(displayed - expected) > 0.035f || glGetError() != GL_NO_ERROR) {
            std::cerr << "Glass shadow backend mismatch for " << label << ": "
                      << displayed.r << "," << displayed.g << "," << displayed.b << " vs "
                      << expected.r << "," << expected.g << "," << expected.b << "\n";
            return glm::vec3(-1);
        }
        return linear;
    };
    const auto update = [&]() { return gpu.updateDynamic(scene) != rayzen::SceneUpdate::Error; };
    const auto boxTransform = [](float depth) {
        return glm::scale(glm::translate(glm::mat4(1), glm::vec3(0.75f,0,1)),
                          glm::vec3(0.62f,0.6f,depth));
    };
    const glm::vec3 unblocked = measure("unblocked");
    scene.gameObjects.push_back({box, boxTransform(0.4f)});
    if (!update()) return false;
    const glm::vec3 clear = measure("clear");
    scene.materials[1].albedo = glm::vec3(0.25f,1,1);
    if (!update()) return false;
    const glm::vec3 colored = measure("colored");
    scene.materials[1].ior = 1.0f;
    if (!update()) return false;
    const glm::vec3 noFresnel = measure("IOR one");
    scene.materials[1].ior = 1.5f;
    scene.materials[1].transparency = 0.5f;
    if (!update()) return false;
    const glm::vec3 partial = measure("partial");
    scene.materials[1].transparency = 1.0f;
    scene.gameObjects[1].transform = boxTransform(0.8f);
    if (!update()) return false;
    const glm::vec3 thick = measure("thick");
    if (unblocked.r < 0.2f || clear.r / unblocked.r < 0.90f || clear.r / unblocked.r > 0.94f ||
        std::abs(colored.r / clear.r - 0.25f) > 0.02f ||
        std::abs(noFresnel.g / unblocked.g - 1.0f) > 0.02f ||
        std::abs(partial.g / colored.g - 0.5f) > 0.02f ||
        std::abs(thick.r / colored.r - 0.25f) > 0.03f ||
        std::abs(thick.g / colored.g - 1.0f) > 0.02f) {
        std::cerr << "Glass Fresnel, color, transparency or thickness shadow failed\n";
        return false;
    }
    scene.gameObjects[1].transform = boxTransform(0.4f);
    scene.materials[1].albedo = glm::vec3(1);
    scene.materials[1].ior = 1.0f;
    scene.materials[1].transparency = 0.01f;
    if (!update()) return false;
    const glm::vec3 faint = measure("low transparency");
    if (std::abs(faint.r / unblocked.r - 0.01f) > 0.002f) {
        std::cerr << "Low glass transmission was discarded\n";
        return false;
    }
    scene.materials[1].transparency = 1.0f;
    scene.materials[1].albedo = glm::vec3(0.25f,1,1);
    scene.gameObjects.push_back({box, glm::scale(glm::translate(glm::mat4(1), glm::vec3(0.93f,0,1.24f)),
                                                glm::vec3(0.62f,0.6f,0.4f))});
    if (!update()) return false;
    const glm::vec3 overlapping = measure("overlapping glass");
    if (std::abs(overlapping.r / unblocked.r - 0.0625f) > 0.01f ||
        std::abs(overlapping.g / unblocked.g - 1.0f) > 0.02f) {
        std::cerr << "Overlapping glass absorption failed\n";
        return false;
    }
    scene.gameObjects.pop_back();
    scene.materials[1].albedo = glm::vec3(1);
    scene.materials[1].baseColorTexture = 0;
    if (!update()) return false;
    const glm::vec3 textured = measure("textured");
    if (std::abs(textured.g / textured.r - 0.216f) > 0.02f) {
        std::cerr << "Glass shadow did not sample its base-color texture\n";
        return false;
    }
    scene.materials[1].baseColorTexture = -1;
    scene.materials[1].transparency = 0.0f;
    if (!update()) return false;
    if (glm::length(measure("opaque")) > 0.001f) return false;
    scene.lights[0].positionOrDirection = glm::vec4(0.18f,0,0.24f,1);
    if (!update()) return false;
    const glm::vec3 beforeBlocker = measure("point light before blocker");
    scene.gameObjects.pop_back();
    if (!update()) return false;
    const glm::vec3 pointUnblocked = measure("unblocked point light");
    if (glm::length(beforeBlocker - pointUnblocked) > 0.01f) {
        std::cerr << "Blocker behind point light cast a shadow\n";
        return false;
    }
    scene.lights[0].positionOrDirection = glm::vec4(0.75f,0,1.0f,1);
    scene.materials[1].transparency = 0.25f;
    scene.gameObjects.push_back({box, boxTransform(0.4f)});
    if (!update()) return false;
    const glm::vec3 pointInside = measure("point light inside glass");
    scene.gameObjects.pop_back();
    if (!update()) return false;
    const glm::vec3 pointOutside = measure("unblocked inside point light");
    if (std::abs(pointInside.r / pointOutside.r - 0.5f) > 0.03f) {
        std::cerr << "Shadow attenuation to point light inside glass failed\n";
        return false;
    }
    scene.lights[0].positionOrDirection = glm::vec4(0.6f,0,0.8f,0);
    scene.materials[1] = Material(glm::vec3(1), 0.0f, 0.05f, 0.0f, 1.0f, 1.0f);
    scene.gameObjects.push_back({box, boxTransform(0.4f)});
    if (!update()) return false;
    rayzen::CausticPass caustics;
    if (!caustics.initialize((dir / "caustics.comp").string())) return false;
    const auto photons = [&](int frame) {
        gpu.bindAll();
        if (!caustics.render(scene, settings, 128, 128, frame)) return glm::vec3(-1);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, caustics.buffer());
        std::vector<unsigned int> values(128 * 128 * 4);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, values.size() * sizeof(unsigned int), values.data());
        double total = 0.0, xsum = 0.0, green = 0.0;
        for (int y = 0; y < 128; ++y) for (int x = 0; x < 128; ++x) {
            const size_t p = (y * 128 + x) * 4;
            total += values[p];
            green += values[p + 1];
            xsum += x * double(values[p]);
        }
        return total > 0 ? glm::vec3(total / 1048576.0, xsum / total, green / total) : glm::vec3(0);
    };
    const glm::vec3 unbent = photons(0);
    settings.causticPhotonCount = 16385;
    const glm::vec3 fewerPhotons = photons(0);
    if (unbent.r <= 0.0f || std::abs(fewerPhotons.r / unbent.r - 1.0f) > 0.15f) {
        std::cerr << "Caustic photon budget changed the light energy\n";
        return false;
    }
    settings.causticPhotonCount = 65536;
    if (photons(0).r <= 0.0f) return false;
    const auto renderCausticImage = [&](bool enabled) {
        settings.causticsEnabled = enabled;
        tracer.dispatch(scene.camera, settings, {}, gpu.totalTriangles(), 1, true, 0, 0);
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
        glBindTexture(GL_TEXTURE_2D, tracer.accumulationTexture());
        std::vector<float> pixels(128 * 128 * 4);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, pixels.data());
        return pixels;
    };
    const auto photonImage = renderCausticImage(true);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, caustics.buffer());
    const GLuint zero = 0;
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    const auto blockedImage = renderCausticImage(true);
    const auto straightImage = renderCausticImage(false);
    double photonEnergy = 0.0, straightEnergy = 0.0;
    for (size_t p = 0; p < photonImage.size(); p += 4) {
        photonEnergy += photonImage[p] - blockedImage[p];
        straightEnergy += straightImage[p] - blockedImage[p];
    }
    if (straightEnergy <= 1.0 || photonEnergy / straightEnergy < 0.75 ||
        photonEnergy / straightEnergy > 1.25) {
        std::cerr << "Unit-IOR photon energy differs from direct transmission: "
                  << photonEnergy << " vs " << straightEnergy << "\n";
        return false;
    }
    scene.materials[1].ior = 1.5f;
    if (!update()) return false;
    const glm::vec3 refracted = photons(0);
    scene.materials[1].albedo = glm::vec3(1,0.1f,0.1f);
    if (!update()) return false;
    const glm::vec3 tinted = photons(0);
    if (unbent.r <= 1.0f || refracted.r <= 1.0f ||
        refracted.g < unbent.g + 1.0f || tinted.b > 0.2f) {
        std::cerr << "Refracted photon caustics failed: straight=" << unbent.r << "," << unbent.g
                  << " refracted=" << refracted.r << "," << refracted.g
                  << " tint=" << tinted.b << "\n";
        return false;
    }
    scene.materials[1].albedo = glm::vec3(1);
    if (!update()) return false;
    if (photons(0).r <= 1.0f) return false;
    settings.causticsEnabled = true;
    tracer.dispatch(scene.camera, settings, {}, gpu.totalTriangles(), 1, true, 0, 0);
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    glBindTexture(GL_TEXTURE_2D, tracer.accumulationTexture());
    std::vector<float> withCaustics(128 * 128 * 4);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, withCaustics.data());
    settings.causticsEnabled = false;
    tracer.dispatch(scene.camera, settings, {}, gpu.totalTriangles(), 1, true, 0, 0);
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    std::vector<float> withoutCaustics(128 * 128 * 4);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, withoutCaustics.data());
    float addedLight = 0.0f;
    for (size_t p = 0; p < withCaustics.size(); p += 4)
        addedLight += std::max(withCaustics[p] - withoutCaustics[p], 0.0f);
    if (addedLight < 1.0f || glGetError() != GL_NO_ERROR) {
        std::cerr << "Photon caustics did not reach the rendered image\n";
        return false;
    }
    scene.lights[0].positionOrDirection = glm::vec4(2.25f,0,3.0f,1);
    scene.lights[0].power = 10.0f;
    if (!update()) return false;
    const glm::vec3 pointPhotons = photons(0);
    scene.lights[0].power = 20.0f;
    if (!update()) return false;
    const glm::vec3 brighterPhotons = photons(0);
    if (pointPhotons.r <= 1.0f ||
        std::abs(brighterPhotons.r / pointPhotons.r - 2.0f) > 0.02f) {
        std::cerr << "Point light caustics did not follow light power\n";
        return false;
    }
    scene.lights[0].positionOrDirection = glm::vec4(0.6f,0,0.8f,0);
    auto occluder = std::make_shared<Mesh>();
    a.v0 = {-20,-20,3}; a.v1 = {20,-20,3}; a.v2 = {20,20,3};
    b.v0 = {-20,-20,3}; b.v1 = {20,20,3}; b.v2 = {-20,20,3};
    occluder->triangles = {a, b};
    scene.gameObjects.push_back({occluder, glm::mat4(1)});
    if (!update()) return false;
    if (photons(0).r != 0.0f) {
        std::cerr << "Occluder between directional light and glass did not block caustics\n";
        return false;
    }
    return true;
}

bool runChecks(const fs::path& dir) {
    {
        png_image image{};
        image.version = PNG_IMAGE_VERSION;
        image.width = image.height = 2;
        image.format = PNG_FORMAT_RGBA;
        const unsigned char pixels[] = {255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255};
        const fs::path path = dir / "texture with spaces.png";
        if (!png_image_write_to_file(&image, path.string().c_str(), 0, pixels, 0, nullptr)) return false;
        TextureAsset texture;
        if (!texture.loadPNG(path.string()) || texture.width() != 2 || texture.height() != 2 ||
            texture.rgba()[0] != 0 || texture.rgba()[2] != 255 || texture.rgba()[8] != 255) {
            std::cerr << "PNG decoding or UV orientation failed\n";
            return false;
        }
        if (texture.loadPNG((dir / "missing.png").string())) return false;
        Scene source;
        source.textures = {TextureAsset::checker(), texture};
        source.materials = {Material(glm::vec3(0.8f, 0.4f, 0.2f), 0.0f, 0.5f, 0.0f, 0.0f, 1.3f, 0.35f),
                            Material(glm::vec3(1), 0.0f, 0.5f)};
        source.materials[0].baseColorTexture = 1;
        source.materials[1].baseColorTexture = 0;
        const fs::path materialPath = dir / "materials.ini";
        if (!rayzen::saveMaterialSettings(materialPath, source)) return false;
        source.materials[0].baseColorTexture = 99;
        if (rayzen::saveMaterialSettings(materialPath, source)) return false;
        Scene restored;
        restored.textures = {TextureAsset::checker()};
        restored.materials = {Material(glm::vec3(1), 0.0f, 1.0f), Material(glm::vec3(1), 0.0f, 1.0f)};
        if (!rayzen::loadMaterialSettings(materialPath, restored) ||
            restored.textures.size() != 2 || restored.materials[0].baseColorTexture != 1 ||
            restored.materials[1].baseColorTexture != 0 ||
            std::abs(restored.materials[0].subsurfaceMeanFreePath - 0.35f) > 1e-6f ||
            restored.textures[1].sourcePath() != texture.sourcePath()) {
            std::cerr << "Material settings did not round-trip\n";
            return false;
        }
        std::ofstream(materialPath) << "0 1 1 1 0 1 0 0 1.5 0 \"@none\"\n1 invalid\n";
        if (rayzen::loadMaterialSettings(materialPath, restored) ||
            restored.materials[0].baseColorTexture != 1) {
            std::cerr << "Invalid material settings were partially applied\n";
            return false;
        }
    }
    const fs::path configPath = dir / "render.ini";
    std::ofstream(configPath) << "path_tracer_bounce_budget=100\n";
    rayzen::RenderConfigWatcher config(configPath);
    config.loadInitial();
    if (config.current().pathTracerBounceBudget != 100) return false;
    std::ofstream(configPath) << "path_tracer_bounce_budget=2\nshading_mode=lambertian\n"
                                 "subsurface_enabled=true\ncaustics_enabled=true\n";
    fs::last_write_time(configPath, fs::file_time_type::clock::now() + std::chrono::seconds(2));
    rayzen::RenderConfig reloaded;
    if (!config.poll(1.0, reloaded) || config.current().pathTracerBounceBudget != 100 ||
        reloaded.pathTracerBounceBudget != 2 || reloaded.shadingMode != rayzen::ShadingMode::Lambertian ||
        !reloaded.subsurfaceEnabled || !reloaded.causticsEnabled) return false;
    config.setCurrent(reloaded);
    std::ofstream(configPath) << "caustic_photon_count=16385\n";
    fs::last_write_time(configPath, fs::file_time_type::clock::now() + std::chrono::seconds(3));
    if (!config.poll(1.5, reloaded) || reloaded.causticPhotonCount != 16385 ||
        reloaded.pathTracerBounceBudget != 5) return false;
    config.setCurrent(reloaded);
    const char* invalidCounts[] = {"1023", "262145", "65536junk"};
    for (int i = 0; i < 3; ++i) {
        std::ofstream(configPath) << "caustic_photon_count=" << invalidCounts[i] << '\n';
        fs::last_write_time(configPath, fs::file_time_type::clock::now() + std::chrono::seconds(4 + i));
        if (config.poll(2.0 + i, reloaded) || config.current().causticPhotonCount != 16385) {
            std::cerr << "Invalid photon budget accepted\n";
            return false;
        }
    }
    std::ofstream(configPath) << "shading_mode=invalid\n";
    fs::last_write_time(configPath, fs::file_time_type::clock::now() + std::chrono::seconds(7));
    if (config.poll(5.0, reloaded) || config.current().causticPhotonCount != 16385) return false;
    const char* invalidNumbers[] = {
        "display_exposure=nan", "path_tracer_ambient=inf", "point_light_power=2junk",
        "path_tracer_bounce_budget=5junk", "swap_interval=1junk"
    };
    for (int i = 0; i < 5; ++i) {
        std::ofstream(configPath) << invalidNumbers[i] << '\n';
        fs::last_write_time(configPath, fs::file_time_type::clock::now() + std::chrono::seconds(8 + i));
        if (config.poll(6.0 + i, reloaded) || config.current().causticPhotonCount != 16385) {
            std::cerr << "Invalid numeric render setting accepted: " << invalidNumbers[i] << "\n";
            return false;
        }
    }

    {
        rayzen::GLTexture texture;
        if (!texture.ensureRGBA32F(4, 4)) return false;
        std::vector<float> pixels(4 * 4 * 4, 1.0f);
        glBindTexture(GL_TEXTURE_2D, texture.id());
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA, GL_FLOAT, pixels.data());
        texture.clearToZero();
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, pixels.data());
        for (float value : pixels) if (value != 0.0f) return false;
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    const fs::path vertex = dir / "test.vert";
    const fs::path fragment = dir / "test.frag";
    std::ofstream(vertex) << "#version 430 core\nvoid main(){gl_Position=vec4(0.0);}\n";
    std::ofstream(fragment) << "#version 430 core\nout vec4 color; void main(){color=vec4(1.0);}\n";

    GLint formats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
    const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const bool binarySupported = formats > 0 && vendor && renderer &&
        std::string(vendor).find("Microsoft") == std::string::npos &&
        std::string(renderer).find("D3D12") == std::string::npos;
    if (binarySupported) {
        rayzen::GLProgram::setCacheMode(rayzen::ShaderCacheMode::Prepare);
        rayzen::GLProgram compiled;
        if (!compiled.loadGraphics(vertex.string(), fragment.string())) return false;
        rayzen::GLProgram::setCacheMode(rayzen::ShaderCacheMode::LoadOnly);
        rayzen::GLProgram loaded;
        if (!loaded.loadGraphics(vertex.string(), fragment.string())) return false;
        std::ofstream(fragment, std::ios::app) << "// changed\n";
        if (loaded.loadGraphics(vertex.string(), fragment.string())) return false;
        rayzen::GLProgram::setCacheMode(rayzen::ShaderCacheMode::Prepare);
        if (!compiled.loadGraphics(vertex.string(), fragment.string())) return false;
        const auto cache = dir / "cache" / "test.vert_test.frag_v2.rzbin";
        std::ofstream(cache, std::ios::binary | std::ios::trunc) << "broken";
        rayzen::GLProgram::setCacheMode(rayzen::ShaderCacheMode::LoadOnly);
        if (loaded.loadGraphics(vertex.string(), fragment.string())) return false;

        const fs::path compute = dir / "test.comp";
        std::ofstream(compute) << "#version 430 core\nlayout(local_size_x=1) in; void main(){}\n";
        rayzen::GLProgram::setCacheMode(rayzen::ShaderCacheMode::Prepare);
        if (!compiled.loadCompute(compute.string())) return false;
        rayzen::GLProgram::setCacheMode(rayzen::ShaderCacheMode::LoadOnly);
        if (!loaded.loadCompute(compute.string())) return false;
        std::ofstream(compute, std::ios::app) << "// changed\n";
        if (loaded.loadCompute(compute.string())) return false;

        const fs::path include = dir / "shared.glsl";
        const fs::path nested = dir / "nested.glsl";
        std::ofstream(nested) << "float value() { return 1.0; }\n";
        std::ofstream(include) << "#include \"nested.glsl\"\n";
        std::ofstream(compute) << "#version 430 core\n#include \"shared.glsl\"\n"
                                 "layout(local_size_x=1) in; void main(){float v=value();}\n";
        rayzen::GLProgram::setCacheMode(rayzen::ShaderCacheMode::Prepare);
        if (!compiled.loadCompute(compute.string())) return false;
        rayzen::GLProgram::setCacheMode(rayzen::ShaderCacheMode::LoadOnly);
        if (!loaded.loadCompute(compute.string())) return false;
        std::ofstream(nested, std::ios::app) << "// changed include\n";
        if (loaded.loadCompute(compute.string())) return false;
        std::ofstream(nested) << "#include \"shared.glsl\"\n";
        rayzen::GLProgram::setCacheMode(rayzen::ShaderCacheMode::Automatic);
        if (compiled.loadCompute(compute.string())) return false;
        std::ofstream(nested) << "#include \"missing.glsl\"\n";
        if (compiled.loadCompute(compute.string())) return false;
    } else {
        std::cout << "Shader binary checks skipped: unsupported by this renderer (formats="
                  << formats << ")\n";
    }

    Scene scene;
    auto mesh = std::make_shared<Mesh>();
    Triangle tri{};
    tri.v0 = {0, 0, 0};
    tri.v1 = {1, 0, 0};
    tri.v2 = {0, 1, 0};
    tri.materialIndex = 0;
    mesh->triangles.push_back(tri);
    scene.gameObjects.push_back({mesh, glm::mat4(1.0f)});
    scene.materials.emplace_back(glm::vec3(1.0f), 0.0f, 0.5f);

    {
        rayzen::SceneGPU prepared;
        if (!prepared.initialize(scene, true, false, true)) return false;
    }
    {
        rayzen::SceneGPU cached;
        if (!cached.initialize(scene, false, true) ||
            cached.updateDynamic(scene) != rayzen::SceneUpdate::Unchanged) return false;
        cached.bindAll();
        GLint blasBuffer = 0;
        glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 7, &blasBuffer);
        BVHNode originalBlas{};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, blasBuffer);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(originalBlas), &originalBlas);
        scene.gameObjects[0].transform = glm::translate(glm::mat4(1.0f), glm::vec3(2, 0, 0));
        if (cached.updateDynamic(scene) != rayzen::SceneUpdate::Transforms) return false;
        BVHNode movedBlas{}, movedTlas{};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, blasBuffer);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(movedBlas), &movedBlas);
        GLint tlasBuffer = 0;
        glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 5, &tlasBuffer);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, tlasBuffer);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(movedTlas), &movedTlas);
        if (movedBlas.boundsMin.x != originalBlas.boundsMin.x ||
            movedBlas.boundsMax.x != originalBlas.boundsMax.x ||
            movedTlas.boundsMin.x != 2.0f || movedTlas.boundsMax.x != 3.0f) return false;
        scene.gameObjects.push_back({mesh, glm::mat4(1.0f)});
        if (cached.updateDynamic(scene) != rayzen::SceneUpdate::Scene || cached.totalTriangles() != 2) return false;
        mesh->triangles[0].v1.x = 2.0f;
        if (cached.updateDynamic(scene) != rayzen::SceneUpdate::Scene) return false;
        scene.gameObjects.clear();
        if (cached.updateDynamic(scene) != rayzen::SceneUpdate::Scene || cached.totalTriangles() != 0) return false;
        scene.gameObjects.push_back({mesh, glm::mat4(1.0f)});
        if (cached.updateDynamic(scene) != rayzen::SceneUpdate::Scene || cached.totalTriangles() != 1) return false;
        scene.gameObjects[0].transform = glm::scale(glm::mat4(1.0f), glm::vec3(0.0f));
        if (cached.updateDynamic(scene) != rayzen::SceneUpdate::Error) return false;
        if (glGetError() != GL_NO_ERROR) return false;
    }
    mesh->triangles[0].v1.x = 1.0f;
    scene.gameObjects[0].transform = glm::mat4(1.0f);
    {
        Scene moving;
        moving.materials = scene.materials;
        for (int i = 0; i < 80; ++i) {
            moving.gameObjects.push_back({mesh, glm::translate(glm::mat4(1.0f),
                glm::vec3(std::ldexp(1.0f, i), 0.0f, 0.0f))});
        }
        rayzen::SceneGPU gpu;
        if (!gpu.initialize(moving, true, false, false, false)) return false;
        gpu.bindAll();
        GLint tlasBuffer = 0, oldBytes = 0, newBytes = 0;
        glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 5, &tlasBuffer);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, tlasBuffer);
        glGetBufferParameteriv(GL_SHADER_STORAGE_BUFFER, GL_BUFFER_SIZE, &oldBytes);
        for (size_t i = 0; i < moving.gameObjects.size(); ++i)
            moving.gameObjects[i].transform = glm::translate(glm::mat4(1.0f), glm::vec3(float(i) * 2.0f, 0, 0));
        if (gpu.updateDynamic(moving) != rayzen::SceneUpdate::Transforms) return false;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, tlasBuffer);
        glGetBufferParameteriv(GL_SHADER_STORAGE_BUFFER, GL_BUFFER_SIZE, &newBytes);
        if (glGetError() != GL_NO_ERROR || newBytes <= oldBytes) {
            std::cerr << "TLAS buffer did not grow for the moved scene\n";
            return false;
        }
    }
    {
        rayzen::SceneGPU preserved;
        if (!preserved.initialize(scene, false, true)) return false;
    }
    scene.gameObjects[0].transform = glm::translate(glm::mat4(1.0f), glm::vec3(2, 0, 0));
    {
        rayzen::SceneGPU stale;
        if (stale.initialize(scene, false, true)) return false;
    }
    scene.gameObjects[0].transform = glm::mat4(1.0f);
    mesh->triangles[0].v1.x = 2.0f;
    {
        rayzen::SceneGPU stale;
        if (stale.initialize(scene, false, true)) return false;
    }
    mesh->triangles[0].v1.x = 1.0f;
    mesh->triangles[0].uv0.x = 0.5f;
    {
        rayzen::SceneGPU stale;
        if (stale.initialize(scene, false, true)) return false;
    }
    mesh->triangles[0].uv0.x = 0.0f;
    std::ofstream(dir / "bvh_cache/v4/ssbo_v4_tlasnodes.bin",
                  std::ios::binary | std::ios::trunc) << "broken";
    {
        rayzen::SceneGPU corrupt;
        if (corrupt.initialize(scene, false, true)) return false;
    }
    {
        rayzen::SceneGPU rebuilt;
        if (!rebuilt.initialize(scene, false)) return false;
    }
    {
        rayzen::SceneGPU cached;
        if (!cached.initialize(scene, false, true)) return false;
    }

    scene.gameObjects.clear();
    mesh->triangles[0].v0 = {-2, -2, 0};
    mesh->triangles[0].v1 = {2, -2, 0};
    mesh->triangles[0].v2 = {0, 2, 0};
    scene.gameObjects.push_back({mesh, glm::mat4(1.0f)});
    scene.materials.clear();
    scene.materials.emplace_back(glm::vec3(1, 0, 0), 0.0f, 1.0f);
    scene.camera = Camera(glm::vec3(0, 0, 3), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0),
                          45.0f, 1.0f, 0.1f, 100.0f);
    fs::copy_file(fs::path(RAYZEN_SOURCE_DIR) / "shaders/path_trace.comp", dir / "path_trace.comp");
    fs::copy_file(fs::path(RAYZEN_SOURCE_DIR) / "shaders/realtime_ray.comp", dir / "realtime_ray.comp");
    fs::copy_file(fs::path(RAYZEN_SOURCE_DIR) / "shaders/lambertian.comp", dir / "lambertian.comp");
    fs::copy_file(fs::path(RAYZEN_SOURCE_DIR) / "shaders/direct_lighting.glsl", dir / "direct_lighting.glsl");
    fs::copy_file(fs::path(RAYZEN_SOURCE_DIR) / "shaders/path_trace_shared.glsl", dir / "path_trace_shared.glsl");
    fs::copy_file(fs::path(RAYZEN_SOURCE_DIR) / "shaders/caustics.comp", dir / "caustics.comp");
    fs::copy_file(fs::path(RAYZEN_SOURCE_DIR) / "shaders/material_sampling.glsl", dir / "material_sampling.glsl");
    fs::copy_file(fs::path(RAYZEN_SOURCE_DIR) / "shaders/pbr.glsl", dir / "pbr.glsl");
    fs::copy_file(fs::path(RAYZEN_SOURCE_DIR) / "shaders/ray_tracing.glsl", dir / "ray_tracing.glsl");
    if (binarySupported) {
        const char* modes[] = {"path_trace.comp", "realtime_ray.comp", "lambertian.comp"};
        rayzen::GLProgram::setCacheMode(rayzen::ShaderCacheMode::Prepare);
        for (const char* mode : modes) {
            rayzen::GLProgram program;
            if (!program.loadCompute((dir / mode).string())) return false;
        }
        rayzen::GLProgram::setCacheMode(rayzen::ShaderCacheMode::LoadOnly);
        for (const char* mode : modes) {
            rayzen::GLProgram program;
            if (!program.loadCompute((dir / mode).string())) return false;
        }
    }
    rayzen::GLProgram::setCacheMode(rayzen::ShaderCacheMode::Automatic);
    {
        const fs::path optics = dir / "optics.comp";
        std::ofstream(optics) << R"(#version 430 core
layout(local_size_x=1) in;
struct Material { vec3 albedo; float metallic; float roughness; float reflectivity; float transparency; float ior; float subsurfaceMeanFreePath; int baseColorTexture; };
struct Light { vec4 positionOrDirection; vec3 color; float power; };
struct Ray { vec3 origin; vec3 direction; };
layout(std430, binding=2) buffer Lights { Light lights[]; };
layout(std430, binding=3) buffer Materials { Material materials[]; };
layout(std430, binding=0) buffer Results { vec4 values[]; };
uniform int numLights;
layout(binding=1) uniform sampler2DArray materialTextures;
const float PI=3.14159265359, MIN_ROUGHNESS=0.05, RAY_EPSILON=0.002;
float rand(vec2 v) { return 0.5; }
vec3 randomHemisphereDirection(vec3 n, vec2 s) { return n; }
vec3 offsetRayOrigin(vec3 p, vec3 n, vec3 d) { return p; }
bool traverseTLAS(Ray r, out float t, out vec3 p, out vec3 n, out int m, out int i) { return false; }
bool traverseTLAS(Ray r, out float t, out vec3 p, out vec3 n, out int m, out int i, out vec2 uv) {
    if (r.origin.z >= 1.0 || r.direction.z <= 0.0) return false;
    t = (1.0 - r.origin.z) / r.direction.z;
    p = r.origin + t * r.direction;
    n = vec3(0,0,1); m = 0; i = 0; uv = vec2(-1e30);
    return true;
}
#include "material_sampling.glsl"
void main() {
    vec3 dir;
    bool transmitted = sampleDielectric(normalize(vec3(0.8660254, 0, -0.5)), vec3(0,0,1), 1.5, 1.0, 0.99, dir);
    values[0] = vec4(dielectricFresnel(1.0,1.0,1.5), dielectricFresnel(0.5,1.5,1.0),
                     dielectricFresnel(1.0,1.0,1.0), transmitted ? 1.0 : 0.0);
    values[1] = vec4(dir, 0.0);
    vec3 inside, throughExit;
    shadowVisibility(vec3(0), vec3(0,0,1), 0.5, inside);
    shadowVisibility(vec3(0), vec3(0,0,1), 2.0, throughExit);
    values[2] = vec4(inside, 0.0);
    values[3] = vec4(throughExit, 0.0);
    Material plastic = Material(vec3(0.8, 0.2, 0.1), 0.0, 0.5, 0.0, 0.0, 1.5, 0.0, -1);
    values[4] = vec4(materialF0(plastic), 0.0);
    plastic.reflectivity = 1.0;
    values[5] = vec4(materialF0(plastic), 0.0);
    plastic.ior = 1.0;
    values[6] = vec4(materialF0(plastic), 0.0);
    plastic.metallic = 1.0;
    values[7] = vec4(materialF0(plastic), 0.0);
}
)";
        rayzen::GLProgram program;
        if (!program.loadCompute(optics.string())) return false;
        GLuint buffer = 0;
        glGenBuffers(1, &buffer);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
        glBufferData(GL_SHADER_STORAGE_BUFFER, 32 * sizeof(float), nullptr, GL_DYNAMIC_READ);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffer);
        GLuint materialBuffer = 0;
        const Material medium(glm::vec3(0.25f, 1, 1), 0.0f, 0.05f, 0.0f, 1.0f, 1.0f);
        glGenBuffers(1, &materialBuffer);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, materialBuffer);
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(medium), &medium, GL_STATIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, materialBuffer);
        program.use();
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
        float values[32]{};
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(values), values);
        glDeleteBuffers(1, &buffer);
        glDeleteBuffers(1, &materialBuffer);
        if (std::abs(values[0] - 0.04f) > 0.001f || values[1] != 1.0f ||
            values[2] != 0.0f || values[3] != 0.0f || values[6] < 0.49f ||
            std::abs(values[8] - 0.5f) > 0.001f || std::abs(values[9] - 1.0f) > 0.001f ||
            std::abs(values[12] - 0.25f) > 0.001f || std::abs(values[13] - 1.0f) > 0.001f ||
            std::abs(values[16] - 0.04f) > 0.001f || std::abs(values[20] - 0.08f) > 0.001f ||
            std::abs(values[24]) > 0.001f || std::abs(values[28] - 0.8f) > 0.001f ||
            std::abs(values[29] - 0.2f) > 0.001f) {
            std::cerr << "Dielectric Fresnel, total internal reflection or inside-medium shadows failed\n";
            return false;
        }
    }
    {
        rayzen::SceneGPU gpu;
        rayzen::PathTracerPass tracer;
        if (!gpu.initialize(scene, true) || !tracer.initialize((dir / "path_trace.comp").string())) return false;
        gpu.bindAll();
        tracer.resize(32, 32);
        rayzen::RenderConfig settings;
        settings.pathTracerAmbient = 0.5f;
        settings.pathTracerBounceBudget = 1;
        const auto centerPixel = [&](int frame = 0) {
            if (!tracer.dispatch(scene.camera, settings, {}, gpu.totalTriangles(),
                                 static_cast<int>(scene.lights.size()), true, 0, frame))
                return glm::vec3(-1);
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            glBindTexture(GL_TEXTURE_2D, tracer.accumulationTexture());
            std::vector<float> pixels(32 * 32 * 4);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, pixels.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            const size_t center = (16 * 32 + 16) * 4;
            return glm::vec3(pixels[center], pixels[center + 1], pixels[center + 2]);
        };
        const glm::vec3 original = centerPixel();
        const GLuint pathProgram = tracer.programId();
        if (original.r < 0.45f || original.g > 0.01f || original.b > 0.01f) return false;
        scene.materials[0].reflectivity = 1.0f;
        scene.lights.emplace_back(glm::vec4(0.8660254f, 0, 0.5f, 0), glm::vec3(1), 1.0f);
        settings.pathTracerAmbient = 0.0f;
        if (gpu.updateDynamic(scene) != rayzen::SceneUpdate::Transforms) return false;
        const glm::vec3 specular = centerPixel();
        if (specular.g < 0.003f || specular.g > 0.008f || specular.r < 0.13f) {
            std::cerr << "PBR dielectric response unexpected: " << specular.r << "," << specular.g << "\n";
            return false;
        }
        scene.materials[0].transparency = 1.0f;
        scene.materials[0].roughness = 0.1f;
        scene.materials[0].ior = 1.5f;
        scene.lights[0].positionOrDirection = glm::vec4(0, 0, 1, 0);
        if (gpu.updateDynamic(scene) != rayzen::SceneUpdate::Transforms) return false;
        const glm::vec3 glassHighlight = centerPixel();
        if (glassHighlight.g < 0.1f || std::abs(glassHighlight.r - glassHighlight.g) > 0.01f) {
            std::cerr << "Glass GGX highlight is missing or tinted\n";
            return false;
        }
        scene.materials[0].transparency = 0.0f;
        scene.materials[0].roughness = 1.0f;
        scene.lights[0].positionOrDirection = glm::vec4(0.8660254f, 0, 0.5f, 0);
        if (gpu.updateDynamic(scene) != rayzen::SceneUpdate::Transforms) return false;
        settings.shadingMode = rayzen::ShadingMode::Lambertian;
        const glm::vec3 diffuse = centerPixel();
        if (tracer.programId() == pathProgram) {
            std::cerr << "Lambertian mode did not select its own program\n";
            return false;
        }
        if (diffuse.r < 0.155f || diffuse.r > 0.165f || diffuse.g > 0.001f) return false;
        {
            const fs::path vertexPath = dir / "legacy.vert";
            const fs::path fragmentPath = dir / "legacy.frag";
            fs::copy_file(fs::path(RAYZEN_SOURCE_DIR) / "shaders/vertex_shader.glsl", vertexPath);
            fs::copy_file(fs::path(RAYZEN_SOURCE_DIR) / "shaders/fragment_shader.glsl", fragmentPath);
            rayzen::LegacyFragmentPass legacy;
            if (!legacy.initialize(vertexPath.string(), fragmentPath.string())) return false;
            GLFWwindow* window = glfwGetCurrentContext();
            int width = 0, height = 0;
            glfwGetFramebufferSize(window, &width, &height);
            if (width < 64 || height < 64) {
                std::cerr << "Fragment test framebuffer too small: " << width << "x" << height << "\n";
                return false;
            }
            glViewport(0, 0, width, height);
            legacy.resize(width, height);
            const auto legacyPixel = [&]() {
                legacy.render(scene, settings, {}, gpu.totalTriangles(), true, 0, 0);
                glm::vec3 pixel;
                glReadPixels(width / 2, height / 2, 1, 1, GL_RGB, GL_FLOAT, &pixel[0]);
                return pixel;
            };
            const glm::vec3 legacyDiffuse = legacyPixel();
            settings.shadingMode = rayzen::ShadingMode::RayTracing;
            const glm::vec3 legacySpecular = legacyPixel();
            if (legacyDiffuse.r < 0.3f || legacyDiffuse.g > 0.02f ||
                legacySpecular.g < 0.03f || legacySpecular.r < legacySpecular.g ||
                glGetError() != GL_NO_ERROR) {
                std::cerr << "Fragment mode pixels: diffuse " << legacyDiffuse.r << "," << legacyDiffuse.g
                          << " specular " << legacySpecular.r << "," << legacySpecular.g << "\n";
                return false;
            }
        }
        settings.shadingMode = rayzen::ShadingMode::PathTracing;
        scene.materials[0].reflectivity = 0.0f;
        scene.lights.clear();
        settings.pathTracerAmbient = 0.5f;
        scene.gameObjects[0].transform = glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0));
        if (gpu.updateDynamic(scene) != rayzen::SceneUpdate::Transforms) return false;
        const glm::vec3 moved = centerPixel();
        if (tracer.programId() != pathProgram) {
            std::cerr << "Path tracing program was not reused after a mode switch\n";
            return false;
        }
        if (!std::isfinite(moved.r) || moved.b < 0.3f || moved.g < 0.2f) return false;
        scene.gameObjects.clear();
        if (gpu.updateDynamic(scene) != rayzen::SceneUpdate::Scene) return false;
        const glm::vec3 empty = centerPixel();
        if (glm::length(empty - moved) > 0.001f || glGetError() != GL_NO_ERROR) return false;

        auto mirror = std::make_shared<Mesh>();
        auto target = std::make_shared<Mesh>();
        Triangle m1{}, m2{}, t1{}, t2{};
        m1.v0 = {-1, -1, 1}; m1.v1 = {1, -1, -1}; m1.v2 = {1, 1, -1}; m1.materialIndex = 0;
        m2.v0 = {-1, -1, 1}; m2.v1 = {1, 1, -1}; m2.v2 = {-1, 1, 1}; m2.materialIndex = 0;
        t1.v0 = {2, -2, -2}; t1.v1 = {2, -2, 2}; t1.v2 = {2, 2, 2}; t1.materialIndex = 1;
        t2.v0 = {2, -2, -2}; t2.v1 = {2, 2, 2}; t2.v2 = {2, 2, -2}; t2.materialIndex = 1;
        mirror->triangles = {m1, m2};
        target->triangles = {t1, t2};
        scene.gameObjects = {{mirror, glm::mat4(1.0f)}, {target, glm::mat4(1.0f)}};
        scene.materials.clear();
        scene.materials.emplace_back(glm::vec3(1), 1.0f, 0.05f, 1.0f);
        scene.materials.emplace_back(glm::vec3(0.8f, 0, 0), 0.0f, 1.0f);
        scene.lights.emplace_back(glm::vec4(-0.5f, 0.8660254f, 0, 0), glm::vec3(1), 1.0f);
        settings.pathTracerAmbient = 0.0f;
        if (gpu.updateDynamic(scene) != rayzen::SceneUpdate::Scene) return false;
        const glm::vec3 oneBounce = centerPixel();
        if (glm::length(oneBounce) > 0.001f) return false;
        settings.pathTracerBounceBudget = 2;
        settings.shadingMode = rayzen::ShadingMode::RayTracing;
        const glm::vec3 reflected = centerPixel();
        if (tracer.programId() == pathProgram) return false;
        if (reflected.r < 0.03f || reflected.r < reflected.g * 2.0f) {
            std::cerr << "Real-time ray tracer failed to reflect the red target\n";
            return false;
        }
        {
            rayzen::LegacyFragmentPass legacy;
            if (!legacy.initialize((dir / "legacy.vert").string(), (dir / "legacy.frag").string())) return false;
            glViewport(0, 0, 128, 128);
            legacy.resize(128, 128);
            legacy.render(scene, settings, {}, gpu.totalTriangles(), true, 0, 0);
            glm::vec3 pixel;
            glReadPixels(64, 64, 1, 1, GL_RGB, GL_FLOAT, &pixel[0]);
            if (pixel.r < 0.1f || pixel.r < pixel.g * 2.0f) {
                std::cerr << "Fragment real-time ray tracer failed to reflect the red target\n";
                return false;
            }
        }
        settings.shadingMode = rayzen::ShadingMode::PathTracing;
        float brightest = 0.0f;
        for (int frame = 0; frame < 16; ++frame) brightest = std::max(brightest, centerPixel(frame).r);
        if (brightest < 0.03f || glGetError() != GL_NO_ERROR) return false;

        auto redMesh = std::make_shared<Mesh>();
        auto blueMesh = std::make_shared<Mesh>();
        Triangle colored{};
        colored.v0 = {-0.55f, -0.55f, 0};
        colored.v1 = {0.55f, -0.55f, 0};
        colored.v2 = {0, 0.55f, 0};
        colored.materialIndex = 3;
        redMesh->triangles.push_back(colored);
        colored.materialIndex = 4;
        blueMesh->triangles.push_back(colored);
        scene.gameObjects = {
            {redMesh, glm::translate(glm::mat4(1), glm::vec3(-0.65f, 0, 0))},
            {blueMesh, glm::translate(glm::mat4(1), glm::vec3(0.65f, 0, 0))}
        };
        scene.materials.clear();
        for (int i = 0; i < 3; ++i) scene.materials.emplace_back(glm::vec3(0), 0.0f, 1.0f);
        scene.materials.emplace_back(glm::vec3(1, 0, 0), 0.0f, 1.0f);
        scene.materials.emplace_back(glm::vec3(0, 0, 1), 0.0f, 0.25f);
        scene.lights.clear();
        settings.pathTracerAmbient = 0.5f;
        settings.pathTracerBounceBudget = 1;
        if (gpu.updateDynamic(scene) != rayzen::SceneUpdate::Scene) return false;
        tracer.dispatch(scene.camera, settings, {}, gpu.totalTriangles(), 0, true, 0, 0);
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
        glBindTexture(GL_TEXTURE_2D, tracer.accumulationTexture());
        std::vector<float> materialPixels(32 * 32 * 4);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, materialPixels.data());
        const size_t left = (16 * 32 + 8) * 4;
        const size_t right = (16 * 32 + 24) * 4;
        if (materialPixels[left] < 0.45f || materialPixels[left + 2] > 0.01f ||
            materialPixels[right + 2] < 0.45f || materialPixels[right] > 0.01f) {
            std::cerr << "Distinct instance materials did not render correctly\n";
            return false;
        }
        settings.pathTracerBounceBudget = 3;
        glm::vec3 blueAverage(0.0f);
        for (int frame = 0; frame < 16; ++frame) {
            tracer.dispatch(scene.camera, settings, {}, gpu.totalTriangles(), 0, true, 0, frame);
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, materialPixels.data());
            blueAverage += glm::vec3(materialPixels[right], materialPixels[right + 1], materialPixels[right + 2]);
        }
        blueAverage /= 16.0f;
        if (blueAverage.b < 0.45f || blueAverage.b < blueAverage.r + 0.2f) {
            std::cerr << "Multi-bounce blue material lost its color: "
                      << blueAverage.r << "," << blueAverage.g << "," << blueAverage.b << "\n";
            return false;
        }

        auto slab = std::make_shared<Mesh>();
        const auto makeSlab = [&](float back) {
            Triangle frontA{}, frontB{}, backA{}, backB{};
            frontA.v0 = {-10,-10,1}; frontA.v1 = {10,-10,1}; frontA.v2 = {10,10,1};
            frontB.v0 = {-10,-10,1}; frontB.v1 = {10,10,1}; frontB.v2 = {-10,10,1};
            backA.v0 = {-10,-10,back}; backA.v1 = {10,10,back}; backA.v2 = {10,-10,back};
            backB.v0 = {-10,-10,back}; backB.v1 = {-10,10,back}; backB.v2 = {10,10,back};
            slab->triangles = {frontA, frontB, backA, backB};
            const auto side = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d) {
                Triangle first{}, second{};
                first.v0 = a; first.v1 = b; first.v2 = c;
                second.v0 = a; second.v1 = c; second.v2 = d;
                slab->triangles.push_back(first);
                slab->triangles.push_back(second);
            };
            side({-10,-10,back}, {-10,10,back}, {-10,10,1}, {-10,-10,1});
            side({10,-10,back}, {10,-10,1}, {10,10,1}, {10,10,back});
            side({-10,10,back}, {10,10,back}, {10,10,1}, {-10,10,1});
            side({-10,-10,back}, {-10,-10,1}, {10,-10,1}, {10,-10,back});
        };
        makeSlab(0.0f);
        scene.gameObjects = {{slab, glm::mat4(1)}};
        scene.materials.clear();
        scene.materials.emplace_back(glm::vec3(0.25f, 1, 1), 0.0f, 0.05f, 0.0f, 1.0f, 1.0f);
        scene.lights.clear();
        settings.pathTracerAmbient = 0.0f;
        settings.pathTracerBounceBudget = 8;
        settings.subsurfaceEnabled = false;
        if (gpu.updateDynamic(scene) != rayzen::SceneUpdate::Scene) return false;
        const glm::vec3 thinGlass = centerPixel();
        settings.shadingMode = rayzen::ShadingMode::RayTracing;
        const glm::vec3 realtimeGlass = centerPixel();
        if (realtimeGlass.b < 0.3f || realtimeGlass.r / realtimeGlass.b > 0.35f) {
            std::cerr << "Real-time glass did not refract and absorb the sky\n";
            return false;
        }
        {
            rayzen::LegacyFragmentPass legacy;
            if (!legacy.initialize((dir / "legacy.vert").string(), (dir / "legacy.frag").string())) return false;
            glViewport(0, 0, 128, 128);
            legacy.resize(128, 128);
            legacy.render(scene, settings, {}, gpu.totalTriangles(), true, 0, 0);
            glm::vec3 pixel;
            glReadPixels(64, 64, 1, 1, GL_RGB, GL_FLOAT, &pixel[0]);
            if (pixel.b < 0.3f || pixel.r >= pixel.b * 0.8f) {
                std::cerr << "Fragment real-time glass did not refract and absorb the sky\n";
                return false;
            }
        }
        settings.shadingMode = rayzen::ShadingMode::PathTracing;
        makeSlab(-1.0f);
        if (gpu.updateDynamic(scene) != rayzen::SceneUpdate::Scene) return false;
        const glm::vec3 thickGlass = centerPixel();
        if (thinGlass.r < 0.06f || thickGlass.r / thinGlass.r < 0.23f ||
            thickGlass.r / thinGlass.r > 0.27f || std::abs(thinGlass.b - thickGlass.b) > 0.01f) {
            std::cerr << "Glass thickness attenuation failed: " << thinGlass.r << " vs " << thickGlass.r << "\n";
            return false;
        }

        makeSlab(0.0f);
        scene.materials[0] = Material(glm::vec3(0.8f, 0.5f, 0.25f), 0.0f, 0.5f, 0.0f, 0.0f, 1.0f, 0.3f);
        scene.lights.emplace_back(glm::vec4(0, 0, -1, 0), glm::vec3(1), 10.0f);
        if (gpu.updateDynamic(scene) != rayzen::SceneUpdate::Scene) return false;
        settings.subsurfaceEnabled = true;
        glm::vec3 litScattering(0.0f);
        for (int frame = 0; frame < 32; ++frame) litScattering += centerPixel(frame);
        scene.lights.clear();
        if (gpu.updateDynamic(scene) != rayzen::SceneUpdate::Transforms) return false;
        glm::vec3 unlitScattering(0.0f);
        for (int frame = 0; frame < 32; ++frame) unlitScattering += centerPixel(frame);
        if (litScattering.r < unlitScattering.r + 1.0f || litScattering.r <= litScattering.b) {
            std::cerr << "Subsurface scattering did not respond to lighting and material color\n";
            return false;
        }
        settings.subsurfaceEnabled = false;
        settings.pathTracerAmbient = 0.5f;
        settings.pathTracerBounceBudget = 1;
        const glm::vec3 disabledScattering = centerPixel();
        if (std::abs(disabledScattering.r - 0.4f) > 0.01f) {
            std::cerr << "Disabled subsurface scattering changed opaque shading\n";
            return false;
        }
        {
            rayzen::LegacyFragmentPass legacy;
            if (!legacy.initialize((dir / "legacy.vert").string(), (dir / "legacy.frag").string())) return false;
            legacy.resize(128, 128);
            glViewport(0, 0, 128, 128);
            settings.subsurfaceEnabled = true;
            settings.pathTracerAmbient = 0.0f;
            settings.pathTracerBounceBudget = 8;
            const auto averageLegacy = [&]() {
                glm::vec3 sum(0.0f);
                for (int frame = 0; frame < 16; ++frame) {
                    legacy.render(scene, settings, {}, gpu.totalTriangles(), true, 0, frame);
                    glm::vec3 pixel;
                    glReadPixels(64, 64, 1, 1, GL_RGB, GL_FLOAT, &pixel[0]);
                    sum += pixel;
                }
                return sum;
            };
            const glm::vec3 legacyUnlit = averageLegacy();
            scene.lights.emplace_back(glm::vec4(0, 0, -1, 0), glm::vec3(1), 10.0f);
            if (gpu.updateDynamic(scene) != rayzen::SceneUpdate::Transforms) return false;
            const glm::vec3 legacyLit = averageLegacy();
            if (legacyLit.r < legacyUnlit.r + 0.5f) {
                std::cerr << "Fragment subsurface scattering did not respond to lighting\n";
                return false;
            }
        }
        scene.materials[0].subsurfaceMeanFreePath = -1.0f;
        if (gpu.updateDynamic(scene) != rayzen::SceneUpdate::Error) return false;

        auto textured = std::make_shared<Mesh>();
        Triangle a{}, b{};
        a.v0 = {-3,-3,0}; a.v1 = {3,-3,0}; a.v2 = {3,3,0};
        a.uv0 = {0,0}; a.uv1 = {1,0}; a.uv2 = {1,1}; a.hasUV = 1;
        b.v0 = {-3,-3,0}; b.v1 = {3,3,0}; b.v2 = {-3,3,0};
        b.uv0 = {0,0}; b.uv1 = {1,1}; b.uv2 = {0,1}; b.hasUV = 1;
        textured->triangles = {a, b};
        scene.gameObjects = {{textured, glm::mat4(1)}};
        scene.materials = {Material(glm::vec3(1), 0.0f, 1.0f)};
        TextureAsset testTexture;
        if (!testTexture.setRGBA("RED BLUE", 2, 1, {255,0,0,255, 0,0,255,255})) return false;
        scene.textures = {testTexture};
        scene.materials[0].baseColorTexture = 0;
        scene.lights.clear();
        settings.pathTracerBounceBudget = 1;
        settings.pathTracerAmbient = settings.rasterAmbient = 0.5f;
        if (gpu.updateDynamic(scene) != rayzen::SceneUpdate::Scene) return false;
        const auto computePair = [&]() {
            tracer.dispatch(scene.camera, settings, {}, gpu.totalTriangles(), 0, true, 0, 0);
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            glBindTexture(GL_TEXTURE_2D, tracer.accumulationTexture());
            std::vector<float> pixels(32 * 32 * 4);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, pixels.data());
            const size_t leftPixel = (16 * 32 + 8) * 4, rightPixel = (16 * 32 + 24) * 4;
            return std::pair<glm::vec3, glm::vec3>(
                {pixels[leftPixel], pixels[leftPixel+1], pixels[leftPixel+2]},
                {pixels[rightPixel], pixels[rightPixel+1], pixels[rightPixel+2]});
        };
        auto pair = computePair();
        if (pair.first.r < 0.45f || pair.first.b > 0.01f ||
            pair.second.b < 0.45f || pair.second.r > 0.01f) {
            std::cerr << "Compute base-color texture sampling failed\n";
            return false;
        }
        for (auto& tri : textured->triangles) {
            tri.uv0.x = 1.0f - tri.uv0.x;
            tri.uv1.x = 1.0f - tri.uv1.x;
            tri.uv2.x = 1.0f - tri.uv2.x;
        }
        if (gpu.updateDynamic(scene) != rayzen::SceneUpdate::Scene) return false;
        pair = computePair();
        if (pair.first.b < 0.45f || pair.second.r < 0.45f) {
            std::cerr << "Changed UVs did not invalidate scene geometry\n";
            return false;
        }
        if (!scene.textures[0].setRGBA("RED BLUE", 2, 1, {128,0,0,255, 0,0,255,255}) ||
            gpu.updateDynamic(scene) != rayzen::SceneUpdate::Transforms) return false;
        pair = computePair();
        if (pair.second.r < 0.105f || pair.second.r > 0.112f) {
            std::cerr << "Texture sRGB decoding or update failed: " << pair.second.r << "\n";
            return false;
        }
        if (!scene.textures[0].setRGBA("RED BLUE", 2, 1, {255,0,0,255, 0,0,255,255}) ||
            gpu.updateDynamic(scene) != rayzen::SceneUpdate::Transforms) return false;
        for (auto& tri : textured->triangles) tri.hasUV = 0;
        if (gpu.updateDynamic(scene) != rayzen::SceneUpdate::Scene) return false;
        pair = computePair();
        if (pair.first.r < 0.45f || pair.first.b < 0.45f) {
            std::cerr << "Missing UV fallback did not preserve material albedo\n";
            return false;
        }
        for (auto& tri : textured->triangles) tri.hasUV = 1;
        if (gpu.updateDynamic(scene) != rayzen::SceneUpdate::Scene) return false;
        scene.materials[0].baseColorTexture = -1;
        if (gpu.updateDynamic(scene) != rayzen::SceneUpdate::Transforms) return false;
        pair = computePair();
        if (pair.first.r < 0.45f || pair.first.b < 0.45f) return false;
        scene.materials[0].baseColorTexture = 0;
        if (gpu.updateDynamic(scene) != rayzen::SceneUpdate::Transforms) return false;
        {
            rayzen::LegacyFragmentPass legacy;
            if (!legacy.initialize((dir / "legacy.vert").string(), (dir / "legacy.frag").string())) return false;
            legacy.resize(128, 128);
            glViewport(0, 0, 128, 128);
            legacy.render(scene, settings, {}, gpu.totalTriangles(), true, 0, 0);
            glm::vec3 leftPixel, rightPixel;
            glReadPixels(32, 64, 1, 1, GL_RGB, GL_FLOAT, &leftPixel[0]);
            glReadPixels(96, 64, 1, 1, GL_RGB, GL_FLOAT, &rightPixel[0]);
            if (leftPixel.b < 0.3f || leftPixel.r > 0.03f ||
                rightPixel.r < 0.3f || rightPixel.b > 0.03f) {
                std::cerr << "Fragment base-color texture sampling failed\n";
                return false;
            }
        }
        {
            fs::copy_file(fs::path(RAYZEN_SOURCE_DIR) / "shaders/editor_vertex.glsl",
                          dir / "editor_vertex.glsl");
            fs::copy_file(fs::path(RAYZEN_SOURCE_DIR) / "shaders/editor_fragment.glsl",
                          dir / "editor_fragment.glsl");
            rayzen::RasterPass raster;
            if (!raster.initialize((dir / "editor_vertex.glsl").string(),
                                   (dir / "editor_fragment.glsl").string())) return false;
            raster.buildSceneGeometry(scene);
            gpu.bindAll();
            raster.render(scene, settings);
            glm::vec3 leftPixel, rightPixel;
            glReadPixels(32, 64, 1, 1, GL_RGB, GL_FLOAT, &leftPixel[0]);
            glReadPixels(96, 64, 1, 1, GL_RGB, GL_FLOAT, &rightPixel[0]);
            if (leftPixel.b < 0.3f || leftPixel.r > 0.03f ||
                rightPixel.r < 0.3f || rightPixel.b > 0.03f) {
                std::cerr << "Raster base-color texture sampling failed\n";
                return false;
            }
        }
        scene.materials[0].baseColorTexture = 8;
        if (gpu.updateDynamic(scene) != rayzen::SceneUpdate::Error) return false;
    }
    return checkGlassShadows(dir);
}

} // namespace

int main() {
    if (!glfwInit()) return 77;
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(128, 128, "cache test", nullptr, nullptr);
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
    const fs::path oldDir = fs::current_path();
    const fs::path dir = fs::temp_directory_path() / ("rayzen-cache-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(dir);
    fs::current_path(dir);
    const bool success = runChecks(dir);
    fs::current_path(oldDir);
    fs::remove_all(dir);
    glfwDestroyWindow(window);
    glfwTerminate();
    if (!success) std::cerr << "Cache integration check failed\n";
    return success ? 0 : 1;
}
