#include "mtv/engine/library.h"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>

#include "mtv/common/constants.h"
#include "mtv/common/log.h"
#include "mtv/common/subprocess.h"
#include "mtv/common/sysinfo.h"

namespace mtv {

namespace fs = std::filesystem;

namespace {

struct Db {
  sqlite3* db = nullptr;
  ~Db() {
    if (db) sqlite3_close(db);
  }
};

struct Stmt {
  sqlite3_stmt* s = nullptr;
  Stmt(sqlite3* db, const std::string& sql) {
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) != SQLITE_OK) {
      throw std::runtime_error(std::string("SQLite: ") + sqlite3_errmsg(db));
    }
  }
  ~Stmt() { sqlite3_finalize(s); }
  Stmt& text(int i, const std::string& v) {
    sqlite3_bind_text(s, i, v.c_str(), -1, SQLITE_TRANSIENT);
    return *this;
  }
  Stmt& real(int i, double v) {
    sqlite3_bind_double(s, i, v);
    return *this;
  }
  Stmt& integer(int i, long long v) {
    sqlite3_bind_int64(s, i, v);
    return *this;
  }
  bool step() {
    int rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw std::runtime_error(std::string("SQLite: ") + sqlite3_errmsg(sqlite3_db_handle(s)));
  }
  std::string col_text(int i) {
    auto p = sqlite3_column_text(s, i);
    return p ? reinterpret_cast<const char*>(p) : "";
  }
};

void exec(sqlite3* db, const std::string& sql) {
  char* err = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
    std::string m = err ? err : "error";
    sqlite3_free(err);
    throw std::runtime_error("SQLite: " + m);
  }
}

MediaItem row(Stmt& st) {
  MediaItem m;
  m.id = st.col_text(0);
  m.name = st.col_text(1);
  m.status = st.col_text(2);
  m.duration = sqlite3_column_double(st.s, 3);
  m.frames = static_cast<long>(sqlite3_column_int64(st.s, 4));
  m.fit = st.col_text(5);
  m.error = st.col_text(6);
  m.created = sqlite3_column_double(st.s, 7);
  return m;
}

const char* COLUMNS = "id, name, status, duration, frames, fit, error, created";

