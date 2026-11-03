// Shared constants of the Mechanical TV C++ port (mirrors the Python package).
#pragma once
#include <cstddef>
#include <cstdint>

namespace mtv {

inline constexpr int WIDTH = 32;                      // disk columns (holes)
inline constexpr int HEIGHT = 25;                     // lines per hole
inline constexpr int FRAME_PIXELS = WIDTH * HEIGHT;   // 800 bytes per frame
inline constexpr int TOTAL_PIXELS = FRAME_PIXELS;     // 800 LED pixel slots per revolution
inline constexpr int FRAME_BYTES = FRAME_PIXELS;
inline constexpr int LIBRARY_FPS = 10;                // prepared library clips run at 10 frames per second

inline constexpr int PREVIEW_SCALE = 10;
inline constexpr int PREVIEW_W = WIDTH * PREVIEW_SCALE;    // 320
inline constexpr int PREVIEW_H = HEIGHT * PREVIEW_SCALE;   // 250
inline constexpr std::size_t PREVIEW_BYTES = static_cast<std::size_t>(PREVIEW_W) * PREVIEW_H * 3;

inline constexpr double MAX_FPS = 24.0;   // speed ceiling: 1440 RPM; above ~20 FPS the stepper may stall and the LED pixel slot drops below 55 us
inline constexpr double MIN_FPS = 1.0;
inline constexpr double DEFAULT_FPS = 10.0;

inline constexpr std::size_t MAX_UPLOAD = 256ull * 1024 * 1024;
inline constexpr int MAX_DURATION_S = 600;
inline constexpr std::size_t RESERVE_BYTES = 512ull * 1024 * 1024;

#ifndef MTV_VERSION
#define MTV_VERSION "0.3.0"
#endif
inline constexpr const char* VERSION = MTV_VERSION;

}  // namespace mtv
