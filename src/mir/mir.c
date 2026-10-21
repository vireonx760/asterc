#include "mir_internal.h"

#include <limits.h>

int mir_reserve(void **ptr, size_t elem_size, size_t *capacity, size_t needed) {
  if (!ptr || !capacity || elem_size == 0u)
    return 0;
  if (needed <= *capacity)
    return 1;
  size_t cap = *capacity ? *capacity : 8u;
  while (cap < needed) {
    if (cap > SIZE_MAX / 2u)
      return 0;
    cap *= 2u;
  }
  size_t bytes = 0u;
  if (ast_size_mul_overflow(cap, elem_size, &bytes))
    return 0;
  void *next = realloc(*ptr, bytes);
  if (!next)
    return 0;
  *ptr = next;
  *capacity = cap;
  return 1;
}

void *mir_malloc_array(size_t count, size_t element_size) {
  size_t bytes = 0u;
  if (element_size == 0u ||
      ast_size_mul_overflow(count ? count : 1u, element_size, &bytes))
    return NULL;
  return malloc(bytes);
}

AstName mir_copy_name(AstArena *arena, AstName name) {
  AstName out = name;
  out.text = NULL;
  if (!name.text && name.span.length != 0u)
    return out;
  out.text =
      ast_arena_strndup(arena, name.text ? name.text : "", name.span.length);
  return out;
}

AstHirText mir_copy_text(AstArena *arena, AstHirText text) {
  AstHirText out;
  out.length = text.length;
  out.data = NULL;
  if (!text.data && text.length != 0u)
    return out;
  out.data = ast_arena_strndup(arena, text.data ? text.data : "", text.length);
  return out;
}

AstHirTextValue mir_copy_text_value(AstArena *arena, AstHirTextValue value) {
  AstHirTextValue out = value;
  if (value.is_present)
    out.text = mir_copy_text(arena, value.text);
  return out;
}

int mir_name_eq_cstr(AstName name, const char *text) {
  if (!name.text || !text)
    return 0;
  size_t length = strlen(text);
  return length == name.span.length && memcmp(name.text, text, length) == 0;
}

AstMirRegBank mir_type_bank(const AstSemaType *type) {
  switch (ast_sema_type_kind(type)) {
  case AST_SEMA_TYPE_BOOL:
  case AST_SEMA_TYPE_INT:
  case AST_SEMA_TYPE_BITS:
    return AST_MIR_BANK_GENERAL;
  case AST_SEMA_TYPE_FLOAT:
    return AST_MIR_BANK_FLOAT;
  case AST_SEMA_TYPE_PTR:
  case AST_SEMA_TYPE_FUNCTION:
  case AST_SEMA_TYPE_STRING:
  case AST_SEMA_TYPE_BYTE_STRING:
  case AST_SEMA_TYPE_C_STRING:
    return AST_MIR_BANK_POINTER;
  case AST_SEMA_TYPE_CAP:
    return AST_MIR_BANK_CAPABILITY;
  case AST_SEMA_TYPE_VECTOR:
  case AST_SEMA_TYPE_SVECTOR:
    return AST_MIR_BANK_VECTOR;
  case AST_SEMA_TYPE_MASK:
  case AST_SEMA_TYPE_PRED:
    return AST_MIR_BANK_MASK;
  case AST_SEMA_TYPE_ARRAY:
  case AST_SEMA_TYPE_TUPLE:
  case AST_SEMA_TYPE_STRUCT:
    return AST_MIR_BANK_AGGREGATE;
  case AST_SEMA_TYPE_TYPE_PARAMETER:
    return AST_MIR_BANK_GENERIC;
  case AST_SEMA_TYPE_ERROR:
  case AST_SEMA_TYPE_VOID:
  case AST_SEMA_TYPE_UNTYPED_INT:
  case AST_SEMA_TYPE_UNTYPED_FLOAT:
  default:
    return AST_MIR_BANK_INVALID;
  }
}

uint32_t mir_type_bit_width(const AstSemaType *type) {
  uint32_t bits = ast_sema_type_bit_width(type);
  if (bits != 0u)
    return bits;
  uint64_t size = ast_sema_type_size(type);
  if (size == AST_MIR_UNKNOWN_SIZE || size > UINT32_MAX / 8u)
    return 0u;
  return (uint32_t)(size * 8u);
}

int mir_type_equal(const AstSemaType *a, const AstSemaType *b) {
  return hir_type_equal(a, b);
}

uint32_t mir_instruction_flags(uint32_t effects, AstMirOp op) {
  uint32_t flags = 0u;
  if (effects & AST_SEMA_EFFECT_READ_MEMORY)
    flags |= AST_MIR_INST_READS_MEMORY;
  if (effects & AST_SEMA_EFFECT_WRITE_MEMORY)
    flags |= AST_MIR_INST_WRITES_MEMORY;
  if (effects & AST_SEMA_EFFECT_ATOMIC)
    flags |= AST_MIR_INST_ATOMIC;
  if (effects & AST_SEMA_EFFECT_CALL)
    flags |= AST_MIR_INST_CALL;
  if (effects & AST_SEMA_EFFECT_MAY_TRAP)
    flags |= AST_MIR_INST_MAY_TRAP;
  if (effects & AST_SEMA_EFFECT_VOLATILE)
    flags |= AST_MIR_INST_VOLATILE;
  if (effects & AST_SEMA_EFFECT_UNSAFE)
    flags |= AST_MIR_INST_UNSAFE;
  if (effects & AST_SEMA_EFFECT_NONTEMPORAL)
    flags |= AST_MIR_INST_NONTEMPORAL;
  if (op == AST_MIR_OP_BIND || op == AST_MIR_OP_NATIVE ||
      op == AST_MIR_OP_NATIVE_RESULT)
    flags |= AST_MIR_INST_CONSTRAINT;
  if (op == AST_MIR_OP_NATIVE)
    flags |= AST_MIR_INST_CONSERVATIVE_EFFECTS;
  return flags;
}

