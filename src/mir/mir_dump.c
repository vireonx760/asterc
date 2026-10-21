#include "mir_internal.h"

typedef struct MirDump {
  void (*write_fn)(void *user, const char *text, size_t length);
  void *user;
} MirDump;

static void mir_dump_write(MirDump *dump, const char *text, size_t length) {
  if (dump && dump->write_fn && text && length)
    dump->write_fn(dump->user, text, length);
}

static void mir_dump_text(MirDump *dump, const char *text) {
  if (text)
    mir_dump_write(dump, text, strlen(text));
}

static void mir_dump_printf(MirDump *dump, const char *fmt, ...)
    ASTER_PRINTF_LIKE(2, 3);

static void mir_dump_printf(MirDump *dump, const char *fmt, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, fmt);
  va_list copy;
  va_copy(copy, args);
  int needed = vsnprintf(buffer, sizeof(buffer), fmt, copy);
  va_end(copy);
  if (needed < 0) {
    va_end(args);
    return;
  }
  if ((size_t)needed < sizeof(buffer)) {
    mir_dump_write(dump, buffer, (size_t)needed);
    va_end(args);
    return;
  }
  size_t size = (size_t)needed + 1u;
  char *large = (char *)malloc(size);
  if (large && vsnprintf(large, size, fmt, args) >= 0)
    mir_dump_write(dump, large, (size_t)needed);
  free(large);
  va_end(args);
}

static void mir_dump_name(MirDump *dump, AstName name) {
  if (!name.text)
    mir_dump_text(dump, "<none>");
  else
    mir_dump_printf(dump, "%.*s", (int)name.span.length, name.text);
}

static void mir_dump_quoted(MirDump *dump, AstHirText text) {
  mir_dump_text(dump, "\"");
  for (uint32_t i = 0u; i < text.length; ++i) {
    unsigned char c = (unsigned char)text.data[i];
    if (c == '\\' || c == '"')
      mir_dump_printf(dump, "\\%c", (int)c);
    else if (c == '\n')
      mir_dump_text(dump, "\\n");
    else if (c == '\r')
      mir_dump_text(dump, "\\r");
    else if (c == '\t')
      mir_dump_text(dump, "\\t");
    else if (c >= 0x20u && c < 0x7fu)
      mir_dump_printf(dump, "%c", (int)c);
    else
      mir_dump_printf(dump, "\\x%02x", (unsigned)c);
  }
  mir_dump_text(dump, "\"");
}