double unix_now() {
  return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

json MediaItem::to_json() const {
  return {{"id", id}, {"name", name}, {"status", status}, {"duration", duration}, {"frames", frames},
          {"fit", fit}, {"error", error}, {"created", created}};
}

std::string new_ident() {
  static thread_local std::mt19937_64 rng{std::random_device{}() ^ static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
  std::uint64_t a = rng(), b = rng();
  a = (a & 0xffffffffffff0fffULL) | 0x0000000000004000ULL;   // version 4
  b = (b & 0x3fffffffffffffffULL) | 0x8000000000000000ULL;   // variant 10
  return log::format("%016llx%016llx", static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
}

Library::Library(const std::string& root) : root_(root) {
  fs::create_directories(root_);
  db_path_ = (fs::path(root_) / "library.sqlite3").string();
  Db d{open_db()};
  exec(d.db,
       "CREATE TABLE IF NOT EXISTS media (id TEXT PRIMARY KEY, name TEXT, status TEXT, duration REAL DEFAULT 0, "
       "frames INTEGER DEFAULT 0, fit TEXT, error TEXT DEFAULT '', created REAL)");
  std::vector<std::string> interrupted;
  {
    Stmt st(d.db, "SELECT id FROM media WHERE status IN ('uploading','preparing')");
    while (st.step()) interrupted.push_back(st.col_text(0));
  }
  exec(d.db,
       "UPDATE media SET status='failed', error='Interrupted by restart. Delete and upload again.' "
       "WHERE status IN ('uploading','preparing')");
  std::error_code ec;
  for (const auto& id : interrupted) fs::remove(fs::path(root_) / id / "upload.part", ec);
}

sqlite3* Library::open_db() {
  sqlite3* db = nullptr;
  if (sqlite3_open(db_path_.c_str(), &db) != SQLITE_OK) {
    std::string m = db ? sqlite3_errmsg(db) : "cannot open";
    if (db) sqlite3_close(db);
    throw std::runtime_error("SQLite: " + m);
  }
  sqlite3_busy_timeout(db, 15000);
  return db;
}

bool Library::valid_ident(const std::string& ident) {
  if (ident.size() != 32) return false;
  for (char c : ident)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

std::string Library::folder(const std::string& ident) const { return (fs::path(root_) / ident).string(); }
std::string Library::frames_path(const std::string& ident) const { return (fs::path(root_) / ident / "frames.raw").string(); }

std::vector<MediaItem> Library::items() {
  Db d{open_db()};
  Stmt st(d.db, std::string("SELECT ") + COLUMNS + " FROM media ORDER BY created DESC");
  std::vector<MediaItem> out;
  while (st.step()) out.push_back(row(st));
  return out;
}

MediaItem Library::get(const std::string& ident) {
  if (!valid_ident(ident)) throw std::invalid_argument("Invalid video identifier");
  Db d{open_db()};
  Stmt st(d.db, std::string("SELECT ") + COLUMNS + " FROM media WHERE id=?");
  st.text(1, ident);
  if (!st.step()) throw std::invalid_argument("Video not found");
  return row(st);
}

void Library::update(const std::string& ident, const json& values) {
  if (!values.is_object() || values.empty()) throw std::invalid_argument("Invalid update");
  std::string sql = "UPDATE media SET ";
  bool first = true;
  for (auto& [k, v] : values.items()) {
    if (k != "status" && k != "duration" && k != "frames" && k != "error") throw std::invalid_argument("Invalid update");
    sql += (first ? "" : ",") + k + "=?";
    first = false;
  }
  sql += " WHERE id=?";
  Db d{open_db()};
  Stmt st(d.db, sql);
  int i = 1;
  for (auto& [k, v] : values.items()) {
    if (v.is_string()) st.text(i, v.get<std::string>());
    else if (v.is_number_integer()) st.integer(i, v.get<long long>());
    else if (v.is_number()) st.real(i, v.get<double>());
    else st.text(i, v.dump());
    ++i;
  }
  st.text(i, ident);
  st.step();
}

std::string Library::create(const std::string& name, const std::string& fit) {
  if (fit != "fit" && fit != "crop") throw std::invalid_argument("Choose Fit or Crop");
  std::string ident = new_ident();
  fs::create_directory(fs::path(root_) / ident);
  Db d{open_db()};
  Stmt st(d.db, "INSERT INTO media(id,name,status,fit,created) VALUES(?,?,?,?,?)");
  st.text(1, ident).text(2, name.substr(0, 160)).text(3, "uploading").text(4, fit).real(5, unix_now());
  st.step();
  return ident;
}

void Library::remove(const std::string& ident) {
  MediaItem item = get(ident);
  if (item.status == "uploading" || item.status == "queued" || item.status == "preparing") {
    throw std::invalid_argument("Wait for preparation before deleting");
  }
  std::error_code ec;
  fs::remove_all(fs::path(root_) / ident, ec);
  Db d{open_db()};
  Stmt st(d.db, "DELETE FROM media WHERE id=?");
  st.text(1, ident);
  st.step();
}

void Library::prepare(const std::string& ident) {
  MediaItem item;
  try {
    item = get(ident);
  } catch (const std::exception& exc) {
    log::error("Preparation failed for " + ident + ": " + exc.what());
    return;
  }
  fs::path dir = fs::path(root_) / ident;
  const std::string original = (dir / "original").string();
  const std::string frames_part = (dir / "frames.part").string();
  update(ident, {{"status", "preparing"}, {"error", ""}});
  std::error_code ec;
  try {
    // Restrict containers and protocols: imports cannot be playlists or network inputs.
    const std::vector<std::string> input_flags = {"-protocol_whitelist", "file", "-format_whitelist", "mov,matroska,avi"};
    std::vector<std::string> probe = {ffprobe, "-v", "error"};
    probe.insert(probe.end(), input_flags.begin(), input_flags.end());
    for (const char* a : {"-show_streams", "-show_format", "-of", "json"}) probe.push_back(a);
    probe.push_back(original);
    auto pr = proc::run(probe, 30, &cancel);
    if (!pr.ok()) throw std::runtime_error("ffprobe failed: " + pr.err + pr.error);
    json meta = json::parse(pr.out);
    json stream;
    for (auto& s : meta.at("streams"))
      if (s.value("codec_type", "") == "video") {
        stream = s;
        break;
      }
    if (stream.is_null()) throw std::invalid_argument("No video stream found");
    double duration = 0;
    auto& fmt = meta.at("format");
    if (fmt.contains("duration")) duration = std::stod(fmt["duration"].is_string() ? fmt["duration"].get<std::string>() : fmt["duration"].dump());
    if (!(duration > 0 && duration <= MAX_DURATION_S)) throw std::invalid_argument("Video must have a known duration of 10 minutes or less");
    int w = stream.value("width", 0), h = stream.value("height", 0);
    if (!(w > 0 && w <= 1920) || !(h > 0 && h <= 1920)) throw std::invalid_argument("Maximum input dimensions are 1920 pixels on either side");
    if (sysinfo::disk_usage(root_).free < RESERVE_BYTES + 64ull * 1024 * 1024) throw std::invalid_argument("Not enough free space to prepare this video");
    const std::string W = std::to_string(WIDTH), H = std::to_string(HEIGHT);
    std::string geometry = item.fit == "fit"
                               ? "scale=" + W + ":" + H + ":force_original_aspect_ratio=decrease,pad=" + W + ":" + H + ":(ow-iw)/2:(oh-ih)/2"
                               : "scale=" + W + ":" + H + ":force_original_aspect_ratio=increase,crop=" + W + ":" + H;
    std::vector<std::string> cmd = {ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-threads", "1"};
    cmd.insert(cmd.end(), input_flags.begin(), input_flags.end());
    for (const std::string& a : {std::string("-i"), original, std::string("-t"), std::to_string(MAX_DURATION_S), std::string("-map"),
                                 std::string("0:v:0"), std::string("-an"), std::string("-sn"), std::string("-dn"),
                                 std::string("-filter_threads"), std::string("1"), std::string("-vf"),
                                 "fps=" + std::to_string(LIBRARY_FPS) + "," + geometry + ",setsar=1,format=gray",
                                 std::string("-pix_fmt"), std::string("gray"), std::string("-f"), std::string("rawvideo"), frames_part}) {
      cmd.push_back(a);
    }
    auto fr = proc::run(cmd, 900, &cancel);
    if (!fr.ok()) throw std::runtime_error("ffmpeg failed: " + fr.err + fr.error);
    auto size = fs::file_size(frames_part);
    auto count = static_cast<long>(size / FRAME_BYTES);
    if (size % FRAME_BYTES || !(count > 0 && count <= static_cast<long>(MAX_DURATION_S) * LIBRARY_FPS)) {
      throw std::invalid_argument("Invalid prepared frame data");
    }
    fs::rename(frames_part, dir / "frames.raw");
    std::ofstream(dir / "profile.json") << json{{"version", 1}, {"width", WIDTH}, {"height", HEIGHT}, {"fps", LIBRARY_FPS},
                                               {"format", "gray8-row-major"}, {"fit", item.fit}, {"frames", count}}.dump(2);
    update(ident, {{"status", "ready"}, {"duration", static_cast<double>(count) / LIBRARY_FPS}, {"frames", count}});
    log::info("Prepared " + item.name + " (" + std::to_string(count) + " frames)");
  } catch (const std::invalid_argument& exc) {
    update(ident, {{"status", "failed"}, {"error", std::string(exc.what()).substr(0, 300)}});
    log::error("Preparation failed for " + ident + ": " + exc.what());
  } catch (const std::exception& exc) {
    if (cancel) {   // shutting down: leave it queued so it converts on the next run
      update(ident, {{"status", "queued"}});
      log::info("Conversion of " + ident + " interrupted by shutdown; it stays queued");
      std::error_code ec2;
      fs::remove(frames_part, ec2);
      return;
    }
    update(ident, {{"status", "failed"}, {"error", "Video preparation failed. Use an MP4/H.264 file or inspect the debug log."}});
    log::error("Preparation failed for " + ident + ": " + exc.what());
  }
  fs::remove(frames_part, ec);
}

}  // namespace mtv
