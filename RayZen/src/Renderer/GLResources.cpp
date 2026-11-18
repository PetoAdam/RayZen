#include "Renderer/GLResources.h"

#include <chrono>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>
#include <filesystem>

#include "Logger.h"

namespace rayzen {

namespace {

namespace fs = std::filesystem;

struct ShaderBinaryMetadata {
    uint64_t magic = 0x5241595a454e0001ull;
    uint64_t sourceHash = 0;
    uint64_t driverHash = 0;
    uint64_t binaryHash = 0;
    uint32_t binaryFormat = 0;
    uint32_t binaryLength = 0;
};

ShaderCacheMode cacheMode = ShaderCacheMode::Automatic;

uint64_t hashText(uint64_t hash, const std::string& value) {
    for (unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    hash ^= 0xff;
    return hash * 1099511628211ull;
}

uint64_t driverFingerprint() {
    uint64_t hash = 1469598103934665603ull;
    for (GLenum name : {GL_VENDOR, GL_RENDERER, GL_VERSION, GL_SHADING_LANGUAGE_VERSION}) {
        const auto* text = glGetString(name);
        hash = hashText(hash, text ? reinterpret_cast<const char*>(text) : "");
    }
    return hash;
}

bool programBinariesSupported() {
    GLint count = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &count);
    const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    return count > 0 && !(vendor && std::string(vendor).find("Microsoft") != std::string::npos) &&
           !(renderer && std::string(renderer).find("D3D12") != std::string::npos);
}

bool readFileContents(const fs::path& path, std::string& out) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool readShaderSource(const fs::path& path, std::string& out, std::vector<fs::path>& stack) {
    const fs::path normalized = fs::absolute(path).lexically_normal();
    if (stack.size() >= 32 || std::find(stack.begin(), stack.end(), normalized) != stack.end()) {
        Logger::error("Shader include cycle or nesting limit at " + normalized.string());
        return false;
    }
    std::string contents;
    if (!readFileContents(normalized, contents)) {
        Logger::error("Failed to open shader source: " + normalized.string());
        return false;
    }
    stack.push_back(normalized);
    std::istringstream input(contents);
    std::string line;
    int lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        const auto start = line.find_first_not_of(" \t");
        if (start != std::string::npos && line.compare(start, 8, "#include") == 0) {
            const auto open = line.find('"', start + 8);
            const auto close = open == std::string::npos ? std::string::npos : line.find('"', open + 1);
            if (open == std::string::npos || close == std::string::npos ||
                line.find_first_not_of(" \t\r", close + 1) != std::string::npos ||
                line.find_first_not_of(" \t", start + 8) != open || close == open + 1) {
                Logger::error("Invalid shader include at " + normalized.string() + ":" + std::to_string(lineNumber));
                stack.pop_back();
                return false;
            }
            out += "#line 1\n";
            if (!readShaderSource(normalized.parent_path() / line.substr(open + 1, close - open - 1), out, stack)) {
                stack.pop_back();
                return false;
            }
            out += "#line " + std::to_string(lineNumber + 1) + "\n";
        } else {
            out += line + "\n";
        }
    }
    stack.pop_back();
    return true;
}

bool readShaderSource(const fs::path& path, std::string& out) {
    std::vector<fs::path> stack;
    out.clear();
    return readShaderSource(path, out, stack);
}

GLuint compileShaderSource(GLenum stage, const std::string& source, const std::string& label) {
    const auto start = std::chrono::steady_clock::now();
    GLuint shader = glCreateShader(stage);
    const char* str = source.c_str();
    glShaderSource(shader, 1, &str, nullptr);
    glCompileShader(shader);
    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        GLint logLength = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
        std::vector<char> log(static_cast<size_t>(std::max(logLength, 1)));
        glGetShaderInfoLog(shader, logLength, nullptr, log.data());
        Logger::error(label + " compilation error: " + std::string(log.data()));
        glDeleteShader(shader);
        return 0;
    }
    Logger::info(label + " compile: " + std::to_string(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()) + " ms");
    return shader;
}

