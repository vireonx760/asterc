#include "mir_internal.h"

/* Sparse conditional constant propagation, analysis-only qualification stage.
 * Edges and SSA block arguments (PHIs) are modeled independently. No MIR,
 * HIR, memory SSA, or CFG structure is modified by this interface.
 * Scalar transfer uses unsigned bit patterns, never host signed overflow.
 * FP, pointers, undefined values and MAY_TRAP operations remain overdefined. */

static uint64_t sccp_mask(uint32_t width) {
  return width == 64u ? UINT64_MAX : (UINT64_C(1) << width) - UINT64_C(1);
}

static int sccp_scalar_width(const AstSemaType *type, uint32_t *width) {
  if (ast_sema_type_kind(type) == AST_SEMA_TYPE_BOOL) {
    *width = 1u;
    return 1;
  }
  AstSemaTypeKind kind = ast_sema_type_kind(type);
  *width = mir_type_bit_width(type);
  return (kind == AST_SEMA_TYPE_INT || kind == AST_SEMA_TYPE_BITS) &&
         (*width == 8u || *width == 16u || *width == 32u || *width == 64u);
}

static AstMirSccpValue sccp_unknown(void) {
  AstMirSccpValue r = {AST_MIR_SCCP_UNKNOWN, 0u};
  return r;
}
static AstMirSccpValue sccp_overdefined(void) {
  AstMirSccpValue r = {AST_MIR_SCCP_OVERDEFINED, 0u};
  return r;
}
static AstMirSccpValue sccp_constant(uint64_t value) {
  AstMirSccpValue r = {AST_MIR_SCCP_CONSTANT, value};
  return r;
}

/* Monotone lattice: UNKNOWN -> CONSTANT -> OVERDEFINED.  Once an edge is
 * executable it cannot become unexecutable, even if subsequent joins widen. */
static int sccp_join(AstMirSccpValue *to, AstMirSccpValue incoming) {
  if (incoming.kind == AST_MIR_SCCP_UNKNOWN ||
      to->kind == AST_MIR_SCCP_OVERDEFINED)
    return 0;
  if (to->kind == AST_MIR_SCCP_UNKNOWN) {
    *to = incoming;
    return 1;
  }
  if (to->kind == AST_MIR_SCCP_CONSTANT &&
      (incoming.kind == AST_MIR_SCCP_OVERDEFINED ||
       incoming.value != to->value)) {
    *to = sccp_overdefined();
    return 1;
  }
  return 0;
}

