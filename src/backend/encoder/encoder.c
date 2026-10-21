#include "../common/backend_compile.h"
#include "../regalloc/allocation_internal.h"
#include "encoder_internal.h"

int encoder_fail(EncoderBuilder *b, AstEncoderDiagCode code,
                 const char *message) {
  if (b->out->diagnostic == AST_ENCODER_DIAG_NONE) {
    b->out->diagnostic = code;
    b->out->instruction = b->instruction;
    b->out->message = message;
  }
  return 0;
}

int encoder_le(EncoderBuilder *b, uint64_t bits, unsigned bytes) {
  size_t need;
  if (bytes > 8u || ast_size_add_overflow(b->out->size, bytes, &need) ||
      need > INT64_MAX)
    return encoder_fail(b, AST_ENCODER_DIAG_OUT_OF_MEMORY,
                        "Encoded function is too large.");
  if (need > b->capacity) {
    size_t cap = b->capacity ? b->capacity : 128u;
    while (cap < need) {
      if (cap > SIZE_MAX / 2u) {
        cap = need;
        break;
      }
      cap *= 2u;
    }
    uint8_t *p = (uint8_t *)realloc(b->out->bytes, cap);
    if (!p)
      return encoder_fail(b, AST_ENCODER_DIAG_OUT_OF_MEMORY,
                          "Cannot allocate instruction bytes.");
    b->out->bytes = p;
    b->capacity = cap;
  }
  for (unsigned i = 0; i < bytes; ++i)
    b->out->bytes[b->out->size++] = (uint8_t)(bits >> (8u * i));
  return 1;
}

int encoder_fixup(EncoderBuilder *b, size_t offset, uint8_t width,
                  AstEncoderFixupKind kind, AstName module, AstName symbol) {
  if (!symbol.text || !symbol.span.length || offset > b->out->size ||
      width > b->out->size - offset)
    return encoder_fail(b, AST_ENCODER_DIAG_UNSUPPORTED_OPERAND,
                        "Invalid symbol fixup.");
  if (!b->block_offsets)
    return 1; /* measuring pass */
  if (b->out->fixup_count == b->fixup_capacity) {
    size_t cap = b->fixup_capacity ? b->fixup_capacity * 2u : 8u;
    if (cap < b->fixup_capacity || cap > SIZE_MAX / sizeof(AstEncoderFixup))
      return encoder_fail(b, AST_ENCODER_DIAG_OUT_OF_MEMORY,
                          "Too many fixups.");
    AstEncoderFixup *p =
        (AstEncoderFixup *)realloc(b->out->fixups, cap * sizeof(*p));
    if (!p)
      return encoder_fail(b, AST_ENCODER_DIAG_OUT_OF_MEMORY,
                          "Cannot allocate fixups.");
    b->out->fixups = p;
    b->fixup_capacity = cap;
  }
  b->out->fixups[b->out->fixup_count++] =
      (AstEncoderFixup){offset, width, kind, symbol, module, 0};
  return 1;
}

int encoder_delta(EncoderBuilder *b, uint32_t block, size_t pc,
                  int64_t *delta) {
  if (block >= b->block_count || pc > INT64_MAX)
    return encoder_fail(b, AST_ENCODER_DIAG_INVALID_PHYSICAL,
                        "Invalid branch target.");
  if (!b->block_offsets) {
    *delta = 0;
    return 1;
  }
  if (b->block_offsets[block] == SIZE_MAX)
    return encoder_fail(b, AST_ENCODER_DIAG_INVALID_PHYSICAL,
                        "Branch targets an empty block.");
  *delta = (int64_t)b->block_offsets[block] - (int64_t)pc;
  return 1;
}

int encoder_reg(const EncoderBuilder *b, const AstPhysicalOperand *o,
                AstTargetRegisterInfo *r) {
  if ((o->kind != AST_PHYSICAL_OPERAND_REGISTER &&
       o->kind != AST_PHYSICAL_OPERAND_MEMORY) ||
      !memchr(o->register_name, 0, sizeof(o->register_name)) ||
      !ast_target_parse_register(&b->target, o->register_name, r))
    return 0;
  MachinePhysRegView view;
  return machine_physreg_view(&b->target, o->register_name, &view) &&
         view.alias_unit == o->register_unit;
}