bool linkProgram(GLuint program, const std::string& label) {
    const auto start = std::chrono::steady_clock::now();
    glLinkProgram(program);
    GLint status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        GLint logLength = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
        std::vector<char> log(static_cast<size_t>(std::max(logLength, 1)));
        glGetProgramInfoLog(program, logLength, nullptr, log.data());
        Logger::error(label + " program linking error: " + std::string(log.data()));
        return false;
    }
    Logger::info(label + " link: " + std::to_string(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()) + " ms");
    return true;
}

bool tryLoadFromCache(const fs::path& cacheDir,
                     const std::string& cacheKey,
                     uint64_t sourceHash,
                     GLuint& outProgram) {
    if (!programBinariesSupported()) return false;
    const fs::path bin = cacheDir / (cacheKey + ".rzbin");
    std::ifstream binFile(bin, std::ios::binary);
    if (!binFile) return false;

    ShaderBinaryMetadata m{};
    if (!binFile.read(reinterpret_cast<char*>(&m), sizeof(m))) return false;
    std::error_code ec;
    const auto length = fs::file_size(bin, ec);
    if (m.magic != ShaderBinaryMetadata{}.magic || m.sourceHash != sourceHash ||
        m.driverHash != driverFingerprint() || m.binaryLength == 0 ||
        m.binaryLength > 256u * 1024u * 1024u || ec || length != sizeof(m) + m.binaryLength) return false;

    std::vector<char> binary(m.binaryLength);
    if (!binFile.read(binary.data(), binary.size()) ||
        hashText(1469598103934665603ull, std::string(binary.data(), binary.size())) != m.binaryHash) return false;

    GLuint program = glCreateProgram();
    glProgramBinary(program, m.binaryFormat, binary.data(), static_cast<GLsizei>(binary.size()));
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        glDeleteProgram(program);
        return false;
    }
    outProgram = program;
    return true;
}

bool writeProgramBinary(GLuint program,
                        const fs::path& cacheDir,
                        const std::string& cacheKey,
                        uint64_t sourceHash) {
    if (!programBinariesSupported()) {
        Logger::error("GL program binary caching is unavailable on this renderer");
        return false;
    }
    GLint binaryLength = 0;
    glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &binaryLength);
    if (binaryLength <= 0) {
        Logger::error("Driver returned no shader program binary for " + cacheKey);
        return false;
    }
    std::vector<char> binary(binaryLength);
    GLenum format = 0;
    GLsizei written = 0;
    glGetProgramBinary(program, binaryLength, &written, &format, binary.data());
    if (written <= 0) {
        Logger::error("Failed to retrieve shader program binary for " + cacheKey);
        return false;
    }
    binary.resize(written);
    ShaderBinaryMetadata m{};
    m.sourceHash = sourceHash;
    m.driverHash = driverFingerprint();
    m.binaryHash = hashText(1469598103934665603ull, std::string(binary.data(), binary.size()));
    m.binaryFormat = format;
    m.binaryLength = static_cast<uint32_t>(binary.size());
    std::error_code ec;
    fs::create_directories(cacheDir, ec);
    if (ec) {
        Logger::error("Cannot create shader cache directory: " + ec.message());
        return false;
    }
    const fs::path destination = cacheDir / (cacheKey + ".rzbin");
    const fs::path temporary = destination.string() + "." +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "." +
        std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + ".tmp";
    std::ofstream out(temporary, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&m), sizeof(m));
    out.write(binary.data(), binary.size());
    out.close();
    if (!out) {
        Logger::error("Failed to write shader binary cache: " + cacheKey);
        fs::remove(temporary, ec);
        return false;
    }
    fs::rename(temporary, destination, ec);
    if (ec) {
        Logger::error("Failed to publish shader binary cache " + cacheKey + ": " + ec.message());
        fs::remove(temporary, ec);
        return false;
    }
    Logger::info("Wrote shader binary cache: " + cacheKey);
    return true;
}