static void mir_dump_type(MirDump *dump, const AstSemaType *type,
                          uint32_t depth) {
  if (!type) {
    mir_dump_text(dump, "<null>");
    return;
  }
  if (depth > 32u) {
    mir_dump_text(dump, "...");
    return;
  }
  AstSemaTypeKind kind = ast_sema_type_kind(type);
  switch (kind) {
  case AST_SEMA_TYPE_VOID:
    mir_dump_text(dump, "void");
    break;
  case AST_SEMA_TYPE_BOOL:
    mir_dump_text(dump, "bool");
    break;
  case AST_SEMA_TYPE_INT:
    mir_dump_printf(dump, "%c%u", ast_sema_type_is_signed(type) ? 'i' : 'u',
                    ast_sema_type_bit_width(type));
    break;
  case AST_SEMA_TYPE_FLOAT:
    mir_dump_printf(dump, "f%u", ast_sema_type_bit_width(type));
    break;
  case AST_SEMA_TYPE_BITS: {
    uint64_t width = 0u;
    if (ast_sema_type_bits_width(type, &width))
      mir_dump_printf(dump, "bits<%llu>", (unsigned long long)width);
    else
      mir_dump_text(dump, "bits<?>");
    break;
  }
  case AST_SEMA_TYPE_PTR:
  case AST_SEMA_TYPE_CAP:
    mir_dump_text(dump, kind == AST_SEMA_TYPE_PTR ? "ptr<" : "cap<");
    if (ast_sema_type_pointer_qualifiers(type) & AST_TYPE_QUAL_CONST)
      mir_dump_text(dump, "const ");
    if (ast_sema_type_pointer_qualifiers(type) & AST_TYPE_QUAL_VOLATILE)
      mir_dump_text(dump, "volatile ");
    mir_dump_type(dump, ast_sema_type_element(type), depth + 1u);
    mir_dump_text(dump, ">");
    break;
  case AST_SEMA_TYPE_VECTOR: {
    uint64_t lanes = 0u;
    mir_dump_text(dump, "vec<");
    if (ast_sema_type_vector_lanes(type, &lanes))
      mir_dump_printf(dump, "%llu, ", (unsigned long long)lanes);
    else
      mir_dump_text(dump, "?, ");
    mir_dump_type(dump, ast_sema_type_element(type), depth + 1u);
    mir_dump_text(dump, ">");
    break;
  }
  case AST_SEMA_TYPE_MASK: {
    uint64_t lanes = 0u;
    if (ast_sema_type_vector_lanes(type, &lanes))
      mir_dump_printf(dump, "mask<%llu>", (unsigned long long)lanes);
    else
      mir_dump_text(dump, "mask<?>");
    break;
  }
  case AST_SEMA_TYPE_SVECTOR:
  case AST_SEMA_TYPE_PRED:
    mir_dump_text(dump, kind == AST_SEMA_TYPE_SVECTOR ? "svec<" : "pred<");
    mir_dump_type(dump, ast_sema_type_element(type), depth + 1u);
    mir_dump_text(dump, ">");
    break;
  case AST_SEMA_TYPE_ARRAY: {
    uint64_t count = 0u;
    mir_dump_text(dump, "[");
    mir_dump_type(dump, ast_sema_type_element(type), depth + 1u);
    if (ast_sema_type_array_count(type, &count))
      mir_dump_printf(dump, "; %llu]", (unsigned long long)count);
    else
      mir_dump_text(dump, "; ?]");
    break;
  }
  case AST_SEMA_TYPE_TUPLE: {
    mir_dump_text(dump, "(");
    uint32_t count = ast_sema_type_tuple_count(type);
    for (uint32_t i = 0u; i < count; ++i) {
      if (i)
        mir_dump_text(dump, ", ");
      mir_dump_type(dump, ast_sema_type_tuple_item(type, i), depth + 1u);
    }
    mir_dump_text(dump, ")");
    break;
  }
  case AST_SEMA_TYPE_STRUCT:
    mir_dump_text(dump, "struct ");
    mir_dump_name(dump, ast_sema_type_struct_name(type));
    break;
  case AST_SEMA_TYPE_FUNCTION: {
    mir_dump_text(dump, "fn(");
    uint32_t count = ast_sema_type_function_param_count(type);
    for (uint32_t i = 0u; i < count; ++i) {
      if (i)
        mir_dump_text(dump, ", ");
      mir_dump_type(dump, ast_sema_type_function_param(type, i), depth + 1u);
    }
    mir_dump_text(dump, ") -> ");
    mir_dump_type(dump, ast_sema_type_function_result(type), depth + 1u);
    break;
  }
  case AST_SEMA_TYPE_TYPE_PARAMETER:
    mir_dump_text(dump, "type_param");
    break;
  case AST_SEMA_TYPE_STRING:
    mir_dump_text(dump, "string");
    break;
  case AST_SEMA_TYPE_BYTE_STRING:
    mir_dump_text(dump, "byte_string");
    break;
  case AST_SEMA_TYPE_C_STRING:
    mir_dump_text(dump, "c_string");
    break;
  case AST_SEMA_TYPE_UNTYPED_INT:
    mir_dump_text(dump, "untyped_int");
    break;
  case AST_SEMA_TYPE_UNTYPED_FLOAT:
    mir_dump_text(dump, "untyped_float");
    break;
  case AST_SEMA_TYPE_ERROR:
  default:
    mir_dump_text(dump, "<error>");
    break;
  }
}

