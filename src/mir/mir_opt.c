#include "mir_internal.h"

/* Equality is deliberately exact.  No commutation, FP reassociation, alias
 * reasoning or integer-overflow transformations are admissible here. */
int mir_opt_gvn_match(const AstMirInstruction *a, const AstMirInstruction *b) {
  if (!a || !b || a->info.op != b->info.op ||
      a->info.result == AST_MIR_INVALID_ID ||
      b->info.result == AST_MIR_INVALID_ID || a->info.effects ||
      b->info.effects || a->info.flags || b->info.flags ||
      a->info.operand_count != b->info.operand_count ||
      !mir_type_equal(a->info.type, b->info.type) ||
      a->info.token_op != b->info.token_op ||
      a->info.intrinsic != b->info.intrinsic ||
      a->info.memory_order != b->info.memory_order ||
      a->info.failure_memory_order != b->info.failure_memory_order)
    return 0;
  switch (a->info.op) {
  case AST_MIR_OP_UNARY:
  case AST_MIR_OP_BINARY:
  case AST_MIR_OP_CAST:
  case AST_MIR_OP_SELECT:
    break;
  default:
    return 0;
  }
  /* Strict scalar semantics: FP status/NaNs, pointers, capabilities,
   * vector masks and any undef/poison-like virtual register are excluded. */
  AstSemaTypeKind result_kind = ast_sema_type_kind(a->info.type);
  if (result_kind != AST_SEMA_TYPE_INT && result_kind != AST_SEMA_TYPE_BITS &&
      result_kind != AST_SEMA_TYPE_BOOL)
    return 0;
  if (a->info.operand_count == 0u || !a->operands || !b->operands)
    return 0;
  for (uint32_t i = 0u; i < a->info.operand_count; ++i)
    if (a->operands[i] != b->operands[i])
      return 0;
  return 1;
}

/* Only algebraic *neutral* elements with exact integer semantics.
 * Never fold x*0 (could hide trapping/poison operand or effectful evaluation).
 */
int mir_opt_identity_match(const AstMirFunction *fn,
                           const AstMirInstruction *original,
                           const AstMirInstruction *constant,
                           AstMirVReg *kept) {
  if (!fn || !original || !constant || !kept ||
      original->info.op != AST_MIR_OP_BINARY ||
      original->info.operand_count != 2u || !original->operands ||
      original->info.result >= fn->vreg_count || original->info.flags ||
      original->info.effects || constant->info.op != AST_MIR_OP_CONSTANT ||
      constant->info.flags || constant->info.effects ||
      constant->info.constant.kind != AST_SEMA_CONST_INTEGER ||
      constant->info.constant.hi != 0u || constant->info.constant.is_negative ||
      constant->info.result >= fn->vreg_count)
    return 0;
  AstSemaTypeKind kind = ast_sema_type_kind(original->info.type);
  if (kind != AST_SEMA_TYPE_INT && kind != AST_SEMA_TYPE_BITS)
    return 0;
  AstTokenKind token = original->info.token_op;
  uint64_t neutral;
  int commutative = 1;
  if (token == AST_TOK_PLUS || token == AST_TOK_CARET || token == AST_TOK_PIPE)
    neutral = 0u;
  else if (token == AST_TOK_STAR)
    neutral = 1u;
  else if (token == AST_TOK_MINUS) {
    neutral = 0u;
    commutative = 0;
  } else
    return 0;
  if (constant->info.constant.lo != neutral)
    return 0;
  for (uint32_t i = 0u; i < (commutative ? 2u : 1u); ++i) {
    uint32_t c = i == 0u ? 1u : 0u;
    uint32_t x = c ^ 1u;
    AstMirVReg cv = original->operands[c];
    AstMirVReg xv = original->operands[x];
    if (cv == constant->info.result && xv < fn->vreg_count &&
        !(fn->vregs[xv].info.flags & AST_MIR_VREG_UNDEFINED) &&
        mir_type_equal(fn->vregs[xv].info.type, original->info.type) &&
        mir_type_equal(fn->vregs[cv].info.type, original->info.type)) {
      *kept = xv;
      return 1;
    }
  }
  return 0;
}

/* Width-safe, conservative constant transfer.  This is deliberately a
 * small SCCP building block rather than an assertion that the CFG is solved:
 * only unsigned 8/16/32/64-bit operations with constant SSA operands can
 * move to the constant lattice state. Division needs a nonzero divisor; shift
 * counts are bounded to the operand width (never rely on host shift UB).
 * No trapping expression, signed overflow, casts, poison, floating point or
 * alias-sensitive instruction is evaluated.
 * The verifier independently calls this routine on the saved original IR. */
