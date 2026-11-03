#include "mtv/console/frame_view.h"

#include <QPainter>

#include "mtv/console/palette.h"

namespace mtv::console {

FrameView::FrameView(QWidget* parent) : QWidget(parent) {
  setAttribute(Qt::WA_OpaquePaintEvent);
  setMinimumSize(160, 125);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void FrameView::set_image(const QImage& image) {
  bool resized = image.size() != image_.size();
  image_ = image;
  if (resized) recompute();
  update();
}

void FrameView::recompute() {
  if (image_.isNull()) {
    zoom_ = 1;
    return;
  }
  zoom_ = std::max(1, std::min((width() - 2) / image_.width(), (height() - 2) / image_.height()));
}

void FrameView::resizeEvent(QResizeEvent*) { recompute(); }

void FrameView::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), Qt::black);
  p.setPen(QColor(color::BORDER));
  p.drawRect(rect().adjusted(0, 0, -1, -1));
  if (image_.isNull()) return;
  p.setRenderHint(QPainter::SmoothPixmapTransform, false);
  QSize s = image_.size() * zoom_;
  QRect target((width() - s.width()) / 2, (height() - s.height()) / 2, s.width(), s.height());
  p.drawImage(target, image_);
}

}  // namespace mtv::console
