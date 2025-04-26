cmake_minimum_required(VERSION 3.10)
project(ur-mavdiscovery C)

# Set compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -g -O2 -w -Wno-unused-parameter -Wno-unused-result -D_DEBUG_MODE -D_GNU_SOURCE")
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)


add_subdirectory(thirdparty/ur-rpc-template/deps/cJSON)
add_subdirectory(thirdparty/ur-rpc-template)

add_library(ur_mavdis STATIC
    spec/libmavlink.c
    spec/cssl.c
    spec/ur-discovery.c
)

# Include directories
target_include_directories(ur_mavdis PRIVATE
    spec
    ../thirdparty/c_library_v1/common
    ../thirdparty/c_library_v1
    ../thirdparty/ur-rpc-template
    ../thirdparty/ur-rpc-template/deps/cJSON
)
target_link_libraries(ur_mavdis PRIVATE udev ur-rpc-template cJSON mqtt-client-static)


# Include directories
include_directories(
    src
    spec
    ../thirdparty/c_library_v1/common
    ../thirdparty/c_library_v1
    ../thirdparty/ur-rpc-template
    ../thirdparty/ur-rpc-template/deps/cJSON
)

# Source files
set(SRC_FILES
    src/main.c
)

# Add executable
add_executable(${PROJECT_NAME} ${SRC_FILES})

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE udev pthread ur-rpc-template cJSON ur_mavdis)

# Installation configuration
set(INSTALL_BIN_DIR bin CACHE PATH "Installation directory for binaries")
set(INSTALL_CONFIG_DIR etc/ur-mavdiscovery CACHE PATH "Installation directory for configuration")

install(TARGETS ${PROJECT_NAME} 
        DESTINATION ${INSTALL_BIN_DIR}
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

install(FILES config/mavdiscovery-config.json
        DESTINATION ${INSTALL_CONFIG_DIR}
        PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)

# Add uninstall target
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    IMMEDIATE @ONLY)

add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)
