// A black pane that shows a small picture at the largest whole-number zoom that fits, centred, with
// nearest-neighbour scaling so every disk pixel stays a crisp square.
#pragma once
#include <QImage>
#include <QWidget>

namespace mtv::console {

class FrameView : public QWidget {
  Q_OBJECT
 public:
  explicit FrameView(QWidget* parent = nullptr);
  void set_image(const QImage& image);   // shared, cheap
  int zoom() const { return zoom_; }
  QSize shown_size() const { return image_.size() * zoom_; }

 protected:
  void paintEvent(QPaintEvent*) override;
  void resizeEvent(QResizeEvent*) override;

 private:
  void recompute();
  QImage image_;
  int zoom_ = 1;
};

}  // namespace mtv::console
