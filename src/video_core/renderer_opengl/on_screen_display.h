
#pragma once

#include <string>

#include "common/common_types.h"

namespace Layout {
struct FramebufferLayout;
}

namespace Frontend {
class EmuWindow;
}

namespace OSD {

enum class MessageType {
    FPS,
    D24S8,
    ShaderCache,
    Typeless,
    HWShader,
    CPUJit,
    New3DS,
};

namespace Color {
constexpr u32 CYAN = 0xFF00FFFF;
constexpr u32 GREEN = 0xFF00FF00;
constexpr u32 RED = 0xFFFF0000;
constexpr u32 BLUE = 0xFF00FFFF;
constexpr u32 YELLOW = 0xFFFFFF30;
}; // namespace Color

namespace Duration {
constexpr u32 SHORT = 2000;
constexpr u32 NORMAL = 5000;
constexpr u32 VERY_LONG = 10000;
constexpr u32 FOREVER = -1;
}; // namespace Duration

void Initialize();

void AddMessage(const std::string& message, MessageType type, u32 duration, u32 color);

void DrawMessage(const Frontend::EmuWindow& window, const Layout::FramebufferLayout& layout);

void Shutdown();

} // namespace OSD
