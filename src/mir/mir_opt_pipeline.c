#include "mir_internal.h"

/* Structural optimization is out-of-place. The source of a round is immutable;
 * only a fully constructed and verified replacement can enter the program.
 * No HIR identity is repurposed to justify a different instruction. */
typedef struct MirOptEdge {
  AstMirBlockId target;
  AstMirVReg *args;
  uint32_t count;
} MirOptEdge;

typedef struct MirOptTerm {
  AstMirTermKind kind;
  AstMirVReg value;
  MirOptEdge edges[2];
} MirOptTerm;

typedef struct MirOptPredecessor {
  uint32_t from, edge, next;
} MirOptPredecessor;

typedef struct MirOptPlan {
  const AstMirFunction *source;
  AstMirSccpResult sccp;
  MirOptTerm *terms;
  AstMirVReg *alias;
  uint8_t *constant, *reachable, *live, *instructions;
  uint32_t *queue, *first_pred;
  MirOptPredecessor *preds;
  uint32_t *visited;
  uint32_t generation;
  uint32_t count, head;
  AstMirOptimizeStats *stats;
} MirOptPlan;

static int mir_opt_pure(const AstMirInstruction *inst) {
  if (inst->info.effects ||
      (inst->info.flags & ~(uint32_t)AST_MIR_INST_SYNTHETIC))
    return 0;
  switch (inst->info.op) {
  case AST_MIR_OP_CONSTANT:
  case AST_MIR_OP_UNDEF:
  case AST_MIR_OP_COPY:
  case AST_MIR_OP_STACK_ADDR:
  case AST_MIR_OP_GLOBAL_ADDR:
  case AST_MIR_OP_FUNCTION_ADDR:
  case AST_MIR_OP_GENERIC_CONST:
  case AST_MIR_OP_UNARY:
  case AST_MIR_OP_BINARY:
  case AST_MIR_OP_CAST:
  case AST_MIR_OP_SELECT:
  case AST_MIR_OP_PACK:
  case AST_MIR_OP_INDEX:
  case AST_MIR_OP_MEMBER:
  case AST_MIR_OP_ADDRESS_OF:
    return 1;
  default:
    return 0; /* Includes calls, native, constraints and unknown ops. */
  }
}

static AstMirVReg mir_opt_root(const MirOptPlan *p, AstMirVReg value) {
  if (value == AST_MIR_INVALID_ID)
    return value;
  for (uint32_t steps = 0; steps < p->source->vreg_count; ++steps) {
    if (value >= p->source->vreg_count)
      return AST_MIR_INVALID_ID;
    AstMirVReg next = p->alias[value];
    if (next == value)
      return value;
    value = next;
  }
  return AST_MIR_INVALID_ID;
}

static void mir_opt_destroy_plan(MirOptPlan *p) {
  if (p->terms)
    for (uint32_t b = 0; b < p->source->block_count; ++b)
      for (unsigned e = 0; e < 2u; ++e)
        free(p->terms[b].edges[e].args);
  free(p->terms);
  free(p->alias);
  free(p->constant);
  free(p->reachable);
  free(p->live);
  free(p->instructions);
  free(p->queue);
  free(p->first_pred);
  free(p->preds);
  free(p->visited);
  ast_mir_sccp_result_destroy(&p->sccp);
}

static int mir_opt_copy_edge(MirOptEdge *edge, AstMirBlockId target,
                             const AstMirVReg *args, uint32_t count) {
  edge->target = target;
  edge->count = count;
  if (count) {
    edge->args = (AstMirVReg *)mir_malloc_array(count, sizeof(*edge->args));
    if (!edge->args)
      return 0;
    memcpy(edge->args, args, (size_t)count * sizeof(*args));
  }
  return 1;
}

static int mir_opt_create_plan(MirOptPlan *p, const AstMirFunction *fn,
                               AstMirOptimizeStats *stats) {
  memset(p, 0, sizeof(*p));
  p->source = fn;
  p->stats = stats;
  if (fn->block_count > UINT32_MAX / 2u)
    return 0;
  size_t blocks = fn->block_count ? fn->block_count : 1u;
  size_t values = fn->vreg_count ? fn->vreg_count : 1u;
  p->terms = (MirOptTerm *)calloc(blocks, sizeof(*p->terms));
  p->alias = (AstMirVReg *)mir_malloc_array(values, sizeof(*p->alias));
  p->constant = (uint8_t *)calloc(values, 1u);
  p->reachable = (uint8_t *)calloc(blocks, 1u);
  p->live = (uint8_t *)calloc(values, 1u);
  p->instructions =
      (uint8_t *)calloc(fn->instruction_count ? fn->instruction_count : 1u, 1u);
  p->queue = (uint32_t *)mir_malloc_array(values > blocks ? values : blocks,
                                          sizeof(*p->queue));
  p->first_pred = (uint32_t *)mir_malloc_array(blocks, sizeof(*p->first_pred));
  p->preds =
      (MirOptPredecessor *)mir_malloc_array(blocks * 2u, sizeof(*p->preds));
  p->visited = (uint32_t *)calloc(blocks, sizeof(*p->visited));
  if (!p->terms || !p->alias || !p->constant || !p->reachable || !p->live ||
      !p->instructions || !p->queue || !p->first_pred || !p->preds ||
      !p->visited || !mir_sccp_analyze_function(fn, &p->sccp, 0))
    return 0;
  int complete = mir_sccp_complete_edges(fn, &p->sccp);
  for (uint32_t v = 0; v < fn->vreg_count; ++v) {
    p->alias[v] = v;
    const AstMirVRegInfo *info = &fn->vregs[v].info;
    int pure =
        info->kind == AST_MIR_VREG_BLOCK_ARGUMENT
            ? info->block != fn->entry_block
            : info->definition_index < fn->instruction_count &&
                  mir_opt_pure(&fn->instructions[info->definition_index]);
    p->constant[v] =
        (uint8_t)(complete && pure &&
                  p->sccp.values[v].kind == AST_MIR_SCCP_CONSTANT &&
                  !(info->flags & AST_MIR_VREG_UNDEFINED));
  }
  for (uint32_t b = 0; b < fn->block_count; ++b) {
    const AstMirTerminatorInfo *term = &fn->blocks[b].term.info;
    MirOptTerm *out = &p->terms[b];
    out->kind = term->kind;
    out->value = term->condition_or_value;
    unsigned first = 0;
    if (complete && term->kind == AST_MIR_TERM_COND_BR &&
        p->sccp.values[term->condition_or_value].kind ==
            AST_MIR_SCCP_CONSTANT) {
      first = p->sccp.values[term->condition_or_value].value ? 0u : 1u;
      out->kind = AST_MIR_TERM_BR;
      out->value = AST_MIR_INVALID_ID;
      stats->branch_folded++;
    }
    if (out->kind == AST_MIR_TERM_BR || out->kind == AST_MIR_TERM_COND_BR) {
      if (!mir_opt_copy_edge(
              &out->edges[0], first ? term->target_false : term->target_true,
              first ? term->false_args : term->true_args,
              first ? term->false_arg_count : term->true_arg_count))
        return 0;
      if (out->kind == AST_MIR_TERM_COND_BR &&
          !mir_opt_copy_edge(&out->edges[1], term->target_false,
                             term->false_args, term->false_arg_count))
        return 0;
    }
  }
  return 1;
}

