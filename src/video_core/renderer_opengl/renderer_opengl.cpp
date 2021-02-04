// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <glad/glad.h>
#include <queue>
#include "common/assert.h"
#include "common/bit_field.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/hw/gpu.h"
#include "core/hw/hw.h"
#include "core/hw/lcd.h"
#include "core/memory.h"
#include "core/settings.h"
#include "core/tracer/recorder.h"
#include "video_core/debug_utils/debug_utils.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/renderer_opengl/gl_vars.h"
#include "video_core/renderer_opengl/on_screen_display.h"
#include "video_core/renderer_opengl/renderer_opengl.h"
#include "video_core/video_core.h"

namespace OpenGL {

// If the size of this is too small, it ends up creating a soft cap on FPS as the renderer will have
// to wait on available presentation frames. There doesn't seem to be much of a downside to a larger
// number but 9 swap textures at 60FPS presentation allows for 800% speed so thats probably fine
constexpr std::size_t SWAP_CHAIN_SIZE = 6;

struct OGLFrame {
    u32 width{};                 /// Width of the frame (to detect resize)
    u32 height{};                /// Height of the frame
    bool color_reloaded = false; /// Texture attachment was recreated (ie: resized)
    OGLRenderbuffer color{};     /// Buffer shared between the render/present FBO
    OGLFramebuffer render{};     /// FBO created on the render thread
    OGLFramebuffer present{};    /// FBO created on the present thread
    GLsync render_fence{};       /// Fence created on the render thread
    GLsync present_fence{};      /// Fence created on the presentation thread
};

class OGLTextureMailbox {
public:
    std::mutex swap_chain_lock;
    std::condition_variable free_cv;
    std::array<OGLFrame, SWAP_CHAIN_SIZE> swap_chain{};
    std::queue<OGLFrame*> free_queue;
    std::deque<OGLFrame*> present_queue;
    OGLFrame* previous_frame = nullptr;
    std::chrono::milliseconds elapsed{0};

    OGLTextureMailbox() {
        if (Settings::values.use_frame_limit) {
            // min to 1, speed 200%
            u16 time = std::max(200 - Settings::values.frame_limit, 2) >> 1;
            elapsed = std::chrono::milliseconds{time};
        }
        for (auto& frame : swap_chain) {
            free_queue.push(&frame);
        }
    }

    ~OGLTextureMailbox() {
        // lock the mutex and clear out the present and free_queues and notify any people who are
        // blocked to prevent deadlock on shutdown
        std::scoped_lock lock(swap_chain_lock);
        std::queue<OGLFrame*>().swap(free_queue);
        present_queue.clear();
        free_cv.notify_all();
    }

    /// called in core thread
    void ResetPresent() {
        if (!present_queue.empty()) {
            std::scoped_lock lock(swap_chain_lock);
            for (auto& frame : present_queue) {
                free_queue.push(frame);
            }
            present_queue.clear();
        }
    }

