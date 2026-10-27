cmake_minimum_required(VERSION 3.22)
# Must be set BEFORE project(): afterwards CMake ignores it and builds for the host OS version.
# Both are ignored outside Apple platforms.
set(CMAKE_OSX_DEPLOYMENT_TARGET 13.0 CACHE STRING "Minimum macOS version" FORCE)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
project(OscillaFX VERSION 0.5.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Platform split: shared sources live anywhere under app/Source; platform code lives in mac/ or
# win/ subfolders (audio/mac, platform/win, ui/crt/mac ...) and only the host's folder is built.
# See docs/PLATFORMS.md.
if(APPLE)
    enable_language(OBJCXX)
    set(OSCILLA_OS mac)
    # Keep build-machine paths (and user names) out of the binaries (__FILE__ in asserts and logs).
    add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.")
elseif(WIN32)
    set(OSCILLA_OS win)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "OscillaFX for Windows is 64-bit only: configure with -A x64")
    endif()
    # Static CRT: OscillaFX.exe needs no VC++ redistributable. Applies to JUCE and the engine too.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/utf-8 /MP)
    add_compile_definitions(_USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
else()
    message(FATAL_ERROR "OscillaFX builds on macOS and Windows only")
endif()

add_subdirectory(third_party/JUCE)
add_subdirectory(engine)

# App icon master (1024x1024 RGBA). Falls back to the pre-rename icon so the build never breaks.
# JUCE turns it into the .icns (macOS) or .ico (Windows).
set(OSCILLA_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icon/OscillaFX.png")
if(NOT EXISTS "${OSCILLA_APP_ICON}")
    message(WARNING "packaging/icon/OscillaFX.png missing; using packaging/icon/Oscilla.png")
    set(OSCILLA_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icon/Oscilla.png")
endif()

# Bundle settings are ignored on Windows.
juce_add_gui_app(OscillaFX
    PRODUCT_NAME "OscillaFX"
    COMPANY_NAME "OscillaFX"
    BUNDLE_ID com.oscilla.app
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "OscillaFX captures the OscillaFX Audio virtual device to apply sound enhancement."
    PLIST_TO_MERGE "<plist><dict><key>NSSupportsAutomaticGraphicsSwitching</key><true/></dict></plist>"
    ICON_BIG "${OSCILLA_APP_ICON}")

# Shared sources, plus the host platform's mac/ or win/ folders.
file(GLOB_RECURSE OSCILLA_ALL_SOURCES CONFIGURE_DEPENDS
    app/Source/*.cpp app/Source/*.mm app/Source/*.h)
set(OSCILLA_APP_SOURCES ${OSCILLA_ALL_SOURCES})
list(FILTER OSCILLA_APP_SOURCES EXCLUDE REGEX "/app/Source/(.+/)?(mac|win)/")
set(OSCILLA_PLATFORM_SOURCES ${OSCILLA_ALL_SOURCES})
list(FILTER OSCILLA_PLATFORM_SOURCES INCLUDE REGEX "/app/Source/(.+/)?${OSCILLA_OS}/")
list(APPEND OSCILLA_APP_SOURCES ${OSCILLA_PLATFORM_SOURCES})
target_sources(OscillaFX PRIVATE ${OSCILLA_APP_SOURCES})
target_include_directories(OscillaFX PRIVATE app/Source)

target_compile_definitions(OscillaFX PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:OscillaFX,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:OscillaFX,JUCE_VERSION>")

target_link_libraries(OscillaFX PRIVATE
    oscilla_dsp
    juce::juce_audio_devices juce::juce_audio_utils juce::juce_gui_extra juce::juce_opengl
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# Frameworks used by the mac/ sources: CoreAudio engine, LoginItem, MicrophoneAccess, ImageValidation.
set(OSCILLA_MAC_FRAMEWORKS
    "-framework CoreAudio" "-framework AudioToolbox" "-framework ServiceManagement"
    "-framework AVFoundation" "-framework ImageIO")
if(APPLE)
    target_link_libraries(OscillaFX PRIVATE ${OSCILLA_MAC_FRAMEWORKS})
endif()

# The target was called Oscilla before the rename; keep `--target Oscilla` working.
add_custom_target(Oscilla DEPENDS OscillaFX)

# Read-only resources: OscillaFX.app/Contents/Resources (macOS), next to OscillaFX.exe (Windows).
# Must match AppPaths::resourcesDirectory().
if(APPLE)
    set(OSCILLA_RESOURCES_DIR "$<TARGET_BUNDLE_CONTENT_DIR:OscillaFX>/Resources")
else()
    set(OSCILLA_RESOURCES_DIR "$<TARGET_FILE_DIR:OscillaFX>")
endif()

# ==== bundled presets =====================================
# Copies OscillaFX's own presets (presets/, written by engine make_preset) and the FxSound bonus
# .fac presets into <resources>/Presets (see core/Presets.cpp).
set(OSCILLA_BONUS_PRESET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/presets_fxsound"
    CACHE PATH "Directory with .fac presets bundled into the app")
file(GLOB OSCILLA_BONUS_PRESETS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/presets/*.fac" "${OSCILLA_BONUS_PRESET_DIR}/*.fac")
if(OSCILLA_BONUS_PRESETS)
    add_custom_command(TARGET OscillaFX POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OSCILLA_RESOURCES_DIR}/Presets"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OSCILLA_BONUS_PRESETS}
            "${OSCILLA_RESOURCES_DIR}/Presets"
        VERBATIM)
else()
    message(WARNING "No .fac presets in presets/ or ${OSCILLA_BONUS_PRESET_DIR}; app ships without presets")
endif()
# ==== end bundled presets =====================================================

# ==== crash watchdog ==========================================================
# macOS: helper/OscillaWatchdog -> OscillaFX.app/Contents/Helpers (platform/mac/CrashWatchdog_mac.cpp).
# Windows: helper/win/OscillaWatchdog.exe -> next to OscillaFX.exe (platform/win/CrashWatchdog_win.cpp).
if(APPLE)
    add_subdirectory(helper)
    add_dependencies(OscillaFX OscillaWatchdog)
    add_custom_command(TARGET OscillaFX POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:OscillaFX>/Helpers"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:OscillaWatchdog>"
            "$<TARGET_BUNDLE_CONTENT_DIR:OscillaFX>/Helpers/OscillaWatchdog"
        VERBATIM)
else()
    enable_testing()
    add_subdirectory(helper/win)
    add_dependencies(OscillaFX OscillaWatchdog)
    add_custom_command(TARGET OscillaFX POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:OscillaWatchdog>" "$<TARGET_FILE_DIR:OscillaFX>"
        VERBATIM)
endif()
# ==== end crash watchdog ======================================================

# ==== gallery images  =======================================
# assets/gallery/* -> <resources>/Gallery. Missing or empty dir is fine.
file(GLOB OSCILLA_GALLERY_FILES LIST_DIRECTORIES false CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/gallery/*")
if(OSCILLA_GALLERY_FILES)
    add_custom_command(TARGET OscillaFX POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OSCILLA_RESOURCES_DIR}/Gallery"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OSCILLA_GALLERY_FILES}
            "${OSCILLA_RESOURCES_DIR}/Gallery"
        VERBATIM)
endif()
# ==== end gallery images ======================================================

# ==== engine_selftest  ==============================
# Headless checks: device listing, ProfileStore, ring buffer, drift resampler, DSP mapping.
# Builds every non-UI app source (audio, core, profiles, platform) so they compile even before the UI target links.
juce_add_console_app(engine_selftest PRODUCT_NAME "engine_selftest")
set(OSCILLA_SELFTEST_SOURCES ${OSCILLA_APP_SOURCES})
list(FILTER OSCILLA_SELFTEST_SOURCES INCLUDE REGEX "/app/Source/(audio|core|profiles|platform)/.*\\.(cpp|mm)$")
target_sources(engine_selftest PRIVATE app/Tests/EngineSelfTest.cpp ${OSCILLA_SELFTEST_SOURCES})
target_include_directories(engine_selftest PRIVATE app/Source)
target_compile_definitions(engine_selftest PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1) # tests pump the message loop
target_link_libraries(engine_selftest PRIVATE
    oscilla_dsp juce::juce_core juce::juce_events
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
if(APPLE)
    target_link_libraries(engine_selftest PRIVATE ${OSCILLA_MAC_FRAMEWORKS} "-framework AppKit")
else()
    target_link_libraries(engine_selftest PRIVATE juce::juce_graphics) # platform/win/ImageValidation_win.cpp
endif()
enable_testing()
# Run from the repo root: on macOS __FILE__ is repo-relative (-ffile-prefix-map), and the dev fallbacks
# for presets and gallery (core/Presets.cpp, core/GalleryStore.cpp) resolve against it.
add_test(NAME engine_selftest COMMAND engine_selftest WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
# ==== end engine_selftest =====================================================

# ==== UI preview  ================================================
# cmake -S . -B build-ui -DOSCILLA_UI_STUB=ON && cmake --build build-ui --target OscillaFX
# Swaps the real audio engine for app/Source/ui/dev/ControllerStub.cpp (fake signal) and the
# microphone permission check for ui/dev/MicrophoneAccessStub.cpp (OSCILLA_STUB_MIC_DENIED=1 -> denied).
if(OSCILLA_UI_STUB)
    target_compile_definitions(OscillaFX PRIVATE OSCILLA_UI_STUB=1)
    # Real sources left out of the UI preview build (stand-ins live in app/Source/ui/dev).
    set(OSCILLA_UI_STUB_EXCLUDED
        app/Source/audio/mac/AudioEngine_mac.cpp app/Source/platform/mac/MicrophoneAccess_mac.mm
        app/Source/audio/win/AudioEngine_win.cpp app/Source/platform/win/MicrophoneAccess_win.cpp)
    set_source_files_properties(${OSCILLA_UI_STUB_EXCLUDED} PROPERTIES HEADER_FILE_ONLY TRUE)
endif()
# ==== end UI preview ==========================================================

# ==== crt_trace_test  ===========================================
# Offline check of the CRT's wave / X-Y trace generation (ui/crt/CrtTraces): a fuzzy trace fails.
juce_add_console_app(crt_trace_test PRODUCT_NAME "crt_trace_test")
target_sources(crt_trace_test PRIVATE app/Tests/CrtTraceTest.cpp app/Source/ui/crt/CrtTraces.cpp)
target_include_directories(crt_trace_test PRIVATE app/Source)
target_compile_definitions(crt_trace_test PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(crt_trace_test PRIVATE
    juce::juce_gui_basics
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
add_test(NAME crt_trace_test COMMAND crt_trace_test)
# ==== end crt_trace_test ======================================================

# ==== crt_cpu_bench  ============================================
# Frame time of the CPU CRT renderer (the one Windows uses) at 100 / 150 / 200 % display scaling.
juce_add_console_app(crt_cpu_bench PRODUCT_NAME "crt_cpu_bench")
target_sources(crt_cpu_bench PRIVATE app/Tests/CrtCpuBench.cpp app/Source/ui/crt/CpuCrtRenderer.cpp app/Source/ui/PhosphorScreen.cpp)
target_include_directories(crt_cpu_bench PRIVATE app/Source)
target_compile_definitions(crt_cpu_bench PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(crt_cpu_bench PRIVATE
    juce::juce_gui_basics
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
add_test(NAME crt_cpu_bench COMMAND crt_cpu_bench)
# ==== end crt_cpu_bench =======================================================
