configure_file(core/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/core/version.h @ONLY)

# Platform-independent code shared by the app and the tests.
add_library(porthole_core STATIC
    core/log.c
    hid/hid_reports.c
)
target_include_directories(porthole_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
porthole_set_warnings(porthole_core)

add_executable(porthole main.c)
target_link_libraries(porthole PRIVATE porthole_core)
porthole_set_warnings(porthole)

if(PORTHOLE_WITH_AIRPLAY)
    target_link_libraries(porthole PRIVATE porthole_airplay_core)
    target_compile_definitions(porthole PRIVATE PORTHOLE_HAVE_AIRPLAY=1)
endif()

if(MINGW)
    # Ship a self-contained .exe: no dependency on MinGW's libgcc DLL.
    target_link_options(porthole PRIVATE -static-libgcc)
endif()