int mir_opt_fold_unsigned(const AstMirFunction *fn,
                          const AstMirInstruction *original,
                          AstSemaConstInfo *out) {
  if (!fn || !original || !out || original->info.op != AST_MIR_OP_BINARY ||
      original->info.effects || original->info.flags ||
      original->info.operand_count != 2u || !original->operands ||
      original->info.result >= fn->vreg_count ||
      ast_sema_type_is_signed(original->info.type))
    return 0;
  AstSemaTypeKind kind = ast_sema_type_kind(original->info.type);
  if (kind != AST_SEMA_TYPE_INT && kind != AST_SEMA_TYPE_BITS)
    return 0;
  uint32_t bits = mir_type_bit_width(original->info.type);
  if (bits != 8u && bits != 16u && bits != 32u && bits != 64u)
    return 0;
  uint64_t operands[2];
  uint64_t mask = bits == 64u ? UINT64_MAX : ((UINT64_C(1) << bits) - 1u);
  for (uint32_t i = 0u; i < 2u; ++i) {
    AstMirVReg vr = original->operands[i];
    if (vr >= fn->vreg_count ||
        (fn->vregs[vr].info.flags & AST_MIR_VREG_UNDEFINED) ||
        !mir_type_equal(fn->vregs[vr].info.type, original->info.type) ||
        fn->vregs[vr].info.kind != AST_MIR_VREG_INSTRUCTION_RESULT)
      return 0;
    AstMirInstId definition = fn->vregs[vr].info.definition_index;
    if (definition >= fn->instruction_count)
      return 0;
    const AstMirInstruction *constant = &fn->instructions[definition];
    if (constant->info.op != AST_MIR_OP_CONSTANT || constant->info.flags ||
        constant->info.effects || constant->info.result != vr ||
        constant->info.constant.kind != AST_SEMA_CONST_INTEGER ||
        constant->info.constant.is_negative || constant->info.constant.hi ||
        (constant->info.constant.lo & ~mask))
      return 0;
    /* Constant propagation across previously certified folds is safe
     * only when every intermediate SSA definition has an independently
     * checked constant-transfer certificate.  We deliberately reject
     * COPY/GVN witnesses, which do not prove a constant value here.
     * The strict MIR verifier proves the original SSA operands dominate
     * their uses and validates *all* certificates, including dependencies.
     * Thus the dependency graph is acyclic along the original SSA edges. */
    if (fn->gvn_certificates && fn->gvn_certificates[definition].valid &&
        fn->gvn_certificates[definition].valid != 3u)
      return 0;
    operands[i] = constant->info.constant.lo;
  }
  uint64_t result;
  switch (original->info.token_op) {
  case AST_TOK_PLUS:
    result = operands[0] + operands[1];
    break;
  case AST_TOK_MINUS:
    result = operands[0] - operands[1];
    break;
  case AST_TOK_STAR:
    result = operands[0] * operands[1];
    break;
  case AST_TOK_AMP:
    result = operands[0] & operands[1];
    break;
  case AST_TOK_PIPE:
    result = operands[0] | operands[1];
    break;
  case AST_TOK_CARET:
    result = operands[0] ^ operands[1];
    break;
  case AST_TOK_SLASH:
    if (operands[1] == 0u)
      return 0;
    result = operands[0] / operands[1];
    break;
  case AST_TOK_PERCENT:
    if (operands[1] == 0u)
      return 0;
    result = operands[0] % operands[1];
    break;
  case AST_TOK_SHIFT_LEFT:
    if (operands[1] >= bits)
      return 0;
    result = (operands[0] << operands[1]);
    break;
  case AST_TOK_SHIFT_RIGHT:
    if (operands[1] >= bits)
      return 0;
    result = (operands[0] >> operands[1]);
    break;
  default:
    return 0;
  }
  memset(out, 0, sizeof(*out));
  out->kind = AST_SEMA_CONST_INTEGER;
  out->lo = result & mask;
  return 1;
}

/* Replace only an already HIR-verified pure instruction.  The source
 * operands survive in a separately owned immutable optimization certificate. */
