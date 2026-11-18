#ifndef RAYZEN_RENDERER_GL_RESOURCES_H
#define RAYZEN_RENDERER_GL_RESOURCES_H

#include <GL/glew.h>
#include <string>

namespace rayzen {

enum class ShaderCacheMode { Automatic, Prepare, LoadOnly };

// RAII wrapper for a single OpenGL program (graphics or compute).
// Owns one GL program object, releases it on destruction.
class GLProgram {
public:
    GLProgram() = default;
    ~GLProgram();

    GLProgram(const GLProgram&) = delete;
    GLProgram& operator=(const GLProgram&) = delete;
    GLProgram(GLProgram&& other) noexcept;
    GLProgram& operator=(GLProgram&& other) noexcept;
    static void setCacheMode(ShaderCacheMode mode);

    // Compile + link a vertex/fragment program. Uses the on-disk binary cache.
    bool loadGraphics(const std::string& vertPath, const std::string& fragPath);

    // Compile + link a compute program. Uses the on-disk binary cache.
    bool loadCompute(const std::string& compPath);

    // Take ownership of an externally-created program (used for async compile pickup).
    void adopt(GLuint program);
    GLuint release() noexcept;

    GLuint id() const noexcept { return id_; }
    bool   valid() const noexcept { return id_ != 0; }
    GLint  uniformLocation(const char* name) const;
    void   use() const;
    void   reset();

private:
    GLuint id_ = 0;
};

// RAII wrapper for a single 2D texture (RGBA32F by default).
class GLTexture {
public:
    GLTexture() = default;
    ~GLTexture();

    GLTexture(const GLTexture&) = delete;
    GLTexture& operator=(const GLTexture&) = delete;
    GLTexture(GLTexture&& other) noexcept;
    GLTexture& operator=(GLTexture&& other) noexcept;

    // Allocates / reallocates the texture if size changed. Returns true if reallocated.
    bool ensureRGBA32F(int width, int height);

    void clearToZero();

    GLuint id() const noexcept { return id_; }
    int    width() const noexcept { return width_; }
    int    height() const noexcept { return height_; }

private:
    GLuint id_ = 0;
    int width_ = 0;
    int height_ = 0;
};

} // namespace rayzen

#endif