int encoder_shape(const AstPhysicalInstructionInfo *i, uint32_t defs,
                  uint32_t count) {
  return i->result_count == defs && i->operand_count == count;
}
int encoder_same(const AstPhysicalOperand *a, const AstPhysicalOperand *b) {
  return a->kind == AST_PHYSICAL_OPERAND_REGISTER && b->kind == a->kind &&
         a->register_unit == b->register_unit && a->bit_width == b->bit_width &&
         a->bank == b->bank;
}

void ast_encoder_output_destroy(AstEncoderOutput *out) {
  if (!out)
    return;
  free(out->bytes);
  free(out->fixups);
  memset(out, 0, sizeof(*out));
}

static int layout_return(AstMachineOpcode op) {
  return op == AST_MACHINE_X86_RET || op == AST_MACHINE_X86_TRAP ||
         op == AST_MACHINE_A64_RET || op == AST_MACHINE_A64_TRAP ||
         op == AST_MACHINE_RISCV_RET || op == AST_MACHINE_RISCV_EBREAK;
}
static int layout_terminator(AstMachineOpcode op) {
  return layout_return(op) || op == AST_MACHINE_X86_BR ||
         op == AST_MACHINE_X86_COND_BR || op == AST_MACHINE_X86_ICMP_BR ||
         op == AST_MACHINE_A64_BR || op == AST_MACHINE_A64_COND_BR ||
         op == AST_MACHINE_A64_CMP_BR || op == AST_MACHINE_RISCV_J ||
         op == AST_MACHINE_RISCV_CMP_BR;
}
/* Complete managed CFGs can be laid out as deterministic traces. Keep
   the entry first, follow unconditional edges and prefer a non-return arm.
   Block IDs and fixup targets remain stable; offsets are indexed by old ID.
   Raw/native streams and incomplete test views retain their explicit order. */
static int trace_layout(const AstPhysicalBlockInfo *blocks, size_t count,
                        uint32_t entry, uint32_t *order, size_t *length) {
  for (size_t b = 0; b < count; ++b) {
    const AstPhysicalBlockInfo *bb = &blocks[b];
    if (bb->id != b || !bb->instruction_count || !bb->instructions)
      return 0;
    for (uint32_t j = 0; j < bb->instruction_count; ++j) {
      const AstPhysicalInstructionInfo *i = &bb->instructions[j];
      if (i->opcode == AST_MACHINE_X86_RAW_BYTES ||
          i->opcode == AST_MACHINE_X86_JMP ||
          i->opcode == AST_MACHINE_X86_JMP_INDIRECT)
        return 0;
      if (i->operand_count > AST_PHYSICAL_MAX_OPERANDS)
        return 0;
      for (uint32_t o = 0; o < i->operand_count; ++o)
        if (i->operands[o].kind == AST_PHYSICAL_OPERAND_BLOCK) {
          int rv_pair = j + 2u == bb->instruction_count &&
                        i->opcode == AST_MACHINE_RISCV_BNE &&
                        bb->instructions[j + 1u].opcode == AST_MACHINE_RISCV_J;
          if ((j + 1u != bb->instruction_count && !rv_pair) ||
              i->operands[o].block >= count)
            return 0;
        }
    }
    AstMachineOpcode op = bb->instructions[bb->instruction_count - 1u].opcode;
    if (!layout_terminator(op))
      return 0;
  }
  uint32_t *pending = malloc(count * sizeof(*pending));
  uint8_t *seen = calloc(count, 1u);
  if (!pending || !seen) {
    free(pending);
    free(seen);
    return -1;
  }
  size_t n = 0u, top = 0u;
  pending[top++] = entry;
  seen[entry] = 1u;
  while (top) {
    uint32_t id = pending[--top];
    for (;;) {
      order[n++] = id;
      const AstPhysicalBlockInfo *bb = &blocks[id];
      uint32_t targets[2], edges = 0u;
      uint32_t begin = bb->instruction_count - 1u;
      if (begin && bb->instructions[begin - 1u].opcode == AST_MACHINE_RISCV_BNE)
        --begin;
      for (uint32_t j = begin; j < bb->instruction_count; ++j)
        for (uint32_t o = 0; o < bb->instructions[j].operand_count; ++o)
          if (bb->instructions[j].operands[o].kind ==
                  AST_PHYSICAL_OPERAND_BLOCK &&
              edges < 2u)
            targets[edges++] = bb->instructions[j].operands[o].block;
      uint32_t preferred = 0u;
      if (edges == 2u) {
        const AstPhysicalBlockInfo *no = &blocks[targets[1]];
        AstMachineOpcode last =
            no->instructions[no->instruction_count - 1u].opcode;
        preferred = layout_return(last) ? 0u : 1u;
        uint32_t other = targets[preferred ^ 1u];
        if (!seen[other]) {
          seen[other] = 1u;
          pending[top++] = other;
        }
      }
      if (!edges || seen[targets[preferred]])
        break;
      id = targets[preferred];
      seen[id] = 1u;
    }
  }
  *length = n;
  free(pending);
  free(seen);
  return 1;
}