int mir_instruction_is_effectful(const AstMirInstruction *inst) {
  const uint32_t ordered_effects =
      AST_SEMA_EFFECT_READ_MEMORY | AST_SEMA_EFFECT_WRITE_MEMORY |
      AST_SEMA_EFFECT_ATOMIC | AST_SEMA_EFFECT_CALL | AST_SEMA_EFFECT_MAY_TRAP |
      AST_SEMA_EFFECT_VOLATILE;
  return inst && ((inst->info.effects & ordered_effects) != 0u ||
                  inst->info.op == AST_MIR_OP_STACK_LOAD ||
                  inst->info.op == AST_MIR_OP_STACK_STORE ||
                  inst->info.op == AST_MIR_OP_NATIVE);
}

static char *mir_format_message(const char *fmt, va_list args)
    ASTER_PRINTF_LIKE(1, 0);

static char *mir_format_message(const char *fmt, va_list args) {
  va_list copy;
  va_copy(copy, args);
  int needed = vsnprintf(NULL, 0u, fmt, copy);
  va_end(copy);
  if (needed < 0)
    return NULL;
  size_t size = (size_t)needed + 1u;
  char *message = (char *)malloc(size);
  if (!message)
    return NULL;
  if (vsnprintf(message, size, fmt, args) < 0) {
    free(message);
    return NULL;
  }
  return message;
}

static void mir_function_diag_v(AstMirFunction *fn, AstMirDiagCode code,
                                AstDiagSeverity severity, AstSpan span,
                                const char *fmt, va_list args)
    ASTER_PRINTF_LIKE(5, 0);

static void mir_function_diag_v(AstMirFunction *fn, AstMirDiagCode code,
                                AstDiagSeverity severity, AstSpan span,
                                const char *fmt, va_list args) {
  if (!fn || !fmt)
    return;
  if (fn->diagnostic_count >= SIZE_MAX - 1u) {
    fn->oom = 1u;
    return;
  }
  size_t capacity = fn->diagnostic_capacity;
  if (!mir_reserve((void **)&fn->diagnostics, sizeof(*fn->diagnostics),
                   &capacity, fn->diagnostic_count + 1u)) {
    fn->oom = 1u;
    return;
  }
  fn->diagnostic_capacity = capacity;
  char *message = mir_format_message(fmt, args);
  if (!message) {
    fn->oom = 1u;
    return;
  }
  AstMirDiagnostic *diag = &fn->diagnostics[fn->diagnostic_count++];
  memset(diag, 0, sizeof(*diag));
  diag->code = code;
  diag->severity = severity;
  diag->span = span;
  diag->message = message;
}

void mir_function_diag(AstMirFunction *fn, AstMirDiagCode code,
                       AstDiagSeverity severity, AstSpan span, const char *fmt,
                       ...) {
  va_list args;
  va_start(args, fmt);
  mir_function_diag_v(fn, code, severity, span, fmt, args);
  va_end(args);
}

static void mir_resolve_location(const AstFrontend *frontend,
                                 AstMirDiagnostic *diag) {
  if (!frontend || !diag || diag->span.file_id == 0u)
    return;
  AstFrontendFile file;
  memset(&file, 0, sizeof(file));
  if (!ast_frontend_get_file(frontend, (size_t)diag->span.file_id - 1u, &file))
    return;
  diag->path = file.path;
  diag->line = 1u;
  diag->column = 1u;
  if (!file.source || !file.source->data)
    return;
  size_t limit = diag->span.offset > file.source->size
                     ? file.source->size
                     : (size_t)diag->span.offset;
  for (size_t i = 0u; i < limit; ++i) {
    unsigned char c = file.source->data[i];
    if (c == '\n') {
      diag->line++;
      diag->column = 1u;
    } else if (c == '\r') {
      if (i + 1u < limit && file.source->data[i + 1u] == '\n')
        i++;
      diag->line++;
      diag->column = 1u;
    } else {
      diag->column++;
    }
  }
}

static void mir_program_diag_v(AstMirProgram *program, AstMirDiagCode code,
                               AstDiagSeverity severity, AstSpan span,
                               const char *fmt, va_list args)
    ASTER_PRINTF_LIKE(5, 0);

static void mir_program_diag_v(AstMirProgram *program, AstMirDiagCode code,
                               AstDiagSeverity severity, AstSpan span,
                               const char *fmt, va_list args) {
  if (!program || !fmt)
    return;
  if (program->diagnostic_count >= program->config.max_diagnostics)
    return;
  size_t capacity = program->diagnostic_capacity;
  if (!mir_reserve((void **)&program->diagnostics,
                   sizeof(*program->diagnostics), &capacity,
                   program->diagnostic_count + 1u)) {
    program->oom = 1u;
    return;
  }
  program->diagnostic_capacity = capacity;
  char *message = mir_format_message(fmt, args);
  if (!message) {
    program->oom = 1u;
    return;
  }
  AstMirDiagnostic *diag = &program->diagnostics[program->diagnostic_count++];
  memset(diag, 0, sizeof(*diag));
  diag->code = code;
  diag->severity = severity;
  diag->span = span;
  diag->message = message;
  mir_resolve_location(program->frontend, diag);
}

