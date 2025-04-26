cmake_minimum_required(VERSION 3.10)
project(mqtt-client C)

# Generate position independent code (-fPIC)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Specify C and C++ compilers
set(CMAKE_C_COMPILER "gcc")
set(CMAKE_CXX_COMPILER "g++")

# Source files
file(GLOB SOURCES 
    "src/*.c"
    "src/**/*.c"
)

# Header files
file(GLOB HEADERS
    "include/*.h"
    "include/**/*.h"
)

# Create shared library
add_library(mqtt-client-shared SHARED ${SOURCES} ${HEADERS})

# Create static library
add_library(mqtt-client-static STATIC ${SOURCES} ${HEADERS})

# Include directories for shared library
target_include_directories(mqtt-client-shared
    PUBLIC 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Include directories for static library
target_include_directories(mqtt-client-static
    PUBLIC 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Set library output names
set_target_properties(mqtt-client-shared PROPERTIES
    OUTPUT_NAME "mqtt-client-shared"
    VERSION 1.0.0
    SOVERSION 1
)

set_target_properties(mqtt-client-static PROPERTIES
    OUTPUT_NAME "mqtt-clien-statict"
)

# Install rules for shared library
install(TARGETS mqtt-client-shared
    LIBRARY DESTINATION lib
    PUBLIC_HEADER DESTINATION include
)

# Install rules for static library
install(TARGETS mqtt-client-static
    ARCHIVE DESTINATION lib
    PUBLIC_HEADER DESTINATION include
)
