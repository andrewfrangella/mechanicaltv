#include "mtv/console/console_window.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "mtv/common/clock.h"
#include "mtv/common/constants.h"
#include "mtv/common/sysinfo.h"
#include "mtv/console/app_setup.h"
#include "mtv/console/debug_window.h"
#include "mtv/console/frame_view.h"
#include "mtv/console/palette.h"

namespace mtv::console {

namespace {

constexpr int UI_HZ = 25;              // preview repaint rate
constexpr int HUD_EVERY = 2;           // telemetry every 2nd paint (12.5 Hz)
constexpr int HUD_WIDTH = 26;          // characters per telemetry value
constexpr double LIBRARY_REFRESH_S = 2.0;
constexpr double SCALE_GRACE_S = 1.5;  // after a release, give the worker time to apply the value before syncing back
const char* SRC_LABEL_FRAME = "SOURCE VIDEO · 32 × 25 GRAYSCALE";
const char* SRC_LABEL_LIVE = "HDMI INPUT · LIVE PICTURE";

QString fit_text(const std::string& text) {
  QString t = QString::fromStdString(text);
  return t.size() <= HUD_WIDTH ? t : t.left(HUD_WIDTH - 1) + QStringLiteral("…");
}

QString stylesheet() {
  using namespace color;
  return QString(R"(
QWidget { background: %1; color: %5; font-family: "DejaVu Sans", "Helvetica Neue", sans-serif; font-size: 11pt; }
QWidget#panel { background: %2; border-radius: 4px; }
QWidget#panel QLabel, QWidget#panel QCheckBox { background: %2; }
QLabel#eyebrow { color: %6; font-family: "DejaVu Sans Mono", Menlo, monospace; font-size: 9pt; background: transparent; }
QLabel#title { font-size: 22pt; font-weight: bold; background: transparent; }
QLabel#heading { color: %7; font-size: 12pt; font-weight: bold; }
QLabel#muted { color: %6; font-family: "DejaVu Sans Mono", Menlo, monospace; font-size: 9pt; }
QTabWidget::pane { border: 0; }
QTabBar::tab { background: %2; color: %6; font-family: "DejaVu Sans Mono", Menlo, monospace; font-size: 10pt; padding: 8px 22px; border: 1px solid %4; border-bottom: 0; border-top-left-radius: 3px; border-top-right-radius: 3px; margin-right: 4px; }
QTabBar::tab:selected { background: %3; color: %7; font-weight: bold; }
QLabel#big { font-family: "DejaVu Sans Mono", Menlo, monospace; font-size: 18pt; font-weight: bold; }
QLabel#value { font-family: "DejaVu Sans Mono", Menlo, monospace; font-size: 13pt; font-weight: bold; }
QLabel#notice { color: %7; font-size: 9pt; background: transparent; }
QLabel#pill { font-family: "DejaVu Sans Mono", Menlo, monospace; font-size: 9pt; padding: 4px 12px; border: 1px solid %4; border-radius: 3px; background: %1; }
QPushButton { background: %3; color: %5; border: 1px solid %4; border-radius: 3px; padding: 8px 14px; }
QPushButton:hover { background: %4; }
QPushButton:disabled { background: %2; color: %6; }
QPushButton#primary { background: %7; color: %8; border-color: %7; font-weight: bold; }
QPushButton#primary:hover { background: #e9f7a8; }
QPushButton#primary:disabled { background: %2; color: %6; border-color: %4; }
QPushButton#danger { color: %9; border-color: %9; }
QPushButton#danger:hover { background: #3a2222; }
QCheckBox { spacing: 8px; }
QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid %4; background: %3; border-radius: 2px; }
QCheckBox::indicator:checked { background: %7; border-color: %7; }
QComboBox { background: %3; color: %5; border: 1px solid %4; padding: 6px; border-radius: 3px; }
QComboBox QAbstractItemView { background: %3; color: %5; selection-background-color: %7; selection-color: %8; }
QComboBox::drop-down { border: 0; width: 22px; }
QComboBox::down-arrow { image: none; border-left: 5px solid transparent; border-right: 5px solid transparent; border-top: 7px solid %7; width: 0; height: 0; margin-right: 8px; }
QSlider { min-height: 28px; background: %2; }
QSlider::groove:horizontal { height: 8px; background: %3; border: 1px solid %4; border-radius: 4px; }
QSlider::sub-page:horizontal { background: #6f7f45; border: 1px solid %4; border-radius: 4px; }
QSlider::handle:horizontal { width: 22px; height: 22px; margin: -8px 0; background: %7; border: 2px solid %8; border-radius: 11px; }
QSlider::handle:horizontal:hover { background: #e9f7a8; }
QSlider::handle:horizontal:pressed { background: #ffffff; }
QTableWidget { background: %3; alternate-background-color: %2; color: %5; gridline-color: %4; border: 1px solid %4; selection-background-color: %7; selection-color: %8; }
QHeaderView::section { background: %2; color: %6; font-family: "DejaVu Sans Mono", Menlo, monospace; font-size: 9pt; border: 0; padding: 4px; }
QScrollBar:vertical { background: %2; width: 12px; }
QScrollBar::handle:vertical { background: %4; min-height: 24px; border-radius: 4px; }
QPlainTextEdit#logText { background: #0b0d0c; color: %5; font-family: "DejaVu Sans Mono", Menlo, monospace; font-size: 9pt; border: 0; }
QMessageBox, QFileDialog { background: %2; }
)")
      .arg(BG, PANEL, PANEL_ALT, BORDER, TEXT, MUTED, ACCENT, INK, DANGER);
}

}  // namespace

const std::vector<SourceChoice>& source_choices() {
  static const std::vector<SourceChoice> s = {
      {"hdmi", "Live HDMI Video (capture adapter)"},
      {"smpte_bars", "Test Pattern: SMPTE Grayscale Bars"},
      {"grayscale_ramp", "Test Pattern: Horizontal Gray Ramp"},
      {"vertical_ramp", "Test Pattern: Vertical Gray Ramp"},
      {"grid", "Test Pattern: Aperture Grid & Crosshair"},
      {"rotating_bar", "Test Pattern: Rotating Phase Sync Bar"},
      {"pulse_test", "Test Pattern: 1 Hz Photodiode Pulse Test"},
      {"checkerboard", "Test Pattern: 4x4 Checkerboard"},
      {"single_column", "Test Pattern: Sweeping Column"},
      {"single_row", "Test Pattern: Sweeping Row"},
      {"diagonal_line", "Test Pattern: Moving Diagonal"},
      {"library", "Selected Library Clip"},
  };
  return s;
}

static int source_index(const std::string& key) {
  const auto& s = source_choices();
  for (std::size_t i = 0; i < s.size(); ++i)
    if (key == s[i].key) return static_cast<int>(i);
  return 1;
}

ConsoleWindow::ConsoleWindow(Studio& studio, bool fullscreen, const QString& hardware_note, const QString& log_path)
    : studio_(studio), rt_(studio.realtime()), pipe_(studio.pipeline()), hardware_note_(hardware_note) {
  qApp->setStyleSheet(stylesheet());
  setWindowTitle(QString("Mechanical TV %1").arg(VERSION));
  setMinimumSize(1100, 700);
  resize(1500, 900);
  log::set_listener([this](const log::Record& r) {
    std::lock_guard<std::mutex> lock(log_lock_);
    log_queue_.push_back(r);
    if (log_queue_.size() > 5000) log_queue_.pop_front();
  });
  worker_ = std::thread([this] {
    log::set_thread_name("console-actions");
    job_loop();
  });
  build();
  debug_ = new DebugWindow(log_path, nullptr);
  set_fullscreen(fullscreen);
  sync_calibration_widgets();
  for (auto& [s, info] : sliders_) info.show(slider_value(s));   // value labels for sliders still at their minimum
  notice(std::string("Mechanical TV ") + VERSION + " · " + (studio_.hardware_enabled() ? "REALTIME HARDWARE" : "SIMULATION"));
  if (!hardware_note.isEmpty()) {
    notice((studio_.hardware_enabled() ? "Hardware note: " : "Hardware unavailable, running in simulation: ") + hardware_note.toStdString());
  }
  connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
  timer_.start(1000 / UI_HZ);
}

ConsoleWindow::~ConsoleWindow() {
  log::set_listener(nullptr);
  {
    std::lock_guard<std::mutex> lock(jobs_lock_);
    stop_worker_ = true;
  }
  jobs_cv_.notify_all();
  if (worker_.joinable()) worker_.join();
  delete debug_;
}

// ---------------------------------------------------------------- layout helpers

QWidget* ConsoleWindow::panel(QWidget* parent) {
  auto* w = new QWidget(parent);
  w->setObjectName("panel");
  w->setAttribute(Qt::WA_StyledBackground, true);
  return w;
}

QLabel* ConsoleWindow::heading(const QString& text) {
  auto* l = new QLabel(text);
  l->setObjectName("heading");
  return l;
}

QLabel* ConsoleWindow::pill() {
  auto* l = new QLabel("");
  l->setObjectName("pill");
  return l;
}

void ConsoleWindow::set_pill(QLabel* p, const QString& text, const std::string& state) {
  static const std::map<std::string, const char*> colors = {
      {"locked", color::GOOD}, {"free-run", color::WARN}, {"ramping", color::WARN}, {"running", color::WARN},
      {"testing", color::WARN}, {"idle", color::MUTED}, {"live", color::GOOD}, {"danger", color::DANGER}, {"error", color::DANGER}};
  auto it = colors.find(state);
  const char* c = it == colors.end() ? color::MUTED : it->second;
  if (p->text() != text) p->setText(text);
  QString style = QString("color: %1; border-color: %1;").arg(c);
  if (p->styleSheet() != style) p->setStyleSheet(style);
}

namespace {
constexpr int MICROSTEP_CHOICES[] = {1, 2, 4, 8, 16, 32, 64};
constexpr double RAMP_SLIDER_MAX = 10.0;   // FPS/s; MTV_RAMP_RATE allows up to 50 for the bench
QString microstep_label(int n) { return QString("1/%1  (%2 steps/rev)").arg(n).arg(200 * n); }

// Index edges of the last 5 s on a time line, newest at the right: accepted edges tall and green,
// rejected ones (bounce, noise) short and red.
class EdgeStrip : public QWidget {
 public:
  static constexpr double SPAN_S = 5.0;
  EdgeStrip() { setMinimumHeight(70); }
  void set_edges(const json& recent) {
    edges_.clear();
    for (const auto& e : recent) edges_.emplace_back(e.value("age_s", 0.0), e.value("accepted", false));
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    const QRect r = rect().adjusted(24, 1, -24, -16);   // room for the end labels
    p.fillRect(rect(), QColor(color::PANEL_ALT));
    p.setPen(QColor(color::BORDER));
    p.drawRect(r);
    p.setPen(QColor(color::MUTED));
    QFont f = p.font();
    f.setPointSize(8);
    p.setFont(f);
    for (int s = 0; s <= 5; ++s) {
      int x = r.right() - static_cast<int>(r.width() * s / SPAN_S);
      p.drawLine(x, r.bottom(), x, r.bottom() + 3);
      p.drawText(QRect(x - 30, r.bottom() + 2, 60, 14), Qt::AlignCenter, s == 0 ? QString("now") : QString("-%1 s").arg(s));
    }
    for (const auto& [age, accepted] : edges_) {
      if (age > SPAN_S) continue;
      int x = r.right() - static_cast<int>(r.width() * age / SPAN_S);
      p.setPen(QPen(QColor(accepted ? color::GOOD : color::DANGER), accepted ? 3 : 1));
      int top = accepted ? r.top() + 4 : r.top() + r.height() / 2;
      p.drawLine(x, top, x, r.bottom() - 1);
    }
  }

 private:
  std::vector<std::pair<double, bool>> edges_;
};
}  // namespace

void ConsoleWindow::sync_microsteps_box(int microsteps) {
  int idx = microsteps_box_->findData(microsteps);
  if (idx < 0) {
    microsteps_box_->addItem(microstep_label(microsteps), microsteps);
    idx = microsteps_box_->count() - 1;
  }
  if (idx == microsteps_box_->currentIndex()) return;
  syncing_ = true;
  microsteps_box_->setCurrentIndex(idx);
  syncing_ = false;
}

int ConsoleWindow::microsteps_selected() const { return microsteps_box_->currentData().toInt(); }

void ConsoleWindow::save_motor_settings() {
  std::string why;
  json patch = {{"microsteps", rt_.microsteps()}, {"ramp_rate_fps", rt_.motor().ramp_rate()}};
  if (!save_console_settings(studio_.root(), patch, &why)) log::warning("motor settings not saved: " + why);
}

QSlider* ConsoleWindow::slider(double from, double to, double step, std::function<void(double)> commit, std::function<void(double)> show) {
  auto* s = new QSlider(Qt::Horizontal);
  double scale = 1.0 / step;
  s->setRange(0, static_cast<int>(std::lround((to - from) * scale)));
  s->setSingleStep(1);
  s->setPageStep(std::max(1, static_cast<int>(std::lround((to - from) * scale / 20))));
  s->setMinimumWidth(220);
  s->setFocusPolicy(Qt::StrongFocus);
  sliders_[s] = {scale, from, std::move(commit), std::move(show)};
  connect(s, &QSlider::valueChanged, this, [this, s](int) {
    auto& info = sliders_[s];
    double v = slider_value(s);
    info.show(v);
    if (syncing_) return;
    if (!s->isSliderDown()) {   // keyboard, wheel or a click on the groove: commit right away
      info.released_at = now_s();
      info.commit(v);
    }
  });
  connect(s, &QSlider::sliderReleased, this, [this, s] {
    auto& info = sliders_[s];
    info.released_at = now_s();
    info.commit(slider_value(s));
  });
  return s;
}

double ConsoleWindow::slider_value(QSlider* s) const {
  const auto& info = sliders_.at(s);
  return info.from + s->value() / info.scale;
}

void ConsoleWindow::set_slider(QSlider* s, double v) {
  const auto& info = sliders_.at(s);
  syncing_ = true;
  s->setValue(static_cast<int>(std::lround((v - info.from) * info.scale)));
  syncing_ = false;
  info.show(slider_value(s));
}

bool ConsoleWindow::dragging(QSlider* s) const {
  if (s->isSliderDown()) return true;
  return now_s() - sliders_.at(s).released_at < SCALE_GRACE_S;
}

// ---------------------------------------------------------------- layout

void ConsoleWindow::build() {
  auto* central = new QWidget();
  setCentralWidget(central);
  auto* root = new QGridLayout(central);
  root->setContentsMargins(24, 16, 24, 10);
  root->setHorizontalSpacing(24);
  root->setVerticalSpacing(8);
  root->setColumnStretch(0, 3);
  root->setColumnStretch(1, 2);
  root->setRowStretch(1, 1);

  // Header
  auto* header = new QHBoxLayout();
  auto* titles = new QVBoxLayout();
  auto* eyebrow = new QLabel("EXPERIMENTAL MECHANICAL TELEVISION / 001");
  eyebrow->setObjectName("eyebrow");
  auto* title = new QLabel("Mechanical TV");
  title->setObjectName("title");
  titles->addWidget(eyebrow);
  titles->addWidget(title);
  header->addLayout(titles);
  header->addStretch(1);
  pill_hw_ = pill();
  pill_motor_ = pill();
  pill_sync_ = pill();
  pill_hdmi_ = pill();
  pill_log_ = pill();
  pill_log_->setText("LOG");
  pill_log_->setCursor(Qt::PointingHandCursor);
  pill_log_->installEventFilter(this);
  for (QLabel* p : {pill_hw_, pill_motor_, pill_sync_, pill_hdmi_, pill_log_}) header->addWidget(p);
  auto* b_debug = new QPushButton("Debug log (F12)");
  auto* b_full = new QPushButton("Full screen (F11)");
  auto* b_quit = new QPushButton("Quit");
  header->addSpacing(8);
  header->addWidget(b_debug);
  header->addWidget(b_full);
  header->addWidget(b_quit);
  connect(b_debug, &QPushButton::clicked, this, [this] { debug_->toggle(); });
  connect(b_full, &QPushButton::clicked, this, [this] { set_fullscreen(!fullscreen_); });
  connect(b_quit, &QPushButton::clicked, this, [this] { quit_console(); });
  root->addLayout(header, 0, 0, 1, 2);

  // Left: source, previews, HUD
  auto* left = new QGridLayout();
  left->setHorizontalSpacing(12);
  left->setVerticalSpacing(6);
  left->setColumnStretch(0, 1);
  left->setColumnStretch(1, 1);
  left->setRowStretch(2, 1);
  auto* src_panel = panel();
  auto* src = new QGridLayout(src_panel);
  src->setContentsMargins(12, 12, 12, 12);
  src->addWidget(heading("Video input"), 0, 0);
  source_box_ = new QComboBox();
  for (const auto& c : source_choices()) source_box_->addItem(c.label, QString(c.key));
  source_box_->setCurrentIndex(source_index("smpte_bars"));
  source_box_->setMinimumWidth(380);
  source_box_->setFocusPolicy(Qt::StrongFocus);
  src->addWidget(source_box_, 0, 1, Qt::AlignLeft);
  source_status_ = new QLabel("");
  source_status_->setObjectName("muted");
  src->addWidget(source_status_, 0, 2, Qt::AlignRight);
  src->setColumnStretch(1, 1);
  source_hint_ = new QLabel("");
  source_hint_->setObjectName("muted");
  source_hint_->setWordWrap(true);
  src->addWidget(source_hint_, 1, 0, 1, 3);
  connect(source_box_, QOverload<int>::of(&QComboBox::activated), this, [this](int i) { source_selected(i); });
  left->addWidget(src_panel, 0, 0, 1, 2);

  label_src_ = new QLabel(SRC_LABEL_FRAME);
  label_src_->setObjectName("eyebrow");
  left->addWidget(label_src_, 1, 0);
  auto* label_out = new QLabel("MECHANICAL TV IMAGE · AS SENT TO THE DISK");
  label_out->setObjectName("eyebrow");
  left->addWidget(label_out, 1, 1);
  view_src_ = new FrameView();
  view_out_ = new FrameView();
  left->addWidget(view_src_, 2, 0);
  left->addWidget(view_out_, 2, 1);

  auto* hud_panel = panel();
  auto* hud = new QGridLayout(hud_panel);
  hud->setContentsMargins(12, 12, 12, 4);
  const std::vector<std::pair<std::string, const char*>> items = {
      {"target", "TARGET SPEED"},   {"measured", "MEASURED SPEED"}, {"lock", "SYNC LOCK"},     {"jitter", "OPTO JITTER"},
      {"duty", "LED DUTY CYCLE"},   {"watchdog", "SAFETY WATCHDOG"}, {"capture", "CAPTURE"},   {"frames", "FRAMES TO DISK"},
      {"temp", "CPU TEMPERATURE"},  {"motor", "MOTOR DRIVER"},     {"sensor", "INDEX SENSOR"}, {"led", "LED OUTPUT"}};
  for (std::size_t i = 0; i < items.size(); ++i) {
    auto* cell = new QVBoxLayout();
    auto* name = new QLabel(items[i].second);
    name->setObjectName("muted");
    auto* value = new QLabel("—");
    value->setObjectName("value");
    value->setMinimumWidth(180);
    cell->addWidget(name);
    cell->addWidget(value);
    cell->setSpacing(2);
    hud->addLayout(cell, static_cast<int>(i / 4), static_cast<int>(i % 4));
    hud->setColumnStretch(static_cast<int>(i % 4), 1);
    hud_[items[i].first] = value;
  }
  left->addWidget(hud_panel, 3, 0, 1, 2);
  tabs_ = new QTabWidget();
  auto* console_page = new QWidget();
  auto* page = new QGridLayout(console_page);
  page->setContentsMargins(0, 10, 0, 0);
  page->setHorizontalSpacing(24);
  page->setColumnStretch(0, 3);
  page->setColumnStretch(1, 2);
  page->addLayout(left, 0, 0);

  // Right: controls
  auto* right = new QVBoxLayout();
  right->setSpacing(12);
  auto* ctl_panel = panel();
  auto* ctl = new QGridLayout(ctl_panel);
  ctl->setContentsMargins(14, 14, 14, 14);
  ctl->setColumnStretch(1, 1);
  ctl->addWidget(heading("Motor & LED"), 0, 0, 1, 3);
  auto* buttons = new QHBoxLayout();
  btn_start_ = new QPushButton(QStringLiteral("▶  Spin up motor"));
  btn_start_->setObjectName("primary");
  btn_stop_ = new QPushButton(QStringLiteral("■  Stop motor"));
  auto* btn_blank = new QPushButton(QStringLiteral("⚠  Emergency blank"));
  btn_blank->setObjectName("danger");
  buttons->addWidget(btn_start_);
  buttons->addWidget(btn_stop_);
  buttons->addWidget(btn_blank);
  buttons->addStretch(1);
  ctl->addLayout(buttons, 1, 0, 1, 3);
  connect(btn_start_, &QPushButton::clicked, this, [this] { action("start"); });
  connect(btn_stop_, &QPushButton::clicked, this, [this] { action("stop"); });
  connect(btn_blank, &QPushButton::clicked, this, [this] { action("emergency_blank"); });

  auto value_label = [] {
    auto* l = new QLabel("");
    l->setObjectName("muted");
    l->setMinimumWidth(150);
    return l;
  };
  ctl->addWidget(new QLabel("Speed"), 2, 0);
  speed_val_ = value_label();
  speed_ = slider(MIN_FPS, MAX_FPS, 0.1,
                  [this](double v) {
                    action("speed", {{"fps", v}});
                    if (test_speed_) set_slider(test_speed_, v);
                  },
                  [this](double v) { speed_val_->setText(QString("%1 FPS (%2 RPM)").arg(v, 0, 'f', 1).arg(std::lround(v * 60))); });
  ctl->addWidget(speed_, 2, 1);
  ctl->addWidget(speed_val_, 2, 2);
  set_slider(speed_, rt_.target_fps());

  ctl->addWidget(new QLabel("LED brightness"), 3, 0);
  bright_val_ = value_label();
  bright_ = slider(0, 100, 1, [this](double v) { action("calibration", {{"brightness", v / 100.0}}); },
                   [this](double v) { bright_val_->setText(QString("%1%").arg(std::lround(v))); });
  ctl->addWidget(bright_, 3, 1);
  ctl->addWidget(bright_val_, 3, 2);

  free_run_box_ = new QCheckBox("Free-run output: ignore disk sync, clock the LED from an internal timer (LED fires with no disk spinning)");
  free_run_box_->setChecked(rt_.free_run());
  ctl->addWidget(free_run_box_, 4, 0, 1, 3);
  connect(free_run_box_, &QCheckBox::clicked, this, [this](bool on) {
    free_run_changed_at_ = now_s();
    action("free_run", {{"enabled", on}});
  });
  ctl->addWidget(new QLabel("Spin-up ramp"), 5, 0);
  ramp_val_ = value_label();
  ramp_ = slider(MIN_RAMP_RATE, RAMP_SLIDER_MAX, 0.1, [this](double v) { action("ramp", {{"fps_per_s", v}}); },
                 [this](double v) { ramp_val_->setText(QString("%1 FPS/s").arg(v, 0, 'f', 1)); });
  ramp_->setToolTip("How fast the disk accelerates after the coils energise, in frames per second gained each second. "
                    "Lower it if a heavy disk slips or stalls while spinning up. Saved for the next start of the console.");
  ctl->addWidget(ramp_, 5, 1);
  ctl->addWidget(ramp_val_, 5, 2);
  set_slider(ramp_, std::min(RAMP_SLIDER_MAX, rt_.motor().ramp_rate()));

  ctl->addWidget(new QLabel("Microsteps"), 6, 0);
  auto* test_row = new QHBoxLayout();
  microsteps_box_ = new QComboBox();
  for (int n : MICROSTEP_CHOICES) microsteps_box_->addItem(microstep_label(n), n);
  microsteps_box_->setToolTip("The driver's microstep mode, set by its MS jumpers (A4988: MS1, MS2 and MS3 all high is 1/16). "
                              "The motor is commanded 200 x this many steps per revolution. Change it with the motor "
                              "stopped; it is saved for the next start of the console.");
  sync_microsteps_box(rt_.microsteps());
  connect(microsteps_box_, &QComboBox::currentIndexChanged, this, [this](int) {
    if (syncing_) return;
    microsteps_changed_at_ = now_s();
    action("microsteps", {{"value", microsteps_box_->currentData().toInt()}});
  });
  btn_test_ = new QPushButton("Check microstepping");
  btn_test_->setToolTip("Spins the disk slowly for up to 40 s and counts motor steps between two index pulses, "
                        "to confirm the driver's microstep jumpers match the Microsteps setting.");
  test_row->addWidget(microsteps_box_);
  test_row->addWidget(btn_test_);
  test_row->addStretch(1);
  ctl->addLayout(test_row, 6, 1, 1, 2);
  connect(btn_test_, &QPushButton::clicked, this, [this] { action("microstep_test"); });
  right->addWidget(ctl_panel);

  auto* cal_panel = panel();
  auto* cal = new QGridLayout(cal_panel);
  cal->setContentsMargins(14, 14, 14, 14);
  cal->setColumnStretch(1, 1);
  cal->addWidget(heading("Nipkow disc calibration"), 0, 0, 1, 3);
  cal->addWidget(new QLabel("Phase offset"), 1, 0);
  phase_val_ = value_label();
  phase_ = slider(0, 799, 1, [this](double v) { action("calibration", {{"phase_offset", static_cast<int>(std::lround(v))}}); },
                  [this](double v) {
                    int px = static_cast<int>(std::lround(v));
                    phase_val_->setText(QString("%1 px (%2°)").arg(px).arg(std::lround(px * 360.0 / 800)));
                  });
  cal->addWidget(phase_, 1, 1);
  cal->addWidget(phase_val_, 1, 2);
  cal->addWidget(new QLabel("Gamma"), 2, 0);
  gamma_val_ = value_label();
  gamma_ = slider(0.5, 3.0, 0.1, [this](double v) { action("calibration", {{"gamma", v}}); },
                  [this](double v) { gamma_val_->setText(QString::number(v, 'f', 1)); });
  cal->addWidget(gamma_, 2, 1);
  cal->addWidget(gamma_val_, 2, 2);
  cal->addWidget(new QLabel("Contrast"), 3, 0);
  contrast_val_ = value_label();
  contrast_ = slider(0.5, 2.5, 0.1, [this](double v) { action("calibration", {{"contrast", v}}); },
                     [this](double v) { contrast_val_->setText(QString::number(v, 'f', 1)); });
  cal->addWidget(contrast_, 3, 1);
  cal->addWidget(contrast_val_, 3, 2);
  auto* flips = new QHBoxLayout();
  inv_x_ = new QCheckBox("Invert horizontal (flip X)");
  inv_y_ = new QCheckBox("Invert vertical (flip Y)");
  flips->addWidget(inv_x_);
  flips->addSpacing(16);
  flips->addWidget(inv_y_);
  flips->addStretch(1);
  cal->addLayout(flips, 4, 0, 1, 3);
  connect(inv_x_, &QCheckBox::clicked, this, [this](bool on) { action("calibration", {{"invert_x", on}}); });
  connect(inv_y_, &QCheckBox::clicked, this, [this](bool on) { action("calibration", {{"invert_y", on}}); });
  right->addWidget(cal_panel);

  auto* lib_panel = panel();
  auto* lib = new QGridLayout(lib_panel);
  lib->setContentsMargins(14, 14, 14, 14);
  lib->addWidget(heading("Media library"), 0, 0);
  table_ = new QTableWidget(0, 3);
  table_->setHorizontalHeaderLabels({"NAME", "STATUS", "LENGTH"});
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  table_->verticalHeader()->setVisible(false);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setMinimumHeight(150);
  lib->addWidget(table_, 1, 0);
  lib->setRowStretch(1, 1);
  auto* lib_buttons = new QHBoxLayout();
  auto* b_add = new QPushButton(QStringLiteral("Add video file…"));
  auto* b_show = new QPushButton("Show on disk");
  b_show->setObjectName("primary");
  auto* b_del = new QPushButton("Delete");
  lib_buttons->addWidget(b_add);
  lib_buttons->addWidget(b_show);
  lib_buttons->addWidget(b_del);
  lib_buttons->addStretch(1);
  lib->addLayout(lib_buttons, 2, 0);
  connect(b_add, &QPushButton::clicked, this, [this] { add_file(); });
  connect(b_show, &QPushButton::clicked, this, [this] { select_clip(); });
  connect(b_del, &QPushButton::clicked, this, [this] { delete_clip(); });
  auto* fit_row = new QHBoxLayout();
  auto* fit_label = new QLabel("Import framing:");
  fit_label->setObjectName("muted");
  crop_box_ = new QCheckBox("Crop to fill (instead of letterbox)");
  fit_row->addWidget(fit_label);
  fit_row->addWidget(crop_box_);
  fit_row->addStretch(1);
  lib->addLayout(fit_row, 3, 0);
  lib_status_ = new QLabel("");
  lib_status_->setObjectName("muted");
  lib_status_->setWordWrap(true);
  lib->addWidget(lib_status_, 4, 0);
  right->addWidget(lib_panel, 1);
  page->addLayout(right, 0, 1);
  tabs_->addTab(console_page, "Console");
  tabs_->addTab(build_tests_page(), "Tests");
  tabs_->setFocusPolicy(Qt::NoFocus);
  tabs_->tabBar()->setFocusPolicy(Qt::NoFocus);
  root->addWidget(tabs_, 1, 0, 1, 2);

  // Footer
  auto* footer = new QHBoxLayout();
  notice_label_ = new QLabel("");
  notice_label_->setObjectName("notice");
  notice_label_->setWordWrap(true);
  footer_right_ = new QLabel("");
  footer_right_->setObjectName("eyebrow");
  footer->addWidget(notice_label_, 1);
  footer->addWidget(footer_right_);
  root->addLayout(footer, 2, 0, 1, 2);
  // Space is the start/stop key: buttons and check boxes must not take the keyboard focus and
  // swallow it (a focused button would re-fire, a focused check box would toggle free-run).
  for (auto* b : central->findChildren<QAbstractButton*>()) b->setFocusPolicy(Qt::NoFocus);
}

// ---------------------------------------------------------------- tests tab

QWidget* ConsoleWindow::build_tests_page() {
  auto* page = new QWidget();
  auto* grid = new QGridLayout(page);
  grid->setContentsMargins(0, 10, 0, 0);
  grid->setHorizontalSpacing(24);
  grid->setVerticalSpacing(12);
  grid->setColumnStretch(0, 2);
  grid->setColumnStretch(1, 3);
  auto value_label = [] {
    auto* l = new QLabel("");
    l->setObjectName("muted");
    l->setMinimumWidth(150);
    return l;
  };
  auto note = [](const QString& text) {
    auto* l = new QLabel(text);
    l->setObjectName("muted");
    l->setWordWrap(true);
    return l;
  };

  // Motor only
  auto* motor_panel = panel();
  auto* m = new QGridLayout(motor_panel);
  m->setContentsMargins(14, 14, 14, 14);
  m->setColumnStretch(1, 1);
  m->addWidget(heading("Motor only"), 0, 0, 1, 3);
  m->addWidget(note("Spins the motor at the speed below with the LED kept off and no stall checks: the motor runs "
                    "whatever the sensor sees. The index sensor is still read, so the panel on the right shows how it "
                    "picks up the notch."), 1, 0, 1, 3);
  auto* mb = new QHBoxLayout();
  btn_motor_test_ = new QPushButton(QStringLiteral("▶  Run motor only"));
  btn_motor_test_->setObjectName("primary");
  btn_motor_test_stop_ = new QPushButton(QStringLiteral("■  Stop motor"));
  mb->addWidget(btn_motor_test_);
  mb->addWidget(btn_motor_test_stop_);
  mb->addStretch(1);
  m->addLayout(mb, 2, 0, 1, 3);
  connect(btn_motor_test_, &QPushButton::clicked, this, [this] { action("motor_test"); });
  connect(btn_motor_test_stop_, &QPushButton::clicked, this, [this] { action("stop"); });
  m->addWidget(new QLabel("Speed"), 3, 0);
  test_speed_val_ = value_label();
  test_speed_ = slider(MIN_FPS, MAX_FPS, 0.1,
                       [this](double v) {
                         action("speed", {{"fps", v}});
                         set_slider(speed_, v);
                       },
                       [this](double v) {
                         test_speed_val_->setText(QString("%1 FPS (%2 RPM)").arg(v, 0, 'f', 1).arg(std::lround(v * 60)));
                       });
  m->addWidget(test_speed_, 3, 1);
  m->addWidget(test_speed_val_, 3, 2);
  set_slider(test_speed_, rt_.target_fps());
  test_motor_status_ = note("");
  m->addWidget(test_motor_status_, 4, 0, 1, 3);
  grid->addWidget(motor_panel, 0, 0);

  // LED only
  auto* led_panel = panel();
  auto* l = new QGridLayout(led_panel);
  l->setContentsMargins(14, 14, 14, 14);
  l->setColumnStretch(1, 1);
  l->addWidget(heading("LED only"), 0, 0, 1, 3);
  l->addWidget(note(log::format("Lights the LED on its own, steady or blinking once a second, with the motor stopped "
                                "or in the motor-only test. It switches itself off after %.0f s.",
                                RealtimeController::LED_TEST_MAX_S).c_str()),
               1, 0, 1, 3);
  auto* lb = new QHBoxLayout();
  btn_led_steady_ = new QPushButton("Steady");
  btn_led_blink_ = new QPushButton("Blink");
  btn_led_off_ = new QPushButton("LED off");
  btn_led_off_->setObjectName("danger");
  lb->addWidget(btn_led_steady_);
  lb->addWidget(btn_led_blink_);
  lb->addWidget(btn_led_off_);
  lb->addStretch(1);
  l->addLayout(lb, 2, 0, 1, 3);
  auto led_level = [this] { return slider_value(test_led_level_) / 100.0; };
  connect(btn_led_steady_, &QPushButton::clicked, this, [this, led_level] { action("led_test", {{"mode", "steady"}, {"level", led_level()}}); });
  connect(btn_led_blink_, &QPushButton::clicked, this, [this, led_level] { action("led_test", {{"mode", "blink"}, {"level", led_level()}}); });
  connect(btn_led_off_, &QPushButton::clicked, this, [this] { action("led_test", {{"mode", "off"}}); });
  l->addWidget(new QLabel("Level"), 3, 0);
  test_led_level_val_ = value_label();
  test_led_level_ = slider(1, 100, 1,
                           [this](double v) {
                             if (rt_.led_test() != "off") action("led_test", {{"mode", rt_.led_test()}, {"level", v / 100.0}});
                           },
                           [this](double v) { test_led_level_val_->setText(QString("%1%").arg(std::lround(v))); });
  l->addWidget(test_led_level_, 3, 1);
  l->addWidget(test_led_level_val_, 3, 2);
  set_slider(test_led_level_, 50);
  test_led_status_ = note("");
  l->addWidget(test_led_status_, 4, 0, 1, 3);
  grid->addWidget(led_panel, 1, 0);
  grid->setRowStretch(2, 1);

  // Index sensor
  auto* sensor_panel = panel();
  auto* s = new QGridLayout(sensor_panel);
  s->setContentsMargins(14, 14, 14, 14);
  s->setColumnStretch(0, 1);
  s->setColumnStretch(1, 1);
  s->addWidget(heading(QString("Index sensor (GPIO %1)").arg(rt_.sensor().pin)), 0, 0, 1, 2);
  sensor_watch_box_ = new QCheckBox("Watch the sensor with the motor stopped (turn the disk by hand)");
  connect(sensor_watch_box_, &QCheckBox::clicked, this, [this](bool on) { action("sensor_watch", {{"on", on}}); });
  s->addWidget(sensor_watch_box_, 1, 0, 1, 2);
  test_sensor_level_ = new QLabel(QStringLiteral("—"));
  test_sensor_level_->setObjectName("big");
  s->addWidget(test_sensor_level_, 2, 0);
  test_sensor_counts_ = note("");
  s->addWidget(test_sensor_counts_, 2, 1);
  auto* strip_label = new QLabel("EDGES, LAST 5 S · TALL GREEN = ACCEPTED NOTCH · SHORT RED = REJECTED (BOUNCE, NOISE)");
  strip_label->setObjectName("eyebrow");
  s->addWidget(strip_label, 3, 0, 1, 2);
  edge_strip_ = new EdgeStrip();
  s->addWidget(edge_strip_, 4, 0, 1, 2);
  test_sensor_hint_ = new QLabel("");
  test_sensor_hint_->setObjectName("notice");
  test_sensor_hint_->setWordWrap(true);
  s->addWidget(test_sensor_hint_, 5, 0, 1, 2);
  auto make_table = [](const QStringList& headers) {
    auto* t = new QTableWidget(0, headers.size());
    t->setHorizontalHeaderLabels(headers);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    t->verticalHeader()->setVisible(false);
    t->setSelectionMode(QAbstractItemView::NoSelection);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setFocusPolicy(Qt::NoFocus);
    t->setMinimumHeight(180);
    return t;
  };
  auto* el = new QLabel("LAST EDGES (NEWEST FIRST)");
  el->setObjectName("eyebrow");
  auto* rl = new QLabel("REVOLUTIONS BETWEEN ACCEPTED NOTCHES");
  rl->setObjectName("eyebrow");
  s->addWidget(el, 6, 0);
  s->addWidget(rl, 6, 1);
  edges_table_ = make_table({"AGO", "GAP", "MOTOR STEPS", "RESULT"});
  revs_table_ = make_table({"PERIOD", "SPEED", "STEPS / REV", "EXPECTED"});
  s->addWidget(edges_table_, 7, 0);
  s->addWidget(revs_table_, 7, 1);
  s->setRowStretch(7, 1);
  grid->addWidget(sensor_panel, 0, 1, 3, 1);
  return page;
}

QString ConsoleWindow::test_text(const std::string& key) const {
  if (key == "motor") return test_motor_status_->text();
  if (key == "led") return test_led_status_->text();
  if (key == "level") return test_sensor_level_->text();
  if (key == "counts") return test_sensor_counts_->text();
  if (key == "hint") return test_sensor_hint_->text();
  return {};
}

void ConsoleWindow::update_tests(const json& rt) {
  const json& b = rt.contains("bench") ? rt["bench"] : json::object();
  const std::string state = rt.value("state", "idle");
  const bool motor_test = b.value("motor_test", false);
  const bool normal_run = state != "idle" && state != "error" && state != "testing" && !motor_test;
  const bool free_run = rt.value("free_run", false);
  const int spr = 200 * rt.value("microsteps", 16);
  btn_motor_test_->setEnabled(state == "idle" || state == "error");
  btn_motor_test_->setEnabled(btn_motor_test_->isEnabled() && !free_run);
  btn_motor_test_stop_->setEnabled(state != "idle" && state != "error");
  const json motor = rt.contains("motor") ? rt["motor"] : json::object();
  if (motor_test) {
    test_motor_status_->setText(log::format("RUNNING · %.2f FPS commanded (%.0f RPM) · 1/%d, %d steps per revolution · LED off",
                                            motor.value("current_fps", 0.0), motor.value("current_fps", 0.0) * 60,
                                            spr / 200, spr).c_str());
  } else if (normal_run) {
    test_motor_status_->setText("The motor is in the normal run (started from the Console tab). Stop it to use the motor-only test.");
  } else if (free_run) {
    test_motor_status_->setText("Free-run is on (Console tab): it lights the LED, so the motor-only test is unavailable.");
  } else {
    test_motor_status_->setText(log::format("Stopped · coils released · microstepping 1/%d (set on the Console tab)", spr / 200).c_str());
  }
  if (!dragging(test_speed_) && std::fabs(slider_value(test_speed_) - rt.value("target_fps", 10.0)) > 0.05)
    set_slider(test_speed_, rt.value("target_fps", 10.0));

  const std::string led = b.value("led_test", "off");
  const bool led_allowed = !normal_run && !free_run;
  btn_led_steady_->setEnabled(led_allowed);
  btn_led_blink_->setEnabled(led_allowed);
  if (led != "off" && !dragging(test_led_level_)) {
    const double pct = b.value("led_test_level", 0.5) * 100;
    if (std::fabs(slider_value(test_led_level_) - pct) > 0.5) set_slider(test_led_level_, pct);
  }
  if (led != "off") {
    test_led_status_->setText(log::format("LED %s at %.0f%% · switches off in %.0f s", led == "steady" ? "STEADY" : "BLINKING",
                                          b.value("led_test_level", 0.0) * 100, b.value("led_test_left_s", 0.0)).c_str());
  } else {
    test_led_status_->setText(led_allowed ? "LED off." : "LED test unavailable while the normal run or free-run drives the LED.");
  }

  const bool watch = b.value("sensor_watch", false);
  if (sensor_watch_box_->isChecked() != watch) sensor_watch_box_->setChecked(watch);
  const int level = b.value("sensor_level", -1);
  const bool open = b.value("sensor_open", false);
  QString level_text = !open ? QString("SENSOR CLOSED") : level < 0 ? QString("LEVEL UNKNOWN") : level ? QString("LINE HIGH") : QString("LINE LOW");
  if (test_sensor_level_->text() != level_text) test_sensor_level_->setText(level_text);
  test_sensor_level_->setStyleSheet(QString("color: %1;").arg(!open || level < 0 ? color::MUTED : level ? color::ACCENT : color::TEXT));
  const long total = b.value("edges_total", 0L), shown = b.value("edges_shown", 0L), accepted = b.value("edges_accepted", 0L);
  const long per_s = b.value("edges_last_second", 0L);
  test_sensor_counts_->setText(log::format("%ld edges seen · %ld of the last %ld accepted · %ld in the last second",
                                           total, accepted, shown, per_s).c_str());
  static_cast<EdgeStrip*>(edge_strip_)->set_edges(b.value("recent", json::array()));

  QString hint;
  const double fps = motor.value("current_fps", 0.0);
  if (!open) {
    hint = "Start the motor-only test, or tick the box above, to read the sensor.";
  } else if (per_s > 20 && (fps <= 0 || per_s > 3 * fps + 5)) {
    hint = "Far more edges than revolutions: the sensor output is flickering. Usually the notch edge sits in the beam "
           "(turn the disk a little) or the signal picks up noise from the motor wiring.";
  } else if (motor_test && fps > 0.5 && total == 0) {
    hint = "No edges while the motor turns: check that the disk is fitted, the notch passes through the sensor slot, "
           "and the sensor wiring.";
  } else if (watch && !motor_test && total == 0) {
    hint = "Turn the disk by hand: each pass of the notch should add one tall green mark.";
  }
  if (test_sensor_hint_->text() != hint) test_sensor_hint_->setText(hint);

  auto fill = [](QTableWidget* t, const json& rows, const std::function<QStringList(const json&)>& cells) {
    t->setRowCount(static_cast<int>(rows.size()));
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
      QStringList c = cells(rows[i]);
      for (int j = 0; j < c.size(); ++j) {
        auto* item = t->item(i, j);
        if (!item) t->setItem(i, j, item = new QTableWidgetItem());
        if (item->text() != c[j]) item->setText(c[j]);
      }
    }
  };
  fill(edges_table_, b.value("last_edges", json::array()), [](const json& e) {
    QString dt = e.contains("dt_ms") ? QString("%1 ms").arg(e["dt_ms"].get<double>(), 0, 'f', 1) : QString("—");
    QString st = e.contains("steps") ? QString::number(e["steps"].get<double>(), 'f', 0) : QString("—");
    return QStringList{QString("%1 s").arg(e.value("age_s", 0.0), 0, 'f', 1), dt, st,
                       e.value("accepted", false) ? QString("notch") : QString("rejected")};
  });
  fill(revs_table_, b.value("revolutions", json::array()), [spr](const json& r) {
    const double steps = r.value("steps", 0.0);
    QString expected = steps <= 0 ? QString("motor stopped")
                       : std::fabs(steps - spr) / spr <= 0.08 ? QString("%1 ✓").arg(spr)
                                                               : QString("%1 ✗ (%2x)").arg(spr).arg(steps / spr, 0, 'f', 2);
    return QStringList{QString("%1 ms").arg(r.value("period_ms", 0.0), 0, 'f', 1),
                       QString("%1 FPS").arg(r.value("fps", 0.0), 0, 'f', 2), QString::number(steps, 'f', 0), expected};
  });
}

// ---------------------------------------------------------------- actions (worker thread)

void ConsoleWindow::action(const std::string& name, json params) {
  if (name == "led_test" && params.value("mode", "") == "off") {
    // Like stop: switching the LED off never waits behind a slow job.
    log::info("action led_test off");
    rt_.set_led_test("off");
    return;
  }
  if (name == "emergency_blank" || name == "stop") {
    // Safety actions never wait behind a slow job (a microstep check, a source switch, an import):
    // they run now, on this thread, and queued work that would restart things is dropped.
    {
      std::lock_guard<std::mutex> lock(jobs_lock_);
      if (name == "emergency_blank") jobs_.clear();
      else jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
                                      [](const auto& j) { return j.first == "start" || j.first == "motor_test"; }),
                       jobs_.end());
    }
    log::info("action " + name);
    try {
      if (name == "emergency_blank") {
        rt_.emergency_blank();
        notice("Emergency blank: LED off, motor stopped, free-run off.");
      } else {
        rt_.stop();
      }
    } catch (const std::exception& exc) {
      notice(name + ": " + exc.what());
      log::error("action " + name + ": " + exc.what());
    }
    return;
  }
  {
    std::lock_guard<std::mutex> lock(jobs_lock_);
    jobs_.emplace_back(name, std::move(params));
  }
  jobs_cv_.notify_all();
}