/* Compose arguments across an empty trampoline. Stop before a cycle or entry;
 * an infinite empty loop is observable termination behavior and must survive.
 */
static int mir_opt_thread_edge(MirOptPlan *p, uint32_t from, MirOptEdge *edge) {
  const AstMirFunction *fn = p->source;
  uint32_t stamp = ++p->generation;
  p->visited[from] = stamp;
  for (uint32_t step = 0; step < fn->block_count; ++step) {
    uint32_t target = edge->target;
    if (target >= fn->block_count || target == fn->entry_block ||
        p->visited[target] == stamp || fn->blocks[target].instruction_count ||
        fn->blocks[target].argument_count ||
        p->terms[target].kind != AST_MIR_TERM_BR)
      break;
    p->visited[target] = stamp;
    const MirOptEdge *next = &p->terms[target].edges[0];
    if (next->target == target || p->visited[next->target] == stamp)
      break;
    AstMirVReg *args =
        next->count ? (AstMirVReg *)mir_malloc_array(next->count, sizeof(*args))
                    : NULL;
    if (next->count && !args)
      return 0;
    for (uint32_t a = 0; a < next->count; ++a) {
      AstMirVReg value = next->args[a];
      const AstMirVRegInfo *info = &fn->vregs[value].info;
      if (info->kind == AST_MIR_VREG_BLOCK_ARGUMENT && info->block == target) {
        if (info->definition_index >= edge->count) {
          free(args);
          return 0;
        }
        value = edge->args[info->definition_index];
      }
      args[a] = value;
    }
    free(edge->args);
    edge->args = args;
    edge->count = next->count;
    edge->target = next->target;
    p->stats->edges_threaded++;
  }
  return 1;
}

static int mir_opt_edges(MirOptPlan *p) {
  const AstMirFunction *fn = p->source;
  for (uint32_t b = 0; b < fn->block_count; ++b) {
    unsigned count = p->terms[b].kind == AST_MIR_TERM_COND_BR ? 2u
                     : p->terms[b].kind == AST_MIR_TERM_BR    ? 1u
                                                              : 0u;
    for (unsigned e = 0; e < count; ++e)
      if (!mir_opt_thread_edge(p, b, &p->terms[b].edges[e]))
        return 0;
  }
  if (!fn->block_count)
    return 1;
  p->reachable[fn->entry_block] = 1u;
  p->queue[p->count++] = fn->entry_block;
  while (p->head < p->count) {
    uint32_t b = p->queue[p->head++];
    unsigned edges = p->terms[b].kind == AST_MIR_TERM_COND_BR ? 2u
                     : p->terms[b].kind == AST_MIR_TERM_BR    ? 1u
                                                              : 0u;
    for (unsigned e = 0; e < edges; ++e) {
      uint32_t target = p->terms[b].edges[e].target;
      if (target >= fn->block_count)
        return 0;
      if (!p->reachable[target]) {
        p->reachable[target] = 1u;
        p->queue[p->count++] = target;
      }
    }
  }
  uint32_t used = 0;
  for (uint32_t b = 0; b < fn->block_count; ++b)
    p->first_pred[b] = AST_MIR_INVALID_ID;
  for (uint32_t b = 0; b < fn->block_count; ++b) {
    if (!p->reachable[b])
      continue;
    unsigned edges = p->terms[b].kind == AST_MIR_TERM_COND_BR ? 2u
                     : p->terms[b].kind == AST_MIR_TERM_BR    ? 1u
                                                              : 0u;
    for (unsigned e = 0; e < edges; ++e) {
      uint32_t target = p->terms[b].edges[e].target;
      p->preds[used] = (MirOptPredecessor){b, e, p->first_pred[target]};
      p->first_pred[target] = used++;
    }
  }
  p->head = p->count = 0;
  return 1;
}

