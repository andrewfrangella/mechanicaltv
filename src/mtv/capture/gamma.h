// Gamma + contrast lookup table (port of capture.build_gamma_lut).
#pragma once
#include <array>
#include <cstdint>

namespace mtv {

using Lut = std::array<std::uint8_t, 256>;

// Contrast is applied around 128, clamped, then gamma: lut[i] = round(255 * ((c/255) ** gamma)).
// gamma is clamped to [0.1, 4.0], contrast to [0.1, 3.0] here (the pipeline clamps tighter before calling).
Lut build_gamma_lut(double gamma = 1.8, double contrast = 1.0);

}  // namespace mtv
