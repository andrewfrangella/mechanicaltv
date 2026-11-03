// The debug log window (F12): every engine message, operator action and error, errors in red,
// with an Errors-only filter and a Mark button. The same lines are in the rotating log file.
#pragma once
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QWidget>
#include <deque>

#include "mtv/common/log.h"

namespace mtv::console {

class DebugWindow : public QWidget {
  Q_OBJECT
 public:
  static constexpr int LOG_LINES = 2000;
  DebugWindow(const QString& log_path, QWidget* parent = nullptr);
  void append(const log::Record& record);
  void toggle();
  void show_window();
  bool is_shown() const { return isVisible(); }
  int line_count() const;

 protected:
  void keyPressEvent(QKeyEvent* e) override;

 private:
  void write(const log::Record& record);
  void refilter();
  QPlainTextEdit* text_;
  QCheckBox* errors_only_;
  std::deque<log::Record> records_;
};

}  // namespace mtv::console