static void mir_dump_constant(MirDump *dump, AstSemaConstInfo constant) {
  switch (constant.kind) {
  case AST_SEMA_CONST_INTEGER:
    if (constant.hi)
      mir_dump_printf(dump, "0x%llx%016llx", (unsigned long long)constant.hi,
                      (unsigned long long)constant.lo);
    else if (constant.is_negative)
      mir_dump_printf(dump, "-%llu", (unsigned long long)constant.lo);
    else
      mir_dump_printf(dump, "%llu", (unsigned long long)constant.lo);
    break;
  case AST_SEMA_CONST_FLOAT:
    mir_dump_printf(dump, "%.17g", constant.floating);
    break;
  case AST_SEMA_CONST_BOOL:
    mir_dump_text(dump, constant.lo ? "true" : "false");
    break;
  case AST_SEMA_CONST_NONE:
  default:
    mir_dump_text(dump, "?");
    break;
  }
}

static void mir_dump_effects(MirDump *dump, uint32_t flags) {
  static const struct {
    uint32_t bit;
    const char *name;
  } names[] = {{AST_MIR_INST_READS_MEMORY, "read"},
               {AST_MIR_INST_WRITES_MEMORY, "write"},
               {AST_MIR_INST_ATOMIC, "atomic"},
               {AST_MIR_INST_CALL, "call"},
               {AST_MIR_INST_MAY_TRAP, "trap"},
               {AST_MIR_INST_VOLATILE, "volatile"},
               {AST_MIR_INST_UNSAFE, "unsafe"},
               {AST_MIR_INST_NONTEMPORAL, "nontemporal"},
               {AST_MIR_INST_CONSTRAINT, "constraint"},
               {AST_MIR_INST_CONSERVATIVE_EFFECTS, "conservative"},
               {AST_MIR_INST_SYNTHETIC, "synthetic"}};
  if (!flags)
    return;
  mir_dump_text(dump, " effects[");
  int first = 1;
  for (size_t i = 0u; i < sizeof(names) / sizeof(names[0]); ++i) {
    if (!(flags & names[i].bit))
      continue;
    if (!first)
      mir_dump_text(dump, ",");
    mir_dump_text(dump, names[i].name);
    first = 0;
  }
  mir_dump_text(dump, "]");
}

static void mir_dump_vreg_list(MirDump *dump, const AstMirVReg *values,
                               uint32_t count) {
  mir_dump_text(dump, "(");
  for (uint32_t i = 0u; i < count; ++i) {
    if (i)
      mir_dump_text(dump, ", ");
    mir_dump_printf(dump, "%%r%u", values[i]);
  }
  mir_dump_text(dump, ")");
}

