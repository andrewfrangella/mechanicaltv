// Thin wrappers over the Linux device interfaces the hardware drivers use. Linux only.
//
//   GpioChip / GpioLine  GPIO character device v2 (/dev/gpiochipN): kernel-timestamped edge events
//                        (hard-IRQ CLOCK_MONOTONIC, no kernel debounce) and output lines
//   GpioMem              /dev/gpiomem register access for nanosecond-scale output toggles (Pi 0-4)
//   SysfsPwm             /sys/class/pwm hardware PWM channel (the A4988 STEP clock)
//   SpiDev               /dev/spidevB.C full-duplex transfers (the LED bitstream)
//   I2cDev               /dev/i2c-N (the Adafruit Motor HAT's PCA9685)
#pragma once
#ifdef MTV_LINUX
#include <cstdint>
#include <memory>
#include <string>

namespace mtv::hw {

class GpioLine {
 public:
  ~GpioLine();
  GpioLine(const GpioLine&) = delete;
  GpioLine& operator=(const GpioLine&) = delete;

  enum class Bias { None, PullDown, PullUp };
  // Input with rising-edge events (the index sensor). Returns nullptr with *why set.
  static std::unique_ptr<GpioLine> input_rising(const std::string& chip, int offset, const std::string& consumer,
                                                Bias bias, std::string* why);
  // Output, driven to `initial` immediately.
  static std::unique_ptr<GpioLine> output(const std::string& chip, int offset, const std::string& consumer, int initial,
                                          std::string* why);

  int fd() const { return fd_; }
  int offset() const { return offset_; }
  bool set(int value);
  int get();   // -1 on error
  // Read one queued edge event (call after poll() reports POLLIN). false when none is queued.
  bool read_event(std::int64_t* timestamp_ns, std::uint32_t* seqno = nullptr);

 private:
  GpioLine(int fd, int offset) : fd_(fd), offset_(offset) {}
  int fd_;
  int offset_;
};

// Path of the Pi's main GPIO controller: MTV_GPIOCHIP (number or path) if set, else the chip labelled
// pinctrl-bcm2711 / pinctrl-bcm2835 / pinctrl-rp1, else /dev/gpiochip0. *label receives its label.
std::string main_gpiochip(std::string* label = nullptr);

class GpioMem {
 public:
  // Maps /dev/gpiomem; nullptr with *why when unavailable (Pi 5, permissions).
  static std::unique_ptr<GpioMem> open(std::string* why);
  ~GpioMem();
  inline void set_high(int pin) { regs_[7] = 1u << pin; }    // GPSET0
  inline void set_low(int pin) { regs_[10] = 1u << pin; }    // GPCLR0
 private:
  explicit GpioMem(volatile std::uint32_t* regs, int fd) : regs_(regs), fd_(fd) {}
  volatile std::uint32_t* regs_;
  int fd_;
};

class SysfsPwm {
 public:
  // chip: "" picks the first /sys/class/pwm/pwmchip*. Exports the channel if needed. Throws
  // std::runtime_error with an actionable message.
  SysfsPwm(const std::string& chip, int channel);
  ~SysfsPwm();
  // Set the period (ns) keeping 50 % duty, respecting duty <= period at every moment.
  void set_period_ns(std::uint64_t period_ns);
  void enable(bool on);
  std::uint64_t period_ns() const { return period_; }
  const std::string& path() const { return path_; }

 private:
  void write(const std::string& file, std::uint64_t value);
  std::string path_;
  std::uint64_t period_ = 0;
  bool enabled_ = false;
};

class SpiDev {
 public:
  // Throws std::runtime_error.
  SpiDev(const std::string& path, std::uint32_t speed_hz, int mode = 0);
  ~SpiDev();
  // Blocking write of len bytes (len <= max_transfer()). false with *why on failure.
  bool write(const std::uint8_t* data, std::size_t len, std::string* why = nullptr);
  std::uint32_t speed_hz() const { return speed_; }
  std::size_t max_transfer() const { return max_transfer_; }   // min(spidev bufsiz, 65532)
  const std::string& path() const { return path_; }

 private:
  std::string path_;
  int fd_ = -1;
  std::uint32_t speed_;
  std::size_t max_transfer_ = 4096;
};

class I2cDev {
 public:
  I2cDev(int bus, int address);   // throws std::runtime_error
  ~I2cDev();
  void write_byte(std::uint8_t reg, std::uint8_t value);
  std::uint8_t read_byte(std::uint8_t reg);
  void write_block(std::uint8_t reg, const std::uint8_t* data, std::size_t len);

 private:
  int fd_ = -1;
  int address_;
};

}  // namespace mtv::hw
#endif
