#include "mtv/capture/v4l2_reader.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace mtv {

std::string fourcc_name(std::uint32_t code) {
  std::string s(4, ' ');
  for (int i = 0; i < 4; ++i) {
    char c = static_cast<char>((code >> (8 * i)) & 0xff);
    s[static_cast<std::size_t>(i)] = (c >= 32 && c < 127) ? c : '?';
  }
  return s;
}

V4L2Syscalls V4L2Syscalls::real() {
  V4L2Syscalls s;
  s.open = [](const char* path, int flags) { return ::open(path, flags | O_CLOEXEC); };
  s.close = [](int fd) { return ::close(fd); };
  s.ioctl = [](int fd, unsigned long req, void* arg) {
    int rc;
    do {
      rc = ::ioctl(fd, req, arg);
    } while (rc < 0 && errno == EINTR);
    return rc;
  };
  s.mmap = [](std::size_t length, int fd, std::uint32_t offset) -> void* {
    void* p = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, static_cast<off_t>(offset));
    return p == MAP_FAILED ? nullptr : p;
  };
  s.munmap = [](void* p, std::size_t length) { return ::munmap(p, length); };
  s.poll = [](int fd, double timeout_s, bool* event) {
    pollfd p{fd, static_cast<short>(POLLIN | POLLPRI), 0};
    int rc;
    do {
      rc = ::poll(&p, 1, static_cast<int>(timeout_s * 1000));
    } while (rc < 0 && errno == EINTR);
    if (rc > 0) {
      if (event) *event = (p.revents & POLLPRI) != 0;
      if (p.revents & (POLLERR | POLLNVAL)) return -1;
      return (p.revents & POLLIN) ? 1 : (event && *event ? 1 : 0);
    }
    return rc;
  };
  return s;
}

V4L2Reader::V4L2Reader(const std::string& device, int buffers, V4L2Syscalls sys) : device_(device), sys_(std::move(sys)) {
  open(buffers);
}

V4L2Reader::~V4L2Reader() { close(); }

int V4L2Reader::xioctl(unsigned long request, void* arg) { return sys_.ioctl(fd_, request, arg); }

bool V4L2Reader::bt709() const {
  if (colorspace_ == V4L2_COLORSPACE_REC709) return true;
  if (colorspace_ == V4L2_COLORSPACE_SMPTE170M || colorspace_ == V4L2_COLORSPACE_470_SYSTEM_BG) return false;
  return height_ >= 720;   // unknown: HD is BT.709 by convention
}

void V4L2Reader::open(int buffers) {
  fd_ = sys_.open(device_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd_ < 0) throw std::runtime_error(device_ + ": " + std::strerror(errno));
  try {
    v4l2_capability cap{};
    if (xioctl(VIDIOC_QUERYCAP, &cap) != 0) throw std::runtime_error(std::string("VIDIOC_QUERYCAP: ") + std::strerror(errno));
    std::uint32_t caps = cap.device_caps ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
      throw std::runtime_error(device_ + " is not a streaming video capture device");
    }
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(VIDIOC_G_FMT, &fmt) != 0) throw std::runtime_error(std::string("VIDIOC_G_FMT: ") + std::strerror(errno));
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_UYVY) {
      fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY;
      fmt.fmt.pix.field = V4L2_FIELD_NONE;
      if (xioctl(VIDIOC_S_FMT, &fmt) != 0) throw std::runtime_error(std::string("VIDIOC_S_FMT: ") + std::strerror(errno));
      if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_UYVY) {
        throw std::runtime_error(device_ + " cannot deliver UYVY (driver offers " + fourcc_name(fmt.fmt.pix.pixelformat) + ")");
      }
    }
    width_ = static_cast<int>(fmt.fmt.pix.width);
    height_ = static_cast<int>(fmt.fmt.pix.height);
    stride_ = fmt.fmt.pix.bytesperline ? static_cast<int>(fmt.fmt.pix.bytesperline) : width_ * 2;
    pixelformat_ = fmt.fmt.pix.pixelformat;
    colorspace_ = fmt.fmt.pix.colorspace;
    if (width_ < 2 || height_ < 1 || stride_ < width_ * 2) {
      throw std::runtime_error(device_ + " reports an unusable format " + std::to_string(width_) + "x" +
                               std::to_string(height_) + " stride " + std::to_string(stride_));
    }
    frame_bytes_ = static_cast<std::size_t>(stride_) * height_;

    v4l2_requestbuffers req{};
    req.count = static_cast<__u32>(buffers);
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(VIDIOC_REQBUFS, &req) != 0) throw std::runtime_error(std::string("VIDIOC_REQBUFS: ") + std::strerror(errno));
    if (req.count < 2) throw std::runtime_error(device_ + " granted only " + std::to_string(req.count) + " buffer(s)");
    for (__u32 i = 0; i < req.count; ++i) {
      v4l2_buffer buf{};
      buf.index = i;
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      if (xioctl(VIDIOC_QUERYBUF, &buf) != 0) throw std::runtime_error(std::string("VIDIOC_QUERYBUF: ") + std::strerror(errno));
      void* p = sys_.mmap(buf.length, fd_, buf.m.offset);
      if (!p) throw std::runtime_error(std::string("mmap: ") + std::strerror(errno));
      maps_.push_back({p, buf.length});
      if (xioctl(VIDIOC_QBUF, &buf) != 0) throw std::runtime_error(std::string("VIDIOC_QBUF: ") + std::strerror(errno));
    }
    // Source-change events are optional: unicam forwards them from the bridge on most kernels.
    v4l2_event_subscription sub{};
    sub.type = V4L2_EVENT_SOURCE_CHANGE;
    events_ = xioctl(VIDIOC_SUBSCRIBE_EVENT, &sub) == 0;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(VIDIOC_STREAMON, &type) != 0) throw std::runtime_error(std::string("VIDIOC_STREAMON: ") + std::strerror(errno));
    streaming_ = true;
  } catch (...) {
    close();
    throw;
  }
}

