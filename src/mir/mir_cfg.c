#include "mir_internal.h"

typedef struct MirDfsFrame {
  AstMirBlockId block;
  uint32_t next_successor;
} MirDfsFrame;

static int mir_append_predecessor(AstMirFunction *fn, AstMirBlockId block_id,
                                  AstMirBlockId predecessor) {
  AstMirBlock *block = &fn->blocks[block_id];
  size_t capacity = block->predecessor_capacity;
  if (!mir_reserve((void **)&block->predecessors, sizeof(*block->predecessors),
                   &capacity, (size_t)block->predecessor_count + 1u)) {
    fn->oom = 1u;
    return 0;
  }
  if (capacity > UINT32_MAX) {
    fn->oom = 1u;
    return 0;
  }
  block->predecessor_capacity = (uint32_t)capacity;
  block->predecessors[block->predecessor_count++] = predecessor;
  return 1;
}

static int mir_cfg_successors(AstMirFunction *fn) {
  for (uint32_t bi = 0u; bi < fn->block_count; ++bi) {
    AstMirBlock *block = &fn->blocks[bi];
    block->predecessor_count = 0u;
    block->successor_count = 0u;
    block->reachable = 0u;
    block->reverse_postorder = AST_MIR_INVALID_ID;
    block->immediate_dominator = AST_MIR_INVALID_ID;
    block->loop_depth = 0u;
  }
  for (uint32_t bi = 0u; bi < fn->block_count; ++bi) {
    AstMirBlock *block = &fn->blocks[bi];
    AstMirTermKind kind = block->term.info.kind;
    if (kind == AST_MIR_TERM_BR || kind == AST_MIR_TERM_COND_BR) {
      AstMirBlockId target = block->term.info.target_true;
      if (target >= fn->block_count)
        return 0;
      block->successors[block->successor_count++] = target;
      if (!mir_append_predecessor(fn, target, bi))
        return 0;
    }
    if (kind == AST_MIR_TERM_COND_BR) {
      AstMirBlockId target = block->term.info.target_false;
      if (target >= fn->block_count)
        return 0;
      block->successors[block->successor_count++] = target;
      if (!mir_append_predecessor(fn, target, bi))
        return 0;
    }
  }
  return 1;
}

static AstMirBlockId mir_intersect_idom(const AstMirFunction *fn,
                                        AstMirBlockId a, AstMirBlockId b) {
  while (a != b) {
    while (fn->blocks[a].reverse_postorder > fn->blocks[b].reverse_postorder)
      a = fn->blocks[a].immediate_dominator;
    while (fn->blocks[b].reverse_postorder > fn->blocks[a].reverse_postorder)
      b = fn->blocks[b].immediate_dominator;
  }
  return a;
}

static int mir_block_dominates(const AstMirFunction *fn,
                               AstMirBlockId dominator, AstMirBlockId block) {
  if (dominator >= fn->block_count || block >= fn->block_count ||
      !fn->blocks[dominator].reachable || !fn->blocks[block].reachable)
    return 0;
  AstMirBlockId cursor = block;
  while (cursor != AST_MIR_INVALID_ID) {
    if (cursor == dominator)
      return 1;
    cursor = fn->blocks[cursor].immediate_dominator;
  }
  return 0;
}

static int mir_compute_loop_depth(AstMirFunction *fn) {
  if (fn->block_count == 0u)
    return 1;
  uint8_t *members = (uint8_t *)calloc(fn->block_count, 1u);
  AstMirBlockId *stack =
      (AstMirBlockId *)mir_malloc_array(fn->block_count, sizeof(*stack));
  if (!members || !stack) {
    free(members);
    free(stack);
    fn->oom = 1u;
    return 0;
  }
  for (uint32_t header = 0u; header < fn->block_count; ++header) {
    if (!fn->blocks[header].reachable)
      continue;
    memset(members, 0, fn->block_count);
    size_t stack_count = 0u;
    int has_backedge = 0;
    members[header] = 1u;
    for (uint32_t pi = 0u; pi < fn->blocks[header].predecessor_count; ++pi) {
      AstMirBlockId latch = fn->blocks[header].predecessors[pi];
      if (mir_block_dominates(fn, header, latch)) {
        has_backedge = 1;
        if (!members[latch]) {
          members[latch] = 1u;
          stack[stack_count++] = latch;
        }
      }
    }
    if (!has_backedge)
      continue;
    while (stack_count) {
      AstMirBlockId block = stack[--stack_count];
      for (uint32_t pi = 0u; pi < fn->blocks[block].predecessor_count; ++pi) {
        AstMirBlockId predecessor = fn->blocks[block].predecessors[pi];
        if (predecessor < fn->block_count && !members[predecessor] &&
            mir_block_dominates(fn, header, predecessor)) {
          members[predecessor] = 1u;
          stack[stack_count++] = predecessor;
        }
      }
    }
    for (uint32_t bi = 0u; bi < fn->block_count; ++bi) {
      if (members[bi] && fn->blocks[bi].loop_depth != UINT32_MAX)
        fn->blocks[bi].loop_depth++;
    }
  }
  free(members);
  free(stack);
  return 1;
}

