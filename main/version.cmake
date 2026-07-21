# version.cmake
#
# Dual-mode. When included from CMakeLists.txt it runs once at configure
# time (seeding ${CMAKE_BINARY_DIR}/version.h so the first build has the
# file). When invoked as a standalone script via `cmake -P` with SRC_DIR
# and BIN_DIR set, it re-runs at build time so BUILD_DATE stays current.
#
# The version comes from main/version.txt and nowhere else. Git is
# deliberately not consulted: deriving it from `git describe` meant an
# uncommitted tree built as "<sha>-dirty", so you had to commit before
# building to get a meaningful number. version.txt is also read by the
# top-level CMakeLists into PROJECT_VER (the app descriptor).
#
# Bump main/version.txt by hand, semver style:
#   MAJOR - breaks something a device or operator depends on: parameter
#           layout/semantics, ./post or /ws schema, UART protocol to the
#           STM, a hardware revision requirement. Migration hook territory.
#   MINOR - new functionality, backwards compatible.
#   PATCH - bug fixes only.

if(NOT DEFINED SRC_DIR)
    set(SRC_DIR ${CMAKE_CURRENT_LIST_DIR})
endif()
if(NOT DEFINED BIN_DIR)
    set(BIN_DIR ${CMAKE_BINARY_DIR})
endif()

set(_VERSION_FILE ${SRC_DIR}/version.txt)
if(NOT EXISTS ${_VERSION_FILE})
    message(FATAL_ERROR "Version file not found: ${_VERSION_FILE}")
endif()

file(STRINGS ${_VERSION_FILE} FIRMWARE_VERSION LIMIT_COUNT 1)
string(STRIP "${FIRMWARE_VERSION}" FIRMWARE_VERSION)

# FIRMWARE_VERSION is written to the BUILD_VERSION param, whose storage is
# PARAM_STR_SIZE (16) bytes -> 15 chars plus the NUL. main.c compares the
# stored string against the running one to fire the one-time migration
# hook, so a truncated version would silently break that comparison. Fail
# the build instead.
string(LENGTH "${FIRMWARE_VERSION}" _ver_len)
if(_ver_len EQUAL 0)
    message(FATAL_ERROR "${_VERSION_FILE} is empty")
endif()
if(_ver_len GREATER 15)
    message(FATAL_ERROR
        "Firmware version '${FIRMWARE_VERSION}' is ${_ver_len} chars; "
        "PARAM_BUILD_VERSION (PARAM_STR_SIZE) holds 15")
endif()
if(NOT FIRMWARE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(WARNING
        "Firmware version '${FIRMWARE_VERSION}' is not MAJOR.MINOR.PATCH; "
        "OTA version comparisons assume semver ordering")
endif()

string(TIMESTAMP BUILD_DATE "%Y-%m-%d %H:%M:%S" UTC)

# configure_file only touches the output file when content actually changes.
# BUILD_DATE moves every run, so version.h is rewritten on every build and
# the handful of TUs that include it recompile — that is the price of an
# accurate build timestamp, and it is only a handful of files.
configure_file(
    ${SRC_DIR}/version.h.in
    ${BIN_DIR}/version.h
    @ONLY
)

if(CMAKE_SCRIPT_MODE_FILE)
    # Standalone -P invocation: quiet success.
else()
    message(STATUS "Firmware Version: ${FIRMWARE_VERSION}")
    message(STATUS "Build Date: ${BUILD_DATE}")
endif()
