// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <glad/glad.h>
#include "common/common_types.h"
#include "common/math_util.h"
#include "core/hw/gpu.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_opengl/gl_resource_manager.h"
#include "video_core/renderer_opengl/gl_state.h"

namespace Layout {
struct FramebufferLayout;
}

namespace OpenGL {

class OGLTextureMailbox;

/// Structure used for storing information about the textures for each 3DS screen
struct TextureInfo {
    OGLTexture resource;
    GLsizei width;
    GLsizei height;
    GPU::Regs::PixelFormat format;
    GLenum gl_format;
    GLenum gl_type;
};

/// Structure used for storing information about the display target for each 3DS screen
struct ScreenInfo {
    GLuint display_texture;
    Common::Rectangle<float> display_texcoords;
    TextureInfo texture;
};

class RendererOpenGL : public VideoCore::RendererBase {
public:
    explicit RendererOpenGL(Frontend::EmuWindow& window);
    ~RendererOpenGL() override;

    /// Initialize the renderer
    VideoCore::ResultStatus Init() override;

    /// Shutdown the renderer
    void ShutDown() override;

    /// Finalizes rendering the guest frame
    void SwapBuffers() override;

    /// Draws the latest frame from texture mailbox to the currently bound draw framebuffer in this
    /// context
    bool TryPresent() override;

    ///
    void ResetPresent() override;

private:
    void InitOpenGLObjects();
    void ConfigureFramebufferTexture(TextureInfo& texture,
                                     const GPU::Regs::FramebufferConfig& framebuffer);
    void DrawScreens(const Layout::FramebufferLayout& layout);
    void DrawSingleScreenRotated(u32 index);
    void RenderToMailbox(const Layout::FramebufferLayout& layout);

    // Loads framebuffer from emulated memory into the display information structure
    void LoadFBToScreenInfo(const GPU::Regs::FramebufferConfig& framebuffer,
                            ScreenInfo& screen_info, bool right_eye);
    // Fills active OpenGL texture with the given RGB color.
    void LoadColorToActiveGLTexture(u8 color_r, u8 color_g, u8 color_b, const TextureInfo& texture);

    OpenGLState state;

    std::unique_ptr<OGLTextureMailbox> mailbox;

    // OpenGL object IDs
    OGLVertexArray vertex_array;
    OGLBuffer vertex_buffer;
    OGLProgram shader;
    OGLSampler filter_sampler;

    /// Display information for top and bottom screens respectively
    std::array<ScreenInfo, 3> screen_infos;

    // Shader uniform location indices
    GLuint uniform_color_texture;
    GLuint uniform_resolution;

    // Shader attribute input indices
    GLuint attrib_position;
    GLuint attrib_tex_coord;
};

} // namespace OpenGL
