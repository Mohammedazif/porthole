cmake_minimum_required(VERSION 3.21)

project(porthole
    VERSION 0.1.4
    DESCRIPTION "Share and control your iPhone screen from a PC"
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

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND PORTHOLE_WITH_AIRPLAY)
    include(GNUInstallDirs)
    install(TARGETS porthole RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(PROGRAMS helpers/ble-bluez/porthole_ble.py
        DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/porthole")
    install(FILES packaging/linux/porthole.desktop
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    foreach(size IN ITEMS 32 48 64 128 256)
        install(FILES "branding/porthole/app-icons/dark/porthole-app-${size}.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps"
            RENAME porthole.png)
    endforeach()
    install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/porthole" RENAME copyright)
    install(FILES third_party/UxPlay/LICENSE
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/porthole" RENAME UxPlay-LICENSE)
    install(FILES src/ui/fonts/LICENSE
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/porthole" RENAME Geist-LICENSE)

    set(CPACK_GENERATOR DEB)
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/Mohammedazif/porthole")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Porthole maintainers")
    set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS
        "avahi-daemon, bluez, python3, python3-dbus, python3-gi, xdg-utils")
    include(CPack)
endif()

message(STATUS "Porthole ${PROJECT_VERSION} (${CMAKE_BUILD_TYPE}): "
               "airplay=${PORTHOLE_WITH_AIRPLAY} tests=${PORTHOLE_BUILD_TESTS}")
