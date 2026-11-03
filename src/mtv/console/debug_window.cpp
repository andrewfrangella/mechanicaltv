#include "mtv/console/debug_window.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>

#include "mtv/console/palette.h"

namespace mtv::console {

// Stays on top so it is visible over the full-screen console.
DebugWindow::DebugWindow(const QString& log_path, QWidget* parent) : QWidget(parent, Qt::Window | Qt::WindowStaysOnTopHint) {
  setWindowTitle(QStringLiteral("Mechanical TV — debug log"));
  resize(980, 520);
  setObjectName("debugWindow");
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(12, 10, 12, 10);
  auto* bar = new QHBoxLayout();
  auto* clear = new QPushButton("Clear view");
  auto* mark = new QPushButton("Mark");
  errors_only_ = new QCheckBox("Errors only");
  auto* path = new QLabel(QString("Log file: %1").arg(log_path.isEmpty() ? QStringLiteral("unavailable") : log_path));
  path->setObjectName("eyebrow");
  path->setTextInteractionFlags(Qt::TextSelectableByMouse);
  bar->addWidget(clear);
  bar->addWidget(mark);
  bar->addWidget(errors_only_);
  bar->addStretch(1);
  bar->addWidget(path);
  layout->addLayout(bar);
  text_ = new QPlainTextEdit();
  text_->setReadOnly(true);
  text_->setObjectName("logText");
  text_->setMaximumBlockCount(LOG_LINES);
  text_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
  layout->addWidget(text_, 1);
  connect(clear, &QPushButton::clicked, this, [this] {
    records_.clear();
    refilter();
  });
  connect(mark, &QPushButton::clicked, this, [] { log::info("---- operator mark ----"); });
  connect(errors_only_, &QCheckBox::toggled, this, [this] { refilter(); });
}

int DebugWindow::line_count() const { return text_->document()->blockCount(); }

void DebugWindow::append(const log::Record& record) {
  records_.push_back(record);
  while (records_.size() > LOG_LINES) records_.pop_front();
  if (errors_only_->isChecked() && record.level < log::Level::Error) return;
  write(record);
}

void DebugWindow::write(const log::Record& record) {
  const char* c = color::TEXT;
  if (record.level >= log::Level::Error) c = color::DANGER;
  else if (record.level == log::Level::Warning) c = color::WARN;
  else if (record.level == log::Level::Debug) c = color::MUTED;
  QScrollBar* sb = text_->verticalScrollBar();
  bool at_end = sb->value() >= sb->maximum() - 2;
  QString line = QString::fromStdString(record.format(true)).toHtmlEscaped();
  text_->appendHtml(QString("<span style=\"color:%1; white-space:pre-wrap\">%2</span>").arg(c, line));
  if (at_end) sb->setValue(sb->maximum());
}

void DebugWindow::refilter() {
  text_->clear();
  for (const auto& r : records_)
    if (!errors_only_->isChecked() || r.level >= log::Level::Error) write(r);
}

void DebugWindow::show_window() {
  show();
  raise();
  activateWindow();
  text_->verticalScrollBar()->setValue(text_->verticalScrollBar()->maximum());
}

void DebugWindow::toggle() {
  if (isVisible()) hide();
  else show_window();
}

void DebugWindow::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_F12 || e->key() == Qt::Key_Escape) {
    hide();
    return;
  }
  QWidget::keyPressEvent(e);
}

}  // namespace mtv::console
