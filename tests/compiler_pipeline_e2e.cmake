if(NOT DEFINED ASTERC OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "ASTERC, SOURCE_DIR and BINARY_DIR are required")
endif()
set(out_dir "${BINARY_DIR}/compiler_e2e")
file(MAKE_DIRECTORY "${out_dir}")

function(compile_twice tag)
    set(extra ${ARGN})
    set(out1 "${out_dir}/${tag}.one.bin")
    set(out2 "${out_dir}/${tag}.two.bin")
    file(REMOVE "${out1}" "${out2}")
    execute_process(COMMAND "${ASTERC}" build ${extra} --emit bin -o "${out1}" "${SOURCE_DIR}/tests/compiler_e2e/main.aa"
                    RESULT_VARIABLE rc1 OUTPUT_VARIABLE stdout1 ERROR_VARIABLE stderr1)
    if(NOT rc1 EQUAL 0)
        message(FATAL_ERROR "${tag} first compile failed (${rc1}): ${stderr1}")
    endif()
    execute_process(COMMAND "${ASTERC}" build ${extra} --emit bin -o "${out2}" "${SOURCE_DIR}/tests/compiler_e2e/main.aa"
                    RESULT_VARIABLE rc2 OUTPUT_VARIABLE stdout2 ERROR_VARIABLE stderr2)
    if(NOT rc2 EQUAL 0)
        message(FATAL_ERROR "${tag} second compile failed (${rc2}): ${stderr2}")
    endif()
    file(SIZE "${out1}" size1)
    file(SIZE "${out2}" size2)
    if(size1 LESS 80 OR NOT size1 EQUAL size2)
        message(FATAL_ERROR "${tag} produced invalid sizes: ${size1}, ${size2}")
    endif()
    file(SHA256 "${out1}" hash1)
    file(SHA256 "${out2}" hash2)
    if(NOT hash1 STREQUAL hash2)
        message(FATAL_ERROR "${tag} compiler output is not deterministic")
    endif()
endfunction()

compile_twice(x86_64 --target x86_64)
compile_twice(aarch64 --target aarch64)
compile_twice(riscv64 --target riscv64 --feature m --feature f --feature d)

# r5 regression: direct calls/global addresses must retain the defining module
# through MIR -> Machine IR -> Physical IR -> encoder fixups.  The wrapper and
# leaf intentionally export same-named `init` and `value` symbols; r4 dropped
# the module qualifier and the raw linker rejected the relocation as ambiguous.
foreach(arch IN ITEMS x86_64 aarch64 riscv64 x86_32 x86_16)
    set(extra)
    if(arch STREQUAL "riscv64")
        set(extra --feature a)
    elseif(arch STREQUAL "x86_32" OR arch STREQUAL "x86_16")
        set(extra --abi cdecl --feature i586)
    endif()
    set(out "${out_dir}/qualified_relocation.${arch}.bin")
    execute_process(COMMAND "${ASTERC}" build --target ${arch} ${extra}
        -I "${SOURCE_DIR}/tests/compiler_e2e/qualified_relocation"
        --emit bin -o "${out}"
        "${SOURCE_DIR}/tests/compiler_e2e/qualified_relocation/main.aa"
        RESULT_VARIABLE qrc OUTPUT_VARIABLE qout ERROR_VARIABLE qerr)
    if(NOT qrc EQUAL 0)
        message(FATAL_ERROR "qualified relocation regression failed for ${arch} (${qrc}): ${qerr}")
    endif()
    file(SIZE "${out}" qsize)
    if(qsize LESS 32)
        message(FATAL_ERROR "qualified relocation regression for ${arch} produced implausibly small image: ${qsize}")
    endif()
endforeach()

# Post-r6 regression: sema exposes an unaliased import via its implicit last-segment
# alias (`import lib.hooks; hooks.touch()`). HIR must preserve the same resolution
# for both void and value-returning calls instead of emitting g.undef.
foreach(arch IN ITEMS x86_64 aarch64 riscv64 x86_32 x86_16)
    set(extra)
    if(arch STREQUAL "riscv64")
        set(extra --feature a)
    elseif(arch STREQUAL "x86_32" OR arch STREQUAL "x86_16")
        set(extra --abi cdecl --feature i586)
    endif()
    set(alias_out "${out_dir}/import_alias_call.${arch}.bin")
    execute_process(COMMAND "${ASTERC}" build --target ${arch} ${extra}
        -I "${SOURCE_DIR}/tests/compiler_e2e/import_alias_call"
        --emit bin -o "${alias_out}"
        "${SOURCE_DIR}/tests/compiler_e2e/import_alias_call/main.aa"
        RESULT_VARIABLE arc OUTPUT_VARIABLE aout ERROR_VARIABLE aerr)
    if(NOT arc EQUAL 0)
        message(FATAL_ERROR "implicit import-alias call regression failed for ${arch} (${arc}): ${aerr}")
    endif()
endforeach()

