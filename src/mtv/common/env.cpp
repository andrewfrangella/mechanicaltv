#include "mtv/common/env.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

namespace mtv::env {

std::string trim(const std::string& s) {
  auto b = s.begin();
  auto e = s.end();
  while (b != e && std::isspace(static_cast<unsigned char>(*b))) ++b;
  while (e != b && std::isspace(static_cast<unsigned char>(*(e - 1)))) --e;
  return std::string(b, e);
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::optional<std::string> get_opt(const std::string& name) {
  const char* v = std::getenv(name.c_str());
  if (!v) return std::nullopt;
  return std::string(v);
}

bool is_set(const std::string& name) { return std::getenv(name.c_str()) != nullptr; }

std::string get(const std::string& name, const std::string& fallback) {
  auto v = get_opt(name);
  if (!v || v->empty()) return fallback;
  return *v;
}

bool flag(const std::string& name) { return get(name) == "1"; }

int get_int(const std::string& name, int fallback) {
  auto v = get_opt(name);
  if (!v) return fallback;
  try {
    std::size_t used = 0;
    int r = std::stoi(trim(*v), &used);
    return r;
  } catch (...) {
    return fallback;
  }
}

double get_double(const std::string& name, double fallback) {
  auto v = get_opt(name);
  if (!v) return fallback;
  try {
    double r = std::stod(trim(*v));
    if (r != r) return fallback;  // NaN
    return r;
  } catch (...) {
    return fallback;
  }
}

void set(const std::string& name, const std::string& value) { ::setenv(name.c_str(), value.c_str(), 1); }
void set_default(const std::string& name, const std::string& value) { ::setenv(name.c_str(), value.c_str(), 0); }
void unset(const std::string& name) { ::unsetenv(name.c_str()); }

void load_env_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) return;
  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = trim(line.substr(0, eq));
    std::string value = trim(line.substr(eq + 1));
    if (key.empty()) continue;
    set_default(key, value);
  }
}

}  // namespace mtv::env