bool verifyPreparedBinary(const fs::path& cacheDir, const std::string& cacheKey, uint64_t sourceHash) {
    GLuint program = 0;
    if (!tryLoadFromCache(cacheDir, cacheKey, sourceHash, program)) {
        Logger::error("Driver rejected prepared shader binary: " + cacheKey);
        return false;
    }
    glDeleteProgram(program);
    return true;
}

} // namespace

// =============================================================================
// GLProgram
// =============================================================================

GLProgram::~GLProgram() { reset(); }

void GLProgram::setCacheMode(ShaderCacheMode mode) { cacheMode = mode; }

GLProgram::GLProgram(GLProgram&& other) noexcept : id_(other.id_) { other.id_ = 0; }

GLProgram& GLProgram::operator=(GLProgram&& other) noexcept {
    if (this != &other) {
        reset();
        id_ = other.id_;
        other.id_ = 0;
    }
    return *this;
}

void GLProgram::reset() {
    if (id_ != 0) {
        glDeleteProgram(id_);
        id_ = 0;
    }
}

void GLProgram::adopt(GLuint program) {
    reset();
    id_ = program;
}

GLuint GLProgram::release() noexcept {
    GLuint program = id_;
    id_ = 0;
    return program;
}

GLint GLProgram::uniformLocation(const char* name) const {
    if (id_ == 0) return -1;
    return glGetUniformLocation(id_, name);
}

void GLProgram::use() const {
    glUseProgram(id_);
}

bool GLProgram::loadGraphics(const std::string& vertPath, const std::string& fragPath) {
    reset();
    fs::path va = fs::absolute(fs::path(vertPath));
    fs::path fa = fs::absolute(fs::path(fragPath));
    fs::path cacheDir = va.parent_path() / "cache";
    std::string keyBase = va.filename().string() + "_" + fa.filename().string();
    std::string cacheKey = keyBase + "_v2";

    std::string vsrc, fsrc;
    if (!readShaderSource(va, vsrc) || !readShaderSource(fa, fsrc)) {
        Logger::error("Failed to open shader sources: " + va.string() + " and/or " + fa.string());
        return false;
    }
    const uint64_t sourceHash = hashText(hashText(1469598103934665603ull, vsrc), fsrc);

    GLuint cached = 0;
    if (cacheMode != ShaderCacheMode::Prepare && tryLoadFromCache(cacheDir, cacheKey, sourceHash, cached)) {
        Logger::info("Loaded shader binary cache for " + keyBase);
        id_ = cached;
        return true;
    }

    if (cacheMode == ShaderCacheMode::LoadOnly) {
        Logger::error("No compatible shader binary cache for " + keyBase);
        return false;
    }
    if (cacheMode == ShaderCacheMode::Prepare && !programBinariesSupported()) {
        Logger::error("GL program binary caching is unavailable on this renderer");
        return false;
    }

    GLuint vs = compileShaderSource(GL_VERTEX_SHADER, vsrc, "Vertex shader (" + va.filename().string() + ")");
    if (vs == 0) return false;
    GLuint fs_ = compileShaderSource(GL_FRAGMENT_SHADER, fsrc, "Fragment shader (" + fa.filename().string() + ")");
    if (fs_ == 0) { glDeleteShader(vs); return false; }

    GLuint program = glCreateProgram();
    if (programBinariesSupported()) {
        glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    }
    glAttachShader(program, vs);
    glAttachShader(program, fs_);
    bool linked = linkProgram(program, keyBase);
    glDeleteShader(vs);
    glDeleteShader(fs_);
    if (!linked) {
        glDeleteProgram(program);
        return false;
    }
    const bool saved = programBinariesSupported() && writeProgramBinary(program, cacheDir, cacheKey, sourceHash);
    if (cacheMode == ShaderCacheMode::Prepare &&
        (!saved || !verifyPreparedBinary(cacheDir, cacheKey, sourceHash))) {
        Logger::error("Could not prepare shader cache for " + keyBase);
        glDeleteProgram(program);
        return false;
    }
    id_ = program;
    return true;
}

