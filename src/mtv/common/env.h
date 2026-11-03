// Environment and settings helpers (MTV_* variables, the install.sh environment file).
#pragma once
#include <optional>
#include <string>
#include <vector>

namespace mtv::env {

// getenv with a default; empty or unset -> fallback.
std::string get(const std::string& name, const std::string& fallback = "");
std::optional<std::string> get_opt(const std::string& name);
bool is_set(const std::string& name);
// "1" -> true (mirrors `os.environ.get(X) == "1"`).
bool flag(const std::string& name);
int get_int(const std::string& name, int fallback);
double get_double(const std::string& name, double fallback);
void set(const std::string& name, const std::string& value);
void set_default(const std::string& name, const std::string& value);
void unset(const std::string& name);

// Read KEY=value lines (install.sh format) into the environment without overriding existing values.
// Missing or unreadable files are ignored (like the Python load_env_file).
void load_env_file(const std::string& path);

// Lower-cased, trimmed copy.
std::string lower(std::string s);
std::string trim(const std::string& s);

}  // namespace mtv::env
