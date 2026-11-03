// FFmpeg capture fallback (USB UVC dongles in MJPEG, or any V4L2 device): one raw RGB pipe carrying
// the 320x250 preview stacked over the 32x25 disk frame (port of build_ffmpeg_filter/command and
// split_ffmpeg_frame in capture.py).
#pragma once
#include <string>
#include <vector>

#include "mtv/common/frame.h"

namespace mtv::ffmpeg {

inline constexpr int CAPTURE_FPS = 10;   // frames per second the FFmpeg path delivers
inline constexpr std::size_t FRAME_BYTES_OUT = static_cast<std::size_t>(PREVIEW_W) * (PREVIEW_H + HEIGHT) * 3;

// Filter graph: one input, 10 fps, the RGB preview stacked over the 32x25 disk frame.
std::string build_filter(const std::string& fit = "fit");
// argv: V4L2 in, raw RGB frames of FRAME_BYTES_OUT on stdout. input_format: "mjpeg", "uyvy422",
// or "none"/"auto" to let FFmpeg negotiate.
std::vector<std::string> build_command(const std::string& device, const std::string& fit = "fit",
                                       const std::string& input_format = "mjpeg", const std::string& ffmpeg = "ffmpeg");
// Split one output frame into (preview RGB, disk frame). Throws std::invalid_argument on a wrong size.
void split_frame(const std::uint8_t* buf, std::size_t size, Preview& preview, Frame& frame);

}  // namespace mtv::ffmpeg