void ConsoleWindow::wait_idle() {
  for (int i = 0; i < 3000; ++i) {
    {
      std::lock_guard<std::mutex> lock(jobs_lock_);
      if (jobs_.empty() && !busy_) return;
    }
    sleep_s(0.005);
  }
}

void ConsoleWindow::job_loop() {
  while (true) {
    std::pair<std::string, json> job;
    {
      std::unique_lock<std::mutex> lock(jobs_lock_);
      jobs_cv_.wait(lock, [this] { return stop_worker_ || !jobs_.empty(); });
      if (stop_worker_ && jobs_.empty()) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
      busy_ = true;
    }
    const std::string& name = job.first;
    const json& p = job.second;
    log::info("action " + name + (p.empty() ? std::string("") : " " + p.dump()));
    try {
      if (name == "start") {
        rt_.start();
      } else if (name == "motor_test") {
        rt_.start_motor_test();
        notice("Motor-only test: the motor spins with the LED off and no stall checks. Watch the index sensor panel.");
      } else if (name == "sensor_watch") {
        rt_.set_sensor_watch(p.at("on").get<bool>());
      } else if (name == "led_test") {
        rt_.set_led_test(p.at("mode").get<std::string>(), p.value("level", 0.5));
      } else if (name == "speed") {
        rt_.set_target_fps(p.at("fps").get<double>());
      } else if (name == "free_run") {
        bool on = p.at("enabled").get<bool>();
        rt_.set_free_run(on);
        notice(on ? "Free-run output ON: internal clock drives the LED; the disk sync and stall interlock are bypassed."
                  : "Free-run output off: the index sensor clocks the LED again.");
      } else if (name == "calibration") {
        Calibration c;
        if (p.contains("phase_offset")) c.phase_offset = p["phase_offset"].get<int>();
        if (p.contains("invert_x")) c.invert_x = p["invert_x"].get<bool>();
        if (p.contains("invert_y")) c.invert_y = p["invert_y"].get<bool>();
        if (p.contains("gamma")) c.gamma = p["gamma"].get<double>();
        if (p.contains("brightness")) c.brightness = p["brightness"].get<double>();
        if (p.contains("contrast")) c.contrast = p["contrast"].get<double>();
        rt_.set_calibration(c);
      } else if (name == "source") {
        std::string mode = p.at("mode");
        if (mode == "pattern") {
          rt_.set_source_mode("pattern");
          rt_.set_pattern(p.at("pattern"));
        } else if (mode == "library") {
          rt_.set_source_mode("library", studio_.clip_frames_path(p.at("id")));
        } else {
          rt_.set_source_mode("hdmi");
        }
      } else if (name == "import") {
        std::string path = p.at("path");
        std::string ident = studio_.import_file(path, p.at("fit"));
        notice("Queued " + QFileInfo(QString::fromStdString(path)).fileName().toStdString() + " for conversion (" + ident.substr(0, 8) + ").");
      } else if (name == "delete") {
        studio_.library().remove(p.at("id"));
        notice("Clip deleted.");
      } else if (name == "microsteps") {
        int n = p.at("value").get<int>();
        rt_.set_microsteps(n);
        save_motor_settings();
        notice(log::format("Microstepping set to 1/%d (%d steps per revolution). Run Check microstepping to confirm the "
                           "driver's MS jumpers agree.", n, 200 * n));
      } else if (name == "ramp") {
        rt_.set_ramp_rate(p.at("fps_per_s").get<double>());
        save_motor_settings();
      } else if (name == "microstep_test") {
        notice("Microstep check running: the disk turns slowly for up to 40 s…");
        json r = rt_.run_microstep_test();
        notice(r.value("message", ""));
      } else if (name == "close") {
        studio_.close();
      }
    } catch (const std::exception& exc) {
      if (name == "microsteps") microsteps_changed_at_ = 0;   // the selector snaps back to the real setting
      notice(name + ": " + exc.what());
      log::error("action " + name + ": " + exc.what());
    }
    std::lock_guard<std::mutex> lock(jobs_lock_);
    busy_ = false;
  }
}