    /// called in core thread
    /// Recreate the frame if the size of the window has changed
    void ReloadRenderFrame(OGLFrame* frame, u32 width, u32 height) {
        // Recreate the color texture attachment
        frame->color.Release();
        frame->color.Create();
        GLuint prev_render_buffer = OpenGLState::BindRenderbuffer(frame->color.handle);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
        // Recreate the FBO for the render target
        frame->render.Release();
        frame->render.Create();
        OpenGLState::BindReadFramebuffer(frame->render.handle);
        OpenGLState::BindDrawFramebuffer(frame->render.handle);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                  frame->color.handle);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            LOG_CRITICAL(Render_OpenGL, "Failed to recreate render FBO!");
        }
        OpenGLState::BindRenderbuffer(prev_render_buffer);
        frame->width = width;
        frame->height = height;
        frame->color_reloaded = true;
    }

    /// called in core thread
    OGLFrame* GetRenderFrame() {
        std::unique_lock<std::mutex> lock(swap_chain_lock);

        // If theres no free frames, we will reuse the oldest render frame
        if (free_queue.empty()) {
            // wait for new entries in the present_queue
            free_cv.wait_for(lock, elapsed, [this] { return !free_queue.empty(); });
            if (free_queue.empty()) {
                auto frame = present_queue.front();
                present_queue.pop_front();

                // recycle one more pending frame
                free_queue.push(present_queue.front());
                present_queue.pop_front();

                return frame;
            }
        }

        OGLFrame* frame = free_queue.front();
        free_queue.pop();
        return frame;
    }

    /// called in core thread
    void ReleaseRenderFrame(OGLFrame* frame) {
        std::unique_lock<std::mutex> lock(swap_chain_lock);
        present_queue.push_back(frame);
    }

    /// called in present thread
    OGLFrame* TryGetPresentFrame() {
        if (present_queue.empty()) {
            return nullptr;
        } else {
            std::unique_lock<std::mutex> lock(swap_chain_lock);

            // free the previous frame and add it back to the free queue
            if (previous_frame) {
                free_queue.push(previous_frame);
                free_cv.notify_one();
            }

            // the newest entries are pushed to the front of the queue
            previous_frame = present_queue.front();
            present_queue.pop_front();

            if (present_queue.size() > 1) {
                // skip a frame if pendings more than 1
                free_queue.push(present_queue.front());
                present_queue.pop_front();
            }

            return previous_frame;
        }
    }

    /// called in present thread
    /// Recreate the presentation FBO if the color attachment was changed
    void ReloadPresentFrame(OGLFrame* frame, u32 height, u32 width) {
        frame->present.Release();
        frame->present.Create();
        GLint previous_draw_fbo{};
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, frame->present.handle);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                  frame->color.handle);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            LOG_CRITICAL(Render_OpenGL, "Failed to recreate present FBO!");
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previous_draw_fbo);
        frame->color_reloaded = false;
    }
};

static const char vertex_shader[] = R"(
in vec2 vert_position;
in vec2 vert_tex_coord;
out vec2 frag_tex_coord;

// This is a truncated 3x3 matrix for 2D transformations:
// The upper-left 2x2 submatrix performs scaling/rotation/mirroring.
// The third column performs translation.
// The third row could be used for projection, which we don't need in 2D. It hence is assumed to
// implicitly be [0, 0, 1]
uniform mat3x2 modelview_matrix;

void main() {
    // Multiply input position by the rotscale part of the matrix and then manually translate by
    // the last column. This is equivalent to using a full 3x3 matrix and expanding the vector
    // to `vec3(vert_position.xy, 1.0)`
    gl_Position = vec4(mat2(modelview_matrix) * vert_position + modelview_matrix[2], 0.0, 1.0);
    frag_tex_coord = vert_tex_coord;
}
)";

static const char fragment_shader[] = R"(
in vec2 frag_tex_coord;
out vec4 color;

uniform sampler2D color_texture;

void main() {
    color = texture(color_texture, frag_tex_coord);
}
)";

static const char post_processing_header[] = R"(
// hlsl to glsl types
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define uint2 uvec2
#define uint3 uvec3
#define uint4 uvec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4

in float2 frag_tex_coord;
out float4 output_color;

uniform float4 resolution;
uniform sampler2D color_texture;

float4 Sample() { return texture(color_texture, frag_tex_coord); }
float4 SampleLocation(float2 location) { return texture(color_texture, location); }
float4 SampleFetch(int2 location) { return texelFetch(color_texture, location, 0); }
int2 SampleSize() { return textureSize(color_texture, 0); }
float2 GetResolution() { return resolution.xy; }
float2 GetInvResolution() { return resolution.zw; }
float2 GetCoordinates() { return frag_tex_coord; }
void SetOutput(float4 color) { output_color = color; }
)";

/**
 * Vertex structure that the drawn screen rectangles are composed of.
 */
struct ScreenRectVertex {
    ScreenRectVertex(GLfloat x, GLfloat y, GLfloat u, GLfloat v) {
        position[0] = x;
        position[1] = y;
        tex_coord[0] = u;
        tex_coord[1] = v;
    }

    GLfloat position[2];
    GLfloat tex_coord[2];
};

