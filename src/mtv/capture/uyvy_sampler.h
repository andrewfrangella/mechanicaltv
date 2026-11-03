// UYVY frame -> 320x250 RGB preview + 32x25 disk frame (port of v4l2.UYVYSampler / placement).
//
// The preview reads one source pixel per preview pixel; the disk frame is the box average of the
// preview's full-range luma over each 10x10 cell ("fit") or of 5x5 centre samples per cell of a
// centre crop to the 32:25 disk aspect ("crop"). Each sampled source row is first block-copied out
// of the (uncached) V4L2 DMA buffer into cached scratch memory: one wide sequential read per row is
// several times faster on the Pi than scattered single-byte loads from uncached memory.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mtv/common/frame.h"

namespace mtv {

struct Placement { int x = 0, y = 0, w = 0, h = 0; };

// (x, y, w, h) of a source picture scaled to fit a box, its offset snapped down to a `grid` lattice
// so a 16:9 picture occupies exactly disk rows 3-20.
Placement placement(int src_w, int src_h, int box_w, int box_h, int grid = 1);

// floor(origin + (i + 0.5) * span / count) for i in [0, count)
std::vector<int> sample_centres(int count, double span, double origin = 0.0);

class UYVYSampler {
 public:
  // bt709: colour matrix for the preview (the TC358743 driver reports SMPTE 170M, i.e. BT.601, for
  // UYVY); luma on the disk is the same either way.
  UYVYSampler(int width, int height, int stride, const std::string& fit = "fit", bool bt709 = false);

  // Convert one raw buffer of at least stride*height bytes. Throws std::invalid_argument
  // ("short frame: N of M bytes") on a short buffer.
  void convert(const std::uint8_t* raw, std::size_t size, Preview& preview, Frame& frame);

  int width() const { return width_; }
  int height() const { return height_; }
  int stride() const { return stride_; }
  const std::string& fit() const { return fit_; }
  const Placement& picture() const { return place_; }

 private:
  int width_, height_, stride_;
  std::string fit_;
  Placement place_;
  std::vector<int> rows_;     // source row for each preview row inside the placement
  std::vector<int> y_cols_;   // byte offsets of Y, U, V for each preview column inside the placement
  std::vector<int> u_cols_;
  std::vector<int> v_cols_;
  std::vector<int> crop_rows_, crop_cols_;   // luma-canvas coordinates of the crop samples
  std::vector<std::uint8_t> luma_;           // PREVIEW_W x PREVIEW_H canvas, zero outside the picture
  std::vector<std::uint8_t> canvas_;         // RGB canvas, zero outside the picture
  std::vector<std::uint8_t> row_;            // cached copy of one source row
  std::int16_t yl_[256], rv_[256], gu_[256], gv_[256], bu_[256];
  std::uint8_t yl8_[256];
};

}  // namespace mtv