void mir_program_diag(AstMirProgram *program, AstMirDiagCode code,
                      AstDiagSeverity severity, AstSpan span, const char *fmt,
                      ...) {
  va_list args;
  va_start(args, fmt);
  mir_program_diag_v(program, code, severity, span, fmt, args);
  va_end(args);
}

void mir_merge_function_diagnostics(AstMirProgram *program) {
  if (!program)
    return;
  for (size_t fi = 0u; fi < program->function_count; ++fi) {
    AstMirFunction *fn = program->functions[fi];
    if (!fn)
      continue;
    for (size_t di = 0u; di < fn->diagnostic_count; ++di) {
      if (program->diagnostic_count >= program->config.max_diagnostics)
        break;
      size_t capacity = program->diagnostic_capacity;
      if (!mir_reserve((void **)&program->diagnostics,
                       sizeof(*program->diagnostics), &capacity,
                       program->diagnostic_count + 1u)) {
        program->oom = 1u;
        return;
      }
      program->diagnostic_capacity = capacity;
      AstMirDiagnostic diag = fn->diagnostics[di];
      fn->diagnostics[di].message = NULL;
      mir_resolve_location(program->frontend, &diag);
      program->diagnostics[program->diagnostic_count++] = diag;
    }
    if (fn->oom)
      program->oom = 1u;
  }
  if (program->oom &&
      program->diagnostic_count < program->config.max_diagnostics) {
    mir_program_diag(program, AST_MIR_DIAG_INTERNAL, AST_DIAG_ERROR,
                     (AstSpan){0}, "out of memory while constructing MIR");
  }
}

AstMirVReg mir_add_vreg(AstMirFunction *fn, AstMirVRegKind kind,
                        const AstSemaType *type, AstSpan span,
                        AstMirBlockId block, uint32_t definition_index,
                        uint32_t flags) {
  if (!fn || fn->vreg_count == AST_MIR_INVALID_ID)
    return AST_MIR_INVALID_ID;
  size_t capacity = fn->vreg_capacity;
  if (!mir_reserve((void **)&fn->vregs, sizeof(*fn->vregs), &capacity,
                   (size_t)fn->vreg_count + 1u)) {
    fn->oom = 1u;
    return AST_MIR_INVALID_ID;
  }
  if (capacity > UINT32_MAX) {
    fn->oom = 1u;
    return AST_MIR_INVALID_ID;
  }
  fn->vreg_capacity = (uint32_t)capacity;
  AstMirVReg id = fn->vreg_count++;
  AstMirVRegInfo *info = &fn->vregs[id].info;
  memset(info, 0, sizeof(*info));
  info->kind = kind;
  info->type = type;
  info->span = span;
  info->block = block;
  info->definition_index = definition_index;
  info->bank = mir_type_bank(type);
  info->bit_width = mir_type_bit_width(type);
  info->flags = flags;
  info->live_start = UINT64_MAX;
  return id;
}

AstMirMemoryId mir_add_memory(AstMirFunction *fn, AstMirMemoryKind kind,
                              AstMirBlockId block, uint32_t definition_index) {
  if (!fn || fn->memory_value_count == AST_MIR_INVALID_ID)
    return AST_MIR_INVALID_ID;
  size_t capacity = fn->memory_value_capacity;
  if (!mir_reserve((void **)&fn->memory_values, sizeof(*fn->memory_values),
                   &capacity, (size_t)fn->memory_value_count + 1u)) {
    fn->oom = 1u;
    return AST_MIR_INVALID_ID;
  }
  if (capacity > UINT32_MAX) {
    fn->oom = 1u;
    return AST_MIR_INVALID_ID;
  }
  fn->memory_value_capacity = (uint32_t)capacity;
  AstMirMemoryId id = fn->memory_value_count++;
  memset(&fn->memory_values[id], 0, sizeof(fn->memory_values[id]));
  fn->memory_values[id].info.kind = kind;
  fn->memory_values[id].info.block = block;
  fn->memory_values[id].info.definition_index = definition_index;
  return id;
}

int mir_append_block_argument(AstMirFunction *fn, AstMirBlockId block_id,
                              AstMirVReg vreg, AstHirLocalId source_local) {
  if (!fn || block_id >= fn->block_count || vreg >= fn->vreg_count)
    return 0;
  AstMirBlock *block = &fn->blocks[block_id];
  size_t capacity = block->argument_capacity;
  size_t needed = (size_t)block->argument_count + 1u;
  if (needed > capacity) {
    size_t next_capacity = capacity ? capacity : 4u;
    while (next_capacity < needed) {
      if (next_capacity > SIZE_MAX / 2u) {
        fn->oom = 1u;
        return 0;
      }
      next_capacity *= 2u;
    }
    size_t vreg_bytes = 0u, local_bytes = 0u;
    if (ast_size_mul_overflow(next_capacity, sizeof(*block->arguments),
                              &vreg_bytes) ||
        ast_size_mul_overflow(next_capacity,
                              sizeof(*block->argument_source_locals),
                              &local_bytes)) {
      fn->oom = 1u;
      return 0;
    }
    AstMirVReg *arguments = (AstMirVReg *)malloc(vreg_bytes);
    AstHirLocalId *locals = (AstHirLocalId *)malloc(local_bytes);
    if (!arguments || !locals) {
      free(arguments);
      free(locals);
      fn->oom = 1u;
      return 0;
    }
    if (block->argument_count) {
      memcpy(arguments, block->arguments,
             (size_t)block->argument_count * sizeof(*arguments));
      memcpy(locals, block->argument_source_locals,
             (size_t)block->argument_count * sizeof(*locals));
    }
    free(block->arguments);
    free(block->argument_source_locals);
    block->arguments = arguments;
    block->argument_source_locals = locals;
    block->argument_capacity =
        next_capacity > UINT32_MAX ? UINT32_MAX : (uint32_t)next_capacity;
    if (next_capacity > UINT32_MAX) {
      fn->oom = 1u;
      return 0;
    }
  }
  block->arguments[block->argument_count] = vreg;
  block->argument_source_locals[block->argument_count] = source_local;
  block->argument_count++;
  return 1;
}

