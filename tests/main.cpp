#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdlib>

int main(int argc, char** argv) {
  // The simulated controllers must not pin their realtime thread to the isolated CPU: a running
  // console (install.sh runs the suite while it may be open) owns that core at realtime priority
  // and would starve them, failing every timing test.
  setenv("MTV_RT_CPU", "-1", 0);
  doctest::Context context;
  context.applyCommandLine(argc, argv);
  return context.run();
}
