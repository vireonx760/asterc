#include "mir_internal.h"

/* Structural post-lowering verifier. Unlike the HIR-fidelity verifier,
 * this validation does not depend on the number, ordering or provenance of
 * source HIR instructions. It verifies the MIR graph in its own right.
 * It deliberately does NOT prove semantic equivalence of transformations. */

typedef struct MirStructuralCheck {
  AstMirProgram *program;
  AstMirFunction *fn;
  AstMirBlockId *def_blocks;
  uint32_t *positions;
  uint8_t *seen_insts;
  uint8_t *seen_vregs;
  uint8_t *seen_memory;
} MirStructuralCheck;

static int structural_error(MirStructuralCheck *c, AstMirDiagCode code,
                            const char *message) {
  mir_program_diag(c->program, code, AST_DIAG_ERROR, c->fn->span, "%s",
                   message);
  return 0;
}

static int structural_dominates(const AstMirFunction *fn,
                                AstMirBlockId dominator, AstMirBlockId block) {
  if (dominator >= fn->block_count || block >= fn->block_count)
    return 0;
  if (!fn->blocks[block].reachable)
    return dominator == block;
  AstMirBlockId current = block;
  for (uint32_t step = 0; step < fn->block_count; ++step) {
    if (current == dominator)
      return 1;
    if (current >= fn->block_count)
      break;
    current = fn->blocks[current].immediate_dominator;
  }
  return 0;
}

static int structural_use(MirStructuralCheck *c, AstMirBlockId block,
                          uint32_t position, AstMirVReg reg) {
  AstMirFunction *fn = c->fn;
  if (reg >= fn->vreg_count)
    return structural_error(c, AST_MIR_DIAG_INVALID_VREG,
                            "structural: operand out of range");
  const AstMirVRegInfo *value = &fn->vregs[reg].info;
  if (!c->seen_vregs[reg])
    return structural_error(c, AST_MIR_DIAG_INVALID_VREG,
                            "structural: operand has no unique definition");
  AstMirBlockId def_block = value->block;
  if (def_block >= fn->block_count)
    return structural_error(c, AST_MIR_DIAG_INVALID_VREG,
                            "structural: defining block out of range");
  if (def_block == block) {
    if (value->kind == AST_MIR_VREG_INSTRUCTION_RESULT) {
      AstMirInstId def = value->definition_index;
      if (def >= fn->instruction_count || c->positions[def] >= position)
        return structural_error(
            c, AST_MIR_DIAG_DOMINANCE,
            "structural: same-block use precedes or equals its definition");
    }
  } else if (!structural_dominates(fn, def_block, block)) {
    return structural_error(c, AST_MIR_DIAG_DOMINANCE,
                            "structural: SSA use is not dominated");
  }
  return 1;
}

/* Structural arity/type contract. It intentionally never consults HIR and
 * therefore remains applicable after deleting or inserting MIR operations. */
