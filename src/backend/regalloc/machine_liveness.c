#include "regalloc_internal.h"

static uint32_t words_for(uint32_t n) { return n / 64u + (n % 64u != 0u); }
static void set_bit(MachineBitSet *s, uint32_t n) {
  s->words[n / 64u] |= UINT64_C(1) << (n % 64u);
}
static void clear_bit(MachineBitSet *s, uint32_t n) {
  s->words[n / 64u] &= ~(UINT64_C(1) << (n % 64u));
}
int machine_liveness_test_bit(const MachineBitSet *s, uint32_t n) {
  return s && s->words && n / 64u < s->word_count &&
         (s->words[n / 64u] & (UINT64_C(1) << (n % 64u))) != 0u;
}

int machine_cfg_validate(const MachineFunction *fn) {
  uint8_t *seen;
  if (!fn ||
      (fn->info.block_count &&
       (!fn->blocks || fn->info.entry_block >= fn->info.block_count)) ||
      (fn->info.instruction_count && !fn->instructions) ||
      (fn->operand_count && !fn->operands))
    return 0;
  seen = fn->info.instruction_count
             ? (uint8_t *)calloc(fn->info.instruction_count, 1u)
             : NULL;
  if (fn->info.instruction_count && !seen)
    return 0;
  for (uint32_t b = 0; b < fn->info.block_count; ++b) {
    const MachineBlock *mb = &fn->blocks[b];
    if (!mb->info.instruction_count || !mb->instructions ||
        (mb->info.argument_count && !mb->arguments))
      goto fail;
    for (uint32_t a = 0; a < mb->info.argument_count; ++a)
      if (mb->arguments[a] >= fn->info.vreg_count)
        goto fail;
    for (uint32_t j = 0; j < mb->info.instruction_count; ++j) {
      uint32_t id = mb->instructions[j];
      const MachineInstruction *mi;
      if (id >= fn->info.instruction_count || seen[id])
        goto fail;
      seen[id] = 1u;
      mi = &fn->instructions[id];
      int term = machine_opcode_terminator_kind(mi->info.opcode);
      if ((j + 1u == mb->info.instruction_count) != (term != 0) ||
          (term && mi->info.result_count))
        goto fail;
      if (mi->operand_offset > fn->operand_count ||
          mi->info.operand_count > fn->operand_count - mi->operand_offset ||
          (mi->info.result_count &&
           (mi->info.result_first >= fn->info.vreg_count ||
            mi->info.result_count >
                fn->info.vreg_count - mi->info.result_first)))
        goto fail;
      for (uint32_t o = 0; o < mi->info.operand_count; ++o) {
        const AstMachineOperand *op = &fn->operands[mi->operand_offset + o];
        if (op->kind == AST_MACHINE_OPERAND_VREG &&
            op->as.vreg >= fn->info.vreg_count)
          goto fail;
        if (op->kind == AST_MACHINE_OPERAND_BLOCK) {
          if ((term != AST_MIR_TERM_BR && term != AST_MIR_TERM_COND_BR) ||
              op->as.block >= fn->info.block_count)
            goto fail;
          uint32_t n = fn->blocks[op->as.block].info.argument_count;
          if (n > mi->info.operand_count - o - 1u)
            goto fail;
          for (uint32_t a = 0; a < n; ++a) {
            const AstMachineOperand *arg =
                &fn->operands[mi->operand_offset + o + 1u + a];
            if (arg->kind != AST_MACHINE_OPERAND_VREG ||
                arg->as.vreg >= fn->info.vreg_count)
              goto fail;
          }
        }
      }
      if (term == AST_MIR_TERM_BR || term == AST_MIR_TERM_COND_BR) {
        uint32_t cursor = term == AST_MIR_TERM_COND_BR ? 1u : 0u;
        uint32_t edges = term == AST_MIR_TERM_COND_BR ? 2u : 1u;
        if (cursor &&
            (!mi->info.operand_count ||
             fn->operands[mi->operand_offset].kind != AST_MACHINE_OPERAND_VREG))
          goto fail;
        for (uint32_t e = 0; e < edges; ++e) {
          if (cursor >= mi->info.operand_count)
            goto fail;
          const AstMachineOperand *edge =
              &fn->operands[mi->operand_offset + cursor++];
          if (edge->kind != AST_MACHINE_OPERAND_BLOCK)
            goto fail;
          cursor += fn->blocks[edge->as.block].info.argument_count;
          if (cursor > mi->info.operand_count)
            goto fail;
        }
        if (cursor != mi->info.operand_count)
          goto fail;
      } else if (term == AST_MIR_TERM_UNREACHABLE && mi->info.operand_count)
        goto fail;
      else if (term == AST_MIR_TERM_RETURN) {
        for (uint32_t o = 0; o < mi->info.operand_count; ++o)
          if (fn->operands[mi->operand_offset + o].kind !=
              AST_MACHINE_OPERAND_VREG)
            goto fail;
      }
    }
  }
  for (uint32_t i = 0; i < fn->info.instruction_count; ++i)
    if (!seen[i])
      goto fail;
  free(seen);
  return 1;
fail:
  free(seen);
  return 0;
}

