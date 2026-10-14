cmake_minimum_required(VERSION 3.22)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(profiler LANGUAGES C)
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT DEFINED PROFILER_TRANSPORT)
    set(PROFILER_TRANSPORT "null" CACHE STRING "Profiler transport backend")
endif()
set_property(CACHE PROFILER_TRANSPORT PROPERTY STRINGS
    null memory uart_stm32_hal_dma uart_stm32_hal_it)

set(PROFILER_PORT_DIR "${CMAKE_CURRENT_LIST_DIR}/port")
if(PROFILER_TRANSPORT STREQUAL "null")
    set(PROFILER_TRANSPORT_SOURCE "${PROFILER_PORT_DIR}/profiler_transport_null.c")
elseif(PROFILER_TRANSPORT STREQUAL "memory")
    set(PROFILER_TRANSPORT_SOURCE "${PROFILER_PORT_DIR}/profiler_transport_memory.c")
elseif(PROFILER_TRANSPORT STREQUAL "uart_stm32_hal_dma")
    set(PROFILER_TRANSPORT_SOURCE "${PROFILER_PORT_DIR}/profiler_transport_uart_dma.c")
elseif(PROFILER_TRANSPORT STREQUAL "uart_stm32_hal_it")
    set(PROFILER_TRANSPORT_SOURCE "${PROFILER_PORT_DIR}/profiler_transport_uart_it.c")
else()
    message(FATAL_ERROR "Unsupported PROFILER_TRANSPORT '${PROFILER_TRANSPORT}'")
endif()

if(NOT DEFINED PROFILER_PORT)
    set(PROFILER_PORT "custom" CACHE STRING "Profiler platform port backend")
endif()
set_property(CACHE PROFILER_PORT PROPERTY STRINGS
    custom dwt 16bit_it 2x16bit win posix riscv)

set(PROFILER_PORT_SOURCE "")
if(PROFILER_PORT STREQUAL "custom")
    # Consuming application provides its own profiler_port implementation
elseif(PROFILER_PORT STREQUAL "dwt")
    set(PROFILER_PORT_SOURCE "${PROFILER_PORT_DIR}/profiler_port_dwt.c")
elseif(PROFILER_PORT STREQUAL "16bit_it")
    set(PROFILER_PORT_SOURCE "${PROFILER_PORT_DIR}/profiler_port_16bit_it.c")
elseif(PROFILER_PORT STREQUAL "2x16bit")
    set(PROFILER_PORT_SOURCE "${PROFILER_PORT_DIR}/profiler_port_2x16bit.c")
elseif(PROFILER_PORT STREQUAL "win")
    set(PROFILER_PORT_SOURCE "${PROFILER_PORT_DIR}/profiler_port_win.c")
elseif(PROFILER_PORT STREQUAL "posix")
    set(PROFILER_PORT_SOURCE "${PROFILER_PORT_DIR}/profiler_port_posix.c")
elseif(PROFILER_PORT STREQUAL "riscv")
    set(PROFILER_PORT_SOURCE "${PROFILER_PORT_DIR}/profiler_port_riscv.c")
else()
    message(FATAL_ERROR "Unsupported PROFILER_PORT '${PROFILER_PORT}'")
endif()

function(profiler_resolve_dependency name output_variable)
    string(TOUPPER "${name}" uppercase_name)
    set(override_variable "${uppercase_name}_DIR")

    if(DEFINED ${override_variable} AND EXISTS "${${override_variable}}/${name}.h")
        set(${output_variable} "${${override_variable}}" PARENT_SCOPE)
    elseif(EXISTS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/modules/${name}/${name}.h")
        set(${output_variable}
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/modules/${name}" PARENT_SCOPE)
    elseif(EXISTS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../${name}/${name}.h")
        set(${output_variable}
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../${name}" PARENT_SCOPE)
    else()
        message(FATAL_ERROR
            "profiler: dependency '${name}' not found. Set ${override_variable} or place "
            "it in modules/${name} or beside the profiler repository.")
    endif()
endfunction()

profiler_resolve_dependency(hashmap PROFILER_HASHMAP_DIR)
profiler_resolve_dependency(stream PROFILER_STREAM_DIR)

set(PROFILER_LIB_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/profiler.c"
    "${PROFILER_PORT_DIR}/profiler_port_context_default.c"
    "${PROFILER_HASHMAP_DIR}/hashmap.c"
    "${PROFILER_STREAM_DIR}/stream.c"
    "${PROFILER_TRANSPORT_SOURCE}"
)
if(PROFILER_PORT_SOURCE)
    list(APPEND PROFILER_LIB_SOURCES "${PROFILER_PORT_SOURCE}")
endif()

add_library(profiler STATIC
    ${PROFILER_LIB_SOURCES}
)
target_compile_definitions(profiler PRIVATE PROFILER_HAS_TRANSPORT=1)

target_include_directories(profiler PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}"
    "${PROFILER_PORT_DIR}"
    "${PROFILER_HASHMAP_DIR}"
    "${PROFILER_STREAM_DIR}"
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(profiler PRIVATE -fno-instrument-functions)
endif()

message(STATUS "Profiler transport: ${PROFILER_TRANSPORT}")

# GCC exclusions match substrings, not globs. Keep the entire hook dependency path excluded.
function(profiler_instrument_target target)
    cmake_parse_arguments(INSTR "" "" "EXCLUDE_FILES;EXCLUDE_FUNCTIONS" ${ARGN})
    if(INSTR_UNPARSED_ARGUMENTS OR NOT TARGET ${target})
        message(FATAL_ERROR "profiler_instrument_target requires an existing target and valid exclusion lists")
    endif()
    if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR "profiler_instrument_target requires GCC; for Clang use per-source instrumentation and NO_INST")
    endif()
    set(files profiler.c profiler_port profiler_context profiler_transport
        hashmap.c stream.c Drivers CMSIS system_stm32 startup_ ${INSTR_EXCLUDE_FILES})
    set(functions main SystemInit SystemClock_Config Error_Handler assert_failed
        HAL_TIM_PeriodElapsedCallback ${INSTR_EXCLUDE_FUNCTIONS})
    list(JOIN files "," files)
    list(JOIN functions "," functions)
    target_compile_options(${target} PRIVATE
        "$<$<COMPILE_LANGUAGE:C>:-finstrument-functions>"
        "$<$<COMPILE_LANGUAGE:C>:-finstrument-functions-exclude-file-list=${files}>"
        "$<$<COMPILE_LANGUAGE:C>:-finstrument-functions-exclude-function-list=${functions}>")
endfunction()
