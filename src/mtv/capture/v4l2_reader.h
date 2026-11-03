// Memory-mapped V4L2 capture of the TC358743 CSI bridge (port of v4l2.V4L2Reader). Linux only.
//
// Keeps `buffers` driver buffers queued; dequeue() hands out the newest good frame as a pointer into
// the driver's mmap buffer, which the caller converts in place and requeues immediately. Buffers the
// driver flags as bad (V4L2_BUF_FLAG_ERROR, set by unicam around mode changes and signal loss) or
// delivers short are requeued unseen and counted. A V4L2_EVENT_SOURCE_CHANGE (resolution change,
// signal loss) is reported as an exception so the owner reopens and re-locks the timings.
#pragma once
#ifdef MTV_LINUX
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mtv {

struct V4L2Buffer {
  int index = -1;
  const std::uint8_t* data = nullptr;
  std::size_t bytes = 0;
};

// The system calls the reader uses; tests replace them.
struct V4L2Syscalls {
  std::function<int(const char*, int)> open;
  std::function<int(int)> close;
  std::function<int(int, unsigned long, void*)> ioctl;
  std::function<void*(std::size_t, int, std::uint32_t)> mmap;   // (length, fd, offset) -> ptr or nullptr
  std::function<int(void*, std::size_t)> munmap;
  std::function<int(int, double, bool*)> poll;                   // (fd, timeout_s, event_pending) -> >0 frame ready, 0 timeout, <0 error
  static V4L2Syscalls real();
};

class V4L2Reader {
 public:
  // Throws std::runtime_error with the reason on failure (the device is closed again).
  explicit V4L2Reader(const std::string& device, int buffers = 4, V4L2Syscalls sys = V4L2Syscalls::real());
  ~V4L2Reader();
  V4L2Reader(const V4L2Reader&) = delete;
  V4L2Reader& operator=(const V4L2Reader&) = delete;

  // Next good frame, or nullopt on timeout or after requeueing a bad buffer. Throws
  // std::runtime_error on a read error or a source change.
  std::optional<V4L2Buffer> dequeue(double timeout_s);
  void requeue(int index);   // throws std::runtime_error on failure
  void close();

  int width() const { return width_; }
  int height() const { return height_; }
  int stride() const { return stride_; }
  std::uint32_t pixelformat() const { return pixelformat_; }
  std::uint32_t colorspace() const { return colorspace_; }
  bool bt709() const;   // colour matrix the driver reports (REC709), else BT.601
  std::size_t frame_bytes() const { return frame_bytes_; }
  int buffer_count() const { return static_cast<int>(maps_.size()); }
  long error_frames() const { return error_frames_; }
  bool events_subscribed() const { return events_; }
  std::uint32_t last_sequence() const { return last_sequence_; }
  long dropped_frames() const { return dropped_; }   // gaps in the driver's sequence numbers

 private:
  void open(int buffers);
  int xioctl(unsigned long request, void* arg);
  std::string device_;
  V4L2Syscalls sys_;
  int fd_ = -1;
  int width_ = 0, height_ = 0, stride_ = 0;
  std::uint32_t pixelformat_ = 0, colorspace_ = 0;
  std::size_t frame_bytes_ = 0;
  long error_frames_ = 0;
  long dropped_ = 0;
  std::uint32_t last_sequence_ = 0;
  bool have_sequence_ = false;
  struct Map { void* ptr; std::size_t length; };
  std::vector<Map> maps_;
  bool streaming_ = false;
  bool events_ = false;
};

std::string fourcc_name(std::uint32_t code);

}  // namespace mtv
#endif