int mir_analyze_cfg(AstMirFunction *fn) {
  if (!fn)
    return 0;
  if (fn->block_count == 0u)
    return 1;
  if (fn->entry_block >= fn->block_count || !mir_cfg_successors(fn))
    return 0;
  MirDfsFrame *stack =
      (MirDfsFrame *)mir_malloc_array(fn->block_count, sizeof(*stack));
  AstMirBlockId *postorder =
      (AstMirBlockId *)mir_malloc_array(fn->block_count, sizeof(*postorder));
  uint8_t *visited = (uint8_t *)calloc(fn->block_count, 1u);
  if (!stack || !postorder || !visited) {
    free(stack);
    free(postorder);
    free(visited);
    fn->oom = 1u;
    return 0;
  }
  size_t stack_count = 0u, post_count = 0u;
  stack[stack_count++] = (MirDfsFrame){fn->entry_block, 0u};
  visited[fn->entry_block] = 1u;
  while (stack_count) {
    MirDfsFrame *frame = &stack[stack_count - 1u];
    AstMirBlock *block = &fn->blocks[frame->block];
    if (block->successor_count > AST_MIR_MAX_SUCCESSORS) {
      free(stack);
      free(postorder);
      free(visited);
      return 0;
    }
    if (frame->next_successor < block->successor_count &&
        frame->next_successor < AST_MIR_MAX_SUCCESSORS) {
      AstMirBlockId successor = block->successors[frame->next_successor++];
      if (!visited[successor]) {
        visited[successor] = 1u;
        stack[stack_count++] = (MirDfsFrame){successor, 0u};
      }
    } else {
      postorder[post_count++] = frame->block;
      stack_count--;
    }
  }
  for (uint32_t i = 0u; i < (uint32_t)post_count; ++i) {
    AstMirBlockId block = postorder[post_count - 1u - i];
    fn->blocks[block].reachable = 1u;
    fn->blocks[block].reverse_postorder = i;
  }
  uint32_t next_rpo = (uint32_t)post_count;
  for (uint32_t bi = 0u; bi < fn->block_count; ++bi)
    if (!visited[bi])
      fn->blocks[bi].reverse_postorder = next_rpo++;

  fn->blocks[fn->entry_block].immediate_dominator = fn->entry_block;
  int changed = 1;
  while (changed) {
    changed = 0;
    for (uint32_t rpo = 1u; rpo < (uint32_t)post_count; ++rpo) {
      AstMirBlockId block_id = postorder[post_count - 1u - rpo];
      AstMirBlock *block = &fn->blocks[block_id];
      AstMirBlockId next_idom = AST_MIR_INVALID_ID;
      for (uint32_t pi = 0u; pi < block->predecessor_count; ++pi) {
        AstMirBlockId predecessor = block->predecessors[pi];
        if (!fn->blocks[predecessor].reachable ||
            fn->blocks[predecessor].immediate_dominator == AST_MIR_INVALID_ID)
          continue;
        next_idom = next_idom == AST_MIR_INVALID_ID
                        ? predecessor
                        : mir_intersect_idom(fn, predecessor, next_idom);
      }
      if (block->immediate_dominator != next_idom) {
        block->immediate_dominator = next_idom;
        changed = 1;
      }
    }
  }
  fn->blocks[fn->entry_block].immediate_dominator = AST_MIR_INVALID_ID;
  int ok = mir_compute_loop_depth(fn);
  free(stack);
  free(postorder);
  free(visited);
  return ok;
}

static void mir_bit_set(uint64_t *bits, AstMirVReg vreg) {
  bits[vreg / 64u] |= UINT64_C(1) << (vreg % 64u);
}

static int mir_bit_test(const uint64_t *bits, AstMirVReg vreg) {
  return (bits[vreg / 64u] & (UINT64_C(1) << (vreg % 64u))) != 0u;
}

