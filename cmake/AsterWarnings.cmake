function(asterasm_enable_warnings target)
    if(MINGW)
        # Ask MinGW's stdio headers to expose the ISO C printf family.  Without
        # this, GCC validates formats against legacy MSVCRT and rejects C99
        # length modifiers such as %zu even though AsterASM requires C17.
        target_compile_definitions(${target} PRIVATE __USE_MINGW_ANSI_STDIO=1)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
        # Arena headers deliberately contain a maximally aligned payload member.
        target_compile_options(${target} PRIVATE /wd4324)
        # C11 atomics are optional in C17 and still opt-in in native MSVC.
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /experimental:c11atomics)
        endif()
        # Use the ISO C runtime interfaces, retaining portable source code.
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        if(ASTERASM_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
            -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Wundef
        )
        if(CMAKE_C_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE -Wno-unsafe-buffer-usage)
        endif()
        if(ASTERASM_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
