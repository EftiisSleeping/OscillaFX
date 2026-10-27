# Crash watchdog (Windows): restores the default playback endpoint if OscillaFX dies while routing.
# No JUCE. Copied next to OscillaFX.exe (spawned by app/Source/platform/win/CrashWatchdog_win.cpp).
add_executable(OscillaWatchdog OscillaWatchdog.cpp)
target_include_directories(OscillaWatchdog PRIVATE ${CMAKE_SOURCE_DIR}/app/Source)
target_compile_definitions(OscillaWatchdog PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(OscillaWatchdog PRIVATE /W4)
target_link_libraries(OscillaWatchdog PRIVATE ole32)

# Dry-run tests: a fake app spawns the helper and dies with a restore record left behind.
add_executable(watchdog_test test_watchdog.cpp)
target_compile_definitions(watchdog_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
add_test(NAME watchdog_test COMMAND watchdog_test $<TARGET_FILE:OscillaWatchdog>)