static AstMirSccpValue sccp_evaluate(const AstMirFunction *fn,
                                     const AstMirSccpValue *values,
                                     const AstMirInstruction *inst) {
  const AstMirInstructionInfo *info = &inst->info;
  if (info->result >= fn->vreg_count)
    return sccp_unknown();
  if (fn->vregs[info->result].info.flags & AST_MIR_VREG_UNDEFINED)
    return sccp_overdefined();
  if (info->op == AST_MIR_OP_CONSTANT) {
    uint32_t width = 0u;
    if (ast_sema_type_kind(info->type) == AST_SEMA_TYPE_BOOL &&
        info->constant.kind == AST_SEMA_CONST_BOOL)
      return sccp_constant(info->constant.lo ? 1u : 0u);
    if (sccp_scalar_width(info->type, &width) &&
        info->constant.kind == AST_SEMA_CONST_INTEGER && !info->constant.hi &&
        !(info->constant.lo & ~sccp_mask(width)))
      return sccp_constant((info->constant.is_negative
                                ? UINT64_C(0) - info->constant.lo
                                : info->constant.lo) &
                           sccp_mask(width));
    return sccp_overdefined();
  }
  if (info->effects || (info->flags & ~(uint32_t)AST_MIR_INST_SYNTHETIC))
    return sccp_overdefined();
  if (info->op == AST_MIR_OP_COPY && info->operand_count == 1u &&
      inst->operands && inst->operands[0] < fn->vreg_count &&
      mir_type_equal(info->type, fn->vregs[inst->operands[0]].info.type))
    return values[inst->operands[0]];
  uint32_t width = 0u;
  if (!sccp_scalar_width(info->type, &width) || !inst->operands ||
      !info->operand_count || inst->operands[0] >= fn->vreg_count)
    return sccp_overdefined();
  AstMirSccpValue first = values[inst->operands[0]];
  if (info->op == AST_MIR_OP_SELECT && info->operand_count == 3u) {
    if (inst->operands[1] >= fn->vreg_count ||
        inst->operands[2] >= fn->vreg_count)
      return sccp_overdefined();
    if (first.kind == AST_MIR_SCCP_CONSTANT)
      return values[inst->operands[first.value ? 1u : 2u]];
    AstMirSccpValue left = values[inst->operands[1]],
                    right = values[inst->operands[2]];
    if (left.kind == AST_MIR_SCCP_CONSTANT && right.kind == left.kind &&
        left.value == right.value)
      return left;
    return first.kind == AST_MIR_SCCP_UNKNOWN ? sccp_unknown()
                                              : sccp_overdefined();
  }
  if ((info->op == AST_MIR_OP_UNARY || info->op == AST_MIR_OP_CAST) &&
      info->operand_count == 1u) {
    if (first.kind != AST_MIR_SCCP_CONSTANT)
      return first;
    const AstSemaType *from = fn->vregs[inst->operands[0]].info.type;
    uint32_t source_width = 0;
    if (!sccp_scalar_width(from, &source_width))
      return sccp_overdefined();
    uint64_t value = first.value;
    if (info->op == AST_MIR_OP_CAST) {
      if (width == 1u)
        return sccp_constant(value != 0u);
      int extend_sign = info->token_op == AST_TOK_KW_SEXT ||
                        (info->token_op != AST_TOK_KW_ZEXT &&
                         info->token_op != AST_TOK_KW_BITCAST &&
                         ast_sema_type_is_signed(from));
      if (extend_sign && (value & (UINT64_C(1) << (source_width - 1u))))
        value |= ~sccp_mask(source_width);
    } else if (info->token_op == AST_TOK_BANG)
      value = value == 0u;
    else if (info->token_op == AST_TOK_TILDE)
      value = ~value;
    else if (info->token_op == AST_TOK_MINUS) {
      if (ast_sema_type_is_signed(from) &&
          value == (UINT64_C(1) << (source_width - 1u)))
        return sccp_overdefined();
      value = UINT64_C(0) - value;
    } else if (info->token_op != AST_TOK_PLUS)
      return sccp_overdefined();
    return sccp_constant(value & sccp_mask(width));
  }
  if (info->op != AST_MIR_OP_BINARY || info->operand_count != 2u)
    return sccp_overdefined();
  AstMirVReg a = inst->operands[0], b = inst->operands[1];
  uint32_t input_width = 0;
  if (b >= fn->vreg_count ||
      !sccp_scalar_width(fn->vregs[a].info.type, &input_width) ||
      !mir_type_equal(fn->vregs[a].info.type, fn->vregs[b].info.type))
    return sccp_overdefined();
  AstMirSccpValue x = values[a], y = values[b];
  if (x.kind == AST_MIR_SCCP_OVERDEFINED || y.kind == AST_MIR_SCCP_OVERDEFINED)
    return sccp_overdefined();
  if (x.kind != AST_MIR_SCCP_CONSTANT || y.kind != AST_MIR_SCCP_CONSTANT)
    return sccp_unknown();
  uint64_t result, mask = sccp_mask(input_width);
  uint64_t sign = UINT64_C(1) << (input_width - 1u);
  int signed_op = ast_sema_type_is_signed(fn->vregs[a].info.type);
  uint64_t ordered_x = x.value ^ (signed_op ? sign : 0u);
  uint64_t ordered_y = y.value ^ (signed_op ? sign : 0u);
  switch (info->token_op) {
  case AST_TOK_EQ_EQ:
    return sccp_constant(x.value == y.value);
  case AST_TOK_BANG_EQ:
    return sccp_constant(x.value != y.value);
  case AST_TOK_LT:
    return sccp_constant(ordered_x < ordered_y);
  case AST_TOK_LT_EQ:
    return sccp_constant(ordered_x <= ordered_y);
  case AST_TOK_GT:
    return sccp_constant(ordered_x > ordered_y);
  case AST_TOK_GT_EQ:
    return sccp_constant(ordered_x >= ordered_y);
  case AST_TOK_PLUS:
    result = (x.value + y.value) & mask;
    if (signed_op && ((x.value ^ result) & (y.value ^ result) & sign))
      return sccp_overdefined();
    break;
  case AST_TOK_MINUS:
    result = (x.value - y.value) & mask;
    if (signed_op && ((x.value ^ y.value) & (x.value ^ result) & sign))
      return sccp_overdefined();
    break;
  case AST_TOK_STAR:
    if (signed_op) {
      uint64_t ax = x.value & sign ? (UINT64_C(0) - x.value) & mask : x.value;
      uint64_t ay = y.value & sign ? (UINT64_C(0) - y.value) & mask : y.value;
      uint64_t limit = (x.value ^ y.value) & sign ? sign : sign - 1u;
      if (ay && ax > limit / ay)
        return sccp_overdefined();
    }
    result = x.value * y.value;
    break;
  case AST_TOK_AMP:
    result = x.value & y.value;
    break;
  case AST_TOK_PIPE:
    result = x.value | y.value;
    break;
  case AST_TOK_CARET:
    result = x.value ^ y.value;
    break;
  case AST_TOK_AMP_AMP:
    result = x.value && y.value;
    break;
  case AST_TOK_PIPE_PIPE:
    result = x.value || y.value;
    break;
  case AST_TOK_SHIFT_LEFT:
    if (signed_op || y.value >= input_width)
      return sccp_overdefined();
    result = x.value << y.value;
    break;
  case AST_TOK_SHIFT_RIGHT:
    if (y.value >= input_width)
      return sccp_overdefined();
    result = x.value >> y.value;
    if (signed_op && y.value && (x.value & sign))
      result |= mask ^ (mask >> y.value);
    break;
  /* MAY_TRAP division/remainder are never evaluated (effects above).
   * Future effect proofs must independently revalidate memory SSA. */
  default:
    return sccp_overdefined();
  }
  return sccp_constant(result & sccp_mask(width));
}