/**
 * Defines a 1:1 pixel ortographic projection matrix with (0,0) on the top-left
 * corner and (width, height) on the lower-bottom.
 *
 * The projection part of the matrix is trivial, hence these operations are represented
 * by a 3x2 matrix.
 */
static std::array<GLfloat, 3 * 2> MakeOrthographicMatrix(float width, float height) {
    std::array<GLfloat, 3 * 2> matrix; // Laid out in column-major order

    // clang-format off
    matrix[0] = 2.f / width; matrix[2] = 0.f;           matrix[4] = -1.f;
    matrix[1] = 0.f;         matrix[3] = -2.f / height; matrix[5] = 1.f;
    // Last matrix row is implicitly assumed to be [0, 0, 1].
    // clang-format on

    return matrix;
}

/**
 * option example: //! key = value
 * @param shader
 * @param options
 */
static void ParsePostShaderOptions(const std::string& shader,
                                   std::unordered_map<std::string, std::string>& options) {
    std::size_t i = 0;
    std::size_t size = shader.size();

    std::string key;
    std::string value;

    bool is_line_begin = true;
    u32 slash_counter = 0;
    bool is_option_key = false;
    bool is_option_value = false;
    while (i < size) {
        char c = shader[i++];
        switch (c) {
        case '/':
            slash_counter += 1;
            break;
        case '!':
            if (is_line_begin && (slash_counter == 2)) {
                is_option_key = true;
            }
            is_line_begin = false;
            break;
        case '=':
            if (is_option_key) {
                is_option_key = false;
                is_option_value = true;
            }
            is_line_begin = false;
            break;
        case ' ':
        case '\t':
            is_line_begin = false;
            break;
        case '\n':
        case '\r':
            is_line_begin = true;
            is_option_key = false;
            is_option_value = false;
            slash_counter = 0;
            if (!key.empty() && !value.empty()) {
                options[key] = value;
                key.clear();
                value.clear();
            }
            break;
        default:
            if (is_option_key) {
                key += c;
            } else if (is_option_value) {
                value += c;
            }
            is_line_begin = false;
            break;
        }
    }
}

RendererOpenGL::RendererOpenGL(Frontend::EmuWindow& window) : RendererBase{window} {
    mailbox = std::make_unique<OGLTextureMailbox>();
}

RendererOpenGL::~RendererOpenGL() = default;

/// Swap buffers (render frame)
void RendererOpenGL::SwapBuffers() {
    const Layout::FramebufferLayout& layout = render_window.GetFramebufferLayout();
    // Maintain the rasterizer's state as a priority
    OpenGLState prev_state = OpenGLState::GetCurState();
    state.viewport.x = 0;
    state.viewport.y = 0;
    state.viewport.width = layout.width;
    state.viewport.height = layout.height;
    state.Apply();

    for (int i : {0, 2}) {
        int fb_id = i == 2 ? 1 : 0;
        const auto& framebuffer = GPU::g_regs.framebuffer_config[fb_id];

        // Main LCD (0): 0x1ED02204, Sub LCD (1): 0x1ED02A04
        u32 lcd_color_addr =
            (fb_id == 0) ? LCD_REG_INDEX(color_fill_top) : LCD_REG_INDEX(color_fill_bottom);
        lcd_color_addr = HW::VADDR_LCD + 4 * lcd_color_addr;
        LCD::Regs::ColorFill color_fill = {0};
        LCD::Read(color_fill.raw, lcd_color_addr);

        if (color_fill.is_enabled) {
            LoadColorToActiveGLTexture(color_fill.color_r, color_fill.color_g, color_fill.color_b,
                                       screen_infos[i].texture);

            // Resize the texture in case the framebuffer size has changed
            screen_infos[i].texture.width = 1;
            screen_infos[i].texture.height = 1;
        } else {
            if (screen_infos[i].texture.width != (GLsizei)framebuffer.width ||
                screen_infos[i].texture.height != (GLsizei)framebuffer.height ||
                screen_infos[i].texture.format != framebuffer.color_format) {
                // Reallocate texture if the framebuffer size has changed.
                // This is expected to not happen very often and hence should not be a
                // performance problem.
                ConfigureFramebufferTexture(screen_infos[i].texture, framebuffer);
            }
            LoadFBToScreenInfo(framebuffer, screen_infos[i], i == 1);
        }
    }

    RenderScreenshot();

    if (Settings::values.use_present_thread) {
        RenderToMailbox(layout);
    } else {
        DrawScreens(layout);
        render_window.SwapBuffers();
    }
    m_current_frame++;

    Core::System::GetInstance().perf_stats->EndSystemFrame();

    // processing thread events
    render_window.PollEvents();

    Core::System::GetInstance().perf_stats->BeginSystemFrame();

    prev_state.Apply();
    RefreshRasterizerSetting();
}