static int mir_opt_aliases(MirOptPlan *p) {
  const AstMirFunction *fn = p->source;
  int changed = 1;
  while (changed) {
    changed = 0;
    for (uint32_t v = 0; v < fn->vreg_count; ++v) {
      const AstMirVRegInfo *info = &fn->vregs[v].info;
      if (p->alias[v] != v || p->constant[v] || !p->reachable[info->block] ||
          (info->flags &
           (AST_MIR_VREG_FUNCTION_ARGUMENT | AST_MIR_VREG_UNDEFINED)))
        continue;
      AstMirVReg kept = AST_MIR_INVALID_ID;
      if (info->kind == AST_MIR_VREG_INSTRUCTION_RESULT) {
        const AstMirInstruction *inst =
            &fn->instructions[info->definition_index];
        if (mir_opt_pure(inst) && inst->info.op == AST_MIR_OP_COPY &&
            inst->info.operand_count == 1u)
          kept = mir_opt_root(p, inst->operands[0]);
      } else if (info->kind == AST_MIR_VREG_BLOCK_ARGUMENT &&
                 info->block != fn->entry_block) {
        int conflict = 0;
        for (uint32_t pi = p->first_pred[info->block]; pi != AST_MIR_INVALID_ID;
             pi = p->preds[pi].next) {
          MirOptPredecessor pred = p->preds[pi];
          const MirOptEdge *edge = &p->terms[pred.from].edges[pred.edge];
          if (info->definition_index >= edge->count) {
            return 0;
          }
          AstMirVReg next = mir_opt_root(p, edge->args[info->definition_index]);
          if (next == v)
            continue;
          if (kept != AST_MIR_INVALID_ID && next != kept) {
            conflict = 1;
            break;
          }
          kept = next;
        }
        if (conflict)
          kept = AST_MIR_INVALID_ID;
      }
      if (kept != AST_MIR_INVALID_ID && kept != v) {
        if (kept >= fn->vreg_count ||
            !mir_type_equal(info->type, fn->vregs[kept].info.type)) {
          return 0;
        }
        p->alias[v] = kept;
        changed = 1;
      }
    }
  }
  return 1;
}

static int mir_opt_mark(MirOptPlan *p, AstMirVReg value) {
  value = mir_opt_root(p, value);
  if (value == AST_MIR_INVALID_ID)
    return 0;
  if (!p->live[value]) {
    p->live[value] = 1u;
    p->queue[p->count++] = value;
  }
  return 1;
}

static int mir_opt_mark_operands(MirOptPlan *p, const AstMirInstruction *inst) {
  for (uint32_t a = 0; a < inst->info.operand_count; ++a)
    if (!mir_opt_mark(p, inst->operands[a]))
      return 0;
  return 1;
}

/* Mark from observable roots, then sweep. Unlike repeated zero-use deletion,
 * this also removes unused cyclic PHIs and their mutually dependent values. */
static int mir_opt_dce(MirOptPlan *p) {
  const AstMirFunction *fn = p->source;
  for (uint32_t b = 0; b < fn->block_count; ++b) {
    if (!p->reachable[b])
      continue;
    if (p->terms[b].value != AST_MIR_INVALID_ID &&
        !mir_opt_mark(p, p->terms[b].value))
      return 0;
    if (b == fn->entry_block)
      for (uint32_t a = 0; a < fn->blocks[b].argument_count; ++a)
        if (!mir_opt_mark(p, fn->blocks[b].arguments[a]))
          return 0;
    for (uint32_t j = 0; j < fn->blocks[b].instruction_count; ++j) {
      uint32_t id = fn->blocks[b].instructions[j];
      const AstMirInstruction *inst = &fn->instructions[id];
      p->stats->examined++;
      if (!mir_opt_pure(inst)) {
        p->instructions[id] = 1u;
        p->stats->skipped_effectful++;
        if (!mir_opt_mark_operands(p, inst))
          return 0;
      }
    }
  }
  while (p->head < p->count) {
    uint32_t v = p->queue[p->head++];
    const AstMirVRegInfo *info = &fn->vregs[v].info;
    if (p->constant[v])
      continue;
    if (info->kind == AST_MIR_VREG_INSTRUCTION_RESULT) {
      uint32_t id = info->definition_index;
      p->instructions[id] = 1u;
      if (!mir_opt_mark_operands(p, &fn->instructions[id]))
        return 0;
    } else if (info->block != fn->entry_block) {
      for (uint32_t pi = p->first_pred[info->block]; pi != AST_MIR_INVALID_ID;
           pi = p->preds[pi].next) {
        MirOptPredecessor pred = p->preds[pi];
        const MirOptEdge *edge = &p->terms[pred.from].edges[pred.edge];
        if (info->definition_index >= edge->count ||
            !mir_opt_mark(p, edge->args[info->definition_index]))
          return 0;
      }
    }
  }
  return 1;
}

AstMirFunction *mir_clone_function_header(const AstMirFunction *source) {
  AstMirFunction *fn = (AstMirFunction *)calloc(1u, sizeof(*fn));
  if (!fn)
    return NULL;
  ast_arena_init(&fn->arena, source->arena.block_size);
  fn->source_hir = source->source_hir;
  fn->module_name = source->module_name;
  fn->name = source->name;
  fn->type = source->type;
  fn->span = source->span;
  fn->flags = source->flags;
  fn->frame_policy = source->frame_policy;
  fn->stack_alignment = source->stack_alignment;
  fn->abi = source->abi;
  fn->link_name = source->link_name;
  fn->target_constraint = source->target_constraint;
  fn->requires_constraint = source->requires_constraint;
  fn->promoted_local_count = source->promoted_local_count;
  fn->stack_slot_count = fn->stack_slot_capacity = source->stack_slot_count;
  if (source->stack_slot_count) {
    fn->stack_slots = (AstMirStackSlot *)mir_malloc_array(
        source->stack_slot_count, sizeof(*fn->stack_slots));
    if (!fn->stack_slots) {
      mir_function_destroy(fn);
      return NULL;
    }
    memcpy(fn->stack_slots, source->stack_slots,
           (size_t)source->stack_slot_count * sizeof(*fn->stack_slots));
  }
  fn->inline_site_count = source->inline_site_count;
  if (source->inline_site_count) {
    fn->inline_sites = (AstMirInlineSiteInfo *)mir_malloc_array(
        source->inline_site_count, sizeof(*fn->inline_sites));
    if (!fn->inline_sites) {
      mir_function_destroy(fn);
      return NULL;
    }
    memcpy(fn->inline_sites, source->inline_sites,
           (size_t)source->inline_site_count * sizeof(*fn->inline_sites));
  }
  return fn;
}

