get_filename_component(output_directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
execute_process(
    COMMAND "${PERL}" -CSD "${GENERATOR}" "${INPUT}"
    OUTPUT_FILE "${OUTPUT}"
    ERROR_VARIABLE generator_error
    RESULT_VARIABLE generator_result)
if(NOT generator_result STREQUAL "0")
    file(REMOVE "${OUTPUT}")
    message(FATAL_ERROR "Could not generate libvterm encoding: ${generator_error}")
endif()
