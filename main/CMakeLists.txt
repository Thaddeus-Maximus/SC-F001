# See the build system documentation in IDF programming guide
# for more information about component CMakeLists.txt files.

# Seed version.h at configure time so it exists for the first build.
include(${CMAKE_CURRENT_LIST_DIR}/version.cmake)

idf_component_register(
    SRCS main.c log_test.c partition_test.c i2c.c rtc.c storage.c uart_comms.c control_fsm.c power_mgmt.c bringup.c rf_433.c rtc.c sensors.c solar.c webserver.c simple_dns_server.c comms.c bt_hid.c # list the source files of this component
    INCLUDE_DIRS "." "${CMAKE_BINARY_DIR}"
    PRIV_INCLUDE_DIRS   # optional, add here private include directories
    
    REQUIRES driver esp_http_server esp_netif lwip json esp_timer esp_adc app_update esp_wifi nvs_flash mdns bt esp_hid    # optional, list the public requirements (component names)
    # esp_https_server 
    PRIV_REQUIRES  # optional, list the private requirements
    #EMBED_TXTFILES servercert.pem prvtkey.pem
)


if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    add_custom_command(
        OUTPUT ${COMPONENT_DIR}/webpage.h
        COMMAND python ${COMPONENT_DIR}/webpage_compile.py
        DEPENDS ${COMPONENT_DIR}/webpage.html ${COMPONENT_DIR}/webpage_compile.py
        WORKING_DIRECTORY ${COMPONENT_DIR}
        COMMENT "Generating webpage.h from landingpage.html"
        VERBATIM
    )

    add_custom_target(generate_webpage_h DEPENDS ${COMPONENT_DIR}/webpage.h)
    add_dependencies(${COMPONENT_LIB} generate_webpage_h)

    # Regenerate version.h on every build so git SHA / dirty flag stay
    # current as commits land between builds. configure_file inside
    # version.cmake is a no-op when the content matches, so rebuilds only
    # cascade when something actually changed.
    add_custom_target(update_version ALL
        COMMAND ${CMAKE_COMMAND}
            -DSRC_DIR=${COMPONENT_DIR}
            -DBIN_DIR=${CMAKE_BINARY_DIR}
            -P ${COMPONENT_DIR}/version.cmake
        BYPRODUCTS ${CMAKE_BINARY_DIR}/version.h
        COMMENT "Refreshing version.h from git"
    )
    add_dependencies(${COMPONENT_LIB} update_version)
endif()