# version.cmake
#
# Dual-mode. When included from CMakeLists.txt it runs once at configure
# time (seeding ${CMAKE_BINARY_DIR}/version.h so the first build has the
# file). When invoked as a standalone script via `cmake -P` with SRC_DIR
# and BIN_DIR set, it re-runs at build time so the SHA / dirty flag stay
# current as commits happen between builds.

if(NOT DEFINED SRC_DIR)
    set(SRC_DIR ${CMAKE_CURRENT_LIST_DIR})
endif()
if(NOT DEFINED BIN_DIR)
    set(BIN_DIR ${CMAKE_BINARY_DIR})
endif()

# Parent dir of the main/ component is the firmware project root, which
# is inside the outer git repo.
get_filename_component(_GIT_WORK_DIR ${SRC_DIR} DIRECTORY)

execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${_GIT_WORK_DIR}
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

execute_process(
    COMMAND git rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY ${_GIT_WORK_DIR}
    OUTPUT_VARIABLE GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${_GIT_WORK_DIR}
    OUTPUT_VARIABLE GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

string(TIMESTAMP BUILD_DATE "%Y-%m-%d %H:%M:%S" UTC)

if(NOT GIT_VERSION)
    set(GIT_VERSION "unknown")
endif()
if(NOT GIT_BRANCH)
    set(GIT_BRANCH "unknown")
endif()
if(NOT GIT_SHA)
    set(GIT_SHA "0000000")
endif()

# configure_file only touches the output file when content actually changes
# — ninja / make won't needlessly re-compile translation units that include
# version.h if nothing is different.
configure_file(
    ${SRC_DIR}/version.h.in
    ${BIN_DIR}/version.h
    @ONLY
)

if(CMAKE_SCRIPT_MODE_FILE)
    # Standalone -P invocation: quiet success.
else()
    message(STATUS "Firmware Version: ${GIT_VERSION}  Branch: ${GIT_BRANCH}  SHA: ${GIT_SHA}")
    message(STATUS "Build Date: ${BUILD_DATE}")
endif()
