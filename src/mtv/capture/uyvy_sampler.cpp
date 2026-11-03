#include "mtv/capture/uyvy_sampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace mtv {

Placement placement(int src_w, int src_h, int box_w, int box_h, int grid) {
  double scale = std::min(static_cast<double>(box_w) / src_w, static_cast<double>(box_h) / src_h);
  int w = std::max(1, std::min(box_w, static_cast<int>(std::nearbyint(src_w * scale))));
  int h = std::max(1, std::min(box_h, static_cast<int>(std::nearbyint(src_h * scale))));
  Placement p;
  p.w = w;
  p.h = h;
  p.x = ((box_w - w) / 2) / grid * grid;
  p.y = ((box_h - h) / 2) / grid * grid;
  return p;
}

std::vector<int> sample_centres(int count, double span, double origin) {
  std::vector<int> out(static_cast<std::size_t>(std::max(0, count)));
  double step = span / count;
  for (int i = 0; i < count; ++i) out[static_cast<std::size_t>(i)] = static_cast<int>(std::floor(origin + (i + 0.5) * step));
  return out;
}

UYVYSampler::UYVYSampler(int width, int height, int stride, const std::string& fit, bool bt709)
    : width_(width), height_(height), stride_(stride), fit_(fit == "crop" ? "crop" : "fit") {
  if (width < 2 || height < 1 || stride < width * 2) throw std::invalid_argument("unusable UYVY geometry");
  place_ = placement(width, height, PREVIEW_W, PREVIEW_H, PREVIEW_SCALE);
  rows_ = sample_centres(place_.h, height);
  auto xs = sample_centres(place_.w, width);
  y_cols_.resize(xs.size());
  u_cols_.resize(xs.size());
  v_cols_.resize(xs.size());
  for (std::size_t i = 0; i < xs.size(); ++i) {
    y_cols_[i] = 2 * xs[i] + 1;
    // The last pixel of an odd-width line has no chroma pair of its own: reuse the previous pair.
    u_cols_[i] = std::min((xs[i] / 2) * 4, stride - 4);
    v_cols_[i] = u_cols_[i] + 2;
  }
  // Limited-range YUV -> RGB in x256 fixed point: R = Y' + Rv, G = Y' - Gu - Gv, B = Y' + Bu.
  const int kr = bt709 ? 459 : 409, kgu = bt709 ? 55 : 100, kgv = bt709 ? 136 : 208, kb = bt709 ? 541 : 516;
  for (int i = 0; i < 256; ++i) {
    int yl = (298 * (i - 16) + 128) >> 8;   // arithmetic shift, like numpy
    yl_[i] = static_cast<std::int16_t>(yl);
    rv_[i] = static_cast<std::int16_t>((kr * (i - 128) + 128) >> 8);
    gu_[i] = static_cast<std::int16_t>((kgu * (i - 128) + 128) >> 8);
    gv_[i] = static_cast<std::int16_t>((kgv * (i - 128) + 128) >> 8);
    bu_[i] = static_cast<std::int16_t>((kb * (i - 128) + 128) >> 8);
    // Full-range luma for the disk (video black 16 -> 0, white 235 -> 255), like FFmpeg format=gray.
    yl8_[i] = static_cast<std::uint8_t>(std::clamp(yl, 0, 255));
  }
  luma_.assign(static_cast<std::size_t>(PREVIEW_W) * PREVIEW_H, 0);
  canvas_.assign(PREVIEW_BYTES, 0);
  row_.resize(static_cast<std::size_t>(stride));
  if (fit_ == "crop") {
    const int k = 5;
    const double aspect = static_cast<double>(WIDTH) / HEIGHT;
    double cw, ch;
    if (static_cast<double>(place_.w) / place_.h > aspect) {
      ch = place_.h;
      cw = place_.h * aspect;
    } else {
      cw = place_.w;
      ch = place_.w / aspect;
    }
    double cx0 = place_.x + (place_.w - cw) / 2.0;
    double cy0 = place_.y + (place_.h - ch) / 2.0;
    crop_rows_ = sample_centres(HEIGHT * k, ch, cy0);
    crop_cols_ = sample_centres(WIDTH * k, cw, cx0);
  }
}

void UYVYSampler::convert(const std::uint8_t* raw, std::size_t size, Preview& preview, Frame& frame) {
  const std::size_t needed = static_cast<std::size_t>(stride_) * height_;
  if (size < needed) {
    throw std::invalid_argument("short frame: " + std::to_string(size) + " of " + std::to_string(needed) + " bytes");
  }
  const int pw = place_.w;
  const std::uint8_t* row = row_.data();
  for (int py = 0; py < place_.h; ++py) {
    const std::uint8_t* src = raw + static_cast<std::size_t>(rows_[static_cast<std::size_t>(py)]) * stride_;
    std::memcpy(row_.data(), src, static_cast<std::size_t>(stride_));   // one wide read from uncached memory
    const int cy = place_.y + py;
    std::uint8_t* rgb = canvas_.data() + (static_cast<std::size_t>(cy) * PREVIEW_W + place_.x) * 3;
    std::uint8_t* lum = luma_.data() + static_cast<std::size_t>(cy) * PREVIEW_W + place_.x;
    for (int px = 0; px < pw; ++px) {
      const std::uint8_t y = row[y_cols_[static_cast<std::size_t>(px)]];
      const std::uint8_t u = row[u_cols_[static_cast<std::size_t>(px)]];
      const std::uint8_t v = row[v_cols_[static_cast<std::size_t>(px)]];
      const int yl = yl_[y];
      const int r = yl + rv_[v];
      const int g = yl - gu_[u] - gv_[v];
      const int b = yl + bu_[u];
      rgb[0] = static_cast<std::uint8_t>(std::clamp(r, 0, 255));
      rgb[1] = static_cast<std::uint8_t>(std::clamp(g, 0, 255));
      rgb[2] = static_cast<std::uint8_t>(std::clamp(b, 0, 255));
      rgb += 3;
      lum[px] = yl8_[y];
    }
  }
  if (fit_ == "crop") {
    const int k = 5;
    for (int cy = 0; cy < HEIGHT; ++cy)
      for (int cx = 0; cx < WIDTH; ++cx) {
        unsigned sum = 0;
        for (int j = 0; j < k; ++j) {
          const std::uint8_t* lr = luma_.data() + static_cast<std::size_t>(crop_rows_[static_cast<std::size_t>(cy * k + j)]) * PREVIEW_W;
          for (int i = 0; i < k; ++i) sum += lr[crop_cols_[static_cast<std::size_t>(cx * k + i)]];
        }
        frame[static_cast<std::size_t>(cy * WIDTH + cx)] = static_cast<std::uint8_t>((sum + k * k / 2) / (k * k));
      }
  } else {
    const int k = PREVIEW_SCALE;
    for (int cy = 0; cy < HEIGHT; ++cy)
      for (int cx = 0; cx < WIDTH; ++cx) {
        unsigned sum = 0;
        for (int j = 0; j < k; ++j) {
          const std::uint8_t* lr = luma_.data() + static_cast<std::size_t>(cy * k + j) * PREVIEW_W + cx * k;
          for (int i = 0; i < k; ++i) sum += lr[i];
        }
        frame[static_cast<std::size_t>(cy * WIDTH + cx)] = static_cast<std::uint8_t>((sum + k * k / 2) / (k * k));
      }
  }
  preview.assign(canvas_.begin(), canvas_.end());
}

}  // namespace mtv
