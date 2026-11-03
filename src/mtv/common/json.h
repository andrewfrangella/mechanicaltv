// Telemetry and API payloads use nlohmann::json so the structure matches the Python dictionaries 1:1
// (the web page and the diagnostics download consume exactly those shapes).
#pragma once
#include <nlohmann/json.hpp>

namespace mtv {
using json = nlohmann::json;

// round(x, digits) with Python semantics for the values telemetry reports.
inline double round_to(double value, int digits) {
  double scale = 1.0;
  for (int i = 0; i < digits; ++i) scale *= 10.0;
  return std::round(value * scale) / scale;
}
}  // namespace mtv