static int mir_opt_install_copy(AstMirFunction *fn, AstMirInstId id,
                                AstMirInstId witness, AstMirVReg kept,
                                uint8_t kind) {
  AstMirInstruction *inst = &fn->instructions[id];
  size_t bytes = (size_t)inst->info.operand_count * sizeof(AstMirVReg);
  AstMirVReg *saved = (AstMirVReg *)malloc(bytes);
  if (!saved)
    return 0;
  memcpy(saved, inst->operands, bytes);
  MirGvnCertificate *cert = &fn->gvn_certificates[id];
  cert->original = *inst;
  cert->original.operands = saved;
  cert->original.info.operands = saved;
  cert->witness = witness;
  cert->valid = kind;
  inst->operands[0] = kept;
  inst->info.operand_count = 1u;
  inst->info.op = AST_MIR_OP_COPY;
  return 1;
}

/* The constant must carry the original instruction's HIR identity and
 * operand snapshot.  We never discard the source computation or its proof. */
static int mir_opt_install_constant_kind(AstMirFunction *fn, AstMirInstId id,
                                         AstSemaConstInfo value,
                                         uint8_t proof_kind) {
  AstMirInstruction *inst = &fn->instructions[id];
  size_t bytes = (size_t)inst->info.operand_count * sizeof(AstMirVReg);
  AstMirVReg *saved = (AstMirVReg *)malloc(bytes);
  if (!saved)
    return 0;
  memcpy(saved, inst->operands, bytes);
  MirGvnCertificate *cert = &fn->gvn_certificates[id];
  cert->original = *inst;
  cert->original.operands = saved;
  cert->original.info.operands = saved;
  cert->witness = AST_MIR_INVALID_ID;
  cert->valid = proof_kind;
  inst->info.op = AST_MIR_OP_CONSTANT;
  inst->info.operand_count = 0u;
  inst->info.constant = value;
  return 1;
}

static int mir_opt_install_constant(AstMirFunction *fn, AstMirInstId id,
                                    AstSemaConstInfo value) {
  return mir_opt_install_constant_kind(fn, id, value, 3u);
}

/* Canonical CFG dominance is available only after strict ast_mir_verify().
 * The entry block dominates itself, but the witness must be ordered earlier
 * within the same block. */
static int mir_opt_dominates(const AstMirFunction *fn, AstMirBlockId definition,
                             AstMirBlockId use) {
  if (definition >= fn->block_count || use >= fn->block_count ||
      !fn->blocks[definition].reachable || !fn->blocks[use].reachable)
    return 0;
  AstMirBlockId cursor = use;
  for (uint32_t i = 0; i < fn->block_count; ++i) {
    if (cursor == definition)
      return 1;
    if (cursor == AST_MIR_INVALID_ID || cursor >= fn->block_count)
      break;
    cursor = fn->blocks[cursor].immediate_dominator;
  }
  return 0;
}

/* A fixed-seed, open hash-bucket index avoids an O(N^2) walk over the whole
 * function. Collision resolution remains exact via mir_opt_gvn_match(). */
typedef struct MirOptIndex {
  AstMirInstId *heads;
  AstMirInstId *next;
  uint32_t *positions;
  AstMirBlockId *order;
  size_t capacity;
} MirOptIndex;

static uint64_t mir_opt_mix(uint64_t hash, uint64_t value) {
  hash ^= value + UINT64_C(0x9e3779b97f4a7c15) + (hash << 6u) + (hash >> 2u);
  return hash;
}

static uint64_t mir_opt_expr_hash(const AstMirInstruction *inst) {
  uint64_t hash = UINT64_C(0x243f6a8885a308d3);
  hash = mir_opt_mix(hash, (uint32_t)inst->info.op);
  hash = mir_opt_mix(hash, (uint32_t)inst->info.token_op);
  hash = mir_opt_mix(hash, (uint32_t)inst->info.intrinsic);
  hash = mir_opt_mix(hash, inst->info.operand_count);
  hash = mir_opt_mix(hash, (uint32_t)ast_sema_type_kind(inst->info.type));
  hash = mir_opt_mix(hash, mir_type_bit_width(inst->info.type));
  for (uint32_t i = 0u; i < inst->info.operand_count; ++i)
    hash = mir_opt_mix(hash, inst->operands[i]);
  return hash;
}