static int encode_blocks(AstTargetConfig target,
                         const AstPhysicalBlockInfo *blocks, size_t count,
                         uint32_t entry, int traces, AstEncoderOutput *out) {
  if (!out)
    return 0;
  memset(out, 0, sizeof(*out));
  out->diagnostic = AST_ENCODER_DIAG_NONE;
  EncoderBuilder b = {0};
  b.out = out;
  b.target = target;
  b.block_count = count;
  b.instruction = AST_MACHINE_INVALID_ID;
  b.target.features = ast_target_feature_closure(target.arch, target.features);
  size_t *offsets = NULL;
  uint32_t *layout = NULL;
  size_t layout_count = count;
  if (!blocks || !count || count > UINT32_MAX || entry >= count ||
      !blocks[entry].instruction_count || count > SIZE_MAX / sizeof(*offsets)) {
    encoder_fail(&b, AST_ENCODER_DIAG_INVALID_ARGUMENT,
                 "Invalid function blocks or entry.");
    goto done;
  }
  offsets = (size_t *)malloc(count * sizeof(*offsets));
  if (!offsets) {
    encoder_fail(&b, AST_ENCODER_DIAG_OUT_OF_MEMORY,
                 "Cannot allocate block offsets.");
    goto done;
  }
  if (traces) {
    layout = malloc(count * sizeof(*layout));
    int result =
        layout ? trace_layout(blocks, count, entry, layout, &layout_count) : -1;
    if (result < 0) {
      encoder_fail(&b, AST_ENCODER_DIAG_OUT_OF_MEMORY,
                   "Cannot allocate CFG layout.");
      goto done;
    }
    if (!result) {
      free(layout);
      layout = NULL;
    }
  }
  for (size_t id = 0; id < count; ++id)
    offsets[id] = SIZE_MAX;
  if (target.arch == AST_TARGET_ARCH_X86_64 ||
      target.arch == AST_TARGET_ARCH_RISCV64) {
    size_t instructions = 0;
    for (size_t id = 0; id < count; ++id) {
      if (blocks[id].instruction_count > (SIZE_MAX - 1u) / 2u - instructions) {
        encoder_fail(&b, AST_ENCODER_DIAG_OUT_OF_MEMORY,
                     "Too many branch instructions.");
        goto done;
      }
      instructions += blocks[id].instruction_count;
    }
    b.branch_capacity = instructions * 2u + 1u;
    if (b.branch_capacity > SIZE_MAX / sizeof(*b.local_branches)) {
      encoder_fail(&b, AST_ENCODER_DIAG_OUT_OF_MEMORY,
                   "Too many local branches.");
      goto done;
    }
    b.local_branches = (EncoderLocalBranch *)calloc(b.branch_capacity,
                                                    sizeof(*b.local_branches));
    b.short_branches = (uint8_t *)calloc(b.branch_capacity, 1u);
    if (!b.local_branches || !b.short_branches) {
      encoder_fail(&b, AST_ENCODER_DIAG_OUT_OF_MEMORY,
                   "Cannot allocate branch relaxation.");
      goto done;
    }
  }
  int final = 0;
  for (;;) {
    out->size = 0u;
    out->fixup_count = 0u;
    b.branch_count = 0u;
    b.block_offsets = final ? offsets : NULL;
    for (size_t order = 0; order < layout_count; ++order) {
      /* Place the actual entry first, then all remaining IDs in order. */
      size_t id = layout           ? layout[order]
                  : order == 0u    ? entry
                  : order <= entry ? order - 1u
                                   : order;
      const AstPhysicalBlockInfo *block = &blocks[id];
      b.current_block = (uint32_t)id;
      b.next_block = AST_MACHINE_INVALID_ID;
      for (size_t next_order = order + 1u; next_order < layout_count;
           ++next_order) {
        size_t next_id = layout                ? layout[next_order]
                         : next_order <= entry ? next_order - 1u
                                               : next_order;
        if (blocks[next_id].instruction_count) {
          b.next_block = (uint32_t)next_id;
          break;
        }
      }
      if (block->id != id ||
          (block->instruction_count && !block->instructions)) {
        encoder_fail(&b, AST_ENCODER_DIAG_INVALID_PHYSICAL,
                     "Invalid block view.");
        goto done;
      }
      if (!final)
        offsets[id] = block->instruction_count ? out->size : SIZE_MAX;
      for (uint32_t j = 0; j < block->instruction_count; ++j) {
        const AstPhysicalInstructionInfo *i = &block->instructions[j];
        b.instruction = i->source_instruction;
        int ok = 0;
        if (i->operand_count <= AST_PHYSICAL_MAX_OPERANDS &&
            i->result_count <= i->operand_count) {
          if (target.arch == AST_TARGET_ARCH_X86_64)
            ok = encoder_x86_instruction(&b, i);
          else if (target.arch == AST_TARGET_ARCH_AARCH64)
            ok = encoder_a64_instruction(&b, i);
          else if (target.arch == AST_TARGET_ARCH_RISCV64)
            ok = encoder_rv_instruction(&b, i);
        }
        if (!ok) {
          encoder_fail(&b, AST_ENCODER_DIAG_UNSUPPORTED_INSTRUCTION,
                       "Instruction form has no encoding contract.");
          goto done;
        }
      }
    }
    if (final)
      break;
    int changed = 0;
    for (size_t r = 0; r < b.branch_count; ++r) {
      const EncoderLocalBranch *branch = &b.local_branches[r];
      if (b.short_branches[r] || branch->target >= count ||
          offsets[branch->target] == SIZE_MAX)
        continue;
      size_t target_offset = offsets[branch->target];
      /* Shrinking this branch also moves forward targets. Other shrinks
       * can only reduce either displacement, so selection is monotone. */
      int64_t delta;
      if (target.arch == AST_TARGET_ARCH_RISCV64)
        delta = (int64_t)target_offset - (int64_t)branch->offset -
                (target_offset > branch->offset ? 4 : 0);
      else {
        size_t pc = branch->offset +
                    (target_offset > branch->offset ? branch->size : 2u);
        delta = (int64_t)target_offset - (int64_t)pc;
      }
      if (delta >=
              (target.arch == AST_TARGET_ARCH_RISCV64 ? -4096 : INT8_MIN) &&
          delta <= (target.arch == AST_TARGET_ARCH_RISCV64 ? 4094 : INT8_MAX)) {
        b.short_branches[r] = 1u;
        changed = 1;
      }
    }
    if (!changed)
      final = 1;
  }
done:
  free(offsets);
  free(layout);
  free(b.local_branches);
  free(b.short_branches);
  if (out->diagnostic != AST_ENCODER_DIAG_NONE) {
    free(out->bytes);
    free(out->fixups);
    out->bytes = NULL;
    out->fixups = NULL;
    out->size = out->fixup_count = 0u;
    return 0;
  }
  return 1;
}

