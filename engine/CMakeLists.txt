cmake_minimum_required(VERSION 3.16)
project(oscilla_engine C CXX)

# Standalone engine build on Windows (the top-level CMakeLists sets the same): static CRT (/MT)
# so nothing needs the VC++ redistributable.
if(MSVC AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/utf-8 /MP)
    add_compile_definitions(_USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS ON)

# Sanitizers: cmake -DOSCILLA_SANITIZE=ON (ASan + UBSan)
option(OSCILLA_SANITIZE "Build with AddressSanitizer + UBSanitizer" OFF)
set(OSCILLA_PRESET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../presets_fxsound"
    CACHE PATH "Directory with .fac presets used by dsp_offline")

# Legacy DSP: `long` was rewritten to `int` in dsp/ and support/ (Windows long is 32-bit).
# Do not reintroduce `long` there.
if(OSCILLA_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

set(OSCILLA_DSP_SOURCES
    dsp/DfxDsp.cpp
    dsp/DfxDspEq.cpp
    dsp/DfxDspPreset.cpp
    dsp/DfxDspPrivate.cpp
    dsp/DfxDspRegistry.cpp
    dsp/ptComSftDfx/Comsftwr.c
    dsp/ptComSftDfx/ComsftwrCPP.cpp
    dsp/ptechDsp/Aural/Aural0/Auralp.c
    dsp/ptechDsp/Aural/Aural032/Auralp32.c
    dsp/ptechDsp/Dly32/Dly832/dly8p32.c
    dsp/ptechDsp/Dly8/Dly8p.c
    dsp/ptechDsp/Lex/Lex16/Lex16.c
    dsp/ptechDsp/Lex/Lex32/Lex32.c
    dsp/ptechDsp/Maximizer/Maxi16/Maxi16.c
    dsp/ptechDsp/Maximizer/Maxi32/Maxi32.c
    dsp/ptechDsp/Peq/Peq8/peq8p.c
    dsp/ptechDsp/Peq/Peq832/peq8p32.c
    dsp/ptechDsp/Play/Play16/Play16.c
    dsp/ptechDsp/Play/Play32/Play32.c
    dsp/ptechDsp/wide/Wide16/Wide16.c
    dsp/ptechDsp/wide/Wide32/Wide32.c
    dsp/ptutil/COM/Com.cpp
    dsp/ptutil/COM/ComMem.cpp
    dsp/ptutil/COM/Comget.cpp
    dsp/ptutil/COM/Compass.cpp
    dsp/ptutil/COM/Comread.cpp
    dsp/ptutil/COM/Comwave.cpp
    dsp/ptutil/COM/Comwrite.cpp
    dsp/ptutil/DspUtil/BinauralSync/BinauralSynGet.cpp
    dsp/ptutil/DspUtil/BinauralSync/BinauralSynInit.cpp
    dsp/ptutil/DspUtil/BinauralSync/BinauralSynProcess.cpp
    dsp/ptutil/DspUtil/BinauralSync/BinauralSynSet.cpp
    dsp/ptutil/DspUtil/GraphicEq/GraphicEqGet.cpp
    dsp/ptutil/DspUtil/GraphicEq/GraphicEqInit.cpp
    dsp/ptutil/DspUtil/GraphicEq/GraphicEqInitBands.cpp
    dsp/ptutil/DspUtil/GraphicEq/GraphicEqInitSections.cpp
    dsp/ptutil/DspUtil/GraphicEq/GraphicEqProcess.cpp
    dsp/ptutil/DspUtil/GraphicEq/GraphicEqSet.cpp
    dsp/ptutil/DspUtil/SurroundSyn/SurroundSynInit.cpp
    dsp/ptutil/DspUtil/SurroundSyn/SurroundSynProcess.cpp
    dsp/ptutil/DspUtil/spectrum/spectrumGet.cpp
    dsp/ptutil/DspUtil/spectrum/spectrumInit.cpp
    dsp/ptutil/DspUtil/spectrum/spectrumMessageValues.cpp
    dsp/ptutil/DspUtil/spectrum/spectrumProcess.cpp
    dsp/ptutil/DspUtil/spectrum/spectrumReset.cpp
    dsp/ptutil/DspUtil/spectrum/spectrumSet.cpp
    dsp/ptutil/Filt/Fil12But.cpp
    dsp/ptutil/Filt/FiltCalcBiqd.cpp
    dsp/ptutil/Filt/FiltCalcFilterResponse.cpp
    dsp/ptutil/Filt/FiltRun.cpp
    dsp/ptutil/Filt/FiltbiqdSos.cpp
    dsp/ptutil/Filt/Filtpoly.cpp
    dsp/ptutil/PRELST/Prelst.cpp
    dsp/ptutil/PWAV/PwavConvert.cpp
    dsp/ptutil/Qnt/Qnt.cpp
    dsp/ptutil/Qnt/Qnt2But.cpp
    dsp/ptutil/Qnt/QntitoBoostCut.cpp
    dsp/ptutil/Qnt/Qntitol.cpp
    dsp/ptutil/Qnt/Qntitor.cpp
    dsp/ptutil/Qnt/Qntitor2.cpp
    dsp/ptutil/Qnt/Qntrtoi.cpp
    dsp/ptutil/Qnt/Qntrtol.cpp
    dsp/ptutil/Qnt/Qntrtor.cpp
    dsp/ptutil/SOS/Sos.cpp
    dsp/ptutil/SOS/SosGet.cpp
    dsp/ptutil/SOS/SosProcess.cpp
    dsp/ptutil/SOS/SosSet.cpp
    dsp/ptutil/VALS/Vals.cpp
    dsp/ptutil/VALS/Valscfg.cpp
    dsp/ptutil/VALS/Valsfile.cpp
    dsp/ptutil/VALS/Valsget.cpp
    dsp/ptutil/VALS/Valsset.cpp
    dsp/ptutil/VALS/Valswarp.cpp
    dsp/ptutil/dfxSharedUtil/dfxSharedUtil.cpp
    dsp/ptutil/dfxp/dfxpComm.cpp
    dsp/ptutil/dfxp/dfxpEq.cpp
    dsp/ptutil/dfxp/dfxpGet.cpp
    dsp/ptutil/dfxp/dfxpInit.cpp
    dsp/ptutil/dfxp/dfxpProcess.cpp
    dsp/ptutil/dfxp/dfxpProcessClear.cpp
    dsp/ptutil/dfxp/dfxpProcessInt.cpp
    dsp/ptutil/dfxp/dfxpProcessReal.cpp
    dsp/ptutil/dfxp/dfxpQnt.cpp
    dsp/ptutil/dfxp/dfxpQuit.cpp
    dsp/ptutil/dfxp/dfxpRegistryStandard.cpp
    dsp/ptutil/dfxp/dfxpSession.cpp
    dsp/ptutil/dfxp/dfxpSet.cpp
    dsp/ptutil/dfxp/dfxpSpectrum.cpp
    dsp/ptutil/dfxp/dfxpUniversal.cpp
    dsp/ptutil/realSample/realSampleForceLegalValues.cpp
)

set(OSCILLA_SUPPORT_SOURCES
    support/src/SLOUT/Slout.cpp
    support/src/MRY/Mry.cpp
    platform/dsp_platform.cpp
)
file(GLOB OSCILLA_MTH_SOURCES support/src/MTH/*.cpp)

add_library(oscilla_dsp STATIC
    ${OSCILLA_DSP_SOURCES} ${OSCILLA_SUPPORT_SOURCES} ${OSCILLA_MTH_SOURCES})
# Only DfxDsp.h is public. The WIN32 define, Windows shim and legacy headers stay private so
# consumers (JUCE app) are not compiled as a Windows target by accident.
target_include_directories(oscilla_dsp PUBLIC dsp/include)
target_include_directories(oscilla_dsp PRIVATE
    dsp/ptutil/include support/include dsp dsp/ptutil/dfxp
    support/src/SLOUT support/src/MTH support/src/MRY support/src/FILE support/src/reg)
target_compile_definitions(oscilla_dsp PRIVATE
    WIN32 PT_NON_MFC DSPSOFT_TARGET PT_DSP_BUILD=PT_DSP_DFX)
if(MSVC)
    # Native build, as FxSound's DfxDsp.vcxproj (Unicode, real windows.h). No shim.
    target_compile_definitions(oscilla_dsp PRIVATE UNICODE _UNICODE)
    target_compile_options(oscilla_dsp PRIVATE /w)
else()
    # macOS: shim/ stands in for the Windows SDK headers.
    target_include_directories(oscilla_dsp PRIVATE shim)
    target_compile_options(oscilla_dsp PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/shim/windows.h -w)
endif()

add_executable(dsp_offline tests/dsp_offline.cpp)
target_link_libraries(dsp_offline PRIVATE oscilla_dsp)
target_compile_definitions(dsp_offline PRIVATE OSCILLA_PRESET_DIR="${OSCILLA_PRESET_DIR}")

enable_testing()
add_test(NAME dsp_offline COMMAND dsp_offline)

# Oscilla's own presets: make_preset writes ../presets/*.fac, preset_tuning measures them.
add_executable(make_preset tools/make_preset.cpp)
target_link_libraries(make_preset PRIVATE oscilla_dsp)

add_executable(preset_tuning tests/preset_tuning.cpp)
target_link_libraries(preset_tuning PRIVATE oscilla_dsp)
# Starter profile tables are shared with the app (plain C++ header, no JUCE).
target_include_directories(preset_tuning PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../app/Source/core)
target_compile_definitions(preset_tuning PRIVATE
    OSCILLA_TUNED_PRESET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../presets")
add_test(NAME preset_tuning COMMAND preset_tuning)

# ../presets/*.fac must be exactly what tools/make_preset.cpp writes.
add_test(NAME presets_match_make_preset
    COMMAND ${CMAKE_COMMAND}
        -DMAKE_PRESET=$<TARGET_FILE:make_preset>
        -DEXPECTED_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../presets
        -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/presets_generated
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_presets.cmake)
