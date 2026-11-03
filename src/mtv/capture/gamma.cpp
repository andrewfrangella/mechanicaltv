#include "mtv/capture/gamma.h"

#include <algorithm>
#include <cmath>

namespace mtv {

Lut build_gamma_lut(double gamma, double contrast) {
  Lut lut{};
  gamma = std::clamp(gamma, 0.1, 4.0);
  contrast = std::clamp(contrast, 0.1, 3.0);
  for (int i = 0; i < 256; ++i) {
    double c = (i - 128.0) * contrast + 128.0;   // contrast centred at 128
    c = std::clamp(c, 0.0, 255.0);
    // std::nearbyint rounds half to even, matching Python's round().
    double v = std::nearbyint(255.0 * std::pow(c / 255.0, gamma));
    lut[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(std::clamp(v, 0.0, 255.0));
  }
  return lut;
}

}  // namespace mtv