static uint32_t mir_popcount64(uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
  return (uint32_t)__builtin_popcountll((unsigned long long)value);
#else
  uint32_t count = 0u;
  while (value) {
    value &= value - 1u;
    count++;
  }
  return count;
#endif
}

static uint32_t mir_ctz64(uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
  return (uint32_t)__builtin_ctzll((unsigned long long)value);
#else
  uint32_t count = 0u;
  while ((value & UINT64_C(1)) == 0u) {
    value >>= 1u;
    count++;
  }
  return count;
#endif
}

static uint64_t mir_block_weight(uint32_t loop_depth) {
  uint64_t weight = 1u;
  while (loop_depth-- && weight <= UINT64_MAX / 10u)
    weight *= 10u;
  return weight;
}

static void mir_note_use(AstMirFunction *fn, AstMirVReg vreg, uint64_t position,
                         uint64_t weight, uint64_t *block_use,
                         const uint64_t *block_def) {
  if (vreg >= fn->vreg_count)
    return;
  AstMirVRegInfo *info = &fn->vregs[vreg].info;
  if (info->use_count != UINT32_MAX)
    info->use_count++;
  if (position > info->live_end)
    info->live_end = position;
  if (UINT64_MAX - info->spill_weight < weight)
    info->spill_weight = UINT64_MAX;
  else
    info->spill_weight += weight;
  if (!mir_bit_test(block_def, vreg))
    mir_bit_set(block_use, vreg);
}