static int mir_opt_safe_scalar(const AstMirFunction *fn,
                               const AstMirInstruction *inst) {
  if (!inst || inst->info.effects || inst->info.flags ||
      inst->info.result >= fn->vreg_count || !inst->operands ||
      inst->info.operand_count == 0u)
    return 0;
  for (uint32_t oi = 0u; oi < inst->info.operand_count; ++oi) {
    AstMirVReg vr = inst->operands[oi];
    if (vr >= fn->vreg_count ||
        (fn->vregs[vr].info.flags & AST_MIR_VREG_UNDEFINED))
      return 0;
    AstSemaTypeKind kind = ast_sema_type_kind(fn->vregs[vr].info.type);
    if (kind != AST_SEMA_TYPE_INT && kind != AST_SEMA_TYPE_BITS &&
        kind != AST_SEMA_TYPE_BOOL)
      return 0;
  }
  return 1;
}

static void mir_opt_index_destroy(MirOptIndex *index) {
  free(index->heads);
  free(index->next);
  free(index->positions);
  free(index->order);
  memset(index, 0, sizeof(*index));
}

static int mir_opt_index_create(const AstMirFunction *fn, MirOptIndex *index) {
  memset(index, 0, sizeof(*index));
  size_t count = fn->instruction_count;
  if (count > UINT32_MAX / 2u)
    return 0;
  size_t capacity = 32u;
  while (capacity < count * 2u)
    capacity *= 2u;
  index->heads = (AstMirInstId *)malloc(capacity * sizeof(*index->heads));
  index->next =
      (AstMirInstId *)malloc((count ? count : 1u) * sizeof(*index->next));
  index->positions =
      (uint32_t *)malloc((count ? count : 1u) * sizeof(*index->positions));
  index->order = (AstMirBlockId *)malloc(
      (fn->block_count ? fn->block_count : 1u) * sizeof(*index->order));
  if (!index->heads || !index->next || !index->positions || !index->order) {
    mir_opt_index_destroy(index);
    return 0;
  }
  index->capacity = capacity;
  for (size_t i = 0u; i < capacity; ++i)
    index->heads[i] = AST_MIR_INVALID_ID;
  for (size_t i = 0u; i < count; ++i) {
    index->next[i] = AST_MIR_INVALID_ID;
    index->positions[i] = UINT32_MAX;
  }
  for (uint32_t bi = 0u; bi < fn->block_count; ++bi)
    index->order[bi] = AST_MIR_INVALID_ID;
  for (uint32_t bi = 0u; bi < fn->block_count; ++bi) {
    const AstMirBlock *block = &fn->blocks[bi];
    if (block->reverse_postorder >= fn->block_count ||
        index->order[block->reverse_postorder] != AST_MIR_INVALID_ID)
      return 0;
    index->order[block->reverse_postorder] = bi;
    for (uint32_t ii = 0u; ii < block->instruction_count; ++ii) {
      AstMirInstId id = block->instructions[ii];
      if (id >= count || index->positions[id] != UINT32_MAX)
        return 0;
      index->positions[id] = ii;
    }
  }
  return 1;
}

/* Failed verification or allocation must leave the strict MIR unchanged. */
static void mir_opt_rollback(AstMirProgram *program) {
  for (size_t fi = 0u; fi < program->function_count; ++fi) {
    AstMirFunction *fn = program->functions[fi];
    if (!fn->gvn_certificates)
      continue;
    for (uint32_t i = 0u; i < fn->instruction_count; ++i) {
      MirGvnCertificate *cert = &fn->gvn_certificates[i];
      if (!cert->valid)
        continue;
      AstMirInstruction *inst = &fn->instructions[i];
      uint32_t count = cert->original.info.operand_count;
      memcpy(inst->operands, cert->original.operands,
             (size_t)count * sizeof(AstMirVReg));
      inst->info = cert->original.info;
      inst->info.operands = inst->operands;
      free(cert->original.operands);
    }
    free(fn->gvn_certificates);
    fn->gvn_certificates = NULL;
    (void)mir_analyze_liveness(fn);
  }
}