AstMirInstId mir_emit_instruction(AstMirFunction *fn, AstMirBlockId block_id,
                                  AstMirInstruction inst,
                                  const AstMirVReg *operands,
                                  uint32_t operand_count) {
  if (!fn || block_id >= fn->block_count ||
      fn->instruction_count == AST_MIR_INVALID_ID)
    return AST_MIR_INVALID_ID;
  size_t inst_capacity = fn->instruction_capacity;
  if (!mir_reserve((void **)&fn->instructions, sizeof(*fn->instructions),
                   &inst_capacity, (size_t)fn->instruction_count + 1u)) {
    fn->oom = 1u;
    return AST_MIR_INVALID_ID;
  }
  if (inst_capacity > UINT32_MAX) {
    fn->oom = 1u;
    return AST_MIR_INVALID_ID;
  }
  fn->instruction_capacity = (uint32_t)inst_capacity;
  AstMirBlock *block = &fn->blocks[block_id];
  size_t block_capacity = block->instruction_capacity;
  if (!mir_reserve((void **)&block->instructions, sizeof(*block->instructions),
                   &block_capacity, (size_t)block->instruction_count + 1u)) {
    fn->oom = 1u;
    return AST_MIR_INVALID_ID;
  }
  if (block_capacity > UINT32_MAX) {
    fn->oom = 1u;
    return AST_MIR_INVALID_ID;
  }
  block->instruction_capacity = (uint32_t)block_capacity;
  AstMirVReg *copy = NULL;
  if (operand_count) {
    copy = AST_ARENA_ARRAY(&fn->arena, AstMirVReg, operand_count);
    if (!copy) {
      fn->oom = 1u;
      return AST_MIR_INVALID_ID;
    }
    memcpy(copy, operands, (size_t)operand_count * sizeof(*copy));
  }
  AstMirInstId id = fn->instruction_count++;
  inst.info.id = id;
  inst.info.operands = copy;
  inst.info.operand_count = operand_count;
  inst.operands = copy;
  fn->instructions[id] = inst;
  block->instructions[block->instruction_count++] = id;
  return id;
}

static int mir_copy_term_args(AstMirVReg **storage, uint32_t *capacity,
                              const AstMirVReg *values, uint32_t count) {
  if (count == 0u)
    return 1;
  size_t cap = *capacity;
  if (!mir_reserve((void **)storage, sizeof(**storage), &cap, count))
    return 0;
  if (cap > UINT32_MAX)
    return 0;
  *capacity = (uint32_t)cap;
  memcpy(*storage, values, (size_t)count * sizeof(**storage));
  return 1;
}

int mir_set_terminator(AstMirFunction *fn, AstMirBlockId block_id,
                       AstMirTerminator term, const AstMirVReg *true_args,
                       uint32_t true_count, const AstMirVReg *false_args,
                       uint32_t false_count) {
  if (!fn || block_id >= fn->block_count)
    return 0;
  AstMirBlock *block = &fn->blocks[block_id];
  if (!mir_copy_term_args(&term.true_args, &term.true_arg_capacity, true_args,
                          true_count) ||
      !mir_copy_term_args(&term.false_args, &term.false_arg_capacity,
                          false_args, false_count)) {
    free(term.true_args);
    free(term.false_args);
    fn->oom = 1u;
    return 0;
  }
  term.info.true_args = term.true_args;
  term.info.true_arg_count = true_count;
  term.info.false_args = term.false_args;
  term.info.false_arg_count = false_count;
  free(block->term.true_args);
  free(block->term.false_args);
  block->term = term;
  return 1;
}

static int mir_copy_function_metadata(AstMirProgram *program,
                                      AstMirFunction *fn,
                                      const AstHirFunctionInfo *info) {
  fn->module_name = mir_copy_name(&program->arena, info->module_name);
  fn->name = mir_copy_name(&program->arena, info->name);
  fn->abi = mir_copy_text_value(&program->arena, info->abi);
  fn->link_name = mir_copy_text_value(&program->arena, info->link_name);
  fn->target_constraint =
      mir_copy_text_value(&program->arena, info->target_constraint);
  fn->requires_constraint =
      mir_copy_text_value(&program->arena, info->requires_constraint);
  return fn->module_name.text && fn->name.text &&
         (!fn->abi.is_present || fn->abi.text.data) &&
         (!fn->link_name.is_present || fn->link_name.text.data) &&
         (!fn->target_constraint.is_present ||
          fn->target_constraint.text.data) &&
         (!fn->requires_constraint.is_present ||
          fn->requires_constraint.text.data);
}

AstMirFunction *mir_function_create(AstMirProgram *program,
                                    const AstHirFunction *hir_function) {
  if (!program || !hir_function)
    return NULL;
  AstHirFunctionInfo info;
  if (!ast_hir_function_info(hir_function, &info))
    return NULL;
  AstMirFunction *fn = (AstMirFunction *)calloc(1u, sizeof(*fn));
  if (!fn)
    return NULL;
  ast_arena_init(&fn->arena, program->config.arena_block_size);
  fn->source_hir = hir_function;
  fn->type = info.type;
  fn->span = info.span;
  fn->flags = info.flags;
  fn->entry_block = info.entry_block;
  fn->frame_policy = info.frame_policy;
  fn->stack_alignment = info.stack_alignment;
  if (!mir_copy_function_metadata(program, fn, &info)) {
    mir_function_destroy(fn);
    return NULL;
  }
  return fn;
}