void ConsoleWindow::notice(const std::string& text) {
  std::lock_guard<std::mutex> lock(notices_lock_);
  notices_.push_back(text);
}

// ---------------------------------------------------------------- UI events

void ConsoleWindow::keyPressEvent(QKeyEvent* e) {
  const int k = e->key();
  if (k == Qt::Key_F11) set_fullscreen(!fullscreen_);
  else if (k == Qt::Key_Escape) set_fullscreen(false);
  else if (k == Qt::Key_F12) debug_->toggle();
  else if (k == Qt::Key_Q && (e->modifiers() & Qt::ControlModifier)) quit_console();
  else if (k == Qt::Key_Space) {
    QWidget* f = QApplication::focusWidget();
    if (qobject_cast<QComboBox*>(f) || qobject_cast<QSlider*>(f)) return QMainWindow::keyPressEvent(e);
    const bool moving = rt_.state() != "idle" && rt_.state() != "error";
    action(moving ? "stop" : tests_tab_shown() ? "motor_test" : "start");
  } else {
    QMainWindow::keyPressEvent(e);
  }
}

bool ConsoleWindow::eventFilter(QObject* obj, QEvent* e) {
  if (obj == pill_log_ && e->type() == QEvent::MouseButtonRelease) {
    debug_->toggle();
    return true;
  }
  return QMainWindow::eventFilter(obj, e);
}