bool GLProgram::loadCompute(const std::string& compPath) {
    reset();
    fs::path ca = fs::absolute(fs::path(compPath));
    fs::path cacheDir = ca.parent_path() / "cache";
    std::string keyBase = ca.filename().string();
    std::string cacheKey = keyBase + "_compute_v2";

    std::string csrc;
    if (!readShaderSource(ca, csrc)) {
        Logger::error("Failed to open compute shader source: " + ca.string());
        return false;
    }
    const uint64_t sourceHash = hashText(1469598103934665603ull, csrc);

    GLuint cached = 0;
    if (cacheMode != ShaderCacheMode::Prepare && tryLoadFromCache(cacheDir, cacheKey, sourceHash, cached)) {
        Logger::info("Loaded compute shader binary cache for " + keyBase);
        id_ = cached;
        return true;
    }

    if (cacheMode == ShaderCacheMode::LoadOnly) {
        Logger::error("No compatible compute shader binary cache for " + keyBase);
        return false;
    }
    if (cacheMode == ShaderCacheMode::Prepare && !programBinariesSupported()) {
        Logger::error("GL program binary caching is unavailable on this renderer");
        return false;
    }

    GLuint cs = compileShaderSource(GL_COMPUTE_SHADER, csrc, "Compute shader (" + keyBase + ")");
    if (cs == 0) return false;
    GLuint program = glCreateProgram();
    if (programBinariesSupported()) {
        glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    }
    glAttachShader(program, cs);
    bool linked = linkProgram(program, keyBase);
    glDeleteShader(cs);
    if (!linked) {
        glDeleteProgram(program);
        return false;
    }
    const bool saved = programBinariesSupported() && writeProgramBinary(program, cacheDir, cacheKey, sourceHash);
    if (cacheMode == ShaderCacheMode::Prepare &&
        (!saved || !verifyPreparedBinary(cacheDir, cacheKey, sourceHash))) {
        Logger::error("Could not prepare shader cache for " + keyBase);
        glDeleteProgram(program);
        return false;
    }
    id_ = program;
    return true;
}

// =============================================================================
// GLTexture
// =============================================================================

GLTexture::~GLTexture() {
    if (id_ != 0) glDeleteTextures(1, &id_);
}

GLTexture::GLTexture(GLTexture&& other) noexcept
    : id_(other.id_), width_(other.width_), height_(other.height_) {
    other.id_ = 0;
    other.width_ = 0;
    other.height_ = 0;
}

GLTexture& GLTexture::operator=(GLTexture&& other) noexcept {
    if (this != &other) {
        if (id_ != 0) glDeleteTextures(1, &id_);
        id_ = other.id_;
        width_ = other.width_;
        height_ = other.height_;
        other.id_ = 0;
        other.width_ = 0;
        other.height_ = 0;
    }
    return *this;
}

bool GLTexture::ensureRGBA32F(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    bool reallocated = false;
    if (id_ == 0) {
        glGenTextures(1, &id_);
        glBindTexture(GL_TEXTURE_2D, id_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, id_);
    }
    if (width != width_ || height != height_) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
        const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (GLEW_VERSION_4_4 || GLEW_ARB_clear_texture) {
            glClearTexImage(id_, 0, GL_RGBA, GL_FLOAT, zero);
        } else {
            std::vector<float> pixels(static_cast<size_t>(width) * height * 4, 0.0f);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data());
        }
        width_ = width;
        height_ = height;
        reallocated = true;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    return reallocated;
}

void GLTexture::clearToZero() {
    if (id_ == 0) return;
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (GLEW_VERSION_4_4 || GLEW_ARB_clear_texture) {
        glClearTexImage(id_, 0, GL_RGBA, GL_FLOAT, zero);
    } else {
        std::vector<float> pixels(static_cast<size_t>(width_) * height_ * 4, 0.0f);
        GLint previous = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
        glBindTexture(GL_TEXTURE_2D, id_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_, GL_RGBA, GL_FLOAT, pixels.data());
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
    }
}

} // namespace rayzen