void mir_function_destroy(AstMirFunction *fn) {
  if (!fn)
    return;
  if (fn->optimization_proof) {
    mir_function_destroy(fn->optimization_proof->original);
    free(fn->optimization_proof);
  }
  for (uint32_t bi = 0u; bi < fn->block_count; ++bi) {
    free(fn->blocks[bi].arguments);
    free(fn->blocks[bi].argument_source_locals);
    free(fn->blocks[bi].instructions);
    free(fn->blocks[bi].predecessors);
    free(fn->blocks[bi].term.true_args);
    free(fn->blocks[bi].term.false_args);
  }
  for (size_t i = 0u; i < fn->diagnostic_count; ++i)
    free((void *)fn->diagnostics[i].message);
  free(fn->diagnostics);
  free(fn->blocks);
  if (fn->gvn_certificates) {
    for (uint32_t i = 0; i < fn->instruction_count; ++i)
      if (fn->gvn_certificates[i].valid)
        free(fn->gvn_certificates[i].original.operands);
  }
  free(fn->gvn_certificates);
  if (fn->branch_certificates) {
    for (uint32_t bi = 0u; bi < fn->block_count; ++bi) {
      free(fn->branch_certificates[bi].saved_true);
      free(fn->branch_certificates[bi].saved_false);
    }
  }
  free(fn->branch_certificates);
  free(fn->instructions);
  free(fn->vregs);
  free(fn->memory_values);
  free(fn->stack_slots);
  free(fn->inline_sites);
  free(fn->live_in);
  free(fn->live_out);
  ast_arena_destroy(&fn->arena);
  free(fn);
}

AstMirConfig ast_mir_config_default(void) {
  AstMirConfig config;
  memset(&config, 0, sizeof(config));
  config.thread_count = 0u;
  config.arena_block_size = AST_MIR_DEFAULT_ARENA_BLOCK;
  config.max_diagnostics = AST_MIR_DEFAULT_MAX_DIAGNOSTICS;
  config.verify_hir_before_build = 1u;
  config.verify_after_build = 1u;
  config.promote_locals = 1u;
  return config;
}

typedef struct MirParallelContext {
  AstMirProgram *program;
  size_t chunk_size;
} MirParallelContext;

static void mir_lower_worker(void *context, size_t chunk_index,
                             size_t worker_index) {
  ASTER_UNUSED(worker_index);
  MirParallelContext *parallel = (MirParallelContext *)context;
  if (!parallel || !parallel->program || parallel->chunk_size == 0u)
    return;
  size_t begin = chunk_index * parallel->chunk_size;
  if (begin >= parallel->program->function_count)
    return;
  size_t end = begin + parallel->chunk_size;
  if (end < begin || end > parallel->program->function_count)
    end = parallel->program->function_count;
  for (size_t index = begin; index < end; ++index) {
    AstMirFunction *fn = parallel->program->functions[index];
    if (fn)
      (void)mir_lower_function(parallel->program, fn);
  }
}

static void mir_rebuild_stats(AstMirProgram *program) {
  if (!program)
    return;
  AstMirStats stats;
  memset(&stats, 0, sizeof(stats));
  stats.module_count = program->hir ? ast_hir_module_count(program->hir) : 0u;
  stats.function_count = program->function_count;
  stats.global_count = program->hir ? ast_hir_global_count(program->hir) : 0u;
  for (size_t fi = 0u; fi < program->function_count; ++fi) {
    AstMirFunction *fn = program->functions[fi];
    if (!fn)
      continue;
    if (fn->flags & AST_HIR_FUNCTION_EXTERN)
      stats.extern_function_count++;
    if (fn->flags & AST_HIR_FUNCTION_DISPATCH)
      stats.dispatch_function_count++;
    stats.block_count += fn->block_count;
    stats.instruction_count += fn->instruction_count;
    stats.vreg_count += fn->vreg_count;
    stats.memory_value_count += fn->memory_value_count;
    stats.stack_slot_count += fn->stack_slot_count;
    stats.promoted_local_count += fn->promoted_local_count;
    stats.phi_count += fn->phi_count;
    stats.memory_phi_count += fn->memory_phi_count;
    stats.arena_bytes_used += fn->arena.bytes_used;
    stats.arena_bytes_reserved += fn->arena.bytes_reserved;
    for (uint32_t bi = 0u; bi < fn->block_count; ++bi) {
      if (fn->blocks[bi].term.info.kind != AST_MIR_TERM_NONE)
        stats.terminator_count++;
      if (!fn->blocks[bi].reachable)
        stats.unreachable_block_count++;
    }
  }
  for (size_t di = 0u; di < program->diagnostic_count; ++di) {
    if (program->diagnostics[di].severity == AST_DIAG_ERROR)
      stats.error_count++;
    else if (program->diagnostics[di].severity == AST_DIAG_WARNING)
      stats.warning_count++;
  }
  stats.arena_bytes_used += program->arena.bytes_used;
  stats.arena_bytes_reserved += program->arena.bytes_reserved;
  program->stats = stats;
}