void ConsoleWindow::closeEvent(QCloseEvent* e) {
  if (!closing_) quit_console();
  e->accept();
}

void ConsoleWindow::source_selected(int index) {
  std::string key = source_box_->itemData(index).toString().toStdString();
  source_changed_at_ = now_s();
  if (key == "hdmi") {
    action("source", {{"mode", "hdmi"}});
  } else if (key == "library") {
    if (selected_clip_.empty()) {
      notice("Select a ready clip in the media library first.");
      return;
    }
    action("source", {{"mode", "library"}, {"id", selected_clip_}});
  } else {
    action("source", {{"mode", "pattern"}, {"pattern", key}});
  }
}

void ConsoleWindow::add_file() {
  QString path = QFileDialog::getOpenFileName(this, "Add a video to the library", QString::fromStdString(sysinfo::home_dir()),
                                              "Video files (*.mp4 *.m4v *.mov *.mkv *.webm *.avi);;All files (*)");
  if (!path.isEmpty()) action("import", {{"path", path.toStdString()}, {"fit", crop_box_->isChecked() ? "crop" : "fit"}});
}

std::string ConsoleWindow::selected_id() const {
  auto rows = table_->selectionModel()->selectedRows();
  if (rows.isEmpty()) return "";
  return table_->item(rows.first().row(), 0)->data(Qt::UserRole).toString().toStdString();
}