static void mir_dump_instruction(MirDump *dump, const AstMirInstruction *inst) {
  mir_dump_text(dump, "    ");
  if (inst->info.result != AST_MIR_INVALID_ID)
    mir_dump_printf(dump, "%%r%u = ", inst->info.result);
  mir_dump_text(dump, ast_mir_op_name(inst->info.op));
  if (inst->info.op == AST_MIR_OP_CONSTANT) {
    mir_dump_text(dump, " ");
    mir_dump_constant(dump, inst->info.constant);
  }
  if (inst->info.op == AST_MIR_OP_UNARY || inst->info.op == AST_MIR_OP_BINARY ||
      inst->info.op == AST_MIR_OP_CAST)
    mir_dump_printf(dump, " [%s]", ast_token_kind_name(inst->info.token_op));
  if (inst->info.op == AST_MIR_OP_INTRINSIC)
    mir_dump_printf(dump, " [%s]",
                    ast_hir_intrinsic_name(inst->info.intrinsic));
  if (inst->info.op == AST_MIR_OP_ATOMIC) {
    mir_dump_printf(dump, " order[%s",
                    ast_memory_order_name(inst->info.memory_order));
    if (inst->info.failure_memory_order != AST_MEMORY_ORDER_NONE)
      mir_dump_printf(dump, ",fail=%s",
                      ast_memory_order_name(inst->info.failure_memory_order));
    mir_dump_text(dump, "]");
  } else if (inst->info.op == AST_MIR_OP_FENCE) {
    mir_dump_printf(dump, " order[%s]",
                    ast_memory_order_name(inst->info.memory_order));
  }
  if (inst->info.stack_slot != AST_MIR_INVALID_ID)
    mir_dump_printf(dump, " @s%u", inst->info.stack_slot);
  if (inst->info.symbol.span.length) {
    mir_dump_text(dump, " @");
    if (inst->info.module.span.length) {
      mir_dump_name(dump, inst->info.module);
      mir_dump_text(dump, ".");
    }
    mir_dump_name(dump, inst->info.symbol);
  }
  if (inst->info.op == AST_MIR_OP_NATIVE ||
      inst->info.op == AST_MIR_OP_NATIVE_RESULT)
    mir_dump_printf(dump, " @nr%u", inst->info.native_region);
  if (inst->info.op == AST_MIR_OP_NATIVE_RESULT)
    mir_dump_printf(dump, ".%u", inst->info.aux_index);
  if (inst->info.operand_count) {
    mir_dump_text(dump, " ");
    mir_dump_vreg_list(dump, inst->operands, inst->info.operand_count);
  }
  if (inst->info.result != AST_MIR_INVALID_ID && inst->info.type) {
    mir_dump_text(dump, " : ");
    mir_dump_type(dump, inst->info.type, 0u);
    mir_dump_printf(dump, " <%s%u>",
                    ast_mir_reg_bank_name(mir_type_bank(inst->info.type)),
                    mir_type_bit_width(inst->info.type));
  }
  if (inst->info.memory_in != AST_MIR_INVALID_ID)
    mir_dump_printf(dump, " [!m%u -> !m%u]", inst->info.memory_in,
                    inst->info.memory_out);
  mir_dump_effects(dump, inst->info.flags);
  if (inst->info.inline_site)
    mir_dump_printf(dump, " inline.site%u", inst->info.inline_site);
  if (inst->info.source_hir_instruction != AST_HIR_INVALID_ID)
    mir_dump_printf(dump, " ; hir.i%u", inst->info.source_hir_instruction);
  mir_dump_text(dump, "\n");
}

static void mir_dump_live_set(MirDump *dump, const AstMirFunction *fn,
                              AstMirBlockId block, int out) {
  mir_dump_text(dump, out ? "    ; liveout" : "    ; livein");
  int first = 1;
  for (uint32_t vreg = 0u; vreg < fn->vreg_count; ++vreg) {
    int live = out ? ast_mir_function_is_live_out(fn, block, vreg)
                   : ast_mir_function_is_live_in(fn, block, vreg);
    if (!live)
      continue;
    mir_dump_printf(dump, "%s%%r%u", first ? " " : ", ", vreg);
    first = 0;
  }
  if (first)
    mir_dump_text(dump, " -");
  mir_dump_text(dump, "\n");
}

