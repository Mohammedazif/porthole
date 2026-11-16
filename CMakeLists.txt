cmake_minimum_required(VERSION 3.21)

project(porthole
    VERSION 0.1.0
    DESCRIPTION "Mirror and control an iPhone from a Windows PC"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MINGW)
    # C99 formatting (including size_t diagnostics) for the app and native tests.
    add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(PORTHOLE_WITH_AIRPLAY "Build the Wi-Fi receiver on UxPlay's AirPlay core" ON)
option(PORTHOLE_BUILD_TESTS "Build unit tests" ON)
option(PORTHOLE_WARNINGS_AS_ERRORS "Treat warnings in Porthole's own code as errors" OFF)

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
include(CompilerWarnings)

if(PORTHOLE_WITH_AIRPLAY)
    include(UxPlay)
endif()

add_subdirectory(src)

if(PORTHOLE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "Porthole ${PROJECT_VERSION} (${CMAKE_BUILD_TYPE}): "
               "airplay=${PORTHOLE_WITH_AIRPLAY} tests=${PORTHOLE_BUILD_TESTS}")