void ConsoleWindow::select_clip() {
  std::string ident = selected_id();
  if (ident.empty()) {
    notice("Choose a clip in the list first.");
    return;
  }
  try {
    studio_.clip_frames_path(ident);
  } catch (const std::exception& exc) {
    notice(exc.what());
    return;
  }
  selected_clip_ = ident;
  source_box_->setCurrentIndex(source_index("library"));
  source_changed_at_ = now_s();
  action("source", {{"mode", "library"}, {"id", ident}});
}

void ConsoleWindow::delete_clip() {
  std::string ident = selected_id();
  if (ident.empty()) {
    notice("Choose a clip in the list first.");
    return;
  }
  auto rows = table_->selectionModel()->selectedRows();
  QString name = table_->item(rows.first().row(), 0)->text();
  if (QMessageBox::question(this, "Delete clip", QString("Delete “%1” and its prepared files?").arg(name)) != QMessageBox::Yes) return;
  if (selected_clip_ == ident) {
    selected_clip_.clear();
    if (pipe_.source_mode() == "library") {
      source_box_->setCurrentIndex(source_index("smpte_bars"));
      action("source", {{"mode", "pattern"}, {"pattern", "smpte_bars"}});
    }
  }
  action("delete", {{"id", ident}});
}

void ConsoleWindow::set_fullscreen(bool on) {
  fullscreen_ = on;
  if (on) showFullScreen();
  else if (isFullScreen()) showNormal();
}

