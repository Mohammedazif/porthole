add_executable(test_hid_reports test_hid_reports.c)
target_link_libraries(test_hid_reports PRIVATE porthole_core)
porthole_set_warnings(test_hid_reports)
add_test(NAME hid_reports COMMAND test_hid_reports)

add_test(NAME cli_version COMMAND porthole --version)
set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "Porthole ${PROJECT_VERSION}")

add_test(NAME cli_rejects_unknown_option COMMAND porthole --no-such-option)
set_tests_properties(cli_rejects_unknown_option PROPERTIES WILL_FAIL TRUE)
