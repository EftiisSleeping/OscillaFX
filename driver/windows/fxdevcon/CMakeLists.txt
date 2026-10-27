# fxdevcon.exe: FxSound's device install tool, built from its unchanged sources in src/.
#   fxdevcon install <inf> [hwid]   create the root devnode (default Root\FXVAD) and install the INF on it
#   fxdevcon remove [hwid]          remove the first devnode with that hardware id
# Prints "Success" and exits 0, otherwise prints a failure line and exits -1 (also when a reboot is needed).
# Standalone project so the app build stays untouched: packaging/windows/build_installer.ps1 builds it.
cmake_minimum_required(VERSION 3.22)
project(fxdevcon LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "fxdevcon is a Windows tool")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # no VC++ redistributable

add_executable(fxdevcon src/fxdevcon/fxdevcon.cpp src/DfxInstall/DfxInstall.cpp)
target_include_directories(fxdevcon PRIVATE src/DfxInstall)
# Same settings as FxSound's fxdevcon.vcxproj: Unicode console app, /W3 /sdl.
target_compile_definitions(fxdevcon PRIVATE UNICODE _UNICODE _CONSOLE)
target_compile_options(fxdevcon PRIVATE /W3 /sdl /utf-8)
# DfxInstall.cpp also holds FxSound's MSI custom actions (Msi.lib, wbemuuid.lib come from #pragma comment).
target_link_libraries(fxdevcon PRIVATE setupapi newdev ole32 oleaut32)