static int mir_opt_indexed_function(AstMirFunction *fn,
                                    AstMirOptimizeStats *stats) {
  MirOptIndex index;
  if (!mir_opt_index_create(fn, &index)) {
    mir_opt_index_destroy(&index);
    return 0;
  }
  int success = 1;
  for (uint32_t rpo = 0u; success && rpo < fn->block_count; ++rpo) {
    AstMirBlockId bi = index.order[rpo];
    if (bi >= fn->block_count) {
      success = 0;
      break;
    }
    const AstMirBlock *block = &fn->blocks[bi];
    for (uint32_t j = 0u; j < block->instruction_count; ++j) {
      AstMirInstId id = block->instructions[j];
      AstMirInstruction *inst = &fn->instructions[id];
      stats->examined++;
      if (inst->info.flags || inst->info.effects) {
        stats->skipped_effectful++;
        continue;
      }
      if (!block->reachable || !mir_opt_safe_scalar(fn, inst))
        continue;
      AstSemaConstInfo folded;
      if (mir_opt_fold_unsigned(fn, inst, &folded)) {
        if (!mir_opt_install_constant(fn, id, folded)) {
          success = 0;
          break;
        }
        stats->rewritten++;
        stats->constant_folded++;
        continue;
      }
      AstMirInstId witness = AST_MIR_INVALID_ID;
      AstMirVReg kept = AST_MIR_INVALID_ID;
      uint8_t kind = 0u;
      /* Source-typed identity constant is always a direct SSA operand.
       * Resolve its definition in O(1), without scanning all constants. */
      if (!kind && inst->info.op == AST_MIR_OP_BINARY &&
          inst->info.operand_count == 2u) {
        for (uint32_t oi = 0u; oi < 2u; ++oi) {
          const AstMirVRegData *vreg = &fn->vregs[inst->operands[oi]];
          AstMirInstId definition = vreg->info.definition_index;
          if (vreg->info.kind != AST_MIR_VREG_INSTRUCTION_RESULT ||
              definition >= fn->instruction_count)
            continue;
          AstMirBlockId wb = vreg->info.block;
          if (!mir_opt_dominates(fn, wb, bi) ||
              (wb == bi && index.positions[definition] >= j))
            continue;
          if (!fn->gvn_certificates[definition].valid &&
              mir_opt_identity_match(fn, inst, &fn->instructions[definition],
                                     &kept)) {
            witness = definition;
            kind = 2u;
            break;
          }
        }
      }
      if (!kind) {
        uint64_t hash = mir_opt_expr_hash(inst);
        size_t bucket = (size_t)hash & (index.capacity - 1u);
        AstMirInstId candidate_id = index.heads[bucket];
        while (candidate_id != AST_MIR_INVALID_ID) {
          const AstMirInstruction *candidate = &fn->instructions[candidate_id];
          AstMirVReg result = candidate->info.result;
          AstMirBlockId wb = fn->vregs[result].info.block;
          if (mir_opt_dominates(fn, wb, bi) &&
              (wb != bi || index.positions[candidate_id] < j) &&
              mir_opt_gvn_match(inst, candidate)) {
            kept = result;
            witness = candidate_id;
            kind = 1u;
            break;
          }
          candidate_id = index.next[candidate_id];
        }
        if (!kind) {
          /* Do not index expressions that cannot be rewritten. */
          if (inst->info.op == AST_MIR_OP_BINARY ||
              inst->info.op == AST_MIR_OP_UNARY ||
              inst->info.op == AST_MIR_OP_CAST ||
              inst->info.op == AST_MIR_OP_SELECT) {
            index.next[id] = index.heads[bucket];
            index.heads[bucket] = id;
          }
        }
      }
      if (kind) {
        if (!mir_opt_install_copy(fn, id, witness, kept, kind)) {
          success = 0;
          break;
        }
        stats->rewritten++;
        if (fn->vregs[fn->instructions[witness].info.result].info.block != bi)
          stats->cross_block_rewritten++;
      }
    }
  }
  mir_opt_index_destroy(&index);
  return success && mir_analyze_liveness(fn);
}

int ast_mir_optimize_local_gvn(AstMirProgram *program,
                               AstMirOptimizeStats *stats) {
  AstMirOptimizeStats counts = {0};
  if (!program || !ast_mir_verify_mode(program, AST_MIR_VERIFY_PRE_OPT))
    return 0;
  counts.verifier_runs++;
  for (size_t fi = 0u; fi < program->function_count; ++fi)
    if (program->functions[fi]->gvn_certificates)
      return 0;
  /* Allocate certificate tables before any MIR mutation. */
  for (size_t fi = 0u; fi < program->function_count; ++fi) {
    AstMirFunction *fn = program->functions[fi];
    if (!fn->instruction_count)
      continue;
    fn->gvn_certificates = (MirGvnCertificate *)calloc(
        fn->instruction_count, sizeof(*fn->gvn_certificates));
    if (!fn->gvn_certificates) {
      mir_opt_rollback(program);
      return 0;
    }
  }
  for (size_t fi = 0u; fi < program->function_count; ++fi) {
    if (!mir_opt_indexed_function(program->functions[fi], &counts)) {
      mir_opt_rollback(program);
      return 0;
    }
  }
  if (!ast_mir_verify_mode(program, AST_MIR_VERIFY_POST_OPT)) {
    mir_opt_rollback(program);
    (void)ast_mir_verify(program);
    return 0;
  }
  counts.verifier_runs++;
  if (stats)
    *stats = counts;
  return 1;
}

