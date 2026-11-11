// The operator console (port of app.Console). Every Qt call happens on the GUI thread; engine calls
// that can be slow (stopping the motor, re-opening the capture device, a microstep check) run on a
// worker thread so the window never freezes. Previews repaint at 25 Hz, telemetry at 10 Hz.
#pragma once
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QSlider>
#include <QTableWidget>
#include <QTimer>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "mtv/common/json.h"
#include "mtv/common/log.h"
#include "mtv/engine/studio.h"

namespace mtv::console {

class FrameView;
class DebugWindow;

struct SourceChoice {
  const char* key;
  const char* label;
};
const std::vector<SourceChoice>& source_choices();

class ConsoleWindow : public QMainWindow {
  Q_OBJECT
 public:
  ConsoleWindow(Studio& studio, bool fullscreen, const QString& hardware_note, const QString& log_path);
  ~ConsoleWindow() override;

  // Queue an engine call on the worker thread; exceptions become a notice and a log line.
  void action(const std::string& name, json params = json::object());
  void notice(const std::string& text);
  void quit_console();
  DebugWindow* debug() { return debug_; }
  bool closing() const { return closing_; }
  int error_count() const { return error_count_; }
  QString hud_text(const std::string& key) const;
  QString pill_text(const std::string& key) const;
  QString source_status() const;
  QString slider_label(const std::string& key) const;
  QSize source_shown() const;
  QSize output_shown() const;
  void wait_idle();   // tests: wait until the action queue is empty
  void refresh_now() {
    tick_count_ = 0;   // force a telemetry refresh, not just a repaint
    tick();
  }

 protected:
  void closeEvent(QCloseEvent* e) override;
  void keyPressEvent(QKeyEvent* e) override;
  bool eventFilter(QObject* obj, QEvent* e) override;

 private:
  void build();
  QWidget* panel(QWidget* parent = nullptr);
  QLabel* heading(const QString& text);
  QLabel* pill();
  void set_pill(QLabel* pill, const QString& text, const std::string& state);
  QSlider* slider(double from, double to, double step, std::function<void(double)> commit, std::function<void(double)> show);
  double slider_value(QSlider* s) const;
  void set_slider(QSlider* s, double v);
  bool dragging(QSlider* s) const;
  void tick();
  void paint_frames();
  void update_hud(const json& rt);
  void sync_source_widgets(const json& pipe);
  void sync_calibration_widgets();
  void refresh_library();
  void drain_log();
  void source_selected(int index);
  void add_file();
  void select_clip();
  void delete_clip();
  void set_fullscreen(bool on);
  void job_loop();
  std::string selected_id() const;

  Studio& studio_;
  RealtimeController& rt_;
  FramePipeline& pipe_;
  QString hardware_note_;
  DebugWindow* debug_ = nullptr;
  QTimer timer_;
  int tick_count_ = 0;
  bool closing_ = false;
  bool fullscreen_ = false;
  int error_count_ = 0;

  // header
  QLabel *pill_hw_, *pill_motor_, *pill_sync_, *pill_hdmi_, *pill_log_;
  // source
  QComboBox* source_box_;
  QLabel *source_status_, *source_hint_, *label_src_;
  FrameView *view_src_, *view_out_;
  std::map<std::string, QLabel*> hud_;
  // controls
  QPushButton *btn_start_, *btn_stop_, *btn_test_;
  QSlider *speed_, *bright_, *phase_, *gamma_, *contrast_;
  QLabel *speed_val_, *bright_val_, *phase_val_, *gamma_val_, *contrast_val_;
  QCheckBox *free_run_box_, *inv_x_, *inv_y_, *crop_box_;
  QTableWidget* table_;
  QLabel *lib_status_, *notice_label_, *footer_right_;

  struct SliderInfo {
    double scale;
    double from;
    std::function<void(double)> commit;
    std::function<void(double)> show;
    double released_at = -1e9;
  };
  std::map<QSlider*, SliderInfo> sliders_;
  bool syncing_ = false;
  double source_changed_at_ = 0;
  double free_run_changed_at_ = 0;
  std::string selected_clip_;
  std::string last_library_;
  double last_library_at_ = -1e9;
  std::string last_error_;
  double last_summary_at_ = 0;
  FramePtr painted_src_, painted_out_;
  PreviewPtr painted_preview_;
  bool showing_preview_ = false;

  // worker
  std::thread worker_;
  std::mutex jobs_lock_;
  std::condition_variable jobs_cv_;
  std::deque<std::pair<std::string, json>> jobs_;
  bool busy_ = false;
  bool stop_worker_ = false;
  std::mutex notices_lock_;
  std::deque<std::string> notices_;
  std::mutex log_lock_;
  std::deque<log::Record> log_queue_;
};

}  // namespace mtv::console