int encoder_blocks(AstTargetConfig target, const AstPhysicalBlockInfo *blocks,
                   size_t count, uint32_t entry, AstEncoderOutput *out) {
  return encode_blocks(target, blocks, count, entry, 0, out);
}
int encoder_blocks_ordered(AstTargetConfig target,
                           const AstPhysicalBlockInfo *blocks, size_t count,
                           uint32_t entry, AstEncoderOutput *out) {
  return encode_blocks(target, blocks, count, entry, 1, out);
}

static int encoder_source_function_is_extern(const AstRegAllocProgram *p,
                                             size_t function) {
  if (!p || !p->machine || !p->machine->mir)
    return 0;
  const AstMirFunction *mf = ast_mir_function_at(p->machine->mir, function);
  AstMirFunctionInfo info;
  return mf && ast_mir_function_info(mf, &info) &&
         (info.flags & AST_HIR_FUNCTION_EXTERN) != 0u;
}

static int encode_function(const AstRegAllocProgram *p, size_t f,
                           AstTargetArch arch, int verify,
                           AstEncoderOutput *out) {
  if (!out)
    return 0;
  memset(out, 0, sizeof(*out));
  out->diagnostic = AST_ENCODER_DIAG_INVALID_PHYSICAL;
  out->instruction = AST_MACHINE_INVALID_ID;
  out->message = "Encoder requires a verified finalized function for the "
                 "requested target.";
  if (!p || f >= p->function_count || (arch && p->target.arch != arch) ||
      (verify && !ast_physical_verify(p)))
    return 0;
  if (encoder_source_function_is_extern(p, f)) {
    out->diagnostic = AST_ENCODER_DIAG_NONE;
    out->message = NULL;
    return 1;
  }
  size_t count = ast_physical_block_count(p, f);
  if (count > SIZE_MAX / sizeof(AstPhysicalBlockInfo))
    return 0;
  AstPhysicalBlockInfo *blocks =
      (AstPhysicalBlockInfo *)calloc(count ? count : 1u, sizeof(*blocks));
  if (!blocks) {
    out->diagnostic = AST_ENCODER_DIAG_OUT_OF_MEMORY;
    return 0;
  }
  for (uint32_t id = 0; id < count; ++id)
    if (!ast_physical_block(p, f, id, &blocks[id])) {
      free(blocks);
      return 0;
    }
  const MachineFunction *machine = &p->machine->functions[f];
  int traces = 1;
  for (uint32_t i = 0; i < machine->info.instruction_count; ++i)
    if (machine->instructions[i].info.source_op == AST_MIR_OP_NATIVE)
      traces = 0;
  int ok = encode_blocks(p->target, blocks, count, machine->info.entry_block,
                         traces, out);
  free(blocks);
  return ok;
}
static int encode_stream_function(AstRegAllocProgram *p, uint32_t f,
                                  void *opaque) {
  AstEncoderModuleOutput *output = opaque;
  /* NONE is not zero. Allocation/native validation can fail before the
     encoder initializes its output; preserve that earlier diagnostic. */
  output->functions[f].diagnostic = AST_ENCODER_DIAG_NONE;
  int ok = regalloc_allocate_function(p, f) && regalloc_finalize_function(p, f);
  if (ok &&
      (!regalloc_verify_function(p, f) || !physical_verify_one_function(p, f)))
    ok = regalloc_fail(p, AST_REGALLOC_DIAG_INVALID_PHYSICAL, f,
                       AST_MACHINE_INVALID_ID, "Physical verification failed.");
  p->allocation_verified = p->finalized = 1u;
  if (ok &&
      !encode_function(p, f, AST_TARGET_ARCH_INVALID, 0, &output->functions[f]))
    ok = regalloc_fail(p, AST_REGALLOC_DIAG_UNSUPPORTED_INSTRUCTION, f,
                       output->functions[f].instruction,
                       output->functions[f].message
                           ? output->functions[f].message
                           : "Encoding failed.");
  if (p->stats.error_count && p->diagnostic.message &&
      p->diagnostic.message != p->diagnostic_storage) {
    snprintf(p->diagnostic_storage, sizeof(p->diagnostic_storage), "%s",
             p->diagnostic.message);
    p->diagnostic.message = p->diagnostic_storage;
  }
  regalloc_release_function(&p->functions[f]);
  return ok;
}
int backend_encode_stream(const AstMachineProgram *machine,
                          const AstRegAllocConfig *config,
                          AstEncoderModuleOutput *out, AstRegAllocStats *stats,
                          char *error, size_t capacity) {
  if (!out || !stats || !error || !capacity)
    return 0;
  memset(out, 0, sizeof(*out));
  memset(stats, 0, sizeof(*stats));
  out->failed_function = SIZE_MAX;
  out->diagnostic = AST_ENCODER_DIAG_INVALID_PHYSICAL;
  out->message = error;
  snprintf(error, capacity, "Cannot prepare streamed backend.");
  AstRegAllocProgram *p = regalloc_prepare(machine, config);
  if (!p)
    return 0;
  if (p->stats.error_count) {
    snprintf(error, capacity, "%s", p->diagnostic.message);
    ast_regalloc_destroy(p);
    return 0;
  }
  out->functions = calloc(p->function_count ? p->function_count : 1u,
                          sizeof(*out->functions));
  if (!out->functions) {
    ast_regalloc_destroy(p);
    return 0;
  }
  out->function_count = p->function_count;
  int ok = regalloc_parallel_functions(p, encode_stream_function, out);
  *stats = p->stats;
  if (!ok) {
    size_t failed = p->diagnostic.function_index;
    AstEncoderDiagCode code = AST_ENCODER_DIAG_INVALID_PHYSICAL;
    if (failed < out->function_count &&
        out->functions[failed].diagnostic != AST_ENCODER_DIAG_NONE) {
      code = out->functions[failed].diagnostic;
      snprintf(error, capacity, "%s",
               out->functions[failed].message ? out->functions[failed].message
                                              : "Encoding failed.");
    } else {
      snprintf(error, capacity, "%s", p->diagnostic.message);
    }
    ast_encoder_module_output_destroy(out);
    out->failed_function = failed;
    out->diagnostic = code;
    out->message = error;
  } else {
    out->diagnostic = AST_ENCODER_DIAG_NONE;
    out->message = NULL;
  }
  ast_regalloc_destroy(p);
  return ok;
}

