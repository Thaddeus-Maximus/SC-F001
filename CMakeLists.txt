# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# Firmware version — main/version.txt is the single source of truth.
# main/version.cmake generates version.h from the same file; setting
# PROJECT_VER here (it must be set before project()) puts the version into
# esp_app_desc_t.version, so `esptool`/OTA tooling can read it straight out
# of an unflashed .bin.
file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/main/version.txt PROJECT_VER LIMIT_COUNT 1)
string(STRIP "${PROJECT_VER}" PROJECT_VER)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(SC-F001)

# PROJECT_VER above is captured at configure time, so without this a bumped
# version.txt would refresh version.h (regenerated every build) while the app
# descriptor kept the stale value until someone ran `idf.py reconfigure`.
# Listing the file here makes the generator re-run cmake when it changes.
set_property(DIRECTORY APPEND PROPERTY
    CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/main/version.txt)

# Emit a versioned copy of the app image next to the stock one:
#   build/SC-F001.bin        <- IDF's own name, used by flash / partition tooling
#   build/SC-F001-1.1.0.bin  <- what ota_deploy.py picks up and what gets archived
# Copying rather than renaming keeps every IDF flash path working untouched,
# and leaves old versions in build/ so you can re-deploy one without a rebuild.
# `app` is IDF's ALL target that depends on gen_project_binary, so POST_BUILD
# here runs after the .bin exists.
if(TARGET app)
    add_custom_command(TARGET app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin"
                "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}-${PROJECT_VER}.bin"
        COMMENT "Versioned image: ${CMAKE_PROJECT_NAME}-${PROJECT_VER}.bin")
endif()
