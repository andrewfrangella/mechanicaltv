// The mmap V4L2 reader against a fake driver (from test_v4l2.py's FakeIoctl/FakeMap tests). Linux only.
#include <doctest/doctest.h>

#ifdef MTV_LINUX
#include <linux/videodev2.h>

#include <algorithm>
#include <cerrno>
#include <stdexcept>
#include <cstring>
#include <deque>
#include <vector>

#include "mtv/capture/v4l2_reader.h"

using namespace mtv;

namespace {

struct FakeDriver {
  std::uint32_t pixfmt = V4L2_PIX_FMT_UYVY;
  bool can_uyvy = true;
  std::vector<std::vector<std::uint8_t>> bufs;
  std::deque<v4l2_buffer> done;   // filled buffers ready to dequeue
  std::vector<std::string> calls;
  bool streaming = false;
  int unmapped = 0, closed = 0;
  bool event_pending = false;

  V4L2Syscalls sys() {
    V4L2Syscalls s;
    s.open = [this](const char*, int) { return 42; };
    s.close = [this](int) {
      ++closed;
      calls.push_back("close");
      return 0;
    };
    s.mmap = [this](std::size_t, int, std::uint32_t off) -> void* { return bufs[off / 4147200].data(); };
    s.munmap = [this](void*, std::size_t) {
      ++unmapped;
      calls.push_back("munmap");
      return 0;
    };
    s.poll = [this](int, double, bool* ev) {
      if (ev) *ev = event_pending;
      return (!done.empty() || event_pending) ? 1 : 0;
    };
    s.ioctl = [this](int, unsigned long req, void* arg) -> int {
      if (req == VIDIOC_QUERYCAP) {
        auto* c = static_cast<v4l2_capability*>(arg);
        c->capabilities = c->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
        return 0;
      }
      if (req == VIDIOC_G_FMT || req == VIDIOC_S_FMT) {
        auto* f = static_cast<v4l2_format*>(arg);
        if (req == VIDIOC_S_FMT && !can_uyvy) {
          f->fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
          return 0;
        }
        f->fmt.pix.width = 1920;
        f->fmt.pix.height = 1080;
        f->fmt.pix.bytesperline = 3840;
        f->fmt.pix.pixelformat = req == VIDIOC_G_FMT ? pixfmt : V4L2_PIX_FMT_UYVY;
        f->fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;
        return 0;
      }
      if (req == VIDIOC_REQBUFS) {
        auto* r = static_cast<v4l2_requestbuffers*>(arg);
        r->count = 4;
        bufs.assign(4, std::vector<std::uint8_t>(4147200, 0));
        return 0;
      }
      if (req == VIDIOC_QUERYBUF) {
        auto* b = static_cast<v4l2_buffer*>(arg);
        b->length = 4147200;
        b->m.offset = b->index * 4147200;
        return 0;
      }
      if (req == VIDIOC_QBUF) {
        calls.push_back("qbuf" + std::to_string(static_cast<v4l2_buffer*>(arg)->index));
        return 0;
      }
      if (req == VIDIOC_DQBUF) {
        if (done.empty()) {
          errno = EAGAIN;
          return -1;
        }
        *static_cast<v4l2_buffer*>(arg) = done.front();
        done.pop_front();
        return 0;
      }
      if (req == VIDIOC_STREAMON) {
        streaming = true;
        calls.push_back("streamon");
        return 0;
      }
      if (req == VIDIOC_STREAMOFF) {
        streaming = false;
        calls.push_back("streamoff");
        return 0;
      }
      if (req == VIDIOC_SUBSCRIBE_EVENT) return 0;
      if (req == VIDIOC_DQEVENT) {
        auto* e = static_cast<v4l2_event*>(arg);
        e->type = V4L2_EVENT_SOURCE_CHANGE;
        event_pending = false;
        return 0;
      }
      errno = ENOTTY;
      return -1;
    };
    return s;
  }
  void fill(int index, std::uint32_t used, std::uint32_t flags = 0, std::uint32_t seq = 0) {
    v4l2_buffer b{};
    b.index = static_cast<__u32>(index);
    b.bytesused = used;
    b.flags = flags;
    b.sequence = seq;
    done.push_back(b);
  }
};

}  // namespace

TEST_CASE("V4L2 reader: setup queues every buffer and streams; teardown stops before unmapping and closing") {
  FakeDriver d;
  {
    V4L2Reader r("/dev/video0", 4, d.sys());
    CHECK(r.width() == 1920);
    CHECK(r.stride() == 3840);
    CHECK(r.buffer_count() == 4);
    CHECK_FALSE(r.bt709());   // the bridge reports SMPTE 170M
    CHECK(d.streaming);
    CHECK(std::count_if(d.calls.begin(), d.calls.end(), [](const std::string& c) { return c.rfind("qbuf", 0) == 0; }) == 4);
  }
  auto off = std::find(d.calls.begin(), d.calls.end(), "streamoff");
  auto unmap = std::find(d.calls.begin(), d.calls.end(), "munmap");
  auto close = std::find(d.calls.begin(), d.calls.end(), "close");
  CHECK(off < unmap);
  CHECK(unmap < close);
  CHECK(d.unmapped == 4);
}

TEST_CASE("V4L2 reader: good frames are returned, bad and short ones requeued unseen, empty queue is not an error") {
  FakeDriver d;
  V4L2Reader r("/dev/video0", 4, d.sys());
  CHECK_FALSE(r.dequeue(0.01).has_value());
  d.fill(1, 4147200, 0, 10);
  auto got = r.dequeue(0.01);
  REQUIRE(got.has_value());
  CHECK(got->index == 1);
  CHECK(got->bytes == 4147200);
  d.calls.clear();
  d.fill(2, 4147200, V4L2_BUF_FLAG_ERROR, 11);
  CHECK_FALSE(r.dequeue(0.01).has_value());
  CHECK(d.calls.back() == "qbuf2");
  d.fill(3, 1000, 0, 14);
  CHECK_FALSE(r.dequeue(0.01).has_value());
  CHECK(r.error_frames() == 2);
  CHECK(r.dropped_frames() == 2);   // sequence 11 -> 14
}

TEST_CASE("V4L2 reader: a source-change event is reported; a non-UYVY device is refused and closed") {
  FakeDriver d;
  V4L2Reader r("/dev/video0", 4, d.sys());
  d.event_pending = true;
  CHECK_THROWS_AS(r.dequeue(0.01), std::runtime_error);
  FakeDriver m;
  m.pixfmt = V4L2_PIX_FMT_MJPEG;
  m.can_uyvy = false;
  CHECK_THROWS_WITH(V4L2Reader("/dev/video0", 4, m.sys()), doctest::Contains("cannot deliver UYVY"));
  CHECK(m.closed == 1);
}
#endif