int mir_analyze_liveness(AstMirFunction *fn) {
  if (!fn)
    return 0;
  free(fn->live_in);
  fn->live_in = NULL;
  free(fn->live_out);
  fn->live_out = NULL;
  size_t word_count =
      fn->vreg_count ? ((size_t)fn->vreg_count + 63u) / 64u : 0u;
  if (word_count > UINT32_MAX) {
    fn->oom = 1u;
    return 0;
  }
  fn->liveness_word_count = (uint32_t)word_count;
  for (uint32_t vi = 0u; vi < fn->vreg_count; ++vi) {
    fn->vregs[vi].info.use_count = 0u;
    fn->vregs[vi].info.live_start = UINT64_MAX;
    fn->vregs[vi].info.live_end = 0u;
    fn->vregs[vi].info.spill_weight = 0u;
  }
  if (fn->block_count == 0u || fn->liveness_word_count == 0u)
    return 1;
  size_t cells = 0u, bytes = 0u;
  if (ast_size_mul_overflow(fn->block_count, fn->liveness_word_count, &cells) ||
      ast_size_mul_overflow(cells, sizeof(uint64_t), &bytes)) {
    fn->oom = 1u;
    return 0;
  }
  size_t scratch_bytes = 0u;
  if (ast_size_mul_overflow(bytes, 2u, &scratch_bytes)) {
    fn->oom = 1u;
    return 0;
  }
  uint64_t *use_def_storage = (uint64_t *)calloc(1u, scratch_bytes);
  uint64_t *uses = use_def_storage;
  uint64_t *defs = use_def_storage ? use_def_storage + cells : NULL;
  fn->live_in = (uint64_t *)calloc(1u, bytes);
  fn->live_out = (uint64_t *)calloc(1u, bytes);
  AstMirBlockId *order =
      (AstMirBlockId *)mir_malloc_array(fn->block_count, sizeof(*order));
  if (!uses || !defs || !fn->live_in || !fn->live_out || !order) {
    free(use_def_storage);
    free(order);
    fn->oom = 1u;
    return 0;
  }
  for (uint32_t bi = 0u; bi < fn->block_count; ++bi)
    order[fn->blocks[bi].reverse_postorder] = bi;
  uint64_t position = 0u;
  for (uint32_t oi = 0u; oi < fn->block_count; ++oi) {
    AstMirBlockId block_id = order[oi];
    AstMirBlock *block = &fn->blocks[block_id];
    uint64_t *block_use = uses + (size_t)block_id * fn->liveness_word_count;
    uint64_t *block_def = defs + (size_t)block_id * fn->liveness_word_count;
    block->position_begin = position++;
    uint64_t weight = mir_block_weight(block->loop_depth);
    for (uint32_t ai = 0u; ai < block->argument_count; ++ai) {
      AstMirVReg vreg = block->arguments[ai];
      if (vreg < fn->vreg_count) {
        mir_bit_set(block_def, vreg);
        fn->vregs[vreg].info.live_start = block->position_begin;
      }
    }
    for (uint32_t ii = 0u; ii < block->instruction_count; ++ii) {
      AstMirInstId id = block->instructions[ii];
      if (id >= fn->instruction_count)
        continue;
      AstMirInstruction *inst = &fn->instructions[id];
      uint64_t inst_position = position++;
      for (uint32_t oi2 = 0u; oi2 < inst->info.operand_count; ++oi2)
        mir_note_use(fn, inst->operands[oi2], inst_position, weight, block_use,
                     block_def);
      if (inst->info.result < fn->vreg_count) {
        mir_bit_set(block_def, inst->info.result);
        fn->vregs[inst->info.result].info.live_start = inst_position;
      }
    }
    uint64_t term_position = position++;
    if (block->term.info.condition_or_value != AST_MIR_INVALID_ID)
      mir_note_use(fn, block->term.info.condition_or_value, term_position,
                   weight, block_use, block_def);
    for (uint32_t ai = 0u; ai < block->term.info.true_arg_count; ++ai)
      mir_note_use(fn, block->term.true_args[ai], term_position, weight,
                   block_use, block_def);
    for (uint32_t ai = 0u; ai < block->term.info.false_arg_count; ++ai)
      mir_note_use(fn, block->term.false_args[ai], term_position, weight,
                   block_use, block_def);
    block->position_end = term_position;
  }

  int changed = 1;
  while (changed) {
    changed = 0;
    for (uint32_t oi = fn->block_count; oi > 0u; --oi) {
      AstMirBlockId block_id = order[oi - 1u];
      AstMirBlock *block = &fn->blocks[block_id];
      uint64_t *in = fn->live_in + (size_t)block_id * fn->liveness_word_count;
      uint64_t *out = fn->live_out + (size_t)block_id * fn->liveness_word_count;
      uint64_t *use = uses + (size_t)block_id * fn->liveness_word_count;
      uint64_t *def = defs + (size_t)block_id * fn->liveness_word_count;
      for (uint32_t wi = 0u; wi < fn->liveness_word_count; ++wi) {
        uint64_t next_out = 0u;
        for (uint32_t si = 0u; si < block->successor_count; ++si) {
          AstMirBlockId successor = block->successors[si];
          next_out |=
              fn->live_in[(size_t)successor * fn->liveness_word_count + wi];
        }
        uint64_t next_in = use[wi] | (next_out & ~def[wi]);
        if (out[wi] != next_out || in[wi] != next_in) {
          out[wi] = next_out;
          in[wi] = next_in;
          changed = 1;
        }
      }
    }
  }

  for (uint32_t bi = 0u; bi < fn->block_count; ++bi) {
    AstMirBlock *block = &fn->blocks[bi];
    block->live_in_count = 0u;
    block->live_out_count = 0u;
    const uint64_t *in = fn->live_in + (size_t)bi * fn->liveness_word_count;
    const uint64_t *out = fn->live_out + (size_t)bi * fn->liveness_word_count;
    for (uint32_t wi = 0u; wi < fn->liveness_word_count; ++wi) {
      uint64_t in_word = in[wi];
      uint64_t out_word = out[wi];
      block->live_in_count += mir_popcount64(in_word);
      block->live_out_count += mir_popcount64(out_word);
      while (in_word) {
        uint32_t bit = mir_ctz64(in_word);
        size_t index = (size_t)wi * 64u + bit;
        if (index < fn->vreg_count) {
          AstMirVRegInfo *info = &fn->vregs[index].info;
          if (block->position_begin < info->live_start)
            info->live_start = block->position_begin;
        }
        in_word &= in_word - 1u;
      }
      while (out_word) {
        uint32_t bit = mir_ctz64(out_word);
        size_t index = (size_t)wi * 64u + bit;
        if (index < fn->vreg_count) {
          AstMirVRegInfo *info = &fn->vregs[index].info;
          if (block->position_end > info->live_end)
            info->live_end = block->position_end;
        }
        out_word &= out_word - 1u;
      }
    }
  }
  for (uint32_t vi = 0u; vi < fn->vreg_count; ++vi) {
    AstMirVRegInfo *info = &fn->vregs[vi].info;
    if (info->live_start == UINT64_MAX)
      info->live_start = 0u;
    if (info->live_end < info->live_start)
      info->live_end = info->live_start;
  }
  free(use_def_storage);
  free(order);
  return 1;
}