void RendererOpenGL::RenderScreenshot() {
    if (VideoCore::g_screenshot_complete_callback) {
        // Draw this frame to the screenshot framebuffer
        OGLFramebuffer screenshot_framebuffer;
        screenshot_framebuffer.Create();
        OpenGLState::BindReadFramebuffer(screenshot_framebuffer.handle);
        OpenGLState::BindDrawFramebuffer(screenshot_framebuffer.handle);

        const Layout::FramebufferLayout& layout = render_window.GetFramebufferLayout();
        GLuint renderbuffer;
        glGenRenderbuffers(1, &renderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, layout.width, layout.height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                  renderbuffer);

        DrawScreens(layout);

        std::vector<u32> pixels(layout.width * layout.height);
        glReadPixels(0, 0, layout.width, layout.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        FlipPixels(pixels.data(), layout.width, layout.height);

        screenshot_framebuffer.Release();
        glDeleteRenderbuffers(1, &renderbuffer);
        VideoCore::g_screenshot_complete_callback(layout.width, layout.height, pixels);
        VideoCore::g_screenshot_complete_callback = nullptr;
    }
}

/// run in core thread
void RendererOpenGL::RenderToMailbox(const Layout::FramebufferLayout& layout) {
    OGLFrame* frame = mailbox->GetRenderFrame();

    // Clean up sync objects before drawing

    // INTEL driver workaround. We can't delete the previous render sync object until we are
    // sure that the presentation is done
    if (frame->present_fence) {
        glClientWaitSync(frame->present_fence, 0, GL_TIMEOUT_IGNORED);
        glWaitSync(frame->present_fence, 0, GL_TIMEOUT_IGNORED);
        glDeleteSync(frame->present_fence);
        frame->present_fence = nullptr;
    }

    // drawing

    // Recreate the frame if the size of the window has changed
    if (layout.width != frame->width || layout.height != frame->height) {
        LOG_DEBUG(Render_OpenGL, "Reloading render frame");
        mailbox->ReloadRenderFrame(frame, layout.width, layout.height);
    }
    OpenGLState::BindDrawFramebuffer(frame->render.handle);

    // draw frame
    DrawScreens(layout);

    // delete the draw fence if the frame wasn't presented
    if (frame->render_fence) {
        glDeleteSync(frame->render_fence);
    }
    // Create a fence for the frontend to wait on and swap this frame to OffTex
    frame->render_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();

    mailbox->ReleaseRenderFrame(frame);
}

/// run in present thread
bool RendererOpenGL::TryPresent() {
    auto frame = mailbox->TryGetPresentFrame();
    if (!frame) {
        LOG_DEBUG(Render_OpenGL, "TryGetPresentFrame returned no frame to present");
        return false;
    }

    const auto& layout = render_window.GetFramebufferLayout();

    // Clearing before a full overwrite of a fbo can signal to drivers that they can avoid a
    // readback since we won't be doing any blending
    glClear(GL_COLOR_BUFFER_BIT);

    // Recreate the presentation FBO if the color attachment was changed
    if (frame->color_reloaded) {
        LOG_DEBUG(Render_OpenGL, "Reloading present frame");
        mailbox->ReloadPresentFrame(frame, layout.width, layout.height);
    }
    glWaitSync(frame->render_fence, 0, GL_TIMEOUT_IGNORED);
    // INTEL workaround.
    // Normally we could just delete the draw fence here, but due to driver bugs, we can just delete
    // it on the emulation thread without too much penalty
    // glDeleteSync(frame.render_sync);
    // frame.render_sync = 0;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, frame->present.handle);
    glBlitFramebuffer(0, 0, frame->width, frame->height, 0, 0, layout.width, layout.height,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);

    /* insert fence for the main thread to block on */
    frame->present_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    return true;
}

///
void RendererOpenGL::ResetPresent() {
    mailbox->ResetPresent();
}

/**
 * Loads framebuffer from emulated memory into the active OpenGL texture.
 */
void RendererOpenGL::LoadFBToScreenInfo(const GPU::Regs::FramebufferConfig& framebuffer,
                                        ScreenInfo& screen_info, bool right_eye) {

    if (framebuffer.address_right1 == 0 || framebuffer.address_right2 == 0)
        right_eye = false;

    const PAddr framebuffer_addr =
        framebuffer.active_fb == 0
            ? (!right_eye ? framebuffer.address_left1 : framebuffer.address_right1)
            : (!right_eye ? framebuffer.address_left2 : framebuffer.address_right2);

    LOG_TRACE(Render_OpenGL, "0x{:08x} bytes from 0x{:08x}({}x{}), fmt {:x}",
              framebuffer.stride * framebuffer.height, framebuffer_addr, (int)framebuffer.width,
              (int)framebuffer.height, (int)framebuffer.format);

    int bpp = GPU::Regs::BytesPerPixel(framebuffer.color_format);
    std::size_t pixel_stride = framebuffer.stride / bpp;

    // OpenGL only supports specifying a stride in units of pixels, not bytes, unfortunately
    ASSERT(pixel_stride * bpp == framebuffer.stride);

    // Ensure no bad interactions with GL_UNPACK_ALIGNMENT, which by default
    // only allows rows to have a memory alignement of 4.
    ASSERT(pixel_stride % 4 == 0);

    if (!Rasterizer()->AccelerateDisplay(framebuffer, framebuffer_addr,
                                         static_cast<u32>(pixel_stride), screen_info)) {
        // Reset the screen info's display texture to its own permanent texture
        screen_info.display_texture = screen_info.texture.resource.handle;
        screen_info.display_texcoords = Common::Rectangle<float>(0.f, 0.f, 1.f, 1.f);

        Memory::RasterizerFlushRegion(framebuffer_addr, framebuffer.stride * framebuffer.height);

        const u8* framebuffer_data = VideoCore::g_memory->GetPhysicalPointer(framebuffer_addr);

        GLuint old_tex = OpenGLState::BindTexture2D(0, screen_info.texture.resource.handle);

        glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)pixel_stride);

        // Update existing texture
        // TODO: Test what happens on hardware when you change the framebuffer dimensions so that
        //       they differ from the LCD resolution.
        // TODO: Applications could theoretically crash Citra here by specifying too large
        //       framebuffer sizes. We should make sure that this cannot happen.
        if (GLES) {
            u32 bytes_per_pixel = screen_info.texture.gl_format == GL_RGB ? 3 : 4;
            std::vector<u8> pixels(framebuffer.width * framebuffer.height * 4);
            u32 offsets[] = {2, 1, 0, 3};
            for (u32 i = 0; i < framebuffer.width * framebuffer.height * bytes_per_pixel;
                 i += bytes_per_pixel) {
                for (u32 j = 0; j < bytes_per_pixel; ++j) {
                    pixels[i + j] = framebuffer_data[i + offsets[j]];
                }
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, framebuffer.width, framebuffer.height,
                            screen_info.texture.gl_format, screen_info.texture.gl_type,
                            pixels.data());
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, framebuffer.width, framebuffer.height,
                            screen_info.texture.gl_format, screen_info.texture.gl_type,
                            framebuffer_data);
        }

        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

        OpenGLState::BindTexture2D(0, old_tex);
    }
}

