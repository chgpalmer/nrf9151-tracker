cmake_minimum_required(VERSION 3.20.0)

# Use the tracker app's own Kconfig as the root so every TRACKER_* symbol
# exists here with its production default — the test always runs against the
# thresholds the firmware ships with, not a private copy that can drift.
set(KCONFIG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../apps/tracker/Kconfig)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(loc_fsm_test)

set(TRACKER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../apps/tracker/src)

# The unit under test is compiled in directly; the test's own main.c provides
# the one modem call it makes (nrf_modem_gnss_agnss_expiry_get) as a
# controllable fake, and ztest provides main().
target_sources(app PRIVATE
  src/main.c
  src/motion_test.c
  ${TRACKER_SRC}/loc/loc_fsm.c
  ${TRACKER_SRC}/loc/motion.c
)
target_include_directories(app PRIVATE
  ${TRACKER_SRC}/loc
  # Nordic headers aren't on the include path without NRF_MODEM_LIB (which
  # doesn't exist on native_sim) — same trick as the tracker's sim build.
  $ENV{ZEPHYR_BASE}/../nrfxlib/nrf_modem/include
)
