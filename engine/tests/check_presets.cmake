# ctest: the shipped presets (EXPECTED_DIR) must be byte-identical to what make_preset writes.
# cmake -DMAKE_PRESET=<exe> -DEXPECTED_DIR=<dir> -DOUTPUT_DIR=<scratch dir> -P check_presets.cmake
file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
execute_process(COMMAND "${MAKE_PRESET}" "${OUTPUT_DIR}" RESULT_VARIABLE result OUTPUT_QUIET)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "make_preset failed (${result})")
endif()

file(GLOB generated RELATIVE "${OUTPUT_DIR}" "${OUTPUT_DIR}/*")
file(GLOB expected RELATIVE "${EXPECTED_DIR}" "${EXPECTED_DIR}/*")
list(SORT generated)
list(SORT expected)
if(NOT generated STREQUAL expected)
    message(FATAL_ERROR "preset files differ: generated [${generated}] vs shipped [${expected}]")
endif()
foreach(name IN LISTS generated)
    execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${OUTPUT_DIR}/${name}" "${EXPECTED_DIR}/${name}"
                    RESULT_VARIABLE differs)
    if(differs)
        message(FATAL_ERROR "${name} differs from what make_preset writes")
    endif()
endforeach()
message(STATUS "presets match make_preset: ${generated}")
