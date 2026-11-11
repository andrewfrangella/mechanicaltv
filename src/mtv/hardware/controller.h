// Realtime controller (port of hardware/controller.py, redesigned).
//
// One realtime thread (SCHED_FIFO, pinned to CPU 3, which install.sh isolates) owns the index
// sensor and the LED. For each index edge it takes the kernel timestamp of the edge, the newest
// frame and the current calibration, and emits that revolution's 800 pixels on a timeline anchored
// to the edge, so the picture's phase is re-established every revolution and cannot drift. The
// motor runs open-loop from the hardware PWM (a stepper does not need a speed loop); the index
// pulses are used to measure speed, lock, jitter and to detect a stall.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mtv/capture/pipeline.h"
#include "mtv/common/json.h"
#include "mtv/hardware/led.h"
#include "mtv/hardware/motor.h"
#include "mtv/hardware/sensor.h"

namespace mtv {

struct ControllerOptions {
  int motor_port = 1;
  int opto_pin = 4;
  int led_pin = 21;
  bool use_spi_led = false;
  bool force_mock = false;
  double target_fps = DEFAULT_FPS;
  int microsteps = 0;      // > 0 overrides MTV_MICROSTEPS (the console's saved setting)
  double ramp_rate = 0.0;  // > 0 overrides MTV_RAMP_RATE
};

class RealtimeController {
 public:
  RealtimeController(FramePipeline& pipeline, const ControllerOptions& options);
  // Tests: inject drivers.
  RealtimeController(FramePipeline& pipeline, std::unique_ptr<MotorDriver> motor, std::unique_ptr<IndexSensor> sensor,
                     std::unique_ptr<LedOutput> led, double target_fps = DEFAULT_FPS);
  ~RealtimeController();

  void start();             // spin up the motor and start clocking frames from the index sensor
  void stop();              // stop the motor, blank the LED (free-run stays as it is)
  void emergency_blank();   // LED off, motor stopped, free-run off
  void set_free_run(bool enabled);
  void set_target_fps(double fps);
  // Console motor settings. The microstep change throws std::invalid_argument while the motor (or
  // the microstep check) runs, for a value that is not a power of two up to 256, or on a driver
  // whose stepping is fixed.
  void set_microsteps(int microsteps);
  void set_ramp_rate(double fps_per_s);
  int microsteps() const { return motor_->microsteps(); }
  void set_source_mode(const std::string& mode, const std::string& library_path = "");
  void set_pattern(const std::string& pattern);
  void set_calibration(const Calibration& cal);
  // Bench tests (the console's Tests tab): each runs one part of the machine on its own.
  //   Motor test: the motor spins at the target speed with the LED kept dark and no stall checks; the
  //     index pulses are still measured, so it shows how the sensor sees the turning disk.
  //   Sensor watch: keeps the index sensor open with the motor stopped (turn the disk by hand) and
  //     logs every edge, accepted or rejected, together with the line level.
  //   LED test: the LED alone, "steady" or "blink" (1 Hz) at `level` (0-1), never with the normal
  //     run or free-run; it switches itself off after LED_TEST_MAX_S.
  // They throw std::invalid_argument when another mode owns the motor or the LED.
  static constexpr double LED_TEST_MAX_S = 30.0;
  void start_motor_test();
  bool motor_test() const { return motor_test_ && running_; }
  void set_sensor_watch(bool on);
  bool sensor_watch() const { return sensor_watch_; }
  void set_led_test(const std::string& mode, double level = 0.5);
  std::string led_test() const;
  // Blocking (up to ~40 s): spin slowly, count microsteps between two index pulses, report whether the
  // driver's microstep jumpers match MTV_MICROSTEPS. Requires the motor stopped and free-run off.
  json run_microstep_test();
  json get_status();
  FramePtr get_preview_frame();   // the frame most recently sent to the disk, else the live source frame
  void close();

  std::string state();
  bool free_run() const { return free_run_; }
  double target_fps() const { return target_fps_; }
  bool hardware_mock() const { return motor_->driver().is_mock(); }
  MotorController& motor() { return *motor_; }
  IndexSensor& sensor() { return *sensor_; }
  LedOutput& led() { return *led_; }
  FramePipeline& pipeline() { return pipeline_; }
  const std::vector<std::string>& hardware_notes() const { return notes_; }
  long frame_counter() const { return frame_counter_; }
  long free_run_frames() const { return free_run_frames_; }

 private:
  void launch();
  void rt_loop();
  void on_edge(std::int64_t t_ns);
  void drive(std::int64_t t0, std::int64_t period_ns, bool free_run);
  bool led_test_active();   // realtime thread: an LED test runs now (ends it once it times out)
  void led_test_slice();    // realtime thread: about 10 ms of LED test output
  json bench_status();
  void halt(const std::string& reason);
  void open_sensor_if_needed();
  void close_sensor_if_open();

  FramePipeline& pipeline_;
  std::unique_ptr<MotorController> motor_;
  std::unique_ptr<IndexSensor> sensor_;
  std::unique_ptr<LedOutput> led_;
  std::vector<std::string> notes_;

  std::atomic<double> target_fps_;
  std::atomic<bool> running_{false};
  std::atomic<bool> testing_{false};
  std::atomic<bool> free_run_{false};
  std::atomic<bool> quit_{false};
  std::atomic<bool> inhibit_{true};    // set: the LED must not be driven (stop, emergency, start of free-run off)
  std::atomic<bool> safety_tripped_{false};
  std::atomic<bool> motor_test_{false};
  std::atomic<bool> sensor_watch_{false};
  std::atomic<int> led_test_{0};             // 0 off, 1 steady, 2 blink
  std::atomic<bool> led_test_abort_{true};
  std::atomic<double> led_test_level_{0.5};
  std::atomic<double> led_test_started_{0.0};
  std::atomic<int> sensor_level_{-1};
  std::atomic<long> frame_counter_{0};
  std::atomic<long> free_run_frames_{0};
  std::atomic<double> started_at_{0.0};
  std::thread rt_;

  std::mutex stats_lock_;   // guards tracker_, error_, preview_, sensor state strings, rt info
  RevolutionTracker tracker_;
  std::string error_;
  bool stalled_ = false;
  std::string sensor_error_;
  FramePtr preview_;
  double preview_at_ = 0.0;
  int slip_revs_ = 0;
  struct RawEdge {
    std::int64_t t_ns;
    double steps;
    bool accepted;
  };
  std::deque<RawEdge> raw_edges_;   // every index edge, newest last (bench tests)
  long raw_edge_count_ = 0;
  std::vector<std::pair<std::int64_t, double>> test_edges_;
  std::condition_variable test_cv_;
  json rt_info_ = json::object();
  std::atomic<bool> abort_test_{false};
  bool stall_detect_ = true;          // MTV_STALL_DETECT, read once: index checks and warnings
  bool stall_stop_ = false;           // MTV_STALL_STOP=1, read once: index problems stop the motor
  std::string warning_;               // index problem while the motor keeps running (guarded by stats_lock_)
  std::int64_t guard_ns_ = 150'000;   // MTV_LED_GUARD_US, read once
  std::atomic<double> edge_latency_us_{0.0};   // edge -> realtime thread wake-up
  std::atomic<double> edge_latency_max_us_{0.0};
};

}  // namespace mtv