void machine_liveness_destroy(MachineLiveness *l) {
  if (!l)
    return;
  ast_arena_destroy(&l->storage);
  memset(l, 0, sizeof(*l));
}

static int segment_add(MachineLiveness *l, MachineLiveInterval *iv,
                       uint32_t begin, uint32_t end) {
  if (begin >= end)
    return 0;
  if (iv->segment_count == iv->segment_capacity) {
    uint32_t cap = iv->segment_capacity ? iv->segment_capacity * 2u : 4u;
    size_t bytes;
    if (cap < iv->segment_capacity ||
        ast_size_mul_overflow(cap, sizeof(*iv->segments), &bytes))
      return 0;
    MachineLiveSegment *next = (MachineLiveSegment *)ast_arena_alloc(
        &l->storage, bytes, ASTER_ALIGNOF(MachineLiveSegment));
    if (!next)
      return 0;
    if (iv->segments && iv->segment_count)
      memcpy(next, iv->segments, (size_t)iv->segment_count * sizeof(*next));
    iv->segments = next;
    iv->segment_capacity = cap;
  }
  iv->segments[iv->segment_count++] = (MachineLiveSegment){begin, end};
  return 1;
}
static int use_add(MachineLiveness *l, MachineLiveInterval *iv, uint32_t slot,
                   uint32_t id, uint32_t operand, uint32_t depth,
                   uint8_t fixed) {
  if (iv->use_count == iv->use_capacity) {
    uint32_t cap = iv->use_capacity ? iv->use_capacity * 2u : 4u;
    size_t bytes;
    if (cap < iv->use_capacity ||
        ast_size_mul_overflow(cap, sizeof(*iv->uses), &bytes))
      return 0;
    MachineUsePosition *next = (MachineUsePosition *)ast_arena_alloc(
        &l->storage, bytes, ASTER_ALIGNOF(MachineUsePosition));
    if (!next)
      return 0;
    if (iv->uses && iv->use_count)
      memcpy(next, iv->uses, (size_t)iv->use_count * sizeof(*next));
    iv->uses = next;
    iv->use_capacity = cap;
  }
  uint32_t weight = 1u << (depth > 10u ? 10u : depth);
  iv->uses[iv->use_count++] =
      (MachineUsePosition){slot, id, operand, weight, fixed};
  iv->weight += weight;
  return 1;
}
static int segment_cmp(const void *a, const void *b) {
  const MachineLiveSegment *x = (const MachineLiveSegment *)a,
                           *y = (const MachineLiveSegment *)b;
  return x->begin < y->begin   ? -1
         : x->begin > y->begin ? 1
         : x->end < y->end     ? -1
                               : x->end > y->end;
}
static int use_cmp(const void *a, const void *b) {
  const MachineUsePosition *x = (const MachineUsePosition *)a,
                           *y = (const MachineUsePosition *)b;
  return x->slot < y->slot         ? -1
         : x->slot > y->slot       ? 1
         : x->operand < y->operand ? -1
                                   : x->operand > y->operand;
}

