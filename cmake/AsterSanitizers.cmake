function(asterasm_copy_windows_asan_runtime target)
    if(NOT WIN32 OR NOT ASTERASM_ENABLE_ASAN)
        return()
    endif()

    get_target_property(_aster_type ${target} TYPE)
    if(_aster_type STREQUAL "STATIC_LIBRARY" OR
       _aster_type STREQUAL "OBJECT_LIBRARY" OR
       _aster_type STREQUAL "INTERFACE_LIBRARY")
        return()
    endif()

    get_filename_component(_aster_compiler_dir "${CMAKE_C_COMPILER}" DIRECTORY)
    set(_aster_runtime_search_dirs "${_aster_compiler_dir}")
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        execute_process(
            COMMAND "${CMAKE_C_COMPILER}" --print-resource-dir
            OUTPUT_VARIABLE _aster_clang_resource_dir
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(_aster_clang_resource_dir)
            list(APPEND _aster_runtime_search_dirs
                "${_aster_clang_resource_dir}"
                "${_aster_clang_resource_dir}/lib/windows"
            )
        endif()
    endif()

    set(_aster_asan_runtimes "")
    foreach(_aster_dir IN LISTS _aster_runtime_search_dirs)
        file(GLOB _aster_dir_runtimes
            "${_aster_dir}/clang_rt.asan_dynamic-*.dll"
            "${_aster_dir}/libclang_rt.asan_dynamic-*.dll"
            "${_aster_dir}/libasan-*.dll"
            "${_aster_dir}/libasan.dll"
        )
        list(APPEND _aster_asan_runtimes ${_aster_dir_runtimes})
    endforeach()
    list(REMOVE_DUPLICATES _aster_asan_runtimes)

    if(NOT _aster_asan_runtimes)
        message(FATAL_ERROR
            "AddressSanitizer is enabled, but its Windows runtime DLL could not "
            "be found beside the compiler or in the Clang resource directory"
        )
    endif()

    foreach(_aster_runtime IN LISTS _aster_asan_runtimes)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_aster_runtime}" "$<TARGET_FILE_DIR:${target}>"
            COMMENT "Copying AddressSanitizer runtime for ${target}"
            VERBATIM
        )
    endforeach()
endfunction()

function(asterasm_enable_sanitizers target)
    if(MSVC)
        if(ASTERASM_ENABLE_TSAN)
            message(FATAL_ERROR "ThreadSanitizer is not supported by the MSVC configuration")
        endif()
        if(ASTERASM_ENABLE_ASAN)
            # MSVC requires debug information for useful ASan reports (C5072
            # is an error under /WX). Instrumented objects select the ASan
            # runtime through linker directives; link.exe has no /fsanitize.
            target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
            asterasm_copy_windows_asan_runtime(${target})
        endif()
        return()
    endif()

    set(_aster_sanitizers "")
    if(ASTERASM_ENABLE_ASAN)
        list(APPEND _aster_sanitizers address)
    endif()
    if(ASTERASM_ENABLE_UBSAN)
        list(APPEND _aster_sanitizers undefined)
    endif()
    if(ASTERASM_ENABLE_TSAN)
        list(APPEND _aster_sanitizers thread)
    endif()
    if(NOT _aster_sanitizers)
        return()
    endif()

    list(JOIN _aster_sanitizers "," _aster_sanitizer_flags)
    target_compile_options(${target} PRIVATE
        -fsanitize=${_aster_sanitizer_flags}
        -fno-omit-frame-pointer
    )

    get_target_property(_aster_type ${target} TYPE)
    if(_aster_type STREQUAL "STATIC_LIBRARY")
        target_link_options(${target} INTERFACE -fsanitize=${_aster_sanitizer_flags})
    else()
        target_link_options(${target} PRIVATE -fsanitize=${_aster_sanitizer_flags})
    endif()
    asterasm_copy_windows_asan_runtime(${target})
endfunction()
