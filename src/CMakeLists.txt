configure_file(core/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/core/version.h @ONLY)

# Platform-independent code shared by the app and the tests.
add_library(porthole_core STATIC
    core/config.c
    core/metrics.c
    control/keyboard.c
    control/input.c
    control/mouse_input.c
    control/gesture.c
    control/abs_pointer.c
    control/pointer.c
    control/scroll.c
    control/setup.c
    core/log.c
    hid/hid_reports.c
    video/crop.c
    video/scrollbar.c
    video/cursor.c
    video/codecs.c
    audio/sync.c
    control/paste.c
    security/gate.c
    session/devices.c
    touch/wda.c
    ui/placement.c
    ui/tray.c
    files/picture_hash.c
    files/pull.c
    usb/packet.c
    usb/switch.c
    usb/winusb.c
    usb/capture.c
    usb/sample.c
)
target_include_directories(porthole_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
if(UNIX)
    target_link_libraries(porthole_core PUBLIC m)
endif()
porthole_set_warnings(porthole_core)

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
endif()
if(TARGET PkgConfig::LIBUSB)
    target_compile_definitions(porthole_core PUBLIC PH_USB_HAVE_LIBUSB=1)
    target_link_libraries(porthole_core PUBLIC PkgConfig::LIBUSB)
endif()

add_executable(porthole main.c)
target_link_libraries(porthole PRIVATE porthole_core)
porthole_set_warnings(porthole)

if(PORTHOLE_WITH_AIRPLAY)
    # The full app: AirPlay receiver -> FFmpeg decoder -> SDL2 window.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale libswresample libavformat)
    find_package(OpenSSL REQUIRED)
    find_package(Threads REQUIRED)

    add_library(porthole_app STATIC
        app.c
        airplay/receiver.c
        audio/player.c
        hid/link.c
        ui/window.c
        ui/generated/brand_art.c
        ui/chrome.c
        video/decoder.c
        media/shot.c
        media/record.c
        files/origin.c
        files/xdnd.c
        control/paste_sdl.c
        touch/wda_net.c
        polish/runtime.c
    )
    target_link_libraries(porthole_app PUBLIC
        porthole_core
        porthole_airplay_core
        PkgConfig::SDL2
        PkgConfig::FFMPEG
        OpenSSL::Crypto
        Threads::Threads
    )
    if(WIN32)
        target_link_libraries(porthole_app PUBLIC ws2_32)
    endif()
    if(UNIX AND NOT APPLE)
        find_package(X11)
        if(X11_FOUND)
            target_compile_definitions(porthole_app PRIVATE PH_HAVE_X11=1)
            target_link_libraries(porthole_app PUBLIC X11::X11)
        endif()
    endif()
    porthole_set_warnings(porthole_app)

    target_link_libraries(porthole PRIVATE porthole_app)
    target_compile_definitions(porthole PRIVATE PORTHOLE_HAVE_AIRPLAY=1)
    if(WIN32)
        include(WinRtHelper)
    else()
        target_compile_definitions(porthole PRIVATE
        # Development default; an installed build will look next to the executable.
        PORTHOLE_BLE_HELPER="${PROJECT_SOURCE_DIR}/helpers/ble-bluez/porthole_ble.py")
    endif()
endif()

if(MINGW)
    # Ship a self-contained .exe: no dependency on MinGW's libgcc DLL.
    target_link_options(porthole PRIVATE -static-libgcc)
endif()