static int structural_shape(MirStructuralCheck *c,
                            const AstMirInstruction *inst) {
  uint32_t n = inst->info.operand_count;
  uint32_t min = 0u, max = UINT32_MAX;
  int result_required = 0, result_forbidden = 0;
  switch (inst->info.op) {
  case AST_MIR_OP_CONSTANT:
  case AST_MIR_OP_UNDEF:
  case AST_MIR_OP_STACK_ADDR:
  case AST_MIR_OP_STACK_LOAD:
  case AST_MIR_OP_GLOBAL_ADDR:
  case AST_MIR_OP_GLOBAL_LOAD:
  case AST_MIR_OP_FUNCTION_ADDR:
  case AST_MIR_OP_GENERIC_CONST:
  case AST_MIR_OP_NATIVE_RESULT:
    max = 0u;
    result_required = 1;
    break;
  case AST_MIR_OP_COPY:
  case AST_MIR_OP_UNARY:
  case AST_MIR_OP_CAST:
  case AST_MIR_OP_MEMBER:
  case AST_MIR_OP_ADDRESS_OF:
  case AST_MIR_OP_LOAD:
    min = max = 1u;
    result_required = 1;
    break;
  case AST_MIR_OP_BINARY:
  case AST_MIR_OP_INDEX:
    min = max = 2u;
    result_required = 1;
    break;
  case AST_MIR_OP_SELECT:
    min = max = 3u;
    result_required = 1;
    break;
  case AST_MIR_OP_PACK:
    min = 1u;
    result_required = 1;
    break;
  case AST_MIR_OP_STACK_STORE:
  case AST_MIR_OP_GLOBAL_STORE:
  case AST_MIR_OP_BIND:
    min = max = 1u;
    result_forbidden = 1;
    break;
  case AST_MIR_OP_STORE:
    min = max = 2u;
    result_forbidden = 1;
    break;
  case AST_MIR_OP_FENCE:
    max = 0u;
    result_forbidden = 1;
    break;
  case AST_MIR_OP_CALL_INDIRECT:
  case AST_MIR_OP_ATOMIC:
    min = 1u;
    break;
  case AST_MIR_OP_CALL_DIRECT:
  case AST_MIR_OP_INTRINSIC:
    break;
  case AST_MIR_OP_NATIVE:
    result_forbidden = 1;
    break;
  case AST_MIR_OP_INVALID:
  default:
    return structural_error(c, AST_MIR_DIAG_INVALID_OPERAND,
                            "structural: unsupported instruction opcode");
  }
  if (n < min || n > max ||
      (result_required && inst->info.result == AST_MIR_INVALID_ID) ||
      (result_forbidden && inst->info.result != AST_MIR_INVALID_ID))
    return structural_error(c, AST_MIR_DIAG_INVALID_OPERAND,
                            "structural: instruction arity/result mismatch");
  if (inst->info.op == AST_MIR_OP_COPY && n == 1u &&
      inst->operands[0] < c->fn->vreg_count &&
      !mir_type_equal(inst->info.type,
                      c->fn->vregs[inst->operands[0]].info.type))
    return structural_error(c, AST_MIR_DIAG_TYPE_MISMATCH,
                            "structural: copy changes SSA value type");
  if (inst->info.op == AST_MIR_OP_SELECT && n == 3u &&
      inst->operands[0] < c->fn->vreg_count &&
      ast_sema_type_kind(c->fn->vregs[inst->operands[0]].info.type) !=
          AST_SEMA_TYPE_BOOL)
    return structural_error(c, AST_MIR_DIAG_TYPE_MISMATCH,
                            "structural: select condition is not boolean");
  if (inst->info.op == AST_MIR_OP_SELECT && n == 3u) {
    for (uint32_t i = 1u; i < 3u; ++i)
      if (inst->operands[i] < c->fn->vreg_count &&
          !mir_type_equal(inst->info.type,
                          c->fn->vregs[inst->operands[i]].info.type))
        return structural_error(
            c, AST_MIR_DIAG_TYPE_MISMATCH,
            "structural: select alternative has wrong result type");
  }
  return 1;
}

static int structural_edge(MirStructuralCheck *c, AstMirBlockId from,
                           AstMirBlockId target, const AstMirVReg *args,
                           uint32_t count, uint32_t position) {
  AstMirFunction *fn = c->fn;
  if (target >= fn->block_count)
    return structural_error(c, AST_MIR_DIAG_INVALID_SUCCESSOR,
                            "structural: edge target out of range");
  const AstMirBlock *destination = &fn->blocks[target];
  if (count != destination->argument_count ||
      (count && (!args || !destination->arguments)))
    return structural_error(c, AST_MIR_DIAG_BRANCH_ARGUMENT_MISMATCH,
                            "structural: branch / phi arity mismatch");
  for (uint32_t ai = 0; ai < count; ++ai) {
    if (args[ai] >= fn->vreg_count ||
        destination->arguments[ai] >= fn->vreg_count)
      return structural_error(c, AST_MIR_DIAG_INVALID_VREG,
                              "structural: edge operand out of range");
    if (!mir_type_equal(fn->vregs[args[ai]].info.type,
                        fn->vregs[destination->arguments[ai]].info.type))
      return structural_error(c, AST_MIR_DIAG_TYPE_MISMATCH,
                              "structural: edge argument type mismatch");
    if (!structural_use(c, from, position, args[ai]))
      return 0;
  }
  return 1;
}