std::optional<V4L2Buffer> V4L2Reader::dequeue(double timeout_s) {
  bool event = false;
  int ready = sys_.poll(fd_, timeout_s, &event);
  if (ready < 0) throw std::runtime_error(std::string("poll: ") + std::strerror(errno));
  if (event && events_) {
    v4l2_event ev{};
    if (xioctl(VIDIOC_DQEVENT, &ev) == 0 && ev.type == V4L2_EVENT_SOURCE_CHANGE) {
      throw std::runtime_error("HDMI source changed (resolution change or signal loss)");
    }
  }
  if (ready == 0) return std::nullopt;
  v4l2_buffer buf{};
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;
  if (xioctl(VIDIOC_DQBUF, &buf) != 0) {
    if (errno == EAGAIN) return std::nullopt;
    throw std::runtime_error(std::string("VIDIOC_DQBUF: ") + std::strerror(errno));
  }
  int index = static_cast<int>(buf.index);
  std::size_t used = buf.bytesused ? buf.bytesused : frame_bytes_;
  if (have_sequence_ && buf.sequence > last_sequence_ + 1) dropped_ += static_cast<long>(buf.sequence - last_sequence_ - 1);
  last_sequence_ = buf.sequence;
  have_sequence_ = true;
  if ((buf.flags & V4L2_BUF_FLAG_ERROR) || used < frame_bytes_ || index < 0 || index >= buffer_count()) {
    ++error_frames_;
    if (index >= 0 && index < buffer_count()) requeue(index);
    return std::nullopt;
  }
  V4L2Buffer out;
  out.index = index;
  out.data = static_cast<const std::uint8_t*>(maps_[static_cast<std::size_t>(index)].ptr);
  out.bytes = std::min(used, maps_[static_cast<std::size_t>(index)].length);
  return out;
}

void V4L2Reader::requeue(int index) {
  v4l2_buffer buf{};
  buf.index = static_cast<__u32>(index);
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;
  if (xioctl(VIDIOC_QBUF, &buf) != 0) throw std::runtime_error(std::string("VIDIOC_QBUF: ") + std::strerror(errno));
}

void V4L2Reader::close() {
  if (fd_ < 0) return;
  if (streaming_) {
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(VIDIOC_STREAMOFF, &type);
    streaming_ = false;
  }
  for (auto& m : maps_) sys_.munmap(m.ptr, m.length);   // after STREAMOFF, before close
  maps_.clear();
  sys_.close(fd_);
  fd_ = -1;
}

}  // namespace mtv