AstMirProgram *ast_mir_build(const AstFrontend *frontend, AstHirProgram *hir,
                             const AstMirConfig *config) {
  AstMirConfig selected = config ? *config : ast_mir_config_default();
  if (selected.arena_block_size == 0u)
    selected.arena_block_size = AST_MIR_DEFAULT_ARENA_BLOCK;
  if (selected.max_diagnostics == 0u)
    selected.max_diagnostics = AST_MIR_DEFAULT_MAX_DIAGNOSTICS;
  AstMirProgram *program = (AstMirProgram *)calloc(1u, sizeof(*program));
  if (!program)
    return NULL;
  program->frontend = frontend;
  program->hir = hir;
  program->config = selected;
  ast_arena_init(&program->arena, selected.arena_block_size);
  if (!hir) {
    mir_program_diag(program, AST_MIR_DIAG_HIR_REQUIRED, AST_DIAG_ERROR,
                     (AstSpan){0}, "verified HIR is required to construct MIR");
    program->build_diagnostic_count = program->diagnostic_count;
    mir_rebuild_stats(program);
    return program;
  }
  if (selected.verify_hir_before_build && !ast_hir_verify(hir)) {
    mir_program_diag(program, AST_MIR_DIAG_HIR_INVALID, AST_DIAG_ERROR,
                     (AstSpan){0}, "HIR verifier rejected the input program");
    program->build_diagnostic_count = program->diagnostic_count;
    mir_rebuild_stats(program);
    return program;
  }
  size_t hir_function_count = ast_hir_function_count(hir);
  for (size_t i = 0u; i < hir_function_count; ++i) {
    const AstHirFunction *source = ast_hir_function_at(hir, i);
    if (source && !(source->flags & AST_HIR_FUNCTION_GENERIC))
      program->function_count++;
  }
  if (program->function_count) {
    size_t bytes = 0u;
    if (ast_size_mul_overflow(program->function_count,
                              sizeof(*program->functions), &bytes)) {
      program->oom = 1u;
    } else {
      program->functions = (AstMirFunction **)calloc(1u, bytes);
      if (!program->functions)
        program->oom = 1u;
    }
  }
  size_t out_index = 0u;
  for (size_t i = 0u; i < hir_function_count && !program->oom; ++i) {
    const AstHirFunction *source = ast_hir_function_at(hir, i);
    if (!source || (source->flags & AST_HIR_FUNCTION_GENERIC))
      continue;
    program->functions[out_index] = mir_function_create(program, source);
    if (!program->functions[out_index])
      program->oom = 1u;
    out_index++;
  }
  if (!program->oom && program->function_count) {
    size_t workers = selected.thread_count ? selected.thread_count
                                           : ast_hardware_thread_count();
    if (workers == 0u)
      workers = 1u;
    if (workers > program->function_count)
      workers = program->function_count;
    size_t target_chunks = workers > SIZE_MAX / 8u ? workers : workers * 8u;
    size_t chunk_size =
        target_chunks
            ? (program->function_count + target_chunks - 1u) / target_chunks
            : 1u;
    if (chunk_size == 0u)
      chunk_size = 1u;
    size_t chunk_count =
        (program->function_count + chunk_size - 1u) / chunk_size;
    MirParallelContext parallel = {program, chunk_size};
    size_t workers_used = 0u;
    if (ast_parallel_for_workers(chunk_count, workers, mir_lower_worker,
                                 &parallel, &workers_used) != 0) {
      mir_program_diag(program, AST_MIR_DIAG_INTERNAL, AST_DIAG_ERROR,
                       (AstSpan){0}, "parallel MIR lowering failed");
    }
  }
  mir_merge_function_diagnostics(program);
  program->build_diagnostic_count = program->diagnostic_count;
  mir_rebuild_stats(program);
  if (selected.verify_after_build && program->stats.error_count == 0u)
    (void)ast_mir_verify(program);
  return program;
}

void ast_mir_destroy(AstMirProgram *program) {
  if (!program)
    return;
  for (size_t i = 0u; i < program->function_count; ++i)
    mir_function_destroy(program->functions[i]);
  for (size_t i = 0u; i < program->diagnostic_count; ++i)
    free((void *)program->diagnostics[i].message);
  free(program->diagnostics);
  free(program->functions);
  mir_inline_proof_destroy(program->inline_proof);
  ast_arena_destroy(&program->arena);
  free(program);
}

AstMirStats ast_mir_stats(const AstMirProgram *program) {
  return program ? program->stats : (AstMirStats){0};
}

uint32_t ast_mir_function_inline_site_count(const AstMirFunction *fn) {
  return fn ? fn->inline_site_count : 0u;
}

int ast_mir_function_inline_site(const AstMirFunction *fn, uint32_t site,
                                 AstMirInlineSiteInfo *out) {
  if (!fn || !out || !site || site > fn->inline_site_count)
    return 0;
  *out = fn->inline_sites[site - 1u];
  return 1;
}

const AstHirProgram *ast_mir_source_hir(const AstMirProgram *program) {
  return program ? program->hir : NULL;
}

const AstMirDiagnostic *ast_mir_diagnostics(const AstMirProgram *program,
                                            size_t *count_out) {
  if (count_out)
    *count_out = program ? program->diagnostic_count : 0u;
  return program ? program->diagnostics : NULL;
}

size_t ast_mir_function_count(const AstMirProgram *program) {
  return program ? program->function_count : 0u;
}

const AstMirFunction *ast_mir_function_at(const AstMirProgram *program,
                                          size_t index) {
  return program && index < program->function_count ? program->functions[index]
                                                    : NULL;
}