int machine_liveness_build(const MachineFunction *fn, MachineLiveness *out) {
  MachineLiveness l = {0};
  MachineBitSet live = {0};
  uint32_t *ends = NULL;
  if (!out)
    return 0;
  memset(out, 0, sizeof(*out));
  if (!machine_cfg_validate(fn))
    return 0;
  l.block_count = fn->info.block_count;
  l.vreg_count = fn->info.vreg_count;
  /* Liveness owns many short-lived arrays and per-vreg growable vectors.
     Keep them in one function-local arena so the allocator does not perform
     dozens or hundreds of tiny libc allocations for every function. */
  size_t rough = 4096u;
  if (fn->info.instruction_count <= (SIZE_MAX - rough) / 32u)
    rough += (size_t)fn->info.instruction_count * 32u;
  if (l.vreg_count <= (SIZE_MAX - rough) / 96u)
    rough += (size_t)l.vreg_count * 96u;
  size_t arena_block = 4096u;
  while (arena_block < rough && arena_block < 262144u)
    arena_block *= 2u;
  ast_arena_init(&l.storage, arena_block);
  /* One owner for all equally sized sets: no per-block allocation failures,
     and no null storage even for a function without virtual registers. */
  size_t sets, words, bytes;
  uint32_t word_count = words_for(l.vreg_count);
  if (ast_size_mul_overflow(l.block_count, 4u, &sets) || sets == SIZE_MAX ||
      ast_size_mul_overflow(sets + 1u, word_count, &words) ||
      ast_size_mul_overflow(words ? words : 1u, sizeof(uint64_t), &bytes))
    goto fail;
  l.bitset_storage = (uint64_t *)ast_arena_calloc(&l.storage, 1u, bytes,
                                                  ASTER_ALIGNOF(uint64_t));
  if (!l.bitset_storage)
    goto fail;
  live.words = l.bitset_storage;
  live.word_count = word_count;
  l.blocks = AST_ARENA_ARRAY(&l.storage, MachineLivenessBlock,
                             l.block_count ? l.block_count : 1u);
  l.intervals = AST_ARENA_ARRAY(&l.storage, MachineLiveInterval,
                                l.vreg_count ? l.vreg_count : 1u);
  l.instruction_slots = AST_ARENA_ARRAY(
      &l.storage, uint32_t,
      fn->info.instruction_count ? fn->info.instruction_count : 1u);
  l.block_begin =
      AST_ARENA_ARRAY(&l.storage, uint32_t, l.block_count ? l.block_count : 1u);
  l.block_end =
      AST_ARENA_ARRAY(&l.storage, uint32_t, l.block_count ? l.block_count : 1u);
  ends =
      AST_ARENA_ARRAY(&l.storage, uint32_t, l.vreg_count ? l.vreg_count : 1u);
  if (!l.blocks || !l.intervals || !l.instruction_slots || !l.block_begin ||
      !l.block_end || !ends)
    goto fail;
  for (uint32_t b = 0; b < l.block_count; ++b) {
    MachineLivenessBlock *lb = &l.blocks[b];
    const MachineBlock *mb = &fn->blocks[b];
    uint64_t *storage = l.bitset_storage + ((size_t)b * 4u + 1u) * word_count;
    lb->use = (MachineBitSet){storage, word_count};
    lb->def = (MachineBitSet){storage + word_count, word_count};
    lb->live_in =
        (MachineBitSet){storage + (size_t)word_count * 2u, word_count};
    lb->live_out =
        (MachineBitSet){storage + (size_t)word_count * 3u, word_count};
    uint64_t end =
        (uint64_t)l.slot_count + (uint64_t)mb->info.instruction_count * 4u + 4u;
    if (end > UINT32_MAX)
      goto fail;
    l.block_begin[b] = l.slot_count;
    l.block_end[b] = (uint32_t)end;
    l.slot_count = (uint32_t)end;
    for (uint32_t a = 0; a < mb->info.argument_count; ++a) {
      set_bit(&lb->def, mb->arguments[a]);
      l.intervals[mb->arguments[a]].def_count++;
    }
    for (uint32_t j = 0; j < mb->info.instruction_count; ++j) {
      uint32_t id = mb->instructions[j];
      const MachineInstruction *mi = &fn->instructions[id];
      l.instruction_slots[id] = l.block_begin[b] + 4u * j + 1u;
      if (mi->dead)
        continue;
      for (uint32_t o = 0; o < mi->info.operand_count; ++o) {
        const AstMachineOperand *op = &fn->operands[mi->operand_offset + o];
        if (op->kind != AST_MACHINE_OPERAND_VREG)
          continue;
        if (!machine_liveness_test_bit(&lb->def, op->as.vreg))
          set_bit(&lb->use, op->as.vreg);
        if (!use_add(&l, &l.intervals[op->as.vreg], l.instruction_slots[id], id,
                     o, mb->info.loop_depth,
                     (uint8_t)((op->flags &
                                AST_MACHINE_OPERAND_FIXED_REGISTER) != 0u)))
          goto fail;
      }
      for (uint32_t r = 0; r < mi->info.result_count; ++r) {
        set_bit(&lb->def, mi->info.result_first + r);
        l.intervals[mi->info.result_first + r].def_count++;
      }
    }
  }
  /* Only actual CFG edges are traversed: no B-by-B successor bitmap. */
  for (;;) {
    int changed = 0;
    for (uint32_t rev = l.block_count; rev; --rev) {
      uint32_t b = rev - 1u;
      MachineLivenessBlock *lb = &l.blocks[b];
      const MachineBlock *mb = &fn->blocks[b];
      const MachineInstruction *term =
          mb->info.instruction_count
              ? &fn->instructions[mb->instructions[mb->info.instruction_count -
                                                   1u]]
              : NULL;
      for (uint32_t w = 0; w < live.word_count; ++w) {
        uint64_t value = 0u;
        if (term)
          for (uint32_t o = 0; o < term->info.operand_count; ++o) {
            const AstMachineOperand *op =
                &fn->operands[term->operand_offset + o];
            if (op->kind == AST_MACHINE_OPERAND_BLOCK)
              value |= l.bitset_storage[((size_t)op->as.block * 4u + 3u) *
                                            word_count +
                                        w];
          }
        uint64_t in = lb->use.words[w] | (value & ~lb->def.words[w]);
        if (value != lb->live_out.words[w] || in != lb->live_in.words[w])
          changed = 1;
        lb->live_out.words[w] = value;
        lb->live_in.words[w] = in;
      }
    }
    if (!changed)
      break;
  }
  for (uint32_t b = 0; b < l.block_count; ++b) {
    const MachineBlock *mb = &fn->blocks[b];
    if (live.word_count)
      memcpy(live.words, l.blocks[b].live_out.words,
             (size_t)live.word_count * sizeof(uint64_t));
    for (uint32_t v = 0; v < l.vreg_count; ++v)
      if (machine_liveness_test_bit(&live, v))
        ends[v] = l.block_end[b];
    for (uint32_t rev = mb->info.instruction_count; rev; --rev) {
      uint32_t id = mb->instructions[rev - 1u], slot = l.instruction_slots[id];
      const MachineInstruction *mi = &fn->instructions[id];
      if (mi->dead)
        continue;
      for (uint32_t r = 0; r < mi->info.result_count; ++r) {
        uint32_t v = mi->info.result_first + r;
        uint32_t end =
            machine_liveness_test_bit(&live, v) ? ends[v] : slot + 2u;
        if (!segment_add(&l, &l.intervals[v], slot + 1u, end))
          goto fail;
        clear_bit(&live, v);
      }
      for (uint32_t o = 0; o < mi->info.operand_count; ++o) {
        const AstMachineOperand *op = &fn->operands[mi->operand_offset + o];
        if (op->kind != AST_MACHINE_OPERAND_VREG)
          continue;
        if (!machine_liveness_test_bit(&live, op->as.vreg))
          ends[op->as.vreg] = slot + 1u;
        set_bit(&live, op->as.vreg);
      }
    }
    for (uint32_t a = 0; a < mb->info.argument_count; ++a) {
      uint32_t v = mb->arguments[a];
      if (!segment_add(&l, &l.intervals[v], l.block_begin[b],
                       machine_liveness_test_bit(&live, v)
                           ? ends[v]
                           : l.block_begin[b] + 1u))
        goto fail;
      clear_bit(&live, v);
    }
    for (uint32_t v = 0; v < l.vreg_count; ++v)
      if (machine_liveness_test_bit(&live, v))
        if (!segment_add(&l, &l.intervals[v], l.block_begin[b], ends[v]))
          goto fail;
  }
  for (uint32_t v = 0; v < l.vreg_count; ++v) {
    MachineLiveInterval *iv = &l.intervals[v];
    if (iv->segment_count) {
      qsort(iv->segments, iv->segment_count, sizeof(*iv->segments),
            segment_cmp);
      uint32_t n = 1u;
      for (uint32_t s = 1u; s < iv->segment_count; ++s) {
        if (iv->segments[s].begin <= iv->segments[n - 1u].end) {
          if (iv->segments[s].end > iv->segments[n - 1u].end)
            iv->segments[n - 1u].end = iv->segments[s].end;
        } else
          iv->segments[n++] = iv->segments[s];
      }
      iv->segment_count = n;
      iv->valid = 1u;
      iv->first_slot = iv->segments[0].begin;
      iv->last_slot = iv->segments[n - 1u].end - 1u;
    }
    if (iv->use_count)
      qsort(iv->uses, iv->use_count, sizeof(*iv->uses), use_cmp);
  }
  *out = l;
  return 1;
fail:
  machine_liveness_destroy(&l);
  return 0;
}