static void mir_dump_terminator(MirDump *dump, const AstMirTerminator *term) {
  mir_dump_text(dump, "    ");
  switch (term->info.kind) {
  case AST_MIR_TERM_BR:
    mir_dump_printf(dump, "br ^bb%u", term->info.target_true);
    if (term->info.true_arg_count) {
      mir_dump_text(dump, " ");
      mir_dump_vreg_list(dump, term->true_args, term->info.true_arg_count);
    }
    break;
  case AST_MIR_TERM_COND_BR:
    mir_dump_printf(dump, "cond_br %%r%u, ^bb%u", term->info.condition_or_value,
                    term->info.target_true);
    if (term->info.true_arg_count) {
      mir_dump_text(dump, " ");
      mir_dump_vreg_list(dump, term->true_args, term->info.true_arg_count);
    }
    mir_dump_printf(dump, ", ^bb%u", term->info.target_false);
    if (term->info.false_arg_count) {
      mir_dump_text(dump, " ");
      mir_dump_vreg_list(dump, term->false_args, term->info.false_arg_count);
    }
    break;
  case AST_MIR_TERM_RETURN:
    mir_dump_text(dump, "return");
    if (term->info.condition_or_value != AST_MIR_INVALID_ID)
      mir_dump_printf(dump, " %%r%u", term->info.condition_or_value);
    break;
  case AST_MIR_TERM_UNREACHABLE:
    mir_dump_text(dump, "unreachable");
    break;
  case AST_MIR_TERM_NONE:
  default:
    mir_dump_text(dump, "<missing terminator>");
    break;
  }
  mir_dump_printf(dump, " [!m%u]\n", term->info.memory);
}

