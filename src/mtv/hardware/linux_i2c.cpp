#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "mtv/hardware/linux_hw.h"

namespace mtv::hw {

I2cDev::I2cDev(int bus, int address) : address_(address) {
  std::string path = "/dev/i2c-" + std::to_string(bus);
  fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd_ < 0) throw std::runtime_error(path + ": " + std::strerror(errno));
  if (::ioctl(fd_, I2C_SLAVE, address) < 0) {
    int err = errno;
    ::close(fd_);
    fd_ = -1;
    throw std::runtime_error(path + ": cannot address 0x" + std::to_string(address) + ": " + std::strerror(err));
  }
}

I2cDev::~I2cDev() {
  if (fd_ >= 0) ::close(fd_);
}

void I2cDev::write_byte(std::uint8_t reg, std::uint8_t value) { write_block(reg, &value, 1); }

std::uint8_t I2cDev::read_byte(std::uint8_t reg) {
  if (::write(fd_, &reg, 1) != 1) throw std::runtime_error(std::string("I2C write failed: ") + std::strerror(errno));
  std::uint8_t v = 0;
  if (::read(fd_, &v, 1) != 1) throw std::runtime_error(std::string("I2C read failed: ") + std::strerror(errno));
  return v;
}

void I2cDev::write_block(std::uint8_t reg, const std::uint8_t* data, std::size_t len) {
  std::vector<std::uint8_t> buf(len + 1);
  buf[0] = reg;
  std::memcpy(buf.data() + 1, data, len);
  if (::write(fd_, buf.data(), buf.size()) != static_cast<ssize_t>(buf.size())) {
    throw std::runtime_error(std::string("I2C write to 0x") + std::to_string(address_) + " failed: " + std::strerror(errno));
  }
}

}  // namespace mtv::hw
