cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tracker)

target_sources(app PRIVATE
  src/main.c
  src/loc/loc_fsm.c
  src/loc/motion.c
  src/loc/imu.c
  src/loc/agnss.c
  src/net/coap_pub.c
  src/net/obs_queue.c
  src/net/event_queue.c
  src/net/uplink.c
  src/net/log_uplink.c
  src/ui/leds.c
  # Generated from proto/tracker.proto by `make proto` — do not edit.
  src/proto/tracker.pb.c
)
target_include_directories(app PRIVATE src/loc src/net src/proto src/ui)

if(NOT CONFIG_NRF_MODEM_LIB)
  # Nordic headers are not on the include path when NRF_MODEM_LIB=n.
  # Add them explicitly so main.c and the mock files can include them.
  target_include_directories(app PRIVATE
    $ENV{ZEPHYR_BASE}/../nrfxlib/nrf_modem/include
    $ENV{ZEPHYR_BASE}/../nrf/include
    $ENV{ZEPHYR_BASE}/scripts/native_simulator/common/src/include
  )
  # lte_lc.h gates event types and struct members behind Kconfig symbols.
  # Define the ones main.c uses so the header exposes them correctly.
  # hw_id.h gates HW_ID_LEN behind the source symbol — pick IMEI (len 16).
  target_compile_definitions(app PRIVATE
    CONFIG_LTE_LC_NETWORK_REGISTRATION_MODULE=1
    CONFIG_LTE_LC_PSM_MODULE=1
    CONFIG_LTE_LC_MODEM_EVENTS_MODULE=1
    CONFIG_HW_ID_LIBRARY_SOURCE_IMEI=1
  )
  target_sources(app PRIVATE
    src/lib/nrf_modem_mock/nrf_modem_mock.c
    src/lib/nrf_modem_mock/lte_lc_mock.c
  )
endif()
