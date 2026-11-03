cmake_minimum_required(VERSION 3.16)
project(mechanical_tv VERSION 0.3.0 LANGUAGES CXX)

# Mechanical TV: realtime Nipkow-disk television for the Raspberry Pi 4 (desktop application).
#
#   mtv_core          engine library: capture, frame pipeline, library/player, realtime hardware
#   mechanical-tv     the operator console (Qt Widgets) that runs on the Pi's display
#   mtv_tests         doctest unit and integration tests
#
# Linux-only hardware backends (V4L2, gpiochip, /dev/gpiomem, sysfs PWM, spidev, i2c-dev) compile only
# on Linux; everywhere else the mock drivers are used, so the console runs on macOS for development.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(MTV_BUILD_CONSOLE "Build the Qt operator console" ON)
option(MTV_BUILD_TESTS "Build the tests" ON)
option(MTV_WERROR "Treat warnings as errors" OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
if(MTV_WERROR)
  add_compile_options(-Werror)
endif()

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

set(MTV_CORE_SOURCES
  src/mtv/common/log.cpp
  src/mtv/common/env.cpp
  src/mtv/common/subprocess.cpp
  src/mtv/common/sysinfo.cpp
  src/mtv/common/rt.cpp
  src/mtv/capture/gamma.cpp
  src/mtv/capture/patterns.cpp
  src/mtv/capture/uyvy_sampler.cpp
  src/mtv/capture/csi_control.cpp
  src/mtv/capture/ffmpeg_source.cpp
  src/mtv/capture/hdmi_capture.cpp
  src/mtv/capture/library_clip.cpp
  src/mtv/capture/pipeline.cpp
  src/mtv/engine/library.cpp
  src/mtv/engine/studio.cpp
  src/mtv/hardware/motor.cpp
  src/mtv/hardware/sensor.cpp
  src/mtv/hardware/led.cpp
  src/mtv/hardware/controller.cpp
  src/mtv/hardware/failsafe.cpp
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND MTV_CORE_SOURCES
    src/mtv/capture/v4l2_reader.cpp
    src/mtv/hardware/linux_gpio.cpp
    src/mtv/hardware/linux_pwm.cpp
    src/mtv/hardware/linux_spi.cpp
    src/mtv/hardware/linux_i2c.cpp
  )
endif()

add_library(mtv_core STATIC ${MTV_CORE_SOURCES})
target_include_directories(mtv_core PUBLIC src third_party)
target_link_libraries(mtv_core PUBLIC Threads::Threads SQLite::SQLite3)
target_compile_definitions(mtv_core PUBLIC MTV_VERSION="${PROJECT_VERSION}")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(mtv_core PUBLIC MTV_LINUX=1)
endif()

# The EDID files are installed next to the binaries. MTV_SHARE_DIR is the compiled-in default location.
set(MTV_SHARE_DIR "${CMAKE_INSTALL_PREFIX}/share/mechanical-tv" CACHE PATH "Runtime data directory (edid/)")
target_compile_definitions(mtv_core PUBLIC MTV_SHARE_DIR="${MTV_SHARE_DIR}")
target_compile_definitions(mtv_core PUBLIC MTV_SOURCE_SHARE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

if(MTV_BUILD_CONSOLE)
  find_package(QT NAMES Qt6 Qt5 COMPONENTS Widgets REQUIRED)
  find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Widgets REQUIRED)
  set(CMAKE_AUTOMOC ON)
  add_library(mtv_console STATIC
    src/mtv/console/console_window.cpp
    src/mtv/console/debug_window.cpp
    src/mtv/console/frame_view.cpp
    src/mtv/console/app_setup.cpp
  )
  target_link_libraries(mtv_console PUBLIC mtv_core Qt${QT_VERSION_MAJOR}::Widgets)
  add_executable(mechanical-tv src/mtv/console/main.cpp)
  target_link_libraries(mechanical-tv PRIVATE mtv_console)
  install(TARGETS mechanical-tv RUNTIME DESTINATION bin)
endif()

install(DIRECTORY edid DESTINATION share/mechanical-tv)

if(MTV_BUILD_TESTS)
  enable_testing()
  file(GLOB MTV_TEST_SOURCES CONFIGURE_DEPENDS tests/test_*.cpp)
  add_executable(mtv_tests tests/main.cpp ${MTV_TEST_SOURCES})
  target_link_libraries(mtv_tests PRIVATE mtv_core)
  if(MTV_BUILD_CONSOLE)
    target_link_libraries(mtv_tests PRIVATE mtv_console)
    target_compile_definitions(mtv_tests PRIVATE MTV_HAVE_CONSOLE=1)
  endif()
  target_compile_definitions(mtv_tests PRIVATE MTV_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME mtv_tests COMMAND mtv_tests)
endif()