static void structural_destroy_cfg(AstMirFunction *copy) {
  if (!copy->blocks)
    return;
  for (uint32_t b = 0; b < copy->block_count; ++b)
    free(copy->blocks[b].predecessors);
  free(copy->blocks);
}

static int structural_cfg(MirStructuralCheck *c) {
  AstMirFunction *fn = c->fn;
  AstMirFunction expected = {0};
  expected.block_count = fn->block_count;
  expected.entry_block = fn->entry_block;
  expected.blocks =
      (AstMirBlock *)calloc(fn->block_count, sizeof(*expected.blocks));
  if (!expected.blocks)
    return 0;
  for (uint32_t b = 0; b < fn->block_count; ++b) {
    expected.blocks[b].id = b;
    expected.blocks[b].term.info.kind = fn->blocks[b].term.info.kind;
    expected.blocks[b].term.info.target_true =
        fn->blocks[b].term.info.target_true;
    expected.blocks[b].term.info.target_false =
        fn->blocks[b].term.info.target_false;
  }
  int ok = mir_analyze_cfg(&expected);
  if (ok) {
    for (uint32_t b = 0; b < fn->block_count && ok; ++b) {
      const AstMirBlock *a = &fn->blocks[b], *e = &expected.blocks[b];
      if (a->id != b || a->predecessor_count != e->predecessor_count ||
          a->successor_count != e->successor_count ||
          a->immediate_dominator != e->immediate_dominator ||
          a->reverse_postorder != e->reverse_postorder ||
          a->loop_depth != e->loop_depth || a->reachable != e->reachable ||
          (a->predecessor_count && !a->predecessors)) {
        ok = 0;
        break;
      }
      for (uint32_t i = 0; i < a->predecessor_count; ++i)
        if (a->predecessors[i] != e->predecessors[i])
          ok = 0;
      for (uint32_t i = 0; i < a->successor_count; ++i)
        if (a->successors[i] != e->successors[i])
          ok = 0;
    }
  }
  structural_destroy_cfg(&expected);
  if (!ok)
    return structural_error(
        c, AST_MIR_DIAG_INVALID_CFG,
        "structural: CFG analysis does not match stored metadata");
  return 1;
}