static int mir_opt_keep_phi(const MirOptPlan *p, AstMirVReg v) {
  return p->live[v] && p->alias[v] == v && !p->constant[v];
}

static AstSemaConstInfo mir_opt_constant(const AstSemaType *type,
                                         uint64_t bits) {
  AstSemaConstInfo value = {0};
  value.kind = ast_sema_type_kind(type) == AST_SEMA_TYPE_BOOL
                   ? AST_SEMA_CONST_BOOL
                   : AST_SEMA_CONST_INTEGER;
  value.is_signed = (uint8_t)(ast_sema_type_is_signed(type) != 0);
  uint32_t width = mir_type_bit_width(type);
  if (value.is_signed && width && width <= 64u &&
      (bits & (UINT64_C(1) << (width - 1u)))) {
    value.is_negative = 1u;
    bits = (UINT64_C(0) - bits) &
           (width == 64u ? UINT64_MAX : (UINT64_C(1) << width) - 1u);
  }
  value.lo = bits;
  return value;
}

static int mir_opt_emit_constant(AstMirFunction *fn, uint32_t block,
                                 AstMirVReg result, AstMirVReg old,
                                 const MirOptPlan *p) {
  AstMirInstruction inst = {0};
  const AstMirVRegInfo *value = &p->source->vregs[old].info;
  inst.info.op = AST_MIR_OP_CONSTANT;
  inst.info.result = result;
  inst.info.type = value->type;
  inst.info.span = value->span;
  if (value->kind == AST_MIR_VREG_INSTRUCTION_RESULT)
    inst.info.inline_site =
        p->source->instructions[value->definition_index].info.inline_site;
  inst.info.constant = mir_opt_constant(value->type, p->sccp.values[old].value);
  inst.info.source_local = AST_HIR_INVALID_ID;
  inst.info.stack_slot = AST_MIR_INVALID_ID;
  inst.info.memory_in = inst.info.memory_out = AST_MIR_INVALID_ID;
  inst.info.source_hir_instruction = AST_HIR_INVALID_ID;
  uint32_t id = mir_emit_instruction(fn, block, inst, NULL, 0u);
  if (id == AST_MIR_INVALID_ID)
    return 0;
  fn->vregs[result].info.definition_index = id;
  p->stats->constant_folded++;
  return 1;
}

/* Inlining exposes (base +/- a) +/- b. Combine unsigned offsets with exact
 * modular arithmetic; do not reassociate signed or effectful operations. */
static int mir_opt_combine_offset(AstMirFunction *fn, uint32_t block,
                                  const MirOptPlan *p, AstMirInstruction *inst,
                                  AstMirVReg *args, const uint32_t *regs) {
  if (!mir_opt_pure(inst) || inst->info.op != AST_MIR_OP_BINARY ||
      inst->info.operand_count != 2u ||
      (inst->info.token_op != AST_TOK_PLUS &&
       inst->info.token_op != AST_TOK_MINUS) ||
      ast_sema_type_is_signed(inst->info.type))
    return 1;
  AstSemaTypeKind kind = ast_sema_type_kind(inst->info.type);
  uint32_t width = mir_type_bit_width(inst->info.type);
  if ((kind != AST_SEMA_TYPE_INT && kind != AST_SEMA_TYPE_BITS) ||
      (width != 8u && width != 16u && width != 32u && width != 64u))
    return 1;
  AstMirVReg left = mir_opt_root(p, inst->operands[0]),
             right = mir_opt_root(p, inst->operands[1]);
  if (!p->constant[right] ||
      p->source->vregs[left].info.kind != AST_MIR_VREG_INSTRUCTION_RESULT)
    return 1;
  const AstMirInstruction *child =
      &p->source->instructions[p->source->vregs[left].info.definition_index];
  if (!mir_opt_pure(child) || child->info.op != AST_MIR_OP_BINARY ||
      child->info.operand_count != 2u ||
      (child->info.token_op != AST_TOK_PLUS &&
       child->info.token_op != AST_TOK_MINUS) ||
      !mir_type_equal(inst->info.type, child->info.type))
    return 1;
  AstMirVReg base = mir_opt_root(p, child->operands[0]),
             offset = mir_opt_root(p, child->operands[1]);
  if (!p->constant[offset] ||
      (p->source->vregs[base].info.flags & AST_MIR_VREG_UNDEFINED))
    return 1;
  uint64_t a = p->sccp.values[offset].value, b = p->sccp.values[right].value;
  if (child->info.token_op == AST_TOK_MINUS)
    a = UINT64_C(0) - a;
  if (inst->info.token_op == AST_TOK_MINUS)
    b = UINT64_C(0) - b;
  uint64_t combined =
      (a + b) & (width == 64u ? UINT64_MAX : (UINT64_C(1) << width) - 1u);
  args[0] = regs[base];
  inst->info.source_hir_instruction = AST_HIR_INVALID_ID;
  inst->info.flags |= AST_MIR_INST_SYNTHETIC;
  if (!combined) {
    inst->info.op = AST_MIR_OP_COPY;
    inst->info.token_op = AST_TOK_INVALID;
    inst->info.operand_count = 1u;
    return 1;
  }
  AstMirInstruction constant = {0};
  constant.info.op = AST_MIR_OP_CONSTANT;
  constant.info.type = inst->info.type;
  constant.info.span = inst->info.span;
  constant.info.inline_site = inst->info.inline_site;
  constant.info.result =
      mir_add_vreg(fn, AST_MIR_VREG_INSTRUCTION_RESULT, inst->info.type,
                   inst->info.span, block, fn->instruction_count, 0u);
  if (constant.info.result == AST_MIR_INVALID_ID)
    return 0;
  constant.info.constant = mir_opt_constant(inst->info.type, combined);
  constant.info.source_local = AST_HIR_INVALID_ID;
  constant.info.stack_slot = AST_MIR_INVALID_ID;
  constant.info.source_hir_instruction = AST_HIR_INVALID_ID;
  constant.info.memory_in = constant.info.memory_out = AST_MIR_INVALID_ID;
  if (mir_emit_instruction(fn, block, constant, NULL, 0u) == AST_MIR_INVALID_ID)
    return 0;
  args[1] = constant.info.result;
  inst->info.token_op = AST_TOK_PLUS;
  p->stats->constant_folded++;
  return 1;
}