typedef struct SccpUse {
  uint32_t block;
  uint32_t next;
} SccpUse;

typedef struct SccpWorklist {
  const AstMirFunction *fn;
  AstMirSccpResult *result;
  int original_view;
  uint32_t *queue;
  uint8_t *queued;
  uint32_t head, tail, count;
  uint32_t *uses_head;
  SccpUse *uses;
  size_t use_count, use_capacity;
} SccpWorklist;

static const AstMirInstruction *sccp_instruction(const SccpWorklist *w,
                                                 AstMirInstId id) {
  if (w->original_view && w->fn->gvn_certificates &&
      w->fn->gvn_certificates[id].valid)
    return &w->fn->gvn_certificates[id].original;
  return &w->fn->instructions[id];
}

static void sccp_schedule(SccpWorklist *w, uint32_t block) {
  if (block >= w->fn->block_count || !w->result->executable_blocks[block] ||
      w->queued[block])
    return;
  w->queue[w->tail] = block;
  w->tail = (w->tail + 1u) % w->fn->block_count;
  w->count++;
  w->queued[block] = 1u;
}

static void sccp_enqueue_users(SccpWorklist *w, AstMirVReg value) {
  if (value >= w->fn->vreg_count)
    return;
  for (uint32_t i = w->uses_head[value]; i != UINT32_MAX; i = w->uses[i].next)
    sccp_schedule(w, w->uses[i].block);
}

static int sccp_add_use(SccpWorklist *w, AstMirVReg value, uint32_t block) {
  if (value >= w->fn->vreg_count || block >= w->fn->block_count ||
      w->use_count >= w->use_capacity)
    return 0;
  uint32_t id = (uint32_t)w->use_count++;
  w->uses[id].block = block;
  w->uses[id].next = w->uses_head[value];
  w->uses_head[value] = id;
  return 1;
}

/* Construct a sparse SSA use index, including PHI *edge* arguments, so an
 * arriving lattice value re-evaluates the destination PHI, not every block. */