void ConsoleWindow::quit_console() {
  if (closing_) return;
  closing_ = true;
  timer_.stop();
  log::info("console closing: stopping motor, blanking LED");
  notice_label_->setText("Shutting down: stopping motor and blanking LED…");
  QApplication::processEvents();
  rt_.emergency_blank();   // LED off and motor stopped first; also cancels a microstep check
  {
    std::lock_guard<std::mutex> lock(jobs_lock_);
    jobs_.clear();         // nothing queued may run after the engine is closed
    stop_worker_ = true;
  }
  jobs_cv_.notify_all();
  if (worker_.joinable()) worker_.join();
  studio_.close();
  debug_->hide();
  close();
  QApplication::quit();
}

// ---------------------------------------------------------------- periodic refresh

void ConsoleWindow::paint_frames() {
  PreviewPtr preview = pipe_.get_source_preview();
  FramePtr src = pipe_.get_latest_frame();
  FramePtr out = rt_.get_preview_frame();
  if (preview != painted_preview_ || (!preview && src != painted_src_)) {
    painted_preview_ = preview;
    painted_src_ = src;
    if (preview) {
      QImage img(preview->data(), PREVIEW_W, PREVIEW_H, PREVIEW_W * 3, QImage::Format_RGB888);
      view_src_->set_image(img.copy());
    } else {
      QImage img(src->data(), WIDTH, HEIGHT, WIDTH, QImage::Format_Grayscale8);
      view_src_->set_image(img.copy());
    }
  }
  if ((preview != nullptr) != showing_preview_) {
    showing_preview_ = preview != nullptr;
    label_src_->setText(showing_preview_ ? SRC_LABEL_LIVE : SRC_LABEL_FRAME);
  }
  if (out != painted_out_) {
    painted_out_ = out;
    Frame shown = pipe_.reconstruct_display_preview(pipe_.serialize_nipkow_stream(*out));
    QImage img(shown.data(), WIDTH, HEIGHT, WIDTH, QImage::Format_Grayscale8);
    view_out_->set_image(img.copy());
  }
}

