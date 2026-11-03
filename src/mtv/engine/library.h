// Persistent media library (port of core.Library): a SQLite catalogue plus one folder per clip with
// the imported original and the prepared 32x25 grayscale frames (frames.raw, 10 fps, row-major).
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "mtv/common/json.h"

struct sqlite3;

namespace mtv {

struct MediaItem {
  std::string id;
  std::string name;
  std::string status;     // uploading | queued | preparing | ready | failed
  double duration = 0.0;
  long frames = 0;
  std::string fit;        // fit | crop
  std::string error;
  double created = 0.0;   // Unix time
  json to_json() const;
};

class Library {
 public:
  explicit Library(const std::string& root);   // creates the folder and table; marks interrupted jobs failed
  const std::string& root() const { return root_; }

  std::vector<MediaItem> items();   // newest first
  MediaItem get(const std::string& ident);   // std::invalid_argument("Invalid video identifier" / "Video not found")
  // Only status, duration, frames and error may change (std::invalid_argument("Invalid update")).
  void update(const std::string& ident, const json& values);
  std::string create(const std::string& name, const std::string& fit);   // std::invalid_argument("Choose Fit or Crop")
  void remove(const std::string& ident);   // std::invalid_argument("Wait for preparation before deleting")
  // Convert <id>/original with ffprobe + ffmpeg into frames.raw and profile.json; marks the item
  // ready, or failed with a message. Never throws.
  void prepare(const std::string& ident);

  static bool valid_ident(const std::string& ident);
  std::string folder(const std::string& ident) const;
  std::string frames_path(const std::string& ident) const;

  std::string ffmpeg = "ffmpeg";
  std::string ffprobe = "ffprobe";
  std::atomic<bool> cancel{false};   // set on shutdown: a running conversion is killed

 private:
  sqlite3* open_db();
  std::string root_;
  std::string db_path_;
};

std::string new_ident();   // uuid4 as 32 lowercase hex digits

}  // namespace mtv
