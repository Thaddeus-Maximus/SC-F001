# version.cmake
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

execute_process(
    COMMAND git rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
    OUTPUT_VARIABLE GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

string(TIMESTAMP BUILD_DATE "%Y-%m-%d %H:%M:%S" UTC)

# Fallback if not in a git repo
if(NOT GIT_VERSION)
    set(GIT_VERSION "unknown")
endif()

if(NOT GIT_BRANCH)
    set(GIT_BRANCH "unknown")
endif()

message(STATUS "Firmware Version: ${GIT_VERSION}")
message(STATUS "Git Branch: ${GIT_BRANCH}")
message(STATUS "Build Date: ${BUILD_DATE}")

configure_file(
    ${CMAKE_CURRENT_LIST_DIR}/version.h.in
    ${CMAKE_BINARY_DIR}/version.h
    @ONLY
)