/* Preferred spelling for the combined scalar pass. */
int ast_mir_optimize_scalar(AstMirProgram *program,
                            AstMirOptimizeStats *stats) {
  return ast_mir_optimize_local_gvn(program, stats);
}

/* Large Slice 25: replay-certified inter-block SCCP value propagation.
 * No CFG/PHI/memory-SSA changes: only pure, exact-width unsigned expressions
 * can be replaced; every original SSA operand survives in a separate proof. */
int ast_mir_optimize_sccp_constants(AstMirProgram *program,
                                    AstMirOptimizeStats *stats) {
  AstMirOptimizeStats counts = {0};
  if (!program || !ast_mir_verify_mode(program, AST_MIR_VERIFY_PRE_OPT))
    return 0;
  counts.verifier_runs++;
  AstMirSccpResult *results = (AstMirSccpResult *)calloc(
      program->function_count ? program->function_count : 1u, sizeof(*results));
  if (!results)
    return 0;
  int ok = 1;
  for (size_t fi = 0; fi < program->function_count; ++fi) {
    AstMirFunction *fn = program->functions[fi];
    if (fn->gvn_certificates ||
        !mir_sccp_analyze_function(fn, &results[fi], 0)) {
      ok = 0;
      break;
    }
  }
  for (size_t fi = 0; ok && fi < program->function_count; ++fi) {
    AstMirFunction *fn = program->functions[fi];
    const AstMirSccpResult *r = &results[fi];
    if (!mir_sccp_complete_edges(fn, r))
      continue;
    for (uint32_t id = 0; id < fn->instruction_count; ++id) {
      AstMirInstruction *inst = &fn->instructions[id];
      if (inst->info.op != AST_MIR_OP_BINARY &&
          inst->info.op != AST_MIR_OP_COPY)
        continue;
      counts.examined++;
      if (inst->info.effects || inst->info.flags) {
        counts.skipped_effectful++;
        continue;
      }
      uint32_t width = mir_type_bit_width(inst->info.type);
      if (inst->info.result >= fn->vreg_count ||
          ast_sema_type_is_signed(inst->info.type) ||
          (ast_sema_type_kind(inst->info.type) != AST_SEMA_TYPE_INT &&
           ast_sema_type_kind(inst->info.type) != AST_SEMA_TYPE_BITS) ||
          (width != 8u && width != 16u && width != 32u && width != 64u) ||
          !r->executable_blocks[fn->vregs[inst->info.result].info.block] ||
          r->values[inst->info.result].kind != AST_MIR_SCCP_CONSTANT ||
          !mir_opt_safe_scalar(fn, inst))
        continue;
      if (!fn->gvn_certificates) {
        fn->gvn_certificates = (MirGvnCertificate *)calloc(
            fn->instruction_count, sizeof(*fn->gvn_certificates));
        if (!fn->gvn_certificates) {
          ok = 0;
          break;
        }
      }
      AstSemaConstInfo constant = {0};
      constant.kind = AST_SEMA_CONST_INTEGER;
      constant.lo = r->values[inst->info.result].value;
      if (!mir_opt_install_constant_kind(fn, id, constant, 4u)) {
        ok = 0;
        break;
      }
      counts.rewritten++;
      counts.constant_folded++;
    }
    if (ok && fn->gvn_certificates && !mir_analyze_liveness(fn))
      ok = 0;
  }
  for (size_t fi = 0; fi < program->function_count; ++fi)
    ast_mir_sccp_result_destroy(&results[fi]);
  free(results);
  if (ok)
    ok = ast_mir_verify_mode(program, AST_MIR_VERIFY_POST_OPT);
  if (!ok) {
    mir_opt_rollback(program);
    (void)ast_mir_verify(program);
    return 0;
  }
  counts.verifier_runs++;
  if (stats)
    *stats = counts;
  return 1;
}