int machine_intervals_overlap(const MachineLiveInterval *a,
                              const MachineLiveInterval *b) {
  uint32_t i = 0u, j = 0u;
  if (!a || !b)
    return 0;
  while (i < a->segment_count && j < b->segment_count) {
    if (a->segments[i].begin < b->segments[j].end &&
        b->segments[j].begin < a->segments[i].end)
      return 1;
    if (a->segments[i].end <= b->segments[j].end)
      ++i;
    else
      ++j;
  }
  return 0;
}
int machine_interval_covers(const MachineLiveInterval *iv, uint32_t slot) {
  if (!iv)
    return 0;
  uint32_t lo = 0u, hi = iv->segment_count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2u;
    if (iv->segments[mid].end <= slot)
      lo = mid + 1u;
    else
      hi = mid;
  }
  return lo < iv->segment_count && iv->segments[lo].begin <= slot;
}
int machine_liveness_verify(const MachineFunction *fn,
                            const MachineLiveness *l) {
  MachineLiveness expected = {0};
  int ok = 0;
  if (!fn || !l || l->block_count != fn->info.block_count ||
      l->vreg_count != fn->info.vreg_count || !l->blocks || !l->intervals ||
      !l->block_begin || !l->block_end || !l->instruction_slots ||
      !machine_liveness_build(fn, &expected))
    return 0;
  if (l->slot_count != expected.slot_count)
    goto done;
  for (uint32_t b = 0; b < l->block_count; ++b) {
    const MachineBitSet *a[] = {&l->blocks[b].use, &l->blocks[b].def,
                                &l->blocks[b].live_in, &l->blocks[b].live_out};
    const MachineBitSet *e[] = {
        &expected.blocks[b].use, &expected.blocks[b].def,
        &expected.blocks[b].live_in, &expected.blocks[b].live_out};
    if (l->block_begin[b] != expected.block_begin[b] ||
        l->block_end[b] != expected.block_end[b])
      goto done;
    for (uint32_t s = 0; s < 4u; ++s)
      if (a[s]->word_count != e[s]->word_count ||
          (a[s]->word_count &&
           (!a[s]->words ||
            memcmp(a[s]->words, e[s]->words,
                   (size_t)a[s]->word_count * sizeof(uint64_t)))))
        goto done;
  }
  for (uint32_t i = 0; i < fn->info.instruction_count; ++i)
    if (l->instruction_slots[i] != expected.instruction_slots[i])
      goto done;
  for (uint32_t v = 0; v < l->vreg_count; ++v) {
    const MachineLiveInterval *a = &l->intervals[v],
                              *e = &expected.intervals[v];
    if (a->segment_count != e->segment_count || a->use_count != e->use_count ||
        a->def_count != e->def_count || a->valid != e->valid ||
        a->first_slot != e->first_slot || a->last_slot != e->last_slot ||
        a->weight != e->weight ||
        (a->segment_count &&
         (!a->segments ||
          memcmp(a->segments, e->segments,
                 (size_t)a->segment_count * sizeof(*a->segments)))) ||
        (a->use_count && !a->uses))
      goto done;
    for (uint32_t u = 0; u < a->use_count; ++u)
      if (a->uses[u].slot != e->uses[u].slot ||
          a->uses[u].instruction != e->uses[u].instruction ||
          a->uses[u].operand != e->uses[u].operand ||
          a->uses[u].fixed != e->uses[u].fixed ||
          a->uses[u].weight != e->uses[u].weight)
        goto done;
  }
  ok = 1;
done:
  machine_liveness_destroy(&expected);
  return ok;
}