/**
 * Fills active OpenGL texture with the given RGB color. Since the color is solid, the texture can
 * be 1x1 but will stretch across whatever it's rendered on.
 */
void RendererOpenGL::LoadColorToActiveGLTexture(u8 color_r, u8 color_g, u8 color_b,
                                                const TextureInfo& texture) {
    u8 framebuffer_data[3] = {color_r, color_g, color_b};
    GLuint old_tex = OpenGLState::BindTexture2D(0, texture.resource.handle);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, framebuffer_data);
    OpenGLState::BindTexture2D(0, old_tex);
}

/**
 * Initializes the OpenGL state and creates persistent objects.
 */
void RendererOpenGL::InitOpenGLObjects() {
    glClearColor(Settings::values.bg_red, Settings::values.bg_green, Settings::values.bg_blue,
                 0.0f);

    // Generate VBO handle for drawing
    vertex_buffer.Create();

    // Generate VAO
    vertex_array.Create();

    // sampler for post shader
    filter_sampler.Create();

    // Link shaders and get variable locations
    std::string frag_source;
    bool linear_mag_filter = true;
    bool linear_min_filter = true;
    if (GLES) {
        frag_source += fragment_shader_precision_OES;
    }
    if (!Settings::values.pp_shader_name.empty()) {
        std::string pp_shader = FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir) +
                                Settings::values.pp_shader_name + ".glsl";
        std::size_t size = FileUtil::ReadFileToString(true, pp_shader, pp_shader);
        if (size > 0 && size == pp_shader.size()) {
            std::unordered_map<std::string, std::string> options;
            ParsePostShaderOptions(pp_shader, options);
            linear_mag_filter = options["mag_filter"] != "nearest";
            linear_min_filter = options["min_filter"] != "nearest";
            frag_source += post_processing_header;
            frag_source += pp_shader;
        } else {
            frag_source += fragment_shader;
        }
    } else {
        frag_source += fragment_shader;
    }
    shader.Create(vertex_shader, frag_source.data());

    // sampler
    glSamplerParameteri(filter_sampler.handle, GL_TEXTURE_MAG_FILTER,
                        linear_mag_filter ? GL_LINEAR : GL_NEAREST);
    glSamplerParameteri(filter_sampler.handle, GL_TEXTURE_MIN_FILTER,
                        linear_min_filter ? GL_LINEAR : GL_NEAREST);
    glSamplerParameteri(filter_sampler.handle, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glSamplerParameteri(filter_sampler.handle, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    state.texture_units[0].sampler = filter_sampler.handle;

    // apply
    state.draw.shader_program = shader.handle;
    state.draw.vertex_array = vertex_array.handle;
    state.draw.vertex_buffer = vertex_buffer.handle;
    state.Apply();

    uniform_modelview_matrix = glGetUniformLocation(shader.handle, "modelview_matrix");
    uniform_color_texture = glGetUniformLocation(shader.handle, "color_texture");
    uniform_resolution = glGetUniformLocation(shader.handle, "resolution");
    attrib_position = glGetAttribLocation(shader.handle, "vert_position");
    attrib_tex_coord = glGetAttribLocation(shader.handle, "vert_tex_coord");

    // Bind texture in Texture Unit 0
    glUniform1i(uniform_color_texture, 0);

    // Attach vertex data to VAO
    glVertexAttribPointer(attrib_position, 2, GL_FLOAT, GL_FALSE, sizeof(ScreenRectVertex),
                          (GLvoid*)offsetof(ScreenRectVertex, position));
    glVertexAttribPointer(attrib_tex_coord, 2, GL_FLOAT, GL_FALSE, sizeof(ScreenRectVertex),
                          (GLvoid*)offsetof(ScreenRectVertex, tex_coord));
    glEnableVertexAttribArray(attrib_position);
    glEnableVertexAttribArray(attrib_tex_coord);

    // Allocate textures for each screen
    for (auto& screen_info : screen_infos) {
        screen_info.texture.resource.Create();

        // Allocation of storage is deferred until the first frame, when we
        // know the framebuffer size.

        OpenGLState::BindTexture2D(0, screen_info.texture.resource.handle);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        screen_info.display_texture = screen_info.texture.resource.handle;
    }

    // init
    OSD::Initialize();
    if (!Settings::values.use_hw_shader) {
        OSD::AddMessage("HW Shader Off", OSD::MessageType::HWShader, OSD::Duration::NORMAL,
                        OSD::Color::YELLOW);
    }
    if (!Settings::values.use_cpu_jit) {
        OSD::AddMessage("CPU JIT Off", OSD::MessageType::CPUJit, OSD::Duration::NORMAL,
                        OSD::Color::YELLOW);
    }

    OpenGLState::BindTexture2D(0, 0);
}

void RendererOpenGL::ConfigureFramebufferTexture(TextureInfo& texture,
                                                 const GPU::Regs::FramebufferConfig& framebuffer) {
    GPU::Regs::PixelFormat format = framebuffer.color_format;
    GLint internal_format;

    texture.format = format;
    texture.width = framebuffer.width;
    texture.height = framebuffer.height;

    switch (format) {
    case GPU::Regs::PixelFormat::RGBA8:
        internal_format = GL_RGBA;
        texture.gl_format = GL_RGBA;
        texture.gl_type = GLES ? GL_UNSIGNED_BYTE : GL_UNSIGNED_INT_8_8_8_8;
        break;

    case GPU::Regs::PixelFormat::RGB8:
        // This pixel format uses BGR since GL_UNSIGNED_BYTE specifies byte-order, unlike every
        // specific OpenGL type used in this function using native-endian (that is, little-endian
        // mostly everywhere) for words or half-words.
        // TODO: check how those behave on big-endian processors.
        internal_format = GL_RGB;

        // GLES Dosen't support BGR , Use RGB instead
        texture.gl_format = GLES ? GL_RGB : GL_BGR;
        texture.gl_type = GL_UNSIGNED_BYTE;
        break;

    case GPU::Regs::PixelFormat::RGB565:
        internal_format = GL_RGB;
        texture.gl_format = GL_RGB;
        texture.gl_type = GL_UNSIGNED_SHORT_5_6_5;
        break;

    case GPU::Regs::PixelFormat::RGB5A1:
        internal_format = GL_RGBA;
        texture.gl_format = GL_RGBA;
        texture.gl_type = GL_UNSIGNED_SHORT_5_5_5_1;
        break;

    case GPU::Regs::PixelFormat::RGBA4:
        internal_format = GL_RGBA;
        texture.gl_format = GL_RGBA;
        texture.gl_type = GL_UNSIGNED_SHORT_4_4_4_4;
        break;

    default:
        UNIMPLEMENTED();
    }

    GLuint old_tex = OpenGLState::BindTexture2D(0, texture.resource.handle);

    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, texture.width, texture.height, 0,
                 texture.gl_format, texture.gl_type, nullptr);

    OpenGLState::BindTexture2D(0, old_tex);
}