void ConsoleWindow::tick() {
  if (closing_) return;
  {
    std::lock_guard<std::mutex> lock(notices_lock_);
    if (!notices_.empty()) {
      notice_label_->setText(QString::fromStdString(notices_.back()));
      notices_.clear();
    }
  }
  try {
    paint_frames();
    if (tick_count_++ % HUD_EVERY == 0) {
      json status = rt_.get_status();
      update_hud(status);
      update_tests(status);
      std::string st = status.value("state", "idle");
      if ((st == "locked" || st == "running" || st == "ramping" || st == "motor test") && now_s() - last_summary_at_ >= 5.0) {
        last_summary_at_ = now_s();
        const json& sn = status["sensor"];
        log::info(log::format("index: %.2f FPS, period %.3f ms, jitter %.3f ms (rms %.3f), %.0f steps/rev, %s, LED lead-in %.0f us, late pixels %ld",
                              sn.value("measured_fps", 0.0), sn.value("period_ms", 0.0), sn.value("jitter_ms", 0.0),
                              sn.value("jitter_rms_ms", 0.0), sn.value("steps_per_rev_measured", 0.0),
                              sn.value("sync_locked", false) ? "locked" : "unlocked", status["realtime"].value("lead_in_us", 0.0),
                              status["realtime"].value("late_pixels", 0L)));
      }
      std::string err = status.value("error", "");
      if (err.empty()) err = status.value("warning", "");   // index problem while the motor keeps running
      if (!err.empty() && err != last_error_) notice_label_->setText(QString::fromStdString(err));
      last_error_ = err;
      if (now_s() - last_library_at_ >= LIBRARY_REFRESH_S) {
        last_library_at_ = now_s();
        refresh_library();
      }
    }
  } catch (const std::exception& exc) {
    notice_label_->setText(QString("UI error: %1").arg(exc.what()));
    log::error(std::string("ui refresh: ") + exc.what());
  }
  drain_log();
}

void ConsoleWindow::drain_log() {
  std::deque<log::Record> batch;
  {
    std::lock_guard<std::mutex> lock(log_lock_);
    for (int i = 0; i < 200 && !log_queue_.empty(); ++i) {
      batch.push_back(std::move(log_queue_.front()));
      log_queue_.pop_front();
    }
  }
  if (batch.empty()) return;
  for (const auto& r : batch) {
    if (r.level >= log::Level::Error) ++error_count_;
    debug_->append(r);
  }
  if (error_count_) set_pill(pill_log_, QString("LOG: %1 ERROR%2").arg(error_count_).arg(error_count_ != 1 ? "S" : ""), "danger");
  else set_pill(pill_log_, "LOG", "idle");
}

void ConsoleWindow::update_hud(const json& rt) {
  const std::string state = rt.value("state", "idle");
  const bool locked = rt.value("sync_locked", false);
  const bool free_run = rt.value("free_run", false);
  const bool hw = studio_.hardware_enabled();
  set_pill(pill_hw_, hw ? "REALTIME HARDWARE" : "SIMULATION", hw ? "live" : "danger");
  QString st = QString::fromStdString(state).toUpper();
  set_pill(pill_motor_, "MOTOR: " + (state == "error" ? QString("STALLED") : st), state == "motor test" ? "testing" : state);
  if (free_run) set_pill(pill_sync_, "SYNC: FREE-RUN", "free-run");
  else set_pill(pill_sync_, QString("SYNC: %1").arg(locked ? "LOCKED" : "UNLOCKED"), locked ? "locked" : (state == "idle" || state == "error" ? "idle" : "ramping"));
  if (free_run_box_->isChecked() != free_run && now_s() - free_run_changed_at_ > 3.0) free_run_box_->setChecked(free_run);
  const bool moving = state != "idle" && state != "error";
  btn_start_->setEnabled(!moving);
  btn_stop_->setEnabled(moving);
  btn_test_->setEnabled(!moving && !free_run);
  microsteps_box_->setEnabled(!moving);
  if (now_s() - microsteps_changed_at_ > 3.0) sync_microsteps_box(rt.value("microsteps", rt_.microsteps()));
  if (!dragging(ramp_)) {
    double ramp = std::min(RAMP_SLIDER_MAX, rt.value("ramp_rate_fps", rt_.motor().ramp_rate()));
    if (std::fabs(ramp - slider_value(ramp_)) > 0.05) set_slider(ramp_, ramp);
  }

  hud_["target"]->setText(QString("%1 RPM (%2 FPS)").arg(std::lround(rt.value("target_rpm", 0.0))).arg(rt.value("target_fps", 0.0), 0, 'f', 1));
  hud_["measured"]->setText(QString("%1 RPM").arg(rt.value("measured_rpm", 0.0), 0, 'f', 1));
  const std::string warning = rt.value("warning", std::string());
  const bool no_notch = warning.rfind("No index pulse", 0) == 0;
  const bool slipping = !warning.empty() && !no_notch;
  QString lock_text = free_run ? "FREE-RUN CLOCK" : state == "testing" ? "MICROSTEP CHECK" : state == "error" ? "STALLED"
                    : state == "motor test" ? "MOTOR TEST (LED OFF)"
                    : locked ? "LOCKED (±3%)" : state == "idle" ? "STANDBY" : no_notch ? "NO NOTCH (LED OFF)"
                    : slipping ? "SLIPPING (SENSOR-TIMED)" : "ACQUIRING";
  hud_["lock"]->setText(lock_text);
  const json& sensor = rt["sensor"];
  hud_["jitter"]->setText(QString("%1 ms").arg(rt.value("jitter_ms", 0.0), 0, 'f', 3));
  const json& led = rt["led"];
  hud_["duty"]->setText(QString("%1%").arg(led.value("average_duty_cycle", 0.0) * 100, 0, 'f', 1));
  hud_["watchdog"]->setText(led.value("safety_tripped", false) ? "TRIPPED (OFF)" : free_run ? "BYPASSED (FREE-RUN)" : state == "idle" ? "ARMED" : "ACTIVE");
  hud_["frames"]->setText(QString::number(rt.value("frame_counter", 0L)));
  auto temp = sysinfo::temperature_c();
  hud_["temp"]->setText(temp ? QString("%1 °C").arg(*temp, 0, 'f', 1) : QString("n/a"));
  const json& motor = rt["motor"];
  json drv = motor.contains("driver") && motor["driver"].is_object() ? motor["driver"] : json::object();
  std::string name = drv.value("driver", "?");
  for (const char* cut : {"Driver", "Adafruit"}) {
    auto pos = name.find(cut);
    if (pos != std::string::npos) name.erase(pos, std::strlen(cut));
  }
  std::string mode = drv.value("mode", "");
  hud_["motor"]->setText(fit_text(name + (mode.empty() ? "" : " · " + mode) + " · " + log::format("%.0f", motor.value("current_rpm", 0.0)) + " RPM"));
  hud_["sensor"]->setText(fit_text(sensor.value("backend", "?") + " · GPIO " + std::to_string(sensor.value("pin", 0)) + " · " +
                                   std::to_string(sensor.value("pulse_count", 0L)) + " pulses"));
  hud_["led"]->setText(fit_text(led.value("backend", "?") + " · " + (led.value("is_blanked", true) ? "BLANK" : "ON") + " · " +
                                std::to_string(led.value("frames_rendered", 0L)) + " frames"));
  sync_source_widgets(rt["pipeline"]);
  sync_calibration_widgets();
  if (!dragging(speed_) && std::fabs(slider_value(speed_) - rt.value("target_fps", 10.0)) > 0.05) set_slider(speed_, rt.value("target_fps", 10.0));
  footer_right_->setText(QString("v%1 · data %2 · Esc leaves full screen · Ctrl+Q quits").arg(VERSION, QString::fromStdString(studio_.root())));
}

