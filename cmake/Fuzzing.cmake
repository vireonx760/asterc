# Fuzz-only objects keep coverage instrumentation out of shipped libraries.
option(ASTERASM_BUILD_FUZZERS "Build Clang libFuzzer + ASan/UBSan targets" OFF)
set(_aster_fuzz_frontend_sources
    tests/fuzz_frontend.c ${ASTERASM_CORE_SOURCES} ${ASTERASM_LEXER_SOURCES}
    ${ASTERASM_AST_SOURCES} ${ASTERASM_PARSER_SOURCES})
set(_aster_fuzz_decoder_sources
    tests/fuzz_decoder.c src/cli/asterdump_elf.c src/cli/asterdump_coff.c
    src/cli/asterdump_x86.c src/isa/aarch64_scalable.c)
foreach(_aster_fuzz_kind frontend decoder)
    if(BUILD_TESTING)
        add_executable(aster-fuzz-${_aster_fuzz_kind}-smoke
            tests/fuzz_smoke.c ${_aster_fuzz_${_aster_fuzz_kind}_sources})
        target_include_directories(aster-fuzz-${_aster_fuzz_kind}-smoke PRIVATE
            "${ASTERASM_PUBLIC_INCLUDE_DIR}" ${ASTERASM_PRIVATE_INCLUDE_DIRS})
        target_link_libraries(aster-fuzz-${_aster_fuzz_kind}-smoke PRIVATE Threads::Threads)
        asterasm_enable_warnings(aster-fuzz-${_aster_fuzz_kind}-smoke)
        asterasm_enable_sanitizers(aster-fuzz-${_aster_fuzz_kind}-smoke)
        add_test(NAME fuzz_${_aster_fuzz_kind}_smoke COMMAND aster-fuzz-${_aster_fuzz_kind}-smoke)
        set_tests_properties(fuzz_${_aster_fuzz_kind}_smoke PROPERTIES TIMEOUT 120 LABELS "fuzz;stress")
    endif()
    if(ASTERASM_BUILD_FUZZERS)
        if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR NOT UNIX OR ASTERASM_ENABLE_TSAN)
            message(FATAL_ERROR "Fuzz targets require Linux/macOS Clang without TSan")
        endif()
        add_executable(aster-fuzz-${_aster_fuzz_kind} ${_aster_fuzz_${_aster_fuzz_kind}_sources})
        target_include_directories(aster-fuzz-${_aster_fuzz_kind} PRIVATE
            "${ASTERASM_PUBLIC_INCLUDE_DIR}" ${ASTERASM_PRIVATE_INCLUDE_DIRS})
        target_link_libraries(aster-fuzz-${_aster_fuzz_kind} PRIVATE Threads::Threads)
        target_compile_options(aster-fuzz-${_aster_fuzz_kind} PRIVATE
            -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
        target_link_options(aster-fuzz-${_aster_fuzz_kind} PRIVATE -fsanitize=fuzzer,address,undefined)
        asterasm_enable_warnings(aster-fuzz-${_aster_fuzz_kind})
    endif()
endforeach()
if(BUILD_TESTING AND TARGET asterc AND Python3_Interpreter_FOUND AND ASTERASM_UNICORN_FOUND)
    add_test(NAME compiler_randomized_execution COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_randomized_execution.py"
        --compiler "$<TARGET_FILE:asterc>" --cases 8)
    set_tests_properties(compiler_randomized_execution PROPERTIES TIMEOUT 600 LABELS "fuzz;optimization;cpu")
endif()
if(BUILD_TESTING AND TARGET asterc AND TARGET asterdump AND Python3_Interpreter_FOUND)
    add_test(NAME release_metadata COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_metadata.py"
        --compiler "$<TARGET_FILE:asterc>" --dumper "$<TARGET_FILE:asterdump>")
endif()
