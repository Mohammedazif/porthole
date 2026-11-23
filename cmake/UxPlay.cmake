# Builds UxPlay's AirPlay receiver core (third_party/UxPlay/lib) as the static library `airplay`
# and exposes it to Porthole as the interface target `porthole_airplay_core`.
#
# Only UxPlay's protocol core is used (RTSP server, pairing, FairPlay, stream decryption);
# its GStreamer renderers and uxplay.cpp front end are not built.
# UxPlay is GPL-3.0, which is why Porthole is GPL-3.0 too (see third_party/UxPlay/LICENSE).

set(UXPLAY_DIR ${PROJECT_SOURCE_DIR}/third_party/UxPlay)

if(NOT EXISTS ${UXPLAY_DIR}/lib/raop.h)
    message(FATAL_ERROR
        "UxPlay sources not found in ${UXPLAY_DIR}.\n"
        "Run: git submodule update --init --recursive")
endif()

# Release binaries must run on other PCs, so never let UxPlay add -march=native.
set(NO_MARCH_NATIVE ON CACHE BOOL "Disable -march=native in UxPlay's core" FORCE)

set(_uxplay_bin ${CMAKE_BINARY_DIR}/third_party/uxplay)
add_subdirectory(${UXPLAY_DIR}/lib/llhttp   ${_uxplay_bin}/llhttp   EXCLUDE_FROM_ALL)
add_subdirectory(${UXPLAY_DIR}/lib/playfair ${_uxplay_bin}/playfair EXCLUDE_FROM_ALL)
if(WIN32)
    # Upstream's CMake requires Apple's Bonjour SDK and its dnssd.c loads dnssd.dll.
    # Keep the pinned sources intact and replace only discovery with the Windows
    # DNS API. The rest of UxPlay already supports MinGW/winpthreads.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(UXPLAY_PLIST REQUIRED IMPORTED_TARGET libplist-2.0)
    find_package(OpenSSL 1.1.1 REQUIRED)
    find_package(Threads REQUIRED)
    file(GLOB _uxplay_sources CONFIGURE_DEPENDS ${UXPLAY_DIR}/lib/*.c)
    list(REMOVE_ITEM _uxplay_sources ${UXPLAY_DIR}/lib/dnssd.c ${UXPLAY_DIR}/lib/raop_buffer.c)
    add_library(airplay STATIC ${_uxplay_sources}
        ${PROJECT_SOURCE_DIR}/src/airplay/raop_buffer.c
        ${PROJECT_SOURCE_DIR}/src/airplay/dnssd_records.c
        ${PROJECT_SOURCE_DIR}/src/airplay/dnssd_win.c)
    target_include_directories(airplay PRIVATE ${UXPLAY_DIR}/lib
        ${UXPLAY_DIR}/lib/playfair ${UXPLAY_DIR}/lib/llhttp ${PROJECT_SOURCE_DIR}/src)
    target_compile_definitions(airplay PRIVATE WIN32=1 _WIN32_WINNT=0x0A00 NOHOLD
        STANDALONE OPENSSL_API_COMPAT=0x10101000L)
    if(UXPLAY_PLIST_VERSION VERSION_GREATER_EQUAL 2.1)
        target_compile_definitions(airplay PRIVATE PLIST_210)
    endif()
    if(UXPLAY_PLIST_VERSION VERSION_GREATER_EQUAL 2.3)
        target_compile_definitions(airplay PRIVATE PLIST_230)
    endif()
    target_link_libraries(airplay PUBLIC playfair llhttp Threads::Threads
        PkgConfig::UXPLAY_PLIST OpenSSL::Crypto ws2_32 iphlpapi dnsapi)
else()
    add_subdirectory(${UXPLAY_DIR}/lib ${_uxplay_bin}/airplay EXCLUDE_FROM_ALL)
    # Same bounded audio-resend wait as Windows: build Porthole's raop_buffer.c
    # in place of upstream's, leaving the pinned submodule untouched.
    get_target_property(_airplay_sources airplay SOURCES)
    list(FILTER _airplay_sources EXCLUDE REGEX "(^|/)raop_buffer\\.c$")
    set_property(TARGET airplay PROPERTY SOURCES ${_airplay_sources}
        ${PROJECT_SOURCE_DIR}/src/airplay/raop_buffer.c)
    target_include_directories(airplay PRIVATE ${UXPLAY_DIR}/lib)
endif()

include(UxPlayVerifiedIdentity)

if(UNIX AND NOT APPLE)
    # playfair calls sin(); UxPlay's own app only gets libm indirectly through GStreamer.
    target_link_libraries(playfair PUBLIC m)
endif()

add_library(porthole_airplay_core INTERFACE)
target_link_libraries(porthole_airplay_core INTERFACE airplay)
target_include_directories(porthole_airplay_core SYSTEM INTERFACE ${UXPLAY_DIR}/lib)