int ast_mir_find_function(const AstMirProgram *program, const char *module_name,
                          const char *function_name,
                          const AstMirFunction **out) {
  if (out)
    *out = NULL;
  if (!program || !module_name || !function_name)
    return 0;
  for (size_t i = 0u; i < program->function_count; ++i) {
    const AstMirFunction *fn = program->functions[i];
    if (fn && mir_name_eq_cstr(fn->module_name, module_name) &&
        mir_name_eq_cstr(fn->name, function_name)) {
      if (out)
        *out = fn;
      return 1;
    }
  }
  return 0;
}

int ast_mir_function_info(const AstMirFunction *fn, AstMirFunctionInfo *out) {
  if (!fn || !out)
    return 0;
  memset(out, 0, sizeof(*out));
  out->module_name = fn->module_name;
  out->name = fn->name;
  out->type = fn->type;
  out->span = fn->span;
  out->flags = fn->flags;
  out->entry_block = fn->entry_block;
  out->block_count = fn->block_count;
  out->instruction_count = fn->instruction_count;
  out->vreg_count = fn->vreg_count;
  out->memory_value_count = fn->memory_value_count;
  out->stack_slot_count = fn->stack_slot_count;
  out->promoted_local_count = fn->promoted_local_count;
  out->phi_count = fn->phi_count;
  out->memory_phi_count = fn->memory_phi_count;
  out->frame_policy = fn->frame_policy;
  out->stack_alignment = fn->stack_alignment;
  out->abi = fn->abi;
  out->link_name = fn->link_name;
  out->target_constraint = fn->target_constraint;
  out->requires_constraint = fn->requires_constraint;
  return 1;
}

int ast_mir_function_block(const AstMirFunction *fn, AstMirBlockId block_id,
                           AstMirBlockInfo *out) {
  if (!fn || !out || block_id >= fn->block_count)
    return 0;
  const AstMirBlock *block = &fn->blocks[block_id];
  memset(out, 0, sizeof(*out));
  out->id = block->id;
  out->span = block->span;
  out->arguments = block->arguments;
  out->argument_source_locals = block->argument_source_locals;
  out->argument_count = block->argument_count;
  out->instructions = block->instructions;
  out->instruction_count = block->instruction_count;
  out->terminator = block->term.info;
  out->terminator.true_args = block->term.true_args;
  out->terminator.false_args = block->term.false_args;
  out->memory_argument = block->memory_argument;
  out->predecessor_count = block->predecessor_count;
  out->successor_count = block->successor_count;
  out->immediate_dominator = block->immediate_dominator;
  out->reverse_postorder = block->reverse_postorder;
  out->loop_depth = block->loop_depth;
  out->live_in_count = block->live_in_count;
  out->live_out_count = block->live_out_count;
  out->reachable = block->reachable;
  return 1;
}

int ast_mir_function_instruction(const AstMirFunction *fn, AstMirInstId inst,
                                 AstMirInstructionInfo *out) {
  if (!fn || !out || inst >= fn->instruction_count)
    return 0;
  *out = fn->instructions[inst].info;
  out->operands = fn->instructions[inst].operands;
  return 1;
}

int ast_mir_function_vreg(const AstMirFunction *fn, AstMirVReg vreg,
                          AstMirVRegInfo *out) {
  if (!fn || !out || vreg >= fn->vreg_count)
    return 0;
  *out = fn->vregs[vreg].info;
  return 1;
}

int ast_mir_function_memory(const AstMirFunction *fn, AstMirMemoryId memory,
                            AstMirMemoryInfo *out) {
  if (!fn || !out || memory >= fn->memory_value_count)
    return 0;
  *out = fn->memory_values[memory].info;
  return 1;
}

int ast_mir_function_stack_slot(const AstMirFunction *fn,
                                AstMirStackSlotId slot,
                                AstMirStackSlotInfo *out) {
  if (!fn || !out || slot >= fn->stack_slot_count)
    return 0;
  *out = fn->stack_slots[slot].info;
  return 1;
}

int ast_mir_function_predecessor(const AstMirFunction *fn, AstMirBlockId block,
                                 uint32_t index, AstMirBlockId *out) {
  if (!fn || !out || block >= fn->block_count ||
      index >= fn->blocks[block].predecessor_count)
    return 0;
  *out = fn->blocks[block].predecessors[index];
  return 1;
}

int ast_mir_function_successor(const AstMirFunction *fn, AstMirBlockId block,
                               uint32_t index, AstMirBlockId *out) {
  if (!fn || !out || block >= fn->block_count ||
      index >= fn->blocks[block].successor_count)
    return 0;
  *out = fn->blocks[block].successors[index];
  return 1;
}

static int mir_liveness_contains(const AstMirFunction *fn, AstMirBlockId block,
                                 AstMirVReg vreg, const uint64_t *bits) {
  if (!fn || !bits || block >= fn->block_count || vreg >= fn->vreg_count ||
      fn->liveness_word_count == 0u)
    return 0;
  size_t word = (size_t)block * fn->liveness_word_count + (vreg / 64u);
  return (bits[word] & (UINT64_C(1) << (vreg % 64u))) != 0u;
}

int ast_mir_function_is_live_in(const AstMirFunction *fn, AstMirBlockId block,
                                AstMirVReg vreg) {
  return mir_liveness_contains(fn, block, vreg, fn ? fn->live_in : NULL);
}

int ast_mir_function_is_live_out(const AstMirFunction *fn, AstMirBlockId block,
                                 AstMirVReg vreg) {
  return mir_liveness_contains(fn, block, vreg, fn ? fn->live_out : NULL);
}

const AstHirFunction *ast_mir_function_source_hir(const AstMirFunction *fn) {
  return fn ? fn->source_hir : NULL;
}