void ConsoleWindow::sync_calibration_widgets() {
  if (!dragging(bright_) && std::lround(slider_value(bright_)) != std::lround(pipe_.brightness() * 100)) set_slider(bright_, std::round(pipe_.brightness() * 100));
  if (!dragging(phase_) && std::lround(slider_value(phase_)) != pipe_.phase_offset()) set_slider(phase_, pipe_.phase_offset());
  if (!dragging(gamma_) && std::fabs(slider_value(gamma_) - pipe_.gamma()) > 0.05) set_slider(gamma_, pipe_.gamma());
  if (!dragging(contrast_) && std::fabs(slider_value(contrast_) - pipe_.contrast()) > 0.05) set_slider(contrast_, pipe_.contrast());
  if (inv_x_->isChecked() != pipe_.invert_x()) inv_x_->setChecked(pipe_.invert_x());
  if (inv_y_->isChecked() != pipe_.invert_y()) inv_y_->setChecked(pipe_.invert_y());
}

void ConsoleWindow::sync_source_widgets(const json& pipe) {
  const json details = pipe.contains("source_details") && pipe["source_details"].is_object() ? pipe["source_details"] : json::object();
  const std::string mode = pipe.value("source_mode", "pattern");
  if (now_s() - source_changed_at_ > 2.0 && !source_box_->view()->isVisible()) {
    std::string wanted = mode == "hdmi" ? "hdmi" : mode == "library" ? "library" : details.value("pattern", "smpte_bars");
    int idx = source_index(wanted);
    if (source_box_->currentIndex() != idx) source_box_->setCurrentIndex(idx);
  }
  std::string text, hint, colour;
  if (mode == "hdmi") {
    std::string bridge = details.value("bridge", "");
    for (auto& c : bridge) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    std::string where = "HDMI " + details.value("device", "") + " · " + bridge;
    if (!details.value("backend", "").empty()) where += " · " + details.value("backend", "");
    if (details.value("signal", false)) {
      double fps = details.value("input_fps", 0.0);
      std::string rate = fps > 0 ? log::format(" @ %.0f FPS", fps) : "";
      std::string res = details.value("input_resolution", "");
      if (res.empty()) res = "?";
      if (details.value("picture", false)) {
        text = where + " · " + res + rate + " · LIVE";
        colour = "live";
      } else {
        text = where + " · " + res + rate + " · BLACK PICTURE (peak " + std::to_string(details.value("frame_peak", 0)) + "/255)";
        hint = "Signal locked and frames are arriving, but every pixel is black: the device on the HDMI cable is sending a "
               "black picture. Wake it, un-blank its second display, or move a window onto that display.";
        colour = "ramping";
      }
    } else if (details.value("connected", false)) {
      text = where + " · WAITING FOR FRAMES";
      colour = "ramping";
      hint = details.value("error", "");
      if (hint.empty()) hint = "Timings locked; waiting for the first frame from the capture backend.";
    } else {
      text = where + " · NO SIGNAL";
      colour = "danger";
      hint = details.value("error", "");
      if (hint.empty()) hint = "Waiting for an HDMI source. The source must show a second display.";
    }
    set_pill(pill_hdmi_, details.value("picture", false) ? "HDMI: LIVE" : details.value("signal", false) ? "HDMI: BLACK" : "HDMI: NO SIGNAL", colour);
  } else if (mode == "library") {
    int total = details.value("total_frames", 0);
    text = total ? "LIBRARY CLIP · frame " + std::to_string(details.value("current_frame", 0)) + "/" + std::to_string(total)
                 : "LIBRARY CLIP · nothing loaded";
    set_pill(pill_hdmi_, "HDMI: IDLE", "idle");
  } else {
    text = "PROCEDURAL PATTERN · " + details.value("pattern", "");
    set_pill(pill_hdmi_, "HDMI: IDLE", "idle");
  }
  source_status_->setText(QString::fromStdString(text));
  source_hint_->setText(QString::fromStdString(hint));
  std::string cap = text;
  if (mode == "hdmi") {
    auto pos = text.find(" · ");
    if (pos != std::string::npos) cap = text.substr(pos + std::strlen(" · "));
  }
  hud_["capture"]->setText(fit_text(cap));
}

void ConsoleWindow::refresh_library() {
  auto items = studio_.library().items();
  json all = json::array();
  for (const auto& i : items) all.push_back(i.to_json());
  std::string key = all.dump();
  if (key == last_library_) return;
  last_library_ = key;
  std::string selected = selected_id();
  table_->setRowCount(0);
  for (const auto& item : items) {
    int r = table_->rowCount();
    table_->insertRow(r);
    auto* name = new QTableWidgetItem(QString::fromStdString(item.name));
    name->setData(Qt::UserRole, QString::fromStdString(item.id));
    std::string status = item.status;
    for (auto& c : status) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (!item.error.empty()) status += " — " + item.error;
    std::string length = item.duration > 0 ? log::format("%d:%02d", static_cast<int>(item.duration) / 60, static_cast<int>(item.duration) % 60) : "";
    table_->setItem(r, 0, name);
    table_->setItem(r, 1, new QTableWidgetItem(QString::fromStdString(status)));
    auto* len = new QTableWidgetItem(QString::fromStdString(length));
    len->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    table_->setItem(r, 2, len);
    if (item.id == selected) table_->selectRow(r);
  }
  int pending = studio_.pending_jobs();
  if (items.empty()) lib_status_->setText("Library is empty. Add an MP4/WebM/MOV file to convert it to 32 × 25 frames.");
  else lib_status_->setText(pending ? QString("%1 clip(s) · %2 converting").arg(items.size()).arg(pending) : QString("%1 clip(s)").arg(items.size()));
}

// ---------------------------------------------------------------- test accessors

QString ConsoleWindow::hud_text(const std::string& key) const { return hud_.at(key)->text(); }
QString ConsoleWindow::pill_text(const std::string& key) const {
  if (key == "hw") return pill_hw_->text();
  if (key == "motor") return pill_motor_->text();
  if (key == "sync") return pill_sync_->text();
  if (key == "hdmi") return pill_hdmi_->text();
  return pill_log_->text();
}
QString ConsoleWindow::source_status() const { return source_status_->text(); }
QString ConsoleWindow::slider_label(const std::string& key) const {
  if (key == "speed") return speed_val_->text();
  if (key == "brightness") return bright_val_->text();
  if (key == "ramp") return ramp_val_->text();
  if (key == "phase") return phase_val_->text();
  if (key == "gamma") return gamma_val_->text();
  return contrast_val_->text();
}
QSize ConsoleWindow::source_shown() const { return view_src_->shown_size(); }
QSize ConsoleWindow::output_shown() const { return view_out_->shown_size(); }

}  // namespace mtv::console