static int sccp_index_uses(SccpWorklist *w) {
  size_t count = 0u;
  for (uint32_t b = 0u; b < w->fn->block_count; ++b) {
    const AstMirBlock *block = &w->fn->blocks[b];
    for (uint32_t i = 0u; i < block->instruction_count; ++i) {
      AstMirInstId id = block->instructions[i];
      if (id >= w->fn->instruction_count)
        return 0;
      const AstMirInstruction *inst = sccp_instruction(w, id);
      if (count > SIZE_MAX - inst->info.operand_count)
        return 0;
      count += inst->info.operand_count;
    }
    const AstMirTerminatorInfo *t = &block->term.info;
    size_t extra = (size_t)t->true_arg_count + t->false_arg_count +
                   (t->kind == AST_MIR_TERM_COND_BR ? 1u : 0u);
    if (count > SIZE_MAX - extra)
      return 0;
    count += extra;
  }
  if (count > UINT32_MAX)
    return 0;
  w->use_capacity = count;
  w->uses = (SccpUse *)calloc(count ? count : 1u, sizeof(*w->uses));
  w->uses_head = (uint32_t *)malloc(
      (w->fn->vreg_count ? w->fn->vreg_count : 1u) * sizeof(*w->uses_head));
  if (!w->uses || !w->uses_head)
    return 0;
  for (uint32_t v = 0u; v < w->fn->vreg_count; ++v)
    w->uses_head[v] = UINT32_MAX;
  for (uint32_t b = 0u; b < w->fn->block_count; ++b) {
    const AstMirBlock *block = &w->fn->blocks[b];
    for (uint32_t i = 0u; i < block->instruction_count; ++i) {
      const AstMirInstruction *inst =
          sccp_instruction(w, block->instructions[i]);
      for (uint32_t a = 0u; a < inst->info.operand_count; ++a)
        if (!sccp_add_use(w, inst->operands[a], b))
          return 0;
    }
    const AstMirTerminatorInfo *t = &block->term.info;
    if (t->kind == AST_MIR_TERM_COND_BR &&
        !sccp_add_use(w, t->condition_or_value, b))
      return 0;
    if (block->successor_count) {
      uint32_t dst = block->successors[0];
      for (uint32_t a = 0u; a < t->true_arg_count; ++a)
        if (!sccp_add_use(w, t->true_args[a], dst))
          return 0;
    }
    if (block->successor_count > 1u) {
      uint32_t dst = block->successors[1];
      for (uint32_t a = 0u; a < t->false_arg_count; ++a)
        if (!sccp_add_use(w, t->false_args[a], dst))
          return 0;
    }
  }
  return 1;
}

static int sccp_mark_edge(const AstMirFunction *fn, AstMirSccpResult *result,
                          uint32_t block, uint32_t slot) {
  if (block >= fn->block_count || slot >= fn->blocks[block].successor_count ||
      slot >= 2u)
    return 0;
  uint32_t successor = fn->blocks[block].successors[slot];
  if (successor >= fn->block_count)
    return 0;
  uint8_t *edge = &result->executable_edges[(size_t)block * 2u + slot];
  if (*edge)
    return 0;
  *edge = 1u;
  result->executable_blocks[successor] = 1u;
  return 1;
}

static void sccp_scan_phis(SccpWorklist *w, uint32_t block_id) {
  const AstMirFunction *fn = w->fn;
  AstMirSccpResult *result = w->result;
  const AstMirBlock *block = &fn->blocks[block_id];
  for (uint32_t ai = 0u; ai < block->argument_count; ++ai) {
    AstMirVReg phi = block->arguments[ai];
    if (phi >= fn->vreg_count)
      continue;
    /* Entry block arguments are externally supplied, not constants. */
    if (block_id == fn->entry_block) {
      if (sccp_join(&result->values[phi], sccp_overdefined()))
        sccp_enqueue_users(w, phi);
      continue;
    }
    for (uint32_t pi = 0u; pi < block->predecessor_count; ++pi) {
      uint32_t pred_id = block->predecessors[pi];
      const AstMirBlock *pred = &fn->blocks[pred_id];
      for (uint32_t ei = 0u; ei < pred->successor_count; ++ei) {
        if (!result->executable_edges[(size_t)pred_id * 2u + ei] ||
            pred->successors[ei] != block_id)
          continue;
        const AstMirVReg *args =
            ei == 0u ? pred->term.info.true_args : pred->term.info.false_args;
        uint32_t count = ei == 0u ? pred->term.info.true_arg_count
                                  : pred->term.info.false_arg_count;
        if (ai >= count || !args || args[ai] >= fn->vreg_count)
          continue;
        if (sccp_join(&result->values[phi], result->values[args[ai]]))
          sccp_enqueue_users(w, phi);
      }
    }
  }
}

void ast_mir_sccp_result_destroy(AstMirSccpResult *result) {
  if (!result)
    return;
  free(result->values);
  free(result->executable_blocks);
  free(result->executable_edges);
  memset(result, 0, sizeof(*result));
}