static void mir_dump_function_impl(MirDump *dump, const AstMirFunction *fn) {
  mir_dump_text(dump, "mir.func @");
  mir_dump_name(dump, fn->module_name);
  mir_dump_text(dump, ".");
  mir_dump_name(dump, fn->name);
  mir_dump_text(dump, " : ");
  mir_dump_type(dump, fn->type, 0u);
  if (fn->flags & AST_HIR_FUNCTION_EXTERN)
    mir_dump_text(dump, " [extern]");
  if (fn->flags & AST_HIR_FUNCTION_DISPATCH)
    mir_dump_text(dump, " [dispatch]");
  if (fn->flags & AST_HIR_FUNCTION_EXPORT)
    mir_dump_text(dump, " [export]");
  if (fn->flags & AST_HIR_FUNCTION_GENERIC)
    mir_dump_text(dump, " [generic]");
  if (fn->flags & AST_HIR_FUNCTION_NAKED)
    mir_dump_text(dump, " [naked]");
  if (fn->flags & AST_HIR_FUNCTION_LEAF)
    mir_dump_text(dump, " [leaf]");
  if (fn->flags & AST_HIR_FUNCTION_INLINE)
    mir_dump_text(dump, " [inline]");
  if (fn->flags & AST_HIR_FUNCTION_NO_INLINE)
    mir_dump_text(dump, " [noinline]");
  if (fn->abi.is_present) {
    mir_dump_text(dump, " [abi=");
    mir_dump_quoted(dump, fn->abi.text);
    mir_dump_text(dump, "]");
  }
  mir_dump_printf(dump, " [promoted=%u slots=%u phis=%u memory_phis=%u] {\n",
                  fn->promoted_local_count, fn->stack_slot_count, fn->phi_count,
                  fn->memory_phi_count);
  for (uint32_t s = 0; s < fn->inline_site_count; ++s) {
    const AstMirInlineSiteInfo *site = &fn->inline_sites[s];
    mir_dump_printf(dump, "  inline.site%u parent=%u callee=@", s + 1u,
                    site->parent);
    mir_dump_name(dump, site->callee_module);
    mir_dump_text(dump, ".");
    mir_dump_name(dump, site->callee_name);
    mir_dump_printf(dump, " call.file=%u offset=%llu\n",
                    site->call_span.file_id,
                    (unsigned long long)site->call_span.offset);
  }
  for (uint32_t si = 0u; si < fn->stack_slot_count; ++si) {
    const AstMirStackSlotInfo *slot = &fn->stack_slots[si].info;
    mir_dump_printf(dump, "  stack @s%u @l%u ", si, slot->source_local);
    mir_dump_name(dump, slot->name);
    mir_dump_text(dump, " : ");
    mir_dump_type(dump, slot->type, 0u);
    if (slot->size == AST_MIR_UNKNOWN_SIZE)
      mir_dump_text(dump, " size=?");
    else
      mir_dump_printf(dump, " size=%llu", (unsigned long long)slot->size);
    mir_dump_printf(dump, " align=%u flags=0x%x\n", slot->alignment,
                    slot->flags);
  }
  for (uint32_t bi = 0u; bi < fn->block_count; ++bi) {
    const AstMirBlock *block = &fn->blocks[bi];
    mir_dump_printf(dump, "  ^bb%u(", bi);
    for (uint32_t ai = 0u; ai < block->argument_count; ++ai) {
      if (ai)
        mir_dump_text(dump, ", ");
      AstMirVReg vreg = block->arguments[ai];
      mir_dump_printf(dump, "%%r%u", vreg);
      if (block->argument_source_locals[ai] != AST_HIR_INVALID_ID)
        mir_dump_printf(dump, "@l%u", block->argument_source_locals[ai]);
      mir_dump_text(dump, ": ");
      if (vreg < fn->vreg_count)
        mir_dump_type(dump, fn->vregs[vreg].info.type, 0u);
    }
    mir_dump_printf(dump, "; !m%u", block->memory_argument);
    const AstMirMemoryInfo *memory =
        &fn->memory_values[block->memory_argument].info;
    if (bi == fn->entry_block) {
      mir_dump_text(dump, "=live_on_entry");
    } else {
      mir_dump_text(dump, block->predecessor_count > 1u ? "=memory_phi("
                                                        : "=memory_arg(");
      for (uint32_t pi = 0u; pi < memory->incoming_count; ++pi) {
        if (pi)
          mir_dump_text(dump, ", ");
        mir_dump_printf(dump, "^bb%u:!m%u", block->predecessors[pi],
                        memory->incoming[pi]);
      }
      mir_dump_text(dump, ")");
    }
    mir_dump_printf(
        dump, ") ; preds=%u succs=%u rpo=%u idom=", block->predecessor_count,
        block->successor_count, block->reverse_postorder);
    if (block->immediate_dominator == AST_MIR_INVALID_ID)
      mir_dump_text(dump, "-");
    else
      mir_dump_printf(dump, "^bb%u", block->immediate_dominator);
    mir_dump_printf(dump, " loop=%u reachable=%u\n", block->loop_depth,
                    (unsigned)block->reachable);
    mir_dump_live_set(dump, fn, bi, 0);
    for (uint32_t ii = 0u; ii < block->instruction_count; ++ii) {
      AstMirInstId id = block->instructions[ii];
      if (id < fn->instruction_count)
        mir_dump_instruction(dump, &fn->instructions[id]);
    }
    mir_dump_terminator(dump, &block->term);
    mir_dump_live_set(dump, fn, bi, 1);
  }
  mir_dump_text(dump, "}\n");
}

void ast_mir_dump_function(const AstMirFunction *function,
                           void (*write_fn)(void *user, const char *text,
                                            size_t length),
                           void *user) {
  if (!function || !write_fn)
    return;
  MirDump dump = {write_fn, user};
  mir_dump_function_impl(&dump, function);
}

void ast_mir_dump_program(const AstMirProgram *program,
                          void (*write_fn)(void *user, const char *text,
                                           size_t length),
                          void *user) {
  if (!program || !write_fn)
    return;
  MirDump dump = {write_fn, user};
  mir_dump_text(&dump, "mir.program {\n");
  if (program->hir) {
    for (size_t i = 0u; i < ast_hir_module_count(program->hir); ++i) {
      AstHirModuleInfo module;
      if (!ast_hir_module_at(program->hir, i, &module))
        continue;
      mir_dump_text(&dump, "  module @");
      mir_dump_name(&dump, module.name);
      mir_dump_text(&dump, "\n");
    }
  }
  for (size_t i = 0u; i < program->function_count; ++i)
    if (program->functions[i])
      mir_dump_function_impl(&dump, program->functions[i]);
  mir_dump_text(&dump, "}\n");
}
