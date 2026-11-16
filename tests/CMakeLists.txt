add_executable(test_hid_reports test_hid_reports.c)
target_link_libraries(test_hid_reports PRIVATE porthole_core)
porthole_set_warnings(test_hid_reports)
add_test(NAME hid_reports COMMAND test_hid_reports)

add_executable(test_crop test_crop.c)
target_link_libraries(test_crop PRIVATE porthole_core)
porthole_set_warnings(test_crop)
add_test(NAME crop COMMAND test_crop)

add_executable(test_keyboard test_keyboard.c)
target_link_libraries(test_keyboard PRIVATE porthole_core)
porthole_set_warnings(test_keyboard)
add_test(NAME keyboard COMMAND test_keyboard)

add_executable(test_input test_input.c)
target_link_libraries(test_input PRIVATE porthole_core)
porthole_set_warnings(test_input)
add_test(NAME input COMMAND test_input)

add_executable(test_scroll test_scroll.c)
target_link_libraries(test_scroll PRIVATE porthole_core)
porthole_set_warnings(test_scroll)
add_test(NAME scroll COMMAND test_scroll)

add_executable(test_scrollbar test_scrollbar.c)
target_link_libraries(test_scrollbar PRIVATE porthole_core)
porthole_set_warnings(test_scrollbar)
add_test(NAME scrollbar COMMAND test_scrollbar)

add_executable(test_cursor test_cursor.c)
target_link_libraries(test_cursor PRIVATE porthole_core)
porthole_set_warnings(test_cursor)
add_test(NAME cursor COMMAND test_cursor)

add_executable(test_pointer test_pointer.c)
target_link_libraries(test_pointer PRIVATE porthole_core)
porthole_set_warnings(test_pointer)
add_test(NAME pointer COMMAND test_pointer)

if(PORTHOLE_WITH_AIRPLAY)
    add_executable(test_audio test_audio.c)
    target_link_libraries(test_audio PRIVATE porthole_app)
    porthole_set_warnings(test_audio)
    add_test(NAME audio COMMAND test_audio)
    set_tests_properties(audio PROPERTIES TIMEOUT 15)
endif()

if(UNIX AND PORTHOLE_WITH_AIRPLAY)
    # The Linux app uses Python/BlueZ; this check needs no adapter or D-Bus service.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME ble_sender
        COMMAND ${Python3_EXECUTABLE} -m unittest discover
                -s ${CMAKE_CURRENT_SOURCE_DIR} -p test_ble_sender.py -v)
    add_executable(test_ble_reconnect test_ble_reconnect.c)
    target_link_libraries(test_ble_reconnect PRIVATE porthole_app)
    target_compile_definitions(test_ble_reconnect PRIVATE
        PORTHOLE_TEST_BLE_HELPER="${CMAKE_CURRENT_SOURCE_DIR}/fake_ble_helper.py")
    porthole_set_warnings(test_ble_reconnect)
    add_test(NAME ble_reconnect COMMAND test_ble_reconnect)
    set_tests_properties(ble_reconnect PROPERTIES TIMEOUT 15)

    add_executable(test_app_pointer test_app_pointer.c)
    target_link_libraries(test_app_pointer PRIVATE porthole_app)
    porthole_set_warnings(test_app_pointer)
    add_test(NAME app_pointer COMMAND test_app_pointer)
    add_test(NAME app_drag COMMAND test_app_pointer --drag)
    set_tests_properties(app_pointer PROPERTIES TIMEOUT 10)
    set_tests_properties(app_drag PROPERTIES TIMEOUT 10)
endif()

add_test(NAME cli_version COMMAND porthole --version)
set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "Porthole ${PROJECT_VERSION}")

add_test(NAME cli_rejects_unknown_option COMMAND porthole --no-such-option)
set_tests_properties(cli_rejects_unknown_option PROPERTIES WILL_FAIL TRUE)