# r6 regressions: qualified exported constants are compile-time values; generic
# sizeof is folded after specialization; derived generic aggregate addresses
# use concrete field offsets and dynamic index scaling instead of a two-operand
# g.address_of.
foreach(arch IN ITEMS x86_64 aarch64 riscv64 x86_32 x86_16)
    set(extra)
    if(arch STREQUAL "riscv64")
        set(extra --feature a)
    elseif(arch STREQUAL "x86_32" OR arch STREQUAL "x86_16")
        set(extra --abi cdecl --feature i586)
    endif()
    foreach(case IN ITEMS generic_sizeof generic_address generic_local_pointer byte_mul)
        set(out "${out_dir}/r6_${case}.${arch}.bin")
        execute_process(COMMAND "${ASTERC}" build --target ${arch} ${extra} --emit bin -o "${out}"
            "${SOURCE_DIR}/tests/compiler_e2e/${case}.aa"
            RESULT_VARIABLE r6rc OUTPUT_VARIABLE r6out ERROR_VARIABLE r6err)
        if(NOT r6rc EQUAL 0)
            message(FATAL_ERROR "r6 ${case} regression failed for ${arch} (${r6rc}): ${r6err}")
        endif()
    endforeach()
    set(imported_out "${out_dir}/r6_imported_const.${arch}.bin")
    execute_process(COMMAND "${ASTERC}" build --target ${arch} ${extra}
        -I "${SOURCE_DIR}/tests/compiler_e2e/imported_const"
        --emit bin -o "${imported_out}"
        "${SOURCE_DIR}/tests/compiler_e2e/imported_const/main.aa"
        RESULT_VARIABLE irc OUTPUT_VARIABLE iout ERROR_VARIABLE ierr)
    if(NOT irc EQUAL 0)
        message(FATAL_ERROR "r6 imported-const regression failed for ${arch} (${irc}): ${ierr}")
    endif()
endforeach()

# CLI must never report success when semantic diagnostics contain errors.
execute_process(COMMAND "${ASTERC}" check --target x86_64
    "${SOURCE_DIR}/tests/compiler_e2e/semantic_failure.aa"
    RESULT_VARIABLE diag_rc OUTPUT_VARIABLE diag_out ERROR_VARIABLE diag_err)
if(diag_rc EQUAL 0)
    message(FATAL_ERROR "semantic diagnostic regression returned exit 0: ${diag_err}")
endif()
string(FIND "${diag_err}" "error[" diag_error_at)
if(diag_error_at EQUAL -1)
    message(FATAL_ERROR "semantic diagnostic regression did not emit an error diagnostic: ${diag_err}")
endif()

# Kernel-style regression: aggregate .rodata serialization, function/global
# addresses, pointer/integer conversion, shifts/masks and scalar memory access.
# This is the exact feature combination that previously failed with
# `cannot serialize initializer` and generic MIR legality diagnostics.
foreach(arch IN ITEMS x86_64 aarch64 riscv64 x86_32 x86_16)
    set(extra)
    if(arch STREQUAL "riscv64")
        set(extra --feature m --feature f --feature d)
    elseif(arch STREQUAL "x86_32" OR arch STREQUAL "x86_16")
        set(extra --abi cdecl)
    endif()
    set(out "${out_dir}/kernel_backend_regression.${arch}.bin")
    execute_process(COMMAND "${ASTERC}" build --target ${arch} ${extra} --emit bin -o "${out}"
        "${SOURCE_DIR}/tests/compiler_e2e/kernel_backend_regression.aa"
        RESULT_VARIABLE rc OUTPUT_VARIABLE rout ERROR_VARIABLE rerr)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "kernel backend regression failed for ${arch} (${rc}): ${rerr}")
    endif()
    file(SIZE "${out}" rsize)
    if(rsize LESS 128)
        message(FATAL_ERROR "kernel backend regression for ${arch} produced implausibly small image: ${rsize}")
    endif()
endforeach()

# Native islands may materialize a same-module global/function symbol address
# directly with LEA; this used to be rejected as an unsupported target binding.
set(native_symbols "${out_dir}/native_symbol_address.bin")
execute_process(COMMAND "${ASTERC}" build --target x86_64 --emit bin -o "${native_symbols}"
    "${SOURCE_DIR}/tests/compiler_e2e/native_symbol_address.aa"
    RESULT_VARIABLE nrc OUTPUT_VARIABLE nout ERROR_VARIABLE nerr)
if(NOT nrc EQUAL 0)
    message(FATAL_ERROR "native symbol-address compile failed (${nrc}): ${nerr}")
endif()

set(globals "${out_dir}/globals.bin")
file(REMOVE "${globals}")
execute_process(COMMAND "${ASTERC}" build --target x86_64 --base 0x100000 --emit bin -o "${globals}"
                "${SOURCE_DIR}/tests/hir_cases/globals/main.aa"
                RESULT_VARIABLE grc OUTPUT_VARIABLE gout ERROR_VARIABLE gerr)
if(NOT grc EQUAL 0)
    message(FATAL_ERROR "global-data compile failed (${grc}): ${gerr}")
endif()
file(SIZE "${globals}" gsize)
if(NOT gsize EQUAL 96)
    message(FATAL_ERROR "global-data image size is ${gsize}, expected 96")
endif()
file(READ "${globals}" ghex HEX)
string(TOLOWER "${ghex}" ghex)
set(expected_prefix "2a0000000100000002000000484952210000000000000000000010000000000000")
string(FIND "${ghex}" "${expected_prefix}" prefix_at)
if(NOT prefix_at EQUAL 0)
    message(FATAL_ERROR "global-data serialization/address fixup does not match the 1.0 contract")
endif()
