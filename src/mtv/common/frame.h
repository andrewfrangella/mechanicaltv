// Frame types. A Frame is one 32 x 25 8-bit grayscale picture (row-major, 800 bytes); a Preview is a
// 320 x 250 RGB picture of the full HDMI input. Sources publish frames as shared pointers so the
// consumers (LED thread, UI) can compare identity cheaply ("same frame as last time?") and never
// copy under a lock.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "mtv/common/constants.h"

namespace mtv {

using Frame = std::array<std::uint8_t, FRAME_PIXELS>;
using FramePtr = std::shared_ptr<const Frame>;
using Preview = std::vector<std::uint8_t>;   // PREVIEW_BYTES bytes, RGB
using PreviewPtr = std::shared_ptr<const Preview>;
using NipkowStream = std::array<std::uint8_t, TOTAL_PIXELS>;   // one revolution of pixel values, disk order

inline FramePtr black_frame() {
  static const FramePtr black = std::make_shared<const Frame>(Frame{});
  return black;
}

}  // namespace mtv