int mir_verify_structural_function(AstMirProgram *program, AstMirFunction *fn) {
  if (!fn || !fn->type)
    return 0;
  if (fn->flags & (AST_HIR_FUNCTION_EXTERN | AST_HIR_FUNCTION_DISPATCH))
    return fn->block_count == 0 && fn->instruction_count == 0 &&
           fn->vreg_count == 0 && fn->memory_value_count == 0;
  MirStructuralCheck c = {0};
  c.program = program;
  c.fn = fn;
  if (fn->inline_site_count && !fn->inline_sites)
    return structural_error(&c, AST_MIR_DIAG_INVALID_CFG,
                            "structural: missing inline source sites");
  for (uint32_t s = 0; s < fn->inline_site_count; ++s)
    if (fn->inline_sites[s].parent > s || !fn->inline_sites[s].callee_name.text)
      return structural_error(&c, AST_MIR_DIAG_INVALID_CFG,
                              "structural: invalid inline source chain");
  if (!fn->block_count || fn->entry_block >= fn->block_count || !fn->blocks ||
      (fn->instruction_count && !fn->instructions) ||
      (fn->vreg_count && !fn->vregs) ||
      (fn->memory_value_count && !fn->memory_values))
    return structural_error(&c, AST_MIR_DIAG_INVALID_CFG,
                            "structural: incomplete function IR tables");
  if (!structural_cfg(&c))
    return 0;
  size_t ni = fn->instruction_count ? fn->instruction_count : 1u;
  size_t nv = fn->vreg_count ? fn->vreg_count : 1u;
  size_t nm = fn->memory_value_count ? fn->memory_value_count : 1u;
  c.def_blocks = (AstMirBlockId *)malloc(ni * sizeof(*c.def_blocks));
  c.positions = (uint32_t *)malloc(ni * sizeof(*c.positions));
  c.seen_insts = (uint8_t *)calloc(ni, 1u);
  c.seen_vregs = (uint8_t *)calloc(nv, 1u);
  c.seen_memory = (uint8_t *)calloc(nm, 1u);
  int ok = c.def_blocks && c.positions && c.seen_insts && c.seen_vregs &&
           c.seen_memory;
  if (!ok)
    goto done;
  for (uint32_t i = 0; i < fn->instruction_count; ++i) {
    c.def_blocks[i] = AST_MIR_INVALID_ID;
    c.positions[i] = UINT32_MAX;
  }
  /* Pass 1: identify *all* definitions before checking uses. */
  for (uint32_t b = 0; b < fn->block_count && ok; ++b) {
    const AstMirBlock *block = &fn->blocks[b];
    if ((block->argument_count &&
         (!block->arguments || !block->argument_source_locals)) ||
        block->hir_argument_count > block->argument_count ||
        (block->instruction_count && !block->instructions)) {
      ok = structural_error(
          &c, AST_MIR_DIAG_INVALID_CFG,
          "structural: missing block argument/instruction table");
      break;
    }
    for (uint32_t ai = 0; ai < block->argument_count && ok; ++ai) {
      AstMirVReg r = block->arguments[ai];
      if (r >= fn->vreg_count || c.seen_vregs[r] ||
          fn->vregs[r].info.kind != AST_MIR_VREG_BLOCK_ARGUMENT ||
          fn->vregs[r].info.block != b ||
          fn->vregs[r].info.definition_index != ai || !fn->vregs[r].info.type) {
        ok = structural_error(
            &c, AST_MIR_DIAG_INVALID_VREG,
            "structural: invalid or duplicated block argument");
        break;
      }
      c.seen_vregs[r] = 1u;
    }
    for (uint32_t p = 0; p < block->instruction_count && ok; ++p) {
      AstMirInstId id = block->instructions[p];
      if (id >= fn->instruction_count || c.seen_insts[id] ||
          fn->instructions[id].info.id != id) {
        ok = structural_error(
            &c, AST_MIR_DIAG_INVALID_CFG,
            "structural: duplicated or invalid instruction ID");
        break;
      }
      c.seen_insts[id] = 1u;
      c.def_blocks[id] = b;
      c.positions[id] = p;
      const AstMirInstruction *inst = &fn->instructions[id];
      if (inst->info.inline_site > fn->inline_site_count) {
        ok = structural_error(&c, AST_MIR_DIAG_INVALID_CFG,
                              "structural: invalid inline source site");
        break;
      }
      if (inst->info.result != AST_MIR_INVALID_ID) {
        AstMirVReg r = inst->info.result;
        if (r >= fn->vreg_count || c.seen_vregs[r] ||
            fn->vregs[r].info.kind != AST_MIR_VREG_INSTRUCTION_RESULT ||
            fn->vregs[r].info.block != b ||
            fn->vregs[r].info.definition_index != id ||
            !mir_type_equal(fn->vregs[r].info.type, inst->info.type)) {
          ok = structural_error(
              &c, AST_MIR_DIAG_INVALID_VREG,
              "structural: invalid or duplicated instruction result");
          break;
        }
        c.seen_vregs[r] = 1u;
      }
    }
  }
  for (uint32_t i = 0; i < fn->instruction_count && ok; ++i)
    if (!c.seen_insts[i])
      ok = structural_error(&c, AST_MIR_DIAG_INVALID_CFG,
                            "structural: orphan instruction");
  for (uint32_t i = 0; i < fn->vreg_count && ok; ++i)
    if (!c.seen_vregs[i])
      ok = structural_error(&c, AST_MIR_DIAG_INVALID_VREG,
                            "structural: orphan SSA value");
  if (!ok)
    goto done;
  /* Pass 2: verify uses, memory effects, and all control-flow edges. */
  for (uint32_t b = 0; b < fn->block_count && ok; ++b) {
    const AstMirBlock *block = &fn->blocks[b];
    AstMirMemoryId memory = block->memory_argument;
    if (memory >= fn->memory_value_count) {
      ok = structural_error(&c, AST_MIR_DIAG_INVALID_MEMORY_CHAIN,
                            "structural: missing block memory argument");
      break;
    }
    const AstMirMemoryInfo *m = &fn->memory_values[memory].info;
    if (c.seen_memory[memory] || m->block != b ||
        (m->kind != AST_MIR_MEMORY_ENTRY &&
         m->kind != AST_MIR_MEMORY_BLOCK_ARGUMENT) ||
        (m->kind == AST_MIR_MEMORY_ENTRY && b != fn->entry_block) ||
        (m->kind == AST_MIR_MEMORY_BLOCK_ARGUMENT &&
         (m->incoming_count != block->predecessor_count ||
          (m->incoming_count && !m->incoming)))) {
      ok = structural_error(&c, AST_MIR_DIAG_INVALID_MEMORY_CHAIN,
                            "structural: malformed memory block state");
      break;
    }
    c.seen_memory[memory] = 1u;
    if (m->kind == AST_MIR_MEMORY_BLOCK_ARGUMENT) {
      for (uint32_t pi = 0; pi < m->incoming_count; ++pi) {
        AstMirBlockId pred = block->predecessors[pi];
        if (pred >= fn->block_count ||
            m->incoming[pi] != fn->blocks[pred].term.info.memory) {
          ok = structural_error(&c, AST_MIR_DIAG_INVALID_MEMORY_CHAIN,
                                "structural: predecessor memory phi mismatch");
          break;
        }
      }
      if (!ok)
        break;
    }
    for (uint32_t p = 0; p < block->instruction_count && ok; ++p) {
      AstMirInstId id = block->instructions[p];
      const AstMirInstruction *inst = &fn->instructions[id];
      if (inst->info.operands != inst->operands ||
          (inst->info.operand_count && !inst->operands) ||
          (inst->info.result != AST_MIR_INVALID_ID && !inst->info.type)) {
        ok = structural_error(&c, AST_MIR_DIAG_INVALID_OPERAND,
                              "structural: malformed operand layout");
        break;
      }
      if (!structural_shape(&c, inst)) {
        ok = 0;
        break;
      }
      uint32_t expected_flags =
          mir_instruction_flags(inst->info.effects, inst->info.op) |
          (inst->info.flags & AST_MIR_INST_SYNTHETIC);
      if (expected_flags != inst->info.flags ||
          ((inst->info.op == AST_MIR_OP_LOAD ||
            inst->info.op == AST_MIR_OP_STACK_LOAD ||
            inst->info.op == AST_MIR_OP_GLOBAL_LOAD) &&
           !(inst->info.effects & AST_SEMA_EFFECT_READ_MEMORY)) ||
          ((inst->info.op == AST_MIR_OP_STORE ||
            inst->info.op == AST_MIR_OP_STACK_STORE ||
            inst->info.op == AST_MIR_OP_GLOBAL_STORE) &&
           !(inst->info.effects & AST_SEMA_EFFECT_WRITE_MEMORY))) {
        ok = structural_error(
            &c, AST_MIR_DIAG_INVALID_EFFECTS,
            "structural: effect flags disagree with instruction");
        break;
      }
      for (uint32_t oi = 0; oi < inst->info.operand_count && ok; ++oi)
        ok = structural_use(&c, b, p, inst->operands[oi]);
      if (!ok)
        break;
      if (mir_instruction_is_effectful(inst)) {
        AstMirMemoryId next = inst->info.memory_out;
        if (inst->info.memory_in != memory || next >= fn->memory_value_count ||
            c.seen_memory[next] ||
            fn->memory_values[next].info.kind !=
                AST_MIR_MEMORY_INSTRUCTION_RESULT ||
            fn->memory_values[next].info.block != b ||
            fn->memory_values[next].info.definition_index != id ||
            fn->memory_values[next].info.incoming_count != 0u) {
          ok = structural_error(&c, AST_MIR_DIAG_INVALID_MEMORY_CHAIN,
                                "structural: invalid effect chain transition");
          break;
        }
        c.seen_memory[next] = 1u;
        memory = next;
      } else if (inst->info.memory_in != AST_MIR_INVALID_ID ||
                 inst->info.memory_out != AST_MIR_INVALID_ID) {
        ok = structural_error(
            &c, AST_MIR_DIAG_INVALID_MEMORY_CHAIN,
            "structural: pure operation consumes memory chain");
        break;
      }
    }
    if (!ok)
      break;
    const AstMirTerminatorInfo *term = &block->term.info;
    if (term->memory != memory) {
      ok = structural_error(&c, AST_MIR_DIAG_INVALID_MEMORY_CHAIN,
                            "structural: terminator has wrong memory state");
      break;
    }
    uint32_t end = block->instruction_count;
    if (term->kind == AST_MIR_TERM_BR || term->kind == AST_MIR_TERM_COND_BR) {
      ok = structural_edge(&c, b, term->target_true, term->true_args,
                           term->true_arg_count, end);
      if (ok && term->kind == AST_MIR_TERM_COND_BR)
        ok = structural_edge(&c, b, term->target_false, term->false_args,
                             term->false_arg_count, end);
    } else if (term->kind != AST_MIR_TERM_RETURN &&
               term->kind != AST_MIR_TERM_UNREACHABLE) {
      ok = structural_error(&c, AST_MIR_DIAG_MISSING_TERMINATOR,
                            "structural: missing/invalid terminator");
    }
    if (ok && term->condition_or_value != AST_MIR_INVALID_ID)
      ok = structural_use(&c, b, end, term->condition_or_value);
    if (ok && term->kind == AST_MIR_TERM_COND_BR &&
        (term->condition_or_value >= fn->vreg_count ||
         ast_sema_type_kind(fn->vregs[term->condition_or_value].info.type) !=
             AST_SEMA_TYPE_BOOL))
      ok = structural_error(&c, AST_MIR_DIAG_TYPE_MISMATCH,
                            "structural: conditional branch expects bool");
    if (ok && term->kind == AST_MIR_TERM_RETURN) {
      const AstSemaType *result = ast_sema_type_function_result(fn->type);
      AstMirVReg value = term->condition_or_value;
      int is_void = result && ast_sema_type_kind(result) == AST_SEMA_TYPE_VOID;
      if (!result || (is_void && value != AST_MIR_INVALID_ID) ||
          (!is_void && (value >= fn->vreg_count ||
                        !mir_type_equal(result, fn->vregs[value].info.type))))
        ok = structural_error(
            &c, AST_MIR_DIAG_TYPE_MISMATCH,
            "structural: return value disagrees with function result");
    }
  }
  for (uint32_t i = 0; i < fn->memory_value_count && ok; ++i)
    if (!c.seen_memory[i])
      ok = structural_error(&c, AST_MIR_DIAG_INVALID_MEMORY_CHAIN,
                            "structural: orphan memory state");

  if (ok)
    ok = mir_verify_canonical_liveness(program, fn);
done:
  free(c.def_blocks);
  free(c.positions);
  free(c.seen_insts);
  free(c.seen_vregs);
  free(c.seen_memory);
  return ok;
}

int ast_mir_verify_structural(AstMirProgram *program) {
  if (!program || (program->function_count && !program->functions))
    return 0;
  int ok = 1;
  for (size_t f = 0; f < program->function_count; ++f)
    if (!mir_verify_structural_function(program, program->functions[f]))
      ok = 0;
  return ok;
}