static int mir_opt_build_edge(const MirOptPlan *p, const MirOptEdge *edge,
                              const uint32_t *regs, AstMirVReg **out,
                              uint32_t *count) {
  const AstMirBlock *target = &p->source->blocks[edge->target];
  *count = 0;
  *out = NULL;
  if (edge->count != target->argument_count)
    return 0;
  if (edge->count) {
    *out = (AstMirVReg *)mir_malloc_array(edge->count, sizeof(**out));
    if (!*out)
      return 0;
  }
  for (uint32_t a = 0; a < edge->count; ++a) {
    if (!mir_opt_keep_phi(p, target->arguments[a]))
      continue;
    AstMirVReg value = mir_opt_root(p, edge->args[a]);
    if (value >= p->source->vreg_count || regs[value] == AST_MIR_INVALID_ID)
      return 0;
    (*out)[(*count)++] = regs[value];
  }
  return 1;
}

static AstMirFunction *mir_opt_build(MirOptPlan *p) {
  const AstMirFunction *source = p->source;
  AstMirFunction *fn = mir_clone_function_header(source);
  uint32_t *blocks =
      (uint32_t *)mir_malloc_array(source->block_count, sizeof(*blocks));
  uint32_t *regs = (uint32_t *)mir_malloc_array(
      source->vreg_count ? source->vreg_count : 1u, sizeof(*regs));
  if (!fn || !blocks || !regs)
    goto fail;
  for (uint32_t v = 0; v < source->vreg_count; ++v)
    regs[v] = AST_MIR_INVALID_ID;
  for (uint32_t b = 0; b < source->block_count; ++b) {
    blocks[b] = p->reachable[b] ? fn->block_count++ : AST_MIR_INVALID_ID;
  }
  fn->blocks = (AstMirBlock *)calloc(fn->block_count, sizeof(*fn->blocks));
  if (!fn->blocks) {
    fn->block_count = 0;
    goto fail;
  }
  fn->entry_block = blocks[source->entry_block];
  for (uint32_t b = 0; b < source->block_count; ++b) {
    if (!p->reachable[b])
      continue;
    uint32_t to = blocks[b];
    const AstMirBlock *old = &source->blocks[b];
    AstMirBlock *block = &fn->blocks[to];
    block->id = to;
    block->span = old->span;
    block->memory_argument =
        mir_add_memory(fn,
                       b == source->entry_block ? AST_MIR_MEMORY_ENTRY
                                                : AST_MIR_MEMORY_BLOCK_ARGUMENT,
                       to, 0);
    if (block->memory_argument == AST_MIR_INVALID_ID)
      goto fail;
    for (uint32_t a = 0; a < old->argument_count; ++a) {
      AstMirVReg v = old->arguments[a];
      if (!mir_opt_keep_phi(p, v))
        continue;
      const AstMirVRegInfo *info = &source->vregs[v].info;
      regs[v] =
          mir_add_vreg(fn, AST_MIR_VREG_BLOCK_ARGUMENT, info->type, info->span,
                       to, block->argument_count, info->flags);
      if (regs[v] == AST_MIR_INVALID_ID ||
          !mir_append_block_argument(fn, to, regs[v],
                                     old->argument_source_locals[a]))
        goto fail;
      if (a < old->hir_argument_count)
        block->hir_argument_count++;
      if (info->flags & AST_MIR_VREG_PHI)
        fn->phi_count++;
    }
  }
  /* Allocate instruction-result IDs before uses/backedges are remapped. */
  for (uint32_t v = 0; v < source->vreg_count; ++v) {
    const AstMirVRegInfo *info = &source->vregs[v].info;
    int keep = p->live[v] && p->alias[v] == v && p->constant[v];
    if (info->kind == AST_MIR_VREG_INSTRUCTION_RESULT &&
        p->instructions[info->definition_index])
      keep = 1;
    if (!p->reachable[info->block] || !keep)
      continue;
    regs[v] = mir_add_vreg(fn, AST_MIR_VREG_INSTRUCTION_RESULT, info->type,
                           info->span, blocks[info->block], 0,
                           info->flags & ~(uint32_t)AST_MIR_VREG_PHI);
    if (regs[v] == AST_MIR_INVALID_ID)
      goto fail;
  }
  for (uint32_t b = 0; b < source->block_count; ++b) {
    if (!p->reachable[b])
      continue;
    uint32_t to = blocks[b];
    const AstMirBlock *old = &source->blocks[b];
    AstMirMemoryId memory = fn->blocks[to].memory_argument;
    for (uint32_t a = 0; a < old->argument_count; ++a) {
      AstMirVReg v = old->arguments[a];
      if (p->constant[v] && p->live[v] && p->alias[v] == v &&
          !mir_opt_emit_constant(fn, to, regs[v], v, p))
        goto fail;
    }
    for (uint32_t j = 0; j < old->instruction_count; ++j) {
      uint32_t id = old->instructions[j];
      const AstMirInstruction *before = &source->instructions[id];
      AstMirVReg v = before->info.result;
      if (v != AST_MIR_INVALID_ID && p->constant[v] && p->live[v] &&
          p->alias[v] == v) {
        if (!mir_opt_emit_constant(fn, to, regs[v], v, p))
          goto fail;
        continue;
      }
      if (!p->instructions[id])
        continue;
      AstMirInstruction inst = *before;
      inst.info.result = v == AST_MIR_INVALID_ID ? v : regs[v];
      AstMirVReg *args = inst.info.operand_count
                             ? (AstMirVReg *)mir_malloc_array(
                                   inst.info.operand_count, sizeof(*args))
                             : NULL;
      if (inst.info.operand_count && !args)
        goto fail;
      int valid = 1;
      for (uint32_t a = 0; a < inst.info.operand_count; ++a) {
        AstMirVReg r = mir_opt_root(p, before->operands[a]);
        if (r >= source->vreg_count || regs[r] == AST_MIR_INVALID_ID) {
          valid = 0;
          break;
        }
        args[a] = regs[r];
      }
      if (!valid) {
        free(args);
        goto fail;
      }
      if (!mir_opt_combine_offset(fn, to, p, &inst, args, regs)) {
        free(args);
        goto fail;
      }
      inst.info.memory_in = inst.info.memory_out = AST_MIR_INVALID_ID;
      uint32_t next_id = fn->instruction_count;
      if (mir_instruction_is_effectful(&inst)) {
        inst.info.memory_in = memory;
        memory =
            mir_add_memory(fn, AST_MIR_MEMORY_INSTRUCTION_RESULT, to, next_id);
        if (memory == AST_MIR_INVALID_ID) {
          free(args);
          goto fail;
        }
        inst.info.memory_out = memory;
      }
      uint32_t emitted =
          mir_emit_instruction(fn, to, inst, args, inst.info.operand_count);
      free(args);
      if (emitted == AST_MIR_INVALID_ID)
        goto fail;
      if (v != AST_MIR_INVALID_ID)
        fn->vregs[regs[v]].info.definition_index = emitted;
    }
    const MirOptTerm *planned = &p->terms[b];
    AstMirTerminator term = {0};
    term.info.kind = planned->kind;
    term.info.span = old->term.info.span;
    term.info.memory = memory;
    term.info.target_true = term.info.target_false = AST_MIR_INVALID_ID;
    term.info.condition_or_value = planned->value == AST_MIR_INVALID_ID
                                       ? AST_MIR_INVALID_ID
                                       : regs[mir_opt_root(p, planned->value)];
    AstMirVReg *true_args = NULL, *false_args = NULL;
    uint32_t true_count = 0, false_count = 0;
    int ok = 1;
    if (planned->kind == AST_MIR_TERM_BR ||
        planned->kind == AST_MIR_TERM_COND_BR) {
      term.info.target_true = blocks[planned->edges[0].target];
      ok = mir_opt_build_edge(p, &planned->edges[0], regs, &true_args,
                              &true_count);
    }
    if (ok && planned->kind == AST_MIR_TERM_COND_BR) {
      term.info.target_false = blocks[planned->edges[1].target];
      ok = mir_opt_build_edge(p, &planned->edges[1], regs, &false_args,
                              &false_count);
    }
    if (ok)
      ok = mir_set_terminator(fn, to, term, true_args, true_count, false_args,
                              false_count);
    free(true_args);
    free(false_args);
    if (!ok)
      goto fail;
  }
  if (!mir_analyze_cfg(fn) || !mir_link_memory_ssa(fn) ||
      !mir_analyze_liveness(fn))
    goto fail;
  free(blocks);
  free(regs);
  return fn;
fail:
  free(blocks);
  free(regs);
  mir_function_destroy(fn);
  return NULL;
}

