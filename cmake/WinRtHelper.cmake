# A separate .NET process owns WinRT so the native mirror keeps its C/MSYS2 toolchain.
option(PORTHOLE_WITH_WINDOWS_BLUETOOTH "Publish the Windows WinRT Bluetooth helper" ON)
if(PORTHOLE_WITH_WINDOWS_BLUETOOTH)
    target_compile_definitions(porthole PRIVATE PORTHOLE_HAVE_WINDOWS_BLUETOOTH=1)
    find_program(DOTNET_EXECUTABLE dotnet REQUIRED)
    set(_ble_project "${PROJECT_SOURCE_DIR}/helpers/ble-winrt/Porthole.Ble.WinRT.csproj")
    file(GLOB_RECURSE _ble_sources CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/helpers/ble-winrt/*.cs"
        "${PROJECT_SOURCE_DIR}/helpers/ble-winrt/*.csproj"
        "${PROJECT_SOURCE_DIR}/helpers/ble-winrt/README.md"
        "${PROJECT_SOURCE_DIR}/helpers/ble-winrt/licenses/*")
    set(_ble_output "${CMAKE_CURRENT_BINARY_DIR}/ble-winrt")
    add_custom_command(
        OUTPUT "${_ble_output}/porthole-ble-winrt.exe"
        COMMAND "${DOTNET_EXECUTABLE}" publish "${_ble_project}"
            --configuration Release --runtime win-x64 --self-contained true
            --artifacts-path "${PROJECT_BINARY_DIR}/dotnet" --output "${_ble_output}"
        DEPENDS ${_ble_sources}
            "${PROJECT_SOURCE_DIR}/src/hid/hid_reports.c"
            "${PROJECT_SOURCE_DIR}/src/hid/hid_reports.h"
        COMMENT "Publishing Windows Bluetooth HID helper (.NET 10, self-contained)"
        VERBATIM)
    add_custom_target(porthole_ble_winrt ALL DEPENDS "${_ble_output}/porthole-ble-winrt.exe")
    add_dependencies(porthole porthole_ble_winrt)
endif()
