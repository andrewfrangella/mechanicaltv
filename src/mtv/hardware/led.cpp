#include "mtv/hardware/led.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "mtv/common/clock.h"
#include "mtv/common/env.h"
#include "mtv/common/log.h"
#include "mtv/hardware/failsafe.h"
#ifdef MTV_LINUX
#include "mtv/hardware/linux_hw.h"
#endif

namespace mtv {

namespace {

// Set bits [from, to) of a zeroed MSB-first buffer.
inline void set_run(std::uint8_t* out, std::int64_t from, std::int64_t to) {
  if (to <= from) return;
  std::int64_t first_byte = from >> 3, last_byte = (to - 1) >> 3;
  int head = static_cast<int>(from & 7), tail = static_cast<int>((to - 1) & 7);
  if (first_byte == last_byte) {
    std::uint8_t mask = static_cast<std::uint8_t>((0xFFu >> head) & (0xFFu << (7 - tail)));
    out[first_byte] |= mask;
    return;
  }
  out[first_byte] |= static_cast<std::uint8_t>(0xFFu >> head);
  if (last_byte > first_byte + 1) std::memset(out + first_byte + 1, 0xFF, static_cast<std::size_t>(last_byte - first_byte - 1));
  out[last_byte] |= static_cast<std::uint8_t>(0xFFu << (7 - tail));
}

}  // namespace

std::size_t build_led_bits(const NipkowStream& stream, double bits_per_rev, std::int64_t bit_begin, std::int64_t bit_end,
                           int min_on_bits, std::uint8_t* out) {
  if (bit_end <= bit_begin) return 0;
  const std::size_t bytes = static_cast<std::size_t>((bit_end - bit_begin + 7) / 8);
  std::memset(out, 0, bytes);
  const double per_pixel = bits_per_rev / TOTAL_PIXELS;
  int first = std::max(0, static_cast<int>(std::floor(bit_begin / per_pixel)) - 1);
  for (int i = first; i < TOTAL_PIXELS; ++i) {
    auto start = static_cast<std::int64_t>(std::floor(i * per_pixel));
    if (start >= bit_end) break;
    auto stop = static_cast<std::int64_t>(std::floor((i + 1) * per_pixel));
    const int v = stream[static_cast<std::size_t>(i)];
    if (v == 0) continue;
    auto on = static_cast<std::int64_t>(std::nearbyint(v / 255.0 * static_cast<double>(stop - start)));
    if (on < min_on_bits) continue;
    std::int64_t a = std::max(start, bit_begin), b = std::min(start + on, bit_end);
    set_run(out, a - bit_begin, b - bit_begin);
  }
  return bytes;
}

void LedOutput::account(const NipkowStream& stream) {
  unsigned long sum = 0;
  for (auto v : stream) sum += v;
  average_duty = static_cast<double>(sum) / TOTAL_PIXELS / 255.0;
  ++frames_rendered;
  blanked = false;
}

void MockLedOutput::output(const NipkowStream& stream, std::int64_t, std::int64_t, std::int64_t t_end,
                           const std::atomic<bool>& abort, const EdgeProbe& next_edge) {
  if (abort) return;
  if (blocking_) {
    while (!abort && now_ns() < t_end && !(next_edge && next_edge())) sleep_s(0.0005);
  }
  {
    std::lock_guard<std::mutex> lock(lock_);
    last_ = stream;
  }
  account(stream);
}

NipkowStream MockLedOutput::last_stream() {
  std::lock_guard<std::mutex> lock(lock_);
  return last_;
}

#ifdef MTV_LINUX
namespace {

class SpiLedOutput : public LedOutput {
 public:
  explicit SpiLedOutput(const std::string& path) : path_(path) { pin = 10; }