static int mir_opt_same_span(AstSpan a, AstSpan b) {
  return a.file_id == b.file_id && a.offset == b.offset && a.length == b.length;
}

static int mir_opt_same_name(AstName a, AstName b) {
  return mir_opt_same_span(a.span, b.span) &&
         (!a.span.length ||
          (a.text && b.text && memcmp(a.text, b.text, a.span.length) == 0));
}

static int mir_opt_same_text(AstHirTextValue a, AstHirTextValue b) {
  return a.is_present == b.is_present && a.is_literal == b.is_literal &&
         a.source_kind == b.source_kind &&
         mir_opt_same_span(a.source_span, b.source_span) &&
         a.text.length == b.text.length &&
         (!a.text.length ||
          (a.text.data && b.text.data &&
           memcmp(a.text.data, b.text.data, a.text.length) == 0));
}

static int mir_opt_same_instruction(const AstMirInstructionInfo *a,
                                    const AstMirInstructionInfo *b) {
#define SAME(field)                                                            \
  if (a->field != b->field)                                                    \
  return 0
  SAME(id);
  SAME(op);
  SAME(inline_site);
  SAME(result);
  SAME(type);
  SAME(effects);
  SAME(flags);
  SAME(token_op);
  SAME(intrinsic);
  SAME(memory_order);
  SAME(failure_memory_order);
  SAME(source_local);
  SAME(stack_slot);
  SAME(aux_index);
  SAME(native_region);
  SAME(call_signature);
  SAME(operand_count);
  SAME(memory_in);
  SAME(memory_out);
  SAME(source_hir_instruction);
  SAME(constant.kind);
  SAME(constant.lo);
  SAME(constant.hi);
  SAME(constant.is_negative);
  SAME(constant.is_signed);
#undef SAME
  if (memcmp(&a->constant.floating, &b->constant.floating,
             sizeof(a->constant.floating)) ||
      !mir_opt_same_span(a->span, b->span) ||
      !mir_opt_same_name(a->symbol, b->symbol) ||
      !mir_opt_same_name(a->module, b->module))
    return 0;
  return !a->operand_count ||
         (a->operands && b->operands &&
          memcmp(a->operands, b->operands,
                 (size_t)a->operand_count * sizeof(*a->operands)) == 0);
}