/**
 * Draws a single texture to the emulator window, rotating the texture to correct for the 3DS's LCD
 * rotation.
 */
void RendererOpenGL::DrawSingleScreenRotated(u32 index) {
    const ScreenInfo& screen_info = screen_infos[index];
    OpenGLState::BindTexture2D(0, screen_info.display_texture);

    float src_width = screen_info.texture.width * Settings::values.resolution_factor;
    float src_height = screen_info.texture.height * Settings::values.resolution_factor;
    glUniform4f(uniform_resolution, src_width, src_height, 1.0f / src_width, 1.0f / src_height);

    glDrawArrays(GL_TRIANGLE_STRIP, index * 2, 4);
}

/**
 * Draws the emulated screens to the emulator window.
 */
void RendererOpenGL::DrawScreens(const Layout::FramebufferLayout& layout) {
    if (VideoCore::g_renderer_bg_color_update_requested.exchange(false)) {
        // Update background color before drawing
        glClearColor(Settings::values.bg_red, Settings::values.bg_green, Settings::values.bg_blue,
                     0.0f);
    }

    glClear(GL_COLOR_BUFFER_BIT);

    OpenGLState::BindSampler(0, filter_sampler.handle);

    // Set projection matrix
    std::array<GLfloat, 3 * 2> ortho_matrix = MakeOrthographicMatrix(layout.width, layout.height);
    glUniformMatrix3x2fv(uniform_modelview_matrix, 1, GL_FALSE, ortho_matrix.data());

    // Set vertices
    const auto& top_screen = layout.top_screen;
    const auto& top_texcoords = screen_infos[0].display_texcoords;

    const auto& bottom_screen = layout.bottom_screen;
    const auto& bottom_texcoords = screen_infos[2].display_texcoords;

    const std::array<ScreenRectVertex, 8> vertices = {{
        // top screen
        ScreenRectVertex(top_screen.left, top_screen.top, top_texcoords.bottom, top_texcoords.left),
        ScreenRectVertex(top_screen.left, top_screen.top + top_screen.GetHeight(),
                         top_texcoords.top, top_texcoords.left),
        ScreenRectVertex(top_screen.left + top_screen.GetWidth(), top_screen.top,
                         top_texcoords.bottom, top_texcoords.right),
        ScreenRectVertex(top_screen.left + top_screen.GetWidth(),
                         top_screen.top + top_screen.GetHeight(), top_texcoords.top,
                         top_texcoords.right),
        // bottom screen
        ScreenRectVertex(bottom_screen.left, bottom_screen.top, bottom_texcoords.bottom,
                         bottom_texcoords.left),
        ScreenRectVertex(bottom_screen.left, bottom_screen.top + bottom_screen.GetHeight(),
                         bottom_texcoords.top, bottom_texcoords.left),
        ScreenRectVertex(bottom_screen.left + bottom_screen.GetWidth(), bottom_screen.top,
                         bottom_texcoords.bottom, bottom_texcoords.right),
        ScreenRectVertex(bottom_screen.left + bottom_screen.GetWidth(),
                         bottom_screen.top + bottom_screen.GetHeight(), bottom_texcoords.top,
                         bottom_texcoords.right),
    }};
    // prefer `glBufferData` than `glBufferSubData` on mobile device
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices.data(), GL_STREAM_DRAW);

    if (layout.top_screen_enabled) {
        DrawSingleScreenRotated(0);
    }

    if (layout.bottom_screen_enabled) {
        DrawSingleScreenRotated(2);
    }

    // draw on screen display
    OSD::DrawMessage(layout);
}

