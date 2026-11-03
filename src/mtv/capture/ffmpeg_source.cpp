#include "mtv/capture/ffmpeg_source.h"

#include <cstring>
#include <stdexcept>

#include "mtv/common/env.h"

namespace mtv::ffmpeg {

std::string build_filter(const std::string& fit) {
  const std::string W = std::to_string(WIDTH), H = std::to_string(HEIGHT);
  const std::string PW = std::to_string(PREVIEW_W), PH = std::to_string(PREVIEW_H);
  std::string disk;
  if (fit == "crop") {
    disk = "scale=" + W + ":" + H + ":force_original_aspect_ratio=increase,crop=" + W + ":" + H + ",setsar=1,format=gray";
  } else {
    disk = "scale=" + W + ":" + H + ":force_original_aspect_ratio=decrease,pad=" + W + ":" + H +
           ":(ow-iw)/2:(oh-ih)/2,setsar=1,format=gray";
  }
  // Letterbox offsets snap to the 10-pixel cell lattice so the bars cover whole disk rows.
  std::string preview = "scale=" + PW + ":" + PH + ":force_original_aspect_ratio=decrease,pad=" + PW + ":" + PH +
                        ":floor((ow-iw)/20)*10:floor((oh-ih)/20)*10,setsar=1,format=rgb24";
  return "[0:v]fps=" + std::to_string(CAPTURE_FPS) + ",split=2[pv][dk];[pv]" + preview + "[preview];[dk]" + disk +
         ",format=rgb24,pad=" + PW + ":" + H + ":0:0[disk];[preview][disk]vstack";
}

std::vector<std::string> build_command(const std::string& device, const std::string& fit, const std::string& input_format,
                                       const std::string& ffmpeg) {
  std::vector<std::string> cmd = {ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-f", "v4l2"};
  std::string f = env::lower(input_format);
  if (!f.empty() && f != "auto" && f != "none") {
    cmd.push_back("-input_format");
    cmd.push_back(input_format);
  }
  // Low latency: no demuxer read-ahead and no decoder frame delay.
  for (const char* a : {"-fflags", "nobuffer", "-flags", "low_delay"}) cmd.push_back(a);
  for (const std::string& a : {std::string("-i"), device, std::string("-filter_complex"), build_filter(fit), std::string("-pix_fmt"),
                               std::string("rgb24"), std::string("-f"), std::string("rawvideo"), std::string("pipe:1")}) {
    cmd.push_back(a);
  }
  return cmd;
}

void split_frame(const std::uint8_t* buf, std::size_t size, Preview& preview, Frame& frame) {
  if (size != FRAME_BYTES_OUT) {
    throw std::invalid_argument("expected " + std::to_string(FRAME_BYTES_OUT) + " bytes, got " + std::to_string(size));
  }
  preview.assign(buf, buf + PREVIEW_BYTES);
  const std::size_t row = static_cast<std::size_t>(PREVIEW_W) * 3;
  for (int y = 0; y < HEIGHT; ++y) {
    const std::uint8_t* src = buf + PREVIEW_BYTES + static_cast<std::size_t>(y) * row;
    for (int x = 0; x < WIDTH; ++x) frame[static_cast<std::size_t>(y * WIDTH + x)] = src[x * 3];
  }
}

}  // namespace mtv::ffmpeg
