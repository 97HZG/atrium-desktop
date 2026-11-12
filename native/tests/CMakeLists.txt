add_executable(display_settings_tests
    display_settings.cpp
    ../core/shell/displays.cpp
    ../core/settings.cpp
    ../core/log.cpp
    ../core/shell/logon_task.cpp)
target_include_directories(display_settings_tests PRIVATE ../core)
target_compile_definitions(display_settings_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN ATRIUM_DEVELOPMENT)
target_link_libraries(display_settings_tests PRIVATE windowsapp user32 shell32 ole32 oleaut32 taskschd advapi32)
add_test(NAME display_settings COMMAND display_settings_tests)

add_executable(app_processes_tests
    app_processes.cpp
    ../core/shell/app_processes.cpp
    ../core/log.cpp)
target_include_directories(app_processes_tests PRIVATE ../core)
target_compile_definitions(app_processes_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
add_test(NAME app_processes COMMAND app_processes_tests)

add_executable(tray_match_tests tray_match.cpp ../core/shell/tray_match.cpp)
target_include_directories(tray_match_tests PRIVATE ../core)
add_test(NAME tray_match COMMAND tray_match_tests)

add_executable(tray_click_tests tray_click.cpp)
target_include_directories(tray_click_tests PRIVATE ../core)
add_test(NAME tray_click COMMAND tray_click_tests)

add_executable(tray_notify_probe EXCLUDE_FROM_ALL tray_notify_probe.cpp)
target_link_libraries(tray_notify_probe PRIVATE ole32)

# Manual, read-only by default. Explicit --click uses the actual TrayModel worker.
add_executable(tray_live_probe EXCLUDE_FROM_ALL tray_live_probe.cpp ../core/shell/tray_match.cpp ../core/log.cpp)
target_include_directories(tray_live_probe PRIVATE ../core)
target_compile_definitions(tray_live_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(tray_live_probe PRIVATE windowsapp user32 shell32 ole32 oleaut32 uuid shlwapi windowscodecs version)
