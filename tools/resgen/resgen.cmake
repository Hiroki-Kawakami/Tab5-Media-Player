set(RESGEN_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/resgen.py")

function(resgen_add_resources)
    cmake_parse_arguments(R "" "TARGET;DEFINITION" "" ${ARGN})
    if(NOT R_TARGET OR NOT R_DEFINITION)
        message(FATAL_ERROR "resgen_add_resources: TARGET and DEFINITION are required")
    endif()
    get_filename_component(definition "${R_DEFINITION}" ABSOLUTE)

    set(python "$ENV{RESGEN_PYTHON}")
    if(NOT python)
        message(FATAL_ERROR "resgen: RESGEN_PYTHON is not set; run the build through `nix develop`")
    endif()

    set(out "${CMAKE_CURRENT_BINARY_DIR}/resgen")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${definition}" "${RESGEN_SCRIPT}")
    execute_process(
        COMMAND "${python}" "${RESGEN_SCRIPT}" cmake "${definition}" "${out}/entries.cmake"
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "resgen: invalid definition ${definition}")
    endif()
    include("${out}/entries.cmake")

    add_custom_command(
        OUTPUT "${out}/resources.h"
        COMMAND "${python}" "${RESGEN_SCRIPT}" header "${definition}" "${out}"
        DEPENDS "${definition}" "${RESGEN_SCRIPT}"
        COMMENT "resgen: resources.h"
        VERBATIM)
    set(sources "${out}/resources.h")
    foreach(entry ${RESGEN_ENTRIES})
        add_custom_command(
            OUTPUT "${out}/${entry}.c"
            COMMAND "${python}" "${RESGEN_SCRIPT}" entry "${definition}" "${entry}" "${out}"
            DEPENDS "${definition}" "${RESGEN_SCRIPT}" ${RESGEN_INPUTS_${entry}}
            COMMENT "resgen: ${entry}"
            VERBATIM)
        list(APPEND sources "${out}/${entry}.c")
    endforeach()

    # The simulator shim adds sources to a target defined in another directory,
    # where custom command outputs are neither GENERATED nor attached to a rule.
    add_custom_target(resgen_${R_TARGET} DEPENDS ${sources})
    add_dependencies(${R_TARGET} resgen_${R_TARGET})
    set_source_files_properties(${sources} TARGET_DIRECTORY ${R_TARGET} PROPERTIES GENERATED TRUE)
    target_sources(${R_TARGET} PRIVATE ${sources})
    target_include_directories(${R_TARGET} PUBLIC "${out}")
endfunction()
