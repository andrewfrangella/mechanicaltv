// LED pulse output (port of hardware/led.py, redesigned for timing accuracy).
//
// Each revolution is emitted on an absolute timeline anchored to the index edge's kernel
// timestamp: pixel i owns the slot [t0 + i*T/800, t0 + (i+1)*T/800) and is lit for value/255 of it.
// Three outputs share that contract:
//
//   spi   SPI0 MOSI (GPIO 10, pin 19) bitstream sent by DMA: 0.5 us resolution at 2 MHz, immune to
//         scheduling inside the revolution. Recommended wiring.
//   gpio  bit-banged GPIO (default GPIO 21, pin 40) from the realtime thread through /dev/gpiomem
//         registers; works with the original wiring, a few microseconds of jitter, occasional late
//         pixels under load.
//   mock  simulation (records the stream and duty cycle).
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "mtv/common/frame.h"
#include "mtv/common/json.h"

namespace mtv {

inline constexpr double DEFAULT_MIN_PULSE_US = 5.0;   // shorter pulses are not emitted (LED driver rise time)
inline constexpr double MAX_SPI_CHUNK_S = 0.1;        // one SPI transfer at most: bounds the delay of an emergency blank

// Bits [bit_begin, bit_end) of one revolution timeline at bits_per_rev bits per revolution, MSB first.
// Pixel i spans [floor(i*B/800), floor((i+1)*B/800)); its first round(v/255 * span) bits are 1 unless
// that is fewer than min_on_bits. Returns the bytes written (ceil((end-begin)/8)); `out` must hold them.
std::size_t build_led_bits(const NipkowStream& stream, double bits_per_rev, std::int64_t bit_begin, std::int64_t bit_end,
                           int min_on_bits, std::uint8_t* out);

class LedOutput {
 public:
  virtual ~LedOutput() = default;
  virtual bool open(std::string* why) { return true; }
  // Emit one revolution whose pixel 0 slot starts at t0_ns, lasting period_ns, stopping at t_end_ns
  // (the guard before the next edge). Blocks until done, until `abort` is set, or until
  // `next_edge()` reports that the next index edge has already arrived (the revolution is over).
  using EdgeProbe = std::function<bool()>;
  virtual void output(const NipkowStream& stream, std::int64_t t0_ns, std::int64_t period_ns, std::int64_t t_end_ns,
                      const std::atomic<bool>& abort, const EdgeProbe& next_edge) = 0;
  virtual void blank() = 0;
  virtual std::string driver() const = 0;
  virtual std::string backend() const = 0;
  virtual bool is_mock() const { return false; }
  virtual json details() { return json::object(); }
  int pin = 21;

  // Shared statistics (written by the realtime thread)
  void account(const NipkowStream& stream);
  std::atomic<long> frames_rendered{0};
  std::atomic<double> average_duty{0.0};
  std::atomic<bool> blanked{true};
  std::atomic<double> lead_in_us{0.0};   // dark time at the start of the last revolution (wake-up + setup)
  std::atomic<long> late_pixels{0};      // pixels skipped because their slot had already passed
};

class MockLedOutput : public LedOutput {
 public:
  // blocking: behave like the real outputs and return only at t_end (or on abort / next edge).
  explicit MockLedOutput(int p = 21, bool blocking = false) : blocking_(blocking) { pin = p; }
  void output(const NipkowStream& stream, std::int64_t t0_ns, std::int64_t period_ns, std::int64_t t_end_ns,
              const std::atomic<bool>& abort, const EdgeProbe& next_edge) override;
  void blank() override { blanked = true; }
  std::string driver() const override { return "MockLEDModulator"; }
  std::string backend() const override { return "simulated"; }
  bool is_mock() const override { return true; }
  NipkowStream last_stream();
  std::atomic<std::int64_t> last_period_ns{0};   // revolution length given to the last output() (tests)

 private:
  bool blocking_;
  std::mutex lock_;
  NipkowStream last_{};
};

// MTV_LED_OUTPUT=spi|gpio (MTV_SPI_LED=1 means spi). Returns the output and, on failure to open the
// requested hardware, a mock with *note set to the reason.
std::unique_ptr<LedOutput> create_led_output(int pin, bool use_spi, bool force_mock, std::string* note);

}  // namespace mtv