/// Initialize the renderer
VideoCore::ResultStatus RendererOpenGL::Init() {
    const char* gl_version{reinterpret_cast<char const*>(glGetString(GL_VERSION))};
    const char* gpu_vendor{reinterpret_cast<char const*>(glGetString(GL_VENDOR))};
    const char* gpu_model{reinterpret_cast<char const*>(glGetString(GL_RENDERER))};

    LOG_INFO(Render_OpenGL, "GL_VERSION: {}", gl_version);
    LOG_INFO(Render_OpenGL, "GL_VENDOR: {}", gpu_vendor);
    LOG_INFO(Render_OpenGL, "GL_RENDERER: {}", gpu_model);

    auto& telemetry_session = Core::System::GetInstance().TelemetrySession();
    telemetry_session.AddField(Telemetry::FieldType::UserSystem, "GPU_Vendor", gpu_vendor);
    telemetry_session.AddField(Telemetry::FieldType::UserSystem, "GPU_Model", gpu_model);
    telemetry_session.AddField(Telemetry::FieldType::UserSystem, "GPU_OpenGL_Version", gl_version);

    if (!strcmp(gpu_vendor, "GDI Generic")) {
        return VideoCore::ResultStatus::ErrorGenericDrivers;
    }

    if (!(GLAD_GL_VERSION_3_3 || GLAD_GL_ES_VERSION_3_1)) {
        return VideoCore::ResultStatus::ErrorBelowGL33;
    }

    OpenGL::GLES = Settings::values.use_gles;

    InitOpenGLObjects();

    RefreshRasterizerSetting();

    return VideoCore::ResultStatus::Success;
}

/// Shutdown the renderer
void RendererOpenGL::ShutDown() {
    OSD::Shutdown();
    mailbox.reset();
}

} // namespace OpenGL