/* Compare semantic tables, never allocator addresses, struct padding or cached
 * analysis metadata. The separate structural verifier checks those analyses. */
int mir_functions_equal(const AstMirFunction *a, const AstMirFunction *b) {
  if (a->source_hir != b->source_hir || a->type != b->type ||
      a->flags != b->flags || a->entry_block != b->entry_block ||
      a->frame_policy != b->frame_policy ||
      a->stack_alignment != b->stack_alignment ||
      a->block_count != b->block_count ||
      a->instruction_count != b->instruction_count ||
      a->vreg_count != b->vreg_count ||
      a->stack_slot_count != b->stack_slot_count ||
      a->memory_value_count != b->memory_value_count ||
      a->promoted_local_count != b->promoted_local_count ||
      a->phi_count != b->phi_count ||
      a->memory_phi_count != b->memory_phi_count ||
      !mir_opt_same_span(a->span, b->span) ||
      !mir_opt_same_name(a->name, b->name) ||
      !mir_opt_same_name(a->module_name, b->module_name) ||
      !mir_opt_same_text(a->abi, b->abi) ||
      !mir_opt_same_text(a->link_name, b->link_name) ||
      !mir_opt_same_text(a->target_constraint, b->target_constraint) ||
      !mir_opt_same_text(a->requires_constraint, b->requires_constraint))
    return 0;
  if (a->inline_site_count != b->inline_site_count)
    return 0;
  for (uint32_t s = 0; s < a->inline_site_count; ++s) {
    const AstMirInlineSiteInfo *x = &a->inline_sites[s],
                               *y = &b->inline_sites[s];
    if (x->parent != y->parent ||
        !mir_opt_same_name(x->callee_module, y->callee_module) ||
        !mir_opt_same_name(x->callee_name, y->callee_name) ||
        !mir_opt_same_span(x->call_span, y->call_span))
      return 0;
  }
  for (uint32_t s = 0; s < a->stack_slot_count; ++s) {
    const AstMirStackSlotInfo *x = &a->stack_slots[s].info,
                              *y = &b->stack_slots[s].info;
    if (x->id != y->id || x->source_local != y->source_local ||
        x->type != y->type || x->size != y->size ||
        x->alignment != y->alignment || x->flags != y->flags ||
        !mir_opt_same_span(x->span, y->span) ||
        !mir_opt_same_name(x->name, y->name))
      return 0;
  }
  for (uint32_t id = 0; id < a->instruction_count; ++id)
    if (!mir_opt_same_instruction(&a->instructions[id].info,
                                  &b->instructions[id].info))
      return 0;
  for (uint32_t v = 0; v < a->vreg_count; ++v) {
    const AstMirVRegInfo *x = &a->vregs[v].info, *y = &b->vregs[v].info;
    if (x->type != y->type || x->kind != y->kind || x->block != y->block ||
        x->definition_index != y->definition_index || x->flags != y->flags ||
        x->bank != y->bank || x->bit_width != y->bit_width ||
        !mir_opt_same_span(x->span, y->span))
      return 0;
  }
  for (uint32_t bi = 0; bi < a->block_count; ++bi) {
    const AstMirBlock *x = &a->blocks[bi], *y = &b->blocks[bi];
    if (!mir_opt_same_span(x->span, y->span) ||
        !mir_opt_same_span(x->term.info.span, y->term.info.span) ||
        x->id != y->id || x->hir_argument_count != y->hir_argument_count ||
        x->argument_count != y->argument_count ||
        x->instruction_count != y->instruction_count ||
        x->term.info.kind != y->term.info.kind ||
        x->term.info.condition_or_value != y->term.info.condition_or_value ||
        x->term.info.target_true != y->term.info.target_true ||
        x->term.info.target_false != y->term.info.target_false ||
        x->term.info.true_arg_count != y->term.info.true_arg_count ||
        x->term.info.false_arg_count != y->term.info.false_arg_count ||
        x->memory_argument != y->memory_argument ||
        x->term.info.memory != y->term.info.memory)
      return 0;
    if ((x->argument_count &&
         (memcmp(x->arguments, y->arguments,
                 (size_t)x->argument_count * sizeof(*x->arguments)) ||
          memcmp(x->argument_source_locals, y->argument_source_locals,
                 (size_t)x->argument_count *
                     sizeof(*x->argument_source_locals)))) ||
        (x->instruction_count &&
         memcmp(x->instructions, y->instructions,
                (size_t)x->instruction_count * sizeof(*x->instructions))) ||
        (x->term.info.true_arg_count &&
         memcmp(x->term.true_args, y->term.true_args,
                (size_t)x->term.info.true_arg_count *
                    sizeof(*x->term.true_args))) ||
        (x->term.info.false_arg_count &&
         memcmp(x->term.false_args, y->term.false_args,
                (size_t)x->term.info.false_arg_count *
                    sizeof(*x->term.false_args))))
      return 0;
  }
  return 1;
}