int mir_sccp_analyze_function(const AstMirFunction *fn, AstMirSccpResult *out,
                              int original_view) {
  if (!fn || !out || (original_view != 0 && original_view != 1))
    return 0;
  AstMirSccpResult result = {0};
  result.value_count = fn->vreg_count;
  result.block_count = fn->block_count;
  result.values = (AstMirSccpValue *)calloc(
      fn->vreg_count ? fn->vreg_count : 1u, sizeof(*result.values));
  result.executable_blocks =
      (uint8_t *)calloc(fn->block_count ? fn->block_count : 1u, 1u);
  result.executable_edges = (uint8_t *)calloc(
      fn->block_count ? (size_t)fn->block_count * 2u : 1u, 1u);
  if (!result.values || !result.executable_blocks || !result.executable_edges) {
    ast_mir_sccp_result_destroy(&result);
    return 0;
  }
  SccpWorklist w = {0};
  w.fn = fn;
  w.result = &result;
  w.original_view = original_view;
  w.queue = (uint32_t *)calloc(fn->block_count ? fn->block_count : 1u,
                               sizeof(*w.queue));
  w.queued = (uint8_t *)calloc(fn->block_count ? fn->block_count : 1u, 1u);
  if (!w.queue || !w.queued || !sccp_index_uses(&w)) {
    free(w.queue);
    free(w.queued);
    free(w.uses);
    free(w.uses_head);
    ast_mir_sccp_result_destroy(&result);
    return 0;
  }
  if (fn->block_count) {
    result.executable_blocks[fn->entry_block] = 1u;
    sccp_schedule(&w, fn->entry_block);
  }
  while (w.count) {
    uint32_t bi = w.queue[w.head];
    w.head = (w.head + 1u) % fn->block_count;
    w.count--;
    w.queued[bi] = 0u;
    result.iterations++; /* Number of actual block re-evaluations. */
    const AstMirBlock *block = &fn->blocks[bi];
    sccp_scan_phis(&w, bi);
    for (uint32_t j = 0u; j < block->instruction_count; ++j) {
      AstMirInstId id = block->instructions[j];
      const AstMirInstruction *inst = sccp_instruction(&w, id);
      if (inst->info.result < fn->vreg_count &&
          sccp_join(&result.values[inst->info.result],
                    sccp_evaluate(fn, result.values, inst)))
        sccp_enqueue_users(&w, inst->info.result);
    }
    const AstMirTerminatorInfo *term = &block->term.info;
    if (term->kind == AST_MIR_TERM_BR) {
      if (sccp_mark_edge(fn, &result, bi, 0u))
        sccp_schedule(&w, block->successors[0]);
    } else if (term->kind == AST_MIR_TERM_COND_BR) {
      AstMirVReg cond = term->condition_or_value;
      AstMirSccpValue state =
          cond < fn->vreg_count ? result.values[cond] : sccp_overdefined();
      if (state.kind == AST_MIR_SCCP_CONSTANT) {
        uint32_t edge = state.value ? 0u : 1u;
        if (sccp_mark_edge(fn, &result, bi, edge))
          sccp_schedule(&w, block->successors[edge]);
      } else if (state.kind == AST_MIR_SCCP_OVERDEFINED) {
        for (uint32_t edge = 0u; edge < 2u; ++edge)
          if (sccp_mark_edge(fn, &result, bi, edge))
            sccp_schedule(&w, block->successors[edge]);
      }
    }
  }
  free(w.queue);
  free(w.queued);
  free(w.uses);
  free(w.uses_head);
  result.effectful_instructions = 0u;
  for (uint32_t bi = 0u; bi < fn->block_count; ++bi) {
    if (result.executable_blocks[bi])
      result.executable_block_count++;
    const AstMirBlock *block = &fn->blocks[bi];
    for (uint32_t ei = 0u; ei < block->successor_count; ++ei)
      if (result.executable_edges[(size_t)bi * 2u + ei])
        result.executable_edge_count++;
    if (!result.executable_blocks[bi])
      continue;
    for (uint32_t ii = 0u; ii < block->instruction_count; ++ii) {
      AstMirInstId id = block->instructions[ii];
      if (id < fn->instruction_count && (fn->instructions[id].info.effects ||
                                         fn->instructions[id].info.flags))
        result.effectful_instructions++;
    }
  }
  for (uint32_t vr = 0u; vr < fn->vreg_count; ++vr) {
    if (result.values[vr].kind == AST_MIR_SCCP_CONSTANT)
      result.constant_value_count++;
    if (result.values[vr].kind == AST_MIR_SCCP_OVERDEFINED)
      result.overdefined_value_count++;
  }
  *out = result;
  return 1;
}