  bool open(std::string* why) override {
    try {
      auto hz = static_cast<std::uint32_t>(std::clamp(env::get_double("MTV_SPI_HZ", 2'000'000.0), 100'000.0, 16'000'000.0));
      spi_ = std::make_unique<hw::SpiDev>(path_, hz);
    } catch (const std::exception& exc) {
      if (why) *why = exc.what();
      return false;
    }
    latency_ns_ = static_cast<std::int64_t>(env::get_double("MTV_SPI_LATENCY_US", 30.0) * 1000.0);
    min_pulse_us_ = env::get_double("MTV_LED_MIN_PULSE_US", DEFAULT_MIN_PULSE_US);
    buf_.assign(spi_->max_transfer() + 8, 0);
    if (spi_->max_transfer() < 32768) {
      log::warning(log::format("LED SPI: spidev transfers are limited to %zu bytes; a revolution is split into several "
                               "transfers with short dark gaps. Add spidev.bufsiz=65536 to /boot/firmware/cmdline.txt (install.sh does).",
                               spi_->max_transfer()));
    }
    // A zero byte leaves MOSI low (LED off) until the first revolution.
    std::uint8_t zero[8] = {0};
    spi_->write(zero, sizeof zero);
    return true;
  }

  void output(const NipkowStream& stream, std::int64_t t0, std::int64_t period, std::int64_t t_end,
              const std::atomic<bool>& abort, const EdgeProbe& next_edge) override {
    const double f = spi_->speed_hz();
    const double bits_per_rev = period * f / 1e9;
    const auto end_bit = static_cast<std::int64_t>(std::floor((t_end - t0) * f / 1e9));
    const int min_on = static_cast<int>(std::ceil(min_pulse_us_ * 1e-6 * f));
    // Leave one zero byte at the end of every transfer so MOSI rests low between transfers.
    const std::int64_t chunk_bits = std::min<std::int64_t>(static_cast<std::int64_t>(spi_->max_transfer() - 1) * 8,
                                                           static_cast<std::int64_t>(MAX_SPI_CHUNK_S * f));
    std::int64_t now = now_ns();
    if (t0 - latency_ns_ > now) {   // early (free-run tick): wait for the slot
      sleep_s((t0 - latency_ns_ - now) / 1e9);
    }
    bool first = true;
    long chunks = 0;
    while (!abort) {
      if (!first && next_edge && next_edge()) break;   // the next revolution has begun
      now = now_ns();
      auto begin = static_cast<std::int64_t>(std::ceil((now + latency_ns_ - t0) * f / 1e9));
      begin = std::max<std::int64_t>(begin, 0);
      if (begin >= end_bit) break;
      if (first) {
        lead_in_us = begin / f * 1e6;
        first = false;
      }
      std::int64_t end = std::min(end_bit, begin + chunk_bits);
      std::size_t n = build_led_bits(stream, bits_per_rev, begin, end, min_on, buf_.data());
      buf_[n++] = 0;
      std::string why;
      if (!spi_->write(buf_.data(), n, &why)) {
        log::error("LED SPI: " + why);
        break;
      }
      ++chunks;
    }
    chunks_ = chunks;
    if (!abort) account(stream);
  }

  void blank() override { blanked = true; }   // every transfer ends low; nothing is in flight after the loop stops
  std::string driver() const override { return "SPILEDModulator"; }
  std::string backend() const override { return "spi-dma"; }
  json details() override {
    return {{"spidev", path_}, {"speed_hz", spi_ ? spi_->speed_hz() : 0}, {"max_transfer", spi_ ? spi_->max_transfer() : 0},
            {"chunks_last_rev", chunks_.load()}, {"latency_us", latency_ns_ / 1000.0}};
  }

 private:
  std::string path_;
  std::unique_ptr<hw::SpiDev> spi_;
  std::vector<std::uint8_t> buf_;
  std::int64_t latency_ns_ = 30'000;
  double min_pulse_us_ = DEFAULT_MIN_PULSE_US;
  std::atomic<long> chunks_{0};
};

class GpioLedOutput : public LedOutput {
 public:
  explicit GpioLedOutput(int p) { pin = p; }

  bool open(std::string* why) override {
    line_ = hw::GpioLine::output(hw::main_gpiochip(), pin, "mechanical-tv-led", 0, why);
    if (!line_) return false;
    failsafe::add_line(line_->fd(), 0);   // a crash turns the LED off
    std::string mem_why;
    mem_ = hw::GpioMem::open(&mem_why);
    if (!mem_) log::warning("LED GPIO: " + mem_why + "; using the GPIO character device (about 1 us per edge)");
    min_pulse_ns_ = static_cast<std::int64_t>(env::get_double("MTV_LED_MIN_PULSE_US", DEFAULT_MIN_PULSE_US) * 1000.0);
    return true;
  }

  inline void high() {
    if (mem_) mem_->set_high(pin);
    else line_->set(1);
  }
  inline void low() {
    if (mem_) mem_->set_low(pin);
    else line_->set(0);
  }

  void output(const NipkowStream& stream, std::int64_t t0, std::int64_t period, std::int64_t t_end,
              const std::atomic<bool>& abort, const EdgeProbe& next_edge) override {
    const double slot = static_cast<double>(period) / TOTAL_PIXELS;
    long late = 0;
    bool first = true;
    for (int i = 0; i < TOTAL_PIXELS && !abort; ++i) {
      if ((i & 15) == 15 && next_edge && next_edge()) break;   // the next revolution has begun
      auto s = t0 + static_cast<std::int64_t>(i * slot);
      auto e = t0 + static_cast<std::int64_t>((i + 1) * slot);
      if (e > t_end) break;
      std::int64_t now = now_ns();
      if (now >= e) {   // slot already over (woke late, or preempted): skip, never catch up
        ++late;
        continue;
      }
      if (first) {
        lead_in_us = std::max<std::int64_t>(0, now - t0) / 1000.0;
        first = false;
      }
      const int v = stream[static_cast<std::size_t>(i)];
      auto on = static_cast<std::int64_t>(v / 255.0 * slot);
      if (on < min_pulse_ns_) continue;
      while (now_ns() < s) {
      }
      high();
      const std::int64_t off = std::min(s + on, e);
      while (now_ns() < off) {
      }
      low();
    }
    low();
    late_pixels += late;
    if (!abort) account(stream);
  }

  void blank() override {
    blanked = true;
    low();
  }
  ~GpioLedOutput() override {
    if (line_) {
      low();
      failsafe::remove_line(line_->fd());
    }
  }
  std::string driver() const override { return "GPIOLEDModulator"; }
  std::string backend() const override { return mem_ ? "gpiomem" : "gpio-cdev"; }

 private:
  std::unique_ptr<hw::GpioLine> line_;
  std::unique_ptr<hw::GpioMem> mem_;
  std::int64_t min_pulse_ns_ = 5000;
};

}  // namespace
#endif

std::unique_ptr<LedOutput> create_led_output(int pin, bool use_spi, bool force_mock, std::string* note) {
  if (force_mock || env::flag("MTV_MOCK_HARDWARE")) return std::make_unique<MockLedOutput>(pin);
  std::string kind = env::lower(env::trim(env::get("MTV_LED_OUTPUT")));
  if (kind.empty()) kind = use_spi ? "spi" : "gpio";
#ifdef MTV_LINUX
  std::string why;
  if (kind == "spi") {
    auto spi = std::make_unique<SpiLedOutput>(env::get("MTV_SPIDEV", "/dev/spidev0.0"));
    if (spi->open(&why)) return spi;
    if (note) *note = "LED SPI output unavailable (" + why + "); falling back to GPIO " + std::to_string(pin);
    log::error(*note);
  }
  auto gpio = std::make_unique<GpioLedOutput>(pin);
  if (gpio->open(&why)) return gpio;
  if (note) *note = "LED output unavailable: " + why;
  log::error("LED output unavailable: " + why + "; simulating the LED");
#else
  if (note) *note = "LED hardware needs Linux";
#endif
  return std::make_unique<MockLedOutput>(pin);
}

}  // namespace mtv