const char *ast_mir_op_name(AstMirOp op) {
  switch (op) {
  case AST_MIR_OP_CONSTANT:
    return "g.constant";
  case AST_MIR_OP_UNDEF:
    return "g.undef";
  case AST_MIR_OP_COPY:
    return "g.copy";
  case AST_MIR_OP_STACK_ADDR:
    return "g.stack_addr";
  case AST_MIR_OP_STACK_LOAD:
    return "g.stack_load";
  case AST_MIR_OP_STACK_STORE:
    return "g.stack_store";
  case AST_MIR_OP_GLOBAL_ADDR:
    return "g.global_addr";
  case AST_MIR_OP_GLOBAL_LOAD:
    return "g.global_load";
  case AST_MIR_OP_GLOBAL_STORE:
    return "g.global_store";
  case AST_MIR_OP_FUNCTION_ADDR:
    return "g.function_addr";
  case AST_MIR_OP_GENERIC_CONST:
    return "g.generic_const";
  case AST_MIR_OP_UNARY:
    return "g.unary";
  case AST_MIR_OP_BINARY:
    return "g.binary";
  case AST_MIR_OP_CAST:
    return "g.cast";
  case AST_MIR_OP_SELECT:
    return "g.select";
  case AST_MIR_OP_PACK:
    return "g.pack";
  case AST_MIR_OP_CALL_DIRECT:
    return "g.call_direct";
  case AST_MIR_OP_CALL_INDIRECT:
    return "g.call_indirect";
  case AST_MIR_OP_INDEX:
    return "g.index";
  case AST_MIR_OP_MEMBER:
    return "g.member";
  case AST_MIR_OP_ADDRESS_OF:
    return "g.address_of";
  case AST_MIR_OP_LOAD:
    return "g.load";
  case AST_MIR_OP_STORE:
    return "g.store";
  case AST_MIR_OP_ATOMIC:
    return "g.atomic";
  case AST_MIR_OP_FENCE:
    return "g.fence";
  case AST_MIR_OP_INTRINSIC:
    return "g.intrinsic";
  case AST_MIR_OP_BIND:
    return "g.bind";
  case AST_MIR_OP_NATIVE:
    return "g.native";
  case AST_MIR_OP_NATIVE_RESULT:
    return "g.native_result";
  case AST_MIR_OP_INVALID:
  default:
    return "invalid";
  }
}

const char *ast_mir_term_kind_name(AstMirTermKind kind) {
  switch (kind) {
  case AST_MIR_TERM_BR:
    return "br";
  case AST_MIR_TERM_COND_BR:
    return "cond_br";
  case AST_MIR_TERM_RETURN:
    return "return";
  case AST_MIR_TERM_UNREACHABLE:
    return "unreachable";
  case AST_MIR_TERM_NONE:
  default:
    return "none";
  }
}

const char *ast_mir_reg_bank_name(AstMirRegBank bank) {
  switch (bank) {
  case AST_MIR_BANK_GENERAL:
    return "gp";
  case AST_MIR_BANK_FLOAT:
    return "fp";
  case AST_MIR_BANK_POINTER:
    return "ptr";
  case AST_MIR_BANK_CAPABILITY:
    return "cap";
  case AST_MIR_BANK_VECTOR:
    return "vec";
  case AST_MIR_BANK_MASK:
    return "mask";
  case AST_MIR_BANK_AGGREGATE:
    return "agg";
  case AST_MIR_BANK_GENERIC:
    return "generic";
  case AST_MIR_BANK_INVALID:
  default:
    return "invalid";
  }
}

const char *ast_mir_diag_code_name(AstMirDiagCode code) {
  switch (code) {
  case AST_MIR_DIAG_NONE:
    return "none";
  case AST_MIR_DIAG_HIR_REQUIRED:
    return "hir_required";
  case AST_MIR_DIAG_HIR_INVALID:
    return "hir_invalid";
  case AST_MIR_DIAG_UNSUPPORTED_HIR:
    return "unsupported_hir";
  case AST_MIR_DIAG_INVALID_CFG:
    return "invalid_cfg";
  case AST_MIR_DIAG_MISSING_TERMINATOR:
    return "missing_terminator";
  case AST_MIR_DIAG_INVALID_SUCCESSOR:
    return "invalid_successor";
  case AST_MIR_DIAG_BRANCH_ARGUMENT_MISMATCH:
    return "branch_argument_mismatch";
  case AST_MIR_DIAG_INVALID_VREG:
    return "invalid_vreg";
  case AST_MIR_DIAG_INVALID_OPERAND:
    return "invalid_operand";
  case AST_MIR_DIAG_TYPE_MISMATCH:
    return "type_mismatch";
  case AST_MIR_DIAG_DOMINANCE:
    return "dominance";
  case AST_MIR_DIAG_INVALID_MEMORY_CHAIN:
    return "invalid_memory_chain";
  case AST_MIR_DIAG_INVALID_STACK_SLOT:
    return "invalid_stack_slot";
  case AST_MIR_DIAG_INVALID_EFFECTS:
    return "invalid_effects";
  case AST_MIR_DIAG_INVALID_LIVENESS:
    return "invalid_liveness";
  case AST_MIR_DIAG_INTERNAL:
    return "internal";
  case AST_MIR_DIAG_TOO_MANY_DIAGNOSTICS:
    return "too_many_diagnostics";
  case AST_MIR_DIAG_INLINE_POLICY:
    return "inline_policy";
  default:
    return "unknown";
  }
}

/* Called by the verifier after diagnostics have been replaced. */
void mir_refresh_stats_after_verify(AstMirProgram *program) {
  mir_rebuild_stats(program);
}