/* Public inspection never analyzes unverified MIR.  Internal replay is called
 * only after every origin certificate passed HIR and structural verification.
 */
int ast_mir_sccp_analyze(AstMirProgram *program, size_t function_index,
                         AstMirSccpResult *out) {
  if (!out || !program || function_index >= program->function_count ||
      !ast_mir_verify_mode(program, AST_MIR_VERIFY_PRE_OPT))
    return 0;
  return mir_sccp_analyze_function(program->functions[function_index], out, 0);
}

/* An unresolved branch can activate another PHI predecessor at runtime.
 * Refuse all speculative propagation in a function with such a branch. */
int mir_sccp_complete_edges(const AstMirFunction *fn,
                            const AstMirSccpResult *r) {
  for (uint32_t bi = 0; bi < fn->block_count; ++bi) {
    if (!r->executable_blocks[bi])
      continue;
    const AstMirTerminatorInfo *term = &fn->blocks[bi].term.info;
    if (term->kind == AST_MIR_TERM_COND_BR &&
        r->values[term->condition_or_value].kind == AST_MIR_SCCP_UNKNOWN)
      return 0;
    /* A PHI cannot be trusted while an active incoming SSA operand is
     * UNKNOWN, even if another predecessor already supplied a constant. */
    for (uint32_t edge = 0; edge < fn->blocks[bi].successor_count; ++edge) {
      if (!r->executable_edges[(size_t)bi * 2u + edge])
        continue;
      const AstMirVReg *args = edge == 0u ? term->true_args : term->false_args;
      uint32_t count =
          edge == 0u ? term->true_arg_count : term->false_arg_count;
      for (uint32_t ai = 0; ai < count; ++ai)
        if (r->values[args[ai]].kind == AST_MIR_SCCP_UNKNOWN)
          return 0;
    }
  }
  return 1;
}

/* The optimized stream is untrusted: independently replay SCCP on saved
 * HIR-checked ORIGINAL instructions, including all cross-block PHI inputs. */
int mir_sccp_verify_certificates(AstMirProgram *program, AstMirFunction *fn) {
  if (!fn->gvn_certificates)
    return 1;
  int has_sccp = 0;
  for (uint32_t id = 0; id < fn->instruction_count; ++id)
    if (fn->gvn_certificates[id].valid == 4u) {
      has_sccp = 1;
      break;
    }
  if (!has_sccp)
    return 1;
  AstMirSccpResult r = {0};
  if (!mir_sccp_analyze_function(fn, &r, 1))
    return 0;
  int ok = mir_sccp_complete_edges(fn, &r);
  for (uint32_t id = 0; ok && id < fn->instruction_count; ++id) {
    const MirGvnCertificate *cert = &fn->gvn_certificates[id];
    if (cert->valid != 4u)
      continue;
    const AstMirInstruction *inst = &fn->instructions[id];
    AstMirVReg vr = inst->info.result;
    uint32_t width = mir_type_bit_width(inst->info.type);
    uint64_t mask = width == 64u                ? UINT64_MAX
                    : width > 0u && width < 64u ? (UINT64_C(1) << width) - 1u
                                                : 0u;
    ok = vr < fn->vreg_count && r.executable_blocks[fn->vregs[vr].info.block] &&
         (width == 8u || width == 16u || width == 32u || width == 64u) &&
         !ast_sema_type_is_signed(inst->info.type) &&
         (ast_sema_type_kind(inst->info.type) == AST_SEMA_TYPE_INT ||
          ast_sema_type_kind(inst->info.type) == AST_SEMA_TYPE_BITS) &&
         r.values[vr].kind == AST_MIR_SCCP_CONSTANT &&
         inst->info.constant.kind == AST_SEMA_CONST_INTEGER &&
         !inst->info.constant.is_negative && !inst->info.constant.is_signed &&
         !inst->info.constant.hi &&
         inst->info.constant.lo == r.values[vr].value &&
         !(inst->info.constant.lo & ~mask);
  }
  if (!ok)
    mir_program_diag(
        program, AST_MIR_DIAG_HIR_INVALID, AST_DIAG_ERROR, fn->span,
        "SCCP replacement fails independent original SSA/CFG replay");
  ast_mir_sccp_result_destroy(&r);
  return ok;
}
