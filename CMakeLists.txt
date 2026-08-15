cmake_minimum_required(VERSION 3.16)
project(intel-arc-utility VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED libsystemd)

find_path(LEVEL_ZERO_INCLUDE_DIR NAMES level_zero/ze_api.h)
find_library(LEVEL_ZERO_LOADER NAMES ze_loader)

# 1. The System Daemon
add_executable(arcd arcd.c)
target_include_directories(arcd PRIVATE ${LEVEL_ZERO_INCLUDE_DIR} ${SYSTEMD_INCLUDE_DIRS})
target_link_libraries(arcd PRIVATE ${LEVEL_ZERO_LOADER} ${SYSTEMD_LIBRARIES})

# 2. The Command Line Client
add_executable(arc-cli arc_cli.c)
target_include_directories(arc-cli PRIVATE ${SYSTEMD_INCLUDE_DIRS})
target_link_libraries(arc-cli PRIVATE ${SYSTEMD_LIBRARIES})