AstMirFunction *mir_optimize_function(AstMirProgram *program,
                                      const AstMirFunction *original,
                                      AstMirOptimizeStats *stats) {
  const AstMirFunction *current = original;
  AstMirFunction *owned = NULL;
  uint64_t budget = (uint64_t)original->block_count +
                    original->instruction_count + original->vreg_count + 2u;
  for (uint64_t round = 0; round < budget; ++round) {
    MirOptPlan p;
    AstMirFunction *next = NULL;
    const char *failed_stage = "SCCP plan";
    int planned = mir_opt_create_plan(&p, current, stats);
    if (planned) {
      failed_stage = "CFG edges";
      planned = mir_opt_edges(&p);
    }
    if (planned) {
      failed_stage = "aliases";
      planned = mir_opt_aliases(&p);
    }
    if (planned) {
      failed_stage = "dead values";
      planned = mir_opt_dce(&p);
    }
    if (planned) {
      failed_stage = "graph construction";
      next = mir_opt_build(&p);
    }
    mir_opt_destroy_plan(&p);
    if (!next) {
      mir_program_diag(
          program, AST_MIR_DIAG_INTERNAL, AST_DIAG_ERROR, original->span,
          "MIR normalization of '%.*s' failed during %s",
          (int)original->name.span.length, original->name.text, failed_stage);
      mir_function_destroy(owned);
      return NULL;
    }
    stats->rounds++;
    if (!mir_verify_structural_function(program, next)) {
      mir_function_destroy(next);
      mir_function_destroy(owned);
      return NULL;
    }
    if (mir_functions_equal(current, next)) {
      mir_function_destroy(owned);
      return next;
    }
    mir_function_destroy(owned);
    owned = next;
    current = next;
  }
  mir_function_destroy(owned);
  mir_program_diag(program, AST_MIR_DIAG_INTERNAL, AST_DIAG_ERROR,
                   original->span,
                   "MIR normalization of '%.*s' did not converge",
                   (int)original->name.span.length, original->name.text);
  return NULL; /* A non-converging rewrite never enters the compiler. */
}

int mir_verify_optimization(AstMirProgram *program, const AstMirFunction *fn) {
  if (!fn || !fn->optimization_proof || !fn->optimization_proof->original ||
      fn->optimization_proof->original->optimization_proof ||
      fn->gvn_certificates || fn->branch_certificates ||
      !mir_verify_function(program, fn->optimization_proof->original) ||
      !mir_verify_structural_function(program, (AstMirFunction *)fn))
    return 0;
  AstMirOptimizeStats ignored = {0};
  AstMirFunction *expected = mir_optimize_function(
      program, fn->optimization_proof->original, &ignored);
  int ok = expected && mir_functions_equal(fn, expected);
  mir_function_destroy(expected);
  if (!ok)
    mir_program_diag(
        program, AST_MIR_DIAG_HIR_INVALID, AST_DIAG_ERROR, fn->span,
        "MIR optimization rewrite differs from original-SSA replay");
  return ok;
}

int ast_mir_optimize(AstMirProgram *program, AstMirOptimizeStats *stats) {
  if (!program || !ast_mir_verify_mode(program, AST_MIR_VERIFY_PRE_OPT))
    return 0;
  AstMirOptimizeStats counts = {0};
  counts.verifier_runs++;
  AstMirFunction **candidate = (AstMirFunction **)calloc(
      program->function_count ? program->function_count : 1u,
      sizeof(*candidate));
  if (!candidate)
    return 0;
  int ok = 1;
  for (size_t f = 0; f < program->function_count && ok; ++f) {
    AstMirFunction *original = program->functions[f];
    if (!original->block_count || (original->flags & AST_HIR_FUNCTION_NAKED) ||
        original->frame_policy == AST_HIR_FRAME_NAKED ||
        original->frame_policy == AST_HIR_FRAME_VALIDATE)
      continue;
    candidate[f] = mir_optimize_function(program, original, &counts);
    if (!candidate[f]) {
      ok = 0;
      break;
    }
    candidate[f]->optimization_proof = (MirOptimizationProof *)calloc(
        1u, sizeof(*candidate[f]->optimization_proof));
    if (!candidate[f]->optimization_proof) {
      ok = 0;
      break;
    }
    candidate[f]->optimization_proof->original = original;
    counts.blocks_removed += original->block_count - candidate[f]->block_count;
    if (original->instruction_count > candidate[f]->instruction_count)
      counts.instructions_removed +=
          original->instruction_count - candidate[f]->instruction_count;
    if (original->phi_count > candidate[f]->phi_count)
      counts.phis_removed += original->phi_count - candidate[f]->phi_count;
  }
  if (ok) {
    for (size_t f = 0; f < program->function_count; ++f)
      if (candidate[f])
        program->functions[f] = candidate[f];
    ok = ast_mir_verify_mode(program, AST_MIR_VERIFY_POST_OPT);
    if (!ok)
      for (size_t f = 0; f < program->function_count; ++f)
        if (candidate[f])
          program->functions[f] = candidate[f]->optimization_proof->original;
  }
  if (!ok) {
    for (size_t f = 0; f < program->function_count; ++f)
      if (candidate[f]) {
        if (candidate[f]->optimization_proof)
          candidate[f]->optimization_proof->original = NULL;
        mir_function_destroy(candidate[f]);
      }
    (void)ast_mir_verify(program);
  } else {
    counts.verifier_runs++;
    counts.rewritten = counts.instructions_removed + counts.blocks_removed +
                       counts.phis_removed;
    if (stats)
      *stats = counts;
  }
  free(candidate);
  return ok;
}