int ast_encoder_function(const AstRegAllocProgram *p, size_t f,
                         AstEncoderOutput *o) {
  return encode_function(p, f, AST_TARGET_ARCH_INVALID, 1, o);
}
int ast_encoder_x86_64_function(const AstRegAllocProgram *p, size_t f,
                                AstEncoderOutput *o) {
  return encode_function(p, f, AST_TARGET_ARCH_X86_64, 1, o);
}
int ast_encoder_aarch64_function(const AstRegAllocProgram *p, size_t f,
                                 AstEncoderOutput *o) {
  return encode_function(p, f, AST_TARGET_ARCH_AARCH64, 1, o);
}
int ast_encoder_riscv64_function(const AstRegAllocProgram *p, size_t f,
                                 AstEncoderOutput *o) {
  return encode_function(p, f, AST_TARGET_ARCH_RISCV64, 1, o);
}

void ast_encoder_module_output_destroy(AstEncoderModuleOutput *o) {
  if (!o)
    return;
  for (size_t f = 0; f < o->function_count; ++f)
    ast_encoder_output_destroy(&o->functions[f]);
  free(o->functions);
  memset(o, 0, sizeof(*o));
}
typedef struct EncodeContext {
  const AstRegAllocProgram *program;
  AstEncoderModuleOutput *out;
} EncodeContext;
static int encode_task(void *opaque, size_t index) {
  EncodeContext *ctx = opaque;
  return encode_function(ctx->program, index, AST_TARGET_ARCH_INVALID, 0,
                         &ctx->out->functions[index]);
}
int ast_encoder_module(const AstRegAllocProgram *p,
                       AstEncoderModuleOutput *out) {
  if (!out)
    return 0;
  memset(out, 0, sizeof(*out));
  out->failed_function = SIZE_MAX;
  out->diagnostic = AST_ENCODER_DIAG_INVALID_PHYSICAL;
  out->message = "Module encoder requires verified finalized Physical IR.";
  if (!p || !ast_physical_verify(p))
    return 0;
  if (p->function_count > SIZE_MAX / sizeof(*out->functions))
    return 0;
  out->functions = (AstEncoderOutput *)calloc(
      p->function_count ? p->function_count : 1u, sizeof(*out->functions));
  if (!out->functions) {
    out->diagnostic = AST_ENCODER_DIAG_OUT_OF_MEMORY;
    out->message = "Cannot allocate module encoding outputs.";
    return 0;
  }
  out->function_count = p->function_count;
  EncodeContext ctx = {p, out};
  size_t f;
  if (!ast_parallel_check(p->function_count, 0u, encode_task, &ctx, &f)) {
    AstEncoderDiagCode code = out->functions[f].diagnostic;
    const char *message = out->functions[f].message;
    ast_encoder_module_output_destroy(out);
    out->failed_function = f;
    out->diagnostic = code;
    out->message = message;
    return 0;
  }
  out->diagnostic = AST_ENCODER_DIAG_NONE;
  out->message = NULL;
  return 1;
}
