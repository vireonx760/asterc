cmake_minimum_required(VERSION 3.20)

project(AsterASM
    VERSION 1.2.1
    DESCRIPTION "AsterASM typed assembly compiler with verified x86-64, AArch64 and RV64 physical backends"
    LANGUAGES C
)

include(cmake/MsvcNinjaDependencies.cmake)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
option(BUILD_TESTING "Build unit and regression tests" OFF)
include(CTest)
include(cmake/AsterWarnings.cmake)
include(cmake/AsterSanitizers.cmake)

option(ASTERASM_BUILD_SHARED "Build aggregate shared frontend library" ON)
option(ASTERASM_BUILD_TOOLS "Build frontend command-line tools" ON)
option(ASTERASM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(ASTERASM_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ASTERASM_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ASTERASM_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(ASTERASM_ENABLE_IPO "Enable interprocedural optimization in Release builds" OFF)
# Explicit cross-target division qualification gate. With ON, QEMU must be
# present and actually execute every freestanding ELF case; compile-only is
# never counted as a successful Division runtime qualification.
option(ASTERASM_REQUIRE_DIVISION_QEMU "Require actual QEMU AArch64/RV64 division runtime" OFF)
option(ASTERASM_REQUIRE_BAREMETAL_QEMU "Require OS-free AArch64/RV64 system QEMU execution" OFF)
option(ASTERASM_REQUIRE_UNICORN "Require independent Unicorn CPU execution tests" OFF)
if((ASTERASM_REQUIRE_DIVISION_QEMU OR ASTERASM_REQUIRE_BAREMETAL_QEMU OR ASTERASM_REQUIRE_UNICORN) AND
   (NOT BUILD_TESTING OR NOT ASTERASM_BUILD_TOOLS))
    message(FATAL_ERROR "Required CPU qualification needs BUILD_TESTING and ASTERASM_BUILD_TOOLS")
endif()
include(cmake/ObjectWriter.cmake)

if(ASTERASM_ENABLE_TSAN AND ASTERASM_ENABLE_ASAN)
    message(FATAL_ERROR "ThreadSanitizer and AddressSanitizer cannot be enabled together")
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(Threads REQUIRED)

set(ASTERASM_PUBLIC_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(ASTERASM_PRIVATE_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lexer"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/parser"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/frontend"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/sema"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/hir"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mir"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/target"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/abi"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/common"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/regbank"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/isel"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/x86_64"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/regalloc"
)

set(ASTERASM_CORE_SOURCES
    src/core/arena.c
    src/core/hashmap.c
    src/core/hash.c
    src/core/parallel.c
    src/core/source.c
)
set(ASTERASM_LEXER_SOURCES
    src/lexer/lexer.c
    src/lexer/lexer_mt.c
)
set(ASTERASM_AST_SOURCES
    src/ast/ast.c
    src/ast/dump.c
)
set(ASTERASM_PARSER_SOURCES
    src/parser/parser.c
    src/parser/parser_decl.c
    src/parser/parser_expr.c
    src/parser/macro_expand.c
    src/parser/parser_parallel.c
    src/parser/parser_stmt.c
    src/parser/parser_type.c
    src/parser/parser_util.c
)
set(ASTERASM_FRONTEND_SOURCES
    src/frontend/frontend.c
    src/diag/diagnostics.c
    src/module/graph.c
)
set(ASTERASM_SEMA_SOURCES
    src/sema/sema.c
    src/sema/sema_interfaces.c
    src/sema/sema_type.c
    src/sema/sema_const.c
    src/sema/sema_expr.c
    src/sema/sema_stmt.c
    src/sema/sema_attributes.c
    src/sema/sema_diagnostics.c
    src/sema/sema_parallel.c
    src/sema/sema_facts.c
)
set(ASTERASM_HIR_SOURCES
    src/hir/hir.c
    src/hir/hir_init.c
    src/hir/hir_native.c
    src/hir/hir_lower.c
    src/hir/hir_verify.c
    src/hir/hir_dump.c
)
set(ASTERASM_MIR_SOURCES
    src/mir/mir.c
    src/mir/mir_lower.c
    src/mir/mir_cfg.c
    src/mir/mir_verify.c
    src/mir/mir_opt.c
    src/mir/mir_sccp.c
    src/mir/mir_opt_pipeline.c
    src/mir/mir_inline.c
    src/mir/mir_branch_opt.c
    src/mir/mir_structural_verify.c
    src/mir/mir_dump.c
)
set(ASTERASM_TARGET_SOURCES
    src/target/target.c
    src/target/target_registers.c
    src/target/scalable_native.c
    src/isa/aarch64_scalable.c
    src/target/target_dump.c
)
set(ASTERASM_ABI_SOURCES
    src/abi/abi.c
    src/abi/abi_i386.c
    src/abi/abi_x86.c
    src/abi/abi_a64.c
    src/abi/abi_riscv.c
    src/abi/abi_dump.c
)
set(ASTERASM_BACKEND_SOURCES
    src/backend/common/machine_ir.c
    src/backend/common/machine_peephole.c
    src/backend/common/machine_verify.c
    src/backend/common/machine_dump.c
    src/backend/common/backend.c
    src/backend/regbank/regbank.c
    src/backend/isel/isel.c
    src/backend/x86_64/x86_64_isel.c
    src/backend/aarch64/aarch64_isel.c
    src/backend/aarch64/aarch64_moves.c
    src/backend/aarch64/aarch64_physical.c
    src/backend/aarch64/aarch64_abi.c
    src/backend/aarch64/aarch64_frame.c
    src/backend/aarch64/aarch64_verify.c
    src/backend/riscv64/riscv64_isel.c
    src/backend/riscv64/riscv64_physical.c
    src/backend/riscv64/riscv64_abi.c
    src/backend/riscv64/riscv64_lower.c
    src/backend/riscv64/riscv64_verify.c
    src/backend/regalloc/physreg.c
    src/backend/regalloc/machine_liveness.c
    src/backend/regalloc/regalloc.c
    src/backend/frame/frame.c
    src/backend/postra/physical_common.c
    src/backend/postra/physical_peephole.c
    src/backend/postra/atomic_lower.c
    src/backend/frame/abi_boundary.c
    src/backend/frame/prologue_epilogue.c
    src/backend/postra/physical_rewrite.c
    src/backend/postra/native_lower.c
    src/backend/postra/native_bridge.c
    src/backend/postra/native_transfer.c
    src/backend/postra/native_system.c
    src/backend/postra/native_vector.c
    src/backend/postra/native_vector_other.c
    src/backend/postra/native_scalable.c
    src/backend/postra/native_verify.c
    src/backend/postra/postra_verify.c
    src/backend/regalloc/regalloc_dump.c
    src/backend/encoder/x86_64_encoder.c
    src/backend/encoder/x86_legacy_encoder.c
    src/backend/encoder/encoder.c
    src/backend/encoder/encoder_fixups.c
    src/backend/encoder/aarch64_encoder.c
    src/backend/encoder/riscv64_encoder.c
)
set(ASTERASM_ALL_LIBRARY_SOURCES
    ${ASTERASM_CORE_SOURCES}
    ${ASTERASM_LEXER_SOURCES}
    ${ASTERASM_AST_SOURCES}
    ${ASTERASM_PARSER_SOURCES}
    ${ASTERASM_FRONTEND_SOURCES}
    ${ASTERASM_SEMA_SOURCES}
    ${ASTERASM_HIR_SOURCES}
    ${ASTERASM_MIR_SOURCES}
    ${ASTERASM_TARGET_SOURCES}
    ${ASTERASM_ABI_SOURCES}
    ${ASTERASM_BACKEND_SOURCES}
)

function(asterasm_configure_library target)
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${ASTERASM_PUBLIC_INCLUDE_DIR}>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE ${ASTERASM_PRIVATE_INCLUDE_DIRS}
    )
    target_link_libraries(${target} PUBLIC Threads::Threads)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    asterasm_enable_warnings(${target})
    asterasm_enable_sanitizers(${target})
endfunction()

add_library(asterasm_core STATIC ${ASTERASM_CORE_SOURCES})
add_library(AsterASM::core ALIAS asterasm_core)
set_target_properties(asterasm_core PROPERTIES EXPORT_NAME core)
asterasm_configure_library(asterasm_core)

add_library(asterasm_lexer STATIC ${ASTERASM_LEXER_SOURCES})
add_library(AsterASM::lexer ALIAS asterasm_lexer)
set_target_properties(asterasm_lexer PROPERTIES EXPORT_NAME lexer)
asterasm_configure_library(asterasm_lexer)
target_link_libraries(asterasm_lexer PUBLIC asterasm_core)

add_library(asterasm_ast STATIC ${ASTERASM_AST_SOURCES})
add_library(AsterASM::ast ALIAS asterasm_ast)
set_target_properties(asterasm_ast PROPERTIES EXPORT_NAME ast)
asterasm_configure_library(asterasm_ast)
target_link_libraries(asterasm_ast PUBLIC asterasm_core asterasm_lexer)

add_library(asterasm_parser STATIC ${ASTERASM_PARSER_SOURCES})
add_library(AsterASM::parser ALIAS asterasm_parser)
set_target_properties(asterasm_parser PROPERTIES EXPORT_NAME parser)
asterasm_configure_library(asterasm_parser)
target_link_libraries(asterasm_parser PUBLIC asterasm_ast asterasm_lexer asterasm_core)

add_library(asterasm_frontend STATIC ${ASTERASM_FRONTEND_SOURCES})
add_library(AsterASM::frontend ALIAS asterasm_frontend)
set_target_properties(asterasm_frontend PROPERTIES EXPORT_NAME frontend)
asterasm_configure_library(asterasm_frontend)
target_link_libraries(asterasm_frontend PUBLIC asterasm_parser asterasm_ast asterasm_lexer asterasm_core)

add_library(asterasm_sema STATIC ${ASTERASM_SEMA_SOURCES})
add_library(AsterASM::sema ALIAS asterasm_sema)
set_target_properties(asterasm_sema PROPERTIES EXPORT_NAME sema)
asterasm_configure_library(asterasm_sema)
target_link_libraries(asterasm_sema PUBLIC asterasm_frontend asterasm_parser asterasm_ast asterasm_lexer asterasm_core)
if(UNIX)
    target_link_libraries(asterasm_sema PUBLIC m)
endif()

add_library(asterasm_hir STATIC ${ASTERASM_HIR_SOURCES})
add_library(AsterASM::hir ALIAS asterasm_hir)
set_target_properties(asterasm_hir PROPERTIES EXPORT_NAME hir)
asterasm_configure_library(asterasm_hir)
target_link_libraries(asterasm_hir PUBLIC asterasm_sema asterasm_frontend asterasm_parser asterasm_ast asterasm_lexer asterasm_core)
if(UNIX)
    target_link_libraries(asterasm_hir PUBLIC m)
endif()

add_library(asterasm_mir STATIC ${ASTERASM_MIR_SOURCES})
add_library(AsterASM::mir ALIAS asterasm_mir)
set_target_properties(asterasm_mir PROPERTIES EXPORT_NAME mir)
asterasm_configure_library(asterasm_mir)
target_link_libraries(asterasm_mir PUBLIC asterasm_hir asterasm_sema asterasm_frontend asterasm_parser asterasm_ast asterasm_lexer asterasm_core)
if(UNIX)
    target_link_libraries(asterasm_mir PUBLIC m)
endif()

add_library(asterasm_target STATIC ${ASTERASM_TARGET_SOURCES})
add_library(AsterASM::target ALIAS asterasm_target)
set_target_properties(asterasm_target PROPERTIES EXPORT_NAME target)
asterasm_configure_library(asterasm_target)
target_link_libraries(asterasm_target PUBLIC asterasm_mir)

add_library(asterasm_abi STATIC ${ASTERASM_ABI_SOURCES})
add_library(AsterASM::abi ALIAS asterasm_abi)
set_target_properties(asterasm_abi PROPERTIES EXPORT_NAME abi)
asterasm_configure_library(asterasm_abi)
target_link_libraries(asterasm_abi PUBLIC asterasm_target)

add_library(asterasm_backend STATIC ${ASTERASM_BACKEND_SOURCES})
add_library(AsterASM::backend ALIAS asterasm_backend)
set_target_properties(asterasm_backend PROPERTIES EXPORT_NAME backend)
asterasm_configure_library(asterasm_backend)
target_link_libraries(asterasm_backend PUBLIC asterasm_abi)

if(ASTERASM_BUILD_SHARED)
    add_library(asterasm_shared SHARED ${ASTERASM_ALL_LIBRARY_SOURCES})
    add_library(AsterASM::shared ALIAS asterasm_shared)
    set_target_properties(asterasm_shared PROPERTIES OUTPUT_NAME asterasm EXPORT_NAME shared)
    asterasm_configure_library(asterasm_shared)
    target_compile_definitions(asterasm_shared PUBLIC ASTERASM_SHARED PRIVATE ASTERASM_BUILDING_SHARED)
    if(UNIX)
        target_link_libraries(asterasm_shared PRIVATE m)
    endif()
endif()

if(ASTERASM_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ASTERASM_IPO_SUPPORTED OUTPUT ASTERASM_IPO_ERROR)
    if(ASTERASM_IPO_SUPPORTED)
        foreach(target asterasm_core asterasm_lexer asterasm_ast asterasm_parser asterasm_frontend asterasm_sema asterasm_hir asterasm_mir asterasm_target asterasm_abi asterasm_backend)
            set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        endforeach()
        if(TARGET asterasm_shared)
            set_property(TARGET asterasm_shared PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        endif()
    else()
        message(WARNING "IPO requested but unavailable: ${ASTERASM_IPO_ERROR}")
    endif()
endif()

if(ASTERASM_BUILD_TOOLS)
    add_executable(asterdump src/cli/asterdump.c src/cli/asterdump_elf.c src/cli/asterdump_coff.c src/cli/asterdump_x86.c src/isa/aarch64_scalable.c)
    target_include_directories(asterdump PRIVATE "${ASTERASM_PUBLIC_INCLUDE_DIR}")
    asterasm_enable_warnings(asterdump)
    asterasm_enable_sanitizers(asterdump)
    add_executable(asterc src/cli/asterc.c src/cli/compiler_pipeline.c)
    if(WIN32 AND CMAKE_C_SIMULATE_ID STREQUAL "MSVC" AND NOT MSVC)
        target_link_options(asterc PRIVATE "LINKER:/manifest:embed" "LINKER:/manifestinput:${CMAKE_CURRENT_SOURCE_DIR}/src/cli/asterc.manifest")
    elseif(WIN32 AND MSVC)
        # Keep CRT argv and Win32 A filesystem APIs in the same UTF-8 encoding
        # as source imports. Windows 10 1903+ honors activeCodePage per process.
        target_sources(asterc PRIVATE src/cli/asterc.manifest)
    endif()
    target_link_libraries(asterc PRIVATE asterasm_backend)
    if(NOT ASTERASM_BUILD_OBJECT_WRITER)
        target_sources(asterc PRIVATE src/cli/elf64_fallback.c)
    endif()
    if(NOT ASTERASM_BUILD_OBJECT_WRITER AND BUILD_TESTING)
        add_executable(aster-elf64-fallback-tests tests/elf64_fallback_unit.c src/cli/elf64_fallback.c)
        target_include_directories(aster-elf64-fallback-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/cli" "${CMAKE_CURRENT_SOURCE_DIR}/include")
        asterasm_enable_warnings(aster-elf64-fallback-tests)
        asterasm_enable_sanitizers(aster-elf64-fallback-tests)
        add_test(NAME elf64_fallback_bounds_and_relocations COMMAND aster-elf64-fallback-tests)
    endif()
    if(ASTERASM_BUILD_OBJECT_WRITER)
        target_link_libraries(asterc PRIVATE asterasm_object)
        if(UNIX AND NOT APPLE)
            # Rust writer is a separate shared component in the installed tree.
            set_target_properties(asterc PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
        endif()
        target_compile_definitions(asterc PRIVATE ASTERASM_HAS_OBJECT_WRITER=1)
        if(WIN32)
            add_custom_command(TARGET asterc POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different $<TARGET_FILE:asterasm_object> $<TARGET_FILE_DIR:asterc>)
        endif()
    endif()
    asterasm_enable_warnings(asterc)
    asterasm_enable_sanitizers(asterc)

    add_executable(aster-lex src/cli/aster_lex.c)
    target_link_libraries(aster-lex PRIVATE asterasm_lexer)
    asterasm_enable_warnings(aster-lex)
    asterasm_enable_sanitizers(aster-lex)

    add_executable(aster-ast src/cli/aster_ast.c)
    target_link_libraries(aster-ast PRIVATE asterasm_frontend)
    asterasm_enable_warnings(aster-ast)
    asterasm_enable_sanitizers(aster-ast)

    add_executable(aster-hir src/cli/aster_hir.c)
    target_link_libraries(aster-hir PRIVATE asterasm_hir)
    asterasm_enable_warnings(aster-hir)
    asterasm_enable_sanitizers(aster-hir)

    add_executable(aster-mir src/cli/aster_mir.c)
    target_link_libraries(aster-mir PRIVATE asterasm_mir)
    asterasm_enable_warnings(aster-mir)
    asterasm_enable_sanitizers(aster-mir)
endif()

if(BUILD_TESTING)
    if(ASTERASM_BUILD_TOOLS)
        add_test(NAME compiler_pipeline_e2e COMMAND "${CMAKE_COMMAND}"
            -DASTERC=$<TARGET_FILE:asterc>
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_pipeline_e2e.cmake")
        add_test(NAME compiler_extern_contract COMMAND "${CMAKE_COMMAND}"
            -DASTERC=$<TARGET_FILE:asterc>
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_extern_e2e.cmake")
    endif()
    add_executable(aster-lexer-tests tests/test_lexer.c)
    target_link_libraries(aster-lexer-tests PRIVATE asterasm_lexer)
    asterasm_enable_warnings(aster-lexer-tests)
    asterasm_enable_sanitizers(aster-lexer-tests)
    add_test(NAME lexer COMMAND aster-lexer-tests)

    add_executable(aster-parser-tests tests/test_parser.c)
    target_link_libraries(aster-parser-tests PRIVATE asterasm_frontend)
    asterasm_enable_warnings(aster-parser-tests)
    asterasm_enable_sanitizers(aster-parser-tests)
    add_test(NAME parser COMMAND aster-parser-tests)

    add_executable(aster-frontend-tests tests/test_frontend.c)
    target_link_libraries(aster-frontend-tests PRIVATE asterasm_frontend)
    target_compile_definitions(aster-frontend-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-frontend-tests)
    asterasm_enable_sanitizers(aster-frontend-tests)
    add_test(NAME frontend COMMAND aster-frontend-tests)

    add_executable(aster-module-tests tests/test_module_graph.c)
    target_link_libraries(aster-module-tests PRIVATE asterasm_frontend)
    asterasm_enable_warnings(aster-module-tests)
    asterasm_enable_sanitizers(aster-module-tests)
    add_test(NAME module_graph COMMAND aster-module-tests)
    add_executable(aster-parser-context-tests tests/test_parser_context.c)
    target_link_libraries(aster-parser-context-tests PRIVATE asterasm_frontend)
    asterasm_enable_warnings(aster-parser-context-tests)
    asterasm_enable_sanitizers(aster-parser-context-tests)
    add_test(NAME parser_context_recovery COMMAND aster-parser-context-tests)

    add_executable(aster-parser-case-tests tests/test_parser_cases.c)
    target_link_libraries(aster-parser-case-tests PRIVATE asterasm_frontend)
    asterasm_enable_warnings(aster-parser-case-tests)
    asterasm_enable_sanitizers(aster-parser-case-tests)
    set(ASTERASM_PARSER_CASE_NAMES
        nested_generics shift_expression atomic_load atomic_store atomic_rmw atomic_cas native_bridge
        unsafe_exact_bytes macro_tokens unicode_names struct_attributes dispatch comptime_constructs
        nested_initializer target_features qualified_keyword_import missing_module_semicolon
        missing_generic_close malformed_parameter expression_recovery native_positional_operand
        malformed_import_alias missing_block_close invalid_generic_param atomic_missing_dot bad_array_type
        top_level_recovery recursion_limit contextual_params_locals contextual_assign
        contextual_types contextual_generic_types contextual_intrinsics
        section_keyword_name_recovery section_integer_recovery section_statement_recovery
        section_export_recovery struct_statement_recovery target_expression_recovery
    )
    set(_aster_parser_case_index 0)
    foreach(_aster_parser_case IN LISTS ASTERASM_PARSER_CASE_NAMES)
        add_test(NAME parser_${_aster_parser_case} COMMAND aster-parser-case-tests ${_aster_parser_case_index})
        set_tests_properties(parser_${_aster_parser_case} PROPERTIES TIMEOUT 15)
        math(EXPR _aster_parser_case_index "${_aster_parser_case_index} + 1")
    endforeach()

    add_executable(aster-sema-parallel-tests tests/test_sema_parallel.c)
    target_link_libraries(aster-sema-parallel-tests PRIVATE asterasm_sema)
    asterasm_enable_warnings(aster-sema-parallel-tests)
    asterasm_enable_sanitizers(aster-sema-parallel-tests)
    add_test(NAME sema_parallel_determinism COMMAND aster-sema-parallel-tests)

    add_executable(aster-sema-facts-tests tests/test_sema_facts.c)
    target_link_libraries(aster-sema-facts-tests PRIVATE asterasm_sema)
    target_compile_definitions(aster-sema-facts-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-sema-facts-tests)
    asterasm_enable_sanitizers(aster-sema-facts-tests)
    add_test(NAME sema_facts COMMAND aster-sema-facts-tests)

    add_executable(aster-sema-case-tests tests/test_sema_cases.c)
    target_link_libraries(aster-sema-case-tests PRIVATE asterasm_sema)
    target_compile_definitions(aster-sema-case-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-sema-case-tests)
    asterasm_enable_sanitizers(aster-sema-case-tests)
    set(ASTERASM_SEMA_CASE_NAMES
        core_success modules_success generics_success flow_success atomic_success native_success
        duplicate_symbol unknown_name unknown_type not_exported literal_overflow const_cycle
        static_assert_fail call_arity return_type missing_return uninitialized assign_const invalid_store atomic_order_bad
        aggregate_success aggregate_type_bad branch_forward_success branch_into_scope_bad
        loop_break_init_success loop_break_uninit_bad unreachable_init_bad comptime_dead_branch_success
        requires_false_success attribute_leaf_bad attribute_conflict_bad align_layout_success
        load_pointee_bad string_mutable_bad naked_success
        generic_fn_expected_success generic_fn_arg_success generic_fn_const_success
        generic_fn_conflict_bad generic_fn_uninferred_bad generic_fn_import_success
        pointer_qual_success pointer_qual_bad pointer_nested_bad float_overflow_bad float_format_bad
        cast_success cast_trunc_same_bad ptrcast_const_bad ptrcast_unsafe_success ptrcast_integer_bad
        bitcast_pointer_bad naked_semantic_bad
        generic_nominal_const_success generic_const_expr_success generic_fn_const_expr_success generic_fn_const_expr_bad
        target_success target_duplicate_property_bad target_duplicate_feature_bad target_nonconst_bad
        dispatch_nonconst_bad dispatch_success
        comptime_for_success comptime_for_late_assert_bad comptime_for_zero_dead_success
        comptime_for_limit_bad comptime_for_nonconst_bad
        generic_duplicate_param_bad generic_param_value_collision_bad
        comptime_dead_label_success requires_dead_label_success comptime_else_if_success
        sema_recursion_limit_bad fence_success fence_missing_order_bad fence_duplicate_order_bad fence_invalid_attribute_bad
        integer_wrap_success signed_wrap_success context_literal_overflow_bad
        signed_min_literal_success signed_min_literal_overflow_bad
        intrinsic_select_success intrinsic_select_bad intrinsic_bitops_success intrinsic_bitops_bad
        intrinsic_rotate_success intrinsic_rotate_bad intrinsic_ptr_int_unsafe_success
        intrinsic_ptr_to_int_safe_bad intrinsic_int_to_ptr_safe_bad intrinsic_checked_success
        intrinsic_wrap_sat_success intrinsic_checked_type_bad intrinsic_mul_sat_bad
        intrinsic_const_select_success intrinsic_const_bitops_success intrinsic_const_rotate_success
        intrinsic_const_wrap_sat_success intrinsic_u128_wrap_sat_success intrinsic_bswap_width_bad
        checked_tuple_index_success checked_tuple_index_nonconst_bad checked_tuple_index_oob_bad
        shift_modulo_success signed_shift_success signed_bitwise_success bits_shift_success
        division_by_zero_bad cast_const_success cast_const_range_bad facts_success
        vector_arithmetic_success vector_compare_mask_success vector_select_success
        vector_float_remainder_bad vector_element_bad mask_index_success svector_pred_success
        void_param_bad void_local_bad void_field_bad ptr_void_success
        labeled_continue_success labeled_continue_unknown_bad labeled_break_success
        division_overflow_bad remainder_min_success float_div_zero_success
        static_assert_message_success static_assert_message_bad pred_select_mismatch_bad
        void_deref_bad void_index_bad sizeof_void_bad
    )
    set(_aster_sema_case_index 0)
    foreach(_aster_sema_case IN LISTS ASTERASM_SEMA_CASE_NAMES)
        add_test(NAME sema_${_aster_sema_case} COMMAND aster-sema-case-tests ${_aster_sema_case_index})
        math(EXPR _aster_sema_case_index "${_aster_sema_case_index} + 1")
    endforeach()
    add_test(NAME sema_imported_constants_parallel_repeat
        COMMAND aster-sema-case-tests 1 --repeat 100)
    set_tests_properties(sema_imported_constants_parallel_repeat PROPERTIES
        TIMEOUT 90 LABELS "sema;parallel;regression")
    add_executable(aster-hir-case-tests tests/test_hir_cases.c)
    target_link_libraries(aster-hir-case-tests PRIVATE asterasm_hir)
    target_compile_definitions(aster-hir-case-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-hir-case-tests)
    asterasm_enable_sanitizers(aster-hir-case-tests)
    set(ASTERASM_HIR_CASE_NAMES
        core_success modules_success generics_success flow_success atomic_success native_success aggregate_success
        branch_forward_success loop_break_init_success comptime_dead_branch_success requires_false_success
        align_layout_success naked_success generic_fn_expected_success generic_fn_arg_success generic_fn_const_success
        generic_fn_import_success pointer_qual_success cast_success ptrcast_unsafe_success generic_nominal_const_success
        generic_const_expr_success generic_fn_const_expr_success target_success dispatch_success comptime_for_success
        comptime_for_zero_dead_success comptime_dead_label_success requires_dead_label_success comptime_else_if_success
        fence_success integer_wrap_success signed_wrap_success signed_min_literal_success intrinsic_select_success
        intrinsic_bitops_success intrinsic_rotate_success intrinsic_ptr_int_unsafe_success intrinsic_checked_success
        intrinsic_wrap_sat_success intrinsic_const_select_success intrinsic_const_bitops_success intrinsic_const_rotate_success
        intrinsic_const_wrap_sat_success intrinsic_u128_wrap_sat_success checked_tuple_index_success shift_modulo_success
        signed_shift_success signed_bitwise_success bits_shift_success cast_const_success facts_success
        vector_arithmetic_success vector_compare_mask_success vector_select_success mask_index_success svector_pred_success
        ptr_void_success labeled_continue_success labeled_break_success remainder_min_success float_div_zero_success
        static_assert_message_success
    )
    foreach(_aster_hir_case IN LISTS ASTERASM_HIR_CASE_NAMES)
        add_test(NAME hir_case_${_aster_hir_case} COMMAND aster-hir-case-tests ${_aster_hir_case})
    endforeach()

    add_executable(aster-hir-tests tests/test_hir.c)
    target_link_libraries(aster-hir-tests PRIVATE asterasm_hir)
    target_compile_definitions(aster-hir-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-hir-tests)
    asterasm_enable_sanitizers(aster-hir-tests)
    add_test(NAME hir_core COMMAND aster-hir-tests)

    add_executable(aster-hir-parallel-tests tests/test_hir_parallel.c)
    target_link_libraries(aster-hir-parallel-tests PRIVATE asterasm_hir)
    target_compile_definitions(aster-hir-parallel-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-hir-parallel-tests)
    asterasm_enable_sanitizers(aster-hir-parallel-tests)
    add_test(NAME hir_parallel_determinism COMMAND aster-hir-parallel-tests)

    add_executable(aster-hir-verifier-tests tests/test_hir_verifier.c)
    target_link_libraries(aster-hir-verifier-tests PRIVATE asterasm_hir)
    target_include_directories(aster-hir-verifier-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/hir")
    target_compile_definitions(aster-hir-verifier-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-hir-verifier-tests)
    asterasm_enable_sanitizers(aster-hir-verifier-tests)
    add_test(NAME hir_verifier COMMAND aster-hir-verifier-tests)

    add_executable(aster-mir-case-tests tests/test_mir_cases.c)
    target_link_libraries(aster-mir-case-tests PRIVATE asterasm_mir)
    target_compile_definitions(aster-mir-case-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-mir-case-tests)
    asterasm_enable_sanitizers(aster-mir-case-tests)
    foreach(_aster_mir_case IN LISTS ASTERASM_HIR_CASE_NAMES)
        add_test(NAME mir_case_${_aster_mir_case} COMMAND aster-mir-case-tests ${_aster_mir_case})
    endforeach()

    add_executable(aster-mir-tests tests/test_mir.c)
    target_link_libraries(aster-mir-tests PRIVATE asterasm_mir)
    target_compile_definitions(aster-mir-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-mir-tests)
    asterasm_enable_sanitizers(aster-mir-tests)
    add_test(NAME mir_core COMMAND aster-mir-tests)

    add_executable(aster-mir-parallel-tests tests/test_mir_parallel.c)
    target_link_libraries(aster-mir-parallel-tests PRIVATE asterasm_mir)
    target_compile_definitions(aster-mir-parallel-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-mir-parallel-tests)
    asterasm_enable_sanitizers(aster-mir-parallel-tests)
    add_test(NAME mir_parallel_determinism COMMAND aster-mir-parallel-tests)

    add_executable(aster-mir-verifier-tests tests/test_mir_verifier.c)
    target_link_libraries(aster-mir-verifier-tests PRIVATE asterasm_mir)
    target_include_directories(aster-mir-verifier-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/mir")
    target_compile_definitions(aster-mir-verifier-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-mir-verifier-tests)
    asterasm_enable_sanitizers(aster-mir-verifier-tests)
    add_test(NAME mir_verifier COMMAND aster-mir-verifier-tests)

    add_executable(aster-mir-structural-tests tests/test_mir_structural.c)
    target_link_libraries(aster-mir-structural-tests PRIVATE asterasm_mir)
    asterasm_enable_warnings(aster-mir-structural-tests)
    asterasm_enable_sanitizers(aster-mir-structural-tests)
    target_include_directories(aster-mir-structural-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/mir ${CMAKE_CURRENT_SOURCE_DIR}/src/hir)
    target_compile_definitions(aster-mir-structural-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME mir_structural_ssa_cfg_memory COMMAND aster-mir-structural-tests)

    add_executable(aster-mir-optimizer-tests tests/test_mir_optimizer.c)
    target_link_libraries(aster-mir-optimizer-tests PRIVATE asterasm_mir)
    target_include_directories(aster-mir-optimizer-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/mir")
    target_compile_definitions(aster-mir-optimizer-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-mir-optimizer-tests)
    asterasm_enable_sanitizers(aster-mir-optimizer-tests)
    add_test(NAME mir_optimizer_transactional_rewrite COMMAND aster-mir-optimizer-tests)

    add_executable(aster-mir-inline-tests tests/test_mir_inline.c)
    target_link_libraries(aster-mir-inline-tests PRIVATE asterasm_mir)
    target_include_directories(aster-mir-inline-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/mir")
    target_compile_definitions(aster-mir-inline-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-mir-inline-tests)
    asterasm_enable_sanitizers(aster-mir-inline-tests)
    add_test(NAME mir_inline_transactional_cfg_and_recursion COMMAND aster-mir-inline-tests)

    add_executable(aster-mir-sccp-tests tests/test_mir_sccp.c)
    target_link_libraries(aster-mir-sccp-tests PRIVATE asterasm_mir)
    target_include_directories(aster-mir-sccp-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/mir")
    target_compile_definitions(aster-mir-sccp-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-mir-sccp-tests)
    asterasm_enable_sanitizers(aster-mir-sccp-tests)
    add_test(NAME mir_sccp_executable_edges_and_phi COMMAND aster-mir-sccp-tests)

    add_executable(aster-mir-sccp-transform-tests tests/test_mir_sccp_transform.c)
    target_link_libraries(aster-mir-sccp-transform-tests PRIVATE asterasm_backend)
    target_include_directories(aster-mir-sccp-transform-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/mir")
    target_compile_definitions(aster-mir-sccp-transform-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-mir-sccp-transform-tests)
    asterasm_enable_sanitizers(aster-mir-sccp-transform-tests)
    add_test(NAME mir_sccp_replay_certified_crossisa COMMAND aster-mir-sccp-transform-tests)

    add_executable(aster-mir-const-branch-tests tests/test_mir_const_branch.c)
    target_link_libraries(aster-mir-const-branch-tests PRIVATE asterasm_backend)
    target_include_directories(aster-mir-const-branch-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/mir")
    target_compile_definitions(aster-mir-const-branch-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-mir-const-branch-tests)
    asterasm_enable_sanitizers(aster-mir-const-branch-tests)
    add_test(NAME mir_certified_const_branch_cfg_crossisa COMMAND aster-mir-const-branch-tests)

    add_executable(aster-mir-gvn-tests tests/test_mir_gvn.c)
    target_link_libraries(aster-mir-gvn-tests PRIVATE asterasm_backend)
    target_include_directories(aster-mir-gvn-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/mir")
    target_compile_definitions(aster-mir-gvn-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-mir-gvn-tests)
    asterasm_enable_sanitizers(aster-mir-gvn-tests)
    add_test(NAME mir_gvn_certified COMMAND aster-mir-gvn-tests)

    add_executable(aster-mir-fold-unsigned-tests tests/test_mir_fold_unsigned.c)
    target_link_libraries(aster-mir-fold-unsigned-tests PRIVATE asterasm_backend)
    target_include_directories(aster-mir-fold-unsigned-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/mir")
    target_compile_definitions(aster-mir-fold-unsigned-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-mir-fold-unsigned-tests)
    asterasm_enable_sanitizers(aster-mir-fold-unsigned-tests)
    add_test(NAME mir_fold_unsigned_native_x86 COMMAND aster-mir-fold-unsigned-tests)

    add_executable(aster-mir-gvn-execution-tests tests/test_mir_gvn_execution.c)
    target_link_libraries(aster-mir-gvn-execution-tests PRIVATE asterasm_backend)
    target_compile_definitions(aster-mir-gvn-execution-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-mir-gvn-execution-tests)
    asterasm_enable_sanitizers(aster-mir-gvn-execution-tests)
    add_test(NAME mir_gvn_crossisa_native_execution COMMAND aster-mir-gvn-execution-tests)

    add_executable(aster-target-tests tests/test_target.c)
    target_link_libraries(aster-target-tests PRIVATE asterasm_target)
    target_include_directories(aster-target-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/target")
    target_compile_definitions(aster-target-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-target-tests)
    asterasm_enable_sanitizers(aster-target-tests)
    set(ASTERASM_TARGET_TEST_NAMES
        triple_linux triple_windows feature_x86 feature_a64 feature_riscv x86_high8_rex
        x86_rip_special x86_ymm_gate a64_x31_aliases a64_sve_gate riscv_gpr_aliases
        riscv_fpr_gate immediate_signed immediate_unsigned legalization_program
        x86_encoding_indices x86_extended_aliases x86_avx512_gate a64_alias_widths
        a64_predicate_gate riscv_fpr_alias_map riscv_vector_gate feature_name_aliases
        triple_macos_baremetal triple_unknown_os immediate_64_boundaries verify_value_mutation
        verify_instruction_mutation verify_stats_mutation dump_determinism
        popcnt_independent_feature popcnt_missing_feature popcnt_missing_metadata popcnt_stripped_config popcnt_enabled
        x86_32_legalization x86_16_legalization
        x86_32_reject_unsupported_scalars x86_16_reject_unsupported_scalars
    )
    set(_aster_target_test_index 0)
    foreach(_aster_target_test IN LISTS ASTERASM_TARGET_TEST_NAMES)
        add_test(NAME target_${_aster_target_test} COMMAND aster-target-tests ${_aster_target_test_index})
        math(EXPR _aster_target_test_index "${_aster_target_test_index} + 1")
    endforeach()

    add_executable(aster-backend-tests tests/test_backend.c)
    target_link_libraries(aster-backend-tests PRIVATE asterasm_backend)
    target_include_directories(aster-backend-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/common")
    target_compile_definitions(aster-backend-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-backend-tests)
    asterasm_enable_sanitizers(aster-backend-tests)
    set(ASTERASM_BACKEND_TEST_NAMES
        generic_stage regbank_stage selected_stage stats sysv_gpr_constraint sysv_fp_constraint
        split_u128 aggregate_deferred direct_call_isel integer_add_isel return_isel dump_determinism
        verify_generic_mutation verify_operand_mutation verify_fixed_bank_mutation win64_constraint
        reject_second_isel null_inputs source_abi_lifetime block_api manual_pipeline target_split_identity
        stats_constraints names call_operand_constraints call_result_constraint return_constraint
        verify_call_same_bank_mutation fixed_operand_stats
    )
    set(_aster_backend_test_index 0)
    foreach(_aster_backend_test IN LISTS ASTERASM_BACKEND_TEST_NAMES)
        add_test(NAME backend_${_aster_backend_test} COMMAND aster-backend-tests ${_aster_backend_test_index})
        math(EXPR _aster_backend_test_index "${_aster_backend_test_index} + 1")
    endforeach()

    add_executable(aster-machine-verify-tests tests/test_machine_verify.c)
    target_link_libraries(aster-machine-verify-tests PRIVATE asterasm_backend)
    target_include_directories(aster-machine-verify-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/common")
    target_compile_definitions(aster-machine-verify-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-machine-verify-tests)
    asterasm_enable_sanitizers(aster-machine-verify-tests)
    set(ASTERASM_MACHINE_VERIFY_TEST_NAMES
        token intrinsic effects flags result operand operand_kind immediate stack_slot symbol
        source_map source_parts source_count part_identity width reg_class argument_flag
        operand_flags vreg_flags source_span opcode missing_source instruction_order terminator missing_result
        dead_live_value dead_terminator invalid_dead_flag alias_range forged_alias symbol_module)
    set(_aster_machine_verify_index 0)
    foreach(_case IN LISTS ASTERASM_MACHINE_VERIFY_TEST_NAMES)
        foreach(_arch RANGE 0 2)
            foreach(_stage RANGE 0 2)
                add_test(NAME machine_verify_${_case}_arch${_arch}_stage${_stage}
                    COMMAND aster-machine-verify-tests ${_aster_machine_verify_index} ${_arch} ${_stage})
            endforeach()
        endforeach()
        math(EXPR _aster_machine_verify_index "${_aster_machine_verify_index} + 1")
    endforeach()

    add_executable(aster-machine-peephole-tests tests/test_machine_peephole.c)
    target_link_libraries(aster-machine-peephole-tests PRIVATE asterasm_backend)
    target_include_directories(aster-machine-peephole-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/common" "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/regalloc")
    asterasm_enable_warnings(aster-machine-peephole-tests)
    asterasm_enable_sanitizers(aster-machine-peephole-tests)
    add_test(NAME machine_peephole COMMAND aster-machine-peephole-tests)
    add_executable(aster-physical-peephole-tests tests/test_physical_peephole.c)
    target_link_libraries(aster-physical-peephole-tests PRIVATE asterasm_backend)
    target_include_directories(aster-physical-peephole-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/postra")
    asterasm_enable_warnings(aster-physical-peephole-tests)
    asterasm_enable_sanitizers(aster-physical-peephole-tests)
    add_test(NAME physical_peephole COMMAND aster-physical-peephole-tests)

    add_executable(aster-signed-div-magic-tests tests/test_signed_div_magic.c)
    target_compile_features(aster-signed-div-magic-tests PRIVATE c_std_17)
    asterasm_enable_warnings(aster-signed-div-magic-tests)
    asterasm_enable_sanitizers(aster-signed-div-magic-tests)
    add_test(NAME compiler_signed_magic_division_math COMMAND aster-signed-div-magic-tests)
    add_executable(aster-narrow-div-magic-tests tests/test_narrow_div_magic.c)
    target_compile_features(aster-narrow-div-magic-tests PRIVATE c_std_17)
    asterasm_enable_warnings(aster-narrow-div-magic-tests)
    asterasm_enable_sanitizers(aster-narrow-div-magic-tests)
    add_test(NAME compiler_narrow_magic_math COMMAND aster-narrow-div-magic-tests)
    add_executable(aster-div-magic-tests tests/test_div_magic.c)
    asterasm_enable_warnings(aster-div-magic-tests)
    asterasm_enable_sanitizers(aster-div-magic-tests)
    add_test(NAME compiler_magic_division_math COMMAND aster-div-magic-tests)

    add_executable(aster-encoder-tests tests/test_encoder.c)
    target_link_libraries(aster-encoder-tests PRIVATE asterasm_backend)
    target_include_directories(aster-encoder-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/encoder" "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/regalloc")
    asterasm_enable_warnings(aster-encoder-tests)
    asterasm_enable_sanitizers(aster-encoder-tests)
    add_test(NAME encoder_forms_and_boundaries COMMAND aster-encoder-tests)
    add_executable(aster-native-processor-tests tests/test_native_processor.c)
    target_link_libraries(aster-native-processor-tests PRIVATE asterasm_backend)
    target_include_directories(aster-native-processor-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/postra"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/encoder"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/regalloc")
    target_compile_definitions(aster-native-processor-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-native-processor-tests)
    asterasm_enable_sanitizers(aster-native-processor-tests)
    add_test(NAME native_processor_source_proof COMMAND aster-native-processor-tests)
    add_test(NAME native_binding_source_proof COMMAND aster-native-processor-tests --bindings)
    add_test(NAME native_scalar_source_proof COMMAND aster-native-processor-tests --scalar)
    add_test(NAME native_operand_source_proof COMMAND aster-native-processor-tests --operands)
    add_test(NAME native_memory_source_proof COMMAND aster-native-processor-tests --memory)
    add_test(NAME native_aarch64_scalar_source_proof COMMAND aster-native-processor-tests --a64-scalar)
    add_test(NAME native_data_source_proof COMMAND aster-native-processor-tests --data)
    add_test(NAME native_scalable_source_proof COMMAND aster-native-processor-tests --scalable)
    add_executable(aster-native-vector-tests tests/test_native_vector.c)
    target_link_libraries(aster-native-vector-tests PRIVATE asterasm_backend)
    target_include_directories(aster-native-vector-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/postra"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/regalloc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/encoder")
    target_compile_definitions(aster-native-vector-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-native-vector-tests)
    asterasm_enable_sanitizers(aster-native-vector-tests)
    add_test(NAME native_vector_source_proof COMMAND aster-native-vector-tests)
    add_executable(aster-encoder-module tests/encoder_module.c)
    target_link_libraries(aster-encoder-module PRIVATE asterasm_backend)
    target_compile_definitions(aster-encoder-module PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-encoder-module)
    asterasm_enable_sanitizers(aster-encoder-module)
    if(ASTERASM_BUILD_OBJECT_WRITER)
        add_executable(aster-object-writer-tests tests/test_object_writer.c)
        target_link_libraries(aster-object-writer-tests PRIVATE asterasm_object)
        asterasm_enable_warnings(aster-object-writer-tests)
        add_test(NAME object_writer_c_ownership COMMAND aster-object-writer-tests)
        add_executable(aster-object-writer-static-tests tests/test_object_writer.c)
        target_link_libraries(aster-object-writer-static-tests PRIVATE asterasm_object_static)
        asterasm_enable_warnings(aster-object-writer-static-tests)
        add_test(NAME object_writer_c_static_ownership COMMAND aster-object-writer-static-tests)
        if(WIN32)
            add_custom_command(TARGET aster-object-writer-tests POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different $<TARGET_FILE:asterasm_object> $<TARGET_FILE_DIR:aster-object-writer-tests>)
        endif()
        target_link_libraries(aster-encoder-module PRIVATE asterasm_object)
        target_compile_definitions(aster-encoder-module PRIVATE ASTERASM_HAS_OBJECT_WRITER=1)
        if(WIN32)
            add_custom_command(TARGET aster-encoder-module POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different $<TARGET_FILE:asterasm_object> $<TARGET_FILE_DIR:aster-encoder-module>)
        endif()
    endif()
    set(ASTERASM_UNICORN_PYTHON_PATH "" CACHE PATH "Optional test-only Unicorn Python package directory")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        set(_backend_parallel_args)
        if(ASTERASM_BUILD_OBJECT_WRITER)
            list(APPEND _backend_parallel_args --object-writer)
        endif()
        add_test(NAME compiler_backend_parallel COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_backend_parallel.py"
            --compiler $<TARGET_FILE:asterc> ${_backend_parallel_args})
        set_tests_properties(compiler_backend_parallel PROPERTIES TIMEOUT 600 LABELS "backend;parallel;determinism")
    endif()
    if((ASTERASM_REQUIRE_DIVISION_QEMU OR ASTERASM_REQUIRE_BAREMETAL_QEMU) AND NOT Python3_Interpreter_FOUND)
        message(FATAL_ERROR "Required QEMU qualification needs a Python interpreter")
    endif()
    set(ASTERASM_UNICORN_FOUND FALSE)
    if(Python3_Interpreter_FOUND)
        execute_process(COMMAND "${Python3_EXECUTABLE}" -c
            "import sys; sys.path.insert(0, sys.argv[1]); import unicorn"
            "${ASTERASM_UNICORN_PYTHON_PATH}"
            RESULT_VARIABLE _aster_unicorn_import OUTPUT_QUIET ERROR_QUIET)
        if(_aster_unicorn_import EQUAL 0)
            set(ASTERASM_UNICORN_FOUND TRUE)
        endif()
    endif()
    if(ASTERASM_REQUIRE_UNICORN AND NOT ASTERASM_UNICORN_FOUND)
        message(FATAL_ERROR "Unicorn CPU qualification required, but Python cannot import unicorn")
    endif()
    if(ASTERASM_BUILD_OBJECT_WRITER AND Python3_Interpreter_FOUND)
        find_program(ASTERASM_LLVM_READOBJ llvm-readobj)
        find_program(ASTERASM_LD_LLD ld.lld)
        if(WIN32 AND TARGET asterc AND ASTERASM_LLVM_READOBJ)
            find_program(ASTERASM_EXTERN_TEST_CLANG clang)
            find_program(ASTERASM_EXTERN_TEST_LLD lld-link)
            if(ASTERASM_EXTERN_TEST_CLANG AND ASTERASM_EXTERN_TEST_LLD)
                add_test(NAME compiler_unused_extern COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_unused_extern.py"
                    --compiler $<TARGET_FILE:asterc> --clang "${ASTERASM_EXTERN_TEST_CLANG}"
                    --readobj "${ASTERASM_LLVM_READOBJ}")
                set_tests_properties(compiler_unused_extern PROPERTIES TIMEOUT 120)
                add_test(NAME compiler_editor_regressions COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_editor_regressions.py"
                    --compiler $<TARGET_FILE:asterc> --clang "${ASTERASM_EXTERN_TEST_CLANG}")
                set_tests_properties(compiler_editor_regressions PROPERTIES TIMEOUT 120)
                add_test(NAME compiler_division_win64_c_abi COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_division_win64_abi.py"
                    --compiler $<TARGET_FILE:asterc> --clang "${ASTERASM_EXTERN_TEST_CLANG}")
                set_tests_properties(compiler_division_win64_c_abi PROPERTIES TIMEOUT 120 LABELS "division;abi;coff")
            endif()
        endif()
        if(ASTERASM_LLVM_READOBJ AND ASTERASM_LD_LLD)
            add_test(NAME object_writer_llvm_oracle COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/object_writer_oracle.py"
                --compiler $<TARGET_FILE:aster-encoder-module> --readobj "${ASTERASM_LLVM_READOBJ}" --linker "${ASTERASM_LD_LLD}")
            set_tests_properties(object_writer_llvm_oracle PROPERTIES TIMEOUT 120)
        endif()
    endif()
    if(Python3_Interpreter_FOUND AND TARGET asterc)
        add_test(NAME asterdump_cli
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/asterdump_cli.py"
                --compiler $<TARGET_FILE:asterc> --dumper $<TARGET_FILE:asterdump>)
        set_tests_properties(asterdump_cli PROPERTIES TIMEOUT 90)
        find_program(ASTERASM_DUMPER_TEST_CLANG NAMES clang)
        add_test(NAME asterdump_x86_cli
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/asterdump_x86_cli.py"
                --dumper $<TARGET_FILE:asterdump> --clang "${ASTERASM_DUMPER_TEST_CLANG}")
        set_tests_properties(asterdump_x86_cli PROPERTIES TIMEOUT 120)
        # Slice 18: independent target-assembler bytes plus strict decoding,
        # including x86 variable-length, A64 and feature-gated RV64IMFD.
        if(ASTERASM_DUMPER_TEST_CLANG)
            add_test(NAME asterdump_isa_coverage
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/asterdump_isa_coverage.py"
                    --dumper $<TARGET_FILE:asterdump>
                    --compiler $<TARGET_FILE:asterc>
                    --clang "${ASTERASM_DUMPER_TEST_CLANG}")
            set_tests_properties(asterdump_isa_coverage PROPERTIES TIMEOUT 180)
            add_test(NAME native_processor_contracts
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_processor_contracts.py"
                    --compiler $<TARGET_FILE:asterc> --dumper $<TARGET_FILE:asterdump>
                    --clang "${ASTERASM_DUMPER_TEST_CLANG}")
            set_tests_properties(native_processor_contracts PROPERTIES TIMEOUT 1200)
            add_test(NAME native_scalable_contracts COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_scalable_contracts.py"
                --compiler $<TARGET_FILE:asterc> --dumper $<TARGET_FILE:asterdump>
                --clang "${ASTERASM_DUMPER_TEST_CLANG}")
            set_tests_properties(native_scalable_contracts PROPERTIES TIMEOUT 1200 LABELS "native;scalable;oracle")
            if(ASTERASM_UNICORN_FOUND)
                foreach(_scalar_suite IN ITEMS scalar branch memory indirect aarch64_scalar data bit)
                    add_test(NAME native_${_scalar_suite}_contracts COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_${_scalar_suite}_contracts.py"
                        --compiler $<TARGET_FILE:asterc> --dumper $<TARGET_FILE:asterdump>
                        --clang "${ASTERASM_DUMPER_TEST_CLANG}" --python-path "${ASTERASM_UNICORN_PYTHON_PATH}")
                    set_tests_properties(native_${_scalar_suite}_contracts PROPERTIES TIMEOUT 1200 LABELS "native;scalar;oracle;cpu")
                endforeach()
            endif()
            add_test(NAME native_vector_contracts COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_vector_contracts.py"
                --compiler $<TARGET_FILE:asterc> --dumper $<TARGET_FILE:asterdump>
                --clang "${ASTERASM_DUMPER_TEST_CLANG}" --proof $<TARGET_FILE:aster-native-vector-tests>)
            add_test(NAME native_vector_other_contracts COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_vector_other_contracts.py"
                --compiler $<TARGET_FILE:asterc> --dumper $<TARGET_FILE:asterdump>
                --clang "${ASTERASM_DUMPER_TEST_CLANG}" --proof $<TARGET_FILE:aster-native-vector-tests>)
            set_tests_properties(native_vector_contracts native_vector_other_contracts PROPERTIES TIMEOUT 600 LABELS "native;vector;oracle")
            find_program(ASTERASM_VECTOR_QEMU_X86 NAMES qemu-system-x86_64)
            find_program(ASTERASM_VECTOR_QEMU_A64 NAMES qemu-system-aarch64)
            find_program(ASTERASM_VECTOR_QEMU_RV NAMES qemu-system-riscv64)
            if(ASTERASM_VECTOR_QEMU_A64)
                add_test(NAME native_scalable_execution COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_scalable_execution.py"
                    --compiler $<TARGET_FILE:asterc> --clang "${ASTERASM_DUMPER_TEST_CLANG}")
                set_tests_properties(native_scalable_execution PROPERTIES TIMEOUT 600 LABELS "native;scalable;qemu;cpu")
            endif()
            if(ASTERASM_VECTOR_QEMU_X86)
                add_test(NAME native_vector_execution COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_vector_execution.py"
                    --compiler $<TARGET_FILE:asterc> --clang "${ASTERASM_DUMPER_TEST_CLANG}")
                set_tests_properties(native_vector_execution PROPERTIES TIMEOUT 600 LABELS "native;vector;qemu;cpu")
            endif()
            if(ASTERASM_VECTOR_QEMU_A64 AND ASTERASM_VECTOR_QEMU_RV)
                if(ASTERASM_UNICORN_FOUND)
                    add_test(NAME native_scalar_execution COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_scalar_execution.py"
                        --compiler $<TARGET_FILE:asterc> --clang "${ASTERASM_DUMPER_TEST_CLANG}"
                        --python-path "${ASTERASM_UNICORN_PYTHON_PATH}")
                    set_tests_properties(native_scalar_execution PROPERTIES TIMEOUT 600 LABELS "native;scalar;qemu;cpu")
                    foreach(_data_suite IN ITEMS data bit)
                        add_test(NAME native_${_data_suite}_execution COMMAND "${Python3_EXECUTABLE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_${_data_suite}_contracts.py" --qemu
                            --compiler $<TARGET_FILE:asterc> --clang "${ASTERASM_DUMPER_TEST_CLANG}"
                            --python-path "${ASTERASM_UNICORN_PYTHON_PATH}")
                        set_tests_properties(native_${_data_suite}_execution PROPERTIES TIMEOUT 1200 LABELS "native;scalar;qemu;cpu")
                    endforeach()
                    add_test(NAME native_aarch64_scalar_execution COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_aarch64_scalar_contracts.py" --qemu
                        --compiler $<TARGET_FILE:asterc> --dumper $<TARGET_FILE:asterdump> --clang "${ASTERASM_DUMPER_TEST_CLANG}"
                        --python-path "${ASTERASM_UNICORN_PYTHON_PATH}")
                    set_tests_properties(native_aarch64_scalar_execution PROPERTIES TIMEOUT 600 LABELS "native;scalar;qemu;cpu")
                    add_test(NAME native_memory_execution COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_memory_contracts.py" --qemu
                        --compiler $<TARGET_FILE:asterc> --clang "${ASTERASM_DUMPER_TEST_CLANG}"
                        --python-path "${ASTERASM_UNICORN_PYTHON_PATH}")
                    set_tests_properties(native_memory_execution PROPERTIES TIMEOUT 600 LABELS "native;memory;qemu;cpu")
                    add_test(NAME native_indirect_execution COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_indirect_contracts.py" --qemu
                        --compiler $<TARGET_FILE:asterc> --clang "${ASTERASM_DUMPER_TEST_CLANG}"
                        --python-path "${ASTERASM_UNICORN_PYTHON_PATH}")
                    set_tests_properties(native_indirect_execution PROPERTIES TIMEOUT 600 LABELS "native;abi;qemu;cpu")
                    add_test(NAME native_binding_execution COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_binding_execution.py"
                        --compiler $<TARGET_FILE:asterc> --clang "${ASTERASM_DUMPER_TEST_CLANG}"
                        --proof $<TARGET_FILE:aster-native-processor-tests> --dumper $<TARGET_FILE:asterdump>
                        --python-path "${ASTERASM_UNICORN_PYTHON_PATH}")
                    set_tests_properties(native_binding_execution PROPERTIES TIMEOUT 600 LABELS "native;abi;qemu;cpu")
                endif()
                add_test(NAME native_exception_execution COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_exception_execution.py"
                    --compiler $<TARGET_FILE:asterc> --clang "${ASTERASM_DUMPER_TEST_CLANG}")
                set_tests_properties(native_exception_execution PROPERTIES TIMEOUT 600 LABELS "native;privileged;qemu;cpu")
                add_test(NAME native_vector_other_execution COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_vector_other_execution.py"
                    --compiler $<TARGET_FILE:asterc> --clang "${ASTERASM_DUMPER_TEST_CLANG}")
                set_tests_properties(native_vector_other_execution PROPERTIES TIMEOUT 600 LABELS "native;vector;qemu;cpu")
            endif()
        endif()
        add_test(NAME asterdump_elf_cli
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/asterdump_elf_cli.py"
                --dumper $<TARGET_FILE:asterdump>)
        set_tests_properties(asterdump_elf_cli PROPERTIES TIMEOUT 90)
        find_program(ASTERASM_DUMPER_TEST_LLD_LINK NAMES lld-link)
        set(_asterasm_coff_lld_args)
        if(ASTERASM_DUMPER_TEST_LLD_LINK)
            list(APPEND _asterasm_coff_lld_args --lld-link "${ASTERASM_DUMPER_TEST_LLD_LINK}")
        endif()
        add_test(NAME asterdump_coff_mutation
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/asterdump_coff_mutation.py"
                --dumper $<TARGET_FILE:asterdump>)
        set_tests_properties(asterdump_coff_mutation PROPERTIES TIMEOUT 120)
        set(_asterasm_coff_clang_args)
        if(ASTERASM_DUMPER_TEST_CLANG)
            list(APPEND _asterasm_coff_clang_args --clang "${ASTERASM_DUMPER_TEST_CLANG}")
        endif()
        add_test(NAME asterdump_coff_pe_cli
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/asterdump_coff_pe_cli.py"
                --dumper $<TARGET_FILE:asterdump>
                ${_asterasm_coff_clang_args} ${_asterasm_coff_lld_args})
        set_tests_properties(asterdump_coff_pe_cli PROPERTIES TIMEOUT 120)
        add_test(NAME asterdump_linked_elf_cli
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/asterdump_linked_elf_cli.py"
                --dumper $<TARGET_FILE:asterdump> --clang "${ASTERASM_DUMPER_TEST_CLANG}")
        set_tests_properties(asterdump_linked_elf_cli PROPERTIES TIMEOUT 120)

        add_test(NAME formatter_unary_regressions
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/formatter_unary_regressions.py"
                --formatter "${CMAKE_CURRENT_SOURCE_DIR}/tools/asterfmt.py")
        add_test(NAME frontend_context_contracts COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/frontend_context_contracts.py"
            --parser $<TARGET_FILE:aster-ast> --lexer $<TARGET_FILE:aster-lex>
            --compiler $<TARGET_FILE:asterc>)
        set_tests_properties(frontend_context_contracts PROPERTIES TIMEOUT 600 LABELS "frontend;formatter;grammar")
        set(_aster_report_test_args)
        if(NOT ASTERASM_BUILD_OBJECT_WRITER)
            list(APPEND _aster_report_test_args --c-elf-fallback)
        endif()
        add_test(NAME compiler_optimization_report COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_optimization_report.py" --compiler $<TARGET_FILE:asterc> ${_aster_report_test_args})
        set_tests_properties(compiler_optimization_report PROPERTIES TIMEOUT 180 LABELS "frontend;tooling")
        add_test(NAME compiler_ir_dump_e2e
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_ir_dump_e2e.py"
                --compiler $<TARGET_FILE:asterc>)
        add_test(NAME compiler_unsigned_pow2_codegen
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_pow2_codegen.py"
                --compiler $<TARGET_FILE:asterc>)
        set_tests_properties(formatter_unary_regressions compiler_ir_dump_e2e
            compiler_unsigned_pow2_codegen PROPERTIES TIMEOUT 120)
        # 139 source-level semantic shapes x {x86_16,x86_32} x
        # {promoted, stack-local MIR}. These are intentionally separate CTest
        # cases so a regression points at one target/semantic contract instead
        # of hiding hundreds of checks behind one aggregate result.
        add_test(NAME riscv64_baseline_integer_arithmetic
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/riscv64_baseline_arithmetic.py"
                --compiler $<TARGET_FILE:asterc>)
        set_tests_properties(riscv64_baseline_integer_arithmetic PROPERTIES TIMEOUT 60)
        add_test(NAME legacy_scalar_regression
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy_scalar_regression.py"
                --compiler $<TARGET_FILE:asterc>)
        set_tests_properties(legacy_scalar_regression PROPERTIES TIMEOUT 60)
        foreach(_legacy_case RANGE 0 555)
            add_test(NAME legacy_compile_matrix_${_legacy_case}
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy_compile_matrix.py"
                    --compiler $<TARGET_FILE:asterc> --case ${_legacy_case})
            set_tests_properties(legacy_compile_matrix_${_legacy_case} PROPERTIES TIMEOUT 45)
        endforeach()
        # 3 backends x 6 semantic shapes x 4 scalar element widths x 3
        # lane counts. Register every case independently so vector regressions
        # stay visible in CTest and sanitizer/strict-build gates.
        foreach(_fixed_vector_case RANGE 0 215)
            add_test(NAME fixed_vector_codegen_matrix_${_fixed_vector_case}
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixed_vector_codegen_matrix.py"
                    --compiler $<TARGET_FILE:asterc> --case ${_fixed_vector_case})
            set_tests_properties(fixed_vector_codegen_matrix_${_fixed_vector_case} PROPERTIES TIMEOUT 45)
        endforeach()
        foreach(_native_simd_case RANGE 0 4)
            add_test(NAME native_simd_codegen_${_native_simd_case}
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_simd_codegen.py"
                    --compiler $<TARGET_FILE:asterc> --case ${_native_simd_case})
            set_tests_properties(native_simd_codegen_${_native_simd_case} PROPERTIES TIMEOUT 30)
        endforeach()
        foreach(_native_address_case RANGE 0 3)
            add_test(NAME native_address_codegen_${_native_address_case}
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_address_codegen.py"
                    --compiler $<TARGET_FILE:asterc> --case ${_native_address_case})
            set_tests_properties(native_address_codegen_${_native_address_case} PROPERTIES TIMEOUT 30)
        endforeach()
        find_program(ASTERASM_MAGIC_ORACLE_CLANG NAMES clang)
        find_program(ASTERASM_MAGIC_ORACLE_READELF NAMES readelf)
        set(_magic_oracle_args)
        if(ASTERASM_MAGIC_ORACLE_CLANG AND ASTERASM_MAGIC_ORACLE_READELF)
            list(APPEND _magic_oracle_args --clang "${ASTERASM_MAGIC_ORACLE_CLANG}")
        endif()
        set(_x86_magic_runtime)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
            list(APPEND _x86_magic_runtime --execute)
        endif()
        # Division target-word execution. These tests fail closed when a
        # generated instruction is outside the independent decoder subset.
        foreach(_division_isa_test IN ITEMS compiler_narrow_isa_execution
                                        compiler_dynamic_div_abi
                                        compiler_multisite_div_abi)
            add_test(NAME ${_division_isa_test}
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/${_division_isa_test}.py"
                    --compiler $<TARGET_FILE:asterc>)
            set_tests_properties(${_division_isa_test} PROPERTIES TIMEOUT 240)
        endforeach()
        # Division cross-target callee-saved and native SIGFPE qualification.
        # Only the x86_64 probe is hardware-executed; A64/RV64 use the
        # independent fail-closed target instruction decoders.
        add_test(NAME compiler_division_abi_preservation
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_division_abi_preservation.py"
                --compiler $<TARGET_FILE:asterc>
                --cc "${CMAKE_C_COMPILER}")
        set_tests_properties(compiler_division_abi_preservation PROPERTIES TIMEOUT 240)
        add_test(NAME compiler_division_fault_contract
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_division_fault_contract.py"
                --compiler $<TARGET_FILE:asterc>)
        set_tests_properties(compiler_division_fault_contract PROPERTIES TIMEOUT 120)
        add_test(NAME compiler_division_policy_environment
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_division_policy_environment.py"
                --compiler $<TARGET_FILE:asterc>)
        set_tests_properties(compiler_division_policy_environment PROPERTIES TIMEOUT 120 LABELS "division")
        find_program(ASTERASM_DIVISION_CLANG NAMES clang)
        find_program(ASTERASM_DIVISION_READELF NAMES readelf llvm-readelf)
        find_program(ASTERASM_DIVISION_LLD NAMES ld.lld)
        find_program(ASTERASM_DIVISION_QEMU_A64 NAMES qemu-aarch64)
        find_program(ASTERASM_DIVISION_QEMU_RV64 NAMES qemu-riscv64)
        find_program(ASTERASM_DIVISION_SYSTEM_A64 NAMES qemu-system-aarch64)
        find_program(ASTERASM_DIVISION_SYSTEM_RV64 NAMES qemu-system-riscv64)
        if(ASTERASM_REQUIRE_BAREMETAL_QEMU AND
           (NOT ASTERASM_DIVISION_CLANG OR NOT ASTERASM_DIVISION_LLD OR
            NOT ASTERASM_DIVISION_SYSTEM_A64 OR NOT ASTERASM_DIVISION_SYSTEM_RV64))
            message(FATAL_ERROR "Required bare-metal qualification needs Clang/LLD and system QEMU for AArch64/RV64")
        endif()
        # System emulation is independent of Linux user-mode QEMU. In
        # particular Windows can run both bare-metal CPUs without qemu-user.
        if(ASTERASM_DIVISION_CLANG AND ASTERASM_DIVISION_LLD AND
           ASTERASM_DIVISION_SYSTEM_A64 AND ASTERASM_DIVISION_SYSTEM_RV64)
            add_test(NAME compiler_baremetal_qemu
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_baremetal_qemu.py"
                    --compiler $<TARGET_FILE:asterc> --clang "${ASTERASM_DIVISION_CLANG}")
            set_tests_properties(compiler_baremetal_qemu PROPERTIES
                TIMEOUT 180 LABELS "division;qemu;baremetal")
            add_test(NAME compiler_baremetal_optimizer_qemu
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_baremetal_qemu.py"
                    --compiler $<TARGET_FILE:asterc> --clang "${ASTERASM_DIVISION_CLANG}" --opt-level 2)
            set_tests_properties(compiler_baremetal_optimizer_qemu PROPERTIES
                TIMEOUT 180 LABELS "mir;optimizer;qemu;baremetal")
        endif()
        if(ASTERASM_REQUIRE_DIVISION_QEMU AND
           (NOT ASTERASM_DIVISION_CLANG OR NOT ASTERASM_DIVISION_READELF OR
            NOT ASTERASM_DIVISION_LLD OR NOT ASTERASM_DIVISION_QEMU_A64 OR NOT ASTERASM_DIVISION_QEMU_RV64))
            message(FATAL_ERROR "Division QEMU runtime qualification needs Clang/LLD and readelf")
        endif()
        if(ASTERASM_DIVISION_CLANG AND ASTERASM_DIVISION_READELF)
            set(_division_qemu_args)
            if(ASTERASM_REQUIRE_DIVISION_QEMU)
                list(APPEND _division_qemu_args --require-qemu)
                add_test(NAME compiler_division_optimizer_qemu_elf
                    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_division_qemu_elf.py"
                        --compiler $<TARGET_FILE:asterc> --clang "${ASTERASM_DIVISION_CLANG}"
                        --readelf "${ASTERASM_DIVISION_READELF}" --require-qemu --opt-level 2)
                set_tests_properties(compiler_division_optimizer_qemu_elf PROPERTIES
                    TIMEOUT 240 LABELS "mir;optimizer;division;qemu")
            endif()
            add_test(NAME compiler_division_qemu_elf
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_division_qemu_elf.py"
                    --compiler $<TARGET_FILE:asterc>
                    --clang "${ASTERASM_DIVISION_CLANG}"
                    --readelf "${ASTERASM_DIVISION_READELF}"
                    ${_division_qemu_args})
            if(WIN32)
                # 192 runners each spawn asterc, Clang/LLD and two ELF readers.
                # Keep all cases and per-child deadlines on slower process
                # startup hosts; the Linux qualification budget is unchanged.
                set_tests_properties(compiler_division_qemu_elf PROPERTIES TIMEOUT 600)
            else()
                set_tests_properties(compiler_division_qemu_elf PROPERTIES TIMEOUT 240)
            endif()
            if(ASTERASM_DIVISION_LLD AND ASTERASM_DIVISION_QEMU_A64 AND ASTERASM_DIVISION_QEMU_RV64)
                add_test(NAME compiler_external_c_abi_qemu
                    COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_external_c_abi_qemu.py"
                        --compiler $<TARGET_FILE:asterc> --clang "${ASTERASM_DIVISION_CLANG}")
                set_tests_properties(compiler_external_c_abi_qemu PROPERTIES
                    TIMEOUT 240 LABELS "division;qemu;abi")
                add_test(NAME compiler_external_c_abi_optimizer_qemu
                    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_external_c_abi_qemu.py"
                        --compiler $<TARGET_FILE:asterc> --clang "${ASTERASM_DIVISION_CLANG}" --opt-level 2)
                set_tests_properties(compiler_external_c_abi_optimizer_qemu PROPERTIES
                    TIMEOUT 240 LABELS "mir;optimizer;qemu;abi")
            endif()
        endif()
        add_test(NAME compiler_narrow_div_cross_codegen
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_narrow_div_cross_codegen.py"
                --compiler $<TARGET_FILE:asterc> ${_magic_oracle_args})
        set_tests_properties(compiler_narrow_div_cross_codegen PROPERTIES TIMEOUT 180)
        add_test(NAME compiler_x86_narrow_magic_execution
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_x86_narrow_magic_execution.py"
                --compiler $<TARGET_FILE:asterc>
                ${_x86_magic_runtime} ${_magic_oracle_args})
        set_tests_properties(compiler_x86_narrow_magic_execution PROPERTIES TIMEOUT 180)
        add_test(NAME compiler_x86_narrow_size_execution
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_x86_narrow_magic_execution.py"
                --compiler $<TARGET_FILE:asterc>
                --policy size ${_x86_magic_runtime} ${_magic_oracle_args})
        set_tests_properties(compiler_x86_narrow_size_execution PROPERTIES TIMEOUT 180)
        add_test(NAME compiler_x86_signed_magic_execution
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_x86_signed_magic_execution.py"
                --compiler $<TARGET_FILE:asterc> --dumper $<TARGET_FILE:asterdump>
                ${_x86_magic_runtime} ${_magic_oracle_args})
        set_tests_properties(compiler_x86_signed_magic_execution PROPERTIES TIMEOUT 180)
        add_test(NAME compiler_x86_magic_execution
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_x86_magic_execution.py"
                --compiler $<TARGET_FILE:asterc> --dumper $<TARGET_FILE:asterdump>
                ${_x86_magic_runtime} ${_magic_oracle_args})
        set_tests_properties(compiler_x86_magic_execution PROPERTIES TIMEOUT 180)
        add_test(NAME compiler_signed_magic_cross_codegen
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_signed_magic_cross_codegen.py"
                --compiler $<TARGET_FILE:asterc> ${_magic_oracle_args})
        set_tests_properties(compiler_signed_magic_cross_codegen PROPERTIES TIMEOUT 180)
        add_test(NAME compiler_signed_magic_rv_execution
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_signed_magic_rv_execution.py"
                --compiler $<TARGET_FILE:asterc>)
        set_tests_properties(compiler_signed_magic_rv_execution PROPERTIES TIMEOUT 180)
        add_test(NAME compiler_signed_magic_a64_execution
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_signed_magic_a64_execution.py"
                --compiler $<TARGET_FILE:asterc>)
        set_tests_properties(compiler_signed_magic_a64_execution PROPERTIES TIMEOUT 180)
        add_test(NAME compiler_magic_cross_codegen
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_magic_cross_codegen.py"
                --compiler $<TARGET_FILE:asterc> --dumper $<TARGET_FILE:asterdump>
                ${_magic_oracle_args})
        set_tests_properties(compiler_magic_cross_codegen PROPERTIES TIMEOUT 180)
        add_test(NAME compiler_pow2_cross_codegen
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_pow2_cross_codegen.py"
                --compiler $<TARGET_FILE:asterc>)
        set_tests_properties(compiler_pow2_cross_codegen PROPERTIES TIMEOUT 90)
        # Cross-ISA manual register and exact-encoding slice, with an optional
        # independent assembler oracle. Plain acceptance/rejection tests never
        # require LLVM/Clang or binutils as compiler dependencies.
        find_program(ASTERASM_MANUAL_ORACLE_CLANG NAMES clang)
        find_program(ASTERASM_MANUAL_ORACLE_READELF NAMES readelf)
        set(_manual_oracle_args)
        if(ASTERASM_MANUAL_ORACLE_CLANG AND ASTERASM_MANUAL_ORACLE_READELF)
            list(APPEND _manual_oracle_args --clang "${ASTERASM_MANUAL_ORACLE_CLANG}")
        endif()
        foreach(_manual_isa aarch64 riscv64)
            add_test(NAME native_manual_exact_${_manual_isa}
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_manual_cross_isa.py"
                    --compiler $<TARGET_FILE:asterc> --target ${_manual_isa}
                    ${_manual_oracle_args})
            set_tests_properties(native_manual_exact_${_manual_isa} PROPERTIES TIMEOUT 120)
        endforeach()
        # First G1 control-flow slice: local labels / exact forward B and JAL x0.
        foreach(_manual_isa aarch64 riscv64)
            add_test(NAME native_manual_forward_branch_${_manual_isa}
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_manual_forward_branch.py"
                    --compiler $<TARGET_FILE:asterc> --target ${_manual_isa}
                    ${_manual_oracle_args})
            set_tests_properties(native_manual_forward_branch_${_manual_isa} PROPERTIES TIMEOUT 120)
            add_test(NAME native_manual_conditional_branch_${_manual_isa}
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_manual_conditional_branch.py"
                    --compiler $<TARGET_FILE:asterc> --target ${_manual_isa}
                    ${_manual_oracle_args})
            set_tests_properties(native_manual_conditional_branch_${_manual_isa} PROPERTIES TIMEOUT 120)
            add_test(NAME native_manual_path_state_${_manual_isa}
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_manual_path_state.py"
                    --compiler $<TARGET_FILE:asterc> --target ${_manual_isa}
                    ${_manual_oracle_args})
            set_tests_properties(native_manual_path_state_${_manual_isa} PROPERTIES TIMEOUT 120)
            if(NOT ASTERASM_BUILD_OBJECT_WRITER AND Python3_Interpreter_FOUND)
                find_program(ASTERASM_NATIVE_ELF_LLD NAMES ld.lld)
                find_program(ASTERASM_NATIVE_ELF_OBJDUMP NAMES llvm-objdump)
                if(ASTERASM_MANUAL_ORACLE_CLANG AND ASTERASM_MANUAL_ORACLE_READELF
                   AND ASTERASM_NATIVE_ELF_LLD AND ASTERASM_NATIVE_ELF_OBJDUMP)
                    add_test(NAME native_elf64_link_${_manual_isa}
                        COMMAND "${Python3_EXECUTABLE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_elf64_link.py"
                            --compiler $<TARGET_FILE:asterc> --target ${_manual_isa}
                            --clang "${ASTERASM_MANUAL_ORACLE_CLANG}"
                            --readelf "${ASTERASM_MANUAL_ORACLE_READELF}"
                            --lld "${ASTERASM_NATIVE_ELF_LLD}"
                            --objdump "${ASTERASM_NATIVE_ELF_OBJDUMP}"
                            --dumper $<TARGET_FILE:asterdump>)
                    set_tests_properties(native_elf64_link_${_manual_isa} PROPERTIES TIMEOUT 120)
                endif()
            endif()
            if(_manual_isa STREQUAL "riscv64")
                set(_mixed_oracle_args)
                if(ASTERASM_MANUAL_ORACLE_CLANG)
                    list(APPEND _mixed_oracle_args --clang "${ASTERASM_MANUAL_ORACLE_CLANG}")
                endif()
                if(ASTERASM_MANUAL_ORACLE_READELF)
                    list(APPEND _mixed_oracle_args --readelf "${ASTERASM_MANUAL_ORACLE_READELF}")
                endif()
                if(ASTERASM_NATIVE_ELF_LLD)
                    list(APPEND _mixed_oracle_args --lld "${ASTERASM_NATIVE_ELF_LLD}")
                endif()
                add_test(NAME native_mixed_pc_map_riscv64
                    COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_mixed_pc_map.py"
                        --compiler $<TARGET_FILE:asterc>
                        --dumper $<TARGET_FILE:asterdump>
                        ${_mixed_oracle_args})
                set_tests_properties(native_mixed_pc_map_riscv64 PROPERTIES TIMEOUT 180)
            endif()
            add_test(NAME native_external_symbol_calls_${_manual_isa}
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_external_symbol_calls.py"
                    --compiler $<TARGET_FILE:asterc> --target ${_manual_isa}
                    ${_manual_oracle_args}
                    $<$<BOOL:${ASTERASM_MANUAL_ORACLE_READELF}>:--readelf>
                    $<$<BOOL:${ASTERASM_MANUAL_ORACLE_READELF}>:${ASTERASM_MANUAL_ORACLE_READELF}>)
            set_tests_properties(native_external_symbol_calls_${_manual_isa} PROPERTIES TIMEOUT 120)
            add_test(NAME native_manual_local_calls_${_manual_isa}
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_manual_local_calls.py"
                    --compiler $<TARGET_FILE:asterc> --target ${_manual_isa}
                    ${_manual_oracle_args})
            set_tests_properties(native_manual_local_calls_${_manual_isa} PROPERTIES TIMEOUT 120)
            add_test(NAME native_manual_multicall_${_manual_isa}
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_manual_multicall.py"
                    --compiler $<TARGET_FILE:asterc> --target ${_manual_isa})
            set_tests_properties(native_manual_multicall_${_manual_isa} PROPERTIES TIMEOUT 120)
            add_test(NAME native_manual_multicall_execution_${_manual_isa}
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_manual_multicall_execution.py"
                    --compiler $<TARGET_FILE:asterc> --target ${_manual_isa}
                    ${_manual_oracle_args})
            set_tests_properties(native_manual_multicall_execution_${_manual_isa} PROPERTIES TIMEOUT 120)
            add_test(NAME native_manual_call_execution_${_manual_isa}
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_manual_call_execution.py"
                    --compiler $<TARGET_FILE:asterc> --target ${_manual_isa})
            set_tests_properties(native_manual_call_execution_${_manual_isa} PROPERTIES TIMEOUT 120)
            add_test(NAME native_manual_loops_${_manual_isa}
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_manual_loops.py"
                    --compiler $<TARGET_FILE:asterc> --target ${_manual_isa}
                    ${_manual_oracle_args})
            set_tests_properties(native_manual_loops_${_manual_isa} PROPERTIES TIMEOUT 180)
            add_test(NAME native_manual_loop_execution_${_manual_isa}
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_manual_loop_execution.py"
                    --compiler $<TARGET_FILE:asterc> --target ${_manual_isa}
                    --dumper $<TARGET_FILE:asterdump>)
            set_tests_properties(native_manual_loop_execution_${_manual_isa} PROPERTIES TIMEOUT 120)
            add_test(NAME native_manual_cfg_semantics_${_manual_isa}
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_manual_cfg_semantics.py"
                    --compiler $<TARGET_FILE:asterc> --target ${_manual_isa}
                    --dumper $<TARGET_FILE:asterdump>)
            set_tests_properties(native_manual_cfg_semantics_${_manual_isa} PROPERTIES TIMEOUT 120)

        endforeach()
        foreach(_native_manual_case RANGE 0 9)
            add_test(NAME native_manual_control_${_native_manual_case}
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_manual_control.py"
                    --compiler $<TARGET_FILE:asterc> --case ${_native_manual_case})
            set_tests_properties(native_manual_control_${_native_manual_case} PROPERTIES TIMEOUT 30)
        endforeach()
        foreach(_memory_hint_case RANGE 0 5)
            add_test(NAME memory_hint_codegen_${_memory_hint_case}
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/memory_hint_codegen.py"
                        --compiler $<TARGET_FILE:asterc> --case ${_memory_hint_case})
            set_tests_properties(memory_hint_codegen_${_memory_hint_case} PROPERTIES TIMEOUT 30)
        endforeach()

        # Short-circuit boolean CFG regression.  Cover both x86-64 ABIs,
        # unrestricted/constrained register allocation and promoted/non-promoted
        # local homes so rematerialized edge values cannot become fake copies.
        foreach(_logical_binary_case RANGE 0 7)
            add_test(NAME logical_binary_codegen_${_logical_binary_case}
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/logical_binary_codegen.py"
                        --compiler $<TARGET_FILE:asterc> --case ${_logical_binary_case})
            set_tests_properties(logical_binary_codegen_${_logical_binary_case} PROPERTIES TIMEOUT 30)
        endforeach()

        # Legacy x86 atomic tiers: 386 load/store/XCHG baseline, i486
        # CMPXCHG CAS/RMW, and i586 CMPXCHG8B 64-bit atomics.
        foreach(_legacy_atomic_case RANGE 0 9)
            add_test(NAME legacy_atomic_codegen_${_legacy_atomic_case}
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy_atomic_codegen.py"
                        --compiler $<TARGET_FILE:asterc> --case ${_legacy_atomic_case})
            set_tests_properties(legacy_atomic_codegen_${_legacy_atomic_case} PROPERTIES TIMEOUT 30)
        endforeach()

        foreach(_native_high8_raw_case RANGE 0 8)
            add_test(NAME native_high8_raw_codegen_${_native_high8_raw_case}
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_high8_raw_codegen.py"
                    --compiler $<TARGET_FILE:asterc> --case ${_native_high8_raw_case})
            set_tests_properties(native_high8_raw_codegen_${_native_high8_raw_case} PROPERTIES TIMEOUT 30)
        endforeach()
        if(ASTERASM_UNICORN_FOUND)
            set(_legacy_object_writer_args)
            if(ASTERASM_BUILD_OBJECT_WRITER)
                list(APPEND _legacy_object_writer_args --object-writer)
            endif()
            add_test(NAME legacy_cpu_execution COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy_execution.py"
                --compiler $<TARGET_FILE:asterc> --python-path "${ASTERASM_UNICORN_PYTHON_PATH}"
                ${_legacy_object_writer_args})
            set_tests_properties(legacy_cpu_execution PROPERTIES TIMEOUT 180)
        endif()
    endif()

    if(Python3_Interpreter_FOUND AND ASTERASM_UNICORN_FOUND)
        add_test(NAME encoder_compact_immediate_execution COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/encoder_compact_execution.py"
            --test $<TARGET_FILE:aster-encoder-tests> --python-path "${ASTERASM_UNICORN_PYTHON_PATH}")
        set_tests_properties(encoder_compact_immediate_execution PROPERTIES TIMEOUT 60 LABELS "size;unicorn;cpu")
        add_test(NAME encoder_cpu_execution COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/encoder_cpu_execution.py"
            --compiler $<TARGET_FILE:aster-encoder-module> --python-path "${ASTERASM_UNICORN_PYTHON_PATH}")
        set_tests_properties(encoder_cpu_execution PROPERTIES TIMEOUT 120)
        if(TARGET asterc)
            add_test(NAME compiler_inline_resource_limits COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_inline_limits.py"
                --compiler $<TARGET_FILE:asterc> --python-path "${ASTERASM_UNICORN_PYTHON_PATH}")
            set_tests_properties(compiler_inline_resource_limits PROPERTIES TIMEOUT 300 LABELS "mir;inlining;stress;cpu")
            add_test(NAME compiler_inline_explicit_policy COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_inline_policy.py"
                --compiler $<TARGET_FILE:asterc> --python-path "${ASTERASM_UNICORN_PYTHON_PATH}")
            set_tests_properties(compiler_inline_explicit_policy PROPERTIES TIMEOUT 180 LABELS "mir;inlining;cpu")
            add_test(NAME compiler_dense_constant_returns COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_dense_constants.py"
                --compiler $<TARGET_FILE:asterc> --python-path "${ASTERASM_UNICORN_PYTHON_PATH}")
            set_tests_properties(compiler_dense_constant_returns PROPERTIES TIMEOUT 180 LABELS "size;abi;cpu")
            set(_size_policy_object_args)
            if(ASTERASM_BUILD_OBJECT_WRITER)
                list(APPEND _size_policy_object_args --object-writer)
            endif()
            add_test(NAME compiler_size_policy COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_size_policy.py"
                --compiler $<TARGET_FILE:asterc> ${_size_policy_object_args})
            set_tests_properties(compiler_size_policy PROPERTIES TIMEOUT 600 LABELS "size;inlining;object")
            add_test(NAME legacy_size_policy COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy_size_policy.py"
                --compiler $<TARGET_FILE:asterc> ${_size_policy_object_args})
            set_tests_properties(legacy_size_policy PROPERTIES TIMEOUT 600 LABELS "size;legacy;cpu;object")
            foreach(_optimizer_profile RANGE 0 4)
                add_test(NAME compiler_mir_inline_${_optimizer_profile}
                    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_mir_inline.py"
                        --compiler $<TARGET_FILE:asterc> --profile ${_optimizer_profile}
                        --python-path "${ASTERASM_UNICORN_PYTHON_PATH}")
                set_tests_properties(compiler_mir_inline_${_optimizer_profile}
                    PROPERTIES TIMEOUT 360 LABELS "mir;inlining;size;unicorn;cpu")
                add_test(NAME compiler_mir_optimizer_${_optimizer_profile}
                    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_mir_optimizer.py"
                        --compiler $<TARGET_FILE:asterc> --profile ${_optimizer_profile}
                        --python-path "${ASTERASM_UNICORN_PYTHON_PATH}")
                set_tests_properties(compiler_mir_optimizer_${_optimizer_profile}
                    PROPERTIES TIMEOUT 300 LABELS "mir;optimizer;unicorn;cpu")
            endforeach()
            # 5 ABI/ISA profiles x 8 types x constant/dynamic x quotient/remainder.
            # Each named CPU case executes hot and size policies independently.
            foreach(_unicorn_division_case RANGE 0 159)
                add_test(NAME compiler_division_unicorn_${_unicorn_division_case}
                    COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_division_unicorn.py"
                        --compiler $<TARGET_FILE:asterc>
                        --python-path "${ASTERASM_UNICORN_PYTHON_PATH}"
                        --case ${_unicorn_division_case})
                set_tests_properties(compiler_division_unicorn_${_unicorn_division_case}
                    PROPERTIES TIMEOUT 60 LABELS "unicorn;division;cpu")
                add_test(NAME compiler_division_optimized_unicorn_${_unicorn_division_case}
                    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_division_unicorn.py"
                        --compiler $<TARGET_FILE:asterc> --python-path "${ASTERASM_UNICORN_PYTHON_PATH}"
                        --opt-level 2 --case ${_unicorn_division_case})
                set_tests_properties(compiler_division_optimized_unicorn_${_unicorn_division_case}
                    PROPERTIES TIMEOUT 60 LABELS "mir;optimizer;unicorn;division;cpu")
            endforeach()
            add_test(NAME compiler_runtime_execution COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_runtime_execution.py"
                --compiler $<TARGET_FILE:asterc> --python-path "${ASTERASM_UNICORN_PYTHON_PATH}")
            set_tests_properties(compiler_runtime_execution PROPERTIES TIMEOUT 120)
            set(_string_object_args)
            if(ASTERASM_BUILD_OBJECT_WRITER)
                list(APPEND _string_object_args --object-writer)
            endif()
            add_test(NAME compiler_string_literals COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_string_literals.py"
                --compiler $<TARGET_FILE:asterc> --python-path "${ASTERASM_UNICORN_PYTHON_PATH}"
                ${_string_object_args})
            set_tests_properties(compiler_string_literals PROPERTIES TIMEOUT 180 LABELS "hir;strings;unicorn;cpu")
            add_test(NAME compiler_codegen_cleanup COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_codegen_cleanup.py"
                --compiler $<TARGET_FILE:asterc> --python-path "${ASTERASM_UNICORN_PYTHON_PATH}")
            set_tests_properties(compiler_codegen_cleanup PROPERTIES TIMEOUT 240 LABELS "backend;peephole;unicorn;cpu")
            add_test(NAME compiler_runtime_optimized_execution COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_runtime_execution.py"
                --compiler $<TARGET_FILE:asterc> --python-path "${ASTERASM_UNICORN_PYTHON_PATH}" --opt-level 2)
            set_tests_properties(compiler_runtime_optimized_execution PROPERTIES TIMEOUT 180 LABELS "mir;optimizer;cpu")
            add_test(NAME native_system_execution COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_system_execution.py"
                --compiler $<TARGET_FILE:asterc> --python-path "${ASTERASM_UNICORN_PYTHON_PATH}")
            set_tests_properties(native_system_execution PROPERTIES TIMEOUT 120)
            add_test(NAME native_processor_bridges COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_processor_bridges.py"
                --compiler $<TARGET_FILE:asterc>)
            set_tests_properties(native_processor_bridges PROPERTIES TIMEOUT 180)
            foreach(_atomic_mode RANGE 0 5)
                add_test(NAME atomic_cpu_execution_${_atomic_mode} COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/atomic_execution.py"
                    --compiler $<TARGET_FILE:asterc> --python-path "${ASTERASM_UNICORN_PYTHON_PATH}" --mode ${_atomic_mode})
                set_tests_properties(atomic_cpu_execution_${_atomic_mode} PROPERTIES TIMEOUT 300)
            endforeach()
        endif()
    endif()
    if(TARGET asterc AND ASTERASM_BUILD_OBJECT_WRITER AND Python3_Interpreter_FOUND
       AND NOT CMAKE_CROSSCOMPILING AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
        find_program(ASTERASM_ATOMIC_HOST_CLANG NAMES clang)
        if(ASTERASM_ATOMIC_HOST_CLANG)
            add_test(NAME atomic_native_threads COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/atomic_native.py"
                --compiler $<TARGET_FILE:asterc> --clang "${ASTERASM_ATOMIC_HOST_CLANG}")
            set_tests_properties(atomic_native_threads PROPERTIES TIMEOUT 180)
        endif()
    endif()
    find_program(ASTERASM_LLVM_MC NAMES llvm-mc)
    find_program(ASTERASM_LLVM_OBJCOPY NAMES llvm-objcopy)
    if(Python3_Interpreter_FOUND AND ASTERASM_LLVM_MC AND ASTERASM_LLVM_OBJCOPY)
        add_test(NAME encoder_llvm_oracle COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/encoder_llvm_oracle.py"
            --test $<TARGET_FILE:aster-encoder-tests> --mc "${ASTERASM_LLVM_MC}" --objcopy "${ASTERASM_LLVM_OBJCOPY}")
        set_tests_properties(encoder_llvm_oracle PROPERTIES TIMEOUT 120)
    endif()

    add_executable(aster-regalloc-tests tests/test_regalloc.c)
    add_executable(aster-bind-tests tests/test_bind.c)
    add_executable(aster-many-workers-tests tests/test_many_workers.c)
    target_link_libraries(aster-many-workers-tests PRIVATE asterasm_lexer asterasm_core)
    target_include_directories(aster-many-workers-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/core")
    asterasm_enable_warnings(aster-many-workers-tests)
    asterasm_enable_sanitizers(aster-many-workers-tests)
    add_test(NAME parallel_96_workers_join COMMAND aster-many-workers-tests core)
    add_executable(aster-thread-pool-tests tests/test_thread_pool.c)
    target_link_libraries(aster-thread-pool-tests PRIVATE asterasm_core)
    target_include_directories(aster-thread-pool-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/core")
    asterasm_enable_warnings(aster-thread-pool-tests)
    asterasm_enable_sanitizers(aster-thread-pool-tests)
    add_test(NAME thread_pool_core COMMAND aster-thread-pool-tests)
    set_tests_properties(thread_pool_core PROPERTIES TIMEOUT 45 LABELS "parallel;pool;affinity")
    if(Python3_Interpreter_FOUND)
        add_test(NAME thread_pool_cgroup_quotas COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/thread_pool_quotas.py" --test $<TARGET_FILE:aster-thread-pool-tests>)
        set_tests_properties(thread_pool_cgroup_quotas PROPERTIES TIMEOUT 45 LABELS "parallel;pool;cgroup")
    endif()
    add_test(NAME lexer_96_workers_join COMMAND aster-many-workers-tests lexer)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC" AND CMAKE_GENERATOR MATCHES "Ninja")
        add_test(NAME msvc_ninja_localized_header_dependency
            COMMAND "${CMAKE_COMMAND}" "-DASTER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DASTER_PROBE=${CMAKE_CURRENT_BINARY_DIR}/dependency-probe"
                "-DASTER_COMPILER=${CMAKE_C_COMPILER}" "-DASTER_NINJA=${CMAKE_MAKE_PROGRAM}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestMsvcDependencies.cmake")
        set_tests_properties(msvc_ninja_localized_header_dependency PROPERTIES TIMEOUT 160 RUN_SERIAL TRUE)
    endif()
    target_link_libraries(aster-bind-tests PRIVATE asterasm_backend)
    target_include_directories(aster-bind-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/regalloc")
    target_compile_definitions(aster-bind-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-bind-tests)
    asterasm_enable_sanitizers(aster-bind-tests)
    foreach(_abi RANGE 0 5)
        foreach(_limit 0 2)
            foreach(_promote 0 1)
                set(_case 0)
                foreach(_name abi_role overlapping incompatible invalid address_taken explicit mutation)
                    add_test(NAME bind_${_name}_abi${_abi}_regs${_limit}_promote${_promote}
                        COMMAND aster-bind-tests ${_case} ${_abi} ${_limit} ${_promote})
                    math(EXPR _case "${_case} + 1")
                endforeach()
            endforeach()
        endforeach()
    endforeach()
    target_link_libraries(aster-regalloc-tests PRIVATE asterasm_backend)
    foreach(_abi 0 1)
        foreach(_promote 0 1)
            set(_case 7)
            foreach(_name address attribute effect immediate memory stack callee)
                add_test(NAME native_reject_${_name}_abi${_abi}_promote${_promote}
                    COMMAND aster-bind-tests ${_case} ${_abi} 2 ${_promote})
                math(EXPR _case "${_case} + 1")
            endforeach()
            set(_case 14)
            foreach(_name missing_statement statement_boundary missing_save missing_restore naked_callee
                missing_popcnt_feature stripped_popcnt_feature overwritten_save partial_save_overlap missing_input_restore missing_input_save)
                add_test(NAME native_mutation_${_name}_abi${_abi}_promote${_promote}
                    COMMAND aster-bind-tests ${_case} ${_abi} 2 ${_promote})
                math(EXPR _case "${_case} + 1")
            endforeach()
        endforeach()
    endforeach()
    target_include_directories(aster-regalloc-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/regalloc")
    target_compile_definitions(aster-regalloc-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-regalloc-tests)
    asterasm_enable_sanitizers(aster-regalloc-tests)
    set(ASTERASM_REGALLOC_EXEC_TESTS add subtract multiply bit_mix divide remainder signed_divide shift_left
        shift_right choose loop_sum call_live sum9 call9 pair triple mixed hfa float_add float_const triple_call pair_call mixed_call sum3)
    foreach(_limit 0 2)
        foreach(_promote 0 1)
            set(_case 54)
            foreach(_name argument32 argument64 local32 local64 branch64 loop64)
                add_test(NAME physical_sysv_overaligned_${_name}_regs${_limit}_promote${_promote}
                    COMMAND aster-regalloc-tests ${_case} sysv ${_limit} ${_promote})
                math(EXPR _case "${_case} + 1")
            endforeach()
        endforeach()
    endforeach()
    set(ASTERASM_NATIVE_EXEC_TESTS identity arithmetic multiple_outputs hard_clobber exact_bridge byte_add short_add word_add
        memory_read memory_write population consecutive manual naked)
    set(ASTERASM_SCALAR_EXEC_TESTS logical_not_false logical_not_true divide_i8 divide_i16 divide_u8 remainder_u16 remainder_i8
        extend_signed extend_unsigned truncate16 shift_signed32 compare_signed16 float32_add choose_false divide_i8_overflow min_constant)
    set(ASTERASM_CROSSARCH_EXEC_TESTS ${ASTERASM_REGALLOC_EXEC_TESTS} ${ASTERASM_SCALAR_EXEC_TESTS}
        signed_to_float unsigned_to_double double_to_signed double_to_unsigned widen_float narrow_float
        bits_to_float double_to_bits float_less_nan float_not_equal_nan float_greater_equal_nan
        select_integer_false select_integer_true select_float_false select_float_true float_less_equal float_equal_signed_zero
        float_call_live)
    foreach(_abi sysv win64)
        foreach(_limit 0 2)
            set(_case 0)
            foreach(_name IN LISTS ASTERASM_REGALLOC_EXEC_TESTS)
                add_test(NAME physical_${_abi}_${_name}_regs${_limit} COMMAND aster-regalloc-tests ${_case} ${_abi} ${_limit})
                math(EXPR _case "${_case} + 1")
            endforeach()
            set(_case 40)
            foreach(_name IN LISTS ASTERASM_NATIVE_EXEC_TESTS)
                foreach(_promote 0 1)
                    add_test(NAME native_${_abi}_${_name}_regs${_limit}_promote${_promote}
                        COMMAND aster-regalloc-tests ${_case} ${_abi} ${_limit} ${_promote})
                endforeach()
                math(EXPR _case "${_case} + 1")
            endforeach()
            set(_case 24)
            foreach(_name IN LISTS ASTERASM_SCALAR_EXEC_TESTS)
                foreach(_promote 0 1)
                    add_test(NAME physical_${_abi}_${_name}_regs${_limit}_promote${_promote}
                        COMMAND aster-regalloc-tests ${_case} ${_abi} ${_limit} ${_promote})
                endforeach()
                math(EXPR _case "${_case} + 1")
            endforeach()
        endforeach()
    endforeach()

    add_executable(aster-aarch64-tests tests/test_aarch64_backend.c)
    add_executable(aster-riscv-tests tests/test_riscv_backend.c)
    target_link_libraries(aster-riscv-tests PRIVATE asterasm_backend)
    target_include_directories(aster-riscv-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/regalloc")
    target_compile_definitions(aster-riscv-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-riscv-tests)
    asterasm_enable_sanitizers(aster-riscv-tests)
    foreach(_riscv_abi lp64 lp64f lp64d)
        foreach(_limit 0 2)
            foreach(_promote 0 1)
                set(_case 0)
                foreach(_name IN LISTS ASTERASM_CROSSARCH_EXEC_TESTS)
                    add_test(NAME physical_riscv_${_riscv_abi}_${_name}_regs${_limit}_promote${_promote}
                        COMMAND aster-riscv-tests ${_case} ${_limit} ${_promote} ${_riscv_abi})
                    math(EXPR _case "${_case} + 1")
                endforeach()
            endforeach()
        endforeach()
    endforeach()
    add_executable(aster-regalloc-checks tests/test_regalloc_checks.c)
    add_executable(aster-arch-regalloc-checks tests/test_arch_regalloc_checks.c)
    target_link_libraries(aster-arch-regalloc-checks PRIVATE asterasm_backend)
    target_include_directories(aster-arch-regalloc-checks PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/postra")
    target_compile_definitions(aster-arch-regalloc-checks PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-arch-regalloc-checks)
    asterasm_enable_sanitizers(aster-arch-regalloc-checks)
    foreach(_abi RANGE 0 3)
        foreach(_promote 0 1)
            set(_case 0)
            foreach(_name link_save link_restore link_after_restore link_slot_overwrite link_narrow_restore link_wrong_slot
                call_save call_restore call_slot_overwrite call_after_restore call_source unresolved_operation
                float_cast_bank float_resize_width float_compare_width float_compare_producer float_select_source
                float_to_integer_contract large_borrow_save large_borrow_source large_borrow_restore
                frame_address_8k frame_address_64k missing_sp_compensation overlapping_registers frame_encoding_contract frame_adjust_amount)
                add_test(NAME arch_mutation_${_name}_abi${_abi}_promote${_promote}
                    COMMAND aster-arch-regalloc-checks ${_case} ${_abi} ${_promote})
                math(EXPR _case "${_case} + 1")
            endforeach()
        endforeach()
    endforeach()
    target_link_libraries(aster-regalloc-checks PRIVATE asterasm_backend)
    target_include_directories(aster-regalloc-checks PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/postra" "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/regalloc")
    target_compile_definitions(aster-regalloc-checks PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-regalloc-checks)
    asterasm_enable_sanitizers(aster-regalloc-checks)
    include(cmake/RegallocChecks.cmake)
    set(_case 0)
    foreach(_name IN LISTS ASTERASM_REGALLOC_CHECK_NAMES)
        add_test(NAME regalloc_check_${_name} COMMAND aster-regalloc-checks ${_case})
        math(EXPR _case "${_case} + 1")
    endforeach()
    target_link_libraries(aster-aarch64-tests PRIVATE asterasm_backend)
    target_include_directories(aster-aarch64-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/regalloc")
    target_compile_definitions(aster-aarch64-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-aarch64-tests)
    asterasm_enable_sanitizers(aster-aarch64-tests)
    foreach(_limit 0 2)
        foreach(_promote 0 1)
            set(_case 0)
            foreach(_name IN LISTS ASTERASM_CROSSARCH_EXEC_TESTS)
                add_test(NAME physical_aarch64_${_name}_regs${_limit}_promote${_promote}
                    COMMAND aster-aarch64-tests ${_case} ${_limit} ${_promote})
                math(EXPR _case "${_case} + 1")
            endforeach()
        endforeach()
    endforeach()

    add_executable(aster-regalloc-foundation-tests tests/test_regalloc_foundation.c)
    foreach(_case 9 10 11 13 20 22)
        list(GET ASTERASM_REGALLOC_EXEC_TESTS ${_case} _name)
        foreach(_frame 8192 65536)
            foreach(_abi sysv win64)
                add_test(NAME physical_${_abi}_large${_frame}_${_name}
                    COMMAND aster-regalloc-tests ${_case} ${_abi} 2 0 ${_frame})
            endforeach()
            add_test(NAME physical_aarch64_large${_frame}_${_name}
                COMMAND aster-aarch64-tests ${_case} 2 0 ${_frame})
            foreach(_abi lp64 lp64f lp64d)
                add_test(NAME physical_riscv_${_abi}_large${_frame}_${_name}
                    COMMAND aster-riscv-tests ${_case} 2 0 ${_abi} ${_frame})
            endforeach()
        endforeach()
    endforeach()
    target_link_libraries(aster-regalloc-foundation-tests PRIVATE asterasm_backend)
    target_include_directories(aster-regalloc-foundation-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/common"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/regalloc")
    asterasm_enable_warnings(aster-regalloc-foundation-tests)
    asterasm_enable_sanitizers(aster-regalloc-foundation-tests)
    set(ASTERASM_REGALLOC_FOUNDATION_TEST_NAMES
        physreg_x86_aliases physreg_a64_aliases physreg_riscv_aliases liveness_cfg liveness_mutation
    )
    set(_aster_regalloc_foundation_test_index 0)
    foreach(_aster_regalloc_foundation_test IN LISTS ASTERASM_REGALLOC_FOUNDATION_TEST_NAMES)
        add_test(NAME regalloc_${_aster_regalloc_foundation_test}
            COMMAND aster-regalloc-foundation-tests ${_aster_regalloc_foundation_test_index})
        math(EXPR _aster_regalloc_foundation_test_index "${_aster_regalloc_foundation_test_index} + 1")
    endforeach()

    add_executable(aster-memory-order-tests tests/test_memory_order.c)
    target_link_libraries(aster-memory-order-tests PRIVATE asterasm_backend)
    target_include_directories(aster-memory-order-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/common")
    target_compile_definitions(aster-memory-order-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-memory-order-tests)
    asterasm_enable_sanitizers(aster-memory-order-tests)
    set(ASTERASM_MEMORY_ORDER_TEST_NAMES hir_atomic mir_atomic machine_atomic verify_mutation fence_pipeline dump)
    foreach(_atomic_arch x86 a64 rv64)
        foreach(_mutation order alias width missing duplicate cas_failure)
            list(APPEND ASTERASM_MEMORY_ORDER_TEST_NAMES physical_${_atomic_arch}_${_mutation})
        endforeach()
    endforeach()
    set(_aster_memory_order_test_index 0)
    foreach(_aster_memory_order_test IN LISTS ASTERASM_MEMORY_ORDER_TEST_NAMES)
        add_test(NAME memory_order_${_aster_memory_order_test} COMMAND aster-memory-order-tests ${_aster_memory_order_test_index})
        math(EXPR _aster_memory_order_test_index "${_aster_memory_order_test_index} + 1")
    endforeach()

    add_executable(aster-abi-tests tests/test_abi.c)
    target_link_libraries(aster-abi-tests PRIVATE asterasm_abi)
    target_include_directories(aster-abi-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/abi")
    target_compile_definitions(aster-abi-tests PRIVATE ASTERASM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    asterasm_enable_warnings(aster-abi-tests)
    asterasm_enable_sanitizers(aster-abi-tests)
    set(ASTERASM_ABI_TEST_NAMES
        sysv_many sysv_i128_return sysv_pair_regs sysv_sret win_shadow_positions
        win_pair_indirect win_sret a64_no_partial_split a64_hfa a64_overaligned_hfa
        a64_sret_masks riscv_i128_split riscv_nanbox riscv_sret callsite_vreg_identity
        sysv_small_int_ext sysv_fp_banks sysv_contract_masks win_small_positions win_stack_shadow
        win_callee_saved a64_pair_result a64_i128_even_pair a64_hfa_result a64_hfa_exhaustion
        riscv_mixed_fp_int riscv_hfa2_fp_regs abi_verify_shadow_mutation abi_verify_location_mutation
        dump_determinism a64_sve_direct a64_sve_mixed a64_sve_predicate
        riscv_vector_direct riscv_vector_predicate vector_pcs_fail_closed
        i386_stack_args i386_small_int_return i386_float_x87 i386_sret x86_16_cdecl i386_target_mismatch
    )
    set(_aster_abi_test_index 0)
    foreach(_aster_abi_test IN LISTS ASTERASM_ABI_TEST_NAMES)
        add_test(NAME abi_${_aster_abi_test} COMMAND aster-abi-tests ${_aster_abi_test_index})
        math(EXPR _aster_abi_test_index "${_aster_abi_test_index} + 1")
    endforeach()

endif()


include(cmake/Fuzzing.cmake)

install(DIRECTORY include/asterasm DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES include/aster_lexer.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS asterasm_core asterasm_lexer asterasm_ast asterasm_parser asterasm_frontend asterasm_sema asterasm_hir asterasm_mir asterasm_target asterasm_abi asterasm_backend
    EXPORT AsterASMTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
if(TARGET asterasm_shared)
    install(TARGETS asterasm_shared EXPORT AsterASMTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()
if(ASTERASM_BUILD_TOOLS)
    install(TARGETS asterc asterdump aster-lex aster-ast aster-hir aster-mir RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/AsterASMConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
configure_package_config_file(
    cmake/AsterASMConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/AsterASMConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/AsterASM"
    PATH_VARS CMAKE_INSTALL_BINDIR CMAKE_INSTALL_LIBDIR CMAKE_INSTALL_INCLUDEDIR
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/AsterASMConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/AsterASMConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/AsterASM"
)
install(EXPORT AsterASMTargets
    NAMESPACE AsterASM::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/AsterASM"
)
install(FILES README.md ROADMAP.md BUILDING.md
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/AsterASM")
install(FILES LICENSE NOTICE
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/AsterASM")
install(DIRECTORY LICENSES
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/AsterASM")
