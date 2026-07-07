cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(gnss_sample)

zephyr_library_sources(src/main.c)

# assistance.c is only needed for nRF Cloud and SUPL backends (pulls in cJSON/cloud deps)
zephyr_library_sources_ifdef(CONFIG_GNSS_SAMPLE_ASSISTANCE_NRF_CLOUD src/assistance.c)
zephyr_library_sources_ifdef(CONFIG_GNSS_SAMPLE_ASSISTANCE_SUPL src/assistance.c)

# minimal backend: factory almanac + LTE network time, no cloud deps
zephyr_library_sources_ifdef(CONFIG_GNSS_SAMPLE_ASSISTANCE_MINIMAL src/assistance_minimal.c)
zephyr_library_sources_ifdef(CONFIG_GNSS_SAMPLE_ASSISTANCE_MINIMAL src/mcc_location_table.c)
