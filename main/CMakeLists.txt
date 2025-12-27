# See the build system documentation in IDF programming guide
# for more information about component CMakeLists.txt files.

idf_component_register(
    SRCS main.c i2c.c rtc.c storage.c uart_comms.c control_fsm.c power_mgmt.c rf_433.c rtc.c sensors.c solar.c webserver.c # list the source files of this component
    INCLUDE_DIRS        # optional, add here public include directories
    PRIV_INCLUDE_DIRS   # optional, add here private include directories
    REQUIRES            # optional, list the public requirements (component names)
    PRIV_REQUIRES       # optional, list the private requirements
)


if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    add_custom_command(
        OUTPUT ${COMPONENT_DIR}/webpage.h
        COMMAND python ${COMPONENT_DIR}/webpage_compile.py
        DEPENDS ${COMPONENT_DIR}/landingpage.html ${COMPONENT_DIR}/webpage_compile.py
        WORKING_DIRECTORY ${COMPONENT_DIR}
        COMMENT "Generating webpage.h from landingpage.html"
        VERBATIM
    )

    add_custom_target(generate_webpage_h DEPENDS ${COMPONENT_DIR}/webpage.h)
    add_dependencies(${COMPONENT_LIB} generate_webpage_h)
endif()